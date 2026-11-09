# Grid tilemaps (`edit_tilemap`, `get_tilemap`, `edit_tileset`, `import_tilemap`)

A **Grid** GameObject holds a Layout, a Cell Size and a Cell Gap; each **Tilemap Layer** below it holds the cells of
one **Tileset** (`.tileset`). `edit_tilemap` and `get_tilemap` are always listed; `edit_tileset` and `import_tilemap`
are in the `tilemaps` group (`tool_groups {"enable": ["tilemaps"]}`; callable by name either way).

## Coordinates

Cells are signed `[x, y]`, y up: row 0 is the bottom row, negatives allowed. `get_tilemap` and `edit_tilemap` print a
region as ASCII with `lines[0]` the **top** row (the highest y), so what you read is what you see in a rectangular
Grid. `.` is empty. Isometric rows print in cell order, not on screen: row y is the cells (x, y), and the diamond
shape is `get_tilemap image`'s job (a CPU composite), not the text's. Hexagon and staggered isometric print the same
way, one text row per cell row. A paint `grid` is the same picture: its bottom line sits at row `at[1]`, its first
character at column `at[0]`.

A tile's flip letters are `x`, `y` and `t` (transpose, applied first, as Tiled does). Hexagon layouts refuse
transpose (`layout_mismatch`).

## edit_tilemap

One call is one undo step; any failing op rolls everything back. `create: {name, parent, position}` makes a Grid and a
child Tilemap Layer named "Tilemap" (`layout` rectangle by default, also `isometric`, `isometric_staggered`,
`hexagon`; `cell_size`, default the tile size). `entity` targets a Grid or layer, `layer` (name or index) picks one
layer of a Grid, default the first; an op's own `layer` overrides it for that op. A Free Tilemap (`rects` on a create,
or an existing one) keeps the old behaviour: run `{"op": "convert"}` first to make it a Grid and a layer.

`tileset` is a `.tileset` path, or a slice object (`columns`/`rows` or `tile_size`, `offset`, `padding`, `count`,
`names`, `prefix`), which writes `<texture>.tileset` beside `texture` (re-slicing keeps the tile ids) and assigns it.

`preview: true` runs everything and returns the result and an image, writing nothing. Every result has `grid` (the
Grid's id and name, plus the ASCII view of the touched region: `lines`, `legend`, `columns`, `rows`), `tilemap` and
`layer` (the layer), `created`, `tiles`, `tile_rects`, `changed_cells`, `dirty_chunks` and `warnings`
(`no_tile_for_terrain`, tileset load warnings). `image: true` adds the composite to a non-preview call.

### Ops

Ops run in order. Those taking `keep_terrain` (paint, fill, set, line, flood, random, stamp) lock the cell when the
manual tile lands on a terrain cell, so refresh leaves it alone (R26); `terrain` with `keep_terrain` only fills cells
with no terrain yet.

| op | fields | effect |
|---|---|---|
| `paint` | `grid` (string or lines), `legend` (char -> tile name/id/null, or `[tile, flips]`), `at`, `blank: skip\|erase`, `keep_terrain` | ASCII stamp, the bottom line at `at` |
| `fill` | `tile`, `flip`, `from`, `to` (inclusive), `erase`, `keep_terrain` | box of one tile |
| `erase` | `from`/`to` or `cells` | empties tiles (a terrain stays until a `terrain` op) |
| `set` | `cells: [[x, y, tile, flips?], ...]` (tile null erases) | individual cells |
| `clear` | none | removes every tile and terrain of the layer |
| `line` | `tile`, `flip`, `from`, `to`, `erase` | a line of cells on the layout (Bresenham; cube line on hexagons) |
| `flood` | `tile`, `at`, `scope: contiguous\|every`, `erase` | replaces cells equal to the start, connected or everywhere; bounded at 1,000,000 cells |
| `random` | `tiles: [name \| id \| {tile, weight, flip}]`, `cells` or `from`/`to`, `seed` | each cell by a hash of (layer seed, stroke seed, x, y): same arguments, same map |
| `terrain` | `terrain` (name) + `cells` or `from`/`to`, or `grid` + `legend` (char -> terrain name/null); `erase`, `keep_terrain` | paints the terrain plane and re-chooses the tiles of the changed masks |
| `refresh` | `from`/`to` (default: the used rect) | re-chooses the tiles of every terrain cell in the rectangle |
| `stamp` | `name` (a stamp of the tileset), `at`, `keep_terrain` | stamps its cells at `at` |
| `add_layer` | `name`, `tileset`, `dual_grid` | adds a layer (next Order in Layer); later ops reach it with `layer` |
| `convert` | none, first and alone on a Free Tilemap | Grid + layer on the same GameObject; `preview` only checks it |

Errors are `{kind, message, fix, field}` and name the op (`ops[2].legend`): `unknown_tile`, `unknown_terrain`,
`unknown_layer`, `layout_mismatch`, `not_grid_aligned` (convert), `tileset_missing`, `locked_linked`,
`region_too_large` (over 1,000,000 cells), `parse` (import), `invalid_peering` and `unknown_property` (tilesets).

## get_tilemap

`entity`: a Grid or a layer; on a Free Tilemap it answers the old shape. Result: `layout`, `cell_size`, `layers` (name,
entity, tileset, sorting, cells, used rect) and, for the chosen `layer` and `region` (`[x0, y0, x1, y1]`, default the
used rect), `grid` (ASCII as in edit_tilemap; `max_cells` caps the lines, `lines_omitted` says so). Options:

- `include_terrain`: `terrain` (a second ASCII grid of terrain names, `.` is none).
- `include_tiles`: `cells` as `[x, y, tile, flags]`, up to `limit`.
- `missing_tiles`: `missing_tiles` as `[x, y, tile]`, cells whose id the tileset lacks, `missing_count`.
- `image` with `scale` 1-8: a PNG composite of the region with no GPU (the same arguments give the same bytes).

## Tilesets: edit_tileset

A preview by default: the report, plus a contact sheet image with each tile's id. `output: true` writes it (one undo
step). `check: true` returns only the report (`verdict` PASS/WARN/FAIL: FAIL on a validity problem, WARN on masks
without a tile, `terrain_sets` per set: `required`, `missing_count`, `missing` masks; `allow_transforms` counts flips
of square tiles as candidates).

| Field | Shape |
|---|---|
| `sources` | `{texture, tile_size, margin, spacing, skip_empty}` (slices by Tiled's names; re-slicing keeps ids); `{sheet, frames}`; `{file, tileset}` (an .aseprite tileset) |
| `tiles` | edits selected by `id`, `tile` (name or id) or `range: [first, last]`: `name`, `probability`, `offset`, `random_start`, `nav_area`, `animation: [{tile, ms}]`, `properties: {name: value}`, `collision: {mode: none\|full\|alpha\|custom, threshold, kind: solid\|one_way\|sensor, polygons}`, `terrain: {set, center, peering}` |
| `property_schema` | `[{name, type: bool\|int\|float\|string\|color, default}]`: undeclared properties are refused (`unknown_property`) |
| `terrain_sets` | `[{name, mode: blob\|sides\|dual_grid, two_surface, terrains: [{name, color}], template, origin, terrain, second_terrain}]`: `template` + `origin` assigns names and peering values to the block of tiles whose top-left tile is `origin` |
| `stamps` | `[{name, size, cells: [[x, y, tile, flips?], ...]}]` |

### Terrain masks

Peering keys are `n ne e se s sw w nw` (rectangle, isometric); hexagons use their six sides and six `c_*` corners. A
value is `any` (no constraint), `none` (no terrain of this set) or a terrain of the set. Templates (block's top-left
tile is `origin`): `blob_47` (cr31's 7x7 sheet), `blob_47_godot` (Godot's 12x4 "3x3 minimal"), `sides_16` (4x4, tile i
has the sides of i: N=1 E=2 S=4 W=8), `dual_grid_4x4` (SpriteCook: masks TL=1 TR=2 BL=4 BR=8; frame 12 is the second
surface of a `two_surface` set). A dual-grid layer's tile (x, y) sits between logical cells (x-1, y-1)..(x, y).

## Importing maps: import_tilemap

`file` (`.tmj`, `.tmx`, `.ldtk`), `level` (LDtk), `scene`/`parent` or `prefab`, `tilesets_dir` (default next to the
map), `linked`. A preview by default (grids, layers, tilesets and the files they would write, skipped features,
warnings); `output: true` imports in one undo step (a job when large: `get_job`). A `linked` Grid is read-only
(`locked_linked`) and re-imports when the file changes.

Mapping: a Tiled map becomes one Grid (orthogonal, isometric, staggered, hexagonal) with its tile layers, tilesets and
wang sets as terrains (a corner wang set imports as blob with sides `any`, reported); an LDtk level becomes one Grid per
level with its IntGrid and Auto Layers. Objects, entities, image layers and parallax are `skipped`, not imported.

## Recipes

Paint a lake with a water terrain, then read it back as an image:

```json
{"tool": "edit_tilemap", "create": {"name": "Lake"}, "tileset": "assets/Tilesets/overworld.tileset",
 "ops": [{"op": "terrain", "terrain": "Water", "from": [2, 2], "to": [10, 8]}]}
{"tool": "get_tilemap", "entity": "Lake", "include_terrain": true, "missing_tiles": true, "image": true, "scale": 2}
```

`edit_tileset {"path": "...", "check": true}` tells you whether every mask has a tile before you paint.
