"""Regression checks for save isolation and bounded visual-test execution."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'translation_smoke.py'


@unittest.skipIf(os.name == 'nt', 'Fixture executable and input symlinks require a POSIX host')
class VisualLauncherTests(unittest.TestCase):
    def test_timeout_preserves_source_and_never_claims_visual_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game, source, overlays = [root / name for name in ['game', 'source', 'overlays']]
            for directory in [game, source / 'ms0', overlays]:
                directory.mkdir(parents=True)
            (game / 'EBOOT.ELF').write_bytes(b'invented executable input')
            (game / 'disc.iso').write_bytes(b'invented disc input')
            (source / 'ms0' / 'save').write_bytes(b'protected save')
            (source / 'settings.ini').write_text('text.language=original\n', encoding='utf-8')
            translation = root / 'test.lang'
            translation.write_text('language = test\n[16]\n2:1 = Invented\n', encoding='utf-8')
            executable = root / 'fake-game'
            executable.write_text(f'#!{sys.executable}\n' + '''import os, pathlib, time
assert 'MHP3RD_AUTO_CONFIRM' not in os.environ
assert 'MHP3RD_TEXT_SEARCH_UNLIMITED' not in os.environ
p = pathlib.Path(os.environ['MHP3RD_DATA_DIR'])
(p / 'ms0' / 'save').write_bytes(b'changed disposable save')
(p / 'settings.ini').write_text('changed disposable settings')
print('synthetic launch', flush=True)
time.sleep(20)
''', encoding='utf-8')
            executable.chmod(0o755)
            env = dict(os.environ, MHP3RD_AUTO_CONFIRM='1', MHP3RD_TEXT_SEARCH_UNLIMITED='1')
            result = subprocess.run([sys.executable, str(SCRIPT), '--binary', str(executable),
                '--game-dir', str(game), '--data-dir', str(source), '--overlays', str(overlays),
                '--translation', str(translation), '--language', 'test', '--commit', 'test-fixture',
                '--seconds', '1', '--output-parent', str(root)], env=env,
                capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = next(root.glob('yakumo-quest-translation-*'))
            manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
            self.assertTrue(manifest['timed_out'])
            self.assertLess(manifest['duration_seconds'], 10)
            self.assertEqual(manifest['visual_verdict'], 'NOT TESTED')
            self.assertEqual(manifest['captures'], [])
            self.assertEqual((source / 'ms0' / 'save').read_bytes(), b'protected save')
            self.assertEqual((source / 'settings.ini').read_text(), 'text.language=original\n')
            self.assertIn('synthetic launch', (run / 'run.log').read_text())
            self.assertEqual((run / 'data' / 'ms0' / 'save').read_bytes(), b'changed disposable save')
            self.assertEqual((run / 'translations' / 'selected.lang').read_bytes(), translation.read_bytes())

    def test_invalid_deadline_rejected_before_preparing_data(self):
        result = subprocess.run([sys.executable, str(SCRIPT), '--binary', 'missing',
            '--game-dir', 'missing', '--data-dir', 'missing', '--overlays', 'missing',
            '--translation', 'missing', '--language', 'test', '--commit', 'fixture',
            '--seconds', '601'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn('between 1 and 600', result.stderr)


@unittest.skipIf(os.name == 'nt', 'Fixture executable and input symlinks require a POSIX host')
class LauncherPathTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game = self.root / 'game'
        self.source = self.root / 'source'
        self.overlays = self.root / 'overlays'
        for directory in (self.game, self.source / 'ms0', self.overlays):
            directory.mkdir(parents=True)
        self.protected = self.root / 'protected'
        self.protected.write_bytes(b'protected input')
        for name in ('EBOOT.ELF', 'disc.iso'):
            (self.game / name).symlink_to(self.protected)
        (self.source / 'ms0' / 'save').write_bytes(b'protected save')
        self.translation = self.root / 'translation with spaces.lang'
        self.translation.write_text('language = test\n[16]\n2:1 = Invented\n', encoding='utf-8')
        self.binary = self.root / 'fake-game'
        self.binary.write_text(f'#!{sys.executable}\n' + '''import os, pathlib
assert os.environ['MHP3RD_LANGUAGE'] == 'test'
assert os.environ['MHP3RD_TEXT_SEARCH_UNLIMITED'] == '1'
assert pathlib.Path(os.environ['MHP3RD_FONT']).is_file()
assert pathlib.Path(os.environ['MHP3RD_GAME_DIR'], 'disc.iso').read_bytes() == b'protected input'
pathlib.Path(os.environ['MHP3RD_SCREENSHOT_DIR'], 'fixture.bmp').write_bytes(b'invented capture')
''', encoding='utf-8')
        self.binary.chmod(0o755)
        self.font = self.root / 'fixture font'
        self.font.write_bytes(b'invented font')

    def launch(self, *extra):
        return subprocess.run([sys.executable, str(SCRIPT), '--binary', str(self.binary),
            '--game-dir', str(self.game), '--data-dir', str(self.source),
            '--overlays', str(self.overlays), '--translation', str(self.translation),
            '--language', 'test', '--commit', 'fixture', '--seconds', '2',
            '--font', str(self.font), '--unlimited', '--output-parent', str(self.root),
            *extra], capture_output=True, text=True, timeout=10)

    def assert_rejected_before_output(self, expected):
        result = self.launch()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(expected, result.stderr)
        self.assertEqual(list(self.root.glob('yakumo-quest-translation-*')), [])
        self.assertEqual(self.protected.read_bytes(), b'protected input')

    def test_explicit_read_only_game_links_and_internal_save_link(self):
        (self.source / 'ms0' / 'save-copy').symlink_to('save')
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        run = next(self.root.glob('yakumo-quest-translation-*'))
        manifest = json.loads((run / 'manifest.json').read_text())
        self.assertEqual(manifest['returncode'], 0)
        self.assertFalse(manifest['timed_out'])
        self.assertTrue(manifest['unlimited'])
        self.assertEqual(manifest['captures'], ['fixture.bmp'])
        self.assertEqual(manifest['visual_verdict'], 'NOT TESTED')
        self.assertEqual((run / 'translations' / 'selected.lang').read_bytes(), self.translation.read_bytes())
        self.assertEqual((run / 'data' / 'ms0' / 'save-copy').read_bytes(), b'protected save')
        self.assertFalse((run / 'data' / 'ms0' / 'save-copy').is_symlink())
        self.assertEqual((run / 'data' / 'disc.iso').resolve(), self.protected.resolve())

    def test_external_save_symlink_rejected(self):
        (self.source / 'ms0' / 'external').symlink_to(self.protected)
        self.assert_rejected_before_output('symlink outside ms0')

    def test_external_settings_symlink_rejected(self):
        (self.source / 'settings.ini').symlink_to(self.protected)
        self.assert_rejected_before_output('escapes the output directory')

    def test_save_directory_cycle_rejected(self):
        (self.source / 'ms0' / 'cycle').symlink_to('.')
        self.assert_rejected_before_output('directory symlink')

    def test_save_special_file_rejected(self):
        os.mkfifo(self.source / 'ms0' / 'fifo')
        self.assert_rejected_before_output('special file')

    def test_missing_font_rejected_before_output(self):
        self.font.unlink()
        self.assert_rejected_before_output('existing local file')

    def test_missing_binary_rejected_before_output(self):
        self.binary.unlink()
        self.assert_rejected_before_output('Missing input')

    def test_missing_save_root_rejected_before_output(self):
        (self.source / 'ms0' / 'save').unlink()
        (self.source / 'ms0').rmdir()
        self.assert_rejected_before_output('save source containing ms0')


if __name__ == '__main__':
    unittest.main()
