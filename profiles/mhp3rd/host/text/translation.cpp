#include "text/translation.hpp"

#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mods.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

namespace mhp3rd::text {
namespace {

// The text blocks of the game's archive that a translation file may name. The
// main one (entry 16) is loaded once at a fixed address; the rest are read as
// the game needs them (docs/DATA_BIN.md). kMainEntry is in language.hpp.
const std::uint32_t kTargets[] = {kMainEntry, 2835u, 2836u, 2837u, 2838u, 2839u, 2840u, 2841u};

struct State {
    bool loaded{};
    std::string code{"original"};
    std::string name{"Original"};
    std::map<std::uint32_t, Translations> blocks;  // entry -> its translations
    std::vector<std::filesystem::path> directories;
    std::optional<Arena> arena;
    // The strings of a translation are placed in the arena; a block read into a
    // bounce buffer is shifted by its own base, so each applied block records
    // where its strings went.
    std::map<std::uint32_t, AppliedBlock> applied;
    bool warned_arena{};
    std::size_t arena_used{};
};

State &state() {
    static State value;
    return value;
}

bool is_original(const std::string &code) {
    return code.empty() || code == "original" || code == "en" || code == "en-US" || code == "en-GB";
}

bool is_target(std::uint32_t entry) {
    for (const std::uint32_t target : kTargets)
        if (target == entry) return true;
    return false;
}

// One table at `data + offset`: its string count, or 0 when it does not parse.
// A table is offsets (from the table start) to NUL-terminated strings, ended by
// 0xFFFFFFFF (docs/DEBUG_MENU.md).
std::uint32_t table_count(std::span<const std::uint8_t> data, std::uint32_t base, std::uint32_t offset) {
    const auto read32 = [&](std::uint32_t at) -> std::uint32_t {
        if (at + 4u > data.size()) return 0xFFFFFFFFu;
        return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1u]) << 8u) |
               (static_cast<std::uint32_t>(data[at + 2u]) << 16u) |
               (static_cast<std::uint32_t>(data[at + 3u]) << 24u);
    };
    if (offset < 8u || base + offset + 8u > data.size()) return 0u;
    const std::uint32_t table = base + offset;
    const std::uint32_t first = read32(table);
    if (first < 8u || first % 4u != 0u || first / 4u - 1u > 8192u) return 0u;
    const std::uint32_t count = first / 4u - 1u;
    if (table + first > data.size()) return 0u;
    if (read32(table + count * 4u) != 0xFFFFFFFFu) return 0u;
    return count;
}

bool is_target_entry(std::uint32_t entry) { return is_target(entry); }

} // namespace

void set_language(const std::string &code, const std::vector<std::filesystem::path> &directories) {
    State &s = state();
    s.loaded = true;
    s.code = code.empty() ? "original" : code;
    s.name = "Original";
    s.blocks.clear();
    s.directories = directories;
    s.arena.reset();
    s.applied.clear();
    s.arena_used = 0u;
    s.warned_arena = false;
    if (is_original(s.code)) return;

    for (const std::filesystem::path &directory : directories) {
        for (const Language &language : scan_languages(directory)) {
            if (language.code != s.code) continue;
            std::string error;
            const auto blocks = Translations::from_file(language.file, error);
            if (!blocks) {
                std::cout << "[text] " << error << "\n";
                return;
            }
            std::size_t strings = 0u;
            for (const auto &[entry, translations] : *blocks) {
                if (!is_target_entry(entry)) continue;
                strings += translations.size();
                s.blocks.emplace(entry, translations);
                if (!translations.name().empty()) s.name = translations.name();
            }
            if (!blocks->empty()) s.name = blocks->begin()->second.name();
            std::cout << "[text] " << s.name << " (" << s.code << "): " << strings << " strings in "
                      << s.blocks.size() << " block(s)\n";
            return;
        }
    }
    std::cout << "[text] no " << s.code << " translation found; the game's own text is used\n";
}

bool active() noexcept { return !state().blocks.empty(); }
const std::string &language_code() noexcept { return state().code; }
const std::string &language_name() noexcept { return state().name; }

std::vector<std::uint32_t> translated_entries() {
    std::vector<std::uint32_t> entries;
    for (const auto &[entry, translations] : state().blocks) entries.push_back(entry);
    return entries;
}

std::vector<AppliedBlock> applied_blocks() {
    std::vector<AppliedBlock> blocks;
    for (const auto &[entry, block] : state().applied) blocks.push_back(block);
    std::sort(blocks.begin(), blocks.end(), [](const AppliedBlock &a, const AppliedBlock &b) {
        return a.entry < b.entry;
    });
    return blocks;
}

void forget_blocks() {
    state().applied.clear();
    state().arena.reset();
    state().arena_used = 0u;
    state().warned_arena = false;
}

std::vector<Language> languages() {
    std::vector<Language> all;
    for (const std::filesystem::path &directory : state().directories) {
        for (Language &language : scan_languages(directory)) {
            const bool known = std::any_of(all.begin(), all.end(), [&](const Language &other) {
                return other.code == language.code;
            });
            if (!known) all.push_back(std::move(language));
        }
    }
    std::sort(all.begin(), all.end(), [](const Language &a, const Language &b) { return a.code < b.code; });
    return all;
}

const std::vector<std::filesystem::path> &search_directories() noexcept { return state().directories; }

void translate_read(std::uint64_t offset, std::span<std::uint8_t> bytes) {
    State &s = state();
    if (!s.loaded || s.blocks.empty() || bytes.empty()) return;
    static const bool trace = std::getenv("MHP3RD_TRACE_TEXT") != nullptr;
    const std::optional<mods::EntryAt> at = mods::entry_at_offset(offset);
    if (!at || at->into != 0u) return;
    const auto found = s.blocks.find(at->entry);
    if (found == s.blocks.end()) return;
    // Only a read that carries the whole entry can be translated: the archive
    // is obfuscated per 2 KiB block, and a partial read would be decrypted and
    // encrypted around the wrong bytes. The whole-entry read is the common one
    // (the game reads a text file in one go).
    if (bytes.size() < at->size) {
        if (trace)
            std::cout << "[text] entry " << at->entry << ": read of " << bytes.size() << " of " << at->size
                      << " bytes, left as it is\n";
        return;
    }
    const Translations &translations = found->second;

    // The file I/O serves the archive as stored, still obfuscated: the game
    // decrypts each entry itself, on the way to guest memory. So the bytes are
    // decrypted here, translated, and encrypted again for the same block, so
    // what the game decrypts is the translation (docs/DATA_BIN.md).
    const std::uint32_t block = static_cast<std::uint32_t>(offset / mods::p3rd::kBlock);
    mods::p3rd::decrypt(bytes, block, 0u);
    // Whatever happens below, the bytes go back to the game encrypted for the
    // same block, so the game's own decrypt gets the translated text.
    struct Reencrypt {
        std::span<std::uint8_t> bytes;
        std::uint32_t block;
        ~Reencrypt() { mods::p3rd::encrypt(bytes, block, 0u); }
    } reencrypt{bytes, block};

    // The strings are placed after the whole block, inside the same buffer, so
    // the offsets the game reads are valid wherever it copies the block. The
    // header is `u32[2..]`, table offsets; table k sits at `u32[2+k]`.
    const auto read32 = [&](std::uint32_t i) -> std::uint32_t {
        return static_cast<std::uint32_t>(bytes[i]) | (static_cast<std::uint32_t>(bytes[i + 1u]) << 8u) |
               (static_cast<std::uint32_t>(bytes[i + 2u]) << 16u) |
               (static_cast<std::uint32_t>(bytes[i + 3u]) << 24u);
    };
    const auto store32 = [&](std::uint32_t i, std::uint32_t value) {
        bytes[i] = static_cast<std::uint8_t>(value);
        bytes[i + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[i + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[i + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    if (bytes.size() < 12u || read32(4u) != 8u) return;

    // The game numbers a block's tables by the word in the header that holds
    // their offset: `u32[2]` is table 2, `u32[3]` table 3, and so on
    // (docs/DEBUG_MENU.md; the debug menu's `table 2` reads `u32[2]`). A block
    // with one table (a quest's) has it at word 2.
    std::map<std::uint16_t, std::pair<std::uint32_t, std::uint32_t>> tables;  // index -> (address, count)
    for (std::uint16_t word = 2u; word < 64u; ++word) {
        const std::uint32_t table_offset = read32(word * 4u);
        const std::uint32_t count = table_count(bytes, 0u, table_offset);
        if (count == 0u) break;
        tables.emplace(word, std::make_pair(table_offset, count));
    }
    if (tables.empty()) return;

    // Where the block's own data ends: the farthest string, so the translated
    // strings go in the free space between it and the end of the entry. A block
    // whose own strings fill the entry has no room and is left as it is.
    std::size_t own_end = 0u;
    for (const auto &[word, table] : tables) {
        for (std::uint32_t i = 0u; i < table.second; ++i) {
            const std::uint32_t relative = read32(table.first + i * 4u);
            const std::uint32_t string = table.first + relative;
            if (string >= bytes.size()) continue;
            std::size_t end = string;
            while (end < bytes.size() && bytes[end] != 0u) ++end;
            own_end = std::max(own_end, end + 1u);
        }
    }
    std::size_t cursor = std::max<std::size_t>(own_end, 16u);
    const auto place = [&](std::uint16_t table_index, std::uint32_t index, const std::string &text) {
        const auto table = tables.find(table_index);
        if (table == tables.end() || index >= table->second.second || index == 0u) return;
        const std::size_t needed = text.size() + 1u;
        if (cursor + needed > bytes.size()) return;  // no room in this entry
        const std::uint32_t at_string = static_cast<std::uint32_t>(cursor);
        for (std::size_t i = 0; i < text.size(); ++i) bytes[at_string + i] = static_cast<std::uint8_t>(text[i]);
        bytes[at_string + text.size()] = 0u;
        cursor += needed;
        store32(table->second.first + index * 4u, at_string - table->second.first);
        ++s.applied[at->entry].applied;
    };

    s.applied[at->entry].entry = at->entry;
    const std::size_t before = s.applied[at->entry].applied;
    for (const auto &[id, text] : translations.entries()) place(table_of(id), index_of(id), text);
    for (const auto &[pattern, text] : translations.patterns()) {
        for (const auto &[table_index, table] : tables) {
            if (pattern.any == Pattern::Any::Table && table_index != pattern.table) continue;
            if (pattern.any != Pattern::Any::Table && pattern.any != Pattern::Any::Index &&
                table_index != pattern.table)
                continue;
            for (std::uint32_t index = 1u; index < table.second; ++index)
                if (pattern.matches(table_index, index)) place(table_index, index, text);
        }
    }
    const std::size_t done = s.applied[at->entry].applied - before;
    if (trace)
        std::cout << "[text] block " << at->entry << " read (" << bytes.size() << " bytes): " << done << " of "
                  << translations.size() << " applied\n";
}

void frame(psprecomp::GuestMemory &memory, const ArenaAllocator &allocate) {
    State &s = state();
    if (!s.loaded || s.blocks.empty()) return;
    // The main block (entry 16) is loaded at a fixed address, and its own loader
    // copies it there without going through the read path this file sees; so it
    // is applied here, in place, from the arena.
    const auto main = s.blocks.find(kMainEntry);
    if (main == s.blocks.end() || s.applied.count(kMainEntry) != 0u) return;
    if (!memory.contains(kMainTextBlock, 12u)) return;
    if (!s.arena) {
        std::size_t bytes = 64u;
        for (const auto &[entry, translations] : s.blocks) bytes += translations.arena_bytes();
        const std::optional<Arena> arena = allocate(bytes);
        if (!arena || !arena->valid()) {
            if (!s.warned_arena) {
                std::cout << "[text] no guest memory for the translations; the game's text is used\n";
                s.warned_arena = true;
            }
            return;
        }
        s.arena = arena;
    }
    const ApplyResult result = apply(memory, kMainTextBlock, main->second, *s.arena);
    if (!result.block) return;
    s.applied[kMainEntry] = AppliedBlock{kMainEntry, kMainTextBlock, result.applied};
    std::cout << "[text] block " << kMainEntry << " at " << psprecomp::hex32(kMainTextBlock) << ": applied "
              << result.applied << ", " << result.missing << " not there, " << result.skipped << " did not fit\n";
}

} // namespace mhp3rd::text
