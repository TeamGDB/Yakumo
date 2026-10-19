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

## Quests mods add

A mod adds a quest under an id from 60000 to 65535, which the game's id-to-file code never handles (`[Quests]` in mod.ini: `60001 = quest.bin`). The file is the payload, as above, with the same id in its record. At start Yakumo:

1. checks the instructions around `0x088C18B8` and reads the keys from the executable in memory; on another executable nothing is served;
2. encrypts each quest and serves it as a new file, quest `60000 + n` as file `6000 + n` (hex), through the same takeover of the loader that serves files mods add ([DATA_BIN.md](DATA_BIN.md#files-mods-add));
3. hooks the instruction at `0x088C18B8`: for a mod's quest id in `t1`, `s0` becomes its file, and the game goes on to read, decrypt and play it like its own.

The developer tools list mods' quests with the game's own, and `quest start 60001` departs on one from the village. In game (macOS), a mod's quest 60001, made from quest 204 with a Tigrex, started from the village and the Tigrex was on the map.

`profiles/mhp3rd/tools/quests.py` lists the disc's quests, extracts a quest's payload from your own disc image, shows its fields and changes its id, name and large monsters.

## The counter

The counter's lists (entries 4059 to 4066) hold one record per quest: the quest record and its texts, copied from the quest's payload up to a table of seven pointers, one per language, each to six pointers to the name, objective, failure conditions, description, main monsters and client. `+0x18` of the record points to that table. All of these are offsets from the start of the list file, so a record moved into a list has them rebased; the Japanese strings are zeroed in this release's lists. A rebuilt list with the payload of a new quest added this way loads and reads like the game's own.

The counter does not show what a list holds, though: it shows the quests the save has opened, by id, and looks each one's record up in the list. A record under an id the save has not opened (219, an unused village id, and 60001 were both tried) stays hidden.

The village's quest selection (`0x0A0B1678` in `lobby_task.ovl`) treats every id above 59999 as a downloaded quest and looks its record up among the downloaded quests, not in the lists. The game keeps downloaded quests in a save of their own, `ULJM05800QST/MHP3RD.BIN`; like the game's main save, its data file is encrypted twice: by the save-data utility, which Yakumo already handles, and by the game, whose cipher is `0x0888F114` (a byte substitution through a table at `0x089D5F60` and two number generators, run by the `checkSumThread` at `0x08890970`, with a SHA-1 over the decrypted data). The quest loader reads a downloaded quest from that save instead of from DATA.BIN when the flag at `+0x21DC` of its state is 1, and decrypts it with the same quest cipher. Serving mods' quests as downloaded quests is how they will reach the counter, in single player and in multiplayer; that part is not built yet.

## Not traced yet

- Where the counter offers downloaded quests, the downloaded quest save's layout, and the game's save cipher in full.
- The tables at `+0x24` to `+0x2C`, the reward tables' layout, the small monsters' groups beyond their records, and the area's rules for positions.
- What the u16 before the area and the word at `+0xC` of a spawn record mean.
