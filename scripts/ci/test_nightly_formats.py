"""ZIP framing/privacy contracts using only small independently made fixtures."""

import io
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile
import zlib

import nightly_formats as formats

SOURCE = '0123456789abcdef0123456789abcdef01234567'
VERSION = 'nightly-public-fixture'


class ContentOracle:
    """Isolate ZIP handling from the separately tested platform file policies."""
    def __init__(self, expected):
        self.expected = expected
        self.seen = set()

    def check(self, name, data):
        if name not in self.expected or self.expected[name] != data:
            raise formats.Rejected('Unexpected public fixture content.')
        self.seen.add(name)

    def finish(self, names):
        if names != set(self.expected) or self.seen != names:
            raise formats.Rejected('Incomplete public fixture.')


def zip_bytes(entries, timestamp=(2026, 10, 18, 12, 30, 0), extra=b'', comment=b''):
    result = io.BytesIO()
    with zipfile.ZipFile(result, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.comment = comment
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)  # Intentional duplicate fixtures.
            for name, data in entries:
                info = name if isinstance(name, zipfile.ZipInfo) else zipfile.ZipInfo(name, timestamp)
                if isinstance(name, str):
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | 0o755) << 16
                    info.extra = extra
                archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
    return result.getvalue()


def framed_zip(name=b'public.txt', data=b'public fixture', method=0, flags=0,
               local_extra=b'', central_extra=b'', member_comment=b'', gap=b'',
               descriptor=None, compressed_tail=b'', declared_size=None, crc=None):
    """Construct independent ZIP records, including non-seekable descriptors."""
    encoded = data
    if method == 8:
        compressor = zlib.compressobj(wbits=-15)
        encoded = compressor.compress(data) + compressor.flush()
    encoded += compressed_tail
    size = len(data) if declared_size is None else declared_size
    crc = zlib.crc32(data) if crc is None else crc
    if descriptor:
        flags |= 8
    local_sizes = (0, 0, 0) if descriptor else (crc, len(encoded), size)
    local = struct.pack('<4s5H3L2H', b'PK\x03\x04', 20, flags, method, 0, 0,
                        *local_sizes, len(name), len(local_extra)) + name + local_extra + encoded
    if descriptor:
        local += (b'PK\x07\x08' if descriptor == 'signed' else b'') + struct.pack('<3L', crc, len(encoded), size)
    central = struct.pack('<4s6H3L5H2L', b'PK\x01\x02', 0x0314, 20, flags, method,
                          0, 0, crc, len(encoded), size, len(name), len(central_extra),
                          len(member_comment), 0, 0, (stat.S_IFREG | 0o644) << 16, 0)
    central += name + central_extra + member_comment
    directory = len(local) + len(gap)
    end = struct.pack('<4s4H2LH', b'PK\x05\x06', 0, 0, 1, 1, len(central), directory, 0)
    return local + gap + central + end


def altered(data, offset, fmt, value):
    result = bytearray(data)
    struct.pack_into(fmt, result, offset, value)
    return bytes(result)


class ZipFormatTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / formats.package_name('windows-x86_64', VERSION)
        self.output = self.root / 'canonical.zip'

    def write(self, data):
        self.path.write_bytes(data)
        return self.path

    def checked(self, data, apk=False):
        self.write(data)
        with formats.checked_zip(self.path, apk=apk) as archive:
            return {info.filename: archive.read(info) for info in archive.infolist() if not info.is_dir()}

    def rejected(self, data, apk=False):
        # zipfile itself rejects corrupt directory/CRC encodings; both parser
        # failures and explicit policy rejections stop publication.
        with self.assertRaises((formats.Rejected, zipfile.BadZipFile, UnicodeDecodeError)):
            self.checked(data, apk=apk)

    def canonical(self, entries, expected, platform='windows-x86_64', **metadata):
        self.path = self.root / formats.package_name(platform, VERSION)
        self.write(zip_bytes(entries, **metadata))
        with mock.patch.object(formats, 'policy', return_value=ContentOracle(expected)) as factory:
            formats.canonical_package(self.path, self.output, platform, self.root, SOURCE)
            factory.assert_called_once_with(platform, self.root, VERSION, SOURCE)
        return self.output.read_bytes()

    def test_platform_package_identities_and_bad_versions(self):
        expected = {'linux-x86_64': f'yakumo-{VERSION}-linux-x86_64.tar.gz',
                    'macos-arm64': f'yakumo-{VERSION}-macos-arm64.zip',
                    'windows-x86_64': f'yakumo-{VERSION}-windows-x86_64-portable.zip',
                    'android-arm64': f'yakumo-{VERSION}-android-arm64.apk'}
        for platform, name in expected.items():
            self.assertEqual(formats.package_name(platform, VERSION), name)
            self.assertEqual(formats.version_from_name(platform, name), VERSION)
            for invalid in ['', '../escape', 'has space', 'line\nbreak', 'v:stream']:
                with self.subTest(platform=platform, version=invalid), self.assertRaises(formats.Rejected):
                    formats.package_name(platform, invalid)
            for bad in ['wrong-prefix-' + name, name + '.extra', name.replace('yakumo-', 'other-', 1)]:
                with self.assertRaises(formats.Rejected):
                    formats.version_from_name(platform, bad)
        with self.assertRaises(formats.Rejected):
            formats.package_name('unknown', VERSION)
        with self.assertRaises(formats.Rejected):
            formats.version_from_name('unknown', 'arbitrary.zip')

    def test_safe_member_paths_and_regular_zip_contents(self):
        self.assertEqual(formats.safe_name('directory/public.txt'), 'directory/public.txt')
        self.assertEqual(formats.safe_name('directory/'), 'directory')
        for name in ['', None, '../escape', '/absolute', 'a/../b', 'a/./b', 'a//b',
                     'a\\b', 'a\0b', 'a\nb', 'a\x7fb', 'x' * 1025]:
            with self.subTest(name=name), self.assertRaises(formats.Rejected):
                formats.safe_name(name)
        for method in [0, 8]:
            self.assertEqual(self.checked(framed_zip(method=method)), {'public.txt': b'public fixture'})

    def test_rejects_missing_linked_and_oversized_containers(self):
        with self.assertRaises(formats.Rejected), formats.checked_zip(self.root / 'missing.zip'):
            pass
        self.write(framed_zip())
        link = self.root / 'linked.zip'
        try:
            link.symlink_to(self.path)
        except OSError:
            pass  # Windows public full-build probe covers its real filesystem.
        else:
            with self.assertRaises(formats.Rejected), formats.checked_zip(link):
                pass
        self.rejected(b'not a ZIP')
        with mock.patch.object(formats, 'MAX_COMPRESSED', 22):
            self.rejected(framed_zip())
        self.rejected(zip_bytes([]))

    def test_duplicate_case_alias_traversal_controls_and_links_rejected(self):
        for names in [['same.txt', 'same.txt'], ['Same.txt', 'same.txt'],
                      ['../escape'], ['dir/../escape'], ['/absolute'], ['a\nb']]:
            with self.subTest(names=names):
                self.rejected(zip_bytes([(name, b'public fixture') for name in names]))
        # ZipInfo normalizes os.sep during construction on Windows. Independent
        # raw records preserve the malicious backslash in both ZIP headers.
        self.rejected(framed_zip(name=b'a\\b'))
        self.rejected(framed_zip(name=b'a\0hidden'))
        for mode in [stat.S_IFLNK, stat.S_IFIFO, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFSOCK]:
            info = zipfile.ZipInfo('public.txt')
            info.create_system = 3
            info.external_attr = (mode | 0o644) << 16
            with self.subTest(mode=mode):
                self.rejected(zip_bytes([(info, b'public fixture')]))
        self.rejected(zip_bytes([('directory/', b'unexpected directory payload')]))

    def test_zip_metadata_comments_encryption_compression_and_extra_fields(self):
        self.rejected(zip_bytes([('public.txt', b'public fixture')], comment=b'hidden archive comment'))
        self.rejected(framed_zip(member_comment=b'hidden member comment'))
        self.rejected(framed_zip(flags=1))
        self.rejected(framed_zip(method=99))
        self.rejected(framed_zip(central_extra=struct.pack('<HH', 1, 0)))
        for extra in [b'\xfe\xca', struct.pack('<HH', 0xcafe, 5) + b'x']:
            self.rejected(framed_zip(local_extra=extra))
            self.rejected(framed_zip(central_extra=extra))
        # A well-framed desktop xattr is permitted only because it is dropped
        # during canonicalization; unsigned APKs have a stricter retention rule.
        extra = struct.pack('<HH', 0xcafe, 3) + b'xyz'
        self.assertEqual(self.checked(framed_zip(local_extra=extra, central_extra=extra)),
                         {'public.txt': b'public fixture'})

    def test_local_central_headers_and_crc_must_agree(self):
        original = framed_zip()
        central = original.index(b'PK\x01\x02')
        bad = [altered(original, 0, '<I', 0), altered(original, 6, '<H', 0x800),
               altered(original, 8, '<H', 8), altered(original, 14, '<I', 0),
               altered(original, 18, '<I', 1), altered(original, 22, '<I', 1),
               altered(original, 30, '<B', ord('X')),
               altered(original, central + 16, '<I', 0)]
        for index, data in enumerate(bad):
            with self.subTest(case=index):
                self.rejected(data)
        # Altering actual stored bytes leaves consistent headers but fails CRC.
        start = 30 + len(b'public.txt')
        self.rejected(altered(original, start, '<B', ord('X')))

    def test_framing_rejects_prefix_tail_hidden_gap_and_multipart_records(self):
        original = framed_zip()
        self.rejected(b'hidden prefix' + original)
        self.rejected(original + b'hidden suffix')
        self.rejected(original[:-1])
        self.rejected(framed_zip(gap=b'hidden before central directory'))
        self.rejected(framed_zip(gap=b'hidden before central directory'), apk=True)
        end = len(original) - 22
        for offset, fmt, value in [(4, '<H', 1), (6, '<H', 1), (8, '<H', 0),
                                   (10, '<H', 1501), (12, '<I', 1), (16, '<I', 1), (20, '<H', 1)]:
            with self.subTest(offset=offset):
                self.rejected(altered(original, end + offset, fmt, value))

    def test_descriptors_with_and_without_signature_and_bad_sizes(self):
        for descriptor in ['signed', 'unsigned']:
            original = framed_zip(descriptor=descriptor, method=8)
            self.assertEqual(self.checked(original), {'public.txt': b'public fixture'})
            central = original.index(b'PK\x01\x02')
            self.rejected(altered(original, central - 12, '<I', 0))
            self.rejected(altered(original, central - 8, '<I', 0))
            self.rejected(altered(original, central - 4, '<I', 0))

    def test_member_and_total_expansion_limits(self):
        with mock.patch.object(formats, 'MAX_FILE', 3):
            self.rejected(zip_bytes([('public.txt', b'four')]))
        with mock.patch.object(formats, 'MAX_EXPANDED', 5):
            self.rejected(zip_bytes([('one.txt', b'one'), ('two.txt', b'two')]))

    def test_desktop_canonicalization_drops_metadata_and_is_order_independent(self):
        prefix = self.path.name[:-4] + '/'
        expected = {'first.txt': b'public first', 'second.txt': b'public second'}
        entries = [(prefix + name, data) for name, data in expected.items()]
        extra = struct.pack('<HH', 0xcafe, 4) + b'meta'
        first = self.canonical(entries, expected, extra=extra)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.comment, b'')
            self.assertEqual({info.filename for info in archive.infolist()}, {prefix + name for name in expected})
            for info in archive.infolist():
                self.assertEqual(info.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(info.extra, b'')
                self.assertEqual(info.comment, b'')
                self.assertEqual(info.create_system, 3)
                self.assertEqual(info.external_attr >> 16, stat.S_IFREG | 0o644)
        self.output.unlink()
        second = self.canonical(list(reversed(entries)), expected, timestamp=(2020, 1, 2, 3, 4, 6))
        self.assertEqual(first, second)

    def test_wrong_root_or_policy_rejection_does_not_create_output(self):
        for entries in [[('wrong-root/public.txt', b'fixture')], [(self.path.name[:-4] + '/public.txt', b'changed')]]:
            with self.assertRaises(formats.Rejected) as failure:
                self.canonical(entries, {'public.txt': b'fixture'})
            if entries[0][0].startswith(self.path.name[:-4] + '/'):
                self.assertIn(repr('public.txt'), str(failure.exception))
            self.assertFalse(self.output.exists())

    def test_macos_appledouble_is_removed_and_bounded(self):
        name = formats.package_name('macos-arm64', VERSION)
        public = name[:-4] + '/Yakumo.app/Contents/Resources/public.txt'
        metadata = '__MACOSX/yakumo/._public.txt'
        self.canonical([(public, b'public'), (metadata, b'public xattr fixture')],
                       {'Yakumo.app/Contents/Resources/public.txt': b'public'}, platform='macos-arm64')
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.namelist(), [public])
        self.output.unlink()
        for member, payload in [('__MACOSX/unexpected', b'fixture'), (metadata, b'x' * (1024 * 1024 + 1))]:
            with self.assertRaises(formats.Rejected):
                self.canonical([(public, b'public'), (member, payload)],
                               {'Yakumo.app/Contents/Resources/public.txt': b'public'}, platform='macos-arm64')
            self.assertFalse(self.output.exists())

    def test_unsigned_apk_keeps_exact_validated_bytes_and_allowed_small_extras(self):
        self.path = self.root / formats.package_name('android-arm64', VERSION)
        timestamp = struct.pack('<HHBI', 0x5455, 5, 1, 1700000000)
        uid_gid = struct.pack('<HH', 0x7875, 5) + b'\x01\x01\0\x01\0'
        alignment = struct.pack('<HHH', 0xd935, 6, 4) + b'\0' * 4
        for extra in [b'', timestamp, uid_gid, alignment]:
            if self.output.exists():
                self.output.unlink()
            data = framed_zip(name=b'classes.dex', local_extra=extra, central_extra=extra)
            self.write(data)
            with mock.patch.object(formats, 'policy', return_value=ContentOracle({'classes.dex': b'public fixture'})):
                formats.canonical_package(self.path, self.output, 'android-arm64', self.root, SOURCE)
            self.assertEqual(self.output.read_bytes(), data)

    def test_unsigned_apk_rejects_opaque_extras_and_nonzero_alignment_padding(self):
        bad = [struct.pack('<HH', 0xcafe, 4) + b'data',
               struct.pack('<HHH', 0xd935, 3, 4) + b'X',
               struct.pack('<HH', 0xd935, 1) + b'\0',
               struct.pack('<HH', 0x5455, 15) + b'arbitrary bytes',
               struct.pack('<HH', 0x7875, 15) + b'arbitrary bytes']
        for extra in bad:
            with self.subTest(extra=extra):
                self.rejected(framed_zip(local_extra=extra), apk=True)
                self.rejected(framed_zip(central_extra=extra), apk=True)

    def test_unsigned_apk_accepts_only_bounded_zero_local_stored_alignment(self):
        for padding in range(1, 7):
            length = (4 - (30 + padding) % 4) % 4 or 4
            name = b'p' * length
            data = framed_zip(name=name, local_extra=b'\0' * padding)
            self.assertEqual(self.checked(data, apk=True), {name.decode(): b'public fixture'})
            self.rejected(framed_zip(name=name, local_extra=b'\0' * padding, method=8), apk=True)
            self.rejected(framed_zip(name=name, central_extra=b'\0' * padding), apk=True)
            self.rejected(framed_zip(name=name, local_extra=b'\0' * (padding - 1) + b'X'), apk=True)
            self.rejected(framed_zip(name=name + b'x', local_extra=b'\0' * padding), apk=True)
        for padding in [7, 8, 16, 16384]:
            length = (4 - (30 + padding) % 4) % 4 or 4
            self.rejected(framed_zip(name=b'p' * length, local_extra=b'\0' * padding), apk=True)
        # A retained ordinary metadata field may precede the bounded zero tail.
        timestamp = struct.pack('<HHBI', 0x5455, 5, 1, 1700000000)
        data = framed_zip(name=b'ppp', local_extra=timestamp + b'\0' * 6)
        self.assertEqual(self.checked(data, apk=True), {'ppp': b'public fixture'})
        self.rejected(framed_zip(name=b'p', local_extra=b'X'), apk=True)
        self.rejected(framed_zip(central_extra=b'\0'), apk=True)

    def test_deflate_stream_validation_handles_chunked_expansion(self):
        payload = b'public fixture\n' * (200000)
        result = self.checked(framed_zip(data=payload, method=8), apk=True)
        self.assertEqual(result, {'public.txt': payload})
        # Declared size and CRC matching a prefix cannot conceal the remainder.
        self.rejected(framed_zip(data=payload, method=8, declared_size=10,
                                 crc=zlib.crc32(payload[:10])), apk=True)

    def test_hidden_compressed_suffix_or_truncated_expansion_is_rejected(self):
        for method in [0, 8]:
            for data in [framed_zip(method=method, compressed_tail=b'hidden payload'),
                         framed_zip(method=method, declared_size=6, crc=zlib.crc32(b'public'))]:
                with self.subTest(method=method):
                    self.rejected(data, apk=True)


class OverlayCacheZipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def entries(self, platform):
        suffix = {'windows-x86_64': '.dll', 'macos-arm64': '.dylib', 'android-arm64': '.so'}[platform]
        directory = {'windows-x86_64': 'public-root/overlays/',
                     'macos-arm64': 'public-root/Yakumo.app/Contents/Frameworks/overlays/',
                     'android-arm64': 'lib/arm64-v8a/'}[platform]
        prefix = 'lib' if platform == 'android-arm64' else ''
        return [(f'{directory}{prefix}ovl{index:03}{suffix}', f'public native fixture {index}'.encode().ljust(64, b'\0'))
                for index in range(355)]

    def extract(self, entries, platform='windows-x86_64'):
        path = self.root / formats.package_name(platform, VERSION)
        path.write_bytes(zip_bytes(entries))
        destination = self.root / 'extracted'
        destination.mkdir(exist_ok=True)
        with mock.patch.object(formats, 'binary') as validator:
            formats.zip_overlays(path, destination, platform)
        return destination, validator

    def test_extracts_only_exactly_355_validated_native_modules_on_each_platform(self):
        for platform in ['windows-x86_64', 'macos-arm64', 'android-arm64']:
            with self.subTest(platform=platform):
                entries = self.entries(platform)
                destination, validator = self.extract(entries + [('public-root/ignored.txt', b'public unrelated fixture')], platform)
                self.assertEqual(len(list(destination.iterdir())), 355)
                self.assertEqual(validator.call_count, 355)
                for name, data in entries:
                    self.assertEqual((destination / name.rsplit('/', 1)[1]).read_bytes(), data)
                self.assertFalse((destination / 'ignored.txt').exists())
                for call, (_, data) in zip(validator.call_args_list, entries):
                    self.assertEqual(call.args, (platform, data))
                for path in destination.iterdir():
                    path.unlink()

    def test_incomplete_duplicate_unsafe_or_oversized_cache_fails_before_writes(self):
        entries = self.entries('windows-x86_64')
        bad_sets = [entries[:-1], entries + [('public-root/overlays/ovl000.dll', b'x' * 64)],
                    entries + [('../unsafe', b'public fixture')],
                    [(entries[0][0], b'too short'), *entries[1:]],
                    entries[:353] + [('public-root/overlays/ovlAbc.dll', b'x' * 64),
                                     ('public-root/overlays/ovlabc.dll', b'x' * 64)]]
        for bad in bad_sets:
            with self.subTest(count=len(bad)), self.assertRaises(formats.Rejected):
                self.extract(bad)
            self.assertEqual(list((self.root / 'extracted').iterdir()), [])
        with mock.patch.object(formats, 'MAX_EXPANDED', 100), self.assertRaises(formats.Rejected):
            self.extract(entries)
        self.assertEqual(list((self.root / 'extracted').iterdir()), [])
        path = self.root / formats.package_name('windows-x86_64', VERSION)
        archive = zip_bytes(entries)
        directory = archive.index(b'PK\x01\x02')
        path.write_bytes(altered(archive, directory + 24, '<I', 128 * 1024 ** 2 + 1))
        with self.assertRaises(formats.Rejected), mock.patch.object(formats, 'binary'):
            formats.zip_overlays(path, self.root / 'extracted', 'windows-x86_64')
        self.assertEqual(list((self.root / 'extracted').iterdir()), [])

    def test_native_validator_failure_blocks_extraction_and_existing_files_are_preserved(self):
        entries = self.entries('windows-x86_64')
        with mock.patch.object(formats, 'binary', side_effect=formats.Rejected('Invalid fixture binary.')):
            path = self.root / formats.package_name('windows-x86_64', VERSION)
            path.write_bytes(zip_bytes(entries))
            destination = self.root / 'extracted'
            destination.mkdir()
            with self.assertRaises(formats.Rejected):
                formats.zip_overlays(path, destination, 'windows-x86_64')
            self.assertEqual(list(destination.iterdir()), [])
        sentinel = destination / 'ovl000.dll'
        sentinel.write_bytes(b'preserve existing file')
        with mock.patch.object(formats, 'binary'), self.assertRaises(FileExistsError):
            formats.zip_overlays(path, destination, 'windows-x86_64')
        self.assertEqual(sentinel.read_bytes(), b'preserve existing file')

    def test_unsupported_native_platform_is_rejected(self):
        with self.assertRaises(formats.Rejected):
            formats.binary('unknown', b'public fixture')
        with self.assertRaises(formats.Rejected):
            formats.policy('linux-x86_64', self.root, VERSION, SOURCE)


if __name__ == '__main__':
    unittest.main()
