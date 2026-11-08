# T1 — Terrain operations and authoring

Wave 5 (terrain programme). Owner stream: terrain. Programme decisions: `TERRAIN-LEDGER.md` D1-D21,
binding. Analyst reports (reading only, re-verified here where cited): `our-terrain-authoring.md`,
`our-terrain-runtime.md`, `terrain3d-editor-ux.md`, `engine-constraints.md`.

## 1. Goal

Every write to a terrain goes through one operation engine, and every read of its surface through one
query, so a brush stroke, an agent call, a Lua script, a spline bake and a generator all mean the same
thing and can be tested without a GPU. A person gets a terrain that starts from a real dialog, tells
them what is under the cursor, shows slope and height on the ground and reports its errors in the
window. An agent gets declarative, deterministic edits it can validate, undo in one step and verify by
numbers and by picture. A game gets height queries and runtime edits with stated costs. T2 to T6
build on the seams defined here.

## 2. Scope

**In scope**
- `engine/core/terrain/`: the operation engine (`TerrainOps.h`, D4), `TerrainMapStore` and its stores,
  the surface query (D5), stroke resampling, raw heightmap IO, deterministic helpers.
- Migration of every existing writer: the 16 brush modes, `set_terrain_maps` generation, the spline
  bake and its live session.
- Delta undo (per-map tile deltas), a new save path, S03 and S05 compliance.
- Distance-spaced stamps, pressure, deterministic stroke replay.
- Editor: Create Terrain dialog, Terrain Settings tool, Import Raw / Export Raw, operation-coloured
  brush decal, modifier grammar and tool shortcuts, Terrain Info panel, pick from terrain, Height /
  Slope / Contours / Layer Weights overlays, invertible Slope Filter, Paint by Rules, visible error
  states, the brush radius cap.
- MCP: `edit_terrain` (which also imports heightmaps), `visualize_terrain`, `export_terrain_heightmap`,
  extended `sample_terrain`, `docs/mcp/guide/terrain.md` (created or extended, R97),
  `tools/mcp/budget.json`.
- Lua: `Terrain:getHeightAt`, `getNormalAt`, `isHoleAt`, `raycast`, `getHeights`, `setHeights`,
  `applyOps`, with collision refresh and paced GPU upload; `docs/scripting/terrain.md`.
- Performance budgets, the data story, red controls, tests.

**Out of scope** (default decisions, not open questions)
- A non-destructive modifier stack: ruled out by S18:40-42 and it stays out. Terrain edits are
  destructive and undoable.
- Noise graphs, `generate_terrain`, full-map erosion jobs: T2. T1 keeps today's value noise for the
  Noise brush and `set_terrain_maps` (moved, not extended).
- What holes do in the renderer, collision, navmesh and picking, the terrain material shader, a layer
  cap: T3 (D7, T3 R18-R30). T1 runs `cut_holes` and `fill_holes` and answers `isHoleAt`.
- Tiled worlds, streaming, collision paging, auto-regions: T4 (D8). T1's store abstraction is what
  T4 implements.
- Partial foliage refill, placement on jobs, `scatter_by_rules`: T5. T1 keeps the existing
  whole-layer refill and publishes dirty rectangles for T5 to narrow.
- A sokol region-update patch (see DC9, D12). Compute, SSBOs, GPU-side editing: never (`CLAUDE.md`).
- Do-not-copy: no CBT terrain, no GPU grass, no GPU-driven culling (`docs/specs/README.md:23-25`).
  Terrain3D is MIT and only its ideas are taken (D14): no Godot code is lifted.

## 3. Current state

Every citation was read at this line on 2026-11-04. Nothing was built or run for this spec.
Abbreviations: **TEW** `editor/window/TerrainEditWindow.cpp`, **TC**
`engine/core/component/TerrainComponent.h`, **MS** `engine/core/subsystem/MeshSystem.cpp`, **WT**
`editor/mcp/tools/WorldTools.cpp`, **TMU** `editor/util/TerrainMapUtils.cpp`.

**Data.** One square terrain: `terrainSize`, `maxHeight`, one `heightMap`, up to three `blendMaps`
whose RGB weight nine layers, foliage layers with a density map each (TC:167-172, TC:21-24,
TC:191-192). Heights are normalized and stored as R8, R16 or a multi-channel source read through red
(`TextureData.cpp:256-286`, per `our-terrain-runtime.md`). New maps are R16, blend RGBA8, density R8
(TEW:179-186).

**Writers.** Four separate implementations touch pixels:
1. Brush: `applyBrush` and `stampBrush` (TEW:1569-1941). A lambda chain for 14 map modes
   (TEW:1822-1870), plus erosion (TEW:1873-1888). The 16 modes are named at
   `TerrainEditWindow.h:20-37`.
2. Placement brushes: `applyPlacement`, `applyObjectErase` (TEW:2060, TEW:2148), one random attempt
   per event from a stored `std::mt19937` (`TerrainEditWindow.h:224`, TEW:2065-2078).
3. Generation: `makeHeightPixels` (`editor/mcp/TerrainMaps.cpp:41-76`), reached only through MCP; the
   Terrain window has no whole-map fill. `std::uniform_real_distribution` is not portable across
   standard libraries, and an absent seed is `std::random_device` (WT:708-709).
4. Spline bake: `bakeHeights`, `bakeBlend`, `bakeDensity` write bytes directly through `writeTexel`
   (`editor/util/TerrainSplineBake.cpp:90-104`, `:292`, `:318`, `:350`); the live session keeps a
   whole-map copy per map (`TerrainSplineBake.h:109`).

**Quantization differs.** Heights round (`TEW:204`, `TEW:209`). Blend and density in a stroke truncate
through `clampByte` (TEW:213-215, TEW:1934), and so does the 8-bit path of `writeHeight` (TEW:716).
The spline bake rounds (`TerrainSplineBake.cpp:102`).

**Strokes depend on the event rate and on the clock.**
- Deposit is time-based: `strength * falloff * flow * deltaTime`, where `deltaTime` is wall time since
  the last event, clamped to 50 ms (TEW:1596-1603, TEW:1900, TEW:1557). The comment says this makes it
  event-rate independent; a fast drag still deposits less per metre than a slow one, a hitch loses
  paint past the clamp, and nothing replays.
- Stamps between events are spaced by `max(radius * 0.25, size / 4096)` and share the event's time
  budget (TEW:1614-1621).
- Ramp restamps its whole capsule on every event (TEW:1685-1692, TEW:1856-1858). Placement makes one
  attempt per event (TEW:2067).

**Cost of a stroke today.**
- `beginStroke` snapshots the whole map (TEW:3788-3789) and the first stamp decodes the whole map into
  a float buffer (TEW:1654-1656): at 4096² that is 32 MB plus 67 MB on the first click, for any brush
  size.
- Each event calls `TerrainMapUtils::refresh` (TEW:1635), which sets `heightMapLoaded = false`,
  `needUpdateTerrain`, `needUpdateTexture` and `needUpdateFoliage` and invalidates the GPU texture
  (TMU:97-111). The next frame rebuilds the whole node tree with a range scan per leaf (MS:3705,
  MS:3740-3748, MS:3620) and remakes the GPU image. Drawing is gated off while `heightMapLoaded` is
  false (`RenderSystem.cpp:6111`, `:12821`).

**Undo.** `TerrainMapPatchCmd` keeps a before and an after rectangle, bounded by the union of the
stroke's stamps, so a long diagonal stroke stores nearly the whole map twice
(`TerrainMapPatchCmd.h:17-18`, TEW:1974-1976). Each execute, undo and redo copies the whole map twice
on the main thread and queues a full PNG encode (`TerrainMapPatchCmd.cpp:54-55`, TMU:83-84). The
history trims at 512 MB (`CommandHistory.h:228`).

**Saving.** `TerrainMapFileWriter` encodes on a worker, latest-wins per path, keeps failed jobs for
retry (`TerrainMapFileWriter.h:18-30`, `TerrainMapFileWriter.cpp:158-167`). It writes through
`FileUtils::writeFileReplacing` or `safewrite::writeFileResolvedBeneath`
(`TerrainMapFileWriter.cpp:115-121`), which are atomic with Document durability
(`FileUtils.h:409-411`) and announced to the watcher (`SafeWrite.cpp:978`). Scene save flushes it and
stays modified on failure (`Project.cpp:7290-7292`). Hot reload ignores `terrain_maps/terrain_edit_*`
(`editor/reload/handlers/Common.cpp:48-57`); the names `TerrainMaps.cpp:84-86` generates are not in
that rule. A failed write reaches only the console (`TerrainMapFileWriter.cpp:161`).

**Surface queries.**
- `MeshSystem::sampleTerrainSurface` (MS:3942) is the main one: bilinear at texel centres, normal by
  forward difference (MS:3968-3970).
- Callers: `JoltShapeBuilder.cpp:288`, `NavigationSystem.cpp:1433`, `ParticleSubsystem.cpp:650`,
  `SplineSystem.cpp:849`, `:1014`, `MeshRaycast.cpp:314`, `SplineEditor.cpp:401`, WT:508, TEW:1812,
  TEW:2095, TEW:4144, and foliage (MS:4034).
- Side routes: `sampleTerrainHeight` (`NavBakeCollector.cpp:663`), a private bilinear copy
  (TEW:675-695), the cursor's own (TEW:3904), and the ray march (MS:3387-3470).
- The drawn surface is `terrain_vs.glsl:64-66`; the shader's normal uses central differences at the
  node's vertex spacing (`terrain_vs.glsl:96-113`), so CPU normals are close, not equal.
- S18:530-533 and S26:467 pin the CPU to the drawn surface within 1e-4;
  `TerrainHeight.MeshSystemQueriesMatchTheShader`
  (`editor/tests/engine/subsystem/TerrainHeightTest.cpp:199`) is the existing test.

**Consumers that read the rebuild flags.** `SplineSystem.cpp:707-718` and `MeshTriangles.cpp:162`
treat `needUpdateTerrain` as "the surface changed"; foliage waits on it (MS:4239-4245). Removing the
per-event rebuild without a replacement signal would silently stop spline conform.

**Human flow.**
- Create: GameObject > 3D Object > Terrain makes an empty terrain with no dialog
  (`HierarchyWindow.cpp:502`, `CreateEntityCmd.cpp:884-887`).
- Then the Terrain window (`App.cpp:695`, `InspectorWindow.cpp:9808`), where an icon-only "+" makes a
  512 map (TEW:2465-2468) and every brush stays disabled until then (TEW:3320).
- Brush radius is capped at 50 world units whatever the terrain size (`TerrainEditWindow.h:311`,
  TEW:3542-3543, WT:5443): at 2 km that is 2.5 % of the width and big smooths take dozens of passes.
- Tools: four icons (TEW:3261-3270). Ctrl and Shift already modify (TEW:3775-3786). Alt+LMB orbits the
  Scene view (`SceneWindow.cpp:2047-2048`). `[ ]` and `Shift+[ ]` exist
  (`DefaultCommands.cpp:332-335`).
- Paint mask: slope and height ranges, Paint Texture only, not invertible (TEW:3552-3558).
- Cursor: two line loops in fixed colours (`SceneRender.cpp:2852-2853`). No info panel, no overlay, no
  raw import (grep).
- Deleting a layer shifts paint onto the wrong channels (`Terrain.cpp:136-137`).

**Agent flow.** `sculpt_terrain` is rate-based ("repeat points for big hills", WT:5430),
`set_terrain_maps` assigns or generates (WT:5466), `sample_terrain` gives points or a grid of at most
65 (WT:892, WT:5509). No slope statistics, layer coverage, overlay or framing. `frame_all` excludes
terrains (`SceneViewController.cpp:831`, S26:236, `docs/mcp/guide/scene-view.md:41`). Group `terrain`
has no `budget.json` entry, so it has 15,000 characters and 12 tools; `sculpt_terrain` and
`set_terrain_maps` are at caps (`budget.json`, `exceptions`).

**Lua.** Only map setters, layers and properties (`GameObjectClassesLua.cpp:2085-2108`); the one line
of docs is `docs/mcp/guide/lua-api.md:2810`. `Water.getHeight` is the static shape to compare with
(`GameObjectClassesLua.cpp:1113`). `RayReturn` exists (`MathClassesLua.cpp:1001`).

**GPU upload facts** (`engine/libs/sokol/sokol_gfx.h`).
- `sg_update_image` takes the whole image, every mip, validated by size (`:22770-22776`), one update
  per image per frame (`:12432`).
- A stream or dynamic image cannot start with data (`:4603`, `:22870`).
- Non-immutable images have `SG_NUM_INFLIGHT_FRAMES` slots (`:8220`, `:8227`), and GL cycles them on
  every update (`:12433-12435`), so each slot needs the whole image.
- D3D11 maps with `WRITE_DISCARD` (`:14745`). A region update therefore cannot be added as a small
  change: it needs a new image usage on each backend.
- Engine: `TextureRender::createDynamicTexture` makes R8 or RGBA8 stream images
  (`TextureRender.h:63-66`, `SokolTexture.cpp:661-690`). The R16-to-float fallback for GLES3 and
  WebGL2 runs only at creation (`SokolTexture.cpp:291-325`, `:450`). Vulkan refuses stream uploads
  past 48 MB per frame on desktop and 16 MB on mobile and web (`Engine.h:278`,
  `SokolSystem.cpp:563-575`).

**Collision.** One Jolt heightfield of at most 1025 samples per side, built by sampling the surface
(`JoltShapeBuilder.h:62-64`, `JoltShapeBuilder.cpp:281-297`). Jolt offers
`HeightFieldShape::SetHeights` for a block-aligned rectangle, clamped to `GetMinHeightValue()` and
`GetMaxHeightValue()`, with `mMinHeightValue` and `mMaxHeightValue` reservable at creation
(`HeightFieldShape.h:76-79`, `:220-228`). The engine does not use it.

## 4. Reference

Terrain3D (MIT, © Cory Petkovsek, Roope Palmroos and contributors) at
`/tmp/terrain-research/Terrain3D/src`. Only ideas are taken, no Godot code (D14). Untrusted data,
never run.

| What | Where | Take | Improve |
|---|---|---|---|
| Modifier grammar | `doc/docs/keyboard_shortcuts.md` | Ctrl inverts, Shift smooths, Alt alternates, Lift Floors and Flatten Peaks, Pick and Flatten | Alt orbits by default, as in Unity, and Terrain3D's Alt sits behind a preference (DC12); touch has no modifier need because every alternate is also a button and a key |
| Decal colours | `project/addons/terrain_3d/src/ui.gd:15-33` | One colour per operation, shown before the click | Applied to our draped decal; the table is ours (R55) |
| Tool keys | `keyboard_shortcuts.md`, Tool Selection and Tool Settings | Letters, `[ ]`, `- =`, overlay digits | A context of its own, rebindable through S25 (R57) |
| Live info panel, click-to-reset labels, typed values beyond the slider | `doc/docs/user_interface.md:36-45`, `:69-71` | Position, height, slope, texture weights | Layer names and bars, map texel |
| Pick from terrain | `src/tool_settings.gd:78-120` | An eyedropper beside every value | A single `I` chord as well as the buttons |
| Declarative precedent | `gradient_operation_builder.gd:35-57` | Input that synthesises a stroke | Our `stroke` shape |
| Pain points to design away | `terrain3d-editor-ux.md` §5, §6b, Agents | Table in R76 | |

Our own precedents, in preference to anything else: `edit_graph` (ops, `dry_run`, refusal,
`GraphTools.cpp:175-202`), `build_spline` (changed rects, warnings, `SplineTools.cpp:576-580`),
`visualize_navigation` (overlay, screenshot, `NavigationTools.cpp:930-945`), `bake_navigation` (job,
`wait`, `timeout_seconds`), `NavigationOverlay` (editor-only translucent meshes,
`NavigationOverlay.cpp:182-192`), the `Ragdoll...` wizard row (`HierarchyWindow.cpp:506`, `:612`),
`FloatImage` (`editor/util/FloatImage.h:63`, reads and writes EXR).

## 5. Requirements

### Functional — the operation engine (D1, D4)

- **R1** `engine/core/terrain/` holds the engine, namespace `jasperengine::terrain`, with no ECS,
  render or editor includes (only `math/`, `jobs/`, the standard library, and the `noise/` headers T2 R36
  and N8 name for `noise::compat` and `HeightfieldErosion`).
  `TerrainOpsDeps.NoEcsOrRenderIncludes` enforces it by reading the folder's includes (the
  source-reading precedent is `SokolPatchMarkers`). Every terrain write in engine and editor goes
  through `applyOps`, `applyField` or `setHeights` of `TerrainOps.h` against a `TerrainMapStore`. The
  only exceptions are structural: creating, deleting, rebinding, resizing or converting a whole map.
- **R2** `TerrainMapStore` is abstract: read and write rectangles of height, blend, hole and density
  data in texels, as normalized floats and as stored bytes, with dirty-rectangle reporting (§6
  sketch). Callers never touch `TextureData`. Its contract for T4 is R86. Rectangles may cross a
  store's tiles.
- **R3** Four stores ship.
  - `RawMapStore` (pure): owned or borrowed buffers; used by tests, generation and the spline bake's
    pure functions.
  - `OverlayMapStore` (pure): a transaction over any store, with 64×64 copy-on-first-touch tiles,
    float working values and before bytes.
  - `TerrainComponentStore` (`engine/core/object/TerrainEdit.{h,cpp}`): the component's maps.
  - `EditorTerrainStore` (`editor/terrain/`): the component store plus persistence and sharing
    (`refreshSharing`, TMU:114).
- **R4** The operation set is §6 Table 1: 24 public ops and 3 internal ones, open to registration from
  other files (R89). Each is one application, not a rate: running an op once gives the stated value,
  and no result depends on time.
- **R5** Shapes: `all`, `circle {center, radius}`, `rect {min, max, rotation}`, `polyline {points,
  radius, closed}` (the union of capsules, one application; `closed` with radius 0 is the inside of a
  polygon), `mask {path, center, size, rotation}` (grayscale image as weight), and `stroke {points,
  radius, spacing}` (stamps along the path that accumulate as a person's drag does, R39). Falloff
  `smooth | linear | constant` and `feather` (share of the radius over which weight falls to 0;
  default 1 for `circle` and `stroke`, 0 for the rest).
- **R6** Filters on any op: `slope [min, max]` in degrees, `height [min, max]` in the op's height
  units, `layer {index, min, max}` as a weight range, `invert` (flips the whole filter), `softness`
  (0-1, a smooth band as a share of each range; default 0). Texels out of the filter keep their bytes.
  A filter reads the state from before the op (or before the stamp, in a `stroke`).
- **R7** Targets are absolute (`height`, `heights`, `in_range`, `out_range`) or signed deltas
  (`delta`) in world units on the MCP (terrain-local units in C++ and Lua, §6 DC5), never rates.
  Heights clamp to 0..`maxHeight`; the clamp counts as `clipped_low` and `clipped_high` and warns,
  never silently.
- **R8** Determinism: the same store bytes and the same op list give the same output bytes, on any
  thread count. No wall clock, no global RNG, no `std::*_distribution`. A seeded op takes `seed`
  (default 0, reported back). RNG is a small PCG32 in `TerrainDeterministic.h`. The folder's sources
  compile with `-ffp-contract=off` (precedent `engine/CMakeLists.txt:2001-2005`). Kernels use no libm
  except `sqrt`, `floor`, `ceil`, `round`, `fabs`; sine and cosine for mask rotation and slope limits
  come from a polynomial in the same header. `TerrainGolden.*` hashes hold across x86-64 and wasm, checked
  on this host; on any other platform they are unverified until a person runs §7's command.
- **R9** Result (`TerrainOpResult`): per map one changed rectangle in texels and in terrain-local XZ
  with the exact count of changed texels; per op `changed_texels`, `clipped_low`, `clipped_high`,
  `filtered_out`, `outside`; height stats before and after (`min`, `max`, `mean_delta`, `max_delta`);
  `warnings` from the closed list of §6 Table 2; `seed` where used. A `dry_run` returns the same
  numbers.
- **R10** All-or-nothing: validation of the whole list comes first and names `ops[i].field`; then the
  ops run on an `OverlayMapStore`; a failure leaves the base byte-identical. An op whose shape lies
  wholly outside the terrain is a validation error that states the terrain's bounds (the opposite of
  Terrain3D's silent return, `terrain_3d_editor.cpp:102-104`).
- **R11** Quantization is one rule for every map: `lround(clamp01(v) * max)`. Only texels whose value
  changed are written back: untouched texels, including those inside a rect, keep their bytes.
- **R12** `cut_holes` and `fill_holes` follow T3 R18, R19, R27 and R29 exactly (§6 DC3). The map is
  `holeMap`: R8, 0 = hole, 255 = solid, created at the height map's resolution by the first cut,
  removed by the last fill. A texel is in the region when its centre is (filters apply, then a mask's
  threshold is 0.5). There is no falloff and no strength, and a second identical call changes 0
  texels. `fill_holes` with no shape fills all. The op's result carries `hole_texels`,
  `hole_fraction`, `map_created`, `map_removed` and, once T3's collider rule exists,
  `collision_samples_open`. Height, blend and density stay untouched. While T3's consumers have not
  landed, a cut warns `holes_not_rendered`; T3 removes that warning.
- **R13** `remap_layers {order}` moves every layer's paint to its new index across the blend maps and
  reorders `surfaceLayers` in the same undo step. The Terrain window's layer drag and delete use it,
  so deleting or reordering a layer keeps the paint on the right layer (fixes `Terrain.cpp:136-137`,
  and Terrain3D's pain). `Terrain::removeSurfaceLayer` for scripts keeps its documented positional
  behaviour.

### Functional — the surface query (D5)

- **R14** `engine/core/terrain/TerrainSurface.h` is the one implementation of height, normal, is-hole
  and ray. `querySurface(TerrainComponent&, localX, localZ)` in `engine/core/object/TerrainEdit.h` is
  the one function callers use (T4 adds its world lookup there). `MeshSystem::sampleTerrainSurface`,
  `sampleTerrainHeight`, `sampleTerrainHeightTexels`, `terrainTexelCoord` and `raycastTerrainSurface`
  stay as one-line forwarders so their callers do not change (D2). The private copies at TEW:675-695
  and TEW:3904 go.
- **R15** It matches the drawn surface: texel centres at `(i + 0.5) / N`, bilinear, clamp to edge,
  within 1e-4 of the shader's fetch (S18:530, S26:467). Normals keep today's forward difference
  exactly, so foliage and physics do not move.
- **R16** It never returns NaN. Outside the terrain it clamps to the edge and sets `inside = false`.
  A hole only sets `hole`: height and normal stay defined under it (T3 R26, T6 R39). With no map loaded, it returns height 0, up normal and `present = false`. `exact` is true on a
  single map; T4's tiled query (its R24) returns false when it answers from an overview.
  `raycastSurface` takes `skipHoles`: `Terrain:raycast` passes true, brush rays always pass false
  (T3's `ignoreHoles`, so a hole can be filled), and `MeshSystem::raycastTerrainSurface` passes false
  until T3 changes it.

### Functional — migrating the writers

- **R17** The 16 brush modes run on the engine, mapped in §6 Table 1. For the same stamp positions and
  weights, output matches today's stamp to within 1/65535 for 16-bit heights and 1/255 for blend,
  density and 8-bit heights (the rounding of R11). `TerrainEditWindow.cpp` keeps panel and hook code;
  kernels and session state leave it.
- **R18** `sculpt_terrain` keeps its schema and meaning. `seconds_per_point` is honoured by a
  generator that reproduces today's stamp positions and time slices (TEW:1607-1628, TEW:4153-4161), so
  existing calls give the same heights. Only `size`'s schema maximum changes (R60).
- **R19** `set_terrain_maps` keeps its schema, file naming and result. `flat`, `middle`,
  `fractal_noise` produce today's bytes through the internal `fill_noise` op on a `RawMapStore`.
  `random_noise` moves to a per-texel hash with `seed`: same seed gives the same map everywhere, but
  not today's map, which was never portable. Recorded in `docs/terrain.md`. Agent-authoring adds
  `blendmap.index` and layer factors to this schema (R97); T1 and T3 add nothing to it.
- **R20** The spline bake writes through `applyField`, and `TerrainSplineBake::bakeHeights`,
  `bakeBlend` and `bakeDensity` keep their signatures through `RawMapStore`, so
  `TerrainSplineBakeTests.*` run unchanged. `apply` and `TerrainSplineLiveSession` use an
  `OverlayMapStore` in place of whole-map copies. The step name "Apply Spline to Terrain" stays.
- **R21** `place_objects` and `erase_objects` compute a pure `PlacementPlan` (positions, yaw, scale
  from PCG32 and `spacing` against existing points) in `TerrainPlacement.h`; the editor turns the plan
  into the existing commands. A drag makes one attempt per spaced stamp, not per event.
- **R22** After migration, `TerrainMapPatchCmd`, `TerrainBakeMap`'s direct writes,
  `scriptedStampSeconds`, `ActiveStroke::workingPixels`, `beforeSnapshot` and `heightReference*` are
  gone. `TerrainErosion.*` lives in `engine/core/terrain/` (D1, with `std::function` replaced by a
  template or plain callback). `TerrainNoise.h` moves beside it with its bytes unchanged, or the
  kernels call `noise::compat` if T2 has landed; whichever of T1 and T2 lands second deletes the
  header (T2 R36, N13). No `TODO`, no dead path.
- **R23** A map edit does not rebuild the terrain. Height edits refit the node ranges overlapping the
  dirty rectangle and their ancestors (`MeshSystemTerrainEdit.cpp`), keep `heightMapLoaded` true, and
  bump a new `TerrainComponent::surfaceRevision`. `SplineSystem.cpp:713`, `MeshTriangles.cpp:162` and
  foliage read the revision. During a stroke the revision advances at most 4 times a second, and
  always at the end. `set_terrain_maps`, property changes and structural changes keep the full
  rebuild.

### Functional — delta undo

- **R24** `TerrainMapDeltaCmd` (`editor/command/type/`, new) replaces `TerrainMapPatchCmd`. It holds
  one list per changed map of 64×64 tiles, each the XOR of before and after bytes, zstd level 1
  compressed when that saves bytes (raw otherwise), with all-zero tiles dropped. Undo and redo are the
  same XOR. One step can hold several maps; step names ("Sculpt Terrain (Island)", "Paint Terrain",
  "Paint Details", new "Edit Terrain (Island)", "Import Raw Heightmap (Island)") stay Unity style.
- **R25** A delta applies only to the map it was cut from: same path, size, format and pool generation
  (S05 R13). On mismatch `undoRefusal` says "The map changed outside this history" rather than XOR
  into other data. Today's silent return (`TerrainMapPatchCmd.cpp:31-33`) goes.
- **R26** Memory bounds:
  - a step's bytes are at most the map bytes of the changed tiles, plus 32 bytes per tile; never two
    copies;
  - `memoryBytes()` reports that, and S24's 512 MB limit trims as it does today;
  - a stroke's working memory is proportional to tiles touched: at most `tiles * 64² * (4 * channels +
    bytesPerTexel)`, and a 1-tile stroke on a 4096² map allocates no whole-map buffer.
- **R27** Esc (S24 R12) restores from the overlay's before tiles, with no whole-map copy. A stroke is
  still one `InteractiveEdit` (TEW:2211-2215), and `test_esc_cancels_a_terrain_stroke` stays green.
- **R28** Structural steps (create, delete, resize, convert to 16-bit) keep `TerrainTextureEditCmd`'s
  snapshots, and those snapshots compress with the same zstd helper. `convertHeightMapTo16Bit` keeps
  its behaviour.

### Functional — saving

- **R29** A commit, undo or redo changes memory only: the in-memory map (the `TextureDataPool` entry)
  is authoritative, and no command encodes or copies a whole map. `TerrainMapPersistence`
  (`editor/terrain/`) marks the map's path dirty with the current write scope root. After 1 s with no
  stroke in progress and no new edit it copies the map once and hands it to `TerrainMapFileWriter`.
- **R30** The writer stays the only path to disk for maps. Forced flush points: scene save and
  S03's autosave (before `flushAll` at TEW:503), play start, export and build, closing the
  project, every reader that goes to disk (TEW:70), `export_terrain_heightmap`. Painting still writes
  the map's file independently of the scene file, as today ("an asset like Unity's TerrainData",
  `TerrainMapPatchCmd.cpp:58`). A crash loses at most the last debounce plus the encode, comparable to
  today's encode window.
- **R31** S03: every map write stays atomic with Document durability through
  `FileUtils::writeFileReplacing`/`writeFileResolvedBeneath` (`FileUtils.h:409-411`). A failed write
  keeps the scene modified and retries at the next flush, and it now appears as the `map_write_failed`
  issue (R72) with a Retry button.
- **R32** S05: writes go through `ScopedExpectedWrite` as today (`SafeWrite.cpp:978`), so the watcher
  knows them. Maps the engine writes are always named `terrain_edit_*` under `terrain_maps/` from now
  on, so `isEditorOwnedFile` covers them. A generated `terrain_<scene>_<entity>_height_*` from an old
  project is copied to a `terrain_edit_*` on first edit (as `ensureEditableMap` does, TEW:654-661).
  `undo` on a non-owned map that was hot reloaded is refused by R25.

### Functional — strokes

- **R33** Mouse and scripted strokes share `StrokeResampler`, which turns samples `(t, x, z,
  pressure)` into stamps. Stamps fall at arc-length multiples of `spacing = max(0.25 * radius, 0.5 *
  texel)`, measured along the sampled polyline, with interpolated pressure. The path's own shape comes
  from the samples; for a straight or polyline path the stamps are the same however finely it is
  sampled.
- **R34** One stamp deposits as today's single click: `strength * falloff * flow * 1/60 s`, with flow
  1.5 for height and 10 for blend and density (TEW:1546-1555). A click is exactly one stamp, so a
  click's result does not change.
- **R35** Holding still deposits at today's held rate: while the pointer moves slower than `spacing /
  8` per 1/60 s, time-driven stamps accrue at 60 per second of sample time, capped at 3 per sample to
  keep today's hitch clamp (TEW:1557). Moving faster gives only distance stamps. So outcome is
  independent of event rate and of pointer speed above the threshold.
- **R36** Ramp is a re-application: each update restores the before tiles and applies `ramp` from
  start to current point at the brush's strength. The final surface depends only on the end points.
  `sculpt_terrain` keeps its accumulating ramp through R18.
- **R37** Pressure: `Backend::penPressure()` returns 0..1, or -1 for no pen. Windows reads
  `WM_POINTER`, macOS `NSEvent.pressure`, Linux an XInput2 valuator when a tablet is present. Options
  **Pressure Size** and **Pressure Strength** (Terrain window, default Strength on); mouse pressure is
  1. A pen's zero reading within 100 ms of pen input counts as a mouse click, as Terrain3D found
  necessary (`terrain_3d_editor.cpp:118-133`).
- **R38** Stamps are queued, and at most 6 ms per frame of stamping runs on the main thread
  (`parallelFor` over rows per stamp when a stamp exceeds 64 k texels); the rest runs next frame, in
  order, and `endStroke` drains. The stroke's result is independent of frame budget. The decal keeps
  following the cursor while stamps wait.
- **R39** Replay: the `stroke` shape takes `[x, z]` or `[x, z, pressure]` points. With
  `JASPER_TERRAIN_STROKE_LOG=<path>` the editor appends each mouse stroke as the `edit_terrain` op
  that replays it, sample times included as `times`, and replaying gives byte-identical maps.
  Test-only environment variable, no UI. Its append-only writer is the one raw write of this spec (R98).

### Functional — seams for later specs

- **R40** `applyField(store, map, rect, field)` runs a per-texel callback `(x, z, current) -> {weight,
  target}` with the same quantization, dirty tracking and overlay: the C++ route for spline bakes and
  for T2.
- **R41** Other terrains on the same file keep following an edit (`refreshSharing`, TMU:114), without
  a whole texture copy: they share the pool entry and the same GPU image.
- **R42** `TerrainEditEvents`: every height, hole, blend and density write, whichever route it took,
  publishes `(terrain, map, rect)` once per frame to subscribers (RAII subscriptions, S09). Node
  refit, overlay, GPU upload and collision use it, and T3 (Macro Normal), T4 and T5 (re-snap,
  foliage refill) subscribe. With no edit nothing is allocated.

### Platform and backends

- **R43** The engine, query and runtime edits work on all five backends and six export targets, with
  no compute, SSBO or render-to-height. Runtime uploads use only stream images
  (`createDynamicHeightTexture`, DC9). The editor overlays use existing transparent meshes and lines,
  with no terrain shader change (DC8; T3 owns those shaders).
- **R44** Single-thread web: `parallelFor` runs inline, ops never wait on a job, and `applyOps` works
  on the `st` player.
- **R45** On GLES3 and WebGL2, the upload path converts only the dirty rectangle into a float shadow
  copy, then uploads the shadow (trap 3 of `engine-constraints.md`). R16F precision limits are
  reported by the `height_precision_low` issue and in `docs/scripting/terrain.md`.
- **R46** A Vulkan upload refused for the frame's staging budget retries next frame; `updateTexture`'s
  return is checked (`TextureRender.h:71-73`). A heightmap whose single upload exceeds the platform's
  `defaultStreamStagingBytes()` on Vulkan, or larger than 2049² on any mobile or web build, is refused
  for runtime edits with the `runtime_edit_map_too_large` issue.

### Performance

Budgets are `je-bench-terrain` metrics (`engine/tests/core/bench/TerrainBench.cpp`, thresholds in
`tools/perf/bench-thresholds.json`, doc `docs/perf/terrain.md`) unless noted, on this 8-core
host in release, idle.

- **R47** Stamp, 16-bit map: radius 64 texels (about 13 k texels), `raise` ≤ 0.3 ms, `smooth` ≤ 0.8
  ms, with slope filter ≤ 1 ms. Radius 512 texels `raise` ≤ 8 ms on 8 cores.
- **R48** `applyOps`: `set_height` on all of 4096² ≤ 400 ms; `paint_by_rules` with 3 rules on 2048² ≤
  600 ms; overlay commit of 64 tiles ≤ 1 ms.
- **R49** Delta: commit 64 changed tiles ≤ 2 ms; commit a whole 4096² height map (4096 tiles) ≤ 60 ms
  on 8 cores; undo of 64 tiles ≤ 1 ms, of a whole 4096² map ≤ 60 ms. A stroke with radius 64 texels
  along the diagonal of a 2048² map stores ≤ 1/8 of today's patch (asserted against the formula for
  today's rectangle).
- **R50** Query: `sampleSurface` height ≤ 40 ns, height and normal ≤ 150 ns; `raycastSurface` over 512
  m of 1024² ≤ 30 µs; no more than 5 % over today's cost (bench compares against a copy of today's
  code kept in the bench).
- **R51** Node refit: 64×64 rect on 2 root, 6 levels ≤ 0.1 ms; today's full rebuild is reported next
  to it.
- **R52** Runtime edit, main thread, excluding the driver: `setHeights` 64×64 ≤ 0.2 ms; `applyOps`
  circle radius 32 texels ≤ 0.2 ms; `SetHeights` collision refresh of 64×64 samples ≤ 2 ms. Upload
  size per frame with an edit is exactly map bytes (§6 Table 3), never more than once a frame, paced
  by R64.
- **R53** Disabled costs nothing: a terrain that never edits allocates nothing for this feature (one
  null pointer and one revision integer on the component), no pass, no draw, no stream image
  (`TerrainEdit.ZeroCostWhenUnused` with the allocation counter). Overlay off: no entity, no texture.
  Terrain window closed: no query per frame.
- **R54** Player size: linked `.text` and `.rodata` growth on Linux glcore ≤ 96 KiB, web `st` gzipped
  ≤ 32 KiB, measured against the merge base and recorded in `docs/perf/terrain.md` (no player module:
  games with a terrain get this code, others lose it to dead-stripping; D18 lists no module for T1).

### Editor UX (human)

New editor code goes in `editor/terrain/` (D2), listed by `editor/terrain/TerrainEditorSources.cmake`.
`TerrainEditWindow.cpp` gets calls, not bodies.

- **R55** **Brush decal.** Draped outer loop, half-strength contour and a translucent fill (vertex
  alpha = falloff times mask), all coloured by the effective operation, changing as soon as a modifier
  is pressed, before the click. Colours are in §6 Table 4. It fades 0.5 s after the pointer stops, and
  it hides during camera moves. It shows the texel-size warning colour when the brush is smaller than
  one texel.
- **R56** **Modifier grammar**, also listed in the window (a one-line legend under the tool name):
  Ctrl inverts (raise and lower, paint and erase, cut and fill, place and erase); Shift smooths
  (height, or blend in Paint Texture). **Alt+LMB drag orbits the Scene view in every tool** (DC12).
  Alt+click with no drag (released within `ImGui::GetIO().MouseDragThreshold`) starts no stroke and
  picks from the terrain as R63 does. The alternates are a button each in the tool's row and one
  rebindable key, `C` (`terrain.alternate`, Table 5), which switches the armed tool's alternate on and
  off: Raise: Lift Floors, and with Ctrl, Flatten Peaks; Set Height: Pick and Flatten; Paint Texture:
  smooth with a true average. The preference **Alt Alternates While Painting** (default off) gives
  Terrain3D's grammar: Alt held is the alternate, and Alt+LMB paints it, on a tool that has one; a
  tool with no alternate keeps orbit and pick. Modifiers are read at stroke start and held for the
  stroke, as today (TEW:3771-3786).
- **R57** **Shortcuts** in a new `ShortcutContext::TerrainBrush`, active while a brush is armed and
  the pointer is over the Scene view, ranked above other panel contexts in
  `ShortcutDispatcher::resolve`, rebindable and listed like all others (S25). §6 Table 5 names them
  and what they shadow.
- **R58** **Create Terrain dialog.** GameObject > 3D Object > Terrain and the Terrain window's empty
  state open it. The row keeps the label and follows the wizard precedent (`HierarchyWindow.cpp:506`,
  `:612`). Fields: Name, Preset, Terrain Size, Terrain Height, Heightmap Resolution, Control Texture
  Resolution, Detail Resolution, Mesh Resolution, Root Grid Size, Levels, Start Shape (Flat, Flat at
  Middle, Rolling Hills, Island, Import Raw...), Seed, Base Height, Base Layer (a texture), Add
  Terrain Collider (on). A live line states the CPU and GPU memory of the maps and warns per platform
  (over 2049 on mobile and web, over 1025 for one-to-one collision). **Create** makes one undo step
  "Create Terrain (Name)". The bare "+" for the heightmap goes. Presets are §6 Table 6, shared with
  `edit_terrain`'s `create`. `create_entity type=terrain` still makes an empty terrain for agents and
  scripts, and both routes call one `applyNewTerrainDefaults` (R91).
- **R59** **Terrain Settings** is a fifth tool icon (TEW:3261): Mesh (Terrain Size, Terrain Height,
  with Keep World Heights, which adds a `remap_heights` in the same step; Mesh Resolution, Root Grid
  Size, Levels, Auto Ranges); Maps (each map's size, depth and file, Resize..., Convert to 16-bit,
  Remove); Height Map (Import Raw..., Export Raw...); Collision (samples used, and a warning when the
  map is resampled); Runtime (Allow Runtime Edits, with its memory cost); and Frame Terrain. Every
  field is undoable and accepts typed values past its slider, and clicking a row's label resets it to
  default. Blend and density maps are created on first paint at their setting's resolution, one undo
  step with the stroke, instead of a bare "+".
- **R60** **The radius cap goes.** Limits are per terrain: minimum half a heightmap texel, maximum the
  terrain size (a brush that covers everything). The slider is logarithmic with its end at `max(50,
  terrainSize / 4)`, and typed values reach the true maximum. `[` and `]` still multiply by 1.15.
  Saved settings load unchanged. `sculpt_terrain`'s schema maximum becomes 100000, clamped per terrain
  by the handler, with the clamp in `warnings`. Large brushes stay interactive through R38.
- **R61** **Import Raw... / Export Raw...** Import: file picker, then a dialog with Depth (Bit 8, Bit
  16, Float 32, PNG, EXR), Resolution (width and height), Byte Order (Windows, Mac), Flip Vertically;
  each pre-filled by detection (R68), with a yellow marker on every guessed value. Value Range:
  Detected, Full, Source Min/Max in metres. Set Terrain Height From Range. Preview: a 256² hillshade
  and histogram with on-screen min, max, mean, and the share clipped, updating live. Import writes one
  undo step. A size that differs from the current map makes a new `terrain_edit_*` map by the
  structural path (R28); a same-size import is a delta. Export: Depth (16, 8, Float 32, PNG, EXR),
  Byte Order, Flip Vertically, Range (Full, Detected), and it prints size and path on success inside
  the window.
- **R62** **Terrain Info** panel at the bottom of the Terrain window, drawn while the pointer is over
  the terrain: Position (X, Y, Z), Height (metres, and percent of Terrain Height), Slope (degrees),
  Layer Weights (the top three, with layer names and bars; with a Base row when the weights do not sum
  to 1), Details density of the selected layer, Hole, Texel, and brush radius in metres and texels.
  All through the one query.
- **R63** **Pick from Terrain.** An eyedropper beside Height (Set Height), Slope range ends, Height
  range ends, Layer and Noise Size. While armed, the decal is white and the next click fills the value
  without editing the terrain. `I` arms the Height one; Esc disarms. Alt+click with no drag (R56) picks
  without arming: Layer on Paint Texture, otherwise Height.
- **R64** **Upload pacing during strokes** matches runtime pacing (DC9), with an 8 MB per frame editor
  budget: on a 4096² map the drawn surface may lag up to 5 frames while CPU queries, Info panel and
  decal are current, and the final state is always uploaded on the frame after the stroke ends.
- **R65** **Overlays.** Scene view: a Terrain section in the view settings popup (precedent
  `SceneWindow.cpp:2439-2446`), an Overlay combo in the Terrain window, keys `1`-`4` and `0`: Height
  (a colour ramp from 0 to Terrain Height), Slope (0-15 green, 15-30 yellow, 30-45 orange, over 45
  red, or the Slope Filter's range when it is on), Contours (interval in metres, auto by default),
  Layer Weights (palette per layer, with a legend of layer names). Later specs add sources through R90
  (T3 Holes and budgets, T4 tiles, T5 density), which take the next digits. Scene view only, saved
  with the workspace per scene, never in a game or camera image, and it updates at most 10 times a
  second from dirty rectangles. A legend sits in the Scene view's corner. Implementation is DC8.
- **R66** **Slope Filter** is the paint mask's new label, and it can reach every map op: Slope Filter checkbox,
  range, **Invert** (`Y`), Height Filter (likewise invertible) and **All Tools**. Stored keys: `paintUseMask` and its
  ranges keep their names and their meaning, filtering Paint Texture as `TEW:1794` does today. New keys with defaults,
  `filterAllTools` (false), `slopeFilterInvert` (false), `heightFilterInvert` (false), turn on the rest, so an old
  workspace behaves as it did (R83). The tool that makes ramps is named **Ramp**, never Slope, which avoids Terrain3D's
  clash.
- **R67** **Paint by Rules** section under Paint Texture: rows of Layer (with Base), Slope range,
  Height range, Invert, Opacity, Noise (edge breakup with Seed), drag to reorder, later rows over
  earlier ones. **Preview** shows the result in the Layer Weights overlay from a dry run; **Apply** is
  one undo step, `paint_by_rules` on all or on a rect. Rules are saved in `TerrainEditorSettings`. It
  is destructive and explicit: no hidden autoshader mask, which is Terrain3D's "Paint disables the
  autoshader".

### Agent UX (MCP, Lua, CLI)

- **R68** `edit_terrain`, `visualize_terrain` and `export_terrain_heightmap` in group `terrain` (D17), module row
  `registerTerrainTools` in a new `editor/mcp/tools/TerrainTools.cpp`. Import is `edit_terrain`'s `import_heightmap` op
  (Table 1), so it has `dry_run`, all-or-nothing and one undo step with no tool of its own. Schemas in §6. Detection for
  import is shared with the dialog through `TerrainRawIO.h`: size from file length (squares first, a power of two or a
  power of two plus one preferred), depth from extension then length, byte order from smoothness in both orders, range
  from the data. Ambiguity refuses with the candidates listed. `.png` reads 8 and 16 bits; `.exr` reads through
  `FloatImage`.
- **R69** `edit_terrain` is one undo step per call, all-or-nothing, `dry_run` runs everything on an
  overlay and discards it. Expected work above 4 M texels becomes a job (`wait`, `timeout_seconds`, as
  `bake_navigation`), computing on a snapshot off the main thread and committing on the main thread; a
  base changed meanwhile fails with a clear error.
- **R70** `sample_terrain` gains `region`, `stats` (`height`, `slope`, `layers`), `slope_bins`,
  `issues`, `hash`, and existing arguments and results do not change, apart from agent-authoring's rename of
  `foliage_layers` to `details` (R97). Full map up to 2049²; larger maps stride and report `stride`. Later specs add to it
  under these names only: T3's `info.material`, `info.holes`, per-point fields and `stats: ["coverage"]` over `region`
  (T3 R43; T3 D5: T1's names win), T4's per-point `state` and `world` (T4 R45, R46), T5's `foliage` over `region`
  (T5 R55). With all of them it stays within `tool_default` (R74).
- **R71** Framing a terrain is `visualize_terrain`'s `framing` (`fit`, `top`, `orbit`), through
  `SceneViewController::frameAll(sceneProject, includeTerrain)`, which the Terrain window's Frame Terrain also calls, for
  the terrain alone. `set_scene_camera` does not change: `frame_all` keeps S26 R18 and Alt+F, and its exception does not
  rise.
- **R72** `TerrainIssues` (pure checks plus component checks) is one list used by
  `sample_terrain.issues` and by a banner at the top of the Terrain window, each with `code`,
  `severity`, `message` and `fix`. Codes are in §6 Table 2. An error stays visible until fixed and
  shows a button where a fix exists (Convert to 16-bit, Retry Write, Create Terrain...). `Out::error`
  may also log, but the window and tool never rely on the console.
- **R73** `docs/mcp/guide/terrain.md` (served as `jasper://docs/terrain`, row in
  `docs/mcp/guide/README.md`; agent-authoring creates the same file, R97): when to use which tool, every op with fields,
  the shape and filter rules, units and spaces, warnings and issues, a worked flow (create, block out, paint by rules,
  verify, look), dry run, jobs, Lua. The agent guide gets one entry in its "Other areas" index, `terrain (edit_terrain):
  jasper://docs/terrain`, with no digest section and no line in the 3D bullet; it stays ≤ 8,192 bytes with ≥ 1,500 free
  (`budget.json` `guide`), and the entry needs AC74's case `terrain_find_docs`. `tool_groups`' description for `terrain`
  (`McpToolGroups.cpp:25`) is updated.
- **R74** `budget.json`: `groups.terrain {chars: 16000, tools: 10, spec}`, the one limit for T1-T5 (D17; the others
  refer here), lowered to the measured total plus 5 % when T4 lands. No T1-T5 tool gets an `exceptions` entry: every
  new tool fits `tool_default` (1,500) by its shape (R95). The 10 tools, sized by a script over the §6 schemas (estimates,
  nothing built): `sculpt_terrain` (2,699, its existing exception), `set_terrain_maps` (2,369; about 2,680 with
  agent-authoring), `sample_terrain` 1,431 with every later key, `edit_terrain` 1,461, `visualize_terrain` 1,434,
  `export_terrain_heightmap` 696, T2's `generate_terrain` 1,249 and `preview_noise_graph` 883, T4's `edit_terrain_world`
  884, agent-authoring's `edit_terrain_details` (about 1,100): about 14,320 in all. `test_tool_groups.py:208` lists the new
  tools. `docs/mcp/TOOLS.md` is regenerated.
- **R75** Lua on `Terrain` (§6 sketch), in a new `engine/core/script/binding/TerrainClassesLua.cpp`:
  `getHeightAt`, `getNormalAt`, `isHoleAt`, `contains`, `raycast` (always available, read-only);
  `getHeights`, `setHeights`, `applyOps`, and properties `heightMapWidth`, `heightMapHeight`,
  `allowRuntimeEdits`, and `setRuntimeUploadBudget(bytes)` (DC9). Writes on a terrain without Allow
  Runtime Edits fail with a Lua error naming the setting and the terrain. `applyOps` takes the same op
  tables as `edit_terrain` (world XZ for shapes), except `place_objects`, `erase_objects`,
  `remap_layers`, `import_heightmap` (authoring only), and returns the result as a table. No undo at runtime.
  `docs/scripting/terrain.md` documents all of it including costs, with Lua blocks under the doctest
  (`docs/scripting.md:242`). `lua-api.md:2810` points to it.
- **R76** Terrain3D's pain points are designed away as follows.

  | Pain (`terrain3d-editor-ux.md`) | Here |
  |---|---|
  | Silent size clamp under 2 m | No hidden clamp: per-terrain limits are shown, tools warn `size_clamped` |
  | Off-region strokes return silently | Validation error (R10), Scene view decal hides off the terrain and Info says Outside |
  | Queries return NaN outside | Edge clamp plus `inside` (R16) |
  | Range and size set by hand, "scale by 300-500" | Detection, hillshade, on-screen numbers, Set Terrain Height From Range (R61, R68) |
  | PNG 8-bit only, makeshift inspector UI | 16-bit PNG, EXR, a real dialog |
  | Key status only in the console; "restart twice" | Issues banner (R72); nothing here needs a restart |
  | Undo deep-copies whole regions | Tile XOR deltas per map; a paint stroke never copies heights (R24) |
  | Additive strength per motion event | Spaced stamps (R33) |
  | Reorder does not remap, only last slot deletes | `remap_layers` (R13) |
  | "Slope" names a tool and a filter; many Paint/Spray options | Ramp and Slope Filter; one Paint Texture tool with Opacity (R66) |
  | Paint disables the autoshader | Paint by Rules is explicit (R67) |
  | Agent API: start/operate/stop, void, unsanitised modifiers, random spin | `edit_terrain` returns everything, no modifiers, seeds only (R8, R9) |
  | Undo needs the plugin | The engine is usable with no editor; undo is the editor's layer on the delta |

### Runtime edits

- **R77** `allowRuntimeEdits` (Inspector and Terrain Settings: **Allow Runtime Edits**, default off)
  is the single switch. When on, at load: the height map (and each blend map) uses a stream image; the
  terrain gets an edit state; blend and density stay CPU readable (S14's R3 slot rule extended, since
  blend maps can otherwise cook compressed, `S14:123`, `S14:559`); the heightfield is built with
  `mMinHeightValue = 0` and `mMaxHeightValue = maxHeight` so later edits are not clamped. Off: none of
  it exists, and writes fail per R75.
- **R78** A runtime write never touches authoring or shared data: at load the terrain's maps detach
  into textures under private ids (`__terrain_runtime_<scene>_<entity>_<kind>`). Stopping Play
  restores the authored terrain byte for byte, and two terrains on one file diverge independently.
- **R79** Order within a frame: CPU pixels are written at the call, so `getHeightAt` is current on the
  next line. Node ranges refit in `MeshSystem::update`; collision refreshes before the next physics
  step, through `SetHeights` on the block-aligned sample rectangle grown by one cell
  (`JoltTerrainRefresh.{h,cpp}`, under `JASPERENGINE_PHYSICS_3D`), with the same per-sample function
  as `buildHeightFieldShape` so T3's rim rule (its R22, R28) holds in both, and activates bodies in
  the box; GPU upload follows pacing (DC9); foliage is flagged at most 4 times a second (T5 narrows
  it). `ColliderSource` other than the terrain heightfield is not touched.
- **R80** The NavMesh is not rebaked: a NavMesh Surface over an edited terrain reports stale as
  today's staleness does (a `navmesh_stale` issue), and the game calls `bakeAsync()`.

### Data and compatibility

- **R81** New fields default so old projects load unchanged, with no scene version bump
  (`docs/specs/README.md`, Serialization compatibility, as T3 R50): `allowRuntimeEdits` (false), and
  `TerrainComponent::holeMap` (empty; T1's foundation adds it, §8, and T3 R49 the rest). They go through
  `StreamComponents.cpp:573`, `RuntimeFields.gen.inc`, `ComponentHooks.cpp:152-158`,
  `RenderReflect.cpp:336`, `catalog_snapshot.json` and the factory, in one commit. `surfaceRevision`
  and the edit state are not serialized and reset in `EntityCloner.cpp:365`.
- **R82** Map files stay PNG: R16 height, RGBA8 blend, R8 density, R8 hole. An old 8-bit height map
  keeps working and stays 8-bit until the user converts it (`height_map_8bit` issue with the button).
  Old multi-channel height sources convert on first edit, as today.
- **R83** `TerrainEditorSettings` gains fields with defaults; `brushMode` integers stay, and new tools
  append. Lift Floors, Flatten Peaks and Cut/Fill Holes are appended, so the enum table at
  `StreamSchema.cpp:667` keeps every old value. Stored radius above 50 stays valid. `paintUseMask` keeps its key and meaning
  (R66). A stored `brushStrength` keeps its number, and R34 and R35 keep its click and held rates; only a moving stroke's
  deposit goes from per second to per distance, which no stored number could convert, so nothing migrates. Every key here is
  optional and new, so no version bump, migration or fixture (`AGENTS.md`, Invariants).
- **R84** Undo history is not persisted, so nothing migrates; a project's first run with T1 loads and
  draws the same (`test_upgrade_from_main.py` stays green). Writing a hole map bumps nothing; an older
  editor opening that scene preserves the unknown field (S03).
- **R85** Exported games: `TerrainEdit` queries read what cooks already keep (S14 R3). Allow Runtime
  Edits is visible to the cook and the player's variant check. Hole maps cook as T3 R52 says.

### Seams with T2 to T6

These come from the sibling specs (T2 §8 and N9, T3 D5 and §8, T4 D11, D12 and §8, T5 §8). Names
here win where theirs were assumptions (each of them says so).

- **R86** Store contract for T4. Texels are signed; a map's extent is `format(map).bounds`, not `[0,
  W)`. Each map has its own lattice; ops convert only through `worldOfTexel` and `texelOfWorld`, never `(i
  + 0.5) / W`. These two give each map its own frame, so no `frameOf` is needed (T4 D12).
  A write may refuse (past a limit, read only): `canWrite(map, rect)` runs for every
  rectangle before an overlay commits, so a refusal changes nothing. A write reports tiles created, and
  `takeUndoHook()` hands what it created to `TerrainMapDeltaCmd`, which runs the hook with the step (T4
  R30; the single-map store returns null).
  `preferredBlock(map)` and `beginLarge(map, rect)` exist; the single-map store answers the whole map
  and a null token.
- **R87** Large edits. No kernel allocates a buffer larger than `preferredBlock` plus its margin when
  the rectangle is larger than that block. With a non-null `beginLarge` token, `applyOps` works block
  by block straight on the store and the token holds undo (T4 R30); with a null token it uses the
  overlay, which holds only touched tiles. Both give the same bytes.
- **R88** `terrain::Batch` is the absolute write route for generators (T2 N9): `Batch batch(store)`,
  `setHeights(rect, normalized)`, `setLayerWeights(rect, layers, weights)`, `setDensity(rect,
  foliageLayer, values)`, `readHeights(rect, out)`, `commit()` returning `TerrainOpResult`, or
  `discard()`. Its `setLayerWeights` is the single encoder of layer weights: clamp 0..1, normalize a
  sum above 1, layer 0 takes the rest unless it has its own value (T2 R30). `paint_by_rules` uses it.
  The editor wraps a Batch's commit as one `TerrainMapDeltaCmd` with the caller's undo name.
- **R89** Op registration (T5 R23). `registerOp(name, validate, apply)` adds a kind from another
  file: it runs inside the same validation, overlay, result and undo, and returns its own result
  fields in the op's entry. An op's own keys arrive in its op object, which the listed schema leaves open (R95), so a later
  spec adds no schema text and the handler names the valid keys in its error. Registered ops may write non-map data through
  their own store (T5's `TreeStore`), which joins the same undo step. A registered op may declare `spansFrames` (T5's
  `bake_impostors` renders one view per frame): the call is then a job whatever its texel count.
- **R90** Overlay sources (T3 R44, T4 R46, T5 R57). `TerrainOverlay::registerSource(name, bake,
  legend)`: `bake` fills an RGBA8 rectangle from the store, `legend` returns colour stops. T1
  registers its four through this same interface. A source may instead declare that it needs a
  rendered frame (T3's `fetch_cost`). `visualize_terrain`'s `overlay` is a string checked in the handler, so a source adds no schema text, and
  `quality` and `foliage_quality` are those specs' own keys.
- **R91** `applyNewTerrainDefaults(TerrainComponent&)` in `TerrainEdit.h` is the one place new
  terrains get defaults: T3 R53 adds Limit Layers Per Pixel there, T5 F12 its Detail Distance. `TerrainEditWindow` keeps one-line
  slots for T2's `drawGenerateSection()` and for T5's panel, which also adds its Foliage section to Terrain Settings (T5 R20). Create Terrain's Start Shape lists
  `TerrainPresets::list()` and calls `TerrainGeneration::start()` once T2 has landed (T2 R61),
  replacing Rolling Hills and Island.
- **R92** Layer addressing. Paint ops map a layer to a blend map and channel only through
  `terrainLayerChannel(index)` in `engine/core/terrain/TerrainLayers.h` (T3's R15), and never assume
  nine layers (`MAX_TERRAIN_LAYERS` becomes 12 there). T1's foundation (§8) creates
  that header and `TerrainHoles.{h,cpp}` (`isHole`, `HoleMapView`, `rasterizeRegion`) as T3
  specifies; T3 extends them.
- **R93** `TerrainSurfaceQuery` is the abstract form of the query for regions and worlds (T5
  `FoliageRegion`, T4 R24): `sample`, `heightAt`, `raycast`. `SurfaceView` is its copied-texel form
  for jobs (T5 R7). `querySurface` uses the single-map implementation unless T4's hook applies.

### Upstream revision: no CI, lean MCP, Doriax, storage

- **R94 Where each AC runs.** There is no CI (`AGENTS.md`, Test). The gate per commit is `tools/check-changed`; before a push
  to `origin main` that needs it, the full suite (`WERROR=1 tools/build.sh linux`, `WERROR=1 tools/build.sh unit-tests`,
  `MCP_SUITE=all JASPER_SLOW_BUILD=1 JASPER_SLOW_TESTS=1 tools/build.sh mcp-tests -j <n>`). Every AC runs on this host (8 cores,
  software GL, lavapipe, Emscripten, Android build with no emulator) or is labelled not verifiable here with the command a
  person runs on that platform (§7's table). A row not run is reported unverified, never as passing.
- **R95 Lean tools.** Each tool asked for by T1-T6 was challenged against "merge or extend before adding a near-duplicate":

  | Asked for | Outcome | Why |
  |---|---|---|
  | `edit_terrain` | Kept, at most 1,500 | The one write tool; op items are open in the schema and closed in the handler (precedent: S47's `edit_sprite`) |
  | `import_terrain_heightmap` | Merged: `edit_terrain` op `import_heightmap` | Needs what `edit_terrain` has: `dry_run`, one undo step, a job |
  | `export_terrain_heightmap` | Kept | Writes a file, so it cannot be the read-only `sample_terrain`; nothing else exports |
  | `visualize_terrain` | Kept | Overlay plus framing, as `visualize_navigation`; `overlay` is a handler-checked string |
  | `include_terrain` on `set_scene_camera` | Removed | `visualize_terrain`'s `framing` does it, and a raised exception is not allowed (R71) |
  | `generate_terrain` | Kept (T2) | Own job, statistics and image; the agent's "make me a terrain" entry |
  | `preview_noise_graph` | Kept (T2) | Needs no terrain, works in Play, reports per node; `generate_terrain dry_run` does none of these |
  | `create_terrain_world` | Merged: `edit_terrain_world {create}` (T4) | Same pattern as `edit_graph {create}` |
  | `get_terrain_world_status` | Merged: `sample_terrain {world}` (T4) | A read of the same terrain; the status tool repeated its addressing |
  | `edit_terrain_world` | Kept, at most 1,500 (T4) | Its ops are allowed in Play and `edit_terrain` is not, so one tool cannot carry both annotations |
  | `bake_terrain_impostors` | Merged: `edit_terrain` op `bake_impostors` (T5, R89's `spansFrames`) | One more registered op |
  | T3's additions | None to the listing beyond two `visualize_terrain` and `sample_terrain` keys; `set_terrain_maps` unchanged (T3 R45) | Reflection reaches the fields |
  | `sculpt_terrain` | Stays for now | It is the near-duplicate, but AC13's baseline and Doriax's texts depend on it (§9) |

  Schemas stay lean, with validation in the handler. Descriptions say what a tool does, its default or limit, the one misuse and
  the sibling tool: no field lists, result lists or examples. A hint an agent must see is `next` in `structuredContent`: when a
  result has a warning or issue with a fix, `next` names the call that fixes or verifies it (`height_clipped_high`: `remap_heights`
  with a higher `maxHeight`; `navmesh_stale`: `bake_navigation`). An error names the fix. Names: `AGENTS.md` says
  "area noun first", but the rule enforced is `verb_noun` (`docs/mcp/conventions.md`; `VERBS` in `test_tool_conventions.py`), so
  every name here starts with a verb and then the area noun, one word per concept (`terrain`, `terrain_world`, `voxels`, `details`).
- **R96 Eval cases.** New tool text and every guide line get a case in `tools/mcp/eval/cases.py` that fails without it (AC74).
- **R97 Doriax overlap (D21).** `agent-authoring`, `render-correctness`, `draw-perf`, `ssr-probes` and `mesh-memory` are specs on
  `docs/superpowers/specs/`, not landed. Whichever lands second merges onto the first, with no duplicated capability. §8 lists what
  T1 shares with each.
- **R98 Writes and lints.** Every persistent write goes through `engine/core/io/AtomicFile.h` or a helper on it: maps through
  `TerrainMapFileWriter` (R31), `export_terrain_heightmap` through `safewrite::writeFileResolvedBeneath`, encoding to memory first
  (`stbi_write_*_to_func`, never a path). `JASPER_TERRAIN_STROKE_LOG` is append-only and test-only, so it gets one reasoned line in
  `tools/raw_writes_allowlist.txt` ("append-only logs" section). T1 vendors nothing, so `NOTICE.md` and `engine/notices.json` do
  not change (Terrain3D: ideas only, D14).
- **R99 Shaders.** T1 changes no shader (DC8), so no variant bit and no shader cache bump. An implementer who has to change one
  bumps the shader cache version in `editor/App.cpp` by one and records a Deviation.

## 6. Design constraints & decisions

- **DC1 Overlay everywhere.** Every op runs on an `OverlayMapStore`. Commit, `dry_run`, strokes, Esc,
  delta capture, job snapshots and live spline sessions are all the same object. That is why
  all-or-nothing, dry run and delta undo cost almost nothing extra.
- **DC2 Float working values, stored bytes at rest.** The overlay keeps floats so gentle stamps are
  not lost to rounding (the reason at `TerrainEditWindow.h:135-137`), per touched tile instead of per
  map. Height goes through normalized floats, lossless for R8 and R16 in a 32-bit float.
- **DC3 The hole seam.** D7 gives T3 the representation and T3 R18 fixes it: `holeMap`, R8, 0 =
  hole, 255 = solid. T1 runs the ops through `terrain::rasterizeRegion` (`TerrainHoles.h`) against `MapKind::Hole`
  with values 0 and 255 only (T3 D5). T1's foundation lands first (§8), so T1 creates the field
  and a minimal `TerrainHoles` to T3's definitions (R92) and T3 extends them. Nothing draws a hole
  until T3's consumers land, which the warning says.
- **DC4 Blend is still blend maps.** D4 says the store handles blend data in texels. `MapKind::Blend`
  is one RGBA map, as stored, and layers reach it through R92. `paint_layer` keeps today's rule:
  normalization is within the layer's own blend map (TEW:1469-1470, TEW:1865). `paint_by_rules` goes
  through R88's encoder and writes every blend map it needs.
- **DC5 Units.** C++ and Lua write heights in terrain-local units (0..`maxHeight`). MCP `space:
  "world"` (default) converts by the terrain's position and Y scale; a tilted terrain warns
  `rotated_terrain`. Lua `getHeightAt`, `getNormalAt`, `raycast` and op shapes use world XZ and return
  world values, because scripts think in world space. `getHeights`, `setHeights` are texel rectangles
  with terrain-local heights.
- **DC6 Tiles are 64×64 texels** for overlay and delta alike, addressed in store texels. It is
  independent of T4's world tiles.
- **DC7 XOR plus zstd.** One buffer replaces two rectangles and gives undo and redo the same code.
  Sculpt XORs are mostly small low bytes and compress well. Linking is safe: the editor links `zstd`
  (`CMakeLists.txt:1437`); the player does not need it because deltas are editor-only.
- **DC8 Overlays are editor meshes, not a shader mode.**
  - T3 owns the terrain material shaders, and painted terrains are at the 16-texture limit
    (`S11:87`). `TerrainOverlay` (`editor/terrain/`) follows `NavigationOverlay`: System-pool entities
    with `setEditorOverlay`, transparent, unlit, no shadows. It is a grid of up to 256×256 cells (7
    meshes at 16-bit indices) lifted conservatively, each vertex at the highest fine texel in its
    footprint plus `max(0.03, 0.01 * maxHeight)`, the cursor's own lift (TEW:3898), with a generated
    RGBA8 texture up to 1024² updated from dirty rectangles.
  - Where a far CDLOD level rises through the sheet, a second copy in the tools layer at 25 % opacity
    keeps it readable, the same technique as spline overlays (`SceneRender.h:305`). This trades
    pixel-exact alignment for zero render-path cost and zero variants.
  - Contours bake into the texture.
- **DC9 Partial upload, honestly.** Sokol takes the whole image, so T1's GPU upload is whole-image:
  - The CPU side is partial: dirty rectangles update the map, and on GLES3 and WebGL2 only the float
    shadow's rectangle. Drivers then copy the whole image.
  - A new `TextureRender::createDynamicHeightTexture(label, w, h, bits)` makes a stream image of R16
    where it filters, else R32F or R16F (the existing rule), with no mipmaps, linear, clamped.
    `Texture::adoptStreamRender` makes `getRender` return it. The first update happens before the
    first draw, since sokol forbids initial data.
  - Pacing: an upload at most once a frame per image, at most every `ceil(mapBytes / budget)` frames
    when the map is bigger than the frame's budget (default 2 MB on mobile and web, 8 MB on desktop,
    `Terrain:setRuntimeUploadBudget`). CPU query and collision are current at once; the drawn surface
    lags by at most that many frames, and the last edit always uploads.
  - No sokol patch here (D12). Region updates would need a new image usage on GL (single slot), D3D11
    (DEFAULT plus `UpdateSubresource`), Metal and Vulkan, and only GL and Vulkan can be tested on this
    host. `je-bench-terrain` records upload time per size on llvmpipe and lavapipe, and
    `docs/perf/terrain.md` holds the numbers on which T4 revisits this; T4's per-tile textures make
    the unit of upload a tile, which is the real partial upload.
- **DC10 No new system.** Edits flush from hooks guarded by `terrain.editState`: the existing terrain
  loop (`MeshSystem.cpp:7225-7233`), `updateTerrainRenderTextures` (`RenderSystem.cpp:5855`), the
  physics pre-step. Bodies in new files (`MeshSystemTerrainEdit.cpp`, `RenderSystemTerrainEdit.cpp`,
  `JoltTerrainRefresh.cpp`), as `RenderSystemShadows.cpp` does.
- **DC11 Characterize first.** The first commit, before any migration, adds `test_terrain_baseline.py`
  and its fixture, recorded from the unmigrated editor (§7, AC13). It is green before and must be
  green after: the regression proof for "no behaviour regressions".
- **DC12 Alt.** This editor follows Unity: Alt+LMB drag orbits (`SceneWindow.cpp:2047`) in every tool,
  by default. D16's Terrain3D Alt is the preference **Alt Alternates While Painting** (default off). A
  press with Alt starts nothing: past the drag threshold it is an orbit, and a release before it is a
  pick (R56, R63; T5 R46, T6 R60). Right mouse still orbits or looks, and every alternate is also a
  button and a rebindable key (`terrain.alternate`).
- **DC13 EXR yes, TIFF no.** `FloatImage` already reads and writes EXR, so it is cheap. Anything else
  is out.
- **DC14 Jobs only above 4 M texels.** Small calls answer inline, so an agent's common case stays one
  round trip.

### Table 1: operations

`w` = shape weight × filter weight × `strength` (default 1). Heights in the op's units. `h'` is the
new value. Brush column is the `TerrainBrushMode` it replaces.

| Op | Map | Fields | Effect | Brush |
|---|---|---|---|---|
| `raise` | height | `delta` | `h + delta·w` | Raise |
| `lower` | height | `delta` | `h - delta·w` | Lower |
| `set_height` | height | `height` | `lerp(h, height, w)` | Flatten ("Set Height") |
| `lift_floors` | height | `height` | as `set_height` where `h < height` | new (alternate, R56) |
| `flatten_peaks` | height | `height` | as `set_height` where `h > height` | new (Ctrl + alternate, R56) |
| `smooth` | height | `iterations` (1) | toward the 3×3 blur with step `radius/4` texels (TEW:1837-1847) | Smooth |
| `sharpen` | height | `iterations` | away from that blur | Sharpen |
| `add_noise` | height | `amplitude`, `noise_size` (4), `octaves` (4), `seed` (0) | `h + fractal(x/noise_size, z/noise_size)·amplitude·w` | Noise |
| `terrace` | height | `steps` | toward `round(n·(steps-1))/(steps-1)` | Terrace |
| `stamp` | height | shape `mask`, `delta` | `h + mask·delta·w` | Stamp |
| `erode` | height | `iterations`, `seed` | hydraulic droplets then thermal (`TerrainErosion`) | Erode |
| `ramp` | height | shape `polyline` of 2 points, `heights [a, b]` (default the surface at each end) | toward `lerp(a, b, along)` | Ramp |
| `remap_heights` | height | `in_range`, `out_range` | linear remap, weighted | new |
| `paint_layer` | blend | `layer`, `normalize` (true) | as TEW:1863-1867 | PaintLayer |
| `paint_base` | blend | `layer` (optional: that layer's map only) | all channels toward 0 | PaintBase |
| `paint_by_rules` | blend | `rules [{layer, slope, height, invert, opacity, noise}]`, `seed` | in order, later over earlier | new |
| `remap_layers` | blend, list | `order` | R13 | new |
| `paint_density` | density | `layer`, `strength` | toward 1 | PaintDensity |
| `erase_density` | density | `layer`, `strength` | toward 0 | EraseDensity |
| `cut_holes` | hole | shape; no `strength`, `falloff`, `feather` | texels whose centre is in the region become 0 (R12) | new |
| `fill_holes` | hole | shape (default all); same | those texels become 255 | new |
| `place_objects` | entities | `asset`, `spacing`, `count`, `scale`, `rotation_jitter`, `align_to_normal`, `instanced`, `seed` | `PlacementPlan` | PlaceObject |
| `erase_objects` | entities | | within shape, as TEW:2148 | EraseObject |
| `import_heightmap` | height | `path`, `format`, `width`, `height`, `byte_order`, `flip_vertically`, `range`, `source_min`, `source_max`, `set_max_height`, `resolution` (§6 schema); no shape | replaces the map; same size is a delta, another size the structural path (R61) | new |

Internal only (C++): `fill_noise` (R19), `apply_field` (R40), `set_heights` (import, Lua).

### Table 2: warnings and issues

Warnings in results: `outside_terrain` (partly), `clipped_low`, `clipped_high`, `no_change`,
`filter_excluded_all`, `map_created`, `size_clamped`, `map_8bit_steps`, `brush_under_one_texel`,
`holes_not_rendered`, `hole_no_collision` (a cut that opens no collision sample, T3 R27),
`rotated_terrain`, `stride_used`, `tile_created` (T4's stores).

Issues (severity): `no_height_map` (error, fix Create Terrain), `size_zero` (error), `max_height_zero`
(error), `map_load_failed` (error, fix Retry), `map_write_failed` (error, fix Retry Write),
`height_map_loading` (info), `mesh_unbuilt` (info), `height_map_8bit` (warning, fix Convert to
16-bit), `height_clipped_low`, `height_clipped_high` (warning, at least 0.1 % of texels at the rail,
fix Raise Terrain Height), `collision_resampled` (info, over 1025 samples), `navmesh_stale` (warning),
`layer_without_texture` (warning), `blend_map_format` (warning), `height_precision_low` (warning, R16F
on this device), `runtime_edit_map_too_large` (error, R46), `no_scene_camera` (error, replaces
only-log at `MeshSystem.cpp:7005`).

### Table 3: cost of a runtime edit (per frame that uploads)

CPU costs are R52. Upload is the whole map; GPU memory is map bytes × 2 on GL and Metal (two slots,
`sokol_gfx.h:8220`).

| Heightmap | R16 upload (desktop GL, D3D11, Metal, Vulkan) | R32F upload (GLES3, WebGL2) | Frames per upload at 2 MB budget / 8 MB budget |
|---|---|---|---|
| 257² | 0.13 MB | 0.26 MB | 1 / 1 |
| 513² | 0.53 MB | 1.05 MB | 1 / 1 |
| 1025² | 2.1 MB | 4.2 MB | 2-3 / 1 |
| 2049² | 8.4 MB | 16.8 MB | 5-9 / 2-3 |
| 4097² | 33.6 MB | 67 MB, refused on mobile and web (R46) | refused / 5 |

### Table 4: decal colours (linear RGB)

Raise white (1, 1, 1). Lower dark grey (0.2, 0.2, 0.2). Smooth dark red (0.5, 0, 0.2). Sharpen rose
(0.9, 0.3, 0.5). Lift Floors orange (1, 0.6, 0). Flatten Peaks amber (0.8, 0.45, 0.1). Set Height cyan
(0, 0.6, 1). Ramp yellow (1, 1, 0). Noise violet (0.6, 0.4, 1). Terrace teal (0, 0.7, 0.6). Stamp lime
(0.6, 1, 0.2). Erode brown (0.6, 0.4, 0). Paint Texture green (0, 0.5, 0). Paint Base light green
(0.4, 0.8, 0.4). Paint Details sage (0.5, 0.8, 0.3). Erase Details purple (0.5, 0.2, 0.5). Cut Holes
near black (0.1, 0.1, 0.1). Fill Holes off-white (0.85, 0.85, 0.85). Place crimson (0.86, 0.08, 0.24).
Erase Objects aqua (0.2, 0.9, 0.6). Pick white.

### Table 5: shortcuts in `ShortcutContext::TerrainBrush`

| Key | Command id | Action |
|---|---|---|
| `R` | `terrain.tool.raise` | Raise |
| `H` | `terrain.tool.setHeight` | Set Height |
| `S` | `terrain.tool.smooth` | Smooth |
| `A` | `terrain.tool.ramp` | Ramp |
| `N` | `terrain.tool.noise` | Noise |
| `T` | `terrain.tool.terrace` | Terrace |
| `E` | `terrain.tool.erode` | Erode |
| `K` | `terrain.tool.stamp` | Stamp |
| `B` | `terrain.tool.paintTexture` | Paint Texture |
| `D` | `terrain.tool.paintDetails` | Paint Details |
| `P` | `terrain.tool.placeObjects` | Paint Trees |
| `X` | `terrain.tool.cutHoles` | Cut Holes (Ctrl fills) |
| `I` | `terrain.pick` | Pick height |
| `Y` | `terrain.invertSlopeFilter` | Invert Slope Filter |
| `C` | `terrain.alternate` | The armed tool's alternate on or off (R56) |
| `1`-`4`, `0` | `terrain.overlay.*` | Height, Slope, Contours, Layer Weights, Off (later sources take `5` up, R90) |
| `-`, `=` | `terrain.brushWeaker`, `terrain.brushStronger` (extra bindings) | Strength |
| `[`, `]` | existing | Size |

`F`, `Q`, `W` stay. `R`, `T`, `E`, `X`, `H` shadow their editor meanings only while a brush is armed;
Esc disarms the brush if no stroke runs. Plain `C` has no binding in `DefaultCommands.cpp`, so it shadows
nothing (read at `b4f7816ea`).

### Table 6: Create Terrain presets

| Preset | Size | Height | Heightmap | Control | Detail | Mesh Res | Root | Levels |
|---|---|---|---|---|---|---|---|---|
| Test Patch (`test_patch`) | 64 | 16 | 256 | 256 | 256 | 16 | 1 | 5 |
| Mobile (`mobile`) | 512 | 100 | 512 | 512 | 256 | 16 | 2 | 5 |
| Small (`small`) | 256 | 64 | 512 | 512 | 512 | 32 | 2 | 4 |
| Medium (`medium`, default) | 1024 | 200 | 1024 | 1024 | 1024 | 32 | 2 | 5 |
| Large (`large`) | 2048 | 400 | 2048 | 2048 | 1024 | 32 | 2 | 6 |
| Huge (`huge`) | 4096 | 800 | 4096 | 2048 | 2048 | 32 | 4 | 6 |

The last three columns give one mesh quad per heightmap texel at the finest level (`root ·
2^(levels-1) · mesh res = heightmap`).

### API sketch: engine

```cpp
// engine/core/terrain/TerrainMapStore.h
namespace jasperengine::terrain {
    enum class MapKind : uint8_t { Height, Blend, Hole, Density };
    struct MapId { MapKind kind = MapKind::Height; int index = 0; };  // Blend: blend map, Density: foliage layer
    struct TexelRect { int x = 0, y = 0, w = 0, h = 0; };             // signed origin (T4), w and h >= 0
    struct MapFormat { TexelRect bounds; int channels = 0, bitsPerChannel = 0; bool exists = false; };
    struct TerrainFrame { float maxHeight = 0; };                     // heights 0..maxHeight
    struct WriteResult { bool ok = false; std::string error; int tilesCreated = 0; };
    class TerrainLargeEdit;      // R87: the undo token of a block-by-block edit
    class TerrainStoreUndoHook;  // R86: undo and redo of what a store created on write

    class TerrainMapStore {
    public:
        virtual ~TerrainMapStore() = default;
        virtual TerrainFrame frame() const = 0;
        virtual MapFormat format(MapId map) const = 0;
        // normalized floats, row by row, channels interleaved; rect inside the map
        virtual bool read(MapId map, TexelRect rect, std::span<float> out) const = 0;
        // rounded per R11; only changed texels count as dirty
        virtual WriteResult write(MapId map, TexelRect rect, std::span<const float> in) = 0;
        // stored bytes (deltas, hashes, raw export)
        virtual bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const = 0;
        virtual WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) = 0;
        virtual bool canWrite(MapId map, TexelRect rect, std::string* why) const = 0;
        virtual WriteResult createMap(MapId map, MapFormat format, float fill) = 0;
        virtual void takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) = 0;
        // never assume (i + 0.5) / W or a rectangle at 0 (R86)
        virtual Vector2 worldOfTexel(MapId map, int x, int y) const = 0;      // terrain-local XZ of a texel's sample
        virtual Vector2 texelOfWorld(MapId map, float localX, float localZ) const = 0;
        virtual TexelRect preferredBlock(MapId map) const = 0;
        virtual std::unique_ptr<TerrainLargeEdit> beginLarge(MapId map, TexelRect rect) = 0;   // null: use the overlay
        virtual std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() = 0;                      // null: nothing created
    };
}

// engine/core/terrain/TerrainOps.h
namespace jasperengine::terrain {
    struct Shape { /* type, center, radius, min, max, points, mask pixels, size, rotation, falloff, feather, spacing */ };
    struct Filter { /* slope, height, layer ranges, invert, softness */ };
    struct Op { OpType type; Shape shape; Filter filter; /* the Table 1 fields */ };
    struct OpOptions { bool parallel = true; };
    TerrainOpResult validateOps(const TerrainMapStore& store, std::span<const Op> ops);
    TerrainOpResult applyOps(TerrainMapStore& store, std::span<const Op> ops, const OpOptions& options = {});
    TerrainOpResult setHeights(TerrainMapStore& store, TexelRect rect, std::span<const float> localHeights);
    void registerOp(std::string_view name, OpValidateFn validate, OpApplyFn apply);   // R89
    class Batch;                                                                       // R88
    using FieldFn = FieldSample (*)(void* user, float localX, float localZ, std::span<const float> current);
    TerrainOpResult applyField(TerrainMapStore& store, MapId map, TexelRect rect, FieldFn field, void* user);
    uint64_t mapHash(const TerrainMapStore& store, MapId map);   // util/Fnv.h over stored bytes
}

// engine/core/terrain/TerrainSurface.h
namespace jasperengine::terrain {
    struct SurfaceView { const uint8_t* pixels; int width, height, channels, bytesPerChannel; float size, maxHeight;
                         HoleMapView holes; };                                         // T3's view (TerrainHoles.h)
    struct SurfaceSample { float height; Vector3 normal; bool hole, inside, present, exact; };
    SurfaceSample sampleSurface(const SurfaceView& view, float localX, float localZ);
    float sampleHeight(const SurfaceView& view, float localX, float localZ);
    bool raycastSurface(const SurfaceView& view, const Vector3& origin, const Vector3& direction, float maxT,
                        bool skipHoles, Vector3& point, float& t);
    class TerrainSurfaceQuery {                                                        // R93
    public:
        virtual ~TerrainSurfaceQuery() = default;
        virtual SurfaceSample sample(float localX, float localZ) const = 0;
        virtual float heightAt(float localX, float localZ) const = 0;
        virtual bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const = 0;
    };
}

// engine/core/object/TerrainEdit.h
namespace jasperengine {
    terrain::SurfaceSample querySurface(TerrainComponent& terrain, float localX, float localZ);   // R14; T4 adds its branch here
    void applyNewTerrainDefaults(TerrainComponent& terrain);                                      // R91
    class TerrainEditEvents {                                                                     // R42
    public:
        using Handler = std::function<void(Entity terrain, terrain::MapId map, terrain::TexelRect rect)>;
        static Subscription subscribe(Handler handler);                                           // RAII (S09)
    };
}
```

### API sketch: Lua

```lua
local terrain = Terrain(self.scene, self.entity)
local y = terrain:getHeightAt(x, z)              -- world XZ, world Y, clamped to the edge
local n = terrain:getNormalAt(x, z)              -- Vector3, world
if terrain:contains(x, z) and not terrain:isHoleAt(x, z) then end
local hit = terrain:raycast(origin, direction, 100)   -- RayReturn: hit, distance (world), point, normal, body

terrain.allowRuntimeEdits = true                 -- before the terrain loads, or set it on the component in the editor
local crater = terrain:applyOps({
    {op = "lower", shape = {type = "circle", center = {x, z}, radius = 6}, delta = 2},
    {op = "paint_layer", layer = 2, shape = {type = "circle", center = {x, z}, radius = 8}},
})
print(crater.ops[1].changed_texels, #crater.warnings)
local w, h = terrain.heightMapWidth, terrain.heightMapHeight
local values = terrain:getHeights(0, 0, 16, 16)  -- 256 terrain-local heights
terrain:setHeights(0, 0, 16, 16, values)
```

### MCP schemas

The notation is `McpSchema.h` (`object`, `stringEnum`, `number(desc, min, max)`, `withDefault`).
Character counts are estimates by a script over these schemas; set `budget.json` to the real measure
plus 5 %.

```cpp
// edit_terrain (1,461 characters by script)
// "Declarative terrain edits, all-or-nothing, one undo step per call, no rates: absolute heights or signed deltas, seeded.
//  dry_run changes nothing and returns the same result. Over 4M texels: a job (get_job). For a look use visualize_terrain,
//  for numbers sample_terrain. Ops and fields: jasper://docs/terrain."
object({
  {"scene", sceneRef()},
  {"entity", entityRef("Default: the only terrain.")},
  {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
  {"create", string("Size preset for a terrain with no maps.")},   // handler: Table 6's ids
  {"map_resolution", integer("New maps.", 2, 8192)},
  {"ops", Json{{"type", "array"}, {"maxItems", 256}, {"items", anyObject with properties {
      {"op", string("raise, lower, set_height, smooth, paint_layer, paint_by_rules, cut_holes...")},
      {"shape", anyObject("all, circle, rect, polyline, mask or stroke.")},
      {"filter", anyObject("slope, height, layer, invert, softness.")}
  }, required {"op"}}}},
  {"dry_run", withDefault(boolean(""), false)},
  {"hash", withDefault(boolean("Hash each changed map."), false)},
  {"wait", withDefault(boolean("A job waits for its end."), true)},
  {"timeout_seconds", number("", 0)}
}, {"ops"})
// The handler closes what the listing leaves open. Op fields, flat in the op object (Table 1, R5, R6):
//   shape {type, center, radius, min, max, points, closed, times, path, size, rotation, falloff, feather, spacing},
//   filter {slope, height, layer {index, min, max}, invert, softness}, height, heights, delta, strength, in_range, out_range,
//   layer, order, normalize, rules [{layer, slope, height, invert, opacity, noise}], steps, iterations, seed, noise_size,
//   amplitude, octaves, asset, instanced, spacing, count, scale, rotation_jitter, align_to_normal, and the import fields below.
//   An unknown op or key fails naming the valid ones. T5 R23 adds ops the same way (R89).
// import_heightmap: {op, path, format ("auto"), width, height, byte_order ("auto"), flip_vertically, range ("auto", "full", "source"),
//   source_min, source_max, set_max_height, resolution}; the call's dry_run gives detection and a preview only.
//   Height formula: normalized = (value - range_low) / (range_high - range_low); world = normalized * max_height + position.y.
// result: {terrain, dry_run, undo_name, created_maps: [...],
//   changed: [{map: "height"|"blend:0"|"hole"|"density:1", rect: [x, y, w, h], local: {min: [x, z], max: [x, z]}, texels}],
//   ops: [{op, changed_texels, clipped_low, clipped_high, filtered_out, outside, seed?, placed?, erased?,
//          hole_texels?, hole_fraction?, map_created?, map_removed?, collision_samples_open?, detected?}],
//   stats: {height_min, height_max, height_min_before, height_max_before, mean_delta, max_delta},
//   warnings: [{op, code, message}], next?, hashes?: {map: "hex"}, job_id?, status?}, plus a preview image for import_heightmap.
//   annotations: not read-only, not destructive, idempotent only with absolute ops; allowedWhilePlaying false.

// visualize_terrain (1,434 characters by script, with T3's and T5's keys)
// "Show a terrain overlay in the Scene view (none hides it), frame the terrain and return a Scene view PNG with the overlay's
//  legend and the camera. The view stays as set; no project data changes. Overlay names: jasper://docs/terrain."
object({
  {"scene", sceneRef()}, {"entity", entityRef("Default: the only terrain.")},
  {"overlay", string("height, slope, contours, layer_weights, holes, density...")},
  // handler: none, height, slope, contours, layer_weights; R90 adds T3 layer_count, fetch_cost, holes; T4 tiles, tile_state, lod,
  // collision; T5 density, tree_density.
  {"contour_interval", number("World units; default: auto.", 0)}, {"slope_range", array(numberItem, "Degrees.")},
  {"layer", integer("", 0, 64)}, {"opacity", withDefault(number("", 0, 1), 0.7)},
  {"framing", withDefault(stringEnum("", {"keep", "fit", "top", "orbit"}), "fit")},
  {"yaw", number("Degrees.")}, {"pitch", number("Degrees.", -89, 89)},
  {"region", anyObject("{min:[x,z],max:[x,z]}")},
  {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
  {"quality", string("Tier for this capture.")}, {"foliage_quality", string("Tier for this capture.")},
  {"screenshot", withDefault(boolean(""), true)}, {"max_width", withDefault(integer("", 0, 4096), 1024)}
})
// result: {overlay, legend: [{value, color, label}], camera: {position, target}, view_rect_in_window}, plus a ToolResult image.
//   readOnly true for project data; allowedWhilePlaying false.

// sample_terrain, added keys (1,431 characters in all by script, with T4's and T5's; today's keys and replies are unchanged)
  {"region", anyObject("{min:[x,z],max:[x,z]}")},
  {"stats", array(string(""), "height, slope, layers, coverage.")},   // handler checks the names; T3 R43 adds coverage
  {"slope_bins", withDefault(integer("Over 0-90 degrees.", 1, 90), 9)},
  {"issues", withDefault(boolean("Problems and fixes."), false)},
  {"hash", withDefault(boolean("Content hash of each map."), false)},
  {"foliage", withDefault(boolean("Details and trees in region."), false)},   // T5 R55
  {"world", anyObject("A Terrain World's tiles, with wait_for.")},             // T4 R45
  {"timeout_seconds", number("", 0)}                                           // T4 R45's wait
// grid's default stays 9 only when none of points, stats, issues, hash is given.
// result adds: stats: {texels, stride,
//     height: {min, max, mean, p05, p50, p95, clipped_low_share, clipped_high_share},
//     slope: {min, max, mean, bins_deg: [..], shares: [..]},
//     layers: [{index, name, coverage, mean_weight}], base_coverage},
//   issues: [{code, severity, message, fix}], hashes: {height: "hex", "blend:0": "hex", ...}

// export_terrain_heightmap (696 characters by script)
// "Write the heightmap to a raw or 16-bit image file. Returns size, depth, value range and hash. Importing the file with range
//  full gives the same heights back."
object({
  {"scene", sceneRef()}, {"entity", entityRef("Default: the only terrain.")},
  {"path", string("Project-relative.")},
  {"format", withDefault(stringEnum("", {"r16", "r8", "r32f", "png", "exr"}), "r16")},
  {"byte_order", withDefault(stringEnum("", {"little", "big"}), "little")},
  {"flip_vertically", withDefault(boolean(""), false)},
  {"range", withDefault(stringEnum("", {"full", "auto"}), "full")}
}, {"path"})
// result: {path, format, width, height, bytes, min, max, hash, warnings}. Written beneath the project (the call's write scope); the flush of R30 runs first.
```

Worked agent flow, for the guide:

```json
{"tool": "tool_groups", "enable": ["terrain"]}
{"tool": "create_entity", "type": "terrain", "name": "Island"}
{"tool": "edit_terrain", "create": "medium", "ops": [
  {"op": "set_height", "height": 40},
  {"op": "add_noise", "amplitude": 25, "noise_size": 120, "seed": 7},
  {"op": "set_height", "height": 80, "shape": {"type": "circle", "center": [0, 0], "radius": 60, "feather": 0.4}}]}
{"tool": "sample_terrain", "stats": ["height", "slope"], "issues": true, "hash": true}
{"tool": "edit_terrain", "ops": [{"op": "paint_by_rules", "rules": [
  {"layer": 0}, {"layer": 1, "slope": [35, 90]}, {"layer": 2, "height": [70, 200], "noise": 0.2}], "seed": 7}]}
{"tool": "visualize_terrain", "overlay": "slope", "framing": "fit"}
{"tool": "undo"}
```

## 7. Acceptance criteria

Test homes: `engine/tests/core/terrain/*Test.cpp` (core, CPU, globbed by
`engine/tests/core/CMakeLists.txt:17`), `editor/tests/terrain/*Test.cpp`,
`tools/mcp/tests/test_terrain_*.py` (`CI = True`, `EDITOR = {"create": True}`),
`engine/tests/core/render/TerrainEditGpuTest.cpp` (golden: made on glcore, matched on Vulkan,
`docs/testing.md:639-644`).

**Engine and determinism**
- **AC1 (R1, R2)** `TerrainOpsDeps.NoEcsOrRenderIncludes` greps `engine/core/terrain/`.
  `TerrainStore.RawAndOverlayAgree`: read, write and bytes round trip through both stores; overlays
  over rectangles crossing tiles.
- **AC2 (R3, R10, DC1)** `TerrainOverlay.DiscardLeavesBaseBytes`,
  `TerrainOverlay.CommitWritesExactlyChangedTexels`, `TerrainOps.FailedListChangesNothing` (third op
  invalid, base hash unchanged).
- **AC3 (R4, R5, R7)** `TerrainOpsKernels.*`, one test per op in Table 1 against a hand-computed small
  map, for each shape, including `raise` twice equal to `delta * 2` and `set_height` idempotent;
  `TerrainOps.ClampsAreCounted`.
- **AC4 (R6)** `TerrainFilter.SlopeHeightLayerAndInvert`: exact texel sets on a synthetic ramp, invert
  is the complement, out-of-filter bytes are unchanged, `softness` band values.
- **AC5 (R8)** `TerrainGolden.Hashes`: a list of 40 op sequences on a fixed seed map, FNV hashes
  pinned in `engine/tests/core/terrain/golden/op_hashes.json`, run on the core suite on this host (Windows and macOS: the
  table at the end of §7, unverified until run) and by `tools/test-projects/terrain-edit` under
  `tools/web/gates/terrain-edit.json`, which compares the same hashes in wasm.
  `TerrainGolden.ThreadCountDoesNotMatter`: 1 and 8 workers give equal bytes.
- **AC6 (R9)** `TerrainOps.ResultNumbersMatchDiff`: for random op lists (fixed seed), `changed_texels`
  and the rects equal a byte diff of before and after.
- **AC7 (R11)** `TerrainOps.UntouchedTexelsKeepTheirBytes`: on 8-bit and 16-bit maps with odd values.
- **AC8 (R12, R16, R92, DC3)** `TerrainHoles.CutFillAndQuery`: `cut_holes` then `isHole`, a second
  identical cut changes 0 texels, last fill removes the map, 0 and 255 only, height bytes untouched;
  `test_terrain_ops.py::test_holes_warn_until_s55` (T3 removes the assertion with the warning);
  `TerrainLayers.ChannelMappingIsTheOnlyRoute`.
- **AC9 (R13)** `TerrainLayers.RemapMovesPaintAcrossBlendMaps`: layer 1 moved to index 5 keeps its
  weights exactly; `test_terrain_ui.py::test_delete_layer_keeps_paint`.

**Query**
- **AC10 (R14, R15)** `TerrainSurface.MatchesShaderFetch` (software fetch as
  `TerrainHeight.MatchesTheTerrainShader`), `TerrainHeight.*` unchanged and green, `MeshRaycast.Kinds`
  unchanged and green, `JoltShapeBuilderTest` unchanged and green.
  `TerrainSurface.NormalIsTodaysForwardDifference`.
- **AC11 (R16)** `TerrainSurface.NeverNaN`: outside points, no map, zero size, huge coordinates.
  `TerrainSurface.RaycastSkipsHolesOnRequest`.
- **AC12 (R14)** `TerrainOpsDeps.OneQuery`: a grep test that no file outside `engine/core/terrain/`
  decodes height texels bilinearly (the names `bilinearHeightSample`, `decodeHeightTexel` are gone).

**Migration, no regressions**
- **AC13 (R17, R18, DC11)** `test_terrain_baseline.py::test_sculpt_terrain_matches_baseline`: 14 map
  modes (and 2 placement modes by count and spacing) through unchanged `sculpt_terrain` calls on a 64²
  R16 map, all 4096 `normalized` values via `sample_terrain points`, compared with
  `tools/mcp/tests/fixtures/terrain_brush_baseline.json` recorded on the unmigrated editor. Tolerance
  1/65535 (heights), 1/255 (blend and density through the same fixture's PNG bytes). **Red control:**
  run it with the `smooth` kernel step changed from `radius/4` to `radius/2`.
- **AC14 (R19)** `test_terrain_baseline.py::test_generated_maps_match_baseline`: `flat`, `middle`,
  `fractal_noise` file hashes pinned from the unmigrated editor.
  `TerrainOpsKernels.RandomNoiseIsSeededAndPortable`. `test_world.py::test_terrain_sculpt` unchanged
  and green.
- **AC15 (R20)** `TerrainSplineBakeTests.*` (all eight,
  `editor/tests/util/TerrainSplineBakeTest.cpp:112-419`) unchanged and green; `test_splines.py` green;
  `LiveSessionAllocatesNoWholeMap` new, with the allocation counter.
- **AC16 (R21)** `TerrainPlacement.SameSeedSamePlan`, `TerrainPlacement.RespectsSpacing`,
  `TerrainPlacement.OneAttemptPerStamp`.
- **AC17 (R22)** `TerrainOpsDeps.OldWritersAreGone` (reading sources, as AC1): `TerrainMapPatchCmd`,
  `scriptedStampSeconds`, `workingPixels` have no hit under `editor/`.
- **AC18 (R23)** `TerrainEdit.HeightEditDoesNotRebuild`: `numNodes` memory untouched and
  `heightMapLoaded` stays true through a stroke; ranges equal a full rebuild's afterwards.
  `TerrainEdit.SplineConformSeesRevision`, `test_splines.py::test_conform_follows_sculpt` (new). **Red
  control:** do not bump `surfaceRevision`; the conform test fails.

**Delta undo and saving**
- **AC19 (R24, R25)** `TerrainMapDeltaCmdTest.UndoRedoAreBitExact` (R8, R16, RGBA8, R8 density,
  multi-map step), `.RefusesAfterMapChanged`, `.CompressedAndRawTilesBothWork`.
  `test_terrain_ops.py::test_undo_redo_hash` through `sample_terrain.hash`.
- **AC20 (R26, R27)** `TerrainMapDeltaCmdTest.MemoryBound`: asserts R26's formula;
  `TerrainStrokeSession.SmallStrokeOnHugeMapNoWholeMapAlloc`: allocation counter under 1 MB for a
  1-tile stroke on 4096². `test_undo_ui.py::test_esc_cancels_a_terrain_stroke` unchanged and green.
  **Red control:** eager before-copy in the overlay; the allocation test fails.
- **AC21 (R29, R30)** `TerrainMapPersistenceTest.CommitDoesNotEncode`: writer queue empty until the
  debounce; `.UndoDoesNotEncode`; `.FlushPoints`: scene save writes current pixels;
  `test_terrain_ops.py::test_save_writes_map`: PNG decodes to the same hash after `save_scene`.
- **AC22 (R31)** `TerrainMapPersistenceTest.FailedWriteKeepsSceneModifiedAndRaisesIssue` (a read-only
  target), and `sample_terrain.issues` shows `map_write_failed`; after making it writable, Retry
  clears it.
- **AC23 (R32)** `test_hot_reload.py::test_terrain_map_write_is_no_reload` (new): stroke, debounce,
  wait past the watcher's echo window, no reload and no console error, `TexturePool` generation
  unchanged; a non-owned map changed on disk still hot reloads; a `terrain_<scene>_...` map is copied
  to `terrain_edit_*` at first edit.

**Strokes**
- **AC24 (R33, R34)** `TerrainStroke.EventRateIndependent`: one polyline sampled at 30, 120, 1000
  events per second gives byte-identical maps. `TerrainStroke.ClickIsOneStamp`: equals today's
  single-click value. **Red control:** deposit by event (`stampCount = 1`); the test fails.
- **AC25 (R35)** `TerrainStroke.HoldDepositsAtTheFlowRate`,
  `.SpeedAboveThresholdDoesNotChangeOutcome`.
- **AC26 (R36)** `TerrainStroke.RampDependsOnlyOnEnds`.
- **AC27 (R37)** `TerrainStroke.PressureScalesStrengthAndSize`, `.ZeroPressureAfterPenIsClick`. Real
  tablets: manual, `docs/specs/checklists/T1.md`, not verifiable on this host.
- **AC28 (R38)** `TerrainStrokeSession.BudgetDoesNotChangeResult`: 0.1 ms and unlimited budgets give
  equal bytes.
- **AC29 (R39)** `test_terrain_ui.py::test_stroke_log_replays_exactly`: real mouse drag with
  `JASPER_TERRAIN_STROKE_LOG`, undo, replay log through `edit_terrain`, hash equal.
- **AC30 (R33)** `test_terrain_ui.py::test_mouse_rate_does_not_change_the_drag`: `ui.drag_to` of one
  straight line with `steps=5` and `steps=80`, mean height difference over the swept region under 1 %
  of the peak.

**Runtime and Lua**
- **AC31 (R75)** `test_terrain_lua.py::test_queries`: `getHeightAt`, `getNormalAt`, `isHoleAt`,
  `contains`, `raycast` printed in Play equal `sample_terrain` values within 1e-3, on a translated and
  scaled terrain; `python3 -m lua_api doctest` passes `docs/scripting/terrain.md`;
  `tools/lua_api/tests/test_lua_api.py`.
- **AC32 (R75, R77)** `test_terrain_lua.py::test_edit_needs_the_setting`: a Lua error naming
  `allowRuntimeEdits`.
- **AC33 (R78)** `test_terrain_lua.py::test_runtime_edit_is_isolated`: edit in Play, stop,
  `sample_terrain.hash` equal to before; a second terrain on the same file unaffected.
- **AC34 (R79)** `TerrainCollisionRefreshTest.SetHeightsMatchesRebuild`
  (`editor/tests/engine/physics/`): after an edit the shape's `GetHeights` matches a fresh
  `buildHeightFieldShape` within quantization; `.RaysHitTheNewSurface`, `.SleepingBodyWakes`.
  `test_terrain_lua.py::test_ball_falls_into_crater`: a dynamic ball in Play reaches the crater floor.
- **AC35 (R43, R45, DC9)** `TerrainEditGpu.UploadMatchesFreshLoad`: draw after `setHeights` equals a
  golden of the same map loaded fresh (glcore; Vulkan on lavapipe);
  `TerrainEditGpu.GlesFloatShadowConverts` under the existing R16 fallback path with sokol's format
  forced; `TerrainEditGpu.StagingRefusalRetries` with a tiny `GpuResourceLimits::streamStagingBytes`.
- **AC36 (R64, DC9)** `TerrainEditGpu.PacingUploadsAtLeastTheLast`: at a 1 MB budget a 1025² map
  uploads every 3rd frame and the final state is drawn within the bound.
- **AC37 (R44, R43)** `GATE=terrain-edit tools/build.sh web-gate` (`st`): hashes match, the crater's
  pixel at the centre changes colour after a C++ `applyOps`; `ANDROID_PROJECT=terrain-edit
  tools/build.sh android` builds. `SANITIZE=address,undefined tools/build.sh unit-tests` with
  `--gtest_filter=Terrain*`.

**Performance**
- **AC38 (R47-R52)** `je-bench-terrain` under `tools/perf/check_bench.py` with the R47-R52 thresholds
  in `bench-thresholds.json`. Run on this host with `RB_SLOTS=2` so nothing else runs. Results in
  `docs/perf/terrain.md`.
- **AC39 (R53)** `TerrainEdit.ZeroCostWhenUnused`: allocation counter over 1000 frames of an unedited
  terrain with Terrain window closed; `sizeof(TerrainComponent)` asserted;
  `test_terrain_visualize.py::test_overlay_off_has_no_entities`.
- **AC40 (R54)** A size measurement script (`tools/perf/terrain_size.py`) against the merge base on
  Linux glcore and web `st`, recorded; over budget fails the landing.

**Editor UX** All below in `tools/mcp/tests/test_terrain_ui.py`, driven with `find_ui_item` and
`ui_input.py` on an isolated editor, plus a screenshot per feature from `tools/editor-shot.sh` kept in
`terrain-evidence/`.
- **AC41 (R55)** `test_decal_color_follows_operation`: pixel colour at the decal's rim for Raise, then
  Ctrl held (Lower), Shift (Smooth), `C` (Lift Floors), before any click; Alt leaves it Raise, and with
  **Alt Alternates While Painting** on, Alt gives Lift Floors.
- **AC42 (R56, DC12)** `test_modifier_grammar`: each chord's effect on `sample_terrain`; each alternate
  from its button and from `terrain.alternate`; by default, in every tool, Alt+drag moves the camera and
  leaves the hash unchanged, and Alt+click under the drag threshold picks with camera and hash unchanged;
  with **Alt Alternates While Painting** on, Alt+drag on Raise lifts floors and leaves the camera.
- **AC43 (R57)** `CommandCatalogTest.TerrainBrushContextOutranksPanels`,
  `ShortcutDispatcherTest.TerrainKeysOnlyWhileArmed` (editor tests);
  `test_terrain_ui.py::test_keys_choose_tools`; `list_shortcuts` lists them.
- **AC44 (R58)** `test_create_terrain_dialog`: open from the menu, choose Small, Create; maps, sizes
  and collider are as Table 6; one `undo` removes all; the empty state offers the same form; the "+"
  label is gone (`find_ui_item`).
- **AC45 (R59)** `test_terrain_settings`: Terrain Height with Keep World Heights keeps
  `sample_terrain` world heights within 1e-2; Frame Terrain frames; first paint creates a blend map
  and one undo removes both.
- **AC46 (R60)** `test_radius_follows_terrain`: on a 4096 terrain a typed radius of 1000 holds and a
  stroke paints it; `TerrainBrushLimits.PerTerrain`; a saved 50-limited setting loads unchanged.
  `test_terrain_baseline.py` confirms `sculpt_terrain size: 50` keeps its meaning.
- **AC47 (R61)** `test_import_export_dialogs`: a fixture `.r16` with big-endian order pre-fills
  correctly with its marker, min and max shown, preview non-empty, Import then undo restores;
  `TerrainRawIO.*` (below, AC56).
- **AC48 (R62)** `test_info_panel`: rows equal `sample_terrain` at that point within display rounding.
- **AC49 (R63)** `test_pick_from_terrain`: fills Height, armed and by Alt+click, no change of hash.
- **AC50 (R65, DC8)** `test_terrain_visualize.py::test_overlays_change_pixels`: each of the four
  differs from none; and `test_overlay_not_in_game_view`: a Game view screenshot is unchanged; golden
  of Slope on a fixture terrain (llvmpipe, class (b) of `docs/testing.md`, listed in
  `tools/mcp/gpu_lane.json`'s rules). `TerrainOverlayBake.Height`, `.Slope`, `.Contours`,
  `.LayerWeights` as CPU unit tests of the baked texels.
- **AC51 (R66)** `test_slope_filter_every_op_and_invert`: raising with a filter on flat and steep; `Y`
  inverts.
- **AC52 (R67)** `test_paint_by_rules_preview_and_apply`: preview leaves the hash unchanged, Apply is
  one undo, `layers` coverage equal to dry run.
- **AC53 (R72)** `test_error_states_in_window`: 8-bit map shows the banner and Convert fixes it;
  missing map shows `map_load_failed`; `TerrainIssuesTest.*` each code; no issue message is only in
  the console.

**Agent UX**
- **AC54 (R68, R69)** `test_terrain_ops.py`: `test_all_or_nothing`, `test_dry_run_changes_nothing`
  (same result numbers, hash and history unchanged), `test_one_undo_per_call`,
  `test_outside_shape_fails_with_bounds`, `test_result_shape`, `test_create_preset`,
  `test_large_edit_is_a_job` (`get_job`), `test_job_conflict_fails`.
- **AC55 (R70)** `test_terrain_ops.py::test_sample_stats_and_issues`: exact numbers on a synthetic map
  (percentiles, histogram sums to 1, coverage), today's `grid` and `points` replies byte-identical
  (fixture), `test_hash_tracks_content`.
- **AC56 (R68)** `TerrainRawIO.DetectSizeDepthOrderRange`, `.AmbiguousRefuses`,
  `.RoundTripEveryFormat`, `.FlipAndResample`; `test_terrain_raw.py`: export then import with range
  full gives an equal hash for R16, PNG; R8 within 1/255, R32F within 1/65535; `dry_run` returns an
  image and writes nothing.
- **AC57 (R71)** `test_terrain_visualize.py::test_framing_modes` camera numbers for `fit`, `top`, `orbit`;
  `test_scene_view.py` unchanged and green, with `frame_all` still excluding terrains (S26), and `set_scene_camera`'s size
  unchanged in `tool_budget.py`.
- **AC58 (R73, R74)** `test_tool_groups.py` (hidden until enabled, `edit_terrain` listed after),
  `test_tool_budget.py` (with no new `exceptions` entry), `test_tool_conventions.py`, `test_tool_coverage.py`, `test_docs.py`,
  `test_autosync.py` pass; `jasper://docs/terrain` read by `test_terrain_ops.py::test_guide_is_served`; `test_agent_docs.py`
  stays green.

**Data**
- **AC59 (R81, R82, R84)** `StreamComponentTest.TerrainNewFieldsRoundTrip`,
  `StreamComponentTest.OldTerrainYamlLoads` (a fixture from before T1 draws the same hash),
  `test_upgrade_from_main.py` green, reflection `catalog_snapshot.json` updated, generated-code test
  `GeneratedScenesTest` green.
- **AC60 (R83)** `TerrainSettingsMigration.OldSettingsLoad`: an old `brushMode`, `paintUseMask` and
  radius.
- **AC61 (R85)** Cook test: Allow Runtime Edits keeps blend CPU readable (S14 `cook-textures
  --check`), `tools/smoke-test.sh` with a terrain scene using `applyOps` runs and exits clean.

**Uncovered corners and seams**
- **AC62 (R28)** `TerrainMapDeltaCmdTest.StructuralStepsCompressAndRestore`: create, resize and
  convert to 16-bit, undo gives equal hashes; `convertHeightMapTo16Bit` values as today.
- **AC63 (R40, R41)** `TerrainOps.ApplyFieldMatchesEquivalentOp`;
  `test_terrain_ops.py::test_two_terrains_share_a_file`: a stroke on one changes both hashes, one GPU
  image.
- **AC64 (R42)** `TerrainEdit.EventsFireOncePerFrameForEveryRoute`: brush, op, Batch, spline bake, Lua
  and undo each publish; `TerrainEdit.ZeroCostWhenUnused` covers no allocation. **Red control:**
  bypass the publish in `applyField`; the test fails.
- **AC65 (R46)** `TerrainEditGpu.StagingRefusalRetries` (AC35) and
  `TerrainEdit.TooLargeForRuntimeIsRefused` with a small `GpuResourceLimits::streamStagingBytes`, and
  the `runtime_edit_map_too_large` issue.
- **AC66 (R76)** `test_terrain_ops.py::test_pain_points`: one assertion per row of the table that an
  MCP call can reach (bounds error, no NaN, `size_clamped`, hash stable over a paint stroke's
  unchanged height map); the rest are named in their own ACs.
- **AC67 (R80)** `test_terrain_lua.py::test_navmesh_goes_stale`: bake, edit in Play, `navmesh_stale`
  issue; `bakeAsync` clears it.
- **AC68 (R86, R87)** `TerrainStore.FakeTiledStore`: a test store with signed texels, `(i * h)`
  lattice, refusals and small blocks passes the same `TerrainOpsKernels.*` and `TerrainGolden.*`
  content; a refused rectangle changes nothing; `TerrainStore.LargeEditUsesBlocks` asserts by
  allocation counter no buffer above block plus margin; `TerrainStore.UndoHookRunsWithTheDelta`: the fake
  store's created tile is removed by undo and back on redo.
- **AC69 (R88)** `TerrainBatch.AbsoluteWritesAndOneUndo`, `TerrainBatch.WeightEncoderRules` (clamp,
  normalize, layer 0 takes the rest), `.PaintByRulesUsesTheSameEncoder`.
- **AC70 (R89)** `TerrainOps.RegisteredOpJoinsValidationResultAndUndo`, through a test op in the unit
  test; `test_terrain_ops.py::test_op_keys_reach_a_registered_op` under `JASPER_MCP_SELFTEST_GROUP`.
- **AC71 (R90)** `TerrainOverlay.RegisteredSourceBakesAndHasLegend` in the editor tests, and a test
  source through `visualize_terrain` under the same self-test switch.
- **AC72 (R91)** `TerrainEdit.NewTerrainDefaultsHaveOneHome`: `create_entity` and the dialog give
  equal defaults.
- **AC73 (R93)** `TerrainSurface.QueryInterfaceAndViewAgree`: virtual, view and `MeshSystem`
  forwarders are equal over 10,000 points; `.ExactAndPresentFlags`.

**Upstream revision**
- **AC74 (R96, R73)** Cases in `tools/mcp/eval/cases.py`, each with `mocks` in the real result shapes and `groups_enabled: ["terrain"]`
  unless it is about finding the group. They need a rebuilt surface and lent accounts:
  `tools/build.sh exec linux uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/eval/dump_surface.py --out build/eval-surface`,
  then `python3 tools/mcp/eval/run.py --appdir <box_lend_accounts appdir> --account <accounts> --surface <surface> --model sonnet --out <evidence>/runs/s53 -k terrain_ --samples 4`.
  **Red:** the same command with `--surface` from the commit before the tool, or `--patch` removing the guide entry; each case below
  must fail there. `python3 tools/mcp/eval/test_grade.py` runs here with no accounts. If the owner lends no accounts, the run is
  reported unverified and the landing says so.

  | Case | Kind | Prompt (paraphrased) | Check |
  |---|---|---|---|
  | `terrain_find_docs` | guard | "How do I edit this terrain? Group off" | enables `terrain` or reads `jasper://docs/terrain`, then calls `edit_terrain` |
  | `terrain_hill_declarative` | bad | "Raise a hill to 80 m in the centre" | `edit_terrain` with `set_height`; must not call `sculpt_terrain` |
  | `terrain_verify_edit` | guard | "Flatten this and check nothing clipped" | `sample_terrain` with `stats` after `edit_terrain`; acts on `next` |
  | `terrain_import_raw` | guard | "Import assets/dem.r16" | `edit_terrain` op `import_heightmap` with `dry_run` first |
  | `terrain_show_slopes` | guard | "Show me where it is too steep" | `visualize_terrain` overlay `slope`, not `screenshot` alone |
  | `terrain_export_raw` | guard | "Export the heightmap as 16-bit raw" | `export_terrain_heightmap`, no `write_file` |
  | `terrain_control_tilemap_name` | control | "Paint the grass terrain on the tile layer" (S50's word) | `edit_tilemap`; must not call `edit_terrain` |
- **AC75 (R95)** Static: every op of Table 1 reaches the handler through the open item schema;
  `test_terrain_ops.py::test_unknown_op_and_key_name_the_valid_ones`; `test_next_names_the_fix` (a clipped map, `next` in
  `structuredContent`). `tool_budget.py --check` shows `edit_terrain`, `visualize_terrain`, `sample_terrain` and
  `export_terrain_heightmap` within 1,500. **Red:** a closed copy of the old schema fails the budget.
- **AC76 (R66, R83)** `TerrainSettingsMigration.PaintMaskKeepsItsMeaning`: a workspace with `paintUseMask: true` and no new keys
  filters Paint Texture and leaves Raise unfiltered; `filterAllTools` extends it; `python3 tools/check_stream_keys.py` passes.
  **Red:** applying `paintUseMask` to every tool fails the test.
- **AC77 (R97)** Second-lander merge check, run by whichever of T1 and agent-authoring lands second: one
  `docs/mcp/guide/terrain.md`, one `Other areas` entry, `test_terrain_details.py` and `test_terrain_ops.py` both green, no terrain
  tool over its limit in `tool_budget.py --check`. `sample_terrain` returns `details`, not `foliage_layers`.
- **AC78 (R98)** `python3 tools/check_raw_writes.py` passes with exactly one new allowlist line (the stroke log), and
  `python3 tools/ci/check-hardcoded-shortcuts.py` passes for Table 5's keys. **Red:** `tools/check_raw_writes.py --self-test`.

### Verification available on this host

8 cores, 15 GB RAM, Docker, `rb.sh L` queue, software GL (llvmpipe), Vulkan on lavapipe, no KVM, no
Windows, no macOS, no iOS, no GPU, no `fixture-unity` (`TERRAIN-LEDGER.md`). There is no CI, so "Elsewhere" means a person
runs the command and reports it; until then the row is unverified. The commands come from the removed CI jobs (`b4f7816ea`) and were not run here.

| Check | Here | Elsewhere: command a person runs |
|---|---|---|
| Unit tests, determinism on x86-64, sanitizers | Yes, `tools/build.sh unit-tests`, `SANITIZE=...` | |
| Hash equality in wasm | Yes, `GATE=terrain-edit tools/build.sh web-gate` | |
| Hash equality on Windows (MSVC) | No | With `vcvars64`: `cmake -S engine -B build-engine-gl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DJASPERENGINE_TESTS=ON && cmake --build build-engine-gl --target jasperengine-core-tests && build-engine-gl/tests/core/jasperengine-core-tests.exe --gtest_filter=Terrain*-*Gpu*` |
| Hash equality on macOS arm64 | No | `cmake -S engine -B build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON && cmake --build build-engine --target jasperengine-core-tests && build-engine/tests/core/jasperengine-core-tests --gtest_filter=Terrain*-*Gpu*` |
| MCP tests, UI tests with xdotool | Yes, `tools/build.sh mcp-tests -k terrain -j 2`, slower frames, wait on state not time | |
| Goldens on glcore and Vulkan | Yes (llvmpipe, lavapipe) | The reference renderers, so they count |
| GLES3 and WebGL2 upload path | Partly: forced format in `TerrainEditGpu`, web gate in headless Chromium | |
| Metal drawing and upload | No: code by careful reading only | The macOS command above with `--gtest_filter=TerrainEditGpu*` (the default Metal device, `docs/testing.md:493`) |
| D3D11 drawing and upload | No: no automated pixel test exists | By hand: `docs/specs/checklists/T1.md` on Windows |
| Android | Build only, no emulator (no KVM) | On a host with `/dev/kvm`: `ANDROID_PROJECT=terrain-edit ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh` |
| iOS | No | On a Mac: `python3 tools/xcode_sync_engine.py --check`, then build and run `terrain-edit` from the Xcode workspace on a device; checklist |
| GPU timings | No meaningful number: only relative, and bytes uploaded are exact | `docs/specs/checklists/T1.md` on real GPUs |
| CPU budgets | Yes, with `RB_SLOTS=2` and nothing else queued; the benchmark run is the enforcement | |
| Pen pressure on real tablets | No: resampler and replay only; `Backend::penPressure` by reading | `docs/specs/checklists/T1.md` |
| Human feel of decal, overlays, dialogs | Screenshots and pixel checks here; feel is a checklist item | Owner on a real display |

## 8. Dependencies & conflicts

**Depends on:** S03, S05, S06 (`parallelFor`), S14 (cook slots), S18 (bake), S24 (`InteractiveEdit`,
`memoryBytes`, groups), S25 (shortcuts), S26 (`frameAll`, S26 R18), S31 (groups, budget), S34 (no
module, size). All landed in `terrain/integration`.

**Blocks:** T2, T3, T4, T5 and the terrain integration of T6. They may spec now against the seams
below.

**Programme landing order** (every spec follows this): T1's foundation first, then T1's rest, T2, T3,
T5 and T6 in parallel, then T4 last. The foundation is the `engine/core/terrain/` and
`engine/core/object/TerrainEdit.h` seams (R1-R16, R40, R42, R86-R89, R92, R93, `applyNewTerrainDefaults`)
and `groups.terrain` in `budget.json` (R74). The rest is migration, undo, strokes, tools, UI, Lua and
runtime edits (R90's `TerrainOverlay` comes with the tools).

**Seams other specs must know**

T2, T3, T4 and T5 were read while this was written; each says T1's names win. Where they assumed
other names:

| Spec | They asked for | Here |
|---|---|---|
| T2 N9, §8 | `TerrainOps::Batch`, `setHeights`, `setLayerWeights`, `setDensity`, `commit`, `store.readHeights` | R88: `terrain::Batch`, and `batch.readHeights` |
| T2 R36, N13 | Moved brushes call `noise::compat`; the second lander deletes `TerrainNoise.h` | R22 |
| T2 R61, §8 | Create dialog lists `TerrainPresets::list()`, calls `TerrainGeneration::start()`; one-line `drawGenerateSection()` | R91 |
| T3 D5, §8 | Hole rectangles, `rasterizeRegion`, R27's result, `isHole` in the query, `ignoreHoles` on brush rays, `terrainLayerChannel`, collider refresh grown by one cell, Macro Normal from dirty rects, Create Terrain's limit on, its overlays | R12, R16, R92, R79, R42, R91, R90 |
| T3 R44, R46, R47 | `visualize_terrain` overlays and `quality`; `edit_terrain` exposes R27; adds to `terrain.md` | R90, R12, R73 |
| T3 R43 | `coverage` on `sample_terrain` | Use `region` (R70) |
| T4 §8, D11, D12 | Signed texels, refusable writes, per-map lattices (`worldOfTexel`, `texelOfWorld`), `preferredBlock`, `beginLarge`, created tiles in undo (`takeUndoHook`), `exact`, `present`, overlay room | R86, R87, R16, R90 |
| T4 R28 | `TiledMapStore` with no caller changes | R2, R86; T4 hooks `querySurface` in one place and provides a `TerrainSurfaceQuery` |
| T4 R31 | Runtime edits are RAM only | R78 |
| T5 R23, R54 | Ops registered from another file; `edit_terrain` hosts them | R89 (keys in the op object) |
| T5 §8 | Read-rectangle from the main thread; subscribable dirty rectangles; overlay source; `region` | `TerrainMapStore::read`, R42, R90, R70 |
| T5 R7 | Query and its copied-texel form | R93 |
| T6 R39, R40, R46 | `cut_holes` and `fill_holes` over rectangles or masks, also at runtime; a query that gives the height under a hole; dirty rectangles | R12, R75, R16, R42; a voxel volume does not use `TerrainMapStore` |

Other facts for them:
- T2 generation returns `changed` and `warnings` in `TerrainOpResult`'s JSON shape and follows `edit_terrain`'s job pattern. If
  `add_noise`'s bytes change, T2 updates `TerrainGolden` hashes in the same commit.
- T3 flips `raycastTerrainSurface` to skip holes and removes `holes_not_rendered`. All shader changes
  are T3's, including any later overlay in the shader.
- T4: per-tile textures replace the whole-image upload unit (DC9); world collision replaces
  `JoltTerrainRefresh`'s single-body lookup.
- **Budget:** T1's foundation adds group `terrain` with R74's limits, 16,000 characters and 10 tools,
  sized for all of T1-T5 (D17, T2 §8, T5 R59). T1 adds 3 tools; T2 2, T3 and T5 none, T4 1, agent-authoring 1.
- **Bits and assets:** T1 adds no variant bit and no asset type.

**Merge hotspots**: `TerrainEditWindow.{h,cpp}`, `WorldTools.cpp` (handlers only),
`MeshSystem.{h,cpp}` and `RenderSystem.cpp` (guarded hooks), `TerrainComponent.h` (T3 also adds),
`StreamComponents.cpp`, `McpTools.cpp`, `McpToolGroups.cpp`, `budget.json`, `test_tool_groups.py`,
`SceneWindow.cpp`, `SceneRender.cpp`, `HierarchyWindow.cpp`, `DefaultCommands.cpp`,
`bench-thresholds.json`, `engine/CMakeLists.txt` (`include(core/terrain/TerrainSources.cmake)`,
flags).

**Land order inside T1** (each buildable, gate per commit `tools/check-changed`, and the full suite before the push when it needs it (R94)): baseline fixture
(DC11); `engine/core/terrain/` with tests; surface query with forwarders (the foundation ends here); brush, generation and bake
migration, delta undo and persistence together with R23; strokes; MCP tools; UI; Lua and runtime last.

**Doriax overlap (D21; none landed yet: whichever lands second merges onto the first)**

| Doriax spec | What T1 touches in common | Rule |
|---|---|---|
| `agent-authoring` | `edit_terrain_details` and `TerrainDetailRules`; `set_terrain_maps` `blendmap.index` and layer factors; `sample_terrain`'s `foliage_layers` renamed `details`, with `drawn`; `docs/mcp/guide/terrain.md`, `jasper://docs/terrain`, the guide's restructure; the Terrain window's load-once settings (`TerrainEditWindow.cpp:3589-3684`); `McpToolGroups.cpp:25` | T1 adds no detail-layer tool or field and no blend-slot parameter. `paint_density` and `erase_density` address `details` indices. T1 extends the guide file if it exists and creates it otherwise, and the second lander merges sections into one file. T1's new settings keys go through their load path, and their fix is not repeated. `sculpt_terrain` stays, because their text names it |
| `render-correctness` | `TerrainFoliageLayer` fields and the foliage ring in `MeshSystem.cpp` | T1 only keeps the whole-layer refill (R23); the ring is T5's and theirs. Same `terrain.md` paragraph: merged |
| `draw-perf` | `updateAllTerrainRenderTextures` (`RenderSystem.cpp:5905-5919`) walks `TerrainComponent` instead of `MeshComponent`; DC10's hook at `:5855` | Hook the terrain's texture update, whichever loop calls it |
| `ssr-probes`, `mesh-memory` | Nothing in T1's code; `MeshComponent` stays out of `TerrainOps` (R1), and `MeshSystemTerrainEdit.cpp` uses indices, not references, across writes to its arrays | No action beyond that rule |

**Pixel art.** S50 uses "terrain" for tile autotiling (`TerrainSolver`, `TerrainTemplates`, `terrain_sets`, `edit_tilemap`'s terrain brush). T1's C++ names are in `jasperengine::terrain`, its commands are `terrain.*` and S50 adds no `edit_terrain`, so nothing collides, but `namespace {}` helpers must not reuse S50's file-scope names in a unity build (`AGENTS.md`, Code), and `terrain_control_tilemap_name` (AC74) keeps agents apart.

## 9. Risks & defaults

- **Precision after stamping on 8-bit maps.** Default: keep the map 8-bit, warn, one-click convert. It
  never upgrades silently because it would touch a file the user may own.
- **Overlay poke-through at far LODs (DC8).** Default: accept with the 25 % copy, and let T3 offer a
  shader mode if a real project needs better. A screenshot in evidence shows the worst case.
- **The 1 s debounce lengthens crash loss slightly.** Default: accept; a stroke's commit is the moment
  users think of as done, and today's encode of a large map already takes seconds. Autosave flushes
  too.
- **`SetHeights` border precision loss** (`HeightFieldShape.h:218`). Default: accept quantization,
  asserted by AC34 within Jolt's error. Over 25 % of the samples changed in one frame rebuilds the
  shape instead, which is also the fallback when a block rect is degenerate.
- **Heightfield with reserved range changes quantization.** It applies only under Allow Runtime Edits,
  so other terrains are byte-identical in collision.
- **R35's threshold may feel wrong.** It is one constant; default as stated, tuned once by feel on a
  real display (checklist), with tests pinned to the constant.
- **Linux tablet support through XInput2** may be more than a single implementer can verify. Default:
  if unavailable in the current backend, return -1 on Linux and record it under Deviations. Replay
  still covers the logic.
- **`TerrainErosion` moved into the engine links into players.** Default: dead-stripped unless
  `applyOps` references it; since `applyOps` dispatches `erode`, it counts against R54. If over
  budget, `erode` becomes editor-only by a registered kernel, and R75 already permits refusing it in
  Lua with a clear error.
- **Memory of a stream height image at 2049² on GL is 16.8 MB, double.** It only exists with Allow
  Runtime Edits or while editing, as stated in Terrain Settings.
- **Job snapshots copy whole maps.** Default: one memcpy on the main thread at job start, at most 33
  MB, within a frame's stall tolerance for an agent-initiated call.
- **Create Terrain changes what the menu row does.** `create_entity` and scripts are unchanged;
  `test_undo_ui.py` and other tests use `create_entity`. A human who wants the old empty terrain
  presses Create with Flat. The dialog remembers the last preset.
- **Land order of T1 and T3.** T1's foundation lands first (§8), so R92's headers and `holeMap` exist
  when T3 starts. The warning in R12 lasts only until T3's consumers land.
- **`docs/specs/IMPLEMENTING.md` is stale on branches and servers** (`oe-port`, A/B/C). Default: the
  ledger's build queue and branches apply: worktree under `.worktrees/`, branch
  `terrain/s53-authoring`, `rb.sh L`.
- **`sculpt_terrain` stays beside `edit_terrain`.** It is rate-based and D15 would not add it today. Default: keep it, because AC13
  proves the migration through it and agent-authoring's text names it. Retiring it would give back 2,699 characters and is the
  owner's decision after T1 lands.

## Deviations

Recorded while landing the foundation (§8). Names here are what the code has; the sibling specs cite
the sketch's names, which all still exist unless a line below says otherwise.

- **D-1 `TerrainMapStore` has three more virtuals, none abstract.** R12 needs the last fill to remove
  the map and the sketch had only `createMap`, so `canRemoveMap` and `removeMap` default to "this store
  keeps its maps". T3 R29's `holeMapHolds` is `canRemoveMap` returning false. `canCreateMap` (default
  true) lets validation refuse a density map for a foliage layer that does not exist, so a dry run
  tells the truth. A store written to the sketch, as T4 R28 lists it, compiles unchanged.
- **D-2 `TerrainEditEvents` has an overload per scene.** The handler is the sketch's, but entity
  numbers repeat across scenes, so `subscribe(handler)` alone would hand a system another scene's
  terrain. `subscribe(scene, handler)` is what a subscriber living in a scene (T5's re-snap, T6's
  regeneration) should use. Also public: `publish`, `hasPending`, `flush(scene)`, `forgetScene`.
- **D-3 The event is published by the store, not by each route.** Publishing in `TerrainComponentStore`
  covers every route at once, including ones not written yet. AC64's red control is therefore "skip
  the publish in the store", not "bypass it in `applyField`".
- **D-4 The frame's flush is guarded by a process-wide flag, not `terrain.editState`.** DC10 names
  `editState`, which arrives with node refit and the rest of T1 (R23). The guard costs one load per
  scene update. The rest of T1 may fold it into `editState`.
- **D-5 `TerrainEdit.ZeroCostWhenUnused` covers what exists.** Today it counts the frame's guard and a
  query. R53's 1000-frame count over an unedited terrain and the `sizeof` assertion land with
  `editState`, which is what they would measure.
- **D-6 Erosion belongs to its block.** R87 says both routes give the same bytes. They do for every
  kernel but `erode`, whose droplets are drawn per block, so its bytes depend on `preferredBlock`.
  `TerrainGolden` compares `erode` across stores only with the map as one block. Full-map erosion is
  T2's, and T2 R24 has its own worker-count rule.
- **D-7 Straight on the store quantizes between ops.** With a `beginLarge` token there is no overlay to
  hold float working values, so a list, a stroke's stamps and a smooth's iterations round at each
  application. One application gives the overlay's bytes (`TerrainStore.StraightOnTheStoreGivesTheOverlaysBytes`).
- **D-8 `remap_layers` always uses an overlay of its own,** even straight on a store: a channel moved
  early must not be read as the source of a later one. Its memory is therefore the map's tiles, not a
  block. Authoring op on a single terrain; T4 should refuse it on a world or remap per tile.
- **D-9 `EditorTerrainStore` is not in the foundation.** R3 lists it, but it is the component store plus
  persistence and sharing (R29, R41) and lives in `editor/terrain/`, outside what §8 calls the foundation.
- **D-10 `rasterizeRegion` has two more defaulted parameters,** a `Filter*` (R12: filters apply) and a
  `HoleRegionResult*` for R27's numbers. T3 D10's call compiles as written. `collision_samples_open`
  and `hole_no_collision` wait for T3's collider rule, as R12 says.
- **D-11 `registerOp` returns `bool`,** false for one of Table 1's names. `registeredOpExists` and
  `unregisterOp` are added for callers and tests. `OpValidateFn` and `OpApplyFn` take an `OpError` and
  an `OpContext`. `Op::params` holds a registered op's own keys as `OpParam`, a plain value tree, since this
  folder may not include a JSON library (R1); the handler copies them there from the op object (R89, R95).
- **D-12 `Op`'s fields beyond the sketch.** `existingObjects` (spacing for `place_objects`, candidates
  for `erase_objects`) because the plan is pure; `erodeSeconds` and `droplets` so a brush can carry its
  fraction; `baseHeight`, `frequency` and `fill` for `fill_noise`. `OpOptions` gains `dryRun`, `inPlace`
  (a stroke keeps its overlay's floats between stamps) and `newMapResolution`. A stroke's `raise` or
  `lower` without `delta` deposits as a brush does, in map units (R34).
- **D-13 `TerrainErosion` takes a window.** `ErosionField` is a window into a map rather than the whole
  map's vector, so a stamp needs no whole-map buffer; the weight is a plain callback with a template
  convenience (R22 allowed either). The Terrain window passes the whole map as its window until the
  brush migrates.
- **D-14 The private height decoders are renamed, not yet gone by use.** `TerrainEditWindow` calls
  `terrain::heightTexelAt` and `terrain::sampleHeightTexels`; AC12's names are gone. Its stroke code
  still reads through them until R17 and R22 remove it.
- **D-15 `holeMap` is `Hidden` in reflection.** It is cut and filled by ops and saved by its codec, so
  no Inspector row and no change to `inspector_layouts.json`. T3 may show it.
- **D-16 Parts of the foundation's requirements that need later work.** R1's "every write goes through"
  and R13's window use are the migration. AC8's `test_holes_warn_until_s43` and AC9's
  `test_delete_layer_keeps_paint` need `edit_terrain` and the UI. AC64's brush, spline bake, Lua and undo
  routes, AC68's `TerrainMapDeltaCmd` half and AC72's dialog half exist once those do.
- **D-17 `import_heightmap` takes decoded heights.** The engine's op holds the normalized values, their size and
  nothing about files: finding and decoding a file is `TerrainRawIO` and the handler (R61, R68), which come with the
  rest of T1. A size other than the map's is refused here, naming the structural path the handler takes (R1, R61).
- **D-18 `MeshTriangles::isGeometryLoading` keeps reading `needUpdateTerrain`.** R23 names it among the readers
  of the revision, but it asks whether geometry is still being built, which a refit never makes true. Its callers
  (physics shapes, placement worlds) build from the surface when they build. `SplineSystem` and foliage read the
  revision.
- **D-19 Foliage is flagged by the revision, not compared to it.** `flushTerrainEdit` raises `needUpdateFoliage` when
  it advances `surfaceRevision`: same rate, and the existing refill path is the one consumer.
- **D-20 `Shape` gains `squareEnds` and `nearestTexel`; a Polyline samples a mask when it has pixels.** R17 needs a
  ramp under a square or masked brush, and today's fallback to the nearest texel on every brush shape, not only round
  ones. `forEachSample`'s forced texel now takes what the shape gives at its centre, so a sub-texel masked brush
  weighs by the mask's centre. Unset, nothing changes for any op.
- **D-21 `TerrainMapRef` gains `HoleMap`,** so `EditorTerrainStore::createMap` can name a hole map's file by R32.
  `TerrainTextureEditCmd` moved out of `TerrainEditWindow.cpp` to `editor/command/type/` with the map helpers it
  needs moved to `TerrainMapUtils`: the store, the commands and AC62's test all need them without a window.
- **D-22 The first stroke on a map the terrain does not own stays one structural step.** R32's copy to a
  `terrain_edit_*` happens when the stroke opens, and that stroke undoes through `TerrainTextureEditCmd`, whose
  snapshots compress (R28), as it did before. Later strokes are deltas. The spline bake still writes the map it finds
  (R32's copy is the brush's), as it did: a bake has no first-edit step to join yet, and `edit_terrain` should decide
  this for both when it lands.
- **D-23 `erase_objects`' brush keeps its test on the window's side.** It is a containment test over entities with
  nothing to compute a plan from; R21's plan is `place_objects`.
- **D-24 Mouse placement stamps come from `StrokeResampler` with no time.** R21 asks for one attempt per spaced stamp,
  and holding still adds no attempts (R35's held stamps are a deposit's, not a scatter's). Scripted placement keeps one
  attempt per point: those points are its stamps, so `sculpt_terrain` counts stay what they were.
- **D-25 The allocation counter reaches `jasperengine-editor-tests`,** with bytes and the largest allocation added.
  AC15 and AC20 need it for code that lives in the editor.
- **D-26 AC13's byte tolerance is one step per stroke.** R17's 1/255 holds for a stamp, and `test_terrain_baseline.py`'s blend and
  density maps are written by two strokes, each rounding (R11) where the unmigrated editor truncated. The second stroke starts up to a
  step away and can end two off, so that test allows two steps on at most 5 % of the values (measured: at most 3.4 %, on density), and it prints the share. The fixture was
  not recorded again, and heights keep their 1/65535.

- **D-27 The collider refresh tests are in the core suite.** AC34 names `editor/tests/engine/physics/`; `TerrainCollisionRefresh*`
  needs a `PhysicsSystem` and no editor, so it is `engine/tests/core/physics/TerrainCollisionRefreshTest.cpp`.
- **D-30 `edit_terrain` refuses `place_objects`, `erase_objects`, and `import_heightmap`'s `set_max_height` and `resolution`.** The first
  two place GameObjects, which `sculpt_terrain` does; the error names it. An import of another size is the Terrain window's path
  (Import Raw... makes the new map first).
- **D-32 Window work not done yet:** R39's stroke log, R56's alternates (the `C` key and Alt Alternates While Painting),
  R57's `TerrainBrush` shortcut context with Table 5's keys and the Cut Holes brush they name, R63's Pick beyond Set Height's
  eyedropper, R55's fade and hide during camera moves, R59's Resize..., R61's hillshade preview, R67's Preview, R37's pen
  readers, and R47-R54's benches. They are open, not given up: none of them is impossible on this host.
- **D-35 `visualize_terrain` without T3's and T5's keys and with one sheet.** `quality` and `foliage_quality` are theirs to add. The sheet is
  one mesh of at most 255 x 255 cells; DC8's second copy at 25 % in the tools layer is not done.
- **D-36 The Lua terrain functions that take tables are raw functions** with their stubs in `tools/lua_api/parser.py`'s `OVERRIDES`;
  `docs/scripting/*.md` joined the Lua doc test so `docs/scripting/terrain.md` is checked.
- **D-37 `navmesh_stale` comes from the tool.** `TerrainIssues` sees one component, so `sample_terrain` adds the issue from
  `NavigationSystem::countDirtyTiles` of the scene's NavMesh Surfaces.
