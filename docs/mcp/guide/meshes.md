# Meshes

Model import and cook settings, detail levels (LOD) and render budgets: the `meshes` tool group
(`tool_groups {enable: ["meshes"]}`). Each spec of the group adds its own section here.

## Model import and cooks (S53)

| Tool | Answers |
|---|---|
| `inspect_models` | each model's triangles, vertices, memory, download, LODs, cook status and issues, from its glTF header and cook record (no model is loaded); with `paths` and `settings`, a dry run |
| `set_model_import_settings` | a partial update of models' import settings: one undo step, then a cook job |
| `reimport_models` | cooks models again and returns `inspect_models`' records |
| `check_render_budgets` | does a scene fit a platform's `quality.render_budgets`; which models do not |

### The loop

1. `inspect_models {scene: "Main", platform: "mobile", sort_by: "triangles"}`. Each issue has a `fix`:
   the exact call that applies it.
2. `inspect_models {paths: ["models/rock.glb"], platform: "mobile", settings: {mesh_compression: "medium"}}`:
   a dry run. Nothing is written; the numbers (`predicted`) are the real cook's, cached, so applying
   the same settings afterwards is a cache hit.
3. `set_model_import_settings` with the same `paths` and `settings`. `undo {scope: "project"}`
   reverts it. With `wait: false` keep the `job_id` and poll `get_job`.
4. `check_render_budgets {scene: "Main", platform: "mobile"}`. Over budget is an error (exit 1 from
   `jasperengine-editor call`) listing each metric over and the largest models.

### Settings

| Key | Values | Default |
|---|---|---|
| `optimize_mesh` | everything, polygon_order, vertex_order, nothing | everything |
| `mesh_compression` | off, lossless, low, medium, high | lossless |
| `weld_vertices` | true, false | true |
| `index_format` | auto, u16, u32 | auto |
| `read_write` | automatic, on | automatic |
| `normals` | import, calculate (with `smoothing_angle`, 0-180) | import |
| `generate_lods` | automatic, on, off | automatic |
| `lod_count`, `lod_reduction` | 1-3; preserve_detail, balanced, smallest (used when `generate_lods` is on) | 3, balanced |
| `ship_source` | true, false | false |

With `platform` (desktop, mobile, web) the settings are that platform's override. `reset: [...]`
puts settings back to their defaults. They live in the model's `.meta` under
`import.modelImporter`; a model nobody changed has no section (the project's Mesh defaults apply:
`set_project_settings` keys `mesh.defaults.<platform>.<setting>`).

### What changes the image

- `lossless`, the Optimize Mesh orders and Weld Vertices keep every value bit for bit: the same image.
- `low`, `medium` and `high` keep fewer bits; `visual_change` in a record says how far positions
  moved, as a share of the model's size. Dry run them before applying.
- `generate_lods` bakes levels; a scene draws them only with `mesh_lod_enabled` (`set_scene_settings`).
  `lods[].deviation_percent` is the sampled distance a level differs by.
- Blended submeshes keep their triangle order; `kept_order` lists them.

### What the loader finds

`inspect_models` with `paths` returns `load_diagnostics`, what a game's loader finds in the source,
and issues from them: `gltf-required-extension` (an extension the engine cannot read: the model's
status is `failed` and exports stop until it is exported again without it), `gltf-load-warning`, and
`gltf-cook-transcodes` (Draco, WebP: the cook removes them, nothing to do). A record's
`transcoded` lists what the cook decoded; `authored_lod_primitives` counts the primitives whose
levels are the file's own `MSFT_lod`.

### Budgets

Budgets are project settings: `quality.render_budgets.<platform>.triangles`, `.draw_calls`,
`.mesh_memory_mb`, `.download_mb` (0 is no limit). `check_render_budgets` measures the scene's
geometry at LOD 0 with every object drawn, an upper bound of its main view, and the cooked sizes of
its models for that platform. No budget for a platform is an error, never a pass.

### Read/Write

`read_write.keep` says whether something in a game reads a model's CPU copy (a Mesh Collider, a
navigation bake at runtime, a particle mesh shape, a skinned mesh, runtime LOD building, a script
calling `createMeshCollider`) and `reasons` which. Exports record it; players do not free the copy
yet, so it changes no memory today.

## LOD

A mesh draws a coarser level of itself (LOD 1-3, built from LOD 0 or baked by the model cook)
once its geometric error projects below the scene's `mesh_lod_threshold` pixels. Level changes
cross-fade with a dither for `mesh_lod_cross_fade_duration` seconds (0.5) when
`mesh_lod_cross_fade` is on. User docs: `docs/lod.md` in the engine repository.

- **Check the levels of a model:** `capture_model_lods {path: "models/rock.glb"}` builds the
  levels as the runtime does (or reads the levels the cook baked) and returns one tile per level,
  LOD 0 first, with per level `triangles`, `error` (mesh units), `deviation` (model or world
  units), `switches_below_px` (the projected height under which the level is drawn) and `psnr_db`
  against LOD 0 (null: the same image). A model file is drawn on the CPU (Lambert and base
  colour, no textures); `units`, `threshold_px` and `view_height_px` say what the numbers assume.
- **Check them in a scene:** `frame_entity` first, then `capture_model_lods {entity}`: the real
  renderer draws the Scene view once per level, the level forced, and gives the mesh back its
  automatic level. `levels: [0, 2]` limits the tiles; `save_path` also writes the PNG.
- A result's `next` says why a mesh has no levels: Mesh LOD off in the scene
  (`set_scene_settings {settings: {mesh_lod_enabled: true}}`), the mesh's `lodEnabled` off, or a
  mesh that gets none (too few triangles, a 2D mesh, a fork or library shader).
- **What a frame drew:** `get_render_stats` returns `vertices` and `lod: {lod_histogram (meshes
  per level), fading, culled_by_size, fade_draws}` for the view.
- **Per mesh** (`set_properties` on `mesh`): `lodBias` (above 1 keeps detail longer),
  `lodFadeMode` (`none`, `cross_fade`), `lodAnimateCrossFading` (off: fade over the last
  `lodFadeTransitionWidth` of each level's distance band; Instanced Meshes always do),
  `lodCullBelow` (Cull Below Screen Height, a share of the view's height; 0 never culls).
- **Project-wide** (`set_project_settings`): `quality.lod_bias` multiplies every mesh's bias,
  `quality.shadow_lod_bias` the shadow maps' (they cast about one level coarser at 1).
- Lua: `mesh:forceLod(level)` (-1: automatic again), `mesh.currentLod`, `scene.crossFade`.
- **See the levels in the Scene view:** `set_scene_camera {display: {draw_mode: "lod_level"}}`
  tints each mesh by its level (`wireframe`, `shaded_wireframe`, `overdraw` and `shaded` too;
  `show_stats: true` adds the Stats box), then `screenshot`. Editor only: games draw Shaded.
