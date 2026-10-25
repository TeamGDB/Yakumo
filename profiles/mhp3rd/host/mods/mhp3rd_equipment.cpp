#include "mods/mhp3rd_equipment.hpp"

#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_tables.hpp"
#include "mods/mhp3rd_texts.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_native.hpp"

#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#include "overlays.hpp"

#include "psprecomp/runtime.hpp"
#include "yakumo/mod.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>

namespace mhp3rd::mods::equipment {
namespace {

using psprecomp::Runtime;

struct Class {
    WeaponClass weapon;
    std::string_view table; // its game table
    // The game tables the game reads by model number with no bound: the
    // sound banks for every class, and for the bowguns a byte that picks a
    // placement; empty where it has none.
    std::array<std::string_view, 2> by_model{};
    std::uint32_t names{}; // its text tables: names by id, then descriptions
    std::uint32_t descriptions{};
};
// The name tables pair with the kinds as docs/DEBUG_MENU.md lists them.
constexpr std::array<Class, 12> kClasses{{
    {{"great_sword", 5u}, "weapon.great_sword", {"weapon.great_sword.sounds"}, 5u, 6u},
    {{"sword_and_shield", 6u}, "weapon.sword_and_shield", {"weapon.sword_and_shield.sounds"}, 7u, 8u},
    {{"hammer", 7u}, "weapon.hammer", {"weapon.hammer.sounds"}, 9u, 10u},
    {{"lance", 8u}, "weapon.lance", {"weapon.lance.sounds"}, 11u, 12u},
    {{"heavy_bowgun", 9u}, "weapon.heavy_bowgun", {"weapon.heavy_bowgun.sounds", "weapon.heavy_bowgun.placements"}, 13u,
        14u},
    {{"light_bowgun", 11u}, "weapon.light_bowgun", {"weapon.light_bowgun.sounds", "weapon.light_bowgun.placements"},
        15u, 16u},
    {{"long_sword", 12u}, "weapon.long_sword", {"weapon.long_sword.sounds"}, 17u, 18u},
    {{"switch_axe", 13u}, "weapon.switch_axe", {"weapon.switch_axe.sounds"}, 19u, 20u},
    {{"gunlance", 14u}, "weapon.gunlance", {"weapon.gunlance.sounds"}, 21u, 22u},
    {{"bow", 15u}, "weapon.bow", {"weapon.bow.sounds"}, 23u, 24u},
    {{"dual_blades", 16u}, "weapon.dual_blades", {"weapon.dual_blades.sounds"}, 25u, 26u},
    {{"hunting_horn", 17u}, "weapon.hunting_horn", {"weapon.hunting_horn.sounds"}, 27u, 28u},
}};
constexpr std::array<WeaponClass, 12> kWeaponClasses{{kClasses[0].weapon, kClasses[1].weapon, kClasses[2].weapon,
    kClasses[3].weapon, kClasses[4].weapon, kClasses[5].weapon, kClasses[6].weapon, kClasses[7].weapon,
    kClasses[8].weapon, kClasses[9].weapon, kClasses[10].weapon, kClasses[11].weapon}};

struct Part {
    ArmorPart armor;
    std::string_view table;
    std::uint32_t names{}, descriptions{};
};
constexpr std::array<Part, 5> kParts{{
    {{"head", 4u}, "armor.head", 29u, 30u},
    {{"chest", 0u}, "armor.chest", 31u, 32u},
    {{"arms", 1u}, "armor.arms", 33u, 34u},
    {{"waist", 2u}, "armor.waist", 35u, 36u},
    {{"legs", 3u}, "armor.legs", 37u, 38u},
}};
constexpr std::array<ArmorPart, 5> kArmorParts{
    {kParts[0].armor, kParts[1].armor, kParts[2].armor, kParts[3].armor, kParts[4].armor}};

// Fields of an armor record (docs/EQUIPMENT_MODS.md).
constexpr std::uint32_t kMaleModelField = 0x00u;
constexpr std::uint32_t kFemaleModelField = 0x02u;
constexpr std::uint32_t kArmorPriceField = 0x08u;
constexpr std::uint32_t kArmorDescriptionField = 0x1Au;
// The game turns an armor model into its file by adding the base for the
// hunter's sex and part (u16 [sex][part], seven parts per sex, at 0x089E892C)
// and keeping 16 bits, in three places: the hunter's file function
// (0x088A58DC: the base in v1 at 0x088A5968, the sum at 0x088A5974), the
// character select's hunters' (0x088F46D8) and a hunter class of lobby_task's
// own (0x0A10EE8C). Nothing bounds the model.
constexpr std::uint32_t kArmorFile = 0x088A5968u; // lw ra, 8(sp)
constexpr std::uint32_t kArmorFileWord = 0x8FBF0008u;
constexpr std::uint32_t kArmorAdd = 0x088A5974u; // addu v0, v0, v1
constexpr std::uint32_t kArmorAddWord = 0x00431021u;
constexpr std::uint32_t kFileBases = 0x089E892Cu;
constexpr std::uint32_t kBasesPerSex = 7u;

// Fields of a weapon record (docs/EQUIPMENT_MODS.md).
constexpr std::uint32_t kModelField = 0x00u;
constexpr std::uint32_t kPriceField = 0x04u;
constexpr std::uint32_t kDescriptionField = 0x18u;

// The class's model count, u16 every 4 bytes by class (kind - 5).
constexpr std::uint32_t kModelCounts = 0x089CD1F8u;
// The model-to-file code, the same in the three functions that read these
// counts: a1 the model and v1 the class's count where it clamps the model
// below the count, then a0 the model plus the class's first file where it
// keeps 16 bits of the sum, the class in a saved register throughout.
struct WeaponFileCode {
    const char *overlay; // nullptr: the executable
    std::uint32_t clamp, clamp_word, file, file_word;
    std::uint32_t cls; // the register holding the class
};
constexpr std::array<WeaponFileCode, 3> kWeaponFileCode{{
    // The hunter's file function (0x088A58DC), on a quest and in the village.
    {nullptr, 0x088A59E4u, 0x00A3102Bu, 0x088A5A08u, 0x3082FFFFu, 16u}, // sltu v0, a1, v1; andi v0, a0, 0xFFFF
    // The character select's hunters' (0x088F46D8).
    {nullptr, 0x088F4754u, 0x00A3102Bu, 0x088F4780u, 0x3084FFFFu, 16u}, // sltu v0, a1, v1; andi a0, a0, 0xFFFF
    // A hunter class of lobby_task's own (0x0A10EE8C).
    {"lobby_task", 0x0A10EF28u, 0x00A3102Bu, 0x0A10EF54u, 0x3084FFFFu, 17u},
}};

struct Weapon {
    std::string id;
    const Class *cls{};
    std::uint16_t number{}; // its id in its class
    std::uint16_t model{};
    FileId file{};
    bool give{};
};

struct Armor {
    std::string id;
    const Part *part{};
    std::uint16_t number{};
};

// A piece to put in the equipment box once the village loads.
struct Gift {
    std::string id;
    std::uint8_t kind{};
    std::uint16_t number{};
    bool given{};
};

struct State {
    std::vector<Weapon> weapons;
    std::vector<Armor> armors;
    std::vector<Gift> gifts;
    std::map<std::uint32_t, std::uint16_t> armor_added; // rows each part got, by kind
    std::map<std::uint32_t, std::uint16_t> added;       // rows each class got, by kind
    std::map<std::uint32_t, std::uint16_t> models;      // models each class got, by kind
    ymod_mod *host{};
    Runtime *runtime{};
    bool hooked{};
};

State &state() {
    static State s;
    return s;
}

const Weapon *mod_model(std::uint32_t cls, std::uint32_t model) {
    for (const Weapon &w : state().weapons)
        if (w.cls->weapon.kind - 5u == cls && w.model == model) return &w;
    return nullptr;
}

// Whether the word at `address` is `word`: an overlay's code is checked when
// it runs, as it is not there when the hooks go in.
bool code_is(std::uint32_t address, std::uint32_t word) {
    const Runtime *runtime = state().runtime;
    return runtime != nullptr && runtime->memory().contains(address, 4u) && runtime->memory().load32(address) == word;
}

// A mod's model number is past the class's count: let it through.
int unclamp(ymod_cpu *cpu, void *user) {
    const auto *code = static_cast<const WeaponFileCode *>(user);
    if (code_is(code->clamp, code->clamp_word) && mod_model(cpu->gpr[code->cls], cpu->gpr[5] & 0xFFFFu) != nullptr)
        cpu->gpr[3] = 0xFFFFu; // v1
    return YMOD_CONTINUE;
}

// And its file is the mod's, not the class's first plus the number.
int give_file(ymod_cpu *cpu, void *user) {
    const auto *code = static_cast<const WeaponFileCode *>(user);
    if (!code_is(code->file, code->file_word)) return YMOD_CONTINUE;
    if (const Weapon *w = mod_model(cpu->gpr[code->cls], cpu->gpr[5] & 0xFFFFu)) cpu->gpr[4] = w->file; // a0
    return YMOD_CONTINUE;
}

// The game's armor model-to-file code as traced, with the table of bases there.
bool armor_code_as_traced(const psprecomp::GuestMemory &memory) {
    return memory.contains(kArmorFile, 4u) && memory.load32(kArmorFile) == kArmorFileWord &&
        memory.contains(kArmorAdd, 4u) && memory.load32(kArmorAdd) == kArmorAddWord &&
        memory.contains(kFileBases, 2u * 2u * kBasesPerSex);
}

void each_frame(int, std::uint32_t, void *) {
    if (state().runtime != nullptr) frame(*state().runtime);
}

bool hook(Runtime &runtime) {
    State &s = state();
    if (s.hooked) return true;
    s.runtime = &runtime;
    // The executable's code is checked now; lobby_task's when it runs.
    for (const WeaponFileCode &code : kWeaponFileCode)
        if (code.overlay == nullptr && (!code_is(code.clamp, code.clamp_word) || !code_is(code.file, code.file_word)))
            return false;
    native::attach(runtime);
    if (s.host == nullptr) s.host = native::add_mod("yakumo.equipment", "Equipment from mods", "");
    const auto *hooks = static_cast<const ymod_hooks *>(native::host()->service(s.host, YMOD_HOOKS, 1u));
    const auto *events = static_cast<const ymod_events *>(native::host()->service(s.host, YMOD_EVENTS, 1u));
    s.hooked = hooks != nullptr && events != nullptr;
    for (const WeaponFileCode &code : kWeaponFileCode) {
        void *user = const_cast<WeaponFileCode *>(&code);
        s.hooked = s.hooked && hooks->instruction(s.host, code.overlay, code.clamp, &unclamp, user) != 0u &&
            hooks->instruction(s.host, code.overlay, code.file, &give_file, user) != 0u;
    }
    s.hooked = s.hooked && events->subscribe(s.host, YMOD_EVENT_FRAME, &each_frame, nullptr) == 0;
    return s.hooked;
}

const Class *class_of(std::string_view id) {
    for (const Class &c : kClasses)
        if (c.weapon.id == id) return &c;
    return nullptr;
}

} // namespace

std::span<const WeaponClass> weapon_classes() {
    return kWeaponClasses;
}

std::span<const ArmorPart> armor_parts() {
    return kArmorParts;
}

std::optional<std::uint16_t> add_armor(Runtime &runtime, const ArmorSpec &spec, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) -> std::optional<std::uint16_t> {
        if (why != nullptr) *why = std::move(reason);
        return std::nullopt;
    };
    const auto found =
        std::find_if(kParts.begin(), kParts.end(), [&](const Part &p) { return p.armor.id == spec.part; });
    if (found == kParts.end()) return fail("\"" + spec.part + "\" is not an armor part");
    const Part *part = &*found;
    if (spec.id.empty() ||
        std::any_of(s.armors.begin(), s.armors.end(), [&](const Armor &a) { return a.id == spec.id; }))
        return fail("the id \"" + spec.id + "\" is taken or empty");
    if (spec.model_male.empty()) return fail("it has no model");
    const tables::Table *table = tables::find(part->table);
    if (table == nullptr) return fail("the part's table is not known");
    if (spec.based_on == 0u || spec.based_on >= table->rows)
        return fail("the base " + std::to_string(spec.based_on) + " is not one of the part's pieces");
    if (!armor_code_as_traced(runtime.memory())) return fail("the game's armor model-to-file code is not as traced");
    if (!hook(runtime)) return fail("the game's model-to-file code is not as traced");

    const auto number = static_cast<std::uint16_t>(table->rows + s.armor_added[part->armor.kind]);
    std::string reason;
    const std::optional<std::uint32_t> at = tables::extend(runtime, part->table, number + 1u, &reason);
    if (!at) return fail("the part's table cannot grow: " + reason);
    const FileId male = added::free_id();
    if (male == 0u || !added::add(runtime, male, spec.model_male)) return fail("its model cannot be served");
    FileId female = male;
    if (!spec.model_female.empty()) {
        female = added::free_id();
        if (female == 0u || !added::add(runtime, female, spec.model_female)) return fail("its model cannot be served");
    }
    ++s.armor_added[part->armor.kind];

    // The record: the base's, with its own models, price and description.
    auto &memory = runtime.memory();
    const std::uint32_t base = *at + spec.based_on * table->row;
    const std::uint32_t row = *at + number * table->row;
    for (std::uint32_t i = 0; i < table->row; i += 4u) memory.store32(row + i, memory.load32(base + i));
    // Its models: for each sex, the number whose sum with the part's base is
    // the mod's file, so every place the game adds them loads it. Past the
    // game's models (files from 0xC000, bases below 0x600), so never 0, which
    // is the part bare; the game only compares models past its own.
    const auto model_for = [&memory, part](FileId file, std::uint32_t sex) {
        return static_cast<std::uint16_t>(
            file - memory.load16(kFileBases + 2u * (sex * kBasesPerSex + part->armor.kind)));
    };
    memory.store16(row + kMaleModelField, model_for(male, 0u));
    memory.store16(row + kFemaleModelField, model_for(female, 1u));
    if (spec.price != 0u) memory.store32(row + kArmorPriceField, spec.price);
    s.armors.push_back({spec.id, part, number});
    if (spec.give) s.gifts.push_back({spec.id, part->armor.kind, number});

    const std::string label = spec.mod + "'s " + spec.id;
    texts::add(part->names, spec.name, [label, number](std::uint32_t index) {
        if (index != number)
            std::cerr << "[mods] " << label << ": its name is at " << index << ", not at its id " << number << "\n";
    });
    const std::string_view table_name = part->table;
    texts::add(part->descriptions, spec.description, [table_name, number, &runtime](std::uint32_t index) {
        const tables::Table *t = tables::find(table_name);
        const std::optional<tables::Placement> now = tables::placement(table_name);
        if (t != nullptr && now && number < now->rows)
            runtime.memory().store16(
                now->address + number * t->row + kArmorDescriptionField, static_cast<std::uint16_t>(index));
    });
    std::cout << "[mods] " << spec.mod << " adds " << part->armor.id << " " << number << " (" << spec.name << ")\n";
    return number;
}

std::optional<std::uint16_t> add_weapon(Runtime &runtime, const WeaponSpec &spec, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) -> std::optional<std::uint16_t> {
        if (why != nullptr) *why = std::move(reason);
        return std::nullopt;
    };
    const Class *cls = class_of(spec.kind);
    if (cls == nullptr) return fail("\"" + spec.kind + "\" is not a weapon class");
    if (spec.id.empty() ||
        std::any_of(s.weapons.begin(), s.weapons.end(), [&](const Weapon &w) { return w.id == spec.id; }))
        return fail("the id \"" + spec.id + "\" is taken or empty");
    if (spec.model.empty()) return fail("it has no model");
    const tables::Table *table = tables::find(cls->table);
    std::vector<const tables::Table *> by_model;
    for (const std::string_view name : cls->by_model)
        if (!name.empty()) by_model.push_back(tables::find(name));
    if (table == nullptr || std::count(by_model.begin(), by_model.end(), nullptr) != 0)
        return fail("the class's table is not known");
    if (spec.based_on == 0u || spec.based_on >= table->rows)
        return fail("the base " + std::to_string(spec.based_on) + " is not one of the class's weapons");
    auto &memory = runtime.memory();
    const std::uint32_t cls_index = cls->weapon.kind - 5u;
    if (!memory.contains(kModelCounts + 4u * cls_index, 2u)) return fail("the model counts cannot be read");
    if (!hook(runtime)) return fail("the game's model-to-file code is not as traced");

    const std::uint16_t number = static_cast<std::uint16_t>(table->rows + s.added[cls->weapon.kind]);
    std::string reason;
    const std::optional<std::uint32_t> at = tables::extend(runtime, cls->table, number + 1u, &reason);
    if (!at) return fail("the class's table cannot grow: " + reason);
    // The record: the base's, with its own model, price and, once the text
    // block is loaded, description.
    const std::uint32_t base = *at + spec.based_on * table->row;
    const std::uint32_t row = *at + number * table->row;
    const std::uint16_t base_model = memory.load16(base + kModelField);
    const auto model =
        static_cast<std::uint16_t>(memory.load16(kModelCounts + 4u * cls_index) + s.models[cls->weapon.kind]);
    // The rows the game reads by model, the base's at its model: its sound
    // banks (a quest loads the banks of the hunter's weapon by model with no
    // bound, and a model past the class's rows took file ids from the bytes
    // after them) and a bowgun's placement.
    std::vector<std::uint32_t> placed;
    for (const tables::Table *t : by_model) {
        if (base_model >= t->rows) return fail("the base's model has no row in " + t->name);
        const std::optional<std::uint32_t> at_model = tables::extend(runtime, t->name, model + 1u, &reason);
        if (!at_model) return fail(t->name + " cannot grow: " + reason);
        placed.push_back(*at_model);
    }
    const FileId file = added::free_id();
    if (file == 0u || !added::add(runtime, file, spec.model)) return fail("its model cannot be served");
    ++s.added[cls->weapon.kind];
    ++s.models[cls->weapon.kind];
    for (std::size_t t = 0; t < by_model.size(); ++t)
        for (std::uint32_t i = 0; i < by_model[t]->row; ++i)
            memory.store8(
                placed[t] + model * by_model[t]->row + i, memory.load8(placed[t] + base_model * by_model[t]->row + i));

    for (std::uint32_t i = 0; i < table->row; i += 4u) memory.store32(row + i, memory.load32(base + i));
    memory.store16(row + kModelField, model);
    if (spec.price != 0u) memory.store32(row + kPriceField, spec.price);
    s.weapons.push_back({spec.id, cls, number, model, file, spec.give});
    if (spec.give) s.gifts.push_back({spec.id, cls->weapon.kind, number});

    const std::string label = spec.mod + "'s " + spec.id;
    texts::add(cls->names, spec.name, [label, number](std::uint32_t index) {
        if (index != number)
            std::cerr << "[mods] " << label << ": its name is at " << index << ", not at its id " << number << "\n";
    });
    const std::string_view table_name = cls->table;
    texts::add(cls->descriptions, spec.description, [table_name, number, &runtime](std::uint32_t index) {
        const tables::Table *t = tables::find(table_name);
        const std::optional<tables::Placement> now = tables::placement(table_name);
        if (t != nullptr && now && number < now->rows)
            runtime.memory().store16(
                now->address + number * t->row + kDescriptionField, static_cast<std::uint16_t>(index));
    });
    std::cout << "[mods] " << spec.mod << " adds " << cls->weapon.id << " " << number << " (" << spec.name << ")\n";
    return number;
}

void frame(Runtime &runtime) {
    State &s = state();
    texts::frame(runtime);
    if (std::none_of(s.gifts.begin(), s.gifts.end(), [](const Gift &g) { return !g.given; })) return;
    // In the village (lobby_task loaded) with a character: the box is the save's.
    const std::optional<LoadedOverlay> overlay = overlay_at(runtime.memory(), 0x0A05E600u);
    game::GuestRam ram(runtime.memory());
    if (!overlay || !iequals(overlay->name, "lobby_task") || !game::character_loaded(ram)) return;
    auto &memory = runtime.memory();
    for (Gift &w : s.gifts) {
        if (w.given) continue;
        std::optional<std::uint32_t> empty;
        bool owned = false;
        for (std::uint32_t slot = 0; slot < game::kEquipmentBoxSlots && !owned; ++slot) {
            const std::uint32_t at = game::kEquipmentBox + slot * game::kEquipmentRecord;
            if (memory.load8(at) == 0u) {
                if (!empty) empty = at;
                continue;
            }
            owned = memory.load8(at + 1u) == w.kind && memory.load16(at + 2u) == w.number;
        }
        if (!owned && empty) {
            for (std::uint32_t i = 0; i < game::kEquipmentRecord; i += 4u) memory.store32(*empty + i, 0u);
            memory.store8(*empty, 1u);
            memory.store8(*empty + 1u, w.kind);
            memory.store16(*empty + 2u, w.number);
            std::cout << "[mods] " << w.id << " is put in the equipment box, slot "
                      << (*empty - game::kEquipmentBox) / game::kEquipmentRecord << "\n";
        }
        w.given = owned || empty.has_value();
    }
}

void reset_for_tests() {
    state() = {};
    texts::reset_for_tests();
}

} // namespace mhp3rd::mods::equipment
