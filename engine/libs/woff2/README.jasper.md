# WOFF2 in JasperEngine

Upstream: https://github.com/google/woff2 at
`7b67e12355fe7da148db5c19d4261f0a498a9537`, MIT (`LICENSE`).

Only the decoder's five translation units, their private headers and
`include/woff2/{decode,output}.h` are vendored. The encoder, tools, file I/O and
tests are omitted. `CMakeLists.txt` replaces the upstream build and links the
vendored Brotli decoder. `FontFace` bounds decoded output to 64 MiB before
passing SFNT bytes to the existing font validation, rasterizer and shaper.

## Patches

`src/woff2_dec.cc`: reject an intermediate transformed-table buffer larger than
the upstream output limit (`kDefaultMaxSize`, 128 MiB) before allocating it.
The output writer's size limit otherwise does not bound this separate buffer.
No format transformations or reconstruction algorithms are changed.
