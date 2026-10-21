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
    std::uint32_t names{};  // its text tables: names by id, then descriptions
    std::uint32_t descriptions{};
};
// The name tables pair with the kinds as docs/DEBUG_MENU.md lists them.
constexpr std::array<Class, 12> kClasses{{
    {{"great_sword", 5u}, "weapon.great_sword", 5u, 6u},
    {{"sword_and_shield", 6u}, "weapon.sword_and_shield", 7u, 8u},
    {{"hammer", 7u}, "weapon.hammer", 9u, 10u},
    {{"lance", 8u}, "weapon.lance", 11u, 12u},
    {{"heavy_bowgun", 9u}, "weapon.heavy_bowgun", 13u, 14u},
    {{"light_bowgun", 11u}, "weapon.light_bowgun", 15u, 16u},
    {{"long_sword", 12u}, "weapon.long_sword", 17u, 18u},
    {{"switch_axe", 13u}, "weapon.switch_axe", 19u, 20u},
    {{"gunlance", 14u}, "weapon.gunlance", 21u, 22u},
    {{"bow", 15u}, "weapon.bow", 23u, 24u},
    {{"dual_blades", 16u}, "weapon.dual_blades", 25u, 26u},
    {{"hunting_horn", 17u}, "weapon.hunting_horn", 27u, 28u},
}};
constexpr std::array<WeaponClass, 12> kWeaponClasses{{kClasses[0].weapon, kClasses[1].weapon, kClasses[2].weapon,
    kClasses[3].weapon, kClasses[4].weapon, kClasses[5].weapon, kClasses[6].weapon, kClasses[7].weapon,
    kClasses[8].weapon, kClasses[9].weapon, kClasses[10].weapon, kClasses[11].weapon}};

// Fields of a weapon record (docs/EQUIPMENT_MODS.md).
constexpr std::uint32_t kModelField = 0x00u;
constexpr std::uint32_t kPriceField = 0x04u;
constexpr std::uint32_t kDescriptionField = 0x18u;

// The class's model count, u16 every 4 bytes by class (kind - 5).
constexpr std::uint32_t kModelCounts = 0x089CD1F8u;
// The model-to-file code: a1 the model, v1 the class's count, s0 the class;
// then a0 the model plus the class's first file.
constexpr std::uint32_t kClamp = 0x088A59E4u; // sltu v0, a1, v1
constexpr std::uint32_t kClampWord = 0x00A3102Bu;
constexpr std::uint32_t kFile = 0x088A5A08u; // andi v0, a0, 0xFFFF
constexpr std::uint32_t kFileWord = 0x3082FFFFu;

struct Weapon {
    std::string id;
    const Class *cls{};
    std::uint16_t number{}; // its id in its class
    std::uint16_t model{};
    FileId file{};
    bool give{};
    bool given{};
};

struct State {
    std::vector<Weapon> weapons;
    std::map<std::uint32_t, std::uint16_t> added;  // rows each class got, by kind
    std::map<std::uint32_t, std::uint16_t> models; // models each class got, by kind
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

// A mod's model number is past the class's count: let it through.
int unclamp(ymod_cpu *cpu, void *) {
    if (mod_model(cpu->gpr[16], cpu->gpr[5] & 0xFFFFu) != nullptr) cpu->gpr[3] = 0xFFFFu; // v1
    return YMOD_CONTINUE;
}

// And its file is the mod's, not the class's first plus the number.
int give_file(ymod_cpu *cpu, void *) {
    if (const Weapon *w = mod_model(cpu->gpr[16], cpu->gpr[5] & 0xFFFFu)) cpu->gpr[4] = w->file; // a0
    return YMOD_CONTINUE;
}

void each_frame(int, std::uint32_t, void *) {
    if (state().runtime != nullptr) frame(*state().runtime);
}

bool hook(Runtime &runtime) {
    State &s = state();
    if (s.hooked) return true;
    s.runtime = &runtime;
    if (runtime.memory().load32(kClamp) != kClampWord || runtime.memory().load32(kFile) != kFileWord) return false;
    native::attach(runtime);
    if (s.host == nullptr) s.host = native::add_mod("yakumo.equipment", "Equipment from mods", "");
    const auto *hooks = static_cast<const ymod_hooks *>(native::host()->service(s.host, YMOD_HOOKS, 1u));
    const auto *events = static_cast<const ymod_events *>(native::host()->service(s.host, YMOD_EVENTS, 1u));
    s.hooked = hooks != nullptr && events != nullptr &&
        hooks->instruction(s.host, nullptr, kClamp, &unclamp, nullptr) != 0u &&
        hooks->instruction(s.host, nullptr, kFile, &give_file, nullptr) != 0u &&
        events->subscribe(s.host, YMOD_EVENT_FRAME, &each_frame, nullptr) == 0;
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
    if (table == nullptr) return fail("the class's table is not known");
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
    const FileId file = added::free_id();
    if (file == 0u || !added::add(runtime, file, spec.model)) return fail("its model cannot be served");
    ++s.added[cls->weapon.kind];

    // The record: the base's, with its own model, price and, once the text
    // block is loaded, description.
    const std::uint32_t base = *at + spec.based_on * table->row;
    const std::uint32_t row = *at + number * table->row;
    for (std::uint32_t i = 0; i < table->row; i += 4u) memory.store32(row + i, memory.load32(base + i));
    const auto model =
        static_cast<std::uint16_t>(memory.load16(kModelCounts + 4u * cls_index) + s.models[cls->weapon.kind]++);
    memory.store16(row + kModelField, model);
    if (spec.price != 0u) memory.store32(row + kPriceField, spec.price);
    s.weapons.push_back({spec.id, cls, number, model, file, spec.give});

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
    if (std::none_of(s.weapons.begin(), s.weapons.end(), [](const Weapon &w) { return w.give && !w.given; })) return;
    // In the village (lobby_task loaded) with a character: the box is the save's.
    const std::optional<LoadedOverlay> overlay = overlay_at(runtime.memory(), 0x0A05E600u);
    game::GuestRam ram(runtime.memory());
    if (!overlay || !iequals(overlay->name, "lobby_task") || !game::character_loaded(ram)) return;
    auto &memory = runtime.memory();
    for (Weapon &w : s.weapons) {
        if (!w.give || w.given) continue;
        std::optional<std::uint32_t> empty;
        bool owned = false;
        for (std::uint32_t slot = 0; slot < game::kEquipmentBoxSlots && !owned; ++slot) {
            const std::uint32_t at = game::kEquipmentBox + slot * game::kEquipmentRecord;
            if (memory.load8(at) == 0u) {
                if (!empty) empty = at;
                continue;
            }
            owned = memory.load8(at + 1u) == w.cls->weapon.kind && memory.load16(at + 2u) == w.number;
        }
        if (!owned && empty) {
            for (std::uint32_t i = 0; i < game::kEquipmentRecord; i += 4u) memory.store32(*empty + i, 0u);
            memory.store8(*empty, 1u);
            memory.store8(*empty + 1u, w.cls->weapon.kind);
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
