# Translating the game's own text

Yakumo can replace the text the game shows — menu options, button prompts and
system messages — with a translation, without touching the disc image. The
game's own text stays exactly where it is; a translation file only names the
entries it changes, and every entry it leaves out falls back to what the game
already has (the English of a patched disc, or the Japanese of an original
one).

The translation files are plain UTF-8 text and contain no game data.

## Choosing a language

| Where | What |
| --- | --- |
| Menu (**System → Text → Game text language**) | Cycles through English and every translation file found. Applies at the next start, like a mod's choice |
| `settings.ini`, key `text.language` | The same choice, by hand |
| `MHP3RD_LANGUAGE`, for one run | Overrides the file for the run, not written back |

`en` (the default) loads nothing and keeps the disc's own text.

## Where the files are looked for

In order, the first match for the chosen language wins:

1. `MHP3RD_TRANSLATIONS_DIR`, when set
2. `translations/` in the per-user data directory
3. `translations/` next to the executable

Files must end in `.lang`. To add a language, drop a file into the data
directory's `translations` folder (the menu shows that folder) and set the
language, normally through the menu.

The repository ships two examples, `profiles/mhp3rd/translations/pt-BR.lang`
and `es.lang`; a build copies them next to the executable.

## The file format

```text
# comments start with # or ;
language = pt-BR
name = Português (Brasil)

[16]                     # the shared block: menus, items, equipment
2:20 = Cancelar
2:21 = Sim
2:115 = Missão concluída!

[2835]                   # another archive block, of its own
2:129 = Bem-vindo ao mundo de Monster Hunter.
```

- `language` names the language and `name` what the menu shows. A file with no
  `language` line is named by its own file name (`de.lang` → `de`).
- A `[entry]` header names the archive entry (a block) the lines below belong
  to; `[main]` is a shorthand for `[16]`. A file written the old way (no
  `[entry]`) is read as the main block, so older files still load.
- Every other line is `TABLE:INDEX = TEXT`.
  - In the main block, table 2 holds the menu options and system messages, table
    3 the item names, tables 5 to 38 the equipment names and descriptions in
    pairs (`docs/DEBUG_MENU.md`). In the dialogue block (4289) the numbers are
    `id:index` instead (see *The dialogue block* below).
  - A key may be a range (`2:308-382`) or a wildcard (`2:*`, `*:5`), which keeps
    a file small when the same text repeats.
  - `TEXT` is UTF-8. `\n`, `\r`, `\t`, `\\` and `\#` are understood.
  - The game's own formatting codes (`~B..` for a button glyph, `~C..` for a
    colour) are copied as they are, in order.
- A repeated key keeps the last value; a line that is not a key is ignored.

To find a key to translate, run the game with a developer build
(`MHP3RD_DEBUG_MENU=1`), open the menu's **Debug** page and use `table 2`, or
`table 2 FIRST N` to start further in.

### The dialogue block

The NPC and quest dialogue is archive entry **4289**, in a different shape: a
list of `(id, offset)` pairs, each a block of `(kind, offset)` pairs, each a
string. It is not a text block, so its keys are `id:index`, e.g. `[4289]` with
`0:4 = ...`. `tools/extract_dialogue.py` reads it and `host/text` applies it the
same way (the strings go to the arena, the sub-block's offset is rewritten).

## How it works

The game keeps its text in several blocks, each an archive entry: the shared one
(entry 16) loaded at a fixed address, and a quest's, a menu's or the dialogue's
loaded as the game needs them, into buffers whose addresses change.

When a language is chosen, the text is applied between two frames
(`host/hle/hle_media.cpp` calls `text::frame` at the flip):

1. The file I/O tells `text::translate_read` every read of `DATA.BIN`
   (`hle_io.cpp`); a read that carries a block the file names, whole or gathered
   in pieces, lets the block be found once the game has loaded it (the main
   block by its fixed address, another by the first string it holds).
2. Each translated string is copied into one arena reserved in guest memory
   through the kernel's own allocator, and the table's (or sub-block's) offset
   for that string is pointed at it. Because the string lives in the arena, a
   translation may be longer or shorter than the original, and does not have to
   fit the block.
3. A block the file does not name, a string it does not translate, or a table it
   cannot read is left alone: the game's own text is shown.

The archive is obfuscated per 2 KiB block, so a block read in pieces is
decrypted whole before anything is read from it; a partial read is collected
until enough has been seen. Nothing reads the disc image, and nothing is written
before the game loads its text. `host/text/` holds the file format
(`language.{hpp,cpp}`) and the apply logic (`translation.{hpp,cpp}`); both are
unit-tested in `tests/text_tests.cpp` on a buffer, with no game data.

### Fitting the box

The game draws a string in the lines its own text had: a dialogue's are about 21
characters, a menu's about 32. A translation wrapped differently is cut, and one
with more lines overflows the box. `tools/build_lang.py` (and the local
translation tool) wraps each translation to the source's **line count and
width**, so it fits where the game puts it.

## Limits

- **One language per run.** The choice is applied once, when the game's text
  loads. Changing it needs a restart, as the menu says.
- **Entry 0 of a table cannot be translated.** Its offset word doubles as the
  table's header, so Yakumo leaves it alone (35 such strings, `No Equipment`
  among them).
- **Only blocks the file names.** The eight text blocks and the dialogue block
  are the game's text; the thousands of other archive entries are models,
  textures and data, not text.
- **A glyph the font lacks draws as its fallback.** A translation with
  accented letters needs a font that has them; install one with
  `text.font` (see the profile README) if the game's own font does not. The
  English patch's font has them.

## Adding a language

1. Copy `profiles/mhp3rd/translations/pt-BR.lang` to `<code>.lang` and set
   `language` and `name`.
2. Translate the lines you want, keeping the `TABLE:ENTRY` keys and any `~B..`
   or `~C..` codes.
3. Put the file in the data directory's `translations` folder, then choose the
   language in **System → Text** and restart.

An entry left untranslated is not an error: it falls back to the game's own
text, so a file may cover one screen at a time.

## Working from the disc's whole text

Two tools in `profiles/mhp3rd/tools/` dump the game's text and turn a
translation back into `.lang` files. Nothing they read or write is committed;
both write under `docs/TEXT_DUMP/`, which is ignored.

```sh
# The eight text blocks, with the exact table numbering the game uses:
# entry, table, index, text (one row per string).
python3 profiles/mhp3rd/tools/extract_blocks.py game.iso blocks.tsv

# The dialogue of entry 4289: id, index, kind, text.
python3 profiles/mhp3rd/tools/extract_dialogue.py game.iso dialogue.tsv

# The whole image at once (slower), classified as English or Japanese-leftover,
# into docs/TEXT_DUMP/strings.csv, strings_jp.csv and codes.txt.
python3 profiles/mhp3rd/tools/extract_text.py game.iso docs/TEXT_DUMP --no-runs
python3 profiles/mhp3rd/tools/text_report.py docs/TEXT_DUMP
```

`strings.csv` holds every string as `entry,table,index,kind,has_format,codes,
text`: `kind` is `en`, `jp`, `misto` or `sym`, and `has_format` marks the rows
that carry the game's own `~Cnn`/`~Bnn` codes. Entries 2835–2841 are the
quest/menu tables, 16 the big block (menu, items, equipment), 4703–4716 the
download quests.

The translations themselves live in `translations/glossary.tsv`, one row per
string: `entry, table, index, english, portuguese, spanish`. The English column
is only a check: `build_lang.py` reads the English back from the disc and stops
on a mismatch, so a key cannot drift from what the game holds.

```sh
# glossary.tsv -> pt-BR.lang and es.lang, checked against the disc
python3 profiles/mhp3rd/tools/build_lang.py game.iso
```

`glossary_from_menu.py`, `glossary_from_items.py`, `glossary_from_quest.py` and
`glossary_from_options.py` append batches to the glossary from a dictionary of
terms; they read `docs/TEXT_DUMP/strings.csv` and are kept for reference. A
translation added by hand to the glossary, or to a `.lang` directly, works the
same.
