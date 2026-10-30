# Sprite Normals: high-resolution sprites — design (binding)

Owner request 2026-10-30: "do absolutely all the fixes 100% so it also works for high res sprites".
Evidence: build/hires-eval/REPORT.md (36 inputs, 14 CC0 real + 22 synthetic with true normals; scripts in
build/hires-eval/scripts/, incl. a numpy reference of the generator and the prototype inflate profile).
Commit this spec as docs/superpowers/specs/2026-10-30-sprite-normals-hires-design.md (with the final tuned values).

## Invariants
- Pixel art (every island/cell short side <= 128 px) with existing presets: output unchanged EXCEPT where auto radius
  is used (rule 3 changes it slightly; equal on discs). Golden upstream-parity fixtures (explicit radius) stay
  byte-identical.
- Old recipes (version 1) regenerate with version-1 semantics (bbox auto radius, no resolution scaling, no outer band,
  profile as recorded) so regenerate/staleness stay reproducible. New recipes are version 2 and record every new setting.
- Deterministic regardless of thread count; no new warnings; all surfaces (core, recipe, window, MCP, CLI, check, docs)
  use the same names/ranges.

## Changes
1. Resolution scaling — new setting `resolution_scale`: "auto" (default) | "off". k = max(1, S/128), S = the same size
   measure as auto radius (island shorter bbox side for stills; median over frames of the largest island's shorter side
   for sequences/cells). Effective smoothing, softness and detail strength are multiplied by k (settings stay in
   "128-px reference" units; document it). k = 1 at <= 128 px.
2. Limits: smoothing and softness max 8 -> 32 (detail max stays 8; k scaling covers large sizes). Validation messages and
   every surface's ranges updated.
3. Auto radius: R = clamp(0.3 x island's largest inscribed distance (max of the EDT), 4, 512) per island; sequences use
   the median over frames. (Equal to 0.15 x short side on a disc.)
4. New profile `inflate`: solve the Poisson problem Laplacian(u) = -1 inside the shape mask, u = 0 outside (boundary
   per rule 5, respecting edges modes: wrap periodic, extend = Neumann at the image border), h = 2 * volume * sqrt(u)
   (tune the constant so the best effective A ~= 0.85 as in the report). Robust, deterministic, fast solver (multigrid
   or preconditioned CG in float/double with fixed iteration/termination rules), parallel inside one island. Target:
   2048x2048 single sprite generation < 0.6 s on 8 threads, 1024 < 0.2 s.
   New profile value `auto` = the new default in all presets except where a preset says otherwise: rounded when the
   island's short side <= 128 px, inflate above. Profiles: auto | rounded | dome | bevel | inflate.
5. Outer-band soft-alpha rule: pixels with threshold < alpha < 0.5 that are connected (8-connectivity) to fully
   transparent outside pixels count as OUTSIDE the shape for the silhouette terms (distance field / inflate boundary),
   still keep their alpha and still get a normal (from the shape field evaluated there + detail); interior
   semi-transparent pixels (glass) stay inside. Version-2 behaviour only.
6. Assessment: new metric `roughness_deg` (mean angle between each normal and its sigma=4 px Gaussian blur over pixels
   > 8 px inside the alpha-0.5 silhouette, scaled with k) and a crease detector; new warnings `creased` and `streaky`
   with plain-language messages and fixes (e.g. "use profile inflate or dome, or raise softness"), thresholds
   calibrated on the eval set so pixel-art fixtures and good results don't warn. Today every hi-res result says
   "Looks fine." — that must no longer happen for the default-profile crate/potions in the eval set.
7. Presets: add `painted` (organic/painted/vector art: profile inflate), `props` (crates, panels, rings: profile dome),
   `tile` (edges wrap). Tune their values ON the eval set after rules 1-5 are in (report's starting points:
   painted str 3, vol 0.575, det 8, sm 8, soft 8; props dome str 1.5 vol 0.87 det 8 sm 4 soft 8; tile str 8 det 8 sm 8
   wrap) and record the measured errors/roughness vs `default` in the spec. Existing presets get profile `auto`.
8. Performance: parallelize within a large island deterministically (separable blurs by rows/cols, EDT lines, the
   solver). Report timings at 512/1024/2048 for every preset.
9. Surfaces: window (profile combo with inflate/auto, Resolution scale toggle in Advanced, slider ranges, preset row
   with the new presets — check it wraps cleanly), MCP schema enums within the 106,100 tool budget (pay back with
   prose the guide holds), CLI flags/usage, recipe v2 + v1 compatibility, check --normals, guide section
   "High-resolution sprites" with worked examples, README sentence, contract table, TOOLS.md.
10. Tests: core tests for each rule (scaling no-op <= 128 px, thickness radius, inflate exact on a disc within a
   tolerance, outer band keeps glass, recipe v1 reproduces old pixels exactly, determinism across threads, limits);
   regression fixtures: commit the synthetic generator script + small synthetic inputs, and 3-4 of the CC0 real
   sprites (downscaled if needed to keep fixtures small; attribution in a fixtures README) with error/roughness
   thresholds asserting the improvement; MCP/CLI e2e for the new settings/presets; window test for the new controls.

## Final values (implemented and tuned on the evaluation set, 2026-10-30)

Measured with the new CLI on `build/hires-eval` (scripts `v2_*.py`; report `build/hires-eval/IMPLEMENTATION.md`).

- **Inflate height** `h = kInflateHeight * volume * sqrt(u)`, `kInflateHeight = 1.0`: the presets' strength x volume
  of 1.725 is an effective slope of 0.86 (the report's 0.85). A volume sweep with profile inflate (strength 1.5) put
  the optimum at 1.25 when weighted by the six sphere variants and at 1.05-1.15 with spheres, tori and capsules
  weighted equally; 1.15 (the presets' value) is kept.
- **Presets** (128 px reference units):

| preset | profile | strength | detail | smoothing | volume | softness | edges | tuned on |
|---|---|---|---|---|---|---|---|---|
| `painted` | inflate | 1.5 | 3 | 1 | 1.15 | 1.15 | transparent | 18 combinations on 7 shape inputs + 3 cobble tiles: the default values were best (6.81° mean; smoothing 2: 7.22°, detail 4.5: 6.84°) |
| `props` | dome | 1.5 | 3 | 0.5 | 0.7 | 1 | transparent | 162 combinations on the rounded boxes + cobbles_1024: box 1.2-1.4° (softness 0.5 gave the same 0.1° better; 1 kept for smoother rims) |
| `tile` | auto | 2 | 3 | 1 | 1.15 | 1.15 | wrap | 27 combinations on 6 cobble tiles: detail x strength = 6 is the ridge (11.67°; 4.5 x 1.5 = 11.59°) |

- **Errors** (mean angular error, degrees; old = main before this change, default preset):

| set | old default | new default | new painted | new props | new tile |
|---|---|---|---|---|---|
| round shapes and variants (11 inputs, 512-2048) | 12.5 | 4.4 | 4.4 | 23.9 | |
| rounded boxes (2) | 24.8 | 25.2 | 25.2 | **1.4** | |
| cobble tiles (6, wrap) | 14.4 | 12.2 | | | **11.7** |

- **Roughness / creases** (rule 6) are measured on the silhouette's share of the normals (`Result::shape`: the
  same field without the painted relief), not the final map: with k-scaled detail the intended painted relief alone
  measured 1.4-8.2° on the good new defaults, so the full map could not separate creases from relief. Measured only
  above 128 px (k > 1); at 32 px an 8 px inset leaves the rounded apex and every pixel-art fixture would "warn".
  Crease pixel: deviation from the sigma-4k blur over 8°. Real art, 13 sprites over 128 px: old default
  7.73° / 29.1% creases, new default 0.46° / 0.0%, rounded profile 4.21° / 21%.
- **Warning thresholds:** `creased` when crease_pct > 2 (worst good result 1.24%, mildest crease 2.67%); `streaky`
  when roughness_deg > 1.5 and not creased (good results up to 1.29°). `soft_edges` is not raised above 128 px with
  version 2 (the outer band handles soft rims).
- **Outer band:** applied with version 2 to islands above 128 px only (the pixel-art invariant: the golden fixtures
  `alpha` and `gradient_alpha` have soft rims).
- **Version 1 and profile auto:** a version 1 generation resolves `auto` to `rounded` (version 1's default), so a
  version 1 recipe, `params.version: 1` or `--settings-version 1` alone reproduce the old bytes.
- **Timing** (generation, 8 threads, default preset): 512 px 0.03 s, 1024 px 0.11 s, 2048 px 0.41 s (all presets
  <= 0.58 s at 2048). Gaussians above sigma 16 run as a four-box cascade of the same variance.
