#!/usr/bin/env bash
# Build the macOS ARM64 nightly with private inputs and compatible native
# overlays. The workflow captures all output in a private, ephemeral log.
set -euo pipefail
umask 077
ulimit -c 0

repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_dir"
source profiles/mhp3rd/packaging/macos/sources.sh

fail() { echo "error: $*" >&2; exit 1; }
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || fail "an ARM64 Mac is required"
: "${YAKUMO_PRIVATE_INPUTS:?private verified inputs are required}"
: "${YAKUMO_WORK:?a private build workspace is required}"
: "${NIGHTLY_VERSION:?a nightly version is required}"
[[ "$NIGHTLY_VERSION" =~ ^nightly-[0-9a-f]{12}$ ]] || fail "invalid nightly version"
[[ "$YAKUMO_PRIVATE_INPUTS" == /* && "$YAKUMO_WORK" == /* ]] || fail "absolute private paths are required"
[[ ! -L "$YAKUMO_WORK" ]] || fail "the private workspace cannot be a symbolic link"
python3 scripts/ci/nightly.py verify-overlays --platform macos-arm64 --inputs "$YAKUMO_PRIVATE_INPUTS"
for file in EBOOT.ELF disc.iso; do
    [[ -f "$YAKUMO_PRIVATE_INPUTS/$file" && ! -L "$YAKUMO_PRIVATE_INPUTS/$file" ]] || fail "missing private build input"
    # The workflow may already have installed these links. Refuse an unrelated
    # file or link rather than overwrite a developer's prepared game directory.
    destination="$repo_dir/profiles/mhp3rd/game/$file"
    if [[ -e "$destination" || -L "$destination" ]]; then
        [[ -L "$destination" && "$(readlink "$destination")" == "$YAKUMO_PRIVATE_INPUTS/$file" ]] ||
            fail "the checkout already has another prepared game input"
    else
        mkdir -p "$(dirname "$destination")"
        ln -s "$YAKUMO_PRIVATE_INPUTS/$file" "$destination"
    fi
done

export YAKUMO_PRIVATE_PATH_MAPS=1
export CMAKE_BUILD_PARALLEL_LEVEL=2
export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
mkdir -p "$YAKUMO_WORK"
# Use the packager's identical dependency recipes and stamps; no second SDL3 or
# Vulkan loader build or unpinned Homebrew dependencies are needed afterwards.
bash profiles/mhp3rd/scripts/release_macos.sh --prepare-deps --jobs 2

sources="$YAKUMO_WORK/sources"
deps="$YAKUMO_WORK/deps"
build="$YAKUMO_WORK/build"
tool_src="$YAKUMO_WORK/deps-build/glslang-$GLSLANG_COMMIT"
tool_build="$YAKUMO_WORK/deps-build/glslang-build"
archive="$sources/glslang-$GLSLANG_COMMIT.tar.gz"
if [[ ! -f "$archive" || "$(shasum -a 256 "$archive" | cut -d' ' -f1)" != "$GLSLANG_SHA256" ]]; then
    curl -fsSL --retry 3 --connect-timeout 30 --max-time 300 "$GLSLANG_URL" -o "$archive.part"
    [[ "$(shasum -a 256 "$archive.part" | cut -d' ' -f1)" == "$GLSLANG_SHA256" ]] || fail "shader compiler checksum mismatch"
    mv "$archive.part" "$archive"
fi
[[ -d "$tool_src" ]] || tar -xzf "$archive" -C "$YAKUMO_WORK/deps-build"
maps="-ffile-prefix-map=$repo_dir=/yakumo/source -ffile-prefix-map=$YAKUMO_WORK=/yakumo/work"
cmake -S "$tool_src" -B "$tool_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
    "-DCMAKE_C_FLAGS=$maps" "-DCMAKE_CXX_FLAGS=$maps" \
    -DBUILD_EXTERNAL=OFF -DGLSLANG_TESTS=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF \
    -DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_ENABLE_INSTALL=ON -DENABLE_PCH=OFF
cmake --build "$tool_build" --target glslang-standalone -j2
# Only this host tool is needed. The upstream install target also expects
# optional static libraries that glslang-standalone does not build.
mkdir -p "$deps/bin"
install -m 755 "$tool_build/StandAlone/glslang" "$deps/bin/glslang"
export PATH="$deps/bin:$PATH"

cmake -S "$repo_dir" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp3rd \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
    "-DCMAKE_CXX_FLAGS=$maps" -DCMAKE_PREFIX_PATH="$deps" \
    -DVulkan_INCLUDE_DIR="$deps/include" -DVulkan_LIBRARY="$deps/lib/libvulkan.1.dylib" \
    -DMHP3RD_GLSLANG="$deps/bin/glslang" -DMHP3RD_RENDERER=ON \
    -DMHP3RD_RELEASE=ON -DMHP3RD_FFMPEG=bundled \
    -DMHP3RD_FFMPEG_DOWNLOAD_DIR="$sources" \
    -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF \
    -DPSPRECOMP_CCACHE=OFF -DPSPRECOMP_GENERATED_JOBS=1 \
    2>&1 | tee "$YAKUMO_WORK/configure.log"
# Renderer discovery is optional for developer builds. A nightly must fail if
# a missing dependency would silently produce an application without a window.
grep -qx '.*mhp3rd: Vulkan renderer enabled' "$YAKUMO_WORK/configure.log" || fail "the Vulkan renderer is required"
bash profiles/mhp3rd/scripts/generate.sh "$build"
cmake --build "$build" --target Yakumo -j2
mkdir -p "$build/bin/overlays"
cp "$YAKUMO_PRIVATE_INPUTS/overlays/"*.dylib "$build/bin/overlays/"
bash profiles/mhp3rd/scripts/release_macos.sh --version "$NIGHTLY_VERSION" \
    --jobs 2 --no-dmg --zip "$build"
