# T4 — Terrain Worlds: tiles, streaming, collision paging

Terrain programme (`TERRAIN-LEDGER.md`, decisions D1-D21), step 4 of 6. It lands last, after T1, T2, T3,
T5 and T6 (programme D8; order in T1 §8). Everything under "Current state" was checked by
reading today's code at the cited line (T1 and T3 will move some of it); nothing in this spec was
built or run while writing it. Numbers marked *calc* are arithmetic, *assumed* are background knowledge
that a named test or checklist item must confirm. "D" means this spec's decisions in section 6;
"programme D" means the ledger's.

## 1. Goal

A game can have ground far larger than one map: a **Terrain World** is a grid of tiles that streams
around one or more focus points, draws the distance from one low-resolution **overview** in a bounded
number of draws, and pages Jolt collision so nothing falls through. An ordinary Terrain keeps working
exactly as it does and a project with no world pays nothing. Brushes, MCP, Lua, spline bakes and
generation edit across tile borders through T1's operation engine as if the world were one big map,
in one undo step, and tiles appear where a brush goes. People get a panel with a minimap of the tiles;
agents get one tool and a status read with a deterministic wait, so "the area is ready" and "the seam is closed" are
testable facts.

## 2. Scope

**In scope**
- `TerrainWorldComponent` beside a `TerrainComponent` (layers and material stay where they are); the
  `.terrainworld` folder asset, `manifest.yaml`, versioned `tiles/x_z.jtile`.
- World-spanning CDLOD selection that never walks the world; near tiles from a bounded page pool, the
  distance from one overview (height plus macro colour); skirts; a format with one code path on all
  five backends.
- Streaming on `jobs::` and `AssetIOService`: focus points, distance priorities, cancellation, a
  per-frame upload byte budget, zero-worker operation, residency states, area-ready wait.
- Collision paging: Jolt heightfield chunks round each focus, coarse colliders as the no-fall-through
  guarantee, holes, world-scale surface query and raycast.
- `TiledMapStore`, the tiled `TerrainMapStore` of D4; automatic tile creation; heightmap import that
  slices; convert Terrain to Terrain World; seam verification and repair.
- Navigation of a world (region bakes, follow-the-focus rebakes); the hooks S18 and T5 attach to.
- Player module `JASPERENGINE_TERRAIN_WORLD`; cook and export; web limits.
- Editor panel, overlay, inspector; `edit_terrain_world` (which also creates a world), `sample_terrain {world}` for status;
  the Lua `TerrainWorld` class.

**Out of scope** (decisions, not open questions)
- A floating origin or camera-relative rendering (D13 records why). Worlds stop at ±16,384 m.
- Terrain3D's clipmap (programme D11); CBT terrain and GPU-driven culling (`docs/specs/README.md:23-24`).
- Compute, SSBOs, bindless, indirect draws, integer textures (programme D13), a shared array of tiles
  (programme D12).
- Fetching tiles over HTTP on the web (R38 states what that leaves). Saving runtime edits to disk.
- Navmesh data paged per tile (D15). Per-tile surface layers: one layer set per world.
- New material features: T3 owns them (programme D7). T3's Macro Normal Map and Base Map macro, which are one
  texture over one terrain, are unavailable on a World for now and the Inspector says so. Foliage placement itself: T5 (programme D9).
- Overhangs and caves: T6.

## 3. Current state

**Selection walks everything.** `RenderSystem::updateTerrain` clears `visible` on every node
(`engine/core/subsystem/RenderSystem.cpp:12822`), then recurses from each root (`:12830-12831`). The
tree is built eagerly: `rootGridSize² * (4^levels-1)/3` nodes (`engine/core/component/TerrainComponent.h:15-20`,
`engine/core/subsystem/MeshSystem.cpp:3701-3707,3740-3748`). It runs when camera or transform changed
(`RenderSystem.cpp:15093-15106`), and render-to-texture views run it again (`:15214`, `:17432`). A world
needs a new selection, and needs something to mark residency changes (`RenderSystem.cpp:7199-7201` is
the precedent: set `transform.needUpdate`).

**Draws.** Two instanced grids (`MeshSystem.cpp:3718-3720`; `RenderSystem.cpp:8725-8728`), one per
submesh, per pass; 4 views cached (`TerrainComponent.h:12`); depth and G-buffer passes draw view 0's cut
(`RenderSystem.cpp:9021-9024,9857-9860`). Per-view instance buffers are sized by the whole tree
(`:6232-6236`), 320 MB at the node cap (*calc*: 2M nodes x 20 B x 8). A bind swap with a byte offset
already exists (`bindInstances`, `:8403-8405`; `ObjectRender::replaceVertexBuffer`,
`engine/core/render/ObjectRender.h:40`), and bindings may change between `beginDraw` and `draw`
(`ObjectRender.h:62-64`). Base instance is unavailable on GLES3 (`engine/libs/sokol/sokol_gfx.h:10301`).

**Height in the shader.** `getHeight` uses a filtered `texture()` at `(pos + size/2)/size`
(`engine/shaderlib/includes/terrain_vs.glsl:64-65`), so a map's edge lies half a texel beyond its first
sample: texel `i` sits at `(i + 0.5)/W` (`engine/core/physics/JoltShapeBuilder.cpp:268-271`). Six height
reads per vertex (`terrain_vs.glsl:73,89,103-111`). Morph depends on eye distance and node data only
(`:78-85`). No skirts. Texel-fetch in the vertex stage has precedent on all backends
(`engine/shaderlib/includes/skinning.glsl:25`, `lattice.glsl:86`).

**Formats, by backend** (`sokol_gfx.h`): R16 is absent on GLES3 and WebGL2 (`:9952-9955`); on Metal it
is filterable (`:15366-15369`); R16F filters everywhere it exists (`:10003-10007`); R32F is
sample-only without the float-linear extension (`:10039`); **RG8 is fully supported on GL
(`:9958`), Metal (`:15375`) and maps straight on D3D11 (`:13036`) and Vulkan (`:20500`)**. The engine
converts R16 to R32F, or R16F when R32F does not filter, only at creation
(`engine/renders/sokol/SokolTexture.cpp:291-320,450-452,557`); `SokolSystem::updateImage` passes bytes
unchanged (`engine/renders/sokol/SokolSystem.cpp:563-574`). R16F has a step of 2^-11 in [0.5, 1), so
0.49 m at a 1,000 m range against 15 mm for 16 bits (*calc*). S11's statement that GLES3 "cannot
sample" R16 (`docs/specs/S11-materials-lighting.md:719`) is stale.

**Uploads.**
- `sg_update_image` must supply every slice (`sokol_gfx.h:22775-22776`), once per image per frame
  (`:12432`).
- A non-immutable image has two slots on GL and Metal (`:8227`, `:11021`, `:12433`), so a dynamic or
  stream page costs twice its memory there.
- On Vulkan an update of a `dynamic_update` image waits for the queue (`:20026-20028`, `:22158`), and
  creating an immutable image with data submits and waits idle too (`:21188`, `:19932`). Only
  `stream_update` avoids the stall (`:22154-22156`), and it goes through the frame's staging buffer:
  48 MB desktop, 16 MB mobile and web (`engine/core/Engine.h:278,309-311`). Past it `updateImage`
  returns false (`SokolSystem.cpp:564-571`) and only the first refusal is logged.
- Existing creators cover RGBA8, R8 and RGBA32F (`SokolTexture.cpp:620-720`);
  `createTextureLevels` takes `GpuTextureFormat::RG8` (`SokolTexture.cpp:520`), and image pools are large
  (`Engine.h` `GpuResourceLimits`: 16,384 images, 2,048 samplers, 24,576 views).

**Physics.** One static heightfield (`JoltShapeBuilder.cpp:257-300`), at most 1,025 samples, a larger
map resampled (`JoltShapeBuilder.h:64`), main thread. The grid's diagonal runs from low x, low z to high
x, high z (`MeshSystem.cpp:3312-3323`, z = -y), the same as Jolt's (`engine/libs/joltphysics/Jolt/Physics/Collision/Shape/HeightFieldShape.cpp:215-233`).
Batch add exists in Jolt (`Jolt/Physics/Body/BodyInterface.h:120-127`), precedent
`engine/core/physics/PlacementWorld.cpp:1461-1462`. Bodies carry `packBodyUserData(entity, mask)`
(`engine/core/subsystem/PhysicsSystem.cpp:2795`), and contact callbacks wrap the entity's Rigidbody
(`engine/core/util/JoltPhysicsAux.h:680-681`). Spline chunks are engine-owned GameObjects with a
Rigidbody each, made on the main thread (`engine/core/subsystem/SplineSystem.cpp:1385-1404`,
`SplineSystem.h:88-90`); fine at 10 chunks a rebuild, too heavy for a sliding window. Jolt is single
precision (`DOUBLE_PRECISION` off, `engine/libs/joltphysics/CMakeLists.txt:573`).

**Navigation.** The collector reads whole heightmaps on the main thread, one lattice per terrain, at
most 4,097 samples a side (`engine/core/navigation/NavBakeCollector.cpp:50,615-672,677-693`).
`NavigationSystem::updateTiles(surface, bounds, error)` exists (`engine/core/subsystem/NavigationSystem.h:111`).

**Jobs and I/O.** `AssetIOService::submitRead` reads a whole file or a range through `FileData`, with
priorities, `CancelToken`, and an inline mode with no readers (`engine/core/io/AssetIOService.h:31-36,74-96`).
Priorities are fixed at submit. With 0 workers nothing runs at submit; the Engine pumps once per frame
(`engine/core/jobs/JobSystem.h:24`), Background for 4 ms (`docs/specs/S06-jobs-memory.md:252-253`).
`jobs::drainMainQueue` and the pump run on paused frames too (`engine/core/Engine.cpp:2226-2231`), but
`MainThreadQueue` does not (`engine/core/thread/MainThreadQueue.h:16-18`, `Engine.cpp:2356-2358`), nor does
`Scene::update`. The editor's frames go through `Engine::systemDraw`, once per view
(`editor/EditorFrame.cpp:170`).

**Packs.** `.jpak` has no range API: `JPak::read` returns a whole entry, checked by FNV-1a 64; `view`
gives memory packs a span in place (`engine/core/io/JPak.h:65-68`). `ResourcePack::RangeReader` works only
on the legacy pack: it opens through `findEntry` (`engine/core/io/ResourcePack.cpp:370-383`) while content
packs are served through `contentEntryPack` (`:338`, `:402`). The web player keeps `content.jpak` in the
heap (`docs/specs/S30-shipping.md:326`), initial memory 256 MB (`engine/CMakeLists.txt:416`).

**Assets.** Terrain maps are textures with a `.meta` each (`docs/specs/S04-asset-identity.md:275,289-290`).
The asset scan skips dot-directories and `build/` (`editor/asset/AssetTypes.h`, `isSkippedDirectory`;
`editor/util/AssetDatabase.cpp:1073-1078`). S03 versions only YAML (`docs/specs/S03-data-safety.md:241-252`).
`writeFileAtomic` takes a whole buffer (`S03:143`). The binary patterns for git are a list
(`editor/Project.cpp:6592`). Terrain maps are in the self-write suppression list
(`docs/specs/S05-file-watcher-hot-reload.md:158`).

**Modules.** Six modules (`engine/core/player/PlayerModules.h`, `engine/player/variants.json`), wired in
`editor/Exporter.cpp:3595` and the Xcode replacement `:4611`, `engine/cmake/JasperPlayerVariants.cmake`, CMake
`engine/CMakeLists.txt:42,2055`,
detection `editor/players/ModuleUsage.cpp:316-330`. zstd's decompressor is in every game and the
compressor in the editor tree (`engine/CMakeLists.txt:1142-1146`). The player never parses YAML scenes
(`docs/specs/S29-version-control.md:299`), so a world needs a cooked binary manifest.

## 4. Reference

Upstream clones are data; nothing is lifted. No code is copied, so `NOTICE.md` gets no entry unless the
implementer copies something, in which case the normal rule applies.

| Source (licence) | File | Take | Improve or avoid |
|---|---|---|---|
| Terrain3D (MIT) | `src/terrain_3d_data.h:19` (32 x 32 regions), `doc/docs/double_precision.md` (65.5 km) | The sparse grid of regions; the limits as a yardstick | Ours is 512 x 512 addressable and needs no uniform table: the CPU selects |
| | `doc/docs/performance.md:50` | Streaming is only planned upstream: no code to study | Written from scratch |
| | `src/shaders/main.glsl:169-176` | `texelFetch` plus manual bilerp for height | Used on every backend, not as a fallback |
| | `src/terrain_3d_collision.h:34-35` (shape 16, radius 64) | Sliding collision window round the player | Off the main thread, Jolt heightfields, coarse fallback that upstream lacks |
| | `src/terrain_3d_editor.cpp:58-59,102` | Auto regions: sculpting creates a tile | Same rule, applied in the store so MCP and Lua get it too |
| godot_voxel (MIT) | `src/engine/priority_dependency.cpp:44-53` | Priority from distance, re-polled | A module-side queue, because `AssetIOService` priorities are fixed at submit |
| | `src/terrain/variable_lod/voxel_lod_terrain_update_clipbox_streaming.cpp:20-22` | The warning: a dropped task can leave a hole for good | Cancelled tiles return to Cold and are demanded again by R14 and AC15 |
| | `src/engine/priority_dependency.cpp:47-50` | The warning: load order changes cracks | Skirts make cracks independent of load order (D5) |
| | `src/doc/source/performance.md` (8 ms main budget, mesh frees spread) | Spread main-thread application | 2 ms, and a byte budget beside the clock |
| OpenEngine | none | Its CBT terrain is on the do-not-copy list | |

## 5. Requirements

### Functional — world and tiles

- **R1 Component.** A Terrain World is a GameObject with Transform, Mesh, Terrain and
  `TerrainWorldComponent`. The Terrain holds layers, material and foliage layers as today; its height
  map, size-derived LOD settings and node tree are unused. Height, blend, hole and density data live in
  tiles. A plain Terrain without the new component takes every path it takes today.
- **R2 Lattice.** A world has one lattice: `tileQuads` N ∈ {64, 128, 256, 512, 1024} (default 512);
  `spacing` h, a power of two from 0.25 to 8 m (default 1); tile size S = N·h; tile `(x, z)` covers
  `[x·S, (x+1)·S]` by `[z·S, (z+1)·S]` in the entity's local XZ, x and z in [-256, 255]. `maxHeight` is
  world-wide, heights run from 0 to it as on a Terrain (T1's `TerrainFrame`), and samples are 16-bit
  normalised over it, so changing it rescales the world without touching a tile. Ground below sea level
  means moving the entity down.
- **R3 What a tile stores, by kind of map** (T1's `MapKind`):

  | Kind | Texels a side | Meaning of a texel | At a tile border |
  |---|---|---|---|
  | Height | N + 3 | sample at `i·h`, with a one-sample gutter | N + 1 shared samples, gutter copied from the neighbour |
  | Blend | `blendQuads` + 1 (default N/2) | filtered sample | shared; no gutter |
  | Hole | `holeTexels` (default N) | an area, T3 R19's `floor(u·W)` | each texel belongs to one tile, nothing shared |
  | Density | T5's size | an area, as today's nearest read | nothing shared |

  So a central difference at a border reads the same heights from either side, and filtered blend agrees
  across it. Where a neighbour is absent the gutter repeats the border, and adding the neighbour rewrites
  it.
- **R4 Transform.** Worlds translate only. Rotation and scale are forced to identity; the inspector
  shows them disabled and `set_transform` calls that ask for them log one warning.
- **R5 Sparse grid.** Tiles may be absent. An absent tile draws nothing and has no collision, and the
  query reports it as such; this is not a "hole" in the sense of R10, which is about present tiles.
- **R6 Limits.** |x·S| and |z·S| ≤ 16,384 m, so 32.768 km a side at most (*calc*: float32 half-ULP at the
  edge 0.98 mm; D13 has the table). The bounding box of present tiles is at most 256 tiles a side on
  desktop, 128 on mobile and web, at A = 16 (the overview's side, R34). Past ±8,192 m the editor warns once
  per world. An add past a limit is refused and names the limit.

### Functional — rendering

- **R7 Selection.** Per view, a single world quadtree whose level T node is exactly a tile
  (T = log2(N / R), R = 32 the grid resolution). Selection follows D4 exactly. Its visited-node count
  does not grow with world size: on a filled 256 x 256 world it stays within the count on a filled
  8 x 8 world plus 4 per doubling of the side. No per-frame per-node or per-tile write outside what it
  visits.
- **R8 Near tiles.** A level ≤ T node draws from its tile's page: height (RG8) and maps. One page per
  tile (programme D12). Pages come from a pool bounded per platform (R34), least recently used out, and
  never one in the focus's tile or a pinned edited one.
- **R9 Overview.** Beyond the near band everything draws from one overview over the bounding box of
  present tiles: a height texture with `A·tiles + 1` samples a side (A = 16 by default, 32 optional)
  holding the exact tile samples at that stride, and a macro colour texture of the same size. It costs at
  most 2 instanced draws per view (the full and half grid), whatever the world size. It draws as T3's Low
  tier with no layers, casts no shadows by default (Overview Casts Shadows, off) and ignores holes.
- **R10 Never a hole.** A present tile whose page is absent draws from the overview as a level T node,
  from the first frame the overview exists. Pool exhaustion, draw-cap overflow, read errors, corrupt
  tiles and uploads refused by R13 all fall back the same way. Before the overview exists the world
  draws nothing and `isReady()` is false; the scene's load wait covers it (D10).
- **R11 No cracks.** Geometry is watertight at every residency state, on every backend (D5): identical
  samples and dyadic coordinates give bit-identical shared vertices at equal LOD, CDLOD morph handles a
  one-level difference, and skirts cover any other adjacency. The result is measured (AC8).
- **R12 One height path.** Every backend stores height as 16 bits in RG8 and reads it with `texelFetch`
  and a manual bilerp (D3). No R16, R32F or R16F for world tiles, no sampler filtering of height, no
  conversion at upload. A vertex costs 9 height fetches at most (5 in depth passes) against today's 6
  filtered, plus one macro fetch in the colour pass (D14).

### Functional — streaming

- **R13 Uploads.** A page goes up through `TextureRender::createTerrainPage`, immutable on GL, GLES3,
  Metal and D3D11, and a reused `stream_update` image on Vulkan (D6). No more than the frame's byte
  budget (R34) is uploaded, but always at least one image, so progress is guaranteed. The Vulkan path
  checks the return of `SokolSystem::updateImage`: a refusal keeps the data, counts in
  `upload_retries` and retries next frame, and the frame's budget is at most a quarter of
  `getStreamBytes`. A tile becomes drawable only when all its images are up.
- **R14 Tile states.** `Absent`, `Cold` (file exists, nothing loaded), `Queued`, `Loading`, `CpuReady`,
  `Uploading`, `Resident`, `Error`. Cancelling a load returns the tile to `Cold`, and if it is wanted
  again it is queued again. A failed read retries 3 times with backoff (0.5, 1, 2 s), then `Error`,
  logged once per tile. `Error` falls back per R10, and a changed file clears it.
- **R15 Focus points.** A focus is a position, a radius multiplier and a velocity. The scene's active
  camera is a focus by default (Use Camera Focus); in edit mode, the Scene view camera. Scripts and the
  component add more, at most 8 a world, for split screens, cut scenes and server-side use with no
  camera at all. A headless player has no GPU work: tiles reach `CpuReady`, which is enough for
  collision and queries.
- **R16 Priorities.** A tile is wanted when it touches a focus's near band, centred on the focus or on its
  predicted position (`position + velocity · 1.5 s`). A module-side queue, re-sorted every 100 ms, orders
  wanted tiles by distance. `maxInFlight` loads run at once, mapped to `IOPriority`: the focus's tile
  `Critical`, the band round the focus `High`, the band round the prediction `Normal`. A load whose tile
  is more than 2·S outside every wanted band is cancelled through its `CancelToken`.
- **R17 Threads.** All reading through `AssetIOService::submitRead` (whole file, no ranges, D8), all
  decoding in Background jobs with `Tag::Asset`, split per section so no single job exceeds about 4 ms
  on the web's tile size. Works with 0 workers and 0 readers. No threads of its own. Results are polled
  from a pump that runs once per engine frame, paused or not (D9), never through `MainThreadQueue`.
- **R18 Ready.** `isAreaReady(x, z, radius, level)` is true when every present tile touching the circle
  is `Resident` (level `render`), or `CpuReady` or better with its fine collision active (level
  `collision`). Scripts wait with `Coroutine.waitUntil` or `world:waitForArea`, or get
  `world:onAreaReady`, called in the update phase. A pending area is itself a focus, so waiting for one
  loads it. `world:ensureCollisionAt(x, z)` builds the coarse collider now, for teleports (R22).
- **R19 Visible state.** `getSurfaceAt(x, z)` and `sample_terrain {world}` report for any point:
  absent, `overview` or `full`. Queries over a non-resident tile answer from the overview and say so.

### Functional — collision

- **R20 Chunks.** Physics enabled by a static Rigidbody on the World, whose settings (layer, category,
  mask, friction, restitution, listeners) every paged body copies. Its own body has no shape.
  A tile divides into chunks of `collisionChunkQuads` (default 64; ≥ 32, power of two), each a
  `JPH::HeightFieldShape` built from the tile's samples directly, with no resampling, on a Background job.
  Bodies are created and added in batches with `AddBodiesPrepare` and `AddBodiesFinalize` in the pump,
  at most 16 per frame. Removal goes through the deferred destroy path.
- **R21 Subjects.** Collision subjects are the focuses (radius `collisionRadius`, default 128 m) and
  every awake dynamic body and Character Controller (one chunk ring round its position and round
  `position + velocity · 0.25 s`). At most 64 transient subjects; past that the farthest are put to
  sleep and counted in `frozen_bodies`, with one warning.
- **R22 No fall-through.** Exactly this: when a subject stands over a present tile that is not
  `CpuReady`, that whole tile gets a **coarse collider** in the same pump, built on the main thread from
  the CPU copy of the overview (A+1 samples a side, ≤ 0.2 ms, at most 8 per frame), and the tile is
  queued `Critical`. A coarse tile matches what is drawn there, because the overview is what is drawn.
  When the tile arrives, its coarse body and its fine chunks swap in one batch before the next physics
  step. A subject over an absent tile has no ground: this is the designer's world edge. A body teleported
  below the surface is not rescued; `ensureCollisionAt` is called before the teleport.
- **R23 Match.** Fine collision uses the tile's own samples, with Jolt's bits chosen by
  `CalculateBitsPerSampleForError` for 1 cm (at most 8), so at lattice points a ray agrees with the query
  and with the drawn surface at LOD 0 within 1 cm plus one quantisation step. Mesh and collider split a quad
  along the same diagonal. Holes follow T3 R22: a sample is `cNoCollisionValue` when all its adjacent
  squares are open, which needs the adjacent squares of the next chunk or tile; a neighbour tile that is not
  resident counts as solid, so the collider never opens under drawn ground. A runtime edit (T1) rebuilds the
  dirty chunks on a job and swaps them in the pump. Physics Placement (`PlacementWorld.cpp:1418-1430`) builds
  fine shapes for the chunks its context touches, reading tiles synchronously; `createHeightFieldCollider` on a World refuses
  with a message that names paging.
- **R24 Surface and rays.** One kernel (programme D5): `querySurface` (T1 R14) gets the one branch T1
  reserves for it, finds the tile and calls T1's `sampleSurface` on a per-tile `SurfaceView` with the gutter
  included (N+3 is odd, so `(i + 0.5)/W` over a view of size `(N+3)·h` lands exactly on `i·h`), with T3's
  per-tile `HoleMapView`. `SurfaceSample.exact` and `.present` (T1 R16) carry the answer's standing. Waiting for a tile is allowed from a job
  or the editor; on the game's main thread the query never waits and answers from the overview with
  `exact = false`. T1's `Terrain:getHeightAt`, `getNormalAt`, `isHoleAt` and `raycast` so work on a World as
  they are. `raycastTerrainSurface` gets one branch to `TerrainWorldQuery::raycast`, a grid DDA over tiles
  with min/max rejection calling `raycastSurface`, so `MeshRaycast.TERRAIN`, picking, the brush cursor and
  `VatCrowd` work unchanged. Resident tile hits agree with the query within 1e-4 (S26:467).

### Functional — navigation, splines, foliage

- **R25 Navigation.** The collector takes a world's tiles overlapping the surface's bounds, one lattice
  a tile, holding each tile's CPU data by shared reference for the bake, in job-loaded form. A surface
  without a Volume on a world over 64 tiles is refused before collecting, in the wording of S19 R6. Each
  tile's lattice applies T3 R23. A NavMesh Surface gets **Follow Terrain World Focus** (radius, default
  128 m), rebaking through `updateTiles` when the focus has moved S/4 and dropping tiles outside radius + S/2, with agents kept
  moving as S19 R10 says. Off by default, and off on web single-thread unless a script sets it.
- **R26 Splines.** Bakes pass through T1's operations and so cross tiles. Conform queries from its
  Background jobs, where R24 may wait, so a chunk's height never comes from the overview when the tile
  exists. S18's water fill reads the rectangle through the store rather than a heightmap
  (`SplineSystem.cpp:1005-1019`).
- **R27 Foliage.** Per T5 F11: `TerrainWorldSystem` calls `FoliageSystem::addRegion` with one
  `TileFoliageRegion` when a tile turns `Resident` and `removeRegion` when it leaves. A region's `maps()` is
  a store over that tile, `surface()` the query, `trees()` T5's `.jetrees` file beside the tile
  (`tiles/x_z.jetrees`; null while loading), `cellOffset` the tile's origin in whole 8-unit cells (S ≥ 16, so
  always whole), `regionKey` the packed coordinate. Ground that is not resident has no foliage and nothing
  blocks on it. A world without foliage layers makes no regions. T5's tree tools (the Paint Trees brush and
  `sculpt_terrain`'s `paint_trees`/`erase_trees`, Mass Place Trees, `edit_terrain`'s foliage ops, a spline's Clear
  Trees, Remove Prototype) route each tree to the tile under it through `TerrainWorldTreeEdit`
  (`editor/terrainworld/TerrainWorldTrees.*`): a resident tile through its region's store, a cold one read from its
  file there and then (R24: the editor may wait); a tile without a file gets one; one undo step a stroke or op,
  keeping each file's bytes before and after. FoliageSystem's calls that take no position cover every tile on a
  World: one hit and removed handler for all tiles (tiles that arrive later too), the runtime state (`JEFW1`, tiles
  out of memory keep their overlay) and the placement hash in key order, the resident cells in the World's space;
  `getTreeStore` without a position is a lone Terrain's (null on a World), with a position the tile's there.
  T5 owns the rest.

### Functional — editing and data flow

- **R28 Tiled store.** `TiledMapStore` implements T1's `TerrainMapStore` (`read`, `write`, `readBytes`,
  `writeBytes`, `canWrite`, `createMap`, `takeDirty`, `format`, `frame`, `worldOfTexel`, `texelOfWorld`,
  `preferredBlock`, `beginLarge`, `takeUndoHook`) over **global texels**, which this store keeps non-negative
  (T1's signed `TexelRect` allows it): texel 0 is the addressable minimum corner, and `format` reports the whole addressable lattice. A
  write updates every tile holding each sample (up to 4, gutters included) in one call, so a seam can never be
  half-written, and reports dirty rectangles in global texels. Reads across tiles stitch, and a read over an
  absent tile returns `baseHeight` or zero. Brush strokes, `edit_terrain`, Lua edits, spline bakes and
  `generate_terrain` take a World through `entity` in T1's schema, with no other change (D12).
- **R29 Auto tiles.** With **Auto Add Tiles** (on for edits by brush, `edit_terrain` and generation;
  off for Lua), a write over an absent tile creates it, filled with `baseHeight` and with border samples
  copied from any neighbours. Creating is part of the same undo step. An op whose shape reaches past the
  addressable lattice is refused whole by T1's `validateOps` (programme D15), and one that would pass R6's
  bounding box by the store's `canWrite` (T1 R86), before anything commits.
- **R30 Undo.** A brush or op is one undo step, whatever it crosses: T1's `OverlayMapStore` and
  `TerrainMapDeltaCmd` work over the tiled store unchanged, and the command also keeps the store's `takeUndoHook()`,
  which removes and restores the tiles it created (T1 R86). World-scale writes (D11) replace the overlay: the store snapshots the affected tile files (hard
  link or copy under `.jasperengine/trash/`), up to 2 GiB. Past that the call fails unless
  `allow_no_undo: true`, and `dry_run` predicts which applies. Add Tile and Remove Tile are undoable.
- **R31 Persistence.** Dirty tiles stay pinned in the editor's cache, and flush atomically through
  `writeFileAtomic` plus `ScopedExpectedWrite` (S05) on `ioWriteChannel`, at the debounce and the forced flush
  points of T1 R30 (scene save, autosave, Play, export, close, every reader). Play reads flushed files and
  never writes one. An external change of a tile
  (git pull) reloads it. Runtime edits (T1's Lua edits) are RAM only: an edited tile is pinned, at most
  16 per world; past that the edit returns an error naming the cap. Stopping Play discards them.
- **R32 Import.** Import Heightmap slices a 16-bit RAW or R16 file by reading bands, RAM bounded by two
  tile rows, up to the world limits; PNG up to 8,193 per side (it decodes whole). Parameters: origin
  tile, spacing, max height, flip. A job with progress and cancel, writing to a sibling temp folder
  and swapping in on success.
- **R33 Convert.** Convert to Terrain World turns a Terrain into a World at the same place: samples come
  through `sampleTerrainSurface`, so the new lattice lies on the drawn surface, even though single-map
  texels sit at `(i + 0.5)/W`; blend, hole and density maps resample to tile lattices; layers and material
  copy, and the source is disabled, not deleted. Undoable. A rotated or scaled source is refused with a
  clear message. Detail tiling keeps its metres (`terrainSize` and `textureDetailTiles` are rewritten
  so that UVs per metre do not change).

### Performance

- **R34 Budgets** (Release; desktop figures gate on the benchmark run with `JASPER_TEST_PERF_SLACK`,
  mobile and web ones are defaults and a checklist). Per process, across worlds:

  | | Desktop | Android, iOS | Web |
  |---|---|---|---|
  | Largest `tileQuads` | 1024 | 512 | 256 |
  | Pool (resident pages, default) | 64 | 20 | 16 |
  | Page GPU memory cap | 128 MiB | 32 MiB | 16 MiB |
  | CPU tile memory cap | 96 MiB | 32 MiB | 16 MiB |
  | Near tiles drawn per view D | 16 | 6 | 6 |
  | Upload per frame | 8 MiB | 2 MiB | 2 MiB |
  | In-flight loads | 4 | 2 | 2 (1 single-thread) |
  | Near band multiplier (LOD Distance) | 1 | 0.5 | 0.5 |
  | Collision radius | 128 m | 96 m | 64 m |
  | Largest overview side | 4,097 | 2,049 | 2,049 |

  Per tile (*calc*; N = 512, blend at N/2): height 515² x 2 = 0.51 MiB GPU and CPU; blend ≤ 3 x 257² x 4
  = 0.76 MiB GPU. N = 256: 0.13 and 0.19 MiB. Overview at 64 x 64 tiles, A = 16: 2.0 MiB height,
  4.0 MiB macro, plus a 2.0 MiB CPU copy. A world over its budget degrades to overview tiles, never to a
  failure.
- **R35 Main-thread bound.** The sum of the world's pump, selection and upload work on the main thread
  (profiler scope `TerrainWorld`) is ≤ 2 ms per frame on desktop hardware and ≤ 3 ms on mobile and web
  (web single-thread adds the 4 ms pump, a single job overshooting it by at most 4 ms). Measured on this
  host's software GL: ≤ 4 ms at every frame of the fly-through, and the flight's worst frame is at most
  6 ms worse than the same flight with every tile resident up front.
- **R36 Draws.** Near draws per pass ≤ 2·D + 2 (table in D4). Instance buffers total ≤ 0.64 MiB
  (*calc*: 4,096 nodes x 20 B x 2 grids x 4 views) and a view allocates its own on first use. Selection
  of a view ≤ 0.3 ms on desktop at 2,000 visited nodes.
- **R37 Zero cost.** Module compiled out: nothing. Module in but no world in the scene: one empty-array
  check a frame in `TerrainWorldSystem`, one null test per terrain at each of the hooks, no allocation,
  no pass, no draw, no thread. Pixels of ordinary terrain goldens are unchanged. The module adds at most
  200 KiB to the stripped Linux glcore player and 100 KiB gzipped to the web st player (measured by
  `tools/players/player_sizes.py`).

### Platform & backends

- **R38 Web.** Stated plainly: the web player downloads `content.jpak` whole and keeps it in the heap
  (`S30:326`), so **streaming saves no download and no compressed bytes on the web; it saves decompressed
  CPU memory and GPU memory only**. So web limits are firm: `tileQuads` ≤ 256, overview side ≤ 2,049
  (so ≤ 128 tiles a side), and the world's compressed tiles ≤ 96 MiB, warned from 64 MiB and refused by the
  export above that. At about 80 KB a tile (*assumed*; AC30 measures it) that is roughly 1,200 tiles,
  about 35 x 35 tiles, 9 km a side at 1 m. Single-thread works (AC21), with loads spread by the 4 ms pump.
- **R39 Backends and targets.** Everything works on GL 3.3, GLES3/WebGL2, Metal, D3D11, Vulkan and on
  all six targets, with no compute, SSBO, bindless, indirect draw, integer texture or base instance.
  Instance slices bind by byte offset. New shader variants reach the export collector through
  `getSubmeshShaderInputs`, and the variant bit is never dropped by a fallback, because it changes a
  uniform block (`engine/core/pool/ShaderPool.h:75-78`).

### Editor UX (human)

- **R40 Creating.** GameObject › 3D Object › Terrain World opens a dialog (Tile Size, Spacing, Max
  Height, Grid Width, Grid Length, Base Height, asset path) and makes the entity, the Rigidbody and the
  asset, one undo step. Terrain inspector: **Convert to Terrain World…**. On a World the Terrain
  inspector hides Height Map, Size, Max Height and LOD rows.
- **R41 Terrain World panel** (Window › Terrain World, new file): a **minimap** of tile cells coloured
  by state (absent, cold, loading, resident, pinned, error), focus markers, the camera's frustum,
  click to frame; **Add Tile** and **Remove Tile** by clicking cells; **Import Heightmap…**;
  **Rebuild Overview**; **Repair Seams**; settings (Pool Size, LOD Distance, Upload Budget, Collision
  Radius, Auto Add Tiles); a live info block (resident, GPU and CPU memory against caps, queue, uploads
  this frame, draws, collision bodies fine and coarse, worst main-thread ms). Everything is undoable that
  changes data. Tooltips say what each state means.
- **R42 Overlay.** A Scene view overlay **Terrain World** with Tile Borders, Tile State, LOD Level and
  Collision Chunks. Works in Play. Editing tools work unchanged: the brush decal crosses borders, and
  a tile under a brush is pinned on hover.

### Agent UX (MCP, Lua, CLI)

- **R43 Creating, by `edit_terrain_world {create}`** (group `terrain`, D17 of the programme), merged rather than a `create_terrain_world` tool (T1 R95).
  `create` takes `name`, `asset_path`, `tile_quads`, `spacing`, `max_height`, `base_height`, one of
  `grid: {width, length, origin}` or `from_terrain: <entity>`; the handler validates them. With `dry_run`, `wait`, `timeout_seconds` beside it.
  One undo step; returns entity, GUID, tile count, predicted memory. A job past 1 s. `create` and `ops` may be one call, as `edit_graph` allows.
- **R44 `edit_terrain_world`.** `entity`, `ops[]` validated all-or-nothing first (programme D15):
  `add_tiles`, `remove_tiles`, `import_heightmap`, `repair_seams`, `rebuild_overview`, `set_streaming`
  (pool, LOD distance, upload budget, `freeze`), `set_focus`, `remove_focus`, `set_collision`, plus
  `dry_run`, `wait`, `timeout_seconds`. Returns changed tiles and warnings. In Play only `set_streaming`,
  `set_focus`, `remove_focus` run; the others refuse by name.
- **R45 Status, by `sample_terrain {world}`**, merged rather than a `get_terrain_world_status` tool (T1 R95). `world: {tiles?, wait_for?: {x, z, radius, level}}` with
  `timeout_seconds` returns `world.tile_counts`, per-tile `state`, `gpu_bytes`, `cpu_bytes`, budgets, `queue`, `in_flight`, `uploads` (bytes last frame, total, retries),
  `main_thread_ms` (last, worst), `draws` per view and pass, `nodes` (visited, selected), `collision`
  (fine, coarse, frozen), `seam_errors`, `pool_exhausted`, `late_pages`, `errors`. `wait_for` makes a job
  that ends on R18's condition, or fails at timeout with the tiles that were missing, with `next` in `structuredContent` naming
  `set_streaming {freeze: true}` when screenshots are the goal. Together with `freeze` this is the deterministic test wait: no load, eviction or upload runs
  after it returns. On a plain Terrain `world` is an error that names `edit_terrain_world {create}`.
- **R46 Existing tools.** `edit_terrain`, `sample_terrain` and `visualize_terrain` take a World through
  `entity`, in T1's schema (T1's seam). `sample_terrain` gains `state` per point (R19); `visualize_terrain`
  gains overlays `tiles`, `tile_state`, `lod`, `collision`. The group's limits are T1 R74's. T4 adds one tool, `edit_terrain_world`, at most
  1,500 characters with no `exceptions` entry (884 by script: `ops` items and `create` are open in the schema and closed in the handler, T1 R95; its description
  names the ops, not their fields). It is not folded into `edit_terrain` because its streaming ops run in Play and `edit_terrain` does not. Its name follows the gated `verb_noun`
  rule (`docs/mcp/conventions.md`), not `AGENTS.md`'s "area noun first". Guidance goes in `docs/mcp/guide/terrain.md` under a `Terrain Worlds` heading, with no agent
  guide line. Tests prove the tool is hidden until `tool_groups` enables `terrain`.
- **R47 Lua.** Class `TerrainWorld` (bound under `#if JASPERENGINE_TERRAIN_WORLD`), got by
  `go:getTerrainWorld()` or `TerrainWorld(scene, entity)`: `addFocus`, `setFocus`, `removeFocus`,
  `getSurfaceAt` (height, normal, hole, exact, present), `getTileState`, `isAreaReady`, `waitForArea`,
  `onAreaReady`, `ensureCollisionAt`, `isReady`, `getStats`, properties `lodDistance`, `poolSize`,
  `uploadBudget`, `collisionRadius`. Documented with Lua blocks the doc test runs. T1's `Terrain` queries and
  `applyOps` work on a World (R24, R31); `Terrain`'s map setters warn. Lua names are read by module detection through the bindings' conditions.

### Data & compatibility

- **R48 Asset.** `Name.terrainworld/` is one asset of a new type `terrain_world`, `AssetType::TerrainWorld = 31` (after upstream's ProceduralMaterial took 29; stored in `assetmap.bin`), in `AssetTypes`, with
  in-file identity: `manifest.yaml` holds `formatVersion`, `guid`, the lattice, `maxHeight`, maps'
  lattices, `hasHoles` and `layerHash`. The scan does not enter the folder, so there is no `.meta` per tile; the
  Project window shows one icon; moving, renaming and S04 redirects treat it as one asset. **The tile set
  is the set of files present**, not a list, so adding tiles on two branches never conflicts.
- **R49 `.jtile`.** Own version header, little-endian (D7), with sections each zstd-compressed and
  checked. Unknown sections are preserved on rewrite. A newer major version is refused: the tile is
  `Error`, and saving is blocked with a message as in S03 R23. Migrations are a table in
  `engine/core/terrain/world/TerrainTileFormat.cpp`, with a fixture per version. The same input and zstd
  version give identical bytes. Corrupt or truncated input yields `Error`, never a crash.
- **R50 Version control.** `*.jtile` joins `versionControlBinaryPatterns` (`editor/Project.cpp:6592`), so
  it is binary or LFS and lockable according to the project's setting (S29 R21). `manifest.yaml` is text
  already, and T5 adds its `.jetrees` pattern. A merge is per tile; a border edit touches both tiles.
- **R51 Seams.** Loading a tile checks its borders against resident neighbours and counts mismatches in
  `seam_errors`, with one warning. **Repair Seams** resolves in favour of the tile with the lower (x, z).
  The cook repairs a copy and reports each repair, and the `check` command reports them too.
- **R52 Overview persistence.** Every tile carries its own decimated samples and macro colour. The
  editor assembles the overview into `.jasperengine/cache/terrainworld/<guid>/`, not versioned, using a
  stored fingerprint of the tile list, rebuilding only changed tiles. Macro colour is the blend-weighted
  mean of each layer's colour, and is rebuilt when `layerHash` changes. The overview refreshes at stroke
  end, debounced 500 ms, and at most once a second in Play after runtime edits, because
  `sg_update_image` cannot patch part of it.
- **R53 Cook.** The export ships a referenced world into the pack as `world.bin` (a binary manifest with
  occupancy and tile bounds, because the player has no YAML path), `overview.jtile` and one entry for each
  `tiles/x_z.jtile`, with editor-only sections stripped, recompressed. Both Prebuilt and Compile builds
  carry it. Runtime code reads through one `TerrainWorldSource` interface: `CookedWorldSource` in a player,
  and the editor's source over the project folder in Play. The component gets a runtime codec and encoder (`S30:174-178`, `RuntimeFields.gen.inc`,
  `ComponentHooks.cpp:152-158`) and a `Factory` path. Same input, same pack bytes.
- **R54 Old projects.** New optional component, no scene version bump (S03 R22). The new type and pattern
  change nothing for projects with no world. Existing `.gitattributes` get the pattern the way S29 updates
  them.
- **R55 Module.** `terrainworld`, `JASPERENGINE_TERRAIN_WORLD`, default ON. Touches: `allModules`,
  `moduleDisplayName`, `engine/CMakeLists.txt` option and definition, `JasperPlayerVariants.cmake`,
  `variants.json` (in `full`, `ui` and `core`; **not** `lite3d` or `lite2d`), `Exporter.cpp:3595` and `:4611`
  with its source exclusions,
  `ModuleUsage` (a Terrain World on a GameObject, the Lua names). A player without it refuses content
  that needs it, by S34 R3. No new library.

### Upstream revision: no CI, lean MCP, writes

- **R56 Where each AC runs.** There is no CI (`AGENTS.md`, Test): the gate per commit is `tools/check-changed`, and the full suite runs before the push. Each AC runs on this host
  or is labelled not verifiable here with the command a person runs (§7's table). A row not run is reported unverified.
- **R57 Writes.** Every persistent write goes through `engine/core/io/AtomicFile.h`: tiles and `manifest.yaml` (R31), the import's temp-folder tiles (R32, then a directory rename), the overview
  cache with `Durability` for regenerable output (R52), and the tile undo snapshot (R30, hard link or `publishFile`; a `copy_file` without overwrite is not flagged). So `tools/raw_writes_allowlist.txt` gets no line, and
  `python3 tools/check_raw_writes.py` proves it. `world.bin` goes through the existing pack writer. Nothing is vendored: `NOTICE.md` and `engine/notices.json` do not change.
- **R58 Shaders.** Each commit that changes a shader (`terrain_world_vs.glsl`, `HAS_TERRAIN_WORLD` in `terrain_vs.glsl`) bumps the shader cache version in `editor/App.cpp` by one over its `main`. `Twd` takes the next
  free bit of MESH, DEPTH and GBUFFER at landing (T3 D2; T4 lands last, so after all of ours), pinned by `ShaderKey.TerrainWorldBitIsPinned`.
- **R59 Eval cases.** The tool, the `world` key and the guide text get cases that fail without them (AC45).

## 6. Design constraints & decisions

**D1 Layout (programme D1, D2).**
```
engine/core/terrain/world/        pure, no ECS, no render, namespace jasperengine::terrain
  WorldLattice.h                  sample/tile/node arithmetic; triangle and bilerp kernels
  TerrainTileFormat.{h,cpp}       .jtile read/write, predictor, versions, migrations
  TerrainWorldSelect.{h,cpp}      the selection of D4 as a pure function
  TileStreamer.{h,cpp}            states, queue, LRU, budgets; clock and I/O injected
  TiledMapStore.{h,cpp}           TerrainMapStore over a tile cache
engine/core/component/TerrainWorldComponent.h
engine/core/subsystem/TerrainWorldSystem.{h,cpp}     owns runtime, pump, focuses, signals
engine/core/subsystem/RenderSystemTerrainWorld.cpp   draw items and uploads (RenderSystemShadows.cpp is the precedent)
engine/core/physics/TerrainWorldCollision.{h,cpp}    chunks, coarse tiles, subjects
engine/core/object/TerrainWorld.{h,cpp}              the Lua-facing wrapper
engine/shaderlib/includes/terrain_world_vs.glsl      height fetch, morph, skirts, varyings
editor/terrainworld/                                 panel, overlay, import, convert, cook source
editor/cook/TerrainWorldCooker.{h,cpp}
editor/mcp/tools/TerrainWorldTools.cpp
```
`engine/core/terrain/world/` obeys T1 R1's `TerrainOpsDeps.NoEcsOrRenderIncludes` (`math/`, `jobs/`, the standard library): zstd and
file reads come in as injected functions, supplied by `TerrainWorldSystem`. `TileFoliageRegion` (R27) lives in
the subsystem layer.

Hooks in the hot files, nothing else:
- `RenderSystem.cpp`: at the top of `updateTerrain` (`:12820`), one branch; at the geometry draw of
  `drawMesh`, `drawMeshDepth`, `drawMeshGBuffer` (`:8973`, `:9141`, `:9951`), one branch to
  `drawTerrainWorldItems`; `loadMesh`'s terrain buffer sizing (`:6232`, and capacity `:17421`);
  `getSubmeshShaderInputs` (`:3622`), one input.
- `MeshSystem.cpp`: `createTerrain` (`:3671`) builds the skirted grids and no node tree for a World;
  `raycastTerrainSurface` (`:3387`, T1's forwarder) gets one branch.
- `engine/core/object/TerrainEdit.h`: `querySurface` gets the one branch T1 R14 reserves.
- `Engine.cpp`: one call after the pump (`:2231`).
- `InspectorWindow.cpp`, `TerrainEditWindow.cpp`: one call each to code in `editor/terrainworld/`.
- `TerrainComponent.h`: a runtime-only `TerrainWorldRuntime* world = nullptr`, reset on clone.

**D2 Why a World is a Terrain plus a component.** Terrain's layers, detail array, T3's material path and
every `terrain` branch of the three draw paths already do what a World needs. A separate draw path would
fork them all, against programme D2. Unused Terrain fields are hidden, not removed.

**D3 Height format (the brief's question, answered).** The brief's candidates were R16 where filterable
and R32F elsewhere. Reading sokol says **RG8 everywhere is better**, and it is what this spec requires:

| | R16 | R32F | R16F | RG8 + `texelFetch` |
|---|---|---|---|---|
| GLES3 / WebGL2 | none (`:9952`) | sample only without extension (`:10039`) | yes | yes (`:9958`) |
| Bytes per texel | 2 | 4 | 2 | 2 |
| Step at 1,000 m range (*calc*) | 15 mm | under 0.1 mm, but data is 16 bit | 490 mm | 15 mm |
| Upload conversion | on creation only (trap) | yes | yes | none |
| Works on every backend alike | no | no | no | **yes** |

A hardware filter gives no seam guarantee: at a shared border two tiles sample different textures at
uv computed in float, and any filter weight other than exactly 1 at a border mixes in a neighbour
(*assumed*, filter fraction precision is not specified to be exact). `texelFetch` at integer coordinates
with weight exactly 0 for a neighbour gives bit-identical heights. T1's `sampleSurface` is the same
bilinear on the same samples. Decode: `(round(rg.r·255)·256 + round(rg.g·255))/65535` in highp.
`GpuTextureFormat::RG8` already reaches `createTextureLevels`; the shared nearest sampler is `getDataSampler`. Per vertex: 1 fetch for the
pre-morph height (unmorphed vertices are always on the texel lattice, since vertex spacing is h·2^L), 4
for the final bilerp, 4 nearest for the normal at `pos ± delta` rounded to texels.

**D4 Selection across tiles (the core of the spec).** Node level L has size `R·h·2^L`, position dyadic,
on one quadtree aligned so that level T nodes are exactly tiles; 4 roots cover the signed grid. Nodes are
implicit; nothing is built per world. Range `r_L = 2·size_L·lodDistance`; the last level reaches the
camera's far clip, as `MeshSystem.cpp:4314-4318` does. Bounds come from a world min/max pyramid above T
(dense, from `world.bin` or scanned tile headers) and from each resident tile's min/max pyramid at or
below it. Then:
```
visit(node, L):                                // terrainNodeLODSelect's rule, implicit
    if unoccupied(node): return true           // absent tiles are skipped
    if !inSphere(eye, r_L, bounds): return false
    if !inFrustum(bounds): return true
    if L > T:
        if partlyOccupied(node): recurse children, each forced  (emit L==T level nodes as overview half grid, range=inf)
        elif !inSphere(eye, r_(L-1), bounds): emit overview full grid
        else for child: if !visit(child): emit child in overview half grid, parent's range
        return true
    if L == T:
        if pageDrawable(tile) and tileDrawCount < D: near walk below, into this tile's draw list
        else: emit tile as overview half grid, range = inf (no morph, lattice stable)
        return true
    near walk (L < T): ordinary rule, full or half grid, into this tile's draw list
```
A near draw list never spans two textures, by construction, because no node below T crosses a tile. An
overview node at level ≥ T has vertex spacing ≥ S/16 = the overview's stride at A = 16, so its vertices
are exactly stored samples and agree with the tile's own. Tiles are taken nearest first against D;
those past it fall back. Morph remains a function of eye and position only. So across a border at equal
LOD both sides compute the same morph from the same inputs.

| | Today | World |
|---|---|---|
| Node visits | every node, every selection (`:12822`) | only nodes in range or on the frustum; not world size |
| Grids | 2 instanced per pass | near: ≤ 2 per drawn tile; overview: ≤ 2 |
| Draws per pass, desktop D = 16 | 2 | ≤ 34; a typical view fills one grid of many tiles |
| Draws per pass, mobile and web D = 6 | 2 | ≤ 14 |
| Shadow pass | T3 R17's shadow cut, or the view's | the same: selection re-run with T3's shadow-camera test; near items only by default, ≤ 32 / ≤ 12 |
| Views | 4 | 4, same fallback to view 0 |
| Instance buffers | sized by all nodes, 8 of them, at load | 2 x 4,096 nodes per view used |
| Texture slots | 16 fragment when painted (`S11:87`) | **0 added in the fragment stage**; +1 vertex (macro) |

The brief's "one draw" for the distance becomes at most 2 because CDLOD needs the full and half grids;
both are instanced and both use the one texture. Variant bit `Twd` (`HAS_TERRAIN_WORLD`): next free on MESH, DEPTH and GBUFFER at landing (R58, T3 D2's table). It adds fields to `u_vs_terrainParams` under its define, so there is
no new `UniformBlockType` and draws upload the wider block.

**D5 Cracks.** Skirts, not only morph, because streaming breaks CDLOD's one-level assumption whenever a
tile is late, and godot_voxel's own cracks depend on load order (`priority_dependency.cpp:47-50`). World
grids carry an outer ring of 4·(R+1) vertices, flagged in `a_position.y`. A ring vertex takes the
position, height and normal of its edge vertex and drops by `skirtDepth`, but only when the edge lies on
a tile border (exact test: dyadic coordinates); otherwise it collapses onto the edge, a degenerate
triangle. `skirtDepth` is the world's largest difference between a tile's edge and its overview lattice,
stored in tile headers and maintained on edits, at least 1 m. Vertex cost +12% on the full grid, +24% on
the half (*calc*), rasterised only on tile borders. T3 R20's hole test uses the interpolated local XZ, which
a skirt vertex shares with its edge, so a hole at a border also removes its skirt. A curtain of `skirtDepth` shows at the world's outer edge when
seen from outside; accepted.

Coordinates are exact: S is `N·h` with both powers of two, and node positions, sizes and grid offsets are
dyadic well inside float32's 24 bits (at h = 0.25 and ±16,384 m: 2^16 steps). The world has a single
model matrix, and vertices are computed in world-local space, not with a per-tile matrix, whose product
with the view matrix would round differently per tile and open sub-pixel cracks far from the origin.

**D6 Pages.** Immutable images on GL, GLES3, Metal and D3D11 because a stream image costs two slots on GL
and Metal (`sokol_gfx.h:8227,11002`) and those include the tightest platforms. On Vulkan, `stream_update`
images reused by a pool, because `sg_make_image` with data there waits for the queue
(`:19932,21188`) and `dynamic_update` too (`:20027`), a hitch on every tile. Both hide behind
`TextureRender::createTerrainPage` and `updateTerrainPage`, so `RenderSystemTerrainWorld.cpp` has no
backend branches. All page work runs between frames' passes, in the upload step of `RenderSystem`
(before the first pass; `updateTerrainNodesBuffer`'s rule, `:8585-8589`). The overview is always immutable
and is re-made when it changes. Page GPU accounting uses `JE_GPU_ALLOC_SCOPE(Terrain, ...)`.

**D7 `.jtile` layout.**
```
"JTIL\0\0\0\0", u32 version=1, i32 tileX, i32 tileZ, u32 tileQuads, f32 spacing, u32 sectionCount, u32 flags
section[sectionCount]: u32 id, u32 index, u32 codec (0 raw, 1 zstd), u32 filter (0 none, 1 predictor16),
                       u64 offset, u64 storedBytes, u64 rawBytes, u64 fnv1a64 (of the raw bytes)
blobs, 16-byte aligned, in table order
```
Sections: `HGHT` (u16 (N+3)², predictor left+up-upleft then split byte planes), `BLND`[index], `HOLE`,
`DENS`[index], `OVRH` (u16 (A+1)²), `OVRC` (RGBA8 (A+1)²), `MNMX` (u16 min/max pyramid), `EDGE` (the
tile's `skirtDepth`). `BLND`, `HOLE` and `DENS` carry a small lattice prefix; their meaning belongs to
T3 and T5. zstd level 9 in the editor, 19 in the cook; players have only the decompressor, and this
matches `engine/CMakeLists.txt:1142-1146`. Per-section checksums are what lets `Error` be per tile on
all paths, including a loose file.

**D8 No range reads.** `JPak` has no range API, and `RangeReader` serves only the legacy pack (section 3).
One entry per tile, read whole, is cheap enough (a tile is 100-400 KB compressed, *assumed*) and needs no
change to the pack. `AssetIOService` already reads pack entries through `FileData`. The editor uses the
ranged form on loose files to read only `OVRH` and `OVRC` when assembling the overview.

**D9 Pump placement.** `TerrainWorldSystem::pump()` runs from `Engine::runFrame` right after the job pump,
once per `FrameTimes` frame, because editor views re-enter `systemDraw`. There it: polls tasks with
`Task::isReady`, re-sorts on its 100 ms tick, adds and removes physics bodies (the physics step comes later
in the frame on this thread), and hands finished CPU data to the render upload step. It runs when paused,
and when `draw` is false (headless). It does not touch Lua; `onAreaReady` fires from `update`, so a paused
game gets it on resume. When a tile's drawability changes it sets the World's `transform.needUpdate`, so
that selection re-runs (`RenderSystem.cpp:15100` otherwise would not).

**D10 Startup.** The overview and `world.bin` load as `Tag::Asset` jobs, so `getAssetJobCount` and the
load-settled wait include them (*inferred* from `JobSystem.h`; AC6 checks). The initial focus's tile goes
in `Critical`. Scene load does not wait for pages. A game wanting ground under its player waits with R18.

**D11 World-scale operations.** T1's overlay holds floats per touched 64 x 64 block, which is fine for a
brush and impossible for a world: 16,384² is 1 GiB of floats. So an op or generation whose rectangle exceeds
`largeEditTexels` (default 2,048²) runs tile by tile: T1's `beginLarge(map, rect)` returns
the undo token of R30 and `preferredBlock(map)` gives the tile rectangle; T1's `applyOps` (T1 R87) and generation apply the op per block,
with padding as T1 and T2 already read, straight to the store. The single-map store answers the whole map and a
null token, which is today's path, so single terrains see no change.

**D12 Texel mapping: T1's per-map conversion.** Ops turn texels into positions only through `worldOfTexel` and
`texelOfWorld`, per `MapId` (T1 R86). A world's height lattice has W = 2M + 1 samples, and `worldOfTexel` answers
`(i - M)·h`, the value a single map's `(i+0.5)/W` gives over a centred frame of size `W·h`: exactly the dyadic
lattice, tile (0, 0)'s corner at local (0, 0). Global texels are `sample + M`,
never negative by this store's choice, and T2 R23's global starts stay meaningful. The area maps fit too: `W = 2M`, size
`W·h`, centres at half-steps. Each map's frame differs, which is why the conversion takes the `MapId`; T1's
`frame()` holds only `maxHeight`, which is world-wide (R2).

**D13 Precision and origin (recorded decision).** float32 ULP at distance (*calc*):

| Half extent | ULP | Half-ULP error | Verdict |
|---|---|---|---|
| 4,096 m | 0.49 mm | 0.24 mm | clean |
| 8,192 m | 0.98 mm | 0.49 mm | editor warns past here |
| **16,384 m** | **1.95 mm** | **0.98 mm** | **hard limit** |
| 32,768 m | 3.9 mm | 1.95 mm | refused |

After matrix products expect about 3 ULP of camera-relative jitter, 6 mm at the limit (*assumed*): about a
pixel on an object 1 m from the camera at 1080p, invisible at 10 m and on terrain, whose lattice is exact.
Physics at the limit keeps 10 ULP inside Jolt's 2 cm slop (*assumed*; AC24 tests a box at rest there).
**No floating origin**, because it is not cheap: grep finds none, nor camera-relative rendering; matrices
are built with world positions in `updateMVP`, Jolt is single precision, and a shift must reach transforms,
bodies, particles, navigation, audio and scripts' stored positions. A later spec can add it; nothing here
blocks that, because worlds use one matrix and local coordinates.

**D14 Near-to-far fade and UVs.** Near draws read macro colour in the vertex stage (+1 vertex texture, no
fragment slot) and pass `v_terrainMacro` (rgb, fade), with fade rising over the last 25% of the near band. T4
adds one line under `HAS_TERRAIN_WORLD` where T3's fragment path has resolved its layers; T3 needs no action,
and this is the only fragment-stage change here (programme D7 keeps the rest with T3). Overview draws use the
existing no-layers path, with the macro texture as base colour and uv1 from the overview transform. Detail UVs
stay world-absolute, and T3's Noise macro uses local XZ, so hex and stochastic tiling and tints show no seam at
borders. Blend UVs are tile-local, `((p - origin)/S · N_b + 0.5)/(N_b + 1)`, and the hole's `u` is
`(p - origin)/S`. `Thl` is per World (`hasHoles`), not per tile as T3 suggested: a tile with no holes binds a
shared solid texel, because a per-tile variant would double each draw's render objects for a cost T3 budgets
at +3%.

**D15 Navigation (recorded decision).** No per-tile navmesh pages in this spec. S19's Surface stores one
baked file and polys are limited by 32-bit refs (`S19` R6), and scene obstacles belong to the scene. A
region bake (Volume) plus Follow covers open worlds with AI near the player. A later spec can add pages.

**D16 Why not `MainThreadQueue` or per-chunk entities.** The first stops while paused (section 3). The second
costs a GameObject per chunk and main-thread `createRigidbody` per slide; a paged body needs no component of
its own, since `packBodyUserData(worldEntity, mask)` plus the World's Rigidbody serves contact callbacks.

**API sketches.**
```cpp
// engine/core/terrain/world/ (pure)
struct WorldLattice { int tileQuads; float spacing; int gridResolution = 32; int overviewStride = 16; };
struct TileCoord { int16_t x, z; };
enum class TileState : uint8_t { Absent, Cold, Queued, Loading, CpuReady, Uploading, Resident, Error };

// T1's interface, our implementation; global texels (D12)
class TiledMapStore final : public TerrainMapStore {
    MapFormat format(MapId map) const override;               // the whole addressable lattice
    bool read(MapId map, TexelRect rect, std::span<float> out) const override;
    WriteResult write(MapId map, TexelRect rect, std::span<const float> in) override;  // every copy of each sample
    // ...readBytes, writeBytes, canWrite, createMap, takeDirty, as T1
    Vector2 worldOfTexel(MapId map, int x, int y) const override;             // D12
    Vector2 texelOfWorld(MapId map, float localX, float localZ) const override;
    TexelRect preferredBlock(MapId map) const override;                       // the tile rectangle
    std::unique_ptr<TerrainLargeEdit> beginLarge(MapId map, TexelRect rect) override;
    std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() override;            // the tiles created since the last take
};
```
```lua
local world = self.gameObject:getTerrainWorld()
world:setFocus(self.focus, player.position)

Coroutine.start(function()                       -- teleport with no fall-through
    world:ensureCollisionAt(dest.x, dest.z)
    local ok = world:waitForArea(dest.x, dest.z, 64, "collision", 5)
    if ok then player.position = dest end
end)
local s = world:getSurfaceAt(x, z)               -- s.exact false: from the overview
local y = Terrain(scene, self.entity):getHeightAt(x, z)  -- T1's query works on a World
```

**Build order.** (1) Pure core: lattice, format, select, streamer. (2) Tiled store plus editor asset.
(3) Render: pages, overview, shaders, skirts. (4) Streaming end to end, then the fly-through. (5) Collision.
(6) Module, cook, web. (7) Panel, tools, Lua, navigation. Each step lands with its tests.

## 7. Acceptance criteria

Unit suites: `jasperengine-core-tests` (`engine/tests/core/terrain/`, GPU in `engine/tests/core/render/`),
`jasperengine-editor-tests` (`editor/tests/terrainworld/`; physics through the editor's shared engine,
`editor/tests/engine/`). MCP: `tools/mcp/tests/test_terrain_world.py` (`CI = True`, own project). Fixture:
`tools/fixtures/terrain-world/` generator (sum of integer sines: deterministic, no noise library needed).
"Red" says the control that must fail without the thing tested.

| AC | R | Verification |
|---|---|---|
| AC1 | R2, R3 | `WorldLattice.*`: sample, tile and node arithmetic round-trips; every coordinate is exact to the last bit at ±16,384 m for all legal N, h (`std::bit_cast` compare). |
| AC2 | R7 | `TerrainWorldSelect.DoesNotScaleWithWorld`: identical eye over filled 8², 64², 256² worlds: visited-node count within R7's bound. Count-based, no clock. Red: a variant visiting every tile fails. |
| AC3 | R7, R8 | `TerrainWorldSelect.NodeNeverCrossesTile`, `CoversEachPointOnce` (rasterised coverage on a CPU grid: every occupied point in exactly one selected node, at every residency state of a checkerboard). |
| AC4 | R36 | `TerrainWorldSelect.DrawBound`: draw items ≤ 2·D + 2 at D = 16 and 6, over 1,000 random eyes; `TerrainWorldGpu.DrawCountMatches`: `getStats` draw calls equal items per pass. |
| AC5 | R9, R10 | `TerrainWorldSelect.FallsBackWhenNotResident`: an absent page, an error tile and past-D tiles all emit level T overview half-grid nodes with infinite range. |
| AC6 | R10 | `TerrainWorldGpu.OverviewFirstFrame`: pages blocked by a test hook; camera over the world: frame fully covered once `isReady()`; load-settled wait covers it. Red: no fallback leaves clear colour visible. |
| AC7 | R12 | `TerrainWorldGpu.HeightMatchesKernel` on glcore, Vulkan: silhouette camera (orthographic, along X, 1,024 px over 100 m) over a known column: drawn top within one pixel of `WorldLattice` height at 50 columns including borders. |
| AC8 | R11 | `TerrainWorldGpu.NoCracksAnyResidency`: camera pitched down so that terrain fills the frame, background magenta, unlit: **zero magenta pixels** in each frame, over borders and a 4-tile corner, with tiles resident in checkerboard, all, none, and one-of-four states, at the origin and at ±16,000 m. **Red: the same test with skirts disabled by the test hook counts > 0 magenta pixels** on the checkerboard. |
| AC9 | R3, R11 | `WorldLattice.BorderNormalsMatch`, `BorderHeightsBitIdentical`: CPU mirror of the shader kernels on both sides of each border of random tiles, compared bit for bit. Red: omit the gutter and normals differ. |
| AC10 | R11 | Goldens made on glcore, matched on Vulkan: `TerrainWorldGpu.Golden` `terrain_world_near`, `terrain_world_corner` (4 tiles, lit, layers), `terrain_world_overview` (2 km altitude), each after wait and freeze. |
| AC11 | R13 | `TerrainWorldGpu.UploadBudget`: bytes per frame ≤ budget except a lone over-budget image; `UploadRefusalRetries` (Vulkan, small stream budget set through `GpuResourceLimits`): refusals counted, tile draws only after retry, no missing page, no error log. Red: ignoring the return value leaves a tile black. |
| AC12 | R35, R36, R13 | **`TerrainWorldGpu.FlyThrough`, the hard test.** 8 x 8 tiles, N = 256, canvas 640 x 360, 1,200 frames at 2 m a frame at 80 m altitude on a fixed path, pool 40. Asserts: ≥ 20 tiles loaded, ≥ 10 evicted (it really streamed), zero magenta at each frame (with a look-down camera path), `late_pages` = 0, profiler scope `TerrainWorld` max ≤ 4 ms, worst frame ≤ all-resident control's worst + 6 ms. Prints `[bench]` lines; clocks enforce with `JASPER_TEST_PERF_SLACK` (`docs/testing.md:514-525`), on an otherwise idle host, the count assertions always. |
| AC13 | R37 | `TerrainWorldZeroCost.*`: scene with a plain Terrain, 10,000 frames of `TerrainWorldSystem`: zero allocations (`AllocationCountScope`), mean ≤ 1 µs; draw calls and existing terrain goldens unchanged; `tools/test-projects/terrain` web gate still passes. |
| AC14 | R16 | `TileStreamer.PriorityByDistance`, `Reprioritises`, `CancelsOutsideRadius` (injected clock and I/O): order, cancel, `maxInFlight`. |
| AC15 | R14 | `TileStreamer.CancelledTileReturns`: a cancelled tile demanded again loads (the godot_voxel hole). `ReadFailureRetriesThenError`. Red: no re-queue leaves the tile `Cold` for good. |
| AC16 | R8, R34 | `TileStreamer.LruEvicts`, `NeverEvictsFocusOrPinned`, `RespectsMemoryCaps`, `PoolExhaustedFallsBack`. |
| AC17 | R15, R18 | `TerrainWorldStreaming.MultiFocus`, `NoCameraFocus` (headless: `CpuReady`, no GPU calls, by a counting render stub), `AreaReadyWaits`, `OnAreaReadyWhilePausedFiresOnResume`. |
| AC18 | R17 | `TerrainWorldStreaming.PausedStillLoads`: paused engine, focus moved: tile reaches `Resident`. Red: delivering through `MainThreadQueue` hangs the test at its timeout. |
| AC19 | R17 | `TerrainWorldStreaming.ZeroWorkers`: `JE_JOB_WORKERS=0 JE_ASSET_IO_THREADS=0`: tiles load through the pump; no job over 4 ms at N = 256 (`[bench]`); no thread created (count via `/proc/self/task`). |
| AC20 | R17 | TSan: `SANITIZE=thread tools/build.sh unit-tests` with `UNIT_TEST_ARGS=--gtest_filter=TerrainWorldStreaming.*:TileStreamer.*` clean; ASan and UBSan over format and store. |
| AC21 | R38, R39 | `tools/test-projects/terrain-world`, `GATE=terrain-world tools/build.sh web-gate` (`tools/web/gates/terrain-world.json`): WebGL2, single-thread: canvas centre is lit ground through a page; console proves a tile went `Resident` and that RG8 heights arrived; `GATE=terrain-world-threads` for mt. |
| AC22 | R20, R22 | `TerrainWorldCollision.NoFallThrough`: tile I/O delayed 2 s by a hook; a dynamic sphere dropped 5 m over a `Cold` tile rests on the coarse collider; after arrival it rests on the fine surface within 1 cm + one quantum, no drop in between. Character Controller too. **Red: coarse colliders off, it falls past -100 m.** |
| AC23 | R23 | `TerrainWorldCollision.MatchesQuery`: rays at 1,000 lattice points: Jolt hit, `TerrainWorldQuery::raycast` and `querySurface` agree (R23's tolerance) on both sides of a chunk and tile border; off the lattice they differ by at most the quad's twist/4. `DiagonalMatchesMesh` (red: flip the mesh's diagonal). `HolesAgree`: T3 R22's rim across a chunk border and a tile border, solid when the neighbour is not resident. `RuntimeEditRebuildsChunks`. |
| AC24 | R6, R21 | `TerrainWorldCollision.AtWorldEdge`: box at rest at (16,000, 16,000) for 10 s moves < 1 cm. `FrozenPastCap`; `BatchAddsOnly`: Jolt batch count, no `AddBody` per chunk. |
| AC25 | R24, R19 | `TerrainWorldQuery.*`: equals T1's `sampleSurface` over the same samples, across borders; `exact` and `present` on resident, overview and absent; main-thread call never waits (time bound, hook delaying I/O); `MeshRaycast.Kinds`' terrain case passes on a World. |
| AC26 | R28, R29 | `TiledMapStore.WritesEveryCopy`, `ReadStitches`, `RefusedWholeByValidateOps`, `DirtyRectsAndTilesReported`: 4-tile corner writes leave all copies and gutters equal. `TiledMapStore.WorldOfTexelOnLattice`: T1's `worldOfTexel` lands exactly on the lattice for each map kind (D12). T1's `TerrainOps` golden hashes pass on a `TiledMapStore` of one tile's size. Red: skipping gutter copies fails `BorderNormalsMatch` after an edit. |
| AC27 | R28-R30 | `test_terrain_world.py::test_brush_across_border`: `sculpt_terrain` and `edit_terrain` strokes over a border and off the grid: new tile, `sample_terrain` equal on both sides, one undo restores files bytes-identical and removes the tile. `TiledMapStore.LargeEditSnapshots`, `DryRunPredictsNoUndo`. |
| AC28 | R31 | `TerrainWorldAsset.AtomicFlush` (kill between write and rename leaves the old file), `FlushesBeforePlay`, `ExternalChangeReloads`, no echo through the S05 ledger; `PlayNeverWrites`. `RuntimeEditPinnedAndCapped`. |
| AC29 | R32, R33 | `TerrainWorldImport.SlicesExactly`: 2,049² RAW: reassembled samples equal, RAM by `AllocationCounter` bounded by two tile rows; cancel leaves the folder untouched. `TerrainWorldConvert.SurfaceMatches`: heights sampled before and after within one quantum at 1,000 points; source disabled; undo restores. |
| AC30 | R38, R34 | `TerrainTileFormat.CompressedSizeBudget`: fixture tile N = 256 ≤ 120 KB (prints the real figure); `TerrainWorldCook.WebLimits`: over 96 MiB, N = 512 refused for the web with the limit named. |
| AC31 | R49 | `TerrainTileFormat.RoundTrip`, `DeterministicBytes`, `PreservesUnknownSections`, `RefusesNewerMajor`, `RejectsCorrupt` (1,000 mutated inputs under ASan: `Error`, no crash), `MigratesV1Fixture` once a v2 exists, else the table is tested with a synthetic step. |
| AC32 | R48, R50 | `AssetTypes.TerrainWorldIsOneAsset`: no `.meta` per tile, no tile tracked, in-file GUID found, moved folder keeps GUID; `VersionControlMetadataTest` gains `*.jtile`; `TerrainWorldAsset.TileListIsFiles`: tiles added on two simulated branches merge with no manifest change. |
| AC33 | R51 | `TerrainWorldAsset.SeamCheckAndRepair`: a hand-corrupted border: `seam_errors` 1, repair picks the lower (x, z), then 0; cook repairs a copy. |
| AC34 | R52 | `TerrainWorldAsset.OverviewIncremental`: one tile changed rebuilds one patch; macro changes on `layerHash`; `TerrainWorldGpu.OverviewRefreshAfterStroke`. |
| AC35 | R53, R39 | `TerrainWorldCook.DeterministicPack`, `ContainsTilesAsEntries` (read with `JPak`), `TerrainWorldGpu.PlaysFromPack`; `tools/smoke-test.sh` with the world fixture on glcore, gles3, vulkan images; shader cook collects the variant: `ShaderCook.TerrainWorldVariants`; no fallback drops the bit (`ShaderPool`'s test). |
| AC36 | R55 | `ModuleUsage.TerrainWorld*`: component and each Lua evidence kind; empty project needs none. `PlayerModules.*` lists it. A build with `-DJASPERENGINE_TERRAIN_WORLD=OFF` links; a player without the module exits 5 on content needing it (`tools/ci/test_player_cli.py`). Sizes for R37 recorded in the spec by `player_sizes.py`. |
| AC37 | R25 | `TerrainWorldNav.CollectsTiles`, `RefusesUnboundedSurface`, `FollowRebakes` (agents keep a path across a follow step); existing nav suites unchanged. |
| AC38 | R26, R27 | `TerrainWorldSplines.BakeCrossesBorder`, `ConformWaitsForTile`; `TerrainWorldFoliage.RegionPerResidentTile`: a region added on `Resident`, removed on eviction, none with no foliage layers; a fixture's scatter equal to one map's across a border (T5's hash); `RemovedHandlerCoversEveryTile`, `TreeHitHandlerOnAnyTile`, `RuntimeStateCoversEveryTile`, `TreeStoreUnderAPosition`; editor `TerrainWorldTrees.*` (a stroke across a border, a resident tile's new file, Mass Place per tile; undo and redo byte for byte); `test_terrain_world.py::test_trees_across_border_with_undo`. |
| AC39 | R43-R46 | `test_terrain_world.py`: `edit_terrain_world {create}`, `sample_terrain {world}`, `wait_for` plus `freeze`, then two identical `screenshot` hashes (determinism); all-or-nothing refusal leaves files unchanged; `dry_run` writes nothing; timeout lists missing tiles; the tool hidden until `tool_groups` enables `terrain`; Play refusals by name; `tool_budget.py --check`, `gen_tool_docs.py --check`. |
| AC40 | R47 | Lua doc test (`python3 -m lua_api doctest`) over the new blocks; `TerrainWorldLua.*`: focus lifetime on destroy, `waitForArea` in a coroutine, `onAreaReady`, errors on bad arguments. |
| AC41 | R40-R42 | `TerrainWorldPanel.*` (logic: cell states, undo of Add and Remove, dialog defaults); screenshots from an isolated editor (`tools/editor-shot.sh`): panel with minimap mid-stream, overlay in each of its four modes, kept in the evidence directory. |
| AC42 | R1, R4, R5, R54 | `TerrainWorldScene.RoundTrip` (component saved, loaded, cooked and generated by `Factory`, field by field), `TransformForcedIdentity`, `AbsentTileNoDrawNoCollision`; `TerrainHeight.*` and `test_upgrade_from_main.py` unchanged and green, proving a plain Terrain and an old project take no new path. |
| AC43 | R34, R35 (hardware), R39 | `docs/specs/checklists/T4.md`, manual on real hardware by a person, with results recorded: Android mid-range, iPhone, Windows D3D11, macOS Metal: fly-through project, no cracks, frame graph, memory against caps; Chrome and Safari on the web export. |
| AC44 | R57, R58 | `python3 tools/check_raw_writes.py` with no new allowlist line; `python3 tools/check_stream_keys.py`; `ShaderKey.TerrainWorldBitIsPinned`; `TerrainWorldAsset.AssetTypeValueIs31`. Red: `tools/check_raw_writes.py --self-test`. |
| AC45 | R59, R46 | Cases in `tools/mcp/eval/cases.py`, run as T1 AC74 says (`-k terrain_world`; lent accounts, else reported unverified): `terrain_world_create` (bad; "make an 8 by 8 km world": `edit_terrain_world {create}`, must not make many Terrain entities), `terrain_world_wait_ready` (guard; "screenshot the far corner": `sample_terrain {world: {wait_for}}`, `set_streaming freeze`, then `screenshot`), `terrain_world_sculpt_across` (guard; "raise this ridge across tiles": `edit_terrain` with `entity`, not `edit_terrain_world`). Red: a surface from before the tool. |

**What this host can verify** (8 cores, software GL, no KVM; `TERRAIN-LEDGER.md`). There is no CI, so a row not run here is unverified until a person runs its command:

| Item | Here | Elsewhere: command a person runs |
|---|---|---|
| Core, editor, TSan and ASan suites; MCP tests | yes | |
| GL core (llvmpipe), Vulkan (lavapipe) goldens, cracks, fly-through | yes | |
| GLES3 path | smoke through the linux-backends image | |
| WebGL2 st and mt | web gate with chrome-headless-shell | Safari: checklist |
| Android | build only (`tools/build.sh android`) | On a host with `/dev/kvm`: `ANDROID_PROJECT=terrain-world ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh`; device: checklist |
| Windows CPU suites | no | With `vcvars64`: `cmake -S engine -B build-engine-gl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DJASPERENGINE_TESTS=ON && cmake --build build-engine-gl --target jasperengine-core-tests && build-engine-gl/tests/core/jasperengine-core-tests.exe --gtest_filter=WorldLattice*:TerrainWorld*:TileStreamer*:TiledMapStore*:TerrainTileFormat*-*Gpu*` |
| macOS CPU suites; Metal | no | `cmake -S engine -B build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON && cmake --build build-engine --target jasperengine-core-tests && build-engine/tests/core/jasperengine-core-tests --gtest_filter=WorldLattice*:TerrainWorld*:TileStreamer*:TiledMapStore*:TerrainTileFormat*` |
| D3D11, iOS | not built; code kept compiling by care | AC43's checklist |
| Absolute ms budgets on a GPU | no: llvmpipe times the CPU upload and work, not a GPU | checklist |

## 8. Dependencies & conflicts

**Depends on.** **T1** (hard): `TerrainOps`, `TerrainMapStore`, surface query, delta undo, `edit_terrain`,
`visualize_terrain`, `docs/mcp/guide/terrain.md`, `groups.terrain` of `budget.json`. **T3** (hard): hole map,
`TerrainHoles`, blend maps, tiers, shadow cut, the fragment path. **T5**: `FoliageRegion`. T2: `generate_terrain`
works through T1. All of them land before T4, which is last (T1 §8). S03 (atomic writes, R22), S04 (type
table, GUID, typed dependencies), S05 (`ScopedExpectedWrite`), S06 (lanes, I/O, pump), S14 (tiles bypass its
cook), S15 (profiler scopes), S19 (`updateTiles`, Volume), S24 (undo names), S26 (picking tolerance), S29
(patterns), S30 (cook, runtime codec), S32 (variant warm-up), S34 (modules).

**Seams others must know.**
- **T1:** the store stays as T1 R86 and R87 write it: this store's global texels are never negative, and
  per-map `worldOfTexel` and `texelOfWorld` place them (D12). What T4 uses from it: `preferredBlock` and `beginLarge`,
  by ops and generation above `largeEditTexels` (D11); and `exact` on `SurfaceSample` (R24). `format` reports a lattice up to 131,073 a side,
  so no op may allocate by `format` rather than by its rectangle. `TerrainMapDeltaCmd` carries
  `takeUndoHook` (R30). `querySurface` and `raycastTerrainSurface` take the branches you reserved.
  `visualize_terrain` needs overlay extension room (R46).
- **T3:** blend stays blend maps, with shared borders per tile (R3); hole maps are per tile, area texels, as
  your seam says. One disagreement: `Thl` is **per World, not per tile** (D14). The world adds **no fragment
  slots** and one fade line in the fragment path after yours (D14); nothing for you to do. T4 reuses R22 and R23
  across chunk and tile borders, and R17's shadow cut. Macro Normal is unavailable on a World (a terrain-wide
  map); Base Map macro likewise, because it is one texture over one terrain: Noise macro works. The overview is
  Low with no layers and ignores holes. Skirts inherit the hole test (D5). `Twd` is appended at landing (your D2 table).
- **T5:** T4 implements F11 as written: `TileFoliageRegion`, one a resident tile, `tiles/x_z.jetrees` inside the
  folder (no `.meta`, because the scan does not enter it), shipped by the cook as entries. `maps()` is per tile. Tree
  capsules go with their region. Add the `*.jetrees` pattern in your own S29 change.
- **T2:** `GridDomain` gets global texel starts from `TiledMapStore` (D12). Whole-world generation goes in blocks
  through `beginLarge` (D11); erosion's padding is yours to request through T1's read.
- **S18:** `SplineSystem.cpp:1005-1019` reads a heightmap directly for water and must read the store on a World.
- **S19:** the collector gets a tile branch; `NavMeshSurface` gets one field.
- **Doriax (D21; none landed: whichever lands second merges onto the first).** `draw-perf` moves `updateAllTerrainRenderTextures` and adds `Dlo`: a World's textures are not in that walk (D1's hook at `updateTerrain`), and `Twd` keys
  follow its twin rule. `ssr-probes`: the G-buffer's terrain AO applies to near tiles only, since the overview has no layers (R9). `mesh-memory`: a World's Mesh uses `StableArray` like any other, and D1 keeps indices across writes. `render-correctness`: foliage ring work reaches a World only through T5's regions.
  `agent-authoring`: `terrain.md` gets T4's heading, and `edit_terrain_details` takes a World through `entity`, per region density in `DENS`.
- **Pixel art.** `AssetType` 24-27 are theirs; no shared names (`jasperengine::terrain`'s `TileCoord`, `WorldLattice` against S50's `TileGridLayout`, `TileCells`).

**Merge hotspots.** `RenderSystem.cpp`, `MeshSystem.cpp`, `InspectorWindow.cpp`, `TerrainEditWindow.cpp`
(hooks only, D1), `terrain_vs.glsl` (one include under `HAS_TERRAIN_WORLD`), `ShaderVariantCompiler.cpp` and
`ShaderPool.cpp` (MESH, DEPTH and G-buffer bit), `editor/Catalog.h` (`ComponentType` append), `Stream*.cpp`,
`Factory.cpp`, `RuntimeFields.gen.inc`, `ComponentHooks.cpp`, `Exporter.cpp`, `variants.json`,
`tools/mcp/budget.json` (programme D17), `McpToolGroups.cpp`, `Engine.cpp` (one line), `Project.cpp:6592`.

## 9. Risks & defaults

| Risk or question | Default taken |
|---|---|
| The 9-fetch vertex path costs too much on low mobile | Measure in the checklist. Fallback: normals from the overview past level 1. The format does not change |
| Skirts on half grids (+24% vertices) | Accept; AC12 and the checklist watch it. Skirt indices can go in a separate range later |
| Page pops when a tile arrives after the view wanted it | Prefetch ring plus 1.5 s prediction; `late_pages` counted. Teleports pop by nature; scripts wait with R18 |
| R16 would have let the driver filter; RG8 forces manual bilerp | Decided, D3. Seams and one path outrank it |
| float32 detail UVs at the world's edge with small tiles | 16,384 m at a 2 m detail tile leaves 1/1,024 of a tile (*calc*), so 2 m is the documented lower bound past ±8 km. Revisit with T3's tiers |
| The world's height range is one for all tiles: 16 bits over 8 km steps 12 cm | Warn in the dialog above 2,000 m. Documented |
| A swap from coarse to fine collision pushes or drops a body by the overview's error | Bounded by `skirtDepth` of that tile; AC22 checks rest. Character Controllers snap to ground as usual |
| Overview macro is stale after a Lua layer change | Macro is bake data, refreshed only by the editor. Documented |
| Something queries from the game's main thread over a cold tile | Returns the overview with `exact = false`; callers decide. Spline jobs wait on a worker, legal per `JobSystem.h:21-22` |
| Vulkan page reuse needs same-size images | True by construction: one world, one lattice. A second world with other sizes has its own pool slots within the memory cap |
| Module not in `lite3d`: a World moves a no-UI game up to `core` (22 vs 18 MiB, `S34` M8) | Accepted: a World's assets dwarf 4 MiB, and `lite3d` games without a World stay untouched |
| Terrain3D-style 65 km | Not offered. 32 km is about 1,000 km², and single precision says stop there |

## Deviations

Recorded while implementing (`terrain/t4-worlds`). The code is what holds; each item says what changed and why.

- **Names.** The tile in memory is `terrain::WorldTile`, not `TileData`: `jasperengine::TileData` is the tilemap's, and both are visible where the runtime is written. The world's non-pure runtime is `TerrainWorldRuntime` (engine/core/subsystem/), which is also the `TileAccess` the `TiledMapStore` reads; D1's `TileFoliageRegion` and `TerrainWorldQuery` are not separate classes (see R27 below; the query is `TerrainWorldRuntime::query` and `::raycast`).
- **R7, AC2: the visited-node bound per doubling of the side is 40, not 4.** In a filled world a level more expands the eye's node and every neighbour within its range, about nine nodes of four children; a small world reaches the same levels through partly occupied nodes that expand one child. `TerrainWorldSelect.DoesNotScaleWithWorld` measured 60, 128 and 184 visits at 8, 64 and 256 tiles a side (64-quad tiles, eye at a quadrant corner, the worst case); a walk over the big world's tiles would visit 65,536.
- **AC26: T1's golden hashes are not rerun on a one-tile `TiledMapStore`.** The store's format is the addressable lattice (D12), so T1's whole-map sequences would cover 32,769² texels, and edges differ (an absent tile reads the base height where one map clamps). `TiledMapStore.OpsMatchOneMap` instead runs raise, smooth, set_height and terrace on a 2 x 2 world and on one 129-sample map over the same samples and compares the bytes. `add_noise` anchors its field at the map's first texel, the world's addressable corner, so it is left out of that comparison: same op, another field.
- **R29: a write makes an absent tile only when it changes a sample inside it.** A change on a shared border reaches the tiles there; an absent neighbour is made by a change strictly inside it and copies its borders from its neighbours when made. Otherwise an import or a stroke along a border spawned the ring of neighbours.
- **R20, D16, AC24: collision is one static compound on the World's own Rigidbody.** Chunks are built on Background jobs as specified, but instead of a body per chunk added with `AddBodiesPrepare`/`AddBodiesFinalize`, every coarse tile and fine chunk is a child of one `StaticCompoundShape` set with `PhysicsSystem::setPrebuiltBodyShape3D` (Jolt `SetShape`) in the pump, at most once per pump that changed something. Contacts so report the World's Rigidbody with its layer, mask, friction and listeners, a coarse tile and its fine chunks swap in one shape before the next step (R22), and no per-chunk body exists. AC24's `BatchAddsOnly` becomes "no AddBody per chunk; one shape swap per changed pump" (`TerrainWorldCollision::shapeSwaps`).
- **R13, D6: pages are immutable images on every backend, Vulkan included.** No pool of reused `stream_update` images; a page that cannot be made is refused, counted in `upload_retries`, and goes again next frame. On Vulkan creating an image with data waits for the queue, a hitch per tile upload that the spec's pool avoided; not measured here (lavapipe).
- **D14: no near-to-far fade, and the overview draws with the near tiles' variant.** Near draws read no macro colour in the vertex stage and the fragment stage is unchanged. An overview draw binds black blend maps (every layer at weight 0, the base layer only), the overview's macro texture where the base colour map goes, and its own UVs (`terrainWorldOverview()`), on the same shader as the near tiles: no second, Low-tier pipeline per pass. Its fragment cost is that of the terrain's tier, not Low's.
- **R17: a tile decodes in one Background job,** not one per section. At N = 256 a tile is one zstd frame per section of under 0.2 MiB.
- **R12, D3: the normal's deltas are held inside the page symmetrically.** A vertex `d` samples from the page's edge reads at most `d` samples either way, so a vertex on a border reads its two neighbours on both tiles alike (`WorldLattice.BorderNormalsMatch`); interior vertices read the spec's delta.
- **R32: the import's temp folder is a stash under `.jasperengine/trash/terrain-world-import/`, not a sibling of the world's folder,** so the
  asset database never sees half an import. The job writes every tile it changes there, whole (the covered tiles and the present tiles round
  them, whose shared samples and gutters change too); the swap is one undo step (`TerrainWorldImportCmd`) that keeps each replaced file in the
  same stash (a hard link, else a copy) instead of snapshots in memory, so an import has no size cap beyond the world's limits. The swap refuses
  when a tile file changed after the job read it. Spacing and max height are the world's (a file sample lands on each world sample; the file's
  range maps to 0..max height, T1's Auto range); the panel's dialog says so. `import_heightmap` runs as a job and is sent alone in its call.
- **R33, R40, R43: Convert to Terrain World deactivates the source GameObject** (not only its Terrain), so its collider and foliage stop with it. Tiles cover the Terrain's square from its centre, and where they reach past it their samples repeat its border (`sampleTerrainSurface` clamps) rather than being holes. `create.from_terrain` runs `ops` in the same call only when the conversion runs in the call (up to 2,049² samples); a larger one is a job, and `ops` go in a second call. Undoing a create or a conversion removes the GameObject and its Rigidbody (and reactivates the source); the asset folder stays (a later conversion picks the next free name; a create naming the folder is refused).
- **R25: tiles not in memory are demanded, and the collection is retried until they are in,** rather than loaded by a job the bake waits on. S19's collection is a main-thread snapshot that hashes every tile's inputs before a job runs, so a tile's samples must be there when it runs. `NavBakeCollector` holds the tiles in memory by shared reference in the surface's context, demands the rest from the streamer (its AssetIOService jobs read them) and fails with `pendingTiles`; `NavigationSystem` reports the surface Baking and collects again at each `processBakes` until every tile is in. No main-thread read, no job holds the runtime, and single-threaded web needs no worker to wait on. `bakeNow` pumps the World while it waits (60 s at most). The stale-tile check (`countDirtyTiles`) answers -1 until a World's tiles are in memory.
- **R25: a tile's lattice reads the tile's samples in place.** `NavTerrainInput` gains a centre (a World's tiles share the World's `toNav`, so neighbours' border samples are the same nav points, bit for bit) and a 16-bit view of the tile's heights with the tile held by `shared_ptr`. Its sample step is the smallest power of two whose spacing reaches the voxel size (addTerrainLattice's one sample per voxel), so borders stay on shared samples. Lattices of other terrains hash as before.
- **R25: Follow follows the first Terrain World** (entity order) whose runtime runs; other Worlds a surface collects follow the Volume and 64-tile rules. Its focus is the World's Focus GameObject, else its camera, else a script's focus. The first bake round the focus is a Dirty bake (only tiles whose inputs differ from the loaded navmesh), then each step is an `updateTiles` over the box of the World tiles that come and go. A navmesh made by a Follow bake reserves a tile pool for the whole region (`NavBakeInput::reserveTiles`), so steps replace tiles in place and never rebuild the navmesh under its agents. The field is ignored on single-threaded web unless `NavMeshSurface.followTerrainWorldFocus` was set by a script.
- **R25 against S19's code: `updateTiles` kept baked tiles outside the grid of what it collected again,** so a navmesh tile whose World tile left a Follow region would have stayed. `updateTiles` now also drops baked tiles that meet its box outside that grid (they have no input any more).
- **R26: S18's water fill on a World reads the world's own sample lattice** (samples at `s * spacing`) rather than texel centres, on the spline's
  Background job through a `TerrainWorldHeightReader` (the tiles in memory, the rest read from the source on the job), so the runtime may go
  while it runs. Conform still probes on the main thread as T1's query answers there.
- **R27: edit_terrain's foliage ops run tile by tile on a World.** Each op runs once per tile it reaches, on that
  tile's own copy of its maps and trees in the tile's frame (so `count` without a shape is shared out over the tiles,
  and each tile's scatter seed is mixed with its key), because T5's ops work on one centred square and its one tree
  store. A scatter's spacing therefore reads only its own tile's trees: two trees either side of a border may stand
  closer than it; the brush, which reads every tile within its spacing, does not. A World's tree ids in MCP
  (`place_trees`, `remove_trees`, `pick_at`) are the tile and record in one integer under 2^53, as T5's 64-bit
  TreeId does not fit JSON's numbers for every tile.
- **R23: Physics Placement builds one static body per fine chunk in its own Jolt world** (`TerrainWorldCollision::chunkSamplesNow`, the
  collider's sampling and bit depth), for the chunks each context region reaches, never the whole World; `JoltShapeBuilder::buildHeightFieldShape`
  still refuses a World.
- **R41: the panel opens from the Terrain World inspector's Open Terrain World Panel button and after Create,** not from Window. Its minimap has no camera frustum and no click-to-frame.
- **R42: the overlay's LOD Level shows the main camera's cut** (the Scene view's camera in the editor, the game's in Play), recorded by render
  only while the overlay shows it, and Collision Chunks shows the World's collider, which exists while the scene simulates (Play); so
  `visualize_terrain` now runs in Play. The sheet covers the box of present tiles (no ring past them). A brush pins the tiles it hovers each
  frame; a pump half a second after the last hover lets them go. T1's overlays (height, slope, ...) and T4's are each refused on the other kind.
- **D9: what changes a World's drawability sets its `transform.needUpdate` where it changes, not only in the pump.** The render upload step, which runs in the frame's draw, sets it when a page goes up or away or the overview is made, so the next update selects again; the pump sets it for what the pump itself changes (a page evicted, a new overview). The pump skips frames closer than its 1 ms dedupe (the editor's extra draws of a frame), so when the pump passed the upload step's changes on, a world whose pages and overview arrived in frames with no pump after them drew nothing (26 `TerrainVariantGpu` World cases on a fast CPU). `TerrainWorldGpu.DrawsPagesUploadedWithNoPumpBetweenFrames` holds the pump (`TerrainWorldSystem::setPumpHeldForTesting`) to make that interleaving every run.
- **Play in place restarts a World's runtime,** saving the editor's edited tiles first (`Host::released`), so Play's write cap and waits apply (R29, R19).
- **R29, R30: T1's overlay commits make a World's tiles on the step's first application.** `edit_terrain` writes into an `OverlayMapStore` that `TerrainMapDeltaCmd` applies on its first execute; that application may make tiles and keeps the store's undo hook with the step, and its redos and undos never make one. A brush stroke, which writes in place, keeps the hook as it ends.
- **R30, AC27: an undo puts a tile's snapshot back as it was, derived sections included** (`TileAccess::restore`), and a new world's tiles carry the macro colour the runtime derives from the new Terrain's layers, so undo and redo write the bytes the files had. Changing the Terrain's layers later changes the colour of the tiles an edit then touches.
