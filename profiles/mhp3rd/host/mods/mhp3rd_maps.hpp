#pragma once

#include "mods/mod_library.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psprecomp {
class Runtime;
}
struct ymod_cpu;

// Areas mods add (the "yakumo.maps" service): an area of a mod's own takes the
// place of one of the game's (an area of one of its maps) on the mod's quests
// that name it; a quest may name several, one in place of each of the map's
// areas it plays in. While such a quest is played, the files the game loads
// for the base areas are served with the mod's in them (docs/MAPS.md):
//
// - each area's pack (by map, the u16 table game_sub keeps at 0x0A0457C4,
//   plus 2 for each area before it), with the mod's model as entry 0 and its
//   textures as entry 3; the rest (scene, camera paths, settings) the base's,
//   but for an area with a dragonator's spear, whose first gimmick group of
//   the scene is made the spear (place_spear());
// - each area's GE data (the table at 0x0A04579C, the same way): the mod's;
// - the map's collision pack (0x125A + the map), with each area's two blocks
//   the mod's: its ground (1 + 2 x area) and its walls (2 + 2 x area); and,
//   for an area that gives its points, its points table (1 + 2 x areas +
//   area, where the map has `areas`): where the hunter comes in, where he
//   leaves, the camp's supply box, delivery box, bed and cart, and the
//   gathering points, ballistas and cannons; and, when the quest's areas give
//   the map's picture, entry 0, the picture the minimap draws.
//
// With the picture, while the quest is played and game_sub and game_task are
// loaded, the tables that place the map's areas on it hold the mod's numbers:
// the map's frame (game_sub's 0x0A044E0C + 20 x map: width, depth, x0, z0 and
// a pointer to the areas' records, 24 bytes each, whose first two floats, dx
// and dz, move an area's points into the frame), and the camp's tent icon
// (game_task's 0x0A19D1E8 + 4 x map, s16 u and v). The game's are put back on
// every other quest and when the quest's overlays go (overlay_loaded()).
//
// An area whose GE data is larger than the game's is not served, nor the
// quest's other areas: the game puts the large monsters' GE data right after
// it in video memory. Nor are a quest's areas when one of them, or an area its
// exits lead to or its arrivals come from, is past the map's areas (a byte by
// map in game_sub at 0x0A0457EC), or the map's collision pack lacks a block
// pair and a points table for each of its areas: what is written would land
// on other entries.
//
// An area with a spear also has, while its quest is played, the game's spear
// shot placed at it (game_task's 0x0A151320 knows the sand ship's only) and
// the camera's view of it given by Yakumo's own camera script (event 13, a
// slot the executable's table at 0x0897169C leaves empty), or none; both
// through hooks that leave every other quest to the game's code, and the
// table's slot is put back on every other quest.
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
    Cannon = 10u,
    Dragonator = 11u // its button: the executable's 0x088C2764 starts the wait, the spear and its shot
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

// The map's picture and an area's place on it (yakumo/mod.h, ymod_area_map;
// docs/MAPS.md). A point (x, z) of the area lands on pixel (origin_u + x /
// units_x, origin_v + z / units_z): the map's frame is 160 units_x by 160
// units_z, from the game's x0 and z0, and the area's dx and dz are origin_u x
// units_x + x0 and origin_v x units_z + z0 (0x088D0C88 adds them, 0x088CE854
// gives the frame, game_task's 0x0A16FD3C scales into the picture).
struct MapPicture {
    std::vector<std::uint8_t> picture; // a TMH laid out as the game's (below; picture_problem())
    float units_x{}, units_z{};        // world units a pixel spans, across and down
    float origin_u{}, origin_v{};      // the pixel the area's x 0, z 0 lands on
    std::int32_t camp_u{}, camp_v{};   // the camp's tent icon, 0 to 159; read on area 0 only
};
// The game's pictures (entry 0 of every map's collision pack): a TMH of one
// image, two on maps 4 and 17, of 160 x 168 pixels, 4-bit indices into 16
// RGBA8888 colours; the map takes the top 160 x 160 (game_task's layout table,
// 0x0A197224 + 1160).
inline constexpr std::uint32_t kPictureSide = 160u;
inline constexpr std::size_t kPictureHeaderSize = 16u;   // the bank's signature and image count
inline constexpr std::size_t kPictureImageSize = 13552u; // one image with its palette and headers
// Why `picture` is not laid out as the game's pictures are, or nothing.
[[nodiscard]] std::optional<std::string> picture_problem(const std::vector<std::uint8_t> &picture);
// Why a map picture cannot be an area's (`area` its number), or nothing: a
// picture not laid out as the game's, units not finite and above 0, an origin
// not finite or a frame too large to hold, a camp's tent off the picture.
[[nodiscard]] std::optional<std::string> map_picture_problem(const MapPicture &map, std::uint8_t area);

// The dragonator's spear an area gives (yakumo/mod.h, ymod_area_spear;
// docs/MAPS.md, "The dragonator"): a mesh of its model, at rest with its
// origin at (x, y, z), turned `facing` about y and `pitch` up (65536ths of a
// turn), pointing along its own -z; and the camera's view while it fires.
struct SpearView {
    float eye_x{}, eye_y{}, eye_z{}, at_x{}, at_y{}, at_z{};
};
struct Spear {
    float x{}, y{}, z{};
    std::uint16_t facing{};
    std::int16_t pitch{};
    std::uint16_t mesh{};
    std::optional<SpearView> view; // none: the camera stays with the hunter
};
// How far pitch reaches either way: a quarter turn, straight up or down.
inline constexpr std::int16_t kMostPitch = 16384;

// Where the game's shot of the dragonator starts, its spear's anchor and the
// step it moves by: what game_task's 0x0A151320 gives for the sand ship (start
// 1600 ahead of the spear along it, a step of 350), here for `spear`.
struct SpearShot {
    std::array<float, 3> start{}, anchor{}, step{};
};
[[nodiscard]] SpearShot spear_shot(const Spear &spear);

// An area pack's scene (entry 1) and its nodes' records (entry 2) with the
// first gimmick group made the spear's (kind 14, which the executable's
// 0x088D2E98 makes game_task's spear object of, 0x0A062508) and its first
// node that draws a mesh made the spear: at rest at the spear's place, turned
// as it is, drawing `spear.mesh`. Nothing, with the reason in `why`, when they
// are not laid out as the game's are, the scene has no gimmick group with a
// node that draws a mesh, or one of its nodes draws `spear.mesh`.
struct SpearScene {
    std::vector<std::uint8_t> scene, records;
};
[[nodiscard]] std::optional<SpearScene> place_spear(
    std::span<const std::uint8_t> scene, std::span<const std::uint8_t> records, const Spear &spear, std::string &why);

// The camera's script for the spear's view: the game's script for the sand
// ship's (event 3, `ship`, as the executable has it at 0x08972C8C) with its
// eye (command 0x0402) and target (0x0405) the view's, in units of 1/4096;
// nothing when `ship` is not as traced.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> view_script(
    std::span<const std::uint8_t> ship, const SpearView &view);

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
    std::optional<MapPicture> minimap;  // none: the game's picture and places stay
    std::optional<Spear> spear;         // its dragonator's spear, which its dragonator's button fires
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
// past kBallistaSlots, the area is past kMostAreas, its map picture is not as
// map_picture_problem() wants it, it has a dragonator's button and no spear,
// its spear's place is not finite, its pitch past kMostPitch or its mesh not
// one of its model's, the game's area it takes the place of cannot give the
// spear a gimmick group (place_spear(); its pack is read from the disc), the
// hooks the spear needs cannot be installed, or the loader cannot be taken
// over.
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

// Why the areas `ids`, played by one quest, cannot give the map's picture, or
// nothing: some give one and others none, they give different pictures or
// units, or the quest leaves some of the map's areas to the game and their
// units are not the game's map's (from the game's game_sub, when it can be
// read; it is checked again when the quest is played), or the picture has
// another number of images than the game's map's. Unknown ids are not counted.
[[nodiscard]] std::optional<std::string> quest_map_problem(const std::vector<std::string> &ids);

// Plays `quest` in the area `id` (its own quest, under a mod's id), besides
// the areas it is played in already. False, with the reason in `why`, when
// there is no such area, or it is on another map than the quest's others or
// takes the place of the same game's area as one of them, or the quest's
// ballistas would need words past kBallistaSlots, or its areas' map pictures
// do not go together so far: some give one and others none, they give
// different pictures or units, or the picture has another number of images
// than the game's map's (what needs the quest's whole list, its units on a
// map it leaves areas of to the game, is quest_map_problem()'s).
bool play_quest_in(std::string_view id, std::uint32_t quest, std::string *why = nullptr);

// After the game loads code (an overlay): while a quest whose areas give the
// map's picture is played (all of them served) and game_sub and game_task are
// loaded, writes the map's frame, those areas' dx and dz and the camp's tent
// into game_sub's and game_task's tables; everywhere else puts back the words
// the game loads there (the overlays as the mods have them) where it wrote
// and nothing has written since. An overlay loaded again is written again.
// Nothing is written for a quest whose words are not, before the first
// write, the ones the game loads (another mod changed them after the load),
// nor, from the start, when the mods' overlays differ from the disc's there
// (said once in the log; the game's picture stays).
void overlay_loaded(psprecomp::Runtime &runtime);

// The hooks of the spear: game_task's 0x0A151320 (REPLACE), when asked for
// the dragonator's shot (a2 1) by a hunter (a1) in an area with a spear of
// the quest played, gives its place (start in a3, anchor in t0, step in t1,
// each with a w of 1) and returns 1; and the executable's 0x088C2850, the
// jump of 0x088C2764 into 0x0882A590 with the dragonator's camera event in
// a1, makes it Yakumo's event 13 with the area's view, or 0, none, for a
// spear without one, while the area loaded is such an area. Anything else is
// left to the game's code: false.
bool on_spear_shot(psprecomp::Runtime &runtime, struct ::ymod_cpu &cpu);
bool on_spear_view(psprecomp::Runtime &runtime, struct ::ymod_cpu &cpu);

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
// And the files as the game loads them (the mods' game_sub and game_task)
// through `reader`, instead of the same reader.
void read_loaded_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader);

} // namespace mhp3rd::mods::maps
