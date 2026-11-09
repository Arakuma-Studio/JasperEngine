# T5 — Foliage upgrade

Terrain programme, step 5 of 6 (`TERRAIN-LEDGER.md`; decisions D1-D21 there are binding and are
cited as "ledger D*n*"). Reports: `terrain3d-shaders-instancer.md` §4,
`voxel-meshing-generators.md` §5, `our-terrain-runtime.md` §3. Decisions of this spec are F1-F16 in
§6, so they don't collide with the ledger's numbers.

## 1. Goal

A Terrain carries the two foliage kinds Unity users expect. **Details** are grass and flowers:
hundreds of thousands of 32-byte instances, placed on jobs, culled one by one on the CPU into a
stream buffer, thinned with distance and swayed by shader wind. **Trees** are individually stored,
painted, chopped and collided instances, with mesh LODs, a baked impostor as their last LOD and
per-cell culling. Both run on all five backends without compute, indirect draws or base instance;
both have budgets that hold on mobile and web; both can be driven by a person with a brush and by
an agent with a deterministic `scatter_by_rules`. Scenes made before this spec look the same.

## 2. Scope

**In scope**
- Placement on jobs, with time-slicing at zero workers, a main-thread budget and bit-identical
  output on every target; one mesh load per layer instead of one per chunk.
- Details: compact instances, per-instance culling, density falloff, wind, Unity's detail fields.
- Trees: prototypes, the tree store and its file, cells, per-cell LOD and culling, impostor bake and
  draw, shadow control per LOD, Jolt capsules, wind.
- Terrain Settings (Detail Distance, Detail Density, Tree Distance, Billboard Start, Fade Length,
  Max Mesh Trees) and a Foliage Quality tier per platform group.
- Foliage operations on T1's engine, including `scatter_by_rules`; re-snap after sculpting; partial
  rebuilds.
- Runtime removal, Lua API, hit events, navigation obstacles, picking.
- Paint Trees and Paint Details panels, Mass Place Trees, stats, density overlay, impostor button.
- `edit_terrain` foliage ops with `bake_impostors`, `sample_terrain` foliage statistics, the benchmark scene and budgets.

**Out of scope** (default decisions, not open questions)
- **GPU grass, GPU-driven culling, compute, SSBOs, bindless, indirect draws, `base_instance`.** GPU
  grass and GPU-driven culling are on the do-not-copy list (`docs/specs/README.md:23-24`).
- Terrain3D's particle placement path (`instancer.md:6,203-216`): the same list.
- Occlusion culling. Frustum and distance only.
- Skinned or animated trees, destruction into pieces and falling-tree physics: a script gets the
  removed tree's pose and spawns what it wants (R31).
- Foliage on Voxel Volumes (T6). T6 may feed the same scatter later through `FoliageRegion`; this
  spec only keeps that seam open.
- Paging tiles in and out, and per-tile file management. T4 does that through the seam in F11
  (ledger D8). T5 ships and is tested with one region per Terrain.
- Octahedral impostors. The eight-view cylindrical impostor is decided in F7.
- SpeedTree import and tree creation tools.

## 3. Current state

Citations were checked against `terrain/integration` at `b4f7816ea`. **MS** =
`engine/core/subsystem/MeshSystem.cpp`, **RS** = `RenderSystem.cpp` beside it, **TEW** =
`editor/window/TerrainEditWindow.cpp`, **TC** = `engine/core/component/TerrainComponent.h`, **IMC** =
`engine/core/component/InstancedMeshComponent.h`. Everything here is read from code, except what is
marked *inferred*; nothing was built or run for this spec.

**Details today (density-map scatter).**
- `TerrainFoliageLayer` (TC:141-157) has mesh, density map, density per square unit, scale range,
  yaw jitter, normal alignment, slope and height ranges, draw distance, seed and `castShadows`. No
  wind, no LOD settings, no collision. `foliageLayers` is at TC:172.
- Placement is a pure function of cell, seed and maps, with 8-unit cells and at most 8192 instances
  per chunk (MS:3765-3766, 3803-3820, 3996-4062), so a port can reproduce it exactly. It is not
  bit-stable across targets: the slope test uses `std::acos` (MS:4036), yaw builds a `Quaternion`
  through libm (MS:4050), and the tilt uses `Quaternion::slerp` (MS:4056). No file in this path is
  compiled with `-ffp-contract=off` (*inferred* from the list at `engine/CMakeLists.txt:1261-2151`).
- **It all runs on the main thread**: `updateTerrainFoliage` from `MeshSystem::update` (MS:7184-7190),
  which fills chunks inline (MS:4214-4220). A height or density change sets `needUpdateFoliage`, and
  `refillAll` then re-places every chunk in the ring (MS:4131).
- **One entity per chunk.** A sliding ring (MS:3797-3801) of entities, each with `MeshComponent`,
  `ModelComponent` and `InstancedMeshComponent` (MS:3823-3836). `loadFoliageMesh` passes
  `asyncLoad = false` (MS:3932-3939), so the first chunk parses the file on the main thread. The
  comment at MS:4134 says each chunk parses for itself; more exactly, later chunks take the parse
  from `ModelPool` (MS:5130-5137) and GPU buffers from `MeshBufferPool` (MS:2110), but each one still
  rebuilds its mesh component and materials, at most two per frame (MS:4135).
- **Instances are heavy.** `InstanceData` (IMC:69-85) is expanded to `InstanceRenderData` (IMC:87-91),
  a matrix, colour and rect: 96 bytes.
- **Culling is per instance, on the main thread, with a static scratch list**
  (RS:8295-8345, 8301), and any camera move dirties every instanced mesh in the scene
  (RS:8215-8218, 8238-8241). Non-primary cameras draw everything (IMC:153).
- **The renderer already draws by offset.** `bindInstances` swaps the instance buffer with a byte
  offset (RS:8403-8405; `SokolObject.cpp:531`) and draws each LOD bucket of a slice (RS:8415-8433).
  The colour, depth and G-buffer passes all go through `drawSubmeshGeometry` (RS:8973, 9141, 9951).
  Uploads go once per view, and a miss of the stream budget retries next frame (RS:8546-8564).
- Fade shrinks instances towards their base on XZ distance (`mesh.vert:221-231`, IMC:162-167).

**Trees today.** There is no tree data. Paint Trees (TEW:3268-3269, 3479-3512) places models as
child GameObjects, or as `InstanceData` in one instanced child "host" per asset
(`TerrainInstancePlaceCmd`, TEW:1155-1233). Consequences (*inferred*): 50,000 trees are 50,000 records
in the scene YAML, in one draw list culled per instance on the main thread; there are no impostors,
no capsules and no per-LOD shadow control; a sculpted terrain leaves them floating, as nothing
re-snaps them. Erase is Ctrl (TEW:3741-3749), not Unity's Shift.

**Wind.** No built-in foliage wind exists. `library:foliage` has it per material, with a height or
vertex-colour mask, in both colour and depth programs (`engine/shaderlib/library/foliage.glsl:10-56`,
`foliage.vert`, `foliage_depth.vert`). `ForceFieldComponent` says it is "the wind source other systems
may read" (`ForceFieldComponent.h:11-12`), and nothing but particles reads it (*inferred*, grep).

**Consumers that see the chunk entities.**
- Navigation collects any instanced `MeshComponent` entity (`NavBakeCollector.cpp:589-609`) and
  does not exclude foliage chunks, so a bake includes the grass that happens to be near the editor
  camera (*inferred: untested bug*; R34 fixes it).
- Picking gives each instance a leaf (`SceneBvh.cpp:702-703`) and hits it as `PickKind::INSTANCE`
  (`MeshRaycast.cpp:418-422`). A chunk maps to its Terrain through `getFoliageOwner`
  (`ProjectUtils.cpp:544`, `MS:3881`).
- `PlacementWorld.cpp:1557-1561`, `EntityCloner.cpp:1389-1396` and `Project.cpp:3758-3765` (the
  shader collector) use `getFoliageEntities`.
- `TerrainSplineBake.cpp:457-470` clears density along a spline (`SplineComponent.h:41-42`).

**Other constraints read for this spec.**
- `SplineSystem` is the job pattern: below `INLINE_BUDGET_MS` inline, otherwise Background jobs,
  debounced while dragging (`SplineSystem.h:88-93`; `SplineSystem.cpp:1239-1251, 1666-1673`).
- Single-thread mode runs nothing at `submit`; the Engine pumps Background work for 4 ms a frame (S06
  R18, `S06-jobs-memory.md:250-253`).
- `sg_update_buffer` is once per frame per buffer and has no offset (`sokol_gfx.h:293-300`).
  `base_instance` is unavailable on GLES3 (`sokol_gfx.h:236-238`).
- Vulkan stream staging is 16 MB per frame on mobile and web (`Engine.h:278`).
- `MAX_MESH_LODS` is 4 (`Engine.h:94`); generated LODs come from `MeshLodPool` (`MeshLodPool.h:26-29`).
- Shader variants: MESH uses bits to 35 (`ShaderPool.cpp:737`) of 64; fallbacks never drop a bit that
  changes a uniform block (`ShaderPool.h:75-78`).
- Scene format is version 7 (`StreamMigrations.cpp:704`). Terrain has three serializers that must
  change together: `StreamComponents.cpp:535-611`, `RuntimeFields.gen.inc:519-538`,
  `ComponentHooks.cpp:152-158`; plus `Factory.cpp:1703-1722` and `RenderReflect.cpp:125`.
- The `terrain` MCP group has no budget entry, so 15,000 characters and 12 tools apply
  (`tools/mcp/budget.json`), and ledger D17 says to raise them there.

## 4. Reference

OpenEngine is not available on this host (its path listed nothing), and nothing is needed from it: its
grass is on the do-not-copy list. The upstream clones are data; no code is copied from them (ledger
D14), so T5 adds no `NOTICE.md` entry.

| Source (`/tmp/terrain-research/`) | What to take | What to do better |
|---|---|---|
| `Terrain3D/src/src/terrain_3d_instancer.h:25` | The 32-unit tree cell | Our cells also carry their LOD choice and their capsule body |
| `Terrain3D/src/src/terrain_3d_instancer.h:37-44` | Nothing: one MultiMesh per cell, mesh and LOD duplicates instance data across LODs | One buffer per cell shared by all its LODs (F5) |
| `Terrain3D/src/src/terrain_3d_instancer.cpp:230-235`, `terrain_3d_mesh_asset.h:66-67` | The idea of a last shadow LOD and a shadow impostor | A per-LOD mask (R17) |
| `Terrain3D/src/doc/docs/instancer.md:99-101` | The reason it has no per-instance cull | Details cull per instance (R9) |
| `Terrain3D/src/src/terrain_3d_assets.h:25` | A limit on prototypes | 64 per Terrain (R13): a mobile budget, not a data limit |
| `godot_voxel/src/terrain/instancing/voxel_instance_generator.cpp:985-990` | Seeding by block hash plus layer | Our existing hash stays (F2); output is hash-tested |
| `godot_voxel/src/streams/instance_data.h:26-34` | Quantized rotations | 16-bit, not 8-bit: tilted trees show 8-bit steps (*inferred*) |
| `godot_voxel/src/doc/source/instancing.md:126-130` | Stable IDs are what make saves work | Our IDs are never reused (R22), and need no SQLite |

Both licences allow reading for ideas (Terrain3D: MIT, `src/LICENSE.txt`). Godot code is never
lifted (ledger D14).

Inside the repository, follow `SplineSystem` for jobs, `bake_navigation` (`NavigationTools.cpp:178,
826-838`) for the job tool, `visualize_navigation` (`:930-945`) for look-and-verify,
`SplineGpuTest.cpp` for goldens, `engine/tests/core/bench/SplineBench.cpp` for the benchmark and
`tools/test-projects/terrain` with `tools/web/gates/terrain.json` for the web gate.

## 5. Requirements

### Functional: placement

- **R1. Pure scatter.** `engine/core/terrain/FoliageScatter.{h,cpp}` (ledger D1) places one cell from
  a self-contained input: seed, cell coordinates, a copy of the height, density and hole texels it
  needs, and the layer's fields. No ECS, no render types, no globals.
- **R2. Deterministic output.** The same input gives the same `FoliageInstance` bytes on Windows,
  Linux, macOS, Android, iOS and HTML5, whatever the worker count, frame timing or camera path that
  brought the cell in. `foliageHash()` hashes instances in canonical order (layer, cell Z, cell X,
  attempt), and the expected value is a constant in the test. Rules: no libm in the kernel
  (deterministic sine, cosine and arccosine from F2), no `-ffast-math`, `-ffp-contract=off` on the
  file. `std::sqrt` is allowed: IEEE exact.
- **R3. Jobs.** Each cell that enters the ring is a `jobs::submit` on `Lane::Background`
  (`"foliage-detail-cell"`). The main thread copies the cell's texels, submits nearest cells first,
  and polls `Task::isReady`. It does not use `postToMain` (`MainThreadQueue` doesn't drain while
  paused, `MainThreadQueue.h:16-18`). Without a job system (`!jobs::hasSystem()`) a cell runs inline, as
  in `SplineSystem`. In-flight jobs per Terrain are capped: 32 desktop, 8 mobile, 4 web.
- **R4. Zero workers.** With 0 workers (`JE_JOB_WORKERS=0`, single-thread web) nothing blocks.
  Placement runs from the pump's Background budget, one cell per job so the pump can stop between
  cells, and completes over later frames. The result equals the threaded result (R2).
  `hasPendingFoliageUpdates()` stays true until done, so a screenshot still waits for loaded content.
- **R5. Main-thread budget.** Per Terrain per frame, on the main thread, excluding the view cull
  (R41): at most 0.5 ms for texel copies, submits and applies. Applying a cell moves a buffer pointer;
  it doesn't copy instances. Over budget, work waits for the next frame.
- **R6. Shared mesh.** Each detail layer and each tree prototype LOD has one engine-owned
  **prototype entity** with the mesh. Loads pass `asyncLoad = true`. A 900-cell ring makes one mesh
  load, not 900. The prototype entity is unsaved, outside the hierarchy (the existing foliage
  precedent), and has an empty `instances` list. `MeshSystem`'s public foliage functions stay as
  one-line forwards to `FoliageSystem` (ledger D2), so `getFoliageOwner`, `getFoliageEntities`, `getFoliageInstanceCount` (agent-authoring),
  `setFoliagePreviewEntity`, `hasPendingFoliageUpdates` and `reloadFoliageMesh` keep their callers.
- **R7. Surface query.** Placement, re-snap and queries use T1's surface query (ledger D5, D9) for
  height, normal and is-hole. The copied-texel form for jobs is T1's `SurfaceView` (T1 R93), with
  a test that it matches `querySurface` within 1e-4 (S26:467). The hole test is T3's pure
  `terrain::isHole(HoleMapView, u, v)` at the instance origin, after the density test and before height
  (T3 R19, R26).

### Functional: Details

- **R8. Compact instance.** `FoliageInstance` is exactly 32 bytes (F3), one for Details and Trees.
- **R9. Per-instance cull into a stream.** Per view, for each visible cell of a layer, instances pass
  into one stream vertex buffer shared by all layers of that view (F4). Tests by frustum, XZ distance,
  density rank; cells entirely inside the frustum skip the plane tests. Result per layer: an offset
  and four LOD bucket counts (`InstanceViewRange`), drawn through vertex-buffer offsets. Each layer's
  draws are `submeshes × non-empty LOD buckets`. A still camera re-culls nothing and uploads nothing.
  Views: main camera, each shadow slot of a casting layer, each render view; render-to-texture
  cameras have their own list up to `MAX_TERRAIN_VIEWS`, past which they reuse the main camera's
  (the same rule as TC:8-12).
- **R10. Density.** Each instance has a rank byte from its hash. An instance draws if
  `(rank + 1) / 256 ≤ detailDensity × layerDensityScale × falloff(distance)` (at 1, all draw), so changing density or distance
  shows a stable subset, with no re-placement. `falloff` goes from 1 at `densityFalloffStart` (a share
  of the draw distance) to `farDensity` at the draw distance. Shrink fade from 75% of the draw distance
  is applied by the cull to the instance's scale, so it matches today's fade (`mesh.vert:221-231`).
- **R11. Wind for Details.** A layer with `windBend > 0` draws a wind variant (F8). Colour, depth and
  G-buffer programs use the same function, so shadows sway with the plant. Bend 0 selects the variant
  without wind: no GPU cost.
- **R12. Unity's detail fields.** New on the layer, with defaults that change nothing:
  `minHeightRatio`, `maxHeightRatio` (Min/Max Height over the existing Width scale), `healthyColor`,
  `dryColor`, `noiseSpread`. The colour noise is hash-based and deterministic.

### Functional: Trees

- **R13. Tree prototypes.** `TerrainComponent::treePrototypes` holds up to 64 prototypes, each with a
  stable `id`, `meshPath`, optional explicit `lodMeshes` (up to 3) and `lodDistances` (when empty,
  LODs come from `MeshLodPool`'s error metric, RS:8222-8234), `bendFactor`, `windMask`, `shadowLodMask`,
  `collision` (None, Capsule), `collisionRadius`, `collisionHeight`, `navObstacle` and the impostor
  fields (R16).
- **R14. Cells.** Trees are grouped in 32-unit cells of terrain-local space. A cell within Tree Distance
  plus one cell holds a CPU array of `FoliageInstance`, sorted by prototype. A cell whose mesh band is
  in range also has one GPU vertex buffer of that array, with one sub-range per prototype, bound by
  offset. Creating or updating these buffers is capped per frame (16 desktop, 4 mobile and web). Farther
  cells cost only their stored records.
- **R15. Per-cell culling and LOD.** Each view tests cell AABBs, nearest first. For each visible cell
  and prototype, the system selects bands from the cell's nearest and farthest points: mesh LOD *n*,
  impostor, or both in the fade band. All instances of the cell in that band draw with that LOD. Mesh
  bands draw from the cell's static buffer, one draw per cell, prototype and submesh. The impostor band
  copies whole visible cells into the view's stream, for one draw per prototype (F5).
  **Fade Length** is a per-instance dither between the mesh and impostor, computed from instance
  distance in the vertex shader (F6), so per-cell selection shows no square ring. `Max Mesh Trees`
  applies by distance: past it, a cell draws impostors only. Without a baked impostor, a tree fades out
  at Tree Distance with its lowest LOD.
- **R16. Impostor draw.** The last LOD of a prototype with a baked impostor is one quad per instance,
  facing the camera around Y, drawn by `library:impostor` (new): it picks the view from the camera
  direction and instance yaw, and lights an albedo and a normal atlas through the normal lit path
  (F7). Prototype fields: `impostorAlbedo`, `impostorNormal`, `impostorViews`, `impostorBoundsCenter`,
  `impostorSize`, `impostorSourceHash`.
- **R17. Shadow control per LOD.** `shadowLodMask`: bit *n* is mesh LOD *n*, bit 7 the impostor;
  default all LODs on, impostor off. A band whose bit is clear casts nothing. Shadow passes draw the
  cell's bands, with no dither.
- **R18. Tree collision.** A prototype with `collision = Capsule` makes, while the scene simulates,
  one static Jolt body, on an engine-owned entity, per cell with trees of such prototypes: a `StaticCompoundShape` built on a job,
  one capsule per tree, each with `SetUserData(slot)`, so `colliderIndex` in contacts and rays
  (`Contact3D.h:38-39`) identifies the tree. No prototype with collision: no body, no Jolt work.
  Under `#ifdef JASPERENGINE_PHYSICS_3D`; a Terrain with capsules makes `ModuleUsage` require
  `physics3d`.
- **R19. Tree wind.** The same function as R11, with `bendFactor`: whole-tree sway growing with
  height squared, plus leaf flutter masked by height or vertex colour red (`windMask`). The impostor
  sways only by its whole-tree term.

### Functional: settings and budgets

- **R20. Terrain Settings.** New fields with these names in the Inspector, a Foliage section of the Terrain
  window's Terrain Settings tool (T1 R59, R91), Lua and MCP:

  | Name | Field | Default for a new Terrain | Default for an old scene |
  |---|---|---|---|
  | Detail Distance | `detailDistance` | 80 | 0 (each layer's Draw Distance only) |
  | Detail Density | `detailDensity` | 1 | 1 |
  | Tree Distance | `treeDistance` | 1000 | 1000 |
  | Billboard Start | `billboardStart` | 80 | 80 |
  | Fade Length | `fadeLength` | 5 | 5 |
  | Max Mesh Trees | `maxMeshTrees` | 200 | 200 |
  | Wind Direction, Speed, Size, Bending | `windDirection`, `windSpeed`, `windSize`, `windBending` | +Z, 1, 8, 0.5 | same; no layer bends |

  A layer's effective draw distance is `min(drawDistance, detailDistance)`, ignoring zero, times Foliage Distance (R66). A Directional
  Force Field with an Infinite shape (the first enabled one) overrides the wind direction and
  multiplies the strength with its pulse, as Unity's Wind Zone does; it is read once per frame and only
  if some layer or prototype bends.
- **R21. Foliage Quality.** `Project Settings > Quality > Foliage Quality (Desktop / Mobile / Web)`,
  values Low, Medium, High, as Atmosphere Quality (`Engine.h:617-627`, `ProjectSettingsWindow.cpp:1974`),
  and `Engine.setFoliageQuality`. New projects: High, Low, Low. A project without the field: High on
  all three, so it looks the same (F12). The tier scales and caps; it never writes the Terrain's values.
  **Distance is not in the tier:** `render-correctness`'s Foliage Distance (per platform group, `Engine.foliageDistanceScale`) is the one global
  distance multiplier, on Detail Distance, Tree Distance and Billboard Start (R66). New project templates write Foliage Distance 0.5 on Mobile and Web; old
  projects keep 1, as that spec decides.

  | | Low | Medium | High |
  |---|---|---|---|
  | Detail Density × | 0.5 | 0.75 | 1 |
  | Fade Length × | 0.5 | 1 | 1 |
  | Max visible detail instances per view | 30,000 | 120,000 | 400,000 |
  | Max Mesh Trees cap | 60 | 200 | 500 |
  | Max visible trees per view | 8,000 | 15,000 | 30,000 |
  | Max foliage draws, main view | 150 | 400 | 1,200 |
  | Detail layer shadows | off | as authored | as authored |
  | Impostor | nearest view | two views blended | two views blended |

  A cap drops the farthest cells first, deterministically for a given camera, and sets
  `budget_exceeded` in the stats.

### Functional: tree data and authoring

- **R22. Tree store.** Trees live in a binary `.jetrees` file referenced by `TerrainComponent::treeData`
  (F9), loaded on a Background job through `AssetIOService`. Each record has a stable `uint32` id that
  is never reused, even after removal, and a normalized position, so resizing a Terrain moves trees
  with its maps as Unity's does. The file has its own magic, version and hash. A truncated or corrupt
  file logs an error naming the file, draws no trees, and is not overwritten by a later save without
  explicit consent, following S03's refusal of lossy saves.
- **R23. Terrain operations.** Tree and density writes go through T1's operation engine (ledger D4).
  T5 registers through `registerOp` (T1 R89) in new file `engine/core/terrain/FoliageOps.{h,cpp}`: `scatter_by_rules`,
  `clear_foliage`, `place_trees`, `remove_trees`, `convert_placed_to_trees`. Density writes go through
  `TerrainMapStore`; tree writes go through `TreeStore`. Results report changed rectangles, counts
  and rejections. Brush strokes call the same code.
- **R24. `scatter_by_rules`.** Fills a target, a detail layer's density map or tree prototypes with
  weights, by rules: `slope`, `height`, surface `layer` weights, a `noise_mask` (T2 Noise Graph,
  threshold), `exclude.holes` (T3; on by default), `exclude.splines` with a margin, and `min_spacing`
  for trees. A `seed` makes the result deterministic. `mode` is `replace`, `add` or `remove`. It is
  all-or-nothing: invalid input changes nothing. `dry_run` reports the counts a real run produces,
  and the two match exactly. For trees, cells run in canonical order, and spacing checks only earlier
  trees and neighbouring cells. Output is hash-tested as in R2.
- **R25. Re-snap.** When T1's `TerrainEditEvents` (T1 R42) report a dirty height rectangle (brush, Lua, spline bake, generation):
  trees in touched cells recompute height, and normal alignment if they have it, from their
  `yOffset`; detail cells in the rectangle are placed again. A tree over a hole is hidden, not
  deleted, and comes back when the hole is filled. A drag is debounced at 120 ms as in S18. Re-snap is
  idempotent, so undoing the sculpt gives the original bytes.
- **R26. Partial rebuild.** A write of any kind touches only its cells' records, GPU buffers and
  bodies, and `needUpdateFoliage` stays only as the whole-Terrain fallback for a new heightmap or
  changed layer settings. `FoliageStats` counts cells rebuilt, so tests can assert it.
- **R27. Existing detail layers.** `foliageLayers` keeps its name, keys and semantics: a layer made
  before T5 places the same positions and scales from the same seed (F2), and new fields default to
  no change. **No conversion runs on load**, nothing is moved or re-typed, so there is no version bump or migration (`AGENTS.md`, Invariants).
- **R28. Existing placed trees.** Placed GameObjects and instanced hosts stay what they are, and draw
  as they do today. `Convert Placed Instances to Trees` in the panel and `convert_placed_to_trees` move
  a host's instances into the store, as one undo step; GameObjects with components stay untouched.
- **R29. Spline zones.** `SplineTerrainSettings` gets `clearTrees` (default off) next to
  `clearFoliage`: the bake removes trees in the footprint through `remove_trees` (one small hook at
  `TerrainSplineBake.cpp:457`).

### Functional: interaction

- **R30. Runtime removal.** `removeTreeInstance` hides the instance in its cell's buffer, rebuilds its
  cell's compound shape in the same frame, and fires `onTreeRemoved`. `addTreeInstance` and
  `restoreTreeInstance` work the same way. Runtime changes are an overlay on the store; the store and
  its file stay untouched (F10).
- **R31. Persistence.** The overlay lasts as long as the scene is loaded: reloading the scene or
  stopping Play in the editor drops it. Nothing is saved automatically. `getRuntimeFoliageState()`
  returns a compact string (removed ids, runtime additions, cleared-detail discs) that a game stores
  wherever it keeps saves, and `setRuntimeFoliageState()` restores it. `removeTreeInstance` returns
  the pose, so a script can spawn a falling prefab or a stump.
- **R32. Lua API.** As §6 F14, in a new binding file; `FoliageQuality` and the Terrain fields of R20
  included. Documented in `docs/scripting/foliage.md` with doctested blocks.
- **R33. Hit events.** `onTreeHit` fires on the main thread after the fixed step, when a body touches
  a capsule, with the tree id, prototype, point, normal, other body and relative speed, one event per
  body and tree per contact start. With no subscriber it adds no work. `getTreeFromHit` maps a
  `Physics` ray hit, a contact or a `MeshRaycast` hit to a tree id.
- **R34. Navigation.** A new `Collector::collectTrees` adds a cylinder (`NavBakeCollector.cpp:152`) per
  visible tree with `navObstacle`, under every Use Geometry, following the Terrain entity's
  `NavMeshModifier` policy. Detail layers and prototype entities are never collected, which removes
  the camera-dependent input of §3. `removeTreeInstance(id, {updateNavigation = true})` calls S19 R10's
  `updateTiles` for its bounds on each surface; the default is false.
- **R35. Picking.** Foliage has no BVH leaves. `MeshRaycast` asks `FoliageSystem` by a cell walk
  along the ray: bounding spheres, then LOD0 triangles through the existing pick cache. New kinds:
  `PickKind::TREE`, in `PICK_ALL`, and `PickKind::DETAIL`, opt-in, on resident cells only. A hit has
  `entity` = the Terrain, `foliageId`, and `prototype`. In the Scene view a click on a tree selects the
  Terrain (as today for chunks); `pick_at` reports the tree. Grass doesn't block default rays.

### Platform and backends

- **R36. All five backends and six targets.** Foliage draws the same on GL 3.3, GLES3/WebGL2, Metal,
  D3D11 and Vulkan. Only vertex formats the engine already maps are used (FLOAT4, SHORT4N, UBYTE4N:
  `SokolObject.cpp:216-248`). No `gl_InstanceID` dependency, no integer attributes.
- **R37. Variants reach exports.** New variant bits (`Cin`, `Wnd`, `Dth`: appended at landing, T3 D2's table) are collected by the export
  collector through the prototype entities, so a lite player with no runtime compiler draws foliage.
  collector through the prototype entities, so a lite player with no runtime compiler draws foliage.
  They aren't in the fallback lists, since they change a uniform block or inputs. A material whose
  custom vertex program lacks a compact variant draws through a 96-byte expanded stream for that layer
  (F4), so it looks the same at higher cost.
- **R38. Disabled costs nothing.** A scene with no Terrain, or a Terrain with no layer and no prototype,
  has no pass, draw, buffer, allocation or job; `FoliageSystem::update` is one empty-array check. No
  wind, fade or compact bit is set on any non-foliage material. Existing non-foliage goldens are
  unchanged. A Terrain disabled, or on an inactive GameObject, has no draws or jobs (today its chunks
  go on drawing, *inferred*).
- **R39. Core, not a module.** Foliage stays in core (it already is, and `foliageLayers` needs it);
  ledger D18's detection is used only for R18's physics. Its size is budgeted in R43.

### Performance

Device classes are S20's (`S20-particles.md:250-253`): **Mid mobile** = Snapdragon 778G or A14, 1080p, 4
workers; **Web** = Chrome stable, wasm-simd, single thread. **Host** = this repository's reference
Linux x64 Release build, idle, with `JE_JOB_WORKERS` as stated.

- **R40. Counts, on the benchmark scene (R44), every frame of its path.**

  | Metric | Desktop (High) | Mobile (Low) | Web (Low) |
  |---|---|---|---|
  | Visible detail instances, main view | ≤ 400,000 | ≤ 30,000 | ≤ 30,000 |
  | Visible trees, main view | ≤ 30,000 | ≤ 8,000 | ≤ 8,000 |
  | Mesh trees | ≤ 500 | ≤ 60 | ≤ 60 |
  | Foliage draws, main view | ≤ 1,200 | ≤ 150 | ≤ 150 |
  | Foliage stream upload per frame, all views | ≤ 24 MB | ≤ 4 MB | ≤ 4 MB |
  | Upload with a still camera | 0 | 0 | 0 |

- **R41. CPU per frame, moving camera, on the benchmark scene.** Main-thread wall time for all foliage:
  visible-cell tests, cull, cell draw lists, plus R5.

  | | Desktop | Mid mobile | Web, 0 workers |
  |---|---|---|---|
  | Foliage main thread | ≤ 1.5 ms | ≤ 2.0 ms | ≤ 3.0 ms |
  | Placement job, one full-density cell (1,280 attempts) | ≤ 0.35 ms | ≤ 1.0 ms | ≤ 1.0 ms |

  Measured on host: cull of 100,000 visible instances ≤ 3.0 ms with 0 workers and ≤ 1.2 ms with 4;
  one attempt ≤ 250 ns; a still frame ≤ 50 µs; empty scene ≤ 1 µs mean with zero allocations.
- **R42. Memory, on the benchmark scene, excluding meshes and impostor textures.** Mobile and web ≤ 24 MB
  for CPU and GPU together; desktop ≤ 128 MB. Stored tree: 40 bytes; a tree within Tree Distance: 32
  more on the CPU; a tree in a mesh-band cell: 32 more on the GPU. Each is registered in `MemoryStats` (`foliage-cpu`, `foliage-gpu`).
- **R43. Size.** The web player grows by at most 64 KiB gzipped with T5's code, at a measured
  before and after (`tools/build.sh web`, the S34 size method). More needs a recorded deviation and,
  by ledger D18, the coordinator's view on a module.
- **R44. Benchmark scene.** `FoliageBenchScene` (made by code in `engine/tests/core/bench/`, with no
  assets): 1024-unit Terrain; 1024² 16-bit hash-fractal height; 512² density maps; 4 detail layers at
  4, 2, 1 and 0.5 per square unit; 3 tree prototypes (trunk and leaves) with 50,000 trees in total; 600
  frames on a fixed path at 5 units per second, turning 30° per second; Detail Distance 120, Tree Distance
  1000, Billboard Start 80, before Foliage Distance. `je-bench-foliage` gates its
  numbers through `tools/perf/bench-thresholds.json`. Measured results go in `docs/perf/foliage.md`,
  along with the same detail-only scene on the commit before T5 as the before-picture.
- **R45. Editing.** A brush stamp adding 8 trees costs ≤ 2 ms total on the main thread, host. Mass
  placing 50,000 trees runs as a job and leaves no frame over 8 ms on the host from it. Adding 100 trees
  to one cell of the benchmark scene rebuilds at most its 9 neighbouring cells' data.

### Editor UX (human)

Paint Trees and Paint Details keep their tool icons (TEW:3262-3269); T1 R59 adds a fifth, Terrain Settings. New code is in
`editor/window/TerrainFoliagePanel.{h,cpp}`; TEW only calls it (ledger D2). T1's brush grammar and
decal apply, and these bindings are what T5 adds for these two tools.

- **R46. Paint Trees.**
  - A thumbnail grid of prototypes (`drawAssetThumbnail`, TEW:2473); drop a model on it or use Add.
    Select one to paint; right-click for Edit, Remove, Bake Impostor. Removing asks if instances exist,
    and it's one undo step.
  - Rows: Brush Size, Tree Density, Tree Height with a random range, Lock Width to Height, Tree Width,
    Random Tree Rotation, Color Variation, Normal Alignment, Place As (Tree or GameObject; Prefabs
    are GameObject only; a new key `placeAs`, default Tree, while stored `placeInstanced` keeps its meaning for GameObject), Spacing.
  - **Shift erases** every prototype, **Ctrl+Shift** erases only the selected one, Ctrl still erases
    as today (TEW:3741-3749), and **Alt+click** with no drag selects the prototype under the cursor (T1 R56's
    drag threshold; Alt+drag orbits, as in every tool).
  - Selected prototype foldout: LODs, Wind, Shadows (a per-LOD checkbox row), Collision with Fit from
    Mesh and a capsule gizmo in the Scene view, Nav Obstacle, Impostor (R49).
- **R47. Paint Details.** A thumbnail grid of layers, replacing the combo at TEW:3370. All existing rows
  (TEW:3413-3476) stay, renamed to Unity's where they differ, in the UI only (Draw range, R66; Min/Max Width, Min/Max Height, Healthy
  Color, Dry Color, Noise Spread, Bend Factor). New layers: `castShadows` off, `windBend` 0.5,
  `densityFalloffStart` 0.5, `farDensity` 0.3. Brush rows: Brush Size, Opacity, Target Strength. Shift
  erases; Ctrl keeps erasing as today.
- **R48. Mass Place Trees.** A dialog: Number of Trees, Keep Existing Trees, Seed, prototype weights,
  slope and height ranges and Exclude Holes. It runs `scatter_by_rules` as a job with a progress bar
  and Cancel, as one undo step named "Mass Place Trees (Island)". A Preview Count button runs `dry_run`.
- **R49. Impostor bake button.** Bake Impostor (or Bake All) with Views (4, 8, 16) and Resolution (128,
  256, 512), a preview of the atlas, and a state: None, Baked, Out of Date (`impostorSourceHash` no
  longer matches the mesh file). It runs as a job with progress; the scene stays usable. Field changes
  are one undo step; the files stay on disk.
- **R50. Stats.** A Stats foldout on both tools, updated at 2 Hz: instance count (total, resident,
  visible), draws, cells, estimated memory (CPU and GPU, per the R42 records), pending cells, with an
  Estimate For combo (Desktop, Mobile, Web) that shows the counts for that tier's scales against R40's
  budgets, in warning colours when over.
- **R51. Scene overlay.** A Show Density toggle draws the selected layer's density as a heat map on the
  terrain, and Paint Trees shows tree density per cell with T1's overlay mechanism. It adds no pass
  when off.
- **R52. Undo.** Every edit is undoable, named after S24: "Paint Trees (Island)", "Erase Trees (Island)",
  "Paint Details (Island)", "Scatter Foliage (Island)". Tree commands store deltas of changed records, not
  the file; `memoryBytes` reports them; a stroke merges by stroke id, as `TerrainObjectStrokeCmd`
  does (TEW:1315-1366), and Esc cancels a stroke (S24 R12).
- **R53. Convert and warnings.** The panel offers R28's conversion when the Terrain has an instanced
  host. It warns when a prototype has no impostor but is drawn past Billboard Start, when a prototype's
  mesh has no geometry on its root, and when a tree count exceeds the Mobile budgets.

### Agent UX (MCP, CLI)

All in the `terrain` group (ledger D17): T5 adds no tool. `editor/mcp/tools/FoliageTools.cpp` holds the op handlers, with hooks in T1's
tools. Guidance goes into `docs/mcp/guide/terrain.md`, read through `jasper://docs/terrain`, and the
agent guide isn't grown.

- **R54. `edit_terrain` operations.** T5's operations (R23) follow ledger D15: `dry_run`, seeds,
  absolute values, all-or-nothing validation, changed rectangles and statistics in the result, one undo
  step per call. Results: `instances_added`, `instances_removed`, `rejected` by rule (slope, height,
  layer, noise, hole, spline, spacing), `changed_rects`, `hash`. Sketch in F15.
- **R55. `sample_terrain` statistics.** A `foliage: true` parameter on `sample_terrain`, with T1's
  `region`, returns `foliage`: `details` per layer (mesh, instances in region, exact up to 4,096 cells, else `estimated: true`, mean density), `trees`
  per prototype (mesh, count, impostor state), and totals (resident, visible, cells, draws, stream and buffer bytes, CPU and GPU bytes, last-frame main-thread
  ms, pending cells, `budget_exceeded`, placement `hash`). `info.details[i].drawn` stays agent-authoring's field and reads `FoliageStats` (R65). No `get_foliage_stats` tool:
  conventions prefer a parameter on an existing tool (`docs/mcp/conventions.md`, checklist item 1). Allowed while playing, so runtime removals are visible.
- **R56. `bake_impostors`.** An `edit_terrain` op (T1 R89, with `spansFrames`), registered by `editor/util/ImpostorBaker`, not a tool of its own (T1 R95):
  `{op, prototypes?, views?, resolution?}`, with the call's `wait` and `timeout_seconds`. Result entry per prototype: atlas paths,
  bounds, source hash, ms. One undo step; refused in Play as `edit_terrain` is; `dry_run` reports sizes without rendering. Hidden until `tool_groups` enables `terrain`, as
  `edit_terrain` is. Lua's `applyOps` does not know it: it is registered in the editor only.
- **R57. `visualize_terrain`.** T1's tool gets overlay values `density` (with `layer`) and `tree_density`,
  and takes a `foliage_quality` for the capture only. The impostor can be verified by screenshot:
  set `billboardStart` through `set_properties`, frame, and capture.
- **R58. Prototypes and settings.** `set_properties` reaches `treePrototypes[n].*`, `foliageLayers[n].*`
  and R20's fields through reflection, with agent-authoring's `checkDetailLayer` guard on `foliageLayers`. No new tool.
- **R59. Budget.** The group stays within `tools/mcp/budget.json`: T5 adds no tool and at most 300
  characters to `sample_terrain`'s schema, already in T1 §6's count. The group's entry and limits are T1 R74's (16,000 characters, 10 tools), added
  by T1's foundation, sized for all of T1-T5 (ledger D17; T2 §8). `docs/mcp/TOOLS.md` is regenerated.

### Data and compatibility

- **R60. Format.** No scene version bump (S03 R22; as T1 R81, T3 R50, T4 R54 and T6 R80): all new keys are
  optional, and a missing key loads as F12's default. There is no migration. All serializers change together: `StreamComponents.cpp`,
  `RuntimeFields.gen.inc` (regenerated with `jasperengine-editor reflect --emit-runtime-fields` and committed), `ComponentHooks.cpp`, the terrain codec in
  `engine/core/serialization/codecs/` and its editor encoder, `Factory.cpp`, `RenderReflect.cpp`,
  `editor/tests/reflect/catalog_snapshot.json`.
- **R61. Asset plumbing.**
  - S04: `.jetrees` is `AssetType::TreeData = 30` (after upstream's ProceduralMaterial took 29; stored in `assetmap.bin`), name `tree_data`, a row in `editor/asset/AssetTypes` (sidecar, assets base).
  - Maps folder: files are `terrain_edit_trees_<scene>_<entity>_<serial>.jetrees`, so the orphan collector
    (TEW:160, 525-575) handles them; the active-file list gets the tree path; written with
    `writeFileAtomic` and reported through S05's self-write suppression; flushed when maps flush.
  - S29: `*.jetrees` joins the binary LFS patterns (`S29-version-control.md:303-304`).
  - S30: the file ships as a pack entry, read with `FileData::newFile`. S14: impostor atlases are normal
    textures, cooked with alpha coverage kept.
- **R62. Copies.** Duplicating a Terrain or instantiating a prefab with one shares the file; the first
  write gives the copy its own file, as painted maps do (`isOwnedEditableTexturePath`, TEW:163, 654). Runtime
  scene clones share the immutable store plus their own overlay.
- **R63. Docs.** `docs/scripting/foliage.md`, `docs/perf/foliage.md`, `docs/mcp/guide/terrain.md` (Foliage
  section), `docs/specs/checklists/T5.md`, a row in `docs/testing.md`.

### Upstream revision: no CI, lean MCP, Doriax, writes

- **R64. Where each AC runs.** There is no CI (`AGENTS.md`, Test): the gate per commit is `tools/check-changed`, and the full suite runs before the push. Each AC runs on this host or
  is labelled not verifiable here with the command a person runs (§7's table). A row not run is reported unverified.
- **R65. Details build on `agent-authoring` (D21; not landed: whichever lands second merges onto the first).** It owns `edit_terrain_details`, `TerrainDetailRules`, `density_map {fill}`,
  the `details` key with `drawn` and `MeshSystem::getFoliageInstanceCount`. T5 adds no near-duplicate: creating, updating and removing detail layers stays that tool; T5's
  new layer fields (`min_height_ratio`, `max_height_ratio`, `healthy_color`, `dry_color`, `noise_spread`, `wind_bend`, `density_falloff_start`, `far_density`) join its `detailRanges()` table with validated
  ranges, and so reach that tool, `set_properties`' guard and the load clamp. `scatter_by_rules` with a `detail_layer` target fills an existing layer's density map by rules; it creates no layer. If T5 lands
  first, these fields go in through `set_properties` and agent-authoring's table adds them when it lands; if it lands second, the forward and `drawn` move to `FoliageStats`. Names stay consistent:
  **Details** in tools and docs, `foliageLayers` in data, and Detail Distance, Detail Density, Tree Distance, Billboard Start as R20.
- **R66. Ring and distance come from `render-correctness` (D21; whichever lands second merges).** T5's move and rewrite absorb its fixes rather than losing them:
  the ring side is fixed from the draw distance (`ceil(2 * distance / cell) + 1`), never from the view span, so walking fills only cells that enter; a slot outside the span stays as it is, and is cleared only on a whole refill;
  `TerrainFoliageLayer::minDrawDistance` and its near fade are that spec's field and formula (`nearFadeStart = minDistance * TERRAIN_FOLIAGE_FADE_START`), applied by R9's cull to the instance scale, and a cell whose farthest corner is
  nearer than the start is skipped. Foliage Distance and `Engine.foliageDistanceScale` multiply effective Draw, Min Draw, Tree and Billboard Start distances (not in preview), 0 removes foliage, and T5 adds no other global distance knob. Its instanced-batch
  slack stays on non-foliage instanced meshes; foliage's equivalent is R9. If `render-correctness` lands second, its §6.6 and §6.8 engine edits go into `FoliageSystem`; if first, F1's move takes them, and AC6's verbatim copy is of the code after it.
- **R67. Eval cases and names.** T5 adds no tool, but its ops, `foliage` key and guide section need cases that fail without them (AC41). Op and key names follow the lean rules: no agent guide line, hints in `next` (a tree count past the Mobile budget, an
  out-of-date impostor), and the handler validates. The tool names it uses keep the gated `verb_noun` form (`docs/mcp/conventions.md`), not `AGENTS.md`'s "area noun first".
- **R68. Writes, assets, shaders.** `.jetrees` writes through `writeFileAtomic` (R61). The baker encodes PNGs to memory (`stbi_write_png_to_func`) and writes them through `writeFileAtomic`, because `stbi_write_png` to a path fails `tools/check_raw_writes.py`. So
  `tools/raw_writes_allowlist.txt` gets no line. Nothing is vendored: `NOTICE.md` and `engine/notices.json` do not change. Each commit that changes a shader (`mesh.vert` and siblings, `foliage_instance.glsl`, `foliage_wind.glsl`, `library:impostor`) bumps the shader cache version in `editor/App.cpp` by one over its `main`.

## 6. Design constraints & decisions

**F1. Where it lives.** Pure code goes in `engine/core/terrain/` (ledger D1): `FoliageTypes.h`,
`FoliageScatter`, `FoliageCull`, `TreeStore`, `FoliageRules`, `FoliageOps`. Then
`engine/core/subsystem/FoliageSystem.{h,cpp}`, registered right after `MeshSystem` (`Scene.cpp:59-60`);
`RenderSystemFoliage.cpp`, following `RenderSystemShadows.cpp`; `FoliagePhysics.cpp` under the physics
define; `engine/core/script/binding/FoliageLua.cpp`; `engine/shaderlib/includes/foliage_instance.glsl`,
`includes/foliage_wind.glsl`, `library/impostor.*`; `editor/util/ImpostorBaker.{h,cpp}`;
`editor/command/type/TreeEditCmd.h`.

Hooks in hotspots (ledger D2), and nothing more:
- `MeshSystem.cpp`: the foliage functions (MS:3763-4298, except `sampleTerrainSurface`) move to
  `FoliageSystem`; the loop at MS:7177-7190 goes; six one-line forwards stay.
- `RenderSystem.cpp`: one call in `buildInstanceViews`, one in `uploadInstanceViews`, and one branch at
  the top of `drawSubmeshGeometry` (`if (instmesh && instmesh->foliage) { drawFoliageSubmesh(...); return; }`).
  The three passes share it (RS:8973, 9141, 9951).
- `TerrainEditWindow.cpp`: calls into the panel. `WorldTools.cpp`: one parameter on `sample_terrain`.
  `InspectorWindow.cpp`: nothing beyond what reflection gives.

**F2. Same placement, stable bits.** `hashFoliage`, `mixFoliage`, `randomFoliage`, `foliageCellAttempts`
and the attempt order move verbatim to `FoliageScatter` (MS:3791-3820, 3996-4062). Positions and scales keep
their arithmetic, so they equal today's on x64 exactly. Only these change: `std::acos` becomes a compare
of `normal.y` against a deterministic cosine of the slope limit; yaw's sine and cosine, and the tilt's
half-angle, become polynomial kernels in `FoliageScatter.cpp`, accurate to 1e-5 rad. The cell seed is
`(seed, cellX + regionOffsetX, cellZ + regionOffsetZ, i)` with offset 0 for one Terrain. Rank, wind phase and
colour noise use extra `mixFoliage` draws after the existing five, so they cannot disturb them.

**F3. The instance.**

```cpp
namespace jasperengine::terrain {
    // one foliage instance on the GPU; stored and drawn in terrain-local space
    struct FoliageInstance {
        float position[3];
        float scale;            // the uniform (width) scale
        int16_t rotation[4];    // snorm quaternion
        uint8_t color[4];       // unorm linear tint; white is no tint
        uint8_t heightRatio;    // height scale = value / 128 (128 is exactly 1)
        uint8_t windPhase;      // from the hash
        uint8_t windBend;       // 0..255, the layer's or prototype's
        uint8_t densityRank;    // R10's stable subset
    };
    static_assert(sizeof(FoliageInstance) == 32);

    // the stored tree, 40 bytes
    struct TreeRecord {
        uint32_t id;
        float u, v;             // 0..1 over the Terrain
        float height;           // cached surface, 0..1 of maxHeight
        float yOffset;          // world units above the surface
        float scale;
        int16_t rotation[4];
        uint16_t prototypeId;
        uint8_t heightRatio;
        uint8_t flags;          // snap to terrain, align to normal, hidden by hole
        uint8_t color[4];
    };
    static_assert(sizeof(TreeRecord) == 40);
}
```

In the shader, `foliage_instance.glsl` under `HAS_COMPACT_INSTANCE` declares `i_packed0..3` as inputs (new
`AttributeType` entries) and makes `i_matrix_col1..4` and `i_color` file-scope variables, filled by
`compactInstanceSetup()` as the first line of `main`. So `mesh.vert`, `depth.vert`, `gbuffer.vert`,
`surface_vs.glsl`, `surface_depth_vs.glsl` and `motion.glsl` keep their code; each gets one `#ifndef`
around its inputs and one call. `library:foliage` then works on compact instances with no change.

**F4. Detail views.** For each view, in order: test cells on the main thread and reserve per-cell write
ranges by prefix sum; `jobs::parallelFor` writes the survivors into their ranges with the main thread
helping; compact sequentially; count-sort into LOD buckets; upload once. Output is independent of scheduling.
With zero workers it's inline, bounded by the caps (R21, R41). Opaque layers go nearest cell first;
blended ones farthest first. The stream is one `STREAM` buffer per view, grown with headroom and never
shrunk, using the retry path (RS:8564). The 96-byte expansion for R37 runs in the same job into a
second stream, only when a layer needs it. Mesh AABB and `shadowCasterRevision` of the prototype entity
update when residency changes, so pass culling and S10's shadow cache see the change (RS:13323).

**F5. Mesh bands use one buffer per cell; the impostor band uses the stream.** `sg_update_buffer` has no
offset and runs once a frame per buffer (`sokol_gfx.h:293-300`), so with one buffer per prototype or
Terrain a chop would re-upload megabytes. A cell of 100 trees re-uploads 3.2 KB. Near cells are few, so
their draws are affordable and a moving camera uploads nothing for them. Far cells are many: at 32 units
a 500-unit Tree Distance has about 150 visible cells, which with 3 prototypes is 450 draws and past
mobile's whole budget. Copying their instances costs at most 256 KB a frame on Low (8,000 × 32 bytes) for
one draw per prototype. Cells stay the unit of culling and LOD. Rejected: all trees through the stream
(wasted upload for the near band, and a chop still has to reach it) and per-cell mesh entities (the
cost this spec removes).

**F6. Fade by dither.** Bit `Dth` (on MESH and GBUFFER; its DEPTH bit is for depth passes that must match the colour's discard, and shadow draws never set it, R17) discards by a screen-space pattern against a
factor computed per instance from distance in the vertex shader. Mesh fade: 1 before
`billboardStart − fadeLength`, 0 at `billboardStart`. Impostor fade: the complement, and fading out over
`fadeLength` before Tree Distance. Blended alternatives need sorting; a shrink looks wrong at tree size. Cells
in the band draw twice, which R21's draw caps include.

**F7. Impostor.** Eight views around Y by default, in a grid atlas (1024×512 at 256 a view), with two
RGBA8 textures: albedo with alpha; normal with AO. The baker renders the prototype alone in an
isolated preview scene (`editor/render/preview/ModelRender.h`) with an orthographic fitted camera,
one view per frame on the main thread, taking the G-buffer's base colour and normal
(`gbuffer.frag:3-6`) through `FrameReadback::readFramebuffer` (`FrameReadback.h:31`), dilating colour past
the alpha edge, and writing PNGs on a job through `writeFileAtomic` (R68). The output is deterministic on one renderer. Edit time only:
no player code bakes. `library:impostor` picks the view with a polynomial arctangent in the vertex stage,
costs two fragment textures on Low and four on Medium and High. Octahedral is rejected: it costs more
texture memory and a three-view blend on phones, and trees are mostly seen from the side.

**F8. Wind.** Bit `Wnd`, block `u_vs_foliage` (fade range, eye, wind vector, phase scale, gust, flutter,
height range), set per prototype draw. The function lives in `includes/foliage_wind.glsl`, after
`library/foliage.glsl:36-56`: same sway, gust and flutter formula so a person who learned one sees the
same result. A material already on `library:foliage` keeps its own wind, and `Wnd` is not set for it, so it
doesn't sway twice. `frame.time` is the clock; goldens pin it, as `SplineGpuTest` does.

**F9. Storage.** One binary file per Terrain, not scene YAML: 50,000 trees are 2 MB as records, and a
scene file should stay diffable.

```
offset  size  field
0       4     magic "JETR"
4       4     version = 1
8       4     record count
12      4     next id
16      4     payload bytes
20      4     flags (bit 0: zstd)
24      8     xxhash64 of the uncompressed records
32      ...   TreeRecord[count], little-endian, sorted by id; zstd above 64 KiB
```

Cells are derived at load on a job. Unknown versions refuse to load with a named error. A density-map tree
workflow (the brief's "may also be used for trees") is `scatter_by_rules` with `density_map`
as a mask: the output is individual trees, so there is one tree path at runtime.

**F10. Runtime overlay.** A per-scene `TreeRuntimeState`: a bitset of removed ids by cell, runtime additions
with ids from `0x80000000`, and up to 64 cleared-detail discs. Cell buffers and compound shapes are rebuilt
from store plus overlay. Rejected: writing back to the file (a chop would alter the project in the editor)
and automatic save slots (the engine has no save-game system to attach to).

**F11. The seam for T4.** `FoliageSystem` works on regions, and a lone Terrain is one region:

```cpp
namespace jasperengine::terrain {
    using TreeId = uint64_t;    // (uint64_t(regionKey) << 32) | record id; regionKey 0 for one Terrain

    class FoliageRegion {
    public:
        virtual ~FoliageRegion() = default;
        virtual uint32_t regionKey() const = 0;
        virtual TerrainMapStore& maps() = 0;            // T1; density, height, holes (ledger D4)
        virtual const TerrainSurfaceQuery& surface() const = 0;   // T1 (ledger D5)
        virtual std::shared_ptr<TreeStore> trees() = 0; // may be null while loading
        virtual void cellOffset(int& x, int& z) const = 0;  // whole 8-unit cells from the world origin
        virtual Matrix4 localToWorld() const = 0;
    };
}
// FoliageSystem
void addRegion(Entity owner, std::shared_ptr<terrain::FoliageRegion> region);
void removeRegion(Entity owner, uint32_t regionKey);    // budgeted release over frames
```

T4 adds one region per resident tile and removes it on unload, with per-tile `.jetrees` files. A tile
boundary inside a cell splits it: each side uses the same hash and rejects its outside (as MS:4019), so the
union is what one map would give. Collision bodies, cell buffers and the overlay are per region, so
paging releases them; a game's saved overlay is keyed by `TreeId`, so it survives a tile unloading.
T5 ships `TerrainFoliageRegion` only.

**F12. Old content defaults.** `detailDistance = 0` means no Terrain-wide limit; `applyNewTerrainDefaults`
(T1 R91) sets 80. New layer fields default to no effect: white colours, ratios of 1, `windBend` 0,
`densityFalloffStart` 1, `farDensity` 1. A project without the field is High on all platforms, and that High
has no instance or draw cap. So an old mobile project keeps its density, and the panel shows the budget
warning instead.

**F13. Views.** Foliage culls per view and shares nothing between views except residency, because
light views differ. Residency follows the main camera only. Detail cells outside the main camera's ring
draw in no view, as chunks do today.

**F14. Lua sketch.** All indices are 0-based, as S18 Deviation 1.

```lua
local terrain = Terrain(scene, entity)

terrain.detailDistance = 60
terrain.detailDensity = 0.5
terrain:setDetailDensity(0, 0.25)     -- one layer's own multiplier
terrain.treeDistance, terrain.billboardStart, terrain.fadeLength, terrain.maxMeshTrees = 400, 60, 4, 100
terrain.windDirection, terrain.windSpeed = Vector3(1, 0, 0), 2
Engine.setFoliageQuality(FoliageQuality.LOW)

-- nearest first; kind "trees" (default), "details" or "all"; max defaults to 256
local near = terrain:getFoliageInstancesInRadius(player.position, 6, {kind = "trees"})
-- {id, kind, prototype, position, rotation, scale}; details have layer instead of prototype, and id 0

local pose = terrain:removeTreeInstance(near[1].id, {updateNavigation = true})  -- nil if unknown
local stump = GameObject.instantiate(stumpPrefab); stump.position = pose.position
terrain:restoreTreeInstance(near[1].id)
local id = terrain:addTreeInstance({prototype = 0, position = Vector3(3, 0, 4), yaw = 90, scale = 1.2})
terrain:clearDetailsInRadius(player.position, 3)        -- optional third argument: layer
terrain:restoreDetails()

local saved = terrain:getRuntimeFoliageState()          -- a string
terrain:setRuntimeFoliageState(saved)                   -- false if it is not valid

terrain.onTreeHit = function(hit) print(hit.id, hit.prototype, hit.point, hit.normal, hit.body, hit.speed) end
terrain.onTreeRemoved = function(id, pose) end

local r = MeshRaycast.raycast(scene, ray)               -- r.kind == MeshRaycast.TREE
local treeId = terrain:getTreeFromHit(r)                -- also a Physics ray result or a contact
```

**F15. MCP sketch.**

```json
{"tool": "edit_terrain", "ops": [{"op": "scatter_by_rules",
  "shape": {"type": "rect", "min": [-200, -200], "max": [200, 200]},
  "seed": 42,
  "target": {"trees": [{"prototype": "Pine", "weight": 3}, {"prototype": "Birch", "weight": 1}]},
  "density": 0.02, "mode": "replace",
  "rules": {"slope": [0, 30], "height": [10, 140],
            "layer": [{"index": 1, "min": 0.5, "max": 1}],
            "noise_mask": {"graph": "noise/forest.noisegraph", "output": "mask", "threshold": 0.45},
            "density_map": {"layer": 2},
            "exclude": {"holes": true, "splines": ["Road"], "spline_margin": 4},
            "min_spacing": 3},
  "jitter": {"scale": [0.8, 1.3], "height_ratio": [0.9, 1.2], "yaw": 1, "align_to_normal": 0}}],
 "dry_run": true}
```

An op's own keys are flat in the op object, which the listed schema leaves open and the handler closes (T1 R89); `shape`, `seed` and `dry_run` are T1's, and rule ranges use
the units of T1 R6 (degrees; world units on the MCP). `target` may instead be `{"detail_layer": 0}`, and then
`density` is in the layer's instances per
square unit, written to its density map as an absolute value, with no rate arithmetic. Other ops, with a region as T1's `shape`:
`clear_foliage {target, spline?, margin?}`, `place_trees {prototype, points: [[x, z, yaw?, scale?]]}`
returning ids, `remove_trees {ids?, prototype?}`, `convert_placed_to_trees {host}`, `bake_impostors {prototypes?, views?, resolution?}`.

`sample_terrain {…, region?, foliage: true}`.

**F16. Accepted behaviour changes**, each documented in `docs/upgrading-from-main.md`:
1. grass and detail meshes stop entering navmesh bakes (§3's bug);
2. `MeshRaycast` no longer hits detail instances by default, and its hit `entity` is the Terrain, not a
   hidden chunk;
3. Paint Trees places Trees by default in new strokes (Place As keeps GameObjects);
4. a disabled Terrain hides its foliage. `sculpt_terrain`'s `place_object` keeps today's result, so
   existing agent scripts and tests keep working.

## 7. Acceptance criteria

Backends: **gl** glcore on llvmpipe, **vk** Vulkan on lavapipe, **gles** the `linux-backends` image with
`SMOKE_BACKEND=gles3`, **web** headless Chromium WebGL2 through `tools/build.sh web-gate`, **CPU**
the Linux x64 CPU suites here (Windows and macOS: the commands below, unverified until run), **metal** the macOS command below (the default Metal device, `docs/testing.md:492-493`), unverified until run. "Red" is the
red control: what to break, to see the named test fail, recorded in the commit or PR.

| AC | Covers | Test | Runs on | Red control |
|---|---|---|---|---|
| AC1 | R1, R2 | `FoliageScatter.GoldenHashOnEveryTarget`, `.GoldenHashWithSeedChanges` (pinned hash of a fixture of 4 layers, 16-bit heights, both tilted and upright) | CPU, web gate `GATE=foliage` prints and compares the same constant | Compile the kernel with `std::sin`, or remove `-ffp-contract=off`, and run on an FMA target; change one `mixFoliage` constant locally |
| AC2 | R2, R4 | `FoliageSystemCore.SameHashWithZeroOneAndFourWorkers`, `.SameHashForAnyCameraPath` | CPU | Let the cell order depend on job completion |
| AC3 | R3, R5 | `FoliageSystemCore.NoCellPlacedOnTheMainThreadWithWorkers`, `.ApplyStaysWithinBudget` (job names counted through S06's profiler hooks) | gl host | Call the scatter inline in `update` |
| AC4 | R4 | `GATE=foliage` in `tools/web/gates/foliage.json`: single-thread; camera jump; p99 frame foliage work ≤ 4 ms; all cells complete; hash equal | web | Run placement synchronously in `update` |
| AC5 | R6 | `FoliageSystemCore.OneMeshLoadPerLayer` (a 900-cell ring makes one mesh load and one prototype entity; `MeshBufferPool::getBufferCount` constant) | gl | Create one entity per cell |
| AC6 | R27, F2 | `FoliageScatter.LegacyParity` (a verbatim test copy of today's `appendFoliageCell`: same count, position and scale bytes on x64; rotation within 2e-4) | Linux CPU | Change one attempt's order |
| AC7 | R27 | Golden `foliage_legacy_layer` (`FoliageGpu.LegacyLayerLooksTheSame`): **committed first on the pre-T5 engine** as a characterization test, unchanged afterwards; mean error ≤ 0.6/255 and ≤ 0.5% of pixels beyond 16/255 | gl, vk, metal | Change the fade start or drop the tilt |
| AC8 | R8, R9 | `FoliageTypes.InstanceIs32BytesAtFixedOffsets`; `FoliageCull.KeepsExactlyTheInstancesInTheFrustum`, `.InsideCellsSkipPlaneTests`, `.LodBucketsMatchSelectMeshLod` | CPU | Off-by-one in a plane test |
| AC9 | R9 | `FoliageGpu.StillCameraUploadsNothing` (buffer update counter), `FoliageGpu.DrawsThroughOffsets` (`base_instance` never used: sokol frame stats, draw count = expected) | gl, vk | Rebuild every frame |
| AC10 | R10 | `FoliageCull.DensityIsAStableSubset`, `.FalloffFollowsTheCurve`, `.FadeMatchesTheOldShaderRange` | CPU | Re-randomize rank by frame |
| AC11 | R11, R19 | Goldens `foliage_wind_pinned` (time pinned, colour and shadow) and `FoliageGpu.WindZeroUsesNoWindVariant`; `FoliageGpu.ShadowSwaysWithThePlant` (shadow moves between two pinned times by the expected direction) | gl, vk, metal | Use the wind only in colour |
| AC12 | R12 | `FoliageScatter.DefaultColoursAreWhite`, `.HealthyDryNoiseIsDeterministic` | CPU | Default dry colour non-white |
| AC13 | R13-R15 | Golden `foliage_trees_lod_bands` (three cameras); `FoliageTreesCore.CellLodBandsFromDistance`, `.MaxMeshTreesFallsBackToImpostors`, `.ExplicitLodMeshesAreUsed`, `.MeshBandBufferIsPerCell`, `.ImpostorBandIsOneDrawPerPrototype` | gl, vk, metal; CPU | Choose LOD by Terrain, not cell |
| AC14 | R15, F6 | `FoliageGpu.FadeBandShowsNoCellRing` (a ring of trees across a cell edge; a neighbouring-column luminance jump under a threshold) | gl, vk | Remove per-instance dither |
| AC15 | R16, F7 | Golden `foliage_tree_impostor_far`; `test_foliage_impostor.py::test_silhouette_matches_mesh` (bake by tool; at the same camera, mesh-only and impostor-only captures: coverage IoU ≥ 0.85 and mean colour within 20/255) | gl, vk, gles; web for the draw | Rotate view selection by 90° |
| AC16 | R17 | Golden `foliage_shadow_lod` and `FoliageGpu.ShadowMaskOffCastsNothing` | gl, vk | Ignore the mask |
| AC17 | R18 | `FoliagePhysics.CapsuleStopsADynamicBody`, `.ColliderIndexIdentifiesTheTree`, `.NoCollisionMeansNoBody`, `ModuleUsage.TreeCapsulesNeedPhysics3d`; `tools/build.sh` with `JASPERENGINE_PHYSICS_3D=OFF` builds | gl (headless), editor suite; Linux | Build the shape with the wrong scale |
| AC18 | R20, R21 | `FoliageSettings.EffectiveDistanceIsTheMinimum`, `.TierScalesWithoutWriting`, `.CapsDropTheFarthestFirst`, `.OldProjectIsHighEverywhereAndUncapped`; `test_foliage.py::test_force_field_overrides_wind` | CPU; gl | Write the tier into the component |
| AC19 | R22, F9 | `TreeStore.RoundTripIsByteIdentical`, `.RejectsTruncatedFile`, `.RejectsFutureVersion`, `.RejectsBadHash`, `.IdsAreNeverReused`, `.CorruptFileIsNotOverwritten` | CPU | Skip the hash check |
| AC20 | R23, R24, R54 | `FoliageRules.SlopeHeightLayerRules`, `.NoiseMask`, `.HolesAreExcluded`, `.SplineZoneIsExcluded`, `.MinSpacingHolds`, `.DryRunMatchesARealRun`, `.GoldenHashOnEveryTarget`; `test_foliage.py::test_scatter_by_rules_is_deterministic`, `::test_invalid_op_changes_nothing`, `::test_one_undo_step` | CPU, web gate; gl | Omit the hole check |
| AC21 | R7, R25, R26 | `FoliageTreesCore.ResnapFollowsTheSurface`, `.UndoRestoresTheBytes`, `.HoleHidesAndRestores`, `.TouchesOnlyDirtyCells`; `test_foliage.py::test_sculpt_resnaps_trees` (T1 surface agrees within 1e-4 at each tree) | CPU; gl | Re-snap all cells |
| AC22 | R45 | `FoliageBench` thresholds for a stamp; `test_foliage.py::test_mass_place_does_not_stall` (`harness.over_budget`, enforced with `JASPER_MCP_PERF_SLACK` on an idle host) | gl | Run Mass Place on the main thread |
| AC23 | R28, R29 | `TreeEditCmdTests.ConvertHostIsOneUndoStep`, `TerrainSplineBakeTests.ClearTreesRemovesFootprint` | editor suite | Leave the host populated |
| AC24 | R30, R31 | `FoliageLua.RemoveHidesAndFreesThePath`, `.RestoreReturnsIt`, `.StateRoundTrips`, `.ReloadDropsTheOverlay`; `test_foliage.py::test_chop_in_play_leaves_the_file_alone` (file hash before and after) | core suite; gl | Write removals into the store |
| AC25 | R32, R33 | `FoliageLua.ApiMatchesTheDocs` (Lua doctest of `docs/scripting/foliage.md`), `FoliagePhysics.OnTreeHitFiresOncePerContact`, `.NoSubscriberNoWork` | core and editor suites | Fire per persisted contact |
| AC26 | R34 | `NavBakeFoliage.TreesAreObstaclesAtEveryCamera`, `.GrassIsNeverCollected`, `.BakeDoesNotDependOnTheCamera`, `.ModifierExcludesTrees`; `test_navigation.py::test_tree_obstacle_path_goes_around`. The camera test **fails on the pre-T5 code** and is the red run | editor suite; gl | Pre-T5 commit |
| AC27 | R35 | `MeshRaycastFoliage.HitsATreeAndReportsItsId`, `.GrassIsOptIn`, `.MatchesDrawnTriangles`; `test_foliage.py::test_pick_at_reports_tree` (S26's `je-bench-picking` numbers stay within their thresholds) | gl | Omit the cell walk |
| AC28 | R36 | `FoliageGpu.*` goldens compare on gl, vk, metal with S18's cross-backend bound; `SMOKE_BACKEND=gles3 tools/smoke-test.sh` on a foliage fixture with no black or NaN pixels and no sokol validation errors; `GATE=foliage` canvas check on WebGL2 | gl, vk, gles, web, metal; D3D11 per checklist | Use an unmapped vertex format |
| AC29 | R37 | `test_foliage_export.py::test_prebuilt_lite_player_draws_foliage` (`run_build` with a `runtimeshaders`-free prebuilt player; `screenshot_frame` non-sky at the grass), `FoliageGpu.CustomVertexProgramUsesExpandedStream` | gl, Linux prebuilt | Skip prototype entities in the collector |
| AC30 | R38 | `FoliageSystemCore.EmptySceneDoesOneCheck` (counter and zero allocations through `AllocationCounter`), `FoliageGpu.NoFoliageNoPassNoBuffer`, existing `RenderGpu.*` goldens unchanged, `FoliageGpu.DisabledTerrainDrawsNothing` | gl, vk | Allocate streams at load |
| AC31 | R40, R44 | `FoliageBudget.BenchSceneWithinCounts/{High,Low}` (every frame's counts from `FoliageStats`, on llvmpipe: counts don't depend on the GPU) and `FoliageGpu.DrawCountEqualsStats` | gl; **counts, not device time** | Lift a cap |
| AC32 | R41, R42 | `je-bench-foliage` through `tools/perf/check_bench.py` (`bench-thresholds.json`): attempt, cull at 0 and 4 workers, tree view, still frame, empty scene, bytes; web p99 from `GATE=foliage` with a threshold | host, idle; web | Deliberate quadratic scan |
| AC33 | R39, R43 | Before-and-after gzipped size in `docs/perf/foliage.md`, by S34's method | host | n/a (a measurement) |
| AC34 | R46-R48, R52, R53 | `test_foliage_ui.py` (`find_ui_item` and scripted input: add prototype, stamp, Shift erases, Ctrl+Shift erases one prototype, Alt+click with no drag picks, Alt+drag orbits and places nothing, Mass Place job and Cancel, undo names), `TreeEditCmdTests.DeltaMemoryIsSmall`, `.StrokeMergesAndEscCancels` | gl, own project | Bind erase to Ctrl only |
| AC35 | R49 | `test_foliage_impostor.py::test_panel_state_goes_out_of_date` (touch the model: Out of Date), plus AC15 | gl | Ignore the source hash |
| AC36 | R50, R55 | `test_foliage.py::test_stats_match_a_count` (`sample_terrain` exact counts equal a Python count of `dry_run` plus scene; memory equals records × sizes within 10%); UI row presence in `test_foliage_ui.py` | gl | Report the resident count as total |
| AC37 | R51, R57 | `test_foliage.py::test_density_overlay_changes_the_capture` (structural: heat map appears, off leaves no pass via `get_gpu_passes`), golden `foliage_density_overlay` | gl | Leave the pass on |
| AC38 | R56, R58, R59, R63 | `test_foliage_impostor.py::test_bake_op_job_and_refuses_in_play`; `test_foliage.py::test_group_hidden_until_enabled`, `::test_set_properties_reaches_prototypes`, `::test_docs_exist`; `tools/mcp/tool_budget.py --check`; `gen_tool_docs.py --check` | gl | Register in core |
| AC39 | R60-R62 | `StreamTerrainFoliage.OldSceneLoadsWithDefaults` (a fixture from the pre-T5 format), `.SaveKeepsTheSceneVersion`, `.SaveAndLoadRoundTrip`, `.FactoryEmitsTheNewFields`, `CookedScene.TerrainFoliageRoundTrip`, `jasperengine-editor reflect --emit-runtime-fields` leaving no diff, `VersionControlMetadata.ListsTreeFiles`; `test_foliage.py::test_duplicate_terrain_copies_on_first_write`; `test_foliage_export.py::test_trees_ship_in_the_pack` | editor suite; gl | Drop a field from one serializer |
| AC40 | all visible | `docs/specs/checklists/T5.md`, by hand with screenshots from an isolated editor: painting feel, thumbnails, thinning at distance, wind in motion, fade band in motion, a phone (Android, iOS) and D3D11 frame times | a person with devices | n/a |
| AC41 | R67, R65 | Cases in `tools/mcp/eval/cases.py`, run as T1 AC74 says (`-k terrain_foliage`; lent accounts, else reported unverified): `terrain_foliage_add_details` (guard; "add grass on the flats": `edit_terrain_details`, then `scatter_by_rules` with `detail_layer` or `density_map {fill}`, then `sample_terrain` `details[0].drawn`), `terrain_foliage_mass_trees` (bad; "plant a forest": `scatter_by_rules` with `dry_run` first, must not call `create_entity` or `sculpt_terrain place_object`), `terrain_foliage_impostors` (guard; "far trees cost too much": `edit_terrain` op `bake_impostors`) | eval | A surface from before T5's text |
| AC42 | R65, R55 | `TerrainDetailRulesTest` gains T5's fields; `test_terrain_details.py` green with them; `test_foliage.py::test_drawn_reads_foliage_stats`. Run by whichever of T5 and `agent-authoring` lands second | editor suite; gl | Omit a field from `detailRanges()` |
| AC43 | R66, R21 | `FoliageSystemCore.WalkingFillsOnlyEnteringCells` (the 4-and-5 column walk of that spec's §4.6: 0 refills in cells that stay), `FoliageCull.MinDrawDistanceFadesAndSkips`, `.ScaleZeroRemovesFoliage`, `FoliageSettings.OneGlobalDistanceScale` (no second multiplier: a tier change leaves distances alone); `test_foliage.py::test_distance_scale_from_lua`. Run by whichever of T5 and `render-correctness` lands second | CPU; gl | Size the ring from the view span |
| AC44 | R68, R61 | `python3 tools/check_raw_writes.py` with no new allowlist line; `python3 tools/check_stream_keys.py`; `ShaderKey.CompactInstanceBitIsPinned`, `.FoliageWindBitIsPinned`, `.FoliageDitherBitIsPinned`; `AssetTypes.TreeDataValueIs30`; `TerrainSettingsStream.PlaceInstancedKeepsItsMeaning` | CPU, editor suite | `tools/check_raw_writes.py --self-test` |

**Verifiable where.** Decided up front, so no report overstates it. There is no CI: a command below is run by a person, and the row is unverified until then.

| Runs on this 8-core host (software GL, no KVM) | Not verifiable here: the command |
|---|---|
| Linux build, CPU suites, `FoliageGpu.*` goldens on llvmpipe (gl) and lavapipe (vk) | **Metal, macOS arm64 hash:** `cmake -S engine -B build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON && cmake --build build-engine --target jasperengine-core-tests && build-engine/tests/core/jasperengine-core-tests --gtest_filter=Foliage*` |
| GLES3 through `linux-backends` | **Windows hash, CPU suites:** with `vcvars64`, `cmake -S engine -B build-engine-gl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DJASPERENGINE_TESTS=ON && cmake --build build-engine-gl --target jasperengine-core-tests && build-engine-gl/tests/core/jasperengine-core-tests.exe --gtest_filter=Foliage*-*Gpu*`. **D3D11:** no automated pixel test exists, so AC40 by hand; attribute formats are D3D11's native ones (*inferred*) |
| WebGL2 in headless Chromium, single-thread, gate and hash | **Android:** APK **builds** here; running needs `/dev/kvm`: `ANDROID_PROJECT=foliage ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh`; frame time by AC40 |
| MCP tests with their own projects; those needing `fixture-unity` skip, so T5's use their own | **iOS hash:** `python3 tools/xcode_sync_engine.py --check`, then the `foliage` project from the Xcode workspace; its gate script prints the hash. **Real-device frame times and memory:** R41's mobile column, AC40 |
| Counts and bytes of every budget (AC31), CPU kernels on host (AC32) | **Host timings under load**: gated only on an idle host. `rb.sh` has 2 slots, so run `je-bench-foliage` with nothing else queued, or the result is reported, not enforced |
| Hash on Linux x64, wasm | Hash everywhere else: the rows above |

Software GL says nothing about GPU cost: a golden proves correctness, and AC31 proves counts. No ms-per-frame
claim for a GPU can come from this host, and a report must say so.

## 8. Dependencies & conflicts

- **Depends on, hard:**
  - **T1**: `TerrainOps.h` operation registration, `TerrainMapStore`, dirty rectangle events, surface
    query, the group limit from its foundation; `edit_terrain` and `visualize_terrain` overlays from its rest. See the seam table.
  - **T3**: is-hole in the surface query, `cut_holes`/`fill_holes` dirty events. `isHole` and `HoleMapView` come with T1's foundation (T1 R92).
  - **T2**: `noise_mask` evaluates a Noise Graph (`jasperengine::noise`); see Risks while T2 is in flight.
  - Landed: S06 (jobs), S18 (bake, spline XZ distance), S19 (`updateTiles`), S24, S26, S10, S15, S14, S04,
    S03, S05, S29, S30, S34.
- **Landing order** (T1 §8): T1's foundation first, then T1's rest, T2, T3, T5 and T6 in parallel, then T4 last (ledger D8).
- **Seams others must honour:**

  | With | T5 needs | T5 gives |
  |---|---|---|
  | T1 | Register operations from another file (`registerOp`); a read-rectangle on `TerrainMapStore` usable from the main thread to fill job inputs (`read`); a subscribable dirty-rectangle event from every height, hole and density write (`TerrainEditEvents`); an overlay source interface (`TerrainOverlay::registerSource`); `region` on `sample_terrain` | `FoliageOps`, `density` and `tree_density` overlays, stats for `sample_terrain` |
  | T2 | `CompiledGraph::evaluateGrid` from `NoiseGraphRuntime.h` (T2 R21, R23). The editor's operation evaluates the mask and hands `FoliageRules` a float buffer, so `engine/core/terrain/` and players don't link `JASPERENGINE_NOISE` | Nothing |
  | T3 | `terrain::isHole` and `HoleMapView`; T3's `TerrainHolesConsumers.Foliage` (its AC33) stays green when the scatter moves here | Hides trees and rejects placement over holes; narrows T3 R26's `needUpdateFoliage` to the dirty cells of T3 R28 |
  | T4 | Adds regions, offsets, per-tile files, per-tile budget release (F11) | `FoliageRegion`, `addRegion`, `removeRegion`, `TreeId` |
  | T6 | Nothing now | May use the scatter later through a region |
  | S18 | `SplineTerrainSettings` extended by one field | Trees cleared by a bake |
  | S19 | `updateTiles` | `collectTrees` |
- **Variant bits.** T3 D2's table is the one list: `Cin`, `Wnd`, `Dth`, in that order among ours, appended at landing; each is pinned by a test (AC44).
- **Merge hotspots:** `MeshSystem.cpp` (a big deletion, no additions), `RenderSystem.cpp`/`.h` (three hooks,
  `InstancedMeshComponent` one flag), `TerrainEditWindow.*` (T1, T3 too), `TerrainComponent.h`,
  `StreamComponents.cpp`, `RuntimeFields.gen.inc` (regenerate after merging, don't hand-merge),
  `editor/tests/reflect/catalog_snapshot.json`,
  `ShaderPool.cpp` and `ShaderVariantCompiler.cpp` (bits), `Render.h` (`AttributeType`,
  `UniformBlockType`), `mesh.vert` and its siblings, `PickTypes.h`, `NavBakeCollector.cpp`,
  `tools/mcp/budget.json`, `docs/mcp/TOOLS.md` (generated), `docs/specs/README.md`.
- **Doriax overlap (D21; none landed: whichever lands second merges onto the first).**

  | Doriax spec | What T5 touches in common | Rule |
  |---|---|---|
  | `agent-authoring` | Details tool and rules, `details`, `drawn`, `getFoliageInstanceCount`, `terrain.md`, load clamp in `StreamComponents.cpp:554` | R65 |
  | `render-correctness` | Foliage ring, `minDrawDistance`, Foliage Distance, `InstancedMeshComponent` near fade and slack, `TerrainComponent.h:154`, Terrain window Draw range | R66 |
  | `draw-perf` | `updateAllTerrainRenderTextures`; state sort in `drawSubmeshGeometry`, where F1's branch goes; `Dlo` on lit foliage keys | F1's branch stays first in the function. `Cin`, `Wnd`, `Dth` keys ship with their `Dlo` twins |
  | `ssr-probes` | G-buffer variants of `Cin` and `Wnd`: `Occ` and `Uv2` are appended beside them | Bits by name. Detail meshes with an AO map take its rule as any mesh does |
  | `mesh-memory` | `MeshComponent` containers; R6's prototype entities and `MeshSystem` use | Prototype entities hold `StableArray` submeshes; keep indices across writes. R6's memory claim gets better, and R42 does not count on it |

- **Pixel art.** S52's 40-byte sprite instance and `HAS_SPRITE_INSTANCING` are a separate format from `FoliageInstance`; both append to `AttributeType` and both branch in `drawSubmeshGeometry` (with `sprite-batching` too), so the merge keeps each branch guarded. S49's `Pixel` and `Analytic` bits are PARTICLES bits. S50's "terrain" is autotiling; no shared names.

## 9. Risks & defaults

- **Compound shape creation on a job.** `StaticCompoundShapeSettings::Create` off the main thread and a
  body with a prebuilt shape are *inferred* to work: `composeColliderShape3D`
  (`PhysicsSystem.cpp:2247-2254`) calls it on the main thread today, and `createGenericJoltBody`
  (`PhysicsSystem.h:329`) takes a shape but is private. Default: a small public wrapper. If job creation
  fails under TSAN, build on the main thread at most 2 cells per frame and record a Deviation.
- **Empty `colliders` on the body.** A `RigidbodyComponent` with a prebuilt shape and no `colliders` may
  surprise `collectColliders3D` and the Inspector. Default: engine-owned entities are hidden, and the
  collector finds nothing to collect; a test covers it (AC26).
- **Dither and MSAA or TAA.** Dither shows at a distance without TAA. Default: Fade Length is short and
  impostors are far away; mobile tier halves it. No alpha-to-coverage path: it isn't on every target.
- **Impostor look under different light.** Baked normals, re-lit: not perfect at sunset; default accepted.
  Baked AO only. Translucent leaves lose transmission in the impostor band, which F7's normal path may
  approximate with `foliageTransmission` (`Material.h:186`) if cheap. Default: copied from the mesh's
  first material.
- **Mesh-band draw count with many prototypes per cell.** Default: R21's cap, then Max Mesh Trees. Merging
  ranges across cells isn't possible with one buffer per cell (F5). A prototype with no impostor keeps its
  mesh band to Tree Distance, so it pays per-cell draws all the way, and the panel warns (R53).
- **8-unit detail cells at long distances.** Up to 900 cells per layer, per view. Default: linear AABB tests
  ≤ 100 µs per view at this size; a coarser grid only if the benchmark says so.
- **Preview of a whole big Terrain** (`setFoliagePreviewEntity`): today it places everything. Default: same
  behaviour, on jobs, nearest first, stopping at 2,000,000 instances with a note in the panel.
- **Density maps in old scenes with non-8-bit or multi-channel data.** Today reads the first byte
  (MS:3992). Default: same read, so parity holds; T1's store must keep that meaning.
- **`std::sqrt` and normalization on wasm.** IEEE exact by the wasm spec; the web gate proves it.
- **Hash stability versus future tuning.** Changing placement changes the constant. Default: a changed hash
  needs a new `foliageAlgorithmVersion` on the layer, with old layers staying on their version. T5 only ships
  version 1, so the mechanism is one field and one test, `FoliageScatter.VersionFieldIsWritten`.
- **Quality default on old mobile projects.** High keeps today's look but no protection; the panel warns
  (R53), and the coordinator may prefer otherwise. Default as F12.
- **Lua API on `Terrain`.** Bindings reopen the class in a new file (`GameObjectClassesLua.cpp:2086-2108` stays
  untouched). If LuaBridge's reopened class hits S34's binding-size rules, it goes under the same
  source list.
- **T2 slipping.** Default: `noise_mask` is rejected with a clear message until T2 lands, and only
  `FoliageRules.NoiseMask` waits on it. No stub ships: the parameter is added with T2 present, as a
  recorded Deviation otherwise.
- **Sibling specs were written at the same time.** All six were read and matched afterwards. T1's names win
  for `TerrainMapStore`, `TerrainSurfaceQuery`, operation registration and overlay source where a later change
  differs, and this spec's sketches follow with no change in behaviour.

## Deviations

Recorded by the implementation, each with what it does instead and why.

1. **`FoliageMapPatch` instead of a `SurfaceView` window.** T1's `SurfaceView` samples a whole component on
   the main thread and has no window. A placement job copies its cell's rectangle of the height, density and
   noise maps into a patch and samples it with the same arithmetic, equal to the bit to whole-map sampling
   (`FoliageScatter.PatchesMatchTheWholeMap`). Holes are a shared whole-map copy, refreshed on a hole edit,
   and read through T1's `isHole`.
2. **T1's `edit_terrain`, `visualize_terrain` and overlays, with T5's ops in a call of their own.** T1's
   checkpoint landed first (R67). Its parser reads T5's registered ops; a list of them runs as a job
   (`FoliageOpRunner`: T1's `applyOps` on a snapshot, off the main thread, so a big scatter does not stall),
   which a list mixing them with map ops or `create` would make all-or-nothing across two engines: such a
   call is refused with the split to make. `edit_terrain` gains `wait` and `timeout_seconds` for that job.
   Density undo is T1's `TerrainMapDeltaCmd`; trees are a `TreeEditCmd` in the same group. `density` and
   `tree_density` are T1 overlay sources (registered by `TerrainFoliagePanel`); `visualize_terrain`'s
   `foliage_quality` is gone (a tier is a project setting). A tree overlay redraws when its settings change or
   the terrain's maps do, not after each tree edit.
3. **Compound shapes on the main thread.** At most two cells a frame, and in the same frame on a runtime
   removal (the risk table's default): Jolt's shape build off the main thread is not on every target.
4. **Runtime clones load the flushed file** instead of sharing the store's pointer. `shareTreeStore` and
   copy on first write exist for duplicated Terrains; a Play clone goes through its loader so its path is the
   one an exported game runs.
5. **AC7 compares within one run.** The pre-T5 golden could not be committed first, since this branch starts
   after T1's foundation changed the terrain draw. `FoliageGpu.LegacyLayerLooksTheSame` draws T5's layer and
   the same placement on a legacy Instanced Mesh with the old fade, and holds them to AC7's bounds.
6. **A fourth hook in `RenderSystem`.** `selectMeshDraw` needs a branch of its own for a foliage slot's
   instance range, beyond F1's three.
7. **The bake renders two passes** (unlit colour and world normal) with `library:impostor_bake` rather than
   reading the G-buffer: that path works on the backends and on the lite player alike.
8. **Point-light shadow faces use the main camera's list** (`instanceView` -1). A list per face would cost
   six culls for a light that rarely reaches foliage.
9. **`noise_mask` is refused until T2 lands.** `host.noiseMask` is the hook, and `FoliageRules.NoiseMask`
   passes with a caller's buffer.
10. **Dither on built-in Lit, `library:foliage` and `library:impostor` only.** A custom surface shader keeps
    its own alpha; it fades with distance through its discard.
11. **`TreeRecord.rotation` stores the yaw; the lean's share is in the upper five flag bits.** The tilt is
    derived at a cell's build, so the undo of a sculpt restores the store's bytes (AC21).
12. **The "Terrain Settings: Foliage" section lives in the panel.** T1's Terrain Settings panel
    (`TerrainSettingsPanel`) landed with the T1 checkpoint; moving the section into it is left for after this
    merge, with the same fields.
13. **R66: render-correctness landed first.** T5 takes its `TerrainFoliageLayer::minDrawDistance`, its
    `Engine` Foliage Distance API and project key (`foliageDistanceScale`, settings `quality.foliage_distance_*`),
    and adds Tree Distance and Billboard Start to what the scale multiplies. Its new-project default stays 1
    everywhere (its `test_render_correctness.py` pins that), so new projects do not write 0.5 on Mobile and Web
    (R20): the new-project Foliage Quality of Low there is the mobile protection. Its chunk-entity foliage in
    `MeshSystem` gives way to F1's `FoliageSystem`; `TerrainFoliageGpu.*` keeps what each test pinned (one entity
    a layer that walking never destroys and that places only entering cells; scaled Draw and Min Draw distances;
    a scale change drawing nothing twice; 0 removing foliage and the preview ignoring it), read from the view's
    instances instead of chunk components.
14. **Details on a brush frame.** T1's stroke sessions publish what each frame wrote as `TerrainEditEvents`,
    which `FoliageSystem` hears; a restored snapshot refills every cell.
15. **Tree obstacles do not need Physics 3D.** The collector's cylinder builder is compiled in without it,
    since a navmesh is not a physics feature.
16. **R65: agent-authoring landed first.** `edit_terrain_details`, `sample_terrain`'s `details` and `drawn` and
    `MeshSystem::getFoliageInstanceCount` stay theirs; the count forwards to `FoliageSystem` (the instances a
    layer holds over its resident cells, what `drawn` meant). T5's float layer fields join `detailRanges()`
    (`min_height_ratio`, `max_height_ratio`, `noise_spread`, `wind_bend`, `density_falloff_start`,
    `far_density`, `density_scale`), so the tool, `set_properties` and the load clamp hold them. The colours
    (`healthy_color`, `dry_color`) are vectors, which that table of floats does not hold: they go through
    `set_properties` as any colour does.
