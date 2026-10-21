# Luau

The scripting language of script mods (`host/mods/script_mods.cpp`).

- Upstream: https://github.com/luau-lang/luau
- Release: `0.743`, commit `9d7748a4f324213a18cb8283fa2a77c330225fa0`
- Licence: MIT, in `LICENSE.txt`; the Lua 5.1 code Luau derives from is under `lua_LICENSE.txt`
- Contents: the `Common`, `Ast`, `Bytecode`, `Compiler` and `VM` directories of that release, unmodified. The analysis, code generation, CLI, tests and upstream build files are not included; `CMakeLists.txt` here is this project's own.
