"""Public Android orchestration and unsigned APK packaging regressions."""

import base64
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
from unittest import mock
import zipfile

import nightly_android_policy as policy
from nightly_formats import canonical_package
from test_nightly_android_policy import android_tools
from test_nightly_transfer import namespace, sdk_tools

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r'''
import hashlib, json, os, pathlib, sys
name, args = pathlib.Path(sys.argv[0]).name, sys.argv[1:]
with open(os.environ['CALLS'], 'a') as stream:
    stream.write(json.dumps([name, *args]) + '\n')
if name == 'uname':
    print(os.environ.get('MOCK_SYSTEM', 'Linux') if args == ['-s'] else 'x86_64')
elif name == 'python3':
    sys.exit(int(os.environ.get('REJECT_CACHE', '0')))
elif name == 'git':
    print('7')
elif name == 'curl':
    pathlib.Path(args[args.index('-o') + 1]).write_bytes(b'corrupt remote source')
elif name == 'sha256sum':
    checksum, path = sys.stdin.read().strip().split('  ', 1)
    sys.exit(hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() != checksum)
elif name == 'bash':
    os.execv(os.environ['REAL_BASH'], [os.environ['REAL_BASH'], *args])
elif name == 'cmake':
    if '-B' in args:
        build = pathlib.Path(args[args.index('-B') + 1])
        build.mkdir(parents=True, exist_ok=True)
        if '-DMHP3RD_ANDROID_APP=ON' in args:
            if not os.environ.get('NO_RENDERER'): print('mhp3rd: Vulkan renderer enabled')
            if not os.environ.get('NO_AUDIO'): print('mhp3rd: bundled FFmpeg')
        if build.name == 'host-tools':
            for tool in ['psp_analyze', 'psp_recomp']:
                destination = build / tool
                destination.write_text(pathlib.Path(__file__).read_text())
                destination.chmod(0o755)
    if '--build' in args and 'glslang-standalone' in args:
        destination = pathlib.Path(args[args.index('--build') + 1]) / 'StandAlone/glslang'
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(b'public host shader tool')
        destination.chmod(0o755)
    if '--build' in args and 'Yakumo' in args:
        destination = pathlib.Path(args[args.index('--build') + 1]) / 'bin'
        destination.mkdir(parents=True, exist_ok=True)
        (destination / 'libmain.so').write_bytes(b'public synthetic host library')
elif name == 'psp_analyze':
    pathlib.Path(args[1]).write_text('{}')
elif name == 'psp_recomp':
    path = pathlib.Path(args[args.index('--auto') + 1]);path.mkdir(parents=True, exist_ok=True)
    (path / 'unit.cpp').write_text('// synthetic public corpus\n')
'''


@unittest.skipUnless(os.name == 'posix' and shutil.which('bash'), 'POSIX Android build driver')
class AndroidBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='yakumo android driver ')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo, self.work, self.inputs = self.base / 'repo', self.base / 'private work', self.base / 'inputs'
        (self.repo / 'scripts/ci').mkdir(parents=True)
        shutil.copyfile(ROOT / 'scripts/ci/nightly_android.sh', self.repo / 'scripts/ci/nightly_android.sh')
        profile = self.repo / 'profiles/mhp3rd'
        (profile / 'scripts').mkdir(parents=True)
        shutil.copyfile(ROOT / 'profiles/mhp3rd/scripts/generate.sh', profile / 'scripts/generate.sh')
        (profile / 'packaging/android').mkdir(parents=True)
        (profile / 'packaging/android/build_apk.sh').write_text('''#!/usr/bin/env bash
set -euo pipefail
[[ "$NIGHTLY_UNSIGNED" == 1 && "$ANDROID_BUILD_TOOLS_VERSION" == 35.0.0 && "$ANDROID_COMPILE_SDK_VERSION" == 35 ]]
[[ "$(find "$1/bin/overlays" -name 'libovl*.so' | wc -l | tr -d ' ')" == 355 ]]
cp "$1/bin/libmain.so" "$4"
''')
        (profile / 'packaging/macos').mkdir(parents=True)
        (profile / 'cmake').mkdir()
        sources = self.work / 'sources'
        sources.mkdir(parents=True)
        pins = ['GLSLANG_COMMIT=fixture', 'SDL3_VERSION=0.0.0']
        for name, directory, variable in [('glslang-fixture.tar.gz', 'glslang-fixture', 'GLSLANG'),
                                          ('SDL3-0.0.0.tar.gz', 'SDL3-0.0.0', 'SDL3')]:
            path = sources / name
            with tarfile.open(path, 'w:gz') as archive:
                entry = tarfile.TarInfo(directory + '/public-source.txt');entry.size = 6
                archive.addfile(entry, io.BytesIO(b'public'))
            pins += [f'{variable}_SHA256={hashlib.sha256(path.read_bytes()).hexdigest()}',
                     f'{variable}_URL=https://example.invalid/{name}']
        for name, variable in [('NotoSansCJKjp-Regular.otf', 'NOTO_CJK'),
                               ('NotoSansCJK-LICENSE.txt', 'NOTO_CJK_LICENSE')]:
            (sources / name).write_bytes(b'public source fixture')
            pins += [f'{variable}_SHA256={hashlib.sha256((sources / name).read_bytes()).hexdigest()}',
                     f'{variable}_URL=https://example.invalid/{name}']
        (profile / 'packaging/macos/sources.sh').write_text('\n'.join(pins) + '\n')
        ffmpeg = sources / 'ffmpeg-0.0.0.tar.xz'
        ffmpeg.write_bytes(b'public FFmpeg source fixture')
        (profile / 'cmake/FFmpeg.cmake').write_text('set(MHP3RD_FFMPEG_VERSION 0.0.0)\n'
                                                   f'set(MHP3RD_FFMPEG_SHA256 {hashlib.sha256(ffmpeg.read_bytes()).hexdigest()})\n')
        (self.inputs / 'overlays').mkdir(parents=True)
        for name in ['EBOOT.ELF', 'disc.iso']:
            (self.inputs / name).write_bytes(b'public synthetic fixture')
        for index in range(355):
            (self.inputs / f'overlays/libovl{index:04}.so').write_bytes(b'public native fixture ' + str(index).encode())
        self.sdk = self.base / 'sdk'
        for name in ['build-tools/35.0.0/aapt2', 'build-tools/35.0.0/d8', 'build-tools/35.0.0/zipalign',
                     'platforms/android-35/android.jar', 'ndk/28.2.13676358/build/cmake/android.toolchain.cmake']:
            destination = self.sdk / name;destination.parent.mkdir(parents=True, exist_ok=True);destination.touch()
        (self.sdk / 'ndk/28.2.13676358/source.properties').write_text('Pkg.Revision = 28.2.13676358\n')
        tools = self.base / 'tools';tools.mkdir()
        for name in ['uname', 'python3', 'git', 'curl', 'sha256sum', 'bash', 'cmake']:
            path = tools / name;path.write_text('#!' + sys.executable + '\n' + DRIVER);path.chmod(0o755)
        self.calls_path = self.base / 'calls.jsonl'
        self.env = {**os.environ, 'PATH': str(tools) + os.pathsep + os.environ['PATH'],
                    'YAKUMO_PRIVATE_INPUTS': str(self.inputs), 'YAKUMO_WORK': str(self.work),
                    'NIGHTLY_VERSION': 'nightly-0123456789ab', 'ANDROID_HOME': str(self.sdk),
                    'CALLS': str(self.calls_path), 'REAL_BASH': shutil.which('bash')}
        self.env.pop('DEBUGGABLE', None);self.env.pop('DEVICE_TEST_PACKAGE', None)

    def run_driver(self, **updates):
        return subprocess.run([self.env['REAL_BASH'], str(self.repo / 'scripts/ci/nightly_android.sh')],
                              env={**self.env, **updates}, capture_output=True, text=True, timeout=30)

    def calls(self):
        return [json.loads(line) for line in self.calls_path.read_text().splitlines()] if self.calls_path.exists() else []

    def test_native_generators_are_separate_and_only_android_host_is_rebuilt(self):
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertIn(['python3', 'scripts/ci/nightly.py', 'verify-overlays', '--platform', 'android-arm64',
                       '--inputs', str(self.inputs)], calls)
        native = next(call for call in calls if '-DPSPRECOMP_PROFILE=' in call)
        self.assertFalse(any('ANDROID' in flag or 'TOOLCHAIN' in flag for flag in native))
        target = next(call for call in calls if '-DMHP3RD_ANDROID_APP=ON' in call)
        for flag in ['-DANDROID_PLATFORM=android-29', '-DANDROID_ABI=arm64-v8a', '-DMHP3RD_RELEASE=ON',
                     '-DMHP3RD_FFMPEG=bundled', '-DMHP3RD_RENDERER=ON', '-DPSPRECOMP_CCACHE=OFF']:
            self.assertIn(flag, target)
        builds = [call for call in calls if call[:2] == ['cmake', '--build']]
        self.assertEqual([call[call.index('--target') + 1] if '--target' in call else 'SDL3' for call in builds],
                         ['glslang-standalone', 'psp_analyze', 'SDL3', 'Yakumo'])
        self.assertFalse(any('build_overlays.sh' in ' '.join(call) for call in calls))
        self.assertFalse(any('--install' in call and 'glslang-build' in ' '.join(call) for call in calls))
        generate = next(call for call in calls if call[0] == 'bash' and 'profiles/mhp3rd/scripts/generate.sh' in call)
        self.assertEqual(generate[-1], str(self.work / 'host-tools'))
        self.assertEqual(builds[1][2], str(self.work / 'host-tools'))
        self.assertTrue(any(call[0] == 'psp_analyze' for call in calls))
        for file in (self.inputs / 'overlays').iterdir():
            self.assertEqual((self.work / 'build/bin/overlays' / file.name).read_bytes(), file.read_bytes())
        self.assertEqual(sorted(file.name for file in (self.work / 'dist').iterdir()),
                         ['ffmpeg-0.0.0.tar.xz', 'yakumo-nightly-0123456789ab-android-arm64.apk'])

    def test_rejected_cache_wrong_sdk_and_wrong_identity_stop_before_sources(self):
        for updates in [{'REJECT_CACHE': '1'}, {'MOCK_SYSTEM': 'Darwin'}, {'DEBUGGABLE': '1'},
                        {'DEVICE_TEST_PACKAGE': 'other.app'}, {'NIGHTLY_VERSION': '../not-a-version'}]:
            with self.subTest(updates=updates):
                self.calls_path.unlink(missing_ok=True)
                result = self.run_driver(**updates)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call[0] in ('curl', 'cmake') for call in self.calls()))
        (self.sdk / 'ndk/28.2.13676358/source.properties').write_text('Pkg.Revision = 29.0.1\n')
        self.assertNotEqual(self.run_driver().returncode, 0)

    def test_missing_renderer_or_audio_prevents_apk_packaging(self):
        for setting in ['NO_RENDERER', 'NO_AUDIO']:
            with self.subTest(setting=setting):
                self.calls_path.unlink(missing_ok=True)
                result = self.run_driver(**{setting: '1'})
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any('build_apk.sh' in ' '.join(call) for call in self.calls()))

    def test_corrupt_source_and_unrelated_game_inputs_are_preserved(self):
        original = self.work / 'sources/glslang-fixture.tar.gz'
        original.write_bytes(b'corruption')
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(call[0] == 'cmake' for call in self.calls()))
        game = self.repo / 'profiles/mhp3rd/game/EBOOT.ELF'
        game.unlink();game.write_bytes(b'preserve developer data')
        self.assertNotEqual(self.run_driver().returncode, 0)
        self.assertEqual(game.read_bytes(), b'preserve developer data')


SIGN_TOOL = r'''
import json, os, pathlib, shutil, sys
with open(os.environ['SIGN_CALLS'], 'a') as stream:
    stream.write(json.dumps({'tool': pathlib.Path(sys.argv[0]).name, 'args': sys.argv[1:],
                            'password': os.environ.get('YAKUMO_APK_KEY_PASSWORD')}) + '\n')
if pathlib.Path(sys.argv[0]).name == 'keytool':
    sys.exit(99)
args=sys.argv[1:]
shutil.copyfile(args[-1], args[args.index('--out') + 1])
'''


@unittest.skipUnless(os.name == 'posix', 'POSIX APK packaging script')
class AndroidPackagingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tools = android_tools()
        if tools is None or not shutil.which('javac') or not shutil.which('java') or not shutil.which('zip'):
            raise unittest.SkipTest('Android SDK 35 and Java are needed for real public APK packaging')
        cls.aapt, cls.android_jar = tools
        sdk = cls.android_jar.parents[2]
        clang = list((sdk / 'ndk').glob('28.2.13676358/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang'))
        if not clang:
            raise unittest.SkipTest('NDK 28.2.13676358 is needed for the real public AArch64 fixture')
        cls.clang, cls.ndk = clang[0], sdk / 'ndk/28.2.13676358'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='yakumo public apk ')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'repo'
        for name in ['LICENSE', 'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
                     'profiles/mhp3rd/packaging/android/AndroidManifest.xml',
                     'profiles/mhp3rd/packaging/android/build_apk.sh',
                     'profiles/mhp3rd/third_party/imgui/LICENSE.txt', 'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE',
                     'profiles/mhp3rd/third_party/xxhash/LICENSE', 'profiles/mhp3rd/packaging/linux/sources.sh',
                     'third_party/asmjit/LICENSE.md', 'profiles/mhp3rd/third_party/luau/LICENSE.txt',
                     'profiles/mhp3rd/third_party/luau/lua_LICENSE.txt',
                     'profiles/mhp3rd/cmake/FFmpeg.cmake']:
            destination = self.repo / name;destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        android = self.repo / 'profiles/mhp3rd/packaging/android'
        shutil.copytree(ROOT / 'profiles/mhp3rd/packaging/android/res', android / 'res')
        (android / 'java').mkdir()
        (android / 'java/YakumoActivity.java').write_text('package io.github.teamgdb.yakumo; public class YakumoActivity extends android.app.Activity {}')
        self.sdl = self.base / 'sdl'
        java = self.sdl / 'android-project/app/src/main/java/org/libsdl/app';java.mkdir(parents=True)
        (java / 'SDLActivity.java').write_text('package org.libsdl.app; public class SDLActivity extends android.app.Activity {}')
        self.license = b'PUBLIC LICENSE FIXTURE\n'
        (self.sdl / 'LICENSE.txt').write_bytes(self.license)
        self.build = self.base / 'build';(self.build / 'bin/lib').mkdir(parents=True)
        (self.build / 'bin/overlays').mkdir()
        c = self.base / 'public.c';c.write_text('int public_fixture(void) { return 7; }\n')
        so = self.build / 'bin/libmain.so'
        result = subprocess.run([str(self.clang), '-shared', '-nostdlib', '-o', str(so), str(c)], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ['libavcodec.so', 'libavutil.so']:
            shutil.copyfile(so, self.build / 'bin/lib' / name)
        self.sdl_lib = self.base / 'libSDL3.so';shutil.copyfile(so, self.sdl_lib)
        for index in range(355):
            shutil.copyfile(so, self.build / f'bin/overlays/libovl{index:04}.so')
        (self.build / 'bin/lib/FFmpeg-COPYING.LGPLv2.1.txt').write_bytes(self.license)
        (self.build / 'bin/lib/FFmpeg-SOURCE.txt').write_bytes(policy.ffmpeg_notice(self.repo))
        self.font = b'PUBLIC FONT FIXTURE'
        (self.base / 'NotoSansCJKjp-Regular.otf').write_bytes(self.font)
        (self.base / 'NotoSansCJK-LICENSE.txt').write_bytes(self.license)
        (self.repo / 'profiles/mhp3rd/packaging/linux/sources.sh').write_text(
            f'NOTO_CJK_SHA256={hashlib.sha256(self.font).hexdigest()}\n'
            f'NOTO_CJK_LICENSE_SHA256={hashlib.sha256(self.license).hexdigest()}\n')
        for name in ['SDL_SHA', 'FFMPEG_SHA']:
            patch = mock.patch.object(policy, name, hashlib.sha256(self.license).hexdigest())
            patch.start();self.addCleanup(patch.stop)
        self.sdk = self.base / 'sdk'
        (self.sdk / 'build-tools').mkdir(parents=True)
        (self.sdk / 'build-tools' / self.aapt.parent.name).symlink_to(self.aapt.parent, target_is_directory=True)
        (self.sdk / 'platforms').mkdir()
        (self.sdk / 'platforms/android-35').symlink_to(self.android_jar.parent, target_is_directory=True)
        self.tools = self.base / 'tools';self.tools.mkdir()
        for name in ['keytool']:
            path = self.tools / name;path.write_text('#!' + sys.executable + '\n' + SIGN_TOOL);path.chmod(0o755)
        self.sign_log = self.base / 'sign-calls.jsonl'
        self.output = self.base / 'yakumo-nightly-0123456789ab-android-arm64.apk'
        self.env = {**os.environ, 'PATH': str(self.tools) + os.pathsep + os.environ['PATH'],
                    'ANDROID_HOME': str(self.sdk), 'ANDROID_NDK': str(self.ndk),
                    'ANDROID_BUILD_TOOLS_VERSION': self.aapt.parent.name, 'ANDROID_COMPILE_SDK_VERSION': '35',
                    'NIGHTLY_UNSIGNED': '1', 'VERSION_NAME': 'nightly-0123456789ab', 'VERSION_CODE': '7',
                    'FONT_DIR': str(self.base), 'KEYSTORE': str(self.base / 'must-not-be-read.keystore'),
                    'KEYSTORE_PASS': 'PUBLIC-PASSWORD-CANARY', 'KEY_ALIAS': 'public-fixture',
                    'SIGN_CALLS': str(self.sign_log)}
        self.env.pop('DEBUGGABLE', None);self.env.pop('DEVICE_TEST_PACKAGE', None)

    def run_packager(self, **updates):
        return subprocess.run(['bash', str(self.repo / 'profiles/mhp3rd/packaging/android/build_apk.sh'),
                               str(self.build), str(self.sdl), str(self.sdl_lib), str(self.output)],
                              env={**self.env, **updates}, capture_output=True, text=True, timeout=60)

    def test_actual_unsigned_packaging_never_reads_or_creates_a_key(self):
        result = self.run_packager()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.sign_log.exists())
        self.assertFalse((self.build / 'apk-research.keystore').exists())
        check = policy.AndroidPolicy(self.repo, 'nightly-0123456789ab', 'a' * 40)
        with zipfile.ZipFile(self.output) as archive:
            names = set()
            for entry in archive.infolist():
                if not entry.is_dir():
                    check.check(entry.filename, archive.read(entry));names.add(entry.filename)
            check.finish(names)
        alignment = subprocess.run([str(self.aapt.parent / 'zipalign'), '-c', '-P', '16', '4', str(self.output)], capture_output=True, timeout=20)
        self.assertEqual(alignment.returncode, 0, alignment.stderr)
        inspected = self.base / 'inspected.apk'
        canonical_package(self.output, inspected, 'android-arm64', self.repo, 'a' * 40)
        self.assertEqual(inspected.read_bytes(), self.output.read_bytes())

    def test_actual_unsigned_package_survives_protected_signing_byte_for_byte(self):
        result = self.run_packager()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.output) as archive:
            # The official signer removes empty directory records. Keeping them
            # would invalidate the strict unchanged-payload contract even when
            # every regular file is intact. Exercise the real native packager.
            self.assertFalse(any(entry.is_dir() for entry in archive.infolist()))
        tools = sdk_tools()
        self.assertIsNotNone(tools)
        _, _, keytool, java_home = tools
        environment = {**self.env, 'PUBLIC_TEST_PASSWORD': 'public-disposable-fixture-password'}
        if java_home:
            environment['JAVA_HOME'] = java_home

        def run(args):
            return subprocess.run([str(arg) for arg in args], env=environment, check=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120).stdout

        key = self.base / 'public-test.keystore'
        run([keytool, '-genkeypair', '-keystore', key, '-storepass:env', 'PUBLIC_TEST_PASSWORD',
             '-keypass:env', 'PUBLIC_TEST_PASSWORD', '-alias', 'public-fixture', '-keyalg', 'RSA',
             '-keysize', '2048', '-validity', '2', '-dname', 'CN=Public disposable test fixture', '-noprompt'])
        certificate = run([keytool, '-exportcert', '-keystore', key, '-storepass:env', 'PUBLIC_TEST_PASSWORD',
                           '-alias', 'public-fixture'])
        pinned_tools = self.sdk / 'build-tools/35.0.0'
        if not pinned_tools.exists():
            pinned_tools.symlink_to(self.aapt.parent, target_is_directory=True)
        source = '0123456789abcdef0123456789abcdef01234567'
        private = self.base / 'sign-private'
        signing_environment = {**environment, 'GITHUB_SHA': source, 'SIGN_INPUT': str(self.base),
                               'SIGN_PRIVATE': str(private),
                               'ANDROID_RELEASE_KEYSTORE_B64': base64.b64encode(key.read_bytes()).decode(),
                               'ANDROID_RELEASE_KEYSTORE_PASSWORD': environment['PUBLIC_TEST_PASSWORD'],
                               'ANDROID_RELEASE_KEY_ALIAS': 'public-fixture'}
        with mock.patch.dict(os.environ, signing_environment):
            namespace('sign-with-key')['main']()
        signed = private / 'signed.apk'
        verification = run([pinned_tools / 'apksigner', 'verify', '--verbose', '--print-certs',
                            '--min-sdk-version', '24', '--max-sdk-version', '35', signed]).decode()
        validator = namespace('verify-signed')
        validator['CERTIFICATE'] = hashlib.sha256(certificate).hexdigest()
        validator['validate_signed_pair'](self.output, signed, verification)

    def test_debug_identity_and_invalid_sdk_selection_fail_before_key_access(self):
        for updates in [{'DEBUGGABLE': '1'}, {'DEVICE_TEST_PACKAGE': 'other.app'},
                        {'ANDROID_BUILD_TOOLS_VERSION': '../35.0.0'}, {'ANDROID_COMPILE_SDK_VERSION': '../35'},
                        {'NIGHTLY_UNSIGNED': 'yes'}]:
            with self.subTest(updates=updates):
                self.assertNotEqual(self.run_packager(**updates).returncode, 0)
                self.assertFalse(self.sign_log.exists())
                self.assertFalse(self.output.exists())

    def test_real_version_hash_layouts_preserve_bounded_cumulative_alignment(self):
        # The ffff layout reproduced six zero bytes on resources.arsc with real
        # SDK 35.0.1: retained AAPT2 padding plus newly appended zipalign padding.
        # Different version hashes change compressed manifest lengths, so test
        # complete producer/inspector agreement rather than a fixed ZIP offset.
        for revision in ['0' * 40, 'f' * 40]:
            version = 'nightly-' + revision[:12]
            self.output = self.base / f'yakumo-{version}-android-arm64.apk'
            result = self.run_packager(VERSION_NAME=version)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            inspected = self.base / f'inspected-{revision[:12]}.apk'
            canonical_package(self.output, inspected, 'android-arm64', self.repo, revision)
            self.assertEqual(inspected.read_bytes(), self.output.read_bytes())
            self.assertFalse(self.sign_log.exists())

    def test_regular_signer_password_uses_environment_not_command_arguments(self):
        # Replace only the sign command; compile, dex, resource linking, native
        # stripping, archive creation and alignment still execute for real.
        local_tools = self.sdk / 'build-tools/35.0.2'
        local_tools.mkdir()
        for source in self.aapt.parent.iterdir():
            if source.name != 'apksigner':
                (local_tools / source.name).symlink_to(source, target_is_directory=source.is_dir())
        signer = local_tools / 'apksigner'
        signer.write_text('#!' + sys.executable + '\n' + SIGN_TOOL);signer.chmod(0o755)
        key = self.base / 'public-fixture.keystore';key.write_bytes(b'public signing-command fixture')
        result = self.run_packager(NIGHTLY_UNSIGNED='0', KEYSTORE=str(key), ANDROID_BUILD_TOOLS_VERSION='35.0.2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line) for line in self.sign_log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]['tool'], 'apksigner')
        self.assertNotIn('PUBLIC-PASSWORD-CANARY', ' '.join(calls[0]['args']))
        self.assertEqual(calls[0]['password'], 'PUBLIC-PASSWORD-CANARY')
        self.assertIn('env:YAKUMO_APK_KEY_PASSWORD', calls[0]['args'])


if __name__ == '__main__':
    unittest.main()
