"""Public toy ELF, compiler boundary and cache isolation contracts."""

import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import nightly
import nightly_probe as probe


class SyntheticElfTests(unittest.TestCase):
    def setUp(self):
        self.data = probe.synthetic_elf()
        self.header = struct.unpack_from('<16sHH5I6H', self.data)

    def test_valid_elf32_mips_headers_and_separate_tiny_loads(self):
        ident, kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, strings = self.header
        self.assertEqual(ident[:7], b'\x7fELF\x01\x01\x01')
        self.assertEqual((kind, machine, version, entry, ehsize, phsize, phnum, shsize, shnum, strings),
                         (2, 8, 1, probe.ENTRY, 52, 32, 21, 40, 23, 22))
        self.assertLess(len(self.data), 4096)
        self.assertEqual(len(self.data), shoff + shnum * shsize)
        addresses = set()
        spans = []
        for index in range(phnum):
            ptype, offset, address, physical, size, memory, permission, align = struct.unpack_from(
                '<8I', self.data, phoff + index * phsize)
            self.assertEqual((ptype, physical, memory, permission, align), (1, address, size, 5, 4))
            self.assertEqual((address % 4, offset % 4, size % 4), (0, 0, 0))
            self.assertLessEqual(offset + size, shoff)
            addresses.add(address)
            spans.append((offset, offset + size))
            section = struct.unpack_from('<10I', self.data, shoff + (index + 1) * shsize)
            self.assertEqual(section[1:6], (1, 6, address, offset, size))
        self.assertEqual(addresses, {probe.ENTRY, probe.DISPATCH, probe.TERMINAL, *probe.HOOKS})
        self.assertEqual(len(addresses), 21)
        for before, after in zip(spans, spans[1:]):
            self.assertLessEqual(before[1], after[0])

    def test_standard_instructions_make_all_profile_hooks_discoverable(self):
        _, _, _, _, _, phoff, _, _, _, phsize, phnum, *_ = self.header
        calls = {}
        for index in range(phnum):
            _, offset, address, _, size, *_ = struct.unpack_from('<8I', self.data, phoff + index * phsize)
            words = struct.unpack_from('<' + 'I' * (size // 4), self.data, offset)
            self.assertEqual(words[-2:], (0x03E00008, 0))
            for instruction, delay in zip(words[:-2:2], words[1:-2:2]):
                self.assertEqual(instruction >> 26, 3)  # Standard MIPS JAL.
                self.assertEqual(delay, 0)
                target = ((address + 4) & 0xF0000000) | ((instruction & 0x03FFFFFF) << 2)
                calls.setdefault(address, set()).add(target)
        self.assertEqual(calls[probe.ENTRY], {probe.DISPATCH})
        self.assertEqual(calls[probe.DISPATCH], {*probe.HOOKS, probe.TERMINAL})
        cmake = (nightly.ROOT / 'profiles/mhp3rd/CMakeLists.txt').read_text()
        for name, expected in (('MHP3RD_HUD_FUNCTIONS', probe.HUD), ('MHP3RD_LAYERED_FUNCTIONS', probe.LAYERED)):
            values = re.search(r'set\(' + name + r'\s+([^)]*)\)', cmake)
            self.assertIsNotNone(values)
            self.assertEqual({int(value, 16) for value in values[1].split()}, set(expected))
        self.assertIn(f'0x{probe.CAMERA:08X}', cmake)

    def test_disc_is_explicit_small_sentinel_without_original_assets(self):
        self.assertLess(len(probe.DISC_SENTINEL), 256)
        self.assertIn(b'Not a disc image', probe.DISC_SENTINEL)
        self.assertNotIn(b'CD001', probe.DISC_SENTINEL)
        self.assertEqual(self.data, probe.synthetic_elf())


class ProbeCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.output = self.base / 'probe'
        self.calls = []
        data = bytearray(64)
        data[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', data, 16, 3, 62)
        self.native = bytes(data)

    def compiler(self, command, **options):
        self.calls.append((command, options))
        self.assertEqual(options, {'check': True, 'timeout': 180})
        if '--build' in command:
            target = Path(command[command.index('--build') + 1]) / 'bin/probe_overlay.so'
            target.parent.mkdir(parents=True)
            target.write_bytes(self.native)

    def test_native_compiler_boundary_and_checksummed_355_module_cache(self):
        output = probe.create_probe(self.output, 'linux-x86_64', runner=self.compiler)
        self.assertEqual(output, self.output.absolute())
        modules = list((output / 'overlays').iterdir())
        self.assertEqual(len(modules), 355)
        self.assertEqual(len({p.name for p in modules}), 355)
        self.assertTrue(all(p.read_bytes() == self.native and not p.is_symlink() for p in modules))
        marker = json.loads((output / 'overlay-files.json').read_text())
        self.assertEqual(set(marker), {'schema', 'platform', 'abi_sha256', 'toolchain', 'files'})
        self.assertEqual(marker['platform'], 'linux-x86_64')
        self.assertEqual(marker['toolchain'], nightly.TOOLCHAINS['linux-x86_64'])
        self.assertEqual(marker['abi_sha256'], nightly.abi_fingerprint())
        self.assertEqual(set(marker['files']), {p.name for p in modules})
        self.assertEqual(set(marker['files'].values()), {hashlib.sha256(self.native).hexdigest()})
        nightly.verify_overlays(output)
        self.assertEqual((output / 'EBOOT.ELF').read_bytes(), probe.synthetic_elf())
        self.assertEqual((output / 'disc.iso').read_bytes(), probe.DISC_SENTINEL)
        configure, build = [entry[0] for entry in self.calls]
        self.assertIn('-G', configure)
        self.assertIn('Ninja', configure)
        self.assertIn('-DCMAKE_BUILD_TYPE=Release', configure)
        self.assertEqual(build[-3:], ['--target', 'probe_overlay', '-j2'])
        source = (output / 'probe-build/source/probe.cpp').read_text()
        self.assertIn('extern "C"', source)
        self.assertIn('__declspec(dllexport)', source)
        modules[0].write_bytes(self.native + b'modified')
        with self.assertRaises(nightly.Rejected):
            nightly.verify_overlays(output)

    def test_existing_output_and_symlink_are_preserved(self):
        self.output.mkdir()
        sentinel = self.output / 'keep'
        sentinel.write_bytes(b'preserve me')
        with self.assertRaises(nightly.Rejected):
            probe.create_probe(self.output, 'linux-x86_64', runner=self.compiler)
        self.assertEqual(sentinel.read_bytes(), b'preserve me')
        link = self.base / 'link'
        link.symlink_to(self.output, target_is_directory=True)
        with self.assertRaises(nightly.Rejected):
            probe.create_probe(link, 'linux-x86_64', runner=self.compiler)
        self.assertEqual(sentinel.read_bytes(), b'preserve me')
        self.assertEqual(self.calls, [])

    def test_failed_or_wrong_architecture_build_removes_only_owned_output(self):
        sentinel = self.base / 'outside'
        sentinel.write_bytes(b'keep')
        for failure in ('configure', 'wrong_binary', 'timeout'):
            def compiler(command, **options):
                if failure == 'configure':
                    raise subprocess.CalledProcessError(1, command)
                if failure == 'timeout':
                    raise subprocess.TimeoutExpired(command, 180)
                self.compiler(command, **options)
                if '--build' in command:
                    (Path(command[command.index('--build') + 1]) / 'bin/probe_overlay.so').write_bytes(b'not a native library')
            with self.subTest(failure=failure), self.assertRaises(Exception):
                probe.create_probe(self.output, 'linux-x86_64', runner=compiler)
            self.assertFalse(self.output.exists())
            self.assertEqual(sentinel.read_bytes(), b'keep')

    def test_platform_toolchain_is_fixed_and_missing_ndk_fails_early(self):
        self.assertEqual(probe.toolchain_arguments('linux-x86_64'), [])
        self.assertEqual(probe.toolchain_arguments('windows-x86_64'), [])
        self.assertEqual(probe.toolchain_arguments('macos-arm64'),
                         ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0'])
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(nightly.Rejected):
                probe.create_probe(self.output, 'android-arm64', runner=self.compiler)
        self.assertFalse(self.output.exists())
        with mock.patch.object(probe, 'ANDROID_SDK_ROOTS', (self.base,)), \
                mock.patch.dict(os.environ, {'ANDROID_HOME': str(self.base)}):
            with self.assertRaises(nightly.Rejected):
                probe.toolchain_arguments('android-arm64')
            toolchain = self.base / 'ndk' / probe.NDK_VERSION / 'build/cmake/android.toolchain.cmake'
            toolchain.parent.mkdir(parents=True)
            toolchain.write_text('# public synthetic toolchain placeholder\n')
            options = probe.toolchain_arguments('android-arm64')
            self.assertIn('-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), options)
            self.assertIn('-DANDROID_ABI=arm64-v8a', options)
            self.assertIn('-DANDROID_PLATFORM=android-29', options)
            self.assertIn('-DANDROID_STL=c++_static', options)

    def test_android_sdk_environment_only_selects_exact_approved_roots(self):
        unknown = ['../Android/Sdk', str(self.base) + '/.', str(self.base) + '/',
                   str(self.base) + '/../' + self.base.name, str(self.base / 'unknown'),
                   'https://example.invalid/sdk', str(self.base) + '\nuntrusted']
        with mock.patch.object(probe, 'ANDROID_SDK_ROOTS', (self.base,)):
            for value in unknown:
                with self.subTest(value=value), mock.patch.dict(os.environ, {'ANDROID_HOME': value}), \
                        mock.patch.object(Path, 'is_file') as inspect_file, \
                        mock.patch.object(Path, 'is_symlink') as inspect_link:
                    with self.assertRaises(nightly.Rejected):
                        probe.toolchain_arguments('android-arm64')
                    inspect_file.assert_not_called()
                    inspect_link.assert_not_called()

    @unittest.skipUnless(os.name == 'posix', 'POSIX SDK symlink ownership contract')
    def test_android_sdk_and_nested_ndk_aliases_are_rejected(self):
        actual = self.base / 'actual-sdk'
        toolchain = actual / 'ndk' / probe.NDK_VERSION / 'build/cmake/android.toolchain.cmake'
        toolchain.parent.mkdir(parents=True)
        toolchain.write_text('# public synthetic toolchain\n')
        sdk = self.base / 'approved-sdk'
        sdk.symlink_to(actual, target_is_directory=True)
        with mock.patch.object(probe, 'ANDROID_SDK_ROOTS', (sdk,)), \
                mock.patch.dict(os.environ, {'ANDROID_HOME': str(sdk)}), self.assertRaises(nightly.Rejected):
            probe.toolchain_arguments('android-arm64')
        sdk.unlink();sdk.mkdir()
        (sdk / 'ndk').symlink_to(actual / 'ndk', target_is_directory=True)
        with mock.patch.object(probe, 'ANDROID_SDK_ROOTS', (sdk,)), \
                mock.patch.dict(os.environ, {'ANDROID_HOME': str(sdk)}), self.assertRaises(nightly.Rejected):
            probe.toolchain_arguments('android-arm64')
        (sdk / 'ndk').unlink()
        nested = sdk / 'ndk' / probe.NDK_VERSION / 'build/cmake/android.toolchain.cmake'
        nested.parent.mkdir(parents=True)
        nested.symlink_to(toolchain)
        with mock.patch.object(probe, 'ANDROID_SDK_ROOTS', (sdk,)), \
                mock.patch.dict(os.environ, {'ANDROID_HOME': str(sdk)}), self.assertRaises(nightly.Rejected):
            probe.toolchain_arguments('android-arm64')

    def test_unknown_platform_cannot_create_output(self):
        with self.assertRaises(nightly.Rejected):
            probe.create_probe(self.output, 'unknown', runner=self.compiler)
        self.assertFalse(self.output.exists())
        self.assertEqual(self.calls, [])

    def test_installation_is_an_explicit_cli_action(self):
        with mock.patch.object(probe, 'create_probe', return_value=self.output) as create:
            with mock.patch.object(nightly, 'install_inputs', create=True) as install:
                self.assertEqual(probe.main(['--platform', 'macos-arm64', '--output', str(self.output)]), 0)
                install.assert_not_called()
                self.assertEqual(probe.main(['--platform', 'macos-arm64', '--output', str(self.output), '--install']), 0)
                install.assert_called_once_with(self.output)
            self.assertEqual(create.call_count, 2)

    def test_cli_failure_is_bounded_and_does_not_print_paths(self):
        self.output.mkdir()
        result = subprocess.run([sys.executable, str(Path(probe.__file__)), '--platform', 'linux-x86_64',
                                 '--output', str(self.output)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'The public packaging probe could not be prepared.\n')
        self.assertEqual(result.stdout, '')
        self.assertTrue(self.output.is_dir())


if __name__ == '__main__':
    unittest.main()
