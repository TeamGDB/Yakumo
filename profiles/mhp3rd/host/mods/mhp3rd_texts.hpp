#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace psprecomp {
class Runtime;
}

// Strings mods add to the game's main text block (entry 16, loaded once at
// start at 0x08A40640: a header of offsets to tables, each table a list of
// offsets from itself to UTF-8 strings ended by 0xFFFFFFFF;
// docs/DEBUG_MENU.md). Tables are numbered by their header word, as the
// developer tools and docs number them (the game's reader, 0x08868E98, is
// passed the number less 2 and reads block + block[2 + n] + table[i]).
//
// A table given strings is copied to guest memory of its own with the new
// offsets after the game's, every offset rebased to the copy, and the header
// pointed at it, between frames once the block is loaded and each time the
// game loads it again. Strings a translation (text/translation.hpp) points
// elsewhere keep pointing there: the copy keeps the addresses, not the
// offsets.
namespace mhp3rd::mods::texts {

// The main block's tables used for names (docs/DEBUG_MENU.md).
inline constexpr std::uint32_t kItemNames = 3u;

// Adds a string at the end of table `table`. `placed` is called with the
// string's index once the block is loaded and the table copied (its game rows
// plus the strings added to it before). Strings are added before the game
// loads the block or at any time after.
void add(std::uint32_t table, std::string text, std::function<void(std::uint32_t index)> placed = {});

// Puts the added strings into the block when it is loaded; called between
// frames.
void frame(psprecomp::Runtime &runtime);

// Forgets the strings, so tests can add again.
void reset_for_tests();

} // namespace mhp3rd::mods::texts
