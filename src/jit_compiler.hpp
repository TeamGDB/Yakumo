#pragma once

// The JIT's compiler: one guest function from memory to native code
// (jit_compiler.cpp). jit.cpp keeps what it compiled, registers it and drops
// it again; see psprecomp/jit.hpp.

#include "psprecomp/decoder.hpp"
#include "psprecomp/jit.hpp"

#include <asmjit/core.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace psprecomp {

class Runtime;

namespace jit {

// A compiled function runs the code reachable from its entry inside the
// entry's window.
constexpr std::uint32_t kWindow = 0x4000u;

struct Compiled {
    std::uint32_t entry{};
    std::uint32_t low{}; // guest bytes the code was compiled from: [low, high)
    std::uint32_t high{};
    void *code{};
    std::vector<std::unique_ptr<DecodedInstruction>> decoded; // read by the shared-instruction calls
    std::vector<std::uint8_t> source;                         // the guest bytes in [low, high) it was compiled from
};

// PSPRECOMP_JIT_LOG, _TRACE, the off switches of the two optimisations,
// _NO_REGISTER_CACHE and _NO_NATIVE_FPU, to compare against, and
// _NO_PREEMPTION, which lets a loop run until it leaves, as before loops
// returned to the dispatcher.
struct Settings {
    bool log{};
    bool trace{};
    bool cache_registers{true};
    bool native_fpu{true};
    bool preempt{true};
};

// How many times compiled code jumps back (goes round a loop) before it
// returns to the dispatcher, which counts that as a dispatch and lets the
// scheduler switch threads: about the interpreter's 1024 instructions a run
// for a short loop.
constexpr std::uint32_t kBackEdges = 1024u;
[[nodiscard]] const Settings &settings();

struct Compilation {
    void *code{}; // null when the code could not be compiled
    std::size_t instructions{};
    std::size_t shared{}; // of those, run through the shared semantic function
    std::string error;    // why the emitter failed; empty when the code was not there to compile
};

// Compiles the guest code at `entry` into `jit`, with calls to the
// instruction hook before the addresses in `hooked`; fills in `out`.
[[nodiscard]] Compilation compile(Runtime &runtime, asmjit::JitRuntime &jit, std::uint32_t entry,
    const std::set<std::uint32_t> &hooked, Compiled &out);

// What compiled code calls before a hooked instruction.
void set_instruction_hook(JitInstructionHook hook) noexcept;

} // namespace jit
} // namespace psprecomp
