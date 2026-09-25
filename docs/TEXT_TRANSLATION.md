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

2:20 = Cancelar
2:21 = Sim
2:115 = Missão concluída!
```

- `language` names the language and `name` what the menu shows. A file with no
  `language` line is named by its own file name (`de.lang` → `de`).
- Every other line is `TABLE:ENTRY = TEXT`.
  - `TABLE` and `ENTRY` are decimal indices into the game's text block. Table 2
    holds the menu options and system messages, table 3 the item names, table 2
    from entry 308 the monster names, and tables 5 to 38 the equipment names
    and descriptions in pairs. `docs/DEBUG_MENU.md` describes how they were
    found and lists the equipment tables.
  - `TEXT` is UTF-8. `\n`, `\t`, `\\` and `\#` are understood; any other
    character after a backslash is left as it is.
  - The game's own formatting codes (`~B..` for a button glyph, `~C..` for a
    colour) are copied as they are. A translation that changes a prompt must
    keep them.
- A repeated key keeps the last value; a line that is not a key is ignored.

To find the entry to translate, run the game with a developer build
(`MHP3RD_DEBUG_MENU=1`), open the menu's **Debug** page and use `table 2`, or
`table 2 FIRST N` to start further in.

## How it works

The game loads one block of its text at start (at `0x08A40640` on NPJB-40001,
the executable Yakumo supports). The block is a header of offsets to tables;
each table is a list of 32-bit offsets to UTF-8 strings, ended by `0xFFFFFFFF`
(`host/debug/game_state.cpp` reads it the same way).

When a language is chosen, between two frames (`host/hle/hle_media.cpp` calls
`text::frame` at the flip):

1. The block is checked and, once the game has loaded it, its tables are read.
2. Each translated string is copied into a small arena reserved in guest memory
   through the kernel's own allocator, and the table's offset for that entry is
   pointed at it. Because the string lives in the arena, a translation may be
   longer or shorter than the original.
3. An entry with no translation, an entry the table does not have, or a string
   that does not fit the arena is left alone: the game's own text is shown.

Nothing is ever written before the game loads its text, and nothing reads the
disc image: `host/text/` holds the file format (`language.{hpp,cpp}`) and the
apply logic (`translation.{hpp,cpp}`); both are unit-tested in
`tests/text_tests.cpp` on a buffer, with no game data.

## Limits

- **One language per run.** The choice is applied once, when the game's text
  block loads. Changing it needs a restart, as the menu says.
- **Entry 0 of a table cannot be translated.** Its offset word doubles as the
  table's header, so Yakumo leaves it alone.
- **Only tables the game lays out as this block does.** A table that fails the
  sanity check (a bad end marker, entries out of order) is skipped whole, and
  its entries fall back.
- **A glyph the font lacks draws as its fallback.** A translation with
  accented letters needs a font that has them; install one with
  `text.font` (see the profile README) if the game's own font does not.

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
# Every string block of the image: docs/TEXT_DUMP/all_strings.tsv and one
# readable file per entry. --no-runs skips the loose-text scan (slow).
python3 profiles/mhp3rd/tools/extract_text.py game.iso docs/TEXT_DUMP --no-runs

# Classify as English or Japanese-leftover and build worksheets.
python3 profiles/mhp3rd/tools/text_report.py docs/TEXT_DUMP     # strings.csv, strings_jp.csv, codes.txt
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
