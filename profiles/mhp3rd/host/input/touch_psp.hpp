#pragma once

#include "input/touch_action.hpp"

namespace mhp3rd::input::touch {

// PSP buttons keep their existing indices; the editor also places the D-pad and stick.
enum class PspElement : std::uint8_t {
    Triangle,
    Circle,
    Cross,
    Square,
    L,
    R,
    Start,
    Select,
    Menu,
    R3,
    DpadToggle,
    Combo1,
    Combo2,
    Combo3,
    Combo4,
    Combo5,
    Combo6,
    Combo7,
    Combo8,
    DPad,
    Stick,
    Count
};
inline constexpr std::size_t kPspElements = static_cast<std::size_t>(PspElement::Count);
inline constexpr std::size_t kPspCombos = 8u;

struct PspLayout {
    std::array<Placement, kPspElements> elements{};
    bool floating_stick{true};
    [[nodiscard]] Placement &at(PspElement e) { return elements[static_cast<std::size_t>(e)]; }
    [[nodiscard]] const Placement &at(PspElement e) const { return elements[static_cast<std::size_t>(e)]; }
    friend bool operator==(const PspLayout &, const PspLayout &) = default;
};

[[nodiscard]] const PspLayout &default_psp_layout();
[[nodiscard]] const ElementInfo &info(PspElement element);
[[nodiscard]] bool rebindable(PspElement element);
[[nodiscard]] bool is_combo(PspElement element);
// Invalid PSP masks leave the old placement intact. The menu cannot be hidden; host controls cannot be rebound.
bool parse_psp_placement(PspElement element, std::string_view text, Placement &placement);
[[nodiscard]] std::optional<PspElement> add_psp_combo(PspLayout &layout);
void remove_psp_combo(PspLayout &layout, PspElement element);

[[nodiscard]] Layout make_layout(
    float width, float height, Insets insets, float size, bool dpad, const PspLayout &custom);
[[nodiscard]] Circle psp_element_circle(const Layout &layout, PspElement element);

} // namespace mhp3rd::input::touch
