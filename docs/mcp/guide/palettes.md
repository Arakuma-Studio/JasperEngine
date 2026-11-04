# Palettes (`edit_palette`, `apply_palette`)

A palette is a `.palette` asset (UTF-8 JSON, `docs/palettes.md` has the format): up to 256 exact sRGB colours,
names, locked colours, a transparent index, named ramps (optionally colour-cycling) and up to 15 variants (team
colours, a hurt flash) aligned index by index with the colours. Both tools are in the `pixel_art` group
(`tool_groups {"enable": ["pixel_art"]}`; callable by name either way). Humans have Window > 2D > Palette, the
Project window's Apply Palette... / Reduce Colours..., and the material and post-processing inspectors.

## Naming a palette

- A project path: `assets/Palettes/hero.palette` (the result's `path`). Materials and scene settings store the path
  relative to the assets folder, the result's `asset_path` (`Palettes/hero.palette`).
- `builtin:<id>`: one of the 38 built-ins (`edit_palette` with no arguments lists them: `pico-8`, `game-boy`,
  `sweetie-16`, `endesga-32`, `aap-64`, `resurrect-64`, `cga`, `ega`, `commodore-64`, `zx-spectrum`, pixelfx's 27
  ramps such as `fire` and `ice`, and `pixelfx-effects`). Using one copies it once to `assets/Palettes/<id>.palette`;
  later uses reuse that copy (`reused: true`). `apply_palette` reads a built-in without copying it.
- `apply_palette` also takes `[hex, ...]`, and without `palette` (and without `reduce`) the project's Default
  Palette (`set_project_settings {"values": {"palettes.default": "<path>"}}`).

## The loop: preview, then write

1. Call without `output`: nothing is written. The result has the palette (`palette: {name, count, colors, ramps,
   variants, locked, transparent}`), `changed` (indices), `warnings`, and `next`: what writing needs.
2. Call again with the same arguments plus `output: true`: the `.palette` (and the export) are written as one undo
   step, listed in `undo` (`undo {"scope": "project"}` puts the files back). A new file that exists needs
   `overwrite: true`.

Writes are refused while a scene plays (`play_mode`); previews work. Errors are `{kind, message, fix, field}`.

## edit_palette

| Arguments | Does |
|---|---|
| none | lists the project's palettes and the built-ins |
| `path` | reads a palette (also `builtin:<id>`) |
| `from: {builtin: id}` | the built-in's project copy (made on `output`, reused afterwards) |
| `from: {lospec: slug}` | fetches a Lospec palette: a slug, `https://lospec.com/palette-list/<slug>` or `lospec-palette://<slug>`; a job of at most 10 s (the call waits for it); default path `assets/Palettes/<slug>.palette` |
| `from: {file: path}` | imports `.gpl`, `.hex`, JASC/RIFF `.pal`, Paint.NET `.txt`, Adobe `.ase`, `.colors`, or an image's colours; default path beside it |
| `from: {texture: path, max_colors?}` | a texture's colours (Reduce Colours to `max_colors`, 2-256, when it has more than 256) |
| `from: {colors: [hex]}` | a new palette from colours (`path` required) |
| `ops: [...]` | edits, below, applied in order as one batch: any failing op changes nothing |
| `export: {path, format?, swatch_size?}` | `gpl`, `hex`, `pal` (JASC), `pal_riff`, `txt`, `ase`, `png` (a strip, `swatch_size` px per colour), `colors` (the first 76); the format follows the extension when left out |
| `texture: path` | `usage`: pixels per palette index, off-palette pixels and colours with their nearest index |
| `image: true` | a swatch image: the colours, each variant beside them, a strip per ramp |

Lospec errors: `network_unavailable` (offline, no curl, blocked; the fix: download the `.hex` and import it with
`from: {file}`), `lospec_not_found` (copy the slug from the palette's address), `bad_response`.

### Ops

Indices count from 0. Colours are `"#rrggbb"` or `"#rrggbbaa"`.

| op | Fields |
|---|---|
| `set` | `index`, `color` |
| `insert` | `index?` (default the end), `color` or `colors: [...]` |
| `remove` | `index` or `indices: [...]` (one colour stays) |
| `move` | `from`, `to` |
| `sort` | `by`: `hue`, `saturation`, `luma` (OKLab L), `value`, `reverse`; ramps, variants, locks, names and the transparent index follow their colours (it warns: indexed Aseprite art keeps its old numbers) |
| `rename_color` | `index`, `name` (`""` removes it) |
| `lock` | `index` or `indices`, `locked?` (default true): Reduce Colours keeps locked colours |
| `set_transparent` | `index` (`-1`: none) |
| `set_info` | `name?`, `author?`, `description?`, `columns?` (1-64, a display hint) |
| `add_ramp` | `name`, `colors: [index or hex]` (darkest first; a new hex is appended), `cycle?` (steps per second) |
| `set_ramp` | `name`, `colors?`, `cycle?`, `reverse?: true`, `fit?: steps` (resample keeping both ends), `rename?` |
| `remove_ramp` | `name` |
| `generate_ramp` | `base` (hex or index), `darker?`/`lighter?` (0-15, default 3), `mode?` (`hue_shift` default, `oklch`), `hue?`/`saturation?`/`value?` per step (10), `hue_lighten_limit?` (60), `hue_darken_limit?` (240), `saturation_floor?`, `value_floor?` (10), `darkest_l?` (0.2), `lightest_l?` (0.95), `chroma_peak?` (0.5), `name?`, `replace?: <ramp>` (rewrites that ramp instead); new colours are appended, exact duplicates reused |
| `add_variant` | `name`, `colors?: [hex or null]` (null keeps the base colour) |
| `set_variant` | `name`, then `colors?` (the whole list), or `index` with `color` (no `color`: back to the base), `rename?` |
| `remove_variant` | `name` |

Op errors name the op and field (`ops[2].index`): `invalid_value`, `index_out_of_range`, `not_found` (a ramp or
variant name; the fix lists the names), `exists`, `read_only` (a palette from a newer format version).

```json
{"tool": "edit_palette", "arguments": {"path": "assets/Palettes/hero.palette", "output": true, "ops": [
  {"op": "add_variant", "name": "Team Blue", "colors": [null, "#29366f", "#3b5dc9", "#41a6f6"]},
  {"op": "generate_ramp", "base": "#b13e53", "darker": 2, "lighter": 2, "name": "cape"}]}}
```

## apply_palette

Maps every pixel of a texture (PNG, BMP, TGA) to its nearest palette colour (`distance`: `oklab` default,
`ciede2000`, `rgb`), with `dither` (`none` default, `bayer2`, `bayer4`, `bayer8`, `floyd_steinberg`) at
`dither_strength` (0-1, default 1). `alpha_threshold` 0 keeps alpha; 1-255 makes it binary at that value (128 is
the pixel-art choice). `reduce: N` (2-256) first builds an N-colour palette from the texture (median cut and
k-means in OKLab; with `palette`, its locked colours are kept); `save_palette` writes that palette with the texture.

The result: `verdict`, `stats {colors_before, colors_after, pixels_changed, pixels_mapped, mean_delta_e, max_delta_e}`
(OKLab x 100), `usage [{index, hex, count}]`, `lost [{hex, mapped, count, box: [x, y, w, h]}]` (8-connected groups
whose colour moved more than 6 dE; `lost_groups` counts them all), and a before | after image (`image: false`
drops it; `max_width` default 768).

| verdict | Means | `call --measure-only` exits |
|---|---|---|
| `clean` | already only palette colours: 0 pixels change | 0 |
| `fixable` | max dE 15 or less and no lost group of 16 px or more | 3 |
| `needs_person` | anything else: look at `lost` (another palette, more colours, or a person) | 4 |

`output: true` overwrites the texture (a PNG; its `.meta` and GUID stay), `output: "<path>.png"` writes a new file
(it must not exist). `measure_only: true` is a read-only check (no image by default, `cli_exit` in the result); it
refuses `output` and `save_palette`. A large texture runs as a job (`job_id`; the call waits up to 120 s).

## Palette Swap (existing tools)

`library:sprite`'s Palette Swap draws each pixel whose colour is exactly a colour of the source palette with the
same index of the active row: row 0 is the source itself, rows 1... its variants, then extra targets. Off-palette
and filtered pixels stay as they are, so the texture must be lossless (no compression or mipmaps, Nearest filter).

```json
{"tool": "set_properties", "arguments": {"entity": "Hero", "values": {"mesh": {
  "submeshes[0].material.shader": "library:sprite"}}}}
```

```json
{"tool": "set_properties", "arguments": {"entity": "Hero", "values": {"mesh": {
  "submeshes[0].material.shaderFeatures": ["PALETTE_SWAP"],
  "submeshes[0].material.shaderPalettes": {"u_palette": {"source": "Palettes/hero.palette",
    "targets": ["Palettes/enemy.palette", "Palettes/hero.palette#Hurt"], "useVariants": true}},
  "submeshes[0].material.shaderParams.paletteRow": 1}}}}
```

- `shaderPalettes` paths are relative to the assets folder (edit_palette's `asset_path`); a built-in is used
  through its project copy (make it with `edit_palette {"from": {"builtin": "<id>"}, "output": true}`).
- `get_entity` returns `palette_rows: [{submesh, rows: ["", "Team Blue", ...], row}]`: the names `sprite.palette`
  takes.
- `PALETTE_CYCLE` rotates the source's ramps that have a `cycle` speed; `shaderParams.paletteCycleSpeed` scales it
  (negative reverses, 0 freezes).
- `create_material` / `update_material` take the same `shaderFeatures`, `shaderParams` and `shaderPalettes`.

## Palette Quantize (existing tools)

The main camera's last colour step puts every pixel of the frame onto a palette (before CRT and film grain; the UI
drawn after it is not quantized):

```json
{"tool": "set_scene_settings", "arguments": {"settings": {"post_processing": {"palette_enabled": true,
  "palette": "assets/Palettes/game-boy.palette", "palette_dither": "bayer4", "palette_dither_strength": 1,
  "palette_distance": "oklab", "palette_precision": "normal"}}}}
```

`palette_dither`: `none`, `bayer2`, `bayer4` (default), `bayer8`; `palette_distance`: `oklab`, `rgb`;
`palette_precision`: `normal` (32 cells per channel), `high` (64, for palettes of many close shades). The table
bakes in the background: the first frames after a change keep the previous look.

## Lua

Indices count from 0; colours are linear `Vector4`s that draw as the palette's exact bytes.

```lua
local p = Palette.load("Palettes/hero.palette")   -- relative to the assets folder; nil when it can't be read
print(p.name, p.count, p:getHex(3))               -- "#b13e53"
local first = p:getColor(0)                       -- a Vector4
local i = p:nearest(Vector4(0.6, 0.1, 0.1, 1))    -- OKLab; p:nearest(color, "ciede2000")
local cape = p:getRamp("cape")                    -- a table of Vector4
local hot = p:rampColor("cape", 0.8)              -- a step of the ramp, never blended
for _, name in ipairs(p.variants) do print(name) end
for _, name in ipairs(p.ramps) do print(name) end
```

```lua
local hero = Sprite(self.scene, self.entity)
hero.palette = "Team Blue"         -- a row by name (Lua error listing the rows when there is none)
hero.palette = 0                   -- or by number; "" or 0 is the original colours
hero.paletteRow = 2
hero.paletteCycleSpeed = -0.5
local custom = Palette.new({ "#000000", "#ff00ff", "#00ffff" })
hero:setPaletteTarget(3, custom)   -- row 3 shows any palette, one made at run time included
```

```lua
local pp = self.scene.postProcessing   -- a copy: edit it, then assign it back
pp.paletteEnabled = true
pp.palette = "Palettes/game-boy.palette"
pp.paletteDither = PaletteDither.BAYER4        -- NONE, BAYER2, BAYER4, BAYER8
pp.paletteDistance = PaletteDistance.OKLAB     -- OKLAB, RGB
pp.palettePrecision = PalettePrecision.NORMAL  -- NORMAL, HIGH
pp.paletteDitherStrength = 1
self.scene.postProcessing = pp
```

Tilemap and MeshPolygon have the same `palette`, `paletteRow`, `paletteCycleSpeed` and `setPaletteTarget`.
