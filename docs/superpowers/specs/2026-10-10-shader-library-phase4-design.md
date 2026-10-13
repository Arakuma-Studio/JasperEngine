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
