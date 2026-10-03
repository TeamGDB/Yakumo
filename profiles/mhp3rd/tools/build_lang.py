#!/usr/bin/env python3
"""Build the .lang translation files from translations/glossary.tsv.

The glossary is one row per string: entry, table, index, English, Portuguese,
Spanish. This writes pt-BR.lang and es.lang next to it, one `TABLE:ENTRY = TEXT`
line each, keeping the game's own formatting codes (~Cnn, ~Bnn) untouched.

With an image argument it checks every row against the disc: the entry, table
and index must exist and the English must match what the disc holds, so a wrong
row is caught before it reaches the game.

    python3 build_lang.py [IMAGE.iso]
"""
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
GLOSSARY = os.path.join(HERE, "..", "translations", "glossary.tsv")
OUT = os.path.join(HERE, "..", "translations")

HEADER = {
    "pt-BR": ("pt-BR", "Português (Brasil)"),
    "es": ("es", "Español"),
}


def unescape(text):
    """`\\n` and `\\t` in the glossary stand for the game's real breaks."""
    return text.replace("\\n", "\n").replace("\\t", "\t")


def read_glossary():
    rows = []
    with open(GLOSSARY, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 6:
                continue
            entry, table, index, en, pt, es = parts[:6]
            rows.append((int(entry), int(table), int(index), unescape(en), unescape(pt), unescape(es)))
    return rows


def check_with_disc(rows, image):
    sys.path.insert(0, HERE)
    import databin
    import struct
    archive = databin.Archive(image)

    def text_of(entry, table, index):
        data = archive.read(entry)
        t = struct.unpack_from("<I", data, table * 4)[0]
        first = struct.unpack_from("<I", data, t)[0]
        count = first // 4 - 1
        if not 0 < index < count:
            return None
        off = struct.unpack_from("<I", data, t + index * 4)[0]
        end = data.index(b"\x00", t + off)
        return data[t + off:end].decode("utf-8", "replace")

    bad = 0
    for entry, table, index, en, _pt, _es in rows:
        disc = text_of(entry, table, index)
        if disc != en:
            bad += 1
            print("MISMATCH entry %d %d:%d\n  glossary: %r\n  disc    : %r" % (entry, table, index, en, disc))
    print("checked %d rows, %d mismatch" % (len(rows), bad))
    return bad == 0


def escape(text):
    """A real line break would split the line; the format's \\n keeps it."""
    return text.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n")


def write_lang(code, name, rows, column):
    path = os.path.join(OUT, "%s.lang" % code)
    # The format groups strings by the archive entry they come from, so the
    # same table:index is not confused between blocks (docs/TEXT_TRANSLATION.md).
    by_entry = {}
    for entry, table, index, _en, pt, es in rows:
        text = pt if column == "pt" else es
        if text:
            by_entry.setdefault(entry, []).append((table, index, text))
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# %s\n" % name)
        handle.write("# Gerado de translations/glossary.tsv por tools/build_lang.py.\n")
        handle.write("# Entradas sem traducao aqui caem no texto do proprio jogo.\n")
        handle.write("language = %s\n" % code)
        handle.write("name = %s\n" % name)
        count = 0
        for entry in sorted(by_entry):
            handle.write("\n[%d]\n" % entry)
            for table, index, text in sorted(by_entry[entry]):
                handle.write("%d:%d = %s\n" % (table, index, escape(text)))
                count += 1
    print("wrote", path, "(%d rows)" % count)


def main(argv):
    rows = read_glossary()
    if not rows:
        raise SystemExit("no rows in %s" % GLOSSARY)
    if argv:
        if not check_with_disc(rows, argv[0]):
            raise SystemExit("fix the mismatches before building")
    write_lang("pt-BR", HEADER["pt-BR"][1], rows, "pt")
    write_lang("es", HEADER["es"][1], rows, "es")


if __name__ == "__main__":
    main(sys.argv[1:])
