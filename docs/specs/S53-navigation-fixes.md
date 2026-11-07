# S53 — Navigation hygiene (N0): upstream fixes, bake reporting, corridor status, links, carving

Wave 4 (navigation N0). A follow-up to `docs/specs/S19-navigation.md`, from an audit of its code.
It lands before S54 (navigation diagnostics) and S55 (gameplay API), which build on it. Paths are
relative to the repository root. `UPSTREAM` is recastnavigation's history (read-only clone,
`/tmp/recast-analysis/src`). Every claim in §3 was checked by reading the code at the cited lines.

## 1. Goal

S19 shipped a complete navigation stack, but an audit found defects that later features would
build on:
- Upstream crash and assert fixes that are not vendored yet.
- Bake failures and Recast diagnostics that never reach the user.
- A synchronous A* search on every crowd path arrival, which undoes Detour's time-sliced path queue.
- A NavMeshLink option that is stored but does nothing.
- Carving obstacles whose cuts can be lost or stay forever, and whose timing depends on the machine.

S53 fixes and hardens what exists. It adds no new features: S54 owns diagnostics and S55 owns the
gameplay API. Every fix ships a test that fails without it.

## 2. Scope

**In scope**
- Three upstream recastnavigation fixes, applied as marked local patches (R1).
- Bake reporting: failures, the state of the last bake, and Recast warnings in the existing
  `warnings` array (R2-R5).
- Path status derived from the crowd corridor. This covers arrival at the destination, corridor
  extension, and detecting a new mesh (R6-R9).
- `NavMeshLink` Auto Update Positions (R10).
- Carving: an obstacle registry, feeding the request queue, thresholds, a deterministic budget,
  deferred tile builds, Max Carving Obstacles (R11-R17).
- Small editor and MCP defects, test-infrastructure gaps, and records missing from S19 (R20-R28).

**Out of scope**
- **S54:** `nav::PathFailure` reasons, islands, coverage, findings, the Path Tester, new overlays,
  and new MCP parameters for diagnostics.
- **S55:** new queries, runtime area toggles, enabling or disabling links at runtime, `setPath`,
  root motion, stuck detection, and Lua handles for Obstacle, Link and Modifier.
- Re-vendoring upstream `main`, and the upstream commits `0096f0d`, `3e94c3b` and `c393777` (R1).
- Per-tile vertical bounds. They would change every tile hash (`NavBakeInput.cpp:287-288`) and
  dirty every existing bake, so R5 adds a warning instead.
- Splitting large carving obstacles into pieces. R16 warns instead.
- Nothing from the README do-not-copy list is involved. No OpenEngine code is taken.

## 3. Current state (verified by reading; "inferred" = from code, not run)

**Item 1: upstream fixes.** The vendored code is v1.6.0 (`6dc1667f5803`). All six commits named in
the brief come after it in `UPSTREAM`.

- `9147d09`: `dtPathCorridor::setCorridor` asserts `npath < m_maxPath`
  (`engine/libs/recastnavigation/DetourCrowd/Source/DetourPathCorridor.cpp:515`).
  - The crowd's corridor and path buffer hold 256 polygons (`DetourCrowd.cpp:421`).
  - A path-queue result is clipped to `m_maxPathResult - (npath-1)` (`:848-849`) and set with up to
    256 polygons (`:888`).
  - So a corridor of exactly 256 polygons fires the assert. It only matters where asserts are
    compiled in: `DetourAssert.h` disables them under `NDEBUG`, and sanitizer builds are Debug
    (`tools/build.sh:396`).
- `599fd0f`: `dtMergeCorridorStartMoved` computes `size = maxPath - req`, which can be negative, and
  then calls `memmove(..., size*4)` and writes `req` entries (`DetourPathCorridor.cpp:59-66`).
  - This is out of bounds in every build when `nvisited - furthestVisited > maxPath`.
  - dtCrowd cannot reach it: `moveAlongSurface` visits at most 16 polygons, and the corridor holds
    256. Only a direct call can (inferred).
  - `dtMergeCorridorStartShortcut` (`:142-152`) has the same pattern, but `req <= MAX_RES` (32) is
    always below 256. Upstream left it unchanged too, so S53 does not patch it.
- `9f4ce64` (#796): `distToTriMesh(..., &tris[0], ...)` runs on a possibly empty `rcIntArray`
  (`Recast/Source/RecastMeshDetail.cpp:846`).
  - `delaunayHull` can drop every dangling face (`:518-528`).
  - `rcTempVector::operator[]` then asserts (`RecastAlloc.h:150`). In Release it is harmless,
    because zero triangles are never read (inferred).
- Not taken:
  - `0096f0d` makes `raycast` stop once its path buffer is full. That changes query and crowd
    results (`optimizePathVisibility` raycasts into 32 slots), and v1.6.0 already keeps walking
    safely.
  - `3e94c3b` changes `rcFilterLedgeSpans` (`>` became `>=`, early breaks), and `c393777` rewrites
    detail-edge detection. Both change bake output without fixing a crash: a rebake would differ
    from the shipped asset (S19 R40) for no gain.
  - Re-vendoring `main` would also pull `af6d475`, `63dd4aa`, `77f7e54` and `f9addb2`. Those gate
    asserts on `RC_DISABLE_ASSERTS` instead of `NDEBUG`, which turns asserts on in Release builds.
- The existing local patch is the crowd's avoidance priority (`PATCHES.md`; `README.jasper.md`
  says "one patch"; `NOTICE.md:32` says the same).

**Item 2: bake reporting.**
- What already works: the engine logs tile failures and mesh-swap failures with `Log::error`
  (`engine/core/subsystem/NavigationSystem.cpp:778, 807`). `Out` sits on top of the same logger,
  so these reach the Console. **The brief's "logs nothing" is therefore narrowed** to the gaps
  below.
- `NavigationEditor::finish` (`editor/navigation/NavigationEditor.cpp:277-315`) logs successes
  (`:310`), warnings and save failures. It logs nothing for:
  - `result.ok == false` coming from the engine,
  - "the bake produced no navmesh" (`:286`),
  - "the bake did not finish" (`:269`),
  - "the scene closed" (`:257`, passed to the callbacks only).
- A refused start of a Shift+Bake (FULL) is discarded (`editor/window/InspectorNavigation.cpp:267`).
  Every other caller logs the refusal itself (`:261`, `NavigationWindow.cpp:207, 248`).
- No per-surface failure state exists in the UI. The Inspector shows only Not baked, Baked or Dirty
  (`InspectorNavigation.cpp:276-292`).

**Item 3: synchronous search.** When a pending crowd request becomes Valid, which happens on every
first path and every replan, `writeBack` calls `calculatePathOn` (`NavigationSystem.cpp:2444-2451`).
- That is a full `dtNavMeshQuery::findPath` plus `findStraightPath`, bounded by the query's 2048
  nodes (`NavMesh.cpp:392-394, 945-984`). It is not unbounded, but it runs on the main thread inside
  the fixed tick.
- It can disagree with the crowd's own search, which has 4096 path-queue nodes
  (`DetourCrowd.cpp:50`).
- It also allocates a `NavPath`.

**Related defects found:**
- Navmesh arrival (`:2468-2479`) does not require a Complete path, while grid arrival does
  (`:2350-2353`). It measures to `corridor.getTarget()` (`NavCrowd.cpp:503-514`). So
  `onDestinationReached` fires at the end of a Partial path, and could fire at the end of a corridor
  truncated at 256 polygons (inferred).
- With Auto Repath off, any Valid→Pending transition drops the path (`:2432-2440`). That includes
  dtCrowd's own extension replan of a truncated corridor (`DetourCrowd.cpp:1026-1032`) (inferred).
- New Detour meshes are detected by comparing the `dtNavMesh` pointer (`:1721-1722, 1751`). But
  `replaceTiles` falls back to `create()` inside the same `NavMesh` (`NavMesh.cpp:865-866, 876`),
  which frees and reallocates a `dtNavMesh` of the same size. The allocator can return the same
  address, and then the crowd is kept with stale corridors (inferred).

**Item 4: links.**
- `NavMeshLinkComponent::autoUpdatePosition` is serialized and shown, but never read:
  `NavMeshLinkComponent.h:30`, `StreamNavigation.cpp:158, 174`, `FactoryNavigation.cpp:125`,
  `NavigationReflect.cpp:204`, `InspectorNavigation.cpp:442`.
- `NavMesh::updateLink` (`NavMesh.cpp:1164-1264`) has no caller.
  - Carving: it moves link records between tiles and rebuilds the touched tiles synchronously.
  - Static: it patches tile copies and re-adds them, and fails when a start leaves its baked tile or
    the connection count changes.
- `NavBakeCollector::linkConnections` (`NavBakeCollector.cpp:1160-1203`) computes a link's
  connections and can be reused.

**Item 5: carving.**
- 5a. `removeObstacle`'s result is ignored and the entry erased (`NavigationSystem.cpp:2023-2026`).
  - `dtTileCache` refuses requests once its 64-request queue is full (`DetourTileCache.h:244`,
    `.cpp:361, 396, 430, 471`). A refused removal leaves the cut forever.
  - Its 64-entry update list silently drops tiles beyond 64 (`.cpp:552-559, 568-575`). The obstacle
    then reaches PROCESSED with tiles that were never rebuilt.
  - A failed add is retried, because nothing is stored on failure (`:2041-2045`).
- 5b. Cuts are lost in two ways:
  - After a full bake, reload or `setNavMeshData`: a new `NavMesh` object has no obstacles
    (`:417, 443, 801`), while `obstacle.carves` keeps the old ids and `carved` stays true
    (`:2047`). Nothing re-adds them.
  - After `replaceTiles` on a carving mesh: `removeLayersAt` drops the compressed tiles
    (`NavMesh.cpp:871`), and the new ones get new refs. `buildNavMeshTile` rasterizes only
    obstacles whose `touched[]` holds the new ref (`DetourTileCache.cpp:687`), and none does.
- 5c. The rotation test is `|dot(q0, q1)| < cos(0.5 * moveThreshold)` (`:1999`), which uses metres
  as radians.
  - Related: "stationary" means no move above 1e-5 m in a tick (`:2000`), so sub-millimetre rest
    jitter keeps `carveOnlyStationary` obstacles uncut.
  - Related: changes to shape, size, radius, height, centre or world scale never re-cut, because
    `moved` checks only position and rotation (`:1997-1999`).
  - Detour boxes use yaw only (`:2031-2034`), but pitch and roll count as moves.
- 5d. `CARVE_BUDGET_MS = 1.0` (`:59`) and `NavMesh::updateCarving` loop on wall clock
  (`NavMesh.cpp:1136-1162`).
  - The landing tick varies by machine, and `NavCrowdTest.cpp:99-113` relaxes AC10 under
    sanitizers because of it.
  - It breaks S19 R40c (identical replays) once carving obstacles move.
  - A failed tile build `break`s with `upToDate` still true (`:1146-1147`).
- 5e. `create()` builds every carved tile synchronously (`NavMesh.cpp:708-712, 786-803`).
  `replaceTiles` (`:878`) and `updateLink` (`:1195`) do the same.
  - At load and in edit-mode bakes this is acceptable.
  - In play, a full `bakeAsync` of a carving surface lands every tile build in one fixed tick. That
    breaks S19 R10 (agents keep the old tiles until the swap) and R23 (≤ 4 ms per web frame)
    (inferred).
- 5f. `maxObstacles` (fixed 128, `NavMesh.h:40`) **is already stored** in the `.navmesh` header
  (`NavMesh.cpp:248` write, `:316` read, `:329` range 0-65535), so no format change is needed.
  - `syncObstacles` allocates a sorted entity vector every tick (`:1982`).
  - `sortedAgents` does the same (`:1702`, called several times per tick).
  - The grid lookahead allocates `cells` per agent per tick (`:2340`).
- Related: `queryTiles` caps an obstacle at `DT_MAX_TOUCHED_TILES` = 8 tile layers
  (`DetourTileCache.h:62`, `.cpp:548`). A larger obstacle is silently cut in only 8 of them.

**Item 6: Recast diagnostics.**
- `TileJob::State` uses `rcContext context{false}` (`NavMeshBuilder.cpp:100`), which turns logging
  off, so every Recast message is dropped.
- Some Recast calls log an error and still return true:
  - `rcBuildRegions`: "%d overlapping regions" (`RecastRegion.cpp:1649`).
  - `rcBuildCompactHeightfield`: "Heightfield has too many layers" (`Recast.cpp:535-539`). This
    happens with more than 63 spans in a column, and the upper floors are then connected wrongly.
- Failing calls report only our generic text, for example "Recast could not build the tile-cache
  layers" (`NavMeshBuilder.cpp:488-490`), while Recast's own reason ("layer overflow",
  `RecastLayers.cpp:306`) is lost.
- `rcContext::log` returns before formatting when logging is off (`Recast.cpp:58-79`). Recast has
  no `RC_LOG_PROGRESS` call sites, so collecting messages costs nothing when nothing logs.

**Item 7: vertical range and layers.**
- Each tile heightfield spans the surface's whole y range (`NavMeshBuilder.cpp:167, 170`).
- Spans clamp at `RC_SPAN_MAX_HEIGHT` = 8191 cells (`Recast.h:284-286`,
  `RecastRasterization.cpp:445-446`). At the default cell height of 0.083 m that is about 682 m.
  Nothing checks it; only `checkPolyRefBits` exists as a preflight (`NavBakeCollector.cpp:1112`).
- Carving layers are truncated silently at `MAX_LAYERS` = 32 (`NavMeshBuilder.cpp:34, 496`). The
  value 32 matches `buildNavMeshTilesAt`'s `MAX_TILES` and is kept.

**Item 8: editor and MCP.**
- `NavModifierInclusion::AUTO` is labelled "Remove Object" (`NavigationReflect.cpp:71`), but it means
  the surface's policy (`NavMeshModifierComponent.h:17`). EXCLUDE is "Remove Object (always)". The
  same labels appear in `InspectorNavigation.cpp:470` and `catalog_snapshot.json:34077-34087`.
- `visualize_navigation` (`NavigationTools.cpp:747-818`):
  - It has a dead expression at `:777`.
  - Its path uses the default filter, ignoring agent type and area mask (`:791`).
  - It resolves the scene with `sceneOf(call, false)` (`:751`). That is only wrong after a scene
    load replaced the played scene, or for additive runtime scenes (`PlayTools.cpp:772-780`).
    **Narrowed** from the brief.
  - It has no `activity` set, so it gets Auto, which means Edit (`McpServer.cpp:1138`). It only
    changes workspace display settings, so Control fits.
- `bake_navigation` mode `clear` deletes the `.navmesh` (`NavigationEditor.cpp:209-219`), but the
  tool has no `destructive` annotation (`:823-840`).
- None of the six tools sets `allowedWhilePlaying`; the default is true (`McpRegistry.h:52`).
  `conventions.md` says it must be set on purpose. `bake_navigation` refuses play in its handler
  (`:145`). The brush is edit-only (`NavigationEditor.cpp:437`), but `paint_nav_grid` is accepted in
  play.
- The `[`/`]` handler resizes the nav-grid brush (`editor/AppCommands.cpp:296-298`), but its enable
  predicate is `terrainEditing() || (size && dyePainting())` (`:302`). The keys are therefore
  disabled while only the nav-grid brush is armed.
- `test_overlay_golden` writes the golden and passes when the file is missing
  (`tools/mcp/tests/test_navigation.py:232`).
- The S19 R33 "reservation tint in play" is missing: the overlay never reads reservations
  (`NavigationOverlay.cpp:388-462`).
- S19's Deviations miss two facts:
  - `bench_nav_city` has 2643 polys on linux-gcc (`engine/tests/core/fixtures/navigation/bench_nav_city_polys.txt`),
    not about 20k.
  - The +10 % timing allowance lives only in `tools/perf/bench-thresholds.json:2`.
- No gate builds `JASPERENGINE_NAVIGATION=OFF`, so `NavStubTest` (compiled only when it is OFF) never
  runs. `tools/check-physics-off.sh` is the precedent for such a gate.
- S19 AC27 has no test: `test_build.py` has no navigation test.

## 4. Reference

| Source | Take | Notes |
|---|---|---|
| `UPSTREAM` `9147d09` (#619) | the one-character assert fix | `DetourPathCorridor.cpp` |
| `UPSTREAM` `599fd0f` (#635) | `if (size > 0)` and the `dtMin(req, maxPath)` store loop; the shape of its test (`Tests/DetourCrowd/Tests_DetourPathCorridor.cpp`, Catch2, rewritten as gtest) | the test is reference only |
| `UPSTREAM` `9f4ce64` (#796) | `if (tris.size() == 0) continue;` | `RecastMeshDetail.cpp` |
| Unity AI Navigation docs (names only) | `NavMeshLink.autoUpdate` ("Auto Update Positions"); `NavMeshObstacle.carving`, `carvingMoveThreshold` ("treated as moving when it moved more than"), `carvingTimeToStationary`, `carveOnlyStationary`; `NavMeshPathStatus` | behaviour reference; no code |

recastnavigation is zlib-licensed. Every changed line carries `// JasperEngine patch:` (or
`/* JasperEngine patch */` inside an expression), and each patch gets a `PATCHES.md` entry naming
its upstream hash. No OpenEngine code is used.

## 5. Requirements

### Functional — vendored fixes

- **R1 Upstream patches.**
  - Apply `9147d09`, `599fd0f` and `9f4ce64` verbatim as patches 2, 3 and 4 in `PATCHES.md`. Each
    entry gives its upstream hash, PR number, the file, why it is needed here (the §3 analysis) and
    the test that covers it.
  - `README.jasper.md` "Local changes" lists all four patches. Its upgrade note says that patches
    2-4 are dropped when the vendored tag contains them.
  - The `NOTICE.md:32` row says "four local patches (`PATCHES.md`)".
  - `PATCHES.md` keeps a "Considered and not taken" list: `0096f0d`, `3e94c3b`, `c393777` and the
    assert-gating series, each with the §3 reason.

### Functional — bake reporting

- **R2 One Console error per failed bake.** Every editor bake that ends neither ok nor cancelled
  produces exactly one error line, `NavMeshSurface '<name>': <reason>`. This covers the Inspector,
  the Navigation window, Bake All and MCP.
  - The engine keeps its `Log::error` (games need it) and sets `NavBakeResult::logged`.
  - `NavigationEditor::finish` logs only results that are not `logged`.
  - `NavigationEditor::bake`, `bakeAll` and `clear` log their own refusals, so callers stop logging
    and the Shift+Bake gap closes.
  - A cancel logs one info line, `bake cancelled`.
- **R3 Last bake state.** `NavigationEditor` keeps, per (scene, surface) and for the session, the
  last result: ok, cancelled, error, warnings, tiles built, polys and ms. A refused start and an
  editor-side failure (save, "the scene closed") count as results.
  - Inspector, under Bake/Clear: a red line `Bake failed: <reason>` until a later bake succeeds. A
    successful bake with warnings shows `Baked with N warning(s); see the Console` in the warning
    colour.
  - Navigation window, Surfaces tab: the State column reads "Bake failed" in red, with the reason as
    a tooltip.
  - Entries of closed scenes are dropped.
- **R4 Recast diagnostics.** Tile jobs use `NavRecastLog`, an `rcContext` subclass with logging on
  and timers off.
  - It collects `RC_LOG_WARNING` and `RC_LOG_ERROR` messages into `NavTileBuildResult::warnings`.
  - Hexadecimal pointer values become `?`, so results are deterministic (`mergeHoles` prints `%p`).
  - "rcBuildContours: Expanding max contours" is dropped, because it is only an allocation notice.
  - When a Recast call fails, the tile's error ends with Recast's last error message.
  - `processBakes` adds the tile warnings to `NavBakeResult::warnings` as `tile (x, y): <message>`,
    in tile-index order. Equal messages are merged with a count. At most 32 lines are kept, then
    `... and N more`.
  - These warnings reach `bake_navigation` and the Console through the existing paths.
  - No allocation and no formatting happen unless Recast logs.
- **R5 Range warnings.** The collector warns when `(bmax.y − bmin.y) / ch + 2` exceeds 8191:
  `the surface spans H m of height, more than the 8191 voxels (M m at this voxel height) a tile
  holds: geometry above Y m is cut off; increase the voxel size or split the surface`. The bake
  continues, because an error would break scenes whose clamped part is not walkable. A carving tile
  with more than 32 layers warns `tile (x, y): N walkable layers, the lowest 32 kept`.

### Functional — crowds and agents

- **R6 Path status from the corridor.** No path search runs on behalf of crowd agents.
  `NavCrowd::getPathState(agent)` reads dtCrowd's state when a request lands (Pending → Valid) and
  on every later replan:

  | Crowd state | pathStatus | Event |
  |---|---|---|
  | the corridor's last polygon is the target's polygon | Complete | — |
  | `partial` (Detour found no path to the target: unreachable, or more than the queue's 4096 nodes) | Partial | `onPathFailed`, once per destination |
  | not partial, and the last polygon is not the target's (the search reached the target; the corridor holds its first 256 polygons) | Complete (truncated) | — |
  | the request failed | Invalid | `onPathFailed` |

  - dtCrowd extends a truncated corridor when fewer than 10 polygons remain and at least 1 s has
    passed since the last replan (`DetourCrowd.cpp:1026-1032`). Partial paths are re-planned the
    same way.
  - A later result can turn Partial into Complete (no event) or Complete into Partial (an
    `onPathFailed` if none fired for this destination).
  - `NavigationSystem::getPathQueryCount()` counts main-thread searches (`calculatePath*`,
    `isReachable`). Crowd agents never increase it.
- **R7 Arrival only at the destination.** `onDestinationReached` requires the corridor to end in
  the target's polygon (Complete and not truncated). The navmesh and grid branches then agree.
  - An agent at the end of a Partial path stops there, keeps `pathStatus` Partial and gets no
    arrival.
  - For a truncated corridor, `remainingDistance` is the steering distance plus the straight line
    from the corridor's end to the destination (a lower bound).
- **R8 Extension is not a repath.** A replan that dtCrowd starts while the previous state was a
  Valid truncated or Partial corridor with fewer than 10 polygons left is an extension. It keeps the
  path even with Auto Repath off. Only a replan caused by an invalid corridor counts as a repath.
- **R9 A new mesh is detected by generation.** `NavMesh::getGeneration()` changes on every
  `create()` and `clear()`. Crowd rebuilds compare generations, not `dtNavMesh` pointers.

### Functional — links

- **R10 Auto Update Positions (Unity `NavMeshLink.autoUpdate`).**
  - In play, every fixed tick (D2 step 2), each enabled, active link with `autoUpdatePosition`
    recomputes its connections with `linkConnections`. This is done for every surface whose
    navmesh holds the link's `userId`.
  - When an endpoint moves more than half that surface's cell size, or the bidirectional flag, area,
    cost or connection count changes, it calls `updateLink`.
  - Carving surfaces: any move works, including into another tile. The rebuilt tiles keep their
    cuts and go through the R14 budget (R15).
  - Static surfaces: moves within the start's tile work. A start that leaves its tile, or a changed
    connection count, keeps the last applied connections and warns once per link and surface:
    `NavMeshLink '<link>' left its baked tile on NavMeshSurface '<surface>': turn on Runtime Carving
    or rebake`.
  - A cost override that is not in the header's `linkAreaCosts` uses the area's cost and warns once.
  - With Auto Update off, a moved link stays where it was baked (Unity behaviour).
  - Scenes without auto-updating links do no per-link work.

### Functional — carving

- **R11 Obstacle registry.** `NavMesh` owns the carving obstacles.
  - Ids are stable and never 0. Each entry holds its description and its `dtObstacleRef`, which is 0
    until the request is submitted.
  - Requests (add, move = remove + add, remove) go into a FIFO.
  - They are submitted to `dtTileCache` in batches of at most `MAX_UPDATE / DT_MAX_TOUCHED_TILES` = 8,
    and only while the tile cache has no pending requests or updates. That way neither 64-entry list
    can overflow.
  - `removeObstacle` cannot fail. Queued adds count against Max Carving Obstacles (R17), so
    `DT_OUT_OF_MEMORY` cannot happen.
  - An add beyond the limit returns 0. The system then avoids the obstacle instead and warns once:
    `NavMeshSurface '<s>' holds its Max Carving Obstacles (N): '<o>' is avoided instead of carved`.
- **R12 Cuts survive mesh changes.**
  - `replaceTiles` re-requests (remove + add) every obstacle that overlaps a replaced tile.
  - `create()` inside an existing `NavMesh` (the pool-growth fallback) re-adds every obstacle.
  - Every place that replaces a surface's `NavMesh` object calls `next.adoptObstacles(previous)`
    before the swap: a full bake (`processBakes`), `reloadNavMesh` and `setNavMeshData`. The
    system's ids stay valid.
- **R13 Re-cut thresholds** (Unity semantics, no new field).
  - An obstacle is moving once it is more than `moveThreshold` from its last cut. A box is also
    moving when its yaw changed by more than `moveThreshold / r` radians, where r is the half
    diagonal of its world XZ footprint (the arc its farthest corner travels).
  - Capsule rotation, and pitch or roll of either shape, never re-cut, because Detour ignores them.
  - Changes to shape, centre, size, radius, height or world scale re-cut.
  - It is stationary once it has stayed within `moveThreshold` of the point where its stationary
    timer started for `timeToStationary` seconds.
- **R14 Deterministic budget.** Each fixed tick a surface does at most `carveTilesPerTick` tile-cache
  builds: deferred tiles first (R15), then obstacle work.
  - Project Settings > Navigation > **Carve Tiles Per Tick**: 0 means Auto (4 on desktop, 2 on
    Android, iOS and web); 1-64 sets the number.
  - A safety ceiling of 4 ms of wall clock stops a tick after at least one build. Tests and
    benchmarks turn it off with `NavigationSystem::setCarveTimeCeiling(0.0)`.
  - A failed tile build is logged once per tile, counted and skipped, and the loop goes on.
- **R15 Deferred carved builds in play.** While playing (`processBakes` inside the fixed tick),
  carved tile builds are queued in the mesh and drained by R14 instead of all being built at once:
  - `create()` from a full bake: the surface keeps its current mesh in service until the new mesh
    has no queued builds. Then it adopts obstacles, swaps, and fires `onBakeCompleted`.
  - `replaceTiles`: each tile keeps its old Detour tile until its rebuild replaces it. Detour tiles
    of layers that no longer exist are removed in that same build.
  - `updateLink`: the touched tiles are rebuilt the same way.
  - Edit-mode bakes and every load still build at once, because neither runs under a frame budget.
- **R16 Large obstacles warn.** An obstacle whose bounds touch more than 8 tile-cache layers warns
  once: `NavMeshObstacle '<o>' covers N tile layers of '<s>' and is cut in 8: make it smaller or
  the tiles larger`.
- **R17 Max Carving Obstacles.** `NavMeshSurfaceComponent::maxCarvingObstacles` (int, 1-4096,
  default 128).
  - It is read when the navmesh is created (load, bake landing, Play) and passed to `create()`.
  - It is written into the header at bake time, where it is informational only. It is not part of
    any tile hash.

### Platform & backends

- **R18** All five backends and every export target, unchanged: no GPU code and no new threads.
  - Web single-threaded runs the same deterministic count budget.
  - The three patches are plain C++ in files we already compile on GCC 11, Clang/Xcode, MSVC, the
    Android NDK and Emscripten.

### Performance

- **R19 Costs.**
  - Unchanged: S19 R25, zero cost with no navigation (`NavSystemTest.ZeroCostEmptyScene`).
  - A steady-state tick allocates nothing in `NavigationSystem` (agents walking, obstacles standing,
    no request landing). `syncObstacles`, `sortedAgents` and the grid lookahead use `Runtime`
    scratch, rebuilt when navigation components are added or removed.
  - A path arrival costs no search (R6).
  - `je-bench-nav` `carve_tick_max_ms` ≤ 1.1 ms at desktop Auto, and `carve_ticks_to_land` ≤ 2.
    If the desktop Auto of 4 misses, the implementer lowers it and records the change.
  - `crowd_100_tick_ms` and `crowd_500_tick_ms` do not regress.
  - R4 and R10 cost nothing when nothing logs and no link auto-updates.

### Editor UX (human)

- **R20 Modifier labels.** The token values stay the same:

  | Token | New label |
  |---|---|
  | `auto` | "Surface Policy" |
  | `include` | "Add or Modify Object" |
  | `exclude` | "Remove Object" |

  - The tooltip and the Inspector help (`InspectorNavigation.cpp:470`) explain the three choices.
  - The catalog snapshot is regenerated.
- **R21 Brush keys.** `[` and `]` are enabled while the nav-grid brush is painting in the selected
  scene, and resize it by one cell (1-64).
- **R22 Reservation tint.** In play, Show NavGrid tints the cells reserved at the current fixed tick.
  The tint is a 50 % blend toward the agent-path colour. The overlay rebuilds when the reservations
  change. It is editor-only and absent from players.
- **R23 Rows.**
  - Inspector, NavMesh Surface > Advanced: **Max Carving Obstacles**, shown with Runtime Carving on.
    It uses the existing property rows, which provide undo.
  - Project Settings > Navigation: **Carve Tiles Per Tick**, with an Auto checkbox for 0. It uses the
    existing settings command, which provides undo, and a `SettingsRows.cpp` search entry.

### Agent UX (MCP, Lua, CLI)

- **R24 Tools** (`editor/mcp/tools/NavigationTools.cpp`). Each tool's play policy is set explicitly:

  | Tool | allowedWhilePlaying | annotations / activity |
  |---|---|---|
  | `bake_navigation` | false (a game uses `bakeAsync`) | destructive (clear deletes the `.navmesh`); Edit |
  | `get_navigation_info` | true | readOnly, idempotent; adds `surfaces[].last_bake {ok, cancelled, error?, warnings}` (R3) |
  | `query_navigation` | true | readOnly, idempotent |
  | `set_navigation_settings` | true (area costs apply at the next tick; the edit stays after Stop, like Project Settings) | Edit; adds `carve_tiles_per_tick` (integer 0-64) |
  | `paint_nav_grid` | false (the brush is edit-only; a game edits cells from Lua) | Edit |
  | `visualize_navigation` | true | Control; `path` takes `agent_type` and `area_mask`, with the shapes of `query_navigation`; it resolves the running scene in play (`sceneOf(call, true)`); the dead expression is removed |

  - The schemas stay within `tools/mcp/budget.json`.
  - `docs/mcp/TOOLS.md` is regenerated.
- **R25 Lua.** No new bindings: Link and Obstacle handles are S55's. `docs/scripting.md` and the
  Lua stub comments describe the R6 and R7 semantics of `pathStatus`, `remainingDistance` and
  `onDestinationReached`.
- **R26 Docs.**
  - `docs/navigation.md`: carving budget and thresholds, Max Carving Obstacles, Auto Update
    Positions, long paths, bake failures and warnings.
  - `docs/mcp/guide/navigation.md`: `last_bake`, warnings, and the play policy.
  - `docs/testing.md`: the OFF gate and the export test.
  - `docs/specs/README.md` index: an S53 row.

### Data & compatibility

- **R27 Old projects load.**
  - New fields get defaults: `maxCarvingObstacles` defaults to 128 when its key is missing;
    `carveTilesPerTick` defaults to 0, which writes nothing to `project.yaml`.
  - Both are added to the stream known keys, generated code (`FactoryNavigation.cpp`,
    `generateNavigationSettingsCode`) and reflection.
  - The scene version is unchanged (S19 Deviations, R41 rule). The `.navmesh` format stays at
    VERSION 1. The labels change, the tokens do not.
- **R28 Test infrastructure and records.**
  - `test_overlay_golden` fails when the golden is missing, unless `JASPER_UPDATE_GOLDEN=1` is set.
  - `tools/check-navigation-off.sh` builds the standalone engine with `JASPERENGINE_NAVIGATION=OFF`
    and `JASPERENGINE_TESTS=ON`, and runs `NavStubTest.*:NavGridTest.*`. It fails when no
    `NavStubTest` ran.
  - `tools/check-changed` plans it when `engine/core/navigation/**`,
    `engine/libs/recastnavigation/**`, `engine/core/subsystem/NavigationSystem.*`,
    `engine/core/object/navigation/**`, `engine/core/component/Nav*` or `engine/CMakeLists.txt`
    change. CI runs it.
  - S19 AC27 gets a prebuilt-player export test.
  - S19's Deviations record the `bench_nav_city` size (2643 polys on linux-gcc, 2670 on
    apple-clang-arm64, against "about 20k") and the +10 % allowance for the timing server.

## 6. Design constraints & decisions

- **D1 Where things live.**
  - `NavRecastLog` goes in `engine/core/navigation/NavDetourSupport.{h,cpp}` (namespace `detail`).
  - The registry, the request FIFO, the deferred-build queue and the generation counter go in
    `NavMesh::Impl`.
  - Thresholds, link tracking and adoption go in `NavigationSystem::Runtime`.
  - No new source files are needed. If one is added, run `tools/xcode_sync_navigation.py --check`.
- **D2 Fixed-tick order (S19 D2).**
  - Step 2 adds link auto-update after the obstacle sync.
  - Step 4 runs each surface's `updateCarving(budget)`: live mesh first, then any staging mesh with
    what is left of the budget (at least one build per tick).
  - The swap of a staged mesh happens at the end of step 4.
- **D3 Obstacle descriptions are in nav space.** Re-adding after a new mesh reuses them: same
  surface, same frame. `adoptObstacles` moves the registry and queues every entry as an add on the
  new tile cache.
- **D4 No per-tile y bounds** (§2). **No obstacle splitting** (R16). **No getters patched into
  dtTileCache:** feeding is decided from our own FIFO and `update()`'s `upToDate`.
- **D5 Detecting truncation needs no patch.** A corridor that is not partial and whose last polygon
  is not the target's is truncated: the queue's search reached the target only if
  `DT_PARTIAL_RESULT` is unset (`DetourNavMeshQuery.cpp:1526-1527`).

```cpp
namespace jasperengine::nav {
// NavMesh.h
struct NavObstacleDesc { enum class Shape : uint8_t { Cylinder, Box } shape = Shape::Box;
    Vector3 position;            // cylinder: base centre; box: centre (nav space)
    float radius = 0.5f, height = 2.0f; Vector3 halfExtents{0.5f, 0.5f, 0.5f}; float yRadians = 0.0f; };
struct NavMeshCreateOptions { bool deferCarvedBuilds = false; int maxObstacles = -1; /* -1: the header's */ };
struct NavCarveBudget { int maxTileBuilds = 4; double ceilingMs = 4.0; /* 0: no ceiling */ };
class NavMesh {
    bool create(NavMeshData data, bool keepData, std::string& error, const NavMeshCreateOptions& options = {});
    bool isComplete() const;               // no queued carved builds
    int getQueuedBuilds() const;
    uint64_t getGeneration() const;        // R9
    uint32_t addObstacle(const NavObstacleDesc& desc);          // R11; 0 = not carving / full
    void moveObstacle(uint32_t id, const NavObstacleDesc& desc);
    void removeObstacle(uint32_t id);
    void adoptObstacles(NavMesh& previous);                     // R12
    int countTileLayers(const NavObstacleDesc& desc) const;     // R16
    bool updateCarving(const NavCarveBudget& budget, bool& upToDate, int& tilesRebuilt);   // R14
    bool updateLink(uint32_t userId, const std::vector<NavLinkInput>& connections, std::string& error); // queues in play (R15)
};
// NavCrowd.h
enum class NavCrowdPath : uint8_t { Complete, Truncated, Partial };
NavCrowdPath NavCrowd::getPathState(int agent) const;   // R6, D5
// NavTypes.h: NavigationSettings
int carveTilesPerTick = 0;  int effectiveCarveTilesPerTick() const;   // 0 = Auto per platform
// NavMeshBuilder.h: NavTileBuildResult
std::vector<std::string> warnings;
}
namespace jasperengine {
// NavigationSystem.h
struct NavBakeResult { /* ... */ bool logged = false; };     // R2
uint64_t NavigationSystem::getPathQueryCount() const;        // R6
void NavigationSystem::setCarveTimeCeiling(double ms);       // R14
bool NavigationSystem::getGridReservedCells(Entity grid, std::vector<int>& cells);   // R22
// NavMeshSurfaceComponent.h
int maxCarvingObstacles = 128;                               // R17
}
// editor/navigation/NavigationEditor.h
struct LastBake { bool ok, cancelled; std::string error; std::vector<std::string> warnings; int tilesBuilt, polys; double ms; };
const LastBake* NavigationEditor::lastBake(uint32_t sceneId, Entity surface) const;     // R3
// editor/navigation/NavigationOverlay.h (pure, testable without a GPU)
static void gridCellColors(const nav::NavGridMap& map, const std::array<Vector4, nav::AREA_COUNT>& areaColors,
                           const std::vector<int>& reserved, std::vector<Vector4>& out);   // R22
```

The old `addCylinderObstacle`/`addBoxObstacle` and `updateCarving(double, ...)` are replaced, not
kept beside the new calls. Their callers and tests move to the new API.

## 7. Acceptance criteria

Engine tests go in `engine/tests/core/navigation/` (fixtures `NavFixtures.h`). Editor tests go in
`editor/tests/navigation/`. MCP tests go in `tools/mcp/tests/`. "Red" names the run that fails
without the fix. Sanitizer means `SANITIZE=address,undefined tools/build.sh unit-tests` (a Debug
build, so asserts are on).

| AC | Verifies | Check (red control) |
|---|---|---|
| AC1 | R1, R6, R7 | `NavCrowdTest.CorridorAtCapacity` (`NavCrowdTest.cpp`): a 600 m × 3 m strip on 16-cell tiles of 0.1 m, so the corridor has ≥ 300 polygons. A `dtAssertFailSetCustom` handler (`#ifndef NDEBUG`) records 0 failures; `pathStatus` is Complete while truncated; an agent at speed 20 arrives once, at the destination, after the extension. Red: sanitizer before patch 2 records `npath < m_maxPath` |
| AC2 | R1 | `NavCrowdTest.MergeCorridorStartMovedBounds`: a direct `dtMergeCorridorStartMoved` call with maxPath 4, 8 visited, and canaries after the buffer. It returns ≤ 4 and leaves the canaries intact. Red: before patch 3, ASan reports a negative-size `memmove` (the canaries break in Release) |
| AC3 | R1 | `NavBakeTest.DetailSamplingDegenerate`: a sanitizer bake of slopes far from the origin (100 km) succeeds. If no geometry reproduces #796, the AC states that no red control could be built and the patch is upstream's one line (recorded in Progress) |
| AC4 | R1 | `test_docs.py` and `tools/ci/check-notices.py` pass. Manual: `grep -rn "JasperEngine patch" engine/libs/recastnavigation` lists the lines of patches 1-4, and every `PATCHES.md` entry names its upstream hash |
| AC5 | R2, R3, R24 | `test_navigation.py::test_bake_failures_reported`, three cases, each with exactly one Console error line naming the surface (`read_console` cursor) and `get_navigation_info` `last_bake.ok == false` with the reason: (a) a refused start ("Surface too large"); (b) a tile failure (carving, Tile Size 300); (c) a save failure (the `.navmesh` path is a directory). Then a good bake gives `last_bake.ok` true. Red: (a) logs 0 lines before; `last_bake` is missing in all three |
| AC6 | R3 | Manual, with screenshots from `tools/editor-shot.sh` on a scratch project: after case (b) of AC5, the Inspector shows the red "Bake failed: tile (0, 0): runtime carving needs a tile size of at most 255 cells" and the Surfaces tab shows "Bake failed" with that tooltip; a good bake clears both |
| AC7 | R4 | `NavBakeTest.RecastLogContext`: `NavRecastLog` keeps warnings and errors, rewrites `%p` values as `?`, drops "Expanding max contours" and makes no allocation without a log (counting allocator). `NavBakeTest.RecastWarningsReported`: 70 stacked 3 × 3 m slabs, 2.5 m apart, on a static surface bake ok with a warning containing "Heightfield has too many layers"; on a carving surface the bake fails with an error containing "layer overflow". `test_navigation.py::test_bake_warnings`: `bake_navigation` returns the same warning. Red: no warning, and the generic error, before |
| AC8 | R5 | `NavBakeTest.TallSurfaceWarns` (geometry 700 m tall at the default voxel size) and `NavBakeTest.LayerCapWarns` (40 slabs, carving: "40 walkable layers, the lowest 32 kept"). Red: no warning before |
| AC9 | R6 | `NavAgentTest.PathStatusWithoutSearch`: 20 agents, destinations and a carving obstacle that forces replans; `getPathQueryCount()` stays 0 across every arrival and replan, and each status matches `calculatePath` run after the window. Red: the old code calls `calculatePathOn` once per arrival (the counter goes up) |
| AC10 | R7 | `NavAgentTest.PartialPathNoArrival`: the two islands of `PathFailedFires`, run for 900 ticks: the agent stops at the near edge, `pathStatus` is Partial, `onPathFailed` fires once, `onDestinationReached` fires 0 times. Red: it fires once before (to be confirmed by that run) |
| AC11 | R8 | `NavAgentTest.ExtensionKeepsPathWithoutAutoRepath`: the AC1 strip with Auto Repath off; the agent arrives and `onPathFailed` fires 0 times. Red: the path is dropped at the extension |
| AC12 | R9 | `NavCrowdTest.CrowdRebuiltOnNewMesh`: `updateTiles` grows the tile count past the pool, so `create()` runs inside the mesh; the generation changes, every agent is re-placed and `isOnNavMesh` holds. A pointer-equality red depends on the allocator; record whether the old code failed it |
| AC13 | R10 | `NavAgentTest.LinkAutoUpdateStatic` (the link moves 2 m within its tile: the path's link corner follows within 1 tick), `NavAgentTest.LinkAutoUpdateCarving` (its start moves into the next tile; a carving obstacle's cut in the touched tile survives), `NavAgentTest.LinkAutoUpdateLimits` (Auto Update off leaves the link in place; on a static surface a start leaving its tile warns once and keeps the old link). Red: the first two fail before |
| AC14 | R11 | `NavObstacleTest.RemoveNeverLost`: 70 carving boxes added in one tick, disabled the next; within the budget the path is restored and no tile-cache obstacle is left. `NavObstacleTest.ManyObstaclesManyTiles`: 40 boxes, each straddling 4 tiles of a 12 × 12 tile surface, appear in one tick; within the budget every one blocks its own probe path. Red: 6 cuts stay; tiles past the 64 update entries are never cut |
| AC15 | R11, R17 | `NavObstacleTest.MaxCarvingObstaclesRespected`: with a maximum of 4, 6 carving obstacles give 4 cuts, 2 avoided obstacles and one warning each. Red: no warning, and silent retries before |
| AC16 | R12 | `NavObstacleTest.CutsSurviveRebakeAndReload` (in play: a full `bakeAsync`, then `reloadNavMesh`; the stationary cut still blocks the path) and `NavObstacleTest.CutsSurviveUpdateTiles` (`updateTiles` over the obstacle). Red: the path opens before |
| AC17 | R13 | `NavObstacleTest.RecutThresholds`, on a 6 × 6 m box (r = 4.24 m) with threshold 0.1: 1° of yaw does not re-cut (the revision is unchanged) and 3° does; a capsule turned 90° does not; a scale change does; ±1 mm of jitter per tick still becomes stationary and is cut. Red: before, 3° does not re-cut, the capsule re-cuts, and the jitter never cuts |
| AC18 | R14 | `NavObstacleTest.CarveCountBudget`: with Carve Tiles Per Tick 1 and the ceiling off, an obstacle touching 3 tiles is fully cut exactly at tick 3. `NavObstacleTest.CarveBlocksAndRestores` lands in 2 ticks in every build (its sanitizer branch at `NavCrowdTest.cpp:99-113` removed). `NavDeterminismTest.CarvingReplay`: 2 runs × 600 ticks, with moving carving obstacles and 20 agents, give bit-identical positions. Red: before, the 1 ms wall-clock budget lands at tick 1 |
| AC19 | R15 | `NavBakeTest.RuntimeCarvingBakeDeferred`: in play, a full `bakeAsync` of a 64-tile carving surface. When the bake lands the old `NavMesh` stays in service; no tick does more than the budget's builds; an agent has a valid corridor every tick; `onBakeCompleted` fires on the swap. `NavBakeTest.RuntimeUpdateTiles` also passes on a carving surface. Red: the mesh is swapped in the landing tick |
| AC20 | R16 | `NavObstacleTest.LargeObstacleWarns`: a 20 m box on 3.2 m tiles warns once, naming the layer count. Red: silent before |
| AC21 | R18 | `tools/build.sh web` and `tools/build.sh android` build; the S19 web gate (single-threaded, AC20 there) passes with `NavObstacleTest`'s carving fixture added to its scene list |
| AC22 | R19 | `NavSystemTest.SteadyStateNoAllocations`: 10 walking grid and navmesh agents, 2 obstacles (one carving and stationary); ticks 120-240 of `nav->fixedUpdate` allocate 0 (counting allocator). Red: `syncObstacles` and `sortedAgents` allocate. The `je-bench-nav` thresholds pass (`tools/perf/check_bench.py`, median of 5 on E) |
| AC23 | R20, R27 | `ReflectRegistry.CatalogSnapshot` with the regenerated snapshot (new labels, the `maxCarvingObstacles` field); `NavigationStream.NavMeshSurfaceReEncodesIdentically` covers the new field; `NavigationStream.OldSurfaceLoadsDefaults`: S19-era surface YAML loads with 128; `NavigationProjectSettings.DefaultsWriteNothing` and `GeneratedCodeSetsThem` cover `carveTilesPerTick` |
| AC24 | R21 | `tools/mcp/tests/test_navigation_ui.py::test_brush_size_keys` (new file, `EDITOR = {"xvfb": "harness", ...}`, `ui_input`): select the grid, click Inspector "Paint" (`find_ui_item`), then `run_command id="terrain.brushLarger"` succeeds and the brush row shows size 2. Red: `run_command` is refused as disabled |
| AC25 | R22 | `NavigationOverlay.ReservationTint` (`NavigationEditorTest.cpp`): `gridCellColors` blends exactly the reserved cells. `NavGridTest.ReservedCellsReported`: two grid agents in play, and `getGridReservedCells` returns their lookahead cells. Red: neither API exists (no tint) |
| AC26 | R17, R23 | `test_navigation.py::test_carving_settings_undo`: `set_component` Max Carving Obstacles and `set_navigation_settings carve_tiles_per_tick=1` both undo; `get_navigation_info` shows both |
| AC27 | R24 | `test_navigation.py::test_tool_play_policy`: in play, `bake_navigation` and `paint_nav_grid` are refused by the server's play policy, and `visualize_navigation` with a `path` of `agent_type` 1 draws on the running scene after a scene load. The listing shows `bake_navigation` destructive. `test_tool_conventions.py` passes |
| AC28 | R28 | `test_navigation.py::test_overlay_golden` fails when the golden is renamed away (red: it passed and wrote it). `tools/check-navigation-off.sh` reports ≥ 2 `NavStubTest` tests passed; `python3 -I tools/test_check_changed.py` covers the new plan entry |
| AC29 | R28 | `tools/mcp/tests/test_navigation_export.py::test_desktop_export_ships_navmesh` (`PREBUILT_PLAYERS = ["glcore-release"]`): a baked scene with a Lua marker on `onDestinationReached`. `tools/jpak.py list` shows the `.navmesh` in the pack, and the headless player logs the marker. A moved static box makes the build's preflight warn (tool "navigation") |
| AC30 | R26 | `test_docs.py`; `tools/mcp/gen_tool_docs.py --check`; `tools/mcp/tool_budget.py --check tools/mcp/budget.json`; `tools/lua_api/tests/test_lua_api.py` |

## 8. Dependencies & conflicts

- **Depends on:**
  - S19, landed: all of its code.
  - S08 (reflection tables), S24 (undo of rows and settings), S30 (prebuilt players, for AC29),
    S25 (`run_command`, for AC24), S02 (sanitizer presets, the web gate, `check_bench.py`).
- **Blocks:**
  - S54, which builds failure reasons on R6's status and findings on R4's warnings.
  - S55, which builds Link and Obstacle handles on R10-R12 and the agent API on R6-R8.
  - Both rebase on S53.
- **Merge hotspots:**
  - `NavigationSystem.cpp` and `NavCrowd.*` (S55).
  - `NavigationTools.cpp`, `NavigationOverlay.*`, `NavigationWindow.cpp` and the
    `get_navigation_info` schema (S54).
  - `InspectorNavigation.cpp` and `NavigationReflect.cpp` (S54, S55).
  - `tools/mcp/budget.json` and `docs/mcp/TOOLS.md` (every MCP stream).
  - `editor/tests/reflect/catalog_snapshot.json` (any reflection change).
  - `NOTICE.md`.
  - `tools/check_changed.py`.

## 9. Risks & defaults

| Risk / question | Default taken |
|---|---|
| A tile cache that never reports `upToDate` would stall the FIFO | Feeding waits only on `update()`'s own flag, so a failed build still pops its tile (`DetourTileCache.cpp:588-591`); `RemoveNeverLost` covers it |
| A desktop Auto of 4 builds over 1 ms on the timing server | The bench decides: lower Auto to 2 and record it in Deviations |
| Deferred builds delay a runtime carving rebake by about tiles / budget ticks (400 tiles at 4 per tick ≈ 1.7 s) | Accepted: the old mesh stays in service (R10); the docs say so |
| Partial paths no longer fire `onDestinationReached`, which some scripts may rely on | It is a fix (S19 R12 says "of the destination"); recorded in `docs/navigation.md` "Changes" and the guide's Pitfalls |
| The R5 warning lets clamped bakes ship | A hard error would refuse scenes that work today; S54's findings can raise it to an error |
| Static links cannot move across tiles | A warning, with Runtime Carving as the fix (R10); growing static tiles is out of scope |
| A `%p` rewrite merges different pointers | Intended: the messages are for people, and deterministic results matter more |
| The AC3 red control may not be constructible | Recorded in Progress; the patch is upstream's |

## 10. Work packages

Two implementers work in separate worktrees, each on its own branch, and merge into one
integration branch.
- **P1** lands an interface commit first: the §6 declarations with their default implementations.
  It is named `nav: S53 interfaces` and touches only headers and stubs.
- **P2** merges that commit before its step 3.
- After that the packages share no file except as noted.

**P1, engine runtime.** It owns:
- `engine/libs/recastnavigation/**` (patches, `PATCHES.md`, `README.jasper.md`) and `NOTICE.md`.
- In `engine/core/navigation/`: `NavMesh.*`, `NavMeshBuilder.*`, `NavDetourSupport.*`,
  `NavBakeCollector.cpp`, `NavCrowd.*`, `NavTypes.*`.
- `engine/core/subsystem/NavigationSystem.*` and `engine/core/component/NavMeshSurfaceComponent.h`.
- `engine/tests/core/navigation/**`, `engine/tests/core/bench/NavBench.cpp`,
  `tools/perf/bench-thresholds.json`, `docs/navigation.md` and `docs/scripting.md`.

P1 order:
1. The interface commit.
2. R1 and AC1-AC4.
3. R4 and R5 (AC7, AC8).
4. R6-R9 (AC9-AC12).
5. R11, R13, R14, R16 and R17 engine side (AC14, AC15, AC17, AC18, AC20).
6. R12 and R15 (AC16, AC19).
7. R10 (AC13).
8. R19 and the bench (AC22).
9. R25 and the runtime docs, then AC21.

**P2, editor, MCP and test infrastructure.** It owns:
- In `editor/navigation/`: `NavigationEditor.*`, `NavigationWindow.cpp`, `NavigationOverlay.*`,
  `NavigationProjectSettings.cpp`.
- In `editor/`: `window/InspectorNavigation.cpp`, `reflect/components/NavigationReflect.cpp`,
  `StreamNavigation.cpp`, `FactoryNavigation.cpp`, `AppCommands.cpp`,
  `window/dialog/ProjectSettingsWindow.cpp`, `settings/SettingsRows.cpp`, `mcp/tools/NavigationTools.cpp`.
- `editor/tests/navigation/**` and `editor/tests/reflect/catalog_snapshot.json`.
- `tools/mcp/tests/test_navigation.py`, `test_navigation_ui.py` and `test_navigation_export.py`.
- `tools/check-navigation-off.sh`, `tools/check_changed.py`, `tools/test_check_changed.py` and
  `tools/mcp/budget.json`.
- `docs/mcp/guide/navigation.md`, `docs/mcp/TOOLS.md`, `docs/testing.md`, the S19 Deviations and
  the README index.

P2 order:
1. R24's tool fixes that need no P1 API, and the golden fix (AC27, AC28 first half).
2. R20 and R21 (AC23 labels, AC24).
3. After merging P1's interfaces: R2 and R3 (AC5, AC6).
4. R17 and R23 serialization, rows and codegen (AC23, AC26).
5. R22 (AC25).
6. The OFF gate and the export test (AC28, AC29).
7. Docs and regeneration (AC30).
8. Its paragraphs for `docs/navigation.md` (bake failures, brush keys), added last on top of P1's
   version.

## Deviations

- S53 P1: R15, a full bake landing in play: the staged mesh adopts the obstacles when the bake
  lands, not at the swap. Its tiles are then built with their cuts and the swap leaves no tick
  without them; until the swap, obstacle changes reach the staged mesh and the mesh in service
  keeps the cuts it had. The swap waits for the adopted cuts (`NavMesh::isComplete`).
- S53 P1: R15, `replaceTiles`' pool-growth fallback (a new Detour mesh inside the same `NavMesh`)
  builds every tile at once even in play: no old Detour tile survives it to stay in service.
- S53 P1: R14, a deferred build is one tile (x, y) with all its layers, counted as one build, so
  a tile never serves half old and half new layers.
- S53 P1: R11, an add whose obstacle touches no tile-cache layer is not handed to Detour (it would
  cut nothing, and Detour never frees the slot of such an obstacle); it is handed over when
  `replaceTiles` adds layers under it. Detour's slots are Max Carving Obstacles + 8 (the removals
  of one batch in flight).
- S53 P1: R6, `onPathFailed` and `pathStatus` read dtCrowd's corridor; `cancelBake` cannot cancel
  a bake that landed and is building its carved tiles (`isBaking` stays true until they are done).
- S53 P1: R6, after a tile change (the mesh revision) the crowd checks whole corridors, not only
  dtCrowd's next 10 polygons, and plans a corridor holding a polygon that is gone again: a cut
  far ahead otherwise left a stale Complete status until the agent reached it (AC9). A status is
  also read again whenever the corridor's state changes, not only after a Pending tick (dtCrowd's
  quick search can land a plan within one step).
- S53 P1: R4, a tile keeps at most 64 Recast messages, then `... and N more Recast messages`.
- S53 P1: interface additions beyond §6, used by the tests and the editor: `NavMesh::hasObstacle`,
  `getTileCacheObstacleCount`, `getFailedCarveBuilds`, `getLinkConnections`,
  `setDeferCarvedBuilds`; `NavCrowd::getCorridorSize`; `NavSurfaceStatus::queuedBuilds`;
  `NavigationSystem::getCarveTileBuilds`; `detail::NavRecastLog` in `NavDetourSupport.h`.
- S53 P1: R19 needed `GridReservations::reserve` (`GridPathfinder.cpp`) to reuse an agent's cell
  list: it allocated on every reservation.
- S53 P1: AC7, a stack of slabs alone never overflows Recast's layer set (a column keeps at most
  63 regions, each overlapping at most 62 others): the carving case adds a ground plate under the
  stack and 10 slabs beside it, so the plate overlaps more than 63 platforms.
- S53 P1: AC14 RemoveNeverLost, 70 boxes disabled the tick after they appear do not lose a cut
  (the removals follow the adds' tile order); the test disables 60 cut boxes while it enables 10
  others, 70 requests in one tick, which is what loses cuts in the S19 code.
- S53 P1: AC21's carving fixture lives in `tools/test-projects/navigation` (the web gate's and the
  Android test's project), with two gate lines.

## Progress
