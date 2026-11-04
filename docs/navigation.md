# Navigation

JasperEngine's navigation follows Unity's AI Navigation package: you bake a **navmesh** on a
**NavMesh Surface**, and **NavMesh Agents** walk it, steering around each other and around
**NavMesh Obstacles**, crossing gaps through **NavMesh Links**. For grid games (tilemaps,
strategy, puzzles) a **Nav Grid** gives square or hex cells with A* paths. The engine side is
Recast/Detour ([engine/libs/recastnavigation](../engine/libs/recastnavigation/README.jasper.md),
zlib licence); the design and its acceptance criteria are in
[specs/S19-navigation.md](specs/S19-navigation.md). Agents (MCP) read
[mcp/guide/navigation.md](mcp/guide/navigation.md).

## Quick start (3D)

1. Select the ground (or a parent of the level) and **Add Component > Navigation > NavMesh Surface**.
2. Press **Bake** in its Inspector (or Window > AI > Navigation > Surfaces > Bake All). The bake
   runs in the background; the progress bar has **Cancel**. The result is saved as
   `assets/<scene folder>/<SceneName>/NavMesh-<GameObject>.navmesh` and the surface's
   *NavMesh Data* points at it.
3. Add **NavMesh Agent** to a character and, in a script, call `setDestination`:

```lua
local Player = {}
function Player:init()
    self.agent = NavMeshAgent(self.scene, self.entity)
    self.agent.onDestinationReached = function(agent) print("arrived") end
end
function Player:start()
    self.agent:setDestination(Vector3(10, 0, 5))
end
return Player
```

Turn on the Scene view's **AI Navigation** overlay (the gizmos menu) to see the navmesh in its
area colours, tile bounds, agent paths, links and obstacles.

## Surfaces and baking

A surface collects static geometry and voxelizes it for one **agent type** (radius, height,
step height, max slope):

- **Collect Objects**: All Game Objects, Volume (a box in the surface's space), or Current
  Object Hierarchy. **Include Layers** filters by layer.
- **Use Geometry**: Render Meshes, Physics Colliders (the collider shapes are read directly:
  no physics world is needed, and a build without physics bakes the same), or Both. Terrains
  are collected either way.
- What moves is never collected: GameObjects with a NavMesh Agent, a NavMesh Obstacle, a
  non-static Rigidbody (2D or 3D) or a Character Controller, and everything under them. A
  **NavMesh Modifier** overrides this (Add or Modify / Remove), and can give a hierarchy, or a
  volume box, an area.
- **Advanced**: Voxel Size (default the agent radius / 3), Tile Size (64 voxels), Minimum Region
  Area, the NavMesh Data asset.

Bakes are **tiled and incremental**: every tile's inputs are hashed, and Bake rebuilds only the
tiles whose inputs changed (Shift-click Bake rebuilds all). When static geometry moves after a
bake, the Inspector shows "The baked navmesh is out of date: N tiles changed". Tiles build on the
job system in the editor (in the background lane) and in games; a web build without threads
time-slices them on the main thread (**Project Settings > Navigation > Web Bake Budget**, 4 ms per
frame by default). The same inputs give byte-identical `.navmesh` files whatever the thread count.

A surface too large for Detour's 32-bit polygon references is refused before anything runs:
"Surface too large: N tiles x M polys; increase cell/tile size or split the surface".

### Runtime rebakes

`NavMeshSurface:bakeAsync()` rebakes from what the scene holds now, and `updateTiles(aabb)`
rebuilds only the tiles touching a box (procedural levels). Agents keep moving on the previous
navmesh until the new tiles land; `onBakeCompleted` fires then.

### Runtime carving

Turn on **Runtime Carving** on a surface to keep tile-cache layers (about 30 % more memory):
NavMesh Obstacles with **Carve** then cut holes into it (re-cut when they move more than
*Move Threshold*, optionally only once stationary). Carving updates are budgeted (1 ms per fixed
tick). Without carving, obstacles are avoided like agents.

## Areas and agent types

**Window > AI > Navigation** has four tabs:

- **Agents**: agent types (Humanoid 0.5 / 2 / 0.4 / 45 by default). Ids are stable; a removed type's
  surfaces and agents fall back to the first one.
- **Areas**: 16 areas. 0 Walkable, 1 Not Walkable (never entered), 2 Jump (cost 2), 3-15 yours.
  Costs (>= 1, per metre) apply at once; the colour tints the overlays.
- **Surfaces**: every surface in the open scenes with its state (Not baked / Baked / Dirty N
  tiles / Baking %), tiles, polys and memory; Bake, Clear, Select; Bake All.
- **Grids**: every Nav Grid, with Rebuild from source.

They are saved in project.yaml (`navigation:`) and every edit is an undo step. **Project
Settings > Navigation** holds the budgets (crowd max agents, grid solves per tick, web bake
budget) and **Include in Builds**.

Unity has 32 areas; JasperEngine has 16 (an area is a polygon flag, so a mask is Detour's native
filter).

## Agents

The NavMesh Agent has Unity's fields: agent type, base offset, speed, angular speed, acceleration,
stopping distance, auto braking, radius, height, obstacle avoidance quality, priority (0-99:
an agent avoids agents of its priority or lower and ignores higher numbers), auto traverse off-mesh
links, auto repath, area mask. **Driver** says what moves the GameObject:

| Driver | The GameObject moves by |
|---|---|
| Transform | its transform (interpolated between fixed steps) |
| Rigidbody | its Rigidbody / Rigidbody2D linear velocity |
| Character Controller | `simpleMove` of its Character Controller (2D or 3D) |
| None | nothing: read `desiredVelocity` / `nextPosition` in a script |

Movement steps in the fixed update, in entity order, single-threaded: two runs of a build with
the same inputs give the same positions. Events (`onDestinationReached`, `onPathFailed`,
`onOffMeshLinkStart`, `onOffMeshLinkEnd`) are dispatched after the step, so a callback may call
any navigation function.

An agent spawned off the navmesh reports `isOnNavMesh == false` and logs one warning; `warp(p)`
puts it on. In play, its Inspector shows Path Status, Remaining Distance, Is On NavMesh, Last Path
Failure, Path Stale, its own area costs, and Stuck Time with Detect Stuck on.

### Links

A NavMesh Link joins two points (in its GameObject's space; drag the handles in the Scene view).
An agent reaching it traverses it at its speed (`onOffMeshLinkStart` / `End`), or, with
**Auto Traverse Off Mesh Link** off, waits there until a script calls `completeOffMeshLink()`.
One-way links are only crossed from the start.

### Crowds

Agents of one surface and agent type form a crowd (Detour's dtCrowd) of up to
*Crowd Max Agents* (128); a full crowd grows, with a warning.

### Scripted paths and root motion

`agent:calculatePath(target)` plans at once from where the agent stands, on its surface, with its
area mask and costs; the target snaps as `setDestination`'s does. It does not change the agent's
path. `agent:setPath(path)` hands it a path of `calculatePath` (the agent's or the system's): the
path must have been planned on the agent's surface, its polygons must still be open to the agent,
and the agent must stand on one of its first 16 polygons. The agent then follows it as given (the
crowd takes no shortcut through it) to its last corner; `pathPending` is false at once. A path of
more than 256 polygons is followed for its first 256, and the crowd plans the rest near their end.
A refused path returns `false` and leaves the agent without a path (Unity), as does a path table a
script built (it has no `corridor`; one warning).

```lua
local agent = NavMeshAgent(self.scene, self.entity)
local filter = NavMeshQueryFilter{areaMask = NavAreas.All} ; filter:setAreaCost(NavAreas.getAreaFromName("Mud"), 1)
local path = self.scene:getNavigationSystem():calculatePath(agent.nextPosition, target, filter)
if not agent:setPath(path) then print("refused") end
local corridor = agent.path            -- the corridor from where it stands (corners[1] is its position)
local next = agent.steeringTarget      -- the corner it steers to
if agent.isPathStale then agent:setDestination(agent.destination) end
```

`agent.path` is computed when read: its first corner is the agent's position, the rest are
`getPathCorners()` (which stop at a link) and the corridor after them. `isPathStale` is true when
its corridor holds a polygon that is gone (a carving obstacle rebuilt the tile) or closed to its
filter; false while the path is pending. The crowd replans by itself once the change is close.

Root motion: with `updatePosition = false` (or the None driver) the Transform is the script's.
`agent.nextPosition = p` moves the agent's simulation position along the surface towards `p`
(walls stop it) and keeps its path; `agent.velocity = v` sets its velocity for the next fixed tick
only. With a path it moves `v * dt` along the surface and reports `v`, then follows its path again;
without one the crowd steers it at `v` for that tick (avoidance applies, within its acceleration).
Neither writes the Transform.

```lua
agent.updatePosition = false
function MyScript:fixedUpdate(dt)
    agent.nextPosition = self.animatedRoot     -- or: agent.velocity = rootVelocity
end
```

The agent also answers the queries from where it stands with its own filter:
`agent:raycast(target)` (blocked, hit), `agent:findClosestEdge()` (found, hit) and
`agent:samplePathPosition(areaMask, maxDistance)`, which walks its path and returns `true` with
the point where the path first enters an area outside `areaMask`, else `false` with the point
`maxDistance` along it (or its end); the hit carries `area`, `mask` and `distance`.
`agent.nextOffMeshLinkData` is the next link along its path (within 64 corners), the shape of
`currentOffMeshLinkData`.

### Stuck agents

With **Detect Stuck** (`stuckDetection`) on, an agent walking its path (placed, not pending, not
stopped, not on a link, farther than its stopping distance plus radius from the end) keeps an
anchor. Once it is farther than *Stuck Speed Ratio* (0.1) x speed x *Stuck After* (2 s) from the
anchor, the anchor moves there; otherwise `stuckTime` grows. Back-and-forth jitter in place counts
as stuck. When `stuckTime` reaches *Stuck After*, `onStuck(agent)` fires, the agent asks for its
destination again with *Repath When Stuck* (on), and `stuckTime` starts over. `stuckTime` is 0
while the agent is not walking. Off, the watchdog costs one flag test per agent and tick.

```lua
agent.stuckDetection = true
agent.onStuck = function(a) print("stuck for", a.stuckTime, "at", a.nextPosition) end
```

Why a path failed: `onPathFailed(agent, reason)` passes the reason, and `agent.lastPathFailure`
keeps it (`"none"` after a later successful path or `resetPath`). The reasons are the
`pathFailureName`s of path queries: `"no_surface"` (no surface of its agent type),
`"not_baked"`, `"end_off_mesh"` (the crowd refused the destination), and for a target the crowd
could not reach or a path dropped by a replan without Auto Repath, the reason a query from the
agent to its destination gives at that moment (`"disconnected"`, `"blocked_by_area"`, ...). A
one-argument callback still works. C++ subscribers take `(NavMeshAgent, nav::PathFailure)`.

```lua
agent.onPathFailed = function(a, reason)
    if reason == "blocked_by_area" then print("the door is closed") end
end
```

## Queries

```lua
local nav = scene:getNavigationSystem()
local path = nav:calculatePath(from, to, NavMeshQueryFilter{agentType = "Humanoid", areaMask = NavAreas.All})
if path.status == NavMeshPathStatus.Complete then ... end   -- path.corners, path.length
local found, hit = nav:samplePosition(point, 2.0)            -- hit.position, hit.normal, hit.area, hit.distance
local blocked, hit = nav:raycast(a, b)
local found, edge = nav:findClosestEdge(point)
local reachable = nav:isReachable(a, b)
```

Every path also carries `cost` (each stretch's length times its area's cost, a link at its own
cost), `cornerAreas` and `cornerFlags` (one per corner: the area of the stretch that starts there,
and `NavCornerFlags.START` / `END` / `OFF_MESH_LINK`), `areaLengths` (area index to the length
walked in it, only areas walked) and `corridor`, what `NavMeshAgent:setPath` takes (nil on a path a
script built). `{areaCrossings = true}` as the fourth argument adds a corner wherever the area
changes. A raycast's hit has `cost`, the cost of the part it travelled. Lengths are in scene units
(metres in 3D). `NavAreas.getAreaName(3)` names an area.

```lua
local path = nav:calculatePath(a, b, nil, {areaCrossings = true})
print(path.cost, path.areaLengths[NavAreas.getAreaFromName("Mud")], path.cornerFlags[1] == NavCornerFlags.START)
local found, edge = nav:findClosestEdge(p, nil, 5.0)       -- false (edge.distance == 5) when no wall is that close
local ok, move = nav:moveAlongSurface(position, position + step)   -- a player controller's step
-- move.position (on the navmesh), move.hit (a wall stopped it), move.distance, move.area
```

`moveAlongSurface` follows the surface from the polygon under `from` (false when `from` is off the
navmesh) and crosses at most 64 polygons: use it for one step per tick, not for long moves.

### Random points

```lua
nav.randomSeed = 1234                                              -- restarts the stream
local found, hit = nav:randomPoint()                               -- anywhere, uniform by area
local found, hit = nav:randomPoint(filter, {center = p, radius = 10})   -- reachable from p within 10 m
local found, hit = nav:randomPoint(filter, {sameIslandAs = p, seed = 7}) -- on p's island, its own stream
```

- *Anywhere* is uniform by area over the polygons the filter lets in (never Not Walkable, never a
  link). The first call on a navmesh builds an area table (8 bytes per polygon, kept until the
  navmesh changes).
- *Around* (`center` with `radius`) picks among the polygons reachable from the centre through the
  filter, then a point in one; points past the radius are drawn again (8 tries, then false). It is
  **not** uniform: use it for wandering, not for statistics.
- *Same island* keeps to the part of the navmesh connected to `sameIslandAs` (false when nothing is
  under it); it combines with *around*.
- Draws come from the system's stream: a scene and Play start it at `0x5EED`, `randomSeed` restarts
  it, and the same seed, navmesh and calls give the same points on one platform. `seed` draws from
  a stream of its own and leaves the system's alone.

### Area costs

A **NavMesh Query Filter** has its own area costs for the queries it is passed to; the areas it
does not set cost the project's. A NavMesh Agent has its own too (Unity's `SetAreaCost`): runtime
only, gone at Stop, used by its crowd's planning and its own queries.

```lua
local filter = NavMeshQueryFilter{agentType = "Humanoid", areaMask = NavAreas.All}
filter:setAreaCost(NavAreas.getAreaFromName("Mud"), 5) ; print(filter:getAreaCost(3)) ; filter:resetAreaCost(3)
local agent = NavMeshAgent(scene, self.entity)
agent:setAreaCost(3, 10) ; print(agent:getAreaCost(3)) ; agent:resetAreaCost(3)
```

Costs are clamped to at least 1 (A*'s estimate needs it; a lower cost logs one warning). To keep a
query or an agent out of an area, clear the area's bit in its `areaMask`. A crowd has 16 query
filters, one per area mask and costs, shared by the agents with the same ones and freed when no
agent uses them; a 17th combination uses a filter with the agent's mask (its areas kept, other
costs) or, with none, every area, with one warning per crowd.

## Nav Grids

A Nav Grid lays cells along its GameObject's X and Z (plane XZ) or X and Y (plane XY): Square
(4 or 8 neighbours, no corner cutting) or Hex (pointy-top, odd rows shifted right). Every cell has
an area; paths cost cells x area cost. A reachability index rejects unreachable goals at once.
Agents on a grid reserve the cells ahead of them (*Reservation Lookahead*); others pay *Reservation
Penalty* there, so a group spreads over parallel lanes. All the grid requests of a fixed tick are
solved together on the job system; the result does not depend on the thread count.

- **Paint** (in the Inspector) arms the nav-grid brush in the Scene view: Paint Area, Erase
  (to Walkable) or Fill, circle or square, `[` and `]` resize, Shift-click draws a line from the
  last stroke. One stroke is one undo step ("Paint Nav Grid"). The cells are saved to a `.navgrid`
  with the scene.
- **Source Tilemap**: one row per tile rect gives its area; cells without tiles take *Cells
  Without Tiles* (Not Walkable). Static Collider2D footprints can block cells. With *Auto
  Rebuild* the grid follows tilemap edits; painted cells are overrides kept by every rebuild
  (**Reset Overrides** clears them).
- **Source Terrain**: heights from the terrain, steps limited by *Max Slope* and *Max Step*.

```lua
local grid = NavGrid(scene, entity)
local p = grid:findPath(grid:worldToCell(a), grid:worldToCell(b), {allowPartial = true})
for i, point in ipairs(p.points) do ... end
grid:setCellArea(GridCell(4, 2), 1)            -- Not Walkable; agents crossing it repath
grid:fillRect(GridCell(0, 0), GridCell(3, 3), 3)
```

## 2D scenes

A 2D scene uses either a Nav Grid (tilemaps) or a NavMesh Surface with **Plane XY**: it bakes
the static Collider2D footprints (polygons, circles, capsules, segments and chains) out of the
volume or the scene's ground. The same NavMesh Agent walks both. Agent lengths and speeds are in
metres; the scene's points per metre convert them.

## Assets, export and compatibility

- `.navmesh` (binary, little-endian, "JNAV" version 1) and `.navgrid` ("JNGR" version 1) are
  written atomically and have GUIDs (.meta), so renaming them keeps the references. A newer or
  damaged file is refused: the surface reports Not baked and logs why.
- Exported builds ship the baked files. The export check warns when a scene in the build has a
  surface that is not baked or is out of date.
- **Include in Builds** off builds the game without Recast/Detour (`JASPERENGINE_NAVIGATION=OFF`):
  navmesh bakes, queries and agents on navmeshes fail and log once; grids keep working.
- Scenes without navigation components cost nothing: the navigation system does no work and
  allocates nothing.

## Performance

Budgets and measured numbers: [perf/navigation.md](perf/navigation.md).
