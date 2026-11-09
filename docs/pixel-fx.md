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

## Game feel

### Hit-Stop

A hit-stop freezes the game for a moment on contact (a few frames; more than about 0.3 s reads as
a stall). From a script:

```lua
HitStop.play(0.08)                                  -- freeze 80 ms
HitStop.play(0.08, {shake = "pixel_hit"})           -- and shake 2 px through the freeze
HitStop.play(0.12, {timeScale = 0.25})              -- slow to a quarter instead
HitStop.play(0.12, {only = {self.gameObject, enemy}}) -- only the fighters freeze
HitStop.play(0.1, {exclude = {player}})              -- everyone but the player
```

- It starts with the next frame (the frame that asks for it runs on) and counts down on unscaled
  time, so it ends after its seconds even at `Time.timeScale` 0.
- It stops what game time drives: `Time.deltaTime` (0), the fixed steps (physics), game timers and
  `Coroutine.wait`, actions and tweens, animations (Animator, Animation Player, Pose Animator,
  Sprite Animation), scaled particles, Camera Follow and scaled shakes.
- It does not stop unscaled time, audio, the HTML UI clock, or anything with its Unscaled Time
  switch on.
- A shake given with `shake` runs on unscaled time, so it moves during the freeze.
- Calls that overlap keep the longer remaining time and the lower time scale.
- `HitStop.stop()` ends it; `HitStop.active`, `HitStop.remaining` and `HitStop.unfrozenDeltaTime`
  read it. While one runs, the Game view's statistics show `HIT STOP 0.06 s`.

**Hit Stop Exempt** (Add Component > Pixel Art) keeps its GameObject, and with Include Children
its descendants, running through a hit-stop: its animations, actions, tweens, particles, Camera
Shaker, Camera Follow, Pixel FX effects and its scripts (`Time.deltaTime` inside them is the
unfrozen delta). A tween counts as part of the GameObject it moves. `exclude` does the same for one
hit; `only` inverts the hit so only the listed GameObjects freeze and the world runs.

Rigidbodies cannot be exempted or frozen one by one: they run with the physics world, which a
hit-stop freezes (an `only` hit leaves it running). A character moved by a script or a Character
Controller follows its script's delta.

### Pixel screen shake

The Camera Shaker (docs/camera.md) has two pixel-art fields:

- **Pixel Snap** rounds the shake to whole art pixels (the Pixel Perfect Camera's grid, else the
  camera's screen pixels) and drops the roll. Inert on a perspective camera.
- **Tick Rate** (Hz, 0 = every frame) moves the shake only on ticks of its clock: 30 on a 60 fps
  game changes it at most every other frame.

Shake Presets gain the type **Kick** (pixelfx's hit shake: a whole-pixel jolt decaying over Fade
Out seconds), **Units** (View, or Pixels: strength in art pixels) and **Tick**. The built-ins
`pixel_hit` (2 px, 0.15 s), `pixel_heavy` (4 px, 0.35 s) and `pixel_rumble` (a held 1 px rumble at
30 Hz) use them. `CameraShake.kick(amplitudePx, seconds)` plays an unnamed kick. An older `.shake`
file loads and saves unchanged.

### Pose Frame Rate

Animator, Animation Player and Pose Animator have **Pose Frame Rate** (Smooth, 8, 12, 24 or a
custom rate). Above 0 every track holds its value on steps of 1 / rate counted from its previous
key, so every key's pose shows at its time and the animation reads as held drawings ("on twos").

- Root motion and the root bone's translation stay smooth, so the character still glides over the
  ground.
- Events fire at their true times.
- Blends between states step on the same ticks.
- Sprite Animation is already frame by frame and ignores it.
