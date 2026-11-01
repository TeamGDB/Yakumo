#include "ui/touch_editor.hpp"

#include "ui/layer.hpp"
#include "ui/hud_editor.hpp"
#include "ui/touch_overlay.hpp"
#include "ui/widgets.hpp"

#include "gpu/vulkan_renderer.hpp"
#include "gpu/game_hud.hpp"
#include "input/touch_action.hpp"
#include "settings/settings.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace mhp3rd::ui {
namespace {

using input::touch::Element;

struct State {
    bool open{};
    bool psp{};
    bool adaptive{};
    input::touch::AdaptiveMode adaptive_mode{input::touch::AdaptiveMode::Quest};
    std::optional<std::size_t> selected;
    bool dragging{};
    bool element_right{};
    input::touch::AttackProfile preview{input::touch::AttackProfile::LongSword};
    bool choosing_buttons{};
    ImVec2 grab{};     // the pointer's offset from the element's centre
    int panel_place{}; // top, bottom, left or right
};

State &state() {
    static State value;
    return value;
}

int cycle(int value, int delta, int count) {
    return ((value + delta) % count + count) % count;
}

} // namespace

void open_touch_editor() {
    State &s = state();
    s = State{};
    s.open = true;
    s.psp = settings::current().touch_layout == settings::TouchLayout::Psp;
    s.adaptive = settings::current().touch_layout == settings::TouchLayout::Adaptive;
    if (s.psp || s.adaptive) s.panel_place = 2;
    if (s.adaptive) s.adaptive_mode = Layer::get().renderer().adaptive_touch_controls().mode();
    if (s.adaptive) {
        const auto profile = Layer::get().renderer().adaptive_touch_controls().weapon_profile();
        if (input::touch::weapon_profile(profile)) s.preview = profile;
    }
    focus_next_row();
}

bool touch_editor_open() {
    return state().open;
}

void touch_editor_frame(bool back) {
    State &st = state();
    settings::Settings &s = settings::current();
    const ImGuiIO &io = ImGui::GetIO();
    const float font = Layer::get().font_size();
    const input::touch::ActionControls &live = Layer::get().renderer().action_touch_controls();
    const input::touch::Area area = live.area();
    const float scale = live.scale();
    using A = input::touch::AdaptiveElement;
    const auto &adaptive = Layer::get().renderer().adaptive_touch_controls();
    auto &adaptive_settings = settings::adaptive_layout(s, st.adaptive_mode);
    const std::size_t count = st.psp ? input::touch::kPspElements
        : st.adaptive                ? input::touch::kAdaptiveElements - 1
                                     : input::touch::kElements;
    const auto placement = [&](std::size_t i) -> input::touch::Placement & {
        if (st.psp) return s.touch_psp.elements[i];
        if (st.adaptive) return adaptive_settings.elements[i];
        return s.touch_action.elements[i];
    };
    const auto normal = [&](std::size_t i) -> const input::touch::Placement & {
        if (st.psp) return input::touch::default_psp_layout().elements[i];
        if (st.adaptive) return input::touch::default_adaptive_layout(st.adaptive_mode).elements[i];
        return input::touch::default_action_layout().elements[i];
    };
    const auto linked_items = [&](std::size_t i) {
        return st.adaptive && st.adaptive_mode == input::touch::AdaptiveMode::Quest &&
            i == static_cast<std::size_t>(A::ItemBar) && adaptive.items_follow_hud();
    };
    const auto placed = [&](std::size_t i) {
        if (st.psp) {
            const auto circle = input::touch::psp_element_circle(
                Layer::get().renderer().touch_controls().layout(), static_cast<input::touch::PspElement>(i));
            return input::touch::Placed{circle.centre, circle.radius, circle.radius};
        }
        if (st.adaptive)
            return linked_items(i)
                ? adaptive.placed(static_cast<A>(i))
                : input::touch::place(placement(i), static_cast<A>(i), area, scale,
                      st.adaptive_mode == input::touch::AdaptiveMode::Quest ? st.preview
                                                                            : input::touch::AttackProfile::Off);
        return input::touch::place(placement(i), static_cast<Element>(i), area, scale);
    };

    // The game, dimmed a little, and the layout over it.
    ImDrawList *list = ImGui::GetBackgroundDrawList();
    list->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(0, 0, 0, 90));
    for (std::size_t i = 0; i < count; ++i) {
        if (st.psp && input::touch::is_combo(static_cast<input::touch::PspElement>(i)) && placement(i) == normal(i))
            continue;
        if (st.adaptive && static_cast<A>(i) == A::LeftFire &&
            (st.adaptive_mode != input::touch::AdaptiveMode::Quest || st.preview != input::touch::AttackProfile::Bow))
            continue;
        const auto e = static_cast<Element>(i);
        ElementLook look;
        look.opacity = std::max(s.touch_opacity, 0.6f);
        look.hidden = !placement(i).shown;
        if (st.psp && i == static_cast<std::size_t>(input::touch::PspElement::DPad))
            look.hidden = look.hidden || !s.touch_dpad;
        if (st.adaptive && st.adaptive_mode == input::touch::AdaptiveMode::Quest) {
            const auto element = static_cast<A>(i);
            if (element == A::Attack || element == A::Special)
                look.available = input::touch::gesture_directions(
                    element == A::Special ? input::touch::secondary_profile(st.preview) : st.preview);
        }
        look.selected = st.selected == i;
        look.held = st.dragging && st.selected == i;
        if (st.psp)
            draw_psp_element(list, static_cast<input::touch::PspElement>(i),
                Layer::get().renderer().touch_controls().layout(), look);
        else if (st.adaptive)
            draw_adaptive_element(list, static_cast<A>(i), placed(i), {}, look, input::touch::AttackDirection::None,
                st.adaptive_mode, input::touch::WeaponPosture::Drawn, st.preview);
        else
            draw_action_element(list, e, placement(i), placed(i), {}, look);
    }

    if (st.adaptive && st.selected && static_cast<A>(*st.selected) != A::ItemBar) {
        const auto p = placed(*st.selected);
        list->AddCircle(
            {p.centre.x, p.centre.y}, input::touch::adaptive_hit_radius(p, area), IM_COL32(241, 237, 216, 100), 0, 1.f);
    }

    // Choosing and dragging, where the panel is not.
    if (!io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.choosing_buttons = false;
        st.selected.reset();
        float best = 0;
        bool best_core = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (st.psp && input::touch::is_combo(static_cast<input::touch::PspElement>(i)) && placement(i) == normal(i))
                continue;
            if (st.adaptive && static_cast<A>(i) == A::LeftFire &&
                (st.adaptive_mode != input::touch::AdaptiveMode::Quest ||
                    st.preview != input::touch::AttackProfile::Bow))
                continue;
            const auto p = placed(i);
            const bool wide =
                st.adaptive ? static_cast<A>(i) == A::ItemBar : !st.psp && static_cast<Element>(i) == Element::Swipe;
            const float distance = std::hypot(io.MousePos.x - p.centre.x, io.MousePos.y - p.centre.y);
            const bool core = wide || distance <= p.radius;
            const bool hit = wide ? std::abs(io.MousePos.x - p.centre.x) <= p.half_width &&
                    std::abs(io.MousePos.y - p.centre.y) <= p.radius
                                  : distance <=
                    (st.adaptive ? input::touch::adaptive_hit_radius(p, area) : std::max(p.radius * 1.2f, 12.f));
            const float size = p.radius * p.half_width;
            if (hit &&
                (!st.selected || (st.adaptive && core && !best_core) ||
                    ((!st.adaptive || core == best_core) && size < best))) {
                st.selected = i;
                best = size;
                best_core = core;
            }
        }
        st.dragging = st.selected.has_value() && !linked_items(*st.selected);
        if (st.selected) {
            const input::touch::Placed p = placed(*st.selected);
            st.grab = {io.MousePos.x - p.centre.x, io.MousePos.y - p.centre.y};
            st.element_right = p.centre.x > io.DisplaySize.x * .5f;
        }
    }
    if (st.dragging && st.selected) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const input::touch::Point centre{io.MousePos.x - st.grab.x, io.MousePos.y - st.grab.y};
            auto &p = placement(*st.selected);
            if (st.adaptive)
                p = input::touch::move_to(p, static_cast<A>(*st.selected), centre, area);
            else
                p = input::touch::move_to(
                    p, st.psp ? Element::Stick : static_cast<Element>(*st.selected), centre, area);
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            st.dragging = false;
            settings::save();
        }
    }

    // The panel: at the top or the bottom with nothing chosen, beside the
    // element chosen on the other half of the screen otherwise.
    const float margin = font * 0.5f;
    float width = std::min(io.DisplaySize.x * 0.62f, font * 26.0f);
    ImVec2 at{io.DisplaySize.x * 0.5f, st.panel_place == 1 ? io.DisplaySize.y - margin : margin};
    ImVec2 pivot{0.5f, st.panel_place == 1 ? 1.0f : 0.0f};
    if (st.panel_place >= 2) {
        width = std::min(io.DisplaySize.x * 0.46f, font * 22.0f);
        at = {st.panel_place == 2 ? margin : io.DisplaySize.x - margin, io.DisplaySize.y * 0.5f};
        pivot = {st.panel_place == 2 ? 0.0f : 1.0f, 0.5f};
    }
    if (st.selected) {
        const input::touch::Placed p = placed(*st.selected);
        if (!st.adaptive || !st.dragging) st.element_right = p.centre.x > io.DisplaySize.x * 0.5f;
        const bool element_right = st.element_right;
        width = std::min(io.DisplaySize.x * 0.46f, font * 22.0f);
        at = {element_right ? margin : io.DisplaySize.x - margin, margin};
        pivot = {element_right ? 0.0f : 1.0f, 0.0f};
    }
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, io.DisplaySize.y - 2.0f * margin});
    const bool moving = st.adaptive && st.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (moving ? .35f : 1.f));
    ImGui::Begin("##touch_editor", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize | (moving ? ImGuiWindowFlags_NoInputs : 0));
    const auto preview_row = [&] {
        if (!st.adaptive || st.adaptive_mode != input::touch::AdaptiveMode::Quest) return;
        heading("Weapon Preview:");
        if (const int delta = choice_row("##Weapon Preview", adaptive_profile_name(st.preview),
                {false, {}, "Preview only. Does not change equipped gear or saved button positions."})) {
            using P = input::touch::AttackProfile;
            constexpr P profiles[] = {P::LongSword, P::GreatSword, P::SwordAndShield, P::DualBlades, P::Hammer, P::Bow};
            int selected = 0;
            for (int i = 0; i < 6; ++i)
                if (profiles[i] == st.preview) selected = i;
            st.preview = profiles[cycle(selected, delta, 6)];
        }
    };
    bool done = back;
    if (st.selected) {
        const std::size_t i = *st.selected;
        const auto e = static_cast<Element>(i);
        const auto psp = static_cast<input::touch::PspElement>(i);
        input::touch::Placement &p = placement(i);
        heading(st.psp        ? input::touch::info(psp).name
                : st.adaptive ? input::touch::info(static_cast<A>(i)).name
                              : input::touch::info(e).name);
        preview_row();
        if (st.adaptive && st.adaptive_mode == input::touch::AdaptiveMode::Quest) {
            const bool custom = input::touch::rebindable(static_cast<A>(i)) && p.buttons != normal(i).buttons;
            const char *help = custom
                ? "Custom binding: this control sends the selected buttons without weapon gestures."
                : adaptive_gesture_help(static_cast<A>(i), st.preview);
            if (*help) paragraph(help, colors::kTextDim);
        }
        if (st.psp ? psp == input::touch::PspElement::R3 : st.adaptive ? static_cast<A>(i) == A::R3 : e == Element::R3)
            paragraph("Uses your gamepad R3 assignment (Lock-On by default).", colors::kTextDim);
        if (st.psp            ? input::touch::rebindable(psp)
                : st.adaptive ? input::touch::rebindable(static_cast<A>(i))
                              : input::touch::rebindable(e)) {
            const auto &choices = input::touch::button_choices();
            int current = 0;
            for (std::size_t i = 0; i < choices.size(); ++i)
                if (choices[i].buttons == p.buttons) current = static_cast<int>(i);
            if (const int delta = choice_row("Presses", input::touch::buttons_label(p.buttons),
                    {false, {}, "The PSP button, or two pressed in the same frame."})) {
                p.buttons =
                    choices[static_cast<std::size_t>(cycle(current, delta, static_cast<int>(choices.size())))].buttons;
                settings::save();
            }
            if (st.psp && input::touch::is_combo(psp)) {
                if (button_row("Choose Buttons…", {false, {}, "Choose any PSP buttons to hold together."}))
                    st.choosing_buttons = !st.choosing_buttons;
                if (st.choosing_buttons) {
                    for (std::size_t button = 0; button < 12u; ++button) {
                        const auto &choice = choices[button];
                        const std::string label =
                            std::string(choice.label) + "##touch_button_" + std::to_string(choice.buttons);
                        if (toggle_row(label.c_str(), (p.buttons & choice.buttons) != 0u)) {
                            p.buttons ^= choice.buttons;
                            settings::save();
                        }
                    }
                    if (button_row("Done Choosing")) st.choosing_buttons = false;
                }
            }
        }
        if (st.adaptive && static_cast<A>(i) == A::ItemBar) {
            paragraph(linked_items(i)
                    ? "Tap the game item HUD to use an item; swipe left/right to select. Its position and size follow HUD Settings. The values here affect only the fallback strip."
                    : "Tap to use; swipe left/right to select. In a quest this follows the visible item HUD; this strip is used when the HUD is absent or hidden.",
                colors::kTextDim);
            if (button_row("Edit HUD Layout...",
                    {!gpu::hud::available() || !gpu::hud::layout_enabled() || gpu::hud::hidden(), {},
                        "Move or resize the game item block in HUD Settings."})) {
                st.open = false;
                open_hud_editor();
            }
        }

        const float default_size = normal(i).size;
        int size = static_cast<int>(std::lround(p.size / default_size * 100.0f));
        const RowOptions size_options{
            linked_items(i), linked_items(i) ? "Use HUD Settings" : "", "Of the element's own default size."};
        if (slider_row("Size", size, 50, 250, 5, "%d%%", size_options)) {
            p.size = std::clamp(default_size * static_cast<float>(size) / 100.0f, input::touch::kMinElementSize,
                input::touch::kMaxElementSize);
            settings::save();
        }
        RowOptions shown{false, {}, "A hidden element is not drawn and does not take fingers."};
        if (st.psp ? psp == input::touch::PspElement::Menu
                : st.adaptive
                ? (static_cast<A>(i) == A::Pause || static_cast<A>(i) == A::InterfaceToggle ||
                      (st.adaptive_mode == input::touch::AdaptiveMode::Interface && static_cast<A>(i) == A::DPad))
                : e == Element::Pause) {
            shown.disabled = true;
            shown.note = "Required for navigation";
        }
        if ((st.psp && psp == input::touch::PspElement::Stick) || (st.adaptive && static_cast<A>(i) == A::Stick)) {
            bool &floating = st.adaptive ? adaptive_settings.floating_stick : s.touch_psp.floating_stick;
            if (toggle_row("Floating Stick", floating,
                    {false, {}, "On: appears under the left thumb. Off: stays at its edited position."})) {
                floating = !floating;
                settings::save();
            }
        }
        const bool dpad = st.psp && psp == input::touch::PspElement::DPad;
        const bool visible = p.shown && (!dpad || s.touch_dpad);
        if (toggle_row("Shown", visible, shown)) {
            p.shown = !visible;
            if (dpad) s.touch_dpad = p.shown;
            settings::save();
        }
        if (button_row("Reset this element", {false, {}, "Back to where and what it was at first."})) {
            p = normal(i);
            if (dpad) s.touch_dpad = true;
            if (st.psp && input::touch::is_combo(psp)) {
                p.buttons = 0x2200u;
                p.shown = true;
            }
            settings::save();
        }
        if (st.psp && input::touch::is_combo(psp) && button_row("Remove Combination")) {
            input::touch::remove_psp_combo(s.touch_psp, psp);
            st.selected.reset();
            settings::save();
        }
        if (button_row("Choose another", {false, {}, "Or tap another element."})) st.selected.reset();
    } else {
        heading(st.psp ? "PSP Touch Layout:" : st.adaptive ? "Adaptive Touch Layout:" : "Touch layout");
        paragraph("Drag an element to move it; tap one to resize, rebind or hide it.", colors::kTextDim);
        preview_row();
        if (st.adaptive) {
            static constexpr const char *modes[] = {"Quest", "Village", "Interfaces"};
            if (const int delta = choice_row("Layout", modes[static_cast<int>(st.adaptive_mode)])) {
                st.adaptive_mode =
                    static_cast<input::touch::AdaptiveMode>(cycle(static_cast<int>(st.adaptive_mode), delta, 3));
                // The selected profile reference belongs to this frame; render
                // the new profile on the next frame before accepting edits.
                ImGui::End();
                ImGui::PopStyleVar();
                return;
            }
        }
        if (st.adaptive && toggle_row("Floating Stick", adaptive_settings.floating_stick)) {
            adaptive_settings.floating_stick = !adaptive_settings.floating_stick;
            settings::save();
        }
        if (st.psp) {
            if (toggle_row("Floating Stick", s.touch_psp.floating_stick,
                    {false, {}, "On: appears under the left thumb. Off: stays at the position shown in the editor."})) {
                s.touch_psp.floating_stick = !s.touch_psp.floating_stick;
                settings::save();
            }
            bool full = true;
            for (std::size_t i = static_cast<std::size_t>(input::touch::PspElement::Combo1);
                i <= static_cast<std::size_t>(input::touch::PspElement::Combo8); ++i)
                full = full && s.touch_psp.elements[i] != normal(i);
            if (button_row("Add Combination",
                    {full, full ? "Eight buttons already added" : "",
                        "Add a button that holds a PSP button combination, such as R + Circle."})) {
                if (const auto added = input::touch::add_psp_combo(s.touch_psp))
                    st.selected = static_cast<std::size_t>(*added);
                settings::save();
            }
        }
        int opacity = static_cast<int>(std::lround(s.touch_opacity * 100.0f));
        if (slider_row("Opacity", opacity, 10, 100, 5, "%d%%", {false, {}, "How strongly the controls are drawn."})) {
            s.touch_opacity = static_cast<float>(opacity) / 100.0f;
            settings::save();
        }
        int all = static_cast<int>(std::lround(s.touch_size * 100.0f));
        if (slider_row(st.psp || st.adaptive ? "Size" : "Size of all", all, 60, 160, 5, "%d%%",
                {false, {}, "Every element together."})) {
            s.touch_size = static_cast<float>(all) / 100.0f;
            settings::save();
        }
        if (toggle_row("Haptic Feedback", s.touch_haptics,
                {false, {},
                    "A short vibration when a button is pressed or a swipe is taken, where the "
                    "device can (the system's touch feedback setting applies)."})) {
            s.touch_haptics = !s.touch_haptics;
            settings::save();
        }
        static constexpr const char *positions[] = {"Top", "Bottom", "Left", "Right"};
        if (const int delta = choice_row("Panel", positions[st.panel_place], {false, {}, "Where this panel sits."}))
            st.panel_place = cycle(st.panel_place, delta, st.psp || st.adaptive ? 4 : 2);
        if (button_row(
                "Reset the whole layout", {false, {}, "Every element back to where and what it was at first."})) {
            if (st.psp) {
                s.touch_psp = input::touch::default_psp_layout();
                s.touch_dpad = true;
            } else if (st.adaptive)
                adaptive_settings = input::touch::default_adaptive_layout(st.adaptive_mode);
            else
                s.touch_action = input::touch::default_action_layout();
            settings::save();
        }
    }
    if (button_row("Done", {false, {}, "Back to the menu."})) done = true;
    ImGui::End();
    ImGui::PopStyleVar();
    if (done) {
        st = State{};
        settings::save();
    }
}

} // namespace mhp3rd::ui
