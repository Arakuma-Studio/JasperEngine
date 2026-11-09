# Pixel FX

Effects that make a pixel-art game feel finished, kept on the art pixel grid and coloured from palette ramps.

## Screen transitions

A screen transition covers the screen in one ink colour and reveals it again. Use one to hide a scene change, a
teleport or a death.

- **Kinds:** Iris (circle, diamond or square, closing on a point or a GameObject), Blinds, Slash, Pixel Dissolve,
  CRT Off, Burn Away and Fade (a dithered fade, optionally through a ramp's colours).
- **Hard pixels:** every edge follows the art pixel grid. That is the Pixel Perfect Camera's base pixels, or Pixel
  Size screen pixels per art pixel. The rim band just inside the edge is the same width along its whole length and
  takes ramp steps, never blends.
- **Over everything:** the transition draws after every scene of the frame (the HUD's layer scenes and the loading
  scene too) and before the debug overlay. With Cover UI off it draws right after the main scene, under the layer
  scenes.
- **Free while idle:** with no transition running there is no pass, no draw and no upload. The program is made the
  first time one plays.

**Project Settings > Graphics > Screen Transitions** holds the defaults scripts and the Game view use: Default Kind
(Iris), Duration (0.6 s), Colour (#0b0a10), Ramp (empty: the built-in ember ramp), Rim (2 art pixels), Pixel Size (0:
the Pixel Perfect Camera's grid) and Cover UI (on). Every change is undoable, and exported games take them.

The Game view's toolbar has a **Transition** button. It plays a transition over the view, in edit mode or in Play,
with its own Duration, Colour and Ramp for the preview.

From Lua:

```lua
local handle = Transition.play("iris", 0.8, {shape = "diamond", onCovered = function() print("covered") end})
Coroutine.waitFor(handle)
Transition.loadScene("Level2", "burn_away", 1.0)
```

`play` covers in 45 % of the duration, holds for 10 % (or `hold` seconds) and reveals in 45 %. `cover` and `reveal`
run one phase each. A transition started while another runs takes over from the current coverage.
`Transition.loadScene` covers the screen, loads the scene and holds while it loads, then reveals once the new
scenes have drawn. A loading scene shown meanwhile is revealed, then covered again. Transitions run on unscaled
time.

The agent guide's page (`docs/mcp/guide/pixel-fx.md`) lists every option.

## Glitch

**Post-processing > Stylised > Glitch** corrupts the frame in bursts:

- rows torn sideways;
- red and blue split apart;
- every other row going dark.

All of it moves in whole source texels, which are art pixels with a Pixel Perfect Camera. Burst Rate sets how often
(0: all the time at the Intensity), Burst Length how long, and Frame Rate how often the corruption changes. The schedule
depends only on the time and the Seed. It is one uber-pass feature with two extra texture taps.

`PixelFX.glitch(seconds, intensity)` plays a burst from a script without changing the saved settings. While playing,
the Inspector's **Burst now** button does the same. Scripted bursts show only where Glitch is enabled; set its
Intensity to 0 to see nothing but them.
