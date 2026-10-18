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
the current main executable. Flatpak and DMG automation, scheduled execution,
automatic private-cache refresh and automatic stable releases are outside this
workflow. Existing manual stable-release packaging remains available.

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

Private build, signing and publication jobs accept `workflow_dispatch` only in
`TeamGDB/Yakumo` on `refs/heads/main`. Source is pinned to the dispatch commit,
not a movable user-supplied ref. Do not add `pull_request_target`, an untrusted
checkout or feature-branch access to any credentialed job. Keep scheduling off.

Use three GitHub environments restricted to the `main` branch and reviewed by a
maintainer:

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

## Running the manual rollout

The dispatch becomes available after the workflow is reviewed and merged into
`main`. It deliberately cannot retrieve private inputs from a feature branch.
First run all platforms without release publication:

```sh
gh workflow run nightly.yml --repo TeamGDB/Yakumo --ref main -f publish=false
```

Approve `nightly-build-inputs` only after reviewing the exact dispatch commit.
Builds use bounded jobs, low compiler parallelism and verified overlay reuse.
Linux needs at least 25 GiB free after pulling its SDK. A full main executable
still requires generation and compilation; the first original-game builds can
take hours despite the small public probe completing quickly. Game-derived
objects and inputs never enter a shared Actions cache.

Approve `nightly-android-signing` after reviewing the successful Android build
and its inspected intermediate. `publish=false` still produces the final signed
APK for audit and device tests; it does not create a GitHub release.

Download each successful final artifact, named
`nightly-final-<run-id>-<platform>` with the platform keys from the manifest
below. Audit public logs, verify `SHA256SUMS`, inspect the exact packages and
test them on the corresponding devices before the first publication. Final
artifacts expire after one day. Raw private build logs are intentionally
unavailable from Actions, including on failure.

For publication, the fine-grained `NIGHTLY_PUBLISH_TOKEN` must select resource
owner `TeamGDB` and repository `Yakumo-nightly` only, with Contents read/write.
After the build-only audit, dispatch with `-f publish=true` and approve the
separate publication environment. The publisher creates one new pre-release
containing the validated final platform packages and required source/checksums;
it neither replaces stable downloads nor overwrites an earlier nightly.

Configuration of R2, environments and signing credentials does not establish
that a hosted original-game build has succeeded. Record the first complete
build-only run, package inspection and device results before enabling public
nightly distribution. Full gameplay and stable-release sign-off remain separate
requirements under [RELEASING.md](RELEASING.md).

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

The publisher rechecks transferred filenames and SHA-256 values. New immutable
pre-releases in `Yakumo-nightly` are never marked stable or Latest. Release notes
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

Before the first private run, verify bucket privacy and scoped read/write
denial, review the exact dispatch commit, then run with publication disabled.
Audit its public outputs and test final packages on real devices. Retain the
source commit and platform-specific gaps in the verification record; synthetic
probes and package inspection cannot replace gameplay testing.

References: [R2 access policies](https://developers.cloudflare.com/r2/api/tokens/),
[GitHub cache access](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching),
and [secure workflow use](https://docs.github.com/en/actions/reference/security/secure-use).
