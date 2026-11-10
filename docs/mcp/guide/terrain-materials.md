# Terrain materials, tiers and holes

Served as `jasper://docs/terrain-materials`; the terrain tools are in `jasper://docs/terrain`.

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

## Twelve layers and coverage

A terrain holds up to 12 layers: blend map slots 0-2 weight layers 0-8 in their RGB, and layers 9-11 by the
inverted alpha of slots 0-2, so an opaque map means 0. `sample_terrain` `stats: ["coverage"]` (with a
`region`) gives the hole fraction and the drawn-layer histogram.
