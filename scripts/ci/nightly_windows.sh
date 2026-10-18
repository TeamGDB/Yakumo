#!/usr/bin/env bash
# Build one Windows portable ZIP using verified prebuilt overlay DLLs. Launch
# through nightly_windows.ps1 and nightly.py build-platform; no output is public.
set -euo pipefail
export LC_ALL=C

repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
profile_dir="$repo_dir/profiles/mhp3rd"
work="${YAKUMO_WORK:?set the private work directory}"
inputs="${YAKUMO_PRIVATE_INPUTS:?set the verified private input directory}"
version="${NIGHTLY_VERSION:?set the nightly version}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ;; *) echo 'error: Windows Git Bash is required' >&2; exit 1 ;; esac
[[ "$version" =~ ^nightly-[0-9a-f]{12}$ ]] || { echo 'error: invalid nightly version' >&2; exit 1; }
[[ "${VCToolsVersion:-}" == 14.44.* ]] || { echo 'error: MSVC 14.44 is required' >&2; exit 1; }
for tool in cl dumpbin cmake ninja python3 curl tar sha256sum cygpath; do
    command -v "$tool" > /dev/null || { echo 'error: missing build prerequisite' >&2; exit 1; }
done

work="$(cygpath -u "$work")"
inputs="$(cygpath -u "$inputs")"
python3 "$repo_dir/scripts/ci/nightly.py" verify-overlays --platform windows-x86_64 --inputs "$inputs"
for file in EBOOT.ELF disc.iso; do
    [[ -f "$inputs/$file" && ! -L "$inputs/$file" ]] || { echo 'error: missing private input' >&2; exit 1; }
done
# The trusted workflow installs these files in the fresh checkout. Never run
# prepare_game.sh or copy any player data or settings into a package.
for file in EBOOT.ELF disc.iso; do
    [[ -f "$profile_dir/game/$file" ]] || { echo 'error: game inputs are not prepared' >&2; exit 1; }
done

# shellcheck source=nightly_windows_sources.sh
source "$repo_dir/scripts/ci/nightly_windows_sources.sh"
sources="$work/sources"
deps_build="$work/deps-build"
deps="$work/deps"
build="$work/build"
mkdir -p "$sources" "$deps_build" "$deps"
# Preserve MSVC /pathmap values inside these CMake arguments. Keep normal path
# conversion for native curl, Python and Git, which also receive POSIX paths.
export MSYS2_ARG_CONV_EXCL='-DCMAKE_C_FLAGS_INIT=;-DCMAKE_CXX_FLAGS_INIT='
win() { cygpath -m "$1"; }
windows_tar="$(cygpath -u "${SYSTEMROOT:?Windows system directory is required}")/System32/tar.exe"
[[ -x "$windows_tar" ]] || { echo 'error: Windows archive tool is unavailable' >&2; exit 1; }
fetch() {
    local destination="$sources/$1"
    if [[ -f "$destination" ]] && echo "$3  $destination" | sha256sum -c --status; then return; fi
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 --max-time 600 \
        --proto '=https' --tlsv1.2 -o "$destination.part" "$2"
    echo "$3  $destination.part" | sha256sum -c --status || { rm -f "$destination.part"; return 1; }
    mv "$destination.part" "$destination"
}

fetch "SDL3-devel-$SDL3_VERSION-VC.zip" "$SDL3_VC_URL" "$SDL3_VC_SHA256"
fetch vulkan-headers.tar.gz "$VULKAN_HEADERS_URL" "$VULKAN_HEADERS_SHA256"
fetch vulkan-loader.tar.gz "$VULKAN_LOADER_URL" "$VULKAN_LOADER_SHA256"
fetch glslang.tar.gz "$GLSLANG_URL" "$GLSLANG_SHA256"
fetch "$NIGHTLY_WINDOWS_FFMPEG_SOURCE_NAME" "$NIGHTLY_WINDOWS_FFMPEG_SOURCE_URL" "$NIGHTLY_WINDOWS_FFMPEG_SOURCE_SHA256"
# Git for Windows ships GNU tar, which cannot extract ZIP. The Windows system
# bsdtar handles the checksum-verified VC archive, as in the release packager.
"$windows_tar" -xf "$(win "$sources/SDL3-devel-$SDL3_VERSION-VC.zip")" -C "$(win "$deps_build")"
tar -xzf "$sources/vulkan-headers.tar.gz" -C "$deps_build"
tar -xzf "$sources/vulkan-loader.tar.gz" -C "$deps_build"
tar -xzf "$sources/glslang.tar.gz" -C "$deps_build"
sdl="$deps_build/SDL3-$SDL3_VERSION"
headers="$deps_build/Vulkan-Headers-$VULKAN_SDK_TAG"
loader="$deps_build/Vulkan-Loader-$VULKAN_SDK_TAG"
glslang="$deps_build/glslang-$GLSLANG_COMMIT"

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(win "$deps")")
cmake -S "$(win "$headers")" -B "$(win "$deps_build/headers-build")" "${common[@]}" \
    -DVULKAN_HEADERS_ENABLE_TESTS=OFF
cmake --install "$(win "$deps_build/headers-build")"
cmake -S "$(win "$loader")" -B "$(win "$deps_build/loader-build")" "${common[@]}" \
    -DVULKAN_HEADERS_INSTALL_DIR="$(win "$deps")" -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF
cmake --build "$(win "$deps_build/loader-build")" --parallel 2
cmake --install "$(win "$deps_build/loader-build")"
cmake -S "$(win "$glslang")" -B "$(win "$deps_build/glslang-build")" "${common[@]}" \
    -DBUILD_EXTERNAL=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF \
    -DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_ENABLE_INSTALL=ON
cmake --build "$(win "$deps_build/glslang-build")" --parallel 2
cmake --install "$(win "$deps_build/glslang-build")"
export PATH="$deps/bin:$PATH"
export VULKAN_SDK="$(win "$deps")"

prefix_flags="/experimental:deterministic /pathmap:\"$(win "$repo_dir")=/src/yakumo\" /pathmap:\"$(win "$work")=/build\""
cmake -S "$(win "$repo_dir")" -B "$(win "$build")" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp3rd -DMHP3RD_RELEASE=ON \
    -DMHP3RD_RENDERER=ON -DMHP3RD_FFMPEG=bundled -DPSPRECOMP_CCACHE=OFF \
    -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF \
    -DPSPRECOMP_GENERATED_JOBS=2 -DPSPRECOMP_MSVC_MP_JOBS=1 \
    -DCMAKE_C_FLAGS_INIT="$prefix_flags" -DCMAKE_CXX_FLAGS_INIT="$prefix_flags" \
    -DSDL3_DIR="$(win "$sdl/cmake")" -DCMAKE_PREFIX_PATH="$(win "$deps")" \
    -DMHP3RD_FFMPEG_DOWNLOAD_DIR="$(win "$sources")" | tee "$work/configure.log"
for feature in 'mhp3rd: Vulkan renderer enabled' 'mhp3rd: bundled FFmpeg'; do
    grep -F -q "$feature" "$work/configure.log" || { echo 'error: required renderer/audio feature unavailable' >&2; exit 1; }
done

# Generate only the main executable's AOT units. No overlay corpus generation,
# default all-target build, or expensive build_overlays.sh invocation is allowed.
cmake --build "$(win "$build")" --target psp_analyze psp_recomp --parallel 2
mkdir -p "$profile_dir/analysis"
"$build/psp_analyze.exe" "$(win "$inputs/EBOOT.ELF")" "$(win "$profile_dir/analysis/report.json")"
"$build/psp_recomp.exe" "$(win "$inputs/EBOOT.ELF")" --auto "$(win "$profile_dir/generated")" --dispatch-target 0x088E8B18
cmake --build "$(win "$build")" --target Yakumo --parallel 2
mkdir -p "$build/bin/overlays"
cp "$inputs/overlays/"*.dll "$build/bin/overlays/"

# Existing packaging checks the final DLL imports and checks the unpacked ZIP.
export YAKUMO_WORK="$work" YAKUMO_PRIVATE_INPUTS="$inputs"
bash "$profile_dir/scripts/release_windows.sh" --version "$version" --no-installed "$build"
