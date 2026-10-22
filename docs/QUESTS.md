# Quests: the game's quest files and quests mods add

Each quest the game plays is one `DATA.BIN` file, encrypted. This page says how the files are laid out, how the game decrypts them, how a quest id leads to its file, and how Yakumo serves quests that mods add (mod.ini's `[Quests]`, [MODDING.md](MODDING.md#quests-new-quests)). Everything here was traced on NPJB-40001 in this project, with the developer tools and script mods described below; no other project's notes or code were used.

## Where a quest's file is

The quest overlay asks for a quest by id through `0x088C1770`. An id is `A * 10000 + stars * 100 + n`, and the file is a base for `A` and the stars, plus `n`:

| Quests | Ids | Files |
| --- | --- | --- |
| Village, 1 to 6 stars | 101 to 699 | 2842 for 101, then 64 files per star level |
| Guild Hall, 1 to 8 stars | 10101 to 10899 | from 3226, 64 files per star level |
| Two more ranges and one arena quest | 2xxxx, 4xxxx, 45001 | further up to 3994 |

Every other id reaches `0x088C18B8` with no file (`0xFFFF` in `s0`, the id in `t1`), and the quest is not started. The file is read whole, through the loader's request submit (`0x08863CDC`, called through the loader's table at `0x0896F648`, slot `+0x30`), into a buffer of 24576 bytes.

Found by tracing, in order: `MHP3RD_TRACE_IO=1` showed the read of entry 2909 right after quest 204 started; a write watch (`PSPRECOMP_WATCH_WRITE`) on the decrypted copy found the code that wrote it; a script mod that hooked every call of that code, comparing the buffer before and after each, narrowed it to `0x0888EE30`, and the id arithmetic was read in the executable.

## The cipher

`0x0888EE30` decrypts a file:

- The file starts with four 16-bit seeds. Each starts one of four keys; a seed of 0 starts its key from that key's multiplier.
- Every 16-bit word after the seeds is XORed with the next value of one key: for the word at byte offset `p` of the file, key `(p >> 1) & 3`, stepped first as `key = key * multiplier[i] % modulus[i]`. The multipliers and moduli are u32 tables at `0x089D5CE0` and `0x089D5CD0`.
- Bytes 8 to 31 decrypt to the payload's size (u32, at most 24576) and a SHA-1 (20 bytes). The payload follows, the keys running on from the header.
- The game appends 17 bytes from `0x0896BB48` to the payload in its buffer and hashes the payload and the first 16 of them; a mismatch refuses the quest.

Checked by decrypting every quest file on the disc with an independent implementation: each digest matches, and quest 204's payload is byte for byte the game's in-memory copy at `0x08A3A630`, apart from the ids the game writes into its spawn records after loading. Encrypting a payload again with the file's own seeds gives the disc's bytes. Yakumo reads the keys from the running executable and never stores them (`profiles/mhp3rd/host/game/quest_file.cpp`).

## The payload

All offsets are from the payload's start, all values little-endian.

| Offset | What |
| --- | --- |
| `+0x00` | Offset of the quest record (0x94 on the disc's quests) |
| `+0x04` | `v005` |
| `+0x08` | Offset of the reward tables |
| `+0x1C` | Offset of the small monsters' spawn groups |
| `+0x20` | Offset of the large monsters' group |
| `+0x24`..`+0x2C` | Offsets of further tables, not traced |

The **quest record** is the same record the counter's lists hold (entries 4059 to 4066, one per star level): the fee at `+4`, the reward at `+8`, the time limit in frames (30 a second) at `+0x10`, the id (u16) at `+0x1C`, the stars at `+0x1E`, the map at `+0x20`, the objective's monster kind and count (u16 each) at `+0x2C`, up to five main monsters (u16) at `+0x3C`, and from `+0x48` the name, objective, failure conditions, description, main monster lines and client, each ended by a zero byte, in English and then in Japanese.

The **large monsters' group** holds a count, a word, the offset of the list of kinds to load (u16 kind and u16 zero each, ended by `0xFFFF`) and the offset of the spawn records. A **spawn record** is 0x30 bytes: the kind (u32), a u16, the area (u16), at `+0xC` a word the game keeps, and at `+0x10` the position as three floats. Small monsters' spawn groups use records of the same size.

Monster kinds seen so far: 1 Rathian, 2 Rathalos, 42 Arzuros, 44 Volvidon, 45 Great Wroggi, 58 Tigrex, 63 Bulldrome.

Checked by changing them: quest 204 served with the Arzuros' kind changed to 58 in the list, the spawn record, the objective and the main monster loaded the Tigrex's overlay (`em058m0`), and the developer tools' `quest` command listed "Tigrex 1750/1750".

## Event quests

Ids above 59999 are event quests, which the game handles like downloaded ones. The Guild Hall's quest counter offers them under *Event Quest*: the game first loads the save of downloaded quests, `ULJM05800QST`, into a buffer of 100 slots of 24592 bytes (allocated by `0x088914CC`, its address at `+0x950` of the state `*0x09FBC940`). Without that save it offers "the included bonus content", the event quests on the disc: 41 of them, ids 60001 to 60053, whose ids are listed in `lobby_task.ovl` at `0x0A188B1C` (u16 each, 0 after the last). The loop at `0x0A0B1324` loads each into the next free slot through `0x0888F9F4`:

- the file is `id + 9530` for ids up to 61000 and `id + 8583` above (16 bits): files 3995 to 4047 for the bonus quests, and 4048 to 4058 for ids 61001 to 61011, which the game has too;
- the file is read whole with the loader's synchronous read (`0x08864C54`, [DATA_BIN.md](DATA_BIN.md#how-the-game-reads-it)), decrypted with the quest cipher into the slot, and the quest's name copied to a 43-byte record per slot (`+0x1490` of the state, 43 bytes each), which marks the slot as used;
- `0x0A0B11A8` then sorts the slots' quests into the Low Rank and High Rank lists by bytes `+0x21` and `+0x22` of the record, and the quest loader plays an accepted one from its slot (the flag at `+0x21DC` of its state is 1).

Found by following the counter in game: a script mod hooked the slot allocator, logged each slot's id and name, and a write watch on a slot's name record found the decryption writing it; the counter's menu was reached by setting the hunter's position (`hunter.load_step`'s object, `+0x80`) through the developer tools.

## Quests mods add

A mod adds a quest under an id from 62000 to 65535 (`[Quests]` in mod.ini: `62001 = quest.bin`), above the game's event quests. The file is the payload, as above, with the same id in its record. At start Yakumo:

1. checks the instructions around `0x088C18B8` and `0x0888FA4C` and reads the keys from the executable in memory; on another executable nothing is served;
2. encrypts each quest and serves it as a new file, quest `62000 + n` as file `6000 + n` (hex), through the same takeover of the loader that serves files mods add ([DATA_BIN.md](DATA_BIN.md#files-mods-add));
3. makes the event quest list longer by one row per quest (the game table `quest.events`, [MODDING.md](MODDING.md#game-tables)) and, each time `lobby_task` loads, writes the mods' ids after the game's;
4. hooks the instruction at `0x0888FA4C`, where the event quest loader has worked out the file for the id in `a1` into `s3`: for a mod's quest, `s3` becomes its file, and the game reads, decrypts, lists and plays it like its own event quests;
5. hooks the instruction at `0x088C18B8`, where the id-to-file code arrives with no file (`0xFFFF` in `s0`, the id in `t1`): for a mod's quest, `s0` becomes its file, for the developer tools' `quest start`.

In game (macOS), a mod's quest 62001, made from quest 204 with a Tigrex, was listed under *Event Quest*, *Low Rank Quest*, accepted at the counter for its fee, and departed on from the Hall's gate; the game loaded the Tigrex's overlay on the quest's map. The developer tools list mods' quests with the game's own, and `quest start 62001` departs on one from the village.

When a save of downloaded quests exists, the game loads it instead of offering the bonus content, and mods' quests are not listed; that case was not tried.

`profiles/mhp3rd/tools/quests.py` lists the disc's quests, extracts a quest's payload from your own disc image, shows its fields and changes its id, name and large monsters.

## Quests a mod describes

A native mod can describe a quest instead of shipping a payload: `yakumo.quests` builds the payload (`game/quest_builder.cpp`) and serves it as a quest added from memory, at the counter and for the developer tools as above. A built quest is a hunt: its map, its large monsters (by id, the game's or a mod's) with the area and position each starts at and their levels, the objective, time, fee, reward, stars and texts. It can carry a main reward: lines of items, each line one draw from items whose chances add up to 100, named by the item names the game shows. Those names are only known once the game's text is loaded, so such a quest is served first without them and built again, with its file replaced, once `lobby_task` loads. It can name an area a mod added ([MAPS.md](MAPS.md#areas-mods-add)): its map is then that area's, and the area's files are the mod's while it is played. Nothing in it comes from a quest on the disc. Built that way, a quest in the arena (map 8) with a mod's monster loaded, spawned the monster with the health and size asked for, and showed the reward menu when the monster fell.

What the builder writes, found by comparing the disc's 379 quests and changing one field at a time on a quest played in the arena:

| Where | What |
| --- | --- |
| Header `+0x30` + 8 × *i* | Large monster *i*'s stats, up to five. The first word: its health level in the top byte (`0x1E` gave a Tigrex-based kind 5400 health, `0x0A` 1750; the game's hunts use `0x0A` to `0x34`) and its size in percent in the low half (`0x96`: 150); byte 2 not traced. The second word: its attack level in the low byte (`0x30` hit three times as hard as `0x13`), then two bytes not traced (`0x13` and `0x03` in most hunts) |
| Header `+0x58`, `+0x5C` | The same two words for the small monsters |
| Header `+0x24` | Three pointers (and a 0) to the small monsters' area index: eight bytes, then entries of 16 (two spawn lists, the area, 0) ended by a zero list. Empty, its second word must be `0x9999`: with 0 there the large monsters never came |
| Header `+0x28` | Further spawn records (0x30 bytes each) ended by kind `0xFFFF` |
| Header `+0x2C` | 52 bytes starting `FF FF 00 FF FF FF` in every quest looked at |
| Header `+0x08` | The rewards: groups of 8 bytes (u8 kind, 0 the main reward; u8 its line count; then a word not traced) ended by `FF`, 8 bytes in all for the end, then each line's items of 4 bytes (u8 chance in percent, u8 count, u16 item) ended by `0xFFFFFFFF`. The builder writes the main reward's lines; a built quest with them was cleared in the arena and showed its reward pages |
| Header `+0x1C`, `+0x60` | Gathering and supplies: lists the builder leaves empty (`FF FF` and zeros), which the game took |
| Record `+0x00` | The quest's kind (`4` a hunt, `1` slay, `2` deliver, `8` main monsters); `+0x27` the number of objectives, `+0x28` the first one's kind (1 hunt or slay, 2 deliver) |
| Record `+0x0C` | A third of the reward in the game's hunts |
| Record `+0x18` | Seven pointers, one per language, each to six pointers to the texts |
| Record `+0x38` | 0, or the training quests' loadout |

The large monsters' group and spawn records are as above; the builder writes the kinds to load once each.

## The quest lists

The counter's lists for the village and the Guild Hall's ranks (entries 4059 to 4066) hold one record per quest: the quest record and its texts, copied from the quest's payload up to a table of seven pointers, one per language, each to six pointers to the name, objective, failure conditions, description, main monsters and client. `+0x18` of the record points to that table. All of these are offsets from the start of the list file, so a record moved into a list has them rebased; the Japanese strings are zeroed in this release's lists. These lists show only the quests the save has opened: a record under an id the save has not opened (219, an unused village id, was tried) stays hidden.

## Not traced yet

- The save of downloaded quests: its layout and the game's cipher on it (`0x0888F114`, run by the `checkSumThread` at `0x08890970`, with a table at `0x089D5F60` and a SHA-1 salted from `0x0896BB5C`).
- What the 28-byte record per slot at `+0x954` of the state holds, and how a quest reaches the other hunters in multiplayer.
- The tables at `+0x24` to `+0x2C`, the reward tables' layout, the small monsters' groups beyond their records, and the area's rules for positions.
- What the u16 before the area and the word at `+0xC` of a spawn record mean.
