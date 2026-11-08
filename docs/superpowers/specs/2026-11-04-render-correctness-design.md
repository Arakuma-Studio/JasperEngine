# Render correctness: bounds, vertex inputs, depth, variants, foliage, shader safety and gizmos (design)

Date 2026-11-04. Branch `dc/render-correctness`. Program: Doriax catch-up, ledger
[`docs/doriax-catchup/LEDGER.md`](../../doriax-catchup/LEDGER.md) (feature 10).

Evidence labels used below: **read** = from reading the code (inferred), **ran** = a script was run over
the tree while writing this spec (the command is given), **dossier** = the comparison's skeptic verdicts.
Nothing here was built or drawn; every behaviour claim is inferred until the tests of section 10 run.

## 1. Goals

1. Every mesh's bounds come from its vertices only (never stretched to its local origin), so culling,
   selection boxes, picking and water levels follow the geometry. The same holds for UI quads.
2. A shader never declares a vertex input the mesh does not feed: a mesh without normals draws lit with flat
   normals (glTF's rule) instead of reading an unfed `a_normal` (zero on GL, undefined on Vulkan, invalid on
   Metal and D3D11).
3. Transparent Points stop writing depth (Unity's transparent queue convention), and change pipelines when
   their effective transparency changes.
4. A texture edit on a loaded mesh always gets the shader variants (colour, depth, G-buffer) the new
   material needs, without a reload flash.
5. GL never drops a flattened uniform block because the driver trimmed an unused tail of its array (the two-UV-set
   bug, fixed for every flattened block).
6. Walking through foliage costs nothing beyond the chunks entering the ring; instanced batches are not rebuilt
   for sub-margin eye moves; billboards (spherical and cylindrical) keep facing the camera; instance distance
   fade is applied in every pass that draws the instance (colour, depth, SSR G-buffer).
7. New foliage controls: a per-layer **Min Draw Distance** with a scale cross-fade (a layer copy with a cheaper
   mesh becomes the far detail level, with no pop), an Instanced Mesh **Near Fade** range behind it, and a
   per-platform **Foliage Distance** quality scale (Unity's `QualitySettings.terrainDetailDistanceFactor`).
8. Light loops are safe on every driver: no implicit-derivative texture reads or derivatives in non-uniform
   control flow, with a lint that keeps it that way; D3D11 gets the same 2D shadow PCF quality as the others.
9. The Scene view draws an orthographic camera's frustum with real depth and a selected camera's full frustum
   (Unity); a perspective camera with near <= 0 gets a valid projection.
10. The Vulkan editor enables robust buffer access where it is safe with descriptor buffers, and reports it.

## 2. Non-goals

- Doriax `1251c3139` (point-light shadow `textureLod`): already in Jasper (`shadows.glsl:117`,
  `shadow_sampling.glsl:51,54,194`, `punctual.glsl:140`; dossier verdict confirmed). Nothing to do.
- Robust buffer access in shipped players (`engine/platform/common/VulkanContext.cpp:209-276`): not enabled.
  Reason: it has a measurable cost on some mobile GPUs (Mali, Adreno) and no out-of-range read is known in
  Jasper's shaders; the editor runs shaders under development (forks, library shaders), players run tested
  ones. The decision function is shared so a later change is one line (section 6.11).
- Automatic transparency for Points from per-point colour alpha (a point colour with alpha < 1 does not move
  the component to the transparent queue). Reason: a script fading points would rebuild pipelines while it
  fades. Documented: set **Transparent** on for points that fade.
- Per-terrain foliage distance scale (Doriax's `Terrain.foliageDistanceScale`). Reason: each layer already
  has its own Draw Distance per terrain; the knob a game needs per device is global (Unity puts it in
  QualitySettings). See decision 7.
- A hard near cut for foliage (Doriax `e1bfe0341`'s `cullNearDistance`). Replaced by a scale fade (decision 6).
- Performance work on the per-frame foliage scan, the secondary-view instance list rebuild frequency and
  `MeshSystem.cpp:7254` (a mesh whose bounds are ZERO recomputing every frame): `draw-perf`'s mandate.
- SSR composite changes: `ssr-probes`. This feature only makes the G-buffer pass shrink faded instances.

## 3. Users and stories

- Human: imports a glTF building whose root node is offset 50 m from the origin. Today its selection box and
  culling box reach back to the origin; after, they hug the building.
- Human: imports a glTF without normals (a photogrammetry scan, a CAD export). Today it draws black or garbage
  on Vulkan; after, it draws lit with flat facets, as glTF and Unity do.
- Human: makes snowflakes with a Points component and a round soft texture. Today the transparent corners of
  each quad hide the snowflakes and the smoke drawn after them; after, they blend.
- Human, SSR on: drags a metallic-roughness texture onto a loaded material. Today SSR keeps reflecting with the
  factors until a reload; after, the reflections follow the texture on the next frames.
- Human: walks a character through a grass field. Today every few metres a hitch and a few frames of missing
  grass chunks; after, smooth.
- Human: makes a tree layer with a full mesh to 40 m and a copy with a billboard mesh from 40 m to 300 m
  (**Min Draw Distance** 40). The two cross-fade by scale, no pop.
- Human shipping to phones: sets **Project Settings > Quality > Foliage Distance (Mobile)** to 0.6; desktop
  stays at 1. A settings menu calls `Engine.foliageDistanceScale = 0.8` at runtime.
- Human: an orthographic camera of width 20 shows as a box 10 deep in the Scene view instead of a flat slab;
  clicking it shows its full near-to-far volume.
- Agent: `set_properties` on `InstancedMeshComponent.nearFadeStart` / `TerrainComponent.foliageLayers[1].minDrawDistance`,
  `set_project_settings {"quality.foliage_distance_mobile": 0.6}`, `eval_lua "Engine.foliageDistanceScale"`,
  `screenshot` to check; `get_render_stats` reports `gpu.robust_buffer_access` on a Vulkan editor.

## 4. Findings

Jasper paths are at `main` `b4f7816ea`; Doriax commits are references (`analysis/doriax`, read only).

### 4.1 Mesh bounds stretched to the origin (Doriax `55f64b09f`, AABB part)

- `MeshSystem::calculateMeshAABB` (`engine/core/subsystem/MeshSystem.cpp:3192-3273`) starts from
  `mesh.verticesAABB = AABB::ZERO` (`:3212`), a finite box at the origin (`math/AABB.cpp:12`), and merges every
  vertex into it (`:3251`); `AABB::merge(Vector3)` keeps a finite box's extents (`AABB.cpp:238-248`). So every
  mesh's bounds include its local origin (read). Copied to `mesh.aabb` (`:3268`). Callers include primitives
  (`:778,866,918,1129`), shapes (`:4381-5039`), glTF loads (`:6054,6407`), `:6757`, `:7255`.
- Effects (read): looser frustum culling, oversized selection and picking boxes (`picking/SceneBvh.cpp:93`),
  wrong water level for an offset water mesh (`object/environment/Water.cpp:144` uses the bounds' max Y).
- A test pins the bug: `editor/tests/engine/subsystem/GltfMeshOffsetTest.cpp:192` expects minimum `(-6, 0, 0)`
  for vertices all at z = 3 (and its helper comment `:100` says "the AABB also holds the origin").
- Same class: `UISystem::calculateUIAABB` (`engine/core/subsystem/UISystem.cpp:2630-2640`) seeds `ui.aabb`
  with `AABB::ZERO` too (read).
- `AABB::ZERO` is also a sentinel for "not computed / unloaded" (`MeshSystem.cpp:7254`, `RenderSystem.cpp:388,
  1684,5300`, `RenderSystemShared.h:41`, `RenderSystemShadows.cpp:47`, `SceneRender2D.cpp:199`): a mesh with no
  float positions must keep ZERO (Doriax keeps a ZERO fallback for the empty case).

### 4.2 Normal-less meshes leave `a_normal` unfed (Doriax `55f64b09f`, `p_hasNormal` part)

- `RenderSystem::getSubmeshShaderVariant` (`RenderSystem.cpp:3715`) sets `p_hasNormal = true` on the built-in
  Lit's lit path (`:3767`) and on the 2D light path (`:3792`) whatever `inputs.normal` says, and sets
  `p_hasTangent` from `inputs.tangent` without requiring normals (`:3768-3770`, `:3793-3795`). `mesh.vert:55-57`
  then declares `in vec3 a_normal`. Only morph inputs have a fallback binding
  (`RenderSystem::bindUnfedMorphInputs`); `SokolObject::checkVertexInputs` (`engine/renders/sokol/SokolObject.cpp:916-929`)
  warns and counts the unfed load (`ObjectRender::getUnfedVertexInputLoads`) (read).
- The glTF loader creates no normals when the file has none (`MeshSystem.cpp:5807` maps only present
  attributes; dossier). The shaders already handle the missing case: `pbr.glsl:221-226` builds flat normals
  from `dFdx/dFdy(v_position)`, `mesh.vert:322-327` uses the model's +Y for vertex lights,
  `lighting2d.glsl:114` returns +Z (read). The manifest path already uses `inputs.normal` (`:3840`).

### 4.3 Transparent Points write depth (Doriax `0141520ca`)

- `RenderSystem::resolveComponentRenderState` (`RenderSystem.cpp:3315-3338`) turns depth write on for AUTO,
  "blended or not" (`:3333-3336`); `loadPoints` uses it (`:11250-11253`); `points.frag` has no discard. So a
  transparent Points quad writes depth over its whole quad, hiding points and transparent objects drawn after
  it (read; dossier confirmed). Lines and trails already turn AUTO depth write off when blended (`:11544-11548`).
  Particle Systems take depth write from their renderer (`RenderSystemParticles.cpp:368`): out of this path.
- `points.derivedTransparent` (the texture's alpha) is set only in `loadPoints` (`:11248`); a texture swapped
  later (rebound in `drawPoints`, `:11961-11969`) never updates the queue (read). `Points::setTransparent`
  (`engine/core/object/Points.cpp:266-270`) and the reflected `transparent` field (`RenderReflect.cpp:266`, no
  update flag) do not reload the pipeline.

### 4.4 Texture edits keep stale variants (Doriax `17ce79969`)

- On `needUpdateTexture` the update loop (`RenderSystem.cpp:14837-14885`) only sets
  `needUpdateGBufferTexture` / `needUpdateDepthTexture`, reloads on a few hand-picked bits ('Uv1'/'Uv2' at
  `:14851-14861`, 'Nmp' at `:14874-14882`) and compares the Lit lobes (`pbrVariantMatches`, `:3268-3279`). The
  G-buffer variant's base colour ('Bct', bit 0) and metallic-roughness ('Mrt', bit 8) bits, the depth variant's
  alpha-clip texture ('Tex') and the Stochastic Tiling bits are never compared, so the G-buffer binds only the
  slots its old variant declares (`:5426-5443`) (read; dossier confirmed). Reached from the Inspector and MCP
  (`MaterialReflect.cpp:174,176`, `Catalog.cpp:887,965-967`) and Lua (`Mesh::setMaterial`).
- `meshVariantShadersReady` (`:13089-13140`) already rebuilds the full asked-for variant from the submesh's
  attributes of its last load (`getSubmeshShaderInputs` + `getSubmeshShaderVariant`), and `loadMesh` stores the
  asked-for properties even when a stand-in or fallback draws (`:6312-6349`) (read).

### 4.5 Two UV sets, no normal map: the GL uniform array (Doriax `ebd4b64e7`, dossier verdict uncertain)

- GL flattens a uniform block into one array uniform of `size/16` vec4s (`SokolShader.cpp:191-198`), and sokol
  uploads it with `glUniform4iv(loc, count)` using the declared count (`sokol_gfx.h:11275-11279,12318`).
- `u_fs_texCoordSets` is `ivec4 set0, set1` (`pbr.glsl:35-38`); without a normal map, Stochastic Tiling or
  `PBR_EXT`, nothing reads `set1` (`:41-46`), so a driver may report the array with one active element or as a
  non-array. Then `count = 2` is clamped (spec) or rejected with `GL_INVALID_OPERATION` (drivers that turn it
  into a non-array), and in the second case `set0` never reaches the shader and maps on UV set 2 sample set 1.
  Which drivers reject it cannot be read from code (dossier).
- The class: any flattened block whose tail is unused by a variant. Doriax's fix (read `set1.w` in
  `selectUV`) is narrow and unsafe in Jasper, where `set1.w` is a real value under `PBR_EXT` (`pbr_ext.glsl:50`).

### 4.6 Foliage ring resizes while walking (Doriax `de70fa31c`)

- `MeshSystem::updateFoliageLayer` (`MeshSystem.cpp:4064-4222`) sizes the chunk grid `columns x rows` from the
  current view span (`:4086-4087`); with Draw Distance 50 (default, `TerrainComponent.h:154`) columns alternate
  4 and 5 as the camera walks (dossier arithmetic). Each change destroys or creates chunk entities
  (`:4093-4101`, each re-parses the mesh, 2 per frame `:4135`) and remaps every slot through
  `foliageSlotIndex(…, columns, rows)` (`:3797-3801`, `:4142`), so all chunks refill (read).

### 4.7 Instanced batches: rebuild margin, billboards, fade passes (Doriax `e3aa4e1b8`)

- Any XZ eye change on a fading mesh reruns `updateInstancedMesh` (`RenderSystem.cpp:14707-14713`), rebuilding
  render lists and instance buffers every frame the camera moves; foliage chunks always fade (read).
- An opaque instanced billboard without fade keeps its old rotation on the main view: the camera-move trigger
  (`:14719`) tests `sortTransparentInstances || particleCamera`, not `instancedBillboard`; the rotation is
  computed only on rebuild (`:13349-13353`). Secondary views rebuild their own lists (`:17595`), so the Scene
  view can hide it while the Game view and exports show it (read; dossier confirmed).
- **Cylindrical Billboard** (`InstancedMeshComponent::instancedCylindricalBillboard`) is in the Inspector
  (`InspectorRenderers.cpp:650`), reflection, serialization, generated code and Lua
  (`GameObjectClassesLua.cpp:1867`) but no render code reads it (ran: `grep -rn instancedCylindricalBillboard
  engine/core` finds only `object/Mesh.cpp` and the Lua binding): the instances face the camera fully.
- The SSR G-buffer variant has no instance fade (`gbuffer.vert`, ran: `grep -n fade` finds nothing;
  `ShaderPool::getGBufferMeshProperties` has no fade bit), so with SSR on faded-out instances are drawn full
  size into the G-buffer; `instanceFadeCullEnd` (`RenderSystemShared.h:20-35`) then disables the CPU cut
  under SSR, so SSR scenes draw every instance (read).
- The fade code is copied in four vertex stages (`mesh.vert:220-231`, `depth.vert:124-134`,
  `includes/surface_vs.glsl:406`, `includes/surface_depth_vs.glsl:207`); `u_vs_fade.fadeRange.zw` is unused
  (`RenderSystem.cpp:394-398`).

### 4.8 Foliage distance scale and min draw distance (Doriax `c3546403b`, `e1bfe0341`)

- Foliage draw distance is unscaled (`MeshSystem.cpp:4069`); no quality knob exists (ran: grep
  `foliageDistanceScale` finds nothing). Layers have no minimum distance (`TerrainComponent.h:141-157`),
  instanced meshes no near cut (`RenderSystem.cpp:13372-13375`).
- Jasper has per-platform quality defaults with a runtime override for atmosphere
  (`Engine::setAtmosphereQualityDefaults`, `Engine.cpp:840-870`; Project Settings rows
  `ProjectSettingsWindow.cpp:1972-1985`; settings registry `editor/settings/SettingsRows.cpp:1669-1690`).

### 4.9 2D lights add tangents without a normal map (Doriax `86c90b17d`)

- Under `useLight2D` the variant takes 'Tan' whenever the mesh has tangents (`RenderSystem.cpp:3791-3799`),
  though `lighting2d.glsl` reads tangents only for a normal map (`:97-98`, `:122`): an extra variant, attribute
  and varyings (read). `ShaderPool::normalizeKey` (`ShaderPool.cpp:1697-1704`) does not drop the stale bit from
  saved variant collections and warm-up lists.

### 4.10 Implicit derivatives in non-uniform control flow (EXTRA-FINDINGS)

- `lighting2d.glsl:155,169`: `texture()` on the 2D shadow atlas inside `shadow2DCalculation`, called from the
  per-light loop after a per-fragment `continue` (`:182-202`). The Intel GL hang pattern of Doriax `1251c3139`.
  The atlas has one level, so `textureLod(…, 0.0)` gives the same value (read).
- `ssao.frag:100`: `texture()` on the depth target after a per-fragment `continue` (`:97-98`) (read).
- `mesh.frag:454-456`: `lightShadow` (which takes `dFdx/dFdy` of the position first, `lights.glsl:277-278`) is
  called inside `if (NdotL > 0.0 || foliageTransmission > 0.0)`, a per-fragment branch, against the rule at
  `lights.glsl:43` (read). The receiver-plane bias is undefined on the terminator's quads.
- ran: a call-graph scan of `engine/shaderlib` (implicit-derivative calls, direct or through functions,
  inside loops whose body contains `continue` or `discard`) reports exactly `lighting2d.glsl:202`,
  `ssao.frag:100` and `terrain_fs.glsl:565` (`setupTerrainStochastic`; the terrain path uses `textureGrad`
  with precomputed derivatives, a false positive to mark).
- D3D11 2D shadows: `MAX_SHADOW2D_RADIUS` is 2 on HLSL ("MEDIUM and HIGH render as LOW here",
  `lighting2d.glsl:130-135`) because `texture()` in a variable loop cannot compile there; the 3D shadows already
  clamp and use explicit LOD on every backend (`shadows.glsl:22-26`).

### 4.11 Orthographic camera frustum drawn flat (Doriax `570a023cf`)

- `SceneRender::drawCameraFrustumLines` (`editor/render/SceneRender.cpp:3888-3905`) sets `far = near + 2` for
  every camera type; `SceneRender3D.cpp:1447` always passes fixed size and `SceneRender2D.cpp:1194` uses the
  default. A 20-wide ortho camera draws as a 2-deep slab (read). `updateCameraFrustum`'s change check
  (`:3856-3871`) has no fixed-size input.
- `RenderSystem::updateCamera` (`RenderSystem.cpp:12517-12525`) passes raw near/far to
  `Matrix4::perspectiveMatrix`: near <= 0 gives an invalid projection; reflection-probe capture already clamps
  (`:2840-2841`) (read).

### 4.12 Vulkan robustness in the editor (Doriax `0078f75ab` + `82d13ab0f`, dossier verdict uncertain)

- `Renderer::State::createVulkanDevice` (`editor/backend/renderer/RendererVulkan.cpp:211-249`) never sets
  `robustBufferAccess`. It enables descriptor buffers (`:218-225`), and sokol writes plain descriptor sizes
  (`sokol_gfx.h:21742,22087`), so robustness is only safe where the robust sizes equal the plain ones (Doriax
  `82d13ab0f`). No out-of-range read is known in Jasper; the Vulkan editor is a configure option, not the
  default (`CMakeLists.txt:92`) (dossier).

## 5. Product decisions

1. **Bounds hug the vertices; an empty mesh keeps `AABB::ZERO`.** Seed with a null box, merge, and fall back to
   ZERO when nothing merged, so the sentinel users (4.1) keep working. The same for UI. Reason: the box is used
   for culling, picking and water levels; the origin is not geometry.
2. **A mesh without normals draws lit with flat normals** (glTF 2.0: "client implementations MUST calculate flat
   normals"), never with an unfed input. Tangents count only with normals. Reason: same result on all five
   backends; Unity computes normals at import, we compute them in the fragment stage at no cost when present.
3. **Points' Depth Write Auto follows the queue**: transparent (the **Transparent** flag, or **Auto
   Transparency** and a texture with alpha) writes no depth, opaque writes it; **On** and **Off** and a shader
   manifest's `zwrite=` stay as set. Reason: Unity's transparent shaders are `ZWrite Off`; Jasper's lines and
   trails already do this. Existing scenes change look; that is the fix (section 9). The Inspector's Depth
   Write tooltip says it.
4. **A texture edit re-resolves the whole variant** (colour, depth, G-buffer, lobes, maps) and swaps it in
   with the variant-reload path (the old variant draws until the new one is built and warmed). Reason: the
   class of bug is "a hand-picked subset of bits", so the hand-picked checks go.
5. **GL uploads a flattened block with the count the driver reports active**, fixed once in sokol. Reason: it
   fixes every block and variant, costs one query per uniform at shader creation, and needs no shader change
   (Doriax's shader-side read would be wrong under `PBR_EXT`).
6. **Min Draw Distance fades in by scale** over `[minDrawDistance * 0.75, minDrawDistance]`, the mirror of the far
   fade over `[drawDistance * 0.75, drawDistance]`. A near layer with Draw Distance D and a far layer with Min
   Draw Distance D cross-fade over the same band. The general mechanism is two new Instanced Mesh fields,
   **Near Fade Start** and **Near Fade End**, carried in the unused `fadeRange.zw` of the existing fade block.
   Reason: a hard near cut pops (Unity's LOD Group cross-fades); the uniform slot is free, so no new variant.
7. **Foliage Distance is a quality setting**: per platform group in Project Settings > Quality (Desktop, Mobile,
   Web; default 1.0 each, so old projects look the same), overridable at runtime with
   `Engine.foliageDistanceScale` (Lua and C++), reset when Play stops. It multiplies every foliage layer's Draw
   Distance and Min Draw Distance. 0 removes foliage. Reason: Unity's `QualitySettings.terrainDetailDistanceFactor`;
   Jasper's precedent is Atmosphere Quality (defaults per platform + runtime override, `Engine.vSyncCount` in
   the glossary). Mobile default stays 1.0: changing it would change shipped games' look on upgrade; the docs
   recommend 0.5-0.7 for phones.
8. **Instance distance fade applies in every pass**: the SSR G-buffer variant gets the fade ('Ifd', GBUFFER bit
   18), and the CPU cut no longer turns off under SSR. Reason: a faded instance must not appear in reflections
   or SSR depth; and SSR scenes regain the cut.
9. **Cylindrical Billboard works**: the instances turn about the mesh's local up axis only, facing the
   camera's view direction projected on the local XZ plane (the instanced billboard is view-aligned, one
   rotation for the batch). Reason: the control exists and is documented by its name; Unity's billboard
   renderer has the same two modes.
10. **Instanced lists rebuild past a slack margin**: far slack `max(0.25, 0.04 * (fadeEnd - fadeStart))` model
    units beyond Fade End (instances there are already shrunk to nothing), near slack
    `min(0.25, 0.1 * nearFadeStart)` inside Near Fade Start. Reason: Doriax's values, safe because both
    margins sit where the shader draws nothing.
11. **The Scene view draws a selected camera's full frustum** (near to far, Unity), and every other camera a
    compact one: depth 2 for perspective, `max(width, height) / 2` for orthographic, each capped at
    `far - near`. No new display setting (Doriax's "Show full camera frustum"): selection already says which
    camera the user is looking at. A perspective camera renders with a near of 0 or less as 0.001
    (a smaller positive near is kept) and `far = max(far, near + 0.001)`; what linearises its depth (the
    volumetric fog and clouds, the screen-tile lights, the shadow cascades) reads the same near and far.
12. **Vulkan robust buffer access: editor only, guarded**: on when the device supports it and
    `robustUniformBufferDescriptorSize == uniformBufferDescriptorSize` and
    `robustStorageBufferDescriptorSize == storageBufferDescriptorSize`; logged once at startup with the reason;
    reported as `gpu.robust_buffer_access` (true/false, null on other backends) by `get_render_stats`.
13. **Light loops sample with explicit LOD and take derivatives in uniform control flow**: `textureLod(…, 0.0)`
    for every single-level target sampled in a loop or after a per-fragment exit; `lightShadow` gains a form
    taking the derivatives, computed once before the light loop. A lint (`tools/check_shader_lod.py`) enforces
    it. D3D11 2D shadows get the full radius (6) like the other backends.
14. **2D lights take tangents only with a normal map**; `normalizeKey` strips 'Tan' from stale built-in
    (custom id 0) `Ult`+`L2d` MESH keys without 'Nmp'. Keys with a custom id keep it: a manifest shader may
    `@require tangents` on the 2D path.

## 6. Design

### 6.1 Bounds (engine)

- `MeshSystem::calculateMeshAABB` (`MeshSystem.cpp:3192`): `mesh.verticesAABB.setNull()` before the submesh
  loop; after it, `if (mesh.verticesAABB.isNull()) mesh.verticesAABB = AABB::ZERO;`. `bonesAABB` entries are
  already default (null) boxes. Nothing else changes (`mesh.aabb = mesh.verticesAABB`, flags).
- `UISystem::calculateUIAABB` (`UISystem.cpp:2630`): same seed and fallback.
- `GltfMeshOffsetTest`: `:192` expects `(-6, 0, 3)`; `:100`'s comment loses "(the AABB also holds the origin)";
  `NestedMeshNodeKeepsItsTransforms` adds the minimum `(0, 2, 6)`.

### 6.2 Vertex inputs follow the mesh (engine)

In `RenderSystem::getSubmeshShaderVariant` (`RenderSystem.cpp:3715`):

- Lit path (`:3767-3770`): `p_hasNormal = inputs.normal; p_hasTangent = p_hasNormal && inputs.tangent;`
  (`p_hasNormalMap` unchanged: `pbr.glsl:212-235` builds a tangent frame from derivatives without tangents).
- 2D path (`:3791-3799`): `p_hasNormal = p_hasNormal || inputs.normal;` and
  `if (p_hasNormal && inputs.tangent && inputs.normalTexture) p_hasTangent = true;` (keeps the lit path's tangent
  in mixed scenes; decision 14).
- Morph inputs: if the unfed-input test (10, `MeshInputsGpu`) reports unfed morph normal or tangent inputs on a
  mesh without normals, gate `inputs.morphNormal` on `p_hasNormal` and `inputs.morphTangent` on `p_hasTangent`
  when calling `ShaderPool::getMeshProperties`, and store the gated values in `Submesh::hasMorphNormal/Tangent`
  at `loadMesh` (`:6307-6310`) so binding matches.
- `ShaderPool::normalizeKey` (`ShaderPool.cpp:1697`): for `ShaderType::MESH`, custom id 0, bits 0 ('Ult') and
  22 ('L2d') set and 7 ('Nmp') clear: clear bit 8 ('Tan'). Callers dedupe already
  (`ShaderVariantCollection.cpp:45,77,94,102`, `ShaderWarmup.cpp:361`).

### 6.3 Points depth write (engine, editor)

- New static `RenderState RenderSystem::resolvePointsRenderState(const PointsComponent& points)` (header next
  to `resolveComponentRenderState`): calls `resolveComponentRenderState(points.blendMode, points.depthWrite,
  points.stencil, points.colorWrite, manifest, features, false)`, then, when `points.depthWrite == AUTO` and the
  manifest's resolved `depthWrite` is AUTO, sets `state.depthWrite = !points.isTransparent()`.
  `resolveComponentRenderState` itself is unchanged (UI and others share it).
- `loadPoints` (`:11250`) uses it and records `points.loadedTransparent = points.isTransparent()` (new runtime
  field in `PointsComponent.h`, not serialized, reset by `EntityCloner` like `derivedTransparent`).
- `drawPoints` (`:11961-11969`): after binding a new texture render, recompute
  `points.derivedTransparent = points.autoTransparency && points.texture.isTransparent()`.
- `loadPoints` also records `points.depthFollowsQueue` (runtime): Depth Write is Auto and the manifest's resolved
  `depthWrite` is Auto. Update loop (`:15026-15050`), before the reload check:
  `if (points.loaded && points.depthFollowsQueue && points.isTransparent() != points.loadedTransparent)
  points.needReload = true;` (one bool compare per Points component per frame). Without `depthFollowsQueue`
  the queue changes with no reload, as today.
- Inspector: the Points **Depth Write** row help: "Auto: written when opaque, not when Transparent (or when Auto
  Transparency finds alpha in the texture)." (`editor/window/InspectorRenderers.cpp`, Points section).

### 6.4 Variant re-resolve on texture edits (engine)

- Extract from `meshVariantShadersReady` (`:13089-13140`) a helper
  `SubmeshShaderVariant RenderSystem::getCurrentSubmeshVariant(Entity, MeshComponent&, unsigned int s,
  const TerrainComponent*, const InstancedMeshComponent*, bool hasMirror, const std::string& sceneMeshShader,
  const MeshShaderEnvironment&)` (inputs from `getSubmeshShaderInputs`, the fluid override on submesh 0, then
  `getSubmeshShaderVariant`). `meshVariantShadersReady` uses it.
- New `bool RenderSystem::loadedVariantMatches(const Submesh&, const SubmeshShaderVariant&)`: `properties ==
  shaderProperties`, `MaterialPbr::getDefineBits(litDefines) == litDefineBits`, every `pbrMaps[k]`, `depth ==
  (depthShader != nullptr)` and when depth `depthProperties == depthShaderProperties`, `gbuffer ==
  (gbufferShader != nullptr)` and when gbuffer `gbufferProperties == gbufferShaderProperties`. The extra pass's
  variant is compared too when `hasExtraPass`.
- In the `needUpdateTexture` branch (`:14837-14885`) replace the 'Uv1'/'Uv2', 'Nmp' and `pbrVariantMatches`
  checks for non-manifest submeshes with: `if (mesh.loaded && !mesh.needReload && !loadedVariantMatches(…))
  mesh.needVariantReload = true;`. Keep the manifest `@require normalmap` check (`:14866-14872`) and the
  terrain hex check above it. Compute the environment and `sceneMeshShader` once per mesh, only when some
  submesh has `needUpdateTexture` (no cost on frames without edits).
- `pbrVariantMatches` stays for its other callers (ran a grep before deleting; if none remain, delete it).

### 6.5 GL uniform array count (sokol, PATCHES.md)

- `engine/libs/sokol/sokol_gfx.h`, `_sg_gl_create_shader` uniform resolve (`:11275-11284`): after
  `glGetUniformLocation`, when `gl_loc != -1 && u->count > 1`, query
  `glGetUniformIndices(gl_prog, 1, &name, &index)` and `glGetActiveUniformsiv(gl_prog, 1, &index,
  GL_UNIFORM_SIZE, &size)`; when `index != GL_INVALID_INDEX && size > 0 && size < u->count`,
  `u->count = (uint16_t)size`. Offsets are unchanged (the CPU block layout is the declared one). Each changed
  line carries `// JasperEngine:`; add both functions to the `_SG_XMACRO` list (`:9222` block) for the Windows
  GL loader. GL 3.1+, GLES 3.0 and WebGL2 (`getUniformIndices` / `getActiveUniforms`, Emscripten implements
  both) have them.
- Test hook: `int sgj_gl_uniform_count(sg_shader shd, int ub_slot, int u_index)` (GL: the stored count; other
  backends: -1), declared in the JasperEngine section of the header, listed in `PATCHES.md` ("Flattened uniform
  arrays: active count").
- No shader change, no cache bump for this item.

### 6.6 Foliage ring (engine)

In `MeshSystem::updateFoliageLayer` (`MeshSystem.cpp:4064`):

- `ringSide = preview ? 2 * terrainRadius : std::min((int)std::ceil(2.0f * drawDistance / chunkSize) + 1,
  2 * terrainRadius)`; `chunkCount = ringSide * ringSide`; `foliageSlotIndex(chunkX, chunkZ, side)` wraps both
  axes by `side`. The loops run `offsetX, offsetZ` over `[0, ringSide)`.
- A slot whose offset is outside the current span (`offsetX >= columns || offsetZ >= rows`) is **left as it
  is** (no fill, no clear, no entity creation): every residue maps to a chunk beyond the draw distance, which
  the far fade and the CPU cut already hide. When `refillAll` is set, such a slot is cleared
  (`instances.clear()`, `needUpdateInstances`, `assigned = false`) so stale geometry cannot come back.
  (Doriax clears them every frame; keeping them avoids refilling the edge column when the span flips.)
- With Min Draw Distance (6.7): a chunk whose farthest XZ corner from `viewLocal` is nearer than the layer's
  near fade start is skipped (cleared once, `assigned = false`, no fill); it has nothing to draw.

### 6.7 Near fade, slack, billboards, G-buffer fade (engine, shaders)

Instanced Mesh fields (`engine/core/component/InstancedMeshComponent.h`, after `fadeEnd`):

```cpp
// Instances grow from nothing at nearFadeStart to full size at nearFadeEnd (model-space XZ distance);
// an empty range (nearFadeEnd <= nearFadeStart) turns it off. Needs distanceFade (the variant).
float nearFadeStart = 0;
float nearFadeEnd = 0;
// runtime: the near cut and eye the render lists were made with, and the eye movement they still cover
float lastFadeCullNear = 0;
float lastCullSlack = 0;
```

- Shared include `engine/shaderlib/includes/instance_fade.glsl`: declares `u_vs_fade { vec4 fadeRange; vec4
  fadeEye; }` (layout unchanged: `fadeRange = (start, end, nearStart, nearEnd)`) and
  `vec3 instanceFadePosition(vec3 origin, vec3 pos)` = far factor `clamp((end - d) / (end - start))` when
  `end > start`, times near factor `clamp((d - nearStart) / (nearEnd - nearStart))` when `nearEnd > nearStart`,
  `mix(origin, pos, factor)`. `mesh.vert`, `depth.vert`, `gbuffer.vert`, `includes/surface_vs.glsl` and
  `includes/surface_depth_vs.glsl` include it under `USE_INSTANCE_FADE` and drop their copies.
- `applyInstanceFadeUniform` (`RenderSystem.cpp:394`) writes `nearFadeStart, nearFadeEnd` into `fade[2], fade[3]`.
- G-buffer: `ShaderPool::getGBufferMeshProperties` gains `instanceFade` (bit 18, short name 'Ifd', long
  "Instance Fade" in `getShaderPropertyName`); `getSubmeshShaderVariant` passes `p_instanceFade`; `loadMesh` reads
  `slotVSGBufferFade` (new `Submesh` field, -1 by default, reset where `slotVSFade` is at `:10815`) and
  `drawMeshGBuffer` applies the fade block like the depth pass (`:9092`). `gbuffer.vert` declares the instance
  matrix origin as `depth.vert` does.
- `rendershared::instanceFadeCullEnd` drops the `scene.isSSREnabled()` exclusion. New
  `rendershared::instanceFadeCullNear(instmesh, mesh, scene)`: the same eligibility (built-in triangle shaders,
  no custom/fork shaders, `distanceFade`), then `nearFadeStart` when `nearFadeEnd > nearFadeStart`, finite and
  `>= 0`, else 0.
- `updateInstancedMesh` (`:13321`): `slackFar` and `slackNear` per decision 10; `cullRadius = cullEnd + slackFar
  + epsilon`; skip an instance when `d² < (cullNear - slackNear)²` (only when `cullNear > 0`); store
  `lastFadeCullEnd`, `lastFadeCullNear`, `lastFadeCullEye` and `lastCullSlack`, the smaller margin of the cuts
  in use (both: `min(slackNear, slackFar)`; near only: `slackNear`; far only: `slackFar`).
- `RenderSystem::update` (`:14690-14735`): `fadeEyeLocal` is updated when `distanceFade` (unchanged: near fade
  needs it too); `eyeMoved = dx² + dz² > lastCullSlack²`; `fadeSelectionChanged = cullEnd != lastFadeCullEnd ||
  cullNear != lastFadeCullNear || ((cullEnd >= 0 || cullNear > 0) && eyeMoved)`; the camera-move rebuild
  condition uses `cameraBound` (billboards and camera-dependent particles) instead of `particleCamera`.
- Billboard rotation, in `updateInstancedMesh` (`:13349`) and `buildInstanceList` (`:17454`), through one helper
  `Quaternion instanceBillboardRotation(const InstancedMeshComponent&, const Matrix4& cameraView, const
  Quaternion& entityWorldRotation)`: spherical as today; cylindrical: the camera's forward in the entity's local
  space, projected on XZ; when its length is below 1e-4 (camera straight above or below) keep the batch's last
  yaw (`InstancedMeshComponent::lastBillboardYaw`, runtime); else a yaw-only rotation facing it.
- `buildInstanceList` (`:17440-17530`) applies the same near cut.
- Inspector (`InspectorRenderers.cpp:657-661`): under **Distance Fade**, rows **Near Fade Start** and **Near Fade
  End** (FloatPositive), help "Instances grow from nothing at Near Fade Start to full size at Near Fade End,
  by horizontal distance in the mesh's local space. Use with Fade Start/End for a distance band (a far level of
  detail)." **Cylindrical Billboard** is disabled when **Billboard** is off, help "Turn about the mesh's up axis
  only (trees, grass cards)."
- Reflection (`RenderReflect.cpp:320-327`): `nearFadeStart`, `nearFadeEnd` (`.unit = Meters, .min = 0`), added to
  `kInstancedMeshKnownKeys` (`:586`). Serialization `StreamComponents.cpp:2113-2147` (encode always, decode with
  `decodeFinite` defaults); generated code `Factory.cpp` near `:3624`; `EntityCloner.cpp:341` resets the runtime
  fields. Lua (`GameObjectClassesLua.cpp` near `:1871`): `Mesh.nearFadeStart`, `Mesh.nearFadeEnd`
  (`Mesh::setNearFadeStart/getNearFadeStart/...` in `object/Mesh.{h,cpp}`, creating the Instanced Mesh like
  `setFadeStart`).

### 6.8 Foliage Min Draw Distance and Foliage Distance (engine, editor)

- `TerrainFoliageLayer::minDrawDistance = 0` (`TerrainComponent.h:154`, after `drawDistance`), comment "nearer
  instances fade out; with a nearer layer whose Draw Distance equals it, the two cross-fade".
- `updateFoliageLayer`: `scale = Engine::getFoliageDistanceScale()` (not in preview: 1);
  `drawDistance = max(0, layer.drawDistance * scale)`; when not preview and `drawDistance <= 0`,
  `destroyFoliageInstances(instances)` and return. `minDistance = clamp(layer.minDrawDistance * scale, 0,
  drawDistance)`. Per chunk: `instmesh.nearFadeEnd = preview ? 0 : minDistance; instmesh.nearFadeStart =
  minDistance * TERRAIN_FOLIAGE_FADE_START`.
- Engine (`engine/core/Engine.{h,cpp}`), mirroring Atmosphere Quality: `static void
  setFoliageDistanceScaleDefaults(float desktop, float mobile, float web)`, `getFoliageDistanceScaleDefault()`,
  `setFoliageDistanceScale(float)` (override; non-finite ignored with a warning like `setTimeScale`; clamped
  to `[0, 10]`), `getFoliageDistanceScale()` (override when set, else the platform default),
  `resetFoliageDistanceScale()`. Lua (`CoreClassesLua.cpp` near `:2209`): `Engine.foliageDistanceScale`
  (property) and `Engine.resetFoliageDistanceScale()`. Lua API data regenerates from the bindings
  (`tools/lua_api`); add the details to `manual.py` only if the generated detail is unclear.
- `editor::Project`: `std::array<float, 3> foliageDistanceScale` (default `{1, 1, 1}`), getter/setter that calls
  `Engine::setFoliageDistanceScaleDefaults`, saved in `project.yaml` next to `atmosphereQuality` (missing key:
  defaults), passed to the `Generator`, which emits `Engine::setFoliageDistanceScaleDefaults(…)` beside
  `setAtmosphereQualityDefaults` (`Generator.cpp:2585`). `Project::resetEngineConfigs` (`Project.cpp:5588`)
  calls `Engine::resetFoliageDistanceScale()`.
- Settings registry (`editor/settings/SettingsRows.cpp` near `:1669`): `quality.foliage_distance_desktop`,
  `_mobile`, `_web`, type number, range 0..10, description "Foliage Distance of <group>: multiplies every
  terrain foliage layer's Draw Distance and Min Draw Distance (Lua: Engine.foliageDistanceScale)", with the
  search row entries (`:2743`). Project Settings > Quality (`ProjectSettingsWindow.cpp:1972-1985`): three rows
  **Foliage Distance (Desktop/Mobile/Web)**, `DragFloat` 0..2 (typed values up to 10), reset to 1, following the
  Atmosphere rows' Apply/undo behaviour.
- Terrain window (`TerrainEditWindow.cpp:3462-3466`): the **Draw distance** row becomes **Draw range**, a
  `DragFloatRange2` (min, max) like the Height range row: min writes `minDrawDistance` (0..max), max writes
  `drawDistance` (>= 1), each through `setFoliageLayerProperty` (undoable). Help: "Instances show between these
  distances. A copy of the layer with a cheaper mesh whose Min equals this layer's Max takes over with a
  cross-fade. Scaled by Project Settings > Quality > Foliage Distance."
- Reflection (`RenderReflect.cpp:137`): `JE_FIELD(TerrainFoliageLayer, minDrawDistance, .updateFlags =
  kFoliage, .unit = Unit::Meters, .min = 0.0f)`. Serialization `StreamComponents.cpp:548,569`; generated code
  `Factory.cpp:1718`.

### 6.9 Shader loops (shaders, tools)

- `lighting2d.glsl`: both atlas reads `textureLod(…, 0.0)`; one loop `for (int t = -radius; t <= radius; ++t)` on
  every backend; `MAX_SHADOW2D_RADIUS = 6` on every backend (the `IS_HLSL` split and its comment go; the
  comment points at `shadows.glsl`'s rule).
- `ssao.frag`: every `u_depthTexture` read `textureLod(…, 0.0)` (the loop's at `:100` is the required one).
  `ssao_blur.frag:21` too (uniform loop, kept consistent).
- `lights.glsl`: `float lightShadowD(int index, vec3 worldPosition, vec3 worldNormal, vec3 dpdx, vec3 dpdy)`
  (the body of today's `lightShadow` after the derivatives); `lightShadow` keeps its signature and calls it
  with `dFdx/dFdy` (library shaders unchanged). Comment block at `:41-44` names both.
- `mesh.frag`: `vec3 shadowDpdx = dFdx(v_position), shadowDpdy = dFdy(v_position);` before the light loop
  (`#ifdef USE_SHADOWS`), and `lightShadowD(i, v_position, n, shadowDpdx, shadowDpdy)` inside the branch
  (`:454-456`).
- Lint `tools/check_shader_lod.py`: parses `engine/shaderlib/**/*.{glsl,frag,vert}` (comments stripped), builds
  the set of functions that call `texture`, `dFdx`, `dFdy` or `fwidth` (transitively, by name), and fails on
  any such call inside a `for`/`while` body that contains `continue` or `discard`, unless the line or the called
  function's definition line carries `// lod-safe: <reason>`. `terrain_fs.glsl:565` gets `// lod-safe:
  textureGrad with the flat derivatives taken before the loop`. Its test `tools/ci/test_check_shader_lod.py`
  (fixtures: a direct hit, a hit through a function, a marked line, a uniform loop without continue).
  `tools/check_changed.py` runs it when `engine/shaderlib/**` changes; `docs/testing.md` lists it under Tool
  checks; `.claude/rules/rendering.md` gets one line: "In a loop with a per-fragment exit, sample with
  textureLod/textureGrad and take derivatives before the loop (tools/check_shader_lod.py)."
- Shader cache: `editor/App.cpp:5365` v44 -> v45 with a comment line (lighting2d, ssao, mesh.frag shadows,
  instance fade include, G-buffer 'Ifd'). The lead serializes the number at merge if another branch bumps it.

### 6.10 Camera gizmo (editor) and near/far (engine)

- `static Vector2 RenderSystem::getCameraNearFar(const CameraComponent&)` (decision 11; ortho and UI unchanged).
  Used by `updateCamera`'s perspective branch (`:12524`), `RenderSystemShadows.cpp:663-664,681`, and the gizmo.
- `SceneRender::drawCameraFrustumLines(lines, camera, isMainCamera, bool full)`: near/far from
  `getCameraNearFar`, swapped if reversed; when not full: `depth = ortho ? max(|right-left|, |top-bottom|) / 2 :
  2`, `far = min(far, near + depth)`. The corner math moves to a pure function
  `std::array<Vector3, 8> editor::cameraGizmoCorners(const CameraComponent&, bool full)` (in
  `editor/render/SceneRender.h`, unit-tested).
- `CameraObjects` gains `bool fullFrustum`; `updateCameraFrustum` redraws when it changes.
- `SceneRender3D::update` (`:1447`) and `SceneRender2D::update` (`:1194`) pass `full = isSelected` (the
  entity is in `selEntities`), as the spot-light cones do (`createSpotLightCones(…, isSelected)`).

### 6.11 Vulkan robustness (editor, platform)

- New header `engine/platform/common/VulkanFeatures.h` (inline, no state): `struct VulkanRobustness { bool
  enable; const char* reason; };` and `VulkanRobustness decideRobustBufferAccess(VkBool32 supported, const
  VkPhysicalDeviceDescriptorBufferPropertiesEXT& props)` (reasons: "enabled", "not supported by the device",
  "robust descriptor sizes differ from the ones sokol writes").
- `RendererVulkan.cpp` (`:211-249`): query the descriptor buffer properties with
  `vkGetPhysicalDeviceProperties2`, call the decision, set `requiredFeatures.features.robustBufferAccess`, log
  `Log::info("Vulkan robust buffer access: %s", reason)`, and `SystemRender::setVulkanRobustBufferAccess(bool)`
  (new, beside `setVulkanHostQueryReset`, `engine/core/render/SystemRender.h:155`; getter returns
  `std::optional<bool>`-like tri-state: unknown until set).
- `get_render_stats` (`editor/mcp/tools/ProfilerTools.cpp:173-175`): `gpu.robust_buffer_access` (bool, null when
  not Vulkan or unknown). Output-only; regenerate `docs/mcp/TOOLS.md` if outputs are documented there
  (`gen_tool_docs.py --check`); `tool_budget.py --check` (descriptions unchanged).
- Players (`VulkanContext.cpp`) do not call it (section 2). Windows and macOS code is untouched; nothing here
  needs a platform this host cannot build.

### 6.12 Docs

- `docs/mcp/guide/shaders.md:56`: Points auto depth write follows the queue.
- `docs/mcp/guide/scene-view.md` (Camera section) and `docs/editor/scene-view.md`: selected camera full frustum,
  compact others.
- `docs/camera.md`: perspective near/far clamp.
- New `docs/rendering/instancing.md`: distance fade (far and near), slack, billboard modes, fade in every pass,
  foliage Draw range and cross-fade recipe, Foliage Distance quality and `Engine.foliageDistanceScale`. The
  terrain foliage guide page of `agent-authoring` (or a new `docs/mcp/guide/terrain.md` if it lands first)
  gets a short "Draw range and Foliage Distance" paragraph linking it.
- `docs/unity-glossary.md`: `QualitySettings.terrainDetailDistanceFactor` -> `Engine.foliageDistanceScale` /
  Project Settings > Quality > Foliage Distance; billboard renderer modes -> Billboard / Cylindrical Billboard.
- `docs/testing.md`: the new suites and the shader lint.
- `docs/rendering/shadows.md`: the receiver-plane derivatives rule (`lightShadowD`), and 2D PCF radius 6 on D3D11.
- No change to the agent guide's digest (byte budget).

## 7. Behaviour rules and edge cases

1. A mesh with no float POSITION in any submesh keeps `AABB::ZERO` (and is recomputed as today). A mesh whose
   vertices are all at one point has a zero-size box there.
2. A skinned mesh's `bonesAABB` are unaffected (already null-seeded).
3. A mesh without normals and with tangents takes neither 'Nor' nor 'Tan'; with a normal map it shades with the
   derivative tangent frame (`pbr.glsl:212-235`).
4. Under 2D lights only, a mesh with normals, tangents and a normal map keeps 'Tan'; without the map it drops
   it. In a scene with 3D lights too, the lit path keeps 'Tan'.
5. Points: Depth Write On/Off and manifest `zwrite=` are never overridden. A texture gaining or losing alpha
   with Auto Transparency, or the Transparent flag toggled, reloads the component once (next frame), only
   when Depth Write is Auto. UI Points (under a RectTransform) follow the same rule (no depth in UI passes).
6. A texture edit on a manifest submesh keeps today's path (its UV sets come from `@require`). A failing custom
   shader that fell back to the built-in keeps its asked-for properties, so the comparison does not loop.
7. A texture edit while the mesh is not loaded changes nothing here (the load resolves the variant).
8. The GL clamp only lowers counts; a driver reporting the full size changes nothing. Non-GL backends untouched.
9. Foliage ring: terrain edges clamp `ringSide` to `2 * terrainRadius`; the preview covers the whole terrain;
   a Draw Distance, Foliage Distance or chunk size change resizes the ring once (refill all, as today).
10. Near fade: `nearFadeEnd <= nearFadeStart`, negative or non-finite values turn it off; it needs Distance
    Fade on; a near range beyond the far range draws nothing (documented, not refused). Shaders forked before
    this change ignore `fadeRange.zw`, and the CPU near cut is off for custom shaders, so such a fork draws its
    instances inside the near range at full size (documented: re-fork).
11. Foliage Distance 0 destroys the layers' chunks (no entities, no draws); going back above 0 recreates them
    over frames (2 mesh loads per frame, as today). The editor's views use the desktop default outside Play.
12. Min Draw Distance >= Draw Distance (after scaling) is clamped to Draw Distance: the layer draws nothing.
13. Cylindrical billboard with the camera straight above keeps the last yaw; spherical is unchanged.
14. Slack: a camera that moves less than the slack in XZ never rebuilds; any change of the fade or near ranges
    rebuilds at once.
15. Gizmo: reversed near/far are swapped for drawing only; an ortho camera with zero width and height draws a
    compact depth of 0 (just the near rectangle), never a NaN.
16. Perspective near <= 0 renders with 0.001; the component's value is not rewritten (the Inspector shows what
    was typed).
17. Vulkan: a device without the feature, or with differing robust sizes, creates the device exactly as today.

## 8. Performance and cost

- Bounds, normals, 2D tangents: no runtime cost; normal-less meshes and 2D-only tangent meshes get smaller
  variants (fewer attributes and varyings).
- Points: one bool compare per Points component per frame; a reload only on an effective transparency change.
- Variant re-resolve: runs only for meshes with a texture edit that frame (a manifest lookup and a variant
  computation per submesh); zero on other frames.
- GL clamp: one `glGetUniformIndices` + `glGetActiveUniformsiv` per array uniform at shader creation; nothing
  per draw.
- Foliage ring: fewer entity creations and refills while walking (the point of the fix); out-of-span slots cost
  nothing per frame.
- Slack: fewer instance list and buffer rebuilds while moving (Doriax measured none; expected from the
  arithmetic: a 0.5 m slack at the default 50 m foliage). Instances in the slack ring are drawn as degenerate
  vertices; bounded by a ring of width `slackFar`.
- Near fade: in `USE_INSTANCE_FADE` variants only, a few ALU per vertex and one uniform comparison; no new
  variant, uniform or binding. Off (empty range) is the same branch as today's empty far range.
- G-buffer fade: only with SSR on and Distance Fade on; and the CPU cut now works under SSR, so SSR scenes draw
  fewer instances.
- Foliage Distance: a multiply per layer per frame. Below 1 it saves CPU fill, memory and draws on mobile.
- Shader loops: `textureLod` is never slower than `texture`; D3D11 2D shadows at High take 13 taps instead of 5
  (the quality the scene asked for; Low is unchanged).
- Vulkan robustness: editor only; drivers implement it in hardware or with bounds checks on buffer access.

## 9. Compatibility

- No file format change and no version bump: new fields (`InstancedMeshComponent.nearFadeStart/nearFadeEnd`,
  `TerrainFoliageLayer.minDrawDistance`, `project.yaml` foliage distance per group) have defaults that keep
  today's result (0, 0, 0 and 1.0) and are added to the known-key lists, so `check_stream_keys.py` and the
  unknown-data preservation stay green. `editor/tests/reflect/catalog_snapshot.json` is regenerated.
- Visible changes in existing projects, all intended (listed in the merge's ledger entry):
  - Offset meshes: tighter bounds (culling, selection, picking); an offset Water mesh's level moves to its real
    surface.
  - Normal-less meshes: draw lit with flat normals instead of black/garbage.
  - Transparent Points: no depth write (set Depth Write On to get the old behaviour).
  - Faded instances under SSR: gone from reflections.
  - Instanced billboards with Cylindrical Billboard: turn about their up axis only.
  - D3D11: 2D shadows at Medium/High get their full softness.
- Saved shader variant collections and warm-up lists: stale built-in `Ult+L2d+Tan` keys normalize (decision
  14); stale 'Nor' keys of normal-less meshes still compile an unused variant once (export-pipeline's stale-key
  warning reports them).
- Shader cache v45 (6.9): the first open recompiles.

## 10. Tests

Suites per `docs/testing.md`. "Red" = fails on `main` without the change (the run to record as the red control).

| # | Suite / file | Test | Asserts | Red |
|---|---|---|---|---|
| T1 | editor-tests `engine/subsystem/GltfMeshOffsetTest.cpp` | `MeshUnderAnotherSceneRoot` (changed), `NestedMeshNodeKeepsItsTransforms` (min added) | minimum `(-6, 0, 3)` / `(0, 2, 6)` | yes |
| T2 | core-tests `subsystem/MeshBoundsTest.cpp` (new) | `OffsetMeshBoundsHugTheVertices`, `EmptyMeshKeepsZero`, `UiQuadBoundsHugTheVertices` | a Mesh with vertices in [10,11]³ has that box; no positions -> `AABB::ZERO`; a UI buffer at x in [5,9] -> min x 5 | first and third yes |
| T3 | core-tests `render/MeshInputsGpuTest.cpp` (new, glcore + Vulkan) | `ANormallessMeshFeedsEveryInput` (Lit pixel, vertex, mixed lighting; IBL only; 2D lights; with and without tangents; a morph mesh without normals) | `ObjectRender::getUnfedVertexInputLoads()` unchanged after load and draw; the draw is not black (mean luminance above a floor) | yes (unfed count rises) |
| T4 | core-tests `render/MeshInputsGpuTest.cpp` | `TwoDLightsTakeTangentsOnlyWithANormalMap` | the submesh's `shaderProperties` lacks 'Tan' without a normal map, has it with one | yes |
| T5 | core-tests `shader/ShaderVariantCollectionTest.cpp` | `NormalizeDropsStaleTwoDTangents` | `Ult+L2d+Tan` id 0 -> no 'Tan'; with 'Nmp' kept; custom id kept | yes |
| T6 | core-tests `render/PointsDepthWriteGpuTest.cpp` (new) | `TransparentPointsDoNotHideWhatIsDrawnAfter` | camera `transparentSort` off; transparent Points A (half-transparent texture) in front, then Points B behind: a pixel in A's transparent corner shows B's colour | yes |
| T7 | same | `DepthWriteOnKeepsWriting`, `ATextureGainingAlphaMovesThePointsToTheTransparentQueue` | explicit On hides B; swapping to an alpha texture flips `isTransparent()` and reloads once | first no (guard), second yes |
| T8 | core-tests `render/MaterialVariantGpuTest.cpp` (new, model: `TextureLobeMapGpuTest.cpp`) | `GBufferFollowsATextureGivenToALoadedMaterial` (SSR on, mesh with UVs and a normal map loaded, then a base colour and an MR texture) | after the swap frames, `gbufferShaderProperties` has 'Bct' and 'Mrt'; a G-buffer readback (`capture_pass_output` path or `Readback.h`) shows the MR texture's roughness | yes |
| T9 | same | `DepthClipFollowsABaseColourGivenToAMaskMaterial`, `NoReloadFlash` | depth variant gains 'Tex' after the edit; the mesh draws on every frame of the swap (no frame without it) | first yes |
| T10 | core-tests `render/SokolGlUniformArrayTest.cpp` (new, glcore only, skipped elsewhere; model `SokolGlTextureLevelsTest.cpp`) | `AFlattenedArrayWithAnUnusedTailUploadsItsHead` | shader with `uniform ivec4 u_blk[2]` reading only `u_blk[0]`: `sgj_gl_uniform_count` equals the driver's `GL_UNIFORM_SIZE`; `glGetError()` clean after apply+draw; the output is `u_blk[0]`'s value | yes if llvmpipe trims the array (Mesa's linker sizes uniform arrays to the highest constant index; the implementer records the measured size). If llvmpipe reports 2, this has no red control on the CI renderers: record it in the spec's Deviations |
| T11 | core-tests `render/MeshInputsGpuTest.cpp` | `TwoUvSetsWithoutANormalMapSampleTheirSets` | base colour on UV set 2, MR on set 1, no normal map: pixels match each texture's set; no GL error | only on a driver that rejects the upload |
| T12 | editor-tests `engine/subsystem/TerrainFoliageRingTest.cpp` (new) | `WalkingKeepsTheChunkEntities` | terrain + one foliage layer (Draw Distance 50, a one-triangle glTF written by the test as `GltfMeshOffsetTest` does); main camera walked x = 0..64 in 1 m steps: `getFoliageEntities` returns the same entity set at every step after warm-up | yes (16/20/25 resizes) |
| T13 | same | `MinDrawDistanceSetsTheNearFade`, `FoliageDistanceScalesBothDistances`, `ZeroScaleRemovesTheLayer`, `PreviewIgnoresTheScale` | chunk Instanced Mesh `nearFadeStart/End`, `fadeEnd` = scaled values; scale 0 -> no entities | new behaviour |
| T14 | core-tests `render/InstancedFadeTest.cpp` (new, CPU, RenderSystem update without draws where possible, else GPU env) | `AnOpaqueBillboardFollowsTheCameraRotation` | rotate the main camera only: `renderInstances[0]` rotation equals the camera's | yes |
| T15 | same | `CylindricalBillboardTurnsAboutUpOnly`, `CameraStraightAboveKeepsTheYaw` | instance matrix's up column stays the mesh up; yaw faces the camera | yes |
| T16 | same | `SubMarginMovesDoNotRebuild`, `PastTheMarginRebuilds` | eye moved 0.1 m: `lastFadeCullEye` unchanged; 1 m: changed | yes |
| T17 | same | `NearFadeCutsAndShrinks`, `FadeAppliesUnderSsr` | instances nearer than `nearFadeStart - slack` absent from `renderInstances`; SSR on keeps the CPU cut; GBUFFER variant has 'Ifd' | new / yes |
| T18 | core-tests `render/RenderGpuTest.cpp` golden (glcore; Vulkan by `GRAPHIC_BACKEND=vulkan`) | `InstanceNearFadeBand` | a row of instances across a near band: golden of the shrink-in | new |
| T19 | tools `tools/ci/test_check_shader_lod.py` + `tools/check_shader_lod.py` on the tree | fixtures (6.9); the tree passes | the tree fails before the shader edits (`lighting2d.glsl:202`, `ssao.frag:100`) | yes |
| T20 | core-tests 2D light goldens (existing `htmlui-tests`/core suites drawing Light 2D shadows; `engine/tests/htmlui/TestLight2DNormalMaps.cpp` area) | rerun | unchanged pixels on glcore and Vulkan (the lod change is exact on single-level atlases) | no (guard) |
| T21 | editor-tests `sceneview/SceneViewMathTest.cpp` | `OrthoCameraGizmoHasDepth`, `SelectedCameraShowsTheFullFrustum`, `PerspectiveNearIsClamped` | `cameraGizmoCorners` for ortho width 20: depth 10; full: far plane at `far`; `getCameraNearFar(near = 0)` -> 0.001 | first yes (depth 2) |
| T22 | MCP `tools/mcp/tests/test_render_correctness.py` (new) | `test_foliage_quality_settings`, `test_near_fade_properties`, `test_points_depth_write_hint`, `test_render_stats_robustness_field` | `set_project_settings` round-trips `quality.foliage_distance_*` (undo per the settings rules), `eval_lua "Engine.foliageDistanceScale"` in Play reads the platform default and a set value, resets after Stop; `set_properties` on `nearFadeStart`, `minDrawDistance` with undo; `get_render_stats` has `gpu.robust_buffer_access` (null on glcore) | new |
| T23 | editor-tests `reflect` catalog snapshot, `tools/check_stream_keys.py`, `tools/lua_api` doc test | regenerate | new fields present, keys known, Lua blocks of the docs run | guard |

Screenshots (evidence, outside the repo, from an isolated editor): an ortho camera unselected and selected; a
foliage field with a two-layer cross-fade; snow Points before/after.

Builds to run before landing (rendering rule): `tools/build.sh linux`, `unit-tests` (glcore and
`GRAPHIC_BACKEND=vulkan`), `htmlui-tests`, `web`, `android`, `tools/smoke-test.sh`, `tools/check-changed`.
D3D11, Metal and the Windows/macOS/iOS paths cannot be built on this host: the shader edits go through the
same cross-compilation as today; the HLSL loop change follows the 3D shadows' existing pattern
(`shadows.glsl:22-26`), recorded as inferred, not run.

## 11. Acceptance criteria

1. T1-T2 pass; on `main` T1 and `OffsetMeshBoundsHugTheVertices` fail (recorded red run).
2. No load in T3 increases `getUnfedVertexInputLoads()`, on glcore and Vulkan; on `main` it increases.
3. T4, T5 pass; a 2D-lit mesh with tangents and no normal map has no 'Tan'.
4. T6 shows B through A's transparent corner; T7's explicit On still hides B.
5. T8-T9 pass: after a texture edit with SSR on, the G-buffer variant has the new bits and the G-buffer readback
   follows the texture; no frame drops the mesh.
6. T10 passes on glcore; the measured `GL_UNIFORM_SIZE` on llvmpipe is written in the PR; T11 passes on glcore
   and Vulkan.
7. T12 passes and fails on `main`; T13 passes.
8. T14-T17 pass; T14-T16 fail on `main`.
9. T18 golden reviewed (glcore) and Vulkan within tolerance.
10. `python3 tools/check_shader_lod.py` passes on the tree and T19 passes; it reports the two sites on `main`.
11. T20 goldens unchanged.
12. T21 passes; screenshots of an ortho camera (unselected compact box, selected full volume) are attached.
13. T22 passes; `docs/mcp/TOOLS.md` is current (`gen_tool_docs.py --check`), `tool_budget.py --check` passes.
14. The Inspector shows Near Fade Start/End under Distance Fade with undo; the Terrain window shows Draw range;
    Project Settings > Quality shows Foliage Distance (Desktop/Mobile/Web); each edit undoes.
15. `Engine.foliageDistanceScale` works in Lua in Play and in an exported Linux and web build (smoke test scene
    with a terrain layer, value read back by script).
16. A Vulkan editor logs the robustness decision once and `get_render_stats` reports it.
17. `tools/build.sh web` and `android` build; `tools/smoke-test.sh` passes; `tools/check-changed` passes.
18. Docs of 6.12 updated; `tools/check_agent_docs.py` passes (rendering rule line added).

## 12. Files likely touched

- Engine: `engine/core/subsystem/MeshSystem.cpp` (calculateMeshAABB, updateFoliageLayer, foliageSlotIndex),
  `engine/core/subsystem/RenderSystem.cpp` (getSubmeshShaderVariant, loadPoints/drawPoints/update points,
  texture-edit branch, meshVariantShadersReady, updateInstancedMesh, update's instanced block,
  buildInstanceList, applyInstanceFadeUniform, loadMesh/drawMeshGBuffer fade slot, updateCamera),
  `engine/core/subsystem/RenderSystem.h`, `RenderSystemShared.h`, `RenderSystemShadows.cpp` (near/far),
  `engine/core/subsystem/UISystem.cpp`, `engine/core/pool/ShaderPool.{h,cpp}`,
  `engine/core/component/{InstancedMeshComponent,PointsComponent,TerrainComponent,MeshComponent}.h`
  (Submesh `slotVSGBufferFade`), `engine/core/registry/EntityCloner.cpp`, `engine/core/object/Mesh.{h,cpp}`,
  `engine/core/Engine.{h,cpp}`, `engine/core/render/SystemRender.{h,cpp}`,
  `engine/core/script/binding/{GameObjectClassesLua,CoreClassesLua}.cpp`.
- Shaders: `engine/shaderlib/{mesh.vert,mesh.frag,depth.vert,gbuffer.vert,ssao.frag,ssao_blur.frag}`,
  `engine/shaderlib/includes/{lighting2d,lights,surface_vs,surface_depth_vs,terrain_fs,instance_fade(new)}.glsl`;
  generated `engine/shaders/*.h` if the build regenerates them in-tree.
- sokol: `engine/libs/sokol/sokol_gfx.h`, `engine/libs/sokol/PATCHES.md`.
- Platform: `engine/platform/common/VulkanFeatures.h` (new), `editor/backend/renderer/RendererVulkan.cpp`.
- Editor: `editor/render/SceneRender.{h,cpp}`, `SceneRender3D.cpp`, `SceneRender2D.cpp`,
  `editor/window/InspectorRenderers.cpp`, `editor/window/TerrainEditWindow.cpp`,
  `editor/window/dialog/ProjectSettingsWindow.cpp`, `editor/settings/SettingsRows.cpp`,
  `editor/reflect/components/RenderReflect.cpp`, `editor/StreamComponents.cpp`, project.yaml stream
  (`editor/Stream*.cpp`), `editor/Factory.cpp`, `editor/Generator.{h,cpp}`, `editor/Project.{h,cpp}`,
  `editor/App.cpp` (cache version), `editor/mcp/tools/ProfilerTools.cpp`.
- Tests: see section 10; `editor/tests/reflect/catalog_snapshot.json`.
- Tools/docs: `tools/check_shader_lod.py` (new), `tools/ci/test_check_shader_lod.py` (new),
  `tools/check_changed.py`, `.claude/rules/rendering.md`, docs of 6.12, `docs/testing.md`.

Merge hotspots: `RenderSystem.cpp` (update loop, loadMesh, updateInstancedMesh), `MeshSystem.cpp`
(calculateMeshAABB, foliage), `editor/App.cpp` shader cache line, `GameObjectClassesLua.cpp`,
`RenderReflect.cpp`, `StreamComponents.cpp`, `Factory.cpp`, `catalog_snapshot.json`.

## 13. Cross-feature dependencies

- `mesh-memory`: reshapes `MeshComponent`/`Submesh` storage. This feature adds one `Submesh` field
  (`slotVSGBufferFade`) and reads `Submesh` variant fields in `loadedVariantMatches`; whoever lands second
  rebases onto the other's layout. No dependency in either direction otherwise.
- `draw-perf`: edits the same `RenderSystem::update` instanced block, culling and `calculateMeshAABB`'s bone
  scan ("no bone-bounds scan on rigid meshes"). Land order free; the second rebases. draw-perf's "no per-frame
  scans for features that are off" may change how `updateTerrainFoliage` is called; the ring logic of 6.6 is
  independent of how often it runs.
- `ssr-probes`: works on the SSR composite and G-buffer use; this feature adds the G-buffer 'Ifd' bit and the
  variant re-resolve on texture edits. Coordinate the GBUFFER property bit numbers (this takes bit 18; ssr-probes has 16 and 17) and the
  shader cache version.
- `water-reflections` / `water-boats`: Water level comes from the mesh bounds (`Water.cpp:144`); after 6.1 an
  offset water mesh's level is its real surface. They also bump the shader cache. Tell them; no API change.
- `sprite-batching`: touches sprite variants and possibly `UISystem` bounds; the 2D tangent rule (6.2) and
  `calculateUIAABB` (6.1) are small and merge mechanically.
- `export-pipeline`: owns shader keys and stale-key warnings; `normalizeKey`'s new rule (6.2) and the
  removed 'Nor' bit for normal-less meshes produce stale keys it should warn about. The export shader list
  follows `getSubmeshShaderVariant`, so no export change here.
- `agent-authoring`: owns the validated foliage-layer authoring tool and the terrain guide page: it must
  accept and report `minDrawDistance`; whoever lands second adds the field to the other's surface. Its "editor
  camera degeneracy" work may reuse `RenderSystem::getCameraNearFar`.
- `scripting`: edits `GameObjectClassesLua.cpp` / Lua API data; mechanical merge.
- `code-editor-git`: scene-level default shader uniforms touch component uniform paths, not Points depth state;
  no dependency.
- No dependency on the other features.

## 14. Deviations

Recorded during implementation (branch `dc/render-correctness`).

1. **Polygon extents (added).** `UISystem::createUIPolygon` and `MeshSystem::createMeshPolygon` measured a
   polygon's extent from `numeric_limits<float>::min()` (the smallest positive float) as the start of the
   maximum, so a polygon left of or below its origin got texture coordinates and a width measured to 0. Same
   class as 6.1 (bounds seeded at the origin); fixed with `lowest()`, tested by
   `MeshBounds.PolygonLeftOfItsOriginMeasuresItsOwnExtent`.
2. **Morph inputs (6.2).** Not gated: glTF requires a morph target's attributes to exist on the base primitive,
   and the unlit path already builds `HAS_MORPHNORMAL` without `HAS_NORMALS` with `bindUnfedMorphInputs`. T3 has
   no normal-less morph case and no IBL-only case (an IBL-only scene needs a baked environment the test
   fixtures do not have); the lit pixel, vertex, mixed and 2D light cases, with and without tangents, cover the
   variant code paths that changed.
3. **Points (6.3).** The Inspector's Depth Write Auto label resolves the Points rule too (`Auto (Off)` on
   transparent Points), in `InspectorShaderLibrary.cpp`'s `resolveComponentAuto`; `PointsComponent` keeps a
   runtime `depthFollowsQueue` beside `loadedTransparent`. T7's first case (Depth Write On) is a guard, not a
   red control.
4. **Variant re-resolve (6.4).** `getCurrentSubmeshInputs` is the extracted helper (the spec's
   `getCurrentSubmeshVariant` would have had no caller beside it). Presence of a pass the variant no longer
   builds is not a mismatch (`loadMesh` keeps the G-buffer handle when SSR goes off; comparing presence would
   reload on every later texture edit). `pbrVariantMatches` had no other caller and is removed. T8 asserts the
   G-buffer variant bits ('Bct', 'Mrt') instead of a G-buffer readback: the bits decide which slots
   `loadGBufferTextures` binds, and the SSR suites already check what a bound map draws.
5. **GL uniform count (6.5).** The test hook is `sgj_query_gl_uniform_count` (the spec's name was
   `sgj_gl_uniform_count`). The measured `GL_UNIFORM_SIZE` of llvmpipe and the red control status are in the
   final report of the branch.
6. **Foliage (6.6, 6.8).** The Foliage Distance rows are `DragFloat` 0-10 (typed and dragged, clamped); the
   value is written to `project.yaml` only away from its default and carried by the prebuilt player's
   `game.json` (`PlayerConfig::foliageDistanceScale`, `PlayerMain`), which the spec did not list.
   `Engine.foliageDistanceScale` and `resetFoliageDistanceScale` are shader-neutral for the prebuilt export's
   module scan (`ShaderNeutralApi`). The foliage tests are core GPU suites (`TerrainFoliageGpu`), not editor
   tests: the ring needs loaded terrains and chunk entities.
7. **Near fade test (T18).** A pixel-coverage check (`InstancedFadeGpu.TheNearBandShrinksTheInstance`: an
   instance half way through the band covers between a tenth and a half of its full area) replaces the golden:
   it holds on every renderer, so it runs on glcore and Vulkan without per-renderer images.
8. **Camera gizmo (6.10).** `cameraGizmoCorners` is a free function in `editor/render/SceneRender.h`; the tests
   are `editor/tests/sceneview/CameraGizmoTest.cpp`. The shadow light cameras already clamped near and far
   (`RenderSystemShadows.cpp:665-666`), so only `updateCamera` takes `getCameraNearFar`. Main's gizmo has no
   pure function to test against, so `OrthoCameraGizmoHasDepth` has no red run; main's `far = near + 2` is
   read at `SceneRender.cpp:3903-3905`.
9. **Vulkan (6.11).** The decision is `engine/core/render/VulkanRobustness.h` (`decideVulkanRobustBufferAccess`,
   plain integers, unit-tested without a device) instead of `engine/platform/common/VulkanFeatures.h`: the editor
   reaches `engine/core/render` headers already. It is logged to stderr with the editor's other Vulkan device
   messages. `get_render_stats` returns `gpu.robust_buffer_access`; the tool's description is unchanged (the lean
   MCP rules: no result field lists in descriptions).
10. **Docs (6.12).** Under the lean MCP rules (new guide text needs an eval case) the new material is in user
    docs: `docs/rendering/instancing.md` (fades, billboards, foliage Draw range, Foliage Distance),
    `docs/editor/scene-view.md` (camera gizmo, near clamp), `docs/rendering/shadows.md` (`lightShadowD`, 2D PCF);
    `docs/mcp/guide/shaders.md` gets only the corrected Points sentence; no terrain guide page is created
    (agent-authoring owns the foliage guide). `docs/camera.md` is about shake and follow, so the near clamp is in
    the Scene view doc.
11. **MCP test (T22).** No Points depth-write hint test: the Auto label is Inspector text with no MCP surface;
    `PointsDepthWriteGpu` covers the behaviour.
12. **Merge with S54, S55, S57 (main, 2026-11-05).** S57's capsule and contact shadows wrapped
    `lightShadow` in `LIGHT_OCCLUSION`; the merged `includes/lights.glsl` has `lightShadowMapD` (the shadow
    map's term with given derivatives), `lightShadowD` (times the light's occlusion under `LIGHT_OCCLUSION`) and
    `lightShadow` (derivatives taken, then `lightShadowD`), so the hoisted derivatives of `mesh.frag` keep the
    occlusion. S54/S55's instance matrices carry the LOD fade in the first column's w; the instance fade reads
    only `i_matrix_col4`, so they compose. The shader cache is v50 (ssr-probes, merged first, took v49 and
    GBUFFER bits 16 'Occ' and 17 'Uv2'); 'Ifd' is GBUFFER bit 18 (lead decision).
13. **Texture-edit swap (6.4, T9).** The swap reloads the mesh as every variant change does: the reload's
    load lands a frame or more later (its queued `changeLoaded`, its textures), so T9's "no frame without the
    mesh" is not asserted; the tests assert the settled variants. A terrain keeps main's checks (its base material's UV
    sets and normal map, beside its layers' surface and hex tiling): their reload is what builds its layer image
    array again after `invalidateTerrainDetailArray` (`TextureRebindGpu.ATerrainsLayerImageBindsANewImage` failed
    without it, and passes on main).
14. **Zero-cost list.** `zero_cost_mesh_variants.txt` is rewritten: the normal-less setup's variants lose
    'Nor' (6.2) and their fragment stages turn the derived normal toward the eye (deviation 16), the 'Ifd'
    vertex stages include the near fade, the 'Shw' fragment stages take the shadow derivatives before the light
    loop; no other variant's preprocessed source changed.
15. **Points tests (T6, T7).** The tests turn transparency on with the Transparent flag. Auto Transparency did
    not find the alpha of the tests' 4x4 PNG images (`isTransparent()` stayed false with or without the fix,
    after the image loaded); the derivation in `drawPoints` is kept, but no test covers a texture swap.
16. **Review round (2026-11-06).** Flat normals (6.2): `cross(dFdx, dFdy)` takes its sign from the framebuffer's
    y, which differs between backends, so `pbr.glsl` turns the derived normal toward the eye and does not flip
    it for back faces (the projected decal's rule); `MeshInputsGpu` runs without ambient light and checks a
    light in front, a light behind and a double-sided back face. Foliage (6.8): a slot past the span kept its
    old chunk, instances and fade when the scaled distances changed (doubled foliage); the layer keeps the
    distances it filled for and refills every slot on a change, from a script or Project Settings
    (`TerrainFoliageGpu.ChangingTheScaleRefillsEverySlot`). Cylindrical billboards (6.7) take their yaw with
    the entity's scale applied (`atan2(sx * bx, sz * bz)`: the face's normal turns by the inverse scale).
    Near clip (6.10): clamped only at 0 or below. Texture edits (6.4, 8): a render target's image made again,
    seen by a Before pass, is a rebind (`Submesh::needRebindTexture`), not an edit; so is a stand-in or a
    rebuilt image found by the update; neither runs the variant check (`RenderSystem::getTextureVariantChecks`,
    asserted by `TextureRebuiltGpu`). The lint (6.9) also covers `textureOffset`/`textureProj`, macros, loops
    without braces and `if` branches on a varying, gl_FragCoord, gl_FrontFacing, gl_PointCoord or a sample
    inside loops (a `break` or `return` under one is an exit); a local computed from them is not seen.
    `SokolGlUniformArray` asserts the uploaded count only: a driver that clamps silently raises no GL error, so
    a `glGetError` check could not fail.

## Follow-ups (2026-11-07)

1. **The LOD lint follows per-fragment locals** (`tools/check_shader_lod.py`). A condition is per-fragment
   when it reads a variable its function assigns from a varying, gl_FragCoord, gl_FrontFacing,
   gl_PointCoord, a sample or another such variable (any order in the function, since a loop carries a
   value back to its top); a loop whose own condition is per-fragment (`for`, `while`, a `do`'s trailer)
   has a per-fragment exit. Parameters are not followed: a helper's caller decides what they hold, and
   treating every parameter as per-fragment flags nothing more in the library today (measured). Tests:
   `test_a_branch_on_a_per_fragment_local_is_flagged`, `test_a_loop_condition_on_a_per_fragment_local_is_flagged`
   (both fail against the old lint), `test_a_local_of_uniform_values_passes`.
2. **Real hits: none.** In `engine/shaderlib` 16 loops gain a per-fragment branch through a local
   (mesh.frag's light loop on `NdotL`, ssr.frag, the volumetrics, terrain, water, ssao and others) and
   none takes an implicit derivative there: render-correctness' 6.9 fixes already moved those to
   `textureLod`/`textureGrad` and derivatives before the loop. No loop of the library has a
   per-fragment condition. No shader changed, so no shader cache bump.
