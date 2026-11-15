#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace mhp3rd::game {
class Ram;
}
namespace mhp3rd::game::combat {
// NPJB-40001's actor manager has twenty slots, including small monsters.
inline constexpr unsigned kMonsterSlots = 20u;
struct Monster {
    std::uint32_t address{};
    std::uint8_t kind{};
    int health{}, maximum{};
    std::array<float, 3> position{};
    bool in_area{};
    bool large{};
};
// Read-only validated snapshots; no game names or models are embedded here.
[[nodiscard]] bool large_monster_kind(std::uint8_t kind);
[[nodiscard]] std::vector<Monster> read_monsters(const Ram &ram);
// Living, present monsters only; large-monster slots come first in All mode.
[[nodiscard]] std::vector<Monster> visible_monsters(std::vector<Monster> monsters, bool large_only);
}
