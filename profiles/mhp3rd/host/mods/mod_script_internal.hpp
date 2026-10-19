#pragma once

// Inside the script mod host (mod_script*.cpp): a script's state and what
// the yakumo.* functions share with the core. Nothing else includes this.

#include "mods/mod_native.hpp"
#include "mods/mod_script.hpp"

#include "yakumo/mod.h"

#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace mhp3rd::mods::scripts {

using Clock = std::chrono::steady_clock;
constexpr auto kCallBudget = std::chrono::milliseconds(5);

struct Script {
    ymod_mod *mod{};
    const ymod_host *host{};
    const ymod_memory *memory{};
    const ymod_hooks *hooks{};
    const ymod_events *events{};
    const ymod_files *files{};
    const ymod_settings *settings{};
    const ymod_symbols *symbols{};
    const ymod_ui *ui{};
    lua_State *state{};
    lua_State *thread{};                           // the sandboxed thread everything runs on
    std::size_t used{};                            // bytes the script's Luau state holds
    std::map<std::string, char> setting_kinds;     // 's'witch, 'n'umber, 'c'hoice, as declared
    std::map<ymod_hook, struct Callback *> hooked; // the script's hooks in place, and their callbacks
    int widgets{};                                 // ref of the table draw callbacks get
    bool failed{};
};

// A Luau function a hook or an event calls.
struct Callback {
    Script *script{};
    int ref{};
};

void log(Script &script, const std::string &line, int level = YMOD_LOG_INFO);
// Runs what is on the thread's stack; on an error the script stops being called.
bool call(Script &script, int arguments, int results, Clock::duration budget = kCallBudget);
// The script a yakumo.* function belongs to (its upvalue).
Script &self(lua_State *state);
std::uint32_t address_argument(lua_State *state, int index);
std::uint32_t word_argument(lua_State *state, int index);

// Callbacks: Luau functions hooks, events and draws call.
Callback *keep(lua_State *state, int index);
void release(Callback *callback);
void push_hook(lua_State *state, Script &script, ymod_hook id, Callback *callback);
int hook_callback(ymod_cpu *cpu, void *user);
void event_callback(int event, std::uint32_t value, void *user);
void draw_callback(const ymod_widgets *ui, void *user);
// The interface's widgets, while a panel or an overlay draws.
extern const ymod_widgets *g_widgets;

// mod_script_api.cpp: the global table yakumo and the widgets table.
void install_api(Script &script);

} // namespace mhp3rd::mods::scripts
