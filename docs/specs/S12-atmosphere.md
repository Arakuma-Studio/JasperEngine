# S12 — Atmosphere: height fog, volumetric fog (fragment raymarch), volumetric clouds

Wave 2. Owner: rendering. Reference report: `docs/openengine-analysis/01-rendering.md` §1.5, Tier A item 3,
Tier B item 7, "do not copy" list; `00-SUMMARY.md` row 3.

## 1. Goal

Replace "distance fog plus a gradient sky" with a coherent atmosphere that runs on every backend and
export target. The stream ships three features:

- **Analytic height fog**, a 4th mode of our existing Fog, with directional inscattering toward the
  sun and a sky fill.
- **Volumetric fog**, an option of height fog. It is our own reduced-resolution fragment-shader
  raymarch that samples the sun's shadow map (light shafts) and Local Volumetric Fog volumes.
- **Raymarched volumetric clouds** lit by the sun and the sky.

All three share one medium and one lighting model, so they agree where they meet. Each costs zero
when it is disabled. Each has Unity-like components, Lua and MCP access, and per-tier budgets.

## 2. Scope

**In scope**
- `FogType::HEIGHT` on `FogComponent`: analytic height and distance optical depth, colour modes
  (Color / Sky), directional inscattering (Henyey-Greenstein), max opacity, and sky fill. It is
  evaluated per fragment in every fogged shader variant, and in one sky-composite pass for sky pixels.
- Volumetric fog. The pipeline is: low-res depth downsample, then a raymarch with a blue-noise
  static jitter, then a depth-aware denoise, then a full-res joint-bilateral upsample and composite.
  It samples the directional light's cascaded shadow map. On High it also takes up to 4
  point/spot lights without shadows. It reads up to 8 `LocalVolumetricFogComponent` volumes
  (box/sphere) and an optional global density noise.
- Volumetric clouds (`VolumetricCloudsComponent`): a weather-map plus 3D Worley/Perlin-Worley
  density model, lit by the sun (Beer-powder, dual-lobe HG, multi-scatter octaves) and the sky
  ambient. Two render modes:
  - **Sky Map**: a world-direction hemi-octahedral map, refreshed time-sliced with a crossfade.
    No reprojection.
  - **Screen**: a quarter-resolution per-pixel march.
- Atmosphere quality tiers (Off/Low/Medium/High) with per-platform defaults. Editor UI, presets,
  Lua, MCP, docs, unit/MCP/golden tests.

**Out of scope**
- Froxel volumetric fog, compute, 3D storage images and SSBOs (README do-not-copy list; their
  `Engine/Modules/VolumetricFog/*` is reference for phase/media maths only).
- Temporal reprojection or TAA history for fog or clouds. This v1 is temporal-free. Sky Map's
  time-slicing is not reprojection.
- Physical sky (Hillaire LUTs), solar path, stars and moon. These are a separate future stream. This
  spec only consumes "the sky" through `SkyboxComponent`.
- Cloud shadows on the ground, clouds feeding IBL or reflection-probe ambient, fog glow, GPU fog
  particles, and noise in the analytic height fog.
- Volumetric fog on non-main cameras (mirrors, render-to-texture, probes). These get analytic fog,
  sky fill and Sky Map clouds.

## 3. Current state

- **Fog** is a GameObject with `FogComponent`: `engine/core/component/FogComponent.h` has
  `FogType {LINEAR, EXPONENTIAL, EXPONENTIALSQUARED}`, plus colour, density and linear start/end.
  - The handle class is `engine/core/object/environment/Fog.{h,cpp}`.
  - The first one is used (`RenderSystem::loadAndProcessFog`, `engine/core/subsystem/RenderSystem.cpp:2011`).
    Adding or removing it reloads mesh variants (`HAS_FOG`, mesh bit 12,
    `engine/core/shader/ShaderVariantCompiler.cpp:375`).
- The fog maths is `engine/shaderlib/includes/fog.glsl`. It is distance-only: `gl_FragCoord.z / w`,
  which is not even metres in orthographic views. Its parameters are packed into 3 spare lanes of the
  Lit camera block (`includes/lit_frame.glsl`, `lit_frame_t`, `RenderSystem.h:124-131`, filled in
  `fillFrameLighting`, `RenderSystem.cpp:1278`), into `frame_t.fogColor_type/fogDensity_start_end`
  (`RenderSystem.h:107-108`), or into `u_fs_fog`. Fog is applied in `finalizeColor`
  (`includes/output.glsl`).
- Fog is plumbed through these files:
  - serialization: `editor/Stream.cpp:6446`
  - Catalog properties: `editor/Catalog.cpp:570`
  - inspector: `editor/window/InspectorWindow.cpp:9029`
  - Lua: `engine/core/script/binding/GameObjectClassesLua.cpp:395`, `ECSClassesLua.cpp:111`
  - exported scene code: `editor/Factory.h:206`
  - MCP `create_entity type fog`: `editor/mcp/tools/EntityTools.cpp:194`
  - Scene-view toggle `disable_fog`: `editor/mcp/tools/ViewTools.cpp:768`, `editor/render/SceneRender.cpp:773`
- **Sky**: there is one `SkyboxComponent` (`engine/core/component/SkyboxComponent.h`), drawn with
  `drawSky` (`RenderSystem.cpp:10076`) inside the colour pass. It is a cubemap (`shaderlib/sky.frag`)
  or a library/fork shader (`shaderlib/library/procedural_sky.frag` with 2D scrolling clouds,
  `gradient_sky`, `panoramic_sky`). IBL is generated on the CPU from cubemap face data
  (`updateSkyEnvironment`, `RenderSystem.cpp:2055`). No sky shader is fogged.
- **Colour targets** are RGBA8 holding sRGB-encoded colour. Tone mapping and grading run at the end
  of every shader under `USE_POST_COLOR` (`includes/output.glsl` header,
  `engine/core/SceneSettings.h:56-66`). Transparent and additive draws already blend in display
  space, and additive colours go through `encodeAdditiveOutput`. There is no HDR scene target.
- **Pass order** (`RenderSystem::renderCameraPass`, `RenderSystem.cpp:13712`):
  1. G-buffer when SSR is on, then SSAO with its packed-depth pre-pass, or `renderDepthPrePass` for
     user post passes needing depth. This yields `sharedSceneDepth`: RGBA8 `encodeDepth`
     (`includes/depth_util.glsl`).
  2. Colour pass: optional z-prepass, sky, opaque queue, `takeSceneColor`, the per-entity loop (UI,
     points, lines), the transparent queue, HTML UI.
  3. SSR composite, then the user post chain (`postprocess.frag`), then the uber pass (`uber.frag`;
     `PostProcessingTiers`, `RenderSystem.h:346`).
- **Shadows**: the directional cascades (`MAX_SHADOWCASCADES 4`, `engine/core/Engine.h:42`) live in
  a comparison-sampled atlas (`u_shadowAtlas` / `samplerShadow`, `includes/lights.glsl:129`). Light
  matrices are applied in the vertex stage (`v_lightProjPos`). No fragment-side cascade-matrix block
  exists. Note: `includes/shadows.glsl:17-22`: "HLSL unrolls any loop holding a texture lookup".
- **Capabilities**: `RenderFeatures.renderRGBA16F/blendRGBA16F/filterRGBA16F`
  (`engine/core/render/SystemRender.h:14`). `TextureType::TEXTURE_3D` maps to `SG_IMAGETYPE_3D`
  (`engine/renders/sokol/SokolShader.cpp:100`), but `SokolTexture::createTexture` has no depth
  parameter. 3D texture creation is missing.
- **Quality**: Project Settings › Quality only has VSync (`editor/window/dialog/ProjectSettingsWindow.cpp:1314`).
  No tier system exists. All mesh variant bits are taken except **bit 5**, which is retired
  (`ShaderPool.cpp:1251,1284`).
- **Tests**: MCP render checks use `screenshot` and `image_difference`
  (`tools/mcp/tests/test_post_processing_views.py`). There are no 3D golden images yet.

**What is wrong today:** there is no height fog, no light shafts and no real clouds. Fog distance is
wrong in orthographic views, and fog never touches the sky, so the horizon shows a hard seam.

## 4. Reference (OpenEngine, MIT; read-only at `.../JasperEngine-2/external/OpenEngine`)

| What | Path | Take | Improve / change |
|---|---|---|---|
| Height fog term | `Engine/Modules/Rendering/Shaders/Includes/height_fog_term.glsl` (377 lines) | Optical depth `GE_HeightFogHeightOpticalDepth` (homogeneous below base + exponential above, closed form), smooth distance ramp, HG phase, `scattering = C·(1−T)` energy form, sky amount `GE_HeightFogSkyAmount` | Reparameterise to Unity-like physical fields (attenuation distance, base/maximum height). Add a segment form `[t0,t1]` for the volumetric handoff. Drop the noise, gradients, layer modes and their `sky_composite.glsl` backdrop. Our Sky colour comes from a CPU horizon ring. |
| Params layout | `.../Includes/height_fog_params_fields.glsl`, `Engine/Source/Engine/Rendering/HeightFogParamsUBO.cpp` | Field inventory | A packed 14×vec4 block of our own (§6.2) |
| Presets | `Engine/Modules/HeightFog/Source/HeightFogEffectPresets.cpp` | Look targets: Morning Haze, Ground Mist, Mountain Valley, Horizon Haze, Night | Re-derive the values in our parameterisation |
| Forward fog integration | `.../Includes/forward_fog.glsl` | Blend-mode-aware fog of blended surfaces (we already have it in `output.glsl`) | n/a |
| Cloud march | `Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_march.frag` (381) | Slab/cylinder ray intersection, `SampleDensity` (shape FBM × height gradient, detail erosion weighted by `(1−shape)³`), light march with Beer's law, dual-lobe HG `Phase`, static jitter (their comment at the `randomOffset` line explains why static beats animated without history) | Add a weather map (coverage/type), which they left out. Add sky ambient and multi-scatter octaves, coloured output (they output scalar energy), aerial perspective by height fog, earth curvature, and empty-space skipping. Use blue noise instead of IGN. Drop the resolve/temporal pass. |
| Worley bake | `.../Shaders/volumetric_clouds_worley.glsl`, `_shape_noise.comp`, `_detail_noise.comp`, `_noise_normalize.comp` | Hash-derived tileable Worley, octave cell counts, per-channel min/max normalisation | Port to C++ and bake on the CPU job system (no compute). 64³ RGBA8 shape (R = Perlin-Worley) and 32³ RGBA8 detail, not 128³ RGBA16F. |
| Composite | `.../Shaders/volumetric_clouds_composite.frag` | Over-scene compositing idea | Our single blended composite (§6.4) |
| Vol fog phase/media | `Engine/Modules/VolumetricFog/Shaders/volumetric_fog_common.glsl`, `.../Includes/volumetric_fog_phase.glsl` | HG, energy-conserving step integration | No froxels, compute or temporal |

Licences: OpenEngine is MIT. The height fog is derived from SanielX/Height-Fog (MIT, commit
`ab6f8278`, `Engine/Modules/HeightFog/ThirdParty/SanielX-Height-Fog/UPSTREAM.md`). The clouds are
derived from SebLague/Clouds (MIT, `Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/LICENSE`).
Ported files keep both copyright headers, and `NOTICE.md` gets entries for OpenEngine, SanielX and
SebLague. The blue-noise texture is generated by our own tool. No third-party asset is used.

## 5. Requirements

### Functional
- **R1** `FogType::HEIGHT` (serialized `height`). Fog in this mode is
  `colour·T + S` along the view ray. T comes from the optical depth over `[startDistance, min(d, maxFogDistance)]`:
  - homogeneous below `baseHeight`;
  - exponential above it, with scale height `H = (maximumHeight − baseHeight)/ln(1000)`;
  - extinction `σ = 1/attenuationDistance`.

  S is `L_in·(1−T)`, and opacity is clamped to `maxOpacity`. It works in perspective *and*
  orthographic cameras. World position is reconstructed from `gl_FragCoord` and the pass's inverse
  render view-projection.
- **R2** Lighting model, shared by analytic fog, volumetric fog and local volumes:
  `L_in = fogColor + sunColor · directionalInscattering · 4π·HG(anisotropy, cosθ) · visibility`.
  - `fogColor` is the Color, or in Sky mode the horizon ring (R3).
  - `sunColor` is the main directional light's linear colour × intensity, as the frame block
    (`mainLightColor`) has it.
  - `visibility` is 1 in analytic fog and the shadow term in the volumetric march.
- **R3** Colour Mode **Sky**:
  - For cubemap skies, the CPU computes a horizon ring of 4 azimuth colours (0–5° elevation) once
    per sky texture change, from the same face data `IBLEnvironment::generate` reads. Skybox colour
    and rotation are applied.
  - For library/fork skies that declare a `horizonColor` property, that colour is used for all 4.
  - Otherwise the Fog colour is used, and the inspector shows a warning.
- **R4** Sky fill: sky pixels get the fog colour by `skyFillAngle` (degrees above the horizon, default
  3°, as `GE_HeightFogSkyAmount`), capped by `maxOpacity`. This is applied in the Sky Composite pass
  (R9) for every camera that draws the sky, reflection-probe captures included. No sky shader changes.
- **R5** Volumetric fog (`FogComponent.volumetric`, only with `type == HEIGHT`) raymarches the
  medium of R1 and R2 over `[0, volumetricDistance]`.
  - It adds the sun shadow term (cascaded atlas, 1 comparison tap per step).
  - It adds global density noise (`noiseIntensity/noiseScale/wind`), faded out over the last 20% of
    the distance.
  - It adds `LocalVolumetricFogComponent` densities and their albedo tint.
  - On High it adds up to 4 point/spot lights with `LightComponent.volumetricIntensity > 0`, without
    shadows.

  Beyond `volumetricDistance`, opaque fragments get the analytic term on
  `[volumetricDistance, d]`, so the two segments compose without double counting.
- **R6** Handoff invariant. With volumetric on, a non-shadow-casting sun, no volumes and no noise,
  the image equals analytic-only height fog within the golden tolerance (AC5).
- **R7** `LocalVolumetricFogComponent` ("Local Volumetric Fog"):
  - fields: `shape` (BOX|SPHERE), `size` (Vector3, local, × Transform scale), `attenuationDistance`,
    `albedo` (colour), `blendDistance` (edge fade, m), `falloff` (LINEAR|EXPONENTIAL),
    `noiseIntensity`, `noiseScale`, `scrollSpeed`;
  - the volume uses its Transform (rotation included);
  - a Scene-view gizmo shows the outer and inner (blend) shape;
  - it contributes only through volumetric fog, and the inspector warns when that is off;
  - per view the volumes are culled to the frustum ∩ `volumetricDistance`, sorted by camera
    distance, and capped by the tier (Low 2, Medium 4, High 8).
- **R8** `VolumetricCloudsComponent` ("Volumetric Clouds"). One per scene; the first wins, as
  Skybox does.
  - Fields:
    - `preset` (CUSTOM|SPARSE|CLOUDY|OVERCAST|STORMY);
    - `rendering` (AUTO|SKY_MAP|SCREEN);
    - `coverage`, `density`, `shapeScale`, `shapeFactor`, `erosionFactor`, `erosionScale`;
    - `bottomAltitude` (m, default 1200), `altitudeRange` (m, default 2000);
    - `earthCurvature` (0–1);
    - `windDirection` (deg), `windSpeed` (m/s);
    - `weatherMap` (texture: R coverage, G type; empty means a built-in procedural 256² tileable map),
      `weatherMapScale` (km);
    - `sunLightDimmer`, `ambientLightDimmer`;
    - `maxDistance` (km).
  - Lighting: the sun is the main light, with dual-lobe HG, Beer-powder and 3 multi-scatter octaves.
    Ambient is the scene ambient × `ambientLightDimmer` with a bottom-to-top gradient. A sun below the
    horizon fades out over ±3°.
  - The clouds get aerial perspective: the analytic height-fog term at the transmittance-weighted
    mean cloud distance, applied in the march.
- **R9** Render modes:
  - **Sky Map**: hemi-octahedral world-direction map, RGBA (S.rgb, T).
    - Its back buffer is re-marched 1/N per frame from the current camera position, then swapped.
      The composite crossfades front and back over the next N frames.
    - It is composited by the Sky Composite pass on sky pixels only. This works in every camera.
    - When the camera altitude goes above `bottomAltitude − 100 m`, the clouds fade out over 200 m
      (documented limitation). AUTO switches to Screen instead when the tier allows it.
  - **Screen**: quarter resolution per axis. The march is clamped at the scene depth, and the result
    goes into the Volumetric Composite pass over all pixels, so clouds can hide terrain. Main camera
    only.

  AUTO picks Sky Map on Low/Medium and Screen on High.
- **R10** Composition order inside the colour pass:
  1. opaque queue;
  2. **Sky Composite**: fullscreen at far depth, depth test LEQUAL, no depth write, so it touches sky
     pixels only. It applies Sky Map clouds and sky fill.
  3. **Volumetric Composite**: no depth test; vol fog and Screen clouds; blend
     `dst·(Tc·Tv) + (Sc·Tv + Sv)`.
  4. `takeSceneColor`, so refraction shaders see the fogged background.
  5. per-entity loop;
  6. transparent queue.

  Transparents get analytic height fog over `[0, d]`: RenderSystem resets the handoff start to 0
  before the transparent queue. Both composites run only when the camera sorts by distance
  (`sortsByDistance`, i.e. 3D cameras).
- **R11** Quality tiers `AtmosphereQuality {OFF, LOW, MEDIUM, HIGH}`. Project defaults are
  Desktop HIGH, Mobile LOW, Web LOW. They can be overridden at runtime (Lua/C++) and per editor Game
  view. OFF disables volumetric fog and clouds. Analytic height fog and sky fill are never tiered.
  Tier parameters:

  | | Low | Medium | High |
  |---|---|---|---|
  | Vol fog resolution / steps | 1/4 per axis, 16 | 1/2, 32 | 1/2, 64 |
  | Vol fog local lights / volumes | 0 / 2 | 0 / 4 | 4 / 8 |
  | Clouds (AUTO) | Sky Map 256², N=8, 32+3 steps, no erosion | Sky Map 512², N=4, 64+4 steps | Screen ¼-res, 96+6 steps |

  A component may override its tier downward only (`qualityOverride`: PROJECT|LOW|MEDIUM).
- **R12** Every entry point is static per-pixel deterministic: blue-noise jitter from a 64×64 R8
  texture, static, no frame index. Two consecutive frames of a static scene are bit-identical
  (Sky Map after convergence).

### Platform & backends
- **R13** Everything runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan using fragment shaders,
  2D/3D sampled textures and render targets only. No compute, SSBOs or storage images.
- **R14** Low-res targets are RGBA16F when `renderRGBA16F && filterRGBA16F`. Otherwise they are RGBA8
  with `S' = S/(1+S)` range compression, decoded in the composite. Both paths are tested (AC11).
- **R15** Every texture sample inside a march loop uses explicit LOD (`textureLod`, including the
  shadow comparison sampler), so D3D11 keeps `[loop]`. Loop bounds are compile-time constants with
  `break`. Positions are `highp` (GLES).
- **R16** 3D noise textures need a new `TextureRender` 3D creation path (width, height, depth, RGBA8,
  linear, repeat) in the sokol backend.

### Performance (1080p, reference hardware: "mid desktop" GTX 1660 / RX 5500 class; "mobile" Adreno 650 / Mali-G78 class at 1280×720 render; GPU times via S15 pass timings)
- **R17** Zero cost when disabled:
  - no Fog, or Fog types 0–2: no new passes, targets or uniform uploads, and identical shader keys;
  - HEIGHT without volumetric: no extra passes beyond the Sky Composite, and that only when sky fill
    is on;
  - no clouds component, or tier OFF: no cloud passes, textures or noise bake.
- **R18** GPU budgets:
  - height fog: ≤0.10 ms over linear fog (desktop), ≤0.3 ms (mobile);
  - volumetric fog (downsample + march + denoise + composite): Medium ≤1.0 ms and High ≤1.8 ms on
    mid desktop; Low ≤1.5 ms on mobile;
  - clouds: Medium ≤0.8 ms, High ≤2.5 ms desktop; Low ≤0.6 ms mobile;
  - Sky Composite ≤0.15 ms desktop;
  - the packed-depth pre-pass that vol fog forces when neither SSAO, SSR nor a depth-reading post
    pass runs is reported separately and not counted.
- **R19** VRAM at 1080p: Low ≤4 MB, Medium ≤16 MB, High ≤24 MB (all atmosphere targets plus noise).
  CPU main thread ≤0.1 ms/frame (volume selection, uniform fill).
- **R20** Noise bake (shape 64³ + detail 32³ + weather 256²) runs on the S06 job system:
  - desktop ≤250 ms wall time;
  - the main thread is never blocked for more than 2 ms;
  - on web without threads it is time-sliced at ≤2 ms/frame;
  - clouds fade in over 0.5 s once the bake is ready;
  - the bake is done once per process and shared by every scene.

### Editor UX (human)
- **R21** Fog inspector with Type = Height shows these foldouts, all edits undoable through the
  existing `propertyRow` path:
  - **Height Fog**: Color Mode, Color, Attenuation Distance, Base Height, Maximum Height, Start
    Distance, Max Fog Distance, Max Opacity;
  - **Directional Inscattering**: Intensity, Anisotropy;
  - **Sky**: Sky Fill, Sky Fill Angle;
  - **Volumetric Fog**: Enabled, Distance, Light Shafts, Noise Intensity/Scale, Wind, Quality
    Override, plus a read-only line "Running: Medium · 960×540 · 32 steps · RGBA16F".

  A Presets dropdown sits in the header. The Volumetric checkbox is disabled with a tooltip for the
  other types.
- **R22** GameObject menu › Effects gains "Fog", "Local Volumetric Fog" and "Volumetric Clouds"
  (`CreateEntityCmd`, `ComponentAddDialog`). The Clouds inspector has a Preset dropdown and a
  Weather Map texture slot.
- **R23** The Scene view effects toggle "Fog" also disables vol fog. A new "Clouds" toggle is added.
  Project Settings › Quality gets "Atmosphere Quality" with Desktop/Mobile/Web rows. The Game view
  settings get a "Preview Atmosphere Quality" (Project/Off/Low/Medium/High) editor preference.

### Agent UX (MCP, Lua, CLI)
- **R24** MCP:
  - `create_entity` types `local_volumetric_fog` and `volumetric_clouds`;
  - every field is settable through `set_properties` (Catalog);
  - new tools `list_atmosphere_presets {kind: "fog"|"clouds"}`,
    `apply_atmosphere_preset {entity, preset}` (undoable), and `get_atmosphere_info {view}`;
  - `configure_game_view` gets `atmosphere_quality`, and the Scene display settings get
    `disable_clouds`;
  - project settings `atmosphere_quality_{desktop,mobile,web}`.

  The MCP agent guide (`editor/mcp/McpInstructions.cpp`) and `docs/mcp/TOOLS.md` are updated.
- **R25** Lua:
  - `FogType.HEIGHT` and the new `Fog` properties;
  - classes `LocalVolumetricFog` and `VolumetricClouds` (GameObject handles, all fields as
    properties);
  - enums `FogColorMode`, `FogVolumeShape`, `FogVolumeFalloff`, `CloudsPreset`, `CloudsRendering`,
    `AtmosphereQuality`;
  - `Engine.setAtmosphereQuality(q)` and `Engine.getAtmosphereQuality()`.

  These are documented in `docs/mcp/guide/lua-api.md`.

### Data & compatibility
- **R26** New fields have defaults equal to today's behaviour, so old scenes load and render
  byte-identically (AC1). `type: height` and the two new components bump the scene version through
  S03's mechanism. An older editor preserves unknown components as S03 specifies.
- **R27** Exported games work through these paths:
  - Factory C++ scene code (`editor/Factory.*`);
  - `EntityCloner`;
  - the `ShaderVariantCollector`, which collects `HAS_HEIGHT_FOG` variants and the atmosphere pass
    shaders only for scenes that use them;
  - the project-setting tiers, written into the export.

## 6. Design constraints & decisions

**6.1 Medium on the Fog GameObject.** Volumetric fog is a property of the height fog, as HDRP's Fog
override is, not a separate component. One medium definition gives one source of truth for
analytic, volumetric and transparent fog. Clouds and local volumes are separate components.

**6.2 Shader plumbing.** Retired mesh bit 5 becomes `'Hfg'` → `HAS_HEIGHT_FOG`. It is set together
with `HAS_FOG` when the scene fog type is HEIGHT. Switching to or from HEIGHT calls
`needReloadMeshVariants()`, as fog add/remove does. The variant declares its own block in `fog.glsl`,
and `litFrame` stays untouched:
```glsl
uniform u_fs_heightFog {            // 14 vec4, filled per pass by RenderSystem::updatePassFrame
    mat4 invRenderViewProj;         // inverse of the matrix the pass draws with (GL flip included)
    vec4 viewport;                  // xy viewport offset, zw 1/viewport size
    vec4 color_colorMode;           // linear fog colour, w 0 Color / 1 Sky
    vec4 extinction_base_H_maxOpacity;
    vec4 start_handoff_maxDist_inscatter;   // handoff = volumetricDistance (opaque) or 0
    vec4 sunDir_anisotropy;         // xyz towards the light
    vec4 sunColor_skyFillSin;
    vec4 horizonRing[4];            // Sky colour mode, azimuth 0/90/180/270
} heightFog;
```
The maths lives in `engine/shaderlib/includes/height_fog.glsl`: `heightFogSegment(origin, dir, t0, t1)
→ (S, T)` and `heightFogSkyAmount(dir)`. It is mirrored 1:1 in C++ in `engine/core/render/AtmosphereMath.h`
for unit tests and CPU queries. `getFogColor()` keeps its signature, so library and fork shaders get
height fog with no edits.

**6.3 Passes** (new `ShaderType`s; labels as S15 shows them):
- `Atmosphere/DepthDownsample`: packed `sharedSceneDepth` → low-res RGBA8 packed linear depth
  (farthest of 2×2 or 4×4).
- `Atmosphere/VolumetricFog`: the march, quadratic step distribution, energy-conserving step
  integration. Output (S.rgb, Tv).
- `Atmosphere/Denoise`: one 3×3 depth-weighted pass at low res.
- `Atmosphere/CloudsScreen` and `Atmosphere/CloudsSkyMap`: march.
- `Atmosphere/SkyComposite`: inside the colour pass, at far z.
- `Atmosphere/VolumetricComposite`: inside the colour pass. It does a joint bilateral upsample from
  the 4 low-res neighbours, with bilinear × `exp(−|zLow−zFull|/(0.05·zFull))` weights and a nearest-
  depth fallback. Blend: src ONE, dst SRC_ALPHA. Output `encodeAdditiveOutput(S)` and alpha
  `Tc·Tv`, so blending happens in the engine's display space like every transparent draw.

When vol fog needs depth and none exists, it calls `renderDepthPrePass` the way user post passes do
(`RenderSystem.cpp:13885`). Low-res targets live in the `RenderView` (`rv()`), sized from the colour
target at the render scale. They are recreated only when the size changes by more than 8 px, so S13
dynamic resolution uses a viewport sub-rect instead of reallocating.

**6.4 Shadows.** S10 exposes a fragment-side `u_fs_sunShadow` block: up to 4 cascade matrices, atlas
rects, split distances and bias, filled once per pass. Vol fog binds `u_shadowAtlas` with the
comparison sampler, selects the cascade by view distance, and makes 1 explicit-LOD tap per step. If
the sun casts no shadows, visibility is 1. "Light Shafts" off skips the taps.

**6.5 Noise and blue noise.**
- `engine/core/render/AtmosphereNoise.{h,cpp}` bakes 3D textures deterministically on the CPU (hash
  Worley from `volumetric_clouds_worley.glsl`, Perlin-Worley in shape R), plus the procedural weather
  map, on S06 jobs.
- Blue noise is a committed generated header `engine/core/render/BlueNoise64.h` (64×64 R8,
  void-and-cluster). Its generator is `tools/gen_blue_noise.py`, our own code.

**6.6 Public API sketch**
```cpp
enum class FogType { LINEAR, EXPONENTIAL, EXPONENTIALSQUARED, HEIGHT };
enum class FogColorMode { COLOR, SKY };
struct FogComponent {             // existing fields unchanged; new defaults:
    FogColorMode colorMode = FogColorMode::COLOR;
    float attenuationDistance = 400, baseHeight = 0, maximumHeight = 50;
    float startDistance = 0, maxFogDistance = 2000, maxOpacity = 1;
    float directionalInscattering = 0.2f, anisotropy = 0.6f;
    bool skyFill = true; float skyFillAngle = 3;
    bool volumetric = false; float volumetricDistance = 64; bool lightShafts = true;
    float noiseIntensity = 0, noiseScale = 20; Vector3 wind = {0,0,0};
    AtmosphereQualityOverride qualityOverride = AtmosphereQualityOverride::PROJECT;
};
struct LocalVolumetricFogComponent { FogVolumeShape shape = BOX; Vector3 size{10,10,10};
    float attenuationDistance = 10; Vector3 albedo{1,1,1}; float blendDistance = 1;
    FogVolumeFalloff falloff = LINEAR; float noiseIntensity = 0, noiseScale = 5; Vector3 scrollSpeed{}; };
struct VolumetricCloudsComponent { CloudsPreset preset = CLOUDY; CloudsRendering rendering = AUTO; /* R8 fields */ };
// Engine: static void setAtmosphereQuality(AtmosphereQuality); static AtmosphereQuality getAtmosphereQuality();
```
MCP `get_atmosphere_info` returns
`{quality, platformDefault, fog:{type, volumetric:{active, width, height, steps, format, volumes:{used, culled}, localLights}}, clouds:{active, mode, noiseReady, mapSize, sliceFrames}, passes:[{label, gpuMs?}]}`.
The `gpuMs` value is present when S15 timings exist.

**6.7 Rejected:**
- Per-fragment volumetric sampling in every forward variant: it needs a texture binding in all
  variants.
- A sky-shader fog edit: it would need every library sky and fork changed.
- Temporal accumulation: it violates the temporal-free rule and depends on history on all backends.
- MRT for the depth plus colour outputs: mixed-format MRT risk on GLES3/WebGL2.

## 7. Acceptance criteria

- **AC1** Compatibility. The existing fog scenes (linear/exp/exp²) and a scene without fog render
  pixel-identically before and after, on the GL golden runner (`image_difference == 0`). Shader keys
  for a linear-fog mesh are unchanged.
  - gtest `Atmosphere.LegacyFogKeysUnchanged`
  - MCP `test_atmosphere.py::test_legacy_fog_identical`
- **AC2** Zero cost (R17). With no atmosphere components, `get_atmosphere_info` / S15 pass list
  shows no `Atmosphere/*` pass, and VRAM attributed to atmosphere is 0. With HEIGHT and
  `skyFill = false`, there are still 0 atmosphere passes. MCP `test_atmosphere.py::test_zero_cost`.
- **AC3** Maths. `AtmosphereMath` analytic optical depth matches a 10k-step numeric integration
  within 0.5% across 200 randomised rays (up/down/horizontal, camera above/below base). The segment
  form satisfies `T[0,a]·T[a,b] == T[0,b]` within 1e-5. gtest `AtmosphereMath.*`.
- **AC4** Height fog golden images on 3 scenes: `valley` (perspective, base height 0, sun low),
  `ortho_iso` (orthographic) and `sky_mode_cubemap`. Each matches its reference under
  `tools/mcp/tests/golden/atmosphere/`: mean diff ≤1.0/255 and ≥99% of pixels within 8/255.
  Orthographic fog varies with world height, which the test checks by sampling 2 pixels.
  MCP `test_atmosphere.py::test_height_fog_golden`.
- **AC5** Handoff (R6): volumetric on (non-shadowing sun, no volumes, no noise) vs analytic only
  gives mean diff ≤2/255 at every tier. MCP `test_atmosphere.py::test_volumetric_handoff`.
- **AC6** Light shafts. In the `shafts` golden scene (a pillar between camera and low sun), the
  shadowed fog column's luminance is ≥15% lower than the lit column, at all tiers. With Light
  Shafts off, the difference is ≤2%. Golden images per tier. MCP `test_atmosphere.py::test_light_shafts`.
- **AC7** Local volumes:
  - a box volume adds fog inside its bounds and none 1 m outside `size + blendDistance`, on the
    screenshot sampled across the edge;
  - rotation is respected;
  - with 10 volumes, the Low/Medium/High caps use 2/4/8 (reported in `get_atmosphere_info`) and pick
    the nearest;
  - gtest `Atmosphere.VolumeSelection` covers frustum culling, sorting and the caps.
- **AC8** Clouds golden images: presets Sparse/Cloudy/Overcast in Sky Map (Low, Medium) and Screen
  (High) match their references within the AC4 tolerance. A Sky Map that has converged (N frames
  after the last change) is bit-identical across 2 frames (R12). The `mountain` scene in Screen mode
  shows the cloud in front of the far terrain. MCP `test_atmosphere.py::test_clouds_golden`.
- **AC9** Noise bake: deterministic (a fixed SHA-256 of the bytes), tileable (wrap texels equal
  within 1/255), each channel normalised to the full [0,1] range. Desktop wall time ≤250 ms with the
  main thread blocked for ≤2 ms (measured in the test with the S09 profiler scope). gtest
  `AtmosphereNoise.*`.
- **AC10** Performance (R18/R19), measured with `tools/performance/atmosphere_bench.py` on the
  reference machines. It loads `tools/fixtures/atmosphere/bench.scene` at 1080p (mobile at 720p),
  takes 300 frames, and reads the median of S15 GPU timings per `Atmosphere/*` label. Every budget
  holds. The results are recorded in `docs/mcp/guide/atmosphere.md`. This is a manual/perf-lab gate
  that must run before merge.
- **AC11** Backends:
  - the full MCP atmosphere suite passes on the GL golden runner;
  - the web browser gate (S02) runs the `valley` and `clouds` scenes in WebGL2, both with
    `EXT_color_buffer_float` and with it forced off (RGBA8 path, R14), mean diff vs GL ≤3/255;
  - D3D11, Metal, Vulkan and Android builds compile every atmosphere shader;
  - the D3D11 compile of the High march takes ≤10 s.
- **AC12** Editor:
  - Fog type Height shows the R21 foldouts;
  - each edit is one undo step (MCP `set_properties`, then undo, restores the value);
  - the GameObject menu creates the 3 objects;
  - the Scene view Fog and Clouds toggles remove the effects from the Scene view only.
  - MCP `test_atmosphere_editor.py`.
- **AC13** Presets: `list_atmosphere_presets` returns 5 fog and 4 cloud presets (plus Custom).
  `apply_atmosphere_preset` changes the fields and is undoable. MCP `test_atmosphere_editor.py::test_presets`.
- **AC14** Lua: in Play, a script sets `fog.type = FogType.HEIGHT`, `fog.volumetric = true`, creates
  `VolumetricClouds(scene)` and calls `Engine.setAtmosphereQuality(AtmosphereQuality.OFF)`.
  `get_atmosphere_info` then reflects each step. MCP `test_atmosphere.py::test_lua_api`.
- **AC15** Serialization: a round trip of all new fields; an old scene YAML loads with defaults; the
  version bump goes through S03's migration test; an exported game (Factory path) reproduces the
  `valley` golden image. gtest `Atmosphere.Serialize*` and `tools/smoke-test.sh` export of the
  fixture.
- **AC16** Transparents and refraction: a glass sphere in vol fog takes analytic fog (no shafts) and
  shows the fogged background through `scenecolor` (screenshot check). Documented in the guide.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20) and S02 (gtest, web gate).
  - S03 (scene version bump and unknown-component preservation).
  - S06 (jobs for the noise bake).
  - S10 (fragment-side sun shadow block, §6.4). If S10 slips, S12 adds the block itself, and S10
    adopts it.
  - S15 (GPU pass timings, frame pass viewer) for AC10. Without S15 the passes are still labelled
    with `sg_push_debug_group`.
- **Coordinates with:**
  - S08: if reflection tables have landed, the new components are declared through them instead of
    hand-written Catalog/Stream/Factory code.
  - S11: photometric units change `sunColor` scaling; the fog uses whatever Lit uses. S11 must not
    claim mesh bit 5.
  - S13: dynamic resolution (§6.3 sizing rule), and `.cube` LUTs, which run after us in the uber
    pass, with no interaction.
  - S04: weather map by GUID.
  - S05: weather map hot reload.
  - S24: undo names "Change Fog", "Apply Atmosphere Preset".
- **Merge hotspots:**
  - `engine/core/subsystem/RenderSystem.cpp` (`renderCameraPass`, `fillFrameLighting`,
    `updatePassFrame`): S10, S11, S13, S15.
  - `engine/shaderlib/includes/{fog,output,lit_frame}.glsl`: S11.
  - `ShaderVariantCompiler.cpp` / `ShaderPool.cpp` bit tables: S11.
  - `editor/{Catalog,Stream,Factory}.cpp`, `InspectorWindow.cpp`, `EntityCloner.cpp`: S08, S24.
  - `LightComponent.h`: `volumetricIntensity`; also touched by S10 and S11.
  - `ProjectSettingsWindow.cpp`: S13.

## 9. Risks & defaults

- **Display-space blending** (RGBA8 sRGB targets). The composite is exact only for T∈{0,1}.
  **Default:** accept, as transparents already do. If an HDR scene target ever lands, switch the
  composite to linear blending (this needs no API change).
- **Mesh bit 5 reuse.** A stale cache key from the old `USE_SHADOWS_PCF` era would now mean height
  fog. **Default:** the stream bumps the shader-cache version, so old `.sdat` files are rebuilt.
- **Static jitter pattern at Low** (1/4 res). **Default:** the denoise pass plus bilateral upsample.
  If the pattern is still visible in AC6 review, Low drops to a 5×5 denoise, which costs about
  0.1 ms.
- **Vol fog forces a depth pre-pass** in scenes without SSAO/SSR. **Default:** allowed, and reported
  in `get_atmosphere_info` and the inspector "Running" line.
- **Sky Map parallax and camera altitude.** **Default:** fade near the cloud base; AUTO uses Screen
  on High. Flight games set `rendering = SCREEN`.
- **D3D11 loop unrolling or compile time.** **Default:** explicit LOD everywhere (R15). If compile
  still exceeds 10 s, cut the High maximum step constant to 64 primary.
- **Web noise bake time.** **Default:** time-sliced, with clouds fading in. A future option is caching
  the bake in the export, which is not done in v1.
- **Transparents lack shafts and local volumes.** **Default:** documented limitation, consistent
  through the shared medium (R10).
- **Sky colour mode for procedural skies** without `horizonColor`. **Default:** use the fog colour
  and show an inspector warning.

## Deviations

- **No scene format version bump (R26, AC15).** S03 landed with the rule that adding an optional
  component or field needs no version bump: every new key is registered in
  `editor/StreamSchema.cpp` (`knownFieldKeys`, the `FogType` table's `height`, the new enum
  tables), so an older editor keeps a height fog's fields and the two new components as unknown
  data and writes them back (S03 preservation), and a newer one reads old scenes with the
  defaults. A bump would only make older editors refuse to save. The new Fog and Light fields are
  written only when they differ from their defaults, so distance-fog scenes save byte-identically.
  Tests: `editor/tests/stream/StreamAtmosphereTest.cpp`, `StreamSchemaTest` (every component's
  keys known).
- **Golden images live with the engine's GPU tests.** S02 put rendering goldens in
  `engine/tests/core/golden/` (`*Gpu` suites on the headless device, glcore and Vulkan), so the
  AC4/AC5/AC6/AC8 images are `atmosphere_*.png` there, checked by
  `engine/tests/core/render/AtmosphereGpuTest.cpp`, not under `tools/mcp/tests/golden/`. AC1's
  legacy goldens (`atmosphere_legacy_*.png`, `AtmosphereLegacyFogGpuTest.cpp`) were rendered by
  the engine before S12 and must match exactly. The MCP tests (`test_atmosphere.py`,
  `test_atmosphere_editor.py`) cover the agent and editor features.
- **Sky pixels under volumetric fog (R6).** The marched fog in front of the sky is drawn over the
  sky fill (the physically right result, and where light shafts show against the sky), so the
  handoff invariant holds for surfaces; AC5 is measured on a view of geometry.
- **Handoff by replacement, not by split (§6.3, R5, R6, R10).** Blending a marched near segment
  over surfaces fogged from the volumetric distance on is a display-space blend (the colour
  targets hold display-encoded colour), which is far from the analytic fog for half-transparent
  fog (about 16/255 in the AC5 scene). Instead every opaque surface takes the analytic fog over
  its whole ray, and `Atmosphere/VolumetricComposite` replaces the analytic near part [0, m] with
  the marched one: `colour' = ((colour T_c + C) - A) k + V`, `k = T_v / T_a`, A, V and C the
  analytic, marched and Screen cloud layers (A computed per full-resolution pixel in the
  composite). Where the march equals the analytic fog, k = 1 and V = A: the image is unchanged
  (AC5 measures 0.5-0.8/255). It draws twice, with a new `BlendMode::SUBTRACT_PREMULTIPLIED`
  (reverse subtract) for the part of the correction below zero, then premultiplied. The block's
  `start_handoff_maxDist_inscatter.y` is `start_volumetric_maxDist_inscatter.y`: the distance the
  pass marched.
- **Sun shadow block (§6.4).** S10 has not landed: `shaderlib/includes/sun_shadow.glsl` declares
  `u_fs_sunShadow` (cascade matrices, atlas rects, split distances, bias), filled from the camera's
  current shadow uniforms, with S10's `sampleDirectionalShadow1` signature, for S10 to adopt.
- **Pass timings (AC10).** S15 has not landed: passes are labelled, `get_atmosphere_info` lists
  them, and `tools/performance/atmosphere_bench.py` measures frame time with each feature on and
  off (it reads `gpuMs` once S15 provides it). The perf-lab run on the reference hardware is
  still to be made; `docs/mcp/guide/atmosphere.md` states the budgets.
- **Preset field.** The clouds' `preset` shows the last look applied; editing a field does not
  switch it to Custom.
- **Determinism hash (AC9).** The bake's SHA-256 is pinned for x86-64 Linux builds; on other
  toolchains the test checks that two bakes are identical (libm may round differently).
- **Bake budget scales with the workers (AC9).** `AtmosphereNoiseTest` allows
  250 ms x max(1, 4 / background workers): the bake is spread over S06's Background lane, so a
  shared CI host with fewer workers gets proportionally more time.
- **Web gate (AC14).** `tools/web/gates/atmosphere.json` checks the Low-tier valley scene's centre
  pixel and the tier's console line in a WebGL2 browser; the forced RGBA8 path is compared with
  the float one by `AtmosphereGpu.LowPrecisionTargetsMatchFloatOnes` (glcore).
- **D3D11/Metal compile (AC11).** Checked on Linux with the engine's shader compiler (glslang +
  SPIRV-Cross, as an export does): every atmosphere program and the height fog mesh variants for
  GLSL 410, GLSL 300 es, HLSL 5, MSL macOS/iOS and SPIR-V (`editor/tests/shader/
  AtmosphereShaderBuildTest.cpp`; the editor CLI `shaders --backend all`). The HLSL march takes
  about 0.25 s to produce; the driver's own D3D compile on Windows was not timed.
- **Noise bake without workers.** A web build without threads bakes on the main thread, half the
  last frame's time per frame (4 to 250 ms), so a slow frame does not stretch the bake: the debug
  WebGL2 gate on SwiftShader (about 1 fps) went from no ready line in 900 s to ready in 92 s.
- **Not covered by a dedicated test:** water refraction under fog (AC16 relies on the water pass
  sampling the fogged scene colour, unchanged by S12), and the AC10 reference-hardware run.
