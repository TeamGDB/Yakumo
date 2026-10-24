# Yakumo mod SDK

What a mod with code of its own is built against. [docs/MODDING.md](../../../docs/MODDING.md) is the guide to every kind of mod and to the API; this folder holds the files.

| Path | What it is |
| --- | --- |
| `include/yakumo/mod.h` | The native mod API: plain C, versioned, the only header a native mod needs |
| `include/yakumo/mod.hpp` | A header-only C++ layer over it: typed memory access, service lookups, and the functions taking arrays with their strides filled in |
| `templates/native/` | A native mod to copy: CMake project, `mod.ini` and a GitHub Actions workflow that builds it for Windows, Linux, macOS and Android and packs a `.ymod` |
| `templates/script/` | The same mod as a Luau script, which needs no compiler |
| `symbols.txt` | The game's known functions and data by name, which mods look up at run time instead of carrying addresses |

## A native mod from the template

1. Copy `templates/native/` to a folder or repository of your own, and point `YAKUMO_SDK_DIR` at a copy of `include/`.
2. Rename the project and `MOD_LIBRARY` in `CMakeLists.txt`, and give `[Native] Library` in `mod.ini` the same name. Keep `YAKUMO_MOD_API_VERSION();` in one source file: it tells Yakumo which interface the mod was built for.
3. Build and install into your mods folder:

   ```sh
   cmake -S . -B build -DYAKUMO_SDK_DIR=path/to/include
   cmake --build build --config Release
   cmake --install build --config Release --prefix "<mods folder>/My Mod"
   ```

4. Start Yakumo, turn the mod on in **Mods**, and restart when asked. What the mod logs is on its page and in Yakumo's log.

A native mod runs on the platforms it has a library for: `native/<platform>/<Library>.<dll|dylib|so>`, with the platform one of `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, `macos-x64` and `android-arm64`. The template's workflow builds four of them in one run; push the template to GitHub and download the `.ymod` from the run.

Native code runs with the player's rights, like any program they install. Players are told so on the mod's page; publish the source with the mod.

## Compatibility

`YAKUMO_API_VERSION` and each service's version only grow. A service keeps its functions in place and adds new ones at the end, and its `size` says how many the host has. A mod built against an older SDK keeps working with a newer Yakumo. A mod that needs a newer service gets NULL from `service()` on an older Yakumo, and decides itself whether it can run without it. The structs a mod passes grow at their end too: one passed on its own starts with its `size`, and an array comes with its stride, the bytes from one element to the next ("Sizes and strides" in `mod.h`). In C++ they are filled in; a C mod sets them to the `sizeof` of its structs.
