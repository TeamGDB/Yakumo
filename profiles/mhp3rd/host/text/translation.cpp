#include "text/translation.hpp"

#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mods.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

namespace mhp3rd::text {
namespace {

// The text blocks of the game's archive that a translation file may name. The
// main one (entry 16) is loaded once at a fixed address; the rest are read as
// the game needs them (docs/DATA_BIN.md). kMainEntry is in language.hpp.
const std::uint32_t kTargets[] = {kMainEntry, 2835u, 2836u, 2837u, 2838u, 2839u, 2840u, 2841u};
// The NPC/quest dialogue, in a different shape from the text blocks: a list of
// (id, offset) pairs, each block another list of (kind, offset) pairs, each
// offset a string (tools/extract_dialogue.py). The ids number across the
// entries (4289 is 0..16, 4290 is 17..23, 4291 is 24..27), so a key is unique.
const std::uint32_t kDialogues[] = {4289u, 4290u, 4291u};
// The quest files: an array of record offsets, then records with a table of
// offsets to their strings (village quests; tools/extract_text.py `quest_block`).
const std::uint32_t kQuests[] = {4059u, 4060u, 4061u, 4062u, 4063u, 4064u,
                                 4065u, 4066u, 4070u, 4071u, 4072u, 4073u};

bool is_dialogue(std::uint32_t entry) {
    for (const std::uint32_t dialogue : kDialogues)
        if (dialogue == entry) return true;
    return false;
}

bool is_quest(std::uint32_t entry) {
    for (const std::uint32_t quest : kQuests)
        if (quest == entry) return true;
    return false;
}
// 0x08800000-0x0A800000 is the game's writable RAM (host/kernel/kernel.hpp).
constexpr std::uint32_t kRamBegin = 0x08800000u;
constexpr std::uint32_t kRamEnd = 0x0A800000u;

struct QuestField {
    std::uint32_t ref{};      // position of the offset word in the file
    std::uint32_t offset{};   // the string's offset in the file
    std::string text;         // the game's own string
};

struct Pending {
    bool read{};            // the game has read this entry (whole)
    bool applied{};
    std::string probe;      // the first non-empty string of the block, to find it in RAM
    std::uint32_t probe_into{};  // where the probe string sits inside the entry
    std::uint32_t size{};   // the entry's byte size, to sanity-check a found base
    // A dialogue's ids number across its entries (4289 is 0..16, 4290 is 17..23),
    // so the first id is not always 0; it tells the entry apart in RAM.
    std::uint32_t first_id{};
    // A block not found after many frames has a probe that will not match; stop
    // scanning (scanning the whole of RAM every frame drops the frame rate).
    std::uint32_t missed{};
    // The game may read a large entry in pieces; the pieces are collected here
    // as they come.
    std::vector<std::uint8_t> partial;
    std::size_t partial_have{};  // how many bytes of the entry have been read
    // A quest file is read again every time its screen opens (the quest list,
    // and again in the quest), each read loading the game's own offsets. Its
    // translation is re-applied to the new buffer, always into the same arena
    // slice, so the same strings are not copied twice.
    std::uint32_t arena_offset{};
    std::uint32_t arena_bytes{};
    // The quest's records (title, objective, result, description, monsters,
    // client), parsed from the file, to find and patch the game's own quest
    // structure, which holds the strings inline (not by offset).
    std::vector<std::vector<QuestField>> records;
    std::uint32_t struct_checks{};  // remaining searches for that structure
    std::uint64_t struct_next{};    // frame of the next search
    std::vector<std::uint32_t> copy_bases;  // the file copies to leave alone
};

struct State {
    bool loaded{};
    std::string code{"original"};
    std::string name{"Original"};
    std::map<std::uint32_t, Translations> blocks;  // entry -> its translations
    std::vector<std::filesystem::path> directories;
    std::optional<Arena> arena;
    std::map<std::uint32_t, Pending> pending;  // entry -> what is known about it
    std::map<std::uint32_t, AppliedBlock> applied;
    bool warned_arena{};
    std::size_t arena_used{};
    std::uint64_t frames{};  // for the timed quest-structure searches
};

State &state() {
    static State value;
    return value;
}

bool is_original(const std::string &code) {
    return code.empty() || code == "original" || code == "en" || code == "en-US" || code == "en-GB";
}

bool is_target(std::uint32_t entry) {
    if (is_dialogue(entry) || is_quest(entry)) return true;
    for (const std::uint32_t target : kTargets)
        if (target == entry) return true;
    return false;
}

// One table at `data + offset`: its string count, or 0 when it does not parse.
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

// The first `limit` bytes of a string in `memory` from `address`, up to its NUL.
// Used to compare a prefix: the probe is a block's first string truncated to a
// fixed length, so only that many bytes are read.
std::string peek_string(const psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t limit) {
    std::string text;
    for (std::size_t i = 0; i < limit && memory.contains(address + i, 1u); ++i) {
        const std::uint8_t byte = memory.load8(address + i);
        if (byte == 0u) break;
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

// Looks for the loaded dialogue entry: finds the probe string in RAM and works
// back to the entry start by the offset the string had inside it.
std::uint32_t find_dialogue(const psprecomp::GuestMemory &memory, const std::string &probe, std::uint32_t into,
                            std::uint32_t first_id) {
    if (probe.empty()) return 0u;
    const auto equal = [&](std::uint32_t at) {
        for (std::size_t i = 0; i < probe.size(); ++i) {
            if (!memory.contains(at + i, 1u) || memory.load8(at + i) != static_cast<std::uint8_t>(probe[i]))
                return false;
        }
        return true;
    };
    for (std::uint32_t at = kRamBegin; at + probe.size() < kRamEnd; at += 4u) {
        if (!memory.contains(at, 1u) || memory.load8(at) != static_cast<std::uint8_t>(probe[0])) continue;
        if (!equal(at)) continue;
        const std::uint32_t base = at - into;
        if (base < kRamBegin || base >= kRamEnd) continue;
        // Sanity: the top-level list is (id, offset) pairs, the first id being
        // this entry's (0 for 4289, 17 for 4290, ...) and the first offset
        // inside the entry.
        if (memory.load32(base) != first_id) continue;
        const std::uint32_t first = memory.load32(base + 4u);
        if (first == 0u || first >= 0x00400000u) continue;
        return base;
    }
    return 0u;
}

// Looks for a loaded block in guest memory by its shape and first string: a
// header whose `u32[1]` is 8 and whose table holds `probe` as one of its
// strings. Returns the address, or 0. The probe is what tells the block apart
// from the others in memory.
std::uint32_t find_block(const psprecomp::GuestMemory &memory, const std::string &probe) {
    if (probe.empty()) return 0u;
    for (std::uint32_t at = kRamBegin; at + 16u < kRamEnd; at += 4u) {
        if (!memory.contains(at, 64u)) continue;
        if (memory.load32(at + 4u) != 8u) continue;
        bool found = false;
        for (std::uint32_t word = 2u; word < 64u && !found; ++word) {
            const std::uint32_t offset = memory.load32(at + word * 4u);
            // A word that is not a table offset is skipped, not the end of the
            // header: entry 16 has a 0xFFFFFFFF between its two groups of
            // tables (indices 2..38 and 40..50).
            if (offset == 0u || offset >= 0x00100000u) continue;
            const std::uint32_t table = at + offset;
            if (!memory.contains(table, 8u)) continue;
            const std::uint32_t first = memory.load32(table);
            if (first < 8u || first % 4u != 0u || first / 4u - 1u > 8192u) continue;
            const std::uint32_t count = first / 4u - 1u;
            if (!memory.contains(table, first) || memory.load32(table + count * 4u) != 0xFFFFFFFFu) continue;
            for (std::uint32_t i = 0u; i < count && !found; ++i) {
                const std::uint32_t relative = memory.load32(table + i * 4u);
                if (relative == 0u) continue;
                const std::string text = peek_string(memory, table + relative, probe.size() + 1u);
                if (text.size() >= probe.size() && text.compare(0u, probe.size(), probe) == 0) found = true;
            }
        }
        if (found) return at;
    }
    return 0u;
}

// Every loaded copy of the quest file: the probe string (the first record's
// title) is found in RAM and the entry start worked back from the offset it had
// inside it, with the record array at the top checked. A quest file is in RAM
// more than once (the quest list's buffer, the quest's own), so the patch
// translates every copy it can find.
std::vector<std::uint32_t> find_quest_copies(const psprecomp::GuestMemory &memory, const std::string &probe,
                                             std::uint32_t into, std::uint32_t size) {
    std::vector<std::uint32_t> copies;
    if (probe.empty()) return copies;
    const auto equal = [&](std::uint32_t at) {
        for (std::size_t i = 0; i < probe.size(); ++i) {
            if (!memory.contains(at + i, 1u) || memory.load8(at + i) != static_cast<std::uint8_t>(probe[i]))
                return false;
        }
        return true;
    };
    for (std::uint32_t at = kRamBegin; at + probe.size() < kRamEnd; at += 4u) {
        if (!memory.contains(at, 1u) || memory.load8(at) != static_cast<std::uint8_t>(probe[0])) continue;
        if (!equal(at)) continue;
        if (at < into) continue;
        const std::uint32_t base = at - into;
        if (base < kRamBegin || base >= kRamEnd || !memory.contains(base, 8u)) continue;
        const std::uint32_t first = memory.load32(base);
        const std::uint32_t second = memory.load32(base + 4u);
        // The record array holds file offsets (the archive image as it is) or
        // absolute pointers when the game relocated the copy.
        const bool relative = first != 0u && first < size && second > first && second < size;
        const bool absolute = first > base && first - base < size && second > first && second - base < size;
        if (!relative && !absolute) continue;
        copies.push_back(base);
    }
    return copies;
}

// The game's own quest structure holds the six strings of the quest inline (not
// by offset), so it is a copy the file I/O never sees. It is found by its title
// followed by the other fields at the game's slots (each field's length rounded
// up to four), and every field is overwritten in place with the translation
// when it fits the slot. Returns how many fields were written.
std::uint32_t apply_quest_struct(psprecomp::GuestMemory &memory,
                                 const std::vector<std::vector<QuestField>> &records,
                                 const Translations &translations, const std::vector<std::uint32_t> &copies,
                                 std::uint32_t copy_size) {
    if (records.empty()) return 0u;
    std::uint32_t end = kRamEnd;
    while (end > kRamBegin + 0x100000u && !memory.contains(end - 4u, 4u)) end -= 0x100000u;
    const std::uint32_t span = end - kRamBegin;
    const std::uint8_t *ram = memory.raw_pointer(kRamBegin, span);
    if (ram == nullptr) return 0u;
    std::vector<std::uint32_t> by_first[256];
    for (std::uint32_t r = 0u; r < records.size(); ++r)
        if (!records[r].empty() && !records[r][0].text.empty())
            by_first[static_cast<std::uint8_t>(records[r][0].text[0])].push_back(r);
    // The quest file itself (and the buffer the game read it into) holds the
    // same strings in the same order; only the game's own structure is patched.
    const auto in_copy = [&](std::uint32_t at) {
        for (const std::uint32_t base : copies)
            if (at >= base && at - base < copy_size) return true;
        return false;
    };

    std::uint32_t written = 0u;
    for (std::uint32_t at = kRamBegin; at + 4u < end; ++at) {
        const std::uint32_t here = at - kRamBegin;
        for (const std::uint32_t r : by_first[ram[here]]) {
            if (in_copy(at)) continue;
            const std::vector<QuestField> &fields = records[r];
            const std::string &title = fields[0].text;
            if (title.size() > end - at) continue;
            if (std::memcmp(ram + here, title.data(), title.size()) != 0) continue;
            // The game's structure keeps the strings with the file's own gaps:
            // each field sits at its file offset relative to the first, and its
            // slot is the distance to the next field (the last, to the zeros
            // that follow the strings).
            std::uint32_t pos[8]{};
            std::uint32_t slot[8]{};
            bool ok = true;
            for (std::uint32_t n = 0u; n < fields.size() && n < 8u && ok; ++n) {
                pos[n] = fields[n].offset - fields[0].offset;
                if (n + 1u < fields.size() && n + 1u < 8u) {
                    slot[n] = fields[n + 1u].offset - fields[n].offset;
                } else {
                    std::uint32_t stop = pos[n] + static_cast<std::uint32_t>(fields[n].text.size()) + 1u;
                    while (stop < 4096u && here + stop < span && ram[here + stop] == 0u) ++stop;
                    slot[n] = stop - pos[n];
                }
                if (pos[n] + fields[n].text.size() > end - at) { ok = false; break; }
                if (n != 0u && std::memcmp(ram + here + pos[n], fields[n].text.data(), fields[n].text.size()) != 0)
                    ok = false;
            }
            if (!ok) continue;
            for (std::uint32_t n = 0u; n < fields.size() && n < 8u; ++n) {
                const std::string *text =
                    translations.find(static_cast<std::uint16_t>(fields[n].ref), fields[n].offset);
                if (text == nullptr) continue;
                const std::uint32_t need = std::max(static_cast<std::uint32_t>(fields[n].text.size()) + 1u,
                                                    static_cast<std::uint32_t>(text->size()) + 1u);
                if (need > slot[n]) continue;  // does not fit the game's slot
                const std::uint32_t base = at + pos[n];
                for (std::uint32_t i = 0u; i < need; ++i)
                    memory.store8(base + i, i < text->size() ? static_cast<std::uint8_t>((*text)[i]) : 0u);
                ++written;
            }
            break;
        }
    }
    return written;
}

} // namespace

void set_language(const std::string &code, const std::vector<std::filesystem::path> &directories) {
    State &s = state();
    s.loaded = true;
    s.code = code.empty() ? "original" : code;
    s.name = "Original";
    s.blocks.clear();
    s.directories = directories;
    s.arena.reset();
    s.pending.clear();
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
                if (!is_target(entry)) continue;
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
    std::sort(blocks.begin(), blocks.end(),
              [](const AppliedBlock &a, const AppliedBlock &b) { return a.entry < b.entry; });
    return blocks;
}

void forget_blocks() {
    State &s = state();
    s.pending.clear();
    s.applied.clear();
    s.arena.reset();
    s.arena_used = 0u;
    s.warned_arena = false;
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
    const std::optional<mods::EntryAt> at = mods::entry_at_offset(offset);
    if (!at || at->into != 0u) return;
    const auto found = s.blocks.find(at->entry);
    if (found == s.blocks.end()) return;
    static const bool trace = std::getenv("MHP3RD_TRACE_TEXT") != nullptr;
    if (trace)
        std::cout << "[text] read entry " << at->entry << " offset " << offset << " size " << bytes.size()
                  << " (entry " << at->size << ")\n";
    // The archive is obfuscated per 2 KiB block; a piece is decrypted by its own
    // starting block, so a read that does not carry the whole entry can still be
    // collected. The pieces are gathered until the entry is whole.
    Pending &pending = s.pending[at->entry];
    pending.size = static_cast<std::uint32_t>(at->size);
    // A quest file is read again every time its screen opens; a read that starts
    // at the entry's first byte is a fresh load, so its translation is applied
    // again to the new buffer (which came back with the game's own offsets).
    // A read that starts at the entry's first byte is a fresh load: the block
    // is translated again, because the game reloads a block it already had
    // (leaving a quest reloads the menus), which puts its own offsets back.
    if (at->into == 0u) {
        pending.read = false;
        pending.applied = false;
        pending.missed = 0u;
        pending.probe.clear();
        pending.partial.clear();
        pending.partial_have = 0u;
    } else if (pending.read) {
        return;
    }
    // Collect the entry's stored bytes, then decrypt them in 2 KiB blocks (the
    // keystream is seeded per block, so a piece is not decrypted on its own).
    // The game may never read the whole entry (a dialogue can be read up to the
    // part it needs); whatever has been read is enough for the probe, and the
    // translation applies to the strings inside it.
    std::vector<std::uint8_t> stored;
    if (bytes.size() >= at->size) {
        stored.assign(bytes.begin(), bytes.end());
    } else {
        if (pending.partial.size() != at->size) pending.partial.assign(at->size, 0u);
        const std::size_t from = static_cast<std::size_t>(at->into);
        if (from + bytes.size() > pending.partial.size()) return;
        std::copy(bytes.begin(), bytes.end(), pending.partial.begin() + from);
        if (pending.partial_have < from + bytes.size()) pending.partial_have = from + bytes.size();
        stored.assign(pending.partial.begin(), pending.partial.begin() + pending.partial_have);
    }
    // The obfuscation is one keystream seeded from the block the entry starts
    // at and running through the whole entry, not a fresh one every 2 KiB
    // (mods::p3rd::decrypt does the entry from `block`, offset 0).
    std::vector<std::uint8_t> clear = stored;
    const std::uint32_t entry_start_block = static_cast<std::uint32_t>((offset - at->into) / mods::p3rd::kBlock);
    mods::p3rd::decrypt(clear, entry_start_block, 0u);
    const auto read32 = [&](std::uint32_t i) -> std::uint32_t {
        return static_cast<std::uint32_t>(clear[i]) | (static_cast<std::uint32_t>(clear[i + 1u]) << 8u) |
               (static_cast<std::uint32_t>(clear[i + 2u]) << 16u) |
               (static_cast<std::uint32_t>(clear[i + 3u]) << 24u);
    };
    // The probe is the first non-empty string and where it sits in the entry, so
    // the entry can be found in RAM by that string alone (the dialogue has no
    // header to look for).
    if (is_dialogue(at->entry)) {
        // The first id of the entry (4289 starts at 0, 4290 at 17, ...), which
        // tells the entry apart from the others in RAM.
        pending.first_id = read32(0u);
        // (id, offset) pairs at the top; the first block's first string.
        for (std::uint32_t k = 0u; k < 64u && pending.probe.empty(); ++k) {
            const std::uint32_t id = read32(k * 8u);
            const std::uint32_t block_offset = read32(k * 8u + 4u);
            if (id == 0xFFFFFFFFu || block_offset >= clear.size()) break;
            for (std::uint32_t j = 0u; j < 64u && pending.probe.empty(); ++j) {
                const std::uint32_t sub = block_offset + j * 8u;
                if (sub + 8u > clear.size()) break;
                if (read32(sub) == 0xFFFFFFFFu) break;
                const std::uint32_t string = block_offset + read32(sub + 4u);
                if (string >= clear.size()) break;
                std::string text;
                for (std::uint32_t c = string; c < clear.size() && clear[c] != 0u && text.size() < 24u; ++c)
                    text.push_back(static_cast<char>(clear[c]));
                if (text.size() >= 4u) {
                    pending.probe = text;
                    pending.probe_into = string;
                }
            }
        }
    } else if (is_quest(at->entry)) {
        // A quest file: an array of record offsets at the top, each record
        // holding a string table at `record + 72`. Every field is kept: the
        // upload repoints the file's offsets, but the game's own quest
        // structure holds the strings inline, with no offsets, so its fields
        // are found by these and overwritten in place (frame).
        pending.records.clear();
        std::uint32_t starts[64];
        std::uint32_t count = 0u;
        for (; count < 64u; ++count) {
            if ((count + 1u) * 4u > clear.size()) break;
            const std::uint32_t value = read32(count * 4u);
            if (value == 0u || (count != 0u && value <= starts[count - 1u]) || value >= clear.size()) break;
            starts[count] = value;
        }
        for (std::uint32_t k = 0u; k < count; ++k) {
            const std::uint32_t start = starts[k];
            const std::uint32_t end = k + 1u < count ? starts[k + 1u] : static_cast<std::uint32_t>(clear.size());
            const std::uint32_t anchor = start + 72u;
            std::uint32_t best_pos = 0u, best_len = 0u;
            for (std::uint32_t at = start; at + 4u <= end; at += 4u) {
                std::uint32_t run[8];
                std::uint32_t len = 0u;
                for (std::uint32_t p = at; p + 4u <= end && len < 8u;) {
                    const std::uint32_t value = read32(p);
                    if (value == 0u || value >= clear.size() || clear[value] < 32u || clear[value] >= 127u) break;
                    if (len != 0u && value <= run[len - 1u]) break;
                    run[len++] = value;
                    p += 4u;
                }
                if (len >= 3u && run[0] == anchor && len > best_len) {
                    best_len = len;
                    best_pos = at;
                }
            }
            if (best_len < 3u) continue;
            std::vector<QuestField> fields;
            for (std::uint32_t n = 0u; n < best_len; ++n) {
                const std::uint32_t string = read32(best_pos + n * 4u);
                std::string text;
                for (std::uint32_t c = string; c < clear.size() && clear[c] != 0u && text.size() < 256u; ++c)
                    text.push_back(static_cast<char>(clear[c]));
                fields.push_back(QuestField{best_pos + n * 4u, string, std::move(text)});
            }
            if (!fields.empty()) pending.records.push_back(std::move(fields));
        }
        if (!pending.records.empty() && !pending.records[0].empty()) {
            pending.probe = pending.records[0][0].text;
            pending.probe_into = pending.records[0][0].offset;
        }
        // The game's own quest structure may be made now or when the quest
        // starts; search for it a few times over the next minutes.
        pending.struct_checks = 20u;
        pending.struct_next = 0u;
        std::cout << "[text] quest " << at->entry << ": parsed " << pending.records.size() << " record(s)\n";
    } else if (clear.size() >= 12u && read32(4u) == 8u) {
        for (std::uint32_t word = 2u; word < 64u && pending.probe.empty(); ++word) {
            const std::uint32_t table_offset = read32(word * 4u);
            const std::uint32_t count = table_count(clear, 0u, table_offset);
            // A gap or header field between tables is skipped, not the end of
            // the header (entry 16 has a 0xFFFFFFFF before its second group).
            if (count == 0u) continue;
            for (std::uint32_t i = 1u; i < count; ++i) {
                const std::uint32_t relative = read32(table_offset + i * 4u);
                if (relative == 0u) continue;
                const std::uint32_t string = table_offset + relative;
                if (string >= clear.size()) continue;
                std::string text;
                for (std::uint32_t c = string; c < clear.size() && clear[c] != 0u && text.size() < 24u; ++c)
                    text.push_back(static_cast<char>(clear[c]));
                if (text.size() >= 4u) {
                    pending.probe = text;
                    pending.probe_into = string;
                    break;
                }
            }
        }
    }
    pending.read = true;
    if (trace)
        std::cout << "[text] block " << at->entry << " read: probe \"" << pending.probe << "\"\n";
}

void frame(psprecomp::GuestMemory &memory, const ArenaAllocator &allocate) {
    State &s = state();
    if (!s.loaded || s.blocks.empty()) return;
    ++s.frames;

    // The main block sits at its fixed address as soon as the game has loaded
    // it; the others are found in RAM by their first string once read.
    auto main = s.pending.find(kMainEntry);
    if (s.blocks.count(kMainEntry) != 0u && main == s.pending.end()) {
        s.pending[kMainEntry].read = true;
        s.pending[kMainEntry].applied = s.applied.count(kMainEntry) != 0u;
    }

    bool any = false;
    for (const auto &[entry, pending] : s.pending)
        if (pending.read && (!pending.applied || (is_quest(entry) && pending.struct_checks > 0u))) any = true;
    if (!any) return;

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

    for (auto &[entry, pending] : s.pending) {
        // A quest file is read once, but the game's own quest structure (the
        // strings inline) may be made when the quest starts; look for it a few
        // times over the next seconds and overwrite its fields in place.
        if (is_quest(entry) && pending.applied && pending.struct_checks > 0u && s.frames >= pending.struct_next) {
            const auto quest = s.blocks.find(entry);
            std::uint32_t patched = 0u;
            if (quest != s.blocks.end())
                patched = apply_quest_struct(memory, pending.records, quest->second, pending.copy_bases, pending.size);
            if (patched > 0u) {
                pending.struct_checks = 0u;
                std::cout << "[text] quest " << entry << ": patched " << patched
                          << " field(s) in the quest structure\n";
            } else if (--pending.struct_checks > 0u) {
                pending.struct_next = s.frames + (pending.struct_checks > 14u ? 60u : 300u);
            }
        }
        if (!pending.read || pending.applied) continue;
        const auto translations = s.blocks.find(entry);
        if (translations == s.blocks.end()) continue;

        std::uint32_t address = 0u;
        std::vector<std::uint32_t> quest_copies;
        if (entry == kMainEntry) address = kMainTextBlock;
        else if (is_dialogue(entry)) address = find_dialogue(memory, pending.probe, pending.probe_into, pending.first_id);
        else if (is_quest(entry)) {
            quest_copies = find_quest_copies(memory, pending.probe, pending.probe_into, pending.size);
            address = quest_copies.empty() ? 0u : quest_copies.front();
        } else if (!pending.probe.empty()) address = find_block(memory, pending.probe);
        static const bool trace = std::getenv("MHP3RD_TRACE_TEXT") != nullptr;
        if (address == 0u) {
            // A probe that never matches must not be retried forever: scanning
            // the whole of RAM each frame is what dropped the frame rate.
            if (++pending.missed > 300u) {
                pending.read = false;
                if (trace) std::cout << "[text] block " << entry << ": gave up looking for it\n";
            } else if (trace) {
                std::cout << "[text] block " << entry << ": not found in RAM yet\n";
            }
            continue;
        }

        // The dialogue has its own shape; the text blocks share one. Each block
        // gets its own slice of the arena, reserved once and reused, so a block
        // translated again after the game reloads it does not grow the arena.
        if (is_dialogue(entry)) {
            if (pending.arena_bytes == 0u) {
                const std::size_t need = translations->second.arena_bytes();
                if (need == 0u || s.arena_used + need > s.arena->end - s.arena->begin) continue;
                pending.arena_offset = static_cast<std::uint32_t>(s.arena_used);
                pending.arena_bytes = static_cast<std::uint32_t>(need);
                s.arena_used += need;
            }
            const Arena slice{s.arena->begin + pending.arena_offset,
                              s.arena->begin + pending.arena_offset + pending.arena_bytes};
            std::size_t used = 0u;
            const std::uint32_t applied = apply_dialogue(memory, address, translations->second, slice, used);
            if (applied == 0u) continue;
            pending.applied = true;
            s.applied[entry] = AppliedBlock{entry, address, applied};
            std::cout << "[text] dialogue " << entry << " at " << psprecomp::hex32(address) << ": applied " << applied
                      << " of " << translations->second.size() << ", " << used << " bytes\n";
            continue;
        }

        // A quest file's strings are repointed at the arena (any length), in
        // every copy in RAM and again on every re-read, all pointing at one
        // slice so the strings are not copied twice.
        if (is_quest(entry)) {
            if (pending.arena_bytes == 0u) {
                const std::size_t need = translations->second.arena_bytes();
                if (need == 0u || s.arena_used + need > s.arena->end - s.arena->begin) continue;
                pending.arena_offset = static_cast<std::uint32_t>(s.arena_used);
                pending.arena_bytes = static_cast<std::uint32_t>(need);
                s.arena_used += need;
            }
            const Arena slice{s.arena->begin + pending.arena_offset,
                              s.arena->begin + pending.arena_offset + pending.arena_bytes};
            std::uint32_t applied = 0u;
            pending.copy_bases = quest_copies;
            for (const std::uint32_t copy : quest_copies) {
                std::size_t used = 0u;
                applied += apply_quest(memory, copy, translations->second, slice, used);
            }
            pending.applied = true;
            if (applied > 0u || trace) {
                s.applied[entry] = AppliedBlock{entry, address, applied};
                std::cout << "[text] quest " << entry << " at " << psprecomp::hex32(address) << ": applied " << applied
                          << " in " << quest_copies.size() << " copy/copies, " << pending.arena_bytes << " bytes\n";
            }
            continue;
        }

        // This block's own slice of the arena, reserved once and reused, so a
        // block translated again after the game reloads it does not grow the
        // arena.
        if (pending.arena_bytes == 0u) {
            const std::size_t need = translations->second.arena_bytes();
            if (need == 0u || s.arena_used + need > s.arena->end - s.arena->begin) continue;
            pending.arena_offset = static_cast<std::uint32_t>(s.arena_used);
            pending.arena_bytes = static_cast<std::uint32_t>(need);
            s.arena_used += need;
        }
        const Arena slice{s.arena->begin + pending.arena_offset,
                          s.arena->begin + pending.arena_offset + pending.arena_bytes};
        const ApplyResult result = apply(memory, address, translations->second, slice);
        if (!result.block) continue;
        pending.applied = true;
        s.applied[entry] = AppliedBlock{entry, address, result.applied};
        std::cout << "[text] block " << entry << " at " << psprecomp::hex32(address) << ": applied "
                  << result.applied << ", " << result.missing << " not there, " << result.skipped
                  << " did not fit\n";
    }
}

} // namespace mhp3rd::text
