#!/usr/bin/env python3
"""Extract the NPC/quest dialogue of MHP3rd's DATA.BIN entry 4289.

The dialogue is stored in a different shape from the text blocks
(docs/DEBUG_MENU.md):

    u32[0] = 0
    then a list of pairs (id, offset) ending in 0xFFFFFFFF; `id` counts from 0
    each `offset` points, from the file start, at a block:
        the block is again a list of pairs (kind, offset) ending in 0xFFFFFFFF
        each `offset` points, from the block start, at a NUL-terminated string

So a string is addressed by (id, entry): entry 0 is the first (kind, offset) of
the block `id`. This tool flattens that to `id  entry  kind  text`, the same
shape the translation tools use (entry -> index).

    python3 extract_dialogue.py IMAGE.iso OUT.tsv [--entry 4289]
"""
import argparse
from contextlib import closing
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import databin
from extraction_paths import extraction_path


def u32(data, offset):
    if offset + 4 > len(data):
        return 0xFFFFFFFF
    return struct.unpack_from("<I", data, offset)[0]


def read_cstr(data, offset, limit=8192):
    if offset < 0 or offset >= len(data):
        return None
    end = data.find(b"\x00", offset, min(len(data), offset + limit))
    if end < 0:
        return None
    return data[offset:end].decode("utf-8", "replace")


def dialogue_of(data):
    """Yields (id, index, kind, text) for every dialogue string."""
    # Top level: (id, offset) pairs, ending in 0xFFFFFFFF.
    top = []
    for k in range(0, 4096):
        ident = u32(data, k * 8)
        offset = u32(data, k * 8 + 4)
        if ident == 0xFFFFFFFF:
            break
        if offset > len(data):
            break
        top.append((ident, offset))
    for ident, block_start in top:
        # Each block: (kind, offset) pairs, ending in 0xFFFFFFFF.
        for k in range(0, 4096):
            at = block_start + k * 8
            kind = u32(data, at)
            offset = u32(data, at + 4)
            if kind == 0xFFFFFFFF:
                break
            if offset > len(data) - block_start:
                break
            text = read_cstr(data, block_start + offset)
            if text is None or text == "":
                continue
            yield ident, k, kind, text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=argparse.FileType("rb"))
    parser.add_argument("out")
    parser.add_argument("--entries", default="4289,4290,4291",
                        help="the DATA.BIN entries that hold dialogue")
    options = parser.parse_args(argv)

    archive = databin.Archive(options.image)
    with closing(archive.stream):
        total = 0
        with open(extraction_path(os.path.dirname(options.out) or ".", (os.path.basename(options.out),)), "w", encoding="utf-8", newline="\n") as out:
            out.write("id\tindex\tkind\ttext\n")
            for entry in (int(x) for x in options.entries.split(",") if x):
                data = archive.read(entry)
                for ident, index, kind, text in dialogue_of(data):
                    out.write("%d\t%d\t%d\t%s\n" % (ident, index, kind, text.replace("\n", "\\n")))
                    total += 1
        print("wrote %s, %d dialogue strings" % (options.out, total))

if __name__ == "__main__":
    main(sys.argv[1:])
