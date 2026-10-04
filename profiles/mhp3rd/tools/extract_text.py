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

    The shape is the one the game itself indexes (host/text/translation.cpp
    `table_count`): the table's first word is its own size in bytes, a multiple
    of 4 that is at least 8, and the word after the offsets is 0xFFFFFFFF. A
    table that does not have that shape is one the game could never read, so it
    is rejected here as well (`extract_blocks.py` and the run-time patch use the
    same rule, which is why a key here is a key the game can apply)."""
    if table < 0 or table + 8 > len(data):
        return None
    first = u32(data, table)
    if first < 8 or first % 4 != 0:
        return None
    count = first // 4 - 1
    if count < 2 or count > 8192:
        return None
    if table + first > len(data):
        return None
    if u32(data, table + count * 4) != 0xFFFFFFFF:
        return None
    strings = []
    for k in range(count):
        offset = u32(data, table + k * 4)
        if offset == 0:
            strings.append("")
            continue
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

    The layout is the one the debug tools read (`host/debug/game_state.cpp`) and
    the run-time patch uses (`host/text/translation.cpp`): a header whose
    `u32[1]` is 8, whose `u32[0]` is the number of table slots that follow, and
    whose words 2 .. 1 + u32[0] are the tables' offsets. A slot that is 0 or
    out of range is empty and skipped, not the end of the header: entry 16 has
    a 0xFFFFFFFF between its two groups of tables (slots 2..38 and 40..50 in a
    run of 49). A table the game could not read - one whose first word is not
    its own size ending in 0xFFFFFFFF (`parse_table`) - is not a table.

    Entry 16's u32[0] and u32[1] are header fields of their own; the first
    table slot is always index 2.
    """
    if len(data) < 12 or u32(data, 4) != 8:
        return None
    slots = u32(data, 0)
    if not 0 < slots <= 62:
        return None
    tables = []
    for index in range(2, 2 + slots):
        slot = index * 4
        if slot + 4 > len(data):
            break
        offset = u32(data, slot)
        if offset == 0 or offset >= len(data):
            continue
        table = parse_table(data, offset)
        if not table or len(table) < 2:
            continue
        tables.append((index, table))
    if not tables or sum(len(t) for _, t in tables) < 8:
        return None
    return (0, tables)


def quest_block(data):
    """The strings of a quest file (entries 4059-4073 and friends), as
    [(ref_offset, string_offset, text)], or None.

    A quest entry is not an offset-table block: it is an array of record
    offsets at its start (u32s that increase, ended by 0), and each record
    holds, near its end, a table of u32 offsets (absolute in the entry) to its
    own strings - the title, the objective, the result line, the description,
    the monsters and the client. The run of increasing offsets that point at
    printable strings is that table; the game reads each string at
    `base + offset`, so the run-time patch repoints those words at the arena
    (host/text/translation.cpp, the quest path) and prints any length.

    Returns the (position of the offset word, the offset it holds, the string)
    of every field of every record, or None when the entry has no records.
    """
    if len(data) < 16:
        return None
    records = []
    index = 0
    while index * 4 + 4 <= len(data) and index < 64:
        value = u32(data, index * 4)
        if value == 0 or (records and value <= records[-1]) or value >= len(data):
            break
        records.append(value)
        index += 1
    if len(records) < 2:
        return None

    fields = []
    for k, start in enumerate(records):
        end = records[k + 1] if k + 1 < len(records) else len(data)
        # The title is at a fixed 72 bytes into the record; its offset anchors
        # the record's string table (a run of increasing offsets that each
        # point at a printable string, at most the six fields the game reads).
        anchor = start + 72
        best = None  # (position, [offsets])
        position = start
        while position + 4 <= end:
            run = []
            at = position
            while at + 4 <= end and len(run) < 8:
                value = u32(data, at)
                if value == 0 or value >= len(data) or not 32 <= data[value] < 127:
                    break
                if run and value <= run[-1]:
                    break
                # The string table ends with a sentinel holding the table's own
                # position; it is not a string, and its low byte often reads as
                # printable, so stop before it instead of taking a seventh field.
                if run and value == position:
                    break
                run.append(value)
                at += 4
            if run and run[0] == anchor and (best is None or len(run) > len(best[1])):
                best = (position, run)
            position += 4
        if best is None or len(best[1]) < 3:
            continue
        position, run = best
        for n, string_offset in enumerate(run):
            text = read_cstr(data, string_offset)
            if text:
                fields.append((position + n * 4, string_offset, text))
    # A real quest file has several records of five or six fields; a handful
    # of fields is a binary entry that happened to look like one.
    if len(fields) < 10:
        return None
    return fields


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
        quest = quest_block(data) if not block else None
        runs = []
        if not block and not quest and not options.no_runs and (
                not options.skip_large or len(data) <= options.skip_large):
            runs = loose_runs(data)
        if not block and not quest and not runs:
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
            if quest:
                # A quest field is keyed by `ref:offset`: the word that holds
                # the offset (the run-time patch rewrites that word), and the
                # offset it holds. The table column is the word's position.
                out.write("\n== quest fields (%d)\n" % len(quest))
                for ref_offset, string_offset, text in quest:
                    out.write("%d\t%d\t%s\n" % (ref_offset, string_offset, text.replace("\n", "\\n")))
                    all_strings.write("%d\t%d\t%d\t%s\n" % (index, ref_offset, string_offset,
                                                            text.replace("\n", "\\n")))
                    strings_here += 1
                    total_strings += 1
            if runs:
                out.write("\n== loose text runs (%d)\n" % len(runs))
                for text in runs:
                    out.write("%s\n" % text.replace("\n", "\\n"))

        report.append((index, len(data), 1 if (block or quest) else 0, strings_here, len(runs)))
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
