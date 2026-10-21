#pragma once

// Packs: the container most of the game's files are (a monster's model set,
// an area, a map's collision). A u32 entry count, then each entry's offset and
// size (u32 each); an entry of size 0 is absent. The game aligns entries to 16
// bytes, and so does write_pack().

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mhp3rd::game {

// The entries of a pack, as views of `data`; nothing when `data` is not one
// (no room for its table, an entry outside it, or more than 64 entries).
[[nodiscard]] std::optional<std::vector<std::span<const std::uint8_t>>> read_pack(std::span<const std::uint8_t> data);

// A pack of `entries`, an empty one written as absent.
[[nodiscard]] std::vector<std::uint8_t> write_pack(const std::vector<std::vector<std::uint8_t>> &entries);

// `pack` with some entries replaced (by index); nothing when `pack` is not a
// pack or an index is past its entries.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> replace_entries(std::span<const std::uint8_t> pack,
    const std::vector<std::pair<std::size_t, std::span<const std::uint8_t>>> &replacements);

} // namespace mhp3rd::game
