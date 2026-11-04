# Title Art goldens

The golden ledger of the Title Art program (`docs/specs/S80-title-art-core.md` §7, S80-S87): every
title golden under `engine/tests/core/golden/titleart/` is added or updated only with a row here,
checked by `python3 tools/titleart/check_golden_ledger.py` (in `tools/mcp/tests/test_docs.py`). Our
goldens are written on glcore (llvmpipe, `JASPER_TEST_UPDATE_GOLDEN=1`) and Vulkan (lavapipe) must
match them; they are compared premultiplied (max 48/255, mean 0.25/255, at most 0.1 % of pixels over
8/255). The Pachislop references (`pachislop/**`) are never changed. An update row gives the default
comparison of the old golden against the new image (before) and after the update.

## New goldens

| Golden | Reason |
|---|---|
| `effects/outer_glow` | Outer Glow at defaults (softer, behind) |
| `effects/inner_glow` | Inner Glow at defaults (softer, screen, from the edge) |
| `effects/drop_shadow` | Drop Shadow at defaults (global light 120: falls down-right) |
| `effects/inner_shadow` | Inner Shadow at defaults |
| `effects/outline` | Outline at defaults (1.5 px keyline, follows the layer) |
| `effects/color_overlay` | Color Overlay at defaults (red, normal) |
| `effects/gradient_overlay` | Gradient Overlay at defaults (gold bands, linear, top to bottom) |
| `effects/adjust` | Adjust at defaults (Hue/Saturation +25 saturation) |
| `effects/pattern_overlay` | Pattern Overlay at defaults (stripes, layer-aligned) |
| `effects/gaussian_blur` | Gaussian Blur at defaults (radius 8, repeat edges) |
| `effects/outer_glow-precise` | Precise technique: the exact edge distance (bounded jump flood) |
| `effects/outer_glow-add-gradient` | Added as light with a gradient across the layer's box (the bounds reduction) |
| `effects/inner_glow-precise-center` | Precise technique glowing out from the centre |
| `effects/inner_glow-distance` | Gradient from the edge (the distance field), added as light |
| `effects/drop_shadow-colored` | Own angle (global light off), longer distance, coloured |
| `effects/drop_shadow-hard` | Small blur with a high spread: a hard shadow |
| `effects/inner_shadow-choke` | Own angle, choke 0.4 |
| `effects/inner_shadow-blue` | A large, opaque coloured inner shadow |
| `effects/outline-wide` | A 6 px white outline |
| `effects/outline-soft-stack` | A soft outline that follows the stack |
| `effects/color_overlay-multiply` | Multiply at 70 % |
| `effects/color_overlay-color` | The Color blend mode (non-separable) |
| `effects/gradient_overlay-radial-mirror` | Radial style, mirrored wrap |
| `effects/gradient_overlay-sweep-light` | A sweep band added as light |
| `effects/pattern_overlay-zebra` | Zebra, turned 30 degrees |
| `effects/pattern_overlay-checker-canvas` | Checker with a secondary colour, canvas-aligned, multiply |
| `effects/gaussian_blur-large` | Radius 30: the downsampled pyramid |
| `effects/gaussian_blur-horizontal-edges` | Horizontal only, edges fading at the canvas (Repeat Edge Pixels off) |
| `effects/adjust-levels` | Levels (input white 180, gamma 1.4): the tone lookup |
| `effects/adjust-gradient_map` | Gradient Map: the ramp lookup |
| `effects/fill` | Fill at defaults (solid #202020) |
| `effects/pattern_fill` | Pattern Fill at defaults (zebra) |
| `effects/fill-linear` | A linear gradient at 45 degrees |
| `effects/fill-radial` | A radial gradient, scaled and offset |
| `effects/pattern_fill-tilt` | Seigaiha on a floor tilted 50 degrees |
| `effects/pattern_fill-leopard` | Leopard turned, the background at 40 % |
| `pachislop/effect/adjust` | copied unchanged from Pachislop's `tests/golden/refs/effect/adjust.png` (parity reference, AC12) |
| `pachislop/effect/dropShadow` | copied unchanged from Pachislop's `tests/golden/refs/effect/dropShadow.png` (parity reference, AC12) |
| `pachislop/effect/gaussianBlur` | copied unchanged from Pachislop's `tests/golden/refs/effect/gaussianBlur.png` (parity reference, AC12) |
| `pachislop/effect/gradientOverlay` | copied unchanged from Pachislop's `tests/golden/refs/effect/gradientOverlay.png` (parity reference, AC12) |
| `pachislop/effect/innerGlow` | copied unchanged from Pachislop's `tests/golden/refs/effect/innerGlow.png` (parity reference, AC12) |
| `pachislop/effect/innerShadow` | copied unchanged from Pachislop's `tests/golden/refs/effect/innerShadow.png` (parity reference, AC12) |
| `pachislop/effect/keyline` | copied unchanged from Pachislop's `tests/golden/refs/effect/keyline.png` (parity reference, AC12) |
| `pachislop/effect/outerGlow` | copied unchanged from Pachislop's `tests/golden/refs/effect/outerGlow.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-asanoha` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-asanoha.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-checker` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-checker.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-giraffe` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-giraffe.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-hazard` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-hazard.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-leopard` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-leopard.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-seigaiha` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-seigaiha.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-shippo` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-shippo.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-tiger` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-tiger.png` (parity reference, AC12) |
| `pachislop/generator/patternFill-zebra` | copied unchanged from Pachislop's `tests/golden/refs/generator/patternFill-zebra.png` (parity reference, AC12) |

## Updated goldens

| Golden | Before (max / mean / % over 8) | After | Reason |
|---|---|---|---|
