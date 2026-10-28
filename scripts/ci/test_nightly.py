"""Synthetic contracts for private inputs and the public package boundary."""

import contextlib
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

import nightly


def checksum(data):
    return hashlib.sha256(data).hexdigest()


def elf(machine=62, kind=3):
    data = bytearray(64)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH', data, 16, kind, machine)
    return bytes(data)


def archive(path, entries, extras=(), padding=b'', gzip_options=None):
    with path.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0,
                                             **(gzip_options or {})) as zipped:
        with tarfile.open(fileobj=zipped, mode='w', format=tarfile.GNU_FORMAT) as packed:
            for name, data in entries.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                packed.addfile(member, io.BytesIO(data))
            for member in extras:
                packed.addfile(member)
        zipped.write(padding)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'repo'
        self.root.mkdir()
        self.source = b'PUBLIC SOURCE FIXTURE'
        self.font = b'OTTO PUBLIC FONT FIXTURE'
        self.license = b'PUBLIC LICENSE FIXTURE\n'
        self.package = self.base / 'yakumo-test-linux-x86_64.tar.gz'
        self.prefix = self.package.name[:-7]
        fixed = {
            'LICENSE': 'PUBLIC PROJECT LICENSE',
            'profiles/mhp3rd/host/adhoc/COPYING': 'PUBLIC AD HOC LICENSE',
            'profiles/mhp3rd/host/adhoc/EXCEPTION': 'PUBLIC AD HOC EXCEPTION',
            'profiles/mhp3rd/packaging/linux/yakumo.sh': '#!/bin/sh\nexec Yakumo\n',
            'profiles/mhp3rd/packaging/linux/README.txt': 'PUBLIC INSTALLATION GUIDE',
            'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md': 'PUBLIC THIRD PARTY NOTICES',
            'profiles/mhp3rd/third_party/imgui/LICENSE.txt': 'PUBLIC IMGUI LICENSE',
            'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE': 'PUBLIC AES LICENSE',
            'profiles/mhp3rd/third_party/xxhash/LICENSE': 'PUBLIC HASH LICENSE',
            'profiles/mhp3rd/packaging/linux/sources.sh':
                'NOTO_CJK_SHA256=' + checksum(self.font) + '\nNOTO_CJK_LICENSE_SHA256='
                + checksum(self.license) + '\n',
            'profiles/mhp3rd/cmake/FFmpeg.cmake':
                'set(MHP3RD_FFMPEG_VERSION 0.0)\nset(MHP3RD_FFMPEG_SHA256 ' + checksum(self.source)
                + ')\nset(MHP3RD_FFMPEG_CONFIGURE_FLAGS\n --enable-shared\n --disable-network)\n',
        }
        for name, text in fixed.items():
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            # These are Linux package inputs, including on a Windows test host.
            target.write_bytes(text.encode('utf-8'))
        for name in ['SDL_LICENSE_SHA', 'FFMPEG_LICENSE_SHA']:
            patch = mock.patch.object(nightly, name, checksum(self.license))
            patch.start()
            self.addCleanup(patch.stop)
        self.source_file = self.base / 'ffmpeg-0.0.tar.xz'
        self.source_file.write_bytes(self.source)
        self.entries = {
            'Yakumo': elf(), 'yakumo': fixed['profiles/mhp3rd/packaging/linux/yakumo.sh'].encode(),
            'README.txt': fixed['profiles/mhp3rd/packaging/linux/README.txt'].encode(),
            'fonts/NotoSansCJKjp-Regular.otf': self.font,
            'lib/libSDL3.so.0': elf(),
            'licenses/Yakumo-LICENSE.txt': fixed['LICENSE'].encode(),
            'licenses/Yakumo-adhoc-GPL-3.0.txt': fixed['profiles/mhp3rd/host/adhoc/COPYING'].encode(),
            'licenses/Yakumo-adhoc-EXCEPTION.txt': fixed['profiles/mhp3rd/host/adhoc/EXCEPTION'].encode(),
            'licenses/THIRD_PARTY_NOTICES.md': fixed['profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md'].encode(),
            'licenses/DearImGui-LICENSE.txt': fixed['profiles/mhp3rd/third_party/imgui/LICENSE.txt'].encode(),
            'licenses/tiny-AES-c-UNLICENSE.txt': fixed['profiles/mhp3rd/third_party/tiny_aes/UNLICENSE'].encode(),
            'licenses/xxHash-LICENSE.txt': fixed['profiles/mhp3rd/third_party/xxhash/LICENSE'].encode(),
            'licenses/SDL3-LICENSE.txt': self.license,
            'licenses/FFmpeg-COPYING.LGPLv2.1.txt': self.license,
            'licenses/NotoSansCJK-OFL.txt': self.license,
            'licenses/FFmpeg-SOURCE.txt': nightly.ffmpeg_notice(self.root),
        }
        self.entries.update({f'overlays/overlay_{i:04d}.so': elf() for i in range(355)})
        self.write_package()

    def write_package(self, changes=None, extras=(), padding=b''):
        values = {**self.entries, **(changes or {})}
        archive(self.package, {self.prefix + '/' + name: data for name, data in values.items()}, extras, padding)

    def inspect(self):
        return nightly.inspect_linux(self.package, self.root)


class PublicationTests(Fixture):
    def test_complete_native_package_is_accepted(self):
        self.inspect()

    def test_stage_exports_exact_files_and_verifiable_checksums(self):
        out = self.base / 'public'
        nightly.stage_public(self.package, self.source_file, out, 'a' * 40, self.root)
        self.assertEqual({p.name for p in out.iterdir()},
                         {self.package.name, self.source_file.name, 'build.json', 'SHA256SUMS', 'release-notes.md'})
        manifest = json.loads((out / 'build.json').read_text())
        self.assertEqual(manifest['source'], 'a' * 40)
        for item in manifest['files']:
            self.assertEqual(nightly.digest(out / item['name']), item['sha256'])
        self.assertIn('Back up your saves', (out / 'release-notes.md').read_text())

    def test_game_source_logs_saves_and_unexpected_files_rejected(self):
        names = ['disc.iso', 'EBOOT.ELF', 'DATA.BIN', 'game/image', 'ms0/PSP/SAVEDATA/save',
                 'generated/unit.cpp', 'cache/object.o', 'game.txt', 'private.log', 'notes.json',
                 'overlays/image.png', 'licenses/unknown.txt', 'original.lang', 'PARAM.SFO',
                 'fonts/game-font.otf']
        for name in names:
            with self.subTest(name=name):
                self.write_package({name: b'SYNTHETIC PRIVATE CONTENT'})
                with self.assertRaises(nightly.Rejected):
                    self.inspect()

    def test_disguised_psp_executable_disc_and_binary_assets_are_rejected(self):
        iso = bytearray(32775)
        iso[32769:32774] = b'CD001'
        payloads = [elf(machine=8), b'\x7fELF\x01\x01' + bytes(64), b'\0PBP' + bytes(64),
                    b'~PSP' + bytes(64), b'\0PSF' + bytes(64), bytes(iso), b'PRIVATE DATA']
        for payload in payloads:
            for name in ['Yakumo', 'lib/libSDL3.so.0', 'overlays/overlay_0000.so']:
                with self.subTest(name=name, size=len(payload)):
                    self.write_package({name: payload})
                    with self.assertRaises(nightly.Rejected):
                        self.inspect()

    def test_assets_cannot_be_smuggled_as_licenses_font_or_launcher(self):
        for name in ['yakumo', 'README.txt', 'fonts/NotoSansCJKjp-Regular.otf',
                     'licenses/SDL3-LICENSE.txt', 'licenses/FFmpeg-SOURCE.txt',
                     'licenses/NotoSansCJK-OFL.txt', 'licenses/FFmpeg-COPYING.LGPLv2.1.txt',
                     'licenses/Yakumo-LICENSE.txt', 'licenses/DearImGui-LICENSE.txt']:
            with self.subTest(name=name):
                self.write_package({name: b'ARBITRARY PRIVATE TEXT'})
                with self.assertRaises(nightly.Rejected):
                    self.inspect()

    def test_paths_links_and_special_members_are_refused(self):
        for name in ['../outside', '/absolute', 'lib/../outside', 'lib\\outside', 'bad\nname', 'lib//file.so']:
            with self.subTest(name=name):
                self.write_package({name: b'private'})
                with self.assertRaises(nightly.Rejected):
                    self.inspect()
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE]:
            info = tarfile.TarInfo(self.prefix + '/lib/extra.so')
            info.type = kind
            info.linkname = '../../outside'
            self.write_package(extras=[info])
            with self.assertRaises(nightly.Rejected):
                self.inspect()

    def test_duplicate_and_extended_metadata_are_refused(self):
        info = tarfile.TarInfo(self.prefix + '/README.txt')
        self.write_package(extras=[info])
        with self.assertRaises(nightly.Rejected):
            self.inspect()
        self.write_package({'x' * 150: b'private'})
        with self.assertRaises(nightly.Rejected):
            self.inspect()

    def test_data_after_tar_end_marker_and_truncated_archives_are_refused(self):
        for padding in [b'PRIVATE TRAILING DATA', b'\0' * 20000]:
            self.write_package(padding=padding)
            with self.assertRaises(nightly.Rejected):
                self.inspect()
        self.write_package()
        self.package.write_bytes(self.package.read_bytes()[:-50])
        with self.assertRaises((nightly.Rejected, EOFError, tarfile.TarError)):
            self.inspect()

    def test_gzip_metadata_and_concatenated_content_are_refused(self):
        data = self.package.read_bytes()
        modified = bytearray(data)
        modified[4] = 1
        self.package.write_bytes(modified)
        with self.assertRaises(nightly.Rejected):
            self.inspect()
        self.package.write_bytes(data + gzip.compress(b'PRIVATE'))
        with self.assertRaises(nightly.Rejected):
            self.inspect()

    def test_limits_and_incomplete_packages_are_refused(self):
        with mock.patch.object(nightly, 'MAX_PACKAGE', 1), self.assertRaises(nightly.Rejected):
            self.inspect()
        with mock.patch.object(nightly, 'MAX_EXPANDED', 1), self.assertRaises(nightly.Rejected):
            self.inspect()
        del self.entries['overlays/overlay_0354.so']
        self.write_package()
        with self.assertRaises(nightly.Rejected):
            self.inspect()

    def test_unexpected_source_and_existing_output_are_preserved(self):
        out = self.base / 'existing'
        out.mkdir()
        (out / 'keep').write_text('user data')
        with self.assertRaises(nightly.Rejected):
            nightly.stage_public(self.package, self.source_file, out, 'a' * 40, self.root)
        self.assertEqual((out / 'keep').read_text(), 'user data')
        self.source_file.write_bytes(b'WRONG SOURCE')
        with self.assertRaises(nightly.Rejected):
            nightly.stage_public(self.package, self.source_file, self.base / 'new', 'a' * 40, self.root)
        self.assertFalse((self.base / 'new').exists())

    def test_copy_failure_removes_only_new_publication_directory(self):
        out = self.base / 'public'
        with mock.patch.object(nightly.shutil, 'copyfile', side_effect=OSError('private detail')):
            with self.assertRaises(OSError):
                nightly.stage_public(self.package, self.source_file, out, 'a' * 40, self.root)
        self.assertFalse(out.exists())
        self.assertTrue(self.package.exists())

    def test_symlinked_input_and_bad_commit_are_refused(self):
        if os.name == 'posix':
            link = self.base / 'link'
            link.symlink_to(self.package)
            with self.assertRaises(nightly.Rejected):
                nightly.regular(link)
        for value in ['', 'main', 'a' * 39, 'a' * 40 + '\n', '../../main']:
            with self.subTest(value=value), self.assertRaises(nightly.Rejected):
                nightly.commit(value)

    def test_cli_errors_do_not_print_private_exception_details(self):
        secret = 'SYNTHETIC-PRIVATE-CANARY'
        stderr = io.StringIO()
        with mock.patch.object(nightly, 'stage_public', side_effect=RuntimeError(secret)), contextlib.redirect_stderr(stderr):
            rc = nightly.main(['stage-public', '--package', str(self.package), '--source-archive',
                               str(self.source_file), '--output', str(self.base / 'public'), '--source-sha', 'a' * 40])
        self.assertEqual(rc, 1)
        self.assertNotIn(secret, stderr.getvalue())
        self.assertNotIn(str(self.base), stderr.getvalue())


class PrivateInputTests(Fixture):
    def setUp(self):
        super().setUp()
        self.game = b'SYNTHETIC EXECUTABLE'
        self.disc = b'SYNTHETIC DISC'
        self.cache = self.package.read_bytes()
        self.manifest = {'schema': 1, 'files': [
            {'name': name, 'size': len(data), 'sha256': checksum(data),
             'parts': [{'key': name + '/part', 'size': len(data), 'sha256': checksum(data)}]}
            for name, data in [('EBOOT.ELF', self.game), ('disc.iso', self.disc)]],
            'overlay_cache': {'filename': self.package.name, 'key': 'cache/package', 'source_commit': 'a' * 40,
                              'abi_sha256': 'b' * 64, 'size': len(self.cache), 'sha256': checksum(self.cache)}}
        self.objects = {'EBOOT.ELF/part': self.game, 'disc.iso/part': self.disc, 'cache/package': self.cache}

    def fetch(self):
        def get(bucket, name, target, limit, expected=None):
            data = json.dumps(self.manifest).encode() if name == 'manifest' else self.objects[name]
            self.assertLessEqual(len(data), limit)
            if expected:
                self.assertEqual(checksum(data), expected)
            Path(target).write_bytes(data)
            return len(data)
        with mock.patch.object(nightly, 'r2_get', side_effect=get), \
                mock.patch.object(nightly, 'abi_fingerprint', return_value='b' * 64), \
                mock.patch.object(nightly, 'EXPECTED_EBOOT', checksum(self.game)):
            nightly.fetch_inputs(self.base / 'private', 'manifest', self.root)

    def test_verified_input_restore_and_native_overlay_reuse(self):
        self.fetch()
        private = self.base / 'private'
        self.assertEqual((private / 'EBOOT.ELF').read_bytes(), self.game)
        self.assertEqual((private / 'disc.iso').read_bytes(), self.disc)
        self.assertEqual(len(list((private / 'overlays').iterdir())), 355)
        if os.name == 'posix':
            self.assertEqual((private / 'disc.iso').stat().st_mode & 0o777, 0o600)

    def test_bad_names_schema_sizes_hashes_and_cache_paths_leave_no_restore(self):
        cases = [lambda: self.manifest.update(schema=2),
                 lambda: self.manifest['files'][0].update(name='../outside'),
                 lambda: self.manifest['files'][0].update(size=0),
                 lambda: self.manifest['files'][0].update(parts=[]),
                 lambda: self.manifest['files'][0]['parts'][0].update(size=0),
                 lambda: self.manifest['files'][0].update(sha256='c' * 64),
                 lambda: self.manifest['overlay_cache'].update(abi_sha256='c' * 64),
                 lambda: self.manifest['overlay_cache'].update(filename='../outside'),
                 lambda: self.manifest['overlay_cache'].update(source_commit='main'),
                 lambda: self.manifest['overlay_cache'].update(size=0)]
        original = json.dumps(self.manifest)
        for change in cases:
            self.manifest = json.loads(original)
            change()
            with self.subTest(change=cases.index(change)), self.assertRaises(nightly.Rejected):
                self.fetch()
            self.assertFalse((self.base / 'private').exists())

    def test_untrusted_object_keys_and_checksums_are_rejected(self):
        for value in ['../data', '/absolute', 'a//b', 'a/./b', 'a/../b', 'https://host/object',
                      'a?token=x', '--flag', 'file\nname', 'a' * 513, 5]:
            with self.subTest(value=value), self.assertRaises(nightly.Rejected):
                nightly.key(value)
        self.assertEqual(nightly.key('valid/part-01.bin'), 'valid/part-01.bin')
        for value in ['', 'x' * 64, 'a' * 63, None]:
            with self.assertRaises(nightly.Rejected):
                nightly.sha(value)

    def test_overlay_extraction_refuses_missing_duplicate_and_non_native_files(self):
        for change in ['missing', 'mips', 'duplicate']:
            with self.subTest(change=change):
                values = {self.prefix + '/' + k: v for k, v in self.entries.items()}
                extras = []
                if change == 'missing':del values[self.prefix + '/overlays/overlay_0000.so']
                if change == 'mips':values[self.prefix + '/overlays/overlay_0000.so'] = elf(8)
                if change == 'duplicate':extras = [tarfile.TarInfo(self.prefix + '/overlays/overlay_0000.so')]
                archive(self.package, values, extras)
                out = self.base / ('overlays-' + change)
                out.mkdir()
                with self.assertRaises(nightly.Rejected):
                    nightly.extract_overlays(self.package, out)

    @unittest.skipUnless(os.name in ('posix', 'nt'), 'Private build process contract')
    def test_raw_subprocess_logs_are_private_and_not_forwarded(self):
        out = self.base / 'private.log'
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            nightly.private_run(out, [sys.executable, '-c', 'print("PRIVATE CANARY")'], 10)
        self.assertIn('PRIVATE CANARY', out.read_text())
        self.assertEqual(stdout.getvalue() + stderr.getvalue(), '')
        if os.name == 'posix':
            self.assertEqual(out.stat().st_mode & 0o777, 0o600)
        with self.assertRaises(nightly.Rejected):
            nightly.private_run(self.base / 'failed.log', [sys.executable, '-c', 'raise RuntimeError("PRIVATE")'], 10)

    @unittest.skipUnless(os.name in ('posix', 'nt'), 'Private build process contract')
    def test_timeout_stops_owned_private_command(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            nightly.private_run(self.base / 'timeout.log', [sys.executable, '-c', 'import time; time.sleep(60)'], 0.05)


class AbiTests(unittest.TestCase):
    def test_crlf_checkout_matches_frozen_lf_source_and_real_changes_still_invalidate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            subprocess.run(['git', '-C', str(root), 'config', 'core.autocrlf', 'true'], check=True)
            subprocess.run(['git', '-C', str(root), 'config', 'core.safecrlf', 'false'], check=True)
            header = root / 'include/psprecomp/abi.hpp'
            header.parent.mkdir(parents=True)
            content = b'#pragma once\nstruct PublicAbi { int value; };\n'
            header.write_bytes(content)
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Synthetic Test', '-c',
                            'user.email=test@example.invalid', 'commit', '-qm', 'Synthetic LF fixture'], check=True)
            source = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
            expected = nightly.abi_fingerprint(root, source)
            self.assertEqual(nightly.abi_fingerprint(root), expected)
            header.unlink()
            subprocess.run(['git', '-C', str(root), 'checkout', '--', 'include/psprecomp/abi.hpp'], check=True)
            self.assertEqual(header.read_bytes(), content.replace(b'\n', b'\r\n'))
            self.assertEqual(nightly.abi_fingerprint(root), expected)
            header.write_bytes(content.replace(b'int value', b'long value').replace(b'\n', b'\r\n'))
            self.assertNotEqual(nightly.abi_fingerprint(root), expected)
            self.assertEqual(nightly.abi_fingerprint(root, source), expected)
            header.write_bytes(content.replace(b'\n', b'\r\n', 1))
            self.assertEqual(nightly.abi_fingerprint(root), expected)
            (header.parent / 'new.hpp').write_bytes(b'struct AddedAbi {};\r\n')
            self.assertNotEqual(nightly.abi_fingerprint(root), expected)

    def test_source_changes_invalidate_cache_but_unrelated_docs_do_not(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            header = root / 'include/psprecomp/abi.hpp'
            header.parent.mkdir(parents=True)
            header.write_text('public synthetic ABI v1')

            def save():
                subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
                subprocess.run(['git', '-C', str(root), '-c', 'user.name=Synthetic Test', '-c',
                                'user.email=test@example.invalid', 'commit', '-qm', 'Synthetic fixture'], check=True)
            save()
            first = nightly.abi_fingerprint(root)
            frozen = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
            self.assertEqual(first, nightly.abi_fingerprint(root, frozen))
            (root / 'README.md').write_text('public documentation')
            save()
            self.assertEqual(first, nightly.abi_fingerprint(root))
            header.write_text('public synthetic ABI v2')
            self.assertNotEqual(first, nightly.abi_fingerprint(root))
            self.assertEqual(first, nightly.abi_fingerprint(root, frozen))
            save()
            self.assertNotEqual(first, nightly.abi_fingerprint(root))
            previous = nightly.abi_fingerprint(root)
            for relative in ['CMakeLists.txt', 'profiles/mhp3rd/host/overlay_module.hpp',
                             'profiles/mhp3rd/host/overlay_module.cpp.in', 'tools/codegen_main.cpp',
                             'src/decoder.cpp', 'src/elf32.cpp', 'src/program_analysis.cpp',
                             'profiles/mhp3rd/tools/add_overlay.py', 'profiles/mhp3rd/tools/wrap_overlay.py',
                             'profiles/mhp3rd/tools/databin.py', 'profiles/mhp3rd/scripts/build_overlays.sh']:
                dependency = root / relative
                dependency.parent.mkdir(parents=True, exist_ok=True)
                dependency.write_bytes(b'public synthetic dependency')
                current = nightly.abi_fingerprint(root)
                self.assertNotEqual(previous, current, relative)
                save()
                self.assertEqual(current, nightly.abi_fingerprint(root))
                previous = current
            # Host runtime implementation changes resolve through the host and
            # do not require recompiling all overlay modules.
            (root / 'src/runtime.cpp').write_text('public synthetic host implementation')
            save()
            self.assertEqual(previous, nightly.abi_fingerprint(root))
            # So do the host sources, tests and shaders that nearly every change
            # to the profile's CMakeLists.txt adds.
            profile = root / 'profiles/mhp3rd/CMakeLists.txt'
            profile.write_text('public synthetic host sources v1')
            save()
            self.assertEqual(previous, nightly.abi_fingerprint(root))
            profile.write_text('public synthetic host sources v2')
            self.assertEqual(previous, nightly.abi_fingerprint(root))
            self.assertEqual(first, nightly.abi_fingerprint(root, frozen))

    def test_cmake_keeps_the_link_contract_cached_overlays_rely_on(self):
        # The profile's CMakeLists.txt is outside the fingerprint, so a cached
        # overlay would not notice these changing. Each one decides whether it
        # still finds the host's symbols; changing one needs a cache refresh.
        self.assertNotIn('profiles/mhp3rd/CMakeLists.txt', nightly.ABI_PATHS)
        contract = {
            'CMakeLists.txt': ['add_library(psprecomp_core OBJECT'],
            'profiles/mhp3rd/CMakeLists.txt': [
                'add_library(Yakumo SHARED ${MHP3RD_PROGRAM_SOURCES}', 'OUTPUT_NAME main',
                'add_executable(Yakumo ${MHP3RD_PROGRAM_SOURCES})', 'ENABLE_EXPORTS TRUE',
                'WINDOWS_EXPORT_ALL_SYMBOLS TRUE)',
                'set(MHP3RD_RELEASE_LINK_OPTIONS -static-libstdc++ "LINKER:--exclude-libs,libstdc++.a:libsupc++.a")',
                'add_library(${target} MODULE ${sources} "${entry_point}")',
                'target_link_libraries(${target} PRIVATE Yakumo)',
                'target_link_options(${target} PRIVATE "LINKER:-z,undefs")',
                'target_compile_definitions(${target} PRIVATE PSPRECOMP_IMPORT_HOST_SYMBOLS=1)',
                'target_link_options(${target} PRIVATE ${MHP3RD_RELEASE_LINK_OPTIONS})',
                'CXX_VISIBILITY_PRESET hidden'],
        }
        for relative, lines in contract.items():
            present = [line.strip() for line in (nightly.ROOT / relative).read_text().splitlines()]
            for line in lines:
                self.assertTrue(any(x.startswith(line) for x in present), f'{relative}: {line}')


class DownloadTests(unittest.TestCase):
    def test_bucket_scoped_sdk_configuration_and_invalid_credentials(self):
        sdk, config = mock.Mock(), mock.Mock()
        modules = {'boto3': sdk, 'botocore': mock.Mock(),
                   'botocore.config': mock.Mock(Config=config)}
        env = {'R2_ACCOUNT_ID': 'a' * 32, 'R2_ACCESS_KEY_ID': 'b' * 32, 'R2_SECRET_ACCESS_KEY': 'c' * 64}
        with mock.patch.dict(sys.modules, modules), mock.patch.dict(os.environ, env, clear=True):
            self.assertIs(nightly.r2_client(), sdk.client.return_value)
            kwargs = sdk.client.call_args.kwargs
            self.assertEqual(kwargs['endpoint_url'], 'https://' + 'a' * 32 + '.r2.cloudflarestorage.com')
            self.assertEqual(kwargs['aws_access_key_id'], 'b' * 32)
            self.assertEqual(kwargs['aws_secret_access_key'], 'c' * 64)
            self.assertEqual(kwargs['region_name'], 'auto')
            self.assertEqual(config.call_args.kwargs['s3'], {'addressing_style': 'path'})
            for name in env:
                with mock.patch.dict(os.environ, {name: 'https://untrusted.invalid'}), self.assertRaises(nightly.Rejected):
                    nightly.r2_client()

    def test_streaming_download_enforces_size_hash_and_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'private'
            content = b'PUBLIC SYNTHETIC CANARY'
            client = mock.Mock()
            client.get_object.return_value = {'Body': io.BytesIO(content), 'ContentLength': len(content)}
            with mock.patch.object(nightly, 'r2_client', return_value=client):
                self.assertEqual(nightly.r2_get(nightly.INPUT_BUCKET, 'safe/key', path,
                                              len(content), checksum(content)), len(content))
            self.assertEqual(path.read_bytes(), content)
            if os.name == 'posix':
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            client.get_object.assert_called_once_with(Bucket=nightly.INPUT_BUCKET, Key='safe/key')

    def test_download_failure_modes_do_not_accept_partial_or_unbounded_data(self):
        cases = [(11, b'123', 10, None), (3, b'12345678901', 10, None),
                 (4, b'123', 10, None), (3, b'123', 10, 'a' * 64),
                 ('3', b'123', 10, None), (3, b'123', 0, None),
                 (3, b'123', 10, 'bad-checksum')]
        for length, data, limit, expected in cases:
            with self.subTest(length=length, limit=limit, data=data), tempfile.TemporaryDirectory() as directory:
                body = io.BytesIO(data)
                client = mock.Mock()
                client.get_object.return_value = {'Body': body, 'ContentLength': length}
                with mock.patch.object(nightly, 'r2_client', return_value=client), self.assertRaises(nightly.Rejected):
                    nightly.r2_get(nightly.CACHE_BUCKET, 'safe/key', Path(directory) / 'data', limit, expected)
                if client.get_object.called:
                    self.assertTrue(body.closed)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'existing'
            target.write_bytes(b'preserve user data')
            for bucket, name in [(nightly.INPUT_BUCKET, 'safe/key'), ('other-bucket', 'safe/key'),
                                 (nightly.INPUT_BUCKET, '../outside')]:
                with self.assertRaises(nightly.Rejected):
                    nightly.r2_get(bucket, name, target, 10)
            self.assertEqual(target.read_bytes(), b'preserve user data')


class OverlayReuseTests(Fixture):
    def test_verified_overlay_directory_can_be_reused_and_mutations_fail(self):
        inputs = self.base / 'inputs'
        inputs.mkdir()
        overlays = inputs / 'overlays'
        overlays.mkdir()
        nightly.extract_overlays(self.package, overlays)
        marker = inputs / 'overlay-abi.sha256'
        marker.write_text('a' * 64 + '\n')
        with mock.patch.object(nightly, 'abi_fingerprint', return_value='a' * 64):
            nightly.write_overlay_marker(inputs, 'linux-x86_64', self.root)
        with mock.patch.object(nightly, 'abi_fingerprint', return_value='a' * 64):
            nightly.verify_overlays(inputs)
            marker.write_text('b' * 64)
            with self.assertRaises(nightly.Rejected):
                nightly.verify_overlays(inputs)
            marker.write_text('a' * 64)
            target = overlays / 'overlay_0000.so'
            target.write_bytes(elf(8))
            with self.assertRaises(nightly.Rejected):
                nightly.verify_overlays(inputs)
            target.write_bytes(elf())
            target.rename(overlays / 'unexpected.txt')
            with self.assertRaises(nightly.Rejected):
                nightly.verify_overlays(inputs)
            (overlays / 'unexpected.txt').unlink()
            with self.assertRaises(nightly.Rejected):
                nightly.verify_overlays(inputs)
            if os.name == 'posix':
                target.symlink_to(self.package)
                with self.assertRaises(nightly.Rejected):
                    nightly.verify_overlays(inputs)
                target.unlink()
                target.write_bytes(elf())
                overlays.rename(inputs / 'renamed')
                overlays.symlink_to(inputs / 'renamed', target_is_directory=True)
                with self.assertRaises(nightly.Rejected):
                    nightly.verify_overlays(inputs)


class CommandTests(unittest.TestCase):
    def test_successful_cli_dispatches_and_constant_failure_output(self):
        cases = [(['abi-fingerprint'], 'abi_fingerprint'),
                 (['fetch-inputs', '--output', 'private'], 'fetch_inputs'),
                 (['verify-overlays', '--inputs', 'private'], 'verify_overlays'),
                 (['stage-public', '--package', 'package', '--source-archive', 'source',
                   '--output', 'public', '--source-sha', 'a' * 40], 'stage_public')]
        for argv, name in cases:
            with self.subTest(command=name), mock.patch.object(nightly, name, return_value='a' * 64) as call:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(nightly.main(argv), 0)
                call.assert_called_once()
        for argv, name in cases:
            with mock.patch.object(nightly, name, side_effect=RuntimeError('PRIVATE CANARY')):
                with contextlib.redirect_stderr(io.StringIO()) as error:
                    self.assertEqual(nightly.main(argv), 1)
                self.assertEqual(error.getvalue(),
                                 'Nightly operation failed closed. No private diagnostic output was published.\n')

    def test_generic_command_execution_is_not_a_cli_operation(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            nightly.main(['private-run', '--log', 'unused', '--', 'arbitrary-program'])
        self.assertEqual(error.exception.code, 2)



if __name__ == '__main__':
    unittest.main()
