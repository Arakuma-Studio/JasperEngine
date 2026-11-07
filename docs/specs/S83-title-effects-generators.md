# S83 — Title effects and generators: the full Pachislop catalogue, genre effects, live variants

**Wave / Depends on.** Title Art program (spec-first), **wave 6B** (with S81, S82, S86; program
graph in §8, program rules in S80 §6.15). Depends on:
- **S80** (wave 6, lands first; every hook below is in S80 §6.15's table): the `.titleart` document, the 2D renderer and its pixel contract, the
  effect and generator registry, the distance-field (SDF) service, the core effects (Outer Glow,
  Inner Glow, Drop Shadow, Inner Shadow, Outline, Color Overlay, Gradient Overlay, Pattern
  Overlay, Gaussian Blur, Adjust), the generators `fill` and `pattern_fill`, `TitleArtOps`, and
  the MCP tools `list_title_library`, `get_title`, `edit_title`, `render_title`, `check_title`;
- **S81** (same wave; S81 owns them): Realtime mode, its time-sliced cache, the bake sidecar, the live compose
  that runs the live kernels this spec defines (S81 R9), Glyph Atlas and its `glyph_safety` classes
  (S81 R31);
- **S85** (wave 6C, after this spec): the time axis; it animates the parameters this spec classes
  with S80's `FxAnim` (S85 R11-R12) and depends on the live variants of §6.12.

S82 (the Title Designer) shows what the registry declares; S84 converts Pachislop looks with the
id map of §6.9; S86 hosts the 3D glint of §6.6.30 inside its 3D render. Owner area:
- S83's definitions in S80's layout: `engine/core/titleart/effects/*.cpp` and
  `engine/core/titleart/generators/*.cpp` (one file per family); `engine/core/titleart/analysis/`
  (CPU analysis: highlights, edges, contours, layouts); `engine/core/titleart/gpu/QuadBatch.*`;
- shader code as `TA_PROGRAM_*` programs of S80's `ShaderType::TITLE_ART` (S80 §6.3), in includes
  `engine/shaderlib/titleart/fx/*.glsl`, `engine/shaderlib/titleart/gen/*.glsl` and the shared
  `engine/shaderlib/titleart/tfx_*.glsl`;
- `editor/resources/titleart/fx-presets/` (one YAML file per effect or generator type);
- `editor/titleart/fx/FxRecipes.*`, `editor/titleart/fx/FxCost.*`;
- S83's columns of the generated `docs/mcp/guide/title-art-effects.md` (S80's
  `tools/titleart/gen_effect_docs.py`) and the "Effects and generators" section of
  `docs/mcp/guide/title-art.md`.

## 1. Goal

Every effect and generator Pachislop has, and the genre effects it lacks, are available to Title
Art documents with one definition each: the Title Designer, MCP, Lua, the generated docs page and
the golden tests all read that definition. A person or an agent can make a casino title, a horror
logo with drips and grunge, a retro arcade title with scanlines and pixels, a sci-fi title with a
holographic foil and glitch, or an embroidered sports badge, and see what each layer costs on a
desktop and on a phone.

The look comes from Pachislop's ideas, which are ported exactly: the power-law Light Glow with its
camera over-exposure response, glints placed on real highlights by a Hessian ridge test, lightning
drawn with MAX blending, flames that follow a distance field upstream, shatter pieces whose cut
edges carry their exact distance, 集中線 with a clean silhouette hole. The ports also fix what the
report found weak: no readback or jump flood at runtime when a bake can hold the result, no
desktop-only float formats, no pinned pseudo-effects, and no generators shown as effects.

## 2. Scope

**In scope**
- The 17 Pachislop effects S80 does not port (§6.6.1-6.6.17): Light Glow, Contour Lines, Motion
  Blur, Lens Blur, Tilt-Shift Blur, Radial Blur, Extrude, Motion Trail, Halftone, Perspective
  Shadow, Floor Reflection, Wave Warp, Chromatic Aberration, Grain, Glitch, Material, Shatter.
- The 15 Pachislop generators S80 does not port (§6.7.1-6.7.15): Particles, Ink Splatter, Speed
  Lines, Light Streaks, Shockwave, Magic Circle, Brush Swirl, Glints, Lens Flare, Backlight, Fire,
  Smoke, Aura, Lightning, Sunburst. Pattern Fill and Fill are S80's (§6.7.17); S83 adds to them.
- Ten new genre effects (§6.6.18-6.6.27): Distress, Drips, Scanlines, Pixelate, Ink Brush Stroke,
  Frost, Neon Tube, Chalk, Stitch, Holographic Foil. One new generator: Fractal Noise (§6.7.16).
- Twelve new procedural material kinds for S80's pattern library (wood, stone, metal and more,
  §6.6.28), used by Pattern Overlay, the Material `pattern` member and Pattern Fill.
- The two Pachislop pinned pseudo-effects, redesigned as style settings (§6.6.29 かすれ Dry
  Brush, §6.6.30 Glint (3D highlights)): no pinned rows in an effect stack.
- Presets: the 72 Pachislop presets of these 15 generators (its 81 less Pattern Fill's 9), the
  effect presets, and new presets for the genre effects (§6.8), as editor data files.
- Shared machinery: `QuadBatch` (instanced oriented quads with sub-pixel energy preservation), the
  CPU analysis library on a packed 8-bit readback grid, seeded random streams, the 8-bit light
  headroom encoding, the cost model and readout (§6.4, §6.5).
- Per entry: parameters (types, defaults, ranges, units), pass structure, cost class, seeds,
  animatable parameters for S85, 8-bit fallback, and the live variant for S81 or "bake only".
- Registry integration: inspector rows, MCP validation, Lua names, the generated docs page,
  goldens, the Add Effect and Add Generator menus (§6.10), MCP additions (§6.11).
- Tests: a golden per effect at defaults plus its presets, one per generator preset, parity with
  Pachislop's references, determinism, forced 8-bit fallback, static and timed budgets (§7).

**Out of scope**
- The registry mechanism itself, the effect runner, distance fields, blend modes, masks, the
  document format and the core effects: S80. This spec states what it needs from them (§8).
- Realtime caching, time slicing, the Title Art Renderer component and export: S81. Keyframes,
  tracks and flipbooks: S85. Designer panels and gizmo drawing: S82. Looks and the converter: S84.
  The 3D render and its tone map: S86.
- A particle simulation. Title particles are a closed-form sampler (D6); simulated particles stay
  S20's. A "convert to Particle System" command is not provided (§9 risk 6).
- Changing the HTML UI effects (`engine/shaderlib/effects/`) or the scene post stack (S13). No
  title effect is added to them (D9).
- The do-not-copy list of `docs/specs/README.md` is not touched: everything here is fragment
  passes, instanced quads and CPU jobs; nothing needs a render graph.

## 3. Current state (`feature/title-art` 3ffafdfaa, read)

- **No title code exists.** `engine/core/titleart/`, `editor/titleart/` and
  `engine/shaderlib/titleart/` are absent. S80 (`docs/specs/S80-title-art-core.md`, written in
  parallel, read at its current text) fixes what S83 builds on:
  - §6.1 the document: effect entry `{id, type, enabled, params}` with every param written;
    generator layer `{generator: {type, params}, follow}`; `px` units scaled by `px_scale`; angles
    clockwise on screen from +x except Photoshop-convention light directions; `style.erode` (かすれ)
    as a Style2D member; Material with `pattern.kind` and `texture.kind` enums;
  - §6.2 the ops (`add_effect`, `add_layer {type: "generator", generator}`, `set` with clamping
    echoed as `clamped`, the `fx:<type>#n.` and `gen.` shorthands);
  - §6.3 the renderer: one `ShaderType::TITLE_ART` with `TA_PROGRAM_*` programs and includes in
    `engine/shaderlib/titleart/`; targets addressed in memory rows with `ta_dir(angle,
    convention)`; regions of interest grown by `reach`; content hashes and node caches; the RGBA8
    fallback (SRGB8A8 premultiplied linear accumulators, jump-flood seeds as RGBA8 12.4 fixed point,
    distances as 16-bit fixed point) behind `TitleArtCaps` and the per-request flags
    `TITLE_ART_TEST_NO_FLOAT_TARGETS`, `_NO_R16F`, `_NO_RGBA32F`, `_NO_FLOAT_BLEND`;
  - §6.4 the 18 blend modes, Normal and Add through `PREMULTIPLIED`/`ADDITIVE_PREMULTIPLIED`, and
    the ink-versus-light stack (`adds_light`, `outline`);
  - §6.5 `TitleEffectRegistry` (`EffectDef`, `GeneratorDef`, `ParamDef` with `ParamKind`
    `Number ... Adjustment`, `CostClass` C0-C4 (S80's integrated text), `passVersion` in the bake version), the
    ten core effects and the generators `fill` and `pattern_fill`; the docs page
    `docs/mcp/guide/title-art-effects.md` generated by `tools/titleart/gen_effect_docs.py`;
  - §6.11-6.12 `list_title_library {kind: "effects" | "generators", id?, query?}`, `render_title`,
    `check_title` and the `title_art` budget (S80 §6.11's program budget table).
- **HTML UI effects** (`engine/shaderlib/effects/`, 103 `.frag` files) already have per-element
  versions of a third of Pachislop's effects: `glints.frag` (a grid-scattered port of shaders.com
  Sparkle, `@param density`), `sunburst.frag`, `fire.frag` (a generic edge flame), `scanlines.frag`,
  `crt.frag`, `pixelate.frag`, `halftone.frag`, `glitch.frag`, `chromatic-aberration.frag`,
  `emboss.frag`, `outline.frag`, `neon-border.frag`, `frosted-glass.frag`. Their manifest grammar is
  `engine/core/htmlui/effects/EffectManifest.h:16-41`; they work in CSS px on premultiplied sRGB
  through `JfxContext` and a parameter-only uniform block. The include library
  `engine/shaderlib/effects/lib/` (`jfx_hash.glsl`, `jfx_noise.glsl`, `jfx_color.glsl` with OKLab,
  `jfx_tone.glsl` with Bayer, `fam_sim_blend.glsl` with 18 blend modes) is a shaders.com port
  (MIT, `NOTICE.md:91`).
- **Blending.** `BlendMode` (`engine/core/render/Render.h:47-59`) has no MAX and no "add colour,
  union alpha" mode; `BlendOperation` is `{ADD, SUBTRACT, REVERSE_SUBTRACT}` (:470). sokol has
  `SG_BLENDOP_MAX` (`engine/libs/sokol/sokol_gfx.h:2783`, GL :9612, D3D11 :13294). Dynamic-path
  blends map to sokol in `engine/renders/sokol/SokolPipelineCache.cpp:219`.
- **Float targets.** `RenderFeatures` (`engine/core/render/SystemRender.h:24-39`) reports
  render/blend/filter per float format. The fallback precedent is `FluidSimulator`'s `Caps` with
  test flags such as `FLUID_TEST_NO_FLOAT_TARGETS` (`engine/core/fluid/FluidSimulator.cpp:98-115`);
  `AtmosphereRenderer.cpp:235` gates on `renderRGBA16F && filterRGBA16F`. RGBA16F rendering needs an
  extension on GLES3/WebGL2 (report e1 §1.2); RGBA32F is not filterable on iOS.
- **Readback** is synchronous and 8-bit: `FrameReadback::readTexture` takes 4 or 1 bytes per pixel
  (`engine/core/render/FrameReadback.cpp:247`); float reads are not supported (e1 §1.3).
- **S20 particles** (`engine/core/particles/`): emitter shapes `Sphere ... Mesh`
  (`ParticleEffect.h:96`), with no mask or texture emitter; render modes (:384); the stateless hash
  `hashRandom` (`ParticleKernels.h:35-43`). S49 specifies an Analytic (closed-form) mode; it has
  not landed.
- **Thin-film iridescence** exists for PBR: `iridescenceFresnel(cosTheta, filmIor, thicknessNm,
  baseF0)` (`engine/shaderlib/includes/iridescence.glsl:37`, adapted from OpenEngine, MIT).
- **Line ribbons**: `engine/core/render/LineRibbonBuilder.h` builds camera-facing mitred ribbons
  for Line and Trail Renderers.
- **Cost readout precedent**: `estimatePostProcessingCost` (`editor/PostProcessingPresets.h:78`,
  S13 R17). **GPU timing**: `SystemRender::acquireGpuTiming` (`SystemRender.h:149`) and
  `GpuTimingState` (`engine/core/render/RenderStats.h:88`), S15's per-pass timings.
- **Goldens**: `EXPECT_GOLDEN` in core GPU suites, images in `engine/tests/core/golden/`, written
  on glcore only, Vulkan must match; real-GPU lane on server H (RTX 3080 Ti); wall-clock budgets
  through `perfSlack()` (`docs/testing.md`, "Golden images", "The real-GPU lane").
- **Rule**: a disabled effect adds no pass, no draw and no allocation (`.claude/rules/rendering.md`).

## 4. Reference

### 4.1 Pachislop (`/tmp/pachislop-analysis/repo` at `1c32502`, by SafEight, used with the author's permission): files ported

Code, shaders and looks are ported to C++/GLSL with a `NOTICE.md` entry (§6.1 D15). Every row's
"Improve" column is what the port changes on purpose.

| Pachislop file | Ported as | Improve |
|---|---|---|
| `src/effects/a/glow.ts` (`deepGlow`, `contourLines`), `src/effects/a/lightGlow.ts` | `light_glow`, `contour_lines` | Mobile tier accumulates down the pyramid (D11); 8-bit light headroom (§6.5) |
| `src/effects/a/blur.ts` (`motionBlur`, `lensBlur`, `tiltShift`) | `motion_blur`, `lens_blur`, `tilt_shift` | `textureLod` in every loop (D3D11, report p4 §5) |
| `src/effects/a/distort.ts` (`radialBlur`, `wave`, `chromaticAberration`) | `radial_blur`, `wave_warp`, `chromatic_aberration` | Wave gains a `turbulent` type |
| `src/effects/a/stylize.ts` (`gradientOverlay` → S80, `extrude2d`, `motionTrail`, `halftone`) | `extrude`, `motion_trail`, `halftone` | Halftone gains dot shapes for comic screens |
| `src/effects/a/shadow.ts` (`perspectiveShadow`, `floorReflection`; drop and inner → S80) | `perspective_shadow`, `floor_reflection` | Layer box from S80's CPU bounds, not a 6-pass GPU reduction |
| `src/effects/a/finish.ts` (`glitch`, `grain`, `keyline`) | `glitch`, `grain`; Keyline is S80's Outline | Glitch gains `blocks` mode |
| `src/effects/a/recipes.ts` | `editor/titleart/fx/FxRecipes.cpp` | Same recipes (shine, rainbow, long shadow, vignette), snake_case |
| `src/effects/b/material.ts` | `material` | Fields cached by content key instead of rebuilt every run |
| `src/effects/b/shatter.ts`, `src/effects/b/common.ts` | `shatter`, `analysis/ReadGrid`, `gpu/QuadBatch` | Layout on a job; packed 8-bit grid |
| `src/effects/lib.ts` (`sdf`, `bounds`, `blur`) | S80's distance fields (`render/FieldPasses.cpp`); S83 uses them | Cached by content hash; RGBA8 seeds on devices (S80) |
| `src/effects/globalLight.ts` (`lit`) | S80's `withGlobalLight(def, angleKey, relation)` | Same three relations: from, away, floor |
| `src/generators/common.ts` (`GLINT_STAR_GLSL`, `ANAMORPHIC_GLSL`, `drawInstancedQuads`) | `tfx_glint_star.glsl`, `tfx_anamorphic.glsl`, `QuadBatch` | |
| `src/generators/particles.ts` | `particles` | Adds `time`, `loop`, `gravity` (closed form) for animation |
| `src/generators/inkSplatter.ts` | `ink_splatter` | Adds `growth` |
| `src/generators/lines/{speedLines,lightStreaks,sunburst,shared}.ts` | `speed_lines`, `light_streaks`, `sunburst` | Focus layer = the generator's `follow`; sunburst rotation live |
| `src/generators/rings/{shockwave,magicCircle,brushSwirl,shared}.ts` | `shockwave`, `magic_circle`, `brush_swirl` | Magic circle rows are an array; brush swirl gains `reveal` |
| `src/generators/glints.ts` | `glints`, `analysis/HighlightFinder` | Anchors cached with the bake; live twinkle |
| `src/generators/light/{lensFlare,backlight}.ts` | `lens_flare`, `backlight` | |
| `src/generators/energy/{fire,smoke,corona,lightning,shared}.ts` | `fire`, `smoke`, `aura`, `lightning`, `tfx_upstream.glsl` | Noise texture and baked upstream field on the mobile tier |
| `src/generators/pattern.ts`, `src/engine/patterns.ts` | S80's `pattern_fill` and pattern library | Twelve new kinds, a scroll `offset` |
| `src/ui/erode.ts`, `src/ui/glint.ts` | Style settings (§6.6.29, §6.6.30) | Not pinned effect rows |
| `src/generators/fromEffect.ts`, `src/model/companions.ts` | Add Effect's "Generators that follow this layer" section (§6.10) | Real layers in a visible group, no folding |
| `tests/harness/cases.ts:360-376`, `tests/golden/compare.ts`, `tests/golden/refs/{effect,generator}/` | `TitleFxGoldenGpu`, `TitleFxParityGpu` | Refs copied as fixtures; parity tolerance §7 |
| `tests/perf/budgets.json` | `TitleFxBench` budgets (§5 Performance) | Per class, desktop and mobile |

### 4.2 Engine code shared, and code deliberately not shared

| Engine code | Decision | Why |
|---|---|---|
| `engine/shaderlib/effects/lib/jfx_color.glsl` (sRGB, OKLab, HSV) | **Shared** (`#include`) | Pure maths, same conventions |
| `engine/shaderlib/effects/lib/jfx_tone.glsl` (Bayer 2/4/8) | **Shared** by Pixelate | Same dither the HTML UI and S48 use |
| `engine/shaderlib/includes/iridescence.glsl` | **Shared** by Holographic Foil | One thin-film model for PBR and foil |
| `jfx_hash.glsl`, `jfx_noise.glsl` | **Not shared** | Pachislop's presets were tuned against its own hash and value noise (Hoskins hash, mulberry32 streams); different spectra would move every flame and star. `tfx_hash.glsl` and `tfx_noise.glsl` port those |
| HTML UI `glints.frag`, `sunburst.frag`, `fire.frag`, `scanlines.frag`, `pixelate.frag`, `halftone.frag`, `glitch.frag`, `chromatic-aberration.frag`, `neon-border.frag` | **Not shared** | Different pixel contract (CSS px, 8-bit premultiplied sRGB, `JfxContext`), different units, and no texture inputs in the block (report e1 §1.4); the title versions need linear premultiplied light above 1, `px_scale` units and SDF/anchor textures. Parameter names follow theirs where the meaning is the same (`spacing`, `thickness`, `intensity`) |
| S13 uber features (`UBER_CHROMATIC`, `UBER_GRAIN`, `RenderSystem.h:414-429`) | **Not shared** | Scene-wide, one pass for the frame; titles need per-layer, alpha-aware versions |
| S20 particles | **Not used** for title particles (D6) | A fixed-tick simulation cannot give bake = Realtime parity or exact scrubbing; the title sampler is closed form. `tfx_particle_shapes.glsl` is written so S20 can include it later |
| `ParticleKernels.h` `hashRandom` | **Not used** | Parity with Pachislop's mulberry32 streams (D7) |
| `LineRibbonBuilder` | **Not used** by Lightning | Pachislop's bolts are independent quads overlapped half a width and merged by MAX blending, which needs no joints; a mitred ribbon would change the look |
| S43 palettes (`.palette`, OKLab nearest) | **Shared** by Pixelate | One palette asset type |
| `estimatePostProcessingCost` | **Pattern copied** for `estimateTitleFxCost` | Same readout shape |

### 4.3 Not taken

- The third-party product names Pachislop used as look references are not used as feature names,
  aliases or docs text (they are in S84's denied terms); the docs say "power-law glow", "star
  filter", and so on. Pachislop's own type keys (`deepGlow`, ...) appear only in the converter's id
  map (§6.9), as source keys.
- Pachislop's two overlapping param-builder families and its inline GLSL strings: one C++ table
  per definition, one shader include per pass family.
- The 6-pass GPU bounds reduction (`bounds`): S80 knows a layer's content box on the CPU.
- The folded companion group and pinned pseudo-effects (report p5 §7.2 item 2).

## 5. Requirements

### Functional — registry integration

- **R1. One definition per type.** Every effect and generator of §6.6 and §6.7 is one entry of
  S80's `TitleEffectRegistry` (`EffectDef` or `GeneratorDef`, S80 §6.5, with the fields S83 adds
  in §6.2), registered from one `.cpp` per family in `engine/core/titleart/effects/` or
  `generators/`. The entry is the only place that states the type's id, user
  name, category, parameters (key, label, type, default, range, step, unit, `when` condition,
  animation class, one-line doc), gizmo hints, `reach`, `gather`, `adds_light`, `outline`,
  inputs (`input`, `content`, `sdf`, `analysis`), global-light relation, cost model, fallback class
  and live variant. Inspector rows (S82), MCP validation, Lua names, the docs page and the golden
  list are generated from it; no list elsewhere names the types.
- **R2. Ids and names.** Ids are snake_case (`light_glow`, `speed_lines`); user names are
  Unity/After Effects-like Title Case ("Light Glow", "Speed Lines", "Tilt-Shift Blur"). Param keys
  are snake_case. An id never changes once shipped; a rename keeps the old id as an alias that
  `normalizeTitleArt` rewrites (S80 migration rule 1: no version bump).
- **R3. Search aliases.** Each definition lists aliases searched by the Add menus, `search_editor`
  (S25) and `list_title_library {query}`: Pachislop's labels and types, Japanese terms and common
  names (`light_glow`: "power glow", "bloom", "glare", "発光"; `speed_lines`: "集中線", "流線",
  "manga lines"; `aura`: "corona", "オーラ"; `distress`: "grunge", "かすれ", "worn").
- **R4. Parameters are total and stable.** Every parameter has a default inside its range; a
  stored value outside the range is clamped on read with a warning (S80 lenient read); an unknown
  param is preserved. A parameter hidden by `when` still applies its value and is still written.
- **R5. Effect vs generator.** Effects transform a layer's pixels and live in an Effects stack.
  Generators draw new content and are Generator layers (`type: "generator"`). No generator is
  offered as an effect entry; the Add Effect menu's generator section adds a layer and says so
  (§6.10, D3).
- **R6. Following.** A generator definition declares `follow: required | optional | none`. A
  following generator reads the followed layer (S80 `follow` member) as `content` (its pixels
  before its own effects: shapes, flame sources, emitter masks) or `styled` (after its effects:
  highlight analysis). With `follow` required and empty or dangling, it draws nothing and reports
  `fx_follow_missing` (`check_title`, §6.11) instead of failing the render.
- **R7. Presets.** Each type has a preset file `editor/resources/titleart/fx-presets/<id>.yaml`:
  `{presets: [{id, name, aliases, params, layer?: {blend, opacity, place: above|below, effects:
  [...]}}]}`. Effect presets set params only; generator presets may also set the generator
  layer's blend, opacity, its own effects and its place relative to the followed layer (Pachislop
  `GeneratorPreset`). Adding a generator without a preset uses its first preset. Preset ids are
  snake_case and unique per type.
- **R8. Recipes.** Menu entries that are presets of other types (Long Shadow, Shine, Rainbow,
  Vignette, Emboss, Deboss, Icicles, CRT, Comic Dots, Ben-Day Dots) are recipe functions in
  `editor/titleart/fx/FxRecipes.cpp` (Pachislop `recipes.ts`), applied through ops, never new
  types (D4).

### Functional — shared machinery

- **R9. Pixel contract.** Every pass reads and writes S80's contract: linear, premultiplied,
  targets addressed in memory rows (row 0 = the top), RGBA16F where `renderRGBA16F &&
  filterRGBA16F`, and the RGBA8 path of §6.5 otherwise. Pachislop's y-up formulas get their
  directions through S80's `ta_dir(angle, convention)` and CPU positions through `TitleFrame`, once
  per formula with a comment, never by flipping a pass. Light is encoded as Pachislop's: RGB may
  exceed 1, alpha = `clamp(max(r, g, b), 0, 1)`; light passes blend colour `(ONE, ONE)` and alpha
  `(ONE, ONE_MINUS_SRC_ALPHA)` (`BlendMode::LIGHT_PREMULTIPLIED`, R12).
- **R10. Units.** `px` params are canvas px times `px_scale`; `%` params are percent of the
  param's stated base; `deg` params are clockwise on screen from +x, except the light directions
  (`light_angle`) which use S80's Photoshop convention. Analysis grids are locked to canvas px on a
  lattice from the canvas centre, so a glint, bolt or stitch lands at the same canvas position at
  any `px_scale` (Pachislop's comp-pixel invariance).
- **R11. `QuadBatch`** (`engine/core/titleart/gpu/QuadBatch.{h,cpp}`, shaders
  `tfx_quads.glsl`): one instanced draw of oriented quads with per-instance centre, half size,
  rotation, premultiplied colour and a free `vec4`; a body function per user shader receives
  `quad` (−1..1), `color`, `extra`, `size_px`. Quads below `min_px` (default 1.5 render px) are
  drawn at `min_px` with colour scaled by the area ratio. Blends: normal, add, screen, light
  (R9), max. Up to 65,536 instances per draw from a streamed instance buffer. Used by particles,
  glints, ink splatter, speed lines, light streaks, lightning, stitch and drips.
- **R12. Blend modes.** S83's passes use S80's `LIGHT_PREMULTIPLIED` (colour ONE/ONE, alpha
  ONE/ONE_MINUS_SRC_ALPHA) and `MAX_PREMULTIPLIED` (`BlendOperation::MAX`, all factors ONE) (S80
  §6.4, mapped on all five backends there).
- **R13. Analysis grid.** `analysis/ReadGrid` reads a layer (styled or content) box-averaged to
  cells fixed in canvas px (4, 6 or 8 px per cell, per consumer), packed into RGBA8: R = mean
  alpha, G/B = mean premultiplied luminance as 16-bit fixed point over 0..4, A = a mean-saturation
  byte for colour sampling. One `FrameReadback::readTexture` per grid (4 bytes/px, every backend),
  at most 480 × 288 cells (0.55 MB). Grids are cached by S80's content key of the layer, so an
  unchanged layer is never read back twice.
- **R14. Analysis results are bake products.** Highlight anchors, edge lists, contours, shatter
  layouts and emitter masks are computed in the editor, cached by the analysed layer's content
  hash, and written into S81's bake sidecar (S81 R8) and Realtime export data for every title that
  is Realtime or has a live variant.
  A title whose analysed layers never change
  at runtime performs no readback on the device. A runtime text change re-runs the analysis inside
  S81's time-sliced re-render (one readback per analysed layer, R50 budget).
- **R15. Seeds.** Every random element draws from `tfx::Stream(seed_total, index, salt)`:
  mulberry32 seeded by a 32-bit mix of the element's `seed` param, the document `seed` (S80), the
  type id hash, the element index and a per-purpose salt. All random values of an element are drawn
  before any condition, so toggling one option keeps others in place, and changing a count never
  moves elements with lower indices (Pachislop `stream(seed, i, salt)`). GPU noise uses
  `tfx_hash.glsl` (Hoskins "Hash without Sine", `highp`) and `tfx_noise.glsl`.
- **R16. No clock.** No effect or generator reads time. Motion is animated parameters (S85):
  every param marked L or R in §6.6/6.7 may be keyed; `phase`, `time`, `offset`, `reveal`,
  `growth`, `twinkle_phase` and `evolution` exist for that purpose; strobing effects (lightning, speed lines, glitch,
  grain, neon flicker) animate their `seed` with S85's stepped "Reseed every N frames" track.

### Functional — the catalogue

- **R17. Effects.** The 27 effect types of §6.6.1-6.6.27 exist with the parameters, passes, cost
  model, fallback class and live variant their entry states. The 12 pattern kinds of §6.6.28 join
  S80's pattern enum. §6.6.29 and §6.6.30 are style settings with Add Effect entries.
- **R18. Generators.** The 16 generator types of §6.7.1-6.7.16 exist likewise.
- **R19. Light Glow kernel.** `light_glow` implements Pachislop's power-law kernel exactly:
  intensity ∝ r^−p with `p = 1.6 + 1.6 × falloff/100`, built as a scale mixture of unit-energy
  Gaussians weighted ∝ σ^(2−p) per octave (§6.6.1); octave weights are normalised to sum to 1
  so `radius` and `falloff` never change total energy; the over-exposure response is the identity,
  bit for bit, where no channel exceeds 1.
- **R20. Glint placement.** `glints` with `placement: highlights` runs Pachislop's
  `findHighlights`/`pickHighlights` (§6.7.8) on the styled followed layer: a lit straight edge gives
  no row of evenly spaced stars (Hessian ridge weight), a dimmer second text line still gets
  glints (regional reference), large glints spread over the whole ink.
- **R21. Lightning continuity.** Bolt cores are drawn with `MAX_PREMULTIPLIED`; joints and
  crossings never exceed a single bolt's brightness.
- **R22. Shatter edges.** Each piece's per-vertex edge distance interpolates to the exact distance
  to its cut edge; pieces tile the plane with no gap at `gap` 0; texture coordinates stay at the
  undisplaced position.
- **R23. Speed-line hole.** Focus lines never enter the focus region (ellipse, rectangle, or the
  followed layer's silhouette dilated by `padding`), with the 1,024-sample boundary table and the
  ±2-sample running max of §6.7.3.
- **R24. Flames follow shapes.** Fire, Smoke and Aura derive their field from the followed layer's
  content SDF through the 40-step `upstream` march (`tfx_upstream.glsl`), so every silhouette
  gets rising flames with no particles.
- **R25. Particle stability.** Particles is a sampler of states, never a simulation: a particle's
  state is a pure function of `(params, index, time)`; changing `count` keeps the first N
  particles identical; a flipped mask cell moves only particles sampled on it.

### Functional — genre effects

- **R26.** Distress, Drips, Scanlines, Pixelate, Ink Brush Stroke, Frost, Neon Tube, Chalk, Stitch
  and Holographic Foil (§6.6.18-6.6.27) and Fractal Noise (§6.7.16) are designed like the ports:
  same units, seeds, cost model, fallback, goldens and docs.
- **R27. Shape-derived effects read `content`.** Distress, Drips, Ink Brush Stroke, Frost, Neon
  Tube, Chalk, Stitch, Contour Lines and Shatter's ink box read the layer's content (and its SDF),
  not the stack's input, so an earlier glow never grows their band (Pachislop `ctx.content`).
- **R28. Recipes cover the rest of the genre list.** Long Shadow (Extrude), Emboss and Deboss
  (Material), Icicles (Drips), Comic Dots and Ben-Day Dots (Halftone), CRT (Scanlines), Shine,
  Rainbow and Vignette (Gradient Overlay) are menu entries with recipes (§6.8); "keyline" is an
  alias of S80's Outline, which is Pachislop's Keyline.

### Platform & backends

- **R29.** Every pass is a fragment shader over a full-screen triangle or a bounded quad, or an
  instanced draw; no compute, SSBO, bindless, indirect draw or 64-bit atomics. All shaders compile
  through `engine/libs/shadercompiler` for GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan.
- **R30.** Loops with a data-dependent length use `textureLod(…, 0)` or `texelFetch` and a
  constant maximum with `break`; every title shader declares `precision highp float`.
- **R31.** No definition needs a renderable or filterable RGBA32F target at runtime. SDF seeds
  use S80's float targets in the editor and its RGBA8 12.4 fixed-point seed encoding where
  `renderRGBA16F` is false (§6.5); data textures (speed-line table, gradient LUTs) are RGBA16F or
  RGBA8 sampled with nearest or linear filtering, which every backend samples.
- **R32. Forced fallback.** S80's per-request flags `TITLE_ART_TEST_NO_FLOAT_TARGETS` and
  `TITLE_ART_TEST_NO_FLOAT_BLEND` (the `FluidSimulator` pattern) and `render_title {fallback: "rgba8"}`
  force the RGBA8 path; every definition agrees with its float render within its fallback class
  (§6.5).
- **R33.** Exported games build on Windows, Linux, macOS, Android, iOS and HTML5 with the module on
  and off; with `JASPERENGINE_TITLE_ART` off no S83 code or shader is linked (S80 module rule).

### Performance (budgets)

Classes and the cost model are in §6.4. Desktop timings are measured on server H (RTX 3080 Ti) at
a 1920 × 1080 bake; mobile figures are for a Mali-G57 / Adreno 610-class phone with a 1024 × 512
title surface, predicted by the static model and confirmed by hand (AC22).

| Class | Static limit at defaults (1920 × 1080) | Desktop GPU (H) | Mobile re-render | Per-frame live kernel |
|---|---|---|---|---|
| C0 trivial | ≤ 1 FRE, ≤ 16 taps/px | ≤ 0.1 ms | ≤ 0.4 ms | allowed |
| C1 light | ≤ 3 FRE, ≤ 64 taps/px | ≤ 0.3 ms | ≤ 1.2 ms | allowed |
| C2 medium | ≤ 10 FRE, ≤ 200 taps/px | ≤ 1.0 ms | ≤ 4 ms | only the declared live kernel |
| C3 heavy | ≤ 40 FRE, or needs SDF/analysis | ≤ 4 ms | ≤ 15 ms, time-sliced | no |
| C4 bake only | above C3 | ≤ 40 ms | not in Realtime on the mobile tier (D12) | no |

- **R34.** Each definition's entry states its class at defaults; `TitleFxBudget.StaticModel`
  checks every type at defaults and every preset against its class's static limit (presets may be
  up to one class heavier, stated in their file).
- **R35. Live kernels.** A live kernel costs ≤ 1.2 FRE over its own bounds and ≤ 2,000 instances on
  the mobile tier (≤ 0.45 ms each predicted); one title's live compose stays within S81's per-title
  live budget (0.5 ms on mobile and web, 1 ms on desktop, S81 R9), else S81 re-renders and
  `check_title` reports `fx_live_budget` with fixes (§6.11).
- **R36. CPU.** On server A (Release editor), single thread: highlight analysis of a 480 × 288 grid
  ≤ 3 ms; edge extraction ≤ 1 ms and 12 bolts ≤ 1 ms; shatter layout ≤ 2 ms (glass, 400 pieces) and
  ≤ 8 ms (Voronoi, 400); speed-line layout (600 lines, layer focus) ≤ 1.5 ms; 5,000 particles ≤ 2
  ms; ink splatter ≤ 2 ms; stitch contour ≤ 2 ms. Analysis runs on the S06 job system, never on the
  render thread.
- **R37. Disabled costs nothing.** A disabled effect, a hidden generator layer and a type a
  document does not use add no pass, no draw, no target and no shader variant (variants compile
  only for types a document uses, S32 warm-up list); the cost readout of a document with every
  effect disabled equals that of the same document without them.
- **R38. Memory.** Per analysed layer: grid ≤ 0.55 MB CPU; SDF per S80; light accumulation
  targets are pooled from S80's scratch pool at render size, never kept between renders except the
  splits that live kernels read (S81).

### Editor UX (human)

- **R39. Inspector rows** (S82 draws them from the definition): one control per param type
  (slider with unit, angle dial, colour, gradient, curve, enum dropdown, bool, vec2 handle, layer
  picker, asset picker, array of rows with add/remove for magic circle rows); `when` hides rows;
  presets as chips at the top; a dice button on every `seed` (one undo step "Randomize Seed").
- **R40. Gizmos** declared by the definition: `point` (vec2 handle), `direction` (angle, optional
  length), `radius` (circle/ellipse/box), `light` (angle + altitude sphere, writes the global light
  while `use_global_light` is on). S82 draws them; each drag is one undo step.
- **R41. Cost badge.** Each effect row and generator layer row shows its class (C0-C4) as a small
  badge; the tooltip gives the estimate on desktop and mobile and "Bake only" where it applies; the
  Effects header shows the stack total. A Realtime document marks C4 entries amber with the text
  "Baked on mobile" (D12).
- **R42. Add Effect and Add Generator menus** follow §6.10: categories, counts per category,
  search over names and aliases (English and Japanese), hover preview (S82), the generator section
  inside Add Effect, style-setting entries.
- **R43. Missing follow.** A following generator whose layer is gone shows the row warning
  "Follows a deleted layer" with a "Pick Layer" button.

### Agent UX (MCP, Lua, CLI)

- **R44. Discovery.** S80's `list_title_library {kind: "effects" | "generators", id?, query?}`
  lists S83's types with the row fields and the `category` filter of §6.11. No new tool.
- **R45. Editing.** `edit_title` ops `add_effect {type, preset?}` and `add_layer {type:
  "generator", generator, preset?, follow?, place?, group?}` accept every id and preset id here.
  Unknown types, presets and param keys fail listing the nearest valid names (edit distance ≤ 3,
  then the category's list); out-of-range numbers are clamped and echoed in `clamped` (S80); a
  value set on a param hidden by `when` succeeds with the note `inactive: <key> applies only when
  <cond>`.
- **R46. Cost readout.** `render_title {options: {cost: "desktop" | "mobile"}}` returns the cost model per layer and per
  effect, plus S15 GPU pass times when timing is available (§6.11). `check_title` gains the checks
  `fx_follow_missing`, `fx_bake_only_realtime`, `fx_live_budget`, `fx_realtime_budget`,
  `fx_hdr_on_8bit` with fix ops.
- **R47. Lua.** Scripts reach effect and generator params through Exposed Parameters only (S81
  R66, contract decision 13): exposing the target `{layer: "Title", field:
  "fx:light_glow.intensity"}` as `glow` (S80 `expose` op) lets
  `titleArt:setParameter("glow", 2)` drive it, and an exposed L param changes through S81's live
  compose without a re-render. Ids and keys, which the exposed targets name, are listed in
  `docs/mcp/guide/title-art-effects.md`.
- **R48. Docs.** S80's generated page `docs/mcp/guide/title-art-effects.md`
  (`tools/titleart/gen_effect_docs.py`, checked by `test_docs.py`) covers S83's types; S83 adds its
  columns: aliases, animation class per param, presets, cost class, fallback class and live
  variant. `docs/mcp/guide/title-art.md` gets a section "Effects and generators" (≤ 2,500 bytes);
  the agent guide itself is not changed.

### Data & compatibility

- **R49.** Adding these types, params and enum values does not bump `TITLE_ART_VERSION` (S80 rule
  1). A document naming a type this build lacks keeps it verbatim (S80 `unknown_type`).
- **R50. Analysis budget on device.** A runtime re-render needing analysis performs ≤ 1 readback
  per analysed layer and ≤ 4 ms CPU on the mobile tier (predicted from R36 × 2.5), on a job.
- **R51. Pachislop map.** §6.9 maps every Pachislop effect type, generator type, preset id and
  param key to ours, including unit and angle-convention conversions; S84's converter and S87's
  importers use `editor/titleart/fx/PachislopFxMap.cpp`, which is generated from that table's data
  file `editor/resources/titleart/pachislop-fx-map.yaml`.
- **R52. Bake version.** A change to a definition's output bumps its `passVersion` (S80 §6.5), which
  is part of the bake version, so stale bakes are reported.
- **R53. Glyph Atlas classes.** Every S83 definition has a `glyph_safety` (§6.13) that S81's Glyph
  Atlas uses (S81 R31).
- **R54. S80's definitions get classes and live variants** (§6.12), so S85's minimum live set
  (S85 §6.2) is complete when S83 lands.
- **R55. Colour roles for S84.** Light colours carry `light: true` and the two spectrum switches
  are declared (§6.2, §6.3), so S84's color schemes recolour light without a list of their own.

## 6. Design constraints & decisions

### 6.1 Decisions

- **D1. Definitions are C++ entries of S80's `TitleEffectRegistry`, passes are `TA_PROGRAM_*`
  programs of `ShaderType::TITLE_ART`** (S80 §6.3, §6.5). Not the HTML UI `@effect` manifest: that
  grammar is parsed from shader comments for project-authored effects, has no texture inputs
  (report e1 §1.4) and no CPU stage, while title definitions need cost, reach and layout functions
  and CPU analysis. One include per pass family (`titleart/fx/light_glow.glsl` holds emit, blur,
  accumulate and compose), shared code in `engine/shaderlib/titleart/tfx_*.glsl`.
- **D2. Port for look parity.** The desktop path reproduces Pachislop's maths, constants, hashes
  and noise, so its presets look as tuned and its reference images can be matched (AC4). Where the
  port changes the algorithm (mobile tier, D11), the desktop path keeps the exact one.
- **D3. Generators are layers, discoverable from Add Effect.** Add Effect has a section
  "Generators that follow this layer" listing every preset of every following generator; choosing
  one runs `add_layer {type: "generator", generator, preset, follow: <layer>, place, group:
  "auto"}`. `group: "auto"` wraps the layer and the new generator in a visible Group named
  "<layer name> FX" when the generator's blend is not Normal and the layer has siblings below it, so
  an Add-blended companion lights only that layer (Pachislop's companion group, without folding);
  `"none"` inserts it as a sibling; `"always"` always wraps. The entry's row says "Adds a Generator
  layer". Pachislop's companion and folded-wrapper model is not copied (report p5 §7.2 item 2).
- **D4. Fewer types, more recipes** (Pachislop `effectDedupe.ts`). Keyline is S80's Outline (its
  port, drawn under light), found by the alias "keyline"; Long Shadow is Extrude; Emboss and
  Deboss are Material; Icicles are Drips; CRT is Scanlines; Shine, Rainbow
  and Vignette are S80's Gradient Overlay. Each has its own menu entry and search aliases.
- **D5. No pinned pseudo-effects.** かすれ Dry Brush is S80's `style.erode`; Glint (3D highlights)
  is S86's 3D style member. Both are style settings shown in the layer's Style section; Add Effect
  lists them as entries marked "Style setting" that switch them on and open that section. Nothing
  in an Effects stack is pinned or "deleted by resetting".
- **D6. Title particles are a closed-form sampler, not S20.** Pachislop's particle generator is "a
  sampler of end states": each particle's state is a function of params, index and `time`. That
  gives bake = Realtime parity (contract decision 1), exact scrubbing for S85 and no per-frame
  CPU. S20 stays the tool for world-space simulated effects; `tfx_particle_shapes.glsl` (dot,
  glint, ember, petal, confetti foil, star, heart, bokeh, rain) is written as a standalone include
  so S20's renderer can adopt it later.
- **D7. Randomness.** CPU: mulberry32 streams `tfx::Stream` (R15). GPU: Hoskins "Hash without
  Sine" and Pachislop's value noise, `highp`. No other generator of randomness is used by title
  code, so a render on any backend and any worker count is identical (AC5).
- **D8. Analysis on the CPU from a packed 8-bit grid** (R13), never from float readback: works on
  every backend (`FrameReadback` reads 4 bytes/px), 16-bit luminance is enough for the Hessian and
  NMS, and results are cached and shipped in S81's runtime data (R14).
- **D9. Whole HTML UI effects and S13 post effects are not shared** (§4.2): different pixel
  contract, units and inputs. Maths includes are shared where conventions match.
- **D10. Live variants.** Each definition declares `live.kind`:
  - `none`: animating any param re-renders the layer through S81's time-sliced cache (Realtime)
    or a flipbook (S85 Baked);
  - `scale_split`: the effect's light or shadow output is kept as its own cached split; its L
    params (an intensity or opacity) scale that split per frame in one compose pass;
  - `kernel`: a per-frame pass list reading cached inputs (content split, SDF, anchors, edges,
    instance buffers), run by S81's live compose into the display target (S81 R9; S81's own four
    built-in live layers in `library:title_fx` are separate). The kernel is the same program as
    the bake pass compiled with `TFX_LIVE`, so bake and live cannot drift
    (AC17).
  Only params marked L change per frame without a re-render. A live kernel that moves pixels
  (Wave Warp, Radial Blur, Chromatic Aberration, Glitch, Shatter) reads the stack image before
  it as a cached split; a later non-`scale_split` effect in the same stack makes the animation fall
  back to re-render, which `check_title` reports (`fx_live_budget`, note "effects after a live
  distort re-render").
- **D11. Light Glow on the mobile tier** accumulates octaves down the pyramid (each octave added
  into the next finer level while upsampling, with the upsample's tent variance folded into the
  per-octave correction) and stops at half resolution. The desktop and editor path is Pachislop's
  full-resolution accumulation (D2). The two agree within F1 (AC6 reuses the comparison).
- **D12. C4 in Realtime on the mobile tier.** Each C4 definition names its mobile reduction (Lens
  Blur: 64-tap quarter-res gather; Light Glow: D11; Extrude: steps of twice the length; Glint (3D):
  S86's reduced pass list). A Realtime document whose
  layer would still exceed C3 on the mobile tier keeps that layer's output as a static split baked
  by the editor; when the layer's content changes at runtime the effect is skipped and the player
  logs `titleart: <path> skipped (bake-only effect on this device)` once. `check_title` reports it
  (`fx_bake_only_realtime`, warning) with the fix "set Render Mode to Baked" or "remove the effect".
- **D13. Cost is a model plus a measurement** (§6.4): the static model is deterministic and gates
  CI on any renderer; GPU times are measured on server H and on request in the editor.
- **D14. 8-bit headroom encoding for light** (§6.5): on the RGBA8 path, light targets are
  SRGB8A8 holding premultiplied linear light × 1/4, so additive blending stays linear (the hardware
  decodes sRGB for blending), dark haze keeps precision, and values up to 4.0 survive to the final
  compose. Ink targets stay plain RGBA8 premultiplied.
- **D15. Provenance.** `NOTICE.md` gains: Pachislop (SafEight, used with the author's permission;
  the effect and generator ports listed by file); Inigo Quilez's `sdStar5` and `sdHeart` (MIT, taken
  from his published distance-function articles, not from Pachislop); Dave Hoskins' "Hash without
  Sine" (MIT); mulberry32 (Tommy Ettinger, public domain). Ported shader files keep a header naming
  the Pachislop source file.
- **D16. Angle conventions.** Every `deg` param is clockwise on screen from +x (S80), so 90 = down
  and −90 = up. Pachislop generator params in its counter-clockwise convention are negated by the
  converter (§6.9) and their defaults here are already converted (Fire `angle` −90 = up). Light
  directions (`light_angle`) use S80's Photoshop convention; Pachislop's Extrude "Light From"
  −120 (clockwise) becomes `light_angle` 120.
- **D17. Fallback is a structure, not a different effect.** Every definition runs the same passes
  on the RGBA8 path; only the target formats and the light encoding change (contract decision 3).
  No definition switches to another algorithm on 8-bit devices.

### 6.2 What S83 fills in S80's registry

S80's `EffectDef`, `GeneratorDef` and `ParamDef` (`engine/core/titleart/TitleEffectRegistry.h`,
S80 §6.5) declare every member the catalogue needs, with defaults, so S81, S82 and S86 build
against one header in wave 6B: `type`, `label`, `category`, `description`, `params`, `gizmos`,
`reach`, `gather`, `fillsCanvas`, `addsLight`, `outline`, `onParts`, `costClass`, `fallback`,
`live`, `glyphSafety`, `aliases`, `inputs`, `spectrum`, `estimate`, `analyse`, `presetFile`,
`passVersion`, `apply`/`render`, and for generators `bounds`, `follow`, `followReach`; `ParamDef`
carries `anim`, `interp`, `combine` and `light`; `ParamKind` includes `String`, `Material` and
`RowArray`. S83 sets their values for its 43 definitions and, in the same change, for S80's ten
effects and two generators (§6.12, §6.13). The types, as S80 declares them:

```cpp
namespace jasperengine::titleart {   // TitleEffectRegistry.h (S80)
    enum class FxAnim : uint8_t { None, Live, Rerender, Step, Relayout };   // §6.3's L, R, S, X; S85 R11
    enum class ParamInterp : uint8_t { Smooth, Stepped };    // stepped: holds between keys (a Live seed)
    enum class ParamCombine : uint8_t { Replace, Multiply }; // S85: glow intensities combine by multiply
    enum class GlyphSafety : uint8_t { Safe, Approximate, Unsafe };   // S81 R31, §6.13
    enum class CostClass : uint8_t { C0, C1, C2, C3, C4 };   // §6.4; list_title_library's `cost`
    enum class FallbackClass : uint8_t { F0, F1, F2 };        // §6.5
    enum class LiveKind : uint8_t { None, ScaleSplit, Kernel };   // D10
    enum class FxTier : uint8_t { Desktop, Mobile };

    struct FxCost {                       // the static model, §6.4
        int passes = 0; double fre = 0; int maxTapsPerPixel = 0; int instances = 0;
        double cpuMs = 0; bool sdf = false, analysis = false, readback = false;
    };
    struct FxInputs { bool input = true, content = false, sdf = false;
                      AnalysisSource analysis = AnalysisSource::None; };   // None, Content, Styled
    struct LiveVariant { LiveKind kind = LiveKind::None; std::vector<const char*> params;
                         uint32_t program = 0; };   // the TA_PROGRAM_* compiled with TFX_LIVE
    struct SpectrumSwitch { const char* param; const char* from; const char* to; };
    // ParamDef: FxAnim anim; ParamInterp interp; ParamCombine combine; bool light;
    // EffectDef / GeneratorDef: aliases, inputs, costClass, fallback, live, glyphSafety, spectrum,
    //   estimate (FxCost (*)(const ParamValues&, const FxSize&, FxTier)), analyse (CPU job), presetFile
}
```

`analyse` runs on the S06 job system from the packed grid (R13); its result is cached by the
analysed layer's content hash (S80 `TitleArtHash`) and shipped in S81's runtime data (R14); `render`
receives it.

### 6.3 Catalogue notation

Each entry lists: **Params** as `key` type default [min, max] unit, separated by semicolons; type
letters `f` float, `i` int, `b` bool, `e{...}` enum, `c` colour, `g` gradient, `v2` vec2, `lyr`
layer, `asset`, `str`. Animation marks are S80's `FxAnim` values (S85's classes derive from them): **R** = `Rerender`, smooth (numbers
and colours unless marked); **S** = `Step` (enums, bools and ints unless marked); **X**
= `Relayout` (the CPU layout or analysis re-runs); **L** = `Live` (live variant, D10; a stepped
`seed` marked L reseeds live). Light colours (`light: true`) are those of `adds_light` definitions
and Add-blended generator presets, every colour and gradient param but `shadow`-role ones.
**Passes** at defaults on a 1920 × 1080 bake; **Cost** class (§6.4); **Seeds**; **8-bit** fallback class (§6.5); **Live**
variant (D10); **Mobile** tier reductions. Every effect has the S80 effect members (`enabled`), every
generator the S80 layer members (blend, opacity, transform as an offset when following).

### 6.4 Cost classes, the model and the readout

**The static model** (`editor/titleart/fx/FxCost.{h,cpp}`, `estimateTitleFxCost(doc, tier) ->
TitleFxCost`, mirroring `estimatePostProcessingCost`): each definition's `estimate` returns its
passes, its **full-res equivalents** (FRE = Σ over passes of target pixels × max(1, taps/4), ÷ bake
pixels), its maximum taps per pixel, instances, CPU ms and whether it needs the SDF, analysis or a
readback. Shared work is counted once per layer and shown as its own row: the content SDF (S80, C3
when rebuilt, 0 when cached), each analysis grid. The class follows from the larger of FRE and
taps (table in §5 Performance); S80's `cost` row field reports it. Tier `desktop` uses the
editor path, tier `mobile` the reductions and a 1024 × 512 surface.

**Predicted ms** = FRE × the tier's cost of one FRE (`desktop` 0.012 ms at 1920 × 1080 on H,
`mobile` 0.35 ms per FRE at 1024 × 512), + instances × (0.4 µs desktop, 2 µs mobile). The
desktop constant is calibrated by `TitleFxBench.Calibrate` on H and recorded in `FxCost.cpp` with
the date; the mobile constant is an estimate (S13 has no device measurement either) until AC22
measures it, and mobile figures stay labelled "predicted" in every readout.

**Readout** (Designer badge R41, `render_title {options: {cost}}` §6.11): per effect and generator
`{path, type, class, passes, fre, taps, instances, cpu_ms, sdf, analysis, readback, desktop_ms,
mobile_ms, live: {kind, params, mobile_ms}}`, totals per layer and document, and measured GPU times
from S15 pass timing (labels `titleart/<layer id>/<effect id>/<pass>`) when
`SystemRender::getGpuTimingState().available`.

### 6.5 8-bit fallback classes

The RGBA8 path is S80's (§6.3: SRGB8A8 premultiplied linear accumulators, RGBA8 12.4 seeds, 16-bit
fixed-point distances) and keeps every pass. S83 adds one thing (D14): the light accumulators and
light splits of `adds_light` definitions hold light × 1/4 in SRGB8A8, so values up to 4.0 survive to
the final compose, which multiplies by 4 before the over-exposure response. Writes to RGBA8 add an
ordered Bayer 4 dither of ±0.5/255 so blurs do not band. Agreement with the float render of the
same document, premultiplied comparison (S80 `ImageDiff`):

| Class | Meaning | Limit (mean / max / pixels over 8/255) |
|---|---|---|
| F0 | no light above 1, ≤ 2 accumulating passes | S80's §6.3 tolerance: ≤ 1.0/255 / ≤ 48/255 / ≤ 0.5 % |
| F1 | multi-pass blurs or accumulations, no light above 1 at the end | ≤ 1.5/255 / ≤ 64/255 / ≤ 2 % |
| F2 | look depends on light above 1 (cores, over-exposure) | ≤ 3/255 / ≤ 96/255 / ≤ 5 % |

Light above 4.0 clips on the RGBA8 path; `check_title` reports `fx_hdr_on_8bit` (info) when a
float render of the document has more than 0.5 % of pixels above 4 in any light split.

### 6.6 Effect catalogue

#### 6.6.1 Light Glow (`light_glow`) · Glow · from `deepGlow`
- **Params:** `exposure` f 1 [−4, 6] stops; `threshold` f 0 [0, 2]; `knee` f 0.25 [0, 1];
  `radius` f 200 [4, 1000] px; `falloff` f 50 [0, 100] %; `intensity` f 1 [0, 8] **L**; `chroma`
  f 0 [−100, 100] %; `color_mode` e{layer, custom} layer; `color` c `#ffffff` (custom);
  `tint_amount` f 0 [0, 1]; `tint_inner` c `#ffffff`; `tint_outer` c `#ff8a2a`; `highlight_shift`
  f 60 [0, 100] %; `core_white` f 20 [0, 100] %; `on_face` f 0 [0, 1].
- **Passes:** emit (soft-knee threshold that subtracts the threshold, × 2^exposure); 2 × 2 box
  pyramid; per octave (σ_min 1 canvas px, σ_max = radius/3, one octave apart, count =
  ⌈log2 σ_max⌉ + 1) blur on the coarsest level whose narrowest channel σ is ≥ 2 texels with the
  remaining width `sqrt(σ² − ((4^l − 1)/12 + 4^l/6))`, per-channel σ × (1 + k, 1, 1 − k), k = 0.2 ×
  chroma/100, outside-frame taps zero; accumulate octave × normalised weight σ^(2−p) × intensity,
  tinted from `tint_inner` to `tint_outer` by octave index; compose direct light + glow × (1 − (1 −
  on_face) × L.a) through the two-step over-exposure bleed (green takes half of red + blue, red and
  blue half of green, `core_white` swaps red and blue) and the shoulder `min(x, 1) + 0.5(1 −
  exp(−2 max(x − 1, 0)))`, written as `x + (y − x) k` so it is the identity where no channel
  exceeds 1. Defaults: 8 octaves, 31 passes, ≈ 16 FRE. `adds_light` true; `reach` = radius.
- **Cost:** C3 (radius ≥ 600: C4). **Seeds:** none. **8-bit:** F2.
- **Live:** `scale_split` on `intensity` (glow split × intensity(t)/intensity, then the response:
  1 pass over the layer bounds). **Mobile:** D11 cascaded accumulation to half res, ≤ 6 octaves,
  chroma off: C2.

#### 6.6.2 Contour Lines (`contour_lines`) · Glow · from `contourLines` (疑似導光板)
- **Params:** `mode` e{inside, outside, both} inside; `count` i 6 [1, 40]; `spacing` f 9 [2, 200]
  px; `width` f 2.5 [0.5, 40] px; `offset` f 4 [0, 200] px **L**; `falloff` f 0.6 [0, 1]; `gradient`
  g rainbow spectrum (`#ff2a2a` 0, `#ff9b00` 0.17, `#fff200` 0.33, `#2bff3a` 0.5, `#00c8ff` 0.67,
  `#3a3aff` 0.83, `#d22cff` 1); `color_by` e{across, line} across; `angle` f −45 deg (across);
  `intensity` f 1.2 [0, 6] **L**; `glow_size` f 12 [0, 200] px; `glow` f 0.8 [0, 4] **L**; `dim` f
  0 [0, 1]; `follow` e{layer, stack} layer.
- **Passes:** content SDF (shared); iso-lines at `offset + k × spacing` antialiased by width,
  weighted by `falloff^k`; glow = separable Gaussian at half res (2); compose. 4 passes, ≈ 2 FRE
  (Pachislop: 25 with its JFA and bounds). `outline` true, `adds_light` true.
- **Cost:** C2 (C3 when the SDF is rebuilt). **Seeds:** none. **8-bit:** F2.
- **Live:** `kernel` on `offset`, `intensity`, `glow`: lines + quarter-res glow over the bounds
  (≤ 1.5 FRE), the crawling 導光板 lines. **Mobile:** same.

#### 6.6.3 Motion Blur (`motion_blur`) · Blur · from `motionBlur`
- **Params:** `angle` f 0 deg; `distance` f 40 [0, 2000] px; `mode` e{centered, trail} centered.
- **Passes:** comb × box factorisation: a box of length D is a 32-tap comb at D/32 then a box of
  D/32, ≤ 64 taps per pass; 1 pass for D ≤ 64 render px, one more per ×32. **Cost:** C1 (D > 256
  px: C2). **Seeds:** none. **8-bit:** F1. **Live:** none. Gizmo: direction (angle, distance).

#### 6.6.4 Lens Blur (`lens_blur`) · Blur · from `lensBlur`
- **Params:** `radius` f 20 [0, 300] px; `shape` e{circle, hexagon, octagon} hexagon; `rotation` f
  0 [0, 360] deg; `brightness` f 15 [0, 100]; `threshold` f 235 [0, 255]; `noise` f 2 [0, 100];
  `distribution` e{uniform, gaussian} uniform; `mono` b true; `seed` i 1 [0, 999].
- **Passes:** highlight-weighted half-res downsample (bright points survive); iris gather on a
  Vogel spiral (≤ 512 taps at half res); compose with noise. **Cost:** C4. **Seeds:** noise.
  **8-bit:** F1. **Live:** none. **Mobile:** 64-tap quarter-res gather: C3.

#### 6.6.5 Tilt-Shift Blur (`tilt_shift`) · Blur · from `tiltShift`
- **Params:** `blur` f 16 [0, 200] px; `center` v2 [0, 0] px **L**; `angle` f 0 deg **L**; `width`
  f 160 [0, 4000] px **L**; `falloff` f 240 [0, 4000] px **L**.
- **Passes:** 4 Gaussian levels (8 passes on a pyramid) and a band blend (1). **Cost:** C2.
  **8-bit:** F1. **Live:** `kernel`: the band blend only, levels cached. Gizmos: point, direction.

#### 6.6.6 Radial Blur (`radial_blur`) · Blur · from `radialBlur`
- **Params:** `type` e{zoom, spin} zoom; `amount` f 0.25 [0, 0.95] **L**; `spin` f 12 [0, 180] deg
  **L**; `center` v2 [0, 0] px; `keep_original` b true.
- **Passes:** 2 (32 taps, then 13 taps with per-pixel jitter). **Cost:** C1. **8-bit:** F1.
- **Live:** `kernel` (both passes over the bounds; mobile at half res): impact moments.

#### 6.6.7 Extrude (`extrude`) · Shadow & Depth · from `extrude2d`
- **Params:** `angle` f 75 deg (the direction the stack recedes); `length` f 22 [1, 3000] px;
  `gradient` g `#d97a00` 0 → `#4a1600` 1 (front → back); `light_angle` f 120 (Photoshop
  convention); `use_global_light` b true (relation `from`); `shading` f 0.7 [0, 1]; `edge` f 0.55
  [0, 1]; `fade` f 0 [0, 1]; `opacity` f 1 [0, 1].
- **Passes:** normals from an alpha blur (2, σ = max(1.5, 3 × px_scale)); one march behind the face
  (≤ 1,024 steps of max(0.75, length/1024) px with early exit at coverage 0.996), shaded by the
  outward normal · light, a contact line at the face and the back. Default 29 steps.
- **Cost:** C2 (length > 200 px: C3; > 1,000 px: C4). **8-bit:** F0. **Live:** none. Gizmos:
  direction (angle, length), light.

#### 6.6.8 Motion Trail (`motion_trail`) · Distort · from `motionTrail`
- **Params:** `mode` e{smear, echo} smear; `angle` f 180 deg; `length` f 160 [1, 1500] px (smear);
  `fade` f 1.4 [0, 6]; `taper` f 0.25 [0, 0.95]; `copies` i 4 [1, 12] (echo); `offset` f 70 [1, 600]
  px **L** (echo); `scale_step` f 0 [−30, 30] %; `decay` f 0.4 [0, 0.95]; `echo_blur` f 0 [0, 40]
  px; `opacity` f 0.8 [0, 1] **L**; `tint` c `#ff7a00` (smear); `tint_start` c `#ff7a00`,
  `tint_end` c `#7a2cff` (echo); `tint_amount` f 0.5 [0, 1].
- **Passes:** smear: 1 max-march pass (≤ 1,024 taps, 214 at defaults) about the layer box centre;
  echo (残像): up to 3 pre-blurred levels plus 1 compose of `copies` × 4 taps. **Cost:** smear C3,
  echo C2. **8-bit:** F0. **Live:** echo `kernel` on `offset` and `opacity` (compose only).

#### 6.6.9 Halftone (`halftone`) · Stylize · from `halftone`
- **Params:** `size` f 9 [2, 120] px; `angle` f 45 deg; `dot` e{round, square, diamond, line,
  cross} round (new); `dot_scale` f 0.8 [0, 2] **L**; `color` c `#8a1c00`; `amount` f 0.6 [0, 1];
  `source` e{ramp, luminance, uniform, alpha} ramp; `fade` f 28 [0, 400] px **L** (alpha);
  `ramp_angle` f 90 deg (ramp); `mode` e{S80 blend modes} multiply.
- **Passes:** 1 (alpha source: + 1 blur, cached). AM screen; area-correct radius `sqrt(dens/π)`;
  dots touch at 70 %, then the gaps close; `alpha` = Halftone Dissolve toward soft alpha.
- **Cost:** C1. **8-bit:** F0. **Live:** `kernel` on `dot_scale`, `fade` (entrances).

#### 6.6.10 Perspective Shadow (`perspective_shadow`) · Shadow & Depth · from `perspectiveShadow`
- **Params:** `angle` f −55 [−180, 180] deg **L** (cast direction); `use_global_light` b true
  (relation `floor`); `length` f 230 [5, 2000] px **L**; `perspective` f 0.35 [0, 0.95]; `blur_near`
  f 2 [0, 100] px; `blur_far` f 22 [0, 300] px; `fade` f 0.75 [0, 1]; `offset` f 0 [−300, 300] px;
  `color` c `#000000`; `opacity` f 0.7 [0, 1] **L**.
- **Passes:** 2 blur levels (4) and a shear compose from the layer box's bottom (S80 CPU bounds):
  5 (Pachislop 14). **Cost:** C2. **8-bit:** F0. **Live:** `kernel` (compose only).

#### 6.6.11 Floor Reflection (`floor_reflection`) · Shadow & Depth · from `floorReflection`
- **Params:** `gap` f 4 [−100, 300] px; `length` f 150 [5, 2000] px; `opacity` f 0.45 [0, 1] **L**;
  `blur` f 14 [0, 200] px; `falloff` f 1.4 [0.3, 5]; `scale` f 1 [0.2, 2].
- **Passes:** 2 blur levels, 1 mirror compose. **Cost:** C2. **8-bit:** F0. **Live:** `scale_split`
  on `opacity`.

#### 6.6.12 Wave Warp (`wave_warp`) · Distort · from `wave`
- **Params:** `type` e{linear, radial, turbulent} linear (`turbulent` new); `amplitude` f 8 [0, 200]
  px **L**; `wavelength` f 140 [4, 3000] px; `phase` f 0 [0, 1] **L** (wraps); `angle` f 0 deg;
  `center` v2 [0, 0] px (radial); `complexity` i 3 [1, 6] (turbulent octaves); `evolution` f 0
  [0, 100] **L** (turbulent).
- **Passes:** 1 displaced lookup. **Cost:** C0 (turbulent C1). **8-bit:** F0. **Live:** `kernel`.

#### 6.6.13 Chromatic Aberration (`chromatic_aberration`) · Stylize · from `chromaticAberration`
- **Params:** `amount` f 6 [0, 200] px **L**; `type` e{radial, directional} radial; `angle` f 0
  deg (directional); `smooth` b true (triangular spectral smear, 3-31 taps by amount); `center` v2
  [0, 0] px (radial).
- **Passes:** 1. **Cost:** C0 (smooth: C1). **8-bit:** F0. **Live:** `kernel`.

#### 6.6.14 Grain (`grain`) · Color · from `grain`
- **Params:** `amount` f 0.14 [0, 1] **L**; `size` f 1.2 [0.3, 20] px; `color` b false; `seed` i 1
  [0, 999] **L** (stepped). Two-octave clumpy grain weighted to midtones, in canvas px.
- **Passes:** 1. **Cost:** C0. **8-bit:** F0. **Live:** `kernel`.

#### 6.6.15 Glitch (`glitch`) · Stylize · from `glitch`
- **Params:** `mode` e{slices, blocks} slices (`blocks` new: rectangular blocks of `block_size`);
  `amount` f 0.35 [0, 1] **L**; `slices` i 14 [1, 80]; `offset` f 28 [0, 600] px; `split` f 7 [0, 100]
  px; `block_size` f 40 [4, 400] px (blocks); `seed` i 1 [0, 999] **L** (stepped).
- **Passes:** 1. Slice heights are fractions of the layer box height; RGB from three shifted
  premultiplied copies, so lone copies show pure R, G or B. **Cost:** C0. **8-bit:** F0.
  **Live:** `kernel`.

#### 6.6.16 Material (`material`) · Texture & Material · from `material`
- **Params:** `color_mode` e{layer, fill} layer; `size` f 200 [10, 2000] px (the 100 % of the
  material's `%` members); `material` Material (S80 §6.1 Material (2D), every member).
- **Passes:** S80's material pipeline (SDF, cover, height, satin fields, shade) on the input's
  alpha, fields cached by the input's content key (Pachislop rebuilt them every run).
- **Cost:** C3. **8-bit:** F1. **Live:** none. Gizmo: light (bevel light, global light).
- **Addition to S80's Material:** `bevel.direction` e{up, down} up; `down` negates the height
  field (Deboss). Enum addition, no version bump.

#### 6.6.17 Shatter (`shatter`) · Distort · from `shatter`
- **Params:** `pattern` e{glass, voronoi, triangles} glass **X**; `pieces` i 60 [10, 400] **X**;
  `impact` v2 [0, 0] px **X**; `radius` f 300 [0, 4000] px **X**; `progress` f 1 [0, 1] **L** (new:
  0 assembled, 1 blown; scales push, side, rotation, fall and fade, the S85 Shatter In/Out field);
  `strength` f 80 [0, 1000] px **L**;
  `rotation` f 25 [0, 180] deg **L**; `fall` f 0 [0, 4000] px **L** (new: downward drift × f²);
  `fade` f 0 [0, 1] **L** (new: pieces fade with displacement); `gap` f 1 [0, 40] px **X**;
  `edge_color` c `#ffffff`; `edge_highlight` f 100 [0, 300] %; `lost` f 10 [0, 100] % **X**;
  `cracks_only` b false; `seed` i 1 [0, 999] **X**.
- **Passes:** CPU layout on a job (`analysis/ShatterLayout`): content grid (8 px), ink box of cells
  ≥ half covered, padded and snapped to 32 px; partition tiling the plane (glass: K =
  clamp(round(sqrt(0.9 n)), 5, 40) jittered spokes and M = n/K rings at radii ((j + jitter)/M)^1.5,
  25 % of ring cells split on a diagonal, wedge fan when the impact is outside the box; Voronoi:
  half-plane clipping nearest first with the 2 × reach early out; triangles: jittered grid,
  random diagonals); per piece 5 randoms drawn first, falloff f = smoothstep(1 − dist/radius),
  lost if r1 < lost × 2.5 × f, push, side, rotation, inset gap/2 with mitred corners, sorted by
  displacement. GPU: one mesh draw of centroid fans; per vertex `a_edge` is the centroid's distance
  to that fan triangle's cut edge at the centroid and 0 on the edge, and since distance to a line is
  affine over the triangle its interpolation is the exact distance to the cut edge; UVs at the rest
  position; coverage clamp(v + 0.5); highlight 0.8 e^(−v/W) + 0.3 e^(−v/4W) × a per-piece factor.
  The vertex shader applies strength, rotation, fall and fade from rest position, centre, push and
  spin attributes.
- **Cost:** C1 GPU, CPU ≤ 2-8 ms per layout (R36). **Seeds:** layout. **8-bit:** F0.
- **Live:** `kernel`: one mesh draw of the cached pre-shatter split per frame; layout only on X
  changes (a runtime text change re-runs it, R36). The jackpot sequence (cracks only, then
  `progress` 0 → 1 with strength 600, fall and fade) is the preset `blast`.

#### 6.6.18 Distress (`distress`) · Texture & Material · new
- **Params:** `pattern` e{grunge, dry_brush, rough_edges, scratches, speckle, spray} grunge;
  `amount` f 0.5 [0, 1]; `edge` f 12 [0, 400] px (band inward from the silhouette; 0 = whole
  layer); `scale` f 24 [1, 1000] px; `angle` f 0 deg (dry_brush, scratches); `contrast` f 0.6 [0,
  1]; `wear` f 0 [0, 1] (dirt darkening of what remains); `wear_color` c `#3a2a1a`; `outside` f 0
  [0, 1] (speckle and spray beyond the edge, within `edge` px); `seed` i 1 [0, 999].
- **Passes:** content SDF (shared) + 1 pass: alpha × keep-mask from multi-scale value noise
  (grunge), S80's erode field (dry_brush: the code `style.erode` uses, its
  96-step exit march), noise-displaced SDF threshold (rough_edges), seeded thin anisotropic lines
  (scratches), thresholded fine noise (speckle), droplet noise outside the edge (spray); analytic
  antialiasing through the field gradient.
- **Cost:** C1 (dry_brush C3). **8-bit:** F0. **Live:** none.

#### 6.6.19 Drips (`drips`) · Texture & Material · new
- **Params:** `style` e{paint, slime, blood, icicles} paint; `count` i 14 [0, 200] **X**; `length`
  f 90 [0, 2000] px **L**; `length_random` f 60 [0, 100] %; `width` f 14 [1, 200] px; `width_random`
  f 40 [0, 100] %; `head` f 1.4 [1, 3] × (rounded head radius / width; icicles: 0 = point);
  `angle` f 90 deg (gravity, 90 = down); `edge_bias` f 70 [0, 100] % (prefer edges facing
  gravity); `merge` f 6 [0, 60] px (smooth union with the shape); `color_mode` e{layer, custom}
  layer (layer: the ink colour above the drip, Pachislop's inward vivid walk); `color` c `#c8102e`;
  `gloss` f 0.6 [0, 1]; `light_angle` f 120; `use_global_light` b true; `seed` i 1 **X**.
- **Passes:** CPU: edge cells from the content grid (6 px, shared `EdgeExtractor`), drips seeded
  on cells weighted by normal · gravity^edge_bias; GPU: drips as capsule/cone instances
  (`QuadBatch`) writing −distance into a drip field with `MAX_PREMULTIPLIED` (min distance; RGBA8
  path: monotonic unorm encoding of distance, which MAX also orders), then one compose: d = smooth
  min(content SDF, drip field, merge), coverage, colour and a gloss streak from the field gradient.
  Styles set the head, gloss and colour (`slime` translucent with a light rim, `blood` darker
  head, `icicles` taper to a point, blue-white `#dff6ff`, upward-facing gloss).
- **Cost:** C2. **8-bit:** F1. **Live:** `kernel` on `length` (re-draw ≤ 200 instances + compose
  over the bounds): drips grow.

#### 6.6.20 Scanlines (`scanlines`) · Stylize · new
- **Params:** `style` e{lines, crt, interlace, lcd} lines; `spacing` f 4 [1, 64] px; `thickness` f
  0.5 [0.05, 0.95]; `intensity` f 0.45 [0, 1] **L**; `angle` f 0 [−90, 90] deg; `mask_strength` f
  0.5 [0, 1] (crt aperture-grille RGB stripes, lcd RGB sub-cells); `brightness` f 1.15 [0.5, 2]
  (compensates the darkening); `offset` f 0 px **L** (scroll); `flicker` f 0 [0, 1]; `seed` i 1
  **L** (stepped, flicker).
- **Passes:** 1. The line term is the HTML UI `scanlines.frag` formula (1 − intensity × (sin(2πy/
  spacing) × 0.5 + 0.5), stretched by `thickness`), on linear premultiplied colour.
- **Cost:** C0. **8-bit:** F0. **Live:** `kernel`.

#### 6.6.21 Pixelate (`pixelate`) · Stylize · new
- **Params:** `size` f 8 [1, 256] px **L** (cell size); `shape` e{square, round, diamond, led} square; `gap` f
  0 [0, 0.9]; `palette` asset null (an S43 `.palette`: each cell snaps to the nearest entry in
  OKLab); `levels` i 0 [0, 64] (no palette: posterize per channel; 0 = off); `dither` e{none,
  bayer2, bayer4, bayer8} none; `dither_amount` f 0.5 [0, 1]; `alpha_mode` e{hard, soft} hard;
  `alpha_threshold` f 0.5 [0, 1]; `align` e{canvas, layer} canvas.
- **Passes:** 1 coverage-weighted box downsample per cell, 1 shape/quantize upsample (palette ≤ 256
  entries as a 256 × 1 RGBA8 texture, nearest in OKLab via `jfx_color.glsl`, Bayer via
  `jfx_tone.glsl`). **Cost:** C1. **8-bit:** F0. **Live:** `kernel` on `size` (pixel transitions).

#### 6.6.22 Ink Brush Stroke (`brush_stroke`) · Stroke & Edge · new
- **Params:** `position` e{outside, center, inside} outside; `width` f 10 [0.5, 200] px;
  `pressure` f 0.6 [0, 1] (width variation along the contour); `bristles` f 0.5 [0, 1] (hair
  gaps); `dry` f 0.3 [0, 1]; `roughness` f 0.4 [0, 1]; `color` c `#111111`; `opacity` f 1 [0, 1];
  `seed` i 1.
- **Passes:** content SDF + its blurred normals; 1 pass: the nearest edge point q = p − n·d is
  constant across the stroke, so noise sampled at q (along the contour) gives pressure, and
  noise sampled at (q along the tangent, d across) stretched along the tangent gives bristles and
  dry gaps (Pachislop Corona's trick); ragged edge by noise on the band edges. **Cost:** C1.
  **8-bit:** F0. **Live:** none.

#### 6.6.23 Frost (`frost`) · Texture & Material · new
- **Params:** `amount` f 0.7 [0, 1] **L**; `reach` f 18 [0, 400] px (inward from the edge);
  `outside` f 6 [0, 200] px (rime crystals outward); `crystal_size` f 10 [1, 200] px; `color` c
  `#dff6ff`; `tint` c `#7fc8ff`; `tint_amount` f 0.35 [0, 1]; `sparkle` f 0.4 [0, 1] (facet glints
  lit by the global light); `seed` i 1.
- **Passes:** content SDF + 1 pass: Worley F1/F2 crystals masked by the SDF band and `amount`
  (frost grows from the edge as amount rises), facet normals from F2 − F1 for sparkle.
  **Cost:** C1. **8-bit:** F1. **Live:** `kernel` on `amount`.

#### 6.6.24 Neon Tube (`neon_tube`) · Glow · new
- **Params:** `source` e{outline, fill} outline (fill: the layer's own thin shape is the tube);
  `width` f 8 [1, 100] px; `offset` f 0 [−100, 100] px (tube centre from the edge, + outside);
  `color` c `#ff2bd6`; `core_color` c `#ffffff`; `core` f 0.6 [0, 1] **L**; `glow_size` f 40 [0,
  400] px; `glow` f 1 [0, 4] **L**; `gaps` f 0 [0, 1] (broken-tube gaps along the contour);
  `flicker` f 0 [0, 1]; `light_angle` f 120; `use_global_light` b true; `seed` i 1 **L** (stepped:
  flicker).
- **Passes:** content SDF; tube pass (cylindrical normal across the band, specular from the light,
  white-hot core above 1, gaps and flicker by along-contour noise at q); glow = Light Glow's kernel
  at falloff 2.4 on the tube (`tfx_light_glow.glsl`, ≤ 7 octaves at defaults); compose.
  `adds_light` true. **Cost:** C2 (C3 with the SDF rebuilt). **8-bit:** F2.
- **Live:** `scale_split` on `glow` and `core`; flicker per frame as a `kernel` (tube pass over the
  bounds, glow scaled by the tube's mean brightness). The HTML UI `neon-border.frag` keeps its own
  code (D9).

#### 6.6.25 Chalk (`chalk`) · Texture & Material · new
- **Params:** `grain` f 3 [0.5, 40] px; `coverage` f 0.7 [0, 1]; `stroke_angle` f −30 deg;
  `stroke_length` f 40 [1, 400] px; `edge_rough` f 0.5 [0, 1]; `edge_width` f 4 [0, 50] px; `dust`
  f 0.3 [0, 1]; `dust_reach` f 12 [0, 200] px; `color_mode` e{layer, custom} layer; `color` c
  `#f4f1e8`; `seed` i 1.
- **Passes:** content SDF + 1 pass: anisotropic noise along the strokes × paper grain sets the
  coverage, rough edge from SDF + noise, dust specks outside. **Cost:** C1. **8-bit:** F0.
  **Live:** none.

#### 6.6.26 Stitch (`stitch`) · Stroke & Edge · new
- **Params:** `style` e{running, cross, zigzag, satin_border} running; `offset` f −6 [−200, 200] px
  **X** (contour inset, negative inside); `stitch_length` f 12 [2, 100] px **X**; `gap` f 6 [0,
  100] px **X**; `thread_width` f 3 [0.5, 20] px; `color` c `#ffffff`; `shade` f 0.5 [0, 1];
  `holes` f 0.4 [0, 1]; `fill` e{none, satin, tatami} none; `fill_angle` f 45 deg; `fill_density`
  f 3 [1, 20] px; `light_angle` f 120; `use_global_light` b true; `seed` i 1 **X** (jitter).
- **Passes:** CPU (`analysis/ContourTracer`): marching squares on the content SDF sampled at
  `offset` from the content grid, arc-length resampling into stitches; GPU: one `QuadBatch` of
  thread capsules shaded across (highlight and shadow by the light), needle-hole darkening at the
  ends; optional fill pass (satin: parallel thread lines at `fill_angle`; tatami: brick-offset
  short stitches) modulating the layer colour. **Cost:** C2, CPU ≤ 2 ms. **8-bit:** F0.
  **Live:** none.

#### 6.6.27 Holographic Foil (`holographic`) · Color · new
- **Params:** `amount` f 1 [0, 1]; `tilt` v2 [0, 0] [−1, 1] **L** (simulated view tilt: the foil
  shimmer); `film_thickness` f 450 [100, 1200] nm; `film_ior` f 1.5 [1, 2.5]; `relief` f 0.5 [0, 1]
  (SDF bevel normal and noise shift the angle); `pattern` e{none, diffraction, glitter, stars}
  glitter; `pattern_scale` f 6 [1, 100] px; `mode` e{S80 blend modes} overlay; `seed` i 1.
- **Passes:** 1 (+ content SDF when relief > 0): view angle from tilt and the relief normal,
  colour from `iridescenceFresnel` (`engine/shaderlib/includes/iridescence.glsl`), modulated by the
  micro pattern. **Cost:** C1. **8-bit:** F0. **Live:** `kernel` on `tilt`.

#### 6.6.28 Procedural material kinds (pattern library additions)
Added to S80's `pattern.kind` enum and the shared pattern library, so Pattern Overlay, the
Material `pattern` member and Pattern Fill all get them. Each returns S80's `(d, d2, w)` triple
(mark distance, secondary distance, relief width), so `shade` and `relief` work unchanged; `color`
is the mark, `color2` the secondary tone:

| Kind | Technique |
|---|---|
| `wood` | rings of noise-distorted distance from an axis plus fine grain streaks |
| `marble` | domain-warped fbm veins, `abs(sin)` sharpened |
| `granite` | three-scale cellular speckle |
| `concrete` | low-frequency fbm with thresholded pores |
| `brushed_metal` | one-axis stretched noise lines (relief adds the anisotropic sheen) |
| `diamond_plate` | raised diamonds on a grid, with relief |
| `carbon_fiber` | 2 × 2 twill weave |
| `rust` | fbm patches with pitting at their edges |
| `leather` | Worley cells with creases |
| `brick` | running bond with mortar joints (`color2` = mortar) |
| `camo` | three-level thresholded fbm (woodland) |
| `circuit` | per-cell hashed traces and pads |

**Cost:** C1 each inside their host. **8-bit:** F0. Golden: one Pattern Fill render per kind.

#### 6.6.29 かすれ Dry Brush (style setting)
S80's `style.erode` (Text, Shape, Vector, Image layers). S83 adds the Add Effect entry "Dry Brush
(かすれ)" (Texture & Material, marked "Style setting") that sets `style.erode.enabled = true` and
opens the Style section, and registers its search aliases. Any other layer type (Group, Generator)
uses Distress with the `dry_brush` pattern, which shares the shader include.

#### 6.6.30 Glint (3D highlights) (style setting on Dimension 3D layers)
Port of `src/engine/three/glint.ts`, stored where S86 puts the 3D style (expected
`style_3d.glint`), implemented by S83 in `engine/core/titleart/three_d/Glint3D.cpp` (compiled with S81's
`JASPERENGINE_TITLE_ART_3D` sub-option) and `engine/shaderlib/titleart/fx/glint3d.glsl` and called by S86 on the 3D layer's linear HDR target before tone mapping.
- **Params:** `enabled` b false; `threshold` f 1 [0, 16] (linear HDR luminance); `intensity` f 1 [0,
  4] **L**; `radius` f 6 [0, 100] % of size; `streaks` e{0, 2, 4, 6, 8} 4; `length` f 40 [0, 400] %
  of size; `angle` f 0 deg; `tint` c `#fff4dc`.
- **Passes** on a grid locked to canvas px (level B = round(log2 px_scale)): bright pass e²/(e + 4);
  box mips; coverage "tame" K/(K + coverage); two-scale bloom; point detector (bright − max of its
  1D blurs); pinch; density budget; hot core; Kawase streaks per direction (4 taps per pass at
  stride 4^k, one-sided, `fall = 0.02^(1/n)`), long thin plus short wide rays; sum × tint. ≈ 66
  passes. **Cost:** C4. **8-bit:** the bright pass decodes S86's compressed-light main target
  (`s = a·C/(1 + max C)`, S86 R30) when the 3D target is SRGB8A8, so the chain runs without float
  targets; F2 against the float path. **Live:** `scale_split` on `intensity`. **Mobile:** no density
  budget, 2 directions, half res: C3 (S86 R32 "Medium"; S86's sprite glints are its Low tier).

### 6.7 Generator catalogue

Positions (`center`, `position`, `origin`, `impact`, `point`) are Vec2 canvas px, +y up (S80);
with `follow` set they are offsets from the followed layer's centre (Pachislop `drawFollowed`).

#### 6.7.1 Particles (`particles`) · Particles · follow optional · from `particles`
- **Emitter:** `emitter` e{ellipse, rect, ring, line, point, full, layer} ellipse **X** (`layer`
  needs `follow`); `source` e{fill, edge} fill **X** (layer); `edge_width` f 4 [1, 100] px **X**;
  `direction` e{normal, point, angle} normal (layer); `point` v2 [0, 0] px; `heading` f −90 [−180,
  180] deg; `radius` v2 [450, 300] [0, 4000] px **X**; `distribution` e{scatter, burst, stream}
  burst **X**; `falloff` f 60 [0, 100] % **X**; `travel` f 200 [0, 4000] px **L**;
  `distance_random` f 100 [0, 100] %; `angle` f 90 deg (scatter, stream; 90 = down); `cone` f 20
  [0, 360] deg; `split` e{all, front, back} all (front/back: the half above or below the followed
  layer by a stable per-particle random, for a pair of layers).
- **Look:** `shape` e{dot, glint4, glint6, ember, square, confetti, petal, star, heart, bokeh, rain}
  dot; `glint_mix` f 0 [0, 100] %; `count` i 400 [1, 5000] **X**; `size` f 6 [0.5, 200] px;
  `size_random` f 60 %; `depth` f 50 %; `streak` f 40 [0, 400] px; `ring` f 30 % (bokeh);
  `rotation_random` f 20 [0, 180] deg; `tumble` f 0 %; `sheen` f 50 % (confetti foil); `color_mode`
  e{solid, radial, palette, sample} radial; `color` c `#ffd166`; `gradient` g `#ffffff` →
  `#ffd166`; `opacity` f 100 %; `opacity_random` f 40 %; `seed` i 1 **X**.
- **Animation (new, closed form):** `time` f 1 [0, 1] **L** (1 = Pachislop's still: travel
  fraction tf = r^(0.6 + 1.6 × falloff) × ease_out(time)); `loop` b false (each particle's time =
  fract(time + phase_i): a continuous stream); `gravity` f 0 [−2000, 2000] px **L** (adds gravity ×
  time² down); `spin` f 0 [−1080, 1080] deg **L**; `twinkle` f 0 [0, 1] **L** (glint shapes flash
  with a per-particle phase); `fade_in` f 0 [0, 1], `fade_out` f 0 [0, 1] (alpha ramps over each
  particle's time).
- **Passes:** CPU builds ≤ 5,000 instance records on **X** changes: depth z = lerp(z0^1.7, t,
  0.5 d), size × (1 + d(2.4z − 0.7)), brightness × (1 + d(0.8z − 0.35)), defocus about the focal
  plane z = 0.6 (energy-conserving pad), shape and colour; the layer emitter reads the content grid
  (fill or edge-band map, normals from smoothed alpha, colour from the most vivid of 8 cells
  inward) with rejection sampling keyed by try index. GPU: the vertex shader evaluates position
  from `time`, `travel`, `gravity`, `spin`; 2 `QuadBatch` draws (solid shapes far → near, normal
  blend; light shapes `LIGHT_PREMULTIPLIED`, glints last); shapes from `tfx_particle_shapes.glsl`
  (star and heart SDFs after Inigo Quilez, D15).
- **Cost:** C1 GPU (gold dust 2,600 particles: 1 FRE + instances), CPU ≤ 2 ms. **8-bit:** F2.
- **Live:** `kernel`: the draws with new uniforms. **Mobile:** `count` capped at 2,000 (the first
  2,000, a subset by R25).

#### 6.7.2 Ink Splatter (`ink_splatter`) · Particles · follow required · from `inkSplatter`
- **Params:** `amount` f 45 [0, 100] %; `spread` f 150 [0, 1500] px; `size` f 22 [1, 300] px;
  `satellites` f 60 [0, 200] %; `streaks` f 35 [0, 100] %; `streak_length` f 110 [0, 1500] px;
  `direction` e{radial, directional} radial; `angle` f 10 deg (directional); `color` c `#0b0907`;
  `growth` f 1 [0, 1] **L** (new: drops scale up from 0); `seed` i 1 **X**.
- **Passes:** content grid (8 px) and chamfer distance; CPU rejection sampling (density falls with
  distance from the silhouette, drag streaks pile their bulk at the far end); 1 `QuadBatch` of
  round-cone SDF drops, ragged by noise at the drop's scale, AA by the true slope. **Cost:** C1,
  CPU ≤ 2 ms. **8-bit:** F0. **Live:** `kernel` on `growth`.

#### 6.7.3 Speed Lines (`speed_lines`) · Lines · follow optional · from `speedLines`
- **Params:** `type` e{focus, flow, flash, beta} focus **X**; `center` v2 [0, 0] px **X**; `shape`
  e{ellipse, rect, layer} ellipse **X** (`layer` = the followed layer's silhouette); `padding` f 40
  [0, 1000] px **X**; `radius` v2 [420, 260] [0, 4000] px **X**; `angle` f 0 deg **X** (flow); `side`
  e{both, front, back} both; `count` i 160 [4, 600] **X**; `width` f 10 [0.2, 200] px; `width_random`
  f 60 %; `length` f 100 [1, 300] % **X**; `length_random` f 0 % **X**; `jitter` f 60 [0, 1000] px **X**;
  `jag_count` i 0 [0, 64]; `jag_height` f 40 [0, 1000] px **X**; `clump` i 1 [1, 20]; `gap` f 0 [0,
  10] **X**; `taper_in` f 100 %; `taper_out` f 0 %; `curve` f 0 [−100, 100] % **X**; `color_mode`
  e{solid, length, around} solid; `color` c `#ffffff`; `gradient` g `#ffffff` → `#ffc83a`;
  `opacity` f 85 %; `seed` i 1 **L** (stepped; re-runs the layout, allowed live).
- **Passes:** CPU (`analysis/SpeedLineLayout`): the 1,024-angle focus table (ellipse and rectangle
  analytic; layer: alpha marched on a 6 px grid taking the last exit, ±2-sample running max) and
  frame exits; stratified slots u = (i + gaps + 0.15 + 0.7 r)/units; clumps sharing a length
  component; jag teeth; frame-anchored lengths; shared curvature split per 2° (≤ 32 segments, no
  overlap at inner joints); a fixed number of draws per line so toggles keep other lines in place.
  GPU: 1 `QuadBatch` with wedge-distance AA (cov = clamp(max(hw, 0.5) − x + 0.5) × min(1, 2hw)); beta:
  lines to a mask, max(lines, hole), fill outside (3 passes; the Beta preset adds a Halftone
  dissolve). **Cost:** C1, CPU ≤ 1.5 ms. **8-bit:** F0.
- **Live:** `kernel`: reseed (layout + draw); the guide recommends a reseed every 2-3 frames.

#### 6.7.4 Light Streaks (`light_streaks`) · Lines · follow none · from `lightStreaks`
- **Params:** `mode` e{anamorphic, bundle, spears} anamorphic; `origin` v2 [0, 0] px **L**; `angle`
  f 0 deg **L**; `spread` f 3 [0, 360] deg; `count` i 6 [1, 200] **X**; `length` f 1600 [10, 6000] px
  **L**; `length_random` f 55 %; `thickness` f 16 [0.5, 200] px; `thickness_random` f 60 %;
  `taper_head` f 100 %; `taper_tail` f 100 %; `core_color` c `#ffffff`; `edge_color` c `#3d8bff`;
  `core_width` f 25 %; `brightness` f 250 [0, 400] % **L**; `glow` f 140 [0, 400] px; `chroma` f 0
  [0, 40] px; `seed` i 1 **X**.
- **Passes:** 2 `QuadBatch` draws (the core; the halo source with core weight 0, so the halo is
  blurred from edge colour alone), halos at ×0.25 radius (gain 1.5) and ×1 (gain 1.2), add ≈ 14
  passes; per-channel profile offset for `chroma`. **Cost:** C2. **8-bit:** F2.
- **Live:** `kernel` (quads + halos at quarter res on mobile).

#### 6.7.5 Shockwave (`shockwave`) · Rings · follow none · from `shockwave`
- **Params:** `center` v2 [0, 0] px; `radius` f 380 [10, 3000] px **L**; `rings` i 2 [1, 8];
  `spacing` f 60 [0, 1000] px **L**; `width` f 24 [1, 600] px **L**; `profile` e{hard, soft, sine,
  double, fibrous} soft; `soft_in` f 30 [0, 800] px; `soft_out` f 6 [0, 800] px; `squash` f 100 [5,
  100] %; `tilt` f 0 [−90, 90] deg; `breakup` f 25 %; `breakup_scale` f 40 [2, 1000] px; `smear` f
  0 %; `split` f 0 [0, 60] px; `flash` f 15 [0, 200] % **L**; `inner` c `#ffffff`; `outer` c
  `#d8ecff`; `seed` i 1.
- **Passes:** 1 bounding-box quad; band profiles with cubic soft tails, chromatic split at r ±
  split, noise sampled on the circle, breakup stretched along the ring, radial smear, core flash,
  trailing rings × 0.72^i. **Cost:** C1. **8-bit:** F1. **Live:** `kernel`.

#### 6.7.6 Magic Circle (`magic_circle`) · Rings · follow none · from `magicCircle`
- **Params:** `center` v2 [0, 0] px; `radius` f 320 [10, 3000] px; `rotation` f 0 deg **L**; `tilt`
  f 0 [0, 80] deg; `color` g gold (centre → edge); `fill` f 6 [0, 100] %; `glyphs` e{runes, kuji,
  zodiac, trigrams, hud, custom} runes; `custom_glyphs` str ""; `font` asset null (null = Zen Old
  Mincho Black for runes, kuji, zodiac and trigrams, Zen Kaku Gothic New Black for hud, both OFL
  from google/fonts, copied into the project on first use as S80's library fonts are); `rows`
  array of ≤ 6 `{type: e{ring, dashed, ticks, text, polygon, star2, star3, chevrons, arcs, hex},
  radius: f [0, 150] %, size: f [0.5, 600] px, count: i [1, 360], angle: f deg L}` (defaults:
  Pachislop's six `ROW_DEFAULTS`); `seed` i 1.
- **Passes:** a glyph atlas once per (set, font, size bucket) through S80's text core; 1 bounding
  quad of polar distance-field marks, AA from the Jacobian of the tilted plane, ≤ 32 chords per row;
  majors every 5, 4 or 3 ticks when the count divides. **Cost:** C2. **8-bit:** F0.
- **Live:** `kernel` on `rotation` and row angles (rows turn at their own speeds).

#### 6.7.7 Brush Swirl (`brush_swirl`) · Rings · follow none · from `brushSwirl`
- **Params:** `center` v2 [0, 0] px; `path` e{arc, spiral, slash, ring} arc **X**; `radius` f 380
  [10, 3000] px **X**; `sweep` f 300 [10, 1080] deg **X**; `angle` f −150 deg **X** (start);
  `strokes` i 4 [1, 12] **X**; `spacing` f 26 [0, 400] px **X**; `width` f 70 [1, 600] px;
  `taper_in` f 15 %; `taper_out` f 85 %; `dry` f 45 %; `splatter` f 30 %; `color` c `#0b0907`;
  `reveal` f 1 [0, 1] **L** (new: drawn up to u = reveal); `seed` i 1 **X**.
- **Passes:** ribbon mesh with stroke coordinates (cached by key), bristle noise and a dry threshold
  rising toward the tail and rims; 1 `QuadBatch` of splatter. **Cost:** C2. **8-bit:** F0.
  **Live:** `kernel` on `reveal` (a brush stroke drawing itself).

#### 6.7.8 Glints (`glints`) · Light · follow required · from `glints`
- **Params:** `placement` e{highlights, peaks, hero, scatter, point} highlights **X**; `large` i 5
  [0, 20] **X**; `small` i 6 [0, 40] **X**; `stars` i 5 [1, 40] **X** (peaks); `count` i 40 [1, 400]
  **X** (scatter); `hero_width` f 80 [20, 150] %; `hero_thickness` f 100 [20, 400] %; `hero_cross` f
  22 %; `hero_angle` f 0 [−90, 90] deg; `hero_stars` i 3 [0, 8] **X**; `position` v2 [0, 0] px
  (point); `size` f 50 [2, 800] px; `size_variation` f 60 %; `threshold` f 30 % **X**; `spacing` f 80
  [0, 600] px **X**; `face_bias` f 70 % **X**; `jitter` f 50 %; `spread` f 60 [0, 600] px **X**;
  `inside` b true; `rays` e{4, snow, 8} 4; `spectral` b false; `thickness` f 100 [20, 400] %;
  `color` c `#fff4c0`; `intensity` f 130 [0, 400] % **L**; `rotation` f 0 deg **L**;
  `brightness_variation` f 50 %; `seed` i 1 **X**; new: `twinkle` f 0 [0, 1] **L**,
  `twinkle_phase` f 0 [0, 1] **L** (wraps), `twinkle_rotate` f 0 [0, 45] deg **L**.
- **Analysis** (`analysis/HighlightFinder`, on the styled followed layer, 4 px cells): luminance
  smoothed by [1 2 1]/4 four times per axis; local background as a ±6-cell box mean (integral
  image); 8-neighbour non-maximum suppression on cells with alpha ≥ 0.5, strict ties on one half;
  parabola sub-cell position; score = ((c − bg + 0.15 c)/ref) × w_depth × w_ridge with ref blending
  the global and the regional (≈ 128 px) maximum (0.7 for sparkles), w_depth from the chamfer depth
  by `face_bias`, w_ridge = 1 − ridge × (1 − sqrt(|λweak|/|λstrong|)) from the Hessian at 2 cells.
  Picking: greedy by score with min distance max(min, spacing × (r1 + r2)); the large tier spaced by
  0.7 × ink diagonal / n_large; small ones avoid large; near-ties blend, the last picks fade
  against the runner-up; size falls with score. Hero: the strongest peak in the central band, an
  anamorphic streak with its taper term. Anchors (internally `{x, y, radius, score, phase}`) are
  cached (R14) and shipped in S80's sidecar shape `{x, y, size, strength, fade}` (frame-normalised;
  `size` = radius, `strength` = score over the strongest pick, `fade` = the pick's fade against the
  runner-up; the phase is re-derived from `glintSeed` and the index, S81 R37), at most 16.
- **Star** (`tfx_glint_star.glsl`): hot core 4 exp(−r²/rc²); bloom 0.8 exp(−45 r²) + 0.35
  exp(−5 r); rays with quadratic taper w = max(thin (0.12 + 0.88 u²), w_min), footprint we =
  sqrt(w² + 0.25 px²), spike exp(−pe/we) (w/we), sheath, along falloff (2 e^(−5 al) + 1.3 e^(−1.5 al))
  sqrt(u); ray sets 4 / snow / 8; white core to saturated tips; spectral variant; thin ≥ 0.6 px.
- **Passes:** analysis (CPU, once per content key) + 1 `QuadBatch` (≤ 60 stars, light blend).
  **Cost:** C1, CPU ≤ 3 ms. **8-bit:** F2.
- **Live:** `kernel`: stars with intensity × (1 − twinkle × (0.5 − 0.5 cos 2π(twinkle_phase +
  phase_i)))² and a rotation jitter; anchors from the bake, no readback.

#### 6.7.9 Lens Flare (`lens_flare`) · Light · follow optional · from `lensFlare`
- **Params:** `position` v2 [−380, 120] px **L**; `size` f 70 [4, 1000] px; `streak` f 700 [0, 4000]
  px; `rotation` f 0 deg; `rays` f 60 [0, 200] %; `ghosts` b true; `color` c `#7cc4ff`; `intensity`
  f 100 [0, 400] % **L**; `seed` i 1.
- **Passes:** 1 additive full-canvas pass: core and Lorentzian bloom, cusped anamorphic streak,
  starburst from noise sampled on a circle, chromatic halo ring (radii 2.35/2.25/2.15), 6 ghosts on
  the axis through the canvas centre. **Cost:** C1. **8-bit:** F2. **Live:** `kernel`.
  **Mobile:** core, streak and ghosts as 8 additive quads, no full-canvas pass: C0.

#### 6.7.10 Backlight (`backlight`) · Light · follow required · from `backlight`
- **Params:** `center_mode` e{shape, manual} shape; `center` v2 [0, 0] px (manual); `length` f 260
  [10, 2000] px; `intensity` f 240 [0, 600] % **L**; `color` c `#fff6d0`; `outer_color` c `#ffa318`;
  `rays` i 36 [2, 120]; `noise` f 90 %; `rotation` f 0 deg **L**; `seed` i 1.
- **Passes:** at half res: source = content alpha × noise on a circle; radial blur over `length`
  (40 taps); a second over length/40 (40 taps): 1,600 effective taps; colour mix(outer, inner, r) ×
  r. 3 passes (Pachislop 10). **Cost:** C2. **8-bit:** F2. **Live:** `kernel` (quarter res on mobile).

#### 6.7.11 Fire (`fire`) · Energy · follow required · from `fire`
- **Params:** `over_edge` b false; `height` f 150 [5, 600] px; `width` f 22 [3, 200] px;
  `turbulence` f 60 [0, 150] %; `angle` f −90 deg (up); `spread` f 35 % (side burn); `inside` f 12
  [0, 100] px (over edge); `phase` f 0 [0, 10] **L** (Evolution); `intensity` f 110 [0, 300] %;
  `gradient` g `#fffbe0` 0, `#ffd000` 0.3, `#ff4a00` 0.65, `#7a0000` 1 (core → tips); `seed` i 1.
- **Passes:** content SDF; 1 pass over the layer box + height: sway fbm, upstream distance E =
  min over 40 steps of max(sd(p − dir × st), 0) × side + st (side = 1/max(0.08, spread)), tongue
  noise narrow across and stretched along the flow, per-tongue length, the f(1 − f) flicker,
  colour at 1 − f with a white-hot core × (1 + 0.6 smoothstep(0.8, 1, f)), over-edge licking.
- **Cost:** C3 (40 SDF taps + 12 value noises). **8-bit:** F2. **Live:** `kernel` on `phase`.
  **Mobile:** E baked to a half-res field while `angle`, `height`, `spread` are unchanged, noise
  from a 256² tiling RG noise texture (3 fetches), half res: C1 per frame.

#### 6.7.12 Smoke (`smoke`) · Energy · follow required · from `smoke`
- **Params:** `height` f 220 [10, 800] px; `width` f 40 [5, 300] px (billow size); `angle` f −90
  deg; `spread` f 35 %; `softness` f 60 [5, 100] %; `color` c `#d8d2e6`; `shadow_color` c `#4a4458`;
  `phase` f 0 [0, 10] **L**; `seed` i 1.
- **Passes:** content SDF; 1 pass: soft 3-octave noise, domain warp, plume, upstream E with the warp,
  density, two-tone shading from the same noise. **Cost:** C3. **8-bit:** F0. **Live:** `kernel`.
  **Mobile:** as Fire, half res.

#### 6.7.13 Aura (`aura`) · Energy · follow required · from `corona`
- **Params:** `mode` e{tendrils, rising} tendrils (Pachislop `aura` mode = `rising`); `length` f 120
  [10, 600] px; `wispiness` f 60 [0, 200] %; `density` f 100 [20, 400] %; `strength` e{strong, weak}
  strong (rising); `flow` e{outward, up} outward (rising); `tongue` f 90 [5, 600] px;
  `turbulence_scale` f 60 [5, 400] px; `breakup` f 50 %; `glow` f 60 [0, 200] %; `intensity` f 140
  [0, 400] %; `gradient` g blue corona; `phase` f 0 [0, 10] **L**; `seed` i 1.
- **Passes:** content SDF and a blurred SDF for normals continuous across letter creases; tendrils:
  noise at the nearest edge point q = p − n d gives radial streaks, warp growing with distance,
  ridged noise to powers 6 and 14, outward pulses; rising: the upstream field. 3 passes.
  **Cost:** C3. **8-bit:** F2. **Live:** `kernel`. **Mobile:** half res, 3-octave fbm: C2.

#### 6.7.14 Lightning (`lightning`) · Energy · follow required · from `lightning`
- **Params:** `mode` e{mixed, outward, crawl, edge} mixed **X**; `count` i 12 [1, 40] **X**; `arcs`
  i 24 [1, 60] **X** (edge); `length` f 240 [10, 800] px **X**; `jaggedness` f 55 [0, 150] % **X**;
  `branches` f 60 % **X**; `wrinkle` f 60 [0, 150] % **X**; `branchiness` f 40 % **X**; `core_width`
  f 3.5 [0.5, 12] px; `glow_size` f 18 [0, 80] px; `glow_width` f 16 [0, 80] px (edge); `glow` f 100
  [0, 300] % **L**; `color` c `#4fd8ff`; `intensity` f 100 [0, 300] % **L**; `seed` i 1 **L**
  (stepped).
- **Passes:** edges from the content grid (6 px, Sobel normals, hash grid of cells), cached; CPU
  (`analysis/BoltBuilder`): midpoint displacement depth 6, amplitude jag × length × 0.35 × 0.55^level,
  outward bulge, width taper 1 − 0.5 i/n, branches (±0.35-0.85 rad, length 0.25-0.55, width × 0.55,
  intensity × 0.7, depth − 2), outward and crawl bolts from cells picked by position, Electrify edge
  walks; GPU: segment quads across ±1.6 with profile exp(−2.2 a²), extended half a width past their
  ends, `MAX_PREMULTIPLIED` into a core target; 2 Gaussian glows (σ 0.35 and 1.6 × glow size) at
  half res; compose core × mix(colour, white, 0.8) × 1.6 + (5 g1 + 4 g2) × colour × glow. ≈ 8
  passes. **Cost:** C2, CPU ≤ 2 ms. **8-bit:** F1 (MAX works on unorm). **Live:** `kernel`: reseed
  on cached edges + draws.

#### 6.7.15 Sunburst (`sunburst`) · Lines · follow optional · from `sunburst`
- **Params:** `style` e{sunburst, wedges, focus, rings} sunburst; `center_mode` e{layer, point}
  layer (the followed layer's centre, or the canvas centre without one); `on_layer` e{all, clip,
  over} all (with `follow`: rays everywhere, only on its pixels, or on them and around); `center` v2
  [0, 0] px (point); `rays` i 24 [2, 300]; `rotation` f 0 deg **L**; `width` f 50 [2, 100] %;
  `width_variation` f 40 % (wedges, focus, rings); `length_variation` f 60 % (focus, rings);
  `twist` f 0 [−360, 360] deg **L** (per 1,000 px of radius; sunburst); `inner` f 140 [0, 4000] px;
  `softness` f 10 % (sunburst); `inner_softness` f 60 % (bursts); `color_mode` e{ab, radial} ab
  (sunburst); `color_a` c `#ffc93c`; `alternate` b false (bursts); `color_b` c `#c2121c`; `gradient`
  g `#fff3b0` 0, `#e3a82a` 0.45, `#7a4a0a` 1 (radial); `glow` f 70 [0, 300] % (sunburst); `opacity`
  f 70 % (bursts); `seed` i 1 (bursts).
- **Passes:** 1 full-canvas pass: θ = atan(d) − rotation + twist × r; the ray period in px shortened
  by sqrt(1 + r² twist²) so antialiasing uses the true perpendicular distance; edge width mix(1 px,
  half a period, softness); moiré control cov = mix(0.5, cov, clamp(0.5 × period − 1)) so the hub
  never moirés; rays merge into a solid inner disc; centre glow mix(ray, white, 0.6) exp(−2r²/gr²).
  Bursts: per-sector hashed widths and centres, focus needles from random radii, speed-ring arcs;
  ray opacity computed in sRGB (Photoshop-like), energy above 1 kept. Opaque by design: it is a
  background; presets add the Vignette recipe. **Cost:** C1. **8-bit:** F0.
- **Live:** `kernel` on `rotation` and `twist` (the jackpot background). The HTML UI
  `sunburst.frag` keeps its own code (D9).

#### 6.7.16 Fractal Noise (`fractal_noise`) · Fill & Pattern · follow none · new
- **Params:** `noise` e{fbm, turbulent, ridged, cells} fbm; `scale` f 240 [4, 4000] px; `aspect` f
  1 [0.1, 10]; `complexity` i 5 [1, 8]; `contrast` f 1 [0, 4]; `brightness` f 0 [−1, 1];
  `evolution` f 0 [0, 100] **L**; `offset` v2 [0, 0] px **L**; `rotation` f 0 deg; `gradient` g
  `#000000` → `#ffffff`; `seed` i 1.
- **Passes:** 1 full-canvas pass (octaves of `tfx_noise` value noise; cells = Worley F2 − F1),
  coloured through the gradient LUT. **Cost:** C1 (complexity > 5: C2). **8-bit:** F0.
  **Live:** `kernel`. **Mobile:** complexity ≤ 4.

#### 6.7.17 Pattern Fill and Fill (S80)
S80 ships `pattern_fill` (Pachislop's `patternFill`) and `fill`. S83 adds to Pattern Fill the
twelve kinds of §6.6.28, `offset` v2 [0, 0] px **L** (scroll), a `kernel` live variant on `offset`
and `rotation` (one full-canvas pass; C1, cellular kinds C2), and the presets of §6.8 (Pachislop's
nine when S80 ships the type without them, plus six new). Fill is unchanged.

### 6.8 Presets

Preset files hold Pachislop's values converted by §6.9 (generators: its `GeneratorPreset` params,
blend, opacity, place and effects; Pachislop's glows become our Light Glow or S80 Outer Glow
entries as its preset effects were). Japanese labels move into the alias list and the name keeps
the term in parentheses. New presets are tuned by eye against the goldens and reviewed with
screenshots (AC2).

| Type | Presets (id: name; key values for new ones) |
|---|---|
| `particles` (9) | gold_dust: Gold Dust; embers: Embers; light_motes: Light Motes; confetti: Confetti; sakura: Sakura; star_shower: Star Shower; bokeh: Bokeh; rain: Rain; fireworks: Fireworks |
| `ink_splatter` (4) | ink_behind: Ink Behind (墨); red_ink: Red Ink (赤); ink_over: Ink Spattered Over (墨); swept: Swept Streaks |
| `speed_lines` (8) | focus: Focus Lines (集中線); focus_red: Focus Lines Red, Additive; flow_h: Flow Lines Horizontal (流線); flow_diag: Flow Lines Diagonal; flash: Flash (フラッシュ); beta: Beta Flash (ベタフラ); spiky: Spiky Burst; rainbow: Rainbow Tunnel |
| `light_streaks` (4) | anamorphic: Anamorphic; bundle: Light Bundle; spears: Spears; cyan: Cyan Streak |
| `shockwave` (5) | white: Shockwave White; red: Red Shockwave; neon: Neon Rings; halo: Ring of Light; ground: Ground Shockwave |
| `magic_circle` (6) | gold: Magic Circle Gold (魔法陣); blue: Magic Circle Blue; hud: HUD Target; hologram: Hologram Base; radar: Radar; bagua: Bagua (八卦) |
| `brush_swirl` (4) | ink: Ink Swirl (筆渦); gold: Gold Brush; red: Red Brush; slash: Slash |
| `glints` (5) | highlights: Highlight Glints; hero: Hero Glint; stars: Star Glints; rainbow: Rainbow Glints; scatter: Scattered Sparkles |
| `lens_flare` (3) | blue: Blue Flare; gold: Gold Flare; streak: Long Streak |
| `backlight` (3) | god_rays: God Rays; jackpot: Jackpot Rays (大当り); blue: Blue Rays |
| `fire` (4) | behind: Flames Behind; over_edges: Flames over the Edges; hot: Hot Fire (激アツ); blaze: Tall Blaze |
| `smoke` (3) | grey: Grey Smoke; steam: White Steam; black: Black Smoke |
| `aura` (4) | tendrils: Blue Tendrils; purple: Purple Tendrils; strong: Strong Aura (強); rising_weak: Rising Weak Aura (弱) |
| `lightning` (3) | bolts: Lightning Bolts; electric_blue: Electric Blue; electrify: Electrify (along Edge) |
| `pattern_fill` (S80's type; 6 new + 9) | new: wood, marble, brushed_metal, carbon_fiber, brick, camo (each its kind with tuned colours); Pachislop's zebra, giraffe, leopard, tiger, checker, hazard, seigaiha (青海波), asanoha (麻の葉), shippo (七宝) when S80 ships none |
| `fractal_noise` (6) | clouds (fbm, scale 400); fog (fbm, contrast 0.6, gradient transparent → `#c8d0dc`); plasma (turbulent, gradient `#ff2bd6` → `#29f0ff`); caustics (ridged, contrast 2, gradient `#002a4a` → `#bff6ff`); static (fbm, scale 2, contrast 3); nebula (fbm, complexity 7, gradient `#0a0018` → `#7a2cff` → `#ff6ad5`) |
| `sunburst` (7) | red_gold: Red & Gold Rays; blue: Blue Rays; rainbow_halo: Rainbow Halo; purple: Purple Rays; black_gold: Black & Gold; focus_lines: Focus Lines; two_colour_wedges: Two-Colour Wedges (each with the Vignette recipe, as Pachislop's) |
| `light_glow` | soft_bloom (radius 120, falloff 70, exposure 0.5); jackpot_flash (exposure 3, radius 400, highlight_shift 80, core_white 50) |
| `contour_lines` | light_guide (defaults, 導光板); outside_rings (outside, count 4, spacing 14) |
| `motion_blur` | speed (distance 120, trail) |
| `lens_blur` | bokeh_highlights (radius 40, brightness 60, threshold 200) |
| `tilt_shift` | miniature (blur 24, width 120) |
| `radial_blur` | impact_zoom (amount 0.5); spin (spin, 30°) |
| `extrude` | long_shadow (recipe: angle 45, length 260, fade 1, flat `#000000`, opacity 0.55, shading 0, edge 0); block_letters (length 40, shading 0.8) |
| `motion_trail` | echo (echo, copies 4); smear (defaults) |
| `halftone` | comic_dots (luminance, round, size 6, `#000000`, multiply); ben_day (uniform, size 8, dot_scale 0.6, `#ff2a6d`); line_screen (line, angle 45); dissolve (alpha, fade 60) |
| `perspective_shadow` | low_sun (length 400, perspective 0.6) |
| `floor_reflection` | glossy_floor (opacity 0.6, blur 6) |
| `wave_warp` | heat_haze (turbulent, amplitude 3, wavelength 40); ripple (radial, amplitude 12) |
| `chromatic_aberration` | rgb_split (directional, amount 10, smooth off) |
| `grain` | film (amount 0.2, size 1.6) |
| `glitch` | digital (blocks, amount 0.5); vhs (slices, split 12, slices 30) |
| `material` | bevel, chrome, gold_chrome, glass, brushed_metal, spun_metal, gold_leaf (Pachislop); emboss (bevel smooth, size 4 %, depth 3, layer colours); deboss (emboss with `bevel.direction: down`) |
| `shatter` | glass_break (defaults); cracks (cracks_only); blast (progress 1, strength 600, fall 800, fade 0.6) |
| `distress` | grunge; dry_brush (かすれ: dry_brush, edge 30, scale 40); worn_print (speckle, wear 0.4); spray_paint (spray, outside 0.6); scratched (scratches, angle 20) |
| `drips` | paint; slime (`#7cff2a`); blood (`#8a0010`); icicles (icicles, `#dff6ff`) |
| `scanlines` | scanlines; crt (crt, mask 0.6); lcd (lcd, spacing 3); interlace |
| `pixelate` | retro_8bit (size 6, levels 4, bayer4); led_board (led, gap 0.2); mosaic (size 20, soft) |
| `brush_stroke` | sumi (bristles 0.7, dry 0.5); marker (bristles 0.1, roughness 0.1) |
| `frost` | frost; frozen_solid (amount 1, reach 60) |
| `neon_tube` | pink_neon; cyan_neon (`#29f0ff`); broken_neon (gaps 0.3, flicker 0.4) |
| `chalk` | chalkboard; pastel (layer colours, grain 5) |
| `stitch` | patch_border (running); cross_stitch (cross); embroidered (satin_border, fill satin) |
| `holographic` | foil (glitter); rainbow_sticker (stars); diffraction (diffraction) |

Recipe entries (D4) and what they add: **Long Shadow** → `extrude/long_shadow`;
**Emboss**, **Deboss** → `material/emboss`, `material/deboss`; **Icicles** → `drips/icicles`;
**CRT** → `scanlines/crt`; **Comic Dots**, **Ben-Day Dots** → `halftone/comic_dots`,
`halftone/ben_day`; **Shine** → S80's Gradient Overlay with `style: sweep`, Pachislop's
`glintOverlay` recipe written in S80's keys: `angle` 30, `position` −20, `length` = 70 % of the
layer box, `gradient` = a white band whose edge stops encode softness 85 plus a second band at
0.32 × width trailing 2.1 widths at alpha 0.8, `amount` 0.9, `mode: light`; **Rainbow** →
`rainbowOverlay`: `style: linear`, `angle` 20, `gradient` = one spectrum cycle at saturation 0.9
(stops), `mode: color`; **Vignette** → `style: radial`, `mode: shade` (multiply), `amount` =
`vignette(amount)`.

### 6.9 Pachislop id map (`editor/resources/titleart/pachislop-fx-map.yaml`)

Rules applied to every entry: camelCase keys become snake_case; `x`/`y` pairs become one Vec2
(`center`, `position`, `origin`, `impact`, `point`; `cx`/`cy` too); `rx`/`ry` become `radius`;
`useGlobalLight` → `use_global_light`; preset ids become snake_case; a generator's `follow` param
becomes the layer's `follow` member; `companionOf` groups become a visible Group "<layer> FX" (D3).
**Every Pachislop generator angle is counter-clockwise (its comp space is y-up) and is negated**;
effect angles (clockwise) are kept, except Extrude's `light` (−v → `light_angle`).

| Pachislop | Ours | Renames beyond the rules |
|---|---|---|
| `outerGlow`, `innerGlow`, `dropShadow`, `innerShadow`, `gradientOverlay`, `gaussianBlur`, `adjust` | S80 types | S80's map |
| `keyline` | S80 `outline` | S80's map |
| `deepGlow` | `light_glow` | `colorMode` → `color_mode` |
| `contourLines` | `contour_lines` | |
| `motionBlur`, `lensBlur`, `tiltShift`, `radialBlur` | `motion_blur`, `lens_blur`, `tilt_shift`, `radial_blur` | `centerX/centerY` → `center` |
| `wave` | `wave_warp` | |
| `chromaticAberration`, `grain`, `glitch` | same, snake_case | glitch gets `mode: slices` |
| `extrude2d` | `extrude` | `light` → `light_angle` = −light |
| `motionTrail`, `halftone`, `perspectiveShadow`, `floorReflection` | snake_case | halftone gets `dot: round` |
| `material`, `shatter` | same | shatter `x/y` → `impact` |
| `particles` | `particles` | `rx/ry` → `radius`, `x/y` → `point`, `angle`, `heading` negated |
| `inkSplatter` | `ink_splatter` | `angle` negated |
| `speedLines` | `speed_lines` | `layer` → the layer's `follow`; `angle` negated |
| `lightStreaks` | `light_streaks` | `angle` negated |
| `sunburst` | `sunburst` | `centre` → `center_mode`, `onLayer` → `on_layer`, `colorA/B` → `color_a/b`; `rotation`, `twist` negated |
| `shockwave`, `magicCircle`, `brushSwirl` | snake_case | magic circle `text` → `glyphs` (`eto` → `zodiac`, `bagua` → `trigrams`), `row{n}*` → `rows[n−1].*`, `rotation` and row angles negated; brush swirl `angle` negated |
| `glints`, `lensFlare`, `backlight` | snake_case | `rotation`, `heroAngle` negated; backlight `centreMode` → `center_mode` |
| `fire`, `smoke` | same | `angle` negated (90 → −90) |
| `corona` | `aura` | `mode: aura` → `rising`, `aura` → `strength` |
| `lightning` | `lightning` | |
| `patternFill` | S80 `pattern_fill` | `rotation` negated |
| `style.erode` | S80 `style.erode` | |
| `style3d.glint` | S86's 3D glint (§6.6.30) | |

### 6.10 Add Effect and Add Generator menus (S82 draws them from the registry)

**Add Effect** (a layer, a part or several selected; categories in this order, `*` = S80):

| Category | Entries |
|---|---|
| Glow | Outer Glow*, Inner Glow*, Light Glow, Contour Lines, Neon Tube; Glint (3D highlights) (style setting, 3D layers) |
| Shadow & Depth | Drop Shadow*, Inner Shadow*, Long Shadow (recipe), Extrude, Perspective Shadow, Floor Reflection |
| Stroke & Edge | Outline* (alias Keyline), Ink Brush Stroke, Stitch |
| Color | Color Overlay*, Gradient Overlay*, Shine (recipe), Rainbow (recipe), Vignette (recipe), Adjust*, Holographic Foil, Grain |
| Texture & Material | Pattern Overlay*, Material, Emboss (recipe), Deboss (recipe), Distress, Dry Brush (かすれ) (style setting), Drips, Icicles (recipe), Frost, Chalk |
| Stylize | Halftone, Comic Dots (recipe), Pixelate, Scanlines, CRT (recipe), Glitch, Chromatic Aberration |
| Blur | Gaussian Blur*, Motion Blur, Radial Blur, Lens Blur, Tilt-Shift Blur |
| Distort | Wave Warp, Motion Trail, Shatter |
| Generators that follow this layer | every preset of every `follow` optional or required generator, grouped by the generator categories below; each row reads "Adds a Generator layer" and shows above/below |

A new effect is appended at the end of the stack (S80 default). Search ranks name matches over
alias matches over description matches; each row shows its cost badge (R41).

**Add Layer > Generator** (generator categories): Particles (Particles, Ink Splatter); Lines
(Speed Lines, Sunburst, Light Streaks); Light (Glints, Lens Flare, Backlight); Rings (Shockwave,
Magic Circle, Brush Swirl); Energy (Fire, Smoke, Aura, Lightning); Fill & Pattern (Fill*, Pattern
Fill*, Fractal Noise). Each opens to its presets; a following generator added here follows the
selected layer when one is selected.

### 6.11 MCP, Lua and docs

**`list_title_library`** (S80's tool, kinds `effects` and `generators`, `id` detail): S83 adds one
argument and some row fields, no tool:

```text
list_title_library {kind: "effects" | "generators", query?, category?: string, id?: "<type or recipe>"}
```

- Rows gain `entry` (`effect`, `generator`, `recipe`, `style_setting`), `cost` as the class C0-C4,
  `live` (kind and params), `follow` (generators) and `presets` (ids). Recipes and style settings
  are listed in `effects` with the type and preset they add; `query` also searches aliases.
- `id` detail gains each param's `anim` class, the presets with their params, `fallback`, and the
  docs anchor in `docs/mcp/guide/title-art-effects.md`; the parameter-line format stays S80's
  (`size px 40 [1..400] L: how far the glow reaches`).
- Schema: one `category` string (built with `McpSchema.h`); growth ≤ 120 characters of the
  tool's 800 cap. An unknown `id` fails listing the nearest ids.

**`edit_title`** (S80 ops): `add_effect {type, preset?}` applies preset params over the defaults;
`add_layer {type: "generator", generator, preset?, follow?, place?: "above"|"below", group?:
"auto"|"none"|"always"}` (D3); `apply_recipe {layer?, part?, recipe}` (new op: the §6.8 recipe
entries by id: `long_shadow`, `shine`, `rainbow`, `vignette`, `emboss`, `deboss`, `icicles`, `crt`,
`comic_dots`, `ben_day`). Validation follows R45.

**`render_title {options: {cost: "desktop" | "mobile"}}`** (a key of S80's `options`) adds `cost`
(§6.4 readout) to the result; `"mobile"` uses the mobile model. Text summary example: `cost: C3 total, desktop 3.1 ms, mobile
11.8 ms predicted; Title: light_glow C3 2.4 ms (mobile C2 7.0 ms), glints C1 0.2 ms`.

Budget: S83's row of S80 §6.11's program budget table: `category` on `list_title_library` (≤ 120
characters); `cost` is a key of `render_title`'s `options` (0 schema characters); no
`budget.json` entry.

**`check_title`** gains (each PASS/FAIL/WARN with fix ops, never an aesthetic verdict):
`fx_follow_missing` (fix: `set {follow}` to the nearest layer or remove), `fx_bake_only_realtime`
(D12; fix: Render Mode Baked), `fx_live_budget` (R35; fix: reduce the named param, e.g. `count`
to 2,000, or bake), `fx_realtime_budget` (re-render > 15 ms predicted on mobile; fix: the class's
heaviest param), `fx_hdr_on_8bit` (info).

**Lua** (S81's `TitleArt`): through Exposed Parameters (R47). **Guide**: `docs/mcp/guide/title-art.md`
section "Effects and generators": discover with `list_title_library {kind, id}`, effects vs
generators and following, recipes, live params, the cost readout, reseeding strobes.

### 6.12 Live variants and classes for S80's definitions (S85's minimum set)

S83 sets the §6.2 members on S80's twelve definitions in the same change. Live variants:

| Definition | L params | Kind |
|---|---|---|
| `gradient_overlay` | `position`, `amount`, `angle` (sweep); `offset`, `scale` (linear, radial) | `kernel`: the band or gradient from the 256 × 1 LUT over the cached input split (S85's sweep and Color Cycle) |
| `outer_glow` | `intensity`, `opacity` | `scale_split` (S80's glow split) |
| `drop_shadow`, `outline` | `opacity` | `scale_split` |
| `color_overlay` | `opacity`, `color` | `kernel` over the cached input split |
| `pattern_fill` | `offset`, `rotation` | `kernel` (§6.7.17) |
| `inner_glow`, `inner_shadow`, `pattern_overlay`, `gaussian_blur`, `adjust`, `fill` | none | re-render |

With §6.6 and §6.7 this covers S85's §6.2 table: `light_glow` `intensity`, `chromatic_aberration`
`amount`, `wave_warp` `phase` and `amplitude`, `shatter` `progress`, `glitch`
`seed` and `amount`, `sunburst` `rotation`, `shockwave` `radius`, `width`, `flash`. Every
`intensity`, `glow` and `brightness` param of an `adds_light` definition has `combine: multiply`
(S85). S85 R12's suffix rule applies: a live param followed in its stack by an enabled effect
without a live variant plays as re-render.

### 6.13 Glyph Atlas safety (S81 R31)

| Class | Definitions |
|---|---|
| safe | `material` (per-shape shading) |
| approximate | `light_glow`, `contour_lines`, `lens_blur`, `halftone`, `wave_warp`, `chromatic_aberration`, `grain`, `distress`, `drips`, `scanlines`, `pixelate`, `brush_stroke`, `frost`, `neon_tube`, `chalk`, `stitch`, `holographic` (per glyph: overlaps, joins and noise differ from the merged render) |
| unsafe | `motion_blur`, `tilt_shift`, `radial_blur`, `extrude`, `motion_trail`, `perspective_shadow`, `floor_reflection`, `glitch`, `shatter` (they read the layer box or neighbours beyond the cell); every generator that follows the text layer |

## 7. Acceptance criteria

The fixture is S80's **standard title** (確定, Dela Gothic One, size 200, Pachislop's
`classicTitleStyle2D` converted; `engine/tests/core/titleart/fixtures/`), rendered as Pachislop's
goldens are (`GOLDEN_STILL`: PNG path at 0.25 ×, trimmed). Following generators follow it, placed
as their preset says. Goldens are written on glcore and Vulkan must match.

| AC | Verification |
|---|---|
| AC1 (R1-R4, R7, R8, R49, R53, R55) | `TitleFxCatalogTest.EveryDefinitionComplete`: the registry holds the 27 effect and 16 generator ids of §6.6/6.7 (plus S80's); ids snake_case and unique, names unique, aliases non-empty; every param has label, doc, unit and animation class, a default inside its range, enum defaults among the options, `when` naming an existing param and value; every preset file parses and sets only known params in range; every recipe names an existing type and preset; every definition has a `glyph_safety`, every param an `anim` class, and the light colours of §6.3 carry `light: true`; `TITLE_ART_VERSION` is unchanged. Red control: a test-only definition with a default outside its range fails |
| AC2 (R17, R26-R28) | `TitleFxGoldenGpu.Effects/*`: each S83 effect at defaults and with its first two presets of §6.8, and each pattern kind of §6.6.28 through Pattern Fill, matches `engine/tests/core/golden/titleart/fx/<id>[_<preset>].png` (written on glcore, Vulkan matches). New-effect goldens are reviewed by eye once; the screenshots are in the evidence |
| AC3 (R18) | `TitleFxGoldenGpu.Generators/*`: every generator preset of §6.8 (72 ported, 6 Fractal Noise, 6 new Pattern Fill) matches `golden/titleart/gen/<id>_<preset>.png` |
| AC4 (D2, R19-R25) | `TitleFxParityGpu`: the 17 ported effects at defaults and the 72 ported generator presets against Pachislop's references (copied to `engine/tests/core/golden/titleart/pachislop/{effect,generator}/` with S80's provenance file), with S80 AC12's metric (4 × 4 block means, block threshold 24, ≤ 3 % of blocks over, colour histogram distance ≤ 6 %). It runs after S80's `TitleArtParityGpu` on the standard title, and a failure there is named in every dependent failure. At most 10 entries may be listed in `docs/mcp/guide/title-art-effects.md` "Parity exceptions", each naming the S83 improvement or S80 base difference responsible, with a side-by-side image. Red control: `glints` `threshold` 30 → 45 fails `glints_highlights` |
| AC5 (R10, R15, R25, D7) | `TitleFxDeterminism`: each definition and preset rendered twice from fresh documents (fresh ids), and with 1 and 8 job workers, is identical (tolerance 0) on one backend; a `seed` change changes it; particles `count` 400 → 401 keeps the first 400 instance records bit-identical; glint anchors at `px_scale` 1 and 4 agree within 0.5 canvas px; speed-line, shatter and stitch layouts at `px_scale` 1 and 2 agree in canvas px |
| AC6 (R9, R31, R32, §6.5, D14, D17) | `TitleFxFallbackGpu`: every definition at defaults with `TITLE_ART_TEST_NO_FLOAT_TARGETS`, and with `TITLE_ART_TEST_NO_FLOAT_BLEND`, agrees with its float render within its F class, with the same pass names and count; the D11 mobile Light Glow agrees with the desktop one within F1. Red control: headroom H = 1 makes `light_glow` exceed F2 |
| AC7 (R19) | `LightGlowMathTest`: the CPU twin of the response is bit-exact identity on 10⁶ random colours with channels ≤ 1; octave weights sum to 1 ± 1e-6 over falloff 0-100 and radius 4-1000; doubling `radius` keeps total glow energy within 1 %. `LightGlowGpu.Identity`: with `threshold` 2 and a layer whose channels are all ≤ 1, the ink pixels equal the input (on_face 0) |
| AC8 (R20) | `HighlightFinderTest` on synthetic grids: a lit straight bar gives ≤ 1 pick on it (red control: w_ridge forced to 1 gives ≥ 4); two isolated spots are both picked; a second text line at 60 % brightness gets ≥ 1 large glint; a flat plateau gives one peak |
| AC9 (R21) | `LightningGpu.MaxBlendNoDoubling`: at the crossing of two bolt segments the core is ≤ the brighter segment alone + 1/255. Red control: additive blending exceeds it |
| AC10 (R22) | `ShatterLayoutTest`: over 1,000 random convex pieces, the interpolated `a_edge` at random interior points of each fan triangle equals the analytic distance to that triangle's cut edge within 1e-4 px; at `gap` 0 piece areas sum to the tiled box area within 0.01 %; UVs equal rest positions |
| AC11 (R23) | `SpeedLineLayoutTest`: no sampled point of any line lies inside the focus ellipse, rectangle or the silhouette dilated by `padding`; toggling `side` leaves the other lines' records unchanged; the largest angular gap is ≤ 2 × the mean gap at `jitter` 60 |
| AC12 (R24) | `FireGpu.FollowsSilhouette`: on an "L" shape the flame above the top edge reaches ≥ 0.9 × `height`; at `spread` 0 the flame beside the vertical edge is < 5 % of `height`; the mobile baked-field path agrees within F1 |
| AC13 (R11) | `QuadBatchGpu.SubPixelEnergy`: a 0.3 px quad's summed light equals its area × colour within 2 %; quads at `min_px` and above match analytic coverage; 65,536 instances draw in one call |
| AC14 (R34, R35, §6.4) | `TitleFxBudget.StaticModel`: every definition at defaults and each preset, at 1920 × 1080 (desktop) and 1024 × 512 (mobile), is within its declared class (presets at most one class heavier, as their file states) and every live kernel within R35. Red control: a test definition declaring C1 with 5 FRE fails |
| AC15 (§5 Performance, R36) | `TitleFxBench` on server H (`JASPER_TEST_PERF_SLACK=1`, GPU lane): measured GPU time per definition at defaults is within its class's desktop budget; `AnalysisBench` on server A checks R36; `TitleFxBench.Calibrate` prints the FRE constant. A missed budget is recorded in Deviations with its table |
| AC16 (R37, R38) | `TitleFxZeroCost`: a document with every S83 effect disabled and every generator layer hidden has the same S15 pass labels, the same scratch-target allocations and no extra shader variant as the document without them; scratch targets return to S80's pool after a render (pool high-water unchanged on a second render); with `JASPERENGINE_TITLE_ART=OFF` no `titleart::fx` or `titleart::gen` symbol is linked (S80's module-off check) |
| AC17 (D10, R35) | `TitleFxLiveGpu`: for each definition with a live variant and each L param at 3 values, the live frame equals a full re-render at that value within F1 (`scale_split` within F0); the per-frame pass count equals the declared kernel's; no live frame reads back |
| AC18 (R5, R6, R44-R46, §6.11) | `tools/mcp/tests/test_title_fx.py`: `list_title_library {kind: "effects"}` lists the menu entries (recipes and style settings included) with classes, `id: "light_glow"` returns its table with `anim` classes and presets, `query: "power glow"` finds it; `add_effect {type: "lite_glow"}` fails naming `light_glow`; an out-of-range `set` is clamped and echoed; a `set` on a hidden param returns `inactive`; `add_layer {generator: "fire", follow: "Title", group: "auto"}` creates "Title FX" with the fire inside, and an Add-blended generator there leaves a layer below the group unchanged (`render_title` crop); `apply_recipe long_shadow` adds an Extrude with the preset's values; `render_title {options: {cost: "mobile"}}` has every §6.4 field; removing the followed layer makes `check_title` report `fx_follow_missing` whose fix op passes when applied; a Realtime document with Lens Blur reports `fx_bake_only_realtime`. `tool_budget.py --check tools/mcp/budget.json` and `gen_tool_docs.py --check` pass |
| AC19 (R39-R43, §6.10) | `tools/mcp/tests/test_title_fx_menu.py` (isolated editor, xvfb; after S82): Add Effect shows the categories in order with counts; "power glow" finds Light Glow, "かすれ" finds Dry Brush (style setting) and Distress, "fire" shows the generator section; adding a recipe is one undo step named after it; the seed dice is one undo step; the cost badge exists (`find_ui_item`); a following generator whose layer is deleted shows "Follows a deleted layer". Screenshots of the menu and the Light Glow inspector are in the evidence |
| AC20 (R29-R31, R33) | `TitleFxShadersCompile`: every S83 program and its `TFX_LIVE` variant compiles through `engine/libs/shadercompiler` to GLSL 410, GLSL ES 300, MSL, HLSL and SPIR-V; a static scan finds no `texture(` inside a loop and `precision highp float` in each. `tools/build.sh web` and `android` build with the module on and off; `tools/smoke-test.sh` exports a Realtime title with Glints and Light Glow and runs it |
| AC21 (R48) | `test_docs.py` (`gen_effect_docs.py --check`): `docs/mcp/guide/title-art-effects.md` equals the text generated from the registry and presets, S83's columns included. Red control: changing a label fails it |
| AC22 (§5 Performance) | Manual: on a Mali-G57 or Adreno 610-class phone, the fixture in Realtime with Gold Dust (`loop`, `time` animated), Glints twinkle and Light Glow `scale_split` keeps the live compose ≤ 0.5 ms GPU per frame (S81's budget), and a full re-render of the classic look ≤ 15 ms time-sliced (S15 timings or a vendor profiler); recorded in `docs/mcp/guide/title-art.md` "Device measurements". Without a device it is recorded as pending in Deviations (S13 AC23 precedent) |
| AC23 (R51, D16) | `PachislopFxMapTest`: every Pachislop effect type, generator type, preset id and param key appears in the map; each negated param is a generator angle; converting each Pachislop preset gives exactly the preset file's values (the renders are AC4) |
| AC24 (R12) | S83's passes that use them (lightning cores, drips, light accumulators) pass AC6 and AC9; the blend modes themselves are S80 AC44 (`BlendModesGpu.LightAndMax`) |
| AC25 (R13, R14, R50) | `ReadGridTest`: packed luminance round-trips 0-4 within 1/16384; one readback per content key (counter); a Realtime export with static text performs zero `FrameReadback` calls over 120 frames, and one per analysed layer after a runtime text change |
| AC26 (R16) | `TitleFxNoClock`: a static scan finds no time or clock uniform in `engine/shaderlib/titleart/{fx,gen}` and `tfx_*.glsl`, and no clock call in `engine/core/titleart/{effects,generators,analysis,gpu}`; one document rendered at two engine times is identical |
| AC27 (R47) | `TitleArtLuaFxTest` (S81's harness): with `{layer: "Title", field: "fx:light_glow.intensity"}` exposed as `glow` and `fx:light_glow.radius` as `glow_radius`, `setParameter("glow", 2)` changes the next frame with no re-render (re-render counter unchanged) and `setParameter("glow_radius", 300)` causes exactly one |
| AC28 (R52) | `TitleFxPassVersion.GoldenHash`: each definition's golden images are hashed and recorded with its `passVersion` in `engine/tests/core/golden/titleart/pass-versions.json`; a changed golden without a bumped `passVersion` fails |
| AC29 (R54, §6.12) | `TitleFxLiveGpu.S58MinimumSet`: every parameter of S85's §6.2 table, keyed on the fixture, plays live (S85's plan reports no `live_fallback`) and its live frames match re-renders within F1; a sweep followed by an enabled Gaussian Blur falls back with S85's reason text (red control for the suffix rule) |

## 8. Dependencies & conflicts

**The Title Art program graph and waves** (the same in S80-S87; S80 §8 is the source, S80 §6.15
the shared rules; an arrow means "needs"):

| Wave | Specs | Needs |
|---|---|---|
| 6 (title art) | S80 core | merged work only |
| 6B | S81 runtime, S82 Title Designer, S83 effects and generators, S86 3D | S80 (every hook of S80 §6.15); within the wave, by owner: S81 owns the component, scheduler, live compose, `title_fx`, the module wiring and the 3D and mesher sub-options; S82 owns the window, its §6.8 hooks, `DocRow` and the 3D gizmo; S83 owns the catalogue values of the registry members (glyph safety, live variants, classes, cost model), `Glint3D` and the light encoding; S86 owns 3D layers, `Gizmo3DTarget`, `quality`/`debug_view`, environments and the compressed-light encoding |
| 6C | S84 library, S85 animation, S87 localisation and interchange | S80 and wave 6B; within the wave, by owner: S84 owns `PachislopPort`, `pachislop-map.json`, `TitleLibraryApply` (and `lookFromStyle`), the denied terms; S85 owns sampling, `TitleAnimPlan`, flipbooks and `<file>.anim.json`; S87 owns the sidecar `languages` table and the per-language bake sets |

Edges: S81 → S80. S82 → S80, S81 (Preview As Glyph Atlas, create menus). S83 → S80, S81 (live
compose runs its kernels; sidecar anchors). S86 → S80, S81 (scheduler, sub-options, component
fields), S82 (sections, gizmo), S83 (`Glint3D`). S84 → S80, S82 (Library panel), S83, S86. S85 →
S80, S81, S82, S83 (S86 optional). S87 → S80, S81, S82, S83, S84, S85, S86. One exception crosses a
wave: S82's Library panel milestone (its M6) draws S84's data and lands with S84 in wave 6C; the
rest of S82 lands in 6B.

- **S80 (wave 6, blocking).** Used as S80 specifies it: `TitleEffectRegistry` (§6.5), `ShaderType::TITLE_ART`
  programs and includes (§6.3), regions of interest, content hashes, the target pool, `TitleArtCaps`
  and its RGBA8 fallback with the per-request test flags (§6.3), the ink-versus-light runner for
  Outline (§6.4), layer sources for `follow` (§6.4), `style.erode`, Material, the pattern library,
  Gradient Overlay's `sweep` style (for Shine), `ImageDiff`, and the tools of §6.11. The hooks S83
  needs are S80's (S80 §6.15): the registry members of §6.2 and `FxAnim`, `ParamKind` String,
  Material, RowArray, Material `bevel.direction`, `LIGHT_PREMULTIPLIED` and `MAX_PREMULTIPLIED`,
  the reserved op `apply_recipe`, `render_title.options` (S83's `cost` key), the check registry and
  the sidecar's `anchors` member. S83 adds, without a format version bump: the values of the
  registry members (its 43 definitions and S80's twelve); twelve `pattern.kind` values; Pattern
  Fill's `offset`, live variant and presets; the light headroom of D14; `list_title_library` row
  fields and `category`; `apply_recipe`'s implementation; five `check_title` checks; S83's columns
  in `gen_effect_docs.py`.
- **S81 (same wave, owner of the runtime).** Runs S83's live kernels in its live compose (S81 R9) within its per-title
  budget, keeps the splits they read (light splits, the pre-distort input, instance buffers,
  anchors), ships analysis results in its bake sidecar (S81 R8, the 16 strongest anchors) and its
  Realtime export data (R14), applies D12 on the mobile tier, takes `glyph_safety` from §6.13 (S81
  R31), and drives params through Exposed names (S81 R66, R47).
- **S82** draws rows, gizmos, badges and the §6.10 menus from the registry. **S84** converts
  Pachislop looks with §6.9, references preset ids, reads `light: true` and the spectrum switches
  (R55), and its classic style port is the fixture's; S84 uses `drips` (`count`, `length`,
  `width`) and `pixelate.size` as defined here.
  **S85** derives its track classes from `ParamDef::anim` (S80's `FxAnim`, S85 R11), keys the
  params, provides "Reseed every N frames", and relies on §6.12 (`light_glow`, `wave_warp`,
  `shatter.progress`). **S86** (same wave) hosts the 3D glint and stores its style
  (`style_3d.glint`); `Glint3D` accepts S86's compressed SRGB8A8 light encoding (§6.6.30). **S87** maps Photoshop Bevel and Emboss "Emboss"/"Pillow Emboss" to Material
  with `bevel.direction`, and Outer Glow, Satin and Stroke to S80's types.
- **S15** (pass timings), **S43** (palettes for Pixelate), **S06** (jobs), **S32** (warm-up list,
  through S81), **S11** (`iridescence.glsl`, read only).
- **Merge hotspots:** `engine/core/render/Render.h` (`BlendMode`, `BlendOperation`),
  `engine/renders/sokol/SokolPipelineCache.cpp`, `SokolObject.cpp`; `NOTICE.md`; S80's registry
  and `TitleArtOps` files; `editor/resources/titleart/`; `docs/mcp/guide/title-art.md`;
  `tools/mcp/budget.json` (`title_art` group).

## 9. Risks & defaults

1. **Parity depends on S80's base title.** If the fixture alone does not match Pachislop's classic
   reference, effect parity fails too. Default: AC4's thresholds are not loosened; the base is
   fixed first (S80's `TitleArtParityGpu` runs first and is named in every failure).
2. **Mobile figures are predictions.** No phone is on the build servers. Default: readouts label
   mobile numbers "predicted", AC22 is manual, pending is recorded (S13 precedent).
3. **MAX on float targets** needs `blendRGBA16F`. Default: the drip field and the lightning core
   use RGBA8 with a monotonic encoding whenever float blending is missing; MAX on unorm is core in
   GLES3/WebGL2.
4. **Readback stalls on WebGL** (synchronous). Default: only on a runtime change of an analysed
   layer, inside the time-sliced re-render; static titles never read back (R14).
5. **Shader count.** About 60 pass shaders and their live variants. Default: variants compile only
   for types a document uses (S32 warm-up list through S81); goldens run on every type.
6. **No S20 conversion.** Screen-wide simulated celebrations stay S20's; a converter would approximate
   a closed-form look with a simulation and confuse parity. Default: not provided; the guide says
   when to use which.
7. **Hash precision** on `mediump`-only drivers. Default: `highp` is required in every title shader;
   devices without highp fragment precision are below the engine's GLES3 baseline.
8. **Magic circle fonts offline.** Default: when a set's library font cannot be copied, the built-in
   face draws the glyphs and the layer reports S80's missing-glyph problem.
9. **Catalogue size confuses people.** Default: 27 effect types in 8 categories, recipes for the
   rest, search with aliases, hover preview, cost badges.
10. **New pattern kinds and genre effects are judged by eye.** Default: their goldens are reviewed
    with screenshots once, then guarded like every other golden.
11. **"Light Glow" is unfamiliar to users who know the effect by a product name.** Default: the
    generic aliases "power glow", "bloom", "glare" find it; the docs explain the technique; product
    names stay out (S84's denied terms).

## Milestones

| M | Content | Gate |
|---|---|---|
| M1 | Machinery: blend modes, `QuadBatch`, `ReadGrid`, streams, cost model, 8-bit light encoding, registry fields, docs generator; Light Glow, Glints, Shine recipe | AC1, AC5-AC7, AC8, AC13, AC14, AC24, AC25 for these |
| M2 | Live-value generators: Shockwave, Speed Lines, Particles, Lightning, Fire, Smoke, Aura, Shatter, Light Streaks, Lens Flare | AC3, AC4, AC9-AC12, AC17 for these |
| M3 | Remaining ports: blurs, distorts, shadows, Extrude, Motion Trail, Halftone, Contour Lines, Material, Magic Circle, Brush Swirl, Backlight, Ink Splatter, Sunburst, Pattern Fill additions | AC2-AC4 complete |
| M4 | Genre effects, pattern kinds, Fractal Noise, recipes | AC2 complete |
| M5 | MCP and menus, budgets on H, device check, parity ledger | AC15, AC16, AC18-AC23, AC26-AC28 |

## Deviations

1. **Reconciled: S80 declares every registry member and the blend modes.** S83 fills the values;
   the types (`FxAnim`, `CostClass`, `GlyphSafety`, `LiveKind`, ...) and `LIGHT_PREMULTIPLIED` /
   `MAX_PREMULTIPLIED` are in S80's headers so that the same-wave S81, S82 and S86 compile against
   them (S80 §6.5, §6.4).
2. **Reconciled: `ParamDef::anim` is S80's `FxAnim {None, Live, Rerender, Step, Relayout}`,** one
   enum for S83's L/R/S/X marks and S85's classes; the separate `relayout` flag is gone (it is
   `Relayout`), `interp` stays for stepped live params.
3. **Reconciled: `CostTier` is gone;** the row field `cost` is the class C0-C4 everywhere,
   including S84's derived item cost.
4. **Reconciled: `render_title`'s `cost` and `tier` are one key `options.cost: "desktop" |
   "mobile"`** of S80's free-form `options`, to keep the tool under 1,500 characters (S80 §6.11).
5. **Reconciled: live kernels run only in S81's live compose, never inside `title_fx`; Lua reaches
   params only through Exposed names** (S81 R9, R66, contract decision 13).
6. **Reconciled: no product names as aliases.** "deep glow" is replaced by "power glow"; `deepGlow`
   stays only as the Pachislop source key in the id map.
7. **Reconciled: `Glint3D` lives in `engine/core/titleart/three_d/`** behind S81's
   `JASPERENGINE_TITLE_ART_3D`, and shipped anchors use S80's sidecar shape `{x, y, size, strength,
   fade}`.
8. **Reconciled: the Shine, Rainbow and Vignette recipes are written in S80's `gradient_overlay`
   keys** (`length`, `gradient` stops, `amount`, modes `light`, `color`, `shade`); Pachislop's
   width, softness, double band, bands and saturation become gradient stops.

### Generators stream (`ta/s83-generators`)

9. **Implementation: the generator programs have a number space of their own.** A `TITLE_ART`
   program with property bit 9 ('Gen', `TITLE_VARIANT_GENERATOR`) compiles
   `titleart_gen.vert` / `titleart_gen.frag` with the number of `generators/TitleGen.h`'s
   `GenProgram` (`titleart/gen/programs.glsl`), so the effects and the generators never share a
   `TA_PROGRAM` number. Their includes live in `engine/shaderlib/titleart/gen/` (`tfx_noise`,
   `tfx_quads`, `tfx_field`, `tfx_glint_star`, `tfx_anamorphic`, `tfx_upstream`,
   `tfx_particle_shapes` and one file per pass family) rather than in `titleart/` itself.
10. **Implementation: `QuadBatch` pulls its records in the vertex stage.** Records are an RGBA32F
    data texture (4 texels a quad; meshes 2 a vertex) read with `texelFetch` in the vertex stage,
    as the lattice, VAT and skinning shaders already do, and a quad is 6 vertices addressed by
    `gl_VertexIndex`: no instanced vertex buffers, no storage buffers. The texture is sampled,
    never rendered or filtered, so R31 (no renderable or filterable RGBA32F) holds. The records
    upload through `TitlePassRunner::lut`, cached by content, so a live frame that changes only
    uniforms uploads nothing.
11. **Implementation: interface additions (all additive).** `TitlePass::vertices` and
    `vertexData`, `TitleGpuPass::vertices` and `vertexData`, a vertex-stage block `u_vs_titleart`
    mirroring p[0..5]; `TitleLutData::width`, `height`, `filter`; `TitlePassRunner::skippedPasses()`;
    `GeneratorContext::followContent`, `followKey`, `followContentKey`; the planner's
    `layerContentSource` (the followed layer drawn without its own effect stack: R6's `content`),
    used when a definition sets `FxInputs::content`; the pass kit's targets answer `colorTexture()`.
12. **Implementation: seeds keep Pachislop's streams.** An element's seed is its `seed` param plus
    the document's seed − 1 (S80 Deviation 39) and each generator derives its streams as Pachislop
    does (its two mulberry32 seedings `gen::Rng`, its integer hash `gen::hashN`, its salts), not
    R15's mix with the type-id hash: the default document then draws Pachislop's particles,
    glints and bolts bit for bit, which AC4 needs.
13. **Implementation: analysis runs in the generator's own step.** The grid pass and one
    synchronous `FrameReadback` of the ≤ 480 × 288 RGBA8 grid run inside the generator's kernel
    step (the readback is synchronous anyway), and the CPU analysis (≤ R36's budgets) follows it;
    the grid is cached by the followed layer's key, so an unchanged layer is never read back again.
    A step whose grid program is still compiling skips the readback (`skippedPasses`) and runs
    again. `EffectDef::analyse` stays unused by the generators.
14. **Implementation: preset files are YAML in its JSON-compatible form.** The engine's golden
    tests read them with the JSON parser (the engine has no YAML parser); the editor embeds them
    (`editor/titleart/fx/FxPresets.cmake`) and parses them with yaml-cpp, so block-style YAML would
    load there too. Each preset carries `source` (the Pachislop preset it ports, the parity
    tests' reference name) and optionally `class`.
15. **Implementation: generators declare no `bounds`.** S80's planner maps `bounds` to the frame
    without the generator layer's transform, so a box would cut a moved layer; every S83 generator
    leaves `bounds` null (the canvas) and scissors its passes to its own box where it has one.
16. **Implementation: a generator's live kernel is its own passes.** `LiveVariant::program` names
    the generator program (with the Gen bit); S81's live compose runs the definition's passes
    again with the new values over cached inputs (grids, anchors, follow splits). No `TFX_LIVE`
    variant is compiled: generator passes read no stack input that a live variant would replace.
17. **Implementation: enum values start with a letter** (S80's snake_case check): Glints `rays`
    are `four`, `snow`, `eight`.
18. **Implementation: `add_layer` generator members.** Without `preset` the first preset applies
    (R7) with its layer look (blend, opacity, effects, name); `follow` places the layer beside the
    followed one (`place`, else the preset's, else above) unless `parent`, `before` or `after`
    say otherwise; `group` defaults to `none` (D3's `auto` is what the Add Effect menu sends).
19. **Implementation: the Fractal Noise `static` preset** uses `scale` 4, the param's minimum
    (§6.8 said 2, outside the range §6.7.16 gives).
20. **Implementation: generators get the render's assets and report problems.**
    `GeneratorContext::assets` (the request's `AssetResolver`) and `problems` (appended to the
    job's) carry Magic Circle's fonts and its missing glyphs. Its library fonts (Zen Old Mincho
    Black, Zen Kaku Gothic New Black) are read where S84 copies them
    (`TitleArt/Fonts/<dir>/<file>`); without them the built-in face draws and CJK sets report
    `glyph_missing` (§9 risk 8). The tests carry both fonts (OFL, google/fonts).
21. **Implementation: the mobile tier is a cost statement.** `GeneratorContext` carries no tier, so
    the mobile reductions of §6.7 (Particles' 2,000 cap, Lens Flare's 8 quads, Backlight's
    quarter-res live kernel, Fire and Smoke's baked half-res field, Fractal Noise's 4 octaves) are
    what each definition's `estimate` reports for `FxTier::Mobile`; S81's tier selection passes
    them to the render when it lands (Particles and Fractal Noise already apply theirs from the
    params). AC12's mobile-agreement half is therefore not tested.
22. **Implementation: per-generator details.**
    - Particles: `travel` is X, not L (burst colour and depth depend on it, so a live change would
      differ from a re-render: AC17); the default gradient is Pachislop's white-gold ramp; the
      animation shaping of `time`, `loop`, `spin`, `twinkle` is the port's own (at the defaults
      the image is Pachislop's still).
    - Speed Lines and Sunburst draw in layer space as Pachislop does; following, their centre is
      the followed layer's ink centre (from the grid) plus `center` (for every `shape`).
      `followReach` < 0 means the whole canvas. The Beta Flash preset carries the Halftone
      dissolve as a layer effect, which renders once the effects stream's `halftone` lands.
    - Brush Swirl: `width`, `taper_in`, `taper_out` are X (they rebuild the strokes); the strokes
      draw random values in Pachislop's order (only non-main strokes draw some), which R25's
      "all values first" would change; `reveal` cuts the ribbon in the shader and the splatter on
      the CPU after every droplet's values are drawn.
    - Aura's third program is `AuraNormals` (the field's central differences packed for RGBA8
      before the blur); Fire's default gradient is Pachislop's default, §6.7.11's ramp is the
      `hot` and `blaze` presets'. Lightning's two glows are Pachislop's blur radii (0.35 and 1.6
      x glow size), at half resolution once sigma reaches 2 px.
    - AC12 as written cannot hold: the side weight 1/max(0.08, spread) leaves the flame beside a
      vertical edge at 8 % of `height` at spread 0; `FireGpu.FollowsSilhouette` checks < 8 % (and
      spread 35 % > 20 % as its control).
    - Backlight runs 3 passes with the noise source folded into the first radial blur; its and
      Lens Flare's "shape" centres come from the 4 px grid's ink box (within ~2 canvas px of
      Pachislop's per-pixel box). Glints' `point` is measured from the canvas centre (Pachislop).
23. **Implementation: parity exceptions (AC4).** 63 of the 72 ported presets meet AC12's metric;
    nine are listed in `TitleGenParityGpu`'s `PARITY_EXCEPTIONS` with their cause and held to
    twice the histogram limit (all have histogram distance <= 3.5 % except Beta's 8.2 %; the
    block share is what they miss): `speedLines-beta` 17.6 % (its Halftone dissolve is the effects
    stream's `halftone`, absent on this branch), `glints-highlights` 6.2 %, `glints-rainbow` 6.4 %
    (the picks follow S80's base title, whose highlights differ from Pachislop's, read from the
    8-bit grid), `glints-scatter` 11.7 % (stars kept out of the ink by the 8-bit grid's distance),
    `backlight-godRays` 8.7 % and `backlight-jackpot` 14.0 % (the shape centre from the 4 px grid
    turns every ray slightly), `corona-tendrils` 13.1 %, `corona-auraStrong` 7.5 % and
    `corona-auraWeakUp` 27.3 % (normals from the RGBA8-packed field gradient). They are the first
    follow-ups for look parity. AC4's red control is a reseeded Gold Dust instead of a `glints`
    threshold change, since `glints_highlights` is an exception.
24. **Implementation: fallback agreement of grid-driven layouts.** Lightning's bolts and
    Backlight's shape centre come from the followed layer's grid, which the RGBA8 path reads from
    its own render of that layer: a borderline cell can flip, so a few pixels (0.15-0.2 %) differ
    by more than the class's max while the mean and the share over 8/255 stay within F1/F2.
    `TitleGenGpu.FallbackWithinClass` holds these two to the mean and the share only.
25. **Implementation: the mobile tier renders (supersedes 21).** `GeneratorContext::tier` and
    `RenderRequest::fxTier` (`tier` there is S86's) carry `FxTier`; the node cache and the
    followed-layer keys tell the tiers apart, and each definition's mobile `estimate` counts the
    passes its mobile path runs. Where the reductions depart from §6.7:
    - Fire and Smoke draw at half resolution through a downsample of the content and a copy
      back: the content's field, E baked into a field (`UpstreamField`, R16F or RGBA8 fixed
      point, E / length), the flame or smoke pass reading E at its swayed point. The field is
      baked on every render (4 passes, ~4.1 FRE at 1024 × 512, inside C3), so §6.7.11's "C1 per
      frame" holds only once S81 keeps the baked field while `angle`, `height` and `spread`
      hold. The 256² noise texture holds two independent fbm-like fields repeating every 255
      texels (16 lattice units); being another random field than the desktop value noise, it
      moves the tongues and billows, so AC12's "mobile baked-field path agrees within F1" is
      checked on the baked field (the flame's base coverage, `FireMobileGpu.BakedFieldAgrees`)
      and the whole mobile image against the desktop one by colour histogram only.
    - Aura's half resolution covers its field, normals and blur too; its 3-octave fbm adds the
      dropped octaves' mean so the tongues keep their lengths.
    - Lens Flare's quads also keep the halo ring (inside the core's quad); the starburst is left
      out and the bloom and streak tails fade out at 6 sizes and 3 streak lengths.
    - Light Streaks draws the halo source and both halo blurs at quarter resolution; the cores
      stay at full resolution.
    - Measured mobile-against-desktop agreement at 0.25 (`TitleGenMobileGpu.AgreesWithDesktop`,
      first presets): Particles equals the desktop render of its first 2,000 (F1) and Fractal
      Noise the desktop render at complexity 4 (F1, and F1 against complexity 5); Backlight 0.8 %
      of blocks, Aura 0.5 % (AC12's block metric); Light Streaks 3.2 % of blocks (held at 4 %:
      quarter-res halos); Lens Flare mean 1.7/255, 2.2 % of pixels over 8/255, max 135/255 (held
      to F2's mean and share: its quads cut the bloom's faint tail at their edges); Fire and
      Smoke 5.5 % and 4.9 % of blocks with histogram distance 1.2 % and 0.9 % (held to the
      histogram: the noise texture is another random field), Fire's baked field to F1.
26. **Implementation: the two S83 streams' shared names.** S83's effect programs take
    `ShaderType::TITLE_ART` property bit 9 ('Efx', `titleart_fx.frag`) and live kernels bit 10
    ('Liv'); the generator programs take bit 11 ('Gen', `titleart_gen.vert/.frag`), so a generator
    program and an effect program with the same number differ in their properties (bit 11 vs bit
    9), hence in their ShaderPool keys and their shader names (`titleart_Pb2Pb3Gen` vs
    `titleart_Pb2Pb3Efx`); the property count of `TITLE_ART` is 12 and `TitleGpu` takes numbers up
    to 0xFFF. The editor files of both streams are shared: the preset loader `FxPresets`, the cost
    model `FxCost` and the Pachislop map `PachislopFxMap` with its data file are the effects
    stream's; the generators add `FxGeneratorCost` (their rows, appended to `FxCost`'s estimate),
    `PachislopGeneratorMap` (the `generators:` section of the same data file) and their tests.
    `.gitattributes` merges `pachislop-fx-map.yaml` and `FxCost.cpp` with the union driver, where
    each stream appends its own lines.
