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
3. The translations the release ships: `translations/` next to the executable,
   `Yakumo.app/Contents/Resources/translations` in the macOS bundle, or the
   unpacked assets of the Android app

Files must end in `.lang`. To add a language, drop a file into the data
directory's `translations` folder (the menu shows that folder) and set the
language, normally through the menu.

The repository ships the full Brazilian Portuguese translation,
`profiles/mhp3rd/translations/pt-BR.lang` (machine translated, with a
hand-checked glossary), and `es.lang`, a shorter Spanish example; a build copies
them next to the executable.

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

The width is a hard limit: the game wraps a line that runs past the field's edge
without stopping at a space, so a translation wider than the source splits a
word. A translation that does not fit is shortened, never widened. A few fields
are narrower than their source text (some menu labels, the status screen's skill
names, an item description's three and a half lines); the local tool names them
and holds each translation to the field's own box.

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
   language in **System -> Text** and restart.

An entry left untranslated is not an error: it falls back to the game's own
text, so a file may cover one screen at a time. A few things to keep in mind
while translating:

- **Keep the codes.** `~Cnn` sets a colour and `~Bnn` draws a button glyph; copy
  every one, in order. `%s`, `%d` and friends are the game's own substitutions.
- **Use `\n` for a line break.** The file is one line per key, so a real break is
  written `\n`. `\\`, `\r`, `\t` and `\#` are understood too.
- **Keys may be a range or a wildcard.** `2:308-382 = ...` sets a whole run and
  `2:*` every entry of a table, which keeps a file short when a word repeats.
- **Quote the quest names.** The game's own text wraps a name in the "mountain
  bracket" (`<<...>>` / `…`); use ordinary double quotes instead.
- **Keep each text inside its field.** See *Field sizes* below; a text that is
  too wide is wrapped by the game without stopping at a space and splits a word,
  and one with too many lines runs past the box.

## Field sizes

The game draws each string in a field sized to the text it was authored for, so
the translation has to fit that field, not the screen. The rule of thumb is the
source's own line count and width; a few fields are narrower, and one skill
panel is wider. These are the fields that were measured:

| Where | Key | Field |
| --- | --- | --- |
| Dialogue (NPCs, quests, shops) | `[4289]`-`[4291]`, `id:index` | the source's own lines and width; never wider, or the game wraps it and splits a word |
| Menu labels and system messages | `[16] 2:*` | one line, about the source's width; the item-box menus hold 16 |
| Item and equipment names | `[16] 3:*` and the even tables to `38` | item names are one line of at most 15 |
| Item and equipment descriptions | `[16] 4:*` and the odd tables to `38` | the source's lines; the item detail box shows three and a half lines |
| Smithy and item-box options | `[2838] 2:*` | 16-17 |
| Change-equipment menu | `[2838] 2:283-322` | 17 |
| Guild card page and card list | `[2838] 2:733-763` | 17 |
| Status screen skill names | `[16] 2:1090-1189` | 15 |
| The "Expert" title | `[16] 2:936` | 10 |
| Skill descriptions | `[2838] 5:*`, `[2840] 3:*` | the source's lines; 30 wide when three lines or fewer, 19 otherwise |
| The smithy's armor entries | `[2838] 2:36`, `2:97` | shortened to `Armdr` / `Forjar Armdr` |

Two more rules the fields taught:

- **No space before a punctuation mark.** The model sometimes writes `item .`;
  when the text is wrapped the mark is left alone on a line. Portuguese sets no
  space before it either, so it is joined to the word (`item.`).
- **A word is never wider than the field.** The game has nowhere to break there
  and splits the word, so a too-long word is replaced by a shorter one (the
  smithy's `Armor` becomes `Armdr`, `Heavy Bowgun` becomes `Fuzil. Pesado`).
- **A name may be abbreviated with a period** when the field is narrow and the
  sense survives: `Armadura` -> `Arm.`, `Peixe Dourado Pequeno` ->
  `Peixe Dour. P.`. Item names are capped at fifteen characters; a translation
  that needs more is shortened, never left to overflow.

A translation is wrapped to the field's width, and one that still needs too many
lines is shortened, never widened. The Brazilian Portuguese file was checked
against these rules with a small harness of its own; the rules above are the
ones it enforces, kept here so another language can be checked the same way.

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
