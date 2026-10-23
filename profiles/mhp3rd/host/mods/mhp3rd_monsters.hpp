#pragma once

#include "mods/mod_library.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psprecomp {
class Runtime;
}
struct ymod_cpu;

// Large monsters mods add (the "yakumo.monsters" service): a mod names a new
// kind, the game's kind it starts from, and its own model files, and gets the
// kind's number. Built on the game tables (mhp3rd_tables.hpp) and the files
// mods add (mhp3rd_added_files.hpp); docs/MONSTERS.md describes what each
// step does in the game.
//
// For a kind the game numbers it from 75 up, and:
//
// - extends every table indexed by kind or model set by a row;
// - serves the mod's three files as new files, and points the kind's model
//   set at them;
// - gives the kind the base's rows in game_task's tables (behaviour,
//   constructor, object size, extra files, carves) each time game_task loads;
// - completes the mod's model pack with the base's entries it leaves out
//   (beyond the skeleton, model and textures), and gives the model at least
//   as many meshes as the base's, since the base's behaviour writes to its
//   meshes by number;
// - sends the kind the base's way where the base's behaviour checks its own
//   kind, and keeps every material of the kind's model shown where the base's
//   behaviour hides parts by material number (both traced for the Tigrex).
namespace mhp3rd::mods::monsters {

// The game's kinds of large monster (and the small ones that share the
// numbers), as mods name them: from the game's own names (text block 16,
// table 2, from entry 308), with subspecies told apart by the quests that use
// them (docs/MONSTERS.md).
struct GameKind {
    std::string_view id;
    std::uint8_t kind{};
};
[[nodiscard]] std::span<const GameKind> game_kinds();

// A kind by its id, the game's or one a mod added; nothing for an unknown id.
[[nodiscard]] std::optional<std::uint8_t> kind(std::string_view id);
// The id of a kind, or empty for a number nothing has.
[[nodiscard]] std::string_view id_of(std::uint8_t kind);

struct Spec {
    std::string id;                     // the mod's name for the kind
    std::string base;                   // the game's kind it starts from, by id
    std::vector<std::uint8_t> model;    // its model pack (skeleton, model, textures)
    std::vector<std::uint8_t> geometry; // the model's GE data
    std::vector<std::uint8_t> motions;  // its motions
    std::string mod;                    // who adds it, for the log
    const void *owner{};                // and which mod, for behave()
    std::string effects;                // the game's kind whose effects it takes, by id; empty: the base's
};

// Adds a kind and returns its number, or nothing (with the reason in `why`)
// when the id is taken, the base is not a large monster this knows, a table
// cannot grow, or the files cannot be served.
[[nodiscard]] std::optional<std::uint8_t> add(
    psprecomp::Runtime &runtime, const Spec &spec, std::string *why = nullptr);

// A part of behaviour a mod gives a kind it added: the function the game
// calls for that part (a vtable slot of the monster's class, shared by Freedom
// Unite's and Portable 3rd's monsters: docs/MONSTERS.md) runs `fn` first.
// fn returns 1 (YMOD_RETURN) when it has done the part, anything else to have
// the base's run after it.
using PartFn = int (*)(::ymod_cpu *cpu, std::uint32_t monster, void *user);
// Act runs after Think, each frame, and carries out the action Think chose;
// a kind whose Think the mod does itself (YMOD_RETURN) skips the base's Act in
// that frame, unless the mod gives Act too.
enum class Part { Think = 0, Act = 1, Effects = 2 };
[[nodiscard]] bool behave(psprecomp::Runtime &runtime, const void *owner, std::uint8_t kind, int part, PartFn fn,
    void *user, std::string *why = nullptr);

// A sphere an attack hits with, on one of the kind's joints: its radius and
// its centre from the joint, in the joint's frame.
struct HitSphere {
    std::uint32_t joint{};
    float radius{};
    float x{}, y{}, z{};
};
// Gives a kind a mod added its own spheres for one of the base's attack
// shapes (a shape is what a row of the attack table hits with; the base's
// are on the base's joints). Applies to its monsters while it has a part of
// behaviour. False when the kind is not the mod's, the shape number is out of
// range or a sphere is not usable.
[[nodiscard]] bool attack_shape(const void *owner, std::uint8_t kind, std::uint32_t shape,
    std::span<const HitSphere> spheres, std::string *why = nullptr);

// Gives a kind a mod added an attack row of its own: row `row` of its attack
// table becomes a copy of the base's row `like` (its strength and kind of
// hit) that hits with shape `shape`, the base's or one the mod gives with
// attack_shape. Rows and shapes past the base's own may be given, up to
// kMostAttackRows and 32. Applies to its monsters while it has a part of
// behaviour. False when the kind is not the mod's or a number is out of range.
inline constexpr std::uint32_t kMostAttackRows = 64u;
[[nodiscard]] bool attack_row(const void *owner, std::uint8_t kind, std::uint32_t row, std::uint32_t like,
    std::uint32_t shape, std::string *why = nullptr);

// A capsule (or a sphere) of a monster's body, where hunters' hits land: from
// `from` in joint `joint`'s frame to `to` in joint `joint_to`'s, `radius`
// round; a sphere at `from` when joint_to is negative. `part` is the body part
// it belongs to and `zone` the hit zone word, as the base's own capsules give
// them (docs/MONSTERS.md, "A monster's body capsules").
struct BodyCapsule {
    std::uint32_t joint{};
    std::int32_t joint_to{-1};
    std::uint32_t part{};
    std::uint32_t zone{};
    float radius{};
    float from[3]{}, to[3]{};
};
// Gives a kind a mod added its own body capsules in place of the base's
// (which are on the base's joints). Applies to its monsters while it has a
// part of behaviour. False when the kind is not the mod's, there are none or
// more than 64, or one is not usable.
[[nodiscard]] bool body_shapes(
    const void *owner, std::uint8_t kind, std::span<const BodyCapsule> capsules, std::string *why = nullptr);

// A footstep: in motion `motion`, at its frame `frame`, joint `joint` comes
// down, raising the dust of a step of size `size` (the game's kinds of step
// for large monsters, 0 to 9; the Tigrex's use 0, 1, 2, 4 and 5) on whatever
// ground the monster stands on. The game keeps them in tables by motion slot
// that a monster's effects part plays (docs/MONSTERS.md, "A monster's
// footsteps").
struct Footstep {
    std::uint16_t motion{};
    float frame{};
    std::uint16_t joint{};
    std::uint16_t size{};
};
inline constexpr std::size_t kMostFootsteps = 256u;
inline constexpr std::size_t kMostFootstepsInMotion = 255u; // a motion's row counts them in a byte
inline constexpr std::uint16_t kFootstepSizes = 10u;
// Gives a kind a mod added its own footsteps: after each frame its mod's
// THINK did alone, the game plays those of the motion playing. False when the
// kind is not the mod's, there are none or more than kMostFootsteps, more
// than kMostFootstepsInMotion in one motion, or one is not usable (a motion
// past 999, a size past 9).
[[nodiscard]] bool footsteps(
    const void *owner, std::uint8_t kind, std::span<const Footstep> steps, std::string *why = nullptr);

// During a part of behaviour (inside its fn): queues the game's motion setter
// for `monster`, carried out once fn returns. False outside a part.
[[nodiscard]] bool play_motion(std::uint32_t monster, std::uint16_t motion, std::uint32_t blend);
// Whether the motion `monster` plays has reached its last frame, as the
// game's own check (0x0A0AD0B0) works it out; nothing when its model cannot be
// read (its class's model getter is not the usual one-instruction leaf).
[[nodiscard]] std::optional<bool> motion_finished(psprecomp::Runtime &runtime, std::uint32_t monster);

// The monster's body, as the game keeps it (docs/MONSTERS.md, "A monster's
// body"). Positions are in the game's units, y up; facings in degrees from
// +z, growing towards +x. Each gives nothing for an address that is not
// guest memory.
struct Vec3 {
    float x{}, y{}, z{};
};
[[nodiscard]] std::optional<Vec3> position(psprecomp::Runtime &runtime, std::uint32_t monster);
[[nodiscard]] std::optional<float> facing(psprecomp::Runtime &runtime, std::uint32_t monster);
// Its health now and at most (+0x246 and +0x288, s16), as the developer
// tools read them; 0 now once it has fallen.
struct Health {
    std::int32_t now{}, most{};
};
[[nodiscard]] std::optional<Health> health(psprecomp::Runtime &runtime, std::uint32_t monster);
bool set_facing(psprecomp::Runtime &runtime, std::uint32_t monster, float degrees);
// The point the monster is after: the hunter it targets, which the game keeps
// up to date.
[[nodiscard]] std::optional<Vec3> target(psprecomp::Runtime &runtime, std::uint32_t monster);
// During a part of behaviour: queues the game's own update of the target
// point (0x0A0AFEB0), from the target it has chosen (a hunter, by default the
// first). The game updates it on its own only now and then.
[[nodiscard]] bool aim(std::uint32_t monster);
// Turns towards the target by at most `degrees`, as the game's own turn
// (0x0A0A7C74) does; whether it faces the target now.
[[nodiscard]] std::optional<bool> turn_to_target(psprecomp::Runtime &runtime, std::uint32_t monster, float degrees);
// Moves the monster by (x, z) in its own frame: z ahead, x to the side, as in
// its motions (the game moves a monster only as its behaviour says: motions
// turn it, they do not carry it).
bool move(psprecomp::Runtime &runtime, std::uint32_t monster, float x, float z);
// The frame the motion has reached.
[[nodiscard]] std::optional<float> motion_frame(psprecomp::Runtime &runtime, std::uint32_t monster);
// Whether the motion passes `frame` in this frame's step, as the game's frame
// check (0x0887A118, for motion channel 0) works it out.
[[nodiscard]] std::optional<bool> at_frame(psprecomp::Runtime &runtime, std::uint32_t monster, float frame);
// During a part of behaviour: queues an attack of the base kind's (its number
// in the kind's attack table, below kMostAttackRows) in one of the monster's
// two attack slots (0 or 1), or the end of the attack in a slot. False outside
// a part, for a slot or row past those, or when the game's code is not as
// traced.
[[nodiscard]] bool attack(std::uint32_t monster, std::uint8_t slot, std::uint32_t number);
// During a part of behaviour: queues one of the kind's own effects (entries of
// its model pack), by number, on one of its joints, `offset` from it, at
// `scale`. False outside a part, when the kind's model set is not loaded, or
// when the game's code is not as traced.
[[nodiscard]] bool effect(std::uint32_t monster, std::uint32_t number, std::uint32_t joint, Vec3 offset, float scale);
[[nodiscard]] bool end_attack(std::uint32_t monster, std::uint8_t slot);

// Writes the added kinds' rows into game_task's tables once it has loaded
// (after tables::overlay_loaded() moved them).
void overlay_loaded(psprecomp::Runtime &runtime);

// The model pack a kind gets: the entries of `mod` that are present, over the
// entries of `base`, with the model (entry 1) given at least `meshes` meshes.
// With `effects`, the effect entries (3 and on) the mod leaves out come from
// that pack instead. The effects name the kind that owns them, and their
// models are looked up by it: those owned by `from` become `to`'s. Empty when
// a pack cannot be read.
[[nodiscard]] std::vector<std::uint8_t> compose_pack(std::span<const std::uint8_t> mod,
    std::span<const std::uint8_t> base, std::uint32_t meshes, std::span<const std::uint8_t> effects = {},
    std::uint8_t from = 0u, std::uint8_t to = 0u);

// Reads base model packs with `reader` instead of the game's own files, for
// tests; an empty function goes back to the game's.
void read_packs_with_for_tests(std::function<std::optional<std::vector<std::uint8_t>>(FileId)> reader);

// Forgets the kinds added, so tests can add again.
void reset_for_tests();

} // namespace mhp3rd::mods::monsters
