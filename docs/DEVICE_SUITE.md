# Automatic pre-release suite

Implementation tracking: [#310](https://github.com/TeamGDB/Yakumo/issues/310).
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
