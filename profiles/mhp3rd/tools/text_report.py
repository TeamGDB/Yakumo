#!/usr/bin/env python3
"""Build translation worksheets from a text dump made by extract_text.py.

Reads <dump>/all_strings.tsv (entry, table, index, text) and writes, in the
same folder:

    strings.csv     entry, table, index, kind, has_format, codes, en, jp, text
                    one row per string, ready for a spreadsheet. `kind` is
                    en/jp/misto/sym; `has_format` is 1 when the game's own
                    formatting (~Cnn colour, ~Bnn button glyph, \\n break) is
                    present; `codes` lists the distinct codes; `jp` holds the
                    original for the rows whose text is English when a
                    Japanese row exists at the same position in another dump
                    (see --jp), else empty.
    strings_en.csv  only the rows whose text is not Japanese
    strings_jp.csv  only the Japanese-leftover rows (dialogue, descriptions)
    codes.txt       every formatting code and how often it appears

The text is the game's own and is never committed; a .csv written here stays
in the ignored docs/TEXT_DUMP.
"""
import argparse
import csv
import os
import re
import sys

CODE = re.compile(r"~[A-Za-z]\d\d?")


def kind(text):
    """en, jp, misto (Latin and full-width Japanese) or sym."""
    cjk = any("\u3040" <= c <= "\u30ff" or "\u4e00" <= c <= "\u9fff" for c in text)
    fullwidth = any("\uff01" <= c <= "\uff5e" or "\uff66" <= c <= "\uff9f" for c in text)
    latin = any(c.isascii() and c.isalpha() for c in text)
    if cjk or (fullwidth and not latin):
        return "jp"
    if fullwidth and latin:
        return "misto"
    if latin:
        return "en"
    return "sym"


def codes_of(text):
    return sorted(set(CODE.findall(text)))


def clean(text):
    """The text without the game's formatting codes, for reading."""
    return CODE.sub("", text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", help="the folder extract_text.py wrote")
    options = parser.parse_args(argv)

    tsv = os.path.join(options.dump, "all_strings.tsv")
    if not os.path.exists(tsv):
        raise SystemExit("no %s; run extract_text.py first" % tsv)

    rows = []
    with open(tsv, encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t", 3)
            if len(parts) != 4:
                continue
            entry, table, index, text = parts
            the_kind = kind(text)
            codes = codes_of(text)
            rows.append({
                "entry": entry,
                "table": table,
                "index": index,
                "kind": the_kind,
                "has_format": "1" if (codes or "\\n" in text or "\n" in text) else "0",
                "codes": " ".join(codes),
                "text": text,
            })

    # A same-position Japanese row for English rows, when a second dump holds
    # it (a patched disc against an original one). Left empty otherwise.
    with open(os.path.join(options.dump, "strings.csv"), "w", encoding="utf-8",
              newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["entry", "table", "index", "kind",
                                                    "has_format", "codes", "en", "jp", "text"])
        writer.writeheader()
        for row in rows:
            writer.writerow({
                "entry": row["entry"], "table": row["table"], "index": row["index"],
                "kind": row["kind"], "has_format": row["has_format"], "codes": row["codes"],
                "en": row["text"] if row["kind"] == "en" else "",
                "jp": row["text"] if row["kind"] in ("jp", "misto") else "",
                "text": row["text"],
            })

    for name, wanted in (("strings_en.csv", {"en"}), ("strings_jp.csv", {"jp", "misto"})):
        with open(os.path.join(options.dump, name), "w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=["entry", "table", "index", "kind",
                                                        "has_format", "codes", "text"])
            writer.writeheader()
            for row in rows:
                if row["kind"] in wanted:
                    writer.writerow(row)

    counts = {}
    for row in rows:
        for code in row["codes"].split():
            counts[code] = counts.get(code, 0) + 1
    with open(os.path.join(options.dump, "codes.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# formatting codes in the game's text, and how often each appears\n")
        for code, count in sorted(counts.items(), key=lambda item: -item[1]):
            handle.write("%s\t%d\n" % (code, count))

    by_kind = {}
    for row in rows:
        by_kind[row["kind"]] = by_kind.get(row["kind"], 0) + 1
    print("rows:", len(rows), "by kind:", by_kind)
    print("wrote strings.csv, strings_en.csv, strings_jp.csv, codes.txt in", options.dump)


if __name__ == "__main__":
    main(sys.argv[1:])
