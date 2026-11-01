#pragma once

#include <array>
#include <cstddef>

namespace mhp3rd::gpu::hud {

// Other includes world-space names and prompts: hidden with the HUD,
// but never moved with a screen-space block.
enum class Part { None, Other, Status, Map, Items };
inline constexpr std::array<Part, 3> kParts{Part::Status, Part::Map, Part::Items};
inline constexpr float kMinScale = 0.5f;
inline constexpr float kMaxScale = 2.0f;

struct Placement {
    float scale{1.0f};
    // Offset from the original corner, as a fraction of the game picture.
    float x{};
    float y{};
    bool operator==(const Placement &) const = default;
};
struct Layout {
    std::array<Placement, 3> blocks{};
    bool operator==(const Layout &) const = default;
};
struct Rect {
    float left{}, top{}, right{}, bottom{};
};
struct Transform {
    float scale{1.0f};
    float x{}, y{};
    [[nodiscard]] bool identity() const;
    void point(float &px, float &py) const;
    // Full-picture clips stay full-picture, partial clips follow the block.
    void clip(float (&edges)[4], bool full_x, bool full_y) const;
};

[[nodiscard]] std::size_t index(Part part);
[[nodiscard]] const char *name(Part part);
// In 480x272 space, after the renderer's optional Fill fitting. Anchors
// preserve the original top-left, top-right and bottom-right corners.
[[nodiscard]] Transform transform(const Layout &layout, Part part, float fit_x = 1.0f, float fit_y = 1.0f);
[[nodiscard]] Rect bounds(const Layout &layout, Part part, float fit_x = 1.0f, float fit_y = 1.0f);
// Map a block to a window-space picture rectangle, clipping off-screen edges.
[[nodiscard]] Rect screen_bounds(
    const Layout &layout, Part part, const Rect &picture, float fit_x = 1.0f, float fit_y = 1.0f);
// Move a block's top-left here, keeping it on screen. Oversized blocks can
// be panned across the picture.
void move_to(Layout &layout, Part part, float left, float top, float fit_x = 1.0f, float fit_y = 1.0f);

// Continuous keyboard/gamepad movement, in game pixels per second. Diagonals
// have the same speed as cardinal directions and long UI stalls cannot jump.
void move_by(Layout &layout, Part part, float x, float y, float seconds, float fit_x = 1.0f, float fit_y = 1.0f);

} // namespace mhp3rd::gpu::hud