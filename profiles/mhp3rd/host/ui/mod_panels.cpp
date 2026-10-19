#include "ui/mod_panels.hpp"

#include "ui/layer.hpp"
#include "ui/text_input.hpp"
#include "ui/widgets.hpp"

#include "gpu/vulkan_renderer.hpp"
#include "mods/mod_native.hpp"

#include "yakumo/mod.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>

namespace mhp3rd::ui {
namespace {

// Panels draw with the menu's rows; overlays with plain widgets in a window
// that takes no input, and may draw on the game's picture.
bool g_overlay = false;

// Text typed on the keyboard screen for an input, by its label, until the
// panel asks for that input again.
std::map<std::string, std::string> &typed() {
    static std::map<std::string, std::string> text;
    return text;
}

float px(float value) {
    return std::round(value * Layer::get().scale());
}

ImU32 colour(std::uint32_t rgba) {
    return IM_COL32((rgba >> 24u) & 0xFFu, (rgba >> 16u) & 0xFFu, (rgba >> 8u) & 0xFFu, rgba & 0xFFu);
}

std::string label_of(const char *label) {
    return label != nullptr ? label : "";
}

// A point on the game's picture, from 0..1 to the window.
ImVec2 on_picture(float x, float y) {
    const ImGuiIO &io = ImGui::GetIO();
    const std::array<float, 4> picture = Layer::get().renderer().game_picture();
    return {(picture[0] + x * picture[2]) * io.DisplaySize.x, (picture[1] + y * picture[3]) * io.DisplaySize.y};
}

float picture_height() {
    return Layer::get().renderer().game_picture()[3] * ImGui::GetIO().DisplaySize.y;
}

void w_text(const char *text) {
    if (g_overlay)
        ImGui::TextUnformatted(text != nullptr ? text : "");
    else
        paragraph(label_of(text), colors::kText);
}

void w_text_colored(std::uint32_t rgba, const char *text) {
    if (g_overlay) {
        ImGui::PushStyleColor(ImGuiCol_Text, colour(rgba));
        ImGui::TextUnformatted(text != nullptr ? text : "");
        ImGui::PopStyleColor();
    } else {
        paragraph(label_of(text), colour(rgba));
    }
}

void w_separator() {
    ImGui::Separator();
}

void w_same_line() {
    ImGui::SameLine();
}

int w_section(const char *label) {
    if (g_overlay)
        ImGui::SeparatorText(label != nullptr ? label : "");
    else
        section(label_of(label).c_str());
    return 1;
}

int w_button(const char *label) {
    if (g_overlay) {
        ImGui::TextUnformatted(label != nullptr ? label : "");
        return 0;
    }
    return button_row(label_of(label).c_str()) ? 1 : 0;
}

int w_checkbox(const char *label, int *value) {
    if (value == nullptr) return 0;
    if (g_overlay) {
        bool shown = *value != 0;
        ImGui::Checkbox(label != nullptr ? label : "", &shown);
        return 0;
    }
    if (!toggle_row(label_of(label).c_str(), *value != 0)) return 0;
    *value = *value != 0 ? 0 : 1;
    return 1;
}

int w_slider_int(const char *label, std::int32_t *value, std::int32_t minimum, std::int32_t maximum) {
    if (value == nullptr || minimum > maximum) return 0;
    if (g_overlay) {
        ImGui::Text("%s: %d", label != nullptr ? label : "", static_cast<int>(*value));
        return 0;
    }
    int shown = std::clamp(static_cast<int>(*value), static_cast<int>(minimum), static_cast<int>(maximum));
    const bool changed = slider_row(label_of(label).c_str(), shown, minimum, maximum, 1, "%d");
    if (!changed && shown == *value) return 0;
    *value = shown;
    return 1;
}

int w_slider_float(const char *label, float *value, float minimum, float maximum) {
    if (value == nullptr || !(minimum <= maximum)) return 0;
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(*value));
    if (g_overlay) {
        ImGui::Text("%s: %s", label != nullptr ? label : "", text);
        return 0;
    }
    // A hundred steps across the range, with left and right.
    const int delta = choice_row(label_of(label).c_str(), text);
    if (delta == 0) return 0;
    const float step = (maximum - minimum) / 100.0f;
    *value = std::clamp(*value + static_cast<float>(delta) * step, minimum, maximum);
    return 1;
}

int w_input_text(const char *label, char *buffer, std::uint32_t capacity) {
    if (buffer == nullptr || capacity == 0u) return 0;
    buffer[capacity - 1u] = '\0';
    if (g_overlay) {
        ImGui::Text("%s: %s", label != nullptr ? label : "", buffer);
        return 0;
    }
    const std::string key = label_of(label);
    if (const auto done = typed().find(key); done != typed().end()) {
        const std::size_t length = std::min<std::size_t>(done->second.size(), capacity - 1u);
        std::memcpy(buffer, done->second.data(), length);
        buffer[length] = '\0';
        typed().erase(done);
        return 1;
    }
    if (value_row(key.c_str(), buffer)) {
        TextInputRequest request;
        request.title = key;
        request.initial = buffer;
        request.max_length = capacity - 1u;
        open_text_input(std::move(request), [key](std::optional<std::string> text) {
            if (text) typed()[key] = *text;
        });
    }
    return 0;
}

void w_line(float x0, float y0, float x1, float y1, std::uint32_t rgba, float thickness) {
    if (!g_overlay) return;
    ImGui::GetForegroundDrawList()->AddLine(
        on_picture(x0, y0), on_picture(x1, y1), colour(rgba), std::max(1.0f, thickness * Layer::get().scale()));
}

void w_rect(float x0, float y0, float x1, float y1, std::uint32_t rgba, int filled) {
    if (!g_overlay) return;
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    if (filled != 0)
        draw->AddRectFilled(on_picture(x0, y0), on_picture(x1, y1), colour(rgba));
    else
        draw->AddRect(on_picture(x0, y0), on_picture(x1, y1), colour(rgba), 0.0f, 0, px(2.0f));
}

void w_circle(float x, float y, float radius, std::uint32_t rgba, int filled) {
    if (!g_overlay) return;
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    const float r = radius * picture_height();
    if (filled != 0)
        draw->AddCircleFilled(on_picture(x, y), r, colour(rgba), 32);
    else
        draw->AddCircle(on_picture(x, y), r, colour(rgba), 32, px(2.0f));
}

void w_text_at(float x, float y, std::uint32_t rgba, const char *text) {
    if (!g_overlay || text == nullptr) return;
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    const ImVec2 at = on_picture(x, y);
    draw->AddText({at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, 160), text);
    draw->AddText(at, colour(rgba), text);
}

const ymod_widgets kWidgets{1u, sizeof(ymod_widgets), &w_text, &w_text_colored, &w_separator, &w_same_line, &w_section,
    &w_button, &w_checkbox, &w_slider_int, &w_slider_float, &w_input_text, &w_line, &w_rect, &w_circle, &w_text_at};

} // namespace

void draw_mod_panels(const std::string &mod_id) {
    const std::vector<mods::native::Panel> panels = mods::native::panels(mod_id, false);
    int index = 0;
    for (const mods::native::Panel &panel : panels) {
        const std::string heading = (panel.title.empty() ? panel.mod : panel.title) + ":";
        section(heading.c_str());
        ImGui::PushID(index++);
        g_overlay = false;
        panel.draw(&kWidgets, panel.user);
        ImGui::PopID();
    }
}

bool mod_overlays_present() {
    return !mods::native::panels({}, true).empty();
}

void draw_mod_overlays() {
    const std::vector<mods::native::Panel> overlays = mods::native::panels({}, true);
    const ImVec2 corner = on_picture(0.0f, 0.0f);
    float y = corner.y + px(8.0f);
    int index = 0;
    for (const mods::native::Panel &overlay : overlays) {
        const std::string id = "##mod overlay " + std::to_string(index++);
        ImGui::SetNextWindowPos({corner.x + px(8.0f), y});
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        if (ImGui::Begin(id.c_str(), nullptr, kFlags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors::kTextDim);
            ImGui::TextUnformatted((overlay.title.empty() ? overlay.mod : overlay.title).c_str());
            ImGui::PopStyleColor();
            g_overlay = true;
            overlay.draw(&kWidgets, overlay.user);
            g_overlay = false;
            y += ImGui::GetWindowHeight() + px(6.0f);
        }
        ImGui::End();
    }
}

} // namespace mhp3rd::ui
