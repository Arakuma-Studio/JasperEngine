# Pixel FX: sprite effects, screen transitions, glitch, game feel, contact sheets

Effects that keep pixel art on its grid: every effect works on whole art pixels, takes palette colours from a ramp
instead of blending, costs nothing until it is used and runs on every backend with vertex and fragment shaders only.
Design and acceptance: `docs/specs/S48-pixel-fx.md`. Agents: `docs/mcp/guide/pixel-fx.md` (`jasper://docs/pixel-fx`).

## Sprite effects

The sprite effects are features of the `library:sprite` shader (Inspector › Material › Shader *Sprite*), so sprites,
tilemaps and polygons get them. Turn a feature on with its checkbox; its rows show below it. Beside each feature is a
40 × 40 thumbnail of the sprite's frame with only that feature applied at the material's values. Under each animated
amount (Flash Amount, Dither Fade, Dissolve Progress, Ramp Amount) a **Preview** slider and a ▶ button show the effect
in the Scene and Game views without saving: Stop, a save, Play or another selection puts the material's value back.
Every edit is one undo step.

| Feature | What it does | Main values |
|---|---|---|
| Dither Fade | Discards the sprite's pixels through an ordered Bayer pattern (S43's table): a hurt or teleporting sprite fades without partial alpha | Dither Fade (0 shown, 1 gone), Dither Matrix (Bayer 4x4, 8x8), Dither Space (Sprite texels: the pattern moves with the sprite; Screen pixels: on S41's base pixels, else the screen's) |
| Dissolve | Burns the sprite away along a front: an edge band in ramp steps from the hot end, a scorched band one step darker, then gone | Dissolve Progress (0 intact, 1 gone), Direction (texels, y up; (0, 0): noise only), Noise, Noise Cell, Jitter, Edge Band (whole texels), Scorch Band, Edge Glow (HDR, black off), Seed |
| Ramp Recolour | Maps each texel's brightness to a ramp step: petrify, frozen, poison | Ramp Amount (a dithered choice per texel, never a blend), Ramp Low and High (the steps for luma 0 and 1; negative counts from the end), Bevel (lit from the top left), Pulse (steps a second) |
| Drop Shadow | The sprite's shape, moved by whole texels, behind the sprite and its outline | Shadow Offset (texels, y up), Shadow Colour, Shadow Colour From (Colour, Ramp darkest) |
| Outline › Outline Style | Smooth (today's outline), or Pixel4 / Pixel8: a one or two texel outline on the 4 or 8 neighbours | Outline Width (rounded to 1 or 2), Outline Colour From (Colour, Ramp darkest, Selective: sel-out, the art's colour a ramp step darker), Ramp Steps, Inline Width and Colour |

The ramp is the **Ramp** texture slot: an N × 1 PNG strip (2 to 32 colours, darkest first, imported as a sprite:
Nearest, no mips, never compressed) or a palette ramp, `<palette>.palette#ramp:<name>` (S43). Unset, it is an 8-step
grey ramp.

The features run in a fixed order, so they combine predictably: palette swap, colour replace, hue shift, ramp
recolour, grayscale, overlay, flash, dissolve, inline, outline (behind), drop shadow (behind the outline), the
material's alpha mode, 2D lights, glow, dither fade (last, so the whole look fades together).

A pixel outline and a drop shadow draw outside the art: the sprite's quad grows by their reach in texels (the
outline width on every side, the shadow offset on its sides), so they are never cut at the frame edge, and the
bounds the renderer culls and picks with grow with it. The texels beyond the frame read transparent, so atlas
neighbours never bleed in. The smooth outline and glow keep today's quad.

**Cost.** A feature that is off adds nothing to the shader. A feature on at its neutral amount (Dither Fade 0,
Dissolve 0, Ramp Amount 0) skips its work on a uniform branch. Texture reads per pixel: Dither Fade none; Dissolve
one ramp read in its bands; Ramp Recolour one ramp read (two more with a bevel); a pixel outline 4 (Pixel4) or 8
(Pixel8) per texel of width, Selective a pass over the ramp on outline pixels only; Drop Shadow one. Every feature at
once stays within 8 fragment textures (base, normal map, replace mask, overlay, ramp, the palette's two).

**Presets** (Inspector › Material › Presets…, or `apply_material_preset`): Hit Flash, Pixel Outline, Sel-Out Outline,
Drop Shadow, Teleport Fade, Burn Away (fire ramp), Petrify, Frozen and Poison (the stone, ice and poison ramps with a
bevel), and Enemy (flash, dissolve and a pixel outline). A preset sets only its own fields; one naming a built-in
palette's ramp makes the project's copy of that palette (`Palettes/<id>.palette` in the assets) in the same undo step.

**Scripts** set the values as any material value: `mesh:setShaderUniform("dissolveProgress", 0.5)` and
`material:setShaderFeature("DISSOLVE", true)` on the material (`getMaterial`, then `setMaterial`) (a feature turned on at runtime builds its variant then: prefer the
material, or a preset, at authoring time).
