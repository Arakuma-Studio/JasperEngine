# S10 — Shadows: stable CSM, cascade blend/fade, receiver-plane bias, PCSS, static shadow caching

Wave 2. Owner stream: rendering. Global decisions in `README.md` apply.

## 1. Goal

Directional shadows must stop shimmering and stop showing seams. The cascades should fit
bounding spheres snapped to the texel grid, blend into each other and fade out at a real
shadow distance. Bias should follow the receiver plane and the shadow texel size instead of
hand tuning. Soft shadows should harden near contact (PCSS) on desktop, with a cheap
fallback that looks consistent on mobile and web. Shadows from static casters are cached
and redrawn only when something they depend on changes. On mobile that is the largest
shadow saving available. Spot and point lights use the same bias, filter and caching
rules. Quality is set by per-platform tiers and by per-light settings with Unity names,
and it is the same through the Inspector, Lua and MCP on every sokol backend.

## 2. Scope

**In scope**
- Directional cascades: bounding-sphere fit, texel snapping, a rotation-only light basis,
  a shadow distance, Auto (log-lambda) or Manual splits, cascade blend band (dither on
  Low), distance fade, caster-bounded depth range, and fit freeze for caching.
- Receiver side, all projective and point lights: shadow coordinates computed in the
  fragment stage, a depth bias and normal offset in texel units, receiver-plane depth
  bias, a Hard / Soft filter per light, rotated Vogel PCF, PCSS (Ultra tier) and the PCF
  fallback, plus shadow strength.
- Point lights move to the same depth-only comparison atlas and filter as spots. Faces
  get a guard band, per-face culling and per-light sub-rect resolution. Slots are given
  out by importance with hysteresis.
- Static shadow caching: per-slot skip, a static depth layer plus dynamic dirty-rect
  restore, and the invalidation rules.
- Quality tiers per platform, scene override, debug views, Inspector, Lua, MCP, docs,
  tests, golden images and cost budgets.

**Out of scope**
- Ray-traced shadows (do-not-copy list).
- SDSM depth readback ("Close" projection: GPU→CPU readback, compute).
- MSM/EVSM/DPCF filter families.
- Coloured translucent shadows.
- Screen-space contact shadows: possible later work, Apache-2.0 Bend code would need a
  fragment port and a separate spec.
- Terrain clearance maps.
- Virtual or scrolling shadow maps.
- The min/max pyramid as a required path (see R14).
- Raising `MAX_SHADOW_ATLAS_SLOTS` above 9 (Risks).
- 2D shadows (`shadow2d.*`, `lighting2d.glsl`) are untouched.

## 3. Current state (verified)

**Cascade fit.**
- `RenderSystem::getDirLightProjection` (`engine/core/subsystem/RenderSystem.cpp:11127`)
  works on light view space. Every frame it takes the AABB of the 8 slice corners plus
  one point pushed `shadowMaxDistance*0.5` toward the light, pads it 5 % in XY and 10 % in
  Z, and builds `orthoMatrix`. Nothing is snapped and the extent changes with camera
  rotation, so shimmer is guaranteed.
- `computeLightCameras` (`:11200`) builds the light view with
  `lookAtMatrix(transform.worldPosition, …)`. It switches the up vector only when
  `worldDirection.crossProduct(up) == Vector3::ZERO` (exact test, `:11202-11205`), so a
  near-vertical sun gets an ill-conditioned basis.
- The split factor is hard-coded (`shadowSplitLogFactor = 0.7f`). Each slice gets a
  `+5 %` far offset.
- The shadow distance is the camera far clip: `configureLightShadowNearFar` (`:11112`)
  uses `camera.farClip` when `range == 0`, and so does `prepareCameraShadows` (`:13084`).
- The cascades are refit for every camera that draws its own atlas (`CameraShadows`,
  `RenderSystem.h:664`).

**Receiver.**
- `engine/shaderlib/includes/shadows.glsl::shadowCascadedCalculationPCF` takes the first
  cascade whose `nearFar` contains the view depth: a hard switch. Past the last cascade it
  returns 0, so the shadow ends in a line.
- Bias is `max(bias*(1-NdotL), bias/10)` in NDC depth units. The value is the same for
  every cascade even though their depth ranges differ by orders of magnitude.
- The normal offset is applied per vertex: `surface_vs.glsl:398`, `mesh.vert:284`,
  `shadowParams.x = shadowNormalBias * texelSizeWorld` (`RenderSystem.cpp:1231-1234`).
- `LightComponent::shadowNormalBias` (`engine/core/component/LightComponent.h:93`) is not
  serialized (`editor/Stream.cpp:6298`), not in the Inspector
  (`editor/window/InspectorWindow.cpp:8863`), not in Lua and not in `Factory.cpp:1771`.
- Shadow coordinates arrive through `v_lightProjPos[MAX_SHADOW_ATLAS_SLOTS]`: 9 vec4
  varyings per vertex. `engine/core/Engine.h:44-58` documents that the slot count is
  bounded by varyings.

**Filter.**
- 3D shadows use a bilinear-weighted grid PCF whose radius is the uniform
  `SHADOW_PCF_RADIUS`, set from scene `ShadowQuality` NONE/LOW/MEDIUM/HIGH
  (`engine/core/SceneSettings.h:28`, `RenderSystem::shadowPCFRadius`).
- On HLSL the radius is capped at 3x3 (`MAX_PCF_RADIUS` under `IS_HLSL`) because
  implicit-LOD `texture()` in loops forces unrolling. D3D11 is therefore lower quality.
- There is no Hard/Soft choice per light, no strength and no PCSS.

**Point lights.**
- They use an RGBA8 colour atlas with packed depth: `encodeDepth` in `depth.frag`,
  `PIP_DEPTH`, NEAREST, a manual compare in `shadowCubeCompare`.
- The kernel is 8 ring offsets with a hard-coded `diskRadius = length*0.0005`, and a
  half-texel clamp at face edges, which leaves cube seams.
- Each face is its own render pass (`renderShadowMaps`, `RenderSystem.cpp:13344`).
- The result does not match spot shadows.

**Atlas.**
- `loadLights` (`:845`) gives out slots in component order, first come first served.
- The slot resolution is the maximum `mapResolution` of all lights (`ensureShadowAtlas`,
  `:2582`), so per-light resolution is not honoured.
- The projective pass clears the whole atlas and redraws every slot every frame
  (`setClearDepth(1.0f)`, `:13444`).
- `ShadowAtlasKey`/`computeShadowKeys` (`RenderSystem.h:644`, `.cpp:12897`) already
  compares slot VP and caster masks so that cameras can share atlases. That is the
  natural seed for caching.

**Casters.**
- `MeshComponent` has `castShadows`, `receiveShadows` and `shadowsBillboard`
  (`MeshComponent.h:357-359`) and no static flag.
- Shadow LOD is already light-relative (`shadowLodView`, `:6926`), which is good for
  caching.
- Terrain, billboards and camera-culled instances are camera-dependent casters
  (`ShadowAtlasKey::cameraCasters`).

**Shader pipeline.**
- GLSL 450 sources in `engine/shaderlib` go through glslang → SPIR-V → spirv-cross
  (`engine/libs/shadercompiler`) to glsl410, glsl300es, hlsl5, msl21 macOS/iOS and
  spirv10 (`engine/core/shader/ShaderBuildTypes.h:18`).
- Variant bits are in `ShaderVariantCompiler.cpp:362` (bit 5 is reserved, formerly
  `USE_SHADOWS_PCF`).
- `// @image_sample_type <tex> unfilterable_float` and `// @sampler_type <smp> nonfiltering`
  annotations exist (`ShaderVariantCompiler.cpp:225,327`).
- sokol requires `SG_IMAGESAMPLETYPE_DEPTH` to pair with a COMPARISON sampler
  (`sokol_gfx.h:23001`). A depth image bound to an `UNFILTERABLE_FLOAT` view passes
  validation (`:23697`).

**Settings, Lua and MCP.**
- Lua `Light` exposes `shadows`, near/far, `automaticShadowCamera` and `numCascades`
  (`engine/core/script/binding/GameObjectClassesLua.cpp:584-593`). `Light::setBias` and
  `setShadowMapSize` exist but are not bound.
- MCP has `shadows_quality` in `set_scene_settings` (`editor/mcp/tools/SceneTools.cpp:608`),
  and light fields go through the generic `set_properties` (`editor/Catalog.cpp:540`).
- There are no per-platform tiers and no shadow statistics.

## 4. Reference (OpenEngine, MIT, read-only)

Root: `external/OpenEngine/Engine/`. OpenEngine uses LH reverse-Z. Every depth comparison
taken from it must be inverted for our GL-convention depth (`[−1,1]` NDC, Vulkan fixup in
`surface_vs.glsl`).

| Take | File | What / how |
|---|---|---|
| Sphere fit | `Source/Engine/Rendering/ShadowMapRenderFeature.cpp:545` `SliceBoundingSphereRadius`, `:565` `SolveSliceFarForRadius` | Copy the math (closed form, on our `Vector3`/`Matrix4`) |
| Texel snap | same file `:179-190` `SnapClipTranslationToTexels`, `:163` `MakeLightViewAtEye` (double-precision translation), `:194` `CascadeLightUp` (0.99 threshold) | Copy and adapt |
| Extent/freeze bands | same file `:78-110` (`GetCascadeFitTuning`, guard bands 1/16 for c0 and 1/8 otherwise, shrink hysteresis 0.85) | Take the numbers for the fit freeze (R5) |
| Blend band | `Modules/Rendering/Shaders/Includes/shadow_cascade_blend.glsl` (0.175 of the cascade range, bounded by the slice overlap) | Copy the constant and reasoning. The overlap must match R3 |
| Distance fade | `…/Includes/shadow_distance_fade.glsl` | Copy `GE_ShadowDistanceFade` |
| Receiver plane | `…/Includes/shadow_receiver_plane.glsl` | Copy, with the NaN guard by `isnan` (we have no WGSL) |
| Vogel/rotation | `…/Includes/shadow_filter.glsl:22` `GE_VogelDisk`, `shadow_sampling.glsl:235` `GE_PoissonRotation` (IGN) | Copy. The default basis is **shadow-space**, not screen (we have no TAA) |
| PCSS | `shadow_sampling.glsl:448-600` (blocker search, `GE_PcssTapsForRadius`), `…/Includes/shadow_pcss_decisions.glsl` | Port with the comparisons inverted. Drop the bindless and pyramid parts |
| Pyramid | `Source/Engine/Rendering/ShadowMinMaxPyramid.cpp`, `Shaders/shadow_minmax_reduce.comp` | Study only. Optional fragment-shader port (R14) |
| Cache state machine | `Include/Engine/Rendering/CascadeShadowCache.h`, `Source/…/CascadeShadowCache.cpp` | Take the idea of a dirty-cause enum and a pure CPU evaluator. Their keys (GPU LOD, RG pool identity) do not apply. Write our own key (R16) |
| Point planner | `Include/Engine/Rendering/PointShadowAtlasPlanner.h`, `Source/…/PointShadowAtlasPlanner.cpp`, `PointShadowFaceCull.cpp` | Take importance ranking, sticky slots with hysteresis, sub-rect tiers and face culling |
| Tests to mirror | `Engine/Tests/CascadeShadowCacheTests.cpp`, `ShadowDistanceFitTests.cpp`, `ShadowPcssDecisionTests.cpp`, `PointShadowAtlasPlannerTests.cpp`, `PointShadowFaceCullTests.cpp` | Rewrite as gtest |

Licence: MIT. Copied functions keep their header and get a `NOTICE.md` entry. Nothing
from the do-not-copy list is used (no render graph, no compute, no bindless).

## 5. Requirements

### Functional — directional cascades
- **R1 Light basis.** The cascade light view is rotation-only and built from the light
  direction alone. Up is world Y, or world Z when `|dot(dir, Y)| > 0.99`. The translation
  is accumulated in double precision. The light entity's position has no effect on
  directional shadows.
- **R2 Sphere fit and snapping.** Each cascade covers a sphere around the camera frustum
  slice `[n_c, f_c]` (`SliceBoundingSphereRadius`) with an ortho half-extent equal to `r_c`.
  - The sphere centre, in light view XY, is snapped to `texelWorld = 2 r_c / res_c`.
  - `r_c` is rounded up to a multiple of `r_c / 64` so it changes only when splits,
    FOV or aspect change.
  - Result: when the camera translates without crossing a texel, or rotates in place,
    the light VP is bit-identical.
- **R3 Splits and distance.**
  - `shadowDistance` (per-light override, otherwise the scene/tier default) replaces the
    camera far as the end of the last cascade, clamped to the camera far.
  - Split mode **Auto** uses log/uniform lambda 0.7 (today's value). **Manual** uses
    `cascadeSplits` (normalized, strictly increasing).
  - Slice `c+1` starts `blendFraction*(f_c−n_c)` before `f_c` so its box holds the blend
    band.
- **R4 Depth range.** The ortho near plane is pulled toward the light up to the
  light-space bounds of the shadow casters that overlap the box's XY, with the far plane at
  the sphere back. The default is scene caster bounds. Near/far are quantized to `r_c/8`
  steps so they do not change the key every frame. No depth clamp is used (GLES lacks it).
- **R5 Fit freeze (caching aid).**
  - When caching is on, a cascade keeps its previous VP while the current slice sphere
    still lies inside the frozen box shrunk by a guard band: 1/16 of the extent for c0,
    1/8 for the others. Coverage is never lost.
  - Refit is *requested* at 50 % of the guard consumed and *forced* at 100 %.
  - At most one requested (non-forced) refit runs per light per frame, staggered across
    cascades.
  - Receivers always sample with the VP the slot was rendered with.
- **R6 Blend band.** Within the last `0.175*(f_c−n_c)` of cascade `c < last`, the
  receiver samples `c` and `c+1` and mixes them with a smoothstep. Low tier picks one
  cascade per pixel instead, by a world-locked IGN dither against the same weight.
- **R7 Distance fade.** The directional shadow term fades to lit over the last
  `shadowFadeFraction` (default 0.1) of `shadowDistance`, by view depth. Nothing past it
  is shadowed.
- **R8 Orthographic cameras** keep 1 cascade, using the same sphere fit with
  `r = max(orthoHalfDiag, …)`, snapping and fade.

### Functional — receiver, filter, bias (all projective and point lights)
- **R9 Fragment-stage coordinates.**
  - `lightShadow()` (`includes/lights.glsl`) computes `lightVP * (worldPos + n*normalOffset)`
    in the fragment stage, using the per-slot matrices in the new FS block (R27).
  - Library shaders stop writing and reading `v_lightProjPos`, which removes 9 vec4
    varyings.
  - The legacy entry points (`shadowCalculationPCF`, `shadowCascadedCalculationPCF`,
    `u_fs_shadows` layout, `v_lightProjPos`) keep compiling and working for full-copy
    forks of `mesh.vert/frag` (R30).
- **R10 Bias model** (per light, in shadow-texel units, resolution- and cascade-independent):
  - `shadowBias` (default 1.0): depth offset = `shadowBias * texelWorld / depthRangeWorld`.
  - `shadowNormalBias` (default 1.0): world normal offset = `shadowNormalBias * texelWorld * (1 − NdotL)`.
  - For spot and point lights, `texelWorld` is evaluated at the receiver's distance
    (`2·d·tan(fov/2)/res`).
  - **Receiver-plane bias** (Medium and above): the depth gradient is computed from
    `dFdx/dFdy(worldPos)` taken once in uniform control flow and transformed by the slot
    matrix (`GE_ShadowReceiverPlaneGradient`). Each tap compares against the plane
    through the receiver.
  - The slope term from today is removed.
- **R11 Shadow type and strength.**
  - Per light: **None / Hard / Soft**, plus `shadowStrength` 0–1. The final factor is
    `mix(1, visibility, strength)`.
  - Hard is one bilinear comparison tap. Soft uses the tier filter (R12).
- **R12 Soft filter per tier.**
  - Low: 3x3 optimized grid (4 bilinear compare fetches).
  - Medium: 5x5 grid (9 fetches).
  - High: Vogel disk, 16 taps, rotated by shadow-space IGN. Radius
    `= clamp(tan(θ/2)·d_ref / texelWorld, 1, 8)` texels with `d_ref = 2 m`.
  - Ultra: PCSS (R13).
  - Point lights use the same family on the face tile (Vogel at High+, 5-tap at Low).
- **R13 PCSS (Ultra; directional and spot).**
  - Blocker search: 16 Vogel taps on raw depth, radius bounded by the kernel cap.
  - Penumbra: `(dReceiver−dBlocker)·tan(θ/2)` for directional, using the ortho depth
    span in world units. Spot uses `lightRadius·(dR−dB)/dB` with linearized depth.
  - The penumbra is clamped to `[1 texel, 24 texels]`.
  - PCF taps are `GE_PcssTapsForRadius` in [16, 32].
  - No blockers found → lit, early out after the search.
  - Point lights on Ultra use High's Vogel filter (no PCSS across cube faces).
- **R14 Pyramid (optional).** The min/max pyramid early-out may be added on desktop only
  if AC14 misses its budget. It must be a fragment-shader reduction into RG16F with an
  RGBA8-packed fallback, and its output must match the non-pyramid path (AC8 tolerance).
- **R15 Point and spot consistency.**
  - Point shadows render depth-only into a D32 comparison atlas (`PIP_SHADOW_DEPTH`).
    All 6 faces go in one pass with a viewport and scissor per face.
  - Faces are rendered with an FOV guard band `2·atan(1 + k/res)`, where `k` is the
    tier's maximum kernel radius in texels, so no tap leaves its face.
  - A face whose frustum holds no caster is cleared, not drawn.
  - Per-light `mapResolution` is honoured as a sub-rect of the slot (slot = tier maximum).
  - Slots go to lights by importance (`intensity × screen-coverage / (1 + distance)`).
    Slots are sticky and a challenger needs a +25 % margin to take one, with a 30-frame
    cooldown (planner semantics).

### Functional — static shadow caching
- **R16 Slot key.**
  - Each projective/point slot has a content key: `{lightVP, depth range, slot rect,
    resolution, casterMask, static-set revision, dynamic-set revision, camera-caster flag}`.
  - Revisions are sums of per-caster revisions over the casters whose world AABB
    intersects the slot frustum. Revisions come from a caster registry in RenderSystem
    (R18).
  - Receiver-only parameters (biases, strength, filter, fade, tier filter changes) never
    affect the key.
- **R17 Update paths per slot, per frame:**
  - (a) key unchanged → **skip** (no draw).
  - (b) the static part is unchanged and only dynamic casters changed → **dirty-rect
    restore**: inside the union of the dynamic casters' light-space rects this frame and
    last frame (scissor), copy static depth from the static cache atlas
    (`shadow_restore.frag` writes `gl_FragDepth`), then draw the dynamic casters only.
  - (c) the static part changed → render the static casters into the static cache atlas
    slot, copy the whole slot, then draw the dynamic casters.
  - (d) camera-dependent casters (terrain, billboards, camera-culled instances) in the
    mask → re-render every frame (no cache).
  - The pass uses LOAD when any slot is kept and CLEAR when all re-render. It is skipped
    entirely when every slot is (a).
- **R18 Invalidation.**
  - A caster's revision bumps on any of:
    - Transform world-matrix change.
    - Mesh buffer, AABB or reload (`needUpdateBuffer`, `needReload`, `needUpdateAABB`).
    - Submesh material alpha mode/cutoff or depth texture change.
    - `castShadows` or `shadowsBillboard` toggle.
    - Entity active, visible or layer change.
    - Component add/remove.
    - Instanced mesh instance change.
    - LOD settings change.
  - Skinned and morphing meshes that animated this frame bump every frame.
  - The old **and** new AABBs both dirty the slots they touch.
  - A light invalidates all its slots on a change to: type, direction, position, range,
    cone, near plane, `mapResolution`, cascade count, splits, `shadowDistance`,
    `cullingMask`, enable, the atlas being recreated, or a device or backend reset.
  - The scene camera's FOV, aspect or near clip invalidate the directional slots through
    the key.
- **R19 Static flag.** `MeshComponent::staticShadowCaster` (default false). Static
  casters go into the static layer; all others are dynamic. A static caster that changes
  stays static, and its change takes path (c).
- **R20 Update mode per light.** `shadowUpdateMode`:
  - **Auto** (default): R17.
  - **EveryFrame**: always full re-render.
  - **OnDemand**: renders on enable, on light or resolution change, and on Lua/MCP
    `requestShadowUpdate()`. Caster changes are ignored (Unity HDRP semantics).
- **R21 Scope of the static layer.** It exists only for the frame atlas
  (`primaryView.shadows`). Other `CameraShadows` atlases (render textures, mirrors,
  probes, editor secondary views) use (a)/(d) only, without a static layer. The static
  cache atlas is created lazily, only when a slot has both static casters and dynamic
  casters. It is freed after 300 frames unused.

### Platform & backends
- **R22** Every feature works through the existing glslang/spirv-cross path on glsl410,
  glsl300es (GLES3/WebGL2), hlsl5, msl21 macOS/iOS and spirv10. No compute, SSBO, gather
  (GLES3.0 has no `textureGather`), bindless, or depth clamp.
- **R23** Every shadow lookup inside a loop uses explicit LOD 0: `textureLod` on
  `sampler2DShadow` (→ `SampleCmpLevelZero` / `sample_compare(…, level(0))`) or
  `texelFetch`. The HLSL `MAX_PCF_RADIUS = 1` cap is removed and D3D11 gets the same
  quality as the other backends.
- **R24** Raw-depth reads (PCSS, restore) go through a second texture view of the depth
  image, annotated `unfilterable_float` with a `nonfiltering` sampler. It is declared only
  in variants that need it.
- **R25** Variant bit 5 (reserved) becomes `USE_SHADOWS_HQ` ('Shq'): the Vogel/PCSS code
  and the raw view. It is set when the effective tier is High or Ultra.
  - Exports cook bit-5 variants only for platforms whose tier is High or above.
  - A runtime override above the cooked tier falls back to Medium filters and logs one
    warning.

### Performance (budgets; measured per AC14–AC16)
Reference devices:
- **Mobile**: Adreno 640 / Mali-G76 class, 1080p, GLES3/Vulkan.
- **Web**: Intel Iris Xe, Chrome WebGL2, 1080p.
- **Desktop**: GTX 1660 class, 1440p.

The scene is `tools/fixtures/shadows/bench.scene`: 1 sun, 4 spots, 2 point lights, 2,000
static casters and 20 skinned dynamic casters.

| Tier (default for) | Low (Android, iOS) | Medium (Web) | High (Win/Linux/macOS) | Ultra (opt-in) |
|---|---|---|---|---|
| Max cascades / slot res | 2 / 1024 | 3 / 2048 | 4 / 2048 | 4 / 4096 |
| Default shadow distance | 40 m | 60 m | 100 m | 150 m |
| Soft filter | 3x3 grid | 5x5 grid | Vogel 16 | PCSS 16+16..32 |
| Cascade transition | dither | blend | blend | blend |
| Receiver plane | off | on | on | on |
| Receiver GPU, full-screen sun, Soft | ≤ 1.0 ms | ≤ 1.5 ms | ≤ 0.6 ms | ≤ 1.2 ms |
| Shadow pass GPU, static scene and camera, cached | ≤ 0.05 ms | ≤ 0.05 ms | ≤ 0.05 ms | ≤ 0.05 ms |
| Shadow pass GPU, walking camera (5 m/s), cached + 20 dynamic | ≤ 1.5 ms | ≤ 2.0 ms | ≤ 1.0 ms | ≤ 1.5 ms |
| Shadow memory (atlases + static cache) | ≤ 16 MB | ≤ 48 MB | ≤ 96 MB | ≤ 288 MB |

- **R26** CPU costs:
  - Cascade fit + key evaluation: ≤ 30 µs per camera.
  - Caster registry update: ≤ 0.2 ms per frame for 10k casters when 1 % move.
  - With caching on, a frame with zero changes issues **zero** shadow draw calls.
  - Disabled shadows (`hasShadows == false`) keep costing zero, as today.

### Editor UX (human)
- **R27 Light Inspector, "Shadows" section** (Unity order), all undoable through
  `propertyRow`, with multi-edit:
  - Shadow Type (No Shadows / Hard Shadows / Soft Shadows), Strength.
  - Resolution (Use Quality Settings, 256 … 4096).
  - Bias, Normal Bias.
  - Near Plane (when not automatic).
  - Update Mode.
  - Directional only: Shadow Distance (0 = quality setting), Cascade Count (Use Quality,
    1–4), Split Mode + a draggable split bar showing the cascades as coloured segments
    with metres, Angular Diameter (°, default 0.53).
  - Spot/Point only: Source Radius (m, default 0.05).
  - Disabled rows explain why (for example "PCSS needs Ultra; this platform uses High").
- **R28 Mesh Inspector** "Lighting": add **Static Shadow Caster** next to Cast/Receive
  Shadows. **Scene Inspector, "Shadows"**: Quality (Auto, Hard Only, Low, Medium, High,
  Ultra), Shadow Distance, Fade, Cascade Blend (on/off), and a read-only "Effective tier
  on <platform>". **Project Settings → Graphics → Shadows**: tier per platform.
- **R29 Debug views** (Scene view shading menu and scene setting `shadows_debug_view`,
  editor-only, not saved): Cascades (tinted, blend band hatched), Shadow Factor (grey),
  Texel Density (checker in shadow texels), Cache (green = skipped, yellow = restored,
  red = re-rendered this frame), Atlas (overlay of both atlases and the static cache).

### Agent UX (MCP tools, Lua API)
- **R30 MCP.**
  - New `LightComponent` and `MeshComponent` fields are registered in `Catalog`, so
    `set_properties`/`get_entity` cover them.
  - `set_scene_settings`/`get_scene_settings` gain `shadow_distance`,
    `shadow_fade_fraction`, `shadow_cascade_blend`, `shadows_debug_view`, and
    `shadows_quality` gains `auto` and `ultra`.
  - `get_project_settings`/`set_project_settings` gain `graphics.shadow_tier.<platform>`.
  - New tool **`get_shadow_stats`** `{camera?: "scene"|"game"}` returns:
    - Effective tier, atlas sizes, memory bytes.
    - Per light: slots, rect, resolution, cascade splits, `r_c`, `texelWorld`.
    - Per slot: cache path (skip/restore/static/full/uncached), dirty cause, frames since
      last render.
    - Draw calls per path, and GPU ms per shadow pass when S15 timings exist.
  - New tool **`set_shadow_debug`** `{fit_camera_offset?: [x,y,z], fit_camera_euler?:
    [p,y,r], freeze_cache?: bool, force_rerender?: bool}` (editor-only, transient). It
    offsets *only* the camera that cascades are fitted to, for the stability tests.
  - Updated: `docs/mcp/TOOLS.md`, the agent guide (`editor/mcp/McpInstructions.cpp`),
    and a new `docs/mcp/guide/shadows.md` served as `jasper://docs/shadows`.
- **R31 Lua.**
  - On `Light`: `shadowType` (`ShadowType.NONE|HARD|SOFT`), `shadowStrength`,
    `shadowBias`, `shadowNormalBias`, `shadowResolution` (0 = quality), `numCascades`
    (exists), `cascadeSplitMode`, `cascadeSplits` (Vector3), `shadowDistance`,
    `shadowAngularDiameter`, `shadowSourceRadius`, `shadowUpdateMode`,
    `requestShadowUpdate()`.
  - On `Mesh`: `staticShadowCaster`.
  - On `Scene`: `shadowQuality` (+AUTO, ULTRA), `shadowDistance`, `shadowFadeFraction`,
    `shadowCascadeBlend`, read-only `effectiveShadowTier`.
  - Documented in `docs/mcp/guide/lua-api.md`.

### Data & compatibility
- **R32 New serialized fields, with defaults.**
  - Light: `softShadows` (true), `shadowStrength` (1), `shadowNormalBias` (now saved),
    `cascadeSplitMode` (auto), `cascadeSplits` (0.067, 0.2, 0.467), `shadowDistance` (0),
    `shadowAngularDiameter` (0.53), `shadowSourceRadius` (0.05), `shadowUpdateMode`
    (auto). `mapResolution` 0 means "quality".
  - Mesh: `staticShadowCaster` (false).
  - Scene: `shadowDistance` (0 = tier), `shadowFadeFraction`, `shadowCascadeBlend`.
  - Project: `graphics.shadowTier.<platform>`.
  - Written in `Stream.cpp` and in `Factory.cpp` (exported C++).
- **R33 Migration** (scene version bump via S03; old projects load):
  - `shadowBias` changes units from NDC depth to texels. A pre-version value is replaced
    by 1.0, with one info log per scene.
  - `shadowQuality` LOW (the old default) → AUTO. NONE → Hard Only. MEDIUM/HIGH stay
    explicit.
  - Lights saved with `automaticShadowCamera` keep it. The old camera-far distance
    becomes the tier `shadowDistance`, which is the intended behaviour change.
- **R34 Legacy forks.** Shaders that declare the legacy `u_fs_shadows` layout and
  `v_lightProjPos` keep working for directional and spot lights: the engine still fills
  `u_vs_shadows` and the legacy `u_fs_shadows` when a shader declares them.
  - Point shadows in such forks read as lit, because the packed colour atlas is gone.
    The engine warns once per shader.
  - `check_shader` reports "legacy shadow interface: re-fork to get point shadows, blend
    and PCSS".

## 6. Design constraints & decisions

- **No render graph.** The shadow work stays in `RenderSystem::renderShadowMaps`. The fit
  and cache logic move to new pure-CPU units under `engine/core/render/shadow/`:
  - `CascadeFit.{h,cpp}`: sphere, snap, splits, freeze.
  - `ShadowCache.{h,cpp}`: key, paths, dirty cause.
  - `ShadowCasterRegistry.{h,cpp}`: revisions and AABBs.
  - `ShadowSlotPlanner.{h,cpp}`: importance, sticky slots, sub-rects.

  These units take no sokol types, so they are unit-testable. `RenderSystem.cpp` only
  calls them, to keep the 15k-line hotspot small.
- **Precision.** The cascade VP is composed in double and stored as float. Snapping
  happens on the light-view-space centre (equivalent to OpenEngine's clip-translation
  snap) before the ortho is built.
- **One projective atlas, one point atlas, one static cache atlas.** The static cache
  mirrors the projective atlas layout. Point static caching reuses a static point cache
  with the same rules. All atlases are D32 depth-only.
- **Restore** is a fullscreen-triangle pass per dirty rect: the scissored slot rect,
  depth compare ALWAYS, depth write, no colour attachment.
- **FS shadow block** (new, `u_fs_shadow_data`; the C++ struct `fs_shadow_data_t` needs a
  `static_assert` on size and a layout unit test):
  ```glsl
  uniform u_fs_shadow_data {
      mat4 lightVP[MAX_SHADOW_ATLAS_SLOTS];      // the VP the slot was rendered with
      vec4 atlasRect[MAX_SHADOW_ATLAS_SLOTS];    // offset.xy, scale.zw (sub-rect aware, y-flip as today)
      vec4 bias[MAX_SHADOW_ATLAS_SLOTS];         // depthBias(ndc), normalOffsetPerTexel(world), texelWorld, depthSpanWorld
      vec4 split[MAX_SHADOW_ATLAS_SLOTS];        // near, far, blendStart, slotRes
      vec4 filter[MAX_SHADOW_ATLAS_SLOTS];       // mode(0 hard,1 grid,2 vogel,3 pcss), radiusTexels, tanHalfAngle|radius, strength
      vec4 fade[MAX_SHADOW_ATLAS_SLOTS];         // fadeStart, fadeEnd, transition(0 none,1 dither,2 blend), receiverPlane(0/1)
  } shadowData;
  // u_fs_point_shadow_data mirrors it per point slot (face VP, rect, bias, filter).
  uniform texture2D u_shadowAtlas;   uniform samplerShadow u_shadowAtlas_smp;
  #ifdef USE_SHADOWS_HQ
  // @image_sample_type u_shadowAtlasRaw unfilterable_float
  // @sampler_type u_shadowAtlasRaw_smp nonfiltering
  uniform texture2D u_shadowAtlasRaw; uniform sampler u_shadowAtlasRaw_smp;   // same image, second view
  #endif
  float lightShadow(int index, vec3 worldPos, vec3 worldNormal);   // signature unchanged
  float sampleDirectionalShadow1(vec3 worldPos);   // 1-tap, for S12 volumetrics
  ```
- **Hard/Soft per light** is packed per slot (`filter.x`), not in the per-object light
  lists (`objectLights.spot_shadow.w` stays as is), to avoid merge conflicts with S11.
- **Tiers** are resolved once per frame:
  `effective = scene.shadowQuality != AUTO ? scene.shadowQuality : project.tier[Engine::getPlatform()]`,
  then capped by the cooked variant set (R25). In the editor, the platform is the
  selected build target (Game view) or "Desktop".
- **API sketches.**
  ```cpp
  enum class ShadowType { NONE, HARD, SOFT };                 // UI view of shadows + softShadows
  enum class ShadowUpdateMode { AUTO, EVERY_FRAME, ON_DEMAND };
  enum class CascadeSplitMode { AUTO, MANUAL };
  enum class ShadowQuality { NONE, LOW, MEDIUM, HIGH, AUTO, ULTRA }; // append-only: stored ints stay valid
  struct CascadeFitInput { Matrix4 cameraView; float tanHalfY, aspect, nearClip, shadowDistance;
                           Vector3 lightDir; unsigned cascades, resolution; float splits[4];
                           CascadeSplitMode mode; float blendFraction; bool freeze; };
  struct CascadeFitResult { Matrix4 view, proj, viewProj; float splitNear[4], splitFar[4], radius[4], texelWorld[4];
                            bool refit[4]; };
  CascadeFitResult fitCascades(const CascadeFitInput&, const CascadeFitResult* previous);
  enum class ShadowSlotPath { SKIP, RESTORE_DYNAMIC, RENDER_STATIC, FULL, UNCACHED };
  enum class ShadowDirtyCause { NONE, FIRST, LIGHT, FIT, STATIC_CASTER, DYNAMIC_CASTER, CAMERA_CASTER, ATLAS, FORCED };
  ```
  MCP: `get_shadow_stats {camera?}`, `set_shadow_debug {fit_camera_offset?, fit_camera_euler?, freeze_cache?, force_rerender?}`.
  Lua: `light:requestShadowUpdate()`, `light.shadowType = ShadowType.SOFT`.
- **Noise basis.** Vogel rotation is IGN keyed on the continuous shadow-texel coordinate
  (world-locked). Screen-keyed rotation is not offered: we have no TAA, and swimming grain
  would fail AC3.

## 7. Acceptance criteria

The fixture `tools/fixtures/shadows/` (stability, filter, cache and bench scenes) and the
MCP test `tools/mcp/tests/test_shadows.py` are new. Golden images use
`tools/mcp/pngdiff.py` (tolerance 24, max fraction 1 %) on the CI backends: OpenGL on
llvmpipe and Vulkan on lavapipe.

- **AC1 Fit unit tests** (gtest, `engine/tests/render/CascadeFitTests.cpp`):
  - `SphereRadius.MatchesBruteForceCorners`: within 1e-4 relative, 1,000 random frusta.
  - `Snap.TranslationWithinTexelKeepsVP`: bit-identical VP.
  - `Snap.RotationKeepsExtent`: radius bit-identical over 360 yaw steps.
  - `Basis.NearVerticalLightIsFinite`: dir within 0.1° of ±Y.
  - `Splits.ManualAndAutoMonotonic`.
  - `Freeze.NeverLosesCoverage`: 10,000 random walk steps; the slice sphere is always
    inside the box.
  - `Freeze.AtMostOneRequestedRefitPerFrame`.
- **AC2 Shimmer metric, translation** (MCP, `test_shadows.py::test_no_shimmer_translate`):
  - Scene `stability.scene`, debug view Shadow Factor, 640x360.
  - 32 frames of `set_shadow_debug fit_camera_offset` in steps of 0.13·texelWorld(c0)
    along light-space X and view Z.
  - The fraction of pixels whose 8-bit value changes by more than 2 between consecutive
    frames is **≤ 0.05 %**.
  - The same harness on `main` before this stream records the baseline in the PR, where
    > 1 % is expected.
- **AC3 Shimmer metric, rotation** (`test_no_shimmer_rotate`): `fit_camera_euler` yaw in
  0.25° steps, 32 frames. Same bound, ≤ 0.05 %.
- **AC4 Seams and fade** (`test_cascade_blend_fade`):
  - On `stability.scene` with the camera looking along a long plane, the Cascades debug
    screenshot matches its golden image.
  - Along the view centre line, the Shadow Factor profile has no step greater than 8/255
    between adjacent pixels at the split depths.
  - The value is exactly 255 (lit) beyond `shadowDistance`.
- **AC5 Golden images per tier** (`test_shadow_goldens`): `filter.scene` (sun, spot and
  point; Hard and Soft lights; a contact-hardening pole) at Low, Medium, High and Ultra,
  one golden each, on both CI backends.
- **AC6 Acne and peter-panning** (`test_bias`): a 1°–80° tilted plane under the sun at
  default biases shows no acne. On the shadowed half the Shadow Factor variance is
  < 1/255, and the lit half is ≥ 99.5 % at 255. A cube's shadow touches the cube base:
  the gap is ≤ 1 texel, measured on the debug view.
- **AC7 Cross-backend compile**: `jasperengine-editor shaders --library all --backend all`
  passes, with and without bit 5.
  - The HLSL output of the High variant contains no `[unroll]` forced by a shadow loop:
    check the generated hlsl5 text for `SampleCmpLevelZero` in a `[loop]`.
  - The Metal macOS/iOS and D3D11 sources are produced without errors.
- **AC8 PCSS correctness** (gtest `PcssDecisionTests`, ported from
  `ShadowPcssDecisionTests.cpp` with inverted depth): the blocker test, the penumbra clamp
  and the tap selection. Visually, in AC5 Ultra, the penumbra width at the pole's base is
  < 2 texels and at 3 m height > 6 texels (measured by the MCP test on the profile).
- **AC9 Point/spot consistency** (`test_point_shadows`):
  - Point-light cube-edge views show no seam: no step greater than 8/255 across face
    borders on the Shadow Factor view.
  - The point light's Soft penumbra width matches a spot light at the same distance and
    resolution within ±1 texel.
- **AC10 Caching paths** (gtest `ShadowCacheTests`): every R17 path and every R18 cause
  produces the expected `ShadowSlotPath`/`ShadowDirtyCause`. Receiver-only edits produce
  SKIP.
- **AC11 Caching end-to-end** (`test_shadow_cache`, `cache.scene`), via `get_shadow_stats`:
  - Static scene and camera: after 2 frames, every slot is SKIP and draws are 0.
  - Moving one dynamic cube: only slots that hold it are RESTORE_DYNAMIC, and the rect
    covers old ∪ new.
  - Moving a static caster: RENDER_STATIC on the intersecting slots only.
  - Image identity: `freeze_cache=false` + `force_rerender=true` vs cached produces
    identical screenshots (max fraction 0).
- **AC12 Invalidation coverage**: each R18 trigger has one MCP step that edits it through
  `set_properties`/`set_transform`/`set_entity_state`. The next frame's slot cause matches
  the trigger and the image matches a forced re-render.
- **AC13 OnDemand**: with `shadowUpdateMode = ON_DEMAND`, moving a caster changes nothing
  until `eval_lua light:requestShadowUpdate()`, and then the image updates.
- **AC14 GPU budgets**: run the bench fixture with S15 GPU pass timings
  (`JASPERENGINE_FRAME_STATS=1`, or `get_shadow_stats`) on the reference devices
  (manual, recorded in `docs/rendering/shadow-budgets.md`). Each budget cell in §5 is met;
  a miss blocks merging of that tier's default.
- **AC15 Zero cost**: on CI, a no-shadow scene draws exactly as many passes and draws as
  on `main`. With caching on and a static scene, the shadow pass is skipped (frame stats
  pass list).
- **AC16 Memory**: `get_shadow_stats.memoryBytes` is ≤ the tier cap for the bench scene
  at each tier (CI, via a scene override).
- **AC17 Editor/Lua/MCP round-trip** (`test_shadow_settings`):
  - Every R27–R31 field is set via MCP, saved, reloaded and read back via Lua `eval_lua`.
  - Undo in the Inspector reverts each field (manual check list in the PR: Shadow Type,
    Resolution, Bias, Normal Bias, Distance, Cascades, Split bar drag, Static Shadow
    Caster).
- **AC18 Migration**: a scene saved by `main` (shadowBias 0.002, shadowQuality low)
  loads with Bias 1.0 and quality Auto, logs one info line, and renders a non-flat image.
  A legacy fork (`tools/fixtures/runtime-shaders` `tinted.*`) still compiles and shows
  directional shadows. `check_shader` reports the legacy notice. `tools/smoke-test.sh`
  passes on `unified-lighting` and the new `shadows` fixture.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20).
  - S02 (gtest target `engine/tests`, CI runs it).
  - S03 (scene version bump and migration hook for R33).
  - S15 (GPU pass timers for AC14). Until S15 lands, `get_shadow_stats` reports CPU-side
    counts and draws only.
- **Uses if present:**
  - S07 change tracking can feed `ShadowCasterRegistry`. Without it, the registry hooks
    `Transform::needUpdate` processing and mesh flags itself.
  - S08 reflection registers the new fields. Otherwise they are added by hand in
    `Catalog.cpp`, `Stream.cpp` and `Factory.cpp`.
  - S06 jobs for parallel caster AABB updates.
- **Blocks or informs:**
  - S12: volumetric fog and clouds use `sampleDirectionalShadow1`.
  - S11: lighting includes, and photometric units touching `LightComponent`.
  - S20 and S21: particles and animation must bump caster revisions (skinned → dynamic).
  - S18: generated meshes bump revisions.
- **Merge hotspots:**
  - `engine/core/subsystem/RenderSystem.{h,cpp}` (everyone).
  - `engine/shaderlib/includes/{lights,shadows,surface_vs}.glsl`, `mesh.vert/frag` and
    library `*.vert` (S11, S12).
  - `engine/core/component/{LightComponent,MeshComponent}.h` (S11).
  - `engine/core/SceneSettings.h`, `editor/Stream.cpp`, `editor/Factory.cpp`,
    `editor/Catalog.cpp`, `editor/window/InspectorWindow.cpp`, `editor/Project.h`.
  - `editor/mcp/tools/SceneTools.cpp`, `editor/mcp/McpValues.cpp`.
  - `engine/core/script/binding/GameObjectClassesLua.cpp`, `CoreClassesLua.cpp`.
  - `engine/core/shader/ShaderVariantCompiler.cpp` and `engine/core/pool/ShaderPool.cpp`
    (variant bit 5).

  Land the CPU units first, then the receiver shaders, then caching.

## 9. Risks & defaults

- **Raw depth view on Metal and D3D11.** Default: a second `texture2d<float>` / SRV view
  of the D32 image. If a backend rejects it, PCSS and restore there fall back to an
  R32F colour copy of the slot. This costs one extra fullscreen pass per updated slot, is
  only needed where the view fails, and is verified manually on macOS, iOS and Windows
  before Ultra is enabled.
- **Moving coordinates to the fragment stage** adds 2 mat4·vec4 per shadowed light per
  pixel. It saves 36 interpolated floats per vertex. Default: FS coordinates. If AC14
  mobile misses by more than 10 %, keep the varyings for the Low tier only (the legacy
  path still exists) and record the measurement.
- **Fit freeze lowers texel density** by up to the guard band (6–12 %). Default: on for
  Low and Medium, off for High and Ultra, where VPs refit every frame and caching then
  helps only spot, point and still cameras. This can be changed per tier after AC14.
- **The dither transition on Low** is visible grain without TAA. Default: accepted, since
  it is world-locked and does not swim. If users object, Low can use the blend at 2x
  taps inside the band.
- **Static cache memory on mobile.** Default: on, capped by the tier's memory row. If the
  cap would be exceeded, the static layer is dropped for the lowest-importance slots,
  which fall back to path (a)/(d).
- **9 projective slots.** Default: unchanged. FS coordinates remove the varying bound, but
  legacy forks still write `v_lightProjPos[MAX_SHADOW_ATLAS_SLOTS]`. Raising the count is
  follow-up work once legacy forks are migrated.
- **Bias reset on migration** changes the look of tuned scenes. Default: reset and log.
  The new texel-unit defaults are tuned by AC6, and the old units cannot be converted
  without knowing each cascade's range.
- **Shadow distance becoming finite** changes far shadows in old scenes. Default: the tier
  default (Unity behaviour). Users can set `shadowDistance` per light or per scene.

## Deviations (implementation record)

- **Bias as a world offset.** `shadowBias` moves the receiver `shadowBias × texelWorld` towards
  the light before projecting (spot and point: the texel at the receiver's distance) instead of
  subtracting `shadowBias × texelWorld / depthRangeWorld` from its NDC depth. For a directional
  cascade the two are the same; for a perspective slot the world offset is exact where an NDC
  offset is not linear in distance.
- **PCSS kernel floor.** The PCF kernel radius is clamped to [0.5, 24] texels (R13 says
  [1, 24]): at 1 texel the comparison's own bilinear texel makes a contact penumbra at least
  2 texels wide, which AC8 (< 2 texels at the pole's base) excludes. 0.5 is a penumbra one texel
  wide; the cap is unchanged.
- **PCSS on GLES3/WebGL2 and painted terrains** uses the High tier's Vogel disk: the raw-depth
  view is a 17th fragment texture unit there (GLES3 has 16; the painted terrain variant is at 16
  already). R24 asks the view only in variants that need it; these cannot have it.
- **Point-light Low filter** is the Low tier's 3×3 grid (R12 says a 5-tap kernel); Medium uses
  the 5×5 grid, High and Ultra the Vogel disk, as for spot lights. One filter family on every
  light kind keeps point and spot penumbras equal (AC9).
- **High filters per shader backend (R25).** Exports are per shader backend, not per platform:
  an export cooks the `Shq` variants for a backend when a platform it ships to (GLES3: Android,
  web; Vulkan: Android, Linux, Windows; OpenGL: the desktops; Metal and Direct3D: theirs) has a
  tier of High or above (`ShadowTiers::cooksHighFilters`). A game without the runtime shader
  compiler asked for a higher tier renders Medium and warns once; with the compiler (the
  default) it compiles the variants it lacks. Scenes of Quality Auto, and scenes whose scripts
  set `shadowQuality`, collect both states. The old `Pcf` bit (bit 5) is cleared from scenes'
  cached variant keys by the scene 1 → 2 migration.
- **Caster changes are a log.** `RenderSystem::update` can run more than once per draw (the
  editor updates the scene before its frame does), so the shadow cache reads the caster changes
  of every update since an atlas last evaluated its slots (`casterChangeLog`), not only the last
  update's. An atlas that falls behind the log's bound (16,384 changes) redraws its slots.
- **Static layers are kept** for `STATIC_CACHE_KEEP_UPDATES` (300) updates after the last
  dynamic caster disappears (R21's lifetime), so a dynamic caster toggled off and on does not
  rebuild every static layer.
- **Fixture scenes.** The stability, filter, cache, bias and penumbra scenes are built by
  `tools/mcp/tests/test_shadows.py` with the MCP tools (explicit light directions, primitives)
  rather than stored as `.scene` files; `tools/fixtures/shadows` holds the bench scene (built at
  runtime by `scripts/Shadows.lua`, as the smoke test runs it) and `Legacy.scene` (AC18). The
  goldens are `tools/fixtures/shadows/golden/*.png`.
- **AC3 metric margin.** `set_shadow_debug fit_camera_euler` turns only the camera the cascades
  are fitted to; the view stays. The screen's sides it turns away from leave that frustum and
  with it the cascades (a camera that really turns takes its view along), so the rotation metric
  leaves out 10 % of the width on each side; the yaw sweeps −4° to +4° in 0.25° steps.
- **AC2/AC3 baseline on `main`.** `set_shadow_debug` does not exist before this stream, so the
  baseline is not measured with the same harness; the shimmer of the old fit follows from §3
  (an unsnapped, rotation-dependent AABB per frame).
- **AC16** is checked by the planner unit test on the bench scene's lights at every tier
  (`ShadowSlotPlanner.MemoryBudgetDropsStaticLayersThenHalves`) and by `get_shadow_stats` in the
  MCP golden test's scene; the runtime-built bench scene logs its memory in the smoke test.
- **S15 is not on `oe-port`:** `get_shadow_stats` reports CPU-side counts, passes and draws;
  `gpu_ms` is null until the GPU pass timers land.
- **R14** (the min/max pyramid) is not implemented: it is optional and only for an AC14 miss.
