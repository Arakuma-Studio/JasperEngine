# bc7enc_rdo in JasperEngine

- Upstream: https://github.com/richgel999/bc7enc_rdo, `master` at commit `b9438627eef7`
  (2026-07-30), MIT or Unlicense (`LICENSE`; the ISPC kernel `bc7e.ispc`, Apache-2.0, and LodePNG
  are not vendored).
- Used by: the editor's texture cook (`editor/texture/BcEncoder.cpp`, spec S14): BC7 (bc7enc),
  BC1/BC3/BC4/BC5 (rgbcx) and the rate-distortion pass (ert, the entropy-reduction transform).
  Editor only; games never link it.

## Vendored (byte for byte)
`LICENSE`, `README.md`, `bc7enc.{cpp,h}`, `bc7decomp.{cpp,h}`, `rgbcx.{cpp,h}`,
`rgbcx_table4.h`, `rgbcx_table4_small.h`, `ert.{cpp,h}`, `utils.h`, `dds_defs.h` (the last two
only for `ert.cpp`'s includes).

Not vendored: the command-line tool (`test.cpp`, `rdo_bc_encoder.*`, `utils.cpp`), `bc7e.ispc` and
`ispc.exe`, `lodepng.*`, `miniz.h`, the MSVC project.

## Ours
- `CMakeLists.txt`: one static library of the four sources, scalar (no ISPC), hidden visibility,
  `-ffp-contract=off` so the compiler never fuses multiply-adds differently per target.
- No source file is changed.
