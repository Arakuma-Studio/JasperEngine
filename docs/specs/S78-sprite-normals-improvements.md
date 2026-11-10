# S78 — Sprite normals improvements: Light Steps, 2D ambient without lights, stone gaps, boxy and low-light warnings

Builds on the sprite normal-map generator (`editor/spritenormals/`, contract
`docs/superpowers/specs/2026-10-27-sprite-normals-contract.md`, history only) and the 2D light path
(`engine/shaderlib/includes/lighting2d.glsl`, `Light2DComponent`). Inspired by SpriteCook's
"Pixel art normal maps from the outline" (Reference). Everything here works the same for pixel art
(islands of 128 px and below) and high-resolution sprites.

## 1. Goal

The generator already makes the article's outline normal maps. What a user still cannot get is the
article's *look* and its pitfalls caught early:

- the hard, stepped pixel-art lighting bands (the light quantized at shading time);
- a dark 2D ambient that holds whether or not a Light 2D exists (Godot's CanvasModulate);
- floor tiles whose dark gaps bevel every stone;
- a warning before a crate becomes a pillow;
- a warning when a light sits too low over a sprite, and scripted lights that respond to normal
  maps like editor-made ones.

## 2. Scope

**In scope**
- Scene setting **Light Steps** (2D lighting): the 2D light quantized per light into N bands, in
  the shader, free when off.
- The scene's **Ambient Light 2D** applied to 2D-lit materials when no Light 2D exists.
- Generator setting **gaps** ("Dark Gaps Are Edges") and preset **stones**.
- Generator assessment warning **boxy**; scene warning **light_low**; the existing `ambient_high`
  reading the ambient's brightness, not only its intensity.
- A Light 2D created by Lua or the C++ object API starts at height 64, as editor-made ones do.
- Light 2D Inspector notes: height 0 ignores normal maps; a bright 2D ambient washes lights out
  (one-click fix).
- The Sprite Normal Map window's lit preview and the MCP contact sheet honour Light Steps.
- Editor UI with undo, Lua, MCP, guide text and eval cases for each of the above.

**Out of scope (default decisions)**
- Per-light or per-material step counts (one scene value keeps every lit sprite's bands
  matching, as the article does by sharing one material).
- Bright-grout tiles (gaps lighter than stones); a colour-picked gap mask (regions with `lift`
  remain for that).
- Specular 2D light, light-position snapping to the pixel grid.
- Changing the stored default of Ambient Light 2D (white, 1.0) or of `Light2DComponent::height`
  (0): old projects must load and look as before.
- Normal-map import changes (resampling of non-multiple-of-4 maps): a separate change.
- Any SpriteCook service or code: only the published technique is used.

## 3. Current state (read from the code)

- 2D lighting runs only when a Light 2D exists: `hasLights2D = numLights2D > 0`
  (`RenderSystem.cpp`, `processLights`), so a non-white ambient with no lights is ignored.
- `getLight2DIntensity` (`lighting2d.glsl`) sums `pow(1 - d/range, falloff) * N·L` (× shadow,
  colour, intensity) per light, continuously; no banding. `atlasInfo.z` of `u_fs_lighting2d` is
  uploaded and read by no shader.
- MESH shader property bits 0-53 are taken (`ShaderKeyTest.cpp`); `USE_LIGHT2D` is bit 22.
- The generator's Toon steps quantize normal *directions* (facets), not the light.
- An opaque cell (wrap/extend, or no transparency) has no distance field: shape height 0, only
  `detail` relief (Phase 2 §3.4). No mode turns dark pixels into edges.
- `assess()` warns `creased` only above 128 px; nothing flags a boxy island on `rounded`/`inflate`.
- `sceneWarnings` warns `no_light`, `no_light_height` (height exactly 0) and `ambient_high`
  (intensity ≥ 0.8, colour ignored).
- `Light2D::Light2D(Scene*)` keeps the struct's height 0; only `CreateEntityCmd` and
  `AddComponentCmd` set 64.

## 4. Reference

- SpriteCook, "Pixel art normal maps from the outline"
  (https://www.spritecook.ai/blog/pixel-art-normal-maps-from-outline, Nov 2026): outline-distance
  height with a quarter-circle bevel (our `dome`), mean-centred luminance detail, a 6-band
  `ceil(lit * bands) / bands` light shader, floor tiles masked at the darkest 30 % of a blurred
  grey, light height ≈ 2× the top height, CanvasModulate ≈ (0.24, 0.25, 0.33). Technique only; no
  code copied.

## 5. Requirements

### Light Steps (runtime)

- **R1.** `SceneSettings::light2DSteps` (int, 0..16, default 0 = off). Lua `Scene.light2DSteps`,
  YAML key `light2DSteps`, cooked scene settings, MCP `set_scene_settings` / `get_scene_settings`
  `light_2d_steps`, Inspector "Light Steps" in the scene's 2D lighting rows, undoable.
- **R2.** With N > 0, each Light 2D's term `t = atten * N·L` (in 0..1, before shadow, colour and
  intensity) becomes `ceil(t * N) / N`. 0 stays 0. The shadow factor, colour and intensity
  multiply after, unchanged. Ambient is not quantized.
- **R3.** Free when off: N = 0 compiles and draws exactly today's variants (no new uniform reads,
  no branch). N > 0 selects a new MESH property bit (54, `'Lst'`, define `LIGHT2D_STEPS`) only on
  variants that already have `USE_LIGHT2D`; the count travels in `u_fs_lighting2d.atlasInfo.z`.
  The shader cache version is bumped; `ShaderKeyTest`, the variant collector and the shader lists
  know the bit.
- **R4.** Every surface the 2D light path lights gets the bands: sprites, tilemaps, polygons,
  lit meshes in a 2D scene, the uniform and the light-records paths.
- **R5.** All five backends: the variant cross-compiles for GL 3.3, GLES3/WebGL2, Metal, D3D11,
  Vulkan (the editor's shader compile for every target).

### Ambient without lights (runtime)

- **R6.** A scene whose Ambient Light 2D (linear colour × intensity) differs from (1,1,1) by more
  than 1e-4 in any channel tints every 2D-lit *unlit* material (sprites, tilemaps, polygons:
  the path that multiplies the base colour) with that ambient even when it has no Light 2D, with
  the result it would have with one Light 2D out of range. Lit (PBR) materials with no Light 2D
  in the scene are unchanged. A neutral ambient with no Light 2D draws exactly as today.
- **R7.** Deliberate behaviour change: a saved scene with a non-neutral 2D ambient and no Light 2D
  now renders tinted. No version bump: the value's meaning is unchanged; it was not applied.

### Stone gaps (generator)

- **R8.** `Settings::gaps` = `{share}` (float, 0..0.9, default 0 = off), JSON/recipe key
  `"gaps": {"share": x}` written only when on; old recipes regenerate byte-identically.
- **R9.** With share > 0, per cell: the Oklab lightness of pixels above the alpha cutoff, blurred
  (Gaussian σ = 0.8 × k px; toroidal for `wrap`, clamped for `extend`, masked for `transparent`);
  the pixels at or below its `share` quantile are gap pixels. Gap pixels count as outside the
  shape for the distance field, the inflate solve, the auto radius and islands' thickness. They
  keep their alpha and get normals as other pixels do; `detail`, regions and paint apply as
  before. Deterministic for any thread count.
- **R10.** Works with every `edges` mode and profile; with `wrap` the result tiles seamlessly
  (3×3 tiling has no seam).
- **R11.** Preset `stones`, appended after `tile`: edges wrap, profile dome, gaps 0.3, and values
  tuned on a pixel-art (32 px) and a high-resolution (512 px) stone tile so each stone reads as a
  rounded slab. The `tile` preset is unchanged.
- **R12.** A gaps setting that marks no pixel (flat colour) or every pixel gives the warning
  `gaps_unmatched` with a fix.

### Warnings

- **R13.** `boxy` (assessment): an island (`transparent` edges only; `wrap`/`extend` cells are
  tiles) whose resolved profile is `rounded` or `inflate` and whose opaque pixels fill ≥ 0.9 of its
  bounding box, at any size. Fix: preset `props`, or profile `bevel` with
  radius `max(1, round(0.05 × shorter side))`. Not raised for `dome`/`bevel`.
- **R14.** `light_low` (scene warning, next to `no_light_height`): the enabled Light 2Ds with height
  > 0 whose height is below 0.5 × the sprite's larger side in world units (its frame size in px
  × world scale ÷ pixels per unit). Names up to three lights. Fix: set their height to at least
  the sprite's larger side (rounded up). Pixel art and high-resolution sprites alike.
- **R15.** `ambient_high` fires on brightness = intensity × the largest linear colour channel
  ≥ 0.8 (a dark-blue ambient at intensity 1 no longer warns; white at 0.8 still does). One helper
  answers "is the 2D ambient bright" for every caller.

### Lights

- **R16.** `Light2D::Light2D(Scene*)` (a new entity: Lua `Light2D(scene)`, C++) sets height 64.
  `Light2D(Scene*, Entity)` (wrapping an existing one) and the loader keep stored values; the
  struct default stays 0. Deliberate behaviour change for scripts that relied on 0.

### Editor UX (human)

- **R17.** Scene settings: "Light Steps" (0 shown as "Off") beside Ambient Light 2D, undoable.
- **R18.** Sprite Normal Map window: "Dark Gaps Are Edges" checkbox + "Gap Amount" slider (%) in
  Settings (not Advanced), with tooltips; the Stones preset button; warnings `boxy`, `light_low`,
  `gaps_unmatched` with one-click fixes (Use Bevel, Raise Light(s), adjust Gap Amount), worded in
  the window's labels; every fix undoable.
- **R19.** The window's lit preview takes a "Light Steps" control: "Scene" (the target scene's
  value, the default), "Off", or 1..16; the CPU lit preview uses the R2 formula exactly.
- **R20.** Light 2D Inspector: at height 0 a note "Height 0: normal maps are ignored" with "Set
  Height 64"; with a bright 2D ambient (R15) a note with "Darken Ambient" setting Ambient Light 2D
  to colour (0.24, 0.25, 0.33) sRGB, intensity 1. Both undoable.

### Agent UX

- **R21.** `generate_sprite_normals`: `params.gaps`, preset `stones`, the warnings above with fix
  text; the contact sheet's lit columns use the target scene's Light Steps (or
  `params.light_steps`, 0..16) and the result states the steps used.
- **R22.** `generate_sprite_normals` results put the write step and the sheet legend in
  `structuredContent.next` / a structured field (Claude Code shows the model only that part).
- **R23.** Creating or editing a Light 2D through MCP (`create_object` light_2d,
  `set_properties` light2d) returns a `next` hint when the scene's 2D ambient is bright (R15).
- **R24.** Each new text has an eval case in `tools/mcp/eval/cases.py` that fails without it;
  `docs/mcp/TOOLS.md` regenerated; `tool_budget.py --check` passes (lower the budget when cut).
- **R25.** The sprite-normals guide documents stones/gaps, Light Steps and the new warnings; the
  2D lighting guide section says a light needs height for normal maps and how Light Steps and
  the ambient behave.

### Data and compatibility

- **R26.** Optional new fields with defaults: `light2DSteps`, `gaps`. No YAML version bump. The
  cooked scene settings format changes as earlier fields did (cooked data is rebuilt by export).
  `StreamSchema` lists `light2DSteps`; `check_stream_keys.py` passes.
- **R27.** Old projects load and render as before, except R7 and R16.

## 6. Design decisions

- **D1. One scene-wide step count**, not per light: the article's look needs every lit sprite
  stepped alike; a per-light value would need a fourth vec4 per light (+256 B uniforms).
- **D2. `ceil`, as the article**: any lit pixel gets at least one band, so the light's edge is a
  hard ring, which is the pixel-art look; 0 stays 0 so unlit stays unlit.
- **D3. A variant bit, not a uniform branch**, for "free when off".
- **D4. Gaps change the shape mask only**, so every profile, auto radius and edges mode work on
  stones unchanged; the quantile (not a fixed darkness) makes one value fit any tileset's palette.
- **D5. Sizes relative to the sprite**, never fixed pixels, in every new threshold, so pixel art
  and high-resolution sprites get the same advice.

## 7. Acceptance criteria

| AC | Covers | Evidence |
|---|---|---|
| AC1 | R1, R26 | Unit: YAML and cooked round trip of `light2DSteps`; Lua get/set; an old scene loads with 0 |
| AC2 | R2-R4 | Golden (GL): a normal-mapped sprite and a tile under one Light 2D at steps 0 and 6; at 6 the image shows ≤ 7 distinct light levels per light; steps 0 matches today's golden |
| AC3 | R3 | `ShaderKeyTest`: bit 54 `'Lst'`; steps 0 collects no `'Lst'` variant |
| AC4 | R5 | The `Lst` variant compiles for every backend target |
| AC5 | R6, R7 | Render test: no Light 2D, ambient (0.3,0.3,0.4): sprite tinted; neutral: identical to today, no `L2d` variant |
| AC6 | R8-R11 | Core unit: a 32 px and a 512 px stone tile with gaps 0.3 give per-stone plateaus; off is byte-identical; wrap 3×3 seamless; recipe round trip; deterministic over thread counts |
| AC7 | R12-R15 | Unit: each warning raised and not raised (square on rounded vs dome; low vs high light at 16 px and 512 px; ambient white 0.8 vs dark blue 1.0) |
| AC8 | R16 | Lua test: `Light2D(scene).height == 64`; a loaded light keeps 0 |
| AC9 | R17-R20 | MCP/editor tests drive the controls and fixes; screenshots from a scratch project |
| AC10 | R21-R25 | MCP tests for params and results; eval cases red without the text, green with it |

## 8. Work packages

- **WP-A (runtime):** R1-R7, R17, the R21 contact-sheet step source after merge.
- **WP-B (generator):** R8-R13, R18 (gaps, stones, boxy), R19 (preview control), R21-R22, R25 (sprite-normals guide).
- **WP-C (lights):** R14-R16, R18 (light_low fix), R20, R23, R25 (2D lighting guide).

## Deviations

- **R9 (WP-B), the blur's k:** the gap blur uses k = max(1, S / 128), S the shorter side of the cell's
  pixels above the cutoff (an opaque tile's k), because the items' thickness k needs the gaps first.
- **R9 (WP-B), the auto radius:** with gaps on, every stone (a part of the shape between gaps) gets its
  own auto radius from its own thickness and shorter side, as an island does; one radius from the
  largest stone left the smaller ones domes without a flat face.
- **R12 (WP-B):** "no pixel or every pixel" is one condition: a cell whose pixels above the quantile have
  a median less than 0.02 (Oklab L) above it (flat or evenly lit) gets no gaps and the warning.
- **R13 (WP-B):** boxy looks at each frame's largest island with a shorter side of 3 px or more (specks
  and 1-2 px lines are no boxes).
- **R18 (WP-B), "every fix undoable":** the Sprite Normal Map window keeps no undo history of its
  settings (every setting change, the earlier Switch to OpenGL fix included, is a live edit that the
  preset's reset arrows take back); the boxy and gaps_unmatched fixes are setting changes like them.
- **R19, R21 (WP-B):** the window's Light Steps control is Off or 1..16 and `params.light_steps`
  defaults to 0; the "Scene" choice and the scene's value as the default come with the integration of
  WP-A's `SceneSettings::light2DSteps`.
- **Tests (WP-B):** `jasperengine-spritenormals-tests` links the engine library: the texsynth sources it
  compiles call the engine's atomic file writes and named threads.
