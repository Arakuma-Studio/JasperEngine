# Sprite Normals: normal quantization ("Toon steps") — design

Status: implemented on branch `sprite-normals-quantize` (from main 34b083838). Owner request 2026-10-28: "do it too"
(normal quantization: snap normals to a few fixed directions for crisp, cel-shaded pixel-art lighting). This is the
binding spec as given, with the toon preset values chosen and the clarifications made while implementing (marked
**Clarification**).

## Goal
Pixel-art lighting usually looks best in a few flat bands, not smooth gradients. Quantization snaps every generated
normal to a small palette of directions, so a moving light produces crisp, stepped shading (like hand-shaded pixel art),
with no new artefacts and deterministic output. Off by default: with quantization off, every output is byte-identical
to today (golden parity, all existing tests unchanged).

## Setting (same name on every surface)
`quantize` — object, default off:

| key | type, range | default | meaning |
|---|---|---|---|
| `directions` | 0, 4, 6, 8, 12, 16 (0 = off) | 0 | number of azimuth steps around the circle |
| `tilts` | 1..4 | 2 | number of tilt rings between `flat_deg` and 90° |
| `flat_deg` | 0..45 | 10 | normals tilted less than this become exactly flat (0,0,1) |
| `rotation_deg` | 0..360 (360 excluded) | 0 | rotates the azimuth palette (0 = one direction points right, +X) |

- JSON/MCP: `params.quantize: {directions, tilts, flat_deg, rotation_deg}` (inside the existing loose `params`
  object; unknown keys refused with the valid list; out-of-range values refused with the house message).
- CLI: `--quantize <directions>` plus `--quantize-tilts N`, `--quantize-flat DEG`, `--quantize-rotation DEG`.
- UI label: "Toon steps" (segmented Off / 4 / 8 / 16 in the main Settings, after Volume, with a tooltip explaining it);
  Advanced foldout: "Tilt rings", "Flat below", "Rotation" (and 6 / 12 via a combo or the segmented control's overflow).
- Recipe: `settings.quantize` written only when directions > 0 (so recipes/fingerprints of unquantized maps are
  unchanged); `specFromRecipe` restores it; regenerate reproduces identical pixels.
- Validation in `validateSettings`; parse/serialize in the recipe `settingsToJson/settingsFromJson`.

**Clarifications.**
- *Recipe key when the preset's quantize is on.* `settingsToJson` also writes `quantize` when the settings' *preset*
  has it on (only `toon`), even with directions 0. Every reader resolves a recipe over its preset
  (`presetSettings(p)` then `settingsFromJson`: the window's stored settings in Stream.cpp, MCP `regenerate`'s
  `paramsDiff`), so a toon map turned off must say so or it would come back on. Unquantized maps of the other presets
  still have no key (their recipes and fingerprints are unchanged, verified byte for byte below).
- *Whole numbers.* `directions` and `tilts` are integers: a fraction (`8.5`) is refused by `settingsFromJson`
  ("quantize.directions is 8.5, expected a whole number"); values past ±1e9 saturate there and `validateSettings`
  names them. `settingsFromJson` stays lenient on unknown keys inside `quantize` (recipes from a newer editor); the MCP
  handler refuses them (`params.quantize has an unknown key "steps"`, fix: the valid keys), as it does for `regions`.
- *Messages.* `quantize.directions must be 0 (off), 4, 6, 8, 12 or 16 (got 5)`, `quantize.tilts must be between 1 and
  4 (got 0)`, `quantize.flat_deg must be between 0 and 45 (got 46)`, `quantize.rotation_deg must be at least 0 and
  below 360 (got 360)`; fields `quantize.<key>` (MCP `params.quantize.<key>`; the CLI names the flag:
  `--quantize-tilts must be between 1 and 4 (got 9)`).
- *Window.* "Toon steps" is a row after Volume with segmented Off / 4 / 8 / 16 (6 and 12 select no button) and a
  reset-to-preset arrow when it differs from the preset; Advanced has a "Directions" combo (Off, 4, 6, 8, 12, 16),
  "Tilt rings" (1..4), "Flat below" (0..45°), "Rotation" (0..360°, 360 wraps to 0), disabled with a reason tooltip
  while Toon steps are off. Choosing a preset sets its quantize (off for all but toon), like its other values.
  The window has no Help page of its own (App's shortcut sheet lists keys; Toon steps has none): the explanation is
  the row's tooltip.

## Algorithm (core, editor/spritenormals, engine-free, deterministic)
Applied per pixel above the alpha threshold, to the generated OpenGL-convention normal, AFTER generation and BEFORE
paint compositing and the DirectX flip (painted pixels keep the exact direction the user painted):

1. t = acos(clamp(nz, -1, 1)) in degrees. If t < flat_deg → n = (0,0,1).
2. Else azimuth a = atan2(ny, nx); snapped a' = rotation + round((a − rotation) / step) · step, step = 360/directions.
3. Tilt ring: split [flat_deg, 90) into `tilts` equal bins; t' = the centre of the bin containing t (t ≥ 90 → last bin).
4. n' = (sin t' cos a', sin t' sin a', cos t'); then encode exactly like unquantized normals.
- Must be deterministic across thread counts; use double for the trig and snap ties deterministically (round half
  away from zero), then the existing float encode.
- Applies to height-map export? No — the height map stays the generator's height (document it).
- Metrics/assessment are computed on the final (quantized) result as today; add no new warnings.

**Implementation.** `quantizeNormal(const Quantize&, x, y, z)` and `quantizePalette(const Quantize&)` are public in
`SpriteNormals.h` (tests and tools use the palette). In `runItem` the quantized path computes the unit normal with the
same float operations as `encodeNormal` (`detail::slopeNormal`), quantizes in double, and encodes with
`detail::encodeUnit` (the same rounding, then `y = -y` for DirectX). `encodeNormal` itself is untouched, so with
`directions == 0` the code path and bytes are exactly as before.

**Clarifications.**
- *Wrap-around and ties.* (a − rotation) is reduced to [0, 360) before rounding, so −180° and +180° (atan2 of y = −0 and
  +0) snap alike and a tie always rounds up from the rotation (half away from zero on a non-negative value); k is taken
  modulo `directions`.
- *No azimuth.* A normal with x = y = 0 (exactly facing the viewer) stays (0, 0, 1) even with `flat_deg` 0: it has no
  azimuth to snap.
- *Flat regions* (`regions[].flat`) keep their forced (128, 128, 255); they are not quantized (they are already flat).
- *Paint.* `generatePainted` composites over `generate()`'s quantized result, so painted pixels keep the painted
  direction byte for byte and erased paint shows the quantized bytes again (tested).
- *Metrics.* Quantization adds no warning kind. The existing ones apply to the quantized result: with 2+ tilt rings
  the outer ring sits at 70° or more, so `too_steep` can fire on strongly domed sprites; the toon preset uses one
  ring and warns nothing the default preset does not (table below).

## Presets
Add preset `toon` (contract preset list becomes soft, default, crisp, detailed, chunky, toon): choose its six main
values + quantize empirically (start from strength 2.0, detail 1, smoothing 1.5, volume 1.3, quantize 8 directions,
2 tilts, flat 10°), evaluate with lit contact sheets on the eval sprites (/tmp/sfsn-eval/in/*.png and the golden
fixtures) and pick what reads best as cel-shaded pixel art; record the chosen values and why in the spec. Other presets
keep quantize off. Update every surface that lists presets (window segmented buttons, MCP schema enum, CLI usage,
guide, contract table).

### Chosen toon values

| strength | detail | smoothing | volume | softness | radius | quantize |
|---|---|---|---|---|---|---|
| 2.0 | 0.5 | 2.0 | 1.3 | 1.15 | auto | `{directions: 8, tilts: 1, flat_deg: 15, rotation_deg: 0}` |

How they were chosen: the CLI's lit contact sheet (colour | normal | lit from the four corners) of 17 sprites (the eval
set ball, character, donut, highlight, thin, flat_outline, sheet 32 px frames, walk.gif, spin.gif, tiles, alpha,
flat2, and the golden fixtures sprite9, islands_sheet, disk, tiny_diagonal, gradient_alpha) for 15 candidates in four
rounds, looked at side by side (sheets in `build/sprite-normals-analysis/q-*.png`, scripts `q-eval.py`,
`q-compose.py`, candidates `q-cands*.json`):

1. **The starting point (2 / 1 / 1.5 / 1.3, 8 directions, 2 tilts, flat 10)** reads as a cut gem rather than cel
   shading: the outer ring (tilt 70°) puts a dark/bright rim band around every rounded shape, and `too_steep` fires on
   10 of 12 sprites (56-70% of pixels over 60°) where the default preset warns on none (`q-round1-*.png`,
   `q-start-sheet.png`).
2. **One tilt ring** (every non-flat pixel at 45° + flat/2) gives the classic look: per light, one facet lit, its
   neighbours mid, the rest in shadow, two to three tones with clean edges; 0% steep, noise ≤ 8.5% (thin 1 px
   lines). Three rings were cleaner than two but busier than one; lowering strength to keep two rings mostly fills
   only the inner ring (softer, less contrast) (`q-round2-*.png`).
3. **Rotation 0, not 22.5.** 22.5° puts step boundaries on the axes: pixel-perfect diagonal terminators on a ball, but
   every straight-sided shape (tiles, crates, the donut's flat sides, torsos) has its edge normals exactly on a
   boundary, where painted detail flips them between two steps: speckle (`q-rotation-tiles.png`,
   `q-rotation-donut.png`). With 0 the axis normals are step centres. The guide says so.
4. **Less painted detail (detail 0.5, smoothing 2)** keeps facets following the silhouette: with detail 1 the face
   and belt of the character break the halves into fragments; strength 2 / volume 1.3 keep the dome strong enough
   that most pixels leave the flat cone.
5. **flat 15** (ring at 52.5°): a small flat top on large shapes and calmer near-flat interiors (donut noise 1.9% vs
   3.6% at 10); 20 flattens 1 px lines entirely (the thin sword loses its edge) (`q-flat-*.png`).

Final contact sheet: `q-toon-sheet.png` (beside it the default preset's `q-default-sheet.png`). Metrics of the toon
preset on the 17 sprites: steep 0% everywhere, noise 0-8.5%, flat 0-17% (tiny_diagonal 100%, as default's 90%), and
the warnings are the default preset's minus one (`leaning` on the sheet goes away): `no_alpha` (sprite9, tiles),
`frames_touch_edges` (sheet, walk), `soft_edges` (alpha, gradient_alpha), `mostly_flat` (tiny_diagonal).

## Surfaces to update
Core (SpriteNormals.h/.cpp: Settings, validate, preset), recipe, session fingerprint (via settings), window (controls +
live preview + tooltips + Help), MCP tool (params, schema enum for preset, guide row), CLI (flags, usage text, JSON
settings echo), docs (guide section "Toon steps" with a worked example; README sentence; contract; merge notes), tool
budget (stay under the 106,100 cap — put text in the guide, not the schema).

Done: the schema only gains `"toon"` in the preset enum (params stays a loose object); MCP `paramKeys` gains
`quantize` (unknown-key list, `params.` field names, `regenerate`'s `paramsDiff`); the CLI echoes `settings.quantize`
in its JSON (from `settingsToJson`); the session fingerprint and the window's stored settings go through
`settingsToJson` and need no change. The window's segmented controls name each button for the UI probe
(`spriteNormals.presets.<label>`, `spriteNormals.toonSteps.<label>`, `...Selected.<label>` when selected).

## Tests
- Core: off = byte-identical to before on every golden fixture (both green conventions); each output pixel is one of
  the palette normals (directions × tilts + flat) after encode/decode within 1 LSB; flat_deg boundary; rotation;
  azimuth wrap-around at ±180°; determinism across thread counts; paint over quantized output keeps painted values;
  DirectX flip after quantize.
- Recipe round trip; validation errors; MCP e2e (params.quantize preview/write, preset toon, bad values -> fix text);
  CLI e2e (--quantize flags, JSON echo); window test (Toon steps control changes the result, recipe restores it).
- Screenshot of the window with Toon steps on (save under build/sprite-normals-analysis/q-*.png) and LOOK at it.

Beyond the unit tests, "byte-identical to before" was checked against an editor built before this branch: 42 inputs
(the golden fixtures and the eval set) x the five old presets x both conventions, with height maps and GIF atlases:
900 PNGs, decoded pixels identical, no `quantize` key in any recipe.
