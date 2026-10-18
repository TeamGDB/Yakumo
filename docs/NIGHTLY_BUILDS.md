# Private inputs and nightly distribution

Nightly workflows belong in `TeamGDB/Yakumo`. Public packages belong in
[`TeamGDB/Yakumo-nightly`](https://github.com/TeamGDB/Yakumo-nightly/releases).
Stable releases remain in the main repository and require maintainer approval.

The workflow builds all four supported platforms from a reviewed `main` commit,
with one distribution format per platform:

| Platform | Nightly package | Build contract |
| --- | --- | --- |
| Linux x86-64, including Steam Deck | `yakumo-nightly-<sha12>-linux-x86_64.tar.gz` | Pinned Steam Runtime sniper SDK and GCC 14 |
| Windows x86-64 | `yakumo-nightly-<sha12>-windows-x86_64-portable.zip` | MSVC 14.44 and pinned SDL3/Vulkan dependencies |
| macOS Apple Silicon | `yakumo-nightly-<sha12>-macos-arm64.zip` | ARM64, macOS 13 minimum, pinned SDL3/Vulkan/MoltenVK |
| Android ARM64 | `yakumo-nightly-<sha12>-android-arm64.apk` | NDK 28.2, API 29, signed with the production release key |

Each build reuses 355 verified native overlay libraries and generates/compiles
the current main executable. A daily run at **00:00 UTC** builds and publishes
only when the pinned `main` commit differs from the latest complete nightly.
Manual runs can force a rebuild. Flatpak and DMG automation, automatic
private-cache refresh and automatic stable Yakumo releases are outside this
workflow. Existing manual stable-release packaging remains available.

Nightlies are published immediately after package and Android signature checks
as normal GitHub Releases marked **Latest** in `Yakumo-nightly`. They remain
experimental development snapshots; the GitHub release status does not mean
they passed stable-release gameplay testing. The separate channel provides a
convenient [latest nightly link](https://github.com/TeamGDB/Yakumo-nightly/releases/latest).

The Android nightly uses the normal Yakumo application ID and production signing
identity. It updates the normal application rather than installing a separate
test application. Back up saves before installing experimental packages.

## Data boundaries

- `yakumo-build-inputs` is a private R2 bucket for approved game build inputs.
  `yakumo-build-cache` holds checksum-pinned native overlay seeds for all four
  platforms. Keep managed `r2.dev` access disabled, attach no public domains or
  Workers, and issue a read-only token restricted to these two buckets.
- Never upload disc images, EBOOT/DATA.BIN content, generated source, overlay
  corpora, saves, raw private logs, dumps or captures to Git, GitHub Actions
  artifacts, dependency caches, releases or another public service. A public
  base-branch Actions cache can be restored by fork pull requests; it is not
  private storage.
- Credentialed builds use dedicated `RUNNER_TEMP` directories. Original inputs
  are verified by size and SHA-256, restored with restrictive permissions, and
  never placed in publication directories. Player saves are not build inputs.
- Generator, build and packaging output for original game inputs is captured
  by the internal `private_run()` helper behind `nightly.py build-platform`. Public logs receive short status messages,
  including on failure. Raw diagnostics remain ephemeral and are never uploaded.
  Reproduce failures in a private maintainer environment.
- Publication starts from new directories populated only after inspecting the
  actual packages. Never upload `out/**`, a whole workspace, a build directory,
  a producer-supplied file list or an unchecked private artifact on an `always()`
  path.

## Trusted execution and credentials

Private build, signing and publication jobs accept `schedule` and
`workflow_dispatch` only in `TeamGDB/Yakumo` on `refs/heads/main`. Source is pinned
to the event's immutable commit,
not a movable user-supplied ref. Do not add `pull_request_target`, an untrusted
checkout or feature-branch access to any credentialed job. Main changes follow
the maintainer-reviewed pull-request process in [CONTRIBUTING.md](../CONTRIBUTING.md).

Use three GitHub environments restricted to the `main` branch. For unattended
nightlies, omit required reviewers and wait timers on these environments;
retain the custom branch policy allowing only the `main` branch. This changes
per-run approval, not the main-only source boundary or separation of secrets:

| Environment | Secrets | Purpose |
| --- | --- | --- |
| `nightly-build-inputs` | `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY` | Read only the two private buckets; expose credentials only to input retrieval |
| `nightly-android-signing` | `ANDROID_RELEASE_KEYSTORE_B64`, `ANDROID_RELEASE_KEYSTORE_PASSWORD`, `ANDROID_RELEASE_KEY_ALIAS` | Sign the inspected Android APK with the existing production identity; the key and store use the same password |
| `nightly-publication` | `NIGHTLY_PUBLISH_TOKEN` | Fine-grained token with Contents read/write only in `TeamGDB/Yakumo-nightly` |

Builders receive no Android signing key or publication token. The signing job
receives only the inspected unsigned APK and its public metadata: it has no R2
credentials, game inputs, build checkout or project-code execution. The signing
key is materialized only inside that job's ephemeral workspace. The publisher
receives no R2 credentials or signing key and does not check out or execute
project code. The unsigned Android intermediate is transferred only to the
signing job; it is never a release asset.

Never reuse a maintainer's broad interactive Cloudflare login or GitHub token as
a CI secret. Store secrets separately, not in an opaque JSON blob, and rotate
expiring credentials through the appropriate protected environment. Never paste
credentials into issues, pull requests, command-line arguments or public logs.

Use `cf` for bucket and token administration. The downloader uses the official
S3 SDK with a bucket-scoped read-only key; the administrative object endpoint
would require broader account permissions. SDK dependencies are pinned with
hashes in `requirements/nightly.txt`.

## Public packaging probes

Pull requests run full synthetic build/packaging probes for all four platforms.
They use no secrets, R2 access or original game files. The public
`scripts/ci/nightly_probe.py` creates a tiny original MIPS ELF containing only
standard stub instructions, a disc sentinel that is not an image, and 355 copies
of a tiny natively compiled module. The regular analyzer, generator, dependency
bootstrap, host build and platform packager then exercise the real build path.

The Android probe accepts `ANDROID_HOME` only at `/usr/local/lib/android/sdk`,
`~/Library/Android/sdk` or `~/Android/Sdk`. SDK aliases, traversal spellings and
symlinked SDK/NDK/toolchain entries are refused; install the pinned NDK there.

These applications are never executed, published or offered to players. Only
synthetic failure logs may be uploaded for diagnosis. This exception does not
allow uploading logs from credentialed builds using original game inputs.

The Android probe also exercises the actual APK packager, protected signing
command and unchanged-payload validator with public native fixtures and a
disposable key. The packager omits directory ZIP records, which the official
signer would otherwise remove; all regular files and unsigned ZIP regions must
remain byte-identical through signing. This regression must run without skips.

A successful public probe verifies the synthetic packaging path. It does not
measure full AOT memory/time requirements, verify a complete game build, test
real overlay execution or establish device/gameplay compatibility. Inspect the
actual probe results before merging; adding the jobs alone is not evidence that
they passed. Keep those limits in the PR and release-testing record.

## Scheduled and manual runs

The workflow runs daily with `0 0 * * *` (00:00 UTC) after merge into `main`.
GitHub may queue a scheduled run after that time; see its
[schedule documentation](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule).

A secret-free planning job checks public metadata of the channel's latest
published release. It verifies its source, four-platform completeness and
GitHub asset digests before deciding whether that exact commit was already
published. If unchanged, it skips builds before any private input retrieval.
Missing releases permit the first build; API errors or invalid/incomplete
metadata fail planning and leave credentialed jobs blocked. Manual runs can
repair or rebuild a release without this deduplication check.

Scheduled and manual runs share one concurrency group, so they cannot publish
in parallel. In-progress trusted builds are not cancelled by a later trigger.
Pull-request probes have separate concurrency and receive no private secrets.

Manual dispatch defaults to publication. For an audit without publication:

```sh
gh workflow run nightly.yml --repo TeamGDB/Yakumo --ref main -f publish=false
```

Builds use bounded jobs, low compiler parallelism and verified overlay reuse.
Linux needs at least 25 GiB free after pulling its SDK. A full main executable
still requires generation and compilation; the first original-game builds can
take hours despite the small public probe completing quickly. Game-derived
objects and inputs never enter a shared Actions cache.

Isolated Android signing starts after all four builds and content inspections
succeed. `publish=false` still produces the final signed APK for audit and device
tests; it does not create a GitHub release.

Download each successful final artifact, named
`nightly-final-<run-id>-<platform>` with the platform keys from the manifest
below. Audit public logs, verify `SHA256SUMS` and inspect the exact packages.
Use the corresponding devices for gameplay testing and record that evidence
separately; nightly publication does not claim it was performed. Final
artifacts expire after one day. Raw private build logs are intentionally
unavailable from Actions, including on failure.

For publication, the fine-grained `NIGHTLY_PUBLISH_TOKEN` must select resource
owner `TeamGDB` and repository `Yakumo-nightly` only, with Contents read/write.
Dispatch with `-f publish=true`, or let the daily schedule publish automatically.
The publisher creates one new normal GitHub Release marked Latest
containing the validated final platform packages and required source/checksums;
it does not overwrite an earlier nightly or publish into the stable repository.

The first complete original-input build and publication succeeded in
[run 39109539747](https://github.com/TeamGDB/Yakumo/actions/runs/39109539747),
from source `2f829071d8c3b7081cfcebd08b7ef0326c530e03`. All four package
inspections and the production Android signature/payload checks passed; all
eight published asset digests were verified. This is packaging evidence, not
a claim of full device/gameplay testing. Stable-release sign-off remains a
separate requirement under [RELEASING.md](RELEASING.md).

## Private input manifest

The operator seeds `mhp3rd-hd/v1/manifest.json` inside `yakumo-build-inputs`.
Actual checksums, credentials and account-specific values stay private. Schema 2
selects a separate compatible native cache for each platform:

```json
{
  "schema": 2,
  "files": [
    {"name": "EBOOT.ELF", "size": 1, "sha256": "<sha256>",
     "parts": [{"key": "mhp3rd-hd/v1/executable/0000", "size": 1, "sha256": "<sha256>"}]},
    {"name": "disc.iso", "size": 1, "sha256": "<sha256>",
     "parts": [{"key": "mhp3rd-hd/v1/disc/0000", "size": 1, "sha256": "<sha256>"}]}
  ],
  "overlay_caches": {
    "linux-x86_64": {
      "filename": "yakumo-0.7.5-linux-x86_64.tar.gz",
      "key": "overlays/linux-x86_64/v1/<sha256>.tar.gz",
      "source_commit": "<full-frozen-release-source-commit>",
      "abi_sha256": "<compatibility-fingerprint>",
      "toolchain": "steamrt-sniper-gcc14", "size": 1, "sha256": "<sha256>"
    },
    "macos-arm64": {
      "filename": "yakumo-0.7.5-macos-arm64.zip",
      "key": "overlays/macos-arm64/v1/<sha256>.zip",
      "source_commit": "<full-frozen-release-source-commit>",
      "abi_sha256": "<compatibility-fingerprint>",
      "toolchain": "libcxx1-macos13", "size": 1, "sha256": "<sha256>"
    },
    "windows-x86_64": {
      "filename": "yakumo-0.7.5-windows-x86_64-portable.zip",
      "key": "overlays/windows-x86_64/v1/<sha256>.zip",
      "source_commit": "<full-frozen-release-source-commit>",
      "abi_sha256": "<compatibility-fingerprint>",
      "toolchain": "msvc14.44", "size": 1, "sha256": "<sha256>"
    },
    "android-arm64": {
      "filename": "yakumo-0.7.5-android-arm64.apk",
      "key": "overlays/android-arm64/v1/<sha256>.apk",
      "source_commit": "<full-frozen-release-source-commit>",
      "abi_sha256": "<compatibility-fingerprint>",
      "toolchain": "ndk28.2-api29", "size": 1, "sha256": "<sha256>"
    }
  }
}
```

Numbers and checksums above are placeholders, not accepted game fixtures. Split
large originals into immutable parts no larger than 128 MiB; the current
Cloudflare object upload endpoint is limited to 300 MB per request. Verify each
uploaded part and the reconstructed file. The supported executable must match
the existing profile's SHA-256. Upload the manifest last, after every object is
verified, and do not overwrite objects referenced by an active manifest.

The initial cache seeds are the approved v0.7.5 distributions built from the
frozen release source. Extraction accepts only the platform's native overlay
modules, never game data or generated source. The seed's source and current
source must have the same compatibility fingerprint over runtime headers,
profile/overlay ABI declarations, the overlay entry point, generator/parser
sources, overlay/wrapper and DATA.BIN tools, CMake configuration and SDK pins.
Source fingerprints normalize CRLF to LF so Windows checkouts match immutable
Git snapshots; other content changes and added inputs still invalidate reuse.
Each restored file has a full SHA-256 in `overlay-files.json`, alongside the
platform, toolchain and ABI identity. Local uncommitted changes and new ABI
headers also invalidate reuse.

A checksum, ABI or toolchain mismatch fails closed instead of silently
rebuilding or loading incompatible overlays. Refresh caches deliberately when
required; never rebuild 355 overlays merely because a new checkout or nightly
is being prepared.

## Publication checks

`scripts/ci/nightly.py stage-public` inspects the actual package against its
platform policy before constructing any distributable directory:

- Only expected native executables/libraries, exactly 355 overlays, approved
  documentation, pinned fonts and known licenses are accepted. Android also
  validates its bounded compiled manifest/resources, classes and native layout.
- No links, devices, traversal, duplicate names, oversized expansion, hidden
  trailers or unexpected payloads. Desktop ZIPs are rewritten canonically,
  dropping producer metadata and macOS AppleDouble/xattr streams.
- Native architectures and executable/library roles are checked. Original
  PSP/MIPS executables, renamed ISO/PBP/PSP/SFO data and arbitrary files are
  refused; a filename alone never establishes that a payload is publishable.
- Launcher, documentation, font and license bytes must match approved public
  sources. The required FFmpeg source archive must match the reviewed platform
  pin: CMake's release archive for source-built FFmpeg, or the exact upstream
  commit corresponding to Windows' prebuilt FFmpeg, pinned in
  `scripts/ci/nightly_windows_sources.sh`.
- macOS checks ad hoc code-directory page hashes, nested library identities and
  resource seals, including the exact app templates. Unused signature allocation
  is zeroed without changing signed bytes, code-directory identities or seals.
- Android signing runs separately after unsigned-content inspection. The final
  package is signed with the existing production key and verified before final
  transfer; unsigned intermediates are never published.
- Public directories contain only validated packages, required FFmpeg source,
  `SHA256SUMS`, public build metadata and experimental-release notes. Invalid
  input or a failed copy leaves no partial publication directory.

The publisher rechecks transferred filenames and SHA-256 values. Every nightly
gets a unique release tag in `Yakumo-nightly`; the latest successful publication
is marked Latest there. Release notes retain the experimental warning and
identify the main repository's actual source commit and testing limitations.

These checks protect a defined packaging boundary for reviewed source. They are
not a sandbox for malicious build scripts or arbitrary encoded data in native
code. Pinned dependencies, environment protections and credential separation
remain required.

## Verification

Run the full Python suite and coverage gate described in [TESTING.md](TESTING.md)
and [COVERAGE.md](COVERAGE.md). Public contracts cover renamed original-file
formats, checksums, traversal, links, duplicate members, hidden data, native
architectures, resource/signature seals, cache isolation and private-log canaries.
The public full-build probes complement those tests; no game data is a public
fixture.

When changing private storage or credentials, verify bucket privacy and scoped
read/write denial before dispatching. Audit public outputs and use final
packages for device tests. Retain the source commit and platform-specific gaps
in the verification record; synthetic probes and package inspection cannot
replace gameplay testing or stable-release sign-off.

References: [R2 access policies](https://developers.cloudflare.com/r2/api/tokens/),
[GitHub cache access](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching),
and [secure workflow use](https://docs.github.com/en/actions/reference/security/secure-use).
