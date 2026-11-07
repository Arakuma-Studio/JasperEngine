# Terrain

Served as `jasper://docs/terrain`. The terrain tools are in the `terrain` group:
`tool_groups {"enable": ["terrain"]}` lists them (`edit_terrain`, `sample_terrain`, `visualize_terrain`,
`export_terrain_heightmap`, `edit_terrain_details`, `sculpt_terrain`, `set_terrain_maps`). Create a terrain with
`create_entity` type `terrain`, then `edit_terrain` `create` with a preset.

## Which tool

| Want | Call |
|---|---|
| Hills, flats, noise, paint, holes | `edit_terrain` with `ops` |
| Import a heightmap file (.r16, .raw, .png, .exr) | `edit_terrain` op `import_heightmap`, `dry_run: true` first |
| Numbers: heights, slopes, layer coverage, problems | `sample_terrain` with `stats`, `issues`, `hash` |
| Look: where it is steep, high, painted | `visualize_terrain` with `overlay` |
| Write the heightmap to a file | `export_terrain_heightmap` |
| Grass, rocks, flowers scattered by density | `edit_terrain_details`, then `edit_terrain` `paint_density` |
| Place trees or rocks as GameObjects | `sculpt_terrain` `place_object` |
| A game changing the terrain | Lua `Terrain:applyOps` (below) |

## The Terrain component

| Unity | Engine | Edited with |
|---|---|---|
| Terrain size, height | `terrainSize`, `maxHeight` | `set_properties` |
| Heightmap | `heightMap` (16-bit image, 0-1 of `maxHeight`) | `edit_terrain`, `set_terrain_maps heightmap` |
| Terrain Layers (Paint Texture) | `surfaceLayers` (up to 12) | `set_terrain_maps layers` |
| Splat / control maps | `blendMaps` (up to 3) | `set_terrain_maps blendmap`, `sculpt_terrain paint_layer` |
| Details (Paint Details: grass, rocks, flowers) | `foliageLayers` | `edit_terrain_details`, `sculpt_terrain paint_density` |

Maps are images under the assets folder; generated ones go to the project's terrain maps folder.
`sample_terrain` reads everything back: size, maps, layers, details (with the instances drawn
now) and heights at points or on a grid.

## Heights

`set_terrain_maps heightmap` takes an image path or `{generate}`: `middle` (flat at 0.5, room to
dig and raise), `flat` at `base_height`, `random_noise`, `fractal_noise` (`amplitude`,
`frequency`, `octaves`, `seed`). `resolution` defaults to the Terrain window's setting.
`sculpt_terrain` runs brush strokes as a person drags them: `raise`, `lower`, `smooth`,
`flatten`, `terrace`, `noise`, `erode`, `ramp`, `stamp` (with a mask). Heights are 0-1 of
`max_height`; check the result with `sample_terrain points`. A height to reach or an amount to add is
`edit_terrain` (below), in metres and in one call.

## Terrain Layers and blend maps

Blend map slot `i` (`blendMaps[i]`) weights surface layers `3i`, `3i+1` and `3i+2` through its red,
green and blue channels: slot 0 for layers 0-2, slot 1 for 3-5, slot 2 for 6-8. Layers 9-11 weigh by
the inverted alpha of slots 0-2.

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
drawn instanced in chunks around the camera.

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
| `density` | 0-128 | instances per square unit where the map is full; the engine draws at most 8192 per 8 m cell |
| `min_scale`, `max_scale` | 0.01-20, min ≤ max | random scale per instance |
| `rotation_jitter` | 0-1 | share of a full turn of random yaw |
| `align_to_normal` | 0-1 | 0 upright, 1 along the ground |
| `min_slope`, `max_slope` | 0-90 degrees, min ≤ max | where it grows |
| `min_height`, `max_height` | 0-1 of `max_height`, min ≤ max | where it grows |
| `draw_distance` | 1-500 | world units from the camera |
| `min_draw_distance` | 0-500, at most `draw_distance` | where instances start to show (a nearer copy of the layer, with a finer mesh, ends there) |
| `seed` | 0-4294967295 | another seed reshuffles the positions |
| `cast_shadows` | bool | off for grass-scale scatter: it skips the shadow cascades |

`set_properties` on `foliageLayers` refuses the same invalid values; a scene file with values out of
range loads with them clamped, and the Console says which. Paint density where it should grow with
`sculpt_terrain` mode `paint_density` (`layer` = the detail layer), erase with `erase_density`.

Check: `sample_terrain` lists `details[i].drawn`, the instances the editor holds for that layer now
(near the Scene view camera; 0 without a density map or mesh). Budget for mobile and web: a few
instances per square unit, `cast_shadows: false` for grass, a short `draw_distance`.

## The Terrain window

Window > Terrain (or Edit Terrain in the Inspector) has the same brushes for a person. Its brush,
paint and placement settings and the default map resolutions are saved per project and user in
`.jasperengine/user/workspace.yaml`; `set_terrain_maps` and `sculpt_terrain` use those
resolutions when a call gives none.

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

`sample_terrain` with `stats: ["height", "slope", "layers", "coverage"]` (optionally a `region`) returns
percentiles, a slope histogram, layer coverage and the hole fraction with the drawn-layer histogram; `issues: true` lists problems with a `fix`
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

## Material budget and tiers

`sample_terrain` gives `info.material`: the effective `quality` and what decided it (`quality_source`),
`blend_maps_read`, `fetches` (`flat`, `slope`, `worst`, for the tier and `by_tier`), `texture_slots` and
`detail_array`. A point gives `layers_painted`, `layers_drawn` (after the cap) and `fetches`; a call of over 64 points gives each only `hole`.

Set a tier with `set_project_settings graphics.terrainQuality.<platform>` (`high`, `medium`, `low`; absent means
high), or per terrain with `set_properties` on `terrain.quality`. What an export ships is
`graphics.shipped_terrain_tiers` in `get_project_settings`.

| Tier | Layers per pixel | Worst reads | Plus holes | Plus Macro Normal |
|---|---|---|---|---|
| high | all (4 with `limitLayersPerPixel`) | 111 (51 limited) | +1 | +1 |
| medium | 3 | 21 | 22 | 23 |
| low | 2 | 6 | 7 | not drawn |

"Why is this slow on phones": read `info.material.fetches.by_tier`, then look at where the pixels go: the Scene view's
Terrain dropdown, View: Fetch Cost or Layer Count. Don't stop at listing the materials.

## Holes end to end

Holes are one R8 map (`terrain.holeMap`): 0 is a hole, 255 solid, read with no filter. Every pass draws the same texels.
After a cut, check each reader at a point in the middle of the hole with `sample_terrain` `points`:

- `hole`: the drawn surface and the query.
- `collision_gap`: the collider. A sample opens only when all its squares are hole, so a hole under 3 collision cells across
  can draw and still stop a ball: `collision_samples_open` and the warning `hole_no_collision` say so.
- `nav_excluded`: the navmesh leaves the cell out at the next bake; nothing rebakes by itself.
- `pick_at`, or a Lua ray: goes on through the hole to what lies beyond it.

Filling restores the surface exactly: heights, blend and density stay under a hole. A hole takes the base material's Occlusion
map's slot (`features` says `base_occlusion_dropped`); a custom shader that does not read `TERRAIN_HOLES` draws over the hole.

## Material features

On `terrain`: `macroMode` (off, noise, base_map), `resampleEnabled`, `heightBlendSharpness`, `macroNormal`, and per layer
`surfaceLayers[i].cliffProjection` (auto, flat_only). Each is off by default and costs nothing off; on a terrain with no layer Macro Variation waits for one
(issue `macro_without_layers`). Up to 12 layers: layers 10-12
weigh by the inverted alpha of blend maps 1-3, so an opaque map means 0.

## Common mistakes

- Adding a detail layer without a density map and expecting grass: use `density_map {fill: 1}`.
- Expecting `blendmap` to set slot 1 by default: a plain path is slot 0; pass `index`.
- Densities in the hundreds: the cell cap thins them and the frame rate drops; use 1-10.
