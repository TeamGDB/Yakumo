"""Android publication contracts use public resources and synthetic binaries."""

import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile
import zlib

import nightly_android_policy as policy
from nightly_formats import Rejected

ROOT = Path(__file__).resolve().parents[2]
VERSION = 'nightly-0123456789ab'


def elf(machine=183):
    data = bytearray(120)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI', data, 16, 3, machine, 1)
    struct.pack_into('<QQ', data, 32, 64, 0)
    struct.pack_into('<6H', data, 52, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, len(data), len(data), 4096)
    return bytes(data)


def dex():
    data = bytearray(140)
    data[:8] = b'dex\n039\0'
    struct.pack_into('<3I', data, 32, len(data), 112, 0x12345678)
    struct.pack_into('<I', data, 52, 112)
    struct.pack_into('<2I', data, 104, 28, 112)
    struct.pack_into('<I', data, 112, 2)
    struct.pack_into('<HHII', data, 116, 0, 0, 1, 0)
    struct.pack_into('<HHII', data, 128, 0x1000, 0, 1, 112)
    data[12:32] = hashlib.sha1(data[32:], usedforsecurity=False).digest()
    struct.pack_into('<I', data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    return bytes(data)


def android_tools():
    candidates = [Path(os.environ[name]) for name in ['ANDROID_HOME', 'ANDROID_SDK_ROOT'] if os.environ.get(name)]
    candidates.append(Path.home() / 'Library/Android/sdk')
    for sdk in candidates:
        android_jar = sdk / 'platforms/android-35/android.jar'
        for version in ['35.0.1', '35.0.0']:
            aapt = sdk / 'build-tools' / version / ('aapt2.exe' if os.name == 'nt' else 'aapt2')
            if aapt.is_file() and android_jar.is_file():
                return aapt, android_jar
    return None


class BinaryContracts(unittest.TestCase):
    def test_real_elf_and_dex_headers_are_required(self):
        policy.validate_binary(elf())
        policy.validate_binary(elf(), library=False)
        policy.validate_dex(dex())
        for data in [elf(8), elf(62), b'~PSP' + bytes(200), b'\0PBP' + bytes(200), b'CD001' + bytes(200), b'game data']:
            with self.subTest(prefix=data[:8]), self.assertRaises(Rejected):
                policy.validate_binary(data)
        for offset, replacement in [(4, b'\x01'), (16, b'\x02\0'), (20, bytes(4)),
                                    (32, struct.pack('<Q', 100000)), (52, bytes(2)),
                                    (72, struct.pack('<Q', 100000)), (96, struct.pack('<Q', 100000))]:
            broken = bytearray(elf())
            broken[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaises(Rejected):
                policy.validate_binary(broken)
        for data in [b'', b'\x7fELF' + bytes(200), dex() + b'PRIVATE DATA', dex()[:100]]:
            with self.assertRaises(Rejected):
                policy.validate_dex(data)
        for offset in [8, 12, 36, 40, 44, 52, 60, 104, 108]:
            broken = bytearray(dex())
            broken[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(Rejected):
                policy.validate_dex(broken)

    def test_chunks_and_xml_refuse_unbounded_or_unknown_layouts(self):
        for data in [b'', bytes(8), struct.pack('<HHI', 3, 8, 8), struct.pack('<HHI', 3, 8, 1000)]:
            with self.assertRaises((Rejected, struct.error)):
                policy.binary_xml(data)
        for data in [bytes(4), struct.pack('<HHI', 1, 8, 7), struct.pack('<HHI', 1, 8, 12),
                     struct.pack('<HHI', 1, 8, 9) + bytes(1)]:
            with self.assertRaises(Rejected):
                list(policy.chunks(data))


class AndroidPackageContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tools = android_tools()
        if tools is None:
            raise unittest.SkipTest('Android SDK 35 resource compiler is unavailable; binary contracts still run')
        cls.temp = tempfile.TemporaryDirectory(prefix='yakumo android public resources ')
        cls.addClassCleanup(cls.temp.cleanup)
        base = Path(cls.temp.name)
        archive, package = base / 'resources.zip', base / 'resources.apk'
        aapt, android_jar = tools
        resource_root = ROOT / 'profiles/mhp3rd/packaging/android'
        commands = [[str(aapt), 'compile', '--no-crunch', '--dir', str(resource_root / 'res'), '-o', str(archive)],
                    [str(aapt), 'link', '-I', str(android_jar), '--manifest', str(resource_root / 'AndroidManifest.xml'),
                     '--min-sdk-version', '29', '--target-sdk-version', '35', '--version-name', VERSION,
                     '--version-code', '7', '-o', str(package), str(archive)]]
        for command in commands:
            result = subprocess.run(command, capture_output=True, timeout=60)
            if result.returncode:
                raise RuntimeError('Public Android resource compilation failed: ' + result.stderr.decode(errors='replace'))
        with zipfile.ZipFile(package) as packed:
            cls.resources = {entry.filename: packed.read(entry) for entry in packed.infolist() if not entry.is_dir()}

    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory()
        self.addCleanup(self.fixture.cleanup)
        self.root = Path(self.fixture.name)
        for name in ['LICENSE', 'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
                     'profiles/mhp3rd/host/adhoc/COPYING', 'profiles/mhp3rd/host/adhoc/EXCEPTION',
                     'profiles/mhp3rd/third_party/imgui/LICENSE.txt', 'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE',
                     'profiles/mhp3rd/third_party/xxhash/LICENSE', 'profiles/mhp3rd/cmake/FFmpeg.cmake',
                     'profiles/mhp3rd/packaging/linux/sources.sh', 'profiles/mhp3rd/packaging/android/AndroidManifest.xml']:
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        shutil.copytree(ROOT / 'profiles/mhp3rd/packaging/android/res', self.root / 'profiles/mhp3rd/packaging/android/res')
        self.font, self.license = b'PUBLIC SYNTHETIC FONT', b'PUBLIC SYNTHETIC LICENSE'
        pins = self.root / 'profiles/mhp3rd/packaging/linux/sources.sh'
        pins.write_text('NOTO_CJK_SHA256=' + hashlib.sha256(self.font).hexdigest()
                        + '\nNOTO_CJK_LICENSE_SHA256=' + hashlib.sha256(self.license).hexdigest() + '\n')
        for name in ['SDL_SHA', 'FFMPEG_SHA']:
            patch = mock.patch.object(policy, name, hashlib.sha256(self.license).hexdigest())
            patch.start()
            self.addCleanup(patch.stop)
        self.payloads = {**self.resources, 'classes.dex': dex(),
                         'assets/fonts/NotoSansCJKjp-Regular.otf': self.font,
                         'assets/licenses/NotoSansCJK-OFL.txt': self.license,
                         'assets/licenses/SDL3-LICENSE.txt': self.license,
                         'assets/licenses/FFmpeg-COPYING.LGPLv2.1.txt': self.license,
                         'assets/licenses/FFmpeg-SOURCE.txt': policy.ffmpeg_notice(self.root)}
        checker = self.checker()
        self.payloads.update({name: file.read_bytes() for name, file in checker.fixed.items()})
        self.payloads.update({f'lib/arm64-v8a/{name}.so': elf() for name in ['libmain', 'libSDL3', 'libavcodec', 'libavutil']})
        self.payloads.update({f'lib/arm64-v8a/libovl{i:04}.so': elf() for i in range(355)})

    def checker(self):
        return policy.AndroidPolicy(self.root, VERSION, 'a' * 40)

    def inspect(self, changes=None, omit=()):
        checker = self.checker()
        payloads = {**self.payloads, **(changes or {})}
        for name in omit:
            payloads.pop(name)
        for name, data in payloads.items():
            checker.check(name, data)
        checker.finish(set(payloads))

    def test_complete_unsigned_public_package_matches_source(self):
        self.inspect()

    def test_renamed_game_unknown_assets_and_signatures_are_rejected(self):
        for name in ['assets/disc.iso', 'assets/EBOOT.ELF', 'assets/translation.lang', 'META-INF/MANIFEST.MF',
                     'lib/x86_64/libmain.so', 'assets/licenses/extra.txt', 'res/drawable/other.png',
                     'res/mipmap-mdpi-v5/ic_launcher.png', 'classes2.dex']:
            with self.subTest(name=name), self.assertRaises(Rejected):
                self.inspect({name: b'PRIVATE ORIGINAL STAND-IN'})
        for name in ['lib/arm64-v8a/libmain.so', 'lib/arm64-v8a/libovl0000.so', 'classes.dex']:
            with self.subTest(name=name), self.assertRaises(Rejected):
                self.inspect({name: b'~PSP' + bytes(200)})

    def test_icons_font_licenses_and_source_notices_require_exact_content(self):
        for name in ['res/mipmap-mdpi-v4/ic_launcher.png', 'assets/fonts/NotoSansCJKjp-Regular.otf',
                     'assets/licenses/SDL3-LICENSE.txt', 'assets/licenses/FFmpeg-SOURCE.txt',
                     'assets/licenses/Yakumo-LICENSE.txt', 'assets/licenses/NotoSansCJK-OFL.txt']:
            with self.subTest(name=name), self.assertRaises(Rejected):
                self.inspect({name: self.payloads[name] + b'PRIVATE APPENDIX'})

    def test_missing_duplicate_and_incorrect_overlay_architecture_are_rejected(self):
        for name in ['lib/arm64-v8a/libovl0000.so', 'lib/arm64-v8a/libavcodec.so', 'classes.dex',
                     'resources.arsc', 'assets/licenses/xxHash-LICENSE.txt', 'res/xml/backup_rules.xml']:
            with self.subTest(name=name), self.assertRaises(Rejected):
                self.inspect(omit=[name])
        with self.assertRaises(Rejected):
            self.inspect({'lib/arm64-v8a/libovl0000.so': elf(62)})
        checker = self.checker()
        checker.check('classes.dex', dex())
        with self.assertRaises(Rejected):
            checker.check('classes.dex', dex())

    def test_manifest_identity_version_security_and_resource_trees_are_checked(self):
        manifest = self.resources['AndroidManifest.xml']
        for before, after in [('io.github.teamgdb.yakumo', 'io.github.teamgdb.otherx'),
                              (VERSION, 'nightly-ffffffffffff'), ('.YakumoActivity', '.OtherxActivity')]:
            # aapt2's linked manifest string pool uses UTF-16. Length-preserving
            # mutation keeps the container valid and exercises semantic policy.
            self.assertEqual(len(before), len(after))
            changed = manifest.replace(before.encode('utf-16-le'), after.encode('utf-16-le'))
            self.assertNotEqual(changed, manifest)
            with self.subTest(before=before), self.assertRaises(Rejected):
                self.inspect({'AndroidManifest.xml': changed})
        tree = policy.binary_xml(manifest)
        tree[2][0][1]['{' + policy.ANDROID + '}minSdkVersion'] = (0x10, 30)
        with self.assertRaises(Rejected):
            policy.validate_manifest(tree, self.root, VERSION)
        path = 'res/xml/backup_rules.xml'
        changed = self.resources[path].replace(b'settings.ini', b'anything.ini')
        self.assertNotEqual(changed, self.resources[path])
        with self.assertRaises(Rejected):
            self.inspect({path: changed})

    def test_xml_and_resource_binary_tampering_cannot_hide_arbitrary_data(self):
        for name in ['AndroidManifest.xml', 'resources.arsc', 'res/mipmap-anydpi-v26/ic_launcher.xml']:
            original = self.resources[name]
            for data in [original[:-4], original + b'PRIVATE DATA', b'PRIVATE DATA', original[:8] + bytes(len(original) - 8)]:
                with self.subTest(name=name, size=len(data)), self.assertRaises((Rejected, struct.error)):
                    self.inspect({name: data})
        table = self.resources['resources.arsc']
        for before, after in [('Yakumo', 'SECRET'), ('app_name', 'game_iso'), ('io.github.teamgdb.yakumo', 'io.github.teamgdb.otherx')]:
            for encoding in ['utf-8', 'utf-16-le']:
                changed = table.replace(before.encode(encoding), after.encode(encoding))
                if changed != table:
                    with self.subTest(before=before, encoding=encoding), self.assertRaises(Rejected):
                        self.inspect({'resources.arsc': changed})


if __name__ == '__main__':
    unittest.main()
