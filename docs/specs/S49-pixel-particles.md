# S49 — Pixel Particles: the pixel grid, palette-ramp colour, pixel shapes, dithered fade, pixelfx presets, Analytic simulation

**Wave / Depends on.** Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), spec-first, wave 5B. It
extends S20's particle system, which is merged (`engine/core/particles/`).

Dependencies:
- **S43** (wave 5A): palette ramps, `rampAt`/`fitRamp`, `includes/bayer.glsl` (the family's one
  Bayer table), `builtin:` names. A ramp may also be an inline list of hex colours.
- **S41** (wave 5A): the snap rule `pixelart::snap`, `includes/pixel_snap.glsl` and Pixels Per
  Unit. Without an active Pixel Perfect Camera the grid comes from the camera's orthographic size
  and the project PPU (R2).
- **S44** (wave 5A): the lint result types (`lint/LintTypes.h`) for the capture checks.
- **S48** (same wave): the effect lints `spill`, `one_shot_end`, `loop_seam`, `determinism` and the
  rule lints `off_palette`, `partial_alpha` in `editor/pixelart/lint/EffectLints` (S48 D13); until
  S48 lands those checks report `SKIP` ("needs S48"), and AC11's assertions on them run once it has.
- **S45** (same wave, soft): the json-hash import that reads Bake to Sprite Sheet's JSON (S45 R10).

Owner area:
- `engine/core/particles/` (the Pixel Art module, the Ramp gradient mode, pixel packing, the
  Analytic program and its CPU twin);
- `engine/shaderlib/includes/particles_pixel.glsl` and `particles_analytic_vs.glsl` (the pixel
  and analytic parts of the particle stages only);
- `editor/pixelart/particles/` (presets, capture checks);
- the particle inspector and overlay (pixel parts only);
- `editor/mcp/tools/ParticleTools.cpp` (the extensions);
- the `docs/mcp/guide/particles.md` Pixel section and `docs/particles-pixel.md`.

## 1. Goal

Particles in a pixel-art game must look drawn, not rendered. Today a 2D particle is a soft,
blended, sub-pixel quad: it lands between pixels, fades through colours no palette has, and
scales by any amount. This spec adds a **Pixel Art** module and a **Pixel** render mode to S20's
particle system:

- particles sit on S41's pixel grid at whole-pixel sizes;
- colour steps along an S43 palette ramp with no blending;
- the shapes (dots, crosses, discs, rings, speed-stretched streaks, smoke puffs, tumbling
  shards, shaded chunks, banded light flashes) are drawn pixel by pixel in the shader, with no
  textures;
- fading uses a Bayer dither instead of alpha;
- motion can be held to a drawn frame rate.

Seven presets carry pixelfx's tuned constants (Spark, Smoke, Fire→Smoke, Ember, Grit, Debris,
Light Flash), plus two seamless ambient loops.

An optional **Analytic** simulation mode computes each particle in the vertex shader from its
slot and the time, with no state: closed-form drag, stateless hashing, exact seamless loops,
instant and exact scrubbing, and no CPU cost per particle. Agents get contact sheets and
pixel-art checks of an effect through the existing `particles` tools.

## 2. Scope

**In scope**
- **The Pixel Art module** (`ModuleId::PixelArt`, appended):
  - snap to the pixel grid, sizes in pixels;
  - fade (Dither, Cutout or Alpha) with Fade over Lifetime, Bayer size and space;
  - Hold Frames (a drawn frame rate);
  - the secondary colour;
  - Flicker (a blink near the end of life);
  - Wobble (a closed-form render offset).
- **The Pixel render mode** (`RenderMode::Pixel`, appended) with nine pixel shapes, an optional
  4-byte pixel instance stream, the engine variant bit `Pixel`, and `includes/particles_pixel.glsl`.
- **A Ramp mode for `MinMaxGradient`** (`GradientValueMode::Ramp`, appended): stepped colour from
  an S43 ramp (`<palette>.palette#ramp:<name>`), with steps, reverse and bias. It works in Start
  Color, Color over Lifetime, Color by Speed and the trail gradients.
- **Collision `settleSpeed`:** a hit below this speed makes the particle rest (debris that
  settles).
- **The Analytic simulation mode:**
  - a document field;
  - a closed-form vertex program for a stated module subset;
  - a shared slot-index stream;
  - a CPU twin, `AnalyticParticles`, for bounds, counts, picking and tests;
  - exact periodic loops;
  - validation diagnostics for unsupported modules.
- **Presets** (data files with pixelfx's constants): pixel_spark, pixel_smoke, pixel_fire (with
  a smoke sub-emitter), pixel_ember, pixel_grit, pixel_debris, pixel_light_flash, plus the
  Analytic loops pixel_smoke_loop and pixel_ember_loop.
- **Editor:** inspector fields and shape previews, the Simulation Mode switch, the Scene-view
  overlay (exact scrub, pixel grid), Bake to Sprite Sheet, and GameObject and Create menus.
- **MCP:** extensions to `create_particle_effect` (presets), `add_particle_module` (the module
  name) and `capture_particles` (contact sheets, integer zoom, pixel checks). No new tool.
- **Lua:** enums, `ps.pixelArt`, `ps.simulationMode`, and `setRamp` on gradient views.

**Out of scope (decided)**
- **Collisions, forces from fields, noise, sub-emitters, trails, events and sorting in Analytic
  mode.** These need per-particle history (§6.3 D3). The Simulated mode keeps all of S20.
- **GPU simulation with state** (ping-pong, transform feedback, compute). S20 §2 ruled it out,
  and Analytic is not that: it reads no previous frame and writes nothing back (D2).
- **pixelfx's Painter API and its separate additive "light" buffer** (`painter.js`). Glow
  shapes draw in the particle's own blend mode.
- **pixelfx's BRAND and Blazes** (burning chunks that start fires where they land, `sim.js:84-96`,
  `242-276`). The docs give the recipe with S20's collision sub-emitters (Debris → Fire on
  Collision). It is not a preset.
- **The other pixelfx kinds** (MOTE, STREAK, FLAKE, VAPOUR, DUST, STAR). The shapes cover them;
  they are not presets.
- **Rendering pixel particles through S52's Sprite Batch.** Particles are already instanced (one
  draw per emitter, S20 R27) and take part in S52's sort (S52 M6) as they are.
- **Pixel-perfect 3D particles.** In a perspective camera, Pixel mode draws shapes at their
  size in screen pixels at the projected centre, with no world-grid snap (R4).

## 3. Current state (read on `specs/pixel-art` 1b1c34fbd)

- **The document.**
  - `engine/core/particles/ParticleEffect.h:26`: `kEffectSchemaVersion = 1`.
  - `ParticleEffectDocument` (`:535-586`) holds `tickRate = 30` (`:543`, 0 = a variable step),
    `interpolate = true`, `maxParticles = 1000`, `seed`, then the module structs (`:560-582`),
    `subEmitters` and `exposed`.
  - `ModuleId` (`ParticleModules.h:25-49`) ends `Renderer = 21, SetOverAge = 22, Count = 23`.
    The ids seed the random streams (`:24`), so they are file format: append only.
- **The Renderer module.**
  - `RenderMode` (`ParticleEffect.h:384-395`, 0–9): Billboard … `None`, `VelocityAligned`.
  - `ParticleBlend {Alpha, Premultiplied, Additive, Multiply}` (`:402`).
  - `UntexturedShape {Disc, Square}` (`:408`): the untextured billboard is a soft disc
    (`includes/particles_fs.glsl:78-84`).
  - `minScreenSize`/`maxScreenSize` clamp the size (`particles_vs.glsl:169-182`).
  - There is no pixel snapping, no integer size and no dithering anywhere in
    `engine/core/particles/` or the particle shaders (grep).
- **Colours.**
  - `MinMaxGradient` (`ParticleValues.h:95-118`) has modes Color, Gradient, TwoColors,
    TwoGradients and RandomColor (`ParticleValues.h:26-32`).
  - `Gradient` holds at most 8 colour keys (`math/Curve.h:195-225`).
  - `GradientMode::Fixed` (`Curve.h:171-174`) is already stepped, but 8 keys cannot hold a
    9-step cooling ramp, and nothing ties it to a palette.
  - The compiler bakes a Fixed gradient to 1,025 LUT entries (`ParticleCompiler.cpp:159-161`).
- **Motion.**
  - Limit Velocity has a linear `drag` in 1/s (`ParticleEffect.h:170-183`).
  - Collision (`:329-348`) has dampen, bounce, lifetime loss, min/max kill speed and types
    Planes/World/Terrain/World2D. It has no "come to rest".
- **The instance record.**
  - `ParticleInstance` (`ParticleRender.h:30-39`) is 32 bytes: position FLOAT3, colour UBYTE4N
    (sRGB-encoded with gamma 2.2), size HALF2, rotation/frame USHORT2N, velocity HALF4.
  - Optional streams: mesh rotation and custom data (`:42-50`).
  - Uploaded with `sg_append_buffer` through paged stream buffers
    (`RenderSystemParticles.cpp:160-187`, attributes `:402-414`).
  - Engine variant bits (`ShaderPool.h:30-41`, packing `ShaderPool.cpp:1863-1878`) number 11
    (`:680`).
- **Time.**
  - Fixed tick: `kMaxTicksPerFrame = 4`, `kMaxVariableStep = 1/20` (`EmitterRuntime.h:35-36`),
    `planFrame`/`endFrame` (`EmitterRuntime.cpp:420-480`).
  - Seek re-simulates from checkpoints (`beginSeek`/`continueSeek`/`seek`, `:1513-1599`;
    `ParticlePreview.h:7-12` keeps a checkpoint per simulated second, up to 32 MB). The overlay's
    scrub budget is 4 ms per frame (`editor/window/ParticleEffectOverlay.cpp:21`, seek `:147`).
  - Randomness is stateless: `hashRandom(key, particle)` (`ParticleKernels.h:35-43`) with
    `randomSalt(module, stream)` (`:32`).
- **Codec.**
  - `ParticleEffectCodec.cpp`: missing fields keep their defaults (`:712`).
  - Unknown fields warn and are skipped (`:494-507`). Unknown modules are an error (`:759`). A
    newer `version` is refused (`:718-733`).
  - Inline legacy keys are rewritten in place (`:779-803`).
- **Editor.**
  - The inspector draws modules from their `ModuleDesc` tables (`editor/window/InspectorParticles.cpp:605`
    `drawEffectDocument`, `:307` `drawField`, `:231` `drawMinMaxGradient`, `:731` the
    component).
  - Undoable edits go through `editor/util/ParticleEffectEdit.h`.
  - Presets exist only for the core `create_particles` tool: `particlePresetNames()` (fire,
    smoke, sparks, explosion, snow, rain, magic, none; `editor/mcp/tools/WorldTools.cpp:5390`),
    built from legacy settings (`:4874`, `:5380`).
- **MCP.**
  - The `particles` group (`McpToolGroups.cpp:27`, `McpTools.cpp:68`) has 11 tools
    (`docs/mcp/TOOLS.md:7`): create/get/edit_particle_effect, add/remove_particle_module,
    set_particle_effect, control_particles, get_particle_stats, capture_particles (one image at
    one time, `ParticleTools.cpp:654-660`), get_particle_migration_report, and
    import_unity_particles.
  - `budget.json` has no `particles` entry, so `group_default` applies: 12 tools, 15,000
    characters.
- **Lua.**
  - `ParticleSystem` (`GameObjectClassesLua.cpp:2226-2296`).
  - Module views `ps.main … ps.renderer` (`ParticleClassesLua.cpp:470-497`), resolving fields
    through `ParticleModules::resolve` (`:313-371`).
  - Enum tables from `:507`. `ParticleSystemRenderMode` lists billboard, stretched, horizontal,
    vertical, mesh and none only.
- **Bayer** helpers exist only for the HTML UI (`effects/lib/jfx_tone.glsl:10-20`). S43 moves
  them to `includes/bayer.glsl`.

## 4. Reference

| Source (`/tmp/pixelart-research/pixelfx`, MIT, read-only) | What we take | Change |
|---|---|---|
| `src/core/particles.js:10-20` `fly`, `flyV` | Closed-form position and velocity under gravity and linear drag: `f = (1−e^(−kt))/k`, `p = p₀ + g·t/k + (v₀ − g/k)·f`, `v = g/k + (v₀ − g/k)·e^(−kt)`; without drag `p₀ + v₀t + ½gt²` | Vector form in the analytic vertex program and its CPU twin. `k → 0` uses the series form (`f ≈ t − kt²/2`) below `k·t < 1e-3`, so float32 stays stable. |
| `src/core/particles.js:34-55` `cycle`, `stream` | Each particle's birth phase from a stateless hash; loops with no seam; a rate stream between t0 and t1 | Becomes the slot model (§6.3): one GPU instance per slot, the newest particle of the slot computed from t, and the random streams keyed by `n mod N_loop`, so a looping effect is exactly periodic. |
| `src/core/math.js:54-99` `hash`, `rng` | A stateless per-particle generator | We keep S20's `hashRandom` (`ParticleKernels.h:35-43`) so Simulated and Analytic modes draw the same random numbers for the same seed, particle and stream. It is ported to GLSL `uint` maths. |
| `src/core/color.js:37-48` `fit`, `at` | Resampling a ramp, and a step with no blending | Through S43's `fitRamp`/`rampAt`. |
| `src/core/painter.js:10, 46-49` | Bayer 4×4 dithered alpha: draw only if `a·fade > BAYER[(y&3)·4 + (x&3)]`, in screen pixels | Bayer 2/4/8 from `includes/bayer.glsl`, in art pixels of the target, or anchored to the particle (R6). |
| `src/core/sim.js:97-106, 144-150` SPARK | Gravity `950·0.45` px/s², drag 2.2/s, a floor bounce `vy·−0.45` with `vx·0.7`; streak length `min(9, 1 + speed·0.022)`, head colour by life (k > 0.6, > 0.3), a tail split at 0.45 of the length | Preset `pixel_spark` (§6.6). The bounce is the Collision module (bounce 0.45, dampen 0.3). The head steps along the `hot` ramp. |
| `src/core/sim.js:119-126, 177-186` SMOKE | Rise 16 px/s² (23 when BIG), a sideways wobble `sin(1.3t + seed)·8`, drag 1.6/1.2; radius `size·(0.65 + min(1, 2.5·age)·0.45 + 0.4·age)`; alpha `min(1, 12·age)·min(1, 2.4·k)·0.58`; three discs (body; a core of 0.74r at (0.05r, 0.06r); a lit cap of 0.5r at (−0.22r, −0.3r) while age < 0.5); colour from `SMOKE_RAMP` at `1.05·age` | Preset `pixel_smoke`, and shape Puff with those exact proportions and coverages (0.45 body, 0.62 core). |
| `src/core/sim.js:15, 58-71, 188-201` FIRE | Rise 95 px/s², wobble `sin(17t + seed)·30`, drag 5/3.2; heat `age^0.62 + (seed mod 1)·0.1` on the 9-step `FIRE_RAMP`; radius `size·(0.55 + 0.45·min(1, 7·age))·(1 − 0.55·age)`; a darker rim two steps down; smoke at death (`PF.SMOKES`, chance 0.3–0.4, speed ×0.6, size ×0.7) | Preset `pixel_fire`: Ramp bias 0.62, Disc with a rim (secondary offset +2), and a Death sub-emitter `pixel_smoke` with probability 0.35 and inherit velocity 0.6 and size 0.7. |
| `src/core/sim.js:113-118, 215-221` EMBER | Rise `−30 + sin(3t + seed)·20`, sway `cos(2.3t + seed)·45`, drag 1.6; colour by life thresholds; blink at 40 Hz below k = 0.25 | Preset `pixel_ember`, with Flicker (R8). |
| `src/core/sim.js:72-83, 209-214` GRIT | Gravity 950, drag 0.8; on the floor, bounce −0.25 if `vy > 130` with chance 0.4, otherwise stop and live at most 0.5 s more; 1 px, or 2×2 with a shaded bottom row | Preset `pixel_grit`: Collision bounce 0.25, dampen 0.5, lifetime loss 0.5, `settleSpeed` 130. Dot with Shade Bottom. |
| `src/core/sim.js:278-341` Bits | Debris that tumbles (`a += va·dt`) and bounces when hitting faster than 70 px/s (bounce, `vx·0.7`, `va·0.6`), then rests; alpha `min(1, life/0.4)`; SHARD (a line with a white tip), CHUNK (a shaded rotated diamond `|u|/s + |v|/(0.62s) ≤ 1.3`, a rim of R[0], quadrants R[n−1], R[n−2], R[n−3], R[max(1, n−4)]) | Preset `pixel_debris` (Shard and Chunk sub-emitters 50/50), `settleSpeed` 70, shapes Shard and Chunk with those exact rules. |
| `src/core/sim.js:343-360` Lights | Radius `r·(0.55 + 0.45k)`, strength `k_light·k²` | Preset `pixel_light_flash`: shape Glow, additive, Fade over Lifetime `(1 − t)²`. |
| `src/core/sim.js:362-375` `sparkle` | A cross with tips in a second colour beyond 0.6 of the arm, diagonals at arm ≥ 3 and ≥ 6, a white heart | Shape Cross. |
| `tools/check.mjs` (pixelfx's effect lints) | spill past a margin, one-shots not empty at the end, the loop seam (last → first change against 2× the 90th-percentile step), non-determinism | The `capture_particles` `checks` (R20). |

Licence: pixelfx is MIT (© 2026 Patch). `sim.js` adapts Sprite Fusion's pixel-art VFX (MIT,
© 2026 Hugo Duprez, `NOTICE.md` of pixelfx). The presets copy constants and formulas, and the
shape rules port drawing code. Each ported file keeps both notices in its header comment, and
`NOTICE.md` gets one entry naming pixelfx and Sprite Fusion.

## 5. Requirements

### Functional — the Pixel Art module

- **R1. The module.**
  - `PixelArtModule` (`ModuleId::PixelArt = 23`, `Count` becomes 24; JSON `pixelArt`;
    inspector label "Pixel Art"), with the usual present/enabled.
  - It applies to the Pixel render mode and to every camera-facing billboard mode (Billboard,
    Facing Camera Position, Stretched, Velocity Aligned) with or without a texture. Mesh mode
    ignores it, with the warning `pixel_art_mesh`.
- **R2. Snap to Pixel Grid** (`snap`, default on).
  - **The rule:** S41's rule (`pixelart::snap(p) = floor(p + 0.5 + 2⁻⁷)`, `pixelSnapRound` in
    GLSL), applied in base pixels of the drawing target (S41 R7).
  - **What snaps:** the **lower-left corner** of the particle's pixel box, not its centre:
    `corner = snap(centre_px − n/2)` for a box n pixels wide, so an odd-sized box covers whole
    pixels. For even n this equals S41's centre snap. Textured billboards snap the same way
    (their box is the size), and their texture must be Nearest (R13).
  - **With an active Pixel Perfect Camera** (S41), the grid is S41's `SnapGrid` (frame block
    `pixelSnap`).
  - **Without one, in an orthographic camera,** one base pixel is `1 / PPU` world units in view
    space (PPU = S41 R6's project Pixels Per Unit; 1 without S41), anchored at the view-space
    origin.
  - **In a perspective camera,** one base pixel is one screen pixel, and only the projected
    centre is snapped.
  - **Off:** positions are left as simulated. The shapes still rasterise on whole pixels around
    the unsnapped centre (the box moves by sub-pixels, then the fragment test is whole-pixel).
  - **Rotation (S41 R10, §6.2).** A Pixel-mode box is never rotated: it stays axis-aligned with its
    corner snapped, and Shard and Chunk evaluate their rotation (quantised to `rotationSteps`)
    per whole pixel inside it, so they are on the grid under either policy. A rotated textured
    billboard follows the drawing camera's policy with S41's functions: under Steps90 (the
    default) its rotation is rounded with `pixelSnapRotationStep` and the box then snaps as an
    unrotated one; under Rotate On Grid it keeps its angle and its pivot snaps with
    `pixelSnapCenterDelta` (vertices unsnapped).
- **R3. Sizes in Pixels** (`pixelSizes`, default on).
  - Start Size, Size over Lifetime and Size by Speed are in base pixels.
  - The drawn size is `n = max(1, round(size))` per axis. With Integer Size off it is
    `max(1, size)` with no rounding (`integerSize`, default on).
  - Min and Max Screen Size are ignored while it is on, and the inspector hides them.
  - **Off:** sizes are world units as in S20, and the box is the projected size, rounded.
- **R4. Fade** (`fade`, default Dither), with **Fade over Lifetime** (`fadeOverLifetime`, a
  MinMaxCurve over normalised age, default constant 1) multiplying the particle colour's alpha
  into the coverage a. The modes:
  - **Dither:** a pixel is drawn, fully opaque, only if `a > bayer(cell)`. Pixels are always
    written with alpha 1.
    - `ditherSize`: 2, 4 or 8 (default 4).
    - `ditherSpace`:
      - **Screen** (default; pixelfx): the cell is the base-pixel coordinate of the target, so
        the pattern holds still while particles move through it;
      - **Particle**: the cell is relative to the particle's snapped corner, so the pattern
        travels with the particle.
  - **Cutout:** drawn if `a ≥ 0.5`, alpha 1.
  - **Alpha:** S20's blending, unchanged (for soft, non-pixel looks).

  Dither and Cutout keep a palette colour exact on screen (R13). They need no sorting, and depth
  write can be on.
- **R5. Hold Frames** (`holdFrameRate`, in Hz, 0 = off; presets leave it at 0).
  - The drawn state changes only at whole steps of `1/holdFrameRate` of emitter time: a particle
    is drawn where it was at the last step boundary. This is pixelfx `act.js`'s 12 fps posing,
    or animating "on twos".
  - The simulation keeps its own tick.
  - In Simulated mode, render interpolation is off and the instance data is packed only when a
    step boundary is crossed (between steps the previous upload is drawn again). So Hold Frames
    also saves CPU.
  - In Analytic mode, t is quantised to `floor(t·rate)/rate`.
- **R6. The secondary colour** (`secondary`), used by shapes that need a second colour: a streak's
  tail, a puff's lit cap, a disc's rim, a shard's tip, a cross's tips. Modes:
  - **Ramp Offset** (default): the colour `offset` steps from the particle's current ramp step
    (`secondaryOffset` −8…8, default +1; positive moves toward the end of the ramp as evaluated, after Reverse), clamped to the ramp. It uses
    the ramp of whichever module produced the colour: Color over Lifetime if it is a Ramp, else
    Start Color if it is a Ramp.
  - **Color:** a fixed colour (`secondaryColor`).

  Without a ramp, Ramp Offset falls back to the secondary colour, and the inspector notes it.
- **R7. Wobble** (`wobble`: amplitude in px, x and y; frequency in Hz; phase random per particle,
  default on).
  - A render-time offset `A·sin(2π·f·age + φ)` per axis, added before snapping.
  - It is closed-form, so it works in both simulation modes and never feeds back into the
    motion. pixelfx's smoke, fire and ember sway as a force; the presets' wobble is tuned to look
    the same (§6.6).
- **R8. Flicker** (`flickerBelow` 0–1 of remaining life, default 0 = off; `flickerRate`, default
  40 Hz).
  - When the remaining life `k < flickerBelow`, the particle is hidden on alternate half
    periods: `sin(2π·rate·time + φ) < 0` (pixelfx EMBER, `sim.js:217`).
  - φ is the particle's random phase, so particles do not blink together.

### Functional — the Pixel render mode and shapes

- **R9. The render mode.** `RenderMode::Pixel` (= 10, JSON `pixel`, label "Pixel") with
  `pixelShape`.
  - No texture is bound. The fragment shader evaluates the shape per whole base pixel, inside a
    box that the vertex stage places on the grid (R2).
  - All fragments are written at the particle's colour with alpha 1 under Dither/Cutout (R4).
  - The library features of `library:particles` (GLOW, EROSION, SOFT, NEAR_FADE) still compile;
    EROSION and SOFT are meaningless for pixel art, and the inspector says so.
  - In local pixel coordinates (x, y), with (0, 0) the box's lower-left pixel, n the size, and
    c = the particle's centre:

  | Shape | Rule (whole pixels) | Box | Uses |
  |---|---|---|---|
  | **Dot** | every pixel of the n×n box; `shadeBottom` paints the bottom row in the secondary colour when n ≥ 2 (pixelfx GRIT) | n×n | sparks at rest, grit, embers |
  | **Cross** | `x == c` or `y == c`, arms `a = floor(n/2)`; tips (beyond 0.6a) secondary; diagonals ±1 when a ≥ 3 and ±2 when a ≥ 6; the heart is the colour's ramp end (white on the presets) | n×n, n odd (even n rounds up) | sparkles, hits |
  | **Disc** | `(x + 0.5 − n/2)² + (y + 0.5 − n/2)² ≤ (n/2)²`; `rim` paints the outer 1 px ring secondary | n×n | fire puffs, bubbles |
  | **Ring** | the disc minus the disc of radius n/2 − `ringWidth` (1–8 px, default 1) | n×n | shockwaves |
  | **Streak** | a pixel-perfect line from the head (the particle position) back along the on-screen velocity, `L = clamp(1 + speed_px·lengthScale, 1, maxLength)` px (defaults 0.022, 9: pixelfx); one pixel per major-axis step, minor = `floor(t·slope + 0.5)`, so there are no doubled corners; the first `headFraction` (0.45) from the head is primary, the rest secondary; width n (1–3) | the line's box + n | sparks, rain, laser bits |
  | **Puff** | the union of three discs (R: pixelfx SMOKE): body r = n/2; core 0.74r offset (0.05r, −0.06r); cap 0.5r at (−0.22r, +0.3r) in secondary while normalised age < 0.5; coverage weights 0.45 (body), 0.62 (core), 0.4·(1 − 2·age) (cap) multiply a before the dither | ⌈1.6n⌉ square | smoke |
  | **Shard** | a pixel-perfect line of length n through c at the particle's rotation; the last pixel is secondary (pixelfx's white tip) | ⌈n⌉ + 2 square | debris, glass |
  | **Chunk** | rotated (u, v): `d = |u|/s + |v|/(0.62s)`, s = n/2, drawn if d ≤ 1.3; d > 1 is ramp[0] (the rim), inside by quadrant ramp[last], ramp[last−1], ramp[last−2], ramp[max(1, last−3)] (pixelfx CHUNK, top-left lit) | ⌈2.6s⌉ + 2 square | rocks, bricks |
  | **Glow** | `bands` (1–4, default 3) concentric rings of radius n/2·(i/bands); band i's coverage is `1 − i/bands` times a | n×n | light flashes, muzzle flash |

  - **Rotation:** Shard and Chunk use the particle rotation, quantised to `rotationSteps`
    (default 16 per turn), so the shapes do not shimmer between angles. The other shapes ignore
    rotation.
  - **Streak:** uses the screen-space velocity of the analytic or simulated particle, and is a
    single pixel when the speed is 0.
- **R10. The pixel instance stream.**
  - When Pixel mode (or a Pixel Art module with Flicker, Wobble or a Ramp Offset secondary) is
    active, packing adds one 4-byte per-instance attribute `i_particlePixel` (UBYTE4):
    - x: the primary ramp step (255 = not from a ramp);
    - y: the normalised age ×255 (for Puff's cap);
    - z: the random phase byte (Wobble, Flicker);
    - w: flags.
  - The ramp itself (≤ 32 colours, exact sRGB8) is a `vec4 pixelRamp[32]` uniform of the Pixel
    variant, present only when a shape or the secondary colour reads it.
  - With the module absent and the mode not Pixel, there is no stream, no uniform and no bit.
- **R11. Pixel-perfect lines.** Streak and Shard draw exactly one pixel per major-axis step. A
  45° line is a clean diagonal, and no L-corner ever appears (the Pixelorama pixel-perfect
  criterion). AC5 checks every angle in 1° steps for lengths 1–16.

### Functional — colour on a palette ramp

- **R12. The Ramp gradient mode.** `GradientValueMode::Ramp` (appended; label "Palette Ramp")
  with these fields:
  - `ramp`: a reference `<palette>.palette#ramp:<name>` (S43 R4a), or `builtin:<id>#ramp:<name>`
    (resolved by copying, S43 R7), or an inline `rampColors: ["#rrggbb", …]`;
  - `rampSteps` (0 = the ramp as is; else S43 `fitRamp` to n, 2–32);
  - `rampReverse`;
  - `rampBias` (0.1–10, default 1: the driver t becomes `t^bias`; pixelfx's fire uses 0.62).

  It evaluates S43 `rampAt(ramp, t)`, a step with no blending, where t is the MinMaxGradient's
  driver (normalised age, speed, …) after its remap. In Start Color, t is the particle's random
  value: a random step. The colour's alpha is the ramp colour's alpha; fading belongs to Fade
  over Lifetime (R4).
- **R13. Exact palette colours on screen.** With Ramp colours, the Pixel mode or a Nearest
  texture, Dither or Cutout fade, an unlit renderer, a white tint and neutral post-processing
  (tone mapping None, no grade), every drawn pixel is exactly a ramp colour's sRGB8 bytes. The
  measures:
  - the compiler bakes a Ramp into an exact n-entry table, not a 1,025-entry LUT;
  - packing writes the sRGB8 bytes unchanged into `color` (UBYTE4N);
  - the shader's decode/encode round trip (`sRGBToLinear` → output `linearTosRGB`) is exact at
    8 bits (AC6 asserts all 256 values on every backend);
  - a textured particle in a Pixel Art module needs a Nearest, uncompressed texture, or the
    inspector warns with a Fix (S14 settings, as S43 R15 does).
- **R14. Settle.** Collision gains `settleSpeed` (units/s, default 0 = off; Simulated mode only).
  - After a collision whose post-response speed is below it, the particle stops: its velocity is
    0, it ignores forces and gravity, and its rotation stops changing. It stays until its life
    ends (pixelfx Bits `rest`, GRIT).
  - A per-particle flag channel is allocated only when `settleSpeed > 0`.

### Functional — Analytic simulation

- **R15. The mode.** The document field `simulationMode` {Simulated (default), Analytic} (label
  "Simulation Mode", in the Main module's header area).
  - **Supported modules in Analytic:**
    - **Main:** Start Lifetime, Speed, Size, Rotation and Color, as Constant or Random Between
      Two Constants. Start Color may also be a Ramp.
    - Gravity Modifier and Simulation Space (Local; World with a static emitter, R18).
    - **Emission:** Rate over Time, Constant; Bursts ≤ 4, with time, count, cycles, interval and
      probability.
    - **Shape:** Point, Sphere, Hemisphere, Cone, Circle, Box, Edge, Donut; arc, radius
      thickness, randomize direction.
    - **Velocity over Lifetime:** a constant linear velocity, added to the start velocity and
      dragged with it. This is a documented difference from Simulated mode.
    - **Limit Velocity:** the constant linear `drag` only.
    - **Force over Lifetime:** a constant, folded into g.
    - **Color over Lifetime:** Color, Gradient, Ramp (baked into ≤ 32 uniform entries).
    - **Size over Lifetime:** a curve (baked into 32 samples).
    - **Rotation over Lifetime:** a constant angular velocity.
    - **Texture Sheet Animation:** Grid, by Lifetime or FPS.
    - **Pixel Art** (all of it) and **Renderer** (Billboard modes, Stretched, Pixel).
  - **Everything else is unsupported:** curves on start values, Noise, Orbit, External Forces,
    Collision, Trails, Sub Emitters, Custom Data, the By-Speed modules, Set over Age, Limit
    Velocity's limit and quadratic drag, inherit velocity and sorting.
  - **How unsupported modules show:** each present, enabled unsupported module is a validation
    error `analytic_unsupported` `{path: "modules.<m>", fix: "disable it or use Simulated"}`.
    `get_particle_effect` returns it, and the inspector shows the module greyed out with that
    line.
- **R16. The slot model** (D3). The effect compiles to a fixed slot count:
  `C_rate = ceil(rate · L_max) + 1` plus `Σ count_b` over the bursts, ≤ 65,536 (more is the error
  `analytic_capacity` with the count). For slot s at emitter time t:
  - **Rate slots:** the newest particle in the slot is
    `n = s + C_rate·floor((t·rate − s)/C_rate)` for `t·rate ≥ s`, born at `n/rate`, of age
    `t − n/rate`. It is alive if `age < life(n)` (and `n < rate·duration` when not looping).
  - **Burst slots:** burst b's particle j is in cycle `floor((t − time_b)/interval_b)`, clamped
    to the burst's cycles. Its age is measured from that cycle's time, and probability is a
    hash test per cycle.
  - **Dead slots** emit a degenerate quad.
  - **Randoms:** `hashRandom(seed ⊕ salt(module, stream), n mod N_loop)`, the same function and
    salts as Simulated mode.
- **R17. Exact loops.**
  - A looping Analytic effect is periodic with its duration D: the rate is snapped so that
    `N_loop = rate·D` is an integer (the inspector shows "Rate 24 → 24.0/s for a seamless
    loop"), and randoms use `n mod N_loop`. So the frame at t equals the frame at t + D, bit for
    bit, including the particles that cross the seam (pixelfx `cycle`).
  - The time fed to the shader is `t mod D` plus the integer loop count kept on the CPU, so
    float32 precision never degrades over a long session.
  - Prewarm is free: playing starts at any phase.
- **R18. The motion.**
  - **Position in simulation space:** pixelfx `fly` (§4) with `g` = gravity × modifier +
    constant force, `k` = drag, and `v₀` = start velocity + constant VOL.
  - **Rotation:** `θ₀ + ω·age`. **Size:** start × curve(age/life). **Colour:** start × gradient
    or ramp step (R12).
  - **Velocity**, for Stretched, Streak and the Velocity-aligned modes: pixelfx `flyV`.
  - **Local space:** the emitter's transform applies to all live particles (as in S20's Local).
  - **World space with a moving emitter:** live particles would move with it, so this is the
    warning `analytic_moving_world_emitter`. Analytic World space is for static emitters
    (ambient smoke, torches).
- **R19. The CPU twin.** `particles::AnalyticParticles::evaluate(program, slot, t)` evaluates the
  same formulas in float32, in the same order. It is used for:
  - the conservative bounds (`maxSpeed`, drag, g and `L_max` give an AABB at compile time,
    cached), so culling has no per-frame cost;
  - `particleCount` and `get_particle_stats` (O(C) on demand only);
  - Scene-view picking and the AC tests.

  Before snapping, GPU and CPU agree within 1e-3 base px. After snapping, ≥ 99.9 % of particles
  land on the same pixel, and any difference is exactly one pixel (AC8). On the GPU, Analytic
  emitters raise no `onParticleDeath`, `onCollision` or sub-emitter events; Lua's
  `ps.onParticleDeath` on an Analytic system logs the warning `analytic_no_events` once.

### Functional — presets and capture

- **R20. Presets.** Nine `.particles` documents under `editor/pixelart/particles/`, embedded,
  with the values of §6.6.
  - Pixel constants are in base pixels per second and per second². On creation they are divided
    by the project PPU (S41 R6; 1 by default, so they are used as is), and the document records
    `designPixelsPerUnit`.
  - **Changed PPU:** when the project PPU later differs, the inspector offers "Rescale to Pixels
    Per Unit N" (one undo step).
  - **Where they appear:**
    - GameObject ▸ Effects ▸ Pixel ▸ Spark, Smoke, Fire, Ember, Grit, Debris, Light Flash, Smoke
      Loop, Ember Loop (a Particle System with that inline effect, at the Scene view's pivot,
      snapped);
    - Project ▸ Create ▸ Particle Effect ▸ Pixel ▸ … (an asset);
    - the inspector's effect menu "Replace with Preset".
  - **Palette:** each preset's colours are Ramps of `builtin:pixelfx-effects` (S43 R9). When the
    project has a Default Palette (S43 R4b), "Match Project Palette" remaps each ramp step to the
    nearest OKLab colour of that palette's closest ramp.
- **R21. Bake to Sprite Sheet.**
  - The overlay's ⋯ menu and the Renderer offer "Bake to Sprite Sheet…": frames 1–64 (default
    16), time range (default one loop or the full one-shot), integer scale 1–8, padding,
    background (transparent).
  - It writes a PNG strip plus an Aseprite json-hash file (frame rects and durations, the layout
    Aseprite's `--data` writes and S46 R23 writes too), which S45's texture Sprite Mode imports
    (Slice… › From JSON, S45 R10); it creates no material.
- **R21a. Ember entry point (for S48).** `particles::PixelParticles::emitAt(scene, effect,
  positions, velocities = {})` emits one particle of a Pixel Particles preset or effect asset at
  each given world position this frame (Simulated mode; an Analytic effect refuses it with
  `analytic_no_events`). S48's dissolve embers (S48 R26) call it with the texels that crossed the
  front; it costs nothing when nobody calls it.
  - It is undoable (`writeUndoable`) and deterministic for Analytic effects, and for Simulated
    ones through seek.

### Platform & backends

- **R22. Backends.** All of it runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan:
  - per-instance vertex attributes only, plus a static slot-index stream (FLOAT, divisor 1,
    shared and grown in powers of two up to 65,536 entries = 256 KB) for Analytic;
  - no `gl_InstanceID`/`gl_VertexID` dependence (S20 R17);
  - `highp uint` hashing (GLSL ES 3.00), `texelFetch`-free shapes (integer maths and `floor`
    only), no derivatives, and `discard` for the shape mask.
- **R23. Engine variant bits.** `Pixel` and `Analytic` are appended to the particle variant bits
  (`ShaderPool.h:30-41`; names in `ShaderPool.cpp:837-849`) after S41's `Pxs` (wave 5A), so the
  count goes from 12 to 14; bit numbers are taken at landing with fixed names. They are set only by
  effects that use them.
- **R24. Every export target** builds the code; web runs Analytic on its single thread with no
  extra work (there is no CPU work per particle).

### Performance (reference machines and the reference stack of S20 §5)

- **R25. Off costs nothing.**
  - Without a Pixel Art module, Pixel mode, Ramp or Analytic, a particle system's channels,
    instance bytes, uniforms, variant and CPU time are unchanged from S20.
  - `ParticleBudgetTest` asserts the bytes per particle, and S20's benchmarks must not move by
    more than their noise band (±3 %).
- **R26. Pixel mode on the CPU.**
  - Packing the pixel stream adds ≤ 10 % to S20's pack time per particle, and 4 bytes per
    particle.
  - Snapping and shapes cost no CPU (the vertex stage does them).
  - Hold Frames at 12 Hz on a 60 Hz display cuts pack time by ≥ 70 % (measured).
- **R27. Pixel mode on the GPU.** At a 320×180 base resolution scaled 6×, 20,000 Spark particles
  cost ≤ 0.3 ms on mid desktop and ≤ 1 ms on mid mobile. A Pixel-mode fragment costs no more
  than an untextured S20 billboard fragment at the same coverage (no texture fetch).
- **R28. Analytic.**
  - CPU per emitter per frame ≤ 5 µs on mid desktop and ≤ 20 µs on mid mobile and web: one
    uniform block (≤ 1 KB) and one draw, independent of the particle count.
  - GPU: 65,536 particles cost ≤ 0.5 ms of vertex work on mid desktop and ≤ 2 ms on mid mobile.
  - Seek and scrub cost O(1): ≤ 0.1 ms whatever the target time (S20's checkpointed seek takes
    up to its 4 ms budget per frame).
  - No checkpoint memory is used.
- **R29. Memory.**
  - Analytic: no channel storage. Its only memory is the shared slot stream (≤ 256 KB) and the
    cached bounds.
  - Pixel mode adds 4 B per instance.
  - Ramps add ≤ 512 B of uniforms per draw.

### Editor UX (human)

- **R30. The Pixel Art module in the inspector**, in Unity's style (a checkbox in the header):
  - Snap to Pixel Grid, Sizes in Pixels, Integer Size;
  - Fade (Dither / Cutout / Alpha), with Dither Size and Dither Space under it, and the Fade
    over Lifetime curve (S27 curve field);
  - Hold Frames (Hz, "0 = every frame"), Secondary Color (Ramp Offset ± / Color), Wobble,
    Flicker.

  A one-line status shows the grid in use ("Pixel Perfect Camera, 320×180, PPU 16", or
  "Orthographic, PPU 1", or "Perspective: screen pixels").
- **R31. The Renderer, with Render Mode Pixel.**
  - Pixel Shape is a dropdown with a 16×16 live icon per shape, drawn by the CPU reference of the
    shape rule (the same code as the AC golden), so the icon is the truth.
  - Shape-specific fields appear under it (Streak: Length Scale, Max Length, Head Fraction,
    Width; Disc: Rim; Ring: Width; Dot: Shade Bottom; Glow: Bands; Shard/Chunk: Rotation Steps).
  - Texture, Untextured Shape and Min/Max Screen Size hide.
- **R32. The Ramp gradient mode in every gradient field.**
  - "Palette Ramp" is in the gradient mode menu. It shows a palette asset field, a ramp dropdown
    (with strips), Steps, Reverse, Bias, and a stepped preview bar.
  - Dragging a ramp strip from the S43 Palette window onto the field sets it.
- **R33. Simulation Mode.**
  - Switching to Analytic validates at once. Unsupported modules grey out with a tooltip, and a
    banner offers "Disable unsupported modules" (one undo step).
  - Switching back restores nothing automatically; the modules stay as they were, disabled.
- **R34. The overlay** (`ParticleEffectOverlay`).
  - For Analytic systems, the scrub slider is exact and instant; its label shows "Analytic:
    exact".
  - A Pixel Grid toggle draws the base-pixel grid around the emitter when zoomed in ≥ 4× (Scene
    view only).
  - The stats line adds "pixel" and "analytic" tags.
- **R35. Docs.** `docs/particles-pixel.md`: a walkthrough per preset with sheets, the shapes,
  Analytic's limits (a table), the Debris → Fire recipe, and a migration hint: "to make an
  existing 2D effect pixel art, add Pixel Art and set Render Mode to Pixel".

### Agent UX (MCP, Lua, CLI)

- **R36. `create_particle_effect`.** Its `preset` enum gains the nine `pixel_*` names
  (`pixelParticlePresetNames()`, editor-only). The core tool `create_particles` keeps its list,
  so core's budget is untouched. The agent flow is `create_particle_effect` then
  `set_particle_effect`, or a `set_particle_effect` with `copy_to_inline`.
- **R37. `edit_particle_effect` and `add_particle_module`.**
  - Every new field is reachable through the existing merge patch. Examples:
    - `{"modules": {"pixelArt": {"fade": "dither", "ditherSize": 4}}}`;
    - `{"modules": {"renderer": {"renderMode": "pixel", "pixelShape": "streak"}}}`;
    - `{"simulationMode": "analytic"}`;
    - `{"modules": {"colorOverLifetime": {"color": {"mode": "ramp", "ramp": "palettes/fx.palette#ramp:fire", "rampBias": 0.62}}}}`.
  - `add_particle_module`'s description gains `pixelArt`.
  - A patch that makes an Analytic effect invalid is refused with the R15 diagnostics (S20's
    existing rule).
- **R38. `capture_particles`.** The existing tool gains:
  - `frames` (1–64): a contact sheet of N evenly spaced times from `time` to `time_end` (default:
    one loop, or the one-shot's end), with labels;
  - `scale` (integer 1–8, Nearest; default 1, or the S41 pixel scale for pixel effects);
  - `background` (`#rrggbb`, default `#ff00ff`, a key colour for the checks);
  - `checks` (bool). It returns the family's lint results (`{id, status: PASS|WARN|FAIL|SKIP,
    measured, threshold, threshold_source, count, at, message, fix}`, S44's `LintTypes.h`) for:
    - `off_grid` (S49's: simulated or analytic positions vs. their snapped positions under a
      pixel grid);
    - `off_palette` (S48's function: effect pixels not in the effect's ramps, or in the project's
      Default Palette, S43 R4b);
    - `partial_alpha` (S48's function: blended pixels in a pixel effect);
    - `spill` (S48's: pixels beyond the bounds + 24 px, from pixelfx);
    - `one_shot_end` (S48's: a one-shot not empty at its end);
    - `loop_seam` (S48's: the last → first change > 2× the 90th-percentile frame step, from
      pixelfx);
    - `determinism` (S48's: two captures at the same time differ);
    - `over_budget` (S49's: the particle count against the world budget share).

  The ids and definitions are S48 R28's, so a capture sheet and a contact sheet report the same
  lint the same way. The result stays one image (the sheet), with the per-frame stats in
  `structuredContent`.
- **R39. `control_particles` and `get_particle_stats`.**
  - No schema change.
  - `seek` on an Analytic system is exact and returns at once.
  - The stats add `simulation_mode`, `pixel` and `analytic_slots`.
  - `get_particle_effect` diagnostics include R15's.
- **R40. Lua** (§6.5).
  - Enums: `ParticleSystemRenderMode.Pixel` (and the other S20 modes Lua lacks today:
    facingCameraPosition, worldOriented, localOriented, velocityAligned), `ParticlePixelShape`,
    `ParticleFade`, `ParticleSimulationMode`.
  - The `ps.pixelArt` module view (generic, through `ParticleModules::resolve`).
  - `ps.simulationMode` (setting it restarts the system).
  - `MinMaxGradient`-valued views gain `:setRamp(palettePath, rampName[, steps])`.
  - The CPU twin as `ps:getAnalyticParticle(slot, time)` (for debugging and tests; nil for
    Simulated systems).

### Data & compatibility

- **R41. The effect format.** `kEffectSchemaVersion` stays 1. Every addition is optional and
  defaults to today's behaviour:
  - the `pixelArt` module (absent);
  - `renderMode: "pixel"` and the shape fields;
  - the gradient `mode: "ramp"` and its fields;
  - Collision `settleSpeed` (0);
  - `simulationMode` (`simulated`);
  - `designPixelsPerUnit` (absent).

  Missing fields keep their struct defaults (`ParticleEffectCodec.cpp:712`). Older builds
  refuse a file that uses the new module ("unknown module", `:759`): expected, newer data. No
  migration is needed. `ModuleId::PixelArt = 23` is appended, so the random streams of existing
  modules, and so every existing effect's output, are bit-identical (AC14).
- **R42. Inline effects** in scenes and prefabs (a JSON string in YAML, S20 §6) carry the same
  fields. The scene format is not bumped (optional fields, `StreamMigrations.h:18-26`).
- **R43. Asset references.** `ramp` strings are S04 asset references: `AssetRefSchema` gets a row
  for MinMaxGradient ramps inside effects (beside the existing `inlineEffect` visitor,
  `editor/asset/AssetRefSchema.cpp:505`), so renaming a palette keeps the effects working.
  `builtin:` refs never reach a saved file (S43 R7).

## 6. Design constraints & decisions

### 6.1 MCP tools and the budget (S31; group `particles`, 11 tools today, ≤ 12 and 15,000 characters)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `create_particle_effect` | `particles` | `preset` enum gains 9 `pixel_*` names | +130 |
| `add_particle_module` | `particles` | description lists `pixelArt` | +10 |
| `capture_particles` | `particles` | `frames`, `time_end`, `scale`, `background`, `checks`; description +1 sentence | +380 |
| `edit_particle_effect`, `get_particle_effect`, `control_particles`, `get_particle_stats` | `particles` | none (new fields are values; diagnostics in results) | 0 |
| `create_particles` | core | none | 0 |

No tool is added; the group keeps its free slot. The total growth is about 520 characters. M7
measures the group with `tool_budget.py --groups particles`. If the group would exceed
15,000, the pixel preset names leave the schema: `preset` stays a string, the handler validates
it, and the names move to `jasper://docs/particles` (a typed `invalid_value` error lists them).
The agent guide gains no line: the `particles` digest already points to `jasper://docs/particles`,
whose new "Pixel particles" section holds the details.

### 6.2 The Pixel Art module and new fields (JSON, keyed form)

```json
{
  "simulationMode": "simulated",
  "designPixelsPerUnit": 1,
  "modules": {
    "pixelArt": {
      "snap": true, "pixelSizes": true, "integerSize": true,
      "fade": "dither", "ditherSize": 4, "ditherSpace": "screen",
      "fadeOverLifetime": {"mode": "curve", "curve": [[0, 1], [0.7, 1], [1, 0]]},
      "holdFrameRate": 0,
      "secondary": "rampOffset", "secondaryOffset": 1, "secondaryColor": [1, 1, 1, 1],
      "wobble": {"amplitude": [0, 0], "frequency": 1.3, "randomPhase": true},
      "flickerBelow": 0, "flickerRate": 40
    },
    "renderer": {
      "renderMode": "pixel", "pixelShape": "streak",
      "streakLengthScale": 0.022, "streakMaxLength": 9, "streakHeadFraction": 0.45, "streakWidth": 1,
      "discRim": false, "ringWidth": 1, "dotShadeBottom": false, "glowBands": 3, "rotationSteps": 16
    },
    "colorOverLifetime": {
      "color": {"mode": "ramp", "ramp": "Assets/Palettes/pixelfx-effects.palette#ramp:hot",
                "rampSteps": 0, "rampReverse": false, "rampBias": 1}
    },
    "collision": {"settleSpeed": 0}
  }
}
```

The shape fields live in the Renderer and show only for their shape. The writer leaves out
defaults (`StructDesc::defaults`, `ParticleModules.h:127-128`), so a non-pixel effect's file is
byte-identical to today's.

### 6.3 Decisions

- **D1. A module plus one render mode, not nine render modes.** One `RenderMode::Pixel` with a
  `pixelShape` keeps the Render Mode menu Unity-like. Shape fields appear contextually. The
  enum stays small, and Lua and MCP gain one value instead of nine. The Pixel Art module holds
  what applies to every 2D mode (grid, size, fade, hold, secondary), so a textured pixel sprite
  particle gets snapping and dithered fades without the Pixel shapes.
- **D2. Analytic is not GPU simulation.** S20 §2 excludes stateful GPU simulation. Analytic has
  no state on the GPU: each vertex evaluates a pure function of (slot, t, uniforms), and nothing
  is read back. It is the closed-form technique of pixelfx's `particles.js`, made per-instance.
  It needs only vertex shaders and instanced attributes, so it works on all five backends.
- **D3. The slot model, not a particle list.** With a constant rate, the particle that occupies
  slot s at time t is computable (`n ≡ s mod C`, the newest born ≤ t), and `C ≥ rate·L_max`
  guarantees the previous occupant is dead. That gives:
  - a bounded instance count;
  - exact seeks;
  - exact loops (R17).

  The limits follow from it, and they are documented. There is no history, so:
  - no collisions or settling (a bounce depends on the path, and with drag it has no closed
    form);
  - no noise or force fields (they integrate along the path);
  - no sub-emitters or events (nothing observes a death on the CPU);
  - no trails (they need past positions);
  - no sorting (draw order is slot order, which is why Analytic pairs with Dither/Cutout or
    Additive);
  - World space with a moving emitter drags its live particles.
- **D4. Snap the corner, not the centre.** S41 snaps centres, which is right for even sizes and
  pivots. A 1-px dot centred on a pixel corner would straddle four pixels. Snapping the box's
  corner with S41's own function (R2) keeps one rounding rule and covers whole pixels for every
  size.
- **D5. Dither in base pixels of the target, by default.** It matches pixelfx and reads as one
  dithered surface when particles overlap. Particle space is offered for big, slow puffs, whose
  pattern would otherwise visibly crawl.
- **D6. Exact colour through the existing 32-byte record.** The ramp colour already fits
  UBYTE4N sRGB8. The 4-byte pixel stream carries only what the shapes need. That keeps S20's
  32-byte record and its packing kernels unchanged.
- **D7. The presets are data.** Nine `.particles` files with comments in `docs/particles-pixel.md`.
  They are tuned constants, not code paths, so a user can open, copy and change them.

### 6.4 C++ sketch

```cpp
// engine/core/particles/ParticleEffect.h (appended)
enum class RenderMode : uint8_t { /* ... */ VelocityAligned, Pixel };
enum class PixelShape : uint8_t { Dot, Cross, Disc, Ring, Streak, Puff, Shard, Chunk, Glow };
enum class PixelFade : uint8_t { Dither, Cutout, Alpha };
enum class DitherSpace : uint8_t { Screen, Particle };
enum class SecondaryColorMode : uint8_t { RampOffset, Color };
enum class SimulationMode : uint8_t { Simulated, Analytic };

struct PixelArtModule {
    bool present = false, enabled = true;
    bool snap = true, pixelSizes = true, integerSize = true;
    PixelFade fade = PixelFade::Dither;
    int ditherSize = 4;                         // 2, 4, 8
    DitherSpace ditherSpace = DitherSpace::Screen;
    MinMaxCurve fadeOverLifetime = MinMaxCurve(1.0f);
    float holdFrameRate = 0.0f;                 // Hz, 0 = every frame
    SecondaryColorMode secondary = SecondaryColorMode::RampOffset;
    int secondaryOffset = 1;
    Vector4 secondaryColor = Vector4(1.0f);     // sRGB
    Vector2 wobbleAmplitude = Vector2(0.0f);    // base px
    float wobbleFrequency = 1.0f;
    bool wobbleRandomPhase = true;
    float flickerBelow = 0.0f, flickerRate = 40.0f;
};
// RendererModule gains: PixelShape pixelShape; float streakLengthScale = 0.022f, streakMaxLength = 9,
// streakHeadFraction = 0.45f; int streakWidth = 1; bool discRim; int ringWidth = 1;
// bool dotShadeBottom; int glowBands = 3; int rotationSteps = 16.
// MinMaxGradient gains (mode Ramp): std::string ramp; std::vector<Vector4> rampColors; int rampSteps;
// bool rampReverse; float rampBias = 1.
// CollisionModule gains: float settleSpeed = 0.
// ParticleEffectDocument gains: SimulationMode simulationMode; float designPixelsPerUnit = 0.

// engine/core/particles/AnalyticParticles.h
struct AnalyticProgram {                        // compiled once per effect; uploaded as one uniform block
    uint32_t slotsRate, slotsBurst, loopCount;  // N_loop for exact loops (0: not looping)
    float rate, duration, lifeMin, lifeMax;
    Vector3 gravity; float drag;
    /* start ranges, shape, bursts[4], colour LUT[32], size curve[32], angular velocity, sheet ... */
    AABB bounds;                                // conservative, simulation space
};
struct AnalyticParticle { bool alive; uint32_t index; float age, life; Vector3 position, velocity; float rotation; Vector2 size; uint32_t color; uint8_t rampStep; };
class AnalyticParticles {
public:
    static bool compile(const ParticleEffectDocument&, uint32_t seed, AnalyticProgram& out, std::vector<Diagnostic>& diagnostics);
    static AnalyticParticle evaluate(const AnalyticProgram&, uint32_t slot, double emitterTime);  // the GPU's twin
    static uint32_t aliveCount(const AnalyticProgram&, double emitterTime);
};
```

### 6.5 Shader sketch

```glsl
// includes/particles_pixel.glsl (PARTICLE_PIXEL): vertex part
//   in vec4 i_particlePixel;                  // x ramp step, y age, z phase, w flags (UBYTE4)
//   particlesParams.pixel: x shape, y size flags, z dither size, w dither space;
//   particlesParams.pixelGrid: xy base px per world unit or the S41 SnapGrid, z k, w perspective
vec2 boxPx = pixelShapeBox(shape, n, velocityPx, rotation);         // whole pixels
vec2 cornerPx = vec2(pixelSnapRound(centrePx.x - boxPx.x * 0.5),     // S41's function (D4)
                     pixelSnapRound(centrePx.y - boxPx.y * 0.5));
v_pixelLocal = (a_corner + 0.5) * boxPx;                             // 0..box, floor() per fragment
gl_Position = pixelGridToClip(cornerPx + v_pixelLocal);              // S41 pixelSnapFromPixels, or the ortho grid

// fragment part
ivec2 p = ivec2(floor(v_pixelLocal));
float cover; int tone;                                               // tone: 0 primary, 1 secondary, 2+ ramp entries
if (!pixelShape(p, cover, tone)) discard;                            // the R9 table, integer maths
float a = v_color.a * v_fade * cover;
#if PIXEL_FADE_DITHER
    if (a <= bayer(ditherCell(p), ditherSize)) discard;              // includes/bayer.glsl (S43)
    a = 1.0;
#endif

// includes/particles_analytic_vs.glsl (PARTICLE_ANALYTIC): replaces the instance record reads
in float i_slot;                                                     // the shared slot stream
uniform u_vs_analyticParams { /* AnalyticProgram, with t split as loop phase + seconds */ } analytic;
// evaluate(slot, t) exactly as AnalyticParticles::evaluate; outputs the same varyings as
// particles_vs.glsl, so particles_fs.glsl and the library:particles features are unchanged
```

### 6.6 Presets (pixel units: px, px/s, px/s²; y up; `hot` etc. are ramps of `builtin:pixelfx-effects`)

| Preset | Emission and shape | Motion | Look |
|---|---|---|---|
| `pixel_spark` | Burst 24, Cone 70° up, speed 120–260, life 0.5–0.9, tick 120 Hz | Force (0, −427.5) (= 950·0.45); Limit Velocity drag 2.2; Collision Planes at y = −16, bounce 0.45, dampen 0.3 | Pixel Streak, width 1, length 0.022/9, head 0.45; Color over Lifetime Ramp `hot` (as in `sim.js:13`, bright → dark over life: pale-yellow head, then red); secondary +1; Fade Cutout, Fade over Lifetime 1 until 0.85, then 0 |
| `pixel_smoke` | Rate 10, Circle r 3, speed 4–10 up, life 0.8–1.4, size 6–10 | Force (0, 16); drag 1.4 (pixelfx 1.6 x / 1.2 y, averaged); Wobble x 3 px at 0.2 Hz | Pixel Puff; Size over Lifetime `0.65 + min(1, 2.5t)·0.45 + 0.4t`; Color Ramp `smoke_thin`, bias 1, ×1.05 remap; secondary −1 (lit cap); Fade Dither 4, Fade over Lifetime `min(1, 12t)·min(1, 2.4(1−t))·0.58` |
| `pixel_fire` | Rate 30, Edge 6 px, speed 12–30 up, life 0.28–0.6, size 2–5, tick 60 Hz | Force (0, 95); drag 4 (5 x / 3.2 y); Wobble x 2 px at 2.7 Hz | Pixel Disc with rim (secondary +2); Ramp `fire_cooling`, bias 0.62; Size over Lifetime `(0.55 + 0.45·min(1, 7t))·(1 − 0.55t)`; Dither 4; Sub Emitters: Death → `pixel_smoke` (inline), probability 0.35, inherit velocity 0.6 and size 0.7 |
| `pixel_ember` | Rate 6, Edge 8 px, speed 15–45 up, life 0.6–1.4, size 1 | Force (0, 30); drag 1.6; Wobble x 4 px at 0.37 Hz, y 3 px at 0.48 Hz | Pixel Dot; Ramp `hot` (bright → dark over life); Flicker below 0.25 at 40 Hz; Cutout |
| `pixel_grit` | Burst 16, Hemisphere up, speed 60–200, life 1–1.6, size 1–2 | Force (0, −950); drag 0.8; Collision at y = −8, bounce 0.25, dampen 0.5, lifetime loss 0.5, settleSpeed 130 | Pixel Dot, Shade Bottom; Start Color Ramp `dust` (random step); Cutout |
| `pixel_debris` | Burst 10 (Sub Emitters: Birth → shards 5, chunks 5), Cone 60°, speed 80–220, life 1.5–2.5, size 3–6, rotation speed ±720°/s | Force (0, −950); drag 0.3; Collision at y = −12, bounce 0.35, dampen 0.3, settleSpeed 70 | Pixel Shard (secondary Color white) or Chunk (Ramp `stone`); Fade over Lifetime 1, then to 0 over the last 0.4 s; Dither 4 |
| `pixel_light_flash` | Burst 1, life 0.25, size 24 | none | Pixel Glow, 3 bands; blend Additive; Start Color Ramp `volt` step 5; Fade over Lifetime `(1 − t)²`; Size over Lifetime `1 − 0.45t`; Dither 2 |
| `pixel_smoke_loop` | Analytic, looping 4 s, rate 6 (N_loop 24), Circle r 2 | g (0, 16), drag 1.4; Wobble x 3 px at 0.25 Hz (4 s period) | as `pixel_smoke` (no sub-emitters) |
| `pixel_ember_loop` | Analytic, looping 3 s, rate 8 (N_loop 24), Edge 10 px | g (0, 30), drag 1.6; Wobble x 4 px at 0.333 Hz | as `pixel_ember` |

- **Tuning ranges:** where pixelfx draws from a range in code (e.g. `R.range(0.8, 1.4)`), the
  preset uses Random Between Two Constants with the same bounds. Where the source has no
  emitter (pixelfx effects spawn by hand), the emission values are ours, chosen to read well
  at 320×180, and marked "ours" in the docs.
- **Wobble periods:** a looping preset's wobble frequency is a multiple of `1/D`, so the loop
  stays exact.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R2, D4) | `engine/tests/core/particles/PixelSnapParticleTest.cpp`: the CPU reference box placement for sizes 1–9 at 1,000 random sub-pixel centres always covers exactly n×n whole pixels; under S41's grid it matches `pixelSnapRound` bit for bit; corner snap equals centre snap for even n. |
| AC2 (R2, R3, R9, R22) | `engine/tests/core/render/ParticlesGpuTest.cpp` `PixelShapes` on glcore, gles3 and vulkan: a 64×64 target with each shape at sizes 1, 2, 3, 5, 8 and rotations 0/37°/90°; the read-back pixels equal the CPU reference rasterisation of R9, pixel for pixel; goldens committed. |
| AC3 (R4, D5) | `ParticlesGpuTest.DitherFade`: a 16×16 Dot at a = k/16 shows exactly k pixels per 4×4 cell (Bayer 4) and k/4 per 2×2 (Bayer 2); Screen space keeps the pattern fixed when the particle moves 1 px; Particle space moves it; no pixel has alpha ≠ 1 under Dither/Cutout. |
| AC4 (R5) | `engine/tests/core/particles/HoldFramesTest.cpp`: with hold 12 Hz on a 60 Hz loop the packed instance buffer changes only on 5-frame boundaries (hash per frame); pack calls drop by ≥ 70 % (counter). |
| AC5 (R11) | `PixelLineTest`: for every angle 0–359° in 1° steps and lengths 1–16 the Streak and Shard rasterisations have exactly one pixel per major-axis step and no L-corner (no pixel with both a horizontal and a vertical neighbour along the line). |
| AC6 (R12, R13) | `ParticleRampTest` (CPU): `rampAt`/`fitRamp` stepping, bias, reverse, Start Color random step distribution (χ² over 10⁵ draws). `ParticlesGpuTest.ExactColors` on three backends: all 256 sRGB8 values round-trip the particle colour path exactly; a Ramp-coloured Pixel effect under neutral post has every non-background pixel equal to a ramp colour (set inclusion). |
| AC7 (R15–R17) | `engine/tests/core/particles/AnalyticParticlesTest.cpp`: unsupported modules give `analytic_unsupported` with paths; capacity 65,537 gives `analytic_capacity`; for a looping effect `evaluate(s, t) == evaluate(s, t + D)` bit for bit for all slots at 100 random t; the alive count equals a brute-force enumeration of births; seeking to t = 3,600 s gives the same frame as t mod D. |
| AC8 (R18, R19) | `ParticlesGpuTest.AnalyticMatchesCpu` (glcore, gles3, vulkan): 4,096 analytic Dot particles rendered as 1-px dots at 10 times; each lit pixel is matched to the CPU twin's snapped position: ≥ 99.9 % exact, the rest at Chebyshev distance 1; no extra or missing particles. |
| AC9 (R18, §4) | `AnalyticParticlesTest.MatchesPixelfxFly`: for 200 random (v₀, g, k, t), positions equal a direct transcription of `particles.js:10-20` (double) within 1e-4 px, the small-k series included. |
| AC10 (R14, R20, §6.6) | `editor/tests/pixelart/PixelPresetsTest.cpp`: each preset validates; `pixel_spark`'s single-particle trajectory (fixed seed, no collision) at 120 Hz matches a transcription of `sim.js:97-106` within 1 px over 0.5 s; with the floor, the first bounce's outgoing vertical speed is 0.45× ± 2 %; `pixel_grit` and `pixel_debris` particles come to rest (speed 0) and stay until death (`settleSpeed`); `pixel_fire` spawns smoke at death with probability 0.35 ± 0.03 over 10⁴ deaths. |
| AC11 (R20, R21, R38) | `tools/mcp/tests/test_particles_pixel.py`: `create_particle_effect` with each `pixel_*` preset, then `set_particle_effect`; `capture_particles frames 8 scale 4 checks true` returns one sheet image of the expected size and checks all PASS for every preset (red controls: Spark with `snap: false` under a Pixel Perfect Camera fails `off_grid`; with Fade Alpha it fails `partial_alpha`; a looping Simulated smoke with a short duration fails `loop_seam`; the Analytic loops PASS it); Bake to Sprite Sheet writes the PNG and JSON, undo removes them. |
| AC12 (R36–R40) | `test_particles_pixel.py::test_patch_paths`: every example patch of R37 applies and round-trips through `get_particle_effect`; an Analytic patch with Noise enabled is refused with its diagnostic. `tools/mcp/tests/test_particles_lua.py` (extended): a Play script sets `ps.renderer.renderMode = ParticleSystemRenderMode.Pixel`, `ps.pixelArt.fade = ParticleFade.Dither`, `ps.colorOverLifetime.color:setRamp(...)`, `ps.simulationMode = ParticleSimulationMode.Analytic`; `getAnalyticParticle` returns the twin's values; `tools/lua_api/tests` find the bindings; the docs' Lua blocks run. |
| AC13 (R25–R29) | `tools/perf/particles_bench` (S20's) on CI: the reference stack unchanged within ±3 %; Pixel Spark 20 k at 320×180×6 ≤ 0.3 ms GPU (S15 timings); Analytic 65,536 ≤ 0.5 ms GPU and ≤ 5 µs CPU per emitter; Analytic seek ≤ 0.1 ms at any t; web build in headless Chrome for the Analytic CPU numbers. Mid-mobile numbers measured once on the reference phone (manual, M7) and recorded in the evidence table. |
| AC14 (R41, R42) | `ParticleEffectCodecTest`: every fixture `.particles` and inline effect re-saves byte-identically; `ParticleDeterminismTest` hashes of every existing fixture effect are unchanged (the module-id append does not move random streams); a file with `pixelArt` read by the codec of the base commit fails with "unknown module" (recorded red control, run once on the base). |
| AC15 (R30–R34) | `tools/mcp/tests/test_particles_editor.py` (extended; xvfb, isolated editor): add Pixel Art, choose Pixel/Streak (icons render), set a Ramp from the Palette window by drag, switch to Analytic (banner, "Disable unsupported modules", one undo), scrub the overlay to 10 random times and compare with `capture_particles` at the same times (identical images), toggle Pixel Grid; screenshots in the evidence. |
| AC16 (R22, R24) | `tools/build.sh web` and `android`; `tools/smoke-test.sh` on a fixture with a Pixel Spark and an Analytic smoke loop: the exported game's screenshot at a fixed time equals the editor's Play screenshot (Analytic exactly; Simulated within S20's tolerance), on GL, GLES3 and Vulkan (`SMOKE_BACKEND`). |
| AC17 (§6.1) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_protocol.py`. |

## 8. Dependencies & conflicts

- **Depends on.**
  - **S20** (merged): the whole particle system.
  - **S43** (wave 5A): the Palette asset, ramp references `#ramp:<name>` (R4a), `fitRamp`/`rampAt`,
    `builtin:pixelfx-effects` with the `hot`, `fire_cooling` and `smoke_thin` ramps (R9), the
    Default Palette (R4b), `includes/bayer.glsl` (included as is: no local copy), and the
    lossy-texture Fix pattern.
  - **S41** (wave 5A): `pixelart::snap`, `includes/pixel_snap.glsl` (`pixelSnapRound`,
    `pixelSnapToPixels`/`FromPixels`, `pixelSnapCenterDelta`, `pixelSnapRotationStep`, frame
    `pixelSnap`), the project PPU and the `SnapGrid`.
  - **S44** (wave 5A): `lint/LintTypes.h` for the R38 checks.
  - **S48** (same wave): `EffectLints` for six of the R38 checks (soft: `SKIP` until it lands).
  - **S45** (same wave, soft): the json-hash import of Bake to Sprite Sheet's output.
  - Existing: S04, S06, S14, S15, S24, S27.
- **Blocks.** Nothing hard. S48's dissolve embers call `PixelParticles::emitAt` (R21a; S48 mocks
  it until S49 lands); S52's sort sees pixel particles as ordinary particle draws. S42's
  `particles_grid` rule reads the Pixel Art module (S42 R10).
- **Interfaces other specs see.**
  - `RenderMode::Pixel`, `PixelShape`, the `pixelArt` module keys and the Ramp gradient mode
    (`ramp`, `rampSteps`, `rampReverse`, `rampBias`). S48 may reuse the Ramp gradient for its
    particle-like effects.
  - `AnalyticParticles` (the closed-form twin) as a reusable library for S48's time-driven
    effects.
  - The `capture_particles` check ids (S48 R28's, plus `off_grid` and `over_budget`).
  - `particles::PixelParticles::emitAt` (R21a).
- **Merge hotspots.**
  - `ParticleEffect.h`, `ParticleModules.cpp` (`all()`, enum JSON names `:125-138`),
    `ParticleCompiler.cpp`, `ParticleRender.{h,cpp}`, `RenderSystemParticles.cpp`.
  - `ShaderPool.{h,cpp}` (variant bits).
  - `includes/particles_vs.glsl`/`_fs.glsl` (S41 adds its snap there too: S49 calls S41's
    functions; the Pixel branch replaces S41's centre snap for pixel boxes, and the order is
    documented in the file header).
  - `InspectorParticles.cpp`, `ParticleEffectOverlay.cpp`, `ParticleTools.cpp`,
    `ParticleClassesLua.cpp`.
  - `docs/mcp/guide/particles.md`.

## 9. Risks & defaults

- **Wobble instead of force sway.** The presets differ from pixelfx in micro-motion. Default:
  accepted. AC10 pins the main trajectories, and the docs say the sway is a render offset.
- **GPU vs CPU float differences in Analytic** (`exp` precision). Default: the 99.9 %
  exact / ±1 px tolerance of AC8. Exact equality is promised only frame-to-frame on one GPU.
- **Drag per axis.** pixelfx uses different x/y drags for smoke and fire; S20's linear drag is
  isotropic. Default: averaged in the presets (§6.6). A per-axis drag is a later S20 change, not
  S49's.
- **Analytic World space with moving emitters** looks wrong. Default: the warning, and the
  ambient presets use Local or static emitters.
- **The `particles` group budget is unmeasured.** Default: the §6.1 fallback (preset names to the
  guide) if needed.
- **Hold Frames and physics-driven emitters** (a moving parent) can show the emitter moving
  between held frames. Default: the emitter transform is held too (sampled at the step), and
  the docs say so.
- **Perspective cameras** have no world pixel grid. Default: screen-pixel shapes without world
  snap (R2), and S42's validator flags pixel particles under perspective cameras.

## Milestones

- **M1** The Ramp gradient mode, the Pixel Art module data and codec, Collision `settleSpeed`
  (R1, R3, R6, R12, R14, R41–R43); AC6 (CPU), AC14.
- **M2** Pixel snapping, the Pixel render mode, the shapes, dither, the pixel stream, exact
  colours (R2, R4, R7–R11, R13, R22, R23); AC1–AC3, AC5, AC6 (GPU).
- **M3** Hold Frames (R5); AC4.
- **M4** Analytic: compiler, CPU twin, vertex program, slot stream, loops, diagnostics
  (R15–R19, R24); AC7–AC9.
- **M5** Presets and Bake to Sprite Sheet (R20, R21); AC10.
- **M6** Editor UX (R30–R35); AC15.
- **M7** MCP and Lua extensions, checks, budgets, performance, export (R25–R29, R36–R40);
  AC11–AC13, AC16, AC17.

## Deviations

Recorded by the S49 implementation (`feature/s49-pixel-particles`).

- **S48 is not implemented.** `capture_particles {checks}` reports S48's ids (`off_palette`,
  `partial_alpha`, `spill`, `one_shot_end`, `loop_seam`, `determinism`) as SKIP with "needs S48
  (Pixel FX)" (`editor/pixelart/particles/ParticleCaptureChecks.cpp`); S49's own `off_grid` and
  `over_budget` run. AC11's `partial_alpha` and `loop_seam` red controls wait for S48.
  `PixelParticles::emitAt` (R21a) is provided for S48 to call; it adds one system entity per
  (scene, effect) and is not itself an undo step (the particles are runtime state, not data).
- **Variant bits** (R23): `Pxl` is particle bit 12 and `Ana` bit 13 (count 14), pinned by
  `PixelSnapShaderTest`; the shader cache version is taken at the batch landing.
- **AC2, AC3, AC6 (GPU) and AC8** live in their own suite, `engine/tests/core/render/ParticlesPixelGpuTest.cpp`
  (`ParticlesPixelGpu.*`), beside S20's `ParticlesGpuTest.cpp`. Per the owner's per-feature gate
  they ran on glcore only; gles3 and Vulkan run at the batch landing.
- **AC13 and AC16** (benchmarks, mobile numbers, web/android builds and the smoke export) are not
  run per feature (owner decision of 2026-11-08); they belong to the batch landing.
- **AC15** is covered through the Inspector's command path in `test_particles_editor.py::test_pixel_analytic_scrub`
  (Pixel Art added, Pixel/Streak, a Ramp, Analytic with one undo step, ten exact scrubs whose
  captures do not depend on the scrub's path), plus screenshots; the shape icons, the palette drag
  and the Pixel Grid toggle are not driven by a UI robot.
- **Analytic Velocity over Lifetime and Force** follow R15 (constant, dragged with the start
  velocity; force folded into g). Analytic Start Color as a Ramp takes the random step from the
  same stream as Simulated mode. A Hold Frames rate on an Analytic effect quantises t (R5); the
  compiler clears the Simulated pack-hold for it.
- **Hold Frames packing** keeps the last pack per (emitter, view) in `HeldPack` and repacks only
  when the step changes; AC4's "pack calls drop by >= 70 %" is measured with `packCount()`.
- **AC10's spark path.** S20 moves a particle by the tick's mean velocity, pixelfx by its end
  velocity, so the two paths differ by (v₀ − v)·dt/2 by construction (1.4 px at 0.5 s for a fast
  spark); `PixelPresetsTest.SparkFliesAsPixelfx` takes that known offset out and holds the rest to
  the 1 px budget (0.24 px measured by the same model). AC9's reference uses pixelfx's formula
  where it is well conditioned and its exact series below k·t = 1e-3, where the formula cancels.
- **The Analytic block's random keys** are 16-bit halves stored as floats (`AnalyticBlock::keys`,
  `analyticKey`): the GL backends flatten a uniform block only when all its members are one type,
  and an unflattened block is limited to sokol's 16 members.
- **A sub-emitter spawner** (Render Mode None with sub-emitters, the Debris preset) no longer gets
  S20's "None draws only trails" warning.
- **Sub-emitter particles are simulated but not drawn (S20 gap, open).** S20's renderer draws a
  system's own emitter only (`RenderSystem::drawParticles`); its sub-emitter children run (counts,
  events, bounds) but no draw call shows them. So the Fire preset's smoke and the Debris preset's
  shards and chunks do not show in the Scene view, the Game view or a contact sheet yet, though
  `PixelPresetsTest` proves their motion and `capture_particles` counts and checks them. Drawing
  children is an S20 renderer change (a render data per child emitter), left to its own task.
- **Presets.** Sub-emitter children (`pixel_smoke` of `pixel_fire`; `pixel_debris_shard` and
  `pixel_debris_chunk` of `pixel_debris`) are written beside the created asset; `create_particle_effect`
  lists them in `children`. Rates the pixelfx demos do not state (smoke, fire, ember) are ours,
  as `docs/particles-pixel.md` says.

