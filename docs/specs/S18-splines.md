# S18 — Splines and everything built on them

Wave 3. Owner stream: world building. Report: `docs/openengine-analysis/03-world-building.md`
§1.5, §2.6, §3 rows "Spline …", §5 candidates 1/7/8/9; `00-SUMMARY.md` roadmap item 8.

## 1. Goal

JasperEngine gets one CPU spline model that roads, paths, walls, fences, rows of props, terrain road
beds, rivers, lakes, 2D level edges and moving objects all use. A designer draws a curve in the Scene view.
The curve then becomes a mesh with world-metre UVs, a row of instanced props, a fence, a cut into the
terrain or a water surface. It can be edited again at any time and the results regenerate. Every
edit can be undone. Lua scripts can read and move along the curve, and agents can do the same
through MCP. Splines are our largest missing world-building feature (report §4 "THEIRS is better" item 1).
They are pure CPU work, so they run on every backend and export target.

## 2. Scope

**In scope**
- Spline core: Linear / CatmullRom / CubicBezier; open or closed; per-knot radius (half-width) and
  roll; arc-length LUT; evaluate by t or by distance; closest point; signed distance (3D, XZ, XZ
  band); uniform sampling; Douglas-Peucker; resample. Runs in 3D (up axis Y) and 2D (up axis Z, XY plane).
- Four components: **Spline**, **Spline Extrude**, **Spline Instantiate**, **Spline Animate**
  (Unity Splines names: SplineContainer, SplineExtrude, SplineInstantiate, SplineAnimate).
- Editor: the Spline edit context (knots, tangents, marquee, insert, delete, extend), the Draw
  Spline tool (click-to-place and freehand stroke → simplify → smooth → snap), presets, and
  inspector warnings.
- Extrude profiles: Flat ribbon, Bevel path, Crown road (with shoulders), Rectangle wall
  (Racked/Stepped grade), Tube, Custom. Mitre/Round corners, end taper, end caps, chunked output.
- Placement along a spline: Spacing / Tile (fit to length) / Fence (posts, spans, gates, caps,
  Racked/Stepped grade). Conform to terrain, jitter, dropout and a max tilt.
- Spline → terrain bake: flatten the road bed (cut/fill), paint a layer along the path or across the
  area, and clear foliage density. The bake is destructive and undoable, and it works on our map-based
  terrain.
- Water from a spline: a river ribbon (Channel), FitToBanks flood fill over the terrain, and a flat
  lake fill. Both use `library:water`, which gets new `FLOW` and `MESH_DEPTH` features.
- 2D: XY splines, a ribbon (edge) mesh, a filled closed shape, a Box2D chain collider, and followers.
- Lua API, MCP tools, serialization, undo, the performance budgets and the tests.

**Out of scope** (default decisions, not open questions)
- The OpenEngine non-destructive terrain modifier stack (`TerrainModifierSystem.cpp`). The README
  and report §6 rule it out because it does not fit our destructive brush model. We take only the
  idea of a SplinePath/SplineArea volume, as a bake.
- Their placement controller framework (`Apps/Editor/Source/Placement/*`, report §6). We
  re-implement it on our command system.
- Junctions/intersections, multiple splines per container, and knot links.
- Bending a model's mesh along the curve (Unity SplineMesh-style deform), and the fence `Sheared`
  grade that needs it. Our `InstanceData` is TRS-only (`InstancedMeshComponent.h:14-21`).
- Conforming to arbitrary scene meshes at runtime. Mesh-surface snapping is an editor stroke option
  only (§6 D7).
- Their ocean spline raster (`OceanSplineRaster.cpp`, compute; FFT ocean is on the do-not-copy list).

## 3. Current state

- **No splines.** The only cubic curves are glTF `CUBICSPLINE` animation sampling
  (`engine/core/math/Interpolation.h:11`, `MeshSystem.cpp:5267-5400`). `LineRendererComponent`
  draws segments only (`engine/core/component/LineRendererComponent.h`).
- Terrain: `engine/core/component/TerrainComponent.h` holds one heightmap (heights 0–1 of `maxHeight`,
  `terrainSize` square, CDLOD nodes, `MAX_TERRAIN_VIEWS 4`), up to 3 blend maps (9 layers) and
  foliage density maps. The CPU queries are `MeshSystem::raycastTerrainSurface` and `sampleTerrainSurface`
  (`MeshSystem.h:229,231`) plus `getTerrainHeightRangeArea` (`:131`).
- Terrain editing: `editor/window/TerrainEditWindow.{h,cpp}`. `ActiveStroke` keeps a float
  working copy of the map and records a dirty region. Undo is a rect patch
  (`editor/command/type/TerrainMapPatchCmd.h`, `editor/util/TerrainMapUtils.h`). `scriptedStroke()`
  runs brush strokes without the mouse. MCP has `sculpt_terrain`, `set_terrain_maps` and `sample_terrain`
  (`editor/mcp/tools/WorldTools.cpp:4098-4170`).
- Runtime-generated geometry pattern: foliage chunks are engine-owned entities that are neither
  serialized nor shown in the hierarchy (`MeshSystem.h:48-70`, `MeshSystem::createFoliageEntity`
  `MeshSystem.cpp:3119`). Generated meshes skip buffer encoding through `Stream::isGeneratedMesh`
  (`editor/Stream.cpp:4179`).
- Constraints found in code:
  - `Signature = std::bitset<64>` (`engine/core/ecs/Signature.h:12`), and about 53 types are
    registered today. S07 raises the limit to 256.
  - `ComponentType` must be appended at the end (`editor/Catalog.h:71-121`).
  - `Rigidbody::createMeshCollider` takes `uint16_t` indices (`object/physics/Rigidbody.h:67`).
  - `Rigidbody2D::createChainCollider(vertices, loop)` exists (`Rigidbody2D.h:53`).
  - `MeshPolygon` builds a `TRIANGLE_STRIP` (`object/MeshPolygon.cpp:17`), so it cannot fill a concave
    shape.
  - The web build has no threads by default (`engine/CMakeLists.txt:208`).
  - We are right-handed, with the camera looking down −Z (`Matrix4::lookAtMatrix`,
    `math/Matrix4.cpp:562`).
- Editor infrastructure we build on:
  - Indexed catalog properties (`"tiles[N].field"`, `editor/Catalog.cpp:1405`).
  - `Command::mergeWith` (`editor/command/Command.h:18`).
  - Undo groups `mark()`/`groupSince()` (`editor/command/CommandHistory.h:46-51`).
  - `GizmoSelected` (`editor/render/RenderUtil.h:195`).
  - Line helpers `editor/render/LineDrawUtils.h`.
  - The model drop point uses the AABB top or the terrain surface (`editor/window/SceneWindow.cpp:364-380`).
- Water: `engine/shaderlib/library/water.{frag,vert,glsl}` is anchored to world XZ and has no flow.
  Its shore foam and absorption need the scene-depth pass (`DEPTH_FOAM`, `ABSORPTION`).
- Lua wrappers are `EntityHandle`/`GameObject`-derived classes
  (`engine/core/script/binding/GameObjectClassesLua.cpp:843` `Terrain`).

## 4. Reference (OpenEngine, MIT, © 2026 Gamecrafters Guild Limited and contributors)

Paths are relative to `external/OpenEngine/`. Copied files keep their copyright header and get an
entry in `NOTICE.md`. Exports that link the ported code ship `licenses/OpenEngine-LICENSE.txt`,
following the shaders.com precedent in `NOTICE.md`.

| What | Files | Take | Improve / change |
|---|---|---|---|
| Core | `Engine/Modules/Spline/Include/Spline/{SplineTypes,SplineData,SplineEvaluator,SplineUtility}.h`, `Source/{SplineData,SplineEvaluator}.cpp`, `Tests/*` (≈2.4k LOC incl. tests) | Point/frame types, `IsEffectivelyClosed`, 256-sample arc LUT, segment AABBs, AABB broad phase + ternary search, XZ and band queries, linear per-segment channel interpolation (no overshoot), iterative RDP returning kept indices, `ReplacePointsFromFrames`. **Copy and adapt** to our `Vector3`/`AABB`; inline the Bezier/CR basis. | Roll in degrees in data. Up axis selectable (Y/Z) for 2D. Parallel-transport fallback when forward ∥ up. Drop per-point Euler rotation/scale (unused by our consumers). |
| Strip / profiles / corners | `Engine/Modules/SplineGeometry/{Include,Source}/{SplineStation,SplineStripBuilder,SplineProfile,SplineCorner,SplineEndTaper,SplineStep,SplineVertex}.*` + tests | Station contract (roll-free basis, per-side half-widths, `Join` Advance/Crease/Pivot), profile generators (Bevel, Crown, Rectangle, Custom; Hard/Grounded flags; derived `NominalHalfWidth`), world-metre U/V incl. `UOriginMetres` for chunking and closed-loop snap, mitre limit 120° → Round, corner issue reports. **Copy and adapt.** | **Handedness**: they are LH with (Right,Up,Forward) = (+X,+Y,+Z). We compute `Right = normalize(Forward × Up)`, so profile X and the triangle winding must be mirrored. Ported tests get a winding/normal assertion (AC5). |
| Water fill | `SplineGeometry/{SplineFillField,SplineFillMesher}.*` + tests | Flood fill on the terrain lattice (`SplineGroundGrid`), marching-squares area fill without T-junctions, buried rim (`EdgeDrop`), `uv0 = (arc m, depth m)`, lattice-aligned chunks, waterline-rise diagnostic. **Copy.** | Ground grid comes from our heightmap texels (§6 D9). |
| Layout | `Engine/Modules/SplineLayout/Source/{TileLayout,FenceLayout,SpanMitre,CenterlineSampling}.cpp` + headers + tests (FenceLayout tests 4.7k LOC) | Tile poses (fit-to-length / fixed pitch, `HoldSurfaceAcrossGaps`, `ClampTiltFromWorldUp`, keying by `StationIndex` so dropout does not reshuffle), fence posts/spans/crests/gate overrides/racked+stepped, span mitre. Jitter hash `SplineJitterUnit` (`Engine/Include/Components/Spline/SplineStationJitter.h`). **Copy**, and stub their `ModelRef`/`PieceEntity` with our asset refs. | Drop `SeamShear` and the `Sheared` grade (out of scope). |
| Recipes (field lists only) | `Engine/Include/Components/Spline/{SplineExtrude,SplinePlacement,SplineFence,SplineWall}.h` | Field names, defaults (Width 2, CrownRise 0.12, ShoulderWidth 0.5, PostPitch 2.4, SpanMaxStretch 1.25, MaxTilt 35°), `ResolveGroundAuthority` rule (Crown owns ground, Bevel grades, FitToBanks none). | Our components are non-POD and reflected (S08). One Extrude component covers their Extrude + Wall + water. |
| Terrain route | `Engine/Modules/TerrainECS/{Include,Source}/TerrainRoutePolyline.*`, `Engine/Include/Components/Terrain/TerrainModifierVolume.h` (SplinePath/SplineArea) | Resample the spline by arc length into stations and evaluate the polyline segment-nearest, with spacing bounds (`kMinRouteStationSpacing` 0.05). Inner/outer falloff. **Adapt** into an editor bake. | No modifier stack and no priorities. |
| Editor tool | `Apps/Editor/Source/SceneView/{SplineTool,SplineSceneGizmo,SplineStrokeCapture,SplineDrapePolylines}.cpp` | Algorithms only: `FinalizeSpline`, `SimplifyPolyline`, `SmoothPolyline`, `SnapPointsToSurface`, `TryExtendExistingSpline`, marquee rules, screen-scaled knot markers. **Re-implement** on our SceneWindow/gizmos/commands. | Use our TranslateGizmo for the selected knots, and our undo. |
| Rebuild debounce | `Apps/Editor/Source/Placement/SplineRebuildGate.cpp` | The idea of debouncing a rebuild during a drag. | Our version lives in the engine `SplineSystem` with a cost estimate (R30). |

## 5. Requirements

### Functional — core
- **R1** `SplineData` supports Linear, CatmullRom (centripetal, α = 0.5, auto tangents) and
  CubicBezier (explicit handles, with a tangent mode per knot: Mirrored, Aligned, Broken). Splines
  can be open or closed. A closed spline with < 3 knots generates as open while keeping `closed = true`
  (OpenEngine `IsEffectivelyClosed`).
- **R2** Per-knot channels are `radius` (half-width, world units, ≥ 0) and `roll` (degrees). Both are
  interpolated linearly per segment and never overshoot the knot values.
- **R3** Queries:
  - `evaluate(t)`, `evaluateAtDistance(d)` → frame {position, forward, right, up, radius, roll}
  - `length()`, `tToDistance`, `distanceToT`
  - `closestPoint(p)` (3D) and `closestPointXZ(x,z)`
  - `signedDistance(p)` (to the swept tube), `signedDistanceXZ(x,z)` (inside a closed spline is
    always < 0) and `signedDistanceBandXZ(x,z)` (a ring, never a disc)
  - `sampleUniform(n)`, `simplify(tolerance)` (RDP, indices kept) and `resample(spacing)`, which
    keeps the channels

  The core works in spline-local space. Component wrappers convert to and from world space.
- **R4** Frames: `right = normalize(forward × upAxis)`, then rolled. `upAxis` is +Y in 3D and +Z
  in 2D. Where forward ∥ upAxis (within 1e-4), the frame is the previous frame parallel-transported,
  so vertical segments never flip.
- **R5** The cache (LUT, segment AABBs, CR tangents, XZ polygon) rebuilds lazily when `version`
  changes. Each change increments `version`. Queries on a dirty spline rebuild first, so a stale
  cache can never be read.

### Functional — consumers
- **R6** *Spline Extrude* sweeps one profile along stations. The profiles are:
  - **Flat**: two points.
  - **Bevel**: Width, EdgeDrop, EdgeInset.
  - **Crown**: Width, CrownRise, ShoulderWidth, ShoulderDrop.
  - **Rectangle**: Thickness, Height, with Grade Racked or Stepped and step height.
  - **Tube**: sides, using the radius channel.
  - **Custom**: up to 32 points with Hard/Grounded flags.

  The width source is the radius channel or a constant. U = distance × tilesPerMetreU, measured
  along the *draped* 3D length. V = the scaled profile perimeter × tilesPerMetreV, with an optional
  level-course V for walls. A closed loop snaps U to whole tiles. Other options: corner style
  None/Mitre/Round (mitre past 120° builds Round and reports it), end caps for closed profiles, end
  taper (metres), lateral and vertical offset, cast/receive shadows and a material.
- **R7** *Spline Extrude* `shape` also offers:
  - **Fill**: a closed spline becomes a flat polygon at the mean knot height + vertical offset in 3D,
    or in the XY plane in 2D. It is triangulated by ear clipping, so concave shapes and self-touching
    shapes are refused with a warning.
  - **WaterBanks**: an OpenEngine FitToBanks flood fill over the terrain under the spline, with
    MaxHalfWidth, EdgeDrop and SeaLevelFloor.
- **R8** *Spline Instantiate* has three modes:
  - **Spacing**: items every N world units. The fit is FitToLength or FixedPitch.
  - **Tile**: pieces fitted end to end by their bounds along the chosen axis.
  - **Fence**: post/span/gate/cap pools, PostPitch, SpanMaxStretch, grade Racked/Stepped, posts on
    authored corners, and up to 16 span overrides (Gate or explicit piece by knot index + span ordinal).

  Each mode has up to 6 pool entries (model or prefab + weight). Other options: lateral offset;
  jitter (spacing, yaw, lateral, scale range); dropout; seed; end trim; plant mode BoundsMin/Pivot;
  max tilt. Randomness is keyed by (seed, channel, station index), so changing the dropout never
  moves the pieces that survive it.
- **R9** Conform (Extrude and Instantiate) is None, Terrain-Height or Terrain-HeightAndSlope
  (SlopeBlend), plus a vertical offset. Terrain sampling uses `MeshSystem::sampleTerrainSurface`.
  Where the probe misses (off the terrain), the stations hold the nearest measured height
  (`HoldSurfaceAcrossGaps`).
- **R10** *Spline Animate* (on the moving entity) has these settings:
  - **Spline reference**: an `EntityReference`.
  - **Method**: Duration or Speed.
  - **Loop**: Once, Loop, PingPong.
  - **Easing**: on Once.
  - **Alignment**: None, SplineTangent, SplineTangent+Up, World.
  - **Offsets**: start offset (0–1) and lateral/vertical offset.
  - **Play**: PlayOnAwake.

  The follower moves at constant world speed (arc-length param). It works in 2D, rotating about Z.
- **R11** Spline → terrain bake (editor) reads the settings from `SplineComponent.terrain`:
  - **Region**: Path (band = radius + extraWidth, falloff width) or Area (the interior of a closed
    spline, + falloff).
  - **Flatten**: mode Flatten | CutOnly | FillOnly, height = spline Y at the closest station + height
    offset (a road bed sits at the road minus its crown).
  - **Paint**: surface layer, strength, falloff and edge noise.
  - **Clear foliage density**: a layer mask.

  One "Apply to Terrain" creates one undo step: a `TerrainMapPatchCmd` per touched map, grouped with
  `mark()/groupSince()`. Only the dirty rect is rewritten, and undo restores the maps bit-exactly.
- **R12** Live terrain session: while "Live Terrain" is on during an Edit Spline session, the bake
  works from a snapshot of the affected region taken when the session started. Each spline change
  restores that snapshot inside the union of the old and new footprints, then re-bakes. The session
  commits one grouped patch when it ends. After the session ends, a re-bake works on the current
  terrain, so trenches from earlier positions remain. This limitation is documented (§9).
- **R13** 2D:
  - Splines in `SCENE_2D` default to up axis Z with the points' z = 0.
  - Extrude Flat → an XY ribbon (an edge with tiled texture; V across the width).
  - Fill → an XY polygon.
  - `collision = Chain2D` builds a Box2D chain from the centreline (loop when closed) on the entity's
    `Rigidbody2D` (a static body is added if missing).
  - Spline Animate rotates about Z.
- **R14** Collision (3D): `collision = Mesh` adds static Jolt mesh shapes built from the generated
  chunks (`createMeshCollider`, uint16 indices → ≤ 65,535 vertices per chunk, R27).

### Platform & backends
- **R15** All generation is CPU-side C++20 with no backend-specific code. Generated meshes use the
  standard mesh vertex layout (position, normal, tangent, uv0, color) and draw on GL 3.3,
  GLES3/WebGL2, Metal, D3D11 and Vulkan. No compute shaders, SSBOs, indirect draws or 64-bit atomics.
- **R16** Results are deterministic: the same inputs produce bit-identical vertex/instance output on
  all desktop compilers (no `-ffast-math` in the spline sources, and no unordered containers in the
  output order). On ARM/Wasm the tolerance is 1e-5 relative. This is checked by golden hashes (AC15).
- **R17** Web single-thread (`EMSCRIPTEN_THREAD_SUPPORT=OFF`): regeneration runs through the S06
  single-thread pumped mode, time-sliced to ≤ 4 ms per frame. It never blocks a frame for a whole
  rebuild.

### Performance (Release; "desktop" = CI runner, "mobile" = mid-range 2022 Android, manual)
- **R18** `evaluateAtDistance` ≤ 60 ns. `closestPoint` on a 100-knot spline ≤ 6 µs. Cache rebuild of a
  100-knot spline ≤ 60 µs (desktop).
- **R19** Extrude: a 1 km Crown road (7-point profile, adaptive stations ≤ 1 m) builds in ≤ 3 ms on
  desktop and ≤ 12 ms on mobile, excluding the GPU upload. Instantiate: 2,000 pieces placed in ≤ 2 ms
  on desktop. FitToBanks: a 300 m river on a 1025² terrain builds in ≤ 25 ms on desktop.
- **R20** Terrain bake of a 1 km road on a 1025² heightmap + 1 blend map takes ≤ 150 ms (editor,
  desktop). A live-session re-bake of a 50 m knot drag takes ≤ 16 ms.
- **R21** Zero cost when unused: a scene with no spline components spends ≤ 1 µs per frame in
  `SplineSystem` and makes no allocations. Spline Animate costs ≤ 0.5 µs per follower per frame.
- **R22** Memory: the cache is ≤ 4 KB + 64 B per knot. Generated buffers are freed when the component
  is removed or disabled.

### Editor UX (human)
- **R23** Menus:
  - GameObject › Spline › Empty, Draw Spline Tool, Road, Path, Wall, Fence, River, Lake, Circle and
    Square (3D).
  - GameObject › Spline › Empty, Draw Spline Tool, Edge, Filled Shape and Circle (2D).
  - Add Component lists the four components under "Spline".

  Each preset creates a configured entity in one undo step.
- **R24** Inspector "Spline":
  - Type, Closed and Up Axis.
  - A knot table (index, position, radius, roll; tangents for Bezier), with multi-select synced with
    the Scene view.
  - "Edit Spline" toggle and "Draw" button.
  - Length readout.
  - A Terrain foldout with Apply to Terrain, Live Terrain and "Convert heightmap to 16-bit" (shown
    when the map is 8-bit).

  The Extrude/Instantiate inspectors show warnings from the generators: past mitre limit, inner fold,
  corners overlap, waterline rise, conform misses, instance cap and triangulation failure. Each
  warning names the knot it concerns.
- **R25** Edit Spline context (Scene view, 3D and 2D):
  - Knots are drawn as screen-scaled markers, Bezier handles as lines + dots, and the radius band is
    draped on the terrain.
  - Click selects; Shift/Ctrl add to or toggle the selection; drag on empty space marquee-selects.
  - The current Move/Rotate/Scale tool acts on the selected knots (rotate/scale about their centroid;
    Scale on one knot scales radius).
  - Ctrl+click on the curve inserts a knot; Ctrl+click past an end appends; Delete removes (refused
    below 2 knots, with a status message).
  - F frames the selection; Esc or the toggle exits.
  - Clicking generated geometry selects its owner entity.
- **R26** Draw Spline tool:
  - **Click mode**: click places a knot, click-drag pulls Bezier tangents (Unity-like), Enter or
    double-click finishes, Backspace removes the last knot, Esc cancels.
  - **Freehand mode**: the stroke is simplified (RDP, default tolerance 0.5 units 3D / 4 px-equivalent
    2D), smoothed (default 2 Laplacian iterations, endpoints fixed) and snapped (None/Terrain/Surface)
    into a CatmullRom spline.
  - A stroke that starts within the pick radius of an endpoint of the selected spline extends that
    spline. A stroke that ends on its start point closes it.
  - Surface snap uses S26 triangle picking when available, otherwise terrain then AABB top
    (`getModelDropPosition` behaviour).

### Agent UX (MCP, Lua)
- **R27** MCP tools (in `editor/mcp/tools/SplineTools.cpp`):
  - `edit_spline` creates or edits points, a stroke, the type and closed. Its result returns the
    points and the length.
  - `sample_spline` is read-only: frames at t or distances, uniform samples, the closest point and
    signed distance for query points.
  - `build_spline` applies a preset and/or extrude/instantiate/terrain settings. With `apply_terrain`
    it bakes the terrain, then reports the result: vertex/chunk/instance counts, the terrain rect
    changed, and the warnings.

  All three are undoable (the `undo` tool). `edit_spline` and `build_spline` together add ≤ 6,000
  characters to `tools/mcp/tool_budget.py` totals. The other fields of the four components are set
  through `set_properties` (`points[N].position`, …).
- **R28** Lua (§6 D11): the `Spline`, `SplineExtrude`, `SplineInstantiate` and `SplineAnimate`
  wrappers expose the queries of R3 in world space, knot editing at runtime (which marks consumers
  dirty and regenerates within the R30 budget) and follower control. The Lua stubs and
  `docs/mcp/guide/lua-api.md` are regenerated/updated, and a new `docs/mcp/guide/splines.md` covers
  the agent workflow.

### Data & compatibility
- **R29** Serialization (`editor/Stream.cpp`):
  - The four components are encoded with defaults omitted. Knots are written as flow maps
    `{p:[x,y,z], in:[..], out:[..], r:, roll:, mode:}`, one per line, so diffs stay readable.
  - Generated chunks are never serialized: `isGeneratedMesh` is extended, and chunks are
    engine-owned entities.
  - Old scenes load unchanged. The scene version is bumped once (S03) with an identity migration, and
    newer-editor fields are preserved as unknown data (S03).
  - Pool, model and material refs are GUID refs (S04).
  - Prefab overrides work per knot property, and the whole knot array is overridden when the count
    differs.

### Regeneration
- **R30** Regeneration pipeline (`SplineSystem`):
  - A spline `version` change marks its consumers dirty with the changed arc range. Only the chunks
    intersecting that range rebuild (chunk length 64 units, ≤ 65,535 vertices).
  - Before scheduling, the system estimates the cost (stations × profile points, or pieces). Below
    2 ms the rebuild runs inline in the same frame. Above that it is submitted to the S06 Background
    lane, and in the editor it is debounced to 120 ms after the last change while dragging.
  - Results carry the generation id. Stale results are discarded, and the swap and upload happen on
    the main thread.

## 6. Design constraints & decisions

- **D1 Location.**
  - Core, geometry and layout go in `engine/core/spline/`, a new include dir in
    `engine/CMakeLists.txt` next to `core/math`. Files: `SplineTypes.h`, `SplineData.{h,cpp}`,
    `SplineEvaluator.{h,cpp}`, `SplineUtility.h`, `SplineStation.h`, `SplineProfile.*`,
    `SplineStripBuilder.*`, `SplineCorner.*`, `SplineEndTaper.*`, `SplineStep.*`,
    `SplineFillField.*`, `SplineFillMesher.*`, `SplinePolygonFill.*`, `SplineTileLayout.*`,
    `SplineFenceLayout.*`, `SplineSpanMitre.*`, `SplineJitter.h`.
  - These files depend only on `core/math` and the STL, with no ECS, so they are testable in
    isolation.
- **D2 Components** go in `engine/core/component/`: `SplineComponent.h`, `SplineExtrudeComponent.h`,
  `SplineInstantiateComponent.h` and `SplineAnimateComponent.h`. They are appended to `ComponentType`.
  That makes 4 new ECS types, about 57 of 64 without S07; S07 is not a hard dependency, but it is the
  safety margin. Extrude and Instantiate act on the Spline of their **own entity**. Animate references
  another entity. One entity may hold Spline + Extrude + Instantiate (for example road + lamp posts).
- **D3 Subsystem** `engine/core/subsystem/SplineSystem.{h,cpp}`:
  - It owns the per-entity resolve map, `std::unordered_map<Entity, SplineResolve>` with chunk
    entities, generation ids and warnings, following the `terrainFoliage` pattern (`MeshSystem.h:70`).
  - Chunk entities are created like `createFoliageEntity`: Transform + MeshComponent, or
    + Model/InstancedMesh for pieces (≤ 8,192 instances per chunk).
  - It is also where Spline Animate is updated.
  - Its update order is after scripts and before the Transform/Mesh systems.
- **D4 Spline points are local** to the entity Transform. Radius is in world units, scaled by the
  max-axis scale. Non-uniform scale gives an inspector warning. Roll is stored in degrees.
- **D5 Stations** are the shared seam, as in OpenEngine:
  - Sampling is adaptive: every knot is a station; between knots the spacing is ≤ `stationSpacing`
    (default 1 unit), with extra stations where the frame turn > 4° or the conform height delta
    > 0.1 units.
  - The draped 3D length drives U.
  - Terrain bake, extrude, instantiate and fill all consume the same station list for a given spline
    version.
- **D6 Handedness**: we are RH. `Right = normalize(Forward × Up)`. Port the profile/strip code with
  X mirrored and front faces in our default winding. Every builder test asserts that each triangle's
  geometric normal agrees with its vertex normals (dot > 0).
- **D7 Conform targets** are None/Terrain only, at runtime and in the editor. That keeps runtime
  regeneration identical to the editor and independent of physics. Snapping to arbitrary mesh
  surfaces is a stroke/knot operation (it writes knot positions) and is never re-run at load.
- **D8 Terrain bake** lives in `editor/util/TerrainSplineBake.{h,cpp}`:
  - Input: stations resampled to ≤ ½ texel spacing (`TerrainRoutePolyline` rule, min 0.05).
    Evaluation is segment-nearest per texel inside the footprint AABB, in the float working buffer
    from `TerrainEditWindow`'s stroke path. Output goes through `TerrainMapPatchCmd`.
  - Heights are terrain-local and normalized to `maxHeight`. 8-bit heightmaps are baked but trigger a
    warning, and the inspector offers a one-step 16-bit conversion.
  - Afterwards, foliage, CDLOD ranges and the physics heightfield refresh the way they do after a
    brush stroke.
- **D9 Water.**
  - WaterBanks builds `SplineGroundGrid` straight from the heightmap texels (spacing =
    `terrainSize/(N−1)`, world transform applied), so the shoreline matches what is drawn.
  - The river/lake mesh writes `uv0 = (arc, depth)` and vertex color RG = flow direction XZ
    (0.5+0.5·d), B = speed scale.
  - `library:water` gains `FLOW`: the two ripple layers advect along the vertex flow, with two-phase
    blending against stretching.
  - It also gains `MESH_DEPTH`: shore foam and absorption from `uv0.y`, with no scene-depth pass.
    This is the cheap mobile path.
  - With both features off, the shader output is pixel-identical to today (golden).
  - A Channel river is Extrude Flat with the water material and flow encoding.
- **D10 Polygon fill**: our own ear clipper in `SplinePolygonFill.cpp` (≈ 250 LOC), which avoids a
  new licence. Its input is the uniformly resampled closed curve, which is refused if it
  self-intersects.
- **D11 API sketch** (C++ core, Lua mirrors the component wrappers):

```cpp
namespace jasperengine::spline {
enum class SplineType : uint8_t { Linear, CatmullRom, CubicBezier };
enum class TangentMode : uint8_t { Mirrored, Aligned, Broken };
struct SplineKnot { Vector3 position, tangentIn, tangentOut; float radius = 2.f, rollDeg = 0.f;
                    TangentMode mode = TangentMode::Mirrored; };
struct SplineFrame { Vector3 position, forward, right, up; float radius, rollDeg; };
struct SplineHit { float t, distance; Vector3 position; uint32_t segment; };
class SplineData { public: SplineType type; bool closed; Vector3 upAxis{0,1,0};
  std::vector<SplineKnot> knots; uint64_t version() const; bool effectivelyClosed() const; /*...*/ };
SplineFrame evaluate(const SplineData&, float t);   SplineFrame evaluateAtDistance(const SplineData&, float d);
float length(const SplineData&);                    SplineHit closestPoint(const SplineData&, const Vector3&);
SplineHit closestPointXZ(const SplineData&, float x, float z);
float signedDistance(const SplineData&, const Vector3&);
float signedDistanceXZ(const SplineData&, float x, float z); float signedDistanceBandXZ(const SplineData&, float x, float z);
void sampleUniform(const SplineData&, uint32_t n, std::vector<SplineFrame>&);
void simplifyIndices(std::span<const Vector3>, float tol, std::vector<uint32_t>& kept);
}
```

```lua
local s = scene:findEntity("Road"):getSpline()      -- Spline (GameObject-derived wrapper)
local len = s:getLength()
local f = s:evaluateAtDistance(12.5)               -- {position, forward, right, up, radius, roll}
local hit = s:getClosestPoint(player.position)     -- {t, distance, position, segment}
s:addKnot(Vector3(10,0,4)); s:setKnotPosition(1, Vector3(...)); s:removeKnot(3)
local a = cart:getSplineAnimate(); a.speed = 6; a.loop = SplineLoop.PingPong; a:play()
a:setOnComplete(function() ... end)                -- Once only
```

- **D12 MCP schemas (abridged)**:
  - `edit_spline {scene?, entity? | create:{name, parent?, dimension:"2d"|"3d"}, space:"world"|"local",
    type?, closed?, set_knots?:[[x,y,z]|{position,tangent_in,tangent_out,radius,roll,mode}],
    insert?:[{index, knot}], update?:[{index,...}], remove?:[int], stroke?:{points, tolerance,
    smooth, snap:"none"|"terrain"|"surface", extend:bool}}`.
  - `sample_spline {entity, t?:[..], distance?:[..], count?, query_points?:[[x,y,z]]}`.
  - `build_spline {entity, preset?:"road"|"path"|"wall"|"fence"|"river"|"lake"|"scatter"|"edge2d"|
    "fill2d", extrude?:{…}, instantiate?:{…}, terrain?:{flatten?, paint?, clear_foliage?},
    apply_terrain?:bool}`.
- **D13 Undo** (S24 names):
  - "Draw Spline", "Move Spline Knots", "Insert Spline Knot", "Delete Spline Knots", "Edit Spline
    Tangent", "Apply Spline to Terrain".
  - `SplineEditCmd` stores the before/after knot arrays and merges with consecutive drags of the same
    selection.
  - Generated geometry is derived state and is never in the history.
- **D14** Inspector and serialization use S08 reflection where it has landed. Otherwise they are
  written by hand in `InspectorWindow.cpp`/`Stream.cpp`/`Catalog.cpp`, following the
  Tilemap/Terrain precedents. S18 does not wait for S08.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 | gtest `SplineEvaluatorTests` (ported + ours): endpoints at t=0/1 exact; straight 2-knot length within 1e-5; an 8-knot centripetal-CR closed circle r=10 has length within 1% of 2πr; `evaluateAtDistance` over 200 uniform samples gives consecutive gaps within 0.5% of their mean, for all 3 types. |
| AC2 | gtest `SplineClosestPointTests`: 1,000 seeded random queries per type agree with brute force (20k samples) within 1e-3 units; XZ variant ignores Y; `signedDistanceXZ` < 0 at the centre of a closed square, while `signedDistanceBandXZ` > 0 there. |
| AC3 | gtest `SplineFrameTests`: a vertical helix-like segment (forward ∥ Y) yields continuous `right` (dot between consecutive frames > 0.99); 2D up-axis Z frames lie in XY. |
| AC4 | gtest `SplineStrokeTests`: a 500-point noisy (σ=0.1) circle stroke → ≤ 24 knots, max deviation from the true circle ≤ 0.6; extend-from-endpoint appends and keeps the original knots bit-identical. |
| AC5 | gtest `SplineStripBuilderTests`, `SplineProfileTests`, `SplineCornerTests`, `SplineEndTaperTests`, `SplineStepTests` ported. Also: every edge shared by ≤ 2 triangles; triangle normals agree with vertex normals (D6); U equals draped distance × tiles within 1e-4; a closed loop's U seam lands on an integer; chunk boundaries share bit-identical vertices. |
| AC6 | gtest `SplineFillFieldTests`, `SplineFillMesherTests` ported. Also: a synthetic bowl terrain lake has filled area within 2% of the analytic disc; no T-junctions (edge-sharing check); the rim sits at −EdgeDrop. |
| AC7 | gtest `SplineTileLayoutTests`, `SplineFenceLayoutTests`, `SplineSpanMitreTests` ported (minus Sheared). Also: FitToLength tiles exactly cover the length within 1e-3; the same seed gives identical poses; raising dropout from 0.2→0.4 leaves every surviving piece's pose unchanged; a gate override lands on the requested span. |
| AC8 | gtest `SplinePolygonFillTests`: a concave 12-gon triangulates to n−2 triangles with total area = polygon area within 1e-4; a self-intersecting input is refused. |
| AC9 | gtest `TerrainSplineBakeTests` (editor test target): on a flat 513² 16-bit map, a straight road at height h (half-width 4, falloff 2) leaves every texel within 4 units at h ± 1 quantization step and every texel beyond 6 units unchanged; CutOnly never raises; undo restores the maps byte-for-byte. |
| AC10 | MCP `tools/mcp/tests/test_splines.py::test_road_on_terrain`: create terrain (`set_terrain_maps` fractal), `edit_spline` with a 4-point stroke snapped to terrain, `build_spline preset=road apply_terrain=true`. `sample_terrain` on the centreline matches the road bed within 0.02·maxHeight. `undo` ×1 restores the previous heights. Screenshot non-blank in the road region. |
| AC11 | `test_splines.py::test_round_trip`: a scene with all 4 components saved, reloaded and saved again is byte-identical; the scene file contains no chunk buffers; a pre-S18 fixture scene loads with no diff. |
| AC12 | `test_splines.py::test_sample_and_follow`: `sample_spline` length/closest match the Lua `getLength`/`getClosestPoint` within 1e-4. In play mode, a SplineAnimate (Duration 2 s, Once) reaches distance = length (within 1%) after `wait 2.1`, checked via `get_runtime_entity`. |
| AC13 | `test_splines.py::test_edit_undo`: insert/update/remove knots via `edit_spline`; each `undo` restores the previous knot array exactly; a drag-like sequence of 10 `update`s inside one `batch` is one undo step. |
| AC14 | `test_splines.py::test_2d`: in a 2D scene, `build_spline preset=fill2d` on a concave closed spline renders (non-blank screenshot), `preset=edge2d` with Chain2D collision: a dynamic circle dropped above it comes to rest on it in play mode (y within 0.1 of the edge). |
| AC15 | Golden images (S02 infra) on GL and on one of Metal/D3D11/Vulkan, plus the WebGL2 export smoke: `spline_road_terrain`, `spline_fence_wall`, `spline_river_flow` (MESH_DEPTH+FLOW), `spline_2d_shapes`, and `water_features_off` (pixel-identical to the pre-S18 water golden). Determinism: the hash of the generated vertex buffers for the road fixture matches across GCC/Clang/MSVC CI jobs. |
| AC16 | Benchmarks (`engine/tests/bench`, CI threshold, desktop): R18 and R19 desktop numbers; R20 bake ≤ 150 ms; R21: 10k frames of an empty scene ≤ 1 µs mean in `SplineSystem` (S15 CPU scope) and zero allocations (counting allocator, S06). |
| AC17 | Web: in the HTML5 export of the road fixture (single-thread), regenerating after a Lua `setKnotPosition` keeps every frame ≤ 4 ms of spline work (S15 scope trace) and completes within 10 frames. |
| AC18 | `tool_budget.py` delta for the 3 new tools ≤ 6,000 chars (checked in `test_splines.py`). |
| AC19 | Manual editor check, written in `docs/mcp/guide/splines.md` § Manual QA: GameObject › Spline › Road → Draw (freehand) across the terrain → road appears within 1 s. Enter Edit Spline, marquee 3 knots, move them with W: the road follows live and Live Terrain re-bakes. Ctrl+click inserts, Delete removes. Ctrl+Z steps back through each edit. Clicking the road mesh selects the Road entity. The mitre warning appears when a knot turns > 120°. |

## 8. Dependencies & conflicts

- **Depends on:**
  - **S01** (C++20, `std::span`; hard).
  - **S02** (gtest, goldens, bench; hard for done).
  - **S03** (scene version bump, unknown-data preservation).
  - **S04** (GUID refs in pools/materials).
  - **S06** (Background lane, single-thread pumped mode). Before S06 lands, use `ThreadPoolManager`
    (`engine/core/thread/ThreadPoolManager.h`) with the same interface wrapper.
  - **S24** (undo names).
- **Soft dependencies:**
  - **S07** (>64 component types).
  - **S08** (reflection-driven inspector/serialization).
  - **S26** (BVH triangle picking for surface snap and for picking chunks → owner).
  - **S15** (CPU scopes for budgets).
- **Consumed by:**
  - **S19**: generated chunks must be nav sources, and roads can carry an area cost.
  - **S22**: buoyancy needs `SplineSystem::sampleWaterSurface(x,z)`, which S18 exposes for river
    meshes.
  - **S25**: palette entries for the Spline menu.
- **Merge hotspots:**
  - `editor/Catalog.{h,cpp}` (ComponentType append), `editor/Stream.cpp`,
    `editor/window/InspectorWindow.cpp`, `editor/Factory.cpp`, `editor/window/dialog/ComponentAddDialog.cpp`,
    `engine/core/registry/EntityRegistry.cpp`/`EntityCloner.cpp`, `engine/core/JasperEngine.h`:
    every stream that adds components (S19–S22).
  - `editor/window/SceneWindow.cpp` + `editor/render/SceneRender*.cpp` (S26 picking, S25 shortcuts).
  - `editor/window/TerrainEditWindow.*` (S11 hex tiling UI).
  - `engine/shaderlib/library/water.*` (S22 mirrors `waterWaves` on CPU).
  - `engine/CMakeLists.txt`.
  - `NOTICE.md`.
  - `docs/mcp/TOOLS.md` (generated).

## 9. Risks & defaults

- **Handedness port bugs** (inverted faces, mirrored profiles). Mitigation: D6 + the AC5 normal
  assertion on every ported test. Default: mirror X, keep their test data.
- **FenceLayout size** (2k LOC + 4.7k LOC tests). Default: port it verbatim with its tests. If the
  schedule slips, Fence ships after Spacing/Tile in the same stream; Fence is not dropped.
- **Destructive terrain bake vs moving roads.** Default: the live session (R12) handles the edit
  loop. After that, re-baking leaves earlier cuts, which is documented, and "Undo" is the supported
  way to revert. A persistent modifier stack is explicitly not built.
- **8-bit heightmaps** step visibly when flattened. Default: warn + offer the 16-bit conversion. The
  bake never converts silently.
- **Component cap before S07** (≈57/64). Default: exactly 4 types. Terrain-bake settings live inside
  `SplineComponent`, and Wall/Water are Extrude shapes, not new components.
- **Load-time regeneration on mobile** for large scenes. Default: regenerate at load (async, chunked,
  R30). If a fixture exceeds 50 ms total on mobile, the follow-up is a cooked-cache flag in S30, not
  serialized buffers in scenes.
- **Float precision on long roads.** Chunk-local U origins (`UOriginMetres`) and chunk-local vertex
  positions (chunk entity transform at the chunk origin).
- **Prefab pools as runtime entities** make many entities. Default: model pools go to instancing;
  prefab pools are capped at 512 instances per component with a warning. "Bake to GameObjects"
  (inspector button, one undo step) turns them into real, serialized entities.
- **Picking generated chunks.** Until S26, chunk AABB hits map to the owner through the resolve map.
  Default: chunks are not individually selectable.
- **Centripetal vs uniform CR.** OpenEngine's CR is uniform (`Mathematics/BezierCurve.h:57`
  `CatmullRom`). Default: we use centripetal, which gives no cusps or self-intersections on uneven
  knot spacing, the usual case for freehand strokes. Uniform is not offered. Ported tests that assert
  exact uniform-CR positions are re-baselined against a reference centripetal implementation, and
  their topology/UV/layout assertions stay as they are.

## Deviations

Decisions taken during the implementation where the spec was wrong, impossible or ambiguous in a
detail (each the best correct alternative found; the requirement it serves is named).

1. **Knot property paths are `knots[N].<field>`** (R27 wrote `points[N].position`): the property
   is named after the scene file's `knots` key and Unity's `Spline.Knots`; the fields are
   `position`, `tangentIn`, `tangentOut`, `radius`, `roll`, `mode`. Indices are 0-based in the
   tools, `set_properties` and Lua alike.
2. **The Spline Extrude's material, shadows and lighting are its GameObject's Mesh** (R6 listed a
   material and cast/receive shadows on the component). As Unity's SplineExtrude needs a
   MeshRenderer, Spline Extrude needs a Mesh (added with it): its first submesh's material (any
   library shader and its features, `.material` files, presets) and its flags are copied to the
   chunks, and the Mesh itself draws nothing (RenderSystem skips it; `Stream::isGeneratedMesh`
   leaves its buffers out of the scene, R29).
3. **Chain2D colliders live on generated static bodies** (R13: "on the entity's Rigidbody2D, a
   static body is added if missing"): writing the generated chain into the GameObject's own
   Rigidbody2D would put derived data into the scene file and fight the user's colliders. The
   chain is the centreline simplified to 1 cm, split into chains of `MAX_COLLIDER_POINTS_2D`
   points sharing their end points, `MAX_COLLIDERS_2D` chains a body, on engine-owned bodies like
   the chunks.
4. **WaterBanks samples the heightmap at its texel centres** (D9 wrote spacing
   `terrainSize/(N-1)`): the terrain now draws and samples texel `i` at `(i + 0.5) / N`
   (upstream's texel-centre sampling), so the ground lattice is `terrainSize/N` apart, starting
   half a texel in; the shoreline still matches what is drawn.
5. **"Only the chunks intersecting the changed arc range rebuild" (R30)** is implemented as: a
   chunk rebuilds when the stations it is built from (positions, frames, widths, distances) or
   the run-wide U inputs change. Since U is the draped distance (R6), an edit that changes the
   length upstream changes every downstream chunk's U and so rebuilds them; edits of radius,
   roll, tangents or the end of a spline rebuild only the chunks around them.
6. **Prefab pieces are GameObjects in Play and in exported games only** (§9): prefab factories
   exist only while the game runs (`PrefabManager`), so in the editor's edit mode a prefab pool
   places nothing and the inspector says so; *Bake to GameObjects* places them as real prefab
   instances (one undo step). Model pools are instanced everywhere.
7. **A piece's run axis is derived from its bounds** (OpenEngine's rule: the longer horizontal
   axis, near-square pieces along Z) instead of a "chosen axis" field; fences have a post on every
   authored knot (FenceLayout's model), without a toggle; the fence gains a `crests` pool.
8. **Corners (Mitre/Round) apply at the knots of Linear splines**: Catmull-Rom and Bezier splines
   are smooth through their knots, so a corner there has no turn to build.
9. **Surface snap** (R26) uses the terrain under the point, then the top of the bounds of the
   meshes there (the `getModelDropPosition` rule); S26's triangle picking was not on `oe-port`
   when this stream ended (see the integrator notes in the final report).
10. **Live Terrain restores every texel the session wrote** before each re-bake (R12 wrote the
   union of the old and new footprints): a superset of that union, so no stale cut survives.
11. **Painting needs the blend map to exist**: the bake paints an existing blend map; a layer
   whose blend map is missing gets a warning (the Terrain tool creates it).
12. **Every export ships `licenses/OpenEngine-LICENSE.txt`**: every game links the spline module
   (`SplineSystem` is a scene system), not only those with spline components.
13. **R20's bake budgets are measured by the editor test `TerrainSplineBakeTests.BakeBudgets`**
   (the bake is editor code, `editor/util/TerrainSplineBake.cpp`), enforced with
   `JASPER_TEST_PERF_SLACK` on the benchmark server; R18, R19 and R21 by `je-bench-splines`.
14. **Only the scene format is bumped (3 -> 4)**; prefab files keep version 5: an older editor
   preserves a prefab's unknown spline components as unknown data (S03) either way.
