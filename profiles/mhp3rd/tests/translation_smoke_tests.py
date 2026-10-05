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

    def test_invalid_deadline_rejected_before_preparing_data(self):
        result = subprocess.run([sys.executable, str(SCRIPT), '--binary', 'missing',
            '--game-dir', 'missing', '--data-dir', 'missing', '--overlays', 'missing',
            '--translation', 'missing', '--language', 'test', '--commit', 'fixture',
            '--seconds', '601'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn('between 1 and 600', result.stderr)


if __name__ == '__main__':
    unittest.main()
