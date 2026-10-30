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

## Review round 1 rulings (2026-10-30, binding; they amend the items above)

- **R1 dome auto radius:** R = clamp(min(1.0 x thickness, 0.15 x the island's shorter bbox side), 4, 512) (other
  profiles keep 0.3 x thickness). `props` suits crates, panels and rings.
- **R2 resolution scale from the thickness:** k = max(1, 2 x the largest inscribed distance / 128) (sequences: the
  median over the frames of the largest island's; an opaque wrap/extend cell with no inside distance: its shorter
  side / 128). Profile auto and the outer band stay gated on the bbox shorter side > 128.
- **R3 metrics:** `crease_pct` on the shape field with the paint overlay composited (painted pixels take the painted
  normal), so painting over a crease clears it; `roughness_deg` (`streaky`) on the final map, its own threshold
  calibrated so good art does not warn.
- The outer band's "outside" is the non-mask pixels connected to the field's border (the padding ring with edges
  transparent, the cell's border with wrap/extend): a hole enclosed by the sprite never turns glass into outside.
- `normals-before-hires` is gated on the map's island size (the median over the recipe's cells of the largest
  island's shorter side, from its alpha), not the image size.
- A cancel during an inflate solve returns Cancelled; a solve that hits its iteration cap is reported
  (`inflate_not_converged`); coarse multigrid cells are inside only when all their children are (pinhole masks
  converge); the red-black sweep computes only its own colour (no read of a pixel another thread writes).
- Metric inset: min(8k, half the frame's largest depth) (thin shapes are measured); measured when the shorter side
  is above 128 px (k can be 1 on thin high-resolution art).

## Review round 2 rulings (2026-10-30, binding)

- **A** `crease_pct` is measured on the shape field; painted pixels and 3 sigma around them are excluded (not
  composited: a painted patch's edge is no crease).
- **B/C** The outer band's outside: every transparent region whose largest inscribed distance is at least max(2 px,
  5% of the island's shorter side), plus (edges transparent / extend) those reaching the field's border; with wrap
  the depth rule alone (independent of the cut: rolled tiles give rolled maps up to float summation order).
- **D** The band is skipped for an island it would take over 90% of (ghosts, bubbles keep their shape).
- **E** `measure()` takes the request's thread count; the metrics never depend on it.

## Final values (implemented and tuned on the evaluation set, 2026-10-30, after review round 1)

Measured with the new CLI on `build/hires-eval` (scripts `v2_*.py`; report `build/hires-eval/IMPLEMENTATION.md`).

- **Inflate height** `h = kInflateHeight * volume * sqrt(u)`, `kInflateHeight = 1.0`: the presets' strength x volume
  of 1.725 is an effective slope of 0.86 (the report's 0.85). A volume sweep with profile inflate (strength 1.5) on a
  sphere, torus, capsule and hoop: 0.95 -> 3.79, 1.05 -> 3.91, 1.15 -> 4.39, 1.25 -> 5.05 mean, the sphere best at
  1.25 and the tubes at 0.95; the presets' 1.15 is kept (the eval's sphere variants outnumber its tubes).
- **Presets** (128 px reference units), unchanged by round 1 (re-run grids: props is the grid's best, 3.84; painted
  within 0.02 deg of its best):

| preset | profile | strength | detail | smoothing | volume | softness | edges |
|---|---|---|---|---|---|---|---|
| `painted` | inflate | 1.5 | 3 | 1 | 1.15 | 1.15 | transparent |
| `props` | dome | 1.5 | 3 | 0.5 | 0.7 | 1 | transparent |
| `tile` | auto | 2 | 3 | 1 | 1.15 | 1.15 | wrap |

- **Errors** (mean angular error, degrees; old = main before this change, default preset):

| set | old default | new default | new painted | new props | new tile |
|---|---|---|---|---|---|
| round shapes and variants (13: spheres, tori, capsules, hoops, 512-2048) | 14.5 | 4.1 | 4.1 | 14.9 | |
| torus / hoop 1024 | 19.4 / 26.1 | 5.3 / 4.2 | | 1.8 / 3.0 | |
| rounded boxes (2) | 24.8 | 25.2 | 25.2 | **1.4** | |
| cobble tiles (6, wrap) | 14.4 | 12.2 | | | **11.7** |

- **Metrics:** `crease_pct` on the silhouette's share (`Result::shape`, painted pixels from the map), `roughness_deg`
  on the final map; inner pixels deeper than min(8k, half the frame's largest depth), sigma 4k, k = max(1, 2T/128);
  measured when the shorter side is above 128 px. Real art, 13 sprites over 128 px: creases old default 19.0 %, new
  default 0.0 %, rounded 15.1 %; final-map roughness old 5.72, new default 3.00 (the relief).
- **Warning thresholds:** `creased` when crease_pct > 2 (good results <= 0.9 %, creased maps >= 3.6 %); `streaky` when
  roughness_deg > 13 and not creased (good results <= 12.1: the tile preset on brushed cobbles; unsmoothed strong
  relief, detail 8 with smoothing 0.1-0.3, measures 14-21). `soft_edges` is not raised above 128 px with version 2.
- **Outer band:** version 2, islands above 128 px only (the pixel-art invariant: the golden fixtures `alpha` and
  `gradient_alpha` have soft rims); the outside is what reaches the field's border.
- **Version 1 and profile auto:** a version 1 generation resolves `auto` to `rounded` (version 1's default), so a
  version 1 recipe, `params.version: 1` or `--settings-version 1` alone reproduce the old bytes.
- **Timing** (generation, 8 threads): default 512 px 0.03 s, 1024 px 0.12 s, 2048 px 0.42 s (transparent sphere);
  2048 wrap tile 0.29 s; every preset <= 0.50 s at 2048; an opaque 2048 square with a transparent margin 0.50 s. Gaussians
  above sigma 16 (8 for scaled items) run as a four-box cascade of the same variance.
- **Memory** at 4096 (peak RSS, CLI dry run): +107 MB over main on a 4096 painted potion, +91 MB on a 4096 sphere.
