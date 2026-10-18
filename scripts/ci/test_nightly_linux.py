"""Public synthetic contracts for Linux release container selection."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import nightly_probe


ROOT = Path(__file__).resolve().parents[2]
STOP_AFTER_CONTAINER = 61

# Run the real release script up to its container invocation. These external
# tool doubles never open game originals, start a daemon, or compile a corpus.
DRIVER = r'''
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name in ('docker', 'podman'):
    with open(os.environ['CONTAINER_CALLS'], 'a') as stream:
        stream.write(json.dumps([name, *args]) + '\n')
    sys.exit(61)
elif name == 'git':
    if 'log' in args:
        print('1')
elif name == 'sha256sum':
    # The executable fixture is public text; only isolate the checksum tool's
    # contract so the actual engine selection and invocation remain exercised.
    print('55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c  ' + args[0])
'''


def modern_bash():
    """These Linux/Windows release scripts require Bash 4+, unlike macOS Bash 3."""
    if os.name != 'posix':
        return None
    for candidate in [shutil.which('bash'), '/opt/homebrew/bin/bash', '/usr/local/bin/bash']:
        if candidate and Path(candidate).is_file():
            result = subprocess.run([candidate, '-c', '(( BASH_VERSINFO[0] >= 4 ))'],
                                    capture_output=True, timeout=5)
            if result.returncode == 0:
                return candidate
    return None


BASH = modern_bash()


@unittest.skipUnless(BASH, 'Bash 4+ is required for Linux/Windows release shell contracts')
class LinuxContainerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='yakumo linux fixture ')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'source with spaces'
        self.work = self.base / 'private work'
        self.tools = self.base / 'tools'
        self.tools.mkdir()
        for relative in [
                'profiles/mhp3rd/scripts/release_linux.sh',
                'profiles/mhp3rd/packaging/linux/sources.sh',
                'profiles/mhp3rd/packaging/linux/io.github.teamgdb.Yakumo.yml',
                'profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md',
                'profiles/mhp3rd/cmake/FFmpeg.cmake']:
            destination = self.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        game = self.repo / 'profiles/mhp3rd/game'
        game.mkdir()
        for name in ['EBOOT.ELF', 'disc.iso']:
            (game / name).write_bytes(b'PUBLIC SYNTHETIC FIXTURE, NOT GAME CONTENT')
        # A closed PATH makes installed/missing engine behavior independent of
        # whether Docker or Podman happens to be available on the test machine.
        for name in ['dirname', 'sed', 'head', 'grep', 'tr', 'cut', 'id', 'mkdir']:
            executable = shutil.which(name)
            self.assertIsNotNone(executable, name)
            (self.tools / name).symlink_to(executable)
        for name in ['git', 'sha256sum', 'docker', 'podman']:
            tool = self.tools / name
            tool.write_text('#!' + sys.executable + '\n' + DRIVER)
            tool.chmod(0o755)
        self.log = self.base / 'containers.jsonl'
        self.env = {**os.environ, 'PATH': str(self.tools), 'YAKUMO_WORK': str(self.work),
                    'CONTAINER_CALLS': str(self.log)}
        self.env.pop('YAKUMO_CONTAINER', None)
        self.env.pop('YAKUMO_PRIVATE_INPUTS', None)
        self.env.pop('NIGHTLY_PROBE', None)
        self.bash = BASH

    def run_release(self, engine=None):
        env = dict(self.env)
        if engine is not None:
            env['YAKUMO_CONTAINER'] = engine
        return subprocess.run([self.bash, str(self.repo / 'profiles/mhp3rd/scripts/release_linux.sh'),
                               '--version', 'nightly-0123456789ab', '--no-flatpak'],
                              env=env, capture_output=True, text=True, timeout=15)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def assert_engine(self, engine, result):
        self.assertEqual(result.returncode, STOP_AFTER_CONTAINER, result.stdout + result.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 1)
        invocation = calls[0]
        self.assertEqual(invocation[:3], [engine, 'run', '--rm'])
        self.assertIn(str(self.repo) + ':' + str(self.repo), invocation)
        self.assertIn(str(self.work) + ':' + str(self.work), invocation)
        self.assertIn('YAKUMO_WORK=' + str(self.work), invocation)
        self.assertEqual(invocation[-2:], ['bash', str(self.repo / 'profiles/mhp3rd/packaging/linux/build_in_sdk.sh')])
        self.assertTrue(any(value.startswith('registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:')
                            for value in invocation))
        if engine == 'docker':
            self.assertIn('--user', invocation)
            self.assertNotIn('--userns=keep-id', invocation)
        else:
            self.assertIn('--userns=keep-id', invocation)
            self.assertNotIn('--user', invocation)

    def test_public_probe_accepts_only_exact_toy_elf_and_never_replaces_release_identity(self):
        checksum = self.tools / 'sha256sum'
        checksum.write_text('#!' + sys.executable + '\nimport hashlib, pathlib, sys\n'
                            "print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest() + '  ' + sys.argv[1])\n")
        game = self.repo / 'profiles/mhp3rd/game/EBOOT.ELF'
        game.write_bytes(nightly_probe.synthetic_elf())
        result = self.run_release('docker')
        self.assertNotEqual(result.returncode, STOP_AFTER_CONTAINER)
        self.assertEqual(self.calls(), [])
        self.env['NIGHTLY_PROBE'] = '1'
        self.assert_engine('docker', self.run_release('docker'))
        self.log.unlink()
        game.write_bytes(nightly_probe.synthetic_elf() + b'UNAPPROVED')
        result = self.run_release('docker')
        self.assertNotEqual(result.returncode, STOP_AFTER_CONTAINER)
        self.assertEqual(self.calls(), [])

    def test_explicit_engine_is_used_when_both_are_installed(self):
        for engine in ['docker', 'podman']:
            with self.subTest(engine=engine):
                self.log.unlink(missing_ok=True)
                self.assert_engine(engine, self.run_release(engine))

    def test_unset_engine_preserves_podman_preference_and_docker_fallback(self):
        self.assert_engine('podman', self.run_release())
        self.log.unlink()
        (self.tools / 'podman').unlink()
        self.assert_engine('docker', self.run_release())

    def test_missing_explicit_engine_never_falls_back(self):
        for engine in ['docker', 'podman']:
            with self.subTest(engine=engine):
                path = self.tools / engine
                hidden = self.tools / ('disabled-' + engine)
                path.rename(hidden)
                result = self.run_release(engine)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(engine + ' requested by YAKUMO_CONTAINER is not installed', result.stderr)
                self.assertEqual(self.calls(), [])
                hidden.rename(path)

    def test_nonexecutable_explicit_engine_is_rejected(self):
        for engine in ['docker', 'podman']:
            with self.subTest(engine=engine):
                path = self.tools / engine
                path.chmod(0o644)
                result = self.run_release(engine)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(engine + ' requested by YAKUMO_CONTAINER is not installed', result.stderr)
                self.assertEqual(self.calls(), [])
                path.chmod(0o755)

    def test_invalid_explicit_engine_is_rejected_without_execution(self):
        for engine in ['other', 'docker --privileged', '/usr/bin/docker', 'podman\n--remote']:
            with self.subTest(engine=engine):
                result = self.run_release(engine)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('YAKUMO_CONTAINER must be docker or podman', result.stderr)
                self.assertEqual(self.calls(), [])

    def test_missing_default_engines_reports_requirement(self):
        for engine in ['docker', 'podman']:
            (self.tools / engine).unlink()
        result = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('podman or docker is needed to run the build SDK', result.stderr)
        self.assertEqual(self.calls(), [])


if __name__ == '__main__':
    unittest.main()
