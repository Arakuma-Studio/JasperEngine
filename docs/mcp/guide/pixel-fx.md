# Pixel FX

Sprite effects, screen transitions, glitch, game feel and the contact sheets that check them
(docs/specs/S48-pixel-fx.md). `capture_contact_sheet` is in the `pixel_art` group
(`tool_groups {"enable": ["pixel_art"]}`; callable by name either way). Humans have the Game view's
Contact Sheet... button.

## Contact sheets and lints

`capture_contact_sheet` draws a target at N fixed times into one PNG: the frames in a grid at one integer
scale, a band of rows per backdrop, each frame labelled `#i t=0.25s`.

| Target | Is |
|---|---|
| `target: {entity, scene?}` | a GameObject and its children: animations, particles and material time play from t = 0 |
| `target: {entity, animation: "<.anim>"}` | the GameObject playing that clip (duration and loop default to the clip's) |
| `target: {particles: "<.particles>"}` | the effect alone at the origin (duration and loop default to the effect's) |

- `mode: "isolated"` (default): a copy of the target alone on transparent black, one framebuffer pixel per art
  pixel (1 / pixels per unit), framed on its bounds at t = 0 plus twice `margin` (24). Every random seed and the
  shader time are pinned, so the same arguments give the same PNG bytes. Scripts do not run here.
- `mode: "scene"`: the whole scene through its camera. Stopped, a copy of the scene. In Play, the game is paused and
  stepped at 1/60 s, its scenes drawn after each step; it stays paused afterwards (it resumes if it was running).
- Times: `frames` (1-64, 8) evenly from 0 to `duration` for a one-shot, over one period without its end for a
  `loop`; or `times: [s, ...]`.
- `backdrops`: `checker` (default), `dark`, `light`, `sky`, `ground` (a floor from the target's feet), `black`,
  `scene` (its background colour). Scene mode has one band, the view.
- `scale` 0 picks the largest whole scale (up to 16) whose sheet fits `max_width` (1024); `columns` 0 is up to 8.
- The result: the image, `frames [{index, t, bbox}]` (bbox in art pixels of the frame), `cell` (the frame's size),
  `scale`, `columns`, `backdrops`, `bounds`, and with `lint` the checks.
- A preview writes nothing; the same arguments plus `output: true` and `path` write the PNG (one undo step).

### The loop

Draft three to five variants, capture a sheet of each with `lint: true`, look at the sheet, fix the worst thing
the checks or your eyes find, capture again. A check that FAILs three rounds in a row stops the loop: report it.

### The checks

`lint: true` runs every check, a list runs the named ones. Each is a row of the verdict tools
(`preview_particle_effect`'s shape): `id`, `kind` `effect`, `status` PASS / WARN / FAIL / SKIP, `round`, `measured`,
`threshold`, `threshold_source`, `limited_by` (what it found, or why it was skipped), `at` (the frames' times),
`fix_hint`, `method`, plus `count` and `spots` `[[frame, x, y]]` (frames from 1, art pixels from the top left).
`check_status` sums them up, `verdict` is `clean`, `fixable` or `needs_person`, and `next` says how to go on.

| Check | Measures | WARN | FAIL |
|---|---|---|---|
| `spill` | pixels drawn outside the target's bounds plus `margin`, over every frame and seeds 1, 2, 3, 7 (isolated) | > 0 | |
| `one_shot_end` | pixels still drawn at t = duration of a one-shot | > 0 | |
| `loop_seam` | pixels changed from the last frame to the first, against max(2 x p90 of a step, 12) | over | over x 2 |
| `determinism` | pixels that differ between two fresh captures of 0.37 x duration (in Play: the scene loaded again) | | > 0 |
| `flicker` | the share of drawn pixels that change for exactly one frame and change back | > 6 % | |
| `off_palette` | opaque pixels not in the palette the target's material draws with | > 0 | > 1 % |
| `partial_alpha` | pixels with 0 < alpha < 255 | | > 0 |
| `frame_cost` | the median CPU time a frame takes with the target shown minus hidden (isolated, release builds) | > 1 ms | > 4 ms |
| `hit_stop_length` | the longest hit-stop requested during a Play capture | > 0.3 s | |
| `transition_edges` | the rim's width in art pixels along a transition's edge | spread > 0 | |

`verdict` is `needs_person` when `determinism` or `frame_cost` fails (code or budget decisions), `fixable` when
another check warns or fails, else `clean`.
