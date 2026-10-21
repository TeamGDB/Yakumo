"""Inspect unsigned Android payloads against the public application sources.

Resource layouts follow Android's public ResourceTypes.h and DEX format. This
is an independent bounded reader, not copied framework/parser implementation.
The outer ZIP and cryptographic signing boundary are enforced by the caller.
"""

import hashlib
import re
import struct
import zlib
from pathlib import Path
import xml.etree.ElementTree as ET

from nightly_formats import Rejected

ANDROID = 'http://schemas.android.com/apk/res/android'
PACKAGE = 'io.github.teamgdb.yakumo'
NONE = 0xffffffff
SDL_SHA = '1c040b8271b37e5076359f8fd54240e371114112924d2df81ef87c7d6a1dfdfd'
FFMPEG_SHA = '246041b6ecf9bc32d718a62c57877c78b5eb397b6467e74ed7ae2626ab189c30'
ATTR_IDS = {
    'theme': 0x1010000, 'label': 0x1010001, 'icon': 0x1010002, 'name': 0x1010003,
    'exported': 0x1010010, 'launchMode': 0x101001d, 'screenOrientation': 0x101001e,
    'configChanges': 0x101001f, 'drawable': 0x1010199, 'alwaysRetainTaskState': 0x1010203,
    'minSdkVersion': 0x101020c, 'versionCode': 0x101021b, 'versionName': 0x101021c,
    'targetSdkVersion': 0x1010270, 'allowBackup': 0x1010280, 'required': 0x101028e,
    'hardwareAccelerated': 0x10102d3, 'extractNativeLibs': 0x10104ea,
    'fullBackupContent': 0x10104eb, 'version': 0x1010519, 'roundIcon': 0x101052c,
    'compileSdkVersion': 0x1010572, 'compileSdkVersionCodename': 0x1010573,
    'preferMinimalPostProcessing': 0x101060c, 'dataExtractionRules': 0x101063e,
}
RESOURCE_KEYS = {'color': ['ic_launcher_background'],
                 'mipmap': ['ic_launcher', 'ic_launcher_foreground', 'ic_launcher_monochrome', 'ic_launcher_round'],
                 'string': ['app_name'], 'style': ['AppTheme'], 'xml': ['backup_rules', 'data_extraction_rules']}
RESOURCE_IDS = {f'@{kind}/{name}': 0x7f000000 | (number << 16) | index
                for number, (kind, names) in enumerate(RESOURCE_KEYS.items(), 1)
                for index, name in enumerate(names)}
DENSITIES = {160: 'mdpi', 240: 'hdpi', 320: 'xhdpi', 480: 'xxhdpi', 640: 'xxxhdpi'}


def require(condition):
    if not condition:
        raise Rejected('Android payload does not match the approved application policy.')


def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    require(0 <= start <= end <= len(data))
    while start < end:
        require(start + 8 <= end)
        kind, header, size = struct.unpack_from('<HHI', data, start)
        require(8 <= header <= size and size % 4 == 0 and start + size <= end)
        yield kind, header, data[start:start + size]
        start += size
    require(start == end)


def string_pool(data):
    require(len(data) >= 28 and struct.unpack_from('<HH', data) == (1, 28))
    count, styles, flags, start, style_start = struct.unpack_from('<5I', data, 8)
    require(0 < count <= 512 and styles == style_start == 0 and flags & ~0x101 == 0)
    require(start == 28 + count * 4 and start <= len(data))
    strings, cursor = [], start

    def length(position, wide):
        step, mask = (2, 0x8000) if wide else (1, 0x80)
        require(position + step <= len(data))
        value = int.from_bytes(data[position:position + step], 'little')
        position += step
        if value & mask:
            require(position + step <= len(data))
            value = ((value & (mask - 1)) << (step * 8)) | int.from_bytes(data[position:position + step], 'little')
            position += step
        require(value <= 4096)
        return value, position

    for index in range(count):
        offset = struct.unpack_from('<I', data, 28 + index * 4)[0]
        require(start + offset == cursor)
        units, cursor = length(cursor, not flags & 0x100)
        if flags & 0x100:
            size, cursor = length(cursor, False)
            require(cursor + size + 1 <= len(data) and data[cursor + size] == 0)
            value = data[cursor:cursor + size].decode('utf-8')
            cursor += size + 1
        else:
            size = units * 2
            require(cursor + size + 2 <= len(data) and data[cursor + size:cursor + size + 2] == b'\0\0')
            value = data[cursor:cursor + size].decode('utf-16-le')
            cursor += size + 2
        require(len(value.encode('utf-16-le')) // 2 == units and not any(ord(c) < 32 for c in value))
        strings.append(value)
    require(len(strings) == len(set(strings)) and len(data) - cursor <= 3 and not any(data[cursor:]))
    return strings


def binary_xml(data):
    require(8 <= len(data) <= 64 * 1024 and struct.unpack_from('<HHI', data) == (3, 8, len(data)))
    parts = list(chunks(data, 8))
    require(parts and parts[0][0] == 1)
    strings = string_pool(parts.pop(0)[2])
    used, ids, namespaces, stack, roots = set(), {}, [], [], []

    def text(index, optional=False):
        if optional and index == NONE:
            return ''
        require(index < len(strings))
        used.add(index)
        return strings[index]

    def qualified(ns, name):
        namespace, local = text(ns, True), text(name)
        require(not namespace or namespace == ANDROID)
        return '{' + namespace + '}' + local if namespace else local

    if parts and parts[0][0] == 0x180:
        _, header, resource_map = parts.pop(0)
        require(header == 8 and (len(resource_map) - 8) // 4 <= len(strings))
        for index in range((len(resource_map) - 8) // 4):
            value = struct.unpack_from('<I', resource_map, 8 + index * 4)[0]
            require(value == ATTR_IDS.get(strings[index]))
            ids[index] = value
    for kind, header, part in parts:
        require(header == 16 and len(part) >= 24 and struct.unpack_from('<I', part, 12)[0] == NONE)
        if kind in (0x100, 0x101):
            require(len(part) == 24)
            prefix, uri = struct.unpack_from('<II', part, 16)
            require((text(prefix), text(uri)) == ('android', ANDROID))
            if kind == 0x100:
                require(not namespaces and not roots and not stack)
                namespaces.append((prefix, uri))
            else:
                require(not stack and namespaces == [(prefix, uri)])
                namespaces.pop()
        elif kind == 0x102:
            require(len(part) >= 36)
            ns, name, offset, stride, count, id_index, class_index, style_index = struct.unpack_from('<II6H', part, 16)
            require(offset == stride == 20 and count <= 32 and id_index == class_index == style_index == 0)
            require(len(part) == 36 + count * 20 and len(stack) < 16)
            attrs = {}
            for index in range(count):
                an, key, raw, size, zero, value_type, value = struct.unpack_from('<IIIHBBI', part, 36 + index * 20)
                require(size == 8 and zero == 0 and value_type in (1, 3, 0x10, 0x11, 0x12))
                attr_name = qualified(an, key)
                require(attr_name not in attrs and (an == NONE or key in ids))
                if value_type == 3:
                    value = text(value)
                if raw != NONE:
                    require(value_type == 3 and text(raw) == value)
                attrs[attr_name] = (value_type, value)
            node = [qualified(ns, name), attrs, []]
            if stack:
                stack[-1][2].append(node)
            else:
                require(not roots)
                roots.append(node)
            stack.append(node)
        elif kind == 0x103:
            require(len(part) == 24 and stack)
            ns, name = struct.unpack_from('<II', part, 16)
            require(qualified(ns, name) == stack[-1][0])
            stack.pop()
        else:
            raise Rejected('Unexpected Android XML chunk.')
    require(len(roots) == 1 and not stack and not namespaces and used == set(range(len(strings))))
    return roots[0]


def source_tree(element):
    attrs = {}
    for key, value in element.attrib.items():
        local = key.split('}')[-1]
        if value.startswith('@'):
            require(value in RESOURCE_IDS)
            typed = (1, RESOURCE_IDS[value])
        elif value in ('true', 'false'):
            typed = (0x12, NONE if value == 'true' else 0)
        elif local == 'launchMode':
            require(value == 'singleInstance')
            typed = (0x10, 3)
        elif local == 'screenOrientation':
            require(value == 'sensorLandscape')
            typed = (0x10, 6)
        elif local == 'configChanges':
            require(set(value.split('|')) == set('density|layoutDirection|locale|grammaticalGender|fontScale|fontWeightAdjustment|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation'.split('|')))
            typed = (0x11, 0x5000bff4)
        elif local == 'version':
            typed = (0x11, int(value, 0))
        else:
            typed = (3, value)
        attrs[key] = typed
    return [element.tag, attrs, [source_tree(child) for child in element]]


def validate_manifest(tree, root, version):
    require(tree[0] == 'manifest')
    attrs = dict(tree[1])
    android = lambda name: '{' + ANDROID + '}' + name
    expected = source_tree(ET.parse(root / 'profiles/mhp3rd/packaging/android/AndroidManifest.xml').getroot())
    code = attrs.get(android('versionCode'))
    require(code is not None and code[0] == 0x10 and isinstance(code[1], int) and 0 < code[1] < 2 ** 31)
    expected[1].update({android('versionCode'): code, android('versionName'): (3, version),
                        android('compileSdkVersion'): (0x10, 35),
                        android('compileSdkVersionCodename'): (3, '15'),
                        'platformBuildVersionCode': (0x10, 35), 'platformBuildVersionName': (0x10, 15)})
    expected[2].insert(0, ['uses-sdk', {android('minSdkVersion'): (0x10, 29),
                                      android('targetSdkVersion'): (0x10, 35)}, []])
    require(tree == expected and expected[1]['package'] == (3, PACKAGE))


def validate_binary(data, library=True):
    require(len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01')
    kind, machine, version = struct.unpack_from('<HHI', data, 16)
    require(machine == 183 and version == 1 and kind in ((3,) if library else (2, 3)))
    phoff, shoff = struct.unpack_from('<QQ', data, 32)
    ehsize, phsize, phcount, shsize, shcount, _ = struct.unpack_from('<6H', data, 52)
    require(ehsize == 64 and 0 < phcount <= 256 and phsize == 56 and phoff >= 64)
    require(phoff + phsize * phcount <= len(data) and shsize in (0, 64) and shcount <= 4096)
    require(not shcount or shsize == 64 and shoff >= 64 and shoff + shcount * shsize <= len(data))
    for index in range(phcount):
        entry = phoff + phsize * index
        offset, size = struct.unpack_from('<Q', data, entry + 8)[0], struct.unpack_from('<Q', data, entry + 32)[0]
        require(offset <= len(data) and size <= len(data) - offset)


def validate_dex(data):
    require(112 <= len(data) <= 16 * 1024 * 1024 and data[:8] in (b'dex\n035\0', b'dex\n037\0', b'dex\n038\0', b'dex\n039\0', b'dex\n040\0'))
    require(struct.unpack_from('<3I', data, 32) == (len(data), 112, 0x12345678))
    require(struct.unpack_from('<2I', data, 44) == (0, 0))
    # SHA-1 is mandated by the DEX file format; this checks structure/integrity,
    # not publisher identity, which the separate APK signing job establishes.
    require(data[12:32] == hashlib.sha1(data[32:], usedforsecurity=False).digest())
    require(struct.unpack_from('<I', data, 8)[0] == zlib.adler32(data[12:]) & NONE)
    for offset, width in [(56, 4), (64, 4), (72, 12), (80, 8), (88, 8), (96, 32), (104, 1)]:
        count, start = struct.unpack_from('<2I', data, offset)
        require((count == start == 0) or 112 <= start <= len(data) and count <= (len(data) - start) // width)
    map_offset = struct.unpack_from('<I', data, 52)[0]
    require(112 <= map_offset <= len(data) - 4)
    count = struct.unpack_from('<I', data, map_offset)[0]
    require(0 < count <= 128 and map_offset + 4 + count * 12 <= len(data))
    data_size, data_offset = struct.unpack_from('<2I', data, 104)
    require(data_offset + data_size == len(data))


def resource_table(data, root, resources):
    require(12 <= len(data) <= 64 * 1024 and struct.unpack_from('<HHII', data) == (2, 12, len(data), 1))
    top = list(chunks(data, 12))
    require(len(top) == 2 and top[0][0] == 1 and top[1][:2] == (0x200, 288))
    values = string_pool(top[0][2])
    label = ET.parse(root / 'profiles/mhp3rd/packaging/android/res/values/strings.xml').find("string[@name='app_name']").text
    require(set(values) == {label, *resources})
    package = top[1][2]
    require(struct.unpack_from('<I', package, 8)[0] == 127)
    require(package[12:268] == (PACKAGE.encode('utf-16-le') + bytes(256))[:256])
    type_start, public_type, key_start, public_key, id_offset = struct.unpack_from('<5I', package, 268)
    require(type_start == 288 and public_type == public_key == id_offset == 0)
    parts = list(chunks(package, 288))
    require(len(parts) >= 2 and parts[0][0] == parts[1][0] == 1)
    types, keys = string_pool(parts[0][2]), string_pool(parts[1][2])
    require(types == list(RESOURCE_KEYS) and keys == [name for names in RESOURCE_KEYS.values() for name in names])
    require(key_start == 288 + len(parts[0][2]))
    seen_specs, seen_values = set(), set()
    color = ET.parse(root / 'profiles/mhp3rd/packaging/android/res/values/ic_launcher_background.xml').find('color').text
    require(re.fullmatch('#[0-9a-fA-F]{6}', color) is not None)
    style = ET.parse(root / 'profiles/mhp3rd/packaging/android/res/values/strings.xml').find('style')
    require(style.attrib == {'name': 'AppTheme', 'parent': 'android:Theme.NoTitleBar.Fullscreen'}
            and len(style) == 1 and style[0].attrib == {'name': 'android:windowLayoutInDisplayCutoutMode'}
            and style[0].text == 'shortEdges')
    for kind, header, part in parts[2:]:
        require(kind in (0x201, 0x202) and len(part) >= 16)
        type_id, flags, zero, count = struct.unpack_from('<BBHI', part, 8)
        require(1 <= type_id <= len(types) and flags == 0)
        resource_type = types[type_id - 1]
        names = RESOURCE_KEYS[resource_type]
        require(count == len(names))
        if kind == 0x202:
            require(header == 16 and len(part) == 16 + count * 4 and type_id not in seen_specs)
            require(zero == (6 if resource_type == 'mipmap' else 1))
            require(list(struct.unpack_from('<' + 'I' * count, part, 16)) == [256 if resource_type == 'mipmap' else 0] * count)
            seen_specs.add(type_id)
            continue
        require(zero == 0 and header >= 48 and header <= 84 and len(part) >= header + count * 4)
        entry_start = struct.unpack_from('<I', part, 16)[0]
        config = bytearray(part[20:header])
        require(struct.unpack_from('<I', config)[0] == len(config))
        density = struct.unpack_from('<H', config, 14)[0]
        config[:4], config[14:16] = bytes(4), bytes(2)
        require(not any(config) and density in ({*DENSITIES, 65534} if resource_type == 'mipmap' else {0}))
        require(entry_start == header + count * 4)
        cursor = entry_start
        for index, name in enumerate(names):
            relative = struct.unpack_from('<I', part, header + index * 4)[0]
            if relative == NONE:
                require(resource_type == 'mipmap' and density == 65534 and name in ('ic_launcher_foreground', 'ic_launcher_monochrome'))
                continue
            require(entry_start + relative == cursor and cursor + 16 <= len(part))
            size, entry_flags, key_index = struct.unpack_from('<HHI', part, cursor)
            require(key_index < len(keys) and keys[key_index] == name)
            identity = (resource_type, name, density)
            require(identity not in seen_values)
            seen_values.add(identity)
            if resource_type == 'style':
                require(size == 16 and entry_flags == 1 and cursor + 28 <= len(part))
                require(struct.unpack_from('<3I', part, cursor + 8) == (0x01030007, 1, 0x01010586))
                require(part[cursor + 20:cursor + 28] == struct.pack('<HBBI', 8, 0, 0x10, 1))
                cursor += 28
                continue
            require(size == 8 and entry_flags == 0)
            value_size, reserved, value_type, value = struct.unpack_from('<HBBI', part, cursor + 8)
            require(value_size == 8 and reserved == 0)
            if resource_type == 'color':
                require(value_type == 0x1d and value == (0xff000000 | int(color[1:], 16)))
            else:
                require(value_type == 3 and value < len(values))
                expected = label if resource_type == 'string' else (
                    f'res/xml/{name}.xml' if resource_type == 'xml' else
                    f'res/mipmap-anydpi-v26/{name}.xml' if density == 65534 else
                    f'res/mipmap-{DENSITIES[density]}-v4/{name}.png')
                require(values[value] == expected)
            cursor += 16
        require(cursor == len(part))
    expected_values = {(kind, name, density) for kind, names in RESOURCE_KEYS.items() for name in names
                       for density in (DENSITIES if kind == 'mipmap' else [0])}
    expected_values |= {('mipmap', name, 65534) for name in ('ic_launcher', 'ic_launcher_round')}
    require(seen_specs == set(range(1, 6)) and seen_values == expected_values)


def pin(root, name):
    text = (root / 'profiles/mhp3rd/packaging/linux/sources.sh').read_text()
    match = re.search(r'^' + re.escape(name) + '=([0-9a-f]{64})$', text, re.M)
    require(match is not None)
    return match[1]


def ffmpeg_notice(root):
    text = (root / 'profiles/mhp3rd/cmake/FFmpeg.cmake').read_text()
    version = re.search(r'set\(MHP3RD_FFMPEG_VERSION\s+([^\s)]+)', text)
    checksum = re.search(r'set\(MHP3RD_FFMPEG_SHA256\s+"?([0-9a-f]{64})', text)
    flags = re.search(r'set\(MHP3RD_FFMPEG_CONFIGURE_FLAGS\s+(.*?)\)', text, re.S)
    require(version is not None and checksum is not None and flags is not None)
    version, flags = version[1].strip('"'), ' '.join(flags[1].split())
    return (f'The FFmpeg libraries here (libavcodec, libavutil) are FFmpeg {version},\n'
            'unmodified, licensed under the GNU LGPL version 2.1 or later\n'
            '(FFmpeg-COPYING.LGPLv2.1.txt). They are dynamically linked.\n\n'
            f'Source: https://ffmpeg.org/releases/ffmpeg-{version}.tar.xz\nSHA-256: {checksum[1]}\n'
            f'Configured with: {flags}\n').encode()


class AndroidPolicy:
    def __init__(self, root, version, source_sha):
        self.root, self.version = Path(root), version
        require(re.fullmatch('[A-Za-z0-9._-]+', version) is not None and re.fullmatch('[0-9a-f]{40}', source_sha) is not None)
        self.seen, self.overlays, self.xml, self.table = set(), set(), {}, None
        res = self.root / 'profiles/mhp3rd/packaging/android/res'
        self.resources = {}
        for file in sorted(res.rglob('*')):
            if file.is_file() and not file.relative_to(res).as_posix().startswith('values/'):
                name = file.relative_to(res).as_posix()
                name = re.sub(r'^mipmap-(mdpi|hdpi|xhdpi|xxhdpi|xxxhdpi)/', r'mipmap-\1-v4/', name)
                self.resources['res/' + name] = file
        require(len(self.resources) == 24)
        self.fixed = {'assets/licenses/Yakumo-LICENSE.txt': self.root / 'LICENSE',
                      'assets/licenses/THIRD_PARTY_NOTICES.md': self.root / 'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
                      'assets/licenses/DearImGui-LICENSE.txt': self.root / 'profiles/mhp3rd/third_party/imgui/LICENSE.txt',
                      'assets/licenses/tiny-AES-c-UNLICENSE.txt': self.root / 'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE',
                      'assets/licenses/xxHash-LICENSE.txt': self.root / 'profiles/mhp3rd/third_party/xxhash/LICENSE',
                      'assets/licenses/AsmJit-LICENSE.md': self.root / 'third_party/asmjit/LICENSE.md',
                      'assets/licenses/Luau-LICENSE.txt': self.root / 'profiles/mhp3rd/third_party/luau/LICENSE.txt',
                      'assets/licenses/Luau-Lua-LICENSE.txt': self.root / 'profiles/mhp3rd/third_party/luau/lua_LICENSE.txt'}
        self.hashes = {'assets/fonts/NotoSansCJKjp-Regular.otf': pin(self.root, 'NOTO_CJK_SHA256'),
                       'assets/licenses/NotoSansCJK-OFL.txt': pin(self.root, 'NOTO_CJK_LICENSE_SHA256'),
                       'assets/licenses/SDL3-LICENSE.txt': SDL_SHA,
                       'assets/licenses/FFmpeg-COPYING.LGPLv2.1.txt': FFMPEG_SHA}

    def check(self, name, data):
        require(name not in self.seen)
        self.seen.add(name)
        if re.fullmatch(r'lib/arm64-v8a/libovl[A-Za-z0-9_]+\.so', name):
            validate_binary(data)
            self.overlays.add(name)
        elif name in {f'lib/arm64-v8a/{x}.so' for x in ['libmain', 'libSDL3', 'libavcodec', 'libavutil']}:
            validate_binary(data)
        elif name == 'classes.dex':
            validate_dex(data)
        elif name == 'AndroidManifest.xml' or name in self.resources and name.endswith('.xml'):
            self.xml[name] = binary_xml(data)
        elif name == 'resources.arsc':
            resource_table(data, self.root, self.resources)
            self.table = True
        elif name in self.resources:
            require(data == self.resources[name].read_bytes())
        elif name in self.fixed:
            require(data == self.fixed[name].read_bytes())
        elif name in self.hashes:
            require(hashlib.sha256(data).hexdigest() == self.hashes[name])
        elif name == 'assets/licenses/FFmpeg-SOURCE.txt':
            require(data == ffmpeg_notice(self.root))
        else:
            raise Rejected('Unexpected Android APK member.')

    def finish(self, names):
        required = {'AndroidManifest.xml', 'resources.arsc', 'classes.dex', *self.resources, *self.fixed, *self.hashes,
                    'assets/licenses/FFmpeg-SOURCE.txt',
                    *(f'lib/arm64-v8a/{x}.so' for x in ['libmain', 'libSDL3', 'libavcodec', 'libavutil'])}
        require(names == self.seen == required | self.overlays and len(self.overlays) == 355 and self.table)
        validate_manifest(self.xml['AndroidManifest.xml'], self.root, self.version)
        for name, file in self.resources.items():
            if name.endswith('.xml'):
                require(self.xml[name] == source_tree(ET.parse(file).getroot()))
