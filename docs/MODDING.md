# Making mods

This guide is for people who make mods for Yakumo. A mod is a folder in the mods folder, described by a `mod.ini`. Players turn mods on and off on the Mods page of the menu; the [profile README](../profiles/mhp3rd/README.md#mods) covers that side, the mods folder's location and the community's existing mods.

Yakumo reads two formats, and both run on the same machinery:

- **Yakumo's own format** (`Type="Yakumo"`), described here. One mod can replace, patch and add files, change memory, add code, and run native code or a script of its own. Use it for new mods.
- **The community's format** for the mhp3reload loader and its mod manager (`Type="File"`, `"Patch"`, `"Code"`, `"Pack"`, `"PseudoPack"`, `"Equip…"`). Existing mods work unchanged; [Mods made for the mod manager](#mods-made-for-the-mod-manager) maps their keys to this format.

## What a mod can change

| What | How | When it applies |
| --- | --- | --- |
| A file of `DATA.BIN`: models, textures, quests, text, code overlays | `[Files]` replaces it, at any size; `[Patch <file>]` and `[Patches]` change bytes in it | The next time the game loads the file. A file that grows applies at the next start |
| A new file, for new content | `[Add]` gives it a new id | At the next start |
| Guest memory: the game's data and its code | `[Memory]` edits and `[Code]` images, written before the game's first instruction | At the next start |
| The game's code inside a code overlay | `[Patch <overlay file>]` with guest addresses | Each time the overlay loads |
| Anything, while the game runs | A native library (`[Native]`) or a Luau script (`[Script]`) that hooks the game's functions, reads and writes memory, answers events and offers settings | At the next start |

Pick the simplest tier that does the job. Files and memory edits need no tools beyond a hex editor. A script needs no compiler and runs on every platform as it is. A native library is for work a script is too slow for, and must be built for each platform.

### How changed code runs

The game runs as code recompiled ahead of time, so its instructions are never read from guest memory. When a mod changes the game's code, Yakumo retires the recompiled code around the change, a 16 KiB stretch called a *unit*. Yakumo's JIT then compiles those addresses again from memory, mod's instructions included, to native x86-64 or AArch64 code. A mod's own code in memory is compiled the same way. Inside code overlays, Yakumo installs the overlay's recompiled code from its original bytes first and then applies the mod's changes, so only the units they touch are recompiled.

The JIT keeps the guest registers a function uses in host registers for the whole function and runs single-precision arithmetic, moves and compares as native floating-point instructions; the VFPU and a few rarer instructions go through the interpreter's own definition. Its code is still less optimised than the ahead-of-time code. Keep code changes out of code the game runs thousands of times a frame, and check the speed with `MHP3RD_PERF=log`: it must stay at 100%. `PSPRECOMP_NO_JIT=1` interprets that code instead, for comparison; the interpreter is much slower. `PSPRECOMP_JIT_NO_REGISTER_CACHE=1` and `PSPRECOMP_JIT_NO_NATIVE_FPU=1` turn the two optimisations off, to tell whether one of them is at fault. A loop inside compiled code returns to Yakumo's dispatcher every 1024 times round, where the game's threads can take turns, as they do around the interpreter's runs; `PSPRECOMP_JIT_NO_PREEMPTION=1` lets a loop run until it leaves instead, as the game's recompiled code does. Code that waits on a flag should still call something that yields (a kernel wait or a delay): spinning, it uses its thread's time until the scheduler takes it. An instruction hook sees and may change the registers, but cannot send the game elsewhere; a function hook can.

## A first mod

```
mods/
  brighter_title/
    mod.ini
    preview.png        optional, shown on the Mods page
    title.tm2          a replacement file
```

```ini
[MOD INFO]
Name="Brighter title screen"
Author="You"
Type="Yakumo"
ModVersion="1.0"
Description="First line.\Second line."

[Files]
0601 = title.tm2
```

Put the folder in the mods folder (or point `MHP3RD_MODS_DIR` at a folder of your own while you work), open the Mods page, choose *Read the Folder Again* and turn the mod on. The log says what the mods change:

```
[mods] 1 file(s) replaced, 0 patched
```

## Finding what to change

- **Which file.** A file id is the index of an entry in `DATA.BIN`, written as four hex digits ([DATA_BIN.md](DATA_BIN.md#file-ids-and-mods)). With any mod on, `MHP3RD_TRACE_DATA_BIN=1` logs the id of every file the game reads; make the game load the thing you want to change and note the ids. [EQUIPMENT_MODS.md](EQUIPMENT_MODS.md) explains how equipment maps to files.
- **The file itself.** `profiles/mhp3rd/tools/databin.py` lists and extracts entries from your own disc image: `databin.py disc.iso extract out 0x0601` writes `out/01537.bin` (the name is the id in decimal). `databin.py disc.iso list --overlays` lists the code overlays with their load addresses.
- **An address in memory.** The Debug page's command file ([DEBUG_MENU.md](DEBUG_MENU.md#the-command-file)) has `find8`/`find16`/`find32` and `next` to narrow a value down, `peek` to read memory, `poke` to try a change in the running game, and `dump` to save memory for comparing offline. Addresses are for NPJB-40001, the one executable Yakumo supports.
- **Code.** The game's code starts at `0x08804000`; code overlays load at the addresses their headers give. Disassemble the extracted executable or overlay with the tool you prefer.
- **Known functions and data.** [`sdk/symbols.txt`](../profiles/mhp3rd/sdk/symbols.txt) names what has been traced so far: see [The game's functions by name](#the-games-functions-by-name).
- **What a function does.** A script with a hook that logs its arguments answers that in the running game, without a debugger: see [Script mods](#script-mods). [DATA_BIN.md](DATA_BIN.md#files-mods-add) shows such a trace of the file loader.

## mod.ini reference

`mod.ini` is an INI file: `[section]` headers and `key = value` lines. Section names and keys are not case-sensitive. A line starting with `;` or `#` is a comment, and in the edit sections (`[Memory]`, `[Patch <file>]`) a `;` or `#` after a value starts a comment too. Values may be quoted. File names are relative to the mod's folder, may use `/` or `\`, and are matched without regard to case. A name that leads out of the folder (a `..` part, a drive or a root, or a link pointing outside) is not found.

### [MOD INFO]

| Key | Meaning |
| --- | --- |
| `Type` | `Yakumo` |
| `Name` | Shown on the Mods page; the folder's name when missing |
| `Author` | Shown on the Mods page |
| `Description` | Shown on the mod's page; `\` starts a new line |
| `ModVersion` | Your mod's own version, shown on its page |
| `Format` | The version of this format the mod is written for. Default `1`. A Yakumo that reads an older format refuses the mod and says so |
| `Version` | The game release: `HD` or `BOTH`, or leave it out. Any other value (`NOHD`) refuses the mod. Yakumo runs only the HD release |
| `Depends` | Folder names of mods turned on with this one, separated by `;`. Their native libraries and scripts start before this mod's, so it can import the services they publish |
| `Priority` | 0 to 5, default 3: the order in which mods' code and memory are written, and native libraries and scripts start (after the mods they depend on), 0 first. Where two mods write the same bytes, the one written last wins |

### [Files]: replacing files

```ini
[Files]
0601 = armour.pac
05D2 = models/helm.pac
```

Each line replaces the file with that id with a file from the mod. A replacement may be larger or smaller than the original. Where two mods replace the same file, the one higher on the Mods page wins.

### [Add]: new files

```ini
[Add]
2000 = monster/model.pac
2001 = monster/textures.tm2
```

Each line adds a file under a new id, from `179B` (the disc's 6,043 files are `0000` to `179A`) to `FFFE`, except `6000` to `759F`, which serve the quests mods add. The game loads it like one of its own once something asks for that id: a table the mod patches, or its code. Choose ids well away from `179B` that other mods are unlikely to use; two mods adding the same id conflict like two replacing one file.

Added files are served for whole-file loads, the way the game loads models, textures, quests and overlays. Streamed music and movies are read a piece at a time and cannot be added. [DATA_BIN.md](DATA_BIN.md#files-mods-add) explains how they are served.

### [Quests]: new quests

```ini
[Quests]
62001 = quests/tigrex_peaks.bin
```

Each line adds a quest under an id from 62000 to 65535. The file is the quest as the game holds it once decrypted, and its record must carry the same id; Yakumo encrypts it with the game's keys and serves it. Make one from a quest of your own disc image with `profiles/mhp3rd/tools/quests.py`:

```sh
python3 profiles/mhp3rd/tools/quests.py extract game.iso EBOOT.ELF 204 quest.bin
python3 profiles/mhp3rd/tools/quests.py edit quest.bin tigrex_peaks.bin --id 62001 --name "Tigrex Peaks" --monster 42=58
```

`--monster OLD=NEW` changes a large monster's kind wherever the quest names it: the kinds it loads, its spawn records, the objective and the main monsters. The game takes ids above 59999 for event quests, and mods' quests join its own at the Guild Hall's counter: *Event Quest*, *Yes* to show the included bonus content, then *Low Rank Quest* or *High Rank Quest*. They are accepted and played there like the game's; the developer tools also list them, and `quest start 62001` departs on one from the village. Two mods adding the same id conflict like two replacing one file. [QUESTS.md](QUESTS.md) describes the quest file and how the game finds and decrypts it.

### [Patch <file>]: changing bytes in a file

```ini
[Patch 0702]
0x10 = 01 02 03 04
0x20 = 00 00 -> 10 27
```

Each line changes bytes of file `0702` as the game loads it. The left side is an address, in hex. It means what it means in a patch file of the mod manager's format:

- In a code overlay, a guest address in the overlay's data changes those bytes of the overlay; one in its code is written to memory right after the overlay loads, and only the units it changes are compiled again.
- In a code overlay, a guest address outside its image is written to memory once the overlay has loaded.
- In any file, an address smaller than the file's size is an offset into the file.

`[Patches]` applies patch files in the mod manager's binary format instead: `0702 = fix.bin`. Patches never change a file's size. Patches from several mods apply in turn, lowest on the Mods page first, on top of the winning replacement.

### [Memory]: changing memory at start

```ini
[Memory]
0x0891A2B0 = 0x00000000
0x08A00000 = 27BDFFF0 -> 27BD0000 ; only if the old bytes are there
```

Each line writes bytes to a guest address after the executable is loaded and before its first instruction runs. Use it for the game's data and for small changes to its code. Lines are written in order; the later one wins where two overlap.

### [Code]: adding code

```ini
[Code]
0x08B00000 = hook.bin
Files = "blocks.bin"
```

`address = file` loads a raw image, the bytes of the file as they are, at that address: what an assembler writes when told where the code goes. Images up to 8 MiB are accepted. `Files` lists files in the community loader's block formats: plain blocks (address, length, bytes, ended by `FFFFFFFF`), or typed blocks, where a main block of code with no address is placed in free memory by Yakumo and hooks jump into it. A block whose length has its top bit set is code that runs as it loads: Yakumo calls it on the game's thread at the game's first code load. Block files are written first, then images, then the `[Memory]` lines.

### [Native]: a native library

```ini
[Native]
Library = my_mod
```

The library is `native/<platform>/my_mod.<dll|dylib|so>` in the mod's folder, with `<platform>` one of `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, `macos-x64` and `android-arm64`. A mod without a library for the running platform cannot be turned on there, and its page says which file it lacks. See [Native mods](#native-mods).

### [Script]: a Luau script

```ini
[Script]
File = main.luau
```

See [Script mods](#script-mods).

### Writing bytes

Values are bytes separated by spaces:

| Written | Bytes |
| --- | --- |
| `27 BD FF F0` or `27BDFFF0` | `27 BD FF F0`: hex bytes in the order written, as a hex editor shows them |
| `0x24020001` | `01 00 02 24`: with `0x`, a 32-bit number in the guest's byte order (little-endian). MIPS instructions are written this way |
| `0x1234`, `0xAB` | `34 12` and `AB`: a 16-bit number and a byte |

A `0x` number must have exactly 2, 4 or 8 digits. `expected -> new` writes the new bytes only where the expected ones are there now, and both sides must be the same length. Use it for every change to the game's code: a mod written for different bytes then does nothing instead of corrupting the game, and the Mods page says what it found.

## Writing code

A code mod usually puts a function of its own somewhere in memory and changes one instruction of the game's to call it. With an assembler that writes raw images, such as armips:

```asm
.psp
.create "hook.bin", 0x08B00000
my_function:
    ; your code; preserve what the caller expects, then return
    jr      ra
    nop
.close
```

```ini
[Code]
0x08B00000 = hook.bin

[Memory]
; The game's instruction at 0x0880ABC0 becomes jal 0x08B00000, written only
; where the instruction the mod was made for (here 0x0C123456) is there
0x0880ABC0 = 0x0C123456 -> 0x0E2C0000
```

A `jal` to address `A` is `0x0C000000 | ((A >> 2) & 0x03FFFFFF)`; a `j` is the same with `0x08000000`. Remember the branch delay slot.

**Where to put your code.** Yakumo reserves the memory a mod writes to before the game starts, so the game's allocator cannot hand it out. Memory that is already in use cannot be reserved, and the Mods page lists such a block as a problem. At the title screen, user memory from about `0x0A2E6000` up to the thread stacks near `0x0BFB8000` was free; how much of it the game allocates during a hunt has not been measured, so test your mod in a quest. Changes inside the executable's own image (its code and data) need no reserving, and neither does the 16 KiB just below it, from `0x08800010` to `0x08803FFF`, where published code mods keep their code; Yakumo never lets a mod write `0x08800000` to `0x0880000F`, which hold its own code. Native and script mods ask for memory instead (`allocate`), which is always safe.

**Functions Yakumo replaces.** A few of the game's functions run as Yakumo's own code, such as parts of the HUD drawing, the camera's rotation helper, the layered armor model lookup and a font helper. A change to their instructions cannot take effect, and the Mods page says so. Hooks from native and script mods are different: they wrap whatever runs there, Yakumo's code included, and stay in place when Yakumo switches a function over or back. When the last hook on such a function is removed, Yakumo's code takes it back.

## Script mods

A script mod is a [Luau](https://luau.org) script that Yakumo runs as it is, on every platform. Luau is Lua 5.1 with types and a faster VM, made for running untrusted code. `profiles/mhp3rd/sdk/templates/script/` is a complete script mod to start from.

```lua
-- Log the armor model each part of the hunter uses, while a setting is on.
local logging = yakumo.setting_switch("log", "Log Armor Models", true)
local part = 0

yakumo.hook("armor.model_of_part", "before", function(cpu) part = cpu.a2 end)
yakumo.hook("armor.model_of_part", "after", function(cpu)
    if logging then yakumo.log("part " .. part .. " uses model " .. cpu.v0) end
end)
yakumo.on("frame", function() logging = yakumo.setting("log") end)
```

The script's main chunk runs once at start, before the game's first frame; it sets up hooks and events, which do the work from then on. Everything goes through the global table `yakumo`:

| Function | What it does |
| --- | --- |
| `log(...)` | Writes its arguments, separated by spaces, to Yakumo's log and the mod's page |
| `read8/16/32(address)`, `readfloat(address)` | Guest memory; `nil` for an address outside it |
| `readbytes(address, length)` | A string of up to 1 MiB of guest memory, or `nil` |
| `write8/16/32(address, value)`, `writefloat`, `writebytes(address, string)` | Writes guest memory; `false` outside it |
| `allocate(size)` | Guest memory reserved for the mod from the game's allocator: its address, or `nil` |
| `code_changed(address, length)` | Tells the JIT that instructions there were rewritten |
| `hook(target, kind, function [, overlay])` | Runs `function(cpu)` at a guest function's entry. `target` is an address or a [known function's name](#the-games-functions-by-name) (`"armor.model_of_part"`), which brings its overlay with it. `kind` is `"before"`, `"after"` (when it returns, `v0`/`v1` hold its result) or `"replace"` (return `true` to return to the caller with what you set; anything else runs the game's function after all). With `overlay` (`"game_task"`), the hook is there whenever that overlay is loaded. Returns a hook number, or `nil` when the address cannot be hooked or the name is not known |
| `hook_instruction(target, function [, overlay])` | Runs `function(cpu)` just before the instruction at `target`, anywhere in a function. On a jump or a branch, the instruction in its delay slot has not run yet: an argument it sets still holds its old value. Only code the JIT compiles runs instruction hooks: with `PSPRECOMP_NO_JIT=1` this returns `nil` |
| `unhook(number)` | Removes a hook the script added |
| `symbol(name)` | A known function's or data's address and its overlay (or `nil` for the executable); `nil` when the name is not known |
| `symbol_name(address)` | The name of what is known at exactly `address`, or `nil` |
| `overlay_at(address)` | The code overlay loaded now over `address`, by name, and its load address; `nil` when none is |
| `table(name)` | A [game table](#game-tables)'s address now, its rows and its bytes per row; `nil` when the name is not known |
| `extend_table(name, rows)` | Gives a game table at least `rows` rows: its new address, or `nil` (the reason is in the log) |
| `add_monster{id, based_on, model, geometry, motions}` | Adds a [large monster](#large-monsters) from the mod's files: its kind's number, or `nil` (the reason is in the log) |
| `monster_kind(id)` | A large monster's kind by id, the game's (`"tigrex"`) or a mod's; `nil` when nothing has the id |
| `on(event, function)` | `"frame"` between frames, `"start"` before the first, `"file"` when the game has read a whole file (with its id), `"overlay"` when a code overlay is copied to its slot and `"overlay_unloaded"` when it leaves it (with its load address), `"quest_started"` and `"quest_ended"` (with the quest's id) |
| `panel(title, function)`, `overlay(title, function)` | Draws in Yakumo's interface: see [Panels and overlays](#panels-and-overlays) |
| `setting_switch(key, label, fallback)`, `setting_number(key, label, min, max, fallback)`, `setting_choice(key, label, {choices}, fallback)` | Declares a setting on the mod's page and returns its value. Choices count from 1 |
| `setting(key)` | A declared setting's value now: the player can change it at any time |
| `replace_file(id, bytes)` | Replaces a file from the next time the game loads it, until the game ends: `false` (the reason in the log) for an id the disc does not have or a change the running game cannot take, such as a file that outgrows its blocks in `DATA.BIN`. A file that grows is replaced through `[Files]` |
| `original_file(id)` | The game's own bytes of a file, or `nil` |
| `folder()`, `api_version` | The mod's folder, for its own files; the API's version |

`cpu` names the registers as the assembler does: `zero`, `at`, `v0`–`v1`, `a0`–`a3`, `t0`–`t9`, `s0`–`s8`, `k0`–`k1`, `gp`, `sp`, `fp`, `ra`, plus `hi`, `lo`, `pc`, `r0`–`r31` and the FPU's `f0`–`f31`. Writing a register changes it for the game; writing `pc` sends the game there. `cpu` exists only while its callback runs.

A script runs in a sandbox: no files, processes or network, the standard libraries read-only, at most 64 MiB of memory, and a time limit of 5 ms per callback (250 ms for the main chunk). A script that breaks a rule or fails stops being called; the reason is on its page and in the log.

## Native mods

A native mod is a shared library built against the C API in `profiles/mhp3rd/sdk/include/yakumo/mod.h`, with a header-only C++ layer in `mod.hpp`. It can do everything a script can, at native speed, and talk to other native mods. `profiles/mhp3rd/sdk/templates/native/` is a project to copy: its `CMakeLists.txt` builds the library and lays the mod folder out, and its GitHub Actions workflow builds it for Windows, Linux, macOS and Android and packs a `.ymod` ([sdk/README.md](../profiles/mhp3rd/sdk/README.md)).

```cpp
#include "yakumo/mod.hpp"

namespace {
yakumo::Mod *g_mod;
int after_lookup(ymod_cpu *cpu, void *) {
    g_mod->log("model " + std::to_string(cpu->gpr[2])); // v0
    return YMOD_CONTINUE;
}
}

// The interface this mod was built for, once per mod.
YAKUMO_MOD_API_VERSION();

extern "C" YAKUMO_MOD_EXPORT int yakumo_mod_main(const ymod_host *host, ymod_mod *handle) {
    static yakumo::Mod mod(host, handle);
    g_mod = &mod;
    if (mod.hooks() == nullptr) return 1; // cannot run without it
    // By name; 0 when this Yakumo or this game does not have it.
    if (mod.hook("armor.model_of_part", YMOD_HOOK_AFTER, &after_lookup) == 0u) return 2;
    return 0;
}
```

Yakumo calls `yakumo_mod_main` once at start. Everything else is a *service*, a versioned table of functions looked up by name:

| Service | What it offers |
| --- | --- |
| `yakumo.monsters` | [Large monsters](#large-monsters) by name, and new ones a mod adds from its own model files |
| `yakumo.memory` | Checked reads and writes of guest memory, allocation from the game's allocator, telling the JIT about rewritten code |
| `yakumo.hooks` | Hooks before, after or instead of a function, and before any instruction, in the executable or an overlay |
| `yakumo.calls` | Calling a guest function from a REPLACE hook, as guest code on the same thread; the call's result returns to the hooked function's caller. One call per callback; a second, and a call from other callbacks, is refused |
| `yakumo.events` | Frames, the game's start, whole files read, overlays loaded and unloaded, quests starting and ending; subscribing and unsubscribing |
| `yakumo.files` | Replacing a file with bytes while the game runs, as far as the running game can take it, reading the game's own bytes of one |
| `yakumo.settings` | Switches, numbers and choices on the mod's page, kept in `mods.ini` |
| `yakumo.services` | Tables a mod publishes for others to import: a native mod can be a library for other mods |
| `yakumo.symbols` | [The game's functions and data by name](#the-games-functions-by-name), and the overlay loaded over an address |
| `yakumo.tables` | [Game tables](#game-tables) by name, and making them longer |
| `yakumo.ui` | [Panels on the mod's page and overlays over the game](#panels-and-overlays), added and removed |

`mod.h` documents each function. Callbacks run on the game's thread between guest instructions; they must not block or keep pointers into guest memory. A callback may add and remove hooks, its own among them; a hook removed inside a callback stops from the next call. A service's version and size only grow, so a mod built against an older SDK keeps working with newer Yakumo releases; a mod that needs a newer service gets `NULL` on an older Yakumo and decides itself whether it can run without it. `YAKUMO_MOD_API_VERSION()` tells Yakumo which interface the mod was built for, and a Yakumo older than that refuses the mod instead of starting it. A struct the host fills in, such as `ymod_symbol`, starts with its size, which the mod sets.

A setting's key is 1 to 64 letters, digits, `_`, `.` or `-`; Yakumo refuses others, in scripts too. A native mod that returns non-zero from `yakumo_mod_main()`, and a script that fails, lose everything they registered: hooks, events, panels, overlays and services, and nothing of their code is called again, not even through a callback already under way. What they added to the game before stays for the session, because the game's tables cannot shrink: kinds of monster (whose monsters then do the base's behaviour), quests, weapons, armor, areas, table rows, replaced files, settings and the memory they allocated. A mod that imported a failed mod's service keeps the table it was given; call nothing in it once that mod is shown as failed.

Native code runs with the player's rights, like any program they install. The Mods page says so on every native mod's page. Publish the source with the mod, and build it in CI as the template does, so players can see what they run.

## The game's functions by name

[`sdk/symbols.txt`](../profiles/mhp3rd/sdk/symbols.txt) lists the game's functions and data that this project has traced, each with its address, the overlay it belongs to and a line on what it is: the file loader (`loader.size`, `loader.submit`), the armor and hunter models (`armor.model_of_part`, `hunter.load_step`), text and HUD drawing, the camera, the village's departure, and data such as `hunter.zenny`, `hunter.health`, `quest.time_left`, `quest.monsters` and the equipment tables. Yakumo compiles the table in; mods look names up at run time and never carry raw addresses for what it knows.

Each function's entry has the hash of its first 16 bytes as the game ships them. Yakumo checks the executable's functions at start, before any mod writes code, and an overlay's function when it is looked up while its overlay is loaded. A function whose bytes differ, on another version of the game, is not given out: the lookup fails and the mod can say so instead of hooking the wrong code. Data has no check. "On a quest" addresses hold other things outside one; read them only while `overlay_at(0x0A05E600)` is `game_task` or between the `quest_started` and `quest_ended` events.

A name keeps its meaning once it is in the table. To add one, trace the function in this project (a script hook that logs its arguments is often enough), say how in the document for its subsystem, and add a line with its check; the table's header explains the columns.

## Game tables

Some of the game's tables have one row for each of something the game has a fixed number of: the kinds of large monster, the model sets their files come from. A new monster needs a row of its own in each. A mod asks for more rows, `yakumo.extend_table("monster.kinds", 76)` in a script or `extend` of `yakumo.tables` in a native mod, and Yakumo:

1. checks that every instruction pair forming the table's address, and every comparison of an index with its row count, is as traced on NPJB-40001;
2. moves the table to guest memory of its own, with the game's rows copied and the new rows zeroed;
3. points those instructions at the copy and raises the counts. The units they lie in run through the JIT from then on.

The mod then writes its rows with the memory functions, at the address `table()` gives. A table inside a code overlay (`game_task`, on a quest) moves with no rows until the overlay first loads: write its rows from the `"overlay"` event, once `overlay_at(base)` is the overlay. Each time the overlay loads again, its code is pointed at the copy again, and the copy keeps what mods wrote. Several mods may extend one table; it is as long as the longest asked for, and rows already written move with it. On another executable, or over code another mod changed there, nothing moves and `extend_table` returns `nil`.

| Table | Where | Row | What a row is |
| --- | --- | --- | --- |
| `monster.kinds` | `game_task` | 8 bytes, by kind | The kind's AI overlays and how many slots it takes |
| `monster.constructors` | `game_task` | 4 bytes, by kind | The code that builds the kind's object |
| `monster.sizes` | `game_task` | 4 bytes, by kind | The code that gives the size of its object |
| `monster.carves` | `game_task` | 4 bytes, by kind | Its carving points, read when it is killed |
| `monster.files` | `game_task` | 8 bytes, by kind | Four more files loaded with it (u16, `0xFFFF` for none) |
| `monster.models`, `.second`, `.third` | executable | 2 bytes, by model set from 1 | The model set's three files |
| `monster.step_scales` | executable, read by `game_sub` | 4 bytes, by kind | The size of the dust its footsteps raise (a float). Its one reader is in `game_sub`, pointed at the copy each time `game_sub` loads |
| `quest.events` | `lobby_task` | 2 bytes | The event quests offered at the counter, by id, 0 after the last. Yakumo adds mods' quests to it itself |

A kind's model set has the kind's number. A new kind made as a copy of the Tigrex (58), with rows copied from 58 in each of these tables and quest files naming it, loads, plays and moves as the Tigrex does: [MONSTERS.md](MONSTERS.md) says how they were traced, what a new kind still lacks, and how to test one.

## Large monsters

A mod adds a monster of its own by name, from its own model files, and Yakumo does the rest: it gives the monster a kind number (from 75), a row in each [game table](#game-tables) above, the base monster's rows where the mod has none of its own yet (behaviour, constructor, size, extra files, carves), and serves the files.

```cpp
ymod_monster fatalis{sizeof(ymod_monster)};
fatalis.id = "fatalis";               // the mod's name for it
fatalis.based_on = "tigrex";          // the game's kind it starts from
fatalis.model = "model/model.pac";    // in the mod's folder: model pack (skeleton, model, textures),
fatalis.geometry = "model/model_ge.bin"; // its GE data,
fatalis.motions = "model/motions.bin";   // and its motions
const int32_t kind = mod.monsters()->add(handle, &fatalis); // -1 when not added; the reason is in the log
```

A script does the same with `yakumo.add_monster{id = "fatalis", based_on = "tigrex", model = "model/model.pac", geometry = "model/model_ge.bin", motions = "model/motions.bin"}`. Call it at start (`yakumo_mod_main`, or a script's main chunk). The files' formats are in [MONSTERS.md](MONSTERS.md#a-monsters-model); a monster from another game is converted into them by a tool of the mod's own.

The game's kinds have ids too, for `based_on` and for anything else that names a monster: `rathian`, `tigrex`, `gold_rathian` and the rest, listed in [MONSTERS.md](MONSTERS.md#the-games-kinds-by-id). `kind(id)` and `id_of(kind)` turn one into the other.

The model pack needs only the skeleton, the model and the textures (entries 0 to 2); Yakumo takes the base's other entries, and gives the model as many meshes as the base's behaviour expects. Where the base's behaviour checks for its own kind, or hides parts of its model by material, a monster made from it gets the base's treatment and keeps its own materials shown. That has been traced for the Tigrex's AI only, so the bases it works for are `tigrex` and `brute_tigrex`; another base is accepted, with a warning on the mod's page, and works only as far as its AI does not tell kinds apart, hide materials or write meshes by number ([MONSTERS.md](MONSTERS.md#ai-overlays-that-know-their-kinds)). A kind the mod added can be given its own behaviour, a part at a time. `behave(handle, kind, YMOD_MONSTER_THINK, fn, user)` runs `fn(cpu, monster, user)` each frame for each of its monsters on a quest, where the game asks a monster to think; returning `YMOD_RETURN` means the mod's function did the part, anything else lets the base's behaviour do it after. Give behaviour at start, before any of its monsters is made. Inside the part's function, `play_motion(monster, motion, blend)` asks for one of the kind's motions (by its slot), blended in over `blend` frames: the game plays it as soon as the function returns, before the base's part if that runs. `motion_finished(monster)` says whether the motion playing has reached its last frame, and `at_frame(monster, frame)` whether this frame's step passes a given frame of it, to time what happens during a motion.

The monster's body is reached the same way: `position`, `facing` and `set_facing` (degrees, 0 towards +z), `target` (the point it goes for: the hunter it is after; the game brings it up to date only now and then, and `aim(monster)` has it do so for the next frame), `turn_to_target(monster, degrees)` (by at most that much this frame; 1 once it faces the target) and `move(monster, x, z)` in its own frame (z ahead, x to the side, as in its motions). A motion turns a monster but does not carry it, so a walk moves it a little each frame; `motion_frame(monster)` says how far the motion has got, to move it as far as the motion's own root travels. `attack(monster, slot, attack)` starts one of the base kind's attacks (a row of its attack table, 0 to 63) in one of the monster's two attack slots (0 or 1), and `end_attack(monster, slot)` ends it; both are queued like motions. `effect(monster, number, joint, offset, scale)` starts one of the kind's effects on one of its joints, also queued; the effects are the base's, or another kind's named in the spec's `effects` (`"rathalos"` for fire), and a number the kind's effects do not have is refused. What an attack hits with is a shape of the base's, spheres on the base's joints; `attack_shape(handle, kind, shape, spheres, count)` gives the kind spheres on its own joints for it, at start. `attack_row(handle, kind, row, like, shape)` gives the kind a row of its own: a copy of the base's row `like` (its strength and kind of hit) that hits with `shape`, so an attack can have a shape no other row uses (rows to 63 and shapes to 31, past the base's own). Where hunters' hits land is the base's body capsules, also on the base's joints: `body_shapes(handle, kind, capsules, count)` gives the kind its own ([MONSTERS.md](MONSTERS.md#a-monsters-body-capsules)); without them a kind on another skeleton may take no hits at all. `footsteps(handle, kind, steps, count)` says where its feet come down (motion, frame, joint, size of step), and after each frame its mod thinks alone the game raises the dust there, as a monster's own effects part does for the base ([MONSTERS.md](MONSTERS.md#a-monsters-footsteps)). A kind whose `THINK` the mod does itself (`YMOD_RETURN`) has the base's `YMOD_MONSTER_ACT` (which carries out the base's decisions) and `YMOD_MONSTER_EFFECTS` (the base's sounds and effects by motion slot) left out in that frame, unless the mod gives those parts too. In C++, `yakumo::Monster` wraps these for one monster, with `distance_to_target()` and `angle_to_target()` worked out from them:

```cpp
yakumo::Monster self(*monsters, monster);
if (std::abs(self.angle_to_target()) > 30.0f) self.turn_to_target(4.0f);
else if (self.distance_to_target() > 800.0f) self.move_forward(12.0f);
if (self.at_frame(40.0f)) self.attack(1, 6);
```

More parts get names as the game's calls into a monster are traced; its name, quests and hit zones are to come, as building blocks of their own.

## Quests

A native mod describes a quest with `yakumo.quests`, and Yakumo builds it into the game's format and offers it among the event quests ([QUESTS.md](QUESTS.md#quests-a-mod-describes)):

```cpp
const ymod_quest_monster fatalis{"fatalis", 1, {200.0f, 1.0f, 1500.0f}, 0x2A, 0x28, 100}; // area, position, health, attack, size
ymod_quest quest{sizeof(ymod_quest)};
quest.id = 62666;           // 62000 to 65535
quest.map = 8;              // the arena
quest.stars = 8;
quest.minutes = 50;
quest.fee = 1500;
quest.reward = 18000;
quest.target = "fatalis";   // hunt `count` of it
quest.count = 1;
quest.monsters = &fatalis;
quest.monster_count = 1;
quest.texts = {"The Black Dragon", "Hunt the Fatalis", "Reward Zero\nTime Over", "...", "Fatalis", "Guild Master"};
quest.area = nullptr;       // or an area of the mod's to play it in (see Maps)
static const ymod_quest_reward rewards[] = {{"Rainbow Crystal", 1, 30, 0}, {"Earth Crystal", 3, 70, 0},
                                            {"ElderDragonBlood", 1, 100, 1}};  // item, count, chance, group
quest.rewards = rewards;    // group 0 the main reward, drawn several times; 1 and 2 two more;
quest.reward_count = 3;     // a group's chances add up to 100
static const ymod_quest_supply box[] = {{"First-aid Med", 2}, {"Ration", 2}};
quest.supplies = box;       // the supply box at the start, up to 32
quest.supply_count = 2;
mod.quests()->add(mod.handle(), &quest); // the id, or -1 with the reason in the log
```

Add it at start, after the monsters it names. A built quest is a hunt with its rewards and supply box but no small monsters, gathering or supplies delivered later yet, and its texts are the same in every language ([QUESTS.md](QUESTS.md#quests-a-mod-describes)). A monster's `health(monster, now, most)` says when it has fallen, so a mod's behaviour can play its fall.

## Weapons

A native mod adds a weapon with `yakumo.equipment` ([EQUIPMENT_MODS.md](EQUIPMENT_MODS.md#weapons-mods-add)):

```cpp
ymod_weapon blade{sizeof(ymod_weapon)};
blade.id = "black_blade";
blade.kind = "great_sword";        // one of the twelve classes
blade.based_on = 3;                // the game's great sword it starts from: its attack, element, sharpness, slots
blade.model = "model/blade.pac";   // a pack of model, skeleton and textures
blade.name = "Black Blade";
blade.description = "Forged from a black dragon.";
blade.price = 18000;
blade.give = 1;                    // one in the equipment box when the village loads
const int32_t id = mod.equipment()->add_weapon(mod.handle(), &blade); // its id in its class, or -1
```

Armor is added the same way, a piece of one of the five parts:

```cpp
ymod_armor helm{sizeof(ymod_armor)};
helm.id = "black_helm";
helm.part = "head";                // "head", "chest", "arms", "waist" or "legs"
helm.based_on = 1;                 // the game's head piece it starts from: defence, resistances, skills, slots
helm.model_male = "model/helm_m.pmo";   // the part's model file for a male hunter
helm.model_female = "model/helm_f.pmo"; // and a female one (NULL: the male's)
helm.name = "Black Helm";
helm.description = "A black dragon's skull.";
helm.give = 1;
const int32_t id = mod.equipment()->add_armor(mod.handle(), &helm); // its id in its part, or -1
```

Add them at start. Their stats are the base's for now.

## Maps

A native mod adds an area with `yakumo.maps`: its own model, GE data, textures and collision, in place of one of the game's areas on the mod's quests that name it ([MAPS.md](MAPS.md#areas-mods-add)):

```cpp
ymod_area castle{sizeof(ymod_area)};
castle.id = "castle_schrade";
castle.map = 8;                            // the arena,
castle.area = 1;                           // its fighting area
castle.model = "map/area_model.pmo";       // PMO "102", static
castle.geometry = "map/area_ge.bin";       // its GE data
castle.textures = "map/area_textures.tmh";
castle.ground = "map/area_ground.bin";     // collision blocks ("4.00"): the ground
castle.walls = "map/area_walls.bin";       // and the walls
mod.maps()->add_area(mod.handle(), &castle); // 0, or -1 with the reason in the log

quest.area = "castle_schrade";             // and the quest is played in it
```

Add it at start, before the quests that name it. A quest that names an area is on that area's map, so it needs no `map`, and a monster of it whose `area` is 0 starts in the added area; another area number is kept. While such a quest is played, the base area's files are served with the mod's in them; the area's scene, camera paths and minimap stay the game's, and every other quest loads the game's area. The files' formats are in [MAPS.md](MAPS.md); an area from another game is converted into them by a tool of the mod's own.

## Panels and overlays

A mod can draw in Yakumo's interface through `yakumo.ui` (native) or `yakumo.panel` and `yakumo.overlay` (scripts):

- A **panel** sits on the mod's page in the Mods menu, under its settings, with the title as its heading. It is drawn with the menu's own rows, so a gamepad reaches it: text, buttons, switches, numbers, sliders and text typed on the menu's keyboard.
- An **overlay** is drawn over the running game, every frame, in a small window at the top left of the game's picture with the mod's title. It takes no input: widgets only show their values. It can draw lines, rectangles, circles and text on the game's picture, in coordinates from 0 to 1 from its top left. Overlays are hidden while the menu is open.

```lua
local hits = 0
yakumo.panel("Counters", function(ui)
    ui.text("Hits so far: " .. hits)
    if ui.button("Reset") then hits = 0 end
end)
yakumo.overlay("Hits", function(ui)
    ui.text(tostring(hits), 0xFFD040FF)
    ui.circle(0.5, 0.5, 0.02, 0xFF4040FF, true)
end)
```

The draw function runs every frame its panel or overlay shows, with the widgets as `ui`, which exist only during that call. In scripts:

| Widget | What it does |
| --- | --- |
| `text(text [, colour])` | A line of text; colours are `0xRRGGBBAA` |
| `separator()`, `same_line()`, `section(label)` | A rule, the next widget on the same line, a heading (`true`) |
| `button(label)` | `true` in the frame it is pressed |
| `checkbox(label, value)` | The value after the player's change, and whether it changed |
| `slider(label, value, min, max)`, `slider_float(label, value, min, max)` | A whole number or a number within its range: the new value, and whether it changed |
| `input(label, text [, capacity])` | Text typed on the menu's keyboard, up to `capacity` bytes (256): the new text, and whether it changed |
| `line(x0, y0, x1, y1 [, colour, thickness])`, `rect(x0, y0, x1, y1 [, colour, filled])`, `circle(x, y, radius [, colour, filled])`, `text_at(x, y, text [, colour])` | Shapes on the game's picture, overlays only; a circle's radius is a part of the picture's height |

Native mods get the same widgets as the `ymod_widgets` table that `mod.h` describes, with the value passed by pointer, and remove a panel or an overlay with `remove()`. Keep draw functions short: they run on the game's thread, and a script's draw function has the same 5 ms limit as its other callbacks.

## Packages: one file per mod

A `.ymod` file is the mod's folder zipped (with `mod.ini` at the top of the zip, or the mod folders), under the extension `.ymod`. Players install one by dropping it on Yakumo's window or choosing it with *Import Mod…*; on Android, *Import a .ymod Package…* opens the system picker. A package holding `mod.ini` at its top installs under the package's name.

```sh
cd my_mod && zip -r -X ../my_mod.ymod .
```

Yakumo reads stored and deflated entries only, and refuses encrypted packages, links, Zip64, names that leave the folder, and packages over 1 GiB unpacked (512 MiB a file). The template's workflow writes a `.ymod` with every platform's library in it.

## Testing

- The Mods page lists, under each mod, what it changes and what could not be used. Problems met while running, such as an edit whose expected bytes were not there, appear at the bottom of the page and in the log as `[mods] …` lines. What a native or script mod logs is on its page under *Its Log*.
- `MHP3RD_TRACE_MODS=1` logs every file read a mod serves, every block written to memory and every added file served. The start-up line `[mods] code: N block(s), … unit(s) of recompiled code retired, compiled again from memory by the JIT` says how much code your mod changed. At exit, `[jit] compiled=… instructions=…` says how much the JIT compiled.
- File changes apply the next time the game loads the file; after editing a mod's files, choose *Read the Folder Again*. Changes to memory and code, added files, native libraries and scripts apply at the next start.
- `MHP3RD_NO_MODS=1` turns every mod off for one run, for comparing with the game as it was.
- Run the game with a bound while testing scripted changes: `timeout 60 Yakumo`.

## Sharing a mod

Share the mod as a `.ymod` package, or its folder zipped. Do not share the game's own files or a disc image: a mod that changes a file ships its own version of it, made from the player's disc by the mod's author. Yakumo does not host, bundle or link to mods.

## Mods made for the mod manager

Yakumo reads the mod manager's format as it is. For a new mod, this is how its keys correspond to this format:

| Mod manager | Yakumo format |
| --- | --- |
| `Type="File"`, `Files="a.pac;b.pac"`, `Target="0601;05D2"` | `[Files]` with `0601 = a.pac` and `05D2 = b.pac` |
| `Type="Patch"`, `Files="p.bin"`, `Target="0702"` | `[Patches]` with `0702 = p.bin`, or the patch written out under `[Patch 0702]` |
| `Type="Code"`, `Files="c.bin"` | `[Code]` with `Files = "c.bin"`, or the code as images and `[Memory]` lines |
| `Type="PseudoPack"` with parts | One Yakumo mod with all of the parts |
| `Type="Pack"`, `ModList` | Stays a pack; Yakumo's format has no packs |
| `Type="Equip…"` | Stays an equipment mod: the player chooses which piece it replaces |
| `FilesHD`, `TargetHD`, `Version` | Not needed: Yakumo's format is for the HD release only |
| `Depends`, `Priority`, `Description`, `preview.png` | The same |
| `Script` (the manager's settings scripts), `Animation`, `Audio` | Not supported; a setting is a [script](#script-mods) or [native](#native-mods) mod's job |
