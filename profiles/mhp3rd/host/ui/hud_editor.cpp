#include "ui/hud_editor.hpp"

#include "ui/layer.hpp"
#include "ui/widgets.hpp"
#include "gpu/game_hud.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "settings/settings.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace mhp3rd::ui {
namespace {

namespace hud = gpu::hud;
struct State {
    bool open{};
    std::size_t selected{};
    bool dragging{};
    bool moving{};
    bool confirm_armed{};
    hud::Placement before_move{};
    bool panel_left{};
    bool place_panel{true};
    bool focus_options{true};
    bool focus_move{};
    InputDevice last_device{InputDevice::Keyboard};
    ImVec2 grab{};
};
State &state() {
    static State s;
    return s;
}

float held(std::initializer_list<ImGuiKey> keys) {
    float value = 0.0f;
    for (const ImGuiKey key : keys)
        if (ImGui::IsKeyDown(key)) value = std::max(value, ImGui::GetKeyData(key)->AnalogValue);
    return value;
}
bool gamepad_down() {
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) return true;
    return false;
}

} // namespace

void open_hud_editor() {
    state() = {};
    state().open = true;
    focus_next_row();
}
bool hud_editor_open() {
    return state().open;
}
void hud_editor_frame(bool back) {
    State &st = state();
    auto &layout = settings::current().hud;
    auto &layer = Layer::get();
    const ImGuiIO &io = ImGui::GetIO();
    const auto &renderer = layer.renderer();
    const auto picture = renderer.game_picture();
    const ImVec2 origin{picture[0] * io.DisplaySize.x, picture[1] * io.DisplaySize.y};
    const ImVec2 size{picture[2] * io.DisplaySize.x, picture[3] * io.DisplaySize.y};
    float fit_x = 1.0f, fit_y = 1.0f;
    if (settings::current().aspect == settings::Aspect::Fill) {
        const auto target = renderer.target_size();
        const float x = static_cast<float>(target[0]) / 480.0f;
        const float y = static_cast<float>(target[1]) / 272.0f;
        fit_x = std::min(x, y) / std::max(x, 1.0f);
        fit_y = std::min(x, y) / std::max(y, 1.0f);
    }
    const auto placed = [&](std::size_t i) {
        const hud::Rect r = hud::bounds(layout, hud::kParts[i], fit_x, fit_y);
        return hud::Rect{origin.x + r.left / 480.0f * size.x, origin.y + r.top / 272.0f * size.y,
            origin.x + r.right / 480.0f * size.x, origin.y + r.bottom / 272.0f * size.y};
    };
    const bool gamepad = layer.gamepad_armed() &&
        (gamepad_down() || (layer.input_device() == InputDevice::Gamepad && st.last_device != InputDevice::Gamepad));
    st.last_device = layer.input_device();
    if (gamepad && !st.moving && st.dragging) {
        st.dragging = false;
        st.place_panel = true;
        settings::save();
    }

    // Confirmation must be released after entering the mode. A held button
    // that opened Move Block cannot also finish it.
    const bool was_moving = st.moving;
    if (st.moving) {
        const ImGuiKey confirm = layer.confirm_south() ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
        const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
        const bool confirming = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
            (layer.gamepad_armed() && ImGui::IsKeyPressed(confirm, false));
        const bool cancelled = back || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            (layer.gamepad_armed() && ImGui::IsKeyPressed(cancel, false));
        if (cancelled || (st.confirm_armed && confirming)) {
            if (cancelled) layout.blocks[st.selected] = st.before_move;
            st.moving = false;
            st.place_panel = true;
            st.focus_options = st.focus_move = true;
            settings::save();
        } else {
            const float x =
                held({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}) -
                held({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft});
            const float y =
                held({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}) -
                held({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp});
            hud::move_by(layout, hud::kParts[st.selected], x, y, io.DeltaTime, fit_x, fit_y);
        }
        if (!ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
            !ImGui::IsKeyDown(ImGuiKey_Space) && !ImGui::IsKeyDown(confirm))
            st.confirm_armed = true;
        back = false; // Back cancels a move before it closes the editor.
    }

    // Pointer/touch cannot select or drag anything during Move Block, including
    // its confirmation frame. Outside that mode the normal direct drag remains.
    if (!was_moving && !gamepad && !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float smallest = 0.0f;
        st.dragging = false;
        for (std::size_t i = 0; i < hud::kParts.size(); ++i) {
            const auto r = placed(i);
            const float area = (r.right - r.left) * (r.bottom - r.top);
            if (io.MousePos.x >= r.left && io.MousePos.x <= r.right && io.MousePos.y >= r.top &&
                io.MousePos.y <= r.bottom && (!st.dragging || area < smallest)) {
                if (st.selected != i) st.place_panel = true;
                st.selected = i;
                smallest = area;
                st.grab = {io.MousePos.x - r.left, io.MousePos.y - r.top};
                st.dragging = true;
            }
        }
    }
    if (st.dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            size.x > 0.0f && size.y > 0.0f) {
            hud::move_to(layout, hud::kParts[st.selected], (io.MousePos.x - st.grab.x - origin.x) / size.x * 480.0f,
                (io.MousePos.y - st.grab.y - origin.y) / size.y * 272.0f, fit_x, fit_y);
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            st.dragging = false;
            st.place_panel = true;
            settings::save();
        }
    }

    const float font = layer.font_size();
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    constexpr ImU32 colors_by_part[]{colors::kDanger, colors::kGood, colors::kAccent};
    for (std::size_t i = 0; i < hud::kParts.size(); ++i) {
        const auto r = placed(i);
        draw->AddRect({r.left, r.top}, {r.right, r.bottom}, colors_by_part[i], 0.0f, 0, i == st.selected ? 3.0f : 1.0f);
        const char *label = hud::name(hud::kParts[i]);
        const ImVec2 text = ImGui::CalcTextSize(label);
        const ImVec2 at{std::max(r.left, origin.x) + 5.0f, std::max(r.top, origin.y) + 4.0f};
        draw->AddRectFilled({at.x - 3.0f, at.y}, {at.x + text.x + 3.0f, at.y + text.y}, colors::kPanel);
        draw->AddText(at, colors_by_part[i], label);
    }
    draw->PopClipRect();

    // A held slider or movement operation never relocates the options panel.
    if (st.place_panel && !st.dragging && !was_moving && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemActive()) {
        const auto r = placed(st.selected);
        st.panel_left = r.left + r.right > io.DisplaySize.x;
        st.place_panel = false;
    }
    const bool moving = st.moving || (st.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    const bool blocked = moving || was_moving;
    const RowOptions panel_rows{blocked, {}, {}};
    const float margin = font * 0.5f;
    const float width = std::min(io.DisplaySize.x * 0.45f, font * 22.0f);
    ImGui::SetNextWindowPos({st.panel_left ? margin : io.DisplaySize.x - margin, margin}, ImGuiCond_Always,
        {st.panel_left ? 0.0f : 1.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, io.DisplaySize.y - 2.0f * margin});
    ImGui::Begin("##hud_editor", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize | (blocked ? ImGuiWindowFlags_NoInputs : 0));
    if (!blocked && (gamepad || st.focus_options)) {
        auto *window = ImGui::GetCurrentWindow();
        if (st.focus_options || ImGui::GetCurrentContext()->NavWindow != window) {
            ImGui::FocusWindow(window);
            ImGui::SetFocusID(window->GetID(st.focus_move ? "Move Block" : "Block"), window);
        }
        ImGui::SetNavCursorVisible(true);
        st.focus_options = false;
    }
    heading("HUD Layout:");
    paragraph(st.moving
            ? "Move with the D-pad, left stick, arrows or WASD. Confirm to keep the position; Back or Esc cancels."
            : "Drag a block with the mouse or touch, or choose Move Block for keyboard/gamepad positioning.",
        colors::kTextDim);
    if (!hud::quest_visible()) paragraph("Enter a quest to preview the game's HUD.", colors::kTextDim);
    if (const int delta = choice_row("Block", hud::name(hud::kParts[st.selected]), panel_rows)) {
        st.selected = static_cast<std::size_t>((static_cast<int>(st.selected) + delta + 3) % 3);
        st.dragging = false;
        st.place_panel = true;
    }
    if (button_row("Move Block", panel_rows)) {
        st.before_move = layout.blocks[st.selected];
        st.moving = true;
        st.confirm_armed = false;
        st.dragging = false;
    }
    auto &p = layout.blocks[st.selected];
    int scale = static_cast<int>(std::lround(p.scale * 100.0f));
    if (slider_row("Block Scale", scale, 50, 200, 5, "%d%%", panel_rows)) {
        p.scale = static_cast<float>(scale) / 100.0f;
        settings::save();
    }
    if (button_row("Reset This Block", panel_rows)) {
        p = {};
        settings::save();
    }
    if (button_row("Restore HUD Defaults", panel_rows)) {
        layout = {};
        settings::save();
    }
    bool done = back;
    if (layer.input_device() == InputDevice::Touch) {
        if (big_button("Back##hud_editor_back", ImGui::GetContentRegionAvail().x, false, panel_rows.disabled))
            layer.request_back();
    } else
        done = button_row("Done", panel_rows) || done;
    // Custom rows use explicit colors, so fade every vertex of the panel.
    if (moving || st.moving)
        for (ImDrawVert &vertex : ImGui::GetWindowDrawList()->VtxBuffer) {
            const ImU32 alpha = (vertex.col >> IM_COL32_A_SHIFT) & 255u;
            vertex.col = (vertex.col & ~IM_COL32_A_MASK) | ((alpha / 4u) << IM_COL32_A_SHIFT);
        }
    ImGui::End();
    if (done) {
        state() = {};
        settings::save();
    }
}

} // namespace mhp3rd::ui