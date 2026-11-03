#pragma once

// A distinct mobile layout: floating movement, a right-thumb action arc and
// a HUD-linked item surface (a small strip when the HUD is absent). Only primitive safe-area geometry is shared with other
// layouts; input ownership and the element list are independent.
#include "input/touch_geometry.hpp"
#include "input/adaptive_attack.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mhp3rd::input::touch {
enum class WeaponPosture : std::uint8_t { Unknown, Sheathed, Drawn };
enum class AdaptiveMode : std::uint8_t { Quest, Hub, Interface };
enum class AdaptiveElement : std::uint8_t {
    Stick,
    Attack,
    Alternate,
    Special,
    Evade,
    Interact,
    ItemBar,
    DPad,
    DpadToggle,
    R3,
    Pause,
    Start,
    Select,
    LeftShoulder,
    InterfaceToggle,
    LeftFire,
    YakumoMenu,
    Count
};
inline constexpr auto kAdaptiveElements = static_cast<std::size_t>(AdaptiveElement::Count);
inline constexpr float kItemBarAspect = 4.2f;
struct AdaptiveLayout {
    std::array<Placement, kAdaptiveElements> elements{};
    bool floating_stick{true};
    Placement &at(AdaptiveElement element);
    const Placement &at(AdaptiveElement element) const;
    bool operator==(const AdaptiveLayout &) const = default;
};
struct AdaptiveInfo {
    const char *key;
    const char *name;
};
[[nodiscard]] const AdaptiveLayout &default_adaptive_layout(AdaptiveMode mode = AdaptiveMode::Quest);
[[nodiscard]] const AdaptiveInfo &info(AdaptiveElement element);
[[nodiscard]] bool rebindable(AdaptiveElement element);
bool parse_adaptive_placement(AdaptiveElement element, std::string_view text, Placement &placement);
[[nodiscard]] Placed place(const Placement &placement, AdaptiveElement element, const Area &area, float scale,
    AttackProfile profile = AttackProfile::Off);
[[nodiscard]] float adaptive_hit_radius(const Placed &placed, const Area &area);
[[nodiscard]] Placement move_to(const Placement &placement, AdaptiveElement element, Point centre, const Area &area);

class AdaptiveControls {
public:
    void set_bow_toggle(bool enabled);
    [[nodiscard]] bool bow_toggle_enabled() const;
    [[nodiscard]] bool bow_aiming() const;
    [[nodiscard]] std::uint16_t sample_fire_buttons(bool advance);
    void set_compact(bool enabled);
    [[nodiscard]] bool compact() const;
    [[nodiscard]] bool commands_open() const;
    [[nodiscard]] AttackProfile weapon_profile() const;
    [[nodiscard]] AttackDirection special_direction(std::uint64_t ms) const;
    void set_mode(AdaptiveMode mode);
    [[nodiscard]] AdaptiveMode mode() const;
    [[nodiscard]] bool take_interface_toggle();
    void set_layout(const AdaptiveLayout &layout, const Area &area, float scale);
    // Window coordinates, already clipped to the game picture. nullopt uses
    // the editable fallback strip. Moving/hiding the HUD cancels only its owner.
    void set_item_area(std::optional<Area> area);
    [[nodiscard]] bool items_follow_hud() const;
    void set_weapon_posture(WeaponPosture posture);
    [[nodiscard]] WeaponPosture weapon_posture() const;
    void set_attack_profile(AttackProfile profile);
    [[nodiscard]] AttackProfile attack_profile() const;
    void set_available(std::optional<std::uint8_t> mask);
    [[nodiscard]] std::optional<std::uint8_t> available() const;
    [[nodiscard]] const AdaptiveLayout &layout() const;
    [[nodiscard]] const Area &area() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] const Placed &placed(AdaptiveElement element) const;
    [[nodiscard]] bool visible(AdaptiveElement element) const;
    [[nodiscard]] bool quest_unavailable() const;
    void finger_down(std::uint64_t id, Point at, std::uint64_t ms);
    void finger_move(std::uint64_t id, Point at, std::uint64_t ms);
    void finger_up(std::uint64_t id, std::uint64_t ms, bool canceled = false);
    void release_all();
    [[nodiscard]] std::uint16_t buttons(std::uint64_t ms) const;
    // Item gestures are queued separately from held controls. Advance only on
    // pad samples the game reads, never on UI polls or interpolated presents.
    [[nodiscard]] std::uint16_t sample_item_buttons(bool advance);
    [[nodiscard]] Point stick(std::uint64_t ms) const;
    [[nodiscard]] Point stick_origin() const;
    [[nodiscard]] Point stick_thumb() const;
    [[nodiscard]] bool held(AdaptiveElement element) const;
    [[nodiscard]] std::uint16_t dpad_held() const;
    [[nodiscard]] AttackDirection attack_direction(
        std::uint64_t ms, AdaptiveElement element = AdaptiveElement::Attack) const;
    // Visual cap position: follows recognition, then stays on the latched action.
    [[nodiscard]] Point attack_thumb(std::uint64_t ms, AdaptiveElement element = AdaptiveElement::Attack) const;
    [[nodiscard]] Point take_camera_drag();
    [[nodiscard]] bool take_menu();
    [[nodiscard]] bool take_dpad_toggle();
    [[nodiscard]] int take_haptics();

private:
    enum class Role : std::uint8_t { None, Move, Attack, Special, Button, DPad, Camera, Items, Ammo, AimToggle, Fire };
    struct Finger {
        std::uint64_t id{}, started{};
        bool used{};
        Role role{Role::None};
        AdaptiveElement element{AdaptiveElement::Count};
        Point at{}, start{};
        bool over{}, swiped{};
    };
    Finger *find(std::uint64_t id);
    std::optional<AdaptiveElement> hit(Point at) const;
    [[nodiscard]] bool has_camera_owner() const;
    [[nodiscard]] bool ammo_busy() const;
    bool queue_item(std::uint16_t buttons);
    void clear_items();
    void place_commands();
    enum class FirePhase : std::uint8_t { Idle, Held, Tap, Cancel };
    [[nodiscard]] bool bow_toggle_available() const;
    [[nodiscard]] std::uint16_t fire_buttons() const;
    void cancel_fire(bool leave_aim);
    bool bow_toggle_{true}, aiming_{}, fire_sampled_{}, fire_arc_{}, arc_sampled_{};
    FirePhase fire_phase_{};
    unsigned fire_reads_{};
    bool compact_{true}, commands_{};
    std::uint16_t command_pulse_{};
    std::uint64_t command_released_{};
    bool camera_reset_{};
    std::uint64_t camera_released_{};
    AdaptiveAttack special_;
    AdaptiveLayout layout_;
    AdaptiveMode mode_{AdaptiveMode::Quest};
    WeaponPosture posture_{WeaponPosture::Unknown};
    bool interface_toggle_{};
    std::optional<Area> item_area_;
    Area area_{};
    float scale_{1};
    std::array<Placed, kAdaptiveElements> placed_{};
    std::array<Finger, 10> fingers_{};
    std::optional<std::uint64_t> mover_, item_owner_;
    Point origin_{}, thumb_{}, camera_{};
    std::optional<std::uint8_t> available_;
    AdaptiveAttack attack_;
    AttackProfile profile_{AttackProfile::Off};
    bool menu_{}, dpad_toggle_{};
    int haptics_{};
    // Bounded FIFO: opposite swipes must never combine Square and Circle.
    std::array<std::uint16_t, 16> item_queue_{};
    std::size_t item_head_{}, item_count_{};
    unsigned item_lead_reads_{}, item_shown_reads_{};
    bool item_gap_{};
};
} // namespace mhp3rd::input::touch
