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
| `pachislop/scene/adjustment-in-group` | copied unchanged from Pachislop's `tests/golden/refs/scene/adjustment-in-group.png` (scene parity reference, AC12) |
| `pachislop/scene/adjustment-outside-group` | copied unchanged from Pachislop's `tests/golden/refs/scene/adjustment-outside-group.png` (scene parity reference, AC12) |
| `pachislop/scene/clipping` | copied unchanged from Pachislop's `tests/golden/refs/scene/clipping.png` (scene parity reference, AC12) |
| `pachislop/scene/effects-only-gaussian-blur` | copied unchanged from Pachislop's `tests/golden/refs/scene/effects-only-gaussian-blur.png` (scene parity reference, AC12) |
| `pachislop/scene/flip-2d` | copied unchanged from Pachislop's `tests/golden/refs/scene/flip-2d.png` (scene parity reference, AC12) |
| `pachislop/scene/mask-group-radial` | copied unchanged from Pachislop's `tests/golden/refs/scene/mask-group-radial.png` (scene parity reference, AC12) |
| `pachislop/scene/mask-shape-feather` | copied unchanged from Pachislop's `tests/golden/refs/scene/mask-shape-feather.png` (scene parity reference, AC12) |
| `pachislop/scene/nested-hidden-groups` | copied unchanged from Pachislop's `tests/golden/refs/scene/nested-hidden-groups.png` (scene parity reference, AC12) |
| `pachislop/scene/part-effects-2d` | copied unchanged from Pachislop's `tests/golden/refs/scene/part-effects-2d.png` (scene parity reference, AC12) |
| `text/vertical` | 縦書き with a tate-chu-yoko run in Dela Gothic One through the renderer's text and geometry (AC21) |
| `text/variable_400` | Source Sans 3 VF (CFF2) at wght 400 (AC21) |
| `text/variable_800` | Source Sans 3 VF (CFF2) at wght 800: heavier outlines and advances (AC21) |
| `text/standard` | The standard title (確定, Dela Gothic One, the classic style) on a 640 x 360 canvas (§7) |
| `text/rim_miter` | A star with a mitred rim (AC22) |
| `text/rim_round` | The same star with a round rim (AC22) |
| `effects/brush_stroke` | Ink Brush Stroke at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/brush_stroke-marker` | Ink Brush Stroke with its `marker` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/brush_stroke-sumi` | Ink Brush Stroke with its `sumi` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/chalk` | Chalk at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/chalk-pastel` | Chalk with its `pastel` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/chromatic_aberration` | Chromatic Aberration at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/chromatic_aberration-rgb_split` | Chromatic Aberration with its `rgb_split` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/contour_lines` | Contour Lines at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/contour_lines-outside_rings` | Contour Lines with its `outside_rings` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/distress` | Distress at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/distress-dry_brush` | Distress with its `dry_brush` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/drips` | Drips at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/drips-slime` | Drips with its `slime` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/extrude` | Extrude at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/extrude-block_letters` | Extrude with its `block_letters` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/extrude-long_shadow` | Extrude with its `long_shadow` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/floor_reflection` | Floor Reflection at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/floor_reflection-glossy_floor` | Floor Reflection with its `glossy_floor` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/frost` | Frost at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/frost-frozen_solid` | Frost with its `frozen_solid` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/glitch` | Glitch at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/glitch-digital` | Glitch with its `digital` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/glitch-vhs` | Glitch with its `vhs` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/grain` | Grain at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/grain-film` | Grain with its `film` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/halftone` | Halftone at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/halftone-ben_day` | Halftone with its `ben_day` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/halftone-comic_dots` | Halftone with its `comic_dots` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/holographic` | Holographic Foil at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/holographic-foil` | Holographic Foil with its `foil` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/holographic-rainbow_sticker` | Holographic Foil with its `rainbow_sticker` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/lens_blur` | Lens Blur at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/lens_blur-bokeh_highlights` | Lens Blur with its `bokeh_highlights` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/light_glow` | Light Glow at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/light_glow-jackpot_flash` | Light Glow with its `jackpot_flash` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/light_glow-soft_bloom` | Light Glow with its `soft_bloom` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/material` | Material at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/material-chrome` | Material with its `chrome` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/motion_blur` | Motion Blur at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/motion_blur-speed` | Motion Blur with its `speed` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/motion_trail` | Motion Trail at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/motion_trail-echo` | Motion Trail with its `echo` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/neon_tube` | Neon Tube at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/neon_tube-cyan_neon` | Neon Tube with its `cyan_neon` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/pattern_fill-brick` | Pattern Fill with S83's `brick` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-brushed_metal` | Pattern Fill with S83's `brushed_metal` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-camo` | Pattern Fill with S83's `camo` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-carbon_fiber` | Pattern Fill with S83's `carbon_fiber` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-circuit` | Pattern Fill with S83's `circuit` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-concrete` | Pattern Fill with S83's `concrete` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-diamond_plate` | Pattern Fill with S83's `diamond_plate` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-granite` | Pattern Fill with S83's `granite` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-leather` | Pattern Fill with S83's `leather` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-marble` | Pattern Fill with S83's `marble` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-rust` | Pattern Fill with S83's `rust` procedural kind (§6.6.28), reviewed by eye |
| `effects/pattern_fill-wood` | Pattern Fill with S83's `wood` procedural kind (§6.6.28), reviewed by eye |
| `effects/perspective_shadow` | Perspective Shadow at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/perspective_shadow-low_sun` | Perspective Shadow with its `low_sun` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/pixelate` | Pixelate at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/pixelate-led_board` | Pixelate with its `led_board` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/pixelate-retro_8bit` | Pixelate with its `retro_8bit` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/radial_blur` | Radial Blur at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/radial_blur-impact_zoom` | Radial Blur with its `impact_zoom` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/radial_blur-spin` | Radial Blur with its `spin` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/scanlines` | Scanlines at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/scanlines-crt` | Scanlines with its `crt` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/shatter` | Shatter at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/shatter-cracks` | Shatter with its `cracks` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/stitch` | Stitch at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/stitch-cross_stitch` | Stitch with its `cross_stitch` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/stitch-patch_border` | Stitch with its `patch_border` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/tilt_shift` | Tilt-Shift Blur at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/tilt_shift-miniature` | Tilt-Shift Blur with its `miniature` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/wave_warp` | Wave Warp at defaults (S83 §6.6), on the standard title, reviewed by eye |
| `effects/wave_warp-heat_haze` | Wave Warp with its `heat_haze` preset (S83 §6.8), on the standard title, reviewed by eye |
| `effects/wave_warp-ripple` | Wave Warp with its `ripple` preset (S83 §6.8), on the standard title, reviewed by eye |
| `pachislop/PROVENANC` | copied unchanged from Pachislop's `tests/golden/refs/effect/PROVENANC.png` (S83 parity reference, AC4) |
| `pachislop/effect/chromaticAberration` | copied unchanged from Pachislop's `tests/golden/refs/effect/chromaticAberration.png` (S83 parity reference, AC4) |
| `pachislop/effect/contourLines` | copied unchanged from Pachislop's `tests/golden/refs/effect/contourLines.png` (S83 parity reference, AC4) |
| `pachislop/effect/deepGlow` | copied unchanged from Pachislop's `tests/golden/refs/effect/deepGlow.png` (S83 parity reference, AC4) |
| `pachislop/effect/extrude2d` | copied unchanged from Pachislop's `tests/golden/refs/effect/extrude2d.png` (S83 parity reference, AC4) |
| `pachislop/effect/floorReflection` | copied unchanged from Pachislop's `tests/golden/refs/effect/floorReflection.png` (S83 parity reference, AC4) |
| `pachislop/effect/glitch` | copied unchanged from Pachislop's `tests/golden/refs/effect/glitch.png` (S83 parity reference, AC4) |
| `pachislop/effect/grain` | copied unchanged from Pachislop's `tests/golden/refs/effect/grain.png` (S83 parity reference, AC4) |
| `pachislop/effect/halftone` | copied unchanged from Pachislop's `tests/golden/refs/effect/halftone.png` (S83 parity reference, AC4) |
| `pachislop/effect/lensBlur` | copied unchanged from Pachislop's `tests/golden/refs/effect/lensBlur.png` (S83 parity reference, AC4) |
| `pachislop/effect/material` | copied unchanged from Pachislop's `tests/golden/refs/effect/material.png` (S83 parity reference, AC4) |
| `pachislop/effect/motionBlur` | copied unchanged from Pachislop's `tests/golden/refs/effect/motionBlur.png` (S83 parity reference, AC4) |
| `pachislop/effect/motionTrail` | copied unchanged from Pachislop's `tests/golden/refs/effect/motionTrail.png` (S83 parity reference, AC4) |
| `pachislop/effect/perspectiveShadow` | copied unchanged from Pachislop's `tests/golden/refs/effect/perspectiveShadow.png` (S83 parity reference, AC4) |
| `pachislop/effect/radialBlur` | copied unchanged from Pachislop's `tests/golden/refs/effect/radialBlur.png` (S83 parity reference, AC4) |
| `pachislop/effect/shatter` | copied unchanged from Pachislop's `tests/golden/refs/effect/shatter.png` (S83 parity reference, AC4) |
| `pachislop/effect/tiltShift` | copied unchanged from Pachislop's `tests/golden/refs/effect/tiltShift.png` (S83 parity reference, AC4) |
| `pachislop/effect/wave` | copied unchanged from Pachislop's `tests/golden/refs/effect/wave.png` (S83 parity reference, AC4) |

## Updated goldens

| Golden | Before (max / mean / % over 8) | After | Reason |
|---|---|---|---|
| `../titleart_compositor_part_effects_2d` | 3 / 0.001 / 0 (one pixel over the strict threshold 2) | 0 / 0 / 0 | The renderer tests' stand-in shadow and glow tint through the shared Adjust implementation (S80 Deviation 51) |
| `../titleart_compositor_adjustment_in_group` | ≤ 2 / < 0.05 / 0 (within the strict limits) | 0 / 0 / 0 | Adjustment layers run the Adjust effect's implementation (S80 Deviation 51) |
