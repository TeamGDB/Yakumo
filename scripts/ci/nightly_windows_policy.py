"""Fail-closed file-content policy for a portable Windows nightly ZIP."""

from datetime import datetime
import hashlib
from pathlib import Path
import re
import struct

from nightly_formats import Rejected

MAX_NATIVE = 512 * 1024 * 1024
SDL_LICENSE_SHA = '1c040b8271b37e5076359f8fd54240e371114112924d2df81ef87c7d6a1dfdfd'
FFMPEG_LICENSE_SHA = 'da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768'
ROOT_DLLS = {'SDL3.dll', 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll',
             'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'}
PORTABLE_MARKER = (b'This file makes Yakumo portable: it keeps its settings, saves and game data\r\n'
                   b'in the data folder next to Yakumo.exe. Delete it to use %APPDATA%\\Yakumo\\MHP3rd instead.\r\n')
OVERLAY = re.compile(r'overlays/ovl[A-Za-z0-9_]+\.dll')


def _reject():
    raise Rejected('Unexpected Windows package content.')


def _aligned(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def _certificate(data):
    """Bound Authenticode's PKCS#7 envelope; do not treat it as arbitrary padding."""
    if not 8 <= len(data) <= 256 * 1024 or len(data) % 8:
        _reject()
    cursor = 0
    entries = 0
    while cursor < len(data):
        if cursor + 8 > len(data) or entries >= 8:
            _reject()
        length, revision, kind = struct.unpack_from('<IHH', data, cursor)
        if length < 10 or cursor + length > len(data) or revision != 0x0200 or kind != 0x0002:
            _reject()
        payload = data[cursor + 8:cursor + length]
        if payload[0] != 0x30:
            _reject()
        encoded_length = payload[1]
        header = 2
        if encoded_length & 0x80:
            count = encoded_length & 0x7f
            if not 1 <= count <= 4 or len(payload) < 2 + count or payload[2] == 0:
                _reject()
            encoded_length = int.from_bytes(payload[2:2 + count], 'big')
            header += count
            if encoded_length < 128:
                _reject()
        end = header + encoded_length
        # PKCS#7 signedData ContentInfo, then its explicit [0] content. DER may
        # have at most seven zero bytes of WinCertificate alignment padding.
        if (end > len(payload) or len(payload) - end > 7 or any(payload[end:])
                or payload[header:header + 12] != bytes.fromhex('06092a864886f70d010702a0')):
            _reject()
        following = _aligned(cursor + length, 8)
        if following > len(data) or any(data[cursor + length:following]):
            _reject()
        cursor = following
        entries += 1


def validate_binary(data, library=True):
    """Require a complete x64 PE32+ image, with no unmapped trailing payload.

    This validates format/architecture and the loadable layout, not the behavior
    of compiled code or the trust/signature of a third-party binary. Trusted,
    reviewed sources and pinned dependencies remain part of the boundary.
    """
    if not isinstance(data, bytes) or not 64 <= len(data) <= MAX_NATIVE or data[:2] != b'MZ':
        _reject()
    pe = struct.unpack_from('<I', data, 60)[0]
    if not 64 <= pe <= 4096 or pe % 4 or pe + 24 + 240 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        _reject()
    machine, count, _, symbols, symbol_count, optional_size, flags = struct.unpack_from('<HHIIIHH', data, pe + 4)
    optional = pe + 24
    if (machine != 0x8664 or not 1 <= count <= 96 or optional_size != 240 or symbols or symbol_count
            or not flags & 0x0002 or bool(flags & 0x2000) != library or flags & 0x0100
            or struct.unpack_from('<H', data, optional)[0] != 0x020b
            or struct.unpack_from('<I', data, optional + 108)[0] != 16):
        _reject()
    section_alignment, file_alignment = struct.unpack_from('<II', data, optional + 32)
    image_size, header_size = struct.unpack_from('<II', data, optional + 56)
    entry = struct.unpack_from('<I', data, optional + 16)[0]
    section_table = optional + optional_size
    table_end = section_table + 40 * count
    if (not 512 <= file_alignment <= 65536 or file_alignment & (file_alignment - 1)
            or not file_alignment <= section_alignment <= 65536
            or section_alignment & (section_alignment - 1)
            or table_end > len(data) or header_size != _aligned(table_end, file_alignment)
            or header_size > len(data) or image_size > 1024 * 1024 * 1024):
        _reject()
    ranges = []
    virtual_ranges = []
    executable_entry = False
    for index in range(count):
        section = section_table + 40 * index
        virtual_size, address, size, pointer = struct.unpack_from('<IIII', data, section + 8)
        section_flags = struct.unpack_from('<I', data, section + 36)[0]
        if address < _aligned(header_size, section_alignment) or address % section_alignment or not max(size, virtual_size):
            _reject()
        virtual_end = _aligned(address + max(size, virtual_size), section_alignment)
        if virtual_end > image_size:
            _reject()
        virtual_ranges.append((address, virtual_end))
        if address <= entry < address + max(size, virtual_size) and section_flags & 0x20000000:
            executable_entry = True
        if size:
            if size % file_alignment or pointer < header_size or pointer % file_alignment or pointer + size > len(data):
                _reject()
            ranges.append((pointer, pointer + size))
        elif pointer:
            _reject()
    if not ranges or (entry and not executable_entry) or (not library and not entry):
        _reject()
    virtual_ranges.sort()
    for previous, following in zip(virtual_ranges, virtual_ranges[1:]):
        if previous[1] > following[0]:
            _reject()
    if virtual_ranges[-1][1] != image_size:
        _reject()
    cursor = header_size
    for start, end in sorted(ranges):
        if start != cursor:
            _reject()
        cursor = end
    certificate_offset, certificate_size = struct.unpack_from('<II', data, optional + 112 + 4 * 8)
    if certificate_size:
        if certificate_offset != cursor or certificate_offset % 8 or certificate_offset + certificate_size != len(data):
            _reject()
        _certificate(data[certificate_offset:])
    elif certificate_offset or cursor != len(data):
        _reject()


def _lines(data):
    # Permit the producer's complete LF or complete CRLF text, never mixed
    # separators, NULs, controls or an extra arbitrary metadata line.
    if not isinstance(data, bytes) or b'\0' in data or len(data) > 16384:
        _reject()
    if b'\r' in data:
        if data.count(b'\r') != data.count(b'\r\n') or b'\n' in data.replace(b'\r\n', b''):
            _reject()
        data = data.replace(b'\r\n', b'\n')
    return data


def _pin(root, path, name):
    content = (root / path).read_text()
    match = re.search(r'^' + re.escape(name) + r'=([^\n]+)$', content, re.M)
    if not match:
        _reject()
    return match[1].strip('"')


def ffmpeg_source(root):
    content = (root / 'profiles/mhp3rd/cmake/FFmpeg.cmake').read_text()
    version = re.search(r'set\(MHP3RD_FFMPEG_VERSION\s+([0-9.]+)\)', content)
    url = re.search(r'set\(MHP3RD_FFMPEG_WINDOWS_URL\s+"([^"\r\n]+)"\)', content)
    if not version or not url or not re.fullmatch(r'https://github\.com/BtbN/FFmpeg-Builds/releases/download/[A-Za-z0-9._/-]+\.zip', url[1]):
        _reject()
    text = (f'The FFmpeg libraries here (avcodec, avutil, swresample) are an unmodified prebuilt\n'
            f'LGPL shared build of FFmpeg {version[1]} from BtbN/FFmpeg-Builds, licensed under the\n'
            'GNU LGPL version 3 or later (FFmpeg-LICENSE.txt). They are dynamically linked.\n\n'
            f'Build:  {url[1]}\n'
            'Source: https://github.com/FFmpeg/FFmpeg/tree/release/7.1 (the commit is in the file name)\n'
            'Build scripts: https://github.com/BtbN/FFmpeg-Builds\n')
    return text.encode(), url[1].rsplit('/', 1)[1]


class WindowsPolicy:
    def __init__(self, root, version, source_sha):
        self.root = Path(root)
        if (not isinstance(version, str) or not isinstance(source_sha, str)
                or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,95}', version)
                or not re.fullmatch('[0-9a-f]{40}', source_sha)):
            _reject()
        self.version = version
        self.source = source_sha
        self.checked = set()
        self.folded = set()
        self.fixed = {
            'licenses/Yakumo-LICENSE.txt': 'LICENSE',
            'licenses/Yakumo-adhoc-GPL-3.0.txt': 'profiles/mhp3rd/host/adhoc/COPYING',
            'licenses/Yakumo-adhoc-EXCEPTION.txt': 'profiles/mhp3rd/host/adhoc/EXCEPTION',
            'licenses/THIRD_PARTY_NOTICES.md': 'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
            'licenses/DearImGui-LICENSE.txt': 'profiles/mhp3rd/third_party/imgui/LICENSE.txt',
            'licenses/tiny-AES-c-UNLICENSE.txt': 'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE',
            'licenses/xxHash-LICENSE.txt': 'profiles/mhp3rd/third_party/xxhash/LICENSE',
        }
        self.hashed = {'licenses/SDL3-LICENSE.txt': SDL_LICENSE_SHA,
                       'licenses/FFmpeg-LICENSE.txt': FFMPEG_LICENSE_SHA}
        self.required = {'Yakumo.exe', *ROOT_DLLS, *self.fixed, *self.hashed,
                         'licenses/FFmpeg-SOURCE.txt', 'README.txt', 'portable.txt', 'BUILDINFO.txt'}
        self.ffmpeg_text, self.ffmpeg_name = ffmpeg_source(self.root)
        self.sdl_version = _pin(self.root, 'profiles/mhp3rd/packaging/linux/sources.sh', 'SDL3_VERSION')

    def _buildinfo(self, data):
        try:
            content = _lines(data).decode('ascii')
        except UnicodeDecodeError:
            _reject()
        pattern = (r'Version: v' + re.escape(self.version) + r'\nCommit: ' + self.source
                   + r'\nBuilt: (\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) UTC\n'
                   r'Platform: Windows x86-64\nConfiguration: Release; MHP3RD_RELEASE=ON\n'
                   r'Compiler: MSVC 19\.44\.[0-9]{1,10}(?:\.[0-9]{1,10})?\nSDL3: '
                   + re.escape(self.sdl_version) + r'\nFFmpeg: bundled LGPL shared build \('
                   + re.escape(self.ffmpeg_name) + r'\)\nOverlay DLLs: 355\n')
        match = re.fullmatch(pattern, content, re.ASCII)
        if not match:
            _reject()
        try:
            datetime.strptime(match[1], '%Y-%m-%d %H:%M:%S')
        except ValueError:
            _reject()

    def check(self, relative_name, data):
        if (not isinstance(relative_name, str) or not isinstance(data, bytes)
                or relative_name.casefold() in self.folded):
            _reject()
        if relative_name == 'Yakumo.exe':
            validate_binary(data, library=False)
        elif relative_name in ROOT_DLLS or OVERLAY.fullmatch(relative_name):
            validate_binary(data)
        elif relative_name in self.fixed:
            if data != (self.root / self.fixed[relative_name]).read_bytes():
                _reject()
        elif relative_name in self.hashed:
            if hashlib.sha256(data).hexdigest() != self.hashed[relative_name]:
                _reject()
        elif relative_name == 'licenses/FFmpeg-SOURCE.txt':
            if _lines(data) != self.ffmpeg_text:
                _reject()
        elif relative_name == 'README.txt':
            template = self.root / 'profiles/mhp3rd/packaging/windows/README-portable.txt.in'
            if _lines(data) != _lines(template.read_bytes()).replace(b'@VERSION@', self.version.encode()):
                _reject()
        elif relative_name == 'portable.txt':
            if data != PORTABLE_MARKER:
                _reject()
        elif relative_name == 'BUILDINFO.txt':
            self._buildinfo(data)
        else:
            _reject()
        self.checked.add(relative_name)
        self.folded.add(relative_name.casefold())

    def finish(self, names):
        if names != self.checked or not self.required <= names:
            _reject()
        overlays = {name for name in names if OVERLAY.fullmatch(name)}
        if len(overlays) != 355 or names != self.required | overlays:
            _reject()
