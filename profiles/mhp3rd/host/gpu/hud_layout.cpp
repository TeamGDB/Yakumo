#include "gpu/hud_layout.hpp"

#include <algorithm>
#include <cmath>

namespace mhp3rd::gpu::hud {
namespace {

// Quest packet/sprite traces cover the clock, bars, party plates, map and
// item selector. Reserve space for four party slots and expanded selection.
constexpr std::array<Rect, 3> kBounds{{{0, 0, 350, 152}, {368, 0, 480, 132}, {304, 168, 480, 272}}};

float finite(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}
float scale(float value) {
    return std::clamp(finite(value, 1.0f), kMinScale, kMaxScale);
}
Rect original(Part part, float fit_x, float fit_y) {
    Rect r = kBounds[index(part)];
    r.left = 240.0f + (r.left - 240.0f) * fit_x;
    r.right = 240.0f + (r.right - 240.0f) * fit_x;
    r.top = 136.0f + (r.top - 136.0f) * fit_y;
    r.bottom = 136.0f + (r.bottom - 136.0f) * fit_y;
    return r;
}
float offset(float value, float first, float last, float extent) {
    const float a = -first, b = extent - last;
    return std::clamp(value, std::min(a, b), std::max(a, b));
}

} // namespace

bool Transform::identity() const {
    return scale == 1.0f && x == 0.0f && y == 0.0f;
}
void Transform::point(float &px, float &py) const {
    px = px * scale + x;
    py = py * scale + y;
}
void Transform::clip(float (&edges)[4], bool full_x, bool full_y) const {
    point(edges[0], edges[1]);
    point(edges[2], edges[3]);
    if (full_x) {
        edges[0] = 0.0f;
        edges[2] = 480.0f;
    }
    if (full_y) {
        edges[1] = 0.0f;
        edges[3] = 272.0f;
    }
}
std::size_t index(Part part) {
    return part == Part::Map ? 1u : part == Part::Items ? 2u : 0u;
}
const char *name(Part part) {
    return part == Part::Status ? "Status" : part == Part::Map ? "Map" : part == Part::Items ? "Items" : "";
}
Transform transform(const Layout &layout, Part part, float fit_x, float fit_y) {
    if (part == Part::None || part == Part::Other) return {};
    const Placement &p = layout.blocks[index(part)];
    // Preserve an exact identity path for the unmodified HUD.
    if (p.scale == 1.0f && p.x == 0.0f && p.y == 0.0f) return {};
    const float combined = scale(p.scale);
    const float anchor_x = 240.0f + ((part == Part::Status ? 0.0f : 480.0f) - 240.0f) * fit_x;
    const float anchor_y = 136.0f + ((part == Part::Items ? 272.0f : 0.0f) - 136.0f) * fit_y;
    Transform t{combined, anchor_x * (1.0f - combined), anchor_y * (1.0f - combined)};
    Rect r = original(part, fit_x, fit_y);
    t.point(r.left, r.top);
    t.point(r.right, r.bottom);
    t.x += offset(std::clamp(finite(p.x, 0.0f), -1.0f, 1.0f) * 480.0f, r.left, r.right, 480.0f);
    t.y += offset(std::clamp(finite(p.y, 0.0f), -1.0f, 1.0f) * 272.0f, r.top, r.bottom, 272.0f);
    return t;
}
Rect bounds(const Layout &layout, Part part, float fit_x, float fit_y) {
    if (part == Part::None || part == Part::Other) return {};
    Rect r = original(part, fit_x, fit_y);
    const Transform t = transform(layout, part, fit_x, fit_y);
    t.point(r.left, r.top);
    t.point(r.right, r.bottom);
    return r;
}
Rect screen_bounds(const Layout &layout, Part part, const Rect &picture, float fit_x, float fit_y) {
    if (!std::isfinite(picture.left) || !std::isfinite(picture.top) || !std::isfinite(picture.right) ||
        !std::isfinite(picture.bottom) || picture.right <= picture.left || picture.bottom <= picture.top)
        return {};
    const auto r = bounds(layout, part, fit_x, fit_y);
    const float w = (picture.right - picture.left) / 480.f, h = (picture.bottom - picture.top) / 272.f;
    return {picture.left + std::clamp(r.left, 0.f, 480.f) * w, picture.top + std::clamp(r.top, 0.f, 272.f) * h,
        picture.left + std::clamp(r.right, 0.f, 480.f) * w, picture.top + std::clamp(r.bottom, 0.f, 272.f) * h};
}
void move_to(Layout &layout, Part part, float left, float top, float fit_x, float fit_y) {
    if (part == Part::None || part == Part::Other || !std::isfinite(left) || !std::isfinite(top)) return;
    Layout centred = layout;
    Placement &p = centred.blocks[index(part)];
    p.x = p.y = 0.0f;
    const Rect r = bounds(centred, part, fit_x, fit_y);
    Placement &out = layout.blocks[index(part)];
    out.x = offset(left - r.left, r.left, r.right, 480.0f) / 480.0f;
    out.y = offset(top - r.top, r.top, r.bottom, 272.0f) / 272.0f;
}

void move_by(Layout &layout, Part part, float x, float y, float seconds, float fit_x, float fit_y) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(seconds) || seconds <= 0.0f) return;
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    const float length = std::hypot(x, y);
    if (length == 0.0f) return;
    const float distance = 120.0f * std::min(seconds, 0.1f) / std::max(length, 1.0f);
    const Rect r = bounds(layout, part, fit_x, fit_y);
    move_to(layout, part, r.left + x * distance, r.top + y * distance, fit_x, fit_y);
}

} // namespace mhp3rd::gpu::hud