#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Weapons mods add (the "yakumo.equipment" service): a new weapon of one of
// the twelve classes, starting from one of the game's of that class (its
// attack, element, sharpness, slots and rarity), with its own model, name,
// description and price. Built on the game tables (mhp3rd_tables.hpp), the
// files mods add and the strings mods add to the text block
// (mhp3rd_texts.hpp); docs/EQUIPMENT_MODS.md describes each step.
//
// For a weapon Yakumo:
//
// - makes the class's record table a row longer (the four getters that read
//   it, 0x0885CB38 to 0x0885D8C8) and copies the base's record into the row;
// - gives it a model number past the class's models (counts at 0x089CD1F8)
//   and serves the mod's model as a file added for that number, through two
//   instruction hooks in the model-to-file code (0x088A59E4, 0x088A5A08);
// - adds its name to the class's name table, at its id, and its description
//   to the description table, whose index the record keeps at +0x18;
// - when asked, puts one in the equipment box once the village loads.
namespace mhp3rd::mods::equipment {

// The weapon classes, as mods name them, with the game's equipment kind.
struct WeaponClass {
    std::string_view id;
    std::uint8_t kind{};
};
[[nodiscard]] std::span<const WeaponClass> weapon_classes();

struct WeaponSpec {
    std::string id;                  // the mod's name for it
    std::string kind;                // its class, by id ("great_sword")
    std::uint16_t based_on{};        // the game's weapon of that class whose record it copies
    std::vector<std::uint8_t> model; // its model pack (model, skeleton, textures)
    std::string name, description;
    std::uint32_t price{}; // 0: the base's
    bool give{};           // put one in the equipment box when the village loads
    std::string mod;       // who adds it, for the log
};

// Adds a weapon and returns its id in its class, or nothing (with the reason
// in `why`) when the class or base is unknown, the id taken, the model empty,
// a table cannot grow or the game's code is not as traced.
[[nodiscard]] std::optional<std::uint16_t> add_weapon(
    psprecomp::Runtime &runtime, const WeaponSpec &spec, std::string *why = nullptr);

// Gives the weapons asked for once the village has loaded; between frames.
void frame(psprecomp::Runtime &runtime);

// Forgets the weapons, so tests can add again.
void reset_for_tests();

} // namespace mhp3rd::mods::equipment
