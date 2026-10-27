# astc-encoder in JasperEngine

- Upstream: https://github.com/ARM-software/astc-encoder, tag `5.7.0` (commit `baff485b0ff3`),
  Apache-2.0 (`LICENSE.txt`).
- Used by: the editor's texture cook (`editor/texture/AstcEncoder.cpp`, spec S14): ASTC LDR
  4x4-12x12 encoding for the mobile family and the CPU decode of ASTC previews. Editor only.

## Vendored (byte for byte from the tag)
`LICENSE.txt`, `README.md`, `Source/astcenc*.{cpp,h}` without the command-line tool
(`astcenccli_*`).

Not vendored: `Source/ThirdParty`, `Source/UnitTest`, `Docs/`, `Test/`, `Utils/`, the CMake files.

## Ours
- `CMakeLists.txt`: replaces upstream's `Source/cmake_core.cmake` for the library alone. The ISA is
  SSE4.1 + POPCNT on x86-64 and NEON on arm64 (scalar elsewhere), always built with floating-point
  invariance (upstream's `ASTCENC_INVARIANCE=ON`: no `ASTCENC_NO_INVARIANCE`, `-ffp-contract=off`,
  `/fp:precise` on MSVC), which makes the output byte-identical across those ISAs.
- No source file is changed.
