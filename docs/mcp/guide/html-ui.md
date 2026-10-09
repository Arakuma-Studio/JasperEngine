# HTML/CSS UI (UI Document)

Served as `jasper://docs/html-ui`.

A **UI Document** (`UIDocumentComponent`, YAML/MCP name `uidocument`, Unity's UIDocument) shows
an HTML file over the scene, or on a mesh, laid out and drawn by the engine (RmlUi 6.3 inside,
drawn on the engine's own renderer on every backend: no browser, no JavaScript). It coexists
with the component UI (Text, Image, Button, ...).

This page describes **phase 1**: the complete renderer (rounded clipping, filters, backdrop
filters, gradients, shadows, masks, transforms, MSAA), complete input (mouse, wheel, touch,
keyboard, text and IME, clipboard, cursor), the editor integration with hot reload, the Lua and
C++ element API with data binding, SVG and Lottie, an Anime.js-style animation API, documents
rendered into textures for meshes, and text effects. Documents are still XML-well-formed HTML
styled with **RCSS**, RmlUi's dialect of CSS: standard HTML5 parsing and CSS rewriting come with
phase 2, so for now write the subset below exactly as shown. What is missing is listed under
"Not yet, and limits".

## Quick start (MCP)

```text
write_file  assets/ui/menu.html   (the document)
write_file  assets/ui/menu.css    (its stylesheet)
create_entity  scene=<UI scene>  type=empty  name=Menu
add_component  entity=Menu  component=UIDocument  values={"uidocument": {"filename": "assets/ui/menu.html"}}
screenshot  view=game
```

```html
<html>
<head>
    <title>menu</title>
    <link type="text/css" href="menu.css"/>
</head>
<body>
    <div id="panel">
        <div id="title">My Game</div>
        <div id="play" class="button">Play</div>
    </div>
</body>
</html>
```

```css
/* RCSS: no default stylesheet, so block elements say so */
body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 16px; color: #ffffff; }
div { display: block; }
#panel { position: absolute; left: 40px; top: 40px; width: 300px; padding: 20px;
         display: flex; flex-direction: column; gap: 12px;
         background-color: #20202ce0; border: 2px #5060a0; border-radius: 12px; }
#title { font-size: 32px; }
.button { height: 48px; line-height: 48px; text-align: center; color: #000000; background-color: #808080; }
.button:hover { background-color: #00ff00; }
.button:active { background-color: #008000; }
```

## The component

| Property | Type | Meaning |
|---|---|---|
| `filename` | string | The `.html` document. MCP takes and returns project-relative paths under the assets directory (`assets/ui/menu.html`); the scene stores the asset-relative path (`ui/menu.html`). Empty = no document. Inspector label "Source". |
| `sortOrder` | int | Order of the scene's documents: higher draws on top (default 0). |
| `enabled` | bool | Shown and receiving pointer input (the Inspector's checkbox). |
| `interactive` | bool | Receives pointer, keyboard and text input (default true). Off: every event goes to the game. |
| `renderMode` | enum | `screen` (default: drawn over the scene) or `texture` (rendered into a texture a material, `Mesh` or `Image` shows). |
| `textureWidth`, `textureHeight` | int | Size of the texture in `texture` mode, 1..8192 (default 1024 x 1024). |
| `inputSurface` | entity | `texture` mode: the mesh whose UVs receive the pointer; none = the document's own GameObject. |
| `pixelRatio` | float | Document pixels per CSS pixel; 0 = automatic (the canvas-to-screen scale; 1 in `texture` mode). |

The YAML key is `uidocument`; the phase-1 keys are written only when they differ from their
defaults. `create_entity type=ui_document` makes a GameObject with a Transform and the
component (the Hierarchy's *Create > UI > UI Document*).

- One document per component. All documents of a scene share one layout context: they are
  laid out on the project canvas (`canvasWidth` x `canvasHeight`), CSS `px` = canvas pixels.
  With a scaled canvas (letterbox, crop, high-DPI screens) the document is drawn at screen
  resolution, so text and edges stay sharp.
- Drawn over the scene in the scene's main camera pass, **in front of the component UI**, in
  the Game view (edit mode and play mode) and in exported games. A scene stacked as a layer
  draws its documents over the scenes below it.
- The Scene view of UI and 2D scenes shows a scene's documents in place on the canvas
  rectangle (outlined while a UI Document is selected); documents are selected in the
  Hierarchy, not by clicking them. The Scene view of a 3D scene has no canvas: its documents
  show in the Game view.
- Saved with the scene, copied with copy/paste, duplicate and prefabs (with overrides),
  exported with the game (every file under the assets directory ships, `.html`/`.css`
  included; the export checks the references first, see "Editor workflow").
- A missing or unreadable file logs `[HTML UI] could not load document '<path>'` to the
  Console and draws nothing; nothing crashes. Every HTML UI Console line starts with
  `[HTML UI]`: read the Console after changing a document.
- Editing: save the file (`write_file`, the code editor, any external editor): the document
  reloads by itself within a quarter of a second, in edit mode and while playing (see "Hot
  reload").

## Files and paths

- Keep documents, stylesheets, images and fonts under the assets directory (`assets/ui/...`).
- References inside a document are relative to the document (`href="menu.css"`,
  `src="../images/logo.png"`); `asset://ui/logo.png` is relative to the assets directory.
  The game never uses the network: an `http(s)://` resource (Google Fonts, a CDN stylesheet,
  a remote image or Lottie animation) is imported into the project by the editor first (see
  "Remote resources (imports)").
- Images: PNG, JPG (and the other formats the engine loads) and SVG files (LunaSVG, see
  "SVG and Lottie"); Lottie animations as `.json` or `.lottie`.

## Writing HTML/CSS for the engine

Documents are **standard HTML5 and CSS**, written as for a browser (phase 2). The engine parses
the HTML like a browser does (no XML strictness: unclosed `<p>`, `<br>`, unquoted attributes,
`&nbsp;` and every named entity are fine), applies a **user-agent stylesheet** equivalent to
Chromium's defaults, and rewrites the CSS for its layout engine. A page written for Chrome at
the canvas size looks the same in the engine, within the limits listed below: the golden
pages of the engine's test suite (a main menu, a HUD, a settings screen, a confirm dialog,
unstyled HTML, a gallery of 48 CSS rewrites) are compared pixel by pixel and box by box with
Chromium 153.

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@700&family=Inter:wght@400;600&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="menu.css">
</head>
<body>
  <div class="panel">
    <h1>Void Chronicles</h1>
    <button class="btn primary"><i data-lucide="play"></i> Continue</button>
    <button class="btn"><i data-lucide="settings"></i> Settings</button>
  </div>
  <script type="text/lua">
    document:querySelector(".btn.primary"):addEventListener("click", function() print("continue") end)
  </script>
</body>
</html>
```

```css
:root { --primary: #7c3aed; --glass: rgba(255, 255, 255, 0.08); }
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: 'Inter', sans-serif; min-height: 100vh; display: flex; align-items: center;
       justify-content: center; background: linear-gradient(135deg, #0f0c29, #302b63); color: #fff; }
.panel { width: 420px; padding: 40px; background: var(--glass); backdrop-filter: blur(12px);
         border: 1px solid rgba(255, 255, 255, 0.18); border-radius: 16px; box-shadow: 0 8px 32px rgba(0, 0, 0, 0.37); }
h1 { font-family: 'Orbitron', sans-serif; text-shadow: 0 0 20px rgba(124, 58, 237, 0.8); }
.btn { width: 100%; padding: 12px 20px; border: 1px solid rgba(255, 255, 255, 0.15); border-radius: 10px;
       background: rgba(255, 255, 255, 0.06); color: #fff; font: inherit; transition: all .3s ease; }
.btn:hover { background: linear-gradient(90deg, var(--primary), #4f46e5); }
.btn.primary { background: linear-gradient(135deg, var(--primary), #4f46e5); }
```

### The page

- `<html>` is the document's root element and `<body>` its child, as in a browser: `html {}`,
  `:root {}`, `body {}`, `rem` and `html { font-size: 62.5% }` behave as in CSS. The page
  covers the canvas (CSS `px` = canvas pixels, `100vw` x `100vh` = the canvas); media queries
  see the canvas size.
- The background is **transparent**, not white: the UI draws over the game. Give `html` or
  `body` a background for a full-screen menu (a `body` background covers the whole canvas, as
  in CSS).
- Default styles come from the user-agent stylesheet: block and inline display, heading sizes
  and margins, paragraph and list margins, `body { margin: 8px }`, list markers, tables, links
  (`#0000ee`, underlined), and form controls that look like Chromium's light theme (buttons,
  text fields, checkboxes and radios in the accent color, sliders, `<select>`, `<progress>`),
  classic 15 px scrollbars and a focus ring for keyboard focus. Every rule you write beats
  every user-agent rule, whatever its specificity, as in a browser.
- `accent-color` colors checkboxes, radios, sliders and progress bars (a light accent gets a
  dark check mark, as in Chrome); `appearance: none` removes a control's own look so your
  `background`, `border` and `::-webkit-slider-thumb` styles draw alone. As in a browser, a
  reset such as `* { margin: 0; padding: 0; box-sizing: border-box }` does not reach a
  control's inner parts (slider track and thumb, select arrow, scrollbars): style those
  through their pseudo-elements.
- `:focus` and `:focus-visible` match the focused element only (`:focus-within` its
  ancestors too); Tab moves the focus through buttons, links, inputs and `tabindex="0"`.

### Scripts, icons, remote files

- **No JavaScript.** `<script>` with JavaScript is not run (an error diagnostic says so, with a
  tailored hint for known libraries); `on*` attributes too. Use `<script type="text/lua">`
  (inline or `src="menu.lua"`): it runs after the document loads with `document` bound to the
  `UIDocument` and the element API of "Scripting" (`querySelector`, `addEventListener`,
  `classList`, `style`, ...), and Lua errors point at the HTML line. `<noscript>` content shows.
- **Lucide icons are built in**: `<i data-lucide="play"></i>` becomes the inline SVG
  `lucide.createIcons()` would make, colored by the CSS `color` (`currentColor`), sized by
  CSS `width`/`height`. The Lucide `<script>` is not needed (an info says so).
- **Remote files** (Google Fonts, CDN stylesheets, images, LottieFiles) are never fetched at
  runtime. The editor imports them into `assets/htmlui-imports/` (fonts as static TTF, with
  their license files) and writes `assets/htmlui-imports/manifest.json`, which the runtime
  reads to serve each URL from the project: keep the `https://` URLs in your HTML/CSS as they
  are. Imports run automatically when a document loads in the editor; MCP `import_files` and
  `jasperengine-editor import-ui --project <dir> <html>` run them on demand. A remote URL that
  was not imported is an `HTMLUI-IMP-0001` error and falls back (fonts: the next family;
  images: not drawn).
- Fonts: `@font-face` with TTF/OTF `url()`s (WOFF/WOFF2 sources are skipped; an imported
  Google Font is TTF). `font-family` lists fall back in order; generic families map to the
  built-in Roboto.
- SVG: `<img src="icon.svg">` and inline `<svg>` draw as vectors; an SVG whose paint is
  `currentColor` follows the element's CSS `color` (also through `<img>`, unlike a browser).

### What the engine rewrites

You write standard CSS; the engine translates it. Most of it is exact; where it is not, a
diagnostic (below) says what changed.

| You write | The engine |
|---|---|
| any CSS color: `rgba(0,0,0,.5)`, `rgb(0 0 0 / 50%)`, `hsl()`, `oklch()`, `color-mix()`, 148 names, `#rgba` | exact (hex) |
| `border: 1px solid #fff`, per-side borders, `border-color: red green` | exact; `dashed`/`dotted`/`double` draw solid (warning), `inset`/`outset`/`groove`/`ridge` with Chromium's shades |
| `background:` shorthands, several layers, `url()` with `cover`/`contain`/`repeat`/`space`/`round`/positions, gradients incl. `repeating-*` and `-webkit-` forms | exact |
| `border-radius: 50%` | exact on squares and pills (a stadium on other rectangles); other `%` radii dropped until phase 3 |
| `outline`, `outline-offset` | a ring around the box (the offset gap is filled with the outline color) |
| `text-shadow`, `-webkit-text-stroke` | text effects (glow for blurred shadows) |
| `transition: all .3s ease`, `animation: x .4s ease forwards`, `@keyframes` | times in `ms` work; CSS easings use the nearest built-in curve; `forwards`/`both` keep the end state |
| `font: 600 14px/1.5 'Inter', sans-serif`, quoted family names | exact |
| `:root` variables, `var()` | exact, also inside shorthands and gradients |
| `@media (width >= 600px)`, `prefers-color-scheme`, `prefers-reduced-motion`, `@supports`, `@layer`, `@import`, CSS nesting (`&`), vendor prefixes | evaluated or flattened (`UIDocument:setColorScheme("dark")`, `setReducedMotion(true)` switch the two preferences) |
| `inherit`, `initial`, `unset`, `!important` | exact for inherited properties and `!important` against equal or lower specificity (a warning names a more specific rule that still wins) |
| `calc()`, `min()`, `max()`, `clamp()` | computed when the units allow (`calc(40px + 2rem)`); a `%` mixed with `px` keeps the `%` part (warning) until phase 3 |
| `vmin`/`vmax`, `dvh`/`svh`, `ch`/`ex` | exact / `vh` / em multiples |
| `clip-path` shapes, `mask-image`, `object-fit`, `image-rendering: pixelated` | exact |
| `::-webkit-scrollbar(-thumb/-track)`, `scrollbar-width`, `scrollbar-color`, `::placeholder`, `::-webkit-slider-thumb` | the engine's scrollbar, placeholder and slider parts |
| `position: fixed` | positioned like `absolute` against the page (no scrolling page in a game UI) until phase 3; `sticky` acts as `relative` |

Not yet (phase 3 adds them; until then a warning names each use): `display: grid` (laid out as
a wrapping flex row: fixed column counts work, placement does not), `::before`/`::after`
(the rule is dropped: use a real element), `:has()`, `order`, `aspect-ratio`, `opacity` as a
group (each child fades separately), `line-height: normal` (1.2: set `line-height` where text
must line up exactly). Not planned: `<video>`, `<canvas>`, `<iframe>` (a placeholder box:
render a scene into a texture instead), JavaScript. Other differences to know: a
`<legend>` is as wide as its fieldset and the fieldset's top border runs under it (no gap is
cut); a table without a `width` fills its container (Chrome shrinks it to its content: give
tables a width); `border-collapse: collapse` removes the spacing but keeps each cell's own
borders (a warning says so); font sizes are whole pixels (`13.333px` draws at 13px), so text
can be up to a pixel or two narrower than in Chrome.

### Reading the diagnostics

Every problem, loss or approximation is reported with the place in **your** file:

```text
[HTML UI] ui/menu.css:42:3: warning: 'font-variant-numeric' is not supported; dropped (fix: use a monospace font for aligned digits) [HTMLUI-CSS-0002]
[HTML UI] ui/menu.html: 0 errors, 1 warning, 7 notes
```

- Format: `path:line:column: severity: message (fix: suggestion) [code]`. The code is stable
  (`HTMLUI-<area>-<number>`, areas `GEN HTML CSS SEL AT IMP SVG`): search the support matrix
  for it.
- **Errors** mean something the page needs is missing (a file, an import, JavaScript);
  **warnings** mean it draws differently from Chrome; **notes** (info) record exact rewrites
  and are counted in the per-document summary line, listed one by one with the editor
  setting *HTML UI > Verbose Diagnostics* (Editor Settings).
- Where to read them: the Console (each document logs its lines and a summary when it loads),
  `UIDocument:getDiagnostics()` from Lua or C++ (each with `code`, `severity`, `message`,
  `suggestion`, `file`, `line`, `column`), and the Inspector of the UI Document. Anything
  RmlUi itself rejects in the translated CSS comes back as `HTMLUI-GEN-0006` at your line.

## Writing documents (RCSS)

> **Phase 2:** `.html`/`.htm` files are now read by an HTML5 parser (lexbor) and compiled to
> RmlUi's dialect by the compatibility layer: `<div/>` no longer closes a `<div>` (as in a
> browser; `HTMLUI-HTML-0026` warns), `<link rel="stylesheet">` is the standard form (the old
> `<link type="text/css">` still loads, with a note), entities and unquoted attributes work.
> Diagnostics name `file:line:column` with a stable code (`UIDocument:getDiagnostics()`, the
> Console). Files in RmlUi's own dialect described below keep working as **`.rml`** documents.

The parser is XML: close every tag (`<div/>`, `<img src="a.png"/>`, `<br/>`), quote every
attribute, escape `&` and `<`. There is **no user-agent stylesheet**: every element is
inline until you give it `display: block` (or `flex`). Rules that differ from browser CSS:

| Write | Not (rejected or misread today) |
|---|---|
| `border: 2px #ff0000;` (width and color, always solid) | `border: 2px solid red;` |
| `#rrggbb`, `#rrggbbaa`, `rgba(255, 0, 0, 128)` (alpha 0-255) | `rgba(255, 0, 0, 0.5)` (reads as almost transparent), `rgb(0 0 0 / 50%)` |
| `font-family: Roboto, sans-serif;` (unquoted list) | `font-family: "Roboto", sans-serif;` (quoted names are a syntax error) |
| `decorator: image(bg.png);` for a background image | `background-image: url(bg.png);` |
| `-rmlui-direction: rtl;` (right-to-left base direction) | `direction: rtl;` |
| `border-radius: 12px;` | `border-radius: 50%;` |
| `transition: background-color 0.2s cubic-in-out;` on class / pseudo-class changes | `transition: opacity 200ms ease;` |

What works:
- Box model: `width`/`height` (px, %, em), `min-*`/`max-*`, `margin`, `padding`, `border`
  widths and colors per side, `border-radius`, `box-sizing`, `background-color`,
  `position: absolute/relative` with `left/top/right/bottom/inset`, `z-index`, `overflow:
  hidden/auto/scroll` (clipping, rounded and transformed too: see "Rendering features and
  limits"), `visibility`, `opacity` (multiplies each element's colors; not a group:
  `filter: opacity()` is).
- Flexbox: `display: flex`, `flex-direction`, `flex-wrap`, `justify-content`, `align-items`,
  `align-content`, `align-self`, `flex` and `flex-grow/shrink/basis`, `gap` (no `order`).
- Transforms: `transform` (`translate`, `rotate`, `scale`, `skew`, `perspective`,
  `rotateX/Y`, ...), `transform-origin`, `perspective`.
- Images: `<img src="...">` and `decorator: image(...)`, with alpha.
- Text: `font-size`, `color`, `font-weight` and `font-style` (a loaded face, else synthesized:
  "Fonts and text"), `font-effect` ("Text effects"),
  `line-height`, `text-align` (left, center, right, justify), `letter-spacing`,
  `text-decoration` (underline, overline, line-through), `white-space`, `text-transform`,
  word wrapping; `<br/>`.
- Selectors: type, `#id`, `.class`, descendant/child/sibling combinators, attribute
  selectors, `:hover`, `:active`, `:first-child`, `:nth-child()`, `:not()`.
- `var()` custom properties, `@media` (width/height of the canvas, orientation),
  `@keyframes` + `animation`, transitions as above. They run on the **UI clock**: the
  engine's frame time before the time scale, so a pause menu animates while
  `Engine.timeScale = 0` freezes the game; a fixed frame delta (`play` with `fixed_fps`)
  makes them deterministic, and they stand still while the simulation is held
  (`auto_pause`).
- Input: see "Input" below.

## Rendering features and limits

Each scene's screen-space documents draw together into one offscreen target, which the
camera composites in front of the component UI (post-process applies to it, like component
UI). Under a UI or 2D camera the target covers the canvas rectangle in world space, so the
editor's Scene view of a UI or 2D scene shows documents in place while you pan and zoom;
under a 3D camera it fills the Game view (3D Scene views do not show it).

What draws (RCSS syntax, phase 1):

| Feature | Write | Notes |
|---|---|---|
| Rounded clipping | `overflow: hidden; border-radius: 16px;` | children are clipped to the rounded box (stencil); nested clips intersect |
| Clipping inside transforms | `overflow: hidden; transform: rotate(10deg);` | children in normal flow that overflow are clipped to the transformed box |
| Gradients | `decorator: linear-gradient(90deg, #f00, #00f 60%, #0f0);` | also `radial-gradient(circle 40px at center, ...)`, `conic-gradient(from 30deg at 50% 50%, ...)` and the `repeating-` forms; at most **16 color stops** (the rest are dropped with a warning) |
| Filters | `filter: blur(4px) grayscale(1) opacity(0.5);` | `opacity`, `blur`, `drop-shadow(#000 4px 4px 6px)`, `brightness`, `contrast`, `invert`, `grayscale`, `sepia`, `hue-rotate`, `saturate`, applied left to right; `filter: opacity()` is a group (the subtree fades as one image), unlike the `opacity` property |
| Backdrop filters | `backdrop-filter: blur(10px);` | the same filters, applied to what is behind the element's border box |
| Box shadows | `box-shadow: #00000080 4px 6px 12px 2px, #ff0 0 0 0 3px inset;` | color first, then offset x/y, blur, spread, `inset`; an outer shadow is blurred and then clipped to outside the box, as in CSS (a zero-offset blurred shadow is a glow) |
| Masks | `mask-image: linear-gradient(90deg, #000f, #0000);` or `mask-image: image(mask.png);` | the content's alpha is multiplied by the mask's |

Anti-aliasing: the target is multisampled (4x by default). The project setting *HTML UI MSAA*
(`graphics.html_ui_msaa_samples`: 1, 2, 4 or 8; `UIDocumentSystem::setPreferredSampleCount`
from C++) changes it; devices that cannot multisample fall back to 1.

Cost: a document without filters, masks or shadows is one offscreen pass, and a static
document uploads nothing per frame. Every `filter`, `backdrop-filter`, `mask-image` and
blurred `box-shadow` adds offscreen passes (a blur, 4 to 6; a backdrop blur, a few more), so
keep them to the elements that need them. The Game view statistics overlay shows the
document passes, draws and pipeline switches of the last frame (`UIDocumentSystem::getRenderStats()`
from C++: the scene's screen and texture documents together).

Unsupported names are reported, not ignored: an unknown filter or `decorator: shader(...)`
logs an `[HTML UI]` warning naming it and draws nothing for it.

## Fonts and text

- Built in: Roboto (Latin, Cyrillic, Greek) and Noto Sans Arabic (Arabic with shaping and
  bidirectional text, right-to-left with `-rmlui-direction: rtl`). Generic families
  (`sans-serif`, `serif`, `monospace`, `system-ui`) map to Roboto; there is no built-in serif
  or monospace face yet.
- `font-family` lists fall back in order; an unknown family logs one
  `[HTML UI] font-family '<name>' is not loaded` warning and the next one is used. A
  character no loaded font covers draws a missing-glyph box.
- More fonts (TTF/OTF) are loaded from C++ before or after documents load:
  `HtmlUI::loadFontFace("fonts/Roboto-Bold.ttf")` (`#include "htmlui/HtmlUI.h"`; family,
  weight and style come from the file, or pass them: `loadFontFace(path, fallback, family,
  weight, italic)`). `font-weight: bold` / `font-style: italic` pick the matching face
  (CSS weight matching). Without one the engine synthesizes it, as browsers do
  (`font-synthesis`): a face 300 or more lighter than asked (the built-in Roboto Regular for
  `bold`) is emboldened (strokes and advance grow by `max(1, font-size / 24)` px, so bold text
  is a little wider), and an upright face asked for `italic` is slanted by 12 degrees (same
  advance). A real Bold/Italic file looks better: load it when the design depends on it.
- RCSS `@font-face { font-family: Title; src: fonts/Title.ttf; }` (in a stylesheet or a
  `<style>` block; `font-weight`/`font-style` optional) loads a face too. RmlUi opens `src` as
  written, from the assets directory (not relative to the stylesheet), without `url()`;
  standard CSS `@font-face` with `url()` and `format()` comes with phase 2.
- The text of each context is rasterized at its own pixel ratio (the screen's for screen
  documents, `pixelRatio` for documents on meshes), so text stays sharp in both.

## Text effects

`font-effect` (RmlUi property, inherited) draws effects with the text, comma-separated:

| Effect | Syntax | Drawn |
|---|---|---|
| shadow | `shadow(offset-x offset-y color)` | a copy of the glyphs, offset |
| outline | `outline(width color)` | the glyphs grown by `width` |
| glow | `glow(outline-width blur-width offset-x offset-y color)` | grown, then blurred, offset |
| blur | `blur(width color)` | a blurred copy |

```css
h1   { font-size: 32px; color: #ffffff; font-effect: shadow(2px 2px #000000), outline(1px #202020); }
.hot { font-effect: glow(2px 6px 0px 0px #40ff40); }
```

- All four draw behind the text, in the order written; the text keeps its `color`. The
  effect's colour is multiplied by the element's `opacity`.
- Lengths are whole CSS `px` (`2px`; the number is read, the unit is not converted).
- Effect bitmaps are made at document resolution: on a high-DPI screen outline, glow and
  blur edges are slightly softer than the text itself; shadows are as sharp as the text.
- Synthetic italic slants the effects with the text.
- Standard CSS `text-shadow` and `-webkit-text-stroke` are not read yet (later phases map
  them): write `font-effect`.

## SVG and Lottie

SVG is drawn by **LunaSVG**, Lottie by **ThorVG** (CPU), both inside the engine: no browser,
no network. Both are rasterised into textures and drawn like images, so `opacity`,
transforms, clipping and filters apply to them as to `<img>`.

### SVG

| Write | Result |
|---|---|
| `<svg src="icons/play.svg" style="width: 24px; height: 24px;"/>` | the file, rasterised at the element's size (crisp at any size; one texture per size and colour, cached) |
| `<svg src="logo.svg"/>` | the file's own size (`width`/`height`, else the `viewBox`) |
| `<svg src="logo.svg" crop-to-content="crop-to-content"/>` | trimmed to the drawing's bounding box (intrinsic size and rendering) |
| `<svg width="48" height="48" viewBox="0 0 24 24"> <path .../> </svg>` | inline SVG, the markup between the tags |
| `decorator: svg(icons/bg.svg);` / `decorator: svg(icons/bg.svg crop-to-content);` | an SVG background stretched over the element |
| `image-color: #ff8000;` | tints the SVG: every pixel is multiplied by the colour (draw icons white, tint them per state) |

- Supported well: paths, shapes, strokes (dashes, caps, joins), linear and radial gradients,
  patterns, `clipPath`, masks, markers, `<use>`/`<symbol>`, nested `<svg>`, CSS in
  `<style>` and `style=""`, opacity, `<text>` in the built-in Roboto (any `font-family` uses
  it).
- `currentColor` follows the element's CSS `color`, for inline SVG and `<svg src>` files
  alike (icons recolor on `:hover` and with transitions): an SVG whose paints are all
  `currentColor` or `none` is rasterized once in white and tinted (`color` x `image-color` x
  `opacity`); one that mixes `currentColor` with fixed colors is rasterized again when the
  colour changes (`HTMLUI-SVG-0001`, info). A shape with no fill at all counts as black.
- CSS `fill`, `stroke`, `stroke-width`, `stroke-linecap`, `stroke-linejoin`,
  `stroke-opacity`, `fill-opacity`, `stroke-dasharray` on the `<svg>` element apply to its
  root, as in a browser (`.icon { stroke-width: 1.5 }`); rules aimed at elements inside the
  SVG (`svg path { ... }`) are not applied. `<foreignObject>` is dropped (`HTMLUI-SVG-0002`).
- Icons: `<i data-lucide="house"></i>` is the [Lucide](https://lucide.dev) icon, built into
  the engine (Lucide 1.53.0, all 1869 icons and their aliases, offline): replaced by its
  `<svg>` exactly as `lucide.createIcons()` does (classes `lucide lucide-house`, your
  attributes merged), so it takes the CSS `color` and `.lucide { width: 20px; stroke-width:
  1.5 }`. Names in kebab-case, PascalCase or camelCase; an unknown name is reported with the
  three closest ones (`HTMLUI-HTML-0021`). Setting `data-lucide` from Lua swaps the icon.
- Not supported: SVG filters (`<filter>`, drop shadows inside the file are skipped),
  `mix-blend-mode`, scripts and animation (SMIL) inside SVG files, `<image href="file.png">`
  (only `data:` URIs: the rasteriser never reads files on its own), system fonts.
- `.svg` textures outside HTML UI (`Image`, `Sprite`, materials, `path.svg?svgScale=2`) use
  the same rasteriser: straight alpha, size = intrinsic CSS px x `svgScale`, rounded up, at
  most 16384 px.

### Lottie: `<lottie>`

```html
<lottie id="spinner" src="anim/loading.json" autoplay="autoplay" loop="loop"
        style="width: 96px; height: 96px;"/>
<lottie id="hero" src="anim/hero.lottie" animation="intro" marker="idle" speed="1.5"/>
```

| Attribute | Values | Default |
|---|---|---|
| `src` | a `.json` (Bodymovin) or `.lottie` (dotLottie v1/v2) file, relative to the document | — |
| `autoplay` | present = play on load (`autoplay="false"` = off) | paused on the first frame (or the `.lottie` manifest's `autoplay`) |
| `loop` | present / `true` = forever; a number = that many plays in total (`loop="3"`: two `loop` events, then `complete`; `0` = forever); `false` = once | once |
| `speed` | number > 0 | 1 |
| `direction` | `1` or `-1` (from the last frame backwards) | 1 |
| `mode` | `normal` or `bounce` (there and back; one loop = both ways) | normal |
| `segment` | `"start end"` in frames, end excluded (`segment="30 60"` shows frames 30..59) | the whole animation |
| `marker` | a marker name of the animation: its frames become the segment | — |
| `animation` | the animation id inside a `.lottie` | the manifest's `activeAnimationId` (v1) / `initial.animation` (v2), else the first |

- Attributes override a `.lottie` manifest's `autoplay`, `loop`, `speed`, `direction` and
  `playMode`. Changing an attribute at run time applies it (`src`/`animation` reload).
- Size: without `width`/`height` the element takes the animation's size. Otherwise the
  animation is scaled to fit inside the content box, aspect ratio kept, centred.
- `image-color` and `opacity` tint it like an SVG.
- Time is the **UI clock** (like CSS animations): it keeps playing in a pause menu while
  `Engine.timeScale = 0`, is deterministic with a fixed frame delta and stands still while
  the simulation is held. Frames follow lottie-web: an N-frame animation at F fps shows
  frames 0..N-1, each for 1/F s.
- Cost: each element owns ONE texture of its content-box size, re-rasterised and updated in
  place only when the shown frame changes (a 30 fps animation updates 30 times per second
  whatever the display rate; a paused one costs nothing). Resizing creates a new texture.
- Images used by the animation: embedded (`data:`) or files next to the `.json` (`u` + `p`
  of the asset), and inside a `.lottie` its `images/` (v1) or `i/` (v2) folder. Text layers
  draw with the font embedded in the file, else Roboto.
- Not supported: expressions (they need a JavaScript engine), audio layers; `.lottie`
  archives that are ZIP64, encrypted or multi-disk (an `error` event says so); themes and
  state machines of dotLottie v2.

Events (on the element; they do not bubble, capture listeners see them):

| Event | When |
|---|---|
| `load` | the animation was read (a frame after the document loaded, so listeners added right after loading get it; in the editor a document is already loaded when play starts, so check `getTotalFrames() > 0` instead of waiting for it) |
| `error` | the file is missing or unreadable, the `.lottie` is invalid, `animation` names no animation; `event.detail.message` in Lua (and the Console) says which |
| `play` / `pause` | playback starts (autoplay included) / stops (`pause()`, `stop()`) |
| `loop` | each time an iteration ends and the next begins |
| `complete` | the last iteration ended (never with an infinite loop); playback stays on the last frame |

Control from C++ (`UIElement`, any `<lottie>`; on another element the call logs an error and
does nothing) and from Lua (the same method names on an element handle):

```cpp
UIElement hero = menu.getElementById("hero");
hero.play(); hero.pause(); hero.stop();          // stop = paused on the first frame
hero.seek(1.5f);                                 // seconds into the segment
hero.setFrame(42.0f);                            // absolute frame (clamped to the segment)
hero.setSpeed(2.0f); hero.setDirection(-1);
hero.setLoop(true); hero.setLoopCount(3);        // 3 plays in total; 0 = forever
hero.setSegment(30.0f, 60.0f); hero.setMarker("idle");
bool playing = hero.isPlaying();
float frame = hero.getCurrentFrame(), frames = hero.getTotalFrames(), seconds = hero.getDuration();
```

```lua
-- in init() of a script on the document's GameObject (see "Scripting")
local doc = UIDocument(self.scene, self.entity)
local spinner = doc:getElementById("spinner")
spinner:addEventListener("complete", function(event) print("done") end)
spinner:addEventListener("error", function(event) print(event.detail.message) end)
spinner:setLoopCount(2)
spinner:play()
```

## HTML elements with behavior

Documents loaded from `.html` get these elements with their browser behavior (also in `.rml`
documents: `img`, `input` and `label` replace RmlUi's, compatible with RmlUi's attributes):

| Element | Behavior |
|---|---|
| `<button>` | `type="submit"` (the default) submits its `<form>` (the `submit` event carries the button's `name`/`value` and every named control), `reset` resets the form, `button` does nothing; `form="id"` works; `disabled` gets `:disabled` and no click; Enter and Space activate it |
| `<a href>` | Enter activates; `#id` scrolls to the element; any other link dispatches `htmlnavigate` on the document (`href`, `url`, `fragment`, `document`, `external`): a `.html` link loads that document in the same component, external links only notify (there is no browser) |
| `<input>` | the `type` attribute stays as written; `number` filters typing, steps with Up/Down by `step` within `min`/`max` and clamps on blur/Enter; `date`, `time`, `datetime-local`, `month`, `week` are text fields showing the format; `color` a text field; `reset` resets the form; `image` submits and shows `src`; `file` is a disabled "Choose File" button; Enter in a text field submits its form; Space toggles checkboxes; `autofocus` |
| `<ol>`/`<ul>`/`<li>` | markers from `list-style-type` (disc, circle, square, decimal, decimal-leading-zero, lower/upper-alpha and -latin, lower/upper-roman, lower-greek, strings), `list-style-image`, `list-style-position`; `ol start`, `reversed`, `li value`; style them with `li::marker` |
| `<details>`/`<summary>` | clicking (or Enter/Space on) the summary toggles `open`; a `toggle` event follows; details with the same `name` form an accordion |
| `<dialog>` | `open` shows it; Lua `dialog:showModal()`, `show()`, `close(value)`; Escape closes a modal dialog (`cancel`, then `close`). No backdrop, no focus trap |
| `<img>` | `object-fit`, `object-position`, `image-rendering: pixelated` (nearest sampling), SVG sources drawn at their size |
| `<label>` | clicking it activates its control (`for="id"`, or the control inside it) |
| `<lottie playonhover>` | plays while the pointer is over it, stops (first frame) when it leaves |

`:focus` and `:focus-visible` match the focused element only (not its ancestors; those match
`:focus-within`); `:focus-visible` after keyboard navigation and in text fields, as in
Chromium. `animation-fill-mode: forwards`/`both` keeps the last keyframe after the animation.
CSS backgrounds with `url()` (every `background-size`, `background-position`,
`background-repeat` including `space`/`round`, origin, clip, `fixed`), `clip-path` shapes
and `border-image` are drawn by the engine's own decorators.

## Input

Documents get the mouse (buttons, move, wheel, enter/leave), multi-touch, the keyboard, text
(IME included) and the clipboard, in play mode, in exported games and through MCP
`send_input`. Screen-space UI is drawn over the 3D world, so a pointer event goes to the
screen documents first (top scene layer first), then to the component UI, then to the
documents on meshes ("Documents on meshes"), then to the game. Keys, text and IME go to the
documents (the one that took the last press) before the game.

**What a document consumes.** A consumed event does not reach the component UI under the
document nor, while `Engine.ignoreEventsHandledByUI` is on (the default), the game's
callbacks (`onMouseDown`, `onKeyDown`, `onCharInput`, `onMouseScroll`, `onTouchStart`, ...).
- Pointer (press, release, move, touch): consumed when the element under the pointer is an
  element of a document, or a document (`body`) that paints a background (a colour with
  alpha above 0, or a decorator), as a browser page over a canvas would. A transparent
  `body` lets the pointer through to the game. An element with `pointer-events: none` is
  skipped by the hit test, so a full-screen wrapper with it is click-through (its children
  can set `pointer-events: auto`). A release (and the moves of a drag) belongs to whoever got
  the press.
- Wheel: consumed when it scrolls an element (`overflow: auto/scroll`) or the pointer is
  over content as above. One wheel step scrolls 80 px.
- Keys: consumed when the focused element handles the key (a script stops its propagation
  or prevents its default), or when a text field (`<input>` of a text type, `<textarea>`,
  `<select>`) has focus and the key is not Escape or F1-F25. The release of a consumed key
  is consumed too. Keys go to the documents before the game.
- Text and IME: consumed while a text field has focus. Control characters (`\b`, `\r`,
  `\t`, Escape) are never inserted as text; one that arrives without its key (the Android
  soft backspace) acts as that key.
- Multi-touch: every finger has its own pointer id; each pressed element gets `:active`.
  RmlUi has one pressed (active) element chain per document context, so lifting any finger
  clears `:active` on all of them, and only the last pressed element can be clicked.

**Text fields.** Focusing a text field opens the virtual keyboard (Android, iOS, Web) and
places the IME candidate window at the caret (Windows, macOS, Linux, Web). IME composition
shows inside the field on Windows, macOS, iOS, Android and Web; on Linux the input method
(IBus, fcitx through XIM) draws its own preedit window and the field receives the committed
text, and in the editor's Game view composed text arrives committed (ImGui exposes no
composition). Copy, cut, paste and select all: Ctrl+C/X/V/A (Cmd on macOS and iOS).

**Clipboard.** `System.setClipboardText(text)` / `System.getClipboardText()` (Lua, and
`System::instance()` in C++) use the system clipboard on Linux (X11 `CLIPBOARD`), Windows,
macOS, iOS, Android and in the editor. On Web the browser clipboard is asynchronous:
writes go to `navigator.clipboard` (a key or click must have started them), and
`getClipboardText` returns the text of the last copy or `paste` event; Ctrl/Cmd+V in a
document text field is left to the browser, whose paste event inserts the text.

**Cursor.** The CSS `cursor` of the element under the pointer becomes the mouse cursor:
`default`, `pointer`, `text`, `crosshair`, `move`, `not-allowed`, `wait`, `progress`, `help`,
`grab`, `grabbing` and the `*-resize` family (the first one of a list the engine knows;
others show the arrow). Lua: `Engine.mouseCursor = CursorType.GRAB` sets it for the game.

**MCP `send_input`.** `text` sends the whole UTF-8 string as one input. `ime` (`text`,
`cursor` in code points, -1 selects the composition) updates the composition of the
focused field, `ime_commit` (`text`) commits it, `ime_cancel` drops it. `mouse_leave` and
`mouse_enter` move the pointer out of and back into the game. Wheel offsets follow GLFW:
`dy > 0` scrolls up, `dx > 0` scrolls left.

## Scripting

Lua scripts (and C++ scripts) reach a document through `UIDocument`, a GameObject wrapper:
`UIDocument(self.scene, self.entity)` in a script on the document's GameObject, or a script
property of type `UIDocument`. Elements are `UIElement`s with **browser names and semantics**:

```lua
---@class Menu : ScriptInstance
local Menu = { properties = {} }

function Menu:init()
    local doc = UIDocument(self.scene, self.entity)
    -- data models first: a document that uses one reloads to bind it (handles from before are invalid)
    self.shop = doc:createDataModel("shop", {
        gold = 10,
        items = { {name = "Sword", price = 5}, {name = "Shield", price = 8} },
        buy = function(event, index) print("buy", index) end,   -- data-event-click="buy(i)"
    })
    doc:getElementById("play"):addEventListener("click", function(event)
        local play = event.currentTarget            -- the element now, also after a reload
        play.classList:add("pressed")
        play.textContent = "Loading..."
        self.shop.gold = self.shop.gold - 1        -- {{ gold }} updates
    end)
end

return Menu
```

Documents load with their scene, so `init` can query elements, in the editor and in exported
games alike (a game runs its start scene's `init` before the window exists; layout needs no
GPU). Bind listeners once, in `init`, to elements with an `id`: a reload that keeps the state
(hot reload, `doc:reload()`) moves them to the new elements. `doc.onLoad` (an event:
`doc.onLoad = function() ... end` or `RegisterEvent(self, doc.onLoad, "method")`) runs after
every later load and reload, `onUnload` before every unload: use them for what a reload does
not keep ("Lifetimes"). The reload a `createDataModel` call makes is over when the call returns,
so handlers registered after it are not called for it. A document that uses a data model no
script created yet (edit mode, or before `init`) shows its `{{ }}` empty. When play stops,
everything the scripts made in documents goes (listeners, data models, DOM changes) and the
documents load again as they are on disk.

### Element API (Lua; C++ has the same names as get/set methods)

| Group | Members |
|---|---|
| Document | `doc:getElementById(id)`, `doc:querySelector(sel)`, `doc:querySelectorAll(sel)` (array; CSS selectors, rewritten like the stylesheets in `.html`), `doc.body` (the `<body>`), `doc.documentElement` (the `<html>`), `doc:getDiagnostics()` (`{severity, code, name, message, suggestion, file, line, column, text}`), `doc:setColorScheme("dark"|"light")`, `doc:setReducedMotion(bool)` (the `prefers-*` media queries), `doc:createElement(tag)`, `doc:createTextNode(text)`, `doc.title`, `doc:addEventListener/removeEventListener`, `doc:createDataModel(name, table)`, `doc:getDataModel(name)`, `doc:removeDataModel(name)`, `doc:load(file)`, `doc:reload()`, `doc.loaded`, `doc:show()/hide()`, `doc.visible`, `doc.sortOrder`, `doc.filename`, `doc.onLoad/onUnload`, `UIDocument.setDebuggerVisible(bool)` |
| Identity / tree | `id`, `tagName` (lower case in `.rml`; in `.html` the DOM's: `DIV`, `BODY`, `HTML`, `TEMPLATE`), `localName`, `className`, `parentElement`, `children` (array), `firstElementChild`, `lastElementChild`, `nextElementSibling`, `previousElementSibling`, `childElementCount`, `querySelector`, `querySelectorAll`, `closest`, `matches`, `contains` |
| Mutation | `appendChild(el)`, `insertBefore(el, ref)` (ref nil appends), `removeChild(el)`, `replaceChild(new, old)`, `remove()`, `cloneNode(deep)` |
| Content | `textContent` (text, never parsed), `innerHTML` / `outerHTML` (HTML in `.html` documents, parsed and rewritten like the document; RML in `.rml`), `insertAdjacentHTML(position, html)`, `content` (`<template>`: a fragment; `tpl.content:cloneNode(true)` then `appendChild` it) |
| Attributes | `getAttribute(name)` (nil when absent), `setAttribute`, `hasAttribute`, `removeAttribute`, `getAttributes()` |
| Classes / style / data | `classList:add(...)/remove(...)/toggle(name[, force])/contains(name)`; `style.backgroundColor` or `style["background-color"]` (read: computed value as CSS text; write: inline style, `""`/nil removes), `style:getPropertyValue/setProperty/removeProperty`; `dataset.userName` = `data-user-name` |
| Forms | `value`, `checked`, `disabled` |
| Focus / interaction | `focus()`, `blur()`, `click()`; dialogs: `show()`, `showModal()` (no backdrop), `close([returnValue])` (dispatches `close`) |
| Scroll / geometry | `scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight`, `clientWidth`, `clientHeight`, `offsetWidth`, `offsetHeight`, `offsetLeft`, `offsetTop`, `scrollIntoView([alignToTop])`, `scrollTo(x, y[, smooth])`, `scrollBy(dx, dy[, smooth])`, `getBoundingClientRect()` (`x y width height top left right bottom`, document px, through the transforms of the last drawn frame) |
| Events | `addEventListener(type, fn[, capture or {capture=, once=}])`, `removeEventListener(type, fn[, capture])`, `dispatchEvent(type[, detail[, bubbles]])` (false when a listener prevented it) |

Queries that find nothing return `nil`. Adding the same function for the same type and
phase twice is ignored, as in the DOM. In `.html` documents the engine's own attributes
(`data-htmlui-*`, the source location of every element) and classes (`htmlui-*`) are invisible to
scripts; `style.cssText` reads and replaces the inline declarations.

### Scripts inside a document: `<script type="text/lua">`

An `.html` document can carry its own Lua, inline or as `src="menu.lua"` (relative to the
document). Each script runs after every load and reload of the document **while the game plays**
(the scene's Lua scripts started; never in edit mode), in its own environment table (globals
readable, its own definitions stay local) with `document` (its `UIDocument`) and `anime` bound:

```html
<button id="play">Play</button>
<script type="text/lua">
document:getElementById("play"):addEventListener("click", function()
    anime.animate("#play", { opacity = 0.5, duration = 300 })
end)
</script>
```

Errors are reported at the line of the HTML file (`ui/menu.html:45: ... [HTMLUI-HTML-0022]`) and
in `getDiagnostics()`. JavaScript `<script>`s are not run (`HTMLUI-HTML-0006`; known libraries
such as Lucide or Tailwind's CDN get a tailored note). `<a href="other.html">` loads that document
in the same UI Document when clicked unless a listener of `htmlnavigate` calls `preventDefault()`.

### Events

| DOM name | What fires it |
|---|---|
| `click`, `dblclick`, `mousedown`, `mouseup`, `mousemove`, `mouseover`, `mouseout`, `keydown`, `keyup`, `focus`, `blur`, `scroll`, `resize`, `load`, `unload`, `submit`, `animationend`, `transitionend`, `dragstart`, `drag`, `dragend`, `dragover`, `dragdrop`, `textinput` | the same RmlUi event |
| `wheel` | the mouse wheel (`deltaX`, `deltaY`: steps, positive down/right) |
| `input` | every edit of a text field, and every toggle |
| `change` | a text field on blur when its value changed since it got focus; checkboxes, radios, selects and ranges when they change |
| any other name | custom events sent by `dispatchEvent` |

`mouseenter`, `mouseleave`, `pointer*`, `touch*`, `contextmenu`, `beforeinput` and
`composition*` never reach documents: `addEventListener` logs a warning listing the
supported events. A listener gets a `UIEvent`: `type`, `target`, `currentTarget`,
`eventPhase` (1 capture, 2 target, 3 bubble), `button` (DOM numbering: 0 left, 1 middle,
2 right), `clientX`/`clientY` (document px), `deltaX`/`deltaY`, `ctrlKey`, `shiftKey`,
`altKey`, `metaKey`, `key`, `code`, `keyCode`, `repeat` (`event["repeat"]` in Lua), `data`
(text of `textinput`), `detail` (a copy of the `dispatchEvent` detail), `defaultPrevented`,
and `stopPropagation()`, `stopImmediatePropagation()`, `preventDefault()`. The event is valid
only while the listener runs. `preventDefault()` skips the element's own action (a checkbox
does not toggle, a prevented `keydown` types nothing, a prevented `wheel` does not scroll)
and marks the input as used by the UI (the game does not get it). An error in a listener is
logged with its traceback; the other listeners still run.

### Data binding

RmlUi's data binding, driven from a Lua table (or C++ `UIValue`s):

```html
<div data-model="shop">
    <p>Gold: {{ gold }}</p>
    <div data-for="item, i : items" data-event-click="buy(i)">{{ item.name }}: {{ item.price }}</div>
    <input type="text" data-value="playerName"/>
    <input type="checkbox" data-checked="music"/>
</div>
```

```lua
local shop = doc:createDataModel("shop", { gold = 10, items = {}, playerName = "", music = true,
                                           buy = function(event, i) print("row", i) end })
shop.gold = 20                                   -- the document updates on the next frame
table.insert(shop.items, {name = "Bow", price = 3})
shop.items[1].price = 7                          -- nested tables are proxies too; #, pairs, ipairs work
shop:onChange("playerName", function(value) print("typed", value) end)   -- after a data-value write
```

- The returned value is a **proxy**: reading gives the model's current data (written by the
  document too: `data-value`, `data-checked`), assigning updates the document. Functions in
  the table become `data-event-*` callbacks `fn(event, args...)`.
- The top-level keys are fixed when the model is created: assigning a new one is an error.
  Nested tables can change freely. Tables with keys 1..n are arrays (an empty table is an
  empty array). Only nil, booleans, numbers, strings and tables are stored.
- The `i` of `data-for="item, i : items"` is **0-based** (RmlUi); Lua indices are 1-based
  (`shop.items[i + 1]`).
- Proxy methods: `onChange(name, fn)` (returns an id), `removeOnChange(id)`, `dirty(name)`,
  `dirtyAll()`, `get(path)`/`set(path, value)` (`"items[0].name"`, 0-based), `getName()`,
  `isValid()`. These names cannot be data keys.
- Create data models first in `init`: a document already loaded that uses the model
  (`data-model="shop"`) reloads to bind it, which invalidates element handles from before.
- A model belongs to the document that created it: it survives reloads and is removed with
  `removeDataModel`, with the component, or when the scene ends. Any use of a removed model's
  proxy raises `data model 'shop' was removed`.

### Lifetimes

- Element handles are weak. Once the element is removed for good or its document unloads
  (`doc:load(other)`, the scene ends), `el:isValid()` is false and any other use raises
  `UIElement: element no longer exists (its document was unloaded or it was removed)` in Lua
  (C++ logs it once per method and returns a default).
- Elements made by `createElement`/`createTextNode`, removed by `removeChild`/`remove`/
  `replaceChild` or made by `cloneNode` are **detached**: they live while a handle of them (or
  of an element inside them) exists, and can be inserted again. They are freed at the end of
  the frame in which the last handle went (Lua: after the garbage collector took it), and with
  their document.
- Listeners and data-model callbacks hold their Lua functions until their element, model or
  scene goes. `UIDocument.getLiveLuaRefCount()` (diagnostics) is 0 after play stops.
- A reload that keeps state (hot reload, `doc:reload()`, a late `createDataModel`) moves the
  listeners of elements with an `id` (and of the document) to the new elements; listeners of
  elements without an id are dropped with one warning: give the element an id or add the
  listener again in `onLoad`. It keeps scroll positions, form values and the focus of elements
  with an id too; everything else comes from the file again: classes, inline styles,
  attributes and elements a script (or `anime`) changed, and animations of the replaced
  elements are cancelled. Redo that in `onLoad`. A method that adds listeners and runs from
  both `init` and `onLoad` should pass the same function each time (adding the same function
  twice is ignored), or moved listeners run twice.
- Inside a listener use `event.currentTarget` (or query again), not a handle captured before:
  after a reload the moved listener runs for the new element, and handles to the old one are
  invalid.

### C++

The same API in C++ (`#include "JasperEngine.h"`); events take `std::function`s and return
ids, data models are `UIDataModel` handles over `UIValue` trees:

```cpp
UIDocument menu(scene, entity);
UIElement play = menu.getElementById("play");
uint64_t id = play.addEventListener("click", [](UIEvent& event){ event.getTarget().addClass("pressed"); });
play.removeEventListener(id);

UIValue initial = UIValue::object();
initial["gold"] = 10;
initial["items"] = UIValue::array();
UIDataModel shop = menu.createDataModel("shop", initial);
shop.set("items[0]", UIValue::object());   // paths are 0-based
shop.set("gold", 20);
shop.bindEvent("buy", [](UIEvent& event, const std::vector<UIValue>& args){ /* args[0] = i */ });
shop.onChange("gold", [](const UIValue& value){ });
```

`UIElement` also keeps the phase-0 helpers `setClass(name, bool)`, `hasClass`,
`setStyleProperty` (false and a warning when RmlUi rejects the value), `removeStyleProperty`,
`getBoundingBox()` (canvas pixels) and `isPseudoClassSet("hover")`.

## Animation

A global Lua table `anime` mirrors [Anime.js v4](https://animejs.com/documentation/) (ported
from 4.5.0, checked against reference values generated from the real library). C++ gets the
same API through `Anime` (`#include "htmlui/anim/Anime.h"`).

```lua
-- targets: a CSS selector (the documents of every scene of the game, the current scene's
-- first: a HUD child scene's elements are found from any script, in init too), a UIElement,
-- a plain table, or a list of them
anime.animate("#title", { translateY = { from = -40 }, opacity = { from = 0 }, duration = 600,
                          ease = "outElastic(1, .6)" })
anime.animate(".card", { scale = 1.1, delay = anime.stagger(80, { from = "center" }), loop = true, alternate = true })

local tl = anime.createTimeline({ defaults = { duration = 300, ease = "outQuad" } })
tl:add("#panel", { x = 200 })
  :label("open")
  :add(".item", { opacity = { from = 0 } }, "open-=100")
  :call(function(timeline) print("opened") end, "+=50")

local stats = { hp = 100, color = "#ff0000" }          -- plain tables: number and colour fields
anime.animate(stats, { hp = 0, color = "#00ff00", duration = 2000, modifier = anime.utils.round(0),
                       onUpdate = function(a) hpLabel.textContent = tostring(stats.hp) end })
```

- **Functions**: `animate(targets, params)`, `createTimeline(params)`, `createTimer(params)`,
  `createSpring(params)` (alias `spring`), `stagger(value | {from, to}, params)`,
  `cubicBezier(x1, y1, x2, y2)`, `steps(n[, fromStart])`, `linear(...)`, `irregular(n, r)`,
  `anime.eases.<name>`, `anime.utils.*`.
- **Parameters**: `duration` (ms, default 1000), `delay`, `loop` (count or `true`), `loopDelay`,
  `alternate`, `reversed`, `autoplay` (default true), `playbackRate`, `playbackEase`,
  `frameRate`, `ease` (default `"out(2)"`), `composition` (`"replace"` default, `"none"`,
  `"blend"`: additive), `modifier` (number -> number), `keyframes` (arrays, `{ {x=..}, ..}` with
  durations, or percentages `{ ["0%"] = {..}, ["50%"] = {..} }`), `id`, `root` (a UIElement or
  UIDocument scoping the selectors), `clock`.
- **Values**: numbers, `"10px"`/`"2em"`/`"45deg"`/`"1turn"`, relative `"+=10"`/`"-=10"`/`"*=2"`,
  colours (`#rgb[a]`, `#rrggbb[aa]`, `rgb()/rgba()/hsl()/hsla()`), strings with numbers
  (`"0 0 4px #000"`), `{from, to}`, `[from, to]`, keyframe arrays, `{to, from, duration, delay,
  ease, modifier}`, `var(--name)`, and function values `function(target, index, total)`.
  **Indices are 0-based, as in Anime** (stagger `from = 2` is the third target).
- **Properties of elements**: individual transforms (`translateX/Y/Z`, `x/y/z`, `rotate`,
  `rotateX/Y/Z`, `scale`, `scaleX/Y/Z`, `skew`, `skewX/Y`, `perspective`, composed in Anime's
  order: perspective, translate, rotate, scale, skew; an inline `transform` is the starting
  point), any CSS property (camelCase or kebab-case; `opacity`, lengths and colours are typed
  writes), custom properties (`["--glow"] = "8px"`, so `var(--glow)` users follow),
  `scrollTop`/`scrollLeft`/`textContent`/`innerHTML`, and attributes. Transform and opacity
  never trigger a layout. Elements that disappear are skipped; an animation whose targets are
  all gone is cancelled without callbacks.
- **Callbacks** (receive the animation): `onBegin`, `onBeforeUpdate`, `onUpdate`, `onRender`,
  `onLoop`, `onPause`, `onComplete`; `anim:andThen(fn)` is Anime's `then()` (`then` is a Lua
  keyword). Errors in callbacks go to the Console with the traceback.
- **Control and state**: `play pause resume restart reverse alternate seek(ms) stretch(ms) cancel
  revert complete reset refresh`, each returning the object; `currentTime`, `progress`,
  `iterationProgress`, `currentIteration`, `reversed`, `speed` (settable), `duration`, `paused`,
  `began`, `completed`, `cancelled`, `id`; timelines add `add set call label sync remove` and
  `labels`. Positions: ms, `"+=100"`, `"-=100"`, `"*=2"`, `"<"` (end of the previous child),
  `"<<"` (its start), `"<+=50"`, `"label"`, `"label+=50"`, or a stagger.
- **Easings**: `linear`, `none`, `in`, `out`, `inOut`, `outIn` (power, default 1.68, `in(3)`),
  and `in/out/inOut/outIn` + `Quad Cubic Quart Quint Sine Expo Circ Bounce Back Elastic`
  (`outBack(1.7)`, `outElastic(1, .3)`), as strings or `anime.eases.<name>`; also
  `cubicBezier(...)`, `steps(...)`, `linear(...)`, `irregular(...)`, `spring(m, k, c, v)` as
  strings (Anime 4.5 accepts only the functions), Lua functions `function(t) return t*t end`, and
  springs (`ease = anime.createSpring({ bounce = .5, duration = 400 })`: the animation lasts the
  settling time). An unknown name logs a warning and uses `linear`.
- **utils**: `set get remove random createSeededRandom randomPick shuffle clamp round snap wrap
  mapRange lerp damp degToRad radToDeg roundPad padStart padEnd stagger`; with fewer arguments a
  util returns a function of the missing value (`modifier = anime.utils.round(0)`).
- **Clock**: everything runs on the UI clock, like CSS transitions: `Engine.timeScale` does not
  slow it (a pause menu animates while the game is frozen), `playbackRate`/`pause()` do.
  `clock = "game"` (animations, timelines, timers; children inherit it) runs on the scaled game
  time instead, so UI that follows gameplay freezes with the game. Any other value is an error.
- **Lifetime**: stopping play (and engine shutdown) cancels every animation and frees the Lua
  functions and tables they hold; handles kept in globals stay valid but cancelled. A full
  reload of a document (its HTML changed) replaces its elements: animations of the old ones
  are cancelled, start them again in `onLoad`.
- **Differences from Anime.js**: string forms of `cubicBezier/steps/linear/irregular/spring`
  accepted; `andThen` for `then` (no Promises); no SVG, draggable, scroll, layout, text, WAAPI,
  scope or animatable modules; modifiers return numbers; a timer starts at the last frame time
  (Anime's 12 ms head start is not needed); plain tables get colours back as `#rrggbbaa`.

## Documents on meshes

A document can render into a texture instead of over the screen, and that texture can go on
a mesh (a screen in the 3D world, a computer terminal) or in an `Image`. Pointer input on the
mesh reaches the document.

| Component field | Meaning |
|---|---|
| `renderMode` | `SCREEN` (default) or `TEXTURE` |
| `textureWidth`, `textureHeight` | texture size in pixels (default 1024 x 1024, at most 8192) |
| `pixelRatio` | texture pixels per CSS px (0 = 1). The document is laid out at `textureWidth / pixelRatio` x `textureHeight / pixelRatio` CSS px: a 1024 x 512 texture at ratio 2 is a 512 x 256 px page drawn sharp at 2x |
| `inputSurface` | the entity whose mesh is hit-tested for pointer input; none = the document's own entity |
| `interactive` | false: the document gets no input |

```lua
function Terminal:init()
    self.screen = Primitive(self.scene)
    self.screen:createWall(1.6, 0.9)
    local doc = UIDocument(self.scene, self.entity)
    local c = doc:getUIDocumentComponent()
    c.renderMode = UIDocumentRenderMode.TEXTURE
    c.textureWidth, c.textureHeight, c.pixelRatio = 1280, 720, 2
    c.inputSurface = self.screen.entity
    c.needReload = true                          -- a mode change applies at the next load
    doc.onLoad = function()
        if c.renderMode == UIDocumentRenderMode.TEXTURE then
            self.screen:setTexture(doc:getTargetTexture())   -- Mesh, Primitive, or Image:setTexture
        end
    end
end
```

- `getTargetTexture()` (Lua and C++ `UIDocument`) returns a `Framebuffer`: RGBA8,
  **straight alpha** (what `Image` and mesh materials expect), top-left origin (a wall,
  plane or sprite shows it upright), linear filtering. It is `nil`/`nullptr` for a SCREEN
  document. The object stays the same while the component exists: give it to a mesh once.
  Size and ratio changes apply at once (the texture is recreated, consumers follow).
- The texture is redrawn only when the document changed. A hidden document (`hide()`,
  disabled component or entity) leaves a transparent texture. The document is not drawn
  over the screen.
- Each TEXTURE document has its own layout context: its own size, its own `@media` view,
  its own focus. Everything else is the same: Lua element API, data models, events,
  animations, fonts (text is rasterized at the document's `pixelRatio`).
- Pointer input: a click that no screen document and no component UI took is cast as a ray
  from the scene's camera; the nearest TEXTURE document whose surface it hits gets it at the
  hit's texture coordinate. The usual rules apply: over an element (or a body with a background) the
  document takes the event and the game does not get it; over a transparent body it passes
  on (to a document further back, then to the game). A press keeps the pointer until the
  release, even when it leaves the surface (drags). Touch works the same way per finger.
  Wheel goes to the document under the mouse. Keys, text and IME go to the document that
  took the last press (screen or world); a press elsewhere takes them away.
- The hit test reads the mesh's CPU geometry (primitives, sprites, imported models) with its
  texture coordinates; it ignores other objects between the camera and the surface, and
  skinned or morph-target meshes are tested in their bind pose. Screen documents and the
  component UI are drawn over the 3D world and get pointer events first.
- The mouse cursor shape (`cursor`) follows screen documents only.

## Editor workflow

- **Create**: *Assets > Create > HTML Document* writes `NewDocument.html` and
  `NewDocument.css` (a `<link>` to the stylesheet, a heading and a button) in the Project
  window's folder; *Create > UI > UI Document* in the Hierarchy adds the GameObject. Drag the
  `.html` onto the Inspector's **Source** field (or use its folder button: `.html`, `.htm`,
  `.rml` inside the assets directory).
- **Inspector**: Source with **Open** (in the code editor) and **Reload** (keeps scroll, focus
  and form values), Sort Order, Interactive, Render Mode, Texture Width/Height and Input
  Surface (texture mode), Pixel Ratio ("Auto" = 0), the **Messages** RmlUi logged for this
  document (warnings and errors, with `file:line` in the tooltip when RmlUi gives one) and a
  **Show HTML UI Debugger** button.
- **Project window**: `.html`/`.htm`/`.rml` and `.css`/`.rcss` files have their own icons and
  open in the code editor with HTML and CSS highlighting (`<style>` blocks and `style=""`
  attributes highlighted as CSS) and RCSS property suggestions. `list_assets` /
  `get_asset_info` report them as types `html` and `css`.
- **Rename/move**: renaming or moving the `.html` updates the components that use it.
  References *inside* HTML/CSS files are not rewritten (phase 4).
- **RmlUi debugger**: the Game view toolbar's *HTML UI Debugger* (while playing) and the
  Inspector button show RmlUi's element inspector and log over that scene's documents. It is
  global: showing it in one scene hides it in the other. It is compiled into the editor, Debug
  exports and the tests only (`JASPERENGINE_HTMLUI_DEBUGGER`); release games have no debugger.
- **MSAA**: *Project Settings > Graphics > HTML UI MSAA* (`graphics.html_ui_msaa_samples`:
  1, 2, 4 or 8, default 4) sets the multisampling of every document target, in the editor at
  once and in exports.
- **Export check**: an export lists every reference of every UI Document of its scenes
  (`<link>`, `<img>`, `<svg src>`, `<lottie src>`, templates, `@import`, `url(...)`,
  `@font-face`, `decorator: image()/svg()`, recursively through stylesheets) and **fails**
  with `assets/ui/menu.html:12:18: ...` for a missing file, a path that leaves the assets
  directory or a remote `http(s)://` URL; an absolute path is a warning. RCSS's bare
  `@font-face { src: fonts/a.ttf; }` is opened from the assets directory, as RmlUi does.

## Remote resources (imports)

A document may link what a page for a browser links: Google Fonts, CDN stylesheets (Font
Awesome, Bootstrap Icons, animate.css), remote images, LottieFiles animations. The editor
downloads each of them once into `assets/htmlui-imports/` and records it in
`assets/htmlui-imports/manifest.json`; at runtime the URL resolves through the manifest to the
local file, so the document stays as written and exported games work offline (the manifest and
the files ship with the assets).

- **Automatic**: when the editor loads or reloads a document (scene open, play, an Inspector
  change, a hot reload) the URLs missing from the manifest are downloaded on a background
  worker (`[HTML UI] importing <url>` in the Console), then the document reloads.
  *Editor Settings > General > HTML UI > Import Remote Resources* turns it off. A URL that
  failed is not retried automatically in the same session.
- **Inspector** (UI Document): *Remote resources: 4 imported, 1 missing* with **Import**
  (missing and failed ones, retried), **Re-import all** and **Remove unused** (drops the
  manifest entries no `.html`/`.css` of the project references and deletes their files), the
  status of every URL (tooltip: local file, license, error), and the document's diagnostics
  count with **Show in Console**.
- **Command line** (CI, agents): `jasperengine-editor import-ui --project <dir> assets/ui/menu.html
  [more documents] [--dest <dir>] [--force] [-q]` imports synchronously, prints the diagnostics
  compiler-style and exits 1 when an import failed (2 for a usage error).
- **What each kind becomes**:

| Remote resource | Local files |
|---|---|
| Google Fonts `fonts.googleapis.com/css2`, `/css`, `/icon` | one static TTF per weight/style (`fonts/<family>/<Family>-700-italic.ttf`; ranges such as `wght@100..900` give one file per 100), a local CSS with the same `@font-face` rules (`fonts/<families>-<hash>.css`), each family's license (`OFL.txt`/`LICENSE.txt`) |
| other stylesheets | `css/<host>/<path>.css` with every `url()` and `@import` it uses downloaded into the same tree and rewritten to relative paths; `@font-face` keeps the TTF/OTF sources (WOFF/WOFF2-only fonts: `HTMLUI-IMP-0007`, convert them) |
| images | `images/<host>/<hash>-<name>.<ext>`, the type from the content (PNG, JPEG, GIF, BMP, TGA, SVG); WebP/AVIF are saved but the engine cannot draw them (`HTMLUI-IMP-0003`) |
| Lottie `.json`, `.lottie`, LottieFiles pages | `lottie/<hash>-<name>/animation.json` with its images in `images/` |
| Lucide icons (`data-lucide`) | nothing: they are built into the engine |

- **Licenses**: Google Fonts record their license file (shipped with the game); LottieFiles
  animations record the Lottie Simple License, to verify on lottiefiles.com; other images and
  stylesheets record "unknown". Each import says so in the Console (`HTMLUI-IMP-0008`).
- **Offline**: a failed download is an `HTMLUI-IMP-0002` error in the editor and writes
  nothing; a URL without a manifest entry is `HTMLUI-IMP-0001` at runtime and falls back (a
  stylesheet or font is skipped and the font chain is used, an image is not drawn, a `<lottie>`
  stays empty); an entry whose file is gone is `HTMLUI-IMP-0009` (re-import). curl is the
  download tool (Windows 10 1803+, macOS and Linux have it; missing: `HTMLUI-IMP-0005`).

## Hot reload

- The editor checks the files every document of every open scene (playing copies included)
  depends on every 0.25 s, and at once after a save in its code editor: the document, its
  stylesheets, templates, images, fonts, SVG and Lottie files (what the document refers to,
  plus everything RmlUi opened while loading it). Files that did not exist yet count too:
  creating a missing stylesheet loads it.
- What a change does:

| Changed | Effect |
|---|---|
| a `.css`/`.rcss` | the document's stylesheets reload in place: the DOM, element handles, listeners, scroll and focus stay |
| an image | the image loads again the next time it draws |
| the document, a template, an SVG, a Lottie file | the document reloads, keeping, for every element with an `id`, its scroll position, form value (text, checkbox/radio, `<select>`), and the focused element with its selection; listeners on elements with an `id` (and on the document) move to the new elements, others are dropped with one warning; `onUnload` then `onLoad` fire |
| a font | the font is parsed again and the document reloads as above |

- Edit mode reloads too (no Lua runs there); a reload is not an edit: the scene stays saved.
- Engine entry point: `UIDocumentSystem::reloadChangedFiles(files)` with per-document file
  stamps (modification time and size recorded at each load), so whoever calls it first does
  the reload and a second caller (the MCP auto-sync) finds nothing to do.
  `getDocumentFiles(entity)`, `getLoadCount(entity)` and `getDocumentMessages(entity)`
  expose the same data; `reloadDocument(entity, preserveState)` reloads one document.
- A full reload builds the document from the file again: what scripts changed in it (classes,
  inline styles, created elements) and `anime` animations of its elements are not kept.
  Give elements whose state or listeners matter an `id`, and redo in `onLoad` what scripts
  changed (see "Scripting > Lifetimes").

## Not yet, and limits

Later phases:
- Phase 2, standard HTML and CSS: HTML5 parsing (no XML strictness), a user-agent stylesheet,
  CSS rewritten to RCSS (`border: 1px solid red`, `rgba(..., 0.5)`, `background-image: url()`,
  quoted font names, `200ms ease` transitions), standard `@font-face` and `text-shadow`,
  diagnostics with `file:line:column` and stable codes, `<script type="text/lua">` in
  documents, built-in Lucide icons.
- Phase 3, RmlUi patches: `calc()`/`min()`/`max()`/`clamp()`, grid, `::before`/`::after`,
  border styles, `%` radii, `!important`, CSS-wide keywords, `:root`/`:is`/`:where`/`:has`,
  real `position: fixed`/`sticky`, group `opacity`, `cubic-bezier()`/`steps()` in transitions.
- Phase 4: `check --ui`, the MCP tools `inspect_ui`/`assert_ui`/`create_ui_document`,
  rewriting references inside HTML/CSS when an asset is renamed, the generated support matrix.
- Phase 5: the effects library (`effect()` in CSS) and `@property`.

Limits of phase 1, by design:
- Linux has no multi-touch (core X11 input). The IME composition shows inside the field on
  Windows, macOS, iOS, Android and Web; on Linux the input method draws it, and in the editor's
  Game view composed text arrives committed.
- Lottie: no expressions (they need JavaScript), no audio layers, no dotLottie themes or state
  machines; ZIP64, encrypted and multi-disk `.lottie` archives are rejected with an `error`.
- SVG: no SVG filters, no SMIL animation, no external images (`data:` URIs only), no system fonts.
- Gradients take at most 16 colour stops.
- Documents on meshes: the hit test uses the bind pose of skinned and morph-target meshes and
  ignores objects between the camera and the surface; the mouse cursor follows screen documents
  only.
- Events `mouseenter`, `mouseleave`, `pointer*`, `touch*`, `contextmenu`, `beforeinput` and
  `composition*` are not dispatched (`addEventListener` warns).
- A full reload keeps scroll, form values, focus and the listeners of elements with an id, not
  what scripts changed in the document.
- No built-in serif or monospace face; a character no loaded font covers draws a missing-glyph
  box.
