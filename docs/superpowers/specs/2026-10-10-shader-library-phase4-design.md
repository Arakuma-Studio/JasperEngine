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

### 5.2 Effects (sl/p4-effects)

- Bloom (§2.4). `ShaderType::BLOOM` (appended; `bloom.vert` / `bloom.frag`, property bits 'Pre'
  prefilter, 'Thr' threshold, 'Ups' upsample, 'Mix' level mix; 0 is the downsample). Every tap UV
  is computed in the vertex stage; colour maths is `mediump` (the GLES3 output has
  `precision mediump float` with `highp` UVs). `RenderSystem::renderBloomChain`: a 4-tap prefilter
  from the uber pass's source straight to 1/4 (taps one source texel off the centre: the 2x2
  quadrant centres of the 4x4 box), each tap decoded with the gamma-2 approximation (`c*c`, which
  the uber composite `sqrt(c*c + bloom)` inverts) and a soft-knee threshold on the brightest linear
  channel (threshold squared: it is a display brightness; knee = threshold x bloomKnee, Unity's
  curve); then 3 dual-filter downsamples (5 taps, one texel of the larger image off) and 3
  upsamples (8 taps, half a texel of the smaller), each upsample mixed with the down level of its
  size by `bloomScatter` (0 the 1/4 level alone, a tight glow; 1 the widest blur; this needs the
  up targets apart from the down ones: 7 targets, 0.09 of a screen of memory). Targets are
  `SRGB8A8` without depth (`ColorFormat::SRGB8A8`, `FramebufferDepth::NONE`, pipeline `PIP_POST`,
  no blending): the hardware decodes before filtering and encodes on write, so the chain filters
  in linear light. SRGB8A8 is colour-renderable and filterable in core GLES 3.0 (`GL_SRGB8_ALPHA8`),
  on GL 3.0+ (sokol enables `GL_FRAMEBUFFER_SRGB` for offscreen passes), D3D11
  (`R8G8B8A8_UNORM_SRGB`: render target and filtering required from feature level 10), Metal
  (`RGBA8Unorm_sRGB`, all features) and Vulkan (`R8G8B8A8_SRGB`: sampled-filter, colour attachment
  and blend are mandatory format features); run on GL, GLES3 and Vulkan. Targets live per view
  (`RenderView::bloomTargets`), are made again in place when the source size changes (destroyed
  first, no pile-up) and freed when the effect goes off. Pass labels "bloom prefilter", "bloom
  down", "bloom up"; every pass CLEARs (a tiler loads nothing). The uber pass's `u_bloomTexture`
  is the chain's 1/4 result.
- Blur. The same chain without threshold or mix (`renderBlurChain`, labels "blur ..."), its
  dual-filter levels from `blurRadius` (`RenderSystem::getBlurLevels`: ceil(radius), 0..3, the
  taps spread by radius / levels; 0 is the quarter-resolution box alone): 1 + 2 x levels passes
  (3 at the default radius 1, 7 from radius 2.01). The uber pass mixes it by `blurIntensity`
  (decoding it: `sqrt`), 0 while the chain has no texture yet. Frosted panels: the blurred scene
  is bound by name, `u_blurredSceneTexture` / `u_blurredScene_smp` with the block
  `u_fs_blurredScene` (`includes/blurred_scene.glsl`: `blurredSceneUV()` from `gl_FragCoord`,
  `blurredScene(uv)` the engine's linear colour, `blurredSceneAvailable()`), to any shader
  declaring them (UI, mesh, points, lines, sky: `ShaderLibraryBindings`). It exists for the passes
  drawn after the base layer's uber pass in the same view, i.e. layer scenes (a HUD), which is
  where a panel over the post-processed scene is drawn; in the blurred scene itself (whose image
  the blur is made of: a panel there would blur itself frame after frame) and without blur it
  is black with `available` 0. `blurIntensity` 0 makes only the texture. Library UI shader
  `frosted_panel` (tint, tint amount, brightness; its tint alone without a blurred scene).
- User passes (§2.5). `PostProcessPass` gains `textures` (by uniform: the manifest's `@texture`,
  or any name for a shader without a manifest; bound with the manifest default when unset) and
  `scale` (0.1..1, the pass's output resolution). A pass may declare `u_originalSceneTexture`
  (the scene before the chain): when a pass after the first does, buffer 0 keeps it and a third
  full-size target is made. Passes below full size draw into per-size targets (two per size,
  freed in a frame no pass uses that size); a last pass below full size is copied to the
  destination ("user post-process copy"). With the uber pass, the chain's last output IS the
  uber pass's source: `postSceneFramebuffer` is not made (one full-size target less than 5.1).
  `@shader postprocess` (manifest type, `fullscreen.vert`, parameters in `u_fs_postParams` with
  `@property` defaults): library `crt`, `pixelate`, `posterize`, `edge_neon`, `gameboy`, `blur`;
  the library matrix compiles them (one variant per feature set), `docs/shaders/library.md` has a
  Post-processing section, `list_shaders type=postprocess`. Data paths: scene YAML `textures` /
  `scale` and exported code only when set (a chain without them saves and generates as before),
  Lua `pass:setTexture(uniform, path)`, `getTexture`, `pass.scale`, MCP `post_process` items
  `textures` {uniform: path} and `scale` (refused outside 0.1..1; library textures checked against
  the manifest); asset renames rewrite pass textures. A pass's own textures are rebound every draw;
  changing them reloads the chain. The user chain's passes now CLEAR (they loaded).
- Render-target hygiene (§2.6). Measured with `JASPERENGINE_FRAME_STATS_ACTIONS=1` (new: each
  pass list entry ends with `{colour load, depth store}`): every fullscreen pass CLEARs (uber,
  blit, bloom, blur, user passes, SSR composite...); only the camera layer (it draws over the
  scene) and the point-shadow faces after the first LOAD (the projective shadow atlas lists
  `load`: it has no colour attachment); depth is stored only by the projective
  shadow atlas (sampled later) and split colour passes (resumed after the scene-colour copy). The
  GLES3 invalidation: sokol's `_sg_gl_end_render_pass` invalidated depth and stencil only when the
  pass had NO depth-stencil attachment (`if (!atts->ds_view)`, identical in upstream sokol master
  of 2026-10-13), so on GLES a DONTCARE depth was never invalidated; fixed in the vendored header
  (`if (atts->ds_view)`; GL ignores invalidating an attachment a depth-only framebuffer lacks).
  Vulkan/Metal/D3D11 already honoured DONTCARE, so nothing relied on the GLES behaviour.
- MSAA (§2.6). `msaaSamples` (clamped to what the device has: `getMaxMsaaSamples` asks
  `GL_MAX_SAMPLES`, Vulkan's framebuffer colour/depth/stencil sample counts, D3D11 quality
  levels; lavapipe has 1 and 4, so `msaa2` runs without MSAA there, llvmpipe GL rounds 2 up to 4)
  gives every object `PIP_MSAA` (and `PIP_MSAA_NODEPTH`, `PIP_ZPREPASS_MSAA`) pipelines at that
  count, and the post-processed camera's colour pass a multisampled colour + depth-stencil target
  (`RenderView::msaaFramebuffer`) resolving at the end of the pass into the offscreen target it
  would draw into: the fixed-resolution target, the uber pass's scene target, SSR's scene colour,
  the chain's buffer 0, a render-to-texture main camera's framebuffer or the editor's view
  target. So SSR, the user chain, scene inputs (the scene-colour copy splits the pass: the split
  stores and resolves, the resume loads), bloom and the uber pass all read the resolved colour;
  the G-buffer, SSAO and scene-depth pre-passes stay single-sampled (their own targets). Fixed
  resolution and render scale: the multisampled target is at that size. Exported builds without
  another offscreen effect draw to the swapchain: the scene resolves into a target blitted to it
  (one full-resolution pass more: "camera 1280x720 msaa 4, blit 1280x720"; with any T1/T2 effect
  the resolve is free, inside the colour pass). The samples are never stored (DONTCARE: resolved
  in the tile on Metal and Vulkan tilers; GLES resolves with glBlitFramebuffer, the research's
  caveat). Not applied: mirrors' inverted-winding passes and render-to-texture cameras other than
  the post-processed one, the edit-mode Scene view (no post-processing), layer scenes; a change of
  sample count reloads the objects. FXAA and MSAA can both be on (the editor advises one).
- Runtime switching. (a) The colour bit: points, lines and sky no longer reload at once; they keep
  drawing their variant until every new one is built (`postColorVariantsReady`, asking for them),
  then reload together (meshes already did, phase 3). Measured with `PP_CYCLE=1
  PP_CYCLE_CASES=off,aces,off,bloom,mobile_full,blur,frosted,msaa4,msaa4_bloom,lib_crt,
  user_pass_bloom,off,tail,off` (`JASPERENGINE_FRAME_STATS=all`, GL): every frame from the first
  ready one draws all 134 draws (142 with a bloom chain), including the colour bit switches
  (5.1 measured one frame without points, lines and sky); a frame whose uber variant is still
  compiling at runtime draws without the uber pass. (b) Export: `ShaderVariantCollector::
  plausibleUberFeatures`: the scene's uber variant, and when scripts mention `postProcessing`, the
  variants of the switches they name (`getScriptableEffectFields`: the nine uber effects, bloom,
  blur, and the grade, vignette and dither the uber pass takes; `Project::
  scriptsPostProcessingFields`): every combination when they name up to 4, else each flipped
  alone over the scene's settings, and each switch flipped alone when they name none (at most
  13 variants); the bloom programs those need (5 at most). Anything else a script reaches
  compiles at runtime (as before).
- Pass budget (§1), measured on the fixture (GL, GLES3 and Vulkan agree; `mobile_full`: ACES,
  grade, vignette, dither, FXAA, chromatic aberration, bloom, grain): `shadow 6144x2048 x2, camera rtt
  480x270, camera 1280x720, bloom prefilter 320x180, bloom down 160x90, bloom down 80x45, bloom down
  40x22, bloom up 80x45, bloom up 160x90, bloom up 320x180, uber 1280x720, camera layer 1280x720`. After the scene:
  8 passes (7 bloom + uber), 1 at full resolution, as the table; shaded area after the scene
  1 + 0.0625 + 0.0156 + 0.0039 + 0.0010 + 0.0039 + 0.0156 + 0.0625 = 1.165 screens. Bloom's
  targets: 320x180, 160x90, 80x45, 40x22 (+ 80x45, 160x90, 320x180 up). This matches the editor's
  cost estimate (5.3) for bloom; its blur count now follows `getBlurLevels`. Other measured lists
  (GL): blur radius 2.5 `blur prefilter 320x180, blur down x3, blur up x3, uber`; blur radius 1
  `blur prefilter 320x180, blur down 160x90, blur up 320x180, uber`; `msaa4` (exported, nothing
  else) `camera 1280x720 msaa 4, blit 1280x720`; `msaa4_bloom` `camera 1280x720 msaa 4, bloom x7,
  uber` (no extra pass); `msaa4_fixed` `camera 640x360 msaa 4, blit 1280x720` (the fixed
  resolution's blit, as without MSAA); `lib_pixelate` `camera 1280x720, user post-process 640x360,
  blit 1280x720`; `user_pass_bloom` `camera, user post-process 640x360, user post-process
  1280x720, bloom x7, uber` (no scene target or copy for the uber pass). On Vulkan the exported
  game ends with the platform's present blit, as before phase 4.
- Editor (5.3's hooks): `passFields()` has the scale; the pass's texture slots are rows by uniform
  (its manifest's `@texture`s and any set), drop an image, right-click clears; the original scene is
  a shader input, no field. The cost estimate's blur chain counts `1 + 2 x getBlurLevels(radius)`
  passes (bloom: 7, as measured).
- Checks run (branch merged with `shader-library` cc8baac4): full `tools/build.sh linux`;
  `shaders --library all --backend all` (40 shaders, 13842 compiles, 0 failures; the six post
  shaders 1 variant each, frosted_panel 7); 40 built-in specs x 6 backends (240 compiles, 0
  failures: the five bloom programs, blit/uber with Blm, Blr and both alone and with FXAA, the
  mobile set, every feature, CRT, pixelate, edge, sharpen, LUT/vignette/dither combinations,
  postprocess, Pcl mesh/points/lines/sky, ssao/ssr/composite); the GLES3 bloom output has
  `precision mediump float` with `highp` UVs. `post-processing` fixture on GL (every new case,
  screenshots read: bloom glows on the lit wall, sky and points, tint, wide scatter, full and half
  blur, the frosted panel showing the blurred wall behind it, MSAA smoothing the poles' edges
  (8630 pixels changed, 2x = 4x on llvmpipe), CRT, pixelate with rounded blocks at half
  resolution, posterize, edge neon, gameboy, blur, the user pass's split with the original scene
  on the left and the pixelated chain on the right under the checker overlay), Vulkan (same 18
  runs, same images; msaa2 runs without MSAA on lavapipe) and GLES3 (same 18 runs, same images,
  pass actions listed); `PP_CYCLE` subset on GL (above). GL smokes of `shader-library`,
  `scene-inputs`, `stencil`, `extra-pass`, `unified-lighting`, `runtime-shaders` pass with normal
  screenshots. Post-processing off against the `shader-library-phase3` tag built with the frame
  stats and DOCKER_CPUS commits (temporary worktree, removed): frame stats lines (passes, draws,
  pipelines, uploads, images, views, pass list) identical on `shader-library`, `scene-inputs`
  and `stencil`; `stencil` pixel-identical, `scene-inputs` differs only on the animated heat
  haze's edge (1054 pixels, at most 9/255) and `shader-library` only on its time-animated objects
  (the shine sweep, the spinning and bobbing ones), the capture times differing. Re-save: the
  phase 3 editor and this one re-save `shader-library`, `scene-inputs`, `stencil`,
  `shader-library-export`, `extra-pass`, `unified-lighting` byte-identically (scenes and
  project.yaml; only the generated Lua API stubs and AGENTS.md differ). `check.sh` (7 scenes ok).
  MCP suite 281 passed, 0 failed, 3 skipped (new `test_post_process_passes.py`).
- Not done / limits: bloom is LDR (the scene target is clamped: a white wall blooms like a lamp
  above the threshold; research 3.5 option 1); the blurred scene is not available to the scene it
  is made from (see above); MSAA through sokol on GLES3 is a renderbuffer and a blit resolve (not
  `EXT_multisampled_render_to_texture`); glslang warns "all default precisions are highp" for the
  post shaders, as for the uber pass (harmless: the qualifiers come through).

### 5.3 Editor (sl/p4-editor)

- Inspector (§2.1). Scene settings > Post-processing (`editor/window/InspectorPostProcessing.cpp`)
  draws `PostProcessingSettings` in groups: Color (exposure and tone mapping; colour grading with
  the LUT texture, contribution and LUT size; monochrome; dither), Lens (vignette, chromatic
  aberration, film grain), Anti-aliasing (FXAA, MSAA), Stylised (pixelate, CRT, edge detect,
  sharpen), Bloom, Blur, Resolution (render scale). Each group has a checkbox (mixed while only
  some of its flagged effects are on; off turns every effect of the group off and resets its
  neutral-value ones, on turns its flagged effects on); Bloom and Blur are one effect each, so
  their group checkbox is the effect's. An effect's parameters are hidden while it is off. Every
  effect shows its cost tier ("T0 in shader", "T1 uber pass", "T2 bloom chain") with a tooltip,
  asked of the engine: `RenderSystem::getPostProcessingTiers` of a default set with only that
  effect on (MSAA, which runs no pass, has its own text). Rows edit `post_processing.<field>`
  (`ScenePropertyCmd`, a drag merges into one step); group toggles, reset arrows and presets
  replace the whole struct (`post_processing`) in one step. Field-table rows the groups do not
  name are drawn under "Other" by their type, so a field added to `SceneSettings.cpp` appears
  without editor code. A note says only the main camera is post-processed, and the Camera
  inspector has a "Post" row saying whether the selected camera is the post-processed one.
- Presets (`editor/PostProcessingPresets.*`): `none`; `mobile` (Neutral curve, light grade with a
  16^3 LUT, FXAA, faint bloom: threshold 0.9, intensity 0.6, scatter 0.5); `console` (ACES, grade,
  vignette 0.3, dither, FXAA, bloom, chromatic aberration 0.15, grain 0.12); `stylised` (pixelate
  4, contrast 1.45 / saturation 1.4 with a warm lift/gain, vignette 0.3); `crt_retro` (CRT,
  pixelate 3, chromatic aberration 0.25, grain 0.15, warm grade; T1 only). A preset is a whole
  `PostProcessingSettings` (it replaces every field, the LUT image too). The inspector's Preset
  combo shows the matching preset or "Custom". MCP `list_post_processing_presets` (fields and
  estimated cost of each; `current`: the scene's preset or null, settings, cost readout,
  `applied` while playing) and `apply_post_processing_preset` (one undo step; refused on UI
  scenes), in `SceneTools.cpp`.
- Cost readout ("Estimated cost", §2.7). An estimate, labelled as one, from the research cost
  model (§11 of fast-postprocessing.md): this engine = the enabled user passes (at the scene's
  scaled size), a bloom and a blur chain (7 passes, 0.165 x renderScale^2 of a screen each) and
  the uber pass (1 full-resolution pass); Unity URP 14 for the same effects = the 1024x32 LUT
  bake, bloom at half resolution with 6 mips (16 passes, 0.75 x scale^2), one Full Screen Pass
  renderer feature for each effect URP has no built-in for (pixelate, CRT, edge detect, sharpen,
  blur, each user pass), the uber pass, and the final pass for FXAA or a render scale below 1
  (plus FXAA's scaling setup). The research's reference set gives 8 passes / 1 at full
  resolution / 1.17 screens against 19 / 2 / 2.77 (checked by the MCP test). Notes cover the
  in-shader cost, fixed resolution (the uber pass is the blit drawn anyway), render scale and
  user passes. Deviation from "the measured pass list from frame stats": the list is recorded
  only when `JASPERENGINE_FRAME_STATS` is set at startup and lives in the sokol layer with no
  accessor the editor can reach without an engine change, so while playing the readout shows
  what the renderer applies (`getActivePostProcessingTiers`: in-shader colour, uber pass and its
  features) and its tooltip points at the variable for the measured list.
- Scene view (§5.1's `showPostProcessing`): a toggle button left of Gizmos and a "Show
  post-processing" row in the Gizmos menu's Rendering section (not in UI scenes), saved per
  scene in the workspace file like the other display settings; MCP `set_scene_camera`
  `display.show_post_processing`, which now saves the workspace after a display change.
- User passes (§2.5). The chain is the "Custom Passes" subsection. Each pass's shader row has the
  library picker for `ShaderType::POSTPROCESS` manifests (`drawLibraryShaderCombo`: it lists the
  `@shader postprocess` library shaders once the manifest type exists) next to the fork and file
  buttons; MCP `post_process` accepts `"library:<name>"` (checked by `checkShaderReference` for
  POSTPROCESS). On this base `PostProcessPass` has no new fields yet: the rows for them (custom
  textures, original scene input, resolution scale) come from one table, `passFields()` in
  InspectorPostProcessing.cpp (label, help, kind Bool/Float/Texture, accessor), empty here: each
  field sl/p4-effects adds needs one entry there (and its key in SceneTools' `post_process`
  JSON).
- Fix (pre-existing, p4-core): MCP `post_processing.lut_texture` stored the path as given, so in a
  project with an assets directory it stored `assets/x.png` where the engine and the inspector use
  the assets-relative `x.png`. It is now project-relative in MCP (as every MCP asset path) and
  stored assets-relative.
- Docs: `docs/mcp/guide/post-processing.md` (`jasper://docs/post-processing`: tiers and costs,
  presets, the readout's fields, mobile advice, fields, Lua); the agent guide names the preset
  tools (other lines trimmed: 16,382 of 16,384 bytes); lua-api lists `TonemapMode` and
  `MonochromeMode`; TOOLS.md regenerated (135 tools).
- Checks run (4 cores): full `tools/build.sh linux`; the MCP suite pinned to 4 cores (279 passed,
  3 slow ones skipped; the one failure, the agent guide naming `render_scale`, fixed and its file
  re-run green); new `test_post_processing_presets.py` (presets list/apply/undo/redo/save, cost
  readout, LUT path, Scene view toggle persistence); the 21 scene and prefab files of the fixture
  projects re-saved with this editor and with a build of 1623a48d: all byte-identical; inspector
  screenshots over MCP with the Mobile preset applied (a temporary scroll hack, reverted:
  groups, tiers, the Bloom group, the cost readout 8 / 1 / 1.17 against URP 19 / 2 / 2.77);
  `tools/fixtures/post-processing` on GL with `PP_SCENE=saved` after applying each preset to a
  copy of Saved.scene through MCP: all pass. None draws the pre-phase-4 pass list; Mobile and
  Console add `uber 1280x720` (features Fxa,Lut,Blm and Fxa,Cab,Grn,Lut,Vig,Dth,Blm; the Neutral
  and ACES curves visibly compress the lit wall, Console shows grain, fringes and vignette),
  Stylised (Pix,Lut,Vig: large pixels, punchy colour) and CRT Retro (Cab,Pix,Crt,Grn,Lut: curved
  tube, scanlines, fringes) one uber pass each; the HUD layer is untouched in all. The bloom
  chain is not on this base (`uberBloomTexture` null binds black), so the measured pass lists of
  Mobile and Console are 1 pass where the estimate counts the chain's 7.

### 5.5 Review fixes: editor (sl/p4-fix-editor)

The editor/MCP/tooling items of the phase 4 review (`/tmp/shader-research/p4-review/editor.md`),
each reproduced on 3097628f first (MCP test or xdotool probe), then fixed with a regression test.

- LUT path on asset changes (1). `visitSceneSettingsAssetPaths` (Project.cpp) rewrites
  `postProcessing.lutTexture` like the pass textures: a rename or move rewrites it, a deletion
  clears it (set back through `Scene::setPostProcessing`). Before, the session kept the strip the
  renderer had cached and a reopened scene or an export lost it.
- Pass uniform rows (2). A pass whose shader has a manifest draws the members it declares
  (`@property`) as the material inspector does (`InspectorWindow::drawShaderPropertyValue`, shared
  with `drawShaderManifestRows`): label, range slider, colour picker (sRGB), toggle, and the
  manifest default while unset, which is what the engine renders (the rows showed 0 and a nudge
  stored about 0.01); a set member has a reset arrow. Undeclared members (forks) keep the generic
  rows. The label column also fits the texture slots.
- Pass textures over MCP (3). `get_scene_settings post_process` gives `textures` through
  `McpValues::shaderTexturesToJson` (a plain path, or `{path, min_filter, ...}` / `{cube}`), so a
  read-modify-write keeps sampler settings and cube maps.
- MSAA texts and cost (4). MSAA is applied (5.2): the inspector, `SceneSettings.h`,
  `getPostProcessingTiers`' comment and the guide no longer say it is stored only, nor that FXAA
  turns it off (both can be on; the editor advises one). Its tag is "scene pass": the samples
  resolve at the end of the scene pass (in tile memory on tilers, a blit on GLES); in an exported
  game with nothing else offscreen (no uber pass, user pass or fixed resolution) the scene resolves
  into a target blitted to the screen, one more full-resolution pass, which the estimate now
  counts ("MSAA resolve blit to the screen") with a note.
- Blur count (5). The chain tier text and the guide's T2 row give bloom 7 passes and blur
  `1 + 2 x ceil(radius)` (3 at radius 1, 5 to radius 2, 7 above), as `getBlurLevels`.
- User passes in the estimate (6). `estimatePostProcessingCost(settings, passes, fixedResolution)`
  takes the pass list: each enabled pass at the scene's size times its scale squared (full
  resolution only at scale 1 without render scale or fixed resolution), then, without the uber
  pass, the chain's end: a copy into the fixed-resolution target after a last pass below full size,
  or (no fixed resolution) the blit to the screen an exported game always ends the chain with
  (the editor's Game view draws a last full-size pass straight into the view: noted). With the
  uber pass nothing is added (it reads the chain's output). Render scale alone counts the uber
  pass (the engine fix makes it the plain upscale).
- Undo steps (7). The pass uniform rows (and a component's fork uniform rows, the same pattern)
  close their merged command on release (`setNoMerge`), so separate drags, or a drag then the
  Scale slider, are separate steps (one Ctrl+Z reverted both).
- Shader change (8). Picking another shader for a pass clears its uniforms and keeps only the
  textures the new shader's manifest declares (none for a shader without one).
- Images changed on disk (9). The editor's file pick-up (`sync_files` and the automatic one before
  each MCP call: `reloadTextures` in ScriptTools.cpp) now also reloads the user passes' textures
  (pool entry marked stale with `TexturePool::invalidate`: the chain's copies hold the GPU
  texture; the scene keeps the released copies) and the LUT strip, through a new engine entry
  point, `RenderSystem::invalidatePostProcessTextures()` (forgets the strip's cached path and
  reloads the chain; the only engine code of this branch). The GUI has no image watcher for any
  texture (entity textures included): an image edited outside the editor is picked up by the next
  MCP call or `sync_files`, as before.
- MCP validation (10). `set_scene_settings` refuses a Vector3 field outside its range (it was
  clamped silently), takes enums by name in any case or by index (DESIGN.md), refuses unknown keys
  of a `post_process` item and a non-bool `enabled`, checks a library pass's `uniforms` against its
  manifest (`shaderParamsFromJson`: unknown members refused, colours as hex), and refuses
  `post_processing` / `post_processing.<field>` on UI scenes as the preset tool and the inspector
  do. `tools/build.sh` and `tools/smoke-test.sh` expand their flag arrays as `${a[@]+"${a[@]}"}`
  (an empty array under `set -u` fails on bash before 4.4, macOS).
- Stylised lift (10). Warm, as 5.3 says: lift (0.04, 0.02, 0) (amber shadows) with the warm gain;
  the old (0.02, 0, 0.04) lifted blue more than red. Warm was the intent: the preset's gain is warm
  and a cool lift under a warm gain splits the shadows purple, which reads as a mistake.
- Tests: `test_post_processing_review_fixes.py` (MCP: LUT rename/delete, pass texture round trip,
  validation, the warm lift, the cost estimate cases, the LUT strip and a pass texture changed on
  disk shown in the Game view) and `test_post_process_inspector.py` (xdotool in the harness's Xvfb,
  as the phase 2 inspector test: a drag of an unset CRT parameter starts from its default 240,
  two drags and a Scale click are three undo steps, switching a fork with a texture to
  `library:pixelate` drops the texture). On 3097628f both fail at each item.
- Checks run (4 cores, one heavy job at a time): full `tools/build.sh linux`; MCP suite pinned to
  4 cores (290 passed, 0 failed, 3 skipped); every scene, prefab and project.yaml of the 10 fixture
  projects (31 files) re-saved over MCP with this editor and with a build of 3097628f: all
  byte-identical; `tools/fixtures/post-processing` on GL (`PP_SCENE=saved`, frame stats) after
  applying each preset to a copy of Saved.scene through `apply_post_processing_preset`: all pass,
  and each measured pass list matches the estimate's engine count (None: the pre-phase-4 list;
  Mobile and Console: 7 bloom passes + `uber 1280x720` = 8, as estimated; Stylised and CRT Retro:
  `uber 1280x720` = 1); screenshots read: Mobile and Console compress the lit wall (Console with
  vignette, grain and fringes), Stylised large pixels with the punchy, now warm grade, CRT Retro
  the curved tube and scanlines, the HUD layer untouched in all. Inspector screenshot over MCP
  (no temporary hack: the test scrolls with xdotool) of a `library:crt` pass: Curvature 0.300,
  Scanlines over the height 240.000, Scanline darkness 0.500, Aperture grille 0.300, Colour
  fringing 1.000, Dark corners 0.400, its manifest's labels and sliders
  (`/tmp/shader-research/p4-fix-editor-shots/`).
