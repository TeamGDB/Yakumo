// Public synthetic memory only: no game corpus, names or saves.
#include "game/monster_state.hpp"
#include "game/guest_ram.hpp"
#include "ui/combat_overlay_layout.hpp"

#include <bit>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using namespace mhp3rd::game;
constexpr std::uint32_t task = 0x0A05E600u, table = 0x0A1B0AE0u, actor = 0x09000000u;
int failures{};
void check(bool yes, const char *message) {
    if (!yes) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void quest(Ram &ram) {
    ram.store32(task, 0x336F574Du);
    ram.store32(task + 8u, task);
    constexpr std::string_view name = "game_task.ovl";
    for (unsigned i = 0; i < name.size(); ++i) ram.store8(task + 32u + i, name[i]);
    ram.store8(task + 32u + name.size(), 0);
}
void monster(Ram &ram, std::uint32_t address = actor) {
    ram.store32(table, address);
    ram.store32(address + 4u, 0u);
    ram.store8(address + 0x62u, 12u);
    ram.store16(address + 0x246u, 800u);
    ram.store16(address + 0x288u, 1000u);
    ram.store32(address + 0x80u, std::bit_cast<std::uint32_t>(-25.f));
    ram.store32(address + 0x84u, std::bit_cast<std::uint32_t>(100.f));
    ram.store32(address + 0x88u, std::bit_cast<std::uint32_t>(150.f));
}
void test_monsters() {
    BufferRam ram(0x08800000u, 0x02000000u);
    check(combat::read_monsters(ram).empty(), "unloaded guest is empty");
    quest(ram);
    monster(ram);
    auto list = combat::read_monsters(ram);
    check(list.size() == 1 && list[0].address == actor && list[0].kind == 12 && list[0].health == 800 &&
            list[0].maximum == 1000 && list[0].in_area && list[0].position == std::array<float, 3>{-25.f, 100.f, 150.f},
        "original health and coordinates are read without a delta estimate");
    ram.store32(table, 0u);
    ram.store32(table + 19u * 4u, actor);
    check(combat::read_monsters(ram).size() == 1 && combat::read_monsters(ram)[0].large,
        "a large species in the last manager slot remains large");
    check(combat::visible_monsters(combat::read_monsters(ram), true).size() == 1,
        "Large mode retains a large monster outside the first five slots");
    ram.store32(table + 19u * 4u, 0u);
    ram.store32(table, actor);
    ram.store32(actor + 4u, 7u);
    check(!combat::read_monsters(ram)[0].in_area, "other-area flag is retained for presentation filtering");
    check(combat::read_monsters(ram)[0].large, "large identity survives an other-area flag");
    ram.store32(table + 4u, actor);
    check(combat::read_monsters(ram).size() == 1, "duplicate slot cannot duplicate the overlay");
    ram.store16(actor + 0x246u, 0xFFFEu);
    check(combat::read_monsters(ram)[0].health == 0, "lethal negative HP is displayed as zero");
    ram.store16(actor + 0x246u, 1001u);
    check(combat::read_monsters(ram).empty(), "transient HP above maximum rejected");
    monster(ram);
    ram.store16(actor + 0x288u, 0u);
    check(combat::read_monsters(ram).empty(), "companion or uninitialized slot rejected");
    ram.store16(actor + 0x288u, 0xFFFFu);
    check(combat::read_monsters(ram).empty(), "invalid signed maximum rejected");
    monster(ram);
    ram.store8(actor + 0x62u, 75u);
    check(combat::read_monsters(ram).empty(), "unknown kind rejected");
    for (float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        monster(ram);
        ram.store32(actor + 0x84u, std::bit_cast<std::uint32_t>(bad));
        check(combat::read_monsters(ram).empty(), "nonfinite world position rejected");
    }
    ram.store32(table + 4u, 0u);
    for (auto bad : {0u, 0x087FFFFCu, actor + 1u, 0x0C000000u, 0xFFFFFFFCu, 0x0A7FFFC0u}) {
        ram.store32(table, bad);
        check(combat::read_monsters(ram).empty(), "invalid or unmapped pointer is not dereferenced");
    }
    constexpr auto end_actor = 0x0A800000u - 0x28Cu;
    monster(ram, end_actor);
    check(combat::read_monsters(ram).size() == 1, "object ending exactly at mapped boundary accepted");
    ram.store32(task + 8u, task + 4u);
    check(combat::read_monsters(ram).empty(), "relocated or stale overlay rejected");
    quest(ram);
    ram.store8(task + 32u + 13u, 'x');
    check(combat::read_monsters(ram).empty(), "overlay name must terminate exactly");
    quest(ram);
    monster(ram);
    ram.store32(table + 4u, 0u);
    ram.store8(actor + 0x62u, 11u); // Jaggia, including if it occupies slot zero.
    ram.store16(actor + 0x246u, 5000u);
    ram.store16(actor + 0x288u, 5000u);
    const auto first_small = combat::read_monsters(ram);
    check(first_small.size() == 1 && !first_small[0].large && combat::visible_monsters(first_small, true).empty() &&
            combat::visible_monsters(first_small, false).size() == 1,
        "a high-HP small species in slot zero is hidden in Large mode and kept in All mode");
    ram.store32(task, 0u);
    check(combat::read_monsters(ram).empty(), "overlay signature checked each snapshot");
    BufferRam tiny(task, 63u);
    check(combat::read_monsters(tiny).empty(), "partial header is not read");
    BufferRam missing(task, 64u);
    quest(missing);
    check(combat::read_monsters(missing).empty(), "missing monster table is not read");
}
void test_filter_and_layout() {
    using combat::Monster;
    for (const std::uint8_t small : {10, 11, 13, 17, 21, 22, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39, 48, 49,
             50, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74})
        check(!combat::large_monster_kind(small), "all small species and scenery remain excluded");
    for (const std::uint8_t large : {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 14, 15, 16, 18, 19, 20, 23, 24, 25, 32, 40, 41, 42,
             43, 44, 45, 46, 47, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62})
        check(combat::large_monster_kind(large), "all forty large species include subspecies and elder dragons");
    check(!combat::large_monster_kind(0) && !combat::large_monster_kind(75) && !combat::large_monster_kind(255),
        "unknown species are not guessed to be large");
    const std::vector<Monster> actors{{1, 11, 1000, 1000, {}, true, false}, {2, 42, 50, 100, {}, true, true},
        {3, 42, 0, 100, {}, true, true}, {4, 42, 50, 100, {}, false, true}, {5, 11, 40, 100, {}, true, false}};
    const auto large = combat::visible_monsters(actors, true);
    const auto all = combat::visible_monsters(actors, false);
    check(large.size() == 1 && large[0].address == 2, "Large mode uses game identity, not an HP threshold");
    check(all.size() == 3 && all[0].address == 2 && all[1].address == 1 && all[2].address == 5,
        "All mode excludes dead/absent monsters and keeps stable order with large monsters first");
    check(combat::visible_monsters({}, false).empty(), "empty area has no panels");
    namespace layout = mhp3rd::ui::combat_layout;
    for (const layout::Rect picture : {layout::Rect{0, 0, 480, 272}, layout::Rect{120, 40, 1920, 1080},
             layout::Rect{0, 0, 2400, 1080}, layout::Rect{0, 0, 272, 480}}) {
        for (std::size_t count : {1u, 3u, 5u, 20u}) {
            for (const auto &xy : layout::kPresets) {
                for (const float scale : {0.5f, 1.f, 2.f}) {
                    const auto s = layout::place(picture, 20.f, count, xy[0], xy[1], scale);
                    check(s.font > 0 && s.bounds.x >= picture.x && s.bounds.y >= picture.y &&
                            s.bounds.x + s.bounds.width <= picture.x + picture.width + 0.001f &&
                            s.bounds.y + s.bounds.height <= picture.y + picture.height + 0.001f,
                        "all presets and monster counts stay inside the actual game picture");
                    const auto moved = layout::move_to(s, s.origin_x + s.free_x * 0.37f, s.origin_y + s.free_y * 0.63f);
                    check(std::abs(moved[0] - 0.37f) < 0.001f &&
                            (s.free_y < 0.001f || std::abs(moved[1] - 0.63f) < 0.001f),
                        "manual placement round-trips available-space coordinates");
                    const auto edge = layout::move_to(s, -1000, 100000);
                    check(edge[0] == 0.f && (s.free_y == 0.f || edge[1] == 1.f),
                        "dragging outside the picture clamps the whole stack");
                }
            }
        }
    }
    const auto base = layout::place({0, 0, 1920, 1080}, 20, 3, 1, 1, 1);
    const auto bigger = layout::place({0, 0, 1920, 1080}, 20, 3, 1, 1, 2);
    const auto smaller = layout::place({0, 0, 1920, 1080}, 20, 3, 1, 1, 0.5f);
    check(bigger.font == 2.f * base.font && bigger.row == 2.f * base.row && bigger.gap == 2.f * base.gap &&
            bigger.bounds.width == 2.f * base.bounds.width && bigger.bounds.height == 2.f * base.bounds.height,
        "scale enlarges text, bars, panel width and spacing uniformly");
    check(smaller.font == base.font * 0.5f && smaller.bounds.width == base.bounds.width * 0.5f,
        "half scale produces a smaller panel instead of only smaller text");
    check(std::abs(bigger.bounds.x + bigger.bounds.width - base.bounds.x - base.bounds.width) < 0.001f &&
            std::abs(bigger.bounds.y + bigger.bounds.height - base.bounds.y - base.bounds.height) < 0.001f,
        "resizing preserves the selected right and bottom anchors");
    for (const float scale : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        check(layout::place({0, 0, 1920, 1080}, 20, 3, 1, 1, scale).bounds.width == base.bounds.width,
            "nonfinite scale safely falls back to 100 percent");
    check(layout::place({0, 0, 1920, 1080}, 20, 3, 1, 1, -5).bounds.width == smaller.bounds.width &&
            layout::place({0, 0, 1920, 1080}, 20, 3, 1, 1, 5).bounds.width == bigger.bounds.width,
        "scale is bounded even when called outside settings");
    check(layout::preset(0.5f, 0.f) == 1 && layout::preset(0.37f, 0.63f) == -1,
        "default and custom positions have independent labels");
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        const auto s = layout::place({0, 0, 480, 272}, 20, 3, bad, bad);
        check(std::isfinite(s.bounds.x) && std::isfinite(s.bounds.y), "invalid saved positions use safe defaults");
        check(layout::place({0, 0, bad, 272}, 20, 3, 0, 0).font == 0, "nonfinite picture rejected");
        check(layout::place({0, 0, 480, 272}, bad, 3, 0, 0).font == 0, "nonfinite font rejected");
    }
    check(layout::place({0, 0, 0, 272}, 20, 3, 0, 0).font == 0 &&
            layout::place({0, 0, 480, 272}, 0, 3, 0, 0).font == 0 &&
            layout::place({0, 0, 480, 272}, 20, 0, 0, 0).font == 0 &&
            layout::place({0, 0, 480, 272}, 20, 21, 0, 0).font == 0,
        "empty, oversized or unavailable layout is not drawn");
}

}
int main() {
    test_monsters();
    test_filter_and_layout();
    return failures ? 1 : 0;
}
