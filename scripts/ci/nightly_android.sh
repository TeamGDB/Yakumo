#!/usr/bin/env bash
# Build an aligned unsigned arm64 APK. The caller captures this command's
# output privately; only the inspected APK and pinned FFmpeg source may leave.
set -euo pipefail
umask 077
ulimit -c 0

repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
profile_dir="$repo_dir/profiles/mhp3rd"
cd "$repo_dir"
source profiles/mhp3rd/packaging/macos/sources.sh
fail() { echo "error: $*" >&2; exit 1; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || fail 'an x86-64 Linux host is required'
: "${YAKUMO_PRIVATE_INPUTS:?verified private inputs are required}"
: "${YAKUMO_WORK:?a private workspace is required}"
: "${NIGHTLY_VERSION:?a nightly version is required}"
: "${ANDROID_HOME:?the Android SDK is required}"
[[ "$NIGHTLY_VERSION" =~ ^nightly-[0-9a-f]{12}$ ]] || fail 'invalid nightly version'
[[ "$YAKUMO_WORK" == /* && "$YAKUMO_PRIVATE_INPUTS" == /* && "$ANDROID_HOME" == /* ]] || fail 'absolute paths are required'
[[ ! -L "$YAKUMO_WORK" ]] || fail 'the private workspace cannot be a symbolic link'
[[ -z "${DEBUGGABLE:-}" && -z "${DEVICE_TEST_PACKAGE:-}" ]] || fail 'nightlies require the production release identity'
for tool in cmake ninja python3 curl tar sha256sum java javac zip make; do
    command -v "$tool" > /dev/null || fail 'missing Android build prerequisite'
done

ndk="$ANDROID_HOME/ndk/28.2.13676358"
toolchain="$ndk/build/cmake/android.toolchain.cmake"
llvm="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
[[ -f "$toolchain" && -f "$ndk/source.properties" ]] || fail 'Android NDK 28.2.13676358 is required'
grep -Eq '^Pkg.Revision[[:space:]]*=[[:space:]]*28\.2\.13676358[[:space:]]*$' "$ndk/source.properties" || fail 'unexpected Android NDK revision'
for file in build-tools/35.0.0/aapt2 build-tools/35.0.0/d8 build-tools/35.0.0/zipalign platforms/android-35/android.jar; do
    [[ -f "$ANDROID_HOME/$file" ]] || fail 'Android SDK platform 35 and build-tools 35.0.0 are required'
done
python3 scripts/ci/nightly.py verify-overlays --platform android-arm64 --inputs "$YAKUMO_PRIVATE_INPUTS"
for file in EBOOT.ELF disc.iso; do
    [[ -f "$YAKUMO_PRIVATE_INPUTS/$file" && ! -L "$YAKUMO_PRIVATE_INPUTS/$file" ]] || fail 'missing regular private build input'
    destination="$profile_dir/game/$file"
    if [[ -e "$destination" || -L "$destination" ]]; then
        [[ -L "$destination" && "$(readlink "$destination")" == "$YAKUMO_PRIVATE_INPUTS/$file" ]] || fail 'the checkout already has another prepared game input'
    else
        mkdir -p "$(dirname "$destination")"
        ln -s "$YAKUMO_PRIVATE_INPUTS/$file" "$destination"
    fi
done

work="$YAKUMO_WORK"
sources="$work/sources"
deps_build="$work/deps-build"
deps="$work/deps"
build="$work/build"
host_tools="$work/host-tools"
mkdir -p "$sources" "$deps_build" "$deps/bin" "$work/dist"
export CMAKE_BUILD_PARALLEL_LEVEL=2
export ANDROID_NDK="$ndk" ANDROID_BUILD_TOOLS_VERSION=35.0.0 ANDROID_COMPILE_SDK_VERSION=35
maps="-ffile-prefix-map=\"$repo_dir=/yakumo/source\" -ffile-prefix-map=\"$work=/yakumo/work\""
fetch() {
    local target="$sources/$1"
    if [[ -f "$target" ]] && echo "$3  $target" | sha256sum -c --status; then return; fi
    curl -fsSL --retry 3 --connect-timeout 30 --max-time 600 --proto '=https' --tlsv1.2 -o "$target.part" "$2"
    echo "$3  $target.part" | sha256sum -c --status || { rm -f "$target.part"; return 1; }
    mv "$target.part" "$target"
}

fetch "glslang-$GLSLANG_COMMIT.tar.gz" "$GLSLANG_URL" "$GLSLANG_SHA256"
fetch "SDL3-$SDL3_VERSION.tar.gz" "$SDL3_URL" "$SDL3_SHA256"
fetch NotoSansCJKjp-Regular.otf "$NOTO_CJK_URL" "$NOTO_CJK_SHA256"
fetch NotoSansCJK-LICENSE.txt "$NOTO_CJK_LICENSE_URL" "$NOTO_CJK_LICENSE_SHA256"
ffmpeg_cmake="$profile_dir/cmake/FFmpeg.cmake"
ffmpeg_version="$(sed -n 's/^set(MHP3RD_FFMPEG_VERSION \(.*\))$/\1/p' "$ffmpeg_cmake" | head -1)"
ffmpeg_sha="$(sed -n 's/^set(MHP3RD_FFMPEG_SHA256 "\{0,1\}\([0-9a-f]\{64\}\)"\{0,1\})$/\1/p' "$ffmpeg_cmake" | head -1)"
[[ "$ffmpeg_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$ffmpeg_sha" =~ ^[0-9a-f]{64}$ ]] || fail 'missing FFmpeg source pins'
fetch "ffmpeg-$ffmpeg_version.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" "$ffmpeg_sha"

tar -xzf "$sources/glslang-$GLSLANG_COMMIT.tar.gz" -C "$deps_build"
cmake -S "$deps_build/glslang-$GLSLANG_COMMIT" -B "$deps_build/glslang-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$maps" "-DCMAKE_CXX_FLAGS=$maps" \
    -DBUILD_EXTERNAL=OFF -DGLSLANG_TESTS=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF \
    -DENABLE_GLSLANG_BINARIES=ON -DENABLE_PCH=OFF
cmake --build "$deps_build/glslang-build" --target glslang-standalone --parallel 2
# The install target expects unrelated optional libraries; use only this host tool.
install -m 755 "$deps_build/glslang-build/StandAlone/glslang" "$deps/bin/glslang"

# Code generators must execute on this host, independently of the cross target.
cmake -S "$repo_dir" -B "$host_tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPSPRECOMP_PROFILE= -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF -DPSPRECOMP_CCACHE=OFF \
    "-DCMAKE_C_FLAGS=$maps" "-DCMAKE_CXX_FLAGS=$maps"
bash profiles/mhp3rd/scripts/generate.sh "$host_tools"

tar -xzf "$sources/SDL3-$SDL3_VERSION.tar.gz" -C "$deps_build"
sdl="$deps/sdl3-android"
cmake -S "$deps_build/SDL3-$SDL3_VERSION" -B "$deps_build/sdl3-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sdl" \
    "-DCMAKE_C_FLAGS=$maps" "-DCMAKE_CXX_FLAGS=$maps" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build "$deps_build/sdl3-build" --parallel 2
cmake --install "$deps_build/sdl3-build"

cmake -S "$repo_dir" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp3rd -DMHP3RD_ANDROID_APP=ON \
    -DMHP3RD_RELEASE=ON -DMHP3RD_RENDERER=ON -DMHP3RD_FFMPEG=bundled \
    -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF -DPSPRECOMP_CCACHE=OFF \
    -DPSPRECOMP_GENERATED_JOBS=2 "-DCMAKE_C_FLAGS=$maps" "-DCMAKE_CXX_FLAGS=$maps" \
    -DSDL3_DIR="$sdl/lib/cmake/SDL3" -DCMAKE_FIND_ROOT_PATH="$sdl" \
    -DVulkan_INCLUDE_DIR="$llvm/sysroot/usr/include" \
    -DVulkan_LIBRARY="$llvm/sysroot/usr/lib/aarch64-linux-android/29/libvulkan.so" \
    -DMHP3RD_GLSLANG="$deps/bin/glslang" -DMHP3RD_FFMPEG_DOWNLOAD_DIR="$sources" \
    2>&1 | tee "$work/configure.log"
for feature in 'mhp3rd: Vulkan renderer enabled' 'mhp3rd: bundled FFmpeg'; do
    grep -Fq "$feature" "$work/configure.log" || fail 'required renderer/audio feature unavailable'
done
cmake --build "$build" --target Yakumo --parallel 2
mkdir -p "$build/bin/overlays"
cp "$YAKUMO_PRIVATE_INPUTS/overlays/"libovl*.so "$build/bin/overlays/"

NIGHTLY_UNSIGNED=1 FONT_DIR="$sources" VERSION_NAME="$NIGHTLY_VERSION" \
    VERSION_CODE="$(git rev-list --count HEAD)" \
    bash profiles/mhp3rd/packaging/android/build_apk.sh "$build" "$deps_build/SDL3-$SDL3_VERSION" \
    "$sdl/lib/libSDL3.so" "$work/dist/yakumo-$NIGHTLY_VERSION-android-arm64.apk"
cp "$sources/ffmpeg-$ffmpeg_version.tar.xz" "$work/dist/"
