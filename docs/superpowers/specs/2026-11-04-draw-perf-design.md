# Draw performance: cheaper frames on every backend, nothing paid for what is off (design)

Date 2026-11-04. Branch `dc/draw-perf`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md` (#12).
Line numbers are of `main` at `b4f7816ea`; treat them as hints and re-read the code.

## 1. Goals

1. **No redundant GPU state.** A draw whose pipeline is already applied in the current pass does not apply
   it again; a draw whose bindings equal the applied ones does not apply them again. One tracker serves the
   whole sokol backend (the fixed-pipeline objects and the dynamic path of `SokolPipelineCache`), on all five
   backends, and is reset by every event after which sokol's state may differ from what it recorded. The gain
   is largest on D3D11 and Vulkan, where sokol re-sets every state (and Vulkan writes a descriptor set) per
   apply.
2. **Draws ordered so that the tracker pays.** Every opaque queue of a 3D pass is culled before it is sorted,
   then ordered by sort priority, distance band (one per doubling of the distance), render state, exact
   distance and submission order.
3. **Moved objects cost less.** Local matrices composed in closed form, affine products, inverses and point
   transforms, an affine AABB transform, bone bounds only for skinned meshes, and a 3x3 sign test for mirrored
   draws.
4. **Features that are off cost nothing per frame:** terrain textures (no mesh scan without terrains), mesh
   LOD polling (scene LOD off), per-object light picking (no lights), swapchain pipelines while the scene
   draws into an engine framebuffer (the editor), and backend timers and diagnostic clocks in Release games.
5. **Sun-only scenes draw a directional-only Lit variant** (`Dlo`): the point and spot paths, the spot mask
   atlas, the point shadow atlas and its uniform block compile out.
6. **No dead code:** the shadow receiver culling that nothing calls is removed with its test.
7. **The saving is visible:** Unity's "SetPass calls" in the Game view Stats, the debug overlay, Lua
   (`Profiler.frameStats`, `DebugOverlay.getStats()`), and `get_render_stats` documented as the same count.

## 2. Non-goals

- Automatic batching or instancing of meshes. Sprites are `sprite-batching`'s; meshes already have
  `InstancedMeshComponent`. Unity's dynamic batching is not reproduced.
- Changing the shadow cache, tiers or fits (S10). The receiver culling is removed, not rewired (D6).
- PCSS on GLES3/WebGL2 or painted terrain using the texture units `Dlo` frees (a follow-up, section 13).
- Signature-first component lookups (Doriax `14a30236f` part 2): rejected, D11.
- The per-frame visit of every sprite in `MeshSystem` (Doriax `14a30236f` part 3): `sprite-batching` owns
  sprite update cost (settled sprites).
- A render graph or GPU-driven culling (`docs/specs/README.md` do-not-copy list).
- Reordering 2D passes (their transform order is their layering) or the transparent queue (back to front
  stays; it is only culled earlier).

## 3. Users and stories

- A Windows (D3D11) or Android/desktop Vulkan player of a forest scene with 400 instances of four tree
  models: today 400 pipeline applies and 400 descriptor-set writes per pass; after, about one per model and
  material run, and the Vulkan descriptor budget (`SokolSystem::reserveDescriptorBytes`) no longer drops draws
  in dense scenes.
- A human in the editor turns on Stats in the Game view and reads `SetPass calls: 12` next to
  `Draw calls: 410`, as in Unity, and sees it rise when they give every tree its own material.
- A 2D game moving 2,000 sprites: each moved sprite stops comparing 128 bone matrices and composing three
  matrix products.
- A mobile game lit by one sun: its Lit shader has no point/spot branches and binds two fewer textures.
- An agent asked "why is this scene slow on Vulkan": `get_render_stats` gives `draw_calls`,
  `pipelines_applied` (Unity's SetPass calls) and `bindings_applied`; the profiling guide tells it that
  `pipelines_applied` close to `draw_calls` in a 3D view means materials or meshes are not shared.
  `get_material_variant` reports `directional_only` for the loaded variant.
- A Lua debug HUD reads `Profiler.frameStats().setPassCalls`.

## 4. Findings

| # | What is wrong or missing | Jasper evidence | Doriax |
|---|---|---|---|
| F1 | Every fixed-pipeline draw applies its pipeline and bindings unconditionally; only the dynamic path skips a repeated pipeline. sokol drops a repeated pipeline itself on GL and Metal, but re-sets all state on D3D11 and rebinds pipeline and descriptors on Vulkan. Each apply on Vulkan also takes descriptor-buffer bytes (`reserveDescriptorBytes`), the per-frame budget that drops draws when full. | `engine/renders/sokol/SokolObject.cpp:1100` `sg_apply_pipeline`, `:1128-1130` reserve and `sg_apply_bindings`; `SokolPipelineCache.cpp:661-672` `apply`/`invalidateApplied`; `engine/libs/sokol/sokol_gfx.h:12116-12135` (GL cache), `:14463-14500` (D3D11), `:16545` (Metal), `:22018-22063` (Vulkan); `engine/libs/sokol/sokol.cpp:134-170` | `d103cae0c` |
| F2 | Opaque queues are filled with every listed mesh, sorted by priority and exact distance, and culled only when drawn. Equal render states rarely sit next to each other, so even a filter would rarely skip. | `engine/core/subsystem/RenderSystem.cpp:16038-16083` (camera pass), `:2883-2908` (probe face); `RenderSystem.h:602-615` `OpaqueRenderData`, `:637-647` `OpaqueRenderComparison`; cull at `RenderSystem.cpp:8611`, `:9003` | `0982a0b0c` |
| F3 | A moved rigid mesh with no `bonesAABB` runs the "no per-bone bounds" fallback: 128 `Matrix4 !=` against identity (`bonesMatrix.size()` is the capacity), per moved mesh per update, 2D sprites included. A rigid mesh whose palette holds a non-identity matrix (a posed palette read from a scene file, `ComponentHooks.cpp:74,121`) gets a world AABB inflated by `bone * aabb`. | `RenderSystem.cpp:14940-14958`; `MeshComponent.h:423,465`; `hasSkinning` set by `loadMesh` at `:6308`, which runs earlier in the same loop (`:14919-14923`) | `859ab7d9d` |
| F4 | `AABB::transform` merges 8 corners, each through `Matrix4 * Vector3` with a divide. `computeWorldTransform` builds `T * R * S` with two full 4x4 products, multiplies by the parent with a third, transforms the origin with a divide and inverts the model matrix with the general 4x4 inverse for the normal matrix. | `engine/core/math/AABB.cpp:260-304`; `engine/core/subsystem/TransformPropagation.h:56-85` (product at `:68`); `engine/core/math/Matrix4.cpp:49-61,124-140,271` | `621be8a42` (the direct-field `Matrix4 * Vector3` is already in Jasper) |
| F5 | `isMirroredDraw` takes the full 4x4 determinant (24 four-factor terms) per draw per pass. | `RenderSystem.cpp:13963-13967`, callers `:8047,8635,9018,9854`; `Matrix4.cpp:325-339` | extra finding |
| F6 | `updateAllTerrainRenderTextures` walks every mesh each frame to find terrains. | `RenderSystem.cpp:5905-5919`, called at `:15231` | `14a30236f` |
| F7 | With scene LOD off, every loaded mesh scans its submeshes for a pending LOD each frame. | `RenderSystem.cpp:14743-14751`; `isMeshLodPending` `:8168-8173` | `b743cbef2` |
| F8 | `updateObjectLights` visits every mesh every frame (Transform lookup, `isRenderActive`) even in a scene without lights, only to zero `epoch`. | `RenderSystem.cpp:1851-1925` (the `!hasLights` test at `:1891`), called at `:15173` | extra finding |
| F9 | `getScenePipelines` adds the swapchain pipelines (`PIP_DEFAULT`, and through it `PIP_ZPREPASS`) while `Engine::getFramebuffer()` is set, when no scene pass draws to the swapchain: every mesh builds pipelines the editor never uses. | `RenderSystem.cpp:14293-14295`, `:6966`; main view target `:13795-13801`; secondary views add `PIP_RTT` (`:17776`) | `d732b914b` |
| F10 | The Lit shader reads the light type at runtime and always declares the spot mask atlas, the point shadow atlas and the point shadow block under `USE_PUNCTUAL`/`USE_SHADOWS`; the CPU binds placeholders for them. | `engine/shaderlib/includes/lights.glsl:94-104,148-192,270-300`; `engine/shaderlib/mesh.frag:433-455`; `engine/shaderlib/includes/shadow_data.glsl:54-71`; `RenderSystemShadows.cpp:996-1001`; `RenderSystem.cpp:1563-1568` | `5883187cc`, `ba7cc68a8` |
| F11 | `getShadowReceiverPlanes` has no runtime caller since merge `1c98fe779` (last caller `d61bed5a5:RenderSystem.cpp:13959-13980`); its header says so. Dead code with a test (CLAUDE.md: no dead code). Wiring it naively is unsafe: the slot key has no receiver view and the cached layers outlive the camera. | `RenderSystem.cpp:13450-13517`; `RenderSystem.h:2556-2563`; `editor/tests/engine/subsystem/ShadowReceiverCullingTest.cpp`; `RenderSystemShadows.cpp:1605-1652,1705-1722`; `engine/core/render/shadow/ShadowCache.cpp:150-260` | `56980613a` |
| F12 | `RenderBackendTime::Scope` (a thread-local read and branch) runs in every backend call of every build, Release games included, for two HTML UI perf tests. `drawParticles` reads the steady clock twice per system per view for an editor statistic; the mesh load path reads it twice per load for `JASPER_SHADER_TIMING`. | `engine/core/render/RenderBackendTime.h:31-44` (19 scopes in `renders/sokol`); `RenderSystemParticles.cpp:610,827`; `RenderSystem.cpp:14920-14932` | extra finding |

## 5. Product decisions

- **D1. One state tracker for the backend, reset by every invalidating event.** Doriax resets on pass end,
  uploads and sokol errors. Jasper also resets on pass begin, on every resource destroy (a draw equal to the
  applied bindings would otherwise use a destroyed buffer where sokol skips the draw), on context setup and
  shutdown, and when sokol marks the next draw invalid after `sg_apply_bindings` (a buffer still being
  created). Reason: fix the class (stale applied state), not the cases Doriax met. The tracker replaces
  `SokolPipelineCache`'s own `lastApplied`, so there is one notion of "applied".
- **D2. The filter sits at the apply, after every Jasper check.** `beginDraw` keeps its order: failed or
  pending pipeline, dead shader, mirrored and no-cull variants, GL warm draws (S32), `uniformsDropped`. Only
  the `sg_apply_pipeline` call is filtered. Reason: Doriax returns before its validity check; in Jasper those
  checks are per object state and must run every draw.
- **D3. Cull before sort, then band, state, distance.** Bands double with distance (`ilogb(d²)/2`), so early
  depth rejection keeps most of its value while equal states inside a band become runs. Priority stays first
  (stencil writers before readers), submission order last (a total order). Applies to every opaque queue of a
  3D pass: camera passes and reflection probe faces. The transparent queue keeps back-to-front order and is
  only culled earlier. Reason: the gain of D1 needs runs; a sort of meshes that will not draw is waste.
- **D4. Moved-object math stays bit-equal where it can.** `T * R * S`, the parent product and the origin
  transform are computed in closed or affine form with the same non-zero products in the same order, so they
  equal today's results (zeros' sign aside). The AABB interval method and the affine inverse round
  differently; goldens must pass unchanged at today's tolerances. Doriax's identity shortcut is subsumed by
  the closed form (no branch, every transform benefits).
- **D5. `Dlo` ships, per scene, decided by light components.** A scene whose Light components are all
  Directional (enabled or not, active or not) loads the built-in Lit with `Dlo`. A disabled point light keeps
  the general variant, so toggling a flashlight never swaps variants. Only the engine's own Lit (empty shader,
  `builtin:lit` references) gets it, never forks or library shaders. Exports ship each `Dlo` key with its
  general twin (a script may add a point light); a build missing `Dlo` falls back to the general variant.
  Reason: mobile and web first; the cost is shipped variants of sun-only scenes, which the fallback and the
  twin rule keep safe.
- **D6. The receiver culling is deleted, not rewired.** It is safe only on paths redrawn every frame
  (`EVERY_FRAME` lights, camera casters), which are opt-in and rare; the frame atlas has several readers in a
  frame (reflection probe faces, render-to-texture cameras without their own lighting, editor secondary
  views, and `water-reflections`' planar reflection), each a hole risk; the static cache already cut the
  cost it targeted (High tier 7.2 to under 1 ms, `ShadowTiers.cpp:14-24`). The commit message names the
  commit that last called it (`d61bed5a5`) so it can be revived with a design for those readers.
- **D7. Bone bounds only for skinned meshes.** Gate on any submesh's `hasSkinning`. Reason: rigid meshes
  have no bones bound; this also fixes F3's inflated bounds.
- **D8. Swapchain pipelines only when something can draw to the swapchain.** `PIP_DEFAULT`/`_NODEPTH` only
  while `Engine::getFramebuffer()` is null. The existing `loadedPipelines` check reloads when a game sets or
  clears the framebuffer at runtime. Reason: the editor builds a pipeline per mesh variant it never applies.
- **D9. Backend timing exists only in test builds.** A compile definition `JASPERENGINE_BACKEND_TIME`, on
  with `JASPERENGINE_TESTS` or `JASPERENGINE_HTMLUI_TESTS`, makes `RenderBackendTime::Scope` real; elsewhere
  it is an empty type. Not tied to `JASPERENGINE_PROFILER`: the HTML UI test build is a Release engine without
  the profiler and its perf tests need the timer. The particle render clock and `JASPER_SHADER_TIMING` compile
  only with `JASPERENGINE_PROFILER` (their consumers are the editor and the profiler).
- **D10. Unity's name for the saving.** `SetPass calls` (pipelines actually applied) in the Game view Stats,
  the debug overlay, `Profiler.frameStats().setPassCalls`, `DebugOverlay.getStats().setPassCalls`. MCP keeps
  `pipelines_applied` (already there, per view) and documents it as SetPass calls. No new MCP tool.
- **D11. Rejected: signature-first lookups.** Jasper's `findComponent` is an O(1) paged read
  (`ecs/PagedStorage.h:271-277`); a signature test plus `getComponent` costs about the same
  (`ecs/ComponentManager.h:90-95,143-145`). The camera pass only moves its terrain and tilemap lookups after
  the cull (they are then skipped for culled meshes).
- **D12. No new editor window or Inspector field.** These are engine internals; the Stats overlay, the
  Profiler and the Frame Debugger (which already count applies per pass) show their effect. No Lua API beyond
  the two stats fields: nothing here is behaviour a script drives.

## 6. Design

### 6.1 The state tracker (`engine/renders/sokol`)

`SokolSystem` (`SokolSystem.h/.cpp`) owns the applied state of the render thread:

```cpp
// The pipeline and bindings the current pass applied last (render thread). A draw with the same
// skips the sokol call; every event after which sokol's state may differ forgets them.
static bool applyPipeline(sg_pipeline pipeline);        // true when sg_apply_pipeline ran
static bool isBindingsApplied(const sg_bindings& bind); // equal to the applied bindings
static void applyBindings(const sg_bindings& bind);
static void forgetAppliedState();
```

State in the file's anonymous namespace: `uint32_t appliedPipeline` (`SG_INVALID_ID` when none),
`sg_bindings appliedBindings`, `bool bindingsApplied`.

- `applyPipeline`: equal id returns false. Otherwise it sets `appliedPipeline`, clears `bindingsApplied`
  (sokol requires bindings after every `sg_apply_pipeline`, and on Vulkan a new pipeline may disturb bound
  sets), then calls `sg_apply_pipeline` (state set before the call: an error inside forgets it from the
  logger) and `Engine::addSetPassCall()`.
- `isBindingsApplied`: `bindingsApplied && std::memcmp(&appliedBindings, &bind, sizeof(sg_bindings)) == 0`
  (336 bytes with this sokol). Unused slots holding stale ids only make a comparison unequal, never wrong.
- `applyBindings`: copies, sets `bindingsApplied`, calls `sg_apply_bindings`, then
  `if (!jasperengine_sg_next_draw_valid()) forgetAppliedState();` — sokol keeps skipping draws after a
  binding that is not ready until the next `sg_apply_pipeline`; forgetting makes the next draw apply the
  pipeline again, as every draw does today.
- `engine/libs/sokol/sokol.cpp` gains `extern "C" bool jasperengine_sg_next_draw_valid()` returning
  `_sg.valid && _sg.cur_pass.valid && _sg.next_draw_valid`, and `jasperengine_sg_vk_next_draw_descriptors`
  gains a `bool bindings` parameter: the view/sampler set counts only when bindings will be applied (the
  uniform set as today, when `_sg.vk.uniforms.dirty`). `sokol_gfx.h` is not changed (no PATCHES.md entry).
- `SokolSystem::reserveDescriptorBytes(bool bindings)` passes it through.

`forgetAppliedState()` is called from:

- `SokolSystem::beginPass` and `endPass` (`SokolSystem.cpp:430-458`), replacing
  `SokolPipelineCache::invalidateApplied()`;
- `SokolSystem::updateBuffer`, `appendBuffer`, `updateImage` (`:544-575`): sokol rotates the backend buffers of
  dynamic and stream resources on update (GL, D3D11, Metal), so the same handle names another backend object;
  the command queue's update commands go through these (`SokolCmdQueue.cpp:123-126`);
- every resource destroy and uninit in `renders/sokol`: `SokolBuffer.cpp:139-150`, `SokolTexture.cpp:73,962-981`,
  `SokolFramebuffer.cpp` (8 call sites), `SokolCamera.cpp` (1), `SokolShader.cpp` (1), `SokolPipelineCache.cpp` (1),
  and the destroy/uninit commands of `SokolCmdQueue.cpp:104-120,169-184` (a single helper in each file, so a new
  destroy site cannot forget it);
- `sokol_log` at level 0 or 1 on the render thread (`SokolSystem.cpp:205-258`, `!Engine::isAsyncThread()`);
- `SokolSystem::setup` and `shutdown` (a new context).

Users:

- `SokolObject::beginDraw(PipelineType, bool, bool)` (`SokolObject.cpp:1019-1104`): every check stays as it is
  and in the same order; lines `1100-1102` become `SokolSystem::applyPipeline(selectedPipeline); return true;`.
- `SokolObject::beginDraw(const PipelineState&)` (`:982-1017`): `SokolPipelineCache::apply(pipeline)` delegates
  to `SokolSystem::applyPipeline` and counts `applyCount` only when it applied (`pipeline_cache.applies` of
  `get_render_stats` keeps its meaning). `lastApplied` and `invalidateApplied` are removed from
  `SokolPipelineCache.{h,cpp}`.
- `SokolObject::draw` (`:1119-1133`):

```cpp
if (uniformsDropped){ ... unchanged ... }
const bool rebind = !SokolSystem::isBindingsApplied(bind);
// past the frame's descriptor buffer (Vulkan): skipped, as before; only what this draw writes counts
if (!SokolSystem::reserveDescriptorBytes(rebind))
    return false;
if (rebind)
    SokolSystem::applyBindings(bind);
sg_draw(baseElement, vertexCount, instanceCount);
return true;
```

A skipped draw (dropped uniforms, descriptor budget) changes nothing in the tracker. The tracker is touched
only on the render thread (debug assert `!Engine::isAsyncThread()` in the four functions).

`SokolPassRecorder` counts applies from sokol's trace hooks: its per-pass `pipelines`/`bindings` now count real
applies; its `currentPipeline` stays right (a skipped apply is the same pipeline).

### 6.2 Opaque queues (`engine/core/subsystem/RenderSystem.{h,cpp}`, `engine/core/render/ObjectRender.{h,cpp}`)

- `OpaqueRenderData` (`RenderSystem.h:602-615`) gains `uint8_t band` and `uint64_t stateKey`.
- `static uint8_t RenderSystem::distanceBand(float squaredDistance)`:
  `squaredDistance > 1.0f ? (uint8_t)std::min(std::ilogb(std::min(squaredDistance, FLT_MAX)) / 2, 63) : 0`
  (NaN and values up to 1 m² are band 0; infinity is band 63). One band per doubling of the distance.
- `ObjectRender::getSortKey(PipelineType) const` and `SokolObject::getSortKey`:
  `((uint64_t)getPipelineId(pipType) << 32) | mix32(bind.vertex_buffers[0].id, bind.index_buffer.id,
  bind.views[0].id, bind.views[1].id)` (a fixed integer mix, e.g. multiply-xorshift). Equal bindings give equal
  keys, which is all the sort needs; a collision only interleaves two runs.
- The key of a mesh is that of its first submesh drawn in that queue (`alphaBlend` false for the opaque
  queue), for the pass's colour pipeline type (`colorPip`, `PIP_RTT_INVERT` for probe faces); 0 when not
  loaded.
- `OpaqueRenderComparison` (`RenderSystem.h:637-647`): `sortPriority`, then `band`, then `stateKey`, then
  `distanceToCamera`, then `order`.
- Pre-cull: in the camera pass list loop (`RenderSystem.cpp:16040-16076`) one
  `isBatchVisible(entity, mesh, transform, instmesh, instanceView, camera.farClip, camera.frustumPlanes)` is
  taken after the visibility/layer tests; a mesh that fails it enters neither the opaque nor the transparent
  queue. The `TerrainComponent`/`TilemapComponent` lookups move after it. The probe face loop
  (`:2888-2904`) does the same with the capture camera and `instanceView = -1`.
- `bool passQueueCulled` (beside `passSubmeshQueue`, `RenderSystem.h:1772`), set by a small RAII scope around
  the loops that draw a pre-culled queue (the depth pre-pass `:16088-16099`, the opaque loop `:16141-16146`,
  the transparent queue's mesh entries, the probe face loop `:2906-2908`); `drawMesh` (`:8611`) and
  `drawMeshDepth` (`:9003`) skip their `isBatchVisible` while it is set. The pre-cull uses exactly the
  arguments the draw would.
- `size_t RenderSystem::getOpaqueQueueSize() const` (diagnostics and tests: the opaque queue of the last 3D
  pass).
- 2D passes (`distanceSort` false, no depth pre-pass) are unchanged.

### 6.3 Moved-object math (`engine/core/math`, `TransformPropagation.h`, `RenderSystem::update`)

`Matrix4` (`Matrix4.{h,cpp}`) gains:

- `static Matrix4 composeTRS(const Vector3& t, const Quaternion& r, const Vector3& s)`: the rotation matrix's
  three columns scaled by `s.x`, `s.y`, `s.z`, translation `t`, last row `0 0 0 1` (equal to
  `translateMatrix(t) * r.getRotationMatrix() * scaleMatrix(s)`: the dropped terms are products with exact
  zeros and ones);
- `bool isAffine() const` (row 3 is `0 0 0 1`);
- `Matrix4 affineMultiply(const Matrix4& rhs) const`: both affine; the same non-zero products in the order of
  `operator*`, row 3 set to `0 0 0 1`;
- `Vector3 transformAffine(const Vector3& v) const`: `operator*` without the `w` divide (`w` is 1);
- `Matrix4 affineInverse() const`: the 3x3 inverse by cofactors and `-A⁻¹t`; when the 3x3 determinant is 0
  or not finite it returns `inverse()` (today's result for degenerate scales);
- `float determinant3x3() const` (upper-left 3x3).

`computeWorldTransform` (`TransformPropagation.h:56-85`) uses `composeTRS`, `affineMultiply`,
`transformAffine`, and `affineInverse().transpose()` for the normal matrix. Transform matrices are affine by
construction (debug assert on the parent's `isAffine()`).

`AABB::transform` (`AABB.cpp:260-304`): when `matrix.isAffine()`, Arvo's interval method (translation plus,
per output axis, the sum over input axes of `min`/`max` of `m * oldMin` and `m * oldMax`); if a result
component is NaN, the corner loop runs instead (today's result for broken matrices). Projective matrices keep
the corner loop.

`RenderSystem::update` (`RenderSystem.cpp:14940-14958`): the bone block runs only when
`!instmesh && meshHasSkinning(mesh)` (a file-local helper: any submesh's `hasSkinning`; `loadMesh` set it
earlier in the same iteration). If `mesh-memory` has moved the palette into a `SkinComponent` by then, the gate
is the presence of the palette, with the same meaning.

`RenderSystem::isMirroredDraw` (`:13963-13967`) uses `determinant3x3()` (same sign for affine matrices; model
matrices and the editor's override matrices are affine).

### 6.4 Features that are off

- `updateAllTerrainRenderTextures` (`RenderSystem.cpp:5905-5919`) iterates the `TerrainComponent` array and
  finds each terrain's `MeshComponent` (same `loaded && !needReload` test).
- LOD polling (`:14745`): `else if (lastMeshLodEnabled && mesh.loaded && !mesh.needReload)`; the
  `meshLodToggled` branch is unchanged.
- `updateObjectLights` (`:1851`): while `!hasLights`, the first frame zeroes every mesh's `lights.epoch` and
  sets `objectLightsCleared` (new member, reset in `load()` and on any frame with lights); later frames return at
  once. Lights coming back restructure the light list (`lightListEpoch++`), so every mesh picks again, as today.
- `getScenePipelines` (`:14293-14295`): `PIP_DEFAULT`/`PIP_DEFAULT_NODEPTH` only when
  `!Engine::getFramebuffer()` (D8). Fullscreen passes that load both targets themselves (`uberRender`,
  `compositeRender`, `blitRender`) are unchanged.

### 6.5 The directional-only variant `Dlo`

Engine:

- `MeshShaderEnvironment` (`RenderSystem.h:380`) gains `bool directionalLightsOnly = false`.
- `checkLightsAndShadow` (`RenderSystem.cpp:1290-1328`) computes `directionalLightsOnly = hasLights` and every
  `LightComponent.type == LightType::DIRECTIONAL` (all components, in the loop it already runs).
  `getMeshShaderEnvironment` (`:3115`) copies it. In `update`, beside the light capacity tier (`:14573-14577`), a
  change of it calls `needReloadMeshVariants()` (the loaded variant draws until the new one's shaders are built).
- `SubmeshShaderInputs` gains `bool builtinLit`: the submesh's shader is empty or `builtin:lit` (base
  `ShaderReference::BUILTIN_LIT`) and it has no manifest; set where `stochasticShader` is set.
- `getSubmeshShaderVariant` (`:3715-3900`): after the properties are built,
  `if (environment.directionalLightsOnly && inputs.builtinLit && (p_punctual || p_vertexLights))
  variant.properties |= ShaderPool::getDirectionalOnlyProperty(ShaderType::MESH);`.
- `ShaderPool` (`ShaderPool.cpp`): MESH bit 36 named `Dlo` / "Directional Lights Only" in
  `getShaderPropertyName`; `getShaderPropertyCount(MESH)` 36 → 37 (`:652`); `getDirectionalOnlyProperty`; the
  mesh fallback order (`:1129`) starts with 36 (dropping it keeps every block the draw uploads: the general
  variant only adds resources bound by reflection).
- `ShaderVariantCompiler.cpp:370-403`: bit 36 → `DIRECTIONAL_LIGHTS_ONLY`.
- `orderObjectLights` (`RenderSystem.cpp:2050`) leaves out non-directional lights (`position_type.w != 0`) when
  the properties carry `Dlo`: in the frames between a point light's arrival and the general variant's build it
  is not drawn rather than shaded as a sun. The cached light bits become `uint64_t` and include bit 36
  (`builtLightBits` at `:8027,8072,8659,8831`, today a `uint32_t` cast that would drop bit 36).

Shaders (`engine/shaderlib`):

- `includes/lights.glsl`: `getLightData` sets `type = LightType_Directional` under `DIRECTIONAL_LIGHTS_ONLY`;
  `LIGHTS_SPOT_MASKS` and `SHADOW_DATA_POINT` are defined only without it (no `u_spotMaskAtlas`,
  `u_fs_point_shadow_data`, `u_shadowPointAtlas`); `lightShadow` calls only `shadowDirectional`; the attenuation
  and cone functions fold.
- `mesh.frag:433-455` and the shadow includes (`includes/shadows.glsl`, `includes/shadow_sampling.glsl`,
  `includes/shadow_data.glsl`): every point and spot reference is guarded so the `Dlo` program compiles without
  them. The implementer finds every reference with the preprocessed `Dlo` source of each backend.
- Every variant without `Dlo` preprocesses to exactly today's GLSL (`ShaderZeroCost.DefaultVariantsUnchanged`
  passes with its hash list untouched). The shader cache path (`editor/App.cpp`, `v44`) is not bumped: existing
  keys compile to the same programs and `Dlo` keys are new.

Editor and export (`editor/shader/ShaderVariantCollector.cpp`):

- New fact `FACT_PUNCTUAL_LIGHTS` (a Point or Spot light in the content or a prefab; `:66-81`, collected beside
  `FACT_LIGHTS` at `:121`). `toEnvironment` sets `directionalLightsOnly = environment.lights &&
  !(facts & FACT_PUNCTUAL_LIGHTS)`; `environmentBits` gains bit 26 for it.
- The collector's resulting key set (what the exporter, `generate_shaders` and `get_build_info include_shaders`
  read) adds, for every MESH key with `Dlo`, the same key without it.

MCP: `get_material_variant` (`editor/mcp/tools/MaterialsLightingTools.cpp:51-124`) adds
`"directional_only": bool` (the loaded `shaderProperties` carry bit 36) and " (directional lights only)" to its
text when true. Output only; the schema and tool budget do not change.

### 6.6 Removal of the receiver culling

Delete `RenderSystem::getShadowReceiverPlanes` (`RenderSystem.cpp:13450-13517`, the declaration and comment at
`RenderSystem.h:2556-2563`) and `editor/tests/engine/subsystem/ShadowReceiverCullingTest.cpp` (globbed: no CMake
change). The commit message names `d61bed5a5` and D6's reasons.

### 6.7 Timers out of Release games

- `engine/CMakeLists.txt`: `JASPERENGINE_BACKEND_TIME=$<OR:$<BOOL:${JASPERENGINE_TESTS}>,$<BOOL:${JASPERENGINE_HTMLUI_TESTS}>>`
  as a PUBLIC definition of `jasperengine` (beside `JASPERENGINE_PROFILER`, `:2047-2058`), so every user of the
  header sees the same class.
- `RenderBackendTime.h`: with `JASPERENGINE_BACKEND_TIME` the class as today; without it `setEnabled`,
  `reset` do nothing, `isEnabled` is false, `totalMs` 0, `scopeCount` 0, and `Scope` is an empty class with
  trivial constructor and destructor; `static_assert(std::is_empty_v<RenderBackendTime::Scope> ==
  !JASPERENGINE_BACKEND_TIME)` in the header. The Xcode workspace defines nothing, so its games get the empty
  class.
- `engine/tests/htmlui/RenderPerfTest.cpp` and `engine/tests/htmlui/effects/EffectPerfTest.cpp`:
  `static_assert(JASPERENGINE_BACKEND_TIME, "...")`, so a build that lost the timer fails instead of measuring 0.
- `RenderSystemParticles.cpp:610,827` and `RenderSystem.cpp:14920-14932`: inside `#if JASPERENGINE_PROFILER`
  (`render_us` of the particle MCP stats stays measured in the editor).

### 6.8 SetPass calls (Engine, editor, Lua, docs)

- `Engine::FrameStats` (`Engine.h:226-230`) gains `uint32_t setPassCalls`; `Engine::addSetPassCall()` beside
  `addDrawStats` (`Engine.cpp:929-933`), called by `SokolSystem::applyPipeline`.
- Game view Stats (`editor/window/GameWindow.cpp:1230`): `Draw calls: %u   SetPass calls: %u   Tris: %llu`.
- `DebugStatsSnapshot` (`engine/core/debug/DebugStats.h:26`) gains `setPassCalls` (filled in
  `DebugStats.cpp:76`), shown as "SetPass calls" by the debug overlay (`DebugOverlay.cpp:879`).
- Lua: `Profiler.frameStats()` (`ProfilerClassesLua.cpp:96-101`) and `DebugOverlay.getStats()`
  (`CoreClassesLua.cpp:1277`) add `setPassCalls`; stubs `stubs/Profiler.lua:7` and `stubs/DebugOverlay.lua:25`.

### 6.9 Docs

- `docs/profiling.md` (In the editor) and `docs/mcp/guide/profiling.md` (Caveats): SetPass calls =
  `pipelines_applied`; what makes runs (shared material and mesh, distance bands); `bindings_applied` well
  below `draw_calls` needs shared meshes (Models of one file) and textures.
- `docs/mcp/guide/shaders.md` (variant bits): `Dlo`, when it is used, the twin rule, `directional_only`.
- `docs/rendering/shadows.md`: one line that receiver culling does not exist (the static cache covers it).
- New `docs/perf/draw-perf.md` (as `docs/perf/particles.md`): the bench lines and the before/after counts of
  section 10, with the machine and backend.

### 6.10 Platforms and backends

No platform file changes. All changes are in shared C++ and GLSL, built for GL 3.3, GLES3/WebGL2, Metal, D3D11
and Vulkan. sokol's per-backend behaviour above (what an apply does, slot rotation on update, the Vulkan
descriptor write) was read from `sokol_gfx.h`; D3D11 and Metal cannot run here, so their correctness rests on the
GL and Vulkan GPU tests plus that reading; macOS runs the GPU suites on Metal in its CI lane. Windows/macOS/iOS
builds are kept compiling by reading (no new includes beyond `<cstring>`, `<cmath>`, `<cfloat>`).

## 7. Behaviour rules and edge cases

- The tracker never makes a draw happen that sokol would skip: a not-ready binding, a destroyed resource, a
  failed apply or a new pass all lead to a real apply on the next draw.
- Uniforms are applied every draw as today; skipping a pipeline never skips a uniform block (sokol's
  "required bindings and uniforms" flags accumulate until the next `sg_apply_pipeline`).
- Mirrored and no-cull variants are other pipelines: switching to them applies (and rebinds).
- The HTML UI (dynamic path) and the Frame Debugger's preview blit share the tracker; the blit is its own pass.
- Sort: equal priority, band, key and distance keep submission order. A mesh with a NaN or null world AABB is
  distance 0, band 0. Exactly coplanar opaque surfaces in one band may draw in another order than before (both
  are depth-tested, so only exact depth ties can show it); `opaqueSortPriority` stays the tool for a fixed order.
- A culled mesh is not in the opaque or transparent queue; its Trail Renderer is unaffected (trails are culled
  on their own bounds). Editor billboard overrides and per-view instance bounds cull as `isBatchVisible` does.
- `Dlo`: a scene with lighting forced on and no light gets `Dlo` (nothing to shade either way). The first
  point or spot Light the scene has (enabled or not) switches it to the general variant until the scene loads
  again: removing, retyping or adding lights afterwards reloads nothing. A mesh still drawing `Dlo` while its
  general variant builds takes the new light in its SH ambient: drawn, never shaded as a sun nor dropped.
  Several scene layers decide each for its own lights. Vertex-lit and terrain meshes of the built-in Lit get
  it; unlit submeshes, forks, library shaders and Light Mode Tiled variants never do.
- `Dlo` key missing from a build: the general variant draws (warned once, existing fallback). General key
  missing while a `Dlo` scene gains a point light: impossible after the twin rule, which ships the general
  twin and, unless Light Mode is Per Object, its tiled (`Til`) form (a point light under Automatic or Tiled
  draws the pixel lights from tiles).
- Bone bounds: a skinned mesh keeps today's behaviour; a Vertex Animator's `animatedBounds` is unchanged.
- Zero or negative scales: `composeTRS` and `affineMultiply` give today's values; `affineInverse` falls back to
  `inverse()` on a zero determinant; `isMirroredDraw` gives today's answer.
- Swapchain pipelines: a game that sets `Engine::setFramebuffer` at runtime reloads its meshes once (one hitch,
  as switching fixed resolution does today).

## 8. Performance and cost

| Change | Cost when it does nothing | Expected gain (measured in `docs/perf/draw-perf.md`) |
|---|---|---|
| Tracker | per draw: one 32-bit compare and a 336-byte `memcmp`; a 336-byte copy when bindings change | D3D11/Vulkan: one apply per run instead of per draw; Vulkan: one view/sampler descriptor set per run |
| Pre-cull + sort | none (the cull moved from `drawMesh`; sorting fewer, smaller-keyed elements) | runs of equal state inside each distance band |
| `composeTRS`, affine product/inverse | none | per moved Transform: 2 of 3 matrix products and the general inverse gone |
| Affine AABB | 4 compares for the affine test | about 3x fewer operations than 8 corners with divides |
| Bone gate | a loop over `numSubmeshes` bools per moved mesh | 128 matrix compares per moved rigid mesh |
| Mirror sign | none | 9 multiplies instead of 96 per draw per pass |
| Terrain, LOD, object lights | none | a mesh-array walk per frame each, when off |
| Swapchain pipelines | none | editor: one pipeline per mesh variant (and its pre-pass) not built |
| `Dlo` | off: none (a flag computed in a loop that already runs) | sun-only scenes: shorter shader, 2 fewer texture binds, no point block |
| Timers | Release: zero (empty type) | one thread-local read and branch per backend call |

Budgets enforced by `DrawPerfBench` under `JASPER_TEST_PERF_SLACK` (printed otherwise): `composeTRS` at
least 2x faster than the three-product form; the affine `AABB::transform` at least 2x faster than the corner
loop; `determinant3x3` at least 4x faster than `determinant`. Draw-count targets are acceptance criteria
(AC4, AC6).

## 9. Compatibility

- No scene, prefab, project or asset format changes; no version bump, no migration.
- Shader keys: bit 36 appears only on new keys. Projects' saved key lists may contain `Dlo` keys after this
  change; older editors are not a target. The shader cache keeps `v44` (non-`Dlo` programs are unchanged).
- Exported games built before are unaffected; exports after ship `Dlo` keys with their twins.
- Lua: two new fields in existing tables; nothing removed.
- Visual: goldens must pass unchanged (AABB and normal-matrix rounding and the new draw order are below their
  tolerances); if one does not, the implementer looks at it and reports why before any re-baseline.

## 10. Tests

Red controls: a test marked **red** fails on `main` (run it there first and keep the output). Tests marked
**guard** pass on `main`; their red control is the named mutation of the new code, applied locally and reverted.

Core CPU (`jasperengine-core-tests`):

- `engine/tests/core/math/Matrix4Test.cpp`: `Matrix4.ComposeTRSMatchesTheProduct` (random t, r, s incl. negative
  and zero scales; `==` per element) **guard** (swap two columns in `composeTRS`);
  `Matrix4.AffineMultiplyMatchesTheProduct` (within 2 ulp) **guard**; `Matrix4.AffineInverseMatchesInverse`
  (relative 1e-5; zero scale gives `inverse()`'s result) **guard**; `Matrix4.Determinant3x3HasTheSignOfTheDeterminant`
  **guard**.
- `engine/tests/core/math/AABBTest.cpp`: `AABB.AffineTransformMatchesTheCorners` (random affine matrices; equal
  within 1e-5 relative, contains every transformed corner within that) **guard** (drop the `min` in one axis);
  `AABB.ProjectiveTransformKeepsTheCornerPath`; `AABB.NonFiniteMatrixGivesTodaysResult` **guard**.
- `engine/tests/core/subsystem/TransformPropagationTest.cpp` (new): `TransformPropagation.WorldValuesMatchTheGeneralProducts`
  (a three-level hierarchy with rotations, a negative scale and a UI pivot; model, world position and normal
  matrices against a reference computed in the test with `operator*` and `inverse()`) **guard**.
- `engine/tests/core/render/RenderQueueOrderTest.cpp` (new): `RenderQueueOrder.DistanceBandsDouble` (0, 1, 3.9,
  4, 16, 1e30, inf, NaN), `RenderQueueOrder.PriorityThenBandThenStateThenDistance`,
  `RenderQueueOrder.EqualKeysKeepSubmissionOrder` **red** (fields absent on `main`: a compile-time red, noted as
  such).
- `engine/tests/core/shader/ShaderVariantCollectionTest.cpp`: `ShaderKey.DirectionalOnlyIsMeshBit36` (name `Dlo`,
  count 37) **red**; `ShaderKey.FallbackDropsDirectionalOnlyFirst` **red**.
- `engine/tests/core/shader/DirectionalOnlyShaderTest.cpp` (new): `DirectionalOnlyShader.LeavesOutPointAndSpotResources`
  (preprocess the `Dlo` twin of every lit variant `ShaderZeroCost` enumerates, GLCore and GLSL300ES: no
  `u_spotMaskAtlas`, `u_shadowPointAtlas`, `u_fs_point_shadow_data`, no point/spot branch; compiled where
  `ShaderVariantCompiler::isAvailable()`, its reflection has two fewer images than the general variant) **red**.
- `ShaderZeroCost.DefaultVariantsUnchanged` passes with `shader_zero_cost` hashes untouched (acceptance, not a
  new test).
- `engine/tests/core/bench/DrawPerfBench.cpp` (new): `DrawPerfBench.ComposeTRS`, `DrawPerfBench.AffineAABB`,
  `DrawPerfBench.MirrorSign`: `[bench]` lines and the section 8 budgets under `perfSlack()`.

Core GPU (`DrawPerfGpu`, `engine/tests/core/render/DrawPerfGpuTest.cpp`, glcore and Vulkan/lavapipe on Linux,
Metal on macOS; `JASPER_REQUIRE_GPU()`; counts read from `SystemRender::getRenderStats` of the frame):

- `DrawPerfGpu.RepeatedDrawsApplyStateOnce`: 50 objects sharing one vertex buffer and pipeline in one pass:
  1 pipeline and 1 bindings apply; the read-back image shows all 50 **red** (50/50 on `main`).
- `DrawPerfGpu.NewPassAppliesAgain`: the same in two passes: 1 apply each **guard** (remove the reset in
  `beginPass`).
- `DrawPerfGpu.BufferUpdateBetweenDrawsRebinds`: two draws of one stream buffer with an update between: the
  second shows the new data **guard** (remove the reset in `updateBuffer`).
- `DrawPerfGpu.BindingsNotReadyDoNotStopLaterDraws`: a draw whose buffer is still `ALLOC` (made through the
  command queue, not executed), then two ready draws with the same pipeline: both visible **guard** (remove the
  `jasperengine_sg_next_draw_valid` check).
- `DrawPerfGpu.DestroyedResourceIsNotDrawn`: draw A, destroy its texture, draw B with the same bindings: B is not
  drawn, no validation error **guard** (remove the reset in the destroy helper).
- `DrawPerfGpu.VulkanDescriptorsCountOnlyAppliedBindings`: on Vulkan, 50 equal draws take one view/sampler set
  and 50 uniform sets of descriptor bytes (`SokolSystem` budget counters); on GL the counters are 0 and the test
  checks that **red** on Vulkan.
- `DrawPerfGpu.OpaqueQueueHoldsOnlyVisibleMeshes`: 100 meshes, 90 behind the camera: `getOpaqueQueueSize() == 10`
  **red** (100 on `main`; the getter is new, so `main` is checked by temporarily adding it there).
- `DrawPerfGpu.SortGroupsStatesInABand`: 20 boxes alternating a lit and an unlit material inside one band: the
  colour pass applies 2 mesh pipelines (plus the pass's fixed ones, counted with 0 boxes) **red** (about 20).
- `DrawPerfGpu.RigidMeshIgnoresLeftoverBonePalette`: a rigid mesh with `bonesMatrix[3]` a scale of 5, moved: its
  `worldAABB` equals `modelMatrix * aabb` **red** (inflated on `main`).
- `DrawPerfGpu.EditorFramebufferSkipsSwapchainPipelines`: with `Engine::setFramebuffer(fb)` a mesh's
  `getPipelineId(PIP_DEFAULT)` is 0 and `PIP_RTT`'s is not; after `setFramebuffer(nullptr)` and an update both
  exist **red**.
- `DrawPerfGpu.SunOnlySceneLoadsDirectionalOnly`: one directional light: the lit submesh carries bit 36; a
  disabled point light added: after the reload it does not; removed: it does again **red**.
- `DrawPerfGpu.DirectionalOnlyDrawsTheSameImage`: the `render_lit_cube` setup drawn with `Dlo` and with the
  general variant (a disabled point light) gives images within the strict reference tolerance **red** (no `Dlo`).
- `DrawPerfGpu.ArrivingPointLightIsLeftOutOfDirectionalOnlyDraws`: with a point light picked by a mesh still on
  `Dlo`, `orderObjectLights` lists only the directional lights **red**.
- `DrawPerfGpu.StaticSceneUpdateCost`: `[bench]` of `RenderSystem::update` + `draw` for 2,000 static meshes with
  lights, LOD and terrain off (printed only; before/after recorded in `docs/perf/draw-perf.md`).
- Existing goldens on glcore and Vulkan pass unchanged: `RenderGpu.*`, `Atmosphere*Gpu.*`, `ParticlesGpu.*`,
  `SplineGpu.*`, `Water*Gpu.*` (`render_lit_cube` now draws `Dlo`).

Editor (`jasperengine-editor-tests`):

- `editor/tests/shader/DirectionalOnlyVariantsTest.cpp` (new, fixture as `VatShaderVariantsTest`):
  `DirectionalOnlyVariants.SunOnlySceneShipsBothVariants` **red**, `.PointLightSceneShipsGeneralOnly`,
  `.PrefabWithPointLightReachesTheGeneralVariant`, `.ForksAndLibraryShadersNeverGetIt`.
- `ShadowReceiverCulling.*` deleted with the function (not disabled).

MCP (`tools/mcp/tests/test_draw_perf.py`, `CI = True`):

- `test_setpass_calls_drop`: 30 boxes with one material and 30 Models of one fixture glTF; the Game view's
  `get_render_stats`: `pipelines_applied * 4 < draw_calls` and `bindings_applied < draw_calls` **red** (about
  equal on `main`).
- `test_material_variant_reports_directional_only`: a sun: `directional_only` true; a point light added and
  `wait` until the variant is ready: false **red** (field absent).
- `test_frame_stats_setpass_in_lua`: in Play, a Lua snippet reads `Profiler.frameStats().setPassCalls` and
  `DebugOverlay.getStats().setPassCalls` (both > 0 and ≤ `drawCalls`) **red**.
- Existing `test_profiler.py`, `test_async_shaders.py`, shadow, terrain and HTML UI tests pass unchanged.

Other gates: `tools/check-changed` per commit; `tools/smoke-test.sh` (export, build, run) once; the HTML UI suite
(`tools/build.sh htmlui-tests`, both backends) for the dynamic path and the timer; the web gate
(`tools/build.sh web-gate`) once for WebGL2.

## 11. Acceptance criteria

1. Every test of section 10 passes on glcore and Vulkan; each **red** test was run on `main` (or with the named
   temporary getter) and failed; each **guard** test failed under its mutation. Outputs kept outside the repo.
2. `SokolPipelineCache` has no `lastApplied`/`invalidateApplied`; `sg_apply_pipeline` and `sg_apply_bindings`
   are called in `renders/sokol` only from `SokolSystem::applyPipeline`/`applyBindings`
   (`git grep` shows it).
3. Every resource destroy/uninit site in `renders/sokol` goes through the helper that forgets the applied state
   (`git grep sg_destroy_ sg_uninit_` lists only helper bodies).
4. In `test_setpass_calls_drop`, the Game view's `pipelines_applied` is under a quarter of `draw_calls` and
   `bindings_applied` is under `draw_calls`; the numbers before and after are in `docs/perf/draw-perf.md`.
5. The Game view Stats show `SetPass calls`; a screenshot from an isolated editor is attached to the report.
6. `DrawPerfGpu.SortGroupsStatesInABand` and `OpaqueQueueHoldsOnlyVisibleMeshes` pass.
7. `DrawPerfBench` budgets hold with `JASPER_TEST_PERF_SLACK=1` on a quiet server (`RB_SLOTS=2`).
8. A Release engine build (`engine-linux-release`) compiles `RenderBackendTime::Scope` as an empty class (the
   header's `static_assert`), and the HTML UI perf tests still measure (their `static_assert`).
9. `ShaderZeroCost.DefaultVariantsUnchanged` passes with its hash list untouched.
10. A sun-only scene's lit submeshes load `Dlo`; adding any non-directional Light component (even disabled)
    switches them to the general variant with no frame drawn unlit or missing (the loaded variant draws until the
    new one is built); `generate_shaders` for a sun-only scene emits each `Dlo` key and its twin.
11. `getShadowReceiverPlanes` and its test are gone; no reference remains (`git grep`).
12. `updateAllTerrainRenderTextures` does not walk `MeshComponent`s; LOD polling and `updateObjectLights` return
    early as section 6.4 says (read in review).
13. The editor builds no `PIP_DEFAULT` pipelines for scene meshes (`EditorFramebufferSkipsSwapchainPipelines`).
14. All goldens pass unchanged on glcore and Vulkan, or each changed one is explained before re-baselining.
15. Docs of section 6.9 updated; `tools/mcp/gen_tool_docs.py --check`, `tools/mcp/tool_budget.py --check
    tools/mcp/budget.json` and the Lua doc test pass.
16. `tools/check-changed` green on the final commit; smoke test, HTML UI suites and web gate green once.

## 12. Files likely touched

- `engine/renders/sokol/SokolSystem.{h,cpp}`, `SokolObject.{h,cpp}`, `SokolPipelineCache.{h,cpp}`,
  `SokolBuffer.cpp`, `SokolTexture.cpp`, `SokolFramebuffer.cpp`, `SokolCamera.cpp`, `SokolShader.cpp`,
  `SokolCmdQueue.{h,cpp}`; `engine/libs/sokol/sokol.cpp`.
- `engine/core/render/ObjectRender.{h,cpp}`, `engine/core/render/RenderBackendTime.h`.
- `engine/core/subsystem/RenderSystem.{h,cpp}` (queues, update loop, lights, pipelines, variants, removal),
  `RenderSystemParticles.cpp`, `TransformPropagation.h`.
- `engine/core/math/Matrix4.{h,cpp}`, `AABB.cpp`.
- `engine/core/pool/ShaderPool.{h,cpp}`, `engine/core/shader/ShaderVariantCompiler.cpp`.
- `engine/shaderlib/includes/lights.glsl`, `shadows.glsl`, `shadow_sampling.glsl`, `shadow_data.glsl`,
  `engine/shaderlib/mesh.frag`.
- `engine/core/Engine.{h,cpp}`, `engine/core/debug/DebugStats.{h,cpp}`, `DebugOverlay.cpp`,
  `engine/core/script/binding/ProfilerClassesLua.cpp`, `CoreClassesLua.cpp`, `stubs/Profiler.lua`,
  `stubs/DebugOverlay.lua`.
- `engine/CMakeLists.txt` (one definition).
- `editor/shader/ShaderVariantCollector.cpp`, `editor/mcp/tools/MaterialsLightingTools.cpp`,
  `editor/window/GameWindow.cpp`.
- Tests of section 10; deleted `editor/tests/engine/subsystem/ShadowReceiverCullingTest.cpp`.
- Docs: `docs/profiling.md`, `docs/mcp/guide/profiling.md`, `docs/mcp/guide/shaders.md`,
  `docs/rendering/shadows.md`, new `docs/perf/draw-perf.md`.

Hot spots: `RenderSystem.cpp` (update loop `14660-15000`, camera pass `16020-16150`), `SokolObject.cpp`,
`lights.glsl` and the shadow includes, `ShaderPool.cpp` property tables, `ShaderVariantCollector.cpp`,
`Engine.h` `FrameStats`.

## 13. Cross-feature dependencies

| Feature | Relation |
|---|---|
| `mesh-memory` | Same files: `SokolObject.{h,cpp}` (it drops `pipeline_desc`/`baseDesc` after load; `beginDraw` builds mirrored and no-cull variants from `makePipelineDesc` on first use, so it must keep what that needs) and the bone block of `RenderSystem::update` (its `SkinComponent` replaces `hasSkinning` as the gate, section 6.3). This feature adds no per-object fields, so its `sizeof` tests are unaffected. Whoever merges second adapts. |
| `sprite-batching` | Builds on the tracker: consecutive sprites with equal pipeline and bindings already skip applies; its batching should call `SokolSystem::applyPipeline/applyBindings`, not keep its own "applied" state. It owns the per-frame sprite visit and settled-sprite culling (`isBatchVisible`, which this feature calls earlier but does not change). |
| `render-correctness` | Same shader includes (`1251c3139` explicit-LOD lookups in `shadows.glsl`/`shadow_sampling.glsl`) and the update loop (mesh bounds from real vertices, instanced batch slack near `RenderSystem.cpp:14708`). Merge carefully; `Dlo` guards wrap their lines. |
| `water-reflections` | Its planar reflection camera is one more 3D pass: its opaque queue gets the pre-cull and sort for free. It reads the frame shadow atlas, one reason D6 deletes receiver culling. Its transparent-under-water ordering edits the same camera-pass loop (merge hotspot). |
| `water-boats` | None beyond `water-reflections`. |
| `ssr-probes` | "Probe slots only for drawn IBL meshes" can use the pre-culled opaque queue as the drawn set. Same `RenderSystem.cpp` region. |
| `export-pipeline` | Same `ShaderVariantCollector.cpp` and saved key lists: the `Dlo` twin rule must hold wherever keys are merged or warned stale (`7401e66e7`); a `Dlo` key with its twin is never "outgrown". |
| `runtime-core` | "Hidden child kept hidden" edits `propagateTransforms` next to `computeWorldTransform` (`TransformPropagation.h`). |
| `agent-authoring` | Rotation normalization: `composeTRS` takes the quaternion as stored; no conflict expected. |
| `scene-integrity` | "Stopped scenes beside Play never update cameraless" edits the head of `RenderSystem::update`. |
| `platform-lifecycle` | Hidden games skip drawing; the tracker is reset per pass, so nothing to do. `SokolSystem.cpp` is shared (commit path). |
| Others (`unicode-text`, `mobile-shipping`, `build-toolchain`, `project-window`, `ui-components`, `scripting`, `code-editor-git`) | No dependency. MESH property bit 36 is claimed here; a feature adding a mesh bit takes 37 or above. |

Follow-up (not in this branch): with `Dlo` freeing two texture units, let the Ultra tier's PCSS raw view
(`lights.glsl:109-113`) into GLES3/WebGL2 and painted-terrain `Dlo` variants, after a texture-unit count test per
backend and a web-gate run.

## Deviations

Recorded during implementation (2026-11-06), after mesh-memory, S54 (LOD cross-fade), S55 (automatic
GPU instancing, visible lists) and S57 (Forge lighting) had landed on `main` and changed the draw path.

- **The tracker builds on S55's.** S55 had put a pipeline-only skip in `SokolPipelineCache`
  (`lastApplied`, `appliedEpoch`), on only while GPU Instancing is on. Its state moved to
  `SokolSystem` (`applyPipeline`, `applyBindings`, `isBindingsApplied`, `draw`,
  `forgetAppliedState`, `forgetAppliedBindings`, `appliedEpoch`, `appliedPipeline`) and is on in
  every mode; S55's uniform-block cache keys on `SokolSystem::appliedEpoch()`. S55's R10 ("GPU
  Instancing off restores the previous submission") now covers its batches and uniform skips only:
  a repeated pipeline or binding is skipped either way. `jasperengine_sg_next_draw_valid` already
  existed (S55); `SokolSystem::draw` checks it after every draw, `applyBindings` after the bind.
  Uploads and resource destroys forget only the bindings (`forgetAppliedBindings`), so the uniform
  cache survives them; passes, errors, a pipeline destroyed and context changes forget everything.
- **Cull before sort is S55's visible list.** `buildVisibleQueue` (jobs, per-submesh bounds,
  `preCulled`) now runs for every 3D colour pass and probe face whatever GPU Instancing is, before
  the sort (`sortByState`), and S55's batch grouping runs over that order. The state key reuses
  S55's batch key ingredients (pipeline id, `materialKey`, `meshKey` of the entry's first submesh)
  instead of a key read from `SokolObject`'s bindings. No `passQueueCulled` member and no
  `getOpaqueQueueSize`: S55's `passPreCulled` is the flag, and the tests read the visible list's
  `Engine::FrameStats::culled`. Transparent mesh entries are culled with `isBatchVisible` before
  they are queued, and drawn with `passPreCulled`.
- **LOD polling** was already gated on the scene's LOD on `main` (S54): nothing to change.
- **Bone bounds (F3):** mesh-memory's palette holds only the entries a skin gives, so a rigid mesh
  no longer scans 128 matrices. The `hasSkinning` gate still fixes the inflated bounds of a rigid
  mesh that holds palette entries (a script's write, a posed palette in a file); its regression
  test writes one.
- **`Dlo` is MESH bit 42** (36-41 went to S57's `Til`/`Cap`/`Cts`/`Oit`, S54's `Lfd`, S55's `Aui`).
  It is not combined with Light Mode Tiled (the tiles list the scene's lights as they come). The
  object lights of a `Dlo` variant leave non-directional lights out in `orderObjectLights`; the
  cached light bits keep their `uint32_t` and take `Dlo` as bit 1 (UV1's, never in the mask)
  instead of widening. The editor's prediction reuses S57's `FACT_LOCAL_LIGHTS`. The twin rule runs
  in `ShaderVariantCollector::collectScene` and `Project::finishBuildShaderKeys` (the list every
  build reads). The shader cache moved to `v49` (lead rule: a branch changing shaders bumps it).
- **Tests.** The math guards are one file (`engine/tests/core/math/AffineMathTest.cpp`), the bench a
  gtest suite in `engine/tests/core/math/DrawPerfBenchTest.cpp` (`engine/tests/core/bench` holds
  separate executables). `DrawPerfGpu` uses only engine API older than this branch, so all of it is
  a red control. The dedicated tracker guards (buffer update between draws, a binding not ready, a
  destroyed resource, a new pass, Vulkan descriptor bytes) are not separate tests: every GPU golden
  suite (RenderGpu, InstancingGoldenGpu, LightingGpu, SkinPaletteGpu, SpriteFramesGpu, ParticlesGpu,
  TextureRebindGpu, the HTML UI suites) draws through the tracker and would show a stale binding.
  The MCP test covers what agents and scripts read (`directional_only`, `setPassCalls`); the
  SetPass and binding counts are `DrawPerfGpu.RepeatedDrawsApplyStateOnce`.
- **`scene:getRenderStats()`** keeps its fields (it has no SetPass count; `Profiler.frameStats()`
  and `DebugOverlay.getStats()` do).
- **Review round (2026-11-07).** The first point or spot light keeps the general Lit until the scene
  loads again (`nonDirectionalLightSeen`), so lights coming and going never reload every mesh; a mesh
  still on `Dlo` takes such a light in its SH (`selectMeshLights`, `MeshLights::directionalOnly`
  re-picks when it leaves `Dlo`) instead of dropping it. The first update takes the scene's state
  without a reload. The twin rule adds the tiled (`Til`) form unless Light Mode is Per Object. Dropping
  `Til` is not added to `ShaderPool::getFallbackProperties`: the tiled and the object lights blocks
  differ, and the draw uploads by the key it asked for, so a build lacking the tiled twin cannot fall
  back that way; the collector ships it instead. `SystemRender::setRepeatedStateSkipped` (diagnostics,
  tests) turns the skipping off: `DrawPerfMixedGpu` compares the two with batched sprites, S55
  batches, lone meshes and shared-buffer Models in one pass. Every resource and pipeline destroy in
  `renders/sokol` forgets the state (loading threads excepted: they hold none); the tracker asserts
  the render thread and `computeWorldTransform` the parent's affine matrix in debug builds.
  `TransformPropagation.WorldValuesMatchTheGeneralProducts` lives in `engine/tests/core/ecs` (beside
  its registry helpers); the queue order structs are public for `RenderQueueOrderTest`. Shader cache
  v52 (v51 is water-reflections').
- **D8 is withdrawn.** Skipping the swapchain pipelines while `Engine::getFramebuffer()` is set lost
  draws on Vulkan: in `SpriteBatchGpu.SettledSpritesShowDirectWrites` run alone, a sprite turned
  alpha-masked never drew in its batched single run (one draw counted for two runs) while it drew with
  batching off, in every run with the skip; with `PIP_DEFAULT` built it passes (20 of 20 alone). Not
  every path follows the scene's pipeline set either: `getBatchEligibility` asks for `PIP_DEFAULT`,
  which the editor would never have. The cause in the batched path is not found, so
  `getScenePipelines` is `main`'s again and `EditorFramebufferSkipsSwapchainPipelines` (criterion 13)
  is not shipped.
- **`SettledSpritesShowDirectWrites` waits for the masked variant.** After the suites added here, the
  masked sprite's new variant could still be waiting for its first, warmed draw (spec S32) in the three
  batched and three unbatched frames it compares; the warm request is made by that first draw, so the
  wait for idle GPU creation does not cover it. The test now compares from a frame in which no draw
  waited.
