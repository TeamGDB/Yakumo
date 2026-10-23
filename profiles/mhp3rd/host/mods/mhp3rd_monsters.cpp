#include "mods/mhp3rd_monsters.hpp"

#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "mods/mhp3rd_tables.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_native.hpp"

#include "game/pack_file.hpp"
#include "mhp3rd_profile.hpp"
#include "overlays.hpp"

#include "kernel/kernel.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "yakumo/mod.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <iostream>
#include <map>
#include <numbers>

namespace mhp3rd::mods::monsters {
namespace {

using psprecomp::Runtime;

constexpr GameKind kGameKinds[] = {
    {"rathian", 1},
    {"rathalos", 2},
    {"qurupeco", 3},
    {"gigginox", 4},
    {"barioth", 5},
    {"diablos", 6},
    {"deviljho", 7},
    {"barroth", 8},
    {"uragaan", 9},
    {"jaggi", 10},
    {"jaggia", 11},
    {"great_jaggi", 12},
    {"baggi", 13},
    {"great_baggi", 14},
    {"gold_rathian", 15},
    {"royal_ludroth", 16},
    {"ludroth", 17},
    {"silver_rathalos", 18},
    {"agnaktor", 19},
    {"black_diablos", 20},
    {"uroktor", 21},
    {"delex", 22},
    {"brute_tigrex", 23},
    {"alatreon", 24},
    {"jhen_mohran", 25},
    {"giggi", 26},
    {"aptonoth", 27},
    {"popo", 28},
    {"rhenoplos", 29},
    {"felyne", 30},
    {"melynx", 31},
    {"green_nargacuga", 32},
    {"altaroth", 33},
    {"kelbi", 34},
    {"gigginox_egg", 35},
    {"bnahabra", 36},
    {"bnahabra_plains", 37},
    {"bnahabra_tundra", 38},
    {"bnahabra_volcano", 39},
    {"zinogre", 40},
    {"amatsu_magatsuchi", 41},
    {"arzuros", 42},
    {"lagombi", 43},
    {"volvidon", 44},
    {"great_wroggi", 45},
    {"duramboros", 46},
    {"nibelsnarf", 47},
    {"wroggi", 48},
    {"slagtoth", 49},
    {"gargwa", 50},
    {"crimson_qurupeco", 51},
    {"baleful_gigginox", 52},
    {"sand_barioth", 53},
    {"jade_barroth", 54},
    {"steel_uragaan", 55},
    {"purple_ludroth", 56},
    {"glacial_agnaktor", 57},
    {"tigrex", 58},
    {"nargacuga", 59},
    {"akantor", 60},
    {"ukanlos", 61},
    {"bulldrome", 62},
    {"bullfango", 63},
    {"anteka", 64},
    {"cart_felyne", 65},
    {"nyanjiro", 66},
    {"veggie_elder", 67},
    {"slagtoth_2", 68},
    {"rock", 69},
    {"rock_plains", 70},
    {"rock_forest", 71},
    {"rock_tundra", 72},
    {"rock_volcano", 73},
    {"rock_peaks", 74},
};
constexpr std::uint8_t kGameKindCount = 75u;

// What a base's behaviour does that a kind made from it needs handled. Traced
// for the Tigrex's AI, which serves kinds 23 and 58 (docs/MONSTERS.md, "AI
// overlays that know their kinds" and "Drawing"). An AI comes linked once for
// each monster slot it can load in: em058m0 in the first, em058m1 in the
// second (a Tigrex that is a quest's second large monster), the same code
// 0x34E80 further on. Its kind checks lie at the same offsets from each
// overlay's load address.
struct Behaviour {
    std::uint8_t kind{};
    std::string_view ai;                        // its overlays' names before the slot: em058 for em058m0
    std::uint32_t slots{};                      // the monster slots it has an overlay for: m0 to m<slots - 1>
    std::array<std::uint32_t, 4> kind_checks{}; // from the overlay's load address: compare the kind (in v1) with 58
    std::uint32_t meshes{};                     // meshes it writes to by number
    bool hides_materials{};
};
constexpr Behaviour kBehaviours[] = {
    {58u, "em058", 2u, {0x304u, 0x5318u, 0x5500u, 0x7848u}, 16u, true},
    {23u, "em058", 2u, {0x304u, 0x5318u, 0x5500u, 0x7848u}, 16u, true},
};
// The monster slots AI overlays load in (em*m0 to em*m3), among the overlay
// slots.
constexpr std::size_t kFirstMonsterSlot = 3u;
constexpr std::uint32_t kMonsterSlots = 4u;
static_assert(kOverlaySlots[kFirstMonsterSlot] == 0x0A1BB000u && kOverlaySlots[kFirstMonsterSlot + 1u] == 0x0A1EFE80u);

// Tables by kind in game_task, whose base row an added kind takes.
constexpr std::string_view kKindTables[] = {
    "monster.kinds", "monster.files", "monster.constructors", "monster.sizes", "monster.carves"};
// A float per kind in the executable, the size of its footsteps' dust.
constexpr std::string_view kStepScales = "monster.step_scales";
// Tables by model set (from 1) in the executable: its three files.
constexpr std::string_view kModelTables[] = {"monster.models", "monster.models.second", "monster.models.third"};

// Where monster.build_model calls model.compile (0x0A08FE8C): s2 is the
// material buffer, a2 the PMO, and the model set at sp + 100.
constexpr std::uint32_t kCompileCall = 0x0A08FE8Cu;
// model.draw_mesh reading a material's alpha (lbu v1, 3(t0)).
constexpr std::uint32_t kMaterialAlpha = 0x0889EEC4u;
constexpr std::uint32_t kMaterialAlphaWord = 0x91030003u;

// The task overlay's slot, where game_task loads for a quest.
constexpr std::uint32_t kTaskSlot = 0x0A05E600u;

// The parts of behaviour a mod can give a kind, and the vtable slot of each
// (docs/MONSTERS.md, "A monster's behaviour").
// Think: each frame, from the monster update; Act: then, from game_task's
// 0x0A086254, the action in the monster's mode (+0x18C) carried out (the
// common class's does nothing).
// Effects: slot 36, the sounds and effects of the motion playing, from the
// base's tables by motion slot (the Tigrex's 0x0A1C2810 runs 0x0A01476C over
// them, then effects at frames by motion).
constexpr std::array<std::uint32_t, 3> kPartSlots{32u, 33u, 36u};
constexpr std::size_t kThink = 0u, kAct = 1u, kEffects = 2u;
// game_task's large monster maker: make(manager a0, kind a1) -> the monster.
constexpr std::uint32_t kMakeMonster = 0x0A090AE0u;
// Slots copied into a kind's own vtable: more than the 70 a monster's class has.
constexpr std::uint32_t kVtableSlots = 96u;

struct Added;
struct Callback {
    PartFn fn{};
    void *user{};
    std::uint32_t base{}; // in a vtable's stub: the base's function for the part
    std::size_t part{};
    Added *kind{};
};

// game_task's motion setter: play(monster, motion as u16, blend frames,
// first frame), as the Tigrex's behaviour calls it at each change of action.
constexpr std::uint32_t kPlayMotion = 0x0A0B3004u;
constexpr std::uint32_t kPlayMotionWord = 0x30A5FFFFu; // andi a1, a1, 0xFFFF
// The model getter, vtable +68 (slot 17): a leaf adding the model's offset.
constexpr std::uint32_t kModelSlot = 17u;
// game_task's attacks: start(monster, slot, number in the kind's attack table,
// flags), as the Tigrex's behaviour starts its attacks at a motion's frame,
// and end(monster, slot), which ends the attack object in the slot.
constexpr std::uint32_t kStartAttack = 0x0A09E884u;
constexpr std::array<std::uint32_t, 2> kStartAttackWords{0x27BDFFE0u, 0xAFB60018u};
// game_task's target update: update(monster, point or 0), the point from the
// target chosen at +0x4D8.
constexpr std::uint32_t kAim = 0x0A0AFEB0u;
constexpr std::array<std::uint32_t, 2> kAimWords{0x27BDFF80u, 0xAFB10064u};
constexpr std::uint32_t kEndAttack = 0x0A09B47Cu;
// A monster's attack slots: the attack objects at +0x174 and +0x178. The
// game's own loops over them (the monster's reset at 0x0A0B4A04, an attack
// object leaving its slot at 0x0A133864) cover these two, and the Tigrex's AI
// uses slots 0 and 1; a slot past them is never cleared.
constexpr std::uint32_t kAttackSlots = 2u;
constexpr std::array<std::uint32_t, 2> kEndAttackWords{0x30A500FFu, 0x00052880u};
// game_sub's effect starter: start(bank, effect, monster, joint, t0 offset
// and t1 scale (vec4s), t2, t3, then two halfwords on the stack: flags (2) and
// 0), as the Tigrex starts its effects on its joints (logged: bank 12, effect
// 7, joint 26, offset (0, -10, 90), scale 0.7). The bank of a monster's own
// effects is 12 plus the slot its model set is loaded in; the effects are
// entries of its model pack.
constexpr std::uint32_t kStartEffect = 0x0A00D7D8u;
constexpr std::array<std::uint32_t, 2> kStartEffectWords{0x27BDFFC0u, 0x87A20044u};
constexpr std::uint32_t kEffectFlags = 2u;
constexpr std::uint32_t kModelSetsPointer = 0x08A38644u; // the model sets loaded: 4 of 12 bytes from +0x90
constexpr std::uint32_t kModelSetBank = 12u;
// The kind's data a monster keeps (docs/MONSTERS.md, "A monster's attacks"):
// a copy made with the mod's attack shapes takes the base's place.
constexpr std::uint32_t kKindData = 0xD50u; // cached by 0x0A09B644
constexpr std::uint32_t kKindDataSize = 0xA8u;
constexpr std::uint32_t kAttackTables = 0xA4u; // in the kind's data
constexpr std::uint32_t kAttackTablesSize = 0x70u;
constexpr std::uint32_t kShapeList = 0x5Cu; // in the attack tables: a pointer per shape
constexpr std::uint32_t kShapes = 32u;      // shape pointers copied
constexpr std::uint32_t kRowList = 0x60u;   // in the attack tables: the rows
constexpr std::uint32_t kRowSize = 28u;
constexpr std::uint32_t kRowShape = 0x11u;     // the shape a row hits with (u8)
constexpr std::uint32_t kSphereSize = 40u;     // joint (u16), kind (0: sphere), 0, radius, x, y, z, 0...
constexpr std::uint32_t kLastSphere = 0xFFFFu; // the joint that ends a shape
// The body capsules, where hunters' hits land: the kind's data points at the
// list twice; 44 bytes each (joint u16, joint u16, kind u16 (0 sphere, 2
// capsule), part u16, zone u32, 0, radius, from x/y/z, to x/y/z), ended by
// 0xFFFF in every u16 and the zone.
constexpr std::array<std::uint32_t, 2> kBodyLists{0x1Cu, 0x20u};
// game_sub's player of a monster's motion events (0x0A01476C): seven tables
// (three of effects by ground, footsteps, effects whatever the ground, a row per
// motion, a u16 per motion slot naming its row), the monster, and flags on the
// stack. A row (16 bytes): u16 starts in the five event tables, then a u8
// count for each from byte 10. A footstep (12 bytes): u32 motion slot, f32
// frame, u16 joint, u16 size.
constexpr std::uint32_t kMotionEvents = 0x0A01476Cu;
constexpr std::array<std::uint32_t, 2> kMotionEventsWords{0x27BDFFD0u, 0x93A30030u};
constexpr std::uint32_t kEventSlots = 1000u; // the player reads the index for slots below this
constexpr std::uint32_t kEventRow = 16u;
constexpr std::uint32_t kFootstepSize = 12u;
constexpr std::uint32_t kCapsuleSize = 44u;
// The monster's body (docs/MONSTERS.md, "A monster's body").
constexpr std::uint32_t kPosition = 0x80u; // x, y, z as floats
constexpr std::uint32_t kFacing = 0x74u;   // 65536 a turn, from +z towards +x
constexpr std::uint32_t kTarget = 0x4C0u;  // x, y, z the monster is after
constexpr std::uint32_t kHealth = 0x246u;  // s16, now
constexpr std::uint32_t kMostHealth = 0x288u;
constexpr std::uint32_t kBodySize = 0x4CCu;
constexpr float kTurn = 65536.0f;

// A guest function a part of behaviour asked for, run once its fn returns.
struct Action {
    std::uint32_t function{};
    std::array<std::uint32_t, 4> arguments{};
    std::array<std::uint32_t, 4> more{}; // t0 to t3, the fifth to eighth arguments
    std::vector<std::uint32_t> frame;    // words below the stack pointer for the call: its stack arguments first
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pointers; // register, offset into the frame it points at
};

// The actions a part queued, carried out one after another through the
// kernel's guest calls, then the base's part or the return to the game.
struct Chain {
    std::vector<Action> actions;
    std::size_t next{};
    std::array<std::uint32_t, 4> arguments{}; // the part's own, for the base's
    std::uint32_t base{};                     // 0: return to the game
};

struct Added {
    std::string id;
    const void *owner{};
    std::array<Callback, kPartSlots.size()> parts{}; // as behave() gave them
    // Its own copies of the base's vtable, made as its monsters are, by the
    // base vtable's address: the base's AI has a vtable in each monster slot
    // it loads in, with the functions of that slot's overlay.
    struct Vtable {
        std::uint32_t copy{};
        std::array<Callback, kPartSlots.size()> stubs{}; // the hooks' user data; base set for each stub made
        std::vector<ymod_hook> hooks;
    };
    std::map<std::uint32_t, Vtable> vtables;
    std::uint8_t kind{};
    std::uint8_t base{};
    const Behaviour *behaviour{};
    std::array<FileId, 3> files{};
    std::array<std::uint8_t, 2> check_kind{};               // user data for the kind check hooks: {kind, base}
    std::map<std::uint32_t, std::vector<HitSphere>> shapes; // by the base's shape number
    std::vector<BodyCapsule> body;                          // its own body capsules, or none for the base's
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> rows; // its own rows: row -> {like, shape}
    std::vector<Footstep> footsteps;                                       // by motion, in order
    struct Events {
        std::uint32_t index{}, rows{}, steps{}, none{}; // in guest memory, once made
    } events;
    std::map<std::uint32_t, std::uint32_t> kind_data; // the base's kind data -> the kind's copy
    std::vector<bool> effects;                        // which effect numbers its pack has
};

struct Shown {
    std::uint32_t buffer{};
    std::uint32_t materials{};
};

struct State {
    bool made_hooked{};
    bool in_part{};
    std::vector<Action> queued;
    std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader; // tests only
    std::vector<Added> kinds; // never reallocated while hooks point at them: reserved below
    Runtime *runtime{};
    ymod_mod *host{};
    bool draw_hooked{};
    bool watching_quest_end{};
    std::vector<Shown> shown;
    std::map<std::uint32_t, bool> thought_alone; // by monster: its last Think was the mod's alone
};

State &state() {
    static State s = [] {
        State fresh;
        fresh.kinds.reserve(256u - kGameKindCount);
        return fresh;
    }();
    return s;
}

const ymod_hooks *hooks() {
    State &s = state();
    if (s.host == nullptr) s.host = native::add_mod("yakumo.monsters", "Monsters from mods", "");
    return static_cast<const ymod_hooks *>(native::host()->service(s.host, YMOD_HOOKS, 1u));
}

// A problem with a kind, on the page of the mod that added it (and in the
// log); Yakumo's own when no mod is known.
void problem(const void *owner, const std::string &id, const std::string &line) {
    auto *mod = const_cast<ymod_mod *>(static_cast<const ymod_mod *>(owner));
    native::host()->log(mod, YMOD_LOG_WARNING, ("monster " + id + ": " + line).c_str());
}

// --- Packs and models ---

std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t at) {
    std::uint32_t v = 0u;
    if (at + 4u <= data.size()) std::memcpy(&v, data.data() + at, 4u);
    return v;
}

void put32(std::vector<std::uint8_t> &data, std::size_t at, std::uint32_t v) {
    std::memcpy(data.data() + at, &v, 4u);
}

using game::read_pack;
using game::write_pack;

// A PMO "102" with at least `meshes` meshes: empty ones (no materials, no
// vertex groups) are added after its own, and the sections behind the mesh
// table move down by as much (docs/MONSTERS.md, "Model").
std::vector<std::uint8_t> with_meshes(std::span<const std::uint8_t> pmo, std::uint32_t meshes) {
    std::vector<std::uint8_t> out(pmo.begin(), pmo.end());
    constexpr std::size_t kRecord = 0x30u;
    if (pmo.size() < 0x40u ||
        std::memcmp(pmo.data(),
            "pmo\0"
            "102\0",
            8u) != 0)
        return out;
    std::uint16_t have = 0u;
    std::memcpy(&have, pmo.data() + 0x1Cu, 2u);
    const std::uint32_t table = u32(pmo, 0x20u);
    if (have == 0u || have >= meshes || table + kRecord * have > pmo.size()) return out;
    const std::size_t grow = kRecord * (meshes - have);
    std::vector<std::uint8_t> empty(pmo.begin() + table, pmo.begin() + table + kRecord); // keeps the scale
    put32(empty, 0x20u, 0x80000003u);
    put32(empty, 0x24u, 0u);
    std::fill(empty.begin() + 0x28, empty.end(), std::uint8_t{0});
    const std::size_t end_of_table = table + kRecord * have;
    for (std::uint32_t i = have; i < meshes; ++i)
        out.insert(
            out.begin() + static_cast<std::ptrdiff_t>(end_of_table + kRecord * (i - have)), empty.begin(), empty.end());
    for (std::size_t field = 0x24u; field <= 0x34u; field += 4u) {
        const std::uint32_t offset = u32(pmo, field);
        if (offset >= end_of_table) put32(out, field, offset + static_cast<std::uint32_t>(grow));
    }
    put32(out, 0x08u, u32(pmo, 0x08u) + static_cast<std::uint32_t>(grow));
    const auto count = static_cast<std::uint16_t>(meshes);
    std::memcpy(out.data() + 0x1Cu, &count, 2u);
    return out;
}

// --- Hooks ---

// A kind made from a base whose behaviour checks for its own kind goes the
// base's way: v1 holds the kind just read.
int check_kind(ymod_cpu *cpu, void *user) {
    const auto *kinds = static_cast<const std::uint8_t *>(user);
    if (cpu->gpr[3] == kinds[0]) cpu->gpr[3] = kinds[1];
    return YMOD_CONTINUE;
}

// monster.build_model compiling a model: keep its material buffer when the
// model set is a kind whose base hides materials.
int compile_called(ymod_cpu *cpu, void *) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr) return YMOD_CONTINUE;
    auto &memory = runtime->memory();
    const std::uint32_t at = cpu->gpr[29] + 100u;
    if (!memory.contains(at, 4u)) return YMOD_CONTINUE;
    const std::uint32_t set = memory.load32(at) & 0xFFu;
    for (const Added &added : state().kinds) {
        if (added.kind != set || added.behaviour == nullptr || !added.behaviour->hides_materials) continue;
        const std::uint32_t pmo = cpu->gpr[6];
        if (memory.contains(pmo + 0x1Eu, 2u)) state().shown.push_back({cpu->gpr[18], memory.load16(pmo + 0x1Eu)});
    }
    return YMOD_CONTINUE;
}

// model.draw_mesh about to read a material's alpha: a kept model's material
// is shown.
int alpha_read(ymod_cpu *cpu, void *) {
    Runtime *runtime = state().runtime;
    const std::uint32_t material = cpu->gpr[8]; // t0
    for (const Shown &shown : state().shown)
        if (runtime != nullptr && material >= shown.buffer && material < shown.buffer + 16u * shown.materials &&
            runtime->memory().contains(material + 3u, 1u))
            runtime->memory().store8(material + 3u, 0xFFu);
    return YMOD_CONTINUE;
}

void run_chain(psprecomp::AllegrexContext &ctx, const std::shared_ptr<Chain> &chain) {
    if (chain->next < chain->actions.size()) {
        const Action &action = chain->actions[chain->next++];
        // A frame of its own below the caller's, for stack arguments and
        // what pointer arguments point at; given back when the call returns.
        const auto size = static_cast<std::uint32_t>((action.frame.size() * 4u + 15u) & ~std::size_t{15u});
        const std::uint32_t sp = ctx.gpr[29] - size;
        if (size != 0u && state().runtime != nullptr) {
            ctx.set_gpr(29, sp);
            for (std::size_t i = 0; i < action.frame.size(); ++i)
                state().runtime->memory().store32(sp + 4u * static_cast<std::uint32_t>(i), action.frame[i]);
        }
        for (std::uint32_t i = 0; i < 4u; ++i) ctx.set_gpr(8u + i, action.more[i]);
        for (const auto &[reg, offset] : action.pointers) ctx.set_gpr(reg, sp + offset);
        kernel().call_guest(
            ctx, action.function, action.arguments, [chain, size](psprecomp::AllegrexContext &back, std::uint32_t) {
                back.set_gpr(29, back.gpr[29] + size);
                run_chain(back, chain);
            });
        return;
    }
    // The last action has returned, to the part's caller: run the base's part
    // in its place when the mod let it run.
    if (chain->base != 0u) {
        for (std::uint32_t i = 0; i < 4u; ++i) ctx.set_gpr(4u + i, chain->arguments[i]);
        ctx.pc = chain->base;
    }
}

// A part's stub, in the kind's own vtable: `j` to the base's function. The
// mod's function runs first through a hook on the stub, then what it queued.
// A monster of a kind with attack shapes, rows or body capsules of its own:
// the kind's data it keeps becomes a copy of the base's with the mod's in it.
void give_kind_data(Added &added, std::uint32_t monster) {
    State &s = state();
    if ((added.shapes.empty() && added.body.empty() && added.rows.empty()) || s.runtime == nullptr ||
        !s.runtime->memory().contains(monster + kKindData, 4u))
        return;
    auto &memory = s.runtime->memory();
    const std::uint32_t base = memory.load32(monster + kKindData);
    if (base == 0u) return; // the game has not looked it up yet
    for (const auto &[from, copy] : added.kind_data)
        if (copy == base) return;
    std::uint32_t copy = 0u;
    if (const auto found = added.kind_data.find(base); found != added.kind_data.end()) {
        copy = found->second;
    } else {
        if (!memory.contains(base, kKindDataSize)) return;
        const std::uint32_t tables = memory.load32(base + kAttackTables);
        if (!memory.contains(tables, kAttackTablesSize)) return;
        const std::uint32_t list = memory.load32(tables + kShapeList);
        if (!memory.contains(list, kShapes * 4u)) return;
        const std::uint32_t rows = memory.load32(tables + kRowList);
        const std::uint32_t rows_size = added.rows.empty() ? 0u : kMostAttackRows * kRowSize;
        if (rows_size != 0u && !memory.contains(rows, rows_size)) return;
        std::uint32_t size = kKindDataSize + kAttackTablesSize + kShapes * 4u + rows_size;
        for (const auto &[shape, spheres] : added.shapes)
            size += static_cast<std::uint32_t>(spheres.size() + 1u) * kSphereSize;
        if (!added.body.empty()) size += static_cast<std::uint32_t>(added.body.size() + 1u) * kCapsuleSize;
        const auto *memory_service = static_cast<const ymod_memory *>(native::host()->service(s.host, YMOD_MEMORY, 1u));
        copy = memory_service != nullptr ? memory_service->allocate(s.host, size, 16u) : 0u;
        added.kind_data[base] = copy;
        if (copy == 0u) {
            problem(added.owner, added.id, "its attack shapes cannot be given: no guest memory for them");
            return;
        }
        const auto copy_words = [&memory](std::uint32_t to, std::uint32_t from, std::uint32_t bytes) {
            for (std::uint32_t at = 0; at < bytes; at += 4u) memory.store32(to + at, memory.load32(from + at));
        };
        const std::uint32_t tables_copy = copy + kKindDataSize, list_copy = tables_copy + kAttackTablesSize;
        copy_words(copy, base, kKindDataSize);
        copy_words(tables_copy, tables, kAttackTablesSize);
        copy_words(list_copy, list, kShapes * 4u);
        memory.store32(copy + kAttackTables, tables_copy);
        memory.store32(tables_copy + kShapeList, list_copy);
        std::uint32_t at = list_copy + kShapes * 4u;
        if (rows_size != 0u) {
            // The base's rows (and what follows them, past its own), then the
            // mod's: a base row with the mod's shape.
            copy_words(at, rows, rows_size);
            for (const auto &[row, own] : added.rows) {
                copy_words(at + row * kRowSize, rows + own.first * kRowSize, kRowSize);
                memory.store8(at + row * kRowSize + kRowShape, static_cast<std::uint8_t>(own.second));
            }
            memory.store32(tables_copy + kRowList, at);
            at += rows_size;
        }
        const auto put = [&memory, &at](std::uint32_t joint, float radius, float x, float y, float z) {
            for (std::uint32_t word = 0; word < kSphereSize; word += 4u) memory.store32(at + word, 0u);
            memory.store32(at, joint);
            memory.store32(at + 12u, std::bit_cast<std::uint32_t>(radius));
            memory.store32(at + 16u, std::bit_cast<std::uint32_t>(x));
            memory.store32(at + 20u, std::bit_cast<std::uint32_t>(y));
            memory.store32(at + 24u, std::bit_cast<std::uint32_t>(z));
            at += kSphereSize;
        };
        for (const auto &[shape, spheres] : added.shapes) {
            memory.store32(list_copy + shape * 4u, at);
            for (const HitSphere &sphere : spheres) put(sphere.joint, sphere.radius, sphere.x, sphere.y, sphere.z);
            put(kLastSphere, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        if (!added.body.empty()) {
            for (const std::uint32_t list_field : kBodyLists) memory.store32(copy + list_field, at);
            const auto put_float = [&memory](std::uint32_t to, float v) {
                memory.store32(to, std::bit_cast<std::uint32_t>(v));
            };
            for (const BodyCapsule &b : added.body) {
                for (std::uint32_t word = 0; word < kCapsuleSize; word += 4u) memory.store32(at + word, 0u);
                const bool sphere = b.joint_to < 0;
                memory.store16(at, static_cast<std::uint16_t>(b.joint));
                memory.store16(at + 2u, static_cast<std::uint16_t>(sphere ? 0 : b.joint_to));
                memory.store16(at + 4u, static_cast<std::uint16_t>(sphere ? 0u : 2u));
                memory.store16(at + 6u, static_cast<std::uint16_t>(b.part));
                memory.store32(at + 8u, b.zone);
                put_float(at + 16u, b.radius);
                for (std::uint32_t k = 0; k < 3u; ++k) {
                    put_float(at + 20u + 4u * k, b.from[k]);
                    put_float(at + 32u + 4u * k, b.to[k]);
                }
                at += kCapsuleSize;
            }
            for (std::uint32_t word = 0; word < 12u; word += 4u) memory.store32(at + word, 0xFFFFFFFFu);
            for (std::uint32_t word = 12u; word < kCapsuleSize; word += 4u) memory.store32(at + word, 0u);
            memory.store32(at + 8u, 0xFFFFu);
            at += kCapsuleSize;
        }
    }
    if (copy != 0u) memory.store32(monster + kKindData, copy);
}

// The tables of a kind's footsteps in guest memory, made the first time: the
// index (a u16 per motion slot, 0xFFFF for none), a row per motion with
// footsteps, and the footsteps; the other event tables are empty.
bool footstep_tables(Added &added) {
    State &s = state();
    if (added.events.index != 0u || added.footsteps.empty()) return added.events.index != 0u;
    std::vector<std::uint16_t> motions;
    for (const Footstep &f : added.footsteps)
        if (motions.empty() || motions.back() != f.motion) motions.push_back(f.motion);
    const std::uint32_t index_size = kEventSlots * 2u;
    const auto rows_size = static_cast<std::uint32_t>(motions.size()) * kEventRow;
    const auto steps_size = static_cast<std::uint32_t>(added.footsteps.size()) * kFootstepSize;
    const auto *memory_service = static_cast<const ymod_memory *>(native::host()->service(s.host, YMOD_MEMORY, 1u));
    const std::uint32_t at = memory_service != nullptr
        ? memory_service->allocate(s.host, index_size + rows_size + steps_size + kEventRow, 16u)
        : 0u;
    if (at == 0u) {
        problem(added.owner, added.id, "its footsteps cannot be given: no guest memory for them");
        added.footsteps.clear();
        return false;
    }
    auto &memory = s.runtime->memory();
    const std::uint32_t rows = at + index_size, steps = rows + rows_size;
    for (std::uint32_t slot = 0; slot < kEventSlots; ++slot) memory.store16(at + 2u * slot, 0xFFFFu);
    std::uint32_t first = 0;
    for (std::size_t r = 0; r < motions.size(); ++r) {
        std::uint32_t count = 0;
        while (first + count < added.footsteps.size() && added.footsteps[first + count].motion == motions[r]) ++count;
        const std::uint32_t row = rows + static_cast<std::uint32_t>(r) * kEventRow;
        for (std::uint32_t word = 0; word < kEventRow; word += 4u) memory.store32(row + word, 0u);
        memory.store16(row + 6u, static_cast<std::uint16_t>(first));
        memory.store8(row + 13u, static_cast<std::uint8_t>(count));
        memory.store16(at + 2u * motions[r], static_cast<std::uint16_t>(r));
        first += count;
    }
    for (std::size_t i = 0; i < added.footsteps.size(); ++i) {
        const Footstep &f = added.footsteps[i];
        const std::uint32_t e = steps + static_cast<std::uint32_t>(i) * kFootstepSize;
        memory.store32(e, f.motion);
        memory.store32(e + 4u, std::bit_cast<std::uint32_t>(f.frame));
        memory.store16(e + 8u, f.joint);
        memory.store16(e + 10u, f.size);
    }
    // An empty table for the four kinds of event a kind does not give.
    for (std::uint32_t word = 0; word < kEventRow; word += 4u) memory.store32(steps + steps_size + word, 0u);
    added.events = {at, rows, steps, steps + steps_size};
    return true;
}

// After a Think its mod did alone: the footsteps of the motion playing, as
// the base's effects part (which does not run then) plays its own.
void queue_footsteps(Added &added, std::uint32_t monster) {
    State &s = state();
    if (added.footsteps.empty() || s.runtime == nullptr) return;
    auto &memory = s.runtime->memory();
    for (std::size_t i = 0; i < kMotionEventsWords.size(); ++i)
        if (memory.load32(kMotionEvents + 4u * static_cast<std::uint32_t>(i)) != kMotionEventsWords[i]) return;
    if (!footstep_tables(added)) return;
    const Added::Events &e = added.events;
    Action action{kMotionEvents, {e.none, e.none, e.none, e.steps}};
    action.more = {e.none, e.rows, e.index, monster};
    action.frame = {0u}; // the flags: footsteps and sounds both
    s.queued.push_back(std::move(action));
}

int part_called(ymod_cpu *cpu, void *user) {
    State &s = state();
    const auto *callback = static_cast<const Callback *>(user);
    // A mod that failed or was withdrawn is not called again: its kind's
    // monsters do the base's part.
    if (callback->kind == nullptr || !native::runs(static_cast<const ymod_mod *>(callback->kind->owner))) {
        if (callback->part == kThink) s.thought_alone[cpu->gpr[4]] = false;
        return YMOD_ORIGINAL;
    }
    give_kind_data(*callback->kind, cpu->gpr[4]);
    s.queued.clear();
    s.in_part = true;
    const int result = callback->fn(cpu, cpu->gpr[4], callback->user);
    s.in_part = false;
    const bool base = result != YMOD_RETURN;
    if (callback->part == kThink) s.thought_alone[cpu->gpr[4]] = !base;
    if (callback->part == kThink && !base) queue_footsteps(*callback->kind, cpu->gpr[4]);
    if (s.queued.empty()) return base ? YMOD_ORIGINAL : YMOD_RETURN;
    auto chain = std::make_shared<Chain>();
    chain->actions = std::move(s.queued);
    s.queued.clear();
    chain->arguments = {cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]};
    chain->base = base ? callback->base : 0u;
    run_chain(*reinterpret_cast<psprecomp::AllegrexContext *>(cpu), chain);
    return YMOD_CONTINUE; // the guest goes to the first action: the hook's dispatcher leaves it there
}

// The Act (or Effects) stub of a kind whose mod gives Think but not that
// part: the base's carries out what the base's Think chose, or plays the
// base's sounds and effects for the base's motion slots, so it runs only in a
// frame the base's Think ran too.
int act_follows_think(ymod_cpu *cpu, void *) {
    const auto found = state().thought_alone.find(cpu->gpr[4]);
    return found != state().thought_alone.end() && found->second ? YMOD_RETURN : YMOD_ORIGINAL;
}

// Whether `own` is still a copy of the vtable at `base`: every slot the same,
// and each stub made leading to the base's function for its part.
bool still_copy(const psprecomp::GuestMemory &memory, const Added::Vtable &own, std::uint32_t base) {
    for (std::uint32_t slot = 0; slot < kVtableSlots; ++slot) {
        const std::uint32_t word = memory.load32(base + slot * 4u);
        const auto part = std::find(kPartSlots.begin(), kPartSlots.end(), slot);
        const Callback *stub =
            part != kPartSlots.end() ? &own.stubs[static_cast<std::size_t>(part - kPartSlots.begin())] : nullptr;
        if (stub != nullptr && stub->base != 0u ? stub->base != word : memory.load32(own.copy + slot * 4u) != word)
            return false;
    }
    return true;
}

// The kind's copy of the base vtable at `base`, made the first time one of its
// monsters is made with it, and made again when other code has come to the
// base's slot since. Each part the mod gives gets a stub in the copy, a `j`
// to the base's function, hooked to run the mod's first.
const Added::Vtable *vtable_for(Added &added, std::uint32_t base) {
    State &s = state();
    auto &memory = s.runtime->memory();
    // A monster made from one of the kind's own copies keeps it.
    for (const auto &[from, own] : added.vtables)
        if (own.copy == base) return &own;
    if (!memory.contains(base, kVtableSlots * 4u)) {
        problem(added.owner, added.id,
            "its behaviour cannot be given to its monsters: the base's vtable at " + psprecomp::hex32(base) +
                " is not in memory");
        return nullptr;
    }
    const ymod_hooks *h = hooks();
    if (const auto found = added.vtables.find(base); found != added.vtables.end()) {
        if (still_copy(memory, found->second, base)) return &found->second;
        // Other code in the base's slot: its stubs' hooks go. The copy's memory
        // stays, for a monster that may still point at it.
        for (const ymod_hook hook : found->second.hooks)
            if (h != nullptr) h->remove(s.host, hook);
        added.vtables.erase(found);
    }
    const auto *memory_service = static_cast<const ymod_memory *>(native::host()->service(s.host, YMOD_MEMORY, 1u));
    const std::uint32_t size = kVtableSlots * 4u + 8u * kPartSlots.size();
    const std::uint32_t copy = memory_service != nullptr ? memory_service->allocate(s.host, size, 16u) : 0u;
    if (copy == 0u) {
        problem(added.owner, added.id, "its behaviour cannot be given to its monsters: no guest memory for its vtable");
        return nullptr;
    }
    for (std::uint32_t slot = 0; slot < kVtableSlots; ++slot)
        memory.store32(copy + slot * 4u, memory.load32(base + slot * 4u));
    Added::Vtable &own = added.vtables[base];
    own.copy = copy;
    for (std::size_t part = 0; part < kPartSlots.size(); ++part) {
        const bool follows =
            (part == kAct || part == kEffects) && added.parts[part].fn == nullptr && added.parts[kThink].fn != nullptr;
        if (added.parts[part].fn == nullptr && !follows) continue;
        const std::uint32_t stub = copy + kVtableSlots * 4u + 8u * static_cast<std::uint32_t>(part);
        const std::uint32_t base_function = memory.load32(base + kPartSlots[part] * 4u);
        memory.store32(stub, 0x08000000u | ((base_function >> 2u) & 0x03FFFFFFu)); // j base_function
        memory.store32(stub + 4u, 0u);
        memory.store32(copy + kPartSlots[part] * 4u, stub);
        own.stubs[part] = added.parts[part];
        own.stubs[part].part = part;
        own.stubs[part].kind = &added;
        own.stubs[part].base = base_function;
        const ymod_hook hook = h != nullptr ? h->function(s.host, nullptr, stub, YMOD_HOOK_REPLACE,
                                                  follows ? &act_follows_think : &part_called, &own.stubs[part])
                                            : 0u;
        if (hook == 0u)
            problem(added.owner, added.id, "a part of its behaviour cannot be hooked");
        else
            own.hooks.push_back(hook);
    }
    return &own;
}

// A monster just made: one of a kind with parts of its own gets the kind's
// copy of the vtable it was made with.
int monster_made(ymod_cpu *cpu, void *) {
    State &s = state();
    Runtime *runtime = s.runtime;
    const std::uint32_t monster = cpu->gpr[2];
    if (runtime == nullptr || monster < 0x08800000u || !runtime->memory().contains(monster, 0x64u))
        return YMOD_CONTINUE;
    auto &memory = runtime->memory();
    const std::uint8_t number = memory.load8(monster + 0x62u);
    for (Added &added : s.kinds) {
        if (added.kind != number) continue;
        if (std::none_of(added.parts.begin(), added.parts.end(), [](const Callback &c) { return c.fn != nullptr; }))
            return YMOD_CONTINUE;
        if (const Added::Vtable *own = vtable_for(added, memory.load32(monster))) memory.store32(monster, own->copy);
        return YMOD_CONTINUE;
    }
    return YMOD_CONTINUE;
}

// A quest has ended: its models are gone, and the next quest's are kept as
// they are built. (Not on every overlay load: an area loading during the quest
// must not forget the models already built.)
void quest_ended(int, std::uint32_t, void *) {
    state().shown.clear();
    state().thought_alone.clear();
}

// Once something is kept for a quest's monsters or models: forgotten when the
// quest ends.
void watch_quest_end() {
    State &s = state();
    if (s.watching_quest_end || s.host == nullptr) return;
    const auto *events = static_cast<const ymod_events *>(native::host()->service(s.host, YMOD_EVENTS, 1u));
    s.watching_quest_end =
        events != nullptr && events->subscribe(s.host, YMOD_EVENT_QUEST_ENDED, &quest_ended, nullptr) == 0;
}

const Behaviour *behaviour_of(std::uint8_t base) {
    for (const Behaviour &b : kBehaviours)
        if (b.kind == base) return &b;
    return nullptr;
}

} // namespace

std::span<const GameKind> game_kinds() {
    return kGameKinds;
}

std::optional<std::uint8_t> kind(std::string_view id) {
    for (const GameKind &k : kGameKinds)
        if (k.id == id) return k.kind;
    for (const Added &added : state().kinds)
        if (added.id == id) return added.kind;
    return std::nullopt;
}

std::string_view id_of(std::uint8_t number) {
    for (const GameKind &k : kGameKinds)
        if (k.kind == number) return k.id;
    for (const Added &added : state().kinds)
        if (added.kind == number) return added.id;
    return {};
}

namespace {

// The effects entry of a model pack: a u32 count, an offset per effect (an
// offset at the entry's end is none), and per effect its part count, header
// size and part offsets, then at the header's end a float and a count of
// 80-byte emitters, each starting with its bank: a type (1: a monster's own)
// and the kind (u8). game_sub (0x0A013878) finds a type 1 emitter's models by
// that kind's loaded model set. Rewrites the kind of those owned by `from`.
constexpr std::size_t kEffectsEntry = 7u;
constexpr std::size_t kEmitterSize = 80u;
void give_effects(std::vector<std::uint8_t> &entry, std::uint8_t from, std::uint8_t to) {
    const auto word = [&entry](std::size_t at) { return at + 4u <= entry.size() ? u32(entry, at) : 0u; };
    const std::size_t count = word(0u);
    for (std::size_t i = 0; i < count && 8u + 4u * i <= entry.size(); ++i) {
        const std::size_t effect = word(4u + 4u * i);
        if (effect + 8u > entry.size()) continue;
        const std::size_t emitters = effect + word(effect + 4u);
        const std::size_t emitter_count = word(emitters + 4u);
        for (std::size_t j = 0; j < emitter_count; ++j) {
            const std::size_t at = emitters + 8u + kEmitterSize * j;
            if (at + kEmitterSize > entry.size()) break;
            if (entry[at] == 1u && entry[at + 1u] == from) entry[at + 1u] = to;
        }
    }
}

// Which effect numbers a model pack has: an offset inside the effects entry.
std::vector<bool> effects_of(std::span<const std::uint8_t> pack) {
    const auto entries = read_pack(pack);
    std::vector<bool> present;
    if (!entries || entries->size() <= kEffectsEntry || (*entries)[kEffectsEntry].size() < 4u) return present;
    const std::span<const std::uint8_t> entry = (*entries)[kEffectsEntry];
    const std::size_t count = u32(entry, 0u);
    for (std::size_t i = 0; i < count && 8u + 4u * i <= entry.size(); ++i)
        present.push_back(u32(entry, 4u + 4u * i) + 8u <= entry.size());
    return present;
}

} // namespace

std::vector<std::uint8_t> compose_pack(std::span<const std::uint8_t> mod, std::span<const std::uint8_t> base,
    std::uint32_t meshes, std::span<const std::uint8_t> effects, std::uint8_t from, std::uint8_t to) {
    const auto mine = read_pack(mod);
    const auto theirs = read_pack(base);
    const auto effect_pack = effects.empty() ? theirs : read_pack(effects);
    if (!mine || !theirs || !effect_pack) return {};
    std::vector<std::vector<std::uint8_t>> entries(
        std::max({mine->size(), theirs->size(), effects.empty() ? std::size_t{0} : effect_pack->size()}));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto &others = i >= 3u ? *effect_pack : *theirs;
        const std::span<const std::uint8_t> chosen = i < mine->size() && !(*mine)[i].empty()
            ? (*mine)[i]
            : (i < others.size() ? others[i] : std::span<const std::uint8_t>{});
        entries[i].assign(chosen.begin(), chosen.end());
    }
    if (entries.size() > 1u && !entries[1].empty()) entries[1] = with_meshes(entries[1], meshes);
    if (from != to && entries.size() > kEffectsEntry) give_effects(entries[kEffectsEntry], from, to);
    return write_pack(entries);
}

std::optional<std::uint8_t> add(Runtime &runtime, const Spec &spec, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) -> std::optional<std::uint8_t> {
        if (why != nullptr) *why = std::move(reason);
        return std::nullopt;
    };
    if (spec.id.empty() || kind(spec.id)) return fail("the id \"" + spec.id + "\" is taken or empty");
    const std::optional<std::uint8_t> base = kind(spec.base);
    if (!base || *base >= kGameKindCount) return fail("the base \"" + spec.base + "\" is not one of the game's kinds");
    const std::size_t number = kGameKindCount + s.kinds.size();
    if (number > 0xFEu) return fail("no kind numbers are left");
    const auto kind_number = static_cast<std::uint8_t>(number);
    const Behaviour *behaviour = behaviour_of(*base);
    s.runtime = &runtime;

    // The base's own model pack, for what the mod's leaves out.
    const tables::Table *models = tables::find(kModelTables[0]);
    if (models == nullptr) return fail("the model tables are not known");
    const std::optional<tables::Placement> now = tables::placement(kModelTables[0]);
    const std::uint32_t base_row = now->address + (*base - 1u) * models->row;
    if (*base == 0u || !runtime.memory().contains(base_row, 2u)) return fail("the base's model set cannot be read");
    const FileId base_file = runtime.memory().load16(base_row);
    const std::optional<std::vector<std::uint8_t>> base_pack =
        s.reader ? s.reader(base_file) : original_file(base_file);
    if (!base_pack) return fail("the base's model pack cannot be read");
    // The kind whose effects it takes: the base's unless the mod names another.
    std::uint8_t effects_kind = *base;
    std::optional<std::vector<std::uint8_t>> effects_pack;
    if (!spec.effects.empty()) {
        const std::optional<std::uint8_t> other = kind(spec.effects);
        if (!other || *other == 0u || *other >= kGameKindCount)
            return fail("the effects' kind \"" + spec.effects + "\" is not one of the game's kinds");
        effects_kind = *other;
        const FileId effects_file = runtime.memory().load16(now->address + (*other - 1u) * models->row);
        effects_pack = s.reader ? s.reader(effects_file) : original_file(effects_file);
        if (!effects_pack) return fail("the model pack of \"" + spec.effects + "\" cannot be read");
    }
    std::vector<std::uint8_t> pack = compose_pack(spec.model, *base_pack, behaviour ? behaviour->meshes : 0u,
        effects_pack ? std::span<const std::uint8_t>(*effects_pack) : std::span<const std::uint8_t>{}, effects_kind,
        kind_number);
    if (pack.empty()) return fail("the model pack cannot be read");

    // Every table a row longer; refused before anything is served.
    for (const std::string_view table : kKindTables)
        if (std::string reason; !tables::extend(runtime, table, kind_number + 1u, &reason))
            return fail("table " + std::string(table) + " cannot grow: " + reason);
    std::array<std::uint32_t, 3> model_rows{};
    for (std::size_t i = 0; i < 3u; ++i) {
        std::string reason;
        const std::optional<std::uint32_t> at = tables::extend(runtime, kModelTables[i], kind_number, &reason);
        if (!at) return fail("table " + std::string(kModelTables[i]) + " cannot grow: " + reason);
        model_rows[i] = *at + (kind_number - 1u) * 2u;
    }
    // The size of its footsteps' dust: the base's. Not fatal: past the
    // table's rows a footstep's dust takes the next table's words.
    if (std::string reason; const auto steps = tables::extend(runtime, kStepScales, kind_number + 1u, &reason))
        runtime.memory().store32(*steps + 4u * kind_number, runtime.memory().load32(*steps + 4u * *base));
    else
        problem(spec.owner, spec.id, "its footsteps' dust keeps no size (" + reason + ")");

    Added added;
    added.id = spec.id;
    added.owner = spec.owner;
    added.kind = kind_number;
    added.base = *base;
    added.behaviour = behaviour;
    added.check_kind = {kind_number, *base};
    added.effects = effects_of(pack);
    const std::array<std::vector<std::uint8_t>, 3> files{std::move(pack), spec.geometry, spec.motions};
    for (std::size_t i = 0; i < 3u; ++i) {
        const FileId id = added::free_id();
        if (id == 0u || !added::add(runtime, id, files[i])) return fail("its files cannot be served");
        added.files[i] = id;
        runtime.memory().store16(model_rows[i], static_cast<std::uint16_t>(id));
    }
    s.kinds.push_back(std::move(added));
    Added &kept = s.kinds.back();

    if (behaviour != nullptr) {
        native::attach(runtime);
        const ymod_hooks *h = hooks();
        // In each slot the base's AI can load in, at its overlay there.
        for (std::uint32_t slot = 0; slot < behaviour->slots && slot < kMonsterSlots; ++slot) {
            const std::string overlay = std::string(behaviour->ai) + "m" + std::to_string(slot);
            for (const std::uint32_t offset : behaviour->kind_checks) {
                const std::uint32_t at = kOverlaySlots[kFirstMonsterSlot + slot] + offset;
                if (h == nullptr ||
                    h->instruction(s.host, overlay.c_str(), at, &check_kind, kept.check_kind.data()) == 0u)
                    problem(spec.owner, spec.id,
                        "the base's kind check at " + psprecomp::hex32(at) + " (" + overlay + ") cannot be hooked");
            }
        }
        if (behaviour->hides_materials && !s.draw_hooked && h != nullptr) {
            if (runtime.memory().load32(kMaterialAlpha) != kMaterialAlphaWord)
                problem(spec.owner, spec.id, "the model draw is not as traced; the base's hidden parts stay hidden");
            else
                s.draw_hooked = h->instruction(s.host, "game_task", kCompileCall, &compile_called, nullptr) != 0u &&
                    h->instruction(s.host, nullptr, kMaterialAlpha, &alpha_read, nullptr) != 0u;
            if (s.draw_hooked) watch_quest_end();
        }
    }
    if (behaviour == nullptr)
        problem(spec.owner, spec.id,
            "its base " + spec.base +
                " has no traced behaviour (only tigrex and brute_tigrex do): where the base's AI checks its own kind, "
                "hides materials or writes meshes by number, the kind is not given the base's way, and it may go "
                "another kind's way or stop the game");
    std::cout << "[mods] " << (spec.mod.empty() ? std::string("a mod") : spec.mod) << " adds monster " << spec.id
              << " as kind " << int{kind_number} << ", based on " << spec.base << "\n";
    return kind_number;
}

bool behave(
    Runtime &runtime, const void *owner, std::uint8_t number, int part, PartFn fn, void *user, std::string *why) {
    State &s = state();
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    if (part < 0 || static_cast<std::size_t>(part) >= kPartSlots.size()) return fail("no such part of behaviour");
    if (fn == nullptr) return fail("no function given");
    Added *added = nullptr;
    for (Added &k : s.kinds)
        if (k.kind == number) added = &k;
    if (added == nullptr || added->owner != owner) return fail("the kind was not added by this mod");
    if (!added->vtables.empty()) return fail("its monsters have been made already; give behaviour at start");
    added->parts[static_cast<std::size_t>(part)] = {fn, user, 0u, static_cast<std::size_t>(part), added};
    s.runtime = &runtime;
    if (!s.made_hooked) {
        native::attach(runtime);
        const ymod_hooks *h = hooks();
        s.made_hooked = h != nullptr &&
            h->function(s.host, "game_task", kMakeMonster, YMOD_HOOK_AFTER, &monster_made, nullptr) != 0u;
        if (!s.made_hooked) return fail("the monster maker cannot be hooked");
    }
    // Which monsters thought alone, kept by monster, goes with the quest.
    watch_quest_end();
    return true;
}

bool attack_shape(
    const void *owner, std::uint8_t number, std::uint32_t shape, std::span<const HitSphere> spheres, std::string *why) {
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    Added *added = nullptr;
    for (Added &k : state().kinds)
        if (k.kind == number) added = &k;
    if (added == nullptr || added->owner != owner) return fail("the kind was not added by this mod");
    if (shape >= kShapes) return fail("no such attack shape");
    if (spheres.empty() || spheres.size() > 16u) return fail("a shape has 1 to 16 spheres");
    for (const HitSphere &sphere : spheres)
        if (sphere.joint > 0xFFu || !(sphere.radius > 0.0f) || !std::isfinite(sphere.radius) ||
            !std::isfinite(sphere.x) || !std::isfinite(sphere.y) || !std::isfinite(sphere.z))
            return fail("a sphere needs a joint, a radius above 0 and a centre");
    added->shapes[shape].assign(spheres.begin(), spheres.end());
    added->kind_data.clear(); // made again with the new shapes
    return true;
}

bool attack_row(const void *owner, std::uint8_t number, std::uint32_t row, std::uint32_t like, std::uint32_t shape,
    std::string *why) {
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    Added *added = nullptr;
    for (Added &k : state().kinds)
        if (k.kind == number) added = &k;
    if (added == nullptr || added->owner != owner) return fail("the kind was not added by this mod");
    if (row == 0u || row >= kMostAttackRows || like >= kMostAttackRows) return fail("no such attack row");
    if (shape >= kShapes) return fail("no such attack shape");
    added->rows[row] = {like, shape};
    added->kind_data.clear(); // made again with the new row
    return true;
}

bool footsteps(const void *owner, std::uint8_t number, std::span<const Footstep> steps, std::string *why) {
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    Added *added = nullptr;
    for (Added &k : state().kinds)
        if (k.kind == number) added = &k;
    if (added == nullptr || added->owner != owner) return fail("the kind was not added by this mod");
    if (steps.empty() || steps.size() > kMostFootsteps) return fail("it has none or more than 256");
    for (const Footstep &f : steps)
        if (f.motion >= kEventSlots || f.size >= kFootstepSizes || f.joint > 0xFFu || !std::isfinite(f.frame) ||
            f.frame < 0.0f)
            return fail("a footstep needs a motion below 1000, a frame, a joint and a size from 0 to 9");
    added->footsteps.assign(steps.begin(), steps.end());
    std::stable_sort(added->footsteps.begin(), added->footsteps.end(),
        [](const Footstep &a, const Footstep &b) { return a.motion < b.motion; });
    added->events = {}; // made again with these
    return true;
}

bool body_shapes(const void *owner, std::uint8_t number, std::span<const BodyCapsule> capsules, std::string *why) {
    const auto fail = [why](std::string reason) {
        if (why != nullptr) *why = std::move(reason);
        return false;
    };
    Added *added = nullptr;
    for (Added &k : state().kinds)
        if (k.kind == number) added = &k;
    if (added == nullptr || added->owner != owner) return fail("the kind was not added by this mod");
    if (capsules.empty() || capsules.size() > 64u) return fail("a body has 1 to 64 capsules");
    const auto finite = [](const float *v) {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    };
    for (const BodyCapsule &b : capsules)
        if (b.joint > 0xFFu || b.joint_to > 0xFF || b.part > 0xFFu || !(b.radius > 0.0f) || !std::isfinite(b.radius) ||
            !finite(b.from) || !finite(b.to))
            return fail("a capsule needs joints, a part, a radius above 0 and its ends");
    added->body.assign(capsules.begin(), capsules.end());
    added->kind_data.clear(); // made again with the new capsules
    return true;
}

bool play_motion(std::uint32_t monster, std::uint16_t motion, std::uint32_t blend) {
    State &s = state();
    if (!s.in_part || s.runtime == nullptr || s.runtime->memory().load32(kPlayMotion) != kPlayMotionWord) return false;
    s.queued.push_back({kPlayMotion, {monster, motion, blend, 0u}});
    return true;
}

namespace {

// The monster's model, through its class's model getter (the usual
// one-instruction leaf); nothing when that cannot be read.
std::optional<std::uint32_t> model_of(Runtime &runtime, std::uint32_t monster) {
    auto &memory = runtime.memory();
    if (monster < 0x08800000u || !memory.contains(monster, 4u)) return std::nullopt;
    const std::uint32_t vtable = memory.load32(monster);
    if (!memory.contains(vtable + kModelSlot * 4u, 4u)) return std::nullopt;
    const std::uint32_t getter = memory.load32(vtable + kModelSlot * 4u);
    if (!memory.contains(getter, 8u)) return std::nullopt;
    // addiu v0, a0, offset; jr ra (or the other way round, the addiu in the delay slot)
    const std::uint32_t first = memory.load32(getter), second = memory.load32(getter + 4u);
    const std::uint32_t add = first >> 16u == 0x2482u ? first : second;
    if (add >> 16u != 0x2482u || (first != 0x03E00008u && second != 0x03E00008u)) return std::nullopt;
    const std::uint32_t model = monster + static_cast<std::uint32_t>(static_cast<std::int16_t>(add & 0xFFFFu));
    if (!memory.contains(model, 84u)) return std::nullopt;
    return model;
}

float real(Runtime &runtime, std::uint32_t at) {
    return std::bit_cast<float>(runtime.memory().load32(at));
}

bool body(Runtime &runtime, std::uint32_t monster) {
    return monster >= 0x08800000u && runtime.memory().contains(monster, kBodySize);
}

// A facing in the game's units (65536 a turn) from one point towards another
// on the ground, as the game's own (0x08877E78) works it out.
std::uint32_t facing_towards(float from_x, float from_z, float to_x, float to_z) {
    const double radians = std::atan2(static_cast<double>(to_x - from_x), static_cast<double>(to_z - from_z));
    return static_cast<std::uint32_t>(
               static_cast<std::int32_t>(std::lround(radians * kTurn / (2.0 * std::numbers::pi)))) &
        0xFFFFu;
}

bool queue(std::uint32_t function, std::span<const std::uint32_t> words, std::array<std::uint32_t, 4> arguments) {
    State &s = state();
    if (!s.in_part || s.runtime == nullptr) return false;
    for (std::size_t i = 0; i < words.size(); ++i)
        if (s.runtime->memory().load32(function + 4u * static_cast<std::uint32_t>(i)) != words[i]) return false;
    s.queued.push_back({function, arguments});
    return true;
}

} // namespace

std::optional<bool> motion_finished(Runtime &runtime, std::uint32_t monster) {
    const std::optional<std::uint32_t> found = model_of(runtime, monster);
    if (!found) return std::nullopt;
    auto &memory = runtime.memory();
    const std::uint32_t model = *found;
    // As 0x0A0AD0B0: the step counts unless the motion loops or is held.
    float step = real(runtime, model + 12u);
    const bool held = memory.load32(model + 40u) == 1u && (memory.load32(model + 64u) & 1u) == 0u;
    const bool loops = (memory.load16(model + 80u) & 2u) != 0u;
    if (held || loops || static_cast<std::int32_t>(memory.load32(model + 72u)) > 0) step = 0.0f;
    return real(runtime, model + 20u) < real(runtime, model + 4u) + step;
}

std::optional<float> motion_frame(Runtime &runtime, std::uint32_t monster) {
    const std::optional<std::uint32_t> model = model_of(runtime, monster);
    if (!model) return std::nullopt;
    return real(runtime, *model + 4u);
}

std::optional<bool> at_frame(Runtime &runtime, std::uint32_t monster, float frame) {
    const std::optional<std::uint32_t> found = model_of(runtime, monster);
    if (!found) return std::nullopt;
    auto &memory = runtime.memory();
    const std::uint32_t model = *found;
    // As 0x0887A118 for channel 0: a motion held still, or not advancing,
    // passes no frame.
    const bool held = memory.load32(model + 40u) == 1u && (memory.load32(model + 64u) & 1u) == 0u;
    if (held || (memory.load16(model + 80u) & 2u) != 0u || static_cast<std::int32_t>(memory.load32(model + 72u)) > 0)
        return false;
    const float now = real(runtime, model + 4u), next = now + real(runtime, model + 12u);
    const float last = real(runtime, model + 20u), loop_start = real(runtime, model + 16u);
    if (last < next && (memory.load16(model + 80u) & 1u) != 0u) // wrapping round to the loop's start
        return (frame <= loop_start && loop_start <= frame) || now < frame;
    return now < frame && frame <= std::min(next, last);
}

std::optional<Vec3> position(Runtime &runtime, std::uint32_t monster) {
    if (!body(runtime, monster)) return std::nullopt;
    return Vec3{real(runtime, monster + kPosition), real(runtime, monster + kPosition + 4u),
        real(runtime, monster + kPosition + 8u)};
}

std::optional<float> facing(Runtime &runtime, std::uint32_t monster) {
    if (!body(runtime, monster)) return std::nullopt;
    return static_cast<float>(runtime.memory().load32(monster + kFacing) & 0xFFFFu) * 360.0f / kTurn;
}

std::optional<Health> health(Runtime &runtime, std::uint32_t monster) {
    if (!body(runtime, monster)) return std::nullopt;
    auto &memory = runtime.memory();
    return Health{static_cast<std::int16_t>(memory.load16(monster + kHealth)),
        static_cast<std::int16_t>(memory.load16(monster + kMostHealth))};
}

bool set_facing(Runtime &runtime, std::uint32_t monster, float degrees) {
    if (!body(runtime, monster) || !std::isfinite(degrees)) return false;
    const float turns = degrees / 360.0f - std::floor(degrees / 360.0f);
    runtime.memory().store32(monster + kFacing, static_cast<std::uint32_t>(std::lround(turns * kTurn)) & 0xFFFFu);
    return true;
}

std::optional<Vec3> target(Runtime &runtime, std::uint32_t monster) {
    if (!body(runtime, monster)) return std::nullopt;
    return Vec3{
        real(runtime, monster + kTarget), real(runtime, monster + kTarget + 4u), real(runtime, monster + kTarget + 8u)};
}

std::optional<bool> turn_to_target(Runtime &runtime, std::uint32_t monster, float degrees) {
    const std::optional<Vec3> at = position(runtime, monster), to = target(runtime, monster);
    if (!at || !to || !std::isfinite(degrees)) return std::nullopt;
    auto &memory = runtime.memory();
    const std::uint32_t wanted = facing_towards(at->x, at->z, to->x, to->z);
    const std::uint32_t now = memory.load32(monster + kFacing) & 0xFFFFu;
    const auto step = static_cast<std::int32_t>(std::lround(std::clamp(degrees, 0.0f, 180.0f) * kTurn / 360.0f));
    // As 0x0A0A7C20: the shorter way round, by at most the step.
    const auto difference = static_cast<std::int16_t>(static_cast<std::uint16_t>(wanted - now));
    std::uint32_t turned = wanted;
    if (std::abs(static_cast<std::int32_t>(difference)) >= step)
        turned = (difference < 0 ? now - static_cast<std::uint32_t>(step) : now + static_cast<std::uint32_t>(step)) &
            0xFFFFu;
    memory.store32(monster + kFacing, turned);
    return turned == wanted;
}

bool move(Runtime &runtime, std::uint32_t monster, float x, float z) {
    if (!body(runtime, monster) || !std::isfinite(x) || !std::isfinite(z)) return false;
    auto &memory = runtime.memory();
    const double radians =
        static_cast<double>(memory.load32(monster + kFacing) & 0xFFFFu) * 2.0 * std::numbers::pi / kTurn;
    const double sin = std::sin(radians), cos = std::cos(radians);
    const auto shift = [&](std::uint32_t at, double by) {
        memory.store32(at, std::bit_cast<std::uint32_t>(static_cast<float>(real(runtime, at) + by)));
    };
    // Turned by the facing as the game turns a vector (0x08876D64).
    shift(monster + kPosition, cos * x + sin * z);
    shift(monster + kPosition + 8u, cos * z - sin * x);
    return true;
}

bool attack(std::uint32_t monster, std::uint8_t slot, std::uint32_t number) {
    if (slot >= kAttackSlots || number >= kMostAttackRows) return false;
    return queue(kStartAttack, kStartAttackWords, {monster, slot, number, 0u});
}

bool effect(std::uint32_t monster, std::uint32_t number, std::uint32_t joint, Vec3 offset, float scale) {
    State &s = state();
    if (!s.in_part || s.runtime == nullptr || !s.runtime->memory().contains(monster + 0x62u, 1u)) return false;
    auto &memory = s.runtime->memory();
    for (std::size_t i = 0; i < kStartEffectWords.size(); ++i)
        if (memory.load32(kStartEffect + 4u * static_cast<std::uint32_t>(i)) != kStartEffectWords[i]) return false;
    // As 0x08856504: the slot of the loaded model sets holding the kind's
    // first model file (monster.models, by kind from 1).
    const std::uint8_t kind = memory.load8(monster + 0x62u);
    // Only numbers its pack has: an effect that is not there draws from no model.
    const auto added = std::find_if(s.kinds.begin(), s.kinds.end(), [kind](const Added &k) { return k.kind == kind; });
    if (added == s.kinds.end() || number >= added->effects.size() || !added->effects[number]) return false;
    const std::optional<tables::Placement> models = tables::placement("monster.models");
    const std::uint32_t sets = memory.load32(kModelSetsPointer);
    if (kind == 0u || !models || kind > models->rows || !memory.contains(sets + 0x90u, 48u)) return false;
    const std::uint16_t file = memory.load16(models->address + 2u * (kind - 1u));
    std::optional<std::uint32_t> slot;
    for (std::uint32_t i = 0; i < 4u && !slot; ++i)
        if ((memory.load16(sets + 0x90u + 12u * i) & 2u) != 0u && memory.load16(sets + 0x92u + 12u * i) == file)
            slot = i;
    if (!slot) return false;
    Action action{kStartEffect, {kModelSetBank + *slot, number, monster, joint & 0xFFFFu}};
    // The stack arguments at the frame's start, the offset 16 bytes in, the
    // scale 32 bytes in.
    const std::uint32_t size = std::bit_cast<std::uint32_t>(scale);
    action.frame = {kEffectFlags, 0u, 0u, 0u, std::bit_cast<std::uint32_t>(offset.x),
        std::bit_cast<std::uint32_t>(offset.y), std::bit_cast<std::uint32_t>(offset.z), 0u, size, size, size, 0u};
    action.pointers = {{8u, 16u}, {9u, 32u}};
    s.queued.push_back(std::move(action));
    return true;
}

bool aim(std::uint32_t monster) {
    return queue(kAim, kAimWords, {monster, 0u, 0u, 0u});
}

bool end_attack(std::uint32_t monster, std::uint8_t slot) {
    if (slot >= kAttackSlots) return false;
    return queue(kEndAttack, kEndAttackWords, {monster, slot, 0u, 0u});
}

void overlay_loaded(Runtime &runtime) {
    State &s = state();
    if (s.kinds.empty()) return;
    const std::optional<LoadedOverlay> overlay = overlay_at(runtime.memory(), kTaskSlot);
    if (!overlay || !iequals(overlay->name, "game_task")) return;
    for (const std::string_view name : kKindTables) {
        const tables::Table *table = tables::find(name);
        const std::optional<tables::Placement> now = tables::placement(name);
        if (table == nullptr || !now) continue;
        for (const Added &added : s.kinds) {
            if (added.kind >= now->rows) continue;
            const std::uint32_t from = now->address + added.base * table->row;
            const std::uint32_t to = now->address + added.kind * table->row;
            for (std::uint32_t i = 0; i < table->row; i += 4u)
                runtime.memory().store32(to + i, runtime.memory().load32(from + i));
        }
    }
}

void read_packs_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader) {
    state().reader = std::move(reader);
}

void reset_for_tests() {
    State &s = state();
    s.kinds.clear();
    s.shown.clear();
    // Its hooks and events go with it.
    native::withdraw(s.host);
    s.host = nullptr;
    s.runtime = nullptr;
    s.draw_hooked = false;
    s.watching_quest_end = false;
    s.made_hooked = false;
    s.in_part = false;
    s.queued.clear();
    s.thought_alone.clear();
}

} // namespace mhp3rd::mods::monsters
