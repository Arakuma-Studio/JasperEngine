# Visibility buffer, triangle filtering and meshlets on JasperEngine: feasibility

Date: 2026-11-04. Read-only study. Nothing was built or run from the clones, or from the engine.

**Every percentage or millisecond figure attributed to JasperEngine in this report is an estimate, not a
measurement.** No device was touched. The repository's own docs say the mobile and web reference
devices have not been measured yet (`docs/rendering/shadow-budgets.md:49`) and that the emulator
cannot show tiler bandwidth (`docs/testing.md:314`).

## Labels and path shorthand

- **[V]** verified: I read it at the cited file:line (or grepped for it, and say so).
- **[V-ext]** the external source says this (URL given). I confirmed the source says it, not that it is true.
- **[I]** inferred: reasoned from code I read. Not tested.
- **[A]** assumed: from memory of specs or hardware behaviour, or a back-of-envelope estimate. Not checked here.

| Short | Path |
|---|---|
| `F/` | `/tmp/forge-meshopt-research/forge/The-Forge` (commit cd50468, 2026-08-27) |
| `VB1/` | `F/Examples_3/Visibility_Buffer/src` |
| `VB2/` | `F/Examples_3/Visibility_Buffer2/src` |
| `MO/` | `/tmp/forge-meshopt-research/meshopt/meshoptimizer` (commit f0a684f, v1.3) |
| `JE/` | `/var/lib/orquester/workspaces/jaspersito/forge-meshopt` |
| `sokol` | `JE/engine/libs/sokol/sokol_gfx.h` |
| `RS` | `JE/engine/core/subsystem/RenderSystem.cpp` |

---

## 1. Verdicts

| Design | Verdict | Honest benefit estimate [A unless noted] |
|---|---|---|
| **F** LOD + index/vertex optimisation | **Do first.** It is not "nothing": LOD ships disabled and LOD 0 is never optimised [V] | Largest gain per unit of work. Depends on content; LOD pays most on scenes with distant dense meshes |
| **A** CPU cluster culling | **Build a reduced form after an oracle says yes:** frustum + cone, static IB ranges, big static meshes only. Refuse per-triangle and small-triangle tests on CPU | ~0% on scenes of small props. 10-40% fewer submitted triangles on large meshes. Maybe 2-15% geometry GPU time. Can go negative if draw fragmentation is not capped |
| **B** Vertex pulling + instanced meshlets | **Refuse as a rendering path** | Negative on GPU time: more vertex invocations and fetches. Only saves draw calls within one material, which instancing already covers for repeated meshes |
| **C** Software occlusion culling | **Conditional, gated on an oracle.** JE has none today [V]. meshoptimizer's rasterizer does not help [V]. Simplified occluders are not conservative [I] | Urban/indoor: maybe 20-60% fewer draws. Open outdoor: ~0 with its CPU cost still paid. No pixel benefit on HSR/early-Z hardware beyond vertex and binning load |
| **D** Fragment-only visibility buffer on GLES3/WebGL2 | **Refuse.** No MSAA option on GLES3/WebGL2 [V/I], cannot use the material model [I], adds store+load bandwidth with no subpass path [V/I] | Expected worse than forward on tilers except in extreme density; one third-party test on Apple GPUs shows both outcomes |
| **E** CPU-selected cluster LOD | **Defer.** Feasible with no compute; no current content need shown. Cook-time only, static only | Large only for dense static meshes spanning near to far. Zero to negative elsewhere. Highest cost of the viable designs |

**Headline answer.** Of The Forge's headline, JasperEngine can have the *culling math* (on CPU, at
cluster rather than triangle granularity) and the *data-preparation tools* from meshoptimizer. It
cannot have The Forge's filter pass, its one-draw-call submission or its bindless shade, and VB2 is
excluded outright. The CPU equivalent of triangle filtering is cluster culling over a static index
buffer, and most of what it would save on mobile is vertex and binning work, because tile-based GPUs
already avoid much of the pixel work.

**Recommended order**

1. **Measure:** benchmark harness, device access, two oracles (section 9). No renderer change.
2. **F:** LOD on by default for new projects, optimise LOD 0, per-submesh bounds, job-based visibility.
3. **A-reduced**, if the cluster oracle passes.
4. **C**, if the occlusion oracle passes. The oracles may swap 3 and 4 (section 10).
5. **E**, only when a real project needs it.

**Refuse:** B and D as described above, plus the list in section 10.

---

## 2. What The Forge requires, line by line

### 2.1 Visibility_Buffer (TVB 1.0)

**Triangle filter pass** (`VB1/Shaders/FSL/TriangleFiltering.comp.fsl`, shared math in
`F/Common_3/Renderer/VisibilityBuffer/Shaders/FSL/TriangleFiltering.h.fsl`, abbreviated `TF.h`)

| Required feature | Evidence [V] | JasperEngine constraint |
|---|---|---|
| Compute shader, 256 threads per group | `.comp:61`; `VisibilityBufferShaderDefs.h.fsl:56` | Compute forbidden as a requirement |
| Groupshared memory and atomics | `TF.h:61-63`; `.comp:69,130,142` | Compute only |
| Group memory barriers | `.comp:74,134,146` | Compute only |
| SSBO read: indices and positions | `.comp:96-98`; `TF.h:72`; `PersistentSet.h:29,32` | SSBO forbidden as a requirement |
| RW SSBO: per-vertex material map | `.comp:101-103`; `.srt.h:34` | Forbidden |
| RW byte buffer: filtered index output, per view | `.comp:156-158`; `.srt.h:35` | Forbidden. Also needs one buffer as both UAV and index buffer; JE creates a buffer as exactly one type (`JE/engine/renders/sokol/SokolBuffer.cpp:77-83`) |
| Indirect args written by GPU | `.comp:142`; `.srt.h:32-33` | sokol has no indirect draw (2.3) |
| Cull math: backface determinant, frustum, small primitive | `TF.h:78-194` | **Portable.** Pure arithmetic; this is the only part that can move to CPU |

**C++ side** (`F/Common_3/Renderer/VisibilityBuffer/VisibilityBuffer.cpp`) [V]

- `updateVBMeshFilterGroups` (`:180-218`) only chops meshes into 256-triangle batches. It does no culling.
- One dispatch per frame over all batches (`:298`).
- Output buffers are sized to the whole scene's index count (`:531,:548`).
- The release notes say "We removed CPU cluster culling... Now triangle filtering only takes one dispatch" (`F/README.md:406`). They give simplification as the outcome; they do not claim CPU cluster culling was useless, and with a per-triangle GPU filter it is partly redundant [I]. It is a caution, not proof, for design A.
- Async compute defaults off on Android, on elsewhere (`VB1/Visibility_Buffer.cpp:189-193`).

**ID pass**

| Required feature | Evidence [V] | Constraint |
|---|---|---|
| `SV_VertexID` + SSBO vertex pull | `VisibilityBufferPass.vert.fsl:30,35` | Fetch via SSBO forbidden. `gl_VertexIndex` is fine, texture form possible |
| `SV_PrimitiveID` in fragment | `VisibilityBufferPass.frag.fsl:30,34`; required by `VB1/GPUCfg/gpu.cfg:8` | Absent in WebGL2/ES3 [A: ESSL 3.00 spec]; meshoptimizer warns it may be unsupported on mobile (`MO/README.md`, "Visibility buffers", l.830) |
| Geometry-shader passthrough as primitive-ID fallback | `gpu.cfg:58-59` | No geometry shaders on our backends [A] |
| Packing a 32-bit ID into RGBA8 | `.frag:34`, shade `:72` | **Portable** |
| Indirect single draw per geometry set | `VB1/Visibility_Buffer.cpp:3106,3143` | Forbidden |
| Alpha test via bindless texture | `VisibilityBufferPassAlpha.frag.fsl:37-38,41` | Bindless forbidden |

**Shade pass** (`VisibilityBufferShade.frag.fsl`)

| Required feature | Evidence [V] | Constraint |
|---|---|---|
| Fullscreen fragment pass reading the ID target | `:67` | **Portable** |
| Per-sample MSAA read, `SV_SampleIndex`, `Tex2DMS` | `:53,:61`; `PersistentSet.h:41` | Not available through sokol on GLES3/WebGL2 (2.3) |
| Read filtered index buffer from SSBO | `:95-97` | SSBO, and the buffer it reads is the compute output |
| Read positions/normals/UVs from SSBO | `:100-102,:212` | Texture form possible |
| Per-vertex material lookup | `:175` | Texture form possible |
| Analytic barycentrics and gradients | `:142` | **Portable** math |
| Alternative gradient method (ray differentials) | `:26-28` comment, `USE_RAY_DIFFERENTIALS` branch | **Portable** math, more per-pixel cost [I] |
| Non-uniform bindless texture arrays | `:193-196`; `PersistentSet.h:66-68`; `VisibilityBufferShaderDefs.h.fsl:35` | Bindless forbidden. The core of why one draw shades everything |
| `SampleGrad` | `:194-196` | `textureGrad` exists in ES3 [A]; mobile cost unmeasured |
| Light clusters built by compute, read in shade | `LightClusters.comp.fsl:34,97,100`; shade `:298,:303` | Compute/SSBO. CPU + data texture is the equivalent; JE uses per-object lights instead (`JE/engine/core/Engine.h:17`, `RS:1851`) |

**GPU gate.** `VB1/GPUCfg/gpu.cfg:7` requires `MaxBoundTextures > 3075`, and `:8` requires primitive ID. [V]

### 2.2 Visibility_Buffer2 (TVB 2.0): excluded entirely

| Required feature | Evidence [V] |
|---|---|
| **64-bit atomics, a hard GPU gate** | `VB2/GPUCfg/gpu.cfg:12` |
| Bindless + primitive-ID gate | `gpu.cfg:13-14` |
| Binning compute with atomic bin counters (128 px bins, 4096 triangles per bin) | `TriangleFiltering.comp.fsl:46,252`; `TriangleBinning.h.fsl:31,44` |
| Software raster of small triangles in compute with `AtomicMaxU64` | `TriangleFiltering.comp.fsl:217-218` |
| Bin rasterizer in compute with `AtomicMaxU64` | `BinRasterizer.comp.fsl:61,226,247,268` |
| Bindless texture sampling inside compute for alpha test | `TriangleFiltering.comp.fsl:207-208`; `BinRasterizer.comp.fsl:236-237` |
| 64-bit storage buffer as depth + ID, read in fragment | `VB2/Visibility_Buffer.cpp:2003-2013`; `VisibilityBufferShade.frag.fsl:61`; `VisibilityBufferBlitDepth.frag.fsl:48` |
| Platforms: "limited number" (D3D12, consoles, macOS/iOS), and on Android only S22 | `F/README.md:357,321` |

Every row but the bin layout hits a forbidden feature, and the 64-bit atomics have no fragment
equivalent. There is nothing to port except the idea of bins. No fallback could produce equivalent
results cheaply [I].

### 2.3 What sokol-gfx actually exposes in JE's vendored copy

| Backend | Compute/SSBO | `base_vertex` | `base_instance` | MSAA texture read | Evidence [V] |
|---|---|---|---|---|---|
| GL core | only GL >= 4.3 | GL >= 3.2 | GL >= 4.2 | yes, except Apple | `sokol:10165,10168-10173` |
| GLES3 | only ES >= 3.1 | ES >= 3.2 | no | **no** | `sokol:10292-10293,10300-10301` |
| WebGL2 | **no** | **no** | **no** | **no** | `sokol:835-840`, `:238` |
| D3D11 | yes | yes | yes | yes | `sokol:13336-13339` |
| Metal, Vulkan | yes on Metal per the doc comment; Vulkan not read individually | "generally true on non-GL" | same | not read individually | `sokol:826-831,232-233` |

More sokol facts [V]:

- **No indirect or multi-draw.** The draw API is `sg_draw`, `sg_draw_ex`, `sg_dispatch` (`sokol:5162-5164`). A grep for `multi_draw|MultiDraw|DrawIndirect` finds nothing relevant.
- **No occlusion queries, framebuffer fetch, pixel local storage, input attachments or memoryless targets.** Grep finds none of them. Store actions exist, including DONTCARE (`sokol:2854-2859`, `:11860-11878`).
- **Provoking vertex is not exposed.** No `glProvokingVertex` or `provoking` in `sokol_gfx.h` (grep).
- **Buffer updates:** `sg_update_buffer` once per buffer per frame (`:4864`); `sg_append_buffer` for many (`:4866`), not mixed with update (`:4865,:4868`). On GL these are `glBufferSubData` over a ring of slots (`:12393-12406`).
- **Image updates are whole-mip only and once per frame.** `sg_image_data` holds only mip ranges (`:3380-3382`), the size must match the whole surface (`:4577,:22776`), the GL update writes at offset 0,0 (`:12430`, `:10911-10931`), and one update per frame is allowed (`:4870`). **So a shared data-texture pool cannot be updated partially without patching sokol.**
- **R32UI is sampleable and renderable on GL-family** (`:9962`), **but clears always use `glClearBufferfv`** (`:11803`), which is not valid for integer attachments on ES3/WebGL2 [A: spec]. So R32UI needs a sokol patch or a manual clear draw; RGBA8 packing avoids it.
- **Portable MRT limit is 4** (`:2050`). JE's G-buffer already uses 3 (`JE/engine/shaderlib/gbuffer.frag:12-14`).

**Vertex data paths in JE today** [V]:

- `gl_VertexIndex` cross-compiles in non-indexed form: `JE/engine/shaderlib/fullscreen.vert:8`, `effects/effect_scatter.vert:23`. An indexed use is not exercised in engine shaders [I from grep].
- `texelFetch` in the vertex stage ships for skinning (`includes/skinning.glsl:25-28`) and VAT (`includes/vat.glsl`).
- Skinning has a real fast-path/fallback precedent: `bindSkinningResource` uses a storage buffer when the shader has one, else a texture (`RS:128-145`), and GLSL builds of the mesh, depth and G-buffer shaders force the texture (`JE/engine/core/shader/ShaderVariantCompiler.cpp:1112-1115`).
- Five shader targets: glsl410, glsl300es, hlsl5, msl21, spirv10 (`ShaderBuildTypes.h:59-64`). **Unresolved discrepancy:** docs say "GL 3.3 core", the desktop shader target is GLSL 410, and sokol_app defaults to GL 4.3 / 4.1 on Apple (`sokol_app.h:3528-3535`). I did not find an override. Not decisive here, but worth someone checking.

**WebGL2 extensions not wired into sokol:**

- `WEBGL_multi_draw`: Chrome/Edge 86+, Safari 15+, **not Firefox** [V-ext: https://web-platform-dx.github.io/web-features-explorer/features/webgl-multi-draw]. A plain draw loop gives the same image, so it would be an allowed fast path, but it needs a sokol patch and an entry in `PATCHES.md` [I].
- `WEBGL_provoking_vertex`: exists, and the spec advises the first-vertex convention where available because it can be more efficient [V-ext: https://registry.khronos.org/webgl/extensions/WEBGL_provoking_vertex/].

---

## 3. What JasperEngine does today

| Fact | Evidence [V] |
|---|---|
| Forward renderer; opaque sorted nearest first | `RS:16082`, comparator `RenderSystem.h:638-645` |
| Depth prepass optional, default off | `SceneSettings.h:358`; `RS:16033` |
| Frustum test per mesh inside each draw function, so depth and colour passes each repeat it | `RS:8611,9003,9841`; `isBatchVisible` `RS:13895-13914` |
| **No per-submesh bounds.** AABBs exist at mesh level only; submesh-level selection exists only for tilemap chunks | `MeshComponent.h:461-469`, `Submesh` from `:188`; `RS:8689,8720` |
| **No job parallelism in visibility.** `parallelFor` in render code appears only in particle packing | grep; `RenderSystemParticles.cpp:759` |
| Instance culling: serial loop with a sphere test, LOD pick and sort, rebuilt when dirty so "a still frame uploads nothing" | `RS:8235-8238`, `8295-8380` |
| **No occlusion culling.** Grep for occlusion/occluder/Hi-Z/PVS finds AO textures, 2D shadow occluders, animation layer occlusion (`ActionSystemLayers.cpp:545`), and HZB only in analysis docs listing what not to copy. I checked the non-obvious hits; none is visibility culling | `docs/openengine-analysis/00-SUMMARY.md:102` |
| Per-mesh LOD: 4 levels at 100/50/25/12.5% from `meshopt_simplify*`, built on background jobs at load, as index ranges over the same vertex buffer | `MeshLodPool.cpp:18,126-134,160-177`; `MeshLodPool.h`; `Engine.h:94` |
| **LOD is off by default and off in all four project templates** | `SceneSettings.h:354`; `editor/resources/templates/{2d,3d,empty,ui}/Main.scene:8` |
| LOD excludes terrain and tilemaps; error is the worst across a mesh's submeshes | `RS:7218`, `8177-8190`, selection `8222-8236` |
| **The only meshoptimizer optimisation call in rendering is `optimizeVertexCache` on LOD levels 1+.** LOD 0 is never optimised, and overdraw/fetch optimisers are unused | grep of `meshopt_` over `engine/core` and `editor` |
| Each `SokolObject::draw` re-applies bindings and reserves descriptor bytes | `SokolObject.cpp:1119-1133` |
| Geometry draw funnels through one place | `RS:8415-8450` |
| meshoptimizer 1.2 is vendored and already contains clusterizer, partition, codec, spatial sort, provoking index and shadow index APIs | `engine/libs/meshoptimizer/`; header grep; `NOTICE.md:30` |
| Jobs: 4 workers on mobile; **0 workers on the default web build**, where `parallelFor` runs on the main thread; threaded web is opt-in | `docs/jobs-memory.md:14,109-111`; `engine/CMakeLists.txt:373` |
| WASM SIMD is set per file for selected kernels; neither culling nor anything in `RS` has it | `engine/CMakeLists.txt:2127-2138`; `tools/build.sh:68-69` |
| Per-file NEON/AVX2 selection has precedent | `engine/core/picking/MeshBvhSimd_{avx2,neon}.cpp` |
| Material system is large: 71 `#if` lines in `mesh.frag`, 56 in `mesh.vert`, 95 files in `shaderlib/library`, 47 derivative uses across fragment code | counted by grep |
| Vertex deformers: VAT, lattice, skinning, morph, terrain, user `SURFACE_DISPLACE` hooks | `mesh.vert:131,181,185,199`; `includes/surface_vs.glsl` |
| Measurement tooling exists: `jasperengine-editor benchmark --gpu-passes`, `get_render_stats` (draws, triangles, upload bytes, cpu_ms) | `docs/profiling.md:95`; `docs/mcp/TOOLS.md:4023` |

**Consequence for "job-system threads".** A design that relies on workers runs on the main thread in
the default web build, so its full CPU cost lands in the web frame [I]. Mobile budgets are also
shared with thermal limits [A].

---

## 4. Forge feature to CPU equivalent

| The Forge | CPU-side equivalent on JE | Viable? |
|---|---|---|
| Per-triangle filter in compute | Per-triangle loop on workers | **No.** ~1M triangles at 30-60 ns each is 30-60 ms of core time [A]. Also transforms vertices the GPU will transform again |
| Backface triangle test | Cluster cone test (`meshopt_computeMeshletBounds`, `MO/README.md` l.261-273) | **Yes.** ~8k tests per 1M triangles at 128 per cluster |
| Frustum triangle test | Cluster sphere test; `isInsideCamera` exists (`RS:13032`) | **Yes** |
| Small-primitive test (`TF.h:129-190`) | Per cluster it rarely fires; screen-size LOD does the same job better [I] | **No** as a separate test |
| Filtered index buffer rebuilt each frame | Merged ranges of a static index buffer; append-stream only when runs are too many | **Yes**, static preferred |
| Compaction + indirect draw | Loop of `sg_draw` over merged ranges without re-binding each time | **Yes**, needs a new entry point beside `SokolObject::draw` |
| One draw shading everything | Not reproducible: no bindless. Draws stay per material | **No** |
| Multi-view culling in one pass | Per-view cluster test on jobs, with shadow views a real multiplier; static shadow caching already reduces this load (`docs/specs/S10-shadows.md`) | **Yes**, smaller gain |
| GPU light clusters | Not needed; JE uses per-object lights. CPU clusters + data texture would be a separate project | Out of scope |
| VB2 software raster | Software occlusion depth (design C) is the only related CPU use | Partly, different purpose |

---

## 5. meshoptimizer: what helps and what does not

| Item | Use for JE | Evidence |
|---|---|---|
| `clusterizer.cpp`: `meshopt_buildMeshlets`, `buildMeshletsFlex`, `computeMeshletBounds` | **Useful for A and E.** `cone_weight` should be 0 unless cone culling is used, 0.25 being "reasonable"; Flex favours spatial locality | [V] `MO/src/clusterizer.cpp:1047,1238`; README l.211, l.276 |
| Mesh shading focus | NVIDIA limits 64/126 suit mesh shaders; JE has none, so limits are free to tune by draw cost [I] | [V] README l.211 |
| `meshopt_optimizeMeshlet` | Useful: restores vertex locality after clustering | [V] README l.229-232 |
| `meshletcodec.cpp` | **Not needed at runtime.** Targets 5-7 bits/triangle; decode 7-10 GB/s desktop. Possibly for package size; `indexcodec` may already suit a non-meshlet path [I] | [V] README l.461-500 |
| `spatialorder.cpp` | Cheap: spatial sort of triangles or points, useful for ordering clusters and static batches | [V] `spatialorder.cpp:218,253,307` |
| `partition.cpp` | Needed for E (grouping clusters); max partition is target + target/3 | [V] `partition.cpp:480`; README l.322-340 |
| `rasterizer.cpp` | **Does not help C.** Internal 256x256 buffer, scalar half-space raster, public API is only `meshopt_analyzeOverdraw` and `meshopt_analyzeCoverage`. No depth output or occlusion query | [V] `rasterizer.cpp:13-19,39,217,255` |
| Same analyzers | **Useful as tooling** for an asset report | [V] `meshoptimizer.h:682,696` |
| `meshopt_generateShadowIndexBuffer` | Good F item: fewer unique vertices in depth/shadow passes | [V] `engine/libs/meshoptimizer/meshoptimizer.h:135` |
| `meshopt_generateProvokingIndexBuffer` | Only needed for D; the vertex shader must pull through a `reorder` table, so **D depends on B**. On WebGL2 flat varyings can trigger "emulation slowdowns... implicit use of geometry shaders" without the extension | [V] `MO/README.md` l.830-846 |
| `demo/nanite.cpp`, `demo/clusterlod.h` | The reference for E: MIT, single header, not in JE's vendored copy. Defaults: 128-triangle clusters (min 1/3), partition 16, ratio 0.5, threshold 0.85, BVH via `clodBuildHierarchy` | [V] `clusterlod.h:702-730,175,191`; `nanite.cpp:80,148` |
| API availability | All meshoptimizer calls in `clusterlod.h` were checked against JE's 1.2 header and found; no upgrade is forced for E, but `clusterlod.h` targets 1.3 so compile-testing is required | [V] header grep; [I] for compatibility |
| Cone rejection statistic | `MO/demo/main.cpp:1346-1397` prints rejection % from one camera position; a model for the oracle | [V] |

---

## 6. Design evaluations

### A) CPU cluster culling on jobs

**Feasibility.** High. All CPU-side, so all five backends get the same result by construction, apart
from triangle order changes that can flip z-ties in goldens [I].

**Design that survives scrutiny** [I]

- Cook clusters for **static, opaque or alpha-tested, non-deformed submeshes above a triangle threshold** (say 8-16k). Reorder that submesh's index buffer by cluster with `meshopt_optimizeMeshlet` and spatial order.
- Runtime: per view, sphere + cone test on clusters, merge consecutive survivors into ranges, draw ranges with one `sg_apply_bindings`.
- If merged runs exceed a cap (say 32 per submesh), widen: draw over small gaps, because a culled gap is cheaper to draw than a split is to submit.
- **Static index buffer + ranges, not streaming.** Streamed indices are a fallback to evaluate, not the default.
- Excluded: VAT, lattice, skinning, morph, terrain, `SURFACE_DISPLACE`, and instanced meshes, because a shared index range cannot vary per instance [I]. `MeshComponent::animatedBounds` shows bounds are already special-cased for these (`MeshComponent.h:468-469`).
- Disabled cost: a null pointer check on the submesh; no tables unless cooked, satisfying the zero-cost rule [I].

**Where it plugs in** [V]: `drawSubmeshGeometry` (`RS:8415-8450`) for the draw, a new per-view job before draw for the test, and a new method beside `SokolObject::draw` because that one re-applies bindings each call (`SokolObject.cpp:1119-1133`).

**Why streaming indices is the wrong default**

- 1M triangles at 40% visible with 32-bit indices is about 4.8 MB per view per frame, 288 MB/s at 60 fps, before shadow views [A arithmetic].
- GL path is `glBufferSubData` into a ring (`sokol:12393-12406`); driver copy cost is unmeasured [I].
- WebGL implementations must validate index ranges against vertex buffers; rewriting indices each frame may force re-scans [A, to be measured].
- Only one `sg_update_buffer` per frame; multi-view needs append and a shared cap (`sokol:4864-4868`) [V].

**Cost estimate** [A]: 8k clusters per 1M triangles at ~30-80 ns per test is 0.25-0.65 ms single-core on a mobile big core, worse on a little core or scalar WASM. Index reorder and tables are done at cook or load.

**Benefit estimate and skepticism**

- **Frustum part.** Real gain only when a mesh is much larger than the view (merged level geometry, big buildings). Scenes of small props: ~0%, since per-mesh culling already handled them.
- **Cone part.** Closed meshes have about half their triangles backfacing, and cone tests catch only part of those, typically a minority of clusters on curved surfaces [A]. **Hardware culls those triangles anyway**, after vertex shading, so the saving is only vertex work.
- **On tilers.** Mali's IDVS and Deferred Vertex Shading (G720+: position shading only up front [V-ext: https://community.arm.com/arm-community-blogs/b/graphics-gaming-and-vr-blog/posts/immortalis-g925-the-fragment-prepass]) lower the cost of a rejected triangle, so the saving shrinks further [I]. Tiler memory does not shrink for backfaces, since they would not enter bins anyway [A].
- **Can make things worse:** many small draws, ordering artefacts in goldens, index reorder hurting vertex cache if not re-optimised, and extra CPU on a thermally limited SoC.
- **The Forge dropped CPU cluster culling** (`F/README.md:406`). Their context differs, but it is a reason not to assume the gain [I].

**Refuse within A:** per-triangle tests, small-triangle tests as a separate stage, and instanced per-instance cluster culling.

### B) Vertex pulling from data textures, instanced meshlets

**Feasibility.** It compiles and draws everywhere: `gl_VertexIndex` + `texelFetch` are in use today [V],
RGBA32F is sampleable [V `sokol:10013-10039` family]. Usable means something else.

**Design.** Fixed index-less template of 3 x 126 vertices, instance attribute selects a meshlet, the
vertex shader fetches: triangle micro-index, meshlet vertex indirection, then each attribute.

**Why it loses** [I unless marked]

- **No post-transform cache reuse.** Every corner runs the shader: 3 invocations per triangle versus perhaps 0.6-1 per triangle on optimised indexed meshes, so about 3-5x vertex work [A for the ratio].
- **Degenerates:** a meshlet with fewer than 126 triangles still runs the full vertex count, or needs per-count buckets of draws.
- **Fetch count per vertex:** at least 2 for indirection plus position, normal, tangent, UV: about 6 texel fetches replacing fixed-function attribute fetch.
- **On tilers it lands in the binning phase**, so it hurts in exactly the stage tilers must do up front. Fetching through textures also bypasses whatever attribute prefetch the hardware has [A].
- **Memory:** no packed vertex formats unless textures are chosen per attribute; RGBA32F is 16 B per texel against 12 B position + 4 B normal + 8 B UV as attributes [A for typical layouts].
- **No shared pool without patching sokol**, since image updates are whole-mip, once per frame (2.3) [V]. Texture-size limits force arrays or multiple textures; ES3 minimum is 2048 [A].
- **Cannot batch across materials without bindless**, so a "one draw" story fails. It only merges different meshes under one material, and instancing already handles repeated meshes [V `RS:8295-8380`].

**Verdict: refuse as a path.** Keep the idea only if D is ever revisited, since D needs it. A geometry
pool with **rebased 32-bit indices** and `sg_draw` ranges gets the draw-merging benefit with hardware
fetch; `base_vertex` is missing on WebGL2 and GLES < 3.2 (`sokol:238`), which is why rebasing is needed [I].

### C) Software occlusion culling

**Status today.** None [V]. Hardware occlusion queries are not exposed by sokol [V], and readback
latency would apply regardless [A].

**meshoptimizer's rasterizer: no** (section 5) [V]. Intel's Masked Occlusion Culling is Apache-2.0
and reports 98% of the triangles a full-resolution depth buffer culls [V-ext:
https://fileadmin.cs.lth.se/graphics/research/papers/2016/culling/], **but the repo is archived and
targets x86: AVX2, SSE4.1, SSE2, experimental AVX-512; NEON and WASM are not mentioned** [V-ext:
https://github.com/GameTechDev/MaskedOcclusionCulling]. So it cannot serve mobile or web as is. Porting
it, or writing a smaller scalar/NEON/WASM128 rasterizer, is required [I].

**Design** [I]

- Low-res depth (say 256x128 or 320x180), occluders flagged by users or chosen by size.
- Rasterize on jobs at frame start; test mesh AABBs (later clusters) before submission.
- Occluder geometry from `meshopt_simplify*`.

**The trap: simplified occluders are not conservative.** The simplifier moves surfaces in both
directions and reports an error (`MeshLodPool.cpp:164-171` uses it), so a simplified occluder can
extend past the real silhouette, and **visible objects vanish** [I]. Mitigations: push occluder depth
back by `result_error`, inflate query bounds, reject occluders with high error, or use authored
occluders. Each reduces culling. `meshopt_SimplifyPrune` also removes small parts; that is safe for
occluders. Lattices and displaced meshes cannot be occluders.

**Good properties:** identical on all backends, testable by golden and by unit test, disabled cost near zero [I].

**Benefit and skepticism** [A unless marked]

- Depends entirely on scene: urban/indoor can drop 20-60% of draws; open landscapes ~0 while still paying raster cost.
- **On tilers, pixel savings are small.** HSR or early-Z already removes hidden fragments of opaque geometry. The gain is draw-call CPU and vertex/binning load.
- **Web:** default build runs it on the main thread, with no worker help and SIMD only if the kernel gets `-msimd128` [V]. Budget 1-3 ms [A]. On web the draw-call saving might still pay, because WebGL draw calls are expensive [A]. That is what the oracle must show.
- Mobile: CPU work shares a thermal budget with the GPU, so "free" job threads are not free [A].

**Alternatives to compare:** a baked PVS or portals for indoor scenes would have near-zero runtime cost,
but it requires an editor bake tool and static scenes [I].

### D) Fragment-only visibility buffer on GLES3/WebGL2

**Can it exist?** Yes, as a prototype:

- ID into RGBA8 [V portable], depth buffer, a resolve pass per material, attributes through data textures.
- No `gl_PrimitiveID` on WebGL2: needs provoking-index data and pulling (so B) (`MO/README.md` l.830-846), or expanded non-indexed geometry.

**Why it should not ship as a primary path**

1. **The MSAA option is lost on GLES3/WebGL2.** `msaa_texture_bindings = false` (`sokol:10293`) [V]; resolving IDs is meaningless [I]. The Forge reads samples through `Tex2DMS` (`PersistentSet.h:41`) [V]. A sokol patch could reach ES 3.1 but not WebGL2, which has no multisampled texture sampling [A]. JE exposes MSAA as a scene setting, default 1 (`SceneSettings.h:267`), and FXAA (`SceneSettings.h:172`) [V]; I found no TAA [V grep]. So default projects would not change, but any project using MSAA would look different on this path, which fails "equivalent results" for it, and MSAA is cheap on tilers [A].
2. **No bindless, so no single resolve draw** [V 2.1].
   - Options: one fullscreen or scissored pass per material with depth-equal or stencil rejection. Material count N multiplies fullscreen depth tests, and tile classification normally needs compute or readback [I].
   - Texture arrays need same size and format; JE textures vary. WebGL2/ES3 guarantee only 16 fragment textures [A: spec], before shadows and probes.
3. **No on-tile path.** sokol has no subpass, PLS or framebuffer fetch [V], so the ID target must be stored and sampled.
   - At 1080p, 8.3 MB written + 8.3 MB read per frame, about **1.0 GB/s at 60 fps** for ID alone, and up to as much again if depth must be stored [A arithmetic: 1920x1080x4 B x 2 x 60].
   - Compression may reduce this; high-entropy IDs likely compress poorly [A].
   - Forward can discard depth with `DONTCARE` when nothing reads it [V `sokol:2854-2859`].
   - Patching in subpasses would cover Vulkan and Metal at most, never WebGL2 [A].
4. **Per-pixel fetch replaces free interpolation.** About 3 index + 3 x (position, normal, UV, tangent) = 15 fetches, plus object/instance data, call it ~19 per pixel, about 39M texel fetches per frame at full 1080p coverage [A arithmetic], incoherent in memory. Tilers interpolate varyings from data already in the parameter buffer [A].
5. **Most of the overdraw benefit already exists on tilers.** Forward's fragment overdraw on opaque geometry is reduced by PowerVR/Apple HSR, Mali FPK [V-ext: https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/killing-pixels---a-new-optimization-for-shading-on-arm-mali-gpus] and the newer Mali fragment pre-pass [V-ext: Immortalis post above], plus JE's front-to-back sort [V]. What remains for VB is mainly quad overshading at very high density [I], which only matters if geometry is dense, and LOD addresses that too.
6. **Material model does not fit** [V inventory in section 3, I for consequence].
   - Deformers (VAT, lattice, skinning, morph, `SURFACE_DISPLACE`) run in the vertex stage; re-evaluating for 3 vertices per pixel is impractical, and storing results needs transform feedback or compute. So they stay forward.
   - 47 derivative uses and user-authored shaders need rewriting for analytic gradients.
   - Per-object light lists need per-pixel lookup.
   - Result: **two renderers forever**, with parity goldens on five backends, against "users and agents first-class" and "equivalent results".
7. **Precision and ID budget.** RGBA8 round-trip needs highp; driver precision cannot be checked on the emulator (`docs/testing.md:314`) [V]. Without compute rewriting the index buffer, 32 bits must hold a global triangle index with a table lookup, adding a fetch [I]. Triangles crossing the near plane are a known weak spot for screen-space barycentrics [A]; The Forge offers a ray-differential alternative (`VisibilityBufferShade.frag.fsl:26-28`) [V], which costs more per pixel [I].
8. **WebGL2 flat attribute risk.** Default provoking vertex is last; emulation can cost geometry shaders on some backends (`MO/README.md` l.846) [V] and on-the-fly index rewriting in WebKit's Metal backend [V-ext: https://webkit.googlesource.com/WebKit/+/2ebeb4ab6b63c2adebf2939ad2788585ba7b7aa2]. sokol does not set the convention [V].

**MRT limits are not the obstacle:** an ID pass needs one target, and portable MRT is 4 [V]. The obstacles are items 1-6.

**Evidence against my position, stated plainly**

- The Forge reports "considerable performance gains due to reduced memory bandwidth", but **compared to G-buffer rendering, not forward** (`F/README.md:339`) [V]. JE is forward.
- The Forge runs TVB 1.0 on higher-end phones (`F/README.md:214`, screenshots at `:141`) [V], through Vulkan with bindless, which JE cannot require. I found no published frame-time comparison against forward on those phones.
- One third-party test on Apple Silicon, WebGPU, ~1080p, grass blades: **VB won by 25% at 50k instances** (4.13 vs 5.51 ms), broke even at 100k, and **forward won by 44% at 200k** (5.44 vs 7.80 ms). The author attributes VB's cost mostly to incoherent memory access and says results may invert on discrete GPUs [V-ext: https://app.cinevva.com/blog/2026-05-06-open-world-browser-part-21-visibility-buffer]. It is one developer's spike on a different API, with thin alpha-free geometry. It shows VB is not always worse on TBDR, and also that denser is not automatically better for VB.

**Verdict: refuse.** Reopen only if measurements show the condition in section 9, and even then only as a contained research spike.

### E) Cluster LOD with no compute

**Feasibility.** Yes, offline and CPU-selected [I]. `clusterlod.h` builds the DAG and hierarchy, and
`nanite.cpp:42-74` shows a BVH cut in about 30 lines. Clusters index the original vertices unless
border dilation is enabled (which mutates positions, `clusterlod.h:65`), so vertex buffers stay usable
with normal attributes and need no pulling [I].

**Data growth.** Ratio 0.5 per level is about 2x triangles in index data [A], plus cluster and hierarchy metadata.

**Runtime problem: the cut is not contiguous.** Selected clusters scatter across levels, so merged
ranges will be many. Options: per-view stream (5k clusters x 128 x 3 x 4 B = 7.7 MB per view per
frame [A arithmetic]), or many small draws. Temporal reuse helps: re-cut only when the camera moves
past a threshold, as instance views already do when nothing changed (`RS:8235-8238`) [V/I].

**Why "Nanite-like" oversells it** [I/A]

- Nanite's payoff relies on software raster of tiny triangles and a visibility buffer, neither available here. With hardware raster and forward shading, pixel-size triangles cost overshading and tiler memory, so the error threshold must stay above 1 px, probably 2-4 px. It becomes **continuous LOD for large static meshes**, not unbounded detail.
- Materials: clusters split by submesh, so draws do not collapse across materials.
- Excludes instancing as JE implements it (per-instance cuts would need per-instance draws), and all deformers.
- Per-mesh LOD exists and handles objects smaller than the view well. E wins only where a mesh spans near to far.

**Tiler risk:** dense clusters near the camera raise binning load; streamed index uploads add bandwidth [I].

**A cheaper intermediate to evaluate first** [I]: split large meshes into spatial chunks at cook time and
run per-chunk discrete LOD over static IB ranges with locked borders, reusing `meshopt_buildMeshletsFlex`,
`partitionClusters` and `SimplifyLockBorder`. No per-frame upload.

**Verdict: defer.** No target content currently demands it [A: I found no project requirement]. Effort is the largest of the viable options.

### F) Per-mesh LOD + index/vertex optimisation

**What is really missing** [V section 3, I for benefit]

| Item | Why | Tiler note |
|---|---|---|
| Turn LOD on by default in new-project templates | Off in all four. Old projects keep their stored value via `Stream.cpp:4446` [V] | LOD is the cheapest triangle reduction on every GPU [I] |
| `optimizeVertexCache` on LOD 0 at import | Currently only LOD 1+ | Mali's vertex shading differs from a classic cache; gain unproven there [A] |
| `optimizeOverdraw` | Unused | **No gain on Apple/PowerVR HSR; some on Mali/Adreno.** Ordering only, no runtime cost [A] |
| `optimizeVertexFetch` | Unused | Good for cache and bandwidth; needs care since LOD levels share the buffer [I] |
| Coarse-LOD-prefix vertex order | LOD levels index a sparse subset of the buffer; sparse ranges may be inefficient on Mali [A] | Candidate to measure |
| Shadow index buffers | Unused; can reduce depth-pass vertex work | Applies to all |
| Per-submesh bounds | Mesh level only | Very cheap culling for multi-material assets |
| Job-based per-view visibility | Serial per pass today. Compute once per view, reuse across depth and colour | Native gain; web runs inline, unchanged in result [I] |
| Asset report via `meshopt_analyze*` in MCP | Users and agents get numbers | Tooling only |

**Benefit.** Size unproven until measured. Cost is small and these are prerequisites anyway: A's quality depends on the same optimised data [I].

**Risk.** Triangle order changes can alter goldens; LOD-on changes visuals slightly and needs a threshold check. Load-time optimisation adds cost; prefer import-time or cooked [I]. Simplification currently runs at load on background jobs (`MeshLodPool.cpp:126-134`) [V], and web single-thread mode pumps it with a 4 ms budget (`docs/jobs-memory.md:111`) [V].

---

## 7. Where tile-based GPUs change the answer

| Design | Effect on tilers [I/A] |
|---|---|
| A | Saves vertex/binning work only, shrunk further by IDVS/DVS. Too many small draws can erase it |
| B | **Worse.** More vertex work in the binning phase |
| C | Pixel saving small; vertex and draw saving real if scene occludes. CPU competes for thermal budget |
| D | **Likely worse.** Extra store+load, loss of on-tile benefits, HSR overlap |
| E | Mixed. Better than unmanaged dense meshes, worse than coarser LOD if thresholds are tight |
| F | Safe. Some items are no-ops on HSR GPUs |

Arm's best-practice guide should be consulted for specifics; I could not extract its text (the PDF I
fetched was another Arm document and was unreadable), so the Mali statements marked [A] above rest on memory.

---

## 8. Effort estimates [A]

| Item | Estimate |
|---|---|
| Harness + oracles | 1-2 weeks, plus getting physical devices |
| F | 2-4 weeks with editor UI, Lua, MCP and goldens |
| A-reduced | 3-5 weeks |
| C | 4-6 weeks, more with three SIMD paths |
| Chunked LOD / E | 4-6 weeks / 8-12 weeks |
| D | Months, with permanent parity cost |

Per project rules any shipped feature also needs editor UI with undo, Lua where it is runtime behaviour, MCP tools, serialization defaults and tests (`CLAUDE.md`).

---

## 9. Measurements that decide it

**Blocker first.** Desktop GPU results cannot decide this. CI has software rasterizers and one RTX 3080 Ti
(`docs/perf/gpu-campaign.md`); the emulator cannot show tiler behaviour (`docs/testing.md:314`). **Someone
must get a physical Mali-G76-class, an Adreno 640-class and an iPhone**, the repo's own reference class plus
an Apple TBDR (`docs/rendering/shadow-budgets.md:23`). Without those, mobile conclusions stay assumptions.

### Benchmark scene

**Primary: Amazon Lumberyard Bistro (Exterior)**, about 2.8M triangles [A], converted to glTF and kept
outside the repository (licence to check before any redistribution [A: CC-BY as I recall]). Urban occlusion, big
static meshes, alpha foliage, many materials. Its interior variant can serve for the indoor case.

Three fixed camera paths, each deterministic with fixed frames:

1. **Street:** long view down a street (frustum and occlusion both active).
2. **Alley:** close, heavily occluded (best case for C).
3. **Overview:** high and far (best case for LOD, worst for C).

Scene variants to bracket design effects:

| Variant | Purpose |
|---|---|
| **Bistro as shipped** | Representative |
| **Bistro merged** into one or few meshes | Best case for A and E |
| **`tools/fixtures/shadows`** (2,000 static casters + 20 skinned; existing harness, `docs/rendering/shadow-budgets.md:28`) | "Many small objects" control where A should show ~0. Note S10 names a `bench.scene` that I did not find; the scene is built by `scripts/Shadows.lua` [V] |

Settings: 1080p; shadows on and off; MSAA off and 4x where available.

### Devices and metrics

| Target | Backend | Tools |
|---|---|---|
| Mali-G76 class, Adreno 640 class | GLES3 and Vulkan | `benchmark --gpu-passes`, Streamline, Snapdragon Profiler: tiler primitives, external read/write bytes, fragment invocations |
| iPhone | Metal | Xcode GPU counters |
| Iris Xe | WebGL2, Chrome and Firefox (no multi-draw) | `benchmark`; default **single-thread** web build and threaded one |
| GTX 1660 class | GL, D3D11, Vulkan | `benchmark --gpu-passes` |

Report: GPU ms per pass, CPU ms (including culling), draws, submitted triangles, upload bytes (`get_render_stats`), external bandwidth, and **sustained 20-minute frame time and temperature**, since one-minute numbers hide throttling on phones.

### The oracles, before writing any renderer code

- **Cluster oracle (A).** An offline tool in `tools/performance/` that loads the meshes, runs `meshopt_buildMeshlets`, replays camera paths and counts triangles rejected by frustum and cone per mesh (formula at `MO/demo/main.cpp:1367-1373`). Output: upper bound on triangles saved, and merged-run count per mesh.
- **Occlusion oracle (C).** Editor-only: at reduced resolution draw object IDs with a full-accuracy depth and count objects with no visible pixel. Upper bound on draws and triangles saved. Then re-run with a low-res simplified-occluder rasterizer on CPU to measure the conservative result and CPU time.
- **Overshading check (D).** Vendor counters for fragment invocations per covered pixel and fragment share of frame time.

### Decision thresholds (proposed, adjust as needed)

| Decision | Go if |
|---|---|
| F | Always, unless a sub-item shows no gain or a regression on the devices |
| A | Oracle: >= 20% fewer triangles on at least 2 of 3 paths in Bistro **as shipped** (not only merged); and measured: >= 10% geometry-pass GPU saving on Mali or Adreno, cull cost <= 0.5 ms on mobile, draws up by <= 25% |
| C | Oracle: >= 30% draws or triangles culled on 2 of 3 paths; and measured: net frame time gain >= 10% on the web or mobile target after raster cost, **zero false culls** in a pixel-diff against no culling |
| E | A real project has static meshes >= ~1M triangles spanning near to far, and per-mesh LOD + A leave it over budget |
| D | Reopen only if, on a tiler with F done, fragment invocations stay > 2 per covered pixel, and fragment work is > 60% of frame. Then spike one scene before any design work |

Evidence rule from `AGENTS.md`: every behaviour claim needs a red run; for A and C that means a test that fails without the culling (counts, pixel match), and a no-culling control.

---

## 10. Recommendation and refusals

**Order**

1. Harness, physical devices, oracles.
2. F, in small landings: templates and LOD-0 optimisation first, then per-submesh bounds, then job visibility.
3. A-reduced **or** C, whichever oracle wins on Bistro as shipped. My default is A first, since it has no false-cull risk and lower cost; C may have more upside on draw-heavy web and mobile scenes, so a strong occlusion oracle should reorder them.
4. The other of A and C, if it still clears its gate after the first ships, since their savings overlap.
5. Chunked LOD, then E, only on demand.

**Refuse**

- VB2 in any form: 64-bit atomics [V].
- GPU-driven filtering as a primary path: on the do-not-copy list (`docs/specs/README.md:23`) [V]. A desktop-only compute fast path also gives no gain on the budgets that come first, while doubling test surface [I].
- D as a rendering path, and any bindless-based resolve.
- B as a rendering path.
- Per-triangle CPU filtering and small-triangle stage.
- Streamed index buffers as the default path for A.
- Vendoring Masked Occlusion Culling as the only implementation: x86 only, archived [V-ext].
- Hardware occlusion queries: not in sokol, would need patches across five backends, results arrive late [V/A].
- Cluster LOD on instanced or deformed meshes.
- Using simplified meshes as occluders without an error margin [I].

**Licences.** meshoptimizer and `clusterlod.h`: MIT. The Forge: Apache-2.0, and `TriangleFiltering.comp.fsl` also carries an AMD MIT header. If any of its math is copied, keep the headers and add a `NOTICE.md` entry (`CLAUDE.md`, `NOTICE.md`). Masked Occlusion Culling: Apache-2.0.

---

## 11. What was and was not verified

- **Changed:** nothing in any repo. One file written: this report.
- **Verified by reading:** all [V] citations. Line numbers came from `grep -n`, `sed -n` or file reads at the commits above.
- **Not run:** no builds, benchmarks or shader compiles. Sokol's behaviour is from source, not from execution. No red control exists because no behaviour was changed.
- **Uncertain:**
  - All gain figures, and every mobile hardware statement marked [A].
  - The Arm best-practices guide was not read.
  - Exact Vulkan and Metal `sg_features` values were not read line by line; the table cites doc comments.
  - The GL 3.3 versus GLSL 410 discrepancy (2.3) is unresolved.
  - Bistro's triangle count and licence are from memory.
- **For the next person:** physical tiler access is the blocker for every mobile conclusion here. Start with section 9.

Sources (external):
[web-features: WEBGL_multi_draw](https://web-platform-dx.github.io/web-features-explorer/features/webgl-multi-draw),
[Khronos: WEBGL_provoking_vertex](https://registry.khronos.org/webgl/extensions/WEBGL_provoking_vertex/),
[WebKit ANGLE Metal provoking vertex commit](https://webkit.googlesource.com/WebKit/+/2ebeb4ab6b63c2adebf2939ad2788585ba7b7aa2),
[Arm: Immortalis-G925 fragment pre-pass](https://community.arm.com/arm-community-blogs/b/graphics-gaming-and-vr-blog/posts/immortalis-g925-the-fragment-prepass),
[Arm: Killing pixels (FPK)](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/killing-pixels---a-new-optimization-for-shading-on-arm-mali-gpus),
[Hasselgren et al.: Masked Software Occlusion Culling](https://fileadmin.cs.lth.se/graphics/research/papers/2016/culling/),
[GameTechDev/MaskedOcclusionCulling](https://github.com/GameTechDev/MaskedOcclusionCulling),
[Cinevva: visibility buffer spike](https://app.cinevva.com/blog/2026-05-06-open-world-browser-part-21-visibility-buffer).
