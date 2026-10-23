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

## Areas mods add

A native mod adds an area with `yakumo.maps` (`mods/mhp3rd_maps.cpp`, [MODDING.md](MODDING.md#maps)): its model, GE data, textures and two collision blocks (ground, walls), in place of one of the game's areas, and names it in its quest (`ymod_quest.area`). While a quest that names it is played (the accepted quest's id, at the character's address plus `0x60472`, which the game's own collision choice reads), the files the game loads for the base area are served with the mod's in them, through the loader's size lookup and submit that files mods add go through ([DATA_BIN.md](DATA_BIN.md)):

- the area's pack, the game's with entry 0 and entry 3 the mod's model and textures; its scene, camera paths and settings stay the game's;
- the area's GE data, the mod's;
- the map's collision pack, the game's with the area's two blocks the mod's ground and walls.

On any other quest the game's files load as they are. An area whose GE data is larger than the base area's is not served at all: the game copies an area's GE data into video memory (the arena's to `0x04136000`) with the large monsters' right after it (`0x041AA600`), and a castle built 50 KB larger than the arena's ran into Fatalis's and hung the quest's loading. Frame interpolation finds the camera's motion from the draws that stand still, each counted by its vertices: an area built as a few dozen large draws, fewer than the hunter's and the monster's, would otherwise be outvoted by them, and every frame would be a cut. A mod's model fills the base area's meshes: the mod's sky in meshes 0 and 1 and the rest in mesh 6, with the meshes the scene names left empty, so the scene's nodes draw it.

Checked on macOS (2026-10-21) with the Fatalis mod's Castle Schrade, from Freedom Unite, in place of the arena's area 1: on the mod's quest the castle drew with its walls, gate tower, paved floor and sky, the hunter stood on its collision, and Fatalis fought in it; the arena quests that did not name it loaded the arena.

## Not traced yet

- Entry 2's per-material settings, entries 5 to 9 of the area's pack, and the minimap (the collision pack's textures).
- What the surface and flags words' other values mean (materials; the walls' `0x3F` and `0x400`, perhaps the monsters'), and how monsters are kept in.
- How the scene's nodes move their meshes (the arena's sky and flags), and the camera paths' use.
- Adding a map of a mod's own rather than taking an area's place, and an area in multiplayer.
