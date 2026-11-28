# bc7enc_rdo in JasperEngine

- Upstream: https://github.com/richgel999/bc7enc_rdo, `master` at commit `b9438627eef7`
  (2026-07-30), MIT or Unlicense (`LICENSE`; the ISPC kernel `bc7e.ispc`, Apache-2.0, and LodePNG
  are not vendored).
- Used by: the editor's texture cook (`editor/texture/BcEncoder.cpp`, spec S14): BC7 (bc7enc),
  BC1/BC3/BC4/BC5 (rgbcx) and the rate-distortion pass (ert, the entropy-reduction transform).
  Editor only; games never link it.

## Vendored (byte for byte but for the `ert.h` patch under Ours)
`LICENSE`, `README.md`, `bc7enc.{cpp,h}`, `bc7decomp.{cpp,h}`, `rgbcx.{cpp,h}`,
`rgbcx_table4.h`, `rgbcx_table4_small.h`, `ert.{cpp,h}`, `utils.h`, `dds_defs.h` (the last two
only for `ert.cpp`'s includes).

Not vendored: the command-line tool (`test.cpp`, `rdo_bc_encoder.*`, `utils.cpp`), `bc7e.ispc` and
`ispc.exe`, `lodepng.*`, `miniz.h`, the MSVC project.

## Ours
- `CMakeLists.txt`: one static library of the four sources, scalar (no ISPC), hidden visibility,
  `-ffp-contract=off` so the compiler never fuses multiply-adds differently per target.
- `ert.h` includes `<cstdint>` (a `JasperEngine patch` comment marks it): it uses `uint8_t` and
  `uint32_t`, which GCC 13 and later no longer bring through `<vector>` or `<string>`, so the editor
  failed to build there (reported in #193). No other source file is changed.
