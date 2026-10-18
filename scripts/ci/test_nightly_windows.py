"""Public orchestration contracts; native MSVC is exercised by the PR probe."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_nightly_linux import BASH

ROOT = Path(__file__).resolve().parents[2]

# Stub external executables, not the builder: run its real hash, path, target,
# cache-validation and packaging hand-off decisions with synthetic public files.
TOOL = '''#!{python}
import hashlib, json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['TOOL_LOG'], 'a') as log:
    log.write(json.dumps([name, *args]) + '\\n')
if name == 'uname':
    print(os.environ.get('MOCK_HOST', 'MINGW64_NT'))
elif name == 'cygpath':
    print(args[-1])
elif name == 'python3':
    if 'verify-overlays' in args:
        sys.exit(int(os.environ.get('REJECT_CACHE', '0')))
    os.execv(sys.executable, [sys.executable, *args])
elif name == 'curl':
    # A native Windows downloader cannot write a Git Bash /drive path when
    # conversion has been disabled for every argument (hosted curl error 23).
    if os.environ.get('MSYS2_ARG_CONV_EXCL') == '*':
        sys.exit(23)
    contents = b'checksum-pinned public dependency fixture' if os.environ.get('VALID_DOWNLOAD') else b'corrupt download'
    pathlib.Path(args[args.index('-o') + 1]).write_bytes(contents)
elif name == 'sha256sum':
    checksum, path = sys.stdin.read().rstrip('\\n').split('  ', 1)
    sys.exit(hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() != checksum)
elif name == 'dumpbin':
    if os.environ.get('DUMPBIN_FAIL'):
        sys.exit(int(os.environ['DUMPBIN_FAIL']))
    if os.environ.get('DUMPBIN_IMPORT'):
        print('    ' + os.environ['DUMPBIN_IMPORT'])
elif name == 'git':
    print('0' * 40)
elif name == 'tar':
    # Model the GNU tar installed by Git for Windows: ZIP is unsupported.
    if any(argument.endswith('.zip') for argument in args):
        sys.exit(2)
elif name == 'cmake':
    for argument in args:
        if argument.startswith(('-DCMAKE_C_FLAGS_INIT=', '-DCMAKE_CXX_FLAGS_INIT=')):
            prefix = argument.split('=', 1)[0] + '='
            if prefix not in os.environ.get('MSYS2_ARG_CONV_EXCL', '').split(';'):
                sys.exit(88)
    if '-S' in args and args[args.index('-S') + 1] == os.environ['MOCK_REPO']:
        build = pathlib.Path(args[args.index('-B') + 1])
        build.mkdir(parents=True, exist_ok=True)
        for tool in ['psp_analyze.exe', 'psp_recomp.exe']:
            destination = build / tool
            destination.write_text(pathlib.Path(__file__).read_text())
            destination.chmod(0o755)
        print('mhp3rd: Vulkan renderer enabled')
        if not os.environ.get('DISABLE_AUDIO'):
            print('mhp3rd: bundled FFmpeg')
    elif '--build' in args and 'Yakumo' in args:
        build = pathlib.Path(args[args.index('--build') + 1])
        (build / 'bin').mkdir(parents=True, exist_ok=True)
        (build / 'bin/Yakumo.exe').write_bytes(b'synthetic public host')
elif name == 'psp_analyze.exe':
    pathlib.Path(args[1]).write_text('{{}}')
elif name == 'psp_recomp.exe':
    destination = pathlib.Path(args[args.index('--auto') + 1])
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'unit.cpp').write_text('// synthetic public fixture\\n')
'''


@unittest.skipUnless(BASH, 'Bash 4+ is required for Windows release shell contracts')
class WindowsNightlyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='yakumo windows fixture ')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo = self.base / 'repo with spaces'
        scripts = self.repo / 'scripts/ci'
        scripts.mkdir(parents=True)
        shutil.copyfile(ROOT / 'scripts/ci/nightly_windows.sh', scripts / 'nightly_windows.sh')
        (scripts / 'nightly.py').write_text('# synthetic verifier command fixture\n')
        self.work = self.base / 'private work'
        self.inputs = self.base / 'private inputs'
        (self.inputs / 'overlays').mkdir(parents=True)
        self.overlay_names = [f'ovl{i:03}.dll' for i in range(355)]
        for name in self.overlay_names:
            (self.inputs / 'overlays' / name).write_bytes(b'synthetic public DLL ' + name.encode())
        game = self.repo / 'profiles/mhp3rd/game'
        game.mkdir(parents=True)
        for name in ['EBOOT.ELF', 'disc.iso']:
            (self.inputs / name).write_bytes(b'public fixture, not game data')
            (game / name).write_bytes(b'public fixture, not game data')
        packaging = self.repo / 'profiles/mhp3rd/scripts'
        packaging.mkdir()
        (packaging / 'release_windows.sh').write_text('''#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == --version && "$3" == --no-installed ]]
[[ "$(find "$4/bin/overlays" -name '*.dll' | wc -l | tr -d ' ')" == 355 ]]
mkdir -p "$YAKUMO_WORK/dist"
cp "$4/bin/Yakumo.exe" "$YAKUMO_WORK/dist/yakumo-$2-windows-x86_64-portable.zip"
''')
        data = b'checksum-pinned public dependency fixture'
        checksum = hashlib.sha256(data).hexdigest()
        sources = self.work / 'sources'
        sources.mkdir(parents=True)
        self.archive_names = ['SDL3-devel-3.4.16-VC.zip', 'vulkan-headers.tar.gz',
                              'vulkan-loader.tar.gz', 'glslang.tar.gz', 'ffmpeg-public-fixture.tar.gz']
        for name in self.archive_names:
            (sources / name).write_bytes(data)
        (scripts / 'nightly_windows_sources.sh').write_text(
            'SDL3_VERSION=3.4.16\nVULKAN_SDK_TAG=fixture-sdk\nGLSLANG_COMMIT=fixture-revision\n'
            + 'NIGHTLY_WINDOWS_FFMPEG_SOURCE_NAME=ffmpeg-public-fixture.tar.gz\n'
            + ''.join(f'{name}_URL=https://example.invalid/public-source\n{name}_SHA256={checksum}\n'
                      for name in ['SDL3_VC', 'VULKAN_HEADERS', 'VULKAN_LOADER', 'GLSLANG',
                                   'NIGHTLY_WINDOWS_FFMPEG_SOURCE']))
        tools = self.base / 'tools'
        tools.mkdir()
        for name in ['uname', 'cygpath', 'cl', 'dumpbin', 'cmake', 'ninja', 'python3', 'curl', 'tar', 'sha256sum']:
            path = tools / name
            path.write_text(TOOL.format(python=sys.executable))
            path.chmod(0o755)
        system = self.work / 'windows/System32'
        system.mkdir(parents=True)
        windows_tar = system / 'tar.exe'
        windows_tar.write_text(TOOL.format(python=sys.executable))
        windows_tar.chmod(0o755)
        self.log = self.base / 'tools.jsonl'
        self.env = {**os.environ, 'PATH': str(tools) + os.pathsep + os.environ['PATH'],
                    'YAKUMO_PRIVATE_INPUTS': str(self.inputs), 'YAKUMO_WORK': str(self.work),
                    'NIGHTLY_VERSION': 'nightly-0123456789ab', 'VCToolsVersion': '14.44.35207',
                    'SYSTEMROOT': str(self.work / 'windows'),
                    'TOOL_LOG': str(self.log), 'MOCK_REPO': str(self.repo)}

    def run_builder(self, **settings):
        return subprocess.run([BASH, str(self.repo / 'scripts/ci/nightly_windows.sh')],
                              env={**self.env, **settings}, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=20)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_builds_only_native_host_and_reuses_all_verified_dlls(self):
        result = self.run_builder()
        self.assertEqual(result.returncode, 0, result.stdout)
        commands = self.commands()
        zipped = [command for command in commands if command[0] in ('tar', 'tar.exe')
                  and any(argument.endswith('.zip') for argument in command[1:])]
        self.assertEqual([command[0] for command in zipped], ['tar.exe'])
        self.assertFalse(any(command[0] == 'tar' and any(argument.endswith('.zip') for argument in command)
                             for command in commands))
        self.assertIn(['python3', str(self.repo / 'scripts/ci/nightly.py'), 'verify-overlays',
                       '--platform', 'windows-x86_64', '--inputs', str(self.inputs)], commands)
        builds = [command for command in commands if command[0] == 'cmake' and '--build' in command]
        self.assertEqual(len(builds), 4)  # Vulkan loader, glslang, generators, host.
        host_builds = [command for command in builds if command[2] == str(self.work / 'build')]
        self.assertEqual([command[command.index('--target') + 1:command.index('--parallel')]
                          for command in host_builds], [['psp_analyze', 'psp_recomp'], ['Yakumo']])
        self.assertTrue(all(command[-2:] == ['--parallel', '2'] for command in builds))
        self.assertFalse(any('build_overlays.sh' in argument for command in commands for argument in command))
        configured = next(command for command in commands if '-DMHP3RD_RELEASE=ON' in command)
        self.assertIn('-DMHP3RD_RENDERER=ON', configured)
        self.assertIn('-DMHP3RD_FFMPEG=bundled', configured)
        self.assertIn('-DPSPRECOMP_MSVC_MP_JOBS=1', configured)
        self.assertIn('-DSDL3_DIR=' + str(self.work / 'deps-build/SDL3-3.4.16/cmake'), configured)
        for prefix in ['-DCMAKE_C_FLAGS_INIT=', '-DCMAKE_CXX_FLAGS_INIT=']:
            flags = next(argument[len(prefix):] for argument in configured if argument.startswith(prefix))
            self.assertIn('/experimental:deterministic', flags)
            self.assertIn('/pathmap:', flags)
        self.assertFalse(any(argument.startswith(('-DCMAKE_C_FLAGS=', '-DCMAKE_CXX_FLAGS=')) for argument in configured))
        self.assertEqual(sorted(path.name for path in (self.work / 'build/bin/overlays').iterdir()), self.overlay_names)
        for name in self.overlay_names:
            self.assertEqual((self.work / 'build/bin/overlays' / name).read_bytes(),
                             (self.inputs / 'overlays' / name).read_bytes())
        self.assertEqual([path.name for path in (self.work / 'dist').iterdir()],
                         ['yakumo-nightly-0123456789ab-windows-x86_64-portable.zip'])

    def test_rejected_cache_does_not_fetch_or_compile(self):
        result = self.run_builder(REJECT_CACHE='3')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(command[0] in ('curl', 'cmake') for command in self.commands()))

    def test_wrong_platform_toolchain_or_version_stops_before_private_inputs(self):
        for settings in [{'MOCK_HOST': 'Darwin'}, {'VCToolsVersion': '14.43.1'},
                         {'NIGHTLY_VERSION': '../unexpected'}, {'NIGHTLY_VERSION': 'nightly-123'}]:
            with self.subTest(settings=settings):
                self.log.unlink(missing_ok=True)
                result = self.run_builder(**settings)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(command[0] in ('python3', 'cmake', 'curl') for command in self.commands()))

    def test_incomplete_or_linked_input_stops_before_download(self):
        executable = self.inputs / 'EBOOT.ELF'
        executable.unlink()
        self.assertNotEqual(self.run_builder().returncode, 0)
        executable.symlink_to(self.repo / 'profiles/mhp3rd/game/EBOOT.ELF')
        self.assertNotEqual(self.run_builder().returncode, 0)
        self.assertFalse(any(command[0] in ('curl', 'cmake') for command in self.commands()))

    def test_corrupt_dependency_cannot_be_extracted_or_executed(self):
        (self.work / 'sources' / self.archive_names[0]).write_bytes(b'corruption')
        result = self.run_builder()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any(command[0] == 'curl' for command in self.commands()))
        self.assertFalse(any(command[0] in ('tar', 'cmake') for command in self.commands()))
        self.assertFalse((self.work / 'sources' / (self.archive_names[0] + '.part')).exists())

    def test_fresh_native_download_keeps_filesystem_argument_conversion(self):
        (self.work / 'sources' / self.archive_names[0]).unlink()
        result = self.run_builder(VALID_DOWNLOAD='1')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(any(command[0] == 'curl' for command in self.commands()))
        self.assertTrue((self.work / 'dist/yakumo-nightly-0123456789ab-windows-x86_64-portable.zip').is_file())

    def test_missing_audio_does_not_generate_or_package(self):
        result = self.run_builder(DISABLE_AUDIO='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(command[0].startswith('psp_') for command in self.commands()))
        self.assertFalse((self.work / 'dist').exists())

    def test_portable_readme_producer_canonicalizes_lf_and_crlf_checkout_templates(self):
        source = (ROOT / 'profiles/mhp3rd/scripts/release_windows.sh').read_text()
        writer = source[source.index('write_readme() {'):source.index('\npack() {')]
        template = (ROOT / 'profiles/mhp3rd/packaging/windows/README-portable.txt.in').read_bytes().replace(b'\r\n', b'\n')
        expected = template.replace(b'@VERSION@', b'nightly-0123456789ab').replace(b'\n', b'\r\n')
        path = self.repo / 'profiles/mhp3rd/packaging/windows/README-portable.txt.in'
        path.parent.mkdir(parents=True)
        output = self.work / 'public-readme.txt'
        for newline in [b'\n', b'\r\n']:
            with self.subTest(checkout_newline=newline):
                path.write_bytes(template.replace(b'\n', newline))
                result = subprocess.run([BASH, '-c', writer + '\nprofile_dir="$1"; '
                                         'version=nightly-0123456789ab; write_readme "$2" 1',
                                         'public-readme-fixture', str(self.repo / 'profiles/mhp3rd'), str(output)],
                                        env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(output.read_bytes(), expected)
                self.assertNotIn(b'\r\r\n', output.read_bytes())

    def test_only_documented_vulkan_driver_import_is_allowed_without_system_dll(self):
        source = (ROOT / 'profiles/mhp3rd/scripts/release_windows.sh').read_text()
        block = source[source.index('declare -A packaged=()'):source.index('\n# The package must hold no game data')]
        # Exercise the actual importer on a minimal public stage, keeping the
        # 355-module/cache and archive contracts in their dedicated tests.
        stage = self.work / 'public-import-stage'
        stage.mkdir()
        (stage / 'Yakumo.exe').write_bytes(b'public native fixture')
        lower = next(line for line in source.splitlines() if line.startswith('lower()'))
        command = ('set -euo pipefail\nroot="$1"; work="$2"\n'
                   'fail() { echo "$*" >&2; exit 1; }\n' + lower + '\n' + block)
        for dll, accepted in [('vulkan-1.dll', True), ('unexpected-private.dll', False)]:
            with self.subTest(dll=dll):
                result = subprocess.run([BASH, '-c', command, 'public-import-fixture', str(stage), str(self.work)],
                                        env={**self.env, 'DUMPBIN_IMPORT': dll}, capture_output=True,
                                        text=True, timeout=10)
                self.assertEqual(result.returncode == 0, accepted, result.stdout + result.stderr)

    def release_fixture(self):
        shutil.copyfile(ROOT / 'profiles/mhp3rd/scripts/release_windows.sh',
                        self.repo / 'profiles/mhp3rd/scripts/release_windows.sh')
        build = self.work / 'build'
        (build / 'bin/overlays').mkdir(parents=True)
        for path in (self.inputs / 'overlays').iterdir():
            shutil.copyfile(path, build / 'bin/overlays' / path.name)
        sdl = self.work / 'sdl'
        for directory in ['cmake', 'lib/x64', 'include/SDL3']:
            (sdl / directory).mkdir(parents=True)
        (sdl / 'lib/x64/SDL3.dll').write_bytes(b'synthetic public SDL')
        (sdl / 'LICENSE.txt').write_text('Public test fixture\n')
        (sdl / 'include/SDL3/SDL_version.h').write_text(
            '#define SDL_MAJOR_VERSION 3\n#define SDL_MINOR_VERSION 4\n#define SDL_MICRO_VERSION 16\n')
        (build / 'CMakeCache.txt').write_text(
            f'MHP3RD_RELEASE:BOOL=ON\nSDL3_DIR:PATH={sdl}/cmake\nCMAKE_CXX_COMPILER:FILEPATH=cl\n')
        for name in ['Yakumo.exe', 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll',
                     'FFmpeg-LICENSE.txt', 'FFmpeg-SOURCE.txt']:
            (build / 'bin' / name).write_text('Public test fixture\n')
        crt = self.work / 'redist/x64/Microsoft.VC143.CRT'
        crt.mkdir(parents=True)
        for name in ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']:
            (crt / name).write_bytes(b'synthetic public CRT')
        for name in ['LICENSE', 'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
                     'profiles/mhp3rd/third_party/imgui/LICENSE.txt',
                     'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE',
                     'profiles/mhp3rd/third_party/xxhash/LICENSE']:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('Public test fixture\n')
        system = self.work / 'windows/System32'
        system.mkdir(parents=True, exist_ok=True)
        tar = system / 'tar.exe'
        tar.write_text('#!/bin/sh\nexit 99\n')  # A failure must never reach packing.
        tar.chmod(0o755)
        git = self.base / 'tools/git'
        git.write_text(TOOL.format(python=sys.executable))
        git.chmod(0o755)
        return build, {'VCToolsRedistDir': str(self.work / 'redist'),
                       'SYSTEMROOT': str(self.work / 'windows')}

    def run_release(self, build, settings):
        return subprocess.run([BASH, str(self.repo / 'profiles/mhp3rd/scripts/release_windows.sh'),
                               '--version', 'nightly-0123456789ab', '--no-installed', str(build)],
                              env={**self.env, **settings}, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=30)

    def test_packager_requires_verified_cache_without_overlay_corpora(self):
        build, settings = self.release_fixture()
        result = self.run_release(build, {**settings, 'REJECT_CACHE': '2'})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / 'dist').exists())
        self.assertFalse(any(command[0] == 'dumpbin' for command in self.commands()))

    def test_packager_rejects_dll_different_from_verified_seed(self):
        build, settings = self.release_fixture()
        (build / 'bin/overlays/ovl000.dll').write_bytes(b'changed DLL')
        result = self.run_release(build, settings)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('a built overlay differs from the verified cache', result.stdout)
        self.assertFalse((self.work / 'dist').exists())

    def test_failed_import_inspection_is_not_silently_accepted(self):
        build, settings = self.release_fixture()
        result = self.run_release(build, {**settings, 'DUMPBIN_FAIL': '1'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('DLL import inspection failed', result.stdout)
        self.assertFalse(list((self.work / 'dist').glob('*.zip')))


if __name__ == '__main__':
    unittest.main()
