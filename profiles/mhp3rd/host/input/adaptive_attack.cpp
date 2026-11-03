#include "input/adaptive_attack.hpp"

#include <algorithm>
#include <cmath>

namespace mhp3rd::input::touch {

bool weapon_profile(AttackProfile profile) {
    return profile == AttackProfile::LongSword || profile == AttackProfile::GreatSword ||
        profile == AttackProfile::SwordAndShield || profile == AttackProfile::DualBlades ||
        profile == AttackProfile::Hammer || profile == AttackProfile::Bow;
}
bool ranged_profile(AttackProfile profile) {
    return profile == AttackProfile::Bow;
}
AttackProfile secondary_profile(AttackProfile profile) {
    using P = AttackProfile;
    switch (profile) {
    case P::SwordAndShield:
        return P::Guard;
    case P::GreatSword:
        return P::GreatSwordGuard;
    case P::Hammer:
        return P::HammerCharge;
    case P::DualBlades:
        return P::DemonToggle;
    case P::Bow:
        return P::Aim;
    default:
        return P::Off;
    }
}
namespace {
bool guard_selector(AttackProfile p) {
    return p == AttackProfile::Guard;
}
bool captured_r(AttackProfile p) {
    return p == AttackProfile::HammerCharge || p == AttackProfile::DemonToggle || p == AttackProfile::GreatSwordGuard ||
        p == AttackProfile::Aim;
}
}
std::uint8_t gesture_directions(AttackProfile profile) {
    constexpr auto up = 1u << static_cast<unsigned>(AttackDirection::Up);
    constexpr auto down = 1u << static_cast<unsigned>(AttackDirection::Down);
    constexpr auto left = 1u << static_cast<unsigned>(AttackDirection::Left);
    constexpr auto right = 1u << static_cast<unsigned>(AttackDirection::Right);
    switch (profile) {
    case AttackProfile::LongSword:
    case AttackProfile::GreatSword:
    case AttackProfile::SwordAndShield:
        return up | down | left | right;
    case AttackProfile::DualBlades:
        return up | left | right;
    case AttackProfile::Hammer:
        return right;
    case AttackProfile::Bow:
        return up;
    case AttackProfile::Guard:
        return down | left | right;
    default:
        return 0;
    }
}

bool AdaptiveAttack::begin(std::uint64_t id, Point at, float radius, std::uint64_t ms, AttackProfile profile) {
    if ((profile >= AttackProfile::HuntingHorn && profile <= AttackProfile::HeavyBowgun) || owner_ ||
        !std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(radius) || radius <= 0.0f)
        return false;
    profile_ = profile;
    bow_hold_ = false;
    owner_ = id;
    origin_ = at;
    threshold_ = std::max(6.0f, radius * 0.42f);
    started_ = ms;
    selected_ = AttackDirection::Pending;
    pulse_ = AttackDirection::None;
    return true;
}

void AdaptiveAttack::move(std::uint64_t id, Point at, std::uint64_t ms) {
    if (owner_ != id) return;
    const float x = at.x - origin_.x;
    const float y = at.y - origin_.y;
    if (!std::isfinite(x) || !std::isfinite(y) || ms < started_) {
        clear();
        return;
    }
    // A Bow flick may add Circle without dropping an already-held Triangle.
    // This preserves native charge/Arc Shot handling instead of predicting it.
    if (profile_ == AttackProfile::Bow &&
        (selected_ == AttackDirection::Pending || selected_ == AttackDirection::Primary) && -y >= threshold_ &&
        -y > std::abs(x)) {
        selected_ = AttackDirection::Up;
        bow_hold_ = ms - started_ >= kHoldMs;
        return;
    }
    if (selected_ != AttackDirection::Pending) return;
    // A held charge must not discharge when the finger drifts off its circle.
    // Mode switching is a button, not another attack selector.
    if (captured_r(profile_)) return;
    // Upward shield motion keeps guarding; evade has its own button. Do not
    // consume the contact, so the player can still choose a shield attack.
    if (profile_ == AttackProfile::Guard && -y >= threshold_ && -y >= std::abs(x)) return;
    // A long press has already selected the primary action. It cannot turn
    // into another attack on a later motion event.
    if (!guard_selector(profile_) && ms - started_ >= kHoldMs) {
        selected_ = AttackDirection::Primary;
    } else if (std::hypot(x, y) >= threshold_) {
        if (std::abs(x) > std::abs(y))
            selected_ = x < 0.0f ? AttackDirection::Left : AttackDirection::Right;
        else
            selected_ = y < 0.0f ? AttackDirection::Up : AttackDirection::Down;
    }
    if (selected_ != AttackDirection::Pending && !allowed(selected_))
        selected_ = guard_selector(profile_) ? AttackDirection::Pending : AttackDirection::Blocked;
}

void AdaptiveAttack::set_available(std::optional<std::uint8_t> mask) {
    available_ = mask;
}
bool AdaptiveAttack::allowed(AttackDirection direction) const {
    const bool flick = direction >= AttackDirection::Up && direction <= AttackDirection::Right;
    return direction != AttackDirection::Blocked &&
        (!flick || ((gesture_directions(profile_) >> static_cast<unsigned>(direction)) & 1u)) &&
        (profile_ != AttackProfile::LongSword || !available_ ||
            ((*available_ >> static_cast<unsigned>(direction)) & 1u));
}

void AdaptiveAttack::end(std::uint64_t id, std::uint64_t ms, bool canceled) {
    if (owner_ != id) return;
    const auto chosen = selected_ == AttackDirection::Pending ? AttackDirection::Primary : selected_;
    // Releasing a charged/guarded hold must release its button immediately.
    const bool release_hold = profile_ == AttackProfile::HammerCharge || profile_ == AttackProfile::GreatSwordGuard ||
        profile_ == AttackProfile::Aim ||
        ((profile_ == AttackProfile::Bow) && chosen == AttackDirection::Primary && ms >= started_ &&
            ms - started_ >= kHoldMs) ||
        (profile_ == AttackProfile::Bow && bow_hold_) ||
        (profile_ == AttackProfile::GreatSword &&
            (chosen == AttackDirection::Up ||
                (chosen == AttackDirection::Primary && ms >= started_ && ms - started_ >= kHoldMs))) ||
        (guard_selector(profile_) && ms >= started_ && ms - started_ >= kHoldMs);
    const bool valid = !canceled && ms >= started_ && !release_hold && allowed(chosen);
    clear();
    if (valid) {
        pulse_ = chosen;
        released_ = ms;
    }
}

void AdaptiveAttack::clear() {
    owner_.reset();
    selected_ = pulse_ = AttackDirection::None;
    bow_hold_ = false;
}

bool AdaptiveAttack::held() const {
    return owner_.has_value();
}

AttackDirection AdaptiveAttack::direction(std::uint64_t ms) const {
    if (owner_) {
        if (ms < started_) return AttackDirection::None;
        return selected_ == AttackDirection::Pending && ms - started_ >= kHoldMs ? AttackDirection::Primary : selected_;
    }
    return pulse_ != AttackDirection::None && ms >= released_ && ms - released_ < kPulseMs ? pulse_
                                                                                           : AttackDirection::None;
}

AttackOutput AdaptiveAttack::output(std::uint64_t ms) const {
    const auto d = direction(ms);
    if (profile_ == AttackProfile::Off || !allowed(d)) return {};
    if (captured_r(profile_)) return d == AttackDirection::None ? AttackOutput{} : AttackOutput{0x0200u, 0};
    if (profile_ == AttackProfile::Guard) {
        if (d == AttackDirection::None) return {};
        if (d == AttackDirection::Down) return {0x1200u, 0};
        if (d == AttackDirection::Left || d == AttackDirection::Right) return {0x2200u, 0};
        return {0x0200u, 0};
    }
    if (profile_ != AttackProfile::LongSword) {
        if (d == AttackDirection::None || d == AttackDirection::Pending) return {};
        if (d == AttackDirection::Primary) return {0x1000u, 0};
        switch (profile_) {
        case AttackProfile::Bow:
            return {static_cast<std::uint16_t>(d == AttackDirection::Up && !bow_hold_ ? 0x2000u : 0x3000u), 0};
        default:
            break;
        }
        if (profile_ == AttackProfile::GreatSword) {
            if (d == AttackDirection::Up) return {0x1000u, 0};
            if (d == AttackDirection::Down) return {0x3000u, 0};
            if (d == AttackDirection::Left) return {0x0001u, 0}; // native kick
            return {0x2000u, 0};
        }
        if (profile_ == AttackProfile::Hammer) return {0x2000u, 0}; // the only flick is the native side swing
        if (profile_ == AttackProfile::SwordAndShield) {
            if (d == AttackDirection::Right) return {0x0001u, 0};      // native spinning slash (Select)
            if (d == AttackDirection::Down) return {0x2000u, 0, true}; // neutral Circle: horizontal chain
            if (d == AttackDirection::Up) return {0x3000u, 0};
            // Keep Circle with the player's movement until forward shield input is verified.
            return {0x2000u, 0};
        }
        if (d == AttackDirection::Up) return {0x3000u, 0};
        return {0x2000u, profile_ == AttackProfile::DualBlades && d == AttackDirection::Left ? -1.f : 0.f};
    }
    switch (d) {
    case AttackDirection::Primary:
        return {0x1000u, 0.0f};
    case AttackDirection::Up:
        return {0x2000u, 0.0f};
    case AttackDirection::Down:
        return {0x3000u, 0.0f};
    case AttackDirection::Left:
        return {0x3000u, -1.0f};
    case AttackDirection::Right:
        return {0x3000u, 1.0f};
    default:
        return {};
    }
}
} // namespace mhp3rd::input::touch
