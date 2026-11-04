# Loading glTF and OBJ models

What the engine reads from a model file, what it converts on the way, and what it tells you when a
file has something it cannot read. The design and acceptance record is
`docs/specs/S53-model-import-cook.md` (R1-R7); the Model Importer and the cook that ship smaller
files are in the same spec.

## glTF extensions

Every model goes through one normalise step between the parse and its use
(`engine/core/subsystem/GltfNormalize.h`), in the editor, in Play and in exported games, and for
every reader of a model: scenes, sub-clips, particle mesh shapes and the Vertex Animation import.
A file that uses none of these extensions costs a walk over its JSON objects.

| Extension | What the loader does | Games |
|---|---|---|
| `EXT_meshopt_compression`, `KHR_meshopt_compression` | decodes every compressed bufferView (all modes and filters) into its fallback buffer, after checking its stride, count, sizes and offsets | yes |
| `KHR_mesh_quantization` | widens integer positions (and what the vertex layout cannot bind) to floats; keeps normalized normals, tangents, UVs and colours on the GPU (BYTE4N, SHORT4N, SHORT2N, USHORT2N, UBYTE4N); the node's dequantization transform applies as any node transform | yes |
| `KHR_texture_transform` | bakes the transform into the UVs of each texture set (see below) | yes |
| `KHR_texture_basisu` | the KTX2 image is the texture, transcoded by the Basis transcoder (the editor and web players have it) | web; native games load the cooked texture |
| `EXT_mesh_gpu_instancing` | the node's copies draw as the instances of its mesh (an Instanced Mesh, TRANSLATION, ROTATION and SCALE) | yes |
| `MSFT_lod` | loads the highest level only; the lower levels are kept for the cook, which folds them into the mesh's LOD levels | yes |
| `KHR_draco_mesh_compression` | the editor decodes it | no: the cook transcodes it |
| `EXT_texture_webp` (also `KHR_texture_webp`) | the editor decodes the image | no: the cook transcodes it |
| `JE_mesh_lods` | the cook's baked LOD levels: passed through to the LOD system | yes |
| `KHR_materials_*`, `KHR_xmp` | as before (S11) | yes |

Normalized animation outputs (rotation keys as 16-bit integers) are converted to floats.

### Texture transforms

The engine draws two UV sets and has no per-texture UV transform, so the transform is baked into
the UVs (a copy per primitive and transform; the source data stays as it was):

- One transform on a UV set (the usual case, also gltfpack's quantized UVs): that set's UVs move.
- Two on one set and the second set free: the second transform goes to a new second UV set, and
  its textures read it.
- More than that: the first texture's transform is used for all the textures of that set, with a
  warning that names the material.

### Draco and WebP

Players never carry the Draco or WebP decoders (editor-only, `editor/libs/draco`,
`editor/libs/libwebp`). The editor decodes those files so they load and play; the model cook
transcodes them for every export, so a build never contains them. In a game that somehow gets
such a file the load fails with "requires KHR_draco_mesh_compression, which games do not decode
(the cook transcodes it: reimport the model in the editor and export again)".

## OBJ

An OBJ face list gives one vertex per face corner. The loader welds the corners that are equal in
every attribute (position, UV, normal, colour) and orders each submesh's triangles for the vertex
cache and overdraw, and the vertices by first use, as a cooked mesh is; a blended submesh keeps its
triangle order. What draws does not change (`ObjDedupTest`, `ObjDedupGpuTest`).

## When a file has something the loader cannot read

Each finding is one Console line, once per model, as `<message> (<fix>)`:

| Severity | Example | The model |
|---|---|---|
| Error | `glTF hero.glb: bufferView 4 (EXT_meshopt_compression) has a compressed stream that does not decode (truncated or corrupt) (the file is damaged or was written wrong: export it again)` | does not load |
| Error | `glTF hero.glb requires KHR_draco_mesh_compression, which games do not decode (the cook transcodes it: ...)` | does not load in a game |
| Error | `glTF hero.glb requires EXT_foo, which the engine does not read; it is loaded without it (export the model again without EXT_foo)` | loads, possibly wrong; export fails |
| Warning | `glTF hero.glb uses EXT_foo, which the engine does not read; it is ignored (nothing to do, or export the model again without EXT_foo)` | loads |

A corrupt or truncated stream is always an error naming the file and the bufferView, never a
crash: strides, counts and offsets are checked before the decoder runs, decoded indices are
checked against the vertices, and a model decodes to at most 512 MB.

Tools read the same findings: `ModelLoadDiagnostics::get(path)` (`GltfNormalize.h`) gives each
model's last load as a list of `{severity, extension, part, message, fix, cookFixes}`, and
`revision()` changes when any list changes. The Inspector's failure rows, the Project window
badges and `inspect_models` show them (S53 R18, R22).

The lattice bake (`editor/lattice/LatticeBakeGltf.cpp`) rewrites the source file, so it still
refuses meshopt- and Draco-compressed files: export the model uncompressed to bake it.
