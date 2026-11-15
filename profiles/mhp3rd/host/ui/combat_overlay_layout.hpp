#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mhp3rd::ui::combat_layout {
struct Rect {
    float x{}, y{}, width{}, height{};
};
struct Stack {
    Rect bounds;
    float font{}, row{}, gap{}, margin{}, free_x{}, free_y{}, origin_x{}, origin_y{};
};
inline float fraction(float value, float fallback) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : fallback;
}
// Coordinates describe the available space, keeping the entire stack inside
// the game picture after a resize, aspect change or monster-count change.
inline Stack place(Rect picture, float font, std::size_t count, float x, float y) {
    if (!count || count > 20 || !std::isfinite(picture.x) || !std::isfinite(picture.y) ||
        !std::isfinite(picture.width) || !std::isfinite(picture.height) || picture.width <= 0.f ||
        picture.height <= 0.f || !std::isfinite(font) || font <= 0.f)
        return {};
    Stack s;
    s.margin = std::min({font * 0.6f, picture.width * 0.02f, picture.height * 0.02f});
    const float available_height = picture.height - 2.f * s.margin;
    s.font = std::min(font, available_height / (2.1f * static_cast<float>(count) - 0.2f));
    s.font = std::min(s.font, (picture.width - 2.f * s.margin) / 18.f);
    s.row = 1.9f * s.font;
    s.gap = 0.2f * s.font;
    s.bounds.width = std::min(s.font * 16.f, picture.width * 0.40f);
    s.bounds.height = s.row * static_cast<float>(count) + s.gap * static_cast<float>(count - 1);
    s.origin_x = picture.x + s.margin;
    s.origin_y = picture.y + s.margin;
    s.free_x = std::max(0.f, picture.width - 2.f * s.margin - s.bounds.width);
    s.free_y = std::max(0.f, available_height - s.bounds.height);
    s.bounds.x = s.origin_x + s.free_x * fraction(x, 0.5f);
    s.bounds.y = s.origin_y + s.free_y * fraction(y, 0.f);
    return s;
}
inline std::array<float, 2> move_to(const Stack &s, float left, float top) {
    return {s.free_x > 0.f ? fraction((left - s.origin_x) / s.free_x, 0.5f) : 0.5f,
        s.free_y > 0.f ? fraction((top - s.origin_y) / s.free_y, 0.f) : 0.f};
}
inline constexpr std::array<std::array<float, 2>, 6> kPresets{
    {{0.f, 0.f}, {0.5f, 0.f}, {1.f, 0.f}, {0.f, 1.f}, {0.5f, 1.f}, {1.f, 1.f}}};
inline constexpr std::array<const char *, 6> kNames{
    "Top Left", "Top Center", "Top Right", "Bottom Left", "Bottom Center", "Bottom Right"};
inline int preset(float x, float y) {
    for (std::size_t i = 0; i < kPresets.size(); ++i)
        if (std::abs(x - kPresets[i][0]) < 0.0001f && std::abs(y - kPresets[i][1]) < 0.0001f)
            return static_cast<int>(i);
    return -1;
}
}
