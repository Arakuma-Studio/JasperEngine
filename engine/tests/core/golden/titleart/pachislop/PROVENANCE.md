# Pachislop reference images

Copied unchanged from [Pachislop](https://github.com/SafEight/pachislop) by SafEight, used with the
author's permission (`NOTICE.md`), commit `1c32502dc9feaff1086f615b07f2ca978722d494`:

- `effect/*.png` from `tests/golden/refs/effect/` (each effect at its defaults on Pachislop's
  standard title, 確定 in Dela Gothic One with the classic style, rendered at 0.25 and trimmed);
- `generator/patternFill-*.png` from `tests/golden/refs/generator/` (the Pattern Fill presets alone
  on the 1920 x 1080 canvas at 0.25);
- `scene/*.png` from `tests/golden/refs/scene/` (the semantic scenes of `tests/harness/cases.ts`
  `scenes()`, rendered at 0.25 and trimmed): `clipping`, `adjustment-in-group`,
  `adjustment-outside-group`, `effects-only-gaussian-blur`, `part-effects-2d`,
  `nested-hidden-groups`, `mask-shape-feather`, `mask-group-radial`, `flip-2d`. Not copied:
  `mask-from-layer-luminance` (its matte is the `sunburst` generator, S83's), the 3D scenes,
  `split-image` and `mask-adjustment` (outside AC11's list).
- the other `generator/*.png` from `tests/golden/refs/generator/` (S83: every preset of the other
  fifteen generators, a following one with the standard title, at 0.25 and trimmed).
- `look/f-3d-*.png` from `tests/golden/refs/look/` (Pachislop's 40 3D looks on "BIG" in Dela Gothic
  One at 0.25, trimmed; S86 AC21, `engine/tests/core/titleart/three_d/Title3DParityGpuTest.cpp`,
  compared with AC21's metric: size within 2 px, mean at most 6/255, at most 4 % of pixels over
  24/255).

They were drawn by Chromium's WebGL on unknown hardware, so they are cross-implementation
references compared with S80 AC12's loose structural metric (4 x 4 block means, block threshold
24, at most 3 % of blocks over, colour histogram distance at most 6 %), never regenerated: our own
strict goldens are `../effects/`. Used by `engine/tests/core/titleart/effects/TitleArtEffectsGpuTest.cpp`
(`TitleArtParityGpu`), `engine/tests/core/titleart/render/TitleArtScenesParityGpuTest.cpp`
(`TitleArtScenesParityGpu`), for S83's seventeen ported effects (`deepGlow` .. `shatter`)
`TitleFxCatalogueGpuTest.cpp` (`TitleFxParityGpu`) and, for the generators,
`engine/tests/core/titleart/generators/TitleGenGpuTest.cpp` (`TitleGenParityGpu`). Never shipped
(`engine/tests/` stays out of installs and exports).
