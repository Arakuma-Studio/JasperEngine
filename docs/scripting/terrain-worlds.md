# Terrain Worlds from Lua

A Terrain World streams its tiles round its focuses (docs/specs/T4-terrain-worlds.md). From Lua it
is a `TerrainWorld`: `self.gameObject:getTerrainWorld()` (nil on a GameObject without a Terrain
World) or `TerrainWorld(self.scene, self.entity)`. X and Z are world positions. `Terrain`'s queries
and `applyOps` (docs/scripting/terrain.md) work on a World too.

A bad argument raises an error naming it: a level other than `"render"` or `"collision"`, a
negative radius, a position that is not finite, a focus id that does not exist. Calling a
`TerrainWorld` method on a GameObject without a Terrain World raises too. A world whose data cannot
start answers as one with nothing loaded (`isReady()` false, every tile `"absent"`), and
`addFocus`, `setFocus`, `waitForArea` and `onAreaReady` raise with the reason.

## Focuses

The active camera is a focus (Use Camera Focus), and so is the component's Focus GameObject. A
script adds others, for split screens, cut scenes or a server with no camera: at most 8 a world,
those two included.

```lua
local world = self.gameObject:getTerrainWorld()
local focus = world:addFocus(player.position)        -- its id; optional radiusScale, velocity
world:setFocus(focus, player.position, velocity)     -- the velocity predicts where it streams next
world:removeFocus(focus)                             -- false when it was already gone
```

A focus belongs to the world, not to the script that added it: it stays until `removeFocus`, or
until the world stops (its GameObject destroyed, its Terrain World removed or disabled, the scene
stopped or unloaded). A script on a GameObject that goes before the world removes its focus in
`onDestroy`.

## Waiting for ground

`isAreaReady(x, z, radius, level)` is true when every tile of the world touching the circle is in:
level `"render"` draws it at full detail, `"collision"` has its fine colliders. A pending area
streams like a focus. Before a teleport, `ensureCollisionAt(x, z)` puts a coarse collider under
the destination at once.

```lua
local world = self.gameObject:getTerrainWorld()
Coroutine.start(function()
    world:ensureCollisionAt(dest.x, dest.z)
    local ok = world:waitForArea(dest.x, dest.z, 64, "collision", 5)   -- false after 5 s
    if ok then player.position = dest end
end)

world:onAreaReady(dest.x, dest.z, 64, "render", function()
    print("the destination draws at full detail")
end)
```

- `waitForArea(x, z, radius, level [, timeoutSeconds])` waits inside an engine coroutine
  (`Coroutine.start`, `self:startCoroutine`), as `Coroutine.waitUntil` does, and returns true, or
  false at the timeout or when the world stops. It returns at once when the area is ready.
  Outside a coroutine it raises.
- `onAreaReady(x, z, radius, level, callback)` calls back once, in the update phase; a paused game
  gets it on resume.

## Surface and state

```lua
local world = self.gameObject:getTerrainWorld()
local s = world:getSurfaceAt(x, z)
if s.present and not s.hole then
    print(s.height, s.normal, s.exact, s.state)
end
print(world:getTileState(x, z), world:isReady())
local stats = world:getStats()
print(stats.tiles, stats.tileStates.resident, stats.queue)
```

- `getSurfaceAt` returns a `TerrainWorldSurface`: `height` (world Y), `normal`, `hole`, `present`
  (a tile covers the point), `exact` and `state` (`"absent"`, `"overview"` or `"full"`). In a game
  it never waits for a tile: over a tile not in memory it answers from the overview, with `exact`
  false and `state` `"overview"`.
- `getTileState` names the tile's streaming state: `"absent"`, `"cold"`, `"queued"`, `"loading"`,
  `"cpu_ready"`, `"uploading"`, `"resident"` or `"error"`.
- `getStats` returns the streaming's numbers (`TerrainWorldStats`); `running` is false, with
  `error`, when the world's data could not start.

## Settings

```lua
local world = self.gameObject:getTerrainWorld()
world.poolSize = 96            -- resident pages
world.lodDistance = 1.5        -- the near band's multiplier
world.uploadBudget = 4         -- MiB uploaded a frame
world.collisionRadius = 128    -- metres of fine collision round each focus
```

0 is the platform's default for each; a negative value raises. The world takes them at its next
frame.
