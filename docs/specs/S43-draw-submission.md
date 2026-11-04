# S43: Draw submission (instancing, sorting, constants, culling)

Wave 5. Research: `docs/forge-meshopt-analysis/jasper-render.md` (§1-3, §7, §9) and
`visibility-feasibility.md` (design A, F). Works with S42, which adds LOD fade state to draws.

## 1. Goal

Cut the CPU and driver cost of drawing a frame, which is the largest frame-time cost on WebGL2 and
GLES. Same-mesh scenes should fall from thousands of draws to tens. Frame-constant uniforms go once
per pass instead of once per draw. Each pass culls once, from a visible list built off the main
thread where threads exist. Shadow passes stop drawing geometry they don't need. Every saving is
measured by a benchmark gate, so it can't silently regress.

## 2. Scope

In scope:
- Automatic GPU instancing of meshes that share mesh, material and pipeline.
- Opaque state sort.
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
- LOD selection and fade: S42.

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
    materials take it, plus S42's LOD fade factor and the instance's lightmap/probe index where
    applicable.
  - **Excluded:** skinned, morphed and blended (transparent) draws, and draws with per-object
    uniforms that differ (per-entity material overrides). Those stay single draws.
  - It is on by default as Project Settings > Rendering > GPU Instancing (Unity name), with a
    per-Mesh "Allow GPU Instancing" opt-out.
  - The image must be identical to unbatched rendering (golden equality test).
- **R2. Opaque state sort.**
  - Opaque and alpha-test draws sort by pipeline, then material bindings, then mesh, then front to
    back within a bucket.
  - Transparent draws keep back-to-front order.
  - The sort key is a 64-bit integer, sorted once per pass with a radix or std sort.
- **R3. Constants once per pass.**
  - **Frame block:** camera, time, fog, environment, global lighting, shadow cascades.
  - **Pass block:** a per-pass view, for shadow, depth and RTT.
  - Both are applied once per pipeline change, as sokol requires re-applying after
    `sg_apply_pipeline`, but only when the block's contents differ from the last applied for
    that slot.
  - Per-draw uniforms are what really varies.
  - A test counts uniform bytes per frame on a fixed scene and asserts at least a 60% drop.
- **R4. One visible list per pass.**
  - Culling runs once per camera and pass before sorting.
  - It covers frustum (AABB per submesh, not only per entity), distance, layer mask and shadow
    caster flag.
  - It runs on the job system with ParallelFor where threads exist; single-threaded web runs it
    inline with the same result.
  - Shadow cascades reuse the camera list plus caster-only extras, with per-cascade frustum tests.
- **R5. Shadow geometry.**
  - **Terrain:** per-cascade node culling, cached static terrain casters in the S10 static cache.
  - **LOD:** shadow passes use the S42 shadow LOD (a project-wide Shadow LOD Bias, default one
    coarser level). The same rule applies to point-light faces, probes and render-to-texture
    cameras.
  - **Position-only index buffers** from `generateShadowIndexBuffer` for meshes whose
    attribute-split vertices make it worthwhile (more than 10% fewer unique positions). They are
    built at cook time (S41) or load time, and only when shadows are on.
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
- **R13. MCP:** the R7 outputs, and `check_render_budgets` (S41) uses `main_triangles` and
  `draw_calls`. Docs go in `docs/mcp/guide/meshes.md`, shared with S41.

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

- **Depends on** S10, S15, S32 and S42 (LOD fade in instance data).
- **Hotspots:** `RenderSystem.cpp`, `RenderSystemShadows.cpp`, `ObjectRender.cpp`, the mesh
  shader includes and `SokolSystem.cpp`. S42 and S45 also touch these; merge carefully and keep
  both sides' intent.

## 9. Risks and defaults

- **Per-entity overrides** split batches. Default: hash the override state into the key, so they
  batch with identical overrides.
- **Per-instance probes and lightmaps:** when per-instance data can't express them, the draw stays
  unbatched.
