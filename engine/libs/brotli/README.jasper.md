# Brotli in JasperEngine

Upstream: https://github.com/google/brotli, v1.2.1 at
`db24a98aed38a16de78cc05ca15cdd8dc2a0d92d`, MIT (`LICENSE`).

Only `c/common`, `c/dec` and `c/include` are vendored. The encoder, tools and tests are
omitted. `CMakeLists.txt` is the engine's static, hidden-visibility build; portable
decoding works on desktop, mobile and WebAssembly without runtime CPU detection.
The WOFF2 font decoder uses it. There are no changes to upstream sources.
