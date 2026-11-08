# Terrain

How terrain authoring behaves for whoever depends on its results. The design and its reasons are in
`docs/specs/T1-terrain-authoring.md`.

## Generated heightmaps

`set_terrain_maps` generates through the engine's `fill_noise` operation. `flat`, `middle` and
`fractal_noise` write the bytes they always wrote. `random_noise` hashes each texel with `seed`: the
same seed gives the same map on every platform, but not the map an earlier version made, which
depended on the standard library's random distribution and was never portable.

## When a map reaches its file

An edit changes the map in memory. A second after the last edit, with no stroke in progress, the map is
encoded and written; scene save, Auto Save, Play, export and closing the project write it at once. A
crash loses at most that second and the encode. A failed write keeps the scene modified and is tried
again at the next save.

## Edits from agents, scripts and the window

`edit_terrain`, Lua's `Terrain:applyOps` and the Terrain window's Import Raw..., Keep World Heights and
Paint by Rules run the same operations, read from the same op format (`engine/core/object/TerrainOpsJson.h`).
In the editor a call is one undo step: maps the ops make come first, then one delta of the changed tiles,
then a hole map the last fill removed. A game's edit has no undo, and its first write moves the
terrain's maps under ids of their own, so the project's files and other terrains using them keep what
was authored.

## What follows an edit

- The CPU map and every surface query see the edit at once.
- Colliders take it before the next physics step: the edited block of the heightfield is written in place,
  or the shape is built again when the edit covers most of it or leaves its height range.
- The drawn image goes up whole, at most once a frame, and a map bigger than the frame's budget (2 MB on
  mobile and web, 8 MB on desktop) every few frames. The last edit always goes up.
- Foliage and conforming splines follow at most four times a second during a stroke or a game's edits.
- A NavMesh is not baked again: `sample_terrain`'s issues report `navmesh_stale` until the next bake.

## Materials

A terrain with no layer draws its base material alone: its Quality tier, Limit Layers Per Pixel and the
material features have no layer to act on, and Macro Variation applies once the terrain has a layer
(the Inspector and `sample_terrain`'s issues, `macro_without_layers`, say so). A terrain whose layers
all fit under the cap (High keeps four) draws without the cap, the same image.
