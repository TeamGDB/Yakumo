#!/usr/bin/env python3
"""Extract the text blocks of MHP3rd's DATA.BIN exactly as the game indexes them.

Like extract_text.py, but it trusts the layout the game uses instead of
inferring it: a block's header is `u32[1] == 8`, and the tables are the words
`u32[2]`, `u32[3]`, ...; table `k` is at `u32[k]` and is the table the debug
menu's `table k` reads (docs/DEBUG_MENU.md). The first word whose value does not
point at a table ends the run.

This is what the run-time patch (host/text/translation.cpp) needs, so the keys
in a .lang file name the same tables the game does.

    python3 extract_blocks.py IMAGE.iso OUT.tsv
        Writes `entry  table  index  text` for every table of every block.

    python3 extract_blocks.py IMAGE.iso OUT.tsv --entries 16,2835,2836,2837,2838,2839,2840,2841
        Limits it to the entries given (the text blocks the game has).
"""
import argparse
from contextlib import closing
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import databin
from extraction_paths import extraction_path

# The archive entries that hold text blocks (docs/DATA_BIN.md): the shared one
# and the quest/menu ones.
TEXT_ENTRIES = [16, 2835, 2836, 2837, 2838, 2839, 2840, 2841]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_cstr(data, offset, limit=8192):
    if offset < 0 or offset >= len(data):
        return None
    end = data.find(b"\x00", offset, min(len(data), offset + limit))
    if end < 0:
        return None
    return data[offset:end].decode("utf-8", "replace")


def table_at(data, base, offset):
    """The strings of the table at `base + offset`, or None when it is not one."""
    if offset < 8 or base + offset + 8 > len(data):
        return None
    table = base + offset
    first = u32(data, table)
    if first < 8 or first % 4 != 0:
        return None
    count = first // 4 - 1
    if count > 20000 or table + first > len(data):
        return None
    if u32(data, table + count * 4) != 0xFFFFFFFF:
        return None
    strings = []
    for k in range(count):
        relative = u32(data, table + k * 4)
        if relative == 0:
            strings.append("")
            continue
        text = read_cstr(data, table + relative)
        if text is None:
            return None
        strings.append(text)
    return strings


def blocks_of(data):
    """(index, [strings]) for every table the game would find, or an empty list."""
    if len(data) < 12 or u32(data, 4) != 8:
        return []
    slots = u32(data, 0)
    if not 0 < slots <= 62:
        return []
    tables = []
    for word in range(2, 2 + slots):
        if (word + 1) * 4 > len(data):
            break
        strings = table_at(data, 0, u32(data, word * 4))
        # An empty slot is skipped, not the end of the header: entry 16 has a
        # 0xFFFFFFFF between its two groups of tables (slots 2..38 and 40..50).
        if strings is None:
            continue
        tables.append((word, strings))
    return tables


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=argparse.FileType("rb"))
    parser.add_argument("out")
    parser.add_argument("--entries", default=",".join(str(e) for e in TEXT_ENTRIES),
                        help="comma-separated DATA.BIN entry ids")
    options = parser.parse_args(argv)
    wanted = {int(x) for x in options.entries.split(",") if x}

    archive = databin.Archive(options.image)
    with closing(archive.stream):
        total = 0
        with open(extraction_path(os.path.dirname(options.out) or ".", (os.path.basename(options.out),)), "w", encoding="utf-8", newline="\n") as out:
            out.write("entry\ttable\tindex\ttext\n")
            for entry in sorted(wanted):
                try:
                    data = archive.read(entry)
                except Exception:
                    continue
                for table_index, strings in blocks_of(data):
                    for index, text in enumerate(strings):
                        if not text:
                            continue
                        out.write("%d\t%d\t%d\t%s\n" % (entry, table_index, index, text.replace("\n", "\\n")))
                        total += 1
                print("entry %d: %s" % (entry, [w for w, _ in blocks_of(data)]), flush=True)
        print("wrote %s, %d strings" % (options.out, total))

if __name__ == "__main__":
    main(sys.argv[1:])
