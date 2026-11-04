# gltfpack and the meshopt glTF extensions for JasperEngine

Repo state read: `forge-meshopt` at `b4f7816ea` (main). Upstream clone: meshoptimizer 1.3
(`src/meshoptimizer.h:15`), gltfpack 1.3 (`gltf/gltfpack.h:2`). Vendored: meshoptimizer 1.2.

**Labels.** `[V path:line]` the source says this at that place. `[I]` inferred by following the code.
`[A]` assumed or general knowledge, not checked here. `[W]` read from the web (sources at the end).

**Nothing was built or run.** No editor, no test, no gltfpack. The only thing executed against the
clone is my own text-reading script, which counted the size of the wasm strings (section 4). So
every statement about what a user sees at runtime is `[I]` built from `[V]` code, and needs a red
test before anyone relies on it. No gltfpack output file was examined; the file layouts below come
from gltfpack's writer code.

---

## 0. Summary and correction of the brief

The "known fact" is half right. The engine cannot read these files, but **it does not refuse them,
and the test does not show what happens to them.**

- `UnsupportedWarns` passes because its fixture only *lists* `EXT_meshopt_compression` in
  `extensionsUsed` / `extensionsRequired`. Its data is plain, uncompressed, in a data URI, with no
  bufferView extension and no fallback buffer [V `engine/tests/core/fixtures/gltf/unsupported_required.gltf`].
  The test's comment "a required one is an error, and the model still loads"
  [V `engine/tests/core/subsystem/GltfMaterialExtensionsTest.cpp:255-259`] is true only of that fixture.
- `LatticeBakeGltf.cpp:31-33,285-290` is the *only* real refusal, and it covers only the lattice bake.
- A real gltfpack `-c` glb takes another path: **tinygltf fails first, with a message that never names
  meshopt, or the parse succeeds and the geometry is whatever bytes begin the BIN chunk** (section 1.2).
- Beyond compression, **plain `KHR_mesh_quantization` does not render** (positions), which means every
  default gltfpack output fails even without `-c`. Five independent loader gaps stand between
  a gltfpack file and a correct picture, so "add the decoder" alone does not fix it (section 1.11).

Recommendation (section 3): don't bundle gltfpack and don't link `gltf/*.cpp`. Add a decode and
normalise step in the engine's loader, then an "Optimize" import step (Unity's Model Importer names)
that calls the vendored library, keeps node numbering, and writes a cooked **glb** beside S14's cooked
textures. No new binary mesh format.

---

## 1. What happens today

### 1.1 The path a model takes

There is **no import step for glTF** [I]. A `.glb` in `assets/` is loaded directly when a
`ModelComponent.filename` points at it. FBX is the only model format that is imported, to a cached
`.glb` [V `editor/util/FbxImporter.h:16`, `engine/core/subsystem/MeshSystem.cpp:310-318`]. That FBX
path is the precedent for any conversion.

Parse sites, all without decoding:

| Site | Use |
|---|---|
| `MeshSystem.cpp:5140` | synchronous load |
| `MeshSystem.cpp:1627` | async worker |
| `MeshSystem.cpp:1295` (`parseGLTFFile`) | nodes and animations only; `ModelSubClips` goes through it [I] |
| `particles/ParticleMeshes.cpp:191` | particle mesh shapes |
| `editor/vat/import/VatSourceMesh.cpp:452` | VAT baking |

`LatticeBakeGltf` reads the JSON itself. All tinygltf reads go through `ParseBuffer`.

Order that matters: tinygltf parses all buffers first. **`reportGLTFExtensions` only runs after a
successful parse** [V `MeshSystem.cpp:5207`], so a failed parse never reaches the helpful message.

Export: `Exporter::copyAssets` ships models verbatim except FBX [V `editor/Exporter.cpp:2753,2825-2851`].
A search for `extensions` / `extensionsRequired` in `Exporter.cpp`, `LoadReport.cpp` and
`mcp/tools/WorldTools.cpp` finds nothing [V grep, no hits]. So **an export succeeds silently and the
failure appears in the player's log** [I]. `get_model_info` does not surface required extensions [I].

### 1.2 EXT_meshopt_compression and KHR_meshopt_compression, required

Both take the same path: neither name is in `supportedExtensions` [V `MeshSystemGltfPbr.cpp:31-37`].

What gltfpack writes:
- `buffers[0]` is the BIN chunk (compressed streams plus uncompressed images).
- `buffers[1]` is `{"byteLength":<uncompressed size>,"extensions":{"EXT_meshopt_compression":{"fallback":true}}}`
  with **no uri** [V `gltf/gltfpack.cpp:1018-1052`].
- Compressed bufferViews point at buffer 1, with the real location in their extension
  [V `gltf/write.cpp:739`].
- The extension is required unless `-cf` is used [V `gltf/gltfpack.cpp:896`].
- A `.glb` embeds images in BIN [V `gltf/gltfpack.cpp:1154`], and image views are written first
  [V `gltf/gltfpack.cpp`, images before meshes in `process`].

The cause: **tinygltf copies the BIN chunk into *every* buffer that has no uri**
[V `engine/libs/tinygltf/tiny_gltf.h:4595-4618`]. It knows neither buffer index nor `fallback`. The
Khronos text says a placeholder in a GLB should use index 1 or higher and must not map to BIN [W]. So:

| Case | Outcome |
|---|---|
| **A. glb, fallback bigger than BIN** (typical for geometry-heavy files, since compression shrinks them) | Parse fails. User sees: ``Can't load GLTF model (x.glb): Invalid `byteLength'. Must be equal or less than binary size: `byteLength' = N, binary size = M`` then `Model load failed, not retrying: x.glb` [V `tiny_gltf.h:4608`, `MeshSystem.cpp:1779` or `5153`, `:7128`]. **Meshopt is never mentioned.** The entity stays empty for the session (`failedModelLoads`). |
| **B. glb, fallback fits in BIN** (e.g. large embedded textures) | Parse *succeeds*. `buffers[1]` holds the first N bytes of BIN: image bytes and compressed streams [I]. User sees: `glTF x.glb requires EXT_meshopt_compression, which the engine does not read; it is loaded without it` [V `MeshSystemGltfPbr.cpp:140`]. Vertices and indices are garbage [I]. With default quantized positions nothing draws (1.4). With `-vpf` (FLOAT positions) garbage draws, with out-of-range indices. If Scene LOD is on, those indices reach `meshopt_simplify`, whose bounds are asserts only in the library [A], so there is a real crash risk [I]. |
| **C. `.gltf`, fallback without uri** | `'uri' is missing from non binary glTF file buffer.` then `File not found : ` [V `tiny_gltf.h:4553,2520`]. Parse fails, reported through `Can't load GLTF model`. |

Case B is the worst of the three and the one the current message makes look harmless: "it is loaded
without it" is wrong for such a file.

### 1.3 EXT or KHR meshopt, optional (with a real fallback)

Per the extension, a fallback buffer without a uri requires listing it as required [W], so an optional
file has real uncompressed data. gltfpack's form is `-cf`, which writes `x.fallback.bin`
[V `gltf/gltfpack.cpp:1237-1239,1643`]. tinygltf loads it as an external file and the geometry is valid
[I]. User sees: `glTF x.glb uses EXT_meshopt_compression, which the engine does not read; it is ignored`
[V `MeshSystemGltfPbr.cpp:145`]. The compressed bytes are loaded and unused.

But `-cf` output is still quantized: `KHR_mesh_quantization` remains required
[V `gltf/gltfpack.cpp:895`], so 1.4 applies and it won't render unless made with `-noq`. `-noq -cf`
or a non-quantizing producer's optional file does work today [I].

### 1.4 KHR_mesh_quantization

Reported as required-and-unread (line 140's message) although the loader has partial support [I]. What
gltfpack writes by default, and what the engine does with it:

| Data | gltfpack default | Engine | Result |
|---|---|---|---|
| POSITION | VEC3 u16, **not normalized**, stride 8 [V `gltf/stream.cpp` `writeVertexStream`; `-vpi` default, `gltfpack.cpp` help] | `getVertexFormat(3, USHORT)` returns `SG_VERTEXFORMAT_INVALID` [V `renders/sokol/SokolObject.cpp:228-233,250`] | **Does not render**; the engine's own lattice spec records this [V `docs/superpowers/specs/2026-10-23-lattice-deformer-design.md:1485-1487` (line 1486)]. Possible log: `A pipeline leaves vertex inputs of its shader ... without a buffer: undefined on Vulkan` [V `SokolObject.cpp:930`, I]. |
| NORMAL | VEC3 BYTE normalized, stride 4 | BYTE supports only 4 elements [V `:214-217`] | invalid format |
| TANGENT | VEC4 BYTE normalized | `BYTE4N` | binds |
| TEXCOORD | VEC2 u16 normalized, 12 of 16 bits used, plus `KHR_texture_transform` scale [V `gltf/stream.cpp`, `gltf/write.cpp:200-225`] | `USHORT2N` binds, **transform ignored** (1.5) | UV range about [0, 1/16] [I] |
| Rotation keys | VEC4 SHORT normalized [V `gltf/stream.cpp:806-822`] | `Cannot load animation: %s, channel %zu: no float elements` [V `MeshSystem.cpp:6211,6384`] | rotation tracks dropped on the entity path; `ModelSubClips` accepts normalized [V `animation/ModelSubClips.cpp:181-192`] |
| Morph deltas | VEC3 BYTE/SHORT | same VEC3 limit | invalid [I] |
| Bounds | | AABB skipped for non-float positions [V `MeshSystem.cpp:3235-3238`] | culling input missing [I] |
| LOD | | `findLodAttributes` requires FLOAT VEC3 positions [V `RenderSystem.cpp:7290`] | no LODs |
| Merge Static Meshes | | rejected: `A position, normal, or tangent attribute cannot be transformed safely` [V `MeshSystem.cpp:2655-2656`] | |

Non-normalized short formats are not an option on our shaders. sokol maps `SHORT[*]` to `int*` and
`USHORT[*]` to `uint*` shader inputs, only the `N` forms to `vec` [V `engine/libs/sokol/sokol_gfx.h`,
"ON VERTEX FORMATS"]. So the portable fix for all five backends is widening to FLOAT at load (or
`SHORT4N` with a matrix rescale, which complicates every CPU reader). `convertGLTFColorToVec4` is the
precedent [V `MeshSystem.cpp:2411`].

### 1.5 Companion gap: KHR_texture_transform

gltfpack lists it as *not* required [V `gltf/gltfpack.cpp:897`], yet quantized UVs need it. The
engine warns and ignores it, and a test asserts that [V `GltfMaterialExtensionsTest.cpp:251`].
Grep finds no reader [V]. The material has no UV transform fields [I]. Fix without shader cost: bake
the transform into a synthetic UV stream per primitive on load.

### 1.6 KHR_draco_mesh_compression

`TINYGLTF_ENABLE_DRACO` is not defined anywhere and no Draco library is vendored [V grep,
`engine/libs/`]. tinygltf parses the file fine. Draco primitives have accessors with no bufferView [A,
spec]. User sees `glTF x.glb requires KHR_draco_mesh_compression, which the engine does not read; it is
loaded without it`, then per primitive `GLTF primitive %u has no valid index buffer view: %s`
[V `MeshSystem.cpp:5593`]. Result: an empty model, not a failed load [I]. Optional Draco with a real
fallback works, with a warning [I].

Needs: a Draco decoder. gltfpack does not help: `file requires Draco mesh compression support`
[V `gltf/parsegltf.cpp:635-636`]. Decode editor-side as an import, FBX style, so games never carry the
decoder [I]. Draco is Apache-2.0 [A]; check before vendoring. Blender's exporter offers Draco, not
meshopt [A], so this may reach more of our users than meshopt does; worth measuring.

### 1.7 KHR_texture_basisu

gltfpack writes `{"extensions":{"KHR_texture_basisu":{"source":N}}}` and no plain `source` for `-tc`
[V `gltf/write.cpp:957-1000`]. tinygltf keeps the image's bytes (images-as-is), warning
`Unknown image format. STB cannot decode image header for image[i] ...` [V `tiny_gltf.h:2635`]. The later
decode logs `Failed to decode GLTF image %zu: ...` [V `MeshSystem.cpp:1536`]. The texture has `source`
-1: `Invalid GLTF image index -1 for texture N: x|baseColorTexture` [V `MeshSystem.cpp:2716`]. The model
draws with factors only, untextured [I]. With a PNG fallback `source`, the fallback loads and there's a
warning [I].

Needs, without duplicating S14:
- `loadGLTFTexture` reads the extension's `source`.
- The engine's transcoder runs only on web and in the editor; "native games never load Basis files"
  [V `engine/core/texture/BasisTranscode.h:4-6`]. So a native target needs the **cook** to transcode and
  re-encode to BC/ASTC, with a generation-loss warning; the web family can pass the KTX2 through.
- Embedded images "stay inside the model" today [V `docs/specs/S14-texture-compression.md:550-553`]:
  S14 only cooks external images. That's the seam to extend (section 5).

### 1.8 "KHR_texture_webp" (actually EXT_texture_webp)

`KHR_texture_webp` is absent from the Khronos index; the real name is `EXT_texture_webp` [W, and
`gltf/README.md:91`]. Same flow as 1.7 with the name swapped: no decoder for model textures (stb
only), WebP isn't in S14's source list [V `S14-texture-compression.md`, R1]. Needs an editor-only
decoder (libwebp, BSD-3 [A]) feeding S14. Players never need it. Accept both names when reading.

### 1.9 EXT_mesh_gpu_instancing

Only `LatticeBakeGltf.cpp:696` knows the name [V grep]. Engine: the message on line 140 (gltfpack marks
it required [V `gltfpack.cpp:916`]) and **one** instance is drawn at the node's transform [I]. gltfpack's
instanced node has no TRS at all [V `gltf/write.cpp:1392`], so dequantization lives in the instance data;
the single copy comes out at quantized scale if it draws at all [I].

Needs: on load, read TRANSLATION / ROTATION / SCALE / `_COLOR_0` accessors into the existing
`InstancedMesh` (`object/Mesh.h:174`, already per-instance vertex stepping, so all backends [I]).

### 1.10 "KHR_lod" and MSFT_lod

`KHR_lod` is absent from the registry; `MSFT_lod` is a vendor extension [W]. Treat both the same if a
file ever uses the name. Message: line 145 or 140.

**Actual behaviour is worse than ignoring.** `collectGLTFNodes` walks **all** `gltf.nodes` with a mesh,
not the scene's [V `MeshSystem.cpp:56-66`]. MSFT_lod's lower levels are ordinary nodes outside the scene
tree [A, spec], so **every level draws at once, overlapping** [I]. Likewise, a multi-scene file draws all
scenes [I]. Fix: walk from the default scene (needs care with old scenes, since `meshNodesMapping` is
keyed by node index), and hide nodes named by `MSFT_lod.ids`.

Mapping lower levels into `MeshLodPool` won't fit: levels there index the *same* vertices
[V `pool/MeshLodPool.cpp:151`], and no LOD Group component exists [V grep: no hits]. V1: keep LOD0,
warn, drop the others. No `lod` handling in gltfpack either: unknown extensions are discarded
[V `gltf/README.md`].

### 1.11 What a working gltfpack import needs (all five)

1. tinygltf `ParseBuffer` patch: no BIN copy for fallback or for any no-uri buffer past the first.
2. Decode meshopt bufferViews (EXT and KHR) into the fallback buffer.
3. Widen quantized attributes for sokol's formats and CPU readers.
4. Apply or bake `KHR_texture_transform`.
5. Read normalized animation outputs.

Plus capacities: gltfpack writes one bufferView per stream [I], and `MAX_EXTERNAL_BUFFERS` is 30 and
`MAX_SUBMESHES` 16 by default [V `engine/core/Engine.h:89,129`], so a many-primitive deinterleaved file
hits `reportOverCapacity` [V `MeshSystem.cpp:5341`] or forces a Compile build [V `S30-shipping.md` R5(c)].
Check it with a real file.

### 1.12 Rules that bind the fix

- Do-not-copy list [V `docs/specs/README.md`]: mentions nothing about meshopt or gltfpack (it's about
  OpenEngine). "the npm package" there is OpenEngine's, but the same reasoning rules out a Node
  dependency here [I].
- Licences: MIT/zlib/BSD/Apache may be vendored with `NOTICE.md` entries; copied code keeps its header.
  gltfpack is MIT [V `gltf/README.md`], so its functions can be adapted. Patching tinygltf needs its
  `NOTICE.md` row updated (`NOTICE.md:23` lists no local change today) [V].
- `S14`: owns KTX2, Basis, per-family formats and the texture cook. Model work must hand it pixels, not
  re-decide formats.

---

## 2. Every gltfpack feature, classified

`Have` = already covered. `Own` = implement in our importer with the same library calls. `Tool` =
worth invoking gltfpack. `Skip`. **No row earns `Tool`**, for the reasons in section 3. References are
`gltf/` unless noted.

### Input and output

| Feature | Option | Class | Note |
|---|---|---|---|
| glTF, GLB input | `-i` | Have | tinygltf |
| OBJ input | `-i` | Have | tinyobjloader |
| PLY splats input | `-i` | Skip | see Splats |
| Report, stats | `-r -v -vv -test` | Own (small) | a `get_model_cook_report` MCP tool, like `get_texture_cook_report` |

### Compression

| Feature | Option | Class | Note |
|---|---|---|---|
| Decode EXT, KHR | (input) `parsegltf.cpp:561-627` | **Own, first** | about 70 lines; the recipe to follow |
| Encode attribute, index | `-c`, `stream.cpp:873-911` | Own | Mesh Compression |
| Stronger: filters, strip order | `-cc` | Own | High |
| KHR format, level 3 | `-cz -ce` | Own | our cook only needs our decoder, so use KHR v1 |
| Fallback buffers | `-cf` | Skip | we own the loader |
| Disable quantization | `-noq` | Own | Mesh Compression Off |

### Quantization

| Feature | Option | Class | Note |
|---|---|---|---|
| Position bits, 14 default | `-vp` | Own | Compression level; 16-bit stream widens at load in v1 |
| UV bits, 12 | `-vt` | Own | needs the transform handling |
| Normal bits, 8 | `-vn` | Own | |
| Color bits, 8 | `-vc` | Own | |
| Int, normalized, float positions | `-vpi -vpn -vpf` | Own: float+exp or normalized | `-vpi` conflicts with sokol's rules (1.4) |
| Float UV, normal | `-vtf -vnf` | Own | exp filter; no transform or node change, so simplest for us |
| Interleaved | `-vi` | Own (later) | helps `MAX_EXTERNAL_BUFFERS`, hurts ratio |

Note on `prepareQuantizationPosition` [V `stream.cpp:131-170`]: one grid shared by all meshes, so no
cracks, but the precision depends on scene size, and it warns past 5% relative error. Do the same.
**Its node dequantization transform (`write.cpp`, `writeMeshNode`) adds nodes**; that's why float+exp or a
matrix-free normalized scheme suits us.

### Mesh passes

| Feature | Option / place | Class | Note |
|---|---|---|---|
| Reindex, tolerance per attribute | `mesh.cpp:770-840` | Own | Weld Vertices; `meshopt_generateVertexRemapCustom` |
| Attribute reordering (vcache + vfetch) | `optimizeMesh` `mesh.cpp:1136` | Own | Optimize Mesh. Strip variant under `-cc`. gltfpack doesn't use overdraw; we can add it |
| Degenerate triangle removal | `filterTriangles` `:842` | Own | |
| Bone filter, 4 influences, renormalize | `filterBones` `:1179` | Own | 8 groups into 4; check against our `WEIGHTS_0`-only reader |
| Unused stream filter | `filterStreams` `:579`, `-kv` | Own, conservative | it drops attributes by material needs; a user may switch materials in the editor, so default keep |
| Normals, tangents generation | `-gn -gt` | Own | `meshopt_generateNormals` (experimental; `-gn` is gated off in stable builds [V `gltfpack.cpp`]) and `meshopt_generateTangents` [V vendored `meshoptimizer.h:955`]; Unity's Calculate |
| Geometry dedup | `dedupMeshes` `:381`, `-mdd` | Own | sound, identity-preserving if kept to accessor sharing |

### Simplification

| Feature | Option | Class | Note |
|---|---|---|---|
| Ratio, error | `-si -se` | Have, partly | `MeshLodPool.cpp:160-177`: `simplifyWithAttributes`, `Prune`, `ErrorAbsolute`, runtime, ratios 1, .5, .25, .125 |
| Attribute-aware | built in | Have | |
| **Aggressive** | `-sa` | Own, off by default | only falls back to `meshopt_simplifySloppy` at 1e-1 error if the regular pass got stuck [V `mesh.cpp:1125-1129`]; breaks attribute discontinuities, so distant-only |
| **Permissive** | `-sp` | Own, checkbox | `meshopt_SimplifyPermissive` plus `simplifyProtect` locks [V `mesh.cpp:961,1093,1106`]; collapses across seams, keeps protected ones. Needs the helper; adapt with MIT header |
| Vertex update | `-sv` | Own (later) | `simplifyWithUpdate` moves vertices; disabled with morphs or UV1 [V `:1109`] |
| Lock borders | `-slb` | Own | for chunked meshes; not with Prune |
| Regularize for deform | auto `:1096` | Own | if skinned or morphed. Our runtime LOD doesn't set it today [V] |
| UV island split | `simplifyUvSplit` `:992` | Own (small) | |
| Quality scaling | `computeMeshQuality` | Skip | scene-level ratio tying; we go per-mesh |
| Destructive use (replace the mesh) | `-si` as-is | Own, as "Generate Mesh LODs" | a Unity-like importer doesn't delete source triangles by default |

### Scene

| Feature | Option / place | Class | Why |
|---|---|---|---|
| **Scene merging** | default `mergeMeshes` `mesh.cpp:481`, `-mm`, `detachMesh` `gltfpack.cpp:268` | Have (narrower), skip the rest | Merge Static Meshes per model [V `ModelComponent.h:67`, `MeshSystem.cpp:5223`]. gltfpack's global merge destroys node identity (section 3); not a default |
| **Mesh instancing** | `-mi` | Own: read it (1.9); cook only by request | writes required ext; changes the node graph, so scene-affecting |
| **Node pruning** | `markNeededNodes`, `remapNodes` `node.cpp:62,135` | **Skip** | saved scenes key entities by node index (below) |
| Keep names | `-kn` | N/A | our cook never strips |
| Material merge | `mergeMeshMaterials`, `-km` | Skip | submesh overrides, `.material` links rely on slots [I] |
| Texture merge | `mergeTextures` | Skip/S14 | dedup in S14 and `gltfTextureDedupKey` [V `MeshSystem.cpp:351`] |
| Drop unused alpha | `optimizeMaterials` | S14 has has-alpha analysis | don't change alpha mode in the model |
| Keep extras | `-ke` | Must keep always | `jasperengine_foliage_transmission` is a material extra [V `MeshSystem.cpp`, 5692] |

### Animation

| Feature | Option | Class | Note |
|---|---|---|---|
| **Resample at 30 Hz** | `-af`, `animation.cpp:286` | Own, optional, off for Off | No resampling exists in the engine [V grep]. Unity's Keyframe Reduction + Sample rate; note resampling removes CUBICSPLINE and changes times used by events [I]. |
| Constant track removal | `-ac` | Own | `Track.constant`/`dummy`. Kept nodes still exist in our model, so safe if no node removal |
| Quantize T, R, S | `-at -ar -as` | Own | requires 1.4's rotation fix; our `AnimatorPose` reads floats [I], so decode at load |

### Textures

| Feature | Option | Class | Note |
|---|---|---|---|
| ETC1S, UASTC | `-tc -tu` | Have (S14) | `editor/texture/BasisEncoder.cpp`, per-family |
| WebP encode | `-tw` | Skip | S14 chose KTX2; WebP is CPU decode and RGBA8 upload [I] |
| Quality, class | `-tq` | Have | |
| Scale, limit | `-ts -tl` | Have | Max Size |
| Power of 2 | `-tp` | Skip | WebGL1 not a target; R6's multiple-of-4 instead |
| Flip Y | `-tfy` | Skip | |
| Threads | `-tj` | Have | cook queue |
| Keep paths | `-tr` | Have | external images stay |
| Embedded images | (embed) | **Own with S14** | gap: not cooked today (section 5) |

### Other

| Feature | Option | Class | Why |
|---|---|---|---|
| **Splats** (`splats.cpp`) | `.ply`, `-gsd` | **Skip** | No splat renderer in the engine [V grep]; sorting and SH make it a render feature, not an importer one. Its passes are `spatialSortRemap`, opacity cull, SH degree trim [V `splats.cpp:272-342`]. Revisit only with a renderer spec |
| **Point clouds** | `sortPointMesh`, `simplifyPointMesh` `mesh.cpp:1261,1299` | **Skip for now** | The loader forces `TRIANGLES` and requires indices: `GLTF primitive %u has no valid index accessor` [V `MeshSystem.cpp:5587`, and no `primitive.mode` read]. So point primitives don't load at all. `meshopt_spatialSortRemap` is worth using *if* point support arrives; **non-indexed triangle primitives failing is a separate core-glTF bug** |
| `KHR_materials_variants` | passthrough | Skip | not read by engine [I] |

---

## 3. Bundle, link, or reimplement?

### Options

| | Bundle binary | Link `gltf/*.cpp` | Reimplement passes |
|---|---|---|---|
| Size | not measured (no build allowed); native without textures roughly 1 MB, with Basis and libwebp several MB [A] | adds 11.3 kLOC plus cgltf (205 KB source), fast_obj, micro_ply, sdefl | code only in the editor; decoder in the player (section 4) |
| Third-party | cgltf, fast_obj, sdefl MIT; micro_ply MIT/PD [V `extern/`]; optional **Basis fork** and libwebp | same | none new |
| Basis | upstream builds against `zeux/basis_universal`, branch `gltfpack` [V `gltf/README.md`], a second encoder beside our v2_50 [V `NOTICE.md:141`] | same conflict | S14 |
| WASM, npm | `library.wasm` isn't in the clone [V]; needs Node ≥ 18 [V `gltf/package.json`]; **no BasisU or WebP in that form** [V `gltfpack.cpp`, `__wasi__` notes]; 'native binaries are recommended' [V README] | no | no |
| Structure | `main`, `exit` codes, `fprintf`, `meshopt_encodeVertexVersion` global state [V `gltfpack.cpp:1373`] | C++11, one-shot; needs refactor on each upgrade | ours |

Licence is not the obstacle: everything is permissive. **The obstacle is what gltfpack does to files.**

### Why gltfpack's output is wrong as a pipeline step for this engine

1. **It renumbers nodes.** `remapNodes`, mesh merge and `writeMeshNode` change the graph
   [V `gltf/node.cpp:135`, `gltfpack.cpp:268`]. Scene files store `meshNodesMapping` by **node index**
   [V `editor/StreamComponents.cpp:762-768,833-838`], and Animation Player sub-clip paths address nodes
   [V `MeshSystem.cpp`, 5236]. Running it would break saved scenes. `-kn` only protects *named*
   nodes, and adds no index stability [V `node.cpp:120-127`].
2. **It discards data the engine owns.** Unknown attributes are dropped except a short list, and extras
   are dropped without `-ke` [V `gltf/README.md`, Custom data]. Affected: `_VAT_INDEX` and
   `JASPER_vertex_animation` [V `animation/VertexAnimation.h:48-50`], and the foliage extra. "Unknown
   extension nodes are discarded" [V README]. FBX import's own `asset.extras` key survives (asset extras
   are preserved) [V README, `FbxImporter.cpp:36`].
3. **Defaults assume a viewer, not an authoring pipeline**: global merge and resample in one run, with
   texture work S14 already owns.
4. **No Draco** [V `parsegltf.cpp:635`], which is the other big real-world format.
5. **A second parser** with different failure modes, and a process boundary on iOS and Android
   editor-less flows that nothing needs.
6. **Our hard requirement is decoding at runtime anyway**, which gltfpack doesn't provide. Bundling buys
   only the encoder side, the easy part: every pass is 1-10 library calls [V section 2].

### Recommendation

**Reimplement, in stages, using the vendored library. Don't bundle or link.** Adapt small helpers
(`simplifyProtect`, `simplifyUvSplit`, `reindexTolerance`, `filterBones`, animation resample) under MIT
with header and `NOTICE.md` entry. Users who want gltfpack's destructive result run it themselves;
after stage 1 its output loads. Mention it in docs.

- **Stage 1, loader (fixes what breaks today; the value is here).** A `GltfNormalize` step (name
  proposed) called at the 5 parse sites: meshopt decode, widen quantized data, bake UV transform,
  normalized animation outputs, node walk from scene, read `source` from texture extensions. Zero cost
  for a file with no extensions. Add the names to `supportedExtensions`. Change the
  `unsupported_required` fixture to an unknown name. Failure messages name the extension.
  - Lattice bake: keep refusing or read through this step; decide in the spec.
  - Validate before calling the decoder. Its checks on stride are **`assert`s** (`vertex_size <= 256`,
    `% 4`, filter strides) [V `src/vertexcodec.cpp` `meshopt_decodeVertexBuffer`,
    `src/vertexfilter.cpp:1215,1234,1248,1261`, `src/indexcodec.cpp:393-394`], and `last_vertex[256]` is
    a stack buffer, so a malformed file in a release build corrupts memory [I]. Also check
    `byteLength == stride*count` and offsets, and cap the fallback allocation.
  - Test with real gltfpack and gltf-transform fixtures generated outside and committed small, each
    case in 1.2 red first.
- **Stage 2, Model Importer.** Settings in the S04 meta, `import.modelImporter` with `version: 1`,
  beside `import.textureImporter` [V `editor/texture/TextureImportSettings.h:4-16`]; `ModelImporterView`
  beside `TextureImporterView`; undo through `SetAssetMetaCmd`. MCP: `get_model_import_settings`,
  `set_model_import_settings`, `reimport_models`, `get_model_cook_report`; extended `get_model_info` with
  required extensions, sizes, and compression status (mirroring [V `AssetTools.cpp:4692,4707,4723`]). CLI
  `cook-models --check`. No Lua: import-time only.
- **Stage 3.** Baked LODs, instancing cook option, embedded image hand-off to S14, Draco import.

**Invariant for every stage 2 pass:** node count and order, mesh, primitive, material and animation
order unchanged, checked by a test. Everything in the table below can honour it; anything that can't
(merges) stays an explicit separate operation.

### Unity names mapped to meshoptimizer

Unity's options [W]. Bits below are a **starting proposal to measure** [A]; Unity doesn't publish its
own [W says only "fewer bits"].

| Unity property | Value | Calls and knobs |
|---|---|---|
| **Mesh Compression** | Off (default; old projects unchanged) | No cook. FLOAT stays. |
| | Low | Quantize: pos 16, UV 14, normal 10, color 8. `meshopt_encodeVertexBufferLevel(..., level 2, version 1)`, `encodeIndexBuffer`. No lossy filters. |
| | Medium | gltfpack defaults: 14, 12, 8, 8 [V `gltfpack.cpp:1300-1310`]. Same codec. |
| | High | 12, 10, 8, 8; `encodeFilterOct`, `encodeFilterExp` (positions), `encodeFilterQuat` (rotation at 12 bits); `optimizeVertexCacheStrip`; level 3 (`-cz` [V `gltfpack.cpp:933`]). |
| **Read/Write** | On (our default, deviation from Unity's Off) | No meshopt call. Governs dropping `buffers[].data` after upload. CPU readers require it today: LOD, CpuSkin, picking, Mesh Collider, Particle shapes [I]. Unity's list is similar [W]. Off needs those to refuse. |
| **Optimize Mesh** | Everything (default once cooked) | `generateVertexRemap(Multi)`, `optimizeVertexCache`, optional `optimizeOverdraw`, `optimizeVertexFetchRemap` + remap every stream including morphs. |
| | Polygon Order | `optimizeVertexCache` only |
| | Vertex Order | `optimizeVertexFetch` only |
| | Nothing | none |
| **Weld Vertices** | On | `generateVertexRemapCustom`, tolerances per attribute [V `mesh.cpp:770-790`]; off keeps the order |
| **Index Format** | Auto (default), 16 bit, 32 bit | No library knob. Auto as `writeIndexStream` [V `stream.cpp:763`]. 16 is refused past 65535 vertices (no split). Loader reads 8/16/32 [V `MeshSystem.cpp:5613-5617`]. |
| **Generate Mesh LODs** | On/Off | `simplifyWithAttributes`, as `MeshLodPool` [V `:160-177`], baked at cook; `Prune`; `Regularize` for skinned; + **Permissive** and **Aggressive** (Sloppy) checkboxes. |
| Discard Odd Levels | | keep levels 2 and 4 of `MAX_MESH_LODS` 4 [V `Engine.h:94`] |
| Limit LODs, Maximum Levels | | `target_error` cap, level count |
| **Normals** | Import, Calculate, None | `meshopt_generateNormals`; crease angle = Smoothing Angle |
| **Tangents** | Import, Calculate, None | `meshopt_generateTangents` [V]; compare with Mikktspace before calling it Mikktspace [A] |
| **Import Vertex Colors, Import UVs** | | strip streams, like `filterStreams`; default keep |
| **Anim. Compression** (Animation tab) | Off, Keyframe Reduction | gltfpack's resample and constant-track logic; Rotation, Position, Scale Error as tolerances; bits via `encodeFilterQuat`, `encodeFilterExp` |
| Optimize Game Objects (Rig) | | not offered: node numbering invariant |

Scene `mesh_lod_enabled` / `meshLodThreshold` [V `SceneSettings.h:354-355`] and per-mesh `lodEnabled`,
`lodBias` keep working: baked levels only pre-fill `MeshLodPool` (keyed by geometry hash), and it falls
back to the job it runs today [V `RenderSystem.cpp`, `updateMeshLods`]. On mobile that removes a
background simplify per mesh per run [I].

---

## 4. Decoder runtime

**Web: use the C++ decoder through Emscripten, not `meshopt_decoder.mjs`.** The JS module exists for JS
loaders like three.js [V `gltf/README.md`, `js/README.md`]. Our tinygltf and loader are C++ in wasm and the
decode is a synchronous step on the parse path; a JS bridge would add async plumbing, a heap copy, and
~29 KB [V `wc -c`: 29019 bytes] for no gain [I]. It would also mean a second implementation on one platform.

**Size.** The JS module's embedded wasm decodes to **8413 bytes (scalar) and 12259 bytes (SIMD)** (computed
with my own script reading the strings, for vertex, index, and all filters) [V]. Those were built with
`-O3`, `-nostdlib`, and size-minded flags [V `Makefile:36-41`], so expect Emscripten's to be the same order
[I]. Sources total 4108 lines [V]. `meshoptimizer` already links everywhere [V `engine/CMakeLists.txt:2114`,
`engine/notices.json:29`], but only the objects used are pulled in, so the decoder objects are new to players
[I]. `tools/players/lib_size.py` already groups `meshopt_` [V `:94`], so measure there.

**Disabled cost.** No run cost without the extension. One exception: `vertexcodec.cpp` builds 2304 bytes of
shuffle tables in a static initializer on SIMD paths [V `src/vertexcodec.cpp:800-829`], once at startup.

**Speed.** C++ on desktop: 3-6 GB/s vertex and index; filters 5-10 GB/s; ratio typically 2-4x over already
quantized data; more with a general compressor [V `README.md:419,449,531`]. JS with SIMD 1-3 GB/s
[V `js/README.md`]. Mobile and scalar figures aren't published there. 10 MB of decoded geometry is single-digit
milliseconds on a desktop [I]; state a mobile budget from measurement. `MESHOPTIMIZER_VERTEXCODEC_ZEROCOPY` gives
~10% when the target is cached memory [V `README.md:419`], which is ours (a `std::vector`), so define it [I].
No allocation in decoders; stack bounded at 32 KB [V `README.md:995-997`]: fine for worker threads with small
stacks on web, but check them [I].

**SIMD on each export:**

| Target | Path | Selected by |
|---|---|---|
| Windows, Linux, macOS x64 | SSSE3 + POPCNT, else scalar | cpuid at runtime, function-level target or MSVC [V `vertexcodec.cpp:22-31`] |
| Android x86, x86_64 | same | same [I] |
| Android arm64-v8a | NEON + latency optimization | `__ARM_NEON` [V `:35-36,59`] |
| Android armeabi-v7a | NEON without the latency path; filter code has non-FMA and non-aarch64 paths | NDK enables NEON by default [A]; [V `:59`, `vertexfilter.cpp:545-568,649`] |
| iOS, Apple silicon arm64 | NEON + latency | [V] |
| HTML5 | **Scalar today** unless the file is built with `-msimd128`. | `__wasm_simd128__` only; no runtime detection possible [V `vertexcodec.cpp:44-46`] |

**The web catch.** `engine/libs/meshoptimizer/CMakeLists.txt` sets no SIMD flag and the engine applies `-msimd128`
per file (particles, `AnimatorPose.cpp`), because Jolt's detection breaks under a global flag
[V `engine/CMakeLists.txt:2125-2140`]. So add the flag for `vertexcodec.cpp` and `vertexfilter.cpp` when
`JASPERENGINE_WASM_SIMD` is on, and keep the OFF build scalar. Since the default web build already contains
simd128 code, this adds no new browser requirement [I]. Keep the simplifier out of the flag: the engine relies on
bit-identical simplifier output (`-ffp-contract=off`, `engine/CMakeLists.txt:1264-1268`).

Codec output is lossless so SIMD and scalar must match byte for byte [I]. **Filters do float maths; test that scalar
and SIMD agree (exactly, or document the tolerance) across SSE, NEON, wasm**, since the project values one result
everywhere. Web single-thread runs the decode on the main thread [V `S14-texture-compression.md`, section 3; `docs/shipping.md:51` adds a pthreads player], acceptable at
these speeds [I].

**Vendored 1.2 is enough.** Header API differs by one function, `meshopt_setAllocatorCache` [V diff]. Decoder
version constant is 1 in both [V `vertexcodec.cpp:126`]. `decodeFilterColor`, Permissive, `simplifyWithUpdate`, and
`generateTangents` are all present [V vendored `meshoptimizer.h:424,479,564,955`]. `vertexcodec.cpp` does differ in
internals; I read only the start of that diff. Upgrading isn't required for this work.

**Encoding versions.** EXT requires codec version 0, KHR allows v1 (better ratio) [V `js/README.md`, W]. Use the
`version` argument on `encodeVertexBufferLevel`, not `meshopt_encodeVertexVersion`, which is global and not
thread-safe [V `meshoptimizer.h:381`] and would race on S06 jobs [I]. Decoding detects the version from the header
[V `README.md:540`].

---

## 5. Can the exported format use meshopt?

### What the player reads

Raw glb, parsed by tinygltf in the player.

- Prebuilt: `.jscn` cooked scenes hold `filename`; for model-backed meshes the attributes aren't stored ("a model
  load rebuilds the attributes") [V `engine/core/serialization/codecs/ComponentHooks.cpp:52-56`].
- Compile: generated C++ `modelcomp.filename = "..."` [V `editor/Factory.cpp:2113`].
- Both end in `loadGLTF`, which parses the JSON and copies images at every first load [I].
- `.jpak` has no compression field [V `engine/core/io/JPak.h:9-12`]; packed content is the bytes [I]. So today a
  game ships uncompressed FLOAT geometry, unless hosting gzips.
- Non-model meshes (`runtimeGeometry` false) embed buffers in the cooked scene [V `ComponentHooks.cpp:60-66`]. Out
  of scope here, a candidate later.

### Proposal: no new format. A cooked glb.

The stage 1 loader already reads meshopt-compressed glTF, so the cook writes **a valid glb** with
`KHR_meshopt_compression`, in the same structure as the source.

```
assets/je-cooked/models/index.json     source path -> cooked file, version, sizes (as CookedTextures)
assets/je-cooked/models/<hash>.glb
.jasperengine/cache/models/v1/<k[0:2]>/<k>.glb, <k>.json
```

`k` = SHA-256 of source SHA-256, resolved settings, `kModelCookVersion`, meshoptimizer version, following
`TextureCookCache` [V `editor/texture/TextureCookCache.h:4-11`]. Atomic writes, LRU, background queue, and the same
"provider" pattern in the editor: `CookedTextures::find` is the shape to copy
[V `engine/core/texture/CookedTextures.h`]. `MeshSystem::getImportedModelPath` is the one redirection point to extend
[V `MeshSystem.cpp:310`]. It's already used by every reader [I, `ModelSubClips.cpp:270,399,687`], though `ParticleMeshes`
and `VatSourceMesh` open paths directly and must be routed.

Why not `.jmesh`: another reader, another fuzz surface, a broken external-tools story; meshopt is designed for
"store encoded data directly" and to stay compressible [V `README.md:397`]. A custom format could later save the
JSON parse and allow keeping normalized formats on the GPU; do that only if a profile justifies it.

### Versioning and "old projects must load"

| Change | Rule |
|---|---|
| New meta section | Absent means defaults, Compression **Off**: no cook, the source ships. Unknown keys preserved [V S14 pattern]. |
| Scene format | **No bump**. `filename` stays the source path; numbering is unchanged by the invariant, so `meshNodesMapping` and sub-clip paths keep working. Add a cook-time assertion. |
| Settings format | `import.modelImporter.version: 1`. Migrate upward on read, as textures do [V `TextureImportSettings.cpp:221-223`]. |
| Cook logic | Bump `kModelCookVersion`; cache misses; recooks. Sources are never altered. |
| `index.json` | carries `version`; missing or unknown entry loads raw [V same rule: "Paths not in the index load raw", `CookedTextures.h`]. |
| Player content | Readers refuse a newer major; a newer minor only adds fields [V `CookedScene.h:18,44`]. A player too old to decode can't be told by that rule alone: record a capability in `player-info.json` and have export refuse or disable the cook on mismatch [I]. Prebuilt players ship with the editor [A], so this mostly protects stale templates. |
| Exported source projects | Compile builds compile the matching engine; no issue [I]. |
| If a pass ever must change the graph | Separate opt-in, with a real scene migration: not in this plan. |

A meshoptimizer upgrade keeps old data decodable [V `README.md:537`], and pinned `version` keeps new cooks stable.

### Relation to S14

- **Don't duplicate.** S14 decides formats, KTX2, mips, quality, Max Size and platform families. The model cook touches
  geometry and animation only.
- **External images:** nothing changes; `gltfCookedImagePath` keeps redirecting [V `MeshSystem.cpp:262-272`].
- **Embedded images are the seam.** Today they stay in the model and upload as RGBA8 [V `S14`, Deviations 4;
  `docs/textures.md:60`]. A compressed glb with a 20 MB PNG inside is mostly unimproved, so this is where much of the
  download size is. Proposal: treat embedded images as S04 sub-assets (the table already lists `image` [V `S04:276`]),
  cook them through S14 under a synthetic source id, strip them from the cooked glb and reference them by cook id;
  typed by material slot, using S14 R2's existing inference. **This needs a change to S14's text and a Deviations entry.**
- **Input KTX2, WebP:** 1.7, 1.8. Add `.webp` to S14's R1 source list; Basis input re-encodes per family with a warning.
- **Same export step and report:** `Exporter::cookTextures` [V `Exporter.cpp:1115,2702`] gets a `cookModels` sibling
  before `copyAssets`; a cooked source is dropped the way `cookedTextureSources` are [V `:2807`].

---

## 6. Uncertain, and what to do next

1. **No real file was run.** Generate gltfpack (`-c`, `-cc`, `-cz`, `-noq`, `-vpf`, `-mi`, `-tc`) and gltf-transform files
   on a build server and run stage 1's red tests. Case B's exact symptom is the least certain claim here.
2. **gltf-transform's and others' layouts** assumed like gltfpack's; the extension text supports this [W, A].
3. **Share of real assets per extension:** no data here. Whether Draco or meshopt comes first should be decided by a
   sample of the assets users actually have.
4. **Unity's bit counts** aren't documented; proposed levels need PSNR and size measurements.
5. **KHR_meshopt status** was "Release Candidate" with `EXT_` ratified, per fetched text, through a summarising tool, and
   one search hit contradicted the naming history (a crate's comment); the repo README is the better source [W].
   Support both regardless, as gltfpack does.
6. **`engine/tests` change:** `UnsupportedWarns` must move to a truly unknown extension name when meshopt becomes
   supported; say so in the commit, as this is a fixture change with a real cause.
7. **Not traced:** picking BVH, `PlacementWorld` and CpuSkin with widened data (expected to work, as they then see FLOAT
   [I]); thumbnails on failure; prefab baking.

## Sources

- [glTF extensions registry index](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/README.md)
- [KHR_meshopt_compression](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_meshopt_compression/README.md)
- [EXT_texture_webp](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Vendor/EXT_texture_webp/README.md)
- [Unity manual, Model tab](https://docs.unity.com/en-us/engine/current/manual/assets-and-media/asset-types/models/importing/fbximporter-model)
- [draco-gltf crate source (the contradicting comment)](https://docs.rs/crate/draco-gltf/latest/source/src/extensions.rs)
