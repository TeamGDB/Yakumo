# Quest translation regression scenario

Run this scenario with a built game and a locally supplied translation. It is
for a human or a visual agent; public CI runs the synthetic latency regression
instead. Never upload game data, saves, translation contents or private paths.

## Prepare an isolated run

Use `python3 profiles/mhp3rd/tools/translation_smoke.py --help` for the bounded
launcher. Supply the exact executable, compatible overlays, a prepared game
folder, a copied-save source and a translation covering the chosen quests.
The launcher was verified on macOS; Android still needs an ADB/device launcher
and Windows device launching remains unverified. Use the same capture matrix
with the bounded platform launchers in the local testing guide.
The launcher creates a new output directory, copies saves and the translation,
links read-only game inputs, and records binary/translation fingerprints.
It does not change a developer or release installation.
Game inputs may be read-only links to an existing installation. Save/settings
links that escape their source directory, directory links inside saves and
special save files are rejected before creating a session. The selected
translation is staged with a fixed filename inside the session directory.
An optional output parent must already exist under the OS temporary directory
or this checkout's `out/`; other destinations are rejected.

Run two independent sessions from the same save source:

1. Normal mode, with the default memory-search budget.
2. Diagnostic mode, adding `--unlimited`. This is a comparison, not an acceptable
   performance fix. Its existing search can pause the game noticeably.

Use the same quest IDs, star levels, controller and language in both sessions.
Record the source commit alongside the binary hash. Do not compare screenshots
from different quest records or claim that a translated menu proves quests work.

## Capture matrix

| Capture | Action | Inspect |
| --- | --- | --- |
| `01-menu` | Open a shop or smithy | Known translated text as a positive control |
| `02-village-list` | Open a village quest star level | Names on the first visible screen |
| `03-village-details` | Select a covered quest and open its details | Title, objective, failure condition, description, monsters and client |
| `04-reopen` | Close, choose another star level, then return to the first | Freshly reloaded names and details |
| `05-hall-list` | Open a Guild Hall quest star level | Covered names without a long wait |
| `06-hall-details` | Open details for a covered Hall quest | All covered fields |
| `07-active-quest` | Accept and depart on a covered quest; open quest information | Same translated quest fields in the active quest |
| `08-original` | In a separate original-language run, inspect the same quest | Original text remains intact |

Capture immediately when each screen opens, then again after one second if it
still shows original text. Do not wait tens of seconds and call that a pass.
A human can record the window; an agent can append a finite capture command:

```sh
printf '%s\n' '0:shot 03-village-details' >> "$run_dir/input-live.txt"
```

The launcher prints the actual local run directory. The capture extension is
`.bmp`. Logs alone are not visual evidence: open every required screenshot and
compare its strings against the local translation. A field absent from the
translation is expected to remain original; do not report it as a failure.
A translation too long for a fixed field is a separate layout limitation.

## Verdict and evidence

Use the generated `review.md` to record **PASS**, **FAIL** or **NOT TESTED** for
each row, the quest ID/star level and expected/observed strings locally. Include
both the immediate and delayed captures when timing fails. Mark unavailable
languages, devices or quest types NOT TESTED, rather than assuming coverage.

A pass requires translated list and detail fields within one second of display,
correct repeat loads, no corruption/crash, and no translation-caused recurring
stalls. Review `MHP3RD_PERF` and `MHP3RD_TRACE_TEXT` next to the captures; record
observed spikes without attributing unrelated loading/save spikes to text.
The one-second allowance is a proposed regression ceiling, not a measurement
of the game's actual load deadline. Final-load hooks should translate before
first display whenever possible.

The synthetic `mhp3rd_quest_translation_latency_tests` measures this ceiling in
30 game frames over invented quest data. It tests twelve catalog loads with real-shaped completion notifications and
successive active quests, with a bounded search-work requirement. Each active
quest must translate on its first frame. It cannot prove
real-game loader/decryption/relocation timing or visual correctness.

## Loader research checkpoint

A bounded macOS observational run found that the loader semaphore call returning
to `0x08865840` exposes an entry ID and a dynamic destination in the request.
For ordinary text entry 2835, that destination matched the address eventually
found by the current RAM scanner. Entries 2838 and 4289 produced multiple chunk
notifications with the same destination and increasing offsets. A notification
therefore does not by itself prove that a whole text entry has finished loading.

This is evidence for a possible event-based discovery path, not a validated
quest fix. Quest list loading, relocation/copies and inline field construction
still need the capture matrix above. The generated function at `0x08863664`
checks a destination range; it must not be treated as a decrypt/copy hook merely
because it was mentioned beside the loader in an older description.

## Mod and downloadable quest research

Source inspection identified independent gaps beyond the scanner's latency:

- `mods::entry_at_offset` resolves reads against the original archive directory,
  while `read_data_bin` serves the active virtual layout. Activating a replacement
  does not update the original directory. A replacement that moves entry boundaries
  can therefore make translation identify the wrong entry, relative offset or size.
  This mismatch is confirmed in source; its visible effect still needs an end-to-end
  mod fixture and game capture.
- `set_language` selects the first matching language file and returns. Its search
  directories do not include enabled mods, and it does not merge language layers.
  Mod activation compares file replacements and patches, so adding a language-only
  source also requires explicit activation/invalidation semantics.
- The quest extractor requires at least two records and an ASCII first byte for
  candidate strings. Runtime discovery has similar assumptions. Single-record and
  non-ASCII source fixtures must be covered before claiming custom quest support.
- The savedata load path copies decrypted downloadable quest data into guest RAM
  without notifying translation. Archive read notifications alone cannot cover
  this source.

A read-only inspection of a local game archive found that the existing extractor
recognizes quest fields in the nonempty entries 4059–4073, but recognizes none in
4703–4716. The latter have a different outer structure. This does not establish
what their nested records contain or how they relate to downloaded savedata.
No archive contents or extracted strings are included in this document.

Before implementing mod language layers, verify that translations belong to the
winning source file. Base-game offsets must not silently apply to a mod's changed
record layout. Reuse mod enablement and priority, preserve untranslated fields,
and define cache invalidation when the active source changes. A quest ID plus a
field name is a candidate identity, not a validated format: ID reuse by mods and
DLC still needs investigation.

Remaining evidence: a synthetic moved-layout regression, single-record/UTF-8
fixtures, decoded DLC record identity and copy lifetime, and the visual matrix
for normal, modded and downloaded quests. These findings do not mark any of those
scenarios as passed and do not resolve the failing latency regression.

### Initial implementation

Archive lookup now uses the active virtual directory and rejects offsets beyond
its last entry. A synthetic two-entry ISO exercises a growing replacement,
re-keyed reads, exact-size padding, pending restart, activation and restoration.
The old lookup failed four assertions; the corrected lookup passed the complete
macOS renderer/UI suite. An unrelated pointer-click assertion failed in one
intermediate run and passed on the subsequent complete run; it remains a test
stability observation, not evidence about translation correctness.

Runtime discovery now accepts a zero terminator after a single quest record,
including a relocated record pointer, while rejecting invalid nonzero next
pointers. Extraction accepts a single record only with at least six recognized
fields; the multi-record binary-noise threshold is unchanged. Both new single-
record tests failed before their respective fixes. Non-ASCII source validation,
mod language layering, DLC parsing and event-based discovery remain outstanding.
The display-latency test remains an unsuppressed failure.

### Ordinary quest pointer-table implementation

The latency regression now models twelve loaded catalogs and one active quest
at a time, visiting all twelve records. The earlier fixture's twelve independent
inline copies did not represent the active container observed in the game.
The revised regression checks complete title, objective and description strings,
terminators and preserved sentinels within the original 30-frame ceiling and
256 KiB search budget. Disabling the new path with
`MHP3RD_TEXT_NO_QUEST_DIRECT=1` reproduces its failure.

A bounded macOS run of village quest 101 reached the active quest details screen
and displayed its translated title. A separate disposable run changed only the
title offset to another field and showed that other field in the title row,
confirming that this screen follows the active container's offset table. The
final implementation validates the ID, original strings and sentinel, preserves
inline bytes and stores translated strings in a separate arena slice.
The local run reused compatible AOT objects and overlays; it does not validate
Cyrillic width dispatch changes in regenerated code. Screenshots and memory dumps
remain local. The first-frame guarantee is synthetic-test evidence; the manual
capture was taken after loading and does not measure that deadline.

Automated checks cover reloads, a translation longer than its source slot,
malformed headers, incorrect IDs, source mismatch, corrupted field pointers,
invalid loader callers, incomplete reads and invalid destinations. Other-device,
Hall and DLC visual checks remain outstanding. Mod language-layer loading is
outside this change; source access stays behind the active archive interface.
