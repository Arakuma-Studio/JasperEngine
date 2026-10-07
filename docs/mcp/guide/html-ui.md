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
    local play = doc:getElementById("play")
    play:addEventListener("click", function(event)
        play.classList:add("pressed")
        play.textContent = "Loading..."
        self.shop.gold = self.shop.gold - 1        -- {{ gold }} updates
    end)
end

return Menu
```

Documents load with their scene, so `init` can query elements. A script that runs before
the document loaded (C++ code before the scene loads) creates its data models then, and
queries elements in `doc.onLoad` (an event: `doc.onLoad = function() ... end` or
`RegisterEvent(self, doc.onLoad, "method")`), which runs after every load and reload
(`onUnload` before every unload).

### Element API (Lua; C++ has the same names as get/set methods)

| Group | Members |
|---|---|
| Document | `doc:getElementById(id)`, `doc:querySelector(sel)`, `doc:querySelectorAll(sel)` (array), `doc.body`, `doc:createElement(tag)`, `doc:createTextNode(text)`, `doc.title`, `doc:addEventListener/removeEventListener`, `doc:createDataModel(name, table)`, `doc:getDataModel(name)`, `doc:removeDataModel(name)`, `doc:load(file)`, `doc:reload()`, `doc.loaded`, `doc:show()/hide()`, `doc.visible`, `doc.sortOrder`, `doc.filename`, `doc.onLoad/onUnload`, `UIDocument.setDebuggerVisible(bool)` |
| Identity / tree | `id`, `tagName` (lower case), `className`, `parentElement`, `children` (array), `firstElementChild`, `lastElementChild`, `nextElementSibling`, `previousElementSibling`, `childElementCount`, `querySelector`, `querySelectorAll`, `closest`, `matches`, `contains` |
| Mutation | `appendChild(el)`, `insertBefore(el, ref)` (ref nil appends), `removeChild(el)`, `replaceChild(new, old)`, `remove()`, `cloneNode(deep)` |
| Content | `textContent` (text, never parsed), `innerHTML` (markup; RML in phase 1) |
| Attributes | `getAttribute(name)` (nil when absent), `setAttribute`, `hasAttribute`, `removeAttribute`, `getAttributes()` |
| Classes / style / data | `classList:add(...)/remove(...)/toggle(name[, force])/contains(name)`; `style.backgroundColor` or `style["background-color"]` (read: computed value as CSS text; write: inline style, `""`/nil removes), `style:getPropertyValue/setProperty/removeProperty`; `dataset.userName` = `data-user-name` |
| Forms | `value`, `checked`, `disabled` |
| Focus / interaction | `focus()`, `blur()`, `click()` |
| Scroll / geometry | `scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight`, `clientWidth`, `clientHeight`, `offsetWidth`, `offsetHeight`, `offsetLeft`, `offsetTop`, `scrollIntoView([alignToTop])`, `scrollTo(x, y[, smooth])`, `scrollBy(dx, dy[, smooth])`, `getBoundingClientRect()` (`x y width height top left right bottom`, document px, through the transforms of the last drawn frame) |
| Events | `addEventListener(type, fn[, capture or {capture=, once=}])`, `removeEventListener(type, fn[, capture])`, `dispatchEvent(type[, detail[, bubbles]])` (false when a listener prevented it) |

Queries that find nothing return `nil`. Adding the same function for the same type and
phase twice is ignored, as in the DOM.

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
  listener again in `onLoad`.

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
- Scripting: `<script type="text/lua">` in documents (phase 2).
- Editor: documents in the Scene view, hot reload on save, an Inspector file picker and
  "Create > UI > UI Document", render-to-texture / world-space documents, the in-editor
  RmlUi debugger toggle (phase 1); `check --ui` and the inspect/assert MCP tools (phase 4).
- SVG and Lottie on the decided libraries (LunaSVG, ThorVG) (phase 1).
