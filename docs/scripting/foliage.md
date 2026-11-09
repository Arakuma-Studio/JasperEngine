# Foliage: details and trees from Lua

A Terrain carries the two kinds Unity users expect. **Details** (`foliageLayers`) are grass and flowers,
placed from density maps. **Trees** (`treePrototypes` and the Terrain's `.jetrees` file) are stored one
by one, with LODs, impostors and capsules. All indices are 0-based, as the other Terrain lists.

## Settings

```lua
local terrain = Terrain(scene, entity)

terrain.detailDistance = 60
terrain.detailDensity = 0.5
terrain:setDetailDensity(0, 0.25)     -- one layer's own multiplier
terrain.treeDistance = 400
terrain.billboardStart = 60
terrain.fadeLength = 4
terrain.maxMeshTrees = 100
terrain.windDirection = Vector3(1, 0, 0)
terrain.windSpeed = 2
terrain.windSize = 8
terrain.windBending = 0.5
```

Foliage Quality and Foliage Distance are the project's, per platform group
(Project Settings > Quality), and a game may override them:

```lua
Engine.setFoliageQuality(FoliageQuality.LOW)   -- scales and caps; never writes a Terrain
Engine.foliageDistanceScale = 0.8              -- 0 removes foliage
Engine.resetFoliageQuality()
Engine.resetFoliageDistanceScale()
```

## Runtime changes

Changes are an overlay on the store: its file stays as it is, and reloading the scene or stopping Play
drops them. Nothing is saved for you: `getRuntimeFoliageState` gives a string to keep with your saves.

```lua
local terrain = Terrain(scene, entity)

-- nearest first; kind "trees" (default), "details" or "all"; max defaults to 256
local near = terrain:getFoliageInstancesInRadius(Vector3(0, 0, 0), 6, {kind = "trees"})
-- each: {id, kind, prototype, position, rotation, scale}; details have layer instead of prototype, and id 0

if near[1] then
    local pose = terrain:removeTreeInstance(near[1].id, {updateNavigation = true})  -- nil if unknown
    print(pose.position, pose.rotation, pose.scale)                                 -- where to spawn a stump
    terrain:restoreTreeInstance(near[1].id)
end
local id = terrain:addTreeInstance({prototype = 0, position = Vector3(3, 0, 4), yaw = 90, scale = 1.2})
terrain:clearDetailsInRadius(Vector3(0, 0, 0), 3)      -- optional third argument: layer
terrain:restoreDetails()

local saved = terrain:getRuntimeFoliageState()         -- a string
local ok = terrain:setRuntimeFoliageState(saved)       -- false if it is not valid
```

On a Terrain World the same calls cover every tile. A tile out of memory keeps what the game removed and
added there, and has it again when it comes back; `getRuntimeFoliageState` includes those tiles, in a
form of its own (it starts `JEFW1`, a lone Terrain's `JEFS1`), and `setRuntimeFoliageState` takes only
the form of the Terrain it is given to.

## Events and hits

A tree is a capsule of its cell's body while the scene simulates, when its prototype's Collision is
Capsule. With no subscriber, contacts cost foliage nothing.

```lua
local terrain = Terrain(scene, entity)

terrain.onTreeHit = function(hit) print(hit.id, hit.prototype, hit.point, hit.normal, hit.body, hit.speed) end
terrain.onTreeRemoved = function(id, pose) print(id, pose.position) end
-- on a Terrain World one handler serves every tile, tiles that come into memory later too

-- trees answer MeshRaycast by default, details only when asked
local ray = Ray(Vector3(0, 10, 0), Vector3(0, -20, 0))
local r = MeshRaycast.raycast(scene, ray)
local treeId = terrain:getTreeFromHit(r)                -- also a Physics ray result; 0 for anything else
```
