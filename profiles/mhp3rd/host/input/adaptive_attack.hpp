#pragma once

// Independently implemented single-owner touch attack selection. The shared
// AdaptiveControls handles movement/camera/system buttons around it.
#include "input/touch_controls.hpp"
#include <cstdint>
#include <optional>

namespace mhp3rd::input::touch {

enum class AttackProfile : std::uint8_t {
    Off,
    LongSword,
    GreatSword,
    SwordAndShield,
    DualBlades,
    Hammer,
    // Deferred class labels are retained only for unsupported-weapon notices.
    HuntingHorn,
    Lance,
    Gunlance,
    SwitchAxe,
    LightBowgun,
    HeavyBowgun,
    Bow,
    Guard,
    HammerCharge,
    DemonToggle,
    GreatSwordGuard,
    Aim
};
[[nodiscard]] bool weapon_profile(AttackProfile profile);
[[nodiscard]] bool ranged_profile(AttackProfile profile);
[[nodiscard]] AttackProfile secondary_profile(AttackProfile profile);
enum class AttackDirection : std::uint8_t { None, Pending, Primary, Up, Down, Left, Right, Blocked };
// Configured flick routes, not a prediction of the game's current combo window.
[[nodiscard]] std::uint8_t gesture_directions(AttackProfile profile);
struct AttackOutput {
    std::uint16_t buttons{};
    float lateral{};         // optional full left/right analog deflection, never D-pad
    bool neutral_movement{}; // suppress touch movement only for this action
};

class AdaptiveAttack {
public:
    static constexpr std::uint64_t kHoldMs = 110u;
    static constexpr std::uint64_t kPulseMs = 85u;
    bool begin(
        std::uint64_t id, Point at, float radius, std::uint64_t ms, AttackProfile profile = AttackProfile::LongSword);
    void move(std::uint64_t id, Point at, std::uint64_t ms);
    void end(std::uint64_t id, std::uint64_t ms, bool canceled);
    void clear();
    void set_available(std::optional<std::uint8_t> mask);
    [[nodiscard]] bool held() const;
    [[nodiscard]] AttackDirection direction(std::uint64_t ms) const;
    [[nodiscard]] AttackOutput output(std::uint64_t ms) const;

private:
    [[nodiscard]] bool allowed(AttackDirection direction) const;
    std::optional<std::uint8_t> available_;
    AttackProfile profile_{AttackProfile::LongSword};
    std::optional<std::uint64_t> owner_;
    Point origin_{};
    float threshold_{};
    std::uint64_t started_{};
    AttackDirection selected_{AttackDirection::None};
    AttackDirection pulse_{AttackDirection::None};
    std::uint64_t released_{};
};
} // namespace mhp3rd::input::touch
