# Noise Graphs

A Noise Graph (`.noisegraph`) is the recipe of a generated Terrain: a small readable JSON file of nodes
and edges. The same seed and graph give the same bits on every target.

## The loop

1. `edit_graph` with `create: {kind: "noise", from_preset: "mountains"}` starts a file; `ops` edit it, one undo step per call. `edit_graph` is in group `scripting`; calls reach every tool whatever the listing shows.
2. `preview_noise_graph` evaluates without a terrain: per node `range_analytic`, `range_observed`, `hash`, `ms`, `dead`, `folded`. `ops` tries edits without writing.
3. `generate_terrain` with `dry_run: true` shows statistics and a heightmap image and writes nothing.
4. `generate_terrain` writes, as one undo step named `Generate Terrain (Mountains, seed 7)`.
5. `sample_terrain` reads the result.

A result with an error names the node in `issues` and the fix in `next`.

## Presets

`rolling_hills`, `mountains`, `alpine`, `canyons`, `islands`, `dunes`, `plateaus`, `plains`, and two that read the terrain: `erode_existing`, `paint_by_slope`.

## Nodes

Coordinates are in terrain widths unless `metadata.noise.space` is `world` (metres). A node's seed is the run's seed plus `seed_offset`, and nothing else, so adding a node leaves the others' bits alone. Ports are `field` (one float per point) or `coord` (x, y, z per point) with no conversion. An unconnected `coord` is `position`; an unconnected field input with a same-named parameter uses the parameter.

| Category | Type | Inputs | Parameters | Output |
|---|---|---|---|---|
| Input | `position` | | | `out` coord, `x`, `y`, `z` |
| Input | `terrain_uv` | | | `u`, `v`, 0..1 |
| Input | `constant` | | `value` | field |
| Input | `variable` | | `name` | a graph variable |
| Input | `terrain_height` | | | 0..1, the terrain's heights; bake only |
| Noise | `perlin`, `simplex`, `value` | `coord` | `dimensions`, `frequency`, `seed_offset` | -1..1 |
| Noise | `cellular` | `coord` | those, `metric`, `return`, `jitter` | by `return` |
| Fractal | `fbm` | `coord` | `basis`, `dimensions`, `frequency`, `octaves`, `lacunarity`, `gain`, `seed_offset` | the basis' range |
| Fractal | `ridged` | `coord` | as `fbm` | 0..1 |
| Domain | `domain_warp` | `coord`, `strength` | `basis`, `dimensions`, `frequency`, `amplitude`, `octaves`, `seed_offset` | coord |
| Domain | `domain_transform` | `coord` | `scale`, `offset`, `rotation_y` | coord |
| Math | `scale_offset` | `in` | `scale`, `offset` | |
| Math | `math` | `a`, `b` | `op`, `b` | add, subtract, multiply, divide, min, max, power, modulo, abs, negate, one_minus, sqrt, floor, frac, sin, cos |
| Math | `clamp` | `in` | `min`, `max` | |
| Math | `remap` | `in` | `from_min`, `from_max`, `to_min`, `to_max`, `clamp` | |
| Math | `curve` | `in` | `keys` `[[t, v, in, out], ...]` | |
| Math | `terrace` | `in` | `steps`, `smoothness` | |
| Math | `select` | `a`, `b`, `mask` | `threshold`, `falloff` | `a` below the threshold, `b` above |
| Math | `blend` | `a`, `b`, `weight` | `weight` | |
| Mask | `height_mask` | `in` | `min`, `max`, `falloff` | 0..1 |
| Mask | `slope_mask` | `in` | `min_angle`, `max_angle`, `falloff` (degrees) | 0..1; grid only |
| Stage | `hydraulic_erosion` | `height`, `hardness` | `droplets_per_texel`, `lifetime`, `inertia`, `capacity`, `min_slope`, `erode_rate`, `deposit_rate`, `evaporation`, `gravity`, `radius`, `seed_offset` | `out`, `flow`, `sediment`; grid and bake only |
| Stage | `thermal_erosion` | `height` | `iterations`, `talus_angle`, `rate` | `out`; grid and bake only |
| Output | `output_height` | `in` | `unit` normalized or metres | at most one |
| Output | `output_layer` | `weight` | `layer`, `optional` | one per layer |
| Output | `output_density` | `density` | `foliage_layer`, `optional` | one per foliage layer |
| Output | `output_value` | `in` | `name` | one per name; scripts and voxels |

`list_graph_node_types kind=noise` holds every parameter's default and range.

## A worked example

```json
{"path": "graphs/Hills.noisegraph", "create": {"kind": "noise"}, "ops": [
  {"op": "addNode", "node": {"id": "n1", "type": "fbm", "params": {"frequency": 3.0, "octaves": 5}}},
  {"op": "addNode", "node": {"id": "n2", "type": "remap", "params": {"to_min": 0.1, "to_max": 0.6, "clamp": true}}},
  {"op": "addNode", "node": {"id": "n3", "type": "output_height"}},
  {"op": "connect", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}},
  {"op": "connect", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "in"}}
]}
```

## Generation

- `region` is terrain-local metres, snapped outward to texels and reported as `rect`. `feather` lerps over the old values across its border.
- `resolution`: dry runs at any value; real runs only on a terrain without a height map or a whole-terrain run. A partial run at another resolution is refused.
- A layer weight is clamped to 0..1; a sum above 1 is normalized; layer 0 takes the rest unless it has its own output. A missing layer or foliage layer is an error, or a warning and a skip with `optional`.
- Erosion's `radius` defaults to 2. `radius` 1 is faster and preserves fine detail, and can roughen the ground.
- Erosion on a region warns `erosion-region-seam`: it may not match a whole-map run at the border.
- A graph that reads `terrain_height` fails with `terrain-changed` if the terrain changed during the run, and writes nothing.
