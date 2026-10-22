#pragma once

#include "mods/mod_library.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Areas mods add (the "yakumo.maps" service): an area of a mod's own takes the
// place of one of the game's (an area of one of its maps) on the mod's quests
// that name it. While such a quest is played, the files the game loads for the
// base area are served with the mod's in them (docs/MAPS.md):
//
// - the area's pack (by map, the u16 table game_sub keeps at 0x0A0457C4, plus
//   2 for each area before it), with the mod's model as entry 0 and its
//   textures as entry 3; the rest (scene, camera paths, settings) the base's;
// - the area's GE data (the table at 0x0A04579C, the same way): the mod's;
// - the map's collision pack (0x125A + the map), with the area's two blocks
//   the mod's: its ground (1 + 2 x area) and its walls (2 + 2 x area).
//
// An area whose GE data is larger than the game's is not served: the game puts
// the large monsters' GE data right after it in video memory.
//
// The quest played is the accepted quest's id beside the character, which the
// game itself reads to choose a quest's collision (0x0A065388). The files go
// through the loader the files mods add are served by (mhp3rd_added_files.hpp).
namespace mhp3rd::mods::maps {

// The game's maps: the tables have a row for each.
inline constexpr std::uint8_t kMaps = 20u;

struct AreaSpec {
    std::string id;                     // the mod's name for it
    std::uint8_t map{}, area{};         // the game's area it takes the place of
    std::vector<std::uint8_t> model;    // PMO "102", static; its GE data the next file
    std::vector<std::uint8_t> geometry; // that GE data
    std::vector<std::uint8_t> textures; // TMH
    std::vector<std::uint8_t> ground;   // collision blocks ("4.00"): the ground,
    std::vector<std::uint8_t> walls;    // and the walls, ceilings and steep slopes
    std::string mod;                    // who adds it, for the log
};

struct Area {
    std::string id;
    std::uint8_t map{}, area{};
    std::string mod;
};

// Adds an area. False, with the reason in `why`, when the id is taken or
// empty, the map is not one of the game's, a file is not in its format, or the
// loader cannot be taken over.
bool add_area(psprecomp::Runtime &runtime, AreaSpec spec, std::string *why = nullptr);

// The area a mod added under `id`, or null.
[[nodiscard]] const Area *find(std::string_view id);

// Plays `quest` in the area `id` (its own quest, under a mod's id). False when
// there is no such area.
bool play_quest_in(std::string_view id, std::uint32_t quest);

// What is served for `file` now: bytes standing in for the game's, or null.
[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> serve(psprecomp::Runtime &runtime, FileId file);

// Forgets the areas, so tests can add again; reads the game's files through
// `reader` instead of the disc.
void reset_for_tests();
void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader);

} // namespace mhp3rd::mods::maps
