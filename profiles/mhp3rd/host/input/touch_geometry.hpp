#pragma once
#include "input/touch_controls.hpp"
#include <cstdint>
#include <string>
#include <string_view>
namespace mhp3rd::input::touch {
// Which edge of the safe area an element keeps its distance from, so the
// layout fits screens of any width: the left thumb's elements to the left
// edge, the right thumb's to the right one, the swipe area to the middle.
enum class Anchor : std::uint8_t { Left, Centre, Right };

struct Placement {
    Anchor anchor{Anchor::Left};
    float x{};               // the centre from the anchor (right: leftwards), in heights of the safe area
    float y{};               // the centre from the safe area's top, in heights of the safe area
    float size{};            // the radius in heights of the safe area; the swipe area's half height
    std::uint16_t buttons{}; // SceCtrlButtons pressed; ignored for the stick, Pause, R3 and swipe area
    bool shown{true};
    friend bool operator==(const Placement &, const Placement &) = default;
};

// An element in window pixels, for a layout on a screen.
struct Placed {
    Point centre;
    float radius{};     // for the swipe area, its half height
    float half_width{}; // for the swipe area; the radius for the others
};
struct Area {
    float left{};
    float top{};
    float width{};
    float height{};
};

inline constexpr float kMinElementSize = 0.02f;
inline constexpr float kMaxElementSize = 0.25f;
[[nodiscard]] std::string format(const Placement &placement);
bool parse(std::string_view text, Placement &placement);
[[nodiscard]] Area safe_area(float width, float height, Insets insets);
[[nodiscard]] Placed place(const Placement &placement, const Area &area, float scale, float aspect = 1.0f);
[[nodiscard]] Placement move_to(const Placement &placement, Point centre, const Area &area, bool central = false);
} // namespace mhp3rd::input::touch
