// Entirely synthetic code signatures, actor records and camera matrices.
#include "game/damage_numbers.hpp"
#include "combat_fixture.hpp"
#include "game/damage_hook.hpp"
#include "game/monster_state.hpp"
#include "game/weapon_state.hpp"
#include "game/game_data.hpp"
#include "game/equipment_models.hpp"
#include "game/guest_ram.hpp"
#include "game/control_context.hpp"
#include "settings/settings.hpp"
#include "psprecomp/runtime.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <limits>

namespace {
using namespace mhp3rd::game;
namespace combat = mhp3rd::game::combat;
using namespace mhp3rd::test::combat_fixture;

int failures{};
void check(bool yes, const char *message) {
    if (!yes) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

combat::HitInput input() {
    return {target, hunter, collision, combat::kDamageReturn, 4u};
}
void test_hits() {
    BufferRam ram(0x08800000u, 0x02000000u);
    setup(ram);
    const auto valid = input();
    check(weapon::hunter_actor(ram) == hunter, "local actor uses the existing camera actor/vtable validation");
    ram.store16(target + 0x1F4u, 65530u);
    const auto hit = combat::begin_hit(ram, valid);
    check(hit && hit->counter == target + 0x1F4u && hit->before == 65530u && hit->position[1] == 85.f,
        "one local per-part hit captured");
    if (!hit) return;
    ram.store16(hit->counter, 5u);
    check(combat::finish_hit(ram, *hit) == 11, "actual accumulated damage handles u16 wrap");
    auto reset = *hit;
    reset.before = 100u;
    ram.store16(reset.counter, 0u);
    check(combat::finish_hit(ram, reset) == 0, "counter reset is not a hit");
    reset.before = 0u;
    ram.store16(reset.counter, 32767u);
    check(combat::finish_hit(ram, reset) == 32767, "largest supported signed damage accepted");
    ram.store16(reset.counter, 32768u);
    check(combat::finish_hit(ram, reset) == 0, "ambiguous large reset rejected");
    reset.counter = 0xFFFFFFFEu;
    check(combat::finish_hit(ram, reset) == 0, "unmapped counter not read");
    for (auto changed : {0u, 0x09745780u, 0x0A29AB80u}) {
        auto other = valid;
        other.source = changed;
        check(!combat::begin_hit(ram, other), "companions, other hunters, monsters and missing owners rejected");
    }
    auto other = valid;
    other.return_pc = 0u;
    check(!combat::begin_hit(ram, other), "unverified call site rejected");
    other = valid;
    other.part = 8u;
    check(!combat::begin_hit(ram, other), "part array boundary checked");
    other = valid;
    other.collision = 0xFFFFFFF0u;
    check(!combat::begin_hit(ram, other), "invalid collision record rejected");
    ram.store32(collision + 0x14u, hunter + 4u);
    check(!combat::begin_hit(ram, valid), "saved source register must match collision record owner");
    ram.store32(collision + 0x14u, hunter);
    ram.store32(0x0A1B0AE0u, 0u);
    check(!combat::begin_hit(ram, valid), "unregistered target rejected");
    ram.store32(0x0A1B0AE0u, target);
    ram.store32(target + 4u, 4u);
    check(!combat::begin_hit(ram, valid), "other-area target rejected");
    ram.store32(target + 4u, 0u);
    ram.store16(target + 0x246u, 0u);
    check(!combat::begin_hit(ram, valid), "dead target rejected");
    ram.store16(target + 0x246u, 700u);
    ram.store32(0x09103004u, 1u);
    check(!combat::begin_hit(ram, valid), "interface states suppress new numbers");
    ram.store32(0x09103004u, 0u);
    ram.store32(vt + 0xA8u, 0u);
    check(!combat::begin_hit(ram, valid), "missing local actor never falls back to a guessed source");
    ram.store32(vt + 0xA8u, 0x088A58DCu);
    ram.store32(combat::kApplyDamage, 0u);
    check(!combat::damage_code_supported(ram) && !combat::begin_hit(ram, valid),
        "changed executable disables interception");
    BufferRam empty(0x08800000u, 4u);
    check(!combat::damage_code_supported(empty), "missing executable is unavailable");
}
void test_history() {
    combat::DamageHistory history;
    combat::HitProbe hit{target, 0u, 0u, {1.f, 2.f, 3.f}};
    history.add(hit, 12, 100u);
    history.add(hit, 13, 101u);
    check(history.numbers().size() == 2 && history.numbers()[0].amount == 12 && history.numbers()[1].lane == 1,
        "successive hits remain distinct");
    history.add(hit, 0, 101u);
    history.add(hit, -1, 101u);
    history.add(hit, 32768, 101u);
    auto invalid = hit;
    invalid.target = 0u;
    history.add(invalid, 1, 101u);
    invalid = hit;
    invalid.position[0] = std::numeric_limits<float>::quiet_NaN();
    history.add(invalid, 1, 101u);
    check(history.numbers().size() == 2, "invalid hits do not consume an animation slot");
    history.expire(100u + combat::kNumberLifetimeUs);
    check(history.numbers().size() == 1, "lifetime boundary removes only expired hits");
    history.expire(0u);
    check(history.numbers().empty(), "clock reset cannot leave stale numbers");
    for (unsigned i = 0; i < 60; ++i) history.add(hit, static_cast<int>(i + 1), 1000u + i);
    check(history.numbers().size() == 48 && history.numbers().front().amount == 13 &&
            history.numbers().back().amount == 60,
        "rapid hits have a bounded queue");
    history.clear();
    history.add(hit, 1, 2000u);
    check(history.numbers().size() == 1 && history.numbers()[0].lane == 0,
        "scene reset clears all state including placement");
    check(combat::damage_clock_us() > 0u, "production animation clock is available");
}
void test_projection() {
    BufferRam ram(0x08800000u, 0x02000000u);
    constexpr std::uint32_t camera = 0x09200000u;
    check(!combat::project_damage(ram, {0.f, 0.f, 0.f}), "missing camera rejected");
    ram.store32(0x08A2F958u, camera);
    for (unsigned i = 0; i < 4; ++i) {
        ram.store32(camera + 0xF50u + i * 20u, std::bit_cast<std::uint32_t>(1.f));
        ram.store32(camera + 0xFD0u + i * 20u, std::bit_cast<std::uint32_t>(i == 3 ? 2.f : 1.f));
    }
    check(combat::project_damage(ram, {0.f, 0.f, 0.f}) == std::array<float, 2>{0.5f, 0.5f},
        "world origin projected to picture centre");
    check(combat::project_damage(ram, {2.f, 0.f, 0.f}) == std::array<float, 2>{1.f, 0.5f}, "screen edge retained");
    check(!combat::project_damage(ram, {3.f, 0.f, 0.f}) && !combat::project_damage(ram, {0.f, -3.f, 0.f}),
        "off-screen numbers omitted instead of edge clamping");
    check(!combat::project_damage(ram, {std::numeric_limits<float>::infinity(), 0.f, 0.f}), "invalid point rejected");
    ram.store32(camera + 0x100Cu, std::bit_cast<std::uint32_t>(-2.f));
    check(!combat::project_damage(ram, {0.f, 0.f, 0.f}), "behind-camera number omitted");
    ram.store32(0x08A2F958u, 0x0BFFFFF0u);
    check(!combat::project_damage(ram, {0.f, 0.f, 0.f}), "unmapped matrices not read");
}
unsigned original_calls{};
void original(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ++original_calls;
    const auto at = ctx.gpr[4] + 0x1ECu + (ctx.gpr[5] & 0xFFu) * 2u;
    runtime.memory().store16(at, static_cast<std::uint16_t>(runtime.memory().load16(at) + 15u));
    ctx.gpr[2] = 0x12345678u;
    ctx.pc = ctx.gpr[31];
}
void test_wrapper() {
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    GuestRam ram(runtime.memory());
    setup(ram);
    control_context::refresh(ram);
    auto &settings = mhp3rd::settings::current();
    const auto saved = settings;
    settings.damage_numbers = true;
    {
        psprecomp::FunctionRegistrationScope observe(runtime, &combat::observe_damage_registration);
        runtime.register_function(combat::kApplyDamage, &original, "recomp_unit_fixture");
        runtime.register_function(combat::kApplyDamage + 4u, &original, "irrelevant");
    }
    combat::damage_frame(runtime);
    psprecomp::AllegrexContext ctx;
    const auto prepare = [&] {
        ctx.pc = combat::kApplyDamage;
        ctx.gpr[4] = target;
        ctx.gpr[5] = 4u;
        ctx.gpr[18] = collision;
        ctx.gpr[20] = hunter;
        ctx.gpr[31] = combat::kDamageReturn;
    };
    prepare();
    check(runtime.invoke_isolated_aot(combat::kApplyDamage, ctx), "wrapped function stays callable");
    check(original_calls == 1 && ctx.gpr[2] == 0x12345678u && ram.load16(target + 0x1F4u) == 15u,
        "original damage executes exactly once and preserves its return state");
    check(combat::damage_history().numbers().size() == 1 && combat::damage_history().numbers()[0].amount == 15,
        "wrapper emits actual per-hit damage");
    prepare();
    ctx.gpr[20] = 0x09745780u;
    check(runtime.invoke_isolated_aot(combat::kApplyDamage, ctx),
        "original damage remains callable across source and option changes");
    check(original_calls == 2 && combat::damage_history().numbers().size() == 1,
        "companion hit keeps original gameplay without a local number");
    settings.damage_numbers = false;
    combat::damage_frame(runtime);
    prepare();
    check(runtime.invoke_isolated_aot(combat::kApplyDamage, ctx),
        "original damage remains callable across source and option changes");
    check(original_calls == 3 && combat::damage_history().numbers().empty(),
        "off switch restores original dispatch and clears animations");
    settings.damage_numbers = true;
    combat::damage_frame(runtime);
    prepare();
    check(runtime.invoke_isolated_aot(combat::kApplyDamage, ctx),
        "original damage remains callable across source and option changes");
    check(combat::damage_history().numbers().size() == 1, "setting can be enabled again in the same quest");
    ram.store32(0x09103004u, 1u);
    control_context::refresh(ram);
    combat::damage_frame(runtime);
    check(combat::damage_history().numbers().empty(), "native interface clears quest animations");
    ram.store32(combat::kApplyDamage, 0u);
    combat::damage_frame(runtime);
    settings = saved;
}
}
int main() {
    test_hits();
    test_history();
    test_projection();
    test_wrapper();
    return failures ? 1 : 0;
}
