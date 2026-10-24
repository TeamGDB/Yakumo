#pragma once

#include "mods/mod_library.hpp"

#include <cstddef>
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
// that name it; a quest may name several, one in place of each of the map's
// areas it plays in. While such a quest is played, the files the game loads
// for the base areas are served with the mod's in them (docs/MAPS.md):
//
// - each area's pack (by map, the u16 table game_sub keeps at 0x0A0457C4,
//   plus 2 for each area before it), with the mod's model as entry 0 and its
//   textures as entry 3; the rest (scene, camera paths, settings) the base's;
// - each area's GE data (the table at 0x0A04579C, the same way): the mod's;
// - the map's collision pack (0x125A + the map), with each area's two blocks
//   the mod's: its ground (1 + 2 x area) and its walls (2 + 2 x area); and,
//   for an area that gives its points, its points table (1 + 2 x areas +
//   area, where the map has `areas`): where the hunter comes in, where he
//   leaves and the camp's supply box, delivery box, bed and cart.
//
// An area whose GE data is larger than the game's is not served, nor the
// quest's other areas: the game puts the large monsters' GE data right after
// it in video memory.
//
// The quest played is the accepted quest's id beside the character, which the
// game itself reads to choose a quest's collision (0x0A065388). The files go
// through the loader the files mods add are served by (mhp3rd_added_files.hpp).
namespace mhp3rd::mods::maps {

// The game's maps: the tables have a row for each.
inline constexpr std::uint8_t kMaps = 20u;

// An area's points table (the executable reads it at 0x088D0118): groups of
// 32-byte records by kind. A way out: walking into its cylinder (radius
// across, height centred on y, 0x088D07CC) takes the hunter to area `to`,
// where he comes in at the arrival `from` this area with the same `door`
// (0x088D033C).
struct Exit {
    float x{}, y{}, z{}, radius{}, height{};
    std::uint8_t to{}, door{};
};
// A way in: where the hunter appears coming `from` an area through `door`, or
// at the quest's start (from kQuestStart), facing a 65536th of a turn (0 faces
// +z, 0x8000 -z).
inline constexpr std::uint8_t kQuestStart = 0xFFu;
struct Arrival {
    float x{}, y{}, z{};
    std::uint16_t facing{};
    std::uint8_t from{}, door{};
};
// Something used with the action button (the game's kind 2 records, made into
// objects by game_task's 0x0A1753C8): the camp's supply box, delivery box and
// bed, by the game's numbers for them. The hunter uses it within its cylinder
// (radius across, height centred on y, 0x0A175C2C).
enum class SpotKind : std::uint8_t { SupplyBox = 5u, DeliveryBox = 6u, Bed = 7u };
struct Spot {
    SpotKind kind{SpotKind::SupplyBox};
    float x{}, y{}, z{}, radius{}, height{};
    std::uint16_t facing{};
};
// A camp's cart, where a hunter who fainted is brought back and set down along
// its facing (the one kind 4 record, 0x088CE568).
struct Cart {
    float x{}, y{}, z{};
    std::uint16_t facing{};
};
struct Points {
    std::vector<Exit> exits;
    std::vector<Arrival> arrivals;
    std::vector<Spot> spots;
    std::optional<Cart> cart;
};
// The most of each a points table holds (the game counts them in a byte, and
// its own areas have at most 5 exits, 6 arrivals and 7 things to use).
inline constexpr std::size_t kMostPoints = 16u;

struct AreaSpec {
    std::string id;                     // the mod's name for it
    std::uint8_t map{}, area{};         // the game's area it takes the place of
    std::vector<std::uint8_t> model;    // PMO "102", static; its GE data the next file
    std::vector<std::uint8_t> geometry; // that GE data
    std::vector<std::uint8_t> textures; // TMH
    std::vector<std::uint8_t> ground;   // collision blocks ("4.00"): the ground,
    std::vector<std::uint8_t> walls;    // and the walls, ceilings and steep slopes
    std::string mod;                    // who adds it, for the log
    std::optional<Points> points;       // none: the game's area's stay
};

struct Area {
    std::string id;
    std::uint8_t map{}, area{};
    std::string mod;
};

// Adds an area. False, with the reason in `why`, when the id is taken or
// empty, the map is not one of the game's, a file is not in its format, it has
// more points than a table holds, or the loader cannot be taken over.
bool add_area(psprecomp::Runtime &runtime, AreaSpec spec, std::string *why = nullptr);

// The area a mod added under `id`, or null.
[[nodiscard]] const Area *find(std::string_view id);

// Plays `quest` in the area `id` (its own quest, under a mod's id), besides
// the areas it is played in already. False, with the reason in `why`, when
// there is no such area, or it is on another map than the quest's others or
// takes the place of the same game's area as one of them.
bool play_quest_in(std::string_view id, std::uint32_t quest, std::string *why = nullptr);

// The points table written for `points` (docs/MAPS.md).
[[nodiscard]] std::vector<std::uint8_t> write_points(const Points &points);

// What is served for `file` now: bytes standing in for the game's, or null.
[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> serve(psprecomp::Runtime &runtime, FileId file);

// Forgets the areas, so tests can add again; reads the game's files through
// `reader` instead of the disc.
void reset_for_tests();
void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader);

} // namespace mhp3rd::mods::maps
