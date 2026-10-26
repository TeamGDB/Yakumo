"""Bound the signed macOS package emitted by the trusted main-branch builder.

This validates the permitted package and signature metadata; native compiled
code is trusted build output, not a sandbox for an arbitrary hostile compiler.
Mach-O/signature field facts follow Apple's public loader.h and cs_blobs.h.
"""

import hashlib
import json
import plistlib
import re
import struct
from pathlib import Path
from xml.parsers.expat import ExpatError

from nightly_formats import Rejected


APP = 'Yakumo.app/Contents/'
LIBRARIES = {'libSDL3.0.dylib', 'libvulkan.1.dylib', 'libMoltenVK.dylib',
             'libavcodec.61.dylib', 'libavutil.59.dylib'}
EMPTY_REQUIREMENTS = bytes.fromhex('fade0c010000000c00000000')
EMPTY_CMS = bytes.fromhex('fade0b0100000008')
RULES = {'^Resources/': True, '^Resources/.*\\.lproj/': {'optional': True, 'weight': 1000.0},
         '^Resources/.*\\.lproj/locversion.plist$': {'omit': True, 'weight': 1100.0},
         '^Resources/Base\\.lproj/': {'weight': 1010.0}, '^version.plist$': True}
RULES2 = {'.*\\.dSYM($|/)': {'weight': 11.0}, '^(.*/)?\\.DS_Store$': {'omit': True, 'weight': 2000.0},
          '^(Frameworks|SharedFrameworks|PlugIns|Plug-ins|XPCServices|Helpers|MacOS|Library/(Automator|Spotlight|LoginItems))/':
              {'nested': True, 'weight': 10.0},
          '^.*': True, '^Info\\.plist$': {'omit': True, 'weight': 20.0},
          '^PkgInfo$': {'omit': True, 'weight': 20.0}, '^Resources/': {'weight': 20.0},
          '^Resources/.*\\.lproj/': {'optional': True, 'weight': 1000.0},
          '^Resources/.*\\.lproj/locversion.plist$': {'omit': True, 'weight': 1100.0},
          '^Resources/Base\\.lproj/': {'weight': 1010.0}, '^[^/]+$': {'nested': True, 'weight': 10.0},
          '^embedded\\.provisionprofile$': {'weight': 20.0}, '^version\\.plist$': {'weight': 20.0}}


def reject(condition):
    if not condition:
        raise Rejected('Unexpected macOS package content or signature metadata.')


def _typed_equal(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(_typed_equal(left[key], right[key]) for key in left)
    return left == right


def _macho(data, library):
    reject(isinstance(data, bytes) and 32 <= len(data) <= 2 * 1024 ** 3)
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', data)
    reject(magic == 0xFEEDFACF and cpu == 0x0100000C and subtype == 0 and reserved == 0)
    reject(kind in ((6, 8) if library else (2,)) and count <= 512 and 32 + size <= len(data))
    position, signature, minimum = 32, None, None
    for _ in range(count):
        reject(position + 8 <= 32 + size)
        command, length = struct.unpack_from('<II', data, position)
        reject(length >= 8 and length % 8 == 0 and position + length <= 32 + size)
        if command == 0x1D:
            reject(signature is None and length == 16)
            offset, amount = struct.unpack_from('<II', data, position + 8)
            reject(offset >= 32 + size and amount >= 12 and offset + amount == len(data))
            signature = offset, amount
        elif command == 0x32:
            reject(minimum is None and length >= 24)
            platform, minimum, sdk, tools = struct.unpack_from('<4I', data, position + 8)
            reject(platform == 1 and tools <= 16 and length == 24 + tools * 8)
        position += length
    reject(position == 32 + size)
    return kind, signature, minimum


def _signature_capacity(code_size):
    # Older codesign can preserve the linker's 4KiB-page allocation while
    # replacing it with a smaller 16KiB-page directory. Budget that larger hash
    # table plus bounded metadata/alignment, rather than assuming <4KiB slack.
    return ((code_size + 4095) // 4096) * 32 + 65536


def normalize_signature_padding(data):
    """Zero only unused signature allocation, leaving all signed bytes intact.

    Re-signing an existing binary can retain a few old hash bytes after the
    SuperBlob's declared length. They are outside every CodeDirectory page and
    do not participate in its cdhash or the app's nested-code seals.
    """
    reject(len(data) >= 16)
    kind = struct.unpack_from('<I', data, 12)[0]
    _, signature, _ = _macho(data, kind != 2)
    if signature is None:
        return data
    offset, amount = signature
    magic, length, count = struct.unpack_from('>III', data, offset)
    reject(magic == 0xFADE0CC0 and 12 + count * 8 <= length <= amount <= _signature_capacity(offset))
    return data[:offset + length] + bytes(amount - length)


def validate_binary(data, library=True, require_signature=False):
    """Validate exact native ARM64 kind and, for publication, its ad hoc seal."""
    kind, signature, minimum = _macho(data, library)
    if not require_signature:
        return {'kind': kind}
    reject(signature is not None and minimum is not None and 0x000B0000 <= minimum <= 0x000D0000)
    offset, amount = signature
    blob = data[offset:]
    magic, total, count = struct.unpack_from('>III', blob)
    reject(magic == 0xFADE0CC0 and count == 3 and 36 <= total <= amount <= _signature_capacity(offset))
    reject(not any(blob[total:]))
    pieces, spans = {}, []
    for index in range(count):
        slot, start = struct.unpack_from('>II', blob, 12 + index * 8)
        reject(slot not in pieces and start >= 36 and start + 8 <= total)
        _, length = struct.unpack_from('>II', blob, start)
        reject(length >= 8 and start + length <= total)
        pieces[slot] = blob[start:start + length]
        spans.append((start, start + length))
    reject(set(pieces) == {0, 2, 0x10000})
    cursor = 36
    for start, end in sorted(spans):
        reject(start == cursor)
        cursor = end
    reject(cursor == total and pieces[2] == EMPTY_REQUIREMENTS and pieces[0x10000] == EMPTY_CMS)
    directory = pieces[0]
    reject(len(directory) >= 89)
    (magic, length, version, flags, hash_offset, identifier_offset, special_count,
     code_count, limit, hash_size, hash_type, platform, page) = struct.unpack_from('>9I4B', directory)
    reject(magic == 0xFADE0C02 and length == len(directory) and version == 0x20400 and flags == 2)
    reject(hash_size == 32 and hash_type == 2 and platform == 0 and page in (12, 14))
    reject(identifier_offset == 88 and special_count == (2 if library else 3) and limit == offset)
    reject(not any(directory[40:64]))
    base, extent, exec_flags = struct.unpack_from('>3Q', directory, 64)
    reject(base == 0 and 0 < extent <= offset and exec_flags == (0 if library else 1))
    end = directory.find(b'\0', identifier_offset)
    reject(identifier_offset < end <= identifier_offset + 192)
    identifier = directory[identifier_offset:end]
    reject(re.fullmatch(rb'[A-Za-z0-9_.-]+', identifier) is not None)
    reject(hash_offset == end + 1 + special_count * 32 and hash_offset + code_count * 32 == length)
    page_size = 1 << page
    reject(code_count == (limit + page_size - 1) // page_size)
    for index in range(code_count):
        actual = hashlib.sha256(data[index * page_size:min((index + 1) * page_size, limit)]).digest()
        reject(directory[hash_offset + index * 32:hash_offset + (index + 1) * 32] == actual)
    specials = {slot: directory[hash_offset - slot * 32:hash_offset - (slot - 1) * 32]
                for slot in range(1, special_count + 1)}
    reject(specials[2] == hashlib.sha256(EMPTY_REQUIREMENTS).digest())
    if library:
        reject(specials[1] == bytes(32))
    return {'kind': kind, 'cdhash': hashlib.sha256(directory).digest()[:20],
            'specials': specials, 'identifier': identifier.decode('ascii')}


def _pin(root, name):
    text = (root / 'profiles/mhp3rd/packaging/linux/sources.sh').read_text()
    result = re.search(r'^' + re.escape(name) + r'=([0-9a-f]{64})$', text, re.M)
    reject(result is not None)
    return result[1]


def _ffmpeg_notice(root):
    text = (root / 'profiles/mhp3rd/cmake/FFmpeg.cmake').read_text()
    version = re.search(r'set\(MHP3RD_FFMPEG_VERSION\s+([0-9.]+)\)', text)
    checksum = re.search(r'set\(MHP3RD_FFMPEG_SHA256\s+([0-9a-f]{64})\)', text)
    flags = re.search(r'set\(MHP3RD_FFMPEG_CONFIGURE_FLAGS\s+([^)]*)\)', text)
    reject(version is not None and checksum is not None and flags is not None)
    return (f'The FFmpeg libraries here (libavcodec, libavutil) are FFmpeg {version[1]},\n'
            'unmodified, licensed under the GNU LGPL version 2.1 or later\n'
            '(FFmpeg-COPYING.LGPLv2.1.txt). They are dynamically linked.\n\n'
            f'Source: https://ffmpeg.org/releases/ffmpeg-{version[1]}.tar.xz\nSHA-256: {checksum[1]}\n'
            f'Configured with: {" ".join(flags[1].split())}\n').encode()


class MacPolicy:
    def __init__(self, root, version, source_sha):
        self.root = Path(root)
        reject(re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,80}', version) is not None)
        reject(re.fullmatch('[0-9a-f]{40}', source_sha) is not None)
        pins = (self.root / 'profiles/mhp3rd/packaging/macos/sources.sh').read_text()
        target = re.search(r'^MACOS_DEPLOYMENT_TARGET=([0-9.]+)$', pins, re.M)
        reject(target is not None and target[1] == '13.0')
        target = target[1]
        short = re.match(r'^[0-9]+(?:\.[0-9]+){0,2}', version)
        short = short[0] if short else '0.0.0'
        commits = re.match(r'^[0-9.]+-([0-9]+)-g', version)
        bundle = short + ('.' + commits[1] if commits else '')
        template = (self.root / 'profiles/mhp3rd/packaging/macos/Info.plist.in').read_bytes()
        for marker, value in {'VERSION': version, 'SHORT_VERSION': short, 'BUNDLE_VERSION': bundle,
                              'MINIMUM_SYSTEM_VERSION': target}.items():
            template = template.replace(('@' + marker + '@').encode(), value.encode())
        package = self.root / 'profiles/mhp3rd/packaging'
        third = self.root / 'profiles/mhp3rd/third_party'
        self.exact = {APP + 'Info.plist': template, APP + 'PkgInfo': b'APPL????',
                      APP + 'Resources/Yakumo.icns': (package / 'macos/Yakumo.icns').read_bytes(),
                      'Read Me.txt': (package / 'macos/README.txt').read_bytes().replace(
                          b'@MINIMUM_SYSTEM_VERSION@', target.encode()),
                      APP + 'Resources/licenses/FFmpeg-SOURCE.txt': _ffmpeg_notice(self.root)}
        paths = {'Yakumo-LICENSE.txt': self.root / 'LICENSE',
                 'Yakumo-adhoc-GPL-3.0.txt': self.root / 'profiles/mhp3rd/host/adhoc/COPYING',
                 'Yakumo-adhoc-EXCEPTION.txt': self.root / 'profiles/mhp3rd/host/adhoc/EXCEPTION',
                 'THIRD_PARTY_NOTICES.md': package / 'THIRD_PARTY_NOTICES.md',
                 'DearImGui-LICENSE.txt': third / 'imgui/LICENSE.txt',
                 'tiny-AES-c-UNLICENSE.txt': third / 'tiny_aes/UNLICENSE',
                 'xxHash-LICENSE.txt': third / 'xxhash/LICENSE'}
        for name, path in paths.items():
            self.exact[APP + 'Resources/licenses/' + name] = path.read_bytes()
        icd = {'file_format_version': '1.0.0', 'ICD': {
            'library_path': '../../../Frameworks/libMoltenVK.dylib', 'api_version': '1.4.0',
            'is_portability_driver': True}}
        # Compare the precise public template, including bounded whitespace.
        script = (self.root / 'profiles/mhp3rd/scripts/release_macos.sh').read_text()
        literal = re.search(r'cat > "\$resources/vulkan/icd.d/MoltenVK_icd.json" <<\'JSON\'\n(.*?)\nJSON', script, re.S)
        reject(literal is not None and json.loads(literal[1]) == icd)
        self.exact[APP + 'Resources/vulkan/icd.d/MoltenVK_icd.json'] = (literal[1] + '\n').encode()
        hashes = {'SDL3-LICENSE.txt': '1c040b8271b37e5076359f8fd54240e371114112924d2df81ef87c7d6a1dfdfd',
                  'FFmpeg-COPYING.LGPLv2.1.txt': '246041b6ecf9bc32d718a62c57877c78b5eb397b6467e74ed7ae2626ab189c30',
                  'Vulkan-Loader-LICENSE.txt': '43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece',
                  'MoltenVK-LICENSE.txt': 'cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30',
                  'NotoSansCJK-OFL.txt': _pin(self.root, 'NOTO_CJK_LICENSE_SHA256')}
        self.hashes = {APP + 'Resources/licenses/' + name: value for name, value in hashes.items()}
        self.hashes[APP + 'Resources/fonts/NotoSansCJKjp-Regular.otf'] = _pin(self.root, 'NOTO_CJK_SHA256')
        self.seen, self.overlays, self.digests, self.natives = set(), set(), {}, {}
        self.resources = None

    def check(self, name, data):
        reject(isinstance(name, str) and isinstance(data, bytes) and name not in self.seen)
        reject(len(name) <= 256 and not any(ord(c) < 32 for c in name) and '\\' not in name)
        reject(all(part not in ('', '.', '..') for part in name.split('/')))
        self.seen.add(name)
        if name in self.exact:
            reject(data == self.exact[name])
        elif name in self.hashes:
            reject(hashlib.sha256(data).hexdigest() == self.hashes[name])
        elif name == APP + 'MacOS/Yakumo':
            facts = validate_binary(data, library=False, require_signature=True)
            reject(facts['identifier'] == 'io.github.teamgdb.yakumo')
            self.natives['MacOS/Yakumo'] = facts
        elif (name.startswith(APP + 'Frameworks/') and
              (name[len(APP + 'Frameworks/'):] in LIBRARIES or
               re.fullmatch(re.escape(APP) + r'Frameworks/overlays/[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\.dylib', name))):
            facts = validate_binary(data, require_signature=True)
            reject(facts['kind'] == (8 if '/overlays/' in name else 6))
            basename = name.rsplit('/', 1)[1][:-6]
            identifiers = {basename}
            if basename == 'libSDL3.0':
                identifiers.add('libSDL3')
            identifier = re.sub(r'-55554944[0-9a-f]{32}$', '', facts['identifier'])
            reject(identifier in identifiers)
            self.natives[name[len(APP):]] = facts
            if '/overlays/' in name:
                self.overlays.add(name)
        elif name == APP + '_CodeSignature/CodeResources':
            reject(len(data) <= 1024 * 1024)
            try:
                self.resources = plistlib.loads(data)
            except (ValueError, TypeError, OverflowError, plistlib.InvalidFileException, ExpatError):
                raise Rejected('Invalid macOS resource seal.') from None
            reject(isinstance(self.resources, dict) and set(self.resources) == {'files', 'files2', 'rules', 'rules2'})
            reject(_typed_equal(self.resources['rules'], RULES) and _typed_equal(self.resources['rules2'], RULES2))
            reject(isinstance(self.resources['files'], dict) and isinstance(self.resources['files2'], dict))
            # Apple's XML spelling uses integral reals, unlike plistlib's
            # trailing .0. Apart from that precise spelling, reject comments,
            # duplicate keys, binary trailers and other unsealed extra text.
            canonical = plistlib.dumps(self.resources, fmt=plistlib.FMT_XML, sort_keys=True)
            canonical = re.sub(rb'<real>([0-9]+)\.0</real>', rb'<real>\1</real>', canonical)
            reject(data == canonical)
        else:
            raise Rejected('Unexpected macOS package file.')
        if name.startswith(APP):
            self.digests[name[len(APP):]] = (hashlib.sha1(data).digest(), hashlib.sha256(data).digest())

    def finish(self, names):
        required = (set(self.exact) | set(self.hashes) | {APP + 'MacOS/Yakumo', APP + '_CodeSignature/CodeResources'} |
                    {APP + 'Frameworks/' + name for name in LIBRARIES} | self.overlays)
        reject(self.seen == required and len(self.overlays) == 355 and self.resources is not None)
        directories = {''}
        for name in required:
            parts = name.split('/')
            directories.update('/'.join(parts[:i]) for i in range(1, len(parts)))
        reject(set(names) - directories == required)
        resources = {name: hashes for name, hashes in self.digests.items() if name.startswith('Resources/')}
        files, files2 = self.resources['files'], self.resources['files2']
        reject(set(files) == set(resources))
        reject(set(files2) == set(resources) | (set(self.natives) - {'MacOS/Yakumo'}))
        for name, (sha1, sha256) in resources.items():
            reject(files[name] == sha1 and files2[name] == {'hash2': sha256})
        for name, facts in self.natives.items():
            if name == 'MacOS/Yakumo':
                reject(facts['specials'][1] == self.digests['Info.plist'][1])
                reject(facts['specials'][3] == self.digests['_CodeSignature/CodeResources'][1])
            else:
                cdhash = facts['cdhash']
                reject(files2[name] == {'cdhash': cdhash, 'requirement': 'cdhash H"' + cdhash.hex() + '"'})
