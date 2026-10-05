"""Synthetic archive and command-argument regressions; no game data required."""

import csv
import io
import os
import re
import shutil
import subprocess
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import add_overlay
import embed_shaders
import databin
import wrap_overlay
import extract_iso
import extract_blocks
import extract_dialogue
import extract_text
import text_report
from extraction_paths import extraction_path


def record(name, lba=21, size=4, directory=False):
    result = bytearray(33 + len(name) + (len(name) % 2 == 0))
    result[0] = len(result)
    struct.pack_into("<I", result, 2, lba)
    struct.pack_into("<I", result, 10, size)
    result[25] = 2 if directory else 0
    result[32] = len(name)
    result[33:33 + len(name)] = name
    return bytes(result)


def image_with_entry(entry):
    result = bytearray(22 * extract_iso.SECTOR)
    start = 16 * extract_iso.SECTOR
    result[start + 1:start + 6] = b"CD001"
    root = record(b"\0", lba=20, size=extract_iso.SECTOR, directory=True)
    result[start + 156:start + 190] = root
    result[20 * extract_iso.SECTOR:20 * extract_iso.SECTOR + len(entry)] = entry
    result[21 * extract_iso.SECTOR:21 * extract_iso.SECTOR + 4] = b"test"
    return result


class ArchiveSecurityTests(unittest.TestCase):
    def test_iso_extracts_valid_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            iso = root / "synthetic.iso"
            iso.write_bytes(image_with_entry(record(b"SAFE.BIN;1")))
            self.assertEqual(extract_iso.main(["extract_iso", str(iso), str(root / "output")]), 0)
            self.assertEqual((root / "output" / "SAFE.BIN").read_bytes(), b"test")

    def test_iso_rejects_archive_path_components(self):
        for name in (b"../escape", b"..", b"/absolute", b"dir\\escape", b"C:escape", b"bad\x00name", b"CON", b"LPT1.BIN"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                iso = Path(temporary) / "synthetic.iso"
                iso.write_bytes(image_with_entry(record(name)))
                with self.assertRaises(ValueError):
                    extract_iso.main(["extract_iso", str(iso), str(Path(temporary) / "output")])

    def test_iso_rejects_truncated_record(self):
        malformed = bytearray(record(b"SAFE"))
        malformed[32] = 100
        with self.assertRaises(ValueError):
            extract_iso.walk(io.BytesIO(image_with_entry(malformed)), record(b"\0", 20, 2048, True), "", lambda *args: None)

    def test_iso_rejects_directory_cycle(self):
        entry = record(b"LOOP", 20, 2048, True)
        with self.assertRaises(ValueError):
            extract_iso.walk(io.BytesIO(image_with_entry(entry)), record(b"\0", 20, 2048, True), "", lambda *args: None)

    def test_existing_symlinks_cannot_escape_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "output"
            output.mkdir()
            outside = root / "outside"
            outside.mkdir()
            try:
                (output / "LINK").symlink_to(outside, target_is_directory=True)
                (output / "FILE").symlink_to(outside / "file")
            except OSError as error:
                self.skipTest(f"symlinks unavailable: {error}")
            for components in (["LINK", "escape"], ["FILE"]):
                with self.subTest(components=components), self.assertRaises(ValueError):
                    extraction_path(output, components)
            self.assertFalse((outside / "file").exists())

    def test_databin_rejects_overlay_filename_traversal(self):
        class FakeArchive:
            def read(self, index):
                return b"test"
            def overlay(self, index):
                return {"name": "dir/../../escape.bin"}
        with tempfile.TemporaryDirectory() as temporary, self.assertRaises(ValueError):
            databin.write_entry(FakeArchive(), 0, temporary)

    def test_similar_directory_prefix_does_not_allow_symlink_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / 'output'
            neighbor = root / 'output-neighbor'
            output.mkdir()
            neighbor.mkdir()
            protected = neighbor / 'file'
            protected.write_bytes(b'keep')
            try:
                (output / 'FILE').symlink_to(protected)
            except OSError as error:
                self.skipTest(f'symlinks unavailable: {error}')
            with self.assertRaises(ValueError):
                extraction_path(output, ('FILE',))
            with self.assertRaises(ValueError):
                extraction_path(output, ())
            self.assertEqual(protected.read_bytes(), b'keep')

    def test_databin_valid_overlay_filename(self):
        class FakeArchive:
            def read(self, index):
                return b"test"
            def overlay(self, index):
                return {"name": "demo_task.bin"}
        with tempfile.TemporaryDirectory() as temporary:
            path, size = databin.write_entry(FakeArchive(), 7, temporary)
            self.assertEqual(path.name, "00007_demo_task.bin")
            self.assertEqual(path.read_bytes(), b"test")
            self.assertEqual(size, 4)

    def test_overlay_outputs_cannot_follow_symlinks_outside_corpus(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            header = bytearray(64)
            header[:4] = b"MWo3"
            struct.pack_into("<4I", header, 4, 0, 0x08800000, 0, 0)
            header[32:36] = b"demo"
            dump = root / "dump.bin"
            dump.write_bytes(header)
            corpus = root / "overlays"
            corpus.mkdir()
            outside = root / "outside"
            outside.mkdir()
            prefix = f"ovl08800000_demo_{add_overlay.fnv1a64(header):016X}"
            target = corpus / prefix
            try:
                target.symlink_to(outside, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"symlinks unavailable: {error}")
            with patch.object(add_overlay, "OVERLAY_DIR", str(corpus)), patch.object(add_overlay.subprocess, "run") as run:
                with self.assertRaises(SystemExit):
                    add_overlay.main(["add_overlay", "--no-build", str(build), str(dump), "0x08800000"])
                run.assert_not_called()
                target.unlink()
                target.mkdir()
                (target / "meta.txt").symlink_to(outside / "meta.txt")
                with self.assertRaises(SystemExit):
                    add_overlay.main(["add_overlay", "--no-build", str(build), str(dump), "0x08800000"])
                run.assert_not_called()
            self.assertFalse((outside / "meta.txt").exists())

    def test_overlay_command_paths_cannot_be_interpreted_as_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "-build"
            build.mkdir()
            (build / ("psp_recomp.exe" if sys.platform == "win32" else "psp_recomp")).touch()
            dump = root / "-dump.bin"
            header = bytearray(64)
            header[:4] = b"MWo3"
            struct.pack_into("<4I", header, 4, 0, 0x08800000, 0, 0)
            header[32:36] = b"demo"
            dump.write_bytes(header)
            with patch.object(add_overlay, "OVERLAY_DIR", str(root / "overlays")), patch.object(add_overlay.subprocess, "run") as run:
                add_overlay.main(["add_overlay", "--no-build", str(build), str(dump), "0x08800000"])
                commands = [call.args[0] for call in run.call_args_list]
                self.assertTrue(Path(commands[0][2]).is_absolute())
                self.assertEqual(Path(commands[1][0]), build / ("psp_recomp.exe" if sys.platform == "win32" else "psp_recomp"))
                self.assertTrue(Path(commands[1][0]).is_absolute())
                self.assertEqual(run.call_args_list[1].kwargs["cwd"], str(build))
                self.assertEqual(commands[1][4], "0x08800000")


class ArchiveBehaviorTests(unittest.TestCase):
    @staticmethod
    def mask(block, words):
        high, low = (block >> 16) or 0x2345, (block & 0xFFFF) or 0x7F8D
        result = bytearray()
        for _ in range(words):
            high = high * 0x2345 % 0xFFD9
            low = low * 0x7F8D % 0xFFF1
            result.extend(struct.pack("<HH", low, high))
        return bytes(result)

    @classmethod
    def encrypt(cls, plain, block):
        padded = plain + b"\0" * (-len(plain) % 4)
        xor = bytes(a ^ b for a, b in zip(padded, cls.mask(block, len(padded) // 4)))
        return xor.translate(databin.ENCODE_TABLE)[:len(plain)]

    @classmethod
    def archive(cls, path):
        overlay = bytearray(80)
        overlay[:4] = b"MWo3"
        struct.pack_into("<7I", overlay, 4, 1, 0x08804000, 12, 4, 8, 0x08804018, 0x08804000)
        overlay[32:42] = b"public.ovl"
        directory = bytearray(2048)
        struct.pack_into("<7I", directory, 0, 1, 2, 3, 4, 0, 80, 1)
        # Third entry is block-aligned; first two have explicit logical lengths.
        struct.pack_into("<I", directory, 28, 8)
        path.write_bytes(cls.encrypt(directory, 0) + cls.encrypt(overlay + bytes(2048 - len(overlay)), 1) +
                         b"PSMFtest" + bytes(2040) + cls.encrypt(bytes(2048), 3))
        return bytes(overlay)

    def test_keystream_long_cycle_and_partial_words(self):
        for block in (0, 1, 0x12345678):
            self.assertEqual(databin.keystream(block, 70000), self.mask(block, 70000))
        self.assertEqual(databin.keystream(0, 0), b"")
        for size in (0, 1, 3, 4, 17, 2048):
            plain = bytes((i * 17) & 255 for i in range(size))
            self.assertEqual(databin.decrypt(self.encrypt(plain, 7), 7), plain)

    def test_real_archive_directory_and_commands(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "public.bin"
            overlay = self.archive(path)
            archive = databin.Archive(path)
            try:
                self.assertEqual(len(archive), 3)
                self.assertEqual(archive.read(0), overlay)
                self.assertEqual(archive.read(1), b"PSMFtest")
                self.assertEqual(archive.entry_size(2), 2048)
                self.assertEqual(archive.overlay(0)["name"], "public.ovl")
                self.assertIsNone(archive.overlay(1))
                with patch("sys.stdout", new=io.StringIO()) as output:
                    self.assertEqual(databin.main(["databin", str(path), "list"]), 0)
                    self.assertIn("overlay public.ovl", output.getvalue())
                with patch("sys.stdout", new=io.StringIO()) as output:
                    databin.main(["databin", str(path), "list", "--overlays", "--csv"])
                    self.assertIn("0,1,80,public.ovl,0x8804000", output.getvalue())
                    self.assertNotIn("PSMF", output.getvalue())
                for args in (("extract", str(root / "entries")), ("extract", str(root / "selected"), "0x1"),
                             ("extract-overlays", str(root / "overlays"))):
                    with patch("sys.stdout", new=io.StringIO()):
                        databin.main(["databin", str(path), *args])
                self.assertEqual((root / "entries" / "00000_public.ovl").read_bytes(), overlay)
                self.assertEqual((root / "selected" / "00001.bin").read_bytes(), b"PSMFtest")
                self.assertEqual((root / "overlays" / "overlay_08804000_public.bin").read_bytes(), overlay)
                reference = root / "reference.bin"
                reference.write_bytes(overlay)
                with patch("sys.stdout", new=io.StringIO()):
                    self.assertEqual(databin.main(["databin", str(path), "verify", "0", str(reference)]), 0)
                    reference.write_bytes(b"bad" + overlay[3:])
                    self.assertEqual(databin.main(["databin", str(path), "verify", "0", str(reference)]), 1)
                class BinaryOutput(io.StringIO):
                    buffer = io.BytesIO()
                with patch("sys.stdout", new=BinaryOutput()) as output:
                    databin.main(["databin", str(path), "cat", "1"])
                    self.assertEqual(output.buffer.getvalue(), b"PSMFtest")
            finally:
                archive.stream.close()

    def test_databin_is_located_inside_synthetic_iso(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "data.bin"
            expected = self.archive(archive_path)
            image = bytearray(27 * 2048)
            pvd = 16 * 2048
            image[pvd + 1:pvd + 6] = b"CD001"
            image[pvd + 156:pvd + 190] = record(b"\0", 20, 2048, True)
            for lba, entries in ((20, [record(b"\0", 20, 2048, True), record(b"PSP_GAME", 21, 2048, True)]),
                                 (21, [record(b"USRDIR", 22, 2048, True)]),
                                 (22, [record(b"UNRELATED.BIN;1", 26, 0), record(b"DATA.BIN;1", 23, 8192)])):
                directory = b"".join(entries)
                image[lba * 2048:lba * 2048 + len(directory)] = directory
            image[23 * 2048:] = archive_path.read_bytes()
            iso = root / "public.iso"
            iso.write_bytes(image)
            archive = databin.Archive(iso)
            try:
                self.assertEqual(archive.base, 23 * 2048)
                self.assertEqual(archive.size, 8192)
                self.assertEqual(archive.read(0), expected)
            finally:
                archive.stream.close()
            self.assertIsNone(databin.iso_find(io.BytesIO(image), "/NOT_PRESENT"))

    def test_directory_rejects_invalid_size_and_termination(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.bin"
            for directory in (bytes(2048), struct.pack("<I", 1) + bytes(2044)):
                path.write_bytes(self.encrypt(directory, 0))
                with self.assertRaises(ValueError):
                    databin.Archive(path)

    def test_archive_stream_closes_on_parse_and_command_failures(self):
        opened = []
        real_open = open
        def tracking_open(*args, **kwargs):
            stream = real_open(*args, **kwargs)
            opened.append(stream)
            return stream
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "archive.bin"
            path.write_bytes(self.encrypt(bytes(2048), 0))
            with patch("builtins.open", side_effect=tracking_open):
                with self.assertRaises(ValueError):
                    databin.Archive(path)
            self.assertTrue(opened and all(stream.closed for stream in opened))
            self.archive(path)
            opened.clear()
            with patch("builtins.open", side_effect=tracking_open), patch("sys.stdout", new=io.StringIO()):
                self.assertEqual(databin.main(["databin", str(path), "list"]), 0)
            self.assertTrue(opened and all(stream.closed for stream in opened))
            opened.clear()
            with patch("builtins.open", side_effect=tracking_open):
                with self.assertRaises(IndexError):
                    databin.main(["databin", str(path), "cat", "99"])
            self.assertTrue(opened and all(stream.closed for stream in opened))

    def test_wrap_overlay_produces_exact_elf_load_and_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw = root / "code.bin"
            elf = root / "code.elf"
            raw.write_bytes(struct.pack("<II", 0x03E00008, 0))
            with patch("sys.stdout", new=io.StringIO()):
                self.assertEqual(wrap_overlay.main(["wrap", str(raw), "0x08804000", str(elf)]), 0)
            data = elf.read_bytes()
            header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
            self.assertEqual(header[0][:7], b"\x7fELF\x01\x01\x01")
            self.assertEqual(header[4], 0x08804000)
            segment = struct.unpack_from("<8I", data, header[5])
            self.assertEqual(segment, (1, 84, 0x08804000, 0x08804000, 8, 8, 5, 16))
            self.assertEqual(data[84:92], raw.read_bytes())
            text = struct.unpack_from("<10I", data, header[6] + 40)
            self.assertEqual(text[2:6], (6, 0x08804000, 84, 8))
            with patch("sys.stderr", new=io.StringIO()):
                self.assertEqual(wrap_overlay.main(["wrap"]), 2)
                self.assertEqual(wrap_overlay.main(["wrap", str(raw), "0x08804001", str(elf)]), 1)


class TextToolContracts(unittest.TestCase):
    @staticmethod
    def block():
        data = bytearray(512)
        struct.pack_into("<4I", data, 0, 2, 8, 64, 0)
        strings = ["", "Menu", "Goal", "Yes", "No", "Item", "Craft", "Rest"]
        offset = 36
        for index, text in enumerate(strings):
            struct.pack_into("<I", data, 64 + index * 4, offset)
            encoded = text.encode() + b"\0"
            data[64 + offset:64 + offset + len(encoded)] = encoded
            offset += len(encoded)
        struct.pack_into("<I", data, 96, 0xffffffff)
        return bytes(data)

    @staticmethod
    def dialogue():
        data = bytearray(64)
        struct.pack_into("<3I", data, 0, 17, 16, 0xffffffff)
        struct.pack_into("<3I", data, 16, 1, 16, 0xffffffff)
        data[32:42] = b"Synthetic\0"
        return bytes(data)

    @staticmethod
    def quest():
        data = bytearray(1024)
        struct.pack_into("<3I", data, 0, 64, 512, 0)
        for base in (64, 512):
            for index in range(6):
                offset = base + 72 + index * 40
                struct.pack_into("<I", data, base + index * 4, offset)
                text = ("Field%d" % index).encode() + b"\0"
                data[offset:offset + len(text)] = text
            struct.pack_into("<I", data, base + 24, base)
        return bytes(data)

    @classmethod
    def archive(cls, path):
        entries = [cls.block(), cls.quest(), cls.dialogue(), b"Loose synthetic text\0"]
        directory = bytearray(2048)
        struct.pack_into("<5I", directory, 0, 1, 2, 3, 4, 5)
        for index, data in enumerate(entries):
            struct.pack_into("<2I", directory, 20 + index * 8, index, len(data))
        path.write_bytes(ArchiveBehaviorTests.encrypt(directory, 0) + b"".join(
            ArchiveBehaviorTests.encrypt(data + bytes(2048 - len(data)), index + 1)
            for index, data in enumerate(entries)))

    def test_single_quest_record(self):
        data = bytearray(self.quest()[:512])
        struct.pack_into("<I", data, 4, 0)
        fields = extract_text.quest_block(data)
        self.assertIsNotNone(fields)
        self.assertEqual(len(fields), 6)
        self.assertEqual(fields[0], (64, 136, "Field0"))
        struct.pack_into("<I", data, 64 + 20, 0)
        self.assertIsNone(extract_text.quest_block(data))

    def test_parsers_and_truncation(self):
        data = self.block()
        self.assertEqual(extract_blocks.blocks_of(data)[0][1][1], "Menu")
        self.assertEqual(extract_text.find_block(data)[1][0][0], 2)
        self.assertEqual(list(extract_dialogue.dialogue_of(self.dialogue())), [(17, 0, 1, "Synthetic")])
        fields = extract_text.quest_block(self.quest())
        self.assertEqual(len(fields), 12)
        self.assertEqual(fields[0], (64, 136, "Field0"))
        for size in range(24):
            truncated = bytes(size)
            self.assertEqual(extract_blocks.blocks_of(truncated), [])
            self.assertIsNone(extract_text.find_block(truncated))
            self.assertIsNone(extract_text.quest_block(truncated))
            self.assertEqual(list(extract_dialogue.dialogue_of(truncated)), [])
        self.assertIsNone(extract_blocks.table_at(data, 0, 0))
        self.assertIsNone(extract_text.parse_table(data, -1))
        self.assertIsNone(extract_blocks.read_cstr(data, len(data)))
        self.assertIsNone(extract_dialogue.read_cstr(b"no terminator", 0))
        self.assertEqual(extract_text.loose_runs(b"hello\0trailing"), ["hello", "trailing"])

    def test_commands_use_real_encrypted_synthetic_archive(self):
        with tempfile.TemporaryDirectory() as temporary, patch("sys.stdout", new=io.StringIO()):
            root = Path(temporary)
            image = root / "synthetic.bin"
            self.archive(image)
            extract_blocks.main([str(image), str(root / "blocks.tsv"), "--entries", "0,99"])
            self.assertIn("Menu", (root / "blocks.tsv").read_text())
            extract_dialogue.main([str(image), str(root / "dialogue.tsv"), "--entries", "2"])
            self.assertIn("Synthetic", (root / "dialogue.tsv").read_text())
            extract_text.main([str(image), str(root / "dump")])
            self.assertIn("quest fields", (root / "dump/entry_0001.txt").read_text())
            self.assertIn("loose text runs", (root / "dump/entry_0003.txt").read_text())
            extract_text.main([str(image), str(root / "small"), "--skip-large", "30", "--no-runs"])
            self.assertEqual((root / "small/all_strings.tsv").read_text(), "")
            text_report.main([str(root / "dump")])
            self.assertTrue((root / "dump/strings.csv").is_file())
            with self.assertRaises(IndexError):
                extract_dialogue.main([str(image), str(root / "bad.tsv"), "--entries", "99"])

    def test_report_preserves_text_and_protects_spreadsheet_cells(self):
        self.assertEqual(text_report.kind("ASCII"), "en")
        self.assertEqual(text_report.kind("\u65e5\u672c"), "jp")
        self.assertEqual(text_report.kind("a\uff01"), "misto")
        self.assertEqual(text_report.kind("123"), "sym")
        self.assertEqual(text_report.clean("~C12Hello"), "Hello")
        for value in ("=1+1", "+1", "-1", "@SUM(A1)", "\tplain", " =1"):
            self.assertTrue(text_report.spreadsheet_row({"text": value})["text"].startswith("'"))
        with tempfile.TemporaryDirectory() as temporary, patch("sys.stdout", new=io.StringIO()):
            root = Path(temporary)
            (root / "all_strings.tsv").write_text("entry\ttable\tindex\ttext\n0\t2\t1\t=1+1\n"
                "0\t2\t2\t~C12Hello\\nWorld\n0\t2\t3\t\u65e5\u672c\ninvalid\n", encoding="utf-8")
            text_report.main([str(root)])
            with (root / "strings.csv").open() as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 3)
            self.assertEqual(rows[0]["text"], "'=1+1")
            self.assertEqual(rows[1]["has_format"], "1")
            self.assertIn("~C12", (root / "codes.txt").read_text())
            with self.assertRaises(SystemExit):
                text_report.main([str(root / "missing")])

    @unittest.skipIf(os.name == "nt", "symlink creation requires privileges on Windows")
    def test_text_outputs_cannot_follow_symlinks_outside_root(self):
        with tempfile.TemporaryDirectory() as temporary, patch("sys.stdout", new=io.StringIO()):
            root = Path(temporary)
            image = root / "synthetic.bin"
            self.archive(image)
            outside = root / "outside.txt"
            outside.write_text("keep")
            dump = root / "dump"
            dump.mkdir()
            (dump / "all_strings.tsv").symlink_to(outside)
            with self.assertRaises(ValueError):
                extract_text.main([str(image), str(dump)])
            self.assertEqual(outside.read_text(), "keep")


class BuildToolContracts(unittest.TestCase):
    def test_overlay_header_identity_and_errors(self):
        self.assertEqual(add_overlay.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(add_overlay.fnv1a64(b"hello"), 0xA430D84680AABD0B)
        header = bytearray(64)
        header[:4] = b"MWo3"
        struct.pack_into("<4I", header, 4, 7, 0x08800000, 8, 16)
        header[32:45] = b"demo-task.bin"
        self.assertEqual(add_overlay.parse_header(header, 0x08800000), ("demo_task", 88, 8))
        with self.assertRaises(SystemExit):
            add_overlay.parse_header(b"invalid", 0x08800000)
        with self.assertRaises(SystemExit):
            add_overlay.parse_header(header, 0x08804000)
        header[32:] = bytes(32)
        with self.assertRaises(SystemExit):
            add_overlay.parse_header(header, 0x08800000)

    def test_overlay_generates_public_corpus_without_building_library(self):
        recompiler = os.environ.get("PSPRECOMP_TEST_RECOMP")
        if not recompiler or not Path(recompiler).is_file():
            self.skipTest("set PSPRECOMP_TEST_RECOMP to run the real public overlay workflow")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = bytearray(64)
            header[:4] = b"MWo3"
            struct.pack_into("<4I", header, 4, 7, 0x08800000, 8, 4)
            header[32:40] = b"demo.bin"
            dump = root / "synthetic.bin"
            dump.write_bytes(header + struct.pack("<III", 0x03E00008, 0, 123))
            corpus = root / "corpus"
            with patch.object(add_overlay, "OVERLAY_DIR", str(corpus)), patch("sys.stdout", new=io.StringIO()) as output:
                self.assertEqual(add_overlay.main(["add_overlay", "--no-build", str(Path(recompiler).parent),
                                                   str(dump), "0x08800000"]), 0)
            target = next(corpus.iterdir())
            metadata = (target / "meta.txt").read_text()
            self.assertIn("name=demo\nsize=76\ncode_size=8\n", metadata)
            self.assertIn("source=synthetic.bin\n", metadata)
            self.assertIn(f"target: overlay_{target.name}", output.getvalue())
            self.assertTrue((target / f"{target.name}_registry.cpp").is_file())
            elf = (target / "overlay.elf").read_bytes()
            self.assertEqual(elf[:4], b"\x7fELF")
            self.assertEqual(elf[84:160], dump.read_bytes())

    def test_shader_usage_and_real_compilation_variants(self):
        with patch("sys.stderr", new=io.StringIO()):
            self.assertEqual(embed_shaders.main(["embed_shaders"]), 2)
        compiler = shutil.which("glslangValidator")
        if not compiler:
            self.skipTest("glslangValidator unavailable for actual SPIR-V compilation")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "public.vert"
            source.write_text("#version 450\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n")
            binary = embed_shaders.compile_shader(compiler, str(source), [])
            self.assertEqual(binary[:4], bytes.fromhex("03022307"))
            self.assertEqual(len(binary) % 4, 0)
            destination = root / "nested" / "shaders.inc"
            with patch("sys.stdout", new=io.StringIO()):
                self.assertEqual(embed_shaders.main(["embed_shaders", compiler, str(destination),
                                                     f"kPlain={source}", f"kDefined=A,,B@{source}"]), 0)
            emitted = destination.read_text()
            self.assertIn("constexpr std::uint32_t kPlain[]", emitted)
            self.assertIn("constexpr std::uint32_t kDefined[]", emitted)
            words = re.findall(r"0x([0-9a-f]{8})u", emitted)
            self.assertEqual(int(words[0], 16), 0x07230203)
            self.assertEqual(len(words), len(binary) // 2)
            source.write_text("this is not valid GLSL")
            with patch.object(embed_shaders.tempfile, "tempdir", str(root)):
                before = set(root.glob("*.spv"))
                with self.assertRaises(subprocess.CalledProcessError):
                    embed_shaders.compile_shader(compiler, str(source), [])
                self.assertEqual(set(root.glob("*.spv")), before)

if __name__ == "__main__":
    unittest.main()
