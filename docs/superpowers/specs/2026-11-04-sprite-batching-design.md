# Sprite batching: consecutive sprites, small meshes and UI elements drawn as one draw, cached and culled cheaply, no per-sprite GPU buffers (design)

Date 2026-11-04. Branch `dc/sprite-batching`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`
(feature #13). Dossier: `dc-work/dossiers/sprite-batching.json`; findings: `dc-work/dossiers/EXTRA-FINDINGS.md`
("mesh-memory / sprite-batching") and `dc-work/analysis/verdicts/big-claims.md` #2 and #3.

Evidence labels: **read** (I read the file today), **inferred** (from reading code), **assumed**. Everything
here was read statically at `main` `b4f7816ea` and Doriax `3fb49fbba`; nothing was built or run.

## 1. Goals

1. **Dynamic Batching.** Consecutive draws of a colour pass that share pipeline, textures and every per-object
   input that cannot be baked are drawn as one indexed draw: Sprites, MeshPolygons, primitives and any other
   single-submesh mesh of at most 64 vertices drawn with the built-in mesh shader, and component UI elements
   (Image, Panel, Button, short Text) of at most 256 vertices. Unlit, 2D-lit (Light 2D, Shadow 2D) and lit
   (punctual, vertex lights, shadows received) variants all batch. Output is the same image as drawing them one
   by one (max channel difference 1 on the reference renderers).
2. **Every pass that draws them.** The camera depth pre-pass, the opaque depth of the scene inputs, shadow
   casters and the SSR G-buffer batch the same objects, so an opaque sprite batches in a 3D camera with SSAO
   (Doriax never batches it there).
3. **2D reordering.** In an orthographic camera looking along Z (every 2D and UI camera), a draw may join an
   earlier open batch when it overlaps nothing drawn in between: `A B A B` of two textures that do not overlap is
   two draws, not four. Images stay identical.
4. **Settled objects.** An object whose inputs did not change keeps its baked, world-placed vertices, its bounds
   and its culling verdict between frames; under a scrolling camera it is neither re-baked nor given a new MVP.
   Direct C++ writes stay visible the next frame (no "remember to set a flag" rule, unlike Doriax).
5. **No sprite buffer ceiling.** Immutable vertex and index buffers are shared by content (sprites become
   immutable): 20,000 sprites of 10 distinct shapes use ~20 GPU buffers, not 40,000. The content hash is strong
   (128 bits), and a buffer shared by two attributes keeps their strides apart.
6. **Users and agents can see and control it.** Project Settings > Graphics > Dynamic Batching (default on),
   `Engine.dynamicBatching` in Lua, Game view Stats "Batches / Saved by batching", the Frame Debugger's "why this
   draw call was not batched with the previous one", and MCP `get_render_stats` with batching counters and the
   frame's batch breaks by entity.

## 2. Non-goals

- **Texture atlasing** (Unity's Sprite Atlas): a run breaks on a texture change; packing sprites into sheets is
  the user's (docs say so). Possible later feature.
- **Batching across a pass break** (a scene colour copy, a scissor-less foreign draw, a render pass end) or
  across anything that is not a mesh or component UI element (Points, Lines, Trails, Particle Systems, Tilemaps,
  Terrain, HTML UI, Video planes, fluids): they draw as today and end every open batch.
- **Instancing as the batching mechanism.** Considered and rejected (section 5, decision 2). Explicit instancing
  (`Sprite:createInstancedMesh`, InstancedMeshComponent) stays as it is and is never auto-batched.
- **Skipping `RenderSystem::update`'s per-mesh reconciliation for settled objects** (Doriax's settle skips the
  whole loop body): that loop is where direct writes (alpha mode, lighting mode, texture stand-ins) are noticed.
  Making it cheaper is `draw-perf`'s mandate.
- **GPU resource pool sizes in Project Settings.** Sprites no longer consume per-object buffers; the pools stay
  settable from C++ (`Engine::setGpuResourceLimits`). A project setting can come with a feature that needs it.
- **`SpriteComponent::framesRect` memory** (6 KB inline per sprite): owned by `mesh-memory` with the HybridArray /
  Submesh storage change (section 13).
- **Custom and library shaders, materials with extra passes, IBL / reflection probes, mirrors, skinning, morph
  targets, lattices, Vertex Animation, mesh LODs, smooth normals, render-target textures**: their draws read
  per-object inputs the batch cannot bake; they draw one by one and are named as the break reason.

## 3. Users and stories

- **2D game developer, web build.** 3,000 coins, tiles and enemies from four sprite sheets, camera scrolling every
  frame. Today: 3,000 draws, each with 6-8 uniform blocks (~3,000 x `glUniform*` round-trips into JavaScript),
  3,000 MVPs a frame, a GPU buffer pair per sprite. After: a handful of draws, no MVP for settled sprites, the
  settled sprites' vertices copied, not recomputed.
- **Unity user.** Looks for "Dynamic Batching" in Project Settings and for "Batches" and "Saved by batching" in
  the Game view Stats; opens the Frame Debugger and reads "Not batched with the previous draw: different texture
  (baseColorTexture 'coin.png' vs 'enemy.png')".
- **Bullet-hell or particle-like game in plain sprites.** 20,000 bullets of 3 shapes. Today the 8,193rd sprite
  never appears (two buffers each from a pool of 16,384, one error logged). After: all draw.
- **3D game with billboard foliage cards and crates.** Opaque cards with a depth pre-pass (SSAO on) batch in the
  colour pass, the pre-pass and the shadow cascades; boxes of one material batch too.
- **Agent.** `get_render_stats {"batch_breaks": 10}` after `play_scene`; sees `{"reason": "texture", "entity":
  "Enemy (12)", "previous": "Coin (40)"}` x 900; reorganizes the hierarchy or merges textures into a sheet with
  `set_sprite_frames`; checks `batching.saved_by_batching` went up; toggles `graphics.dynamic_batching` with
  `set_project_settings` and compares two `screenshot`s to prove the change is visual-neutral.

## 4. Findings

### 4.1 No automatic batching (Doriax 635449012; dossier verdict confirmed)

- **read** The colour pass calls `drawMesh` once per entity: the opaque queue
  (`engine/core/subsystem/RenderSystem.cpp:16145`), the hierarchy loop of 2D passes (`:16315`) and the
  transparent queue (`:16401`). Component UI draws one by one (`drawUI`, `:11054`, called at `:16322`).
- **read** `drawMesh` (`:8604`) does per draw: `isBatchVisible` (6-plane box test, `:13895`), `selectMeshDraw`,
  `updateMeshBuffers`, `objectMatrices`, `isMirroredDraw` (a determinant), then per submesh texture checks,
  `beginDraw` (`sg_apply_pipeline` + bindings, `engine/renders/sokol/SokolObject.cpp:1019`), and up to ~15
  uniform blocks (fog, object lights, lit frame, shadows x5, Light 2D, sprite rect `:8873`, material params, UV
  sets, object matrices `:8935`), then `sg_draw`.
- **read** Opt-in one-draw paths exist (big-claims #3): instanced Sprite (`InstancedMeshComponent.h:69-75`),
  Points, Tilemap, Particle Systems, HTML UI (`engine/core/htmlui/RmlRendererBatch.cpp`). None merges
  independent GameObjects.
- **read** Doriax's batcher (`3fb49fbba:engine/core/subsystem/RenderSystem.cpp:4280-4840`): only unlit quads
  with vertex colour (`canBatchSprite` requires `Ult`+`Vc4`), consecutive in draw order, never opaque meshes in a
  depth pre-pass (`batchSprite` `:4296`), duplicated uniform setup (`applySpriteBatchUniforms` `:4440`, which
  knows fog, rect, material and matrices only: the reason lit and 2D-lit sprites cannot join), world-space
  vertices (precision loss far from the origin), a STREAM buffer pair sized from the previous frame's demand,
  first frame of a scene and every growth frame drawn unbatched.

### 4.2 Settled sprites and their invalidation (Doriax 1133715fb, bd2c16fb3, 930133bb5)

- **read** `RenderSystem::update` visits every transform each frame (`:14679`) and for a mesh does ~2
  `findComponent`, per-submesh render-state reconciliation (`resolveSubmeshRenderState`, alpha/lighting mode
  checks, texture epochs, `:14759-14866`), the world AABB on change (`:14934`), and the MVP of every transform
  whenever the main camera moved (`:15094-15097`): a scrolling 2D camera recomputes every sprite's MVP.
- **read** Doriax settles a sprite: a CPU copy of baked vertices, the update loop skips it (`:10333-10353`), the
  pass draws it from the copy (`:11429-11438`). Invalidation is an explicit `SpriteComponent::needUpdateBatch`
  set by Mesh/Sprite objects, ActionSystem, MeshSystem and the editor (bd2c16fb3): a direct write that does not
  set it draws stale data (dossier note). Jasper documents direct C++ writes as untracked by change ticks
  (`docs/specs/S07-ecs.md` R14), yet today they work because the draw reads the component every frame.
- **inferred** Every reader of `Transform::modelViewProjectionMatrix` is in `RenderSystem.cpp` (grep of
  `engine` and `editor`): `objectMatrices`/`objectMVP` in `drawMesh`, the depth pre-pass (`:16098`), `drawUI`,
  `drawPoints`, `drawLines`, UI scissor projection. So a batched mesh's MVP is needed only if it draws unbatched.

### 4.3 Culling (Doriax 6f1d13d13)

- **read** `isBatchVisible` → `isInsideCamera` (`:12991`): a full 6-plane test on `mesh.worldAABB`, which sits in
  a ~200 KB `MeshComponent` (big-claims #1), for every drawn mesh in every pass, no cached verdict. A 2D camera's
  planes are axis-aligned; 4 of the 6 plane tests are then two compares each.

### 4.4 Buffers per sprite and the ceiling (Doriax 09bd9af8b skip; big-claims #2)

- **read** `MeshSystem::prepareSpriteMesh` makes the vertex buffer DYNAMIC (`engine/core/subsystem/MeshSystem.cpp:580`)
  with 5 attributes, 64 bytes per vertex (`:573-577`); the index buffer is IMMUTABLE but has no share source, so
  `MeshBufferPool::createRender` (`engine/core/pool/MeshBufferPool.cpp:127`) gives it a buffer of its own.
  2 buffers per sprite, pool `GpuResourceLimits::buffers = 16384` (`engine/core/Engine.h:284`): 8,192 sprites
  minus every other buffer. Past it: one error, the mesh `loadFailed`, not drawn (`RenderSystem.cpp:14919-14929`, flag at `:14927`).
- **read** `updateMeshBuffers` ignores IMMUTABLE buffers (`:8495-8507`): an immutable sprite could not change its
  geometry today without a reload.

### 4.5 Content-shared immutable buffers and the hash (Doriax b666f6cd3; dossier verdict confirmed, uncertain)

- **read** Jasper interns pipelines (`SokolPipelineCache`, S32), better than Doriax: nothing to take there.
- **read** Content sharing exists only for generated smooth normals (`MeshBufferPool::acquireContent`,
  `MeshBufferPool.cpp:155`, caller `RenderSystem.cpp:7727`), keyed by a 64-bit hash alone: a collision draws
  the wrong bytes. Primitives (`createBox` etc., `engine/core/buffer/Buffer.cpp:14`) get private buffers.
- **read** The hash is word FNV-1a (`engine/core/pool/MeshLodPool.cpp:89-103`), used for LOD keys and smooth
  normal keys. **inferred** (dossier, corrected evidence): a bit-63 flip survives the odd multiply, so two such
  flips in one stream cancel; a tight vec2 or stride-16 stream that flips only high words collides.
- **read** `SokolObject` keys vertex bind slots by `BufferInfo{id, offset}` (`engine/renders/sokol/SokolObject.h:24-35`,
  `SokolObject.cpp:509-523`): two attributes from two Buffers that share one GPU buffer by content (same bytes)
  at the same offset collapse into one bind slot with the first one's stride and step function.

### 4.6 Not applicable or covered (dossier `skip` / `none`)

- f9115acce (shader properties first in `canBatchSprite`): our eligibility test orders its cheapest rejections
  first (section 6.3) by construction.
- 930133bb5 (world bounds of moved settled sprites from placed vertices): adopted in section 6.6.
- 09bd9af8b (buffer pool 32,768): Jasper's pool is settable and fails soft; sharing removes the need.

## 5. Product decisions

1. **One mechanism, "Dynamic Batching", for sprites, small meshes and UI elements** (Unity's name for merging
   small meshes; SpriteRenderer batching is always on there). On by default: it is visual-neutral and the
   biggest 2D win on web and mobile. A project setting turns it off for comparison and bisecting; Lua can flip it
   at runtime (A/B in a game, tests).
2. **CPU vertex batching, not auto-instancing.** The batch reuses each object's own pipeline (same shader
   variant, same vertex layout), so there are no new shader variants to compile, warm (S32) or export, and no
   per-platform instancing paths. Small-mesh instancing is known to under-use vertex wavefronts on mobile GPUs
   (assumed, common knowledge), and sprites differ in size, pivot and inset, which instancing would have to
   re-encode. The cost, 64 bytes per vertex uploaded, is bounded by settled objects (copy, not recompute).
3. **The batch draws through the same uniform code as a single draw.** `drawMesh`'s per-submesh uniform
   application is factored into one function that both paths call, with "neutral" per-object values for a
   batch (identity model and normal matrices, the run's origin MVP, full texture rect, white colour factor).
   No duplicated subset (Doriax's cause for unlit-only). Every pass-wide block (fog, lights frame, shadows,
   Light 2D, SSAO, post colour, editor no-fog) applies identically.
4. **Bake what is per object and cheap to bake; compare the rest.** Model matrix → positions, normals,
   tangents; texture rect → UVs; colour factor → vertex colour (when the variant reads vertex colour, else the
   factor joins the key). Everything else per object (material params, alpha cutoff, UV sets, object lights,
   depth material, UI text params) must be byte-equal to the run's head to join it.
5. **Lit sprites batch when their light lists are equal.** `MeshLights` (indices, count, SH, folded flag) equal
   ⇒ identical light blocks. Typical 2D scenes (scene ambient only, or lights reaching everything) batch fully;
   SH folding per object splits runs, correctly.
6. **World form is a per-object, per-camera decision.** An eligible object is always drawn in batch form in the
   colour pass and the camera depth passes, even alone (a run of one). Both passes then compute `gl_Position` from
   identical inputs (`invariant gl_Position`, `engine/shaderlib/includes/surface_vs.glsl:109`), so opaque
   objects batch with a depth pre-pass. Doriax had to exclude them.
7. **Precision: origin cells, not raw world space.** Positions are baked relative to a 4,096-unit grid cell of
   the object's translation; a run never spans two cells; the run's MVP is `VP * T(cell)`. Precision near the
   origin and at 100,000 units is that of today's per-object MVP, and the same object gives the same inputs in
   every pass whatever run it lands in.
8. **No invalidation flag.** RenderSystem is the only path from components to the GPU: geometry reaches it via
   `needUpdateBuffer`/`needReload`, textures via `needUpdateTexture` and the texture epochs, transforms via
   `Transform::needUpdate`, per-draw values (rect, colour, cutoff) by reading them. The cache is invalidated
   exactly there and per-draw values are compared every frame (36 bytes). A direct C++ write the unbatched
   path would show, the batched path shows too.
9. **Reorder only where it is provably invisible.** Orthographic cameras with a ±Z view axis, where world XY
   bounds disjointness implies screen disjointness; a draw jumps back only over draws whose bounds it does not
   touch. Any foreign draw, scissor-independent state change or pass end flushes everything in order.
10. **Depth-only passes group freely.** Shadow casters and the depth pre-pass write depth only; their order
    changes nothing in the depth buffer, so eligible casters are grouped by state over the whole pass.
11. **Sprites become immutable and content-shared; volatile geometry goes private.** A sprite's geometry changes
    rarely (size, pivot, flip, inset, lattice grid). A change re-acquires the shared buffer of the new bytes. A
    mesh that changes twice within 30 updates gets a private DYNAMIC buffer for the rest of its load (no
    immutable buffer created per frame: on Vulkan each is a blocking staging submit, `Engine.h:258-260`).
12. **Every immutable vertex/index buffer without a share source is content-shared** (primitives, generated
    geometry), keyed by XXH3-128 + size + type. 128 bits makes a collision practically impossible (birthday bound
    2^64); the 64-bit word FNV is replaced everywhere it keys GPU data (LOD keys, smooth normals).
13. **Unity naming in the editor**: Project Settings > Graphics > "Dynamic Batching"; Game view Stats
    "Batches: N  Saved by batching: M"; Frame Debugger "Why this draw call can't be batched with the previous
    one". Agent-facing names in snake_case (`dynamic_batching`, `saved_by_batching`, `batch_breaks`).

## 6. Design

### 6.1 Files and structure

- New `engine/core/subsystem/RenderSystemBatch.cpp` (RenderSystem member functions, like
  `RenderSystemShadows.cpp` and `RenderSystemParticles.cpp`) and `engine/core/render/DrawBatcher.h/.cpp`
  (renderer-independent data: runs, records, staging, reorder window, break reasons).
- New `engine/core/render/FrustumCull.h/.cpp` (planes with a serial and the axis-aligned fast path), reusable by
  `draw-perf` for all meshes.
- New `engine/core/util/ContentHash.h/.cpp` over vendored xxHash (`engine/libs/xxhash/xxhash.h`, BSD-2-Clause,
  v0.8.x, header kept, `NOTICE.md` entry).
- Changes in `RenderSystem.{h,cpp}`, `RenderSystemShadows.cpp`, `MeshSystem.cpp`, `MeshBufferPool.{h,cpp}`,
  `MeshLodPool.cpp`, `SokolObject.{h,cpp}`, `SokolCamera.cpp`, `SokolSystem.{h,cpp}`, `SokolPipelineCache.cpp`,
  `SokolPassRecorder.{h,cpp}`, `RenderStats.h`, `Engine.{h,cpp}`, Lua bindings, editor and MCP (6.10-6.12).

### 6.2 The deferred-draw hook (render layer)

The batcher keeps runs open until something else draws; correctness must not depend on remembering every flush
site. `SokolSystem` gets:

```cpp
// a pending batch (DrawBatcher) drawn before any other GPU command of the pass
using DeferredFlush = void (*)(void* owner);
static void setDeferredFlush(DeferredFlush flush, void* owner); // null: none (zero cost)
static void flushDeferred();          // runs it once, guarded against re-entry
// scissor requested while a flush is installed: applied before the next foreign draw (or by a run)
static void requestScissor(int x, int y, int w, int h, bool originTopLeft);
```

- `SokolObject::beginDraw` (both overloads, top of the function), `SokolPipelineCache` apply (`:664`),
  `SokolCamera::applyViewport*` and `SokolSystem::endPass` call `flushDeferred()` first. With no flush installed
  this is one predictable branch.
- **Lazy scissor.** While a flush is installed, `SokolCamera::applyScissor*` only records the desired rect
  (`requestScissor`); `flushDeferred` and every foreign `beginDraw` apply it when it differs from the last
  applied rect. Each run captures the desired rect at its first item and applies it before its draw. So the
  colour pass's per-element scissor reset (`RenderSystem.cpp:16387-16390`) costs nothing and UI children of a
  clipping panel still batch. Without a flush installed, scissor applies at once, as today.
- RenderSystem installs the flush for each pass it batches (colour, depth pre-pass, shadow slot, G-buffer) and
  removes it after its own final flush.

### 6.3 Eligibility (per object, cached in its record)

`RenderSystem::batchForm(entity, mesh, ...)` returns `BatchForm::NONE` with a `BatchBreak` reason, or the form.
Cheapest checks first. A mesh is eligible when all hold:

- loaded, not `needReload`, not `needVariantReload` pending a swap, `numSubmeshes == 1`, `vertexCount > 0`,
  primitive TRIANGLES or TRIANGLE_STRIP (expanded to a list), at most 64 vertices, indices absent or 16-bit;
- no InstancedMesh, Terrain, Tilemap; no skinning, morph targets, lattice (`mesh.lattice.count == 0`), VAT,
  `lodData`/`numLods > 1`, smooth normals, extra pass, custom/default/library shader (`customShaderId`,
  `customShader`, `customDepthShader`, scene default shader, `manifest`), `hasIBL`, mirror slot, lobe variant
  (`pbrParamsSize != 0`), Stochastic Tiling, a render-target base texture (`isFramebuffer()`), a material fluid
  lent this frame (`frameFluids->getMaterialTexture(entity)`);
- transform affine; not a fake billboard; not an editor-overridden object in this pass (`isObjectOverridden`).
  Real billboards are eligible (their model matrix is turned per camera before the pass) but never settle;
- colour shader properties within the allowlist `Ult Uv1 Uv2 Puc Shw Nor Nmp Tan Vc3 Vc4 Txr Fog L2d S2d Ams Aop
  Vtl Lc4 L16 Pcl Shq Sao` (bits 0-4, 6-12, 21-25, 28-31, 33; `ShaderPool.cpp:689-735`). An allowlist, so a
  future property bit does not batch until someone adds it here with a test.

A UI element is eligible when: loaded, buffer size > 0, built-in UI shader (`customShaderId == 0`, no library
bindings), at most 256 vertices (a 9-patch has 16, a 64-glyph label 256), not drawn into a FLATTEN canvas layer
by `drawCanvasLayer` (layers batch inside their own pass: same code path), no texture override this frame.

### 6.4 The batch key (per item per pass, compared with the open run's head)

```cpp
struct BatchKey {
    uint8_t pass;            // COLOR, DEPTH_PREPASS, SCENE_DEPTH, SHADOW, GBUFFER, UI
    uint8_t pipType;         // PipelineType
    bool mirrored;           // isMirroredDraw: the mirrored pipeline variant (gl_FrontFacing)
    bool noCull;             // passNoFaceCulling (editor)
    uint32_t pipeline;       // ObjectRender::getPipelineId(pipType, mirrored, noCull)
    int32_t cell[3];         // origin cell (decision 7)
};
```

Plus byte-equality of what is not baked: the render's texture views and samplers (`SokolObject::sameBindings
(const SokolObject&)`, a memcmp of `sg_bindings::views/samplers`, buffers excluded); material params bytes
`[16, paramsSize)` (`getLitParamsSize`); UV set selectors; `MeshLights` when the variant has `Puc` or `Vtl`; the
colour factor when the variant reads no vertex colour; the depth material `vec4(baseColor.a, alphaCutoff)` in
depth passes; for UI, `drawColor` when the element has no vertex colour, and `textParams` when SDF. The scissor
rect is per run (6.2), not a key. Equal pipeline id means equal shader, layout, blend, depth, stencil and
primitive (interned pipelines, S32).

### 6.5 Baking (one item)

From the CPU buffer (`mesh.buffer.getData()`), the item's model matrix `M = objectModel(transform)`, its normal
matrix `N` (`transform.normalMatrix` when kept, else `inverse(M).transpose()`), the origin cell `C`:

- position `p' = M * p - C` (the cell subtracted in double, stored in float);
- normal `n' = normalize(N3 * n)` (variants with `Nor`);
- tangent: `t3 = normalize(M3 * t.xyz) * rs.x`, `w' = t.w * sign(det M3) * rs.y * rs.x`, with `rs` the signs of the
  texture rect size (`tangent_frame.glsl`). The shader with identity matrices and a full rect then computes the
  same bitangent: `cross(N, s·T)·w·det·sy·sx = cross(N, T)·w·det·sy` (`surface_vs.glsl:465-489`);
- `uv' = uv * rect.zw + rect.xy` for UV1 and UV2 when the submesh has a texture rect (`surface_vs.glsl:296-318`);
- colour `c' = c * baseColorFactor` when the variant reads `Vc4` (`pbr.glsl:136-155`: factor x texture x vertex
  colour; a Vc3 variant only bakes rgb and keys alpha); UI: `c' = c * drawColor` (`ui.frag:58-66`).

Uniforms of a batch draw: model and normal matrices identity, `mvp = flipY? * VP * T(C)` (as `updateMVP` flips,
`RenderSystem.cpp:14269-14274`), rect `(0,0,1,1)`, colour factor `(1,1,1,1)` (alpha too: it is baked); the depth
block `{I, VP, VP * T(C)}`; the G-buffer block `{I, VP, I}`; UI `mvp = VP * T(C)`.

### 6.6 Batch records (settled objects)

`RenderSystem` holds, per scene, records for eligible objects (paged index entity → record, 1,024 entities per
page, a page freed when empty: entity ids only grow, Doriax's `SettledPage`):

```cpp
struct BatchRecord {
    Entity entity;
    uint16_t vertexCount, indexCount;   // indices expanded (strips, non-indexed)
    uint8_t kind;                       // MESH or UI
    BatchBreak eligibility;             // NONE or why not (cached; recomputed when invalidated)
    uint32_t arenaOffset;               // baked local vertices (rect and colour applied), then placed ones
    Rect rect; Vector4 color; float cutoff;   // per-draw values the bake used
    Matrix4 placedWith;                 // model matrix of the placed vertices
    int32_t cell[3];
    uint32_t mvpSerial;                 // 6.7
    bool hasInstmesh, hasTerrain, hasTilemap;  // saves the pass loop's 3 lookups
};
struct BatchBounds { float lo[3], hi[3]; uint32_t frustumSerial; bool inside; };  // hot array, same index
```

- Vertices live in one arena (`std::vector<unsigned char>`, compacted when a quarter is free). A quad record is
  ~100 bytes plus 2 x 256 bytes of vertices plus 32 bytes of bounds.
- **Created** the first time an eligible object is submitted. **Re-baked in place** when: `needUpdateBuffer`
  was set (geometry, consumed by RenderSystem, 6.8), the mesh loaded or reloaded, `needUpdateTexture` or a
  texture epoch rebinding fired, the object's rect/colour/cutoff differ from the record (compared at submit).
  **Re-placed** (positions, normals, tangents, bounds) when `transform.needUpdate` is seen in `update()`: the
  mesh's `worldAABB` is then written from the placed bounds instead of `modelMatrix * aabb` (930133bb5).
  **Dropped** when the entity is destroyed, the mesh or Sprite/UI component removed, a component added that makes
  it ineligible (`onComponentAdded/Removed`, `RenderSystem.cpp:16938-17036`), the setting turned off, or the
  scene unloaded.
- No hold/backoff (Doriax's `SPRITE_SETTLE_HOLD`): a record changing every frame is re-baked every frame, which
  costs what an unsettled sprite costs; nothing else is paid on the transition.

### 6.7 Lazy MVP

`Transform::modelViewProjectionMatrix` is read only by RenderSystem (4.2). For an entity with a record:

- `update()` does not call `updateMVP` for it (`:15094-15097`); it sets `record.mvpSerial = 0` when the
  transform changed. RenderSystem keeps `mvpSerial`, bumped when the main camera changed and at the start of every
  per-camera MVP pass (`perCameraMVP`), whose pass loops also skip `updateMVP` for records.
- Every unbatched use calls `ensureMVP(record, index, transform, camera, cameraTransform)`: if
  `record.mvpSerial != mvpSerial`, `updateMVP` then stamp. Call sites: `drawMesh` entry when the entity has a
  record (an item that became ineligible this frame, the Vulkan-refusal fallback), the depth pre-pass loop
  (`:16095-16101`) for items not in batch form, `drawUI`.
- Records are never billboards that turn (they never settle; their MVP path is unchanged).

### 6.8 Geometry changes and shared buffers

- In `update()`, for an entity with a record: if `mesh.needUpdateBuffer`, upload now (`updateMeshBuffers`), re-bake
  the record, clear the flag. At submit in a pass, the same check catches writes made after `update()` (late
  script callbacks): `needUpdateBuffer` set → re-bake and upload before use. Without a record nothing changes.
- **Content-shared immutable buffers.** `MeshBufferPool::createRender`: an IMMUTABLE vertex/index buffer with
  bytes and no share source acquires by content: key `{owner = null, hash128, size, type}`
  (`GpuBufferShareKey` gets `uint64_t idHi`). `acquireContent` takes a `ContentHash128`. `holdsSource` for a
  content-keyed render compares the current bytes' hash (cached on the `Buffer` with a write counter, so a load
  hashes once).
- **Sprites immutable.** `prepareSpriteMesh` sets the vertex buffer IMMUTABLE (`MeshSystem.cpp:580`); its index
  buffer, already immutable, now shares: all quads use one index buffer. `createSprite`/`createSpriteGrid`
  keep setting `needUpdateBuffer` (a same-size change) or `needReload` (a grid size change).
- **Re-acquire on change.** `updateMeshBuffers` on an IMMUTABLE content-keyed buffer: acquire the new bytes'
  shared buffer, then retarget every render of the mesh that bound the old one (`render`, `depthRender`,
  `gbufferRender` of each submesh) with the new `SokolObject::retargetVertexBuffer(oldId, sg_buffer)` /
  `retargetIndexBuffer` (rebinds and re-keys `bufferToBindSlot`; `replaceVertexBuffer` cannot, it keeps the
  load-time key). No reload, no pipeline change (same layout).
- **Volatile.** RenderSystem keeps `lastReacquire[entity]` (update counter). A second re-acquire within 30 updates
  switches that buffer to a private DYNAMIC one (`setUsage(DYNAMIC)`, create, retarget) for the rest of the load;
  a reload starts shared again. Logged at verbose level.
- **Bind slots.** `BufferInfo` becomes `{id, offset, stride, perInstance}` (comparisons and `operator<` over all
  four), so equal-byte attributes of different stride or step never share a slot. `replaceVertexBuffer` and the
  retarget functions match on `id` only, as today.
- **Hash.** `ContentHash128 hashContent128(const void*, size_t, uint64_t seed)` and `uint64_t hashContent64(...)`
  (XXH3). `MeshLodPool::hash` becomes `hashContent64` (its seed chaining kept); the smooth-normal key
  (`RenderSystem.cpp:7248-7322`, `7686`) takes `hashContent128`. Keys in memory only: nothing persisted changes.

### 6.9 Passes

**Colour pass** (`renderCameraPass`): after the sky and video far planes, RenderSystem begins a `BatchPass` for
the camera (`pipType`, frustum, VP, flip, depth pre-pass flag, reorder allowed) and installs the flush. The three
`drawMesh` call sites and `drawUI` call `submitMesh` / `submitUI`, which either add to a run or flush and draw
alone (ineligible). `drawMesh` and `drawUI` keep their signatures for every other caller. The pass ends with a
flush before HTML UI documents (any later draw would flush anyway).

**Runs.** A run is `{head item, key, scissor, vertex range, index range, allQuads, items}`. Adding an item:
re-bake if invalid (6.6), cull with the cached verdict (6.9.1; culled items are skipped and do not break a run),
then join the newest compatible open run or open a new one. Limits: 65,536 vertices per run (16-bit indices);
at most 8 open runs (reorder window); a 9th flushes the oldest.

**Drawing a run.** Upload its vertices (and indices unless all items are quads) by `appendBuffer` into the
pass's stream chunk; on the head item's render: `beginDraw(pipType, noCull, mirrored)` (if it waits for a warm
pipeline, S32, try the next item's render; if all wait, the run waits like its items would), apply its scissor,
the shared uniform function with neutral per-object values, `replaceVertexBuffer(own, chunk, offset)`,
`setIndexBuffer(quadIndices or chunk, offset)`, `draw(0, indices, 1)`, restore both. A run of one item draws
the same way (decision 6).

**Stream chunks.** Per RenderSystem: a list of STREAM vertex and index chunks. A full chunk adds another at once
(`max(2 x last, needed)`), so no frame falls back for lack of room. `prepareBatchBuffers()` at the first pass of a
frame: if more than one chunk was used last frame, replace them with one of 1.5 x the summed demand; free them
after 300 frames below a quarter of capacity. Caps: 16 MB per kind on Android, iOS and the web, 64 MB on desktop;
past the cap, runs draw item by item (in world form, so pre-pass agreement holds). Created lazily: a scene
without eligible objects allocates nothing. One immutable quad index buffer (16,384 quads, 192 KB) per process,
made at the first quad run.

**Vulkan stream budget.** An `appendBuffer` refused by `SokolSystem::reserveStreamBytes` (`SokolSystem.cpp:554`)
skips that run this frame, logged by the budget's existing once-per-frame warning: drawing its items unbatched
would break pre-pass agreement. (16 MB on mobile is ~65,000 quads a frame.)

**Reorder (decision 9).** Allowed when the camera is orthographic or UI, its view axis is ±Z
(`|cameraTransform.worldRotation * (0,0,1) . Z| > 1 - 1e-6`), and the pass has no scene-colour reader pending.
An item may join open run R only if its world XY bounds intersect no item's bounds in runs opened after R (each
run keeps its items' XY rects; scan capped at 256 rects, else open a new run). Runs draw in creation order. In
other cameras only the newest run can be joined (strict order).

**Depth pre-pass and scene depth** (`:16095-16101`, `renderOpaqueDepth` `:9364`): items in batch form submit to a
depth `BatchPass` with `depthRender` and grouping by state over the whole pass (decision 10); others draw as
today. **Shadow casters** (`RenderSystemShadows.cpp:1605-1650`): eligible casters (not `shadowsBillboard`
billboards, which get a light-view override) submit per slot, grouped by state, cull against the cascade planes
with the same cache keyed by that slot's frustum serial; the static shadow cache sees the same casters
(`shadowCasterRevision` untouched). **G-buffer** (`renderGBufferPass` `:9958`): eligible opaque items batch in
order (equal-depth ties stay as today).

#### 6.9.1 Culling

`FrustumCull { Plane planes[6]; float far; uint32_t serial; bool axisAligned; uint8_t axis[6]; float sign[6], d[6]; }`.
`set()` bumps `serial` only when the planes or far clip changed (memcmp), and finds whether each plane is
axis-aligned (one non-zero normal component). `inside(lo, hi)`: axis-aligned planes test `sign * (centre[a]) + d
>= -|sign| * halfExtent[a]`, the general path is today's box test. A record's verdict is reused while
`bounds.frustumSerial == cull.serial`. Records re-placed this frame reset their serial. Two views drawing the
same scene alternate serials (recompute, as today).

### 6.10 Engine and Lua API

- `Engine::setDynamicBatching(bool)`, `Engine::isDynamicBatching()` (default true). Turning it off drops all
  records and stream chunks at the next update; on rebuilds them as objects draw.
- Lua: `Engine.dynamicBatching` (static property, CoreClassesLua.cpp next to `targetFrameRate` `:2220`). The Lua
  API data is regenerated by the build (`tools/lua_api`); `docs/mcp/guide/lua-api.md` gets one line.
- C++ scripts: the same Engine statics.
- `RenderSystem::getBatchStats()` (6.12) and `RenderSystem::explainBatchBreaks(max)` for tools.

### 6.11 Editor

- **Project Settings > Graphics > Dynamic Batching** (checkbox, tooltip: "Draws consecutive Sprites, small
  meshes and UI elements that share a material as one draw call. Same image; turn off to compare."). `Project`
  field `dynamicBatching` (default true), `project.yaml` key `dynamicBatching`, `StreamSchema` entry, Stream
  encode/decode with default when absent, `SettingsRows` key `graphics.dynamic_batching` with search words
  "batching batches draw calls saved by batching sprites", applied to the editor at once
  (`Engine::setDynamicBatching`), written into exports: `Generator.cpp` main content
  (`Engine::setDynamicBatching(false);` only when off) and the prebuilt `PlayerConfig` (`graphics.dynamicBatching`,
  `engine/core/player/PlayerConfig.{h,cpp}`, `ExporterPrebuilt.cpp:613` next to `sdfPrimitives`). Setting
  changes go through the existing Project Settings path (not undoable, like every project setting).
- **Game view Stats** (`editor/window/GameWindow.cpp:1221`): a line `Batches: %u  Saved by batching: %u` after
  "Draw calls".
- **Frame Debugger** (`editor/window/profiler/FrameDebuggerWindow.cpp`): `DrawRecord` gets `uint16_t batchItems`
  and `uint8_t batchBreak`; a batch draw reads "Dynamic batch (37 objects)"; selecting a draw shows "Why this draw
  call can't be batched with the previous one: different texture". RenderSystem annotates the next draw through
  `SokolPassRecorder::annotateNextDraw(items, reason)` only while the draw list is recorded.
- **Profiler monitors**: `Batches` and `Saved by batching` series (`get_frame_times` `monitors`).

### 6.12 MCP

- **`get_render_stats`** (`editor/mcp/tools/ProfilerTools.cpp`): a `batching` object per view:
  `{enabled, batches, batched_objects, saved_by_batching, settled, rebaked, replaced, culled, cull_cache_hits,
  upload_bytes, stream_chunks, runs_skipped, reordered}` and `breaks: {<reason>: count}`. New optional parameter
  `batch_breaks` (integer 0..100, default 0): the last frame's first N breaks of that view as
  `[{reason, detail, entity, entity_name, previous, previous_name, pass}]` (entity names through the scene;
  `detail` e.g. `"baseColorTexture coin.png vs enemy.png"`). Reasons (stable snake_case enum):
  `texture, sampler, pipeline, material_params, alpha_cutoff, uv_sets, object_lights, color_factor, ui_color,
  text_params, mirrored, origin_cell, run_vertex_limit, overlap, foreign_draw, ineligible_custom_shader,
  ineligible_extra_pass, ineligible_ibl, ineligible_skinned, ineligible_instanced, ineligible_lattice,
  ineligible_vat, ineligible_lod, ineligible_vertex_count, ineligible_submeshes, ineligible_render_target,
  ineligible_fake_billboard, ineligible_shader_property, disabled`.
- **`get_gpu_passes`** with `include_draws`: each draw gets `batch_items` (0: not a batch) and `batch_break`.
- **`get_project_settings` / `set_project_settings`**: `graphics.dynamic_batching` (bool, describe text as the
  tooltip).
- No new tool. Budget: `tools/mcp/tool_budget.py --check tools/mcp/budget.json`; `docs/mcp/TOOLS.md` regenerated.
- Guide: `docs/mcp/guide/profiling.md` gets a "Batching" section (reasons, how to fix each: same texture via sprite
  sheets, group siblings by texture, avoid per-object material values, what never batches); the agent guide's
  profiling digest gets at most one line ("draw calls high: get_render_stats batch_breaks").

### 6.13 Export and platforms

- No new shader variant, no new pipeline type: the shader keys an export precompiles are unchanged
  (`export-pipeline`'s key collection needs nothing). The mirrored pipeline variants exist today.
- GL 3.3 / GLES3 / WebGL2: STREAM appends are `glBufferSubData`; 16-bit indices; no extension.
- Metal, D3D11: per-attribute bind slots on non-interleaved layouts (`SokolObject.cpp:501-507`) are handled by
  `replaceVertexBuffer`'s offset per slot; append offsets are 4-byte aligned by sokol; index offsets even.
- Vulkan: stream staging budget (6.9); immutable creations reduced by sharing; the volatile rule avoids
  per-frame immutable uploads.
- Windows, macOS, iOS code paths cannot be built here; nothing here is platform-specific beyond sokol, which the
  implementer keeps compiling by the existing patterns (the Metal and D3D11 bind-slot code is shared C++).

## 7. Behaviour rules and edge cases

1. Images with batching on and off match within 1 per channel on llvmpipe/lavapipe, except where float rounding
   of positions moves a triangle edge across a pixel centre (positions differ by at most one float ulp of the
   cell-relative coordinate); tests place geometry off pixel-centre edges.
2. Order: within a strict-order camera, the items of a run draw in submission order, and runs in creation order;
   a run's primitives draw in item order (GPU primitive order holds for blending and stencil).
3. A hidden, inactive, layer-culled, frustum-culled or editor-hidden object is not submitted and breaks nothing.
4. Anything not submitted that draws (Points, Lines, Trails, particles, video planes, fluids, a canvas layer
   composite, HTML UI, the atmosphere composites, a scene-colour copy) flushes every open run first (6.2).
5. A scissor change does not flush (lazy scissor); a viewport change, a pass end and any foreign draw do.
6. An object that turns ineligible (custom shader set, extra pass, skinning) draws alone through `drawMesh`, its
   MVP made by `ensureMVP`; its record is dropped.
7. A sprite whose texture is still loading binds the stand-in like today; the stand-in's view is in the key, so it
   batches with other stand-in sprites of that texture and re-keys when the real texture arrives (epoch).
8. A texture rect with negative size (mirrored frame) bakes reversed UVs and flips the tangent as 6.5.
9. A mirrored model matrix (negative scale) uses the mirrored pipeline variant (key `mirrored`), as `drawMesh`.
10. Equal-depth opaque sprites in a 3D camera with a pre-pass: same inputs in both passes (decision 6, 7).
11. Multiple cameras / render-to-texture cameras / probes: each pass submits with its own camera; records hold
    world data, culling verdicts are per frustum serial, MVPs per `mvpSerial`.
12. Editor Scene view while playing (secondary view with `passObjects`): overridden objects (billboards turned
    to that view) draw alone; others batch with the view's VP.
13. Async loading thread: records and batching run on the main thread only; a mesh loaded by the loading thread
    becomes eligible when `loaded` is seen.
14. Turning Dynamic Batching off mid-play: next update drops records; every object draws alone with a fresh MVP
    (`mvpSerial` bump forces `updateMVP` for all former records).
15. Vulkan stream budget exceeded: the run is skipped this frame (6.9), the budget logs once per frame.
16. A mesh with `needUpdateBuffer` every frame (scripted vertices) re-bakes every frame and, if immutable, turns
    private after its second change within 30 updates.
17. Over 65,536 vertices in one run: a new run (break reason `run_vertex_limit`).
18. Scene layers: each scene's RenderSystem batches its own passes; chunks per RenderSystem.
19. Hidden or minimized game (`platform-lifecycle`): updates without passes keep records placed; chunk demand is
    measured from drawn frames only.

## 8. Performance and cost

- **Disabled** (setting off): one bool test per submitted object; no records, no chunks, no flush installed
  (`flushDeferred` is a null check). Unused (no eligible objects): no allocation; the flush is installed per pass
  but nothing is pending (one branch per foreign `beginDraw`).
- **Per eligible object per frame**: settled and unmoved: cull verdict lookup, a 36-byte per-draw compare, a key
  compare against the run head (pipeline id, bindings memcmp ~176 bytes), a copy of its placed vertices
  (256 bytes for a quad). Moved: placement (4 vertex transforms, normals/tangents for lit variants) and bounds.
  No MVP, no per-object uniforms, no `beginDraw`.
- **Per run**: one upload (vertices; indices only for non-quad runs), one `beginDraw`, the pass-wide uniform
  blocks once, one draw.
- **Memory**: ~650 bytes per quad record (record, two vertex copies, bounds); 10,000 sprites ≈ 6.5 MB CPU. Stream
  chunks sized to demand (10,000 visible quads ≈ 2.56 MB of vertices per frame; sokol keeps a copy per frame in
  flight on Metal and Vulkan).
- **Buffers**: per distinct sprite shape one vertex buffer; one index buffer for all quads; plus ≤ 4 stream
  chunks and the quad index buffer per process.
- **Budgets** (benchmark server, `JASPER_TEST_PERF_SLACK`): `SpriteBatchBench.TenThousandSettledSprites` — the
  colour pass of 10,000 settled sprites of one sheet issues ≤ 2 draws, and `RenderSystem::draw` CPU time is
  ≤ 25 % of the same scene with batching off; `SpriteBatchBench.TenThousandMovingSprites` ≤ 50 %. Counts are
  asserted always, times only with the slack variable (docs/testing.md).

## 9. Compatibility

- `project.yaml` gains `dynamicBatching` (bool). Absent ⇒ true. A new field with a default: no format version bump
  (CLAUDE.md "new fields get defaults"). Scenes, prefabs and assets are unchanged.
- Exported games built before this change are unaffected; re-exported ones get batching on unless the project
  turned it off.
- Visual output is unchanged by design (7.1). Projects that relied on the exact per-sprite draw count (none known;
  MCP tests asserting draw counts will be updated with the reason) change counts.
- Runtime behaviour change: an immutable sprite buffer is now shared; C++ code that wrote into a sprite's
  `BufferRender` directly (none in the tree, grep) would write into a shared buffer. Sprite geometry edits go
  through `MeshComponent::buffer` + `needUpdateBuffer`, which re-acquires (6.8).

## 10. Tests

Suites per `docs/testing.md`. GPU suites end in `Gpu`, run on glcore and Vulkan (lavapipe). "Red" = the run on
`main` before the change, recorded in the commit message.

| # | Suite / file | Test | Asserts | Red control |
|---|---|---|---|---|
| T1 | core `engine/tests/core/render/SpriteBatchGpuTest.cpp` | `SpriteBatchGpu.ConsecutiveSpritesDrawOnce` | 100 sprites of one texture, 2D camera: colour pass draws = 1 for them (`RenderStats` pass draws minus the clear-only baseline) | 100 draws on main |
| T2 | same | `SpriteBatchGpu.MatchesUnbatched/{Unlit,Light2D,Shadow2D,LitPunctual,LitVertex,LitShadows,NormalMap,MirroredScale,MirroredFrame,TextureRect,AlphaMask,AlphaOpaque,Fog,PostColor}` | image with batching on equals off, max channel diff ≤ 1; the on-run has fewer draws | (equivalence; the draw count part is red on main) |
| T3 | same | `SpriteBatchGpu.TextureChangeBreaksRun` | perspective camera (strict order), transparent sprites A A B A → 3 draws, break reason `texture` for B and the last A | 4 draws on main |
| T4 | same | `SpriteBatchGpu.ReorderJoinsDisjoint` | 2D camera: A B A B disjoint → 2 draws, images equal; A B A with the second A over B → 3 draws | 4 / 3 on main |
| T5 | same | `SpriteBatchGpu.LitRunsSplitByObjectLights` | two groups lit by different point light sets → one run per group, images equal | n draws on main |
| T6 | same | `SpriteBatchGpu.DepthPrepassAgrees` | perspective camera, SSAO on (pre-pass), 200 opaque alpha-masked cards at equal and different depths: image equals batching off, no background pixel shows through a card | — (pins decisions 6 and 7: a deliberate red run with a per-run origin, or with the pre-pass drawing items unbatched, shows holes) |
| T7 | same | `SpriteBatchGpu.ShadowCastersGroup` | sprites casting into a directional cascade: shadow pass draws ≤ number of states; image equal | n draws on main |
| T8 | same | `SpriteBatchGpu.FarFromOrigin` | sprites at x = 100,000 drawn by a camera there: image equals batching off (raw world-space batching fails this) | — |
| T9 | same | `SpriteBatchGpu.DirectWritesVisible` | settled sprite; C++ writes `baseColorFactor`, `textureRect`, `alphaMode`, buffer + `needUpdateBuffer` without any other call: next frame's image equals batching off | — (guards decision 8) |
| T10 | same | `SpriteBatchGpu.LazyMvpWhenTurningIneligible` | settled sprites under a scrolling camera for 10 frames, then one gets a custom shader: it draws at its true position | fails with `ensureMVP` removed |
| T11 | same | `SpriteBatchGpu.ScissorKeepsRuns` | 30 Images inside a clipping panel: 1 draw, clipped image equal to off | 30 on main |
| T12 | same | `SpriteBatchGpu.UIElementsBatch` | 20 Images of one texture + Canvas Group alpha 0.5: 1 draw, equal image; a Text pair of equal colour batches, unequal does not (`ui_color`) | 20 on main |
| T13 | same | `SpriteBatchGpu.ChunkGrowthSameFrame` | first frame of 20,000 quads: every run batched (no item drawn alone), 2+ chunks; second frame: 1 chunk | — |
| T14 | same | `SpriteBatchGpu.VulkanStreamBudgetSkipsRun` (Vulkan only) | `streamStagingBytes` 64 KB, 2,000 sprites: the over-budget runs are skipped, one budget warning, no crash, next frame with budget restored draws all | — |
| T15 | same | `SpriteBatchGpu.SpriteCeilingLifted` | `setGpuResourceLimits(buffers = 512)`, 2,000 sprites of 3 shapes: none `loadFailed`, all visible, `MeshBufferPool::getBufferCount() ≤ 8` | fails on main: ~250 load, the rest `loadFailed` |
| T16 | same | `SpriteBatchGpu.VolatileGeometryGoesPrivate` | a sprite resized every update: after its 2nd change its vertex buffer is DYNAMIC private; immutable creations stop | — |
| T17 | same | `SpriteBatchGpu.DisabledCostsNothing` | setting off: no records, no stream chunk created (`SokolVramTracker` has no "batch" allocation), draw counts as main | — |
| T18 | core `engine/tests/core/render/FrustumCullTest.cpp` | `FrustumCull.AxisAlignedMatchesGeneral` | 10,000 random boxes against 2D and rolled-ortho planes: fast path == general path; `serial` bumps only on change | — |
| T19 | core `engine/tests/core/pool/MeshLodPoolTest.cpp` | `MeshLodPool.HashSeparatesHighWordFlips` | two vec2 streams differing by bit-63 flips in an even count of words: different hashes | equal under word FNV on main |
| T20 | core `engine/tests/core/pool/MeshBufferPoolTest.cpp` (Gpu suite `MeshBufferPoolGpu`) | `ContentSharingByBytes` | equal bytes share one GPU buffer; one byte different, or same bytes as index vs vertex, do not; a primitive Box x 100 → 2 buffers | 200 buffers on main |
| T21 | core `engine/tests/core/render/ObjectRenderShaderGpuTest.cpp` | `ObjectRenderShaderGpu.BindSlotsKeepStrideApart` | two attributes from equal-content buffers, strides 8 and 16: two bind slots, both attributes read correctly | one slot on main (garbage in the second) |
| T22 | core `engine/tests/core/render/SpriteBatchBenchTest.cpp` | `SpriteBatchBench.*` | section 8 budgets ([bench] lines) | — |
| T23 | editor `editor/tests/stream/StreamProjectGraphicsTest.cpp` | `DynamicBatchingDefaultsOn`, `DynamicBatchingRoundTrips` | a project.yaml without the key loads true; false saves and loads | key unknown on main |
| T24 | editor `editor/tests/GeneratorTest.cpp` (existing pattern) | `Generator.WritesDynamicBatchingOff` | main content has `Engine::setDynamicBatching(false);` only when off; `PlayerConfig` round-trips `dynamicBatching` | — |
| T25 | MCP `tools/mcp/tests/test_batching.py` (`CI = True`) | `test_sprites_batch_and_report_breaks` | own 2D project, 60 sprites of 2 textures interleaved (overlapping) via MCP, play: `get_render_stats.batching.saved_by_batching ≥ 1`, `batch_breaks` names an entity with reason `texture`; `get_gpu_passes include_draws` shows `batch_items > 1` | stats key absent on main |
| T26 | same | `test_toggle_is_visual_neutral` | `set_project_settings graphics.dynamic_batching false` / true: `screenshot` Game view pngdiff ≤ 1; `saved_by_batching` 0 when off | — |
| T27 | same | `test_lua_toggle` | `Engine.dynamicBatching = false` from a Lua script: stats follow | — |
| T28 | Lua doc test | `docs/mcp/guide/lua-api.md` block | `Engine.dynamicBatching` documented and runnable | — |

## 11. Acceptance criteria

1. T1-T17 pass on glcore and Vulkan (lavapipe); T18-T24 pass; T25-T28 pass in the MCP/Lua suites.
2. On `main` (red), T1, T3, T4, T5, T11, T12, T15, T19, T20, T21, T23, T25 fail as the table says; the run is
   recorded.
3. A 2D scene of 10,000 settled sprites from one sheet draws them in ≤ 2 draw calls (T22 count assertion).
4. 2,000 sprites load and draw with a 512-buffer pool (T15); 20,000 sprites of 10 shapes hold ≤ 30 GPU
   buffers for their geometry (T13 asserts `MeshBufferPool::getBufferCount()`).
5. With Dynamic Batching off, draw counts equal `main`'s and no batch allocation exists (T17).
6. Opaque sprites batch in a camera with a depth pre-pass without holes (T6) and far from the origin (T8).
7. Lit, 2D-lit and unlit sprites batch (T2 variants each show fewer draws).
8. Project Settings > Graphics shows "Dynamic Batching"; Game view Stats shows "Batches" and "Saved by
   batching"; the Frame Debugger shows the break reason of a selected draw (screenshots from an isolated editor,
   kept outside the repo, referenced in the PR).
9. `get_render_stats` returns `batching` and `batch_breaks`; `graphics.dynamic_batching` is listed by
   `get_project_settings describe`; `docs/mcp/TOOLS.md` regenerated, `tool_budget.py --check` passes,
   `gen_tool_docs.py --check` passes.
10. Docs: `docs/rendering/batching.md` (what batches, rules, break reasons, sheets and hierarchy advice, costs),
    `docs/mcp/guide/profiling.md` section, `docs/unity-glossary.md` rows (Dynamic Batching; Stats "Batches",
    "Saved by batching"; Frame Debugger batch reason), `lua-api.md` line, `NOTICE.md` xxHash entry.
11. `tools/check-changed` green on the branch; WERROR build clean; `tools/smoke-test.sh` green.
12. No new shader variant appears in an export's shader key list for the fixture projects (compare key lists
    before/after on `build/fixture-unity` copies).

## 12. Files likely touched

Engine: `engine/core/subsystem/RenderSystem.{h,cpp}` (drawMesh uniform refactor, pass loops, update loop, MVP,
records), new `RenderSystemBatch.cpp`, `RenderSystemShadows.cpp` (`drawShadowCasters`), `MeshSystem.cpp`
(`prepareSpriteMesh`), new `engine/core/render/DrawBatcher.{h,cpp}`, new `engine/core/render/FrustumCull.{h,cpp}`,
`engine/core/render/RenderStats.h`, `engine/core/render/ObjectRender.{h,cpp}`, `engine/core/pool/MeshBufferPool.{h,cpp}`,
`engine/core/pool/MeshLodPool.cpp`, `engine/core/buffer/Buffer.{h,cpp}` (content hash cache), new
`engine/core/util/ContentHash.{h,cpp}`, new `engine/libs/xxhash/`, `engine/renders/sokol/SokolObject.{h,cpp}`,
`SokolCamera.cpp`, `SokolSystem.{h,cpp}`, `SokolPipelineCache.cpp`, `SokolPassRecorder.{h,cpp}`,
`engine/core/Engine.{h,cpp}`, `engine/core/player/PlayerConfig.{h,cpp}`,
`engine/core/script/binding/CoreClassesLua.cpp`.
Editor: `editor/Project.{h,cpp}`, `editor/Stream.cpp`, `editor/StreamSchema.cpp`, `editor/settings/SettingsRows.cpp`,
`editor/window/dialog/ProjectSettingsWindow.cpp`, `editor/Generator.{h,cpp}`, `editor/ExporterPrebuilt.cpp`,
`editor/window/GameWindow.cpp`, `editor/window/profiler/FrameDebuggerWindow.cpp`, `editor/mcp/tools/ProfilerTools.cpp`.
Docs/tools: `docs/rendering/batching.md` (new), `docs/mcp/guide/profiling.md`, `docs/mcp/guide/lua-api.md`,
`docs/unity-glossary.md`, `docs/mcp/TOOLS.md`, `tools/mcp/budget.json`, `NOTICE.md`, tests in section 10.

Hot spots for parallel branches: `RenderSystem.cpp` (`drawMesh`, `renderCameraPass`, `update`, `drawUI`),
`SokolObject.{h,cpp}`, `MeshBufferPool.cpp`, `Generator.cpp`, `ExporterPrebuilt.cpp`, `CoreClassesLua.cpp`,
`SettingsRows.cpp`, `GameWindow.cpp`.

## 13. Cross-feature dependencies

- **mesh-memory**: owns `MeshComponent`/`Submesh`/`SokolObject` storage (pipeline descs out of line) and the
  `HybridArray` change; must give `SpriteComponent::framesRect` and `PointsComponent::framesRect` the same lazy
  storage (6 KB per sprite today). This spec adds **no field** to `MeshComponent` or `SpriteComponent` (records live
  in RenderSystem). Both touch `SokolObject.{h,cpp}`: `BufferInfo` (here) vs pipeline desc storage (there); merge
  the later branch onto the earlier. The ceiling test (T15) uses a small pool, not 20,000 full `MeshComponent`s,
  so it does not wait for mesh-memory.
- **draw-perf**: skipping unchanged pipeline/bindings applies helps runs that share a pipeline; its opaque-queue
  sort by distance band and render state makes longer runs (this spec merges only what is consecutive or, in 2D,
  reorderable); it may adopt `FrustumCull` for all meshes (whoever lands first creates the file); its "no
  per-frame scans for features that are off" and `updateObjectLights` work make the per-mesh `update()` cost this
  spec leaves in place cheaper; `RenderBackendTime::Scope` removal covers batch draws too. Both edit
  `renderCameraPass` and `drawMesh`: land the `drawMesh` uniform refactor (6.1, decision 3) early and tell
  draw-perf.
- **render-correctness**: changes to normals, tangents, UV sets or the 2D light path in `surface_vs.glsl` /
  `lighting2d.glsl` must keep 6.5's bake in step; T2 catches drift. Its "mesh bounds from real vertices" changes
  `aabb`, which records do not use (they bound placed vertices).
- **ui-components**: edits `drawUI`, `applyPassScissor`, button transitions (Color Tint fades `ui.color` every
  frame: records compare colour each frame, so it batches). The lazy scissor (6.2) changes when scissors reach
  the GPU; its 9-patch label scissor fix must go through `CameraRender::applyScissor`, not raw sokol.
- **ssr-probes**: G-buffer batching here, probe slots there; IBL meshes never batch.
- **water-reflections / water-boats**: scene copies and water passes are foreign draws or pass ends (flush);
  their transparent-queue ordering changes do not interact beyond that.
- **export-pipeline**: no shader key changes; `Generator.cpp` and `ExporterPrebuilt.cpp` get one setting each
  (merge hotspot only).
- **platform-lifecycle**: frames updated without drawing leave chunk demand untouched (7.19).
- **runtime-core, scene-integrity**: instantiate, hidden children and scene replacement act through components
  and transforms the batch reads every frame; no coupling beyond `onComponentAdded/Removed`.
- **agent-authoring**: shares the agent guide byte budget (one line here).
- **scripting**: `CoreClassesLua.cpp` merge hotspot (one property).
- unicode-text, mobile-shipping, build-toolchain, project-window, code-editor-git: none.

## Deviations

Recorded by the implementer (2026-11-04/05).

### Hooks for pixel-art S52 (lead decision, LEDGER "Pixel-art program")

S52 lands after this branch and builds on two hooks:

1. **A sort key the batcher respects.** `BatchItem::sortKey` (`engine/core/render/DrawBatcher.h`) is
   the draw's bucket. The batcher never moves a draw past one of another bucket: in REORDER mode a
   draw may only jump back over runs whose objects are all of its bucket, and a run that spans two
   buckets is never jumped over. Every draw is in bucket 0 here. S52 puts `(Sorting Layer rank,
   Order in Layer)` in it (sort-axis distance stays a tie-break of its own sort) and submits each
   pass's draws sorted by it; batching and 2D reordering then work inside each bucket, unchanged.
   Sorting Groups are one bucket each (or a key range S52 reserves).
2. **Per-sprite parameters in vertices, not uniforms.** The bake copies every vertex attribute it
   does not know as it is (it knows POSITION, NORMAL, TANGENT, TEXCOORD1/2, COLOR). A per-sprite
   value that must not break batches (S43's palette row, S48's flash amount, dither fade and
   dissolve progress) lives in a vertex attribute of the sprite's mesh, the same at its four
   vertices, written by its owner with `needUpdateBuffer` (MeshSystem when the value changes); the
   shader variant that declares it reads it from the attribute on both paths. Changing it re-bakes
   the record; an immutable sprite buffer changing twice within 30 updates goes private
   (`refreshImmutableBuffer`), so an animated flash costs one dynamic buffer, not a buffer a frame.
   `RenderSystem::setBatchAttributeRole(type, POINT | DIRECTION)` makes the bake transform such an
   attribute like a position (an object-space point) or a normal. A value kept as a material uniform
   is part of the batch key instead (it batches with equal values only).
3. **S41's pixel snap.** Grid-aligned draws snap `gl_Position` per vertex (`pixelSnapClip`): a batched
   vertex has the same clip position as drawn alone up to float rounding, which S41's bias covers.
   Rotate On Grid snaps the pivot: S41/S52 give the sprite mesh a POINT-role attribute holding the
   object-space origin `(0,0,0)` at every vertex; drawn alone the shader's `mvp * pivot` is the model
   origin, batched it is the baked world origin through the run's `VP * T(cell)`: the same point.
   Steps90 rounds the model matrix on the CPU: the bake reads the model matrix through
   `objectMatrices()`, so S41 rounds it where `objectMatrices` / `objectModel` take it, for both paths.
4. S52's own instance format (D2), `@instanceable` and FX slots are superseded by 2 for sprites drawn
   by this batcher; S52's Sprite Batch component (bulk API) is a producer of its own.

### Changes of plan

- **Strips are not batched.** A batch draws with its first object's pipeline, whose primitive type is
  the object's: only 16-bit indexed TRIANGLES batch (`ineligible_geometry`), so TRIANGLE_STRIP
  MeshPolygons draw alone. The spec's "strips expanded to a list" would need another pipeline.
- **The SSR G-buffer pass is not batched.** Its vertex stage ignores the sprite texture rect
  (`gbuffer.vert`), so baked UVs would make the batched G-buffer differ from the unbatched one (and be
  more right). Batching it waits for that fix (a render-correctness matter); SSR with sprites is rare.
- **FLATTEN Canvas Group layers** draw their elements one by one (their own passes); the main colour
  pass batches UI.
- **Texture comparison** is by the object's own texture slots (base colour, metallic-roughness,
  occlusion, emissive, normal; depth texture; UI texture), a `texture` key section, not every view
  bound: the pass-wide ones (shadow maps, SSAO, spot masks) are bound to the run's draw and differ
  between objects that never drew alone. `sampler` is folded into `texture`.
- **Records** keep their vertices in per-record vectors, not one arena (simpler, the same lifetime
  rules); a record is re-placed when its model matrix differs from the one it was placed with
  (compared at submit, which also covers the editor's per-view matrices), not when `update()` sees
  `needUpdate`. The mesh's `worldAABB` is still computed by `update()` (930133bb5 not taken: culling
  of batched objects uses the record's placed bounds).
- **Lazy MVP** applies when the frame draws every pass with the main camera's MVP: no other camera
  (`hasMultipleCameras`) and no reflection probe; `distanceToCamera` is still computed (it sorts the
  transparent queue). Billboards and UI elements keep their MVP.
- **Stream chunks**: at most 16 MB per chunk on every platform (more chunks when a frame needs more);
  the quad index buffer is per RenderSystem (per scene), not per process.
- **STRICT mode** keeps one open run (opening another draws the one before at once).
- **MCP**: `batching` also has `single_draws` and `drawn_alone`; `batch_breaks` entries have `pass`.
  Agent guidance went into `docs/mcp/guide/profiling.md` (`jasper://docs/profiling`), with no line in
  the agent guide (lean MCP rules; agent-authoring owns the guide).
- **Tests** as section 10 with these differences: T14 (Vulkan stream refusal) and T22 (benchmark)
  are not written (the Vulkan budget path is the one every STREAM upload takes; no benchmark server
  here); T15 counts GPU buffers of 1,500 sprites against the 2 x 1,500 of main instead of shrinking
  the pool (the pool is sized once per process); T13 (chunk growth) is covered by T15's 1,500 quads,
  which need a second chunk in their first frame (`runs_skipped` 0).

### On top of S55 (Draw submission)

S55's GPU Instancing landed on main first. Agreed split (Forge ledger): dynamic batching goes first,
S55 instances the remaining mesh draws, and a draw is never both.

- **One draw path.** A batched run draws through its head object's `drawMesh` / `drawMeshDepth` /
  `drawUI` with `passBatchGeometry`; that branch draws before `offerDrawBatch`, so a run is never
  offered to an instancing batch, and an object that drew alone is offered as before.
- **One flush order.** The deferred runs and S55's held draw share the flush points: `endPass`, a
  viewport change and every applied scissor rect draw the batcher's runs, then S55's held draw. The
  foreign-draw hook moved out of `SokolPipelineCache::apply` (it drew newer runs ahead of an older
  held draw) into `SokolObject::beginDraw` and `beginAccumulationDraw` (OIT) only.
- **S55's queues feed the batcher.** The opaque queue, the depth pre-pass, the scene depth and the
  shadow casters come from S55's visible lists (`buildVisibleQueue`): `submitMesh*` take each entry,
  skip their own culling when it is `passPreCulled`, and leave to `drawMesh*` an entry without
  submesh 0 (`passSubmeshMask`), a fade-copies entry, the OIT accumulation and a submesh the OIT pass
  took. A run resets those pass flags while it draws (whichever entry flushed it). S55's material
  sort of the visible list also keeps like sprites adjacent for STRICT runs.
- **Not duplicated.** Uniform and pipeline skipping are S55's (`SokolDrawBatch::applyUniforms`,
  `applyPipeline`); the batcher adds only the lazy scissor, which S55 does not track. The
  `profiler_frame` `batching` object sits beside S55's `instanced_draws`.
