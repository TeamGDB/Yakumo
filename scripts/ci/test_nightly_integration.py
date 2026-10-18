"""Cross-platform orchestration contracts with public, synthetic inputs only."""

import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

import nightly
import nightly_formats
from test_nightly import Fixture, elf as linux_elf
from test_nightly_android_policy import elf as android_elf
from test_nightly_macos_policy import native as macos_native
from test_nightly_windows_policy import pe as windows_pe

SOURCE = 'a' * 40
FINGERPRINT = 'b' * 64


def checksum(data):
    return hashlib.sha256(data).hexdigest()


def native(platform):
    return {'linux-x86_64': linux_elf, 'android-arm64': android_elf,
            'windows-x86_64': windows_pe, 'macos-arm64': macos_native}[platform]()


def suffix(platform):
    return {'linux-x86_64': 'so', 'android-arm64': 'so', 'windows-x86_64': 'dll', 'macos-arm64': 'dylib'}[platform]


class ManifestIntegrationTests(Fixture):
    def setUp(self):
        super().setUp()
        self.inputs = {'EBOOT.ELF': b'PUBLIC EXECUTABLE FIXTURE', 'disc.iso': b'PUBLIC DISC FIXTURE'}
        self.objects, files, caches = {}, [], {}
        for name, data in self.inputs.items():
            parts = []
            for number, part in enumerate([data[:7], data[7:]]):
                key = f'public/{name}/{number}'
                self.objects[nightly.INPUT_BUCKET, key] = part
                parts.append({'key': key, 'size': len(part), 'sha256': checksum(part)})
            files.append({'name': name, 'size': len(data), 'sha256': checksum(data), 'parts': parts})
        for platform in nightly_formats.PLATFORMS:
            filename = nightly_formats.package_name(platform, 'fixture')
            archive = self.base / filename
            if platform == 'linux-x86_64':
                shutil.copyfile(self.package, archive)
                # The root inside the tar must retain the approved seed name.
                archive = self.package
                filename = archive.name
            else:
                prefix = filename[:-4] + '/'
                with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as packed:
                    for number in range(355):
                        name = f'ovl{number:04}.{suffix(platform)}'
                        path = (f'lib/arm64-v8a/lib{name}' if platform == 'android-arm64' else
                                prefix + ('Yakumo.app/Contents/Frameworks/overlays/' if platform == 'macos-arm64' else 'overlays/') + name)
                        packed.writestr(path, native(platform))
                    packed.writestr('public-seed-metadata.txt', b'Approved private cache metadata is not extracted')
            data = archive.read_bytes()
            key = f'public/cache/{platform}'
            self.objects[nightly.CACHE_BUCKET, key] = data
            caches[platform] = {'filename': filename, 'key': key, 'source_commit': SOURCE,
                                'abi_sha256': FINGERPRINT, 'toolchain': nightly.TOOLCHAINS[platform],
                                'size': len(data), 'sha256': checksum(data)}
        self.manifest = {'schema': 2, 'files': files, 'overlay_caches': caches}
        self.requested = []

    def fetch(self, platform, output=None):
        output = self.base / ('restored-' + platform) if output is None else output

        def get_object(Bucket, Key):
            self.requested.append((Bucket, Key))
            value = json.dumps(self.manifest).encode() if Key == 'public/manifest' else self.objects[Bucket, Key]
            return {'Body': io.BytesIO(value), 'ContentLength': len(value)}

        client = mock.Mock()
        client.get_object.side_effect = get_object

        def fingerprint(root=nightly.ROOT, source=None):
            return FINGERPRINT if source in (None, SOURCE) else 'c' * 64

        with mock.patch.object(nightly, 'r2_client', return_value=client), \
                mock.patch.object(nightly, 'EXPECTED_EBOOT', checksum(self.inputs['EBOOT.ELF'])), \
                mock.patch.object(nightly, 'abi_fingerprint', side_effect=fingerprint):
            nightly.fetch_inputs(output, 'public/manifest', self.root, platform)
            nightly.verify_overlays(output, self.root, platform)
        return output

    def test_schema_two_restores_exact_native_cache_for_every_platform(self):
        for platform in nightly_formats.PLATFORMS:
            with self.subTest(platform=platform):
                self.requested.clear()
                output = self.fetch(platform)
                self.assertEqual({file.name for file in (output / 'overlays').iterdir()},
                                 {('lib' if platform == 'android-arm64' else '') + f'ovl{i:04}.{suffix(platform)}'
                                  for i in range(355)} if platform != 'linux-x86_64' else
                                 {f'overlay_{i:04}.so' for i in range(355)})
                for name, expected in self.inputs.items():
                    self.assertEqual((output / name).read_bytes(), expected)
                self.assertEqual([key for bucket, key in self.requested if bucket == nightly.CACHE_BUCKET],
                                 [self.manifest['overlay_caches'][platform]['key']])
                marker = json.loads((output / 'overlay-files.json').read_text())
                self.assertEqual(marker['platform'], platform)
                self.assertEqual(marker['toolchain'], nightly.TOOLCHAINS[platform])
                self.assertEqual(len(marker['files']), 355)
                self.assertFalse((output / 'public-seed-metadata.txt').exists())

    def test_toolchain_provenance_catalog_and_container_corruption_fail_closed(self):
        platform = 'windows-x86_64'
        original = copy.deepcopy(self.manifest)
        mutations = [lambda m: m.update(schema=99), lambda m: m.update(overlay_caches=[]),
                     lambda m: m['overlay_caches'].pop(platform),
                     lambda m: m['overlay_caches'][platform].update(toolchain='msvc-unreviewed'),
                     lambda m: m['overlay_caches'][platform].update(source_commit='main'),
                     lambda m: m['overlay_caches'][platform].update(source_commit='c' * 40),
                     lambda m: m['overlay_caches'][platform].update(abi_sha256='c' * 64),
                     lambda m: m['overlay_caches'][platform].update(filename='../archive.zip'),
                     lambda m: m['overlay_caches'][platform].update(filename='yakumo-fixture-android-arm64.apk'),
                     lambda m: m['overlay_caches'][platform].update(size=0),
                     lambda m: m['overlay_caches'][platform].update(sha256='c' * 64)]
        for index, change in enumerate(mutations):
            self.manifest = copy.deepcopy(original)
            change(self.manifest)
            target = self.base / f'rejected-{index}'
            with self.subTest(index=index), self.assertRaises(nightly.Rejected):
                self.fetch(platform, target)
            self.assertFalse(target.exists())
        self.manifest = []
        with self.assertRaises(nightly.Rejected):
            self.fetch(platform, self.base / 'rejected-nonobject')
        self.manifest = original
        item = original['overlay_caches'][platform]
        self.objects[nightly.CACHE_BUCKET, item['key']] += b'PRIVATE APPENDIX STAND-IN'
        with self.assertRaises(nightly.Rejected):
            self.fetch(platform, self.base / 'rejected-container')
        self.assertFalse((self.base / 'rejected-container').exists())

    def test_legacy_manifest_remains_linux_only_and_unknown_platform_creates_nothing(self):
        cache = self.manifest['overlay_caches']['linux-x86_64']
        self.manifest = {'schema': 1, 'files': self.manifest['files'], 'overlay_cache': cache}
        self.fetch('linux-x86_64')
        for platform in ['macos-arm64', 'windows-x86_64', 'android-arm64', 'unknown-platform']:
            with self.subTest(platform=platform), self.assertRaises(nightly.Rejected):
                self.fetch(platform)
            self.assertFalse((self.base / ('restored-' + platform)).exists())


class InstallInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root, self.inputs = self.base / 'checkout', self.base / 'private'
        self.inputs.mkdir()
        for name in ['EBOOT.ELF', 'disc.iso']:
            (self.inputs / name).write_bytes(b'PUBLIC ' + name.encode())
        self.game = self.root / 'profiles/mhp3rd/game'

    def test_empty_checkout_is_prepared_idempotently_without_copying_other_files(self):
        nightly.install_inputs(self.inputs, self.root)
        nightly.install_inputs(self.inputs, self.root)
        self.assertEqual({file.name for file in self.game.iterdir()}, {'EBOOT.ELF', 'disc.iso'})
        for name in ['EBOOT.ELF', 'disc.iso']:
            self.assertTrue(os.path.samefile(self.game / name, self.inputs / name))

    def test_unrelated_existing_pair_is_preserved_and_first_new_link_is_rolled_back(self):
        self.game.mkdir(parents=True)
        old = self.game / 'disc.iso';old.write_bytes(b'KEEP DEVELOPER DISC')
        with self.assertRaises(nightly.Rejected):
            nightly.install_inputs(self.inputs, self.root)
        self.assertEqual(old.read_bytes(), b'KEEP DEVELOPER DISC')
        self.assertFalse((self.game / 'EBOOT.ELF').exists())
        self.assertFalse((self.game / 'EBOOT.ELF').is_symlink())

    @unittest.skipUnless(os.name == 'posix', 'POSIX symlink ownership contract')
    def test_game_directory_and_unrelated_input_links_are_never_followed(self):
        external = self.base / 'external';external.mkdir()
        self.game.parent.mkdir(parents=True)
        self.game.symlink_to(external, target_is_directory=True)
        with self.assertRaises(nightly.Rejected):
            nightly.install_inputs(self.inputs, self.root)
        self.assertEqual(list(external.iterdir()), [])
        self.game.unlink();self.game.mkdir()
        (self.game / 'EBOOT.ELF').symlink_to(external / 'absent')
        with self.assertRaises(nightly.Rejected):
            nightly.install_inputs(self.inputs, self.root)
        self.assertEqual((self.game / 'EBOOT.ELF').readlink(), external / 'absent')

    def test_missing_second_source_rolls_back_only_the_link_it_created(self):
        (self.inputs / 'disc.iso').unlink()
        with self.assertRaises(nightly.Rejected):
            nightly.install_inputs(self.inputs, self.root)
        self.assertEqual(list(self.game.iterdir()), [])
        self.assertEqual((self.inputs / 'EBOOT.ELF').read_bytes(), b'PUBLIC EBOOT.ELF')

    def test_windows_hard_links_are_idempotent_and_failure_rolls_back(self):
        windows_os = SimpleNamespace(name='nt', link=os.link, path=os.path)
        with mock.patch.object(nightly, 'os', windows_os):
            nightly.install_inputs(self.inputs, self.root)
            nightly.install_inputs(self.inputs, self.root)
        for name in ['EBOOT.ELF', 'disc.iso']:
            self.assertFalse((self.game / name).is_symlink())
            self.assertTrue(os.path.samefile(self.game / name, self.inputs / name))
            (self.game / name).unlink()
        calls = []

        def link(source, target):
            calls.append(target)
            if len(calls) == 2:
                raise OSError('PUBLIC synthetic hard-link failure')
            os.link(source, target)

        windows_os.link = link
        with mock.patch.object(nightly, 'os', windows_os), self.assertRaises(OSError):
            nightly.install_inputs(self.inputs, self.root)
        self.assertEqual(list(self.game.iterdir()), [])
        self.assertEqual(len(calls), 2)


class MarkerIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        patch = mock.patch.object(nightly, 'abi_fingerprint', return_value=FINGERPRINT)
        patch.start();self.addCleanup(patch.stop)

    def fixture(self, platform, label):
        inputs = self.base / label
        (inputs / 'overlays').mkdir(parents=True)
        for number in range(355):
            (inputs / f'overlays/ovl{number:04}.{suffix(platform)}').write_bytes(native(platform))
        nightly.write_overlay_marker(inputs, platform)
        return inputs

    def test_all_platform_markers_verify_exact_content_and_native_architecture(self):
        for platform in nightly_formats.PLATFORMS:
            with self.subTest(platform=platform):
                inputs = self.fixture(platform, platform)
                nightly.verify_overlays(inputs, platform=platform)
                target = next((inputs / 'overlays').iterdir())
                target.write_bytes(b'~PSP' + bytes(200))
                marker = json.loads((inputs / 'overlay-files.json').read_text())
                marker['files'][target.name] = nightly.digest(target)
                (inputs / 'overlay-files.json').write_text(json.dumps(marker))
                with self.assertRaises(nightly.Rejected):
                    nightly.verify_overlays(inputs, platform=platform)

    def test_platform_toolchain_abi_file_set_and_hash_corruption_are_rejected(self):
        inputs = self.fixture('android-arm64', 'mutations')
        marker_file = inputs / 'overlay-files.json'
        original = marker_file.read_text()
        changes = [lambda m: m.update(schema=2), lambda m: m.update(platform='linux-x86_64'),
                   lambda m: m.update(toolchain='ndk-unreviewed'), lambda m: m.update(abi_sha256='c' * 64),
                   lambda m: m['files'].pop(next(iter(m['files']))),
                   lambda m: m['files'].update({'outside.so': 'c' * 64}),
                   lambda m: m['files'].update({next(iter(m['files'])): 'bad-checksum'}),
                   lambda m: m['files'].update({next(iter(m['files'])): 'c' * 64})]
        for index, change in enumerate(changes):
            marker = json.loads(original);change(marker);marker_file.write_text(json.dumps(marker))
            with self.subTest(index=index), self.assertRaises(nightly.Rejected):
                nightly.verify_overlays(inputs, platform='android-arm64')
        marker_file.write_text(original)
        for name, content in [('overlay-abi.sha256', 'c' * 64), ('overlay-platform.txt', 'windows-x86_64')]:
            file = inputs / name;before = file.read_text();file.write_text(content)
            with self.subTest(name=name), self.assertRaises(nightly.Rejected):
                nightly.verify_overlays(inputs, platform='android-arm64')
            file.write_text(before)
        marker_file.write_bytes(bytes(128 * 1024 + 1))
        with self.assertRaises(nightly.Rejected):
            nightly.verify_overlays(inputs, platform='android-arm64')
        with self.assertRaises(nightly.Rejected):
            nightly.verify_overlays(inputs, platform='unsupported')

    @unittest.skipUnless(os.name == 'posix', 'POSIX link ownership contract')
    def test_marker_directory_and_library_symlinks_are_rejected(self):
        inputs = self.fixture('linux-x86_64', 'links')
        overlays = inputs / 'overlays'
        target = next(overlays.iterdir())
        original = target.read_bytes();external = self.base / 'external.so';external.write_bytes(original)
        target.unlink();target.symlink_to(external)
        with self.assertRaises(nightly.Rejected):
            nightly.verify_overlays(inputs)
        target.unlink();target.write_bytes(original)
        real = inputs / 'real-overlays';overlays.rename(real);overlays.symlink_to(real, target_is_directory=True)
        with self.assertRaises(nightly.Rejected):
            nightly.verify_overlays(inputs)


class PublicationIntegrationTests(Fixture):
    def windows_pin(self):
        name = 'ffmpeg-' + 'c' * 40 + '.tar.gz'
        source = self.base / name;source.write_bytes(b'PUBLIC EXACT WINDOWS SOURCE')
        pin = self.root / 'scripts/ci/nightly_windows_sources.sh';pin.parent.mkdir(parents=True, exist_ok=True)
        pin.write_text(f'NIGHTLY_WINDOWS_FFMPEG_SOURCE_NAME={name}\n'
                       f'NIGHTLY_WINDOWS_FFMPEG_SOURCE_SHA256={nightly.digest(source)}\n')
        return source

    def test_source_pins_select_exact_upstream_source_and_reject_bad_windows_metadata(self):
        source = self.windows_pin()
        for platform in nightly_formats.PLATFORMS:
            expected = (source.name, nightly.digest(source)) if platform == 'windows-x86_64' else (self.source_file.name, nightly.digest(self.source_file))
            self.assertEqual(nightly.source_pin(platform, self.root), expected)
        file = self.root / 'scripts/ci/nightly_windows_sources.sh'
        original = file.read_text()
        for text in ['', original.replace('c' * 40, '../not-a-source'), original.replace(nightly.digest(source), 'bad-hash')]:
            file.write_text(text)
            with self.assertRaises(nightly.Rejected):
                nightly.source_pin('windows-x86_64', self.root)

    def test_each_dispatcher_publishes_only_canonical_output_and_matching_metadata(self):
        windows = self.windows_pin()
        for platform in nightly_formats.PLATFORMS:
            with self.subTest(platform=platform):
                package = self.package if platform == 'linux-x86_64' else self.base / nightly_formats.package_name(platform, 'fixture')
                if platform != 'linux-x86_64':
                    package.write_bytes(b'UNCHECKED PRODUCER BYTES')
                source = windows if platform == 'windows-x86_64' else self.source_file
                output = self.base / ('publication-' + platform)

                def canonical(path, target, actual_platform, root, revision):
                    self.assertEqual((path, actual_platform, root, revision), (package, platform, self.root, SOURCE))
                    Path(target).write_bytes(b'APPROVED CANONICAL BYTES')

                with mock.patch.object(nightly_formats, 'canonical_package', side_effect=canonical) as guard:
                    nightly.stage_public(package, source, output, SOURCE, self.root, platform)
                self.assertEqual(guard.call_count, 0 if platform == 'linux-x86_64' else 1)
                self.assertEqual({p.name for p in output.iterdir()},
                                 {package.name, source.name, 'build.json', 'SHA256SUMS', 'release-notes.md'})
                manifest = json.loads((output / 'build.json').read_text())
                self.assertEqual(manifest['platform'], platform)
                self.assertEqual(manifest['android_signing'], 'required' if platform == 'android-arm64' else 'not-applicable')
                for entry in manifest['files']:
                    self.assertEqual(entry['sha256'], nightly.digest(output / entry['name']))
                if platform != 'linux-x86_64':
                    self.assertNotEqual((output / package.name).read_bytes(), package.read_bytes())

    def test_failed_platform_guard_removes_only_its_new_output_directory(self):
        package = self.base / nightly_formats.package_name('android-arm64', 'fixture')
        package.write_bytes(b'PUBLIC CONTAINER FIXTURE')
        output = self.base / 'failed-publication'

        def reject(path, target, *args):
            Path(target).write_bytes(b'PARTIAL PUBLIC OUTPUT')
            raise nightly.Rejected('PUBLIC synthetic guard rejection')

        with mock.patch.object(nightly_formats, 'canonical_package', side_effect=reject), self.assertRaises(nightly.Rejected):
            nightly.stage_public(package, self.source_file, output, SOURCE, self.root, 'android-arm64')
        self.assertFalse(output.exists())
        self.assertEqual(package.read_bytes(), b'PUBLIC CONTAINER FIXTURE')
        output.mkdir();sentinel = output / 'keep';sentinel.write_bytes(b'KEEP DEVELOPER DATA')
        with self.assertRaises(nightly.Rejected):
            nightly.stage_public(package, self.source_file, output, SOURCE, self.root, 'android-arm64')
        self.assertEqual(sentinel.read_bytes(), b'KEEP DEVELOPER DATA')


class WindowsPrivateProcessTests(unittest.TestCase):
    def run_timeout(self, taskkill_error=None):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'private.log'
            child = mock.Mock(pid=4567)
            child.wait.side_effect = [subprocess.TimeoutExpired('PUBLIC fixture builder', 1),
                                      subprocess.TimeoutExpired('PUBLIC fixture builder', 5), 0]
            child.poll.return_value = None
            windows_os = SimpleNamespace(name='nt', chmod=os.chmod)
            with mock.patch.object(nightly, 'os', windows_os), \
                    mock.patch.object(nightly.subprocess, 'CREATE_NEW_PROCESS_GROUP', 0x200, create=True), \
                    mock.patch.object(nightly.subprocess, 'CREATE_NO_WINDOW', 0x8000000, create=True), \
                    mock.patch.object(nightly.subprocess, 'Popen', return_value=child) as popen, \
                    mock.patch.object(nightly.subprocess, 'run', side_effect=taskkill_error) as kill:
                with self.assertRaises(subprocess.TimeoutExpired):
                    nightly.private_run(log, ['PUBLIC trusted builder'], 1)
                self.assertEqual(popen.call_args.kwargs['creationflags'], 0x8000200)
                self.assertNotIn('start_new_session', popen.call_args.kwargs)
                self.assertEqual(kill.call_args.args[0], ['taskkill', '/PID', '4567', '/T', '/F'])
                self.assertIs(kill.call_args.kwargs['stdout'], subprocess.DEVNULL)
                self.assertIs(kill.call_args.kwargs['stderr'], subprocess.DEVNULL)
                child.kill.assert_called_once_with()
                self.assertEqual(child.wait.call_count, 3)

    def test_windows_timeout_targets_only_owned_pid_and_uses_fallback_cleanup(self):
        self.run_timeout()

    def test_taskkill_failure_cannot_skip_owned_process_cleanup(self):
        self.run_timeout(OSError('PUBLIC taskkill fixture failure'))
        self.run_timeout(subprocess.TimeoutExpired('taskkill', 15))

    def test_additional_cli_commands_dispatch_without_private_error_output(self):
        for argv, function in [(['source-name', '--platform', 'windows-x86_64'], 'source_pin'),
                               (['install-inputs', '--inputs', 'private'], 'install_inputs'),
                               (['build-platform', '--platform', 'android-arm64', '--inputs', 'inputs',
                                 '--work', 'work', '--output', 'public', '--source-sha', SOURCE, '--probe'], 'build_platform')]:
            with mock.patch.object(nightly, function, return_value=('ffmpeg-public.tar.gz', 'a' * 64)) as call, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(nightly.main(argv), 0)
                call.assert_called_once()
            with mock.patch.object(nightly, function, side_effect=OSError('PRIVATE CANARY')), contextlib.redirect_stderr(io.StringIO()) as error:
                self.assertEqual(nightly.main(argv), 1)
                self.assertNotIn('PRIVATE CANARY', error.getvalue())

    def test_windows_environment_is_forwarded_and_unsupported_os_creates_no_log(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'private.log'
            child = mock.Mock()
            child.wait.return_value = 0;child.poll.return_value = 0
            env = {'PUBLIC_FIXTURE': 'approved explicit environment'}
            with mock.patch.object(nightly, 'os', SimpleNamespace(name='nt', chmod=os.chmod)), \
                    mock.patch.object(nightly.subprocess, 'CREATE_NEW_PROCESS_GROUP', 0x200, create=True), \
                    mock.patch.object(nightly.subprocess, 'CREATE_NO_WINDOW', 0x8000000, create=True), \
                    mock.patch.object(nightly.subprocess, 'Popen', return_value=child) as popen:
                nightly.private_run(log, ['PUBLIC builder'], 1, env=env)
                self.assertIs(popen.call_args.kwargs['env'], env)
            with mock.patch.object(nightly, 'os', SimpleNamespace(name='unsupported')), self.assertRaises(nightly.Rejected):
                nightly.private_run(Path(directory) / 'not-created.log', ['PUBLIC builder'], 1)
            self.assertFalse((Path(directory) / 'not-created.log').exists())


class BuildPlatformTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root, self.inputs, self.work, self.output = [base / name for name in ['source', 'inputs', 'work', 'public']]
        self.source_name = 'ffmpeg-public-source.tar.xz'

    def test_trusted_dispatch_uses_exact_platform_builder_and_clears_probe_override(self):
        for platform in nightly_formats.PLATFORMS:
            with self.subTest(platform=platform), \
                    mock.patch.dict(os.environ, {'NIGHTLY_PROBE': '1', 'YAKUMO_CONTAINER': 'podman',
                                                      'NIGHTLY_VERSION': '--untrusted-version'}), \
                    mock.patch.object(nightly, 'install_inputs') as install, \
                    mock.patch.object(nightly, 'private_run') as run, \
                    mock.patch.object(nightly, 'source_pin', return_value=(self.source_name, 'c' * 64)), \
                    mock.patch.object(nightly, 'stage_public') as stage:
                nightly.build_platform(self.inputs, self.work, self.output, SOURCE, platform, root=self.root)
                install.assert_called_once_with(self.inputs.resolve(), self.root)
                self.assertEqual(run.call_count, 1)
                log, command, timeout = run.call_args.args
                self.assertEqual((log, timeout), (self.work.parent / 'build.log', 16800))
                env = run.call_args.kwargs['env']
                self.assertNotIn('NIGHTLY_PROBE', env)
                self.assertEqual(env['NIGHTLY_VERSION'], 'nightly-' + SOURCE[:12])
                self.assertEqual(env['YAKUMO_PRIVATE_INPUTS'], self.inputs.resolve().as_posix())
                self.assertEqual(env['YAKUMO_WORK'], self.work.as_posix())
                self.assertEqual(os.environ['NIGHTLY_PROBE'], '1')
                self.assertEqual(os.environ['NIGHTLY_VERSION'], '--untrusted-version')
                if platform == 'windows-x86_64':
                    self.assertEqual(command, ['pwsh', '-NoProfile', '-File', str(self.root / 'scripts/ci/nightly_windows.ps1')])
                elif platform == 'linux-x86_64':
                    self.assertEqual(env['YAKUMO_CONTAINER'], 'docker')
                    self.assertEqual(command, ['bash', str(self.root / 'profiles/mhp3rd/scripts/release_linux.sh'),
                                               '--version', 'nightly-' + SOURCE[:12], '--jobs', '2', '--no-flatpak'])
                else:
                    name = 'nightly_macos.sh' if platform == 'macos-arm64' else 'nightly_android.sh'
                    self.assertEqual(command, ['bash', str(self.root / 'scripts/ci' / name)])
                stage.assert_called_once_with(self.work / 'dist' / nightly_formats.package_name(platform, 'nightly-' + SOURCE[:12]),
                                              self.work / ('sources' if platform == 'windows-x86_64' else 'dist') / self.source_name,
                                              self.output, SOURCE, self.root, platform)

    def test_public_fixture_preparation_is_private_and_windows_is_deferred_to_vs_wrapper(self):
        for platform in nightly_formats.PLATFORMS:
            with self.subTest(platform=platform), mock.patch.object(nightly, 'install_inputs') as install, \
                    mock.patch.object(nightly, 'private_run') as run, \
                    mock.patch.object(nightly, 'source_pin', return_value=(self.source_name, 'c' * 64)), \
                    mock.patch.object(nightly, 'stage_public'):
                nightly.build_platform(self.inputs, self.work, self.output, SOURCE, platform, probe=True, root=self.root)
                install.assert_not_called()
                self.assertEqual(run.call_count, 1 if platform == 'windows-x86_64' else 2)
                for call in run.call_args_list:
                    self.assertEqual(call.kwargs['env']['NIGHTLY_PROBE'], '1')
                if platform != 'windows-x86_64':
                    prepare = run.call_args_list[0]
                    self.assertEqual(prepare.args, (self.work.parent / 'probe.log',
                        [sys.executable, str(self.root / 'scripts/ci/nightly_probe.py'), '--platform', platform,
                         '--output', str(self.inputs.resolve()), '--install'], 600))

    def test_invalid_identity_and_existing_developer_inputs_stop_before_building(self):
        for source, platform in [('main', 'linux-x86_64'), (SOURCE, 'unknown')]:
            with self.subTest(source=source, platform=platform), \
                    mock.patch.object(nightly, 'install_inputs') as install, \
                    mock.patch.object(nightly, 'private_run') as run, self.assertRaises(nightly.Rejected):
                nightly.build_platform(self.inputs, self.work, self.output, source, platform, root=self.root)
            install.assert_not_called();run.assert_not_called()
        with mock.patch.object(nightly, 'install_inputs', side_effect=nightly.Rejected('existing data')), \
                mock.patch.object(nightly, 'private_run') as run, \
                mock.patch.object(nightly, 'stage_public') as stage, self.assertRaises(nightly.Rejected):
            nightly.build_platform(self.inputs, self.work, self.output, SOURCE, 'linux-x86_64', root=self.root)
        run.assert_not_called();stage.assert_not_called()

    def test_failed_probe_or_build_cannot_stage_any_public_artifact(self):
        for probe in [False, True]:
            with self.subTest(probe=probe), mock.patch.object(nightly, 'install_inputs'), \
                    mock.patch.object(nightly, 'private_run', side_effect=nightly.Rejected('private command failed')) as run, \
                    mock.patch.object(nightly, 'stage_public') as stage, self.assertRaises(nightly.Rejected):
                nightly.build_platform(self.inputs, self.work, self.output, SOURCE, 'android-arm64', probe=probe, root=self.root)
            self.assertEqual(run.call_count, 1)
            stage.assert_not_called()

    def test_inspection_trace_is_private_and_never_forwarded_to_console_or_public_output(self):
        for probe in [False, True]:
            with self.subTest(probe=probe), mock.patch.object(nightly, 'install_inputs'), \
                    mock.patch.object(nightly, 'private_run'), \
                    mock.patch.object(nightly, 'source_pin', return_value=(self.source_name, 'c' * 64)), \
                    mock.patch.object(nightly, 'stage_public', side_effect=nightly.Rejected('PRIVATE inspection canary')), \
                    contextlib.redirect_stdout(io.StringIO()) as output, \
                    contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(nightly.Rejected):
                    nightly.build_platform(self.inputs, self.work, self.output, SOURCE, 'android-arm64', probe=probe, root=self.root)
                self.assertEqual(output.getvalue() + error.getvalue(), '')
                self.assertFalse(self.output.exists())
                log = self.work.parent / 'inspection.log'
                self.assertIn('PRIVATE inspection canary', log.read_text())
                self.assertIn('Traceback', log.read_text())
                if os.name == 'posix':
                    self.assertEqual(log.stat().st_mode & 0o777, 0o600)
                log.unlink()

    @unittest.skipUnless(os.name == 'posix', 'POSIX prepared-input symlink contract')
    def test_installed_link_targets_match_builder_environment_for_aliased_input_roots(self):
        canonical = self.inputs.parent / 'actual-inputs'
        canonical.mkdir()
        for name in ['EBOOT.ELF', 'disc.iso']:
            (canonical / name).write_bytes(b'PUBLIC INPUT FIXTURE')
        self.inputs.symlink_to(canonical, target_is_directory=True)

        def inspect_builder(log, command, timeout, env):
            for name in ['EBOOT.ELF', 'disc.iso']:
                prepared = self.root / 'profiles/mhp3rd/game' / name
                # Both POSIX cross-platform builders enforce this literal
                # ownership contract before reusing an already prepared pair.
                self.assertEqual(prepared.readlink(), Path(env['YAKUMO_PRIVATE_INPUTS']) / name)

        for platform in ['macos-arm64', 'android-arm64']:
            with self.subTest(platform=platform), mock.patch.object(nightly, 'private_run', side_effect=inspect_builder), \
                    mock.patch.object(nightly, 'source_pin', return_value=(self.source_name, 'c' * 64)), \
                    mock.patch.object(nightly, 'stage_public'):
                nightly.build_platform(self.inputs, self.work, self.output, SOURCE, platform, root=self.root)


if __name__ == '__main__':
    unittest.main()
