# S55: Draw submission (instancing, sorting, constants, culling)

Wave 5. Research: `docs/forge-meshopt-analysis/jasper-render.md` (§1-3, §7, §9) and
`visibility-feasibility.md` (design A, F). Works with S54, which adds LOD fade state to draws.

## 1. Goal

Cut the CPU and driver cost of drawing a frame, which is the largest frame-time cost on WebGL2 and
GLES. Same-mesh scenes should fall from thousands of draws to tens. Frame-constant uniforms go once
per pass instead of once per draw. Each pass culls once, from a visible list built off the main
thread where threads exist. Shadow passes stop drawing geometry they don't need. Every saving is
measured by a benchmark gate, so it can't silently regress.

## 2. Scope

In scope:
- Automatic GPU instancing of meshes that share mesh, material and pipeline.
- Composing with the Doriax catch-up's `draw-perf` (state sort, redundant-state tracker, cull before
  sort) and `sprite-batching` (dynamic batching of small meshes, sprites and UI): see §10.
- Splitting frame and pass constants from per-draw uniforms.
- One visible list per pass, built on jobs.
- Shadow-pass geometry: per-cascade terrain culling, shadow LOD bias, and position-only shadow
  index buffers.
- The stream-upload budget bug.
- Render stats that separate main-view and shadow work.
- A mesh benchmark scene and gate.

Out of scope:
- GPU-driven culling, visibility buffer and vertex pulling: refused (`visibility-feasibility.md`
  §10, do-not-copy list).
- Occlusion culling and CPU cluster culling: a later spec, only if this spec's benchmark oracles
  clear their gates (`visibility-feasibility.md` §9).
- LOD selection and fade: S54.

## 3. Current state

From `jasper-render.md`, with file and line citations there:
- **Draws:** one draw per submesh per pass, sorted by priority and distance with no state sort,
  and no auto-instancing. `InstancedMeshComponent` is manual.
- **Uniforms:** about 2 KB of frame-constant uniform blocks are re-sent per draw.
- **Culling:** a per-entity AABB test in each pass, on the main thread, after the opaque sort.
- **Shadows:** cascades redraw the full terrain cut. Terrain defeats the S10 static cache. Point
  faces, render-to-texture and probes draw LOD 0.
- **Upload bug:** a stream upload larger than the whole budget is refused forever
  (`SokolSystem.cpp:126-129`, retried at `RenderSystem.cpp:8565`), so large instance views draw
  stale data.
- **Stats:** `get_render_stats` counts triangles of every pass, shadow and depth included
  (`ObjectRender.cpp:128`).

## 4. Reference

- meshoptimizer `generateShadowIndexBuffer` (MIT, vendored).
- The Forge's frame and pass constant split (`Visibility_Buffer` per-frame UBO) as an idea only.

## 5. Requirements

### Functional

- **R1. Automatic GPU instancing.**
  - Within a pass, visible MeshComponent draws batch into one instanced draw when they share
    mesh buffers, submesh, material state, pipeline and LOD level.
  - **Per-instance data:** model matrix, normal matrix (or a derived form), and colour/tint where
    materials take it, plus S54's LOD fade factor and the instance's lightmap/probe index where
    applicable.
  - **Excluded:** skinned, morphed and blended (transparent) draws, and draws with per-object
    uniforms that differ (per-entity material overrides). Those stay single draws.
  - It is on by default as Project Settings > Rendering > GPU Instancing (Unity name), with a
    per-Mesh "Allow GPU Instancing" opt-out.
  - The image must be identical to unbatched rendering (golden equality test).
- **R2. Sort order: `draw-perf` owns it.**
  - The Doriax catch-up's `draw-perf` (`docs/superpowers/specs/2026-11-04-draw-perf-design.md`)
    sorts opaque queues by priority, distance band, render state and distance, after culling.
  - Instanced batches form inside that order: a batch is a run of the sorted queue.
  - Until `draw-perf` is on `origin/main`, S55 keeps today's order and batches by grouping equal
    keys within each priority. At merge, the batching runs over `draw-perf`'s order.
  - S54's fading draws sort after the non-fading opaque draws in either order.
- **R3. Constants once per pass.** This covers uniform data; the pipeline and binding tracker is
  `draw-perf`'s, and R3 goes through it once it lands.
  - **Frame block:** camera, time, fog, environment, global lighting, shadow cascades.
  - **Pass block:** a per-pass view, for shadow, depth and RTT.
  - Both are applied once per pipeline change, as sokol requires re-applying after
    `sg_apply_pipeline`, but only when the block's contents differ from the last applied for
    that slot.
  - Per-draw uniforms are what really varies.
  - A test counts uniform bytes per frame on a fixed scene and asserts at least a 60% drop.
- **R4. One visible list per pass.**
  - Culling runs once per camera and pass before sorting (the ordering is `draw-perf`'s). S55 adds
    the per-submesh bounds and the job-built parallel list.
  - It covers frustum (AABB per submesh, not only per entity), distance, layer mask and shadow
    caster flag.
  - It runs on the job system with ParallelFor where threads exist; single-threaded web runs it
    inline with the same result.
  - Shadow cascades reuse the camera list plus caster-only extras, with per-cascade frustum tests.
- **R5. Shadow geometry.**
  - **Terrain:** per-cascade node culling, cached static terrain casters in the S10 static cache.
  - **LOD:** shadow passes use the S54 shadow LOD (a project-wide Shadow LOD Bias, default one
    coarser level). The same rule applies to point-light faces, probes and render-to-texture
    cameras.
  - **Position-only index buffers** from `generateShadowIndexBuffer` for meshes whose
    attribute-split vertices make it worthwhile (more than 10% fewer unique positions). They are
    built at cook time (S53) or load time, and only when shadows are on.
- **R6. Stream-upload budget fix.**
  - An upload larger than the per-frame budget goes through alone in its own frame, and the
    budget then accounts for it.
  - Nothing is refused forever, and stale data is never drawn.
  - The red test is an instance view over the budget.
- **R7. Render stats.** `get_render_stats` and the Stats overlay add:
  - `main_triangles`, `shadow_triangles`, `draw_calls` by pass, `instanced_draws`,
    `instances`, `culled`, `visible`, `uniform_bytes` and `buffer_upload_bytes`.
  - `triangles` keeps its meaning (all passes).
  - These are output-only additions with no schema growth.

### Performance

- **R8. Benchmark scene** `tools/fixtures/mesh-bench/`, generated by a script in the repo:
  - 2,000 instances of 10 meshes on a grid, 200 unique meshes, one terrain with 4 cascades, a
    skinned crowd of 50, and an interleaved and a shared-accessor glTF.
  - It runs through `jasperengine-editor benchmark`.
  - Deterministic counters (draws, triangles, uniform bytes and VRAM) are gated exactly in
    `tools/perf/bench-thresholds.json`, and times are informational.
- **R9. Targets on the bench scene:**
  - Main-pass draw calls drop at least 10x.
  - Shadow triangles drop at least 30%.
  - CPU render-submit time drops at least 40% on the linux build (measured; record the numbers).
- **R10. Zero when off.** GPU Instancing off restores the exact previous submission.

### Editor, agent and Lua

- **R11. Editor settings.**
  - Project Settings > Rendering gets GPU Instancing and Shadow LOD Bias, with tooltips.
  - Mesh components get Allow GPU Instancing.
  - All edits are undoable.
- **R12. Lua:** `Mesh.allowGpuInstancing` and `Scene` render stats read-only (`getRenderStats()`),
  following the existing binding style.
- **R13. MCP:** the R7 outputs, and `check_render_budgets` (S53) uses `main_triangles` and
  `draw_calls`. Docs go in `docs/mcp/guide/meshes.md`, shared with S53.

## 6. Design constraints and decisions

- **Instancing uses vertex-rate instance buffers,** as `InstancedMeshComponent` does today. Every
  backend has them, so there are no SSBOs or texture-fetched transforms.
- **Instance buffers** are one streamed buffer per pass with offsets, not a buffer per batch.
- **Library shaders keep their interface:** per-draw versus per-instance is an attribute or
  uniform switch inside the shared mesh includes, so user and forked shaders that include them
  keep compiling.
- **Pipelines:** an instanced variant is a pipeline variant created through the S32 async path;
  draws whose variant is not ready fall back to unbatched draws.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 | `InstancingGoldenTest`: the bench scene and existing core goldens render identically with GPU Instancing on and off, on glcore and Vulkan. |
| AC2 | `DrawSortTest`: sort key ordering, and transparent order unchanged. |
| AC3 | `UniformBytesTest`: at least 60% fewer uniform bytes on the fixed scene. Red is the `main` count. |
| AC4 | `VisibleListTest`: job and inline results are equal, and submesh culling works. |
| AC5 | `StreamBudgetTest`: an oversized upload lands within 2 frames. Red on `main`. |
| AC6 | The bench gate meets R9, with numbers recorded in the spec's Results section. |
| AC7 | MCP `test_render_stats` covers the new fields. |
| AC8 | `tools/build.sh web` and `android` build, and the smoke test passes. |

## 8. Dependencies and conflicts

- **Doriax catch-up (another machine; ledger `docs/doriax-catchup/LEDGER.md`):**
  - `sprite-batching` dynamically batches single-submesh meshes of up to 64 vertices that use the
    built-in mesh shader, plus sprites and UI.
  - S55's GPU instancing takes the mesh draws `sprite-batching` does not. A draw is never both
    batched and instanced: dynamic batching runs first.
  - `draw-perf` adds `pipelines_applied` (SetPass calls) and redundant-state skipping.
  - `mesh-memory` reshapes `MeshComponent` storage. S55 keeps its additions to the component
    small and additive.

- **Depends on** S10, S15, S32 and S54 (LOD fade in instance data).
- **Hotspots:** `RenderSystem.cpp`, `RenderSystemShadows.cpp`, `ObjectRender.cpp`, the mesh
  shader includes and `SokolSystem.cpp`. S54 and S57 also touch these; merge carefully and keep
  both sides' intent.

## 9. Risks and defaults

- **Per-entity overrides** split batches. Default: hash the override state into the key, so they
  batch with identical overrides.
- **Per-instance probes and lightmaps:** when per-instance data can't express them, the draw stays
  unbatched.

## Decisions

- **Batching is decided by content, not predicted by a key.** In a pass's opaque queue
  (`SystemRender::beginDrawBatch` / `endDrawBatch`), `SokolDrawBatch` holds each draw from
  `beginDraw` to `draw` (its pipeline, a copy of its bindings and of every uniform block it
  applied). A held draw that has the same pipeline, bindings, elements and every uniform byte as
  the open batch's first draw, except its object block (`u_vs_pbrParams`, `u_vs_depthParams`),
  joins it; anything else closes the batch. A batch of one is drawn exactly as it would have been.
  Per-entity lights, reflection probes, material overrides and LOD levels therefore split batches
  by themselves (§9's default), and nothing the key forgets can merge two different draws.
- **Bit-identical instances.** An instance carries the draw's own matrices: the model's first three
  rows (a model matrix whose last row is not 0 0 0 1 draws alone), the normal matrix's first three
  columns, the full model-view-projection (10 vectors; depth: model rows, MVP and one extra vector,
  8). The instanced shader rebuilds the same `mat4`s, so batched and unbatched draws compute the
  same expressions on the same values (`InstancingGoldenGpu` compares the images pixel for pixel).
  The colour/tint is not per instance: draws with different material bytes never batch.
- **The shader switch** is `includes/object_params.glsl` / `object_depth_params.glsl`: under
  `HAS_AUTO_INSTANCING` ('Aui', MESH bit 41, DEPTH bit 18) `pbrParams` / `depthParams` are a macro over
  the instance attributes, else the uniform block as before. `mesh.vert`, `depth.vert`,
  `surface_vs.glsl` and `surface_depth_vs.glsl` include them, so the built-in Lit and every library
  shader batch without change. A fork keeps its own block: `linkBatchVariant` sees the block still in
  the variant's reflection and the submesh draws alone. The variant's uniform slots are mapped to
  the source's by name, so slot numbering may differ.
- **Batch variants are pipeline variants on their own `ObjectRender`s** (`SubmeshBatchVariants::render`,
  `depthRender`, held by `Submesh::batch` only while GPU Instancing is on and the submesh can batch),
  made through the S32 async path. A load does not wait for their shaders: the
  submesh draws alone and the mesh reloads when they are built (`needVariantReload`). Exports
  precompile them (`ShaderVariantCollector`).
- **One instance stream buffer a frame** (sokol `sg_append_buffer`, 256 KB growing by doubling up to
  64 MB when a frame needed more). A batch that does not fit, or whose variant pipeline is not ready,
  draws its instances one by one.
- **Constants (R3):** with GPU Instancing on, a pipeline already applied is not applied again and a
  uniform block whose bytes equal those last applied to its slot since that pipeline is skipped
  (sokol keeps them; it asks again only after `sg_apply_pipeline`). Off, every call goes to sokol as
  before (R10).
- **GPU Instancing off restores the previous submission (R10):** no batch variants are loaded (no
  memory, no pipelines), no draw is held, nothing is skipped, the queues keep today's order. The
  visible-list culling (R4) and the shadow geometry (R5) do not depend on it: they remove only work
  that draws nothing or lands under the shadow map's texel.
- **Settings:** `graphics.gpu_instancing` (Project Settings > Graphics > GPU Instancing), one
  `DrawSubmissionSettings`, undoable (`DrawSubmissionSettingsCmd`), written to `project.yaml`
  (`graphics.gpuInstancing`) only when off, into exports by the generated main
  (`Engine::setGpuInstancingEnabled`).
- **Allow GPU Instancing** is per Mesh renderer (Unity has it per material), saved only when off.
- **Shadow LOD Bias** is S56's `quality.shadow_lod_bias` (`QualityResolver::shadowLodBias()`, contract
  C4): a multiplier of the error a shadow pass accepts, which S55 applies the same way to point
  light faces (which now select levels at all), reflection probe faces and render-to-texture cameras.
- **New projects keep `quality.shadow_lod_bias` at 1.0.** The integrator's call was 0.5 in every template
  if that is one coarser level, else the value that is. Measured: doubling the allowed error is 1.04
  levels on the fixture sphere's chain (0.73 on a sphere under S54's recipe 3). S54 then landed `SHADOW_LOD_COARSER` (2), which already doubles
  it at 1.0, so 1.0 is the one coarser level and 0.5 would be about two. The templates set no key, and
  old and new projects read the same value (`NewProjectShadowLodBias`). Reversible by setting the key.
- **Fading draws stay single draws.** S54's fade variant draws both levels alone, on `Lfd`'s pipeline;
  they never join a batch and no 'Aui' + 'Lfd' program exists. They sort after the others of their
  priority (`buildVisibleQueue` groups within runs of one priority and fade state); non-fading draws
  keep their plain pipelines and early depth. `objectLodFade()` is the lane on both paths, so a batch
  that carries fades later needs no shader change.
- **Probe faces and render-to-texture cameras** keep S55 R5's Shadow LOD Bias in `computeLodView`. S54's
  notes say probe captures keep LOD 0; R5 asks otherwise, and at bias 1.0 they select by their own view.
- **Static terrain casters:** a terrain with `staticShadowCaster` draws, in each shadow slot, a cut
  of its own selected from the slot's light camera (from above the cascade's centre by its radius,
  so bigger texels take coarser nodes); it no longer depends on the camera and stays in the S10
  static layer. A dynamic terrain caster draws the camera's cut without the nodes outside the slot.
- **Shadow index buffers** are made on a submesh's first shadow draw (so only with shadows on),
  shared by every submesh over the same GPU buffers, for the built-in position-only depth program
  of a mesh that does not deform; LOD levels keep their own indices.
- **Visible lists:** `VisibleList::cull` (on `jobs::parallelFor`, inline below 128 candidates and on
  single-threaded web) tests plain meshes' bounds once per pass and the submeshes of a mesh whose
  rest bounds are where it draws (no skinning, morph targets, lattices, Vertex Animation or
  billboards). Instanced meshes, terrains, tilemaps and the editor's per-pass overrides keep
  `drawMesh`'s own test. Shadow slots look their casters up once per frame.
- **Stats (R7)** are output-only fields of `get_render_stats` (`draw_calls_by_pass` is an object: the
  `draw_calls` key already held the total), the Game view's Stats overlay, `scene:getRenderStats()`
  and the benchmark's `render` and `renderCpuMs`.
- **C3 (S54 LOD fade):** each instance carries a LOD fade (`ObjectRender::setNextDrawBatch`'s
  `lodFade`, 0 now; `objectLodFade()` in the shaders). A fading draw's dither pipeline differs from
  the plain one, so fading and non-fading draws never share a batch.

## Results

Measured on `tools/fixtures/mesh-bench` with `tools/perf/mesh_bench.py` (`jasperengine-editor benchmark --play`)
in the linux build image, on llvmpipe (the host has no GPU), with one still camera. The tree includes
S54's recipe 3 (Balanced 15 %). Counters are the game frame's. The by-category rows come from copies
of the bench script with the other categories removed, with the sun at `EVERY_FRAME`: a still scene
without the terrain keeps its shadow cache and would draw no shadows. Those copies were not committed,
and the bench was not changed to meet a target.

| R9 target | Measured | Status |
|---|---|---|
| Main-pass draw calls drop at least 10x | 1,611 to 230, 7.0x | **Not met**: this scene can't reach it |
| Shadow triangles drop at least 30% | S55's shadow work alone: 524,845 to 426,842, -18.7% (computed, see below) | **Not met** by S55 alone |
| | With mesh LOD, against LOD off: 2,437,224 to 383,434, -84.3% | **Met** with LOD |
| CPU render submit drops at least 40% (linux) | `renderCpuMs` 61.3 to 56.2 ms, -8.3%, on llvmpipe | **Not met** on llvmpipe; the submission alone was **not measured** |

Also measured: draws in all passes 5,780 to 610; uniform bytes 5,420,464 to 126,656 (-97.7%).

**Draws.** 230 = 28 grid + 200 unique + 2 terrain, and 1,611 = 1,409 + 200 + 2; both sums are exact. The
interleaved and shared-accessor copies and the skinned crowd are outside the camera's view and draw
only in shadow passes.
- 10x needs at most 161 main draws, fewer than the 202 that can't batch. Each unique mesh has its own
  elements, and merging those needs multi-draw or indirect draws, which the engineering values rule out.
- What can batch does: the grid goes from 1,409 to 28 (50x). Its draws split only by elements (mesh
  and detail level). In the whole frame: 426 element splits, 0 pipeline, 2 bindings, 0 uniforms.
  No gap was found to close.

**Shadow triangles.** Full scene at the default Shadow LOD Bias: 426,842. At bias 2.0, which undoes S54's
coarser rule: 484,397. At 0.5: 416,666.
- Per-cascade terrain culling (R5) removes 40,448 terrain triangles: 106,496 to 66,048 with the terrain
  alone, 8 to 6 draws. That was measured before S54. The terrain's triangles do not depend on the mesh
  LOD builder: 55,808 at every bias, before and after.
- Before S55 = bias 2.0 plus those 40,448 = 524,845. That gives the -18.7% (computed). The same tree
  before S55 was not built.
- By caster, at bias 2.0 / 1.0 / 0.5. The parts sum exactly to the full scene: 462,132 / 383,434 / 373,138.

| Caster | Bias 2.0 | Bias 1.0 | Bias 0.5 |
|---|---|---|---|
| Grid | 375,102 | 309,910 | 300,694 |
| Terrain | 55,808 | 55,808 | 55,808 |
| Copies | 21,672 | 10,224 | 9,144 |
| Unique | 8,726 | 6,668 | 6,668 |
| Crowd | 824 | 824 | 824 |

- S54's coarser rule (2.0 to 1.0) saves 17.0%; going further to 0.5 saves only 2.7% more.
- Mesh LOD does most of the work: the grid with LOD off casts 2,363,700, so LOD removes 86.9% of it at
  1.0. The -84.3% row adds the measured non-grid casters (73,524) to that.
- With S54's recipe 2 (2 % target error) chains stopped at one or two levels: 678,864 shadow
  triangles, and the bias had nothing coarser to pick. Recipe 3 fixed that. It was not a batching fault.
- **Default Shadow LOD Bias 1.0:** S54's budget doubles the error a level may have. On a 48 x 24
  sphere's chain that is 0.73 levels coarser on average (`InstancingGoldenGpu.ShadowLodCoarserIsOneLevel`,
  glcore and Vulkan): about one level, so 1.0 is the "one coarser level" setting.

**CPU.** Removing 5,170 draws saves 5.1 ms. Triangles, not draws, track the time on this host. The grid
at bias 1.0 takes 38.9 ms for 445,446 triangles (main and shadow); with LOD off, at a similar draw
count, it takes 156.6 ms for 3,370,472. That fits llvmpipe running vertex work inside the draw call
(inferred). The profile capture meant to split submission from driver work wrote no file, and the
cause was not found, so that split is not measured. A real GPU driver has to confirm or refute the 40%.

`tools/perf/bench-thresholds.json` "mesh-bench" holds these measurements as regression limits, not
R9's targets.

## Deviations

- **R2 (renumbered scope, Update 1):** S55 does not sort. The opaque queues keep today's order
  (priority, nearest first; `draw-perf`'s sort when it lands), and within each priority the draws of
  one batch key (pipeline, material, mesh) are brought together at the first's place
  (`VisibleList::groupBatches`), so batches are runs of the queue. AC2's `DrawSortTest` checks that
  grouping. S54's fading draws sort after the others once S54 gives the queue their fade: no fade
  exists in this branch to order by.
- **R3's pipeline skip** is this branch's own until `draw-perf`'s tracker lands; then the uniform
  cache keys on that tracker's applied pipeline instead of `SokolPipelineCache::appliedEpoch`.
- **Never instanced, for dynamic batching (`sprite-batching`):** single-submesh meshes of up to 64
  vertices drawn with the built-in shader, whatever else `sprite-batching` would refuse in them.
- **R6 "stale data is never drawn"** is held for instance views (their draws skip a frame while their
  upload waits) and terrain nodes; a skinned mesh whose bones upload waits draws the pose before.
- **Shadow index buffers are made at load time**, not cooked: S53's cook does not write them.
- **Position-only indices cover level 0 only.** A coarser level draws its own range of the levels'
  index buffer, so under S54's coarser shadow rule fewer shadow draws use them. The triangles are the
  same; only the saving in vertex shader runs is smaller.
- **R5 "Shadow LOD Bias, default one coarser level"** is S56's multiplier (default 1, the same level
  as the error threshold selects), not a count of levels: S55 had made it an integer setting under
  the same key, dropped when S56 landed (contract C4).
