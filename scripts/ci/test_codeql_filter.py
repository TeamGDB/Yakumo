"""CodeQL SARIF filtering of vendored sources; no network or CodeQL needed."""

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import contextmanager, redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import codeql_filter  # noqa: E402


@contextmanager
def chdir(path):
    previous = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


def result(uri):
    return {'ruleId': 'x', 'locations': [{'physicalLocation': {'artifactLocation': {'uri': uri}}}]}


class CodeqlFilterTests(unittest.TestCase):
    def test_vendored_results_are_removed_and_others_kept(self):
        document = {'runs': [{'results': [
            result('third_party/asmjit/asmjit/ujit/ujit_base.h'),
            result('file:///home/runner/work/Yakumo/Yakumo/profiles/mhp3rd/third_party/luau/VM/src/lvm.cpp'),
            result('src/jit.cpp'),
            {'ruleId': 'no location'},
        ]}, {}]}
        self.assertEqual(codeql_filter.filter_sarif(document), 2)
        kept = document['runs'][0]['results']
        self.assertEqual([r.get('locations', [{}])[0].get('physicalLocation', {}).get('artifactLocation', {})
                          .get('uri') for r in kept], ['src/jit.cpp', None])
        self.assertEqual(document['runs'][1], {'results': []})

    def test_command_rewrites_every_sarif_file(self):
        with tempfile.TemporaryDirectory() as work, chdir(work):
            folder = 'sarif'
            Path(folder).mkdir()
            path = Path(folder) / 'cpp.sarif'
            path.write_text(json.dumps({'runs': [{'results': [result('third_party/asmjit/a.h'),
                                                                result('src/a.cpp')]}]}))
            (Path(folder) / 'notes.txt').write_text('left alone')
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(codeql_filter.main([folder]), 0)
            self.assertIn('1 vendored result(s) removed', output.getvalue())
            self.assertEqual(len(json.loads(path.read_text())['runs'][0]['results']), 1)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(codeql_filter.main([folder, 'src/']), 0)
            self.assertEqual(json.loads(path.read_text())['runs'][0]['results'], [])
        with redirect_stderr(io.StringIO()):
            self.assertEqual(codeql_filter.main([]), 2)

    def test_command_refuses_a_directory_outside_the_working_directory(self):
        with tempfile.TemporaryDirectory() as work, tempfile.TemporaryDirectory() as elsewhere, chdir(work):
            path = Path(elsewhere) / 'cpp.sarif'
            path.write_text(json.dumps({'runs': [{'results': [result('third_party/asmjit/a.h')]}]}))
            message = io.StringIO()
            with redirect_stderr(message):
                self.assertEqual(codeql_filter.main([elsewhere]), 2)
            self.assertIn('not inside the working directory', message.getvalue())
            self.assertEqual(len(json.loads(path.read_text())['runs'][0]['results']), 1)


if __name__ == '__main__':
    unittest.main()
