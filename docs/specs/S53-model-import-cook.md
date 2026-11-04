# S53: Model import and cook (meshoptimizer)

Wave 5. Research: `docs/forge-meshopt-analysis/` (`meshopt-api.md`, `meshopt-gltf.md`,
`jasper-ux-human.md`, `jasper-ux-agents.md`). Works with S54 (LOD and cross-fade), which consumes
the baked LOD levels this spec produces.

## 1. Goal

Every model a user drops into a project loads correctly, whatever tool produced it, and ships
smaller and faster without the user doing anything. Models get the same import experience textures
got in S14: a Model Import inspector with Unity's names, per-platform overrides, a cook that runs in
the background, measured before-and-after numbers, and an audit that points at the most expensive
meshes. Agents drive and check all of it through MCP with the same settings and numbers.

## 2. Scope

In scope:
- **Loader (runtime and editor):** read `EXT_meshopt_compression` and `KHR_meshopt_compression`,
  `KHR_mesh_quantization`, `KHR_texture_transform`, normalized animation outputs,
  `EXT_mesh_gpu_instancing`, `KHR_texture_basisu`, `MSFT_lod`; actionable failures for anything
  still unreadable.
- **Editor-only decoders:** `KHR_draco_mesh_compression` and `EXT_texture_webp`, transcoded by the
  cook so players never carry those decoders.
- **meshoptimizer** at tag `v1.3`; OBJ vertex deduplication.
- **Model Importer:** settings in the `.meta`, a background cook to a node-preserving glb with the
  vendored library, a content-addressed cache, export integration, Read/Write handling.
- **Editor UX:** Model Import inspector, Project Settings > Mesh, Mesh Optimization Audit window,
  Build Report, failures in the Inspector, Project window badges and the status bar.
- **Agent UX:** a `meshes` MCP tool group, CLI use through `call`, docs and the agent guide digest.

Out of scope:
- Runtime LOD selection, cross-fade, Draw Modes and Stats: S54.
- Draw submission: S55.
- Merging meshes or nodes, and reducing LOD 0: those change hierarchy and identity, so they are
  never a cook step.
- Bundling or linking gltfpack: it renumbers nodes, which scenes key on (`meshopt-gltf.md` §3).

## 3. Current state

- **Unreadable files are silently wrong.**
  - A real gltfpack `-c` glb fails in tinygltf with a `byteLength` error that never names meshopt,
    or loads garbage when a large BIN chunk lets the uri-less fallback buffer parse
    (`engine/libs/tinygltf/tiny_gltf.h:4596-4617`).
  - The existing test passes only because its fixture lists the extension over plain data
    (`engine/tests/core/fixtures/gltf/unsupported_required.gltf`).
  - `KHR_mesh_quantization` positions map to an invalid vertex format.
  - Draco loads as an empty model, Basis and WebP textures draw untextured, instancing draws one
    copy, and `MSFT_lod` draws every level at once.
  - Errors reach only the Console: `MeshSystem::reportGLTFExtensions`, `MeshSystemGltfPbr.cpp:134`.
- **Models have no import settings:** the `.meta` holds identity only. Models ship byte for byte,
  and LOD 0 is never optimised (`GlbBuilder.cpp:938`).
- **OBJ:** `loadOBJ` (`MeshSystem.cpp` ~6720) never deduplicates; `pirate.obj` has 4.76x the
  vertices it needs.
- **meshoptimizer:** the vendored copy is an unpatched master snapshot labelled 1.2. Every function
  the engine calls is byte-identical in `v1.3` (verified by blob hash for `simplifier.cpp`,
  `vcacheoptimizer.cpp`, `quantization.cpp` and `vertexcodec.cpp`).
- **The pattern to follow is S14's texture importer:**
  - `editor/window/TextureImporterView.cpp`, `editor/texture/TextureImportSettings.*`,
    `TextureCookQueue`, `TextureCookCache`, `engine/core/texture/CookedTextures.h`.
  - Its MCP tools in `editor/mcp/tools/AssetTools.cpp`.
- **Two defects in the shared machinery:**
  - `SetAssetMetaCmd` has a single static write listener (`SetAssetMetaCmd.cpp:114-127`), so a
    second queue would silently replace the texture queue's.
  - `TextureImporterView::hasPendingEdits` has no caller, so unapplied edits vanish when the
    selection changes.

## 4. Reference

- **meshoptimizer `v1.3`** (MIT), upstream `https://github.com/zeux/meshoptimizer`: `src/` replaces
  `engine/libs/meshoptimizer/` (keep our `CMakeLists.txt` and `-ffp-contract=off`).
- **Small gltfpack helpers to adapt** from `gltf/`: `simplifyProtect`, `simplifyUvSplit`,
  `reindexTolerance`, `filterBones`, the animation resample, and the quantization bit choices
  (`gltfpack.cpp:1300-1310`).
- **Draco:** decoder only, from `https://github.com/google/draco` (`src/draco/compression/decode*`,
  mesh, attributes, core), vendored under `editor/libs/draco` and linked into the editor only.
- **libwebp:** decoder only (`src/dec`, `src/dsp`, `src/utils`), vendored under `editor/libs/libwebp`,
  editor only.

## 5. Requirements

### Functional: loader

- **R1. Normalise before use.** One `GltfNormalize` step runs between parse and use at every glTF
  parse site, both runtime and editor, and costs nothing for a file without these extensions.
  - Decode `EXT_meshopt_compression` and `KHR_meshopt_compression`: every mode and filter, using
    the vendored decoder.
  - Ignore uri-less fallback buffers, and never let tinygltf read them from the BIN chunk.
  - Widen `KHR_mesh_quantization` attributes to formats the vertex layout accepts, or keep
    normalized integer formats where the layout supports them (BYTE4N, SHORT2N, UBYTE4N,
    USHORT2N), including positions with the node dequantization transform.
  - Bake `KHR_texture_transform` into UVs when one transform covers a texture set, otherwise pass
    it to the material as an existing UV transform.
  - Convert normalized animation outputs to floats.
  - Read the `source` of `KHR_texture_basisu` through the existing KTX2/Basis transcoder.
- **R2. Validate before decoding.** The decoder asserts on strides, so check each bufferView's
  stride, count, byte length and offsets, and cap allocations. A malformed file is an error naming
  the file and the bufferView, never memory corruption. A fuzz-style unit test covers truncated
  and corrupted streams.
- **R3. `EXT_mesh_gpu_instancing`:** a node's instances render as instances through the engine's
  instanced mesh path (`InstancedMeshComponent`), with TRANSLATION, ROTATION and SCALE.
- **R4. `MSFT_lod`:** only the highest-detail node loads as the mesh. The cook folds the authored
  lower levels into that mesh's LOD levels (R12) when their attributes match, using
  `MSFT_screencoverage` as the switch points; otherwise it uses them as generated levels' targets.
- **R5. Messages that name the problem.** A required extension the engine cannot read is a load
  error that names the extension and the fix ("re-export without X" or "the cook transcodes X").
  It shows in the Console, the Inspector (R18), the Project window badge and `inspect_models`, and
  fails export. An optional one is a warning in the same places.
- **R6. Draco and WebP in the editor:** the editor decodes `KHR_draco_mesh_compression` and
  `EXT_texture_webp`. Their files always cook (whatever Mesh Compression says), so the build never
  contains them, and WebP images hand off to the S14 texture cook.
- **R7. OBJ:** OBJ import deduplicates vertices (`meshopt_generateVertexRemapMulti` over position,
  normal and UV) and optimises like any cooked mesh.

### Functional: Model Importer and cook

- **R8. Settings** live in the model's `.meta` under `import.modelImporter` with `version: 1`, next
  to `import.textureImporter`.
  - **Defaults:** the default object, plus `platforms.desktop`, `mobile` and `web` overrides with
    an Override flag, as textures have.
  - Absent means defaults, and unknown keys are preserved.
  - **Settings (Unity names):**

    | Setting | Values | Default |
    |---|---|---|
    | Optimize Mesh | Everything, Polygon Order, Vertex Order, Nothing | Everything |
    | Mesh Compression | Off, Lossless, Low, Medium, High | Lossless |
    | Weld Vertices | on/off | on |
    | Index Format | Auto, 16 bit, 32 bit | Auto |
    | Read/Write | Automatic, On | Automatic |
    | Normals | Import, Calculate | Import |
    | Smoothing Angle | 0-180 | 60 |
    | Generate LODs | Automatic, On, Off | Automatic |
    | LOD Count | 1-3 | 3 |
    | Reduction | Preserve Detail, Balanced, Smallest | Balanced |
    | Ship Source File | on/off | off |

- **R9. Cooking is on for every project, including existing ones.** Owner decision: projects get
  smaller builds without asking. The cook never changes nodes, mesh, primitive, material or
  animation order, or their counts (a test asserts it), so scenes, sub-clip paths and submesh
  overrides keep working without a scene migration. Sources are never modified.
- **R10. What each setting does:**
  - **Optimize Mesh:**
    - Everything: `generateVertexRemapMulti` (exact duplicates when Weld is on), then
      `optimizeVertexCache`, `optimizeOverdraw` (threshold 1.05), `optimizeVertexFetchRemap`, with
      every stream remapped, morph targets included.
    - Polygon Order: cache and overdraw only.
    - Vertex Order: fetch only.
    - Blended submeshes (glTF BLEND, or a blend-mode override in any scene that uses the model)
      keep their triangle order.
  - **Mesh Compression:**
    - Off: no codec, and the source data ships as stored (still optimised).
    - Lossless: `KHR_meshopt_compression` codec at level 2 on unquantized data.
    - Low: positions 16, UV 14, normal and tangent 10 bits (Oct), colour 8.
    - Medium: gltfpack's defaults (14, 12, 8, 8).
    - High: 12, 10, 8, 8, with Exp and Oct filters and codec level 3.

    Lossy levels store normalized formats the loader keeps on the GPU (R1), with no shader change.
    Positions use the Exp filter, so they decode to floats and CPU readers stay unchanged.
  - **Index Format:** Auto picks 16 bit when the vertex count allows. 16 bit on a mesh past 65,535
    vertices is refused with a message.
  - **Normals: Calculate** uses `meshopt_generateNormals`-style area weighting with the Smoothing
    Angle crease (implement it if the library lacks it).
  - **Generate LODs:**
    - Automatic: bake the levels the runtime would build, with the same recipe as `MeshLodPool`
      after S54's fixes, for every mesh S54 says qualifies.
    - On: always.
    - Off: none.

    LOD Count and Reduction pick the targets and the error budget: Preserve Detail at 0.6 per
    level and a tight error, Balanced at 0.5, Smallest at 0.35 with Permissive. The baked levels
    and their errors go into the cooked glb (S54 R3 reads them).
- **R11. Read/Write.**
  - Automatic keeps the CPU copy of mesh data only when something at runtime needs it: Mesh
    Collider, CPU skinning, particle mesh shapes, navigation bake sources, runtime LOD building,
    or a script that reads mesh data (a project-wide scan of its Lua API calls). The Inspector
    shows the reason.
  - With no reader, players release the parsed buffers after upload. The editor always keeps them.
  - On always keeps them. A test proves the release frees the memory and that each reader still
    works when it keeps it.
- **R12. The cook writes a valid glb.**
  - **Where:** `assets/je-cooked/models/index.json` plus `<hash>.glb` in exports, and
    `.jasperengine/cache/models/v1/<k[0:2]>/<k>.glb` with a `.json` record in the editor.
  - **Cache key `k`:** SHA-256 of the source bytes, the resolved settings, the cook version, the
    meshoptimizer version and the platform family.
  - **Writes:** atomic, with an LRU cap and a Clear button, following `TextureCookCache`.
  - **Lookup:** one redirection point (`MeshSystem::getImportedModelPath`) serves every reader,
    including `ParticleMeshes` and `VatSourceMesh`. Paths missing from the index load raw.
- **R13. Cook queue.**
  - A background queue shares the status-bar counter with Cancel, and the Project window badges
    show cooked, cooking or failed.
  - Reimport is available from the context menu and MCP.
  - Saving a model or its `.meta` recooks it (S05 watcher).
  - Play in the editor uses the cooked desktop result once ready, and the source until then.
  - Export cooks every model for the target family, and fails naming the file on a cook error:
    no silent fallback.
- **R14. Two importers, one undo.** `SetAssetMetaCmd` supports any number of write listeners. Apply
  is one undo step whatever the selection. Undo of an Apply restores the meta and recooks.
- **R15. meshoptimizer `v1.3`** replaces the vendored copy, and the version macro and `NOTICE.md`
  row say 1.3. Every existing golden and LOD test stays unchanged.

### Editor UX (human)

- **R16. Model Import inspector**, selected from the Project window.
  - **Preview:** orbit, with Shaded/Wireframe, Source/Cooked and Force LOD.
  - **Tabs:** Model and Animation (today's clip table), plus the platform tabs.
  - **Meshes section:** the R8 settings, with Read/Write showing its reason.
  - **Level of Detail section:**
    - A read-only stacked LOD bar in S54's LOD colours, with "about" screen heights.
    - A table of Level, Triangles, % of source, "Differs by at most" (sampled deviation as % of
      model size) and "Switches below".
  - **Before and after table** for the shown platform: Triangles, Vertices, Mesh Memory, Download
    and Visual Change. Every number is measured from the cook record, or reads "cooking..." or
    "not cooked yet", never an estimate.
  - **Per-platform table:** memory, download, LODs and status.
  - **Warning rows,** each with an action.
  - **Buttons:** Revert and Apply.
  - **Multi-select** shows mixed values as an em dash and totals instead of first-file numbers, and
    lists the largest models as clickable rows. A mixed model and texture selection offers Select
    Models and Select Textures.
- **R17. Unapplied edits prompt** for both importers: "Unapplied import settings for 'X'" with
  Apply, Revert and Cancel, on selection change, window close and Play.
- **R18. Failures in the Inspector.**
  - A model that failed to load or cook shows a red row naming the extension, or error and file.
  - It offers the action: Reimport, Open Console, Show in Explorer, or "Cook anyway" for
    Draco/WebP.
  - The Project window badge marks it.
- **R19. Project Settings > Mesh:** project defaults for new models (the R8 defaults per platform),
  the cache size and Clear, and Ship Source Files by default.
- **R20. Mesh Optimization Audit window** (Window > Analysis > Mesh Optimization Audit).
  - A sortable table of the project's models:

    | Path | Triangles | Vertices | Duplicate % | ACMR | Overdraw | Memory | Download | LODs | Status | Issue |
    |---|---|---|---|---|---|---|---|---|---|---|

  - Filters and a scene scope ("models in the open scene").
  - Each issue offers a numbered one-click fix: turn on LODs in this scene, Mesh Compression
    Medium, Reimport, or open the importer. A fix is an undoable command.
  - It reads glTF headers and cook records and never loads every model at once.
- **R21. Build Report** (after every export, and Window > Analysis > Build Report).
  - Totals by category (models, textures, audio, scripts, engine) and the largest files with
    their cooked and source sizes.
  - It is written as `build-report.json` next to the export.

### Agent UX (MCP, CLI, Lua)

- **R22. Tool group `meshes`** (registered in `editor/mcp/McpToolGroups.cpp`, hidden until
  `tool_groups` enables it, named where agents search per the lean-surface rules on `main`):
  - **`inspect_models`** (read-only):
    - `paths` / `scene` / `all`, `platform`, `sort_by`, `limit`.
    - Optional `settings` for a dry run that returns predicted cooked numbers without writing the
      meta (the cache may fill).
    - Returns the audit columns, settings (resolved and overridden), cook status and records,
      issues with their fixes, and required extensions.
  - **`set_model_import_settings`:**
    - `paths`, `settings` (closed object of the R8 keys), `platform`, `reset`, `wait`.
    - It is one undo step, returns the undo id and the cook job id, and is not allowed while
      playing.
  - **`reimport_models`:** `paths` or all, `platforms`, `wait`. Returns the `inspect_models` record.
  - **`check_render_budgets`** (read-only measurement):
    - `scene`, `platform`.
    - Compares the scene against the project's `quality.render_budgets[platform]`: triangles (main
      view), draw calls, mesh memory and download.
    - Fails (`call` exits 1) with the exact list of offenders. `allowedWhilePlaying`.
  - **`capture_model_lods`** is specified in S54.
  - **Text summaries** fit in about 15 lines. structuredContent carries the data. Failures are
    actionable ("`hero.glb` requires KHR_draco_mesh_compression: the cook transcodes it; run
    `reimport_models`").
- **R23. Output-only additions** (no schema growth): `get_asset_info` and `get_model_info` gain
  required extensions, cooked sizes and cook status.
- **R24. Budget:** the group fits the default group budget. `tools/mcp/budget.json` gets a `meshes`
  entry if needed. The guide digest is one short clause (the guide has very little room). Detail
  goes to `docs/mcp/guide/meshes.md`. Regenerate `docs/mcp/TOOLS.md`.
- **R25. Project settings `quality.render_budgets`:** per platform `{triangles, draw_calls,
  mesh_memory_mb, download_mb}`, empty by default. They are editable in Project Settings > Quality
  and through the existing settings tools.
- **R26. Game-project AGENTS.md** (the template generator) gains a short line on
  `inspect_models`, `check_render_budgets` and cooking.

### Platform and performance

- **R27. Decoder speed:** the decoder runs on every platform. On web, `vertexcodec.cpp` and
  `vertexfilter.cpp` compile with `-msimd128` in SIMD builds, plus the scalar build.
- **R28. Size and speed budgets:**
  - The runtime decoder adds at most 8 KB gzipped to the web player.
  - Cooking a 100k-triangle model takes under 2 s on a build-server core.
  - Loading a cooked model is no slower than the source on desktop: load time measured in a bench
    test.

### Data and compatibility

- **R29. Old projects load and get cooked.** There is no scene format change. The `.meta` gains a
  versioned section. A newer cook version recooks through the cache key.
- **R30. Player capability:** a player records the cook versions it decodes in
  `player-info.json`, and export refuses a mismatched prebuilt player with a message.

## 6. Design constraints and decisions

- **Not a new mesh format.** A cooked glb keeps one reader and external tools working, and the
  format is designed to stay compressible.
- **Node invariant first:** every pass is written against it, and anything that can't honour it is
  refused.
- **One code path for Play and export,** keyed by platform family.
- **Same names and numbers** in the Inspector, MCP and the Build Report.
- **Lossy Mesh Compression is never automatic.** The default is Lossless.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 | `GltfNormalizeTest`: real files generated with gltfpack (`-c`, `-cc`, `-kn`, `-vpf`, `-noq`) and committed small render the same triangles and attributes as their source, within quantization error. Red: the same test on `main` fails. |
| AC2 | `GltfNormalizeTest.Malformed*`: truncated or corrupt streams give an error naming the bufferView, with no crash under ASan. |
| AC3 | Instancing, `MSFT_lod`, Basis, texture-transform, Draco and WebP fixtures each load correctly (`GltfExtensionsTest`). |
| AC4 | `ObjDedupTest`: `pirate.obj`-like input loses its duplicate vertices and renders the same image (golden). |
| AC5 | `ModelCookTest`: node, mesh, primitive, material and animation counts and order are unchanged, for every setting combination. |
| AC6 | `ModelCookTest`: Lossless cook decodes to bit-identical vertex data, Optimize reorder only, and ACMR is ≤ source. |
| AC7 | Lossy levels stay within their documented error (sampled deviation). Golden images at Medium. |
| AC8 | Read/Write release test, plus the reader-keeps tests (R11). |
| AC9 | Two listeners: a texture and a model Apply in one session both recook (`SetAssetMetaCmdTest`); red on `main`. |
| AC10 | MCP: `test_meshes.py` covers each tool, dry run versus applied numbers, undo, the budget check failure exit code, and that the group is hidden until enabled. |
| AC11 | Export of a project with models writes `index.json` and cooked glbs, the player loads them (smoke test), and a cook error fails the export. |
| AC12 | Editor screenshots of the inspector (single, multi), Audit window, Build Report and failure row, from an isolated editor. |
| AC13 | The web build passes and the size gate shows the decoder within R28. |

## 8. Dependencies and conflicts

- **Follow-ups overlap:** S65 (models whole on a game's loading thread) edits the same glTF and OBJ
  load paths; the normalise step sits in the parse, shared by both threads.
- **Doriax catch-up overlap:** `render-correctness` makes meshes without normals draw flat (a
  runtime fallback), and S53's Normals: Calculate is the import setting. Both stand. `mesh-memory`
  reshapes `MeshComponent` and submesh storage. S53's Read/Write release works on whatever storage
  lands, merged second onto first.

- **Depends on** S04 (meta and GUIDs), S05 (watcher), S14 (cook queue and cache patterns) and S30
  (export and player info).
- **Hotspots:** `MeshSystem.cpp` and `MeshSystemGltfPbr.cpp`, shared with S54 (LOD attributes);
  `RenderSystem.cpp` only via the cooked-LOD prefill, which is S54's; `AssetTools.cpp`,
  `McpToolGroups.cpp`, `budget.json` and `ProjectSettingsWindow.cpp`, shared with S54 to S56.

## 9. Risks and defaults

- **Overdraw reorder** may change a blended look if a scene override isn't found. Default: skip
  every submesh that any scene marks blended, and log it.
- **Cook time on huge projects:** the queue is background, and export waits with progress.
- **Lua scanning for Read/Write** may miss a dynamic call. Default: a project setting
  `mesh.keep_cpu_data_for_scripts`, on when any script uses the mesh-reading API.

## Decisions (s41-loader: R1-R7, R15, R27, R28's decoder size)

1. **One step, in place.** `GltfNormalize::run` (`engine/core/subsystem/GltfNormalize.h`) runs right
   after every tinygltf parse: `MeshSystem::loadGLTFFile` (the synchronous load and
   `parseGLTFFile`, so sub-clips too), the background parse (`loadModelFileOnWorker`), particle mesh
   shapes and the Vertex Animation import. It changes the parsed model so the rest of the loader reads
   core glTF: widened streams go into one appended buffer named `GltfNormalize`, widened accessors
   keep their index (their `bufferView` and type change), baked UVs are new accessors. Node, mesh,
   primitive, material and animation counts never change.
2. **tinygltf patch** (marked "JasperEngine patch"): a buffer with `EXT_/KHR_meshopt_compression`
   `"fallback": true` and no uri loads no data (in a .gltf too), and only buffer 0 can take a GLB's BIN
   chunk; another uri-less buffer is a parse error naming it. Before, every uri-less buffer got a copy
   of the BIN chunk's first bytes.
3. **Validation before the decoder** (R2): the extension's rules (stride 4..256 and a multiple of 4
   for attributes, 2 or 4 for indices, whole triangles, filter strides, `count * byteStride ==
   bufferView.byteLength`), the compressed range inside its buffer, views into a placeholder must be
   compressed, at most 512 MB decoded per model (`kMaxDecodedBytes`), and decoded indices below the
   primitive's vertex count. Every refusal names the file and the bufferView (or primitive). An
   authored fallback (gltfpack `-cf`) is used as it is, without decoding. The COLOR filter is accepted
   on `KHR_meshopt_compression` only.
4. **Quantized formats kept on the GPU:** normalized TANGENT and COLOR VEC4 (BYTE4N, UBYTE4N, SHORT4N,
   USHORT4N), normalized TEXCOORD VEC2 of 16 bits (SHORT2N, USHORT2N), and normalized VEC3 BYTE/SHORT
   normals whose stride has room for a fourth component (bound as BYTE4N/SHORT4N, the shaders read xyz).
   Positions are always widened to floats (bounds, picking, physics and LODs read floats), as are
   integer UVs, two-byte UVs, morph deltas, instance transforms and animation outputs. Node-transform
   baking (previews, Merge Static Meshes) now also takes normalized normals and tangents.
5. **KHR_texture_transform** is baked per UV set: one transform moves the set; a second on set 0 moves
   to a new set 1 when no texture or primitive of the material uses set 1; anything more uses the
   first texture's transform with a warning naming the material. The extension's `texCoord` override
   becomes the texture's set.
6. **KHR_texture_basisu:** the extension's image becomes the texture's source where the Basis
   transcoder is built (editor, web players, tests). A native player keeps the fallback `source`, or
   reports an error with `cookFixes` when there is none (the cook transcodes it).
7. **EXT_mesh_gpu_instancing:** an instanced node always gets its own child entity (its node transform
   on it, never baked into vertices) with an `InstancedMeshComponent` holding the instances
   (`maxInstances` = their count). Merge Static Meshes refuses such a model with the reason; a
   flattened preview draws the node once, as its first instance.
8. **MSFT_lod:** the lower levels' nodes (and their subtrees) lose their mesh in the normalised model,
   so only level 0 draws; `GltfNormalizeResult::lodChains` (and `GltfNormalize::readLodChains` on a
   model before normalising) keep the chain with `MSFT_screencoverage` for the cook. `MSFT_lod` on a
   material is ignored (level 0's material draws).
9. **An unread required extension** is an Error diagnostic, and the model still loads as before (its
   data is readable core glTF); data the loader cannot decode (corrupt meshopt, Draco in a game) fails
   the load. The error that fails a load is logged once by the caller ("Can't load GLTF model ...");
   every other finding once per model by `run`.
10. **Draco and WebP** decoders register from the editor's objects at start-up
    (`editor/util/GltfEditorDecoders.cpp`, a static registration), so every editor executable and the
    editor tests have them and the engine library never links them. `GltfNormalizeResult::
    needsEditorDecoders` and an Info diagnostic with `cookFixes` tell the cook to always cook such a file.
11. **OBJ** (R7): binary-equal corners welded (`meshopt_generateVertexRemap` over the 48-byte
    interleaved vertex, so colour too), then per submesh vertex cache and overdraw (1.05) order unless
    the submesh blends, then first-use vertex order over all submeshes.
12. **Particle mesh shapes** of a quantized mesh take the transform of the node that draws it (its
    dequantization); other meshes keep their own space, as before.
13. **The lattice bake** still refuses compressed files: it rewrites the source file.
14. **meshoptimizer v1.3** is the tag's `src/` unchanged (`clusterizer.cpp`, `remesher.cpp` and the
    header differ from the old snapshot; every file the engine calls is identical). On the web,
    `vertexcodec.cpp` and `vertexfilter.cpp` build with `-msimd128` when `JASPERENGINE_WASM_SIMD` is on
    (the default); the OFF build stays scalar.
15. **The decoder's size (R28)**, measured as the release web player compiles it (em++ -O3, the
    three decoder sources alone exporting the seven `meshopt_decode*` functions, the web image's
    Emscripten): 16,014 bytes of wasm and 3,897 bytes gzip -9 with simd128, 8,330 and 3,316 scalar;
    under the 8 KB gzipped budget. The normalise step's own code comes on top and is not part of it.

### The loader's API for s41-importer

- `GltfNormalize::normalize(model, filename, result)` after a tinygltf parse in the cook (no Console,
  no registry); `run` is the same plus both. Read `result.ok`, `result.diagnostics`,
  `result.lodChains` (R4), `result.handledExtensions` and `result.needsEditorDecoders` (R6: always
  cook).
- The normalised model still has the meshopt extension objects on its bufferViews and the decoded
  data in the former placeholder buffers: write fresh buffers and views in a cooked glb, never the
  parse's `extensions` of views and buffers.
- `ModelLoadDiagnostics::get(path)` / `hasErrors(path)` / `models()` / `revision()`: each model's last
  load (`GltfDiagnostic`: severity, extension, part, message, fix, cookFixes), keyed by
  `MeshSystem::getModelFilenameKey`. Export fails on an Error (R5); the Inspector row, the badge and
  `inspect_models` show `text()` or the fields.
- `GltfNormalize::supportedExtensions()` / `isSupportedExtension(name)`: includes `JE_mesh_lods`
  (contract C1), which the loader passes through untouched.

## Deviations (s41-loader)

1. **R1, "otherwise pass it to the material as an existing UV transform":** materials have no UV
   transform, and adding one is a shader change on every backend for a case exporters rarely write.
   The second transform of a UV set is baked into the free second UV set instead (Decision 5); only
   beyond that is the first transform used, with a warning.
2. **R5's Inspector, badge and `inspect_models` parts** are s41-importer's (R18, R22); this stream
   provides and tests the API they read (`ModelLoadDiagnostics`) and the Console part.
3. **AC11 (loading):** a cooked glb is a glb, so players load it through this same step; the export
   and smoke-test parts are s41-importer's.
