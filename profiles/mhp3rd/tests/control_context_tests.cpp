#include "game/control_context.hpp"
#include "game/guest_ram.hpp"
#include <iostream>
#include <string_view>
using namespace mhp3rd::game;
using namespace mhp3rd::game::control_context;
namespace {
int failures{};
void check(bool ok, const char *why) {
    if (!ok) {
        ++failures;
        std::cerr << why << '\n';
    }
}
void task(BufferRam &ram, std::string_view name) {
    constexpr auto at = 0x0A05E600u;
    ram.store32(at, 0x336F574Du);
    ram.store32(at + 8, at);
    for (unsigned i = 0; i < 32; ++i) ram.store8(at + 32 + i, i < name.size() ? name[i] : 0);
}
}
int main() {
    BufferRam ram(0x08800000u, 0x03800000u);
    check(read(ram) == Context{}, "empty/title state uses interface controls");
    task(ram, "lobby_task.ovl");
    check(read(ram) == Context{Scene::Hub, true}, "missing lobby state is not free movement");
    ram.store32(0x0A25DD28u, 0x0A25DD2Cu);
    check(read(ram) == Context{Scene::Hub, false}, "lobby walking");
    for (auto state : {12u, 25u, 2u, 11u, 30u}) {
        ram.store32(0x0A25DD2Cu, state);
        check(read(ram).interface_open, "lobby menu, NPC, chest, Smithy and departure");
    }
    ram.store32(0x0A25DD2Cu, 0);
    check(!read(ram).interface_open, "lobby menu closes");
    task(ram, "game_task.ovl");
    check(read(ram) == Context{Scene::Quest, true}, "missing quest menu manager is safe");
    constexpr auto manager = 0x09FF53B4u;
    ram.store32(0x09FF53B0u, manager);
    check(read(ram) == Context{Scene::Quest, false}, "quest walking");
    for (auto offset : {4u, 0x18u, 0xB95u}) {
        ram.store8(manager + offset, 1);
        check(read(ram).interface_open, "each active interface state takes priority");
        ram.store8(manager + offset, 0);
        check(!read(ram).interface_open, "closing each interface restores quest controls");
    }
    for (auto bad : {1u, manager + 1u, 0xFFFFFFFCu, 0x0BFFFFFCu}) {
        ram.store32(0x09FF53B0u, bad);
        check(read(ram).interface_open, "unmapped, unaligned and wrapping pointers are rejected");
    }
    ram.store32(0x09FF53B0u, manager);
    check(refresh(ram), "refresh records changed scene");
    check(current() == Context{Scene::Quest, false}, "current snapshot");
    check(!refresh(ram), "unchanged frame");
    task(ram, "title_task.ovl");
    check(refresh(ram) && current() == Context{}, "title clears previous quest");
    task(ram, "game_task.ovl.extra");
    check(read(ram) == Context{}, "name must match exactly");
    task(ram, "game_task.ovl");
    ram.store32(0x0A05E608u, 0);
    check(read(ram) == Context{}, "wrong load address rejected");
    ram.store32(0x0A05E608u, 0x0A05E600u);
    ram.store32(0x0A05E600u, 0);
    check(read(ram) == Context{}, "wrong overlay magic rejected");
    BufferRam tiny(0x08800000u, 32);
    check(read(tiny) == Context{}, "unmapped overlay header");
    return failures ? 1 : 0;
}
