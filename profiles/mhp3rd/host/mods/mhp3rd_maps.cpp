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
#include <map>

namespace mhp3rd::mods::maps {
namespace {

using psprecomp::Runtime;

// game_sub's tables, a u16 file id by map: an area's pack and its GE data are
// the map's plus 2 for each area before it (docs/MAPS.md).
constexpr std::uint32_t kAreaPacks = 0x0A0457C4u;
constexpr std::uint32_t kAreaGeometry = 0x0A04579Cu;
// The map's collision pack: entry 1 + 2 x area is the area's ground, the next its walls.
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
    std::optional<bool> fits; // its GE data fits where the game's goes, once known
};

struct Served {
    std::uint32_t quest{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct State {
    std::vector<Added> areas;
    std::map<std::uint32_t, std::size_t> quests; // quest id -> area
    std::map<FileId, Served> cache;              // composed for the quest being played
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

// A table's file for the area, while game_sub (which holds the table) is loaded.
std::optional<FileId> area_file(const psprecomp::GuestMemory &memory, std::uint32_t table, const Area &area) {
    const std::uint32_t row = table + 2u * area.map;
    const std::optional<LoadedOverlay> sub = overlay_at(memory, row);
    if (!sub || !iequals(sub->name, "game_sub") || !memory.contains(row, 2u)) return std::nullopt;
    return static_cast<FileId>(memory.load16(row) + 2u * area.area);
}

// The game copies an area's GE data into video memory with the large monsters'
// after it: larger than the game's own, it runs into theirs and the quest
// hangs. Such an area is not served at all, so its model never names GE data
// that is not there.
bool fits(Added &a, const psprecomp::GuestMemory &memory) {
    if (a.fits) return *a.fits;
    const std::optional<FileId> file = area_file(memory, kAreaGeometry, a.area);
    if (!file) return false; // asked again once game_sub is there
    const std::optional<std::vector<std::uint8_t>> base = game_file(*file);
    a.fits = base && a.geometry.size() <= base->size();
    if (!*a.fits)
        std::cerr << "[mods] " << a.area.mod << "'s area " << a.area.id << ": its GE data (" << a.geometry.size()
                  << " bytes) is larger than the game's (" << (base ? base->size() : 0u)
                  << "); the game's area is loaded instead\n";
    return *a.fits;
}

std::shared_ptr<const std::vector<std::uint8_t>> compose(
    const Added &a, FileId file, const psprecomp::GuestMemory &memory) {
    const auto shared = [](std::vector<std::uint8_t> bytes) {
        return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    };
    if (file == area_file(memory, kAreaGeometry, a.area)) return shared(a.geometry);
    if (file == area_file(memory, kAreaPacks, a.area)) {
        const std::optional<std::vector<std::uint8_t>> base = game_file(file);
        if (!base) return nullptr;
        if (auto pack = game::replace_entries(*base, {{kModelEntry, a.model}, {kTexturesEntry, a.textures}}))
            return shared(std::move(*pack));
        std::cerr << "[mods] " << a.area.mod << "'s area " << a.area.id << ": the game's area pack "
                  << psprecomp::hex32(file) << " is not as expected; the game's is loaded\n";
        return nullptr;
    }
    if (file == kCollisionPacks + a.area.map) {
        const std::optional<std::vector<std::uint8_t>> base = game_file(file);
        if (!base) return nullptr;
        const std::size_t first = 1u + 2u * a.area.area;
        if (auto pack = game::replace_entries(*base, {{first, a.ground}, {first + 1u, a.walls}}))
            return shared(std::move(*pack));
        std::cerr << "[mods] " << a.area.mod << "'s area " << a.area.id << ": the game's collision pack "
                  << psprecomp::hex32(file) << " has no blocks for area " << int{a.area.area}
                  << "; the game's is loaded\n";
        return nullptr;
    }
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
    if (!s.installed) {
        if (!added::stand_in(runtime, [](Runtime &r, FileId file) { return serve(r, file); }))
            return fail("the game's file loader cannot be taken over");
        s.installed = true;
    }
    std::cout << "[mods] " << spec.mod << " adds area " << spec.id << " in place of map " << int{spec.map} << "'s area "
              << int{spec.area} << "\n";
    s.areas.push_back({{std::move(spec.id), spec.map, spec.area, std::move(spec.mod)}, std::move(spec.model),
        std::move(spec.geometry), std::move(spec.textures), std::move(spec.ground), std::move(spec.walls)});
    return true;
}

const Area *find(std::string_view id) {
    for (const Added &a : state().areas)
        if (a.area.id == id) return &a.area;
    return nullptr;
}

bool play_quest_in(std::string_view id, std::uint32_t quest) {
    State &s = state();
    for (std::size_t i = 0; i < s.areas.size(); ++i) {
        if (s.areas[i].area.id != id) continue;
        s.quests[quest] = i;
        return true;
    }
    return false;
}

std::shared_ptr<const std::vector<std::uint8_t>> serve(Runtime &runtime, FileId file) {
    State &s = state();
    if (s.quests.empty()) return nullptr;
    const std::optional<std::uint32_t> quest = quest_played(runtime.memory());
    const auto played = quest ? s.quests.find(*quest) : s.quests.end();
    if (played == s.quests.end()) return nullptr;
    // The size and then the copy of one load ask in turn: composed once.
    if (const auto cached = s.cache.find(file); cached != s.cache.end() && cached->second.quest == *quest)
        return cached->second.bytes;
    Added &area = s.areas[played->second];
    if (!fits(area, runtime.memory())) return nullptr;
    auto bytes = compose(area, file, runtime.memory());
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
