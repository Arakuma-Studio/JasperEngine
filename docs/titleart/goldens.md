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
| `title-library/anime_candy-pop-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/anime_impact-burst` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/cartoon_bubble-pop` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/cartoon_jelly-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/casino_orb-candy-gold-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/casino_outline-gold` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/elements_fire-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/elements_ice` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/fantasy_engraved-gold` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/fantasy_gilded-shield-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/gems_diamond-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/gems_red-glass` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/horror_blood-drip` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/horror_bloodstone-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/metal_chrome-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/metal_classic-gold` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/minimal_duotone` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/minimal_matte-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/neon_sign-pink` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/neon_tube-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/retro_chrome-80s` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/retro_pixel-block-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/scheme_gold` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/scheme_neon-pink` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/scheme_rainbow` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/scheme_zebra` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/scifi_hud-cyan` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/scifi_visor-chrome-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/sports_trophy-gold-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/sports_varsity` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/template_casino_big-win-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/template_casino_rush-title-fx` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/template_fantasy_kingdom-title` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/template_minimal_clean-title` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/wa_ink-brush` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
| `title-library/wa_sakura-pearl-3d` | S84 AC28: the library item at 0.25 x on a transparent background (llvmpipe) |
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
| `effects/aura` | Aura at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/backlight` | Backlight at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/brush_swirl` | Brush Swirl at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/fire` | Fire at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/fractal_noise` | Fractal Noise at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/glints` | Glints at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/ink_splatter` | Ink Splatter at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/lens_flare` | Lens Flare at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/light_streaks` | Light Streaks at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/lightning` | Lightning at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/magic_circle` | Magic Circle at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/particles` | Particles at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/shockwave` | Shockwave at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/smoke` | Smoke at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/speed_lines` | Speed Lines at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `effects/sunburst` | Sunburst at defaults, alone on the fixture canvas (S83; `TitleArtEffectsGpu.Goldens` draws every registered generator) |
| `gen/aura_purple` | Aura's Purple Tendrils preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/aura_rising_weak` | Aura's Rising Weak Aura (弱) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/aura_strong` | Aura's Strong Aura (強) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/aura_tendrils` | Aura's Blue Tendrils preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/backlight_blue` | Backlight's Blue Rays preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/backlight_god_rays` | Backlight's God Rays preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/backlight_jackpot` | Backlight's Jackpot Rays (大当り) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/brush_swirl_gold` | Brush Swirl's Gold Brush preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/brush_swirl_ink` | Brush Swirl's Ink Swirl (筆渦) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/brush_swirl_red` | Brush Swirl's Red Brush preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/brush_swirl_slash` | Brush Swirl's Slash preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fire_behind` | Fire's Flames Behind preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fire_blaze` | Fire's Tall Blaze preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fire_hot` | Fire's Hot Fire (激アツ) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fire_over_edges` | Fire's Flames over the Edges preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fractal_noise_caustics` | Fractal Noise's Caustics preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fractal_noise_clouds` | Fractal Noise's Clouds preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fractal_noise_fog` | Fractal Noise's Fog preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fractal_noise_nebula` | Fractal Noise's Nebula preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fractal_noise_plasma` | Fractal Noise's Plasma preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/fractal_noise_static` | Fractal Noise's Static preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/glints_hero` | Glints's Hero Glint preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/glints_highlights` | Glints's Highlight Glints preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/glints_rainbow` | Glints's Rainbow Glints preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/glints_scatter` | Glints's Scattered Sparkles preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/glints_stars` | Glints's Star Glints preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/ink_splatter_ink_behind` | Ink Splatter's Ink Behind (墨) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/ink_splatter_ink_over` | Ink Splatter's Ink Spattered Over (墨) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/ink_splatter_red_ink` | Ink Splatter's Red Ink (赤) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/ink_splatter_swept` | Ink Splatter's Swept Streaks preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/lens_flare_blue` | Lens Flare's Blue Flare preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/lens_flare_gold` | Lens Flare's Gold Flare preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/lens_flare_streak` | Lens Flare's Long Streak preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/light_streaks_anamorphic` | Light Streaks's Anamorphic preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/light_streaks_bundle` | Light Streaks's Light Bundle preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/light_streaks_cyan` | Light Streaks's Cyan Streak preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/light_streaks_spears` | Light Streaks's Spears preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/lightning_bolts` | Lightning's Lightning Bolts preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/lightning_electric_blue` | Lightning's Electric Blue preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/lightning_electrify` | Lightning's Electrify (along Edge) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/magic_circle_bagua` | Magic Circle's Bagua (八卦) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/magic_circle_blue` | Magic Circle's Magic Circle Blue preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/magic_circle_gold` | Magic Circle's Magic Circle Gold (魔法陣) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/magic_circle_hologram` | Magic Circle's Hologram Base preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/magic_circle_hud` | Magic Circle's HUD Target preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/magic_circle_radar` | Magic Circle's Radar preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_bokeh` | Particles's Bokeh preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_confetti` | Particles's Confetti preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_embers` | Particles's Embers preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_fireworks` | Particles's Fireworks preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_gold_dust` | Particles's Gold Dust preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_light_motes` | Particles's Light Motes preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_rain` | Particles's Rain preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_sakura` | Particles's Sakura preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/particles_star_shower` | Particles's Star Shower preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/shockwave_ground` | Shockwave's Ground Shockwave preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/shockwave_halo` | Shockwave's Ring of Light preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/shockwave_neon` | Shockwave's Neon Rings preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/shockwave_red` | Shockwave's Red Shockwave preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/shockwave_white` | Shockwave's Shockwave White preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/smoke_black` | Smoke's Black Smoke preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/smoke_grey` | Smoke's Grey Smoke preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/smoke_steam` | Smoke's White Steam preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_beta` | Speed Lines's Beta Flash (ベタフラ) preset with its Halftone dissolve (S83 §6.8, `TitleGenGpu.PresetGoldens`; written after the effects stream's `halftone` was merged) |
| `gen/speed_lines_flash` | Speed Lines's Flash (フラッシュ) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_flow_diag` | Speed Lines's Flow Lines Diagonal preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_flow_h` | Speed Lines's Flow Lines Horizontal (流線) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_focus` | Speed Lines's Focus Lines (集中線) preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_focus_red` | Speed Lines's Focus Lines Red, Additive preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_rainbow` | Speed Lines's Rainbow Tunnel preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/speed_lines_spiky` | Speed Lines's Spiky Burst preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_black_gold` | Sunburst's Black & Gold preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_blue` | Sunburst's Blue Rays preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_focus_lines` | Sunburst's Focus Lines preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_purple` | Sunburst's Purple Rays preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_rainbow_halo` | Sunburst's Rainbow Halo preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_red_gold` | Sunburst's Red & Gold Rays preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `gen/sunburst_two_colour_wedges` | Sunburst's Two-Colour Wedges preset (S83 §6.8, `TitleGenGpu.PresetGoldens`) |
| `pachislop/generator/backlight-blue` | copied unchanged from Pachislop's `tests/golden/refs/generator/backlight-blue.png` (S83 parity reference, AC4) |
| `pachislop/generator/backlight-godRays` | copied unchanged from Pachislop's `tests/golden/refs/generator/backlight-godRays.png` (S83 parity reference, AC4) |
| `pachislop/generator/backlight-jackpot` | copied unchanged from Pachislop's `tests/golden/refs/generator/backlight-jackpot.png` (S83 parity reference, AC4) |
| `pachislop/generator/brushSwirl-gold` | copied unchanged from Pachislop's `tests/golden/refs/generator/brushSwirl-gold.png` (S83 parity reference, AC4) |
| `pachislop/generator/brushSwirl-ink` | copied unchanged from Pachislop's `tests/golden/refs/generator/brushSwirl-ink.png` (S83 parity reference, AC4) |
| `pachislop/generator/brushSwirl-red` | copied unchanged from Pachislop's `tests/golden/refs/generator/brushSwirl-red.png` (S83 parity reference, AC4) |
| `pachislop/generator/brushSwirl-slash` | copied unchanged from Pachislop's `tests/golden/refs/generator/brushSwirl-slash.png` (S83 parity reference, AC4) |
| `pachislop/generator/corona-auraStrong` | copied unchanged from Pachislop's `tests/golden/refs/generator/corona-auraStrong.png` (S83 parity reference, AC4) |
| `pachislop/generator/corona-auraWeakUp` | copied unchanged from Pachislop's `tests/golden/refs/generator/corona-auraWeakUp.png` (S83 parity reference, AC4) |
| `pachislop/generator/corona-purple` | copied unchanged from Pachislop's `tests/golden/refs/generator/corona-purple.png` (S83 parity reference, AC4) |
| `pachislop/generator/corona-tendrils` | copied unchanged from Pachislop's `tests/golden/refs/generator/corona-tendrils.png` (S83 parity reference, AC4) |
| `pachislop/generator/fire-behind` | copied unchanged from Pachislop's `tests/golden/refs/generator/fire-behind.png` (S83 parity reference, AC4) |
| `pachislop/generator/fire-blaze` | copied unchanged from Pachislop's `tests/golden/refs/generator/fire-blaze.png` (S83 parity reference, AC4) |
| `pachislop/generator/fire-overEdges` | copied unchanged from Pachislop's `tests/golden/refs/generator/fire-overEdges.png` (S83 parity reference, AC4) |
| `pachislop/generator/fire-red` | copied unchanged from Pachislop's `tests/golden/refs/generator/fire-red.png` (S83 parity reference, AC4) |
| `pachislop/generator/glints-hero` | copied unchanged from Pachislop's `tests/golden/refs/generator/glints-hero.png` (S83 parity reference, AC4) |
| `pachislop/generator/glints-highlights` | copied unchanged from Pachislop's `tests/golden/refs/generator/glints-highlights.png` (S83 parity reference, AC4) |
| `pachislop/generator/glints-rainbow` | copied unchanged from Pachislop's `tests/golden/refs/generator/glints-rainbow.png` (S83 parity reference, AC4) |
| `pachislop/generator/glints-scatter` | copied unchanged from Pachislop's `tests/golden/refs/generator/glints-scatter.png` (S83 parity reference, AC4) |
| `pachislop/generator/glints-stars` | copied unchanged from Pachislop's `tests/golden/refs/generator/glints-stars.png` (S83 parity reference, AC4) |
| `pachislop/generator/inkSplatter-black` | copied unchanged from Pachislop's `tests/golden/refs/generator/inkSplatter-black.png` (S83 parity reference, AC4) |
| `pachislop/generator/inkSplatter-over` | copied unchanged from Pachislop's `tests/golden/refs/generator/inkSplatter-over.png` (S83 parity reference, AC4) |
| `pachislop/generator/inkSplatter-red` | copied unchanged from Pachislop's `tests/golden/refs/generator/inkSplatter-red.png` (S83 parity reference, AC4) |
| `pachislop/generator/inkSplatter-swept` | copied unchanged from Pachislop's `tests/golden/refs/generator/inkSplatter-swept.png` (S83 parity reference, AC4) |
| `pachislop/generator/lensFlare-blue` | copied unchanged from Pachislop's `tests/golden/refs/generator/lensFlare-blue.png` (S83 parity reference, AC4) |
| `pachislop/generator/lensFlare-gold` | copied unchanged from Pachislop's `tests/golden/refs/generator/lensFlare-gold.png` (S83 parity reference, AC4) |
| `pachislop/generator/lensFlare-streak` | copied unchanged from Pachislop's `tests/golden/refs/generator/lensFlare-streak.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightStreaks-anamorphic` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightStreaks-anamorphic.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightStreaks-bundle` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightStreaks-bundle.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightStreaks-cyan` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightStreaks-cyan.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightStreaks-spears` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightStreaks-spears.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightning-bolts` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightning-bolts.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightning-electricBlue` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightning-electricBlue.png` (S83 parity reference, AC4) |
| `pachislop/generator/lightning-electrify` | copied unchanged from Pachislop's `tests/golden/refs/generator/lightning-electrify.png` (S83 parity reference, AC4) |
| `pachislop/generator/magicCircle-bagua` | copied unchanged from Pachislop's `tests/golden/refs/generator/magicCircle-bagua.png` (S83 parity reference, AC4) |
| `pachislop/generator/magicCircle-blue` | copied unchanged from Pachislop's `tests/golden/refs/generator/magicCircle-blue.png` (S83 parity reference, AC4) |
| `pachislop/generator/magicCircle-gold` | copied unchanged from Pachislop's `tests/golden/refs/generator/magicCircle-gold.png` (S83 parity reference, AC4) |
| `pachislop/generator/magicCircle-hologram` | copied unchanged from Pachislop's `tests/golden/refs/generator/magicCircle-hologram.png` (S83 parity reference, AC4) |
| `pachislop/generator/magicCircle-hud` | copied unchanged from Pachislop's `tests/golden/refs/generator/magicCircle-hud.png` (S83 parity reference, AC4) |
| `pachislop/generator/magicCircle-radar` | copied unchanged from Pachislop's `tests/golden/refs/generator/magicCircle-radar.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-bokeh` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-bokeh.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-confetti` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-confetti.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-embers` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-embers.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-fireworks` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-fireworks.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-goldDust` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-goldDust.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-lightMotes` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-lightMotes.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-rain` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-rain.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-sakura` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-sakura.png` (S83 parity reference, AC4) |
| `pachislop/generator/particles-starShower` | copied unchanged from Pachislop's `tests/golden/refs/generator/particles-starShower.png` (S83 parity reference, AC4) |
| `pachislop/generator/shockwave-ground` | copied unchanged from Pachislop's `tests/golden/refs/generator/shockwave-ground.png` (S83 parity reference, AC4) |
| `pachislop/generator/shockwave-halo` | copied unchanged from Pachislop's `tests/golden/refs/generator/shockwave-halo.png` (S83 parity reference, AC4) |
| `pachislop/generator/shockwave-neon` | copied unchanged from Pachislop's `tests/golden/refs/generator/shockwave-neon.png` (S83 parity reference, AC4) |
| `pachislop/generator/shockwave-red` | copied unchanged from Pachislop's `tests/golden/refs/generator/shockwave-red.png` (S83 parity reference, AC4) |
| `pachislop/generator/shockwave-white` | copied unchanged from Pachislop's `tests/golden/refs/generator/shockwave-white.png` (S83 parity reference, AC4) |
| `pachislop/generator/smoke-black` | copied unchanged from Pachislop's `tests/golden/refs/generator/smoke-black.png` (S83 parity reference, AC4) |
| `pachislop/generator/smoke-grey` | copied unchanged from Pachislop's `tests/golden/refs/generator/smoke-grey.png` (S83 parity reference, AC4) |
| `pachislop/generator/smoke-steam` | copied unchanged from Pachislop's `tests/golden/refs/generator/smoke-steam.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-beta` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-beta.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-flash` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-flash.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-flowDiag` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-flowDiag.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-flowH` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-flowH.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-focus` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-focus.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-focusRed` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-focusRed.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-rainbow` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-rainbow.png` (S83 parity reference, AC4) |
| `pachislop/generator/speedLines-spiky` | copied unchanged from Pachislop's `tests/golden/refs/generator/speedLines-spiky.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-blackGold` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-blackGold.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-blue` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-blue.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-focusLines` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-focusLines.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-purple` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-purple.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-rainbowHalo` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-rainbowHalo.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-redGold` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-redGold.png` (S83 parity reference, AC4) |
| `pachislop/generator/sunburst-twoColourWedges` | copied unchanged from Pachislop's `tests/golden/refs/generator/sunburst-twoColourWedges.png` (S83 parity reference, AC4) |

## Updated goldens

| Golden | Before (max / mean / % over 8) | After | Reason |
|---|---|---|---|
| `../titleart_compositor_part_effects_2d` | 3 / 0.001 / 0 (one pixel over the strict threshold 2) | 0 / 0 / 0 | The renderer tests' stand-in shadow and glow tint through the shared Adjust implementation (S80 Deviation 51) |
| `../titleart_compositor_adjustment_in_group` | ≤ 2 / < 0.05 / 0 (within the strict limits) | 0 / 0 / 0 | Adjustment layers run the Adjust effect's implementation (S80 Deviation 51) |
