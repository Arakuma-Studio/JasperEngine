# Terrain

Served as `jasper://docs/terrain`. The terrain tools are in the `terrain` group:
`tool_groups {"enable": ["terrain"]}` lists them (`sculpt_terrain`, `set_terrain_maps`,
`edit_terrain_details`, `edit_terrain`, `sample_terrain`, `visualize_terrain`). Create a terrain with
`create_entity` type `terrain`.

## The Terrain component

| Unity | Engine | Edited with |
|---|---|---|
| Terrain size, height | `terrainSize`, `maxHeight` | `set_properties` |
| Heightmap | `heightMap` (16-bit image, 0-1 of `maxHeight`) | `set_terrain_maps heightmap`, `sculpt_terrain` |
| Terrain Layers (Paint Texture) | `surfaceLayers` (up to 9) | `set_terrain_maps layers` |
| Splat / control maps | `blendMaps` (up to 3) | `set_terrain_maps blendmap`, `sculpt_terrain paint_layer` |
| Details (Paint Details: grass, rocks, flowers) | `foliageLayers` | `edit_terrain_details`, `sculpt_terrain paint_density`, `edit_terrain scatter_by_rules` |
| Trees (Paint Trees) | `treePrototypes`, the `.jetrees` file in `treeData` | `set_properties`, `edit_terrain` |
| Terrain Settings: Detail Distance, Detail Density, Tree Distance, Billboard Start, Fade Length, Max Mesh Trees, Wind | `detailDistance`, `detailDensity`, `treeDistance`, `billboardStart`, `fadeLength`, `maxMeshTrees`, `windDirection`, `windSpeed`, `windSize`, `windBending` | `set_properties` |

Maps are images under the assets folder; generated ones go to the project's terrain maps folder.
`sample_terrain` reads everything back: size, maps, layers, details (with the instances drawn
now) and heights at points or on a grid.

## Heights

`set_terrain_maps heightmap` takes an image path or `{generate}`: `middle` (flat at 0.5, room to
dig and raise), `flat` at `base_height`, `random_noise`, `fractal_noise` (`amplitude`,
`frequency`, `octaves`, `seed`). `resolution` defaults to the Terrain window's setting.
`sculpt_terrain` runs brush strokes as a person drags them: `raise`, `lower`, `smooth`,
`flatten`, `terrace`, `noise`, `erode`, `ramp`, `stamp` (with a mask). Heights are 0-1 of
`max_height`; check the result with `sample_terrain points`.

## Terrain Layers and blend maps

Blend map slot `i` (`blendMaps[i]`) weights surface layers `3i`, `3i+1` and `3i+2` through its red,
green and blue channels: slot 0 for layers 0-2, slot 1 for 3-5, slot 2 for 6-8.

```json
{"blendmap": {"color": [1, 0, 0], "index": 1},
 "layers": [{"index": 3, "color": "assets/terrain/snow.png", "uv_scale": [4, 4], "roughness_factor": 0.6}]}
```

- `blendmap`: an image path (slot 0), or `{path, index}`, or `{color, index, resolution}` (a constant
  map). A slot needs the one before it (`index` 2 needs slots 0 and 1).
- `layers[]`: `index` and any of the textures `color`, `normal`, `roughness`, `metallic`,
  `occlusion`, `height` (blending only); `pbr`; the factors `uv_scale` ([x, y], 0.01-100, times the
  detail tiling), `uv_offset`, `tint` (linear [r, g, b] or [r, g, b, a], 0-1), `normal_strength`
  (0-2), `roughness_factor`, `metallic_factor`, `occlusion_strength` (0-1); `stochastic_tiling` and
  its `cell_size`, `blend_sharpness`, `contrast`, `rotation`; `hex_tiling`, `hex_rotation`.
- Paint a layer where it belongs with `sculpt_terrain` mode `paint_layer` (`layer`, `normalize`).

## Details

A detail layer scatters instances of one model (`.obj`, `.gltf`, `.glb` or `.fbx`) over the terrain where
its density map is painted, filtered by slope and height, with a random scale and yaw. They are
placed in cells around the camera and drawn instanced, a few draws a layer.

```json
{"add": [{"mesh": "assets/models/grass.glb", "density": 4, "density_map": {"fill": 1},
          "max_slope": 30, "min_scale": 0.8, "max_scale": 1.3, "cast_shadows": false}]}
```

`edit_terrain_details` takes `remove` (indices before the call), then `update` (`{index, ...}`,
indices after the removals), then `add`, all in one undo step; with none of them it lists the
layers. Every value is checked first: one bad value changes nothing.

| Field | Range | Meaning |
|---|---|---|
| `mesh` | an `.obj`, `.gltf`, `.glb` or `.fbx` under the assets folder | required to add |
| `density_map` | an image path, `""` (none), or `{fill: 0-1, resolution: 2-4096}` | a layer without one draws nothing |
| `density` | 0-128 | instances per square unit where the map is full; the engine places at most 8192 per 8 m cell |
| `min_scale`, `max_scale` | 0.01-20, min ≤ max | random scale per instance |
| `rotation_jitter` | 0-1 | share of a full turn of random yaw |
| `align_to_normal` | 0-1 | 0 upright, 1 along the ground |
| `min_slope`, `max_slope` | 0-90 degrees, min ≤ max | where it grows |
| `min_height`, `max_height` | 0-1 of `max_height`, min ≤ max | where it grows |
| `draw_distance` | 1-500 | world units from the camera |
| `min_draw_distance` | 0-500, at most `draw_distance` | where instances start to show (a nearer copy of the layer, with a finer mesh, ends there) |
| `min_height_ratio`, `max_height_ratio` | 0.01-1.99, min ≤ max | height over the width scale |
| `noise_spread` | 0-10 | noise cells per unit between the dry and healthy colours; 0 per instance |
| `wind_bend` | 0-1 | how far the wind bends the tips; 0 draws it still |
| `density_falloff_start`, `far_density` | 0-1 | density falls from that share of the draw distance to `far_density` |
| `density_scale` | 0-1 | the layer's multiplier of the Terrain's `detailDensity` |
| `seed` | 0-4294967295 | another seed reshuffles the positions |
| `cast_shadows` | bool | off for grass-scale scatter: it skips the shadow cascades |

`set_properties` on `foliageLayers` refuses the same invalid values; a scene file with values out of
range loads with them clamped, and the Console says which. Paint density where it should grow with
`sculpt_terrain` mode `paint_density` (`layer` = the detail layer), erase with `erase_density`.

Check: `sample_terrain` lists `details[i].drawn`, the instances the editor holds for that layer now
(near the Scene view camera; 0 without a density map or mesh). Budget for mobile and web: a few
instances per square unit, `cast_shadows: false` for grass, a short `draw_distance`.

## Trees

**Trees** are stored one by one in the Terrain's tree data, with LODs, impostors, capsules and a stable id.
Add a prototype with `set_properties` on `treePrototypes` (`{id, meshPath}`). Project settings:
`quality.foliage_quality_desktop|mobile|web` (scales and caps) and `quality.foliage_distance_desktop|mobile|web`
(the one distance multiplier).

## Ops of edit_terrain

An op's own keys are flat in its object. `shape` is `{type: "all"}` (default), `circle {center, radius}`,
`rect {min, max}` or `polyline {points, radius}`, in the call's `space`. Results give `instances_added`,
`instances_removed`, `rejected` by rule (slope, height, layer, noise, hole, spline, spacing),
`changed_rects`, and `hash`, the same for the same input on every platform.

```json
{"tool": "edit_terrain", "dry_run": true, "ops": [{"op": "scatter_by_rules",
  "shape": {"type": "rect", "min": [-200, -200], "max": [200, 200]},
  "seed": 42,
  "target": {"trees": [{"prototype": "Pine", "weight": 3}, {"prototype": "Birch", "weight": 1}]},
  "density": 0.02, "mode": "replace",
  "rules": {"slope": [0, 30], "height": [10, 140],
            "layer": [{"index": 1, "min": 0.5, "max": 1}],
            "density_map": {"layer": 2},
            "exclude": {"holes": true, "splines": ["Road"], "spline_margin": 4},
            "min_spacing": 3},
  "jitter": {"scale": [0.8, 1.3], "height_ratio": [0.9, 1.2], "yaw": 1, "align_to_normal": 0}}]}
```

- `scatter_by_rules`: `target` is `{trees: [...]}`, or `{detail_layer: i}`, which fills that layer's density
  map (`density` in instances per square unit, absolute; it creates no layer). `count` asks for a number
  of attempts instead of `density`. `mode`: `replace`, `add`, `remove`. Run the `dry_run` first: its
  counts are what the real run gives.
- `place_trees {prototype, points: [[x, z, yaw?, scale?]]}` returns `ids`.
- `remove_trees {ids?, prototype?}` over the op's `shape`.
- `clear_foliage {target, spline?, margin?}`: `target` is `trees`, `details`, `all`, `{trees: [...]}` or `{detail_layer: i}`.
- `convert_placed_to_trees {host}`: an instanced child of the Terrain becomes trees.
- `bake_impostors {prototypes?, views?, resolution?}`: alone in its call. A job over frames: `wait`,
  `timeout_seconds`, `get_job`. Each prototype gets atlases, bounds and a source hash. Refused in Play.

`noise_mask` needs Noise Graphs, and is refused with the reason until a build has them; `density_map` is
the mask that always works.


## Reading foliage

`sample_terrain {foliage: true, region?: {min: [x, z], max: [x, z]}}` returns `foliage`: `details` per layer
(`instances`, exact up to 4,096 cells, else `estimated: true`; `mean_density`; `drawn`), `trees` per
prototype (`count`, `impostor_state`: `none`, `baked`, `out_of_date`) and `totals` (resident, visible, cells,
draws, stream and buffer bytes, CPU and GPU bytes, `main_thread_ms`, `pending_cells`, `budget_exceeded`,
placement `hash`). It works in Play, so runtime removals show.


## Looking

`visualize_terrain {overlay: "density", layer: i}` or `{overlay: "tree_density"}` draws a heat map and
returns a PNG with its legend; `none` hides it. `foliage_quality` sets a tier for this capture only. To
check an impostor: set `billboardStart` near the camera with `set_properties`, frame, and capture.

## The Terrain window

Window > Terrain (or Edit Terrain in the Inspector) has the same brushes for a person. Its brush,
paint and placement settings and the default map resolutions are saved per project and user in
`.jasperengine/user/workspace.yaml`; `set_terrain_maps` and `sculpt_terrain` use those
resolutions when a call gives none.

## Common mistakes

- Adding a detail layer without a density map and expecting grass: use `density_map {fill: 1}`.
- Expecting `blendmap` to set slot 1 by default: a plain path is slot 0; pass `index`.
- Densities in the hundreds: the cell cap thins them and the frame rate drops; use 1-10.
