#include "input/touch_psp.hpp"

#include <algorithm>
#include <cmath>

namespace mhp3rd::input::touch {

std::uint16_t psp_button(Control control) {
    switch (control) {
    case Control::Triangle:
        return 0x1000u;
    case Control::Circle:
        return 0x2000u;
    case Control::Cross:
        return 0x4000u;
    case Control::Square:
        return 0x8000u;
    case Control::L:
        return 0x0100u;
    case Control::R:
        return 0x0200u;
    case Control::Start:
        return 0x0008u;
    case Control::Select:
        return 0x0001u;
    default:
        return 0u;
    }
}

bool Circle::contains(Point p, float slack) const {
    const float dx = p.x - centre.x;
    const float dy = p.y - centre.y;
    const float r = radius * slack;
    return dx * dx + dy * dy <= r * r;
}

Layout make_layout(float width, float height, Insets insets, float size, bool dpad) {
    Layout layout;
    layout.width = width;
    layout.height = height;
    const float left = insets.left;
    const float top = insets.top;
    const float right = width - insets.right;
    const float bottom = height - insets.bottom;
    // A face button about 9 mm across on a phone held sideways: the short side
    // is what the thumbs span, whatever the screen's shape.
    const float unit = std::min(height, width * 9.0f / 16.0f) * std::clamp(size, 0.5f, 2.0f);
    const float face = unit * 0.075f;
    const float spread = face * 1.45f;
    const Point diamond{right - spread - face * 1.6f, bottom - spread - face * 1.5f};
    auto &c = layout.controls;
    c[static_cast<std::size_t>(Control::Triangle)] = {{diamond.x, diamond.y - spread}, face};
    c[static_cast<std::size_t>(Control::Circle)] = {{diamond.x + spread, diamond.y}, face};
    c[static_cast<std::size_t>(Control::Cross)] = {{diamond.x, diamond.y + spread}, face};
    c[static_cast<std::size_t>(Control::Square)] = {{diamond.x - spread, diamond.y}, face};
    const float shoulder = face * 1.05f;
    c[static_cast<std::size_t>(Control::L)] = {{left + shoulder * 1.6f, top + shoulder * 1.4f}, shoulder};
    c[static_cast<std::size_t>(Control::R)] = {{right - shoulder * 1.6f, top + shoulder * 1.4f}, shoulder};
    const float small = face * 0.55f;
    const float middle = (left + right) * 0.5f;
    c[static_cast<std::size_t>(Control::Select)] = {{middle - small * 3.0f, top + small * 1.6f}, small};
    c[static_cast<std::size_t>(Control::Menu)] = {{middle, top + small * 1.6f}, small};
    c[static_cast<std::size_t>(Control::Start)] = {{middle + small * 3.0f, top + small * 1.6f}, small};
    c[static_cast<std::size_t>(Control::R3)] = {{right - unit * 0.075f, bottom - unit * 0.075f}, unit * 0.04f};
    c[static_cast<std::size_t>(Control::DpadToggle)] = {{left + unit * 0.06f, top + unit * 0.22f}, unit * 0.032f};
    for (std::size_t i = 0; i < kControls; ++i) {
        layout.buttons[i] = psp_button(static_cast<Control>(i));
        layout.shown[i] = i < static_cast<std::size_t>(Control::Combo1);
    }
    // The D-pad: at the left edge, half way down, clear of L above it.
    const float reach = face * 2.3f;
    layout.dpad_shown = dpad;
    layout.dpad = {{left + reach + face * 0.5f,
                       std::max(top + (bottom - top) * 0.47f,
                           c[static_cast<std::size_t>(Control::L)].centre.y + shoulder + reach + face * 0.4f)},
        reach};
    layout.stick_radius = unit * 0.13f;
    layout.stick = {{left + unit * 0.30f, bottom - unit * 0.22f}, layout.stick_radius};
    layout.stick_split = left + (right - left) * 0.45f;
    return layout;
}

const PspLayout &default_psp_layout() {
    static const PspLayout value = [] {
        PspLayout result;
        const Layout base = make_layout(1920.0f, 1080.0f, {}, 1.0f);
        const Area area = safe_area(1920.0f, 1080.0f, {});
        for (std::size_t i = 0; i < kPspElements; ++i) {
            const auto e = static_cast<PspElement>(i);
            if (is_combo(e)) {
                const std::size_t n = i - static_cast<std::size_t>(PspElement::Combo1);
                result.elements[i] = {Anchor::Right, 0.65f + static_cast<float>(n % 2u) * 0.16f,
                    0.36f + static_cast<float>(n / 2u) * 0.16f, 0.055f, 0u, false};
            } else {
                const Circle c = psp_element_circle(base, e);
                Placement p;
                p.size = c.radius / area.height;
                p.buttons = i < kControls ? base.buttons[i] : 0u;
                result.elements[i] = move_to(p, Element::Stick, c.centre, area);
            }
            Placement stable;
            if (parse(format(result.elements[i]), stable)) result.elements[i] = stable;
        }
        return result;
    }();
    return value;
}

const ElementInfo &info(PspElement element) {
    static constexpr ElementInfo entries[] = {{"triangle", "Triangle"}, {"circle", "Circle"}, {"cross", "Cross"},
        {"square", "Square"}, {"l", "L"}, {"r", "R"}, {"start", "Start"}, {"select", "Select"}, {"menu", "Menu"},
        {"r3", "R3"}, {"dpad_toggle", "D-Pad Toggle"}, {"combo1", "Combination 1"}, {"combo2", "Combination 2"},
        {"combo3", "Combination 3"}, {"combo4", "Combination 4"}, {"combo5", "Combination 5"},
        {"combo6", "Combination 6"}, {"combo7", "Combination 7"}, {"combo8", "Combination 8"}, {"dpad", "D-Pad"},
        {"stick", "Stick"}};
    return entries[static_cast<std::size_t>(element)];
}

bool is_combo(PspElement element) {
    return element >= PspElement::Combo1 && element <= PspElement::Combo8;
}

bool rebindable(PspElement element) {
    return element <= PspElement::Select || is_combo(element);
}

bool parse_psp_placement(PspElement element, std::string_view text, Placement &out) {
    Placement p;
    if (!parse(text, p) || (p.buttons & ~0xF3F9u) != 0u) return false;
    if (!rebindable(element)) p.buttons = 0u;
    if (element == PspElement::Menu) p.shown = true;
    out = p;
    return true;
}

std::optional<PspElement> add_psp_combo(PspLayout &layout) {
    for (std::size_t i = static_cast<std::size_t>(PspElement::Combo1);
        i <= static_cast<std::size_t>(PspElement::Combo8); ++i) {
        if (layout.elements[i] != default_psp_layout().elements[i]) continue;
        layout.elements[i] = default_psp_layout().elements[i];
        layout.elements[i].buttons = 0x2200u;
        layout.elements[i].shown = true;
        return static_cast<PspElement>(i);
    }
    return std::nullopt;
}

void remove_psp_combo(PspLayout &layout, PspElement element) {
    if (is_combo(element)) layout.at(element) = default_psp_layout().at(element);
}

Circle psp_element_circle(const Layout &layout, PspElement element) {
    if (element == PspElement::DPad) return layout.dpad;
    if (element == PspElement::Stick) return layout.stick;
    return layout.controls[static_cast<std::size_t>(element)];
}

Layout make_layout(float width, float height, Insets insets, float size, bool dpad, const PspLayout &custom) {
    Layout layout = make_layout(width, height, insets, size, dpad);
    const Area area = safe_area(width, height, insets);
    for (std::size_t i = 0; i < kPspElements; ++i) {
        const auto e = static_cast<PspElement>(i);
        const Placement &p = custom.elements[i];
        Circle circle = psp_element_circle(layout, e);
        // Unedited elements retain the PSP layout's size-dependent spacing.
        const Placement &normal = default_psp_layout().elements[i];
        if (p.anchor != normal.anchor || p.x != normal.x || p.y != normal.y) {
            const Placed placed = place(p, Element::Stick, area, size);
            circle.centre = placed.centre;
        }
        if (is_combo(e)) {
            const Placed placed = place(p, Element::Stick, area, size);
            circle = {placed.centre, placed.radius};
        } else {
            // The editor's percentage scales the actual factory radius, including
            // narrow screens and cutouts, rather than switching sizing units.
            circle.radius *= p.size / normal.size;
        }
        if (e == PspElement::DPad) {
            layout.dpad = circle;
            layout.dpad_shown = dpad && p.shown;
        } else if (e == PspElement::Stick) {
            layout.stick = circle;
            layout.stick_radius = circle.radius;
            layout.stick_shown = p.shown;
            layout.floating_stick = custom.floating_stick;
        } else {
            layout.controls[i] = circle;
            layout.buttons[i] = rebindable(e) ? p.buttons & 0xF3F9u : 0u;
            layout.shown[i] = p.shown || e == PspElement::Menu;
        }
    }
    return layout;
}

void Controls::set_layout(const Layout &layout) {
    layout_ = layout;
    for (Finger &finger : fingers_) {
        if (finger.role == Role::Control && !layout_.shown[static_cast<std::size_t>(finger.control)])
            finger.role = Role::None;
        if (finger.role == Role::DPad && !layout_.dpad_shown) finger.role = Role::None;
        if (finger.role == Role::Stick && !layout_.stick_shown) {
            finger.role = Role::None;
            stick_ = {};
        }
    }
}

std::uint16_t dpad_buttons(Point offset, float reach) {
    const float length = std::hypot(offset.x, offset.y);
    if (!(reach > 0.0f) || length < reach * 0.2f) return 0u;
    // Eight sectors of 45 degrees, the first centred on right; y grows down.
    constexpr float kPi = 3.14159265f;
    float angle = std::atan2(-offset.y, offset.x);
    if (angle < 0.0f) angle += 2.0f * kPi;
    const int sector = static_cast<int>(std::floor(angle / (kPi / 4.0f) + 0.5f)) % 8;
    constexpr std::uint16_t kUp = 0x10u, kRight = 0x20u, kDown = 0x40u, kLeft = 0x80u;
    constexpr std::uint16_t kSectors[8] = {
        kRight, kRight | kUp, kUp, kUp | kLeft, kLeft, kLeft | kDown, kDown, kDown | kRight};
    return kSectors[sector];
}

Point stick_deflection(Point offset, float reach, float dead_zone) {
    const float length = std::hypot(offset.x, offset.y);
    if (!(reach > 0.0f) || length <= reach * dead_zone) return {};
    const float amount = std::min((length / reach - dead_zone) / (1.0f - dead_zone), 1.0f);
    return {offset.x / length * amount, offset.y / length * amount};
}

std::optional<Control> Controls::control_at(Point at, bool face_only) const {
    // A little slack around each control: a thumb is wider than its aim.
    const std::size_t last = face_only ? static_cast<std::size_t>(Control::Square) : kControls - 1u;
    std::optional<Control> best;
    float best_distance = 0.0f;
    for (std::size_t i = 0; i <= last; ++i) {
        if (!layout_.shown[i]) continue;
        const Circle &circle = layout_.controls[i];
        if (!circle.contains(at, 1.2f)) continue;
        const float distance = std::hypot(at.x - circle.centre.x, at.y - circle.centre.y) / circle.radius;
        if (!best || distance < best_distance) {
            best = static_cast<Control>(i);
            best_distance = distance;
        }
    }
    return best;
}

Controls::Finger *Controls::find(std::uint64_t id) {
    for (Finger &finger : fingers_)
        if (finger.used && finger.id == id) return &finger;
    return nullptr;
}

void Controls::finger_down(std::uint64_t id, Point at) {
    if (find(id) != nullptr) finger_up(id);
    Finger *slot = nullptr;
    for (Finger &finger : fingers_)
        if (!finger.used) {
            slot = &finger;
            break;
        }
    if (slot == nullptr) return;
    *slot = Finger{id, true, Role::None, Control::Count, false, at};
    if (const std::optional<Control> control = control_at(at, false)) {
        slot->role = Role::Control;
        slot->control = *control;
        slot->over = true;
        ++haptics_;
        if (*control == Control::Menu) menu_tapped_ = true;
        if (*control == Control::DpadToggle) {
            dpad_toggled_ = !dpad_toggled_;
            layout_.dpad_shown = !layout_.dpad_shown;
            for (Finger &finger : fingers_)
                if (finger.role == Role::DPad) finger.role = Role::None;
        }
    } else if (layout_.dpad_shown && layout_.dpad.contains(at, 1.15f)) {
        slot->role = Role::DPad;
        if (dpad_held_by(*slot) != 0u) ++haptics_;
    } else if (layout_.stick_shown && !stick_.active &&
        (layout_.floating_stick ? at.x < layout_.stick_split : layout_.stick.contains(at, 1.45f))) {
        slot->role = Role::Stick;
        stick_ = {true, layout_.floating_stick ? at : layout_.stick.centre, at};
    } else if (at.x >= layout_.stick_split) {
        slot->role = Role::Camera;
    }
}

void Controls::finger_move(std::uint64_t id, Point at) {
    Finger *finger = find(id);
    if (finger == nullptr) return;
    switch (finger->role) {
    case Role::Stick: {
        stick_.thumb = at;
        // Beyond its reach the stick follows the thumb, so a long drag keeps
        // pushing the way the thumb went instead of losing the centre.
        const float dx = at.x - stick_.origin.x;
        const float dy = at.y - stick_.origin.y;
        const float length = std::hypot(dx, dy);
        if (layout_.floating_stick && length > layout_.stick_radius && length > 0.0f) {
            const float excess = (length - layout_.stick_radius) / length;
            stick_.origin.x += dx * excess;
            stick_.origin.y += dy * excess;
        }
        break;
    }
    case Role::Control: {
        // A thumb may slide from one face button to the next.
        const bool face = static_cast<std::size_t>(finger->control) <= static_cast<std::size_t>(Control::Square);
        if (face) {
            if (const std::optional<Control> control = control_at(at, true)) {
                if (finger->control != *control || !finger->over) ++haptics_;
                finger->control = *control;
                finger->over = true;
            } else {
                finger->over = false;
            }
        } else {
            const bool over = layout_.controls[static_cast<std::size_t>(finger->control)].contains(at, 1.6f);
            if (over && !finger->over) ++haptics_;
            finger->over = over;
        }
        break;
    }
    case Role::Camera:
        camera_drag_.x += at.x - finger->last.x;
        camera_drag_.y += at.y - finger->last.y;
        break;
    case Role::DPad: {
        const auto before = dpad_held_by(*finger);
        const auto after =
            dpad_buttons({at.x - layout_.dpad.centre.x, at.y - layout_.dpad.centre.y}, layout_.dpad.radius);
        if (after != 0u && after != before) ++haptics_;
        break;
    }
    case Role::None:
        break;
    }
    finger->last = at;
}

void Controls::finger_up(std::uint64_t id) {
    Finger *finger = find(id);
    if (finger == nullptr) return;
    if (finger->role == Role::Stick) stick_ = {};
    *finger = Finger{};
}

void Controls::release_all() {
    fingers_ = {};
    stick_ = {};
    camera_drag_ = {};
    menu_tapped_ = false;
    dpad_toggled_ = false;
    haptics_ = 0;
}

std::uint16_t Controls::buttons() const {
    std::uint16_t buttons = 0u;
    for (const Finger &finger : fingers_) {
        if (!finger.used) continue;
        if (finger.role == Role::Control && finger.over)
            buttons |= layout_.buttons[static_cast<std::size_t>(finger.control)];
        if (finger.role == Role::DPad) buttons |= dpad_held_by(finger);
    }
    return buttons;
}

std::uint16_t Controls::dpad_held_by(const Finger &finger) const {
    return dpad_buttons(
        {finger.last.x - layout_.dpad.centre.x, finger.last.y - layout_.dpad.centre.y}, layout_.dpad.radius);
}

std::uint16_t Controls::dpad_held() const {
    std::uint16_t held = 0u;
    for (const Finger &finger : fingers_)
        if (finger.used && finger.role == Role::DPad) held |= dpad_held_by(finger);
    return held;
}

bool Controls::held(Control control) const {
    for (const Finger &finger : fingers_)
        if (finger.used && finger.role == Role::Control && finger.over && finger.control == control) return true;
    return false;
}

bool Controls::any_finger() const {
    for (const Finger &finger : fingers_)
        if (finger.used) return true;
    return false;
}

Point Controls::stick() const {
    if (!stick_.active) return {};
    return stick_deflection({stick_.thumb.x - stick_.origin.x, stick_.thumb.y - stick_.origin.y}, layout_.stick_radius);
}

Point Controls::take_camera_drag() {
    const Point drag = camera_drag_;
    camera_drag_ = {};
    return drag;
}

bool Controls::take_menu() {
    const bool tapped = menu_tapped_;
    menu_tapped_ = false;
    return tapped;
}

bool Controls::take_dpad_toggle() {
    const bool toggled = dpad_toggled_;
    dpad_toggled_ = false;
    return toggled;
}

int Controls::take_haptics() {
    const int count = haptics_;
    haptics_ = 0;
    return count;
}

} // namespace mhp3rd::input::touch
