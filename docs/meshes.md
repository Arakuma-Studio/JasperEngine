# Models: import settings, cooks and budgets

How JasperEngine turns model files into what each platform loads (spec
`docs/specs/S53-model-import-cook.md`). Agents: `docs/mcp/guide/meshes.md`.

## The pipeline

1. **Sources** stay what they are in the project: `.gltf`, `.glb`, and `.fbx` (cooked from its
   imported `.glb`). OBJ files load as they are. Scenes keep naming the source path. The cook never
   writes a source.
2. **Import settings** live in the model's sidecar, `<file>.meta`, under `import.modelImporter`
   (`version: 1`). The section is sparse: a model nobody changed has none, and Project Settings >
   Mesh gives its values, per platform.
3. **Cooks**: per platform family (desktop, mobile, web), the editor rewrites the file's geometry
   with meshoptimizer and writes a valid `.glb` into `.jasperengine/cache/models/v1/<k[0:2]>/<k>.glb`
   with a `<k>.json` record of what it measured. `k` hashes the source's bytes, the settings, the
   family, the submeshes scenes draw blended, the cook's version, meshoptimizer's version and the LOD
   recipe, so a rename or a duplicate never cooks again. Writes are atomic; the cache keeps under
   Project Settings > Mesh > Cache Size (least recently used first) and Clear Mesh Cache empties it.
4. **The editor** cooks every model for the desktop in the background (the status bar counts the
   texture and model cooks together, with one Cancel). Edit mode and Play load the cooked desktop
   file once it is ready, and the source until then; a model's badge in the Project window shows
   queued, cooking, cooked or failed. Saving the model or its `.meta` (from the editor, an undo, or
   another program) cooks it again.
5. **Exports** cook each model for the families their platforms load and ship
   `assets/je-cooked/models/<hash>.glb` (the SHA-256 of its bytes: families that cook the same bytes
   share one file) with `index.json`; the sources (and the `.bin` files only they
   read) stay out unless Ship Source File is on. A cook error fails the export naming the file.
   A prebuilt player must decode cooked models: its `player-info.json` lists `modelCookFormats`,
   and an export refuses a player without the format it writes.

## What the cook keeps

Nodes, meshes, primitives, materials, skins, animations and scenes keep their order and count, and
every accessor its index, so scenes, sub-clip paths and submesh overrides need no change. The cook
checks this on its own output and refuses to write a file that breaks it. A file it cannot rewrite
safely (an extension of its own that points at bufferViews) ships as it is, with the reason in the
Inspector.

The cook reads the source as a game would (the loader's normalise step, `docs/model-loading.md`)
and acts on what it finds:

- **Draco and WebP** (`KHR_draco_mesh_compression`, `EXT_texture_webp`) only the editor decodes:
  the cook writes the decoded geometry and a PNG of each WebP image, so a build never carries
  either. A Basis Universal texture (`KHR_texture_basisu`) without a fallback image gets a PNG one
  in the desktop and mobile cooks.
- **`MSFT_lod`**: the lower levels whose attributes match the base mesh's become its baked levels
  (`JE_mesh_lods`, with the file's `MSFT_screencoverage` as each level's `coverage`); the others set
  how many levels the cook generates and how small the last one gets. Generate LODs Off bakes none.
- **What the loader reports** (an extension it cannot read, a stream it cannot decode) shows in the
  Inspector, on the model's badge and in `inspect_models`. An error fails the export naming the
  file; a warning does not.

## Settings (the Model Import inspector)

| Setting | Values | What it does |
|---|---|---|
| Optimize Mesh | Everything, Polygon Order, Vertex Order, Nothing | Everything: exact duplicates welded (Weld Vertices), then the vertex cache and overdraw triangle order, then the fetch order of the vertices, every stream moved together (morph targets too). Polygon Order: the triangle orders only. Vertex Order: the fetch order only. A blended submesh keeps its triangle order, and a primitive never gets an order worse for the vertex cache than its source's. |
| Mesh Compression | Off, Lossless, Low, Medium, High | Off: no codec. Lossless: `KHR_meshopt_compression`, the same values bit for bit. Low, Medium, High: fewer bits (below), then the codec; positions stay floats. |
| Weld Vertices | on, off | With Everything, merges vertices whose every attribute is the same. |
| Index Format | Auto, 16 bit, 32 bit | Auto takes 16 bits when a mesh has 65,535 vertices or fewer; 16 bit on a larger mesh fails the cook with the reason. |
| Read/Write | Automatic, On | Whether anything in a game needs the mesh data on the CPU after upload (below). |
| Normals | Import, Calculate | Calculate: area-weighted normals, creased at the Smoothing Angle (0-180, default 60). |
| Generate LODs | Automatic, On, Off | Automatic bakes the levels the runtime would build (the runtime's recipe); On bakes LOD Count (1-3) levels of the Reduction; Off bakes none. |
| Reduction | Preserve Detail, Balanced, Smallest | Each level keeps 60% (with an error cap of 0.5% of the mesh's size), 50%, or 35% (crossing attribute seams) of the triangles of the one before. |
| Ship Source File | on, off | Exports keep the source next to its cook (code that reads the file itself). |

The platform tabs override any of them for one platform (Override for Mobile ...).

### The lossy levels

| Level | Position (Exp filter) | Texture coordinate | Normal and tangent (octahedral) | Colour |
|---|---|---|---|---|
| Low | 16 bits | 14 bits | 10 bits | 8 bits |
| Medium | 14 | 12 | 8 | 8 |
| High (codec level 3) | 12 | 10 | 8 | 8 |

Bounds of the change, which the cook's tests hold: a position component moves at most
`max|component| * 2^(1 - bits)`; a texture coordinate in [0, 1] at most half a step of its bits; a
normal at most 0.3 degrees at 10 bits and 1 degree at 8. The Inspector's Visual Change and
`inspect_models`' `visual_change` give the largest position change measured, as a share of the
model's size. Normals and tangents in normalized integers need `KHR_mesh_quantization`, which the
cooked file then requires.

### Levels of detail

Baked levels go into the cooked file as `JE_mesh_lods` on each primitive:
`{"recipe": <int>, "levels": [{"indices": <accessor>, "error": <float>}]}` (an authored level also
has `"coverage"`, its `MSFT_screencoverage`), levels in order of
coarseness after LOD 0 (the primitive itself), their indices over the primitive's vertices, `error`
the simplifier's deviation in mesh units. The extension is listed as used, not required: a reader
without it draws LOD 0. A scene draws levels with Mesh LOD on. The Inspector's table shows each
level's triangles, its share of LOD 0, "Differs by at most" (the largest distance measured from the
source surface, as a share of the model's size) and about where it switches (a 1080-pixel view).

## Read/Write

With Automatic, the cook works out whether anything at runtime reads a model's CPU copy: a Mesh
Collider or convex hull built from its mesh, a navigation surface that bakes at runtime, a particle
mesh shape, a skinned mesh (poses and retargeting), runtime LOD building (a scene with Mesh LOD on
and no baked levels), or scripts that call a mesh-reading API (`createMeshCollider`,
`createConvexHullCollider`, a navigation bake: Project Settings > Mesh > Keep CPU Data for Scripts
decides, automatic by default). The Inspector shows the reason; the export writes the decision into
`index.json` (`keepCpuData`). Players do not free the copy yet: that follows the mesh storage work
(spec S53, Deviations), so today this setting changes no memory.

## Project Settings > Mesh and the render budgets

Mesh holds each platform's default for every setting (a model's own value, then its platform
override, win), the cache size, Clear Mesh Cache and Keep CPU Data for Scripts. Quality holds the
render budgets per platform: triangles, draw calls, mesh memory (MB) and download (MB); 0 is no
limit. `check_render_budgets` compares a scene with them: its geometry at LOD 0 with every object
drawn, and the cooked sizes of its models for that platform.

## Mesh Optimization Audit and the Build Report

Window > Analysis > Mesh Optimization Audit lists every model (Path, Triangles, Vertices, Duplicate
%, ACMR, Overdraw, Memory, Download, LODs, Status, Issue) from its header and cook record, with a
filter and a scope (all, problems, the open scene's). Each issue offers a numbered fix (Generate
LODs, Mesh Compression Medium, Reimport, Turn on LODs in this scene, the importer), one undo step.

Every export writes `build-report.json` next to what it builds: totals by category (models,
textures, audio, scripts, scenes, engine, other) and the 25 largest files with their source and its
size. The export's completion page shows it, and Window > Analysis > Build Report opens it.
