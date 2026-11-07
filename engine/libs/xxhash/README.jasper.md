# xxHash in JasperEngine

- Upstream: https://github.com/Cyan4973/xxHash, version 0.8.2, as shipped by Zstandard 1.5.7
  (`engine/libs/zstd/lib/common/xxhash.h`), dual-licensed BSD / GPLv2; JasperEngine uses it under
  the BSD licence (`LICENSE`, Zstandard's, which covers that copy).
- Used by: `engine/core/util/ContentHash.cpp` (XXH3 64 and 128 bits): the content keys of shared GPU
  buffers (`MeshBufferPool`), mesh LOD and smooth-normal keys (`MeshLodPool`), spec
  `docs/superpowers/specs/2026-11-04-sprite-batching-design.md`.

## Local change
- Zstandard's prelude (`XXH_NO_XXH3`, `XXH_NAMESPACE ZSTD_`) is removed, so XXH3 is available. The
  header is included once, with `XXH_INLINE_ALL`, by `ContentHash.cpp`: every symbol is static
  there and cannot clash with Zstandard's prefixed copy.
