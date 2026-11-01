// Synthetic loaded-character records only; no game files or player saves.
#include "game/weapon_state.hpp"
#include "game/equipment_models.hpp"
#include "game/game_data.hpp"
#include "game/guest_ram.hpp"

#include <array>
#include <bit>
#include <limits>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
using namespace mhp3rd::game;
namespace weapon = mhp3rd::game::weapon;
int failures{};
constexpr auto kRecord = kCharacter + kCharacterWeapon;
constexpr auto kSize = kCharacterWeapon + kEquipmentRecord;

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void equip(Ram &ram, std::uint8_t kind, std::uint16_t id) {
    ram.store16(kCharacter, 0xFF34u); // Synthetic fullwidth T.
    ram.store8(kRecord, 1u);
    ram.store8(kRecord + 1u, kind);
    ram.store16(kRecord + 2u, id);
}

void test_types() {
    BufferRam ram(kCharacter, kSize);
    using enum weapon::Type;
    const std::array<std::pair<std::uint8_t, weapon::Type>, 12> types{{
        {5, GreatSword},
        {12, LongSword},
        {6, SwordAndShield},
        {16, DualBlades},
        {7, Hammer},
        {17, HuntingHorn},
        {8, Lance},
        {14, Gunlance},
        {13, SwitchAxe},
        {11, LightBowgun},
        {9, HeavyBowgun},
        {15, Bow},
    }};
    for (const auto &[kind, type] : types) {
        equip(ram, kind, 123u);
        check(weapon::read(ram) == weapon::Equipped{type, 123u}, "each equipped class has the correct storage kind");
    }
    for (unsigned kind = 0; kind < 256u; ++kind) {
        bool known = false;
        for (const auto &entry : types) known |= entry.first == kind;
        if (known) continue;
        equip(ram, static_cast<std::uint8_t>(kind), 123u);
        check(!weapon::read(ram), "armor, unused and unknown kinds have no weapon identity");
    }
    equip(ram, 5u, 65535u);
    check(weapon::read(ram) == weapon::Equipped{GreatSword, 65535u},
        "identity preserves the full record ID without indexing a name/model table");
    equip(ram, 5u, 0u);
    check(!weapon::read(ram), "no-equipment ID is unavailable even in a marked-used record");
    equip(ram, 5u, 1u);
    ram.store8(kRecord, 0u);
    check(!weapon::read(ram), "an unused record does not expose stale weapon fields");
}

void test_ranges_and_character() {
    BufferRam empty(0u, 0u);
    check(!character_loaded(empty) && !weapon::read(empty), "unmapped character is unavailable");
    BufferRam short_name(kCharacter, kHunterNameUnits * 2u - 1u);
    short_name.store16(kCharacter, 0xFF34u);
    check(!character_loaded(short_name) && !weapon::read(short_name), "truncated name range stays unavailable");
    BufferRam short_record(kCharacter, kSize - 1u);
    equip(short_record, 5u, 1u);
    check(character_loaded(short_record) && !weapon::read(short_record), "truncated equipped record is rejected");
    BufferRam ram(kCharacter, kSize);
    equip(ram, 5u, 1u);
    check(weapon::read(ram).has_value(), "an exactly mapped record is accepted");
    ram.store16(kCharacter, 0u);
    ram.store16(kCharacter + 2u, 0xFF34u);
    check(!character_loaded(ram) && !weapon::read(ram), "empty name invalidates leftover equipment bytes");
    ram.store16(kCharacter, 0x4E00u);
    check(
        character_loaded(ram) && weapon::read(ram).has_value(), "non-ASCII hunter names also mark a loaded character");
}

void test_polling() {
    BufferRam ram(kCharacter, kSize);
    check(!weapon::current(), "snapshot is empty before any poll");
    check(!weapon::refresh(ram) && !weapon::current(), "empty polls do not invent a default weapon");
    equip(ram, 5u, 1u);
    check(weapon::refresh(ram), "loading equipment changes availability");
    const weapon::Equipped first{weapon::Type::GreatSword, 1u};
    check(weapon::current() == first, "consumers receive actual equipped type and ID");
    check(!weapon::refresh(ram), "unchanged equipment does not signal a change");
    const auto retained = weapon::current();
    equip(ram, 5u, 2u);
    check(weapon::current() == first, "memory changes become visible only at the next poll");
    check(weapon::refresh(ram) && weapon::current() == weapon::Equipped{weapon::Type::GreatSword, 2u},
        "a different weapon in the same class changes identity");
    check(retained == first, "consumer snapshots are values, not mutable aliases");
    equip(ram, 12u, 2u);
    check(weapon::refresh(ram) && weapon::current() == weapon::Equipped{weapon::Type::LongSword, 2u},
        "class changes are detected even if the numeric ID stays the same");
    // Decoration/upgrade data, hunter appearance and worn armor are not a
    // replacement for the actual weapon identity.
    ram.store32(kRecord + 4u, 0x12345678u);
    ram.store32(kRecord + 8u, 0xABCDEF12u);
    ram.store8(kCharacter + kCharacterSex, 1u);
    check(!weapon::refresh(ram), "unrelated appearance and decoration fields do not change identity");
    ram.store8(kRecord + 1u, 10u);
    check(weapon::refresh(ram) && !weapon::current(), "invalid equipment clears the previous identity");
    check(!weapon::refresh(ram), "repeated unavailable polls stay unchanged");
    equip(ram, 15u, 3u);
    check(weapon::refresh(ram), "valid equipment is reacquired after unavailable state");
    ram.store16(kCharacter, 0u);
    check(weapon::refresh(ram) && !weapon::current(), "character unload clears cached weapon");
    equip(ram, 9u, 4u);
    check(weapon::refresh(ram), "another loaded character supplies a new identity");
    BufferRam other(kCharacter, kSize);
    check(weapon::refresh(other) && !weapon::current(), "a fresh empty memory instance cannot inherit a cached weapon");
}

void test_read_only() {
    BufferRam ram(kCharacter, kSize);
    equip(ram, 14u, 19u);
    std::vector<std::uint8_t> before;
    for (std::uint32_t i = 0; i < kSize; ++i) before.push_back(ram.load8(kCharacter + i));
    for (unsigned i = 0; i < 1000u; ++i) {
        check(weapon::read(ram) == weapon::Equipped{weapon::Type::Gunlance, 19u}, "stable read across repeated polls");
        weapon::refresh(ram);
    }
    for (std::uint32_t i = 0; i < kSize; ++i)
        check(before[i] == ram.load8(kCharacter + i), "reading and polling never alter guest memory");
    BufferRam empty(kCharacter, kSize);
    weapon::refresh(empty);
}
void test_posture() {
    using P = weapon::Posture;
    BufferRam ram(0x08800000u, 0x03800000u);
    constexpr auto table = 0x09000000u, actor = 0x09100000u, other = 0x09200000u, vt = 0x09300000u;
    equip(ram, 12u, 1u);
    ram.store32(0x0A05E600u, 0x336F574Du);
    ram.store32(0x0A05E608u, 0x0A05E600u);
    const char name[] = "game_task.ovl";
    for (unsigned i = 0; i < sizeof(name); ++i) ram.store8(0x0A05E620u + i, name[i]);
    ram.store32(0x088B6B68u, 0x8C420008u);
    ram.store32(0x088E49C8u, 0x30420008u);
    ram.store32(0x09FBE794u, table);
    ram.store32(table + 8u, actor);
    ram.store32(actor, vt);
    ram.store32(vt + 0xA8u, 0x088A58DCu);
    ram.store32(actor + 0x27Cu, 1u);
    check(weapon::read_posture(ram) == P::Sheathed, "local quest hunter starts sheathed");
    ram.store32(actor + 0x27Cu, 9u);
    check(weapon::read_posture(ram) == P::Drawn, "draw flag is read independently of animation counters");
    ram.store32(actor + 0x27Cu, 0xFFFFFFF7u);
    check(weapon::read_posture(ram) == P::Sheathed, "unrelated actor flags do not imply drawn weapon");
    weapon::refresh(ram);
    check(weapon::current_posture() == P::Sheathed, "posture is published at a flip");
    ram.store16(actor + 0x60u, 1u);
    check(weapon::read_posture(ram) == P::Unknown, "remote hunter is not the local posture");
    ram.store16(actor + 0x60u, 0u);
    ram.store32(table + 12u, other);
    ram.store32(other, vt);
    check(weapon::read_posture(ram) == P::Unknown, "ambiguous local actors fail closed");
    ram.store16(other + 0x60u, 1u);
    check(weapon::read_posture(ram) == P::Sheathed, "other players do not hide the local actor");
    ram.store32(table + 8u, 0u);
    ram.store32(table + 28u, actor);
    check(weapon::read_posture(ram) == P::Sheathed, "local hunter may occupy the last actor slot");
    ram.store32(vt + 0xA8u, 0u);
    check(weapon::read_posture(ram) == P::Unknown, "preview or stale vtable is rejected");
    ram.store32(vt + 0xA8u, 0x088A58DCu);
    for (auto bad : {0u, 0xFFFFFFFFu, actor + 1u, 0x0BFFFFFCu}) {
        ram.store32(table + 28u, bad);
        check(weapon::read_posture(ram) == P::Unknown, "invalid actor cannot be dereferenced");
        ram.store32(table + 28u, actor);
        ram.store32(actor, bad);
        check(weapon::read_posture(ram) == P::Unknown, "invalid vtable cannot wrap into memory");
        ram.store32(actor, vt);
        ram.store32(0x09FBE794u, bad);
        check(weapon::read_posture(ram) == P::Unknown, "invalid actor table is rejected");
        ram.store32(0x09FBE794u, table);
    }
    ram.store32(0x088E49C8u, 0u);
    check(weapon::read_posture(ram) == P::Unknown, "unexpected executable has no posture");
    ram.store32(0x088E49C8u, 0x30420008u);
    ram.store32(0x0A05E600u, 0u);
    weapon::refresh(ram);
    check(weapon::current_posture() == P::Unknown, "scene unload clears old posture despite retained actor memory");
    BufferRam empty(0u, 0u);
    check(weapon::read_posture(empty) == P::Unknown, "unmapped memory has unknown posture");
    weapon::refresh(empty);
}

void test_actions() {
    BufferRam ram(0x08800000u, 0x03800000u);
    constexpr auto manager = 0x09000000u, actor = 0x09100000u, vt = 0x09300000u, table = 0x09400000u;
    equip(ram, 12u, 1u);
    ram.store32(0x0A05E600u, 0x336F574Du);
    ram.store32(0x0A05E608u, 0x0A05E600u);
    const char name[] = "game_task.ovl";
    for (unsigned i = 0; i < sizeof(name); ++i) ram.store8(0x0A05E620u + i, name[i]);
    ram.store32(0x088B6B68u, 0x8C420008u);
    ram.store32(0x088E49C8u, 0x30420008u);
    ram.store32(0x09FBE794u, manager);
    ram.store32(0x09FF53B0u, 0x09500000u);
    ram.store32(manager + 8u, actor);
    ram.store32(actor, vt);
    ram.store32(vt + 0xA8u, 0x088A58DCu);
    ram.store32(vt + 0x44u, 0x088A687Cu);
    ram.store32(actor + 0x27Cu, 9u);
    ram.store8(actor + 0x62u, 7u);
    ram.store32(0x0A10A3CCu, 0x8C8214A8u);
    ram.store32(0x0A10790Cu, 0x8C620304u);
    ram.store32(0x088A687Cu, 0x24820F80u);
    const auto real = [&](unsigned offset, float value) {
        ram.store32(actor + offset, std::bit_cast<std::uint32_t>(value));
    };
    real(0xF84u, 0.f);
    real(0xF8Cu, 2.f);
    real(0xF94u, 100.f);
    ram.store32(actor + 0x14A8u, table);
    const auto row = [&](unsigned index, unsigned request, int first = 0, int last = 0, int buffer = 0) {
        const auto address = table + index * 22u;
        ram.store16(address, 4u);
        ram.store16(address + 2u, 0x71u); // Arbitrary synthetic destination, not a copied game table.
        ram.store16(address + 6u, 25u);
        ram.store16(address + 8u, static_cast<std::uint16_t>(request));
        ram.store16(address + 16u, static_cast<std::uint16_t>(first));
        ram.store16(address + 18u, static_cast<std::uint16_t>(last));
        ram.store16(address + 20u, static_cast<std::uint16_t>(buffer));
        ram.store16(address + 22u, 0xFFu);
    };
    using A = weapon::LongSwordActions;
    row(0, 0);
    ram.store8(actor + 0x220u, 8u);
    check(weapon::read_actions(ram) == A{}, "evade/draw transitions suppress stale idle-table arrows");
    ram.store8(actor + 0x220u, 0x42u);
    check(!weapon::read_actions(ram), "unobserved actor action class is unknown");
    ram.store8(actor + 0x220u, 9u);
    check(weapon::read_actions(ram)->strike, "observed attacking class can expose its transition table");
    ram.store8(actor + 0x220u, 0u);
    row(1, 2);
    row(2, 1);
    check(weapon::read_actions(ram) == A{true, true, true, false, false}, "idle routes omit side fades");
    row(3, 17, 40, 70, 10);
    row(4, 18, 40, 70, 10);
    real(0xF84u, 27.f);
    check(
        weapon::read_actions(ram) == A{true, true, true, false, false}, "side routes stay closed before buffer window");
    real(0xF84u, 28.f);
    check(weapon::read_actions(ram) == A{true, true, true, true, true},
        "predicted pad frame reaches inclusive buffer start");
    real(0xF84u, 68.f);
    check(weapon::read_actions(ram)->left, "inclusive window end accepts one more pad sample");
    real(0xF84u, 69.f);
    check(!weapon::read_actions(ram)->left, "next pad sample past window end is unavailable");
    ram.store16(actor + 0xFD0u, 2u);
    check(weapon::read_actions(ram)->left, "stopped animation is tested at its current frame");
    ram.store16(actor + 0xFD0u, 0u);
    ram.store16(actor + 0x14ACu, 60u);
    ram.store16(actor + 0x14B0u, 90u);
    ram.store16(actor + 0x14AEu, 4u);
    real(0xF84u, 53.f);
    check(!weapon::read_actions(ram)->left, "actor timing overrides replace table timing");
    real(0xF84u, 54.f);
    check(weapon::read_actions(ram)->left, "actor buffer override applies at the exact boundary");
    for (auto offset : {0x14ACu, 0x14B0u, 0x14AEu}) ram.store16(actor + offset, 0u);
    row(3, 17, -20, -2, 0);
    row(4, 18, -20, -2, 0);
    real(0xF84u, 78.f);
    check(weapon::read_actions(ram)->left, "negative timing is relative to animation duration");
    real(0xF84u, 97.f);
    check(!weapon::read_actions(ram)->left, "relative animation end closes the window");
    row(0, 3);
    row(1, 5);
    row(2, 4);
    row(3, 17);
    row(4, 18);
    check(
        weapon::read_actions(ram) == A{true, true, true, true, true}, "combo variants map to the same logical choices");
    row(0, 14);
    ram.store16(table + 22u, 0xFFu);
    check(weapon::read_actions(ram) == A{false, true, false, false, false},
        "side-fade recovery can offer only a Circle follow-up");
    for (auto unrelated : {7u, 12u, 16u, 31u, 32u, 65535u}) {
        row(0, unrelated);
        check(weapon::read_actions(ram) == A{}, "shared/utility/invalid requests do not advertise attacks");
    }
    row(0, 0);
    ram.store16(table + 6u, 29u);
    check(weapon::read_actions(ram) == A{}, "conditional Spirit routes do not replace primary attack availability");
    row(0, 0);
    ram.store16(table + 10u, 1u);
    check(!weapon::read_actions(ram), "unverified extra route condition uses unknown fallback");
    ram.store16(table + 10u, 0u);
    row(0, 0, 999);
    check(!weapon::read_actions(ram), "unsupported timing sentinel is unknown rather than blocked");
    row(0, 0, 20, 10);
    check(!weapon::read_actions(ram), "inverted window is rejected");
    row(0, 0);
    for (float bad : {-1.f, 101.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        real(0xF84u, bad);
        check(!weapon::read_actions(ram), "invalid animation progress is rejected");
    }
    real(0xF84u, 0.f);
    real(0xF8Cu, -1.f);
    check(!weapon::read_actions(ram), "backwards animation speed is unknown");
    real(0xF8Cu, 2.f);
    real(0xF94u, 0.f);
    check(!weapon::read_actions(ram), "missing animation duration is unknown");
    real(0xF94u, 100.f);
    for (auto bad : {table + 1u, 0x0BFFFFFEu, 0xFFFFFFFEu}) {
        ram.store32(actor + 0x14A8u, bad);
        check(!weapon::read_actions(ram), "malformed/truncated transition pointer is unknown");
    }
    ram.store32(actor + 0x14A8u, 0u);
    check(weapon::read_actions(ram) == A{}, "valid actor with no transition table is busy");
    ram.store32(actor + 0x14A8u, table);
    ram.store8(actor + 0x1475u, 1u);
    check(weapon::read_actions(ram) == A{}, "suspended table has no usable main action");
    ram.store8(actor + 0x1475u, 0u);
    for (unsigned i = 0; i < 48; ++i) row(i, 0);
    check(!weapon::read_actions(ram), "unterminated table cannot partially disable controls");
    row(0, 0);
    weapon::refresh(ram);
    check(weapon::current_actions() == A{true, false, false, false, false},
        "availability snapshot is published each flip");
    ram.store32(actor + 0x27Cu, 1u);
    weapon::refresh(ram);
    check(!weapon::current_actions(), "sheathing clears previous combo availability");
    ram.store32(actor + 0x27Cu, 9u);
    ram.store8(actor + 0x62u, 0u);
    check(!weapon::read_actions(ram), "actor class must agree with equipped Long Sword");
    ram.store8(actor + 0x62u, 7u);
    ram.store32(vt + 0x44u, 0u);
    check(!weapon::read_actions(ram), "unknown animation accessor cannot be interpreted as the traced layout");
    ram.store32(vt + 0x44u, 0x088A687Cu);
    ram.store32(0x0A10790Cu, 0u);
    check(!weapon::read_actions(ram), "unmatched game code cannot supply availability");
    ram.store32(0x0A10790Cu, 0x8C620304u);
    equip(ram, 5u, 1u);
    check(!weapon::read_actions(ram), "unsupported weapon keeps its ordinary gesture behavior");
    ram.store32(0x09FF53B0u, 0u);
    check(!weapon::read_actions(ram), "unavailable menu state cannot certify gameplay input");
    BufferRam empty(0u, 0u);
    weapon::refresh(empty);
    check(!weapon::current_actions(), "unload removes the cached action snapshot");
}

} // namespace

int main() {
    test_types();
    test_ranges_and_character();
    test_polling();
    test_read_only();
    test_posture();
    test_actions();
    if (failures != 0)
        std::cerr << failures << " weapon state failures\n";
    else
        std::cout << "Weapon state contracts passed\n";
    return failures == 0 ? 0 : 1;
}
