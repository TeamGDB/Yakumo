# Scenario-based device testing

Use `profiles/mhp3rd/tools/device_testing.py` for the same scenario expectations
on physical devices, emulators and optional Android Device Streaming. This is a
local developer tool, not a public game-data CI job. Human play and agent visual
review complement automatic observations; neither screenshots nor a successful
process launch establish gameplay correctness.

## Trust boundary

This CLI runs with the tester's permissions. Its private configuration deliberately
selects executable/tool paths, arguments, source fixtures and remote hosts; review
that configuration and scenario files like executable test code. Do not run
downloaded configs or expose this CLI as a service for untrusted users. CodeQL with the local
threat model reports these intended path/command permissions as tainted flows;
maintainers must review those findings against this documented boundary. Source
suppression comments alone do not clear GitHub Code Scanning alerts, and this
tool does not dismiss them or change scanner settings. Archive evidence and
fixture links still receive explicit confinement checks. Tester-authored log
regexes execute in a separate process with the wait deadline, so backtracking
cannot leave the controller hanging.

## Start locally

Python 3.9 or newer is sufficient. No game build is needed for the tooling.
Reuse existing compatible binaries/APKs and overlays. Create the ignored
`device-testing.local.json` at the repository root, for example:

```json
{
  "devices": {
    "mac": {
      "adapter": "local",
      "build": "dev",
      "commit": "0000000000000000000000000000000000000000",
      "binary": "/path/to/Yakumo",
      "game_dir": "/path/to/prepared-game",
      "saves": "/path/to/fixture/ms0",
      "settings": "/path/to/fixture/settings.ini",
      "overlays": "/path/to/compatible/overlays",
      "hardware": {"gpu": "Record actual GPU", "driver": "Record actual driver"},
      "seconds": 180,
      "fixtures": {
        "load-character": [
          {"wait": "\\[overlay\\] installed demo_task", "timeout": 30},
          {"input": "0:pad start 4"},
          {"wait": "Replace this with an observed title/menu marker", "timeout": 20}
        ]
      }
    }
  }
}
```

The all-zero commit and fixture steps above are placeholders: replace them with
the exact source commit and measured navigation for your copied character and
settings. The runner records the executable's SHA-256 separately; declaring a
commit does not prove an old executable was rebuilt. Save/image/language hashes
make comparisons reproducible without publishing their contents.

```sh
make device-scenarios
make device-run DEVICE=mac DEVICE_SCENARIO=boot
make device-run DEVICE=mac DEVICE_SCENARIO=quest-village
```

The evidence directory is printed before the bounded session. Exit status `0`
means all required observations and reviews passed; `1` means a failure; `2`
means incomplete or unavailable checks, including visual review pending. A
successful automatic run normally exits `2` until its captures are reviewed.
Keep evidence under this checkout's `out/` or the OS temporary directory.
Original saves/settings are read-only inputs. Every run copies `ms0` and settings
and redirects **both** game and data directories to its own session. Save links
and special files are rejected. Game inputs use read-only source links (hard
links on Windows when symlinks are unavailable); no destructive setup actions
are part of the automatic scenarios.

Use a dedicated development installation. The runner owns and terminates only
the process it launches; local execution has a separate bounded supervisor so a
lost SSH connection does not leave an unbounded game. Check the current desktop
session before launch and do not interfere with another person's play.

## Human play and review

Humans and visual agents use the same expected conditions and evidence layout.
For a bounded human session, use an explicit manual scenario:

```sh
make device-run DEVICE=mac DEVICE_SCENARIO=quest-translation DEVICE_ARGS=--manual
```

A translation scenario additionally requires local `translation` and
`env: {"MHP3RD_LANGUAGE": "<language-code>", "MHP3RD_TRACE_TEXT": "1"}` fields.
The translation is copied into the session. Follow the existing
[quest translation capture matrix](../profiles/mhp3rd/tests/manual/translation_quests.md):
Japanese and patched images, catalog reopening, village/Hall details, active
quest and an original-language control. Missing translation keys are fixture
coverage gaps; present keys left untranslated are runtime failures.

While a local session runs, take a checkpoint with an existing input command:

```sh
printf '%s\n' '0:shot 03-village-details' >> "$run_dir/input-live.txt"
```

ADB manual sessions use the same live file inside the dedicated package; append
through `run-as`, or take a host-side `adb -s SERIAL exec-out screencap -p` capture.
Use relative evidence paths within the printed directory. Review the actual
images and record expectations, observed behavior, timing, quest IDs and gaps:

```sh
python3 profiles/mhp3rd/tools/device_testing.py review \
  --run "$run_dir" --check opening --verdict PASS --reviewer human \
  --notes 'Opening picture inspected; no blank frame or corruption.' \
  --evidence shots/opening.bmp
```

`--reviewer agent` records a visual agent's assessment explicitly. Human and agent
assessments are retained independently; a failing assessment keeps the check
failed. The final release checklist requires human PASS verdicts, so an agent
review cannot certify audio, physical controls or a completed hunt. Automatic
results remain separate; a reviewer cannot overwrite a timeout/refusal/crash.
Regenerating the Markdown and release matrix preserves recorded reviews.
Audio, controller feel, fighting, long play and multiplayer still need human
observation. Record `NOT TESTED` with a reason when those checks were unavailable.

`prepare` / `make device-prepare` creates a fingerprinted checklist without
launching anything. This is useful for final APKs that cannot use `run-as`,
Game Mode installation/update checks and coordinated multiplayer. Test the exact
artifact normally, then record each manual check with evidence/notes. Completing
all required manual checks can produce PASS without pretending that automatic
checks ran. The prepared report initially remains NOT TESTED.

## Device adapters

| Adapter | Use | Setup |
| --- | --- | --- |
| `local` | macOS/Linux/Windows physical host | Run in its logged-on desktop; supply local fixture paths |
| `ssh` | Steam Deck/Linux, Windows or Android attached to Windows | Same runner installed on that host; SSH delegates local/ADB execution and returns evidence only |
| `adb` | Physical Android and emulator | Explicit serial, dedicated debuggable test APK with generated contract |
| `streaming` | Optional Google remote Android | Same APK/ADB contract, authenticated official CLI, idle project, explicit total cap of 300 seconds or less |

An SSH device wraps a `remote` device object with the same `build` and `commit`.
Add `host` (an SSH alias), `checkout` (where this tooling revision is installed),
`python: ["python3"]`, and `shell: "posix"` or `"powershell"`. Remote fields name
paths **on that host**, including an `adb: ["C:/path/to/adb.exe"]` prefix for a
phone attached to Windows. PowerShell invocation uses encoded arguments instead
of interpolating paths into shell expressions. Remote results bring back logs,
reports and captures, never game inputs or copied saves.

SSH does not create a visible Windows desktop session. Run windowed native checks
from a logged-on terminal or the developer's bounded interactive task workflow;
a service-session SSH launch cannot certify visuals. Record that limitation.
Likewise, SSH desktop checks do not certify Steam Deck Game Mode: prepare and
complete its normal-player checklist manually. Configure Windows/Deck separately
rather than silently reusing a result from another OS or display session.

### Dedicated Android test APK

SDL reads environment variables from application manifest metadata; arbitrary
`am start` extras do not configure Yakumo. The existing APK packer can now make
an isolated **developer-only** test package from already-built native libraries:

```sh
DEBUGGABLE=1 DEVICE_TEST_PACKAGE=org.example.yakumo.devicetest \
  profiles/mhp3rd/packaging/android/build_apk.sh \
  /path/to/dev-build /path/to/SDL-source /path/to/libSDL3.so /path/to/device-test.apk
```

This requires `MHP3RD_RELEASE=OFF` in the native build. It generates SDL metadata
and `assets/device-testing.json`, preserving the real Java activity name.
The runner checks this contract and rejects normal player packages. It installs
with `-r` only into the dedicated `.devicetest` identity, stages copied fixtures
in that package's private harness directory and refuses a currently running
package. It never clears or replaces the original player app.

The device object uses `apk`, `package`, explicit `serial`, `game_dir`, `saves`
and optional `settings`. The packer contract has fixed diagnostic environment:
translation/mod variants are not yet supported by this adapter and stay
NOT TESTED. Use local translation scenarios or normal manual APK checks instead.
No APK modification is used to certify a final release artifact.

ADB startup/cleanup has a PID-guarded device watchdog and explicit host cleanup.
Logs come from the app's actual `files/yakumo.log`; logcat and Vulkan diagnostics
are retained separately. Different physical GPUs require separate rows: emulator
or old PowerVR results do not establish support for every Dimensity/PowerVR GPU.

### Optional Device Streaming

Use `adapter: "streaming"`, `project`, `model: "<codename>/<api>"` and an explicit
`seconds` between 1 and 300, alongside the APK/fixture fields. Authentication and
API setup happen separately. The tool does not provision resources, attach
billing, change IAM, or run in public CI. Every Android CLI invocation uses
`--no-metrics`. It requires an idle dedicated project and removes only the
reservation ID reported by its own creation command.

The total deadline includes reservation activation, APK installation and game
uploads, rather than gameplay alone. Host cleanup removes a known reservation
on success/failure/interrupt; a separate deadline watchdog provides a fallback.
An unrecognized creation response is a failure requiring inspection of the
creation log; it must not trigger deletion of someone else's reservation.
Collect evidence before release because the remote device is wiped.

CLI behavior and access can change. Verify the installed version/help and
[official remote commands](https://developer.android.com/tools/agents/android-cli/commands/device_remote).
Check [current quotas/pricing](https://firebase.google.com/docs/test-lab/usage-quotas-pricing)
before use. A configured quota is not a spending cap. Use private project notes
in `LOCAL_TESTING.md`; no project ID or credentials belong in public examples.

## Add a regression

Add a small JSON file under `profiles/mhp3rd/tests/scenarios/`. Its contract is:
`id`, `description`, `build` (`dev`, `release`, `either`), `execution`
(`automatic`, `manual`), `seconds` (1..1800), `prerequisites`, `steps` and `review`.
Use stable IDs and explicit expected behavior. Keep game strings, captures and
saves local; include only public facts such as quest IDs.

Each step has one action:

- `wait`: a log regular expression with its own `timeout`. By default it observes
  text after the most recent command; `fresh: false` permits startup history.
- `input`: one finite `frame:action` command from
  [input_script.hpp](../profiles/mhp3rd/host/ui/input_script.hpp). It schedules
  bounded actions relative to reception, not an open-ended confirm loop.
- `debug`: one command from [DEBUG_MENU.md](DEBUG_MENU.md), dev scenarios only.
- `capture`: a safe checkpoint name; local window BMP or Android screen PNG.
- `fixture`: bounded device-specific navigation steps in local configuration.
  Fixture recursion and release debug commands are rejected.

Navigation is fixture-specific. Observe state/log markers and bound every wait;
do not assume a slow phone reached a menu after an arbitrary wall-clock sleep.
Where the game exposes no reliable marker, use a manual checkpoint instead of
inventing an automatic pass. The initial `quest-village` scenario acknowledges
quest 204, waits for the actual `game_task` handoff, reads back its accepted ID,
and polls the active quest clock; visual review separately confirms map,
monster and Current Quest Details. A refusal, wrong ID, timeout or exit fails.

Debug quest starts are compiled **out** of release builds and writes are refused
during ad hoc play. Never enable shortcuts in a multiplayer check. Unsupported
DLC/event quests remain manual NOT TESTED until an appropriate fixture exists.
Add public synthetic success/failure/cleanup tests in
`scripts/ci/test_device_testing.py` for changes to the runner. `make python-test`
and the existing Python coverage job discover these tests; game files and cloud
reservations are not involved.

## Before a release

Prepare `device-release.local.json` with an explicit row for every required
scenario/device pair and the exact artifact identities, for example:

```json
{
  "required": [
    {
      "device": "deck", "scenario": "release-player", "build": "release",
      "commit": "0000000000000000000000000000000000000000",
      "artifact_sha256": "0000000000000000000000000000000000000000000000000000000000000000"
    }
  ]
}
```

Use real hashes. Give Japanese/patched images or language/settings variants
separate configuration/device IDs when they need distinct required rows; each
run records its fixture and game fingerprints. Add macOS, Windows, Deck, available physical Androids and
selected cloud GPU families. Include dev regression rows when relevant, then
**separate final release-package rows**. Dev results cannot satisfy release rows.
The current scenario fingerprint must match too, so an older scenario definition
cannot silently certify the current checklist.

```sh
make device-matrix
```

The matrix links each row to its reviewed evidence and exits `1` for failures or
`2` for missing/incomplete checks. It does not silently omit required devices,
DLC, audio or multiplayer. A project can publish with a consciously accepted
coverage gap, but the report keeps that gap visible; do not turn it green to
unblock a release. Record exceptions and relevant hardware evidence in release
notes/[COMPATIBILITY.md](COMPATIBILITY.md). Review/redact evidence before any
public upload: captures/logs/configuration can contain copyrighted or personal
information and are never uploaded automatically.
