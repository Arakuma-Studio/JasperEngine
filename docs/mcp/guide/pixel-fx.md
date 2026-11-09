# Pixel FX

Sprite effects, screen transitions, glitch and game feel for pixel-art games (`docs/pixel-fx.md`).

## Game feel

Everything here is runtime Lua: drive it with `eval_lua` in Play (`play {fixed_fps: 60, auto_pause: true}`)
and check it with `wait {frames}` or `wait {until: "HitStop.active == false"}`.

- `HitStop.play(seconds, {timeScale, exclude = {go...}, only = {go...}, shake = "pixel_hit" | {amplitude, seconds}})`
  freezes game time on unscaled time; keep it at or under 0.3 s (longer reads as a stall). `HitStop.stop()`,
  `HitStop.active`, `HitStop.remaining`. Overlaps keep the longer time and the lower scale.
- Hit Stop Exempt (`add_component hitstopexempt`, fields `enabled`, `includeChildren`) keeps a GameObject and its
  children running; its scripts read the unfrozen `Time.deltaTime`. Rigidbodies run with the physics world.
- Camera Shaker `pixelSnap` (whole art pixels, no roll) and `tickRate` (Hz) through `set_properties camerashaker.*`;
  `.shake` `type: "kick"`, `units: "pixels"`, `tick` through `write_file` / `get_asset_info`; built-ins
  `pixel_hit`, `pixel_heavy`, `pixel_rumble`; `CameraShake.kick(px, seconds)`.
- `poseFrameRate` (fps, 0 = smooth) on `animator`, `animationplayer`, `poseanimator`: held poses stepped from each
  key; root motion and events keep the true time.
