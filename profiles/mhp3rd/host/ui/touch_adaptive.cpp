#include "ui/touch_overlay.hpp"
#include "ui/touch_icons_data.hpp"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace mhp3rd::ui {
namespace {
using E = input::touch::AdaptiveElement;
using D = input::touch::AttackDirection;
using P = input::touch::AttackProfile;
using I = touch_icons::Icon;
ImU32 colour(unsigned rgb, float alpha) {
    return IM_COL32(
        (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, static_cast<int>(255 * std::clamp(alpha, 0.f, 1.f)));
}
void icon(ImDrawList *list, I id, ImVec2 centre, float size, float alpha, bool held) {
    const auto &glyph = touch_icons::glyphs[static_cast<unsigned>(id)];
    const auto ink = colour(held && glyph.colour == 0xf1edd8u ? 0xffd38au : glyph.colour, alpha);
    const float scale = size / 64.f;
    for (unsigned i = glyph.first; i < glyph.first + glyph.count; ++i) {
        const auto &path = touch_icons::paths[i];
        // Fixed asset bounds: no per-frame heap allocations or runtime SVG parsing.
        std::array<ImVec2, 256> points;
        if (path.count > points.size()) continue;
        for (unsigned j = 0; j < path.count; ++j) {
            const auto &v = touch_icons::points[path.first + j];
            points[j] = {centre.x + (v.x - 32) * scale, centre.y + (v.y - 32) * scale};
        }
        if (path.filled && path.count >= 3)
            list->AddConcavePolyFilled(points.data(), static_cast<int>(path.count), ink);
        if (path.width > 0) {
            const float width = std::max(.7f, path.width * scale);
            list->AddPolyline(points.data(), static_cast<int>(path.count), ink,
                path.closed ? ImDrawFlags_Closed : ImDrawFlags_None, width);
            if (!path.closed) {
                list->AddCircleFilled(points.front(), width * .5f, ink);
                list->AddCircleFilled(points[path.count - 1], width * .5f, ink);
            }
        }
    }
}
void frame(ImDrawList *list, ImVec2 c, float r, float a, bool held) {
    list->AddCircleFilled(c, r, colour(held ? 0x293b3du : 0x142126u, a * .9f));
    list->AddCircle(c, r * .97f, colour(0x080e11u, a), 0, std::max(1.f, r * .065f));
    list->AddCircle(c, r * .90f, colour(held ? 0xf7d48au : 0x94b2aeu, a), 0, std::max(1.f, r * .04f));
    list->AddCircle(c, r * .80f, colour(0x9f773fu, a), 0, std::max(.8f, r * .03f));
}
void centred(ImDrawList *list, ImVec2 c, float size, const char *text, ImU32 col) {
    auto *font = ImGui::GetFont();
    const auto extent = font->CalcTextSizeA(size, 1e9f, 0, text);
    list->AddText(font, size, {c.x - extent.x * .5f, c.y - extent.y * .5f}, col, text);
}
I weapon_icon(P profile, D direction) {
    switch (profile) {
    case P::GreatSword:
        return direction == D::Primary || direction == D::Up ? I::GreatSwordCharge
            : direction == D::Right                          ? I::GreatSwordSweep
                                                             : I::GreatSword;
    case P::Bow:
        return I::Bow;
    case P::SwordAndShield:
        return I::SwordAttack;
    case P::DualBlades:
        return direction == D::Left || direction == D::Right ? I::DualBladesSpin : I::DualBlades;
    case P::Hammer:
        // Circle is the ordinary side swing, not evidence of a charged spin.
        return I::Hammer;
    default:
        return I::LongSword;
    }
}
I special_icon(P profile, D direction) {
    switch (profile) {
    case P::GreatSword:
        return I::GreatSwordGuard;
    case P::SwordAndShield:
        return direction == D::Left || direction == D::Right || direction == D::Down ? I::GuardAttack : I::ShieldGuard;
    case P::DualBlades:
        return I::DemonMode;
    case P::Hammer:
        return I::HammerCharge;
    case P::Bow:
        return I::Scope;
    default:
        return I::SpiritSlash;
    }
}
I element_icon(E e, input::touch::AdaptiveMode mode, bool sheathed, P profile, D direction) {
    if (mode == input::touch::AdaptiveMode::Interface) {
        if (e == E::Alternate) return I::Confirm;
        if (e == E::Evade) return I::Cancel;
    }
    switch (e) {
    case E::Attack:
        return sheathed ? I::DrawWeapon : weapon_icon(profile, direction);
    case E::Special:
        return mode == input::touch::AdaptiveMode::Hub || sheathed ? I::Sprint : special_icon(profile, direction);
    case E::Alternate:
        if (mode == input::touch::AdaptiveMode::Quest && !sheathed && input::touch::ranged_profile(profile))
            return I::Coating;
        return mode == input::touch::AdaptiveMode::Hub ? I::Talk : I::Interact;
    case E::Evade:
        return I::Evade;
    case E::Interact:
        if (mode == input::touch::AdaptiveMode::Hub) return I::Interact;
        return sheathed ? I::UseItem : I::Sheathe;
    case E::LeftFire:
        return I::BowShot;
    case E::R3:
        return I::LockOn;
    case E::DpadToggle:
        return I::DPad;
    case E::Start:
        return I::Interface;
    case E::Select:
        return I::Select;
    case E::LeftShoulder:
        return I::CameraCenter;
    case E::InterfaceToggle:
        return I::Interface;
    default:
        return I::Menu;
    }
}
} // namespace
const char *adaptive_profile_name(P profile) {
    switch (profile) {
    case P::LongSword:
        return "Long Sword";
    case P::GreatSword:
        return "Great Sword";
    case P::SwordAndShield:
        return "Sword and Shield";
    case P::DualBlades:
        return "Dual Blades";
    case P::Hammer:
        return "Hammer";
    case P::HuntingHorn:
        return "Hunting Horn";
    case P::Lance:
        return "Lance";
    case P::Gunlance:
        return "Gunlance";
    case P::SwitchAxe:
        return "Switch Axe";
    case P::LightBowgun:
        return "Light Bowgun";
    case P::HeavyBowgun:
        return "Heavy Bowgun";
    case P::Bow:
        return "Bow";
    default:
        return "Ordinary Buttons";
    }
}
const char *adaptive_gesture_help(E element, P profile) {
    if (profile >= P::HuntingHorn && profile <= P::HeavyBowgun)
        return "Adaptive does not support this weapon yet. Quest touch controls are hidden.";
    if (element == E::Attack) switch (profile) {
        case P::LongSword:
            return "Tap/hold: strike. Up: thrust. Down: fade slash. Left/right: side fade after an eligible attack.";
        case P::GreatSword:
            return "Tap: slash. Hold or up: charge; lift to release. Right: sweep. Down: rising slash. Left: kick.";
        case P::SwordAndShield:
            return "Tap: sword chain. Up: combined attack. Right: spin. Down: horizontal chain. Left: Circle with your movement.";
        case P::DualBlades:
            return "Tap: primary chain. Up: combined attack. Left/right: side attack. The game selects Normal, Demon or Archdemon moves.";
        case P::Hammer:
            return "Tap: primary chain or follow-up. Right: side swing. Charge on the separate weapon button.";
        case P::Bow:
            return "Hold: charge; lift to fire. Up: Circle; after charging it adds Circle without releasing Triangle for Arc Shot. Down is unassigned. In aim mode swipe either fire button up for Arc Shot.";
        default:
            return "Ordinary button input. A recognized weapon with the default binding enables its gestures.";
        }
    if (element == E::Special) switch (profile) {
        case P::LongSword:
            return "Hold R for Spirit attack. The game controls the Spirit chain and gauge.";
        case P::GreatSword:
            return "Hold to guard; lift to release. Main tap while guarding requests a kick.";
        case P::SwordAndShield:
            return "Hold to guard. Left/right: guarded attack. Down: R + Triangle. Up keeps guarding; evade is separate.";
        case P::DualBlades:
            return "Tap to request Demon Mode on/off. The game controls stamina and mode transitions.";
        case P::Hammer:
            return "Hold to charge; lift to release. Movement selects the native charge result. Evade cancels where the game permits.";
        case P::Bow:
            return "Tap to enter/leave aim. Drag free screen space to aim. In aim mode either fire button holds charge and shoots on release; swipe up adds Circle for Arc Shot. Outside aim, up sends Circle; down is unassigned; an upward flick also preserves a held charge.";
        default:
            return "Ordinary R button; sprint while the weapon is sheathed.";
        }
    if (element == E::Alternate) {
        if (input::touch::ranged_profile(profile))
            return profile == P::Bow
                ? "Swipe left/right: previous/next coating. Tap: attach/remove coating. Release charge/aim before selecting."
                : "Swipe left/right: previous/next ammunition. One entry per gesture. Release fire/aim before selecting.";
        return "Fallback Circle button. Hidden during supported melee attack gestures; available for native interfaces.";
    }
    if (element == E::Interact)
        return "Separate Square action to sheathe. While sheathed it uses the item; in the village it interacts.";
    if (element == E::LeftFire)
        return "Appears on the left only while Bow aim is toggled on. Hold to charge; release to shoot while your right thumb aims. Swipe up for Arc Shot. It shares one charge with the main button.";
    if (element == E::DPad)
        return "Drag across the cross to change direction. Diagonals press two directions; the centre is neutral.";
    return "";
}
void draw_adaptive_element(ImDrawList *list, E e, const input::touch::Placed &p, const input::touch::Point &thumb,
    const ElementLook &look, D direction, input::touch::AdaptiveMode mode, input::touch::WeaponPosture posture,
    P profile) {
    const float a = std::clamp(look.opacity, .1f, 1.f) * (look.hidden ? .35f : 1.f);
    const bool sheathed = mode == input::touch::AdaptiveMode::Quest && posture == input::touch::WeaponPosture::Sheathed;
    const float r = p.radius;
    const ImVec2 c{p.centre.x, p.centre.y};
    const auto col = colour(0xf1edd8u, a), accent = colour(0xffd38au, std::max(a, .8f));
    if (e == E::ItemBar) {
        list->AddRectFilled(
            {c.x - p.half_width, c.y - r}, {c.x + p.half_width, c.y + r}, colour(0x142126u, a), r * .3f);
        list->AddRect({c.x - p.half_width, c.y - r}, {c.x + p.half_width, c.y + r}, col, r * .3f);
        const bool drawn = mode == input::touch::AdaptiveMode::Quest && posture == input::touch::WeaponPosture::Drawn;
        icon(list, drawn && profile != P::SwordAndShield ? I::Sheathe : I::UseItem, c, r * 1.6f, a, look.held);
        centred(list, {c.x - p.half_width + r, c.y}, r, "<", col);
        centred(list, {c.x + p.half_width - r, c.y}, r, ">", col);
    } else if (e == E::DPad) {
        // One continuous, bevelled cross. The centre remains the input dead zone.
        constexpr float w = .36f, b = .08f;
        constexpr ImVec2 shape[] = {{-w, -1 + b}, {-w + b, -1}, {w - b, -1}, {w, -1 + b}, {w, -w - b}, {w + b, -w},
            {1 - b, -w}, {1, -w + b}, {1, w - b}, {1 - b, w}, {w + b, w}, {w, w + b}, {w, 1 - b}, {w - b, 1},
            {-w + b, 1}, {-w, 1 - b}, {-w, w + b}, {-w - b, w}, {-1 + b, w}, {-1, w - b}, {-1, -w + b}, {-1 + b, -w},
            {-w - b, -w}, {-w, -w - b}};
        std::array<ImVec2, std::size(shape)> outline;
        const auto scaled = [&](float factor) {
            for (std::size_t i = 0; i < outline.size(); ++i)
                outline[i] = {c.x + shape[i].x * r * factor, c.y + shape[i].y * r * factor};
        };
        scaled(.96f);
        list->AddConcavePolyFilled(outline.data(), static_cast<int>(outline.size()), colour(0x142126u, a * .9f));
        list->AddPolyline(outline.data(), static_cast<int>(outline.size()), colour(0x080e11u, a), ImDrawFlags_Closed,
            std::max(2.f, r * .075f));
        list->AddPolyline(outline.data(), static_cast<int>(outline.size()), colour(0x94b2aeu, a), ImDrawFlags_Closed,
            std::max(1.f, r * .035f));
        scaled(.84f);
        list->AddPolyline(outline.data(), static_cast<int>(outline.size()), colour(0x9f773fu, a * .8f),
            ImDrawFlags_Closed, std::max(.8f, r * .02f));
        constexpr unsigned bits[] = {0x10, 0x20, 0x40, 0x80};
        constexpr float dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
        for (unsigned i = 0; i < 4; ++i) {
            const auto point = [&](float along, float across) {
                return ImVec2{c.x + r * (dx[i] * along - dy[i] * across), c.y + r * (dy[i] * along + dx[i] * across)};
            };
            const bool pressed = (look.directions & bits[i]) != 0;
            if (pressed) {
                const ImVec2 face[] = {point(.30f, -.25f), point(.74f, -.25f), point(.82f, -.17f), point(.82f, .17f),
                    point(.74f, .25f), point(.30f, .25f)};
                list->AddConvexPolyFilled(face, 6, colour(0xffd38au, a * .28f));
            }
            const ImVec2 arrow[] = {point(.57f, -.12f), point(.71f, 0), point(.57f, .12f)};
            list->AddPolyline(arrow, 3, pressed ? accent : col, 0, std::max(1.5f, r * .045f));
        }
        // A quiet centre mark joins the arms without suggesting a fifth button.
        const ImVec2 centre[] = {
            {c.x, c.y - r * .10f}, {c.x + r * .10f, c.y}, {c.x, c.y + r * .10f}, {c.x - r * .10f, c.y}};
        list->AddPolyline(centre, 4, colour(0x94b2aeu, a * .45f), ImDrawFlags_Closed, std::max(.8f, r * .02f));
    } else if (e == E::Stick) {
        frame(list, c, r, a * .7f, look.held);
        const float dx = thumb.x - c.x, dy = thumb.y - c.y, length = std::hypot(dx, dy);
        const float k = length > r ? r / length : 1.f;
        list->AddCircleFilled(look.held ? ImVec2{c.x + dx * k, c.y + dy * k} : c, r * .35f, col);
    } else {
        frame(list, c, r, a, look.held);
        ImVec2 cap = c;
        const bool quest_action = !sheathed && mode == input::touch::AdaptiveMode::Quest;
        const bool bow_fire = look.bow_aiming && profile == P::Bow && (e == E::Attack || e == E::LeftFire);
        const bool selector = quest_action &&
            (bow_fire || (e == E::Attack && profile != P::Off) || (e == E::Special && profile == P::SwordAndShield));
        if (selector) {
            ImVec2 axis{};
            if (direction == D::Up) axis.y = -1;
            if (direction == D::Down) axis.y = 1;
            if (direction == D::Left) axis.x = -1;
            if (direction == D::Right) axis.x = 1;
            if ((e == E::Attack || bow_fire) && direction != D::None) {
                const float dx = thumb.x - c.x, dy = thumb.y - c.y, length = std::hypot(dx, dy);
                if (std::isfinite(length) && length > 0) {
                    const float k = std::min(r * .22f / length, 1.f);
                    cap = {c.x + dx * k, c.y + dy * k};
                }
            } else {
                cap = {c.x + axis.x * r * .22f, c.y + axis.y * r * .22f};
            }
            // A persistent inner cap identifies a gesture surface, including at rest.
            list->AddCircleFilled(cap, r * .58f, colour(look.held ? 0x293b3du : 0x1c2d31u, a));
            list->AddCircle(cap, r * .58f, colour(look.held ? 0xffd38au : 0x94b2aeu, a), 0, std::max(1.f, r * .025f));
            const auto arrow = [&](ImVec2 axis, bool selected) {
                // Outside the contact circle: these are direction hints, not extra buttons.
                const float reach = r * (selected ? 1.36f : 1.30f);
                const float base = r * 1.12f, wing = r * (selected ? .20f : .17f);
                const ImVec2 points[] = {{c.x + axis.x * base - axis.y * wing, c.y + axis.y * base + axis.x * wing},
                    {c.x + axis.x * reach, c.y + axis.y * reach},
                    {c.x + axis.x * base + axis.y * wing, c.y + axis.y * base - axis.x * wing}};
                const float width = std::max(1.5f, r * (selected ? .075f : .06f));
                const float alpha = std::max(a, .85f);
                list->AddPolyline(points, 3, colour(0x080e11u, alpha), 0, width + std::max(2.f, r * .05f));
                list->AddPolyline(points, 3, colour(selected ? 0xffd38au : 0xf1edd8u, alpha), 0, width);
            };
            auto hints = bow_fire ? std::optional{input::touch::gesture_directions(P::Bow)} : look.available;
            // These are fixed input legends. Only LS supplies live combo availability.
            if (!hints && profile != P::LongSword && profile != P::SwordAndShield)
                hints = input::touch::gesture_directions(
                    e == E::Special ? input::touch::secondary_profile(profile) : profile);
            if (hints) {
                constexpr D choices[] = {D::Up, D::Down, D::Left, D::Right};
                constexpr ImVec2 axes[] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
                for (unsigned i = 0; i < 4; ++i)
                    if ((*hints >> static_cast<unsigned>(choices[i])) & 1u) arrow(axes[i], direction == choices[i]);
            } else if (axis.x != 0 || axis.y != 0) {
                arrow(axis, true);
            }
        }
        const bool ordinary = mode == input::touch::AdaptiveMode::Interface || (quest_action && profile == P::Off);
        // Preserve explicit PSP symbols for the two non-confirm interface keys.
        if (quest_action && e == E::Alternate && input::touch::ranged_profile(profile) && !ordinary) {
            centred(list, {c.x - r * .72f, c.y}, r * .45f, "<", col);
            centred(list, {c.x + r * .72f, c.y}, r * .45f, ">", col);
        }
        if (ordinary && (e == E::Attack || e == E::Interact)) {
            const float z = r * .37f;
            if (e == E::Attack)
                list->AddTriangle({c.x, c.y - z}, {c.x - z, c.y + z}, {c.x + z, c.y + z}, col, 2.f);
            else
                list->AddRect({c.x - z, c.y - z}, {c.x + z, c.y + z}, col, 0, 0, 2.f);
        } else if (ordinary && (e == E::Special || e == E::LeftShoulder)) {
            centred(list, c, r * .85f, e == E::Special ? "R" : "L", col);
        } else {
            const auto glyph = look.bow_aiming && e == E::Attack && profile == P::Bow
                ? I::BowShot
                : element_icon(e, mode, sheathed, profile, direction);
            icon(list, glyph, cap, r * (selector ? 1.10f : 1.35f),
                e == E::Attack && (direction == D::Blocked || (look.available && !*look.available)) ? a * .4f : a,
                look.held);
        }
    }
    if (look.hidden) list->AddLine({c.x - r * .7f, c.y + r * .7f}, {c.x + r * .7f, c.y - r * .7f}, col, 2.f);
    if (look.selected && e != E::ItemBar) list->AddCircle(c, r + 4, accent, 0, 2.f);
}
void draw_adaptive_controls(const input::touch::AdaptiveControls &controls, float opacity, std::uint64_t ms) {
    auto *list = ImGui::GetBackgroundDrawList();
    if (controls.commands_open()) {
        const auto &a = controls.placed(E::YakumoMenu);
        const auto &b = controls.placed(E::InterfaceToggle);
        const float step = a.radius * 3.3f;
        list->AddRectFilled({a.centre.x - a.radius * 1.4f, a.centre.y - a.radius * 1.4f},
            {a.centre.x + 2 * step + a.radius * 1.4f, b.centre.y + a.radius * 2.f}, colour(0x101b20u, .96f),
            a.radius * .4f);
    }
    for (std::size_t i = 0; i < input::touch::kAdaptiveElements; ++i) {
        const auto e = static_cast<E>(i);
        if (!controls.visible(e)) continue;
        if (e == E::ItemBar && controls.items_follow_hud()) continue;
        if (e == E::Stick && controls.layout().floating_stick && !controls.held(e)) continue;
        auto p = controls.placed(e);
        if (e == E::Stick) p.centre = controls.stick_origin();
        ElementLook look;
        look.bow_aiming = controls.bow_aiming();
        look.opacity = opacity;
        look.held = controls.held(e) || (look.bow_aiming && e == E::Special);
        if (e == E::Attack) look.available = controls.available();
        if (e == E::DPad) look.directions = controls.dpad_held();
        const auto direction = e == E::Special ? controls.special_direction(ms) : controls.attack_direction(ms, e);
        const auto thumb = e == E::Attack || e == E::LeftFire ? controls.attack_thumb(ms, e) : controls.stick_thumb();
        draw_adaptive_element(
            list, e, p, thumb, look, direction, controls.mode(), controls.weapon_posture(), controls.weapon_profile());
        if (controls.commands_open() && e != E::Pause) {
            const char *label = e == E::YakumoMenu ? "Yakumo"
                : e == E::Start                    ? "Game Menu"
                : e == E::Select                   ? "Select"
                : e == E::LeftShoulder
                ? (controls.mode() == input::touch::AdaptiveMode::Interface ? "Previous Page" : "Camera")
                : "Interface / Auto";
            centred(list, {p.centre.x, p.centre.y + p.radius * 1.55f}, std::max(9.f, p.radius * .55f), label,
                colour(0xf1edd8u, 1));
        }
    }
}
} // namespace mhp3rd::ui
