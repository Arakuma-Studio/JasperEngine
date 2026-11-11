# Brotli in JasperEngine

[Brotli](https://github.com/google/brotli) 1.2.1 (tag `v1.2.1`, commit `db24a98aed38`), the
decoder only: `c/common/*.{c,h}`, `c/dec/*.{c,h}`, `c/include/brotli/` without `encode.h`, and
`LICENSE`, unmodified. The encoder, the tools, the bindings and the dictionary binaries are not
vendored.

The editor uses it through the WOFF2 decoder (`libs/woff2/`) to import `.woff2` web fonts as
TrueType or OpenType files (`editor/titleart/fonts/WebFontDecode`,
docs/specs/S82-title-designer.md R59). Shipped games never link it.
