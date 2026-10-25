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
// - gives that model the base's sound banks in the class's table of banks by
//   model (moved with a row for it, weapon.<class>.sounds), which a quest
//   reads for the hunter's weapon with no bound;
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
// a table cannot grow, the base's model has no sound banks or the game's
// code is not as traced.
[[nodiscard]] std::optional<std::uint16_t> add_weapon(
    psprecomp::Runtime &runtime, const WeaponSpec &spec, std::string *why = nullptr);

// The armor parts, as mods name them ("head", "chest", "arms", "waist",
// "legs"), with the game's equipment kind.
struct ArmorPart {
    std::string_view id;
    std::uint8_t kind{};
};
[[nodiscard]] std::span<const ArmorPart> armor_parts();

struct ArmorSpec {
    std::string id;                         // the mod's name for it
    std::string part;                       // "head", "chest", "arms", "waist" or "legs"
    std::uint16_t based_on{};               // the game's piece of that part whose record it copies
    std::vector<std::uint8_t> model_male;   // its model file for a male hunter
    std::vector<std::uint8_t> model_female; // and a female one; empty: the male's
    std::string name, description;
    std::uint32_t price{}; // 0: the base's
    bool give{};           // put one in the equipment box when the village loads
    std::string mod;       // who adds it, for the log
};

// Adds an armor piece and returns its id in its part, or nothing (with the
// reason in `why`) when the part or base is unknown, the id taken, the model
// empty, the part's table cannot grow or the game's code is not as traced.
// Its record is the base's (defence, resistances, skills, slots, who may
// wear it) with its own models, price and description. Its files are served
// as files added from memory, and its model for each sex is the file less the
// base the game adds for that sex and part (0x089E892C), so each place that
// turns an armor model into its file loads the mod's: the hunter on a quest
// and in the village (0x088A58DC), the character select (0x088F46D8) and
// lobby_task's own hunter (0x0A10EE8C).
[[nodiscard]] std::optional<std::uint16_t> add_armor(
    psprecomp::Runtime &runtime, const ArmorSpec &spec, std::string *why = nullptr);

// Gives the weapons and armor asked for once the village has loaded; between frames.
void frame(psprecomp::Runtime &runtime);

// Forgets the weapons and armor, so tests can add again.
void reset_for_tests();

} // namespace mhp3rd::mods::equipment
