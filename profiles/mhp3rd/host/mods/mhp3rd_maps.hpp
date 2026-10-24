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
//   leaves, the camp's supply box, delivery box, bed and cart, and the
//   gathering points, ballistas and cannons.
//
// An area whose GE data is larger than the game's is not served, nor the
// quest's other areas: the game puts the large monsters' GE data right after
// it in video memory. Nor are a quest's areas when one of them, or an area its
// exits lead to or its arrivals come from, is past the map's areas (a byte by
// map in game_sub at 0x0A0457EC), or the map's collision pack lacks a block
// pair and a points table for each of its areas: what is written would land
// on other entries.
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
// Something used with the action button (the game's group 2 records, made
// into objects by game_task's 0x0A1753C8), by the game's numbers for them: a
// gathering point (the "?" prompt), the camp's supply box, delivery box and
// bed, a ballista (it fires the ammo the hunter has, 0x0A1516A4) and
// a cannon (it fires the cannonball carried there, 0x0A1500F0, from 120 above
// and 280 ahead along its facing). The hunter uses it within its cylinder
// (radius across, height centred on y, 0x0A175C2C).
enum class SpotKind : std::uint8_t {
    Gathering = 2u,
    SupplyBox = 5u,
    DeliveryBox = 6u,
    Bed = 7u,
    Ballista = 9u,
    Cannon = 10u
};
// One of the items a gathering point gives: by the name the game shows, as a
// quest's rewards are named, `count` of it with `chance` percent.
struct GatherItem {
    std::string item;
    std::uint8_t count{1}, chance{100};
};
// What a gathering point gives: one of its items each time, `uses` times in
// a quest and up to `extra_uses` more, drawn as the quest starts. The quest
// holds these (its header +0x60): the point's record names its list by its
// word (+28), from 1 (game_task's 0x0A175684).
struct Gathering {
    std::vector<GatherItem> items;
    std::uint8_t uses{}, extra_uses{};
};
struct Spot {
    SpotKind kind{SpotKind::SupplyBox};
    float x{}, y{}, z{}, radius{}, height{};
    std::uint16_t facing{};
    Gathering gathering; // a gathering point's; empty for the other kinds
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
// The most items a gathering point gives (the disc's lists have up to 10) and
// the most uses it has in a quest.
inline constexpr std::size_t kMostGatherItems = 16u;
inline constexpr unsigned kMostGatherUses = 99u;
// The most areas a map has: the executable's 0x088D0228 reads 13 tables.
inline constexpr std::size_t kMostAreas = 13u;
// The ballistas a quest has: the game keeps whether each is manned in three
// bytes of its state (+8677 of *0x09FBE874; 0x088C0D1C, 0x088C0D40 and
// 0x088C0D64 index them by the ballista's word, 0x088C0D78 and 0x088C0DA4
// walk three), quest-wide as on the sand ship, whose two areas' ballistas
// share words 0 and 1. A word past them lands on the dragonator's and the
// gong's waits. Cannons have no such slots: their word only names one of an
// area's to the other hunters (0x0A0C5434 finds it by area and word).
inline constexpr std::uint16_t kBallistaSlots = 3u;
// The words with which game_task makes a point of some areas usable only
// while a flag of the area is set (0x0A1755D4 looks up map, area and word
// for every point, through 0x088CF1B0, in game_sub's six rows at
// 0x0A044A58). Yakumo numbers no point of such an area with one.
struct ConditionalWord {
    std::uint8_t map{}, area{};
    std::uint16_t word{};
};
inline constexpr ConditionalWord kConditionalWords[] = {
    {3u, 4u, 5u}, {16u, 4u, 5u}, {12u, 4u, 2u}, {19u, 4u, 2u}, {12u, 5u, 3u}, {19u, 5u, 3u}};
// `count` words for an area's points of one kind, from `first` up, passing
// over the area's conditional words.
[[nodiscard]] std::vector<std::uint16_t> point_words(
    std::uint8_t map, std::uint8_t area, std::size_t count, std::uint16_t first);

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
// more points than a table holds, a place is not a finite number or a size not
// above 0, an exit leads to its own area, an area other than a camp (area 0)
// has a cart, a gathering point gives no item, more than kMostGatherItems, one
// unnamed or with a count of 0, chances not adding up to 100 or uses outside
// 1 to kMostGatherUses, something else gives items, its ballistas need words
// past kBallistaSlots, the area is past kMostAreas, or the loader cannot be
// taken over.
bool add_area(psprecomp::Runtime &runtime, AreaSpec spec, std::string *why = nullptr);

// The area a mod added under `id`, or null.
[[nodiscard]] const Area *find(std::string_view id);

// What the gathering points of the area `id` give, by their records' words
// (word 1 first; a conditional word passed over has an empty list); empty
// for an area that gives none or none added.
[[nodiscard]] std::vector<Gathering> gathering(std::string_view id);

// Why the areas `ids`, played by one quest in this order, cannot be: their
// ballistas, numbered across them, need words past kBallistaSlots; or
// nothing. Unknown ids are not counted.
[[nodiscard]] std::optional<std::string> quest_points_problem(const std::vector<std::string> &ids);

// Plays `quest` in the area `id` (its own quest, under a mod's id), besides
// the areas it is played in already. False, with the reason in `why`, when
// there is no such area, or it is on another map than the quest's others or
// takes the place of the same game's area as one of them, or the quest's
// ballistas would need words past kBallistaSlots.
bool play_quest_in(std::string_view id, std::uint32_t quest, std::string *why = nullptr);

// How write_points() numbers an area's points: the map and area they are in
// (whose conditional words are passed over) and the first ballista's word,
// the quest's ballistas being numbered across its areas.
struct Numbering {
    std::uint8_t map{kMaps}, area{0xFFu};
    std::uint16_t first_ballista{};
};

// The points table written for `points` (docs/MAPS.md). A record's word
// (+28) is a gathering point's list, from 1 in the order they are given, a
// cannon's number among the area's, from 0, and a ballista's its slot, from
// `numbering.first_ballista`, all past the area's conditional words; 0 for
// the camp's things.
[[nodiscard]] std::vector<std::uint8_t> write_points(const Points &points, const Numbering &numbering = {});

// What is served for `file` now: bytes standing in for the game's, or null.
[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> serve(psprecomp::Runtime &runtime, FileId file);

// Forgets the areas, so tests can add again; reads the game's files through
// `reader` instead of the disc.
void reset_for_tests();
void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader);

} // namespace mhp3rd::mods::maps
