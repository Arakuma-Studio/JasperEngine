# S75 — Navigation diagnostics ("N1: navigation explains itself")

Wave 4 (navigation follow-up to S19; parallel with S74 fixes and S76 agent API). OURS paths are
relative to the repo root; recastnavigation's are relative to `/tmp/recast-analysis/src` (the
upstream checkout, zlib, read-only).

## 1. Goal

When navigation does not work, a person in the editor and an agent over MCP must both be able to
find out **why**, from the same core operations. Every failed or partial path names a reason from
one fixed vocabulary (`nav::PathFailure`) and the surface that answered. Navmeshes know their
islands and walkable area. A point can be located and, if it is off the mesh, explained (too steep,
too low, too close to a wall, an excluded mesh...). Bakes report what they collected and why they
failed. One findings list feeds the Navigation window, `get_navigation_info`, the CLI `check` and
the export preflight. Humans get a Path Tester, a "why not walkable?" probe and diagnostic overlays.

## 2. Scope

**In scope**
- Core: `PathFailure` and its detection, richer `NavPath`, island labelling, `locate`, the
  walkability probe, findings, the bake report (sources, islands, error codes, per-tile timings).
- MCP: new parameters and result fields on the existing `query_navigation`, `bake_navigation`,
  `get_navigation_info` and `visualize_navigation` (no new tool).
- Lua: path reason fields, `locate`, `getCoverage`, `getIslands`, `NavMeshSurface.lastBakeResult`.
- Editor: Path Tester tool, probe popup, island / slope / tile colour / tile label / dirty tile
  overlays, Navigation window "Last Bake" column and Problems list, Inspector bake summary and
  pre-bake estimate.
- CLI `check` and export preflight on the shared findings.
- Docs: `docs/navigation.md`, `docs/mcp/guide/navigation.md`, `docs/mcp/TOOLS.md` (regenerated).

**Out of scope (owned elsewhere; do not specify or implement)**
- S74: silent bake-failure logging, crowd path-arrival A*, link auto-update, carving obstacle
  defects, Recast `rcContext` warnings (S75 only consumes them from `NavBakeResult::warnings`),
  `visualize_navigation`'s path filter, play-scene use and activity, the modifier label.
- S76: random point, wall distance, `moveAlongSurface`, area costs, runtime area toggles, the agent
  API (setPath, steeringTarget, root motion, stuck watchdog, `onPathFailed(agent, reason)` using
  `PathFailure`, `agent_detail` stuck fields), Lua handles for Obstacle, Link and Modifier,
  `NavPath::cost` and per-corner areas.
- A runtime debug draw for games (S19 D8 stays: overlays are editor-only).
- Pathing across surfaces (S19 out of scope).
- Do-not-copy list (README): nothing here touches it. RecastDemo is UX reference only; we write our
  own union-find and overlays (no RecastDemo/DebugUtils code copied, so no NOTICE.md change).

## 3. Current state

All verified by reading the worktree at `b4f7816ea`.

- **Path answers carry no reason.** `NavPath` is `{status, corners, length}`
  (`engine/core/navigation/NavTypes.h:78-83`). `NavMesh::findPath` returns a default (Invalid) path
  when the mesh is invalid, when either end finds no polygon, or when Detour fails
  (`NavMesh.cpp:945-962`); it never reads `DT_OUT_OF_NODES` (`:960-965` only test
  `DT_PARTIAL_RESULT` and the last polygon).
- **Surface choice is silent.** `NavigationSystem::calculatePath` returns the first Complete path
  over the agent type's surfaces in entity order, else the first Partial (`NavigationSystem.cpp:938-948`),
  and does not say which surface answered. `isReachable` is `calculatePath(...).status == Complete`
  (`:990-992`): a full A* even when the ends can never connect.
- **MCP.** `query_navigation`'s `reachable` returns only a bool (`NavigationTools.cpp:517-521`).
  `from`/`to` accept only `[x, y, z]` (`parsePoint`, `:327-333`). A `target` surface silently
  replaces `agent_type` (`:477`). `visualize_navigation` draws `calculatePath(from, to)` with the
  default filter (`:782-793`), captures only `view = "scene"` (`:798-806`) and cannot frame
  anything; `SceneWindow::focusOnBounds` exists but is private (`editor/window/SceneWindow.h:119`,
  private section `:23-130`).
- **Bake results are free text.** `NavBakeResult` is `{ok, cancelled, error, tilesBuilt, tilesTotal,
  polys, milliseconds, warnings}` (`engine/core/subsystem/NavigationSystem.h:51-60`); its JSON is
  `bakeResultJson` (`NavigationTools.cpp:116-123`). The collector's `NavCollectStats`
  (`NavBakeCollector.h:120-132`) are kept in `Runtime::Surface::bakeStats` but only their
  `warnings` reach the result (`NavigationSystem.cpp:750`). Collect failures are strings
  (`NavBakeCollector.cpp:1054`, `:1094`, `:1114`). Each tile's build time is recorded
  (`NavMeshBuilder.h:52`, `NavMeshBuilder.cpp:584`) and dropped when tiles land
  (`NavigationSystem.cpp:764-777`).
- **No islands, walkable area or coverage** anywhere (`grep -ri island engine/core/navigation`
  finds nothing). Off-mesh link connection state is not exposed.
- **Findings** exist only for unbaked, unreadable or stale surfaces:
  `NavigationEditor::checkSurfaces` (`editor/navigation/NavigationEditor.cpp:520-557`), used by the
  CLI `check` (`editor/cli/CommandLine.cpp:3042-3047`, all as `"warning"`) and the export preflight
  (`editor/Exporter.cpp:2330-2333`). The crowd's 17th area mask only logs once
  (`engine/core/navigation/NavCrowd.cpp:104-121`).
- **Editor.** No Path Tester: the overlay's query path is set only by MCP
  (`NavigationEditor::setQueryPath`, `NavigationEditor.h:109-115`). Overlay flags are NavMesh, tile
  bounds, heightmesh, grid, paths, avoidance, links, obstacles (`NavigationOverlay.h:35-58`; Scene
  view rows `SceneWindow.cpp:2439-2466`). The Navigation window's Surfaces tab shows state,
  tiles/polys, memory (`NavigationWindow.cpp:192-260`). The Inspector shows state and the stale
  warning, no last bake (`InspectorNavigation.cpp:277-296`), and a voxel-size line without voxel or
  tile counts (`:222-225`). The stale check recollects at most once a second per surface and keeps
  only the count (`NavigationEditor.cpp:397-418`, `NavigationSystem.cpp:486`).
- **Lua.** The path table is `{status, corners, length}` (`NavigationClassesLua.cpp:116-125`); its
  hand-written stub is `engine/core/script/binding/stubs/Navigation.lua:5-9`; the generator's nav
  overrides are `tools/lua_api/parser.py:2920-2935`.
- **Budgets (inferred by rebuilding the schemas in Python from `NavigationTools.cpp:848-946`):**
  the `navigation` group is ~6,221 of 15,000 characters with 6 of 12 tools; `query_navigation`
  ~1,238 and `visualize_navigation` ~1,083 of the 1,500 per-tool default. The agent guide is
  16,366 of 16,384 bytes (S19 Progress) with one navigation line (`McpInstructions.cpp:175`).

## 4. Reference (recastnavigation RecastDemo, zlib — UX only)

| Upstream | Take | Improve |
|---|---|---|
| `RecastDemo/Source/Tool_NavMeshTester.cpp:422-431` (`onClick`: Shift sets start, click sets end, `recalc()`), `:224-282` (modes) | The click model and the mode list (Pathfind, Raycast, Distance to Wall...) | Draggable handles, agent type / area mask from project settings, a reason line, the same core call as MCP |
| `RecastDemo/Source/Tool_NavMeshPrune.cpp:30-70` (`floodNavmesh` over `tile->links`) | Components walk polygon links, off-mesh links included | Union-find over every link (direction ignored), all islands at once, cached per revision |
| `RecastDemo/Source/Sample_TileMesh.cpp:522-534` (`"%.3fms / %dTris / %.1fkB"` at the tile centre) | Per-tile build label | Every visible tile, from the last bake, dirty tiles outlined |
| `DebugUtils/Source/DetourDebugDraw.cpp:126,144` (`DU_DRAWNAVMESH_COLOR_TILES`, `duIntToCol`) | Colour by tile | Our own hash-to-hue; colour by island |

Detour facts the design relies on (vendored `engine/libs/recastnavigation`): an off-mesh
connection's polygon links its start ground polygon with `edge = 0` (`DetourNavMesh.cpp:593`) and
its end with `edge = 1` (`:494`); `findPath` sets `DT_OUT_OF_NODES` when the node pool is exhausted
(`DetourNavMeshQuery.cpp:1162`).

## 5. Requirements

### Functional — path failures

- **R1 Contract.** `engine/core/navigation/NavTypes.h` defines, exactly (shared with S76):
  `enum class PathFailure : uint8_t { None, NoSurface, NotBaked, StartOffMesh, EndOffMesh,
  StartFiltered, EndFiltered, Disconnected, BlockedByArea, OutOfNodes };` and
  `const char* pathFailureName(PathFailure)` returning `"none", "no_surface", "not_baked",
  "start_off_mesh", "end_off_mesh", "start_filtered", "end_filtered", "disconnected",
  "blocked_by_area", "out_of_nodes"`. Values may only be appended. `pathFailureDescription` gives
  one English sentence per value; `pathFailureFromName` parses a name.
- **R2 Invariant.** Every `NavPath` a query returns has `failure == None` exactly when
  `status == Complete`. Invalid goes with NoSurface, NotBaked, StartOffMesh, EndOffMesh,
  StartFiltered, EndFiltered; Partial with Disconnected, BlockedByArea, OutOfNodes. Corners, length
  and status of every path are unchanged from S19 (only fields are added).
- **R3 Detection, in this order** (first match wins; "filter" is the query's area mask and costs,
  "all areas" the same costs with mask `ALL_AREAS`, the search box is the surface's existing
  `halfExtents`):
  1. `NoSurface`: no enabled, active `NavMeshSurface` of the agent type (or the given surface is
     not one).
  2. `NotBaked`: such surfaces exist but none has a valid navmesh (never baked, cleared, asset
     refused). A Detour failure status on valid references also maps here (logged once).
  3. `StartOffMesh` / `StartFiltered`: no polygon in the search box around `from` with the filter;
     Filtered when one exists with all areas. Then the same for the end (`EndOffMesh` /
     `EndFiltered`).
  4. `OutOfNodes`: `findPath` returned `DT_OUT_OF_NODES` or `DT_BUFFER_TOO_SMALL` (the 2048-node /
     2048-polygon budget, S19 R26).
  5. Partial with a mask other than `ALL_AREAS`: the path is searched again with all areas;
     Complete → `BlockedByArea`; out of nodes → `OutOfNodes`; else `Disconnected`.
  6. Partial with all areas → `Disconnected` (no connection at all, or only one-way links).
  When the islands are current (R7) and the ends' islands differ, step 5's second search is
  skipped (`Disconnected` at once); the first search still runs, so a Partial path still leads to
  the closest point (S19 behaviour).
- **R4 NavPath fields.** `failure`; `surface` (the answering entity); `start`, `end` (the ends
  snapped to the mesh) with `startSnapDistance`, `endSnapDistance` (< 0 when that end found no
  polygon); `startIsland`, `endIsland` (-1 unless the islands were current); for Partial,
  `closestPoint` (the last corner) and `gap` (from it to the snapped end, or to the requested end
  when the end is off the mesh); for Complete, `closestPoint` is the last corner and `gap` 0.
  S76 appends `cost` and per-corner areas after these fields.
- **R5 Surface ranking.** With several surfaces of the agent type, the first Complete one (entity
  order) answers. Otherwise the answer with the lowest rank wins: OutOfNodes < BlockedByArea <
  Disconnected < EndFiltered < EndOffMesh < StartFiltered < StartOffMesh < NotBaked < NoSurface,
  ties by smaller `gap`, then entity order. `surface` names it.
- **R6 isReachable** returns `false` without an A* when the islands are current and differ
  (O(1)), and can report the `PathFailure` (`isReachable(from, to, filter, &why)`).

### Functional — islands, coverage, locate

- **R7 Islands.** A navmesh's islands are the connected components of its polygons whose flags are
  non-zero, joined through polygon links and off-mesh connection links in either direction. Per
  surface: `walkableArea` (sum of ground polygon areas, nav units² converted to m²; a 2D surface
  divides by points-per-metre²), and per island `{id, area, polys, bounds}`. Island ids are the
  discovery order over tiles ascending (y, x, layer) then polygon index: deterministic for a given
  navmesh. Lookup polygon → island is O(1) (a label per polygon, per tile).
- **R8 When labelled.** Islands are recomputed lazily on the main thread when
  `NavMesh::getRevision()` differs from the labelled revision. The revision already changes on
  create, clear, `replaceTiles` (bake landing, tile swaps), carving rebuilds and link moves; a new
  `NavMesh::setPolyFlags(polyRef, flags)` is the only way to change flags and bumps it too (S76's
  doors build on it). Path queries never relabel (they use islands only when current, R3/R4/R6);
  explicit diagnostics do: `getIslands`, `getCoverage`, `locate`, findings, the bake report, the
  overlay, the MCP tools. Bakes never label inside the landing (`processBakes`), so web slices keep
  their 4 ms (S19 R23).
- **R9 Coverage** per surface: `{walkableArea, islandCount, largestIslandArea, tilesArea
  (non-empty tiles × tile footprint), fraction = walkableArea / tilesArea}`.
- **R10 locate(point, filter, maxDistance = 5)** → `{onMesh, found, position, snapDistance, island,
  area, surface}`: `onMesh` when a polygon is in the surface's search box (where a path from here
  would start); otherwise `found`/`position` from a search within `maxDistance`. Over several
  surfaces: the on-mesh one with the smallest snap distance, else the closest found.

### Functional — why not walkable (editor-only)

- **R11 Probe.** `NavWalkabilityProbe::explain(scene, system, surface, worldPoint, filter, input?)`
  (editor) returns the location plus ordered reasons `[{code, message, entity?, value?, limit?}]`:
  `not_baked`, `stale_tile` (the tile under the point is dirty), `outside_volume` (Collect Objects =
  Volume), `outside_bounds`, `no_geometry`, `excluded` (`value` names why: layer, moving body,
  agent, obstacle, Exclude modifier, inactive; `entity` = the excluded GameObject),
  `not_walkable_area` (`entity` = the modifier or modifier volume), `filtered` (on the mesh, area
  outside the mask), `too_steep` (slope vs agent Max Slope), `low_clearance` (free height vs agent
  Height), `near_wall` (within agent Radius of an edge), `step_too_high` (a ledge above Step
  Height), `small_region` (removed by Minimum Region Area), `small_island` (on the mesh, but on an
  island that is not the largest, with its area). An on-mesh point returns only `filtered` /
  `small_island` / `stale_tile` if they apply.
- **R12 Probe data.** The collector gains `NavCollectOptions{recordSources}`: with it,
  `NavBakeInput` keeps `triangleSources`, `terrainSources`, `volumeSources` (one entity per item) and
  `NavCollectStats::excludedEntities` (`{entity, reason}`, layer exclusions included). Sources never
  enter tile hashes (S19 R40a holds) and are empty without the option. The geometric checks use
  the input's triangles under the point (slope from the triangle normal; clearance from triangles
  above within the agent height); `near_wall`, `step_too_high` and `small_region` are decided by
  rebuilding only the tile under the point with `walkableRadius = 0`, `walkableClimb =
  walkableHeight`, or `minRegionArea = mergeRegionArea = 0` respectively and testing whether a
  polygon then covers the point (`NavMeshBuilder::buildTile` gains a config override).

### Functional — findings

- **R13 One function.** `NavigationSystem::getFindings(options)` returns findings `{code,
  severity (info|warning|error), entity, message, details}` sorted by severity (error first),
  entity, code. The editor wrapper `collectProjectFindings(project)` runs it over every loaded scene
  with a file and adds the scene. The Navigation window Problems list, `get_navigation_info`, the
  CLI `check` and the export preflight all use it; nothing else produces navigation findings.
- **R14 Codes** (messages name the GameObject and the fix):

  | Code | Severity | Entity | When (details) |
  |---|---|---|---|
  | `not_baked` | warning | surface | enabled, nothing baked (S19's message text kept) |
  | `navmesh_unreadable` | error | surface | the asset was refused (`loadError`) |
  | `stale_bake` | warning | surface | dirty tiles > 0 (`dirty_tiles`) |
  | `bake_failed` | error | surface | the session's last bake failed (`error_code`) |
  | `agent_off_mesh` | warning | agent | edit: its feet (position − base offset) not on a surface of its type (`snap_distance`); play: `isOnNavMesh` false |
  | `agent_no_surface` | warning | agent | no surface of its agent type and no Nav Grid contains it |
  | `agent_type_missing` | error | surface/agent | its agent type id is not in the settings (it falls back to the first) |
  | `agent_type_unused` | info | none | an agent type no surface or agent uses (`agent_type`) |
  | `link_unconnected` | warning | link | an end of a baked link lands on no polygon (`island_start`, `island_end`, -1 = unconnected; `connections`) |
  | `small_island` | info | surface | an island < 5 % of the walkable area, not the largest, with no agent on it and no link end (`island`, `area_m2`); at most 5 per surface, largest first |
  | `crowd_filter_overflow` | warning | surface | more than 16 distinct area masks among the agents of one (surface, agent type) crowd (`masks`) |
  | `obstacle_carve_ignored` | warning | obstacle | Carve on a surface without Runtime Carving |

- **R15 Stale counts** come from `options.dirtyTilesOf` when set (the editor's cache); otherwise
  `options.stale` collects (CLI, export). `stale = false` skips `stale_bake`.

### Functional — bake report

- **R16 Report.** `NavBakeResult` gains `errorCode` (`nav::NavBakeError`: `none, no_surface,
  unavailable, busy, nothing_collected, too_large, tile_failed, navmesh_failed, cancelled,
  save_failed`), `hint` (one fixed sentence per code), `sources` (the `NavCollectStats` counts:
  meshes, instances, terrains, colliders, colliders 2D, volumes, links, excluded, triangles), the
  built tiles' `{x, y, layer, ms, polys, bytes}`, and, filled on first read (R8), `walkableArea`,
  `islandCount` and the 16 largest islands. `warnings` stays the one list (S74 adds Recast's).
- **R17 Tile reports persist per surface** in memory: each bake replaces the entries of the tiles
  it built and drops removed ones, so overlay labels show every tile's last build. They are not
  saved (no `.navmesh` change); after a reload a tile's ms is unknown (`-1`, shown "– ms").

### Platform & backends

- **R18** All new runtime code is CPU-only and runs on every export target; overlays use the
  existing `Mesh`/`LineRenderer` path (all sokol backends); tile labels are ImGui text. No compute,
  SSBO, bindless, indirect draw or 64-bit atomic. `JASPERENGINE_NAVIGATION=OFF` stubs return
  `NotBaked`, no islands and an empty location, logging once (`NavStubTest`).
- **R19** Web without threads: labelling, findings and the probe run on the main thread on demand
  only; nothing new runs inside a bake slice.

### Performance (release; mobile = Pixel 6a / iPhone 12 class; web = wasm, Chrome, one thread; `bench_nav_city` ≈ 20k polygons, 400 tiles)

- **R20 Zero cost.** A scene without navigation components: S19 R25 unchanged (`empty_tick_us`,
  `empty_tick_allocations`). With navigation but no diagnostics asked: no island arrays, no
  probe data, no overlay objects; a Complete `calculatePath` adds no allocation and no search.
- **R21 Island labelling** of `bench_nav_city`: ≤ 1.5 ms desktop, ≤ 5 ms mobile, ≤ 6 ms web;
  memory ≤ 4 B per polygon + 48 B per island; a carving surface relabels at most once per fixed
  tick, and only when a diagnostic asks.
- **R22 Queries.** `query_path_p95_ms` (S19 R26) stays within its threshold with the new fields;
  a failed path costs at most two searches; a disconnected `isReachable` reject ≤ 2 µs desktop;
  `locate` p95 ≤ 0.03 ms desktop (islands current).
- **R23 Findings** of `bench_nav_city` + 100 agents + 20 links with cached stale counts: ≤ 2 ms
  desktop, ≤ 6 ms mobile, ≤ 8 ms web; allocations only for the result.
- **R24 Editor.** The probe ≤ 200 ms desktop (input cached, ≤ 3 single-tile rebuilds); island /
  tile colouring re-extracts only when the revision or the view changes, inside S19's 64k-triangle
  cap; at most 256 tile labels per frame; the slope overlay ≤ 64k triangles. The Navigation
  window's findings refresh at most twice a second, and only while it is visible.

### Editor UX (human)

- **R25 Path Tester** (Scene view toolbar "More tools" menu ▸ Path Tester, the AI Navigation
  section's button, and the rebindable command `navigation.pathTester`, no default key). While on:
  Shift+click sets the start, click sets the end (on what the surface placement hits, S26's
  `surfaceAt`); pressing within 10 px of a handle drags it and re-queries every frame; Esc or the
  toggle ends it. Its panel at the view's top right has Agent Type (dropdown of the project's
  types), Area Mask (area names, checkable), Mode Path / Raycast / Closest Edge / Locate, and the
  result: status, the reason sentence (`pathFailureDescription`), length, corners, gap, snap
  distances, islands, plus "Copy Query" (the equivalent `query_navigation` JSON). It calls the same
  `NavigationSystem` queries as MCP and draws through the shared query draw (`setQueryDraw`):
  start green, end red, Complete magenta, Partial orange plus a red gap segment to the end. Works in
  edit and play; it edits nothing (no undo step), and its state is not saved.
- **R26 Probe popup.** Mode Locate shows the R10 location and, off the mesh, the R11 reasons as a
  popup at the click (reason, value vs limit, a "Select" button for an `entity`).
- **R27 Overlays** (Scene view ▸ AI Navigation, saved with the workspace like the S19 flags):
  "Color By" Areas (default) / Islands / Tiles; with Islands and a Path Tester start set, islands
  the start cannot reach are grey; "Show Steep Slopes" tints collected triangles steeper than the
  agent's Max Slope (from the cached input); "Show Tile Labels" draws `"12.4 ms · 210 polys ·
  18.5 KB"` at each visible tile; dirty tiles are outlined orange while tile bounds or labels show.
  All CPU-built; with the options off nothing is created or allocated.
- **R28 Navigation window, Surfaces tab:** columns Last Bake (`840 ms, 12/40 tiles, ⚠ 3`; the row
  expands to warnings, the error and its hint), Walkable (m²), Islands; below the table a "Problems
  (N)" section listing findings (severity icon, code label, GameObject, message); click selects
  the GameObject, double-click frames it.
- **R29 Inspector (NavMesh Surface):** under the state line, the last bake (time, tiles built of
  total, polys, warnings count with a foldout, error in red with its hint), walkable area and island
  count; above Bake, the estimate "Voxels W × H, N tiles (M voxels per agent radius)" from the
  cached collection (Volume mode: from the volume, no collection), with warnings: fewer than 2
  voxels per agent radius; more than 512 tiles ("above the mobile/web budget"); the R6 32-bit limit
  would refuse the bake.

### Agent UX (MCP, Lua, CLI)

- **R30 `query_navigation`:** `from`/`to` accept `[x, y, z]` or a GameObject (its world position;
  a NavMesh Agent's feet). `to` may be an array of up to 64 points → `results[]` (path,
  reachable, raycast; one entry per target, corners left out). `op: "locate"` (R10; off the mesh
  adds `why[]` from R11). Every non-Complete answer carries `reason`, `surface`, `closest_point`,
  `gap`; paths add `start`/`end` `{position, snap_distance, island}`. Agent type: explicit >
  `from`'s NavMesh Agent > `target` surface > first; a `target` surface whose agent type differs
  from an explicit (or the `from` agent's) one is an error naming both. Grid answers carry
  `reason` too (outside / Not Walkable cell → `*_off_mesh`, masked cell → `*_filtered`,
  unreachable → `disconnected`).
- **R31 `bake_navigation`:** each surface's result adds `sources`, `walkable_area_m2`,
  `island_count`, `islands` (≤ 16), `slowest_tiles` (≤ 5), and on failure `error_code` + `hint`.
- **R32 `get_navigation_info`:** surfaces add `coverage`, `islands` (≤ 16) and `last_bake`; links
  add `connected`, `connections`, `island_start`, `island_end`, `surface`; new `obstacles[]` and
  `modifiers[]`; top-level `findings[]`; parameter `entities` restricts every list and the findings
  to those GameObjects.
- **R33 `visualize_navigation`:** `show` adds `islands`, `slope`, `tile_colors` (islands and
  tile_colors set Color By; true on one clears the other), `tile_labels`; `frame: "navmesh" |
  GameObject` (the union of the baked surfaces' world bounds through `SceneWindow::frameBounds`, or
  Frame Selected's `frameEntities`), applied before the capture; `screenshot: true | "scene" |
  "game"` (true stays the Scene view); `path` (with S74's filter) reports and draws `reason`,
  `surface`, `closest_point`, `gap`; with `tile_labels` the result lists the visible labels.
- **R34 Tool rules.** All six tools stay in group `navigation`. Annotations / activity /
  play: `query_navigation` and `get_navigation_info` read-only, idempotent, Read, allowed in play;
  `bake_navigation` unchanged (Edit; refused in play by its handler); `visualize_navigation`
  idempotent, not read-only (it changes saved overlay flags and, with `frame`, the camera),
  Control, allowed in play. The group stays ≤ 15,000 characters; `query_navigation` ≤ 1,500;
  `visualize_navigation` gets a `budget.json` exception at its measured size + 5 % (estimate
  ~1,640 with S74's path filter). Agent guidance goes to `docs/mcp/guide/navigation.md`;
  `McpInstructions.cpp` is unchanged.
- **R35 Lua.** The path table adds `reason` (string), `surface` (entity id, 0 none),
  `closestPoint`, `gap`; `nav:isReachable(a, b[, filter])` returns `reachable, reason`;
  `nav:locate(p[, filter[, maxDistance]])`, `nav:getCoverage(surface)`, `nav:getIslands(surface)`;
  `NavMeshSurface.lastBakeResult` (nil before a bake): the R31 shape with camelCase keys and the
  same code strings. Stubs in `stubs/Navigation.lua` and `tools/lua_api/parser.py`.
- **R36 CLI and export.** `jasperengine-editor check` reports every finding with its severity
  (`error` findings make the check exit non-zero like other errors) under tool `navigation`; the
  export preflight logs warnings and errors (it never blocks) with "Window > AI > Navigation".

### Data & compatibility

- **R37** No asset or scene format changes; the `.navmesh` version stays 1.
- **R38** New overlay keys (`colorBy`, `showSteepSlopes`, `showTileLabels`) are written only when
  not default and default when absent: S19-era workspace files load unchanged.
- **R39** MCP results only add keys, except the documented new error (target vs agent type
  conflict). Lua results only add keys (the extra `isReachable` result is ignorable). S19's
  `not_baked` message text is kept (`test_checks.py` asserts it).

## 6. Design constraints & decisions

- **D1 Layout.** Engine: `NavTypes.h` (failure, path, location, island, coverage), new
  `engine/core/navigation/NavDiagnostics.{h,cpp}` (bake error names/hints, findings types and the
  pure island labelling over a `dtNavMesh`), `NavMesh` (classification inside `findPath`, islands
  cache, locate, link states, tile infos, `setPolyFlags`), `NavigationSystem` (world conversion,
  ranking, findings, reports). Editor: `editor/navigation/NavWalkabilityProbe.{h,cpp}`,
  `NavigationFindings.{h,cpp}` (project wrapper, preflight logging), `NavPathTester.{h,cpp}`.
- **D2 Islands = union-find**, not flood fill: one pass over every tile's polygons and links;
  links to polygons with zero flags are skipped; off-mesh polygons join both ends. Labels are
  `std::vector<int32_t>` per tile slot, decoded with `dtNavMesh::decodePolyId`. Weakly connected
  components make "different islands ⇒ never reachable" sound; one-way reachability is left to the
  search (R3 step 5-6).
- **D3 Queries never relabel** (R8): the cost of diagnostics is paid only by diagnostics; a carving
  game that never asks pays nothing.
- **D4 Shared reason text.** `pathFailureDescription` is the only sentence table (editor panel, MCP
  text, docs); MCP and Lua return the snake_case name.
- **D5 Probe counterfactuals** rebuild one tile with one Recast limit relaxed: exact for erosion,
  climb and region pruning without exposing Recast internals; the probe is editor-only because it
  needs the collector's sources.
- **D6 Lua keys are camelCase** (Lua convention, e.g. `minScale`), MCP keys snake_case; both come
  from one `NavBakeResult` through two writers (`bakeReportJson`, `bakeReportTable`).
- **D7 Frame "navmesh"** uses a new public `SceneWindow::frameBounds(sceneId, worldBounds)` (the
  private `focusOnBounds` plus the tween finish MCP framing already does).

C++ sketch (exact names; P2/P3 code against these):

```cpp
namespace jasperengine::nav {   // NavTypes.h (includes ecs/Entity.h: a typedef, no ECS dependency)
enum class PathFailure : uint8_t { None, NoSurface, NotBaked, StartOffMesh, EndOffMesh,
                                   StartFiltered, EndFiltered, Disconnected, BlockedByArea, OutOfNodes };
JASPERENGINE_API const char* pathFailureName(PathFailure failure);
JASPERENGINE_API const char* pathFailureDescription(PathFailure failure);
JASPERENGINE_API bool pathFailureFromName(const std::string& name, PathFailure& out);
struct NavPath {
    PathStatus status = PathStatus::Invalid;
    std::vector<Vector3> corners;
    float length = 0.0f;
    PathFailure failure = PathFailure::NoSurface;   // None exactly when Complete
    Entity surface = NULL_ENTITY;
    Vector3 start, end;                              // snapped ends
    float startSnapDistance = -1.0f, endSnapDistance = -1.0f;
    int startIsland = -1, endIsland = -1;            // -1 unless islands were current
    Vector3 closestPoint;
    float gap = 0.0f;
    // S76: cost, per-corner areas
};
struct NavLocation { bool onMesh = false, found = false; Vector3 position; float snapDistance = -1.0f;
                     int island = -1; uint8_t area = 0; Entity surface = NULL_ENTITY; };
struct NavIsland { int id = -1; float area = 0.0f; int polys = 0; Vector3 boundsMin, boundsMax; };
struct NavCoverage { float walkableArea = 0, largestIslandArea = 0, tilesArea = 0, fraction = 0; int islandCount = 0; };
}
namespace jasperengine::nav {   // NavDiagnostics.h
enum class NavBakeError : uint8_t { None, NoSurface, Unavailable, Busy, NothingCollected, TooLarge,
                                    TileFailed, NavMeshFailed, Cancelled, SaveFailed };
JASPERENGINE_API const char* bakeErrorName(NavBakeError error);   // "nothing_collected", ...
JASPERENGINE_API const char* bakeErrorHint(NavBakeError error);
enum class FindingSeverity : uint8_t { Info, Warning, Error };
JASPERENGINE_API const char* findingSeverityName(FindingSeverity severity);
struct NavFinding { std::string code; FindingSeverity severity = FindingSeverity::Warning; Entity entity = NULL_ENTITY;
                    std::string message; std::vector<std::pair<std::string, double>> details; };
struct NavFindingOptions { bool stale = true; std::function<int(Entity surface)> dirtyTilesOf; size_t maxSmallIslands = 5; };
struct NavTileReport { int x = 0, y = 0, layer = 0, polys = 0; size_t bytes = 0; double milliseconds = -1.0; };
struct NavCollectOptions { bool recordSources = false; };
}
// NavBakeCollector.h: NavCollectStats gains std::vector<std::pair<Entity, std::string>> excludedEntities;
//   collect(Scene*, Entity, std::string& error, NavCollectStats* = nullptr, const NavCollectOptions& = {},
//           nav::NavBakeError* code = nullptr)
// NavBakeInput.h: std::vector<Entity> triangleSources, terrainSources, volumeSources (recordSources only)
// NavMeshBuilder.h: static NavTileBuildResult buildTile(const NavBakeInput&, uint32_t index,
//                                                       const NavBuildConfig* configOverride = nullptr);
// NavMesh.h (nav space):
//   const std::vector<NavIsland>& getIslands() const;  float getWalkableArea() const;  bool areIslandsCurrent() const;
//   int getIslandOf(uint32_t polyRef) const;
//   NavLocation locate(const Vector3& p, const NavFilter& f, const Vector3& halfExtents, float maxDistance) const;
//   struct NavLinkState { uint32_t userId; Vector3 start, end; bool startConnected, endConnected; int startIsland, endIsland; };
//   void getLinkStates(std::vector<NavLinkState>& out) const;
//   struct NavTileInfo { int x, y, layer, polys; size_t bytes; float bmin[3], bmax[3]; };
//   void getTileInfos(std::vector<NavTileInfo>& out) const;
//   bool setPolyFlags(uint32_t polyRef, uint16_t flags);          // bumps the revision
//   static bool tileCovers(const NavTileBlob& staticTile, const NavBuildConfig&, const Vector3& p, const Vector3& halfExtents);
//   NavDebugGeometry gains triangleIslands, triangleTiles (int32 per triangle), filled when
//   getDebugGeometry(..., uint32_t extras = 0) has DEBUG_ISLANDS / DEBUG_TILES.
class NavigationSystem {   // additions; world space
    bool isReachable(const Vector3& from, const Vector3& to, const nav::QueryFilter& filter = {}, nav::PathFailure* why = nullptr);
    nav::NavLocation locate(const Vector3& point, const nav::QueryFilter& filter = {}, float maxDistance = 5.0f);
    nav::NavLocation locateOn(Entity surface, const Vector3& point, uint16_t areaMask = nav::ALL_AREAS, float maxDistance = 5.0f);
    std::vector<nav::NavIsland> getIslands(Entity surface);       // world bounds, m²
    nav::NavCoverage getCoverage(Entity surface);
    struct LinkStatus { Entity link; Entity surface; int connections; bool connected; int islandStart, islandEnd; };
    std::vector<LinkStatus> getLinkStatus();
    std::vector<nav::NavTileReport> getTileReports(Entity surface);   // every tile, last build ms or -1
    struct InputReport { int dirtyTiles = -1; std::vector<NavTileCoord> dirty; int tilesWide = 0, tilesHigh = 0,
                         tileSize = 0, nonEmptyTiles = 0; float cellSize = 0, agentRadius = 0; std::string error;
                         nav::NavBakeError errorCode = nav::NavBakeError::None; std::shared_ptr<const nav::NavBakeInput> input; };
    InputReport inspectInputs(Entity surface, const nav::NavCollectOptions& options = {}, bool keepInput = false);
    std::vector<nav::NavFinding> getFindings(const nav::NavFindingOptions& options = {});
    const NavBakeResult* getLastBakeResult(Entity surface);   // now non-const: fills the island fields on first read
};
// NavBakeResult gains: nav::NavBakeError errorCode; std::string hint; nav::NavCollectStats sources;
//   std::vector<nav::NavTileReport> tiles; float walkableArea; int islandCount; std::vector<nav::NavIsland> islands;
// countDirtyTiles(surface, error) stays, as inspectInputs(surface).dirtyTiles.
namespace jasperengine::editor {   // editor/navigation (P1)
struct NavProbeReason { std::string code, message; Entity entity = NULL_ENTITY; float value = 0, limit = 0; };
struct NavProbeResult { nav::NavLocation location; std::vector<NavProbeReason> reasons; };
struct NavWalkabilityProbe { static NavProbeResult explain(Scene* scene, NavigationSystem& system, Entity surface,
    const Vector3& world, const nav::QueryFilter& filter, std::shared_ptr<const nav::NavBakeInput> input = nullptr); };
struct ProjectNavFinding { nav::NavFinding finding; std::string scenePath, sceneName, entityName; };
std::vector<ProjectNavFinding> collectProjectFindings(Project* project, size_t* surfaces = nullptr);
void logNavigationPreflight(Project* project);   // the export preflight's lines
}
```

MCP shapes (`query_navigation` op path, Partial):

```json
{"target": "navmesh", "op": "path", "agent_type": 0, "area_mask": 4,
 "surface": {"id": 12, "name": "Ground"}, "status": "partial", "reason": "blocked_by_area",
 "corners": [[-10, 0, 0], [-2.1, 0, 0]], "length": 7.9, "closest_point": [-2.1, 0, 0], "gap": 12.1,
 "start": {"position": [-10, 0, 0], "snap_distance": 0.0, "island": 0},
 "end": {"position": [10, 0, 0], "snap_distance": 0.0, "island": 0}}
```

`locate`: `{"op": "locate", "on_mesh": false, "found": true, "position": [...], "snap_distance": 1.8,
"island": 2, "area": 0, "area_name": "Walkable", "surface": {...}, "why": [{"code": "too_steep",
"message": "The ground slopes 52° here; Humanoid's Max Slope is 45°.", "entity": {...}, "value": 52,
"limit": 45}]}`. Multi-target: `"results": [{"to": [x, y, z], "status": "complete", "length": 31.2,
"corner_count": 5}, {"to": {...entity}, "status": "invalid", "reason": "end_off_mesh"}]`.
Bake result per surface: S19's keys plus `"sources": {"meshes", "instances", "terrains",
"colliders", "colliders_2d", "volumes", "links", "excluded", "triangles"}, "walkable_area_m2",
"island_count", "islands": [{"id", "area_m2", "polys", "bounds": [[min], [max]]}], "slowest_tiles":
[{"tile": [x, y], "ms", "polys", "kb"}]`, failure `"error_code", "hint"`. `get_navigation_info`
surface `"coverage": {"walkable_area_m2", "island_count", "largest_island_m2", "tiles_area_m2",
"fraction"}`; finding `{"code", "severity", "entity", "message", "details": {...}}`.

Schema additions (McpSchema builders only): a shared `point = anyOf[vec3, entityRef]`;
`to = anyOf[point, {array, maxItems 64, items point}]`; op enum + `"locate"`; `get_navigation_info`
`entities`; `visualize_navigation` `show.{islands, slope, tile_colors, tile_labels}` booleans,
`frame = anyOf[stringEnum{navmesh}, entityRef]`, `screenshot = anyOf[boolean, stringEnum{scene,
game}]`, `path.from/to = point`. Estimated sizes: query ~1,476, visualize ~1,560 (+S74's filter),
info ~530, bake ~800; group ~7,400. Descriptions stay ≤ 400 characters, details in the guide.

Lua:

```lua
local nav = scene:getNavigationSystem()
local path = nav:calculatePath(a, b)
if path.status ~= NavMeshPathStatus.Complete then
    print(path.reason, path.surface, path.closestPoint, path.gap)   -- "disconnected", 12, Vector3, 4.2
end
local ok, why = nav:isReachable(a, b)                                -- false, "disconnected"
local here = nav:locate(p)            -- here.onMesh, here.position, here.snapDistance, here.island, here.area, here.surface
local cov = nav:getCoverage(surfaceEntity)   -- cov.walkableArea, cov.islandCount, cov.largestIslandArea, cov.fraction
for _, island in ipairs(nav:getIslands(surfaceEntity)) do print(island.id, island.area, island.polys) end
local report = NavMeshSurface(scene, surfaceEntity).lastBakeResult   -- report.ok, report.errorCode, report.hint, report.sources.meshes ...
```

Editor interfaces (P3; P2 codes against them):

```cpp
// NavigationOverlay.h: NavOverlaySettings gains
enum class ColorBy { Areas, Islands, Tiles };  ColorBy colorBy = ColorBy::Areas;
bool showSteepSlopes = false;  bool showTileLabels = false;
struct NavTileLabel { Entity surface; int x, y; std::string text; Vector3 world; bool dirty; };
const std::vector<NavTileLabel>& NavigationOverlay::getTileLabels() const;   // the last update's visible labels
// NavigationEditor.h: replaces setQueryPath/getQueryPath
struct NavQueryDraw { nav::NavPath path; Vector3 from, to; bool hasFrom = false, hasTo = false; };
void setQueryDraw(uint32_t sceneId, NavQueryDraw draw);  void clearQueryDraw(uint32_t sceneId);
const NavQueryDraw* getQueryDraw(uint32_t sceneId) const;
NavigationOverlay* overlayOf(uint32_t sceneId);   // for getTileLabels (null: no Scene view yet)
// SceneWindow.h
void frameBounds(uint32_t sceneId, const AABB& worldBounds);
```

## 7. Acceptance criteria

Core tests in `engine/tests/core/navigation/` (`jasperengine-core-tests`), editor tests in
`editor/tests/navigation/` (`jasperengine-editor-tests`), MCP tests in `tools/mcp/tests/`. "Red" is
the run on the tree before the change (or with the named line reverted) that must fail.

| AC | Verifies | Check | Red control |
|---|---|---|---|
| AC1 | R1 | `NavFailureTest.NamesAreTheContract`: every value's name and order (static_assert on the underlying values), `pathFailureFromName` round trip | the enum does not exist (does not compile) |
| AC2 | R2, R3 | `NavFailureTest.StartAndEndOffMesh`, `.FilteredEnds` (start on a Jump strip, mask without Jump), `.NoSurfaceAndNotBaked`: each failure and its status; every Complete path in `NavQueryTest.*` asserts `failure == None` | before: `failure` field absent; with step 3's all-areas retry removed, `.FilteredEnds` reports `start_off_mesh` |
| AC3 | R3, R6 | `NavFailureTest.DisconnectedIslands` (two floors 4 m apart: Disconnected, isReachable rejects with 0 expanded nodes when islands current), `.BlockedByArea` (floor split by an area strip masked out), `.OneWayLink` (reverse direction → Disconnected), `.OutOfNodes` (a comb corridor of ≥ 3,000 polygons) | `.OutOfNodes` without the `DT_OUT_OF_NODES` test reports `disconnected` |
| AC4 | R4 | `NavFailureTest.PartialHasClosestPointAndGap`: closest point = last corner; gap ±1 cell; snapped ends and snap distances on a raised start (0.3 m) | fields absent |
| AC5 | R5 | `NavFailureTest.SurfaceRanking`: two surfaces of one type, start on the second: it answers (`surface`); a Complete on either wins | S19's loop returns the first surface's Invalid result |
| AC6 | R7, R8 | `NavIslandTest.LabelsThroughLinks` (two floors + link: 1 island; link removed and rebaked: 2), `.AreaAndBounds` (20 × 20 m plane, r = 0.5: area within 2 % of 19²), `.RelabelOnRevision` (a carving box closes a corridor: 2 islands after `updateCarving`, 1 after removal), `.ZeroFlagPolysExcluded` (`setPolyFlags(ref, 0)` splits a corridor and bumps the revision), `.DeterministicIds` (two bakes, same ids) | `.RelabelOnRevision` with labels cached without the revision check keeps 1 island |
| AC7 | R8, R20 | `NavIslandTest.QueriesNeverRelabel`: 1,000 Complete paths after a tile swap leave the labelled revision unchanged and allocate nothing (`AllocationCountScope`) | a `getIslands()` call inside `findPath` fails it |
| AC8 | R9, R10 | `NavLocateTest.OnAndOffMesh` (on, 1 m off, 10 m off with maxDistance 5), `.CoverageOfPlane`; `NavStubTest.DiagnosticsStubs` with navigation OFF | functions absent |
| AC9 | R11, R12 | `NavWalkabilityProbe.ExplainsEachReason` (editor): one fixture point per code (60° ramp, 1.2 m ceiling, 0.2 m from a wall, 0.8 m² platform, Not Walkable volume, layer-excluded mesh, dynamic body, outside the volume, empty ground, moved box → `stale_tile`, 0.6 m ledge); `NavBakeTest.SourcesDoNotChangeHashes` (core) | the probe with counterfactual rebuilds disabled reports nothing for the wall, ledge and platform points |
| AC10 | R13, R14, R15 | `NavFindingsTest.EveryCode` (core): a scene raising each code once, with details; `.Deterministic` (two runs equal); `.CachedStaleCounts` (dirtyTilesOf called, no collect); `NavigationFindings.ProjectWrapper` (editor): scene path and order, `logNavigationPreflight` lines | before: only not_baked/stale exist |
| AC11 | R16, R17 | `NavBakeReportTest.SourcesAndIslands` (counts of a fixture: 2 meshes, 1 terrain, 1 excluded body...), `.ErrorCodes` (`nothing_collected`, `too_large` from S19's R6 fixture, `busy`), `.TileReportsKept` (incremental bake replaces only rebuilt tiles' ms; reload → -1) | before: `sources`/`errorCode` absent; reverting `NavigationSystem.cpp:750` to copy only warnings fails `.SourcesAndIslands` |
| AC12 | R20-R23 | `je-bench-nav` new metrics in `tools/perf/bench-thresholds.json` (+10 % timing-server allowance as S19): `city_islands_ms` ≤ 1.65, `query_reject_disconnected_us` ≤ 2.2, `query_locate_p95_ms` ≤ 0.033, `findings_city_ms` ≤ 2.2, `islands_bytes_per_poly` ≤ 4.5; `query_path_p95_ms`, `empty_tick_*` unchanged; mobile/web recorded by the S15 device lab run (release gate) | relabelling on every query makes `query_path_p95_ms` fail |
| AC13 | R25, R26 | `NavPathTester.ShiftClickStartClickEnd`, `.DragHandleRequeries`, `.OpsMatchTheSystem` (each mode's result equals `NavigationSystem`'s call), `.PublishesQueryDraw`, `.LocateRunsTheProbe` (editor gtests, no GPU); `test_navigation_overlays.py::test_path_tester_flow` (toggle via `run_command navigation.pathTester`, Shift+click / click with `send_input`, panel text through `find_ui_item`) | the tester absent |
| AC14 | R27, R38 | `NavigationOverlay.OldWorkspaceLoads` (an S19 YAML decodes to defaults plus its keys), `.NewFlagsRoundTrip`, `.OffCostsNothing` (no Mesh/LineRenderer created with the new options off), `.TileLabelsText`, `.IslandsGreyFromStart`; goldens `tools/mcp/tests/golden/navigation_islands.png`, `navigation_slope.png`, `navigation_tiles.png` in `test_navigation_overlays.py::test_overlay_goldens` (on/off differ, ≤ 1 % pixels vs golden, llvmpipe reference) | without the overlay code the islands image equals the area image |
| AC15 | R28, R29 | `test_navigation_overlays.py::test_window_last_bake_and_problems` (Surfaces tab cells and Problems rows via `find_ui_item`; clicking a row selects its GameObject: `get_selection`), `::test_inspector_summary_and_estimate` (`get_inspector_layout` rows: last bake, walkable, islands, "Voxels ... tiles", the < 2 voxels warning with Voxel Size 0.4 and r = 0.5); screenshots from an isolated editor (`tools/editor-shot.sh`) in the evidence | rows absent |
| AC16 | R30, R39 | `test_navigation_diagnostics.py`: `test_query_reasons` (each reason from the MCP fixture), `test_query_entity_refs` (agent's feet and type), `test_query_multi_target` (64 targets, 65 refused by the schema), `test_query_target_agent_type_conflict` (error text names both), `test_locate_why` (`too_steep` with value/limit), `test_grid_reasons` | `test_query_target_agent_type_conflict` passes silently on S19 |
| AC17 | R31, R32 | `test_navigation_diagnostics.py::test_bake_report` (sources, islands, slowest_tiles; `nothing_collected` + hint on an empty surface), `::test_info_coverage_findings` (coverage, links.connected false for a dangling link, obstacles, modifiers, `entities` filter, findings codes) | keys absent |
| AC18 | R33 | `test_navigation_diagnostics.py::test_visualize_frame_and_flags` (frame navmesh / entity moves the camera: the camera `set_scene_camera {}` reports before/after; `screenshot: "game"` in play returns the Game view; `tile_labels` listed; path reason) | `frame` refused as unknown argument on S19 |
| AC19 | R34 | `test_tool_budget.py` (`tool_budget.py --check`), `test_tool_conventions.py`, `test_docs.py` (`gen_tool_docs.py --check`), `test_navigation.py::test_tools_listed_with_schemas` (group hidden until `tool_groups` enables it) | a too-long description fails the budget gate |
| AC20 | R35 | `test_navigation_diagnostics.py::test_lua_diagnostics` (`eval_lua` in play: reason, surface, closestPoint, gap, isReachable's second result, locate, getCoverage, getIslands, lastBakeResult keys); `tools/lua_api/tests/test_lua_api.py` and the docs' Lua blocks | fields absent (nil) |
| AC21 | R36 | `test_checks.py::test_cli_navigation_findings` (a scratch project with an unconnected link and an off-mesh agent: codes, severities, exit code with `navmesh_unreadable`); S19's `test_cli_shaders_summary_counts_navmesh_surfaces` stays green | the CLI reports only `not_baked` |
| AC22 | R18, R19 | Gate builds: `tools/build.sh linux` (GL and `GRAPHIC_BACKEND=vulkan`), `web`, `android`; the S19 web gate (`nav_2d_topdown`, single thread) passes with `getMaxBakeSliceMs` unchanged; `NavStubTest.*` with navigation OFF | — |
| AC23 | docs | `docs/navigation.md` (a "Why navigation fails" section, Path Tester, overlays), `docs/mcp/guide/navigation.md` (reasons table, recipes), regenerated `docs/mcp/TOOLS.md`; `test_docs.py` passes | — |

## 8. Dependencies & conflicts

- **Depends on:** S19 (all of it), S74 (lands first: its bake-failure logging and
  `visualize_navigation` filter / play scene / activity are the base of R31 and R33; S75 consumes
  its Recast warnings), S26 (`surfaceAt`, Frame Selected), S25 (rebindable command), S31 (CLI).
- **Blocks:** S76 needs R1 (the `PathFailure` contract and `NavPath` fields) and `setPolyFlags`
  (R8). P1's first commit (contract headers, see Work packages) is what S76 merges early.
- **Merge hotspots** (shared with S74 / S76): `engine/core/subsystem/NavigationSystem.{h,cpp}`
  (S74 `processBakes` logging, S76 agents and `onPathFailed`), `engine/core/navigation/NavMesh.{h,cpp}`
  (S76 random point, wall distance, moveAlongSurface), `NavTypes.h` (S76 appends to `NavPath`),
  `NavMeshBuilder.cpp` and `NavBakeCollector.cpp` (S74 rcContext warnings),
  `editor/mcp/tools/NavigationTools.cpp` (S74 visualize fixes, S76 query ops),
  `engine/core/script/binding/NavigationClassesLua.cpp`, `stubs/Navigation.lua` and
  `tools/lua_api/parser.py`'s S19 block (S76 agent API and handles),
  `editor/navigation/NavigationOverlay.cpp` and `editor/window/InspectorNavigation.cpp` (S74
  modifier label), `tools/mcp/budget.json`, `docs/navigation.md`, `docs/mcp/guide/navigation.md`,
  `tools/mcp/tests/test_navigation.py`, `docs/mcp/TOOLS.md` (regenerate after merging, never
  hand-merge). Also `editor/window/SceneWindow.{h,cpp}` (S26 tools) and
  `editor/shortcuts/DefaultCommands.cpp` (S25).

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| `query_navigation` (~1,476) is near the 1,500 cap and S76 adds ops | S75 must measure ≤ 1,500 (trim the description first); S76 records its own exception |
| `visualize_navigation` over 1,500 after S74 + S75 | One `exceptions` entry (measured + 5 %), reason "S74 path filter, S75 frame/islands/slope/tile flags" |
| Weak components call a one-way-only connection "same island" | Islands answer "never reachable"; one-way cases are found by the search and reported `disconnected` |
| Island relabel cost on mobile for frequently carving surfaces | Queries never relabel (D3); diagnostics relabel at most once per fixed tick |
| The probe's counterfactual rebuild differs from the baked tile (stale inputs) | It reports `stale_tile` first and rebuilds from the current inputs |
| Keeping collected inputs in the editor costs memory (MBs on big levels) | The input is kept only while the slope overlay or the Path Tester is on, else only counts/coords |
| Small-island threshold (5 %) flags legitimate rooftops | Severity info, at most 5 per surface, never in export/CLI exit codes |
| Tile labels are ImGui text, absent from Scene view PNGs | `visualize_navigation` returns them as data (R33); goldens cover meshes only |
| `getLastBakeResult` becomes non-const | All callers are main-thread editor/Lua code; documented in the header |
| Lua key casing differs from MCP | D6: camelCase in Lua, same codes; documented in `docs/navigation.md` |

## Work packages

Three implementers, three worktrees, one branch each (`s42-core`, `s42-mcp`, `s42-editor`).

**Order.** Day 0: P1 commits the contract headers (NavTypes.h, NavDiagnostics.h, the NavMesh.h /
NavigationSystem.h / NavBakeCollector.h / NavBakeInput.h / NavMeshBuilder.h declarations,
`NavWalkabilityProbe.h`, `NavigationFindings.h`) with stub bodies returning defaults, and P3
commits its interface headers (`NavOverlaySettings` fields, `NavQueryDraw`, `getTileLabels`,
`SceneWindow::frameBounds`). P2 and P3 merge those commits and code against them. P1 lands first,
then P3, then P2 (P2's tests exercise everything); the overlay goldens are recorded on the merged
tree. S76 merges P1's contract commit.

**P1 — engine core, Lua, probe, findings** (owns):
`engine/core/navigation/NavTypes.{h,cpp}`, `NavDiagnostics.{h,cpp}` (new), `NavMesh.{h,cpp}`,
`NavBakeCollector.{h,cpp}`, `NavBakeInput.h`, `NavMeshBuilder.{h,cpp}`, `GridPathfinder.h`
(`GridPath::failure`), `NavigationSources.cmake`; `engine/core/subsystem/NavigationSystem.{h,cpp}`;
`engine/core/object/navigation/NavMeshSurface.{h,cpp}`; `engine/core/script/binding/NavigationClassesLua.cpp`,
`stubs/Navigation.lua`, the S19 block of `tools/lua_api/parser.py`;
`editor/navigation/NavWalkabilityProbe.{h,cpp}`, `NavigationFindings.{h,cpp}` (new);
tests `engine/tests/core/navigation/NavFailureTest.cpp`, `NavIslandTest.cpp`, `NavLocateTest.cpp`,
`NavFindingsTest.cpp`, `NavBakeReportTest.cpp` (new), `NavQueryTest.cpp` (the `failure == None` asserts), `NavStubTest.cpp`, `NavBakeTest.cpp`
(`SourcesDoNotChangeHashes`), `engine/tests/core/bench/NavBench.cpp`, `tools/perf/bench-thresholds.json`,
`editor/tests/navigation/NavWalkabilityProbeTest.cpp`, `NavigationFindingsTest.cpp` (new);
docs: the "Why navigation fails" and Lua parts of `docs/navigation.md`. Provides every signature
of the §6 C++ sketch.

**P2 — MCP, CLI, export** (owns): `editor/mcp/tools/NavigationTools.cpp`, `tools/mcp/budget.json`,
the navigation hunks of `editor/cli/CommandLine.cpp` and `editor/Exporter.cpp` (switch to
`collectProjectFindings` / `logNavigationPreflight`), `tools/mcp/tests/test_navigation.py`,
`tools/mcp/tests/test_navigation_diagnostics.py` (new), `tools/mcp/tests/test_checks.py`
(`test_cli_navigation_findings`), `docs/mcp/guide/navigation.md`, `docs/mcp/TOOLS.md` (regenerated).
Consumes from P1: `calculatePath`/`calculatePathOn` fields, `isReachable(..., why)`, `locate`/`locateOn`,
`getIslands`, `getCoverage`, `getLinkStatus`, `getTileReports`, `getFindings`, `NavBakeResult`'s
new fields, `bakeErrorName`, `pathFailureName/Description`, `findingSeverityName`,
`NavWalkabilityProbe::explain`, `collectProjectFindings`, `logNavigationPreflight`. From P3:
`NavOverlaySettings::colorBy/showSteepSlopes/showTileLabels`, `NavigationEditor::setQueryDraw`,
`overlayOf(...)->getTileLabels()`, `SceneWindow::frameBounds`.

**P3 — editor UI** (owns): `editor/navigation/NavigationOverlay.{h,cpp}`, `NavigationEditor.{h,cpp}`
(query draw, input cache with coords/estimate/kept input, removal of `checkSurfaces` once P2's
switch is merged), `NavPathTester.{h,cpp}` (new), `NavigationWindow.{h,cpp}`,
`NavigationEditorSources.cmake`, `editor/window/InspectorNavigation.cpp`,
`editor/window/SceneWindow.{h,cpp}` (tester routing, AI Navigation rows, `frameBounds`),
`editor/sceneview/SceneViewToolbar.cpp` (menu item), `editor/shortcuts/DefaultCommands.cpp`
(`navigation.pathTester`), `editor/render/SceneRender.cpp` (only if the overlay hookup needs it);
tests `editor/tests/navigation/NavPathTesterTest.cpp`, `NavigationOverlayTest.cpp` (new; the
existing `NavigationOverlay.SettingsRoundTrip` moves there), `tools/mcp/tests/test_navigation_overlays.py`
(new) and its three goldens; docs: the Path Tester and overlay parts of `docs/navigation.md`.
Consumes from P1: `NavigationSystem` queries and `inspectInputs`, `getDebugGeometry(..., extras)`
with `triangleIslands`/`triangleTiles`, `getTileReports`, `getIslands`, `getFindings` (with
`dirtyTilesOf` from its cache), `getLastBakeResult`, `NavWalkabilityProbe::explain`,
`pathFailureDescription`, `bakeErrorHint`.

## Deviations

- S75 P2: the upstream MCP text rules (AGENTS.md "MCP text", newer than this spec) win where they
  differ. The listed schemas stay lean: a point (`from`, `to`, `path.from/to`) is the loose type
  list `["array", "integer", "string"]` rather than `anyOf[vec3, entityRef]`, `to` is the same list
  with `maxItems` 64 (an array that is not three numbers is a list of targets), `frame` is
  `["integer", "string"]`; the handler validates them. The server does not check `maxItems`, so 65
  targets are refused by the handler with an error naming the limit (AC16 "refused by the schema").
- S75 P2: a failed path or `reachable` also carries `next` in structuredContent (and the text): the
  tool route of its reason (`locate` at the end off the mesh, `bake_navigation`, the islands...),
  since a hint an agent must see goes there; list entries leave it out. `reachable` answers carry
  `surface`, `closest_point` and `gap` from a path searched for them only when not reachable.
- S75 P2: `from` as a NavMesh Agent also gives its Area Mask when `area_mask` is absent (the spec
  names only the agent type), so the answer is the agent's own query.
- S75 P2: R34 "McpInstructions.cpp is unchanged": the guide's "Other areas" index gains
  `navigation (get_navigation_info)` (upstream: an area without a digest is one index entry), with
  the eval case `guard_navigation_findings`. The descriptions of `get_navigation_info` and
  `bake_navigation` lose their result field lists (upstream: descriptions hold no field lists);
  the fields are in `docs/mcp/guide/navigation.md`.
- S75 P2: `show.islands` and `show.tile_colors` both true is an error ("turn on one"); false on the
  current Color By returns to Areas.
- S75 P2: `out_of_nodes` is not raised by the MCP fixture (it needs a 3,000-polygon comb);
  `NavFailureTest.OutOfNodes` (P1) covers it, `test_query_reasons` covers the other eight.
- S75 P3: AC13's `test_path_tester_flow` drives the Scene view with the harness's real input
  (`tools/mcp/ui_input.py`, xdotool in its Xvfb), not `send_input` (that tool sends input to the
  running game only). The panel's answer is read through UIProbe names (`pathTester.status.<status>`,
  `.reason.<name>`, `.length.<dm>`, `.locate.<on_mesh|near_mesh|not_found>`, `.why.<code>`):
  `find_ui_item` only sees labelled widgets, and the panel's lines are text.
- S75 P3: Closest Edge and Locate need one point: any click (Shift or not) sets it. While a Locate
  handle is dragged the location follows every frame, the walkability probe (up to 3 tile
  rebuilds) runs on the click and when the drag ends.
- S75 P3: "More tools ▸ Path Tester": the toolbar shows a Path Tester toggle (route icon) when its
  items fit and a "Path Tester" item in its "..." (More Scene view tools) menu when they do not,
  both only in scenes with a NavMesh Surface or a Nav Grid.
- S75 P3: AC14's goldens set Color By and Show Steep Slopes through the Gizmos menu with real
  input: `visualize_navigation`'s `show.islands/slope/tile_colors/tile_labels` are P2's and land
  after P3.
- S75 P3: Color By Islands greys from the start of the scene's query draw, the Path Tester's or
  `visualize_navigation`'s `path.from`, when it lies on the surface.
- S75 P3: the Inspector's summary lines are `get_inspector_layout` rows of kind Label (paths
  `estimate`, `estimateVoxels`, `estimateTiles`, `estimateLimit`, `estimateError`, `lastBake`,
  `lastBakeWarnings`, `walkable`; the label is the text drawn). A collection that fails shows its
  error and code instead of the estimate.
- S75 P1: island labels are one flat `int32_t` array with a per-tile-slot offset table
  (`NavIslandLabels::tileBase`) instead of a vector per tile slot (D2): a vector header per slot
  alone would cost 0.6 B per polygon of `bench_nav_city` and break R21's 4 B per polygon.
- S75 P1: additions beyond the §6 sketch, all additive: `NavMesh::getPolyAt` (the reference
  `setPolyFlags` takes), `covers` (a polygon right under a point), `isReachable` (R6 on one mesh),
  `getSearchCount`, `getLabelledRevision`, `getIslandMemoryBytes`; `nav::labelIslands` /
  `NavIslandLabels` and `bakeErrorFromName` in `NavDiagnostics.h`;
  `NavigationSystem::getSearchExtents`; `NavDebugGeometry::tileCoords` (x, y, layer per tile of
  `tileBounds`; `triangleTiles` indexes that list); `NavBakeInput::excludedEntities` (a cached
  input carries its exclusions to the probe, which takes no stats); `NavProbeReason::detail`
  (R11 says `value` names why a GameObject was excluded, but `value` is a float: the reason
  string, and for `filtered` the area name, go in `detail`); `ProjectNavFinding::sceneId` and an
  options argument of `collectProjectFindings` (`stale`, a per-scene `dirtyTilesOf`) for P3's
  Problems list.
- S75 P1: `bakeNow` returns the report as it landed (`islandCount` -1): only
  `getLastBakeResult` labels (R8), so a synchronous bake (the bench's incremental bake) never
  pays for islands.
- S75 P1: a bake that cannot start (`nothing_collected`, `too_large`, `unavailable`) becomes the
  surface's last result, so `bake_failed` and the Inspector see it; `busy` and `no_surface` do not
  replace it.
- S75 P1: AC7's "allocate nothing" is checked as exactly one allocation per path: S19's corners
  vector, which every path had before; nothing else (no labels, no second search) allocates.
- S75 P1: the probe calls a point walkable when a polygon lies right under it (a cell wide, Step
  Height + 2 cell heights tall), not when one is in the query search box: R10's `onMesh` (the
  search box, 2 x radius) would call a point 0.2 m from a wall on the mesh and never explain it.
  The location it returns is still R10's.
- S75 P1: when the tile under a point is stale, the probe first rebuilds it with the baked limits;
  if that already covers the point, `stale_tile` is the only reason (a bake fixes it). That case
  takes 4 single-tile rebuilds instead of R24's 3.
- S75 P1: AC8 locates "off the mesh" 1.5 m from the edge: 1 m is exactly the search box's half
  width (2 x 0.5 m radius) and Detour's box test is inclusive.
- S75 P1: `agent_type_unused` is raised only in scenes with navigation components (else every
  project without navigation would get an info for Humanoid); `collectProjectFindings` keeps a
  type only when no scene uses it, once, without a scene.
- S75 P1: grid answers report an end outside the grid or on a Not Walkable / masked cell as
  `*_off_mesh` / `*_filtered` also when `allowPartial` gives a Partial path to it (the reason is
  more useful than `disconnected`).
- S75 P1: AC6 `.AreaAndBounds` (and AC8's coverage) checks the area within 5 % of 19^2, not 2 %:
  Recast erodes whole cells (r = 0.5 m at 0.2 m cells is 3 cells plus the boundary span), so the
  baked square is 18.6 m wide (346 m^2). The test also checks the area equals the island's
  bounds' footprint within 1 %, which pins the labelling's arithmetic exactly.
- S75 P1: `bench_nav_city` (S19's fixture) holds 2643 polygons in 400 tiles, not the ~20k R21-R23
  assume, so a per-tile cost weighs 8 x more per polygon: the island tile table keeps only the
  slots up to the last one in use, in 16 bits while every label index fits. Measured (5 runs,
  `--workers 3`, a shared 8-core host): `city_islands_ms` 0.19, `islands_bytes_per_poly` 4.30,
  `query_reject_disconnected_us` 0.54 (0 searches), `query_locate_p95_ms` 0.0022,
  `findings_city_ms` 0.085, `query_path_p95_ms` 0.122 (unchanged budget).
- S75 P1: files outside P1 touched minimally: `editor/navigation/NavigationEditorSources.cmake`
  (P3's; the two new sources) and `engine/core/navigation/NavBakeInput.cpp` (`memoryBytes` counts
  the sources).

## Progress (implementation notes, keep updated)

- S75 P2: `query_navigation` (points as GameObjects, agent type and mask from an agent, the
  target/agent type conflict, `locate` with the probe's `why[]`, up to 64 targets, every reason on
  paths, reachable and grids, islands labelled for the answer), `bake_navigation` (the bake report:
  sources, islands, slowest tiles, error code and hint), `get_navigation_info` (coverage, islands,
  last bake, link states, obstacles, modifiers, findings, `entities`), `visualize_navigation`
  (islands / tile colours / slope / tile labels, `frame`, `screenshot: "game"`, the path's reason),
  the CLI `check` and the export preflight on `collectProjectFindings` / `logNavigationPreflight`.
  Tests: `test_navigation_diagnostics.py`, `test_checks.py::test_cli_navigation_findings`,
  `test_navigation.py::test_tools_listed_with_schemas` (group hidden, annotations); eval cases
  `guard_navigation_*`.
- S75 P2: `bake_navigation`'s job message was overwritten by its "baking" progress when every start
  was refused (the reasons and hint never reached the text); the progress is set before the loop,
  the same hunk as S74 P2's (`nav/s41-p2`). `nav/s41-p2` after 11ed93a92 is not merged here: it
  conflicts in P1/P3 files too; its `get_navigation_info` `last_bake` (the editor's record) and this
  package's (the bake report, R32) meet in `NavigationTools.cpp` at the integration.
- S75 P1 (branch `nav/s42-p1`): the contract (`nav: S42 contract`, aba8fb7e8) carries the whole
  §6 sketch with working bodies. Done: R1-R10 (failure detection in `NavMesh::findPath`, the
  ranking, `isReachable`'s island reject, islands by union-find cached per revision, coverage,
  `locate`), R11-R12 (`NavWalkabilityProbe`, `NavCollectOptions::recordSources`, the builder's
  config override), R13-R15 (`getFindings`, `collectProjectFindings`, `logNavigationPreflight`),
  R16-R17 (bake codes and hints, sources, tile reports kept per surface), R18 stubs, R35 Lua
  (path reason fields, `isReachable`'s reason, `locate`, `getCoverage`, `getIslands`,
  `NavMeshSurface.lastBakeResult`, stubs and parser overrides), the bench metrics of AC12 and the
  docs' "Why navigation fails". P2 switches the CLI and the export to `collectProjectFindings` /
  `logNavigationPreflight`; P3 sets `NavBakeError::SaveFailed` where the editor writes the asset
  (`NavigationEditor::finish`).
- S75 P3 (branch `nav/s42-p3`): the interfaces (`nav: S75 P3 interfaces`, 030976793:
  `NavOverlaySettings::colorBy/showSteepSlopes/showTileLabels`, `NavTileLabel` and
  `getTileLabels`, `NavQueryDraw` with `setQueryDraw/clearQueryDraw/getQueryDraw` replacing
  `setQueryPath`, `overlayOf`, `SceneWindow::frameBounds`), then R24-R29: `NavPathTester` (four
  modes, handles, Copy Query, Locate's probe popup; toolbar toggle, More menu item, the AI
  Navigation section's Start/Stop, `navigation.pathTester`), the overlay's Color By / Show Steep
  Slopes / Show Tile Labels / dirty outlines, `NavigationEditor::inputReport` / `keptInput` (the
  per-surface collection cache: dirty coordinates, the estimate's grid, the input while asked
  for), the Navigation window's Last Bake / Walkable / Islands and Problems, the Inspector's
  estimate and bake summary, `SaveFailed` / `NavMeshFailed` set where the editor writes the
  asset. `NavigationEditor::checkSurfaces` stays until P2 switches the CLI and the export to
  `collectProjectFindings`; P2 then removes it. Files of other packages touched minimally:
  `editor/mcp/tools/NavigationTools.cpp` (two `setQueryPath` calls become the query draw),
  `editor/render/SceneRender.h` (`getNavigationOverlay`), `editor/AppCommands.cpp` (the
  `navigation.pathTester` binding), `editor/tests/navigation/NavigationEditorTest.cpp` (the
  overlay settings test moved out).
