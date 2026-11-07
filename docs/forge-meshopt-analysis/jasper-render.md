# JasperEngine rendering path audit: where geometry processing would pay off

Repo: `forge-meshopt` at `b4f7816ea` (main). Read-only audit. Nothing was built or run, and no editor was started.

**Labels.**
- **[V]** verified by reading the code at the cited line. It does not mean the code was run.
- **[I]** inferred from reading the code.
- **[A]** assumed, from general knowledge. All savings percentages and throughput numbers are [A]: nothing here was measured.

**Shorthand.**
- `RS` = `engine/core/subsystem/RenderSystem.cpp`
- `RSS` = `engine/core/subsystem/RenderSystemShadows.cpp`
- `MS` = `engine/core/subsystem/MeshSystem.cpp`
- `SO` = `engine/renders/sokol/SokolObject.cpp`
- `MLP` = `engine/core/pool/MeshLodPool.cpp`

---

## 0. Headline findings

1. A frame is forward-rendered, and the CPU drives every draw. Each submesh costs one draw per pass, in entity order or nearest-first order. There is no state sort and no automatic instancing.
2. Culling is a per-entity AABB test, run on the main thread in every pass. Nothing hierarchical is used. `SceneBvh` exists but only picking uses it.
3. LOD exists but is **off by default**, and levels are **simplified again on the device at every launch**. Nothing is cached to disk. The error value that drives selection is **not the position error it claims to be**. LOD has **no tests**.
4. Geometry is never optimised apart from the LOD levels. No vertex cache, overdraw or fetch optimisation runs on LOD 0, at load or at import. meshoptimizer 1.2 is already vendored, including its clusterizer.
5. Vertex data is mostly float32, and the whole glTF parse stays in RAM alongside the GPU copy.
6. No mesh benchmark scene and no draw-call or frame-time regression gate exist. The `benchmark` CLI does exist and can feed one.

---

## 1. How a frame is drawn

### Pass list

Order comes from `RenderSystem::draw` → `drawFramePasses` → `drawCameras` → `renderCameraPass` (RS:15368, 15221, 15416, 15521). **Forward rendering.** The "G-buffer" exists only to feed SSR. [V]

| # | Pass | Condition | Geometry? | Cite |
|---|---|---|---|---|
| 1 | `shadow static` (static cache layer) | S10 cache path RENDER_STATIC | casters | RSS:1755-1775 |
| 2 | `shadow`, `point shadow` atlas (one render pass, a viewport per slot) | `hasShadows`, some slot not SKIP | casters × slots | RSS:1778-1808 |
| 3 | 2D shadows, fluids, video conversions | when used | no 3D meshes | RS:15264-15273 |
| 4 | Reflection probe capture, one cubemap face per frame, then filter | probes | full `drawMesh` | RS:15278-15280, 2907 |
| 5 | HTML UI offscreen | documents | no | RS:15294 |
| 6 | Each render-to-texture camera: its shadows, then its own colour pass | RTT camera, mirror | full | RS:15433-15489 |
| 7 | G-buffer | SSR on (default off, SceneSettings.h:390) | all opaque | RS:15656-15658 |
| 8 | `depth pre-pass` into a packed-depth colour target, shared where it can be | SSAO, post chain, atmosphere or scene depth | all opaque | RS:15703-15707, 15740, 15817-15825, 15835-15849, 9378-9379 |
| 9 | SSAO and blur, atmosphere reduced-resolution targets | feature on | no | RS:15740, 15827 |
| 10 | Colour pass, inside one sokol pass: Z pre-pass (`PIP_ZPREPASS`, default off, SceneSettings.h:358) → sky → far video → opaque nearest-first → atmosphere composite → scene colour copy (splits the pass) → in-order UI, points, lines → transparent back-to-front → near video, fluids → HTML UI | always | opaque ×1 or ×2, transparent ×1 | RS:16088-16103, 16105-16116, 16141-16157, 16204-16391, 16394-16411, 16426 |
| 11 | SSR composite, user post chain, bloom/blur/halation, uber, blit | feature on | no | RS:16440-16505 |

A depth-only draw uses `drawMeshDepth`; a colour draw uses `drawMesh` (RS:8995, 8604). [V]

### Submission and sort keys

- **Opaque key:** `sortPriority`, then AABB squared distance, then submission order (RenderSystem.h:638-646, RS:16066). There is **no pipeline, material or mesh component in the key**. [V]
- **Transparent key:** priority, then far-first distance, then order (RenderSystem.h:626-634). [V]
- **The opaque sort runs over every active mesh, before frustum culling.** Entries are pushed at RS:16065-16067 and sorted at RS:16082. The test happens later, inside `drawMesh` (RS:8611). [V]
- **2D passes** keep transform order. [V]
- **Depth passes are unsorted.** `drawShadowCasters` walks the component array (RSS:1610) and `renderOpaqueDepth` walks transforms (RS:9384). [V]

### Sokol calls per draw

Per submesh per pass (`SO:1019-1133`, RS:8708-8985). [V]

- `beginDraw` makes 1-2 `sg_query_*_state` calls and one `sg_apply_pipeline` (SO:1030, 1041, 1100).
- Each `render.draw` calls `sg_apply_bindings` then `sg_draw` unconditionally (SO:1130-1131). Bindings are not filtered at this layer.
- An instanced batch makes **one draw and one `sg_apply_bindings` per non-empty LOD bucket**, up to 4 (RS:8439-8443).
- GL dedupes only an identical consecutive pipeline (sokol_gfx.h:12116-12119). With no state sort, consecutive equal pipelines depend on scene layout. [I]
- Pipelines are interned, so equal descriptions share one `sg_pipeline` (SO:43-46; default pool 8,192, Engine.h:288). Each `ObjectRender` holds up to 16 pipeline types (SokolObject.h:78). [V]

**Draw formula. [I]**
`draws ≈ Σ_submeshes [ 1 + zprepass + depthprepass + gbuffer ] + Σ_casters × (slots redrawn)`, plus up to 3 more draws per instanced batch for LOD buckets.

### Uniforms per draw

For built-in Lit, lit and shadow-receiving:

- `u_vs_params` is 192 B: model, normal and MVP (RS:8960).
- Object lights go to VS and FS, up to 320 B at capacity 4 (RenderSystem.h:134-136, RS:8835-8836).
- `u_fs_litFrame` is 32-96 B (RS:8841).
- The material block is 48 B (RS:8894).
- `u_fs_shadow_data` is **1,344 B** (RenderSystem.h:212-223; RS:8853).
- `u_fs_point_shadow_data` is **640 B** (RenderSystem.h:226-233; RS:8854).
- Legacy `u_vs_shadows`, `u_fs_shadows` and `u_fs_point_shadows` slots are reflected and cost only a call when absent (SO:1108; `includes/shadows.glsl:1-8` says only old forks declare them). [V]

**Roughly 2-3 KB per draw, of which about 2 KB is identical for every draw in the pass. [I]** `applyUniformBlock` has no redundancy filter (SO:1106-1117). [V]

On Vulkan, Metal and WebGPU every block rounds up to 256 B against a 16 MB per-frame budget on mobile and web (Engine.h:277-299; SokolSystem.cpp:510). Past the budget, **draws are skipped**. [V]

The engine's own comment gives 51 blocks for one skinned hero with 3 materials across colour and shadow cascades (Engine.h:292-294). [V, as a comment]

Depth draws send 192 B of `vs_depth_t`: model, VP and MVP (RS:9086). VP is constant across the pass. [V]

---

## 2. Culling

| Kind | State | Cite |
|---|---|---|
| Frustum, per entity | AABB against 6 planes, scalar, **main thread, once per pass** | RS:12991-13009; RS:13895-13914 |
| Frustum, per submesh | **None.** `MeshComponent` has one `worldAABB` and submeshes have no bounds (a grep of the component found no per-submesh AABB) | MeshComponent.h:461-466 |
| Frustum, per instance | Sphere test per instance, for main camera and each projective shadow slot | RS:8335-8344, 13032-13040 |
| Tilemap chunks | Yes | RS:12929 |
| Terrain nodes | Yes, only during CDLOD selection for the camera | RS:12887 |
| Occlusion | **None** (grep over core, renders and windows found no query or Hi-Z) | |
| Distance | Only `InstancedMeshComponent.cullDistance` and fade (InstancedMeshComponent.h:156-166); none on plain meshes | RS:8342 |
| Shadow casters | Per slot the same `isBatchVisible` test against the light frustum (RS:9003); `lightCasterLayers` prefilter per light (RSS:496, 1695-1696); S10 cache skip per slot (RSS:1725, 1785) | |
| Cascade dedupe | None: a caster inside two cascades draws in both [I] | RSS:1802 |
| BVH | `core/picking/SceneBvh` has binned-SAH build, refit, parallel refit and **frustum traversal** (SceneBvh.h:4, 46, 54-56, 71), and `MeshBvh` exists. **The renderer does not use them**: the only non-picking users are Scene, editor and MCP files | |
| Jobs | **No `jobs::` or `parallelFor` in RS or RSS** (grep) | |

**Per-pass CPU shape. [V]**
- Every pass walks all transforms or all meshes and does 3 ECS lookups per mesh (RS:16059-16062, 9406-9408), or 4 in shadows (RSS:1615-1623).
- `drawMesh` also runs its own loops over submeshes.
- `updateMVP` runs before visibility checks in multi-camera frames (RS:16050-16052).
- Camera movement sets `instanceViewsDirty` (RS:14547-14549). That rebuilds **all instanced batches for the main view and every shadow slot, including a `std::sort`, on the main thread** (RS:8238-8290, 8370).

**Coverage gaps. [V]**
- Render-to-texture cameras, mirrors and probes get `instanceView = -1`, `lodView = nullptr`: all instances, LOD 0 (RS:16030-16031).
- Point shadow faces get the same: "point faces draw everything" (RSS:1603-1608).

---

## 3. Instancing and batching

- **GPU instancing is manual only:** `InstancedMeshComponent`, used by foliage and particle-mesh paths. `maxInstances` defaults to 100 (InstancedMeshComponent.h:150). [V]
- **Automatic instancing and static batching: none.** Grep for autoInstance, staticBatch and GPU Instancing found nothing. The closest thing is `canMergeStaticModel` (MS:2664, MS:5226, editor `ModelLoadCmd.cpp:599`, `HierarchyWindow.cpp:2326`), a user-invoked merge inside one model, not a batch across entities. [V for existence, I for meaning]
- **Shared GPU buffers already exist.** `MeshBufferPool` gives "5,000 Models of one file" one VB/IB set (MeshBufferPool.h:5-10). **They still draw one call per submesh per entity. [I]** The hard part is done: buffer identity is known. What is missing is grouping.
- **Blockers to grouping (all [I]):**
  - Per-object light lists are per draw (RS:8829-8837).
  - Per-object reflection probe selection (RS:8780-8791).
  - Per-submesh custom uniforms and material blocks (RS:8888-8897, 8962).
  - Nearest-first ordering, which would need to become group-then-distance.
- **Instance record is 96 B:** full mat4, colour, rect (MS:7277-7283; InstancedMeshComponent.h:87-91). It is re-uploaded per view. Shadow views use only the matrix unless alpha-tested (`depth.vert:36-44`). Max 9 projective slots (Engine.h:57-66), so up to 10 STREAM view buffers (RenderSystem.h:816-827), each growing to 150% and never shrinking (RS:8555-8559). [V]
- **Double memory for culled batches:** the per-batch STREAM buffer (`maxInstances × 96 B`, RS:6224-6226) is still filled and uploaded (RS:8526-8527), while culled views draw from the view buffers. It is used only by `instanceView = -1` cameras (RS:8404). [V path, I for how often it is wasted]
- **Unity scenes:**
  - **There is no Unity scene or mesh importer in the tree.** `editor/unity/` holds `UnityYaml`, `UnityProject` and `UnityParticleImport` only. [V]
  - FBX arrives through ufbx → `.glb` (`editor/util/GlbBuilder.cpp`, `Project::importFbxModel`, Project.cpp:1057) and the game ships the glb (Exporter.cpp:2823-2835; NOTICE.md:102). [V]
  - `docs/engine-gaps/LEDGER.md` describes a manual port of a Unity game. A Unity-style scene therefore becomes many Model entities, each with child mesh entities, all same-mesh, each drawing separately. [I]
- **Permutations:** mesh `shaderProperties` is 64-bit, with bits used past 32 (`shaderPropertyBit(32)`, RS:6606). Depth and G-buffer have their own sets. **Live counts are only knowable at runtime** through `get_render_stats` (`alive.pipelines`, `interned_pipelines`). [V] I could not derive a static count. New vertex formats add to the pipeline key (S32 R3).

---

## 4. LOD

### What it is [V]

- **Off by default.** `meshLodEnabled = false`, threshold 1 px (SceneSettings.h:354-355); templates also set false (`editor/resources/templates/*/Main.scene:8`). Per-mesh `lodEnabled = true`, `lodBias = 1` (MeshComponent.h:518-519).
- **Who generates and when:** `RenderSystem::loadMesh` → `updateMeshLods(rehash=true)` (RS:7205), or a toggle (RS:14743-14744). Generation is **runtime, on every platform, at every launch**.
- **No import step and no disk cache.** `MeshLodPool` does no I/O, and no exporter code references it (MLP full read; grep).
- **Inputs:** position must be float32×3 (RS:7298), triangle list (RS:7291), u16 or u32 indices. Normal and TEXCOORD_0 guide it if float (RS:7307-7312). **Quantized positions silently get no LOD.** Terrain and tilemaps are excluded (RS:7218).
- **Levels:** targets 100, 50, 25, 12.5% of source triangles, each cut from the source (MLP:18, 157-159). Target error is FLT_MAX, so triangle count always rules (MLP:167, 170). Options are `ErrorAbsolute | Prune` (MLP:160). A level is rejected if it fails to shrink below 90% of the previous one (MLP:173). `optimizeVertexCache` runs per level (MLP:177); there is no overdraw or fetch optimisation.
- **Weld:** a 2e-6-of-extent grid guides collapses over seam copies (MLP:22, 29-76), added by `9db7a376f` to fix cracks.
- **Threading:** Background lane, Heavy gate (MLP:127-134). The pool is keyed by content hash and shared between meshes (MLP:89-103).
- **Selection:** `allowed = threshold × (perUnit × distance + constant) / lodBias`; pick the deepest level with `lodError × worldScale ≤ allowed` (RS:8222-8234). Distance is world AABB to eye (RS:8395), or per-instance sphere (RS:8358-8359). So it is a **projected-error metric, not screen-size or distance thresholds.** View height is the view rect or fixed resolution, not the dynamic-resolution scale (RS:8194-8197). [V]
- **Granularity:** one level per mesh across submeshes, with the worst error per level and clamping per submesh (RS:8177-8188, 8425).
- **Cross-fade and hysteresis: none.** The level changes in one frame, with no hysteresis on the threshold (RS:8226-8232, 8425-8432). [V] Popping follows. [I] Only instance distance fade exists.
- **Blended batches and 2D** keep list order at full detail (RS:8264, 8294).

### Surfaces [V]

- Inspector: scene section "Mesh Detail (LOD)" (InspectorWindow.cpp:12673-12682); per-mesh "Detail Levels" and "Detail Bias" (7447-7452); reflection labels "LOD", "LOD Bias" (RenderReflect.cpp:190-191).
- Lua: `Scene.meshLodEnabled` and `meshLodThreshold` (CoreClassesLua.cpp:3198-3199); `Mesh.lodEnabled` and `lodBias` (GameObjectClassesLua.cpp:1842-1843).
- MCP: `set_scene_settings` / `get_scene_settings` names `mesh_lod_enabled`, `mesh_lod_threshold` (SceneTools.cpp:816-817).
- CLI: `benchmark --mesh-lod on|off` (App.cpp:2968-2970).
- **Missing:** nothing reports a mesh's level count, level errors, chosen level, or triangles saved. No guide section (grep over `docs/mcp/guide`, not exhaustive over all of `docs/`).

### Your specific questions

| Question | Answer |
|---|---|
| Slows project loading? | Simplification does not block load: `isAllLoaded` ignores it, and `hasPendingMeshLods` is only read by the benchmark (App.cpp:4408). [V] **Main-thread costs remain:** every load hashes all index, position, normal and UV bytes per submesh, **even on a pool hit** (RS:7247-7252), and on a miss copies them vertex by vertex through `getVector3` (RS:7342-7356). [V] Cost scales with entities × bytes: 5,000 Models hash the same bytes 5,000 times, with no key from the share owner. [I] Only while the scene switch is on. |
| Deterministic between platforms? | meshoptimizer is built with `-ffp-contract=off` on non-MSVC (engine/CMakeLists.txt:1264-1269; MSVC relies on its default [A]). `MLP` itself lacks the flag, but its arithmetic has no `a*b+c` form to contract, and `unordered_map` is only probed, never iterated (MLP:38-71). [I] Levels are likely identical across platforms, but **no test proves it** for LODs. The existing guard, `PlacementWorldReview.SimplificationErrorIsAbsolute`, covers placement. It matters little today (nothing is shared) and becomes a hard requirement if levels are cooked. |
| Stored or rebuilt? | **Rebuilt at every player launch**, and held in RAM until pools clear (RS:7379-7380). |
| Cost on mobile and web? | CPU: 3 simplify passes over the full source per submesh, plus weld and copy. Throughput unmeasured; on the order of 1 M input tris/s desktop, several times slower on phones and in wasm. [A] Single-thread web (`EMSCRIPTEN_THREAD_SUPPORT` default OFF, CMakeLists:373): jobs run in the frame pump with a 4 ms budget (Engine.h:220; Engine.cpp:2228-2230). One job is a single function call, so a large mesh stalls the main thread for its whole duration. [I] Memory: about 0.875 × the index buffer extra on GPU **and** retained in RAM. Vertex buffer unchanged, since levels index source vertices (MeshLodPool.h:25-30). |
| Skinned? | **Not excluded**, and it runs on bind pose only (RS:7218, 7290-7314). Weights and joints are not attributes, so collapses can cross joints, and error ignores deformation. [I] Same for morph targets and VAT. Selection uses `worldAABB` of the posed mesh but `transform.worldScale`, not bone scale. [V] |
| Shadow LOD? | Yes. Same levels, light-relative, 2 texels error (RSS:1032-1046; RenderSystemShared.h:37). Not for point faces, and not for RTT or probes (see §2). No position-only levels, so shadow levels preserve UV and normal seams they do not need. [I] |
| Collider LOD? | None from this pool. `PlacementWorld.cpp:1340-1352` calls `meshopt_simplify` itself for placement bodies. Picking uses source geometry (no LOD references in `editor/picking`, `PickGeometry.cpp`). [V] |

### Weaknesses in this code

1. **BUG, `error` is not position error.** `MeshLodPool.h:22` says "deviation from the source, in mesh units". With attributes, meshoptimizer adds attribute quadric error to each collapse cost (simplifier.cpp:1432-1433) and reports `sqrtf(result_error) × vertex_scale` (simplifier.cpp:2622), where `result_error` is that combined cost (:1643). `vertex_error`, which is position-only, is used only for tracing (:2571). [V] Most meshes have normals and UVs, so their levels look worse than they are and switch later than the threshold promises. LOD then saves less, and shadows pay the same inflation for attributes they do not use. [I] Attribute weights are absolute while positions are normalised to the extent, so the inflation also varies with mesh size. [I]
2. **BUG, wrong weights without normals.** `ATTRIBUTE_WEIGHTS = {0.5,0.5,0.5,0.3,0.3}` is passed by count (MLP:20, 167). A UV-only mesh weighs its UVs with the normal weights (0.5, not 0.3). [V]
3. **BUG, a mesh can stay pending forever.** `submesh.lodKey` is set before the build (RS:7258). If `readLodSource` refuses an out-of-range index (RS:7337-7338), no build exists. `isMeshLodPending` then stays true (RS:8168-8172) and every frame re-enters `updateMeshLods(rehash=false)`, allocating a `std::map` of buffers and locking the pool (RS:14745-14749, 7223-7236). `hasPendingMeshLods` holds the benchmark to its 90 s timeout (App.cpp:4408-4418). `MeshLodPool::clearUnused` can cause the same state, editor only: it erases a ready entry with `use_count <= 1` (MLP:215), which is exactly a finished build no submesh has polled yet. The sole caller is editor/Project.cpp:5652. [V logic, I timing]
4. **Sparse submeshes cost the whole vertex buffer.** `meshopt_SimplifySparse` is available (meshoptimizer.h:471) but unset, so `vertexCount` is the attribute's whole count, for hash, copy, weld, simplify and vertex-cache (RS:7306, MLP:143, 177). glTF primitives sharing one POSITION accessor pay O(V) per submesh, up to 16 times (Engine.h:90). [V] Common in authored glTF. [A] FBX imports write per-primitive vertices, so they avoid it. [I]
5. **Seams between submeshes are not locked.** There is no `LockBorder` and each submesh simplifies alone (MLP:160), so cracks can open between materials at a level switch. [I] Bounded by the 1 px threshold for position, once bug 1 is fixed.
6. **Levels share all vertices.** There is no per-level fetch remap, so a 12.5% level touches a scattered subset of the full buffer. [I]
7. **Other mesh attributes are ignored:** vertex colour, UV1 and tangents are not weighted (RS:7307-7308). [V]
8. **Weld corner cases.** `grid.emplace` keeps one vertex per cell and is not transitive; "first seen" is really last matched across 27 cells (MLP:58-71). Minor. [I]
9. **No tests.** Grep over `engine/tests`, `editor/tests` and `tools/mcp/tests` for `MeshLod`, `selectMeshLod`, `lodCount`, `mesh_lod_enabled` finds only `TestSharedMeshBuffers.cpp:178`, which turns LOD **off**. [V]

---

## 5. Vertex formats

All [V] unless noted. Bytes per vertex. Streams are per `Buffer`; `MeshComponent` has `buffer`, `indices` and up to 30 `eBuffers` (MeshComponent.h:397-399, Engine.h:128).

| Kind | Layout | B/v | Stream | Index | Cite |
|---|---|---|---|---|---|
| Primitives (plane, wall, box, sphere...), OBJ | pos 3f, uv 2f, normal 3f, colour 4f | **48** | interleaved | u16 (OBJ: u16/u32 by count) | MS:4338-4341, 4449-4452, 4503-4506, 6538-6541, 6723 |
| `createGrid` | pos, uv, normal | 32 | interleaved | u16 [I] | MS:4397-4399 |
| Sprite, polygon, tilemap, spline extrude | pos, uv, normal, tangent 4f, colour 4f | **64** | interleaved, DYNAMIC for sprite and tilemap | u16 | MS:574-580, 589-593, 601-607; SplineSystem.cpp:269-273 |
| glTF | as authored, loaded verbatim. Each buffer view is its own GPU buffer | varies | usually separate [A] | u8 widened to u16 (MS:2125), u16, u32 | MS:2103-2115, 5800-5867 |
| FBX → glb, static | POSITION 12, NORMAL 12, TEXCOORD_0 8, optional TEXCOORD_1 8, optional COLOR_0 16; all float, no tangents written | **32-56** | one view per attribute | u16 or u32 | GlbBuilder.cpp:945-965, 992-996 |
| FBX → glb, skinned | above plus JOINTS_0 u16×4 (8), WEIGHTS_0 f×4 (16) | **56-80** | separate | u16 or u32 | GlbBuilder.cpp:974-976 |
| Terrain patch | pos 3f (**y always 0**), normal 3f (**always 0,1,0**) | **24** | interleaved | u16; row-order grid, not cache-optimised | MS:610-613, 3295-3323 |
| Terrain instance | node pos 2f, size, range, resolution | 20/node | 4 views × 2 buffers, STREAM | | MS:3683-3692, RS:6232-6236 |
| Particle quad | corner 2f (shared) + instance: pos 3f, colour u8×4, size half×2, rotation/frame u16×2, velocity half×4 | 8 + **32** | shared + per instance | u16 | RenderSystemParticles.cpp:406-410, 428-430 |
| Particle mesh | pos, normal, uv | 32 | interleaved | u32 | RenderSystemParticles.cpp:469-476 |
| Points | pos, colour 4f, size, rotation, rect 4f | 52 | interleaved | none | RS:11218-11222 |
| Mesh instance | mat4, colour 4f, rect 4f (VAT +32 in own buffer) | 96 | per instance | | MS:7277-7283, RS:6458-6460 |

**Particles are the prior art for quantisation**: u8, half and u16 formats already work on all backends. Shaders already handle normalised skin attributes through `normAdjust` (`skinning.glsl:38-41`, MS:5843-5867). [V]

**Position-only stream:**
- There is **no explicit one**.
- The depth program reads only `a_position`, plus `a_texcoord1` for alpha clip (`depth.vert:33-47`), and unused attributes bind to nothing (SO:492).
- **FBX and separate-view glTF get a position stream by accident. Primitives, OBJ and interleaved glTF fetch through a 48 B or larger stride.** [I]
- On non-GL backends each attribute of an interleaved buffer takes its own bind slot with an offset (SO:501-512). Colour draws can therefore bind many slots per draw (16 allowed, sokol_gfx.h:2045). [V]

**CPU retention (RAM):**
- `eBuffers` point into the tinygltf parse, kept alive by `shared_ptr` (MS:2111-2112) until `destroyModel` (MS:6916-6919) or the pool drops it.
- Generated meshes keep their `InterleavedBuffer` bytes.
- `MeshLodData.indices` stays for the mesh's life (RS:7367-7369).
- Terrain keeps its heightmap (`setReleaseDataAfterLoad(false)`, MS:3674).
- Instance `renderInstances` and `view.data` are both kept.
- **So every mesh byte exists in RAM and VRAM.** Textures have `releaseDataAfterLoad`; I found no mesh equivalent. [V by grep, not exhaustive]
- Real consumers: picking and Lua `MeshRaycast`, LOD hashing, `CpuSkin`, reload. [I] A runtime "free after upload" switch would need to respect these.

---

## 6. Diagnostics today [V]

| Surface | Gives | Cite |
|---|---|---|
| `get_render_stats` | draws, instances, triangles, passes, pipelines and bindings applied, uniform and upload bytes, alive resources, pipeline cache, CPU/GPU ms | RenderStats.h:156-186; ProfilerTools.cpp:181-190; TOOLS.md:4023 |
| `get_gpu_passes` | per pass label, targets, actions, draws, instances, triangles, pipelines, CPU/GPU ms; optional draw list (pipeline, shader, elements, instances, base) | RenderStats.h:39-85; ProfilerTools.cpp:585-607 |
| `capture_pass_output`, Frame Debugger | a pass's attachment as an image | RenderStats.h:126-153 |
| `capture_profile`, `get_cpu_profile`, `get_frame_times` | CPU scopes, frame times | TOOLS.md:43 |
| `get_vram_report`, VRAM panel | bytes by category and owner; category "Meshes (VB/IB)", owner "Mesh LODs". **Profiler builds only** | GpuMemory.h:15, 50; RS:6200, 7374 |
| `get_shadow_stats` | slots, cache path, draw calls per path | RSS:1806; TOOLS.md:1748 |
| `benchmark` CLI | JSON: avg draws, instances, triangles, cpuMs, per-pass GPU ms | App.cpp:4600-4640; docs/profiling.md:95 |
| `JASPERENGINE_FRAME_STATS` | log line per frame | SokolSystem.cpp:294-298, docs/profiling.md |

**An agent cannot read:**
- culled or visible counts for entities, submeshes or instances (S15:36 scoped out "cull reasons");
- triangles per LOD level, or level distribution;
- vertex shader invocations, ACMR or overdraw;
- mesh RAM;
- instance upload volume per view;
- anything from a release game's VRAM tracking.

GPU timers: WebGL2 only on Chrome; Firefox and Safari unavailable (docs/profiling.md, timing table). Web frame budgets need CPU proxies or Chrome.

---

## 7. Shadows, terrain, skinning, data textures

**Shadow casters [V]**
- All projective slots share one render pass with per-slot viewport and scissor (RSS:1780-1798), which is good.
- S10's cache skips slots (`ShadowCache::evaluate`, RSS:1725).
- **Terrain is `cameraDependent` (RSS:607): its layer goes dirty when the camera moves. A terrain caster therefore defeats caching in practice for a moving camera.** [I] Static-layer caching still helps meshes.

**Terrain [V]**
- CDLOD re-selects only when camera or terrain moves (RS:15093-15107).
- **Every shadow slot draws the main camera's whole node cut.** `terrainViewData(mainTerrainView())` with `instanceCount = node count` (RS:9021-9024, 9081), and only the whole-terrain AABB is tested. There is no per-cascade node culling, and near high-resolution nodes draw in far cascades. [I]
- Worked example [I, node count assumed]: 150 nodes × 2,048 tris × (colour + 4 cascades + optional pre-pass) ≈ 1.5-1.8 M tris/frame from terrain alone.
- Vertex stage: 2 heightmap samples plus a `distance` per vertex (`terrain_vs.glsl:64-89`).
- **The patch mesh is scan-order** (MS:3309-3328). A one-time `meshopt_optimizeVertexCache` would cut re-shading on every instance of every pass. [I]
- Layout waste is small: 1,378 verts × 24 B ≈ 33 KB at default resolution 32 (TerrainComponent.h:193). **Node buffers are bigger:** with `rootGridSize 2, levels 6`, about 5,460 nodes × 20 B × 8 buffers ≈ 870 KB allocated up front, used or not (RS:6232-6236; 4 views × 2). [I]
- Foliage instance chunks use the instanced path, so §2's rebuild applies.

**Skinning [V]**
- GPU only. `CpuSkin` is for baking, not rendering (CpuSkin.h:5-8).
- Bones are in a storage buffer where the shader uses one, else an RGBA32F texture 4 × bones (RS:128-148; `SKINNING_TEXTURE`, ShaderVariantCompiler.cpp:1115).
- Texture path: **16 `texelFetch` per vertex**, 4 per bone × 4 bones (`skinning.glsl:24-28, 38-41`), repeated in every pass. A 3×4 matrix would make it 12 fetches and 48 B per bone. [I]
- **Multi-node skinned models upload the same skeleton once per child mesh** (RS:15134-15153, 8456-8465). No sharing. Measured by comment: 8,192 B per hero per frame (Engine.h:302-303).
- Upload is lazy: it happens in the first pass that draws the mesh (RS:8622, 9013), so a culled mesh does not upload. Good.
- `skinnedAABB` merges per-bone boxes on the main thread when bones change (RS:14940-14947).

**Per-frame waste**
- See §1 uniforms and §2 rebuilds.
- A still camera uploads nothing for instances (RS:8236-8241). Good. [V]

**Data textures: nothing blocks them, with these limits.** [V unless noted]
- RGBA32F with `texelFetch` from the vertex stage is already shipped by bones, lattice (RS:8471-8483) and VAT.
- One update per image per frame (sokol; RS:8449-8453 states the rule for buffers). A per-view texture is therefore needed for per-view data.
- STREAM texture uploads count against Vulkan staging, 16 MB/frame on mobile and web (Engine.h:278-306). The GLES fragment stage is at its 16-unit limit in some variants (`shadow_data.glsl` header), so **vertex** stage use is the safe slot. [I]
- Declaring an unread texture "breaks the bind slots on GL" (same header): declare only what is read.

---

## 8. Spec overlap and merge hotspots

| Spec | Overlap | Must respect |
|---|---|---|
| **S10** | Shadow LOD, caster revision, cache keys (S10:111-113, 271-289) | Any caster culling or LOD change feeds `updateShadowCasters` revision (`revision.add(mesh.lodBias)`, RSS:580), or caches go stale. Slot-level culling must stay consistent with `ShadowCache` dirty rects |
| **S11** | Material fan-out; hex terrain; glTF block | Quantised normal/tangent must keep `tangent_frame.glsl`. `MS` glTF block and RS are named hotspots (S11:629-635) |
| **S13** | Dynamic resolution changes pixels per unit | LOD view uses view height, not render scale (RS:8194). Decide which |
| **S14** | **The pattern to copy**: content-addressed cooked cache (S14:157), export integration, background cook on S06 jobs, no-FMA flags per file (CMakeLists:2147-2151) | S34:36 says content size for meshes belongs to "S14 and the cook". No spec owns meshes yet |
| **S15** | Stats structs, MCP tools, budget | Add counters to `RenderStats` and `get_render_stats`; S15:36 only excludes RG "cull reasons". Tool budget `tools/mcp/budget.json` |
| **S26** | `SceneBvh` frustum traversal, `MeshBvh`, `BvhDiskCache`, `pickGeometryHash` | Vertex reorder or quantise must keep `PickGeometry` correct. The disk cache is a second cook precedent. Reusing the pick tree for rendering couples two owners; it refits only at query time (SceneBvh.h:8) |
| **S32** | Pipeline key includes vertex formats (S32:159-167) | New formats mean new pipelines, warm draws and stand-ins, and more warm-up entries |
| **S34** | meshoptimizer `when: always` (engine/notices.json:29); lib sizes (`tools/players/lib_size.py:94`) | Cooked levels could let players drop the simplifier, except `PlacementWorld.cpp:1345` still calls it at runtime |

**Do-not-copy** (README:23): GPU-driven culling. **Rules:** no compute, SSBO, bindless or indirect. A meshlet path must therefore be CPU cluster culling writing surviving indices into a STREAM index buffer per view, or per-cluster draw ranges. Per-cluster draws would worsen the top waste. Staging limits apply. meshopt's `clusterizer.cpp` and meshlet codec are vendored (MIT). [V files, I design]

**Merge hotspots:**
- `RS` (`loadMesh`, `drawMesh`, `drawMeshDepth`, `buildInstanceView`, `renderCameraPass`, update loop)
- `RSS` (`drawShadowCasters`)
- `MS` (glTF block, terrain)
- `MeshComponent.h` (Submesh)
- `MeshLodPool.*`, `MeshBufferPool.*`
- `editor/util/GlbBuilder.cpp`, `editor/Exporter.cpp`, `editor/Project.cpp`
- `editor/StreamComponents.cpp`, `editor/Stream.cpp`, `editor/Factory.cpp`, `editor/reflect/components/RenderReflect.cpp`, `editor/window/InspectorWindow.cpp`
- `editor/tests/reflect/catalog_snapshot.json`
- `engine/core/render/RenderStats.h`, `editor/mcp/tools/ProfilerTools.cpp`, `tools/mcp/budget.json`, `docs/mcp/TOOLS.md`
- `engine/CMakeLists.txt`, the Xcode project (`tools/xcode_sync_*`)
- `tools/perf/bench-thresholds.json`

---

## 9. Top wastes, ranked

Rank is my judgement for mobile and web. Savings are [A], unmeasured, and depend on the scene.

| # | Waste | Evidence | Likely saving | How to measure |
|---|---|---|---|---|
| **1** | **Draw submission: one draw per submesh per pass, no auto-instancing, no state sort, ~2 KB of frame-constant uniforms re-sent per draw** | §1, §3 | Largest CPU win on WebGL2 and GLES. Same-mesh scenes could fall from thousands of draws to tens. Deduping constant blocks alone removes ~70% of uniform bytes. Past 16 MB, draws are dropped on Vulkan and Metal | Scene of N copies of one model: `benchmark` `avgDrawCalls`, `cpuMs`; `get_render_stats` `pipelines_applied`, `bindings_applied`, `uniform_bytes`. Golden must match |
| **2** | **Geometry repeated in shadow passes without reduction: full terrain cut per cascade, terrain defeats S10 caching, LOD 0 in point faces, RTT and probes, no position-only shadow indices** | §2, §4, §7 | Triangles in shadow passes can often halve with per-slot terrain node culling and shadow-specific levels. On tile-based GPUs, binning work falls with them | `get_gpu_passes` triangles and GPU ms for `shadow`; `get_shadow_stats` last_render on a moving-camera terrain scene (`tools/fixtures/shadows` as base) |
| **3** | **No geometry optimisation; LOD off by default and rebuilt on device** | §0, §4 | Vertex-cache order commonly gives 20-40% fewer vertex shader runs on unoptimised meshes. Cooked levels remove on-device simplify entirely, including web pump stalls | Unit test with `meshopt_analyzeVertexCache` and `analyzeOverdraw` on fixtures (ACMR before and after); `benchmark --mesh-lod on|off` triangles; `get_frame_times` spikes while levels build; web first-minute frame times |
| **4** | **Memory: float32 vertices (32-80 B), whole parse retained in RAM, LOD indices in RAM and VRAM, double instance buffers** | §3, §5 | VB size roughly halved with quantisation (e.g. static 32 → ~16-20 B, skinned 56 → ~28 B). Up to 1× model size in RAM if parse release is possible. Download size if glb is also compressed | `get_vram_report` category `meshes`; wasm `HEAPU8.length` or RSS after load; `.glb` bytes in the export; golden images for precision |
| **5** | **CPU culling: O(N) per pass, main thread, after sort, 3-4 ECS lookups per mesh; instance views all rebuilt on camera move** | §1, §2 | Web single-thread benefits most. A flat visible list for the pass, built once, removes repeats. Job-system instance culling helps on threads only | `capture_profile` scope times in a 5,000-mesh scene, plus new counters for culled entities |
| 6 | Skinning: 16 fetches per vertex, 64 B per bone, skeleton re-uploaded per child mesh | §7 | Up to 25% fewer fetches and upload bytes, more with shared skeletons | Upload bytes in `get_render_stats`; GPU ms for a crowd scene |
| 7 | LOD hash per load per entity, per-frame map allocation while pending | §4 | Load hitches in large scenes with LOD on | `JASPER_SHADER_TIMING` load log (RS:14920-14932); profiler scope |
| 8 | Terrain patch scan order, 870 KB of node buffers | §7 | Cheap fix, good terrain VS saving | ACMR on patch; terrain pass GPU ms; `get_vram_report` Terrain |

### Plain bugs

| Bug | Status | Red test |
|---|---|---|
| LOD `error` includes attribute cost, documented as mesh-unit deviation (MeshLodPool.h:22; simplifier.cpp:1432, 2622) | V cause, I impact | Mesh with hard normals: assert level error ≤ measured max vertex-to-surface distance × a tolerance |
| UV-only mesh weighted with normal weights (MLP:20, 167) | V | White-box test: `ATTRIBUTE_WEIGHTS` selection by present attributes |
| Mesh stuck pending forever on a refused source (RS:7258, 7264, 8168-8172), or after editor `clearUnused` of an unpolled build (MLP:215) | V logic, I timing | Mesh with an out-of-range index and LOD on: `hasPendingMeshLods` must go false. `clearUnused` between job finish and poll: level must still arrive |
| A stream upload larger than the whole budget is refused forever: `frameUsed + size > limit` is never false (SokolSystem.cpp:126-129), with retries at RS:8565-8566. An instance view over the limit (16 MB default on mobile, ~175k × 96 B; 48 MB on desktop) never uploads and draws stale data | V logic, I reach (the staging limit applies to Vulkan: SokolSystem.cpp:164-166) | Vulkan test with `streamStagingBytes` lowered below one view's size: expect a logged error and a defined result |

---

## 10. Benchmarks and regression tests

- **Benchmark scene: none for meshes.** `build/fixture-unity` is external, not in git (S02:67, 426), and **absent from this worktree and this host** (checked `build/`, sibling workspaces, `~/je`). `tools/fixtures/` holds feature fixtures (`shadows`, `pbr-extended`, `post-processing`, `reflection-parity`...), none a many-mesh stress scene. [V]
- **Tools that exist:**
  - `jasperengine-editor benchmark` (`--project --scene --frames --warmup --mesh-lod --depth-prepass --lit-quality --gpu-passes --profile-capture --out`, App.cpp:2941-2978).
  - `tools/perf/gpu_campaign.py` (cases shadows, pbr, post, particles; docs/perf/gpu-campaign.md).
  - `tools/perf/check_bench.py` + `bench-thresholds.json`.
- **Regression gates:**
  - `bench-thresholds.json` covers jobs, diag, memory, picking, particles, nav, splines, anim, async-shaders. **None covers mesh draws, frame time or triangles.** [V]
  - `test_profiler.py:64` only asserts `draw_calls >= 2`.
  - The HTML UI has its own perf suite (docs/testing.md:75).
  - `*Bench` and MCP budgets run only on the benchmark server (docs/testing.md:93-95, 541-559).
- **Suggested gate:** a generated fixture scene in `tools/fixtures/` (N same-mesh Models, an interleaved and a shared-accessor glTF, terrain with cascades, skinned crowd), run through `benchmark`, with deterministic counters (draws, triangles, uniform bytes, VRAM) gated exactly, and times on server E only.

---

## 11. Uncertain or not checked

- No runtime numbers exist. Every saving above needs the benchmark before it is trusted.
- Exact block sets for each Lit variant were not enumerated (`SHADOW_DATA_POINT` and similar); the 2-3 KB figure is a struct-size upper-range estimate.
- Whether Android ships Vulkan or GLES by default was not traced, so the stream-budget bug's reach is unconfirmed.
- `editor/window/profiler` panels were located, not read.
- `canMergeStaticModel` behaviour was read only as far as its reject checks.
- The `jasper` MCP server failed to connect in this session. No effect here: the brief forbade editors.
