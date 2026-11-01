#include "gpu/hud_layout.hpp"
#include "gpu/hud_packets.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace mhp3rd::gpu::hud;
int failures{};
void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
bool near(float a, float b) {
    return std::fabs(a - b) < 0.001f;
}
void test_screen_bounds() {
    const Rect picture{100, 50, 1060, 594};
    auto b = screen_bounds({}, Part::Items, picture);
    check(near(b.left, 708) && near(b.top, 386) && near(b.right, 1060) && near(b.bottom, 594),
        "default item hit rectangle follows a letterboxed picture");
    Layout l;
    l.blocks[2] = {.5f, -.2f, -.1f};
    b = screen_bounds(l, Part::Items, picture);
    check(near(b.left, 692) && near(b.top, 435.6f) && near(b.right, 868) && near(b.bottom, 539.6f),
        "HUD item screen rectangle follows independent scale and both offsets");
    b = screen_bounds({}, Part::Items, picture, .5f, 1.f);
    check(near(b.left, 644) && near(b.right, 820), "Fill preserves the renderer's horizontal UI fitting");
    l.blocks[2] = {2.f, 0, 0};
    b = screen_bounds(l, Part::Items, picture);
    check(b.left >= picture.left && b.top >= picture.top && b.right <= picture.right && b.bottom <= picture.bottom,
        "oversized HUD hot zones remain inside the picture");
    const auto bad = screen_bounds({}, Part::Items, {0, 0, std::numeric_limits<float>::quiet_NaN(), 10});
    check(bad.left == 0 && bad.right == 0, "invalid picture geometry cannot create a hit target");
    check(screen_bounds({}, Part::Items, {0, 0, 0, 10}).bottom == 0, "empty picture has no hot zone");
}
void test_layout() {
    for (float fit_x : {0.5f, 0.9f, 1.0f})
        for (float fit_y : {0.6f, 1.0f})
            for (Part p : kParts) {
                check(transform({}, p, fit_x, fit_y).identity(), "defaults preserve original geometry at every aspect");
                check(name(p)[0] != '\0', "each movable block has an editor label");
            }
    check(transform({}, Part::None).identity() && transform({}, Part::Other).identity() &&
            bounds({}, Part::Other).right == 0.0f && name(Part::None)[0] == '\0',
        "dialogs, prompts and world-space names are not layout blocks");

    Layout l;
    for (auto &p : l.blocks) p.scale = 0.5f;
    auto s = bounds(l, Part::Status);
    auto m = bounds(l, Part::Map);
    auto i = bounds(l, Part::Items);
    check(near(s.left, 0) && near(s.top, 0) && near(s.right, 175), "status shrinks about the top-left corner");
    check(near(m.right, 480) && near(m.left, 424) && near(m.top, 0), "map shrinks about the top-right corner");
    check(near(i.right, 480) && near(i.bottom, 272) && near(i.left, 392) && near(i.top, 220),
        "items shrink about the bottom-right corner");
    l.blocks[index(Part::Map)].scale = 1.0f;
    check(transform(l, Part::Map).identity() && near(bounds(l, Part::Status).right, 175),
        "each block has an independent scale");

    for (Part p : kParts)
        for (float fit : {0.65f, 1.0f}) {
            Layout moved;
            for (auto &placement : moved.blocks) placement.scale = 0.7f;
            move_to(moved, p, 80, 90, fit, 1);
            auto b = bounds(moved, p, fit, 1);
            check(near(b.left, 80) && near(b.top, 90), "drag coordinates agree with render coordinates after fitting");
            const auto other = p == Part::Map ? Part::Items : Part::Map;
            Layout baseline;
            for (auto &placement : baseline.blocks) placement.scale = 0.7f;
            check(bounds(moved, other, fit, 1).left == bounds(baseline, other, fit, 1).left,
                "moving one block leaves another unchanged");
            move_to(moved, p, -1000, -1000, fit, 1);
            b = bounds(moved, p, fit, 1);
            check(near(b.left, 0) && near(b.top, 0), "drag cannot lose a block past the top or left edge");
            move_to(moved, p, 1000, 1000, fit, 1);
            b = bounds(moved, p, fit, 1);
            check(near(b.right, 480) && near(b.bottom, 272), "drag cannot lose a block past the right or bottom edge");
        }

    Layout large;
    large.blocks[0].scale = 2.0f;
    move_to(large, Part::Status, -800, -500);
    auto b = bounds(large, Part::Status);
    check(b.left <= 0 && b.right >= 480 && b.top <= 0 && b.bottom >= 272,
        "an oversized block can be panned while still spanning the picture");
    const auto unchanged = large;
    move_to(large, Part::None, 10, 10);
    move_to(large, Part::Status, std::numeric_limits<float>::quiet_NaN(), 0);
    check(large == unchanged, "invalid drag input cannot corrupt placements");

    Layout invalid;
    invalid.blocks[1] = {100, std::numeric_limits<float>::infinity(), -100};
    auto t = transform(invalid, Part::Map);
    check(std::isfinite(t.scale) && std::isfinite(t.x) && std::isfinite(t.y) && near(t.scale, 2),
        "non-finite settings cannot reach vertex positions and excessive scales are bounded");
    invalid.blocks[1].scale = -100;
    check(near(transform(invalid, Part::Map).scale, 0.5f), "the scale obeys the lower limit");

    Transform clip{0.5f, 100, 30};
    float partial[]{20, 40, 100, 120};
    clip.clip(partial, false, false);
    check(near(partial[0], 110) && near(partial[1], 50) && near(partial[2], 150) && near(partial[3], 90),
        "a partial scissor follows the moved vertices");
    float full[]{0, 0, 480, 272};
    clip.clip(full, true, true);
    check(full[0] == 0 && full[1] == 0 && full[2] == 480 && full[3] == 272,
        "full-picture clipping must not shrink or move with the block");
}
void test_movement() {
    Layout base;
    base.blocks[0].scale = 0.5f;
    move_to(base, Part::Status, 80, 80);
    const Rect start = bounds(base, Part::Status);
    auto moved = base;
    move_by(moved, Part::Status, 1, 0, 0.1f);
    const auto right = bounds(moved, Part::Status);
    check(near(right.left - start.left, 12) && near(right.top, start.top),
        "full directional input moves at 120 game pixels per second");
    check(moved.blocks[1] == base.blocks[1] && moved.blocks[2] == base.blocks[2],
        "directional movement affects only the chosen block");
    moved = base;
    move_by(moved, Part::Status, 1, 1, 0.1f);
    auto r = bounds(moved, Part::Status);
    check(near(std::hypot(r.left - start.left, r.top - start.top), 12),
        "diagonal movement has the same total speed as cardinal movement");
    moved = base;
    move_by(moved, Part::Status, 0.5f, 0, 0.1f);
    check(near(bounds(moved, Part::Status).left - start.left, 6), "partial stick input gives proportional movement");
    moved = base;
    move_by(moved, Part::Status, -1, -1, 0.1f);
    r = bounds(moved, Part::Status);
    check(r.left < start.left && r.top < start.top, "left and up directions move towards the top-left");
    moved = base;
    for (int n = 0; n < 10; ++n) move_by(moved, Part::Status, 1, 0, 0.01f);
    check(near(bounds(moved, Part::Status).left, right.left), "movement is independent of UI frame rate");
    moved = base;
    move_by(moved, Part::Status, 2, 0, 5);
    check(near(bounds(moved, Part::Status).left, right.left), "excessive input and long UI stalls are bounded");
    const auto before = moved;
    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) move_by(moved, Part::Status, 1, 0, bad);
    move_by(moved, Part::Status, std::numeric_limits<float>::infinity(), 0, 0.1f);
    move_by(moved, Part::Status, 0, std::numeric_limits<float>::quiet_NaN(), 0.1f);
    move_by(moved, Part::Status, 0, 0, 0.1f);
    move_by(moved, Part::Other, 1, 0, 0.1f);
    check(moved == before, "invalid, absent and non-HUD movement does not change placements");
    for (float fit : {0.6f, 1.0f}) {
        moved = base;
        for (int n = 0; n < 100; ++n) move_by(moved, Part::Status, -1, -1, 0.1f, fit, 1);
        r = bounds(moved, Part::Status, fit, 1);
        check(near(r.left, 0) && near(r.top, 0), "controller movement obeys the picture edge after fitting");
    }
}
void test_packets() {
    PacketTags packets;
    packets.file(1000, 10, Part::Status);
    packets.file(2000, 5, Part::Map);
    check(packets.at(1000) == Part::Status && packets.at(1039) == Part::Status && packets.at(1040) == Part::None,
        "packet boundaries are half-open");
    check(packets.at(0, 2004) == Part::Map && packets.at(1004, 2004) == Part::Status,
        "display-list calls inherit a group while a directly tagged command takes precedence");
    packets.file(1016, 2, Part::None);
    check(packets.at(1000) == Part::None && packets.at(1038) == Part::None && packets.at(2004) == Part::Map,
        "a non-HUD overwrite clears the whole old packet, preserving unrelated packets");
    packets.file(1996, 10, Part::Items);
    check(packets.at(2004) == Part::Items, "an overlapping HUD packet replaces the old owner");
    packets.file(0, 4, Part::Status);
    packets.file(1996, 0, Part::None);
    packets.file(0xFFFFFFFCu, 2, Part::Status);
    check(packets.at(0) == Part::None && packets.at(0xFFFFFFFCu) == Part::None && packets.at(2004) == Part::Items,
        "empty, null and overflowing packet ranges cannot tag arbitrary memory");
    packets.file(1996, 10, Part::Other);
    check(packets.at(2004) == Part::Other, "world-space HUD stays hideable without becoming movable");
    PacketTags bounded;
    for (std::uint32_t a = 4; a <= 65537u * 4u; a += 4) bounded.file(a, 1, Part::Status);
    check(bounded.at(4) == Part::None, "the tracking table bounds runaway packet streams");

    QueueTags text, icons;
    std::uint8_t count = 0, icon_count = 0, other_count = 0;
    text.update(1000, 12, 256, count, 2, Part::Status);
    text.update(1000, 12, 256, count, 3, Part::Items);
    text.update(5000, 12, 256, other_count, 1, Part::Map);
    icons.update(8000, 8, 10, icon_count, 1, text.at(1024));
    check(text.at(1000) == Part::Status && text.at(1012) == Part::Status && text.at(1024) == Part::Items &&
            icons.at(8000) == Part::Items,
        "deferred glyphs and button icons retain their original block");
    text.update(1000, 12, 256, count, 0, Part::None);
    text.update(1000, 12, 256, count, 1, Part::None);
    check(text.at(1000) == Part::None && text.at(1012) == Part::None && text.at(5000) == Part::Map,
        "emptied queues lose old owners before dialogs reuse entries, without affecting another queue");
    icons.clear();
    check(icons.at(8000) == Part::None, "icon ownership ends when its frame ends");
}
void test_origins() {
    check(quest_part(0x0A17399C) == Part::Status && quest_part(0x0A173A68) == Part::Status &&
            quest_part(0x0A173A74) == Part::Status && quest_part(0x0A173B88) == Part::Status,
        "clock, weapon gauges, bars and party plates share the status group");
    check(quest_part(0x0A173BF8) == Part::Map && quest_part(0x0A173C04) == Part::Items,
        "map and item selection have separate origins");
    check(quest_part(0x0A173ADC) == Part::Other && quest_part(0) == Part::Other,
        "world-space name labels and unrecognized prompts do not move");
}
} // namespace

int main() {
    test_layout();
    test_screen_bounds();
    test_movement();
    test_packets();
    test_origins();
    if (failures) return 1;
    std::cout << "HUD layout and ownership tests passed\n";
    return 0;
}