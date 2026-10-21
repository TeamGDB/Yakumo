# AsmJit

Machine code emitter used by the runtime's JIT (`src/jit.cpp`), which compiles guest code that changed after recompilation, such as code a mod rewrote, to native x86-64 or AArch64 code.

- Upstream: https://github.com/asmjit/asmjit
- Commit: `0464e1c8f1f43c861a0c73d4e59437fff255cdd9` (2026-10-16)
- Licence: Zlib, in `LICENSE.md`
- Contents: the `asmjit/` source directory of that commit, unmodified, including the unit tests kept beside the sources (`*_test.cpp`) and the debugger visualiser `asmjit.natvis`, which are not built. Upstream's top-level test suite, tools, instruction database and build files are not included; `CMakeLists.txt` here is this project's own and builds only the backend of the host machine.
