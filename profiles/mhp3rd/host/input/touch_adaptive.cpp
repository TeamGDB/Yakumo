#include "input/touch_adaptive.hpp"
#include "input/chords.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace mhp3rd::input::touch {
namespace {
using E = AdaptiveElement;
constexpr std::size_t index(E e) {
    return static_cast<std::size_t>(e);
}
bool finite(Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
const AdaptiveInfo kInfo[] = {{"stick", "Movement Stick"}, {"attack", "Attack Selector"},
    {"alternate", "Alternate / Ammunition"}, {"special", "Special / Sprint"}, {"evade", "Evade"},
    {"interact", "Interact / Sheathe"}, {"items", "Items / HUD"}, {"dpad", "D-Pad"}, {"dpad_toggle", "D-Pad Toggle"},
    {"r3", "R3 / Lock-On"}, {"pause", "Commands"}, {"start", "Game Menu"}, {"select", "Select"},
    {"left_shoulder", "L / Previous Page"}, {"interface_toggle", "Interface / Auto"}, {"left_fire", "Left Fire Button"},
    {"yakumo_menu", "Yakumo Menu"}};
}
Placement &AdaptiveLayout::at(E e) {
    return elements[index(e)];
}
const Placement &AdaptiveLayout::at(E e) const {
    return elements[index(e)];
}
const AdaptiveLayout &default_adaptive_layout(AdaptiveMode mode) {
    static const AdaptiveLayout value = [] {
        AdaptiveLayout l;
        const auto set = [&](E e, Anchor a, float x, float y, float r, std::uint16_t bits) {
            l.at(e) = {a, x, y, r, bits, true};
        };
        set(E::Stick, Anchor::Left, .22f, .78f, .115f, 0);
        set(E::Attack, Anchor::Right, .28f, .65f, .105f, 0x1000);
        set(E::Alternate, Anchor::Right, .38f, .39f, .050f, 0x2000);
        set(E::Special, Anchor::Right, .13f, .43f, .065f, 0x0200);
        set(E::Evade, Anchor::Right, .13f, .84f, .067f, 0x4000);
        set(E::Interact, Anchor::Right, .48f, .82f, .058f, 0x8000);
        set(E::ItemBar, Anchor::Centre, 0, .93f, .045f, 0);
        set(E::DPad, Anchor::Left, .20f, .47f, .090f, 0);
        set(E::DpadToggle, Anchor::Left, .055f, .34f, .030f, 0);
        set(E::R3, Anchor::Right, .055f, .955f, .029f, 0);
        set(E::Pause, Anchor::Right, .055f, .08f, .030f, 0);
        set(E::Start, Anchor::Right, .15f, .08f, .027f, 0x0008);
        set(E::Select, Anchor::Right, .24f, .08f, .023f, 0x0001);
        set(E::LeftShoulder, Anchor::Left, .15f, .15f, .050f, 0x0100);
        set(E::InterfaceToggle, Anchor::Left, .055f, .08f, .030f, 0);
        set(E::LeftFire, Anchor::Left, .15f, .22f, .055f, 0);
        set(E::YakumoMenu, Anchor::Right, .055f, .08f, .030f, 0);
        l.at(E::YakumoMenu).shown = false;
        l.at(E::LeftShoulder).shown = false;
        l.at(E::Select).shown = false;
        return l;
    }();
    static const AdaptiveLayout hub = [&] {
        auto l = value;
        l.at(E::Attack).shown = l.at(E::Evade).shown = l.at(E::ItemBar).shown = l.at(E::R3).shown = false;
        l.at(E::Alternate) = {Anchor::Right, .28f, .65f, .085f, 0x2000, true};
        return l;
    }();
    static const AdaptiveLayout interface = [&] {
        auto l = value;
        l.at(E::Stick).shown = l.at(E::ItemBar).shown = l.at(E::R3).shown = l.at(E::DpadToggle).shown = false;
        l.at(E::Attack) = {Anchor::Right, .23f, .49f, .060f, 0x1000, true};
        l.at(E::Alternate) = {Anchor::Right, .10f, .65f, .060f, 0x2000, true};
        l.at(E::Evade) = {Anchor::Right, .23f, .81f, .060f, 0x4000, true};
        l.at(E::Interact) = {Anchor::Right, .36f, .65f, .060f, 0x8000, true};
        l.at(E::Special) = {Anchor::Right, .15f, .26f, .050f, 0x0200, true};
        l.at(E::LeftShoulder).shown = l.at(E::Select).shown = true;
        l.at(E::LeftShoulder).y = .26f;
        l.at(E::DPad) = {Anchor::Left, .22f, .66f, .115f, 0, true};
        return l;
    }();
    return mode == AdaptiveMode::Hub ? hub : mode == AdaptiveMode::Interface ? interface : value;
}
const AdaptiveInfo &info(E e) {
    return kInfo[index(e)];
}
bool rebindable(E e) {
    return e == E::Attack || e == E::Alternate || e == E::Special || e == E::Evade || e == E::Interact ||
        e == E::Start || e == E::Select || e == E::LeftShoulder;
}
bool parse_adaptive_placement(E e, std::string_view text, Placement &out) {
    Placement p;
    if (!parse(text, p)) return false;
    if (!rebindable(e)) p.buttons = 0;
    if (e == E::Pause) p.shown = true;
    out = p;
    return true;
}
Placed place(const Placement &p, E e, const Area &a, float scale, AttackProfile profile) {
    auto sized = p;
    if (e == E::Special && profile == AttackProfile::SwordAndShield && p.buttons == 0x0200) sized.size *= 1.35f;
    return place(sized, a, scale, e == E::ItemBar ? kItemBarAspect : 1.0f);
}
float adaptive_hit_radius(const Placed &p, const Area &a) {
    // Extra forgiveness for small controls, bounded so custom layouts remain usable.
    return std::max(p.radius * 1.15f, std::min(p.radius * 1.3f, a.height * .04f));
}
Placement move_to(const Placement &p, E e, Point c, const Area &a) {
    return move_to(p, c, a, e == E::ItemBar);
}
void AdaptiveControls::set_bow_toggle(bool enabled) {
    if (bow_toggle_ == enabled) return;
    release_all();
    fire_phase_ = FirePhase::Idle;
    bow_toggle_ = enabled;
}
bool AdaptiveControls::bow_toggle_enabled() const {
    return bow_toggle_;
}
bool AdaptiveControls::bow_toggle_available() const {
    return bow_toggle_ && mode_ == AdaptiveMode::Quest && posture_ == WeaponPosture::Drawn &&
        profile_ == AttackProfile::Bow && layout_.at(E::Attack).buttons == 0x1000 &&
        layout_.at(E::Special).buttons == 0x200;
}
bool AdaptiveControls::bow_aiming() const {
    return aiming_ && bow_toggle_available();
}
std::uint16_t AdaptiveControls::fire_buttons() const {
    if (!bow_toggle_available()) return 0;
    if (fire_phase_ == FirePhase::Cancel) return 0x5200;
    const bool firing = fire_phase_ == FirePhase::Held || fire_phase_ == FirePhase::Tap;
    return static_cast<std::uint16_t>(
        (bow_aiming() ? 0x200 : 0) | (firing ? 0x1000 : 0) | (firing && fire_arc_ ? 0x2000 : 0));
}
std::uint16_t AdaptiveControls::sample_fire_buttons(bool advance) {
    const auto bits = fire_buttons();
    if (advance && (fire_phase_ == FirePhase::Held || fire_phase_ == FirePhase::Tap)) {
        fire_sampled_ = true;
        if (fire_arc_) arc_sampled_ = true;
    }
    if (advance && (fire_phase_ == FirePhase::Tap || fire_phase_ == FirePhase::Cancel) && ++fire_reads_ >= 3)
        fire_phase_ = FirePhase::Idle;
    return bits;
}
void AdaptiveControls::cancel_fire(bool leave_aim) {
    if (fire_phase_ == FirePhase::Held || fire_phase_ == FirePhase::Tap) {
        fire_phase_ = fire_sampled_ ? FirePhase::Cancel : FirePhase::Idle;
        fire_reads_ = 0;
    }
    fire_arc_ = arc_sampled_ = false;
    for (auto &f : fingers_)
        if (f.role == Role::Fire) f.role = Role::None;
    if (leave_aim) aiming_ = false;
}
void AdaptiveControls::set_compact(bool enabled) {
    if (compact_ == enabled) return;
    release_all();
    compact_ = enabled;
}
bool AdaptiveControls::compact() const {
    return compact_;
}
bool AdaptiveControls::commands_open() const {
    return commands_;
}
AttackProfile AdaptiveControls::weapon_profile() const {
    return profile_;
}
AttackDirection AdaptiveControls::special_direction(std::uint64_t ms) const {
    return special_.direction(ms);
}
void AdaptiveControls::place_commands() {
    if (!commands_) return;
    constexpr E entries[] = {E::YakumoMenu, E::Start, E::Select, E::LeftShoulder, E::InterfaceToggle};
    // Aim for a 44px target even when the collapsed menu symbol is small.
    // Tiny windows still fit the full panel and its labels inside the safe area.
    const float max_radius = std::min(area_.width / 10.f, area_.height / 7.f);
    const float r = std::max(1.f, std::min(std::max(22.f, placed_[index(E::Pause)].radius), max_radius));
    const float step = r * 3.3f;
    const float left = std::clamp(placed_[index(E::Pause)].centre.x - step, area_.left + r * 1.5f,
        std::max(area_.left + r * 1.5f, area_.left + area_.width - step * 2 - r * 1.5f));
    const float top = std::clamp(placed_[index(E::Pause)].centre.y + step, area_.top + r * 1.5f,
        std::max(area_.top + r * 1.5f, area_.top + area_.height - step - r * 2.f));
    for (unsigned i = 0; i < 5; ++i) placed_[index(entries[i])] = {{left + (i % 3) * step, top + (i / 3) * step}, r, r};
}
void AdaptiveControls::set_mode(AdaptiveMode mode) {
    if (mode_ == mode) return;
    release_all();
    mode_ = mode;
    fire_phase_ = FirePhase::Idle;
    set_layout(layout_, area_, scale_);
}
AdaptiveMode AdaptiveControls::mode() const {
    return mode_;
}
bool AdaptiveControls::take_interface_toggle() {
    return std::exchange(interface_toggle_, false);
}
void AdaptiveControls::set_layout(const AdaptiveLayout &l, const Area &a, float scale) {
    // Visibility of the D-pad can change during a held movement/attack.
    auto comparable = l;
    comparable.at(E::DPad).shown = layout_.at(E::DPad).shown;
    if (comparable != layout_ || a.left != area_.left || a.top != area_.top || a.width != area_.width ||
        a.height != area_.height || scale != scale_)
        release_all();
    if (!l.at(E::DPad).shown && mode_ != AdaptiveMode::Interface)
        for (auto &f : fingers_)
            if (f.role == Role::DPad) f.role = Role::None;
    layout_ = l;
    area_ = a;
    scale_ = scale;
    for (std::size_t i = 0; i < kAdaptiveElements; ++i)
        placed_[i] = place(layout_.elements[i], static_cast<E>(i), area_, scale_, attack_profile());
    place_commands();
    if (item_area_) {
        const auto &a = *item_area_;
        placed_[index(E::ItemBar)] = {{a.left + a.width * .5f, a.top + a.height * .5f}, a.height * .5f, a.width * .5f};
    }
}
void AdaptiveControls::set_item_area(std::optional<Area> a) {
    if (a &&
        (!std::isfinite(a->left) || !std::isfinite(a->top) || !std::isfinite(a->width) || !std::isfinite(a->height) ||
            a->width <= 0 || a->height <= 0))
        a.reset();
    if (a.has_value() == item_area_.has_value() &&
        (!a ||
            (a->left == item_area_->left && a->top == item_area_->top && a->width == item_area_->width &&
                a->height == item_area_->height)))
        return;
    if (item_owner_) {
        if (const auto *f = find(*item_owner_); f && f->role == Role::Items) finger_up(*item_owner_, 0, true);
    }
    if (!ammo_busy()) clear_items();
    item_area_ = a;
    set_layout(layout_, area_, scale_);
}
bool AdaptiveControls::items_follow_hud() const {
    return item_area_.has_value();
}
void AdaptiveControls::set_weapon_posture(WeaponPosture posture) {
    if (posture_ == posture) return;
    posture_ = posture;
    aiming_ = false;
    fire_phase_ = FirePhase::Idle;
    fire_arc_ = arc_sampled_ = false;
    set_layout(layout_, area_, scale_);
    if (mode_ != AdaptiveMode::Quest) return;
    if (ammo_busy()) {
        for (auto &f : fingers_)
            if (f.role == Role::Ammo) f.role = Role::None;
        item_owner_.reset();
        clear_items();
    }
    // A held sprint/draw must not become a spirit attack on the next frame.
    // Movement, camera, evade and item selection retain their finger ownership.
    attack_.clear();
    special_.clear();
    for (auto &finger : fingers_)
        if (finger.role == Role::Attack || finger.role == Role::Special || finger.role == Role::Fire ||
            finger.role == Role::AimToggle ||
            (finger.role == Role::Button &&
                (finger.element == E::Attack || finger.element == E::Alternate || finger.element == E::Special ||
                    finger.element == E::Interact)))
            finger.role = Role::None;
}
WeaponPosture AdaptiveControls::weapon_posture() const {
    return posture_;
}
void AdaptiveControls::set_attack_profile(AttackProfile profile) {
    if (profile_ == profile) return;
    release_all();
    profile_ = profile;
    fire_phase_ = FirePhase::Idle;
    set_layout(layout_, area_, scale_);
    set_available(std::nullopt);
}
AttackProfile AdaptiveControls::attack_profile() const {
    return posture_ != WeaponPosture::Sheathed && mode_ == AdaptiveMode::Quest && layout_.at(E::Attack).shown &&
            layout_.at(E::Attack).buttons == 0x1000
        ? profile_
        : AttackProfile::Off;
}
void AdaptiveControls::set_available(std::optional<std::uint8_t> mask) {
    available_ = mask;
    attack_.set_available(mask);
}
std::optional<std::uint8_t> AdaptiveControls::available() const {
    return attack_profile() == AttackProfile::LongSword ? available_ : std::nullopt;
}
const AdaptiveLayout &AdaptiveControls::layout() const {
    return layout_;
}
const Area &AdaptiveControls::area() const {
    return area_;
}
float AdaptiveControls::scale() const {
    return scale_;
}
const Placed &AdaptiveControls::placed(E e) const {
    return placed_[index(e)];
}
bool AdaptiveControls::quest_unavailable() const {
    return mode_ == AdaptiveMode::Quest && profile_ >= AttackProfile::HuntingHorn &&
        profile_ <= AttackProfile::HeavyBowgun;
}
bool AdaptiveControls::visible(E e) const {
    if (quest_unavailable()) return false;
    if (e == E::Pause) return true;
    if (e == E::LeftFire) return bow_aiming() && !commands_ && layout_.at(e).shown;
    if (compact_) {
        const bool utility =
            e == E::YakumoMenu || e == E::Start || e == E::Select || e == E::InterfaceToggle || e == E::LeftShoulder;
        if (commands_) return utility;
        if (utility && !(mode_ == AdaptiveMode::Interface && e == E::LeftShoulder)) return false;
        // Keep a dedicated sheath action for every implemented weapon layout.
        const bool supported = input::touch::weapon_profile(profile_);
        const bool drawn_weapon = supported && posture_ == WeaponPosture::Drawn;
        // Hide only a duplicate: a disabled item surface must not remove Square.
        if (mode_ == AdaptiveMode::Quest && e == E::Interact && layout_.at(e).buttons == 0x8000 &&
            layout_.at(E::ItemBar).shown && !drawn_weapon)
            return false;
    } else if (e == E::InterfaceToggle)
        return true;
    if (e == E::YakumoMenu) return false;
    if (mode_ != AdaptiveMode::Quest && (e == E::ItemBar || e == E::R3)) return false;
    // Older saved village layouts may still show X. Keep custom rebound actions.
    if (mode_ == AdaptiveMode::Hub && (e == E::Attack || (e == E::Evade && layout_.at(e).buttons == 0x4000)))
        return false;
    if (mode_ == AdaptiveMode::Interface) {
        if (e == E::Stick || e == E::DpadToggle) return false;
        if (e == E::DPad) return true;
    }
    return layout_.at(e).shown &&
        (e != E::Alternate || attack_profile() == AttackProfile::Off || ranged_profile(attack_profile()));
}
AdaptiveControls::Finger *AdaptiveControls::find(std::uint64_t id) {
    for (auto &f : fingers_)
        if (f.used && f.id == id) return &f;
    return nullptr;
}
std::optional<E> AdaptiveControls::hit(Point at) const {
    std::optional<E> result;
    float best = 0;
    bool best_core = false;
    for (std::size_t i = 0; i < kAdaptiveElements; ++i) {
        const E e = static_cast<E>(i);
        if (e == E::Stick || e == E::ItemBar || !visible(e)) continue;
        const auto &p = placed_[i];
        const float x = std::abs(at.x - p.centre.x), y = std::abs(at.y - p.centre.y);
        const float distance = std::hypot(x, y);
        const bool inside = distance <= adaptive_hit_radius(p, area_);
        const bool core = distance <= p.radius;
        const float size = p.radius * p.half_width;
        // A neighbour's invisible padding must not steal a visible button press.
        if (inside && (!result || (core && !best_core) || (core == best_core && size < best))) {
            result = e;
            best = size;
            best_core = core;
        }
    }
    // Explicit buttons always win, even when the expanded item HUD overlaps them.
    if (!result && visible(E::ItemBar)) {
        const auto &p = placed(E::ItemBar);
        if (std::abs(at.x - p.centre.x) <= p.half_width && std::abs(at.y - p.centre.y) <= p.radius) result = E::ItemBar;
    }
    return result;
}
void AdaptiveControls::finger_down(std::uint64_t id, Point at, std::uint64_t ms) {
    if (quest_unavailable()) return;
    if (find(id)) finger_up(id, ms, true);
    if (!finite(at) || at.x < area_.left || at.x > area_.left + area_.width || at.y < area_.top ||
        at.y > area_.top + area_.height)
        return;
    Finger *f = nullptr;
    for (auto &candidate : fingers_)
        if (!candidate.used) {
            f = &candidate;
            break;
        }
    if (!f) return;
    *f = Finger{};
    f->id = id;
    f->started = ms;
    f->used = true;
    f->at = f->start = at;
    if (const auto e = hit(at)) {
        // A camera reset must never turn an item tap into L+Square selection.
        if (*e == E::ItemBar || (layout_.at(*e).buttons & 0xA000u)) camera_reset_ = false;
        f->element = *e;
        f->over = true;
        // Ammunition selection owns face-button chords until L is released.
        if (ammo_busy() &&
            (*e == E::Attack || *e == E::Special || *e == E::Evade || *e == E::Interact || *e == E::ItemBar))
            return;
        if (bow_toggle_available() && !commands_) {
            if (*e == E::Special) {
                f->role = Role::AimToggle;
                ++haptics_;
                return;
            }
            if (bow_aiming() && (*e == E::Attack || *e == E::LeftFire)) {
                if (fire_phase_ == FirePhase::Idle && !item_owner_ && item_count_ == 0) {
                    f->role = Role::Fire;
                    fire_phase_ = FirePhase::Held;
                    fire_sampled_ = false;
                    fire_arc_ = arc_sampled_ = false;
                    fire_reads_ = 0;
                    ++haptics_;
                }
                return;
            }
            if (bow_aiming() && *e == E::Evade && layout_.at(*e).buttons == 0x4000) cancel_fire(false);
            if (bow_aiming() && *e == E::Interact) {
                const bool charging = fire_phase_ != FirePhase::Idle;
                cancel_fire(true);
                if (charging) return;
            }
            if (bow_aiming() && (*e == E::Alternate || *e == E::ItemBar)) {
                if (fire_phase_ != FirePhase::Idle) return;
                aiming_ = false; // Native item/coating chords must not inherit the latched R.
            }
        }
        if (*e == E::Alternate && ranged_profile(attack_profile()) && layout_.at(*e).buttons == 0x2000) {
            // Never interrupt a charge/shot to open ammunition selection.
            if (!item_owner_ && !attack_.held() && !special_.held() && (buttons(ms) & 0xF200u) == 0 &&
                (item_count_ == 0 || ammo_busy())) {
                item_owner_ = id;
                f->role = Role::Ammo;
                camera_reset_ = false;
                ++haptics_;
            }
            return;
        }
        if (*e == E::DPad)
            f->role = Role::DPad;
        else if (*e == E::ItemBar) {
            if (!item_owner_) {
                item_owner_ = id;
                f->role = Role::Items;
            }
        } else if (*e == E::Attack && attack_profile() != AttackProfile::Off)
            f->role = attack_.begin(id, at, placed(E::Attack).radius, ms, attack_profile()) ? Role::Attack : Role::None;
        else if (*e == E::Special && layout_.at(*e).buttons == 0x0200 &&
            secondary_profile(attack_profile()) != AttackProfile::Off) {
            f->role = special_.begin(id, at, placed(*e).radius, ms, secondary_profile(attack_profile())) ? Role::Special
                                                                                                         : Role::None;
        } else {
            f->role = Role::Button;
            if (*e == E::Pause) {
                if (compact_) {
                    const bool open = !commands_;
                    release_all();
                    commands_ = open;
                    set_layout(layout_, area_, scale_);
                    return;
                }
                menu_ = true;
            }
            if (*e == E::YakumoMenu && !commands_) menu_ = true;
            if (*e == E::InterfaceToggle && !commands_) interface_toggle_ = true;
            if (*e == E::DpadToggle) {
                dpad_toggle_ = !dpad_toggle_;
                auto &dpad = layout_.at(E::DPad);
                dpad.shown = !dpad.shown;
                if (!dpad.shown)
                    for (auto &other : fingers_)
                        if (other.role == Role::DPad) other.role = Role::None;
            }
        }
        ++haptics_;
        return;
    }
    if (commands_) {
        release_all();
        return;
    }
    const auto &home = placed(E::Stick);
    const bool movement = layout_.floating_stick
        ? at.x >= area_.left && at.x <= area_.left + area_.width * .42f && at.y >= area_.top + area_.height * .45f &&
            at.y <= area_.top + area_.height
        : std::hypot(at.x - home.centre.x, at.y - home.centre.y) <= home.radius * 1.5f;
    if (visible(E::Stick) && !mover_ && movement) {
        mover_ = id;
        f->role = Role::Move;
        f->element = E::Stick;
        origin_ = layout_.floating_stick ? at : home.centre;
        thumb_ = at;
    } else
        f->role = mode_ == AdaptiveMode::Interface || has_camera_owner() ? Role::None : Role::Camera;
}
bool AdaptiveControls::has_camera_owner() const {
    return (ranged_profile(attack_profile()) && special_.held()) ||
        std::any_of(fingers_.begin(), fingers_.end(), [](const Finger &f) { return f.used && f.role == Role::Camera; });
}
bool AdaptiveControls::ammo_busy() const {
    for (const auto &f : fingers_)
        if (f.used && f.role == Role::Ammo) return true;
    for (std::size_t i = 0; i < item_count_; ++i) {
        const auto bits = item_queue_[(item_head_ + i) % item_queue_.size()];
        if (((bits & 0x0100u) && (bits & 0x5000u)) || (profile_ == AttackProfile::Bow && bits == 0x3000u)) return true;
    }
    return false;
}
bool AdaptiveControls::queue_item(std::uint16_t bits) {
    if (item_count_ == item_queue_.size()) return false;
    item_queue_[(item_head_ + item_count_) % item_queue_.size()] = bits;
    ++item_count_;
    return true;
}
void AdaptiveControls::clear_items() {
    item_head_ = item_count_ = 0;
    item_lead_reads_ = item_shown_reads_ = 0;
    item_gap_ = false;
}
std::uint16_t AdaptiveControls::sample_item_buttons(bool advance) {
    const auto request = item_count_ ? item_queue_[item_head_] : 0u;
    const bool selection = (request & kLeadButton) != 0;
    bool keep_open = false;
    for (const auto &finger : fingers_)
        keep_open =
            keep_open || (finger.used && (finger.role == Role::Items || finger.role == Role::Ammo) && finger.swiped);
    std::uint16_t out = selection || keep_open ? kLeadButton : 0u;
    if (!item_gap_ && request) {
        if (selection) {
            if (item_lead_reads_ >= kLeadReads) out = static_cast<std::uint16_t>(request);
        } else if (item_lead_reads_ == 0) {
            out = static_cast<std::uint16_t>(request);
        } else {
            // An item-use tap following a swipe needs an actual release of L.
            out = 0;
        }
    }
    if (advance) {
        item_lead_reads_ = (out & kLeadButton) ? std::min(item_lead_reads_ + 1u, kLeadReads) : 0u;
        if (item_gap_) {
            item_gap_ = false;
        } else if (request && out == request && ++item_shown_reads_ >= kMinReads) {
            item_head_ = (item_head_ + 1u) % item_queue_.size();
            --item_count_;
            item_shown_reads_ = 0;
            // Even two identical swipes need a released face-button sample.
            item_gap_ = true;
        }
    }
    return out;
}
void AdaptiveControls::finger_move(std::uint64_t id, Point at, std::uint64_t ms) {
    Finger *f = find(id);
    if (!f) return;
    if (!finite(at) || ms < f->started) {
        finger_up(id, ms, true);
        return;
    }
    const Point delta{at.x - f->at.x, at.y - f->at.y};
    switch (f->role) {
    case Role::Fire: {
        // One captured fire contact may add Circle; it never becomes camera input.
        const float x = at.x - f->start.x, y = at.y - f->start.y;
        const float threshold = std::max(6.f, placed(f->element).radius * .42f);
        if (!fire_arc_ && -y >= threshold && -y > std::abs(x)) {
            fire_arc_ = true;
            arc_sampled_ = false;
            ++haptics_;
        }
        break;
    }
    case Role::AimToggle: {
        const auto &p = placed(E::Special);
        f->over = std::hypot(at.x - p.centre.x, at.y - p.centre.y) <= p.radius * 1.55f;
        f->swiped = f->swiped || std::hypot(at.x - f->start.x, at.y - f->start.y) > p.radius * .35f;
        break;
    }
    case Role::Move:
        thumb_ = at;
        break;
    case Role::Attack: {
        const auto before = attack_.direction(ms);
        attack_.move(id, at, ms);
        if (before != attack_.direction(ms)) ++haptics_;
        break;
    }
    case Role::Special: {
        const auto before = special_.direction(ms);
        special_.move(id, at, ms);
        if (ranged_profile(attack_profile())) {
            camera_.x += delta.x;
            camera_.y += delta.y;
        }
        if (before != special_.direction(ms)) ++haptics_;
        break;
    }
    case Role::Button: {
        const auto &p = placed(f->element);
        f->over = std::hypot(at.x - p.centre.x, at.y - p.centre.y) <= p.radius * 1.55f;
        break;
    }
    case Role::DPad: {
        const auto &p = placed(E::DPad);
        if (dpad_buttons({at.x - p.centre.x, at.y - p.centre.y}, p.radius) !=
            dpad_buttons({f->at.x - p.centre.x, f->at.y - p.centre.y}, p.radius))
            ++haptics_;
        break;
    }
    case Role::Ammo:
    case Role::Items: {
        // A contact owns one selection, regardless of length or later reversal.
        // The finger must lift before another swipe can be recognized.
        if (f->swiped) break;
        const float x = at.x - f->start.x, y = at.y - f->start.y;
        const float threshold = std::max(6.0f, area_.height * .045f);
        if (std::abs(x) >= threshold && std::abs(x) > std::abs(y) * 1.2f) {
            const auto request = f->role == Role::Ammo ? (x > 0 ? 0x4100u : 0x1100u) : (x > 0 ? 0x2100u : 0x8100u);
            if (queue_item(static_cast<std::uint16_t>(request))) ++haptics_;
            f->swiped = true;
        } else if (!f->swiped && std::abs(y) >= threshold) {
            if (f->role == Role::Ammo) {
                f->swiped = true;
                break;
            }
            const bool camera_taken = has_camera_owner();
            f->role = camera_taken ? Role::None : Role::Camera;
            f->swiped = true; // An item gesture can rotate the view, never tap-recenter it.
            item_owner_.reset();
            if (camera_taken) break;
            camera_.x += x;
            camera_.y += y;
        }
        break;
    }
    case Role::Camera:
        // The captured aiming surface is the sole touch-camera owner while held.
        if (ranged_profile(attack_profile()) && special_.held()) {
            f->swiped = true;
            break;
        }
        // Remember excursions even when the finger returns to its starting point.
        f->swiped =
            f->swiped || std::hypot(at.x - f->start.x, at.y - f->start.y) >= std::max(6.f, area_.height * .015f);
        camera_.x += delta.x;
        camera_.y += delta.y;
        break;
    case Role::None:
        break;
    }
    f->at = at;
}
void AdaptiveControls::finger_up(std::uint64_t id, std::uint64_t ms, bool canceled) {
    Finger *f = find(id);
    if (!f) return;
    canceled = canceled || ms < f->started;
    if (f->role == Role::AimToggle && !canceled && f->over && !f->swiped && !attack_.held()) {
        if (bow_aiming())
            cancel_fire(true);
        else if (fire_phase_ == FirePhase::Idle && !item_owner_ && item_count_ == 0)
            aiming_ = true;
        camera_reset_ = false;
    }
    if (f->role == Role::Fire) {
        if (canceled)
            cancel_fire(false);
        else {
            const bool observed = fire_arc_ ? arc_sampled_ : fire_sampled_;
            fire_phase_ = observed ? FirePhase::Idle : FirePhase::Tap;
            fire_reads_ = 0;
        }
    }
    if (f->role == Role::Move) {
        mover_.reset();
        origin_ = thumb_ = {};
    }
    if (compact_ && !bow_aiming() && f->role == Role::Camera && !f->swiped && !canceled && ms - f->started < 200u &&
        !item_owner_ && item_count_ == 0 && (buttons(ms) & 0xA000u) == 0) {
        camera_reset_ = true; // Native L camera reset; free-space drags remain camera motion.
        camera_released_ = ms;
    }
    if (f->role == Role::Attack) attack_.end(id, ms, canceled);
    if (f->role == Role::Special) special_.end(id, ms, canceled);
    if (commands_ && f->role == Role::Button) {
        const auto bits = layout_.at(f->element).buttons;
        const bool menu = f->element == E::YakumoMenu && f->over && !canceled;
        const bool toggle = f->element == E::InterfaceToggle && f->over && !canceled;
        const bool commit = f->over && !canceled;
        release_all();
        if (commit) {
            command_pulse_ = bits;
            command_released_ = ms;
            menu_ = menu;
            interface_toggle_ = toggle;
        }
        return;
    }
    if (item_owner_ == id) {
        if (canceled)
            clear_items();
        else if (f->role == Role::Ammo) {
            if (!f->swiped && ms - f->started <= 350u && profile_ == AttackProfile::Bow)
                static_cast<void>(queue_item(0x3000u)); // native attach/remove coating, never during charge
        } else if (!f->swiped && ms - f->started <= 350u) {
            const bool drawn_items = mode_ == AdaptiveMode::Quest && profile_ == AttackProfile::SwordAndShield &&
                posture_ == WeaponPosture::Drawn;
            static_cast<void>(queue_item(drawn_items ? 0x8200u : 0x8000u));
        }
        item_owner_.reset();
    }
    *f = Finger{};
}
void AdaptiveControls::release_all() {
    cancel_fire(true);
    fingers_ = {};
    mover_.reset();
    item_owner_.reset();
    origin_ = thumb_ = camera_ = {};
    attack_.clear();
    special_.clear();
    commands_ = false;
    command_pulse_ = 0;
    camera_reset_ = false;
    menu_ = dpad_toggle_ = interface_toggle_ = false;
    haptics_ = 0;
    clear_items();
}
std::uint16_t AdaptiveControls::dpad_held() const {
    std::uint16_t bits = 0;
    if (!visible(E::DPad)) return bits;
    const auto &p = placed(E::DPad);
    for (const auto &f : fingers_)
        if (f.used && f.role == Role::DPad) bits |= dpad_buttons({f.at.x - p.centre.x, f.at.y - p.centre.y}, p.radius);
    return bits;
}
std::uint16_t AdaptiveControls::buttons(std::uint64_t ms) const {
    if (quest_unavailable()) return 0;
    std::uint16_t bits = dpad_held() | attack_.output(ms).buttons | special_.output(ms).buttons | fire_buttons();
    if (command_pulse_ && ms >= command_released_ && ms - command_released_ < AdaptiveAttack::kPulseMs)
        bits |= command_pulse_;
    for (const auto &f : fingers_)
        if (f.used) {
            // Drawer entries commit on release. Holding/canceling one must not
            // send Start, Select or L to the game underneath the panel.
            if (!commands_ && f.role == Role::Button && f.over) bits |= layout_.at(f.element).buttons;
        }
    if (camera_reset_ && ms >= camera_released_ && ms - camera_released_ < AdaptiveAttack::kPulseMs) bits |= 0x0100u;
    if (ammo_busy()) bits &= static_cast<std::uint16_t>(~0xF200u);
    return bits;
}
Point AdaptiveControls::stick(std::uint64_t ms) const {
    if (quest_unavailable()) return {};
    const auto attack = attack_.output(ms);
    if (attack.neutral_movement) return {};
    const float lateral = attack.lateral;
    if (lateral != 0) return {lateral, 0};
    return mover_ ? stick_deflection({thumb_.x - origin_.x, thumb_.y - origin_.y}, placed(E::Stick).radius) : Point{};
}
Point AdaptiveControls::stick_origin() const {
    return mover_ ? origin_ : placed(E::Stick).centre;
}
Point AdaptiveControls::stick_thumb() const {
    return mover_ ? thumb_ : placed(E::Stick).centre;
}
bool AdaptiveControls::held(E e) const {
    if (e == E::Stick) return mover_.has_value();
    if (e == E::DPad) return dpad_held() != 0;
    if (e == E::Attack && attack_.held()) return true;
    if (e == E::Special && special_.held()) return true;
    for (const auto &f : fingers_)
        if (f.used && f.element == e &&
            (f.role == Role::Button || f.role == Role::Items || f.role == Role::Ammo || f.role == Role::Fire ||
                f.role == Role::AimToggle) &&
            f.over)
            return true;
    return false;
}
AttackDirection AdaptiveControls::attack_direction(std::uint64_t ms, E element) const {
    if (bow_aiming() && (element == E::Attack || element == E::LeftFire)) {
        for (const auto &finger : fingers_)
            if (finger.used && finger.role == Role::Fire && finger.element == element)
                return fire_arc_ ? AttackDirection::Up : AttackDirection::Pending;
        return AttackDirection::None;
    }
    return element == E::Attack ? attack_.direction(ms) : AttackDirection::None;
}
Point AdaptiveControls::attack_thumb(std::uint64_t ms, E element) const {
    const auto &pad = placed(element);
    const Point centre = pad.centre;
    if (attack_profile() == AttackProfile::Off) return centre;
    const float reach = pad.radius * .28f;
    switch (attack_direction(ms, element)) {
    case AttackDirection::Left:
        return {centre.x - reach, centre.y};
    case AttackDirection::Right:
        return {centre.x + reach, centre.y};
    case AttackDirection::Up:
        return {centre.x, centre.y - reach};
    case AttackDirection::Down:
        return {centre.x, centre.y + reach};
    case AttackDirection::Pending:
        for (const auto &finger : fingers_) {
            if (!finger.used || finger.element != element || finger.role != (bow_aiming() ? Role::Fire : Role::Attack))
                continue;
            const double dx = static_cast<double>(finger.at.x) - finger.start.x;
            const double dy = static_cast<double>(finger.at.y) - finger.start.y;
            const double length = std::hypot(dx, dy);
            if (length > 0.0 && std::isfinite(length) && pad.radius > 0) {
                const double factor = std::min(length / pad.radius, 1.0) * reach / length;
                return {centre.x + static_cast<float>(dx * factor), centre.y + static_cast<float>(dy * factor)};
            }
        }
        return centre;
    default:
        return centre;
    }
}
Point AdaptiveControls::take_camera_drag() {
    return std::exchange(camera_, Point{});
}
bool AdaptiveControls::take_menu() {
    return std::exchange(menu_, false);
}
bool AdaptiveControls::take_dpad_toggle() {
    return std::exchange(dpad_toggle_, false);
}
int AdaptiveControls::take_haptics() {
    return std::exchange(haptics_, 0);
}
} // namespace mhp3rd::input::touch
