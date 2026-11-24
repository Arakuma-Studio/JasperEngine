# WOFF2 in JasperEngine

[woff2](https://github.com/google/woff2) 1.0.2 (tag `v1.0.2`, commit `1bccf208bca9`), the
decoder only: `src/woff2_dec.cc`, `woff2_common.cc`, `table_tags.cc`, `variable_length.cc`,
`woff2_out.cc`, the headers they include (`buffer.h`, `port.h`, `round.h`, `store_bytes.h`,
`table_tags.h`, `variable_length.h`, `woff2_common.h`), `include/woff2/decode.h` and `output.h`,
and `LICENSE`, unmodified but for the patch below. The encoder, the command-line tools and the
bundled Brotli submodule are not vendored (the decoder links `libs/brotli/`).

The editor uses it to import `.woff2` web fonts as TrueType or OpenType files: Brotli, then the
glyf/loca and hmtx transforms reversed (`editor/titleart/fonts/WebFontDecode`,
docs/specs/S82-title-designer.md R59). Shipped games never link it.

## Patches

Each changed place carries a `JasperEngine patch` comment.

1. Upstream commit `23a34adec39d` ("Fix undefined type-punning when loading/storing words", after
   1.0.2), backported whole: `ComputeULongSum` (`woff2_common.cc`) reads each word byte by byte and
   both `Store16` (`store_bytes.h`) write byte by byte, instead of casting the buffer to
   `uint32_t*` or `uint16_t*` on little- and big-endian hosts. A table's checksum read through a
   misaligned pointer was undefined behaviour, which UBSan stopped the editor tests on
   (`WebFontDecodeTest`, `FontCatalogTest` under `SANITIZE=address,undefined`). Drop it when updating
   to a release that contains that commit.
