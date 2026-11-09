# Pixel FX (jasper://docs/pixel-fx)

## Sprite effects

`library:sprite` features: `DITHER_FADE`, `DISSOLVE`, `RAMP_RECOLOR`, `DROP_SHADOW`, and `OUTLINE` with the keyword
`OUTLINE_STYLE_PIXEL4` or `OUTLINE_STYLE_PIXEL8` (no keyword: the smooth outline). Turn them on with
`update_material` / `set_properties` (`shaderFeatures`), set values with `shaderParams.<member>`; a `choice` value
takes its index or its name (`"outlineColorFrom": "Selective"`). `list_shaders name=sprite` lists every member.

- Order: palette swap, colour replace, hue shift, ramp recolour, grayscale, overlay, flash, dissolve, inline, outline,
  drop shadow, alpha mode, 2D lights, glow, dither fade.
- Animated amounts (`flashAmount`, `ditherFadeAmount`, `dissolveProgress`, `rampAmount`): 0 is neutral and free.
- Ramp slot `u_fxRampTexture`: an N x 1 strip or `<palette>.palette#ramp:<name>`; unset, a grey ramp.
- A pixel outline or a drop shadow grows the sprite's quad (bounds included) by its reach in texels.
- Presets (`apply_material_preset`): `hit_flash`, `pixel_outline`, `selout_outline`, `drop_shadow`,
  `teleport_fade`, `burn_away`, `petrify`, `frozen`, `poison`, `pixel_enemy`.
