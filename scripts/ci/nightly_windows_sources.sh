# Public, checksum-pinned inputs for the native Windows nightly toolchain.
# Vulkan headers/loader, glslang and SDL's version share release/nightly pins.
# shellcheck source=../../profiles/mhp3rd/packaging/macos/sources.sh
source "$(dirname "${BASH_SOURCE[0]}")/../../profiles/mhp3rd/packaging/macos/sources.sh"

SDL3_VC_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-devel-${SDL3_VERSION}-VC.zip"
SDL3_VC_SHA256=1a784cb2a5c64d56fe7a62090fe9d242d9865f235e4ea9678f1a6ba4e693e7de

# The prebuilt's filename identifies FFmpeg 7.1.5 plus one upstream commit.
# Publish that exact unmodified source, rather than the earlier 7.1.5 tarball.
NIGHTLY_WINDOWS_FFMPEG_SOURCE_NAME=ffmpeg-7d0e8420048cffd0ca3883b877ead2390496d0b2.tar.gz
NIGHTLY_WINDOWS_FFMPEG_SOURCE_URL=https://github.com/FFmpeg/FFmpeg/archive/7d0e8420048cffd0ca3883b877ead2390496d0b2.tar.gz
NIGHTLY_WINDOWS_FFMPEG_SOURCE_SHA256=2caafb2bbfb69c0518470651640e71ac7f5fb3117d188bf6ea2d909307a02b1d
