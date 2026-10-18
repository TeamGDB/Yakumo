// Independent synthetic guest structures. No game archive, model or save is
// needed to assert the setup's state transitions and mutation boundaries.
#include "debug/encounter.hpp"
#include "debug/game_state.hpp"
#include "game/guest_ram.hpp"

#include <bit>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace mhp3rd::debug;
using mhp3rd::game::BufferRam;
constexpr std::uint32_t q = 0x08A3A630u, character = 0x09F4F450u;
constexpr std::uint32_t map = 0x09F00000u, actor = 0x0A300000u, model = 0x09A00000u;
constexpr std::uint32_t hunter = 0x096498D0u, vt = 0x0A1BB800u, getter = 0x0A1BB100u;
constexpr std::uint32_t camera = 0x09B00000u;
int failures{};

void check(bool ok, const char *why) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: encounter: " << why << '\n';
    }
}

struct Fixture {
    BufferRam ram{0x08800000u, 0x02000000u};
    encounter::Controller control;
    quests::Quest quest{507u, 5u, 550u, 1000u, 90000u, "Synthetic hunt", {}, {}, {}};

    void overlay(std::uint32_t at, const char *name) {
        ram.store32(at, 0x336F574Du);
        ram.store32(at + 8u, at);
        ram.store32(at + 12u, 0x1000u);
        ram.store32(at + 16u, 0x1000u);
        for (unsigned i = 0; i < 32u; ++i) ram.store8(at + 32u + i, 0u);
        for (unsigned i = 0; name[i]; ++i) ram.store8(at + 32u + i, name[i]);
    }
    void position(std::uint32_t at, float x, float y, float z) {
        for (float value : {x, y, z}) {
            ram.store32(at, std::bit_cast<std::uint32_t>(value));
            at += 4u;
        }
    }
    Fixture() {
        ram.store32(encounter::kInitialAreaChoice, 0x27BDFFE0u);
        ram.store32(encounter::kInitialAreaChoice + 8u, 0x3C0208ABu);
        ram.store32(encounter::kInitialAreaChoice + 0x3Cu, 0x90C50718u);
        ram.store32(0x088BDF5Cu, 0x0E2338ECu);
        ram.store16(p3rd::kHunterName, 'A');
        ram.store32(quests::kCharacterPointer, character);
        ram.store32(quests::kNextScenePointer, 0x0A25DD2Cu);
        ram.store32(quests::kScenePointer, 0x08B20000u);
        ram.store32(quests::kStatePointer, 0u);
        ram.store32(0x09FBEBA0u, map);
        ram.store8(map + 0x9A30u, 4u);
        ram.store32(p3rd::kMoney, 10000u);
        overlay(p3rd::kTaskSlot, "lobby_task.ovl");
        overlay(quests::kMapSlot, "P_v00a00d.ovl");
    }
    void depart() {
        check(control.begin(ram, quest, 100u).find("preparing") != std::string::npos, "begin accepts a ready village");
        control.tick(ram, 101u, true);
        control.tick(ram, 2100u, true);
        check(control.phase() == encounter::Phase::Village, "stability interval has an exact lower boundary");
        control.tick(ram, 2101u, true);
        check(control.phase() == encounter::Phase::Departing, "stable village requests one real departure");
        check(ram.load32(p3rd::kMoney) == 9450u && ram.load16(character + quests::kQuestIdOffset) == 507u,
            "departure pays the original fee once and selects the original quest");
        overlay(p3rd::kTaskSlot, "game_task.ovl");
        ram.store32(q, 0x100u);
        ram.store16(q + 0x100u + 0x1Cu, 507u);
        ram.store8(q + 0x100u + 0x20u, 4u);
        ram.store32(q + 0x20u, 0x200u);
        ram.store32(q + 0x200u, 1u);
        ram.store32(q + 0x200u + 12u, 0x300u);
        ram.store16(q + 0x300u, 5u);
        ram.store8(q + 0x300u + 6u, 7u);
    }
    void initialize() {
        depart();
        check(!control.initial_choice(ram, 0u, true, true), "an unrelated guest return does not consume the request");
        check(
            control.initial_choice(ram, encounter::kInitialAreaReturn, true, true), "real initial choice selects area");
        check(ram.load8(map + 0x9A31u) == 7u && ram.load8(character + 0x718u) == 0u,
            "area selection does not use or persist gallery/character flags");
        overlay(quests::kMapSlot, "P_m04a07d.ovl");
        overlay(0x0A1BB000u, "em005m0.ovl");
        ram.store32(p3rd::kMonsterTable, actor);
        ram.store32(actor, vt);
        ram.store8(actor + 0x62u, 5u);
        ram.store8(actor + 0xD6u, 7u);
        ram.store32(actor + 4u, 3u);
        ram.store16(actor + 0x246u, 2000u);
        ram.store16(actor + 0x288u, 2500u);
        ram.store32(vt + 0x4Cu, getter);
        ram.store32(getter, 0x24820DD0u);
        ram.store32(getter + 4u, 0x03E00008u);
        ram.store32(actor + 0xDD4u, model);
        ram.store32(model, 0x006F6D70u);
        ram.store32(model + 4u, 0x00323031u);
        ram.store32(model + 8u, 64u);
        ram.store32(hunter, 0x0A1A5A90u);
        ram.store32(0x0A1A5A9Cu, 0x0A102464u);
        ram.store8(hunter + 0xD6u, 7u);
        ram.store16(hunter + 0x246u, 150u);
        position(actor + 0x80u, 1500, 500, 1100);
        position(hunter + 0x80u, 1150, 500, 3250);
        ram.store32(p3rd::kQuestTimeLeft, 89870u);
        ram.store32(0x08A2F958u, camera);
        for (auto i : {0u, 5u, 10u, 15u}) ram.store32(camera + 0xF50u + i * 4u, std::bit_cast<std::uint32_t>(1.0f));
        position(camera + 0xF50u + 48u, -1500, -600, -1800);
        ram.store32(camera + 0xFD0u, std::bit_cast<std::uint32_t>(1.0f));
        ram.store32(camera + 0xFD0u + 20u, std::bit_cast<std::uint32_t>(1.0f));
        ram.store32(camera + 0xFD0u + 44u, std::bit_cast<std::uint32_t>(-1.0f));
    }
    void relocate() {
        initialize();
        check(encounter::inspect(ram).valid(), "independent actor, overlay and model fixture is ready");
        control.tick(ram, 3000u, true);
        control.tick(ram, 3033u, true);
        control.tick(ram, 3066u, true);
        check(control.tick(ram, 3099u, true) == encounter::Action::Focus,
            "settled target relocates once and requests focus");
        check(control.phase() == encounter::Phase::Camera, "camera validation precedes readiness");
        for (const auto offset : {0x40u, 0x80u, 0x90u}) {
            check(ram.load32(hunter + offset) == std::bit_cast<std::uint32_t>(1500.0f) &&
                    ram.load32(hunter + offset + 4u) == std::bit_cast<std::uint32_t>(500.0f) &&
                    ram.load32(hunter + offset + 8u) == std::bit_cast<std::uint32_t>(1800.0f),
                "all hunter positions agree after warp");
        }
        check(ram.load16(actor + 0x246u) == 2000u && ram.load32(actor + 4u) == 3u,
            "warp preserves monster health, AI and flags");
    }
    void ready() {
        relocate();
        control.tick(ram, 3132u, true);
        check(control.phase() == encounter::Phase::Camera, "unchanged quest clock cannot satisfy camera readiness");
        ram.store32(p3rd::kQuestTimeLeft, 89869u);
        for (unsigned i = 0; i < 3u; ++i) control.tick(ram, 3165u + i * 33u, true);
        check(control.phase() == encounter::Phase::Ready, "visible target and advancing quest clock complete setup");
    }
};

void lifecycle() {
    Fixture f;
    check(f.control.status().find("\"phase\":\"idle\"") != std::string::npos, "idle has structured status");
    f.control.tick(f.ram, 0u, true);
    f.ready();
    check(f.control.begin(f.ram, f.quest, 4000u).find("already active") != std::string::npos,
        "active setup cannot be replaced");
    const auto status = f.control.status();
    check(status.find("\"population\":1") != std::string::npos && status.find("\"in_view\":true") != std::string::npos,
        "status records actual population and projected visibility");
    f.control.tick(f.ram, 100000u, true);
    check(f.control.phase() == encounter::Phase::Ready, "ready encounter has no hidden AI/time hold");
    f.ram.store16(actor + 0x246u, 0u);
    f.control.tick(f.ram, 100001u, true);
    check(
        f.control.phase() == encounter::Phase::Finished && f.control.status().find("\"actor\":0") != std::string::npos,
        "target death discards actor/model lifetime state");
    f.control.cancel();
    check(f.control.phase() == encounter::Phase::Idle, "cancel clears all pending temporary state");
}

void guards() {
    using Mutation = std::function<void(Fixture &)>;
    const std::vector<Mutation> initial = {[](auto &f) { f.ram.store32(encounter::kInitialAreaChoice, 0u); },
        [](auto &f) { f.overlay(p3rd::kTaskSlot, "lobby_task.ovl"); }, [](auto &f) { f.ram.store32(q, 0xffffffffu); },
        [](auto &f) { f.ram.store16(character + quests::kQuestIdOffset, 508u); },
        [](auto &f) { f.ram.store32(q + 0x20u, 0u); }, [](auto &f) { f.ram.store32(q + 0x200u, 2u); },
        [](auto &f) { f.ram.store32(q + 0x20u, 0x201u); },
        [](auto &f) { f.ram.store32(q + 0x200u + 12u, 0xffffffffu); }, [](auto &f) { f.ram.store16(q + 0x300u, 6u); },
        [](auto &f) { f.ram.store8(q + 0x300u + 6u, 8u); }, [](auto &f) { f.ram.store32(0x09FBEBA0u, 0u); },
        [](auto &f) { f.ram.store8(map + 0x9A31u, 1u); },
        [](auto &f) { f.ram.store32(p3rd::kMonsterTable + 19u * 4u, actor); }};
    for (const auto &mutate : initial) {
        Fixture f;
        f.depart();
        mutate(f);
        const auto old = f.ram.load8(map + 0x9A31u);
        check(!f.control.initial_choice(f.ram, encounter::kInitialAreaReturn, true, true),
            "invalid initial lifecycle is refused");
        check(f.control.phase() == encounter::Phase::Failed && f.ram.load8(map + 0x9A31u) == old,
            "guard failure does not write map or create actors");
    }
    for (bool resumed : {false, true}) {
        Fixture f;
        f.depart();
        check(!f.control.initial_choice(f.ram, encounter::kInitialAreaReturn, resumed, false),
            "network guard wins over native resumption");
    }
    {
        Fixture f;
        f.depart();
        check(!f.control.initial_choice(f.ram, encounter::kInitialAreaReturn, false, true),
            "changed guest ownership refuses relocation");
    }
    const std::vector<Mutation> actor_guards = {[](auto &f) { f.ram.store32(q, 3u); },
        [](auto &f) { f.ram.store8(q + 0x120u, 3u); }, [](auto &f) { f.overlay(quests::kMapSlot, "P_m04a00d.ovl"); },
        [](auto &f) { f.ram.store8(map + 0x9A31u, 0u); }, [](auto &f) { f.ram.store32(p3rd::kMonsterTable, 0u); },
        [](auto &f) { f.ram.store32(p3rd::kMonsterTable + 4u, actor); },
        [](auto &f) { f.ram.store32(p3rd::kMonsterTable + 76u, 0xffffffffu); },
        [](auto &f) { f.ram.store8(actor + 0x62u, 75u); }, [](auto &f) { f.ram.store16(actor + 0x288u, 0u); },
        [](auto &f) { f.ram.store8(actor + 0xD6u, 6u); }, [](auto &f) { f.ram.store32(actor + 4u, 7u); },
        [](auto &f) { f.overlay(0x0A1BB000u, "em006m0.ovl"); }, [](auto &f) { f.ram.store32(actor, 0x08800000u); },
        [](auto &f) { f.ram.store32(0x0A1BB00Cu, 0xffffff00u); }, [](auto &f) { f.ram.store32(vt + 0x4Cu, 0u); },
        [](auto &f) { f.ram.store32(getter, 0u); }, [](auto &f) { f.ram.store32(getter + 4u, 0u); },
        [](auto &f) { f.ram.store32(getter + 8u, 1u); }, [](auto &f) { f.ram.store32(actor + 0xDD4u, 0u); },
        [](auto &f) { f.ram.store32(model, 0u); }, [](auto &f) { f.ram.store32(model + 4u, 0u); },
        [](auto &f) { f.ram.store32(model + 8u, 63u); }, [](auto &f) { f.ram.store32(model + 8u, 0x800001u); },
        [](auto &f) { f.ram.store32(hunter, 0u); }, [](auto &f) { f.ram.store8(hunter + 0x62u, 1u); },
        [](auto &f) { f.ram.store8(hunter + 0xD6u, 1u); }, [](auto &f) { f.ram.store16(hunter + 0x246u, 0u); },
        [](auto &f) { f.position(actor + 0x80u, std::numeric_limits<float>::infinity(), 0, 0); },
        [](auto &f) { f.position(hunter + 0x80u, 0, 0, std::numeric_limits<float>::quiet_NaN()); },
        [](auto &f) { f.ram.store32(p3rd::kQuestTimeLeft, 90001u); },
        [](auto &f) { f.ram.store32(p3rd::kQuestTimeLeft, 0u); }};
    for (const auto &mutate : actor_guards) {
        Fixture f;
        f.initialize();
        mutate(f);
        check(!encounter::inspect(f.ram).valid(), "unknown actor/model/map cannot certify readiness");
    }
    {
        Fixture f;
        f.initialize();
        f.ram.store32(0x08A2F958u, 0u);
        check(!encounter::inspect(f.ram).in_view, "missing camera is not visible");
    }
    {
        Fixture f;
        f.initialize();
        f.ram.store32(camera + 0xFD0u + 44u, 0u);
        check(!encounter::inspect(f.ram).in_view, "zero projection is not visible");
    }
    BufferRam empty(0x08800000u, 64u);
    check(!encounter::inspect(empty).valid() && !encounter::supported(empty), "partial guest memory stays unknown");
}

void failures_and_reset() {
    for (unsigned phase = 0; phase < 5u; ++phase) {
        Fixture f;
        if (phase == 0u) f.control.begin(f.ram, f.quest, 100u);
        if (phase == 1u) f.depart();
        if (phase == 2u) f.initialize();
        if (phase == 3u) f.relocate();
        if (phase == 4u) f.ready();
        const auto position = f.ram.load32(hunter + 0x80u);
        f.control.tick(f.ram, 4000u, false);
        check(f.control.phase() == encounter::Phase::Failed && f.ram.load32(hunter + 0x80u) == position,
            "enabled/session/join/host guard closure aborts every active phase without mutation");
    }
    {
        Fixture f;
        f.quest.id = 508;
        check(f.control.begin(f.ram, f.quest, 1u).find("refused") != std::string::npos,
            "unknown preset quest is refused");
    }
    {
        Fixture f;
        f.ram.store32(0x0A25DD2Cu, 12u);
        check(f.control.begin(f.ram, f.quest, 1u).find("refused") != std::string::npos,
            "begin refuses an open game dialog");
    }
    {
        Fixture f;
        f.control.begin(f.ram, f.quest, 100u);
        f.control.tick(f.ram, 90101u, true);
        check(f.control.phase() == encounter::Phase::Failed, "overall preparation deadline is finite");
        f.control.tick(f.ram, 999999u, true);
    }
    {
        Fixture f;
        f.ram.store32(p3rd::kMoney, 0u);
        f.control.begin(f.ram, f.quest, 100u);
        f.control.tick(f.ram, 101u, true);
        f.control.tick(f.ram, 2101u, true);
        check(f.control.phase() == encounter::Phase::Failed, "departure failure is retained");
    }
    {
        Fixture f;
        f.control.begin(f.ram, f.quest, 100u);
        f.control.tick(f.ram, 101u, true);
        f.ram.store32(0x0A25DD2Cu, 12u);
        f.control.tick(f.ram, 2000u, true);
        f.ram.store32(0x0A25DD2Cu, 0u);
        f.control.tick(f.ram, 2101u, true);
        f.control.tick(f.ram, 4000u, true);
        check(f.control.phase() == encounter::Phase::Village, "readiness must be continuously stable");
    }
    {
        Fixture f;
        f.initialize();
        f.position(actor + 0x80u, 1500, 10000, 1100);
        f.control.tick(f.ram, 3000u, true);
        f.control.tick(f.ram, 50000u, true);
        check(
            f.control.status().find("timed out") != std::string::npos, "not-landed timeout explains missing readiness");
    }
    {
        Fixture f;
        f.initialize();
        f.position(actor + 0x80u, 0, 500, 0);
        for (unsigned i = 0; i < 5u; ++i) f.control.tick(f.ram, 3000u + i * 33u, true);
        check(f.control.phase() == encounter::Phase::Failed, "unsupported terrain/landing is never guessed");
    }
    {
        Fixture f;
        f.relocate();
        f.ram.store32(0x08A2F958u, 0u);
        f.control.tick(f.ram, 4000u, true);
        f.control.tick(f.ram, 14000u, true);
        check(f.control.phase() == encounter::Phase::Failed, "camera wait has a deadline");
    }
    {
        Fixture f;
        f.ready();
        f.overlay(p3rd::kTaskSlot, "lobby_task.ovl");
        f.control.tick(f.ram, 4000u, true);
        check(f.control.phase() == encounter::Phase::Finished, "quest exit clears encounter lifetime");
    }
    {
        Fixture f;
        f.initialize();
        f.ram.store32(model, 0u);
        f.control.tick(f.ram, 3000u, true);
        check(f.control.phase() == encounter::Phase::Loading, "unloaded model cannot trigger teleport");
        f.control.cancel();
        f.control.tick(f.ram, 4000u, true);
        check(f.control.phase() == encounter::Phase::Idle, "cancelled preparation cannot restart itself");
    }
}
} // namespace

int encounter_tests() {
    lifecycle();
    guards();
    failures_and_reset();
    return failures;
}
