# Pachislop reference images

Copied unchanged from [Pachislop](https://github.com/SafEight/pachislop) by SafEight, used with the
author's permission (`NOTICE.md`), commit `1c32502dc9feaff1086f615b07f2ca978722d494`:

- `effect/*.png` from `tests/golden/refs/effect/` (each effect at its defaults on Pachislop's
  standard title, 確定 in Dela Gothic One with the classic style, rendered at 0.25 and trimmed);
- `generator/patternFill-*.png` from `tests/golden/refs/generator/` (the Pattern Fill presets alone
  on the 1920 x 1080 canvas at 0.25).
- `look/f-3d-*.png` from `tests/golden/refs/look/` (Pachislop's 40 3D looks on "BIG" in Dela Gothic
  One at 0.25, trimmed; S86 AC21, `engine/tests/core/titleart/three_d/Title3DParityGpuTest.cpp`,
  compared with AC21's metric: size within 2 px, mean at most 6/255, at most 4 % of pixels over
  24/255).

They were drawn by Chromium's WebGL on unknown hardware, so they are cross-implementation
references compared with S80 AC12's loose structural metric (4 x 4 block means, block threshold
24, at most 3 % of blocks over, colour histogram distance at most 6 %), never regenerated: our own
strict goldens are `../effects/`. Used by `engine/tests/core/titleart/effects/TitleArtEffectsGpuTest.cpp`
(`TitleArtParityGpu`). Never shipped (`engine/tests/` stays out of installs and exports).
