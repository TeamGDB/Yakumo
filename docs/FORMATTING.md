# C++ formatting proposal

This stage proposes a style for review; it does not adopt a repository-wide
rewrite or a blocking formatting gate. See [#251](https://github.com/TeamGDB/Yakumo/issues/251)
and its parent [#249](https://github.com/TeamGDB/Yakumo/issues/249).

## Tool and scope

Use **clang-format 21.1.8**. The [LLVM release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8)
has binaries for Linux, Apple Silicon macOS and Windows. The
[LLVM formatter documentation](https://releases.llvm.org/21.1.0/tools/clang/docs/ClangFormat.html)
describes configuration files and editor integration. A formatter version does
not select the compiler used to build Yakumo: GCC, MSVC and Clang builds continue
to use their existing toolchains.

For a smaller cross-platform installation, the commands below use the
[clang-format Python package](https://pypi.org/project/clang-format/21.1.8/), which
packages LLVM's formatter separately from LLVM's official release downloads.
`scripts/requirements-format.txt` pins the version and SHA-256 of the published
binary wheels; `--only-binary` prevents a source-build fallback. The wrapper also
rejects any executable whose reported formatter version differs.

From the repository root on macOS/Linux:

```sh
python3 -m venv out/format-venv
out/format-venv/bin/python -m pip install --only-binary=:all: --require-hashes -r scripts/requirements-format.txt
. out/format-venv/bin/activate
python scripts/format_cpp.py report --samples --diff
```

On Windows, use the equivalent virtual environment executables:

```powershell
py -3 -m venv out/format-venv
out/format-venv/Scripts/python.exe -m pip install --only-binary=:all: --require-hashes -r scripts/requirements-format.txt
& out/format-venv/Scripts/Activate.ps1
python scripts/format_cpp.py report --samples --diff
```

The wrapper selects only Git-tracked regular C/C++ files in `include/psprecomp`,
`src`, `tests`, `tools`, `profiles/mhp3rd/host` and `profiles/mhp3rd/tests`.
It excludes vendored code, generated code, game data, overlays and build outputs,
including nested excluded directories and symlinks. Explicit file paths must be
in this scope; untracked files must be staged before they can be selected.
The `.clang-format-ignore` file also protects direct file invocations. Use the
wrapper for bulk operations: editor integrations and stdin invocations may not
respect ignore-file discovery in the same way.

## Style choices for review

The inspected samples cover a runtime header and implementation, a host input
module, profile and framework tests, and a command-line tool. Existing code
usually uses four spaces, attached braces, unindented namespaces and pointer
stars beside the variable, but mixes line widths and compact control flow.
The proposal retains those common choices:

| Choice | Proposal |
| --- | --- |
| Indentation | Four spaces, no tabs; four-space continuation and braced-initializer indentation |
| Wrapped expressions | Fixed continuation indentation rather than horizontal alignment with opening brackets or operands |
| Braces and namespaces | Attached braces; namespaces do not indent their contents |
| Pointer and reference placement | `Type *pointer`, `Type &reference` |
| Line width | 120 columns; clang-format may leave indivisible tokens longer |
| Compact statements | Short `if` without `else` and short loops may stay on one line |
| Functions | Short class-inline functions may stay on one line; ordinary function bodies expand |
| Includes | Preserve ordering and blocks, including platform-dependent ordering |
| Comments | Do not reflow text or add namespace comments |
| Code transformations | Do not insert braces or reorder qualifiers |

Review **120 columns**, compact control flow, inline function treatment and
the remaining LLVM bin-packing defaults before accepting the proposal. The
sample diffs also show stream-expression repacking and constructor wrapping.
No production source is reformatted in this PR.

At baseline `e0b0573`, **215 of 260** scoped files differ. This is a formatting
inventory, not a list of code defects. The CI job reports the count and uploads
`baseline.json` plus six complete `samples.diff` previews, so reviewers can
inspect actual repository code without a mechanical rewrite.

## Local checks and gradual enforcement

Put the exact formatter on PATH, for example by activating the environment above.
The wrapper invokes the fixed command `clang-format`; it does not accept an
arbitrary executable from command-line input.

```sh
# Preview all existing differences; does not write source or fail for differences.
python3 scripts/format_cpp.py report --json
# Preview the representative review set.
python3 scripts/format_cpp.py report --samples --diff > out/format-review/samples.diff
# Check just explicit files (exit 1 if formatting differs).
python3 scripts/format_cpp.py check profiles/mhp3rd/tests/guest_pcm_tests.cpp --diff
# Select changed files since the merge-base with main, including local tracked edits.
python3 scripts/format_cpp.py check --base origin/main --diff
# Format one staged/committed file in a dedicated mechanical commit.
python3 scripts/format_cpp.py format profiles/mhp3rd/tests/guest_pcm_tests.cpp
```

`--json` writes the fixed repository-local `out/format-review/baseline.json`
path; arbitrary report output paths are not accepted.

`check` returns 0 for conforming files, 1 for formatting differences and 2 for a
tool/version/input failure. `report` returns 0 for differences but still returns
2 on a tool failure. `format` writes only selected source files. Without paths,
`--base` or `--samples`, any mode selects the full scoped baseline: do not use a
bare `format` command for this review stage.

The separate GitHub Actions job is **advisory for formatting differences**. It
uses ordinary hosted runners, reads no game data and builds no game code or
overlays. It does not use `continue-on-error`: formatter/tool failures remain
visible. It is not intended as a required style check while policy is unsettled.

After style approval, the proposed next step is to require formatting for new
files and deliberately opted-in existing files. Reformat each adopted existing
file in a separate mechanical commit, then enable its blocking check. The
`--base` option is available for an eventual changed-file policy, but that policy
would check whole touched files, including old untouched lines; do not enable it
as a blocking gate until maintainers accept that cost. Configuration/version
changes need a fresh sample and baseline review. No baseline suppression list or
mass rewrite is introduced here.
