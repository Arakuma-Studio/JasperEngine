# Pixel FX

The effects that make a pixel-art game feel finished (docs/specs/S48-pixel-fx.md). Agents read
`jasper://docs/pixel-fx`.

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
