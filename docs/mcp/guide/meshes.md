# Meshes

For agents: find the models that cost most, try an import change, apply it, and hold a scene to a
budget. The user reference is `docs/meshes.md`.

The tools are the `meshes` group: call `tool_groups {"enable": ["meshes"]}` first.

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
