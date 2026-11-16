#include "ui/combat_overlay.hpp"
#include "ui/layer.hpp"
#include "ui/combat_overlay_layout.hpp"
#include "ui/widgets.hpp"
#include "game/control_context.hpp"
#include "game/damage_hook.hpp"
#include "game/damage_numbers.hpp"
#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#include "game/monster_state.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "settings/settings.hpp"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>

namespace mhp3rd::ui {
namespace {
struct Editor {
    bool open{}, dragging{}, focus{}, panel_left{};
    ImVec2 grab{};
};
Editor editor;
combat_layout::Stack placement(std::size_t count) {
    const auto picture = Layer::get().renderer().game_picture();
    const auto screen = ImGui::GetIO().DisplaySize;
    const auto &s = settings::current();
    return combat_layout::place(
        {picture[0] * screen.x, picture[1] * screen.y, picture[2] * screen.x, picture[3] * screen.y},
        Layer::get().font_size() * 0.65f, count, s.monster_hp_x, s.monster_hp_y, s.monster_hp_scale);
}
void draw_bars(
    const game::Ram &ram, const std::vector<game::combat::Monster> &monsters, const combat_layout::Stack &stack) {
    if (stack.font <= 0.f) return;
    auto *draw = ImGui::GetBackgroundDrawList();
    const auto &bounds = stack.bounds;
    const float x = bounds.x, width = bounds.width, size = stack.font, padding = size * 0.4f;
    float y = bounds.y;
    for (const auto &monster : monsters) {
        std::string name = monster.kind < 75u ? game::text_entry(ram, game::kTextBlock, 2, 308u + monster.kind) : "";
        if (name.empty()) name = "Monster";
        char health[48];
        std::snprintf(health, sizeof(health), "%d / %d", monster.health, monster.maximum);
        draw->AddRectFilled({x, y}, {x + width, y + stack.row}, colors::kBackdrop, padding);
        const float health_width = ImGui::GetFont()->CalcTextSizeA(size, width, 0.f, health).x;
        draw->PushClipRect({x + padding, y}, {x + width - padding - health_width - padding, y + size * 1.4f}, true);
        draw->AddText(ImGui::GetFont(), size, {x + padding, y + padding * 0.5f}, colors::kText, name.c_str());
        draw->PopClipRect();
        draw->AddText(
            ImGui::GetFont(), size, {x + width - padding - health_width, y + padding * 0.5f}, colors::kText, health);
        const float top = y + stack.row - padding * 1.3f, track = width - 2.f * padding;
        draw->AddRectFilled(
            {x + padding, top}, {x + width - padding, top + padding * 0.7f}, colors::kTextDisabled, 2.f);
        draw->AddRectFilled({x + padding, top},
            {x + padding + track * static_cast<float>(monster.health) / std::max(1, monster.maximum),
                top + padding * 0.7f},
            colors::kAccent, 2.f);
        y += stack.row + stack.gap;
    }
}
} // namespace

void draw_combat_overlay() {
    const auto context = game::control_context::current();
    if (context.scene != game::control_context::Scene::Quest || context.interface_open) return;
    game::read([](const game::Ram &ram) {
        const auto picture = Layer::get().renderer().game_picture();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float size = Layer::get().font_size() * 0.65f;
        auto *draw = ImGui::GetBackgroundDrawList();
        if (settings::current().damage_numbers) {
            const auto now = game::combat::damage_clock_us();
            const ImVec2 clip_min{picture[0] * screen.x, picture[1] * screen.y};
            const ImVec2 clip_max{(picture[0] + picture[2]) * screen.x, (picture[1] + picture[3]) * screen.y};
            draw->PushClipRect(clip_min, clip_max, true);
            for (const auto &number : game::combat::damage_history().numbers()) {
                if (now < number.born || now - number.born >= game::combat::kNumberLifetimeUs) continue;
                const auto at = game::combat::project_damage(ram, number.position);
                if (!at) continue;
                const float age = static_cast<float>(now - number.born) / game::combat::kNumberLifetimeUs;
                const float font = size * 1.35f;
                char text[16];
                std::snprintf(text, sizeof(text), "%d", number.amount);
                const float text_width = ImGui::GetFont()->CalcTextSizeA(font, 10000.f, 0.f, text).x;
                const ImVec2 point{(picture[0] + (*at)[0] * picture[2]) * screen.x - text_width * 0.5f +
                        (static_cast<int>(number.lane) - 2) * font * 0.65f,
                    (picture[1] + (*at)[1] * picture[3]) * screen.y - age * font * 2.5f};
                const int alpha = static_cast<int>(255.f * std::min(1.f, (1.f - age) * 4.f));
                for (const ImVec2 offset : {ImVec2{-1, -1}, ImVec2{1, -1}, ImVec2{-1, 1}, ImVec2{1, 1}})
                    draw->AddText(ImGui::GetFont(), font, {point.x + offset.x, point.y + offset.y},
                        IM_COL32(10, 5, 0, alpha), text);
                draw->AddText(ImGui::GetFont(), font, point, IM_COL32(251, 230, 166, alpha), text);
            }
            draw->PopClipRect();
        }
        if (settings::current().monster_hp == settings::MonsterHp::Off) return;
        auto monsters = game::combat::visible_monsters(
            game::combat::read_monsters(ram), settings::current().monster_hp == settings::MonsterHp::Large);
        draw_bars(ram, monsters, placement(monsters.size()));
    });
}
} // namespace mhp3rd::ui

namespace mhp3rd::ui {
void open_hp_layout_editor() {
    editor = {};
    editor.open = editor.focus = true;
    editor.panel_left = settings::current().monster_hp_x > 0.5f;
    focus_next_row();
}
bool hp_layout_editor_open() {
    return editor.open;
}
void hp_layout_editor_frame(bool back) {
    auto &s = settings::current();
    auto &layer = Layer::get();
    auto &io = ImGui::GetIO();
    std::vector<game::combat::Monster> monsters;
    game::read([&](const game::Ram &ram) {
        if (const auto context = game::control_context::current();
            context.scene == game::control_context::Scene::Quest && !context.interface_open)
            monsters = game::combat::visible_monsters(
                game::combat::read_monsters(ram), s.monster_hp != settings::MonsterHp::All);
    });
    const bool sample = monsters.empty();
    if (sample) monsters = {{0u, 255u, 700, 1000}, {0u, 255u, 400, 800}, {0u, 255u, 120, 500}};
    auto stack = placement(monsters.size());
    const auto &b = stack.bounds;
    if (!io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.x >= b.x &&
        io.MousePos.x <= b.x + b.width && io.MousePos.y >= b.y && io.MousePos.y <= b.y + b.height) {
        editor.dragging = true;
        editor.grab = {io.MousePos.x - b.x, io.MousePos.y - b.y};
    }
    if (editor.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            editor.dragging = false;
            settings::save();
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const auto at = combat_layout::move_to(stack, io.MousePos.x - editor.grab.x, io.MousePos.y - editor.grab.y);
            s.monster_hp_x = at[0];
            s.monster_hp_y = at[1];
            stack = placement(monsters.size());
        }
    }
    const auto paint = [&](const game::Ram &ram) { draw_bars(ram, monsters, stack); };
    if (!game::read(paint)) {
        game::BufferRam empty(0, 0);
        paint(empty);
    }
    const auto &bounds = stack.bounds;
    ImGui::GetBackgroundDrawList()->AddRect(
        {bounds.x, bounds.y}, {bounds.x + bounds.width, bounds.y + bounds.height}, colors::kAccent, 0.f, 0, 2.f);
    const float font = layer.font_size(), margin = font * 0.5f;
    const bool dragging = editor.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    const RowOptions rows{dragging, {}, {}};
    const float width = std::min(io.DisplaySize.x * 0.45f, font * 23.f);
    ImGui::SetNextWindowPos({editor.panel_left ? margin : io.DisplaySize.x - margin, margin}, ImGuiCond_Always,
        {editor.panel_left ? 0.f : 1.f, 0.f});
    ImGui::SetNextWindowSizeConstraints({width, 0.f}, {width, io.DisplaySize.y - 2.f * margin});
    ImGui::Begin("##hp_layout_editor", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize | (dragging ? ImGuiWindowFlags_NoInputs : 0));
    if (editor.focus) {
        focus_next_row();
        editor.focus = false;
    }
    heading("HP Panel Layout:");
    paragraph(
        "Drag the panels with the mouse or touch. Presets and fine adjustments also work with keyboard or gamepad.",
        colors::kTextDim);
    if (sample) paragraph("Sample panels. Enter a quest for a live preview.", colors::kTextDim);
    const int preset = combat_layout::preset(s.monster_hp_x, s.monster_hp_y);
    if (const int delta = choice_row("Position", preset < 0 ? "Custom" : combat_layout::kNames[preset], rows)) {
        const int next = (preset < 0 ? (delta > 0 ? 0 : 5) : (preset + delta + 6) % 6);
        s.monster_hp_x = combat_layout::kPresets[next][0];
        s.monster_hp_y = combat_layout::kPresets[next][1];
        settings::save();
    }
    int scale = static_cast<int>(std::lround(s.monster_hp_scale * 100.f));
    if (slider_row("Panel Scale", scale, 50, 200, 5, "%d%%",
            {dragging, {},
                "Scales the HP panels, text and bars together. Large groups automatically fit inside the game picture."})) {
        s.monster_hp_scale = static_cast<float>(scale) / 100.f;
        settings::save();
    }
    int x = static_cast<int>(std::lround(s.monster_hp_x * 100.f));
    int y = static_cast<int>(std::lround(s.monster_hp_y * 100.f));
    if (slider_row("Horizontal Position", x, 0, 100, 1, "%d%%", rows)) {
        s.monster_hp_x = static_cast<float>(x) / 100.f;
        settings::save();
    }
    if (slider_row("Vertical Position", y, 0, 100, 1, "%d%%", rows)) {
        s.monster_hp_y = static_cast<float>(y) / 100.f;
        settings::save();
    }
    if (button_row("Restore Panel Defaults", rows)) {
        s.monster_hp_x = settings::defaults().monster_hp_x;
        s.monster_hp_y = settings::defaults().monster_hp_y;
        s.monster_hp_scale = settings::defaults().monster_hp_scale;
        settings::save();
    }
    bool done = back;
    if (layer.input_device() == InputDevice::Touch) {
        done = big_button("Back##hp_layout_back", ImGui::GetContentRegionAvail().x, false, dragging) || done;
    } else
        done = button_row("Done", rows) || done;
    if (dragging)
        for (ImDrawVert &vertex : ImGui::GetWindowDrawList()->VtxBuffer) {
            const ImU32 alpha = (vertex.col >> IM_COL32_A_SHIFT) & 255u;
            vertex.col = (vertex.col & ~IM_COL32_A_MASK) | ((alpha / 4u) << IM_COL32_A_SHIFT);
        }
    ImGui::End();
    if (done) {
        editor = {};
        settings::save();
    }
}
}
