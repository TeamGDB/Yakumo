#pragma once

#include "mods/mod_library.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
}

// The pictures of large monsters the game shows in a quest's description
// (docs/MONSTERS.md, "A kind's picture"): 42 by 42 pixels,
// each a cell of one texture with a palette of its own. The executable keeps,
// by kind, the cell and the palette in a table of 69 rows (the game table
// `monster.pictures`); a kind past its end reads what follows it and shows no
// picture.
//
// A kind a mod adds gets a row: the base's picture, or a picture of the mod's
// own, which takes a cell and a palette none of the game's pictures uses and is
// written into the texture's file when the game loads it (through the loader
// files mods add go through, mhp3rd_added_files.hpp).
namespace mhp3rd::mods::pictures {

inline constexpr int kSide = 42;                   // pixels across and down
inline constexpr std::size_t kColours = 16;        // a palette's entries
inline constexpr FileId kTextureFile = 0x0FEAu;    // the bank of menu textures
inline constexpr std::uint32_t kTextureImage = 3u; // its image of pictures
inline constexpr std::uint32_t kGameRows = 69u;    // kinds the game's table has

// RGBA, row by row, kSide * kSide * 4 bytes.
using Pixels = std::vector<std::uint8_t>;

// A PNG of kSide by kSide pixels as Pixels; nothing for another size or a
// file that is not a PNG.
[[nodiscard]] std::optional<Pixels> from_png(std::span<const std::uint8_t> png);

// Gives `kind` a row: `like`'s picture, or `own` when given. False, with the
// reason in `why`, when the table cannot grow, `own` is not 42 by 42 or has
// more than 16 colours, or no cell and palette are left.
bool give(
    psprecomp::Runtime &runtime, std::uint8_t kind, std::uint8_t like, const Pixels *own, std::string *why = nullptr);

// The texture bank with the mods' pictures in it, for `file` when it is the
// bank and mods gave pictures; null otherwise.
[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> serve(psprecomp::Runtime &runtime, FileId file);

// Writes `pixels` into the bank's picture image at the cell (column, row) with
// palette `palette`. Nothing when the bank is not as traced (an image 3 of 352
// by 352 at 4 bits with 64 palettes of RGBA8888). Public for tests.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> compose(
    const std::vector<std::uint8_t> &bank, int column, int row, int palette, const Pixels &pixels);

void reset_for_tests();
void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader);

} // namespace mhp3rd::mods::pictures
