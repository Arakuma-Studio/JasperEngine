# Sprite normals golden fixtures

Inputs (`inputs/`) and the outputs the upstream tool made from them (`expected/`), for the parity gate of
`SpriteNormalsCoreTest.cpp` (`SpriteNormalsParity.*`) and the atlas checks of `SpriteNormalsMediaTest.cpp`.

- Upstream: [spritefusion-sprite-normals](https://github.com/Hugo-Dz/spritefusion-sprite-normals) 0.1.0, commit
  `d14b067` (MIT, `../../LICENSE-spritefusion-sprite-normals.txt`), built in release mode
  (`cargo build --release`, rustc 1.99.1) on x86-64 Linux (glibc).
- `inputs/{ball,highlight,donut,character,thin,flat,flat_outline,flat2,alpha,sheet,tiles}.png` and
  `inputs/{walk,spin}.gif` are the evaluation sprites of the 2026-10-27 analysis (32x32 sprites, a 128x32 sheet, a
  64x64 opaque tile, 8-frame 32x32 GIFs).
- The other inputs are written by `make_fixtures.py` (standard library only): the images of upstream's
  `tests/generation.rs` (`disk.png` = `disk(31)`, `gradient_alpha.png`, `ramp.png`, `sprite9.png` and
  `islands_sheet.png`, the 9x9 sprite beside a white block on a 42x34 sheet), the tiny shapes (`tiny_1x1.png`,
  `tiny_1x19.png`, `tiny_23x1.png`, grey 80; `tiny_diagonal.png`, 20x20 with alpha 255 where x == y), and
  `twoframes.png`, a 12x10 APNG (frame 1 orange (200, 100, 50) for 1/12 s, frame 2 a 3x2 blue (50, 100, 200) patch at
  (4, 3) for 1/20 s, blend over).

Regenerate everything (the evaluation sprites are copied when their directory is given):

    python3 -I make_fixtures.py <path to spritefusion-sprite-normals> [<directory of the evaluation sprites>]

which runs, from this directory (`SN` = the upstream binary; every run adds `--force`):

| Fixture | Command |
|---|---|
| ball, highlight, donut, character, thin, flat, flat_outline, flat2, alpha, tiles | `SN inputs/<name>.png -o expected/<name>_n.png` |
| sheet | `SN inputs/sheet.png -o expected/sheet_n.png --frame-size 32x32` (also `sheet_n.color.png`, `sheet_n.frames.json`) |
| walk, spin | `SN inputs/<name>.gif -o expected/<name>_n.png` (atlas `<name>_n.png`, `<name>_n.color.png`, `<name>_n.frames.json`) |
| twoframes | `SN inputs/twoframes.png -o expected/twoframes_n.png` (APNG: as the GIFs) |
| character_variant, ball_variant | `SN inputs/<sprite>.png -o expected/<name>_n.png --normal-strength 2.5 --detail-strength 4 --detail-scale 1 --shape-strength 2 --shape-radius 8 --shape-softness 0.5` |
| character_directx, ball_directx | `SN inputs/<sprite>.png -o expected/<name>_n.png --normal-format directx` |
| tiny_1x1, tiny_1x19, tiny_23x1, tiny_diagonal, disk, gradient_alpha, sprite9, islands_sheet | `SN inputs/<name>.png -o expected/<name>_n.png` |
| ramp | `SN inputs/ramp.png -o expected/ramp_n.png --shape-strength 0` |

The test runs `generate()` with the equivalent settings (upstream defaults = preset `detailed` with radius 4:
strength 1.5, detail 6, smoothing 0.6, volume 1.15, softness 1.15, alpha cutoff 0.03; profile rounded, edges
transparent) and requires alpha exactly equal, RGB within 1 and at least 99.5% of the pixels exactly equal. The port
matches every fixture bit for bit.
