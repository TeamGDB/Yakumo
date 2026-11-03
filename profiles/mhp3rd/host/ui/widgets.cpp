#include "ui/widgets.hpp"

#include "ui/layer.hpp"

#include "gpu/vulkan_renderer.hpp"

#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>

namespace mhp3rd::ui {
namespace {

bool focus_next = false;
bool keep_focus = false;
bool help_enabled{};
struct Help {
    bool open{}, pending{};
    std::string title, description;
} help;

// Moves the gamepad and keyboard focus to the item just submitted when
// focus_next_row() asked for it. SetKeyboardFocusHere() would not do: it
// tabs to the next text field and starts editing it.
void take_focus() {
    if (!focus_next) return;
    focus_next = false;
    ImGui::FocusItem();
    ImGui::SetScrollHereY(0.3f);
}

float font() {
    return Layer::get().font_size();
}

// End of the visible part of a label: "##" starts the part that only makes
// the id unique, as everywhere in ImGui.
const char *shown_end(const char *label) {
    const char *hidden = std::strstr(label, "##");
    return hidden != nullptr ? hidden : label + std::strlen(label);
}
float px(float value) {
    return std::round(value * Layer::get().scale());
}

// Left or right on the keyboard, the D-pad or the left stick, with repeat.
int horizontal_press() {
    int delta = 0;
    for (ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft})
        if (ImGui::IsKeyPressed(key, true)) delta = -1;
    for (ImGuiKey key : {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight})
        if (ImGui::IsKeyPressed(key, true)) delta = 1;
    return delta;
}

void draw_triangle(ImDrawList *draw, ImVec2 center, float size, bool right, ImU32 color) {
    const float h = size * 0.5f;
    if (right)
        draw->AddTriangleFilled({center.x - h * 0.8f, center.y - h}, {center.x - h * 0.8f, center.y + h},
            {center.x + h * 0.8f, center.y}, color);
    else
        draw->AddTriangleFilled({center.x + h * 0.8f, center.y - h}, {center.x + h * 0.8f, center.y + h},
            {center.x - h * 0.8f, center.y}, color);
}

// Keep the fixed controls reachable even in a short landscape window.
bool short_touch_window() {
    return Layer::get().comfortable() && ImGui::GetIO().DisplaySize.y < font() * 24.f;
}
float footer_description_height() {
    return font() * (short_touch_window() ? 1.8f : 2.7f);
}

enum class ScrollIntent { None, Pending, Vertical, Horizontal, Cancelled };
ImGuiID scroll_key(const char *name) {
    return ImHashStr(name, 0, ImGui::GetCurrentWindow()->ID);
}
ScrollIntent scroll_intent() {
    return static_cast<ScrollIntent>(ImGui::GetStateStorage()->GetInt(scroll_key("scroll_intent")));
}
void set_scroll_intent(ScrollIntent value) {
    ImGui::GetStateStorage()->SetInt(scroll_key("scroll_intent"), static_cast<int>(value));
}

struct Row {
    ImVec2 min;
    ImVec2 max;
    bool pressed{};
    bool focused{};
    bool hovered{};
    ImVec2 full_max;
};

// The part every row shares: a full-width focusable strip with its label.
Row row(const char *label, const RowOptions &options, ImU32 color = colors::kText, float height_lines = 1.0f) {
    touch_scroll();
    const float height = std::max(
        std::round(font() * (0.9f + height_lines)), Layer::get().comfortable() ? Layer::get().touch_target() : 0.0f);
    Row result;
    result.min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float body_width = std::max(1.0f, width - setting_help_width(options));
    result.full_max = {result.min.x + width, result.min.y + height};
    result.max = {result.min.x + body_width, result.min.y + height};
    result.pressed = ImGui::InvisibleButton(label, {body_width, height}, ImGuiButtonFlags_EnableNav);
    take_focus();
    result.focused = ImGui::IsItemFocused();
    result.hovered = ImGui::IsItemHovered();

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = px(6.0f);
    if (result.focused) {
        draw->AddRectFilled(result.min, result.max, colors::kRowFocus, rounding);
        draw->AddRectFilled(result.min, {result.min.x + px(4.0f), result.max.y}, colors::kAccent, rounding,
            ImDrawFlags_RoundCornersLeft);
    } else if (result.hovered) {
        draw->AddRectFilled(result.min, result.max, colors::kRowHover, rounding);
    }
    const float text_y = result.min.y + (height - font()) * 0.5f;
    draw->AddText(
        {result.min.x + px(16.0f), text_y}, options.disabled ? colors::kTextDisabled : color, label, shown_end(label));
    if (result.focused || (result.hovered && Layer::get().description().empty())) {
        std::string description = options.description;
        if (!options.note.empty()) description += (description.empty() ? "" : "\n") + options.note;
        Layer::get().set_description(description);
    }
    return result;
}

// Draws `value` right-aligned in the row, with the note to its left.
float draw_value(const Row &r, const std::string &value, const RowOptions &options, float right_inset, ImU32 color) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float text_y = r.min.y + (r.max.y - r.min.y - font()) * 0.5f;
    const float value_width = ImGui::CalcTextSize(value.c_str()).x;
    const float x = r.max.x - right_inset - value_width;
    draw->AddText({x, text_y}, options.disabled ? colors::kTextDisabled : color, value.c_str());
    if (!options.note.empty() && options.disabled) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        const float note_width = ImGui::CalcTextSize(options.note.c_str()).x;
        draw->AddText({x - px(18.0f) - note_width, text_y + font() * 0.15f}, colors::kTextDim, options.note.c_str());
        ImGui::PopFont();
    }
    return x;
}

// A rounded label such as a shoulder button or a key.
float draw_cap(ImDrawList *draw, ImVec2 at, const char *text, bool filled) {
    const float height = font() * 1.15f;
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float width = std::max(height, text_size.x + font() * 0.7f);
    const ImVec2 min{at.x, at.y + (font() - height) * 0.5f};
    const ImVec2 max{min.x + width, min.y + height};
    if (filled)
        draw->AddRectFilled(min, max, colors::kAccent, height * 0.3f);
    else
        draw->AddRect(min, max, colors::kTextDim, height * 0.3f, 0, px(1.5f));
    draw->AddText({min.x + (width - text_size.x) * 0.5f, min.y + (height - text_size.y) * 0.5f},
        filled ? colors::kPanel : colors::kText, text);
    return width;
}

// A face button as the pad in use labels it: a PlayStation shape or a letter.
float draw_face(ImDrawList *draw, ImVec2 at, SDL_GamepadButton button) {
    const float radius = font() * 0.62f;
    const ImVec2 center{at.x + radius, at.y + font() * 0.5f};
    draw->AddCircleFilled(center, radius, colors::kAccent);
    SDL_Gamepad *pad = Layer::get().attached() ? Layer::get().renderer().gamepad() : nullptr;
    SDL_GamepadButtonLabel label =
        pad != nullptr ? SDL_GetGamepadButtonLabel(pad, button) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    if (label == SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) {
        // Positional names of the common Xbox-style layout, as on a Steam Deck.
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            label = SDL_GAMEPAD_BUTTON_LABEL_A;
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            label = SDL_GAMEPAD_BUTTON_LABEL_B;
            break;
        case SDL_GAMEPAD_BUTTON_WEST:
            label = SDL_GAMEPAD_BUTTON_LABEL_X;
            break;
        default:
            label = SDL_GAMEPAD_BUTTON_LABEL_Y;
            break;
        }
    }
    const ImU32 ink = colors::kPanel;
    const float s = radius * 0.45f;
    const float thickness = std::max(1.5f, radius * 0.16f);
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        draw->AddLine({center.x - s, center.y - s}, {center.x + s, center.y + s}, ink, thickness);
        draw->AddLine({center.x - s, center.y + s}, {center.x + s, center.y - s}, ink, thickness);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
        draw->AddCircle(center, s * 1.1f, ink, 0, thickness);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        draw->AddRect({center.x - s, center.y - s}, {center.x + s, center.y + s}, ink, 0.0f, 0, thickness);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        draw->AddTriangle({center.x, center.y - s * 1.1f}, {center.x + s * 1.1f, center.y + s * 0.8f},
            {center.x - s * 1.1f, center.y + s * 0.8f}, ink, thickness);
        break;
    default: {
        const char *letter = label == SDL_GAMEPAD_BUTTON_LABEL_A ? "A"
            : label == SDL_GAMEPAD_BUTTON_LABEL_B                ? "B"
            : label == SDL_GAMEPAD_BUTTON_LABEL_X                ? "X"
                                                                 : "Y";
        const ImVec2 size = ImGui::CalcTextSize(letter);
        draw->AddText({center.x - size.x * 0.5f, center.y - size.y * 0.5f}, ink, letter);
        break;
    }
    }
    return radius * 2.0f;
}

const char *shoulder_name(bool right) {
    SDL_Gamepad *pad = Layer::get().attached() ? Layer::get().renderer().gamepad() : nullptr;
    const SDL_GamepadType type = pad != nullptr ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const char *name = pad != nullptr ? SDL_GetGamepadName(pad) : nullptr;
    const bool playstation =
        type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    // The Steam Deck prints L1 and R1 on its bumpers.
    const bool deck = name != nullptr && std::strstr(name, "Steam Deck") != nullptr;
    if (playstation || deck) return right ? "R1" : "L1";
    if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)
        return right ? "R" : "L";
    return right ? "RB" : "LB";
}

// The button left of the pad's centre, as the pad in use names it.
const char *select_name() {
    SDL_Gamepad *pad = Layer::get().attached() ? Layer::get().renderer().gamepad() : nullptr;
    switch (pad != nullptr ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN) {
    case SDL_GAMEPAD_TYPE_PS3:
        return "Select";
    case SDL_GAMEPAD_TYPE_PS4:
        return "Share";
    case SDL_GAMEPAD_TYPE_PS5:
        return "Create";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return "−";
    default:
        return "View";
    }
}

} // namespace

void enable_context_help(bool enabled) {
    help_enabled = enabled;
    if (!enabled) close_context_help();
}
bool context_help_open() {
    return help.open;
}
void close_context_help() {
    help.open = false;
    help.pending = false;
}
float setting_help_width(const RowOptions &options) {
    return help_enabled && !options.help.empty()
        ? std::max(font() * 1.8f, Layer::get().comfortable() ? Layer::get().touch_target() : 0.f)
        : 0.f;
}
void setting_help_button(const char *label, const RowOptions &options, ImVec2 min, ImVec2 max) {
    const float width = setting_help_width(options);
    if (width <= 0) return;
    const bool keyboard = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_F1, false);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({max.x - width, min.y});
    ImGui::PushID(label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool pressed = ImGui::InvisibleButton("##setting_help", {width, max.y - min.y});
    ImGui::PopItemFlag();
    const ImVec2 centre{max.x - width * .5f, (min.y + max.y) * .5f};
    const auto colour = ImGui::IsItemHovered() ? colors::kAccentBright : colors::kTextDim;
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddCircle(centre, font() * .45f, colour, 20, px(1.2f));
    const auto size = ImGui::CalcTextSize("i");
    draw->AddText({centre.x - size.x * .5f, centre.y - size.y * .5f}, colour, "i");
    if (pressed || keyboard) {
        help.title.assign(label, shown_end(label));
        help.description = options.help;
        if (!options.note.empty()) help.description += "\n\n" + options.note;
        help.open = help.pending = true;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos({cursor.x, cursor.y - ImGui::GetStyle().ItemSpacing.y});
    ImGui::Dummy({0, 0});
}
void draw_context_help() {
    if (!help.open && !help.pending && !ImGui::IsPopupOpen("Setting Help##context_help")) return;
    const bool opening = help.pending;
    if (opening) {
        ImGui::OpenPopup("Setting Help##context_help");
        help.pending = false;
    }
    const auto &io = ImGui::GetIO();
    const float width = std::min(io.DisplaySize.x * .9f, font() * 28.f);
    const float padding = ImGui::GetStyle().WindowPadding.y;
    const float button_height = std::max(font() * 2.2f, Layer::get().comfortable() ? Layer::get().touch_target() : 0.f);
    const float text_height = ImGui::CalcTextSize(help.description.c_str(), nullptr, false,
        std::max(1.f, width - ImGui::GetStyle().WindowPadding.x * 2.f - ImGui::GetStyle().ScrollbarSize))
                                  .y;
    const float height = std::min(io.DisplaySize.y * .9f,
        padding * 2.f + text_height + button_height + font() * 1.15f + px(4.f) + ImGui::GetStyle().ItemSpacing.y * 4.f);
    ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f}, ImGuiCond_Always, {.5f, .5f});
    ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Setting Help##context_help", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings)) {
        if (!help.open)
            ImGui::CloseCurrentPopup();
        else {
            heading(help.title);
            const float body_height =
                std::max(1.f, ImGui::GetContentRegionAvail().y - button_height - ImGui::GetStyle().ItemSpacing.y);
            ImGui::BeginChild("help_body", {0.f, body_height});
            if (opening) ImGui::SetScrollY(0.f);
            touch_scroll();
            paragraph(help.description);
            ImGui::EndChild();
            if (big_button("Close##help_close", ImGui::GetContentRegionAvail().x, true)) {
                close_context_help();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

ImGuiStyle make_style(float scale, float font_size) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 12.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 10.0f;
    style.GrabRounding = 6.0f;
    style.WindowBorderSize = 1.5f;
    style.PopupBorderSize = 1.5f;
    style.WindowPadding = {26.0f, 20.0f};
    style.FramePadding = {12.0f, 7.0f};
    style.ItemSpacing = {10.0f, 4.0f};
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 5.0f;
    style.ScaleAllSizes(scale);
    style.FontSizeBase = font_size;

    ImVec4 *c = style.Colors;
    const auto rgba = [](ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); };
    c[ImGuiCol_Text] = rgba(colors::kText);
    c[ImGuiCol_TextDisabled] = rgba(colors::kTextDisabled);
    c[ImGuiCol_WindowBg] = rgba(colors::kPanel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgba(IM_COL32(38, 27, 19, 252));
    c[ImGuiCol_ModalWindowDimBg] = rgba(IM_COL32(8, 5, 3, 150));
    c[ImGuiCol_Border] = rgba(colors::kPanelEdge);
    c[ImGuiCol_FrameBg] = rgba(IM_COL32(255, 240, 210, 20));
    c[ImGuiCol_FrameBgHovered] = rgba(colors::kRowHover);
    c[ImGuiCol_FrameBgActive] = rgba(colors::kRowFocus);
    c[ImGuiCol_TextSelectedBg] = rgba(IM_COL32(217, 166, 75, 110));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgba(IM_COL32(143, 93, 36, 160));
    c[ImGuiCol_ScrollbarGrabHovered] = rgba(IM_COL32(176, 122, 54, 200));
    c[ImGuiCol_ScrollbarGrabActive] = rgba(colors::kAccent);
    c[ImGuiCol_Separator] = rgba(IM_COL32(143, 93, 36, 120));
    // Rows draw their own focus; ImGui's rectangle would double it.
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    return style;
}

void begin_panel(const char *id, const std::string &title, const std::string &subtitle, bool dim_game, bool *close,
    bool close_disabled) {
    const ImGuiIO &io = ImGui::GetIO();
    if (dim_game) ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, io.DisplaySize, colors::kBackdrop);
    const bool comfortable = Layer::get().comfortable();
    const float margin = std::round(std::min(io.DisplaySize.x, io.DisplaySize.y) * (comfortable ? 0.015f : 0.03f));
    const ImVec2 size{
        comfortable ? io.DisplaySize.x - 2.0f * margin : std::min(io.DisplaySize.x - 2.0f * margin, font() * 46.0f),
        std::min(io.DisplaySize.y - 2.0f * margin, font() * 31.0f)};
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(id, nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    const ImVec2 title_at = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kAccentBright);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopStyleColor();
    const float title_height = ImGui::GetItemRectSize().y;
    ImGui::PopFont();
    const float close_size =
        close ? std::max(font() * 1.9f, Layer::get().comfortable() ? Layer::get().touch_target() : 0.f) : 0.f;
    if (!subtitle.empty()) {
        const float width = ImGui::CalcTextSize(subtitle.c_str()).x;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x -
            (close ? close_size + font() * .5f : 0.f);
        draw->AddText({right - width, title_at.y + title_height - font() * 1.15f}, colors::kTextDim, subtitle.c_str());
    }
    if (close) {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorScreenPos({right - close_size, title_at.y});
        ImGui::BeginDisabled(close_disabled);
        if (ImGui::InvisibleButton("##panel_close", {close_size, close_size})) *close = true;
        const ImU32 colour = close_disabled ? colors::kTextDisabled
            : ImGui::IsItemHovered()        ? colors::kAccentBright
                                            : colors::kText;
        const ImVec2 centre{right - close_size * .5f, title_at.y + close_size * .5f};
        const float radius = font() * .3f;
        draw->AddLine({centre.x - radius, centre.y - radius}, {centre.x + radius, centre.y + radius}, colour, px(2.f));
        draw->AddLine({centre.x - radius, centre.y + radius}, {centre.x + radius, centre.y - radius}, colour, px(2.f));
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(
            {title_at.x, title_at.y + std::max(close_size, title_height) + ImGui::GetStyle().ItemSpacing.y});
    }
    const ImVec2 line = ImGui::GetCursorScreenPos();
    const float line_width = ImGui::GetContentRegionAvail().x;
    draw->AddRectFilledMultiColor(line, {line.x + line_width, line.y + px(2.0f)}, colors::kAccent,
        IM_COL32(143, 93, 36, 0), IM_COL32(143, 93, 36, 0), colors::kAccent);
    ImGui::Dummy({0.0f, px(10.0f)});
}

void begin_content(bool touch_footer) {
    const auto &layer = Layer::get();
    auto *window = ImGui::GetCurrentWindow();
    auto &store = window->StateStorage;
    const float width = ImGui::GetContentRegionAvail().x;
    const float estimated = font() * 1.5f * std::max(1.f, std::ceil(font() * 36.f / std::max(width, 1.f)));
    const bool measured = std::abs(store.GetFloat(ImHashStr("hint_width", 0, window->ID)) - width) < 1.f &&
        store.GetInt(ImHashStr("hint_device", 0, window->ID), -1) == static_cast<int>(layer.input_device()) &&
        std::abs(store.GetFloat(ImHashStr("hint_font", 0, window->ID)) - font()) < .1f;
    const float action_height = layer.input_device() == InputDevice::Touch || touch_footer
        ? std::max(font() * 2.2f, layer.comfortable() ? layer.touch_target() : 0.f)
        : measured ? store.GetFloat(ImHashStr("hint_height", 0, window->ID), estimated)
                   : estimated;
    const float footer = px(10.f) + ImGui::GetStyle().ItemSpacing.y * 3.f + footer_description_height() + action_height;
    const float content = std::max(1.f, ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("content", {0.0f, content}, ImGuiChildFlags_NavFlattened);
    touch_scroll();
}

void stop_scroll_motion() {
    auto *store = ImGui::GetStateStorage();
    store->SetInt(scroll_key("scroll_intent"), static_cast<int>(ScrollIntent::None));
    store->SetInt(scroll_key("scroll_slider"), 0);
    store->SetFloat(scroll_key("scroll_velocity"), 0.f);
    store->SetInt(scroll_key("scroll_frame"), ImGui::GetFrameCount());
}
void touch_scroll() {
    ImGuiIO &io = ImGui::GetIO();
    auto *window = ImGui::GetCurrentWindow();
    auto *store = ImGui::GetStateStorage();
    const ImGuiID frame_id = scroll_key("scroll_frame");
    if (store->GetInt(frame_id, -1) == ImGui::GetFrameCount()) return;
    store->SetInt(frame_id, ImGui::GetFrameCount());
    float velocity = store->GetFloat(scroll_key("scroll_velocity"));
    const float dt = std::isfinite(io.DeltaTime) ? std::clamp(io.DeltaTime, .001f, .05f) : 1.f / 60.f;
    const bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
    const auto *modal = ImGui::GetTopMostPopupModal();
    const bool blocked = modal && modal != window && window->RootWindow != modal;
    const bool navigation = io.MouseWheel != 0.f || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false);
    if (blocked || navigation) {
        stop_scroll_motion();
        return;
    }
    const bool hovered =
        GImGui->HoveredWindow == window && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (ImGui::IsMouseClicked(0)) {
        velocity = 0;
        if (hovered && io.MousePos.x < window->InnerRect.Max.x) {
            set_scroll_intent(ScrollIntent::Pending);
            store->SetFloat(scroll_key("scroll_start_x"), io.MousePos.x);
            store->SetFloat(scroll_key("scroll_start_y"), io.MousePos.y);
            store->SetFloat(scroll_key("scroll_start"), ImGui::GetScrollY());
            store->SetInt(scroll_key("scroll_slider"), 0);
        } else
            set_scroll_intent(ScrollIntent::None);
    }
    auto intent = scroll_intent();
    if (io.MouseDown[0] && intent != ScrollIntent::None) {
        const float dx = io.MousePos.x - store->GetFloat(scroll_key("scroll_start_x"));
        const float dy = io.MousePos.y - store->GetFloat(scroll_key("scroll_start_y"));
        const float threshold = std::max(font() * .4f, io.MouseDragThreshold);
        if (intent == ScrollIntent::Pending) {
            const bool slider = store->GetInt(scroll_key("scroll_slider")) != 0;
            if (slider && std::abs(dx) > threshold && std::abs(dx) > std::abs(dy) * 1.2f)
                intent = ScrollIntent::Horizontal;
            else if (std::abs(dy) > threshold && (!slider || std::abs(dy) >= std::abs(dx)))
                intent = ScrollIntent::Vertical;
            else if (!slider && std::abs(dx) > threshold)
                intent = ScrollIntent::Cancelled;
            set_scroll_intent(intent);
        }
        if (intent == ScrollIntent::Vertical) {
            const float target = std::clamp(store->GetFloat(scroll_key("scroll_start")) - dy, 0.f, window->ScrollMax.y);
            ImGui::SetScrollY(target);
            const float sample = std::clamp(-io.MouseDelta.y / dt, -window->Size.y * 8.f, window->Size.y * 8.f);
            velocity = io.MouseDelta.y == 0.f ? velocity * .5f : velocity * .6f + sample * .4f;
            if (target == 0.f || target == window->ScrollMax.y) velocity = 0.f;
            ImGui::ClearActiveID();
        } else if (intent == ScrollIntent::Cancelled)
            ImGui::ClearActiveID();
    } else {
        const char *inertia = std::getenv("MHP3RD_MENU_INERTIA");
        const bool enabled = !inertia || std::string_view(inertia) != "0";
        if (touch && enabled && intent == ScrollIntent::Vertical && std::abs(velocity) > 8.f) {
            const float target = std::clamp(ImGui::GetScrollY() + velocity * dt, 0.f, window->ScrollMax.y);
            ImGui::SetScrollY(target);
            velocity *= std::exp(-10.f * dt);
            if (target == 0.f || target == window->ScrollMax.y) velocity = 0.f;
        } else
            velocity = 0.f;
        if (!ImGui::IsMouseReleased(0) && std::abs(velocity) < 8.f) set_scroll_intent(ScrollIntent::None);
    }
    store->SetFloat(scroll_key("scroll_velocity"), velocity);
}

void begin_footer() {
    touch_scroll();
    ImGui::EndChild();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 line = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    draw->AddLine(
        {line.x, line.y + px(4.0f)}, {line.x + width, line.y + px(4.0f)}, IM_COL32(143, 93, 36, 90), px(1.0f));
    ImGui::Dummy({0.0f, px(10.0f)});
    // A fixed readable region keeps the actions stable; long descriptions scroll.
    struct Description {
        std::string text;
        ImGuiID window{};
    };
    static std::map<ImGuiID, Description> descriptions;
    auto &state = descriptions[ImGui::GetCurrentWindow()->ID];
    const auto *previous = ImGui::FindWindowByID(state.window);
    const bool reading = previous && (GImGui->HoveredWindow == previous || GImGui->NavWindow == previous);
    const auto &layer = Layer::get();
    const bool changed = (layer.description_set() || !reading) && state.text != layer.description();
    if (layer.description_set() || !reading) state.text = layer.description();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * .88f);
    ImGui::BeginChild("footer_description", {0.f, footer_description_height()}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    state.window = ImGui::GetCurrentWindow()->ID;
    if (changed) {
        stop_scroll_motion();
        ImGui::SetScrollY(0.f);
    }
    touch_scroll();
    paragraph(state.text, colors::kTextDim);
    ImGui::PopFont();
    ImGui::EndChild();
}

void end_panel() {
    ImGui::End();
}

bool tab_bar(const char *const *labels, int count, int &selected) {
    const int before = selected;
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool modal = ImGui::GetTopMostPopupModal() != nullptr;
    if (!modal &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_Q, false))))
        selected = (selected + count - 1) % count;
    if (!modal &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_W, false))))
        selected = (selected + 1) % count;

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const bool pad = Layer::get().input_device() == InputDevice::Gamepad;
    const float height = std::max(font() * 1.9f, Layer::get().comfortable() ? Layer::get().touch_target() : 0.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    float inner_left = start.x;
    float inner_width = width;
    if (Layer::get().input_device() != InputDevice::Touch) {
        // Touch navigation uses the tabs directly and needs no key caps.
        const float cap_left =
            draw_cap(draw, {start.x, start.y + (height - font()) * 0.5f}, pad ? shoulder_name(false) : "Q", false);
        const float cap_right_width = ImGui::CalcTextSize(pad ? shoulder_name(true) : "W").x + font() * 0.7f;
        draw_cap(draw,
            {start.x + width - std::max(font() * 1.15f, cap_right_width), start.y + (height - font()) * 0.5f},
            pad ? shoulder_name(true) : "W", false);
        inner_left += cap_left + px(14.0f);
        inner_width -= cap_left + std::max(font() * 1.15f, cap_right_width) + px(28.0f);
    }
    float label_width = 0.0f;
    for (int i = 0; i < count; ++i) label_width = std::max(label_width, ImGui::CalcTextSize(labels[i]).x);
    const float minimum_width =
        std::max(label_width + px(24.0f), Layer::get().comfortable() ? Layer::get().touch_target() : 0.0f);
    const bool scrollable = minimum_width * count > inner_width + 1.0f;
    const float arrow_width = scrollable ? height : 0.0f;
    const float track_left = inner_left;
    const float track_width = inner_width;
    inner_left += arrow_width;
    inner_width = std::max(1.0f, inner_width - arrow_width * 2.0f);
    const float tab_width = std::max(inner_width / static_cast<float>(count), minimum_width);
    const float total_width = tab_width * static_cast<float>(count);
    const float strip_height = height + (scrollable && !short_touch_window() ? ImGui::GetStyle().ScrollbarSize : 0.0f);
    ImGui::SetCursorScreenPos({inner_left, start.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{});
    ImGui::SetNextWindowContentSize({total_width, 0.0f});
    ImGui::BeginChild("tab_strip", {inner_width, strip_height}, ImGuiChildFlags_None,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            (short_touch_window() ? ImGuiWindowFlags_NoScrollbar : ImGuiWindowFlags_None));
    ImGui::PopStyleVar(2);
    ImGuiStorage *tab_state = ImGui::GetStateStorage();
    const ImGuiID previous_id = ImGui::GetID("previous_tab");
    const ImGuiID drag_id = ImGui::GetID("dragged_tabs");
    const ImGuiID start_x_id = ImGui::GetID("drag_start_x");
    const ImGuiID start_scroll_id = ImGui::GetID("drag_start_scroll");
    // Automatic reveal follows selection changes, not every frame: manual scrolling stays put.
    if (scrollable && tab_state->GetInt(previous_id, -1) != selected) {
        const float left = tab_width * static_cast<float>(selected);
        const float scroll = ImGui::GetScrollX();
        if (left < scroll)
            ImGui::SetScrollX(left);
        else if (left + tab_width > scroll + inner_width)
            ImGui::SetScrollX(left + tab_width - inner_width);
    }
    tab_state->SetInt(previous_id, selected);
    draw = ImGui::GetWindowDrawList();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorPos({tab_width * static_cast<float>(i), 0.0f});
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("tab", {tab_width, height});
        if (ImGui::IsItemActivated()) {
            tab_state->SetBool(drag_id, false);
            tab_state->SetFloat(start_x_id, ImGui::GetIO().MousePos.x);
            tab_state->SetFloat(start_scroll_id, ImGui::GetScrollX());
        }
        if (scrollable && ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
            const float distance = ImGui::GetIO().MousePos.x - tab_state->GetFloat(start_x_id);
            if (std::abs(distance) >= ImGui::GetIO().MouseDragThreshold) tab_state->SetBool(drag_id, true);
            if (tab_state->GetBool(drag_id))
                ImGui::SetScrollX(
                    std::clamp(tab_state->GetFloat(start_scroll_id) - distance, 0.0f, ImGui::GetScrollMaxX()));
        }
        if (pressed && !tab_state->GetBool(drag_id)) selected = i;
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool active = i == selected;
        if (hovered && !active) draw->AddRectFilled(min, max, colors::kRowHover, px(6.0f));
        const ImVec2 size = ImGui::CalcTextSize(labels[i]);
        draw->AddText({min.x + (tab_width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f},
            active ? colors::kAccentBright : colors::kTextDim, labels[i]);
        if (active)
            draw->AddRectFilled({min.x + tab_width * 0.18f, max.y - px(3.0f)}, {max.x - tab_width * 0.18f, max.y},
                colors::kAccent, px(2.0f));
    }
    ImGui::PopItemFlag();
    ImGuiWindow *strip = ImGui::GetCurrentWindow();
    const float scroll = ImGui::GetScrollX();
    const float maximum = std::max(0.0f, total_width - inner_width);
    if (scrollable) {
        const float fade = std::min(px(14.0f), inner_width * .12f);
        const ImU32 transparent = colors::kPanel & 0x00FFFFFFu;
        if (scroll > 1.0f)
            draw->AddRectFilledMultiColor({strip->Pos.x, start.y}, {strip->Pos.x + fade, start.y + height},
                colors::kPanel, transparent, transparent, colors::kPanel);
        if (scroll < maximum - 1.0f)
            draw->AddRectFilledMultiColor({strip->Pos.x + inner_width - fade, start.y},
                {strip->Pos.x + inner_width, start.y + height}, transparent, colors::kPanel, colors::kPanel,
                transparent);
    }
    ImGui::EndChild();
    if (scrollable) {
        const auto arrow = [&](bool right) {
            const bool available = right ? scroll < maximum - 1.0f : scroll > 1.0f;
            if (!available) return;
            const ImVec2 at{right ? track_left + track_width - arrow_width : track_left, start.y};
            ImGui::SetCursorScreenPos(at);
            if (ImGui::InvisibleButton(right ? "##tabs_next" : "##tabs_previous", {arrow_width, height}))
                ImGui::SetScrollX(strip, std::clamp(scroll + (right ? 1.f : -1.f) * inner_width * .7f, 0.f, maximum));
            auto *arrows = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered())
                arrows->AddRectFilled(at, {at.x + arrow_width, at.y + height}, colors::kRowHover, px(6.f));
            draw_triangle(arrows, {at.x + arrow_width * .5f, at.y + height * .5f}, font() * .75f, right,
                ImGui::IsItemActive() ? colors::kAccentBright : colors::kAccent);
        };
        arrow(false);
        arrow(true);
    }
    ImGui::SetCursorScreenPos({start.x, start.y + strip_height + px(8.0f)});
    ImGui::Dummy({0.0f, 0.0f});
    return selected != before;
}

void focus_next_row() {
    if (!keep_focus) focus_next = true;
}
void keep_row_focus(bool keep) {
    keep_focus = keep;
    if (keep) focus_next = false;
}

int choice_row(const char *label, const std::string &value, const RowOptions &options) {
    const Row r = row(label, options);
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const float right_arrow_x = r.max.x - px(16.0f) - font() * 0.45f;
    const float value_x = r.max.x - px(16.0f) - font() * 1.1f - ImGui::CalcTextSize(value.c_str()).x;
    const float left_arrow_x = value_x - font() * 0.65f;
    int delta = r.focused ? horizontal_press() : 0;
    if (r.pressed) {
        // A click on the left arrow steps back; anything else steps forward.
        const float mouse_x = ImGui::GetIO().MousePos.x;
        const bool on_left_arrow = ImGui::GetIO().MouseReleased[0] && mouse_x > left_arrow_x - font() &&
            mouse_x < left_arrow_x + font() * 0.6f;
        delta = on_left_arrow ? -1 : 1;
    }
    const bool live = r.focused || r.hovered;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw_value(r, value, options, px(16.0f) + font() * 1.1f, live ? colors::kAccentBright : colors::kText);
    // A locked row shows its value without the arrows that would change it.
    if (options.disabled) {
        setting_help_button(label, options, r.min, r.full_max);
        return 0;
    }
    const ImU32 arrow = live ? colors::kAccent : colors::kTextDim;
    draw_triangle(draw, {right_arrow_x, mid_y}, font() * 0.55f, true, arrow);
    draw_triangle(draw, {left_arrow_x, mid_y}, font() * 0.55f, false, arrow);
    setting_help_button(label, options, r.min, r.full_max);
    return delta;
}

bool toggle_row(const char *label, bool value, const RowOptions &options) {
    const Row r = row(label, options);
    const int delta = r.focused ? horizontal_press() : 0;
    const bool toggled = !options.disabled && (r.pressed || (delta > 0 && !value) || (delta < 0 && value));

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float height = font() * 0.95f;
    const float width = height * 1.9f;
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const ImVec2 max{r.max.x - px(16.0f), mid_y + height * 0.5f};
    const ImVec2 min{max.x - width, mid_y - height * 0.5f};
    const bool on = toggled ? !value : value;
    const ImU32 track = options.disabled ? colors::kTrack : on ? colors::kAccent : colors::kTrack;
    draw->AddRectFilled(min, max, track, height * 0.5f);
    const float knob = height * 0.5f - px(3.0f);
    const ImVec2 knob_center{on ? max.x - height * 0.5f : min.x + height * 0.5f, mid_y};
    draw->AddCircleFilled(knob_center, knob, options.disabled ? colors::kTextDisabled : colors::kText);
    draw_value(
        r, on ? "On" : "Off", options, px(16.0f) + width + px(12.0f), on ? colors::kAccentBright : colors::kTextDim);
    setting_help_button(label, options, r.min, r.full_max);
    return toggled;
}

bool slider_row(
    const char *label, int &value, int minimum, int maximum, int step, const char *format, const RowOptions &options) {
    const Row r = row(label, options);
    const int before = value;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const float bar_width = std::min(font() * 10.0f, (r.max.x - r.min.x) * 0.38f);
    const ImVec2 bar_max{r.max.x - px(16.0f), mid_y + px(3.0f)};
    const ImVec2 bar_min{bar_max.x - bar_width, mid_y - px(3.0f)};

    if (!options.disabled) {
        if (r.focused) value += horizontal_press() * step;
        // Touch waits for horizontal intent; a vertical swipe never edits the value.
        static ImGuiID dragging = 0;
        const ImGuiID id = ImGui::GetItemID();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool touch = ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen;
        if (ImGui::IsItemActivated() && mouse.x >= bar_min.x - font() && mouse.x <= bar_max.x + font()) {
            dragging = id;
            ImGui::GetStateStorage()->SetInt(scroll_key("scroll_slider"), static_cast<int>(id));
            if (!touch) set_scroll_intent(ScrollIntent::Horizontal);
        }
        if (dragging == id) {
            const auto intent = scroll_intent();
            const bool horizontal = !touch || intent == ScrollIntent::Horizontal;
            const bool tap =
                touch && r.pressed && intent != ScrollIntent::Vertical && intent != ScrollIntent::Cancelled;
            if ((ImGui::IsMouseDown(0) && horizontal) || tap) {
                const float t = std::clamp((mouse.x - bar_min.x) / bar_width, 0.0f, 1.0f);
                const float raw = static_cast<float>(minimum) + t * static_cast<float>(maximum - minimum);
                value = minimum + static_cast<int>(std::round((raw - minimum) / step)) * step;
            }
            if (!ImGui::IsMouseDown(0) || intent == ScrollIntent::Vertical || intent == ScrollIntent::Cancelled)
                dragging = 0;
        }
        value = std::clamp(value, minimum, maximum);
    }

    const float t = static_cast<float>(value - minimum) / static_cast<float>(std::max(1, maximum - minimum));
    const bool live = (r.focused || r.hovered) && !options.disabled;
    draw->AddRectFilled(bar_min, bar_max, colors::kTrack, px(3.0f));
    draw->AddRectFilled(bar_min, {bar_min.x + bar_width * t, bar_max.y},
        options.disabled ? colors::kTextDisabled : colors::kAccent, px(3.0f));
    draw->AddCircleFilled({bar_min.x + bar_width * t, mid_y}, font() * (live ? 0.42f : 0.34f),
        options.disabled ? colors::kTextDisabled
            : live       ? colors::kAccentBright
                         : colors::kText);
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    draw_value(r, text, options, px(16.0f) + bar_width + font() * 1.0f, live ? colors::kAccentBright : colors::kText);
    setting_help_button(label, options, r.min, r.full_max);
    return value != before;
}

bool button_row(const char *label, const RowOptions &options, ImU32 color) {
    const Row r = row(label, options, color);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const ImU32 chevron = options.disabled ? colors::kTextDisabled
        : (r.focused || r.hovered)         ? colors::kAccent
                                           : colors::kTextDim;
    const float x = r.max.x - px(16.0f) - font() * 0.3f;
    const float s = font() * 0.28f;
    draw->AddLine({x - s, mid_y - s * 1.6f}, {x + s * 0.6f, mid_y}, chevron, px(2.0f));
    draw->AddLine({x + s * 0.6f, mid_y}, {x - s, mid_y + s * 1.6f}, chevron, px(2.0f));
    if (options.disabled && !options.note.empty()) draw_value(r, "", options, px(16.0f) + font() * 1.2f, 0);
    setting_help_button(label, options, r.min, r.full_max);
    return r.pressed && !options.disabled;
}

bool value_row(const char *label, const std::string &value, const RowOptions &options) {
    const Row r = row(label, options);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const bool live = (r.focused || r.hovered) && !options.disabled;
    const ImU32 chevron = options.disabled ? colors::kTextDisabled : live ? colors::kAccent : colors::kTextDim;
    const float x = r.max.x - px(16.0f) - font() * 0.3f;
    const float s = font() * 0.28f;
    draw->AddLine({x - s, mid_y - s * 1.6f}, {x + s * 0.6f, mid_y}, chevron, px(2.0f));
    draw->AddLine({x + s * 0.6f, mid_y}, {x - s, mid_y + s * 1.6f}, chevron, px(2.0f));
    draw_value(r, value, options, px(16.0f) + font() * 1.1f,
        options.warning ? colors::kDanger
            : live      ? colors::kAccentBright
                        : colors::kText);
    setting_help_button(label, options, r.min, r.full_max);
    return r.pressed && !options.disabled;
}

bool list_row(const char *id, const std::string &name, const std::string &detail, ListIcon icon, bool highlight) {
    const Row r = row(id, {});
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const float icon_x = r.min.x + px(18.0f);
    const float unit = font() * 0.5f;
    const ImU32 ink = highlight ? colors::kAccent : colors::kTextDim;
    switch (icon) {
    case ListIcon::Folder:
    case ListIcon::ParentFolder: {
        const ImVec2 min{icon_x, mid_y - unit * 0.7f};
        const ImVec2 max{icon_x + unit * 2.0f, mid_y + unit * 0.8f};
        draw->AddRectFilled({min.x, min.y - unit * 0.3f}, {min.x + unit * 0.9f, min.y + unit * 0.2f}, ink, px(2.0f));
        draw->AddRectFilled(min, max, ink, px(2.0f));
        if (icon == ListIcon::ParentFolder)
            draw->AddTriangleFilled({min.x + unit, min.y + unit * 0.25f}, {min.x + unit * 0.55f, min.y + unit * 0.9f},
                {min.x + unit * 1.45f, min.y + unit * 0.9f}, colors::kPanel);
        break;
    }
    case ListIcon::Disc:
        draw->AddCircle({icon_x + unit, mid_y}, unit * 0.85f, ink, 0, px(2.0f));
        draw->AddCircleFilled({icon_x + unit, mid_y}, unit * 0.25f, ink);
        break;
    case ListIcon::Drive:
        draw->AddRect(
            {icon_x, mid_y - unit * 0.55f}, {icon_x + unit * 2.0f, mid_y + unit * 0.55f}, ink, px(2.0f), 0, px(2.0f));
        draw->AddCircleFilled({icon_x + unit * 1.55f, mid_y}, unit * 0.15f, ink);
        break;
    case ListIcon::File:
        draw->AddRect({icon_x + unit * 0.3f, mid_y - unit * 0.8f}, {icon_x + unit * 1.7f, mid_y + unit * 0.8f}, ink,
            px(2.0f), 0, px(1.5f));
        break;
    case ListIcon::None:
        break;
    }
    const float text_x = icon == ListIcon::None ? r.min.x + px(16.0f) : icon_x + unit * 2.0f + px(14.0f);
    const float text_y = mid_y - font() * 0.5f;
    const float detail_width = ImGui::CalcTextSize(detail.c_str()).x;
    const float detail_x = r.max.x - px(16.0f) - detail_width;
    ImGui::PushClipRect({text_x, r.min.y}, {detail_x - px(12.0f), r.max.y}, true);
    draw->AddText({text_x, text_y}, highlight ? colors::kAccentBright : colors::kText, name.c_str());
    ImGui::PopClipRect();
    draw->AddText({detail_x, text_y}, colors::kTextDim, detail.c_str());
    return r.pressed;
}

void info_row(const char *label, const std::string &value) {
    // Label above, value below and wrapped: paths can be long.
    const float width = ImGui::GetContentRegionAvail().x - px(32.0f);
    const float value_height = ImGui::CalcTextSize(value.c_str(), nullptr, false, width).y;
    const float height = std::round(font() * 1.5f + value_height);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, {ImGui::GetContentRegionAvail().x, height}, ImGuiButtonFlags_EnableNav);
    const bool focused = ImGui::IsItemFocused();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 max{min.x + width + px(32.0f), min.y + height};
    if (focused) draw->AddRectFilled(min, max, colors::kRowFocus, px(6.0f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
    draw->AddText({min.x + px(16.0f), min.y + px(5.0f)}, colors::kTextDim, label, shown_end(label));
    ImGui::PopFont();
    draw->AddText(
        nullptr, 0.0f, {min.x + px(16.0f), min.y + font() * 0.95f}, colors::kText, value.c_str(), nullptr, width);
}

void section(const char *title) {
    ImGui::Dummy({0.0f, px(8.0f)});
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kAccent);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(16.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy({0.0f, px(2.0f)});
}

bool big_button(const char *label, float width, bool primary, bool disabled) {
    const float height =
        std::max(std::round(font() * 2.2f), Layer::get().comfortable() ? Layer::get().touch_target() : 0.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max{min.x + width, min.y + height};
    const bool pressed = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
    take_focus();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = px(8.0f);
    ImU32 text = colors::kText;
    if (disabled) {
        draw->AddRect(min, max, colors::kTextDisabled, rounding, 0, px(1.5f));
        text = colors::kTextDisabled;
    } else if (primary) {
        draw->AddRectFilled(min, max, focused || hovered ? colors::kAccentBright : colors::kAccent, rounding);
        text = colors::kPanel;
    } else {
        draw->AddRectFilled(min, max,
            focused       ? colors::kRowFocus
                : hovered ? colors::kRowHover
                          : colors::kRow,
            rounding);
        draw->AddRect(min, max, focused ? colors::kAccent : colors::kPanelEdge, rounding, 0, px(1.5f));
    }
    if (focused && !disabled)
        draw->AddRect({min.x - px(3.0f), min.y - px(3.0f)}, {max.x + px(3.0f), max.y + px(3.0f)}, colors::kAccentBright,
            rounding + px(3.0f), 0, px(2.0f));
    const char *end = shown_end(label);
    const ImVec2 size = ImGui::CalcTextSize(label, end);
    draw->AddText({min.x + (width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f}, text, label, end);
    return pressed && !disabled;
}

void progress_bar(float fraction, const std::string &overlay) {
    const float height = std::round(font() * 1.6f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({width, height});
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 max{min.x + width, min.y + height};
    draw->AddRectFilled(min, max, colors::kTrack, height * 0.5f);
    const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
    if (filled > 1.0f)
        draw->AddRectFilled(min, {min.x + std::max(filled, height), max.y}, colors::kAccent, height * 0.5f);
    const ImVec2 size = ImGui::CalcTextSize(overlay.c_str());
    draw->AddText({min.x + (width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f}, colors::kText, overlay.c_str());
}

void paragraph(const std::string &text, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void heading(const std::string &text) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kAccentBright);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy({0.0f, px(4.0f)});
}

void hints(std::initializer_list<Hint> list) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const bool pad = Layer::get().input_device() == InputDevice::Gamepad;
    const bool south = Layer::get().confirm_south();
    const SDL_GamepadButton confirm = south ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
    const SDL_GamepadButton back = south ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float available = ImGui::GetContentRegionAvail().x;
    ImVec2 at = start;
    const float gap = px(6.f);
    const auto group = [&](const Hint &hint, ImVec2 at, bool paint) {
        float x = at.x;
        const auto cap = [&](const char *text) {
            x += (paint ? draw_cap(draw, {x, at.y}, text, false)
                        : std::max(font() * 1.15f, ImGui::CalcTextSize(text).x + font() * .7f)) +
                gap;
        };
        const auto face = [&](SDL_GamepadButton button) {
            x += (paint ? draw_face(draw, {x, at.y}, button) : font() * 1.24f) + gap;
        };
        const auto arrows = [&] {
            const float size = font() * 1.15f;
            for (int i = 0; i < 2; ++i) {
                const ImVec2 min{x, at.y + (font() - size) * .5f};
                if (paint) {
                    draw->AddRect(min, {min.x + size, min.y + size}, colors::kTextDim, size * .3f, 0, px(1.5f));
                    draw_triangle(draw, {min.x + size * .5f, min.y + size * .5f}, size * .4f, i == 1, colors::kText);
                }
                x += size + gap;
            }
        };
        switch (hint.control) {
        case Control::Confirm:
            if (pad)
                face(confirm);
            else
                cap("Enter");
            break;
        case Control::Back:
            if (pad)
                face(back);
            else
                cap("Esc");
            break;
        case Control::Tabs:
            cap(pad ? shoulder_name(false) : "Q");
            cap(pad ? shoulder_name(true) : "W");
            break;
        case Control::Change:
            arrows();
            break;
        case Control::Menu:
            if (pad) {
                cap("L3");
                cap("R3");
            } else {
                cap("Esc");
            }
            break;
        case Control::Start:
            cap(pad ? "Start" : "Enter");
            break;
        case Control::Toggle:
        case Control::Space:
            face(SDL_GAMEPAD_BUTTON_NORTH);
            break;
        case Control::Delete:
            if (pad)
                face(back);
            else
                cap("Backspace");
            break;
        case Control::Shift:
            face(SDL_GAMEPAD_BUTTON_WEST);
            break;
        case Control::Reset:
            cap(select_name());
            break;
        case Control::Clear:
            if (pad)
                face(SDL_GAMEPAD_BUTTON_NORTH);
            else
                cap("Del");
            break;
        case Control::Symbols:
            cap(select_name());
            break;
        case Control::Cursor:
            if (pad) {
                cap(shoulder_name(false));
                cap(shoulder_name(true));
            } else {
                arrows();
            }
            break;
        }
        if (paint) draw->AddText({x + px(2.f), at.y}, colors::kTextDim, hint.text);
        return x + px(2.f) + ImGui::CalcTextSize(hint.text).x - at.x;
    };
    for (const Hint &hint : list) {
        const bool pad_only = hint.control == Control::Toggle || hint.control == Control::Shift ||
            hint.control == Control::Space || hint.control == Control::Symbols || hint.control == Control::Reset;
        if (pad_only && !pad) continue;
        const float width = group(hint, {}, false);
        if (at.x > start.x && at.x + width > start.x + available) {
            at.x = start.x;
            at.y += font() * 1.5f;
        }
        group(hint, at, true);
        at.x += width + px(26.f);
    }
    const float height = at.y - start.y + font() * 1.2f;
    auto *window = ImGui::GetCurrentWindow();
    window->StateStorage.SetFloat(ImHashStr("hint_height", 0, window->ID), height);
    window->StateStorage.SetFloat(ImHashStr("hint_width", 0, window->ID), available);
    window->StateStorage.SetFloat(ImHashStr("hint_font", 0, window->ID), font());
    window->StateStorage.SetInt(ImHashStr("hint_device", 0, window->ID), static_cast<int>(Layer::get().input_device()));
    ImGui::Dummy({available, height});
}

} // namespace mhp3rd::ui
