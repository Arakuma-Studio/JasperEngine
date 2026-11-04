# Navigation (navmeshes, agents, crowds, grids)

Served as `jasper://docs/navigation`. The user guide is [docs/navigation.md](../../navigation.md);
the design is [docs/specs/S19-navigation.md](../../specs/S19-navigation.md). Tools: group
`navigation` (`tool_groups` enables it in tools/list; callable by name without it).

## The model (Unity AI Navigation names)

| Component (`add_component` name) | What it does |
|---|---|
| NavMesh Surface (`navmeshsurface`) | Bakes a navmesh for one agent type from what it collects (render meshes, collider shapes or both; All, Volume or this hierarchy; layers). Its `navMeshData` is the baked `.navmesh` asset. |
| NavMesh Agent (`navmeshagent`) | Walks a surface (or a Nav Grid): speed, acceleration, stopping distance, auto braking, radius/height, avoidance quality and priority (0-99, lower wins), area mask, driver (Transform, Rigidbody, Character Controller, None). |
| NavMesh Obstacle (`navmeshobstacle`) | Box or capsule agents avoid; with Carve (and Runtime Carving on its surface) it cuts the navmesh. |
| NavMesh Link (`navmeshlink`) | An off-mesh connection (jump, ladder): start/end in its GameObject's space, bidirectional, cost override, area. |
| NavMesh Modifier (`navmeshmodifier`) | Include/exclude a hierarchy from collection, or give it (or a volume box) an area. |
| Nav Grid (`navgrid`) | Square (4/8) or hex cells of one area each, A* with area costs and reservations; painted, or rebuilt from a Tilemap or Terrain. |

Areas: 16 per project (0 Walkable, 1 Not Walkable, 2 Jump, 3-15 yours), each with a cost per metre
(>= 1) and an overlay colour. An area mask has bit `1 << area`. Agent types (default Humanoid 0.5 /
2 / 0.4 / 45 degrees) have stable ids. Both live in project.yaml `navigation:`; change them with
`set_navigation_settings` (one undo step).

Nothing is baked until you bake: a surface reports `not_baked`, agents report `isOnNavMesh` false.
Static geometry only: a GameObject with an agent, an obstacle, a non-static Rigidbody or a
Character Controller (or under one) is never collected.

## Recipes

Bake and check a level:

```json
{"tool": "add_component", "args": {"entity": "Ground", "component": "navmeshsurface"}}
{"tool": "bake_navigation", "args": {}}
{"tool": "get_navigation_info", "args": {}}
{"tool": "query_navigation", "args": {"op": "path", "from": [-10, 0, 0], "to": [10, 0, 5]}}
{"tool": "visualize_navigation", "args": {"show": {"navmesh": true, "links": true}, "path": {"from": [-10, 0, 0], "to": [10, 0, 5]}, "screenshot": true}}
```

- `bake_navigation` is a job: `wait: false` returns a `job_id` (poll `get_job`, stop with
  `cancel_job`; a cancel keeps the previous navmesh). `mode`: `dirty` (default: the tiles whose
  inputs changed, every tile the first time), `full`, `clear` (removes the navmesh and its asset).
  The result has tiles built, polys, ms and warnings per surface.
- After moving static geometry, `get_navigation_info` shows `dirty_tiles` > 0 (the Inspector's
  stale-bake warning); bake again.
- `query_navigation` works in edit mode and in play (the running scene). `op`: `path`, `sample`
  (nearest navmesh point within `max_distance`, default 2), `raycast` (`blocked`, hit point, `cost`),
  `closest_edge` (within `max_distance` when given: `found: false` and `distance` = the limit when
  no wall is that close), `reachable`, `random`, `move_along`. `agent_type` by name or id,
  `area_mask` a bit mask, `target` `navmesh`, `grid` or a surface/grid GameObject. Every op but
  `random` needs `from`.
- Paths carry `cost` (length times area cost), `corner_areas` and `corner_flags` (per corner; flags
  1 start, 2 end, 4 link start) and `area_lengths` (by area name).
- `random`: without `from` anywhere on the navmesh (uniform by area); with `from` and
  `max_distance` a point reachable from `from` within it; with `from` alone a point on `from`'s
  island (`found: false` off the navmesh). `seed` (default 24301) picks the stream: the same seed
  gives the same point, and the game's own stream is never drawn from.
- `move_along` steps from `from` towards `to` along the surface (64 polygons at most): `position`,
  `blocked` (a wall stopped it), `distance`, `area`. Per-query area costs are a Lua feature
  (`NavMeshQueryFilter:setAreaCost` through `eval_lua`).

```json
{"tool": "query_navigation", "args": {"op": "random", "from": [0, 0, 0], "max_distance": 10, "seed": 3}}
{"tool": "query_navigation", "args": {"op": "move_along", "from": [0, 0, 0], "to": [2, 0, 0]}}
```

Agents in play (Lua):

```lua
local agent = NavMeshAgent(scene, self.entity)
agent.onDestinationReached = function(a) print("arrived") end
agent:setDestination(Vector3(10, 0, 5))
-- each frame or tick: agent.remainingDistance, agent.pathStatus, agent.velocity, agent.isOnNavMesh
```

`get_runtime_entity` on an agent shows destination, pathStatus, pathPending, remainingDistance,
velocity, isOnNavMesh, isOnOffMeshLink, stuckTime, lastPathFailure, isPathStale and
steeringTarget. Events fire after the fixed step, in entity order.

Why a path failed (in play): `get_navigation_info` `agents[].last_failure` (`"none"`,
`"disconnected"`, `"blocked_by_area"`, ...) and `stuck_seconds` with stuck detection on; in Lua the
reason is `onPathFailed`'s second argument and `agent.lastPathFailure`.

```lua
agent.onPathFailed = function(a, reason) print("no path:", reason) end   -- one argument works too
agent.stuckDetection = true ; agent.onStuck = function(a) print("stuck", a.stuckTime) end
```

Doors and bridges in play (no rebake): a door is a `navmeshmodifier` with `useVolume` and
`applyAtRuntime` (its `area`, e.g. 1 Not Walkable, applies while it is enabled); a bridge is a
`navmeshlink` with `activated`. Toggle them, then check the path:

```json
{"tool": "set_runtime_property", "args": {"entity": "Door", "component": "navmeshmodifier", "property": "enabled", "value": true}}
{"tool": "query_navigation", "args": {"op": "path", "from": [-10, 0, 0], "to": [14, 0, 0]}}
{"tool": "set_runtime_property", "args": {"entity": "Bridge", "component": "navmeshlink", "property": "activated", "value": false}}
{"tool": "get_navigation_info", "args": {}}
```

- The path's `status` turns `partial` while the door is closed. `get_navigation_info` lists
  `area_overrides` (`source`, `area`, `applied`, `polygons`) and each link's `activated` (and
  `occupied` in play); `get_runtime_entity` shows a modifier's `appliedPolygons`, a link's `occupied`
  and an obstacle's `velocity`. `visualize_navigation` `show.area_overrides` draws the volumes.
- Agents whose path crosses the change ask again at the next fixed tick. Script volumes are Lua
  (`nav:setAreaInVolume`, through `eval_lua`).

Grids (2D, tilemaps, strategy):

```json
{"tool": "add_component", "args": {"entity": "Level", "component": "navgrid", "values": {"navgrid": {"width": 32, "height": 24, "cellSize": 1}}}}
{"tool": "paint_nav_grid", "args": {"grid": "Level", "strokes": [{"mode": "paint", "area": "Not Walkable", "cells": [[10, 5], [10, 9]]}]}}
{"tool": "query_navigation", "args": {"op": "path", "target": "grid", "from": [1, 0, 1], "to": [20, 0, 7]}}
```

- `paint_nav_grid` is ONE undo step ("Paint Nav Grid"); consecutive centres of a stroke are joined
  by a line, `radius` is in cells, `mode` `erase` sets Walkable, `fill` floods the clicked area.
  `space: "world"` takes world `points`. The `.navgrid` is written with the scene save.
- With Source Tilemap, the Inspector maps each tile rect to an area; painted cells are overrides a
  Rebuild keeps (Reset Overrides clears them).

## Lua API (summary)

```lua
local nav = scene:getNavigationSystem()
local path = nav:calculatePath(a, b, NavMeshQueryFilter{agentType = "Humanoid", areaMask = NavAreas.All})
-- path.status (NavMeshPathStatus.Complete/Partial/Invalid), path.corners {Vector3}, path.length
-- path.cost, path.cornerAreas, path.cornerFlags (NavCornerFlags), path.areaLengths[area], path.corridor
-- NavAreas.All / Walkable / NotWalkable / Jump are masks; NavAreas.maskOf(area), getAreaFromName(name), getAreaName(area)
local f = NavMeshQueryFilter{areaMask = NavAreas.All} ; f:setAreaCost(3, 5)   -- per-query costs (>= 1)
local found, pt = nav:randomPoint(f, {center = p, radius = 10, seed = 7})   -- or {} anywhere, {sameIslandAs = p}
local moved, step = nav:moveAlongSurface(a, b) ; local near, edge = nav:findClosestEdge(p, nil, 5.0)
local ok, hit = nav:samplePosition(p, 2.0, NavAreas.All) -- hit.position, hit.normal, hit.area, hit.distance
local blocked, hit2 = nav:raycast(a, b)
local surf = NavMeshSurface(scene, e) ; surf:bakeAsync() ; surf:updateTiles(aabb)
local grid = NavGrid(scene, e) ; local p = grid:findPath(GridCell(0, 0), GridCell(9, 4), {allowPartial = true})
grid:setCellArea(GridCell(3, 3), 1) ; grid:fillRect(c0, c1, 3) ; grid:isReachable(c0, c1) -- areas by index (1 Not Walkable)
```

Agent members: `setDestination`, `resetPath`, `warp`, `move`, `completeOffMeshLink`,
`setAreaCost` / `getAreaCost` / `resetAreaCost` (runtime only); r/w `speed`,
`stoppingDistance`, `isStopped`, `avoidancePriority`, `areaMask`; read `remainingDistance`,
`pathPending`, `pathStatus`, `velocity`, `desiredVelocity`, `isOnNavMesh`, `isOnOffMeshLink`;
events `onDestinationReached`, `onPathFailed(agent, reason)`, `onOffMeshLinkStart`,
`onOffMeshLinkEnd`, `onStuck`. Scripted paths and root motion: `calculatePath(target)`,
`setPath(path)`, `path`, `steeringTarget`, `isPathStale`, rw `nextPosition` and `velocity` (one
tick), `raycast`, `findClosestEdge`, `samplePathPosition`, `nextOffMeshLinkData`.

## Pitfalls

- A 2D surface (`plane` XY) collects static Collider2D footprints; agent lengths and speeds stay in
  metres (the 2D scene's points per metre convert them).
- Carving obstacles need `runtimeCarving` on the surface; without it they are avoided like any
  agent (logged once).
- A crowd holds `crowd_max_agents` (default 128) per surface and agent type; it grows past that with
  a warning.
- Exports carry Recast/Detour unless `navigation.include_in_builds` is off (then navmesh calls fail
  and log once; grids still work). The export check warns on unbaked or stale surfaces.
