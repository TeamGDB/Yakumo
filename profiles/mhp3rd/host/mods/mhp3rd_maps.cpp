#include "mods/mhp3rd_maps.hpp"

#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "mods/mod_ini.hpp"

#include "game/pack_file.hpp"
#include "overlays.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <sstream>

namespace mhp3rd::mods::maps {
namespace {

using psprecomp::Runtime;

// game_sub's tables, a u16 file id by map: an area's pack and its GE data are
// the map's plus 2 for each area before it (docs/MAPS.md).
constexpr std::uint32_t kAreaPacks = 0x0A0457C4u;
constexpr std::uint32_t kAreaGeometry = 0x0A04579Cu;
// And a byte by map: how many areas it has (the executable's 0x088CE544).
constexpr std::uint32_t kAreaCounts = 0x0A0457ECu;
// The map's collision pack: entry 1 + 2 x area is the area's ground, the next
// its walls; after the blocks, entry 1 + 2 x areas + area is its points table.
constexpr FileId kCollisionPacks = 0x125Au;
// The area pack's entries the mod's model and textures take.
constexpr std::size_t kModelEntry = 0u;
constexpr std::size_t kTexturesEntry = 3u;
// The accepted quest's id: u16 at the character's address plus this.
constexpr std::uint32_t kCharacterPointer = 0x08AB3640u;
constexpr std::uint32_t kQuestIdOffset = 0x60472u;
// The map's picture: entry 0 of its collision pack.
constexpr std::size_t kPictureEntry = 0u;
// The overlays whose tables place a map's areas on its picture, as the disc
// has them (files 126 and 123 of DATA.BIN) and where they load.
struct DiscOverlay {
    FileId file;
    std::uint32_t load;
    std::string_view name;
};
constexpr DiscOverlay kGameSub{126u, 0x0A001780u, "game_sub"};
constexpr DiscOverlay kGameTask{123u, 0x0A05E600u, "game_task"};
// game_sub, by map, 20 bytes: the width, depth, x0 and z0 of the world its
// picture's 160 x 160 pixels cover (floats; the executable's 0x088CE854) and a
// pointer to its areas' records, 24 bytes each, whose first two floats, dx and
// dz, take an area's points into that frame (0x088D0C88; 0x088D0D40 takes
// them back). game_task, by map, the camp's tent icon's pixel, s16 u and v
// (0x0A16FF48).
constexpr std::uint32_t kMapFrames = 0x0A044E0Cu;
constexpr std::uint32_t kMapFrameSize = 20u;
constexpr std::uint32_t kAreaRecordSize = 24u;
constexpr std::uint32_t kCampIcons = 0x0A19D1E8u;

// A word Yakumo writes into those tables while a quest is played, and the
// disc's word there, put back afterwards.
struct TableWrite {
    std::uint32_t address{}, value{}, disc{};
};
// What a quest whose areas give the map's picture changes: the picture served
// as the collision pack's entry 0 and the tables' words.
struct MapPlan {
    std::shared_ptr<const std::vector<std::uint8_t>> picture;
    std::vector<TableWrite> writes;
};

struct Added {
    Area area;
    std::vector<std::uint8_t> model, geometry, textures, ground, walls;
    std::optional<Points> points;      // its points, when it gives them
    std::optional<MapPicture> minimap; // the map's picture and its place on it, when it gives them
    std::optional<bool> fits;          // its GE data fits where the game's goes, once known
};

struct Served {
    std::uint32_t quest{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct State {
    std::vector<Added> areas;
    std::map<std::uint32_t, std::optional<MapPlan>> plans;            // quest id -> its map plan, or none, once known
    std::map<std::uint32_t, std::uint32_t> written;                   // address -> the disc's word, where Yakumo wrote
    std::map<FileId, std::optional<std::vector<std::uint8_t>>> discs; // game_sub and game_task, read once
    std::map<std::uint32_t, std::vector<std::size_t>> quests;         // quest id -> its areas
    std::map<FileId, Served> cache;                                   // composed for the quest being played
    std::map<std::uint8_t, std::size_t> collision_entries;            // map -> its collision pack's entries
    std::set<std::uint32_t> refused;                                  // quests whose areas were refused, once logged
    std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader;
    bool installed{};
};

State &state() {
    static State s;
    return s;
}

bool starts_with(const std::vector<std::uint8_t> &bytes, std::string_view magic) {
    return bytes.size() >= magic.size() && std::memcmp(bytes.data(), magic.data(), magic.size()) == 0;
}

std::optional<std::vector<std::uint8_t>> game_file(FileId file) {
    return state().reader ? state().reader(file) : original_file(file);
}

std::optional<std::uint32_t> quest_played(const psprecomp::GuestMemory &memory) {
    if (!memory.contains(kCharacterPointer, 4u)) return std::nullopt;
    const std::uint32_t at = memory.load32(kCharacterPointer) + kQuestIdOffset;
    if (!memory.contains(at, 2u)) return std::nullopt;
    return memory.load16(at);
}

// Whether game_sub, which holds the tables, is loaded over `at`.
bool game_sub_at(const psprecomp::GuestMemory &memory, std::uint32_t at, std::uint32_t size) {
    const std::optional<LoadedOverlay> sub = overlay_at(memory, at);
    return sub && iequals(sub->name, "game_sub") && memory.contains(at, size);
}

// A table's file for the area, while game_sub is loaded.
std::optional<FileId> area_file(const psprecomp::GuestMemory &memory, std::uint32_t table, const Area &area) {
    const std::uint32_t row = table + 2u * area.map;
    if (!game_sub_at(memory, row, 2u)) return std::nullopt;
    return static_cast<FileId>(memory.load16(row) + 2u * area.area);
}

// How many areas the map has, while game_sub is loaded.
std::optional<std::uint8_t> area_count(const psprecomp::GuestMemory &memory, std::uint8_t map) {
    if (!game_sub_at(memory, kAreaCounts + map, 1u)) return std::nullopt;
    return memory.load8(kAreaCounts + map);
}

void put16(std::vector<std::uint8_t> &out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value);
    out[at + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void put32(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t value) {
    put16(out, at, static_cast<std::uint16_t>(value));
    put16(out, at + 2u, static_cast<std::uint16_t>(value >> 16u));
}

void put_float(std::vector<std::uint8_t> &out, std::size_t at, float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    put32(out, at, bits);
}

std::uint32_t get32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1u]) << 8u |
        static_cast<std::uint32_t>(bytes[at + 2u]) << 16u | static_cast<std::uint32_t>(bytes[at + 3u]) << 24u;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_float(std::uint32_t bits) {
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A number as the log writes it: 250, 200.9.
std::string number(float value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

// The disc's overlay, read once: nothing when the file is not that overlay,
// loading where it does.
const std::vector<std::uint8_t> *disc_overlay(const DiscOverlay &overlay) {
    State &s = state();
    auto found = s.discs.find(overlay.file);
    if (found == s.discs.end()) {
        std::optional<std::vector<std::uint8_t>> bytes = game_file(overlay.file);
        const std::optional<p3rd::OverlayImage> image = bytes ? p3rd::overlay_image(*bytes) : std::nullopt;
        const bool named = image && bytes->size() >= 32u + overlay.name.size() &&
            std::memcmp(bytes->data() + 32u, overlay.name.data(), overlay.name.size()) == 0;
        if (!named || image->load != overlay.load) bytes.reset();
        found = s.discs.emplace(overlay.file, std::move(bytes)).first;
    }
    return found->second ? &*found->second : nullptr;
}

// What the disc's overlay holds at `address` once loaded, or nothing outside it.
std::optional<std::uint32_t> disc_word(const DiscOverlay &overlay, std::uint32_t address) {
    const std::vector<std::uint8_t> *bytes = disc_overlay(overlay);
    if (bytes == nullptr || address < overlay.load || bytes->size() < 4u || address - overlay.load > bytes->size() - 4u)
        return std::nullopt;
    return get32(*bytes, address - overlay.load);
}

std::optional<std::uint8_t> disc_byte(const DiscOverlay &overlay, std::uint32_t address) {
    const std::vector<std::uint8_t> *bytes = disc_overlay(overlay);
    if (bytes == nullptr || address < overlay.load || address - overlay.load >= bytes->size()) return std::nullopt;
    return (*bytes)[address - overlay.load];
}

// The overlay whose table holds `address`: game_task's camp icons, game_sub's
// frames and area records.
const DiscOverlay &holder(std::uint32_t address) {
    return address >= kGameTask.load ? kGameTask : kGameSub;
}

// Whether that overlay is loaded over `address` now.
bool holder_loaded(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    const std::optional<LoadedOverlay> overlay = overlay_at(memory, address);
    return overlay && iequals(overlay->name, holder(address).name) && memory.contains(address, 4u);
}

// The map's frame as the disc's game_sub has it: the world its picture covers
// and where its areas' records are.
struct Frame {
    float width{}, depth{}, x0{}, z0{};
    std::uint32_t areas{};
};
std::optional<Frame> disc_frame(std::uint8_t map) {
    const std::uint32_t at = kMapFrames + kMapFrameSize * map;
    const auto width = disc_word(kGameSub, at), depth = disc_word(kGameSub, at + 4u);
    const auto x0 = disc_word(kGameSub, at + 8u), z0 = disc_word(kGameSub, at + 12u);
    const auto areas = disc_word(kGameSub, at + 16u);
    if (!width || !depth || !x0 || !z0 || !areas) return std::nullopt;
    return Frame{bits_float(*width), bits_float(*depth), bits_float(*x0), bits_float(*z0), *areas};
}

// The bytes of a picture of `count` images laid out as the game's.
std::size_t picture_size(std::size_t count) {
    return kPictureHeaderSize + count * kPictureImageSize;
}

std::string images(std::size_t picture_bytes) {
    const std::size_t count =
        picture_bytes >= kPictureHeaderSize ? (picture_bytes - kPictureHeaderSize) / kPictureImageSize : 0u;
    return std::to_string(count) + (count == 1u ? " image" : " images");
}

// The game copies an area's GE data into video memory with the large monsters'
// after it: larger than the game's own, it runs into theirs and the quest
// hangs. Such an area is not served at all, nor the other areas of the quests
// played in it, so its model never names GE data that is not there.
bool fits(Added &a, const psprecomp::GuestMemory &memory) {
    if (a.fits) return *a.fits;
    const std::optional<FileId> file = area_file(memory, kAreaGeometry, a.area);
    if (!file) return false; // asked again once game_sub is there
    const std::optional<std::vector<std::uint8_t>> base = game_file(*file);
    a.fits = base && a.geometry.size() <= base->size();
    if (!*a.fits)
        std::cerr << "[mods] " << a.area.mod << "'s area " << a.area.id << ": its GE data (" << a.geometry.size()
                  << " bytes) is larger than the game's (" << (base ? base->size() : 0u)
                  << "); the game's areas are loaded instead\n";
    return *a.fits;
}

std::shared_ptr<const std::vector<std::uint8_t>> shared(std::vector<std::uint8_t> bytes) {
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
}

// How many entries the map's collision pack has: the disc's, read once.
std::optional<std::size_t> collision_entries(std::uint8_t map) {
    State &s = state();
    if (const auto known = s.collision_entries.find(map); known != s.collision_entries.end()) return known->second;
    const std::optional<std::vector<std::uint8_t>> pack = game_file(kCollisionPacks + map);
    const auto entries = pack ? game::read_pack(*pack) : std::nullopt;
    if (!entries) return std::nullopt;
    return s.collision_entries[map] = entries->size();
}

// Why the quest's areas cannot be played on their map, or nothing. Each must
// be one of the map's areas, as must every area its exits lead to and its
// arrivals come from, and the map's collision pack must hold a block pair
// and a points table for each of its areas; otherwise the blocks and tables
// written would land on others' (the game reads a ground block as a points
// table's group count and runs off the pack) or lead nowhere.
std::optional<std::string> unplayable(const std::vector<const Added *> &areas, std::uint8_t count) {
    const Added &first = *areas.front();
    const std::string map = "map " + std::to_string(first.area.map);
    for (const Added *a : areas) {
        const auto past = [&](std::uint8_t area) { return area >= count; };
        if (past(a->area.area))
            return a->area.id + " takes the place of area " + std::to_string(a->area.area) + " but " + map + " has " +
                std::to_string(count);
        if (!a->points) continue;
        for (const Exit &e : a->points->exits)
            if (past(e.to))
                return a->area.id + "'s exit leads to area " + std::to_string(e.to) + ", past " + map + "'s";
        for (const Arrival &r : a->points->arrivals)
            if (r.from != kQuestStart && past(r.from))
                return a->area.id + "'s arrival comes from area " + std::to_string(r.from) + ", past " + map + "'s";
    }
    const std::optional<std::size_t> entries = collision_entries(first.area.map);
    if (!entries || *entries < 1u + 3u * count)
        return map + "'s collision pack has no block pair and points table for each of its areas";
    return std::nullopt;
}

// Whether the quest's areas can be played: once game_sub (which holds the
// map's area count) is loaded, and only when unplayable() finds nothing.
bool playable(const std::vector<const Added *> &areas, std::uint32_t quest, const psprecomp::GuestMemory &memory) {
    const std::optional<std::uint8_t> count = area_count(memory, areas.front()->area.map);
    if (!count) return false; // asked again once game_sub is there
    const std::optional<std::string> why = unplayable(areas, *count);
    if (why && state().refused.insert(quest).second)
        std::cerr << "[mods] quest " << quest << ": " << *why << "; the game's areas are loaded instead\n";
    return !why;
}

// The quest's areas when all of them are served: each fits and they can be
// played; nothing otherwise. All of them or none: an exit of one leads into
// the others.
std::optional<std::vector<const Added *>> served_areas(const psprecomp::GuestMemory &memory, std::uint32_t quest) {
    State &s = state();
    const auto played = s.quests.find(quest);
    if (played == s.quests.end() || played->second.empty()) return std::nullopt;
    std::vector<const Added *> areas;
    for (const std::size_t i : played->second) {
        if (!fits(s.areas[i], memory)) return std::nullopt;
        areas.push_back(&s.areas[i]);
    }
    if (!playable(areas, quest, memory)) return std::nullopt;
    return areas;
}

// How many of an area's spots are of `kind`.
std::size_t count_spots(const Added &a, SpotKind kind) {
    if (!a.points) return 0u;
    return static_cast<std::size_t>(std::count_if(
        a.points->spots.begin(), a.points->spots.end(), [kind](const Spot &t) { return t.kind == kind; }));
}

// The word after an area's ballistas, numbered from `first` (the quest's
// ballistas are numbered across its areas, in the order it names them), or
// nothing when one of them would be past the game's slots.
std::optional<std::uint16_t> after_ballistas(const Added &a, std::uint16_t first) {
    const std::size_t ballistas = count_spots(a, SpotKind::Ballista);
    if (ballistas == 0u) return first;
    const std::vector<std::uint16_t> words = point_words(a.area.map, a.area.area, ballistas, first);
    if (words.size() < ballistas || words.back() >= kBallistaSlots) return std::nullopt;
    return static_cast<std::uint16_t>(words.back() + 1u);
}

// Why these areas, played by one quest in this order, cannot be, or nothing.
std::optional<std::string> points_problem(const std::vector<const Added *> &areas) {
    std::optional<std::uint16_t> next = 0u;
    for (const Added *a : areas)
        if (next) next = after_ballistas(*a, *next);
    if (!next)
        return "its areas have more ballistas than the game's " + std::to_string(kBallistaSlots) +
            " for a quest (numbered across them, past their areas' conditional words)";
    return std::nullopt;
}

// Why the areas of one quest cannot give the map's picture, or nothing: some
// give it and others not, different pictures or units, a picture with another
// number of images than the game's map's, or, for the quest's `whole` list of
// areas, units other than the game's on a quest that leaves some of the map's
// areas to the game. `strict` when the quest is played: the game's files must
// then be read; otherwise what cannot be read is not checked.
std::optional<std::string> map_problem(const std::vector<const Added *> &areas, bool whole, bool strict) {
    const auto given = static_cast<std::size_t>(
        std::count_if(areas.begin(), areas.end(), [](const Added *a) { return a->minimap.has_value(); }));
    if (given == 0u) return std::nullopt;
    if (given != areas.size()) return "some of its areas give the map's picture and others do not";
    const MapPicture &first = *areas.front()->minimap;
    for (const Added *a : areas)
        if (a->minimap->picture != first.picture || a->minimap->units_x != first.units_x ||
            a->minimap->units_z != first.units_z)
            return "its areas give different map pictures or units";
    const std::uint8_t map = areas.front()->area.map;
    const std::string name = "map " + std::to_string(map);
    const std::optional<std::vector<std::uint8_t>> pack = game_file(kCollisionPacks + map);
    const auto entries = pack ? game::read_pack(*pack) : std::nullopt;
    if (entries && !entries->empty()) {
        if ((*entries)[kPictureEntry].size() != first.picture.size())
            return "its map picture has " + images(first.picture.size()) + ", " + name + "'s has " +
                images((*entries)[kPictureEntry].size());
    } else if (strict) {
        return name + "'s collision pack cannot be read";
    }
    if (!whole) return std::nullopt;
    // The game's areas keep their places on the picture only at the game's
    // scale: their records are the game's.
    const std::optional<std::uint8_t> count = disc_byte(kGameSub, kAreaCounts + map);
    const std::optional<Frame> frame = disc_frame(map);
    if (!count || !frame) {
        if (strict) return name + "'s tables cannot be read from the game's game_sub";
        return std::nullopt;
    }
    const float side = static_cast<float>(kPictureSide);
    if (areas.size() < *count && (side * first.units_x != frame->width || side * first.units_z != frame->depth))
        return "it leaves some of " + name + "'s areas to the game, whose picture has " + number(frame->width / side) +
            " by " + number(frame->depth / side) + " units a pixel, and its areas' have " + number(first.units_x) +
            " by " + number(first.units_z);
    return std::nullopt;
}

// What the quest's areas change for the map's picture, or nothing, with the
// reason in `why` when they give it but it cannot be shown.
std::optional<MapPlan> make_plan(const std::vector<const Added *> &areas, std::string &why) {
    if (std::none_of(areas.begin(), areas.end(), [](const Added *a) { return a->minimap.has_value(); }))
        return std::nullopt;
    if (std::optional<std::string> problem = map_problem(areas, true, true)) {
        why = std::move(*problem);
        return std::nullopt;
    }
    const std::uint8_t map = areas.front()->area.map;
    const Frame frame = *disc_frame(map); // map_problem() read it
    const MapPicture &picture = *areas.front()->minimap;
    MapPlan plan;
    plan.picture = std::make_shared<const std::vector<std::uint8_t>>(picture.picture);
    bool inside = true;
    const auto write = [&plan, &inside](std::uint32_t address, std::uint32_t value) {
        const std::optional<std::uint32_t> disc = disc_word(holder(address), address);
        if (!disc)
            inside = false;
        else
            plan.writes.push_back({address, value, *disc});
    };
    // The frame: 160 pixels of the area's units across and down, from the
    // game's x0 and z0, which stay.
    const std::uint32_t record = kMapFrames + kMapFrameSize * map;
    const float side = static_cast<float>(kPictureSide);
    write(record, float_bits(side * picture.units_x));
    write(record + 4u, float_bits(side * picture.units_z));
    for (const Added *a : areas) {
        const float dx = a->minimap->origin_u * picture.units_x + frame.x0;
        const float dz = a->minimap->origin_v * picture.units_z + frame.z0;
        if (!std::isfinite(dx) || !std::isfinite(dz)) {
            why = a->area.id + "'s place on the map is not a finite number in its frame";
            return std::nullopt;
        }
        const std::uint32_t at = frame.areas + kAreaRecordSize * a->area.area;
        write(at, float_bits(dx));
        write(at + 4u, float_bits(dz));
        if (a->area.area == 0u)
            write(kCampIcons + 4u * map,
                static_cast<std::uint16_t>(a->minimap->camp_u) |
                    static_cast<std::uint32_t>(static_cast<std::uint16_t>(a->minimap->camp_v)) << 16u);
    }
    if (!inside) {
        why = "map " + std::to_string(map) + "'s records are not in the game's game_sub and game_task as traced";
        return std::nullopt;
    }
    return plan;
}

// The quest's map plan, made once; a quest whose areas give a picture that
// cannot be shown is said once in the log, and keeps the game's.
const MapPlan *plan_for(std::uint32_t quest, const std::vector<const Added *> &areas) {
    State &s = state();
    if (const auto known = s.plans.find(quest); known != s.plans.end())
        return known->second ? &*known->second : nullptr;
    std::string why;
    std::optional<MapPlan> &plan = s.plans[quest] = make_plan(areas, why);
    if (!plan && !why.empty())
        std::cerr << "[mods] quest " << quest << ": " << why << "; the game's map picture is shown\n";
    return plan ? &*plan : nullptr;
}

// The map's collision pack with every one of the quest's areas' blocks, and
// the points tables of those that give theirs (playable() has checked that
// the pack has them all), the quest's ballistas numbered across them.
std::shared_ptr<const std::vector<std::uint8_t>> compose_collision(
    const std::vector<const Added *> &areas, FileId file, std::uint32_t quest, const psprecomp::GuestMemory &memory) {
    const std::optional<std::vector<std::uint8_t>> base = game_file(file);
    const std::optional<std::uint8_t> count = area_count(memory, areas.front()->area.map);
    if (!base || !count) return nullptr;
    std::vector<std::vector<std::uint8_t>> tables;
    tables.reserve(areas.size());
    std::vector<std::pair<std::size_t, std::span<const std::uint8_t>>> replacements;
    std::uint16_t ballista = 0u;
    for (const Added *a : areas) {
        const std::size_t blocks = 1u + 2u * a->area.area;
        replacements.emplace_back(blocks, a->ground);
        replacements.emplace_back(blocks + 1u, a->walls);
        if (!a->points) continue;
        replacements.emplace_back(1u + 2u * *count + a->area.area,
            tables.emplace_back(write_points(*a->points, {a->area.map, a->area.area, ballista})));
        ballista = after_ballistas(*a, ballista).value_or(kBallistaSlots); // play_quest_in() kept them within
    }
    // The map's picture, when the areas give it and it can be shown.
    if (const MapPlan *plan = plan_for(quest, areas)) replacements.emplace_back(kPictureEntry, *plan->picture);
    if (auto pack = game::replace_entries(*base, replacements)) return shared(std::move(*pack));
    return nullptr;
}

std::shared_ptr<const std::vector<std::uint8_t>> compose(
    const std::vector<const Added *> &areas, FileId file, std::uint32_t quest, const psprecomp::GuestMemory &memory) {
    for (const Added *a : areas) {
        if (file == area_file(memory, kAreaGeometry, a->area)) return shared(a->geometry);
        if (file != area_file(memory, kAreaPacks, a->area)) continue;
        const std::optional<std::vector<std::uint8_t>> base = game_file(file);
        if (!base) return nullptr;
        if (auto pack = game::replace_entries(*base, {{kModelEntry, a->model}, {kTexturesEntry, a->textures}}))
            return shared(std::move(*pack));
        std::cerr << "[mods] " << a->area.mod << "'s area " << a->area.id << ": the game's area pack "
                  << psprecomp::hex32(file) << " is not as expected; the game's is loaded\n";
        return nullptr;
    }
    if (file == kCollisionPacks + areas.front()->area.map) return compose_collision(areas, file, quest, memory);
    return nullptr;
}

// Why a spot's gathering cannot be the game's, or nothing. A gathering
// point gives 1 to kMostGatherItems items, each named, at least 1 of it,
// their chances adding up to 100 as every list on the disc's quests does,
// `uses` (and extra ones) from 1 to kMostGatherUses; nothing else gives any.
std::optional<std::string> gathering_problem(const Spot &spot) {
    const Gathering &g = spot.gathering;
    if (spot.kind != SpotKind::Gathering) {
        if (!g.items.empty() || g.uses != 0u || g.extra_uses != 0u) return "only a gathering point gives items";
        return std::nullopt;
    }
    if (g.items.empty() || g.items.size() > kMostGatherItems)
        return "a gathering point gives no item or more than " + std::to_string(kMostGatherItems);
    unsigned total = 0;
    for (const GatherItem &i : g.items) {
        if (i.item.empty() || i.count == 0u || i.chance == 0u)
            return "a gathered item is unnamed or none of it is given";
        total += i.chance;
    }
    if (total != 100u) return "a gathering point's chances do not add up to 100";
    if (g.uses == 0u || unsigned{g.uses} + g.extra_uses > kMostGatherUses)
        return "a gathering point's uses are not 1 to " + std::to_string(kMostGatherUses);
    return std::nullopt;
}

} // namespace

bool add_area(Runtime &runtime, AreaSpec spec, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    if (spec.id.empty() || find(spec.id) != nullptr) return fail("the id \"" + spec.id + "\" is taken or empty");
    if (spec.map >= kMaps) return fail("map " + std::to_string(spec.map) + " is not one of the game's");
    if (!starts_with(spec.model,
            std::string_view("pmo\0"
                             "102\0",
                8)))
        return fail("its model is not a PMO \"102\"");
    if (spec.geometry.empty()) return fail("it has no GE data");
    if (!starts_with(spec.textures, ".TMH")) return fail("its textures are not a TMH");
    if (!starts_with(spec.ground, "4.00") || !starts_with(spec.walls, "4.00"))
        return fail("its ground or walls are not a collision block (\"4.00\")");
    if (spec.area >= kMostAreas)
        return fail(
            "area " + std::to_string(spec.area) + " is past the " + std::to_string(kMostAreas) + " a map can have");
    if (spec.points) {
        const Points &p = *spec.points;
        if (p.exits.size() > kMostPoints || p.arrivals.size() > kMostPoints || p.spots.size() > kMostPoints)
            return fail("it has more than " + std::to_string(kMostPoints) + " exits, arrivals or things to use");
        const auto finite = [](std::initializer_list<float> values) {
            return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
        };
        const auto size = [](float v) { return std::isfinite(v) && v > 0.0f; };
        for (const Exit &e : p.exits) {
            if (!finite({e.x, e.y, e.z}) || !size(e.radius) || !size(e.height))
                return fail("an exit's place is not a number or its size is not above 0");
            if (e.to == spec.area) return fail("an exit leads to its own area");
        }
        for (const Arrival &a : p.arrivals)
            if (!finite({a.x, a.y, a.z})) return fail("an arrival's place is not a number");
        for (const Spot &t : p.spots) {
            if (!finite({t.x, t.y, t.z}) || !size(t.radius) || !size(t.height))
                return fail("a thing to use's place is not a number or its size is not above 0");
            if (std::optional<std::string> wrong = gathering_problem(t)) return fail(std::move(*wrong));
        }
        if (p.cart && !finite({p.cart->x, p.cart->y, p.cart->z})) return fail("its cart's place is not a number");
        // The game keeps one cart for the map, the camp's (area 0): game_task
        // puts it there in the camp, and every cart on the disc is area 0's.
        if (p.cart && spec.area != 0u) return fail("only a camp (area 0) has a cart");
        const auto ballistas = static_cast<std::size_t>(
            std::count_if(p.spots.begin(), p.spots.end(), [](const Spot &t) { return t.kind == SpotKind::Ballista; }));
        if (ballistas > 0u && point_words(spec.map, spec.area, ballistas, 0u).back() >= kBallistaSlots)
            return fail("it has more ballistas than the game's " + std::to_string(kBallistaSlots) +
                " for a quest (past its area's conditional words)");
    }
    if (spec.minimap)
        if (std::optional<std::string> wrong = map_picture_problem(*spec.minimap, spec.area))
            return fail(std::move(*wrong));
    if (!s.installed) {
        if (!added::stand_in(runtime, [](Runtime &r, FileId file) { return serve(r, file); }))
            return fail("the game's file loader cannot be taken over");
        s.installed = true;
    }
    std::cout << "[mods] " << spec.mod << " adds area " << spec.id << " in place of map " << int{spec.map} << "'s area "
              << int{spec.area} << "\n";
    s.areas.push_back({{std::move(spec.id), spec.map, spec.area, std::move(spec.mod)}, std::move(spec.model),
        std::move(spec.geometry), std::move(spec.textures), std::move(spec.ground), std::move(spec.walls),
        std::move(spec.points), std::move(spec.minimap)});
    return true;
}

const Area *find(std::string_view id) {
    for (const Added &a : state().areas)
        if (a.area.id == id) return &a.area;
    return nullptr;
}

std::optional<std::string> picture_problem(const std::vector<std::uint8_t> &picture) {
    const std::string_view signature = ".TMH0.14";
    if (picture.size() < kPictureHeaderSize || std::memcmp(picture.data(), signature.data(), signature.size()) != 0)
        return "its map picture is not a TMH";
    const std::uint32_t count = get32(picture, 8u);
    if ((count != 1u && count != 2u) || get32(picture, 12u) != 0u || picture.size() != picture_size(count))
        return "its map picture is not one or two images of " + std::to_string(picture_size(1u)) + " or " +
            std::to_string(picture_size(2u)) + " bytes in all, as the game's are";
    // Each image: its entry (size, 0, 1, a palette), its pixels (size with
    // their header, 1, 4-bit indices, 160 by 168) and its palette (size, 2,
    // RGBA8888, 16 colours).
    constexpr std::uint32_t kPixels = 16u + kPictureSide * 168u / 2u;
    constexpr std::array<std::uint32_t, 8> kEntry{
        static_cast<std::uint32_t>(kPictureImageSize), 0u, 1u, 1u, kPixels, 1u, 4u, kPictureSide | 168u << 16u};
    constexpr std::array<std::uint32_t, 4> kPalette{16u + 16u * 4u, 2u, 3u, 16u};
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = kPictureHeaderSize + i * kPictureImageSize;
        bool laid_out = true;
        for (std::size_t w = 0; w < kEntry.size(); ++w) laid_out &= get32(picture, at + 4u * w) == kEntry[w];
        for (std::size_t w = 0; w < kPalette.size(); ++w)
            laid_out &= get32(picture, at + 16u + kPixels + 4u * w) == kPalette[w];
        if (!laid_out)
            return "its map picture's image " + std::to_string(i + 1u) +
                " is not 160 by 168 pixels of 4-bit indices into 16 RGBA8888 colours";
    }
    return std::nullopt;
}

std::optional<std::string> map_picture_problem(const MapPicture &map, std::uint8_t area) {
    if (std::optional<std::string> wrong = picture_problem(map.picture)) return wrong;
    const float side = static_cast<float>(kPictureSide);
    const auto units = [side](float v) { return std::isfinite(v) && v > 0.0f && std::isfinite(side * v); };
    if (!units(map.units_x) || !units(map.units_z)) return "its map's units are not finite numbers above 0";
    // Its place stays finite in any frame the game's maps have (theirs reach
    // 35000 from the middle).
    constexpr float kFarthest = 1.0e30f;
    const auto place = [kFarthest](float origin, float units) {
        return std::isfinite(origin) && std::fabs(origin * units) <= kFarthest;
    };
    if (!place(map.origin_u, map.units_x) || !place(map.origin_v, map.units_z))
        return "its place on the map is not a finite number";
    const auto pixel = [](std::int32_t v) { return v >= 0 && v < static_cast<std::int32_t>(kPictureSide); };
    if (area == 0u && (!pixel(map.camp_u) || !pixel(map.camp_v)))
        return "its camp's tent is off the map's picture (its pixels are 0 to " + std::to_string(kPictureSide - 1u) +
            ")";
    return std::nullopt;
}

std::optional<std::string> quest_map_problem(const std::vector<std::string> &ids) {
    std::vector<const Added *> areas;
    for (const std::string &id : ids)
        for (const Added &a : state().areas)
            if (a.area.id == id) areas.push_back(&a);
    if (areas.empty()) return std::nullopt;
    return map_problem(areas, true, false);
}

std::vector<std::uint16_t> point_words(std::uint8_t map, std::uint8_t area, std::size_t count, std::uint16_t first) {
    std::vector<std::uint16_t> out;
    for (std::uint32_t word = first; out.size() < count && word <= 0xFFFFu; ++word) {
        const bool conditional = std::any_of(std::begin(kConditionalWords), std::end(kConditionalWords),
            [&](const ConditionalWord &c) { return c.map == map && c.area == area && c.word == word; });
        if (!conditional) out.push_back(static_cast<std::uint16_t>(word));
    }
    return out;
}

std::vector<Gathering> gathering(std::string_view id) {
    std::vector<Gathering> out;
    for (const Added &a : state().areas) {
        if (a.area.id != id || !a.points) continue;
        const std::vector<std::uint16_t> words =
            point_words(a.area.map, a.area.area, count_spots(a, SpotKind::Gathering), 1u);
        std::size_t k = 0;
        for (const Spot &t : a.points->spots) {
            if (t.kind != SpotKind::Gathering) continue;
            out.resize(words[k], Gathering{}); // a conditional word passed over keeps an empty list
            out[words[k++] - 1u] = t.gathering;
        }
    }
    return out;
}

std::optional<std::string> quest_points_problem(const std::vector<std::string> &ids) {
    std::vector<const Added *> areas;
    for (const std::string &id : ids)
        for (const Added &a : state().areas)
            if (a.area.id == id) areas.push_back(&a);
    return points_problem(areas);
}

bool play_quest_in(std::string_view id, std::uint32_t quest, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    const auto found = std::find_if(s.areas.begin(), s.areas.end(), [id](const Added &a) { return a.area.id == id; });
    if (found == s.areas.end()) return fail("there is no area \"" + std::string(id) + "\"");
    const auto index = static_cast<std::size_t>(found - s.areas.begin());
    std::vector<std::size_t> &played = s.quests[quest];
    for (const std::size_t other : played) {
        if (other == index) return true;
        const Area &a = s.areas[other].area;
        if (a.map != found->area.map)
            return fail("its area " + found->area.id + " is on map " + std::to_string(found->area.map) + ", its area " +
                a.id + " on map " + std::to_string(a.map));
        if (a.area == found->area.area)
            return fail("its areas " + a.id + " and " + found->area.id + " both take the place of area " +
                std::to_string(a.area));
    }
    std::vector<const Added *> areas;
    for (const std::size_t other : played) areas.push_back(&s.areas[other]);
    areas.push_back(&*found);
    if (std::optional<std::string> problem = points_problem(areas)) return fail(std::move(*problem));
    if (std::optional<std::string> problem = map_problem(areas, false, false)) return fail(std::move(*problem));
    played.push_back(index);
    s.plans.erase(quest); // made again with the area
    return true;
}

std::vector<std::uint8_t> write_points(const Points &points, const Numbering &numbering) {
    // Records of 32 bytes: x, y, z, 1; two sizes; the facing, the record's
    // index in its group, a u16 and two bytes, which each kind reads its way.
    struct Record {
        float x, y, z, a, b;
        std::uint16_t facing, index, word;
        std::uint8_t byte30, byte31;
    };
    std::vector<std::pair<std::uint32_t, std::vector<Record>>> groups;
    const auto index = [](std::size_t i) { return static_cast<std::uint8_t>(i); };
    if (!points.exits.empty()) {
        std::vector<Record> &out = groups.emplace_back(0u, std::vector<Record>{}).second;
        for (std::size_t i = 0; i < points.exits.size(); ++i) {
            const Exit &e = points.exits[i];
            out.push_back({e.x, e.y, e.z, e.radius, e.height, 0u, index(i), e.door, e.to, index(i)});
        }
    }
    if (!points.arrivals.empty()) {
        std::vector<Record> &out = groups.emplace_back(1u, std::vector<Record>{}).second;
        for (std::size_t i = 0; i < points.arrivals.size(); ++i) {
            const Arrival &a = points.arrivals[i];
            out.push_back({a.x, a.y, a.z, 1.0f, 1.0f, a.facing, index(i), a.door, a.from, index(i)});
        }
    }
    if (!points.spots.empty()) {
        // The word: a gathering point's list in the quest, from 1 (game_task
        // reads entry word - 1, 0x0A175684); a cannon's number among the
        // area's, from 0, and a ballista's slot, as on the disc's sand ship
        // and Sacred Peak (map 6, areas 1 and 2; map 13, area 1); none of them
        // one of the area's conditional words; 0 for the camp's.
        std::map<SpotKind, std::vector<std::uint16_t>> words;
        std::map<SpotKind, std::size_t> used;
        const auto numbered = [&](SpotKind kind, std::uint16_t first) {
            std::size_t count = 0;
            for (const Spot &t : points.spots) count += t.kind == kind;
            words[kind] = point_words(numbering.map, numbering.area, count, first);
        };
        numbered(SpotKind::Gathering, 1u);
        numbered(SpotKind::Cannon, 0u);
        numbered(SpotKind::Ballista, numbering.first_ballista);
        std::vector<Record> &out = groups.emplace_back(2u, std::vector<Record>{}).second;
        for (std::size_t i = 0; i < points.spots.size(); ++i) {
            const Spot &t = points.spots[i];
            const auto own = words.find(t.kind);
            const std::uint16_t word = own != words.end() ? own->second[used[t.kind]++] : 0u;
            out.push_back(
                {t.x, t.y, t.z, t.radius, t.height, t.facing, index(i), word, static_cast<std::uint8_t>(t.kind), 0u});
        }
    }
    if (points.cart) {
        const Cart &c = *points.cart;
        groups.emplace_back(4u, std::vector<Record>{{c.x, c.y, c.z, 1.0f, 1.0f, c.facing, 0u, 0u, 0u, 0u}});
    }
    // A count and a row per group (kind, count, offset), then the records from
    // the next 16 bytes, as the game's tables are laid out.
    std::size_t at = (4u + 12u * groups.size() + 15u) & ~std::size_t{15u};
    std::size_t size = at;
    for (const auto &group : groups) size += 32u * group.second.size();
    std::vector<std::uint8_t> out(size, 0u);
    put32(out, 0u, static_cast<std::uint32_t>(groups.size()));
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const std::vector<Record> &records = groups[g].second;
        put32(out, 4u + 12u * g, groups[g].first);
        put32(out, 8u + 12u * g, static_cast<std::uint32_t>(records.size()));
        put32(out, 12u + 12u * g, static_cast<std::uint32_t>(at));
        for (const Record &r : records) {
            put_float(out, at, r.x);
            put_float(out, at + 4u, r.y);
            put_float(out, at + 8u, r.z);
            put_float(out, at + 12u, 1.0f);
            put_float(out, at + 16u, r.a);
            put_float(out, at + 20u, r.b);
            put16(out, at + 24u, r.facing);
            put16(out, at + 26u, r.index);
            put16(out, at + 28u, r.word);
            out[at + 30u] = r.byte30;
            out[at + 31u] = r.byte31;
            at += 32u;
        }
    }
    return out;
}

std::shared_ptr<const std::vector<std::uint8_t>> serve(Runtime &runtime, FileId file) {
    State &s = state();
    if (s.quests.empty()) return nullptr;
    const std::optional<std::uint32_t> quest = quest_played(runtime.memory());
    const auto played = quest ? s.quests.find(*quest) : s.quests.end();
    if (played == s.quests.end() || played->second.empty()) return nullptr;
    // The size and then the copy of one load ask in turn: composed once.
    if (const auto cached = s.cache.find(file); cached != s.cache.end() && cached->second.quest == *quest)
        return cached->second.bytes;
    const std::optional<std::vector<const Added *>> areas = served_areas(runtime.memory(), *quest);
    if (!areas) return nullptr;
    auto bytes = compose(*areas, file, *quest, runtime.memory());
    if (bytes) s.cache[file] = {*quest, bytes};
    return bytes;
}

void overlay_loaded(Runtime &runtime) {
    State &s = state();
    if (s.quests.empty() && s.written.empty()) return;
    auto &memory = runtime.memory();
    // The quest played, on a quest: game_task holds its HUD and game_sub the
    // tables, and the quest's areas are served.
    std::vector<TableWrite> wanted;
    const std::optional<LoadedOverlay> task = overlay_at(memory, kGameTask.load);
    const std::optional<LoadedOverlay> sub = overlay_at(memory, kGameSub.load);
    const std::optional<std::uint32_t> quest = quest_played(memory);
    if (task && iequals(task->name, kGameTask.name) && sub && iequals(sub->name, kGameSub.name) && quest &&
        s.quests.contains(*quest))
        if (const auto areas = served_areas(memory, *quest))
            if (const MapPlan *plan = plan_for(*quest, *areas)) wanted = plan->writes;
    // The game's words back where Yakumo wrote and wants nothing now; an
    // overlay that has gone took Yakumo's with it, and comes back with the
    // disc's.
    for (auto it = s.written.begin(); it != s.written.end();) {
        const bool kept =
            std::any_of(wanted.begin(), wanted.end(), [&it](const TableWrite &w) { return w.address == it->first; });
        if (kept) {
            ++it;
            continue;
        }
        if (holder_loaded(memory, it->first)) memory.store32(it->first, it->second);
        it = s.written.erase(it);
    }
    for (const TableWrite &w : wanted) {
        if (!holder_loaded(memory, w.address)) continue;
        s.written.try_emplace(w.address, w.disc);
        memory.store32(w.address, w.value);
    }
}

void reset_for_tests() {
    state() = {};
}

void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader) {
    state().reader = std::move(reader);
}

} // namespace mhp3rd::mods::maps
