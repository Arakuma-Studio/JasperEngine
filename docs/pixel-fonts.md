# Pixel Fonts

Text in a pixel-art game should look drawn, not rendered. A **Bitmap Font** is a font made of
pixels: a sheet you draw in Aseprite, a BMFont `.fnt`, or a pixel TrueType font. It draws at its
native size times a whole number, on the pixel grid, with outlines and shadows made of whole
pixels. Design: `docs/specs/S51-pixel-fonts.md`.

## Making a font

- **From a sheet.** Draw the glyphs on a grid of equal cells, in the order of printable ASCII (space
  first, `~` last), and save it as `<name>@<W>x<H>.png` (for example `hud@6x9.png`). Select it in
  the Project window and choose **Create › Bitmap Font**. The Inspector of a PNG named like that
  offers the same as a banner.
- **From BMFont.** A `.fnt` file (text, XML or binary) is a font by itself; **Create › Bitmap
  Font** on it makes a `.bitmapfont` you can edit.
- **From a pixel TTF.** **Create › Bitmap Font** on a `.ttf`/`.otf` detects the pixel size of its
  outlines and rasterises it with no antialiasing. A TTF's Inspector says when it detects one.
- **Built in.** `builtin:pixel` is a 5x7 font in 6x9 cells covering ASCII and the Latin-1
  letters. **GameObject › UI › Text (Pixel)** makes a Text with it.

The result is a `.bitmapfont` file: its source and its metrics. The file format is in
`docs/mcp/guide/pixel-fonts.md`.

## The Bitmap Font Inspector

Select a `.bitmapfont` to edit it. Every edit is one undo step.

- **Source**: the sheet, cell size, margin and gap (**Detect** reads the file name, then the
  gutters), the characters, or the TrueType threshold and pixel size.
- **Glyph grid**: the sheet at 1x-8x with each cell's character. Hover a cell for its codepoint,
  advance, offset and kerning partners. Click to select it, and use the arrow keys to move the
  selection. Type a character to assign it, and drag a cell's right edge to set its advance.
- **Metrics**: spacing, space width, line height, baseline (drag the line across the grid),
  fallback glyph, coloured, alpha cutoff.
- **Kerning**: add and remove pairs, with a preview of each.
- **Preview**: any text at 1x-8x on a checker, dark or light backdrop, with an outline and shadow.
- **Report**: glyph count, native size, partial alpha, missing characters. Click a warning to
  select its glyph.

A `.fnt` used directly is read-only: **Create Bitmap Font to edit** wraps it.

## Using it

Drop the font on a Text in the Scene view, or pick it in the Text's Font slot. Buttons, Input
Fields and HTML UI (`@font-face`) take it too.

- **Font Size** steps in multiples of the font's native size, shown as `18 (9 × 2)`. A note under
  it gives the native size, the scale and the device pixels per font pixel.
- **Outline Width** is 0, 1 or 2 font pixels, grown through 4 or 8 **Neighbors**.
- **Shadow Offset** is in whole font pixels; softness and dilate do not apply.
- With a **Pixel Perfect Camera**, the text lands on the art grid. Without one, every font pixel is
  a whole number of screen pixels, at any Canvas Scaler factor.
- Text Effects, typewriter reveal (`visibleCharacters`) and Input Field carets work as with any
  font.

Outlines and shadows come from a cache of composed glyphs. Its size is **Project Settings ›
Graphics › Bitmap Text Cache (MiB)**: 0 means 2 MiB on Android, iOS and HTML5 and 8 MiB on desktop.
Raise it if the console warns `Bitmap text cache budget too small`.

## Lua

```lua
local hud = BitmapFont.load("assets/fonts/hud.bitmapfont")
local w, h = hud:measure("SCORE 120", 2)
label.font = "assets/fonts/hud.bitmapfont"
label.fontSize = hud.nativeSize * 2
print(label.pixelScale, label.effectiveFontSize)
```

`BitmapFont.load` returns `nil` and the reason when the font does not load. A font also has
`lineHeight`, `baseline`, `glyphCount`, `colored`, `hasGlyph(char)` and `advance(char)`. A Text has
`outlineNeighbors`.

## Exported games

Each bitmap font ships cooked: `<file>.jfont`, lossless and loaded in its place. The sheet and the
TrueType file are not shipped, unless something else uses them (a sheet also drawn as a sprite
ships as that texture). Scripts that load the sheet PNG by path need it referenced by an asset.

## Limits

- One codepoint is one glyph: no shaping, right-to-left or combining marks. A missing character
  draws the font's fallback glyph.
- A bitmap Text never falls back to a vector font: its vector fallbacks are skipped.
- TrueType kerning is read from the `kern` table only (not GPOS).
