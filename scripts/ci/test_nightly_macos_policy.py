"""Synthetic signed-container and macOS publication boundary regressions."""

import copy
import hashlib
import plistlib
import struct
import unittest
from pathlib import Path

from nightly_formats import Rejected
from nightly_macos_policy import (APP, EMPTY_CMS, EMPTY_REQUIREMENTS, LIBRARIES, RULES, RULES2,
                                  MacPolicy, normalize_signature_padding, validate_binary)


ROOT = Path(__file__).resolve().parents[2]
VERSION = 'nightly-' + 'a' * 12


def native(kind=6, identifier='fixture', info=None, resources=None, padding=b''):
    """A tiny public ARM64 Mach-O with one correctly sealed synthetic page."""
    executable = kind == 2
    specials = [hashlib.sha256(info).digest() if info is not None else bytes(32),
                hashlib.sha256(EMPTY_REQUIREMENTS).digest()]
    if executable:
        specials.append(hashlib.sha256(resources or b'').digest())
    identity = identifier.encode() + b'\0'
    hash_offset = 88 + len(identity) + len(specials) * 32
    directory_length = hash_offset + 32
    signature_length = 56 + directory_length
    page = bytearray(4096)
    struct.pack_into('<8I', page, 0, 0xFEEDFACF, 0x0100000C, 0, kind, 2, 40, 0, 0)
    struct.pack_into('<6I', page, 32, 0x32, 24, 1, 0x000D0000, 0x001B0000, 0)
    struct.pack_into('<4I', page, 56, 0x1D, 16, 4096, signature_length + len(padding))
    header = struct.pack('>9I4B', 0xFADE0C02, directory_length, 0x20400, 2, hash_offset,
                         88, len(specials), 1, 4096, 32, 2, 0, 12)
    directory = (header + bytes(24) + struct.pack('>3Q', 0, 4096, int(executable)) + identity +
                 b''.join(reversed(specials)) + hashlib.sha256(page).digest())
    signature = (struct.pack('>9I', 0xFADE0CC0, signature_length, 3, 0, 56, 2, 36, 0x10000, 48) +
                 EMPTY_REQUIREMENTS + EMPTY_CMS + directory)
    return bytes(page) + signature + padding


def resources_xml(value):
    # Apple codesign spells real-valued integral rule weights without `.0`.
    data = plistlib.dumps(value, fmt=plistlib.FMT_XML, sort_keys=True)
    import re
    return re.sub(rb'<real>([0-9]+)\.0</real>', rb'<real>\1</real>', data)


class MacPolicyTests(unittest.TestCase):
    def fixture(self):
        policy = MacPolicy(ROOT, VERSION, 'a' * 40)
        payloads = dict(policy.exact)
        # Tiny synthetic font/upstream-license fixtures exercise the same exact
        # cryptographic checks without copying multi-megabyte public assets into
        # the tests. All first-party/template oracles remain the committed bytes.
        for name in policy.hashes:
            payloads[name] = ('Public synthetic asset: ' + name).encode()
            policy.hashes[name] = hashlib.sha256(payloads[name]).hexdigest()
        for name in sorted(LIBRARIES):
            payloads[APP + 'Frameworks/' + name] = native(identifier=name[:-6])
        for number in range(355):
            name = 'overlay_' + str(number)
            payloads[APP + 'Frameworks/overlays/' + name + '.dylib'] = native(kind=8, identifier=name)
        seal = {'files': {}, 'files2': {}, 'rules': copy.deepcopy(RULES), 'rules2': copy.deepcopy(RULES2)}
        for name, data in payloads.items():
            relative = name[len(APP):] if name.startswith(APP) else name
            if relative.startswith('Resources/'):
                seal['files'][relative] = hashlib.sha1(data).digest()
                seal['files2'][relative] = {'hash2': hashlib.sha256(data).digest()}
            elif relative.startswith('Frameworks/'):
                cdhash = validate_binary(data, require_signature=True)['cdhash']
                seal['files2'][relative] = {'cdhash': cdhash, 'requirement': 'cdhash H"' + cdhash.hex() + '"'}
        blob = resources_xml(seal)
        payloads[APP + '_CodeSignature/CodeResources'] = blob
        payloads[APP + 'MacOS/Yakumo'] = native(kind=2, identifier='io.github.teamgdb.yakumo',
                                              info=payloads[APP + 'Info.plist'], resources=blob)
        return policy, payloads

    def consume(self, policy, payloads):
        for name, data in payloads.items():
            policy.check(name, data)
        policy.finish(set(payloads))

    def test_complete_signed_synthetic_bundle(self):
        policy, payloads = self.fixture()
        self.consume(policy, payloads)
        self.assertEqual(len(payloads), 381)
        self.assertEqual(len(policy.overlays), 355)

    def test_allowed_directories_do_not_change_exact_file_set(self):
        policy, payloads = self.fixture()
        self.consume(policy, payloads)
        policy.finish(set(payloads) | {'', 'Yakumo.app', APP.rstrip('/'), APP + 'Frameworks'})
        with self.assertRaises(Rejected):
            policy.finish(set(payloads) | {APP + 'arbitrary'})

    def test_rejects_renamed_game_formats_and_wrong_cpu(self):
        for content in (b'\x7fELF\x01\x01\x01' + bytes(100), b'~PSP' + bytes(100),
                        b'\0PBP' + bytes(100), b'\0PSF' + bytes(100)):
            with self.subTest(magic=content[:4]):
                with self.assertRaises(Rejected):
                    validate_binary(content, require_signature=True)
        for offset, value in ((0, 0xCAFEBABE), (4, 0x01000007), (8, 2), (12, 1), (28, 1)):
            with self.subTest(header=offset):
                data = bytearray(native())
                struct.pack_into('<I', data, offset, value)
                with self.assertRaises(Rejected):
                    validate_binary(bytes(data), require_signature=True)
        with self.assertRaises(Rejected):
            validate_binary(native(kind=2), library=True)
        with self.assertRaises(Rejected):
            validate_binary(native(), library=False)

    def test_unsigned_cache_library_is_allowed_but_publication_is_not(self):
        data = struct.pack('<8I', 0xFEEDFACF, 0x0100000C, 0, 8, 0, 0, 0, 0)
        self.assertEqual(validate_binary(data)['kind'], 8)
        self.assertEqual(normalize_signature_padding(data), data)
        with self.assertRaises(Rejected):
            validate_binary(data, require_signature=True)

    def test_rejects_malformed_load_commands_and_trailers(self):
        for change in ('empty', 'count', 'size', 'short_command', 'unaligned', 'signature_offset',
                       'signature_amount', 'wrong_platform', 'newer_target', 'trailer'):
            data = bytearray(native())
            if change == 'empty':
                data = data[:20]
            elif change == 'count':
                struct.pack_into('<I', data, 16, 513)
            elif change == 'size':
                struct.pack_into('<I', data, 20, len(data))
            elif change == 'short_command':
                struct.pack_into('<I', data, 36, 4)
            elif change == 'unaligned':
                struct.pack_into('<I', data, 36, 25)
            elif change == 'signature_offset':
                struct.pack_into('<I', data, 64, 32)
            elif change == 'signature_amount':
                struct.pack_into('<I', data, 68, 1)
            elif change == 'wrong_platform':
                struct.pack_into('<I', data, 40, 2)
            elif change == 'newer_target':
                struct.pack_into('<I', data, 44, 0x000F0000)
            else:
                data.extend(b'unsealed trailer')
            with self.subTest(change=change), self.assertRaises(Rejected):
                validate_binary(bytes(data), require_signature=True)

    def test_page_hash_detects_modified_native_bytes(self):
        data = bytearray(native())
        data[1024] ^= 1
        with self.assertRaises(Rejected):
            validate_binary(bytes(data), require_signature=True)

    def test_only_unused_signature_padding_is_normalized(self):
        original = native(padding=b'old hash remnants')
        with self.assertRaises(Rejected):
            validate_binary(original, require_signature=True)
        normalized = normalize_signature_padding(original)
        self.assertEqual(normalized[:-17], original[:-17])
        self.assertEqual(normalized[-17:], bytes(17))
        facts = validate_binary(normalized, require_signature=True)
        self.assertEqual(facts['identifier'], 'fixture')
        original_directory = original[4096 + 56:-17]
        self.assertEqual(facts['cdhash'], hashlib.sha256(original_directory).digest()[:20])
        self.assertEqual(normalize_signature_padding(normalized), normalized)

    def test_older_codesign_large_unused_allocation_is_zeroed_and_bounded(self):
        padding = b'old hash allocation' * 1024
        original = native(padding=padding)
        normalized = normalize_signature_padding(original)
        self.assertEqual(normalized[:-len(padding)], original[:-len(padding)])
        self.assertEqual(normalized[-len(padding):], bytes(len(padding)))
        facts = validate_binary(normalized, require_signature=True)
        directory = original[4096 + 56:-len(padding)]
        self.assertEqual(facts['cdhash'], hashlib.sha256(directory).digest()[:20])
        for operation in (normalize_signature_padding,
                          lambda data: validate_binary(data, require_signature=True)):
            with self.subTest(operation=operation), self.assertRaises(Rejected):
                operation(native(padding=bytes(65536)))

    def test_rejects_signature_structure_and_metadata_corruption(self):
        for offset, value in ((4096, 0), (4104, 4), (4108, 2), (4112, 8),
                              (4136, 11), (4152, 0), (4160, 0x20500), (4164, 0),
                              (4172, 89), (4176, 3), (4180, 2), (4184, 1024), (4192, 1)):
            data = bytearray(native())
            struct.pack_into('>I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(Rejected):
                validate_binary(bytes(data), require_signature=True)
        data = native(padding=bytes(65536))
        with self.assertRaises(Rejected):
            normalize_signature_padding(data)

    def test_fixed_assets_templates_and_known_hashes_are_exact(self):
        policy, payloads = self.fixture()
        for name in [APP + 'Info.plist', APP + 'PkgInfo', APP + 'Resources/Yakumo.icns',
                     'Read Me.txt', APP + 'Resources/licenses/Yakumo-LICENSE.txt',
                     APP + 'Resources/fonts/NotoSansCJKjp-Regular.otf',
                     APP + 'Resources/vulkan/icd.d/MoltenVK_icd.json']:
            with self.subTest(name=name):
                fresh = MacPolicy(ROOT, VERSION, 'a' * 40)
                fresh.hashes = dict(policy.hashes)
                with self.assertRaises(Rejected):
                    fresh.check(name, payloads[name] + b'copied original data')

    def test_rejects_unknown_files_traversal_and_duplicate_members(self):
        for name in ('EBOOT.ELF', APP + 'Resources/private.txt', APP + 'Frameworks/other.dylib',
                     '../secret', APP + '../secret', APP + 'Frameworks//extra.dylib',
                     APP + 'Resources\\private.txt', APP + 'Resources/\nprivate.txt'):
            with self.subTest(name=name), self.assertRaises(Rejected):
                MacPolicy(ROOT, VERSION, 'a' * 40).check(name, b'public stand-in')
        policy, payloads = self.fixture()
        policy.check('Read Me.txt', payloads['Read Me.txt'])
        with self.assertRaises(Rejected):
            policy.check('Read Me.txt', payloads['Read Me.txt'])

    def test_missing_overlay_or_seal_never_publishes(self):
        for missing in (APP + 'Frameworks/overlays/overlay_354.dylib',
                        APP + '_CodeSignature/CodeResources', APP + 'MacOS/Yakumo'):
            policy, payloads = self.fixture()
            payloads.pop(missing)
            with self.subTest(missing=missing), self.assertRaises(Rejected):
                self.consume(policy, payloads)

    def test_resource_hash_and_nested_cdhash_must_match_actual_files(self):
        for mutation in ('resource_sha1', 'resource_sha256', 'native_cdhash', 'missing', 'extra', 'main'):
            policy, payloads = self.fixture()
            name = APP + '_CodeSignature/CodeResources'
            seal = plistlib.loads(payloads[name])
            resource = next(iter(seal['files']))
            code = 'Frameworks/libSDL3.0.dylib'
            if mutation == 'resource_sha1':
                seal['files'][resource] = bytes(20)
            elif mutation == 'resource_sha256':
                seal['files2'][resource]['hash2'] = bytes(32)
            elif mutation == 'native_cdhash':
                seal['files2'][code]['cdhash'] = bytes(20)
            elif mutation == 'missing':
                seal['files2'].pop(code)
            elif mutation == 'extra':
                seal['files2']['Resources/secret'] = {'hash2': bytes(32)}
            else:
                # A well-typed new seal is still rejected if the executable's
                # signed resource-special slot names the original seal bytes.
                seal['files2'][code]['requirement'] = 'cdhash H"' + '0' * 40 + '"'
            payloads[name] = resources_xml(seal)
            if mutation != 'main':
                payloads[APP + 'MacOS/Yakumo'] = native(kind=2, identifier='io.github.teamgdb.yakumo',
                    info=payloads[APP + 'Info.plist'], resources=payloads[name])
            with self.subTest(mutation=mutation), self.assertRaises(Rejected):
                self.consume(policy, payloads)

    def test_resource_xml_cannot_hide_unsealed_content_or_change_rules(self):
        policy, payloads = self.fixture()
        name = APP + '_CodeSignature/CodeResources'
        for data in (payloads[name].replace(b'<dict>', b'<!-- hidden original text --><dict>', 1),
                     payloads[name] + b'private trailer', b'invalid plist',
                     plistlib.dumps({}, fmt=plistlib.FMT_BINARY)):
            with self.subTest(data=data[:20]), self.assertRaises(Rejected):
                MacPolicy(ROOT, VERSION, 'a' * 40).check(name, data)
        for value in (1, False, {'omit': True}):
            seal = plistlib.loads(payloads[name])
            seal['rules']['^Resources/'] = value
            with self.subTest(rule=value), self.assertRaises(Rejected):
                MacPolicy(ROOT, VERSION, 'a' * 40).check(name, resources_xml(seal))

    def test_native_identifiers_are_bound_to_expected_package_roles(self):
        for name, data in ((APP + 'MacOS/Yakumo', native(kind=2, identifier='not-the-app')),
                           (APP + 'Frameworks/libSDL3.0.dylib', native(identifier='unknown-library')),
                           (APP + 'Frameworks/libSDL3.0.dylib', native(kind=8, identifier='libSDL3.0')),
                           (APP + 'Frameworks/overlays/overlay.dylib', native(identifier='overlay'))):
            with self.subTest(name=name), self.assertRaises(Rejected):
                MacPolicy(ROOT, VERSION, 'a' * 40).check(name, data)

    def test_invalid_source_version_metadata_is_rejected(self):
        for version, sha in (('../bad', 'a' * 40), ('nightly-safe', 'bad')):
            with self.subTest(version=version), self.assertRaises(Rejected):
                MacPolicy(ROOT, version, sha)


if __name__ == '__main__':
    unittest.main()
