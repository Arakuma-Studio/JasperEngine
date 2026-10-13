# Shader library, phase 4: fast post-processing

Status: approved scope, refined against the merged phase 1-3 code (tag `shader-library-phase3`).
Depends on: phases 1-3, in particular `includes/output.glsl` (`finalizeColor`), the frame
block and the existing user post-process chain.
Research: `/tmp/shader-research/fast-postprocessing.md` (sources, cost model, pitfalls).

## 1. Goal

A post-processing stack that is much cheaper than Unity's on mobile without visible
quality loss. Unity's cost on mobile comes mostly from full-resolution round trips (every
effect, even vignette alone, renders the frame to an intermediate target and back), not
from the maths. This stack avoids round trips:

| Tier | Effects | Extra passes |
|---|---|---|
| T0 in-shader | exposure, tonemap, colour grade (maths or strip LUT), vignette, dither | 0 |
| T1 uber pass | FXAA, chromatic aberration, pixelate, CRT, grayscale/sepia, edge detect, film grain, sharpen, upscale | 1, replacing the blit the engine already does for fixed resolution |
| T2 bloom | bloom, blur | 7 small passes starting at quarter resolution |
| T3 opt-in | user passes, depth-reading effects | 1 each |

Target (estimate from the research, 1080p, tonemap + grade + vignette + chromatic
aberration + bloom + grain + FXAA): 8 passes, 1 full-resolution pass, about 1.2 screen
areas shaded, versus Unity URP 19 passes / 2 full-resolution / 2.8 screen areas.

## 2. Requirements

### 2.0 Decisions and conventions (binding)

- **Settings:** one plain struct `PostProcessingSettings` in `engine/core/SceneSettings.h`
  (`SceneSettings::postProcessing`), defined in full by the first phase 4 agent (every
  effect's enable flag and parameters, render scale, MSAA), so later agents build on it.
  The existing user chain (`SceneSettings::postProcess`, `PostProcessPass`) stays and is
  extended (§2.5). Serialization, Factory codegen, Catalog, Lua, MCP and the inspector follow
  phases 1-3 (§10 of phase 1). Default-constructed = today's output, byte-identical saves.
- **T0 implementation:** exposure + tonemap + colour grade are baked on the CPU into one small
  strip LUT (e.g. 32x32x32 laid out 1024x32, or 16^3 on low end) whenever the settings change;
  shaders apply it with one texture tap in `finalizeColor` (and the `finalize*` variants),
  plus vignette and dither maths. The LUT and its parameters reach shaders without growing
  the per-draw upload beyond a few bytes (pack into existing engine blocks or a tiny block)
  and within the sampler budget: the built-in Lit worst case already uses 15 of 16 samplers
  per stage — count and prove every variant still fits on every backend, or bake the
  vignette/dither into shaders without a new sampler for the worst case and document.
  A global define (part of the variant key, one switch per setting toggle, never per object)
  selects T0 on/off; when T1/T2 is active, the colour work moves to the uber pass instead.
  UI shaders never apply it.
- **Precision:** SPIRV-Cross emits `highp` for everything today (phase 1 notes). Post shaders
  (uber, bloom, blur) declare `mediump` explicitly where precision allows so GLES gets fp16
  (`RelaxedPrecision`); prove on the GLES3 output that the qualifiers come through.
- **Measurement:** reuse `JASPERENGINE_FRAME_STATS` (phase 3) to report passes, draws and
  target sizes per frame; §2.7 extends it with a per-pass list (name, target size).
- **Backends:** verify on GL, Vulkan and GLES3 at runtime (`SMOKE_BACKEND`), all six by
  compilation.
- Phase 1 §13, phase 2 §5 and phase 3 §4 notes are binding (finalize* functions, frame block,
  scene inputs: a post pass reading depth uses the phase 3 depth sources).

### 2.1 Settings

- Post-processing settings live on the scene (main camera), serialized, with editor UI
  ("Post-processing" section with presets: None, Mobile, Console, Stylised), Catalog paths,
  Lua and MCP access. Defaults keep today's output exactly (everything off).
- Every effect has an enable flag; disabled effects cost nothing (no pass, no shader work).

### 2.2 T0: in-shader colour

- `finalizeColor` (phase 1 section 6.2) applies, in order: exposure, tonemap
  (`@keywords`-style choice: none, ACES fitted, Khronos PBR Neutral, Reinhard, AgX
  approximation), colour grade (contrast, saturation, lift/gamma/gain, temperature/tint,
  or a 2D strip LUT texture), vignette (intensity, smoothness, roundness, colour), dither
  (to remove banding in the 8-bit target), then display encoding.
- Applied by every mesh, library, sky, points and lines shader; UI is excluded.
- When T1 or T2 is active, colour work moves to the uber pass instead (so bloom and
  chromatic aberration are graded once); shaders never apply it twice.
- Switching the scene's colour settings at runtime must not cause per-object variant
  churn: at most one global variant change per setting toggle, values via uniforms.
- The trade-off is documented: transparent objects blend after tonemapping, as in Godot's
  mobile renderer and Unreal's mobile LDR mode.

### 2.3 T1: uber pass

- One fullscreen pass that replaces `blit.frag` when fixed resolution is on and that is
  added (one pass) otherwise, with compile-time features for: FXAA (console-quality
  path, luma from green), chromatic aberration, pixelate, CRT (scanlines, aperture mask,
  curvature, vignette), grayscale/sepia, Sobel edge detect (on luma, aspect-correct),
  film grain, sharpen, bloom composite, colour grading (when T0 moved here), upscale.
- Texture tap UVs computed in the vertex stage where possible; `mediump` where safe.
- Fixed resolution: the uber pass is the upscale, so enabling T1 adds no full-resolution
  pass versus today.

### 2.4 T2: bloom and blur

- Dual-filter bloom: a 4-tap prefilter with threshold/knee from the scene straight to 1/4
  resolution, then 3 downsamples (5 taps) and 3 upsamples (8 taps); `SRGB8A8` targets
  (renderable in core GLES 3.0) so filtering happens in linear light; intensity, tint,
  scatter; composite in the uber pass.
- The same chain provides a blur effect (screen blur, and a blurred background texture the
  UI can sample for frosted panels).

### 2.5 User passes

- `PostProcessPass` gains custom textures (manifest `@texture`, phase 1), access to the
  original scene colour as well as the previous pass, an optional resolution scale, and
  library post-process shaders (`@shader postprocess`): `crt`, `pixelate`, `posterize`,
  `edge_neon`, `gameboy`, `blur`.

### 2.6 Render scale and targets

- Render scale for the 3D scene (0.5-1.0) with native-resolution UI, upscaled in the uber
  pass.
- Render-target hygiene from the research: colour load action DONTCARE/CLEAR when the
  previous contents are not needed (CLEAR on GLES, where DONTCARE does nothing), depth
  store only when depth is sampled later, check the GLES3 depth invalidation condition in
  sokol against upstream, avoid redundant intermediate targets.
- Optional MSAA (2x/4x) for the main camera as the high-quality AA option on tile-based
  GPUs.

### 2.7 Measurability

- A debug report (log line or editor overlay) listing, per frame, every pass with its
  target size, so the pass budget in section 1 can be verified.

## 3. Verification

- Compile matrix covers the uber pass feature combinations, bloom passes and library post
  shaders on all six backends.
- The fixture gains a post-processing scene toggling each preset; the smoke test passes and
  screenshots show each effect; the pass report matches the budget table for each tier.
- With post-processing off, output and pass list are identical to before phase 4.
- Phase 1-3 regression checks still pass.

## 4. Definition of done

All requirements implemented, all checks pass, merged into `shader-library`, review pass
found no open correctness bugs.

## 5. Implementation notes

Decisions taken while implementing phase 4, and deviations from the text above. They override
the sections they refer to; later agents follow them. The working contract for the bloom/blur,
user-pass and editor work is also written out in `/tmp/shader-research/p4-api.md`.

### 5.1 Core (sl/p4-core)

- Settings (§2.0, §2.1). `PostProcessingSettings` (`engine/core/SceneSettings.h`/`.cpp`,
  `SceneSettings::postProcessing`, `Scene::get/setPostProcessing`; the setter clamps and ignores an
  unchanged value) holds every effect: exposure (stops), `TonemapMode` (NONE, ACES = Narkowicz's fit,
  NEUTRAL = Khronos PBR Neutral, REINHARD, AGX = minimal AgX), the grade (white balance, contrast,
  saturation, lift/gamma/gain, a LUT strip with contribution, LUT side 16/32), monochrome
  (grayscale/sepia), vignette, dither, FXAA, chromatic aberration, pixelate, CRT, edge detect, film
  grain, sharpen, bloom (threshold, knee, intensity, tint, scatter), blur (radius, intensity), render
  scale 0.5-1, MSAA samples. Colours are sRGB. No master switch: every effect has its enable flag
  or neutral value. A field table (`PostProcessingSettings::getFields()`: name, snake name, type,
  accessor, enum names, range, help) drives the scene YAML (`postProcessing:`, fields away from their
  defaults only, no key while all are default), the exported scene code
  (`Factory::formatPostProcessing`), the Catalog scene paths (`post_processing`, the whole struct, and
  `post_processing.<snake name>` per field) and MCP `get/set_scene_settings` (the `post_processing`
  object merges, `null` resets; every post-processing key of one call is one command). Lua:
  `scene.postProcessing` (a copy), `PostProcessingSettings`, `TonemapMode`, `MonochromeMode`. The
  inspector section and the presets are the editor wave's. Editor views: the Game view (its own
  pass or a second view) draws the scene's post-processing; the Scene view does not unless
  `SceneDisplaySettings::showPostProcessing` (default off, no UI yet) or a camera preview
  (`RenderSystem::setDisablePostProcessing` for the edit-mode frame, `Engine::ViewLayer::
  postProcessing` / `ViewDesc::postProcessing` for a second view). Off, the colour passes bypass
  the post colour (no variant change) and the uber pass is skipped (MCP test
  `test_post_processing_views.py`).
- Tiers (`RenderSystem::getPostProcessingTiers`). The uber pass runs when FXAA, chromatic
  aberration, pixelate, CRT, edge detect, grain, sharpen, bloom or blur is on, or the render scale
  is below 1. Without it the shaders do everything colour-only (T0). With it the grade, monochrome,
  vignette and dither move into the uber pass, and **exposure and the tone mapping curve stay in the
  shaders** (deviation from §2.0/§2.2's "the colour work moves to the uber pass"): the scene target
  is 8-bit and clamped, so a curve applied there cannot compress highlights the target already
  clipped (ACES of a clipped lamp is grey). Grayscale/sepia is a colour transform: it is baked into
  the grade LUT (zero cost, works in T0 too) rather than being an uber feature.
- T0 (§2.2). One global variant bit, 'Pcl' (`USE_POST_COLOR`): MESH bit 31, POINTS bit 4, LINES bit 2,
  SKYBOX bit 0 (`ShaderPool::getPostColorProperty`); UI never. Turning it on or off is one
  `needReloadMeshVariants` (meshes keep drawing their old variant until the new one is built) plus a
  points/lines/sky reload; value changes rebake data only. Everything colour is one strip LUT
  (`util/PostColorLut`, baked on the CPU when the settings change, RGBA8, 32 KB at side 16, 128 KB at
  32) sampled through the shaper `sqrt(x / (x + 1))` of the linear colour (near-linear in the display
  range; the clamp of tone mapping NONE costs at most about 1/255 at the white point), with two taps
  and a lerp; vignette and dither are maths on `gl_FragCoord` (`includes/post_color.glsl`).
  `output.glsl`: `encodeOutput` (a shader's own colour), `encodeAdditiveOutput` (an additive term:
  black adds nothing, the vignette only darkens it, no dither); `encodeColor` stays the plain encoding
  (the scene colour, which is graded already, and filters); `finalizeFoggedColor` applies no post
  colour (its colour is the scene colour); the multiply blend takes the plain encoding; the
  premultiplied blend grades the straight colour. Glass uses them for its reflection and glint.
  Transparent objects blend after the curve (display values), as Godot's mobile renderer and Unreal's
  LDR mode.
- T0 parameters and budget. Three vectors (`post_color_t`): before the built-in Lit's camera block
  members (`lit_frame_block_t`, so a variant without them keeps its prefix), after the frame block's
  (fragment stage only: `ShaderLibraryBindings::frameSizeFS`), or in a 48-byte block of their own
  (`u_fs_postColor`) for shaders with neither. Per draw: +48 bytes to a block already uploaded, or one
  extra 48-byte upload (sky, points, lines, unlit meshes). Measured on the fixture (130 draws): 311
  uploads / 96336 bytes off, 320 / 98160 with every T0 effect on. The LUT is one texture and sampler:
  the reachable Lit worst case was already 16 textures (a painted terrain with normal map, shadows,
  IBL and SSAO), so a painted terrain (`HAS_TERRAIN_PBR`) with the post colour leaves its base
  material's emissive texture out (the emissive factor still applies; documented in mesh.frag). Every
  built-in variant compiled (86 specs x 6 backends, Pcl ones included) stays within 8 blocks and 16
  textures/samplers per stage (largest: 8 blocks, 16 textures); the library matrix
  (`shaders --library`) compiles every library shader with the bit (post colour cases, every blend
  mode) and now fails a variant over the budget (`ShaderVariantCompiler::checkResourceBudget`; the
  shader builder warns). Pre-existing: a painted terrain with 2D lights and 2D shadows is over the
  budget without phase 4 (17 textures), unchanged.
- Which passes. Only the post-processed camera's colour pass gets the parameters and the LUT: the
  main camera of the base layer (or a main camera drawing into a texture), as the user chain;
  `updatePassFrame` makes every other pass neutral (LUT side 0 = plain `linearTosRGB`, no vignette or
  dither): render-to-texture cameras, mirrors, reflection probes, the Scene view. A camera texture
  shown on a mesh in the main camera is therefore graded once.
- T1 uber pass (§2.3). `ShaderType::BLIT` variants: properties 0 is the plain blit (blit.frag,
  unchanged), feature bits select `uber.vert`/`uber.frag` (`RenderSystem::UberFeature`: FXAA,
  chromatic aberration, pixelate, CRT, edge, grain, sharpen, grade LUT, vignette, dither, bloom, blur;
  'Fxa' 'Cab' 'Pix' 'Crt' 'Edg' 'Grn' 'Shp' 'Lut' 'Vig' 'Dth' 'Blm' 'Blr'). FXAA is the 3.11 console
  algorithm on green; sharpen and the Sobel edge share its four corner taps (half a texel off, so
  their sums are exactly a 3x3 tent and the Sobel kernels), computed in the vertex stage unless the UV
  is warped (pixelate, CRT curvature). Colour maths is `mediump` (the GLES3 output has `precision
  mediump float` with `highp` UVs and texel sizes). Bloom and blur are hooks: `u_bloomTexture`
  (`sqrt(c*c + bloom * tint * intensity)`) and `u_blurTexture` (mixed by blurIntensity), bound from
  `RenderSystem::uberBloomTexture` / `uberBlurTexture` (null: black / the scene itself), so the
  variants compile and draw before the bloom chain exists.
- Where it runs. With fixed resolution the uber pass is the blit (the fixed target is its source:
  no extra pass). Otherwise the post-processed camera draws into `RenderView::postSceneFramebuffer`
  (view rect x render scale, linear) and the uber pass draws it to the destination (the view's target,
  a render-to-texture main camera's framebuffer, or the swapchain: `swapchainRedirect` turns on while
  the tiers ask for it). SSR and the user chain (§2.5's T3) run before it, at the scaled size, into its
  source. Turning the uber pass on at runtime in an exported game switches the scene camera
  offscreen (offscreen pipelines for the objects, as SSR does). Measured with `PP_CYCLE=1`
  (`JASPERENGINE_FRAME_STATS=all`, 28 cases 3 s apart, GL): every frame draws all 130 draws but
  one, the first switch of the colour bit, where the points, lines and sky (which reload, unlike
  meshes, which keep drawing their variant) waited one frame for their variant compiled at runtime;
  each switch of the colour bit makes about 70 pipelines (the new variants), a switch between uber
  variants 2. Point sprites keep their size in pixels of the scaled target (twice as large on
  screen at render scale 0.5), as with fixed resolution.
- UI (deviation from §2.6's "native-resolution UI" and "UI excluded" in T1). UI components drawn by
  the main camera are part of its image (their draw order interleaves with sprites in 2D scenes),
  so the uber pass and the render scale reach them, as fixed resolution and the user chain already
  did; UI shaders never apply the T0 colour. A HUD that must stay untouched and at native resolution
  goes in a layer scene (`Engine.addSceneLayer`, a child scene), which draws after the uber pass; the
  fixture shows it.
- MSAA (§2.6): `msaaSamples` is stored, serialized and reported but not applied: the multisampled
  scene target (sample count and resolve attachment in `SokolFramebuffer`, `PIP_RTT` pipelines with
  that sample count) belongs with the render-target work; it does not turn the uber pass on.
- Measurement (§2.7). `JASPERENGINE_FRAME_STATS` lines end with the frame's pass list
  (`; pass list: shadow 6144x2048 x2, camera rtt 480x270, camera 1280x720, uber 1280x720, camera
  layer 1280x720`): label and size of each render pass (`CameraRender::setLabel`,
  `SokolSystem::recordPass`). With the ready signal a "Post-processing:" line names the tiers and
  uber features. Measured on the fixture (GL): off and T0 (curve, grade, vignette, dither) both
  `shadow x2, camera rtt, camera 1280x720, camera layer` (5 passes; T0 adds none); T1 (FXAA,
  chromatic aberration, grain) adds `uber 1280x720` (6 passes); T1 with fixed resolution 640x360
  `camera 640x360, uber 1280x720` (6 passes, as the plain fixed-resolution blit:
  `camera 640x360, blit 1280x720`); render scale 0.5 `camera 640x360, uber 1280x720`.
- Export. The variant collector takes the scene's colour bit (both states when a project script
  mentions `postProcessing`, `Project::scriptsSetPostProcessing`) and the uber variant of the scene's
  settings; uber features a script turns on compile at runtime.
- Fixture `tools/fixtures/post-processing` (`PP_CASE`, `PP_SCENE=saved`, `PP_CYCLE=1`; its script's
  header lists the cases).
