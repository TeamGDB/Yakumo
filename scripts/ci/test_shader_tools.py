"""Compile every public shader variant and verify the embedded binary contract."""

import contextlib
import io
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'profiles/mhp3rd/tools'))
import embed_shaders

COMPILER = shutil.which('glslangValidator')


@unittest.skipUnless(COMPILER, 'glslangValidator is required; coverage CI installs it')
class ShaderToolTests(unittest.TestCase):
    def test_all_renderer_variants_compile_into_named_spirv_arrays(self):
        directory = ROOT / 'profiles/mhp3rd/host/gpu/shaders'
        variants = {'Ge': 'ge.vert', 'Raw': 'GE_RAW_VERTICES@ge.vert',
                    'Checked': 'GE_RAW_VERTICES,GE_CHECK_DECODE@ge.vert',
                    'Fragment': 'ge.frag', 'Plain': 'GE_NO_SPECIALIZATION@ge.frag',
                    'PlainAlphaFree': 'GE_NO_SPECIALIZATION,GE_NO_ALPHA_TEST@ge.frag',
                    'RotateVertex': 'rotate.vert', 'RotateFragment': 'rotate.frag'}
        entries = []
        for name, source in variants.items():
            defines, separator, path = source.partition('@')
            entries.append(f'{name}={defines + separator if separator else ""}'
                           f'{directory / (path if separator else source)}')
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'include/shaders.inc'
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(embed_shaders.main(['embed_shaders', COMPILER, str(output), *entries]), 0)
            arrays = re.findall(r'constexpr std::uint32_t (\w+)\[\] = \{(.*?)\};', output.read_text(), re.S)
            self.assertEqual({name for name, _ in arrays}, set(variants))
            for name, body in arrays:
                words = re.findall(r'0x([0-9a-f]{8})u', body)
                self.assertGreater(len(words), 5, name)
                self.assertEqual(int(words[0], 16), 0x07230203, name)
                self.assertGreater(int(words[3], 16), 0, name)
                # Walk instruction headers, not arbitrary operand words. Opcode
                # values are from Khronos SPIR-V's unified grammar: OpKill 252,
                # OpSpecConstant{True,False,,Composite,Op} 48 through 52.
                instructions = [int(word, 16) for word in words]
                cursor, opcodes = 5, set()
                while cursor < len(instructions):
                    count = instructions[cursor] >> 16
                    self.assertGreater(count, 0)
                    opcodes.add(instructions[cursor] & 0xffff)
                    cursor += count
                self.assertEqual(cursor, len(instructions))
                if name in ('Plain', 'PlainAlphaFree'):
                    self.assertTrue(opcodes.isdisjoint(range(48, 53)), name)
                    self.assertEqual(252 in opcodes, name == 'Plain', name)
                if name == 'Fragment':
                    self.assertIn(252, opcodes)
                    self.assertTrue(opcodes.intersection(range(48, 53)))

    def test_failed_compilation_preserves_diagnostics_and_removes_temporary_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'invalid.vert'
            source.write_text('#version 450\nthis is not valid shader syntax\n')
            with patch.object(tempfile, 'tempdir', temporary):
                with self.assertRaises(subprocess.CalledProcessError) as raised:
                    embed_shaders.compile_shader(COMPILER, str(source), [])
            self.assertIn(b'ERROR', raised.exception.stdout + raised.exception.stderr)
            self.assertEqual(list(Path(temporary).glob('*.spv')), [])

    def test_missing_arguments_fail_before_creating_an_output(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(embed_shaders.main(['embed_shaders']), 2)


if __name__ == '__main__':
    unittest.main()
