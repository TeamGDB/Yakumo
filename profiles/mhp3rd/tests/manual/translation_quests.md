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
30 game frames over invented quest data. It tests source and inline fields under
multiple pending loads, with a bounded search-work requirement. It cannot prove
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
