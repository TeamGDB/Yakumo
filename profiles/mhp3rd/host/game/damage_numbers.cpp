#include "game/damage_numbers.hpp"
#include "game/monster_state.hpp"
#include "game/control_context.hpp"
#include "game/weapon_state.hpp"
#include "game/guest_ram.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
namespace mhp3rd::game::combat {
namespace {
bool mapped(const Ram &ram, std::uint32_t at, std::size_t size) {
    return at >= 0x08800000u && at < 0x0C000000u && size <= 0x0C000000u - at && ram.contains(at, size);
}
float real(const Ram &ram, std::uint32_t at) {
    return std::bit_cast<float>(ram.load32(at));
}
}
bool damage_code_supported(const Ram &ram) {
    // Local trace facts: the original call keeps the collision owner in s4,
    // and the damage application accumulates actual per-part HP loss at +0x1EC.
    constexpr std::array<std::array<std::uint32_t, 2>, 6> signature{
        {{kApplyDamage, 0x27BDFFF0u}, {0x0A07BA54u, 0x0E81EE1Bu}, {0x0A07BA78u, 0xA46201ECu},
            {0x0A025748u, 0x92A50008u}, {0x0A025750u, 0x02602021u}, {0x0A02575Cu, 0x0E81EE8Bu}}};
    return std::all_of(signature.begin(), signature.end(),
        [&](const auto &word) { return mapped(ram, word[0], 4u) && ram.load32(word[0]) == word[1]; });
}
std::optional<HitProbe> begin_hit(const Ram &ram, const HitInput &input) {
    if (input.return_pc != kDamageReturn || input.part >= 8u || !damage_code_supported(ram) ||
        control_context::read(ram).interface_open)
        return std::nullopt;
    const auto hunter = weapon::hunter_actor(ram);
    if (!hunter || input.source != hunter || !mapped(ram, input.collision, 0x18u) ||
        ram.load32(input.collision + 0x14u) != hunter)
        return std::nullopt;
    const auto monsters = read_monsters(ram);
    const auto found = std::find_if(monsters.begin(), monsters.end(),
        [&](const Monster &m) { return m.address == input.target && m.in_area && m.health > 0; });
    if (found == monsters.end()) return std::nullopt;
    const auto counter = input.target + 0x1ECu + input.part * 2u;
    auto position = found->position;
    position[1] += 85.f;
    return HitProbe{input.target, counter, ram.load16(counter), position};
}
int finish_hit(const Ram &ram, const HitProbe &probe) {
    if (!mapped(ram, probe.counter, 2u)) return 0;
    // Modular subtraction handles a wrapped accumulation. A reset must not
    // become an implausibly large player hit.
    const auto amount = static_cast<std::uint16_t>(ram.load16(probe.counter) - probe.before);
    return amount > 0u && amount <= 32767u ? static_cast<int>(amount) : 0;
}
std::optional<std::array<float, 2>> project_damage(const Ram &ram, const std::array<float, 3> &world) {
    if (!std::all_of(world.begin(), world.end(), [](float f) { return std::isfinite(f); }) ||
        !mapped(ram, 0x08A2F958u, 4u))
        return std::nullopt;
    const auto camera = ram.load32(0x08A2F958u);
    if (!mapped(ram, camera, 0x1010u)) return std::nullopt;
    float eye[4]{}, clip[4]{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            eye[row] += real(ram, camera + 0xF50u + (col * 4u + row) * 4u) * (col == 3 ? 1.f : world[col]);
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            clip[row] += real(ram, camera + 0xFD0u + (col * 4u + row) * 4u) * eye[col];
    if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[3]) || clip[3] <= 1.f)
        return std::nullopt;
    const float x = (clip[0] / clip[3] + 1.f) * 0.5f, y = (1.f - clip[1] / clip[3]) * 0.5f;
    if (x < 0.f || x > 1.f || y < 0.f || y > 1.f) return std::nullopt;
    return std::array<float, 2>{x, y};
}
void DamageHistory::add(const HitProbe &hit, int amount, std::uint64_t now) {
    expire(now);
    if (amount <= 0 || amount > 32767 || !hit.target ||
        !std::all_of(hit.position.begin(), hit.position.end(), [](float f) { return std::isfinite(f); }))
        return;
    if (numbers_.size() == 48u) numbers_.erase(numbers_.begin());
    numbers_.push_back({hit.target, amount, hit.position, now, next_lane_++ % 5u});
}
void DamageHistory::expire(std::uint64_t now) {
    std::erase_if(numbers_,
        [now](const DamageNumber &number) { return now < number.born || now - number.born >= kNumberLifetimeUs; });
}
void DamageHistory::clear() {
    numbers_.clear();
    next_lane_ = 0u;
}
const std::vector<DamageNumber> &DamageHistory::numbers() const {
    return numbers_;
}
std::uint64_t damage_clock_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}
