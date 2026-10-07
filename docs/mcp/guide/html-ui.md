# HTML/CSS UI (UI Document)

Served as `jasper://docs/html-ui`.

A **UI Document** (`UIDocumentComponent`, YAML/MCP name `uidocument`, Unity's UIDocument) shows
an HTML file over the scene, laid out and drawn by the engine (RmlUi 6.3 inside, drawn on the
engine's own renderer on every backend: no browser, no JavaScript). It coexists with the
component UI (Text, Image, Button, ...).

This page describes **phase 0** (the foundation). Documents are XML-well-formed HTML styled
with **RCSS**, RmlUi's dialect of CSS: standard HTML5 parsing and CSS rewriting come with
phase 2, so for now write the subset below exactly as shown. Anything else is listed under
"Not yet".

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
  No `http(s)://` resources: everything a document uses is a project file.
- Images: PNG, JPG (and the other formats the engine loads) and simple SVG files (drawn by the
  engine's current SVG rasterizer; full SVG comes with phase 1).

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
- Give elements whose state matters an `id`, and attach listeners in `onLoad` (or to
  elements with an `id`).

## Writing documents in phase 0 (RCSS)

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
| `<span>Red </span><span>Green</span>` (space inside the text) | `<span>Red</span> <span>Green</span>` (whitespace-only text between elements is dropped) |
| `transition: background-color 0.2s cubic-in-out;` on class / pseudo-class changes | `transition: opacity 200ms ease;` |

What works:
- Box model: `width`/`height` (px, %, em), `min-*`/`max-*`, `margin`, `padding`, `border`
  widths and colors per side, `border-radius`, `box-sizing`, `background-color`,
  `position: absolute/relative` with `left/top/right/bottom/inset`, `z-index`, `overflow:
  hidden/auto/scroll` (rectangular clipping), `visibility`, `opacity` (multiplies each
  element's colors; not a group).
- Flexbox: `display: flex`, `flex-direction`, `flex-wrap`, `justify-content`, `align-items`,
  `align-content`, `align-self`, `flex` and `flex-grow/shrink/basis`, `gap` (no `order`).
- Transforms: `transform` (`translate`, `rotate`, `scale`, `skew`, `perspective`,
  `rotateX/Y`, ...), `transform-origin`, `perspective`.
- Images: `<img src="...">` and `decorator: image(...)`, with alpha.
- Text: `font-size`, `color`, `font-weight` and `font-style` (with loaded faces, below),
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
- Pointer input: mouse buttons, mouse move and the first touch, so `:hover`/`:active` work in
  play mode and in exported games. A press or release over an element of a document is the
  document's: the game's `onMouseDown`/`onMouseUp`/`onMouseMove` (and the component UI under
  it) do not get it when `ignoreEventsHandledByUI` is on (the default). Over the transparent
  `body` the game gets the event. Give a full-screen wrapper `pointer-events: none` to make
  it click-through (its children can set `pointer-events: auto`).

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
  (CSS weight matching); without one the regular face is used (no synthetic bold/italic yet).
  `@font-face` is phase 2.

## C++ API

`UIDocument` (a GameObject; `#include "JasperEngine.h"`) and `UIElement` (a weak handle: it
turns invalid when its element or document goes, and every method then does nothing):

```cpp
UIDocument menu(scene);                     // adds a UIDocumentComponent
menu.load("ui/menu.html");                  // loads now (false if it failed); setFilename loads at the next update
menu.setSortOrder(10);
menu.hide(); menu.show();                   // = enabled
UIElement title = menu.getElementById("title");
title.setTextContent("Paused");
title.setClass("highlight", true);
title.setStyleProperty("color", "#ffcc00"); // false (and a warning) if the value is rejected
Rect box = title.getBoundingBox();          // canvas pixels, top-left origin
bool hovered = menu.getElementById("play").isPseudoClassSet("hover");
```

`UIElement` also has `getId`, `getTagName`, `getTextContent`, `getInnerHTML`/`setInnerHTML`
(RML markup), `hasAttribute`/`getAttribute`/`setAttribute`/`removeAttribute`, `hasClass`,
`removeStyleProperty`. There is no Lua API and no event listener API yet (phase 1): poll
`isPseudoClassSet("active")` or use the game's own input events.

## Not yet (phase 0 limits)

- Standard HTML and CSS: HTML5 parsing, a user-agent stylesheet, CSS rewriting and
  diagnostics with `file:line:column` (phase 2); `calc()`, grid, `::before`/`::after`,
  border styles, `%` radii, `!important`, `:root`, `:is`/`:where`/`:has` (phase 3).
- Rendering: clipping to rounded corners and inside transforms, `box-shadow`, gradients,
  filters (`blur`, `drop-shadow`), `mask-image`, group `opacity` (phase 1).
- Text: font effects (`font-effect`: shadow, outline, glow), synthetic bold and italic,
  `@font-face` (phases 1-2).
- Input: keyboard, focus, text fields, mouse wheel, enter/leave, multi-touch, cursor shapes,
  clipboard, IME (phase 1).
- Scripting: the Lua API (`document:getElementById`, `addEventListener`, ...), data binding,
  the animation API (phase 1); `<script>` in documents (phase 2).
- Editor: `check --ui` and the inspect/assert MCP tools; rewriting references inside
  HTML/CSS on rename (phase 4).
- SVG and Lottie on the decided libraries (LunaSVG, ThorVG) (phase 1).
