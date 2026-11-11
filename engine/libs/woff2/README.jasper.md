# WOFF2 in JasperEngine

Upstream: https://github.com/google/woff2 at
`7b67e12355fe7da148db5c19d4261f0a498a9537`, MIT (`LICENSE`).

Only the decoder's five translation units, their private headers and
`include/woff2/{decode,output}.h` are vendored. The encoder, tools, file I/O and
tests are omitted. `CMakeLists.txt` replaces the upstream build and links the
vendored Brotli decoder. `FontFace` bounds decoded output to 64 MiB before
passing SFNT bytes to the existing font validation, rasterizer and shaper.

## Patches

`src/woff2_dec.cc`: reject intermediate transformed-table or reconstructed-header
buffers larger than the upstream output limit (`kDefaultMaxSize`, 128 MiB)
before allocating them. The output writer's size limit otherwise does not bound
these separate buffers (the header can be large for a collection).
No format transformations or reconstruction algorithms are changed.

`src/variable_length.{cc,h}`: remove the extra blank line at end of file, so the
vendored subset passes the repository's whitespace check.
