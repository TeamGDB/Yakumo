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
| Menu (**System → Text → Game text language**) | Cycles through Original and every translation file found. Applies at the next start, like a mod's choice |
| `settings.ini`, key `text.language` | The same choice, by hand |
| `MHP3RD_LANGUAGE`, for one run | Overrides the file for the run, not written back |

`original` keeps the disc's own text. An `en.lang` file can supply English on
an unpatched Japanese image; without that file, `en` also keeps the source text.

## Where the files are looked for

In order, the first match for the chosen language wins:

1. `MHP3RD_TRANSLATIONS_DIR`, when set
2. `translations/` in the per-user data directory
3. `translations/` next to the executable (a place to drop a file by hand),
   `Yakumo.app/Contents/Resources/translations` in the macOS bundle, or the
   unpacked assets of the Android app

Files must end in `.lang`. To add a language, drop a file into the data
directory's `translations` folder (the menu shows that folder) and set the
language, normally through the menu.

No translation ships with Yakumo. Community translations are linked from the
project README; download one and use **Import translation…** in
**System → Text**, which copies the file into the per-user folder for you, or
drop the `.lang` file in one of the folders above yourself.

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
    pairs, table 40 the save/load screen and tables 45–50 the Felyne (Amigato)
    equipment descriptions and names (`docs/DEBUG_MENU.md`). Tables 41–43 are the
    chat profanity filter, not text the game draws, and are left alone. In the
    dialogue block (4289) the numbers are `id:index` instead (see *The dialogue
    block* below), and in a quest file they are `ref:offset` (see *Quest files*).
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

### Quest files

The village quests are archive entries **4059–4066** and **4070–4073**, each a
list of records: the record offsets sit at the top, and every record holds a
table of offsets (absolute in the entry) to its fields — the title, the
objective, the result line, the description, the monsters and the client. Those
offsets are the keys' `ref:offset`: the position of the offset word, and the
offset it holds, e.g. `[4059]` with `1624:1192 = Derrota Jaggi` (the title
"Jaggi Takedown", whose offset word is at 1624 and points at 1192). The run-time
patch finds the file in RAM by its first title and rewrites each word to point
into the arena, so a quest translation may be any length, unlike a fixed field.
Reloaded catalogs are translated again. The loader completion signal supplies a
validated destination hint for complete quest catalog reads; bounded RAM search
remains a fallback for other copies.

The active quest uses a separate container and can load without reading a quest
catalog first. It also has an offset table; its text is not limited to inline
slots. Yakumo prepares the selected language's catalog metadata from the active
archive, matches the active quest's ID and original fields, and repoints its
validated field words into a separate arena slice. Unknown layouts or mismatched
strings remain untouched. Existing `ref:offset` language keys stay unchanged.
Long translations can still exceed the game's visual layout; pointer storage
removes the source-buffer limit, not the screen's width limit.

Quest source fields accept complete UTF-8, including Japanese and empty optional
fields. Invalid encoding, disallowed controls, missing terminators, strings
outside their record and tables without their own sentinel are rejected.

An English patch can preserve the quest table's position while moving the text
within its reserved slots. Exact `ref:offset` keys are specific to that layout.
For a verified shared reference position, use the existing wildcard syntax,
for example `[4064]` with `644:* = Translated title`. The runtime resolves it
against the selected image's validated fields before applying a catalog or an
active quest. Exact keys take precedence. Wildcards do not include sentinels or
unrecognized fields and do not bypass source-string or pointer validation.
Compare quest IDs and field positions in both images before creating these
aliases; do not assume a different mod or patch preserves the same layout.

For NPJB-40001, the active container starts at `0x08A3A630`; its first word is the
record offset, and the record holds the quest ID at `+0x1C` and text from `+0x48`.
The executable's accessor at `0x088BE458` resolves the record through that first
word. A bounded macOS experiment confirmed that changing one field offset
changes the text shown by the active-quest details screen. The implementation
validates offsets, original strings, record identity and the table sentinel
before changing any field. It currently handles ordinary village and Hall IDs;
DLC support and language files supplied by mods are deferred.

`MHP3RD_TEXT_NO_QUEST_DIRECT=1` restores the previous quest discovery/application
path for comparisons. Restart after changing the language or source files.

## How it works

The game keeps its text in several blocks, each an archive entry: the shared one
(entry 16) loaded at a fixed address, and a quest's, a menu's or the dialogue's
loaded as the game needs them, into buffers whose addresses change.

When a language is chosen, the text is applied between two frames
(`host/hle/hle_media.cpp` calls `text::frame` at the flip):

1. The file I/O tells `text::translate_read` every read of `DATA.BIN`
   (`hle_io.cpp`); a read that carries a block the file names, whole or gathered
   in pieces, lets the block be found once the game has loaded it (the main
   block by its fixed address, another by the first string it holds). A block the
   game loads again - leaving a quest reloads the menus - is translated again,
   each block keeping one slice of the arena.
2. Each translated string is copied into one arena reserved in guest memory
   through the kernel's own allocator, and the table's (or sub-block's) offset
   for that string is pointed at it. Because the string lives in the arena, a
   translation may be longer or shorter than the original, and does not have to
   fit the block.
3. A block the file does not name, a string it does not translate, or a table it
   cannot read is left alone: the game's own text is shown.

The archive is obfuscated per 2 KiB block, so a block read in pieces is
decrypted whole before anything is read from it; a partial read is collected
until enough has been seen. Quest metadata preparation reads bounded entries
through the active archive view without marking them as guest loads. No game
files are modified, and guest writes wait for a validated loaded structure. `host/text/` holds the file format
(`language.{hpp,cpp}`) and the apply logic (`translation.{hpp,cpp}`); both are
unit-tested in `tests/text_tests.cpp` on a buffer, with no game data.

### Runtime search budget

Fallback discovery of loaded blocks and inline quest structures is incremental. All
pending searches share at most 256 KiB of candidate addresses per game frame;
the first entry rotates so an absent probe cannot starve another block. A
candidate is checked against the complete memory range, so its strings can
cross a search slice boundary. Quest-file copies are patched as they are found,
and later sweeps retain discovery of inline structures created behind a cursor.
A reread restarts the cursors while reusing the same translation arena.

Known quest catalog destinations and the validated active quest avoid that sweep.
A full fallback 32 MiB sweep takes 128 search slices, spread over frames (longer when
several blocks share the budget). Until discovery, the original text can remain
visible. This bounds search work, not translation application or a wall-clock
frame deadline. `MHP3RD_TRACE_TEXT=1` reports each search's reserved candidate
bytes, completion and elapsed microseconds. For a performance comparison only,
`MHP3RD_TEXT_SEARCH_UNLIMITED=1` restores unsliced, blocking memory searches.

### Character widths

The game's text paths use fixed half-width or full-width cells rather than the
horizontal advance reported by the font. Yakumo classifies Cyrillic
(U+0400-U+052F) as half-width, matching Latin letters; Japanese and other original
character classes are unchanged. Latin-1 accented letters and inverted Spanish
punctuation already use half-width cells. A font containing these glyphs is
still required. This fixes excessive Cyrillic spacing; it does not automatically
shorten translations that exceed a field's width or line count.

`MHP3RD_ORIGINAL_TEXT_WIDTH=1` restores the original classification for comparison.
`MHP3RD_TRACE_FONT=1` reports each observed character's class and caller along
with the existing glyph rasterization trace. Regenerate the main corpus with
`profiles/mhp3rd/scripts/generate.sh` when adopting this change: the shared text
classifier must remain interceptable at same-unit calls. Only its callers' main
AOT unit changes; runtime headers and overlay ABI do not change.

### Fitting the box

The game draws a string in the lines its own text had: a dialogue's are about 21
characters, a menu's about 32. A translation wrapped differently is cut, and one
with more lines overflows the box. The translation tooling wraps each translation
to the source's **line count and width**, so it fits where the game puts it.

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
- **Only blocks the file names.** The eight text blocks, the dialogue block and
  the twelve quest files are the game's text; the thousands of other archive
  entries are models, textures and data, not text.
- **Quest keys depend on the source layout.** Use exact offsets for one image,
  or verified reference wildcards for images sharing the same field positions.
  Missing translation keys still display the source text.
- **The chat filter is not translated.** Entry 16 tables 41–43 are the word list
  the chat censor uses, not text the game draws; a `.lang` must not name them, or
  translating would break the filter.
- **A glyph the font lacks draws as its fallback.** A translation with
  accented letters needs a font that has them; install one with
  `text.font` (see the profile README) if the game's own font does not. The
  English patch's font has them.

## Adding a language

1. Copy an existing translation (or start from an empty file) to `<code>.lang`
   and set `language` and `name`.
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
| Save/load screen | `[16] 40:*` | the source's lines; the short labels (`MONEY`, `TIME`) are one line, the source's width plus the label slack |
| Felyne (Amigato) equipment names | `[16] 46:*`, `48:*`, `50:*` | one line, the source's width plus the label slack |
| Felyne (Amigato) descriptions | `[16] 45:*`, `47:*`, `49:*` | the source's own lines and width |
| Quest fields | `[4059]`–`[4073]`, `ref:offset` | the source's own lines and width (the box the English already wraps to) |

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

The tools in `profiles/mhp3rd/tools/` dump the game's text so it can be
translated. Nothing they read or write is committed; the dumps go under
`docs/TEXT_DUMP/`, which is ignored.

```sh
# The eight text blocks, with the exact table numbering the game uses:
# entry, table, index, text (one row per string).
python3 profiles/mhp3rd/tools/extract_blocks.py game.iso blocks.tsv

# The dialogue of entry 4289: id, index, kind, text.
python3 profiles/mhp3rd/tools/extract_dialogue.py game.iso dialogue.tsv

# The whole image at once (slower), classified as English or Japanese-leftover,
# into docs/TEXT_DUMP/strings.csv, strings_jp.csv and codes.txt. It reads the
# eight text blocks and the twelve quest files (4059-4073).
python3 profiles/mhp3rd/tools/extract_text.py game.iso docs/TEXT_DUMP --no-runs
python3 profiles/mhp3rd/tools/text_report.py docs/TEXT_DUMP
```

`strings.csv` holds every string as `entry,table,index,kind,has_format,codes,
text`: `kind` is `en`, `jp`, `misto` or `sym`, and `has_format` marks the rows
that carry the game's own `~Cnn`/`~Bnn` codes. Entries 2835–2841 are the
quest/menu tables, 16 the big block (menu, items, equipment, save/load, Felyne
equipment), and 4059–4073 contain quest catalog records. Entries 4703–4716
were previously described here as download quests, but their outer structure is
not recognized by the quest extractor. Downloaded savedata follows a separate
load path; support must not be inferred from these archive entry numbers. See
the [quest research checkpoint](../profiles/mhp3rd/tests/manual/translation_quests.md#mod-and-downloadable-quest-research)
for observed limits and outstanding verification.

This repository ships only the mechanism. The glossary and the script that turns
it into `.lang` files live with the translation itself, outside this repository,
so no game-derived text is committed here.

## Import validation and runtime limits

Translation files must be UTF-8 without embedded NUL bytes. A UTF-8 BOM is
accepted. Files are limited to 16 MiB and individual lines to 64 KiB. The
language code contains 1-64 ASCII letters, digits, hyphens or underscores;
`original` is reserved for the unchanged game text. English codes such as `en`
can name an imported translation too. Invalid input leaves the previous file
in place. Imports stage the validated bytes before replacing the destination;
symlink and directory destinations are refused. If replacement and rollback
both fail, the error names the retained recovery copy.

On Android, **Import translation** opens the system document picker. The
selected document is copied with the same file-size limit into private staging,
then passed through the regular importer. Cancelling writes nothing; temporary
copies are removed after success or failure.

Exact keys take precedence over range/wildcard rules. Among overlapping rules,
the first matching rule wins. `*:*` matches all eligible table entries; index
zero remains unchanged in ordinary text tables. Repeated matches share one
immutable translated string in guest memory, so expanding a wildcard does not
exhaust a reservation calculated from its source text.

Archive reads are gathered by their actual byte ranges. Unread gaps are never
parsed as zeros. Runtime collection is limited to 4 MiB per entry, 16 MiB across
pending entries, and 4096 disjoint fragments per entry. Short or malformed
headers are skipped safely. Reloading a block reuses its arena slice.

Synthetic regression suites `mhp3rd_text_tests`, `mhp3rd_text_runtime_tests` and,
on Unix/Android, `mhp3rd_translation_document_tests` run in the existing public
unit-test, coverage and sanitizer workflows. They use invented strings, archive
metadata and encrypted bytes, never extracted game fixtures. Python text-tool
contracts are part of `tool_security_tests.py`. Worksheet CSV output quotes
formula-like text; extraction outputs are checked against symlink escapes.
Actual translated gameplay, font/layout fit and Android provider behavior still
need device verification with a translation supplied locally by its author.

### CLI path security review

The extraction and worksheet commands are local tools run with the operator's
own filesystem privileges. Their input images and output/dump roots are
explicitly selected through command-line arguments; these paths are not received
from the game archive or a remote user. The PR #227 CodeQL Python dataflows at
`Archive` input opening, extractor output opening, `extraction_path` root
resolution and worksheet input/output all originate at these CLI arguments.
Those path-injection findings were reviewed as false positives. Archive-derived
components still require validation and resolved containment below the selected
root, with traversal and symlink regression tests; this does not authorize
exposing the tools as a service accepting untrusted root paths.
