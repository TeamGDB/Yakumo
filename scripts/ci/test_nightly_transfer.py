"""Exercise the actual checkout-free inline workflow transfer validators."""

import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import textwrap
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = '0123456789abcdef0123456789abcdef01234567'
RUN = '33100001'


def workflow_code(marker):
    """Prefer committed final YAML; fragments exist only during integration."""
    paths = [ROOT / '.github/workflows/nightly.yml',
             ROOT / 'out/nightly-integration/sign.yml', ROOT / 'out/nightly-integration/publish.yml']
    for path in paths:
        if not path.exists():
            continue
        lines = path.read_text().splitlines()
        for index, line in enumerate(lines):
            if line.strip() != '# nightly-transfer:' + marker:
                continue
            end = index + 1
            while end < len(lines) and lines[end].strip() != 'PY':
                end += 1
            if end == len(lines):
                raise AssertionError('Unterminated inline Python block.')
            return textwrap.dedent('\n'.join(lines[index:end])) + '\n'
    raise AssertionError('Missing inline nightly transfer validator: ' + marker)


def namespace(marker):
    scope = {'__name__': 'nightly_transfer_contract_test'}
    exec(compile(workflow_code(marker), '<nightly-workflow-' + marker + '>', 'exec'), scope)
    return scope


def unsigned_apk(entries=None):
    result = io.BytesIO()
    with zipfile.ZipFile(result, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in (entries or [('AndroidManifest.xml', b'public manifest fixture'),
                                       ('classes.dex', b'public DEX fixture')]):
            archive.writestr(name, data)
    return result.getvalue()


def signed_apk(original, pairs=None, padding=b''):
    """Public frame fixture, not a cryptographic signature; SDK test signs below."""
    pairs = pairs or [(0x7109871a, b'public v2 signature fixture'), (0xf05368c0, b'public v3 signature fixture')]
    encoded = b''.join(struct.pack('<QI', len(data) + 4, identity) + data for identity, data in pairs)
    size = len(encoded) + 24
    block = struct.pack('<Q', size) + encoded + struct.pack('<Q', size) + b'APK Sig Block 42'
    end = bytearray(original[-22:])
    directory = struct.unpack_from('<I', end, 16)[0]
    struct.pack_into('<I', end, 16, directory + len(padding) + len(block))
    return original[:directory] + padding + block + original[directory:-22] + end


def tool_output(certificate):
    return ('Verifies\nVerified using v1 scheme (JAR signing): false\n'
            'Verified using v2 scheme (APK Signature Scheme v2): true\n'
            'Verified using v3 scheme (APK Signature Scheme v3): true\n'
            'Verified using v4 scheme (APK Signature Scheme v4): false\n'
            'Number of signers: 1\nSigner #1 certificate SHA-256 digest: ' + certificate + '\n')


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.unsigned = namespace('verify-unsigned')
        self.signed = namespace('verify-signed')
        self.publisher = namespace('publish')
        self.source_data = {name: ('public source fixture ' + name).encode()
                            for name in self.unsigned['SOURCE_PINS']}
        self.pins = {name: hashlib.sha256(data).hexdigest() for name, data in self.source_data.items()}
        # Synthetic source bytes replace only source-hash facts for positive
        # fixtures; the immutable production pins are separately asserted below.
        for scope in (self.unsigned, self.signed, self.publisher):
            scope['SOURCE_PINS'] = self.pins.copy()

    def bundle(self, platform='android-arm64', signing='required', name='bundle'):
        directory = self.root / name
        directory.mkdir(parents=True)
        package = self.unsigned['package_name'](platform, SOURCE)
        if platform == 'android-arm64':
            data = unsigned_apk()
            if signing == 'verified':
                data = signed_apk(data)
        else:
            data = ('public already-inspected package fixture ' + platform).encode()
        source = next(name for name in self.source_data if (len(name) > 25) == (platform == 'windows-x86_64'))
        files = []
        for filename, contents in [(package, data), (source, self.source_data[source])]:
            (directory / filename).write_bytes(contents)
            files.append({'name': filename, 'size': len(contents), 'sha256': hashlib.sha256(contents).hexdigest()})
        record = {'schema': 1, 'source': SOURCE, 'platform': platform, 'files': files, 'android_signing': signing}
        if signing == 'verified':
            record['certificate_sha256'] = self.signed['CERTIFICATE']
        (directory / 'build.json').write_text(json.dumps(record) + '\n')
        (directory / 'SHA256SUMS').write_text(''.join(entry['sha256'] + '  ' + entry['name'] + '\n' for entry in files))
        (directory / 'release-notes.md').write_text(self.unsigned['notes'](platform, SOURCE))
        return directory, record

    def check(self, directory, platform='android-arm64', signing='required'):
        return self.unsigned['validate_bundle'](directory, platform, SOURCE, signing)

    def test_all_inline_blocks_compile_and_production_pins_are_exact(self):
        for marker in ['verify-unsigned', 'verify-signed', 'publish', 'sign-with-key']:
            compile(workflow_code(marker), '<workflow-contract>', 'exec')
        for marker in ['verify-unsigned', 'verify-signed', 'publish']:
            actual = namespace(marker)
            self.assertEqual(actual['CERTIFICATE'], '1ca2f27cfbcd9ea7d875a8ca002f57866037b19f43aaf449ce87739aee868570')
            self.assertEqual(actual['SOURCE_PINS'], {
                'ffmpeg-7.1.5.tar.xz': 'de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f',
                'ffmpeg-7d0e8420048cffd0ca3883b877ead2390496d0b2.tar.gz': '2caafb2bbfb69c0518470651640e71ac7f5fb3117d188bf6ea2d909307a02b1d'})

    def test_exact_bundle_contract_for_every_platform_and_signing_state(self):
        for platform, signing in [('linux-x86_64', 'not-applicable'), ('macos-arm64', 'not-applicable'),
                                  ('windows-x86_64', 'not-applicable'), ('android-arm64', 'required'),
                                  ('android-arm64', 'verified')]:
            with self.subTest(platform=platform, signing=signing):
                directory, expected = self.bundle(platform, signing, platform + signing)
                self.assertEqual(self.check(directory, platform, signing), expected)

    def test_changed_content_hash_size_source_or_extra_metadata_rejected(self):
        changes = [lambda record: record.update(source='f' * 40),
                   lambda record: record.update(platform='windows-x86_64'),
                   lambda record: record.update(schema=True),
                   lambda record: record.update(unexpected='opaque metadata'),
                   lambda record: record['files'][0].update(size=True),
                   lambda record: record['files'][0].update(size=0),
                   lambda record: record['files'][0].update(size=2 ** 31 + 1),
                   lambda record: record['files'][0].update(sha256='f' * 64),
                   lambda record: record['files'][0].update(name='../escape'),
                   lambda record: record['files'][0].update(unexpected='opaque metadata'),
                   lambda record: record.update(android_signing='verified')]
        for index, change in enumerate(changes):
            directory, record = self.bundle(name='change-' + str(index))
            change(record)
            (directory / 'build.json').write_text(json.dumps(record))
            with self.subTest(case=index), self.assertRaises(ValueError):
                self.check(directory)
        directory, record = self.bundle(name='changed-bytes')
        (directory / record['files'][0]['name']).write_bytes(b'wrong payload')
        with self.assertRaises(ValueError):
            self.check(directory)

    def test_missing_added_linked_oversized_or_duplicate_key_metadata_rejected(self):
        directory, _ = self.bundle()
        (directory / 'unexpected.log').write_text('public fixture')
        with self.assertRaises(ValueError):
            self.check(directory)
        (directory / 'unexpected.log').unlink()
        original = (directory / 'build.json').read_text()
        for content in ['x' * 16384, original.replace('{', '{"source":"discarded duplicate",', 1)]:
            (directory / 'build.json').write_text(content)
            with self.assertRaises(ValueError):
                self.check(directory)
        (directory / 'build.json').write_text(original)
        (directory / 'release-notes.md').write_text('arbitrary UTF-8 text\n')
        with self.assertRaises(ValueError):
            self.check(directory)
        (directory / 'release-notes.md').unlink()
        try:
            (directory / 'release-notes.md').symlink_to(directory / 'SHA256SUMS')
        except OSError:
            return
        with self.assertRaises(ValueError):
            self.check(directory)

    def test_source_offer_and_signer_pin_cannot_be_replaced_with_self_consistent_hashes(self):
        directory, record = self.bundle(signing='verified')
        record['certificate_sha256'] = '0' * 64
        (directory / 'build.json').write_text(json.dumps(record))
        with self.assertRaises(ValueError):
            self.check(directory, signing='verified')
        directory, record = self.bundle(name='wrong-source')
        source = record['files'][1]
        data = b'different public source fixture'
        (directory / source['name']).write_bytes(data)
        source.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
        (directory / 'build.json').write_text(json.dumps(record))
        (directory / 'SHA256SUMS').write_text(''.join(entry['sha256'] + '  ' + entry['name'] + '\n' for entry in record['files']))
        with self.assertRaises(ValueError):
            self.check(directory)

    def pair(self, before=None, after=None):
        before = unsigned_apk() if before is None else before
        after = signed_apk(before) if after is None else after
        unsigned, signed = self.root / 'unsigned.apk', self.root / 'signed.apk'
        unsigned.write_bytes(before)
        signed.write_bytes(after)
        return unsigned, signed

    def test_signing_adds_only_known_bounded_block_without_changing_payload_or_zip_regions(self):
        unsigned, signed = self.pair()
        self.signed['validate_signed_pair'](unsigned, signed, tool_output(self.signed['CERTIFICATE']))
        directory = struct.unpack_from('<I', unsigned.read_bytes(), len(unsigned.read_bytes()) - 6)[0]
        signed.write_bytes(signed_apk(unsigned.read_bytes(), padding=b'\0' * (-directory % 4096)))
        self.signed['validate_signed_pair'](unsigned, signed, tool_output(self.signed['CERTIFICATE']))
        signed.write_bytes(signed_apk(unsigned.read_bytes(), [(0x7109871a, b'public v2'),
                                                            (0xf05368c0, b'public v3'),
                                                            (0x42726577, b'\0' * 64)]))
        self.signed['validate_signed_pair'](unsigned, signed, tool_output(self.signed['CERTIFICATE']))

    def test_signature_block_or_payload_changes_are_rejected(self):
        before = unsigned_apk()
        directory = struct.unpack_from('<I', before, len(before) - 6)[0]
        bad = [signed_apk(before, [(0x7109871a, b'v2')]),
               signed_apk(before, [(0x7109871a, b'v2'), (0xf05368c0, b'v3'), (123, b'opaque')]),
               signed_apk(before, [(0x7109871a, b'v2'), (0xf05368c0, b'v3'), (0x7109871a, b'duplicate')]),
               signed_apk(before, [(0x7109871a, b'v2'), (0xf05368c0, b'v3'), (0x42726577, b'X')]),
               signed_apk(before, [(0x7109871a, b'x' * (512 * 1024 + 1)), (0xf05368c0, b'v3')]),
               signed_apk(before) + b'trailing payload',
               signed_apk(before, padding=b'X' * (-directory % 4096)),
               signed_apk(before, padding=b'\0'),
               signed_apk(before, padding=b'\0' * 4096),
               signed_apk(unsigned_apk([('AndroidManifest.xml', b'changed manifest'), ('classes.dex', b'public DEX fixture')])),
               signed_apk(unsigned_apk([('AndroidManifest.xml', b'public manifest fixture'), ('classes.dex', b'public DEX fixture'), ('extra', b'new file')]))]
        for index, data in enumerate(bad):
            unsigned, signed = self.pair(before, data)
            with self.subTest(case=index), self.assertRaises(ValueError):
                self.signed['validate_signed_pair'](unsigned, signed, tool_output(self.signed['CERTIFICATE']))

    def test_official_verifier_output_requires_one_pinned_signer_and_both_modern_schemes(self):
        good = tool_output(self.signed['CERTIFICATE'])
        for value in [good.replace(self.signed['CERTIFICATE'], 'f' * 64),
                      good.replace('Number of signers: 1', 'Number of signers: 2'),
                      good.replace('v2 scheme (APK Signature Scheme v2): true', 'v2 scheme (APK Signature Scheme v2): false'),
                      good.replace('v3 scheme (APK Signature Scheme v3): true', 'v3 scheme (APK Signature Scheme v3): false'),
                      good.replace('v1 scheme (JAR signing): false', 'v1 scheme (JAR signing): true'),
                      good + 'Signer #2 certificate SHA-256 digest: ' + self.signed['CERTIFICATE'] + '\n']:
            with self.assertRaises(ValueError):
                self.signed['verify_tool_output'](value)

    def aggregate_fixture(self):
        transfer = self.root / 'transfer'
        transfer.mkdir()
        for platform in self.publisher['PLATFORMS']:
            signing = 'verified' if platform == 'android-arm64' else 'not-applicable'
            self.bundle(platform, signing, 'transfer/nightly-final-' + RUN + '-' + platform)
        return transfer

    def test_four_platform_aggregate_has_only_six_payloads_two_metadata_files_and_static_notes(self):
        transfer = self.aggregate_fixture()
        output = self.root / 'publication'
        files = self.publisher['aggregate'](transfer, output, SOURCE, RUN)
        self.assertEqual(len(files), 6)
        self.assertEqual(len(list(output.iterdir())), 9)
        self.assertEqual({entry['name'] for entry in files},
                         {self.publisher['package_name'](p, SOURCE) for p in self.publisher['PLATFORMS']} | set(self.source_data))
        record = json.loads((output / 'build.json').read_text())
        self.assertEqual(record['source'], SOURCE)
        self.assertEqual(len(record['platforms']), 4)
        self.assertIn('Full device/gameplay testing is not claimed.', (output / 'release-notes.md').read_text())
        self.assertEqual((output / 'SHA256SUMS').read_text(),
                         ''.join(entry['sha256'] + '  ' + entry['name'] + '\n' for entry in files))

    def test_wrong_run_extra_platform_missing_platform_or_unsigned_android_cannot_publish(self):
        transfer = self.aggregate_fixture()
        output = self.root / 'publication'
        with self.assertRaises(ValueError):
            self.publisher['aggregate'](transfer, output, SOURCE, 'different-run')
        (transfer / 'unexpected-platform').mkdir()
        with self.assertRaises(ValueError):
            self.publisher['aggregate'](transfer, output, SOURCE, RUN)
        (transfer / 'unexpected-platform').rmdir()
        shutil.rmtree(transfer / ('nightly-final-' + RUN + '-android-arm64'))
        with self.assertRaises(ValueError):
            self.publisher['aggregate'](transfer, output, SOURCE, RUN)
        self.bundle('android-arm64', 'required', 'transfer/nightly-final-' + RUN + '-android-arm64')
        with self.assertRaises(ValueError):
            self.publisher['aggregate'](transfer, output, SOURCE, RUN)
        self.assertFalse(output.exists())

    def test_isolated_job_configuration_keeps_credentials_on_only_the_required_steps(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        for name in ['sign', 'publish']:
            match = re.search(r'^  ' + name + r':\n(.*?)(?=^  [A-Za-z_][A-Za-z0-9_-]*:\n|\Z)', workflow, re.M | re.S)
            if not match:
                self.skipTest('Fragments not integrated yet.')
            text = match[0]
            self.assertNotIn('actions/checkout@', text)
            self.assertNotIn('R2_', text)
            self.assertIn("github.event_name == 'workflow_dispatch'", text)
            self.assertIn("github.ref == 'refs/heads/main'", text)
        self.assertEqual(workflow.count('secrets.ANDROID_RELEASE_KEYSTORE_B64'), 1)
        self.assertEqual(workflow.count('secrets.ANDROID_RELEASE_KEYSTORE_PASSWORD'), 1)
        self.assertEqual(workflow.count('secrets.ANDROID_RELEASE_KEY_ALIAS'), 1)


def sdk_tools():
    if os.name != 'posix':
        return None
    roots = [Path(os.environ.get('ANDROID_HOME', '/nonexistent')),
             Path.home() / 'Library/Android/sdk', Path('/opt/homebrew/share/android-commandlinetools')]
    for version in ['35.0.0', '35.0.1']:
        for root in roots:
            tools = root / 'build-tools' / version
            jar = root / 'platforms/android-35/android.jar'
            keytool = shutil.which('keytool') or '/opt/homebrew/opt/openjdk/bin/keytool'
            java = os.environ.get('JAVA_HOME')
            homebrew = Path('/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home')
            if homebrew.is_dir() and not java:
                java = str(homebrew)
            if all((tools / name).is_file() for name in ['aapt2', 'zipalign', 'apksigner']) and jar.is_file() and Path(keytool).is_file():
                return tools, jar, keytool, java
    return None


class PublicApkSignerIntegration(unittest.TestCase):
    def test_disposable_key_and_public_minimal_apk_preserve_regions_and_verify_both_schemes(self):
        tools = sdk_tools()
        if tools is None:
            self.skipTest('Official Android SDK35 tools and Java are unavailable.')
        tools, android_jar, keytool, java_home = tools
        scope = namespace('verify-signed')
        with tempfile.TemporaryDirectory(prefix='yakumo-public-sign-test-') as temporary:
            root = Path(temporary)
            environment = {**os.environ, 'PUBLIC_TEST_PASSWORD': 'public-disposable-fixture-password'}
            if java_home:
                environment['JAVA_HOME'] = java_home

            def run(args):
                return subprocess.run([str(arg) for arg in args], env=environment, check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120).stdout

            manifest = root / 'AndroidManifest.xml'
            manifest.write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                                'package="io.github.teamgdb.publicfixture" android:versionCode="1" android:versionName="1">'
                                '<uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>'
                                '<application android:hasCode="false" android:label="Public fixture"/></manifest>')
            run([tools / 'aapt2', 'link', '-I', android_jar, '--manifest', manifest, '-o', root / 'raw.apk'])
            run([tools / 'zipalign', '-f', '-P', '16', '4', root / 'raw.apk', root / 'unsigned.apk'])
            key = root / 'public-test.keystore'
            run([keytool, '-genkeypair', '-keystore', key, '-storepass:env', 'PUBLIC_TEST_PASSWORD',
                 '-keypass:env', 'PUBLIC_TEST_PASSWORD', '-alias', 'public-fixture', '-keyalg', 'RSA',
                 '-keysize', '2048', '-validity', '2', '-dname', 'CN=Public disposable test fixture', '-noprompt'])
            certificate = run([keytool, '-exportcert', '-keystore', key, '-storepass:env', 'PUBLIC_TEST_PASSWORD',
                               '-alias', 'public-fixture'])
            scope['CERTIFICATE'] = hashlib.sha256(certificate).hexdigest()
            # Execute the actual protected signing block, with only a fresh
            # disposable test key. This catches drift in production tool flags.
            sdk = root / 'public-sdk'
            (sdk / 'build-tools').mkdir(parents=True)
            (sdk / 'build-tools/35.0.0').symlink_to(tools, target_is_directory=True)
            apk = root / ('yakumo-nightly-' + SOURCE[:12] + '-android-arm64.apk')
            shutil.copyfile(root / 'unsigned.apk', apk)
            private = root / 'sign-private'
            signing_environment = {**environment, 'GITHUB_SHA': SOURCE, 'ANDROID_HOME': str(sdk),
                                   'SIGN_INPUT': str(root), 'SIGN_PRIVATE': str(private),
                                   'ANDROID_RELEASE_KEYSTORE_B64': base64.b64encode(key.read_bytes()).decode(),
                                   'ANDROID_RELEASE_KEYSTORE_PASSWORD': environment['PUBLIC_TEST_PASSWORD'],
                                   'ANDROID_RELEASE_KEY_ALIAS': 'public-fixture'}
            signer = namespace('sign-with-key')
            with mock.patch.dict(os.environ, signing_environment):
                signer['main']()
            signed = private / 'signed.apk'
            verification = run([tools / 'apksigner', 'verify', '--verbose', '--print-certs',
                                '--min-sdk-version', '24', '--max-sdk-version', '35', signed]).decode()
            scope['validate_signed_pair'](root / 'unsigned.apk', signed, verification)
            self.assertEqual((private / 'release.keystore').stat().st_mode & 0o777, 0o600)
            self.assertFalse((private / 'signed.apk.idsig').exists())


if __name__ == '__main__':
    unittest.main()
