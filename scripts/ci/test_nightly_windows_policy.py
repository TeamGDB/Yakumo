"""Synthetic PE/package contracts; fixtures contain no game-derived data."""

import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import nightly_windows_policy as windows
from nightly_formats import Rejected

ROOT = Path(__file__).resolve().parents[2]
SOURCE = '0123456789abcdef0123456789abcdef01234567'
VERSION = 'nightly-0123456789ab'
FFMPEG = 'ffmpeg-n7.1.5-fixture-win64-lgpl-shared-7.1.zip'


def pe(library=True, certificate=False):
    """One loadable .text section, optionally a public test PKCS#7 envelope."""
    data = bytearray(1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 132, 0x8664, 1, 0, 0, 0, 240,
                     0x22 | (0x2000 if library else 0))
    optional = 152
    struct.pack_into('<H', data, optional, 0x020b)
    struct.pack_into('<I', data, optional + 16, 4096)
    struct.pack_into('<II', data, optional + 32, 4096, 512)
    struct.pack_into('<II', data, optional + 56, 8192, 512)
    struct.pack_into('<I', data, optional + 108, 16)
    data[392:400] = b'.text\0\0\0'
    struct.pack_into('<IIII', data, 400, 1, 4096, 512, 512)
    struct.pack_into('<I', data, 428, 0x60000020)
    data[512] = 0xc3  # RET, not a translated game instruction.
    if certificate:
        # ContentInfo { signedData OID, [0] {} }, for bounded-envelope tests.
        payload = bytes.fromhex('300d06092a864886f70d010702a000')
        cert = struct.pack('<IHH', 24, 0x0200, 0x0002) + payload + b'\0'
        struct.pack_into('<II', data, optional + 112 + 32, len(data), len(cert))
        data.extend(cert)
    return bytes(data)


def altered(data, offset, fmt, value):
    result = bytearray(data)
    struct.pack_into(fmt, result, offset, value)
    return bytes(result)


class WindowsPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.licenses = {
            'licenses/Yakumo-LICENSE.txt': ('LICENSE', b'Public MIT fixture\n'),
            'licenses/THIRD_PARTY_NOTICES.md': ('profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md', b'Public notices fixture\n'),
            'licenses/DearImGui-LICENSE.txt': ('profiles/mhp3rd/third_party/imgui/LICENSE.txt', b'Public ImGui fixture\n'),
            'licenses/tiny-AES-c-UNLICENSE.txt': ('profiles/mhp3rd/third_party/tiny_aes/UNLICENSE', b'Public AES fixture\n'),
            'licenses/xxHash-LICENSE.txt': ('profiles/mhp3rd/third_party/xxhash/LICENSE', b'Public xxHash fixture\n'),
        }
        for path, content in self.licenses.values():
            self.write(path, content)
        self.write('profiles/mhp3rd/cmake/FFmpeg.cmake',
                   ('set(MHP3RD_FFMPEG_VERSION 7.1.5)\nset(MHP3RD_FFMPEG_WINDOWS_URL\n'
                    f' "https://github.com/BtbN/FFmpeg-Builds/releases/download/public-fixture/{FFMPEG}")\n').encode())
        self.write('profiles/mhp3rd/packaging/linux/sources.sh', b'SDL3_VERSION=3.4.16\n')
        self.template = (ROOT / 'profiles/mhp3rd/packaging/windows/README-portable.txt.in').read_bytes()
        self.write('profiles/mhp3rd/packaging/windows/README-portable.txt.in', self.template)
        self.sdl_license = b'Public SDL license hashing fixture\n'
        self.ffmpeg_license = b'Public LGPL license hashing fixture\n'
        pins = mock.patch.multiple(windows,
                                   SDL_LICENSE_SHA=hashlib.sha256(self.sdl_license).hexdigest(),
                                   FFMPEG_LICENSE_SHA=hashlib.sha256(self.ffmpeg_license).hexdigest())
        pins.start()
        self.addCleanup(pins.stop)

    def write(self, path, data):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def policy(self):
        return windows.WindowsPolicy(self.root, VERSION, SOURCE)

    def buildinfo(self):
        return (f'Version: v{VERSION}\nCommit: {SOURCE}\nBuilt: 2026-10-18 15:30:59 UTC\n'
                'Platform: Windows x86-64\nConfiguration: Release; MHP3RD_RELEASE=ON\n'
                'Compiler: MSVC 19.44.35229\nSDL3: 3.4.16\n'
                f'FFmpeg: bundled LGPL shared build ({FFMPEG})\nOverlay DLLs: 355\n').encode()

    def source_text(self):
        return (b'The FFmpeg libraries here (avcodec, avutil, swresample) are an unmodified prebuilt\n'
                b'LGPL shared build of FFmpeg 7.1.5 from BtbN/FFmpeg-Builds, licensed under the\n'
                b'GNU LGPL version 3 or later (FFmpeg-LICENSE.txt). They are dynamically linked.\n\n'
                + f'Build:  https://github.com/BtbN/FFmpeg-Builds/releases/download/public-fixture/{FFMPEG}\n'.encode()
                + b'Source: https://github.com/FFmpeg/FFmpeg/tree/release/7.1 (the commit is in the file name)\n'
                b'Build scripts: https://github.com/BtbN/FFmpeg-Builds\n')

    def files(self):
        files = {'Yakumo.exe': pe(False), 'README.txt': self.template.replace(b'@VERSION@', VERSION.encode()),
                 'portable.txt': windows.PORTABLE_MARKER, 'BUILDINFO.txt': self.buildinfo(),
                 'licenses/SDL3-LICENSE.txt': self.sdl_license,
                 'licenses/FFmpeg-LICENSE.txt': self.ffmpeg_license,
                 'licenses/FFmpeg-SOURCE.txt': self.source_text()}
        for name in ['SDL3.dll', 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll',
                     'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']:
            files[name] = pe()
        files.update({name: content for name, (_, content) in self.licenses.items()})
        files.update({f'overlays/ovl{index:08X}_public_{index:016X}.dll': pe() for index in range(355)})
        return files

    def check_all(self, files):
        policy = self.policy()
        for name, data in files.items():
            policy.check(name, data)
        policy.finish(set(files))

    def test_complete_portable_package_and_canonical_line_endings(self):
        # Git may check the public template out as CRLF on Windows. Normalize
        # first so this exercises LF and CRLF rather than creating CRCRLF.
        for newline in [b'\n', b'\r\n']:
            files = self.files()
            for name in ['README.txt', 'BUILDINFO.txt', 'licenses/FFmpeg-SOURCE.txt']:
                files[name] = files[name].replace(b'\r\n', b'\n').replace(b'\n', newline)
            files['msvcp140.dll'] = pe(certificate=True)
            self.check_all(files)

    def test_native_formats_bss_and_optional_dll_entry(self):
        windows.validate_binary(pe())
        windows.validate_binary(pe(False), library=False)
        windows.validate_binary(altered(pe(), 168, '<I', 0))
        bss = bytearray(pe())
        struct.pack_into('<H', bss, 134, 2)
        struct.pack_into('<I', bss, 208, 12288)
        bss[432:440] = b'.bss\0\0\0\0'
        struct.pack_into('<IIII', bss, 440, 4096, 8192, 0, 0)
        windows.validate_binary(bytes(bss))

    def test_renamed_original_or_wrong_native_architecture_rejected(self):
        bad = [b'\x7fELF' + b'\0' * 1020, b'\0PBP' + b'\0' * 1020, b'~PSP' + b'\0' * 1020,
               b'\0PSF' + b'\0' * 1020, b'MZ' + b'\0' * 1022, b'game ISO fixture', b'']
        for offset, fmt, value in [(132, '<H', 0x014c), (132, '<H', 0xaa64), (152, '<H', 0x010b),
                                   (150, '<H', 0x2000), (150, '<H', 0x2122)]:
            bad.append(altered(pe(), offset, fmt, value))
        for data in bad:
            with self.subTest(head=data[:4]), self.assertRaises(Rejected):
                self.policy().check('overlays/ovl_public.dll', data)
        with self.assertRaises(Rejected):
            self.policy().check('Yakumo.exe', pe())
        with self.assertRaises(Rejected):
            self.policy().check('SDL3.dll', pe(False))

    def test_truncated_headers_sections_and_unmapped_payload_rejected(self):
        bad = [pe()[:63], pe()[:511], pe()[:-1], pe() + b'private payload']
        mutations = [(60, '<I', 0), (60, '<I', 127), (60, '<I', 4096), (134, '<H', 0),
                     (134, '<H', 97), (140, '<I', 1), (144, '<I', 1), (148, '<H', 239),
                     (260, '<I', 15), (188, '<I', 513), (184, '<I', 500), (212, '<I', 1024),
                     (208, '<I', 8191), (208, '<I', 2 ** 31), (404, '<I', 0),
                     (408, '<I', 511), (412, '<I', 1024), (412, '<I', 0), (168, '<I', 8000),
                     (296, '<I', 1024)]
        bad.extend(altered(pe(), offset, fmt, value) for offset, fmt, value in mutations)
        for index, data in enumerate(bad):
            with self.subTest(case=index), self.assertRaises(Rejected):
                windows.validate_binary(data)
        with mock.patch.object(windows, 'MAX_NATIVE', 1023), self.assertRaises(Rejected):
            windows.validate_binary(pe())

    def test_overlapping_native_sections_rejected(self):
        data = bytearray(pe())
        struct.pack_into('<H', data, 134, 2)
        data[432:472] = data[392:432]
        with self.assertRaises(Rejected):
            windows.validate_binary(bytes(data))
        struct.pack_into('<I', data, 444, 8192)
        struct.pack_into('<I', data, 208, 12288)
        with self.assertRaises(Rejected):
            windows.validate_binary(bytes(data))

    def test_authenticode_is_bounded_and_cannot_hide_trailing_data(self):
        native = pe(certificate=True)
        windows.validate_binary(native)
        bad = [native + b'hidden payload', native[:-1], altered(native, 296, '<I', 1032),
               altered(native, 300, '<I', 32), altered(native, 1024, '<I', 7),
               altered(native, 1028, '<H', 0x100), altered(native, 1030, '<H', 1),
               altered(native, 1032, '<B', 0x31), altered(native, 1033, '<B', 0x80),
               altered(native, 1033, '<B', 100), altered(native, 1047, '<B', 1)]
        for index, data in enumerate(bad):
            with self.subTest(case=index), self.assertRaises(Rejected):
                windows.validate_binary(data)

    def test_unknown_paths_duplicate_case_aliases_or_missing_members_rejected(self):
        for name in ['EBOOT.ELF', 'logs.txt', 'licenses/private.txt', 'data/slot.bin',
                     'overlays/notes.txt', '../Yakumo.exe', 'OVERLAYS/ovl_public.dll', 'lib/foo.dll']:
            with self.subTest(name=name), self.assertRaises(Rejected):
                self.policy().check(name, pe())
        policy = self.policy()
        policy.check('overlays/ovlABC.dll', pe())
        for alias in ['overlays/ovlABC.dll', 'overlays/ovlabc.dll']:
            with self.assertRaises(Rejected):
                policy.check(alias, pe())
        for missing in ['Yakumo.exe', 'vcruntime140.dll', 'portable.txt', 'licenses/SDL3-LICENSE.txt']:
            files = self.files()
            del files[missing]
            with self.subTest(missing=missing), self.assertRaises(Rejected):
                self.check_all(files)
        files = self.files()
        files.pop('overlays/ovl00000000_public_0000000000000000.dll')
        with self.assertRaises(Rejected):
            self.check_all(files)
        files['overlays/ovl_extra_1.dll'] = pe()
        files['overlays/ovl_extra_2.dll'] = pe()
        with self.assertRaises(Rejected):
            self.check_all(files)
        with self.assertRaises(Rejected):
            self.policy().finish({'Yakumo.exe'})

    def test_metadata_and_license_names_cannot_conceal_arbitrary_text(self):
        files = self.files()
        for name in [*self.licenses, 'licenses/SDL3-LICENSE.txt', 'licenses/FFmpeg-LICENSE.txt',
                     'licenses/FFmpeg-SOURCE.txt', 'README.txt', 'portable.txt']:
            for data in [files[name] + b'private payload', b'plausible UTF-8 text\n', b'\x7fELF']:
                with self.subTest(name=name), self.assertRaises(Rejected):
                    self.policy().check(name, data)
        for data in [self.source_text().replace(b'\n', b'\r', 1),
                     self.source_text().replace(b'\n', b'\r\n', 1),
                     self.source_text() + b'\0', b'x' * 16385]:
            with self.assertRaises(Rejected):
                self.policy().check('licenses/FFmpeg-SOURCE.txt', data)

    def test_buildinfo_is_exact_and_calendar_valid(self):
        original = self.buildinfo()
        for old, new in [(SOURCE.encode(), b'f' * 40), (VERSION.encode(), b'unexpected'),
                         (b'2026-10-18', b'2026-02-30'), (b'15:30:59', b'24:30:59'),
                         (b'19.44.35229', b'19.45.35229'), (b'3.4.16', b'3.4.17'),
                         (b'355\n', b'354\n'), (b'UTC', b'local'), (b'19.44.35229', b'19.44.private')]:
            with self.subTest(value=new), self.assertRaises(Rejected):
                self.policy().check('BUILDINFO.txt', original.replace(old, new))
        for data in [original + b'extra metadata\n', original + b'\xff', original.replace(b'Built:', b'Built:\0')]:
            with self.assertRaises(Rejected):
                self.policy().check('BUILDINFO.txt', data)

    def test_invalid_constructor_or_missing_source_pins_rejected(self):
        for version, source in [(None, SOURCE), (VERSION, None), ('../version', SOURCE),
                                ('release\nmetadata', SOURCE), (VERSION, 'short'), (VERSION, 'G' * 40)]:
            with self.assertRaises(Rejected):
                windows.WindowsPolicy(self.root, version, source)
        for path, data in [('profiles/mhp3rd/packaging/linux/sources.sh', b'no pin\n'),
                           ('profiles/mhp3rd/cmake/FFmpeg.cmake', b'no pin\n')]:
            previous = (self.root / path).read_bytes()
            self.write(path, data)
            with self.assertRaises(Rejected):
                self.policy()
            self.write(path, previous)
        with self.assertRaises(Rejected):
            self.policy().check('BUILDINFO.txt', None)


if __name__ == '__main__':
    unittest.main()
