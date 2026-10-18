#!/usr/bin/env python3
"""Create public synthetic inputs for a complete hosted packaging probe.

The MIPS instructions, module and disc sentinel are original tiny fixtures,
not game content. The resulting application is never run or published. Its
purpose is to exercise dependency bootstrap, generation, linking and packaging
without cloud credentials or original game files.
"""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import nightly


ENTRY = 0x08804000
TERMINAL = 0x08804020
DISPATCH = 0x088E8B18
CAMERA = 0x08878B70
HUD = (0x08876874, 0x088EA9B0, 0x088EB07C, 0x088EBF0C, 0x088EC060,
       0x088EC0F8, 0x088EC1F0, 0x088EC2EC, 0x088EC3E0, 0x088EC63C,
       0x088ECB74, 0x088ECC38, 0x088ECCE4, 0x088ECD9C, 0x088ECE58)
LAYERED = (0x08869778, 0x088A5564)
HOOKS = (CAMERA, *HUD, *LAYERED)
NDK_VERSION = '28.2.13676358'
ANDROID_SDK_ROOTS = (Path('/usr/local/lib/android/sdk'),
                     Path.home() / 'Library/Android/sdk', Path.home() / 'Android/Sdk')
DISC_SENTINEL = b'Yakumo public packaging probe. Not a disc image; never execute this application.\n'
MODULE_SOURCE = '''#if defined(_WIN32)
#define PROBE_EXPORT __declspec(dllexport)
#else
#define PROBE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PROBE_EXPORT int yakumo_public_packaging_probe() { return 0; }
'''


def synthetic_elf():
    """Build a sparse ELF32 with 21 tiny loaded executable function sections.

    The entry and dispatcher directly call every required profile hook so the
    normal analyzer discovers their registries. All instructions are standard
    JAL, JR RA and NOP encodings; no bytes come from an original executable.
    """
    def call(address):
        return (0x0C000000 | ((address >> 2) & 0x03FFFFFF), 0)

    bodies = {address: (0x03E00008, 0) for address in (*HOOKS, TERMINAL)}
    bodies[ENTRY] = (*call(DISPATCH), 0x03E00008, 0)
    bodies[DISPATCH] = tuple(word for target in (*HOOKS, TERMINAL) for word in call(target)) + (0x03E00008, 0)
    functions = sorted(bodies.items())
    count = len(functions)
    offset = 52 + count * 32
    content, records = bytearray(), []
    for address, words in functions:
        code = struct.pack('<' + 'I' * len(words), *words)
        records.append((address, offset + len(content), len(code)))
        content.extend(code)
    strings = bytearray(b'\0')
    names = []
    for address, _, _ in records:
        names.append(len(strings))
        strings.extend(f'.probe_{address:08x}'.encode() + b'\0')
    table_name = len(strings)
    strings.extend(b'.shstrtab\0')
    strings_offset = offset + len(content)
    content.extend(strings)
    content.extend(bytes((-len(content) - offset) % 4))
    sections_offset = offset + len(content)
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    header = struct.pack('<16sHH5I6H', ident, 2, 8, 1, ENTRY, 52, sections_offset,
                         0, 52, 32, count, 40, count + 2, count + 1)
    programs = b''.join(struct.pack('<8I', 1, start, address, address, amount, amount, 5, 4)
                        for address, start, amount in records)
    sections = bytearray(40)
    for name, (address, start, amount) in zip(names, records):
        sections.extend(struct.pack('<10I', name, 1, 6, address, start, amount, 0, 0, 4, 0))
    sections.extend(struct.pack('<10I', table_name, 3, 0, 0, strings_offset, len(strings), 0, 0, 1, 0))
    return header + programs + content + sections


def toolchain_arguments(platform):
    if platform == 'android-arm64':
        home = os.environ.get('ANDROID_HOME')
        # The environment selects a known host installation; it never supplies
        # a path used for filesystem access or execution of a CMake toolchain.
        sdk = next((candidate for candidate in ANDROID_SDK_ROOTS if home == str(candidate)), None)
        if sdk is None:
            raise nightly.Rejected('The public Android probe needs the pinned Android SDK.')
        toolchain = sdk
        for component in ('ndk', NDK_VERSION, 'build', 'cmake', 'android.toolchain.cmake'):
            if toolchain.is_symlink():
                raise nightly.Rejected('The public Android probe requires a regular SDK installation.')
            toolchain = toolchain / component
        if toolchain.is_symlink() or not toolchain.is_file():
            raise nightly.Rejected('The public Android probe needs the pinned Android NDK.')
        return ['-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), '-DANDROID_ABI=arm64-v8a',
                '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_static']
    if platform == 'macos-arm64':
        return ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0']
    return []


def create_probe(output, platform, root=nightly.ROOT, runner=None):
    if platform not in nightly.PLATFORMS:
        raise nightly.Rejected('Unsupported public probe platform.')
    options = toolchain_arguments(platform)
    run = runner or subprocess.run
    output = nightly.fresh(Path(output).absolute())
    try:
        (output / 'EBOOT.ELF').write_bytes(synthetic_elf())
        (output / 'disc.iso').write_bytes(DISC_SENTINEL)
        build_root = output / 'probe-build'
        source, build = build_root / 'source', build_root / 'build'
        source.mkdir(parents=True)
        (source / 'probe.cpp').write_text(MODULE_SOURCE)
        suffix = {'linux-x86_64': '.so', 'macos-arm64': '.dylib',
                  'windows-x86_64': '.dll', 'android-arm64': '.so'}[platform]
        cmake = ('cmake_minimum_required(VERSION 3.20)\n'
                 'project(YakumoPublicPackagingProbe LANGUAGES CXX)\n'
                 'add_library(probe_overlay MODULE probe.cpp)\n'
                 'set_target_properties(probe_overlay PROPERTIES PREFIX "" '
                 f'SUFFIX "{suffix}" LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/bin" '
                 'RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")\n')
        if platform == 'macos-arm64':
            cmake += 'target_link_options(probe_overlay PRIVATE -Wl,-no_adhoc_codesign)\n'
        (source / 'CMakeLists.txt').write_text(cmake)
        run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release', *options], check=True, timeout=180)
        run(['cmake', '--build', str(build), '--target', 'probe_overlay', '-j2'],
            check=True, timeout=180)
        library = nightly.regular(build / 'bin' / ('probe_overlay' + suffix))
        overlays = output / 'overlays'
        overlays.mkdir()
        for index in range(355):
            prefix = 'lib' if platform == 'android-arm64' else ''
            shutil.copyfile(library, overlays / f'{prefix}ovl00000000_public_probe_{index:03d}{suffix}')
        nightly.write_overlay_marker(output, platform, root)
        nightly.verify_overlays(output, root, platform)
        return output
    except BaseException:
        # Never delete a preexisting output: fresh() succeeded before this try.
        shutil.rmtree(output)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=nightly.PLATFORMS, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--install', action='store_true', help='Install only these synthetic inputs into the fresh probe checkout')
    arguments = parser.parse_args(argv)
    output = create_probe(arguments.output, arguments.platform)
    if arguments.install:
        nightly.install_inputs(output)
    print('Public synthetic packaging inputs are ready; the application must not be executed or published.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:
        print('The public packaging probe could not be prepared.', file=sys.stderr)
        sys.exit(1)
