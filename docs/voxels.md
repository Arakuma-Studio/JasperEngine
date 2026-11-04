# Voxel Volumes

A Voxel Volume is a bounded box of smooth, editable ground. It holds what a Terrain's heightmap cannot:
caves, tunnels, overhangs, arches, and ground a game digs while it runs. It meshes itself, keeps its own
level of detail and collision, works on every graphics backend and export target, and costs nothing in a
scene without one. A plain hill belongs on a Terrain.

## Making one

**GameObject > 3D Object > Voxel Volume** asks for a size, a Voxel Size, a base and a seed, with an estimate
of the memory it will take. The data lives in a `.voxelvolume` asset next to the scene, saved with it.
Sizes are in voxels, multiples of 16; Voxel Size is metres per voxel. Its Transform takes position and
rotation; its scale stays 1 ("Apply to Voxel Size" converts one).

| Base | Gives |
|---|---|
| Empty, Solid Ground | nothing, or ground to a height |
| From Terrain | the Terrain's surface, ready to dig into |
| Caves, Overhangs, Cave Entrance | presets, by seed |
| Noise Graph, From Mesh | your own graph or a voxelized model |

## Editing

The **Voxel Volume** panel (the Inspector's Edit button) has the brushes: Dig, Add, Smooth and Paint with a
sphere, box, capsule, mesh or stamp. One stroke is one undo step. **Regenerate** gives the volume a new base,
and keeps what you edited unless you say otherwise. The **Slice** view draws one plane through it, and the
Scene view's overlays show its wireframe, levels of detail, blocks, collision and terrain holes.

## On a Terrain

Set **Terrain** to attach a volume: it then has Open bounds, Terrain Cut Automatic and one slot following each
Terrain Layer. Its surface starts flush with the Terrain's, and digging through it cuts holes in the Terrain
exactly where the two stop meeting, undone with the same step. Leave three blocks (48 voxels) of untouched
surface around a cave. A warning about a narrow entrance means the hole map cannot follow the mouth: make it
wider.

## Budgets

| Target | Max dimension | CPU memory, all volumes | Mesh memory | Auto quality |
|---|---|---|---|---|
| Windows, Linux, macOS | 1024 | 256 MiB | 128 MiB | High |
| Android, iOS | 512 | 64 MiB | 32 MiB | Low |
| HTML5 | 256 | 32 MiB | 24 MiB | Low |

**Project Settings > Voxels** can lower them. An export fails naming the volume and the target when a dimension
is over its limit. At runtime an edit that would exceed the memory budget is refused and changes nothing.

## Lua

Play edits a copy: nothing a game digs reaches the asset. Everything is in world space, metres.

```lua
local vol = VoxelVolume.fromGameObject(go)
local r = vol:dig(VoxelShape.sphere(center, 3), {blend = 0.5})   -- r.ok, r.error, r.voxelsChanged, r.bounds
vol:fill(VoxelShape.capsule(a, b, 1), {material = 2})
vol:smooth(VoxelShape.box(center, halfExtents, rotation), {amount = 0.5, iterations = 2})
vol:paint(VoxelShape.sphere(center, 2), {material = 3, weight = 1, falloff = 0.5})
```

A call applies at once, or returns `r.ok == false` with `r.error` and changes nothing: `out_of_bounds`,
`invalid_shape`, `invalid_material`, `missing_asset`, `budget_exceeded`, `not_loaded`. The values are absolute,
not rates. One call has a budget (High 2 M samples, Medium 1 M, Low 256 k), so a big explosion is several calls.

Other shapes:

```lua
local fromMesh = VoxelShape.mesh(go)
local stamp = VoxelShape.stamp("voxels/boulder.voxelvolume", position, rotation, 1.5)
vol:fill(stamp, {material = 1})
```

Queries read the same data the edits write, at once:

```lua
local hit = vol:raycast(origin, dir, 100)         -- hit.hit, hit.point, hit.normal, hit.distance, hit.slot
local d = vol:getValueAt(p)                       -- metres, negative inside
local m = vol:getMaterialAt(p)                    -- m.slot, m.second, m.weight
local stats = vol:getStats()                      -- stats.chunks, stats.triangles, stats.memoryBytes
```

Events and collision. `onEdited` fires once per applied edit, in the frame of the edit, with debris points on a
deterministic lattice:

```lua
vol.onEdited = function(e)
    spawnDebris(e.debrisPoints, e.removedByMaterial)
end
vol.onCollisionUpdated = function(bounds) wakeThingsIn(bounds) end
vol.onBodyRescued = function(body) print(body.name) end

vol:ensureCollision(region)
if vol:isCollisionReady(region) then startLevel() end
```

Saves keep only what the game changed, against the asset it loaded:

```lua
vol:saveChanges("slot1")
if vol:hasSavedChanges("slot1") then vol:loadChanges("slot1") end
vol:revertChanges()
```

## For agents

`docs/mcp/guide/voxels.md` (`jasper://docs/voxels`) describes `edit_voxels`, `sample_voxels`, `visualize_voxels`
and `generate_voxels`.
