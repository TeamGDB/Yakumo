#!/usr/bin/env python3
"""Public synthetic contracts for the private macOS build orchestration.

Tool doubles exercise failure propagation, platform/configuration requirements
and input ownership. The hosted public probe separately compiles and packages
real native binaries; no game data or cloud credentials are used here.
"""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/ci/nightly_macos.sh'
RELEASE = ROOT / 'profiles/mhp3rd/scripts/release_macos.sh'

# These substitutes never compile or inspect game-derived content. They record
# the external-tool protocol and let the actual shell driver decide when to
# continue, stop, create its input links and reuse its native overlay cache.
DRIVER = r'''
import json, os, pathlib, shutil, sys
command = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as stream:
    stream.write(json.dumps([command, *args]) + '\n')
if command == 'uname':
    print(os.environ.get('TEST_SYSTEM', 'Darwin') if args == ['-s'] else os.environ.get('TEST_ARCH', 'arm64'))
elif command == 'python3':
    sys.exit(int(os.environ.get('VERIFY_FAIL', '0')))
elif command == 'bash':
    if '--prepare-deps' in args:
        for name in ('sources', 'deps-build', 'deps/lib'):
            (pathlib.Path(os.environ['YAKUMO_WORK']) / name).mkdir(parents=True, exist_ok=True)
elif command == 'curl':
    shutil.copyfile(os.environ['ARCHIVE'], args[args.index('-o') + 1])
elif command == 'cmake':
    if '-S' in args and args[args.index('-S') + 1] == os.environ['TEST_REPO']:
        print('-- mhp3rd: Vulkan renderer enabled' if not os.environ.get('NO_RENDERER') else '-- mhp3rd: renderer disabled')
    if '--target' in args and args[args.index('--target') + 1] == 'glslang-standalone':
        executable = pathlib.Path(args[args.index('--build') + 1]) / 'StandAlone/glslang'
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_bytes(b'synthetic shader tool')
elif command == 'xcrun':
    print(os.environ.get('SDK_VERSION', '13.1'))
'''


@unittest.skipUnless(os.name == 'posix' and shutil.which('bash'), 'requires POSIX bash')
class MacosNightlyContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'source'
        self.work = self.base / 'private-work'
        self.inputs = self.base / 'private-inputs'
        self.bin = self.base / 'tools'
        self.bin.mkdir()
        self.inputs.mkdir()
        (self.inputs / 'overlays').mkdir()
        (self.inputs / 'overlays/overlay.dylib').write_bytes(b'synthetic native overlay stand-in')
        for name in ('EBOOT.ELF', 'disc.iso'):
            (self.inputs / name).write_bytes(b'public synthetic input')
        script = self.repo / 'scripts/ci/nightly_macos.sh'
        script.parent.mkdir(parents=True)
        shutil.copyfile(SCRIPT, script)
        self.archive = self.base / 'glslang.tar.gz'
        with tarfile.open(self.archive, 'w:gz') as archive:
            member = tarfile.TarInfo('glslang-test/source.txt')
            member.size = 9
            archive.addfile(member, io.BytesIO(b'synthetic'))
        checksum = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        pins = self.repo / 'profiles/mhp3rd/packaging/macos/sources.sh'
        pins.parent.mkdir(parents=True)
        pins.write_text('MACOS_DEPLOYMENT_TARGET=13.0\nGLSLANG_COMMIT=test\n'
                        'GLSLANG_URL=https://example.invalid/glslang.tar.gz\n'
                        f'GLSLANG_SHA256={checksum}\n')
        self.pins = pins
        self.calls_path = self.base / 'calls.jsonl'
        for command in ('uname', 'python3', 'bash', 'curl', 'cmake', 'xcrun'):
            path = self.bin / command
            path.write_text('#!' + sys.executable + '\n' + DRIVER)
            path.chmod(0o755)
        self.env = {**os.environ, 'PATH': str(self.bin) + os.pathsep + os.environ['PATH'],
                    'CALLS': str(self.calls_path), 'ARCHIVE': str(self.archive),
                    'TEST_REPO': str(self.repo), 'YAKUMO_WORK': str(self.work),
                    'YAKUMO_PRIVATE_INPUTS': str(self.inputs),
                    'NIGHTLY_VERSION': 'nightly-' + 'a' * 12}
        self.bash = shutil.which('bash')

    def run_driver(self, **updates):
        return subprocess.run([self.bash, str(self.repo / 'scripts/ci/nightly_macos.sh')],
                              env={**self.env, **updates}, capture_output=True, text=True, timeout=20)

    def calls(self):
        if not self.calls_path.exists():
            return []
        return [json.loads(line) for line in self.calls_path.read_text().splitlines()]

    def test_reuses_overlays_and_builds_renderer_for_macos13(self):
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        verification = next(call for call in calls if call[0] == 'python3')
        self.assertEqual(verification[1:], ['scripts/ci/nightly.py', 'verify-overlays',
                                          '--platform', 'macos-arm64', '--inputs', str(self.inputs)])
        main = next(call for call in calls if call[:3] == ['cmake', '-S', str(self.repo)])
        for option in ('-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                       '-DMHP3RD_RELEASE=ON', '-DMHP3RD_FFMPEG=bundled', '-DMHP3RD_RENDERER=ON',
                       '-DPSPRECOMP_GENERATED_JOBS=1', '-DPSPRECOMP_CCACHE=OFF'):
            self.assertIn(option, main)
        compiler = next(call for call in calls if call[0] == 'cmake' and '-DENABLE_OPT=OFF' in call)
        self.assertIn('-DBUILD_EXTERNAL=OFF', compiler)
        self.assertIn('-DENABLE_HLSL=OFF', compiler)
        self.assertTrue(any('--prepare-deps' in call for call in calls))
        package = calls[-1]
        self.assertIn('--no-dmg', package)
        self.assertIn('--zip', package)
        self.assertFalse(any('build_overlays.sh' in ' '.join(call) for call in calls))
        builds = [call for call in calls if call[:2] == ['cmake', '--build']]
        self.assertEqual([call[call.index('--target') + 1] for call in builds],
                         ['glslang-standalone', 'Yakumo'])
        self.assertEqual((self.work / 'build/bin/overlays/overlay.dylib').read_bytes(),
                         (self.inputs / 'overlays/overlay.dylib').read_bytes())
        self.assertEqual((self.work / 'deps/bin/glslang').read_bytes(), b'synthetic shader tool')
        for name in ('EBOOT.ELF', 'disc.iso'):
            self.assertEqual((self.repo / 'profiles/mhp3rd/game' / name).readlink(), self.inputs / name)

    def test_missing_renderer_stops_before_codegen_or_packaging(self):
        result = self.run_driver(NO_RENDERER='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Vulkan renderer is required', result.stderr)
        self.assertFalse(any('scripts/generate.sh' in ' '.join(call) for call in self.calls()))
        self.assertFalse(any('--zip' in call for call in self.calls()))

    def test_archive_checksum_failure_prevents_tool_execution(self):
        self.pins.write_text(self.pins.read_text().replace('GLSLANG_SHA256=', 'UNUSED=') +
                             'GLSLANG_SHA256=' + '0' * 64 + '\n')
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('shader compiler checksum mismatch', result.stderr)
        self.assertFalse(any(call[0] == 'cmake' for call in self.calls()))

    def test_overlay_verification_failure_stops_before_input_linking(self):
        result = self.run_driver(VERIFY_FAIL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.repo / 'profiles/mhp3rd/game').exists())
        self.assertFalse(any(call[0] == 'bash' for call in self.calls()))

    def test_rejects_unrelated_prepared_inputs_without_overwriting(self):
        game = self.repo / 'profiles/mhp3rd/game'
        game.mkdir()
        sentinel = game / 'EBOOT.ELF'
        sentinel.write_bytes(b'preserve this developer file')
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b'preserve this developer file')
        self.assertFalse(any(call[0] == 'bash' for call in self.calls()))

    def test_correct_existing_input_links_are_reused(self):
        game = self.repo / 'profiles/mhp3rd/game'
        game.mkdir()
        for name in ('EBOOT.ELF', 'disc.iso'):
            (game / name).symlink_to(self.inputs / name)
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_invalid_dispatch_parameters_and_host(self):
        for update in ({'NIGHTLY_VERSION': 'nightly-../unsafe'}, {'TEST_ARCH': 'x86_64'},
                       {'TEST_SYSTEM': 'Linux'}, {'YAKUMO_WORK': 'relative'},
                       {'YAKUMO_PRIVATE_INPUTS': 'relative'}):
            with self.subTest(update=update):
                self.calls_path.unlink(missing_ok=True)
                result = self.run_driver(**update)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call[0] == 'python3' for call in self.calls()))

    def test_rejects_symlinked_workspace(self):
        real = self.base / 'keep'
        real.mkdir()
        self.work.symlink_to(real, target_is_directory=True)
        self.assertNotEqual(self.run_driver().returncode, 0)
        self.assertEqual(list(real.iterdir()), [])

    def run_sdk_resolver(self, sdk, version='13.1'):
        # Exercise the packager's standalone SDK resolution contract on every
        # host, without requiring macOS command-line tools in Python-only CI.
        text = RELEASE.read_text()
        resolver = text.split('retarget_sdk() {\n', 1)[1].split('\n}\n', 1)[0]
        body = ('set -euo pipefail\ntarget=13.0\n'
                'fail() { echo "$*" >&2; exit 1; }\n'
                'retarget_sdk() {\n' + resolver + '\n}\nretarget_sdk\n')
        return subprocess.run([self.bash, '-c', body], text=True, capture_output=True, timeout=10,
                              env={**self.env, 'YAKUMO_CHECK_SDK': str(sdk), 'SDK_VERSION': version})

    def test_retargeting_requires_real_matching_sdk(self):
        sdk = self.base / 'SDK'
        missing = self.run_sdk_resolver(sdk)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn('requires the macOS 13 SDK', missing.stderr)
        sdk.mkdir()
        wrong = self.run_sdk_resolver(sdk, '15.0')
        self.assertNotEqual(wrong.returncode, 0)
        self.assertIn('not 15.0', wrong.stderr)
        correct = self.run_sdk_resolver(sdk)
        self.assertEqual(correct.returncode, 0, correct.stderr)
        self.assertEqual(correct.stdout.strip(), str(sdk))


if __name__ == '__main__':
    unittest.main()
