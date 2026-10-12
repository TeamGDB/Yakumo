#include "ui/touch_editor.hpp"

#include "ui/layer.hpp"
#include "ui/touch_overlay.hpp"
#include "ui/widgets.hpp"

#include "gpu/vulkan_renderer.hpp"
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
    std::optional<std::size_t> selected;
    bool dragging{};
    bool choosing_buttons{};
    ImVec2 grab{};     // the pointer's offset from the element's centre
    int panel_place{}; // top, bottom, left or right
};

State &state() {
    static State value;
    return value;
}

// The element under `at`, the smallest when they overlap, hidden ones
// included: the editor is where they are shown again.
std::optional<Element> element_at(
    const input::touch::ActionLayout &layout, const input::touch::Area &area, float scale, ImVec2 at) {
    std::optional<Element> best;
    float best_size = 0.0f;
    for (std::size_t i = 0; i < input::touch::kElements; ++i) {
        const auto e = static_cast<Element>(i);
        const input::touch::Placed p = input::touch::place(layout.elements[i], e, area, scale);
        const bool inside = e == Element::Swipe
            ? std::fabs(at.x - p.centre.x) <= p.half_width && std::fabs(at.y - p.centre.y) <= p.radius
            : std::hypot(at.x - p.centre.x, at.y - p.centre.y) <= std::max(p.radius * 1.2f, 12.0f);
        const float size = p.radius * p.half_width;
        if (inside && (!best || size < best_size)) {
            best = e;
            best_size = size;
        }
    }
    return best;
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
    if (s.psp) s.panel_place = 2;
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
    const std::size_t count = st.psp ? input::touch::kPspElements : input::touch::kElements;
    const auto placement = [&](std::size_t i) -> input::touch::Placement & {
        return st.psp ? s.touch_psp.elements[i] : s.touch_action.elements[i];
    };
    const auto normal = [&](std::size_t i) -> const input::touch::Placement & {
        return st.psp ? input::touch::default_psp_layout().elements[i]
                      : input::touch::default_action_layout().elements[i];
    };
    const auto placed = [&](std::size_t i) {
        if (st.psp) {
            const auto circle = input::touch::psp_element_circle(
                Layer::get().renderer().touch_controls().layout(), static_cast<input::touch::PspElement>(i));
            return input::touch::Placed{circle.centre, circle.radius, circle.radius};
        }
        return input::touch::place(placement(i), static_cast<Element>(i), area, scale);
    };

    // The game, dimmed a little, and the layout over it.
    ImDrawList *list = ImGui::GetBackgroundDrawList();
    list->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(0, 0, 0, 90));
    for (std::size_t i = 0; i < count; ++i) {
        if (st.psp && input::touch::is_combo(static_cast<input::touch::PspElement>(i)) && placement(i) == normal(i))
            continue;
        const auto e = static_cast<Element>(i);
        ElementLook look;
        look.opacity = std::max(s.touch_opacity, 0.6f);
        look.hidden = !placement(i).shown;
        if (st.psp && i == static_cast<std::size_t>(input::touch::PspElement::DPad))
            look.hidden = look.hidden || !s.touch_dpad;
        look.selected = st.selected == i;
        look.held = st.dragging && st.selected == i;
        if (st.psp)
            draw_psp_element(list, static_cast<input::touch::PspElement>(i),
                Layer::get().renderer().touch_controls().layout(), look);
        else
            draw_action_element(list, e, placement(i), placed(i), {}, look);
    }

    // Choosing and dragging, where the panel is not.
    if (!io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.choosing_buttons = false;
        st.selected.reset();
        if (st.psp) {
            float best = 0.0f;
            for (std::size_t i = 0; i < count; ++i) {
                if (input::touch::is_combo(static_cast<input::touch::PspElement>(i)) && placement(i) == normal(i))
                    continue;
                const auto p = placed(i);
                if (std::hypot(io.MousePos.x - p.centre.x, io.MousePos.y - p.centre.y) <=
                        std::max(p.radius * 1.2f, 12.0f) &&
                    (!st.selected || p.radius < best)) {
                    st.selected = i;
                    best = p.radius;
                }
            }
        } else if (const auto hit = element_at(s.touch_action, area, scale, io.MousePos)) {
            st.selected = static_cast<std::size_t>(*hit);
        }
        st.dragging = st.selected.has_value();
        if (st.selected) {
            const input::touch::Placed p = placed(*st.selected);
            st.grab = {io.MousePos.x - p.centre.x, io.MousePos.y - p.centre.y};
        }
    }
    if (st.dragging && st.selected) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const input::touch::Point centre{io.MousePos.x - st.grab.x, io.MousePos.y - st.grab.y};
            auto &p = placement(*st.selected);
            p = input::touch::move_to(p, st.psp ? Element::Stick : static_cast<Element>(*st.selected), centre, area);
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
        const bool element_right = p.centre.x > io.DisplaySize.x * 0.5f;
        width = std::min(io.DisplaySize.x * 0.46f, font * 22.0f);
        at = {element_right ? margin : io.DisplaySize.x - margin, margin};
        pivot = {element_right ? 0.0f : 1.0f, 0.0f};
    }
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, io.DisplaySize.y - 2.0f * margin});
    ImGui::Begin("##touch_editor", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);
    bool done = back;
    if (st.selected) {
        const std::size_t i = *st.selected;
        const auto e = static_cast<Element>(i);
        const auto psp = static_cast<input::touch::PspElement>(i);
        input::touch::Placement &p = placement(i);
        heading(st.psp ? input::touch::info(psp).name : input::touch::info(e).name);
        if (st.psp ? psp == input::touch::PspElement::R3 : e == Element::R3)
            paragraph("Uses your gamepad R3 assignment (Lock-On by default).", colors::kTextDim);
        if (st.psp ? input::touch::rebindable(psp) : input::touch::rebindable(e)) {
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
        const float default_size = normal(i).size;
        int size = static_cast<int>(std::lround(p.size / default_size * 100.0f));
        if (slider_row("Size", size, 50, 250, 5, "%d%%", {false, {}, "Of the element's own default size."})) {
            p.size = std::clamp(default_size * static_cast<float>(size) / 100.0f, input::touch::kMinElementSize,
                input::touch::kMaxElementSize);
            settings::save();
        }
        RowOptions shown{false, {}, "A hidden element is not drawn and does not take fingers."};
        if (st.psp ? psp == input::touch::PspElement::Menu : e == Element::Pause) {
            shown.disabled = true;
            shown.note = "It opens this menu";
        }
        if (st.psp && psp == input::touch::PspElement::Stick &&
            toggle_row("Floating Stick", s.touch_psp.floating_stick,
                {false, {}, "On: appears under the left thumb. Off: stays where it is placed in this editor."})) {
            s.touch_psp.floating_stick = !s.touch_psp.floating_stick;
            settings::save();
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
        heading(st.psp ? "PSP Touch Layout:" : "Touch layout");
        paragraph("Drag an element to move it; tap one to resize, rebind or hide it.", colors::kTextDim);
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
        if (slider_row(
                st.psp ? "Size" : "Size of all", all, 60, 160, 5, "%d%%", {false, {}, "Every element together."})) {
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
            st.panel_place = cycle(st.panel_place, delta, st.psp ? 4 : 2);
        if (button_row(
                "Reset the whole layout", {false, {}, "Every element back to where and what it was at first."})) {
            if (st.psp) {
                s.touch_psp = input::touch::default_psp_layout();
                s.touch_dpad = true;
            } else
                s.touch_action = input::touch::default_action_layout();
            settings::save();
        }
    }
    if (button_row("Done", {false, {}, "Back to the menu."})) done = true;
    ImGui::End();
    if (done) {
        st = State{};
        settings::save();
    }
}

} // namespace mhp3rd::ui
