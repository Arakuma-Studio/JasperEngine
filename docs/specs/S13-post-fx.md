# S13 — Post FX: VHS, film simulation, halation, .cube LUT import, dynamic resolution

Wave 2. Extends the phase-4 mobile-first post stack
(`docs/superpowers/specs/2026-10-10-shader-library-phase4-design.md`). It does not replace that stack.

## 1. Goal

Add four things to the post stack: stylised effects (VHS tape, film grain types and film damage,
halation); import of industry `.cube` LUTs into our existing colour-grading LUT; and a dynamic-resolution
controller that drives the existing render scale from frame cost on mobile and web. Each effect
costs nothing when off. Each one folds into the existing uber pass or the CPU-baked LUT, so no new
full-resolution pass is added. Each one gets presets, a Unity-URP-style inspector, Lua and MCP control,
and golden images. The aim is a stock of looks (VHS, 35 mm film) that are cheap enough to ship on phones.

## 2. Scope

**In scope**
- VHS as one uber-pass feature (`UBER_VHS`). Ported from OE `vhs.frag`: wobble, tracking, colour bleed,
  tape noise, chroma streaks, dropouts, scanlines and speed. Also folded in from `vhs_interference.frag`
  and `vhs_signal_glitches.frag`, each as a strength.
- Film simulation. New grain types (FidelityFX-Lens-derived "Lens", and "Filmic") extend our Film Grain.
  The film artifacts (hair, scratches, dust, gate weave, gate mask) become a new "Film Damage" uber feature.
- Halation as a T2 effect. It reuses the bloom chain's levels, or runs a 2-pass mini chain when bloom is off.
- `.cube` import (1D, 3D, 1D shaper + 3D) into `PostColorLut`. The LUT is applied by the CPU bake, so it
  has no GPU cost. Display-referred and scene-referred (log) input encodings are supported. Export cooks
  `.cube` files to a compact binary.
- A dynamic-resolution controller (a pure control law, port of OE `DynamicResolutionController`), plus a
  frame-period fallback law of our own. It drives `PostProcessingTiers::renderScale` at runtime.
- Presets, inspector rows, cost readout, Lua, MCP, docs, unit tests and golden images for all of the above.

**Out of scope**
- An HDR scene pipeline. Our scene target stays 8-bit (`SceneSettings.h:138-140`). Halation works on
  display values.
- VHS frame feedback and field history (`vhs_frame_feedback.frag`, `uFieldHistory` in `vhs.frag:591`).
  Both need a history target, and we have no history buffers.
- VHS composite signal and dot crawl, chroma bands, and the on-screen display / date burn
  (`vhs_date_burn.frag`, the OSD in `vhs.frag:195-245`). Build OSD text with the RmlUi HUD instead.
- Post-process volumes and Volume-style blending. Verified: no `PostProcessVolume`/`VolumeComponent`
  exists in `engine/` or `editor/`. Settings stay per scene (`SceneSettings::postProcessing`). A volume
  framework would be its own spec.
- Their render graph and data-driven pipeline (`ForwardPlus.rendergraph`). This is on the README
  do-not-copy list. Effects are hard-wired features of our uber pass and bloom chain.
- Shipping film-stock LUTs. Licensing of film-emulation LUTs is murky. The presets use our parametric grade.
- CAS sharpening, heat distortion and lens dirt (analysis items 15/16). They may follow later.

## 3. Current state

- **Settings**: `engine/core/SceneSettings.h:68-187` `PostProcessingSettings`. It is a plain struct with a
  field table (`getFields`, `SceneSettings.cpp:28`) that drives YAML (`editor/Stream.cpp:3389`, which
  writes only non-default values), exported code (`editor/Factory.cpp:834`), Catalog paths
  `post_processing.<snake>`, MCP (`editor/mcp/tools/SceneTools.cpp:643-697`) and the inspector's
  "Other" fallback. Field types: Bool, Int, Float, Vector3, Enum, String.
- **Tiers**: `RenderSystem::getPostProcessingTiers` (`RenderSystem.cpp:3601-3643`):
  - T0 is the in-shader colour LUT.
  - T1 is the uber pass. Its features are bits in `UberFeature` (`RenderSystem.h:323-338`; 13 bits,
    "appended only", names in `ShaderPool.cpp:648-662`).
  - T2 is the quarter-resolution dual-filter bloom/blur chain (`renderBloomChain`, `RenderSystem.cpp:4210`;
    SRGB8A8 targets, gamma-2 domain).
  - The uber pass runs when `features != 0 || renderScale < 1`.
- **Uber shader**: `engine/shaderlib/uber.frag` (250 lines). It has a fixed 7-step order (UV warp →
  scene/FXAA tap → chromatic aberration → bloom/blur → grade LUT → edge/vignette → CRT/grain/dither).
  Parameters live in `u_fs_uberParams` (12 vec4). Time comes from `edgeSharpenTime.w`
  (`RenderSystem.cpp:4009`). It uses 4 samplers in the worst case.
- **Film grain today**: `UBER_GRAIN` is one `fract(sin())` hash per pixel (`uber.frag:237-243`). It has
  intensity and response only.
- **Colour LUT**: `engine/core/util/PostColorLut.{h,cpp}`.
  - It bakes exposure, curve and grade on the CPU into a 16³/32³ RGBA8 strip.
  - Stages are ALL/CURVE (shaper domain, in-shader tail) and GRADE (display domain, uber).
  - A user LUT is only a PNG strip (`loadStrip`, `PostColorLut.cpp:251`; RGB8, Unity layout), sampled
    trilinearly on the CPU in `grade()` (`:213`).
  - No `.cube` support exists anywhere (grep: no `.cube` handling outside cubemaps).
- **Render scale**: static `renderScale` 0.5–1 (`SceneSettings.h:158`). The uber source target and the
  chain targets are sized from it (`RenderSystem.cpp:13788`). There is no dynamic resolution, and sokol
  has no GPU timestamps (`01-rendering.md` §2.1).
- **Editor**:
  - `editor/window/InspectorPostProcessing.cpp` holds the groups Color, Lens, Anti-aliasing, Stylised,
    Bloom, Blur and Resolution. Each effect has its own cost tier (`isolate`), and there is an
    "Applied now" readout while playing (`:497-515`).
  - `editor/PostProcessingPresets.{h,cpp}` holds the presets none, mobile, console, stylised and
    crt_retro, and a cost model compared with URP 14 (`estimatePostProcessingCost`).
- **Variants**: `editor/shader/ShaderVariantCollector.cpp:836-860` lists the uber variants an export
  ships. `getScriptableEffectFields` names the switches Lua may flip.
- **Lua**: `engine/core/script/binding/CoreClassesLua.cpp:801+` binds `PostProcessingSettings` by
  property (as a copy that is then assigned back).
- **Tests**: MCP post tests `tools/mcp/tests/test_post_processing_*.py`; fixture
  `tools/fixtures/post-processing`; image diff `tools/mcp/pngdiff.py` (`compare(a, b, 24, 0.01)`).
  There is no gtest yet (S02).

## 4. Reference (OpenEngine, read-only, MIT)

| What | Path (under `external/OpenEngine/`) | Take | Improve / change |
|---|---|---|---|
| VHS core | `Engine/Modules/Rendering/Shaders/vhs.frag` (625) | Hash/noise, YIQ bleed (8 taps, `:378-395`), wow/flutter, tracking bands, dropouts, tape noise, gain drift, scanlines | Uniform block instead of push constants. `uber.sourceTexel` instead of `textureSize`. Drop the OSD and the field history. The folded stages sample the source, not the previous stage's output |
| VHS extras | `vhs_interference.frag` (60), `vhs_signal_glitches.frag` (147) | Their maths as two strengths in the same feature | No separate passes |
| VHS licence | `ThirdParty/PunikontaVhs/LICENSE.md`, `UPSTREAM.md` | MIT © Marcel Jovic, upstream rev `6ba87f9` | Keep the header in our include |
| Film grain | `Packages/film-simulation/Assets/Shaders/film_simulation_grain.frag` (144) | PCG3D16, `SampleFidelityFxGrain`, the 5×5 Filmic lattice, the shadow/mid/high response and the display-boundary envelope | Fold into the uber pass. Response bands map onto our existing `filmGrainResponse` plus three band weights |
| Film damage | `Packages/film-simulation/Assets/Shaders/film_simulation_artifacts.frag` (253) | Hair/scratch/dust (8 candidates each), gate weave, gate mask (`FilmGateMask`) | Fewer parameters (see R5). Gate weave joins `UBER_WARP` |
| Film package licence | `Packages/film-simulation/LICENSE.txt` | MIT © 2026 "GameEngine contributors", **plus** MIT © 2024 AMD (FidelityFX Lens 1.1, SDK v1.1.3) | Both notices go in `NOTICE.md`. The package contains only these two shaders, `package.json`, a README and an asset manifest. It has **no film-stock colour data**: "film simulation" there means grain, artifacts and halation |
| Halation | `Shaders/halation_prefilter.frag`, `halation_blur_h.frag`, `halation_composite.frag`, `Includes/halation_blur.glsl` | Fixed display-white threshold and red-orange tint idea (default tint 1, 0.12, 0.025) | Theirs is HDR, full-res separable Gaussian (≤ 65 taps per axis). Ours reuses the 1/4 and 1/8 dual-filter levels |
| .cube parser | `Engine/Modules/Rendering/Source/Common/CubeLutFileParser.cpp` (387) + `Include/Rendering/Utils/CubeLutFileParser.h` | Whole parser (std only), error strings | Lower the 3D cap from 256 to 129 (256³ floats would be 200 MB). Use the `jasperengine` namespace and `float` |
| Colour domains | `Source/Common/CubeLutColorDomain.cpp` (167) + `.h`; curve maths in `Shaders/Includes/cube_lut.glsl:41-170` | `ProposeCubeLutColor` (title/range heuristics), the LogC3/ACEScct/Cineon/DaVinci Intermediate curves and gamut matrices | Port the GLSL maths to C++ for the CPU bake |
| .cube tests | `Engine/Modules/Rendering/Tests/CubeLutFileParserTests.cpp` (311), `Tests/Fixtures/CubeLut/*.cube` (7) | Port the tests and copy the fixtures | gtest (S02) |
| DRS controller | `Engine/Source/Engine/Rendering/DynamicResolutionController.cpp` (437) + `Engine/Include/Engine/Rendering/DynamicResolutionController.h` (233) | Whole control law: deadband, asymmetric gains, slew, dwell, quantise, EMA, emergency, response fit `cost ≈ U + S·scale²`, ineffective latch | Add a frame-period law for backends without GPU timing. Use our defaults (step 0.05, dwell 0.5 s) |
| DRS tests | `Engine/Tests/DynamicResolutionControllerTests.cpp` (782) | Port the tests | Add period-law tests |

OE's root `LICENSE` is MIT © 2026 Gamecrafters Guild Limited and contributors. The copied files have no
per-file header, so we add the OE copyright line to each copied file.

## 5. Requirements

### Functional

- **R1 Zero-cost default.** A default-constructed `PostProcessingSettings` produces exactly today's
  tiers, uber features, pass list and pixels. Each new effect contributes no tier bit and no pass while
  it is disabled or its strength is at the neutral value.
- **R2 VHS** (`UBER_VHS`, T1). Fields:
  - `vhsEnabled` (false)
  - `vhsIntensity` (1, range 0..1)
  - `vhsWobble` (0.5), `vhsTracking` (0.5), `vhsColorBleed` (1), `vhsTapeNoise` (0.6),
    `vhsChromaStreaks` (0.4), `vhsDropouts` (0.3), `vhsScanlines` (0.5), `vhsInterference` (0),
    `vhsGlitches` (0); each 0..2
  - `vhsSpeed` (1, range 0..4)

  Rules:
  - VHS replaces the base scene tap (step 2) and runs its tape noise and scanlines in step 7, before grain.
  - It implies `UBER_WARP`.
  - While VHS is on, `getPostProcessingTiers` drops `UBER_FXAA` and `UBER_SHARPEN`, and the inspector
    shows them as "inactive while VHS is on". The VHS bleed already filters the image horizontally.
  - CRT, edge detect, grade, vignette, grain and dither still combine with VHS.
- **R3 Halation** (T2). Fields:
  - `halationEnabled` (false)
  - `halationIntensity` (0.6, range 0..2)
  - `halationThreshold` (0.85, display brightness 0..1)
  - `halationRadius` (0.5, range 0..1: the mix of the 1/4 and 1/8 levels)
  - `halationTint` (sRGB 1.0, 0.25, 0.08)

  Rules:
  - When bloom is on, halation samples the bloom chain's `down[0]` and `down[1]`. It adds no pass and uses
    bloom's threshold; the inspector disables Threshold and explains why.
  - When bloom is off, a halation chain runs: one prefilter-with-threshold to 1/4 and one dual-filter down
    to 1/8. That is 2 passes.
  - The uber pass composites the halo in the gamma-2 domain like bloom: `sqrt(c² + halo·tint·intensity)`.
  - New bit `UBER_HALATION`.
- **R4 Film grain types.** New fields:
  - `filmGrainType` (enum `FilmGrainType`: `CLASSIC` default = today's `UBER_GRAIN` pixels unchanged;
    `LENS` = FidelityFX-Lens-derived; `FILMIC` = 5×5 particle lattice)
  - `filmGrainSize` (1.5 px, range 0.25..8)
  - `filmGrainColored` (false, FILMIC only)
  - `filmFrameRate` (24, range 1..120; LENS and FILMIC hold the pattern per film frame and share this
    rate with Film Damage)

  Each non-classic type adds one bit (`UBER_GRAIN_LENS`, `UBER_GRAIN_FILMIC`), always together with
  `UBER_GRAIN`. Grain respects the display-boundary envelope, so it never clips black or white.
- **R5 Film damage** (`UBER_FILM_DAMAGE`, T1, implies `UBER_WARP`). Fields:
  - `filmDamageEnabled` (false)
  - `filmDamageIntensity` (1, range 0..4)
  - `filmHairAmount` (0.1), `filmScratchAmount` (0.25), `filmDustAmount` (2.5); each 0..8 candidates,
    and 0 skips that loop
  - `filmDustSize` (3 px, range 0.5..24)
  - `filmGateWeave` (0.3, range 0..1; scales OE defaults of 0.75/0.5 px and 0.08°)
  - `filmGateMask` (enum `FilmGateMask`: `NONE`, `ROUNDED`, `ACADEMY_137`, `WIDESCREEN_185`,
    `ANAMORPHIC_239`)
  - `filmGateMaskFeather` (3 px, range 0..256)
- **R6 .cube LUTs.**
  - `lutTexture` accepts a project `.png` strip (as today) or a `.cube` file.
  - New field `lutInputEncoding` (enum `LutInputEncoding`: `AUTO` default, `SRGB`, `LINEAR`, `LOGC3`,
    `ACESCCT`, `CINEON`, `DAVINCI_INTERMEDIATE`). `AUTO` resolves through `ProposeCubeLutColor`
    (title words, input range).
  - **Display stage** (`SRGB`, or `LINEAR` with input max ≤ 1): the LUT sits where the strip sits in
    `grade()`. The engine's pow-2.2 display colour is converted to piecewise sRGB before the lookup and
    back after it.
  - **Scene stage** (log encodings, or `LINEAR` with input max > 1): the LUT is applied to the exposed
    linear scene colour **before** the tone curve, in the ALL and CURVE stages. The colour goes through
    gamut conversion, then the encoding, the lookup, the decoding and the inverse gamut. A scene-stage
    LUT therefore makes `PostColorLut::hasCurve` true.
  - In both stages, `lutContribution` mixes the result in.
- **R7 .cube parsing.**
  - The parser supports `TITLE`, `LUT_1D_SIZE` (2..65536), `LUT_3D_SIZE` (2..129), `LUT_1D/3D_INPUT_RANGE`
    and scalar `DOMAIN_MIN/MAX`.
  - It returns one-line errors with the line number. Errors are logged, and the grade then runs without
    the LUT, as for a bad strip (`RenderSystem.cpp:3685-3690`).
  - The 3D table is sampled trilinearly in float on the CPU.
- **R8 Dynamic resolution.** Fields:
  - `dynamicResolutionEnabled` (false)
  - `dynamicResolutionMinScale` (0.6, range 0.5..1)
  - `dynamicResolutionTargetFps` (0, range 0..240; 0 = the display refresh rate when the platform
    reports it, else 60)

  The upper bound is `renderScale`. Behaviour:
  - The controller runs only for the post-processed main camera, in Play mode and in exported games.
    It never runs in edit mode or the Scene view.
  - It is disabled while `fixedResolutionEnabled` is on.
  - Its output replaces `PostProcessingTiers::renderScale` at runtime and is never written to settings.
  - While it is enabled, the uber pass is forced on (at least `UBER_UPSCALE`). Crossing scale 1.0 therefore
    never toggles tiers, the in-shader colour variant or object reloads.
- **R9 Controller inputs.**
  - **GPU law (OE)**: when the backend reports GPU frame time (S15 timer queries), the controller uses
    OE's law.
  - **Period law (ours)**: otherwise it uses the frame-period law.
    - Overload means at least 3 of the last 8 frames had a period above 1.2 × target, and CPU busy time
      (frame start to submit end, excluding present wait) below 0.8 × target. That second condition means
      the frame is not CPU-bound.
    - On overload, drop one or more rungs, as decided by the emergency rule.
    - After `probeSeconds` (2 s) without a miss, rise one rung. Each failed rise doubles the probe interval
      (capped at 16 s). The interval resets when the content cost changes by more than 25 %.
  - The sample records which law produced it (`source`: `gpu` or `frame`).
- **R10 Presets.** Add `film` ("Film", 35 mm), `vhs` ("VHS Tape") and `mobile_adaptive` ("Mobile
  Adaptive" = `mobile` plus dynamic resolution at min 0.6).
  - Film: Neutral curve, warm grade, Lens grain, halation, light damage, ACADEMY gate off, vignette.
  - VHS Tape: VHS defaults, classic grain 0.1, a slightly desaturating grade, no CRT.

  Existing presets stay byte-identical, so `matchPostProcessingPreset` still recognises saved scenes.

### Platform & backends

- **R11** Every new variant compiles and runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan.
  - No compute, no SSBOs and no new render-target formats. Halation reuses SRGB8A8 chain targets.
  - Hashes and UV maths are `highp`. PCG uses `highp uint`, which GLSL ES 3.00 guarantees as 32-bit in
    the fragment stage. Colour maths is `mediump` where precision allows (the phase-4 rule).
  - `fract(sin())` hashes take `highp` inputs only.
- **R12** The uber pass's worst case (every bit on) stays within 8 samplers: scene, LUT, bloom, blur and
  halation ×2 make 6. Its parameter block stays within 24 vec4.

### Performance (budgets)

| Effect | Extra passes | Extra uber texture samples | Loops (max iterations) | Notes |
|---|---|---|---|---|
| VHS | 0 | ≤ 9 scene taps in total (it replaces the base tap) | 0 dynamic | FXAA/sharpen dropped |
| Film damage | 0 | 0 | 3 × 8, each skipped by a uniform branch at amount 0 | |
| Grain Lens | 0 | 0 | 0 | ≈ classic cost |
| Grain Filmic | 0 | 0 | 25 | tooltip: "desktop / high-end" |
| Halation | 0 with bloom; 2 (≤ 0.08 screen) without | 2 | 0 | |
| .cube | 0 GPU | 0 | — | CPU bake of 32³ ≤ 10 ms (desktop, release) |
| Dynamic resolution | 0 | 0 | — | controller ≤ 0.01 ms CPU per frame. No target reallocation while the scale holds. At most 1 reallocation set per accepted change. Accepted changes ≤ 2 per second |

- **R13** Parsing a 65³ `.cube` text file takes ≤ 150 ms in the editor. Loading a cooked `.jlut` takes
  ≤ 5 ms. The parse is redone only when the path or the file changes.
- **R14** Device budget: on a Mali-G57 / Adreno 610-class phone at 1080p, the `film` preset costs
  ≤ 1.0 ms of GPU time more than `mobile`, and `vhs` costs ≤ 1.0 ms more than `mobile`. The figures come
  from S15 GPU timing or a vendor profiler.

### Editor UX (human)

- **R15** Inspector (`InspectorPostProcessing.cpp` groups, URP naming):
  - Color › Color Grading: the LUT Texture picker and drop target accept `.cube`. A "LUT Input
    Encoding" row appears for a `.cube` and shows "Auto: ACEScct (the title names ACEScct)".
  - Lens: add "Halation". Film Grain gains "Type" (Classic, Lens, Filmic), "Size", "Colored" and
    "Frame Rate".
  - Stylised: add "VHS" and "Film Damage".
  - Resolution: add "Dynamic Resolution" (Enable, Min Scale, Target FPS). It is disabled with a reason
    while Fixed Resolution is on.
  - Every row has the existing per-field reset, undo and tier tooltip through `isolate`.
- **R16** The "Applied now" readout shows, while playing, the dynamic scale and its input. Example:
  `Dynamic resolution 0.75 (GPU 14.2 / 16.7 ms)` or `(frame law: probing in 4 s)`. Saturation and an
  "ineffective" latch show as warnings.
- **R17** Cost readout (`estimatePostProcessingCost`):
  - The halation chain passes are counted.
  - On the URP side, VHS, film damage and halation are listed in `urpCustom` (one renderer feature
    each), and `.cube` maps to URP Color Lookup (no extra pass).
  - Dynamic resolution adds a note: "scene shading varies between min² and max² of native".
- **R18** The Project panel recognises `.cube` as a "Color LUT" asset (icon, inspector with title,
  sizes, domain, proposed encoding, parse error). Double-click assigns it to the open scene's
  Color Grading as one undo step.

### Agent UX (MCP, Lua, CLI)

- **R19** Every new field reaches MCP through the field table (`set_scene_settings` /
  `get_scene_settings` `post_processing.<snake>`). New presets come through `list_post_processing_presets`
  and `apply_post_processing_preset`.
- **R20** New MCP tools (in `editor/mcp/tools/SceneTools.cpp`):
  - `inspect_color_lut {path}` → `{ok, error, title, size_1d, size_3d, input_range, proposed_encoding,
    reason, stage}`
  - `get_post_processing_status {scene?}` → `{tail_color, uber, uber_features[], render_scale_applied,
    passes[] (estimate), dynamic_resolution: {enabled, scale, source, smoothed_ms, target_ms,
    saturated_low, ineffective, accepted_changes}}`
  - `set_post_processing_debug {scene?, time_seconds?: number|null, force_render_scale?: number|null,
    fake_gpu_ms?: number|null}`. This is editor-only and not serialized. It pins the uber time, forces
    the applied scale, or injects controller samples, so tests can be deterministic.
- **R21** Lua:
  - `PostProcessingSettings` binds every new field. Enums are registered: `FilmGrainType`,
    `FilmGateMask`, `LutInputEncoding`.
  - `scene:getDynamicResolution()` returns a table of the `get_post_processing_status` fields.
  - `scene:setDynamicResolutionOverride(scale)` pins the scale; passing `nil` releases it.
  - Stubs are regenerated with `generate_lua_stubs.py`.
  - `getScriptableEffectFields` (`ShaderVariantCollector.cpp:836`) gains `vhsEnabled`, `halationEnabled`,
    `filmDamageEnabled`, `filmGrainType` and `dynamicResolutionEnabled`, so scripted toggles ship their variants.
- **R22** Docs:
  - `docs/mcp/guide/post-processing.md`: tiers table, fields, presets, mobile advice, LUT encodings,
    dynamic resolution.
  - The post-processing line in `McpInstructions.cpp:227`.
  - Golden thumbnails for the new effects in `docs/mcp/guide/img/effects/`.

### Data & compatibility

- **R23** No scene-format version bump. All new fields are default-off and the encoder writes only
  non-defaults (`Stream.cpp:3389-3400`), so old scenes load and re-save byte-identically. Enum YAML
  values are lower-case names (the existing convention).
- **R24** UberFeature bits are **appended** after `UBER_UPSCALE` (bit 12), in this order: `UBER_VHS` 13
  'Vhs', `UBER_HALATION` 14 'Hal', `UBER_FILM_DAMAGE` 15 'Fdm', `UBER_GRAIN_LENS` 16 'Gln',
  `UBER_GRAIN_FILMIC` 17 'Gfm'. `UBER_FEATURE_COUNT` becomes 18. Names are added to `ShaderPool.cpp:648`.
- **R25** Export cooks every `.cube` that a shipped scene references (or that Lua names in a string
  literal) to `<name>.jlut`. The format is magic, version, 1D size, 3D size, ranges, and float16 RGB. The
  3D table is resampled to at most 33³, and the 1D table is kept. The runtime prefers `.jlut` when it is
  present. A 65³ cube cooks to ≤ 300 KB.

## 6. Design constraints & decisions

- **D1 No new pass types.** VHS, film damage and grain types are `#ifdef` blocks in `uber.frag`. Their
  bodies go in new includes, `engine/shaderlib/includes/post_vhs.glsl` (punikonta header) and
  `includes/post_film.glsl` (AMD and OE headers). That keeps third-party code isolated with its notices.
  Halation adds two bloom-chain programs at most (a reused `BLOOM_PREFILTER_THRESHOLD` with its own
  curve, and `BLOOM_DOWN`) and no new shader file.
- **D2 Uber order** (it extends the comment in `uber.frag:9-24`):
  1. warp: CRT, then pixelate, then VHS line wobble/tracking, then gate weave
  2. base tap: VHS bleed taps replace it, or FXAA, then sharpen
  3. chromatic aberration
  4. bloom, **halation**, blur
  5. grade LUT
  6. edge, vignette, **gate mask**
  7. **VHS noise/scanlines**, CRT, **film damage**, grain (classic, Lens or Filmic), dither
- **D3 Halation reuses the chain.** `PostChainTargets` (`rv().bloomTargets`) gains read access to `down[0]` and
  `down[1]`. Halation without bloom gets its own `rv().halationTargets` with 2 levels, destroyed with
  the effect, as bloom's are (`RenderSystem.cpp:13797`).
- **D4 .cube lives in the CPU bake.** New `engine/core/util/CubeLut.{h,cpp}` (parser port plus
  colour-domain maths plus a sampler). Sketch:
  ```cpp
  struct CubeLut { int size1D=0, size3D=0; float in1D[2]{0,1}, in3D[2]{0,1}; std::string title;
                   std::vector<float> lut1D, lut3D; Vector3 sample(const Vector3& encoded) const; };
  enum class LutStage { DISPLAY, SCENE };
  bool parseCubeLut(std::string_view text, CubeLut& out, std::string& error);   // R7
  bool loadColorLut(const std::string& path, CubeLut& out);   // .cube or .jlut; logs errors
  LutInputEncoding proposeEncoding(const CubeLut&, std::string& reason);
  LutStage stageOf(LutInputEncoding, const CubeLut&);
  ```
  `PostColorLut::bake/grade` take a `const CubeLut*` beside `Strip*`. A PNG strip is converted to a
  `CubeLut` on load, so there is one sampling path. `RenderSystem` caches it by path the way it caches
  `postLutStrip` today, and S05 invalidates the cache when the file changes on disk.
- **D5 Controller** in `engine/core/render/DynamicResolutionController.{h,cpp}`. This is OE's header,
  renamed to our style, plus:
  ```cpp
  struct DynamicResolutionSample { float gpuMs=0, unscalableGpuMs=0, framePeriodMs=0, cpuBusyMs=0,
                                   appliedScale=0; bool scaleIsConnected=true, valid=false; };
  // config adds: float targetMs; float probeSeconds=2, maxProbeSeconds=16; QuantizeStep=0.05, DwellSeconds=0.5
  ```
  - It is pure: no globals and no GPU state.
  - `RenderSystem` of the main scene owns it and ticks it once per app frame. The tick is deduplicated
    like OE's `Stats::Ticks`, so several views do not tick it at N× speed.
  - It resets on mode change, view resize, scene load and fixed-resolution toggles.
- **D6 Reallocation policy.** Every accepted change resizes the uber source, SSAO/SSR and chain targets.
  The quantise step and dwell keep this rare. A viewport-subrect approach (allocate at max, render a
  sub-rect) is rejected for v1, because SSAO, SSR, scene-colour copies and user passes all assume
  full-target UVs.
- **D7 Time.** The uber time stays `frameTime`. VHS and film hold frames at their own rates (29.97/59.94,
  `filmFrameRate`). `set_post_processing_debug.time_seconds` pins the time for goldens.
- **D8 Presets** are data in `PostProcessingPresets.cpp`, so each is one undo step through the existing
  `post_processing` scene property command.

## 7. Acceptance criteria

| # | Criterion | Verification |
|---|---|---|
| AC1 | The parser accepts the 7 OE fixtures and reports title, sizes and ranges | gtest `CubeLut.ParsesFixtures` (OE tests ported; fixtures copied to `engine/tests/fixtures/cubelut/`) |
| AC2 | Malformed input is rejected with the exact error: missing size, 3D size 130, per-channel DOMAIN, incomplete data, trailing data | gtest `CubeLut.RejectsMalformed` |
| AC3 | An identity 33³ `.cube` at contribution 1 bakes the same LUT as no LUT (±1/255, every stage, 16 and 32) | gtest `PostColorLut.IdentityCubeIsNeutral` |
| AC4 | A `.cube` and the equivalent Unity PNG strip bake the same display-stage LUT (±1/255) | gtest `PostColorLut.CubeMatchesStrip` |
| AC5 | An ACEScct identity-in-log LUT is neutral at the scene stage, makes `hasCurve` true and lands in the CURVE LUT when the uber pass is on | gtest `PostColorLut.SceneStageLogLut` |
| AC6 | `ProposeCubeLutColor` matches OE's table (LogC, "Warrior", "Nonlinear", "X to Y") | gtest `CubeLut.ProposesEncoding` |
| AC7 | Ported OE controller tests pass | gtest `DynamicResolutionController.*` |
| AC8 | Period law. (a) With ±8 % noise at target, ≤ 2 accepted changes in 600 simulated 60 Hz frames. (b) A 2× load step drops within 0.5 s. (c) CPU-bound overload (cpuBusy ≥ 0.8·target) never drops. (d) Each failed rise doubles the probe interval, up to 16 s | gtest `DynamicResolutionController.PeriodLaw*` |
| AC9 | Default settings give tiers equal to a hard-coded pre-S13 snapshot. Each new effect at its neutral value sets no bit. Each effect enabled alone sets exactly its bits (VHS drops FXAA/sharpen; LENS sets GRAIN plus GRAIN_LENS) | gtest `PostProcessingTiers.S13Bits` |
| AC10 | `dynamicResolutionEnabled` with `renderScale` 1 still gives `uber=true, UBER_UPSCALE` | gtest `PostProcessingTiers.DynamicResolutionKeepsUber` |
| AC11 | Every new bit alone, plus all bits on, compiles for all 6 backend targets | the existing shader compile matrix in CI (S02) |
| AC12 | Static budgets: the SPIR-V of each variant holds ≤ the R12/Performance-table count of `OpImageSample*` beyond the base variant, and ≤ 8 samplers | gtest `UberBudget.SampleCounts` (compiles via `engine/libs/shadercompiler`) |
| AC13 | Golden images on GL core, 640×360, pinned time, through `pngdiff.compare(tol 24, max 1 %)`. One each for vhs, film (Lens grain + damage + gate mask), halation with and without bloom, Filmic grain, a log `.cube`, a display `.cube`, and presets film/vhs/mobile_adaptive. Vulkan matches the same references | `tools/mcp/tests/test_post_fx_golden.py`; references in `tools/mcp/tests/golden/post_fx/` |
| AC14 | All-default fixture: pixels identical (tolerance 0) to the pre-S13 reference, and the `JASPERENGINE_FRAME_STATS=all` pass list identical | `test_post_fx_golden.py::test_defaults_unchanged` |
| AC15 | Pass counts in the frame stats: halation with bloom adds 0 passes, halation alone adds 2, VHS/damage/grain types add 0 | `test_post_fx_golden.py::test_pass_counts` |
| AC16 | Every new field round-trips through set/get. Presets apply as one undo. `inspect_color_lut` reports fixtures and errors | `tools/mcp/tests/test_post_fx_settings.py` |
| AC17 | Dynamic resolution wiring in Play. `fake_gpu_ms 40` at a 60 fps target reaches min scale within 2 s. `fake_gpu_ms 5` returns to max within 10 s. `force_render_scale 0.5` halves the uber source size (status `render_scale_applied`). Edit mode never changes scale | `tools/mcp/tests/test_dynamic_resolution.py` |
| AC18 | Old scenes re-save byte-identically: every scene in `tools/fixtures/*` is opened, saved and diffed | `test_post_fx_settings.py::test_resave_identical` |
| AC19 | Inspector: new rows and groups present. A `.cube` drop sets `lut_texture` and shows the encoding row. VHS shows FXAA inactive. Each is one undo step | extend `tools/mcp/tests/test_post_process_inspector.py` |
| AC20 | Lua: a fixture script toggles `vhsEnabled`, sets `filmGrainType`, reads `scene:getDynamicResolution().scale`. The exported game logs the values and screenshots differ from the off frame | `tools/fixtures/post-processing/scripts/` + `tools/smoke-test.sh` scheduled shots |
| AC21 | Export cooks a 65³ `.cube` to a `.jlut` ≤ 300 KB, and the exported game's bake matches the editor's (±2/255) | smoke-test fixture + gtest `CubeLut.JlutRoundTrip` |
| AC22 | Performance: `.cube` 65³ parse ≤ 150 ms; 32³ bake ≤ 10 ms; controller tick ≤ 0.01 ms | gtest benchmarks (`--gtest_filter=*Bench*`, release, on the build server; logged and gated) |
| AC23 | R14 device budget | Manual: Android Mali-G57 or Adreno 610 class device, 1080p, the fixture scene. Compare GPU frame time `mobile` vs `film` and `mobile` vs `vhs` (S15 timings or a vendor profiler). Record the results in `docs/mcp/guide/post-processing.md` |
| AC24 | `NOTICE.md` lists OpenEngine (MIT), PunikontaVhs (MIT) and film-simulation (MIT with the GameEngine contributors and AMD notices, verbatim). The copied files keep their headers | review checklist |

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20)
  - S02 (gtest, CI compile matrix; AC1-12 and AC22 need it)
  - S15 (GPU frame time for the GPU law; soft, because the period law covers its absence)
  - S05 (`.cube` hot reload; soft)
  - S14 (cooked cache location for `.jlut`; soft: without it, write next to the exported asset)
  - S04 (the LUT path becomes a GUID reference with its migration)
- **Blocks:** nothing. S27's movie recorder should use `set_post_processing_debug.time_seconds` or a
  fixed delta to get deterministic grain and VHS.
- **Merge hotspots:**
  - `engine/core/subsystem/RenderSystem.{h,cpp}`: also S10, S11, S12, S15, S16.
  - `engine/shaderlib/uber.frag`: S12 if fog moves there.
  - `engine/core/SceneSettings.{h,cpp}`: S08 may turn the field table into reflection. Land S13 fields
    through the same table so S08 migrates them mechanically.
  - `editor/window/InspectorPostProcessing.cpp`, `editor/PostProcessingPresets.cpp`,
    `engine/core/pool/ShaderPool.cpp`.
  - `editor/shader/ShaderVariantCollector.cpp`: also S14 and S30.
  - `CoreClassesLua.cpp`: also S23.
  - `editor/mcp/tools/SceneTools.cpp`.
  - `NOTICE.md`: every porting spec.

## 9. Risks & defaults

1. **Uber register pressure on mobile** as bits accumulate. Only the combinations scenes and scripts
   actually use are compiled and shipped (collector). AC12 enforces sample budgets. Default: Filmic
   grain is labelled desktop/high-end.
2. **Dynamic-resolution hitches** from target reallocation, worst on WebGL. Default: off. The 0.05
   step, 0.5 s dwell and slow rise bound the churn. Subrect rendering is the documented follow-up if
   device tests show hitches.
3. **The period law cannot see headroom under vsync.** It probes upward with exponential backoff. This
   is documented as the reason the GPU law is preferred wherever S15 provides timing.
4. **LDR halation differs from HDR film halation.** Highlights are thresholded on display values, so a
   white wall halates. Accepted, and documented beside bloom's identical caveat.
5. **VHS fidelity versus OE.** The folded stages read the source, not the previous stage, and the OSD,
   field history and composite signal are dropped. The goldens are ours, and we do not chase pixel
   parity with OE.
6. **Licence attribution.** The film package's copyright holder ("GameEngine contributors") differs
   from OE's root notice. Default: reproduce both texts plus AMD's. Copy nothing else from that package.
7. **Display-stage `.cube` gamma.** Our display encoding is pow 1/2.2, while Resolve LUTs assume
   piecewise sRGB / Rec.709. Default: convert at the lookup (R6). A per-asset override is not offered.
8. **8-bit bake precision.** A 65³ LUT is reduced to our 32³ RGBA8 bake. Accepted, because the uber
   dither removes visible banding. `lutSize` 32 is recommended when a `.cube` is set (inspector hint).

## Deviations (implementation)

1. **No asset inspector for project files exists** in the editor (the Inspector shows GameObjects
   and scene settings only), so R18's ".cube inspector" is the Project window's tooltip of a
   `.cube` (title, table sizes, input ranges, the proposed encoding and its stage, or the parse
   error), plus `inspect_color_lut` for agents. The Project window lists `.cube` files with a
   Color LUT icon; a double-click, or its context menu's "Use as Color Grading LUT", assigns the
   file to the selected scene's Color Grading (and turns Color Grading on) as one undo step named
   "Assign Color LUT <file> (<scene>)". The UI test drives the menu: the harness's software-rendered
   editor draws a frame in about 250 ms, too slow for ImGui's 0.3 s double-click.
2. **`.cube` is an S04 asset type of its own** (`AssetType::ColorLut` = 14, "color_lut", appended;
   sidecar meta, assets-relative). `postProcessing.lutTexture` expects a texture or, for a `.cube`
   path, a colour LUT.
3. **Export cooks every `.cube` of the assets tree**, not only the referenced ones: the exporter
   ships every asset anyway (scripts and C++ may name a LUT by a computed path), so the referenced
   set adds nothing but risk. A cooked `.cube` ships as `<name>.jlut` only; one that does not parse
   ships as it is (the game logs the same error the editor does). Written with `io::writeFileAtomic`.
4. **Target FPS 0 is 60 everywhere**: sokol_app reports no display refresh rate on any platform.
5. **GPU law input**: S15 has not landed, so no backend reports GPU time yet; the controller takes
   `DynamicResolutionSample::gpuMs` when given (today only `set_post_processing_debug.fake_gpu_ms`)
   and the frame law otherwise. Wiring S15's timer queries is filling that one field.
6. **Edit mode is the engine's paused game events** (`Engine::areGameEventsPaused()`: the editor's
   edit mode and Pause; an exported game never pauses them), not a flag the editor's views set, so
   a Play copy of a scene runs the controller whichever view shows it. `force_render_scale` /
   `setDynamicResolutionOverride` apply only while the game runs, and Stop or Pause releases the
   pinned scale and the fake GPU time (a script's pin must not leak into the editor or the next Play).
7. **Named undo steps (S24)**: presets, group toggles and the LUT assignment are one
   `CommandGroup` labelled "Apply Film Preset (Main)", "Turn On Stylised (Main)", "Assign Color LUT
   ..."; single fields keep `ScenePropertyCmd`'s "Set Vhs Enabled (Main)".
8. **AC11** is covered by the gtest `UberBudget.CompilesForEveryBackend` (every new bit alone and
   all bits on, for the six backend targets, with the resource budget check), which S02's CI runs.
9. **AC23 (device budget)** was not measured: no Mali-G57 / Adreno 610 class phone is available to
   this stream and the emulator's GPU is the host's. The shader costs are bounded statically
   (AC12) instead; the measurement remains for a device run with S15 timings.
10. **Exports ship `licenses/post-fx-LICENSE.txt`** (from `engine/shaderlib/includes/LICENSE.post-fx.txt`:
    punikonta's, the film-simulation package's with AMD's, and OpenEngine's notices) when a shipped
    uber variant has VHS, film damage or the Lens / Filmic grain, as the shaders.com notice ships with
    UI effects.
11. **`.jlut` stores 16-bit steps across each table's own value range** (two float32 range ends per
    table in the header) instead of float16: a log-encoded LUT keeps its highlights near 0.5..1,
    where float16 has 10 bits, and its gamut decode cancels large channels against each other, so
    float16 missed AC21's ±2/255 on the ACEScct look. Same size (2 bytes per value; 65³ cooks to
    about 210 KB).
12. **A scene-stage LUT scales colours brighter than its input range into it and back** (ACEScct
    ends near linear 222, LogC3 near 55) instead of clamping each channel, which turned a very bright
    saturated colour white; an identity log LUT is then neutral at every texel below the bake's top
    (AC5: the top texels stand for unbounded light, where float precision cannot keep a channel a
    million times smaller than another).
13. **Doc thumbnails** of the new effects are in `docs/mcp/guide/img/post-fx/` (from the goldens):
    `img/effects/` is generated from the HTML UI effects' manifests.
14. **AC18**: the fixtures are partly hand-written, so a first save writes the editor's full form
    (before and after S13); the test checks that the saved `postProcessing` equals the file's and
    that a second open and save gives the same bytes.

## Record (implementation)

Run on build server B unless noted; the benchmarks on the benchmark server E.

- AC1-AC10: `jasperengine-core-tests` `CubeLut.*`, `PostColorLut.*`, `DynamicResolutionController.*`
  (OE's tests ported plus the frame law's), `PostProcessingTiers.*`: pass (754 core tests, the
  one failure on the loaded server B being `CubeLutBench.Bake32`'s 10 ms budget, see AC22).
- AC11: `jasperengine-editor-tests` `UberVariants.CompileForEveryBackend` (GLSL 410, GLSL 300 es,
  HLSL 5, MSL macOS / iOS, SPIR-V): pass (319 editor tests).
- AC12: `UberBudget.SampleCounts`: VHS 9 texture reads (base 1), halation 3 (2 more), film damage
  and the grain types 1 (none more); every bit on: 6 textures, 6 samplers, 320 parameter bytes.
- AC13-AC15: `test_post_fx_golden.py` (11 goldens incl. the pre-S13 `defaults.png` matched at
  tolerance 0; pass counts: halation with bloom +0, alone +2, VHS / damage / grain types +0).
  Vulkan and GLES3: the exported fixture's halation, display and log `.cube` frames match GL's
  (`tools/smoke-test.sh`, SMOKE_BACKEND=vulkan / gles3, pngdiff tolerance 24, 1 %).
- AC16, AC18: `test_post_fx_settings.py`; AC17: `test_dynamic_resolution.py` (40 ms: min scale
  after 0.70 s; 5 ms: max after about 5 s; a pinned 0.5 halves the uber source; edit mode idle).
- AC19: `test_post_fx_inspector_ui.py` (Project window menu, one named step, screenshots).
- AC20, AC21: `tools/smoke-test.sh tools/fixtures/post-processing` with `SMOKE_GAME_RUNS` of the S13
  cases: the script case logs its values, the export ships `luts/*.jlut` (13 KB for 13^3) and
  `licenses/post-fx-LICENSE.txt` and no `.cube`; `CubeLut.JlutRoundTrip`: 65^3 cooks to about 210 KB
  and bakes within 2/255 of the `.cube`.
- AC22 (server E): parse 65^3 11.9 ms, 32^3 bake 8.4 ms (display) / 7.6 ms (scene), `.jlut` load
  0.08 ms, controller tick 0.00001 ms.
- Android: `tools/test-projects/post-fx` on the emulator (server C): the uber pass with VHS, Lens
  grain, damage and halation's chain, dynamic resolution running on the frame law (it settled at
  0.6 on SwiftShader). Web: `GATE=post-fx tools/build.sh web-gate` and the sample project's gate pass.
