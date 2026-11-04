# meshoptimizer audit for JasperEngine

Repo: `/var/lib/orquester/workspaces/jaspersito/forge-meshopt` (paths below are relative to it).
Upstream: clone at `f0a684f`, plus history and release notes fetched separately.

**Labels.** [V] verified: I read the file:line or ran the diff or measurement. [I] inferred from
reading code, not run. [A] assumed. [E] estimate or extrapolation, with its basis stated.

**Method and limits.**
- Nothing was built or executed from the clone, and the repo was not edited.
- Outside the brief, I fetched upstream history (`meshopt-history/m.git`, blobless), the v1.2
  sources (`meshopt-v1.2-baseline/`) and the GitHub release notes (`releases.json`) under
  `/tmp/forge-meshopt-research/`. The clone is shallow and has no CHANGELOG, so the v1.2 question
  could not be answered from it.
- My own parsers are in `/tmp/forge-meshopt-research/scripts/`, run with `python3 -I`. They read
  mesh files as data and pipe bytes through `gzip` and `zstd`.
- No engine build exists on this host and builds belong on the build servers, so **no engine
  binary was measured**. Code sizes come from upstream's prebuilt wasm embedded in `js/`.
- No representative shipped game is in the repo. The largest mesh file is a 2.6 MB test asset.

---

## Findings that change the brief

1. **The vendored copy is not v1.2.** It is an unmodified master snapshot from between
   2026-09-10 and 2026-09-17, so it already contains most of 1.3. `NOTICE.md:30` and the version
   macro both say 1.2, and that is wrong.
2. **`f0a684f` is not v1.3 either.** It is 3 weeks past the tag. There are three states to compare.
3. **Every code path the engine calls is byte-identical in all three states.** The upgrade cannot
   move LOD, placement or golden output. It also gives the engine nothing until new APIs are adopted.
4. **The splats merge touches no file in `src/`.**
5. **The largest gains are outside the upgrade.** The biggest single one is that `loadOBJ` never
   deduplicates vertices (4.76x on a real mesh).

---

## 1. Upgrade: vendored vs v1.3 vs master

### 1.1 Provenance

- All 22 vendored source files match upstream blobs at `bba256e` (2026-09-10) and at `3e5156f`
  (2026-09-16), with 0 differing lines in a strict sorted hash comparison [V]. Upstream's next
  `src/` change is `717ca34` (2026-09-17) [V].
- **There are no local patches, so an upgrade loses nothing** [V].
- Local additions are only `CMakeLists.txt` (9 lines of build glue) and `LICENSE.md`, which is
  sha256-identical to upstream [V].
- The directory has one commit in the repo: `c2eb33b39` (2026-09-20) [V].
- Against the real tag `v1.2` the vendored copy differs in 8 files, including `simplifier.cpp`
  (283 lines) and `tangentspace.cpp` (297 lines), and `remesher.cpp` does not exist at that tag
  (404) [V]. The macro still reads `1020` because upstream bumped it only at release (`7630695`) [V].

Because the snapshot is post-v1.2, these are **already vendored**: `meshopt_remesh`,
`meshopt_generateNormals`, `meshopt_SimplifyErrorClamped`, `meshopt_SimplifyPreserveFolds`
(`engine/libs/meshoptimizer/meshoptimizer.h:483,485,970,997`), and stable
`meshopt_SimplifyPermissive` (`dc9ee93` precedes the snapshot) [V]. No upgrade is needed to use them.

### 1.2 What changed, by file

| File | snapshot to v1.3 tag | v1.3 tag to f0a684f | Engine calls it? |
|---|---|---|---|
| `simplifier.cpp` | none | none | **yes** |
| `vcacheoptimizer.cpp` | none | none | **yes** |
| `quantization.cpp` | none | none | **yes** |
| `allocator.cpp` | none | +171 lines (allocator cache) | yes, indirectly |
| `meshoptimizer.h` | version, Remesh enum | `setAllocatorCache`, comment | yes |
| `clusterizer.cpp` | +9 (SSE prefetch) | 9 (kd-tree split rule) | no |
| `remesher.cpp` | 157 (quadric decider, speedups, `Thicken` removed) | none | no |
| `stripifier.cpp` | none | 54 (algorithm improved, output changes) | no |
| `tangentspace.cpp` | none | 40 (degenerate-triangle normals) | no |
| `vertexcodec.cpp` | none | 91 (encoder 4-channel refactor, decoder rename) | no |
| `indexcodec.cpp` | none | 11 (faster edge search) | no |
| `meshletutils.cpp` | none | 17 (templated bounding sphere) | no |
| all others | none | none | no |

All [V] (`git diff --stat`, `diff -u`). Header diffs do not touch `meshopt_Allocator` or any wrapper
the engine uses [V]. No files are added, so `CMakeLists.txt` (a GLOB) and the Xcode project are
unaffected [V].

### 1.3 Release information

There is no CHANGELOG in the repo; these come from the GitHub releases API [V].

- **v1.3 (2026-09-25):** voxel remesher, `generateNormals`, `PreserveFolds`, `ErrorClamped`,
  `SimplifyPermissive` stable; `generateTangents`, `filterIndexBuffer*`, `opacityMap*` and
  `computePositionExponent` stable; `decodeIndexBuffer` about 15% faster (Clang/GCC);
  `buildMeshletsSpatial` about 15% faster.
- **Splats (#1109):** `git diff --stat f0a684f^1 f0a684f -- src/` is **empty** [V]. It adds only
  gltfpack `.ply` to `KHR_gaussian_splatting` conversion (`gltf/splats.cpp`, `extern/micro_ply.h`,
  `demo/splats.html`). The "splat" hits in `src/vertexfilter.cpp` are SIMD intrinsics like
  `wasm_f32x4_splat` [V]. Nothing applies to the engine. No gaussian-splat renderer turned up in
  `engine/core` [I: the keyword hits were fluid "splat" and UI code].

### 1.4 Why `-ffp-contract=off`, and what it means here

`engine/CMakeLists.txt:1264-1269`: with FMA contraction, an arm64 Clang build collapsed different
edges than x86-64, and the absolute 1 cm target strayed to 1.5 cm, caught by
`PlacementWorldReview.SimplificationErrorIsAbsolute` on macOS [V]. Same fix as Recast above it.

- Keep the flag. It lives in engine CMake, so the upgrade does not disturb it [V].
- It is skipped on MSVC (`if(NOT MSVC)`) [V]. MSVC's default not contracting is [A]; the macOS
  failure above shows a cross-platform determinism test is what catches this kind of drift.
- It gives a useful property for a future cook: the same bytes from any export host [I]. Key the
  cook cache on a version constant the way `kTextureCookVersion` works
  (`editor/texture/TextureCookCache.h`) [V].

### 1.5 Risk and verdict

| Risk | Assessment |
|---|---|
| LOD or placement output, goldens | **None expected.** Sources for all engine calls are identical [V]; bit-identical output follows if compile flags stay the same [I]. |
| API break | `meshopt_RemeshThicken` removed and `RemeshShell`/`RemeshSolve` renumbered [V]. Engine uses neither [V]. |
| Encoded-data compatibility | None shipped yet. Upstream: guarantees apply to point releases, "*no* guarantees are given for development branch" (README, Versioning) [V]. |
| New runtime code at `f0a684f` | `allocator.cpp` adds `std::atomic<uint64_t>` and `thread_local` [V]. Unused code should be collected at link [I]. `MESHOPTIMIZER_ALLOC_NOCACHE` removes it outright [V]. |

**Verdict: upgrade to tag `v1.3`, not `f0a684f`.**
- Do it as a drop-in, with no cherry-picks, so "no local patches" stays true.
- Correct `NOTICE.md:30`. Today it should read "post-1.2 snapshot".
- Reason to prefer the tag: we are about to ship encoded data, and only releases carry a
  compatibility guarantee.
- Master's useful extras (encoder speed, the normal fix for degenerate triangles) can wait for v1.4.
- Run `tools/check-changed` on a build server. Expect no golden change; if one moves, that
  contradicts this analysis and needs investigating before anything is regenerated.

---

## 2. API inventory

Line numbers refer to upstream `src/meshoptimizer.h`.

**Verdicts.** NOW: next changes. SOON: once the mesh cook exists. LATER: real but not yet worth it.
NEVER: needs a feature the constraints forbid, or has no use here.
**Runs at.** R: runtime load. I: FBX import. X: export cook. E: editor tool.

Benefit figures come from upstream's README unless marked as measured.

**Caller check [V]:** the brief's list is correct and complete.
- `engine/core/pool/MeshLodPool.cpp:164,169,177`: `simplifyWithAttributes`, `simplify`, `optimizeVertexCache`
- `engine/core/physics/PlacementWorld.cpp:1345`: `simplify`
- `engine/renders/sokol/SokolTexture.cpp:312`: `quantizeHalf` (texture data, not geometry)
- Name-only mentions: `LatticeBakeGltf.cpp:32`, `GltfMaterialExtensionsTest.cpp:258`, `lib_size.py:94`.

### Indexing and remap (`indexgenerator.cpp`)

| Function | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `generateVertexRemap` | Remap table by binary-equal vertices | no | **Measured: pirate.obj 15,030 to 3,156 vertices (4.76x)**; VB 721,440 to 151,488 B at the engine's 48 B stride. Also unblocks OBJ LODs | `MeshSystem.cpp` `loadOBJ`, after the corner loop (~6720) | R | **NOW** |
| `generateVertexRemapMulti` | Same, over deinterleaved streams | no | Welds per-corner glTF (Khronos grids are unindexed soups, 0.18-0.20 unique positions per vertex, measured) | Exporter mesh cook | X | **NOW** |
| `generateVertexRemapCustom` | Same, with tolerance callback | no | Merges float-drift duplicates binary equality misses | Cook | X | SOON |
| `remapVertexBuffer`, `remapIndexBuffer` | Apply remap | no | Required by the above | With the above | R,X | **NOW** |
| `filterIndexBuffer`, `...Multi` | Drop degenerate and duplicate triangles | no | Fewer triangles after quantization; size unquantified upstream. Key must include skin/morph data | Cook, after quantize | X | SOON |
| `generateShadowIndexBuffer`, `...Multi` | Second IB welded on position only | no | Upper bound from unique positions: **0.96-1.00 on smooth fixtures (no gain), 0.79 AnisotropyRotation, 0.47 robot_apose, ~0.2 on soups** (measured). README agrees it may not be worth it with few seams | Cook; `RenderSystem.cpp:6876,6912` (`depthRender.setIndex`) | X | SOON, per mesh when it saves over ~20% |
| `generatePositionRemap` | Map to first vertex at same position | no | Needed for `Protect` locks in permissive LODs | `MeshLodPool.cpp` | X,R | SOON |
| `generateAdjacencyIndexBuffer` | GS adjacency | no | No geometry shaders in sokol or WebGL2 | none | none | NEVER |
| `generateTessellationIndexBuffer` | PN-AEN indices | no | No tessellation stage | none | none | NEVER |
| `generateProvokingIndexBuffer` | Visibility-buffer provoking IB | no | Needs manual vertex pulling; out of pipeline | none | none | NEVER |

### Vertex cache, overdraw, fetch

| Function | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `optimizeVertexCache` | Reorder triangles for reuse | **LOD 1+ only** | **LOD0 is never optimized**: the loop starts at `l = 1` (`MeshLodPool.cpp:157`) and level 0 draws source order (`RenderSystem.cpp:8431`) [V]. Metric: worst ACMR 3, real meshes 0.5-1.5. Measure with the analyzer | `GlbBuilder.cpp:938`; cook | I,X | **NOW** |
| `optimizeVertexCacheStrip` | Same, favouring index compression | no | Smaller index data at some transform cost | Cook, when codec on | X | SOON |
| `optimizeVertexCacheFifo` | FIFO variant, ~2x faster, worse | no | Only for fast iteration | none yet | I | LATER |
| `optimizeOverdraw` | Reorder to cut overdraw, threshold 1.05 | no | README: no benefit on TBDR (PowerVR, Apple). Mobile-first says skip; reorders triangles | Cook, desktop targets | X | LATER |
| `optimizeVertexFetch` | Reorder interleaved VB by first use | no | Overfetch toward 1.0; prerequisite for good codec ratio | `loadOBJ` | R,X | **NOW** |
| `optimizeVertexFetchRemap` | Same, as remap for many streams | no | Same; engine's glTF is deinterleaved. Also gives the "coarsest LOD first" order (see section 3.6) | `GlbBuilder.cpp:938`; cook | I,X | **NOW** |

### Quantization (`quantization.cpp`, header inlines)

| Function | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `quantizeHalf` | float to half | **yes (textures)** | `SokolTexture.cpp:312` | n/a | R | keep |
| `quantizeFloat` | Zero low mantissa bits, stays float | no | **No format or shader change**, just compresses better (README: 15 or 7 bits) | Cook | X | SOON |
| `quantizeSnorm`, `quantizeUnorm` | Inline to normalized ints | no | BYTE4N normals and tangents, USHORT2N UVs (section 3.5) | Cook | X | SOON |
| `dequantizeHalf` | half to float | no | Only for CPU readers of half data | none | none | LATER |
| `computePositionExponent` | DXR2 shared exponent | no | Raytracing only | none | none | NEVER |

### Codecs (`vertexcodec.cpp`, `indexcodec.cpp`, `vertexfilter.cpp`, `meshletcodec.cpp`)

| Function | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `encodeVertexBuffer`, `...Bound`, `...Level`, `encodeVertexVersion` | Lossless vertex encode | no | **Measured on pirate.glb: 1.97x position, 2.05x normal** (old gltfpack 0.14); README typical 2-4x on quantized data. EXT needs v0, KHR allows v1 (v1.0 notes) | Cook | X | SOON |
| `decodeVertexBuffer`, `decodeVertexVersion` | Decode, 3-6 GB/s, no allocation | no | About 3.5 KB gz for the whole decoder (section 4.3) | After tinygltf parse (section 3.7) | R | SOON |
| `encodeIndexBuffer`, `...Bound`, `encodeIndexVersion`, `decodeIndexBuffer`, `decodeIndexVersion` | Triangle list codec | no | **Measured: 1.13 B/triangle, 5.31x vs u16**; README says 1-1.2. Needs vcache and fetch order first | Cook; reader | X,R | SOON |
| `encodeIndexSequence`, `...Bound`, `decodeIndexSequence` | Generic index codec | no | Sparse LOD index sets: README warns the triangle codec uses ~2 B/index there | Baked LODs | X,R | SOON |
| `encodeFilterOct`, `decodeFilterOct` | Octahedral normals and tangents | no | Pirate normals 1.95 B/vertex (measured) | Cook, reader | X,R | SOON |
| `encodeFilterExp`, `decodeFilterExp` | Shared-exponent float | no | **Decodes to FLOAT**, so positions and UVs need no format or shader change (gltfpack `-vpf`) | Cook, reader | X,R | SOON |
| `encodeFilterColor`, `decodeFilterColor` | YCoCg color | no | Vertex colors | Cook, reader | X,R | SOON |
| `encodeFilterQuat`, `decodeFilterQuat` | Quaternion | no | Animation tracks, not geometry | Animation assets | X,R | LATER |
| `encodeMeshlet`, `...Bound`, `decodeMeshlet`, `decodeMeshletRaw` | Meshlet topology codec | no | Only for mesh shading or clustered RT | none | none | NEVER |

### Simplification (`simplifier.cpp`)

| API | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `simplify` | Topology-preserving decimation | **yes** | Used at `MeshLodPool.cpp:169` and placement | n/a | R,E | keep |
| `simplifyWithAttributes` | Attribute-aware | **yes** | See the weight finding below | `MeshLodPool.cpp:164` | R | keep, fix |
| `SimplifyErrorClamped` (experimental) | Clamp attribute error | no | README: "recommended" when error drives LOD selection, which `selectMeshLod` does. Less conservative switching. **Changes LOD output** | `MeshLodPool.cpp:160` | R | **NOW** |
| `SimplifyPermissive` (stable) | Collapse across seams | no | README: higher quality, "dramatically" above sloppy. Fixes stuck faceted meshes. Pair with `Protect` | `MeshLodPool.cpp:160`, when a level stalls | R,X | **NOW** |
| `SimplifyVertex_Protect` | Keep a seam in permissive mode | no | UV seams | `MeshLodPool.cpp` | R,X | SOON |
| `SimplifyLockBorder` | Lock open borders | no | Engine simplifies per submesh without it; cracks between submeshes at higher LODs are possible [I]. Evaluate, since it also limits reduction | `MeshLodPool.cpp:160` | R,X | SOON |
| `SimplifyRegularize`, `...Light` | More regular triangles | no | Better under skinning (README) | `MeshLodPool.cpp`, skinned sources | R,X | SOON |
| `SimplifyErrorAbsolute`, `SimplifyPrune` | Absolute error; drop islands | **yes** | Both set at `MeshLodPool.cpp:160` | n/a | R | keep |
| `SimplifySparse` | Faster on subsets | no | Only for chained LODs; engine cuts each from source on purpose (`MeshLodPool.cpp:158` comment) | Baked chains | X | LATER |
| `SimplifyPreserveFolds` (experimental) | Keep thin-sheet folds | no | Foliage, remesh output | Cook | X | LATER |
| `SimplifyVertex_Lock`, `..._Priority` | Per-vertex control | no | Authoring feature | Cook | X | LATER |
| `simplifyWithUpdate` | Moves vertices | no | Better coarse LODs; ~doubles vertex memory (README). Last levels only | Baked LODs | X | LATER |
| `simplifySloppy` | Ignores topology | no | README: worse than permissive. Far HLOD only | Cook | X | LATER |
| `simplifyPrune` | Island removal only | no | Scan cleanup | Editor | E | LATER |
| `simplifyPoints` | Point cloud decimation | no | `Points` object could use it; no demand found | none | none | LATER |
| `simplifyScale` | Error scale | no | Unneeded with `ErrorAbsolute` | none | none | NEVER |

### Analyzers (`indexanalyzer.cpp`, `rasterizer.cpp`)

| Function | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `analyzeVertexCache`, `analyzeVertexFetch` | ACMR, ATVR, overfetch | no | Evidence for this work, and CLAUDE.md requires agents can check it. Red control for the cook | Editor, MCP tool | E,X | **NOW, with the cook** |
| `analyzeOverdraw`, `analyzeCoverage` | Overdraw; silhouette per axis | no | Reports; LOD silhouette regression checks | Editor, MCP tool | E | SOON |

### Meshlets, clusters, spatial

| Function | What it does | Used | Benefit here | Verdict |
|---|---|---|---|---|
| `buildMeshlets`, `...Scan`, `...Bound`, `...Flex`, `...Spatial` | Build clusters | no | Value is in mesh shaders or GPU culling, both excluded. CPU cluster culling multiplies draw calls on WebGL2 [I] | LATER, big static meshes only |
| `optimizeMeshlet`, `optimizeMeshletLevel`, `extractMeshletIndices` | Meshlet processing | no | Depends on the above | NEVER unless meshlets land |
| `computeClusterBounds`, `computeMeshletBounds` | Bounds and normal cone | no | Same | LATER |
| `computeSphereBounds` | Sphere over spheres | no | Engine uses AABBs | LATER |
| `partitionClusters`, `demo/clusterlod.h` | Nanite-style DAG LOD | no | Practical use needs GPU-driven selection, on the do-not-copy list (`docs/specs/README.md`) | NEVER |
| `spatialSortRemap` | Locality order for unindexed data | no | Compression of point and instance data | SOON |
| `spatialSortTriangles` | Spatial triangle order | no | For merged static batches | LATER |
| `spatialClusterPoints` | Point clusters | no | No use yet | LATER |

### Remaining

| Function | What it does | Used | Benefit here | Plug-in | At | Verdict |
|---|---|---|---|---|---|---|
| `stripify`, `stripifyBound` | List to strip | no | 50-60% of indices, ~5% worse ACMR. The index codec beats it for download, and LOD code expects lists | none | none | NEVER |
| `unstripify`, `unstripifyBound` | Strip to list | no | Would give LODs to strip meshes, which `findLodAttributes` rejects today | `RenderSystem.cpp` | R | LATER |
| `opacityMapMeasure`, `...Rasterize`, `...EntrySize`, `...Compact` | Ray tracing opacity micromaps | no | No ray tracing | none | none | NEVER |
| `generateTangents`, `TangentCompatible` | MikkTSpace-style, 6-10x faster than `mikktspace.c` (v1.2 notes) | no | **FBX import emits no tangents and engine/editor have no generator** [V], so normal maps there take the non-attribute path [I]. Gives bake-matched tangents. Cost: +4 to 16 B/vertex, plus splits | `GlbBuilder.cpp:938`, option | I | SOON |
| `generateNormals` (experimental) | Crease-angle normals | no | Better than flat `objFaceNormal` for OBJ without normals. Master has a degenerate fix | `loadOBJ` or cook | R,X | LATER |
| `remesh` (experimental) | Voxel remesh, 3 float per corner, discards attributes | no | Far proxies and collider sources in an editor tool. API may change. Not runtime | Editor tool | E | LATER |
| `setAllocator` | Route temp allocations | no | Attribute memory under S06 and profiling | App init | R | LATER |
| `setAllocatorCache` (experimental, master only) | Block pool using 64-bit atomics | no | Not in tag v1.3 | none | none | NEVER at runtime |
| `gltf/` gltfpack | CLI optimizer | no | Good reference and test-file generator. Do not ship; use the library in the cook | Tests | none | LATER |
| Gaussian splats (gltfpack only) | `.ply` to glTF | no | No renderer | none | none | NEVER for now |

### Findings in existing LOD code

- **Attribute weights.** `ATTRIBUTE_WEIGHTS = {0.5,0.5,0.5,0.3,0.3}` (`MeshLodPool.cpp:20`) gives UVs
  0.3, while README says "around 10-100" if included [V]. So UVs barely guide collapses [I].
- **Weight misalignment.** With UV but no normals, `attributeCount` is 2 and UVs read the 0.5 slots [I].
- **Mobile vertex range.** LOD indices reference the whole source VB. README warns this penalizes
  mobile GPUs "only capable of transforming a sequential vertex buffer range" and prescribes
  coarsest-first fetch order [V]. A runtime build cannot reorder immutable shared buffers, so the
  fix requires baking [I].
- **Runtime simplification.** LODs run on device, off by default (`SceneSettings.h:354`) [V]. A
  job, but with 0 workers (web single-thread) jobs run from a per-frame pump (`JobSystem.h:24-25`)
  [V], so a large mesh would hitch the frame [I].

---

## 3. Geometry path

### 3.1 Importers

| Source | Path | Dedup | Layout | Index |
|---|---|---|---|---|
| **glTF/GLB** | tinygltf: `MeshSystem.cpp:1286-1291,1638-1640,5140` | none: authored data used as is | Per-accessor bytes used directly; each bufferView one GPU buffer (`:2075-2119`) | As authored; u8 widened to u16 (`:5613-5620`) |
| **OBJ** | tinyobjloader: `MeshSystem.cpp:6514`; build `:6664-6720` | **none: one vertex per corner** (`:6683` pushes the running count) | Interleaved 48 B: pos 3, uv 2, normal 3, color 4 floats (`:6538-6541`, `InterleavedBuffer.cpp:77`) | Sequential; u32 above 65,536 corners (`:6723`) |
| **FBX** | ufbx, `editor/util/GlbBuilder.cpp`; ships as `.glb` (`Exporter.cpp:2827-2844`) | **yes**, binary, `ufbx_generate_indices` (`:933`), including source vertex id | Deinterleaved floats: P3 N3 UV2 [UV2] [color 4]; skin u16x4 + float4 (`:945-976`). **No tangents** | u16 when it fits, else u32 (`:992-996`); **source face order, no vcache** |
| **Unity** | No separate geometry importer: models go through FBX [I, `UnityProject.cpp` has builtin meshes and no geometry code in the hits] | as FBX | as FBX | as FBX |
| **Procedural** | `MeshSystem.cpp:574-578` etc. | n/a | Interleaved, often DYNAMIC | Not in scope |
| **Particles, VAT** | Own tinygltf loads: `ParticleMeshes.cpp:195`, `VatSourceMesh.cpp:460` | | | Must share any future decode step |

All [V] unless marked. Morph targets become attributes (`MeshSystem.cpp:5901-5994`), up to 8 [V].

### 3.2 In-memory format

- `Buffer` holds bytes, a map of `AttributeType` to `Attribute` (type, elements, offset, count,
  normalized), stride, and a render handle (`engine/core/buffer/Buffer.h`, `Attribute.h`) [V].
- `InterleavedBuffer` is float-only (`InterleavedBuffer.cpp:77`) [V].
- Parsed glTF stays in CPU memory as the owner of the bytes; `MeshBufferPool` shares one GPU buffer
  per source across Models (`MeshBufferPool.h:1-27`) [V]. So cuts affect both CPU and GPU memory.
- `AttributeDataType` already includes `BYTE, UNSIGNED_BYTE, SHORT, UNSIGNED_SHORT, HALF, FLOAT`
  (`Render.h:250-259`) [V].

### 3.3 Upload and export

- Upload: `MeshBufferPool::createRender`, immutable buffers [V].
- Export: models are **plain file copies** (`Exporter.cpp:2886`; FBX `:2844`). No mesh cook exists,
  and `S34-player-size.md:36` says content size belongs to "S14 and the cook" [V].
- Web always packs into `content.jpak` (`ExporterPrebuilt.cpp:716`); compiled web builds use
  `--preload-file` (`engine/CMakeLists.txt:354`) [V].
- **JPak does no compression**: an entry is offset, size, FNV (`JPak.h:8-13`) [V]. `Exporter.cpp`,
  `ExporterPrebuilt.cpp` and `docs/shipping.md` have no gzip or brotli step [V]. Whether a web game
  is compressed in transit therefore depends on its host [A]. S34 sizes assume gzip -9 [V].
- So an exported mesh is exactly its source: raw float `.glb`/`.gltf`, or **`.obj` text parsed on
  device** [V].

### 3.4 Shadow pass: can it use a position-only stream?

**It already does for typical glTF and FBX meshes** [V].
- `RenderSystem.cpp:6880-6905` binds only POSITION, TEXCOORD1, COLOR, VAT_INDEX (plus bones and
  morphs when present).
- `SokolObject.cpp:492` skips attributes the depth shader lacks (slot -1), and `depth.vert` declares
  UV and color only under `HAS_TEXTURE` and `HAS_LATTICE` (`depth.vert:46-47,59-63`).
- A deinterleaved POSITION view is its own GPU buffer, so the fetch is positions only.

**Exceptions:** OBJ and procedural meshes (48 B stride) and authored interleaved glTF still fetch
from the full stride [I]. The cook fixes this by always writing deinterleaved.

What is missing is a **separate index buffer**. That needs a second `setIndex` at
`RenderSystem.cpp:6876,6912` and handling in `drawSubmeshGeometry` (`:8415`, which swaps index
buffers for LODs) [I]. Shadow LOD selection already exists (`RenderSystemShadows.cpp:1032`) [V].
Measured value is low on smooth assets (section 2), so apply per mesh.

### 3.5 What quantization demands

**Sokol constraints** [V]:
- Normalized formats read as `vec*` (`sokol_gfx.h:1205-1209`), so `in vec3 a_normal` from BYTE4N
  needs **no shader edit**.
- Non-normalized `SHORT`/`USHORT` map to integer types (`:1211-1214`) and **cannot feed `in vec3`**.
- All formats are multiples of 4 bytes (`:1217`), with no SHORT3 or BYTE2.
- `INT10_N2` is unavailable on D3D11 and WGPU (`:13341,17390`). **Use BYTE4N**, which every backend
  has, to keep identical results; SHORT4N for high precision.

**Engine gaps:**
- `getVertexFormat` has no 3-element integer case (`SokolObject.cpp:213-251`), and the loader passes
  `elements = accessor.type` (`MeshSystem.cpp:5773`). VEC3 SHORT or BYTE gives `INVALID` [V].
  Fix: pad to 4 when the stride allows.
- `UNSIGNED_SHORT` always maps to `...N`, ignoring `normalized` (`SokolObject.cpp:228-233`) [V].
- `INT10_N2`/`UINT10_N2` are not in `AttributeDataType` (`Render.h:250-259`) [V].
- `KHR_mesh_quantization` and `EXT/KHR_meshopt_compression` are not in `supportedExtensions`
  (`MeshSystemGltfPbr.cpp:31-37`), so required ones log an error [V].
- `KHR_texture_transform` is unsupported (its only mention is a test warning) [V]. gltfpack UV output
  relies on it, so stock gltfpack files need `-vtf` or expansion on load [I].

**CPU readers that require FLOAT positions** [V]: AABB (`MeshSystem.cpp:3235-3238` skips others),
LOD (`RenderSystem.cpp`, `findLodAttributes`, also drops non-float normal and UV), picking
(`PickGeometry.cpp:181`). `MeshTriangles.cpp` reads `getVector3` directly, so physics and placement
assume float [I]. `CpuSkin.cpp:80-84` handles 4-element byte and 2/4-element short [V].

| Attribute | Format | Shader edit | CPU impact | Saves | Verdict |
|---|---|---|---|---|---|
| Normal | BYTE4N | none | LOD guidance, smooth normals need decode | 12 to 4 B | SOON |
| Tangent | BYTE4N | none (w is ±1) | as above | 16 to 4 B | SOON |
| UV in [0,1] | USHORT2N | none | LOD guidance | 8 to 4 B | SOON |
| UV outside [0,1] | keep FLOAT2 | n/a | n/a | 0. HALF2 steps about 0.001 near 1.0, too coarse [I] | keep |
| Joints | UBYTE4N | none: normalized path exists (`MeshSystem.cpp:5846-5851`) | none | 8 to 4 B | SOON |
| Weights | UBYTE4N | none: `normAdjustWeight` exists (`:5859-5860`) | none | 16 to 4 B | SOON |
| Morph deltas | keep FLOAT3 | n/a | n/a | 0 | keep |
| Color | UBYTE4N | none: loader accepts it | none | 16 to 4 B | SOON |
| **Position** | SHORT4N + dequant | **yes, see below** | **every reader above** | 12 to 8 B | LATER |

**Position is the expensive one.** gltfpack moves dequantization into the node transform, but that
works only for static meshes [I]. In the engine the decoded value must exist before morph, skinning,
VAT and lattice, all in model space (`depth.vert` `getPosition`, `mesh.vert`). That means a uniform
in `mesh.vert`, `depth.vert`, `gbuffer.vert`, `surface_vs.glsl`, `surface_depth_vs.glsl`, `vat.glsl`,
`effect_paint.vert`; **user-forked shaders would break**, violating "old projects must load" unless
it's opt-in; and a doubled variant count on S32's warm-up list [I]. `a_position` appears in 21 shader
files, some unrelated (UI) [V]. For 4 B per vertex, leave it. Use `quantizeFloat` or the Exp filter,
which keep FLOAT for download savings.

**VRAM effect** (arithmetic from layouts in 3.1):

| Layout | Today | Normals+UV+joints quantized | Change | With position too |
|---|---|---|---|---|
| Static P N UV | 32 B | 20 B | **-37.5%** | 16 B (-50%) |
| Skinned P N UV J W | 56 B | 28 B | **-50%** | 24 B (-57%) |

Quantized attributes create new vertex layouts, so new pipelines. S32's warm-up list and
`SokolPipelineRecord.cpp` need checking when this lands [I]. Goldens will move slightly on glossy
surfaces with 8-bit normals, hence the "High" tier should keep SHORT4N.

### 3.6 Where optimizing goes

1. **`loadOBJ`** (`MeshSystem.cpp`, after `:6720`): remap on final interleaved bytes, keeping triangle
   order so rendering stays identical [I]. No format change; helps editor and game. Fewer vertices
   push more meshes to u16 too [I].
2. **`GlbBuilder.cpp:938`**, right after `vertices.resize(vertexCount)`: every consumer below reads
   `vertices` and `indices`, so optimizing here is local [V]. Bump `IMPORTER_VERSION`
   (`FbxImporter.cpp:35`) so old imports refresh; `FbxImporter.cpp:300` already handles the mismatch [V].
3. **New `Exporter::cookMeshes()`** beside `cookTextures` (`Exporter.cpp:1115`,
   `ExporterPrebuilt.cpp:473`), with skip logic like `cookedTextureSources` (`:2807`) and a cache
   modeled on `TextureCookCache.h` [V].

**Cook hazards** [I]:
- **Blending:** `optimizeVertexCache` reorders triangles, which changes single-draw BLEND results.
  Skip alpha-BLEND primitives.
- **Shared accessors:** primitives can share a vertex set. Remap per shared set and include every
  attribute, morph targets and `VAT_INDEX`.
- **Play vs build:** a cook only at export means triangle order differs from editor Play. Physics
  shapes built from it could differ subtly.
- **Scripts** reading vertices by index see a new order.

### 3.7 Reader for compressed glTF

- `BufferView` keeps `extensions` (`tiny_gltf.h:802`) [V], so one function run after parse can decode
  views into `buffers` before `MeshSystem` takes pointers. Call sites: `MeshSystem.cpp:1286-1291,
  1638-1640,5140`, `ParticleMeshes.cpp:195`, `VatSourceMesh.cpp:460` [V].
- Must update: `supportedExtensions`; `COMPRESSION_EXTENSIONS` (`LatticeBakeGltf.cpp:31-33`);
  **`GltfMaterialExtensionsTest.cpp:258-259`**, which uses `EXT_meshopt_compression` as its example of
  an unsupported extension, so swap the fixture's extension (`unsupported_required.gltf:115,118`) [V].
- Fallback buffers: `-cf` files declare a buffer with no URI (pirate.glb has one [V]), and tinygltf
  errors on empty URIs in the non-binary path (`tiny_gltf.h:4552-4553`) [V]. GLB behaviour is
  unconfirmed [I]. **`demo/pirate.glb` (MIT) makes a good test fixture**: real gltfpack output with
  `KHR_mesh_quantization`, `EXT_meshopt_compression`, the Oct filter and a fallback buffer.
- Bump `contentFormatVersion` (`ExporterPrebuilt.cpp:782`) so older prebuilt players refuse the new
  content [I].

---

## 4. Expected impact

### 4.1 Real codec, matched pair (pirate, 5,010 triangles)

`demo/pirate.glb` records true encoded sizes in its bufferView extensions, so no code was run [V].
Baseline: the same mesh from `pirate.obj` in the engine's shipped layout: float P+N, u16 indices,
source order [V].

| Encoding | Bytes | vs raw | B/triangle |
|---|---|---|---|
| Raw floats (JasperEngine today) | 99,396 | | 19.84 |
| Raw + gzip -9 | 67,391 | -32% | 13.45 |
| Raw + zstd -19 | 60,526 | -39% | 12.08 |
| **Quantized + meshopt codec** | 23,036 | **-77%** | 4.60 |
| Codec + gzip -9 | 18,825 | -81% | 3.76 |
| **Codec + zstd -19** | 18,673 | **-81%** | 3.73 |

| Comparison | Saving |
|---|---|
| Web host gzips anyway: 67,391 to 18,825 | **-72%** |
| zstd on both sides: 60,526 to 18,673 | **-69%** |
| Raw floats over bare codec | 4.3x |
| Extra from zstd over codec | 19% |

Per stream: position 4.07 B/vertex (1.97x), normal 1.95 B/vertex (2.05x, Oct filter), index
1.13 B/triangle (5.31x, within README's 1-1.2).

**Caveats.**
- One small mesh.
- gltfpack 0.14 and bitstream v0; newer v1 should do better [A].
- This includes **lossy** quantization: 14-bit positions and 8-bit normals are gltfpack's defaults
  (`gltfpack.cpp:1304-1306`) [V].
- No UVs or tangents.
- With floats kept through the Exp filter, position cost rises by an unknown amount [A].

Today's OBJ path is worse: text 368,962 B (gzip 106,160, zstd 81,099; this one includes UVs), then
721,440 B of vertices in memory [V].

**Zstd conclusion.** Most of the saving is the codec. On web, transport gzip gets the same result as
zstd (18,825 vs 18,673), so a zstd layer adds nothing if the host compresses [V for numbers, A for the
host]. It matters for hosts that do not compress, and for native packs, since JPak has no compression.
The decoder is already in every game (`Ktx2.cpp:227`, `NOTICE.md:139`) [V], so it costs no code.

### 4.2 Extrapolation to a game [E]

1M triangles of unique geometry, P+N+UV (assumed scene size; ratios from `pirate`):

| | Download |
|---|---|
| Raw floats (26.16 B/triangle) | 26.2 MB |
| Raw + gzip (17.62 B/triangle) | 17.6 MB |
| Codec + zstd, UV assumed 2-3 B/vertex at 0.63 vertices/triangle (5.0-5.6 B/triangle) | **5.0-5.6 MB** |

About **12 MB less**, compared with the whole gzipped web player at 8.20 MiB
(`S34-player-size.md`, M3) [V for the player]. For a 3D game on web, mesh data can outweigh the entire
engine. Validate on a real project before quoting this.

### 4.3 Code size (web)

**Measured from upstream's own wasm in `js/`** (decoded via the `tools/wasmpack.py` format; magic bytes
checked; `-O3`, no vectorize, sizes before wasm-opt) [V]:

| Upstream module | Sources | wasm | gzip -9 | zstd -19 | Goes in |
|---|---|---|---|---|---|
| **Decoder, scalar** | vertexcodec, indexcodec, vertexfilter (decode) | 8,413 | 3,351 | 3,126 | **game** |
| **Decoder, SIMD** | same, `-msimd128` | 12,259 | 3,516 | 3,286 | **game (web)** |
| Encoder | codecs (encode), filters, vcache, vfetch, spatialorder | 14,682 | 6,449 | 6,137 | editor |
| Simplifier | simplifier, remesher, vfetch, indexgenerator | 40,919 | 16,179 | 14,913 | game today, partly |
| Clusterizer | clusterizer, meshletutils | 18,508 | 8,160 | 7,664 | not used |
| Tangents | tangentspace | 16,687 | 8,953 | 8,371 | editor |

**Decoder cost: 12.3 KB of a 28.29 MiB wasm (0.04%); 3.5 KB of 8.20 MiB gzipped (0.04%)** [E: these are
upstream's flags; the engine's link will differ]. Zstd adds nothing since it is already linked [V].

**Today's linkage** [I]: static archive (`libs/meshoptimizer/CMakeLists.txt`), so only referenced objects
come in: `simplifier`, `vcacheoptimizer`, `quantization`, `allocator`. The whole `*.cpp` GLOB compiles but
is not linked. `S34` M4 does not break out meshoptimizer; get the real number with
`tools/players/lib_size.py <player.wasm> --json out.json`, group `lib: meshoptimizer` (`lib_size.py:94`).

**Remaining modules, from source size only** [E, low confidence]; the measured wasm-to-source ratio ranges
0.09-0.6, so these could be off by 2x or more:

| Module | Source bytes | Rough wasm | Note |
|---|---|---|---|
| `indexgenerator.cpp` | 24,600 | few KB for `generateVertexRemap` alone | **Added to games by the OBJ fix**; templates per variant |
| `vfetchoptimizer.cpp` | 1,915 | under 1 KB | Added by the OBJ fix |
| `vcacheoptimizer.cpp` | 14,171 | linked today | |
| `quantization.cpp` | 3,503 | under 1 KB | linked today |
| `allocator.cpp` | 5,538 | under 1 KB | Unused cache should drop out on master [I] |
| `overdrawoptimizer`, `stripifier`, `spatialorder`, `partition`, `rasterizer`, `opacitymap`, `indexanalyzer`, `meshletcodec` | 116,000 total | | Editor or never; **not in games** [I] |

- **SIMD on web:** `JASPERENGINE_WASM_SIMD` defaults ON (`ParticleSources.cmake:32`), and `AnimatorPose.cpp`
  already compiles with `-msimd128` per file (`engine/CMakeLists.txt:2137-2138`) [V]. Do the same for
  `vertexcodec.cpp` and `vertexfilter.cpp`, and let the scalar build use the portable path [I]. Add a
  test that SIMD and scalar filters give the same bits [A: not checked upstream by me].
- **Possible saving:** if LODs are baked and a project does not use runtime LODs, the simplifier could drop
  from players, up to about 16 KB gz. But `PlacementWorld.cpp` is in engine sources (`CMakeLists.txt:1650`),
  so reaching this needs an S34-style switch [I]. LATER.

### 4.4 Other fixtures

Geometry shape [V, script output]:

| Asset | Vertices | Triangles | Corners/vertex | Unique positions/vertices |
|---|---|---|---|---|
| IORTestGrid | 92,835 | 30,945 | **1.00 (soup)** | 0.20 |
| SheenTestGrid | 64,938 | 21,646 | **1.00 (soup)** | 0.18 |
| TransmissionTest | 25,961 | 50,879 | 5.88 | 0.99 |
| MetalRoughSpheres | 7,013 | 12,209 | 5.22 | 0.99 |
| AnisotropyRotationTest | 5,799 | 7,476 | 3.87 | 0.79 |
| occluded_blob | 1,025 | 1,920 | 5.62 | 1.00 |
| sphere | 1,225 | 2,304 | 5.64 | 0.96 |
| robot_apose | 4,788 | 1,440 | 0.90 | 0.47 |

Counts are per accessor and include vertices a primitive does not reference (hence robot_apose's 0.90 corners per
vertex), so treat its 0.47 as indicative only; it is a low-poly procedural fixture.

The test grids also repeat geometry between primitives, so their compression ratios are not meaningful and
are omitted. I also discarded byte totals for `hero_animated` and `robot_apose`: my script double-counted
shared accessors there.

**Proxy, not the codec** (quantize + first-use order + delta + byte planes + zstd) on three non-repeating
assets; it confirms floats compress poorly and the direction of the gain, nothing more [V for the numbers]:

| Asset | Raw | gzip | zstd | Proxy + zstd |
|---|---|---|---|---|
| TransmissionTest | 1,136,026 | 843,549 | 819,845 | 396,897 |
| MetalRoughSpheres | 241,566 | 113,631 | 88,154 | 39,709 |
| occluded_blob | 68,920 | 35,514 | 26,850 | 10,405 |

### 4.5 Why OBJ LODs likely fail today

README: simplification gets "stuck" on meshes with many seams, and "it's critical that identical vertices
are welded" [V]. `weldPositions` (`MeshLodPool.cpp:29`) only equalizes positions. In an unindexed OBJ every
corner is its own vertex, so the simplifier sees seams everywhere, and `count > previousCount * 0.9f` (`:173`)
then ends the chain, leaving `numLevels` 1 [I]. **Not run.** A red test: load `pirate.obj` with LODs enabled,
assert `numLods > 1` after the fix, and confirm it fails before.

---

## 5. Ranked adoption

| # | What | Plug-in | Runs | Benefit | Cost and risk |
|---|---|---|---|---|---|
| 1 | **OBJ weld**: `generateVertexRemap`, remap, `optimizeVertexFetch` | `MeshSystem.cpp` `loadOBJ` ~`:6720` | load | **4.76x fewer vertices, measured**; 79% less VB; likely enables LODs [I] | A few KB of code [E]. Triangle order kept, so goldens should hold [I] |
| 2 | **Import and export cook**: `...RemapMulti`, `optimizeVertexCache`, `optimizeVertexFetchRemap` + analyzers | `GlbBuilder.cpp:938`; new `Exporter::cookMeshes()` | import, export | LOD0 finally optimized; ACMR toward 0.5-1.5, overfetch toward 1.0; prerequisite for #3. No runtime or shader cost | Skip BLEND primitives. Shared accessors. Bump `IMPORTER_VERSION` |
| 3 | **Compression**: encoders in cook, `decodeVertexBuffer`, `decodeIndexBuffer`, `decodeFilter*` in loader | Cook; after each tinygltf parse (section 3.7) | export, load | **-72% against gzipped floats, measured on one mesh**; ~12 MB per 1M triangles [E] | ~3.5 KB gz [E]. Test flip. `contentFormatVersion` bump |
| 4 | **LOD quality, then baking**: `SimplifyErrorClamped`, `SimplifyPermissive`, weight fix; later bake + coarsest-first fetch | `MeshLodPool.cpp:20,160`; cook | load, then export | Less conservative switching; unstuck meshes; no device hitch; better mobile vertex ranges | Flags change LOD output (default off). `ErrorClamped` is experimental |
| 5 | **Attribute quantization**: BYTE4N normals and tangents, USHORT2N UVs, UBYTE4N skin | Cook; `MeshSystem.cpp:5773`; `SokolObject.cpp:213` | export, load | **-37.5% static, -50% skinned VRAM** (arithmetic). No shader edits | LOD guidance needs decode; new pipelines; 8-bit normals alter goldens |
| 6 | **Shadow index buffer**, per mesh | Cook; `RenderSystem.cpp:6876,6912,8415` | export, draw | Only where seams are heavy: 0.47-0.79 unique positions on some assets, none on smooth | Extra IB per qualifying mesh |

**Do not start with position quantization.** It's the only item that needs shader edits and threatens forks,
for 4 B per vertex.

**Order:** tag upgrade + NOTICE fix, then #1, #4 flags, #2, #3, #5, LOD baking, #6.

**User and agent requirements** (CLAUDE.md) [I]: Project Settings rows and a per-model override in `.meta`
(`AssetMeta`) using Unity-like names such as "Optimize Mesh" and "Mesh Compression: Off/Low/Medium/High";
an MCP tool reporting ACMR, overfetch, overdraw and cooked sizes; no Lua API since it is not runtime
behaviour. Add a spec: S34 explicitly leaves mesh size to a cook that does not exist yet. Disabled means
files copied unchanged, as today, meeting the zero-cost rule. Old projects load because the reader is
additive and cooking touches only export output.

---

## 6. Left open

- **Not measured:** engine code sizes (upstream wasm only); actual ACMR and overfetch on engine assets; frame
  hitch from LOD jobs on web; any render output.
- **Not tested:** every "golden should hold" claim, the OBJ-LOD hypothesis (4.5), and `pirate.glb` through
  tinygltf.
- **Only one true-codec data point**, an old encoder. Run a real encode on representative assets on a build
  server before committing to numbers.
- **Unresolved assumptions:** web host compression; MSVC contraction; SIMD-vs-scalar filter equality.
- **Unconfirmed:** the Unity path was read through search hits, not in full; I found no geometry handling
  beyond FBX and builtin meshes.
