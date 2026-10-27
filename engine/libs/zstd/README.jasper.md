# Zstandard in JasperEngine

- Upstream: https://github.com/facebook/zstd, tag `v1.5.7` (commit `f8745da6ff1a`), dual-licensed
  BSD-3-Clause / GPLv2; JasperEngine uses it under BSD-3-Clause (`LICENSE`).
- Used by: the KTX2 reader of cooked textures (`engine/core/texture/Ktx2.cpp`, decoder only, every
  game) and the editor's texture cook (`editor/texture/`, encoder), spec S14.

## Vendored (byte for byte from the tag)
`LICENSE`, `README.md`, `lib/zstd.h`, `lib/zstd_errors.h`, `lib/zdict.h`, `lib/common/*`,
`lib/compress/*`, `lib/decompress/*` without `huf_decompress_amd64.S`.

Not vendored: `lib/dictBuilder`, `lib/legacy`, `lib/deprecated`, `programs/`, `contrib/`, `tests/`,
`build/`, `doc/`, `examples/`, `zlibWrapper/`.

## Ours
- `CMakeLists.txt`: replaces the upstream build. Two static libraries, `zstd_decompress` (common +
  decompress) and `zstd` (everything), C, `ZSTD_DISABLE_ASM` (the x86-64 Huffman assembly is the
  only `.S` file; without it the decoder is portable C on every compiler), no legacy formats, no
  multithreading, `XXH_NAMESPACE`, hidden visibility (the API's visibility macros are emptied).
  The decoder is built small (S14 R21): `DYNAMIC_BMI2=0`, `HUF_FORCE_DECOMPRESS_X1`,
  `ZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT` (upstream's size options; every frame still decodes).
- No source file is changed.
