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
