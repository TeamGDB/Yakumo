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

## Not traced yet

Other code compares a kind with 75 and leaves a larger one out: a byte per kind at `0x08971650` (read at `0x0882A6F4` and `0x0882A898`), a jump table at `0x08968E40` (`0x08813A94`), a jump on a byte at `+397` of an object (`0x0A1112A4`), a flag per kind kept inside a larger structure (`0x0A0B6B0C`), and the checks at `0x0894D3C8`, `0x0894D4B8`. What each one is for, and so what a new kind lacks without it, is not known yet. Neither are:

- The kind's name, icon and the hunter's notes on it: the developer tools show a new kind as "Monster 75".
- Its rewards, its parts and their damage, and its entries in the save's hunting records.
- Small monsters, which share the kind numbers.
