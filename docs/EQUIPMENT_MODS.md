# Equipment mods: which file a piece of equipment uses

An equipment mod (`Type="EquipSET"`, `EquipHEAD`, `EquipGS`, ...) is a model
without a fixed target: the player says which file of `DATA.BIN` it replaces,
and the pieces that use that file then look like the mod. The Mods page can
fill the targets in from the running game:

- **Use My Current Armor** points each slot at the model file of the armor the
  loaded hunter wears in that part, for the hunter's sex. A weapon mod's button
  reads *Use My Current Weapon* and takes the carried weapon, if it is of the
  mod's class.
- **No Armor** points each armor slot at the file the game shows when that part
  is empty, for the hunter's sex and inner wear.
- Each slot shows, next to its file id, what the game draws from that file,
  with the game's own names read from the running game ("*name*, female",
  "*name* and 1 more, male", "No armor (inner wear), female").
  Typing an id stays possible.

Both buttons only read the game's memory and write only the mod's choices in
`mods.ini`, so they work in release builds and during ad hoc play. They
refuse, and say why, while no character is loaded (the title screen and the
character select) or when a weapon mod's class is not the one the hunter
carries. Felyne gear (`EquipCATSET`, `EquipCATHELM`, ...) has no buttons.

## When the hunter shows the mod

The game loads the armor models when it loads the character and when the
equipment changes at the item box; it keeps them otherwise. Measured with
`MHP3RD_TRACE_DATA_BIN=1`:

| What the player does | The armor models are read again |
| --- | --- |
| Starts the game and enters the village | Yes |
| Changes to another armor set at the item box (Manage Equipment) | Yes, at once |
| Chooses the set the hunter already wears | No |
| Walks from the village into the Guild Hall | No |

So after pressing a button: at the item box change to other armor and back,
or restart. When a mod's file is larger than the file it replaces, the Mods
page says that a restart is needed and offers *Restart Now*, as for any other
mod.

Not measured: whether starting a quest or returning from one reloads them.

## What the game keeps where

All addresses are for NPJB-40001. They were found by tracing, not taken from
elsewhere: the model files the game read were logged with
`MHP3RD_TRACE_DATA_BIN=1`, memory was dumped with the developer tools'
`dump` command ([DEBUG_MENU.md](DEBUG_MENU.md)) and searched offline, and the
code that uses each table was read in the executable to confirm what it
does. The code is `profiles/mhp3rd/host/game/equipment_models.{hpp,cpp}`, and
`tests/equipment_models_tests.cpp` checks it on made-up tables.

### The character

The game reaches the loaded character through the pointer at `0x08AB3640`,
plus `0x85C`: `0x09F4FCAC`, the block that starts with the hunter's name
(`kCharacter` in `host/game/game_data.hpp`). The same layout sits at
`0x09554FA0` while the character select shows the save; the game copies it
when it enters the village.

| Offset | What | How it was found |
| --- | --- | --- |
| `+0x1B` (u8) | Sex: 0 male, 1 female | The code that builds the player copies it to the player object (`+0x458`), which picks the male or female model and file base. 1 on the save's female hunter; set to 0 in the character select's copy, the village loaded the male files for the same armor |
| `+0x1D` (u8) | Inner wear, 0 to 3 | Copied to the player object (`+0x4F0`) and used only to pick the model of an empty chest, arms or legs slot. 2 on the save's hunter; set to 0, the empty slots loaded the models of inner wear 0 |
| `+0x1E`, `+0x1F` | Face, hair | Copied beside it; the face and hair files follow from them (not used here) |
| `+0x38` (24 bytes) | The carried weapon: u8 used, u8 kind, u16 id, then weapon data | Its kind and id matched the Equipment screen's weapon |
| `+0x50` (5 × 12 bytes) | The worn armor: chest, arms, waist, legs, head, each laid out like an equipment box record (u8 used, u8 kind, u16 id, ...) | The ids matched the worn set's names in the name tables; the game's own checks read the ids at `+0x52`, `+0x5E`, `+0x6A`, `+0x76`, `+0x82` |

### Armor data

One table per part in the executable, 40 bytes per armor id; the first two u16
are the model number for a male and for a female hunter, the byte at `+4` says
who can wear the piece (bit 0 male, bit 1 female, bit 2 blademaster, bit 3
gunner).

| Part (kind) | Table |
| --- | --- |
| Chest (0) | `0x08985A7C` |
| Arms (1) | `0x08983934` |
| Waist (2) | `0x0898A6E4` |
| Legs (3) | `0x0898C854` |
| Head (4) | `0x08987EE4` |

Found by searching a memory dump for a table whose entries, by armor id, grow
with the file ids that a community file list gives for the same armor names
(the list was used only as a hint, and nothing of it is in the repository);
the only stride that fit was 40 bytes, and the five tables lie one after
another. The executable's code then showed the game reading them by kind with
exactly these bases, and the male or female half by the sex byte.

### From a model number to a file

The file is a base per sex and part plus the model number, and the bases are
a table in the executable at `0x089E892C`: u16 `[sex][part]`, seven parts per
sex (chest, arms, waist, legs, head, hair, face). On NPJB-40001 the armor parts
have 125 models each. The game's code adds the table's base to the model
number it got from the armor table; the result is the file it reads.

A model number of 0 is the bare part, but for chest, arms and legs the game
shows the inner wear instead: a pointer per sex at `0x089E8704` (chest),
`0x089E86EC` (arms) and `0x089E871C` (legs) leads to a list of model numbers by
inner wear (60 to 63 on this disc). Waist and head show model 0.

Checked in the running game with the 100% test save (a female hunter wearing
the Silver Sol set, inner wear 2):

| Case | Predicted and read |
| --- | --- |
| Female, Silver Sol set | head `02BB`, arms `0144`, chest `00C7`, waist `01C1`, legs `023E` |
| Female, empty chest | chest `00C3` (inner wear 2 of the chest); the Gordon mod's chest aimed there showed on the bare hunter |
| Female, everything empty | `0279`, `0140`, `00C3`, `017F`, `023A` |
| The same hunter made male, Silver Sol, empty chest | chest `034B`, arms `03CE`, waist `044B`, legs `04C8`, head `0545` |
| Male, Hunter set (changed at the item box) | `0317`, `0394`, `0411`, `048E`, `050B` |

That also explains the ids a male hunter with no armor reads (head `0503`,
arms `03C8`, chest `034B`, waist `0409`, legs `04C2`): inner wear 0 of the
chest, arms and legs, and model 0 of the head and waist, which is what the
community lists call "no equipment" for every part.

### Weapons

A weapon's record starts with its model number too. Its file is a base per
weapon class plus that number, kept below the class's model count: bases u16
at `0x089CD22C` and counts u16 every 4 bytes at `0x089CD1F8`, by class, which
is the kind minus 5 (kind 10 is unused and has no models). The record tables
by kind, as the game's code picks them:

| Kind | Table | Record |
| --- | --- | --- |
| 5 Great Sword | `0x08997AA0` | 28 |
| 6 Sword and Shield | `0x089953B0` | 28 |
| 7 Hammer | `0x08994054` | 28 |
| 8 Lance | `0x0899669C` | 28 |
| 9 Heavy Bowgun | `0x08990464` | 80 |
| 11 Light Bowgun | `0x08991954` | 80 |
| 12 Long Sword | `0x08997138` | 28 |
| 13 Switch Axe | `0x08992F0C` | 28 |
| 14 Gunlance | `0x08995E14` | 28 |
| 15 Bow | `0x0898EB14` | 80 |
| 16 Dual Blades | `0x08994A9C` | 28 |
| 17 Hunting Horn | `0x089936EC` | 28 |

Checked by the files read for two great swords (`05F4`, `05DD`) and a long
sword (`06B7`). The other classes follow the same code and were not loaded;
bowguns and bows may use more files than one.

## Weapons mods add

A native mod adds a weapon of its own with `yakumo.equipment` ([MODDING.md](MODDING.md#weapons)): its class, the game's weapon of that class it starts from, its model, name, description and price. What it takes from the base is the base's record, its attack, element, sharpness, slots, rarity and upgrades as the game keeps them there; which field is which is not traced yet beyond the model number (`+0`), rarity (`+2`), price (`+4`) and the description's index (`+0x18`).

What Yakumo does, and how it was found:

| Step | How |
| --- | --- |
| The class's record table a row longer | Every place the executable forms one of the twelve tables' addresses is one of four getters (by field, by id, and by equipment record, `0x0885CB38` to `0x0885D8C8`); no overlay reads them. The tables are game tables (`weapon.great_sword` and the rest, [MODDING.md](MODDING.md#game-tables)), with as many rows as the class's name table has names: 92 great swords, 95 swords and shields, 94 hammers, 97 lances, 67 and 68 bowguns, 86 long swords, 72 switch axes, 78 gunlances, 81 bows, 83 dual blades, 86 hunting horns |
| Its model | The model number is past the class's models (the counts above). Three functions turn a weapon's model number into its file, the only readers of the counts: they clamp the number below the class's count, then add the class's first file. The hunter's (`0x088A59CC`, on a quest and in the village), the character select's hunters' (`0x088F46D8`) and a hunter class of `lobby_task`'s own (`0x0A10EE8C`). An instruction hook where each clamps (`0x088A59E4`, `0x088F4754`, `0x0A10EF28`: the model in a1, the count in v1) lets a mod's number through, and one where each keeps 16 bits of the sum (`0x088A5A08`, `0x088F4780`, `0x0A10EF54`: the sum in a0) gives it the mod's file, served as a file added from memory; the class is in s0, or s1 in `lobby_task`'s, whose code is checked as it runs. With the first only, the character select drew a saved mod's great sword as the class's last model (file `05F8`). A weapon file is a pack of a PMO model, a skeleton and textures, as a monster's model pack |
| Its sounds | A quest loads the sound banks of the hunter's weapon by its model number: `0x088B9BEC` takes the model from `0x0885E6C8` (the record's `+0`, for every class) and reads a row of 6 bytes in the class's table of banks, reached through a list of the twelve tables at `0x089EA1D0` (by kind less 5; kind 10 has none): the bank's header file and its body (u16 each, a `Head`/`Prog` header of 2 KiB and its samples) and a byte that picks a pair the class shares (`0x089EA15C`). A class's table has as many rows as the class has models, and nothing bounds the model. A weapon mod's model past them read the next table's bytes as file ids: the great sword of a test mod (model 40) asked the loader for file `EAFC`, whose size, read from past the loader's table, was over 2 GiB; split into requests of 128 KiB, it ran over the loader's queue of 128, and the next request copied to an address outside memory, which stopped the game as the second quest loaded. The twelve tables are game tables (`weapon.great_sword.sounds` and the rest, with the list's word as the data that holds each one's address): Yakumo moves the class's with a row for the mod's model and puts the base weapon's row there, so the weapon sounds like its base |
| Its name and description | Added to the class's name and description tables of the main text block (`mods/mhp3rd_texts.cpp`): each table is copied with the strings after the game's, its offsets rebased, and the block's header pointed at the copy, once the block is loaded and again whenever the game loads it. The name lands at the weapon's id; the description's index goes into the record |
| One in the box | When asked, once the village has loaded with a character, one in the first free slot of the equipment box unless the box holds one already |

Checked in game (macOS): a test mod's great sword got id 92, its name and description showed in the developer tools' `table 5` and `table 6`, and one was put in the equipment box. Carried (written into the character's weapon record with the developer tools' `poke16`, on a player's own save), the village and the next quest loaded its model file and the quest its base's sound banks (`0844`, `080A`); before its sound banks were added, that quest stopped the game as it loaded (2026-10-25). Saved on the character, it drew on the character select from its own file (`FFF1`), where it was the class's last model before. Not checked yet: equipping it at the box, looking at its model on the hunter, and hearing its sounds. A save that holds a mod's weapon needs the mod to load it: without the mod its id is past the table.

## Armor mods add

A native mod adds an armor piece with `yakumo.equipment`'s `add_armor` ([MODDING.md](MODDING.md#weapons)): its part, the game's piece of that part it starts from, a model file for a male and one for a female hunter, its name, description and price. Its record is the base's (defence, resistances, skills, slots, who may wear it); traced fields are the two model numbers (`+0`, `+2`), who may wear it (`+4`), the price (`+8`, u32), its decoration slots (`+0x12`, u8, 0 to 3) and the description's index (`+0x1A`).

| Step | How |
| --- | --- |
| The part's record table a row longer | The five tables are read through twenty `lui`/`addiu` pairs each, all in the executable (none in any overlay, none with a field offset in the low half), with no bound: they are game tables (`armor.head` and the rest), with as many rows as the part's name table has names: 256 heads, 233 chests, 213 arms, 214 waists, 220 legs. They are also read through a list of the five tables' addresses at `0x089832D8` (chest, arms, waist, legs, head, by an equipment record's part), which nine functions index, among them the count of a piece's slots (`0x0885EED8`) that the equipment screens fill a list of three with; the list follows each table when it moves. Left at the game's tables, a mod's waist piece read a count of 214 there, and opening the equipment box's screen with it overran the screen's stack and stopped the game |
| Its models | The hunter's file function (`0x088A58DC`) gets the model from `0x08869778` (by the hunter record's id), lets `0x088A51E4` turn model 0 of chest, arms or legs into the inner wear, adds the base for its sex and part and keeps 16 bits: no bound on the model. Two more functions do the same with the model `0x08869564` gives by armor id: the character select's hunters' (`0x088F46D8`) and a hunter class of `lobby_task`'s own (`0x0A10EE8C`). A mod's piece's files are served as files added from memory (ids from `FFFE` down), and its model for each sex is its file less the base for that sex and part, so the sum is the mod's file in all three. Numbered from 1000 at first, with an instruction hook at `0x088A5968` making the first function's sum the mod's file, a piece saved on the character made the character select load unrelated disc files (a male hunter in the Fatalis mod's set: `06F9`, `0778`, `07F7`, `0876`, `08EB`) and stop the game as it loaded the save's characters. The game only compares models past its own (the head's special cases 112, 114 and 118; the waist's 47) and turns only 0 into the inner wear, which a mod's model never is. An armor file is the game's own format for that part (a PMO on the hunter's skeleton) |
| Its name and description | The part's name and description tables (29 to 38, names then descriptions, head, chest, arms, waist, legs), as for weapons |
| One in the box | As for weapons, with the part's kind |

The look comes from the hunter record (`LAYERED_ARMOR.md`), the stats from the character's worn pieces; the game copies one to the other when it loads the character or the equipment changes at the box. Checked in game (macOS) with a test mod's head piece (the stats of head 1, the models of the Silver Sol helm read from the disc): it got id 256, `armor.head` moved with 257 rows, one was put in the equipment box, and with its id written in both records the quest's hunter loaded the mod's file for its head and drew it. The Fatalis mod's set, on a player's own save with a male hunter (macOS, 2026-10-25): written into both records, the quest and the village loaded its male files; changed at the house's item box (Change Equipment), the village hunter loaded the files of each piece chosen there through the same function; saved on the character, the character select loaded the set's files at the next start and went on to the village. Not checked: what `lobby_task`'s own hunter class draws (no run made one).

An armor file the game takes, as its own pieces are made and as the Fatalis mod's set from Freedom Unite showed when each piece was worn in the arena's camp (macOS, 2026-10-23):

- **Its skeleton holds the part's bones, named, in the hunter's order:** a chest the whole hunter (21 bones, NULL, COG, BD-00, BD-01, AL-00 to AL-03, AR-00 to AR-03, NK-00, HD-00, WST-00, LL-00 to LL-02, LR-00 to LR-02); arms BD-01, AL-00 to AL-03, AR-00 to AR-03; a waist BD-00, WST-00, LL-00, LL-01, LR-00, LR-01; legs WST-00, LL-00 to LL-02, LR-00 to LR-02; a head BD-01, NK-00, HD-00. Legs holding the whole hunter stopped the game; a chest holding 10 of the 21 bones drew stretched up to the sky, and drew right with the others added.
- **Each bone at the hunter's offset from its parent:** arms with another rest pose stopped the whole scene drawing.
- **A vertex group keeps to one side:** groups holding both arms' bones stopped the game.
- **Small groups:** the game's arms use at most 2 bones a group (its chests up to 4); arms in groups of 3 stopped the scene drawing, in groups of 2 they drew.

## Not verified

- Weapon mods: the formula was checked against the files the game read, and
  the button against a stand-in mod (it set the great sword's file, and
  refused a long sword mod while the hunter carried a great sword), but no
  real weapon mod was shown on the hunter.
- A male character: the save has only a female hunter, so the male case was
  made by changing the sex byte of the character select's copy in memory (never
  saved). A hunter created male was not tried.
- Armor pieces that share a model share a file: a mod aimed at one shows on
  all of them. The slot's description says "and N more" when that is so.
