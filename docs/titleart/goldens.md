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
| `effects/outer_glow` | Outer Glow at defaults (softer, behind), on the standard title |
| `effects/inner_glow` | Inner Glow at defaults (softer, screen, from the edge), on the standard title |
| `effects/drop_shadow` | Drop Shadow at defaults (global light 120: falls down-right), on the standard title |
| `effects/inner_shadow` | Inner Shadow at defaults, on the standard title |
| `effects/outline` | Outline at defaults (1.5 px keyline, follows the layer), on the standard title |
| `effects/color_overlay` | Color Overlay at defaults (red, normal), on the standard title |
| `effects/gradient_overlay` | Gradient Overlay at defaults (gold bands, linear, top to bottom), on the standard title |
| `effects/adjust` | Adjust at defaults (Hue/Saturation +25 saturation), on the standard title |
| `effects/pattern_overlay` | Pattern Overlay at defaults (stripes, layer-aligned), on the standard title |
| `effects/gaussian_blur` | Gaussian Blur at defaults (radius 8, repeat edges), on the standard title |
| `effects/outer_glow-precise` | Precise technique: the exact edge distance (bounded jump flood), on the standard title |
| `effects/outer_glow-add-gradient` | Added as light with a gradient across the layer's box (the bounds reduction), on the standard title |
| `effects/inner_glow-precise-center` | Precise technique glowing out from the centre, on the standard title |
| `effects/inner_glow-distance` | Gradient from the edge (the distance field), added as light, on the standard title |
| `effects/drop_shadow-colored` | Own angle (global light off), longer distance, coloured, on the standard title |
| `effects/drop_shadow-hard` | Small blur with a high spread: a hard shadow, on the standard title |
| `effects/inner_shadow-choke` | Own angle, choke 0.4, on the standard title |
| `effects/inner_shadow-blue` | A large, opaque coloured inner shadow, on the standard title |
| `effects/outline-wide` | A 6 px white outline, on the standard title |
| `effects/outline-soft-stack` | A soft outline that follows the stack, on the standard title |
| `effects/color_overlay-multiply` | Multiply at 70 %, on the standard title |
| `effects/color_overlay-color` | The Color blend mode (non-separable), on the standard title |
| `effects/gradient_overlay-radial-mirror` | Radial style, mirrored wrap, on the standard title |
| `effects/gradient_overlay-sweep-light` | A sweep band added as light, on the standard title |
| `effects/pattern_overlay-zebra` | Zebra, turned 30 degrees, on the standard title |
| `effects/pattern_overlay-checker-canvas` | Checker with a secondary colour, canvas-aligned, multiply, on the standard title |
| `effects/gaussian_blur-large` | Radius 30: the downsampled pyramid, on the standard title |
| `effects/gaussian_blur-horizontal-edges` | Horizontal only, edges fading at the canvas (Repeat Edge Pixels off), on the standard title |
| `effects/adjust-levels` | Levels (input white 180, gamma 1.4): the tone lookup, on the standard title |
| `effects/adjust-gradient_map` | Gradient Map: the ramp lookup, on the standard title |
| `effects/fill` | Fill at defaults (solid #202020) |
| `effects/pattern_fill` | Pattern Fill at defaults (zebra) |
| `effects/fill-linear` | A linear gradient at 45 degrees |
| `effects/fill-radial` | A radial gradient, scaled and offset |
| `effects/pattern_fill-tilt` | Seigaiha on a floor tilted 50 degrees |
| `effects/pattern_fill-leopard` | Leopard turned, the background at 40 % |
| `pachislop/look/f-3d-amethyst` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-amethyst.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-aurora` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-aurora.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-black-chrome` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-black-chrome.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-box-plate` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-box-plate.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-candy` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-candy.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-carbon-orange` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-carbon-orange.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-chrome` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-chrome.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-chrome-blue` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-chrome-blue.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-copper-bronze` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-copper-bronze.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-diamond` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-diamond.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-emerald` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-emerald.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-fire` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-fire.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-galaxy` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-galaxy.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-gold` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-gold.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-gold-plate` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-gold-plate.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-holo-silver` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-holo-silver.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-ice-crystal` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-ice-crystal.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-inflate` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-inflate.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-jade` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-jade.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-kin-gin` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-kin-gin.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-lacquer-gold` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-lacquer-gold.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-lava` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-lava.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-matte-glow` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-matte-glow.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-neon-glass` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-neon-glass.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-obsidian` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-obsidian.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-pearl-pink` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-pearl-pink.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-popout` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-popout.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-rainbow` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-rainbow.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-rainbow-glass-gold` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-rainbow-glass-gold.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-red` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-red.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-rose-chocolate` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-rose-chocolate.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-sakura` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-sakura.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-sapphire` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-sapphire.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-shu-lacquer` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-shu-lacquer.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-silver` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-silver.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-sometsuke` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-sometsuke.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-titanium` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-titanium.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-toxic` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-toxic.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-two-tone` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-two-tone.png` (3D parity reference, S86 AC21) |
| `pachislop/look/f-3d-violet-neon` | copied unchanged from Pachislop's `tests/golden/refs/look/f-3d-violet-neon.png` (3D parity reference, S86 AC21) |
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
| `text/vertical` | 縦書き with a tate-chu-yoko run in Dela Gothic One through the renderer's text and geometry (AC21) |
| `text/variable_400` | Source Sans 3 VF (CFF2) at wght 400 (AC21) |
| `text/variable_800` | Source Sans 3 VF (CFF2) at wght 800: heavier outlines and advances (AC21) |
| `text/standard` | The standard title (確定, Dela Gothic One, the classic style) on a 640 x 360 canvas (§7) |
| `text/rim_miter` | A star with a mitred rim (AC22) |
| `text/rim_round` | The same star with a round rim (AC22) |

## Updated goldens

| Golden | Before (max / mean / % over 8) | After | Reason |
|---|---|---|---|
| `../titleart_compositor_part_effects_2d` | 3 / 0.001 / 0 (one pixel over the strict threshold 2) | 0 / 0 / 0 | The renderer tests' stand-in shadow and glow tint through the shared Adjust implementation (S80 Deviation 51) |
| `../titleart_compositor_adjustment_in_group` | ≤ 2 / < 0.05 / 0 (within the strict limits) | 0 / 0 / 0 | Adjustment layers run the Adjust effect's implementation (S80 Deviation 51) |
