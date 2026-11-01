"""Public SVG-to-runtime geometry contracts; no game data required."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET

TOOL = Path(__file__).resolve().parents[1] / "tools/touch_icons.py"
spec = importlib.util.spec_from_file_location("touch_icons", TOOL)
icons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icons)

class IconTests(unittest.TestCase):
    def test_assets_and_checked_in_geometry(self):
        expected = icons.generate()
        self.assertEqual(icons.OUTPUT.read_text(encoding="utf-8"), expected)
        self.assertIn("Confirm,", expected)
        self.assertIn("0x79cc8au", expected)
        self.assertIn("0xeb6b67u", expected)
        subprocess.run([sys.executable, str(TOOL), "--check"], check=True, timeout=20)

    def test_path_lines_and_curves(self):
        points, closed = icons.flatten("M1 2 H3 V4 L5 6 Q7 8 9 10 C11 12 13 14 15 16 Z")[0]
        self.assertTrue(closed)
        self.assertEqual(points[0], (1,2))
        self.assertEqual(points[-1], (15,16))
        self.assertEqual(len(icons.flatten("M1 2 L3 4 M5 6 L7 8")), 2)
        self.assertEqual(icons.flatten("M1 2 3 4")[0][0][-1], (3,4))
        self.assertEqual(len(icons.flatten("M0 0 A1 1 0 0 1 4 0")[0][0]), 33)
        self.assertEqual(len(icons.flatten("M0 0 A2 2 0 0 0 4 0")[0][0]), 33)

    def test_invalid_paths(self):
        for path in ["bad", "m0 0 l1 1", "M0", "M0 0 L1e999 2", "M0 0 A0 1 0 0 1 4 4", "M0 0 A1 1 45 0 1 4 4", "M0 0 A1 1 0 2 0 4 4", "M1 1 A1 1 0 0 1 1 1"]:
            with self.subTest(path=path), self.assertRaises(ValueError):
                icons.flatten(path)

    def test_transforms_and_rejection(self):
        root = ET.fromstring('<svg><g transform="translate(5 7) scale(2)"><path d="M0 0 L2 0 L0 2Z"/></g></svg>')
        self.assertEqual(icons.geometry(root)[0][0], [(5,7),(9,7),(5,11)])
        self.assertTrue(icons.geometry(ET.fromstring('<svg><g transform="translate(2) scale(2 2)"><circle cx="5" cy="5" r="2"/></g></svg>')))
        for element in ['<image/>','<path transform="rotate(4)" d="M0 0 L1 1"/>','<path transform="scale(-1)" d="M0 0 L1 1"/>','<path transform="scale(1 2)" d="M0 0 L1 1"/>','<path transform="scale(1 2 3)" d="M0 0 L1 1"/>','<path d="M0 0 L100 100"/>','<path d="M0 0"/>','<circle r="0"/>']:
            with self.subTest(element=element), self.assertRaises(ValueError):
                icons.geometry(ET.fromstring('<svg>'+element+'</svg>'))

if __name__ == '__main__':
    unittest.main()
