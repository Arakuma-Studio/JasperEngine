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

- One document per component. All documents of a scene share one layout context: they are
  laid out on the project canvas (`canvasWidth` x `canvasHeight`), CSS `px` = canvas pixels.
  With a scaled canvas (letterbox, crop, high-DPI screens) the document is drawn at screen
  resolution, so text and edges stay sharp.
- Drawn over the scene in the scene's main camera pass, **in front of the component UI**, in
  the Game view (edit mode and play mode) and in exported games. A scene stacked as a layer
  draws its documents over the scenes below it.
- The **Scene view does not show documents** yet (phase 1); use `screenshot view=game`.
- Saved with the scene (`uidocument:` with `filename`, `sortOrder`, `enabled`), copied with
  copy/paste, duplicate and prefabs, exported with the game (every file under the assets
  directory ships, `.html`/`.css` included).
- A missing or unreadable file logs `[HTML UI] could not load document '<path>'` to the
  Console and draws nothing; nothing crashes. Every HTML UI Console line starts with
  `[HTML UI]`: read the Console after changing a document.
- Editing: `write_file` (or your own file tools) then load the document again: set
  `filename` (to `""` and back), reopen the scene, or start play mode. A load always reads the
  `.html` and `.css` files as they are on disk. Images are cached per scene: a replaced image
  shows after the scene is reopened or in play mode. There is no automatic hot reload yet.

## Files and paths

- Keep documents, stylesheets, images and fonts under the assets directory (`assets/ui/...`).
- References inside a document are relative to the document (`href="menu.css"`,
  `src="../images/logo.png"`); `asset://ui/logo.png` is relative to the assets directory.
  No `http(s)://` resources: everything a document uses is a project file.
- Images: PNG, JPG (and the other formats the engine loads) and simple SVG files (drawn by the
  engine's current SVG rasterizer; full SVG comes with phase 1).

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
- Input: see "Input" below.

## Input

Documents get the mouse (buttons, move, wheel, enter/leave), multi-touch, the keyboard, text
(IME included) and the clipboard, in play mode, in exported games and through MCP
`send_input`. They are offered every event **before** the component UI and the game, top
scene layer first.

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
- Scripting: the Lua API (`document:getElementById`, `addEventListener`, ...), data binding,
  the animation API (phase 1); `<script>` in documents (phase 2).
- Editor: documents in the Scene view, hot reload on save, an Inspector file picker and
  "Create > UI > UI Document", render-to-texture / world-space documents, the in-editor
  RmlUi debugger toggle (phase 1); `check --ui` and the inspect/assert MCP tools (phase 4).
- SVG and Lottie on the decided libraries (LunaSVG, ThorVG) (phase 1).
