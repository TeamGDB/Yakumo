#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
namespace mhp3rd::game {
class Ram;
}
namespace mhp3rd::game::combat {
inline constexpr std::uint32_t kApplyDamage = 0x0A07BA2Cu, kDamageReturn = 0x0A025764u;
struct HitInput {
    std::uint32_t target{}, source{}, collision{}, return_pc{};
    unsigned part{};
};
struct HitProbe {
    std::uint32_t target{}, counter{};
    std::uint16_t before{};
    std::array<float, 3> position{};
};
// Unknown executable/caller/owner combinations never produce a player number.
[[nodiscard]] bool damage_code_supported(const Ram &ram);
[[nodiscard]] std::optional<HitProbe> begin_hit(const Ram &ram, const HitInput &input);
[[nodiscard]] int finish_hit(const Ram &ram, const HitProbe &probe);
[[nodiscard]] std::optional<std::array<float, 2>> project_damage(const Ram &ram, const std::array<float, 3> &world);
struct DamageNumber {
    std::uint32_t target{};
    int amount{};
    std::array<float, 3> position{};
    std::uint64_t born{};
    unsigned lane{};
};
class DamageHistory {
public:
    void add(const HitProbe &hit, int amount, std::uint64_t now);
    void expire(std::uint64_t now);
    void clear();
    [[nodiscard]] const std::vector<DamageNumber> &numbers() const;

private:
    std::vector<DamageNumber> numbers_;
    unsigned next_lane_{};
};
inline constexpr std::uint64_t kNumberLifetimeUs = 900000u;
[[nodiscard]] std::uint64_t damage_clock_us();
}
