# Meshes

Model import and cook settings, detail levels (LOD), render budgets and vertex colors: the `meshes`
tool group (`tool_groups {enable: ["meshes"]}`). Each spec of the group adds its own section here.

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
| `normals` | import, calculate (with `smoothing_angle`, 0-180), none (faces shade flat) | import |
| `normals_weighting` | area_and_angle, area (calculate) | area_and_angle |
| `smoothness_source` | from_angle, from_smoothing_groups, prefer_smoothing_groups (an FBX's groups; per model) | from_angle |
| `tangents` | import, calculate (MikkTSpace), none | import |
| `rest_pose`, `rest_pose_take` | node_values, first_frame (an FBX imports a clip's first frame; the clip by name, default the first) | node_values |
| `generate_lods` | automatic, on, off | automatic |
| `lod_count`, `lod_reduction` | 1-3; preserve_detail, balanced, smallest (used when `generate_lods` is on) | 3, balanced |
| `ship_source` | true, false | false |
| `material_creation_mode` | import, none (FBX only, every platform: no `platform`) | the project's `mesh.material_creation_mode`, import |

With `platform` (desktop, mobile, web) the settings are that platform's override. `reset: [...]`
puts settings back to their defaults. They live in the model's `.meta` under
`import.modelImporter`; a model nobody changed has no section (the project's Mesh defaults apply:
`set_project_settings` keys `mesh.defaults.<platform>.<setting>`).

`smoothness_source`, `rest_pose` and `rest_pose_take` are read when the FBX is converted: they are
the model's own, never a platform's. A changed one converts the FBX again and reloads the scenes'
models.

### Model GameObjects (S94)

- A Model's `preserveHierarchy` (Unity's Preserve Hierarchy) gives every node of its file a GameObject
  of its own: the mesh is never merged into the Model's. New projects place models with it on
  (`mesh.preserve_hierarchy_for_new_models`); older ones keep the loader's rule.
- `get_model_info`'s `mesh_nodes[].drawn_by` is the GameObject a node's mesh draws on. Set mesh and
  material values there.
- `create_entity` applies `model.animationEntities` and `model.preserveHierarchy` before the first
  load. Writing either later reloads the model in the same undo step.
- `first_frame_differs` lists clips whose root or hips start away from the file's nodes.
- An FBX of a skeleton and takes only (Unity's `Model@Clip.fbx`) imports as a clip source:
  `"<model>.fbx#<take>"`.
- Submesh `i` is the FBX node's material slot `i`. An empty slot keeps its index. Older projects keep
  `mesh.fbx_submesh_order` `used_materials`: the used slots in order.

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

An FBX import logs one Console warning per model that counts what it could not carry over
("Hall.fbx: 3 textures not found (Wall.psd, Floor.png, +1); tiling/offset not imported on 2
textures"); `inspect_models` lists each as an `fbx-import-warning` issue. When the scene's own
materials replace the FBX's, `material_creation_mode: none` imports none: no texture is looked for or
warned about, and every submesh draws with the default material.

### Budgets

Budgets are project settings: `quality.render_budgets.<platform>.triangles`, `.draw_calls`,
`.mesh_memory_mb`, `.download_mb` (0 is no limit). `check_render_budgets` measures the scene's
geometry at LOD 0 with every object drawn, an upper bound of its main view, and the cooked sizes of
its models for that platform. No budget for a platform is an error, never a pass.

### Read/Write

`read_write.keep` says whether something in a game reads a model's CPU copy (a Mesh Collider, a
navigation bake at runtime, a particle mesh shape, a skinned mesh, runtime LOD building, a script
calling `createMeshCollider`) and `reasons` which. With none, a game frees the copy after upload.

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
  `draw_mode: "vertex_color"` draws each mesh's raw vertex colours unlit (white without any),
  `vertex_color_channel` `rgb` (default), `r`, `g`, `b` or `a` one channel as grey.

## Draw submission

- **GPU Instancing** (project setting `graphics.gpu_instancing`, on by default; Project Settings >
  Graphics): opaque Mesh draws of the same mesh, material and shader become one instanced draw,
  in the camera pass, the depth pre-passes, the shadow maps and the reflection probes. The image
  is the same as with it off. Off submits every draw alone.
- A Mesh opts out with `set_properties` `{"mesh": {"allowGpuInstancing": false}}` (Inspector:
  Allow GPU Instancing; Lua `Mesh.allowGpuInstancing`).
- What never instances: skinned, morphing, lattice and Vertex Animation meshes, transparent
  submeshes, extra passes, sprites and tilemaps, terrains, InstancedMeshes, mirrors, forked
  shaders, and meshes of up to 64 vertices with the built-in shader (dynamic batching's).
  A per-object difference (its own colour, lights or reflection probe) splits a batch.
- Shadow maps, point light faces, reflection probes and render-to-texture cameras take their detail
  levels with `quality.shadow_lod_bias` (S56: below 1 coarser) when the scene's Mesh Detail (LOD)
  is on.
- A terrain with `staticShadowCaster: true` keeps its shadow in the static shadow cache while the
  camera moves.

## Checking the cost

`get_render_stats` (the `profiling` tool group) splits the frame:

- `draw_calls_by_pass`: `main` (the cameras' colour passes), `shadow`, `depth` (depth pre-pass,
  G-buffer), `other`; `draw_calls` and `triangles` keep every pass.
- `main_triangles`, `shadow_triangles`.
- `instanced_draws` and `batched_instances`: the instanced draws GPU Instancing made and the draws
  they replaced. 0 with many copies of one mesh: check the setting, the copies' materials and
  `allowGpuInstancing`.
- `visible`, `culled`: mesh draws the passes' visible lists kept and culled.
- `uniform_bytes` and `uniform_bytes_kept` (constant blocks not applied again),
  `buffer_upload_bytes`.

Lua: `scene:getRenderStats()` gives the same counters (camelCase: `mainDrawCalls`,
`shadowTriangles`, `instancedDraws`...).

## Vertex colors

Colors per vertex of one placed Model or Mesh, stored beside the scene (a Vertex Colors component and
its `.vcolors` file); the model file is never changed. Linear RGBA: `[r, g, b(, a)]` 0..1, or
`"#RRGGBB[AA]"` (sRGB, converted).

| Tool | Does |
|---|---|
| `edit_mesh_colors` | runs `ops` on the entity's targets: all-or-nothing, one undo step a call, `dry_run`, `hash`; adds the component on the first edit (`created`) |
| `sample_mesh_colors` | reads: `query` `stats`, `palette`, `points`, `elements` (with `region`, `filter`, `max_elements`), `hash`; `select: true` shows the elements to the user as the editor's selection |
| `bake_mesh_colors` | a job: `output: "model"` writes a `.glb` with COLOR_0, `output: "texture"` a PNG of one target in UV space |

`get_model_info` and `get_entity` list `vertex_colors`: per submesh its target (`index`, `node` and
`primitive`, or `submesh`), `vertex_count`, `has_vertex_colors`, and once painted `split_vertices`
and `stale` (the model changed since: check with `sample_mesh_colors`, repaint).

### Targets and elements

- **Targets** (`targets` of `edit_mesh_colors`, `target` of the others): the entity's mesh
  primitives, by `index` or `{node, primitive}` / `{submesh}`. Default: all.
- **An op's `target`** says which elements change: `{kind: "all"}`, `{kind: "vertices"|"faces"|"corners", ids}`
  (ids from `sample_mesh_colors {query: "elements"}`), `{kind: "selection"}` (the editor's element
  selection, also what `select: true` set), shapes `sphere {center, radius}`, `box {center,
  half_extents, rotation}` (Euler degrees), `capsule {from, to, radius}`, and `color {value, tolerance}`.
- **Vertex** elements are shared by every face around them: the color blends across faces (smooth).
  UV and normal seams do not split them.
- **Faces and corners** are written per face: **hard color edges**. For flat-colored faces (low-poly,
  one color per face, a sharp border) use `{kind: "faces", ids}`, or a shape with `element: "face"`.
  A `vertices` target blends into the neighbouring faces whatever the values.
- `filter` narrows any target: `facing {direction, angle}`, `slope {min, max}` (degrees from up),
  `height {min, max}`, `curvature`, `channel {channel, min, max}`; each takes `invert`.
- Points, shapes and directions are world space; `space: "local"` reads them in the mesh's own space.

### Ops

Every op takes `target` (default all), `filter` and `channels` (a subset of `"rgba"`; default `rgb`
for a 3-value color, `rgba` for 4); every op but `set` takes `strength` 0..1, and every op but
`set`, `channel` and `clear` takes `blend` (`replace`, `add`, `subtract`, `multiply`, `screen`,
`overlay`, `min`, `max`, `lerp`).

| op | Fields |
|---|---|
| `set` | `value`: exactly this value |
| `fill` | `value`, `blend`, `strength` |
| `stroke` | `points` (`[x, y, z]` or `{p, pressure}`), `radius`, `value`, `mode` (paint, erase, smooth, sharpen), `falloff` (smooth, linear, constant, sphere, custom with `falloff_curve` `[[d, w], ...]`), `hardness`, `spacing`, `buildup`, `stencil` |
| `gradient` | `type` (linear, radial: `from`, `to`; height; axis: `axis`), `stops` `[{t, value, strength}]`, `interpolation` (oklab, linear, hsv with `hue_direction`), `extend`, `element` |
| `smooth`, `sharpen` | `iterations` |
| `noise` | `kind` (value, simplex, cellular), `scale`, `octaves`, `seed`, `space`, `low`, `high` |
| `adjust` | one of `levels {in, gamma, out}`, `brightness_contrast`, `hsv {hue, saturation, value}`, `invert: true`, `clamp [low, high]` |
| `channel` | one of `copy {from, to}`, `swap ["r", "g"]`, `invert: "a"`, `pack {r: "g" or 0.5, ...}` |
| `replace_color` | `from`, `to`, `tolerance` |
| `from_texture` | `texture` (a path) or `material_slot` (`albedo`: map × base color; `emissive`, `occlusion`, `metallic_roughness`, a shader texture by name), `uv_set`, `samples`, `keep_seams` (default true: hard edges where the texture has them) |
| `bake_ao`, `bake_thickness` | `rays`, `distance`, `scope` (self, scene), `seed` |
| `bake_curvature` | `radius_edges`, `contrast` |
| `by_direction` | `value`, `direction`, `range` `[low, high]` (cosine) |
| `transfer` | `source_entity`, `max_distance`, `normal_angle`, `unmatched` (keep, nearest) |
| `clear` | `to` (initial, asset, white, black) |

- `strength` is how far one call moves the values toward the op's value. A stroke changes a vertex
  at most `strength` of the way however often it crosses it. **More** (or less) is one call with
  another `strength`, or `fill` with `blend: "add"`; sending the same op again stacks unpredictably.
- `from_texture` reports `mean_delta_e` and `p95_delta_e` (how far the colors are from the texture;
  under 3 is hard to see) and `under_sampled_faces`.
- Errors name the op and field (`ops[1].target.ids: 99 is out of range ...`); nothing is written.

### Channels and materials

What a channel shows depends on the material: the built-in Lit's Vertex Color section,
`pbr.vertexColor` (`set_properties` `submeshes[0].material.pbr.vertexColor.use`, or
`update_material`; every field in `jasper://docs/materials`, "Vertex Color"):

- `use: "tint"` (default): RGB multiplies the albedo, A the opacity. Visible colors go here.
- `use: "masks"`: `r`, `g`, `b`, `a` each drive one input (`occlusion`, `roughness_multiply`,
  `roughness_replace`, `metallic`, `emission`, `opacity`, `albedo_tint`); the colors themselves
  show nowhere.
- `use: "off"`: the shader ignores them (foliage wind or a lattice may still read them).
- Texture layers blended by R, G, B and A (moss, dirt, wet on a rock): the library shader
  `library:vertex_blend` on the submesh's material (`jasper://docs/shaders`).

The result's `warnings` say `material_ignores_channels` when the material does not read a painted
channel; set the Use it names first.

### The loop

1. `edit_mesh_colors {entity, ops, dry_run: true}`: the counts (`changed`, per op `in_target`) say
   whether the target hits what was meant. Nothing is written.
2. The same call without `dry_run`.
3. `sample_mesh_colors {entity, query: "stats"}` (or `palette`, `points`): the numbers.
4. Look: `set_scene_camera {display: {draw_mode: "vertex_color"}}` (`vertex_color_channel` `r`, `g`,
   `b` or `a` for a mask), `frame_entity`, then `screenshot {view: "scene"}`. The vertex color view
   shows the stored values whatever the material does; a shaded screenshot hides masks and channels
   the material does not use, and lighting changes the colors. `draw_mode: "shaded"` puts it back.

Undo (`undo`) restores the values; `hash` before and after proves it.

## Mesh assets (S98 F1)

A `.mesh` holds one mesh's geometry; a Mesh Renderer shows it through `meshAsset`.

| To | Call |
|---|---|
| save a GameObject's geometry as a reusable asset | `set_mesh_geometry {entity, save_as: "assets/meshes/rock.mesh"}` (with `shape`: the new primitive) |
| show an existing `.mesh` | `set_properties {entity, values: {mesh: {meshAsset: "assets/meshes/rock.mesh"}}}` |
| read a `.mesh` (vertices, submeshes, bounds, who shows it) | `get_asset_info {path}` |

- A scene save moves geometry over 4 KB (spheres, cylinders, capsules, tori, custom meshes) into
  `<scene folder>/<scene name>/<GameObject>.mesh`: the scene file then holds `meshAsset`, not
  `buffer`. Read the geometry with `get_asset_info`, never from the YAML.
- `set_mesh_geometry` without `save_as` makes the geometry the mesh's own again.
