#include "mods/mhp3rd_maps.hpp"

#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "mods/mod_ini.hpp"

#include "game/pack_file.hpp"
#include "overlays.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <cmath>
#include <map>
#include <set>
#include <span>

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

struct Added {
    Area area;
    std::vector<std::uint8_t> model, geometry, textures, ground, walls;
    std::optional<Points> points; // its points, when it gives them
    std::optional<bool> fits;     // its GE data fits where the game's goes, once known
};

struct Served {
    std::uint32_t quest{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct State {
    std::vector<Added> areas;
    std::map<std::uint32_t, std::vector<std::size_t>> quests; // quest id -> its areas
    std::map<FileId, Served> cache;                           // composed for the quest being played
    std::map<std::uint8_t, std::size_t> collision_entries;    // map -> its collision pack's entries
    std::set<std::uint32_t> refused;                          // quests whose areas were refused, once logged
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

// The map's collision pack with every one of the quest's areas' blocks, and
// the points tables of those that give theirs (playable() has checked that
// the pack has them all).
std::shared_ptr<const std::vector<std::uint8_t>> compose_collision(
    const std::vector<const Added *> &areas, FileId file, const psprecomp::GuestMemory &memory) {
    const std::optional<std::vector<std::uint8_t>> base = game_file(file);
    const std::optional<std::uint8_t> count = area_count(memory, areas.front()->area.map);
    if (!base || !count) return nullptr;
    std::vector<std::vector<std::uint8_t>> tables;
    tables.reserve(areas.size());
    std::vector<std::pair<std::size_t, std::span<const std::uint8_t>>> replacements;
    for (const Added *a : areas) {
        const std::size_t blocks = 1u + 2u * a->area.area;
        replacements.emplace_back(blocks, a->ground);
        replacements.emplace_back(blocks + 1u, a->walls);
        if (!a->points) continue;
        replacements.emplace_back(1u + 2u * *count + a->area.area, tables.emplace_back(write_points(*a->points)));
    }
    if (auto pack = game::replace_entries(*base, replacements)) return shared(std::move(*pack));
    return nullptr;
}

std::shared_ptr<const std::vector<std::uint8_t>> compose(
    const std::vector<const Added *> &areas, FileId file, const psprecomp::GuestMemory &memory) {
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
    if (file == kCollisionPacks + areas.front()->area.map) return compose_collision(areas, file, memory);
    return nullptr;
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
        for (const Spot &t : p.spots)
            if (!finite({t.x, t.y, t.z}) || !size(t.radius) || !size(t.height))
                return fail("a thing to use's place is not a number or its size is not above 0");
        if (p.cart && !finite({p.cart->x, p.cart->y, p.cart->z})) return fail("its cart's place is not a number");
        // The game keeps one cart for the map, the camp's (area 0): game_task
        // puts it there in the camp, and every cart on the disc is area 0's.
        if (p.cart && spec.area != 0u) return fail("only a camp (area 0) has a cart");
    }
    if (!s.installed) {
        if (!added::stand_in(runtime, [](Runtime &r, FileId file) { return serve(r, file); }))
            return fail("the game's file loader cannot be taken over");
        s.installed = true;
    }
    std::cout << "[mods] " << spec.mod << " adds area " << spec.id << " in place of map " << int{spec.map} << "'s area "
              << int{spec.area} << "\n";
    s.areas.push_back({{std::move(spec.id), spec.map, spec.area, std::move(spec.mod)}, std::move(spec.model),
        std::move(spec.geometry), std::move(spec.textures), std::move(spec.ground), std::move(spec.walls),
        std::move(spec.points)});
    return true;
}

const Area *find(std::string_view id) {
    for (const Added &a : state().areas)
        if (a.area.id == id) return &a.area;
    return nullptr;
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
    played.push_back(index);
    return true;
}

std::vector<std::uint8_t> write_points(const Points &points) {
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
        std::vector<Record> &out = groups.emplace_back(2u, std::vector<Record>{}).second;
        for (std::size_t i = 0; i < points.spots.size(); ++i) {
            const Spot &t = points.spots[i];
            out.push_back(
                {t.x, t.y, t.z, t.radius, t.height, t.facing, index(i), 0u, static_cast<std::uint8_t>(t.kind), 0u});
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
    // All of the quest's areas or none: an exit of one leads into the others.
    std::vector<const Added *> areas;
    for (const std::size_t i : played->second) {
        if (!fits(s.areas[i], runtime.memory())) return nullptr;
        areas.push_back(&s.areas[i]);
    }
    if (!playable(areas, *quest, runtime.memory())) return nullptr;
    auto bytes = compose(areas, file, runtime.memory());
    if (bytes) s.cache[file] = {*quest, bytes};
    return bytes;
}

void reset_for_tests() {
    state() = {};
}

void read_files_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader) {
    state().reader = std::move(reader);
}

} // namespace mhp3rd::mods::maps
