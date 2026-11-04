# Terrain: foliage

Tools: `edit_terrain` writes, `sample_terrain` reads, `visualize_terrain` looks. Group `terrain`. Tools
and ops are all-or-nothing, seeded and one undo step per call; `dry_run` reports what a real run does.

## Details and trees

**Details** (`foliageLayers`) are grass and flowers: a mesh and a density map. **Trees** are stored one
by one in the Terrain's tree data, with LODs, impostors, capsules and a stable id. Add a prototype with
`set_properties` on `treePrototypes` (`{id, meshPath}`), and a layer on `foliageLayers`.

Terrain Settings, by `set_properties`: `detailDistance`, `detailDensity`, `treeDistance`,
`billboardStart`, `fadeLength`, `maxMeshTrees`, `windDirection`, `windSpeed`, `windSize`, `windBending`.
Project settings: `quality.foliage_quality_desktop|mobile|web` (scales and caps) and
`quality.foliage_distance_desktop|mobile|web` (the one distance multiplier).

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

## Reading

`sample_terrain {foliage: true, region?: {min: [x, z], max: [x, z]}}` returns `foliage`: `details` per layer
(`instances`, exact up to 4,096 cells, else `estimated: true`; `mean_density`; `drawn`), `trees` per
prototype (`count`, `impostor_state`: `none`, `baked`, `out_of_date`) and `totals` (resident, visible, cells,
draws, stream and buffer bytes, CPU and GPU bytes, `main_thread_ms`, `pending_cells`, `budget_exceeded`,
placement `hash`). It works in Play, so runtime removals show.

## Looking

`visualize_terrain {overlay: "density", layer: i}` or `{overlay: "tree_density"}` draws a heat map and
returns a PNG with its legend; `none` hides it. `foliage_quality` sets a tier for this capture only. To
check an impostor: set `billboardStart` near the camera with `set_properties`, frame, and capture.
