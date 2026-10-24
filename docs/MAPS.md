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
| 2, 3 | Something used with the action button, an upright cylinder the same way (`game_task`'s `0x0A175C2C`, by the distance's square root); made into objects by `game_task`'s `0x0A1753C8`. Byte `+30` with `+31` is a u16, its kind: 5 the supply box (its menu opened), 7 the bed (the hunter is moved to its middle, turned to its facing, and lies down), 6 the delivery box (every field camp has a 5, a 6 and a 7, the arena's camp no 6; not tried); 0, 2, 3 and 15 gathering points, the u16 at `+28` their row in the quest's gathering table; 1, 8 to 12 and 18 not traced | | |
| 4 | A camp's cart: one record, its place and facing (`0x088CE568` keeps it at `+0x9ACC` of the map state). A hunter who fainted comes back there: two Felynes carry him on a stretcher along its facing, about 1200 units unless a wall stops them, and set him down | | |

Byte `+31` of an exit or an arrival is its index again; every exit and arrival on the disc has door 0 but fourteen (in maps 2, 5, 12, 15, 18 and 19: doors 1 and 256 to 258). Leaving through an exit, `0x088D033C` looks in the destination's arrivals for the one whose area is the area left and whose door is the exit's (door 257 takes another number from `0x088CF130`); with none, the destination's quest start (`0x088CFEB4` with `0xFF`), and with neither a default in `game_sub` at `0x0A045800`. A quest starts at the quest start of the area it loads first (`game_task`'s `0x0A06DA44`, through `0x088CFFA8`); with bit 2 of word `+0xA4` of `game_task`'s object set, at the cart's place instead (`0x0A06D614`, through `0x088CE6DC`), as after fainting. In the camp, `game_task` puts an object of type 65 at the cart's place (`0x0A111AEC`).

No exit on the disc leads into its own area, and every cart is a camp's (area 0's; maps 1 and 14 repeat theirs in areas 7 and 8, whose tables are copies of the camp's): the map state keeps one cart, the last area's that has one. The arena's camp has one exit, a cylinder of radius 4200 to the floor, its quest start at (284, 0, −909) facing `0x7E94`, a supply box of radius 150 at (846, 0, −1900), a bed of radius 130 and height 300, and its cart at (1016, 0, 699); its floor has two arrivals, (0, 0, 3300) from the camp and (0, 0, 330) for a quest that starts there, and four gathering points, but no exit. The facing: an arrival of `0x8000` faces the hunter to −z and one of `0x4000` to +x (measured with a mod's arrivals, a probe logging the hunter's position as he walked on from each; his facing is the u16 at `+0x74` of his object, `0x096498D0`, and was the arrival's on coming in).

## Areas mods add

A native mod adds an area with `yakumo.maps` (`mods/mhp3rd_maps.cpp`, [MODDING.md](MODDING.md#maps)): its model, GE data, textures and two collision blocks (ground, walls), in place of one of the game's areas, and, if it gives them, its points: exits, arrivals, the camp's supply box, delivery box and bed, and its cart. Its quests name it (`ymod_quest.area`, or several areas of one map in `ymod_quest.areas`, each in place of another of the map's areas). While a quest that names areas is played (the accepted quest's id, at the character's address plus `0x60472`, which the game's own collision choice reads), the files the game loads for the base areas are served with the mod's in them, through the loader's size lookup and submit that files mods add go through ([DATA_BIN.md](DATA_BIN.md)):

- each area's pack, the game's with entry 0 and entry 3 the mod's model and textures; its scene, camera paths and settings stay the game's;
- each area's GE data, the mod's;
- the map's collision pack, the game's with each area's two blocks the mod's ground and walls, and the points table of each area that gives points written from them (*Where the hunter comes in and goes out*, above). An area that gives none keeps the game's table: its start, ways in and out and camp things in the game's places.

On any other quest the game's files load as they are. None of a quest's areas is served when one of them, or an area its exits lead to or its arrivals come from, is past the map's areas (`0x0A0457EC`), or when the map's collision pack lacks a block pair and a table for each of the map's areas: an area past them would have its blocks written over the points tables, and the game would read a ground block's `4.00` as a table's group count and run off the pack. A mod's exit may not lead into its own area, and only a camp (area 0) may give a cart, so a quest has at most one. An area whose GE data is larger than the base area's is not served at all, nor the other areas of its quests (an exit of one leads into the others): the game copies an area's GE data into video memory (the arena's to `0x04136000`) with the large monsters' right after it (`0x041AA600`), and a castle built 50 KB larger than the arena's ran into Fatalis's and hung the quest's loading. Frame interpolation finds the camera's motion from the draws that stand still, each counted by its vertices: an area built as a few dozen large draws, fewer than the hunter's and the monster's, would otherwise be outvoted by them, and every frame would be a cut. A mod's model fills the base area's meshes, the ones its scene names (the word 8 bytes before each node's name): the arena floor's 98, the sky in meshes 0 and 1 and the ground's first part mesh 6; the arena camp's 100, the sky the same and the ground's first mesh 8. The mod's sky goes in the sky's meshes and the rest in the ground's first, with the other meshes the scene names left empty, so the scene's nodes draw it.

Checked on macOS (2026-10-21) with the Fatalis mod's Castle Schrade, from Freedom Unite, in place of the arena's area 1: on the mod's quest the castle drew with its walls, gate tower, paved floor and sky, the hunter stood on its collision, and Fatalis fought in it; the arena quests that did not name it loaded the arena.

Checked on macOS (2026-10-24) with the Fatalis mod's camp, Freedom Unite's gate tower room, in place of the arena's camp and its castle in place of the floor, both with their points, on one quest (`quest.area` and `quest.areas`), a probe logging the map state's area and the hunter's position and facing: the tables loaded as written; the quest started at the camp's quest start (5800, 4, 6400) facing `0x8000`; walking out of the camp's north door loaded the castle with the hunter at the arrival from the camp through door 0, on the walkway (−3369, 347, −1661) facing `0x8000`, and walking on dropped him into the courtyard; the camp's south door brought him to door 1's arrival, (−1919, 502, 539) facing `0x4000`, and each of the castle's doors back to the camp's arrival from the castle with its door. The supply box's menu opened and the hunter slept in the bed, in the room's own model; walls held him in the camp and on the castle's walkways. Fainting in the castle brought him back to the camp's cart. Quest 616, in the arena and naming no areas, loaded the arena's camp as before (the hunter at (284, 7, −909) facing `0x7E94`, its own supply box and bed).

## Not traced yet

- Entry 2's per-material settings, entries 5 to 9 of the area's pack (entry 5 ends with what look like point lights, x, y, z and a radius, by the camp's fire; entry 6 holds two places), and the minimap (the collision pack's textures).
- The things used with the action button of kinds 1, 8 to 12 and 18.
- What the surface and flags words' other values mean (materials; the walls' `0x3F` and `0x400`, perhaps the monsters'), and how monsters are kept in.
- How the scene's nodes move their meshes (the arena's sky and flags), and the camera paths' use.
- Adding a map of a mod's own rather than taking an area's place, and an area in multiplayer.
