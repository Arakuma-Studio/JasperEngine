# bcdec in JasperEngine

- Upstream: https://github.com/iOrange/bcdec, `main` at commit `80859ed3b7af` (2026-09-02),
  MIT or Unlicense (`LICENSE`).
- Used by: the CPU decode of BC1-BC7 textures (`engine/core/texture/TextureDecode.cpp`): the
  desktop fallback when the GPU cannot sample a cooked BC format (macOS GL has no BPTC), the
  editor's texture previews and the Game view's platform simulation (spec S14).

## Vendored (byte for byte)
`bcdec.h`, `LICENSE`, `README.md`. Header only: `TextureDecode.cpp` defines
`BCDEC_IMPLEMENTATION` (and `BCDEC_BC4BC5_PRECISE`, so BC4/BC5 decode to the exact R8/RG8 values
the GPU produces).
