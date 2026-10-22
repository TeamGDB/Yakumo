#pragma once

#include "input/touch_controls.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mhp3rd::input::touch {

enum class Element : std::uint8_t {
    Stick,
    Item,
    DpadUp,
    DpadDown,
    Attack,
    Secondary,
    Combo,
    Evade,
    Use,
    Guard,
    Special,
    Pause,
    Start,
    Select,
    Swipe,
    R3,
    DPad,
    DpadToggle,
    Count
};
inline constexpr std::size_t kElements = static_cast<std::size_t>(Element::Count);

enum class Anchor : std::uint8_t { Left, Centre, Right };

struct Placement {
    Anchor anchor{Anchor::Left};
    float x{};
    float y{};
    float size{};
    std::uint16_t buttons{};
    bool shown{true};
    friend bool operator==(const Placement &, const Placement &) = default;
};

struct ActionLayout {
    std::array<Placement, kElements> elements{};
    friend bool operator==(const ActionLayout &, const ActionLayout &) = default;
    [[nodiscard]] Placement &at(Element e) { return elements[static_cast<std::size_t>(e)]; }
    [[nodiscard]] const Placement &at(Element e) const { return elements[static_cast<std::size_t>(e)]; }
};

[[nodiscard]] const ActionLayout &default_action_layout();

struct ElementInfo {
    const char *key;
    const char *name;
};
[[nodiscard]] const ElementInfo &info(Element element);
[[nodiscard]] bool rebindable(Element element);
inline constexpr float kSwipeAspect = 3.2f;
inline constexpr float kMinElementSize = 0.02f;
inline constexpr float kMaxElementSize = 0.25f;

struct ButtonChoice {
    std::uint16_t buttons;
    const char *label;
};
[[nodiscard]] const std::vector<ButtonChoice> &button_choices();
[[nodiscard]] std::string buttons_label(std::uint16_t buttons);

[[nodiscard]] std::string format(const Placement &placement);
bool parse(std::string_view text, Placement &placement);

struct Placed {
    Point centre;
    float radius{};
    float half_width{};
};
struct Area {
    float left{};
    float top{};
    float width{};
    float height{};
};
[[nodiscard]] Area safe_area(float width, float height, Insets insets);
[[nodiscard]] Placed place(const Placement &placement, Element element, const Area &area, float scale);
[[nodiscard]] Placement move_to(const Placement &placement, Element element, Point centre, const Area &area);

class ActionControls {
public:
    static constexpr float kSwipeDistance = 0.07f;
    static constexpr std::uint64_t kSwipeWindowMs = 280u;
    static constexpr std::uint64_t kSwipePressMs = 130u;

    void set_layout(const ActionLayout &layout, const Area &area, float scale);
    [[nodiscard]] const ActionLayout &layout() const { return layout_; }
    [[nodiscard]] const Area &area() const { return area_; }
    [[nodiscard]] float scale() const { return scale_; }
    [[nodiscard]] const Placed &placed(Element element) const { return placed_[static_cast<std::size_t>(element)]; }

    void finger_down(std::uint64_t id, Point at, std::uint64_t ms);
    void finger_move(std::uint64_t id, Point at, std::uint64_t ms);
    void finger_up(std::uint64_t id, std::uint64_t ms);
    void release_all();

    [[nodiscard]] std::uint16_t buttons(std::uint64_t ms) const;
    [[nodiscard]] Point stick() const;
    [[nodiscard]] Point stick_thumb() const { return stick_thumb_; }
    [[nodiscard]] bool stick_held() const { return stick_finger_.has_value(); }
    [[nodiscard]] bool held(Element element) const;
    [[nodiscard]] Point take_camera_drag();
    [[nodiscard]] bool take_menu();
    [[nodiscard]] bool take_dpad_toggle();
    [[nodiscard]] std::uint16_t dpad_held() const;
    [[nodiscard]] int take_haptics();
    [[nodiscard]] int swipe_shown(std::uint64_t ms) const;

private:
    enum class Role : std::uint8_t { None, Stick, Button, Camera, Swipe, DPad };
    struct Finger {
        std::uint64_t id{};
        bool used{};
        Role role{};
        Element element{Element::Count};
        bool over{};
        Point last;
        Point start;
        std::uint64_t start_ms{};
        bool swiped{};
        Point pending;
        bool attack_drag{};
    };
    static constexpr std::size_t kFingers = 10u;
    [[nodiscard]] std::optional<Element> button_at(Point at, bool cluster_only) const;
    [[nodiscard]] bool in_swipe_area(Point at) const;
    Finger *find(std::uint64_t id);
    void swipe(int direction, std::uint64_t ms);
    [[nodiscard]] std::uint16_t attack_drag_buttons(const Finger &finger) const;

    ActionLayout layout_{default_action_layout()};
    Area area_{};
    float scale_{1.0f};
    std::array<Placed, kElements> placed_{};
    std::array<Finger, kFingers> fingers_{};
    std::optional<std::uint64_t> stick_finger_;
    Point stick_thumb_{};
    Point camera_drag_{};
    bool menu_tapped_{};
    bool dpad_toggled_{};
    int haptics_{};
    std::uint64_t swipe_until_[2]{};
};

} // namespace mhp3rd::input::touch
