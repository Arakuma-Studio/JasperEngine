# S51 — Pixel Fonts: bitmap fonts at integer scale, on the pixel grid

Pixel-art family (S41–S52, `PIXEL-ART-LEDGER.md`), wave 5B. Depends on S41 (art grid and snap rule,
wave 5A). Feeds S42 (its `font_antialiasing` rule) and S48 (contact-sheet labels), both of the same
wave, which read S51 when it is present. Global decisions:
`docs/specs/README.md`. Research: `/tmp/pixelart-research/spryte` (MIT, read-only, never run).

## 1. Goal

Make text in a pixel-art game look drawn, not rendered. A font drawn in Aseprite as a grid
(`font@6x9.png`), exported from BMFont (`.fnt`), or shipped as a pixel TTF becomes a Bitmap Font
asset. Its proportional advances are measured automatically, and per-glyph overrides, kerning, a
fallback glyph, a baseline and a line height can be edited in a character-map Inspector. It draws
at its native size times a whole number, snapped to the art grid, with one-pixel outlines and
shadows that are pixels too. The same font works in world text, the component UI (Text, Button,
Input Field) and the HTML UI. Agents preview any string in any font as an image, read the font's
metrics, and get the S42 rule check on it.

## 2. Scope

**In scope**
- A Bitmap Font asset (`.bitmapfont`, JSON, `AssetType::Font`). It is built from:
  - a grid sheet;
  - an AngelCode BMFont `.fnt` (text, XML and binary);
  - a TTF/OTF pixel font rasterised at its native size with no antialiasing (coverage threshold,
    native size auto-detected).
- `.fnt` files usable directly as fonts.
- Metrics: auto-measured advances (rightmost opaque column + 1), per-glyph overrides (advance,
  offset), kerning pairs, fallback glyph, space width, letter spacing, line height, baseline,
  monochrome or coloured glyphs.
- Rendering at native size × integer scale only, snapped to the S41 grid. `fontSize` snaps to whole
  multiples of the native size.
- Pixel outline (4 or 8 neighbours, 1–2 px) and pixel shadow (whole-pixel offset) through a styled
  glyph cache (shelf-packed, recycled).
- Surfaces:
  - the Text component (component UI and world text: it draws in its scene's camera pass);
  - Button labels and Input Field, which are Text;
  - Text Effects and typewriter (`visibleCharacters`);
  - HTML UI `@font-face`.
- A built-in pixel font drawn for the engine (`builtin:pixel`, 5×7 glyphs in 6×9 cells).
- Editor:
  - the Bitmap Font Inspector with a glyph grid and character-map editor;
  - Create › Bitmap Font from a PNG, `.fnt` or TTF;
  - Text Inspector integer-scale controls;
  - Scene view drop.
- Lua `BitmapFont` info and `Text.pixelScale`.
- MCP: `view_asset` renders a string or the glyph grid; `get_asset_info` reports fonts;
  `write_file` validates `.bitmapfont`.
- Export cooking to `.jfont`.
- The S42 font check data.

**Out of scope**
- Vector text quality (S16 owns HTML UI sharpness, MSDF, GPU effects). Smooth TTF text is unchanged.
- Complex shaping, bidi and right-to-left scripts, and combining marks for bitmap fonts: one
  codepoint is one glyph. A codepoint the font lacks draws the fallback glyph.
- Colour emoji and SVG fonts.
- Drawing fonts in the editor: there is no glyph painting (ledger: Aseprite is the drawing tool;
  S47 is the agent path).
- BMFont distance-field fonts and channel-packed outline channels beyond the alpha mask (`chnl`
  other than 15 or a single channel is refused with a typed error).
- pixelfx's `gallery/fonts/PatchPixel*` (do not copy, ledger and brief). The built-in font is
  original work.

## 3. Current state (read on `specs/pixel-art` 1b1c34fbd)

**Font asset and files**:
- `AssetType::Font = 7` (`engine/core/asset/AssetType.h:23, 51`) covers `.ttf` and `.otf`, sidecar
  `.meta` (`editor/asset/AssetTypes.cpp:32`).
- `TextComponent::font` is a `FontArray` of `MAX_TEXT_FONTS` (4) asset refs: the main font then
  fallbacks (`engine/core/component/TextComponent.h:20-22`, `Engine.h:110-111`; schema
  `editor/asset/AssetRefSchema.cpp:123`).
- The import filters list `*.ttf`, `*.otf` (`editor/Project.cpp:6593`).
- No `.fnt`, no grid font, no bitmap font anywhere (grep: no `fnt`/`bmfont` outside vendored code).

**Text rendering (component UI and world)**: `STBText` (`engine/core/util/STBText.{h,cpp}`, 151 +
690 lines).
- One glyph atlas per (font chain, size, raster scale, SDF padding). Faces come from `FontRegistry`
  (`FontRegistry.h:19-52`), shaping is HarfBuzz through `TextShaper`, and packing is `GlyphAtlas`, a
  shelf packer over R8 or RGBA8 that never moves packed rectangles (`GlyphAtlas.h`).
- Glyphs are antialiased coverage from `stbtt_MakeGlyphBitmap` (`FontFace.cpp:315-336`).
- Quads are aligned to whole text pixels (`STBText.cpp:507-509`), but advances are fractional
  HarfBuzz advances, so a pixel TTF drifts off its grid.
- `UISystem::getTextOptions` (`engine/core/subsystem/UISystem.cpp:224-253`) rasterises at
  `fontSize × canvas raster scale`, rounded to whole pixels.
- Any outline or shadow switches to a signed-distance atlas of at least 32 px with soft,
  `fwidth`-antialiased edges (`ui.frag:69-100`): a 1 px pixel outline is impossible today.
- The atlas is sampled `.r` as coverage (`ui.frag:63-65`) with the texture's default LINEAR filter
  (`Texture.cpp:44-45`).
- Atlases are pooled by id in `FontPool` (`engine/core/pool/FontPool.h:26-46`), with hot reload.
- **Text is drawn in its scene's camera pass with the meshes** (`RenderSystem.cpp:16317-16323`):
  under a UI camera for HUDs, by the scene camera for labels in a 2D world. So "world text" is the
  same Text component (verified by reading the draw loop).
- Text Effects (`TextEffectComponent.h`) and `visibleCharacters` (`TextComponent.h:54-55`,
  `UISystem.cpp:406`) work on per-glyph quads through `glyphInfo`.

**Editor**:
- The Text Inspector is `editor/window/InspectorWindow.cpp:991`; a font dropped on a Text in the
  Scene view is a `PropertyCmd<FontArray>` (`editor/window/SceneWindow.cpp:699`).
- Asset Inspectors exist for several types (`InspectorWindow.h:358-431`, `drawInspectedAsset`), but
  not for fonts.

**Lua**: `Text.font`, `Text.fontSize` and the outline/shadow properties
(`engine/core/script/binding/GameObjectClassesLua.cpp:2920-2927`).

**HTML UI** (S16):
- `RmlFontEngine` (`engine/core/htmlui/RmlFontEngine.{h,cpp}`, 1,872 lines) loads `@font-face`
  through `LoadFontFace` (`RmlFontEngine.cpp:187-204`) into R8 coverage pages with a budget and LRU
  eviction (S16 R12-R13).
- CPU font effects make RGBA8 effect pages (`RmlFontEffects.cpp`).
- Transform-aware exact-scale text and MSDF apply (S16 R14-R16).
- Glyph translation is snapped to device pixels (`RmlRenderer.cpp:1794`, `snapTextTranslation`).
- No bitmap face path.

**MCP**:
- `view_asset` (`editor/mcp/tools/AssetTools.cpp:3813-3828`) shows images and thumbnails; a font
  shows nothing.
- `get_asset_info` (`:3796-3811`) does not describe fonts.
- `write_file` validates some types (`:3847-3870`).

**Reference behaviour** (spryte `src/bitmap-font.ts`, MIT):
- a font is a `font@WxH.png` grid plus a `chars` string (default printable ASCII, `:41`);
- the advance is the rightmost opaque column + 1 (`:97-112`);
- `spacing` is 1, `spaceWidth = round(h/3)`, `lineHeight = h + 1`, fallback `?` (`:56-71`);
- a text cache keyed `scale|font|text` on 1024 px shelf pages with a 1 px gutter, recycled when
  unused this frame and the last (`src/draw.ts:846-916`).

## 4. Reference

| Source | What | Take | Change |
|---|---|---|---|
| spryte `src/bitmap-font.ts:1-127` (MIT) | Grid font model, defaults, measuring | Advance = rightmost opaque column + 1; spacing 1; space = round(h/3); line height h + 1; fallback `?`; error when `chars` outnumbers cells (`:54-55`) | A typed error with the counts and a fix; optional left trim; per-glyph overrides; kerning; baseline |
| spryte `src/draw.ts:846-916` (`TextCache`) | Shelf pages, 1 px gutter, recycling | The page rule: a page whose entries were not drawn this frame or the last can be reused | Caches styled glyphs (outline, shadow) per font and style, not whole strings: Text components keep their meshes, and per-glyph quads keep Text Effects, typewriter and caret extents working |
| spryte `src/draw.ts:306` + `src/webgl2.ts` | One snap rule on CPU and GPU (spryte's is the unbiased `floor(e + 0.5)`) | Text pivots and glyph origins snap through S41's rule | S41's biased rule `floor(p + 0.5 + 2⁻⁷)` in base pixels (`pixelart::snap` on the CPU, `pixelSnapRound` in `pixel_snap.glsl`), never spryte's plain form |
| AngelCode BMFont file format (1.10+: text, XML, binary v3) | `info`, `common` (lineHeight, base, scaleW/H, pages, packed), `page`, `char` (id, x, y, width, height, xoffset, yoffset, xadvance, page, chnl), `kerning` | Whole format: the parser is ours (no third-party code) | Pages must be PNGs in the project; `packed` channel fonts refused |
| `FontFace` + stb_truetype (in tree) | Glyph outlines (`stbtt_GetGlyphShape`), coverage, `kern` table (`stbtt_GetGlyphKernAdvance`) | Native-size detection from outline coordinates; thresholded coverage; kerning rounded to whole pixels | GPOS kerning is not read for TTF-built bitmap fonts (documented) |
| pixelfx `src/core/font.js` (MIT) | A canvas pixel-font helper | Read only, for the "measure, then draw at integer scale" loop; its `gallery/fonts/PatchPixel*` are **not** used | — |

No code is copied, so `NOTICE.md` needs no new entry (spryte's rules are re-implemented). If an
implementer ports a function line by line, it keeps spryte's MIT header and gets an entry.

## 5. Requirements

### Functional — the Bitmap Font asset

- **R1 Asset.**
  - A `.bitmapfont` file (JSON, `"format": "jasperengine.bitmapfont"`, `"version": 1`) is an
    `AssetType::Font` with a sidecar `.meta` (the Font row of `AssetTypes.cpp:32` gains
    `.bitmapfont` and `.fnt`).
  - Every font slot that takes a TTF takes it: Text fonts and fallbacks, Button and Input Field
    labels, HTML UI `@font-face`, Lua `text.font`.
  - Its `source` is one of R2–R4.
  - A `.fnt` file is a font by itself (R3 defaults, no overrides). A `.bitmapfont` wrapping it adds
    overrides.
- **R2 Grid source** `{"type": "grid", "image": "fonts/hud@6x9.png"}`.
  - The cell size comes from the `@WxH` file-name suffix, else `cell: [w, h]`, else auto-detection
    of fully transparent gutter rows and columns at a regular period (reported, never silent).
  - Also `margin` (`[x, y]` before the first cell) and `gap` (`[x, y]` between cells).
  - The characters, in sheet order and row-major:
    - `chars` is a UTF-8 string, default `"ascii"`: U+0020–U+007E, 95 glyphs, spryte's order;
    - or `rows` is one string per sheet row, so a short row does not shift the rest.
  - A cell whose character is U+FFFF is skipped.
  - More characters than cells is the typed error `{kind: "font_chars_overflow", message: "95 chars
    but 90 cells (15 x 6 of 6x9)", fix: "check the cell size in the file name or set cell", field:
    "source.chars"}`.
- **R3 BMFont source** `{"type": "bmfont", "file": "fonts/hud.fnt"}` or a `.fnt` used directly.
  - All three encodings are read, detected by content:
    - text (`info face=...`);
    - XML (`<font>`);
    - binary (`BMF` + version 3, blocks 1–5).
  - Read: pages (PNG files relative to the `.fnt`), chars, kernings, `common.lineHeight`,
    `common.base`.
  - `chnl` must be 15 (all channels) or a single channel (1, 2, 4, 8): that channel is the alpha.
  - `packed=1` with outline channels, a missing page or an id beyond U+10FFFF are typed errors.
  - The file's `xadvance` and offsets are used as given (`advance: "file"`, overridable).
- **R4 TrueType source** `{"type": "truetype", "file": "fonts/pixel.ttf", "pixel_size": 0,
  "threshold": 0.5, "chars": "latin1"}`.
  - The native size is detected when `pixel_size` is 0:
    - take the outline coordinates of the font's ASCII glyphs in font units;
    - their largest common unit `u` (≥ 90 % of coordinates multiples of `u`) gives
      `pixel_size = unitsPerEm / u`, accepted between 5 and 64;
    - otherwise this is the typed error `font_pixel_size_unknown` with the best candidates.
  - Glyphs are rasterised at exactly that size. A texel is opaque when its coverage is ≥ `threshold`,
    else transparent. No antialiasing is ever produced.
  - The pen, advances and bearings are rounded to whole pixels, and kerning comes from the `kern`
    table, rounded.
  - `chars`: `ascii`, `latin1` (default; U+0020–U+00FF), `latin_ext` (adds U+0100–U+017F), or an
    explicit string.
- **R5 Metrics and overrides** (file keys; defaults in brackets):
  - `advance`: `"auto"` (rightmost opaque column + 1, after trimming), `"monospace"` (cell width),
    `"file"` (BMFont), or an integer. [auto; `file` for BMFont]
  - `trim`: `"right"` or `"both"` (both also moves the glyph so its leftmost opaque column is 0).
    [right]
  - `spacing`: whole pixels added after every glyph. [1]
  - `space_width`. [round(cell height / 3)]
  - `line_height`. [cell height + 1; BMFont `lineHeight`]
  - `baseline`: pixels from the cell top. [auto: the most common bottom opaque row of A–Z and 0–9,
    plus 1; BMFont `base`]
  - `fallback`: the glyph drawn for missing codepoints. [`?` when present, else a built-in hollow box
    of the cell size]
  - `colored`: `"auto"`, `true` or `false`. Auto is monochrome when every opaque texel has the same
    RGB.
  - `alpha_cutoff`: 0..255, 0 = keep. Texels with alpha ≥ cutoff become 255, the others 0 (the S42
    fix for sloppy sheets). [0]
  - `glyphs`: `{"<char>": {"advance": n, "offset": [x, y]}}` overrides.
  - `kerning`: `[["T", "o", -1], ...]`, whole pixels, merged over the file's.
  - `fallback_fonts`: other bitmap fonts with the same native size, tried before the fallback glyph.
  - Unknown keys are kept (lenient, canonical writer, like `.shake`).
- **R6 Measuring is deterministic and reported.** Importing produces a report:
  - glyph count, cells used/empty;
  - native size (cell or ascender+descender), baseline, line height;
  - colored;
  - `partial_alpha_share` (texels with 0 < a < 255 over opaque texels);
  - missing ASCII characters;
  - empty glyphs other than space;
  - overrides applied.

  The same file always gives the same metrics.

### Functional — drawing

- **R7 Integer scale only.**
  - A Text with a bitmap font draws at pixel scale `k = max(1, round(fontSize / nativeSize))`. The
    effective font size is `k × nativeSize`.
  - `Text.pixelScale` (read-only, 0 for vector fonts) and `Text.effectiveFontSize` report it.
  - The Inspector snaps `fontSize` to multiples (R22). Lua and MCP may set any value; development
    builds log once per Text when the value is not a multiple.
- **R8 Snapped to the grid.**
  - Text drawn by a camera with S41's Pixel Perfect Camera and Pixel Snapping is snapped by S41's
    UI vertex stage (S41 R9: "UI images, nine-slice and text drawn by that camera"; rule `snap(p) =
    floor(p + 0.5 + 2⁻⁷)` in base pixels, S41 §6.2, through the frame block's `pixelSnap` and
    `pixelSnapOffset`).
  - S51 guarantees that the snap moves a Text as one piece: every glyph quad corner is a whole
    number of font pixels × `k` from the Text's origin, so all corners carry the same fractional
    part and round together. Each font pixel is exactly `k × k` base pixels, or `k·PPU`-scaled world
    units when S41's PPU > 1 (S41 D2).
  - Without S41 snapping (a UI camera with the Canvas Scaler, or no Pixel Perfect Camera), one font
    pixel is `d = max(1, round(k × canvasRasterScale))` device pixels and the Text's origin is
    rounded to device pixels on the CPU (the same `snap` from `PixelSnap.h`). Text is crisp at any
    Canvas Scaler factor; the drawn size may differ from `k × nativeSize` units by under half a
    device pixel per font pixel.
  - Rotation: a rotated bitmap Text follows the camera's S41 rotation policy: under Steps90 (the
    default) it is drawn rotated to the nearest 90° (S41 R10, its Transform keeps the angle), and
    under Rotate On Grid it keeps its angle and its pivot snaps (`pixelSnapCenterDelta`), drawn with
    nearest sampling. S42's `rotation` rule reports it.
- **R9 Sampling.** Bitmap glyphs are always sampled NEAREST, without mips or compression, from the
  font's own atlas (R11). Monochrome fonts use an R8 atlas through the existing coverage path
  (`HAS_FONTATLAS_TEXTURE`) and take the Text colour. Coloured fonts use an RGBA8 atlas through the
  textured path (`HAS_TEXTURE`), with the Text colour multiplying the art (white = as drawn). No new
  UI shader variant.
- **R10 Pixel outline and shadow.**
  - For a bitmap font, the Text's existing `outlineWidth` is whole font pixels (0, 1 or 2; other
    values round), and a new `outlineNeighbors` (4 or 8, default 8) picks the dilation.
  - `shadowOffset` is whole font pixels.
  - `shadowSoftness` and `shadowDilate` are ignored and their Inspector rows are hidden.
  - Outline and shadow come from the styled glyph cache (R12) as a back layer: all back quads of a
    Text are drawn before all face quads, so an outline or shadow never covers a neighbouring
    glyph's face.
  - No SDF atlas is ever made for a bitmap font.
- **R11 Font atlas.** On load the font repacks its glyph cells into its own atlas with
  `GlyphAtlas`: shelf packing, 1 px gutter and `reserveLeading(1)`, so a NEAREST tap at a rect edge
  never reads a neighbour.
  - The atlas is R8 (monochrome) or RGBA8 (coloured), at most 2048², split into several pages beyond
    that.
  - One atlas is shared by every Text, size and scale. The scale is in the quads, not in the
    texture.
- **R12 Styled glyph cache** (`engine/core/pixelart/BitmapTextCache`):
  - Key: (font, style = outline width, neighbours, outline colour, shadow offset, shadow colour).
  - Content: an RGBA8 page of the back-layer images (dilated mask in the outline colour, shadow in the
    shadow colour, composited), plus the face masks.
  - Packing: shelf-packed with a 1 px gutter, filled on demand per glyph.
  - Recycling: a page no Text drew from this frame or the last can be cleared and reused (spryte's
    rule); Texts using a cleared page rebuild on their next update.
  - Budget: 2 MiB on Android, iOS and HTML5, 8 MiB on desktop (project setting
    `bitmapTextCacheMiB`, 0 = platform default).
  - More than 4 recycles in 120 frames log `Bitmap text cache budget too small (n styles live)` once.
  - The Text colour reaches styled face quads as vertex colours, with the component colour's alpha
    as the draw alpha, so a colour change rewrites vertex colours but never the cache (D5).
- **R13 Layout.**
  - Line breaking, word wrap at `fixedWidth`, `multiline`, pivots (`pivotBaseline`,
    `pivotCentered`) and `charPositions`/`charExtents` (caret, selection) behave as with vector
    text.
  - Advances = glyph advance + spacing + kerning(prev, cur), all whole font pixels.
  - Lines are `line_height` apart, and the baseline is the font's.
  - Text Effects, `visibleCharacters` and Input Field carets work unchanged on the per-glyph quads
    (both layers of a styled glyph carry the same character index in `glyphInfo`).
- **R14 Fallback.**
  - A codepoint the bitmap font lacks is looked up in its `fallback_fonts`, then in the Text's own
    fallback fonts that are bitmap fonts of the same native size, else drawn as the fallback glyph.
  - A Text never mixes a bitmap font with a vector fallback: a vector font in its fallback list is
    skipped, and S42 reports it.
- **R15 HTML UI** (`@font-face { font-family: Hud; src: url(fonts/hud.bitmapfont); }`, or `.fnt`):
  - `RmlFontEngine::LoadFontFace` recognises bitmap fonts and registers a bitmap face for the family.
  - `font-size` maps to `k` as R7. Glyphs are rasterised into the engine's pages at
    `d = max(1, round(k × rasterScale))` device pixels per font pixel, nearest-replicated:
    monochrome into R8 base pages, coloured into the RGBA8 effect pages. The pages fall under S16's
    budget and LRU rule (S16 R13).
  - S16's exact-scale and MSDF paths (R14-R16) skip bitmap faces: under a CSS transform the bitmap
    is magnified nearest, never re-rasterised smooth.
  - `font-effect: outline(1px <colour>)` and a sharp `text-shadow` map to R10, in font pixels.
  - Glow, blur and blurred `text-shadow` on a bitmap face are dropped, and `jasperengine-editor check
    --ui` reports each one with its selector.
  - Layout metrics (`line-height: normal`, baseline, x-height = baseline − top of `x`) come from the
    font.
- **R16 Built-in pixel font** `builtin:pixel`.
  - 5×7 glyphs in 6×9 cells, original to this project (no glyph traced or copied from another font),
    released under the engine's licence.
  - It covers printable ASCII and the Latin-1 letters U+00C0–U+00FF, the latter composed at build
    time from the base letters and 5 accent marks.
  - It is used by S48's contact-sheet labels and is available to games
    (`text.font = "builtin:pixel"`).

### Platform & backends

- **R17** All drawing uses existing UI variants (R9) and sokol textures (R8 and RGBA8, NEAREST), so
  every path runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan with no new shader. The parsers
  and the rasteriser are plain C++ (stb_truetype in tree), building with Emscripten and the Android
  NDK.
- **R18** Atlas pages stay ≤ 2048² (under the 4096 limit GLES3/WebGL2 guarantee, `STBText.h:58-59`).

### Performance (budgets)

- **R19 Load:**
  - a 95-glyph grid font loads (parse, measure, pack) in ≤ 2 ms;
  - a 256-glyph TTF pixel font in ≤ 15 ms;
  - a cooked `.jfont` in ≤ 1 ms (desktop release, build server);
  - all on the main thread before the first use, or on S06's I/O lane when preloaded with the scene.
- **R20 Draw and update:**
  - a 200-character Text rebuilds its mesh in ≤ 0.05 ms;
  - a styled rebuild with a cache hit costs the same;
  - a cache miss rasterises ≤ 0.01 ms per glyph;
  - a Text is one draw (unstyled) or one draw over one cache page (styled), as today's single-texture
    UI draw;
  - memory: a 95-glyph 6×9 monochrome font atlas is ≤ 64 KiB.
- **R21 HTML UI:** a document with bitmap text at 3 sizes uses no more pages than the same glyph
  count of vector text, and adds no draws (S16 R21's budgets still hold).

### Editor UX (human)

- **R22 Text Inspector** (`InspectorWindow.cpp:991`) with a bitmap font:
  - Font Size becomes a stepper over multiples of the native size, labelled `18 (9 × 2)`.
  - Outline Width is a 0/1/2 combo, plus Neighbors (4 or 8).
  - Shadow Offset uses whole pixels; Softness and Dilate are hidden.
  - A one-line note shows the native size, scale and effective device pixels.
  - Every edit is one undo step (`PropertyCmd`).
- **R23 Bitmap Font Inspector** (`drawBitmapFontAsset`, opened by selecting a `.bitmapfont` or
  `.fnt`):
  - **Source**: type, file pickers, cell size, margin, gap, a **Detect** button (file name, then
    gutters), characters (string or per row) and threshold or pixel size for TTF with **Detect**.
  - **Glyph grid**: the sheet at an integer zoom (1–8×, NEAREST), cell lines and each cell's
    character label. Hovering shows the glyph's codepoint, advance, offset and kerning partners.
  - Clicking a cell selects it. The side panel edits its character, advance override (with "Auto: 5"
    shown) and offset.
  - A drag on a cell's right edge sets the advance.
  - Keyboard: arrows move the selection; typing a character assigns it.
  - **Metrics**: spacing, space width, line height, baseline (a draggable baseline line drawn across
    the grid), fallback, colored, alpha cutoff.
  - **Kerning**: a table of pairs with add and remove, and a pair preview.
  - **Preview**: a text field (default "The quick brown fox 0123456789"), scale 1–8, backdrop
    (checker, dark, light) and the outline and shadow of R10.
  - **Report**: R6's numbers, with warnings as clickable rows (a click selects the glyph).
  - Every edit writes the `.bitmapfont` through the asset-edit command (one undo step, `WriteFilesCmd`
    with a merge key for drags). A `.fnt` used directly shows **Create Bitmap Font to edit**.
- **R24 Creation:**
  - The Project window's Create › Bitmap Font on a selected PNG, `.fnt` or TTF/OTF writes
    `<name>.bitmapfont` with detected settings and selects it.
  - A PNG named `*@WxH.png` shows a one-click banner in its texture Inspector: "This looks like a
    font sheet: Create Bitmap Font".
  - A TTF whose native pixel size is detected shows "Pixel font detected (8 px): Create Bitmap Font".
- **R25 Scene view and hierarchy:**
  - Dropping a `.bitmapfont` or `.fnt` on a Text sets its font (existing drop, `SceneWindow.cpp:699`)
    and snaps its font size to the nearest multiple, as one undo step.
  - GameObject › UI › Text (Pixel) creates a Text with `builtin:pixel` at ×1.

### Agent UX (MCP, Lua, CLI)

- **R26 `view_asset` on a font** (core, extended):
  - Without `text`, it returns the glyph grid image: every glyph at the chosen `scale`, labelled
    with its character, in the built-in pixel font.
  - With `text` (≤ 256 characters, `\n` allowed), it returns the string drawn in the font at `scale`
    (default the largest integer that fits `max_size`), white on a dark checker, with the outline and
    shadow off.
  - Structured content: `{font: {kind: "bitmap"|"vector", native_size, line_height, baseline,
    glyphs, colored, scale, width, height, missing: ["é", ...]}}`.
  - It works for `.bitmapfont`, `.fnt`, `builtin:pixel` and `.ttf/.otf` (vector: smooth text at
    16 px × scale, so an agent can see why it fails S42).
- **R27 `get_asset_info` on a font** returns R6's report plus:
  - the source and its settings;
  - `kerning_pairs`, `overrides`;
  - `pixel_report: {antialiased: bool, partial_alpha_share, native_size}` (S42's input);
  - for a vector font, `pixel_grid_detected` (R4's detection) and the fix `"create a .bitmapfont with
    source truetype"`;
  - errors as typed `{kind, message, fix, field}`.
- **R28 `write_file` of a `.bitmapfont`** parses and imports it and returns R6's report (or the
  typed errors) in its result, as it does for `.anim`. An invalid file is still written (the editor
  shows the error), as for the other validated types.
- **R29 Lua:**
  - `BitmapFont.load(path) -> BitmapFontInfo` (nil, err on failure), with:
    - read-only properties `nativeSize`, `lineHeight`, `baseline`, `glyphCount`, `colored`;
    - `hasGlyph(char)`;
    - `measure(text [, scale]) -> width, height` (whole pixels, the same layout as R13);
    - `advance(char)`.
  - `Text.pixelScale`, `Text.effectiveFontSize` and `Text.outlineNeighbors` (property).
  - The existing `Text.font = "fonts/hud.bitmapfont"` works.
  - The Lua API data is regenerated by the build (`tools/lua_api`).
- **R30 Agent docs:**
  - `docs/mcp/guide/pixel-fonts.md` (`jasper://docs/pixel-fonts`):
    - the file format;
    - the create → preview → fix loop (`write_file`, `view_asset text`, `get_asset_info`);
    - scale rules;
    - HTML UI usage;
    - the S42 check.
  - One line (≤ 120 bytes) in the `## Pixel art` digest section of the agent guide (S41 creates
    it, S42 owns its wording).
- **R31 CLI:**
  - `jasperengine-editor call view_asset --path fonts/hud.bitmapfont --text "HP 99"` writes the PNG
    (S31's CLI image rule).
  - `jasperengine-editor check --pixel-art` reports bitmap-font import errors as FAIL findings of
    S42's `font_antialiasing` rule (S42 R10), with `check`'s exit codes (0, 1 errors, 2 usage).

### Data & compatibility

- **R32** No change to existing files. `TextComponent` gains `outlineNeighbors` (8), written only
  when not the default. Projects with TTF text render byte-identically (AC2). `bitmapTextCacheMiB` is
  a new project field (0 = default) with a default when missing, exported through `Generator.cpp`.
- **R33** `.bitmapfont` version 1 is lenient: unknown keys are kept and the writer is canonical
  (sorted keys inside `glyphs`, kerning sorted by pair). A newer version than the editor knows loads
  read-only with a warning.
- **R34 Export.**
  - Every `.bitmapfont` and `.fnt` a build ships is cooked to `<name>.jfont`: magic `JFNT`, version,
    metrics, glyph table, kerning and atlas pages (R8 or RGBA8, deflated). The runtime loads
    `.jfont` when present.
  - The source PNG and TTF are not shipped for that use. A PNG also used as a sprite ships as the
    texture it is, cooked by S14 as before.
  - A font sheet is never lossy-compressed, because the cooked font is lossless.
  - Old exports without `.jfont` still load from the source.

## 6. Design constraints & decisions

- **D1 A bitmap font is a Font, not a new asset type.** The ledger says "bitmap fonts use the
  existing font pipeline where possible". Every font slot, the asset reference schema, the GUID
  sidecar and hot reload (S05) already handle `AssetType::Font`, so the Font row gains extensions.
  The engine branches on the file's kind at load (`FontKind::Vector | Bitmap`), in one place:
  `FontLoader::kindOf(path)`.
- **D2 Runtime classes** in `engine/core/pixelart/` (ledger convention):
  - `BitmapFont` (glyph table, metrics, atlas pages, load from `.bitmapfont`/`.fnt`/`.jfont`);
  - `BitmapFontPool` (shared by path, hot-reloaded like `FontPool`);
  - `BitmapText` (layout and quads, the same outputs as `STBText::createText`: buffer, indices,
    `charPositions`, `charExtents`, `glyphInfo`, width and height);
  - `BitmapTextCache` (R12);
  - `BitmapFontParsers.{h,cpp}` (grid, BMFont text/XML/binary, TTF raster);
  - `FontPixelReport`.

  `UISystem::loadFontAtlas` and `createText` (`UISystem.cpp:307, 463`) branch on the kind.
  `STBText` is untouched.
- **D3 Integer scale lives in the mesh, not the texture.** Quads are `k` (or `d`) units per font
  pixel. The atlas is native-size only, so one atlas serves every size and the memory is the
  native glyphs once. The Text's `rasterScale`/`sdfPadding` (`TextComponent.h:60-61`) stay 1/0 for
  bitmap fonts.
- **D4 Snap rule from S41.** On the GPU, S41's UI vertex-stage bit snaps Text drawn by a Pixel
  Perfect Camera. On the CPU (no such camera, editor previews, `view_asset`, contact sheets),
  `pixelart::snap` from `engine/core/pixelart/PixelSnap.h` rounds the origin, and
  `UISystem::getTextRasterScale()` gives the canvas-to-device factor (R8). S51 adds no snap code of
  its own.
- **D5 Styled text = two quad layers + vertex colours.**
  - Back quads (outline and shadow, coloured in the cache) come first in the index buffer, then face
    quads.
  - The buffer carries a vertex colour (the Text Effect path's `vertexColor`,
    `TextComponent.h:62`): white on back quads, the Text colour on face quads.
  - The draw uses the component colour's alpha only (a `UIComponent` flag), so fades fade
    everything.
  - This needs no shader change, and order is correct for any offset.
- **D6 Why not whole-string caching** (spryte's `TextCache`): a Text component already keeps its
  mesh between frames, so caching the string saves nothing. A string image would break Text
  Effects, typewriter reveal, caret extents and per-glyph colour, and a score counter would
  re-raster every change. The cache helps exactly where pixels must be composed (outline and
  shadow), and spryte's paging and recycling rules apply to it unchanged.
- **D7 HTML UI** reuses the engine's page machinery: bitmap glyph runs are ordinary glyph
  geometry, so S16's batching and run cache keep working. A `FaceHandle` gains `bitmap` (the
  `BitmapFont`) and `GlyphKey::rasterPixelSize` holds `d`. Bitmap faces never enter the distance
  atlas.
- **D8 Detection is advisory.** Cell size, baseline and TTF pixel size detection fill the file once,
  at creation or on **Detect**. Loading never re-detects, so a saved font never changes when the
  heuristics improve.

**File example**

```json
{
  "format": "jasperengine.bitmapfont",
  "version": 1,
  "source": {"type": "grid", "image": "fonts/hud@6x9.png", "chars": "ascii"},
  "advance": "auto",
  "spacing": 1,
  "space_width": 3,
  "line_height": 10,
  "baseline": 7,
  "fallback": "?",
  "colored": "auto",
  "glyphs": {"i": {"advance": 2}, "j": {"offset": [-1, 0]}},
  "kerning": [["T", "o", -1], ["A", "V", -1]]
}
```

**API sketch**

```cpp
// engine/core/pixelart/BitmapFont.h
namespace jasperengine::pixelart {
    struct BitmapGlyph { uint16_t page, x, y, w, h; int16_t offsetX, offsetY; int16_t advance; };
    struct BitmapFontReport { int glyphs = 0, cellsUsed = 0, cellsEmpty = 0, nativeSize = 0, lineHeight = 0, baseline = 0;
        bool colored = false; float partialAlphaShare = 0.0f; std::vector<uint32_t> missingAscii, emptyGlyphs;
        std::vector<FontError> errors, warnings; };   // FontError {kind, message, fix, field}
    class JASPERENGINE_API BitmapFont {
    public:
        static std::shared_ptr<BitmapFont> load(const std::string& path, BitmapFontReport* report = nullptr);
        const BitmapGlyph* glyph(uint32_t codepoint) const;   // fallback chain applied
        int kerning(uint32_t left, uint32_t right) const;
        int getNativeSize() const; int getLineHeight() const; int getBaseline() const; int getSpacing() const;
        bool isColored() const; const std::vector<TextureData>& getPages() const;
        int measure(std::u32string_view line) const;           // whole font pixels
    };
    enum class FontKind { VECTOR, BITMAP };
    FontKind fontKindOf(const std::string& path);              // by extension and builtin: names
    struct FontPixelReport { FontKind kind; bool antialiased; float partialAlphaShare; int nativeSize; };
    FontPixelReport analyzeFontForPixelArt(const std::string& path);   // S42's input
}
```

```lua
local hud = BitmapFont.load("fonts/hud.bitmapfont")
local w, h = hud:measure("SCORE 120", 2)
label.font = "fonts/hud.bitmapfont"; label.fontSize = hud.nativeSize * 2
label.outlineWidth = 1; label.outlineNeighbors = 4; label.outlineColor = Vector4(0, 0, 0, 1)
print(label.pixelScale)   -- 2
```

### 6.1 MCP tools and the budget

S51 adds **no new tool**. It extends three core tools (conventions §Checklist 1: "Prefer extending
a tool"); the growth counts against the default listing (≤ 100,000 characters, measured by
`tool_budget.py --check`).

| Tool | Group | Change | Budget (chars added) |
|---|---|---|---|
| `view_asset` | core | `text` (string, ≤ 256), `scale` (integer 1..16); description: "+ fonts: the glyph grid, or `text` drawn at whole `scale`" | ≤ 170 |
| `get_asset_info` | core | description lists "font metrics, glyphs, pixel report" | ≤ 45 |
| `write_file` | core | description adds ".bitmapfont" to the validated types | ≤ 15 |
| guide | — | ≤ 120 bytes in the `## Pixel art` digest section | — |

The core listing grows by ≤ 230 characters and no tool. If the listing has no room when S51 lands,
the fallback is to move font previews into S48's spare `pixel_art` slot as `preview_font` (the same
parameters, ≈ 450 characters), which takes the group to 7 tools and ≈ 8,500 characters, still within
12 and 15,000 (ledger "MCP budget"), and leave the core tools unchanged. That choice is recorded in
Deviations.

## 7. Acceptance criteria

| AC | Criterion | Verification |
|---|---|---|
| AC1 | Grid parsing: `@WxH` names, `cell`, `margin`, `gap`, `chars`, `rows`, U+FFFF skips; overflow gives the typed error with counts; gutter detection finds 6×9 on a sheet with 1 px gutters and reports it | gtest `BitmapFontParsers.Grid*` (`engine/tests/core/pixelart/`) with fixtures in `engine/tests/core/fixtures/fonts/` |
| AC2 | Old projects: every scene with TTF text in `tools/fixtures/*` renders pixel-identical (tolerance 0) to the pre-S51 reference and re-saves byte-identically | `tools/mcp/tests/test_pixel_fonts.py::test_vector_text_unchanged` (references captured before the change: the red control) |
| AC3 | Auto advances equal rightmost opaque column + 1 for 95 ASCII glyphs of the fixture (table compare); `trim: both`, `monospace`, integer advance and per-glyph overrides apply; space width and line height defaults match spryte's | gtest `BitmapFont.Metrics*` |
| AC4 | BMFont text, XML and binary of the same font (generated fixtures) load to identical glyph tables, kerning and metrics; bad `chnl`, missing page and truncated binary give typed errors | gtest `BitmapFontParsers.BMFont*` |
| AC5 | TTF: a pixel TTF fixture built on an 8 px grid (made by a test script from a known bitmap) is detected as 8 px and rasterises to the original bitmap exactly; a smooth TTF (Roboto) reports `font_pixel_size_unknown`; `threshold` changes the result predictably; output has no partial alpha | gtest `BitmapFontParsers.TrueType*` |
| AC6 | Determinism: importing the same source twice gives byte-identical `.jfont` and reports | gtest `BitmapFont.Deterministic` |
| AC7 | Integer scale: `fontSize` 18 on a 9 px font gives `pixelScale` 2; 16 gives 2; 4 gives 1; every font pixel is k×k output pixels in a golden at scales 1, 2, 3 | gtest `BitmapText.Scale`; golden `test_pixel_fonts.py::test_blocks_uniform` |
| AC8 | Snapping: a world Text moved by 0.3 art px per frame for 10 frames lands on whole art pixels each frame (S41 grid) and never changes its glyph shapes; UI text at Canvas Scaler 1.37 draws every font pixel as a whole number of device pixels | `test_pixel_fonts.py::test_world_snap`, `::test_ui_fractional_scale` |
| AC9 | Sampling: bitmap text textures are NEAREST without mips (asserted on the bound texture); no SDF atlas is created for a bitmap Text with outline or shadow (`FontPool` contents) | gtest `BitmapText.NoSdf`; `test_pixel_fonts.py::test_nearest` |
| AC10 | Outline and shadow: goldens of outline 1/2 with 4 and 8 neighbours, shadow (1, 1) and (−2, 0), coloured and monochrome fonts, at scale 3; no back pixel covers a neighbouring glyph's face (pixel check); colour change keeps the cache hit count and rewrites only vertex colours | golden `test_pixel_fonts.py::test_styles`; gtest `BitmapTextCache.ColourChangeNoRebuild` |
| AC11 | Cache recycling: with a 256 KiB budget, cycling 12 styles across texts recycles pages only when unused this frame and the last; no glyph is missing in any drawn frame; the diagnostic logs once | gtest `BitmapTextCache.Recycle` |
| AC12 | Layout: wrapping at `fixedWidth`, newlines, pivots, kerning and caret extents match a hand-computed layout for 5 strings; Text Effect wave and `visibleCharacters` work on bitmap text (both layers of a styled glyph move together) | gtest `BitmapText.Layout*`; `test_pixel_fonts.py::test_text_effect` |
| AC13 | Fallback: missing glyph → `fallback_fonts` → same-size bitmap fallbacks of the Text → fallback glyph; a vector font in the Text's fallbacks is skipped | gtest `BitmapText.Fallback` |
| AC14 | HTML UI: `@font-face` with `.bitmapfont` and `.fnt` renders goldens at font-size 1×, 2×, 3× native and pixel ratio 1 and 1.5 (every font pixel whole device pixels); `font-effect: outline(1px)` and sharp `text-shadow` render pixel styles; a glow on a bitmap face is reported by `check --ui`; S16's budgets hold | `tools/build.sh htmlui-tests` new `TestBitmapFonts.*` goldens (GL core and Vulkan); `check --ui` fixture in `tools/fixtures/pixel-fonts/` |
| AC15 | Built-in font: `builtin:pixel` covers ASCII and U+00C0–U+00FF, 5×7 glyphs in 6×9 cells, and its sheet is generated from the in-repo glyph source (no external font file) | gtest `BitmapFont.BuiltIn`; review checklist for originality |
| AC16 | `view_asset` on a `.bitmapfont` without `text` returns the glyph grid; with `text` the string at the given scale with `missing` listing absent characters; on a TTF a smooth preview; the CLI writes the PNG | `tools/mcp/tests/test_pixel_fonts.py::test_view_asset`, `test_call_cli.py` case |
| AC17 | `get_asset_info` returns R6/R27's fields; an invalid file returns typed errors with `field`; `write_file` of a `.bitmapfont` returns the import report | `test_pixel_fonts.py::test_asset_info`, `::test_write_file` |
| AC18 | Inspector: glyph grid with labels, a click selects a glyph, advance drag writes the override as one undo step (merged while dragging), baseline drag, kerning add/remove, preview; Create › Bitmap Font on a PNG, `.fnt` and TTF; the `@WxH` banner; screenshots from an isolated editor | `tools/mcp/tests/test_pixel_fonts_inspector_ui.py` (`find_ui_item`, `screenshot`, `undo`) |
| AC19 | Text Inspector: font size stepper in multiples, outline combo, hidden softness/dilate rows, drop on a Text snaps the size in one undo step | `test_pixel_fonts_inspector_ui.py::test_text_inspector` |
| AC20 | Lua: `BitmapFont.load`, `measure` equals the drawn Text's width, `hasGlyph`, `Text.pixelScale`; bad path returns `nil, err` | `tools/mcp/tests/test_pixel_fonts.py::test_lua` (play, `eval_lua`, console) |
| AC21 | S42 input: `analyzeFontForPixelArt` reports a TTF as vector and AA, a clean grid font as clean, a sheet with partial alpha with its share; with `alpha_cutoff 128` the share is 0 | gtest `FontPixelReport.*` |
| AC22 | Export: `.jfont` cooked for every shipped bitmap font; the exported game draws the same pixels as the editor (tolerance 0 at scale 2) on GL, Vulkan, GLES3 and web; the source PNG is not shipped unless used elsewhere; `.jfont` load ≤ 1 ms | `tools/smoke-test.sh tools/fixtures/pixel-fonts` (+ `SMOKE_BACKEND=vulkan|gles3`, `tools/build.sh web-gate`); gtest `BitmapFont.JfontRoundTrip`, `BitmapFontBench.*` |
| AC23 | Hot reload: editing the sheet PNG or the `.bitmapfont` on disk updates every Text using it within a frame of the file watcher event (S05) | `test_pixel_fonts.py::test_hot_reload` |
| AC24 | Performance budgets R19-R20 on the build server (release) | gtest `BitmapFontBench.*` (logged and gated) |

## 8. Dependencies & conflicts

- **Depends on:**
  - S41 (wave 5A, landed first): `PixelSnap.h` (`snap`, `snapWorld`), the UI vertex-stage snap
    bit and the PPU (D4, R8). Without an active Pixel Perfect Camera, R8's CPU rules apply.
  - S16: the HTML UI page budget, LRU and the transform-aware text paths that bitmap faces must skip
    (R15). Already merged.
  - S05: hot reload of `.bitmapfont`, its sources and `.fnt` pages (AC23).
  - S04: GUIDs of the new Font extensions (sidecar, as TTF).
  - S14: the exporter's cooking hooks (R34): font sheets bypass texture cooking.
  - S06: the I/O lane for preloading (soft).
- **Feeds:**
  - S42: `analyzeFontForPixelArt` for its `font_antialiasing` rule (S42 R10: WARN for a vector
    font, partial alpha, a size that is not a whole multiple, or a vector fallback; FAIL for a font
    that does not load) and its fixes:
    - Create Bitmap Font from this TTF;
    - set `alpha_cutoff`;
    - snap the font size;
    - remove the vector fallback.
  - S48: `builtin:pixel` for contact-sheet labels.
  - S47: agents drawing a font sheet with S47's tools preview it with `view_asset text`.
- **Merge hotspots:**
  - `engine/core/subsystem/UISystem.cpp` (text loading and building);
  - `engine/core/component/TextComponent.h`, `engine/core/object/ui/Text.{h,cpp}`;
  - `engine/core/htmlui/RmlFontEngine.{h,cpp}` (S16's area; coordinate with any S16 follow-up);
  - `editor/asset/AssetTypes.cpp` (every asset spec);
  - `editor/window/InspectorWindow.{h,cpp}`;
  - `editor/mcp/tools/AssetTools.cpp` (`view_asset`, `get_asset_info`, `write_file`: S44-S47 extend
    the same tools for sprites);
  - `editor/Generator.cpp` and the exporter;
  - `GameObjectClassesLua.cpp`.

## 9. Risks & defaults

| Risk / question | Default decision |
|---|---|
| `fontSize` snapping surprises users who type 16 for a 9 px font | The Inspector shows `18 (9 × 2)` and steps in multiples; Lua/MCP values round to the nearest multiple with a development-build note; `effectiveFontSize` is readable |
| UI text at a fractional Canvas Scaler is slightly off its nominal size (R8) | Crispness wins in a pixel-art font; S41's pixel-perfect UI makes it exact; documented |
| TTF native size detection misfires on fonts with off-grid outlines (accents, hinting artefacts) | 90 % threshold, reported candidates, never silent; the user can set `pixel_size`; detection only fills the file (D8) |
| Core MCP listing has no room for 230 characters | Fallback `preview_font` in `pixel_art` (§6.1), recorded as a deviation |
| Styled cache churn with many distinct outline colours | Style keys are per colour; the budget and recycling bound memory; the diagnostic names the live style count |
| HTML UI bitmap faces conflict with S16 follow-ups in `RmlFontEngine` | Bitmap faces are a separate branch keyed by `FaceHandle::bitmap`; S16's tests (AC1 of S16 with S51 off) stay green |
| BMFont files referencing pages outside the project | Typed error with the path and a fix ("copy the page next to the .fnt") |
| Complex scripts in bitmap fonts | Out of scope; codepoints map 1:1 and missing ones draw the fallback; documented |

## Milestones

1. **M1 Core bitmap fonts.**
   - `BitmapFont` with grid and BMFont parsers, metrics, report and atlas;
   - `BitmapText` layout;
   - Text component rendering at integer scale with snapping, NEAREST;
   - Font row extensions;
   - basic Inspector (source and metrics);
   - AC1-AC4, AC6-AC9, AC12, AC13.
2. **M2 Pixel TTF and styles.** TTF detection and raster, the styled glyph cache with outline and
   shadow, the full character-map Inspector, kerning, Create › Bitmap Font and banners, the Text
   Inspector; AC5, AC10, AC11, AC18, AC19.
3. **M3 Agents and the built-in font.**
   - `builtin:pixel`;
   - `view_asset`, `get_asset_info` and `write_file` extensions;
   - Lua `BitmapFont`;
   - `FontPixelReport` for S42;
   - docs and the guide line;
   - AC15-AC17, AC20, AC21.
4. **M4 HTML UI and export.** `@font-face` bitmap faces, `.jfont` cooking, hot reload, smoke and
   web; AC14, AC22-AC24.

## Deviations


Recorded by the implementation (branch `feature/s51-pixel-fonts`).

1. **The cooked file's name (R34).** A shipped font is cooked to `<file>.jfont` next to it
   (`fonts/hud.bitmapfont.jfont`, `fonts/hud.fnt.jfont`), not `<name>.jfont`: a `.bitmapfont`
   wrapping a `.fnt` of the same name (the usual pair) would otherwise cook both to one file. The
   cooked font replaces its source in the build: the `.bitmapfont` / `.fnt`, its sheet, pages and
   TrueType file are not shipped unless an asset references them (a sheet also drawn as a sprite
   ships as that texture). The runtime's `BitmapFont::load(path)` reads `path + ".jfont"` first.
2. **The built-in font (R16).** Its design is the engine's own: square corners and bowls with
   one-pixel strokes, 5x7 glyphs in 6x9 cells (the glyph source is
   `engine/core/pixelart/BuiltinPixelFont.cpp`). `BitmapFont.BuiltInIsOriginal` holds the classic
   5x7 character-LCD table as the reference it must not copy and requires three quarters of the 94
   printable ASCII glyphs to differ from it by 3 pixels or more (82 % do; the old rounded design,
   30 %, fails it). The accented letters are composed from the base letter and five marks (grave,
   acute, circumflex, tilde, diaeresis): a capital keeps rows 0, 2, 3, 4 and 6 of its 7 and stands
   under the mark; a lowercase letter loses what it has above row 2 (the dot of i). The 15 letters no
   mark makes (Å å Æ æ Ç ç Ð ð × ÷ Ø ø Þ þ ß) are drawn as glyphs of their own.
3. **The agent guide line (R30).** The guide has no `## Pixel art` section and no byte left (S41
   deviation 26 cut its "Other areas" line): S51 adds the page `docs/mcp/guide/pixel-fonts.md`
   (`jasper://docs/pixel-fonts`, listed in the guide README) and its tools' text, and no guide line.
4. **Fallback fonts (R14).** A `.bitmapfont`'s `fallback_fonts` join its own atlas at load, and a
   Text's same-size bitmap fallbacks are merged with its font into one chain font (pooled by the
   chain's paths): a Text always draws from one texture.
5. **Several atlas pages (R11).** A font whose glyphs need more than one 2048² page draws through the
   styled glyph cache (its face layer only), so a Text still binds one texture.
6. **The styled glyph cache's pages (R12).** Pages start at 128² RGBA8 and grow to the size one
   Text's glyphs need (up to 2048²); a page holds several styles. Past the budget, when every page
   was drawn in this frame or the last, a page is made anyway (no glyph goes missing) and the
   diagnostic is logged once.
7. **CPU origin snap (R8).** Without a Pixel Perfect Camera's snap, a bitmap Text's draw rounds its
   origin onto the pass target's pixel (`pixelart::snapCenterDelta` on its MVP), so bitmap Texts are
   not batched by the dynamic batcher (`ineligible_transform`), as S41's snapped draws are not.
8. **Text Inspector (R22).** The bitmap rows (Font Size stepper with Snap, Outline Width 0/1/2,
   Neighbors, a whole-pixel Shadow Offset, the note) are a "Pixel Font" section after the Text's own
   rows; with a bitmap font the vector Font Size, Outline Width, Shadow Offset, Softness and Dilate
   rows are hidden.
9. **Bitmap Font Inspector (R23).** The glyph grid, its cell editing, advance and baseline drags
   apply to grid sources; a BMFont or TrueType source lists its glyphs, and a selected one takes the
   same advance and offset overrides. The Inspector also opens for a `.ttf`/`.otf`, where R24's
   "Pixel font detected" button lives.
10. **HTML UI (R15).** `font-effect: outline(Npx ...)` reads N as font pixels (1 or 2) and a
    `font-effect: shadow` offset as font pixels; a CSS `text-shadow` keeps its offset in CSS pixels
    (RmlUi applies it before the font engine sees the text), drawn sharp; HTML text effects
    (`HtmlTextEffects`) do not move bitmap faces. `check --ui` reports
    `HTMLUI-CSS-0024 bitmap-font-effect` for a rule that sets a bitmap family and a glow, a blur or
    a blurred `text-shadow` in the same block (compat reads no cascade).
11. **S42's command (R31).** `check --pixel-art` and its rule registry belong to S42, which is
    not merged: S42's `editor/pixelart/PixelArtCheck.h` (its worktree) reads fonts through
    `setFontAnalyzer(FontAnalyzer)`, returning its `PixelArtFontInfo`. S51 provides the data
    (`pixelart::analyzeFontForPixelArt`, `analyzeTextForPixelArt`, `get_asset_info`'s
    `pixel_report`); when both land, the editor registers an analyzer mapping `FontPixelReport` to
    `PixelArtFontInfo` field for field (kind, nativeSize, antialiased, partialAlphaShare,
    pixelGridDetected, loads = !loadError with its first error; `TextPixelReport` gives
    pixelScale and vectorFallbacks).
12. **TrueType metrics (R4, R6).** The native size of a TrueType source is its ascender plus
    descender at the detected size (8 for an 8 px font of ascender 7, descender 1), its baseline the
    ascender and its line height the native size + 1 (as a grid's) unless the font has a line gap.
13. **World text and Pixels Per Unit (R8).** A Text drawn by a non-UI camera with a project Pixels
    Per Unit above 1 takes k / PPU units per font pixel: its `fontSize` stays in pixels.
14. **`preview_font` (R26-R28, §6.1's fallback).** The core listing had one character of room, so
    font previews are the `preview_font` tool in the `pixel_art` group (its row added here; S42 and
    S43 add their tools to it), with R26's parameters (`text`, `scale`, `max_size`), and
    `view_asset`, `get_asset_info` and `write_file` keep main's text: `view_asset` on a font answers
    with an error naming `preview_font`, `get_asset_info` and `write_file` return the font report
    with no description change. A preview never exceeds `max_size`: an explicit `scale` that does
    fails naming the largest that fits, and the glyph grid shows the glyphs that fit (it says how
    many). The guide's "Unity names -> engine names" heading is "Unity names" (the guide's headroom
    for the group's name).
15. **R20's styled rebuild is over budget.** A styled Text draws a back quad and a face quad per
    glyph, both with vertex colours, so its rebuild writes about three times the plain rebuild's
    vertex data. On a 16-vCPU build box (release) a 200-character rebuild takes 0.051 ms; a styled
    one with a cache hit takes 0.134 ms, against a budget of 0.05 ms. `BitmapFontBench` logs both
    and gates them only under `JASPER_TEST_PERF_SLACK`. Closing the gap needs a quad writer that
    fills the interleaved buffer directly, not through `Buffer::addVector*`.
16. **BMFont channels (R3).** `chnl=0` reads as all channels, as `chnl=15`: libGDX Hiero writes 0
    (fixture `engine/tests/core/fixtures/fonts/bm_hiero.fnt`).
17. **Scene preload (R19).** A compiled game's scene resources list its Texts' bitmap fonts
    (`SceneResources::bitmapFonts`), and `preloadScene` loads them into `BitmapFontPool` on its
    main-thread step, one per frame when loading is synchronous (a cooked `.jfont` reads in about
    0.01 ms; a font has no I/O-lane loader). The cooked scene's resource list (the prebuilt player)
    is a shared format S51 does not bump: there a font loads at its first use.
18. **HTML UI pages (R15).** A bitmap face's glyphs go to their own glyph pages (`PageKind::PIXEL`
    and `PIXEL_EFFECT`, in the base budget), sampled NEAREST, so a CSS transform magnifies their
    pixels as blocks; vector glyphs keep S16's filtered pages.
