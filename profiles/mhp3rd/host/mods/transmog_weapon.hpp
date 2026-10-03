#pragma once

#include "mods/transmog.hpp"

namespace mhp3rd::mods::transmog::weapon {

struct ModelTable {
    std::uint8_t kind;
    std::uint8_t name_table;
    std::uint32_t address;
    std::uint32_t stride;
};
[[nodiscard]] std::span<const ModelTable> model_tables();
[[nodiscard]] std::optional<std::uint16_t> select_appearance(
    const game::Ram &ram, std::optional<WeaponAppearance> selection,
    std::size_t count, std::uint8_t equipped_kind);
using NamesFunction = std::vector<std::string> (*)(std::uint8_t kind);
using CountFunction = std::size_t (*)(std::uint8_t kind);
struct CatalogFunctions { NamesFunction names{}; CountFunction count{}; };
using SelectionFunction = const Set &(*)();
void install(psprecomp::Runtime &runtime, AppearanceFunction original, Store &settings,
             bool (*enabled)(), CatalogFunctions catalog, SelectionFunction selection, std::string_view executable_sha256);
[[nodiscard]] std::vector<Armor> weapon_list(std::uint8_t kind);
[[nodiscard]] std::optional<std::uint8_t> equipped_weapon_kind();
[[nodiscard]] std::uint16_t effective_model_for_actor(
    const game::Ram &ram, std::uint32_t actor, std::optional<WeaponAppearance> selection,
    std::size_t donor_count);
[[nodiscard]] std::uint16_t effective_resource_id_for_actor(
    const game::Ram &ram, std::uint32_t actor, std::optional<WeaponAppearance> selection,
    std::size_t donor_count);
[[nodiscard]] std::string status();

} // namespace mhp3rd::mods::transmog::weapon
