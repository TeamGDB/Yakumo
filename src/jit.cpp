// The JIT's public side (see psprecomp/jit.hpp): what was compiled for each
// runtime, registering it, dropping it when memory or ownership changes, and
// statistics. jit_compiler.cpp compiles.
#include "psprecomp/jit.hpp"

#include "jit_compiler.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <asmjit/core.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace psprecomp {
namespace {

using jit::Compiled;
using jit::kWindow;

std::optional<bool> g_enabled; // PSPRECOMP_NO_JIT, read when first asked
JitStats g_stats;
// Dropping compiled code unregisters it, which tells the JIT again.
bool g_dropping = false;

struct State {
    asmjit::JitRuntime jit;
    std::map<std::uint32_t, Compiled> functions; // by entry
    std::vector<Compiled> detached;              // compiled for hooks, registered nowhere
    std::set<std::uint32_t> hooked;              // addresses with an instruction hook
    // Entries whose code could not be compiled: not tried again on every
    // dispatch, only once the memory around them changed (drop() forgets them).
    std::set<std::uint32_t> failed;
    // Code dropped while the runtime runs is never freed before it ends:
    // a guest thread may be running it, waiting inside it, or about to
    // return into it, and a hook may still hold it.
    std::vector<Compiled> retired;
};

std::unordered_map<const Runtime *, std::unique_ptr<State>> &states() {
    static std::unordered_map<const Runtime *, std::unique_ptr<State>> map;
    return map;
}

State &state_of(Runtime &runtime) {
    std::unique_ptr<State> &slot = states()[&runtime];
    if (!slot) slot = std::make_unique<State>();
    return *slot;
}

} // namespace

bool jit_enabled() noexcept {
    if (!g_enabled) g_enabled = std::getenv("PSPRECOMP_NO_JIT") == nullptr;
    return *g_enabled;
}

void set_jit_enabled(bool enabled) noexcept {
    g_enabled = enabled;
}

bool jit_compile(Runtime &runtime, std::uint32_t pc) {
    if (!jit_enabled() || (pc & 3u) != 0u) return false;
    State &state = state_of(runtime);
    if (state.functions.contains(pc)) return false; // compiled and still missed: let the caller handle it
    if (state.failed.contains(pc)) return false;
    Compiled compiled;
    compiled.entry = pc;
    const jit::Compilation compilation = jit::compile(runtime, state.jit, pc, state.hooked, compiled);
    if (compilation.code == nullptr) {
        ++g_stats.failed;
        state.failed.insert(pc);
        if (!compilation.error.empty())
            std::cerr << "[jit] could not compile the code at " << hex32(pc) << ": " << compilation.error << "\n";
        return false;
    }
    compiled.code = compilation.code;
    ++g_stats.compiled;
    g_stats.instructions += compilation.instructions;
    g_stats.shared += compilation.shared;
    if (g_stats.compiled == 1u)
        std::cerr << "[jit] compiling guest code that has no recompiled function, first at " << hex32(pc) << "\n";
    if (jit::settings().trace)
        std::cerr << "[jit] " << hex32(pc) << ": " << compilation.instructions << " instructions, "
                  << compilation.shared << " shared, " << hex32(compiled.low) << "-" << hex32(compiled.high) << "\n";
    state.functions.emplace(pc, std::move(compiled));
    runtime.register_function(pc, reinterpret_cast<Runtime::RecompiledFunction>(compilation.code), "jit_" + hex32(pc));
    return true;
}

namespace {

bool overlaps(const Compiled &compiled, std::uint32_t start, std::uint32_t end) {
    return compiled.high > start && compiled.low < end;
}

bool source_changed(Runtime &runtime, const Compiled &compiled) {
    const std::uint32_t length = compiled.high - compiled.low;
    if (compiled.source.size() != length || !runtime.memory().contains(compiled.low, length)) return true;
    std::vector<std::uint8_t> now(length);
    runtime.memory().copy_out(compiled.low, now);
    return now != compiled.source;
}

// Drops what overlaps [start, end), or only what of it was compiled from
// bytes that have changed since.
void drop(Runtime &runtime, std::uint32_t start, std::uint32_t end, bool only_changed) {
    if (g_dropping || start >= end) return;
    const auto found = states().find(&runtime);
    if (found == states().end()) return;
    State &state = *found->second;
    g_dropping = true;
    // A function's code lies in its entry's 16 KiB window (and a delay slot
    // after it), so only entries that close can overlap.
    constexpr std::uint32_t kReach = kWindow + 8u;
    const std::uint32_t from = start > kReach ? start - kReach : 0u;
    const std::uint32_t to = end < 0xFFFFFFFFu - kReach ? end + kReach : 0xFFFFFFFFu;
    // An entry that could not be compiled may compile from the new bytes.
    state.failed.erase(state.failed.lower_bound(from), state.failed.lower_bound(to));
    for (auto it = state.functions.lower_bound(from); it != state.functions.end() && it->first < to;) {
        Compiled &compiled = it->second;
        if (!overlaps(compiled, start, end) || (only_changed && !source_changed(runtime, compiled))) {
            ++it;
            continue;
        }
        runtime.unregister_functions(compiled.entry, compiled.entry + 1u);
        ++g_stats.invalidated;
        state.retired.push_back(std::move(compiled));
        it = state.functions.erase(it);
    }
    for (auto it = state.detached.begin(); it != state.detached.end();) {
        if (!overlaps(*it, start, end) || (only_changed && !source_changed(runtime, *it))) {
            ++it;
            continue;
        }
        state.retired.push_back(std::move(*it));
        it = state.detached.erase(it);
    }
    g_dropping = false;
}

} // namespace

void jit_invalidate(Runtime &runtime, std::uint32_t start, std::uint32_t end) {
    drop(runtime, start, end, false);
}

void jit_memory_changed(Runtime &runtime, std::uint32_t start, std::uint32_t end) {
    drop(runtime, start, end, true);
}

bool jit_detached_current(const Runtime &runtime, JitFunction function) noexcept {
    const auto found = states().find(&runtime);
    if (found == states().end() || function == nullptr) return false;
    for (const Compiled &compiled : found->second->detached)
        if (reinterpret_cast<JitFunction>(compiled.code) == function) return true;
    return false;
}

void jit_forget(const Runtime &runtime) noexcept {
    states().erase(&runtime);
}

JitFunction jit_compile_detached(Runtime &runtime, std::uint32_t pc) {
    if ((pc & 3u) != 0u) return nullptr;
    State &state = state_of(runtime);
    Compiled compiled;
    compiled.entry = pc;
    const jit::Compilation compilation = jit::compile(runtime, state.jit, pc, state.hooked, compiled);
    if (compilation.code == nullptr) return nullptr;
    compiled.code = compilation.code;
    state.detached.push_back(std::move(compiled));
    return reinterpret_cast<JitFunction>(compilation.code);
}

void set_jit_instruction_hook(JitInstructionHook hook) noexcept {
    jit::set_instruction_hook(hook);
}

void jit_hook_instruction(Runtime &runtime, std::uint32_t address) {
    State &state = state_of(runtime);
    if (!state.hooked.insert(address).second) return;
    const std::uint32_t unit = address & ~(kWindow - 1u);
    // A jump or branch just before the address has it compiled in as its delay
    // slot: when that is the unit before's last word, that unit goes too.
    const bool slot_of_unit_before = address == unit && address >= 4u && runtime.memory().contains(address - 4u, 4u) &&
        decode_allegrex(runtime.memory().load32(address - 4u)).has_delay_slot();
    runtime.retire_generated_code(slot_of_unit_before ? unit - 4u : unit, unit + kWindow);
    jit_invalidate(runtime, address, address + 4u);
}

void jit_unhook_instruction(Runtime &runtime, std::uint32_t address) {
    State &state = state_of(runtime);
    if (state.hooked.erase(address) == 0u) return;
    jit_invalidate(runtime, address, address + 4u);
}

JitStats jit_stats() noexcept {
    JitStats stats = g_stats;
    stats.live = 0u;
    for (const auto &[runtime, state] : states()) {
        stats.live += state->functions.size();
        stats.retired += state->retired.size();
    }
    return stats;
}

void report_jit_stats() {
    const JitStats stats = jit_stats();
    if (stats.compiled == 0u && stats.failed == 0u) return;
    std::cerr << "[jit] compiled=" << stats.compiled << " instructions=" << stats.instructions
              << " shared=" << stats.shared << " invalidated=" << stats.invalidated << " failed=" << stats.failed
              << " live=" << stats.live << " retired=" << stats.retired << "\n";
}

} // namespace psprecomp
