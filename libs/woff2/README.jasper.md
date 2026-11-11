# WOFF2 in JasperEngine

[woff2](https://github.com/google/woff2) 1.0.2 (tag `v1.0.2`, commit `1bccf208bca9`), the
decoder only: `src/woff2_dec.cc`, `woff2_common.cc`, `table_tags.cc`, `variable_length.cc`,
`woff2_out.cc`, the headers they include (`buffer.h`, `port.h`, `round.h`, `store_bytes.h`,
`table_tags.h`, `variable_length.h`, `woff2_common.h`), `include/woff2/decode.h` and `output.h`,
and `LICENSE`, unmodified. The encoder, the command-line tools and the bundled Brotli submodule
are not vendored (the decoder links `libs/brotli/`).

The editor uses it to import `.woff2` web fonts as TrueType or OpenType files: Brotli, then the
glyf/loca and hmtx transforms reversed (`editor/titleart/fonts/WebFontDecode`,
docs/specs/S82-title-designer.md R59). Shipped games never link it.
