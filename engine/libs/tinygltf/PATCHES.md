# Local patches to tinygltf

Each changed place in `tiny_gltf.h` carries a `JasperEngine patch` comment, and each patch is
listed here.

## 1. Meshopt fallback buffers and the BIN chunk (S53 R1)

gltfpack and gltf-transform write `EXT_meshopt_compression` / `KHR_meshopt_compression` files
whose uncompressed data lives in a placeholder buffer: `{"byteLength": N, "extensions":
{"EXT_meshopt_compression": {"fallback": true}}}` with no `uri`. Upstream copied the GLB's BIN chunk
into every uri-less buffer, so such a file failed with a `byteLength` error that never named
meshopt, or parsed with the BIN chunk's first bytes as its geometry; in a .gltf it failed on the
missing uri.

- `IsMeshoptFallbackBuffer` (new): whether a buffer object is such a placeholder.
- `ParseBuffer`: a placeholder without a uri loads no data (its extensions and name still parse);
  `engine/core/subsystem/GltfNormalize.cpp` decodes the compressed views into it.
- `ParseBuffer`: only buffer 0 can take the BIN chunk (glTF 2.0, GLB-stored buffer); another
  uri-less buffer is an error naming it. `ParseBuffer` gets the buffer's index for this, from the
  `buffers` loop of `LoadFromString`.

Covered by `GltfNormalizeTest` (`engine/tests/core/subsystem/GltfNormalizeTest.cpp`: the gltfpack
fixtures, and `MalformedSizes`' fallback buffer that claims the BIN chunk).
