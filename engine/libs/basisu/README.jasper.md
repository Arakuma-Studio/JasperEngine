# Basis Universal in JasperEngine

- Upstream: https://github.com/BinomialLLC/basis_universal, tag `v2_50` (commit `9bebe16726b3`,
  2026-07-22), Apache-2.0 (`LICENSE`, `NOTICE`). The encoder's third-party parts keep their own
  licences (`LICENSES/`): `encoder/3rdparty/tinyexr.h` (BSD-3-Clause), `qoi.h` and `tinydds.h`
  (MIT), `android_astc_decomp.cpp` (Apache-2.0), `encoder/basisu_miniz.h` (MIT), `jpgd` (public
  domain).
- Used by (spec S14):
  - the transcoder (`basisu_transcoder`): Basis KTX2 textures of the web family, transcoded at
    load (`engine/core/texture/BasisTranscode.cpp`, through the container-independent
    `basisu_lowlevel_*_transcoder` classes: `ktx2_transcoder` is not linked), and the editor's
    previews of them;
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
  `BASISU_SUPPORT_OPENCL=0`), `-ffp-contract=off`, hidden visibility. UASTC bit fields are read
  byte by byte on x86 too (`BASISD_USE_UNALIGNED_WORD_READS=0`, as upstream does on ARM and wasm):
  the unaligned `uint16_t` loads are undefined behaviour under UBSan.
- The engine's transcoder (`basisu_transcoder`, the web runtime: spec S14 R21, deviation 23) is
  built with only the web transcode lists' paths: `BASISD_SUPPORT_XUASTC=0` (no XUASTC, ASTC LDR
  or XUBC7 sources, with their BC7/ASTC/ETC1 packers and decoders), `BASISD_SUPPORT_PVRTC1/PVRTC2/
  ATC/FXT1=0`, `BASISD_SUPPORT_ASTC=0` and `BASISD_SUPPORT_BC7_MODE5=0` (no ETC1S -> ASTC or BC7:
  TextureFallback's ETC1S lists leave them out) and `JASPER_BASISD_COMPUTE_DXT1_TABLES=1`. The
  encoder's copy (`basisu_encoder`) keeps every path.
- Source changes, each marked `JasperEngine` in the file, all compiled out unless a macro asks:
  - `transcoder/basisu_transcoder.cpp`: `JASPER_BASISD_COMPUTE_DXT1_TABLES` (default 0) declares
    the ETC1S -> BC1 tables `g_etc1_to_dxt_5/6` uninitialised and has `basisu_transcoder_init()`
    compute them (`transcoder/jasper_etc1s_to_bc1_tables.inl`, ours) instead of compiling in
    `basisu_transcoder_tables_dxt1_{5,6}.inc` (120 KB); the computed tables are upstream's entry
    for entry (`TextureBasisTranscode.Etc1sToBc1TablesMatchUpstream`), `jasper_etc1_to_dxt1_table()`
    (`transcoder/jasper_transcoder_tables.h`, ours) hands them to that test. The UASTC -> PVRTC1 calls
    of `basisu_lowlevel_uastc_ldr_4x4_transcoder::transcode_slice` are under `BASISD_SUPPORT_PVRTC1`
    like ETC1S's. `BASISD_SUPPORT_XUASTC` also guards the XBC7 decoder (`basisu_xbc7_decoder.h`
    and `.inl`), the arithmetic decoder's lookup table (`arith_fastbits_f32`) and the XUASTC
    transcoders' block helpers, which v2.50 leaves outside it (it does not compile without XUASTC
    otherwise: spec S14 deviation 12).
  - Speed of the UASTC -> BC7 / ASTC transcodes the web measures (spec S14 R19, deviation 24),
    the same bytes: the 16-bit word reads of UASTC bit fields are `memcpy` loads (no unaligned
    `uint16_t*` read, so the engine's copy takes `BASISD_USE_UNALIGNED_WORD_READS=1` everywhere,
    wasm and UBSan builds included; the encoder's copy keeps byte reads), and under
    `JASPER_BASISD_FAST_TRANSCODE` (default 0; 1 for the engine's copy, so the encoder's copy stays
    upstream's code and the editor's `TextureWebTranscode` tests compare the two byte for byte):
    `encode_bc7_block()` builds the block in two 64-bit words (`bc7_block_bit_writer`), BC7 modes
    5 and 6 (where UASTC's common modes land) written out, the other modes' indices 32 bits at a
    time; `pack_astc_block()` builds the ASTC block in two words (`astc_block_bits`) and
    `astc_set_bits()` ORs a field from one shifted word; `unpack_uastc()` divides the trit / quint
    bundles by the constants 3 and 5 and inverts a one-subset block's weights in one loop.
  - `encoder/basisu_astc_ldr_fencode.h` includes `<bit>` for its `std::popcount`, which the tag
    calls when `__cpp_lib_bitops` is defined without including the header (MSVC's library defines
    the macro in every header).
- Otherwise no source file is changed: the rest of `encoder/*` is byte for byte the tag's.
