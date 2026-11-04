# Navigation (navmeshes, agents, crowds, grids)

Served as `jasper://docs/navigation`. The user guide is [docs/navigation.md](../../navigation.md);
the design is [docs/specs/S19-navigation.md](../../specs/S19-navigation.md), with the fixes of
[S53](../../specs/S53-navigation-fixes.md). Tools: group `navigation` (`tool_groups` enables it in
tools/list; callable by name without it).

## The model (Unity AI Navigation names)

| Component (`add_component` name) | What it does |
|---|---|
| NavMesh Surface (`navmeshsurface`) | Bakes a navmesh for one agent type from what it collects (render meshes, collider shapes or both; All, Volume or this hierarchy; layers). Its `navMeshData` is the baked `.navmesh` asset. |
| NavMesh Agent (`navmeshagent`) | Walks a surface (or a Nav Grid): speed, acceleration, stopping distance, auto braking, radius/height, avoidance quality and priority (0-99, lower wins), area mask, driver (Transform, Rigidbody, Character Controller, None). |
| NavMesh Obstacle (`navmeshobstacle`) | Box or capsule agents avoid; with Carve (and Runtime Carving on its surface) it cuts the navmesh. |
| NavMesh Link (`navmeshlink`) | An off-mesh connection (jump, ladder): start/end in its GameObject's space, bidirectional, cost override, area. |
| NavMesh Modifier (`navmeshmodifier`) | Mode `auto` (Surface Policy: collected as the surface decides), `include` (Add or Modify Object) or `exclude` (Remove Object) for a hierarchy, or give it (or a volume box) an area. |
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
  The result has tiles built, polys, ms and warnings per surface. Warnings include Recast's own
  (`tile (x, y): ...`, for example "Heightfield has too many layers" when more than 63 floors
  stack in one column) and range warnings (a surface taller than a tile's 8191 voxels); the bake
  still succeeds.
- A failed bake (refused, a tile that failed, or the save) fails the job with the reason, logs one
  Console error `NavMeshSurface '<name>': <reason>`, and `get_navigation_info` keeps it as the
  surface's `last_bake` `{ok, cancelled, error?, warnings}` for the session, until a bake succeeds
  (the Inspector shows "Bake failed: ..." meanwhile). A cancel logs `bake cancelled` and keeps the
  previous navmesh; a Clear removes `last_bake`.
- After moving static geometry, `get_navigation_info` shows `dirty_tiles` > 0 (the Inspector's
  stale-bake warning); bake again.
- `query_navigation` works in edit mode and in play (the running scene). `op`: `path`, `sample`
  (nearest navmesh point within `max_distance`), `raycast` (`blocked`, hit point), `closest_edge`,
  `reachable`. `agent_type` by name or id, `area_mask` a bit mask, `target` `navmesh`, `grid` or a
  surface/grid GameObject.
- `visualize_navigation`'s `path` takes the same `agent_type` and `area_mask`; in play it draws on
  the running scene (after a scene load, the copy the game shows now).

Play policy: `bake_navigation` and `paint_nav_grid` are refused while a scene plays (a game rebakes
with `NavMeshSurface:bakeAsync()` and edits cells from Lua); `get_navigation_info`,
`query_navigation` and `visualize_navigation` read the running scene; `set_navigation_settings`
works in play too (area costs apply at the next tick, and the change stays after Stop, like
Project Settings).

Agents in play (Lua):

```lua
local agent = NavMeshAgent(scene, self.entity)
agent.onDestinationReached = function(a) print("arrived") end
agent:setDestination(Vector3(10, 0, 5))
-- each frame or tick: agent.remainingDistance, agent.pathStatus, agent.velocity, agent.isOnNavMesh
```

`get_runtime_entity` on an agent shows destination, pathStatus, pathPending, remainingDistance,
velocity, isOnNavMesh and isOnOffMeshLink. Events fire after the fixed step, in entity order.

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
-- NavAreas.All / Walkable / NotWalkable / Jump are masks; NavAreas.maskOf(area), getAreaFromName(name)
local ok, hit = nav:samplePosition(p, 2.0, NavAreas.All) -- hit.position, hit.normal, hit.area, hit.distance
local blocked, hit2 = nav:raycast(a, b)
local surf = NavMeshSurface(scene, e) ; surf:bakeAsync() ; surf:updateTiles(aabb)
local grid = NavGrid(scene, e) ; local p = grid:findPath(GridCell(0, 0), GridCell(9, 4), {allowPartial = true})
grid:setCellArea(GridCell(3, 3), 1) ; grid:fillRect(c0, c1, 3) ; grid:isReachable(c0, c1) -- areas by index (1 Not Walkable)
```

Agent members: `setDestination`, `resetPath`, `warp`, `move`, `completeOffMeshLink`; r/w `speed`,
`stoppingDistance`, `isStopped`, `avoidancePriority`, `areaMask`; read `remainingDistance`,
`pathPending`, `pathStatus`, `velocity`, `desiredVelocity`, `isOnNavMesh`, `isOnOffMeshLink`;
events `onDestinationReached`, `onPathFailed`, `onOffMeshLinkStart`, `onOffMeshLinkEnd`.

## Pitfalls

- A 2D surface (`plane` XY) collects static Collider2D footprints; agent lengths and speeds stay in
  metres (the 2D scene's points per metre convert them).
- Carving obstacles need `runtimeCarving` on the surface; without it they are avoided like any
  agent (logged once). A surface carves at most `maxCarvingObstacles` (default 128; read when the
  navmesh is created); more are avoided instead, with a warning. `set_navigation_settings
  carve_tiles_per_tick` (0 Auto: 4 on desktop, 2 on Android, iOS and web; 1-64) is how many carved
  tiles each surface rebuilds per fixed tick.
- `onDestinationReached` fires only at the destination: an agent at the end of a Partial path
  stops there with `pathStatus` Partial and gets `onPathFailed` instead.
- A crowd holds `crowd_max_agents` (default 128) per surface and agent type; it grows past that with
  a warning.
- Exports carry Recast/Detour unless `navigation.include_in_builds` is off (then navmesh calls fail
  and log once; grids still work). The export check warns on unbaked or stale surfaces.
