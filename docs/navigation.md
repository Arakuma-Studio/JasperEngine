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

A bake that fails (refused, a tile that fails, or a `.navmesh` that cannot be saved) logs one
Console error, `NavMeshSurface '<name>': <reason>`. Until a bake of that surface succeeds, the
Inspector shows **Bake failed: <reason>** under Bake and Clear, and the Navigation window's
Surfaces tab says **Bake failed** (the reason in its tooltip). A bake that succeeds with warnings
says **Baked with N warning(s); see the Console**. Cancel logs `bake cancelled` and keeps the
previous navmesh. Agents read the same through `get_navigation_info` (`last_bake`).

### Bake warnings

A bake reports Recast's own warnings next to its result (the editor's Console, MCP
`bake_navigation`), one line per tile, `tile (x, y): message`; equal lines are merged with a count
and at most 32 are kept (`... and N more`). A failed tile's error ends with Recast's reason, for
example `Recast could not build the tile-cache layers: rcBuildHeightfieldLayers: layer overflow`.
Two warnings come from the bake's own checks:

- A tile's heightfield holds 8191 voxels of height. A surface taller than that (about 682 m at the
  default voxel height) warns `the surface spans H m of height, more than the 8191 voxels ...`:
  geometry above the limit is cut off. Increase the voxel size or split the surface.
- A carving tile keeps at most 32 walkable layers: `tile (x, y): N walkable layers, the lowest 32
  kept`.

### Runtime rebakes

`NavMeshSurface:bakeAsync()` rebakes from what the scene holds now, and `updateTiles(aabb)`
rebuilds only the tiles touching a box (procedural levels). Agents keep moving on the previous
navmesh until the new tiles land; `onBakeCompleted` fires then. On a surface with Runtime
Carving the new tiles are built within the carving budget (below): the surface keeps its current
navmesh in service until every tile of the new one is built, so a full rebake of 400 tiles lands
about 100 fixed ticks (1.7 s at 4 tiles a tick) after its jobs finish. `isBaking` stays true and
`onBakeCompleted` waits until then.

### Terrain Worlds

A surface bakes a Terrain World from its tiles, one height lattice per tile at the tile's sample
spacing (coarser when the voxels are larger), with each tile's holes left out. Tiles that are not in
memory are loaded by the World's streamer while the bake shows Baking. A surface without a Volume is
refused on a World of more than 64 tiles: set **Collect Objects** to Volume, or turn on **Follow
Terrain World Focus**. With Follow on, the surface bakes the tiles within **Radius** (128 m by
default) of the World's focus (its Focus GameObject, else its camera) and, in play, rebakes with
`updateTiles` once the focus has moved a quarter of a tile, dropping tiles beyond Radius plus half a
tile; agents keep walking meanwhile. A single-threaded web build follows only when a script sets
`NavMeshSurface.followTerrainWorldFocus`.

### Runtime carving

Turn on **Runtime Carving** on a surface to keep tile-cache layers (about 30 % more memory):
NavMesh Obstacles with **Carve** then cut holes into it. Without carving, obstacles are avoided
like agents.

- **Thresholds** (Unity's): an obstacle is cut again once it is more than *Move Threshold* from
  where it was cut. A box also re-cuts when it turned about the vertical by more than
  *Move Threshold* / r radians, r being half the diagonal of its footprint (the distance its
  farthest corner travels). Turning a capsule, and tilting either shape, never re-cuts: Detour cuts
  upright shapes. Changing its shape, centre, size, radius, height or scale re-cuts.
- **Carve Only Stationary**: the cut is made once the obstacle has stayed within *Move Threshold*
  of one point for *Time To Stationary* seconds, so a resting physics body's jitter still counts as
  stationary; while it moves it is avoided.
- **Budget**: each fixed tick a surface rebuilds at most **Project Settings > Navigation > Carve
  Tiles Per Tick** tile-cache tiles (Auto: 4 on desktop, 2 on Android, iOS and the web). The count,
  not the machine's speed, decides on which tick a cut lands, so replays are identical. A safety
  ceiling of 4 ms of wall clock stops a tick early (after at least one tile). Requests wait in a
  queue of their own, so many obstacles appearing at once are all cut, a few tiles a tick.
- **Max Carving Obstacles** (the surface's Advanced section, 128 by default, 1-4096): the cuts a
  surface holds at once. An obstacle beyond it is avoided instead, with a warning.
- Detour cuts an obstacle into at most 8 tile-cache layers. A larger one warns
  `NavMeshObstacle '...' covers N tile layers of '...' and is cut in 8`: make it smaller or the
  tiles larger.
- Cuts survive rebakes, reloads and `updateTiles` of the surface.

### Doors and bridges

Areas change at runtime without a rebake, in edit mode and in play:

- A **NavMesh Modifier** with **Volume** and **Apply At Runtime** is a door: the bake only splits the
  navmesh along the box (the polygons keep their areas), and while the modifier is enabled and its
  GameObject active, the polygons inside take its **Area Type**. Not Walkable closes the door;
  disabling the modifier opens it. Moving, turning or resizing the box applies it again; polygons
  then follow their centre (a polygon whose centre lies in the box takes the area), not the baked
  split. Give touching doors a gap of one cell.
- A script volume does the same without a component: `nav:setAreaInVolume(center, size, area,
  {rotation = q, agentType = "Humanoid"})` returns a **NavAreaOverride** (`area`, `center`,
  `size`, `rotation`, `appliedPolygons`, `valid`, `remove()`). It follows the centre rule too, and
  ends with the scene's navigation (Stop).
- Overrides apply in order, modifier volumes by GameObject, then script volumes by creation; a later
  one wins and Not Walkable always wins. Removing the last one of a tile gives it its baked areas
  back exactly. They survive every rebuild of their tiles (incremental bakes, carving, link updates,
  a full bake, a reload).
- A **NavMesh Link** with **Activated** off passes nothing: paths and agents ignore it, an agent
  already on it finishes. It is not a bake input (toggling it never makes the bake stale).
  `occupied` is true while an agent crosses it.
- When an area changes, the agents whose path crosses a changed polygon ask for their destination
  again at the next fixed tick (their whole path, not only the next polygons), and so do agents
  whose path stopped short. Without **Auto Repath** such a path is dropped and `onPathFailed` fires.
  An agent standing where it becomes Not Walkable is moved to the nearest passable polygon.

```lua
local door = NavMeshModifier(self.scene, doorway)
door.enabled = false                                   -- the door opens
local bridge = NavMeshLink(self.scene, bridgeEntity)
bridge.activated = false ; print(bridge.occupied)
local nav = self.scene:getNavigationSystem()
local gate = nav:setAreaInVolume(Vector3(0, 1, 0), Vector3(2, 3, 0.5), NavAreas.getAreaFromName("Not Walkable"))
gate.area = 0 ; gate:remove()
```

In the Scene view, **Show Area Overrides** (AI Navigation overlay) draws runtime volumes always and
script volumes in play, in their area's colour: solid while applied, faint while not.

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

**Paths and arrival.** Paths come from the crowd's own search (Detour's path queue, time-sliced
across agents); no extra search runs when one arrives or is planned again.

- `pathStatus` is Complete when the path reaches the destination, Partial when no path does (an
  unreachable destination, or one further than the search's 4096 nodes): the agent then walks to
  the nearest reachable point and stops there, and `onPathFailed` fires once per destination.
- A crowd agent's corridor holds 256 polygons. A longer path is still Complete: the crowd extends
  the corridor as the agent nears its end, which is not a repath (it happens with Auto Repath
  off too). Until then `remainingDistance` is the distance along the corridor plus the straight
  line from its end to the destination, a lower bound.
- `onDestinationReached` fires only at the destination, within the stopping distance, never at
  the end of a Partial path.

### Links

A NavMesh Link joins two points (in its GameObject's space; drag the handles in the Scene view).
An agent reaching it traverses it at its speed (`onOffMeshLinkStart` / `End`), or, with
**Auto Traverse Off Mesh Link** off, waits there until a script calls `completeOffMeshLink()`.
One-way links are only crossed from the start.

With **Auto Update Positions** on, a link that moves in play moves its connections (each fixed
tick, once an end moved more than half a voxel or its settings changed). On a surface with Runtime
Carving it may move anywhere, its tiles rebuilt within the carving budget, cuts kept. On a static
surface its start must stay in the tile it was baked in, with as many connections: otherwise it
keeps where it was and warns once (`... left its baked tile ...: turn on Runtime Carving or
rebake`). A cost override the surface was not baked with uses the area's cost until a rebake.
With Auto Update Positions off, a moved link stays where it was baked, as in Unity.

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

### Script names

Lua members use Unity's scripting names, the Inspector Unity's labels; stored fields keep theirs.
`NavMeshObstacle(scene, entity)`, `NavMeshLink(scene, entity)` and `NavMeshModifier(scene,
entity)` are the components' handles:

| Lua | Inspector | Stored field |
|---|---|---|
| obstacle `carving` | Carve | `carve` |
| obstacle `carvingMoveThreshold` | Move Threshold | `moveThreshold` |
| obstacle `carvingTimeToStationary` | Time To Stationary | `timeToStationary` |
| obstacle `velocity` (read) | | from its transform each fixed tick |
| link `agentTypeID` | Agent Type | `agentType` |
| link `autoUpdate` | Auto Update Positions | `autoUpdatePosition` |
| link `activated`, `occupied` (read), `updateLink()` | Activated | `activated` |
| modifier `center`, `size` | Center, Size | `volumeCenter`, `volumeSize` |
| modifier `appliedPolygons` (read) | Applied Polygons (play) | |
| agent `obstacleAvoidanceType` | Quality | `obstacleAvoidance` |
| surface `agentTypeID`, `layerMask` | Agent Type, Include Layers | `agentType`, `includeLayers` |

A modifier's `enabled`, `area`, `center` and `size` change a runtime volume at once; `useVolume`,
`applyAtRuntime`, `overrideArea` and `applyToChildren` are bake inputs (read only). A link's other
fields are bake inputs too: `updateLink()` moves its baked connections to them (false, with a
warning, when a surface must be rebaked).

## Queries

```lua
local nav = scene:getNavigationSystem()
local path = nav:calculatePath(from, to, NavMeshQueryFilter{agentType = "Humanoid", areaMask = NavAreas.All})
if path.status == NavMeshPathStatus.Complete then ... end   -- path.corners, path.length
local found, hit = nav:samplePosition(point, 2.0)            -- hit.position, hit.normal, hit.area, hit.distance
local blocked, hit = nav:raycast(a, b)
local found, edge = nav:findClosestEdge(point)
local reachable, why = nav:isReachable(a, b)                -- why: "none" when reachable
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
  filter, then a point of that polygon within the radius (8 polygon picks, then false). It is
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

## Why navigation fails

Every path that is not Complete says why in `reason` (Lua, MCP; `NavPath::failure` in C++), with
the surface that answered, the point it got closest to and the gap left to the end:

| Reason | Status | What it means, what to do |
|---|---|---|
| `no_surface` | Invalid | No enabled NavMesh Surface of the agent type: add one (or check the Agent Type) |
| `not_baked` | Invalid | The surfaces have no navmesh (never baked, cleared, unreadable asset): bake them |
| `start_off_mesh` / `end_off_mesh` | Invalid | No polygon near that end: move it onto the navmesh (`locate` tells how far it is) |
| `start_filtered` / `end_filtered` | Invalid | That end is on an area the Area Mask leaves out |
| `disconnected` | Partial | The ends are on islands that do not connect (or only through one-way links): add a NavMesh Link |
| `blocked_by_area` | Partial | They connect only through areas the Area Mask leaves out |
| `out_of_nodes` | Partial | The path needs more than 2048 polygons: split the trip, or raise Voxel Size |

The checks run in that order; a Partial path still leads to the reachable point closest to the end.
With several surfaces of the agent type, the first Complete one answers, else the best failure
(`out_of_nodes` first, `no_surface` last). Paths and `isReachable` never compute islands
themselves: once something asked for them (the overlays, `getIslands`, the bake report),
`isReachable` rejects two islands at once without a search.

```lua
local nav = scene:getNavigationSystem()
local path = nav:calculatePath(a, b)
if path.status ~= NavMeshPathStatus.Complete then
    print(path.reason, path.surface, path.closestPoint, path.gap)   -- "disconnected", 12, Vector3, 4.2
end
local here = nav:locate(a)              -- onMesh, found, position, snapDistance, island, area, surface
if not here.onMesh and here.found then print("off the mesh by", here.snapDistance) end
local surfaceEntity = here.surface
local coverage = nav:getCoverage(surfaceEntity)   -- walkableArea (m²), islandCount, largestIslandArea, tilesArea, fraction
for _, island in ipairs(nav:getIslands(surfaceEntity)) do
    print(island.id, island.area, island.polys, island.boundsMin, island.boundsMax)
end
local report = NavMeshSurface(scene, surfaceEntity).lastBakeResult   -- nil before a bake
if report and not report.ok then print(report.errorCode, report.hint) end
```

- **Islands** are the parts of a navmesh that connect (NavMesh Links join them, in either
  direction); ids follow the tiles in order, so the same bake gives the same ids.
- **`lastBakeResult`** is the last bake's report: `ok`, `errorCode` (`nothing_collected`,
  `too_large`, `busy`, `tile_failed`, `navmesh_failed`, `cancelled`, `save_failed`, ...) with its
  `hint`, `sources` (meshes, instances, terrains, colliders, colliders2D, volumes, links,
  excluded, triangles), `warnings`, `walkableArea`, `islandCount`, the 16 largest `islands` and
  the five `slowestTiles`. Lua keys are camelCase; the MCP results carry the same values with
  snake_case keys and the same code strings.
- **Findings** (the Navigation window's Problems list, `get_navigation_info`, `jasperengine-editor
  check` and the export) are one list: `not_baked`, `navmesh_unreadable`, `stale_bake`,
  `bake_failed`, `agent_off_mesh`, `agent_no_surface`, `agent_type_missing`, `agent_type_unused`,
  `link_unconnected`, `small_island`, `crowd_filter_overflow` and `obstacle_carve_ignored`, each
  naming its GameObject and the fix. Errors fail `check`; the export only logs them.
- **Why not walkable?** For a point off the navmesh the editor names the reasons, in order:
  `stale_tile`, `outside_volume`, `outside_bounds`, `no_geometry`, `excluded` (layer, moving body,
  agent, obstacle, Exclude modifier, inactive), `not_walkable_area`, `too_steep` (vs Max Slope),
  `low_clearance` (vs Height), `near_wall` (vs Radius), `step_too_high` (vs Step Height),
  `small_region` (vs Minimum Region Area); on the navmesh only `filtered`, `small_island` and
  `stale_tile` apply. The last three are found by rebuilding only the tile under the point with
  that one limit relaxed.

## Path Tester and diagnostic overlays (editor)

**Path Tester** (Scene view toolbar route button, or its **...** menu; the AI Navigation section of
the Gizmos menu; the command **Path Tester**, `navigation.pathTester`, no default key: bind one in
**Edit > Shortcuts...**). While it is on, **Shift+click** sets the start and a **click** the end, on
whatever the pointer is over; a press within 10 px of a handle drags it and the query runs again
every frame. **Esc**, its close button or the toggle ends it. It edits nothing (no undo step) and is
not saved; it works in Edit and in Play. Its panel (the view's top right) chooses:

- **Agent Type** and **Area Mask** (the project's types and areas), as a NavMesh Agent has them;
- **Mode**: **Path** (`calculatePath`), **Raycast** (a straight walk on the mesh: where it stops),
  **Closest Edge** (the nearest wall from the start) or **Locate** (where a point is).

It shows the status and its reason sentence, the length, corners, the gap a Partial path leaves,
how far each end snapped to the mesh and the ends' islands. **Copy Query** copies the same query
as `query_navigation` arguments. The Scene view draws the start green, the end red, a Complete path
magenta and a Partial one orange with a red gap to the end. In **Locate**, a click (or the end of a
drag) also asks why the point is not walkable: the reasons show beside it, with the value against
the agent's limit and **Select** for the GameObject a reason names (an excluded mesh, a Not Walkable
modifier).

The **AI Navigation** section of the Scene view's Gizmos menu also has (saved with the workspace):

- **Color By**: **Areas** (the area colours), **Islands** (one colour per connected part, links
  joining them; with a Path Tester start, the islands it cannot reach are grey) or **Tiles**.
- **Show Steep Slopes**: the collected ground steeper than the agent type's Max Slope, in red.
- **Show Tile Labels**: each visible tile's last build in this session, "12.4 ms · 210 polys ·
  18.5 KB" ("– ms" for a tile not built since the navmesh loaded). With labels or **Show Tile
  Bounds** on, tiles whose inputs changed since the bake are outlined orange.

Everything is built on the CPU from the navmesh and the editor's cached collection, only while its
option is on; a game never draws any of it.

**Window > AI > Navigation**, Surfaces: each surface's **Last Bake** (time, tiles built of all, a
warning count; the row opens to the warnings, the error and its hint), **Walkable** area (m²) and
**Islands**, and below the table **Problems (N)**: the project's navigation findings (the same list
as `get_navigation_info`, the CLI `check` and the export preflight). A click selects the GameObject,
a double-click frames it too.

The **NavMesh Surface** Inspector shows, above Bake, what a bake would build ("Voxels W × H, N
tiles (M voxels per agent radius)", from the cached collection, or from the box in Volume mode)
with a warning when the agent radius gets fewer than 2 voxels, when there are more than 512 tiles
(above the mobile/web budget) or when Detour's polygon references would refuse it; under the state
line, the last bake (time, tiles, polygons, its warnings, an error in red with its hint), the
walkable area and the island count.


## Nav Grids

A Nav Grid lays cells along its GameObject's X and Z (plane XZ) or X and Y (plane XY): Square
(4 or 8 neighbours, no corner cutting) or Hex (pointy-top, odd rows shifted right). Every cell has
an area; paths cost cells x area cost. A reachability index rejects unreachable goals at once.
Agents on a grid reserve the cells ahead of them (*Reservation Lookahead*); others pay *Reservation
Penalty* there, so a group spreads over parallel lanes. All the grid requests of a fixed tick are
solved together on the job system; the result does not depend on the thread count.

- **Paint** (in the Inspector) arms the nav-grid brush in the Scene view: Paint Area, Erase
  (to Walkable) or Fill, circle or square, `[` and `]` make it a cell larger or smaller (1-64
  cells), Shift-click draws a line from the last stroke. One stroke is one undo step ("Paint Nav Grid"). The cells are saved to a `.navgrid`
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

## Changes

- S74: `onDestinationReached` no longer fires at the end of a Partial path (S19 R12 says "of the
  destination"); a script that relied on it reacts to `onPathFailed` instead. Carving cuts land
  on a tick count (Carve Tiles Per Tick) rather than 1 ms of wall clock.
