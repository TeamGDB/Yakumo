// yakumo/mod.h as a C++11 mod sees it: without the default sizes C++14 gives,
// its structs stay aggregates and are brace-initialized as in C. Compiled as
// an object of its own in C++11 (cmake/Mods.cmake), never run: it no longer
// compiles if a default member initializer reaches C++11.
#include "yakumo/mod.h"

namespace {

const ymod_quest_monster kMonster = {"tigrex", 1, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100, 0u};
const ymod_area_item kItem = {"Cannon Ammo", 1u, 100u};
const ymod_area_spot kSpot = {
    YMOD_SPOT_GATHERING, {0.0f, 0.0f, 0.0f}, 150.0f, 200.0f, 0, &kItem, 1u, 10u, 0u, sizeof(ymod_area_item)};
const ymod_quest kQuest = {sizeof(ymod_quest), 62000u, 8, 1, 30u, 0u, 0u, "tigrex", 1u, &kMonster, 1u};

} // namespace

const ymod_quest *mhp3rd_sdk_cxx11_quest() {
    return kSpot.item_count != 0u ? &kQuest : nullptr;
}
