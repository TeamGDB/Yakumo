#pragma once
#include "game/guest_ram.hpp"
#include "game/game_data.hpp"
#include "game/equipment_models.hpp"
#include <array>
namespace mhp3rd::test::combat_fixture {
using namespace game;
inline constexpr std::uint32_t target = 0x09000000u, hunter = 0x096498D0u, collision = 0x09102000u,
                               actor_table = 0x09100000u, vt = 0x09101000u;
inline void setup(Ram &ram) {
    const auto zero = [&](std::uint32_t at, unsigned count) {
        for (unsigned i = 0; i < count; ++i) ram.store8(at + i, 0u);
    };
    // The renderer suite reuses RAM after unrelated fixtures; every object in
    // this scenario must be fully initialized instead of relying on fresh RAM.
    zero(target, 0x1000u);
    zero(hunter, 0x280u);
    zero(actor_table, 0x100u);
    zero(vt, 0xACu);
    zero(collision, 0x18u);
    zero(0x09103000u, 0xB96u);
    zero(0x0A1B0AE0u, 20u * 4u);
    zero(kCharacter, 26u);
    zero(kCharacter + kCharacterWeapon, kEquipmentRecord);

    ram.store32(0x0A05E600u, 0x336F574Du);
    ram.store32(0x0A05E608u, 0x0A05E600u);
    const char name[] = "game_task.ovl";
    for (unsigned i = 0; i < sizeof(name); ++i) ram.store8(0x0A05E620u + i, name[i]);
    ram.store32(0x09FF53B0u, 0x09103000u);
    ram.store16(kCharacter, 0xFF34u);
    const auto equipped = kCharacter + kCharacterWeapon;
    ram.store8(equipped, 1u);
    ram.store8(equipped + 1u, 12u);
    ram.store16(equipped + 2u, 1u);
    ram.store32(0x088B6B68u, 0x8C420008u);
    ram.store32(0x088E49C8u, 0x30420008u);
    ram.store32(0x09FBE794u, actor_table);
    ram.store32(actor_table + 8u, hunter);
    ram.store32(hunter, vt);
    ram.store32(vt + 0xA8u, 0x088A58DCu);
    ram.store32(0x0A1B0AE0u, target);
    ram.store8(target + 0x62u, 42u);
    ram.store16(target + 0x246u, 700u);
    ram.store16(target + 0x288u, 1000u);
    ram.store32(collision + 0x14u, hunter);
    constexpr std::array<std::array<std::uint32_t, 2>, 6> signature{
        {{0x0A07BA2Cu, 0x27BDFFF0u}, {0x0A07BA54u, 0x0E81EE1Bu}, {0x0A07BA78u, 0xA46201ECu}, {0x0A025748u, 0x92A50008u},
            {0x0A025750u, 0x02602021u}, {0x0A02575Cu, 0x0E81EE8Bu}}};
    for (const auto &entry : signature) ram.store32(entry[0], entry[1]);
}
}
