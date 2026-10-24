# Maps

How the game keeps an area (what it draws, what the hunter stands on), how it finds an area's files, and how a mod's area takes the place of one of the game's on the mod's quests. Traced on NPJB-40001 with the arena's fighting area (map 8, area 1); file ids are in hex.

## Finding an area's files

A quest's map is the byte at `+0x9A30` of the map state and its area the byte after it. `game_sub` keeps three tables of 20 u16 file ids, one row per map:

| Table | Row for the map, plus | File |
| --- | --- | --- |
| `0x0A0457C4` | 2 × area | The area's pack |
| `0x0A04579C` | 2 × area | The area's GE data |
| `0x0A045774` | area | The area's code overlay (`P_m08a01d` for the arena's area 1) |

The executable's `0x088CE5BC`, `0x088CE5F4` and `0x088CE62C` read them. The arena's rows are 1389, 138A and 12EA, so its fighting area is the pack 138B, the GE data 138C and the overlay 12EB. The areas' packs and GE data alternate from file 130D, map by map.

The map's collision is one pack per map, file 125A + the map (`game_task` at `0x0A06D3E4`; the event quests 20101 to 20108 take others, `0x0A065388`). Its entry 0 holds textures; then come two blocks per area: area *a*'s ground is entry 1 + 2*a* and its walls entry 2 + 2*a*. Found on map 1, whose 26 blocks pair with its 13 areas (its two empty areas' blocks are copies of the first area's), and checked on the arena: the hunter in area 1 stood on block 3 when a mod's collision was put there. The arena's block 3 holds only ground (each triangle's material in its surface word), block 4 its walls, ceilings and steep slopes (surface `0x20`, flags `0x3F`, `0x400` or `0x411`). The walls the hunter runs into carry `0x411`: measured with a probe logging the hunter's position while the stick was held each way, the arena's wall triangles written again with `0x3F` (or `0x43F`) for all let it through, and with `0x411` or their own flags stopped it. A mod's block of ground and walls together in both let it through too. The camera goes through walls of `0x411` and keeps out of the arena's rock walls, `0x3F`, which the hunter goes through (the arena's fence of `0x411` is about 1500 high, its rock walls reach their tops): a mod's wall that stops both is written twice, once with each. The camera also keeps above the ground beneath it, so ground no one stands on, such as a tower's roof, belongs in the walls block: with a roof 4545 high among a mod's ground, the camera jumped onto it when it came into the roof's outline (measured with a probe logging the camera's eye every frame, on the Fatalis mod's castle).

## The area's pack

| Entry | What |
| --- | --- |
| 0 | The model: a PMO "102" (docs/MONSTERS.md's layout) whose GE data is the next file, as a monster's is: its size field is the model's size plus the GE data's, and its GE offset the model's size. Static: vertex groups with no bones, no palette, no remap |
| 1 | The scene: nodes of 0x5C bytes, each named in its last 8 bytes, the mesh it draws in the word before the name. The arena's: `MODEL_00` (meshes 0 to 5, 0 and 1 the sky), `NULL01_` (meshes 6 to 37, the ground, `P000_JIM` first) and more groups to mesh 97 |
| 2 | Per-material settings, not traced |
| 3 | The textures (TMH) |
| 5 to 8 | `NSP`, `ASB`, `ARV` blocks and five bytes, not traced |
| 9 | Camera and fog numbers, not traced |
| 13, 14 | Camera paths: keys of 48 bytes (frame, field of view, eye, target) |

How the arena's model draws, which a mod's must follow:

- **Unlit.** Its vertices carry texture coordinates, a colour and a position, no normal, so their colour lights them.
- **Texture coordinates.** 16-bit, which the GE reads as 0 to 2; each mesh scales and offsets them (the two floats after its three scales, then two more).
- **Winding.** FFACE 0.
- **Small triangles near the camera.** The GE drops a triangle that reaches behind the camera, so ground near the hunter is cut into small triangles; a floor of a few large ones does not show.

## Collision blocks

Magic `4.00`:

| Offset | What |
| --- | --- |
| `+0x00` | `4.00`, the block's size, the box's minimum and maximum (x, y, z floats) |
| `+0x20` | Cell width 510, height 20000 and depth 510; cells across x, y and z (y always 1 in the game's); the triangle count; the cell table's offset (`0x44`); the triangles' offset |
| `+0x44` | Per cell, z fastest then x, the offset of its list: offsets of its triangles from the first triangle, ended by `0xFFFFFFFF` |
| triangles | 60 bytes each: a surface word, a flags word, three vertices (floats), the normal, and *d* (normal · p + *d* = 0) |

The normal is cross(v1 − v0, v2 − v0), normalised. The surface word is `0x20` for anything solid with the ground's material in its top byte (the arena's floor mostly `0x22000020`); walls carry `0x3F` in their flags.

## Where the hunter comes in and goes out

After the blocks, the map's collision pack holds a points table for each area: area *a*'s is entry 1 + 2 × *areas* + *a*, where the map has *areas* areas (a byte per map in `game_sub` at `0x0A0457EC`, which the executable's `0x088CE544` reads; the collision packs of maps 1 to 19 all have 1 + 3 × *areas* entries, the arena's 7). When the map loads, `0x088D0228` reads every area's table (`0x088D0118`) into the map state, so an exit can find its way into an area not loaded yet.

| Offset | What |
| --- | --- |
| `+0` | The number of groups |
| `+4` + 12 × *g* | A group: its kind, its count and the offset of its records from the table's start (the game's start at the next 16 bytes after the groups) |
| records | 32 bytes: x, y, z and 1.0 (floats); two sizes (floats); the facing (u16, 65536 to a turn); the record's index in its group (u16); a u16 and two bytes that each kind reads its own way |

| Kind | What | The u16 at `+28` | Byte `+30` |
| --- | --- | --- | --- |
| 0 | A way out: an upright cylinder, the sizes its radius and height. `0x088D07CC` takes the hunter out when y − h/2 ≤ his y < y + h/2 and dx² + dz² ≤ r² (the constants −0.5 and 0.5 at `0x0896D058`) | Its door | The area it leads to |
| 1 | A way in: where the hunter appears, and his facing | The door he came through | The area he comes from; `0xFF` the quest's start |
| 2, 3 | Something used with the action button, an upright cylinder the same way (`game_task`'s `0x0A175C2C`, by the distance's square root); made into objects by `game_task`'s `0x0A1753C8`, for every area of the map as the quest loads. Byte `+30` with `+31` is a u16, its kind: 5 the supply box (its menu opened), 7 the bed (the hunter is moved to its middle, turned to its facing, and lies down), 6 the delivery box (every field camp has a 5, a 6 and a 7, the arena's camp no 6; not tried); 2 a gathering point (the "?" prompt; 0, 3 and 15 are gathering points too, with other prompts); 9 a ballista, 10 a cannon, 11 the dragonator's button and 12 the gong (*Gathering, ballistas and cannons*, below); 1, 8 and 18 not traced | A gathering point's list in the quest, from 1; a ballista's or a cannon's number among the area's of its kind | |
| 4 | A camp's cart: one record, its place and facing (`0x088CE568` keeps it at `+0x9ACC` of the map state). A hunter who fainted comes back there: two Felynes carry him on a stretcher along its facing, about 1200 units unless a wall stops them, and set him down | | |

Byte `+31` of an exit or an arrival is its index again; every exit and arrival on the disc has door 0 but fourteen (in maps 2, 5, 12, 15, 18 and 19: doors 1 and 256 to 258). Leaving through an exit, `0x088D033C` looks in the destination's arrivals for the one whose area is the area left and whose door is the exit's (door 257 takes another number from `0x088CF130`); with none, the destination's quest start (`0x088CFEB4` with `0xFF`), and with neither a default in `game_sub` at `0x0A045800`. A quest starts at the quest start of the area it loads first (`game_task`'s `0x0A06DA44`, through `0x088CFFA8`); with bit 2 of word `+0xA4` of `game_task`'s object set, at the cart's place instead (`0x0A06D614`, through `0x088CE6DC`), as after fainting. In the camp, `game_task` puts an object of type 65 at the cart's place (`0x0A111AEC`).

No exit on the disc leads into its own area, and every cart is a camp's (area 0's; maps 1 and 14 repeat theirs in areas 7 and 8, whose tables are copies of the camp's): the map state keeps one cart, the last area's that has one. The arena's camp has one exit, a cylinder of radius 4200 to the floor, its quest start at (284, 0, −909) facing `0x7E94`, a supply box of radius 150 at (846, 0, −1900), a bed of radius 130 and height 300, and its cart at (1016, 0, 699); its floor has two arrivals, (0, 0, 3300) from the camp and (0, 0, 330) for a quest that starts there, and four gathering points, but no exit. The facing: an arrival of `0x8000` faces the hunter to −z and one of `0x4000` to +x (measured with a mod's arrivals, a probe logging the hunter's position as he walked on from each; his facing is the u16 at `+0x74` of his object, `0x096498D0`, and was the arrival's on coming in).

### Gathering, ballistas and cannons

Only two of the disc's maps have ballistas and cannons: the sand ship of the Great Desert (map 6, areas 1 and 2: two cannons and two ballistas on area 1's deck, a cannon and two ballistas on area 2's, the gong and the dragonator's button on both) and the Sacred Peak (map 13, area 1: three ballistas). They are points of the area (read from every map's points tables), nothing more: what the hunter sees there is the area's model, and the game's code does the rest, the same in any area but for the dragonator.

- **A gathering point** gives what the quest's list for it holds: the payload's header `+0x60` points to an offset per area, each to that area's entries of 8 bytes (an offset to the items, the most and the fewest uses), and a point takes entry *word* − 1 (`0x0A175684`): its uses as the quest starts, the fewest and, for each one more up to the most, another chance (the hunter's skills change it), and its items (u8 chance, u8 count, u16 item, ended by `0xFFFFFFFF`). An entry whose items are 0 makes the point unusable. Every list on the disc's 379 quests has chances adding up to 100, up to 10 items, and its area's points' words reach 6. Quest 615 (*Rumble in the Great Desert*) gives area 1's two ship points `Cannon Ammo` (712) ten times and `Ballista Ammo` (683) twenty times.
- **A cannon** (kind 10) fires the cannonball the hunter carries, item 712, which a gathering point gives and which he holds in both hands like an egg (the carried items are 712 to 718, `0x0887EEDC`; it breaks when he is hit: *Cannon Ammo broke!*). Pressing the action button in the cannon's cylinder plays his loading motion, takes the cannonball (`0x0A11B1B8`) and makes a shot (`0x0A151B84` with 3, `0x0A1500F0`) at the point plus 120 up and 280 ahead along the point's facing (constants at `0x0A1AFFF0`), flying along that facing (0 towards +z, `0x4000` towards +x) in an arc, a little different each shot. Nothing of it reads the map.
- **A ballista** (kind 9) fires the ammo the hunter has (683, the binder 684, 685; `0x0A120888`) when he presses the action button in its cylinder, from the point, aimed the way he faces (`0x0A1516A4`). On map 6 its shot is of kind 2, elsewhere of kind 4. Whether each ballista is manned is kept quest-wide, in three bytes of the quest's state (`+8677` of `*0x09FBE874`), indexed by the ballista's word (`0x088C0D1C`, `0x088C0D40`, `0x088C0D64`; `0x088C0D78` and `0x088C0DA4` walk exactly three): the sand ship's two areas share words 0 and 1 for their ballistas. A word of 3 to 14 lands on the dragonator's and the gong's waits, which read as manned, and on leaving a ballista of word 15 `0x0A120058` frees slot `0xFFFF`, a byte some 64 KB away. A cannon's word only names it among its area's to the other hunters (`0x0A0C5434` finds it by area and word); cannons have no slots.
- **Conditional words.** For every point, `game_task`'s `0x0A1755D4` looks its map, area and word up in six rows of `game_sub` at `0x0A044A58` (through `0x088CF1B0`): map 3 and 16, area 4, word 5; map 12 and 19, area 4, word 2; map 12 and 19, area 5, word 3. A point that matches is usable only while a flag of its area is set.
- **The dragonator's button** (11) and **the gong** (12) start a 10-minute and a 3-minute wait (`0x088C2764`, `0x088C0EF0`). The gong's shot starts at the hunter, but the dragonator's spear has its place written in the code for the ship's two areas only (`0x0A151320`, map 6, area 1 or 2) and is fired nowhere else; a mod's area does not offer them.

## Areas mods add

A native mod adds an area with `yakumo.maps` (`mods/mhp3rd_maps.cpp`, [MODDING.md](MODDING.md#maps)): its model, GE data, textures and two collision blocks (ground, walls), in place of one of the game's areas, and, if it gives them, its points: exits, arrivals, the camp's supply box, delivery box and bed, its cart, gathering points (with what each gives), ballistas and cannons. Its quests name it (`ymod_quest.area`, or several areas of one map in `ymod_quest.areas`, each in place of another of the map's areas). While a quest that names areas is played (the accepted quest's id, at the character's address plus `0x60472`, which the game's own collision choice reads), the files the game loads for the base areas are served with the mod's in them, through the loader's size lookup and submit that files mods add go through ([DATA_BIN.md](DATA_BIN.md)):

- each area's pack, the game's with entry 0 and entry 3 the mod's model and textures; its scene, camera paths and settings stay the game's;
- each area's GE data, the mod's;
- the map's collision pack, the game's with each area's two blocks the mod's ground and walls, and the points table of each area that gives points written from them (*Where the hunter comes in and goes out*, above). An area that gives none keeps the game's table: its start, ways in and out, camp things and gathering points in the game's places. A gathering point's word is its list, from 1 in the order the area gives them; a cannon's its number among the area's, from 0; a ballista's its slot, numbered across the quest's areas in the order the quest names them. None is numbered with its area's conditional words (a gathering list there is left empty). So a quest's areas may have three ballistas in all (two in place of map 12's or 19's area 4), and an area or a quest with more is refused; an area takes the place of one of a map's first 13.

The quests that name such an area are built with its gathering points' lists, in the area's place in the quest's gathering table (header `+0x60`, [QUESTS.md](QUESTS.md)). Their items are named as the game names them, so a quest is first built with the points' uses and no items, and built again with them once the game's text is loaded, as its rewards are. The other areas of the map have entries with no items: their gathering points cannot be used on a mod's quest, where the game would otherwise read the table's other words as entries.

On any other quest the game's files load as they are. None of a quest's areas is served when one of them, or an area its exits lead to or its arrivals come from, is past the map's areas (`0x0A0457EC`), or when the map's collision pack lacks a block pair and a table for each of the map's areas: an area past them would have its blocks written over the points tables, and the game would read a ground block's `4.00` as a table's group count and run off the pack. A mod's exit may not lead into its own area, and only a camp (area 0) may give a cart, so a quest has at most one. An area whose GE data is larger than the base area's is not served at all, nor the other areas of its quests (an exit of one leads into the others): the game copies an area's GE data into video memory (the arena's to `0x04136000`) with the large monsters' right after it (`0x041AA600`), and a castle built 50 KB larger than the arena's ran into Fatalis's and hung the quest's loading. Frame interpolation finds the camera's motion from the draws that stand still, each counted by its vertices: an area built as a few dozen large draws, fewer than the hunter's and the monster's, would otherwise be outvoted by them, and every frame would be a cut. A mod's model fills the base area's meshes, the ones its scene names (the word 8 bytes before each node's name): the arena floor's 98, the sky in meshes 0 and 1 and the ground's first part mesh 6; the arena camp's 100, the sky the same and the ground's first mesh 8. The mod's sky goes in the sky's meshes and the rest in the ground's first, with the other meshes the scene names left empty, so the scene's nodes draw it.

Checked on macOS (2026-10-21) with the Fatalis mod's Castle Schrade, from Freedom Unite, in place of the arena's area 1: on the mod's quest the castle drew with its walls, gate tower, paved floor and sky, the hunter stood on its collision, and Fatalis fought in it; the arena quests that did not name it loaded the arena.

Checked on macOS (2026-10-24) with the same castle giving two cannons, two piles of cannonballs (gathering points of *Cannon Ammo*, ten uses) and two ballistas, a probe moving the hunter and holding Fatalis in the line of fire, the action button pressed by the input script: the points were made as given (their words 0 and 1 for each kind, 1 and 2 for the piles, the piles' items in the quest built again with the game's names); each pile gave a cannonball (*Cannon Ammo obtained.*); each cannon fired it and hit Fatalis (161 health a shot, from both walls, the octagon's at 1600 and 2100 away; at 1100 and 2600 the shot passed it); both ballistas fired with the action button while the hunter had ballista ammo, the south yard's hitting Fatalis (38 a shot). The quest still started in the mod's camp, and its north door led onto the castle's walkway. Not checked: a ballista's ammo being used up (with ammo put in the pouch by the probe, its count stayed the same: the game skips that when its `0x088BFA2C` test passes, which reads a word of a state object not traced), a gathering point's extra uses, multiplayer.

Checked on macOS (2026-10-24) with the Fatalis mod's camp, Freedom Unite's gate tower room, in place of the arena's camp and its castle in place of the floor, both with their points, on one quest (`quest.area` and `quest.areas`), a probe logging the map state's area and the hunter's position and facing: the tables loaded as written; the quest started at the camp's quest start (5800, 4, 6400) facing `0x8000`; walking out of the camp's north door loaded the castle with the hunter at the arrival from the camp through door 0, on the walkway (−3369, 347, −1661) facing `0x8000`, and walking on dropped him into the courtyard; the camp's south door brought him to door 1's arrival, (−1919, 502, 539) facing `0x4000`, and each of the castle's doors back to the camp's arrival from the castle with its door. The supply box's menu opened and the hunter slept in the bed, in the room's own model; walls held him in the camp and on the castle's walkways. Fainting in the castle brought him back to the camp's cart. Quest 616, in the arena and naming no areas, loaded the arena's camp as before (the hunter at (284, 7, −909) facing `0x7E94`, its own supply box and bed).

## Not traced yet

- Entry 2's per-material settings, entries 5 to 9 of the area's pack (entry 5 ends with what look like point lights, x, y, z and a radius, by the camp's fire; entry 6 holds two places), and the minimap (the collision pack's textures).
- The things used with the action button of kinds 1, 8 to 12 and 18.
- What the surface and flags words' other values mean (materials; the walls' `0x3F` and `0x400`, perhaps the monsters'), and how monsters are kept in.
- How the scene's nodes move their meshes (the arena's sky and flags), and the camera paths' use.
- Adding a map of a mod's own rather than taking an area's place, and an area in multiplayer.
