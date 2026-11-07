# Terrain

The `terrain` tool group (`tool_groups` `enable: ["terrain"]`): `edit_terrain` writes, `sample_terrain`
measures, `export_terrain_heightmap` writes a heightmap file. `sculpt_terrain` replays brush strokes
and places GameObjects; `set_terrain_maps` assigns or generates whole maps.

## Which tool

| Want | Call |
|---|---|
| A new terrain | `create_entity` `type: "terrain"`, then `edit_terrain` `create` with a preset |
| Hills, flats, noise, paint, holes | `edit_terrain` with `ops` |
| Import a heightmap file (.r16, .raw, .png, .exr) | `edit_terrain` op `import_heightmap`, `dry_run: true` first |
| Numbers: heights, slopes, layer coverage, problems | `sample_terrain` with `stats`, `issues`, `hash` |
| Look: where it is steep, high, painted | `visualize_terrain` with `overlay` |
| Write the heightmap to a file | `export_terrain_heightmap` |
| Place trees or rocks as GameObjects | `sculpt_terrain` `place_object` |
| A game changing the terrain | Lua `Terrain:applyOps` (below) |

## edit_terrain

One call is one undo step and all-or-nothing: a bad field fails naming `ops[i].field` and nothing
changes. `dry_run: true` returns the same result and changes nothing. Values are absolute or signed
amounts, never rates; a seeded op gives the same bytes every time.

Spaces: `space: "world"` (default) takes X and Z in world units, heights as world Y and deltas in world
units. `space: "local"` takes terrain-local units (heights from 0 to Max Height).

Presets for `create` (terrain size m, Max Height m, height map texels): `test_patch` 64, 16, 256;
`mobile` 512, 100, 512; `small` 256, 64, 512; `medium` 1024, 200, 1024; `large` 2048, 400, 2048;
`huge` 4096, 800, 4096. `map_resolution` overrides the texels of new maps.

Every op takes `shape` (default the whole terrain), `filter` and `strength` (0 to 1, default 1):

| Op | Fields | Effect |
|---|---|---|
| `raise`, `lower` | `delta` | adds or subtracts `delta` |
| `set_height` | `height` | moves to `height` |
| `lift_floors`, `flatten_peaks` | `height` | as `set_height`, only below or only above it |
| `smooth`, `sharpen` | `iterations` (1) | toward or away from the local average |
| `add_noise` | `amplitude`, `noise_size` (4), `octaves` (4), `seed` | fractal noise of that amplitude |
| `terrace` | `steps` | flat steps |
| `stamp` | shape `mask`, `delta` | adds the image times `delta` |
| `erode` | `iterations`, `seed` | water then slope erosion |
| `ramp` | shape `polyline` of 2 points, `heights: [a, b]` | a slope from a to b |
| `remap_heights` | `in_range`, `out_range` | maps one height range to another |
| `paint_layer` | `layer`, `normalize` (true) | paints a texture layer |
| `paint_base` | `layer` (optional) | back to the base layer |
| `paint_by_rules` | `rules: [{layer, slope, height, invert, opacity, noise}]`, `seed` | paints by slope and height, later rules over earlier |
| `remap_layers` | `order` (new index of each layer) | reorders layers, keeping the paint |
| `paint_density`, `erase_density` | `layer` | detail density toward 1 or 0 |
| `cut_holes`, `fill_holes` | shape only | holes in the surface and its collider |
| `import_heightmap` | `path`, `format`, `width`, `height`, `byte_order`, `flip_vertically`, `range`, `source_min`, `source_max`; no shape | replaces the height map |

Shapes: `{type: "circle", center: [x, z], radius}`, `{type: "rect", min: [x, z], max: [x, z]}`,
`{type: "polyline", points: [[x, z], ...], radius, closed}`, `{type: "stroke", points: [[x, z, pressure], ...], radius, spacing}`,
`{type: "mask", path, center, size: [w, d], rotation}`, `{type: "all"}`; `falloff` `smooth` (default), `linear` or
`constant`, `feather` 0 to 1. Filters: `{slope: [min, max] degrees, height: [min, max], layer: {index, min, max}, invert, softness}`.

The result: `changed` per map (`rect` in texels, `local` box in the call's space, `texels`), `ops` per
op (`changed_texels`, `clipped_low`, `clipped_high`, `filtered_out`, `outside`, `seed`), `stats`
(heights before and after), `warnings` and `next`. Warnings: `clipped_high` and `clipped_low` (heights hit
0 or Max Height), `filter_excluded_all`, `outside_terrain`, `no_change`, `map_created`, `rotated_terrain`.
Act on `next`.

## Import and export

`import_heightmap` detects a raw file's size from its length, its depth and its byte order; the op's
`detected` entry says what was guessed. An ambiguous file fails listing the candidates: pass `width`,
`height`, `format` or `byte_order`. `range: "auto"` maps the file's own lowest and highest values to
0 and Max Height, `full` the format's whole range, `source` `source_min` to `source_max`.
`export_terrain_heightmap` with `range: "full"` reads back unchanged through `range: "full"`.

## Verify

`sample_terrain` with `stats: ["height", "slope", "layers"]` (optionally a `region`) returns
percentiles, a slope histogram and layer coverage; `issues: true` lists problems with a `fix`
(`height_map_8bit`, `height_clipped_high`, `no_height_map`...); `hash: true` gives a hash per map, which
changes exactly when the map's bytes do.

## Look

`visualize_terrain` turns on a Scene view overlay and returns the view's image with its legend:
`height` (a colour ramp from 0 to Max Height), `slope` (0-15 green, 15-30 yellow, 30-45 orange, over 45
red; `slope_range` shows one range only), `contours` (`contour_interval`, auto by default) and
`layer_weights` (a colour per layer; `layer` shows one). `framing`: `fit` (the default, the current
direction), `top`, `orbit` (`yaw`, `pitch`) or `keep`; `region` frames part of the terrain. The overlay
stays on until `overlay: "none"`; it is never in the game or a camera's image.

## Worked flow

```json
{"tool": "tool_groups", "enable": ["terrain"]}
{"tool": "create_entity", "type": "terrain", "name": "Island"}
{"tool": "edit_terrain", "create": "medium", "ops": [
  {"op": "set_height", "height": 40},
  {"op": "add_noise", "amplitude": 25, "noise_size": 120, "seed": 7},
  {"op": "set_height", "height": 80, "shape": {"type": "circle", "center": [0, 0], "radius": 60, "feather": 0.4}}]}
{"tool": "sample_terrain", "stats": ["height", "slope"], "issues": true, "hash": true}
{"tool": "edit_terrain", "ops": [{"op": "paint_by_rules", "rules": [
  {"layer": 0}, {"layer": 1, "slope": [35, 90]}, {"layer": 2, "height": [70, 200], "noise": 0.2}], "seed": 7}]}
{"tool": "visualize_terrain", "overlay": "slope", "framing": "fit"}
{"tool": "undo"}
```

## Lua

A game edits a terrain whose Terrain component has Allow Runtime Edits on, with the same op tables:
`Terrain(scene, entity):applyOps({{op = "lower", delta = 2, shape = {type = "circle", center = {x, z}, radius = 6}}})`.
Queries (`getHeightAt`, `getNormalAt`, `isHoleAt`, `contains`, `raycast`) work on any terrain.
`edit_terrain` refuses while the scene plays. Details and costs: `docs/scripting/terrain.md`.
