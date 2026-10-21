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

## Layered armor Smithy regression

Windows gameplay validation of `067bf7e` confirmed that switching Smithy armor
preview categories with layered armor enabled no longer reproduces the hang.
Preview selections update and performance is normal. The local trace confirms
the preview copy guard ran and the session ended through the game menu. The
full synthetic equipment-model suite passed. Android and other platforms remain
unverified for this fix; see [Layered armor](LAYERED_ARMOR.md#smithy-previews).

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

## Touch editor and virtual R3 checks (2026-10-12)

PR #313 application source `f8a2f5f` passed all 69 renderer-enabled native
suites on Apple M1/macOS with SDL 3.4.16. Its integration with `main` at
`43190f0` passed all 72 suites on the same machine. The integration tree
matched GitHub's PR merge tree; it was also used for the physical Android
checks below. All CI checks on `f8a2f5f` passed, including desktop/Android
native suites, sanitizers, formatting, static analysis and the 80% diff gate.

| Platform | Verified behavior |
| --- | --- |
| macOS, Apple M1 / MoltenVK | Synthetic SDL/UI regressions passed for both layouts: selecting or moving less than the drag threshold preserves placement exactly; PSP stock controls resize relative to their factory geometry across viewports, safe areas and global sizes; virtual R3 drives mapped movement/camera axes in Camera, D-pad and Off modes and releases/cancels cleanly |
| Redmi 12C, Android 14, Mali-G52 MC2 / r32p1 | Inspected PSP and Action editor captures through Android input. Moved/resized a PSP R+Circle combination button to 105%, hid/restored it without changing its saved placement/binding, and checked persistence after restarting. Simultaneous real Android touch contacts held that combination and the fixed stick (`0x2200`, stick X 189), then released to neutral. An ordinary Action button tap preserved the saved touch configuration; moving/resizing it retained the PSP combination settings. In a loaded quest, R3 in both layouts drove mapped movement and camera input, then released to neutral. The tester confirmed tactile vibration |

Android used a separate bounded device-test package, copied fixtures, 90
compatible AOT objects and all 355 existing overlays. Captures, saved settings
and input observations were inspected. Player installations were unchanged,
and the owned test process was stopped. Interrupted SSH/controller attempts
were retained separately and excluded from the successful results.

These are targeted input checks. Physical-device default R3 monster lock-on,
vibration intensity/repetition and the disabled setting, rotation, additional
Android devices, audio quality, multiplayer and long-session gameplay were
not checked. The haptic logic has automated coverage; the tactile confirmation
does not establish motor timing or callback counts. No new Steam Deck or
Windows gameplay check was performed for the follow-up fixes.

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


## Android high-refresh display check (2026-10-08)

PR #295 application source `9d3d5a2` was checked on Lenovo TB321FU,
Snapdragon SM8650 / Adreno 750, Android 15, Vulkan driver 0.762.28. The
complete arm64 APK used SDL3 3.4.16, bundled FFmpeg 7.1.5, 90 compatible
AOT objects and all 355 existing overlays. An isolated verification package
had identical production Java bytecode and all 359 native libraries. Runs
used a copied save, finite deadlines and Original runtime text, with no
external texture pack or runtime translation installed.

With **Frame rate = 120**, **Lower when behind = Off**, **Vsync = On**,
**Game speed = Normal** and **Auto / Fill = 2560 x 1600**, the previous
application presented about 58–60 FPS in a stationary village scene. Its
SDL refresh value was 120 Hz, but Android's active display mode was 60 Hz.
A short diagnostic minimum-refresh override produced 115–120 FPS; the
original system setting was restored after that diagnostic.

The patched application requested 120 Hz through the surface API and Android
selected its 120 Hz mode without a global display override. After discarding
five warm-up samples, 48 one-second village samples measured 114.7–120.2 FPS
(median 117.75), with reported game speed 98–102%. Resolution was unchanged.
This measures presented frames; the guest simulation still runs at 30 game
frames per emulated second.

Changing the native Video menu target through 90, 60, 90 and 120 FPS selected
Android's corresponding 90, 60, 90 and 120 Hz modes. Returning from Home
reapplied the 120 Hz request and selected that mode. These setting/lifecycle
checks ran from the startup screen; they do not measure hunt performance.
The test package was stopped and removed, and the temporary USB stay-awake
setting restored. The normal signed update APK was copied to Downloads with
its checksum verified; the player's installation was not replaced.

All 50 headless and 60 renderer-enabled native tests passed. Synthetic tests
cover exact surface/rate/compatibility arguments, invalid input, missing
library/API (Android 10 fallback), rejected requests and the comparison
switch. Changed executable native lines reached 100% coverage (37/37);
Android-specific SDL call sites still require physical evidence. The APK
passed signature and 16 KiB ZIP alignment checks. Android may choose a lower
refresh under battery, thermal or vendor policy.

Android 10/16 physical devices, Mali and other Snapdragon generations,
Match display on the device, rotation, translated gameplay, hunts,
multiplayer, audibility and sustained thermal performance were not verified.
This fixes the measured 60 Hz display restriction; it does not complete all
Snapdragon performance work in #291 or the release validation in #290.

## Settings and compact overlay checks (2026-10-11)

PR #308 native application source `93ea2ff50840fdf17901e1e349ab089efd47862a`
was tested with compatible prebuilt AOT objects and overlays. Later review
commits change the device harness, tests and documentation only; reports retain
separate harness/source declarations and exact artifact hashes.

| Platform | Native suites | Inspected scope |
| --- | --- | --- |
| macOS / Apple M1 / MoltenVK | 65/65 | Rendered menu sections, readable QoL labels, compact overlay, F3 hide/restore and menu reopening |
| Windows x64 / MSVC / Vulkan | 58/58 | Interactive desktop renderer/UI tests and rendered menu/compact/F3 captures; developer environment required for the generated-code compile fixture |
| Steam Deck / SteamOS / RADV | 64/64 | Gamescope/X11 renderer/UI variants and bounded full-game menu/compact captures; window-size tests use a controlled initial logical size |
| Physical Android arm64 / API 29 build | 52/52 headless suites | Dedicated developer APK: all seven public menu sections and compact/F3 captures; scripted keyboard/virtual-controller input |

Real UI regressions verify section-specific resets, preserved startup overrides,
all performance modes, glyphs, buffer bounds and unchanged pixels outside the
overlay. The first Windows-to-Android binary upload exposed Ctrl-Z truncation;
ASCII encoding with source/destination SHA-256 verification now prevents launch
with damaged inputs. The harness also separates the bounded first-upload budget
from the shorter gameplay session and checks manual PID-probe deadline expiry.

These are focused menu/overlay checks. Actual physical-control feel, touch-only
navigation, audible output, representative hunts, multiplayer, long sessions,
final player-package updates, other Android GPUs and fresh emulator variants
remain unverified. Runs used copied saves/settings and bounded owned processes;
player/release installations, original saves and launchers were preserved.

## Automatic device-suite implementation checkpoint (2026-10-11)

Draft [PR #311](https://github.com/TeamGDB/Yakumo/pull/311), tracked by
[#310](https://github.com/TeamGDB/Yakumo/issues/310), adds automatic assertions
alongside the reviewed scenarios above. At `acec3ac`, fresh macOS native/coverage
tests passed 68/68; the application C++ was unchanged from `2aa40eb`. Local
changed-line coverage was 89.35% (1477/1653). Windows 58/58 and Deck 64/64 native
results were collected at the earlier `2f1d617`, with interactive renderer reruns;
they do not certify a current-head device suite.

Bounded macOS developer probes independently observed copied-save boot and update
preservation, quest identity/timer, a small-monster health decrease through normal
attacks, photo freeze/frame step and Japanese text coverage versus original
fallback. Exploratory binaries include source patches recorded privately; these
are qualified development evidence, not exact release-artifact results.

First HUD hiding while already paused in photo mode did not activate before a
guest flip. Quest-retire confirmation did not return to village within the bound,
also on an earlier baseline and with audio enabled. Desktop activation did not
produce observed SDL focus on macOS. These remain failed/unverified outcomes.
The extended translation matrix, complete photo/lifecycle checks, real save/backup
and replacement textures, two-peer hunts and repeated performance comparisons
remain deferred follow-up. At application source `751424a`, Windows native
results are 61/61 (51 SSH tests plus 10 interactive renderer tests); physical
Android native results are a fresh 56/56 after staging the actual FFmpeg/SDL
runtime dependencies. The initial Android run retained nine link failures from
an omitted dependency. An isolated full Android APK also passed copied-character
boot with four independent state assertions and verified input hashes; desktop
installer/portable variants remain unsupported on that adapter. Japanese and
verified English-patched inputs pass the menu/dialogue field checks, including
covered expectations that differ from source and original fallback. The normal
counter/gate route has independently reached quest 204 with an advancing timer
and no quest cheats. At that implementation checkpoint, complete extended device
scenarios and cloud validation were pending;
no cloud reservations have been consumed at this checkpoint. The suite does not
complete any exact-candidate release or human playtesting sign-off.

The local QUICK implementation and validation stage is complete. Google testing
and extended gameplay are deferred under the existing #310/PR #311 and are not
merge requirements for this stage. The capability gaps and historical failures
below retain their actual verdicts.

Final harness `c2eee50` passed all
ten categories (12 automatic subchecks) on macOS in 110.468 seconds, with 0.887
seconds of preparation. Earlier bounded runs passed in 99.7-104.8 seconds.
All passed cleanup,
using application source `751424a` and
artifact SHA-256 `9e33423b4bed48cef751102d4be9c167b0cfd953f60b82070c907a75f2836339`.
The aggregate identifies actual quest/input/
text/photo/telemetry smoke separately from executed synthetic/native installation,
save/mod, two-peer transport and lifecycle/PCM contracts. This result does not
claim completion of the deferred extended matrix. Initial physical Android
preparation measured about 329.5 seconds before the short game checks, exceeding
the entire 300-second Google pilot cap; no paid reservation is justified by that
transfer path at this checkpoint.

On Windows, QUICK harness `738ed74` passed 11/12 subchecks in 135.4 seconds with
cleanup passing, using the `751424a` application artifact
`65b4089b34975fffdff3c695a915f9d7c6a63b3aae7f9a269a5b3b884dcc1c2a`.
The lifecycle contract group was NOT TESTED because that build does not register
all required Android-specific contracts; the aggregate correctly remained NOT
TESTED. The initial Steam Deck preflight timed out over SSH; that evidence remains
separate from the resumed result below. Earlier native results retain their earlier
source qualifier.

After the device was powered on, Steam Deck QUICK ran on application/harness
source `325e28d`, with compatible AOT and all 355 overlays reused. It passed
11/12 subchecks in 97.9 seconds (1.44 seconds preparation), with cleanup passing
and no owned game left running. The lifecycle group remained NOT TESTED because
the build does not register every required contract, so the aggregate remained
NOT TESTED. Artifact SHA-256:
`b654f43e1ccb62befd9d8ba1f26fbe44729d68504d280a7a22c284201f7e317d`.
Short telemetry recorded RADV VANGOGH/Mesa 25.0.7 at 480 by 272, ordinary guest
speed and frame-time tails; it makes no calibrated comparison or improvement claim.
Player/release data and launchers were unchanged.
The freshly rebuilt Deck native suite also passed 67/67 in 168.71 seconds on
`325e28d`, including the available renderer/backend lifecycle regressions. This
separate full-suite result does not fill the aggregate's missing contract row.

On physical Android (Mali-G52 MC2, driver r32p1), QUICK harness `738ed74` passed
all six real-game subchecks: copied-character boot, pad/settings, offline quest/
camera, active field bytes, photo freeze/step and fixed-scene telemetry. They took
142.4 seconds; the complete run including preparation and cleanup took 431.2
seconds. Six native-contract rows remained NOT TESTED because the ADB suite
adapter has no executable native-contract build context, so its aggregate also
remained NOT TESTED. Separate native 56/56 evidence above does not silently turn
these rows green. The isolated APK SHA-256 was
`a8f080a66530832854976887dcbffb529b00f5a9206ccf85db50c0cbd2016295` at application
source `751424a`. Cleanup passed and the owned package returned to the idle
launcher with no game PID. At that earlier checkpoint the transfer path exceeded
the original Google cap and the ledger had zero attempts. Later authorizations
and actual cloud attempts are recorded below.

## Bounded Google QUICK pilot (2026-10-12)

A later explicit authorization raised the task cap to 500 seconds per reservation,
at most two sequential attempts / 1000 seconds total. The existing private ledger
was migrated from 300 to 500 without resetting history. The runner retains a
300-second default; that authorization phase rejects 501 seconds or a separate
setup extension. A later phase is recorded separately below.

| Requested device | Observed capability | Result | Total reservation time |
| --- | --- | --- | --- |
| Pixel 5 (`redfin/30`) | Adreno 620, Vulkan 1.1.128; reported driver version 2149490688 | Setup FAIL: bounded `adb install` timed out after 180 seconds; gameplay NOT TESTED | 244.6 seconds |
| Pixel 6 (`oriole/31`) | Mali-G78, Vulkan 1.1.177; reported driver version 134221824 | Setup FAIL: official Android CLI installation timed out after 180 seconds; gameplay NOT TESTED | 233.6 seconds |

Runner sources were `171a3fa` and `fe02d74`; both attempted the existing isolated
developer APK at native source `751424a`, SHA-256
`a8f080a66530832854976887dcbffb529b00f5a9206ccf85db50c0cbd2016295`.
The owned host copies and private input hashes were verified. Neither installation
completed, so device-side game/save transfer hashes, loaded quest/framebuffer,
controls, photo and telemetry outcomes remain NOT TESTED. No game was launched,
and no game captures or performance comparison are claimed.

Both attempts consumed their allowance and were released, with creation/removal
and recognized final idle-list evidence retained privately. Total measured time
was 478.2 seconds, within both per-reservation and aggregate caps. There were no
retry reservations, cap extensions, billing/IAM/project changes or recompilation
of the APK/AOT/355 overlays. The first report's missing partial setup identity
prompted a runner fix that preserves verified metadata on setup failure; original
reports remain unchanged. These GPU families do not reproduce or rule out the
Dimensity 7025 / IMG BXM-8-256 report.

## Prepared deployment rerun (2026-10-12)

The next explicit authorization permits one additional attempt per original model,
two additional reservations total, targeting $1.50/600 seconds and capped at
$2/800 seconds each before credits at the verified $0.15/device-minute rate.
The two historical released rows remain unchanged in the ledger; a separate
authorization phase enforces the pair, count, duration and cost estimates.

Before spending that phase, the new staged deployment path was measured locally
on physical Android with the same APK/private fixture hashes and no game launch:

| Phase | Measured time |
| --- | --- |
| APK transfer, 203,390,550 bytes | 5.836 seconds |
| Package Manager install | 15.765 seconds |
| EBOOT transfer/hash | 0.859 seconds |
| ISO transfer/hash, 1,323,565,056 bytes | 236.666 seconds |
| Complete preparation including metadata/fixtures/cleanup | 266.624 seconds |

All hashes matched and the unique temporary APK directory was removed. This
validates the deployment implementation and local transport, not Google link
throughput. Setup now uses an explicit share of remaining inclusive time with
gameplay/cleanup reserved, rather than the stale 180-second install cutoff.
APK transfer and package installation have separate timing/output records;
timeouts preserve partial output. Synthetic tests reject corrupt transfers,
expired budgets, repeat model attempts, unauthorized counts/models/rates and
history resets.

The first rerun used runner `c3472b4` on Pixel 5 / Adreno 620, with the same
native source `751424a` and APK hash above. Its arm64/Vulkan preflight passed.

| Google deployment phase | Observed result | Measured time |
| --- | --- | --- |
| APK transfer | PASS; device SHA-256 matched; ADB reported 0.8 MB/s | 237.809 seconds |
| Package Manager install | PASS; `Success` | 4.502 seconds |
| EBOOT transfer/hash | PASS | 1.744 seconds |
| ISO transfer/hash | TIMEOUT; final hash unavailable | 291.012 seconds |

The inclusive reservation lasted 598.347 seconds; its estimated pre-credit cost
is $1.4959 at the checked rate, not an account charge. The remaining-budget
deployment limit was 539.348 seconds, protecting 120 seconds for checks and the
separate cleanup reserve. The temporary APK directory was removed, the owned
package was stopped, the exact reservation was released and the final official
CLI list showed no reservations. No game launched; quest, translation, photo,
framebuffer and telemetry outcomes remain NOT TESTED, with a setup FAIL.

Three attempts have now been consumed and released across both phases. This
rerun phase has consumed one of its two attempts. The remaining Pixel 6 attempt
is held: the current full-input deployment cannot fit the 800-second cap at the
observed link speed. The 1,323,565,056-byte ISO alone would take about 27 minutes
at the APK's reported 0.8 MB/s, before checks and cleanup. That is a planning
estimate, not a measured full-ISO duration or a prediction for Pixel 6. The
staged rerun establishes transfer as the Pixel 5 bottleneck; increasing the
Package Manager timeout alone cannot make this workload fit. Original failures
and private progress/cleanup evidence are retained. No further reservation,
persistent game-data upload or billing/IAM change was made. The maintainer has
now deferred Google testing and transfer/downloader work from the local QUICK
delivery. The unused attempt is not being spent in this stage; all original
FAIL/NOT TESTED reports and private reservation history remain preserved.
## Opaque-draw renderer checks (2026-10-11)

A bounded stationary-village comparison on `5e1fa00` used a physical Android
device, with hardware and driver details kept in local testing records. The
isolated ARM64/API29 build
reused compatible AOT objects and all 355 overlays. Resolution remained
1650×720 with Frame rate Auto selecting 30 FPS, GPU compatibility Auto
(self-test passed, Off),
GPU vertex decode Off, sharp textures Off and no replacement texture pack.

`MHP3RD_PERF_ALTERNATE=opaque` switched only the opacity optimization every
second. In the final 26 one-second samples of two runs, GPU-time medians were
26.5 → 24.8 ms and 27.6 → 26.0 ms. Adjacent on-then-off comparisons saved a
median 1.75 ms and 1.90 ms; 23 of 24 pairs improved. Both optimized medians
were 30 game FPS and 100% speed; the old path measured 100% and 99%. CPU
render medians increased by 0.2 ms in both runs. GPU frequency varied, so
these adjacent comparisons show reduced cost in this scene, not a fixed
performance guarantee.

Synthetic offscreen comparisons of the production shaders on the same Android
GPU covered 1,400 filtered, unequal-perspective-W, lit/unlit and alpha-test
cases: zero differing images, with a forced-unsafe control changing pixels.
EQUAL255 and GEQUAL255 checks drew pixels rather than passing vacuously.
Game captures were inspected at the same resolution; animation and wall-clock
state were not pinned for pixel-identical game comparisons.

Only the isolated diagnostic application was updated. The player's installed
`0.7.0-test.1` build, whose source commit is unknown, was not replaced. Moving
combat, other physical GPUs, raw GPU decoding, interpolation, prolonged play
and the separate Android surface-loss startup crash remain unverified by
these checks. No quality setting, phone-wide setting or upscaler changed.

## Combined alpha-path precision regression (2026-10-12)

PR #312 combines #301 and #302 against main `5b10e66`. The application fix
is at `987a31c`; the savedata fixture correction is at `0cd308f`. These
changes do not alter framework headers, generated game code or overlays.

Linux aarch64, Clang 19.1.7, SDL 3.2.10 and Mesa 25.0.7 software Vulkan
reproduced the filtered/perspective failure. Three vertex-alpha texture modes
at GEQUAL255 differed in 5,214 pixels each when discard was removed. A
constant opaque vertex input can interpolate just below one; it does not
prove that the original strict fragment comparison passes. The updated
regression compares complete RGBA buffers against an isolated rollback
renderer with the same linear sampling and no texture replacements. It failed
before the guard and passed afterward, without pixel tolerances or skipped
comparisons. The reference must cover more than 2,000 pixels, preventing an
empty-triangle pass. Texture-only alpha keeps its independent direct-color
checks.

Opaque selection now retains perspective-varying vertex alpha, including its
blend and discard operations. Through draws and the recognized texture-only
alpha paths still require the original opacity proofs. Nine relevant Linux
checks passed: normal, compatibility, broad legacy, opacity rollback, checked
renderer, trivial-alpha rollback, render/opacity helpers and savedata. The
corruption fixture flips a bit in an existing byte instead of assigning a
constant that could equal its random ciphertext; all 64 repeated savedata
runs passed. Full-suite and final hosted results are recorded separately in
the PR.

The earlier village measurements at `5e1fa00` above used broader eligibility
and do not establish the updated combined performance. The subsequent
physical comparison below measures the narrowed combined source.

## Combined alpha-path physical performance (2026-10-12)

Application source `397d125` was tested on Windows 11 x64 with a Radeon
RX 5600 XT (AMD 26.8.1), SteamOS with RADV VANGOGH (Mesa 26.2.0-devel),
and Android 14 with a Mali-G52 MC2 (r32p1). Host-only builds reused compatible
90-object AOT code and all 355 overlays; no framework, generated-code or
overlay rebuild was needed. These are developer artifacts with diagnostics,
not an exact release-package performance certification.

Each comparison used the same native artifact, copied save and prepared disc
image, original language, fixed developer RTC epoch, audio disabled and a
manual 30 FPS target. Optimizations on means both new rollback switches are
unset; off sets `MHP3RD_NO_TRIVIAL_ALPHA_VARIANTS=1` and
`MHP3RD_NO_OPAQUE_DRAWS=1`. The broad alpha/opaque alternating benchmark is
not an equivalent baseline because it also disables earlier alpha work.
Android APK variants contain identical native-library bytes and differ in
benchmark metadata, including these switches. Their private manifests are
separate from the public generated APK test contract.

Finite state-based navigation loaded the copied character, then started
quest 204 offline through the supported developer command. Both village and
camp were stationary. Readbacks checked the actual map, active quest,
unchanged dimensions, normal loading/speed modes, disabled cheats and inactive
photo mode. After at least 600 warmup vblanks, each scene supplied 20 distinct
one-second performance samples. The first two samples were excluded to keep
capture/write work outside the reported steady-state window. Two runs per
mode therefore contribute 36 samples per row. Mode order was on/off/off/on
on desktops and Android ×1, and off/on/on/off for Android ×2.

Medians below describe this workload; GPU times are logged at 0.1 ms precision.
Internal resolution is separate from presentation size: 960×544 on Windows,
1280×800 on Deck, and 1650×720 on Android. Android's widescreen aspect makes
×1 render at 623×272 and ×2 at 1247×544.

| Device / internal size | Scene | FPS off → on | Speed (%) off → on | GPU (ms) off → on | CPU render (ms) off → on |
| --- | --- | --- | --- | --- | --- |
| Steam Deck / 960×544 | Village | 30.0 → 30.0 | 100.0 → 100.0 | 1.4 → 1.4 | 9.2 → 9.2 |
| Steam Deck / 960×544 | Quest 204 camp | 30.0 → 30.0 | 100.0 → 100.0 | 1.0 → 1.0 | 6.7 → 6.7 |
| Windows / RX 5600 XT / 960×544 | Village | 30.0 → 30.0 | 100.0 → 100.0 | 1.0 → 1.0 | 5.0 → 4.9 |
| Windows / RX 5600 XT / 960×544 | Quest 204 camp | 30.0 → 30.0 | 100.0 → 100.0 | 0.6 → 0.6 | 3.6 → 3.4 |
| Android / Mali-G52 / 623×272 (×1) | Village | 30.0 → 30.0 | 100.0 → 100.0 | 17.5 → 16.9 | 18.4 → 18.2 |
| Android / Mali-G52 / 623×272 (×1) | Quest 204 camp | 30.0 → 30.0 | 100.0 → 100.0 | 16.9 → 15.1 | 13.2 → 14.1 |
| Android / Mali-G52 / 1247×544 (×2) | Village | 27.2 → 28.7 | 90.7 → 95.6 | 29.8 → 28.2 | 17.8 → 17.3 |
| Android / Mali-G52 / 1247×544 (×2) | Quest 204 camp | 29.2 → 29.5 | 97.5 → 98.2 | 26.5 → 27.3 | 12.6 → 12.5 |

The desktop GPU medians did not change at the logging precision. Android ×1
reduced GPU medians by 0.6 ms in the village and 1.8 ms in camp, while camp
CPU render time increased by about 0.9 ms. At ×2 the pooled village median
improved, but the direction did not repeat: the first off/on pair measured
25.1/29.1 FPS, while the later on/off pair measured 28.3/30.0 FPS. Camp also
reversed direction between pairs. The ×2 data therefore establishes neither
a repeatable improvement nor a regression, and the optimized runs still did
not sustain the target speed. Sampled Android GPU clocks varied. Thermal status remained 0 in fresh HAL
readings, while the vendor GPU limit index varied from 0 to 6 during monitoring.
The specific limiter and its contribution to frame time were not identified;
these readings do not establish a fixed clock or unrestricted GPU operation.

All 16 accepted device runs completed with owned-process cleanup. Their
representative village/camp captures were inspected: matching geometry,
textures and UI, with no visible alpha corruption. Animation frames were not
pixel-identical. Initial fixture/log-read/map-transition failures and the
Windows run missing overlays remain in private evidence and are excluded
from the table. Normal player/release installations, saves and launchers were
not replaced. Moving combat, audio performance, multiplayer, long thermal
sessions, other GPUs and the independent historical Android surface-loss
startup issue remain outside this validation.

## Issue #320 development checks (2026-10-19)

Tested source: `a2cfb175c297` on `small-fixes`. The Windows and Android builds
were made from the same C++ working tree before committing. Rebasing onto newer
main CI/packaging commits left all tested implementation files unchanged.

- Windows: Release application build and all 72 CTest cases passed, including
  texture-pack, font, input, touch-menu and renderer/UI contracts. Font spacing
  comparisons were inspected in game; synthetic touch-menu captures were also
  inspected.
- Android: an arm64-v8a Release APK was built with all 355 compatible prebuilt
  overlays. Signature, ZIP integrity/alignment, Android 10 imports, ARM64 and
  16 KiB ELF LOAD alignment checks passed. The reporter manually tested this
  APK and reported it working. Its SHA-256 is
  `bec73f13fc981338fa6c608e19ea804641dd25d7243351abcc7867b6e4627ab7`.

The Android device model, OS version and individual gameplay scenarios were
not recorded. This general smoke report does not certify each issue item,
multiplayer, long sessions or haptic behavior. Representative legacy texture
packs, other fonts and macOS/Linux/Steam Deck behavior remain separate checks.

## Audio menu development check (2026-10-19)

Windows x64, source commit `e7b96c5` (`sound-settings`, based on `c40b979`):

- All 73 registered CTest checks passed across a full run and a targeted rerun after correcting one expected test rounding value. This includes the new synthetic game audio tests and the real renderer/UI menu tests in normal and compatibility configurations.
- A bounded game run with isolated data loaded all 355 existing overlays. The game audio diagnostic reported BGM/SFX at 7/7. Changing BGM through the new Audio menu produced BGM 6/7 with SFX still 7/7 after resuming. The Master Volume, BGM Volume and SFX Volume menu captures were inspected.
- The user also confirmed that the controls work and both volume levels change during gameplay.
- Original Options screen round trips, save/restart persistence of these levels, and other platforms were not checked in this pass. No coverage percentage was measured on Windows.

## Barioth encounter and sparse CPU skinning (2026-10-18)

Tools-only source `fe1711092d156c84927484600717ac57a5cef2b0` and optimized
native source `d9ea6820047ca290b2004c7f0237db5ea9352a1a` use the same cached
native AOT code and all 355 compatible overlays. Framework/runtime headers,
generated code, overlays, shaders and graphics quality are unchanged. Testing
used an Apple M1 Mac with 8 GiB, macOS 27.0 (26A428), Apple M1 Vulkan 1.1.357
and MoltenVK 1.4.2. These are developer artifacts with observation/camera
diagnostics, not exact release-package certification.

A private baseline CPU sample identified weighted vertex decoding and renderer
submission. Twenty post-ready trace windows in original Tundra quest 507 counted
47,410,890 indexed min/max-span vertices for 14,887,446 index references
(3.1846 times as many; worst primitive 32.75 times). This includes all skinned
scene draws, rather than attributing every draw to Barioth. The old decoder
skins unused vertices between sparse indices. Eligible sparse CPU draws now
gather only referenced vertices, retaining the same index order and arithmetic.
Full spans remain for temporal interpolation, which can need vertices referenced
only by an older frame; interpolated 45/60/90/120 FPS receives no performance
improvement claim. Raw GPU, through, morph and noncontiguous paths also retain
their existing behavior. `MHP3RD_NO_SPARSE_VERTEX_DECODE=1` restores the old path.

The [automatic encounter](DEBUG_MENU.md#fast-barioth-encounter) uses the original
quest departure/map/actor/model lifecycle, then relocates the hunter near the
landed Barioth. Ready observations confirmed kind 5, its live PMO model, original
Tundra area 7, five actors (one Barioth and four small monsters), advancing quest
time and normal lock-on camera. Snow, effects, AI and attacks remain active.
This deliberately tests the normal environment rather than an arena. Original
quest abandonment hit Barioth's deleting destructor once and the base actor
destructor five times, ended the target lifetime, released the test camera and
returned to village readiness with an empty monster table. All four copied save
files remained byte-identical. Only owned developer processes/copies were
cleaned up; player installations and unrelated sessions were not stopped.

Six fixed-mode runs compare the same `d9ea682` native artifact with the rollback
switch set (off) versus unset (on), in order off/on, on/off, off/on. Both use the
same copied save/settings/game, developer RTC epoch 1780000000000000 us, native
AOT/overlays, original language, muted audio, no texture pack, 480×272 internal
resolution (×1), 960×544 window, Immediate presentation and manual 30 FPS.
GPU compatibility Auto resolved to off in every run. The collector warmed up
to quest tick 89550 and measured approximately 300 further original 30 Hz ticks
per run, retaining actual start/end ticks. RTC was pinned; identical AI/RNG
trajectories or pixel-identical motion are not asserted. Separate-process AI
and draw-count variation remains a limitation. No external game/compiler was
observed by the private timing guard during these six runs. AC power and no
recorded thermal warning do not establish fixed clocks or sustained thermals.

Values below are medians of the three run medians (frame-time p95/p99 are
medians of the three per-run percentiles). CPU/GPU times are per game frame;
wait includes pacing. Vertex space is each one-second window's frame maximum.

| Metric | Old CPU path (off) | Sparse CPU path (on) |
| --- | --- | --- |
| CPU guest / render / guest + render, ms | 3.786 / 6.915 / 10.803 | 3.738 / 6.409 / 10.383 |
| GPU, ms | 1.167 | 1.231 |
| Wait / pacing, ms | 22.580 / 21.829 | 22.987 / 22.134 |
| Vertex space, MiB | 5.318 | 3.548 |
| Speed, % | 99.979 | 100.022 |
| Game frames/emulated s / presents/wall s | 29.971 / 29.975 | 29.971 / 29.978 |
| Present time median / p95 / p99, ms | 33.308 / 36.230 / 36.918 | 33.311 / 36.063 / 37.299 |

The paired render medians decreased by 3.2%, 7.0% and 11.4%; the aggregate
render median decreased by 7.3%, combined guest/render by 3.9% and vertex space
by 33.3%. The target already sustained 30 FPS: these results establish reduced
CPU work and buffer space, not a higher game rate or improved p99. GPU time
did not improve. The ready/final captures for all six runs were inspected;
normal attacks and camera motion differ, with no observed geometry/texture
corruption. A prior interrupted ensemble with a concurrent game/build in an
unrelated worktree was excluded entirely.

Two separate 600-tick continuous-scene diagnostics alternated the same switch
each second. Each supplied ten on and ten off telemetry windows after warmup.
Off/on render medians were 7.119/6.310 ms and 6.536/5.700 ms (11.4% and 12.8%
lower); render time per draw decreased by 8.1% and 10.4%. These corroborate the
CPU effect in evolving combat, but mixed-mode frame-time/vertex-space peaks
are not pooled with the fixed-mode comparison. Both diagnostics completed
without observed external game/compiler interference and their actual game
captures were inspected.

An additional two pairs compared the separate tools-only `fe17110` and final
`d9ea682` artifacts, in before/after then after/before order, with the same
settings and fixed quest-tick interval. The guard observed no external
game/compiler interference. Medians of the two per-run summaries were:

| Metric | Before (`fe17110`) | After (`d9ea682`) |
| --- | --- | --- |
| CPU guest / render / guest + render, ms | 3.510 / 7.010 / 10.582 | 3.630 / 6.380 / 10.040 |
| GPU / wait / pacing, ms | 1.227 / 22.845 / 22.092 | 1.275 / 23.336 / 22.557 |
| Speed, % | 100.010 | 99.957 |
| Game frames/emulated s / presents/wall s | 29.971 / 29.974 | 29.971 / 29.961 |
| Present time median / p95 / p99, ms | 33.336 / 35.943 / 36.815 | 33.330 / 36.070 / 37.195 |

Render medians decreased by 8.9% and 9.1% in the two pairs. Aggregate render
decreased by 9.0% and combined guest/render by 5.1%, while guest and GPU time
increased slightly. This agrees with a CPU rendering improvement without
establishing an FPS, p95 or p99 gain. All four runs' ready/final frames showed
the actual loaded Barioth and original environment without observed rendering
corruption; normal AI poses were not pixel-identical.

Real-game correctness diagnostics compared over 608,000 gathered draws with
full-span reference vertices, with zero differences. Separate 120 FPS replay
checks reported zero of 522,240 pixels different, including the active combat
window. Ready and combat captures were inspected by the agent for recognizable
Barioth geometry, hunter/camera, terrain, snow/effects and texture correctness;
this is not human play or a completed hunt. The renderer-enabled macOS suite
passed 81/81 tests. A separate headless coverage-stub release build with
`MHP3RD_DEBUG_MENU=ON` excluded developer debug sources/macros/symbols and
encounter strings. Public synthetic tests and ordinary CI require no game data
or timing thresholds.

The severe 5–20 FPS Android symptom in #330 was not reproduced on this Mac.
The follow-up below records Windows and Redmi 12C measurements; Lenovo and
Snapdragon 685 remain unverified. This work identifies a combat CPU cost; it
does not establish the complete cause of that
device-specific slowdown or resolve the separate 120 FPS animation report.
Arbitrary spawning, selective live despawning, an arena, other monsters, audio,
complete hunts, multiplayer and sustained thermal behavior remain unverified.

## Windows and Redmi 12C Barioth follow-up (2026-10-19)

These measurements extend the previous Mac investigation. The original native
sources remain `fe1711092d156c84927484600717ac57a5cef2b0` (tools-only baseline)
and `4fde48f4b62be51fe7decdef40d1d75d631d8fc1` (optimized; its executable
implementation is `d9ea682`). They predate integration with main `5bacba3`.
All runs reuse native AOT and all 355 compatible overlays. Framework/runtime
sources and headers are unchanged. No paid devices were used.

Windows: Windows 11 Pro 10.0.26200, Ryzen 5 5600, Radeon RX 5600 XT driver
32.0.21045.5002, MSVC 19.44 Release. Six fixed-mode rollback runs use the same
`4fde48f` executable, in off/on, on/off, off/on order. Controls match the Mac
30 FPS fixture: pinned RTC, original Tundra quest 507, tick 89550 warmup and
300 measured game ticks, 480×272 internal / 960×544 window, Immediate,
compatibility Auto resolved off, no texture pack and muted audio. A private
process guard recorded no competing game/compiler. All twelve endpoint game
captures were inspected; attacks, jumping and close camera movement showed no
observed geometry/texture corruption. Medians of the three per-run summaries:

| Metric | Old CPU path | Sparse CPU path |
| --- | --- | --- |
| CPU guest / render, ms | 9.007 / 6.737 | 7.313 / 5.912 |
| GPU / wait / pacing, ms | 0.844 / 17.650 / 17.459 | 0.859 / 20.137 / 19.947 |
| Vertex space, MiB | 5.610 | 3.558 |
| Speed, % | 99.966 | 99.963 |
| Game frames/emulated s / presents/wall s | 29.971 / 29.955 | 29.971 / 29.967 |
| Present time median / p95 / p99, ms | 33.180 / 43.948 / 46.457 | 33.357 / 37.291 / 43.648 |

Render decreased 12.2% and vertex space 36.6%. Guest time and scheduling varied;
the percentile improvement was not consistent in each pair, and these results
do not establish a generally higher game rate or better p99. The full Windows
suite passed 74/74 on this source.

Android: **Redmi 12C, model 22126RN91Y**, MediaTek MT6769V/CZ (platform mt6768),
Android 14/API 34, arm64-v8a, Mali-G52 MC2 Vulkan 1.1.177 driver r32p1. This is
**not** the Redmi Note 13 / Snapdragon 685 reported in #330. Its USB connection
was reached through a loopback-only SSH/ADB forward. A separately signed,
debuggable `.devicetest` package used app-private copied fixtures; the stable
player installation was not replaced or launched.

NDK r28c/API 29 Release developer builds use the same 90 native AOT objects and
all 355 overlays. The complete APK native inventories and generated testing
contracts were compared: only `libmain.so` differs; the 358 dependency/overlay
libraries are byte-identical. Neither comparison uses interpreter fallback.
The normal game loads its actual Barioth/PMO, Tundra area 7 and five actors
(one Barioth, four small monsters). AI/attacks, snow and effects remain active;
the hunter does not attack and Barioth's observed HP remains 2508.

Each scale has three paired ten-second comparisons, baseline/fixed, fixed/baseline,
baseline/fixed, with tick 89550 warmup and 300 measured ticks. Same copied saves,
RTC, manual 30 FPS, no texture pack, muted audio and GPU compatibility Auto
resolved off. Actual presentation is FIFO at a 1650×720 surface; actual internal
targets are 623×272 (×1) and 1247×544 (×2), reflecting the phone's aspect ratio.
Medians of the three per-run summaries (p95/p99 are medians of per-run percentiles):

| Metric | ×1 baseline | ×1 fixed | ×2 baseline | ×2 fixed |
| --- | --- | --- | --- | --- |
| CPU guest / render, ms | 10.226 / 16.798 | 10.736 / 15.682 | 11.806 / 17.019 | 11.686 / 15.443 |
| GPU / wait / pacing, ms | 18.807 / 6.560 / 3.885 | 17.628 / 6.891 / 4.013 | 27.296 / 7.412 / 0.151 | 27.089 / 7.089 / 0.678 |
| Speed, % | 99.954 | 100.064 | 94.976 | 97.466 |
| Game frames/emulated s / presents/wall s | 29.971 / 29.965 | 29.971 / 29.979 | 29.971 / 28.465 | 29.971 / 29.203 |
| Present time median / p95 / p99, ms | 33.111 / 37.805 / 53.830 | 33.175 / 36.935 / 53.317 | 33.980 / 38.127 / 74.897 | 33.992 / 37.630 / 71.824 |

Render decreased 6.6% at ×1 and 9.3% at ×2. GPU cost remains significant at ×2;
this CPU optimization does not establish a GPU fix. All 24 original ready/final
frames were inspected, including real attacks/jumps and effects, with no observed
corruption. USB charging stayed at 100%; recorded battery temperature rose from
28.3 to about 33 °C, with thermal HAL status 0. These observations do not prove
fixed clocks or sustained thermal stability. AI paths, draws and scheduling vary.

After the reporter identified takeoff/air attacks, two reversed pairs measured
1800 ticks (60 emulated seconds) at ×2 on the same original before/after sources.
Aggregate CPU render was 15.437→14.590 ms, speed 96.282→97.444%, present rate
28.856→29.205 FPS and median/p95/p99 33.943/37.991/71.342→33.970/38.013/72.620 ms.
The eight endpoint frames were inspected; normal combat continued without
observed corruption. This longer comparison confirms lower CPU work but **does
not improve p99**. Individual long frames and camera motion remain; endpoint
pictures alone cannot assign a spike to a particular attack. The severe
5–20 FPS symptom has not been reproduced on this different phone.

Eight native Android configurations passed: GE/default, scalar, old sparse path,
checked decode, debug/encounter, performance overlay, compact and compact+log.
The checked case uses the exact trace environment registered by CTest. An initial
manual invocation omitted required trace switches; it was corrected and all eight
were rerun without changing tests. Public CI uses synthetic fixtures and has no
private combat workload or flaky performance thresholds.

### Integration and conservative-shader measurements

After integration with main `5bacba3`, the native C++ snapshot `30ab0d0`
contains the same sparse implementation as `d189dce`. Harness `7ad8250` adds
incremental Android log transport. Additional reversed pairs used the previous
controls: two 300-tick Mac pairs, two 900-tick Windows pairs and two 900-tick
Android ×2 pairs. These are separate ensembles, not pooled with the earlier
sources/durations. Medians of the two per-run summaries:

| Metric | Mac old / sparse | Windows old / sparse | Redmi ×2 baseline / fixed |
| --- | --- | --- | --- |
| CPU guest, ms | 3.082 / 2.917 | 8.996 / 8.971 | 11.291 / 11.894 |
| CPU render, ms | 5.904 / 4.876 | 6.773 / 5.962 | 14.952 / 14.930 |
| GPU, ms | 1.181 / 1.213 | 0.845 / 0.844 | 26.777 / 26.434 |
| Wait, ms | 24.242 / 25.438 | 17.766 / 18.813 | 8.094 / 7.865 |
| Pacing, ms | 23.597 / 24.797 | 17.566 / 18.616 | 0.332 / 1.051 |
| Speed, % | 99.949 / 100.012 | 99.937 / 99.946 | 97.332 / 96.933 |
| Presents/wall s | 29.968 / 29.979 | 29.969 / 29.951 | 29.157 / 29.066 |
| Present median, ms | 33.382 / 33.401 | 33.377 / 33.305 | 33.679 / 33.610 |
| Present p95, ms | 35.581 / 34.895 | 43.707 / 43.652 | 37.064 / 37.642 |
| Present p99, ms | 36.361 / 35.522 | 45.174 / 45.444 | 71.894 / 69.781 |

Game rate remains 29.971 frames per **emulated** second in all six columns.
The Mac and Windows CPU render reductions were 17.4% and 12.0%. The longer
Android comparison was effectively flat, with differing AI paths/draw counts;
there is no general Android FPS or p99 improvement claim. All 24 endpoint game
frames were inspected. All four Mac before/after endpoint comparisons were
pixel-identical (0/522,240 pixels different). Normal terrain occlusion and close
camera movement occur in the original map; a frustum check cannot certify
occlusion. The collector also rejects a target that leaves the frustum during
warmup. Android retained a warm application pipeline cache between owned
restarts; its byte identity was not pinned, so these are not cold-start claims.

A separate approximately 80-second diagnostic video at native source `7ad8250`
started without the owned application's pipeline-cache file, which was restored
afterward. Driver/system caches were untouched. Inspected frames showed actual
Barioth jumps, attacks, landings, snow and effects without observed corruption.
The worst sampled second was 25.44 presented FPS. Encoding/readback changes the
workload: this video is excluded from timing comparisons and does not identify
the cause of each frame-time spike.

**Forced compatibility On reproduces a severe slowdown on this Redmi 12C.**
It does not establish that the reporter's Snapdragon 685 Auto selected On.
On retains its conservative feature/frames-in-flight policy; Auto remained Off
in the normal comparisons. One 1800-tick before/after pair compares native
`7ad825092be9bc03e5c6889b3e893c84b3d20de3` with
`92031b2e54b7813b5e30649d6aff38bdbcda64d9`, using the same ×2/manual-30/RTC/quest controls:

| Metric | Original plain shader | Alpha-free plain variant |
| --- | --- | --- |
| CPU guest / render, ms | 16.822 / 33.208 | 15.573 / 30.977 |
| GPU timestamp | unavailable | unavailable |
| Wait / pacing, ms | 63.209 / 8.204 | 57.666 / 7.988 |
| Speed, % | 29.231 | 31.461 |
| Game frames/emulated s / presents/wall s | 29.971 / 8.762 | 29.971 / 9.429 |
| Present median / p95 / p99, ms | 112.214 / 145.342 / 158.634 | 104.313 / 139.077 / 149.480 |

The variant has no specialization constants and no SPIR-V `OpKill`, and is
selected only when the existing pipeline proof needs no alpha test. Genuine
alpha rejection retains the original plain shader. A refused optional module
or pipeline also retains it. All other compatibility workarounds and Auto
activation remain unchanged. The normal specialized and tested-plain SPIR-V
binaries were byte-identical before/after with the same compiler. Public shader
regressions enforce absence of `OpKill`/specialization constants in the new
variant and retain rejection in the tested variant; the old source fails that
regression. Actual alpha/depth/texture pixel contracts also pass the rollback
and refused-pipeline paths.

Because AI/clock variation confounds separate processes, two additional
900-tick continuous diagnostics used exactly the same native libraries with
`alternate-alpha` (harness `0f0c222`). Complete one-second windows strictly
inside each measurement interval were matched to native `perf_second`
observations; all scalar render values agreed within their printed rounding.
The existing alpha switch selects the tested/plain path off and eligible
alpha-free pipelines on. All game AI, geometry and effects remain active.

| Diagnostic | Old / new windows | CPU render, ms | Fence wait, ms | Presents/wall s |
| --- | --- | --- | --- | --- |
| First | 45 / 44 | 28.20 / 28.75 | 50.77 / 47.23 | 9.3 / 9.4 |
| Second | 48 / 49 | 34.25 / 35.30 | 52.74 / 49.59 | 8.5 / 8.8 |

Recorded draws were closely matched: 553/552 and 652/652 per frame. Fence wait
fell 7.0% and 6.0%, while CPU rendering increased slightly and presentation
remained around 9 FPS. This is a modest synchronization-wait improvement, **not
a complete fix for the severe compatibility-mode slowdown**. GPU timestamps
remain disabled by that policy; no hardware early-depth counter was captured.
The inferred mechanism is supported by the compiled shader and Arm's linked
Bifrost documentation in [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md), rather
than claiming direct hardware-counter proof. Mixed-mode present percentiles
are not presented as fixed-path results. Eight original/candidate/diagnostic
endpoint frames were inspected without observed corruption.

Final native validation on `92031b2`: all 85 renderer-enabled macOS cases passed
in one serial run; all 78 Windows cases passed. The eight relevant Android
native configurations had already passed on the integrated implementation.
The full Python suite passed 366 tests with six explicit skips after adding
alternating contracts; affected suites also exercise the final rollback
allow-list and warmup-visibility guard. These ordinary tests use synthetic
fixtures and no combat timing thresholds. Original player installation/settings
remain untouched; only the separate test package, copied fixtures and owned
processes were used. Tablet/Snapdragon 685, audible playback, full hunts,
multiplayer and sustained thermal stability remain unverified. The user will
perform a later tablet check; this PR remains draft and does not close #330.
