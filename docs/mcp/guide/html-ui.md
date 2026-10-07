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
- Images: PNG, JPG (and the other formats the engine loads) and SVG files (LunaSVG, see
  "SVG and Lottie"); Lottie animations as `.json` or `.lottie`.

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
  `<style>` and `style=""`, `currentColor` (black unless the SVG's own CSS sets `color`),
  opacity, `<text>` in the built-in Roboto (any `font-family` uses it).
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
| `load` | the animation was read (a frame after the document loaded, so listeners added after loading get it) |
| `error` | the file is missing or unreadable, the `.lottie` is invalid, `animation` names no animation; the event's `message` parameter (and the Console) says which |
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
-- menu: the UIDocument of the entity (see "Scripting")
local spinner = menu:getElementById("spinner")
spinner:addEventListener("complete", function(event) print("done") end)
spinner:setLoopCount(2)
spinner:play()
```

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
- Editor: documents in the Scene view, hot reload on save, an Inspector file picker and
  "Create > UI > UI Document", render-to-texture / world-space documents, the in-editor
  RmlUi debugger toggle (phase 1); `check --ui` and the inspect/assert MCP tools (phase 4).
