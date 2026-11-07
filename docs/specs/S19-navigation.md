# S19 — Navigation: Recast/Detour navmesh, crowds, agents, grid/hex A*, nav-grid brush

Wave 3. Report: `docs/openengine-analysis/03-world-building.md` §1.6, §2.6, gap #2, port candidate #2
(summary row 28). THEIRS paths are relative to `external/OpenEngine/`, OURS to the repo root.

## 1. Goal

JasperEngine has no navigation at all. Ship a Unity-like navigation stack: a Recast/Detour navmesh
baked in tiles on the job system from terrain, static colliders and meshes (no physics world
needed), runtime carving obstacles, off-mesh links, area costs, `dtCrowd` avoidance and a
`NavMeshAgent` that moves entities. Add a grid/hex A* with costs and reservations for 2D, tilemap
and strategy games, painted with a nav-grid brush. Humans get a Navigation window and Scene view
overlays. Scripts get a Lua API. Agents get MCP tools to bake, query and screenshot. It must run on
every export target, including single-threaded web.

## 2. Scope

**In scope**
- Vendoring recastnavigation (Recast, Detour, DetourCrowd, DetourTileCache, DebugUtils) into
  `engine/libs/recastnavigation/`.
- `NavMeshSurface` tiled bakes: multi-threaded, incremental (only dirty tiles), cancelable, with
  progress.
- A bake input collector: terrain, static 3D colliders, render meshes, and Box2D static colliders
  (2D).
- Areas (16) with costs, agent types, `NavMeshModifier`, `NavMeshLink` (off-mesh links).
- `NavMeshObstacle`: avoidance-only, or carving through `dtTileCache`.
- `NavMeshAgent` with a movement system (Unity fields) and crowd avoidance with priorities.
- `NavGrid` (square 8-neighbour / hex 6-neighbour): A* with area costs, reservations, a reachability
  index and a parallel batch solve. It can be built from a Tilemap or from terrain, and painted.
- Editor: the Navigation window, inspectors, Scene view overlay and gizmos, and the nav-grid brush.
- `.navmesh` / `.navgrid` assets (S04 GUIDs), export, a Lua API with events, MCP tools, docs and
  tests.

**Out of scope (default decisions)**
- Copying their PathfindingECS systems verbatim. They are bound to their POD ECS, which is on the
  do-not-copy list (README "their ECS core"). We rewrite them as our subsystem.
- GPU anything: navigation is CPU-only.
- Auto-generated drop/jump links (Unity "Generate Links"). Links are authored only.
- Pathing across surfaces (their `NavLinks` + Dijkstra across maps). An agent uses one surface.
  Follow-up.
- Side-view platformer navigation graphs. Spherical/planet terrain (we have none).
- 64-bit poly refs (`DT_POLYREF64`). See R6.

## 3. Current state

- **No navigation code exists.** A grep of `engine/` and `editor/` for navmesh/recast/detour finds
  nothing. That matches 03-world-building.md §2.6.
- **Threads.** `engine/core/thread/ThreadPoolManager.h` is a single FIFO `std::function` pool with
  `enqueue` → `std::future`. It has no priorities, no cancellation and no ParallelFor; S06 replaces
  it. Web builds are **single-threaded by default**: `EMSCRIPTEN_THREAD_SUPPORT` is OFF at
  `engine/CMakeLists.txt:208`.
- **ECS and systems.** Systems are registered in order in `engine/core/Scene.cpp:34-40`
  (`registerSystem<…>`), and `Scene::fixedUpdate` (`Scene.cpp:722`) runs them in that order. The
  `SubSystem` virtuals are in `engine/core/ecs/SubSystem.h:41-54` (`fixedUpdate`,
  `onComponentAdded/Removed`, `onEntityLayerChanged`). Component kinds are
  `editor/Catalog.h:71` `ComponentType`, and new kinds must be appended at the end (comment at
  `Catalog.h:123`). Adding a component touches about 25 files; `ReflectionProbeComponent` is the
  template: `Stream.cpp`, `Factory.cpp`, `InspectorWindow.cpp`, `ComponentAddDialog.cpp`,
  `EntityRegistry.cpp`, `EntityCloner.cpp`, `McpValues.cpp`, `CreateEntityCmd.cpp`, and so on.
- **Bake inputs that exist.**
  - Terrain heights are readable on the CPU through `TerrainComponent::heightMap.getData()`, and
    `MeshSystem::sampleTerrainSurface` (`engine/core/subsystem/MeshSystem.cpp:3205`) returns height
    and normal. Size and height come from `terrainSize`/`maxHeight`
    (`component/TerrainComponent.h:169-170`).
  - 3D collider parameters (box, sphere, capsule, tapered capsule, cylinder, convex hull, mesh,
    heightfield) live in `Collider3D` (`component/RigidbodyComponent.h:47-72`) next to the Jolt
    shape, so they can be read without Jolt.
  - Mesh triangle extraction exists, but only inside the Jolt path:
    `PhysicsSystem.cpp:162` `meshSourceEntities` and `:1198-1330`, under
    `#ifdef JASPERENGINE_PHYSICS_3D`.
  - 2D colliders (`Collider2D`: polygon, circle, capsule, segment, chain) are plain data
    (`component/Rigidbody2DComponent.h:24-56`). 2D scenes are in points, scaled by
    `PhysicsSystem::pointsToMeterScale2D` (`PhysicsSystem.h:57`).
- **Tilemaps are free-placed quads, not a cell grid.** In `component/TilemapComponent.h`,
  `TileData` is `{name, rectId, position, width, height}`. Grid integration therefore has to
  rasterize the tiles (R30).
- **Editor hooks.**
  - `TerrainEditWindow` (`editor/window/TerrainEditWindow.h:154`) and its Scene view routing
    (`SceneWindow.cpp:1031, 1329, 1634`) are the model for a brush tool.
  - `SceneRender` owns `LineRenderer` overlays (`editor/render/SceneRender.h:52, 234`).
  - MCP tools register per feature (`editor/mcp/tools/McpTools.h`). Long work runs through
    `McpJobs` (`get_job`/`cancel_job`). `screenshot` with `view:"scene"` exists
    (`ViewTools.cpp:1114`).
- **Export.** `Exporter::copyAssets` (`editor/Exporter.cpp:1736`) ships the whole assets tree, so
  baked files ship automatically.
- **Compression.** `stb_image_write.h` (`stbi_zlib_compress`) and `stb_image.h`
  (`stbi_zlib_decode_*`) are already vendored in `engine/libs/stb/`. They give a deflate pair for
  tile-cache layers with no new dependency.

## 4. Reference (OpenEngine, MIT; recastnavigation, zlib)

| THEIRS | Take | Improve |
|---|---|---|
| `Engine/Modules/Pathfinding/Source/DetourNavMap.cpp` (904 LOC) | Tiled `dtNavMesh` init; `BuildTile` as a pure function of its arguments (runs on any thread); `AddTile`/`RemoveTile` swap on the query thread; Recast config derived from agent + one `CellSize` dial (lines 81-95: `walkableClimb`, `walkableRadius`, `borderSize = walkableRadius + padding`, `detailSampleMaxError = ch`); findPath + straight path, closest point, raycast | They have no areas (every tri is `RC_WALKABLE_AREA`), no off-mesh links, no tile cache and no incremental bake. We add all four |
| `Engine/Modules/Pathfinding/Source/ObstacleAvoidance.cpp` (390 LOC) | `dtCrowd` wrapper and its four `dtObstacleAvoidanceParams` presets (lines 95-126; the comment that every field must be set or velocity is zero) | Their wrapper has no avoidance priority. We patch DetourCrowd (D7) |
| `Engine/Modules/PathfindingECS/Source/NavigationSourceCollector.{h,cpp}` (580 LOC) | **Reference only.** Gather world-space triangles once on the main thread into an immutable struct with per-item bounds (`NavigationStaticGeometry::AppendOverlapping`); the Auto policy excludes entities that move (dynamic/kinematic bodies, agents, and their descendants); terrain lattice per tile with a stride | Gather through a uniform-grid bucket instead of a linear scan per tile; hash inputs per tile for incremental bakes |
| `Engine/Modules/PathfindingECS/Source/NavigationMeshBake.cpp` (139 LOC) | Tile jobs own their input snapshot; finished tiles land as they complete | Assemble in tile-index order (determinism, R40); cancel token; web time-slicing |
| `Engine/Modules/Pathfinding/Source/GridMap.cpp`, `AStarSolver.cpp`, `AStarSolverPool.cpp`, `PathSmoother.cpp`, `HexGridHelpers.cpp` | Flat arrays per cell; generation-stamped A* scratch (no per-solve clear); octile/hex heuristics; slope/step checks; time-windowed cell reservations as a cost penalty (`kReservationPenalty = 3`, `AStarSolver.cpp:16`); leased solver pool for parallel batches | An unreachable goal costs a full flood (their Architecture.md: about 210 ms on 1024²). We add a connected-component index (O(1) reject). Their `WorldToHex` (`HexGridHelpers.cpp:58-80`) is a rectangular approximation, and their doc calls the layout "flat-top" although the code is pointy-top odd-r. We do exact cube rounding and document pointy-top odd-r. Reservations are keyed on fixed ticks, not float seconds |
| `Apps/Editor/Source/SceneView/NavGridBrushTool.cpp` (425 LOC), `Engine/Source/Assets/NavGridAsset.cpp` | Stroke → per-cell before-snapshot → one undo command; brush radius; binary `NGRD` asset with magic + version | We paint **areas** (consistent with navmesh areas) instead of a free float cost + blocked bit; circle/square brushes; line strokes; MCP parity |
| `Engine/Modules/Pathfinding/Architecture.md` | Module split (core library without ECS + integration layer); thread-safety table | — |

Licences:
- OpenEngine code we adapt keeps its MIT header and gets an entry in `NOTICE.md`.
- recastnavigation is zlib. The licence requires altered sources to be plainly marked, so every
  patched file gets a `// JasperEngine patch:` comment and an entry in
  `engine/libs/recastnavigation/PATCHES.md` (lexbor precedent in `NOTICE.md`).
- No licence file has to ship with exports.

## 5. Requirements

### Functional — navmesh

- **R1 Vendoring.** recastnavigation (the latest release tag at vendoring time, ≥ 1.6.0, commit
  pinned) goes under `engine/libs/recastnavigation/`.
  - Contents: `Recast/`, `Detour/`, `DetourCrowd/`, `DetourTileCache/`, `DebugUtils/`, `License.txt`,
    a `CMakeLists.txt` of ours, `README.jasper.md` (what is vendored, the commit) and `PATCHES.md`.
  - Not vendored: RecastDemo, Tests or SDL.
  - CMake option `JASPERENGINE_NAVIGATION` (ON). OFF compiles the navigation API as stubs that
    return failure and log once.
  - `NOTICE.md` gets a row.
- **R2 Surface and bake.** `NavMeshSurfaceComponent` bakes the navmesh for one agent type over its
  bounds.
  - Fields:
    - Collect Objects: All / Volume / Children.
    - Use Geometry: RenderMeshes (default) / PhysicsColliders / Both.
    - Include layers.
    - Default area.
    - Volume size/center.
    - Overrides: cell size, tile size (cells, default 64), min region area.
    - Plane: XZ (3D, default) / XY (2D).
    - Runtime carving (bool).
    - Navmesh asset GUID.
  - Terrain is always collected under any Use Geometry setting.
  - Entities with Dynamic/Kinematic bodies, `NavMeshAgent`s, `NavMeshObstacle`s and their
    descendants are excluded unless a `NavMeshModifier` with `overrideInclusion = Include` says
    otherwise.
- **R3 No physics world.** The collector reads `Collider3D`/`Collider2D` parameters and mesh
  buffers directly. Mesh triangle extraction moves out of `PhysicsSystem.cpp` into a shared
  physics-independent helper (`engine/core/util/MeshTriangles.{h,cpp}`) used by both. A bake must
  work with `JASPERENGINE_PHYSICS_2D=OFF` and `JASPERENGINE_PHYSICS_3D=OFF`.
- **R4 Tiled, threaded, incremental bake.**
  1. The main thread snapshots the inputs: world-space triangles + per-triangle area, bucketed in
     a uniform XZ grid of tile size.
  2. Each tile builds as an independent job on the S06 background lane (the editor) or the normal
     lane (runtime).
  3. Finished tiles are added on the main thread.
  - Each tile stores a 64-bit hash of its inputs: overlapping triangles, areas, links, settings and
    the area table. "Bake" rebuilds only tiles whose hash changed. "Clear + Bake" rebuilds all.
  - Bakes report progress (tiles done/total) and are cancelable. A cancel keeps the previous
    navmesh intact.
- **R5 Areas.** There are 16 areas, configured per project: name, cost (≥ 1), colour.
  - 0 Walkable (cost 1), 1 Not Walkable, 2 Jump (cost 2), 3-15 user-defined.
  - A poly's area is stored in `dtPoly::area`, and its flag is `1 << area`, so agent area masks
    use Detour's native `dtQueryFilter` include flags (no virtual filter).
  - `NavMeshModifierComponent` sets an entity's area (and optionally its subtree's) or
    excludes/includes it. With a box volume it acts as a modifier volume
    (`rcMarkBoxArea`/`rcMarkConvexPolyArea`). Not Walkable becomes `RC_NULL_AREA`.
  - Where areas overlap, the higher area index wins (Recast's max-area span merge).
- **R6 Limits.** 32-bit poly refs. When `maxTiles × maxPolysPerTile` would leave fewer than 10
  salt bits, the bake fails before it starts with the message
  `"Surface too large: N tiles × M polys; increase cell/tile size or split the surface"`.
- **R7 Off-mesh links.** `NavMeshLinkComponent`:
  - Fields: start/end (local), width, bidirectional, area (default Jump), cost override (−1 = area
    cost), auto-update when moved.
  - It bakes as Detour off-mesh connections into the tile that contains its start. A width > 0
    places ⌈width / (2 × agent radius)⌉ parallel links.
  - Moving or editing a link marks only its tile dirty.
  - Agents traverse links linearly at agent speed by default (`autoTraverseOffMeshLink`) and fire
    `onOffMeshLinkStart/End`. With auto-traverse off, scripts call `completeOffMeshLink()`.
- **R8 Queries.** All queries take an agent type + area mask (`NavMeshQueryFilter`) and work in
  editor and play:
  - `calculatePath(from, to)` → status Complete / Partial / Invalid, corners, length.
  - `samplePosition(p, maxDist)` → nearest point on the mesh, its area and normal.
  - `raycast(from, to)` → hit, position, normal (`dtNavMeshQuery::raycast`).
  - `findClosestEdge(p)` → distance, position, normal.
  - `isReachable(from, to)`.
- **R9 Obstacles.** `NavMeshObstacleComponent`:
  - Fields: shape Capsule (cylinder) / Box, center, size, carve, move threshold, time to
    stationary, carve-only-stationary.
  - Non-carving: a zero-speed crowd agent whose position and velocity are written from the
    transform each tick. Agents avoid it.
  - Carving: requires the surface's runtime carving. It becomes a `dtTileCache` cylinder or
    oriented-box obstacle and is re-carved only when it moves more than the threshold, or once it
    has been stationary for the set time.
  - Carving on a surface without runtime carving logs one warning and falls back to non-carving.
- **R10 Runtime rebake.** `NavMeshSurface:bakeAsync()` and `updateTiles(bounds)` re-collect and
  rebuild at runtime for procedural levels. Agents keep moving on the old tiles until the new ones
  swap in.

### Functional — agents and crowds

- **R11 NavMeshAgentComponent** has Unity's fields and defaults:
  - Agent type; speed 3.5; angular speed 120°/s; acceleration 8; stopping distance 0; auto braking
    on; radius 0.5; height 2; base offset 0.
  - Obstacle avoidance quality: None / Low / Medium / Good / High.
  - Avoidance priority 0-99 (lower = more important; default 50).
  - Auto repath on; auto traverse off-mesh link on; area mask (all); update position on; update
    rotation on.
  - Driver: Transform (default) / Rigidbody (sets linear velocity of a 3D or 2D body) /
    CharacterController (S22; passes the desired velocity) / None (script reads `desiredVelocity`).
  - Plus `surface` (an optional entity ref; default: the surface of its agent type under it, else
    the `NavGrid` that contains it).
- **R12 Movement.**
  - `setDestination` snaps the target to the mesh (`samplePosition` within 2 × height).
  - Pathing is asynchronous through dtCrowd's path queue. `pathPending` stays true until a path
    exists.
  - `remainingDistance` follows the corridor. Auto braking decelerates within
    `max(stoppingDistance, speed² / (2 · acceleration))` of the final corner.
  - Arrival = within `stoppingDistance` (min 0.01 m) of the destination → `onDestinationReached`
    fires exactly once per destination.
  - A blocked or invalid path fires `onPathFailed`.
  - `warp(p)` teleports and resets the corridor.
  - An agent that is not on the mesh at enable gets `isOnNavMesh = false`, one warning, and no
    movement.
- **R13 Avoidance priority.** An agent avoids neighbours whose priority ≤ its own and ignores
  neighbours whose priority is greater (Unity semantics). This is implemented as a marked patch in
  `DetourCrowd.cpp`'s neighbour → obstacle-circle and separation loops, plus a priority byte in
  `dtCrowdAgentParams`.
- **R14 Crowd sizing.** There is one `dtCrowd` per (surface, agent type). Max agents comes from the
  project's navigation settings (default 128; the crowd grows by re-init at the next fixed tick
  when it is exceeded, with a warning). Nothing is allocated for scenes without navigation
  components.

### Functional — grids

- **R15 NavGridComponent.**
  - Fields: layout Square / Hex (pointy-top, odd-r); neighbours 4/8 (square); cell size;
    width × height; plane XZ / XY; origin = entity transform; max slope / max step (used when it
    has heights); source None / Tilemap / Terrain; grid asset GUID.
  - Each cell is `uint8 area` (0-15; 1 = blocked). The cost is the project area cost. The cell has
    an optional `float height`.
- **R16 Grid A\*.**
  - Octile (8-neighbour), Manhattan (4) or hex-distance heuristic; generation-stamped scratch;
    corner-cutting off by default.
  - Smoothing: line-of-sight string pulling (Free movement), or cell centres (CellCentres movement
    for strategy/hex games).
  - Results: Complete, Partial (the closest reachable cell, by heuristic), Invalid.
- **R17 Reachability index.** After any cell edit, a union-find labelling over walkable
  neighbours rebuilds on the next query (a dirty flag). Unreachable or blocked goals are rejected
  in O(1) unless `allowPartial`.
- **R18 Reservations.** Agents on a grid reserve the cells along the next `lookahead` ticks
  (default 1.5 s worth of fixed ticks). Other agents pay `reservationPenalty` (default ×3) on
  reserved cells. Reservations expire by tick. This is the grid's avoidance (dtCrowd is
  navmesh-only).
- **R19 Batch solve.** `NavigationSystem` solves all pending grid requests of a fixed tick with
  S06 `ParallelFor` on leased solvers (`maxGridSolvesPerTick`, default 64). Results apply in
  request order, so the outcome does not depend on the thread count.
- **R20 Grid runtime edits.** `setCellArea`, `fillRect` and `stampObstacle` from Lua. Agents whose
  path crosses an edited cell repath on the next tick.

### 2D navigation (Box2D scenes) — decision

- **R21** A 2D scene uses either of two modes, and both use the same agent component:
  - **(a) NavGrid on the XY plane.** For tilemap, grid and strategy games.
  - **(b) NavMeshSurface with Plane = XY.** For free-form top-down games.
    - The collector generates a ground quad over the bounds and rasterizes static
      `Rigidbody2D`/`Collider2D` footprints as `RC_NULL_AREA`: polygons; circles as 16-gons;
      capsules; segments and chains as quads of width 2 × cell size.
    - Recast and Detour run in their XZ space through the mapping (x, y) → (x, 0, y). The agent
      height and climb are fixed internally.
    - Units are scene points. The 2D defaults scale with `pointsToMeterScale2D`
      (radius = 0.5 m in points).
    - dtCrowd avoidance works unchanged.
  - Side-view or platformer navigation is out of scope.

### Platform & backends

- **R22** It runs on every export target. There is no GPU code: the overlays use existing
  line/mesh rendering on all sokol backends.
- **R23 Web single-threaded.** Without `EMSCRIPTEN_THREAD_SUPPORT`, bake/tile jobs run
  time-sliced on the main thread at ≤ 4 ms per frame (a project setting). The results are
  identical to the threaded bake.
- **R24** Navmesh data is stored little-endian. A big-endian host rejects it with an error; we
  ship none.

### Performance (budgets; release builds; "mobile" = Pixel 6a / iPhone 12 class; "web" = wasm, Chrome, single thread)

- **R25 Zero cost when unused.** With no navigation component in a scene, `NavigationSystem`
  `fixedUpdate` returns in < 1 µs, no crowd or query is allocated, and there is no per-frame
  heap allocation.
- **R26 Queries.** `calculatePath` on the reference mesh (`bench_nav_city`: 256 × 256 m,
  400 tiles, about 20k polys), up to 2048 nodes:
  - p95 ≤ 0.15 ms desktop, ≤ 0.5 ms mobile, ≤ 0.6 ms web.
  - `samplePosition` and `raycast` p95 ≤ 0.02 ms desktop.
- **R27 Crowd.** `dtCrowd::update` + sync for 100 agents (Good quality):
  - ≤ 0.6 ms desktop, ≤ 1.5 ms mobile, ≤ 2.0 ms web.
  - 500 agents ≤ 3 ms desktop.
  - Path-queue iterations are capped at 100 per tick (Detour default).
- **R28 Bake.**
  - `bench_nav_city` (terrain + 200 static meshes, cell 0.2 m): full bake ≤ 10 s on an 8-core
    desktop, ≤ 40 s single-threaded.
  - Moving one static mesh: the incremental bake rebuilds ≤ 4 tiles in ≤ 400 ms.
  - Carving: one obstacle move → its tiles updated within 2 fixed ticks, at ≤ 1 ms tile-cache work
    per tick (desktop) / ≤ 2 ms (mobile).
- **R29 Grid.**
  - A* on a 256² grid, worst case reachable: ≤ 2 ms desktop / ≤ 6 ms mobile.
  - Unreachable reject ≤ 5 µs.
  - Batch of 200 paths on a 128² grid ≤ 4 ms on 4 threads.
  - Memory: 1 B/cell (+4 B/cell with heights) + 11 B/cell solver scratch per leased solver.
- **R30 Memory and size.**
  - `bench_nav_city` navmesh ≤ 8 MB in RAM, ≤ 4 MB on disk.
  - Tile-cache layers are deflate-compressed with stb (`stbi_zlib_compress` / `stbi_zlib_decode`).
  - The library adds ≤ 450 KB to the wasm binary (measured by the S02 size check).

### Editor UX (human)

- **R31 Navigation window** (Window ▸ AI ▸ Navigation) has four tabs:
  - **Agents:** agent types (name, radius, height, step height, max slope); default Humanoid
    0.5 / 2 / 0.4 / 45°.
  - **Areas:** name, cost, colour; 0-2 are fixed.
  - **Surfaces:** every surface in the open scenes. For each: bake state (Not baked / Baked /
    Dirty N tiles / Baking x%), tile and poly count, memory, plus Bake, Clear, Select. Bake All.
  - **Grids:** every NavGrid with a Rebuild from source button.

  Settings live in project settings (`navigation:` block) and edits are undoable.
- **R32 Inspectors** for all components, Unity layout.
  - NavMeshSurface: an "Advanced" foldout for the overrides, Bake/Clear buttons, a stale-bake
    warning when the input hashes changed, and a progress bar with Cancel.
  - Agents show `pathStatus`, `remainingDistance` and `isOnNavMesh` (read-only) in play.
- **R33 Scene view overlay** "AI Navigation" (toolbar popup; its state is saved with the
  workspace):
  - Show NavMesh (translucent tris in area colours + edge lines).
  - Show tile bounds, heightmesh off; Show NavGrid (area colours, reservation tint in play).
  - Show agent paths and avoidance (selected / all), obstacles and links.
  - Draws only the tiles intersecting the view frustum, and at most 64k triangles.
- **R34 Gizmos.** Link start/end handles (draggable, undoable). Obstacle shape handles. Surface
  volume box handles. Agent radius/height cylinder on selection.
- **R35 Nav-grid brush.** A brush mode for a selected NavGrid, routed like the terrain brush
  (`SceneWindow.cpp:1031/1329/1634`):
  - Modes: Paint Area (area dropdown), Erase (→ Walkable), Fill (flood).
  - Shape: circle/square; size in cells; `[`/`]` resize; Shift-drag straight line.
  - One undo step per stroke (snapshot of the changed cells, S24 naming "Paint Nav Grid"). The
    `.navgrid` file saves with the scene save, through S03 atomic writes.
- **R36 Tilemap source.** With Source = Tilemap, the inspector shows a rect-name → area table
  (one row per `TileRectData` of the tilemap) and "Cells without tiles" (default Not Walkable).
  - Rebuild rasterizes tile quads onto cells: a cell takes the area of the topmost tile containing
    its centre. Static `Collider2D` footprints can additionally block cells (toggle).
  - With "Auto rebuild" on (default), the grid rebuilds when the tilemap changes. Cells that were
    brush-painted are kept as overrides: a per-cell override bit, cleared by "Reset overrides".

### Agent UX (MCP, Lua, CLI)

- **R37 MCP tools** (`editor/mcp/tools/NavigationTools.cpp`, `registerNavigationTools`):
  - `bake_navigation {scene?, surfaces?: [entityRef], mode: "dirty"|"full"|"clear", wait?: bool}`:
    a job via McpJobs. The result has tiles built, ms, polys and warnings.
  - `get_navigation_info {scene?}`: settings, surfaces (state, tiles, dirty tiles, polys, bytes),
    grids, links, agent and crowd stats (play).
  - `query_navigation {op: "path"|"sample"|"raycast"|"closest_edge"|"reachable", from, to?,
    max_distance?, agent_type?, area_mask?, target?: "navmesh"|"grid"|entityRef}`. Works in edit
    and play.
  - `set_navigation_settings {agents?, areas?, crowd_max_agents?, web_bake_budget_ms?}`: undoable.
  - `paint_nav_grid {grid: entityRef, strokes: [{mode, area?, cells?|points?, radius?, shape?}],
    space?: "world"|"cell"}`: one undo step. The `terrain` strokes tool is the model.
  - `visualize_navigation {scene?, show: {navmesh, tiles, grid, paths, links, obstacles},
    path?: {from, to}, screenshot?: bool, max_width?}`: sets overlay flags (and draws a query path)
    and returns a Scene view PNG through the existing `screenshot` pipeline.
  - `get_runtime_entity` includes the agent state: destination, pathStatus, remainingDistance,
    velocity, isOnNavMesh.
  - All of these are callable through the S31 CLI.
- **R38 Lua.** `NavigationClassesLua.cpp` + stubs + the S23 JSON API description:

```lua
local nav = scene:getNavigationSystem()
local path = nav:calculatePath(fromV3, toV3, NavMeshQueryFilter{agentType="Humanoid", areaMask=0xFFFF})
-- path.status (NavMeshPathStatus.Complete/Partial/Invalid), path.corners {Vector3}, path.length
local ok, hit = nav:samplePosition(p, 2.0, NavAreas.All)   -- hit.position, hit.normal, hit.area, hit.distance
local blocked, hit = nav:raycast(a, b)
local agent = NavMeshAgent(scene, entity)
agent:setDestination(v) ; agent:resetPath() ; agent:warp(p) ; agent:move(offset) ; agent:completeOffMeshLink()
agent.speed, agent.stoppingDistance, agent.isStopped, agent.avoidancePriority, agent.areaMask   -- r/w
agent.remainingDistance, agent.pathPending, agent.pathStatus, agent.velocity, agent.desiredVelocity, agent.isOnNavMesh -- r
agent.onDestinationReached = function(agent) end   -- FunctionSubscribe, like PhysicsSystem.beginContact2D
agent.onPathFailed, agent.onOffMeshLinkStart, agent.onOffMeshLinkEnd
local surf = NavMeshSurface(scene, e) ; surf:bakeAsync() ; surf:updateTiles(aabb) ; surf.onBakeCompleted = fn
local grid = NavGrid(scene, e) ; grid:findPath(cellA, cellB, {allowPartial=true}) ; grid:worldToCell(v) ; grid:cellToWorld(c)
grid:setCellArea(c, area) ; grid:getCellArea(c) ; grid:fillRect(c0, c1, area) ; grid:isReachable(c0, c1)
```

  Events are queued during `fixedUpdate` and dispatched after it, in entity order. A callback may
  call any navigation API (no re-entrancy into dtCrowd).

### Data & compatibility

- **R39 Assets.**
  - **`.navmesh`** (binary, LE): magic `JNAV`, format version, then a header: plane, agent type
    snapshot, area table hash, Recast config, tile count, `dtNavMeshParams`. Per tile: x, y,
    layer, input hash, flags, then the Detour tile data (static mode) or the deflated tile-cache
    layers (carving mode). The links' data is part of the tile.
  - **`.navgrid`**: magic `JNGR`, version, layout, dimensions, cell areas, override bits, optional
    heights.
  - Both get S04 GUIDs and are referenced from components by GUID.
  - Default location: `<scene dir>/<SceneName>/NavMesh-<EntityName>.navmesh` (Unity convention).
  - They are written through S03 atomic writes and appear in the S04 dependency graph
    (scene → navmesh) and missing-asset tracker.
  - An unknown or newer version is refused with an error. The surface reports "Not baked", and
    nothing crashes.
- **R40 Determinism.**
  - (a) The same inputs and settings produce byte-identical `.navmesh` files for any thread count
    and any job completion order on one platform/compiler (tiles are assembled in index order and
    the hashes are content-only).
  - (b) Cross-platform bakes are **not** bit-exact. The baked asset is the shipped truth, and
    players never rebake unless a script asks.
  - (c) Crowd and grid movement step only in `fixedUpdate`, single-threaded, in ascending entity
    order. Two runs of the same build with the same inputs give identical agent positions.
- **R41 Scene data.** The new components are appended to `ComponentType` (after
  `UIDocumentComponent`) with default-valued fields. Old scenes load unchanged. The S03 version
  registry gets an entry, but no migration is needed.
- **R42 Export.**
  - Baked files ship through `copyAssets`.
  - The export preflight (and an S31 `check`) warns when a built scene has a surface that is not
    baked or is stale (hash mismatch) and offers "Bake now".
  - `JASPERENGINE_NAVIGATION=OFF` exports strip the library.

## 6. Design constraints & decisions

- **D1 Layout.** The core library has no ECS dependency. The integration layer is an ordinary
  subsystem.
  - `engine/core/navigation/`:
    - `NavMesh` (dtNavMesh/dtTileCache owner, queries)
    - `NavMeshBuilder` (pure tile build)
    - `NavBakeInput` (snapshot + buckets + hashes)
    - `NavBakeCollector` (scene → input, main thread)
    - `NavCrowd` (dtCrowd wrapper)
    - `NavGridMap`, `GridPathfinder` (A*, solver pool)
    - `NavTypes.h`
  - `engine/core/subsystem/NavigationSystem.{h,cpp}`
  - `engine/core/component/NavMesh{Surface,Agent,Obstacle,Link,Modifier}Component.h`,
    `NavGridComponent.h`
  - `engine/core/object/navigation/` (Lua-facing wrappers, like `object/Tilemap.h`)
- **D2 System order.** `registerSystem<NavigationSystem>()` goes before `PhysicsSystem` in
  `Scene.cpp`. Each fixed tick:
  1. Apply finished bake tiles.
  2. Sync transforms → agents and obstacles.
  3. Grid batch solve.
  4. Tile-cache update (budgeted).
  5. `dtCrowd::update(fixedDt)`.
  6. Write back per driver.
  7. Dispatch events.

  `update(dt)` only interpolates rendered transforms for the Transform driver.
- **D3 Bake pipeline.** The collector runs on the main thread (it touches the ECS) and produces an
  immutable `NavBakeInput` held by `shared_ptr`. Tile jobs read only that and their own
  `rcContext`/scratch. `NavMeshBuilder::buildTile(const NavBakeInput&, TileCoord) → TileBlob` is
  pure. Recast config is derived the way THEIRS `DetourNavMap.cpp:81-95` does it: `ch = cs/2`,
  climb and radius in cells, `borderSize = walkableRadius + 3`.
- **D4 Static vs carving.** Default static `dtNavMesh` tiles (smallest, fastest). Runtime carving
  switches that surface to `dtTileCache` layers (+ about 30 % memory). We do not keep raw geometry
  at runtime unless `bakeAsync`/`updateTiles` is used, in which case it is re-collected.
- **D5 Areas = 16 via poly flags.** This avoids `DT_VIRTUAL_QUERYFILTER` and keeps dtCrowd's
  filter native. Unity's 32 is a documented deviation.
- **D6 Jobs.** Use S06 (background lane in the editor, cancel tokens from S09). Until S06 lands,
  an adapter in `NavBakeScheduler` uses `ThreadPoolManager::enqueue`. Web without threads uses the
  time-sliced main-thread scheduler (R23).
- **D7 DetourCrowd patch.** The priority byte and neighbour filtering (R13) are the only library
  patch. It is listed in `PATCHES.md` and covered by `NavCrowdTest.PriorityYields`.
- **D8 Visualization** is built from `dtNavMesh` tiles into one editor-only dynamic mesh per
  surface (rebuilt on tile change) + `LineRenderer` edges in `SceneRender`. It never exists in
  shipped players. A runtime debug draw for games is out of scope (scripts can use
  `calculatePath` + `LineRenderer`).
- **D9 Grid representation.** Areas, not free floats, so one area table drives navmesh, grid,
  brush and costs. Hex is pointy-top odd-r with exact cube rounding.

C++ sketch (public surface):

```cpp
namespace jasperengine::nav {
enum class PathStatus : uint8_t { Complete, Partial, Invalid };
struct QueryFilter { uint8_t agentType = 0; uint16_t areaMask = 0xFFFF; };
struct NavHit { Vector3 position, normal; float distance = 0; uint8_t area = 0; bool hit = false; };
struct NavPath { PathStatus status = PathStatus::Invalid; std::vector<Vector3> corners; float length = 0; };
class NavigationSystem : public SubSystem {
public:
    NavPath calculatePath(Vector3 from, Vector3 to, QueryFilter = {}) const;
    bool samplePosition(Vector3 p, float maxDistance, NavHit& out, QueryFilter = {}) const;
    bool raycast(Vector3 from, Vector3 to, NavHit& out, QueryFilter = {}) const;
    BakeHandle bake(Entity surface, BakeMode mode);   // progress(), cancel(), onCompleted
    // grid
    GridPath findGridPath(Entity grid, Cell a, Cell b, GridQuery = {}) const;
};
}
```

## 7. Acceptance criteria

Unit tests use gtest (S02) in `engine/tests/navigation/`. MCP tests go in
`tools/mcp/tests/test_navigation.py`. Benchmarks go in `engine/tests/bench/nav_*`. The fixture
scenes are `bench_nav_city` (3D) and `nav_2d_topdown` + `nav_tilemap` (2D), under
`engine/tests/fixtures/navigation/`.

| AC | Verifies | Check |
|---|---|---|
| AC1 | R1, R42 | The CI matrix builds with `JASPERENGINE_NAVIGATION` ON and OFF on all targets. OFF: `NavStubTest.QueriesFail` passes. `NOTICE.md` row and `PATCHES.md` exist (`test_docs.py` link check) |
| AC2 | R3 | `NavBakeTest.NoPhysicsWorld`: bake `bench_nav_city` in a build with both physics options OFF; the poly count equals the default build's ±0 on the same platform |
| AC3 | R2, R4 | `NavBakeTest.CollectPolicy`: a dynamic box and an agent are excluded; a Modifier Include re-includes; Volume and Children modes limit input; terrain is collected under PhysicsColliders |
| AC4 | R4 | `NavBakeTest.IncrementalDirtyTiles`: moving one 2 m box rebuilds ≤ 4 tiles (counted). An untouched rebake rebuilds 0. Cancel mid-bake keeps the previous tile hashes and the query results |
| AC5 | R40a | `NavBakeTest.ByteIdenticalAcrossThreads`: bake with 1, 2 and 8 workers and with shuffled completion order → identical file SHA-256 |
| AC6 | R5, R8 | `NavQueryTest.AreaCosts`: on a plane with a cost-10 strip, the path detours around it. With the strip's area masked out, it never enters. Not Walkable produces no polys |
| AC7 | R6 | `NavBakeTest.TooLargeRejected` gives the exact error string before any job runs |
| AC8 | R7 | `NavLinkTest.GapCrossing`: two islands joined by a link. Path Complete through the link; an agent arrives, firing `onOffMeshLinkStart` then `End` once each. Unidirectional reverse → Partial |
| AC9 | R8 | `NavQueryTest.*`: samplePosition, raycast hit/no-hit, closest edge and isReachable against hand-computed values on a fixture (±1 cell) |
| AC10 | R9 | `NavObstacleTest.CarveBlocksAndRestores`: a carving box placed on a corridor → path Partial/rerouted within 2 fixed ticks; removed → original path restored. Non-carving: 20 agents pass the obstacle with 0 overlaps of radii |
| AC11 | R10 | `NavBakeTest.RuntimeUpdateTiles`: an agent keeps moving during `updateTiles` (no tick without a valid corridor) |
| AC12 | R11, R12 | `NavAgentTest.Arrival`: speed/accel limits respected per tick; with auto braking the speed falls to < 0.05 m/s at the stop with no overshoot past `stoppingDistance + 0.01`; `onDestinationReached` fires once; off-mesh spawn → `isOnNavMesh == false` + 1 warning |
| AC13 | R11 | `NavAgentTest.Drivers`: the Rigidbody (2D and 3D) and None drivers get the same desired velocity as Transform (±1e-4). The CharacterController driver test lands with S22 |
| AC14 | R13 | `NavCrowdTest.PriorityYields`: in a head-on corridor, priority 10 keeps a straighter line (max lateral deviation < 25 % of the priority-90 agent's) |
| AC15 | R14, R25 | `NavSystemTest.ZeroCostEmptyScene`: no allocations (counting allocator) and < 1 µs per fixed tick |
| AC16 | R15, R16, R17 | `NavGridTest.*`: square 4/8 and hex paths match a reference Dijkstra cost (±1e-4). Hex `worldToCell` is exact at cell edges (1e5 random points against a brute-force nearest centre). Unreachable rejected without expansion (expanded-node counter == 0) |
| AC17 | R18 | `NavGridTest.ReservationsSpread`: 8 agents through a 3-wide corridor use ≥ 2 lanes, with fewer than 1 % ticks of two agents in one cell |
| AC18 | R19, R40c | `NavGridTest.BatchDeterministic`: 200 requests, 1 vs 8 threads → identical paths. `NavDeterminismTest.CrowdReplay`: 2 runs × 600 ticks × 50 agents → bit-identical positions |
| AC19 | R21 | `Nav2DTest.TopDownColliders`: `nav_2d_topdown` (Box2D static polygons/circles) bakes on XY; paths keep ≥ agent radius from every collider (sampled). `Nav2DTest.TilemapGrid`: the rect→area table yields the expected cell areas and overrides survive a tilemap edit |
| AC20 | R23 | The web browser gate (S02): `nav_2d_topdown` and `bench_nav_city` (reduced) bake in a single-threaded wasm build; no frame > 4 ms + 2 ms slack during the bake; result SHA equals the threaded wasm bake |
| AC21 | R26–R30 | `bench_nav_*` thresholds (desktop on CI runners; mobile and web numbers recorded by the S15 profiler run on the device lab, release gate). Wasm size delta ≤ 450 KB (S02 size check) |
| AC22 | R31–R34 | Manual (also scripted via MCP + screenshots in `test_navigation.py::test_editor_flow`): open `bench_nav_city`; Navigation window ▸ Surfaces ▸ Bake shows progress and Cancel; the overlay shows area colours; dragging a link handle then Ctrl+Z restores it; the stale warning appears after moving a static mesh |
| AC23 | R35, R36 | `test_navigation.py::test_paint_nav_grid`: paint a 3-cell wall via `paint_nav_grid`, `query_navigation` path detours, `undo` restores the original path; the `.navgrid` is rewritten atomically on save |
| AC24 | R37 | `test_navigation.py`: each tool's schema validates; `bake_navigation wait:false` returns a job and `get_job` reaches done; `query_navigation` works in edit and play; `visualize_navigation screenshot:true` returns a non-flat PNG (existing flat-image warning absent) |
| AC25 | R38 | `test_navigation.py::test_lua_api` via `eval_lua` in play: setDestination → `onDestinationReached` fires; calculatePath/samplePosition/raycast return the documented shapes; generated stubs include all members |
| AC26 | R39, R41 | `NavAssetTest.RoundTrip` (save → load → identical queries); `NavAssetTest.RejectNewerVersion`; an old project without navigation components loads (S03 compatibility corpus). The asset GUID survives a rename (S04 redirect) |
| AC27 | R42 | `test_build.py::test_export_navmesh`: the exported HTML5 and desktop builds contain the `.navmesh`; a headless player (S30) runs `nav_tilemap` and the agents arrive (log marker); the preflight warns on a stale bake |
| AC28 | docs | `docs/navigation.md` (user guide), the MCP agent guide section and the `TOOLS.md` entries exist; `test_docs.py` passes. `tools/smoke-test.sh` adds a bake + path query step and stays green |

## 8. Dependencies & conflicts

- **Depends on:**
  - S01: C++20.
  - S02: gtest, benches, the web gate, the size check.
  - S03: atomic writes, version registry.
  - S04: GUIDs, dependency graph, missing tracker.
  - S06: job lanes, ParallelFor (adapter until it lands).
  - S08: reflection-driven inspector/MCP for the new components (hand-written fallback until it
    lands).
  - S09: cancel tokens, RAII subscriptions.
  - S24: undo naming and grouping for the brush and links.
- **Soft:**
  - S22: CharacterController driver.
  - S18: spline volumes as modifier areas and roads as areas; terrain flatten marks tiles dirty.
    Follow-up.
  - S23: JSON API description and Lua stubs.
  - S15: profiler counters (`Nav.Bake`, `Nav.Crowd`, `Nav.GridSolve`).
  - S30: headless player for AC27.
  - S31: CLI.
  - S26: its BVH could later speed up collector bucketing; not required.
- **Blocks:** nothing in the index. Gameplay templates (AI sample scenes) want this.
- **Merge hotspots:**
  - `editor/Catalog.h` `ComponentType`. S18, S20, S21 and S22 also append there, so coordinate the
    order and rebase.
  - `editor/Stream.cpp/.h`, `Factory.cpp`, `InspectorWindow.cpp`, `ComponentAddDialog.cpp`,
    `EntityRegistry.cpp`, `EntityCloner.cpp`, `McpValues.cpp`.
  - `engine/core/Scene.cpp` (system registration order).
  - `engine/CMakeLists.txt` (libs + sources).
  - `NOTICE.md`.
  - `editor/mcp/tools/McpTools.h`.
  - `editor/render/SceneRender.*` (overlays, shared with S26).
  - `editor/window/SceneWindow.cpp` (tool routing, shared with S18's spline tool and S26).
  - `engine/core/subsystem/PhysicsSystem.cpp` (extracting `MeshTriangles`; S22 also edits it).

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| Terrain heightmap CPU data may be released after upload (`Texture::setReleaseDataAfterLoad`, `texture/Texture.h:107`) | The collector reloads the heightmap image from its asset path when `getData()` is empty; a test covers it |
| Cross-platform float differences make bakes differ per platform | Ship the editor-baked asset (R40b); runtime rebakes are opt-in |
| The DetourCrowd priority patch has to be re-applied on library upgrades | One small marked patch + `PATCHES.md` + a dedicated test; upgrade only by deliberate re-vendoring |
| dtCrowd is O(n·neighbours); large crowds on web | `crowd_max_agents` default 128; neighbour cap 6 (Detour default); a quality preset per agent; the budgets in R27 gate releases |
| 32-bit poly refs cap surface size | Explicit preflight error (R6); 64-bit refs are a follow-up |
| Box2D scale (points) vs Recast cell sizes | 2D defaults derive from `pointsToMeterScale2D`; the inspector shows the cell size in metres and points |
| Agent types beyond one per surface | Like Unity: one surface per agent type; several surfaces may overlap |
| Tile-cache layers vs static tiles diverge in path quality | Same Recast config; `NavObstacleTest` compares path lengths without obstacles (±2 %) |
| Grid heights from terrain on hex layouts | Sample at the cell centre; slope uses the neighbour-centre height difference / centre distance |

## Deviations

What the implementation does differently from the text above, and why.

- **Test location.** The tests are under `engine/tests/core/navigation/` (and the bench in
  `engine/tests/core/bench/NavBench.cpp`), the layout `docs/testing.md` prescribes for core tests,
  not `engine/tests/navigation/`. `NavLinkTest.GapCrossing` (AC8) is `NavAgentTest.LinkGapCrossing`.
- **`JASPERENGINE_NAVIGATION=OFF`** strips Recast/Detour only: navmesh bakes, queries and
  crowds fail (logged once, `NavStubTest`), while Nav Grids (our own A*) keep working.
- **Tile hashes** cover the geometry, areas and Recast config of a tile, not the area costs or
  colours: costs are applied by the query filters at runtime, so editing a cost never dirties a
  bake. The `.navmesh` header still records the area-table hash (R39).
- **Grid memory.** The reachability index (R17) adds 4 bytes per cell on the map, beside the 11
  bytes of solver scratch R29 budgets (`NavGridTest.MemoryPerCell` checks both).
- **Web runtime tiles.** A web player bakes at runtime (`bakeAsync`, `updateTiles`) in tiles of at
  most 32 cells, single-threaded or not, so that each Recast pass of a tile fits the main-thread
  slice of R23; the time-sliced pump also holds back a step its stage's longest so far would push
  past the budget. Baked assets keep their tile size (an `updateTiles` over a 64-cell asset on the
  web rebuilds it whole, at 32 cells).
- **Grid batch budget (R29).** The 200-path batch measures 4.14 ms on 4 threads on the new timing
  server E (median of 5), against the 4 ms desktop budget: accepted under the documented new-E
  allowance (E is about 10 % slower; its gate is 4.4 ms, `tools/perf/bench-thresholds.json`).
- **Jump point search.** Uniform-cost square grids (8 neighbours, no corner cutting, no
  reservations or heights) are searched with JPS: the same path cost as A*
  (`NavGridTest.JumpPointSearchMatchesAStar`). Its scratch adds 17 bytes per cell to a solver
  that ran one (a bordered passability copy and, per straight direction, the distances to the next
  jump point and to the wall, JPS+ style), rebuilt when the map or the area mask changes.
- **Default asset location.** Scenes live at the project root, so the default `.navmesh` is
  `assets/<scene folder>/<SceneName>/NavMesh-<GameObject>.navmesh` (inside the assets folder, where
  the engine reads assets), not next to the scene; `.navgrid` files follow the same rule
  (`NavGrid-<GameObject>.navgrid`).
- **Braking.** dtCrowd always slows an agent within two radii of its goal. With Auto Braking on,
  the agent brakes at half its acceleration (`v = sqrt(a d)`), then closes the last centimetres by
  an eighth per tick, compensating dtCrowd's own slow-down, and arrives within 5 mm of the stopping
  distance (AC12: speed < 0.05 m/s at the stop). With Auto Braking off, dtCrowd's slow-down remains
  (Unity's agents do not slow at all).
- **Non-carving obstacles** are dtCrowd agents that avoidance steers around; after each crowd step
  the obstacles are put back where their transforms are and agents inside an obstacle's circle are
  moved out along the navmesh (AC10's "0 overlaps" holds even when a crowd presses on it).
- **Links.** Detour links are point to point: a link with a `width` bakes ceil(width / 2r)
  connections side by side across it. A link's end connects only within the tile next to its
  start's (Detour).
- **Crowd filters.** dtCrowd has 16 query filters; a crowd whose agents use more than 16 distinct
  area masks gives the extra ones the all-areas filter (logged once).
- **2D units.** On XY surfaces agent radius, height, speed and acceleration stay in metres and are
  converted with the 2D scene's points per metre (`pointsToMeterScale2D`).
- **DebugUtils** is vendored (licence, completeness) but not compiled: the overlays build their
  geometry from Detour tiles directly (`NavMesh::getDebugGeometry`).
- **AC2** compares against a poly count recorded per platform and compiler
  (`engine/tests/core/fixtures/navigation/bench_nav_city_polys.txt`, updated with
  `JASPER_TEST_UPDATE_FIXTURES=1`), so a physics-off build checks it without a second build.
- **AC13**: S22 landed, so the Character Controller driver test is in (`NavAgentTest.CharacterControllerDriver`).
- **Lua extras** beyond R38: `isOnOffMeshLink`, `currentOffMeshLinkData`, `NavAreas` (masks and
  `getAreaFromName`, `getAreaCost`, `maskOf`).
- **R41 version registry.** S03's bump rule (`editor/StreamMigrations.h`) says adding optional
  components does not bump a document version; old scenes load unchanged and newer editors
  preserve unknown components, so the scene version stays where it is (no S18 renumbering needed).
- **`bake_navigation` in play** is refused (a game rebakes with `NavMeshSurface:bakeAsync()`); the
  other tools work in edit mode and in play as R37 says.
- **The S31 check** reports navigation findings (tool `navigation`) in a whole-project
  `check` run rather than behind a flag of its own; the export preflight logs the same warnings
  and points to Window > AI > Navigation > Bake All.
- **Overlay culling.** The Scene view overlay re-extracts the visible tiles when the view, the
  navmesh or the area colours change (and at most 64k triangles), instead of keeping one dynamic
  mesh per surface built once per tile change.
- **Gizmos (R34).** Link start and end handles drag in the Scene view (one undo step per drag);
  obstacle shapes, surface and modifier volumes and the agent's cylinder are drawn on selection
  and edited through their Inspector fields (no drag handles of their own).
- **S08 reflection.** The six components are described by tables in
  `editor/reflect/components/NavigationReflect.cpp` (with their ten enums), which give the Catalog
  properties, names, icons, MCP enum options and schemas, StreamSchema's enum tables and known keys,
  add/remove/copy, locators and the Inspector's enum rows. As S08 did for S22's gameplay
  components, they keep their own codecs (`StreamNavigation.cpp`, every key written: files are
  unchanged) and exported code (`FactoryNavigation.cpp`, enum names from the tables), and their
  hand drawers (`InspectorNavigation.cpp`, registered in `InspectorWindow::componentDrawer`).
  `tileAreas` is one opaque value (the Inspector edits it as a table); runtime state is no field.
  One label per enum value: MCP's labels are now the Inspector's (Modifier mode "Remove Object",
  grid movement "Free (smoothed)", grid source "None (painted)"); the tokens are unchanged.
  S74 R20 relabelled the Modifier modes after what they do: `auto` "Surface Policy", `include`
  "Add or Modify Object", `exclude` "Remove Object" (tokens unchanged).
- **`bench_nav_city` size** (recorded by S74 R28). The reference mesh of R26 bakes to 2643 polys on
  linux-gcc-x86_64 and 2670 on apple-clang-arm64
  (`engine/tests/core/fixtures/navigation/bench_nav_city_polys.txt`), not "about 20k". R26's
  query budget is measured on that mesh.
- **Timing allowance** (recorded by S74 R28). The je-bench-nav gates are the desktop budgets of
  R25-R30 plus 10 % for the timing server E; that allowance is written only in
  `tools/perf/bench-thresholds.json` (its `_comment` and the thresholds), not in this spec's
  requirements.

## Progress (implementation notes, keep updated)

Done on the merged tree (oe-port 424925911, S08 reflection): nav core 50/50; editor 192/192; MCP
55/55 (MCP_SUITE=all subset incl. navigation 9 with the overlay golden, lua_api_json, conventions,
ecs, docs, autosync, reflection); linux GL and Vulkan WERROR; ASan+UBSan core 80/80, editor 125/125
(pre-S08 merge); E bench all budgets (median of 5); web gate single-threaded on E 3/3 and threaded,
same navmesh hashes; Android emulator (A) pass.
Evidence on the final tree (323587682 + a61b23298): linux Vulkan WERROR (D) clean; ASan+UBSan (B)
core 82/82, editor 186/186 (navigation, reflection, stream, ECS, scheduler filters), no sanitizer
reports. The first run leaked every scene in two S08 tests (ReflectMcpRead, ReflectedValueCmd: no
releaseScenes before the Project goes); a61b23298 releases them as the other editor tests do.
Merged oe-port 184010f94 (S16, upstream round 2, BF5, BF3, S08 follow-up) at 3a4c20a8f: upstream's
PoseAnimator and AnimationPlayer keep component types 65 and 66, navigation's six follow (67..72);
AssetType NavMesh/NavGrid are 18/19. The reflection fixture, its goldens, the Catalog snapshot and
the locators are oe-port's plus navigation only. Follow-ups: 7be1f53e8 (AssetRefSchema's member
list, the Animation Player test no longer assumes the last type), 58d22d6ed (the agent guide was
16430 bytes over 16384 with navigation's group and pointer: the pointer is one short line and two
Physics/Build lines say the same in fewer words; 16366 now). The navigation test project has a sun
and colours and an emulator screen golden (tools/test-projects/navigation/golden/screen.png).
Re-gate: xcode_sync_* --check all pass; linux GL and Vulkan WERROR (0 warnings); core 1796/1796,
editor 1265/1265; MCP_SUITE=all navigation, reflection, lua_api_json, budget, conventions,
protocol, groups, docs 53/53 (listing core 98303 of 100k, guide 16366 of 16384); smoke
(post-processing, with the navigation step) pass; web: the sample project's gate pass (C), the
navigation single-threaded gate on E 3/3 with the same hashes; Android emulator (A) with the
screen golden pass.
ASan+UBSan (B) on the merged tree, navigation, reflection, stream, ECS and scheduler suites: core
83/83, editor 189/189, no sanitizer reports.
