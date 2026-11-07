# S55 — Navigation gameplay API (N2): random points, wall distance, surface moves, area costs, runtime areas and links, agent parity, stuck detection

Wave 4 (navigation N2). Builds on S19 (landed). Runs in parallel with S53 (navigation defects) and
S54 (path failures, islands, findings, Path Tester). OURS paths are relative to the repo root;
upstream recastnavigation paths are relative to `engine/libs/recastnavigation/` (vendored 1.6.0;
`/tmp/recast-analysis/src` holds the same release, read-only).

## 1. Goal

S19 ships a Unity-like navmesh, crowds and agents, but a gameplay programmer still cannot do what
Detour offers and Unity users expect: pick a random point to wander to, ask how far the nearest
wall is, clamp a player controller to the navmesh, price a path by its areas, give one agent its
own area costs, close a door or a bridge at runtime without a rebake, hand an agent a path, drive
it by root motion, or learn that it is stuck and why its path failed. S55 exposes these Detour
capabilities with Unity's names, in C++, Lua and MCP, deterministic, and free when unused.

## 2. Scope

**In scope**
- Queries: `randomPoint` (anywhere, around a point, on a point's island; seeded), `findClosestEdge`
  with a maximum distance, `moveAlongSurface`, per-corner areas and flags plus `cost` on paths,
  the raycast's path cost.
- Area costs per query (`NavMeshQueryFilter`) and per agent (`NavMeshAgent:setAreaCost`), with
  crowd filters keyed by mask and costs.
- Runtime area changes without a rebake: NavMesh Modifier volumes applied at runtime (doors,
  bridges), script volumes (`setAreaInVolume`), NavMesh Link `activated` and `occupied`.
- NavMeshAgent parity: `calculatePath`, `setPath`, `path`, `steeringTarget`, `isPathStale`,
  `nextPosition`/`velocity` setters (root motion), `raycast`, `findClosestEdge`,
  `samplePathPosition`, `nextOffMeshLinkData`, `setAreaCost`/`getAreaCost`.
- A stuck watchdog (`onStuck`, `stuckTime`) and `onPathFailed(agent, reason)` with
  `lastPathFailure`.
- Lua handles for NavMesh Obstacle, Link and Modifier; Inspector rows; Scene view drawing of runtime
  area volumes; MCP extensions of the existing navigation and play tools.

**Out of scope (default decisions)**
- S53's defects: the synchronous A* when a crowd path arrives (`NavigationSystem.cpp:2447-2449`),
  making link Auto Update Positions work, carving defects, bake failure logging. S55 adds only the
  Lua `autoUpdate` name and `updateLink()` over S53's code path.
- S54's diagnostics: detecting `nav::PathFailure`, island labels, locate, findings (including the
  code `crowd_filter_overflow`), the Path Tester and diagnostic overlays. S55 consumes them.
- Later roadmap: link traversal modes and jump arcs, auto-generated links, sliced asynchronous path
  requests from scripts, partition choice, streaming, more than 16 areas.
- A runtime debug draw in games (S19 D8 stands).
- Nothing from OpenEngine: their `DetourNavMap` has none of these queries; their PathfindingECS
  is bound to their ECS core (README do-not-copy list).

## 3. Current state

All read from the code (not run):

- **Queries.** `NavigationSystem` has `calculatePath`, `samplePosition`, `raycast`,
  `findClosestEdge`, `isReachable` and `...On(surface)` forms (`NavigationSystem.h:93-103`); no
  random point, no surface move. One `dtNavMeshQuery` of 2048 nodes, paths of at most 2048
  polygons and corners (`NavMesh.cpp:392-394`), main thread only (`NavMesh.h:17`).
- **Closest edge.** `findDistanceToWall` runs with a fixed radius of 64 tile widths
  (`NavMesh.cpp:1080-1083`): there is no maximum distance, and a far wall costs a large Dijkstra.
- **Paths.** `NavPath` is `{status, corners, length}` (`NavTypes.h:79-83`). `findStraightPath` is
  called without flags, refs or options (`NavMesh.cpp:972`), so corner areas, off-mesh flags and
  cost are lost; the polygon corridor is dropped, so a path cannot be handed to an agent. The
  raycast uses the old overload without `dtRaycastHit` (`NavMesh.cpp:1031`): no cost.
- **Filters.** `QueryFilter` is `{agentType, areaMask}` (`NavTypes.h:64-67`); `NavFilter` holds 16
  costs but `fromSettings` always fills the project's (`NavMesh.cpp:56-63`, `NavMesh.h:67-74`).
  Exclude flags are always 0 (`NavMesh.cpp:587-593`).
- **Crowd filters.** `NavCrowd` keys its 16 dtCrowd filters by area mask only
  (`NavCrowd.cpp:104-122`), never frees a slot (a mask stays reserved after its agents leave), and
  a 17th mask falls back to slot 0, which walks every area, with one warning per process.
- **Areas are flags.** A polygon of area `a` has the flag `1 << a` (`NavDetourSupport.h:66-70`);
  links use polygon areas 16..63 for their costs (`NavBakeInput.h:42-43`) but keep the flag of
  their area. Nothing calls `setPolyFlags`/`setPolyArea`; poly flags only change by rebaking.
- **Revision.** `NavMesh::getRevision` changes on every tile change (`NavMesh.cpp:584`, `:1160`);
  the crowd does not read it. dtCrowd checks only the next 10 corridor polygons each tick
  (`DetourCrowd.cpp:942, 1018`), so a polygon blocked 40 polygons ahead is found late.
- **Agents.** The Lua `NavMeshAgent` has Unity's fields and `setDestination`, `resetPath`, `warp`,
  `move`, `completeOffMeshLink`, `getPathCorners`, `currentOffMeshLinkData`
  (`NavigationClassesLua.cpp:307-366`). `velocity` and `nextPosition` are read-only; there is no
  `calculatePath`, `setPath`, `path`, `steeringTarget`, `isPathStale`, `raycast`,
  `samplePathPosition` or per-agent cost. `NavCrowd` has `movePosition` and `setVelocity`
  (`NavCrowd.h:105-114`) but no corridor access.
- **Events** carry no payload: `EventType` + entity (`NavigationSystem.cpp:68-79`);
  `onPathFailed` is `FunctionSubscribe<void(NavMeshAgent)>` (`NavMeshAgentComponent.h:73`), fired
  in five places of `writeBack`/`setDestination` (`NavigationSystem.cpp:1519, 2439, 2454, 2463`)
  and the grid solve (`:2152, 2158`). There is no stuck detection.
- **Links, modifiers, obstacles.** Links have no `activated`/`occupied`; `autoUpdatePosition`
  (`NavMeshLinkComponent.h:30`) is not read by the system (S53); `currentOffMeshLinkData.activated`
  is just `valid` (`NavigationClassesLua.cpp:164`). Modifier volumes are bake-time only
  (`NavBakeCollector.cpp:960-980`) and drawn only when selected (`NavigationOverlay.cpp:573-580`).
  None of the three has a Lua handle.
- **Lua paths are tables** built by `pathTable` (`NavigationClassesLua.cpp:116-125`); the
  hand-written stub declares their shape (`engine/core/script/binding/stubs/Navigation.lua:5-9`).
- **MCP.** `query_navigation` has ops path, sample, raycast, closest_edge, reachable and requires
  `from` (`NavigationTools.cpp:858-878`); `get_navigation_info` lists agents with path state
  (`:302-316`) and links without state (`:293-301`); `get_runtime_entity` adds eight agent fields
  (`PlayTools.cpp:2268-2281`); `set_runtime_property` writes any Catalog property live through
  `McpValues::buildPropertyCommand` (`PlayTools.cpp:2648-2660`).
- **Reflection.** Obstacle, Link and Modifier fields are in `NavigationReflect.cpp:175-229`, with
  hand codecs in `editor/StreamNavigation.cpp` (every key written) and `FactoryNavigation.cpp`.
  Inspector labels are already Unity's ("Carve", "Move Threshold", "Auto Update Positions").
- **Determinism.** `NavDeterminismTest.CrowdReplay` (`NavCrowdTest.cpp:236-263`): 2 runs × 600
  ticks × 50 agents, positions compared with `memcmp`.

## 4. Reference

| Source (licence) | Take | Improve / note |
|---|---|---|
| `Detour/Include/DetourNavMeshQuery.h` (zlib) | `findRandomPointAroundCircle`, `findDistanceToWall`, `moveAlongSurface`, `queryPolygons`, `findStraightPath` options `DT_STRAIGHTPATH_AREA_CROSSINGS`, `raycast` with `dtRaycastHit` + `DT_RAYCAST_USE_COSTS` | `findRandomPoint` picks a tile uniformly, then a polygon by area: not uniform over the mesh. We write our own area-weighted pick (D2). `frand` is a context-free `float (*)()`: we bind it to a thread-local stream pointer for the call |
| `Detour/Include/DetourNavMesh.h` (zlib) | `setPolyFlags`, `setPolyArea`, `getTileStateSize`, `storeTileState`, `restoreTileState`, `getOffMeshConnectionPolyEndPoints` | Tile state is per tile ref (salt checked): a swapped tile needs a new snapshot (D5) |
| `DetourCrowd/Include/DetourPathCorridor.h`, `DetourCrowd.h` (zlib) | `dtPathCorridor::setCorridor`, `movePosition`, `isValid`; `dtCrowd::requestMoveVelocity`; `dtCrowdAgent` target fields (public struct) | Corridor capacity is 256 (`DetourCrowd.cpp:421`); a shorter corridor whose last polygon is not the target is replanned by dtCrowd after `TARGET_REPLAN_DELAY` (1 s) when fewer than 10 polygons remain (`DetourCrowd.cpp:1029-1031`) |
| `Recast/Source/RecastArea.cpp` `rcMarkConvexPolyArea` (zlib) | The span loop over a convex prism | Our boundary marker (D4) keeps each span's area and only moves it to a "runtime volume" id range |
| Unity AI Navigation 2.0 manual and scripting reference (docs only) | Member names: `NavMeshAgent.SetPath`, `CalculatePath`, `path`, `steeringTarget`, `isPathStale`, `nextPosition`, `velocity`, `Raycast`, `FindClosestEdge`, `SamplePathPosition`, `nextOffMeshLinkData`, `SetAreaCost`; `NavMeshObstacle.carving`, `carvingMoveThreshold`, `carvingTimeToStationary`; `NavMeshLink.activated`, `occupied`, `autoUpdate`, `UpdateLink` | Names only, no code |

No code is copied. The boundary marker is our own loop modelled on `rcMarkConvexPolyArea`; if an
implementer adapts upstream lines, the file keeps the zlib notice and gets a `PATCHES.md`-style
note in `engine/libs/recastnavigation/README.jasper.md` ("adapted outside the library").

## 5. Requirements

### Functional — queries (package P1)

- **R1 Random point.** `NavigationSystem::randomPoint(out, filter, query)` returns a navmesh point
  of the filter's agent type (surface choice as the other queries: the first surface of the agent
  type that answers, entity order; `randomPointOn(surface, ...)` for one surface):
  - *Anywhere* (no center): uniform by area over every polygon that passes the filter's flags,
    across all tiles (D2). Polygons with flags 0 (runtime Not Walkable, deactivated links) never.
  - *Around* (`center`, `radius > 0`): Detour's `findRandomPointAroundCircle` from the polygon under
    the center (query extents of the agent type). The result is connected to the center through
    the filter and lies within `radius` (distance in the surface's plane): up to 8 draws, then
    `false`. Documented: **not uniform** (polygons the search reaches, weighted by area).
  - *Same island* (`sameIslandAs` point): only polygons whose S54 island label equals the label of
    the polygon under that point; none under it gives `false`. Combines with *around*. It is an
    explicit island use, so it relabels a stale navmesh first (S54 R8, R21: <= 1.5 ms desktop once
    per revision); the other variants never relabel.
  - Off-mesh links are never returned. `out` is a `NavHit` (position, normal, area, `hit = true`).
- **R2 Seeding.** Draws come from `nav::NavRandom` (splitmix64, D2). Each `NavigationSystem` owns one
  stream seeded with `NavigationSystem::DEFAULT_RANDOM_SEED` (`0x5EED`) at construction and by
  `setRandomSeed(seed)`; a new scene or Play starts at the default. `query.seed` uses a private
  stream seeded with it and leaves the system stream untouched. Same seed, mesh, filter and calls →
  bit-identical points on one platform/compiler. An *anywhere* draw consumes exactly 3 values.
- **R3 Closest edge within a distance.** `findClosestEdge(point, out, filter, maxDistance)`:
  `maxDistance > 0` searches only that far and returns `false` (with `out.distance = maxDistance`,
  `out.hit = false`) when no wall is within it; `<= 0` or omitted keeps S19's reach. Results:
  distance, wall position, normal (from the wall towards the point), area under the point.
- **R4 Move along the surface.** `moveAlongSurface(from, to, out, filter)`: from the polygon under
  `from` (query extents), Detour's `moveAlongSurface` towards `to` with at most 64 visited polygons;
  `out.position` gets the polygon height there, `out.hit` is true when the end is more than 1 mm
  (2D) from `to` (a wall or the cap stopped it), `out.distance` the 2D distance moved, `out.area`
  the end polygon's area. `from` off the mesh → `false`, nothing moved. For per-tick controller
  steps; documented that long moves stop at the 64-polygon cap.
- **R5 Path areas and cost.** `NavPath` gains (D3): `cornerAreas` (per corner, the area of the
  stretch that starts there; the last corner: the area at the end), `cornerFlags` (Detour's
  `DT_STRAIGHTPATH_START/END/OFFMESH_CONNECTION` values), `cost` (Σ metres × the filter's area
  cost, links at their own cost per metre), `areaLengths` (metres per area) and, engine-internal,
  `polys` and `meshRevision` (S54's `surface` names the surface). Cost and lengths are computed on the area-crossing straight
  path whatever the options; `PathOptions::areaCrossings` also returns the crossing points as
  corners. A link's stretch has the link's area. Partial paths price what they cover.
- **R6 Raycast cost.** `raycast` switches to the `dtRaycastHit` overload with
  `DT_RAYCAST_USE_COSTS`; `NavHit::cost` holds the cost of the travelled part. Hit, position,
  normal, distance and area keep S19's meaning (`NavQueryTest.Raycast` unchanged).
- **R7 Everywhere.** R1-R6 work in edit mode and in play, on XZ and XY (2D) surfaces through
  `NavFrame`, and with `JASPERENGINE_NAVIGATION=OFF` fail and log once (`NavStubTest`).

### Functional — area costs (P1)

- **R8 Per-query costs.** `QueryFilter::setAreaCost(area, cost)`, `getAreaCost(area)` (the filter's,
  else the project's), `resetAreaCost(area)`. Costs are clamped to >= 1 (A*'s heuristic needs it;
  one warning per process). Excluding an area is clearing its `areaMask` bit (an area is a flag;
  Detour's exclude flags stay 0). Every query and `calculatePath` honours the filter's costs.
- **R9 Per-agent costs.** `NavMeshAgent:setAreaCost/getAreaCost/resetAreaCost` (Unity): runtime
  only, not saved, cleared at Stop. They apply to the agent's crowd planning, `agent:calculatePath`,
  `samplePathPosition`, `raycast` and `findClosestEdge`. Areas without an override follow the
  project's costs, including later project edits (`refreshFilters`).
- **R10 Crowd filters.** A crowd's 16 filters are keyed by `(areaMask, 16 effective costs)`,
  reference-counted and freed when no agent uses them. A key that finds no free slot uses, in
  order, a slot with the same mask (area restrictions kept, costs approximate), else slot 0 (S19's
  behaviour); one warning per crowd naming the agent, and `NavCrowd::getFilterOverflowCount()`
  (agents on a fallback now) for S54's `crowd_filter_overflow` finding.

### Functional — runtime areas and links (P2)

- **R11 Runtime modifier volumes.** NavMesh Modifier gets `applyAtRuntime` (default false). With
  `useVolume` and `applyAtRuntime`:
  - the bake marks only the volume's boundary (areas kept, polygons split along it, D4), whether the
    component is enabled or not;
  - while the component is enabled and its GameObject active, the polygons whose centroid lies in
    the volume take the modifier's `area` (always `area`; `overrideArea` is for bake volumes), in
    edit mode and in play; Not Walkable gives flags 0;
  - a moved, rotated or resized volume (> 1 mm, > 0.1°) is applied again at the next sync (polygons
    then follow the centroid rule, not the baked boundary).
- **R12 Script volumes.** `NavigationSystem::setAreaInVolume(center, size, rotation, area, agentType)`
  returns a handle (`> 0`; `agentType < 0`: every surface it overlaps); `updateAreaOverride` moves
  it or changes its area, `removeAreaOverride` ends it. Handles die with the system (Stop).
  Polygons follow the centroid rule (no baked boundary).
- **R13 Composition.** Overrides apply in a fixed order: modifier volumes by entity id, then script
  handles by id; a later one wins, Not Walkable always wins. Removing the last override of a tile
  restores its baked flags and areas byte for byte.
- **R14 Tile changes.** Overrides and link states are re-applied inside every operation that adds
  or rebuilds tiles: `NavMesh::create`, `replaceTiles` (incremental bake, `updateTiles`), carving
  rebuilds (`updateCarving`), `updateLink`, a full bake landing, `setNavMeshData`; a query after
  the call already sees them.
- **R15 Revision and replan.** Every applied change bumps the NavMesh revision (S54 islands and the
  overlays recompute). At the next fixed tick, agents of that surface whose whole corridor (not
  just dtCrowd's 10-polygon lookahead) contains a changed polygon, and agents with a Partial path
  or a Failed target, request their destination again. With `autoRepath` off a path crossing a
  newly blocked polygon is dropped and `onPathFailed` fires (reason per R27).
- **R16 Link activated.** NavMesh Link gets `activated` (default true; Unity 2.0). False sets the
  flags of all its connections to 0: paths and agents ignore it, an agent already on it finishes.
  It is not a bake input (toggling never dirties a tile) and applies in edit mode too.
- **R17 Link occupied.** Read-only: true while an agent traverses one of the link's connections
  (between its `onOffMeshLinkStart` and `onOffMeshLinkEnd`).
- **R18 Zero cost.** Without overrides and with every link activated, no tile state is stored, the
  tile-change hook is one empty check and no per-tick work is added.

### Functional — agent API (P3)

- **R19 `calculatePath(target)`** → a path (`NavPath`; in Lua the path table): synchronous, on the agent's surface, from its
  simulation position, with its mask and costs; the target snaps as `setDestination` does (R12 of
  S19). Not placed → status Invalid. It does not change the agent's path.
- **R20 `setPath(path)`** → bool. Accepted when the path is not Invalid, was planned on the agent's
  surface, its polygons are still valid for the agent's filter (checked only if `meshRevision`
  changed), and the agent's current polygon is among its first 16 polygons (earlier ones are
  dropped). Then the corridor is set (`dtPathCorridor::setCorridor`, target state VALID,
  `pathPending` false, `pathStatus` the path's, destination its last corner). A path of more than
  256 polygons gives the corridor its first 256 with the real end as target; dtCrowd plans the rest
  near its end (documented). Refused → false and the path is cleared (Unity).
- **R21 Path state.** `path` (the corridor as a path, computed on read), `steeringTarget`
  (the next corner the crowd steers to; the position when it has no path), `isPathStale` (the
  corridor holds a polygon that is no longer valid or passable for its filter; false while
  `pathPending`).
- **R22 Root motion.** `nextPosition = p` moves the simulation position along the surface
  (`NavCrowd::movePosition`, path kept); `velocity = v` sets the velocity of the next fixed tick only:
  with a path the agent moves `v · dt` along the surface after the crowd step and reports `v`; without
  a path it uses `requestMoveVelocity` for that tick (avoidance applies). Both work with
  `updatePosition = false` and the None driver; neither writes the Transform.
- **R23 Agent queries.** `raycast(target)` and `findClosestEdge()` from its position with its filter;
  `samplePathPosition(areaMask, maxDistance)` walks its path up to `maxDistance` and returns `true`
  with the point where the path first enters an area outside `areaMask`, else `false` with the
  point at `maxDistance` (or the path end); the hit carries area and mask (Unity semantics).
- **R24 `nextOffMeshLinkData`**: the next link along its path within 64 corners, same shape as
  `currentOffMeshLinkData` plus `activated` (the link's R16 state, no longer `valid`) and `occupied`.

### Functional — stuck and failures (P3)

- **R25 Stuck watchdog.** New agent fields: `stuckDetection` (false), `stuckSpeedRatio` (0.1),
  `stuckTimeout` (2 s), `repathWhenStuck` (true). While the agent has a path, is placed, not
  pending, not stopped, not on a link and `remainingDistance > stoppingDistance + radius`, it keeps
  an anchor: once it is farther than `stuckSpeedRatio × speed × stuckTimeout` from the anchor, the
  anchor moves there and `stuckTime` restarts; otherwise `stuckTime += dt` (so back-and-forth
  jitter counts as stuck). When `stuckTime >= stuckTimeout`: `onStuck(agent)` fires, the agent
  requests its destination again when `repathWhenStuck`, and `stuckTime` restarts. Outside those
  conditions `stuckTime` is 0. Off: no anchor, no work beyond one flag test.
- **R26 `stuckTime`** is read-only (Lua, Inspector in play, MCP).
- **R27 Failure reason.** `onPathFailed` becomes `FunctionSubscribe<void(NavMeshAgent, nav::PathFailure)>`
  and `lastPathFailure` is read-only (None after a later success or `resetPath`). Reasons:
  no surface → `NoSurface`; surface not baked → `NotBaked`; `requestMoveTarget` refused →
  `EndOffMesh`; the crowd target Failed, a Partial/Invalid path, or a path dropped by a replan
  without `autoRepath` → S54's `NavPath::failure` of one `calculatePathOn(surface, position,
  destination, filter)` at that event (S54 classifies inside every path query; it reports
  `BlockedByArea` for R11-R16 blocks); grid agents: the reason S54 gives grid answers. In Lua the
  reason and `lastPathFailure` are S54's snake_case names (`"blocked_by_area"`, `"none"`: S54 D4).
- **R28 Compatibility.** A one-argument Lua `onPathFailed` keeps working (Lua drops the extra
  argument). C++ subscribers take the reason (the one breaking change, R49).
- **R29 Events.** `onStuck` is queued like the others and dispatched after the step in entity order
  (S19 R38); within an entity, in queue order.

### Functional — Lua handles and names (P2)

- **R30 `NavMeshObstacle(scene, entity)`:** `enabled`, `shape` (`NavMeshObstacleShape.CAPSULE/BOX`),
  `center`, `size`, `radius`, `height`, `carving`, `carveOnlyStationary`, `carvingMoveThreshold`,
  `carvingTimeToStationary` (r/w); `velocity` (r, from its transform per fixed tick).
- **R31 `NavMeshLink(scene, entity)`:** `enabled`, `agentTypeID`, `startPoint`, `endPoint`, `width`,
  `costModifier`, `bidirectional`, `area`, `autoUpdate`, `activated` (r/w); `occupied` (r);
  `updateLink()` re-places its connections from the current fields through S53's link update.
- **R32 `NavMeshModifier(scene, entity)`:** `enabled`, `area`, `center`, `size` (r/w; a runtime
  volume re-applies); `useVolume`, `applyAtRuntime`, `overrideArea`, `applyToChildren` (r: bake
  inputs); `appliedPolygons` (r).
- **R33 Name policy.** Lua members use Unity's scripting names, the Inspector Unity's labels; C++
  fields and serialized keys never change (no migration). One name per member, no aliases. The
  mapping is documented: obstacle `carve` → `carving`, `moveThreshold` → `carvingMoveThreshold`,
  `timeToStationary` → `carvingTimeToStationary`; link `autoUpdatePosition` → `autoUpdate`,
  `agentType` → `agentTypeID`; agent `obstacleAvoidance` → `obstacleAvoidanceType`; surface
  `agentType`/`includeLayers` → `agentTypeID`/`layerMask` (those three already shipped).

### Platform & backends

- **R34** CPU-only; every export target; single-threaded web unchanged (all new work runs on the
  main thread in the fixed tick or in the caller's query).
- **R35 Determinism.** `NavDeterminismTest.CrowdReplay` stays bit-identical. Overrides, link states,
  per-agent costs, `setPath`, root-motion setters and the watchdog act only on the main thread; their
  crowd effects land in the fixed tick in ascending entity order. A replay using all of them is
  bit-identical across two runs (AC31).

### Performance (release; `bench_nav_city`; mobile and web as S19 §5)

- **R36 Queries** (p95; desktop / mobile / web):

  | Query | Budget |
  |---|---|
  | `randomPoint` anywhere, table cached | 0.005 / 0.015 / 0.02 ms |
  | `randomPoint` anywhere, first call (builds the area table, ~20k polys) | 0.5 / 1.5 / 2.0 ms |
  | `randomPoint` around, radius 10 m | 0.05 / 0.15 / 0.2 ms |
  | `findClosestEdge`, maxDistance 5 m | 0.03 / 0.1 / 0.12 ms |
  | `moveAlongSurface`, step <= 2 m | 0.01 / 0.03 / 0.04 ms |
  | `calculatePath` with cost and corner areas | within S19's 0.15 ms desktop (gate 0.165) |
  | `raycast` with cost | within S19's 0.02 ms desktop (gate 0.022) |

- **R37 Overrides.** Toggling a door volume over <= 4 tiles, apply + replan scan of 100 agents:
  <= 0.2 / 0.6 / 0.8 ms (the replans themselves go through dtCrowd's capped path queue). Re-applying
  after a tile swap: <= 0.05 ms per tile per override (desktop).
- **R38 Agents.** 100 agents with stuck detection add <= 0.01 ms per tick (desktop); per-agent costs
  add nothing per tick (filter assignment on change, O(16)); `crowd_100_tick_ms` and
  `crowd_500_tick_ms` gates unchanged.
- **R39 Memory.** Area tables: 8 B per polygon per cached (surface, mask, island), at most 4 per
  surface, dropped at a revision change. Override base state: `getTileStateSize` (3 B per polygon +
  header) per touched tile. Nothing for scenes that do not use them.

### Editor UX (human)

- **R40 Inspector** (Unity labels, `PropertyCmd` undo, multi-edit as every row):
  - NavMesh Agent: a **Stuck Detection** foldout: Detect Stuck, Stuck Speed Ratio, Stuck After (s),
    Repath When Stuck; in play, read-only Stuck Time, Last Path Failure, Path Stale, and the area
    cost overrides when any.
  - NavMesh Link: **Activated**; in play, read-only Occupied.
  - NavMesh Modifier: **Apply At Runtime** under Volume (shown with Volume on); with it on, Area
    Type is shown whatever Override Area says; in play, read-only Applied Polygons.
- **R41 Scene view.** The AI Navigation overlay gets **Show Area Overrides** (default on, saved with
  the workspace): runtime modifier volumes always (not only when selected) and script volumes in
  play, as `addBox` outlines in the area colour, solid when applied, at 35 % alpha when not;
  deactivated links drawn at 35 % alpha; an occupied link highlighted.

### Agent UX (MCP, Lua, CLI)

- **R42 `query_navigation`**: ops gain `random` and `move_along`; `from` is required per op (not by
  the schema): random without `from` → anywhere, with `from` and `max_distance` → around, with
  `from` only → on `from`'s island; new `seed`. `closest_edge` honours `max_distance`. Path results
  gain `cost`, `corner_areas`, `corner_flags`, `area_lengths` (by area name); raycast gains `cost`.
  Per-query area costs stay a Lua feature (`eval_lua`), to keep the tool in budget.
- **R43 `get_navigation_info`**: links gain `activated` (and `occupied` in play) beside S54's link
  fields; S54's `modifiers[]` items gain `apply_at_runtime`; a new `area_overrides` list; agents in play gain `last_failure` (`pathFailureName`, `"none"`) and
  `stuck_seconds` (with stuck detection on), in every agent item (two scalars, no flag: S54 defines
  no `agent_detail` switch). With S54's `entities` parameter, `area_overrides` lists only those
  GameObjects' volumes (script handles are left out).
- **R44 `get_runtime_entity`**: agent `stuckTime`, `lastPathFailure`, `isPathStale`,
  `steeringTarget`; link `occupied`; modifier `appliedPolygons`; obstacle `velocity`.
- **R45 Toggles without a new tool.** Agents close doors and links in play with
  `set_runtime_property` (`navmeshmodifier.enabled`, `navmeshlink.activated`); the system samples
  component state at its next sync. `visualize_navigation.show` gains `area_overrides`.
- **R46 Docs.** `docs/navigation.md` (sections Random points, Area costs, Doors and bridges,
  Scripted paths and root motion, Stuck agents, Script names), `docs/mcp/guide/navigation.md`
  (recipes: toggle a door in play and check the path; read why a path failed), `TOOLS.md`
  regenerated, `docs/mcp/guide/lua-api.md` navigation lines, the hand-written stub
  (`stubs/Navigation.lua`): `NavMeshPath` gains R5's fields and `corridor`, `NavMeshHit` gains `cost`.

### Data & compatibility

- **R47 Fields.** New serialized fields with defaults: agent `stuckDetection = false`,
  `stuckSpeedRatio = 0.1`, `stuckTimeout = 2.0`, `repathWhenStuck = true`; link `activated = true`;
  modifier `applyAtRuntime = false`. Missing keys load as defaults; the hand codecs write them (S19's
  every-key rule), the known-key lists, `FactoryNavigation.cpp` codegen and
  `editor/tests/reflect/catalog_snapshot.json` are updated. No scene version bump (S03 rule for
  optional fields, S19 Deviations).
- **R48 No `.navmesh` change.** Polygon areas stay 0..15 (links 16..63); `NavMeshFile::VERSION`
  stays 1. Tile input hashes include runtime volumes only when present, so tiles of scenes without
  them hash as before (no dirty tiles after the upgrade, AC13).
- **R49 Script compatibility.** Lua paths stay tables (S54 adds `reason`, `surface`,
  `closestPoint`, `gap` to the same table); S55 adds `cost`, `cornerAreas`, `cornerFlags`
  (arrays aligned with `corners`), `areaLengths` (keyed by area index 0-15, present when > 0) and
  `corridor` (an opaque `NavMeshPathCorridor` userdata holding polys, revision and surface, read by
  `setPath`; nil on grid paths). Only keys are added. `onPathFailed` C++ subscribers need the
  reason parameter (one line per subscriber; release note).

## 6. Design constraints & decisions

- **D1 Layer.** Queries stay in `NavMesh` (nav space) with world wrappers in `NavigationSystem`, as
  S19 D1. The poly-state layer (overrides, link states, base snapshots) is its own internal class
  `nav::detail::NavPolyState` (`engine/core/navigation/NavPolyState.{h,cpp}`) owned by
  `NavMesh::Impl`, created on first use (R18). The system-side volume sync lives in
  `engine/core/subsystem/NavigationAreas.cpp` with its own state struct, so P2 touches
  `NavigationSystem.cpp` only for the hook calls.
- **D2 Random.** `nav::NavRandom` (`NavRandom.h`, header only): splitmix64, as `SplineJitter.h` and
  `PoseSampler.h` already use; `nextFloat()` = top 24 bits × 2⁻²⁴ ∈ [0, 1). *Anywhere*: a cached
  table of passable polygons (refs + cumulative triangle-fan areas, keyed by mask, island and
  revision); draw 1 picks a polygon by binary search, draws 2-3 feed `dtRandomPointInConvexPoly`.
  *Around*: Detour's function with `frand` reading a `thread_local NavRandom*` set for the call
  (queries are main-thread). Islands are S54's: `NavMesh::getIslands()` (relabels when stale),
  `areIslandsCurrent()`, `int getIslandOf(uint32_t polyRef) const` (-1: none).
- **D3 Paths.** `NavMesh::findPath` keeps the polygon corridor, calls `findStraightPath` once for
  corners (with flags and refs) and once with `DT_STRAIGHTPATH_AREA_CROSSINGS` for cost and area
  lengths (a funnel over at most 2048 polygons; measured against R36). A polygon's reported area
  is the index of its flag bit (links report their area; flags 0 reports Not Walkable).
- **D4 Runtime volume boundaries.** At bake, a runtime volume runs our marker over the compact
  heightfield after the bake-time volumes: a walkable span with Recast area `r` in 1..16 inside the
  prism becomes `r + 32` (ids 33..48 are unused by Recast and the collector). Region building then
  splits polygons along the volume; `polyAreaAndFlags` (static tiles and the tile cache's
  `MeshProcess`) maps 33..48 back to area `r - 1`, so stored areas are unchanged (R48).
  `NavConvexVolume` gains `boundaryOnly`, hashed only when true.
- **D5 Poly state.** Per touched tile (keyed by tile ref, salt included) `NavPolyState` keeps the
  `storeTileState` snapshot taken before its first change. Any change to the overrides of a tile:
  `restoreTileState` the snapshot, then apply every override touching it in order (R13), then link
  states. After `NavMesh` adds or rebuilds tiles (R14) it calls `NavPolyState::onTilesChanged()`,
  which drops snapshots of refs that no longer exist and re-applies to tiles whose ref changed.
  Changed polygon refs collect in a list the system takes each fixed tick (`takeChangedPolys`).
  Not Walkable → flags 0 + area `AREA_NOT_WALKABLE`; another area → flags `1 << a`, area `a` (link
  polygons keep their link-cost polygon area, only flags change). Single changes go through S54's
  `NavMesh::setPolyFlags` (it bumps the revision); a tile's restore-and-reapply bumps once.
- **D6 Crowd filters.** `NavCrowdAgentParams` gains `areaCosts[16]` (effective costs);
  `NavCrowd::Impl` keeps per slot `{mask, costs, refCount}`; `setParams`/`removeAgent` release.
  Project cost edits rebuild effective costs of agents without an override at `refreshFilters`.
- **D7 Agent corridor.** `NavCrowd` gets corridor access (`setCorridor`, `getCorridor`,
  `isCorridorValid`, `replanCrossing`, `overrideVelocity`, `requestMoveVelocity`); the velocity
  override is applied after `dtCrowd::update` (corridor `movePosition` to `pos + v·dt`, `vel = v`),
  so no Detour patch is needed (PATCHES.md unchanged).
- **D8 Event payloads.** `Event` gains `uint8_t detail` (the `PathFailure`); `EventType` gains
  `Stuck`. Lua callbacks get `pathFailureName(reason)` (S54's strings; no Lua enum).
- **D9 Edit mode.** The editor calls `NavigationSystem::syncAreaOverrides()` every frame next to
  `processBakes`/`updateGridSources`, so Inspector toggles show in edit-mode queries and overlays;
  `fixedUpdate` calls it in play as step 1b (after bake tiles land, before agents sync).

C++ sketch (exact names, grouped by owning package, §10; S54's `NavPath` fields coexist):

```cpp
namespace jasperengine::nav {
    // ---- P1: NavTypes.h, NavRandom.h, NavMesh.h
    enum CornerFlag : uint8_t { CORNER_START = 1, CORNER_END = 2, CORNER_OFF_MESH_LINK = 4 };  // Detour's values
    struct QueryFilter { int agentType = 0; uint16_t areaMask = ALL_AREAS;
        uint16_t costMask = 0; std::array<float, AREA_COUNT> areaCosts{};      // costs where costMask has the bit
        void setAreaCost(int area, float cost); float getAreaCost(int area) const; void resetAreaCost(int area); };
    struct NavHit { Vector3 position, normal; float distance = 0; uint8_t area = 0; bool hit = false; float cost = 0; };
    struct NavPath { PathStatus status = PathStatus::Invalid; std::vector<Vector3> corners; float length = 0;
        std::vector<uint8_t> cornerAreas, cornerFlags; float cost = 0; std::array<float, AREA_COUNT> areaLengths{};
        std::vector<uint32_t> polys; uint64_t meshRevision = 0; };   // after S54's fields; polys, revision: engine
    struct PathOptions { bool areaCrossings = false; };
    struct RandomPointQuery { bool around = false; Vector3 center; float radius = 0;
        bool sameIsland = false; Vector3 islandOf; bool seeded = false; uint64_t seed = 0; };
    class NavRandom { public: explicit NavRandom(uint64_t seed = 0x5EED); uint64_t next(); float nextFloat(); uint64_t state; };
    struct NavFilter { /* as today */ static NavFilter fromQuery(const QueryFilter& filter); };
    // class NavMesh, additions (nav space)
    NavPath findPath(const Vector3& start, const Vector3& end, const NavFilter&, const Vector3& halfExtents, const PathOptions& = {}) const;
    NavHit findClosestEdge(const Vector3& point, const NavFilter&, const Vector3& halfExtents, float maxDistance = -1.0f) const;
    bool findRandomPoint(const NavFilter&, NavRandom&, uint32_t island, NavHit& out) const;          // island 0: any
    bool findRandomPointAround(const Vector3& center, float radius, const NavFilter&, const Vector3& halfExtents, NavRandom&, NavHit& out) const;
    bool moveAlongSurface(const Vector3& from, const Vector3& to, const NavFilter&, const Vector3& halfExtents, NavHit& out) const;
    // class NavCrowd: int getFilterOverflowCount() const;  NavCrowdAgentParams gains float areaCosts[AREA_COUNT]
    // ---- P2: NavMesh, NavCrowd
    uint32_t addAreaOverride(const NavConvexVolume& volume, uint8_t area);   // nav-space prism, id > 0
    bool updateAreaOverride(uint32_t id, const NavConvexVolume& volume, uint8_t area);
    bool removeAreaOverride(uint32_t id);  int getAreaOverridePolyCount(uint32_t id) const;
    void setLinkActive(uint32_t userId, bool active);  bool isLinkActive(uint32_t userId) const;
    void takeChangedPolys(std::vector<uint32_t>& out);
    void NavCrowd::replanCrossing(const std::vector<uint32_t>& changedPolys, std::vector<int>& replanned);
    // ---- P3: NavCrowd
    bool setCorridor(int agent, const std::vector<uint32_t>& polys, const Vector3& target);
    void getCorridor(int agent, std::vector<uint32_t>& polys) const;  bool isCorridorValid(int agent) const;
    void overrideVelocity(int agent, const Vector3& velocity, float dt);  void requestMoveVelocity(int agent, const Vector3& velocity);
}
namespace jasperengine {   // class NavigationSystem, additions
    // ---- P1
    static constexpr uint64_t DEFAULT_RANDOM_SEED = 0x5EED;
    bool randomPoint(nav::NavHit& out, const nav::QueryFilter& = {}, const nav::RandomPointQuery& = {});
    bool randomPointOn(Entity surface, nav::NavHit& out, const nav::QueryFilter& = {}, const nav::RandomPointQuery& = {});
    void setRandomSeed(uint64_t seed);  uint64_t getRandomSeed() const;
    bool findClosestEdge(const Vector3& point, nav::NavHit& out, const nav::QueryFilter& filter, float maxDistance);
    bool moveAlongSurface(const Vector3& from, const Vector3& to, nav::NavHit& out, const nav::QueryFilter& = {});
    nav::NavPath calculatePath(const Vector3& from, const Vector3& to, const nav::QueryFilter&, const nav::PathOptions&);
    void setAgentAreaCost(Entity agent, int area, float cost);  float getAgentAreaCost(Entity agent, int area) const;
    void resetAgentAreaCost(Entity agent, int area);
    // ---- P2
    uint32_t setAreaInVolume(const Vector3& center, const Vector3& size, const Quaternion& rotation, uint8_t area, int agentType = -1);
    bool updateAreaOverride(uint32_t handle, const Vector3& center, const Vector3& size, const Quaternion& rotation, uint8_t area);
    bool removeAreaOverride(uint32_t handle);  void syncAreaOverrides();
    bool isLinkOccupied(Entity link) const;  Vector3 getObstacleVelocity(Entity obstacle) const;
    // ---- P3
    nav::NavPath calculateAgentPath(Entity agent, const Vector3& target);  bool setAgentPath(Entity agent, const nav::NavPath& path);
    nav::NavPath getAgentCorridorPath(Entity agent);  Vector3 getSteeringTarget(Entity agent);  bool isAgentPathStale(Entity agent);
    void setAgentNextPosition(Entity agent, const Vector3& position);  void setAgentVelocity(Entity agent, const Vector3& velocity);
    bool agentRaycast(Entity agent, const Vector3& target, nav::NavHit& out);  bool agentFindClosestEdge(Entity agent, nav::NavHit& out);
    bool samplePathPosition(Entity agent, uint16_t areaMask, float maxDistance, nav::NavHit& out);
    bool getNextOffMeshLink(Entity agent, Vector3& start, Vector3& end, Entity& link);
}
```

Lua (`NavigationClassesLua.cpp`; handles in a new `NavigationComponentsLua.cpp`):

```lua
local nav = scene:getNavigationSystem()
nav.randomSeed = 1234
local found, hit = nav:randomPoint()                                         -- anywhere (uniform by area)
local found, hit = nav:randomPoint(filter, {center = p, radius = 10})        -- reachable within 10 m (not uniform)
local found, hit = nav:randomPoint(filter, {sameIslandAs = p, seed = 7})
local found, edge = nav:findClosestEdge(p, filter, 5.0)                      -- false: no wall within 5 m
local ok, move = nav:moveAlongSurface(from, to, filter)                      -- move.position, move.hit (blocked), move.distance
local path = nav:calculatePath(a, b, filter, {areaCrossings = true})         -- a table, as S19/S54
print(path.status, path.length, path.cost, path.cornerAreas[1], path.cornerFlags[1], path.areaLengths[3])
local filter = NavMeshQueryFilter{agentType = "Humanoid", areaMask = NavAreas.All}
filter:setAreaCost(NavAreas.getAreaFromName("Mud"), 5) ; filter:getAreaCost(3) ; filter:resetAreaCost(3)
local door = nav:setAreaInVolume(center, Vector3(2, 3, 0.5), NavAreas.getAreaFromName("Not Walkable"), {rotation = q})
door.area = 0 ; door:remove() ; print(door.valid)                            -- NavAreaOverride
local agent = NavMeshAgent(self.scene, self.entity)
local p = agent:calculatePath(target) ; if agent:setPath(p) then end
local corridor = agent.path ; local next = agent.steeringTarget ; local stale = agent.isPathStale
agent.updatePosition = false ; agent.nextPosition = rootPosition ; agent.velocity = rootVelocity
local blocked, hit = agent:raycast(target) ; local found, edge = agent:findClosestEdge()
local blocked, hit = agent:samplePathPosition(NavAreas.Walkable, 5.0)
local link = agent.nextOffMeshLinkData                                       -- {valid, activated, occupied, startPos, endPos, link}
agent:setAreaCost(3, 10) ; agent.stuckDetection = true
agent.onStuck = function(a) print("stuck for", a.stuckTime) end
agent.onPathFailed = function(a, reason) print(reason, a.lastPathFailure) end    -- "blocked_by_area"
local link = NavMeshLink(self.scene, bridge) ; link.activated = false ; print(link.occupied)
local obstacle = NavMeshObstacle(self.scene, crate) ; obstacle.carving = true
local modifier = NavMeshModifier(self.scene, doorway) ; modifier.enabled = false   -- the door opens
```

MCP shapes (`query_navigation`; `get_navigation_info` additions):

```json
{"op": "path", "status": "complete", "corners": [[0,0,0],[6,0,2]], "length": 6.32, "cost": 14.1,
 "corner_areas": [3, 0], "corner_flags": [1, 2], "area_lengths": {"Walkable": 2.1, "Mud": 4.22}}
{"op": "random", "found": true, "position": [3.2, 0, -1.5], "area": 0, "seed": 42}
{"op": "move_along", "found": true, "position": [1.0, 0, 0.4], "blocked": true, "distance": 1.08, "area": 0}
{"links": [{"entity": {"id": 12, "path": "Bridge"}, "activated": false, "occupied": false}],
 "area_overrides": [{"source": {"entity": {"id": 9, "path": "Door"}}, "area": 1, "applied": true, "polygons": 3},
                    {"source": {"handle": 2}, "area": 4, "applied": true, "polygons": 11}],
 "agents": [{"entity": {"id": 5}, "last_failure": "blocked_by_area", "stuck_seconds": 1.2}]}
```

New `query_navigation` inputs: op enum + `random`, `move_along`; `seed` (integer 0..4294967295);
`from` leaves the schema's required list; `max_distance` describes sample, closest_edge, random.
Budget estimate (inferred from the schema helpers, not measured): `query_navigation` is about
1.32k of its 1500 today and S55 adds about 80 (the description lists ops only, detail in
`jasper://docs/navigation`); S54's additions come on top (§9). `get_navigation_info` +60,
`visualize_navigation` +30; the group stays far under 15000; nothing enters core; no new tool.
`tool_budget.py --check` is the gate: over budget, text moves to the guide first; only a merged
S54 + S55 tool that still exceeds gets an exception (measured + 5 %, both specs named).

## 7. Acceptance criteria

Unit tests in `engine/tests/core/navigation/`: new suites in new files (`NavRandomTest`,
`NavPathTest`, `NavFilterTest` in `NavGameplayQueryTest.cpp` (P1); `NavAreaOverrideTest.cpp` (P2);
`NavAgentApiTest.cpp` (P3)); tests of existing suites go in the file that defines their fixture
(gtest needs one fixture class per suite): `NavQueryTest.*` in `NavQueryTest.cpp`,
`NavCrowdTest.*`, `NavSystemTest.*`, `NavDeterminismTest.*` in `NavCrowdTest.cpp`. Editor tests in
`editor/tests/navigation/NavigationEditorTest.cpp`; MCP tests in a new
`tools/mcp/tests/test_navigation_gameplay.py` (`CI = True`, own project). New fixture
`engine/tests/core/fixtures/navigation/nav_doors` (a corridor with a doorway runtime volume, a
bridge link, a mud strip of area 3, two islands). "Red" = the run on the tree without the change.

| AC | Verifies | Check |
|---|---|---|
| AC1 | R1, R2 | `NavRandomTest.SeededStreamRepeats`: two systems with the same seed give identical 100-point sequences (memcmp); an explicit `seed` call leaves the system stream's next value unchanged; `setRandomSeed` restarts it |
| AC2 | R1 | `NavRandomTest.UniformByArea`: a mesh whose tiles are 100 % and 25 % covered; 20k draws split by area (chi-square p > 0.001). Red: the same draws through Detour's `findRandomPoint` fail it |
| AC3 | R1 | `NavRandomTest.AroundCircleConnectedWithinRadius`: 1000 draws, all within radius and `isReachable` from the center; none on the other island; masked area never returned |
| AC4 | R1 | `NavRandomTest.SameIsland` (needs S54's labels): 1000 draws all share the island of the given point (`getIslandOf`); a point off the mesh → false; a door closed by R11 between two halves makes them two islands and draws stay on one |
| AC5 | R3 | `NavQueryTest.ClosestEdgeMaxDistance`: a wall 3 m away: maxDistance 2 → false, distance 2; 5 → true, distance 3 ± one cell, normal towards the point. Red: S19's call ignores the limit (true at 2) |
| AC6 | R4 | `NavQueryTest.MoveAlongSurfaceClampsAtWall`: a move through a wall ends on the wall (± one cell), `hit` true, height on the polygon; an open move reaches `to`, `hit` false; `from` off the mesh → false |
| AC7 | R5 | `NavPathTest.CornerAreasAndCost` on `nav_doors`: a path across the mud strip: `cost` = Σ len × cost (± 1e-3), `areaLengths[3]` = strip width ± one cell, `cornerFlags` start/end set; `areaCrossings` adds corners on both strip edges; a link path flags its link corner and prices the link at its cost |
| AC8 | R6 | `NavPathTest.RaycastCost`: a ray along the strip: `cost` = length × 4 (± 1e-3); hit and position equal `NavQueryTest.Raycast`'s expectations |
| AC9 | R8 | `NavFilterTest.PerQueryAreaCost`: a filter cost of 10 on the strip makes the path detour, `resetAreaCost` restores the straight one, a cost of 0.5 is used as 1 with one warning (`EXPECT_ENGINE_WARNINGS(1)`) |
| AC10 | R9, R10 | `NavCrowdTest.PerAgentAreaCosts`: an agent with `setAreaCost(3, 10)` detours, its neighbour without walks through. Two agents with equal costs share a filter slot (count 1); 17 distinct keys → overflow count 1 and the extra agent never enters an area outside its mask (red: S19's fallback walks it); after 16 agents leave, 16 new keys fit (red: S19 never frees a slot) |
| AC11 | R11, R13, R15 | `NavAreaOverrideTest.DoorBlocksAndOpens`: the doorway volume enabled with Not Walkable → `calculatePath` through it Partial or rerouted; an agent walking towards it replans in the next tick; disabled → the original path; the revision changes each time; removing restores `storeTileState` bytes equal to the pristine tile |
| AC12 | R11, D4 | `NavAreaOverrideTest.BakeBoundaryAlignsPolys`: after a bake with the runtime volume every polygon's vertices lie all inside or all outside the volume's footprint (± one cell); stored areas are 0..15 |
| AC13 | R48 | `NavAreaOverrideTest.HashesUnchangedWithoutRuntimeVolumes`: `nav_doors` without its runtime volume bakes the tile hashes pinned in `fixtures/navigation/nav_doors_hashes.txt`, recorded on the pre-S55 tree (`JASPER_TEST_UPDATE_FIXTURES=1`): the pre-S55 run is the control |
| AC14 | R14 | `NavAreaOverrideTest.SurvivesTileSwap`: with the door closed, an incremental bake of its tile, a carving obstacle rebuild of it, `updateLink` of a link in it and a full bake each leave the door closed (path still blocked right after the call) |
| AC15 | R12, R13 | `NavAreaOverrideTest.ScriptVolumeHandle`: `setAreaInVolume` blocks, a later Walkable handle over it does not unblock (Not Walkable wins), two non-blocking handles: the later area wins; `remove` restores; handles are gone after `destroy` |
| AC16 | R15 | `NavAreaOverrideTest.ReplanCrossingWholeCorridor`: an agent whose corridor crosses the door 40 polygons ahead requests a new path in the tick after the close (`pathPending` true). Red: without `replanCrossing` it keeps the old corridor for > 1 s |
| AC17 | R16, R17 | `NavAreaOverrideTest.LinkActivated`: deactivated bridge → path Partial; activated → Complete; deactivating while an agent crosses lets it finish; `occupied` true exactly between the link events; `countDirtyTiles` stays 0 after toggling |
| AC18 | R18, R38 | `NavAreaOverrideTest.ZeroCostWithoutOverrides` and `NavSystemTest.GameplayZeroCost`: counting allocator: 0 allocations per tick over 600 ticks with 50 agents (stuck detection off, no overrides) and none in a tile swap; no tile snapshot exists; `NavSystemTest.ZeroCostEmptyScene` unchanged |
| AC19 | R19, R20 | `NavAgentApiTest.CalculateAndSetPath`: a path along a costlier route the agent would not plan itself is followed (every tick within 0.1 m of the given corners' polyline) and arrives; `pathPending` false right after; a path not starting at the agent → false and `hasPath` false |
| AC20 | R20 | `NavAgentApiTest.SetPathLongerThan256Polys`: a 400-polygon path is accepted, the agent arrives, `onDestinationReached` once |
| AC21 | R21 | `NavAgentApiTest.CorridorPathAndStale`: `agent.path` corners equal `getPathCorners` (± 1e-4); `steeringTarget` is the first corner; a carving obstacle dropped on the corridor makes `isPathStale` true until the replan lands |
| AC22 | R22 | `NavAgentApiTest.RootMotion`: `updatePosition = false`, `nextPosition` set each tick along a line into a wall stops at the wall, the Transform never changes; `velocity = v` for one tick moves `v·dt` and the path continues next tick; without a path the agent moves with `v` that tick |
| AC23 | R23, R24 | `NavAgentApiTest.AgentQueries`: `samplePathPosition(Walkable, 20)` stops where the path enters the mud (± one cell) and returns true; with the mud in the mask false at 20 m; `raycast`/`findClosestEdge` equal the system queries with the agent's filter; `nextOffMeshLinkData.valid` before the bridge with the bridge entity |
| AC24 | R25, R26, R29 | `NavAgentApiTest.StuckFiresAndRepaths`: an agent whose path runs through a 1-agent-wide corridor blocked by a non-carving NavMesh Obstacle fires `onStuck` at 2 s ± 1 tick, re-requests, fires again 2 s later; an agent jittering in place (setVelocity alternating) is detected; with detection off it never fires and `stuckTime` stays 0 |
| AC25 | R27 | `NavAgentApiTest.PathFailedReason`: no surface → NoSurface; unbaked surface → NotBaked; destination on the other island → S54's Disconnected; door closed with `autoRepath` off → BlockedByArea; `lastPathFailure` back to None after a successful destination |
| AC26 | R28, R30-R33 | `test_navigation_gameplay.py` in play through `eval_lua`: `test_path_failed_callbacks`: a one-argument and a two-argument `onPathFailed` both run (Console markers), the reason is S54's snake_case name. `test_lua_component_handles`: the Obstacle/Link/Modifier handles read and write each member; `jasper://lua-api?class=NavMeshLink` (and the other new classes and members of R19-R32) lists them; `python3 -m lua_api doctest` passes on the new docs blocks; `test_navigation.py::test_lua_api_in_play` passes unchanged (R49: keys only added) |
| AC27 | R42 | `test_navigation_gameplay.py::test_query_ops`: random (seeded twice → same point; with `from` + `max_distance` within it), move_along blocked by a wall, closest_edge with `max_distance`, path `cost`/`corner_areas`/`area_lengths`; edit and play |
| AC28 | R43-R45 | `test_navigation_gameplay.py::test_door_and_bridge_in_play`: `set_runtime_property` modifier `enabled: false` then `true` and link `activated: false`: `query_navigation` path status follows, `get_navigation_info.area_overrides[].applied` and `links[].activated` follow, `get_runtime_entity` shows `appliedPolygons`, agent `lastPathFailure`/`stuckTime`; `tool_budget.py --check` passes; `TOOLS.md` regenerated (`test_docs.py`) |
| AC29 | R40, R47 | `NavigationEditorTest.OldSceneLoadsDefaults`: an S19 scene without the new keys loads with R47's defaults and saves them. `NavigationEditorTest.GameplayRowsUndo`: setting Activated, Apply At Runtime and Detect Stuck through the Inspector's `PropertyCmd` and undoing restores each; the catalog snapshot test passes with the new fields |
| AC30 | R41 | `NavigationEditorTest.OverlayDrawsAreaOverrides`: the overlay adds the runtime volume's 12 box lines in the area colour, alpha 1 enabled and 0.35 disabled; `test_navigation_gameplay.py::test_overlay_screenshot` returns a non-flat Scene view PNG with `show.area_overrides` |
| AC31 | R35 | `NavDeterminismTest.GameplayReplay` (P3, after P1 and P2; `NavCrowdTest.cpp`): 2 runs × 600 ticks × 50 agents with destinations from the system random stream, a door toggled at ticks 200 and 400, per-agent costs on 10 agents, stuck detection on, `setPath` for 5 agents → positions bit-identical and identical event logs (type, entity, reason). `NavDeterminismTest.CrowdReplay` still passes |
| AC32 | R36-R39 | `NavBench` (`engine/tests/core/bench/NavBench.cpp`) prints and `tools/perf/bench-thresholds.json` (`je-bench-nav`) gates `query_random_p95_ms` 0.0055, `query_random_first_ms` 0.55, `query_random_circle_p95_ms` 0.055, `query_wall_p95_ms` 0.033, `query_move_along_p95_ms` 0.011, `override_toggle_ms` 0.22, `stuck_100_extra_ms` 0.011, existing `query_path_p95_ms`/`query_raycast_p95_ms`/`crowd_*` unchanged; mobile/web recorded by the S15 device-lab run |
| AC33 | R7, R34 | `NavStubTest.QueriesFail` extended to the new queries (fail, log once); web gate (`tools/build.sh web`) and Android build pass; `tools/xcode_sync_navigation.py --check` passes with the new files |
| AC34 | R46 | `docs/navigation.md` sections and the guide recipes exist; `test_docs.py` and the Lua doctest pass |

## 8. Dependencies & conflicts

- **Depends on:** S19 (landed). S54: `nav::PathFailure` and `pathFailureName` in `NavTypes.h`
  (fixed contract: `None, NoSurface, NotBaked, StartOffMesh, EndOffMesh, StartFiltered,
  EndFiltered, Disconnected, BlockedByArea, OutOfNodes`), the classification inside every path
  query (`NavPath::failure`, S54 R2-R4), its `NavPath` fields (`surface` is S54's `Entity`; S55
  appends after them), island labels (`getIslands`, `getIslandOf`, S54 R7-R8),
  `NavMesh::setPolyFlags` (bumps the revision; P2's poly state uses it and bumps once per bulk
  restore, keeping S54's "a flag change always bumps"), the Lua path table keys and string
  reasons (S54 R35, D4), and its `crowd_filter_overflow` finding, which counts R10's filter keys
  (mask and costs) through `getFilterOverflowCount`. S54's P1 contract commit merges before S55's
  packages start; AC4 and AC25 need S54 merged.
- **S53 (parallel):** it replaces the synchronous `calculatePathOn` at path arrival
  (`NavigationSystem.cpp:2447-2449`) where P3 hooks the failure reason; it makes
  `autoUpdatePosition` and `updateLink` work (R31's `autoUpdate`/`updateLink()` call that code);
  it fixes carving (`updateCarving`), where P2 hooks re-application. Whichever lands second keeps
  both intents.
- **Merge hotspots** (shared with S53/S54 and between packages): `engine/core/subsystem/NavigationSystem.{h,cpp}`,
  `engine/core/navigation/NavMesh.{h,cpp}`, `NavTypes.{h,cpp}`, `NavCrowd.{h,cpp}`,
  `NavDetourSupport.h`, `NavigationSources.cmake`, the Xcode project
  (`engine/workspaces/xcode/.../project.pbxproj`, via `tools/xcode_sync_navigation.py`),
  `engine/core/script/binding/NavigationClassesLua.cpp`, `editor/mcp/tools/NavigationTools.cpp`,
  `editor/mcp/tools/PlayTools.cpp`, `editor/window/InspectorNavigation.cpp`,
  `editor/reflect/components/NavigationReflect.cpp`, `editor/StreamNavigation.cpp`,
  `editor/FactoryNavigation.cpp`, `editor/tests/reflect/catalog_snapshot.json`,
  `editor/navigation/NavigationOverlay.{h,cpp}`, `docs/navigation.md`, `docs/mcp/guide/navigation.md`,
  `tools/perf/bench-thresholds.json`, `engine/core/registry/EntityCloner.cpp` (clears events: add
  `onStuck`).
- **Blocks:** nothing indexed; AI sample scenes and the later N3 traversal work want it.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| A script volume or a moved modifier volume does not match polygon borders | Centroid rule (R11, R12), documented; a volume that must be exact is a modifier with Apply At Runtime, baked once where it stands |
| An agent standing in a volume that becomes Not Walkable | dtCrowd moves it to the nearest passable polygon (its path-validity check); documented; door scripts can wait for agents to pass |
| Adjacent runtime volumes share one id range, so polygons can span both | Each polygon follows its centroid; documented ("give touching doors a gap of one voxel") |
| `onPathFailed`'s C++ signature changes | Accepted (S19 is weeks old, no C++ users in the repo: grep finds only `NavAgentTest.cpp:330`); release note; Lua unaffected |
| A path table built by a script has no `corridor` | `setPath` refuses it (false, path cleared, one warning): only paths from `calculatePath` carry a corridor |
| 16 crowd filters overflow with many per-agent cost sets | Same-mask fallback keeps restrictions, warning + S54 finding; costs are rarely unique per agent |
| `setPath` with > 256 polygons follows only its first 256 exactly | dtCrowd plans the rest near its end; documented |
| `randomPoint` around a point is not uniform | Documented; anywhere is uniform by area (AC2) |
| `query_navigation` shares its 1500 with S54's additions (locate, `to` arrays, reasons) | S55 adds about 80 characters; whichever lands second measures; over budget, op detail moves to the guide first, then an exception at the measured size + 5 % naming both specs |
| The second straight-path pass breaks the path budget | Measured by AC32; if over, cost and area lengths become `PathOptions::computeCost` (Deviation) |
| Recast area ids 33..48 collide with a future use | Constants named in `NavBakeInput.h` beside `FIRST_LINK_COST_AREA`, with a static_assert on the range |

## 10. Work packages

Three implementers, three worktrees, coding against §6's signatures from day one. Merge order:
**P1, then P2 and P3 in either order** (P3 lands AC31 last). Shared files are edited in the regions
named; each package appends its own section (`// ---- S55 queries`, `// ---- S55 areas`,
`// ---- S55 agents`) in headers, so conflicts stay textual.

**P1 — queries, filters, area costs, paths.** Owns: `navigation/NavRandom.h` (new),
`NavTypes.{h,cpp}` (QueryFilter, NavHit, NavPath, PathOptions, RandomPointQuery, CornerFlag),
`NavMesh.{h,cpp}` query functions and `NavFilter::fromQuery`, `NavCrowd.cpp` filter slots
(`Impl::filterFor/fillFilter/toDetour`, overflow count), its tests in `NavQueryTest.cpp` and
`NavCrowdTest.cpp` (appended), `NavigationSystem.{h,cpp}` query section
and agent area-cost functions, `object/navigation/NavMeshAgent.{h,cpp}` cost members,
`NavigationClassesLua.cpp` (NavigationSystem block, `NavMeshQueryFilter`, path table keys and `NavMeshPathCorridor`,
`NavAreas.getAreaName`, `NavCornerFlags`, agent cost methods), `stubs/Navigation.lua`,
`NavigationTools.cpp` `query_navigation`, `NavGameplayQueryTest.cpp`, NavBench query lines,
`test_navigation_gameplay.py` (creates it: header, fixture helper, `test_query_ops`).
Provides to the others: `NavPath` with `polys`/`meshRevision` filled by `findPath`;
`NavFilter fromQuery(const QueryFilter&)`; in `NavigationClassesLua.cpp` the helpers
`luabridge::LuaRef pathTable(lua_State*, const nav::NavPath&)` (extended) and
`bool pathFromTable(lua_State*, int index, nav::NavPath& out)` (false without a corridor).

**P2 — runtime areas, links, handles, editor.** Owns: `navigation/NavPolyState.{h,cpp}` (new),
`NavMesh.{h,cpp}` override/link functions and the `onTilesChanged` calls in `create`,
`replaceTiles`, `updateCarving`, `updateLink`; `NavBakeInput.h` (`boundaryOnly`, id constants),
`NavBakeCollector.cpp` runtime volumes, `NavMeshBuilder.cpp` boundary marker, `NavDetourSupport.h`
mapping; `NavCrowd.{h,cpp}` `replanCrossing` (appended); `subsystem/NavigationAreas.cpp` (new),
`NavigationSystem.{h,cpp}` areas section, step 1b and the link-occupied query; components
`NavMeshLinkComponent.h` (`activated`), `NavMeshModifierComponent.h` (`applyAtRuntime`);
`object/navigation/NavMeshObstacle|NavMeshLink|NavMeshModifier|NavAreaOverride.{h,cpp}` (new),
`script/binding/NavigationComponentsLua.cpp` (new, registered from `registerNavigationClasses`);
reflection, codecs, factory and Inspector rows for Link and Modifier; `NavigationOverlay.{h,cpp}`;
the editor's per-frame `syncAreaOverrides` call (`editor/navigation/NavigationEditor.cpp`);
`get_navigation_info` links/area_overrides, `visualize_navigation`, `PlayTools.cpp` link/modifier/
obstacle fields; `NavAreaOverrideTest.cpp`, the `nav_doors` fixture and its hash file (recorded
first, on the base tree), editor tests AC29 (its rows)/AC30, MCP `test_door_and_bridge_in_play`,
`test_lua_component_handles`, `test_overlay_screenshot`; NavigationSources.cmake and Xcode sync
for its new files.
P3 calls its `isLinkOccupied` and `isLinkActive` (§6, P2 block).

**P3 — agent API, root motion, stuck, failure reasons.** Owns: `NavCrowd.{h,cpp}` corridor and
velocity functions (`setCorridor`, `getCorridor`, `isCorridorValid`, `overrideVelocity`,
`requestMoveVelocity`), `NavigationSystem.{h,cpp}` agents section, `writeBack` and `dispatch`
(event payload, `Stuck`, watchdog), `NavMeshAgentComponent.h` (stuck fields, `stuckTime`,
`lastPathFailure`, events), `object/navigation/NavMeshAgent.{h,cpp}` (except P1's cost members),
the agent block of `NavigationClassesLua.cpp`, `EntityCloner.cpp`, agent
reflection/codec/factory/Inspector rows, `get_navigation_info` agent fields, `PlayTools.cpp` agent
fields; `NavAgentApiTest.cpp`, `NavDeterminismTest.GameplayReplay` and `NavSystemTest.GameplayZeroCost`
(appended to `NavCrowdTest.cpp`),
NavBench `stuck_100_extra_ms`, MCP `test_path_failed_callbacks` and the agent part of
`test_door_and_bridge_in_play`. P3's signatures are §6's P3 blocks; P1 and P2 call none of them.

Each package: `tools/check-changed` per commit on its build server; before merging, the navigation
core and editor suites, `test_navigation.py`, `test_navigation_gameplay.py`, `test_protocol.py`,
`tool_budget.py --check`, `gen_tool_docs.py --check`, web and Android builds (engine core touched).

## Deviations

- S55 P1: `NavMesh::findRandomPoint` and `findRandomPointAround` take `int island` (-1: any)
  instead of `uint32_t island` (0: any): S54's island ids start at 0. `findRandomPointAround` takes
  the island too, so *around* combines with *same island* inside the 8 tries. Engine-internal (only
  `NavigationSystem` calls them).
- S55 P1: `findClosestEdge(point, out, filter, maxDistance)` and `calculatePath(from, to, filter,
  options)` are the S19 functions with defaulted trailing parameters, not extra overloads. Added
  next to §6: `QueryFilter` overloads of `calculatePathOn`, `raycastOn`, `findClosestEdgeOn`,
  `moveAlongSurfaceOn` (a filter's costs on one surface; S54 R27's `calculatePathOn(surface, ...,
  filter)` and P3's agent queries use them), `getAgentQueryFilter(agent)` (the agent's type, mask
  and own costs, what P3's agent queries pass), `nav::clampAreaCost`, `NavCrowd::getFilterCount` /
  `isFilterFallback` and `NavSurfaceStatus::crowdFilters` / `crowdFilterOverflow` (the tests and
  S54's `crowd_filter_overflow` finding read them).
- S55 P1: `NavCrowdAgentParams::areaCosts` holds the agent's own costs with 0 for "the project's"
  rather than fully resolved costs. A crowd filter's key is (mask, these costs); `refreshFilters`
  refills the filters with the project's current costs, so areas without an override follow project
  edits (R9) without re-keying any agent.
- S55 P1: `query_navigation` `random` always draws from a stream of its own (`seed`, default
  `DEFAULT_RANDOM_SEED` = 24301): the tool is read-only and idempotent, so it never advances the
  game's stream. The result echoes the seed.
- S55 P1: `areaLengths` (and `area_lengths`) are in scene units like `length` (metres in 3D, points
  on a 2D surface).
- S55 P1: AC10's red control. A same-mask fallback cannot differ from S19, which keys filters by
  mask alone (the 16 cost variants of one mask shared one filter there). The red run is the
  freeing part: after the first agents leave, 15 new masks fit (S19 kept the old mask's filter, so
  the 15th new mask fell back to every area and walked the masked strip).
- S55 P1: an agent with area costs of its own steers without Detour's visibility shortcut
  (`DT_CROWD_OPTIMIZE_VIS`): `dtPathCorridor::optimizePathVisibility` raycasts without costs and cut
  the agent straight through the area it pays 10 for (the first `NavCrowdTest.PerAgentAreaCosts`
  run). The topology optimisation, which searches with the filter's costs, stays on. Agents without
  costs of their own keep S19's flags (and S19's behaviour with project costs).
- S55 P1: AC8 compares the raycast's cost with length x cost to 1e-3 *relative*: Detour prices the
  polygons' 3D segments (their detail heights), while `distance` stays S19's planar length.
- S55 P1: AC2's red control is a run with `findRandomPoint` temporarily calling Detour's
  `dtNavMeshQuery::findRandomPoint` (the unit tests cannot include Detour's headers); the log is
  kept with the package's evidence.
- S55 P1: AC4's "door closed by R11" (P2) is S54's `NavMesh::setPolyFlags(ref, 0)` on the doorway's
  polygons, which is what R11 does underneath.
- S55 P2: `NavMesh::addAreaOverride(volume, area, order = 0)` takes an order key: overrides apply in
  ascending (order, id). NavigationSystem passes the entity id for modifier volumes and
  `2^32 + handle` for script volumes, which is R13's order; NavMesh itself knows neither.
- S55 P2: `NavigationSystem::syncAreaOverrides()` returns whether a polygon changed (the editor
  redraws the Scene view then).
- S55 P2: `NavPolyState` writes flags and areas into Detour's tiles itself and `NavMesh` bumps the
  revision once per operation that changed a polygon, instead of a `setPolyFlags` call per polygon
  (D5): the same "every applied change bumps", one bump for a door of many polygons.
- S55 P2: the `nav_doors` fixture is built in code like S19's fixtures (`NavFixtures.h` `doors()`,
  `doorway()` adds the runtime volume); its tile hashes are `fixtures/navigation/nav_doors_hashes.txt`,
  one line per platform tag and tile (bakes are bit-exact per platform, S19 R40b).
- S55 P2: the system's area state is declared in an internal header
  (`subsystem/NavigationAreas.h`) so that `NavigationSystem`'s destructor sees a complete type.
- S55 P2: added beside §6: `hasAreaOverride`, `getAreaOverride` (NavAreaOverride's fields),
  `getAreaOverrides` / `AreaOverrideStatus` (overlay, `get_navigation_info`),
  `getModifierAppliedPolygons`, `getAreaOverridePolygons`, `isLinkActive(Entity)`,
  `updateLink(Entity)`, `getScene()`; `NavMesh::getPolyStateTileCount` and `getTileStateBytes`
  (R13/R18 checks); `NavBakeCollector::boxVolume` (one box-to-prism code for bake and runtime).
- S55 P2: a runtime modifier volume applies to every surface (the component has no agent-type
  filter); only the tiles it overlaps are touched. The centroid of a polygon counts as inside when it
  is within the prism's footprint and within one step (`max(agentClimb, cellHeight)`) of its heights.
- S55 P2: `NavMeshLink:updateLink()` calls `NavigationSystem::updateLink(link)`, which recomputes the
  connections (`linkConnections`) for every surface whose navmesh holds the link and calls
  `NavMesh::updateLink`; a cost the bake did not see uses the area's cost. S53's fixed-tick Auto
  Update reuses the same NavMesh call.
- S55 P2: Occupied (R17) is the engine's own link traversal (`onOffMeshLinkStart` to `End`); Detour's
  fallback traversal (`takeDetourTraversals`) carries no link id and does not count.
- S55 P2: AC18's allocation count covers the area sync's share of a tick (`syncAreaOverrides`, 600
  calls with 50 agents, no overrides): S19's tick itself allocates (its sorted agent list), which
  `NavSystemTest.GameplayZeroCost` (P3) measures.

- S55 P3: `agent.path` (`getAgentCorridorPath`) starts at the agent's position, as `calculatePath`'s
  paths and Unity's `NavMeshAgent.path` do, so AC21 compares its corners after the first with
  `getPathCorners()` (the corners ahead).
- S55 P3: `setPath` checks every polygon it puts in the corridor against the agent's filter
  (`isValidPolyRef`, O(1) each, at most 256) whatever `meshRevision` says, instead of only after a
  revision change: the same answer, and no stale-revision corner case.
- S55 P3: an agent following a path from `setPath` steers without dtCrowd's corridor shortcuts
  (`DT_CROWD_OPTIMIZE_VIS`, `DT_CROWD_OPTIMIZE_TOPO`) until its next request or a crowd replan;
  with them the crowd re-optimises the given route with its own costs and leaves it (AC19).
- S55 P3: without a path, `velocity = v` moves the agent at `v` only as far as its acceleration lets
  the crowd's velocity follow in one tick (Detour's velocity request integrates within
  `maxAcceleration`); AC22's no-path case runs with an acceleration that reaches `v` in a tick.
- S55 P3: AC18's zero allocations per tick also needed the dispatched events' buffer kept between
  ticks and sorted without `stable_sort`'s temporary buffer (S53 R19 keeps the agent order and the
  crowd slot owners); the event order is unchanged (entity, then queue order).
- S55 P3: the one search that classifies a failure (R27) is left out of S53's
  `getPathQueryCount()`, which counts the searches scripts and tools run (S53 R6: a crowd agent's
  path state never searches; a failure's reason does, once per failure).
- S55 P3: P2's R15 drop of a path without Auto Repath fires `onPathFailed` through the same
  classification (`failPath`), so its reason is the `BlockedByArea` AC25 expects.

## Progress (implementation notes, keep updated)

- S55 P1: interfaces (§6 P1 block) and the queries, filters, crowd filter slots, Lua and
  `query_navigation` implemented; tests in `NavGameplayQueryTest.cpp`, `NavQueryTest.cpp`,
  `NavCrowdTest.cpp`, `NavStubTest.cpp`, `test_navigation_gameplay.py::test_query_ops`; NavBench
  query lines and their thresholds.
- S55 P2: `NavPolyState` (overrides and link states over `storeTileState` snapshots, re-applied in
  `create`, `replaceTiles`, `updateCarving`, `updateLink`), the runtime volume boundary in the bake
  (Recast ids 33-48), `NavigationAreas.cpp` (modifier volumes, script volumes, Activated; step 1b
  and the corridor replans), the Obstacle / Link / Modifier / NavAreaOverride Lua handles, the
  Inspector rows, the overlay's Show Area Overrides, `get_navigation_info` `area_overrides` and link
  `activated` / `occupied`, `visualize_navigation` `show.area_overrides`, `get_runtime_entity` link,
  modifier and obstacle fields; tests in `NavAreaOverrideTest.cpp`, `NavigationEditorTest.cpp`,
  `test_navigation_gameplay.py` (door and bridge, handles, overlay screenshot).
- S55 P3: `NavCrowd` corridor and velocity access (`setCorridor`, `getCorridor`, `isCorridorValid`,
  `overrideVelocity`, `requestMoveVelocity`, with `getSteeringTarget`, `getCorridorPath`,
  `getNextOffMeshLink`), the `NavigationSystem` agent section (R19-R24), failure reasons through
  `failPath` / `classifyFailure` at every `onPathFailed` (R27), the stuck watchdog (R25, R26),
  `Event::detail` and `Stuck` (R29), the component's stuck fields, `stuckTime`, `lastPathFailure`,
  `onStuck` and the two-argument `onPathFailed`, the `NavMeshAgent` handle and its Lua members (the
  reason a string, R28), `EntityCloner`, reflection / codec / factory / Inspector rows (Stuck
  Detection foldout, runtime lines), `get_navigation_info` `last_failure` / `stuck_seconds`,
  `get_runtime_entity` agent fields; tests in `NavAgentApiTest.cpp`, `NavCrowdTest.cpp`
  (`GameplayZeroCost`, `GameplayReplay`), `NavigationEditorTest.cpp` (agent rows of AC29),
  `test_navigation_gameplay.py` (`test_path_failed_callbacks`, the Guard of the doors test); NavBench
  `stuck_100_extra_ms`.
