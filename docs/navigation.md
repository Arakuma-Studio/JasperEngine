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
puts it on. In play, its Inspector shows Path Status, Remaining Distance and Is On NavMesh.

### Links

A NavMesh Link joins two points (in its GameObject's space; drag the handles in the Scene view).
An agent reaching it traverses it at its speed (`onOffMeshLinkStart` / `End`), or, with
**Auto Traverse Off Mesh Link** off, waits there until a script calls `completeOffMeshLink()`.
One-way links are only crossed from the start.

### Crowds

Agents of one surface and agent type form a crowd (Detour's dtCrowd) of up to
*Crowd Max Agents* (128); a full crowd grows, with a warning.

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
