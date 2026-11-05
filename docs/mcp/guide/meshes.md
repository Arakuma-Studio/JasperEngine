# Meshes

Model import and cook settings, detail levels (LOD) and render budgets: the `meshes` tool group
(`tool_groups {enable: ["meshes"]}`). Each spec of the group adds its own section here.

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
