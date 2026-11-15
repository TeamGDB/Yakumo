#include "game/monster_state.hpp"
#include "game/guest_ram.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>
namespace mhp3rd::game::combat {
bool large_monster_kind(std::uint8_t kind) {
    // NPJB-40001 species identities, not actor-manager slots or maximum HP.
    // Species ID facts cross-checked with the public monsters_mhp3rd table:
    // github.com/Alexander-Lancellott/MH-HP-Overlay-For-PSP-Emulator
    constexpr std::array<std::uint8_t, 40> large{1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 14, 15, 16, 18, 19, 20, 23, 24, 25, 32,
        40, 41, 42, 43, 44, 45, 46, 47, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62};
    return std::binary_search(large.begin(), large.end(), kind);
}
std::vector<Monster> read_monsters(const Ram &ram) {
    constexpr std::uint32_t task = 0x0A05E600u, table = 0x0A1B0AE0u;
    constexpr std::string_view name = "game_task.ovl";
    if (!ram.contains(task, 64u) || ram.load32(task) != 0x336F574Du || ram.load32(task + 8u) != task ||
        !ram.contains(table, kMonsterSlots * 4u))
        return {};
    for (std::size_t i = 0; i < name.size(); ++i)
        if (ram.load8(task + 32u + static_cast<std::uint32_t>(i)) != name[i]) return {};
    if (ram.load8(task + 32u + static_cast<std::uint32_t>(name.size())) != 0u) return {};
    std::vector<Monster> result;
    result.reserve(kMonsterSlots);
    for (unsigned slot = 0; slot < kMonsterSlots; ++slot) {
        const auto at = ram.load32(table + slot * 4u);
        if (at < 0x08800000u || at > 0x0C000000u - 0x28Cu || (at & 3u) || !ram.contains(at, 0x28Cu)) continue;
        if (std::any_of(result.begin(), result.end(), [at](const Monster &m) { return m.address == at; })) continue;
        const auto maximum = static_cast<std::int16_t>(ram.load16(at + 0x288u));
        const auto health = static_cast<std::int16_t>(ram.load16(at + 0x246u));
        const auto kind = ram.load8(at + 0x62u);
        if (maximum <= 0 || health > maximum || kind >= 75u) continue;
        std::array<float, 3> position{};
        for (unsigned i = 0; i < 3; ++i) position[i] = std::bit_cast<float>(ram.load32(at + 0x80u + i * 4u));
        if (!std::all_of(position.begin(), position.end(), [](float value) { return std::isfinite(value); })) continue;
        result.push_back({at, kind, std::max(0, static_cast<int>(health)), maximum, position,
            (ram.load32(at + 4u) & 4u) == 0u, large_monster_kind(kind)});
    }
    return result;
}
std::vector<Monster> visible_monsters(std::vector<Monster> monsters, bool large_only) {
    std::erase_if(
        monsters, [large_only](const Monster &m) { return !m.in_area || m.health <= 0 || (large_only && !m.large); });
    std::stable_sort(
        monsters.begin(), monsters.end(), [](const Monster &a, const Monster &b) { return a.large && !b.large; });
    return monsters;
}

}
