// Independent public synthetic gestures: timing, ownership and PSP outputs.
#include "input/adaptive_attack.hpp"
#include "input/touch_adaptive.hpp"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
using namespace mhp3rd::input::touch;
int failures{};
void check(bool value, const char *why) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << why << '\n';
    }
}
void timing() {
    AdaptiveAttack a;
    check(!a.held() && a.direction(0) == AttackDirection::None && a.output(0).buttons == 0, "initially released");
    check(a.begin(1, {100, 100}, 50, 1000), "one owner starts a gesture");
    check(
        a.output(1000).buttons == 0 && a.output(1109).buttons == 0, "recognition does not emit a primary attack first");
    check(a.output(1110).buttons == 0x1000, "a deliberate hold selects Triangle");
    a.move(1, {100, 0}, 1111);
    check(a.output(1111).buttons == 0x1000, "a primary hold cannot later switch to a different attack");
    a.end(1, 1112, true);
    check(!a.held() && a.output(1112).buttons == 0, "cancellation never creates a release pulse");
    a.begin(2, {125, 100}, 50, 2000);
    a.end(2, 2010, false);
    check(a.output(2010).buttons == 0x1000 && a.output(2094).buttons == 0x1000 && a.output(2095).buttons == 0,
        "a quick tap survives two game frames and expires at its exact boundary");
    a.begin(3, {125, 100}, 50, 2100);
    check(a.output(2100).buttons == 0, "a new gesture replaces a previous pulse without combining actions");
    a.end(3, 2099, false);
    check(a.output(2100).buttons == 0, "backwards release timestamps cannot create an attack");
    a.move(99, {0, 0}, 3000);
    a.end(99, 3000, false);
    check(!a.held(), "unknown motion and releases are ignored");
}
void directions() {
    struct Case {
        Point delta;
        AttackDirection direction;
        std::uint16_t buttons;
        float lateral;
    };
    const Case cases[] = {{{0, -40}, AttackDirection::Up, 0x2000, 0}, {{0, 40}, AttackDirection::Down, 0x3000, 0},
        {{-40, 0}, AttackDirection::Left, 0x3000, -1}, {{40, 0}, AttackDirection::Right, 0x3000, 1},
        {{40, 40}, AttackDirection::Down, 0x3000, 0}};
    for (const auto &c : cases) {
        AdaptiveAttack a;
        a.begin(1, {120, 80}, 50, 1000);
        a.move(1, {121, 81}, 1001);
        check(
            a.direction(1001) == AttackDirection::Pending, "off-centre origin and small finger jitter remain pending");
        a.move(1, {120 + c.delta.x, 80 + c.delta.y}, 1020);
        check(a.direction(1020) == c.direction && a.output(1020).buttons == c.buttons &&
                a.output(1020).lateral == c.lateral && (a.output(1020).buttons & 0xF0) == 0,
            "direction selects one action with analog-only lateral movement");
        a.move(1, {120 - c.delta.x, 80 - c.delta.y}, 1021);
        check(a.direction(1021) == c.direction, "direction remains latched through diagonal/reversal noise");
        a.end(1, 1030, false);
        check(a.output(1030).buttons == c.buttons && a.output(1030).lateral == c.lateral,
            "a short directional flick is retained until the game can sample it");
        check(a.output(1115).buttons == 0 && a.output(1115).lateral == 0,
            "release pulse ends both buttons and analog override");
    }
}
void invalid_and_ownership() {
    AdaptiveAttack a;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float radius : {0.0f, -1.0f, nan, std::numeric_limits<float>::infinity()})
        check(!a.begin(1, {0, 0}, radius, 0), "invalid geometry cannot acquire ownership");
    check(!a.begin(1, {nan, 0}, 50, 0), "nonfinite origin is rejected");
    a.begin(1, {100, 100}, 50, 10);
    check(!a.begin(2, {100, 100}, 50, 11), "second finger cannot select another sector");
    a.move(2, {100, 0}, 12);
    a.end(2, 13, false);
    check(a.held() && a.direction(13) == AttackDirection::Pending, "a non-owner cannot alter or release the gesture");
    a.move(1, {nan, 100}, 14);
    check(!a.held() && a.output(14).buttons == 0, "nonfinite owner motion cancels safely");
    a.begin(1, {0, 0}, 50, 100);
    a.move(1, {0, 100}, 99);
    check(!a.held(), "backwards motion timestamps cancel safely");
    const auto high = std::numeric_limits<std::uint64_t>::max();
    a.begin(1, {0, 0}, 50, high - 20);
    a.end(1, high - 10, false);
    check(a.output(high).buttons == 0x1000 && a.output(0).buttons == 0,
        "pulse timing does not overflow at the clock boundary");
    a.clear();
    check(a.output(high).buttons == 0, "clear removes pending pulses");
}
void common_controls() {
    const Area area{0, 0, 1000, 600};
    AdaptiveControls c;
    c.set_compact(false);
    auto layout = default_adaptive_layout();
    c.set_layout(layout, area, 1);
    const auto at = c.placed(AdaptiveElement::Attack).centre;
    c.finger_down(1, at, 0);
    check(c.buttons(0) == 0x1000, "ordinary Adaptive fallback behavior is immediate with the profile off");
    c.release_all();
    c.set_attack_profile(AttackProfile::LongSword);
    c.finger_down(1, {at.x + 25, at.y}, 1000);
    check(c.buttons(1000) == 0 && c.held(AdaptiveElement::Attack),
        "off-centre down does not emit a directional combination");
    c.finger_down(2, at, 1001);
    c.finger_move(2, {at.x, at.y + 100}, 1002);
    check(c.buttons(1002) == 0, "a second Attack finger cannot inject another sector");
    c.finger_move(1, {at.x + 25, at.y - 70}, 1020);
    check(c.buttons(1020) == 0x2000, "captured drag selects Circle without an earlier Triangle");
    c.finger_down(3, {500, 500}, 1021);
    c.finger_move(3, {510, 520}, 1022);
    const auto camera = c.take_camera_drag();
    check(
        camera.x == 10 && camera.y == 20 && c.buttons(1022) == 0x2000, "another finger moves the camera independently");
    const auto evade = c.placed(AdaptiveElement::Evade).centre;
    c.finger_down(4, evade, 1023);
    c.finger_move(4, at, 1024);
    check(c.buttons(1024) == 0x2000, "sliding a second finger onto Attack cannot bypass gesture ownership");
    c.finger_up(4, 1025, true);
    c.finger_up(1, 1030, true);
    c.finger_up(2, 1030, true);
    check(c.buttons(1030) == 0 && !c.held(AdaptiveElement::Attack),
        "cancellation clears the gesture without a phantom tap");
    c.release_all();
    c.finger_down(1, at, 2000);
    c.finger_down(1, at, 2001);
    check(c.buttons(2001) == 0, "duplicate IDs cancel the old touch rather than synthesize a tap");
    c.finger_move(1, {at.x - 70, at.y}, 2020);
    check(c.stick(2020).x == -1 && c.stick(2020).y == 0 && (c.buttons(2020) & 0xF0) == 0,
        "side selection drives the analog stick instead of the camera D-pad");
    c.set_layout(layout, {0, 0, 1200, 600}, 1);
    check(c.buttons(2021) == 0 && c.stick(2021).x == 0, "resize cancels an active gesture");
    for (const auto binding : {0u, 0x8000u}) {
        layout.at(AdaptiveElement::Attack).buttons = static_cast<std::uint16_t>(binding);
        c.set_layout(layout, area, 1);
        c.finger_down(1, at, 3000);
        check(c.attack_profile() == AttackProfile::Off && c.buttons(3000) == binding,
            "custom Attack bindings and None are respected");
        c.release_all();
    }
    c.set_layout(default_adaptive_layout(), area, 1);
    c.finger_down(1, at, 4000);
    c.set_attack_profile(AttackProfile::Off);
    check(c.buttons(4010) == 0, "weapon/profile switch releases an active touch");
    c.set_attack_profile(AttackProfile::LongSword);
    for (unsigned i = 0; i < 1000; ++i) {
        c.finger_down(1, at, 5000 + i * 10);
        c.finger_up(1, 5001 + i * 10, true);
        check(c.buttons(5002 + i * 10) == 0, "bounded repeated cancellation never sticks");
    }
}
void visual_feedback() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_compact(false);
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::LongSword);
    const auto home = c.placed(E::Attack).centre;
    const float r = c.placed(E::Attack).radius;
    const auto centred = [&](std::uint64_t ms) {
        const auto p = c.attack_thumb(ms);
        return p.x == home.x && p.y == home.y;
    };
    check(centred(0), "idle attack cap rests at its own edited centre");
    c.finger_down(1, {home.x + 5, home.y}, 1000);
    check(centred(1000), "off-centre touch does not jump the visual attack cap");
    c.finger_move(1, {home.x + 15, home.y + 5}, 1010);
    const auto pending = c.attack_thumb(1010);
    check(pending.x > home.x && pending.y > home.y && c.buttons(1010) == 0,
        "pending recognition gives visual finger feedback without an attack");
    c.release_all();
    for (const Point delta : {Point{-r, 0}, {r, 0}, {0, -r}, {0, r}}) {
        c.finger_down(1, home, 2000);
        c.finger_move(1, {home.x + delta.x, home.y + delta.y}, 2010);
        const auto before = c.buttons(2010);
        const auto cap = c.attack_thumb(2010);
        check((cap.x - home.x) * delta.x + (cap.y - home.y) * delta.y > 0 &&
                std::hypot(cap.x - home.x, cap.y - home.y) < r && c.buttons(2010) == before,
            "visual cap shows the chosen action inside the well without altering buttons");
        c.finger_move(1, {home.x - delta.x, home.y - delta.y}, 2020);
        check(c.attack_thumb(2020).x == cap.x && c.attack_thumb(2020).y == cap.y,
            "visual direction stays consistent with the latched attack after a finger reversal");
        c.finger_up(1, 2030);
        check(c.attack_thumb(2030).x == cap.x && c.attack_thumb(2030).y == cap.y,
            "release pulse retains matching visual feedback");
        check(centred(2115), "visual cap returns home when its pulse ends");
        c.release_all();
    }
    c.finger_down(1, home, 3000);
    c.finger_move(1, {home.x + r, home.y}, 3110);
    check(centred(3110) && c.buttons(3110) == 0x1000,
        "primary hold does not falsely advertise another selected direction");
    c.finger_up(1, 3120, true);
    check(centred(3120), "cancelled touch immediately releases the visual cap");
    c.finger_down(1, home, 4000);
    c.finger_move(1, {home.x + r, home.y}, 4010);
    c.set_attack_profile(AttackProfile::Off);
    check(centred(4010), "fallback weapon has no directional cap override");
}
void mobile_controls() {
    using E = AdaptiveElement;
    const Area area{0, 0, 1000, 600};
    auto layout = default_adaptive_layout();
    AdaptiveControls c;
    c.set_compact(false);
    c.set_layout(layout, area, 1);
    c.finger_down(1, {250, 550}, 100);
    check(c.held(E::Stick) && c.stick(100).x == 0 && c.stick_origin().x == 250,
        "floating movement starts under the thumb without a jump");
    c.finger_move(1, {300, 550}, 110);
    check(c.stick(110).x > .6f, "floating movement follows the original touch centre");
    auto d = c.placed(E::DPad);
    c.finger_down(2, {d.centre.x, d.centre.y - d.radius * .7f}, 111);
    check(c.dpad_held() == 0x10, "D-pad works alongside analog movement");
    const auto toggle = c.placed(E::DpadToggle).centre;
    c.finger_down(3, toggle, 112);
    check(c.take_dpad_toggle() && !c.take_dpad_toggle() && !c.layout().at(E::DPad).shown,
        "D-pad toggle is consumed once");
    layout.at(E::DPad).shown = false;
    c.set_layout(layout, area, 1);
    check(c.dpad_held() == 0 && c.stick(113).x > .6f,
        "hiding the D-pad releases its direction without releasing movement");
    c.release_all();
    layout.floating_stick = false;
    c.set_layout(layout, area, 1);
    auto home = c.placed(E::Stick);
    c.finger_down(1, {home.centre.x + home.radius * .5f, home.centre.y}, 120);
    check(c.stick(120).x > .3f && c.stick_origin().x == home.centre.x,
        "fixed mode deflects from the edited home position");
    c.release_all();
    layout.floating_stick = true;
    c.set_layout(layout, area, 1);
    c.set_item_area(Area{360, 440, 230, 80});
    check(c.items_follow_hud() && c.placed(E::ItemBar).centre.x == 475, "HUD items use their supplied rectangle");
    c.finger_down(1, {400, 480}, 1000);
    check(c.buttons(1000) == 0, "touching the HUD does not use an item before tap recognition");
    c.finger_up(1, 1010);
    for (unsigned read = 0; read < 3; ++read)
        check(c.sample_item_buttons(true) == 0x8000, "short item tap survives until three actual game reads");
    check(c.sample_item_buttons(true) == 0 && c.buttons(1095) == 0, "sampled tap releases without another event");
    for (int side : {-1, 1}) {
        c.release_all();
        c.finger_down(1, {475, 480}, 2000);
        c.finger_move(1, {475 + side * 45.f, 480}, 2010);
        check(c.buttons(2010) == 0 && c.sample_item_buttons(false) == 0x0100,
            "horizontal item swipe prepares L without D-pad or premature Square");
        c.finger_up(1, 2020);
        for (unsigned read = 0; read < 10; ++read)
            check(c.sample_item_buttons(true) == 0x0100, "released short swipe retains the L preparation");
        for (unsigned read = 0; read < 3; ++read)
            check(c.sample_item_buttons(true) == (side < 0 ? 0x8100 : 0x2100),
                "left and right use the game's L plus Square/Circle item controls");
        check(c.sample_item_buttons(true) == 0, "swipes release L and the face button without an item-use tap");
    }
    c.release_all();
    c.finger_down(1, {475, 480}, 3000);
    c.finger_move(1, {475, 520}, 3010);
    c.finger_up(1, 3020);
    check(c.buttons(3020) == 0 && c.sample_item_buttons(true) == 0 && c.take_camera_drag().y == 40,
        "vertical motion over items becomes camera movement without item use");
    c.release_all();
    c.finger_down(1, {475, 480}, 4000);
    c.finger_down(2, {475, 480}, 4001);
    c.finger_up(2, 4002);
    check(c.buttons(4002) == 0 && c.sample_item_buttons(true) == 0, "second item finger cannot inject a tap");
    c.finger_down(3, {250, 550}, 4003);
    c.finger_move(3, {300, 550}, 4004);
    c.set_item_area(Area{500, 410, 200, 70});
    c.finger_up(1, 4010);
    check(c.buttons(4010) == 0 && c.sample_item_buttons(true) == 0 && c.stick(4010).x > .6f,
        "moving the HUD cancels only its item gesture");
    auto attack = c.placed(E::Attack);
    c.release_all();
    c.set_item_area(Area{attack.centre.x - 100, attack.centre.y - 100, 200, 200});
    c.finger_down(1, attack.centre, 5000);
    check(c.buttons(5000) == 0x1000, "explicit attack buttons win over the HUD item rectangle");
    c.release_all();
    c.set_item_area(std::nullopt);
    check(!c.items_follow_hud(), "hidden or absent HUD switches to the editable fallback");
    auto items = c.placed(E::ItemBar).centre;
    c.finger_down(1, items, 6000);
    c.finger_up(1, 6001, true);
    check(c.buttons(6001) == 0 && c.sample_item_buttons(true) == 0, "cancelling an item tap never creates a pulse");
    c.finger_down(1, items, 6100);
    c.finger_up(1, 6090);
    check(c.buttons(6100) == 0 && c.sample_item_buttons(true) == 0, "backwards item release cannot synthesize a use");
    c.finger_down(1, items, 6200);
    c.finger_up(1, 6551);
    check(c.buttons(6551) == 0 && c.sample_item_buttons(true) == 0,
        "long stationary item holds are not mistaken for taps");
    c.set_attack_profile(AttackProfile::LongSword);
    check(!c.visible(E::Alternate), "Long Sword selector replaces the fallback Circle button");
    c.set_attack_profile(AttackProfile::Off);
    check(c.visible(E::Alternate), "unsupported weapons retain the Circle button");
    c.finger_down(1, c.placed(E::Pause).centre, 7000);
    check(c.take_menu() && !c.take_menu(), "menu press is one-shot");
    c.release_all();
    c.finger_down(1, c.placed(E::R3).centre, 7010);
    check(c.held(E::R3) && c.buttons(7010) == 0, "virtual R3 never leaks PSP button bits");
    c.release_all();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    c.set_item_area(Area{nan, 0, 10, 10});
    check(!c.items_follow_hud(), "nonfinite linked geometry cannot acquire touches");
    c.finger_down(1, {nan, 0}, 8000);
    check(c.buttons(8000) == 0, "nonfinite finger origin is ignored");
    c.finger_down(1, c.placed(E::Evade).centre, 8010);
    c.finger_move(1, {nan, 0}, 8011);
    check(c.buttons(8011) == 0, "invalid finger motion cancels its button");
    for (int n = 0; n < 10; ++n) c.finger_down(n, {500, 200}, 9000);
    c.finger_down(20, c.placed(E::Attack).centre, 9001);
    check(c.buttons(9001) == 0, "finger capacity is bounded without stealing ownership");
    c.release_all();
    check(c.buttons(9002) == 0 && !c.held(E::Stick), "release clears every role");
    Placement parsed;
    check(!parse_adaptive_placement(E::Attack, "bad", parsed), "malformed placements are rejected");
    check(parse_adaptive_placement(E::Pause, "right 0.1 0.1 0.05 0x8000 0", parsed) && parsed.buttons == 0 &&
            parsed.shown,
        "menu placement cannot hide the only menu button");
}

// Independent quest-input oracle: Square uses the item unless L has already
// opened selection for eight game frames. Edge counting detects lost repeats.
struct ItemOracle {
    unsigned l_reads{}, left{}, right{}, used{};
    std::uint16_t previous{};
    void read(std::uint16_t bits) {
        check((bits & 0xF0u) == 0, "item gestures never move the D-pad camera");
        check((bits & 0xA000u) != 0xA000u, "opposite swipes never press Square and Circle together");
        const auto pressed = bits & ~previous;
        if (pressed & 0x8000u) {
            if ((bits & 0x100u) && l_reads >= 8)
                ++left;
            else
                ++used;
        }
        if (pressed & 0x2000u) {
            check((bits & 0x100u) && l_reads >= 8, "Circle is sent only after the item bar opens");
            ++right;
        }
        l_reads = bits & 0x100u ? l_reads + 1 : 0;
        previous = bits;
    }
};
void item_sequences() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_compact(false);
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_item_area(Area{360, 440, 230, 80});
    const Point at{475, 480};
    std::uint64_t clock = 1000;
    const auto swipe = [&](int direction, bool release = true) {
        c.finger_down(1, at, clock++);
        c.finger_move(1, {at.x + direction * 45.f, at.y}, clock++);
        if (release) c.finger_up(1, clock++);
    };
    ItemOracle game;
    const auto read = [&] {
        const auto bits = c.sample_item_buttons(true);
        game.read(bits);
        return bits;
    };
    const auto reset = [&] {
        c.release_all();
        game = {};
    };
    swipe(-1);
    swipe(1);
    swipe(-1);
    swipe(-1);
    for (unsigned poll = 0; poll < 100; ++poll)
        check(c.sample_item_buttons(false) == 0x100, "UI polling cannot consume queued swipes or prepare L");
    for (unsigned n = 0; n < 10; ++n) check(read() == 0x100, "cold selection has ten real L-only reads");
    for (int direction : {-1, 1, -1, -1}) {
        const auto expected = direction < 0 ? 0x8100 : 0x2100;
        for (unsigned n = 0; n < 3; ++n) check(read() == expected, "quick swipes preserve direction and FIFO order");
        check((read() & 0xA000) == 0, "consecutive identical swipes have a real button-release edge");
    }
    check(game.left == 3 && game.right == 1 && game.used == 0 && read() == 0,
        "rapid reversals and repeats select exactly four items without using any");
    reset();
    swipe(-1, false);
    for (unsigned move = 0; move < 100; ++move)
        c.finger_move(1, {at.x + (move % 2 ? -1.f : 1.f) * (50 + move), at.y}, clock++);
    c.finger_up(1, clock++);
    for (unsigned frame = 0; frame < 100; ++frame) read();
    check(game.left == 1 && game.right == 0 && game.used == 0,
        "one continuous item contact selects exactly once despite long motion and reversals");
    // Slow render/event timing cannot expire a request before the game sees it.
    reset();
    swipe(-1, false);
    for (unsigned n = 0; n < 13; ++n) read();
    check(read() == 0x100, "holding the swiping finger keeps the bar open after selection");
    c.finger_move(1, {at.x + 45, at.y}, clock++);
    check(read() == 0x100, "continuing or reversing an accepted swipe cannot select again");
    c.finger_up(1, clock++);
    swipe(1, false);
    check(read() == 0x2100, "a fresh swipe on the still-open bar has no second L preparation delay");
    for (unsigned n = 0; n < 2; ++n) read();
    c.finger_up(1, clock++);
    c.finger_down(1, at, clock++);
    c.finger_up(1, clock++);
    check(read() == 0, "item use after selection first releases L");
    clock += 10000;
    for (unsigned n = 0; n < 3; ++n) check(read() == 0x8000, "tap is not lost to wall-clock expiry");
    check(read() == 0 && game.left == 1 && game.right == 1 && game.used == 1,
        "tap after a swipe uses once instead of selecting another item");
    // Cancellation and geometry changes must remove completed-but-unread input.
    for (unsigned mode = 0; mode < 5; ++mode) {
        reset();
        swipe(-1, false);
        for (unsigned n = 0; n < 11; ++n) read();
        if (mode == 0)
            c.finger_up(1, clock++, true);
        else if (mode == 1)
            c.set_item_area(Area{350, 440, 240, 80});
        else if (mode == 2)
            c.set_layout(default_adaptive_layout(), {0, 0, 1200, 600}, 1);
        else if (mode == 3)
            c.set_attack_profile(AttackProfile::LongSword);
        else
            c.release_all();
        for (unsigned n = 0; n < 20; ++n)
            check(c.sample_item_buttons(true) == 0,
                "cancel, HUD move, resize, weapon change and pause clear pending items");
        c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
        c.set_item_area(Area{360, 440, 230, 80});
        c.set_attack_profile(AttackProfile::Off);
    }
    reset();
    for (unsigned n = 0; n < 32; ++n) swipe(n % 2 ? 1 : -1);
    for (unsigned n = 0; n < 100; ++n) read();
    check(game.left == 8 && game.right == 8 && game.used == 0 && read() == 0,
        "bounded queue overflow keeps the first sixteen events without merging or getting stuck");
    for (unsigned n = 0; n < 3; ++n) swipe(-1);
    for (unsigned n = 0; n < 40; ++n) read();
    check(game.left == 11 && game.right == 8 && game.used == 0,
        "wrapped FIFO accepts fresh swipes after draining a full queue");
    reset();
    c.finger_down(2, {250, 550}, clock++);
    c.finger_move(2, {300, 550}, clock++);
    c.finger_down(3, c.placed(E::Special).centre, clock++);
    swipe(1);
    for (unsigned n = 0; n < 20; ++n) {
        read();
        check(c.stick(clock).x > .6f && c.buttons(clock) == 0x200,
            "item preparation does not delay or release held movement and R");
    }
    reset();
    c.finger_down(1, at, clock++);
    c.finger_up(1, clock++);
    c.finger_down(1, at, clock++);
    c.finger_up(1, clock++);
    for (unsigned n = 0; n < 10; ++n) read();
    check(game.used == 2 && game.left == 0 && read() == 0,
        "quick repeated use taps retain separate press edges without L");
}

void context_modes() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_compact(false);
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::LongSword);
    c.finger_down(1, c.placed(E::Special).centre, 1);
    check(c.buttons(2) == 0x200, "held quest shoulder before transition");
    c.set_mode(AdaptiveMode::Hub);
    c.set_layout(default_adaptive_layout(AdaptiveMode::Hub), {0, 0, 1000, 600}, 1);
    check(c.buttons(3) == 0 && !c.visible(E::Attack) && !c.visible(E::ItemBar),
        "hub releases quest input and hides combat");
    check(c.visible(E::Alternate) && c.attack_profile() == AttackProfile::Off,
        "hub interaction is not hidden by weapon profile");
    check(!c.visible(E::Evade), "village layout omits the unused X action");
    auto previous_hub = default_adaptive_layout(AdaptiveMode::Hub);
    previous_hub.at(E::Evade).shown = true;
    c.set_layout(previous_hub, {0, 0, 1000, 600}, 1);
    check(!c.visible(E::Evade), "old saved village arrangements also hide the ordinary X button");
    c.finger_down(101, c.placed(E::Evade).centre, 4);
    check((c.buttons(4) & 0x4000) == 0, "tapping the removed village X position cannot send X");
    c.finger_up(101, 5, true);
    c.finger_down(102, c.placed(E::Interact).centre, 4);
    check(c.buttons(4) == 0x8000, "village hand icon retains the Square binding");
    c.finger_up(102, 5, true);
    previous_hub.at(E::Evade).buttons = 0x0001;
    c.set_layout(previous_hub, {0, 0, 1000, 600}, 1);
    check(c.visible(E::Evade), "an intentionally rebound village action is preserved");
    c.set_layout(default_adaptive_layout(AdaptiveMode::Hub), {0, 0, 1000, 600}, 1);

    c.finger_move(1, c.placed(E::Alternate).centre, 4);
    check(c.buttons(5) == 0, "old contact cannot press a new context button");
    c.finger_down(2, c.placed(E::Alternate).centre, 6);
    check(c.buttons(7) == 0x2000, "hub NPC interaction is Circle");
    c.set_mode(AdaptiveMode::Interface);
    auto layout = default_adaptive_layout(AdaptiveMode::Interface);
    layout.at(E::DPad).shown = false;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(c.visible(E::DPad) && !c.visible(E::Stick) && !c.visible(E::DpadToggle),
        "interface always has navigation and no movement stick");
    for (auto e : {E::Attack, E::Alternate, E::Evade, E::Interact, E::LeftShoulder, E::Special, E::Start, E::Select}) {
        c.finger_down(3, c.placed(e).centre, 10);
        check(c.buttons(11) == layout.at(e).buttons, "interface buttons emit their ordinary PSP input immediately");
        c.finger_up(3, 12);
    }
    c.finger_down(4, {500, 300}, 20);
    c.finger_move(4, {700, 350}, 21);
    const auto drag = c.take_camera_drag();
    check(drag.x == 0 && drag.y == 0 && c.sample_item_buttons(true) == 0,
        "interface background swipes never attack, select items or turn camera");
    c.finger_down(5, c.placed(E::InterfaceToggle).centre, 22);
    check(c.take_interface_toggle() && !c.take_interface_toggle(), "manual interface toggle is a one-shot event");
    c.release_all();
    c.set_mode(AdaptiveMode::Quest);
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    check(c.visible(E::Evade), "quest evasion remains available");
    const auto item = c.placed(E::ItemBar).centre;
    c.finger_down(6, item, 30);
    c.finger_move(6, {item.x + 80, item.y}, 31);
    c.set_mode(AdaptiveMode::Interface);
    for (int i = 0; i < 20; ++i) check(c.sample_item_buttons(true) == 0, "context change clears queued item commands");
}

void weapon_posture() {
    using E = AdaptiveElement;
    using P = WeaponPosture;
    AdaptiveControls c;
    c.set_compact(false);
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::LongSword);
    c.set_weapon_posture(P::Sheathed);
    check(c.attack_profile() == AttackProfile::Off && c.visible(E::Alternate),
        "sheathed controls expose ordinary interaction and immediate draw");
    c.finger_down(1, {150, 500}, 10);
    c.finger_move(1, {190, 500}, 11);
    c.finger_down(2, c.placed(E::Attack).centre, 12);
    c.finger_down(3, c.placed(E::Special).centre, 12);
    check(c.buttons(12) == 0x1200, "draw and sprint have no attack gesture recognition delay");
    c.set_weapon_posture(P::Drawn);
    check(c.buttons(13) == 0 && c.stick(13).x > 0, "drawing cancels old action holds while preserving movement");
    c.finger_move(2, c.placed(E::Attack).centre, 14);
    check(c.buttons(14) == 0, "stale contact cannot become an attack after posture changes");
    check(c.attack_profile() == AttackProfile::LongSword && !c.visible(E::Alternate),
        "drawn sword restores attack selector");
    c.finger_up(2, 15);
    c.finger_down(2, c.placed(E::Attack).centre, 20);
    check(c.buttons(20) == 0 && c.buttons(140) == 0x1000, "fresh drawn contact follows attack gesture contract");
    c.set_weapon_posture(P::Drawn);
    check(c.buttons(141) == 0x1000, "unchanged posture does not interrupt attacks");
    c.set_weapon_posture(P::Sheathed);
    check(c.buttons(142) == 0 && c.stick(142).x > 0, "sheathe clears attack without stopping movement");
    c.set_weapon_posture(P::Unknown);
    check(c.attack_profile() == AttackProfile::LongSword, "unknown posture retains legacy gesture behavior");
    c.set_mode(AdaptiveMode::Interface);
    c.set_layout(default_adaptive_layout(AdaptiveMode::Interface), {0, 0, 1000, 600}, 1);
    c.finger_down(4, c.placed(E::Attack).centre, 150);
    c.set_weapon_posture(P::Sheathed);
    check(c.buttons(151) == 0x1000, "posture changes do not interrupt interface buttons");
    c.release_all();
}

void weapon_profiles() {
    using P = AttackProfile;
    const P profiles[] = {P::GreatSword, P::SwordAndShield, P::DualBlades, P::Hammer};
    const Point directions[] = {{0, -50}, {0, 50}, {-50, 0}, {50, 0}};
    const std::uint16_t expected[][4] = {{0x1000, 0x3000, 0x0001, 0x2000}, {0x3000, 0x2000, 0x2000, 0x0001},
        {0x3000, 0, 0x2000, 0x2000}, {0, 0, 0, 0x2000}};
    for (unsigned p = 0; p < 4; ++p) {
        for (unsigned d = 0; d < 4; ++d) {
            AdaptiveAttack a;
            a.begin(1, {100, 100}, 50, 1000, profiles[p]);
            check(a.output(1001).buttons == 0, "new weapon selector does not pre-fire Triangle");
            a.move(1, {100 + directions[d].x, 100 + directions[d].y}, 1020);
            const auto output = a.output(1020);
            check(output.buttons == expected[p][d] && (output.buttons & 0xf0) == 0,
                "weapon-specific flick emits only its PSP combination");
            check(output.lateral == (profiles[p] == P::DualBlades && d == 2 ? -1.f : 0.f),
                "only a deliberate left dual-blade flick overrides lateral movement");
            a.end(1, 1030, true);
            check(a.output(1030).buttons == 0, "every weapon selector cancels without a phantom action");
        }
    }
    AdaptiveAttack a;
    a.begin(1, {0, 0}, 50, 1000, P::GreatSword);
    check(a.output(1110).buttons == 0x1000, "Great Sword hold starts Triangle charge");
    a.end(1, 1400, false);
    check(a.output(1400).buttons == 0, "release ends Great Sword charge immediately without an 85ms extension");
    a.begin(1, {0, 0}, 50, 2000, P::GreatSword);
    a.end(1, 2010, false);
    check(a.output(2010).buttons == 0x1000, "Great Sword quick tap retains a sampleable attack pulse");
    a.begin(1, {0, 0}, 50, 3000, P::Guard);
    check(a.output(3000).buttons == 0x0200, "shield guards immediately while recognizing a gesture");
    a.move(1, {-50, 0}, 3500);
    check(a.output(3500).buttons == 0x2200, "guarded attack can be chosen after holding the shield");
    a.end(1, 3600, false);
    check(a.output(3600).buttons == 0, "guarded hold releases without a delayed extra attack");
    a.begin(1, {0, 0}, 50, 4000, P::Guard);
    a.move(1, {0, -50}, 4020);
    check(a.output(4020).buttons == 0x0200 && a.direction(4020) == AttackDirection::Pending,
        "shield flick up keeps guarding without selecting evade");
    a.move(1, {0, -100}, 4400);
    check(a.output(4400).buttons == 0x0200, "late upward shield motion also keeps guarding");
    a.move(1, {50, 0}, 4410);
    check(a.output(4410).buttons == 0x2200, "ignored upward motion leaves a later shield attack available");
    a.end(1, 4420, false);
    check(a.output(4420).buttons == 0, "shield attack releases without an evade or delayed pulse");
    a.clear();
    a.begin(1, {0, 0}, 50, 5000, P::Guard);
    a.move(1, {0, 50}, 5020);
    check(a.output(5020).buttons == 0x1200, "guard flick down selects R plus Triangle");
    a.clear();
    a.begin(1, {0, 0}, 50, 6000, P::Off);
    a.end(1, 6010, false);
    check(a.output(6010).buttons == 0, "off profile cannot inject an attack");
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(P::SwordAndShield);
    c.set_weapon_posture(WeaponPosture::Drawn);
    const auto guard = c.placed(AdaptiveElement::Special).centre;
    c.finger_down(1, guard, 7000);
    check(c.buttons(7000) == 0x0200 && c.held(AdaptiveElement::Special),
        "secondary shield selector owns its own immediate guard");
    c.finger_move(1, {guard.x - 100, guard.y}, 7020);
    check(c.buttons(7020) == 0x2200, "secondary selector routes a guarded action");
    c.set_attack_profile(P::Hammer);
    check(c.buttons(7021) == 0 && !c.held(AdaptiveElement::Special), "weapon changes cancel guard and attack owners");
}
void hammer_and_dual_blades() {
    using P = AttackProfile;
    using E = AdaptiveElement;
    for (auto profile : {P::Hammer, P::DualBlades}) {
        AdaptiveAttack a;
        a.begin(1, {0, 0}, 50, 100, profile);
        a.move(1, {0, 50}, 110);
        check(a.direction(110) == AttackDirection::Blocked && a.output(110).buttons == 0,
            "unused downward weapon flick cannot become a duplicate attack");
        a.end(1, 120, false);
        check(a.output(120).buttons == 0, "lifting a blocked weapon flick cannot become a primary tap");
    }
    for (auto profile : {P::HammerCharge, P::DemonToggle, P::GreatSwordGuard}) {
        AdaptiveAttack a;
        a.begin(1, {100, 100}, 50, 1000, profile);
        check(a.output(1000).buttons == 0x200, "charge and mode buttons send R immediately");
        check(!a.begin(2, {100, 100}, 50, 1001, profile), "secondary button has one finger owner");
        for (const Point target : {Point{-100, 100}, {300, 100}, {100, -100}, {100, 300}}) {
            a.move(1, target, 2000);
            check(a.output(2000).buttons == 0x200, "drifting outside a held secondary never emits a different attack");
        }
        a.end(1, 2010, false);
        check(a.output(2010).buttons == (profile == P::DemonToggle ? 0x200 : 0),
            "charge releases immediately; mode toggle keeps a short sampleable pulse");
        check(a.output(2095).buttons == 0, "secondary release never leaves R stuck");
        a.begin(1, {0, 0}, 50, 3000, profile);
        a.end(1, 3001, false);
        check(a.output(3001).buttons == (profile == P::DemonToggle ? 0x200 : 0),
            "even short charge releases immediately while a quick mode tap remains observable");
        a.begin(1, {0, 0}, 50, 4000, profile);
        a.end(1, 4010, true);
        check(a.output(4010).buttons == 0, "canceled charge/mode contacts never queue a new input");
        a.begin(1, {0, 0}, 50, 5000, profile);
        a.move(1, {std::numeric_limits<float>::quiet_NaN(), 0}, 5010);
        check(a.output(5010).buttons == 0, "invalid secondary motion releases ownership safely");
    }
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(P::Hammer);
    c.set_weapon_posture(WeaponPosture::Drawn);
    const auto charge = c.placed(E::Special).centre;
    const auto attack = c.placed(E::Attack).centre;
    c.finger_down(1, charge, 100);
    c.finger_down(2, {200, 450}, 100);
    c.finger_move(2, {240, 450}, 101);
    c.finger_move(1, {charge.x - 100, charge.y - 100}, 5000);
    check(c.buttons(5000) == 0x200 && c.stick(5000).x > .5f,
        "a long Hammer charge preserves movement and stays held outside its target");
    c.finger_up(1, 5010);
    check(c.buttons(5010) == 0 && c.stick(5010).x > .5f,
        "release forwards the current movement for the game's stationary/moving charge result");
    c.finger_down(1, charge, 6000);
    c.finger_down(3, c.placed(E::Evade).centre, 6010);
    check(c.buttons(6010) == 0x4200, "separate evade supplies X while Hammer charge remains held");
    c.finger_up(3, 6020);
    c.finger_up(1, 6030);
    c.finger_down(4, attack, 6100);
    c.finger_up(4, 6110);
    check(c.buttons(6110) == 0x1000, "Hammer tap remains the native charge/spin follow-up input");
    c.set_attack_profile(P::DualBlades);
    c.finger_down(1, charge, 7000);
    c.finger_up(1, 7001);
    check(c.buttons(7085) == 0x200 && c.buttons(7086) == 0,
        "quick Demon-mode toggle survives native-frame sampling and expires at the pulse boundary");
    c.finger_down(2, attack, 7100);
    c.finger_move(2, {attack.x, attack.y - 70}, 7110);
    check(c.buttons(7110) == 0x3000, "DB upward request lets native Normal/Demon/Archdemon rules choose the move");
    c.finger_up(2, 7120, true);
    c.finger_down(1, charge, 7200);
    c.set_weapon_posture(WeaponPosture::Sheathed);
    check(c.buttons(7201) == 0, "sheathing cancels a captured mode control before it becomes sprint");
    c.finger_down(1, charge, 7300);
    c.finger_up(1, 7301);
    check(c.buttons(7301) == 0, "sheathed sprint has no Demon-toggle release pulse");
    c.set_weapon_posture(WeaponPosture::Drawn);
    c.finger_down(1, charge, 7400);
    c.release_all();
    check(c.buttons(7401) == 0, "focus/context cancellation clears the held mode request");
}
void great_sword() {
    using P = AttackProfile;
    using E = AdaptiveElement;
    AdaptiveAttack a;
    a.begin(1, {0, 0}, 50, 1000, P::GreatSword);
    a.move(1, {0, -50}, 1010);
    check(a.output(1010).buttons == 0x1000, "GS upward flick starts charge without waiting for primary hold");
    a.end(1, 1020, false);
    check(a.output(1020).buttons == 0, "even a short upward GS charge releases immediately");
    a.begin(1, {0, 0}, 50, 2000, P::GreatSword);
    a.move(1, {-50, 0}, 2010);
    a.end(1, 2200, false);
    check(a.output(2200).buttons == 1 && a.output(2285).buttons == 0,
        "kick uses native Select and is not confused with a charge release");
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(P::GreatSword);
    c.set_weapon_posture(WeaponPosture::Drawn);
    auto guard = c.placed(E::Special).centre;
    auto attack = c.placed(E::Attack).centre;
    c.finger_down(1, guard, 1000);
    c.finger_move(1, {guard.x - 200, guard.y - 200}, 1020);
    check(c.buttons(1020) == 0x200, "GS guard captures its finger rather than dropping on drift");
    c.finger_down(2, attack, 1030);
    c.finger_up(2, 1040);
    check(c.buttons(1040) == 0x1200, "separate GS guard and main tap can request the native guarded kick");
    c.finger_up(1, 1041);
    check(
        c.buttons(1041) == 0x1000 && c.buttons(1125) == 0, "guard release does not prolong R or cancel another finger");
    c.finger_down(1, {200, 450}, 2000);
    c.finger_move(1, {240, 450}, 2010);
    c.finger_down(2, attack, 2020);
    c.finger_move(2, {attack.x, attack.y - 70}, 2030);
    check(c.buttons(2030) == 0x1000 && c.stick(2030).x > .5f,
        "GS charge preserves player analog for native slap-to-strong-charge continuations");
    c.finger_up(2, 2040);
    check(c.buttons(2040) == 0 && c.stick(2040).x > .5f, "GS charge lift preserves independent movement");
    c.finger_down(2, guard, 2100);
    c.set_mode(AdaptiveMode::Interface);
    check(c.buttons(2101) == 0, "opening an interface clears GS guard and movement captures");
    c.set_mode(AdaptiveMode::Quest);
    c.finger_down(2, guard, 2200);
    c.set_attack_profile(P::Hammer);
    check(c.buttons(2201) == 0, "weapon change clears captured GS guard");
}
void shield_size() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    auto layout = default_adaptive_layout();
    c.set_layout(layout, {10, 20, 1000, 600}, 1);
    const auto original = c.placed(E::Special);
    c.set_attack_profile(AttackProfile::SwordAndShield);
    const auto enlarged = c.placed(E::Special);
    check(std::abs(enlarged.radius / original.radius - 1.35f) < .001f && enlarged.centre.x == original.centre.x &&
            enlarged.centre.y == original.centre.y,
        "SnS shield gains a larger radius without changing the saved position");
    check(c.layout() == layout, "contextual shield size never overwrites saved layout");
    c.finger_down(1, {enlarged.centre.x, enlarged.centre.y - original.radius * 1.3f}, 100);
    check(c.buttons(100) == 0x0200 && c.held(E::Special), "enlarged shield rim is an actual guard touch target");
    c.finger_move(1, {enlarged.centre.x, enlarged.centre.y - enlarged.radius * 3}, 110);
    check(c.buttons(110) == 0x0200, "shield owner dragged upward never presses evade");
    c.finger_up(1, 300);
    check(c.buttons(300) == 0, "releasing ignored upward motion releases guard");
    c.set_weapon_posture(WeaponPosture::Sheathed);
    check(c.placed(E::Special).radius == original.radius, "sheathed sprint retains its ordinary size");
    c.set_weapon_posture(WeaponPosture::Drawn);
    check(c.placed(E::Special).radius == enlarged.radius, "drawing restores the larger shield immediately");
    c.set_mode(AdaptiveMode::Interface);
    check(c.placed(E::Special).radius == original.radius, "interface R retains its ordinary size");
    c.set_mode(AdaptiveMode::Quest);
    layout.at(E::Special).size *= 1.2f;
    c.set_layout(layout, {10, 20, 1000, 600}, 1);
    check(std::abs(c.placed(E::Special).radius / enlarged.radius - 1.2f) < .001f,
        "custom editor scaling still scales the enlarged shield proportionally");
    const auto preview = place(layout.at(E::Special), E::Special, c.area(), c.scale(), c.attack_profile());
    check(preview.radius == c.placed(E::Special).radius,
        "editor placement uses the same shield radius as touch hit testing");
    layout.at(E::Special).buttons = 0x8000;
    c.set_layout(layout, {10, 20, 1000, 600}, 1);
    check(std::abs(c.placed(E::Special).radius / original.radius - 1.2f) < .001f,
        "rebound secondary action is not inflated as a shield");
    c.set_attack_profile(AttackProfile::Hammer);
    check(std::abs(c.placed(E::Special).radius / original.radius - 1.2f) < .001f,
        "other weapon secondary actions keep their original sizing");
}
void sword_and_shield_routes() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_weapon_posture(WeaponPosture::Drawn);
    c.set_attack_profile(AttackProfile::SwordAndShield);
    const Point move{200, 450};
    const auto attack = c.placed(E::Attack).centre;
    c.finger_down(1, move, 100);
    c.finger_move(1, {200, 390}, 101);
    check(c.stick(101).y < -.5f, "ordinary touch movement reaches the game");
    c.finger_down(2, attack, 110);
    check(c.stick(110).y < -.5f && c.buttons(110) == 0, "pending sword gesture leaves movement untouched");
    c.finger_move(2, {attack.x, attack.y + 60}, 120);
    check(c.buttons(120) == 0x2000 && c.stick(120).x == 0 && c.stick(120).y == 0,
        "horizontal sword route sends Circle with neutral touch movement instead of a shield strike");
    c.finger_move(1, {240, 450}, 121);
    c.finger_up(2, 130);
    check(c.buttons(214) == 0x2000 && c.stick(214).x == 0, "neutral movement covers the entire sampled attack pulse");
    check(c.buttons(215) == 0 && c.stick(215).x > .5f, "pulse expiry restores the current held movement contact");
    c.finger_down(2, attack, 300);
    c.finger_move(2, {attack.x + 60, attack.y}, 310);
    check(c.buttons(310) == 0x0001 && c.stick(310).x > .5f,
        "right sword flick selects native spin without Circle or movement suppression");
    c.finger_up(2, 320, true);
    check(c.buttons(320) == 0, "canceled spin has no delayed Select pulse");
    c.finger_down(2, attack, 400);
    c.finger_move(2, {attack.x - 60, attack.y}, 410);
    check(c.buttons(410) == 0x2000 && c.stick(410).x > .5f,
        "left sword route retains ordinary Circle and user-chosen movement pending shield verification");
    c.finger_up(2, 420, true);
    c.finger_down(2, attack, 500);
    c.finger_move(2, {attack.x, attack.y + 60}, 510);
    c.finger_up(2, 520, true);
    check(c.buttons(520) == 0 && c.stick(520).x > .5f, "cancellation immediately restores movement");
    c.finger_down(2, attack, 600);
    c.finger_move(2, {attack.x, attack.y + 60}, 610);
    c.set_weapon_posture(WeaponPosture::Sheathed);
    check(
        c.buttons(611) == 0 && c.stick(611).x > .5f, "posture changes clear neutral movement without losing the stick");
    c.set_weapon_posture(WeaponPosture::Drawn);
    c.finger_down(2, attack, 700);
    c.finger_move(2, {attack.x, attack.y + 60}, 710);
    c.set_attack_profile(AttackProfile::LongSword);
    check(c.buttons(711) == 0 && c.stick(711).x == 0, "weapon switch cancels both action and movement owners");
}
void sword_and_shield_items() {
    using E = AdaptiveElement;
    using P = WeaponPosture;
    AdaptiveControls c;
    auto layout = default_adaptive_layout();
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::SwordAndShield);
    const auto item = c.placed(E::ItemBar).centre;
    const auto sheathe = c.placed(E::Interact).centre;
    for (auto posture : {P::Drawn, P::Sheathed, P::Unknown}) {
        c.release_all();
        c.set_weapon_posture(posture);
        check(c.visible(E::Interact) == (posture == P::Drawn),
            "SnS only exposes a separate sheath button when drawn item use differs from Square");
        c.finger_down(1, item, 100);
        check(c.sample_item_buttons(true) == 0, "an unresolved item tap does not prematurely guard or use an item");
        c.finger_up(1, 110);
        const auto expected = posture == P::Drawn ? 0x8200u : 0x8000u;
        for (unsigned i = 0; i < 3; ++i)
            check(c.sample_item_buttons(true) == expected, "SnS item tap uses native R plus Square only when drawn");
        for (unsigned i = 0; i < 12; ++i)
            check(c.sample_item_buttons(true) == 0, "completed item tap leaves neither guard nor Square held");
    }
    c.set_weapon_posture(P::Drawn);
    c.finger_down(2, sheathe, 300);
    check(c.buttons(300) == 0x8000 && c.sample_item_buttons(true) == 0,
        "separate sheath button sends plain Square without injecting guard or using the item queue");
    c.finger_up(2, 310);
    check(c.buttons(310) == 0, "sheath button releases immediately");
    for (unsigned test = 0; test < 4; ++test) {
        c.release_all();
        c.finger_down(1, item, 400);
        if (test == 2) c.finger_move(1, {item.x + 80, item.y}, 410);
        if (test == 3) c.finger_move(1, {item.x, item.y - 80}, 410);
        c.finger_up(1, test == 1 ? 751 : 420, test == 0);
        unsigned presses = 0;
        bool selected = false;
        for (unsigned i = 0; i < 24; ++i) {
            const auto bits = c.sample_item_buttons(true);
            check((bits & 0x0200) == 0, "canceled, long or swiped item contact never injects R");
            check((bits & 0x8000) == 0, "right swipe and non-taps never use or sheathe the item");
            if (bits & 0x2000) {
                if (!selected) ++presses;
                selected = true;
            } else
                selected = false;
        }
        check(presses == (test == 2 ? 1u : 0u), "right swipe still selects exactly one item with native L plus Circle");
    }
    c.release_all();
    c.finger_down(1, item, 800);
    c.finger_up(1, 1150);
    check(c.sample_item_buttons(true) == 0x8200, "maximum tap duration includes the SnS item route");
    c.set_attack_profile(AttackProfile::Hammer);
    check(c.sample_item_buttons(true) == 0 && c.visible(E::Interact),
        "weapon change cancels queued guard/item input but keeps sheathing");
    c.finger_down(1, item, 1200);
    c.finger_up(1, 1210);
    check(c.sample_item_buttons(true) == 0x8000, "other drawn weapons keep ordinary sheathing on the item surface");
    c.release_all();
    c.set_attack_profile(AttackProfile::SwordAndShield);
    layout.at(E::Interact).shown = false;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(!c.visible(E::Interact), "the separate sheath button respects custom hidden placement");
    layout.at(E::Interact).shown = true;
    layout.at(E::Interact).buttons = 0x0001;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    c.finger_down(2, sheathe, 1300);
    check(c.buttons(1300) == 0x0001, "custom bindings remain intact on the separate action button");
}
void remaining_weapon_profiles() {
    using P = AttackProfile;
    struct Row {
        P p;
        std::uint16_t primary, up, down, left, right;
    };
    const Row rows[] = {{P::Bow, 0x1000, 0, 0x2000, 0, 0}};
    const Point directions[] = {{0, -50}, {0, 50}, {-50, 0}, {50, 0}};
    for (const auto &row : rows) {
        check(weapon_profile(row.p), "new weapon profile is a real weapon, not a secondary recognizer");
        AdaptiveAttack a;
        a.begin(1, {0, 0}, 50, 1000, row.p);
        check(a.output(1000).buttons == 0 && a.output(1109).buttons == 0,
            "new main selectors wait for gesture recognition rather than pre-firing");
        check(a.output(1110).buttons == row.primary, "main held action uses the weapon's native primary button");
        a.end(1, 1200, false);
        const bool ranged = row.p == P::Bow;
        check(a.output(1200).buttons == (ranged ? 0 : row.primary),
            "held ranged fire/charge releases without an extra pulse");
        a.begin(1, {0, 0}, 50, 2000, row.p);
        a.end(1, 2010, false);
        check(a.output(2010).buttons == row.primary && a.output(2095).buttons == 0,
            "quick ranged/melee taps remain sampleable");
        const std::uint16_t expected[] = {row.up, row.down, row.left, row.right};
        for (unsigned i = 0; i < 4; ++i) {
            a.begin(1, {0, 0}, 50, 3000, row.p);
            a.move(1, directions[i], 3010);
            check(a.output(3010).buttons == expected[i] && a.output(3010).lateral == 0,
                "new flick maps to documented native buttons without invented analog direction");
            a.end(1, 3020, false);
            check(a.output(3020).buttons == expected[i], "unsupported flick stays canceled on lift");
            a.begin(1, {0, 0}, 50, 4000, row.p);
            a.move(1, directions[i], 4010);
            a.end(1, 4020, true);
            check(a.output(4020).buttons == 0, "cancellation clears each new gesture");
        }
        AdaptiveControls c;
        c.set_bow_toggle(false); // Retained separate-aim comparison mapping.
        c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
        c.set_attack_profile(row.p);
        c.set_weapon_posture(WeaponPosture::Drawn);
        check(c.visible(AdaptiveElement::Interact), "new weapons retain a dedicated sheath");
        check(c.visible(AdaptiveElement::Alternate) == ranged, "only ranged profiles expose ammunition at Alternate");
        c.finger_down(1, c.placed(AdaptiveElement::Special).centre, 100);
        check(c.buttons(100) == 0x200, "new secondary controls send native R immediately");
        c.set_attack_profile(P::Off);
        check(c.buttons(101) == 0, "weapon change clears each new secondary contact");
    }
    check(!weapon_profile(P::Off) && !weapon_profile(P::Aim) && !ranged_profile(P::Hammer) &&
            secondary_profile(P::Off) == P::Off && gesture_directions(P::Aim) == 0,
        "fallback and secondary profiles are not real weapon layouts");
    for (auto profile : {P::Aim}) {
        AdaptiveAttack a;
        a.begin(1, {0, 0}, 50, 1000, profile);
        a.move(1, {100, 100}, 1020);
        check(a.output(1020).buttons == 0x200, "action/aim keeps R on drift");
        a.end(1, 1030, false);
        check(a.output(1030).buttons == (profile == P::Aim ? 0 : 0x200),
            "Bow aim releases immediately; action taps remain sampleable");
        a.begin(1, {0, 0}, 50, 2000, profile);
        a.move(1, {0, 0}, 1999);
        check(!a.held() && a.output(2000).buttons == 0, "invalid action/aim timestamps clear ownership");
    }
    AdaptiveAttack bow;
    bow.begin(1, {0, 0}, 50, 1000, P::Bow);
    check(bow.output(1110).buttons == 0x1000, "Bow starts native charge");
    bow.move(1, {60, 0}, 2000);
    check(bow.output(2000).buttons == 0x3000, "late Bow right flick adds Circle without first releasing Triangle");
    bow.move(1, {-60, 0}, 2001);
    check(bow.output(2001).buttons == 0x3000, "arc request cannot turn into a second action");
    bow.end(1, 2010, false);
    check(bow.output(2010).buttons == 0, "arc contact releases both buttons without a second release pulse");
}
void ranged_controls() {
    using P = AttackProfile;
    using E = AdaptiveElement;
    for (auto profile : {P::Bow}) {
        AdaptiveControls c;
        c.set_bow_toggle(false); // Retained separate-aim comparison mapping.

        auto layout = default_adaptive_layout();
        c.set_layout(layout, {0, 0, 1000, 600}, 1);
        c.set_attack_profile(profile);
        c.set_weapon_posture(WeaponPosture::Drawn);
        const auto ammo = c.placed(E::Alternate).centre, attack = c.placed(E::Attack).centre,
                   aim = c.placed(E::Special).centre;
        for (int side : {-1, 1}) {
            c.release_all();
            c.finger_down(1, ammo, 1000);
            c.finger_move(1, {ammo.x + side * 60.f, ammo.y}, 1010);
            c.finger_move(1, {ammo.x - side * 120.f, ammo.y}, 1011);
            c.finger_up(1, 1020);
            for (unsigned n = 0; n < 30; ++n)
                check(c.sample_item_buttons(false) == 0x100, "UI polls cannot skip ammunition L preparation");
            c.finger_down(2, attack, 1030);
            c.finger_down(3, c.placed(E::Evade).centre, 1030);
            check(c.buttons(1200) == 0, "ammunition selection excludes fire/evade chords");
            for (unsigned n = 0; n < 10; ++n)
                check(c.sample_item_buttons(true) == 0x100, "ammo waits actual game reads before selecting");
            for (unsigned n = 0; n < 3; ++n)
                check(c.sample_item_buttons(true) == (side < 0 ? 0x1100 : 0x4100),
                    "one ammo swipe sends previous/next through native L plus Triangle/Cross");
            for (unsigned n = 0; n < 10; ++n)
                check(c.sample_item_buttons(true) == 0, "reversal/long travel never selects several ammo entries");
            check(c.buttons(1300) == 0, "blocked fire/evade contacts do not resume after ammo queue drains");
        }
        c.release_all();
        c.finger_down(1, ammo, 2000);
        c.finger_up(1, 2010);
        for (unsigned n = 0; n < 3; ++n)
            check(c.sample_item_buttons(true) == (profile == P::Bow ? 0x3000 : 0),
                "Bow ammo tap applies coating; bowgun ammo tap never fires");
        c.release_all();
        c.finger_down(1, ammo, 3000);
        c.finger_move(1, {ammo.x, ammo.y - 60}, 3010);
        c.finger_up(1, 3020);
        check(c.sample_item_buttons(true) == 0 && c.take_camera_drag().y == 0,
            "vertical ammo motion cannot apply coating or move aim");
        c.finger_down(1, ammo, 4000);
        c.finger_move(1, {ammo.x + 60, ammo.y}, 4010);
        c.finger_up(1, 4020, true);
        check(c.sample_item_buttons(true) == 0, "cancel clears queued ammunition without a face-button leak");
        c.finger_down(1, attack, 5000);
        c.finger_down(2, ammo, 5120);
        c.finger_move(2, {ammo.x + 60, ammo.y}, 5130);
        c.finger_up(2, 5140);
        check(c.sample_item_buttons(true) == 0 && c.buttons(5140) == (profile == P::Bow ? 0x1000 : 0x2000),
            "ammunition cannot interrupt a held charge/fire");
        c.release_all();
        c.finger_down(1, aim, 6000);
        c.finger_move(1, {aim.x + 12, aim.y + 9}, 6010);
        auto drag = c.take_camera_drag();
        check(c.buttons(6010) == 0x200 && drag.x == 12 && drag.y == 9,
            "aim button retains native R while its finger supplies camera movement");
        c.finger_down(2, {500, 200}, 6020);
        c.finger_move(2, {520, 220}, 6030);
        drag = c.take_camera_drag();
        check(drag.x == 0 && drag.y == 0, "a second free-space finger cannot double the aiming drag");
        c.release_all();
        c.finger_down(1, ammo, 7000);
        c.finger_move(1, {ammo.x + 60, ammo.y}, 7010);
        c.set_item_area(Area{300, 400, 140, 60});
        check(
            c.sample_item_buttons(false) == 0x100, "moving item HUD does not cancel independent ammunition selection");
        c.set_weapon_posture(WeaponPosture::Sheathed);
        check(c.sample_item_buttons(true) == 0 && c.buttons(7020) == 0,
            "posture change clears ammunition queue and owner");
        c.set_weapon_posture(WeaponPosture::Drawn);
        layout.at(E::Alternate).buttons = 0x0001;
        c.set_layout(layout, {0, 0, 1000, 600}, 1);
        c.finger_down(1, ammo, 8000);
        check(c.buttons(8000) == 1 && c.sample_item_buttons(true) == 0,
            "rebound ammunition position remains an ordinary button");
    }
    AdaptiveControls c;
    c.set_bow_toggle(false); // Retained separate-aim comparison mapping.
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(P::Bow);
    c.set_weapon_posture(WeaponPosture::Drawn);
    c.finger_down(1, c.placed(E::Attack).centre, 1000);
    c.finger_down(2, c.placed(E::Evade).centre, 1200);
    check(c.buttons(1200) == 0x5000, "Bow evade sends Cross without prematurely dropping a held Triangle charge");
    c.release_all();
}
void dedicated_sheathe() {
    using E = AdaptiveElement;
    using P = AttackProfile;
    for (auto profile : {P::LongSword, P::GreatSword, P::SwordAndShield, P::DualBlades, P::Hammer, P::Bow}) {
        AdaptiveControls c;
        auto layout = default_adaptive_layout();
        c.set_layout(layout, {0, 0, 1000, 600}, 1);
        c.set_attack_profile(profile);
        for (auto posture : {WeaponPosture::Unknown, WeaponPosture::Sheathed, WeaponPosture::Drawn}) {
            c.set_weapon_posture(posture);
            check(c.visible(E::Interact) == (posture == WeaponPosture::Drawn),
                "all six supported drawn weapon layouts keep a dedicated sheath beside the visible item surface");
        }
        const auto at = c.placed(E::Interact).centre;
        c.set_item_area(Area{at.x - 70, at.y - 70, 140, 140});
        c.finger_down(1, at, 100);
        check(c.buttons(100) == 0x8000 && c.sample_item_buttons(true) == 0,
            "dedicated sheath sends ordinary Square even when the moved item HUD overlaps it");
        c.finger_up(1, 110, true);
        check(c.buttons(110) == 0 && c.sample_item_buttons(true) == 0, "canceling sheathe cannot queue item use");
        c.finger_down(1, at, 120);
        c.set_weapon_posture(WeaponPosture::Sheathed);
        check(c.buttons(121) == 0 && !c.visible(E::Interact),
            "completed sheathing cancels the contact before it can use an item");
        c.set_weapon_posture(WeaponPosture::Drawn);
        c.set_item_area(std::nullopt);
        layout.at(E::Interact).shown = false;
        c.set_layout(layout, {0, 0, 1000, 600}, 1);
        check(!c.visible(E::Interact), "explicit editor visibility remains respected");
        layout.at(E::Interact).shown = true;
        layout.at(E::Interact).buttons = 0x0001;
        c.set_layout(layout, {0, 0, 1000, 600}, 1);
        c.finger_down(1, at, 200);
        check(c.buttons(200) == 1, "custom sheath-position bindings are preserved");
        c.release_all();
        layout.at(E::Interact).buttons = 0x8000;
        layout.at(E::ItemBar).shown = false;
        c.set_layout(layout, {0, 0, 1000, 600}, 1);
        c.set_weapon_posture(WeaponPosture::Sheathed);
        check(c.visible(E::Interact), "hidden item surface still exposes the ordinary Square fallback");
        c.set_mode(AdaptiveMode::Hub);
        check(c.visible(E::Interact), "village retains its interaction control");
        c.set_mode(AdaptiveMode::Interface);
        check(c.visible(E::Interact), "interfaces retain ordinary face-button controls");
    }
}
void command_commit_contract() {
    using E = AdaptiveElement;
    for (auto e : {E::Start, E::Select, E::LeftShoulder}) {
        AdaptiveControls c;
        c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
        const auto root = c.placed(E::Pause).centre;
        const auto open = [&] { c.finger_down(1, root, 100); };
        open();
        c.finger_down(2, c.placed(e).centre, 110);
        check(c.held(e) && c.buttons(110) == 0 && c.buttons(2000) == 0,
            "drawer commands highlight on hold without leaking PSP input");
        c.finger_up(2, 2010, true);
        check(c.buttons(2010) == 0, "canceling a held drawer command sends no input");
        open();
        c.finger_down(2, c.placed(e).centre, 110);
        c.finger_move(2, {400, 550}, 120);
        check(!c.held(e) && c.buttons(120) == 0, "dragging off a command keeps it uncommitted");
        c.finger_up(2, 130);
        check(c.buttons(130) == 0, "releasing outside a command cancels it");
        open();
        c.finger_down(2, c.placed(e).centre, 110);
        c.finger_up(2, 120);
        const auto bits = c.layout().at(e).buttons;
        check(c.buttons(120) == bits && c.buttons(204) == bits && c.buttons(205) == 0,
            "a committed drawer command emits one bounded pulse after release");
        c.release_all();
        c.set_mode(AdaptiveMode::Interface);
        c.set_layout(default_adaptive_layout(AdaptiveMode::Interface), {0, 0, 1000, 600}, 1);
        c.finger_down(3, c.placed(E::LeftShoulder).centre, 300);
        check(c.buttons(300) == 0x0100, "ordinary interface paging still responds on press");
    }
}
void compact_commands() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    check(c.compact() && c.visible(E::Pause) && !c.visible(E::Start) && !c.visible(E::InterfaceToggle) &&
            !c.visible(E::Interact),
        "compact arrangement hides utility and Square duplicates");
    check(c.visible(E::Alternate) && c.visible(E::ItemBar), "fallback interaction and item use remain reachable");
    auto layout = default_adaptive_layout();
    layout.at(E::ItemBar).shown = false;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(c.visible(E::Interact), "hiding the item surface restores the ordinary Square fallback");
    c.finger_down(99, c.placed(E::Interact).centre, 1);
    check(c.buttons(1) == 0x8000, "Square remains usable when its compact replacement is hidden");
    c.finger_up(99, 2);
    layout.at(E::ItemBar).shown = true;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(!c.visible(E::Interact), "restoring the item surface removes the duplicate Square again");
    layout.at(E::Interact).buttons = 0x3000;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(c.visible(E::Interact), "a deliberately rebound Square position is never hidden as a duplicate");
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    const auto root = c.placed(E::Pause).centre;
    c.finger_down(1, c.placed(E::Special).centre, 10);
    check(c.buttons(10) == 0x0200, "sprint initially held");
    c.finger_down(2, root, 11);
    check(c.commands_open() && c.buttons(11) == 0, "opening commands releases gameplay contacts");
    check(c.visible(E::YakumoMenu) && c.visible(E::Start) && c.visible(E::LeftShoulder) && !c.visible(E::Attack),
        "drawer exposes utilities and hides underlying controls");
    c.finger_move(1, {500, 500}, 12);
    check(c.take_camera_drag().x == 0 && c.buttons(12) == 0, "stale contacts cannot become camera or attacks");
    c.finger_down(3, {500, 500}, 13);
    check(!c.commands_open() && c.buttons(13) == 0, "outside tap dismisses commands without gameplay input");
    c.finger_move(3, {700, 550}, 14);
    check(c.take_camera_drag().x == 0, "dismissal touch stays canceled until lifted");
    c.finger_down(4, root, 20);
    const auto toggle = c.placed(E::InterfaceToggle).centre;
    c.finger_down(5, toggle, 21);
    check(!c.take_interface_toggle(), "interface utility waits for release");
    c.finger_up(5, 22);
    check(c.take_interface_toggle() && !c.commands_open(), "interface utility commits once and closes");
    c.finger_down(6, root, 30);
    c.finger_down(7, c.placed(E::YakumoMenu).centre, 31);
    c.finger_up(7, 32);
    check(c.take_menu() && !c.take_menu(), "Yakumo command is a one-shot menu event");
    c.finger_down(8, root, 40);
    c.finger_down(9, c.placed(E::Start).centre, 41);
    c.finger_up(9, 42);
    check(c.buttons(42) == 0x0008 && c.buttons(126) == 0x0008 && c.buttons(127) == 0,
        "game-menu tap has a bounded sampled pulse");
    c.finger_down(10, root, 150);
    c.finger_down(11, c.placed(E::InterfaceToggle).centre, 151);
    c.finger_up(11, 152, true);
    check(!c.take_interface_toggle() && !c.commands_open() && c.buttons(152) == 0, "canceled utility cannot commit");
    c.set_compact(false);
    check(!c.compact() && c.visible(E::InterfaceToggle) && c.visible(E::Interact),
        "explicit opt-out restores the editable old controls");
    c.set_compact(true);
    c.set_mode(AdaptiveMode::Interface);
    c.set_layout(default_adaptive_layout(AdaptiveMode::Interface), {0, 0, 400, 900}, 1);
    check(c.visible(E::LeftShoulder) && c.visible(E::Special) && c.visible(E::DPad),
        "compact interfaces retain paging and D-pad");
    c.finger_down(12, c.placed(E::Pause).centre, 200);
    for (auto e : {E::YakumoMenu, E::Start, E::Select, E::LeftShoulder, E::InterfaceToggle}) {
        const auto p = c.placed(e);
        check(p.centre.x - p.radius >= 0 && p.centre.x + p.radius <= 400 && p.centre.y - p.radius >= 0 &&
                p.centre.y + p.radius <= 900,
            "command targets stay within a portrait safe area");
    }
    c.release_all();
    c.set_layout(default_adaptive_layout(AdaptiveMode::Interface), {0, 0, 480, 272}, 1);
    c.finger_down(13, c.placed(E::Pause).centre, 200);
    check(c.placed(E::Start).radius >= 22.f, "expanded command targets reach 44px in a small game window");
    const auto last = c.placed(E::InterfaceToggle);
    check(last.centre.y + last.radius * 2 <= 272, "command labels stay clear of the bottom edge");
    c.set_mode(AdaptiveMode::Hub);
    check(!c.commands_open() && c.buttons(201) == 0, "scene changes dismiss commands and pending input");
}

}

void compact_movement() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    auto layout = default_adaptive_layout();
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::LongSword);
    c.set_weapon_posture(WeaponPosture::Sheathed);
    auto at = c.placed(E::Special).centre;
    check(c.visible(E::Evade) && c.visible(E::Special), "sheathed layout keeps independent sprint and evade");
    c.finger_down(1, at, 100);
    check(c.buttons(100) == 0x200, "sprint starts immediately without recognition delay");
    c.finger_up(1, 101);
    check(c.buttons(101) == 0, "even a short sprint releases R immediately");
    c.finger_down(1, at, 200);
    c.finger_down(2, c.placed(E::Evade).centre, 201);
    check(c.buttons(201) == 0x4200, "two separate contacts combine sprint and evade only when both are pressed");
    c.finger_up(2, 202);
    check(c.buttons(202) == 0x200, "releasing evade leaves the independently held sprint");
    c.finger_down(2, c.placed(E::Evade).centre, 203);
    c.finger_up(1, 204);
    check(c.buttons(204) == 0x4000, "releasing sprint leaves the independently held evade");
    c.finger_up(2, 205);
    check(c.buttons(205) == 0, "both ordinary buttons release without trailing pulses");
    c.finger_down(1, at, 300);
    c.finger_move(1, {at.x, at.y - 10}, 301);
    check(c.buttons(301) == 0x200, "drift inside the sprint button remains sprint");
    c.finger_move(1, {at.x, at.y - 70}, 600);
    check(c.buttons(600) == 0, "swiping off sprint releases R and never injects X");
    c.finger_move(1, at, 601);
    check(c.buttons(601) == 0x200, "returning to the same sprint button only resumes R");
    c.finger_up(1, 602, true);
    check(c.buttons(602) == 0, "canceling sprint releases all its input");
    c.finger_down(1, at, 800);
    c.set_weapon_posture(WeaponPosture::Drawn);
    check(c.buttons(801) == 0 && c.visible(E::Evade), "drawing cancels sprint while keeping the direct evade button");
    c.finger_move(1, {at.x, at.y - 70}, 802);
    check(c.buttons(802) == 0, "old sprint cannot become a spirit slash");
    c.finger_down(3, c.placed(E::Evade).centre, 803);
    check(c.buttons(803) == 0x4000, "drawn evade remains immediate");
    c.release_all();
    c.set_weapon_posture(WeaponPosture::Unknown);
    check(c.visible(E::Evade), "unknown posture keeps the safe direct control");
    c.set_weapon_posture(WeaponPosture::Sheathed);
    layout.at(E::Special).shown = false;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(c.visible(E::Evade), "hiding sprint in the editor preserves ordinary evade");
    layout.at(E::Special).shown = true;
    layout.at(E::Special).buttons = 0x8000;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(c.visible(E::Evade), "rebound sprint never removes ordinary evade");
    c.finger_down(1, at, 900);
    check(c.buttons(900) == 0x8000, "custom sprint binding is preserved");
    c.release_all();
    layout = default_adaptive_layout();
    layout.at(E::Evade).buttons = 0x0001;
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    check(c.visible(E::Evade), "intentionally rebound evade stays visible");
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_compact(false);
    check(c.visible(E::Evade), "comparison switch preserves the separate sheathed evade");
    c.finger_down(1, at, 1000);
    c.finger_move(1, {at.x, at.y - 30}, 1001);
    check(c.buttons(1001) == 0x200, "noncompact sprint remains an ordinary held R");
    c.release_all();
    c.set_compact(true);
    c.finger_down(1, at, 1100);
    c.set_mode(AdaptiveMode::Hub);
    c.set_layout(default_adaptive_layout(AdaptiveMode::Hub), {0, 0, 1000, 600}, 1);
    at = c.placed(E::Special).centre;
    c.finger_down(1, at, 1200);
    c.finger_move(1, {at.x, at.y - 30}, 1201);
    check(c.buttons(1201) == 0x200 && !c.visible(E::Evade), "village sprint has no combat gesture");
}

void camera_taps() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    const Point at{500, 250};
    c.finger_down(1, at, 100);
    check(c.buttons(100) == 0, "free-space down does not reset the camera before recognition");
    c.finger_move(1, {502, 252}, 110);
    c.finger_up(1, 120);
    check(c.buttons(120) == 0x100 && c.buttons(204) == 0x100 && c.buttons(205) == 0,
        "short free-space tap tolerates jitter and sends a bounded L pulse");
    for (unsigned test = 0; test < 7; ++test) {
        c.release_all();
        c.finger_down(1, at, 1000);
        if (test == 0) {
            c.finger_move(1, {510, 250}, 1010);
            c.finger_move(1, at, 1020);
        } else if (test == 1) {
            c.finger_move(1, {509, 250}, 1010); // exact slop boundary
        } else if (test == 4) {
            c.release_all(); // focus loss
        } else if (test == 5) {
            c.set_mode(AdaptiveMode::Interface);
        } else if (test == 6) {
            c.set_compact(false);
        }
        c.finger_up(1, test == 2 ? 1200 : 1030, test == 3);
        check(c.buttons(1030) == 0 && c.buttons(1200) == 0,
            "drag/return, boundary motion, long hold, cancel, focus, interface and comparison mode never recenter");
        c.set_mode(AdaptiveMode::Quest);
        c.set_compact(true);
    }
    c.release_all();
    c.finger_down(1, at, 2000);
    c.finger_up(1, 1999);
    check(c.buttons(2000) == 0, "backwards tap timestamp cannot create a camera reset");
    c.finger_down(1, {-10, 250}, 2100);
    c.finger_move(1, at, 2101);
    c.finger_up(1, 2102);
    check(c.buttons(2102) == 0, "outside-picture contacts cannot become camera taps");
    c.finger_down(1, {250, 550}, 2200);
    c.finger_up(1, 2201);
    check(c.buttons(2201) == 0, "movement-stick taps are not camera taps");
    c.finger_down(1, c.placed(E::Evade).centre, 2300);
    c.finger_up(1, 2301);
    check(c.buttons(2301) == 0, "action-button release does not recenter");
    c.set_item_area(Area{360, 440, 230, 80});
    c.finger_down(1, {475, 480}, 2400);
    c.finger_move(1, {475, 440}, 2401);
    c.finger_move(1, {475, 480}, 2402);
    c.finger_up(1, 2403);
    check(c.buttons(2403) == 0 && c.sample_item_buttons(true) == 0,
        "vertical item swipe and return never use an item or recenter");
    c.finger_down(1, c.placed(E::Pause).centre, 2500);
    c.finger_down(2, at, 2501);
    c.finger_up(2, 2502);
    check(!c.commands_open() && c.buttons(2502) == 0, "dismissing Commands does not recenter");
    // Exercise both orders: reset then item, and item then reset.
    for (bool reset_first : {true, false}) {
        c.release_all();
        if (reset_first) {
            c.finger_down(1, at, 2550);
            c.finger_up(1, 2551);
            check(c.buttons(2551) == 0x100, "camera reset starts before the item touch");
        }
        c.finger_down(2, {475, 480}, 2552);
        c.finger_down(1, at, 2553);
        c.finger_up(1, 2554);
        check(c.buttons(2554) == 0, "active item contact suppresses camera reset");
        c.finger_up(2, 2555);
        c.finger_down(1, at, 2556);
        c.finger_up(1, 2557);
        for (unsigned read = 0; read < 3; ++read)
            check((c.buttons(2557) | c.sample_item_buttons(true)) == 0x8000,
                "camera taps cannot turn queued item use into L+Square selection");
        c.sample_item_buttons(true);
    }
    c.release_all();
    c.finger_down(2, c.placed(E::Alternate).centre, 2580);
    c.finger_down(1, at, 2581);
    c.finger_up(1, 2582);
    check(c.buttons(2582) == 0x2000, "held Circle cannot become an item-selection chord through a camera tap");
    c.release_all();
    c.set_mode(AdaptiveMode::Hub);
    c.set_layout(default_adaptive_layout(AdaptiveMode::Hub), {0, 0, 1000, 600}, 1);
    c.finger_down(1, at, 2600);
    c.finger_up(1, 2601);
    check(c.buttons(2601) == 0x100, "village free-space tap also resets the camera");
}

void available_choices() {
    const auto bit = [](AttackDirection d) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(d)); };
    const auto idle = static_cast<std::uint8_t>(
        bit(AttackDirection::Primary) | bit(AttackDirection::Up) | bit(AttackDirection::Down));
    AdaptiveAttack a;
    a.set_available(idle);
    a.begin(1, {100, 100}, 50, 100);
    a.move(1, {30, 100}, 110);
    check(a.direction(110) == AttackDirection::Blocked && a.output(110).buttons == 0 && a.output(110).lateral == 0,
        "unavailable side fade is silent and never falls back to Triangle");
    a.set_available(0x7Cu);
    a.move(1, {170, 100}, 120);
    a.end(1, 130, false);
    check(a.output(130).buttons == 0, "blocked contact stays blocked after mask changes and release");
    for (const auto d : {AttackDirection::Up, AttackDirection::Down, AttackDirection::Left, AttackDirection::Right}) {
        a.set_available(bit(d));
        a.begin(1, {100, 100}, 50, 200);
        const Point delta = d == AttackDirection::Up ? Point{0, -70}
            : d == AttackDirection::Down             ? Point{0, 70}
            : d == AttackDirection::Left             ? Point{-70, 0}
                                                     : Point{70, 0};
        a.move(1, {100 + delta.x, 100 + delta.y}, 210);
        check(a.output(210).buttons != 0, "available gesture emits its original PSP input");
        a.set_available(0u);
        check(a.output(211).buttons == 0 && a.output(211).lateral == 0,
            "closed window releases both buttons and side override");
        a.end(1, 212, false);
        a.set_available(0x7Cu);
        check(a.output(213).buttons == 0, "closing before release cannot leave a queued attack pulse");
    }
    a.set_available(0u);
    a.begin(1, {100, 100}, 50, 300);
    check(a.output(410).buttons == 0, "busy hunter cannot start a primary hold");
    a.end(1, 411, false);
    check(a.output(411).buttons == 0, "busy primary tap has no release pulse");
    a.set_available(std::nullopt);
    a.begin(1, {100, 100}, 50, 500);
    a.move(1, {30, 100}, 510);
    check(a.output(510).buttons == 0x3000 && a.output(510).lateral == -1,
        "unknown availability preserves legacy gestures");
    a.clear();
    a.set_available(0u);
    a.begin(1, {100, 100}, 50, 600, AttackProfile::GreatSword);
    check(a.output(710).buttons == 0x1000, "Long Sword masks do not gate other weapon profiles");
    a.clear();
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::LongSword);
    c.set_weapon_posture(WeaponPosture::Drawn);
    c.set_available(idle);
    check(c.available() == idle, "main selector exposes verified choices");
    auto at = c.placed(AdaptiveElement::Attack).centre;
    c.finger_down(1, at, 1000);
    c.finger_move(1, {at.x - 80, at.y}, 1010);
    check(c.buttons(1010) == 0 && c.stick(1010).x == 0, "controller routes unavailability to recognition and movement");
    c.set_mode(AdaptiveMode::Interface);
    check(!c.available(), "interface never advertises quest combo arrows");
}

void touch_comfort() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_bow_toggle(false); // Retained separate-aim comparison mapping.

    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::LongSword);
    c.set_weapon_posture(WeaponPosture::Drawn);
    c.finger_down(1, {500, 300}, 100);
    c.finger_down(2, {600, 300}, 100);
    c.finger_move(1, {510, 310}, 110);
    c.finger_move(2, {620, 320}, 110);
    auto drag = c.take_camera_drag();
    check(drag.x == 10 && drag.y == 10, "two free fingers cannot double camera motion");
    c.finger_down(3, {200, 450}, 120);
    c.finger_move(3, {250, 450}, 125);
    const auto attack = c.placed(E::Attack).centre;
    c.finger_down(4, attack, 130);
    c.finger_up(4, 135);
    check(c.stick(135).x > .5f && c.buttons(135) == 0x1000,
        "camera ownership leaves independent movement and attacks working");
    c.finger_up(1, 140, true);
    c.finger_move(2, {640, 320}, 145);
    check(c.take_camera_drag().x == 0, "ignored camera contact cannot take over when the owner lifts");
    c.finger_up(2, 150);
    c.finger_down(2, {600, 300}, 160);
    c.finger_move(2, {610, 300}, 170);
    check(c.take_camera_drag().x == 10, "a new camera contact acquires the released owner");
    c.release_all();
    c.set_item_area(Area{360, 440, 230, 80});
    c.finger_down(1, {500, 300}, 200);
    c.finger_down(2, {475, 480}, 201);
    c.finger_move(2, {475, 530}, 210);
    c.finger_up(2, 220);
    check(c.take_camera_drag().y == 0 && c.sample_item_buttons(true) == 0,
        "vertical item swipe cannot steal camera ownership or use an item");
    c.release_all();
    c.set_attack_profile(AttackProfile::Bow);
    const auto aim = c.placed(E::Special).centre;
    c.finger_down(1, aim, 300);
    c.finger_down(2, {475, 480}, 301);
    c.finger_move(2, {475, 530}, 310);
    check(c.take_camera_drag().y == 0, "item swipe cannot add camera motion to captured ranged aim");
    c.finger_move(1, {aim.x + 12, aim.y + 8}, 315);
    drag = c.take_camera_drag();
    check(drag.x == 12 && drag.y == 8, "captured aim still owns its camera motion");
    c.release_all();
    c.set_item_area(std::nullopt);
    const auto toggle = c.placed(E::DpadToggle);
    c.finger_down(1, {toggle.centre.x + toggle.radius * 1.25f, toggle.centre.y}, 400);
    check(c.take_dpad_toggle(), "small utility button accepts the bounded extra touch margin");
    c.finger_up(1, 410);
    c.finger_down(1, {toggle.centre.x + toggle.radius * 1.31f, toggle.centre.y}, 420);
    check(!c.take_dpad_toggle(), "extra touch margin stops before thirty-one percent");
    c.release_all();
    auto layout = default_adaptive_layout();
    layout.at(E::Alternate) = {Anchor::Left, .8f, .3f, .025f, 0x2000, true};
    layout.at(E::Attack) = {Anchor::Left, .92f, .3f, .1f, 0x1000, true};
    c.set_layout(layout, {0, 0, 1000, 600}, 1);
    c.set_compact(false);
    c.set_attack_profile(AttackProfile::Off);
    c.finger_down(1, {498, 180}, 500);
    check(c.buttons(500) == 0x1000, "a visible button wins over a smaller neighbour's invisible padding");
    c.finger_up(1, 510);
    c.finger_down(1, {480, 180}, 520);
    check(c.buttons(520) == 0x2000, "the small button's actual centre remains independently selectable");
    c.release_all();
}

void bow_toggle_controls() {
    using E = AdaptiveElement;
    AdaptiveControls c;
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_attack_profile(AttackProfile::Bow);
    c.set_weapon_posture(WeaponPosture::Drawn);
    const auto main = c.placed(E::Attack).centre, aim = c.placed(E::Special).centre,
               left = c.placed(E::LeftFire).centre;
    const auto toggle = [&](std::uint64_t ms) {
        c.finger_down(99, aim, ms);
        c.finger_up(99, ms + 1);
    };
    check(!c.bow_aiming() && !c.visible(E::LeftFire), "left fire is absent outside aim");
    c.finger_down(1, main, 100);
    c.finger_move(1, {main.x, main.y + 80}, 110);
    check(c.buttons(110) == 0x2000, "Bow melee down gesture is preserved outside aim");
    c.finger_up(1, 120, true);
    c.finger_down(99, aim, 200);
    check(
        c.buttons(200) == 0 && !c.bow_aiming(), "aim toggle commits on release, without charging or pressing R early");
    c.finger_up(99, 201);
    check(c.bow_aiming() && c.buttons(5000) == 0x200 && c.visible(E::LeftFire),
        "one tap latches aim and exposes the left duplicate");
    c.finger_down(1, {500, 200}, 5100);
    c.finger_move(1, {520, 215}, 5110);
    auto motion = c.take_camera_drag();
    check(motion.x == 20 && motion.y == 15, "free-screen drag controls both axes during latched aim");
    c.finger_down(2, left, 5120);
    check(c.buttons(5120) == 0x1200, "left duplicate starts charge while aim stays latched");
    check(c.sample_fire_buttons(true) == 0x1200, "actual guest read observes left-hand charge");
    c.finger_move(1, {530, 220}, 5130);
    motion = c.take_camera_drag();
    check(motion.x == 10 && motion.y == 5, "camera stays live while the other finger charges");
    c.finger_down(3, main, 5140);
    c.finger_up(3, 5141);
    check(c.buttons(5141) == 0x1200, "second fire button cannot release the owner's charge");
    c.finger_move(2, {left.x + 150, left.y + 100}, 5150);
    check(c.take_camera_drag().x == 0 && c.buttons(5150) == 0x1200,
        "fire contact remains captured and cannot become camera or melee");
    c.finger_up(2, 5160);
    check(c.buttons(5160) == 0x200 && c.bow_aiming(), "release shoots without leaving aim");
    c.finger_up(1, 5161);
    c.finger_down(4, {500, 200}, 5170);
    c.finger_up(4, 5171);
    check(c.buttons(5171) == 0x200, "free-screen aim taps cannot accidentally recenter with L");
    toggle(5200);
    check(c.buttons(5201) == 0 && !c.visible(E::LeftFire), "second aim tap exits and hides the duplicate");
    c.finger_down(99, aim, 5300);
    c.finger_move(99, {aim.x + 90, aim.y}, 5301);
    c.finger_up(99, 5302);
    check(!c.bow_aiming(), "dragging away from aim toggle does not activate it");
    toggle(5400);
    for (auto button : {main, left}) {
        c.finger_down(1, button, 5500);
        c.finger_up(1, 5501);
        for (int n = 0; n < 5; ++n)
            check(c.sample_fire_buttons(false) == 0x1200, "UI polls do not consume a short fire tap");
        for (int n = 0; n < 3; ++n)
            check(c.sample_fire_buttons(true) == 0x1200, "short tap remains sampleable as one charge request");
        check(c.sample_fire_buttons(true) == 0x200, "short fire tap releases only Triangle");
    }
    for (int kind = 0; kind < 4; ++kind) {
        if (!c.bow_aiming()) toggle(6000);
        c.finger_down(1, left, 6010);
        (void)c.sample_fire_buttons(true);
        if (kind == 0) toggle(6020);
        if (kind == 1) c.finger_up(1, 6020, true);
        if (kind == 2) c.release_all();
        if (kind == 3) {
            c.finger_down(2, c.placed(E::Evade).centre, 6020);
            c.finger_up(2, 6021);
        }
        check(c.buttons(6021) == 0x5200, "exit, canceled touch, host menu and evade cancel before charge release");
        for (int n = 0; n < 3; ++n) check(c.sample_fire_buttons(true) == 0x5200, "cancel reaches three guest samples");
        const auto expected = c.bow_aiming() ? 0x200 : 0;
        check(c.sample_fire_buttons(true) == expected,
            "cancel clears Triangle and Cross, preserving only intended aim state");
        c.finger_up(1, 6030);
        check(c.buttons(6030) == expected, "late lifting a canceled contact cannot fire again");
        c.release_all();
    }
    toggle(7000);
    c.finger_down(1, left, 7010);
    (void)c.sample_fire_buttons(true);
    c.set_mode(AdaptiveMode::Interface);
    check(!c.bow_aiming() && c.sample_fire_buttons(true) == 0 && !c.visible(E::LeftFire),
        "menus discard combat state instead of receiving R or X");
    c.set_mode(AdaptiveMode::Quest);
    toggle(7100);
    c.set_weapon_posture(WeaponPosture::Sheathed);
    check(!c.bow_aiming() && c.buttons(7102) == 0, "sheathing clears latched aim");
    c.set_weapon_posture(WeaponPosture::Drawn);
    toggle(7200);
    c.set_attack_profile(AttackProfile::Hammer);
    check(!c.bow_aiming() && c.buttons(7202) == 0, "weapon switch cannot leave R latched");
    c.set_attack_profile(AttackProfile::Bow);
    auto custom = default_adaptive_layout();
    custom.at(E::Special).buttons = 0x0001;
    c.set_layout(custom, {0, 0, 1000, 600}, 1);
    toggle(7300);
    check(!c.bow_aiming(), "custom special binding remains an ordinary button");
    c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
    c.set_bow_toggle(false);
    c.finger_down(99, aim, 7400);
    check(c.buttons(7400) == 0x200 && !c.visible(E::LeftFire), "comparison switch restores original held aim");
    c.release_all();
}

void unavailable_weapons() {
    using P = AttackProfile;
    using E = AdaptiveElement;
    for (auto profile : {P::HuntingHorn, P::Lance, P::Gunlance, P::SwitchAxe, P::LightBowgun, P::HeavyBowgun}) {
        check(!weapon_profile(profile) && !ranged_profile(profile) && secondary_profile(profile) == P::Off &&
                gesture_directions(profile) == 0,
            "deferred weapons have no Adaptive gesture implementation");
        AdaptiveAttack attack;
        check(!attack.begin(1, {0, 0}, 40, 1000, profile) && attack.output(1200).buttons == 0,
            "disabled primary recognizers reject contacts");
        AdaptiveControls c;
        c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
        c.set_attack_profile(P::Bow);
        c.set_weapon_posture(WeaponPosture::Drawn);
        c.finger_down(1, c.placed(E::Attack).centre, 1000);
        c.set_attack_profile(profile);
        check(c.quest_unavailable(), "deferred class disables the Quest controls");
        for (std::size_t i = 0; i < kAdaptiveElements; ++i) {
            const auto e = static_cast<E>(i);
            check(!c.visible(e), "all Quest elements, including commands and movement, are hidden");
            c.finger_down(10 + i, c.placed(e).centre, 1100);
            c.finger_move(10 + i, {500, 400}, 1110);
            c.finger_up(10 + i, 1120, false);
        }
        check(c.buttons(1200) == 0 && c.sample_item_buttons(true) == 0 && c.sample_fire_buttons(true) == 0 &&
                c.stick(1200).x == 0 && c.stick(1200).y == 0 && c.take_camera_drag().x == 0,
            "hidden Quest controls cannot generate input or retain an old supported weapon charge");
        c.set_mode(AdaptiveMode::Hub);
        c.set_layout(default_adaptive_layout(AdaptiveMode::Hub), {0, 0, 1000, 600}, 1);
        check(!c.quest_unavailable() && c.visible(E::Alternate), "village controls remain available");
        c.finger_down(30, c.placed(E::Alternate).centre, 1300);
        check(c.buttons(1300) == 0x2000, "an unsupported weapon can still talk to village NPCs");
        c.set_mode(AdaptiveMode::Interface);
        c.set_layout(default_adaptive_layout(AdaptiveMode::Interface), {0, 0, 1000, 600}, 1);
        check(c.visible(E::DPad) && c.visible(E::Evade), "native menus retain navigation and cancel");
        c.set_mode(AdaptiveMode::Quest);
        c.set_attack_profile(P::LongSword);
        c.set_layout(default_adaptive_layout(), {0, 0, 1000, 600}, 1);
        check(!c.quest_unavailable() && c.visible(E::Attack) && c.buttons(1500) == 0,
            "returning to a supported weapon restores clean Quest input");
    }
}

int main() {
    bow_toggle_controls();
    touch_comfort();
    available_choices();
    compact_movement();
    camera_taps();
    weapon_profiles();
    hammer_and_dual_blades();
    great_sword();
    shield_size();
    sword_and_shield_routes();
    sword_and_shield_items();
    dedicated_sheathe();
    remaining_weapon_profiles();
    unavailable_weapons();
    ranged_controls();
    compact_commands();
    command_commit_contract();
    context_modes();
    weapon_posture();
    timing();
    directions();
    invalid_and_ownership();
    common_controls();
    mobile_controls();
    item_sequences();
    visual_feedback();
    if (failures)
        std::cerr << failures << " adaptive attack failures\n";
    else
        std::cout << "Adaptive attack contracts passed\n";
    return failures ? 1 : 0;
}
