# Compatibility

What works on each platform, as last checked by hand. Each row is a part of the game; each cell is its state on that platform, with the issue that tracks any problem.

| Status | Meaning |
| --- | --- |
| ✅ | Works |
| ⚠️ | Works, with a known problem |
| ❌ | Does not work yet |
| ❔ | Not checked on this platform |

## Game

| | macOS (Apple Silicon) | Linux | Steam Deck | Windows | Android (emulator only, device test pending [#127]) |
| --- | --- | --- | --- | --- | --- |
| Build | ✅ | ❔ [#14] | ✅ in a Debian 13 container [#14] | ✅ MSVC 19.44, all 355 overlay DLLs [#13] | ✅ NDK r28c, arm64-v8a; all 355 overlay libraries |
| Boot, title and menus | ✅ | ❔ | ✅ | ✅ | ✅ |
| Character creation | ✅ | ❔ | ✅ | ❔ | ✅ |
| Village | ✅ | ❔ | ✅ | ✅ reached from a loaded save | ✅ reached from a loaded save |
| Hunts | ✅ | ❔ | ✅ | ❔ | ✅ |
| Graphics | ✅ lighting, fog, the quest reward screen and tiled 2D screens | ❔ | ✅ lighting, fog, the quest reward screen and tiled 2D screens; ⚠️ lighting slows the busiest village spots slightly [#7] | ❔ | ❔ on a device |
| GPU compatibility Auto [#210] [#212] | ✅ `f2416df`: off at start; on after a self-test made to fail (`MHP3RD_GPU_SELFTEST=fail`), a refused pipeline at start and in play (`MHP3RD_GPU_FAIL_PIPELINES=1`, `=play`); the intro draws in each | ❔ | ❔ | ❔ | ❔ on a MediaTek phone: speed with Auto, and whether the self-test passes there |
| Sound effects | ❔ re-check [#4]: the game now runs at real time | ❔ | ❔ | ❔ | ❔ |
| Music | ✅ with FFmpeg | ❔ | ❔ | ❔ | ❔ |
| Picture beside a display cutout [#170] | ✅ unchanged (no cutout) | ❔ | ❔ | ❔ | ✅ centred and full screen with hole, corner and tall cutouts, in both landscapes, after turns, the home screen and the notification shade; ❔ on a device |
| Cutscene movies | ✅ with FFmpeg; the opening movie checked | ❔ | ❔ | ❔ | ✅ the opening movie |
| Saving and loading | ✅ PSP-format saves; no dialog screens yet [#33] | ❔ | ✅ a save copied from a PSP loads | ✅ a new character saves and loads after restarting the game [#13] | ✅ saves load; import and export through Android's file picker |
| Save-data pass [#167]: create, save, load after a restart, overwrite, import, backup | ✅ all of it, the backup reminder and the import's *Back up now* included | ❔ | ❔ the Deck was offline for the pass | ✅ all of it on `main`; the backup reminder is newer than that build and was not checked | ✅ the reminder and *Back up now* through the picker, import of an encrypted PSP save with the review's backup, load after the restart; ❔ creating and overwriting a character (emulator keyboard) |
| Downloadable content | ✅ a player's `ULJM05800QST` folder is read by the download menu | ❔ | ❔ | ❔ | ❔ |
| Free camera (experimental) | ✅ `8690816`: the village and the Misty Peaks base camp, keyboard and mouse and the input script's virtual gamepad, photo mode at 30 and 60 frames a second [#162] | ❔ | ❔ | ❔ | ❔ |
| Lock-on camera [#163] | ✅ `69dff80`: the small arena (event quest with Arzuros, Lagombi and Volvidon) from the full test save, the input script's virtual gamepad: R3 locks and follows the monster through its charges, the stick, the D-pad and L let go, the monster's death lets go, R3 in chords (screenshot, menu, free camera) never locks, the free camera suspends it; leaving the area checked only by setting the game's flag, not by walking out | ❔ | ❔ | ❔ | ❔ |
| Multiplayer | ✅ with a Steam Deck through an ad hoc server: hall and a full quest [#2] | ❔ | ✅ with a Mac through an ad hoc server: hall and a full quest [#2] | ❔ | ❔ |

Rows marked ❌ on every platform are missing features rather than platform problems.

## Input

| | macOS (Apple Silicon) | Linux | Steam Deck | Windows | Android (emulator only, device test pending [#127]) |
| --- | --- | --- | --- | --- | --- |
| Keyboard | ✅ | ❔ | ✅ | ❔ | ⚠️ the emulator's own keyboard releases keys at once; scrcpy's keyboard is the workaround |
| DualSense | ✅ | ❔ | — | ❔ | ✅ through scrcpy on the emulator |
| DualShock 4 | ❔ | ❔ | — | ❔ | ❔ |
| Xbox controllers | ❔ | ❔ | — | ❔ | ❔ |
| Built-in controls | — | — | ✅ Game Mode (added to Steam as a non-Steam game); ⚠️ Desktop Mode sends mouse and Esc from Steam's desktop layout | — | ✅ on-screen touch controls |

## Tested hardware

| Platform | Machine | GPU and driver | Commit | Date |
| --- | --- | --- | --- | --- |
| macOS 27 | Apple M1, 8 GB | Apple M1, MoltenVK | `v0.1.0` | 2026-09-18 |
| macOS 27 | Apple M1, 8 GB | Apple M1, MoltenVK | `3480dc2` (saving and loading) | 2026-09-18 |
| SteamOS 3.8.16 | Steam Deck | AMD Custom GPU 0932, RADV (Mesa 26.0.0-devel) | `5e4b27c` | 2026-09-18 |
| SteamOS 3.8.16 | Steam Deck | AMD Custom GPU 0932, RADV (Mesa 26.0.0-devel) | `v0.3.0` | 2026-09-18 |
| macOS 27 | Apple M1, 8 GB | Apple M1, MoltenVK | `v0.3.0` | 2026-09-19 |
| Windows 11 Pro | x64 PC | AMD Radeon RX 6500 XT | `98e2468` | 2026-09-19 |
| Windows 11 Pro | x64 PC | AMD Radeon RX 6500 XT | `b67cd7a` (saving and loading) | 2026-09-20 |
| Windows 11 Pro | AMD Ryzen 5 5600, 16 GB | AMD Radeon RX 5600 XT, AMD driver (Vulkan 1.4.315) | `87bc4d9` (build with MSVC 19.44, unit tests, a PSP save loaded, village at 30 game fps, 60 presented, 100% speed) | 2026-09-27 |
| Android 15 emulator (API 35, arm64) | Apple M1, 8 GB | Apple M1 through the emulator's gfxstream | `7b87a57` | 2026-09-23 |
| macOS 27 | Apple M1, 8 GB | Apple M1, MoltenVK | `3f1c0e9` (save-data pass) | 2026-09-27 |
| Windows 11 Pro | Ryzen 5 5600, 16 GB | AMD Radeon RX 5600 XT | `87bc4d9` (save-data pass) | 2026-09-27 |
| Android 15 emulator (API 35, arm64) | Apple M1, 8 GB | SwiftShader | `2caf2fd` (save-data pass) | 2026-09-27 |
| macOS 27 | Apple M1, 8 GB | Apple M1, MoltenVK | `65daf82` (the hot spring's glints at 60 fps, [#168]: 240 presents in a row from a scripted run, against `MHP3RD_INTERPOLATION_NO_NEAREST_INSTANCES`) | 2026-09-27 |
| Android 15 emulator (API 35, arm64) | Apple M1, 8 GB | SwiftShader | `cf5af0d` (display cutout, #170) | 2026-09-27 |
| Android 10 emulator (API 29, arm64) | Apple M1, 8 GB | SwiftShader | `cf5af0d` (display cutout, #170) | 2026-09-27 |

## Coverage-expansion device checks (2026-10-04)

These bounded checks validate the coverage-expansion branch; they do not replace the full manual gameplay matrix above. Compatible generated code and prebuilt overlays were reused.

| Platform | Tested commit | Native tests | Inspected game captures |
| --- | --- | --- | --- |
| macOS, Apple M1 / MoltenVK | `1dc7dac` | 41/41 | Title and Video menu |
| Windows, MSVC / Vulkan | `1dc7dac` | 41/41 | Copied-save village and Video menu; stable 100% game speed |
| Steam Deck, SteamOS / RADV | `7e00ebc` | 49/49, including Vulkan and SDL UI variants | Continue menu and Video menu; 30 game fps and 100% speed |
| Android 10 emulator, arm64 / SwiftShader | Native tests: `d5290c0`; application: `1dc7dac` | 40/40 | Title and Video menu |

Application sources are identical between these commits: the later commits correct deterministic test-controller isolation and deployment of the public NID CSV to Android test fixtures. The controller fixtures also passed separately on macOS after the correction.

Game runs used isolated or copied save data and finite deadlines. Captures were inspected, and test processes stopped. macOS used 32 compatible overlays and disabled audio/FFmpeg; Android is an emulator with audio/FFmpeg disabled. Hunts, multiplayer, audio quality and long-session stability were not checked in this pass. The Steam Deck developer binary was installed with a rollback copy; the release installation was unchanged.

## Translation layout checks (2026-10-04)

PR #227 application source `ac40d11` was built with GCC 14 and Vulkan on Steam
Deck. Five affected native suites passed; 52/52 renderer-enabled native suites
also passed on macOS. These are targeted translation checks, not a full gameplay
compatibility pass.

| Language | Inspected copied-save inventory result |
| --- | --- |
| Russian | Cyrillic uses half-width cells; item names and the sampled three-line description fit their panels. A same-frame comparison with the original-width switch reproduces the previous excessive spacing and clipping |
| English | Original Latin cell widths, alignment and inventory layout retained |
| Spanish | Locally authored accented labels and inverted punctuation displayed and fitted; precomposed accented vowels, diaeresis and both cases of n with tilde checked |
| Japanese | Locally authored kana/kanji labels and descriptions retained full-width cells and fitted the panel |

Each run was bounded to 60 seconds with finite input; captures and font traces
were inspected. One main text AOT unit was regenerated/rebuilt; all other main
objects and 355 compatible overlays were reused unchanged. No game text fixtures
or translation files are distributed in this repository. Audio was disabled;
full translation completeness, every field's fit, quests, multiplayer, hunts,
other-device rendering and long sessions were not checked. Translation authors
still need to shorten lines that exceed a field.

## Translation search budget checks (2026-10-04)

PR #227 application source `b385767` was built in GCC 14 Release on Steam Deck;
four affected native suites passed, and all 355 overlay hashes stayed unchanged.
The full 52-suite renderer-enabled run passed on macOS; the final RAM-edge guard
and regression then passed the affected runtime suite again.

A release-optimized benchmark over 32 MiB of synthetic RAM and 40 invented quest
records measured the original inline search at 61-68 ms per blocking call. The
incremental version retained the complete search across 128 slices, with each
slice at 0.45-0.50 ms in that benchmark. The final production runtime's synthetic
trace measured 530 inline-search slices (maximum 0.934 ms) and 650 block/probe
slices (maximum 0.595 ms), excluding the intentional unsliced diagnostic case.
These timings are observations on this device, not wall-clock guarantees.

Regression checks cover shared work across pending entries, slice-crossing and
unaligned candidates, late discovery behind a cursor, repeated loads with arena
reuse, missing-probe retries, RAM-end fields and the diagnostic off switch.
No game-derived fixture is committed. Live quest/frame correlation, real-quest
translation timing and gameplay behavior with the incremental search remain
unverified: the developer's active game was left untouched and no new game
window was opened. This demonstrates removal of the search's blocking cost;
it does not claim every reported quest stutter is resolved.

## Updating this page

Run through [the smoke test](TESTING.md) on the platform, then change the cells you checked in the same pull request as the fix, or in a pull request of their own. Add a row to *Tested hardware* with the commit you tested. A result without a commit cannot be compared with anything later, so it does not go in the table.

To report a result without editing the page, open a **Test report** issue.

[#1]: https://github.com/TeamGDB/Yakumo/issues/1
[#2]: https://github.com/TeamGDB/Yakumo/issues/2
[#3]: https://github.com/TeamGDB/Yakumo/issues/3
[#4]: https://github.com/TeamGDB/Yakumo/issues/4
[#5]: https://github.com/TeamGDB/Yakumo/issues/5
[#6]: https://github.com/TeamGDB/Yakumo/issues/6
[#7]: https://github.com/TeamGDB/Yakumo/issues/7
[#13]: https://github.com/TeamGDB/Yakumo/issues/13
[#14]: https://github.com/TeamGDB/Yakumo/issues/14
[#33]: https://github.com/TeamGDB/Yakumo/issues/33
[#127]: https://github.com/TeamGDB/Yakumo/issues/127
[#162]: https://github.com/TeamGDB/Yakumo/issues/162
[#163]: https://github.com/TeamGDB/Yakumo/issues/163
[#167]: https://github.com/TeamGDB/Yakumo/issues/167
[#168]: https://github.com/TeamGDB/Yakumo/issues/168
[#170]: https://github.com/TeamGDB/Yakumo/issues/170
[#210]: https://github.com/TeamGDB/Yakumo/issues/210
[#212]: https://github.com/TeamGDB/Yakumo/issues/212

## Android user-file checks (2026-10-08)

PR #292 application source `3f6009a` was checked on a Lenovo TB321FU running
Android 15 (arm64), in a separate test package. A public synthetic
`DocumentsProvider` exercised the production Java document methods, JNI
transport, folder-transfer worker, texture-pack checker/copy/install and mod
checker/import. The fixture activity replaces SDL activity initialization. Fixture-provider
checks inject a tree URI; external Downloads checks use the unchanged
production system picker and its normal user grant. These results do not
establish gameplay or menu rendering.
The complete public document-adapter regression executable also passed on the
physical device under a 30-second deadline.

| Check | Measured result |
| --- | --- |
| System picker and Downloads | Passed: selected nested texture/mod folders through Android DocumentsUI and its normal access confirmation; no storage permission or root needed |
| Texture import | Passed: nested Cyrillic/Japanese names and an emoji filename; existing texture checker, copy and installation completed |
| Mod import | Passed: the provider's folder display name is retained as the mod identity; existing mod checker and import completed |
| Cancellation | Passed after 128 KiB from the delayed fixture provider and 768 KiB from a real 128 MiB Downloads file; staging was empty afterwards. Cancelling the system picker itself also returned no staged result |
| Denied and unreadable documents | Passed: an external tree without a grant and a fixture file refusing reads both failed with staging removed |
| Transfer byte limit | Passed: both the fixture stream and a real Downloads file exceeded a reduced 1 KiB limit and left no staging |
| Worker responsiveness | Passed: a delayed 32 MiB provider stream completed while the fixture activity processed 20 heartbeat callbacks; this does not measure game frames |
| Restart persistence and replacements | Passed: installed synthetic texture/mod bytes remained after force-stop/restart and failed/cancelled transfers; replacing each through Downloads retained the prior installation in its existing backup folder |

The player installations and their data were unchanged. No storage permission,
root access, game data, generated-game compilation or overlay compilation was
needed for these checks. Android rejected selection of the storage root;
selecting nested Downloads folders worked. Test fixtures and the temporary
native executable were removed, the USB stay-awake setting was restored, and
only the separate test package was stopped. Android 16, SD-card/cloud
providers, rendering a real texture pack, enabling a real mod, and long-session
gameplay were not checked. The initial component-only pass did not rebuild a playable APK.


### Complete Android APK smoke check

A complete arm64 APK at source `6f6106a` was subsequently built with SDL3
3.4.16, bundled FFmpeg 7.1.5, the fallback font, 90 reused Android AOT
objects and all 355 existing overlay libraries. The archived generated corpus
matched the current corpus byte-for-byte; all 12 framework headers matched
after pinned formatting. The APK passed signing, 16 KiB ZIP alignment,
game-file exclusion and Android 29 strong-import checks (359 libraries,
828 imports, none unresolved). Its certificate matches the existing test
installation and its version code is higher.

On the same Android 15 tablet, an isolated package with identical native
libraries and production Java code rendered the opening movie and the native
pause menu. The native texture menu opened Android's picker, reviewed a
synthetic Downloads folder and installed it successfully; the result screen
and installed files were checked. The native mod menu opened the same picker,
and cancellation returned to the menu with no staged files left. The run had
a 180-second deadline and the test package exited afterwards. Existing player
installations were not replaced. The owned Downloads fixtures were removed
and the temporary stay-awake setting was restored.

This verifies boot/rendering and the stated menu operations. It does not
verify a hunt, long-session stability, the full mod installation flow through
the native menu, a player's texture pack/mod, or Android 16 behavior.
