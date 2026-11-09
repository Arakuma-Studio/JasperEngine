# T6: Voxel Volumes

Wave 5 (terrain programme). Binding decisions: `TERRAIN-LEDGER.md` D1-D21. Analyst reports:
`/tmp/terrain-research/reports/voxel-storage-streaming.md`, `voxel-meshing-generators.md`,
`engine-constraints.md`, `our-terrain-runtime.md`.

`D` numbers below are the ledger's; this spec's own decisions are `V1`-`V15` in section 6. Claim labels: every `file:line` of section 3 was read at `b4f7816ea` (verified by
reading). Nothing was built or run for this spec, so budgets are targets the implementer measures,
and statements about behaviour no test covers yet say *inferred*.

## 1. Goal

A game can have caves, overhangs and ground you can blow up. A **Voxel Volume** is a bounded box of
smooth voxel terrain: an agent or a designer sculpts or generates it, it draws at several levels of
detail without a crack, players collide with it within a few frames of a dig, and placed over a
Terrain it cuts its own holes and meets the ground with no visible seam. It is an opt-in player
module (`JASPERENGINE_VOXEL`) that costs nothing when a project does not use it, and it works on all
five graphics backends and six export targets with CPU jobs, a vertex buffer and texture arrays.

## 2. Scope

**In scope**
- Module `JASPERENGINE_VOXEL` (D18): sources in `engine/core/voxel/` (`jasperengine::voxel`, D1).
- Data: bounded volume, 16³ blocks, 16-bit SDF, compact material channel, uniform-block collapse,
  an LOD sample pyramid, per-platform dimension and memory limits.
- Meshing: Transvoxel regular and transition cells, gradient normals, jobs with zero-worker
  time-slicing, a clipbox of up to 4 levels, a seam guarantee, a per-frame upload budget.
- Rendering in the built-in Lit path: triplanar over a texture array, High, Medium and Low tiers with
  fixed fetch budgets, shadows, picking.
- Physics: Jolt `MeshShape` per block near a focus, built on jobs, inserted in batches; defined
  behaviour during rebuilds; tunnelling protection; navmesh rebuild policy.
- Editing: one stateless deterministic operation set behind the brush, MCP and Lua; undo as
  per-block deltas; runtime destruction with budgets and debris events.
- Generation: from a Noise Graph (T2), from a Terrain (with a blend), from a mesh; presets.
- Terrain integration: automatic holes (T3), hidden overlap, shading parity, surface fallback.
- `.voxelvolume` format, GUIDs, cook and export, runtime saves of player changes.
- Human UX (panel, brushes, materials, Slice view, stats) and agent UX (group `voxels`).

**Out of scope** (decisions, not open questions)
- Blocky (Minecraft-style) and Cubes meshers; greedy meshing (report: wanted only for that product).
- Worlds of many volumes with streaming across them, and unbounded volumes. A world of volumes is a
  later spec; T4 owns tiles. One volume stays below R7's limits.
- Octree LOD (report: single viewer, legacy) and more than one LOD viewer per volume. Collision takes
  several focuses (R22), LOD one.
- Replication and multiplayer sync (brief; report: experimental upstream).
- GPU generation and detail normalmaps upstream runs through SSBOs (`docs/specs/README.md` forbids
  compute and SSBOs; the CPU path is the only eligible one).
- Falling islands: a dig that detaches a piece leaves it floating. `sample_voxels` reports the
  components and `onEdited` gives scripts debris points, but the engine does not simulate collapse.
- Non-destructive modifier stacks (upstream's "modifiers"; S18:40-42 rejected them for terrain too).
- Per-material physics friction: one physics material per volume. Scripts read the material at a
  contact with `getMaterialAt` (footsteps, particles).
- FastNoise2 (D10), all Godot code (D14), SQLite and region-file streams (report table).

## 3. Current state

There is no voxel code in the engine (`engine/core/voxel/` does not exist). What T6 builds on:

**Chunk pattern.** `SplineSystem` is the template.
- Generated chunks are engine-owned entities of the scene, never saved and never in the hierarchy
  (`engine/core/subsystem/SplineSystem.h:88-92`, `SplineSystem.cpp:605-611`).
- A chunk's apply writes `mesh.buffer` and `indices` and sets `needReload` when already loaded
  (`SplineSystem.cpp:278-305`), so a rebuild goes through destroy-and-load, not a partial update
  (*inferred from* `RenderSystem.cpp:14904-14906`).
- Above 2 ms of estimated work a rebuild is `jobs::submit` per chunk on `Lane::Background`
  (`SplineSystem.cpp:1250-1251`), and results are polled by generation.
- Each chunk gets a static body through `createMeshCollider` on the main thread
  (`SplineSystem.cpp:1397-1400`).
- Clicking a chunk selects its owner through `getGeneratedOwner` (`editor/util/ProjectUtils.cpp:549-554`).
- Registered at `engine/core/Scene.cpp:59`, before `MeshSystem` at `:60` and `PhysicsSystem` at `:77`.
  Registration order is the phase order (`Scene.cpp:959-969`, `Scene.h:407-430`).

**Meshes.**
- Index buffers are 16-bit (`engine/core/buffer/IndexBuffer.cpp:37-38`).
- Buffer usages `IMMUTABLE`, `DYNAMIC`, `STREAM` (`engine/core/render/Render.h:143-147`).
- A mesh with `Transform.visible == false` is skipped by the color and depth loops
  (`RenderSystem.cpp:9394,9980`); the load loop has no such test (*inferred from*
  `RenderSystem.cpp:14860-14923`).
- Pool defaults are generous: 16384 buffers, 8192 pipelines (`engine/core/Engine.h:284-288`).
- CPU copies stay in `mesh.buffer`, and a view reload needs them (`RenderSystem.cpp:1044`).

**Physics.**
- `JoltShapeBuilder::buildMeshShape` takes raw vertices and uint32 indices
  (`engine/core/physics/JoltShapeBuilder.cpp:221-246`, `MeshShapeSettings` at `:235`); the raw-mesh
  collider path is `:383-402`.
- Mesh shapes must be static (`engine/core/subsystem/PhysicsSystem.cpp:2030-2035`).
- Bodies are created and added one at a time (`PhysicsSystem.cpp:2819,2840`). Jolt warns that this
  leaves an inefficient broad phase and offers `AddBodiesPrepare`, which "can be done on a background
  thread" (`engine/libs/joltphysics/Jolt/Physics/Body/BodyInterface.h:95-96,120-123`). `SetShape`
  exists (`BodyInterface.h:167`).
- World capacity defaults to 8192 bodies (`PhysicsSystem.cpp:225`).
- `ActivateBodiesInAABox` is already used (`PhysicsSystem.cpp:804`). Motion quality is exposed
  (`component/RigidbodyComponent.h:47,135`).
- Jolt's cast pass ignores back faces (`Jolt/Physics/PhysicsSystem.cpp:1675`): a body past a triangle
  meets nothing until it leaves, which is the no-thickness problem.

**Player modules (S34).** Six modules (`engine/core/player/PlayerModules.cpp:12-14`,
`engine/player/variants.json`), detection in `editor/players/ModuleUsage.{h,cpp}`
(a component field: `ModuleUsage.cpp:335-337`), switches at `editor/Exporter.cpp:3595` and the Xcode
replacement at `:4611`, conditional library at `engine/CMakeLists.txt:1139-1141,2118`.

**Jobs (S06).** `jobs::submit`, `Task::cancel` (`engine/core/jobs/Task.h:65`), `postToMain`
(`JobSystem.h:180`), single-thread mode with 0 workers where nothing runs at submit
(`JobSystem.h:24-25`) and Background gets 4 ms per frame (`docs/specs/S06-jobs-memory.md:250-253`).
`MainThreadQueue` does not drain while paused (`engine/core/thread/MainThreadQueue.h:16-19`), so T6
polls tasks instead. No mesh upload budget exists anywhere (engine-constraints report).

**Rendering.**
- Terrain triplanar is `engine/shaderlib/includes/terrain_fs.glsl:97-104`, sharpness 8 and cutoff
  0.02 (`:28,31`), fetching through `textureGrad` on a `sampler2DArray` (`:122`). A layer costs four
  fetches (`:89`).
- A painted terrain is at the 16-texture limit (`engine/shaderlib/mesh.frag:70-74`,
  `sokol_gfx.h:2051`).
- The detail array is built in `RenderSystem::getTerrainDetailArray`
  (`RenderSystem.cpp:5481-5726`), RGBA8 with a 2048 px cap (`TerrainComponent.h:28`), and S14 excludes
  arrays from compression (`S14:34`).
- Lit variants beyond the property bits go through `variant.litDefines`
  (`RenderSystem.cpp:3904-3917`): no property bit. Terrain hex tiling is the precedent, and a shader change still bumps the cache (R83).
- `flat` varyings already ship on all backends (`engine/shaderlib/htmlui_prim.vert:39-48`).
- Vertex attribute formats include `ubyte4n` and `ushort2n`
  (`engine/renders/sokol/SokolPipelineRecord.cpp:131,135`).

**Terrain.** One heightmap, 3 blend maps, 9 layers, `TerrainSurfaceLayer` (`TerrainComponent.h:24`,
fields to `:110`), `MeshSystem::sampleTerrainSurface` (`MeshSystem.cpp:3942`). No holes yet: T3 adds
them (D7), T1 adds the operation engine and the surface query (D4, D5). Terrain map undo is a rect
patch (`editor/window/TerrainEditWindow.cpp:1979`).

**Assets and export.**
- `.navmesh` is the binary-asset precedent: one row at `editor/asset/AssetTypes.cpp:48`, one at
  `editor/asset/AssetRefSchema.cpp:161`, sidecar GUID.
- `writeFileAtomic` (`engine/core/io/AtomicFile.h`), `FileUtils` and `data://`, with web sync
  (`engine/core/io/FileUtils.h:15,33-36`).
- JPak is uncompressed with a whole-entry check on read and an in-place `view` for memory packs
  (`engine/core/io/JPak.h:61-66`), and the web keeps it in the heap (`S30:326`).
- zstd: `zstd_decompress` in every game, the compressor only in the cook
  (`engine/CMakeLists.txt:1142-1146,2115`).
- Component serialization spans `editor/StreamComponents.cpp:573`, the runtime hooks
  (`engine/core/serialization/codecs/ComponentHooks.cpp:152-158`) and `RuntimeFields.gen.inc` (`S30:173-178`).
- Xcode lists sources explicitly: `tools/xcode_sync_splines.py` is the convention.

**Navigation.** `NavMeshSurface:updateTiles(bounds)` (`engine/core/object/navigation/NavMeshSurface.h:39`,
`S19:186-188`), `NavBakeCollector` collects meshes generically (`NavBakeCollector.cpp:583-611`), and
web tiles are capped at 32 cells (`S19:584-588`).

**Tools.** Group table `editor/mcp/McpToolGroups.cpp:16-38`, module table
`editor/mcp/tools/McpTools.cpp:51-66`, budgets `tools/mcp/budget.json`; `generate`, `sample`,
`create`, `edit` are allowed verbs and S19 added `visualize` (`tools/mcp/tests/test_tool_conventions.py:37-49`).

## 4. Reference

**Eric Lengyel's Transvoxel** (`/tmp/terrain-research/Transvoxel/src/`). `LICENSE` is the MIT licence,
"Copyright (c) 2009 Eric Lengyel" (verified: read, which settles the analyst's open question).
`Transvoxel.cpp` holds `regularCellClass`, `regularCellData`, `regularVertexData`,
`transitionCellClass`, `transitionCellData`, `transitionCornerData`, `transitionVertexData`
(1,009 lines). **Copy verbatim** (D14) with its header to
`engine/libs/transvoxel/Transvoxel.cpp` plus `LICENSE` and a `README.jasper.md` that records the
upstream commit; add a `NOTICE.md` row and, since it sits under `engine/libs`, an `engine/notices.json` entry; `python3 tools/ci/check-notices.py` checks both.

**godot_voxel** (`/tmp/terrain-research/godot_voxel/src`, commit 045ff93; paths below are relative to it). **Ideas only, no code** (D14).
Its table file carries a more restrictive header (`meshers/transvoxel/transvoxel_tables.cpp:10-29`),
another reason not to touch it.

| Idea | Where | Take | Change |
|---|---|---|---|
| Uniform collapse, block layout | `storage/voxel_buffer.h:57-69` | Per-channel uniform state | 2 channels, no 64-bit depth |
| Secondary positions | `meshers/transvoxel/transvoxel.cpp:29-90`, Lengyel's dissertation | The boundary-cell offset and tangent-plane projection, and the clamp to one cell | Computed on the CPU for the block's known mask; no `CUSTOM0`, no `floatBitsToInt` (D13) |
| Higher resolution owns the transition | `terrain/variable_lod/voxel_lod_terrain_update_task.cpp:708-711` | Same ownership | Coarse blocks never remesh for a neighbour |
| Known problems | `terrain/variable_lod/voxel_lod_terrain_update_clipbox_streaming.cpp:20-21`, `engine/priority_dependency.cpp:38,48-50`, `doc/source/performance.md:82,98,124,134-136` | Dropped tasks and priority order cause holes; fast motion churns; colliders cost 3-5x meshing; no thickness | V6 and V8 answer each one |
| Edit shapes | `edition/voxel_tool.h:28-46` (stateful, returns void), `edition/funcs.h:262-428` | Shapes and blends as separable functions | One call, a full result, undo (D15) |
| Block format | `doc/source/specs/block_format_v4.md`, `region_format_v3.md:166-173` | A trailing check | Explicit little-endian and a version header (its documented weakness) |

**Terrain3D** contributes the editor grammar through T1 (D16), not code.

Licences: Transvoxel MIT. Nothing from the OpenEngine do-not-copy list is touched: no compute, no
SSBO, no GPU-driven culling, no render graph. Upstream dropped the README's "Licences" bullet; its remaining rule (copied code keeps its header and gets a `NOTICE.md` entry) is what this spec does for Transvoxel, and ledger D14 still decides the rest: no Godot code, and the MPL-2.0 erosion code stays banned. T6 reads neither.

## 5. Requirements

### Functional: data

- **R1 Voxel Volume component.** `VoxelVolumeComponent` (Inspector name **Voxel Volume**) holds, each
  with a default and saved in the scene:
  - `volumeData` (the `.voxelvolume` asset, by GUID); `materialSlots` (R43);
  - `quality` (Auto, High, Medium, Low); `lodLevels` (0 = from quality, else 1-4); `lodRadius`
    (4, 6, 8 blocks, default 4); `maxDrawDistance` (0 = none);
  - `castShadows`; `collision` (bool, default on); `collisionRadius` (0 = from quality);
  - `rescueBodies` (default on); `navigationUpdate` (Off, Automatic; default Automatic);
  - `terrain` (entity ref, default none), `terrainCut` (Off, Automatic), `overlapDepth`
    (default `0.02 * voxelSize`), `cutTolerance` (default `0.25 * voxelSize`);
  - `focus` (entity ref, default the active camera); `runtimeGeneration` (default off).

  Dimensions and `voxelSize` belong to the asset. A volume's Transform takes translation and rotation.
  Scale stays (1, 1, 1): the Inspector shows an error and offers "Apply to Voxel Size", and
  `edit_voxels {create}` refuses anything else. A volume with `terrain` set must have no rotation.
  A volume is static in Play: moving its Transform at runtime logs one warning and does nothing.
- **R2 SDF channel.** Each block is 16³ samples at voxel spacing, x fastest. A sample is `int16`:
  distance in voxels times 4096, **negative is solid, positive is air**, clamped to +/-32767 (a band of
  about +/-8 voxels). Eight voxels is exactly the sample spacing of level 3, so a crossing edge at the
  coarsest level stays inside the band.
- **R3 Material channel.** Each sample has a `uint16`: slot `a` (4 bits), slot `b` (4 bits), weight of
  `b` (8 bits, 0 means only `a`). So up to **16 slots per volume, 2 per voxel, 4 per vertex** (a vertex
  lies on an edge of two samples). The channel exists and costs memory only where it is not uniform.
- **R4 Uniform collapse and sparse storage.** A channel whose 4096 values are equal stores the value
  alone. Each level has a flat table of 8-byte slots (tagged: both uniform values inline, or a pointer
  to block data), so lookup is O(1) and the table is 8 bytes per block whatever the content. Non-uniform
  block payload is 8 KiB (SDF only) or 16 KiB (both). After every edit and generation a block that
  became uniform collapses. Calculated: a 1024×512×1024 volume has 131,072 level-0 slots (1 MiB).
- **R5 LOD pyramid.** `L` levels, 1 to 4. A block of level `k` is 16³ samples at spacing `2^k`.
  **A level-k sample is exactly the level-0 sample at the same position: point decimation, never an
  average.** This is what makes coarse and fine agree on shared corners (V6). Levels share one block
  type and one mesher path. Edits update every level in the same call by copying the touched positions.
  Levels above a volume's dimensions are not created: `L = min(requested, 1 + floor(log2(minDim / 16)))`.
- **R6 Immutable blocks.** Block data is immutable after publication and reference counted. An edit
  writes a new block (copy-on-write); jobs hold references, so a job reads one consistent version
  without a lock. Only the main thread writes slot tables. Play gets a copy of the tables, so Play edits
  never reach the editor's data.
- **R7 Limits and budgets per platform.** Project Settings > Voxels can lower them.

  | Target | Max dimension per axis (voxels) | CPU memory budget, all volumes | Mesh memory budget | `Auto` quality, free-standing volume |
  |---|---|---|---|---|
  | Windows, Linux, macOS | 1024 | 256 MiB | 128 MiB | High |
  | Android, iOS | 512 | 64 MiB | 32 MiB | Low |
  | HTML5 | 256 | 32 MiB | 24 MiB | Low |

  Dimensions are multiples of 16 per axis, minimum 16. CPU memory counts hot blocks, cold bytes, slot
  tables and (in the editor) undo. Export fails naming the volume and the target when a dimension is
  over the target's limit, unless `voxel.ignoreDimensionLimits` is set. At runtime only the memory
  budget binds:
  - An edit that would exceed it is refused before any write, with error `budget_exceeded` (R27).
  - Unmodified hot blocks are evicted LRU to their cold or generator form before that refusal.
  - A volume whose file alone does not fit does not load: it logs and surfaces the error (R61) and the
    game continues without it.
- **R8 Bounds.** A free-standing volume is **Closed**: the outermost sample layer is locked to air, so
  every surface is a closed mesh. A terrain-attached volume is **Open**: the outermost 2 sample layers
  are locked to their base value, so a cave cannot breach the side of the box, and they are normally
  culled (R41). Edits clip to the unlocked region; the result reports `clamped_voxels`. A sample past the
  end of the volume, which a coarse block can reach, reads as air in a Closed volume and as the nearest edge
  sample in an Open one, so coarse levels close the same way.

### Functional: meshing and LOD

- **R9 Regular cells.** `voxel::meshBlock` runs Lengyel's modified Marching Cubes over a block's 16³
  cells (reading neighbours for the +1 samples and for gradients) with his tables through
  `TransvoxelTables` (D14). Triangles face the air. Output is pure arrays with no ECS or render types
  (D1): fixed-point positions, normals, material data, 16-bit indices, the AABB, a triangle count.
  A mode `CollisionOnly` skips normals and materials and is at most 0.7× the time (R55).
- **R10 Transition cells.** For each side whose neighbour is one level coarser the fine block emits
  Lengyel's transition cells (table `transitionCellData`, winding per the class's high bit) and moves
  its boundary vertices aside, by V5's rules. Coarse blocks never carry transitions and never remesh
  because of a neighbour. The transition width is 0.5 of a level-k cell.
- **R11 Bit-exact shared vertices.** Positions are computed once in fixed point (V5). A vertex on an
  edge shared by two blocks, or by a fine block's transition cell and its coarse neighbour, has
  bit-identical position and normal in both. Chunks draw under one model matrix, so identical inputs
  give identical clip positions.
- **R12 Normals.** From the SDF gradient: central differences at the corner samples, at the level's
  spacing, interpolated along the edge. Zero gradient gives +Y. Reading out of bounds clamps to the
  edge sample.
- **R13 Vertex materials.** Each cell picks up to 4 slots: the highest summed weights of its corners,
  ties by lower index, sorted by weight. Each vertex of the cell has weights over that set, computed from
  its edge's two samples and interpolated by `t`, normalized to sum 255. Vertices of neighbouring cells
  at the same position are shared only when their sets match, else duplicated (the position is still
  identical), so material interpolation stays correct.
- **R14 16-bit indices always.** A block's mesh fits 16-bit indices. If per-cell sets would push a block
  past 65,535 vertices, the mesher reruns with one set for the whole block (top 4 of the block), which
  duplicates nothing and needs at most about 15 k vertices (calculated). Never a split, never a 32-bit
  buffer.
- **R15 Jobs and time-slicing.** All meshing and shape building runs through a module-side priority queue
  (`voxel::WorkQueue`: edit work, then near, then far; re-sorted when the focus moves, because lanes are
  fixed) that feeds `jobs::submit` on `Lane::Background`, `GateClass::Default`:
  - With workers: at most `max(1, workers - 1)` jobs in flight.
  - With 0 workers: per frame, `clamp(pumpBudget / averageJobMs, 1, 4)` jobs, so the S06 pump's 4 ms is
    respected to within one job. With no job system (tests): inline.
  - Results are polled (`Task::isReady`), never `postToMain`, so they arrive whenever the system updates,
    and the editor is not paused by them. No thread of its own is ever made (`PTHREAD_POOL_SIZE_STRICT`).
- **R16 Clipbox.** One LOD focus. For level `k < L-1` a box of half-size `lodRadius` level-k blocks,
  centred on the focus snapped to the level-(k+1) block grid, clamped inside the next box shrunk by one
  level-(k+1) block. Level `L-1` covers the rest of the volume. A block is visible at level `k` when it
  is in box `k` and not in box `k-1`. So visible blocks partition the volume, and face neighbours differ
  by at most one level, also across edges and corners (`lodRadius` is even and at least 4). The centre
  moves only after the focus goes 0.75 of a snap cell from it (hysteresis). `maxDrawDistance` hides
  whole coarsest-level blocks; a seam never faces a hidden block because their neighbours keep no mask
  for it (an open edge at the draw distance is expected).
- **R17 Change sets: never a crack.** Every mesh that goes on screen is part of a **change set**: a list
  of (block, level, side mask) plus the data version it was read at, built off screen (V6):
  - The old meshes stay visible until every mesh of the set is built and uploaded; then one frame hides
    the old ones and shows the new. A failed or cancelled job therefore shows nothing wrong.
  - Edits pre-empt: an edit cancels a pending LOD set that has not activated (the finished meshes stay in
    a small cache keyed by block, level, mask and version), builds its own set for every block whose
    samples or padding changed, and recomputes the LOD set after activation.
  - Sets activate in order, at most 2 are in flight, a third merges into the second.
- **R18 Upload budget.** Chunk meshes are `IMMUTABLE` buffers made through the normal mesh load. Per frame
  at most: High 16 MiB and 32 meshes, Medium 8 MiB and 16, Low 2 MiB and 6 (one mesh always goes, however
  big). A chunk being loaded is not visible yet (R19), so the budget delays a set and never shows a
  partial one. The count is of bytes handed to the mesh load each frame.
- **R19 Chunk entities.** A chunk is an engine-owned entity (`Transform` whose model matrix is exactly the
  owner's, `MeshComponent`, optional `RigidbodyComponent`), never saved, never in
  the hierarchy, with the owner's layer. A pool reuses them. Chunks being built have
  `Transform.visible = false`. A block with no triangles has no chunk. `VoxelSystem::getGeneratedOwner`
  maps chunk to owner, and `VoxelVolume.fromGameObject(go)` maps a physics hit's GameObject back.
  Disabling the component or the GameObject removes chunks and bodies; re-enabling rebuilds them.
- **R20 Loading.** `RenderSystem::isAllLoaded` is false until the volume's first LOD set is active.
  Before the first physics step of a scene the collision blocks around the focus are built blocking
  (`jobs::parallelFor`; inline on single-thread web), so a player does not fall through at start.

### Functional: physics

- **R21 Collision bodies.** Collision uses level 0 only, whatever level the block draws at, from a
  `CollisionOnly` mesh. `JoltShapeBuilder::buildMeshShape` runs on a job (it touches no shared state).
  One static chunk body per block. New bodies go in batches of up to High 32, Medium 16, Low 4 per tick:
  `CreateBody` on the main thread, `AddBodiesPrepare` on a job (inline with 0 workers), `AddBodiesFinalize`
  in `VoxelSystem::fixedUpdate` before the step. A rebuilt block with an existing body uses
  `SetShape(id, shape, false, DontActivate)`. An emptied block's body is removed. Layer and physics
  material come from the owner.
- **R22 Which blocks collide.** Blocks within `collisionRadius` (High 3, Medium 3, Low 2 blocks) of each
  focus, plus the blocks an awake dynamic body inside the bounds overlaps (AABB grown by one block; up to
  64 requests a tick). Focuses are the `focus` entity and every Character Controller in the bounds. An
  already built block stays until the body cap (High 512, Medium 256, Low 96), then the least recently
  needed block with no body nearby goes first. `VoxelVolume:ensureCollision(bounds)` builds blocking,
  `isCollisionReady(bounds)` reports. `collision = off` builds nothing.
- **R23 Bodies during a rebuild.** This is the contract after an edit:
  1. The **old collider stays live until the new one is ready**; there is no time with a gap. For up to
     R56's latency a body rests on the pre-dig surface.
  2. All shape swaps of one change set apply in one fixed tick, before the step, in the first tick at or
     after the render activation.
  3. After the swap, `ActivateBodiesInAABox(changed bounds + 0.1 m)` wakes bodies there. Nothing else
     wakes. A sleeping body over a dug hole falls; one on an unchanged block sleeps on.
  4. A body overlapping new solid after `fill` is handled by R24's rescue.
  5. `VoxelVolume:raycast` sees the new data at once, `Physics` ray casts see the old collider until step
     2 (documented; `onCollisionUpdated` fires at the swap).
- **R24 Tunnelling.** Mesh colliders have no thickness, so a body past a triangle would fall forever. Two
  defences, both on by default through `rescueBodies`:
  - **Fast bodies:** a dynamic body in the bounds whose speed exceeds half its smallest extent per tick
    runs `LinearCast` until it slows down (`SetMotionQuality`, its authored field untouched).
  - **Inside-solid rescue:** the SDF is an inside-outside oracle a triangle mesh does not have. Each tick,
    round robin (High 64, Medium 32, Low 16 bodies a tick), a dynamic body or Character Controller has its
    centre sampled. If the centre is deeper than 1.5 voxels in solid for 2 consecutive checks, it moves to
    the first air straight up (a march that skips uniform blocks; no air above: the last free position
    kept for it) plus its half height, and loses its downward velocity. `onBodyRescued` fires and the
    first rescue of a body logs once. Bodies flagged sensor, kinematic or static never move.
- **R25 Navigation.** A NavMesh Surface collects a volume's **level-0 collision geometry** for the bake
  bounds, not its drawn chunks, through `VoxelSystem::appendNavTriangles` (generated chunks are excluded
  from the generic path at `NavBakeCollector.cpp:590`). Policy:
  - Editor: an edit marks intersecting tiles dirty, as moving a collider does; the panel and
    `get_navigation_info` show it; no automatic bake.
  - Runtime with `navigationUpdate = Automatic`: when no edit landed for 1 s (or 5 s after the first), the
    union of changed bounds goes to `NavMeshSurface::updateTiles` on each overlapping surface, at most one
    in flight per surface. Agents keep moving on the old tiles (S19 R10). No overlapping surface: no cost.
  - `Off`: scripts call `updateTiles` themselves. Web tile rules are S19's.

### Functional: editing

- **R26 Operations.** A stateless function `voxel::applyEdit(store, EditOp, options) -> EditResult`, used by
  brush strokes, MCP, Lua and generators alike:
  - **Ops:** `dig` (solid to air, CSG subtract), `add` (air to solid, CSG union; Lua `fill`), `smooth`
    (Jacobi blur of the SDF within the shape: `amount` 0-1, `iterations` 1-8), `paint` (sets slot and
    weight; SDF untouched).
  - **Shapes:** `sphere`, `box` (rotation), `capsule` (two points), `mesh` (a GameObject's mesh or an asset,
    voxelized on first use per R36), `stamp` (a `.voxelvolume` asset placed with rotation and uniform
    scale; its SDF is the shape).
  - **Parameters:** `blend` (smooth-CSG radius in metres, polynomial), `material` and `weight` (absolute),
    `falloff` (0-1, shape edge softness for paint and smooth). No rates, no "strength per second" (D15).
    `dig` and `add` also paint: `add` writes `material`, `dig` leaves material on exposed walls as it was.
- **R27 Results, dry run, validation.** All-or-nothing validation before any write: a shape wholly outside
  the editable region, a slot out of range, a missing asset or an over-budget edit fails the whole call
  with a code: `out_of_bounds`, `invalid_shape`, `invalid_material`, `missing_asset`, `budget_exceeded`,
  `not_loaded`. `EditResult`: `ok`, `error`, `changed_bounds` (voxels and world), `voxels_changed`,
  `surface_voxels_changed` (sign flipped), `blocks_touched`, `blocks_allocated`, `blocks_collapsed`,
  `memory_delta`, `clamped_voxels`, `removed_volume`, `added_volume`, `removed_by_material[16]`,
  `holes_cut`, `holes_filled`, `content_hash_after` (only when asked, as it may resolve cold blocks).
  With `dry_run` the same computation runs on scratch copies and the result is byte-identical; nothing
  changes.
- **R28 Determinism.** The same store, ops and options give the same bytes on every target: `int16`
  output via a fixed round (`floor(x + 0.5)`), no libm in kernels but `sqrt`, sources built with
  `-ffp-contract=off -fno-math-errno` (`VoxelSources.cmake`, the `SplineSources.cmake` pattern), block
  order irrelevant because each block only reads pre-edit data.
- **R29 Editor undo.** One command `VoxelEditCmd` per brush stroke or MCP call (`dig`, `add` ... named with
  the shape), holding old and new block references per touched block and level (the delta per block),
  plus terrain hole deltas in the same step. Undo and redo swap references: O(touched blocks), byte-exact,
  and the content hash returns to the value before. Undo memory counts against R7 and follows S24's depth
  cap. Selection and panel state are not in it.
- **R30 Runtime destruction.** `dig` and friends apply synchronously on the main thread, with one budget per
  call: `maxEditVoxels` (High 2 M, Medium 1 M, Low 256 k samples in the shape's box, mesh shapes counted by
  triangles times samples). Over it returns `budget_exceeded` and changes nothing. Rebuild work is what gets
  budgeted across frames (R15, R18, R21). A call with zero effect allocates nothing and starts no job. No
  Play edit is undoable; `revertChanges()` returns to the loaded state.
- **R31 Events.** `volume.onEdited = function(e)`, fired from `VoxelSystem::update` in the frame of the edit,
  once per applied edit, in order: `e.bounds`, `e.center`, `e.removedVolume`, `e.addedVolume`,
  `e.removedByMaterial`, `e.debrisPoints` (up to `debrisPointCount` per edit, default 16, on a stratified
  lattice of the changed solid samples, seeded by the edit's index, so deterministic). `onCollisionUpdated(bounds)`
  fires at R23 step 2 and `onBodyRescued(go)` at R24. An edit inside a callback applies at once and its
  event comes next frame.
- **R32 Queries.** `raycast(origin, direction, maxDistance)` (SDF sphere trace over level 0, uniform blocks
  skipped; exact to 1/64 voxel; returns `hit`, `point`, `normal`, `distance`, `slot`), `getValueAt(point)`
  (trilinear signed distance in metres; outside the volume: +band), `getMaterialAt(point)` (dominant slot,
  second slot, weight of the second). All main thread, all read blocks synchronously (cold blocks decode on
  demand).

### Functional: generation

- **R33 Generator base.** A volume has a **base**: empty, solid to a height, a Noise Graph, a Terrain or a mesh
  (`generator` in the file), and **modified blocks** over it. An untouched block is a pure function of the
  base at its sample positions, at any level, so a coarse block never needs its fine blocks. Base blocks
  resolve on the mesh job that first needs them, and blocks that resolve uniform stay in the slot. If the
  source changed since the file's stored hash, the panel and `sample_voxels` report `base_stale` (V10).
  Regenerating keeps modified blocks by default (`keep_edits`), or replaces all.
- **R34 From a Noise Graph (T2).** Only through `engine/core/noise/NoiseGraphRuntime.h` (T2 R69, N15; D6, D10):
  the main thread `compile`s the graph (`world` space, `dimensions: 3d`, no grid-only or bake-only node, else
  the panel and `generate_voxels` name the node), and workers share the immutable `CompiledGraph`, each with its
  own `EvalScratch`.
  - A block calls `evaluateGrid3D` with integer sample indices and `unitsPerVoxel = voxelSize * 2^k`, so a coarse
    sample and the fine sample at its position are the same computation (R5). Coordinates are volume-local, so
    moving a volume does not change its base.
  - The named `output_value` `sdf`, times `graphScale`, is taken as distance. A density graph is not a true SDF,
    and R12's normals tolerate that.
  - `rangeOver` clips first: a block whose range is wholly beyond the band resolves uniform with no evaluation.
- **R35 From a Terrain.** A pure `voxel::TerrainSource` interface supplies height, normal and top-2 layers at
  (x, z), adapted from T1's surface query (D1, D5). Base SDF:
  `(y - h + overlapDepth) * c`, with slope factor `c = 1 / sqrt(1 + |∇h|²)`, materials from the terrain's layers through R43's linked
  slots. Option `blend_width` with another base: `sdf = mix(other, terrain, b)`, `b` going from 1 at the
  footprint edge to 0 at `blend_width` inward (smoothstep). So a noise cave or overhang fades to exact terrain
  before the box ends.
- **R36 From a mesh (voxelize).** `voxel::voxelizeMesh(positions, indices, transform, options)`: distances in
  the band from the nearest triangle (`picking/MeshBvh`), sign by majority of ray parity along the 3 axes.
  Disagreement over 1% of samples sets `non_watertight` in the result and the panel. Result is a stamp or a
  base. Runs as a job in the editor.
- **R37 Presets.** Voxel presets are `.noisegraph` files in T2's format and ops declared in code, listed by
  `editor::VoxelPresets` as `editor::TerrainPresets` lists T2's, deterministic by seed:
  - `caves`: worm tunnels at least 2.5 voxels in radius through solid under a height.
  - `overhangs`: domain-warped 3D noise in a height band, giving cliffs and arches.
  - `cave_entrance`: Terrain base plus a seeded capsule chain from the surface to a chamber; by construction
    the chamber connects to the sky (AC30 checks it).
  - `empty`, `solid_ground`.

  Presets take `seed`, a size scale and `blend_width`.

### Functional: terrain integration

- **R38 Attach.** Setting `terrain` (the create dialog offers it whenever the Scene view points at a Terrain)
  defaults to base Terrain, Open bounds, `terrainCut = Automatic`, one linked slot per terrain layer. The
  volume is meant to extend beyond any planned cave by at least 3 blocks' worth of flush surface.
- **R39 The rule.** For a terrain hole-map texel inside the footprint, at its centre `(x, z)` with height `h`
  (T1's surface query, ignoring holes), let `d = sdf(x, h, z)`, trilinear. The texel is **flush** when
  `|d - overlapDepth * c| <= cutTolerance` (`c` the slope factor of R35): the volume's surface coincides with
  the terrain there. **A non-flush texel is cut.** So the terrain stays only where the volume agrees with it:
  a dig, a cave mouth, a mound on top (no terrain poking through later digs, no foliage on it) are all cut.
  A change smaller than `cutTolerance` stays flush and has no visible effect, which the guide says. Outside the
  footprint nothing changes.
- **R40 Cutting.** With `terrainCut = Automatic`, cuts call T1's `cut_holes` and `fill_holes` with an R8 mask (T1
  R12, T3 R27; D4, D7), and T3 makes them hold in render, collision, navmesh, picking and foliage. Consequences:
  - The hole map has no ownership tag (T3 D5, seams), so the volume records which texels **it** cut (file section
    `terrainCut`). Deleting it, disabling it or changing `terrainCut` to Off fills exactly those, and holes a
    person painted stay.
  - Incremental: only texels inside an edit's changed bounds are re-evaluated. Updates batch per change set, at most
    one per terrain per frame (D12, T3 R28).
  - In the editor the `TerrainMapDeltaCmd` and the `VoxelEditCmd` share one undo step (`CommandHistory::mark`,
    `groupSince`).
  - T3 R22 keeps a collider rim of up to one collision cell inside a drawn hole, so an entrance needs to be at
    least 3 collision cells wide. T3 R27's `warnings` come back in `EditResult` as `entrance_narrow`, and the panel
    shows it; `sample_terrain`'s `collision_gap` is how an agent checks.
  - The first cut creates the map and reloads the terrain variant (T3 R29). To keep that off a first in-game dig,
    a terrain with an attached Automatic volume is treated as holed by the collector (`Thl` ships, T3 R34), and in Play it
    keeps its map while such a volume exists, through the non-saved `holeMapHolds` count on the component (T3 R29,
    R49). T3 owns both; `VoxelSystemTerrain.cpp` raises and lowers the count.
- **R41 Hiding the overlap: exact method.** Three parts, all applied by the mesher for an Open attached volume,
  evaluated per vertex from a `coverage` grid at voxel-column resolution:
  1. **Sunk surface.** The seed puts the voxel surface `overlapDepth` (default 0.02 voxel) under the terrain, so
     terrain wins the depth test in the overlap.
  2. **Depth bias.** Chunk materials of an attached volume draw with `depthOffsetUnits = +2,
     depthOffsetFactor = +1`, so a mismatch from CDLOD morph or bilinear curvature still loses to the terrain
     rather than z-fighting.
  3. **Triangle cull.** A triangle is dropped from the render and collision meshes when all 3 vertices are
     within 0.75 voxel of the seed surface and in flush columns more than `ringWidth` from any cut
     column, `ringWidth = max(2 voxels, 1 hole texel)` as T3 asks. So the volume extends a ring under the terrain
     past each hole and draws nothing beyond it, and a cave ceiling under flush ground is untouched because it
     fails the height test.

  A mesh seam is therefore a terrain edge laid over a matching voxel surface a few centimetres below it; no
  gap is possible because the ring is under the edge. The hole-edge step is at most `overlapDepth` plus the
  surface mismatch (`VoxelTerrain.SeamStep` bounds it).
- **R42 Shading parity.** Across the seam both surfaces read the same textures with the same tiling and the same
  tier: for an attached volume the flat-projection UV uses the terrain's origin, size and detail tiling and
  `uvScale`, plus the same tile-break, triplanar sharpness and cutoff, T3's `terrain_height_blend.glsl` over the
  height in colour alpha (T3 R6), and the same Lit lighting code (`HAS_VOXEL` is in `mesh.frag`, V2). Hex and
  stochastic tiling are drawn plain on voxels, as at T3's Medium and Low (Risks).
- **R43 Material slots.** `VoxelMaterialSlot { name, TerrainSurfaceLayer layer, int linkedTerrainLayer = -1 }`:
  the same struct and field names as Terrain Layers, so the panels look alike. A linked slot follows that layer
  live. At most 16, array slices per R50.
- **R44 Ordering, so the player never sees through the ground.** Render: the hole opens at or after the frame the
  voxel meshes showing the cavity activate, never before (the terrain covering a new cavity for a frame is harmless;
  a hole with nothing under it is not); a fill or undo closes the hole first, then removes the
  voxels. Collision: the terrain's collision hole opens only after the voxel collider of that change set is
  live (next tick at the latest), so a player at the lip always stands on one or the other.
- **R45 Foliage and the surface query.** Foliage cannot grow on a cut texel: T5's origin test reads `hole` from T1's
  query (T3 R26, D9), and R39's rule cuts under mounds too. T1's query stays terrain-only (D5). Above it, for callers
  that want the ground wherever it is: `voxel::sampleGround(scene, x, z, yHint)` in
  `engine/core/object/voxel/GroundQuery.h` (always compiled) returns the terrain when not a hole, else a downward
  `raycast` on any volume over that column: height, normal, source tag (`terrain`, `voxel`, `none`). Lua
  `Terrain:getGroundAt(x, z, yHint)`, and `sample_voxels` `points` with `ground: true`. Module off, it is the plain terrain
  query. The consumers in T3 R26's table keep their behaviour: a road bridges a cave mouth, a particle passes through.
- **R46 Terrain edits afterwards.** T1's `TerrainEditEvents` dirty rectangles reaching an attached volume regenerate its unmodified
  base blocks in that footprint and re-evaluate R39 there. Modified blocks stay, which simply makes more of
  them non-flush and therefore cut. Terrain undo does the reverse by the same path. `Re-seed From Terrain`
  in the panel discards modified blocks in a chosen region, one undo step.

### Platform & backends

- **R47 Module.** CMake option `JASPERENGINE_VOXEL` (default ON; the editor, tests and full player have it):
  `engine/core/voxel/VoxelSources.cmake` included under `if(JASPERENGINE_VOXEL)`, `VoxelSystem` registered right
  after `SplineSystem` in `Scene.cpp` under the same condition, Lua bindings under `#if`, the Transvoxel notice
  left out. It **requires `JASPERENGINE_NOISE`** (T2's seam): a CMake error otherwise. Module name `voxel` in
  `PlayerModules`, `variants.json`, `JasperPlayerVariants.cmake`, the switch at `Exporter.cpp:3595` and the Xcode
  replacement at `:4611`, `_MODULE_MACROS` (`tools/lua_api/extract.py:48`) and `lib_size.py`'s groups, with `tools/xcode_sync_voxels.py` following the `xcode_sync_splines.py`
  convention (ids `VOXL` + sha1, `-ffp-contract=off`, `--check`). Variants `full`, `ui` and `core` contain it;
  `lite3d` and `lite2d` do not change (V13). The component header and field table stay in all builds, as
  navigation's do; a player without the module whose content needs it exits 5 (S34 R3).
- **R48 Detection.** `ModuleUsage`: a Voxel Volume component in a scene or prefab, a Lua name under the module's
  `#if`, a script property typed with `VoxelVolume` need `voxel`, and with it `noise` (T2 R43; D18). Component fields
  add others: `collision` adds `physics3d`, `navigationUpdate = Automatic` with a surface adds `navigation`. Evidence
  strings per kind. A project without any of it needs no `voxel` and its build is unchanged.
- **R49 Shaders.** `HAS_VOXEL` and one of `VOXEL_TIER_HIGH|MEDIUM|LOW` are `litDefines`, pushed at
  `RenderSystem.cpp:3904-3917` and appended to `defineCodes`: 4 more, which T3 R54's 64-bit `getDefineBits` has room
  for (34 entries of 64 with T3's 30). They are in `mesh.vert`, `mesh.frag` with bodies in new
  includes `includes/voxel_vs.glsl` and `includes/voxel_fs.glsl`. Lit defines reach only colour shaders (T3 D2;
  `RenderSystem.cpp:3395`), so in `gbuffer.vert` and `gbuffer.frag` `HAS_VOXEL` comes from the property bit `Vox`
  (appended at landing, T3 D2's table) and T3's G-buffer tier bits `Tqm` and `Tql` choose the tier. Depth and shadow passes use the plain mesh
  depth variant, with no voxel code. New attributes `VOXELSLOTS` (`ushort2n`, x holds four 4-bit slots, y spare)
  and `VOXELWEIGHTS` (`ubyte4n`) are appended to `AttributeType`. The vertex shader unpacks slots to floats with
  integer ops on the attribute value, not on a float's bits (D13), and passes them `flat`. Vertex size 32 bytes.
  One uniform block `u_fs_voxelLayers` (16 × 4 vec4), only in voxel variants, which never combine with `HAS_TERRAIN`.
  No compute, no SSBO, no integer texture. Chunks are not in saved scenes, so `editor/shader/ShaderVariantCollector.cpp`
  gets one branch: a Voxel Volume component adds the Lit variants of its tiers, as T3 R34 does for terrain, so builds
  without a runtime compiler reach them.
- **R50 Texture array.** One array per volume, through T3's `TerrainDetailBuilder`
  (`engine/core/terrain/`, T3 moves `getTerrainDetailArray`'s CPU half there), so voxels get its formats, cooked
  compressed slices (T3 R16, S14's exclusion lifted) and height in alpha. It keys by slice, so a volume whose slots are
  all linked uses the terrain's array itself. Max 48 slices (16 slots × 3 maps), which every backend allows (*inferred
  from* `sokol_gfx.h` and GLES3's minimum of 256). Static after build; a slot change rebuilds it whole (D12 is about
  streaming, which this is not). Own-array max size: High 1024 px, Medium 1024, Low 512, with no surface slices at Low.
- **R51 Tiers and fetch budget.** The tier is T3's `TerrainQuality` (High, Medium, Low). Reads of `u_voxelDetail` per
  pixel, worst case, fixed at compile time (no loops over fetches; counted by AC36). Medium and Low equal T3's worst
  case (T3 R1), so an attached volume costs no more per pixel than the terrain beside it:

  | Tier | Layers per pixel | Projection | Maps | Worst-case reads | Typical (flat, 2 layers) |
  |---|---|---|---|---|---|
  | High | 4 | triplanar, cutoff 0.02; flat plane near and far | top 2 layers colour, normal, surface; other 2 colour | 32 | 12 |
  | Medium | 3 | biplanar (2 dominant axes); far on flat colour | colour, normal, surface | 21 | 8 |
  | Low | 2 | biplanar; near only | colour for both, normal for the strongest; roughness from the slot | 6 | 3 |

  Resolution: `quality` if not Auto; else, attached, the terrain's effective tier (T3 R2); else R7's table, which is
  lower than T3's web default because a free-standing volume has no terrain to match. All use `textureGrad`, and all blend
  by T3's height formula, which costs no extra read. Structural settings also come from the tier:

  | Tier | `lodLevels` | `collisionRadius` | Upload per frame | Body cap |
  |---|---|---|---|---|
  | High | 4 | 3 | 16 MiB, 32 meshes | 512 |
  | Medium | 3 | 3 | 8 MiB, 16 | 256 |
  | Low | 3 | 2 | 2 MiB, 6 | 96 |

- **R52 Shadows.** Chunks cast through the normal depth path. Levels that cast: High all, Medium up to 2, Low up to 1.
  Golden includes a transition seam under a low sun with no light leak.
- **R53 Picking.** Chunks are ordinary pick geometry (S26 `PickGeometry`), clicks resolve to the owner through a
  `getGeneratedOwner` branch in `ProjectUtils`, next to splines', and drawn-surface picks match the drawn level.
  Brushes use `raycast` (level 0) so they hit where editing happens.
- **R54 Web and mobile.** Single-thread web runs every path through R15's slices; HTML5 reads the file in place
  from the pack (`JPak::view`), so cold bytes are not doubled. Vertex buffers and arrays are the only GPU objects.
  Nothing depends on `sg_update_image` or mesh partial updates.

### Performance (targets; thresholds in `tools/perf/bench-thresholds.json`, enforced on a quiet run)

- **R55 CPU**, Release, one core of the reference host (8-core x86, this one), `perfSlack()`:

  | Work | Target |
  |---|---|
  | `meshBlock` regular, terrain-like block (500-1500 triangles) | ≤ 1.5 ms |
  | `meshBlock`, worst block (adversarial sign noise) | ≤ 8 ms |
  | Each masked side, added | ≤ 0.3 ms |
  | `CollisionOnly` | ≤ 0.7 × regular |
  | Jolt `MeshShape` of a typical block | ≤ 3 ms |
  | `dig` sphere radius 4 voxels / 16 voxels (`applyEdit`) | ≤ 0.5 ms / ≤ 8 ms |
  | `VoxelSystem::update`, idle volume, main thread | ≤ 0.1 ms |
  | Activation of a change set of 32 blocks, main thread | ≤ 1 ms |
  | Decode one zstd block / evaluate one base block (flat) | ≤ 0.1 ms / ≤ 1 ms |

  Web single-thread: each job ≤ 3× native (measured in the web gate, printed; budgets are per-job, because the
  pump is what slices).
- **R56 Latency from dig to live collision**, sphere radius 3 voxels on a typical surface, 60 Hz fixed step,
  counted in frames with `jobs` set to: 7 workers ≤ 3 frames, 2 workers ≤ 5, 0 workers (web) ≤ 10. Visual update is
  no later than collision. Bench: game frames, not wall time (`docs/testing.md`, "Game time").
- **R57 GPU**, per view, on the reference content (AC40): visible chunks High ≤ 300, Medium ≤ 180, Low ≤ 120;
  triangles High ≤ 600 k, Medium ≤ 300 k, Low ≤ 150 k; one draw per chunk per pass; mesh memory within R7. Counted
  with `get_frame_stats` and the panel's stats, not in milliseconds, because this host has only software GL.
- **R58 Module size.** At most 350 KiB of stripped arm64 code, 130 KiB gzipped on web, measured with
  `tools/players/lib_size.py --modules`. With the module off, or with no component in the scene:
  - module off: the lite variants' file sizes are unchanged to within linker noise, and no `voxel` symbol;
  - module on, no component: `VoxelSystem::update` and `fixedUpdate` are one empty-array check, no allocation, no
    job, no entity, no GPU object;
  - a component disabled: the same, after its chunks go.

### Editor UX (human)

- **R59 Create.** `GameObject > 3D Object > Voxel Volume` (row by `HierarchyWindow.cpp:502`) opens a dialog: size preset
  (Small 64³, Medium 128×64×128, Large 256×128×256, Custom), Voxel Size (1 m), Base (Empty, Solid Ground, From Terrain,
  Caves, Overhangs, Cave Entrance, Noise Graph..., From Mesh...), seed, terrain when one is in view, and a live memory
  estimate with a warning if it exceeds a target's budget. It creates the `.voxelvolume` next to the scene's assets (S04
  GUID), one undo step.
- **R60 Panel and brushes.** Window `Voxel Volume` (`editor/window/VoxelEditWindow.{h,cpp}`, Inspector button "Edit
  Volume"), the grammar of T1 R55-R57 and R62 (D16):
  - Tools `Dig` (D), `Add` (A), `Smooth` (S), `Paint` (P), `Pick` (K); shapes Sphere, Box, Capsule (drag), Mesh, Stamp;
    `[` `]` size. Keys live in a new `ShortcutContext::VoxelBrush`, next to `TerrainBrush` in the dispatcher, rebindable
    through S25, listed in a one-line legend under the tool name.
  - `Ctrl` inverts Dig and Add, `Shift` smooths, Alt+click with no drag picks the material under the cursor and Alt+drag
    orbits (T1 R56's drag threshold; these tools have no alternate, so `Alt Alternates While Painting` changes nothing here;
    `Pick` is also a button and a key). Modifiers are read at stroke start and held.
  - A decal coloured by the effective operation, changing as soon as a modifier is pressed, with T1's Table 4 colours for
    the same meanings: Add white as Raise, Dig dark grey as Lower, Smooth dark red, Paint green, Pick white. It is a draped
    ring plus the shape's wire preview, and it fades and hides as T1's does.
  - A stroke is stamps at arc-length multiples of `spacing = max(0.25 * radius, 0.5 voxel)` (T1's rule), not a rate, so one
    stroke replays the same through a scripted path. Dig and Add are idempotent where stamps overlap, so holding still adds
    nothing. One stroke is one undo.
  - A live info strip like Terrain Info: position, value and material under the cursor, voxels the stamp would touch, memory.
  - Brush presets (Tunnel, Crater, Smooth Wall, Overhang Chisel).
- **R61 Errors in UI.** Missing or corrupt file, missing noise graph or terrain, `base_stale`, over budget, oversize for a
  target, no slots, module off: a red strip in the panel and the Inspector with what to do, and the matching log line
  once. A failed load keeps the file untouched and refuses to overwrite it (S03's lossy-save rule).
- **R62 Materials tab.** Thumbnail list, Add, Remove (refused while the slot is in use, with a Replace With option as one undo
  step), link to a Terrain Layer. Slots keep their indices, so there is no reordering, same field names and order as Terrain Layers, with undo.
- **R63 Slice view.** A dockable panel: axis X, Y, Z, position slider (and scroll), rendered on the CPU per R70: solid and
  air shading, the iso-line, slots as colour, block grid, the volume's and the terrain's outlines, the plane drawn as a gizmo
  in the Scene view. Click moves the Scene camera's pivot there.
- **R64 Overlays and stats.** Overlays: Wireframe, LOD Colours, Blocks, Collision, Holes (T3 R30's), Dominant Material (Scene view line
  and tint draws; editor only). Stats: blocks total, uniform, non-uniform, hot, cold, modified; memory by kind against each
  target's budget; triangles; chunks; draws; jobs pending; collision bodies; last mesh ms; last latency.
- **R65 Inspector.** `editor/window/InspectorVoxels.cpp` with Unity-style labels (Voxel Size read-only, Quality, LOD Levels,
  Cast Shadows, Collision, Terrain, Terrain Cut, Overlap Depth...). Undoable. `Duplicate` of a GameObject shares the asset, with a
  "Make Unique" button (copy with a new GUID).

### Agent UX (MCP, Lua, CLI)

- **R66 Group `voxels`** (D17): one row in `McpToolGroups.cpp`, module row `registerVoxelTools` in `McpTools.cpp`, tools in
  `editor/mcp/tools/VoxelTools.cpp`, `budget.json` entry (`groups.voxels {chars: 4500, tools: 4}`; about 3,040 by script, every tool within `tool_default` with no `exceptions` entry), hidden until enabled, in
  `tool_coverage`. Edit tools do not run while a job on the same volume runs (`volume_busy`). `sample_voxels`,
  `visualize_voxels` and `edit_voxels` are `allowedWhilePlaying` (the last acts on the Play copy and says `play_only: true`);
  the others are not. Longer than 1 s means a job.
- **R67 Creating, by `edit_voxels {create}`**, merged rather than a `create_voxel_volume` tool (T1 R95, as `edit_graph {create}`): `create: {name?, parent?, position?, rotation?, size: [x,y,z] voxels, voxel_size?, base?, seed?, terrain?,
  blend_width?, asset_path?, materials?: [...]}` with the call's `dry_run?`, open in the schema and closed in the handler. Validates everything first. Result: `entity`, `asset`, `dimensions`,
  `estimated_memory`, `over_budget_targets`. One undo step.
- **R68 `edit_voxels`** `{entity?, create?, ops?: [{op, shape: {type, center?, radius?, half_extents?, rotation?, from?, to?, mesh?, stamp?,
  scale?}, blend?, material?, weight?, falloff?, amount?, iterations?}], dry_run?, hash?}`. One undo step per call, whatever the
  op count (D15). Result: per-op and summed R27 fields. Positions are world space. Over 1 s becomes a job.
- **R69 `sample_voxels`** `{entity?, mode, ...}`:
  - `values`: `points` (up to 1024) to `{distance, slot_a, slot_b, weight, inside, source: "base"|"modified"}`; with
    `ground: true`, R45's ground under each point's x, z instead.
  - `box`: `{min, max}` stats: min, max, solid and air fraction, surface sample count, slots seen, or every value at a stride
    (up to 64³).
  - `bounds`: volume bounds, solid bounds, surface bounds.
  - `stats`: R64's numbers, per target's budget.
  - `hash`: content hash (FNV-1a 64 over decoded level-0 samples in canonical order, independent of representation), optionally
    per block; a job if blocks must resolve.
  - `connectivity`: `{from?, to?}` 6-connected air flood at level 0 with uniform-block acceleration: `connected`, `air_components`
    with volume and bounds, `open_to_sky` (a component touching the air above the volume or its top). A job when large.
- **R70 `visualize_voxels`** `{entity?, mode: "slice"|"wireframe"|"lod"|"materials"|"blocks"|"collision"|"holes", axis?, position?,
  max_width?, frame?: bool}`. `slice` is CPU-drawn and byte-deterministic (returns an image plus iso-line length and fractions);
  the others turn the overlay on, frame the volume on `frame`, and return a Scene view image. Images are `ToolResult` images.
- **R71 `generate_voxels`** `{entity?, source: "noise_graph"|"terrain"|"mesh"|"preset", graph?, seed?, terrain?, blend_width?, mesh?,
  transform?, preset?, params?, region?, keep_edits?, dry_run?}`: a job. It resolves every block it touches so that its result has
  real numbers: `blocks_non_uniform`, `memory`, `triangles_estimate`, `content_hash`, `base_changed`, `non_watertight`. One undo step
  (old base description plus block refs). `dry_run` evaluates a coarse level only and says it is an estimate.
- **R72 Guidance.** `docs/mcp/guide/voxels.md` (`jasper://docs/voxels`): volumes, voxel units versus metres, the edit-verify loop
  (`edit_voxels` with `dry_run`, apply, `sample_voxels hash` and `connectivity`, `visualize_voxels`), terrain attachment, budgets,
  errors and how to react. The agent guide gets one entry in its "Other areas" index, `voxels (edit_voxels): jasper://docs/voxels`, no digest, within its 8,192 bytes and 1,500 free (`test_autosync.py`, `tool_budget.py --check`), and the entry's case is AC54's `voxel_find_docs`. `docs/voxels.md` for people.
- **R73 Lua.** `engine/core/script/binding/VoxelClassesLua.cpp`, under `#if`, with `VoxelVolume(scene, entity)` or
  `go:getComponent`:

  ```lua
  local vol = VoxelVolume.fromGameObject(go)
  local r = vol:dig(VoxelShape.sphere(center, 3), {blend = 0.5})   -- r.ok, r.error, r.voxelsChanged, r.bounds ...
  vol:fill(VoxelShape.capsule(a, b, 1), {material = 2})
  vol:smooth(VoxelShape.box(center, halfExtents, rotation), {amount = 0.5, iterations = 2})
  vol:paint(VoxelShape.sphere(center, 2), {material = 3, weight = 1, falloff = 0.5})
  local hit = vol:raycast(origin, dir, 100)         -- hit.hit, point, normal, distance, slot
  local d = vol:getValueAt(p)                       -- metres, negative inside
  local m = vol:getMaterialAt(p)                    -- m.slot, m.second, m.weight
  vol.onEdited = function(e) spawnDebris(e.debrisPoints, e.removedByMaterial) end
  vol:saveChanges("slot1"); vol:loadChanges("slot1"); vol:revertChanges()
  ```

  Plus `VoxelShape.mesh(go)`, `VoxelShape.stamp(path, position, rotation, scale)`, `ensureCollision`, `isCollisionReady`,
  `getStats`, `hasSavedChanges`, `onCollisionUpdated`, `onBodyRescued`. In the Lua API description, doc-tested, with `modules` =
  `voxel`.

### Data & compatibility

- **R74 `.voxelvolume` format.** Little-endian written explicitly field by field (never struct dumps), canonical, version 1 (V9):
  header, generator section (canonical JSON), terrain-cut section (rows of runs), block directory sorted by (level, z, y, x), block
  payloads, and a trailing FNV-1a 64 of every byte before it. Per channel a codec: 0 uniform, 1 raw, 2 zstd, 3 DRLE16. The same
  content with the same library version gives the same bytes.
- **R75 Versions.** A version header of its own (S03 versions only YAML). The reader takes `version <= current` through a migration
  table. A newer major refuses with a message naming both versions; the file is never modified. Truncated or bit-flipped files fail
  with a code, no crash, no out-of-bounds read (fuzz in AC47). A checked-in v1 fixture must load forever.
- **R76 Modified blocks only.** An editor file stores the base description and blocks whose content differs from the base, at every
  level. A block edited back to base equals it and is not stored. So a 256³ volume with one cave is small and a merge of the scene never
  touches it.
- **R77 Writes and identity.** All writes through `writeFileAtomic` (S03). Editor save happens with the scene's save and through a Save
  button, with S05's self-write suppression. An external change to the file reloads (S05) and drops the undo stack of that volume with a
  note. `AssetType::VoxelVolume = 32` (after upstream's ProceduralMaterial took 29; stored in `assetmap.bin`), row in `AssetTypes.cpp` (sidecar GUID, S04), `AssetRefSchema.cpp` row, dependency edges to graph and mesh assets. `*.voxelvolume`
  joins S29's LFS pattern list. S03 Auto Save writes dirty volumes beside the scene backup, and `open_scene` recovery restores both.
- **R78 Cook and export.** The component uses a hand-written runtime codec (`engine/core/serialization/codecs/`, `afterVoxelVolume` hook,
  `RuntimeFields.gen.inc` regenerated) and an editor encoder (`editor/cook/VoxelVolumeCook.{h,cpp}`), for its slots. The asset cook writes
  a **baked** file: all base blocks evaluated at every level, uniform collapse, zstd at a fixed level, generator stripped (kept when
  `runtimeGeneration`), flag `cooked`. So a player evaluates no graph and needs no terrain or mesh source to load it, unless `runtimeGeneration`
  is on; the `noise` module is linked anyway (R47). It goes through JPak unchanged (per-block compression is why JPak's lack of it does not matter). CMake:
  `VoxelSources.cmake`; Xcode: R47's script.
- **R79 Runtime save of player changes.** `saveChanges(slot)` writes `data://voxels/<assetGuid>/<slot>.voxelvolume` (S03 atomic, IndexedDB
  sync on web through `FileUtils`): flags `delta`, the base's content hash, blocks modified since load, the holes this volume changed, codec
  DRLE16, because players have no zstd compressor (`CMakeLists.txt:1142-1146`) and linking one for this is not worth its size.
  `loadChanges(slot)` is all-or-nothing and refuses `base_changed` when the base hash differs. A save is cheap at the scale of budgets: a
  job, with a completion event; it reads immutable blocks, so play continues. Two instances of one asset must use different slots.
- **R80 Old projects.** Nothing changes for scenes with no volume. `ComponentType` entry appended at the end
  (`editor/Catalog.h`); scene version only per S03's rule for new components with defaults (no bump).

### Upstream revision: no CI, lean MCP, Doriax, writes

- **R81 Where each AC runs.** There is no CI (`AGENTS.md`, Test): the gate per commit is `tools/check-changed`, and the full suite runs before the push. Each AC runs on this host or is
  labelled not verifiable here with the command a person runs (§7's tables). A row not run is reported unverified.
- **R82 Lean tools.** Four tools: `edit_voxels` (with `create`), `sample_voxels`, `visualize_voxels`, `generate_voxels`. Each was challenged (T1 R95): `create_voxel_volume` merged; `sample_voxels` already holds what a status tool would;
  `generate_voxels` stays apart from `edit_voxels` as `generate_terrain` does from `edit_terrain` (its own job and result). Sizes by script over lean schemas: 855, 817, 540, 831. Op and shape fields are open in the schema and closed in the handler; descriptions hold
  no field or result lists. A hint an agent must see is `next` in `structuredContent` (`volume_busy`: `get_job`; `entrance_narrow`: widen it). Names follow the gated `verb_noun` rule (`docs/mcp/conventions.md`, `VERBS`), not `AGENTS.md`'s "area noun first". Each tool and the guide entry have cases that fail without them (AC54).
- **R83 Bits and shaders.** `Vox` is appended at landing and pinned by `ShaderKey.VoxelBitIsPinned`. Each commit that changes a shader (`mesh.vert`, `mesh.frag`, `gbuffer.*`, `voxel_*.glsl`) bumps the shader cache version in `editor/App.cpp` by one over its `main`: Lit defines change programs, so the precedent of "no bump" in §3 does not extend to new source.
- **R84 Writes and vendoring.** `.voxelvolume`, delta saves and Auto Save copies go through `writeFileAtomic` (R77, R79), so `tools/raw_writes_allowlist.txt` gets no line. `engine/libs/transvoxel/` needs a `NOTICE.md` row, a `LICENSE`, and an entry in `engine/notices.json` (`AGENTS.md`, Invariants). It gets no patch ledger, because it is verbatim.

## 6. Design constraints & decisions

**V1. Layout (ledger D1, D2).** No existing hotspot grows by more than a hook.

| Path | What |
|---|---|
| `engine/core/voxel/` (no ECS, render or Jolt includes) | `VoxelTypes.h`, `VoxelBlock`, `VoxelStore` (slots, pyramid, COW, hot and cold), `VoxelEdit` (ops, shapes), `VoxelSample` (raycast, values, connectivity, hash), `TransvoxelMesher`, `TransvoxelTables.cpp`, `VoxelClipbox`, `VoxelChangeSet`, `VoxelWorkQueue`, `VoxelFile`, `VoxelCodec` (DRLE16), `VoxelGenerator` (`TerrainSource`, noise adapter), `VoxelVoxelize`, `VoxelCoverage`, `VoxelSources.cmake` |
| `engine/libs/transvoxel/` | `Transvoxel.cpp` verbatim, `LICENSE`, `README.jasper.md` |
| `engine/core/component/VoxelVolumeComponent.h` | Fields, always compiled |
| `engine/core/subsystem/VoxelSystem.{h,cpp}`, `VoxelSystemPhysics.cpp`, `VoxelSystemTerrain.cpp`, `RenderSystemVoxel.cpp` | ECS, jobs, chunks, bodies, holes, GPU binding |
| `engine/core/object/voxel/VoxelVolume.{h,cpp}`, `script/binding/VoxelClassesLua.cpp` | Lua |
| `engine/shaderlib/includes/voxel_{vs,fs}.glsl` | Shader bodies |
| `editor/window/VoxelEditWindow.*`, `VoxelSliceView.*`, `InspectorVoxels.cpp`, `editor/command/type/VoxelEditCmd.*`, `editor/StreamVoxel.cpp`, `editor/reflect/components/VoxelReflect.cpp`, `editor/FactoryVoxel.cpp`, `editor/cook/VoxelVolumeCook.*`, `editor/mcp/tools/VoxelTools.cpp` | Editor |

Hooks, each small: `Scene.cpp` (registration); `RenderSystem.cpp` (`litDefines` push, slot binding call, `isAllLoaded` line); `ShaderVariantCollector.cpp` (one branch); `Render.h` (2 attributes); `mesh.vert`, `mesh.frag`, `gbuffer.vert`, `gbuffer.frag` (`#ifdef HAS_VOXEL` includes, at most 30 lines in total); `PhysicsSystem.{h,cpp}` (the 3 functions of V8); `NavBakeCollector.cpp` (skip chunks, one call); `ProjectUtils.cpp` (owner branch); `HierarchyWindow.cpp` (menu row); `McpTools.cpp`, `McpToolGroups.cpp`, `budget.json`; `PlayerModules.*`, `variants.json`, `ModuleUsage.cpp`, `Exporter.cpp`; `AssetTypes.cpp`, `AssetRefSchema.cpp`. `InspectorWindow.cpp`, `TerrainEditWindow.cpp`, `WorldTools.cpp`, `MeshSystem.cpp`: no change.

**V2. Why the built-in Lit, not a library shader.** A `library:` shader needs no render hook at first sight, but seam parity is the main product question (R42): its lighting would be a second implementation that can differ, and the SSR G-buffer keeps the built-in shader for library shaders (`docs/mcp/guide/shaders.md`). `litDefines` costs no property bit and uses the same lighting, SSAO, fog and IBL code, so nothing can differ across a seam. Trade-off accepted: voxels take no custom material shaders in this spec.

**V3. Why point-decimated sample levels, not averaged or on-demand.** Averaged mips break the corner agreement that transition cells rely on. Decimating on demand (no stored levels) makes a level-3 block read up to 512 level-0 blocks, each a possible decode. Stored levels add at most 1/7 memory (calculated), make coarse meshing touch only its own 27 blocks, and let distant parts of a file stay cold. Tiny features vanish at distance, which LOD is for.

**V4. The SDF band.** Plus or minus 8 voxels means a block more than 8 voxels from the surface is uniform, which is what makes storage sparse (a wider band would keep every block non-uniform) and level 3 still sees correct crossings (R2). 1/4096 voxel is finer than the 1/256 vertex grid, so quantization does not show.

**V5. Transvoxel specifics, reimplemented from the dissertation and tables.**
- *Fixed point.* For an edge with corners ordered by (z, y, x), `t = (d1 << 8) / (d1 - d0)` in `int32`, position in 1/256 voxel units times `2^k`. `int32` holds 1024 × 256 × 8 with room. The local float is `fixedToLocal(fx) = fx * voxelSize / 256`, one function, so equal inputs give equal bits (R11).
- *Corners of a transition cell.* Corners 0-8 are level-k samples on the block face (a 2×2 group of level-k cells); 9-C have the values of 0, 2, 6, 8 per Lengyel. Since level k+1 is point-decimated these are the coarse block's samples (R5).
- *Low-resolution face vertices.* Corners 9-C stay on the block boundary, so these vertices are computed by the same edge function at spacing `2^(k+1)` with level-(k+1) gradients: bit-equal to the coarse block's vertices. The job's read view therefore includes level k+1 for masked sides.
- *High-resolution face vertices.* Computed by the regular-cell function on the boundary plane, then moved inward by the secondary function below.
- *Secondary position.* In the boundary cell layer of a masked side, per axis, `Δ = (1 - p/2^k) * w` at the low end, mirrored at the high end, `w = 0.5 * 2^k`. Then project onto the tangent plane, `(I - n nᵀ)Δ`, and clamp to one cell. Function of (primary position, normal, level, mask) only.
- *Which vertices move.* A vertex moves only if its cell touches a masked side and it lies on no unmasked block side. So neighbours across an unmasked side, same level, agree automatically: both leave it in place (`VoxelSeam.*` audits this).
- *No shader step.* Vertices are final at build time for the current mask. So a mask change is a remesh, which is why it goes in a change set. Upstream's alternative (secondary in an attribute, `floatBitsToInt` culling in the vertex shader) needs a per-chunk uniform, a vertex path in every pass and float-bits packing, which D13 bans. Churn is bounded by hysteresis and the budgets; fine blocks are the only ones to remesh (R10).

**V6. How a seam never cracks.** The report's problems and their answers:

| Known problem | Answer |
|---|---|
| Dropped tasks leave holes | The old set stays visible until the whole new set is uploaded; a dropped task means the set never activates and is rebuilt (R17) |
| Priority order affects cracks | Priority affects only when a set finishes, never what is visible (R17); AC15 shuffles completion order |
| Blocks at mixed data versions | A set has one data version; edits rebuild every block whose samples or padding changed, as one set (R17) |
| Fast movement stutters | One LOD set at a time, hysteresis, mesh cache, upload budget; a lagging set means coarse near the camera for a moment, never a hole |
| Float rounding at block edges | Fixed-point positions and one model matrix (R11) |

The invariant is testable with no GPU: `voxel::auditWatertight(visibleMeshes)` welds by fixed-point position and requires every directed edge to balance with its reverse. Degenerate triangles preserve the balance, and the outer shell is excluded only for Open volumes. It runs at every activation of the soak (AC16) and in debug builds of the editor behind `JASPER_VOXEL_AUDIT=1`.

**V7. Bodies as chunk components.** A chunk's body is a real `RigidbodyComponent` (static, one `Collider3D` holding the prebuilt shape), so contact events, ray casts, layer filters, debug lines and removal all work unchanged; `packBodyUserData` keeps naming the chunk. `VoxelVolume.fromGameObject` is the bridge to the owner.

**V8. Physics hooks.** On `PhysicsSystem`, under `JASPERENGINE_PHYSICS_3D`:

```cpp
// a static body of an already built shape for this entity, not yet in the broad phase
JPH::BodyID createStaticBodyForShape(Entity entity, const JPH::Shape* shape);
// thread-safe per Jolt's contract (BodyInterface.h:120-123); finalize between steps, main thread
JPH::BodyInterface::AddState prepareBodies(std::span<JPH::BodyID> ids);
void finalizeBodies(std::span<JPH::BodyID> ids, JPH::BodyInterface::AddState state);
```

Everything else (`SetShape`, `ActivateBodiesInAABox`, `SetMotionQuality`, `RemoveBodies`) is reached through `getWorld3D()` (`PhysicsSystem.h:770`) from `VoxelSystemPhysics.cpp`, asserting `!isSteppingWorld3D()`. `VoxelSystem` is registered before `PhysicsSystem`, so its `fixedUpdate` runs before the step.

**V9. File layout.** All little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `JVOX` |
| 4 | 2 | format version (1) |
| 6 | 2 | header bytes |
| 8 | 4 | flags: bit 0 `delta`, bit 1 `cooked`, bit 2 `open_bounds` |
| 12 | 6 | dimensions in blocks, 3 × `u16` |
| 18 | 1 | levels |
| 19 | 1 | slot count |
| 20 | 4 | voxel size, `f32` |
| 24 | 8 | base content hash (FNV-1a 64; 0 when none) |
| 32 | 8 | generator section offset, bytes (`u32`, `u32`) |
| 40 | 8 | terrain-cut section offset, bytes |
| 48 | 8 | directory offset, entry count |
| 56 | 8 | payload offset, bytes |

A directory entry is 28 bytes: `level u8, sdf codec u8, material codec u8, flags u8, x y z u16, sdf uniform i16, material uniform u16, payload offset u32, sdf bytes u32, material bytes u32, 2 reserved`. A payload is decoded with bounds checks and must produce exactly 8192 bytes. DRLE16: delta from the previous sample, zero-run tokens, one hand-written file, under 150 lines. Footer: FNV-1a 64 of all prior bytes. Readers skip sections they do not know through the header's offsets, which is how a minor addition stays compatible.

**V10. Base staleness.** The generator JSON stores the source's GUID and a hash of the source that matters: Noise Graph content hash plus seed, Terrain height and blend hash over the footprint, mesh content hash. The check runs at load in the editor and on `sample_voxels stats`. Staleness never changes data by itself (determinism first): modified blocks and the cook warn, and `Regenerate` is explicit, except for R46's live terrain link.

**V11. Tiers are defines, not uniforms.** A tier change makes a new variant and costs no branch per pixel. Medium reads the 3 largest weights and Low the 2 largest (vertices are sorted per R13, so the first ones) and renormalizes. Fetch counts stay compile-time.

**V12. API sketch.**

```cpp
namespace jasperengine::voxel {
    struct VolumeDesc { Int3 dimensions; float voxelSize; int levels; BoundsMode bounds; };
    class Store;                                  // main thread writes, jobs read through ReadView
    struct ReadView;                              // block refs for one mesh job: 27 at level k, up to 27 at k+1
    struct EditOp { EditKind kind; Shape shape; float blend; int material; float weight, falloff, amount; int iterations; };
    EditResult applyEdit(Store&, std::span<const EditOp>, const EditOptions&);     // options.dry_run, .hash, .max_voxels
    struct MeshRequest { BlockKey key; uint8_t sideMask; MeshMode mode; const Coverage* coverage; };
    bool meshBlock(const ReadView&, const MeshRequest&, MeshOut&);                 // pure, thread-safe
    AuditResult auditWatertight(std::span<const MeshOut* const>);
    VolumeFileResult loadVolumeFile(std::span<const uint8_t>, Store&);
    std::vector<uint8_t> saveVolumeFile(const Store&, const SaveOptions&);         // cook, editor, runtime delta
    struct TerrainSource { virtual float height(float x, float z) const = 0; ... };
}
```

**V13. Variants.** No new smaller variant. A voxel project with no HTML UI gets `core` (about 1.3 to 1.8 MiB larger than `lite3d`, calculated from the reductions at `S34:360-361`, before `noise` and `voxel` themselves). Every extra variant is built for every platform, and voxel games are not the size-critical ones. Revisit from measurements, not by default.

**V14. Milestones.** One implementer, in this order, each landing separately through `tools/check-changed`:
1. Module, store, pyramid, edits, queries, file (R1-R8, R26-R32, R74-R76): CPU only.
2. Mesher, tables, clipbox, change sets, audit, queue (R9-R17): CPU only.
3. Chunks, `VoxelSystem`, shader, array, tiers, shadows, picking (R18-R20, R49-R54).
4. Physics, navigation (R21-R25).
5. Generators and presets (R33-R37): needs T2. Terrain integration (R38-R46): needs T1 and T3.
6. Editor, MCP, Lua, cook, export, saves (R47-R48, R59-R73, R77-R80).

**V15. Editing and threads.** The main thread calls `applyEdit`, which may use `jobs::parallelFor` per block (`smooth` reads pre-edit data, so output is independent of order). Base resolution inside an edit happens inline. `generate_voxels` is the only long write and it holds the volume `volume_busy`; a rule, not a lock.

## 7. Acceptance criteria

Suites: CPU under `engine/tests/core/voxel/`, GPU `*Gpu` with goldens in `engine/tests/core/golden/` (made on glcore, matched on Vulkan), editor under `editor/tests/voxel/`, MCP in `tools/mcp/tests/test_voxels.py` (`CI = True`, own project via `mcp --create`, never fixture-unity). **Red** names the permanent negative case or the local revert that proves the test can fail; a test option disabling a safeguard (`Options::dirtyPadding = 0`) is compiled for tests only.

| AC | R | Verification |
|---|---|---|
| AC1 | R1 | `VoxelComponent.DefaultsAndRoundTrip`: defaults, scene YAML round trip byte-identical; a non-unit scale refused by the tool and flagged by the Inspector schema |
| AC2 | R2, R3 | `VoxelBlock.EncodingAndClamp`, `VoxelBlock.MaterialPacking`: ±8 voxels clamp, sign, nibble layout, exact values |
| AC3 | R4 | `VoxelBlock.UniformCollapse`, `VoxelStore.SlotTableSize`: a filled-then-dug block collapses and frees 16 KiB; a 1024×512×1024 store's tables = 1 MiB plus levels ± 5 %. Red: skip collapse, memory assertion fails |
| AC4 | R5 | `VoxelStore.PyramidIsPointDecimation`: after 1000 random edits every level sample equals level 0 at its position. Red: an averaged level fails |
| AC5 | R6 | `VoxelStore.JobReadsOneVersion` under `SANITIZE=thread`: 4 readers, a writer, no race; a held `ReadView` keeps old bytes |
| AC6 | R7 | `VoxelStore.BudgetRefusesBeforeWrite`, `VoxelStore.EvictsUnmodifiedFirst`; `VoxelExport.OversizeFailsNamingTarget` (editor); `VoxelFile.TooLargeDoesNotLoad` |
| AC7 | R8 | `VoxelEdit.ClosedShellStaysAir`, `VoxelEdit.OpenMarginStaysBase`, with `clamped_voxels` exact |
| AC8 | R9, R12 | `TransvoxelMesher.Sphere`: vertices within 0.02 voxel of the analytic sphere, normals within 2°, `TrianglesFaceAir` ≥ 99.9 % by SDF gradient. `CollisionOnly` gives the same triangles. Red: inverted case bit fails |
| AC9 | R10, R11 | `VoxelSeam.SharedVerticesBitEqual` (same level, all faces), `VoxelSeam.TransitionMatchesCoarse`: bit-equal positions and normals on the low-res face, for all 6 sides and combinations of masks. Red: a shifted `fixedToLocal` for one side fails |
| AC10 | R10 | `TransvoxelMesher.CoarseNeverRemeshesForNeighbour`: scripted LOD move counts mesh requests per block |
| AC11 | R13 | `VoxelMesher.MaterialSetsPerCell`: 4 per vertex, sums 255, sorted, deterministic |
| AC12 | R14 | `VoxelMesher.IndexLimitFallback`: adversarial sign and material noise: ≤ 65,535 vertices always; the fallback ran (counted). Also the attribute layout is 32 bytes |
| AC13 | R15 | `VoxelWorkQueue.SingleThreadStaysInBudget` (`JE_JOB_WORKERS=0`): with 200 queued blocks, per frame the pump overruns by at most one job; `VoxelWorkQueue.FocusReprioritizes`; `VoxelWorkQueue.InFlightCap` |
| AC14 | R16 | `VoxelClipbox.Partition`: over 10,000 random focus positions, visible blocks partition the volume exactly and 26-neighbours differ ≤ 1 level. `VoxelClipbox.Hysteresis`: oscillation inside the band moves nothing |
| AC15 | R17 | `VoxelChangeSet.CancelledJobsKeepOldVisible`, `VoxelChangeSet.ShuffledCompletionNeverCracks` (seeded completion order, a fake scheduler), `VoxelChangeSet.EditPreemptsLod` and `.AtMostTwoInFlight`. Red: activate per block as it finishes, the audit fails |
| AC16 | R11, R17, R26 | **Soak** `VoxelSoakSlow.IncrementalEqualsRebuild`: seeded 5000 random edits (all ops and shapes, near and across block faces) with a moving focus and shuffled jobs. At each activation `auditWatertight` holds. Every 250 edits every visible mesh equals one rebuilt from scratch (sorted triangles, byte-identical), the pyramid matches, the file round trip's hash matches. Run also with ASan and TSan. Red: `Options::dirtyPadding = 0` makes it fail, kept as `VoxelSoakSlow.RedControlMissingPadding` |
| AC17 | R18 | `VoxelSystem.UploadBudgetPerFrame`: bytes and counts per frame within the tier; a chunk larger than the budget still goes |
| AC18 | R19 | `VoxelSystem.ChunksNotSavedNotInHierarchy` (editor): scene saved with a volume holds no chunk; hierarchy unchanged; undo of nothing. MCP `test_voxels.py::test_scene_bytes_stable`: reload, save: byte-identical |
| AC19 | R20 | `VoxelSystem.FirstSetBlocksIsAllLoaded`, `VoxelPhysics.ColliderBeforeFirstStep`: a body on the surface at frame 1 does not fall |
| AC20 | R21 | `VoxelPhysics.ShapeBuiltOffMain` (thread id of the build), `VoxelPhysics.BatchAddMatchesSingleAdd` (ray casts equal), `.BudgetPerTick`, `.EmptiedBlockRemovesBody` |
| AC21 | R22 | `VoxelPhysics.BubbleAroundDynamicBody`: a body thrown into a far level-3 region stops on level-0 collision; `.EvictionSkipsBlocksWithBodies`; `.EnsureCollisionBlocks` |
| AC22 | R23 | `VoxelPhysics.RebuildContract`: scripted dig under a sleeping body: pre-swap still supported (no fall), swap in one tick, body wakes and falls; a body 3 blocks away stays asleep; `onCollisionUpdated` fires. Red: remove before build, the body falls early |
| AC23 | R24 | `VoxelPhysicsSlow.Tunnelling`: 1,000 bodies from 5 to 200 m/s at a wall one voxel thick and at the ground: after 300 ticks none is below or deep in solid; rescues counted. Same with an `add` swallowing bodies. Red: `rescueBodies = false` shows bodies escaping |
| AC24 | R25 | `VoxelNav.CollectsLevelZeroNotDrawn`: bake output equal with focus near and far. `VoxelNav.AutomaticUpdateAfterSettle`: dig blocks a corridor, path reroutes after debounce (game time); `Off` does nothing. MCP: `get_navigation_info` dirty tiles after an edit |
| AC25 | R26 | `VoxelEdit.OpsMatchAnalytic`: dig, add, smooth, paint with sphere, box, capsule against analytic SDFs within 1/256 voxel; mesh and stamp against equivalent primitives |
| AC26 | R27 | `VoxelEdit.DryRunEqualsApply` for all fields over 200 random ops (byte-equal result, store unchanged); `VoxelEdit.ValidationAllOrNothing`: every error code, store hash unchanged |
| AC27 | R28 | `VoxelDeterminism.GoldenHash`: fixed op script's hash equals a recorded constant on Linux glcore and Vulkan builds and on wasm (`tools/test-projects/voxels` printing through the web gate), here; arm64, Windows and macOS by the command in the host table, unverified until run. Red: compiling one file with `-ffp-contract=fast` on an FMA target is the documented local check on x86-64 with FMA, run here |
| AC28 | R29 | `VoxelEditCmd.UndoRedoExact` (editor): 100 commands, undo all, hash back; redo all, hash same; memory counted. MCP: one call with 5 ops is one undo step (`get_undo_history`). Hole deltas undone with it |
| AC29 | R30, R31 | `VoxelRuntime.PerCallBudget`, `.NoOpAllocatesNothing` (`AllocationCounter`), `.EventOrderAndDebrisDeterministic` (Lua), `.RevertChanges` |
| AC30 | R37 | `test_voxels.py::test_presets`: each preset twice with one seed: hashes equal. `cave_entrance`: `sample_voxels connectivity` from sky to chamber `connected: true`; `caves` lists components; `overhangs` has an air sample under solid in a column |
| AC31 | R32 | `VoxelSample.RaycastMatchesMesh` within 1/64 voxel vs a triangle ray cast at level 0; `getValueAt` vs analytic; `getMaterialAt` after paint; Lua binding test |
| AC32 | R33, R34 | `VoxelGenerator.CoarseBlockWithoutFine`: a level-2 block resolves with no level-0 resolved; equals decimation of fine blocks. `VoxelGenerator.NoiseGraphGolden` (T2 golden). `.BaseStaleReported`; `keep_edits` keeps modified blocks |
| AC33 | R35, R36 | `VoxelGenerator.TerrainSeedFlush`: over a fractal terrain the seed's surface stays within `overlapDepth + cutTolerance`. `.BlendReachesTerrainAtEdge`. `VoxelVoxelize.Sphere` vs analytic; `.NonWatertightFlagged` |
| AC34 | R38, R39, R40, R46 | `VoxelTerrain.AttachDefaults`, `VoxelTerrain.CutsOnlyNonFlush`, `.RemovalRestoresExactly` (a person's holes survive, map hash back), `.TerrainSculptRecutsIncrementally` (counted texels), `.HoleMapHeldInPlay`, `.EntranceNarrowWarns`, one undo step for voxels and holes. `VoxelExport.AttachedShipsHoleVariant`. Needs T1 and T3 |
| AC35 | R41, R42, R43, R44 | `VoxelTerrain.CullKeepsCeilingAndRing`, `VoxelMaterials.LinkedSlotFollowsLayer`, `VoxelMaterials.SharesTerrainArray`, `VoxelTerrain.SeamStep` (CPU: step ≤ `overlapDepth + 0.1 voxel` around a cave mouth), `VoxelTerrain.OrderHoleAfterMesh`, `.CollisionHoleAfterCollider`: a body rolled over the lip neither falls nor stops. `VoxelTerrainGpu.CaveMouth` golden; `VoxelTerrainGpu.SeamColourStep`: across the hole edge colour difference within 3 % of the difference across a same-terrain strip, High and Low. Red: no sink plus no bias makes `CaveMouth` z-fight |
| AC36 | R51 | `VoxelShader.FetchBudgetPerTier`: each tier's variant compiled to SPIR-V; `OpImageSample*` on `u_voxelDetail` counted: 32, 21, 6 at most. `VoxelRenderGpu.TierGoldens` per tier: sphere with 4 slots. Red: a High variant with one extra fetch fails |
| AC37 | R45 | `VoxelTerrain.FoliageExcludedOnCut`, `.SampleGroundFallsBackToVoxels`, Lua `getGroundAt`, `GroundQuery.TerrainOnlyWithModuleOff` (compiled in the module-off build of AC44) |
| AC38 | R47, R48 | `ModuleUsage.Voxel*` for each evidence kind and field; `PlayerModules.*` knows `voxel`; `ContentManifest.*`: a manifest needing `voxel` on `lite3d` exits 5, the payload validator refuses it; `cmake -S engine -DJASPERENGINE_NOISE=OFF -DJASPERENGINE_VOXEL=ON` exits non-zero (command and output in the report); `xcode_sync_voxels.py --check`; `tools/ci/check-notices.py` |
| AC39 | R49, R50, R52, R53 | `VoxelShader.CrossCompilesForEveryBackend` (GLSL 410, ESSL 300, MSL, HLSL through the bundled SPIRV-Cross, no driver); `VoxelRenderGpu.LodSeamGolden`, `.ShadowsNoLeak`, glcore and Vulkan (lavapipe); `VoxelExport.CollectorListsTierVariants`; T3's `MaterialPbrDefines.BitsAreSixtyFourWide` over the 34 defines; `VoxelPicking.ClickSelectsOwner` (editor) |
| AC40 | R57 | `test_voxels.py::test_budgets_cave_world`: reference content (256×128×256, 1 m, Cave Entrance over a terrain) from 3 cameras: chunks, triangles and memory per tier within budget; prints `[bench]`. Reference content is generated by the test |
| AC41 | R55 | `VoxelBench` (`je-bench-voxel`) with thresholds in `bench-thresholds.json`, gated by `tools/perf/check_bench.py` on a quiet run. Red: lowered thresholds locally show a fail |
| AC42 | R56 | `VoxelLatencyBench`: `JE_JOB_WORKERS=7,2,0`, frames counted; thresholds per case |
| AC43 | R54 | `tools/test-projects/voxels` with `tools/web/gates/voxels.json`: `GATE=voxels tools/build.sh web-gate`, single-thread and pthreads: dig, screenshot centre changes, ground collision holds, `[gate] voxels` line; hash printed for AC27 |
| AC44 | R58 | `VoxelSystem.NoComponentNoCost` (`AllocationCounter`, no entity, no job, no GPU object); `tools/players/lib_size.py --modules` table in the implementer's report, Linux, web and Android arm64 (library built, not run); `nm` of a module-off player has no `voxel`; S34's player CLI golden unchanged |
| AC45 | R59-R65 | Editor suite: `VoxelCreate.DialogEstimateAndUndo` (R59); `VoxelBrush.CtrlInvertsAndDecalColour`, `.AltClickPicksAltDragOrbits`, `.StrokeSpacing`, `.ScriptedStrokeSame`, `.ShortcutContextRebindable` (R60); `VoxelPanel.ErrorStripPerCode` (R61); `VoxelMaterials.RemoveRefusedInUse` (R62); `VoxelSlice.DeterministicImage` (R63); `VoxelPanel.StatsMatchSample` (R64); `InspectorVoxels.UndoableFields`, `.MakeUnique` (R65). Evidence: screenshots from an isolated editor through `tools/editor-shot.sh` of the dialog, panel, Slice view, each overlay and the error strip |
| AC46 | R66-R72 | `test_voxels.py`: `::test_group_hidden_until_enabled`; `::test_create` (R67: validation, undo); `::test_edit` (R68: every code, `dry_run`, batch is one step); `::test_sample` (R69: every mode, `hash` stable, `connectivity` on a known tunnel and a sealed cavity); `::test_visualize` (R70: slice image hash stable twice, overlay images non-empty); `::test_generate_job` (R71: all sources, undo, cancel); `::test_edit_in_play_leaves_edit_mode` (R66); `::test_guide_served` (R72). Also `test_tool_budget.py`, `test_docs.py`, `test_tool_coverage.py`, `test_autosync.py` |
| AC47 | R74, R75 | `VoxelFile.RoundTripCanonicalBytes`, `.LittleEndianExplicit` (golden bytes for a tiny file), `.LoadsV1Fixture`, `.RefusesNewerMajor`, `VoxelCodec.Drle16RoundTrip`; `VoxelFileSlow.FuzzTruncateAndFlip` under ASan (10,000 mutations, every result a code, no crash) |
| AC48 | R76 | `VoxelFile.ModifiedOnly`: one dig in a 256³ noise-based volume: file under 200 KiB; editing back to base stores nothing |
| AC49 | R77 | Editor: `VoxelAsset.GuidSidecarAndRefs`, `.AtomicWriteOnFailure` (S03 fault injection), `.ExternalChangeReloads`, `.AutoSaveRecovery`; `.gitattributes` writer holds the pattern |
| AC50 | R78 | `VoxelCook.BakedLoadsWithoutSources`: player run (`run_build`, prebuilt) of a project with a noise-based volume, hash equals edit mode, no graph evaluated (counted), `tools/smoke-test.sh` stays green. `CookedScene` field round trip; `RuntimeFields.Generated` passes |
| AC51 | R79 | `VoxelRuntime.SaveLoadChanges`: dig, save, new scene, load: hash equal. `.RefusesBaseChanged`, atomic on failure, `DRLE16` only. In a player build with no zstd compressor linked (`nm`) |
| AC52 | R80 | S34's `upgrade-from-main`, `tools/smoke-test.sh`, the test project goldens with no volume unchanged |
| AC53 | R73 | `VoxelLua.*` for each function; `tools/lua_api/doctest.py` for `docs/voxels.md` and `jasper://docs/voxels` blocks |
| AC54 | R82, R72 | Cases in `tools/mcp/eval/cases.py`, run as T1 AC74 says (`-k voxel_`; lent accounts, else reported unverified). Red: a surface from before the tool. `voxel_find_docs` (guard; group off, "add a destructible cave": enables `voxels` or reads the doc), `voxel_create_and_dig` (bad; "make a volume and dig a tunnel": one `edit_voxels` with `create` and `ops`, then `sample_voxels connectivity`), `voxel_generate_from_graph` (guard; `generate_voxels` source `noise_graph`, not ops by hand), `voxel_control_hill_is_terrain` (control; "raise a hill": `edit_terrain`, must not call `edit_voxels`), `voxel_cave_mouth_not_holes_only` (guard; "cave entrance": a volume with terrain attachment, not `cut_holes` alone) |
| AC55 | R83, R84 | `ShaderKey.VoxelBitIsPinned`; `AssetTypes.VoxelVolumeValueIs32`; `python3 tools/ci/check-notices.py` (fails with a missing `engine/notices.json` entry); `python3 tools/check_raw_writes.py` with no new allowlist line; `python3 tools/check_stream_keys.py`. Red: `tools/check_raw_writes.py --self-test`; removing the notices entry fails the first |
| AC56 | §8 | Second-lander checks: with `draw-perf`, voxel Lit keys ship with `Dlo` twins (`VoxelExport.CollectorListsTierVariants` extended); with `ssr-probes`, a voxel G-buffer variant builds with `Occ` off (`VoxelRenderGpu.LodSeamGolden` passes with no sokol validation error); with `mesh-memory`, `VoxelChangeSet.*` and `VoxelRenderGpu.*` green on its containers |

**What this host can verify** (8 cores, software GL, no KVM, no Windows or macOS; the ledger's limits; there is no CI, so nothing else checks a push):

| Verified here | How |
|---|---|
| All CPU logic (data, mesher, seams, soak, edits, file, physics, navigation, bench thresholds) | `tools/build.sh unit-tests`, with `SANITIZE=address,undefined` and `thread`; benches only on a quiet queue |
| GL 3.3 and Vulkan images | `*Gpu` suites on llvmpipe and lavapipe, Xvfb |
| GLES3 path | `SMOKE_BACKEND=gles3` smoke through `linux-backends`; WebGL2 through the web gate |
| HTML5 single-thread and pthreads | `tools/build.sh web`, web gate |
| Android library, player and size | `tools/build.sh android`: **built, not run** (no emulator without KVM) |
| Metal and D3D11 shaders | SPIRV-Cross translation succeeds (AC39); **translated, not compiled by a driver** |
| Budgets in counts: draws, triangles, fetches, bytes, frames | Exact here, whatever the GPU |

| Not verifiable here | Who |
|---|---|
| Draw time and fill cost on real GPUs; mobile thermal | Manual list `docs/specs/checklists/T6.md` on a device, or the owner |
| Windows build and CPU suites | With `vcvars64`: `cmake -S engine -B build-engine-gl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DJASPERENGINE_TESTS=ON && cmake --build build-engine-gl --target jasperengine-core-tests && build-engine-gl/tests/core/jasperengine-core-tests.exe --gtest_filter=Voxel*:Transvoxel*-*Gpu*`; unverified until run |
| macOS build, CPU suites, arm64 determinism, Metal goldens | `cmake -S engine -B build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON && cmake --build build-engine --target jasperengine-core-tests && build-engine/tests/core/jasperengine-core-tests --gtest_filter=Voxel*:Transvoxel*` (Metal, `docs/testing.md:492-493`); unverified until run |
| iOS | `python3 tools/xcode_sync_voxels.py --check` runs here; building and running `tools/test-projects/voxels` needs a Mac and the Xcode workspace; unverified until run |
| Android run | On a host with `/dev/kvm`: `ANDROID_PROJECT=voxels ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh`; unverified until run |

## 8. Dependencies & conflicts

- **Depends on:** S06 (jobs), S03 (atomic), S04 (GUIDs), S30 (cook), S34 (modules), S19 (tiles), S26 (picking), S24 (undo).
  Landing order is T1 §8's: T1's foundation first, then T1's rest, T2, T3, T5 and T6 in parallel, then T4 last.
  **Milestones 1 and 2 need nothing new.** Milestone 3 needs T3's `TerrainDetailBuilder` and `TerrainQuality` (or, if T6 runs first, it moves them per T3's design and T3 follows). Milestone 5 needs **T2** (noise), **T1** (operations and query), **T3** (holes).
- **What T6 takes from those specs (seams, read in their current text):**
  - **T1:** `querySurface` and `terraincore::SurfaceSample` (`hole`, height defined under a hole, T3 R26), `cut_holes` and
    `fill_holes` with an R8 mask (T1 R12), `TerrainEditEvents` dirty rectangles (T1 R42), `TerrainMapDeltaCmd` in an undo
    group (T1 R24), the brush grammar (R55-R57, R62, Table 4). A voxel volume does not store its data in `TerrainMapStore`.
  - **T2:** `NoiseGraphRuntime.h` only (R69, N15), `rangeOver`, `evaluateGrid3D`, one `EvalScratch` per worker, output `sdf`;
    `JASPERENGINE_VOXEL` requires `JASPERENGINE_NOISE`; presets as `.noisegraph`.
  - **T3:** `TerrainQuality`, `TerrainDetailBuilder`, `terrain_height_blend.glsl`, R22's rim and R27's result, R28's pacing.
    T3 owns what R40 needs of it: R29 keeps the hole map while `holeMapHolds > 0`, and R34's collector ships `Thl` for a terrain
    with an attached Automatic volume. If T3 lands first, T6 adds R34's clause in T3's form (T3 R34).
  - **T4:** a volume over a tiled world queries through `querySurface` and cuts through its store. It is not tested until T4
    lands. T4 owns world paging; T6 does not page, and a volume crossing a tile edge cuts on each tile's map.
  - **T5:** reads holes through T1's query (D9); voxel foliage is out of scope on both sides (T5:41).
  - **S34:** `voxel` joins the module tables, at the end of `allModules`.
  - **Provides to others:** `voxel::sampleGround` (R45), the `.voxelvolume` format, `generate_voxels`' schema.
- **Merge hotspots:** `engine/CMakeLists.txt`, `Scene.cpp:59-60`, `RenderSystem.cpp:3904-3917` (T3 adds defines there too), `mesh.frag`, `gbuffer.frag`, `Render.h` (append only), `ModuleUsage.cpp`, `Exporter.cpp`, `budget.json`, `McpToolGroups.cpp`, `NOTICE.md`, `variants.json`, `RuntimeFields.gen.inc`, `AssetTypes.cpp`, `AssetType.h`, `engine/notices.json`.
- **Doriax overlap (D21; none landed: whichever lands second merges onto the first).** `agent-authoring`: no terrain tool in common; its guide restructure takes `voxels`' index entry. `render-correctness`: G-buffer variants and the variant match; `HAS_VOXEL` joins through T3's helper. `draw-perf`: `Dlo` on the built-in Lit reaches voxel chunks (a sun-only scene), so `voxel_fs.glsl` obeys its guards and the collector ships the twin; its state sort sees voxel chunks as ordinary meshes. `ssr-probes`: G-buffer `Occ` and `Uv2` beside `Vox`; a chunk has no occlusion map, so `g_occlusion` stays off and `frag_albedo.a`'s code is written as for any mesh. `mesh-memory`: V7 and R19's chunk `MeshComponent`s use its containers; keep indices across writes, and assume no capacity.
- **Pixel art.** `AssetType` 24-27 are theirs. S52's and S41's bits append to the same MESH and DEPTH masks, by name. `AttributeType` gains S52's and T6's entries: append only. No shared names.

## 9. Risks & defaults

| Risk or question | Default taken |
|---|---|
| Ghost contacts at block boundaries: separate bodies cannot share Jolt's active-edge data, so a ball can bump at a block seam (*inferred*) | `VoxelPhysics.BallRollsAcrossBlockSeam` measures it (speed loss ≤ 5 %, height deviation ≤ 0.02 voxel). If it fails: 8-block collision groups (one body per 32³), which is a local change to R21 and a Deviation. Character Controllers are expected to be fine (step logic) |
| Mesher speed is unmeasured (upstream publishes no figure) | R55 budgets are targets. A miss is recorded as a Deviation with the number; thresholds are not raised silently. First tool to reach for: uniform-pair early out per cell |
| AddBodiesPrepare during a step | Jolt documents it as background-safe (`BodyInterface.h:120-123`). Inferred from `BroadPhaseQuadTree.cpp:154-205`, it touches only its bodies' tracking. TSan in AC20 confirms; the fallback is Prepare on the main thread between steps |
| A hidden mesh might not load (*inferred*: the loop has no `visible` test) | AC17 checks it. Fallback: chunks stay `visible` but off the camera's layer until flipped |
| Remesh churn from mask changes on web | Hysteresis, one set, cache; `lodLevels = 1` is the escape: no transitions at all |
| T3's collider rim leaves an invisible lip of up to one collision cell in a cave mouth | Inherited (T3 R22, R24). The ring keeps the visuals right; AC35 measures a body rolling over the lip and `entrance_narrow` warns. Accepted, not worked around |
| A hole-edge step of `overlapDepth` shows at grazing angles | Default 0.02 voxel, measured by AC35. Colour match hides most of it. Users can raise `overlapDepth` if morph pokes through |
| Terrain layers with hex or stochastic tiling look different in the ring | Same textures and mean colour, different pattern in the ring. Accepted (Medium and Low terrain draw them plain anyway); panel notes it at High |
| Terrain hole map gives a stair-stepped edge at texel scale (T3 D7) | The ring runs under the edge, so the steps are only a boundary between two surfaces that match. Accepted |
| Very steep terrain makes "flush" fail on cliffs | `c` slope factor handles smooth slopes; a texel that misclassifies is simply cut, and the voxel surface stands in. Worst case costs triangles, not correctness |
| Volumes over budget on web at the default size | Dialog warns at creation; export error for dimensions; runtime refusal for memory. No silent quality loss |
| Noise is not a true SDF, so band collapse is weaker and raycast steps are unsafe | Generators clamp to the band and normalize by a per-graph `graphScale`; `raycast` sphere-traces at most 1 voxel per step and verifies crossing by bisection, so an inexact field is slower, not wrong |
| A scene with many small volumes | Each costs a table, an array and chunks. Sharing arrays is not designed; a project over 8 volumes in a scene gets a panel note. Default: fine |
| Two scenes with one asset, or two instances | One asset, two editors of the same data: edits appear in both in the editor (asset semantics, same as a material). At runtime each is its own copy |
| Android and iOS at default quality untested on devices | Budgets stated in counts; `checklists/T6.md` lists the device checks; recorded as unverified in the report |
| Floating debris after a dig | Out of scope. `connectivity` reports it and scripts can respond |

**Next person must know:** the reference content of AC40 is generated by tests, not checked in. The v1 fixture of AC47 is generated once by Milestone 1 and is then never regenerated: a format change gets a new fixture and a migration.

## Deviations

Recorded as the implementation found them. T3-dependent items name what to swap when T3 lands.

- **R34 and R37, noise.** Built on T2 at `a43bedd29`. The `overhangs` preset is a Noise Graph held as a string in `VoxelGeneration.cpp` rather than a `.noisegraph` file, since T2's preset list is T2's; `caves` and `cave_entrance` are ops declared in code, as R37 allows. Noise hashes are not pinned while T2's kernel table may change: the tests hold R5's and R34's properties instead.
- **R50, `TerrainDetailBuilder`.** T3's builder (`engine/core/texture/`) took a layer-list form beside its terrain one, up to 16 layers (`DETAIL_MAX_LAYERS`) where a terrain holds 12, and plan options: no surface slices (Low), and a colour slice per slot rather than per whole blend map. An export cooks a volume's slices with the terrains' (`gatherTerrainSliceInputs`), keyed alike, so a linked slot shares the terrain's.
- **R42 and R51, the height formula.** `voxel_fs.glsl` applies the formula (`weight * pow(height, 6)`) in `VOXEL_ACCUMULATE` rather than including T3's `terrain_height_blend.glsl`; Low reads height from colour alpha.
- **R49, G-buffer tier.** The G-buffer variant of a volume draws Medium whatever its tier: `Vox` is one bit, and T3's `Tqm` and `Tql` are the terrain's.
- **R51, High's reads.** 28 worst case, under the budget of 32: colour 12 near and 4 far, normal and surface 6 each. No far normal.
- **R36.** `MeshBvh` has no nearest-triangle query, so `voxelizeMesh` visits the samples around each triangle's box directly. Same distances and sign; it runs as a job in the editor.
- **R40.** Cuts use `terraincore::rasterizeRegion` per run of texels, T1's implementation of `cut_holes` and `fill_holes`, not the R8 mask form: a run is exact on the texel-centre rule, and the mask form resamples. `holeMapHolds` is T3's, at `c9c369693`; it was added here in T3's named place, and `canRemoveMap` honours it.
- **R74 header, base hash.** In a delta it is the trailing FNV-1a of the asset it was made over (R79's `base_changed`): the decoded content hash would have to resolve every base block on each save. Non-delta files store 0.
- **AC16 option.** `ChangeSetOptions::dirtyPadding` is a plain option, not compiled for tests only: the core tests link the same static engine as a game, so there is no test-only build to compile it in. Engine code never sets it.
- **R17, when a view is taken.** Each mesh job's read view is taken with its request, in the edit call, not when the job starts: `VoxelChangeSet.AtMostTwoInFlight` cracked otherwise, since a job started later read a newer version than its set.
- **Shader bits after T5's and T3's.** GBUFFER `Vox` is bit 26, after upstream's `Occ`, `Uv2`, `Ifd`, T5's `Cin`, `Wnd`, `Dth` (19-21) and T3's `Thl`, `Tqm`, `Tql`, `Txl` (22-25); the voxel Lit defines are bits 32-35, after T3's. `ShaderKey.VoxelBitIsPinned` pins them; shader cache v57.
- **R1, the component fields.** `overlapDepth` and `cutTolerance` default to -1, meaning 0.01 and 0.25 of the asset's voxel size: the component cannot know the voxel size, which belongs to the asset. The overlap is 0.01, not 0.02, so a hole's rim stays under 2 cm (below).
- **R44, the rim (owner's decision).** No step up over 2 cm where terrain meets voxel ground, along any path into a cave mouth, drawn or colliding. A cut starts where the volume leaves the terrain by more than `cutTolerance`, grows through every texel where the voxel ground lies lower than the seed by more than 0.005 voxel, then by the terrain collider's rim (one collision cell and a texel, T3 R22), so its edge lies on seeded ground: the step there is the overlap plus that slack, 1.5 cm at 1 m voxels. The mesher keeps the volume drawn over the grown cut. A dent under flush terrain that never departs by the tolerance cuts nothing. `VoxelTerrain.RimHasNoStep` samples drawn and colliding ground along radial paths, 5 mm apart.
- **R42 and R62, a slot's colour.** A slot with PBR off is drawn as a Terrain Layer with PBR off is (`terrain_fs.glsl`): its colour map raw, untinted, and, when it follows a terrain layer, the terrain material's roughness and metallic. Otherwise the ground around a cave mouth was darker, then brighter, than the terrain beside it. A tint is a PBR slot's, as the panel shows it, so `edit_voxels`' material `color` turns PBR on, as `roughness` and `metallic` do. The tile break fades in by the distance to the camera, as the terrain's does (the clip w it used is 1 under an orthographic camera). `VoxelTerrainGpu.SeamColourStep` and `.SeamColourStepLow` hold the step across the hole's edge within 3 % of the terrain's own over the same pixels. `.SeamColourStepLow` sets the terrain to Low too: Low reads near only, as T3's Low terrain does (T3 R1).
- **AC50, `VoxelCook.BakedLoadsWithoutSources`.** The editor test cooks and loads a baked file with no source and counts no evaluation. Its `run_build` and `smoke-test` half is left for the end of the programme (owner's rule: no full suites or smoke tests in streams).
- **R33 and R76, bases no description rebuilds.** A base made from a GameObject's mesh, Cave Entrance (a Terrain plus a seeded chain), a blend, or a Terrain the volume is not attached to cannot be rebuilt from the file's description. Generating one bakes it: every block goes in the file over an empty base, and the generator's `kind` is `baked` with the source kind in `other_kind`. Presets of ops and graphs record their size scale in `graph_scale` and are rebuilt by `editor::resolveVoxelBase`, which the editor's `Project` registers with `VoxelAssets`.
- **R78, a volume on an attached Terrain.** The export takes the Terrain's ground while the scenes are loaded (`cook::voxelTerrainBases`, its maps read from their files when a scene never drew them) and cooks the volume over it. A volume attached in two places stops the export, naming both: one file has one ground.
- **AC35, the collider's holes.** After T1's checkpoint reached integration, T1's `JoltTerrainRefresh` writes heights through `JoltShapeBuilder::sampleHeightField` alone, and T3's no-collision rule lived in the build only. The rule moved into `sampleHeightField` (`terraincore::noCollisionSample`, one sample read from the map, the same result as `openSquares` and `noCollisionSamples`), so a hole an edit cuts at runtime reaches the collider. `CollisionHoleAfterCollider` sinks the seed 0.05 m (`overlapDepth`) so a ray tells the seed's collider from the terrain's.
