#pragma once

// Applying a translation file to the running game's text.
//
// The game loads one block of its text at start (0x08A40640 on NPJB-40001, the
// executable Yakumo supports; docs/DEBUG_MENU.md). The block is a header of
// offsets to tables, each table a list of offsets to UTF-8 strings ended by
// 0xFFFFFFFF. To translate an entry, its string is copied into a small arena
// reserved in guest memory and the table's offset for it is pointed there;
// every entry the file does not name keeps its own bytes. That is why the
// fallback is free: an untranslated menu option is simply the game's own text,
// English on a patched disc.
//
// Nothing here reads the disc image: the translation files live beside Yakumo
// (docs/TEXT_TRANSLATION.md), so the game's files are never touched.

#include "text/language.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace psprecomp {
class GuestMemory;
}

namespace mhp3rd::text {

// The game's text block: one per executable Yakumo supports.
inline constexpr std::uint32_t kTextBlock = 0x08A40640u;

// Where translated strings are copied: [begin, end) in guest memory.
struct Arena {
    std::uint32_t begin{};
    std::uint32_t end{};
    [[nodiscard]] bool valid() const noexcept { return end > begin; }
};

// Reserves `bytes` of guest memory, or nothing when there is none to give.
using ArenaAllocator = std::function<std::optional<Arena>(std::size_t bytes)>;

// What applying one file to a text block did.
struct ApplyResult {
    std::uint32_t applied{};  // entries replaced
    std::uint32_t missing{};  // entries the text block does not have
    std::uint32_t skipped{};  // entries that did not fit the arena
    std::uint32_t bytes{};    // arena bytes used
    bool block{};             // the block looked like the game's text
};

// Applies `translations` to the text block at `block` (see the file comment).
// Memory is anything with contains, load32, store8 and store32:
// psprecomp::GuestMemory in the game, a plain buffer in a test.
template <typename Memory>
ApplyResult apply(Memory &memory, std::uint32_t block, const Translations &translations, const Arena &arena) {
    ApplyResult result;
    if (translations.empty() || !memory.contains(block, 8u) || !arena.valid()) return result;

    // Table -> (address, entry count), read once per table.
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
        if (memory.contains(slot, 4u)) {
            const std::uint32_t address = block + memory.load32(slot);
            if (memory.contains(address, 8u)) {
                const std::uint32_t first = memory.load32(address);
                if (first >= 8u && first % 4u == 0u && first / 4u - 1u <= 4096u) {
                    const std::uint32_t count = first / 4u - 1u;
                    if (memory.contains(address, first) &&
                        memory.load32(address + count * 4u) == 0xFFFFFFFFu) {
                        table = Table{address, count, true};
                    }
                }
            }
        }
        return tables.emplace(index, table).first->second;
    };

    std::uint32_t cursor = arena.begin;
    for (const auto &[id, text] : translations.entries()) {
        const Table &table = table_for(table_of(id));
        const std::uint32_t entry = entry_of(id);
        if (!table.valid || entry >= table.count || entry == 0u) {
            ++result.missing;
            continue;
        }
        const std::size_t needed = text.size() + 1u;
        if (cursor + needed > arena.end) {
            ++result.skipped;
            continue;
        }
        const std::uint32_t at = cursor;
        for (std::size_t i = 0; i < text.size(); ++i)
            memory.store8(at + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
        memory.store8(at + static_cast<std::uint32_t>(text.size()), 0u);
        cursor += static_cast<std::uint32_t>(needed);
        result.bytes += static_cast<std::uint32_t>(needed);

        // Offsets are relative to the table; the destination is above it.
        const std::uint32_t offset = at - table.address;
        memory.store32(table.address + entry * 4u, offset);
        ++result.applied;
    }
    result.block = !tables.empty() && std::any_of(tables.begin(), tables.end(),
                                                  [](const auto &item) { return item.second.valid; });
    return result;
}

// The running game's translation: the language chosen at start and the arena
// its strings go in. One language per run, like a mod's set: changing it needs
// a restart. English is the game's own text and loads nothing.
void set_language(const std::string &code, const std::vector<std::filesystem::path> &directories);
[[nodiscard]] bool active() noexcept;
[[nodiscard]] const std::string &language_code() noexcept;
[[nodiscard]] const std::string &language_name() noexcept;
// Every language found in the search directories now, for the menu.
[[nodiscard]] std::vector<Language> languages();
// The directories the language was loaded from.
[[nodiscard]] const std::vector<std::filesystem::path> &search_directories() noexcept;

// Applies the translations to the game's text block once it is loaded, between
// two game frames (hle_media.cpp). Reserves the arena through `allocate` on
// first use. Does nothing while English is chosen or before the block loads.
void frame(psprecomp::GuestMemory &memory, const ArenaAllocator &allocate);

} // namespace mhp3rd::text
