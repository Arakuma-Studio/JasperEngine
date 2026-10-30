# High-resolution sprite fixtures

The regression inputs of `SpriteNormalsHiresTest.cpp` (design: `docs/superpowers/specs/2026-10-30-sprite-normals-hires-design.md`),
a small subset of the evaluation set. All written by `make_hires_fixtures.py` (numpy, scipy, Pillow):

    python3 -I make_hires_fixtures.py <directory of the evaluation sprites> <jasperengine-editor before high-resolution support>

## Synthetic ground truth

Analytic height fields shaded Lambert from the upper left, 4x4 supersampled silhouettes, 256 px:
`sphere_256.png`, `torus_256.png`, `roundbox_256.png` (flat top, quarter-round rim; transparent around) and
`cobbles_256.png` (a seamless, opaque painted cobblestone tile: run with edges wrap). `<name>_true.png` holds the exact
normals (OpenGL, green up, encoded like the generator's output) with alpha 255 on the pixels whose centre is inside the
shape: the pixels the test measures the mean angular error on.

## Real sprites (CC0)

Downscaled (Lanczos on premultiplied colour) so that the longer side is 384 px, converted to RGBA; nothing else changed.

| file | from | source | licence | author |
|---|---|---|---|---|
| `crate.png` | crate_drawn, 512x512 | https://opengameart.org/content/2d-wooden-box | CC0 | Cpt_Flash |
| `potion.png` | healthpotion_painted, 1262x1262 | https://opengameart.org/content/health-potion-vial | CC0 | BorisMedvedev |
| `lavapotion.png` | lavapotion_glossy, 724x950 | https://opengameart.org/content/lava-potion | CC0 (credit requested, not required: "ColorOptimist") | Color Optimist |
| `barbarian.png` | barbarian_flat, 400x900 | https://commons.wikimedia.org/wiki/File:BarbarianIdle.png | CC0 | Kurt Kaiser |

## Version 1 goldens

`v1/sphere_256_n.png` and `v1/potion_n.png`: written by the editor before high-resolution support (main at
153208611, `generate-sprite-normals --preset default`), recipe version 1 included. Their recipe's settings, read as
a version 1 recipe is (version 1, profile rounded), must regenerate these bytes exactly.
