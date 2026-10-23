#pragma once

#include <cstddef>
#include <cstdint>

namespace psprecomp {

class Runtime;

// Native code for guest code the ahead-of-time corpus does not cover: code a
// mod rewrote (its generated unit was retired), a mod's own code, or a code
// overlay without a prebuilt library. Instead of interpreting it, the runtime
// compiles it from guest memory, as it is at that moment, to x86-64 or
// AArch64 code and registers the result at its entry address like a
// generated unit: dispatch, overlays and imports see no difference.
//
// A compiled function enters at one guest address and runs the code reachable
// from it inside the same 16 KiB window, with native branches, until control
// leaves that code, reaches an address something else is registered at (an
// import, a host function, a generated unit) or the runtime stops. It then
// sets ctx.pc and returns. It also returns every 1024 branches back (a loop
// going round), at the loop's head, so that a loop that never calls out still
// reaches the dispatcher, where the scheduler can switch threads
// (PSPRECOMP_JIT_NO_PREEMPTION=1 lets loops run until they leave). Each instruction has the meaning the interpreter
// gives it: what the JIT does not emit itself, it runs through the same shared
// function.
//
// PSPRECOMP_NO_JIT=1 turns it off; the interpreter then runs that code as
// before.

[[nodiscard]] bool jit_enabled() noexcept;
void set_jit_enabled(bool enabled) noexcept;

// Compiles the guest code at `pc` and registers it there. False when the JIT
// is off or the code cannot be compiled (the caller then interprets it); code
// that could not be compiled is not tried again until the memory around it
// changes (jit_invalidate(), jit_memory_changed()).
bool jit_compile(Runtime &runtime, std::uint32_t pc);

// Drops compiled code that read guest memory in [start, end): something
// else now owns an address in it, or the memory changed.
//
// Dropped code is unregistered but not freed until the runtime is replaced
// (jit_forget): a guest thread may be running it or waiting inside it, and a
// mod's callback may have dropped the very code that called it.
void jit_invalidate(Runtime &runtime, std::uint32_t start, std::uint32_t end);

// The guest may have rewritten [start, end) (an instruction cache flush):
// drops the compiled code there whose guest bytes are no longer the ones it
// was compiled from, and keeps the rest.
void jit_memory_changed(Runtime &runtime, std::uint32_t start, std::uint32_t end);

// Frees everything compiled for the runtime that lived at this address. A
// new Runtime calls it, so that a runtime built where an old one was does
// not inherit its code.
void jit_forget(const Runtime &runtime) noexcept;

// Native code for the guest code at `pc` that is registered nowhere: it runs
// from `pc` as a compiled function does and leaves through ctx.pc. A hook that
// took over a function's entry runs the function's own first instructions
// with it; from the next registered address on, the generated code runs as
// before. Null when the code cannot be compiled.
//
// The code stays callable for the runtime's life, but once jit_invalidate()
// or jit_memory_changed() covers it, it is no longer the guest code there:
// jit_detached_current() says so, and the owner compiles it again.
using JitFunction = void (*)(Runtime &, struct AllegrexContext &);
JitFunction jit_compile_detached(Runtime &runtime, std::uint32_t pc);
[[nodiscard]] bool jit_detached_current(const Runtime &runtime, JitFunction function) noexcept;

// Host code compiled into native code just before the guest instruction at
// an address, with ctx.pc set to that address: the instruction hooks of
// native mods. One function serves every hooked address. Adding an address
// drops the generated code around it (its 16 KiB unit) and what the JIT had
// compiled there, so the next run of that code is compiled with the call.
using JitInstructionHook = void (*)(Runtime &, struct AllegrexContext &, std::uint32_t address);
void set_jit_instruction_hook(JitInstructionHook hook) noexcept;
void jit_hook_instruction(Runtime &runtime, std::uint32_t address);
void jit_unhook_instruction(Runtime &runtime, std::uint32_t address);

struct JitStats {
    std::uint64_t compiled{};     // functions compiled
    std::uint64_t instructions{}; // guest instructions in them
    std::uint64_t shared{};       // of those, run through the shared semantic function
    std::uint64_t invalidated{};  // functions dropped
    std::size_t retired{};        // dropped functions and detached code kept until the runtime ends
    std::uint64_t failed{};       // compilations that failed
    std::size_t live{};           // functions registered now
};
[[nodiscard]] JitStats jit_stats() noexcept;
void report_jit_stats();

} // namespace psprecomp
