# Terrain from Lua

A `Terrain` (the GameObject of a Terrain component) answers queries about its surface at any time,
and a game may change it while it plays when the component has **Allow Runtime Edits** on
(`allowRuntimeEdits`). The edits are the ones `edit_terrain` makes in the editor (docs/terrain.md,
jasper://docs/terrain), in the same op tables.

## Queries

World X and Z in, world answers out. A point past the edge answers for the nearest edge point;
`contains` says whether it was on the terrain.

```lua
local terrain = Terrain(self.scene, self.entity)
local y = terrain:getHeightAt(x, z)              -- world Y of the surface
local n = terrain:getNormalAt(x, z)              -- Vector3, world, unit length
if terrain:contains(x, z) and not terrain:isHoleAt(x, z) then
    print(y, n)
end
local hit = terrain:raycast(origin, direction, 100)   -- RayReturn: hit, distance (world), point, normal, body
```

Costs: a query reads four texels of the height map on the CPU; `raycast` steps the map along the
ray, so it grows with the ray's length in texels. None of them allocate. They need no physics body: `Physics.raycast` hits the terrain's collider instead.

## Edits

```lua
local terrain = Terrain(self.scene, self.entity)
local crater = terrain:applyOps({
    {op = "lower", shape = {type = "circle", center = {x, z}, radius = 6}, delta = 2},
    {op = "paint_layer", layer = 1, shape = {type = "circle", center = {x, z}, radius = 8}},
})
print(crater.ops[1].changed_texels, #crater.warnings)

local w, h = terrain.heightMapWidth, terrain.heightMapHeight
local values = terrain:getHeights(0, 0, 16, 16)  -- 256 terrain-local heights, row by row
terrain:setHeights(0, 0, 16, 16, values)
```

- `applyOps(ops)` takes the op tables of `edit_terrain`, with shapes in world X and Z, heights in
  world Y and deltas in world units. `place_objects`, `erase_objects`, `remap_layers` and
  `import_heightmap` are for the editor only, and so are `mask` shapes. The list is all-or-nothing:
  a bad field raises an error naming it (`ops[2].shape.radius: ...`) and nothing changes.
- It returns what `edit_terrain` returns: `changed` (per map, the rectangle in texels and in
  world X and Z, and the changed texel count), `ops` (per op `changed_texels`, `clipped_low`,
  `clipped_high`, `filtered_out`, `outside`, `seed`), `stats` and `warnings` (`code`, `message`).
- `getHeights(x, y, w, h)` and `setHeights(x, y, w, h, values)` read and write a rectangle of the
  height map in texels, as terrain-local heights (0 to the terrain's Max Height). `getHeights`
  works on any terrain.
- A write on a terrain without Allow Runtime Edits raises an error naming the terrain and the
  setting. There is no undo at runtime.

What a write does, in order:

1. The pixels change at the call: `getHeightAt` on the next line sees the edit.
2. The first write moves the terrain's maps to copies of its own, so the project's files and
   another terrain using the same files keep what was authored. Stopping Play gives back the
   authored terrain.
3. Colliders take the edit before the next physics step: the heightfield is updated over the
   edited rectangle, and sleeping bodies on it wake. A ball on the terrain falls into a new crater.
4. The drawn surface follows within a frame or a few: see Upload below.
5. Foliage and what conforms to the surface (splines) follow at most four times a second.
6. A NavMesh baked over the terrain is not baked again; call the NavMesh Surface's `bakeAsync()`.

## Upload

The height map goes to the GPU whole, at most once a frame. A map bigger than the frame's budget
goes every few frames: a 16-bit 1025 x 1025 map (2.1 MB) every second frame at the 2 MB mobile and
web budget and every frame at the 8 MB desktop one, a 2049 map (8.4 MB) every fifth and every second. The last edit
always reaches the screen. `terrain:setRuntimeUploadBudget(bytes)` sets the budget for this terrain
(0: the platform default).

```lua
local terrain = Terrain(self.scene, self.entity)
terrain:setRuntimeUploadBudget(4 * 1024 * 1024)
```

Costs: an op costs what it costs in the editor and grows with the texels its shape covers. Allow Runtime Edits keeps the maps readable on
the CPU (a 1025 R16 height map is 2 MB) and builds the heightfield with room for heights from 0 to
Max Height.
