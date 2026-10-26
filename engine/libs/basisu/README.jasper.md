# Basis Universal in JasperEngine

- Upstream: https://github.com/BinomialLLC/basis_universal, tag `v2_50` (commit `9bebe16726b3`,
  2026-07-22), Apache-2.0 (`LICENSE`, `NOTICE`). The encoder's third-party parts keep their own
  licences (`LICENSES/`): `encoder/3rdparty/tinyexr.h` (BSD-3-Clause), `qoi.h` and `tinydds.h`
  (MIT), `android_astc_decomp.cpp` (Apache-2.0), `encoder/basisu_miniz.h` (MIT), `jpgd` (public
  domain).
- Used by (spec S14):
  - the transcoder (`basisu_transcoder`): Basis KTX2 textures of the web family, transcoded at
    load (`engine/core/texture/BasisTranscode.cpp`), and the editor's previews of them;
  - the encoder (`basisu_encoder`, editor only): ETC1S, UASTC LDR 4x4 and UASTC HDR 4x4 for the
    web family, UASTC transcodes to ETC2/EAC for `android_etc2`, and the `.exr` reader.

## Vendored (byte for byte from the tag)
`LICENSE`, `NOTICE`, `LICENSES/`, `README.md`, `transcoder/*`, `encoder/*` (with
`encoder/3rdparty/`) except `encoder/basisu_wasm_*`.

Not vendored: the command-line tool (`basisu_tool.cpp`, `basisu_text_image.*`), `zstd/` (theirs:
see below), `OpenCL/`, `python/`, `webgl/`, the examples, `test_files/`, `bin/`, `contrib/`, the
shader-deblocking samples, the build files.

## Ours
- `zstd/zstd.h`: a one-line forward to `engine/libs/zstd/lib/zstd.h`, so basisu's
  `#include "../zstd/zstd.h"` uses the engine's single Zstandard copy.
- `CMakeLists.txt`: replaces the upstream build. SSE and OpenCL off (`BASISU_SUPPORT_SSE=0`,
  `BASISU_SUPPORT_OPENCL=0`), `-ffp-contract=off`, hidden visibility. The transcoder is built
  without PVRTC1/2, ATC and FXT1 (`BASISD_SUPPORT_*=0`), which the engine never asks for; XUASTC
  stays on because v2.50's XBC7 decoder does not compile without it. UASTC bit fields are read
  byte by byte on x86 too (`BASISD_USE_UNALIGNED_WORD_READS=0`, as upstream does on ARM and wasm):
  the unaligned `uint16_t` loads are undefined behaviour under UBSan.
- No source file is changed.
