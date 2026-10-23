# Large monsters: kinds, their tables, and adding one

A quest names its large monsters by *kind*, a number from 0 to 74 (1 Rathian, 2 Rathalos, 58 Tigrex: [QUESTS.md](QUESTS.md#the-payload)). This page lists what the game keeps for each kind, how a mod gives a new kind its rows through [game tables](MODDING.md#game-tables), and what a new kind does not have yet. Everything here was traced on NPJB-40001 in this project, with script mods that hooked the game's code and logged what it read, and by reading the executable and the overlays. For [a monster's model](#a-monsters-model) the layouts of the model, texture and motion files were first read from public notes on Monster Hunter Freedom Unite's formats, and every field below was then checked against the code that reads it; no other project's code was used.

## The kind and its model set

A large monster's object keeps its kind as a byte at `+0x62`. The kind is also the number of its *model set* (`em` in the overlays' names: the Tigrex's AI overlay is `em058m0`), and the three files of each model set are found by that number in three tables of the executable, u16 by set from 1:

| Table | Address | Read by | Tigrex (58) |
| --- | --- | --- | --- |
| `monster.models` | `0x08982DD0` | `0x088564E4`; `0x08856684` searches it, with the count 74 at `0x08856694` | 5498 |
| `monster.models.second` | `0x08982E64` | `0x08856664` | 5499 |
| `monster.models.third` | `0x089FC040` | `0x088F4BBC` | 5500 |

Found by hooking the resource loader `0x08856BDC` (its third argument is the file) on a Tigrex quest and on one with a new kind: the new kind loaded every file the Tigrex did except 5500, and the code that asked for 5500 (called with the model set's number from `0x08856F44`) read `0x089FC040`. A model set without its third file crashed in a chunk walk at `0x088A0890`.

## The game's kinds by id

Mods name kinds by id ([MODDING.md](MODDING.md#large-monsters)): the game's own English names (text block 16, table 2, from entry 308, by kind), in lower case with `_`. A subspecies shares its base's name in that table; which kind is which was told apart by the names of the disc's quests that load it: 15 in *Golden Lunar Wheel*, 18 in *Silver Fire Wheel*, 20 in *Black Anger in the Night*, 23 in *Roar of the Black Tiger*, 32 in *Green Gale*, 51 to 57 in quests for Crimson Qurupeco, Baleful Gigginox, Sand Barioth (*Sand Barioth Training*), Jade Barroth, Steel Uragaan, Purple Ludroth (*Gather! Purple Water in Forest*) and Glacial Agnaktor (*Ice Wedge*), while 58 is in *Tigrex Interception* and 59 in *Hungry Eyes*. Kind 68 is a second Slagtoth whose difference from 49 is not traced; it is `slagtoth_2`.

| | | | |
| --- | --- | --- | --- |
| 1 `rathian` | 2 `rathalos` | 3 `qurupeco` | 4 `gigginox` |
| 5 `barioth` | 6 `diablos` | 7 `deviljho` | 8 `barroth` |
| 9 `uragaan` | 10 `jaggi` | 11 `jaggia` | 12 `great_jaggi` |
| 13 `baggi` | 14 `great_baggi` | 15 `gold_rathian` | 16 `royal_ludroth` |
| 17 `ludroth` | 18 `silver_rathalos` | 19 `agnaktor` | 20 `black_diablos` |
| 21 `uroktor` | 22 `delex` | 23 `brute_tigrex` | 24 `alatreon` |
| 25 `jhen_mohran` | 26 `giggi` | 27 `aptonoth` | 28 `popo` |
| 29 `rhenoplos` | 30 `felyne` | 31 `melynx` | 32 `green_nargacuga` |
| 33 `altaroth` | 34 `kelbi` | 35 `gigginox_egg` | 36 `bnahabra` |
| 37 `bnahabra_plains` | 38 `bnahabra_tundra` | 39 `bnahabra_volcano` | 40 `zinogre` |
| 41 `amatsu_magatsuchi` | 42 `arzuros` | 43 `lagombi` | 44 `volvidon` |
| 45 `great_wroggi` | 46 `duramboros` | 47 `nibelsnarf` | 48 `wroggi` |
| 49 `slagtoth` | 50 `gargwa` | 51 `crimson_qurupeco` | 52 `baleful_gigginox` |
| 53 `sand_barioth` | 54 `jade_barroth` | 55 `steel_uragaan` | 56 `purple_ludroth` |
| 57 `glacial_agnaktor` | 58 `tigrex` | 59 `nargacuga` | 60 `akantor` |
| 61 `ukanlos` | 62 `bulldrome` | 63 `bullfango` | 64 `anteka` |
| 65 `cart_felyne` | 66 `nyanjiro` | 67 `veggie_elder` | 68 `slagtoth_2` |
| 69 `rock` | 70 `rock_plains` | 71 `rock_forest` | 72 `rock_tundra` |
| 73 `rock_volcano` | 74 `rock_peaks` |  |  |

## What the quest overlay keeps for each kind

`game_task.ovl`, loaded for a quest, has five more tables indexed by kind, each with 75 rows:

| Table | Address | Row | What it is | How it was found |
| --- | --- | --- | --- | --- |
| `monster.kinds` | `0x0A17FE84` | 8 bytes | A pointer to the kind's AI overlays and their descriptors, and how many slots it takes. Read by `0x0A08CC60` and `0x0A08CE1C`; `0x0A08CF0C` checks the kind against 75 first | The code that finds the AI overlay a quest's kind loads |
| `monster.files` | `0x0A1834F0` | 8 bytes | Four more files loaded with the kind on a quest (u16, `0xFFFF` for none). Read by nine places in the executable and six in the overlay | A new kind without its row crashed walking a resource at `0x08876A10` |
| `monster.constructors` | `0x0A19F440` | 4 bytes | Where `0x0A090AE0`, which makes a large monster, jumps for the kind: the code that builds its object. A kind of 75 or more gets a generic object that never moves | The new kind's object had a vtable in `game_task` (`0x0A1A5610`) where the Tigrex's had one in `em058m0` (`0x0A1D9708`); `0x0A090AE0` compares the kind with 75 before its jump |
| `monster.sizes` | `0x0A19F2D8` | 4 bytes | Where `0x0A08C8A4` jumps for the kind: the code that returns the size of its object (0 for 75 or more) | The other jump on the kind compared with 75 |

| `monster.carves` | `0x0A194B0C` | 4 bytes | A pointer to the kind's 16-byte records, 0 for none. Each holds an index into a list chosen by the quest's rank (through `0x0A192FB4`), a float and a byte, and the records look like its carving points (four for the Tigrex). Read with no bound by `0x0A164F88` when the monster is killed, and by `0x0A164914` | A new kind killed at 1 health stopped at a read of `0x20044`, the word past the table's end; with the Tigrex's row, the hunter carved it |

The two jump tables hold addresses in `game_task`, which always loads at the same address, so a row copied from another kind stays valid.

## Adding a kind

The `yakumo.monsters` service does all of this for a mod ([MODDING.md](MODDING.md#large-monsters)): one call with an id, a base and the mod's model files. What follows is what it does, and how a mod can do it by hand.

A mod extends each table by one row (kind 75: 76 rows by kind, 75 by model set), writes the new rows, and serves quests that name the kind ([MODDING.md](MODDING.md#quests-new-quests)). Starting from an existing kind, copy its row in every table: the executable's at start, `game_task`'s from the `"overlay"` event once `game_task` has loaded. A script mod:

```lua
-- Kind 75, a copy of the Tigrex (58) to start from.
local function copy_row(name, from, to)
    local base, _, row = yakumo.table(name)
    yakumo.writebytes(base + to * row, yakumo.readbytes(base + from * row, row))
end
for _, name in ipairs({"monster.kinds", "monster.files", "monster.constructors", "monster.sizes", "monster.carves"}) do
    yakumo.extend_table(name, 76)
end
for _, name in ipairs({"monster.models", "monster.models.second", "monster.models.third"}) do
    yakumo.extend_table(name, 75)
    copy_row(name, 57, 74) -- model sets count from 1
end
yakumo.on("overlay", function(base)
    if yakumo.overlay_at(base) ~= "game_task" then return end
    for _, name in ipairs({"monster.kinds", "monster.files", "monster.constructors", "monster.sizes", "monster.carves"}) do
        copy_row(name, 58, 75)
    end
end)
-- The Tigrex's AI tells the Tigrex from kind 23 in four places: kind 75 goes the Tigrex's way.
for _, at in ipairs({0x0A1BB304, 0x0A1C0318, 0x0A1C0500, 0x0A1C2848}) do
    yakumo.hook_instruction(at, function(cpu) if cpu.v1 == 75 then cpu.v1 = 58 end end, "em058m0")
end
```

With quests made from the disc's quests 204 and 20201 (`quests.py edit … --monster 42=75`), kind 75 loaded the Tigrex's files and overlay, had its health, moved around the map the way the Tigrex does in the same quest (its position logged every two seconds), fought the hunter on screen, and, hit at 1 health, died, ended the quest and was carved.

### AI overlays that know their kinds

An AI overlay can serve more than one kind and tell them apart by the kind byte. `em058m0` serves kinds 23 and 58 (both rows of `monster.kinds` name its file first), and compares the kind with 58 at `0x0A1BB300`, `0x0A1C0314`, `0x0A1C04FC` and `0x0A1C2844` (`lbu v1, 0x62(...)`, `addiu v0, zero, 58`, `beq v1, v0`). A new kind is not 58, so without the hooks above it goes kind 23's way: there it asked for effects 0x17 and 0x1C of the resources the Tigrex's files give (`0x0895A9DC`, called from `game_sub` at `0x0A013C54`), which those files do not have, and `game_sub` stopped at `0x0A0106B8` on a resource header pointing outside memory. That happened in arena runs once the monster was hurt or set to 1 health, and also by hand; the Tigrex asked for neither effect. A new kind made from another monster has to check that monster's overlay for comparisons with its kind (`lbu …, 0x62(…)` followed by a constant) and send the new kind the base's way.

An AI overlay comes once for each monster slot it can load in (`m0` to `m3` in its name; the slots' load addresses are the overlay slots in `mhp3rd_profile.hpp`), and which one a monster's AI loads as depends on the slot the quest gives it, which has not been traced: a Tigrex in the second slot loads `em058m1` at `0x0A1EFE80`. Compared word by word on NPJB-40001, `em058m1` is `em058m0` linked `0x34E80` further on: the same code and data, its jumps and address pairs moved, its kind checks at the same offsets from its load address (`0x0A1F0180` and on) and its vtable at `0x0A20E588` instead of `0x0A1D9708`, with that overlay's functions in it. The Tigrex has no `m2` or `m3`. The monsters service hooks the kind checks in each slot's overlay, and gives a kind's monsters a copy of whichever base vtable they were made with.

A new monster of its own then changes its rows: its own model set's files (files a mod adds, [MODDING.md](MODDING.md#add-new-files)), its own AI overlay through `monster.kinds`, its own files in `monster.files`.

## A monster's model

Found while putting a model from another game on the Tigrex's files, by reading the code that builds and draws a large monster and dumping its objects on a quest.

### The files

The first file of a model set is a pack: a u32 count, then an offset and a size for each entry (an entry of size 0 is absent); `pac.entry` (`0x088661C8`) returns entry *i*. `monster.build_model` (`0x0A08FDB0`, the monster in `a1`, its model set in `a2`) builds the monster from entry 1, the model (PMO, version `102`), entry 0, the skeleton, and entry 2, the textures (`.TMH0.14`); it keeps entry 3 (8 bytes) at the object's `+0xD08` and hands the pack to `0x0895B90C` for the rest. The second file holds the model's GE data when the model's GE base equals its size. The third holds the motions.

### Skeleton

The header is a u32 `0x80000000`, a u32 count of bones plus one, the size, a zero, the number of roots, and where the first bone starts, counted from `+0x0C`; one byte per root from `+0x1B` names the bone that starts each chain. The Tigrex has two roots, 0 and 43, its severed tail. A bone is 0x5C bytes: its index, parent, first child and next sibling (s32 from `+0x0C`), its scale, rotation and position (four floats each from `+0x1C`, `+0x2C` and `+0x3C`, the position from its parent) and an 8-byte name at `+0x54`. The builder makes a 640-byte joint for each bone (`joint.init`, `0x08881CA8`, copies the bone in), links the joints by those indices and walks the tree from each root; the model keeps the roots' joints from `+88`, their count (u16) at `+104` and the joint count at `+108`. A root byte naming a bone that does not exist made the walk read a joint that was never built.

### Model

From `+0x08` the header holds the size (counting the GE data in the next file), a float extent, the vertex scale on x, y and z, the counts of meshes and materials (u16), and the offsets of the meshes (48 bytes each), the vertex groups (16 bytes), an unused offset (0), the bone palette, the materials (16 bytes) and the GE base. `model.compile` (`0x0889E144`) copies each material's first two words and its texture byte (`+8`) into 16 bytes and each mesh's two words at `+32` into 8 bytes, materials first, into a buffer allocated just before the joints. A mesh's material count and first material, and its group count and first group, are the four u16 from `+40`. A vertex group gives its material within the mesh, its bone count, its first palette entry and the offsets of its GE list, vertices and indices from the GE base.

The palette is pairs of bytes, a bone matrix slot and a joint. For each group the draw (`model.draw_mesh`, `0x0889ED4C`) selects each slot (the GE's bone offset) and calls that joint's bone list: `model.bone_lists` (`0x0887AA94`) writes a 52-byte list per joint into the display list, in joint order, from the joint's skinning matrix at `+80`. No slot carries over from one group to the next: a group's palette sets every slot its vertices' weights use, as all 88 of the Tigrex's groups do.

### Drawing

The monster's draw in `game_task` (around `0x0A08A96C`) runs `model.skin_matrices` (`0x0887AA0C`: each joint's `+80` becomes its `+16` by its `+144`, from the roots) and `model.bone_lists` each frame it draws the monster; a monster out of view keeps the matrices of the last frame it was drawn. `model.draw_mesh` skips a group whose material has a first colour with zero alpha (`0x0889EEC8`) and gives the GE that alpha as the material's (`0x58`).

AI code hides parts this way. The Tigrex's code sets that alpha by material number (on its own model, materials 8 to 11, 16, 19 to 21 and 23 were hidden at one moment), so on a model with other materials it hides other parts. It also writes the 8-byte mesh entries by mesh number, up to 16: a model with fewer meshes takes those writes into its first joint, and the game stopped in `0x0887A91C` or `0x088819E0` on the joint's broken vtable.

### Motions

The third file starts with a list of streams, each a slot count and the offset of its table of slots, then a zero and where the data starts; a slot holds the offset of a block or -1. A block is a u32 count of bone records, its size, a loop flag and a float loop start; a record is a u16 channel count and its size; a channel is a u16 type, a u16 key count and its size, then the keys, four s16 each (value, frame, ease in, ease out). Types are location (`0x40`, `0x80`, `0x100`), rotation (`0x08`, `0x10`, `0x20`) and scale (`0x200`, `0x400`, `0x800`), in that order within a record. The Tigrex's file has a stream of 65 slots, one of 20 and an empty one, with 43 records per block for its 46 joints.

The game hands a block's records to the joints in the order it walks the tree from each root, first child before next sibling. On the Tigrex that is the joints' own order. On a skeleton numbered otherwise, records in index order turned the joints of the chest, wings and head (a joint's world matrix there was no longer a rotation), and sorting the records into walk order put them right.

## A monster's behaviour

A large monster's object starts with a vtable, and the game asks it for each part of its behaviour through one slot of it. Freedom Unite's monsters and Portable 3rd's have the same class: listed side by side, Fatalis's vtable in Freedom Unite and the Tigrex's (`0x0A1D9708` in `em058m0`) override the same slots and keep the common code at the same places; Portable 3rd's adds seven slots at the end (63 to 69, 70 in all).

| Slot | Part | How it was found |
| --- | --- | --- |
| 32 (`+0x80`) | Each frame's behaviour (THINK): decide and act | The monster update in `game_task` (`0x0A08B558`) calls it between its common steps, before slot 14 (`+0x38`); both monsters' functions there are their largest, a switch on the monster's state |
| 36 (`+0x90`) | The sounds and effects of the motion playing (EFFECTS) | The Tigrex's runs its tables by motion slot and its effects at frames by motion; with a mod's motions those are the wrong ones, so it is left out as ACT is |
| 33 (`+0x84`) | Then carry out the action in the monster's mode (ACT) | Called from `0x0A086254` in `game_task`; the common class's is a bare return, and every Portable 3rd monster gives its own, a switch on the mode at `+0x18C`. With the Tigrex's THINK left out, the Tigrex's ACT kept turning a Fatalis back to a facing its THINK had chosen before (found with a write watch on `+0x74`: `0x0A0B2944`, the turn step, called from the Tigrex's turn action `0x0A1BCA64`) |

What a part does goes through the game's own functions, which a mod reaches through the monsters service:

| Function | What it does | How it was found |
| --- | --- | --- |
| `0x0A0B3004` | play(monster, motion, blend frames, first frame): plays a motion of the monster's motions file, by slot | The Tigrex's behaviour calls it 78 times with constants that fall on its motion slots; logged in a fight, it is called once at each change of action (motions 3, 26, 9, 33, 56, 57...), sometimes with a first frame (86, 60) |
| `0x0A0AD0B0` | finished(monster): whether the motion has reached its last frame | Its code: the model's frame (`+4`) and step (`+12`) against its last frame (`+20`); no step for a looping motion (`+80` bit 2), a held one (`+40` 1 without `+64` bit 0) or `+72` above 0 |
| `0x0A0A4914` | at_frame(monster, frame in f12, channel): whether this frame's step passes a frame of the motion; through the model to `0x0887A118`, which compares the frame with the model's frame and frame plus step (no further than the last frame; past it, a looping motion (`+80` bit 0) wraps to its loop start, `+16`) | The Tigrex's behaviour calls it 26 times, through wrappers that start an attack or an effect at a frame |
| `0x0A0A7C74` | turn(monster, step): turns the facing (`+0x74`) towards the target (`+0x4C0`) by at most the step (65536 a turn); whether it faces it | Its code: the angle from `0x08878010` (an arc tangent of the x and z differences), then `0x0A0A7C20`, the shorter way round by at most the step. The Tigrex turns 384 a frame |
| `0x0A09E884` | start_attack(monster, slot, attack, flags): an attack object made from row `attack` of the kind's attack table (28 bytes a row, at `+0x60` of the kind's data at `+0xA4`), kept in slot `slot` (`+0x174` + 4 × slot) | The Tigrex calls it at a motion's frames with attacks 6 to 28 in slots 1 to 3; `0x0A09EA9C` is the same with no flags |
| `0x0A09B47C` | end_attack(monster, slot): calls slot 14 of the attack object in the slot | Its code |

A monster's model is what slot 17 (`+0x44`) of its vtable returns; for the Tigrex that is a leaf adding `0xE00` to the monster (the Barioth's adds `0xDD0`). The monsters service reads it that way and works the finished check out itself; a motion is queued, and Yakumo calls the game's setter through a guest call once the mod's function returns.

### A monster's body

Where a monster is and what it is after, as the monster update and the behaviours read it. Logged each frame with a script probe on a Fatalis in the arena, with its own behaviour playing one motion after another:

| Offset | What | Notes |
| --- | --- | --- |
| `+0x80`, `+0x84`, `+0x88` | Position: x, y (up), z | The previous frame's at `+0x90`. While THINK runs, y holds -20000: the game puts its ground height there later in the frame (`0x0A0A5518`, found with a write watch), so a behaviour reading the position sees -20000 for y; the same on the Tigrex's own behaviour and in the arena |
| `+0x74` | Facing, 65536 a turn: 0 towards +z, growing towards +x | A 32-bit word; the turn code keeps it to 16 bits. Turning motions change it (a motion's root rotation is applied); no motion moved the position |
| `+0x4C0`, `+0x4C4`, `+0x4C8` | Target point: what the monster goes for | Brought up to date by the game only now and then: without the base's THINK it followed the hunter once, then stayed where the hunter had been. `0x0A0AFEB0` fills it from the target's kind at `+0x4D8` (and `+0x4D9`, `+0x4DA`): a hunter, another monster, a point to the side, or by default the first hunter |
| `+0x18E` | The motion slot playing (u16) | Read by `0x0A0AD31C`; `0x0A0B2ED4` plays a motion only when this is another |
| `+0xD50` | The kind's data, cached | From `0x0A09B644`: by the kind (`+0x62`) and its variant (`+0x2E3`). Its `+8` is the kind's base size, `+0xA4` the attack and part tables |

A facing from one point to another is `atan2(dx, dz)`, and a step (x, z) in the monster's own frame becomes `(cos θ · x + sin θ · z, cos θ · z − sin θ · x)`: `0x08876D64` turns a vector that way. The monsters service reads and writes these itself (`position`, `facing`, `set_facing`, `target`, `move`, and the model's frame for `motion_frame`), works the turn out as `0x0A0A7C74` does, and the frame check as `0x0887A118` does; attacks, and `aim` (a call of `0x0A0AFEB0` with no point, which brings the target point up to date), are queued and made by the game's functions, like motions. A monster made from a base keeps the base's attack table: its attacks are the base's rows, with their joints numbered as in the base's skeleton.

### A monster's attacks

An attack is a row of the kind's attack table, made into an attack object by `0x0A09E884`. The table hangs off the kind's data, which a monster looks up once (`0x0A09B644`, by the kind's row in `monster.kinds`: a list per variant whose first word is the kind's data) and keeps at `+0xD50`. Dumped from a Tigrex-based monster in the arena:

| Where | What |
| --- | --- |
| Kind's data, `0xA8` bytes | `+8` base size (float), `+0x50` points, `+0xA4` the attack tables |
| Attack tables, `0x70` bytes | `+0x14`, `+0x18` part tables, `+0x5C` the shapes (a pointer each), `+0x60` the attack rows |
| Attack row, 28 bytes | `+2`, `+4` its strength (u16 each), `+6` its kind of hit, `+0x11` the shape it hits with. The Tigrex has 29 rows (0 unused) and 18 shapes |
| Shape | Spheres of 40 bytes, ended by joint `0xFFFF`: joint (u16; a second joint in the high half for a capsule), kind (0 sphere, 2 capsule, 1 a box), a flag, radius, centre x, y, z from the joint (floats) |

The shapes name the base's joints, so a monster made from a base hits with spheres on whichever of its own joints have those numbers: on Fatalis the Tigrex's head is a front leg. `attack_shape` gives a kind a mod added its own spheres for a shape: the first time one of its monsters thinks with a kind's data the mod has not changed, Yakumo copies the kind's data, its attack tables and the shape pointers, writes the mod's spheres, and points the monster's `+0xD50` at the copy (one per kind and base data, kept for the run). `attack_row` gives the kind a row of its own: the copy then has the base's rows too (64 of them, the base's and what follows), with the mod's row a copy of the base row it names and its shape byte (`+0x11`) the mod's. So an attack can hit with a shape of its own while doing what a base attack does; what the row's other fields mean is not traced, so they stay the base row's. Checked with the Fatalis mod: its breath, a copy of the Tigrex's row 7 hitting with a shape of its own (20), hit a hunter put in front of it from 300 to 1900 away. The Tigrex places its far-reaching shapes on its root joint, centres up to 600 ahead (+z); the same offsets on Fatalis's jaw joint reached no further in game, whichever axis they were on, so a far shape goes on the root.

### A monster's effects

A monster's effects (flames, sparks, dust that follow it) are entries 3 and on of its model pack, which `monster.build_model` hands to `0x0895B90C`: entry 5 is a model for them (a self-contained PMO), entry 6 its textures and entry 7 the effects. The effects entry is a u32 count and an offset per effect (an offset at the entry's end is none: the Tigrex's effect 0); an effect is its part count, header size and part offsets, then at the header's end a float and a count of 80-byte emitters. Each emitter starts with its bank: a type and, for type 1 (a monster's own), the kind (u8, 58 in all of the Tigrex's). When an emitter draws from a model, game_sub (`0x0A013878`) looks the model up by that kind's loaded model set (`0x08856504` over the four sets at `+0x90` of `*0x08A38644`, by the kind's first model file).

A kind made from a base kept the base's kind in its effects, so every effect that draws a model found no set and drew from a model with no PMO (`0x0889F2D0` reading `+4` of it): the Tigrex's effects 3, 4 and 10 stopped the game on a Fatalis, 1, 2, 7, 8 and 9 did not. Yakumo now rewrites those emitters' kind to the new kind's when it composes the pack; all 20 of the Tigrex's effects and the Rathalos's 27 then played on a Fatalis. A mod can take another kind's effects instead of the base's (`effects`), their entries 3 and on.

An effect starts with game_sub's `0x0A00D7D8`: start(bank, effect, monster, joint, offset, scale, ...), the bank of a monster's own effects being 12 plus its model set's slot. Logged on the Tigrex: bank 12, effect 7, joint 26, offset (0, -10, 90), scale 0.7, flags 2 on the stack. `0x0A00D648` is the same without the scale. The Tigrex's motion sounds and effects come from its slot 36, by motion slot (its own tables, through `0x0A01476C`), and `0x0A0AD45C` shakes the ground at a joint.

`game_task`'s maker (`0x0A090AE0`, make(manager, kind)) returns a new monster, with its kind at `+0x62` and its vtable at `+0`. The monsters service gives a kind a mod added a vtable of its own: a copy of the base's whose slot for each part the mod gives runs the mod's function first and then, unless it says it has done the part, the base's. The base's vtable is that of the AI overlay in the monster's slot, so the kind has a copy for each base vtable its monsters are made with, and makes one again when the base vtable at an address no longer matches its copy (another overlay in that slot).

### A monster's footsteps

A monster's effects part (vtable slot 36; the Tigrex's is `0x0A1C2810`) calls game_sub's `0x0A01476C` with seven tables of the kind's, the monster, and flags on the stack. By the monster's motion slot (`+0x18E`), the last table (a u16 per slot) names a row of the sixth (16 bytes: u16 starts into the five event tables, then a u8 count for each from byte 10), and the player passes each event of the motion playing to its handler, which acts when the motion passes the event's frame:

| Table | Event | What it does |
| --- | --- | --- |
| 1 to 3 | 60 bytes: u32 motion slot, f32 frame, u16 bank, u16 effect, u16 joint, u16 1, u16 3, a byte choosing how it is started (`+0x12`), ..., three scales, `0x20` | One of the three by the ground under the monster (`+0x130`): an effect (`0x0A01425C`), from the monster's own bank (12) or the common one (44). The Tigrex's: its effects 17 and 19 at its feet in its walks and turns, and the common effect 102 |
| 4 | 12 bytes: u32 motion slot, f32 frame, u16 joint, u16 size of step (0 to 9) | A footstep (`0x0A0144FC`): the dust and splash the ground and size call for, two effects from a table of 3 grounds by size (`0x08A21B68` for large monsters), at the joint, as large as the kind's float in `0x08A21A40` |
| 5 | 60 bytes, as 1 to 3 | Played whatever the ground |

The Tigrex's (dumped from memory on a quest): footsteps in 4, effects by ground in 1 to 3; no sound is in them, and where its steps' and roar's sounds come from is not traced. `yakumo.monsters`'s `footsteps` builds a kind's index, rows and table 4 (the others empty) and, after each frame its mod's THINK did alone, queues the player with them. With Fatalis's footsteps (from Freedom Unite's em02, where it strikes the ground), its feet raised dust at the frames given: probes on the effect starter (`0x0A00E39C`) saw effects 95, 104 and 107 (steps of size 2, 4 and 5) at its feet's joints. The size table has a float for each of kinds 0 to 73, and the game's own reader in game_sub has no bound, so it is the game table `monster.step_scales`: a kind added takes its base's. Tables 1 to 3 and 5 are not given yet.

## Not traced yet

Other code compares a kind with 75 and leaves a larger one out: a byte per kind at `0x08971650` (read at `0x0882A6F4` and `0x0882A898`), a jump table at `0x08968E40` (`0x08813A94`), a jump on a byte at `+397` of an object (`0x0A1112A4`), a flag per kind kept inside a larger structure (`0x0A0B6B0C`), and the checks at `0x0894D3C8`, `0x0894D4B8`. What each one is for, and so what a new kind lacks without it, is not known yet. Neither are:

- The kind's name, icon and the hunter's notes on it: the developer tools show a new kind as "Monster 75".
- Its rewards, its parts and their damage, and its entries in the save's hunting records.
- Small monsters, which share the kind numbers.

## A monster's body capsules

Where hunters' hits land: the kind's data a monster keeps (at `+0xD50`) points twice, at `+0x1C` and `+0x20`, at a list of capsules, 44 bytes each: u16 joint, u16 second joint, u16 shape (0 a sphere at the first joint, 2 a capsule between the two), u16 body part, u32 hit zone word, a word, f32 radius, then the two ends (x, y, z each, in their joints' frames). The list ends with 0xFFFF in each u16 and in the zone word. Read from the Tigrex's (30 capsules: head, neck, belly, back, tail, legs, arms, a sphere round the whole body).

A kind made from another starts with the base's capsules, on the base's joints. On Fatalis's skeleton the Tigrex's joint numbers are other places, and no hit landed: a lance's thrusts took nothing off it while the same run on a Tigrex did. `yakumo.monsters`'s `body_shapes` gives a kind its own capsules: its monsters keep a copy of the kind's data that lists them (with any attack shapes the mod gave). With its own, Fatalis took the hits.
