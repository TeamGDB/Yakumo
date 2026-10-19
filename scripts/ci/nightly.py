#!/usr/bin/env python3
"""Private native build inputs and a fail-closed nightly publication boundary.

Only the validated staging boundary writes distributable files. Private commands deliberately do
not forward subprocess output, paths, response bodies or exception details.
"""

import argparse
import contextlib
import gzip
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tarfile
import traceback
from pathlib import Path, PurePosixPath

from nightly_formats import Rejected, PLATFORMS

ROOT = Path(__file__).resolve().parents[2]
INPUT_BUCKET = 'yakumo-build-inputs'
CACHE_BUCKET = 'yakumo-build-cache'
MAX_PART = 128 * 1024 * 1024
MAX_INPUT = 8 * 1024 ** 3
MAX_PACKAGE = 2 * 1024 ** 3
MAX_EXPANDED = 4 * 1024 ** 3
EXPECTED_EBOOT = '55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c'
TOOLCHAINS = {'linux-x86_64': 'steamrt-sniper-gcc14', 'macos-arm64': 'libcxx1-macos13',
              'windows-x86_64': 'msvc14.44', 'android-arm64': 'ndk28.2-api29'}
# Public license texts from the pinned SDL3/FFmpeg upstream archives. A changed
# license requires review, never accepting arbitrary UTF-8 as a license file.
SDL_LICENSE_SHA = '1c040b8271b37e5076359f8fd54240e371114112924d2df81ef87c7d6a1dfdfd'
FFMPEG_LICENSE_SHA = '246041b6ecf9bc32d718a62c57877c78b5eb397b6467e74ed7ae2626ab189c30'
ABI_PATHS = ['include/psprecomp', 'CMakeLists.txt', 'profiles/mhp3rd/CMakeLists.txt',
             'profiles/mhp3rd/host/mhp3rd_profile.hpp', 'profiles/mhp3rd/host/overlay_module.hpp',
             'profiles/mhp3rd/host/overlay_module.cpp.in', 'tools/codegen_main.cpp',
             'src/decoder.cpp', 'src/elf32.cpp', 'src/program_analysis.cpp',
             'profiles/mhp3rd/tools/add_overlay.py', 'profiles/mhp3rd/tools/wrap_overlay.py',
             'profiles/mhp3rd/tools/databin.py', 'profiles/mhp3rd/scripts/build_overlays.sh',
             'profiles/mhp3rd/packaging/linux/sources.sh']
BAD_PART = re.compile(r'(?i)^(?:eboot.*|data\.bin|param\.sfo|umd_data.*|ms0|savedata|generated|'
                      r'analysis|overlay_corpora|game|.*\.(?:iso|cso|pbp|prx|elf|bin|ovl|cpp|o|lang))$')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def sha(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise Rejected('Invalid checksum.')
    return value


def commit(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{40}', value):
        raise Rejected('Invalid source commit.')
    return value


def key(value):
    if not isinstance(value, str) or len(value) > 512 or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_./-]*', value):
        raise Rejected('Invalid private object key.')
    if any(p in ('', '.', '..') for p in value.split('/')):
        raise Rejected('Invalid private object key.')
    return value


def regular(path):
    path = Path(path)
    if not path.is_file() or path.is_symlink():
        raise Rejected('A regular input file is required.')
    return path


def fresh(path):
    path = Path(path)
    if path.exists() or path.is_symlink():
        raise Rejected('Output directory must not exist.')
    path.mkdir(parents=True, mode=0o700)
    return path


def pinned(name, root=ROOT):
    text = (root / 'profiles/mhp3rd/packaging/linux/sources.sh').read_text()
    match = re.search(r'^' + re.escape(name) + r'=([^\n]+)$', text, re.M)
    if not match:
        raise Rejected('Missing packaging pin.')
    return match[1].strip('"')


def ffmpeg_pin(root=ROOT):
    text = (root / 'profiles/mhp3rd/cmake/FFmpeg.cmake').read_text()
    version = re.search(r'set\(MHP3RD_FFMPEG_VERSION\s+([^\s)]+)', text)
    checksum = re.search(r'set\(MHP3RD_FFMPEG_SHA256\s+"?([0-9a-f]{64})', text)
    if not version or not checksum:
        raise Rejected('Missing FFmpeg source pin.')
    return 'ffmpeg-' + version[1].strip('"') + '.tar.xz', checksum[1]


def abi_fingerprint(root=ROOT, source=None):
    if source:
        listing = ['ls-tree', '-r', '--name-only', commit(source)]
    else:
        # Local opt-in packaging must also reject changed or newly added ABI
        # inputs that have not been committed. CI uses a clean pinned checkout.
        listing = ['ls-files', '--cached', '--others', '--exclude-standard']
    paths = subprocess.check_output(['git', '-C', str(root), *listing, '--', *ABI_PATHS],
                                     stderr=subprocess.DEVNULL).decode().splitlines()
    if not paths:
        raise Rejected('Missing overlay compatibility inputs.')
    h = hashlib.sha256()
    for path in sorted(set(paths)):
        if source:
            content = subprocess.check_output(['git', '-C', str(root), 'show', source + ':' + path],
                                              stderr=subprocess.DEVNULL)
        else:
            content = regular(root / path).read_bytes()
        # Compare source text independently of Git's checkout line endings.
        # Preserve every other byte so real ABI/generator edits still invalidate.
        content = content.replace(b'\r\n', b'\n')
        h.update(path.encode() + b'\0' + hashlib.sha256(content).digest())
    return h.hexdigest()


def native_elf(head, library=False):
    if len(head) < 64 or head[:7] != b'\x7fELF\x02\x01\x01':
        raise Rejected('A package contains an unexpected binary format.')
    kind, machine = struct.unpack_from('<HH', head, 16)
    if machine != 62 or kind not in ((3,) if library else (2, 3)):
        raise Rejected('A package contains an unexpected binary architecture.')


def safe_member(member, prefix):
    name = member.name.rstrip('/')
    parts = name.split('/')
    if (not parts or parts[0] != prefix or any(p in ('', '.', '..') for p in parts)
            or '\\' in name or any(ord(c) < 32 for c in name)
            or any(BAD_PART.fullmatch(p) for p in parts)):
        raise Rejected('A package contains a forbidden member.')
    if not (member.isdir() or member.isfile()):
        raise Rejected('Package links and special files are forbidden.')
    if member.pax_headers or member.offset_data - member.offset != 512:
        raise Rejected('Extended archive metadata is not accepted by this publication policy.')
    return '/'.join(parts[1:])


@contextlib.contextmanager
def canonical_tar(path):
    with Path(path).open('rb') as source:
        header = source.read(10)
    if len(header) != 10 or header[:4] != b'\x1f\x8b\x08\x00' or any(header[4:8]):
        raise Rejected('Only metadata-free reproducible gzip archives are accepted.')
    with gzip.open(path, 'rb') as stream, tarfile.open(fileobj=stream, mode='r:') as archive:
        yield archive
        # Tar parsers normally stop at the first end marker. Also reject hidden
        # trailing data, concatenated archives, and nonzero extended padding.
        stream.seek(archive.offset)
        padding = stream.read(10241)
        if len(padding) > 10240 or len(padding) < 1024 or any(padding):
            raise Rejected('Unexpected trailing archive data.')


def ffmpeg_notice(root):
    text = (root / 'profiles/mhp3rd/cmake/FFmpeg.cmake').read_text()
    name, checksum = ffmpeg_pin(root)
    version = name[7:-7]
    match = re.search(r'set\(MHP3RD_FFMPEG_CONFIGURE_FLAGS\s+(.*?)\)', text, re.S)
    if not match:
        raise Rejected('Missing FFmpeg configuration pin.')
    flags = ' '.join(match[1].split())
    return (f'The FFmpeg libraries here (libavcodec, libavutil) are FFmpeg {version},\n'
            'unmodified, licensed under the GNU LGPL version 2.1 or later\n'
            '(FFmpeg-COPYING.LGPLv2.1.txt). They are dynamically linked.\n\n'
            f'Source: https://ffmpeg.org/releases/{name}\nSHA-256: {checksum}\n'
            f'Configured with: {flags}\n').encode()


def inspect_linux(path, root=ROOT):
    """Scan the real archive, never trusting a producer's file-list manifest."""
    path = regular(path)
    if not re.fullmatch(r'yakumo-[A-Za-z0-9._-]+-linux-x86_64\.tar\.gz', path.name):
        raise Rejected('Unexpected package name.')
    if path.stat().st_size > MAX_PACKAGE:
        raise Rejected('Package exceeds the inspection limit.')
    prefix = path.name[:-7]
    seen = set()
    overlays = set()
    total = 0
    fixed = {
        'yakumo': root / 'profiles/mhp3rd/packaging/linux/yakumo.sh',
        'README.txt': root / 'profiles/mhp3rd/packaging/linux/README.txt',
        'licenses/Yakumo-LICENSE.txt': root / 'LICENSE',
        'licenses/THIRD_PARTY_NOTICES.md': root / 'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
        'licenses/DearImGui-LICENSE.txt': root / 'profiles/mhp3rd/third_party/imgui/LICENSE.txt',
        'licenses/tiny-AES-c-UNLICENSE.txt': root / 'profiles/mhp3rd/third_party/tiny_aes/UNLICENSE',
        'licenses/xxHash-LICENSE.txt': root / 'profiles/mhp3rd/third_party/xxhash/LICENSE',
        'licenses/AsmJit-LICENSE.md': root / 'third_party/asmjit/LICENSE.md',
        'licenses/Luau-LICENSE.txt': root / 'profiles/mhp3rd/third_party/luau/LICENSE.txt',
        'licenses/Luau-Lua-LICENSE.txt': root / 'profiles/mhp3rd/third_party/luau/lua_LICENSE.txt',
    }
    licenses = {'SDL3-LICENSE.txt', 'FFmpeg-COPYING.LGPLv2.1.txt', 'FFmpeg-SOURCE.txt',
                'DearImGui-LICENSE.txt', 'tiny-AES-c-UNLICENSE.txt', 'xxHash-LICENSE.txt', 'NotoSansCJK-OFL.txt'}
    with canonical_tar(path) as archive:
        for index, member in enumerate(archive):
            if index >= 1000:
                raise Rejected('Too many package members.')
            relative = safe_member(member, prefix)
            if relative in seen:
                raise Rejected('Duplicate package member.')
            seen.add(relative)
            if member.isdir():
                if relative not in ('', 'lib', 'overlays', 'fonts', 'licenses'):
                    raise Rejected('Unexpected package directory.')
                continue
            total += member.size
            if member.size < 0 or total > MAX_EXPANDED:
                raise Rejected('Expanded package exceeds its limit.')
            with archive.extractfile(member) as stream:
                head = stream.read(64)
                if relative == 'Yakumo':
                    native_elf(head)
                elif re.fullmatch(r'(?:overlays|lib)/[A-Za-z0-9_.-]+\.so(?:\.[0-9]+)*', relative):
                    native_elf(head, library=True)
                    if relative.startswith('overlays/'):
                        overlays.add(relative)
                elif relative in fixed:
                    if member.size > 256 * 1024 or head + stream.read() != fixed[relative].read_bytes():
                        raise Rejected('Package documentation does not match its public source.')
                elif relative == 'fonts/NotoSansCJKjp-Regular.otf':
                    h = hashlib.sha256(head)
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        h.update(block)
                    if h.hexdigest() != pinned('NOTO_CJK_SHA256', root):
                        raise Rejected('Unexpected font payload.')
                elif relative.startswith('licenses/') and relative[9:] in licenses:
                    if member.size > 256 * 1024:
                        raise Rejected('Unexpected license payload.')
                    content = head + stream.read()
                    checksums = {'SDL3-LICENSE.txt': SDL_LICENSE_SHA,
                                 'FFmpeg-COPYING.LGPLv2.1.txt': FFMPEG_LICENSE_SHA,
                                 'NotoSansCJK-OFL.txt': pinned('NOTO_CJK_LICENSE_SHA256', root)}
                    if relative == 'licenses/FFmpeg-SOURCE.txt':
                        valid = content == ffmpeg_notice(root)
                    else:
                        valid = hashlib.sha256(content).hexdigest() == checksums.get(relative[9:])
                    if not valid:
                        raise Rejected('Unexpected license payload.')
                else:
                    raise Rejected('A package contains a file outside the publication allowlist.')
    required = {'Yakumo', *fixed, 'fonts/NotoSansCJKjp-Regular.otf',
                *('licenses/' + name for name in licenses)}
    if not required.issubset(seen) or len(overlays) != 355 or not any(x.startswith('lib/') for x in seen):
        raise Rejected('Incomplete Linux distribution.')


def source_pin(platform, root=ROOT):
    if platform != 'windows-x86_64':
        return ffmpeg_pin(root)
    text = (root / 'scripts/ci/nightly_windows_sources.sh').read_text()
    values = {}
    for name in ('NAME', 'SHA256'):
        match = re.search(r'^NIGHTLY_WINDOWS_FFMPEG_SOURCE_' + name + r'=["\']?([^\n"\']+)', text, re.M)
        if not match:
            raise Rejected('Missing corresponding Windows FFmpeg source pin.')
        values[name] = match[1]
    sha(values['SHA256'])
    if not re.fullmatch(r'ffmpeg-[0-9a-f]{40}\.tar\.gz', values['NAME']):
        raise Rejected('Invalid corresponding source name.')
    return values['NAME'], values['SHA256']


def stage_public(package, source_archive, output, source_sha, root=ROOT, platform='linux-x86_64'):
    source_sha = commit(source_sha)
    package, source_archive = regular(package), regular(source_archive)
    name, expected = source_pin(platform, root)
    if source_archive.name != name or digest(source_archive) != expected:
        raise Rejected('FFmpeg source does not match its pinned public archive.')
    if platform == 'linux-x86_64':
        inspect_linux(package, root)
    # Validation happens before the first copy into the publication directory.
    output = fresh(output)
    try:
        files = []
        for path in (package, source_archive):
            target = output / path.name
            if path == package and platform != 'linux-x86_64':
                import nightly_formats
                nightly_formats.canonical_package(path, target, platform, root, source_sha)
            else:
                shutil.copyfile(path, target)
            files.append({'name': target.name, 'sha256': digest(target), 'size': target.stat().st_size})
        (output / 'SHA256SUMS').write_text(''.join(f"{x['sha256']}  {x['name']}\n" for x in files))
        (output / 'build.json').write_text(json.dumps({'schema': 1, 'source': source_sha,
            'platform': platform, 'files': files,
            'android_signing': 'required' if platform == 'android-arm64' else 'not-applicable'}, indent=2) + '\n')
        notes = ('> **Experimental nightly build, not a stable release. Back up your saves and settings.**\n\n'
                 'For regular play use https://github.com/TeamGDB/Yakumo/releases/latest.\n\n'
                 'Source: https://github.com/TeamGDB/Yakumo/commit/' + source_sha + '\n\n'
                 'Platform: ' + platform + '.\n\n'
                 'Packaging and privacy checks passed. Full device/gameplay testing is not claimed. '
                 'Use SHA256SUMS to verify downloads. No game files are included.\n')
        (output / 'release-notes.md').write_text(notes)
    except Exception:
        shutil.rmtree(output)
        raise


def r2_client():
    # The administrative cf object endpoint needs account-wide permissions.
    # The S3 data plane supports a token limited to these exact two buckets.
    import boto3
    from botocore.config import Config

    account = os.environ.get('R2_ACCOUNT_ID', '')
    access = os.environ.get('R2_ACCESS_KEY_ID', '')
    secret = os.environ.get('R2_SECRET_ACCESS_KEY', '')
    if (not re.fullmatch('[0-9a-f]{32}', account) or not re.fullmatch('[0-9a-f]{32}', access)
            or not re.fullmatch('[0-9a-f]{64}', secret)):
        raise Rejected('Missing scoped private-storage credentials.')
    return boto3.client('s3', endpoint_url=f'https://{account}.r2.cloudflarestorage.com',
                        aws_access_key_id=access, aws_secret_access_key=secret, region_name='auto',
                        config=Config(signature_version='s3v4', connect_timeout=10, read_timeout=60,
                                      retries={'total_max_attempts': 2}, s3={'addressing_style': 'path'}))


def r2_get(bucket, object_key, destination, limit, expected=None):
    if bucket not in (INPUT_BUCKET, CACHE_BUCKET):
        raise Rejected('Unexpected private bucket.')
    key(object_key)
    destination = Path(destination)
    if destination.exists() or destination.is_symlink():
        raise Rejected('Private download destination already exists.')
    if not isinstance(limit, int) or not 0 < limit <= MAX_PACKAGE:
        raise Rejected('Invalid private download limit.')
    if expected is not None:
        sha(expected)
    response = r2_client().get_object(Bucket=bucket, Key=object_key)
    with contextlib.closing(response['Body']) as body:
        length = response['ContentLength']
        if not isinstance(length, int) or not 0 <= length <= limit:
            raise Rejected('Private object exceeds its expected length.')
        with destination.open('xb') as stream:
            os.chmod(destination, 0o600)
            size = 0
            for block in iter(lambda: body.read(min(1024 * 1024, limit - size + 1)), b''):
                size += len(block)
                if size > limit:
                    raise Rejected('Private object exceeds its download limit.')
                stream.write(block)
    if size != length or (expected is not None and digest(destination) != expected):
        raise Rejected('Private object download or checksum verification failed.')
    return size


def extract_overlays(archive_path, destination):
    """Read only native overlays from an approved, checksum-pinned cache seed."""
    if not re.fullmatch(r'yakumo-[A-Za-z0-9._-]+-linux-x86_64\.tar\.gz', archive_path.name):
        raise Rejected('Invalid overlay cache name.')
    prefix = archive_path.name[:-7]
    seen = set()
    total = 0
    with canonical_tar(archive_path) as archive:
        for index, member in enumerate(archive):
            if index >= 1000:
                raise Rejected('Too many cache members.')
            relative = safe_member(member, prefix)
            total += member.size
            if total > MAX_EXPANDED or member.size < 0:
                raise Rejected('Expanded overlay cache exceeds its limit.')
            if not member.isfile() or not relative.startswith('overlays/'):
                continue
            if not re.fullmatch(r'overlays/[A-Za-z0-9_.-]+\.so', relative) or relative in seen:
                raise Rejected('Invalid or duplicate cached overlay.')
            if not 64 <= member.size <= 128 * 1024 * 1024:
                raise Rejected('Invalid cached overlay size.')
            seen.add(relative)
            with archive.extractfile(member) as source:
                head = source.read(64)
                native_elf(head, library=True)
                target = destination / PurePosixPath(relative).name
                with target.open('xb') as output:
                    output.write(head)
                    shutil.copyfileobj(source, output)
    if len(seen) != 355:
        raise Rejected('Overlay cache is incomplete.')


def fetch_inputs(destination, manifest_key, root=ROOT, platform='linux-x86_64'):
    if platform not in PLATFORMS:
        raise Rejected('Unsupported build platform.')
    destination = fresh(destination)
    try:
        manifest_path = destination / 'manifest.json'
        r2_get(INPUT_BUCKET, key(manifest_key), manifest_path, 1024 * 1024)
        manifest = json.loads(manifest_path.read_text())
        if not isinstance(manifest, dict) or manifest.get('schema') not in (1, 2):
            raise Rejected('Unsupported private input manifest.')
        entries = manifest['files']
        if len(entries) != 2 or {x['name'] for x in entries} != {'disc.iso', 'EBOOT.ELF'}:
            raise Rejected('Unexpected private input set.')
        for entry in entries:
            size = entry['size']
            sha(entry['sha256'])
            if not isinstance(size, int) or not 0 < size <= MAX_INPUT or not 0 < len(entry['parts']) <= 128:
                raise Rejected('Invalid private input size.')
            target = destination / entry['name']
            with target.open('xb') as final:
                os.chmod(target, 0o600)
                total = 0
                for part in entry['parts']:
                    part_size = part['size']
                    if not isinstance(part_size, int) or not 0 < part_size <= MAX_PART:
                        raise Rejected('Invalid private part size.')
                    partial = destination / 'part.tmp'
                    got = r2_get(INPUT_BUCKET, part['key'], partial, part_size, part['sha256'])
                    total += got
                    if got != part_size or total > size:
                        raise Rejected('Private input length mismatch.')
                    with partial.open('rb') as source:
                        shutil.copyfileobj(source, final)
                    partial.unlink()
            if total != size or digest(target) != entry['sha256']:
                raise Rejected('Private input checksum mismatch.')
        if digest(destination / 'EBOOT.ELF') != EXPECTED_EBOOT:
            raise Rejected('Unsupported game executable.')
        if manifest['schema'] == 1:
            if platform != 'linux-x86_64':
                raise Rejected('Missing platform cache in the legacy manifest.')
            cache = manifest['overlay_cache']
        else:
            catalog = manifest.get('overlay_caches')
            if not isinstance(catalog, dict) or platform not in catalog:
                raise Rejected('Missing native platform cache.')
            cache = catalog[platform]
            if cache.get('toolchain') != TOOLCHAINS[platform]:
                raise Rejected('Unsupported native cache toolchain contract.')
        filename = cache['filename']
        import nightly_formats
        nightly_formats.version_from_name(platform, filename)
        if (cache['abi_sha256'] != abi_fingerprint(root)
                or abi_fingerprint(root, commit(cache['source_commit'])) != cache['abi_sha256']):
            raise Rejected('Overlay cache must be refreshed for this source.')
        if not isinstance(cache['size'], int) or not 0 < cache['size'] <= MAX_PACKAGE:
            raise Rejected('Invalid overlay cache size.')
        archive_path = destination / 'overlays.tar.gz'
        if r2_get(CACHE_BUCKET, cache['key'], archive_path, cache['size'], cache['sha256']) != cache['size']:
            raise Rejected('Incomplete overlay cache.')
        # The seed is an already-published, fully checked Linux distribution.
        archive_path.rename(destination / filename)
        archive_path = destination / filename
        overlay_dir = destination / 'overlays'
        overlay_dir.mkdir()
        if platform == 'linux-x86_64':
            extract_overlays(archive_path, overlay_dir)
        else:
            nightly_formats.zip_overlays(archive_path, overlay_dir, platform)
        write_overlay_marker(destination, platform, root)
    except Exception:
        shutil.rmtree(destination)
        raise


def write_overlay_marker(inputs, platform, root=ROOT):
    fingerprint = abi_fingerprint(root)
    paths = list((inputs / 'overlays').iterdir())
    marker = {'schema': 1, 'platform': platform, 'abi_sha256': fingerprint, 'toolchain': TOOLCHAINS[platform],
              'files': {p.name: digest(regular(p)) for p in paths}}
    (inputs / 'overlay-files.json').write_text(json.dumps(marker, sort_keys=True) + '\n')
    (inputs / 'overlay-platform.txt').write_text(platform + '\n')
    (inputs / 'overlay-abi.sha256').write_text(fingerprint + '\n')


def install_inputs(inputs, root=ROOT):
    """Prepare only an empty checkout; never replace a developer's game pair."""
    inputs = Path(inputs).resolve()
    destination = root / 'profiles/mhp3rd/game'
    if destination.is_symlink():
        raise Rejected('The checkout game directory must not be a link.')
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    created = []
    try:
        for name in ('EBOOT.ELF', 'disc.iso'):
            source = regular(inputs / name)
            target = destination / name
            if target.exists() or target.is_symlink():
                if target.is_symlink() and target.resolve() == source:
                    continue
                if os.name == 'nt' and target.is_file() and os.path.samefile(target, source):
                    continue
                raise Rejected('The checkout already contains another prepared game input.')
            if os.name == 'nt':
                os.link(source, target)
            else:
                target.symlink_to(source)
            created.append(target)
    except Exception:
        for target in created:
            target.unlink()
        raise


def verify_overlays(inputs, root=ROOT, platform='linux-x86_64'):
    inputs = Path(inputs)
    if platform not in PLATFORMS:
        raise Rejected('Unsupported native overlay platform.')
    if regular(inputs / 'overlay-platform.txt').read_text() != platform + '\n':
        raise Rejected('Wrong native overlay platform marker.')
    if regular(inputs / 'overlay-abi.sha256').read_text().strip() != abi_fingerprint(root):
        raise Rejected('Cached overlays do not match this source.')
    marker_path = regular(inputs / 'overlay-files.json')
    if marker_path.stat().st_size > 128 * 1024:
        raise Rejected('Invalid overlay manifest size.')
    marker = json.loads(marker_path.read_text())
    if (marker.get('schema') != 1 or marker.get('platform') != platform
            or marker.get('toolchain') != TOOLCHAINS[platform]
            or marker.get('abi_sha256') != abi_fingerprint(root)):
        raise Rejected('Wrong native overlay cache identity.')
    directory = inputs / 'overlays'
    if directory.is_symlink() or not directory.is_dir():
        raise Rejected('Missing overlay directory.')
    paths = list(directory.iterdir())
    if len(paths) != 355 or set(marker['files']) != {p.name for p in paths}:
        raise Rejected('Overlay cache is incomplete.')
    for path in paths:
        suffix = {'linux-x86_64': 'so', 'android-arm64': 'so',
                  'macos-arm64': 'dylib', 'windows-x86_64': 'dll'}[platform]
        if not re.fullmatch('[A-Za-z0-9_.-]+\\.' + suffix, path.name):
            raise Rejected('Unexpected cached overlay.')
        if digest(regular(path)) != sha(marker['files'][path.name]):
            raise Rejected('Cached overlay checksum changed.')
        if platform == 'linux-x86_64':
            with path.open('rb') as stream:
                native_elf(stream.read(64), library=True)
        else:
            import nightly_formats
            nightly_formats.binary(platform, path.read_bytes())


def private_run(log, command, timeout, env=None):
    """Capture build/generator output without exposing it to public workflow logs."""
    if os.name not in ('posix', 'nt'):
        raise Rejected('Unsupported private command platform.')
    log = Path(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('xb') as stream:
        os.chmod(log, 0o600)
        options = ({'start_new_session': True} if os.name == 'posix' else
                   {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW})
        if env is not None:
            options['env'] = env
        proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, **options)
        try:
            if proc.wait(timeout=timeout):
                raise Rejected('Private build step failed; raw output was not published.')
        finally:
            if proc.poll() is None:
                if os.name == 'nt':
                    # The PID belongs to this still-running Popen. /T stops its
                    # build descendants as well; diagnostics never reach CI.
                    with contextlib.suppress(OSError, subprocess.TimeoutExpired):
                        subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15, check=False)
                else:
                    os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    if os.name == 'nt':
                        proc.kill()
                    else:
                        os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()


def build_platform(inputs, work, output, source_sha, platform, probe=False, root=ROOT):
    """Run the same packagers with either approved originals or public fixtures."""
    import nightly_formats

    source_sha = commit(source_sha)
    if platform not in PLATFORMS:
        raise Rejected('Unsupported build platform.')
    inputs = Path(inputs).resolve()
    work, output = (Path(p).absolute() for p in (work, output))
    version = 'nightly-' + source_sha[:12]
    environment = dict(os.environ, YAKUMO_PRIVATE_INPUTS=inputs.as_posix(),
                       YAKUMO_WORK=work.as_posix(), NIGHTLY_VERSION=version)
    # Probe mode is a separate public PR job; trusted dispatches never inherit it.
    environment.pop('NIGHTLY_PROBE', None)
    if probe:
        environment['NIGHTLY_PROBE'] = '1'
        if platform != 'windows-x86_64':
            private_run(work.parent / 'probe.log', [sys.executable, str(root / 'scripts/ci/nightly_probe.py'),
                        '--platform', platform, '--output', str(inputs), '--install'], 600, env=environment)
    else:
        install_inputs(inputs, root)
    if platform == 'linux-x86_64':
        environment['YAKUMO_CONTAINER'] = 'docker'
        command = ['bash', str(root / 'profiles/mhp3rd/scripts/release_linux.sh'),
                   '--version', version, '--jobs', '2', '--no-flatpak']
    elif platform == 'windows-x86_64':
        command = ['pwsh', '-NoProfile', '-File', str(root / 'scripts/ci/nightly_windows.ps1')]
    else:
        script = 'nightly_macos.sh' if platform == 'macos-arm64' else 'nightly_android.sh'
        command = ['bash', str(root / 'scripts/ci' / script)]
    private_run(work.parent / 'build.log', command, 16800, env=environment)
    package = work / 'dist' / nightly_formats.package_name(platform, version)
    source_directory = 'sources' if platform == 'windows-x86_64' else 'dist'
    source = work / source_directory / source_pin(platform, root)[0]
    try:
        stage_public(package, source, output, source_sha, root, platform)
    except Exception:
        # This directory is private in trusted builds. Only the separate public
        # fixture job may upload its diagnostics; the CLI still prints no trace.
        with (work.parent / 'inspection.log').open('x') as stream:
            os.chmod(stream.name, 0o600)
            traceback.print_exc(file=stream)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('abi-fingerprint')
    build = commands.add_parser('build-platform')
    build.add_argument('--inputs', type=Path, required=True)
    build.add_argument('--work', type=Path, required=True)
    build.add_argument('--output', type=Path, required=True)
    build.add_argument('--source-sha', required=True)
    build.add_argument('--platform', choices=PLATFORMS, required=True)
    build.add_argument('--probe', action='store_true')
    source = commands.add_parser('source-name')
    source.add_argument('--platform', choices=PLATFORMS, required=True)
    fetch = commands.add_parser('fetch-inputs')
    fetch.add_argument('--output', type=Path, required=True)
    fetch.add_argument('--manifest-key', default='mhp3rd-hd/v1/manifest.json')
    fetch.add_argument('--platform', choices=PLATFORMS, default='linux-x86_64')
    verify = commands.add_parser('verify-overlays')
    verify.add_argument('--inputs', type=Path, required=True)
    verify.add_argument('--platform', choices=PLATFORMS, default='linux-x86_64')
    install = commands.add_parser('install-inputs')
    install.add_argument('--inputs', type=Path, required=True)
    stage = commands.add_parser('stage-public')
    stage.add_argument('--package', type=Path, required=True)
    stage.add_argument('--source-archive', type=Path, required=True)
    stage.add_argument('--output', type=Path, required=True)
    stage.add_argument('--source-sha', required=True)
    stage.add_argument('--platform', choices=PLATFORMS, default='linux-x86_64')
    args = parser.parse_args(argv)
    try:
        if args.command == 'abi-fingerprint':
            print(abi_fingerprint())
        elif args.command == 'build-platform':
            build_platform(args.inputs, args.work, args.output, args.source_sha, args.platform, args.probe)
            print('Native package built and publication directory verified.')
        elif args.command == 'source-name':
            print(source_pin(args.platform)[0])
        elif args.command == 'fetch-inputs':
            fetch_inputs(args.output, args.manifest_key, platform=args.platform)
            print('Private inputs and compatible overlays verified.')
        elif args.command == 'verify-overlays':
            verify_overlays(args.inputs, platform=args.platform)
            print('Compatible native overlay cache verified.')
        elif args.command == 'install-inputs':
            install_inputs(args.inputs)
            print('Private build inputs prepared in the empty checkout.')
        elif args.command == 'stage-public':
            stage_public(args.package, args.source_archive, args.output, args.source_sha, platform=args.platform)
            print('Publication directory validated and prepared.')
    except (Exception, KeyboardInterrupt):
        # Even parser/transport/archive exceptions can contain private paths,
        # filenames, response bodies or generated source. Never echo them.
        print('Nightly operation failed closed. No private diagnostic output was published.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
