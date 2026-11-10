# S48 — Pixel FX: sprite effects, screen transitions, glitch, game feel, contact sheets

Pixel-art family (S41–S52, `PIXEL-ART-LEDGER.md`), wave 5B. Depends on S43 (palette ramps, the
Bayer include), S41 (the art pixel grid), S44 (the family's lint result types) and S52 (the FX slots
and `instance=`), all wave 5A; uses S49 (dissolve embers) and S51 (the built-in pixel font) of the
same wave when present. Global decisions:
`docs/specs/README.md`. Research: `/tmp/pixelart-research/pixelfx` (MIT, read-only, never run).

## 1. Goal

Give a pixel-art game the effects that make it feel finished: a hurt sprite that dithers out, an
enemy that burns away along a glowing front coloured from the game's palette, a petrified or frozen
status look, a crisp one-pixel outline and drop shadow, screen transitions with hard pixel edges, a
digital glitch, hit-stop, integer-pixel screen shake and animation held on twos. Every effect stays
on the art pixel grid, uses palette colours instead of blends, costs nothing until it is used, and
runs on all five backends with vertex and fragment shaders only. People drive it from the
Inspector, scripts from Lua, and agents prove it with labelled contact sheets and effect lints that
return PASS, WARN or FAIL with the measured values.

## 2. Scope

**In scope**
- `library:sprite` (`engine/shaderlib/library/sprite.{frag,vert}`): new features DITHER_FADE,
  DISSOLVE, RAMP_RECOLOR, DROP_SHADOW; a pixel style for OUTLINE with an inline, 4 or 8 neighbours,
  and an outline colour from a palette ramp (including selective outline, "sel-out"); quad growth so
  outline, shadow and glow are not cut at the frame edge; a per-instance effect channel for S52.
- Two small manifest extensions the features need: `@keywords ... feature=<DEFINE>` and a
  `choice(...)` property kind.
- The variant and feature budget of `library:sprite`.
- Screen transitions as one overlay pass driven by a progress value: Iris (circle, diamond, square),
  Blinds, Slash, Pixel Dissolve, CRT Off, Burn Away, Fade (through a palette colour, dithered). Hard,
  constant-width art-pixel edges, rims coloured from a ramp. `Transition` Lua API with scene-change
  integration.
- Glitch as an uber-pass feature (`UBER_GLITCH`): RGB split, row tears, interlace, bursts.
- Game feel: Hit-Stop (with per-object exemption and targeted freezes), integer-pixel screen shake
  through the existing Camera Shaker, held-pose stepping ("Pose Frame Rate") on the Animator,
  Animation Player and Pose Animator.
- A `PixelFX` Lua API that plays the sprite effects over time (flash, fade, dissolve, status looks)
  and, with S49, dissolve embers.
- Agent tooling: `capture_contact_sheet` (group `pixel_art`) with effect lints on the family's
  lint result types (S44's `lint/LintTypes.h`), including the three rule lints S46's catalogue
  later reuses (`off_palette`, `partial_alpha`, `flicker`, D13); docs and the agent guide page.
- Editor UX: feature toggles with live thumbnails and a scrub preview, Pixel FX material presets, a
  Glitch group in the post-processing Inspector, a transition preview button and a Contact Sheet
  button in the Game view, the new Camera Shaker and animation fields.

**Out of scope**
- Palette assets, ramps and PALETTE_SWAP (S43). S48 reads ramps; it does not define them.
- Particle simulation (S20, S49). S48 only asks S49 to emit embers along a dissolve front.
- The art grid, PPU and the pixel-snap rule (S41). S48 consumes them.
- The craft lints (jaggies, doubles, holes, contrast; S46). S48 lands before S46, so it implements
  the three rule lints both need (`off_palette`, `partial_alpha`, `flicker`, with S46 R26's
  definitions) next to its effect lints, and S46's catalogue calls them.
- A rotation effect (cleanEdge, rotxel): S41/S42 forbid runtime rotation; S44/S47 own drawn
  rotations.
- Transition kinds that read the screen (warp, swirl, a glitch wipe): they would need a copy of the
  composited frame including the UI layers (§6 D8). The VHS glitches of S13 already cover the look on
  the scene.
- The HTML UI's `effect()` transitions (`engine/shaderlib/effects/iris-wipe.frag`, `glitch.frag`,
  `noise-dissolve.frag`): they stay HTML UI element effects (shaders.com), smooth-edged, per element.
- The do-not-copy list of `docs/specs/README.md` does not touch this spec (no render graph, no
  compute).

## 3. Current state (read on `specs/pixel-art` 1b1c34fbd)

**`library:sprite`** (`engine/shaderlib/library/sprite.frag`, 245 lines; `sprite.vert`, 40 lines):
- Features (`sprite.frag:9-15`): OUTLINE, HUE_SHIFT, FLASH, GRAYSCALE, COLOR_REPLACE, OVERLAY, GLOW,
  in the fixed order of `:35-39` (colour replace, hue shift, grayscale, overlay, flash, outline
  behind the sprite, alpha mode, 2D lights, glow).
- Parameters live in one `u_fs_customParams` block that exists only when a feature is on
  (`:54-88`), with the engine-written `time` first.
- OUTLINE (`:147-168`, `:222-228`) takes the max alpha of 8 neighbours at `outlineWidth` texels,
  the diagonals on the circle (`stepD = step1 * 0.70710678`), so a width of 1 samples the diagonals at
  0.707 texel: with a NEAREST texture that rounds to the centre texel or to a neighbour depending on
  the subpixel position, which is not a pixel outline. The width is a float `range(0, 8)`.
- Every tap is clamped into the current atlas frame inset (`frameInset`, `:140-144`), and taps
  outside the frame read 0. The quad is not grown, so an outline or glow is cut at the frame edge
  unless the artist left transparent margins.
- `SPRITE_FRAME_RECT` (`:46-48`, `sprite.vert:8-38`) passes the frame rect as a flat varying, from
  `spriteParams.textureRect` and, instanced, `i_textureRect`.
- Instance attributes today: `i_matrix` (4), `i_color`, `i_textureRect`
  (`engine/shaderlib/includes/surface_vs.glsl:182-190`).
- No palette input, no noise, no dither, no shadow.

**Shader manifests** (`engine/core/shader/ShaderManifest.h`):
- `@feature` is a compile-time define with a default (`ShaderManifestFeature`, `:125-130`).
- `@keywords` is an exclusive group whose define is always emitted (`ShaderManifestKeywords`,
  `:132-138`), with no `feature=` scope.
- Property kinds: FLOAT, RANGE, INT, TOGGLE, COLOR, HDRCOLOR, VECTOR2-4 (`:40-50`); no choice list.
- The Inspector draws features as checkboxes, each change an undoable `PropertyCmd` on
  `shaderFeatures` (`editor/window/InspectorShaderLibrary.cpp:569-581`). Only the combinations used
  by scenes, materials and the warm-up list are compiled and shipped
  (`editor/shader/ShaderVariantCollector.cpp`; S32 `ShaderWarmup.h:16-18`).

**Post-processing**:
- User post passes (`SceneSettings::postProcess`, `engine/core/SceneSettings.h:308-337, 429`) run
  before the uber pass.
- The uber pass has 18 feature bits, appended only (`engine/core/subsystem/RenderSystem.h:410-432`;
  names at `engine/core/pool/ShaderPool.cpp:884`), the 7-step order of
  `engine/shaderlib/uber.frag:7-33`, and the parameter block at `uber.frag:82-103`.
- With fixed resolution the uber pass is the upscale blit and reads the fixed-size target
  (`RenderSystem.cpp:15582-15595`). Layer scenes (HUD) draw after it, at native resolution
  (`RenderSystem.cpp:16450-16453, 16491-16493`).
- The library post shaders are pixelate, posterize, gameboy and crt (`engine/shaderlib/library/`).
- There is no transition system. `SceneManager` has a loading scene flow (`Covering`, `Loading`,
  `Loaded`, `engine/core/manager/SceneManager.h:79-105`; Lua at `CoreClassesLua.cpp:2386-2432`).
- The frame draws every scene, then the debug overlay (`Engine.cpp:2402-2413`).

**Time**:
- `Engine::timeScale` multiplies the frame delta (`Engine.cpp:2186`,
  `deltatime = simulationHeld ? 0.0 : frameTime * timeScale`) and the game-time scheduler
  (`Engine.cpp:466`). `Time` is the Unity facade (`engine/core/EngineTime.h:20-65`; Lua
  `CoroutineClassesLua.cpp:72-75`).
- Per-component unscaled switches exist: `ParticleSystemComponent::useUnscaledTime`,
  `TweenComponent::useUnscaledTime`, `CameraShakerComponent::unscaledTime`,
  `AnimatorUpdateMode::UnscaledTime` (`engine/core/animation/AnimatorTypes.h:21-25`). There is no
  hit-stop and no per-object freeze.

**Camera shake** (`engine/core/component/CameraShakerComponent.h`; design
`docs/superpowers/specs/2026-10-31-camera-shake-design.md` §6-8):
- Shake Presets (`.shake`, 12 built-ins), trauma, a view offset that never writes the camera
  Transform, and a transform mode for other GameObjects.
- The offsets are continuous noise in units of tenths of the view height. They are not rounded to
  pixels, and rotation is applied.

**Animation**: `AnimatorComponent` (`engine/core/component/AnimatorComponent.h:107-130`),
`AnimationPlayerComponent`, `PoseAnimatorComponent` and `SpriteAnimationComponent` sample poses at
the frame's time; STEP keys exist (`KeyframeSampling.h:18-23`), but there is no way to hold every
track on N fps steps while root motion stays smooth.

**Agent surface**:
- `screenshot` (`editor/mcp/tools/ViewTools.cpp:1938-1962`) captures one view.
- `play {fixed_fps, auto_pause, step}` and `wait {frames}` make Play deterministic, and
  `Engine::setPinnedShaderTime` pins shader time (`Engine.h:663-676`).
- Nothing captures N frames of one object into one image, and nothing checks an effect.

## 4. Reference (pixelfx, MIT, `/tmp/pixelart-research/pixelfx`)

| What | File | Take | Change |
|---|---|---|---|
| Dithered alpha | `src/core/painter.js:10, 46-49` | The 4x4 Bayer matrix `(v+0.5)/16`; a pixel is drawn only if `a*fade > BAYER[(y&3)*4+(x&3)]` | On the GPU: the 4x4 and 8x8 thresholds of S43's `includes/bayer.glsl` (no texture, no second table). Texel space by default, screen space at art resolution as an option |
| Constant-width edges | `src/fx/transitions/_lib.js:115-152` (`field`) | `(k - f) * g` with `g = 1/clamp(|grad f|, 0.45, 3)` is the distance in pixels to the level set | Gradient by finite differences of `f` at the neighbouring art pixels inside the shader: no `dFdx`, so all backends agree |
| Transition structure | `_lib.js:1-17` (`screenMeta`), `iris.js:14-27`, `crt-off.js:15-27`, `pixel-dissolve.js:14-46`, `blinds.js`, `slash-wipe.js`, `burn-away.js`, `diamond-wipe.js` | Cover by about 45 % of the time, hold, uncover so the last frame is empty; rim bands quantised to ramp steps; CRT Off's squeeze to a line and a dot; Pixel Dissolve's mixed block sizes ranked by a clumpy field | One fragment shader, a uniform-selected kind; particles, glints and motes left to S49 (`Transition` options name a particle preset) |
| Dissolve key | `src/fx/spawn/dissolve-burn.js:38-46` | `key = height + 0.08·jitter + 0.06·noise - rim`, against an eased front, giving scorched, flame and gone bands | Integer hash per texel shared by C++ and GLSL (§6 D4), so S49 can spawn embers where the GPU burns |
| Ramp recolour | `src/core/color.js` (`fit`, `at`), status effects (`src/fx/status/`) | Brightness to a ramp step, `C[round(2 + luma·5)]`, no blending; a 4-neighbour bevel | The ramp is a texture (S43 ramp or a PNG strip) |
| Glitch | `src/fx/scifi/glitch.js:43-60` | Burst schedule per frame (30 fps), RGB split of 1-4 px, 1-5 torn rows shifted ±8·k, interlace, a calm row slip | A full-screen uber feature in source texels; sprite-level glitch via OVERLAY + HUE_SHIFT is not attempted |
| Hit-stop and shake | `src/core/act.js:239-252, 263-270` | Freeze for `stop` s, max of overlapping hits, the shake keeps running during the freeze; shake `k = amp·(t/t0)`, `f = floor(t·60)`, offset `round(sin(2.4f)·k), round(cos(3.7f)·k·0.7)` | Hit-Stop on the engine clock; the shake is a new `kick` Shake Preset type with `units: pixels` |
| Held poses | `src/core/act.js:30-49` (`poseAt`) | Limbs move in 12 fps steps counted from each key; the feet's position stays smooth | Per track, from its previous key, in the shared keyframe sampler |
| Effect lints | `tools/check.mjs:14, 108-205` | Spill past a 24 px margin; one-shots empty at the end; seam = last→first change vs `max(2 × p90 step, 12)`; same t twice; slow > 4 ms; a freeze over 0.3 s "reads as a stall" (`:42`) | Run on engine captures; PASS/WARN/FAIL with values; in the family's lint folder, `EffectLints` (D13) |
| Contact sheets | `tools/render.mjs`, `tools/lib/fxlib.mjs:203` (`contactSheet`) | Labelled frames on named backdrops (`src/core/scene.js:10`: dark, ground, sky, light, black) | Integer scale, rows per backdrop, plus `checker` and `scene` |
| Agent loop | `skills/pixelfx/SKILL.md` | Draft 3-5 variants, render sheets, look, fix the worst thing, repeat | Written into `docs/mcp/guide/pixel-fx.md` |

Licence: MIT. Shader code is written fresh from these techniques. Any function ported line by
line keeps pixelfx's copyright line in a header comment and gets a `NOTICE.md` entry. The
Sprite Fusion credits in `iris.js`, `crt-off.js`, `pixel-dissolve.js` and `dissolve-burn.js`
apply only to their particle and glint code, which S48 does not port.

## 5. Requirements

### Functional — sprite features (`library:sprite`)

- **R1 Zero cost when off.** A material without the new features compiles to exactly today's
  variant: same source after preprocessing, same uniform block, same pixels. A new feature adds
  nothing (define, uniform, sampler, varying, instance attribute or vertex work) while it is off.
- **R2 DITHER_FADE** ("Dither Fade"). Properties:
  - `ditherFadeAmount` (range 0..1, default 0: fully shown; 1: gone; `instance=fx.z`);
  - `ditherMatrix` (choice Bayer 4x4 | Bayer 8x8; default 4x4);
  - `ditherSpace` (choice Sprite texels | Screen pixels; default Sprite texels).

  A fragment keeps its colour if `alpha × (1 − ditherFadeAmount) > threshold(cell)`, else it is
  discarded. No partial alpha is ever produced. The thresholds come from S43's
  `includes/bayer.glsl` (pixelfx's `(v + 0.5)/16` table; S43 writes it in wave 5A), the family's
  one Bayer table: S48 adds none of its own.
  - Sprite space indexes the threshold by the texel coordinate inside the frame, so the pattern
    moves with the sprite.
  - Screen space indexes it by the base pixel of S41's target: `floor(p)` of `pixelSnapToPixels`
    (`includes/pixel_snap.glsl`, S41 §6.2), or `floor(gl_FragCoord.xy / frame.pixelSnap.w)` on the
    upscaled target. Without S41 it uses `gl_FragCoord`.
  - It applies last, after outline, shadow and glow, so the whole look fades together.
- **R3 DISSOLVE** ("Dissolve"). Properties:
  - `dissolveProgress` (0..1; 0 = intact, 1 = gone);
  - `dissolveDirection` (vector2, default (0, 1): the front climbs from the bottom; (0, 0) = noise
    only);
  - `dissolveNoise` (0..1, default 0.35), `dissolveNoiseScale` (texels per noise cell, 1..32,
    default 4), `dissolveJitter` (0..0.3, default 0.08);
  - `dissolveEdgeWidth` (whole texels 0..8, default 2), `dissolveScorchWidth` (texels 0..16,
    default 3);
  - `dissolveEdgeGlow` (hdrcolor, default black = off);
  - `dissolveSeed` (int).

  Each texel's key (§6 D4) is compared with the front `progress·(1 + span) - span`, so progress 0
  and 1 are exactly intact and gone.
  - Inside the edge band the texel takes a ramp step from the hot end (at the front) to the cool end,
    with no blend.
  - Inside the scorch band the sprite colour is pulled one ramp step toward the ramp's darkest.
  - Below the front the texel is gone.
  - The band widths are in texels along the whole front, by the field rule of §4.
- **R4 RAMP_RECOLOR** ("Ramp Recolour", the petrify, frozen and poison looks). Properties:
  - `rampAmount` (0..1, default 1);
  - `rampLow`, `rampHigh` (the ramp steps luma 0 and 1 map to; defaults: the ramp's second step
    and its last);
  - `rampBevel` (0..1, default 0);
  - `rampPulse` (steps per second, default 0).

  The texel's display luma picks a ramp step, `round(mix(low, high, luma))`. `rampAmount` below 1
  is a dithered choice per texel between the original and the ramp colour (Bayer, sprite space), not
  a blend.
  - The bevel lightens texels whose up-left neighbour is transparent by one step and darkens those
    whose down-right neighbour is transparent by one step.
  - The pulse cycles the step offset over time (poison), quantised to whole steps.
- **R5 OUTLINE, pixel style.**
  - New keyword `OUTLINE_STYLE` (`feature=OUTLINE`) with the values:
    - `SMOOTH`: first, so the default and today's behaviour bit for bit;
    - `PIXEL4`: the 4 neighbours;
    - `PIXEL8`: the 8 neighbours.

    In the pixel styles `outlineWidth` is rounded to whole texels, 1..2. A texel is outline when it
    is transparent and a neighbour at whole-texel steps 1..width in the 4 (or 8) directions is opaque.
  - New properties:
    - `outlineColorFrom`: a choice of Colour (today), Ramp darkest, or Selective. Selective is
      sel-out: the outline texel takes the colour of the opaque neighbour it touches, moved
      `outlineRampSteps` darker along the ramp (nearest ramp step by luma, then down; clamped at the
      darkest).
    - `outlineRampSteps` (1..4, default 1).
    - `inlineWidth` (whole texels 0..2, default 0) and `inlineColor`: opaque texels with a
      transparent 4-neighbour take the inline colour (or, with Selective, their own colour one step
      darker).
  - With DISSOLVE on, the outline follows the dissolving shape: a neighbour's opacity includes its
    own dissolve key.
- **R6 DROP_SHADOW** ("Drop Shadow"). Properties:
  - `shadowOffset` (whole texels, vector2, default (1, -1));
  - `shadowColor` (colour, default sRGB (0, 0, 0, 0.5)); with `shadowColorFrom` = Ramp darkest it
    takes the ramp's darkest step at full alpha.

  The shadow is the sprite's alpha shape (after DISSOLVE, before DITHER_FADE) moved by the offset,
  drawn behind the sprite and the outline, and inside the grown quad (R7). Shadow alpha below 1 is
  dithered (R2's matrix) when DITHER_FADE is on; otherwise it is the material's blend.
- **R7 Grown quad.**
  - With OUTLINE (pixel or smooth), DROP_SHADOW or GLOW, the sprite's quad grows on each side by the
    features' reach in texels (outline width; the shadow offset on its side; for glow, its manifest
    reach, 4 texels), so nothing is cut at the frame edge.
  - The UVs grow with it. Taps outside the frame still read 0 (today's clamp), so atlas neighbours
    never bleed in.
  - Picking, culling bounds and sorting use the grown bounds.
  - With none of these features the quad is unchanged (R1).
- **R8 Feature order** (it extends `sprite.frag:35-39`):
  1. palette swap (S43);
  2. colour replace;
  3. hue shift;
  4. ramp recolour;
  5. grayscale;
  6. overlay;
  7. flash;
  8. dissolve (colour bands, then alpha);
  9. inline;
  10. outline (behind);
  11. drop shadow (behind the outline);
  12. alpha mode;
  13. 2D lights;
  14. glow;
  15. dither fade.

  The order is fixed and documented in the manifest description and `docs/shaders/library.md`.
- **R9 Ramp input.**
  - One texture slot, `u_fxRampTexture` ("Ramp"), is shown while any of DISSOLVE, RAMP_RECOLOR, or
    OUTLINE/DROP_SHADOW with a ramp colour source is on.
  - It accepts an N×1 PNG strip (N 2..32, NEAREST, no mips, never compressed) or an S43 ramp
    reference (`<palette>.palette#ramp:<name>`, a sub-asset S43 exposes as such a strip).
  - Unset, it binds the new default `ramp_grey` (8 steps black to white).
  - The shader reads `textureSize` for N and samples texel centres only.
- **R10 Effect channel (S52 R22-R23, the family contract).** The animated amounts are declared
  per-instance with S52's manifest attribute, so instanced sprites that differ only in them stay in
  one batch:
  - `flashAmount ... instance=fx.y` (the existing FLASH; S52 M3 adds this line in wave 5A);
  - `ditherFadeAmount ... instance=fx.z`;
  - `dissolveProgress ... instance=fx.w`.

  `fx.x` is S43's PALETTE_SWAP row. With these, all four slots of S52's FX stream are taken; a
  later per-instance value needs a new stream (S52 R23).
  - Non-instanced sprites read the material uniforms.
  - Instanced sprites read S52's `i_fx` stream (HALF4), and an absent stream writes zero. That is
    why every slot's neutral value is 0 (R2's amount counts toward gone).
  - `rampAmount` and every other property are material-wide and part of S52's batch key.
- **R11 Idle cost.** With a feature on but its amount neutral (`ditherFadeAmount` 0,
  `dissolveProgress` 0, `rampAmount` 0, `flashAmount` 0), the shader skips the feature's body on a
  uniform (or flat per-instance) branch: ≤ 4 ALU and 0 extra taps per fragment.

### Functional — screen transitions

- **R12 Kinds.** All of these cover the screen in the ink colour and reveal it again. Each takes the
  common options of R13.
  - `iris`: shape `circle`, `diamond` or `square`; centre (normalized, default the main camera's
    view centre, or a GameObject's screen position via `center_object`).
  - `blinds`: horizontal or vertical bands of `count`, each closing from its own edge.
  - `slash`: a straight wipe at `angle` (degrees, default 30).
  - `pixel_dissolve`: blocks of 1, 2 and 4 cells of `cell` art pixels (default 16), winking in
    in clumps ranked by a value-noise field plus a hash.
  - `crt_off`: the dark closes from above and below onto a 1-pixel line, the line shrinks to a dot,
    the dot fades. The reveal is the reverse with a brief warm-up glow.
  - `burn_away`: a noise front with an ember band from the ramp.
  - `fade`: dithered cover in the ink colour (Bayer 8x8 at art resolution), optionally stepping
    through the ramp's colours (`through_ramp`).
- **R13 Common options.**
  - `color` (ink, default sRGB #0b0a10).
  - `palette` + `ramp` (rim colours, default the built-in ember ramp).
  - `rim` (art pixels 0..8, default 2).
  - `glow` (0..1, default 0.4: additive light just inside the edge, quantised to ramp steps).
  - `pixel_size` (0 = the S41 grid's scale; else screen pixels per art pixel).
  - `seed`, `ease` (linear | in | out | in_out, default in_out).
  - `hold` (seconds covered between cover and reveal in `play`, default 0.1 × duration).
  - Edges are hard: every output pixel inside one art pixel has the same colour, and a rim is the same
    width (±0 art pixels) along its whole length.
- **R14 Phases.** A transition is `cover` (progress 0 → 1, screen covered at 1), `hold` and
  `reveal` (1 → 0).
  - `Transition.play(kind, duration, opts)` runs cover, hold and reveal in `duration` (cover 45 %,
    hold 10 %, reveal 45 % unless `hold` is given) and fires `onCovered` at the hold.
  - `cover` and `reveal` run one phase each.
  - A new transition replaces the running one from its current coverage, so the screen never pops.
- **R15 Scene change.** `Transition.loadScene(scene, kind, duration, opts)`:
  1. Covers.
  2. Calls `SceneManager.loadScene` (holding the load with `holdLoading` until covered).
  3. Holds while the new stack loads.
  4. Reveals once `SceneManager.loading` is false and the new scenes have drawn one frame.
  - If `SceneManager` shows its loading scene (after `loadingDelay`), the transition reveals onto it
    and covers again when the load completes, both at half duration.
  - The transition survives the scene change: it belongs to the engine, not to a scene.
- **R16 What it covers.** The overlay draws after every scene of the frame (main scene, child and
  layer scenes, the loading scene) and before the debug overlay (`Engine.cpp:2402-2413`), so the HUD
  is covered too. A project setting `transitionsCoverUI` (default true) draws it after the main
  scene's uber pass instead, under the layer scenes.
- **R17 Zero cost when idle.** With no transition active (progress 0 and not holding), the frame has
  no transition pass, no draw, no uniform upload and no allocation. The program is created on first
  use and kept.

### Functional — glitch

- **R18 `UBER_GLITCH`** (T1, uber bit appended, name `Gli`). Fields of `PostProcessingSettings`:
  - `glitchEnabled` (false);
  - `glitchIntensity` (1, 0..1);
  - `glitchRgbSplit` (source texels 0..8, default 2);
  - `glitchTears` (rows per burst 0..8, default 3), `glitchTearShift` (source texels 0..32,
    default 8);
  - `glitchInterlace` (0..1 chance per burst frame, default 0.3);
  - `glitchBurstRate` (bursts per second 0..8, default 0.5; 0 = continuous at intensity);
  - `glitchBurstLength` (seconds 0.05..1, default 0.25);
  - `glitchFps` (the corruption's frame rate, 1..60, default 30);
  - `glitchSeed` (int).

  Rows, splits and shifts are in source texels, which are art pixels when S41 renders at art
  resolution. The schedule is a pure function of `floor(time × glitchFps)` and the seed, so a pinned
  time gives one image. In the uber order it is step 1 (row shift, UV), step 2 (split taps beside the
  scene tap) and step 7 (interlace darkening, before grain).
- **R19 Scripted bursts.** `PixelFX.glitch(duration, intensity)` raises the runtime intensity for
  `duration` (unscaled seconds) on the main scene without editing saved settings. It works only when
  the variant has `UBER_GLITCH`. `glitchEnabled` is added to `getScriptableEffectFields`
  (`ShaderVariantCollector.cpp:1055`), so a project that toggles it from Lua ships the variant.

### Functional — game feel

- **R20 Hit-Stop.** `HitStop.play(seconds, opts)` multiplies the game delta by `opts.timeScale`
  (default 0: frozen) for `seconds` of unscaled time, clamped to 0..1.
  - Overlapping calls keep the longer remaining time and the lower scale.
  - It affects everything game time drives: `Time.deltaTime`, the fixed-step accumulator (physics),
    the game-time scheduler (timers, `Coroutine.wait`), actions, tweens, animations, scaled particles,
    sprite animations, camera follow and scaled shakes.
  - It does not affect unscaled time, audio, the HTML UI clock or anything with an unscaled switch
    set.
  - `opts.shake` (a Shake Preset name, or `{amplitude, seconds}` for a pixel kick) starts a shake that
    runs on unscaled time, so it shakes during the freeze (`act.js:247-252`).
  - `HitStop.stop()` ends it.
  - `HitStop.active` and `HitStop.remaining` are readable.
- **R21 Exemptions.**
  - A new component **Hit Stop Exempt** (`HitStopExemptComponent {enabled, includeChildren = true}`)
    keeps its GameObject (and descendants) running at the unfrozen delta in:
    - Animator, Animation Player, Pose Animator and Sprite Animation;
    - Actions, Tweens and Particle Systems;
    - Camera Shaker and Camera Follow;
    - `PixelFX` effects;
    - its scripts: `Time.deltaTime` inside an exempt GameObject's callbacks is the unfrozen delta.
  - `opts.exclude = {go...}` exempts more GameObjects for that hit only.
  - `opts.only = {go...}` inverts the hit: only those GameObjects (and descendants) freeze, and the
    world runs.
  - Rigidbodies freeze and run with the physics world in every mode; per-object exemption does not
    reach physics. This is documented.
  - `HitStop.unfrozenDeltaTime` gives the delta without the hit-stop.
- **R22 Pixel screen shake** (extends the Camera Shaker, no new component):
  - `CameraShakerComponent::pixelSnap` (false). When on, the applied view offset is rounded to whole
    art pixels (S41 grid; without S41, whole screen pixels of the camera's orthographic scale), and
    rotation is dropped (S42's no-rotation rule).
  - `CameraShakerComponent::tickRate` (Hz, 0 = every frame). The offset changes only on ticks of
    1/tickRate, counted on the shake's clock.
  - Shake Presets gain the type `kick` (pixelfx's decaying integer shake: `k = strength·(t/t0)`,
    `f = floor(t·tick)`, offset `(round(sin(2.4f)·k), round(cos(3.7f)·k·0.7))`) and the key
    `units: "pixels" | "view"` (default `view`, today's).
  - New built-ins: `pixel_hit` (kick, 2 px, 0.15 s), `pixel_heavy` (kick, 4 px, 0.35 s), `pixel_rumble`
    (sustained, 1 px, roughness 8, tick 30).
  - `CameraShake.kick(amplitudePx, seconds)` plays an unnamed kick on every global shaker.
- **R23 Held poses.**
  - New field `poseFrameRate` ("Pose Frame Rate", fps, 0 = smooth; presets 8, 12, 24) on
    `AnimatorComponent`, `AnimationPlayerComponent` and `PoseAnimatorComponent`.
  - When > 0, every track holds its value on steps of `1/poseFrameRate` counted from that track's
    previous key: `t_q = k_i + floor((t - k_i)·fps + 1e-6)/fps`. So every key's pose is shown, as
    `act.js:43` does.
  - Root motion (`applyRootMotion`, `onAnimatorMove`) and the root bone's translation are sampled at
    the true time and stay smooth.
  - Events fire at their true times.
  - Blends between states quantise their weight on the same steps.
  - Sprite Animation (already discrete) ignores it.

### Functional — `PixelFX` player

- **R24 `PixelFX.play(go, effect, duration, opts) -> handle`.** It animates the effect channel (R10)
  and related parameters on the GameObject's sprite material over `duration` (game time unless
  `opts.unscaled`; hit-stop aware; easing `opts.ease`). Effects:
  - `flash` (amount 1 → 0, `opts.color`);
  - `fade_out` / `fade_in` (dither fade);
  - `blink` (`opts.count` dithered blinks);
  - `dissolve` / `undissolve` (`opts.ramp`, `opts.embers`);
  - `petrify`, `freeze`, `poison` (ramp recolour in with a preset ramp and bevel; `opts.hold`, then
    out);
  - `outline_pulse`.

  The handle is yieldable (`isDone()`, `Coroutine.waitFor`) and has `stop()`, `progress` and
  `onFinished`. `PixelFX.set(go, name, value)` sets a channel value directly.
- **R25 Missing features.** If the material lacks the needed feature, `play` turns it on in that
  GameObject's own material (never in a shared `.material` file) and logs once per material in
  development builds: `PixelFX: "<effect>" turned on <FEATURE> on <material> at runtime; add it to the
  material (or record the warm-up list) to avoid a compile hitch`. The variant comes through S32's
  asynchronous path: the effect starts when it is ready, and the handle's `progress` stays 0 until
  then.
- **R26 Embers (S49).** `dissolve` with `opts.embers` (a Pixel Particles preset name, default
  `ember`) emits particles, per frame, at up to `opts.emberRate` (default 40 per second) texels that
  crossed the front this frame. Their positions come from the CPU mirror of the key (§6 D4) over the
  frame's opaque texels (the sprite frame's alpha read once per texture and rect, cached).

### Agent tooling

- **R27 `capture_contact_sheet`** (group `pixel_art`, a job above 1 s). It renders a target at N
  fixed times into one labelled PNG at an integer scale, on one or more backdrops.
  - Targets: a GameObject (its animations, particles, materials' time and an optional `PixelFX`
    effect), an `.anim` clip on a GameObject, a `.particles` asset, or a transition kind with its
    options.
  - Modes:
    - `isolated` (default): an offscreen copy of the target alone, with pinned time and seeds, the
      art grid at 1×;
    - `scene`: the target's scene through its camera. While playing, the paused game is stepped at a
      fixed delta.
  - Output: frames in a grid (columns × rows per backdrop), each labelled `#i t=0.25s` in the
    built-in pixel font (S51), with a 1 px separator; the scale is the largest integer that fits
    `max_width` (or the one given).
  - Returns the image, plus `structuredContent` `{frames: [{index, t, bbox}], scale, cell, backdrops,
    lints?, verdict?, path?}`.
  - Preview by default. `output: true` + `path` writes the PNG (one undo step, `WriteFilesCmd`).
  - Deterministic: the same arguments give the same PNG bytes.
- **R28 Effect lints** (`lint: true` or a list of names on the same tool). Each lint returns the
  family's lint result (`{id, status: PASS|WARN|FAIL|SKIP, measured, threshold, threshold_source,
  count, at, message, fix}`, S44's `LintTypes.h`, the shape S46 R25 defines), and the call returns a
  verdict (`clean`, `fixable`, `needs_person`, ledger rule). S49's `capture_particles` checks call
  `spill`, `one_shot_end`, `loop_seam` and `determinism` from here, with these ids.

  | Lint | Measure | WARN | FAIL |
  |---|---|---|---|
  | `spill` | pixels drawn outside the target's bounds + `margin` (default 24 art px; pixelfx `M`) over all frames and seeds 1, 2, 3, 7 | > 0 | — |
  | `one_shot_end` | non-empty pixels at t = duration for a non-looping target (a transition: overlay pixels left) | > 0 | — |
  | `loop_seam` | changed pixels last → first vs `max(2 × p90(step), 12)` | over | over × 2 |
  | `determinism` | changed pixels between two fresh captures of t = 0.37·duration | — | > 0 |
  | `flicker` | S46 R26's definition (1-frame blink on > 6 % of the sprite's pixels), implemented here | > 6 % | — |
  | `off_palette` | pixels whose colour is not in the target's palette (when the material names one, S43; S46 R26's definition, implemented here) | > 0 | > 1 % |
  | `partial_alpha` | pixels with 0 < alpha < 255 (S46 R26's definition, implemented here) | — | > 0 |
  | `frame_cost` | median added CPU frame time (target shown vs hidden, release build) and, with S15 GPU timers, GPU time; plus the variant's static texture taps | > 1 ms or taps > the R33 budget | > 4 ms |
  | `hit_stop_length` | hit-stop seconds requested during the capture (Play) | > 0.3 s | — |
  | `transition_edges` | rim width in art px along the edge, per frame | spread > 0 | — |

  Lints that do not apply to a target report `SKIP` with the reason.
- **R29 Lua for agents.** Everything runtime is reachable through `eval_lua` in Play (`HitStop.play`,
  `Transition.play`, `PixelFX.play`, `CameraShake.kick`), and through `wait {until}` on
  `Transition.active == false` or `handle:isDone()`. No other new tool is needed.

### Platform & backends

- **R30** Every variant compiles and draws the same on GL 3.3, GLES3/WebGL2, Metal, D3D11 and
  Vulkan:
  - integer hashes use `highp uint`, which GLSL ES 3.00 guarantees in fragments;
  - no `dFdx` in any decision that changes a pixel (field gradients are finite differences of the
    analytic function);
  - the Bayer threshold is a constant table or bit maths, never a texture;
  - the ramp is a NEAREST 2D texture sampled at texel centres;
  - no compute, SSBO, texture array or integer vertex attribute (S52's `i_fx` is HALF4, core in
    GLES3/WebGL2).
- **R31** `library:sprite` with every feature on stays within 8 fragment texture bindings (base,
  normal map, replace mask, overlay, ramp, and S43's two palette textures, cells and rows, behind
  one nearest sampler: 7) and a `u_fs_customParams` block of ≤ 32 vec4. S48 adds no vertex
  attribute of its own (the FX slots are S52's `i_fx` stream).
- **R32** The transition pass is one fullscreen triangle, premultiplied-alpha blended
  (`ONE, ONE_MINUS_SRC_ALPHA`), with no texture inputs and no render target of its own.

### Performance (budgets)

- **R33 Per-fragment cost** (counted statically from the SPIR-V of each variant, AC9):

  | Feature | Extra texture taps | Notes |
  |---|---|---|
  | DITHER_FADE | 0 | ≤ 10 ALU; discards |
  | DISSOLVE | 0 (+1 ramp tap in the bands) | ≤ 40 ALU: hash, 2×2 value noise, front, band |
  | RAMP_RECOLOR | 1 ramp (+2 base with bevel > 0) | uniform branch on the bevel |
  | OUTLINE PIXEL4 / PIXEL8, width 1 | 4 / 8 | width 2: 8 / 16; Selective: ≤ 8 ramp taps on outline texels only |
  | Inline | ≤ 4 (shared with the outline taps when both are on) | |
  | DROP_SHADOW | 1 (+ key at the tap with DISSOLVE) | |
  | Every S48 feature on, pixel width 1 | ≤ 24 | |

- **R34 Mobile.**
  - On an Adreno 610 / Mali-G57-class phone at 1080p, 500 instanced 32×32 sprites with FLASH,
    DISSOLVE (progress 0.5) and PIXEL4 OUTLINE cost ≤ 1.0 ms of GPU time more than the same sprites
    with no features.
  - A transition frame costs ≤ 0.3 ms.
  - Glitch on top of the uber pass costs ≤ 0.2 ms (S15 timers or a vendor profiler).
- **R35 CPU.**
  - Hit-Stop and the transition controller cost ≤ 0.01 ms per frame when idle and ≤ 0.02 ms when
    active.
  - `PixelFX` costs ≤ 2 µs per active effect.
  - Ember front evaluation costs ≤ 0.1 ms per 64×64 sprite frame and is cached per frame.
- **R36 Contact sheets.** 16 frames of a 64×64 isolated target at scale 4 in ≤ 2 s on the build
  server, with all lints in ≤ 5 s.

### Editor UX (human)

- **R37 Sprite material Inspector** (`InspectorShaderLibrary.cpp`):
  - Each feature row of `library:sprite` shows a 40×40 live thumbnail: the selected sprite's frame at
    an integer zoom, NEAREST, with only that feature applied at its current values. It is rendered
    offscreen when a value changes and cached.
  - Animated amounts (Flash Amount, Dither Fade, Dissolve Progress, Ramp Amount) get a **Preview**
    scrub slider and a ▶ button that plays the effect in the Scene and Game views without saving,
    like the Camera Shaker's preview. Stop, save, Play or a selection change clears it.
  - `choice` properties show as combo boxes, and keyword groups show only with their feature.
  - Every change is one undo step (existing `PropertyCmd`).
- **R38 Pixel FX presets** (`editor/material/MaterialPresets.cpp`; `apply_material_preset` lists
  them):
  - Hit Flash (FLASH);
  - Pixel Outline (OUTLINE PIXEL4, width 1, Ramp darkest);
  - Sel-Out Outline;
  - Drop Shadow;
  - Teleport Fade (DITHER_FADE);
  - Burn Away (DISSOLVE + fire ramp);
  - Petrify, Frozen, Poison (RAMP_RECOLOR with S43 built-in ramps and bevel);
  - Enemy (FLASH + DISSOLVE + PIXEL4 OUTLINE).

  A preset changes only the fields it sets. In a Pixel Art Mode project (S42, same wave: when S42
  is present), enabling OUTLINE from the Inspector picks `PIXEL4`.
- **R39 Post-processing Inspector** (`InspectorPostProcessing.cpp:138`, Stylised): "Glitch" with
  the R18 fields, the reset, undo and cost tier rows of S13, and a "Burst now" button while playing.
- **R40 Game view toolbar** (`editor/window/GameWindow.cpp`, `drawToolbar`):
  - **Transition ▾** lists the kinds. A click plays it (cover, hold, reveal) over the Game view in
    edit mode or Play, with the Project Settings defaults. The pop-up has Duration, Colour and Ramp
    for the preview.
  - **Contact Sheet…** opens a dialog for R27 on the selection. It has Frames, Duration, Backdrops,
    Scale and Lints, shows the sheet and the lint table in a window, and has Save As… (one undo step).
- **R41 Project Settings › Graphics › Screen Transitions:**
  - Default Kind (Iris), Duration (0.6 s), Colour, Ramp, Rim, Pixel Size (0 = S41 grid);
  - Cover UI (on);
  - all undoable through the settings path; Lua defaults come from here.
- **R42 Components and fields:**
  - Hit Stop Exempt (Add Component › Pixel Art);
  - Camera Shaker's Pixel Snap and Tick Rate (disabled with a reason on a perspective camera);
  - Pose Frame Rate on the three animation components, with a combo of Smooth, 8, 12, 24 and
    Custom.
  - While Play is paused by a hit-stop, the Game view's stats show `HIT STOP 0.06 s`.

### Agent UX (MCP, Lua, CLI)

- **R43** Every new field reaches agents through the existing tools:
  - material features, keywords and properties through `update_material` / `set_properties`
    (`shaderFeatures`, `shaderParams.<member>`);
  - presets through `apply_material_preset`;
  - glitch fields through `set_scene_settings post_processing.glitch_*`;
  - component fields (Hit Stop Exempt, Camera Shaker, `pose_frame_rate`) through the Catalog-driven
    component tools;
  - Project Settings through `set_project_settings`;
  - the new `.shake` keys through `get_asset_info` / `write_file`.
- **R44** One new tool, `capture_contact_sheet` (R27, R28). The `pixel_art` group's budget is in
  §6.1.
- **R45 Lua** (`engine/core/script/binding/`, the API data regenerated by `tools/lua_api`):
  - `Transition`, `HitStop` and `PixelFX` (static tables);
  - `TransitionHandle` and `PixelFXHandle`;
  - `CameraShake.kick`;
  - the enum `TransitionKind`;
  - the fields of R18, R21-R23 on their classes.
- **R46 Docs:**
  - `docs/mcp/guide/pixel-fx.md` (`jasper://docs/pixel-fx`): features, order, costs, presets,
    transitions, hit-stop recipes, the contact-sheet loop of §4 and the lint table;
  - one line in the `## Pixel art` digest section of the agent guide (S41 creates it, S42 owns
    its wording; ≤ 120 bytes, the ledger's share);
  - `docs/shaders/library.md` regenerated;
  - the `library:sprite` manifest description updated.

### Data & compatibility

- **R47** No scene or material version bump.
  - New features, keywords and properties are absent in old materials, and `OUTLINE_STYLE`'s first
    value is today's look, so old materials draw identically (AC2).
  - New component fields default to today's behaviour (`pixelSnap` false, `tickRate` 0,
    `poseFrameRate` 0).
  - `.shake` stays version 1 and gains `units` (default `view`). Its lenient parser keeps unknown
    keys.
  - `PostProcessingSettings` writes only non-defaults (S13 R23).
- **R48** `UberFeature` gains `UBER_GLITCH` appended after the last bit when S48 lands (18 today,
  `RenderSystem.h:430-431`; S41's `UBER_SHARP_BILINEAR` and S43's `UBER_PALETTE` land in wave 5A, so
  it is the next free bit after them, with the name fixed). `UBER_FEATURE_COUNT` is raised
  by one, and the name `Gli` / "Glitch" goes into `ShaderPool.cpp`.
- **R49** New asset-free engine data:
  - the `ramp_grey` texture default (generated);
  - the transition program;
  - built-in shake presets (code, `ShakePresets.cpp`).

  The `.jfont`-style cooking of S51 is not needed here.

## 6. Design constraints & decisions

- **D1 Features, not a new shader.** Everything sprite-side is a `library:sprite` feature, so
  tilemaps, polygons and S52's instanced sprites get it for free, and the Inspector, Lua
  (`material:setShaderFeature`), MCP and the variant collector already handle it. New code lives in
  `engine/shaderlib/includes/pixel_fx.glsl` (hash, value noise, ramp helpers, dissolve key; Bayer
  from S43's `includes/bayer.glsl`),
  included by `sprite.frag` and `transition.frag`.
- **D2 Variant budget.**
  - Defines are only what changes taps or attributes: DITHER_FADE, DISSOLVE, RAMP_RECOLOR,
    DROP_SHADOW, plus `OUTLINE_STYLE` with 3 values.
  - Everything else is a uniform (dither matrix and space, colour sources, inline width, bevel,
    pulse) and is resolved by uniform branches. That keeps `library:sprite` at 13 features (the 7 of
    today, S43's PALETTE_SWAP and PALETTE_CYCLE, these 4) and 1 scoped keyword group
    (`OUTLINE_STYLE`, 3 values). The manifest has no feature-count limit (features are define
    strings, `ShaderManifest.cpp`; the editor's library matrix grows linearly with them), so the
    limits that bind are R31's texture bindings and uniform block and the 4 FX slots.
  - A project ships only the combinations it uses.
  - S42's validator warns above 24 distinct `library:sprite` variants in an export (a count per
    blend mode and light state, from the collector), naming the materials that add the most.
- **D3 Manifest extensions** (`ShaderManifest.{h,cpp}`, parser tests, Inspector, the docs
  generator):
  - `@keywords GROUP "Label" A B C feature=<DEFINE>` emits `GROUP_<VALUE>` only while that feature is
    on and shows the row only then;
  - `@property name choice("Colour", "Ramp darkest", "Selective") 0 "Label"` is a new
    `ShaderPropertyKind::CHOICE`, uploaded as a float index;
  - `@texture u_x ramp_grey` is a new `ShaderTextureDefault::RAMP_GREY`.
- **D4 One dissolve key on CPU and GPU.** `engine/core/pixelart/PixelFxKey.{h,cpp}` and
  `includes/pixel_fx.glsl` implement the same functions:
  ```glsl
  uint pfxHash(uvec2 texel, uint seed);          // PCG2D-style, 32-bit
  float pfxValueNoise(vec2 texel, float cell, uint seed); // bilinear of hashes at whole cells
  float pfxDissolveKey(ivec2 texel, ivec2 frameSize, vec2 dir, float noise, float cell, float jitter, uint seed);
  // Bayer thresholds: S43's includes/bayer.glsl (shared, one table for S43, S48 and S49)
  ```
  The key is evaluated at texel centres (the texel coordinate inside the frame, y up), so it is
  independent of the sprite's screen position, scale and atlas placement. The CPU mirror is
  gtest-checked against GPU-rendered keys (AC5).
- **D5 Pixel outline, inline and shadow sample whole-texel offsets** of the frame-clamped UV
  (`frameInset` stays). Selective outline finds the neighbour's ramp step with one pass over ≤ 32
  texels of the ramp, only for fragments that are outline.
- **D6 Quad growth in `sprite.vert`.** The engine writes a per-draw `spriteParams.frameTexels`
  (frame size in texels, and quad size in object units). The vertex hook moves each corner outward
  by `reach` texels along its corner direction, which is known from its UV relative to the frame
  rect, and grows the UV to match. Sprites cast no shadows (`@state shadows=off`), so no depth
  program has to follow. The CPU bounds (`Sprite`, `MeshComponent` AABB) grow by the same reach
  through the manifest's new `@expand` rule:
  `@expand outlineWidth feature=OUTLINE`, `@expand shadowOffset feature=DROP_SHADOW`,
  `@expand 4 feature=GLOW`.
- **D7 Transitions are an engine overlay**, not a `PostProcessPass` and not an uber bit. They
  cover UI layers and the loading scene, survive scene changes and must cost nothing when idle.
  - `engine/core/pixelart/ScreenTransition.{h,cpp}` holds the controller (one per engine).
  - `engine/shaderlib/transition.frag` with `fullscreen.vert` is a new `ShaderType::TRANSITION`.
  - `Engine::systemDraw` draws it after the scenes loop when active.
  - The kind is a uniform (one program, no variants to collect).
  - Uniforms: `art = (artW, artH, scale, 0)`, `offset`, `progress`, `phase`, `kind`, `shape`,
    `params0..2`, `ink`, the ramp as ≤ 16 vec4 (a uniform array, so no texture binding, and S43
    ramps are read on the CPU).
- **D8 No screen-reading kinds.** Every kind is a function of the art pixel coordinate and time
  that outputs premultiplied `(light + cover·ink, cover)`. CRT Off is that too (`crt-off.js` only
  covers and adds light). A screen copy that includes the layers would cost a full-screen target on
  mobile for every transition.
- **D9 Hit-Stop on the engine clock.** `Engine` gets `hitStopFactor` and a countdown on unscaled
  time, folded into `deltatime` (`Engine.cpp:2186`) and the game scheduler (`:466`). Systems that
  pick a delta per entity (the existing unscaled switches) get a third value, the unfrozen delta,
  for exempt entities. `HitStopExemptComponent` is resolved once per frame into a flag set over the
  hierarchy (`includeChildren`). The Lua `Time.deltaTime` getter reads the flag of the script being
  called (the script system already knows the owner entity).
- **D10 Shake reuses the Camera Shaker.** Pixel snapping and ticks act on the final offset after
  the existing instance maths (`ShakeInstance`, `ShakerRuntime`), so trauma, presets, global shakers
  and transform mode all gain it. `units: pixels` converts through the S41 grid (art px → world).
- **D11 Held poses in the sampler.** `KeyframeSampling` gains a `stepFps` argument that quantises
  per segment (R23). `AnimatorInstance`, `AnimationPlayerRuntime` and `PoseAnimatorSystem` pass it,
  and the root motion path calls the sampler without it.
- **D12 Contact sheets render offscreen.**
  - Isolated mode builds a scratch scene in the editor process (not saved, not undoable) with copies
    of the target's GameObjects, the art grid at scale 1 and a camera framing the target's grown
    bounds plus the margin.
  - For each time it sets: `Engine::setPinnedShaderTime(t)`, animation and particle times through
    their scrub paths (S20 scrub, `AnimationPlayerRuntime` time, `PixelFxPlayer` time), and the
    transition progress.
  - It draws into an RGBA8 framebuffer, reads back, and composes the sheet on the CPU (labels in
    S51's built-in pixel font).
  - Scene mode in Play uses `play {auto_pause}` stepping with `fixed_fps = frames / duration`.
- **D13 Lints live in the family's lint folder.** `editor/pixelart/lint/EffectLints.{h,cpp}` takes
  an `EffectLintInput` (frames of S44 `PixelImage`, their times, the palette, the backdrops (S42's
  `pixel_art.backdrops` when present), `loop`, the target bounds) and returns S44's `LintReport`
  rows (PASS, WARN, FAIL, SKIP). It also holds `off_palette`, `partial_alpha` and `flicker`, S46
  R26's definitions, because S48 lands before S46 and needs them; S46's `lint()` calls them, and S49
  calls the effect lints, so each has one implementation.

**API sketch**

```cpp
// engine/core/pixelart/ScreenTransition.h
namespace jasperengine::pixelart {
    enum class TransitionKind { IRIS, BLINDS, SLASH, PIXEL_DISSOLVE, CRT_OFF, BURN_AWAY, FADE };
    struct TransitionOptions { Vector4 ink{0.0f, 0.0f, 0.0f, 1.0f}; std::string ramp; int rim = 2; float glow = 0.4f;
        int pixelSize = 0; uint32_t seed = 1; Ease ease = Ease::IN_OUT; float hold = -1.0f;
        IrisShape shape = IrisShape::CIRCLE; Vector2 center{0.5f, 0.5f}; Entity centerObject = NULL_ENTITY;
        float angle = 30.0f; int count = 8; int cell = 16; bool vertical = false; bool throughRamp = false; };
    class JASPERENGINE_API ScreenTransition {
    public:
        static TransitionHandle play(TransitionKind, float seconds, const TransitionOptions& = {});
        static TransitionHandle cover(TransitionKind, float seconds, const TransitionOptions& = {});
        static TransitionHandle reveal(TransitionKind, float seconds, const TransitionOptions& = {});
        static TransitionHandle loadScene(const std::string& scene, TransitionKind, float seconds, const TransitionOptions& = {});
        static bool isActive(); static bool isCovered(); static float getCoverage(); static void stop();
        static void update(double unscaledDt);   // Engine::runFrame, after the scenes' update
        static void draw();                      // Engine::systemDraw, when active
        static void setPreviewProgress(float);   // editor and capture_contact_sheet only
    };
    class JASPERENGINE_API HitStop {
    public:
        struct Options { float timeScale = 0.0f; std::vector<Entity> exclude, only; std::string shakePreset;
                         float shakeAmplitudePx = 0.0f, shakeSeconds = 0.0f; };
        static void play(float seconds, const Options& = {});
        static void stop(); static bool isActive(); static float getRemaining();
        static float getFactor(); static double getUnfrozenDeltaTime();
        static bool isExempt(Scene*, Entity);  // the frame's resolved flags
    };
}
```

```lua
-- Lua
Transition.loadScene("Level2", "iris", 0.8, {shape = "diamond", ramp = "ui/palette.palette#ramp:gold"})
local h = Transition.play("crt_off", 1.2); Coroutine.waitFor(h)
HitStop.play(0.08, {shake = "pixel_hit"})            -- freeze 80 ms, shake 2 px meanwhile
HitStop.play(0.12, {only = {self.gameObject, enemy}}) -- only the fighters freeze
CameraShake.kick(3, 0.25)
PixelFX.play(enemy, "dissolve", 0.9, {ramp = "builtin:fire#ramp:fire", embers = "ember"})
PixelFX.play(player, "blink", 1.0, {count = 6, unscaled = true})
PixelFX.glitch(0.3, 0.8)
animator.poseFrameRate = 12
```

### 6.1 MCP tools and the budget

S48 adds one tool to the `pixel_art` group (tools from S42, S43, S46 and S48: 6 tools and ≈ 8,050
of 15,000 characters, `tools/mcp/budget.json` `group_default`, ledger "MCP budget"). S48 claims at
most 2 of the 12 slots and uses 1; the second stays free for the family (S51's `preview_font`
fallback may take it, S51 §6.1).

| Tool | Group | Change | Budget (chars) | Annotations / activity |
|---|---|---|---|---|
| `capture_contact_sheet` | `pixel_art` | new: `target {entity, scene, effect, effect_options, animation, particles, transition, transition_options}`, `frames` (1..64, 8) or `times[]`, `duration`, `loop`, `mode` (isolated, scene), `backdrops[]` (checker, dark, light, sky, ground, black, scene), `scale` (0 = auto, 1..16), `columns`, `labels`, `margin`, `lint` (bool or names), `output`, `path`, `max_width` | ≤ 1,450 | read-only unless `output`; `writesFiles` when `output`; a job above 1 s; `allowedWhilePlaying` true (scene mode needs Play) |
| `apply_material_preset` | core | none: the new presets are data | 0 | — |
| `set_scene_settings` / `get_scene_settings` | core | none: the glitch fields come from the field table | 0 | — |
| guide | — | ≤ 120 bytes in the `## Pixel art` digest section | — | — |

The group row (`McpToolGroups.cpp`) is added by S43 in wave 5A. S48 adds no core
characters. The test asserts `capture_contact_sheet` is hidden until `tool_groups {enable:
["pixel_art"]}` (conventions §Tool groups).

## 7. Acceptance criteria

| AC | Criterion | Verification |
|---|---|---|
| AC1 | Manifest extensions parse, error on bad input (choice without values, keyword `feature=` naming an unknown feature) and resolve: a scoped keyword emits no define with its feature off | gtest `ShaderManifest.ScopedKeywords`, `ShaderManifest.ChoiceProperty`, `ShaderManifest.ExpandRule` (`engine/tests/core/shader/ShaderManifestTest.cpp`) |
| AC2 | Every pre-S48 `library:sprite` feature combination used by the fixtures renders pixel-identical (tolerance 0) to references captured on the merge base; the default OUTLINE (SMOOTH) is unchanged | golden `tools/mcp/tests/test_pixel_fx_golden.py::test_legacy_sprite_unchanged` (references made before the change: the red control) |
| AC3 | Goldens on GL core and Vulkan for each new feature at fixed values: dither fade 0.5 (4x4, 8x8, sprite and screen space), dissolve 0.3/0.6 with ramp, ramp recolour petrify/frozen/poison with bevel, PIXEL4/PIXEL8 outline width 1 and 2, sel-out, inline, drop shadow, the outline on a frame with no transparent margin (grown quad); every output pixel is a ramp or sprite colour (no blends) | `test_pixel_fx_golden.py` (pngdiff tolerance 0 on GL, ≤ 1 % at 24 on Vulkan) |
| AC4 | Hard pixels: in each golden at integer scale k, every k×k block of output pixels for one art pixel is one colour | `test_pixel_fx_golden.py::test_blocks_uniform` |
| AC5 | CPU key equals GPU key: a debug variant writing `pfxDissolveKey` to colour matches `PixelFxKey` on 3 frame sizes, 4 seeds (≤ 1/255) | gtest `PixelFxKey.MatchesShader` (GPU suite) |
| AC6 | Dissolve bands are constant width: along the front the measured edge band is `dissolveEdgeWidth` ±0 texels on 95 % of front columns (noise 0.35) | gtest `PixelFxKey.BandWidth` |
| AC7 | Zero cost: with no new feature, the preprocessed source, reflected uniform block and attribute list of every legacy combination equal the pre-S48 ones | gtest `SpriteVariants.LegacyIdentical` (compiles via `engine/libs/shadercompiler`) |
| AC8 | Every new feature alone and all on compile for all 6 backend targets (GLSL 410, GLSL 300 es, HLSL 5, MSL macOS/iOS, SPIR-V), within 8 samplers and 32 vec4 | gtest `SpriteVariants.CompileForEveryBackend` |
| AC9 | Static tap budget per feature as R33 (count of `OpImageSample*` beyond the base) | gtest `SpriteVariants.TapBudget` |
| AC10 | Idle features skip their body: dissolve progress 0, fade amount 0, ramp 0 render identical to the feature off (tolerance 0) | `test_pixel_fx_golden.py::test_idle_equals_off` |
| AC11 | Instanced effect channel: 64 instanced sprites differing only in `fx.y/z/w` stay one batch and match 64 non-instanced sprites with the same values (tolerance 0); an absent FX stream draws as amount 0 | GPU gtest `SpriteInstancing.FxChannel` (S52's harness; S52 lands in wave 5A, and this adds S48's slot cases to S52 AC7) |
| AC12 | Transitions: for each kind × 3 progress values (0.25, 0.5, 0.9) × 2 pixel sizes (1, 3), a golden; at progress 1 every pixel is ink; at 0 the pass is absent; rim width spread 0 | `tools/mcp/tests/test_transitions.py` (via `capture_contact_sheet {target:{transition}}` and `JASPERENGINE_FRAME_STATS=all` pass lists) |
| AC13 | Transitions in Play: `Transition.loadScene` covers, loads, reveals; `onCovered` fires once before the switch; the HUD layer is covered (pixel check in a layer region); with `transitionsCoverUI` false the HUD stays visible; a loading scene shown after `loadingDelay` is revealed then covered | `test_transitions.py::test_load_scene`, `::test_cover_ui`, `::test_loading_scene` |
| AC14 | Idle transition costs nothing: frame stats pass and draw counts with the system idle equal a build without it, and no transition program exists before first use | `test_transitions.py::test_idle_free` |
| AC15 | Glitch: default tiers unchanged (S13's snapshot test); `glitchEnabled` sets exactly `UBER_GLITCH`; pinned time gives one image; goldens at 3 pinned times; rows shifted by whole source texels | gtest `PostProcessingTiers.GlitchBit`; `tools/mcp/tests/test_post_fx_golden.py::test_glitch` |
| AC16 | Hit-Stop: during `HitStop.play(0.1)` `Time.deltaTime == 0`, physics bodies do not move, timers do not advance, unscaled timers and audio do; after 0.1 s unscaled it resumes; overlapping calls keep the longer; `timeScale 0.25` slows | gtest `HitStop.*` (`engine/tests/core/player/`); MCP `tools/mcp/tests/test_hit_stop.py` (`play fixed_fps 60`, `eval_lua`, `wait`) |
| AC17 | Exemptions: a Hit Stop Exempt GameObject's Animator, Tween, particles and script `Time.deltaTime` advance during a freeze, its children too (`includeChildren`), others do not; `only` freezes just the listed ones | `test_hit_stop.py::test_exempt`, `::test_only` |
| AC18 | Pixel shake: with `pixelSnap` every applied offset over 120 frames is a whole number of art pixels and rotation is 0; with `tickRate 30` at 60 fps the offset changes on at most every other frame; `kick` matches the `act.js` formula for 10 sampled ticks; old `.shake` files and shaker fields load unchanged | gtest `ShakeInstance.Kick`, `CameraShaker.PixelSnap`; extend `tools/mcp/tests/test_camera_shake.py` |
| AC19 | Held poses: `poseFrameRate 12` on a 1 s clip with keys at 0, 0.3, 0.7 shows each key's pose exactly at its key time, changes pose ≤ 12 + 3 times a second, root motion delta per frame is continuous (no step), events fire at true times | gtest `KeyframeSampling.StepFps`, `AnimatorInstance.PoseFrameRateRootMotion` |
| AC20 | `PixelFX`: each effect runs over its duration and ends at the neutral value; `Coroutine.waitFor(handle)` resumes once; a material without the feature gets it at runtime on the object's own material only (the shared `.material` file unchanged) and logs once | `tools/mcp/tests/test_pixel_fx_lua.py` |
| AC21 | Embers: dissolve with embers emits particles only on texels that crossed the front this frame (positions checked against `PixelFxKey`) | gtest `PixelFxPlayer.EmbersOnFront` (S49's emitter mocked until S49 lands) |
| AC22 | `capture_contact_sheet`: grid layout, labels, integer scale, backdrop rows; two identical calls give identical PNG bytes; `output:true` writes the file in one undo step; hidden until `pixel_art` is enabled | `tools/mcp/tests/test_contact_sheet.py` |
| AC23 | Lints on known fixtures: a sprite whose dissolve leaves a texel at progress 1 (`one_shot_end` WARN), a particle burst spilling 30 px (`spill` WARN with spans), a loop with a jump (`loop_seam` WARN), an unseeded random script (`determinism` FAIL), a 1-frame blink (`flicker`), a clean fixture (all PASS, verdict `clean`); each red fixture's fixed twin passes (red control) | `test_contact_sheet.py::test_lints` with `tools/fixtures/pixel-fx/` |
| AC24 | Editor: feature thumbnails appear and update; the scrub preview never dirties the scene; Pixel FX presets apply as one undo step; Glitch rows; Game view Transition and Contact Sheet buttons work; screenshots from an isolated editor | `tools/mcp/tests/test_pixel_fx_inspector_ui.py` (`find_ui_item`, `screenshot`), screenshots attached to the PR |
| AC25 | Mobile budgets R34 | Manual on an Adreno 610 / Mali-G57-class device with S15 timings or a vendor profiler; numbers recorded in `docs/mcp/guide/pixel-fx.md` |
| AC26 | Exported game: a fixture with every feature, a scripted `Transition.loadScene`, `HitStop.play`, `PixelFX.play` and glitch burst runs on Linux GL, Vulkan, GLES3 and web; screenshots at scheduled frames match the editor's (tolerance 24, 1 %) | `tools/smoke-test.sh tools/fixtures/pixel-fx` (+ `SMOKE_BACKEND=vulkan|gles3`, `tools/build.sh web-gate`) |
| AC27 | Old projects: every `tools/fixtures/*` scene, material and `.shake` re-saves byte-identically after open and save | `test_pixel_fx_golden.py::test_resave_identical` |

## 8. Dependencies & conflicts

- **Depends on:**
  - S43:
    - ramp strips `<palette>.palette#ramp:<name>` (S43 R4a: N×1 RGBA8, Nearest; a missing ramp
      binds white and warns `palette_ramp_missing`);
    - the built-in palettes' ramps (`builtin:<palette>#ramp:<name>`, from pixelfx's 27 ramps);
    - `rampColors`/`fitRamp`/`rampAt` on the CPU for transition rims;
    - `includes/bayer.glsl`;
    - PALETTE_SWAP's place first in the order.

    S43 lands in wave 5A, before S48; the ramp slot also takes plain PNG strips (R9).
  - S41: `engine/core/pixelart/PixelSnap.h` (`snap`, `SnapGrid`, `snapWorld`) and the frame block's
    `pixelSnap` (xy base target size, w screen px per base px) in `includes/pixel_snap.glsl` (S41
    §6.2).
    - Transitions take the art pixel from it.
    - The shake rounds through `snapWorld`.
    - The glitch works in the uber pass's source texels, which are S41's base pixels.

    S41 lands in wave 5A. Without an active Pixel Perfect Camera, transitions use `pixel_size` and
    the shake snaps to the camera's pixels.
  - S44 (wave 5A): `lint/LintTypes.h` and `PixelProblem` errors (D13). S48 needs nothing from S46
    (wave 5C), which reuses S48's three rule lints.
  - S52 (wave 5A): the `i_fx` stream, `instance=` and `@instanceable` (R10).
  - S51 (same wave): the built-in pixel font for labels. Soft: until it lands labels use a 3×5
    digits glyph set embedded in the tool.
  - S42 (same wave, soft): Pixel Art Mode for R38's PIXEL4 default, `pixel_art.backdrops`.
  - S32: asynchronous variants for R25.
  - S13: the uber pass conventions and `getScriptableEffectFields`.
  - S15: GPU timers for `frame_cost` (soft).
  - S20: the particle scrub path (contact sheets).
- **Feeds:**
  - S49: the ember front (`PixelFxKey`, `PixelFxPlayer::frontTexels`); S49 provides the emitter
    entry point `PixelParticles::emitAt(preset, positions)`.
  - S52: S48 declares DITHER_FADE's and DISSOLVE's amounts with `instance=fx.z|w` (S52 R22-R23);
    S52 itself declares FLASH's `instance=fx.y` and marks `library:sprite` `@instanceable` (S52 M3,
    wave 5A). Every other S48 property is material-wide, so it is part of S52's batch key. A sprite whose effect needs more per-instance values (none today) falls
    back per S52 R18.
  - S42: the variant count warning (D2), S42's `sprite_variants` rule, and the "OUTLINE picks
    PIXEL4" rule.
  - S46 (wave 5C): `off_palette`, `partial_alpha`, `flicker` in `EffectLints`.
- **Merge hotspots:**
  - `engine/shaderlib/library/sprite.{frag,vert}` (S43's PALETTE_SWAP, S52's instancing): S43 and
    S52 land first (wave 5A, either order), then S48; keep the feature list order of R8.
  - `engine/core/shader/ShaderManifest.{h,cpp}` (S43 may add a palette texture default).
  - `engine/core/subsystem/RenderSystem.{h,cpp}` and `uber.frag` (S41 integer upscale bits).
  - `engine/core/Engine.cpp` (clock, draw loop).
  - `CameraShakerComponent.h`, `ShakePreset.cpp`, `ShakePresets.cpp`.
  - `KeyframeSampling.cpp`, `AnimatorInstance.cpp`.
  - `editor/window/InspectorShaderLibrary.cpp`, `InspectorPostProcessing.cpp`, `GameWindow.cpp`.
  - `editor/material/MaterialPresets.cpp`, `editor/shader/ShaderVariantCollector.cpp`.
  - `editor/mcp/McpToolGroups.cpp`, `tools/mcp/budget.json` (the family's group).
  - The Lua binding files and `NOTICE.md`.

## 9. Risks & defaults

| Risk / question | Default decision |
|---|---|
| Runtime-enabled features (R25) cause a compile hitch in shipped games | Development-build warning naming the material; the S32 warm-up list records them; presets turn the features on at authoring time. Documented as the recommended path |
| Texel-space dither fade shows a different pattern than pixelfx's screen-space one | Sprite space is the default because it is stable under motion and works without S41; screen space is one choice away and matches pixelfx exactly on the S41 grid |
| The grown quad changes sorting or picking for outlined sprites | Bounds grow by whole texels only; the Scene view selection outline uses the grown bounds; AC3's no-margin golden covers it |
| The transition overlay covering the HUD hides a "Loading…" label drawn on the HUD | The loading scene path (R15) reveals onto the loading scene; `transitionsCoverUI` exists for HUD-driven loaders |
| Hit-stop's context-dependent `Time.deltaTime` surprises script authors | Only inside Hit Stop Exempt GameObjects, documented in the component tooltip and `jasper://docs/pixel-fx`; `HitStop.unfrozenDeltaTime` is explicit |
| Rigidbodies cannot be exempted or frozen per object | Documented; the common pixel-art character (kinematic or character controller driven by scripts) follows its script's delta |
| Selective outline is costly on large outlined areas | Only outline fragments do the ramp pass (≤ 32 taps of an N×1 texture, cache-friendly); the default is Ramp darkest |
| The uber bit number collides with S41 | Bits are taken at merge time, append-only; the name `Gli` is fixed |
| Held poses look wrong on blended state transitions | Weights step on the same ticks (R23); `poseFrameRate 0` is the default |
| The S43/S41/S44/S52 interfaces change while they are specified in parallel | They land in wave 5A, before S48; §8 lists the soft fallbacks for S49, S51 and S42 of the same wave; a deviation is recorded if an interface moves |

## Milestones

1. **M1 Sprite features.** Manifest extensions; DITHER_FADE, DISSOLVE, RAMP_RECOLOR, pixel
   OUTLINE/inline/sel-out, DROP_SHADOW; grown quad; ramp slot; Inspector thumbnails and scrub; Pixel FX
   presets; AC1-AC10, AC27.
2. **M2 Game feel.** Hit-Stop and Hit Stop Exempt; pixel snap, tick and `kick` on the Camera Shaker;
   Pose Frame Rate; Lua; AC16-AC19.
3. **M3 Transitions and glitch.** The overlay controller and shader; `Transition` Lua and
   `loadScene`; Project Settings; Game view preview; `UBER_GLITCH`; AC12-AC15.
4. **M4 `PixelFX` player.** The effect channel (non-instanced first, S52's `i_fx` slots); embers
   with S49; AC11, AC20, AC21.
5. **M5 Agent tooling.** `capture_contact_sheet`, `EffectLints`, the Contact Sheet dialog, docs and
   the guide line; exported-game smoke; AC22-AC26.

## Deviations

Implementation notes that change what the spec says. Each item names the requirement it touches.

### Whole spec
- **Guide.** Per the ledger's upstream MCP rules, the agent guide gets one "Other areas" entry, `pixel-fx
  (capture_contact_sheet)` with `jasper://docs/pixel-fx`, instead of a line in a `## Pixel art` digest section (R46).
  Everything else is in `docs/mcp/guide/pixel-fx.md`; user docs are `docs/pixel-fx.md`.
- **S52 names (R10, R30, R31).** S52 calls the FX stream `a_spriteFx` / `i_spriteFx` (FLOAT4, read through
  `spriteFx(slot)` from `includes/sprite_fx.glsl`) and the manifest line `// @batchable framerect=...`, not `i_fx`
  (HALF4) and `@instanceable`. `ditherFadeAmount` is `instance=fx.z` and `dissolveProgress` `instance=fx.w`; the
  Pixel FX features that read the frame rect (pixel outlines, Drop Shadow, Dissolve, Ramp Recolour, Dither Fade) are in
  `@batchable framerect=`. AC11 is `PixelArtBatchGpu.FxSlotsDitherFadeDissolve` beside S52's `FxSlotsZW`.
- **Shader cache.** S48's sources are cached under v64 (S49 took v60, S52 v61, the terrain programme v63).
- **Mobile budgets (R34, AC25).** No Adreno 610 / Mali-G57-class device was available: the phone numbers are not
  measured. R35's CPU costs are inferred from the code (idle paths return at once, no draw, upload or allocation).
- **Test names.** GPU gtests end in `Gpu` (docs/testing.md), so the spec's GPU test names take that suffix.

### Sprite features (M1)
- **D6 / R7, quad growth in the geometry.** The sprite's quad grows in `MeshSystem`'s CPU geometry (createSprite, buildSpriteGrid for sheet frames and lattice grids, the 9-slice in buildSpriteSliced), driven by the manifest's `@expand` rules, not by a `sprite.vert` hook with a `spriteParams.frameTexels` uniform (S52 D6 requires geometry growth for its batches). Culling, picking and sorting use the grown bounds with no extra code. The quad rebuilds when the material's features or values change: `MeshSystem::update` compares `SpriteComponent::builtFxExpand` when `customUniformWrites` moved since the last compare (a write the draw already flushed, from a coroutine or `onPostUpdate`) or while `needUpdateShaderUniforms` or `needReload` is set, and the shader is the one `getSubmeshShader` gives, so a `customShader` or the scene's default shader grows the quad too. Growth applies only to the pixel outline styles (`@expand outlineWidth feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8`) and Drop Shadow (`@expand shadowOffset feature=DROP_SHADOW`). The smooth OUTLINE and GLOW keep today's quad, because R1, AC2 and AC7 require the legacy variants to stay identical in source and pixels, and GLOW reads no neighbour, so a grown quad would add only transparent pixels. There is no `@expand 4 feature=GLOW`. Texels beyond the frame read transparent (`SPRITE_FX_GROWN`).
- **D3, scoped keywords.** `@keywords GROUP … feature=<DEFINE>` defines nothing while its feature is off. Its first value (`OUTLINE_STYLE_SMOOTH`) also defines nothing while the feature is on, so materials made before the group keep their variant name and source (`library:sprite#…,OUTLINE`, AC27). `ShaderManifest::getKeywordChoice` gives the choice the Inspector shows. Only a `@feature` may scope a group.
- **R5, pixel-only outline options.** Outline Colour From, Ramp Steps, Inline Width and Inline Colour exist on the pixel styles only (rows `feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8`). The smooth outline is unchanged. The manifest's `feature=` on properties and textures takes a comma list, meaning shown while one of them is on.
- **R3 / D4, the dissolve key.** With a direction, the key is the texel's height along the direction, displaced by value noise of up to 0.75 × noise × cell texels. The noise is scaled by cell over the frame's extent, so up to a noise of about 0.6 the key keeps rising along the direction. A jitter is shared by each line of texels along the direction, giving a ragged front of straight columns. Without a direction ((0, 0)) the key is the noise alone plus a per-texel jitter. All keys are normalised into [0, 1). With a direction, the bands are exact whole texels along it: a texel is in the edge band when the texel `edge` steps back toward the front is burnt. This costs two extra key evaluations and needs no derivative. AC6 measures 10,359 of 10,359 front columns exact. Without a direction, the bands use pixelfx's field rule: the key difference over the gradient, clamped to 0.45 to 3 times a straight front's gradient. The shader cost is above R33's "≤ 40 ALU" estimate: about 12 hashes in the bands.
- **R4, ramp recolour details.** `rampLow` and `rampHigh` are floats, where a negative value counts from the end (defaults 1 and −1), because N comes from the texture. `rampBevel` is a dithered per-texel choice (Bayer 4x4 below the value), the same as `rampAmount`. For outline, inline, shadow casters and the bevel, "opaque" means alpha ≥ 0.5.
- **R11, FLASH.** FLASH is a legacy feature (R1): a FLASH drawn alone, unbatched, keeps its pre-S48 source (`SpriteVariants.LegacyIdentical`), so the idle branch applies where the variant has S48's structure (another Pixel FX feature's `SPRITE_FX`) or S52's batched slots (`HAS_SPRITE_FX`).
- **R2, alpha and screen space.** A fragment the dither keeps keeps its own alpha: the effect makes no partial alpha but does not change the art's. Screen space uses `gl_FragCoord`, with S41 divided by `frame.pixelSnap.w` (carried as a flat varying from the vertex stage) and without S41 as it is. Both read `gl_FragCoord`'s vertical origin, which is the backend's, so the screen-space pattern can mirror vertically between GL and D3D/Metal/Vulkan on the S41 path as well. Sprite space, the default, is identical on every backend.
- **AC2–AC5, AC9, AC10 test names and counting.** GPU suites must end in `Gpu` (docs/testing.md), so AC2, AC3, AC4, AC5 and AC10 are core GPU gtests `SpritePixelFxGpu.{LegacyCombinationsUnchanged, NewFeatures, LegacyBlocksUniform, KeyMatchesCpu, IdleEqualsOff}` with `GrownQuad` (R7). They render at an exact 4× scale into the headless target; the goldens are in `engine/tests/core/golden/sprite_fx/`. `test_pixel_fx_golden.py` holds AC27. AC9 counts texture reads (OpImageSample*, OpImageFetch, OpImageGather) statically in the Vulkan SPIR-V. Functions are not inlined, so the count is a lower bound on the per-fragment taps.
- **AC27, re-save test.** The fixtures predate `main`'s current scene format (format stamp, guid, newer keys: main's to migrate), so a byte comparison with the fixture is not S48's. The test saves each `shader-library-export` scene and two fixture `.material` files twice. It checks that the second save is byte-identical, that no S48 name appears in a file that does not use it, and that the legacy OUTLINE variant keeps its pre-S48 name.
- **R38 / D2, S42 parts.** "Pixel Art Mode picks PIXEL4" is `PixelArtMode::pickPixelOutline`, called when the
  Inspector ticks Outline (same undo step). The 24-variant export warning is S42's `sprite_variants` rule, which counts
  every distinct `library:sprite` variant (S48's features included) and names the materials that add the most.
- **R38, presets.**
    - Presets set `shaderParams`.
    - The animated amounts are left at 0, idle until played, in Hit Flash, Teleport Fade, Burn Away and Enemy.
    - The Enemy preset's id is `pixel_enemy`.
    - Built-in ramps are named `builtin:<id>#ramp:<name>` and resolved to the project's copy of that palette, written if missing: `editor::resolvePresetPalettes` with S43's `usePalette` and `linkPaletteCopy`. The copy is written in the same undo step as the edit.
    - Thumbnails show beside every feature row of a Sprite's material (Sprites only, not tilemaps or polygons).
    - The Preview row covers the four animated amounts, in edit mode only.
    - Guide text: the area's section is in `docs/mcp/guide/pixel-fx.md`. The README row and the "Other areas" entry are left to the landing.

### Screen transitions and Glitch (M3)
- R13/AC12 rim width: the rim is exactly `rim` art pixels across every row and column it crosses.
  A covered pixel is rim at depth s when the pixel s steps along a row or column is uncovered (an axis
  dilation of the uncovered side). A per-kind bound on how fast the field changes per art pixel (1 for
  iris, blinds and slash, 2.2 for burn away) lets pixels deeper than `rim` steps skip the walk. The §4
  gradient rule (`1/clamp(|grad f|, 0.45, 3)`, by finite differences, no dFdx) only places the glow
  outside the edge: with the gradient rule alone, a noise field (burn away) gave rims of varying width.
- R45: `Transition` and `PixelFX` are LuaBridge classes with static members (`beginClass<Tag>`), not
  namespaces. tools/lua_api's docs and doctest see only classes. `PixelFX`'s tag is
  `pixelart::PixelFxLua` (engine/core/pixelart/PixelFxLua.h): every file adding to PixelFX reopens it.
  `TransitionKind` is an integer enum (IRIS..FADE = 0..6); names ("iris") are accepted too.
- R12/R13: the iris `center` is normalized over the screen from its top-left (x right, y down). A
  Vector4 `color` is read as an sRGB display colour, as picked; `"#rrggbb"` works as well.
- R18: `glitchEnabled` alone sets `UBER_GLITCH`, also at intensity 0, which keeps the variant for
  scripted bursts (PixelFX.glitch) only. The corruption of each frame (split, interlace, up to 8 tears)
  is picked on the CPU (engine/core/pixelart/Glitch.cpp, a pure function of floor(time·glitchFps) and
  the seed) and uploaded as 3 vec4 appended to u_fs_uberParams. Interlace darkens odd source rows by 0.7.
- AC12: the GPU images per kind × progress × pixel size are compared against the CPU twin (ScreenTransition::evaluate;
  ScreenTransitionGpu.MatchesTheCpuTwinWithHardPixels, glcore and Vulkan). The stored PNG goldens are the contact-sheet route
  (`tools/mcp/tests/golden/transitions/`, `test_transitions.py::test_contact_sheet_route`).
- R40: in edit mode the Engine's own transition draw is off (the Scene view must not show it). The
  Game view's preview draws inside Engine::drawView for the view ScreenTransition::setViewDrawId names
  (GAME_VIEW_ID), and through the Engine frame of the Game view pass. Cover UI does not apply to the
  edit-mode preview: it draws over the layers.
- R13 ramp: at runtime a ramp is a `.palette` asset's ramp ("<palette>.palette#ramp:<name>", or
  `palette` + `ramp`). `builtin:` palettes exist only in the editor (templates), so the default is the
  built-in ember ramp embedded in the engine; a missing ramp falls back to it with a warning
  (palette_ramp_missing). Ramps are uploaded as display sRGB, at most 16 steps (fitRamp).
- R16 on Vulkan: the transition is one more pass over the frame, so it composites offscreen
  (`Engine::beginCompositeFramebuffer`). A game whose build ships the transition's program bakes its objects' offscreen
  pipelines from load (`Engine::setCompositeExpected`, `getScenePipelines`): the first composite frame draws them at once
  and nothing reloads when a transition starts or ends. Vulkan only; the other backends draw it to the swapchain.
- R15: SceneManager gained `isLoadingSceneShown()` and `isLoadComplete()`. If the load completes before
  the reveal onto the loading scene reaches 0, the second cover starts from the current coverage.
- R14: `play()` with `hold` takes the hold out of `duration`: cover = reveal = (duration − hold) / 2.
- Not measured: R34/AC25 mobile budgets (no device). R35 CPU costs are inferred (idle update returns at
  once; no draw, upload or allocation while idle).

### Game feel (M2)
- `.shake` also gains a `tick` key (Hz), needed by `pixel_rumble`'s "tick 30".
- A kick's length is its `fade_out`.
- A hit-stop takes effect from the next frame.
- `exclude` lists of overlapping calls of the same kind add up; an `only` call replaces a world hit's lists (and the reverse).
- An Action or Tween follows the exemption of the GameObject it moves (its target).
- An Animator in Animate Physics mode follows the physics world.
- The script scope covers Lua engine events, script messages and a GameObject's Lua coroutines and `self:invoke` timers (their game-time waits follow the unfrozen delta for exempt GameObjects, and freeze for a hit's `only` targets). The global `Timer.after` and a C++ script's `ScriptBase::invoke` have no GameObject and keep the game clock; a C++ script's own `Engine::onUpdate` subscriptions call `HitStop::deltaFor`.
- Pose Animator steps count from each transition's start and arrival.
- Animation Player fade weights step on the player's clock.
- On the retargeted humanoid path the hips position stays smooth.
- AC suites: engine/tests/core/player/HitStopTest.cpp, engine/tests/core/camera/CameraShakePixelTest.cpp, engine/tests/core/animation/PoseFrameRateTest.cpp (AnimatorInstance.*).

### Agent tooling (M5)
- **Check rows (coordinator, S91 consistency).** The lints compute S44's `lint::LintResult` (what S46 and S49 call), and
  `capture_contact_sheet` returns them as the verdict tools' rows (`util/QaChecks.h`, S35 R21 / S91 D5, the rows
  `preview_particle_effect` returns), through `lint::toCheckRow`: `id`, `kind` `effect`, `status`, `round`,
  `measured`, `threshold`, `threshold_source`, `limited_by` (the LintResult message), `at` (the spotted frames'
  times, seconds), `fix_hint` (the fix), `method` `capture`, plus S44's `count` and its pixel spots as `spots
  [[frame, x, y]]` (S44's `at`). The array is `checks` (S91's name), not R27's `lints`; S35's anti-gaming rounds
  (`qa::FailRounds`, 2 fix rounds), `thresholds {values, changed}` and `qa::guidance` (in `next`) come with them.
  `verdict` stays the family's `clean | fixable | needs_person` (ledger, R28); the S35 roll-up is `check_status`,
  since S91's `status` key holds the job status.
- **Shared machinery.** The offscreen draw and read-back of S91's ParticleEffectPreview moved to
  `editor/render/OffscreenDraw.{h,cpp}` (`drawOffscreen`), which both ParticleEffectPreview and S48's
  EffectCapture call; each band of the sheet is a `PoseContactSheet` (the sheet S91's filmstrips draw), so labels,
  strips and separators match. EffectCapture keeps its own scratch-scene stepping (whole GameObjects, not only
  particle systems) and S91's frame pump pattern (processFrame from App::engineRender).
- **Scene-mode stepping.** In Play the paused game steps at a fixed 1/60 s (Project::step, the play tool's auto_pause
  hold lifted for the capture through `w3::GameAdvance`) and a frame is read at the step nearest each time, instead of
  `fixed_fps = frames / duration` (D12): the game simulates at its normal rate whatever the frame count. Its
  `determinism` fresh captures load the played scene again (SceneManager.loadScene of the current scene) twice.
  Scene mode stopped copies the scene into a scratch scene (main scene only; layer scenes are drawn in Play).
- **Isolated mode runs no scripts** (the Lua state is the editor's); a script-driven effect is checked in scene mode
  in Play. Seeds: a Particle System with seed 0 takes the run's seed; spill's runs vary explicit seeds too. Unscaled
  particles run on the capture's one clock.
- **frame_cost.** The CPU time is the median of 9 frames shown and 9 hidden (isolated, release builds); the static taps
  are below (R28 frame_cost); GPU time (S15) is not measured by the capture.
- **transition_edges.** A transition's capture takes the rim's width from its CPU evaluation (`ScreenTransition::measureRim`: the
  run along each row or column away from the uncovered side, the rule the shader draws by, in art pixels at any `pixel_size`),
  because a colour image cannot tell which axis a rim pixel was measured along. Pixel Dissolve and Fade have no continuous
  edge and report SKIP. `lintTransitionEdges` on bare images (mean rim run times the cosine of the fitted edge slope, over 7
  lines) stays for callers with frames only.
- **Contact Sheet... is a window**, not a modal (no EditorModal needed); Save As... uses the editor's file picker.
- **Guide.** One "Other areas" entry `pixel-fx (capture_contact_sheet)` (ledger rule, no digest line).

### PixelFX player and the contact sheet's effect and transition targets (M4, M5)
- R24 end values: every effect ends with its animated value at the neutral 0 except the one-way `fade_out` and
  `dissolve`, which end at 1 (the sprite gone, as asked); `stop()` puts the value back at 0 and does not call
  `onFinished`. `outline_pulse` pulses whole on/off (never a partial alpha) and ends hidden when it turned OUTLINE on
  itself, else at the material's own outline colour; it turns on OUTLINE with `OUTLINE_STYLE_PIXEL4`.
- R24 one effect per feature: a new effect of the same library:sprite feature on a GameObject replaces the running
  one (no `onFinished`); effects of different features run together. `PixelFX.set(go, name, value)` takes a channel
  (`flash`, `fade`, `dissolve`, `ramp`) or any library:sprite property, stops that feature's effect and turns the
  feature on. `PixelFX.activeCount` added (the effects still playing).
- R24 status looks: petrify/freeze set Ramp Bevel 1, poison Bevel 0.5 and Pulse 2 (as the presets); in, held
  (`opts.hold`, default half the duration), out over the rest. Ramps: `opts.ramp`, else the material's ramp, else the
  grey default: `builtin:` palettes exist only in the editor, so the runtime cannot name the presets' built-in ramps
  (the presets copy them into the project at authoring time).
- R25: a Sprite whose material has no shader takes library:sprite with the feature; a material with another shader is
  refused with the fix (give it a Sprite or a Pixel FX preset). The log line is the spec's, once per material (its
  `.material` path, else `"<GameObject>"'s material`) per Play session, in development builds (the editor's Play is
  one). The effect waits for `mesh.loaded && !needReload && !needVariantReload` (S32's stand-in variant counts as not
  ready) and counts from zero the frame it is ready.
- R26 embers: `opts.embers` names a Pixel Particles preset ("ember" is `pixel_ember`), which the editor resolves to the
  preset's effect for the project's Pixels Per Unit (passed inline to emitAt), or a `.particles` path, which an
  exported game needs (S49's presets live in the editor). Embers come from the texels whose cached key lies between the
  burn fronts of the frame's two progresses (`pfxDissolveBurnFront`, the shader's `fxBurnt`), opaque texels only
  (alpha >= 0.5 from the texture's pixels in memory, else its file read once per texture and rect; a compressed or
  unreadable texture counts every texel opaque), at most `emberRate` a second, spread evenly over the crossing texels.
  Only Sprites emit (tilemaps and polygons have no frame).
- R27 contact sheet targets `target {entity, effect, effect_options}` (`effect_options`:
  duration, ease, color, count, ramp, hold; the effect's time is the capture's, no embers) and
  `target {transition, transition_options}` (Transition.play's options in snake_case plus `phase` play/cover/reveal
  and `width`/`height` art pixels, default 160 x 90; drawn by ScreenTransition::renderInto over transparent black at
  pixel_size screen pixels per art pixel, 0 meaning 1 since no Pixel Perfect Camera draws a capture). A transition
  reports `spill` SKIP (it draws over the whole screen) and `frame_cost` SKIP (its cost is its pass). `drawOffscreen`
  gained an `after` hook (draw over the target before the read-back).
- R27 labels: the sheet's titles and frame labels are drawn in S51's built-in pixel font (`builtin:pixel`, 6 x 9
  cells) instead of PoseImage's 6 x 10 font; the strips keep PoseContactSheet's geometry.
- R28 frame_cost: the taps are the copies' library:sprite features' texture reads beyond the base sprite, counted in
  the Vulkan SPIR-V by the same counter as SpriteVariants.TapBudget (moved to editor/shader/SpriteTaps.{h,cpp}); GPU
  time stays unmeasured (no S15 timer in the capture). `hit_stop_length`: in a Play scene-mode capture the longest
  HitStop.play requested during it (HitStop's stats reset at the capture's start; 0 when none); elsewhere SKIP.
- S49 R38 hand-off: preview_particle_effect's S48 rows run S48's EffectLints on the pixel view's cells (base pixels,
  premultiplied from the draws on black and white): off_palette SKIP (a particle effect names no palette),
  partial_alpha (SKIP for additive emitters), spill (the emitters' origin plus 24 base pixels, one seed),
  one_shot_end (SKIP unless a cell sits at the run's end), loop_seam, determinism SKIP (a preview is one run; its
  state_hash compares runs). The rows keep S49's kind `content`.
- AC23 dissolve fixture: library:sprite's DISSOLVE at progress 1 always hides every texel, so the red fixture is a
  dissolve still burning when the capture ends (`tools/fixtures/pixel-fx/effects/dissolve_late.json`, 1.25 s captured
  for 1 s); its twin reaches 1 at the end.
