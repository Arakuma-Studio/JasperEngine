# Pixel FX

Pixel-art effects that stay on the art pixel grid and use palette colours instead of blends.

## Screen transitions

One engine overlay covers the screen in an ink colour and reveals it again. It belongs to the engine, so it
survives `SceneManager` loads, and it covers the layer scenes (a HUD) and the loading scene. Edges are hard art
pixels (the Pixel Perfect Camera's grid, or `pixelSize` screen pixels per art pixel), and the rim band just inside
the edge is exactly `rim` art pixels wide, coloured from a palette ramp. While idle it costs nothing: no pass, no
draw, no upload.

| Kind | What it does | Its options |
|---|---|---|
| `iris` | a hole closes on the centre | `shape` circle, diamond or square; `center` (Vector2, normalized from the top-left) or `centerObject` (a GameObject) |
| `blinds` | bands close, each from its own edge | `count` (8), `vertical` |
| `slash` | a straight wipe | `angle` in degrees (30) |
| `pixel_dissolve` | blocks of 1, 2 and 4 cells wink in by clumps, each first drawn as a ramp outline | `cell` art pixels (16), `seed` |
| `crt_off` | the dark closes onto a 1-pixel line, the line shrinks to a dot, the dot fades; the reveal warms up | none |
| `burn_away` | a ragged front burns in from the edges and opens again from a hole near the middle | `seed` |
| `fade` | an 8x8 Bayer dither of the ink per art pixel | `throughRamp`: step through the ramp's colours first |

Common options (`TransitionOptions`): `color` (`"#rrggbb"` or an sRGB Vector4), `palette` + `ramp` or
`ramp = "<palette>.palette#ramp:<name>"` (the built-in ember ramp by default), `rim` (0..8, 2), `glow` (0..1, 0.4: light
just outside the edge), `pixelSize` (0: the grid), `seed`, `ease` (`linear`, `in`, `out`, `in_out`), `hold` and
`onCovered`. What a call leaves out comes from Project Settings > Graphics > Screen Transitions
(`set_project_settings {"values": {"graphics.screen_transitions.kind": "fade"}}`; keys `kind`, `duration`, `color`,
`ramp`, `rim`, `pixel_size`, `cover_ui`).

```lua
-- cover (45 %), hold (10 %), reveal (45 %); onCovered runs once at the hold
local handle = Transition.play("crt_off", 1.2, {color = "#0b0a10", onCovered = function() print("covered") end})
Coroutine.waitFor(handle)
-- one phase each: covered until reveal() or stop()
Transition.cover(TransitionKind.IRIS, 0.4, {shape = "diamond"})
Transition.reveal("iris", 0.4)
-- covers, loads the scene stack, holds while it loads, reveals once the new scenes drew a frame
Transition.loadScene("Level2", "iris", 0.8, {shape = "diamond", ramp = "Palettes/ui.palette#ramp:gold"})
```

- A new transition replaces the running one from its current coverage, so the screen never pops.
- `Transition.loadScene` reveals onto a loading scene shown meanwhile (`SceneManager.setLoadingScene`,
  `loadingDelay`), then covers again when the load completes, each at half the duration.
- `Transition.active`, `covered`, `coverage` (0..1), `phase` (`none`, `cover`, `hold`, `reveal`) and `kind` are
  readable. To wait in a test: `wait {"until": "h:isDone()"}` or `wait {"until": "Transition.active == false"}`.
- Cover UI off (`graphics.screen_transitions.cover_ui`) draws it right after the main scene, under the layer scenes.
- Transitions run on unscaled time: a hit-stop or the time scale never slows them.
- The Game view's toolbar has Transition (the half-filled circle): it plays one over the view, in edit mode or Play.

## Glitch

Post-processing's Glitch (`set_scene_settings {"post_processing": {"glitch_enabled": true}}`) corrupts the frame in
bursts. It shifts torn rows sideways, splits red and blue apart and darkens every other row, all in whole source
texels (art pixels with a Pixel Perfect Camera). The schedule depends only on `floor(time * glitch_fps)` and
`glitch_seed`, so `set_post_processing_debug {"time_seconds"}` gives one image.

| Field | Default | What it sets |
|---|---|---|
| `glitch_intensity` | 1 | how hard the scheduled bursts are (0: only scripted bursts) |
| `glitch_rgb_split` | 2 | source texels red and blue move apart (0..8) |
| `glitch_tears`, `glitch_tear_shift` | 3, 8 | rows torn per burst frame (0..8), how far they move (0..32 texels) |
| `glitch_interlace` | 0.3 | chance per burst frame that odd rows go dark |
| `glitch_burst_rate`, `glitch_burst_length` | 0.5, 0.25 | bursts per second (0: every frame), seconds each |
| `glitch_fps`, `glitch_seed` | 30, 0 | the corruption's frame rate, the schedule's seed |

`PixelFX.glitch(seconds, intensity)` adds a burst from a script (unscaled seconds, never saved). It shows only where
the scene's post-processing has Glitch enabled; turn it on in the scene (Intensity 0 for scripted bursts only), or a
script that sets `glitchEnabled` ships the variant.

```lua
PixelFX.glitch(0.3, 0.8)
```
