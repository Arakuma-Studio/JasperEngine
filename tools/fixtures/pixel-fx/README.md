# pixel-fx fixtures

Assets for `tools/mcp/tests/test_contact_sheet.py` (docs/specs/S48-pixel-fx.md AC23). The test
copies them into a new project (pixels per unit 16) and builds its scenes there. Each red fixture
has a fixed twin:

| Lint | Red | Fixed twin |
|---|---|---|
| `one_shot_end` | `fx/burst_leftover.particles` (lifetime 3 s, the capture ends at 1 s) | `fx/burst_clean.particles` |
| `one_shot_end` | `effects/dissolve_late.json` (a PixelFX dissolve of 1.25 s on the sprite, captured for 1 s: texels left) | `effects/dissolve_clean.json` (1 s: progress 1, nothing left) |
| `spill` | `fx/burst_spill.particles` (about 38 px from its emitter, margin 24) | `fx/burst_clean.particles` |
| `loop_seam` | `anim/slide_jump.anim` (slides 16 px and wraps back) | `anim/slide_pingpong.anim` |
| `flicker` | `anim/blink_one.anim` (gone for one frame of 8) | `anim/blink_two.anim` (gone for two) |
| `determinism` | `scripts/RandomStart.lua` (unseeded, in Play) | `scripts/SeededStart.lua` |

The clean fixture is a still 8x8 sprite captured as a loop.

The `effects/*.json` files hold a `capture_contact_sheet` target's `effect` and `effect_options` (a PixelFX
effect on the sprite; library:sprite's DISSOLVE at progress 1 always hides every texel, so the red case is a
dissolve that has not reached 1 when the capture ends).
