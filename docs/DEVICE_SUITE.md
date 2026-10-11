# Automatic pre-release suite

Implementation tracking: [#310](https://github.com/TeamGDB/Yakumo/issues/310).
The current deliverable is the **QUICK** stage: ten limited automatic categories,
with real-game smoke and public synthetic/native contracts labelled separately.
The earlier extended matrix is retained as deferred follow-up; its definitions,
failures and missing fixtures do not establish a complete release sign-off.

The suite adds real-game observations to the shared device runner. Public native
contracts remain mandatory; their synthetic inputs are not device gameplay
evidence. Physical controls, audible playback and long hunts retain separate
human sign-offs. Developer shortcuts never satisfy exact release-artifact rows.

## Contract audit

Each top-level scenario has bounded subchecks. An action acknowledgement is not
an outcome. Missing fixtures, unsupported observation channels and unimplemented
subchecks remain NOT TESTED with a reason. The following map records the initial
audit before implementation; availability is reported per run.

| ID / category | Public contracts to reuse | Real-game oracle / initial gap |
| --- | --- | --- |
| 01 Installation/update/boot | installer, user-data and Unicode path suites | Actual installed executable hash, prepared input hashes, loaded character and scene; compare copied saves/settings before and after update. Portable and installer UI variants need separate evidence. |
| 02 Settings/controls | settings, bindings, touch, renderer UI keyboard/virtual gamepad/menu contracts | Read effective values and persisted file, restart, observe the game's pad samples through finite keyboard/gamepad/touch actions and neutral release. Presets, combinations and reset need actual UI operations. |
| 03 Quest/combat/camera | debug quest start, game camera, lock-on | Read loaded task/map/active quest ID, advancing quest clock, camera and monster health; compare movement/attack/area/return checkpoints. Untraced player/action fields must not be guessed. |
| 04 Save/backup | savedata and renderer UI save screens | Actual save/load requests, independent file inventories for backup/export/import, preservation on rejected/cancelled import; restart and load copied result. |
| 05 Translation | text/runtime/document and quest latency suites | Read actual loaded menu/dialogue/catalog/active quest strings against private expected keys, reopen, original fallback; Japanese/patched images need separate hashes and rows. Application logs alone do not prove displayed text. |
| 06 Mods/textures/layered look | mods, texture pack, equipment models, renderer UI | Imported/backup file inventories, actual replacement pixels, real equipment bytes and savedata unchanged while model selection changes. A synthetic replacement needs a measured real-game texture target. |
| 07 Photo tools | free camera, frame-step, PNG and renderer held-frame contracts | Actual camera state, kernel vblank/clock and quest clock, held/stepped captures, HUD on/off pixels, decoded screenshot dimensions/content. |
| 08 Two-peer multiplayer | ad hoc protocol/client/server/socket and kernel HLE loopback | Two independently isolated game peers, common hall/quest, received player state, traffic and disconnect. Host/join acknowledgements and synthetic sockets do not establish a game quest. No quest cheats. |
| 09 Platform lifecycle | renderer UI focus/audio/backend lifecycle; Android Java lifecycle/display policy | Actual focus/background/return and resumed guest clock, audio samples, controller attach/release; Android rotation/surface/display observation. Synthetic SDL events must be labelled separately from OS/device lifecycle. |
| 10 Performance | pacing/interpolation/frame statistics/overlay contracts | Fixed routes on the same physical device, resolution, mode and settings; warm-up and repeated baseline/candidate samples, guest speed, timing tails and measured load intervals. Thresholds need independently measured baseline noise. |

## Initial implementation boundaries

The existing six scenarios contain four manual checklists and two automatic
launch/quest checks with required visual review. Preserve those review workflows.
Build the new automatic suite separately, using the existing ownership,
fingerprint, evidence-confinement and bounded-cleanup mechanisms. Add structured
read-only observations only in explicit developer testing mode; keep ordinary
player behavior unchanged. Public suite definitions carry no fixture contents.

Cloud pilot: validate locally first; at most two sequential owned reservations,
300 seconds each including activation, transfers and cleanup. Keep a private
ledger across resumed sessions. An incomplete upload is an incomplete pilot,
not permission to extend the deadline or retry.

## Running the implementation

```sh
python3 profiles/mhp3rd/tools/device_suite.py --profile quick --list
python3 profiles/mhp3rd/tools/device_suite.py --config device-testing.local.json \
  --profile quick --devices mac windows deck android --seconds 360 --output-parent out
# Focused reruns use the same public definitions:
python3 profiles/mhp3rd/tools/device_suite.py --config device-testing.local.json \
  --profile quick --devices mac --scenarios 01 03 07 --seconds 180 --output-parent out
# Deferred deeper checks require an explicit selection:
python3 profiles/mhp3rd/tools/device_suite.py --config device-testing.local.json \
  --profile extended --devices mac --seconds 1800 --output-parent out
```

`make device-suite DEVICE_CONFIG=device-testing.local.json` accepts additional
arguments through `DEVICE_ARGS`. The private configuration extends the shared
runner's local/SSH/ADB artifact contract. `observations: true` requires a
developer artifact compiled with the debug menu; the runner enables both debug
commands and the explicit device-test gate. Exact release artifacts keep separate
rows and prerequisites. A successful developer run does not certify their UI or
installation packaging.

Each session creates `report.json` and `suite.md` in a new ignored evidence
directory. Exit codes are 0 for all required outcomes passing, 1 for a failure,
and 2 for incomplete coverage. The aggregate lists devices and subchecks that
were never reached. Keep the configuration, expected text, game/save inputs,
navigation captures and reservation ledger private. Do not attach their contents
to a pull request.

QUICK is the default profile. It covers copied-character boot and preservation,
input/settings release, an offline quest/camera smoke, active quest text and photo
freeze/step through the real game. Installation/settings, save/backup, mod/texture/
equipment, two-peer transport and lifecycle/PCM groups run existing public native
oracles. Configure `native_test_dir` as a relative path to the owned CTest build;
missing registered executables or unsupported adapters remain NOT TESTED. Each
contract records its actual executable hashes, test names and output. These rows
do not claim real imports, full multiplayer Hall/hunts or OS lifecycle completion.

Offline core setup uses `offline-quest-204` (or another declared ID), the existing
developer `quest start` command and independent loaded task/map/ID, advancing
timer and no-cheat checks. `ensure-offline-quest-204` can reuse a verified active
offline quest. Both reject multiplayer and release-player use. Normal counter,
catalog and gate navigation is a separate focused regression, not setup for the
core smoke. Failed subchecks retain their verdict and evidence; bounded recovery
resets only owned copies before an independent subsequent check.

QUICK performance collects three short fixed-scene samples of guest speed,
presented/game FPS and frame-time tails, with resolution and GPU/driver evidence.
It asserts valid unpaused telemetry, not a calibrated non-regression threshold.
Repeated comparisons, optimization claims and extended interpolation/fast-forward
variants remain deferred. A local warmed run should take a few minutes; first
device installation/transfers are reported separately and still obey total caps.

## Private fixture contract

`suite_fixtures` contains finite navigation programs using the same actions as
the public scenarios. Fixtures cannot invoke other fixtures. Navigation references
require a reviewed crop, file hash and matching device identity; they guide input
and never constitute the result oracle. Assertions use fresh guest observations,
file/settings inventories and bounded image/audio decoders. Missing fixture
navigation or independent expectations produces NOT TESTED.

`game_variants` pins each Japanese/patched image and selected language file by
SHA-256. Text expectations contain the actual source field reference, expected
UTF-8 bytes and `covered` or `original-fallback` classification. Derive those
expectations from private source archives and language inputs. Active quest
references relocate the source catalog's offset words; do not assume the inline
title is an offset table. The checker reads the resulting guest strings, rather
than accepting an application count or language-selection acknowledgement.
Developer loader observations also expose completed original/fallback text
blocks, independently of whether any language key was applied. Nested dialogue
tables use their source-derived `base_offset` within the observed block; invalid
or partial loader ranges supply no address. These byte readbacks complement the
synthetic renderer contracts; they do not certify all real-world text layout.
An image variant may override `suite_fixtures` and `navigation_references` for
menus whose source artwork differs. Overrides inherit the same finite-program
and hardware/hash checks and are cleared when the original inputs are restored.
Navigation matches execute zero outcome assertions: the actual loaded state and
expected text bytes still determine the verdict. Covered text expectations should
differ from the original source bytes so disabling translation is detectable.
For paged NPC dialogue, `menu_advance` stops when its reviewed navigation region
appears, with at most four individually released input presses and a shared
deadline. Each press waits for guest vblanks before another can be sent. Exhausting
the finite bound fails navigation; matching the cue still supplies no outcome
assertion. This avoids assuming that different translated text lengths need the
same number of confirmation inputs.
An optional authored colour marker can gate presses while dialogue is still
typing. For replacement textures, colour counts can normalise brightness while
retaining a bounded channel tolerance; lighting can change RGB channels unequally.
Use an on/off/on comparison on the same scene and require the marker to disappear
when the pack is disabled. A changed image or a replacement log alone is insufficient.

Performance profiles declare finite preparation and route programs, scene state
and execution mode, with a distinct pinned baseline artifact. Collection runs
three repetitions of each artifact on the same host, warms up, and retains guest
speed, raw steady-state frame times and startup/load intervals. Regression budgets
derive from measured baseline spread; unstable baselines remain incomplete.
Resolution or settings differences invalidate the comparison. No optimization
claim follows from presented FPS alone.

The implementation is incremental. Fixture discovery and remaining device
capabilities are tracked in #310; a definition's presence does not mean its
required outcomes have been validated. The aggregate is the record of what ran.
