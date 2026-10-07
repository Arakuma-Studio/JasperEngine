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

## Animation

A global Lua table `anime` mirrors [Anime.js v4](https://animejs.com/documentation/) (ported
from 4.5.0, checked against reference values generated from the real library). C++ gets the
same API through `Anime` (`#include "htmlui/anim/Anime.h"`).

```lua
-- targets: a CSS selector (every loaded document of the current scene), a UIElement, a plain
-- table, or a list of them
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
  functions and tables they hold; handles kept in globals stay valid but cancelled.
- **Differences from Anime.js**: string forms of `cubicBezier/steps/linear/irregular/spring`
  accepted; `andThen` for `then` (no Promises); no SVG, draggable, scroll, layout, text, WAAPI,
  scope or animatable modules; modifiers return numbers; a timer starts at the last frame time
  (Anime's 12 ms head start is not needed); plain tables get colours back as `#rrggbbaa`.

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
- Scripting: the Lua API (`document:getElementById`, `addEventListener`, ...), data binding
  (phase 1); `<script>` in documents (phase 2).
- Editor: documents in the Scene view, hot reload on save, an Inspector file picker and
  "Create > UI > UI Document", render-to-texture / world-space documents, the in-editor
  RmlUi debugger toggle (phase 1); `check --ui` and the inspect/assert MCP tools (phase 4).
- SVG and Lottie on the decided libraries (LunaSVG, ThorVG) (phase 1).
