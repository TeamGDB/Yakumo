# PSPRecomp architecture

PSPRecomp has two layers: a reusable PSP execution framework and one or more title profiles.

## Framework

`psprecomp_core` provides:

- Allegrex/MIPS instruction decoding and architectural state;
- PSP ELF/PRX loading and relocation support;
- guest memory and EDRAM address handling;
- NID/import registration;
- generated-function registration and dispatch;
- an Allegrex interpreter used as a last-resort fallback for guest addresses the corpus does not cover;
- bounded generated-unit chaining;
- scheduler visibility boundaries;
- AOT hot-register and fast-memory support.

The root tools provide executable analysis and generic C++ generation. Nothing in the framework is supposed to require a specific game address or game asset.

## Profiles

A profile supplies everything needed to turn the framework into a native build for one title: generated guest code, HLE functions, title bootstrap, compatibility behavior, renderer/audio/input integration, native guest-leaf replacements and build packaging.

The profile boundary is intentional. Optimizations that are valid because of a measured address, ABI or data layout in one game stay in that game's profile even when they use reusable runtime APIs.

## Generated execution

Guest functions are emitted ahead of time as C++ and registered at their guest addresses. Generated units may chain directly when the runtime can prove that the target has not been replaced by an import/HLE/host override. Visibility boundaries materialize cached architectural state before host code or scheduling can inspect or replace the guest context.

## Code without generated functions: the JIT, then the interpreter

A corpus only contains the code that was visible when it was generated, so a title that
swaps overlays into a guest address window will eventually jump somewhere no generated
function claims, and a mod may rewrite code or bring its own. The outer dispatcher then
compiles that code from guest memory with the JIT (`include/psprecomp/jit.hpp`,
`src/jit.cpp` and `src/jit_compiler.cpp`, built on the vendored AsmJit) and registers the
result at its address like a generated unit. Each instruction keeps the meaning the
interpreter gives it (`src/allegrex_semantics.cpp`): the JIT emits common integer,
memory, branch and single-precision instructions itself and calls the shared definition
for the rest. Compiled code is dropped when its memory changes (an instruction cache
flush whose bytes differ) or when something else is registered over it; dropped code is
kept until the runtime ends, because a guest thread may still be inside it.

What the JIT cannot compile, or everything with `PSPRECOMP_NO_JIT=1`, is interpreted
instead of stopping, in bounded slices so scheduling and preemption still happen between
them. Reaching any registered address - an AOT unit, a host override or a PSP import
stub - leaves compiled or interpreted code through normal dispatch, so imports still run
their HLE wrappers and thread switching is unaffected. `PSPRECOMP_NO_INTERPRETER=1`
restores the strict stop, and the end-of-run report names the addresses that ran
interpreted.

A profile that layers its own code over registered functions can watch every
registration through `set_function_observer` (`include/psprecomp/function_observer.hpp`);
the mhp3rd profile's native mods use it to keep their hooks on top of host features
that take a hooked function over. It is a header of its own rather than part of
`runtime.hpp`, which every generated unit includes.

## Code that changes at run time

Some titles copy code into memory while they run — overlays swapped into a fixed address window, for example — so the corpus generated from the executable cannot cover it. The framework leaves the policy to the profile and gives it three levers:

- `set_runtime_missing_function_hook` is called when dispatch finds nothing at an address. The profile can register functions for whatever is loaded there and return true, and dispatch retries. Only if it declines does the interpreter take over.
- `set_runtime_unsupported_hook` is called when generated code reaches an instruction it cannot execute, which is what stale code looks like after the memory under it was replaced. Returning true retries at the same address.
- `Runtime::unregister_functions(start, end)` drops every generated function in a range, so code compiled for the previous contents of a window cannot run against the new ones.

A profile typically builds each piece of run-time code as its own shared library, identifies which one is loaded by hashing guest memory, and swaps libraries through these three calls. `profiles/mhp3rd/README.md` describes one such scheme in full.

## Native fast paths

`Runtime::register_native_fast_path(address, callback)` is the extension point for a profile to replace a measured guest leaf without adding title-specific code to `psprecomp_core`. Profile-generated code enters through `Runtime::invoke_native_fast_path()` and falls back to the generated function if no profile callback is registered.
