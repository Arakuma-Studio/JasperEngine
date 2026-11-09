# Pixel fonts

Bitmap fonts drawn at a whole-number scale, on the pixel grid, for agents. User docs:
`docs/pixel-fonts.md`.

## The font file

A bitmap font is a Font asset, so it goes in every font slot a TTF goes in: a Text's `font` (and
its fallbacks), Button and Input Field labels, HTML UI `@font-face`, Lua `text.font`.

- `.fnt` (AngelCode BMFont: text, XML or binary) works directly.
- `builtin:pixel` is the engine's own 5x7 font in 6x9 cells: ASCII and Latin-1 letters.
- `.bitmapfont` (JSON) wraps a source and its metrics:

```json
{
  "format": "jasperengine.bitmapfont",
  "version": 1,
  "source": {"type": "grid", "image": "hud@6x9.png", "chars": "ascii"},
  "spacing": 1,
  "glyphs": {"i": {"advance": 2}, "j": {"offset": [-1, 0]}},
  "kerning": [["T", "o", -1]]
}
```

Paths are relative to the `.bitmapfont`.

Sources:

- `grid`: `image` is a sheet of equal cells. The cell comes from an `@WxH` suffix in the file
  name, else `cell: [w, h]`, else transparent gutters (reported).
  - `margin` and `gap` are `[x, y]`.
  - `chars` lists the cells in row-major order: `"ascii"` (default, U+0020-U+007E), `"latin1"`,
    or a string. Alternatively `rows` gives one string per sheet row.
  - U+FFFF in `chars` skips a cell.
- `bmfont`: `file` is a `.fnt`; its advances, offsets, kerning, line height and base are used.
- `truetype`: `file` is a pixel TTF/OTF, rasterised with no antialiasing.
  - `pixel_size` 0 detects the grid of its outlines.
  - `threshold` (0.5) is the coverage that makes a pixel opaque.
  - `chars` is `ascii`, `latin1` (default), `latin_ext` or a string.

Metrics (defaults in brackets):

- `advance`: `"auto"` (rightmost opaque column + 1), `"monospace"`, `"file"` or a number [auto;
  file for bmfont].
- `trim`: `"right"` or `"both"` (both moves each glyph to its leftmost column) [right].
- `spacing` [1], `space_width` [round(cell height / 3)], `line_height` [cell height + 1].
- `baseline`: rows from the cell top [the bottom of A-Z and 0-9, plus 1].
- `fallback`: the glyph for missing characters ["?", else a box].
- `colored`: `"auto"`, true or false.
- `alpha_cutoff`: 0-255; texels at or above it become opaque, the rest clear. It fixes partial
  alpha.
- `glyphs`: per-character `advance` and `offset` overrides.
- `kerning`: `[left, right, pixels]` pairs.
- `fallback_fonts`: bitmap fonts of the same native size, tried before the fallback glyph.

Unknown keys are kept.

## Create, preview, fix

1. Write the file with `write_file`. Its result has the import report (`font`: glyphs, native
   size, baseline, missing ASCII, `partial_alpha_share`, warnings), or typed `errors` with the
   `field` to fix. A bad file is still written.
2. Look at it with `preview_font {path, text: "HP 99\nSCORE"}` (tool group `pixel_art`; white on a
   dark checker, at the largest whole scale that fits `max_size`, or `scale`). Without `text` it
   shows every glyph, labelled. `font.missing` lists characters it lacks.
3. Read the metrics with `get_asset_info` (`font`, `kerning_pairs`, `overrides`, `pixel_report`).

For a vector font, `preview_font` shows smooth text and `get_asset_info` tells whether its outlines
sit on a pixel grid (`pixel_grid_detected`). If they do, wrap it in a `.bitmapfont` with source
`truetype`.

## Scale

- A Text draws at k = max(1, round(fontSize / native size)). Set `fontSize` to native x k
  (`18` for a 9 px font at x2).
- Read `pixelScale` and `effectiveFontSize` (Lua) or check the Inspector's note.
- `outlineWidth` (0, 1 or 2) and `shadowOffset` are whole font pixels. `outlineNeighbors` is 4
  or 8. Shadow softness and dilate do not apply.
- The Text snaps to whole pixels with a Pixel Perfect Camera, and to whole device pixels without
  one.

```lua
local hud = BitmapFont.load("assets/fonts/hud.bitmapfont")
local w, h = hud:measure("SCORE 120", 2)
label.font = "assets/fonts/hud.bitmapfont"
label.fontSize = hud.nativeSize * 2
label.outlineWidth = 1
label.outlineNeighbors = 4
print(label.pixelScale)
```

## HTML UI

`@font-face { font-family: Hud; src: url(fonts/hud.bitmapfont); }` (or a `.fnt`).

- `font-size` picks the whole scale.
- `font-effect: outline(1px <colour>)` and a sharp `text-shadow` draw pixel outlines and shadows.
- Glow, blur and blurred shadows are dropped, and `jasperengine-editor check --ui` reports each.

## The pixel-art check

S42's `font_antialiasing` rule reads `pixel_report`:

- a vector font: wrap it in a `.bitmapfont` with source `truetype`;
- partial alpha: set `alpha_cutoff`;
- a `fontSize` that is not a multiple of the native size: snap it;
- a vector font among a bitmap Text's fallbacks: remove it.
