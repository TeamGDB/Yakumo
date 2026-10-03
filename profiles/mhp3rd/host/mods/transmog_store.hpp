#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mhp3rd::mods::transmog {

inline constexpr std::array<const char *, 5> kSlots{"Head", "Chest", "Arms", "Waist", "Legs"};
struct ArmorAppearance {
    std::uint16_t id{};
    std::uint8_t gender{};  // the donor model: 0 male, 1 female
    bool operator==(const ArmorAppearance &) const = default;
};
struct WeaponAppearance {
    std::uint16_t id{};
    std::uint8_t kind{};  // the game's equipment kind byte, not a sequential class index
    bool operator==(const WeaponAppearance &) const = default;
};
struct WeaponClass { std::uint8_t kind; const char *label; };
inline constexpr std::array<WeaponClass, 12> kWeaponClasses{{
    {5, "Great Sword"}, {12, "Long Sword"}, {6, "Sword and Shield"}, {16, "Dual Blades"},
    {7, "Hammer"}, {17, "Hunting Horn"}, {8, "Lance"}, {14, "Gunlance"},
    {13, "Switch Axe"}, {11, "Light Bowgun"}, {9, "Heavy Bowgun"}, {15, "Bow"}
}};
// Missing armor: equipped appearance. Armor id 0: the donor's unequipped model.
struct Set {
    std::array<std::optional<ArmorAppearance>, 5> armor{};
    std::optional<WeaponAppearance> weapon;
    bool operator==(const Set &) const = default;
};
using Presets = std::map<std::string, Set>;

class Store {
public:
    void initialize(const std::filesystem::path &path);
    [[nodiscard]] std::filesystem::path back_up_invalid_and_reset();
    [[nodiscard]] const Set &active() const noexcept { return active_; }
    [[nodiscard]] const Presets &presets() const noexcept { return presets_; }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }
    void set(std::size_t slot, std::optional<ArmorAppearance> armor);
    void set_weapon(std::optional<WeaponAppearance> weapon);
    void apply(Set active);
    void reset();
    // Saving an existing name deliberately replaces that preset.
    void save_preset(std::string_view name);
    void save_preset(std::string_view name, Set set);
    void load_preset(std::string_view name);
    void delete_preset(std::string_view name);
    [[nodiscard]] static bool name_character(char32_t character);
private:
    void commit(Set active, Presets presets);
    std::filesystem::path path_;
    Set active_{};
    Presets presets_;
    bool load_valid_{};
};

} // namespace mhp3rd::mods::transmog
