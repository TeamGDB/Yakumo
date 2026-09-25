#!/usr/bin/env python3
"""Extract the text of Monster Hunter Portable 3rd HD (NPJB-40001) from a disc
image, without changing anything.

Every asset is an entry of DATA.BIN (docs/DATA_BIN.md). The game's text lives in
"string blocks": a header of 32-bit offsets to tables; each table is a list of
32-bit offsets (relative to the table) to NUL-terminated UTF-8 strings, ending
with 0xFFFFFFFF. Entry 16 (string_tables/main.bin) is the big one, loaded at
0x08A40640 at start; several other entries (menu data, one per overlay's task,
the gallery, the kitchen, ...) carry the same shape.

The tool also reports the entries a loose scan found text in (NUL-delimited
runs) so a format that is not an offset table is not missed.

Usage:
    python3 extract_text.py IMAGE.iso OUTDIR

Output, in OUTDIR:
    all_strings.tsv     table<TAB>entry<TAB>string, for the main block and every
                        offset-table block found, with the DATA.BIN entry id
    entry_<n>.txt       the tables of entry n, human readable
    report.txt          what every entry holds and the totals
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import databin


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_cstr(data, offset):
    if offset < 0 or offset >= len(data):
        return None
    end = data.find(b"\x00", offset, min(len(data), offset + 8192))
    if end < 0:
        return None
    return data[offset:end].decode("utf-8", "replace")


def parse_table(data, table):
    """A list of u32 offsets relative to `table`, ended by 0xFFFFFFFF; each
    points at a NUL-terminated string. Returns [str] or None.

    Offsets are relative to the table start and the table may be unaligned
    (entry 16 has its first table at offset 49), so nothing here requires a
    4-byte alignment."""
    if table < 0 or table + 4 > len(data):
        return None
    count = None
    max_words = min(20000, (len(data) - table) // 4)
    for k in range(1, max_words):
        value = u32(data, table + k * 4)
        if value == 0xFFFFFFFF:
            count = k
            break
        if value > len(data) and k < 2:
            return None
    if count is None or count < 2:
        return None
    strings = []
    previous = -1
    for k in range(count):
        offset = u32(data, table + k * 4)
        if offset == 0:
            strings.append("")
            continue
        # The game's tables list strings in order, so the offsets grow; a
        # random word that happens to end in 0xFFFFFFFF does not.
        if offset < previous:
            return None
        previous = offset
        text = read_cstr(data, table + offset)
        if text is None:
            return None
        strings.append(text)
    # Reject a table whose entries are mostly binary noise: at least three
    # quarters must look like text.
    textual = sum(1 for s in strings if not s or sum(c.isprintable() or c in "\n\t" for c in s) * 10 >= len(s) * 9)
    if textual * 4 < len(strings) * 3:
        return None
    return strings


def find_block(data):
    """The string block of an entry, as (header offset, [tables]) or None.

    The layout is the one the debug tools read (`host/debug/game_state.cpp`):
    a header of 32-bit offsets, each the position of a table relative to the
    block start; table N is at `u32[N]`. Entry 16's u32[0] and u32[1] are
    header fields of their own (49 and 8), not table offsets, so a header
    starts at index 0 but the first two entries are skipped when they do not
    parse as tables.

    A block is recognised when it yields several tables with text; the header
    is assumed to start at the entry's first bytes.
    """
    tables = []
    first_valid = None
    for index in range(0, 512):
        slot = index * 4
        if slot + 4 > len(data):
            break
        offset = u32(data, slot)
        # The end of the header run: an offset of 0 or a value past the file.
        if offset == 0 or offset >= len(data):
            if tables:
                break
            continue
        table = parse_table(data, offset)
        if not table or len(table) < 2:
            # A header field before the tables (entry 16's u32[0], u32[1]);
            # once tables have started a break ends the run.
            if tables:
                break
            continue
        if first_valid is None:
            first_valid = index
        tables.append((index, table))
    if first_valid is None or sum(len(t) for _, t in tables) < 8:
        return None
    return (0, tables)


def loose_runs(data, min_length=4):
    runs = []
    current = bytearray()
    for byte in data:
        if byte == 0:
            if len(current) >= min_length:
                text = current.decode("utf-8", "replace")
                if sum(c.isprintable() or c in "\n\t" for c in text) * 10 >= len(text) * 9:
                    runs.append(text)
            current = bytearray()
        elif byte in (9, 10, 13) or 32 <= byte < 127 or byte >= 0x80:
            current.append(byte)
        else:
            if len(current) >= min_length:
                text = current.decode("utf-8", "replace")
                if sum(c.isprintable() or c in "\n\t" for c in text) * 10 >= len(text) * 9:
                    runs.append(text)
            current = bytearray()
    if len(current) >= min_length:
        text = current.decode("utf-8", "replace")
        if sum(c.isprintable() or c in "\n\t" for c in text) * 10 >= len(text) * 9:
            runs.append(text)
    return runs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image")
    parser.add_argument("outdir")
    parser.add_argument("--skip-large", type=int, default=0,
                        help="skip entries larger than this many bytes (default: all)")
    parser.add_argument("--no-runs", action="store_true",
                        help="only extract offset-table blocks, not loose text runs "
                             "(much faster: the run scan walks every byte)")
    options = parser.parse_args(argv)

    archive = databin.Archive(options.image)
    entries = len(archive.blocks) - 1
    os.makedirs(options.outdir, exist_ok=True)

    all_strings = open(os.path.join(options.outdir, "all_strings.tsv"), "w",
                       encoding="utf-8", newline="\n")
    report = []
    total_strings = 0

    for index in range(entries):
        try:
            data = archive.read(index)
        except Exception:
            continue
        if not data or data[:4] in (b"MWo3", b"PSMF", b"~SCE"):
            continue
        if options.skip_large and len(data) > options.skip_large:
            continue

        # Loose runs are only collected for entries the block parser did not
        # already cover, and never for huge binary entries: they cost time and
        # a model would swamp the output.
        block = find_block(data)
        runs = []
        if not block and not options.no_runs and (not options.skip_large or len(data) <= options.skip_large):
            runs = loose_runs(data)
        if not block and not runs:
            continue

        strings_here = 0
        with open(os.path.join(options.outdir, "entry_%04d.txt" % index), "w",
                  encoding="utf-8", newline="\n") as out:
            out.write("### entry %d, size %d bytes\n" % (index, len(data)))
            if block:
                base, tables = block
                out.write("### string block at %#x: %d table(s)\n" % (base, len(tables)))
                for table_index, table in tables:
                    out.write("\n== table %d (%d strings)\n" % (table_index, len(table)))
                    for k, text in enumerate(table):
                        out.write("%d\t%s\n" % (k, text.replace("\n", "\\n")))
                        all_strings.write("%d\t%d\t%d\t%s\n" % (index, table_index, k, text.replace("\n", "\\n")))
                        strings_here += 1
                        total_strings += 1
            if runs:
                out.write("\n== loose text runs (%d)\n" % len(runs))
                for text in runs:
                    out.write("%s\n" % text.replace("\n", "\\n"))

        report.append((index, len(data), 1 if block else 0, strings_here, len(runs)))
        if len(report) % 200 == 0:
            print("...", index, "entries with text:", len(report), flush=True)

    all_strings.close()
    with open(os.path.join(options.outdir, "report.txt"), "w", encoding="utf-8", newline="\n") as out:
        out.write("entries with a string block or loose text: %d of %d\n" % (len(report), entries))
        out.write("table strings: %d\n" % total_strings)
        out.write("\nentry\tsize\tblocks\ttable_strings\tloose_runs\n")
        for index, size, blocks, strings, runs in sorted(report, key=lambda r: -r[3]):
            out.write("%d\t%d\t%d\t%d\t%d\n" % (index, size, blocks, strings, runs))

    print("entries with text:", len(report), "of", entries)
    print("table strings:", total_strings)
    print("report: %s" % os.path.join(options.outdir, "report.txt"))


if __name__ == "__main__":
    main(sys.argv[1:])
