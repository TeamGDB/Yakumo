#pragma once

// Applying a translation file to the running game's text.
//
// The game keeps its text in several blocks, each an archive entry of DATA.BIN
// loaded into a buffer it allocates (docs/DATA_BIN.md, docs/DEBUG_MENU.md). The
// big one (entry 16) is loaded once at start at a fixed address; the rest (a
// quest's text, a menu's) are loaded as the game needs them, into buffers whose
// addresses change. To translate them all, the block's strings are copied into
// a small arena reserved in guest memory and the table's offsets are pointed
// there, but first the block has to be found:
//
//   1. The file I/O tells `note_read` every read of DATA.BIN, with the archive
//      offset and the guest address it was read to (hle_io.cpp). When a read
//      covers the start of a block the file names, its load address is learned.
//   2. Between two frames (`frame`), every learned block is translated, in
//      place: the block's header and tables are read, and each translated
//      string is put in the arena and its offset rewritten.
//
// An entry the file does not name, a string it does not translate, or a block
// the game has not loaded yet is left alone: the fallback is the game's own
// text. Nothing here reads the disc image.

#include "text/language.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <limits>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psprecomp {
class GuestMemory;
}

namespace mhp3rd::text {

// The game's main text block: the one entry 16 loads at start (docs/DEBUG_MENU.md).
inline constexpr std::uint32_t kMainTextBlock = 0x08A40640u;

// Where translated strings are copied: [begin, end) in guest memory.
struct Arena {
    std::uint32_t begin{};
    std::uint32_t end{};
    [[nodiscard]] bool valid() const noexcept { return end > begin; }
};

// Reserves `bytes` of guest memory, or nothing when there is none to give.
using ArenaAllocator = std::function<std::optional<Arena>(std::size_t bytes)>;

// Store each immutable translation once per arena slice. A wildcard can point
// many table entries at the same bytes without multiplying the reservation.
template <typename Memory>
std::optional<std::uint32_t> store_translation(Memory &memory, const std::string &text, const Arena &arena,
    std::size_t &used, std::map<const std::string *, std::uint32_t> &stored) {
    if (!arena.valid() || !memory.contains(arena.begin, arena.end - arena.begin)) return std::nullopt;
    if (const auto found = stored.find(&text); found != stored.end()) return found->second;
    const std::size_t capacity = arena.end - arena.begin;
    if (used > capacity || text.size() >= capacity - used) return std::nullopt;
    const auto at = arena.begin + static_cast<std::uint32_t>(used);
    for (std::size_t i = 0; i < text.size(); ++i)
        memory.store8(at + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    memory.store8(at + static_cast<std::uint32_t>(text.size()), 0u);
    used += text.size() + 1u;
    stored.emplace(&text, at);
    return at;
}

[[nodiscard]] inline bool offset_fits(std::uint32_t base, std::uint32_t offset) noexcept {
    return offset <= std::numeric_limits<std::uint32_t>::max() - base;
}

// What applying one file to one block did.
struct ApplyResult {
    std::uint32_t applied{}; // strings replaced
    std::uint32_t missing{}; // strings the block does not have
    std::uint32_t skipped{}; // strings that did not fit the arena
    std::uint32_t bytes{};   // arena bytes used
    bool block{};            // the address held a text block
};

// Applies `translations` to the text block at `block`. Memory is anything with
// contains, load32, store8 and store32: psprecomp::GuestMemory in the game, a
// plain buffer in a test.
template <typename Memory>
ApplyResult apply(Memory &memory, std::uint32_t block, const Translations &translations, const Arena &arena) {
    ApplyResult result;
    if (!memory.contains(block, 8u) || !arena.valid()) return result;
    std::map<const std::string *, std::uint32_t> stored;
    std::set<std::uint64_t> placed;
    std::size_t used = 0;

    struct Table {
        std::uint32_t address{};
        std::uint32_t count{};
        bool valid{};
    };
    std::map<std::uint16_t, Table> tables;

    const auto table_for = [&](std::uint16_t index) -> const Table & {
        const auto found = tables.find(index);
        if (found != tables.end()) return found->second;
        Table table;
        const std::uint32_t slot = block + static_cast<std::uint32_t>(index) * 4u;
        if (index < 64u && offset_fits(block, static_cast<std::uint32_t>(index) * 4u) && memory.contains(slot, 4u) &&
            offset_fits(block, memory.load32(slot))) {
            const std::uint32_t address = block + memory.load32(slot);
            if (memory.contains(address, 8u)) {
                const std::uint32_t first = memory.load32(address);
                if (first >= 8u && first % 4u == 0u && first / 4u - 1u <= 8192u) {
                    const std::uint32_t count = first / 4u - 1u;
                    if (offset_fits(address, first) && memory.contains(address, first) &&
                        memory.load32(address + count * 4u) == 0xFFFFFFFFu) {
                        table = Table{address, count, true};
                    }
                }
            }
        }
        return tables.emplace(index, table).first->second;
    };

    // Every exact string first, then the wildcard rules, so a rule never
    // overwrites a named string.
    const auto place = [&](std::uint16_t table_index, std::uint32_t index, const std::string &text) {
        const Table &table = table_for(table_index);
        if (!table.valid || index >= table.count || index == 0u) {
            ++result.missing;
            return;
        }
        if (translations.find(table_index, index) != &text || placed.contains(key(table_index, index))) return;
        const auto at = store_translation(memory, text, arena, used, stored);
        if (!at) {
            ++result.skipped;
            return;
        }
        result.bytes = static_cast<std::uint32_t>(used);
        memory.store32(table.address + index * 4u, *at - table.address);
        placed.insert(key(table_index, index));
        ++result.applied;
    };

    for (const auto &[id, text] : translations.entries()) place(table_of(id), index_of(id), text);
    for (const auto &[pattern, text] : translations.patterns()) {
        // A wildcard expands against the table's own size, read from the block.
        for (std::uint16_t table_index = 0; table_index < 64u; ++table_index) {
            const Table &table = table_for(table_index);
            if (!table.valid) continue;

            if (pattern.any != Pattern::Any::Table && pattern.any != Pattern::Any::Index &&
                pattern.any != Pattern::Any::Both && table_index != pattern.table)
                continue;
            for (std::uint32_t index = 1u; index < table.count; ++index)
                if (pattern.matches(table_index, index)) place(table_index, index, text);
        }
    }
    result.block = !tables.empty() &&
        std::any_of(tables.begin(), tables.end(), [](const auto &item) { return item.second.valid; });
    return result;
}

// The running game's translation: the language chosen at start, the blocks the
// file names, and the arena its strings go in. One language per run: changing
// it needs a restart. "Original" is the game's own text and loads nothing.
void set_language(const std::string &code, const std::vector<std::filesystem::path> &directories);
[[nodiscard]] bool active() noexcept;
[[nodiscard]] const std::string &language_code() noexcept;
[[nodiscard]] const std::string &language_name() noexcept;
// Every entry the loaded file translates.
[[nodiscard]] std::vector<std::uint32_t> translated_entries();
// Every language found in the search directories now, for the menu.
[[nodiscard]] std::vector<Language> languages();
[[nodiscard]] const std::vector<std::filesystem::path> &search_directories() noexcept;

// The file I/O calls this after reading DATA.BIN (hle_io.cpp): `offset` into the
// archive and the `bytes` read. When the read begins a block the loaded file
// translates and carries the whole block, the block is translated in place, in
// this buffer, so the game copies it out already translated.
void translate_read(std::uint64_t offset, std::span<std::uint8_t> bytes);

// Runs once per frame (hle_media.cpp): reserves the arena the translated strings
// live in, through `allocate`, and applies the block the game loaded at start.
void frame(psprecomp::GuestMemory &memory, const ArenaAllocator &allocate);

// Applies a dialogue translation to the block at `address`, whose shape is a
// list of (id, offset) pairs, each `offset` a sub-block, each sub-block a list
// of (kind, offset) pairs, each `offset` a string (tools/extract_dialogue.py).
// The keys are `id:index`. Returns how many strings were replaced. Memory is
// anything with contains, load8/load32, store8/store32.
template <typename Memory>
std::uint32_t apply_dialogue(
    Memory &memory, std::uint32_t address, const Translations &translations, const Arena &arena, std::size_t &used) {
    std::uint32_t applied = 0u;
    std::map<const std::string *, std::uint32_t> stored;
    if (!arena.valid()) return applied;
    // The top list is (id, offset) pairs; the ids need not start at 0 (a block
    // of dialogue is numbered from wherever the game left off), so they are read
    // from each pair rather than assumed from the position.
    for (std::uint32_t slot = 0u; slot < 512u; ++slot) {
        const std::uint32_t top = address + slot * 8u;
        if (!offset_fits(address, slot * 8u) || !memory.contains(top, 8u)) break;
        const std::uint32_t id = memory.load32(top);
        if (id == 0xFFFFFFFFu) break;
        const std::uint32_t block_offset = memory.load32(top + 4u);
        if (block_offset >= 0x00400000u || !offset_fits(address, block_offset)) break;
        const std::uint32_t block = address + block_offset;
        if (!memory.contains(block, 8u)) continue;
        for (std::uint32_t index = 0u; index < 4096u; ++index) {
            const std::uint32_t at = block + index * 8u;
            if (!offset_fits(block, index * 8u) || !memory.contains(at, 8u)) break;
            if (memory.load32(at) == 0xFFFFFFFFu) break;
            const std::string *text = translations.find(static_cast<std::uint16_t>(id), index);
            if (text == nullptr) continue;
            const auto into = store_translation(memory, *text, arena, used, stored);
            if (!into) continue;
            memory.store32(at + 4u, *into - block);
            ++applied;
        }
    }
    return applied;
}

// Applies a quest translation to the quest file at `address`. A quest file is
// an array of record offsets at its top, then the records, each holding a table
// of offsets (absolute in the entry) to its strings (tools/extract_text.py
// `quest_block`). The keys are `ref:offset`: the position of the offset word and
// the offset it holds. The word is repointed into the arena, so a translation
// may be any length (unlike a fixed field). `fields`, when given, is the set of
// offset-word positions the record parser found; a key whose position is not in
// it (a `.lang` that names the table's sentinel word) is left alone. Returns how
// many strings were replaced. Memory is anything with contains, load32,
// store8/store32.
template <typename Memory>
std::uint32_t apply_quest(Memory &memory, std::uint32_t address, const Translations &translations, const Arena &arena,
    std::size_t &used, const std::vector<std::uint32_t> &fields = {}) {
    std::uint32_t applied = 0u;
    std::map<const std::string *, std::uint32_t> stored;
    if (!arena.valid()) return applied;
    for (const auto &[id, text] : translations.entries()) {
        const std::uint32_t position = table_of(id);
        if (!fields.empty() && std::find(fields.begin(), fields.end(), position) == fields.end()) continue;
        const std::uint32_t ref = address + position;
        const std::uint32_t string_offset = index_of(id);
        if (!offset_fits(address, position) || !offset_fits(address, string_offset) || !memory.contains(ref, 4u))
            continue;
        // The word holds the file offset (the archive image is loaded as it is)
        // or an absolute pointer, when the game relocated the copy (the quest
        // keeps its own); anything else is a different layout and is left alone.
        const std::uint32_t word = memory.load32(ref);
        const bool relative = word == string_offset;
        const bool absolute = word == address + string_offset;
        if (!relative && !absolute) continue;
        const auto into = store_translation(memory, text, arena, used, stored);
        if (!into) continue;
        memory.store32(ref, relative ? (*into - address) : *into);
        ++applied;
    }
    return applied;
}

// The blocks translated so far this run, for the menu's diagnostics.
struct AppliedBlock {
    std::uint32_t entry{};
    std::uint32_t address{}; // where its strings landed, or 0 for a read-in-place block
    std::uint32_t applied{};
};
[[nodiscard]] std::vector<AppliedBlock> applied_blocks();
// Forgets what was applied, when the game reloads its text.
void forget_blocks();

} // namespace mhp3rd::text
