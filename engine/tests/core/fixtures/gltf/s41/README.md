# S53 loader fixtures

Real files of the tools that write optimised glTF, for `GltfNormalizeTest`, `GltfExtensionsTest`,
`GltfLoadDiagnosticsTest`, `ObjDedupTest` and `ObjDedupGpuTest` (engine core tests) and
`GltfEditorDecodersTest` (editor tests); docs/specs/S53-model-import-cook.md, AC1-AC4.

## Sources (written by `make/make_sources.py` and `make/make_handmade.py`)

| File | What |
|---|---|
| `source.glb` | Root node with two children: "Ball" (a 16 x 10 UV sphere: position, normal, tangent, UVs over [0, 2] x [0, 1], a 16 x 16 PNG checker) scaled 0.8 at x -1.2, and "Box" (a cube with vertex colours) at x 1.2 with a rotation and translation animation |
| `instances_source.glb` | one cube mesh under six nodes, each with its own translation, rotation and scale |
| `msft_lod.glb` | a sphere at three resolutions: node 0 with `MSFT_lod` naming nodes 1 and 2 (not in the scene), `MSFT_screencoverage` 0.5, 0.2, 0.01 |
| `texture_transform.glb` | four quads whose materials use `KHR_texture_transform`: one transform on set 0; two on set 0 with set 1 free; two on set 0 with set 1 taken (an occlusion map); a transform whose `texCoord` overrides the texture's |
| `dedup_sphere.obj`, `.mtl` | a 12 x 8 UV sphere with shared `v/vt/vn` indices (576 face corners, 117 distinct) |

## Made from them

gltfpack 1.3 (the `gltfpack-ubuntu.zip` of meshoptimizer's v1.3 release) and gltf-transform 4.x
(`@gltf-transform/core`, `extensions`, `functions` with `draco3dgltf` and `meshoptimizer` from npm),
by `make/make.sh` (the commands are there):

| File | Tool and options | Extensions |
|---|---|---|
| `meshopt_c.glb` | `gltfpack -c` | EXT_meshopt_compression, KHR_mesh_quantization, KHR_texture_transform |
| `meshopt_cc.glb` | `gltfpack -cc` | the same, with the octahedral, quaternion and exponential filters |
| `meshopt_kn.glb` | `gltfpack -c -kn` | the same, named nodes kept |
| `meshopt_vpf.glb` | `gltfpack -c -vpf` | float positions |
| `meshopt_noq.glb` | `gltfpack -c -noq` | compression without quantization |
| `meshopt_khr.glb` | `gltfpack -cz -ce khr` | KHR_meshopt_compression (codec v1, level 3) |
| `meshopt_cf.glb`, `.fallback.bin` | `gltfpack -cf` | optional EXT_meshopt_compression with an uncompressed fallback file |
| `meshopt_c.gltf`, `.bin` | `gltfpack -c` to a .gltf | the uri-less fallback buffer in a .gltf |
| `quantized.glb` | `gltfpack` | KHR_mesh_quantization alone |
| `instancing.glb` | `gltfpack -c -mi` (of `instances_source.glb`) | EXT_mesh_gpu_instancing |
| `basisu.glb` | `gltfpack -tc` | KHR_texture_basisu (ETC1S) |
| `webp.glb` | `gltfpack -tw` | EXT_texture_webp |
| `draco.glb` | gltf-transform `draco({method: 'edgebreaker'})` | KHR_draco_mesh_compression |
| `draco_sequential.glb` | gltf-transform `draco({method: 'sequential', quantizePosition: 16})` | KHR_draco_mesh_compression |
| `gt_meshopt.glb` | gltf-transform `reorder`, `quantize`, `meshopt({level: 'medium'})` | EXT_meshopt_compression, KHR_mesh_quantization |

To make them again: `make/make.sh <gltfpack> <node_modules dir> <out dir>` outside the repository,
then copy the files here. Both tools give the same bytes on every run.
