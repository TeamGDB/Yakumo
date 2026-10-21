#pragma once

// Inside the native mod host (mod_native*.cpp): its state, shared by the
// files that implement the services in sdk/include/yakumo/mod.h. Nothing
// else includes this.

#include "mods/mhp3rd_game_events.hpp"
#include "mods/mod_native.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include "yakumo/mod.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The mod as the host knows it; opaque to mods.
struct ymod_mod {
    std::string id;
    std::string name;
    std::string folder;
    void *library{};
    std::vector<std::uint32_t> allocations;        // kernel block uids
    std::map<std::string, std::int64_t> fallbacks; // of the settings it declared
    bool failed{};                                 // refused to start or stopped: nothing of it is called
};

namespace mhp3rd::mods::native {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

// A callback sees the guest registers in the AllegrexContext itself.
static_assert(offsetof(AllegrexContext, gpr) == offsetof(ymod_cpu, gpr));
static_assert(offsetof(AllegrexContext, hi) == offsetof(ymod_cpu, hi));
static_assert(offsetof(AllegrexContext, lo) == offsetof(ymod_cpu, lo));
static_assert(offsetof(AllegrexContext, pc) == offsetof(ymod_cpu, pc));
static_assert(offsetof(AllegrexContext, fpr) == offsetof(ymod_cpu, fpr));
static_assert(offsetof(AllegrexContext, fcr31) == offsetof(ymod_cpu, fcr31));

inline ymod_cpu *as_cpu(AllegrexContext &ctx) {
    return reinterpret_cast<ymod_cpu *>(&ctx);
}

struct Hook {
    ymod_hook id{};
    ymod_mod *mod{};
    int kind{};
    ymod_hook_fn callback{};
    void *user{};
};

// A guest address with function hooks: the hooks take over its entry.
struct Site {
    std::string overlay; // empty: the executable
    std::vector<Hook> hooks;
    psprecomp::JitFunction original{};
    // A host feature that replaces the game's function there (layered armor,
    // the camera): the hooks wrap it instead of the game's code.
    Runtime::RecompiledFunction host{};
    bool installed{};
    bool refused{}; // the code there is not the known function; said once
};

struct InstructionSite {
    std::string overlay;
    std::vector<Hook> hooks;
    bool installed{};
};

struct Return {
    std::uint32_t ra{};
    std::uint32_t address{};
};

struct Subscriber {
    ymod_mod *mod{};
    int event{};
    ymod_event_fn callback{};
    void *user{};
};

struct Service {
    ymod_mod *owner{};
    std::uint32_t version{};
    const void *table{};
};

struct Drawing {
    ymod_mod *mod{};
    std::string title;
    ymod_draw_fn draw{};
    void *user{};
    bool overlay{};
};

struct State {
    Runtime *runtime{};
    std::vector<std::unique_ptr<ymod_mod>> mods;
    std::map<std::uint32_t, Site> sites;
    std::map<std::uint32_t, InstructionSite> instructions;
    std::map<ymod_hook, std::pair<std::uint32_t, bool>> hook_sites; // id -> address, instruction?
    ymod_hook next_hook{1u};
    std::unordered_map<std::int32_t, std::vector<Return>> returns; // per guest thread
    std::uint32_t trampoline{};
    std::vector<Subscriber> subscribers;
    std::map<std::string, Service> services;
    std::vector<Report> reports;
    std::vector<Drawing> drawings;
    p3rd::GameWatch game; // overlays leaving, quests beginning and ending
    bool started{};
    bool call_pending{};
    bool replacing{}; // inside a REPLACE callback: the only place guest calls work
};

State &state();
void report(ymod_mod *mod, const std::string &line, bool problem);

// mod_native_hooks.cpp: hooks in place for the overlays loaded now; the code
// AFTER hooks return through; instruction hooks from compiled code; the
// hooks and calls services.
void reconcile();
void hooks_remove(ymod_mod *mod, ymod_hook hook);
void hook_return(Runtime &runtime, AllegrexContext &ctx);
void instruction_dispatch(Runtime &runtime, AllegrexContext &ctx, std::uint32_t address);
extern const ymod_hooks kHooks;
extern const ymod_calls kCalls;

// mod_native_services.cpp: the other services, and events to subscribers.
void fire(int event, std::uint32_t value);
extern const ymod_memory kMemory;
extern const ymod_events kEvents;
extern const ymod_files kFiles;
extern const ymod_settings kSettings;
extern const ymod_services kServices;
extern const ymod_symbols kSymbols;
extern const ymod_ui kUi;
extern const ymod_tables kTables;
extern const ymod_monsters kMonsters;
extern const ymod_quests kQuests;
extern const ymod_equipment kEquipment;

} // namespace mhp3rd::mods::native
