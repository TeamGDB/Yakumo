"""Bounded ZIP containers and platform-specific nightly package policies.

Producer ZIP metadata is never published: accepted payloads are written into a
new canonical ZIP. Android APKs keep their validated original ZIP byte layout;
only the separate signing job may add cryptographic signing blocks.
"""

import contextlib
import re
import stat
import struct
import zipfile
import zlib
from pathlib import Path

MAX_COMPRESSED = 2 * 1024 ** 3
MAX_EXPANDED = 4 * 1024 ** 3
MAX_FILE = 512 * 1024 ** 2
PLATFORMS = ('linux-x86_64', 'macos-arm64', 'windows-x86_64', 'android-arm64')


class Rejected(Exception):
    """Validation failed; callers must not expose private exception details."""


def package_name(platform, version):
    if platform not in PLATFORMS or not isinstance(version, str) or not re.fullmatch('[A-Za-z0-9._-]+', version):
        raise Rejected('Unsupported package identity.')
    suffix = {'linux-x86_64': '.tar.gz', 'macos-arm64': '.zip',
              'windows-x86_64': '-portable.zip', 'android-arm64': '.apk'}[platform]
    return f'yakumo-{version}-{platform}{suffix}'


def version_from_name(platform, name):
    if platform not in PLATFORMS or not isinstance(name, str):
        raise Rejected('Unsupported package identity.')
    suffix = {'macos-arm64': '-macos-arm64.zip', 'windows-x86_64': '-windows-x86_64-portable.zip',
              'android-arm64': '-android-arm64.apk', 'linux-x86_64': '-linux-x86_64.tar.gz'}[platform]
    if not name.startswith('yakumo-') or not name.endswith(suffix):
        raise Rejected('Unexpected platform package filename.')
    value = name[7:-len(suffix)]
    if package_name(platform, value) != name:
        raise Rejected('Invalid platform package version.')
    return value


def safe_name(name):
    if (not isinstance(name, str) or not name or len(name) > 1024 or '\\' in name
            or any(ord(c) < 32 or ord(c) == 127 for c in name)
            or any(p in ('', '.', '..') for p in name.rstrip('/').split('/'))):
        raise Rejected('Unsafe ZIP member path.')
    return name.rstrip('/')


def _apk_gap(stream, start, end):
    """Unsigned producer APKs must have no hidden bytes before the directory."""
    if start != end:
        raise Rejected('Unexpected unsigned APK signing or trailing block.')


def _compressed_payload(raw, position, info):
    raw.seek(position)
    remaining, produced, checksum = info.compress_size, 0, 0
    decoder = zlib.decompressobj(-15) if info.compress_type == 8 else None
    if decoder is None and info.compress_size != info.file_size:
        raise Rejected('Stored ZIP member lengths disagree.')
    while remaining:
        block = raw.read(min(64 * 1024, remaining))
        if not block:
            raise Rejected('Truncated compressed ZIP payload.')
        remaining -= len(block)
        while block:
            data = decoder.decompress(block, min(1024 * 1024, info.file_size - produced + 1)) if decoder else block
            produced += len(data)
            checksum = zlib.crc32(data, checksum)
            if produced > info.file_size or (decoder and decoder.unused_data):
                raise Rejected('Hidden or excessive compressed ZIP data.')
            tail = decoder.unconsumed_tail if decoder else b''
            if tail == block and not data:
                raise Rejected('Nonconverging ZIP decompression.')
            block = tail
    if produced != info.file_size or checksum != info.CRC or (decoder and not decoder.eof):
        raise Rejected('Compressed ZIP data does not match its directory.')


def _apk_extra(kind, data, local=True):
    if kind == 0xd935:
        if len(data) < 2 or int.from_bytes(data[:2], 'little') not in (4, 4096, 16384) or any(data[2:]):
            raise Rejected('Nonzero APK alignment padding.')
    elif kind == 0x5455:
        if (not data or data[0] not in (1, 3, 7)
                or len(data) != (1 + 4 * bin(data[0]).count('1') if local else 5)):
            raise Rejected('Invalid APK timestamp metadata.')
        for offset in range(1, len(data), 4):
            if not 946684800 <= int.from_bytes(data[offset:offset + 4], 'little') <= 4102444800:
                raise Rejected('Unexpected APK timestamp value.')
    elif kind == 0x7875:
        if len(data) < 5 or data[0] != 1 or not 1 <= data[1] <= 4:
            raise Rejected('Invalid APK Unix metadata.')
        position = 2 + data[1]
        if position >= len(data) or not 1 <= data[position] <= 4 or position + 1 + data[position] != len(data):
            raise Rejected('Invalid APK Unix metadata lengths.')
        if (int.from_bytes(data[2:position], 'little') > 65535
                or int.from_bytes(data[position + 1:], 'little') > 65535):
            raise Rejected('Unexpected APK Unix identity.')
    else:
        raise Rejected('Unexpected APK extra field.')


@contextlib.contextmanager
def checked_zip(path, apk=False):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or not 22 <= path.stat().st_size <= MAX_COMPRESSED:
        raise Rejected('Invalid ZIP input.')
    with path.open('rb') as raw:
        raw.seek(-22, 2)
        tail = raw.read(22)
        if tail[:4] != b'PK\x05\x06' or tail[20:] != b'\0\0':
            raise Rejected('ZIP comments or trailing bytes are forbidden.')
        _, disk, start_disk, count, count2, directory_size, directory_start, comment = struct.unpack('<4s4H2LH', tail)
        if disk or start_disk or count != count2 or not 0 < count <= 1500 or comment:
            raise Rejected('Unsupported multipart or oversized ZIP.')
        if directory_start + directory_size != path.stat().st_size - 22:
            raise Rejected('Unexpected ZIP prefix, footer or directory.')
        with zipfile.ZipFile(raw) as archive:
            if archive.comment or len(archive.infolist()) != count:
                raise Rejected('Unexpected ZIP metadata.')
            total = 0
            cursor = 0
            seen = set()
            for info in sorted(archive.infolist(), key=lambda i: i.header_offset):
                name = safe_name(info.filename)
                folded = name.casefold()
                if folded in seen or info.flag_bits & 1 or info.compress_type not in (0, 8):
                    raise Rejected('Duplicate, encrypted or unsupported ZIP member.')
                seen.add(folded)
                mode = info.external_attr >> 16
                if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                    raise Rejected('ZIP links and special files are forbidden.')
                if info.is_dir() and info.file_size:
                    raise Rejected('Unexpected directory payload.')
                if not 0 <= info.file_size <= MAX_FILE:
                    raise Rejected('ZIP member exceeds its limit.')
                total += info.file_size
                if total > MAX_EXPANDED or info.header_offset != cursor:
                    raise Rejected('Expanded ZIP or hidden local data exceeds its policy.')
                raw.seek(cursor)
                local = raw.read(30)
                if len(local) != 30 or local[:4] != b'PK\x03\x04':
                    raise Rejected('Invalid ZIP local header.')
                fields = struct.unpack('<4s5H3L2H', local)
                flag, compression = fields[2:4]
                local_name = raw.read(fields[-2])
                local_extra = raw.read(fields[-1])
                encoding = 'utf-8' if flag & 0x800 else 'cp437'
                if (local_name.decode(encoding) != info.filename or flag != info.flag_bits
                        or compression != info.compress_type):
                    raise Rejected('ZIP headers disagree.')
                payload_start = info.header_offset + 30 + len(local_name) + len(local_extra)
                for extras, is_local in ((local_extra, True), (info.extra, False)):
                    offset = 0
                    while offset < len(extras):
                        if (apk and is_local and info.compress_type == 0 and len(extras) - offset <= 6
                                and not any(extras[offset:]) and payload_start % 4 == 0):
                            # AAPT2 can retain 0..3 alignment bytes; zipalign
                            # appends another 0..3 after archive layout changes.
                            break
                        if offset + 4 > len(extras):
                            raise Rejected('Invalid ZIP extra-field framing.')
                        kind, size = struct.unpack_from('<HH', extras, offset)
                        if kind == 1 or offset + 4 + size > len(extras):
                            raise Rejected('ZIP64 or malformed extra fields are unsupported.')
                        # Canonical desktop output drops all extras. Unsigned
                        # APKs may only retain normal timestamps/alignment.
                        if apk:
                            _apk_extra(kind, extras[offset + 4:offset + 4 + size], local=is_local)
                        offset += 4 + size
                _compressed_payload(raw, payload_start, info)
                cursor = payload_start + info.compress_size
                if flag & 8:
                    raw.seek(cursor)
                    descriptor = raw.read(16)
                    if descriptor[:4] == b'PK\x07\x08':
                        descriptor, amount = descriptor[4:], 16
                    else:
                        descriptor, amount = descriptor[:12], 12
                    if len(descriptor) != 12 or struct.unpack('<3L', descriptor) != (info.CRC, info.compress_size, info.file_size):
                        raise Rejected('Invalid ZIP data descriptor.')
                    cursor += amount
                elif fields[6:9] != (info.CRC, info.compress_size, info.file_size):
                    raise Rejected('ZIP local lengths disagree.')
                if info.comment:
                    raise Rejected('Per-member ZIP comments are forbidden.')
            if apk:
                _apk_gap(raw, cursor, directory_start)
            elif cursor != directory_start:
                raise Rejected('Unexpected hidden ZIP data.')
            yield archive


def policy(platform, root, version, source_sha):
    if platform == 'macos-arm64':
        from nightly_macos_policy import MacPolicy
        return MacPolicy(root, version, source_sha)
    if platform == 'windows-x86_64':
        from nightly_windows_policy import WindowsPolicy
        return WindowsPolicy(root, version, source_sha)
    if platform == 'android-arm64':
        from nightly_android_policy import AndroidPolicy
        return AndroidPolicy(root, version, source_sha)
    raise Rejected('Unsupported ZIP platform.')


def canonical_package(path, output, platform, root, source_sha):
    """Inspect actual payloads and remove desktop producer/xattr metadata."""
    path, output = Path(path), Path(output)
    version = version_from_name(platform, path.name)
    rules = policy(platform, root, version, source_sha)
    prefix = '' if platform == 'android-arm64' else path.name[:-4] + '/'
    seen = set()
    def normalize(info):
        data = archive.read(info)
        if platform == 'macos-arm64' and (info.filename.endswith('.dylib') or info.filename.endswith('/MacOS/Yakumo')):
            from nightly_macos_policy import normalize_signature_padding
            data = normalize_signature_padding(data)
        return data

    with checked_zip(path, apk=platform == 'android-arm64') as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            if platform == 'macos-arm64' and info.filename.startswith('__MACOSX/'):
                # ditto's AppleDouble/xattr streams are not part of the app.
                # They never reach the canonical ZIP; bound their allocation.
                if info.file_size > 1024 * 1024 or '/._' not in info.filename:
                    raise Rejected('Unexpected Apple metadata.')
                continue
            if not info.filename.startswith(prefix):
                raise Rejected('Wrong ZIP package root.')
            name = info.filename[len(prefix):]
            if name in seen:
                raise Rejected('Duplicate payload path.')
            seen.add(name)
            try:
                data = normalize(info)  # also checks actual decompression/CRC
                rules.check(name, data)
            except Exception as error:
                # The CLI captures details privately; retain the member context
                # for maintainers reproducing a rejected package.
                raise Rejected('Package inspection rejected member ' + repr(name)) from error
        rules.finish(seen)
        if platform == 'android-arm64':
            # The signer consumes precisely this validated unsigned layout.
            import shutil
            shutil.copyfile(path, output)
        else:
            with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
                for info in sorted(archive.infolist(), key=lambda item: item.filename):
                    if info.is_dir() or info.filename.startswith('__MACOSX/'):
                        continue
                    member = zipfile.ZipInfo(info.filename, (1980, 1, 1, 0, 0, 0))
                    member.create_system = 3
                    member.external_attr = (stat.S_IFREG | (0o755 if '/MacOS/' in info.filename else 0o644)) << 16
                    member.compress_type = zipfile.ZIP_DEFLATED
                    target.writestr(member, normalize(info))


def binary(platform, data, library=True):
    if platform == 'windows-x86_64':
        from nightly_windows_policy import validate_binary
    elif platform == 'macos-arm64':
        from nightly_macos_policy import validate_binary
    elif platform == 'android-arm64':
        from nightly_android_policy import validate_binary
    else:
        raise Rejected('Unsupported native cache platform.')
    return validate_binary(data, library=library)


def zip_overlays(path, destination, platform):
    """Private, pinned cache seeds may retain their original signing metadata."""
    version_from_name(platform, path.name)
    # A released APK has signing blocks. ZipFile is sufficient for a private
    # checksum-pinned seed; only selected, validated native modules are read.
    with zipfile.ZipFile(path) as archive:
        selected = []
        seen = set()
        total = 0
        for info in archive.infolist():
            name = safe_name(info.filename)
            if info.is_dir():
                continue
            if platform == 'macos-arm64':
                match = re.fullmatch(r'[^/]+/Yakumo\.app/Contents/Frameworks/overlays/([A-Za-z0-9_.-]+\.dylib)', name)
            elif platform == 'windows-x86_64':
                match = re.fullmatch(r'[^/]+/overlays/([A-Za-z0-9_.-]+\.dll)', name)
            else:
                match = re.fullmatch(r'lib/arm64-v8a/(libovl[A-Za-z0-9_.-]+\.so)', name)
            if not match:
                continue
            if match[1].casefold() in seen or not 32 <= info.file_size <= 128 * 1024 ** 2:
                raise Rejected('Invalid native cache entry.')
            seen.add(match[1].casefold())
            total += info.file_size
            if total > MAX_EXPANDED:
                raise Rejected('Native cache exceeds its limit.')
            selected.append((info, match[1]))
        if len(selected) != 355:
            raise Rejected('Native cache is incomplete.')
        for info, name in selected:
            data = archive.read(info)
            binary(platform, data)
            with (destination / name).open('xb') as target:
                target.write(data)
