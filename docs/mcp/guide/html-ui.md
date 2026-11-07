# HTML/CSS UI (UI Document)

Served as `jasper://docs/html-ui`. What every CSS property, value, selector and element does in
the engine, and every diagnostic code: `jasper://docs/html-ui-support` (generated from the
engine's own tables, so it is always current). Complete examples to copy: every project's
`.jasperengine/engine-api/html-ui-examples/` and `jasper://examples/html-ui`.

Game UI is written as **standard HTML5 and CSS**, the way you would write it for Chrome, plus
**Lua** instead of JavaScript. The engine parses, styles, lays out and draws it itself (no
browser), reports everything it does not draw like Chrome with `file:line:col`, a stable code
and a fix, and reloads documents as you save them.

## HTML/CSS UI in JasperEngine

**When to use it.** Menus, HUDs, dialogs, inventories, settings, shops, toasts and loading
screens: HTML/CSS. The component UI (`create_ui`: Text, Image, Button, Panel, layout groups,
placed in pixels) still exists and still works; documents draw in front of it. Use the
component UI for a few pixel-placed widgets, HTML/CSS for anything with layout, styling,
lists, forms or animation. Edit mode runs no Lua: a document's script logic runs only in play.

### UI Documents: the model

- A **UI Document** is a component (`UIDocumentComponent`, YAML/MCP name `uidocument`,
  Unity's UIDocument) showing one `.html` file. One document per GameObject: make it with
  `create_ui_document`, or `create_entity type=ui_document` and set `filename`.
- It draws over its scene's camera in the Game view (edit mode and play mode) and in the
  exported game, in front of the component UI. A scene stacked as a layer draws its documents
  over the scenes below; within a scene a higher `sortOrder` draws on top. A scene's screen
  documents draw together into one offscreen target that the camera composites (post-process
  applies to it, as to the component UI).
- **CSS `px` = canvas pixels.** The page is the project canvas (`canvasWidth` x
  `canvasHeight`): `100vw` x `100vh` is the canvas and media queries see its size. With a
  scaled canvas (letterbox, high-DPI screens) the document is drawn at screen resolution, so
  text and edges stay sharp.
- A document can also render into a **texture** for a mesh or an `Image` (`renderMode:
  texture`, below).
- **No browser and no JavaScript.** The layout engine is a fork of RmlUi 6.3 drawn with the
  engine's own renderer on every backend. Logic is Lua: a script on the GameObject
  (`UIDocument(self.scene, self.entity)`) or `<script type="text/lua">` in the document.
  C++ has the same API.
- **Edit mode has no Lua.** Documents load, lay out and draw in the editor without playing,
  but scripts, listeners and data models run only in play: dynamic content (`{{ }}` values,
  lists, text set from Lua) appears in play.

| Component property | Type | Meaning |
|---|---|---|
| `filename` | string | The `.html` document (or `.rml`, RmlUi's own dialect). MCP takes and returns project-relative paths under the assets directory (`assets/ui/menu.html`); the scene stores the asset-relative path (`ui/menu.html`). Empty = no document. Inspector label "Source". |
| `sortOrder` | int | Higher draws on top (default 0). |
| `enabled` | bool | Shown and receiving input (the Inspector's checkbox; Lua `doc.visible`, `show()`, `hide()`). |
| `interactive` | bool | Receives pointer, keyboard and text input (default true). Off: every event goes to the game. |
| `renderMode` | enum | `screen` (default) or `texture`. |
| `textureWidth`, `textureHeight` | int | `texture` mode: texture size, 1..8192 (default 1024 x 1024). |
| `inputSurface` | entity | `texture` mode: the mesh whose UVs receive the pointer; none = the document's own GameObject. |
| `pixelRatio` | float | Document pixels per CSS pixel; 0 = automatic (the canvas-to-screen scale; 1 in `texture` mode). |

- Saved with the scene (YAML key `uidocument`; properties written only when they differ from
  their defaults), copied with copy/paste, duplicate and prefabs (with overrides), exported with
  the game. A missing or unreadable file logs
  `[HTML UI] could not load document '<path>'` and draws nothing; nothing crashes. Every
  HTML UI Console line starts with `[HTML UI]`.
- The Scene view of UI and 2D scenes shows a scene's documents in place on the canvas
  rectangle (outlined while a UI Document is selected; select documents in the Hierarchy).
  A 3D scene's Scene view has no canvas: its documents show in the Game view.

### What "Chromium parity" means, and where it stops

The engine parses HTML like a browser (unclosed `<p>`, unquoted attributes and every named
entity are fine), applies a **user-agent stylesheet equal to Chromium's defaults**, and
translates the CSS for its layout engine, whose block, inline, flex, grid, float and
positioned layout, cascade, selectors, transitions and animations follow the CSS specs. Its
golden pages (a main menu, a HUD, a settings screen, a confirm dialog, unstyled HTML, a
gallery of 48 CSS rewrites) are compared pixel by pixel and box by box with Chromium 153: a
page written for Chrome at the canvas size looks the same.

Parity stops at:

- **No JavaScript, no network access from documents, no `<canvas>` or `<iframe>`** (by design;
  Lua's `Http` makes requests, `jasper://docs/networking`).
  `<video>` plays AV1 + Opus WebM everywhere and MP4 through the platform's decoders (see "Video"). Remote fonts, images and videos are
  imported into the project by the editor.
- **The unsupported CSS** listed under "CSS that differs": container queries, blend modes,
  vertical text, multi-column layout, `subgrid`, scroll snapping, ... Each use is reported.
- **Small differences without a diagnostic**, also listed there: whole-pixel font sizes,
  table column sizing, no built-in monospace or serif face.

Everything else that is not drawn exactly like Chrome is reported where you wrote it, with a
code and a fix ("The loop in detail"). Zero warnings means nothing you wrote was dropped or
approximated: what remains are the few differences without a diagnostic listed under "CSS that
differs".

### Documents on meshes (texture mode)

A document can render into a texture shown on a mesh (a screen in the 3D world, a terminal)
or in an `Image`; pointer input on the mesh reaches the document. A script on the document's
GameObject:

`scripts/Terminal.lua`:

```lua
---@class Terminal : ScriptInstance
local Terminal = { properties = {} }

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
        local texture = doc:getTargetTexture()
        if texture then self.screen:setTexture(texture) end   -- Mesh, Primitive or Image
    end
end

return Terminal
```

- The document is laid out at `textureWidth / pixelRatio` x `textureHeight / pixelRatio`
  CSS px (a 1280 x 720 texture at ratio 2 is a 640 x 360 px page drawn sharp at 2x;
  `pixelRatio` 0 = 1). Each texture document has its own layout context: its own size,
  `@media` view and focus; everything else (Lua API, data models, events, animations, fonts)
  is the same.
- `getTargetTexture()` returns a `Framebuffer` (RGBA8, **straight alpha**, top-left origin,
  linear filtering), `nil` for a screen document. It is the same object while the component
  exists: give it to the mesh once. Size and ratio changes apply at once; a `renderMode`
  change at the next load.
- The texture is redrawn only when the document changed. A hidden document leaves a
  transparent texture; a texture document is not drawn over the screen.
- Pointer input: a press that no screen document and no component UI took is cast as a ray
  from the scene's camera; the nearest texture document whose surface it hits gets it at the
  hit's texture coordinate, with the usual rules (an element, or a page with a background,
  takes it; a transparent page passes it on). A press keeps the pointer until the release
  (drags); every finger is its own pointer (Pointer Events); the wheel goes to the document under the mouse; keys go to
  the document that took the last press. The hit test reads the mesh's CPU geometry
  (primitives, sprites, imported models), ignores other objects in front of the surface and
  tests skinned or morph-target meshes in their bind pose. The mouse cursor shape follows
  screen documents only.

## Quick start

A menu in a few calls (every call's result carries `external_changes` when files changed):

```text
create_ui_document {"html": "assets/ui/main_menu.html", "template": "main_menu"}
    -> a GameObject with a UI Document + assets/ui/main_menu.html, .css, main_menu_assets/, scripts/MainMenuUI.lua (attached)
(edit the .html / .css / .lua with your own file tools)
inspect_ui   {"selector": ".menu-button", "styles": ["font-size", "width"]}
    -> before it runs, external_changes {ui_reloaded, problems: [{path, line, col, severity, code, message, suggestion}]}
assert_ui    {"checks": [{"type": "text_fits", "selector": ".menu-button", "single_line": true},
                         {"type": "no_overlap", "selector": ".menu-button"},
                         {"type": "within_viewport", "selector": ".menu"}],
              "viewports": [[1280, 720], [720, 1280]]}
screenshot   {"view": "game"}
play         {"action": "start"}
send_input   {"events": [{"type": "mouse", "action": "click", "target": "#play"}]}
read_console {}
```

`template` is `blank` (default: an `.html` with a heading, its `.css`, no script) or one of the
examples ("Examples"). Fix every `problems` entry first: each names the line, what the engine
does with it and the fix. A page written by hand looks like this (its Lua is the complete script
under "A complete UI script"):

`assets/ui/menu.html`:

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Menu</title>
  <link rel="stylesheet" href="menu.css">
</head>
<body>
  <main class="menu">
    <h1 id="title" class="title">Skyward</h1>
    <nav class="buttons">
      <button id="play" class="btn primary"><i data-lucide="play"></i> Play</button>
      <button id="settings" class="btn"><i data-lucide="settings"></i> Settings</button>
      <button id="quit" class="btn"><i data-lucide="log-out"></i> Quit</button>
    </nav>
    <p id="status" class="status">Arrows or d-pad, Enter or A, or click</p>
  </main>
</body>
</html>
```

`assets/ui/menu.css`:

```css
:root { --accent: #7c5cff; --panel: rgba(18, 20, 32, 0.86); --text: #f4f6fb; }
* { box-sizing: border-box; }
html { background: radial-gradient(circle at 30% 20%, #2b2f5a, #0b0c16 70%); }
body { margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
       font-family: system-ui, sans-serif; color: var(--text); }
.menu { width: min(420px, 90vw); padding: 32px; display: flex; flex-direction: column; gap: 20px;
        background: var(--panel); border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 16px;
        box-shadow: 0 12px 40px rgba(0, 0, 0, 0.45); backdrop-filter: blur(8px); }
.title { margin: 0; font-size: clamp(32px, 6vw, 48px); letter-spacing: 0.08em; text-align: center;
         text-shadow: 0 0 16px rgba(124, 92, 255, 0.7); }
.buttons { display: flex; flex-direction: column; gap: 12px; }
.btn { display: flex; align-items: center; gap: 10px; padding: 12px 16px; font: inherit; font-size: 18px;
       color: var(--text); background-color: rgba(255, 255, 255, 0.06); border: 1px solid rgba(255, 255, 255, 0.15);
       border-radius: 10px; transition: background-color 150ms ease, translate 150ms ease; }
/* translate, not transform: the Lua entrance animation (anime) writes transform */
.btn:hover, .btn:focus-visible { background-color: rgba(124, 92, 255, 0.35); translate: 4px 0; }
.btn.primary { background-image: linear-gradient(135deg, var(--accent), #4f46e5); }
.btn .lucide { width: 20px; height: 20px; }
.status { margin: 0; text-align: center; opacity: 0.7; }
@media (max-width: 800px) { .menu { padding: 20px; } .btn { font-size: 16px; } }
```

The same by hand: write the files, then `create_entity {"type": "ui_document", "name": "Menu"}`,
`set_properties {"entity": "Menu", "values": {"uidocument": {"filename": "assets/ui/menu.html"}}}`
and `attach_script {"entity": "Menu", "path": "scripts/MenuUI.lua"}`.

## Files

- Documents and their stylesheets live in the assets directory: `assets/ui/<name>.html` and
  `assets/ui/<name>.css`; images, SVG, Lottie and font files of one document in
  `assets/ui/<name>_assets/` (what `create_ui_document` writes); the Lua script in the project's
  Lua directory (`scripts/<Name>UI.lua`). `check --ui` and the editor only treat `.html` files
  inside the assets directory as documents.
- References are relative to the file that contains them, as in a browser: `href="menu.css"`
  and `src="menu_assets/logo.svg"` from the document, a `url()` in a stylesheet relative to that
  stylesheet. A leading `/` (`/ui/logo.png`) or `asset://ui/logo.png` starts at the assets
  directory.
- Formats: PNG, JPG and the other image formats the engine loads (not WebP or AVIF), SVG,
  Lottie `.json` / `.lottie`, fonts TTF / OTF (not WOFF / WOFF2).
- **No remote URLs at run time.** An `http(s)://` font, stylesheet, image or animation is
  imported into the project by the editor ("Images, SVG, Lottie, fonts, icons"); until then
  it is an `HTMLUI-IMP-0001` error.
- **Export** ships every file of the assets directory (`.html`/`.css` and the import manifest
  included) and first checks every UI Document of its scenes: an error (a missing file, a
  remote URL that was not imported, JavaScript) fails the export with the finding
  (`assets/ui/menu.html:12:18: error: ... [HTMLUI-GEN-0001]`).
- Renaming or moving the `.html` in the editor updates the components that use it; references
  *inside* HTML/CSS files are not rewritten: fix them by hand (`check --ui` finds the broken
  ones).
- `list_assets` / `get_asset_info` report `.html/.htm/.rml` as type `html` and `.css/.rcss` as
  `css`.

## HTML

Write a complete document, `<!DOCTYPE html>` with a `<link>` to its stylesheet, as for a
browser. `<html>` is the root and `<body>` its child: `html {}`, `:root {}`, `body {}`,
`rem` and `html { font-size: 62.5% }` behave as in CSS. The page background is
**transparent**, not white (the UI draws over the game): give `html` or `body` a background
for a full-screen menu (it covers the whole canvas, as in CSS). A transparent page lets the
pointer through to what is below ("Input and focus").

### Elements and default styles

Elements and their default styles come from Chromium's user-agent stylesheet: headings,
paragraphs, lists with markers, tables, links (`#0000ee`, underlined), `body { margin: 8px }`,
and form controls in Chromium's light theme (buttons, text fields, checkboxes and radios in the
accent color, sliders, `<select>`, `<progress>`), classic 15 px scrollbars and a focus ring.
Every rule you write beats every user-agent rule. Every element's status:
`jasper://docs/html-ui-support` ("Elements", "Attributes").

Form controls: `accent-color` colors checkboxes, radios, sliders and progress bars;
`appearance: none` removes a control's own look so your `background`, `border` and
`::-webkit-slider-thumb` styles draw alone, leaving what Chrome's default stylesheet gives it: a
checkbox or radio has no border, checked fill or size of its own (give it a `width` and
`height`, or it is sized by its `::before`/`::after`, padding and border: 0x0 without); a
`<select>` keeps its 1px gray border on a light gray background, without its arrow or inner
padding; a button gets a 2px outset border and `padding: 1px 6px`; a text field a 2px inset
border; a `<progress>` a gray bar with a green value. As in Chrome, checkbox, radio and range
inputs draw `::before` and `::after` (laid out in the input's box), so a switch is one element:
an `input[type="checkbox"]` with `appearance: none`, a size, a track `background` and the knob
its `::before`, moved by `:checked::before`; text and button inputs draw none, nor do the types
the engine approximates (`color`, the date and time types, `file`). `appearance` applies to the
element itself, not to the controls inside it. With its look a checkbox or radio is Chrome's:
your `background`, `border`, `padding` and `border-radius` on it have no effect (style it with
`appearance: none` instead), and in a box that is not 13x13 it is drawn as a square of the
smaller side, centered, darker when hovered and lighter when pressed. A button with `display: inline` is an inline-block, and a block-level
button (`display: block`, `flex`, …) is as wide as its label unless you give it a width, as in
Chrome (`margin: 0 auto` centers it). A button's content
and a `<select>`'s or a text field's line are centered vertically in a taller box, and
`border-radius` clips an `<img>`, `<svg>` or `<lottie>` picture to its rounded content box. As
in a browser, a reset such as `* { margin: 0; padding: 0 }` does not reach a control's inner
parts (slider track and thumb, select arrow, scrollbars): style those through their
pseudo-elements (`::-webkit-slider-thumb`, `::-webkit-scrollbar`, `::placeholder`).

| Element | Behavior |
|---|---|
| `<button>` | `type="submit"` (the default) submits its `<form>` (the `submit` event carries the button's `name`/`value` and every named control), `reset` resets it, `button` does nothing; `form="id"` works; `disabled` gets `:disabled` and no click; Enter and Space activate a focused button |
| `<input>` | `text`, `password` (shows `*`), `search`, `email`, `url`, `tel`; `number` (filters typing, Up/Down step by `step` within `min`/`max`, clamps on blur/Enter); `range` (a slider), `checkbox`, `radio` (Space toggles); `date`, `time`, `datetime-local`, `month`, `week` and `color` are text fields showing the format or value (no picker, no `::before`/`::after`, `HTMLUI-HTML-0008`); `image` submits and shows `src`; `file` is a disabled button; Enter in a text field submits its form; `autofocus` |
| `<select>`, `<textarea>`, `<progress>`, `<meter>` | a drop-down, a multi-line field, a bar (`meter` drawn as `progress`) |
| `<label>` | a click activates its control (`for="id"`, or the control inside it) |
| `<a href>` | Enter activates; `#id` scrolls to the element; another link dispatches `htmlnavigate` on the document (`href`, `url`, `fragment`, `document`, `external`): a `.html` link loads that document in the same component unless a listener calls `preventDefault()`; external links only notify |
| `<details>`/`<summary>` | clicking (or Enter/Space on) the summary toggles `open`, then a `toggle` event; details with the same `name` form an accordion |
| `<dialog>` | `open` shows it; Lua `dialog:showModal()`, `show()`, `close(value)`; Escape closes a modal one (`cancel`, then `close`). No backdrop and no focus trap: add a backdrop element |
| `<img>` | `object-fit`, `object-position`, `image-rendering: pixelated`; SVG files drawn as vectors at their size; `srcset`/`<picture>` resolved once at load |
| `<svg>` (inline), `<i data-lucide="name">`, `<lottie>` | vector images, built-in icons, Lottie animations ("Images, SVG, Lottie, fonts, icons") |
| `<ol>`/`<ul>`/`<li>` | markers from `list-style-type`, `list-style-image`, `list-style-position`, `ol start`, `reversed`, `li value`, `::marker`; any `display: list-item` element gets one |
| `<fieldset>`/`<legend>` | the legend sits on the top border, which leaves a gap for it, with any border, radius, padding and background; `fieldset disabled` disables its controls |
| `<table>` | tables, with the limits under "CSS that differs" (give tables and columns a width) |

**Instead of:**

| Browser | Engine |
|---|---|
| `<script>` (JavaScript) | `<script type="text/lua">` or a Lua script on the GameObject (`HTMLUI-HTML-0006`, error) |
| `onclick="..."` and every `on*` attribute | `el:addEventListener("click", fn)` in Lua, or `data-event-click` with a data model (`HTMLUI-HTML-0005`, error) |
| `<canvas>` | a Lottie animation or an image sequence; 3D content drawn by the game behind a transparent part of the page, or the document on a mesh in texture mode (`HTMLUI-HTML-0002`: a placeholder box) |
| `<video src="clip.avi">` (Ogg/Theora, AVI, ...) | `<video src="clip.webm">` with AV1 + Opus (`HTMLUI-HTML-0027`); MP4 (H.264, HEVC) plays through the platform's decoders |
| `<iframe>` | another UI Document (`HTMLUI-HTML-0002`) |
| `<div/>` | `<div></div>`: in HTML a non-void element does not self-close (`HTMLUI-HTML-0026`) |
| Tailwind's Play CDN, GSAP, jQuery `<script>`s | a built stylesheet (Tailwind CLI output), the Lua `anime` API, Lua (`HTMLUI-HTML-0007`) |

Lucide and LottieFiles player `<script>`s are not needed: both are built in (an info says so).
`<noscript>` content shows. Duplicate ids warn (`HTMLUI-HTML-0011`): `getElementById` finds
the first.

### `.rml` documents: RmlUi's own dialect

A file named `.rml` is not translated: RmlUi's own loader reads it. Use it for documents written
for RmlUi; everything else in this guide (rendering, input, scripting, animation, hot reload)
applies to both kinds. A `.html` file is always standard HTML (an RML document named `.html`
loads as HTML: `<div/>` does not close there, `HTMLUI-HTML-0026` warns): rename it `.rml`.

An `.rml` document is XML (close every tag, quote every attribute, escape `&` and `<`), has
**no user-agent stylesheet** (every element is inline until you give it `display: block`) and
its style language is **RCSS**: black 16px text by default, a border without a color takes the
text color, and:

| Write (RCSS) | Not |
|---|---|
| `border: 2px solid #ff0000;` (a style is needed, as in CSS) | `border: 2px #ff0000;` |
| `#rrggbb`, `#rrggbbaa`, `rgba(255, 0, 0, 128)` (alpha 0-255), or the CSS Color 4 forms `rgb(0 0 0 / 50%)` | `rgba(255, 0, 0, 0.5)` (reads as almost transparent) |
| `decorator: image(bg.png);` | `background-image: url(bg.png);` |
| `-rmlui-direction: rtl;` | `direction: rtl;` |
| `@font-face { font-family: Title; src: fonts/Title.ttf; }` (from the assets directory, no `url()`) | a `url()` relative to the stylesheet |

RCSS has the box model, flexbox (no `order`), transforms, transitions (RmlUi tween names such as
`cubic-in-out` too), `@keyframes`, `@media` (canvas width, height, orientation), `var()`,
`calc()`/`min()`/`max()`/`clamp()`, every CSS Color 4 color, logical properties, the selectors
of CSS 2 plus `:hover`, `:active`, `:nth-child()`, `:not()`, and RmlUi's own `decorator`,
`font-effect` and `@spritesheet`. Standard CSS has all of that and more, so prefer `.html`.

## CSS

Write standard CSS; the engine translates it, exactly in almost every case. What every
property, value, function, unit, selector and at-rule does: `jasper://docs/html-ui-support`.

### CSS that works

Native or exactly rewritten, as in Chrome:

- **Layout**: block, inline and `inline-block`, margins that collapse as in browsers, floats,
  `display: flow-root`/`contents`; **flexbox** (a port of Taffy's, checked against Chromium:
  `order`, `flex-basis: content`, `min-width: auto`, `flex-wrap`, every `align-*`/`justify-*`
  keyword incl. `safe`, `baseline`, `inline-flex`); **CSS Grid** (`grid-template-*` with `fr`,
  `minmax()`, `repeat(auto-fill|auto-fit, ...)`, line names, `grid-template-areas`,
  `grid-auto-*` incl. `dense`, spans, negative lines, `place-*`); `position: absolute`,
  `relative`, `fixed` (against the viewport; a transformed or filtered ancestor is its
  containing block, as in browsers) and `sticky`; `z-index`; `aspect-ratio`; `fit-content`/`min-content`/`max-content`/
  `stretch`; `box-sizing`; logical properties (left to right); `overflow` incl. `clip`,
  `overflow-clip-margin`, `scrollbar-gutter`; `visibility` (a hidden parent's `visible` child
  shows); `isolation`; `will-change`.
- **Values**: `calc()`, `min()`, `max()`, `clamp()` (computed by the layout engine in sizes,
  margins, paddings, insets, gaps, `flex-basis`, font sizes, transforms, custom properties;
  elsewhere at load and on resize); `round()`, `mod()`, `abs()`, `sin()` and the other math
  functions (at load); custom properties and `var()` (also inside shorthands and gradients);
  every CSS Color 4 color (`rgb(0 0 0 / 50%)`, `hsl()`, `oklch()`, `color-mix()`, the 148
  names, `currentColor` everywhere); units `px`, `%`, `em`, `rem`, `vw`, `vh`, `vmin`, `vmax`,
  `dvh`/`svh`/`lvh`, `ch`, `ex`, `deg`, `turn`, `grad`, `ms`, `s`; `inherit`, `initial`,
  `unset`, `revert`, `all`, `!important` (the real cascade: it beats more specific rules and
  the `style` attribute).
- **Selectors**: everything of Selectors 4 a game uses: combinators, attributes (`[type="a" i]`),
  `:is()`, `:where()`, `:has()` (restyles when the inner state changes), `:not()` with lists,
  `:nth-child(2n+1 of .x)`, `:root`, `:hover`, `:active`, `:focus`, `:focus-visible`,
  `:focus-within`, `:checked`, `:disabled`, `:enabled`, `:required`, `:read-only`,
  `:placeholder-shown`, `:target`, `:empty`, `::before`, `::after` (any property, transitions,
  clicks go to their element), `::marker`, `::placeholder`, `::selection`,
  `::-webkit-scrollbar*`, `::-webkit-slider-thumb`, `content` with `attr()` and counters.
  `:visited` never matches; a selector the engine does not know drops its rule with a warning,
  as in a browser.
- **At-rules**: `@media` (width, height, aspect-ratio, orientation, resolution, range syntax,
  `prefers-color-scheme` and `prefers-reduced-motion` switched from Lua; `hover` and `pointer`
  evaluated once at load: `none`/`coarse` on touch devices), `@supports` (evaluated against
  the engine's tables), `@layer`, `@import`, CSS nesting, `@keyframes`, `@starting-style`,
  `@font-face`, `@property` (registered: typed, animated, `inherits: false`; see "Animating custom
  properties (@property)").
- **Painting**: backgrounds with several layers, `url()`, every size, position, repeat, origin,
  clip and `fixed`/`local` attachment; linear, radial and conic gradients and their `repeating-`
  forms (at most **16 color stops**, the rest dropped with a warning); `border` with every style
  (`dashed`, `dotted`, `double`, `groove`, `ridge`, `inset`, `outset` as Chrome draws them;
  widths snap as in Chrome: `0.5px` is 1px); `border-radius` with `%` and elliptical radii
  (children clip to the curve, hit testing follows it); `border-image`; `box-shadow` (inset,
  several, blurred; a zero-offset blur is a glow); `outline` with every style and
  `outline-offset`; `opacity` as a group (the subtree fades as one image); `filter`
  (`blur`, `drop-shadow`, `brightness`, `contrast`, `grayscale`, `hue-rotate`, `invert`,
  `opacity`, `saturate`, `sepia`); `backdrop-filter`; `mask-image`/`mask` layers;
  `clip-path` shapes (`inset()`, `circle()`, `ellipse()`, `polygon()`, `path()`);
  `object-fit`; `image-rendering: pixelated`; `accent-color` and `appearance: none` on form
  controls; `cursor` keywords. Everything is anti-aliased (MSAA 4x by default). Filters apply
  left to right to the element and everything it contains (overflowing children too);
  `drop-shadow` without a colour uses the text colour; `backdrop-filter` filters what is behind
  the border box; mask layers take images and gradients with size, position and repeat (not
  `mask-mode`, `-origin`, `-clip`, `-composite`); a transition between two `url()` backgrounds
  cross-fades them.
- **Transforms**: `transform` (2D and 3D functions, `perspective`), the individual `translate`,
  `rotate`, `scale` properties, `transform-origin`.
- **Transitions and animations**, exactly as in Chromium: `transition` (any property change
  starts one: classes, `:hover`, inline styles, data binding, `var()` changes;
  `transition-behavior: allow-discrete` animates `display: none` exits), `@keyframes` with every
  `animation-*` longhand, every easing (`cubic-bezier()` with overshoot, `steps()`,
  `linear()`), negative delays, fill modes, directions, `animation-play-state`,
  `@starting-style` for elements that appear. They run on the UI clock ("Animation").
- **Text**: `font` shorthand, `font-weight` (a weight the font lacks is synthesized),
  `font-style`, `line-height` (`normal` = the font's spacing), `letter-spacing`,
  `word-spacing`, `text-align` (also `justify`, filled as Chrome fills a line: over the spaces,
  around CJK characters, with `text-justify` `auto`/`inter-word`/`inter-character`/`none`), `text-align-last`,
  `text-indent` (lengths, `%` of the block's width, negative, `hanging`, `each-line`; also in
  text fields; it animates), `text-transform` (Unicode), all six `white-space` values,
  `overflow-wrap`, `word-break`, `text-overflow: ellipsis` (one line: `white-space: nowrap;
  overflow: hidden`), several lines ending with "…" (`-webkit-line-clamp`, "Clamping text to a
  few lines" below), `text-decoration` with color, style, thickness and offset, `text-shadow`
  (several, blurred: a glow), `-webkit-text-stroke` (drawn outside the glyphs), soft hyphens
  (`&shy;`) and zero-width spaces as break opportunities, right-to-left text with
  `direction: rtl` (Arabic shaping built in).
- **Native Canvas Groups compared** (`jasper://docs/ui`): CSS `opacity` composites the element
  and its children as one layer (a native group does that only in its `FLATTEN` mode; the default
  fades each element, as Unity), and a child can undo `pointer-events: none` with `auto`, while
  a native child re-enables blocking only with a group of its own with `ignoreParentGroups`. A
  UI Document under a Canvas Group is not affected by it.
- **Details that match Chrome** and surprise people: `opacity: 0` still receives the mouse
  (use `visibility: hidden` or `pointer-events: none`); `outline` paints above the element's
  content but below its positioned children; `will-change: transform` (or `opacity`, `filter`)
  creates the stacking context at once; an inline `!important` beats everything, `all: unset`
  strips a button's built-in look and `revert` goes back to it; floats and margins collapse as
  in browsers; `display: contents` keeps events and inheritance going through it; a page whose
  root scrolls (`overflow: auto` on `html` or `body`) is as tall as the screen; `fit-content(200px)`
  on a width is Firefox's (Chrome ignores it).
- **Computed at load** (and again when the canvas resizes for viewport units): `calc()` and
  the math functions in borders, shadows, gradients and shapes, where a `%` mixed with `px`
  keeps the `%` part (warning). A `var()` inside such a value whose variable is declared more
  than once (`:root` and `.dark`) cannot be computed and the declaration is dropped
  (`HTMLUI-CSS-0012`); so is `round()` of a length without a step (`round(17.4px, 1px)`).
- **Themes**: rules on classes or attributes of `<html>` (`html.dark`,
  `:root[data-theme="light"]`) follow a change from a script
  (`doc.documentElement.classList:toggle("dark")`), and their custom properties with them.

### CSS that differs

Each item below is reported where you write it (the code in brackets), with the replacement:

- **Container queries** (`@container`, `container-type`, `cqw` units: `HTMLUI-AT-0001`,
  `HTMLUI-CSS-0002`, `HTMLUI-CSS-0014`): use `@media` on the canvas size, or toggle a class
  from Lua on the component that changes size.
- **Blend modes** (`mix-blend-mode`, `background-blend-mode`: `HTMLUI-CSS-0002`): use `opacity`,
  or blend the image beforehand.
- **Vertical text** (`writing-mode`: `HTMLUI-CSS-0002`): rotate the element with
  `transform: rotate(90deg)`, or stack the characters in a column.
- **Multi-column layout** (`columns`, `column-count`: `HTMLUI-CSS-0002`): grid or
  `flex-wrap`.
- **`subgrid`, `masonry`** (`HTMLUI-CSS-0004`): nested grids with explicit track sizes.
- **The standard `line-clamp`**, `max-lines`, `block-ellipsis`, `continue` (`HTMLUI-CSS-0002`;
  info beside a `-webkit-line-clamp`): Chrome 153 does not implement them and drops them, so
  does the engine; write the `-webkit-line-clamp` box ("Clamping text to a few lines").
- **Scroll snapping and smooth scrolling** (`scroll-snap-*`: info `HTMLUI-CSS-0002`;
  `scroll-behavior`: info `HTMLUI-CSS-0003`): scroll from Lua (`el:scrollTo(x, y, true)` is
  smooth, `scrollIntoView`).
- **Font features** (`font-variant-*`, `font-feature-settings`: `HTMLUI-CSS-0002`): use a font
  whose default glyphs are the ones you need (tabular digits: a monospace font).
- **`::first-line`, `::first-letter`** (`HTMLUI-SEL-0001`): wrap the text in a `<span>`.
- **Form validation states** (`:invalid`, `:valid`, `:in-range`, `:user-invalid`, ...:
  `HTMLUI-SEL-0001`): validate in Lua and set a class.
- **`@counter-style`** (info `HTMLUI-AT-0001`): use a built-in list style (`decimal`,
  `lower-roman`, `disc`, ...) or a string.
- **`zoom`** (`HTMLUI-CSS-0002`): `transform: scale()`.
- **`text-align: justify-all`** (`HTMLUI-CSS-0004`): dropped, as Chrome drops it (it does not
  parse the value): write `text-align: justify; text-align-last: justify`.
- **SVG filters** (`filter: url(#f)`: `HTMLUI-CSS-0004`): the CSS filter functions.
- **`border-collapse: collapse`** (`HTMLUI-CSS-0006`): removes the spacing but keeps each
  cell's borders: put borders on one side of each cell.
- **Fonts other than TTF/OTF** (WOFF/WOFF2-only `@font-face`: `HTMLUI-CSS-0022`): convert to
  TTF; remote fonts are imported as TTF.
- **`cursor: url()`** (info): the fallback keyword is used.
- **`direction: rtl`** mirrors the text only: flex rows, grid columns, `start`/`end` and
  sticky/fixed insets stay left to right (info).
- **Typos**: an unknown property (`HTMLUI-CSS-0001`) names the closest real one.

Differences **without a diagnostic** (the engine cannot tell when they matter):

- `z-index` on an element that is neither positioned nor a flex or grid item does nothing (as
  in CSS).
- Font sizes are whole pixels (`13.333px` draws at 13px) and every run of text is a whole
  number of pixels wide: text can be a pixel or two narrower or wider than in Chrome. Leave a
  few pixels of slack in fixed-width buttons (`assert_ui text_fits` measures it).
- `text-wrap: balance`/`pretty` wrap greedily, `hyphens:
  auto` does not hyphenate (put `&shy;` where a word may break), `text-decoration-style: wavy`
  draws solid (all infos). A text decoration is one set of lines per text: an element inside an
  underlined one that declares its own `text-decoration` replaces the underline instead of
  adding to it.
- Tables: a table without a `width` fills its container and columns without a width share it
  equally (Chrome sizes them by content): give tables and columns widths.
- `pre`, `code`, `kbd`, `samp` use the inherited font size (Chrome uses 13px); `monospace`,
  `serif` and the other generic families are the built-in Roboto; a tab in `<pre>` is one
  space.
- Text outside inputs cannot be selected; `user-select: none` only turns selection off in text
  inputs.
- A `<legend>` is as wide as its text; a password field shows `*`.

### Clamping text to a few lines

Item descriptions and card texts cut after n lines with "…" use the box Chrome implements:

```css
.item-desc {
    display: -webkit-box;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    overflow: hidden;
}
```

It is two lines tall and the second line ends with "…" in the box's font and colour, after the
characters that fit beside it, exactly as in Chrome: the lines of nested blocks count, a text
with fewer lines shows none, padding and borders are outside the lines, a right-to-left box ends
on its left, a flex or grid item clamps the same (stretched taller, it shows more lines). Without
`overflow: hidden` the lines after the clamp stay visible, as in Chrome. `-webkit-line-clamp:
none` shows every line (from Lua `el.style.webkitLineClamp = "none"`: a "read more" toggle). On
any other `display` it does nothing (info `HTMLUI-CSS-0003`). In a transition it changes at
once (with `transition-behavior: allow-discrete`, at half the duration); transition the width
or `max-height` instead.

### Text effects

`text-shadow` (hard shadows, glows with a blur radius, several shadows) and
`-webkit-text-stroke` (an outline drawn outside the glyphs) are standard. The engine's own
`font-effect` property (inherited; `.html` and `.rml` documents) draws effects CSS has no
property for, behind the text in the order written: `shadow(x y color)`, `outline(width
color)`, `glow(outline-width blur-width x y color)`, `blur(width color)` (whole `px`; the
colour is multiplied by the element's `opacity`):

```css
.score { font-size: 32px; color: #ffffff; font-effect: outline(2px #000000), shadow(2px 2px #00000080); }
.ready { font-effect: glow(2px 6px 0px 0px #40ff40); }
```

Shadows, outlines, glows, blurs and blurred `text-shadow`s up to 0.3 em are drawn on the GPU
from distance-field glyphs, sharp at any pixel ratio and without layers; larger ones (and the
first frame before those glyphs exist) are drawn the old way, through an offscreen layer or CPU
glyphs.

### Animated text: `text-effect`

The engine's `text-effect` (inherited) animates each character, with the formulas of the native
Text Effect component (`jasper://docs/ui`): a comma list of functions, applied in order, omitted
trailing arguments taking their defaults:

| Function | Arguments (defaults) |
|---|---|
| `wave()` | amplitude `<length>` (0.15em), period `<time>` (1.5s), phase step `<angle>` (0.3rad) |
| `bounce()` | `<length>` (0.3em), `<time>` (1s), spread 0..1 (0.5) |
| `shake()` | `<length>` (0.05em), frequency Hz (12) |
| `pulse()` | scale change (0.15), `<time>` (1s), spread (0.5) |
| `typewriter()` | characters per second (30), fade `<time>` (0s) |
| `fade-in()` | `<time>` (0.5s), spread (0.5): one-shot |
| `grow()`, `unfold()` | `<time>` (0.5s), spread (0.5), amplitude (1: the whole change, TMP's Amplitude): one-shot |
| `rainbow()` | `<time>` (2s), hue step `<angle>` (20deg), saturation `%` (80%), lightness `%` (60%) |
| `gradient()` | `<time>` (1.5s), `multiply` / `add` / `replace` (multiply), colour stops `<color> [<percentage>]` |

```css
h1 { text-effect: wave(0.12em 1.5s), gradient(1.5s add #ffffff, #ffd040); }
.dialog p { text-effect: typewriter(40 0.05s); text-effect-iteration: 1; }
```

The index runs on across elements (`Hello <b>World</b>` waves as one text) and lines; outlines and
shadows (`font-effect`, `text-shadow`) follow each glyph. `text-effect-iteration: infinite |
<integer>` repeats the one-shot functions (typewriter, fade-in, grow, unfold), then `texteffectend`
(bubbles) fires at the element that declares the effect; `text-effect-time: ui` (default, unscaled:
runs at `Engine.timeScale = 0`) or `game`. `doc:setReducedMotion(true)` holds the motion functions
still (wave, bounce, shake, pulse, grow, unfold; an effect of only those still ends). Lua:
`el.style.textEffect = "bounce()"` (set, changed or `none` at runtime; new content types again, a
new line wrap does not), `el:restartTextEffect()` (on any element of the effect: all of it). An animating text regenerates its glyphs each frame (never its layout):
cheap for titles and dialog lines.

### CSS units, fonts and icons

- `px` is a canvas pixel; `vw`/`vh` (and `dvh`, `svh`, `lvh`) are the canvas, which has one
  size (no mobile browser bars: `100vh` is exact); `rem` follows `html { font-size }`; `em`,
  `%`, `ch`, `ex` as in CSS.
- Generic families (`system-ui`, `sans-serif`, `serif`, `monospace`, `cursive`, `ui-*`, ...)
  are the built-in **Roboto** (Latin, Cyrillic, Greek) with **Noto Sans Arabic**. A family
  that is not loaded is skipped (info `HTMLUI-CSS-0023`) and the next one in the list is
  used.
- Your own fonts: `@font-face` with the project's TTF/OTF files ("Images, SVG, Lottie, fonts,
  icons"), or a **Google Fonts `<link>`** kept as written and imported by the editor
  (`import_files {"fonts": ["Inter:400,700"]}` or `{"from_document": ...}`).
- Icons: `<i data-lucide="play"></i>` is a built-in **Lucide** icon, colored by `color`
  (`currentColor`) and sized by `width`/`height`.

## Lua API

Scripts reach a document through `UIDocument`, a GameObject wrapper, and its elements through
`UIElement`s with **browser names and semantics**. Calls use a colon
(`doc:querySelector(".btn")`, `el.classList:add("on")`); properties use a dot
(`el.textContent = "Ready"`). The LuaLS stubs (`.jasperengine/engine-api/lua/UIDocument.lua`,
`UIElement.lua`, `UIEvent.lua`, `HtmlUITypes.lua`, `anime.lua`) type all of it, event names
included (`UIEventType`).

### The element API

```lua
local doc = UIDocument(self.scene, self.entity)   -- in a script on the document's GameObject
local play = doc:getElementById("play")           -- nil when absent
local buttons = doc:querySelectorAll(".btn")      -- an array (1-based), possibly empty
if play then
    play.textContent = "Continue"
    play.classList:add("highlight")
    play.style.width = "240px"                    -- inline style; reading gives the computed value
    play.dataset.level = "3"                      -- data-level="3"
    play:setAttribute("aria-label", "Continue the game")
    play:addEventListener("click", function(event) print("clicked", event.button) end)
end
print(#buttons, doc.body ~= nil)
```

`doc` can also be a script property of type `UIDocument` (another GameObject's document),
or `object('Menu', 'UIDocument')` in `eval_lua`. In `<script type="text/lua">` the document
is the global `document`.

| Group | Members |
|---|---|
| Document | `doc:getElementById(id)`, `doc:querySelector(sel)`, `doc:querySelectorAll(sel)` (CSS selectors, rewritten like the stylesheet), `doc.body`, `doc.documentElement` (`<html>`), `doc.title`, `doc:createElement(tag)`, `doc:createTextNode(text)`, `doc:addEventListener/removeEventListener`, `doc:createDataModel(name, table)`, `doc:getDataModel(name)`, `doc:removeDataModel(name)`, `doc:show()`, `doc:hide()`, `doc.visible`, `doc.sortOrder`, `doc.filename`, `doc:load(file)`, `doc:reload()`, `doc.loaded`, `doc.onLoad`, `doc.onUnload`, `doc:getDiagnostics()`, `doc:getRenderStats()` (the last frame's draw, batching and atlas counters: `drawCalls`, `runsMerged`, `stencilDraws`, `atlasBytes`, ...), `doc:setColorScheme("dark")`, `doc:setReducedMotion(true)`, `doc:getTargetTexture()`, `doc:getUIDocumentComponent()`, `UIDocument.setDebuggerVisible(bool)` |
| Tree | `id`, `tagName` (`"DIV"`; lower case in `.rml`), `localName` (`"div"`), `className`, `parentElement`, `children` (array), `firstElementChild`, `lastElementChild`, `nextElementSibling`, `previousElementSibling`, `childElementCount`, `querySelector`, `querySelectorAll`, `closest`, `matches`, `contains` |
| Mutation | `appendChild(el)`, `insertBefore(el, ref)` (nil appends), `removeChild(el)`, `replaceChild(new, old)`, `remove()`, `cloneNode(deep)` |
| Content | `textContent` (text, never parsed), `innerHTML`/`outerHTML` (HTML, parsed and rewritten like the document), `insertAdjacentHTML(position, html)`, `content` (`<template>`: `tpl.content:cloneNode(true)`, then `appendChild` it) |
| Attributes | `getAttribute(name)` (nil when absent), `setAttribute`, `hasAttribute`, `removeAttribute`, `getAttributes()`, `dataset.userName` = `data-user-name` |
| Classes and style | `classList:add(...)`, `remove(...)`, `toggle(name[, force])`, `contains(name)`; `style.backgroundColor` or `style["background-color"]` (write: inline, `""` or nil removes; read: the computed value as CSS text), `style:getPropertyValue/setProperty/removeProperty`, `style.cssText` |
| Forms | `value`, `checked`, `disabled` (read the new value in `input`/`change` listeners from `event.target.value`) |
| Focus and actions | `focus()`, `blur()`, `click()`; `<dialog>`: `show()`, `showModal()`, `close([value])`; `<lottie>`: `play()`, `pause()`, `stop()`, `seek(s)`, ... |
| Geometry and scroll | `getBoundingClientRect()` (`x y width height top left right bottom`, document px = canvas px for screen documents, through transforms), `offsetLeft/Top/Width/Height`, `clientWidth/Height`, `scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight` (all 0 for an element with `display: none` on it or an ancestor, a `data-if` that is false, or one not in the document, as in a browser), `scrollIntoView([alignToTop])`, `scrollTo(x, y[, smooth])`, `scrollBy(dx, dy[, smooth])` |
| Events | `addEventListener(type, fn[, capture or {capture=, once=}])`, `removeEventListener(type, fn[, capture])`, `dispatchEvent(type[, detail[, bubbles]])` (false when a listener prevented it) |

Queries that find nothing return `nil`. The engine's own attributes (`data-htmlui-*`) and
classes (`htmlui-*`) are invisible to scripts. Show and hide parts of a page with a class or
`style.display = "none"`; the whole document with `doc:hide()` / `doc:show()` (hidden documents
get no input). Create elements with `createElement` or `innerHTML`, then `appendChild`;
`remove()` removes one.

### UI events

| DOM name | Fired by |
|---|---|
| `pointerdown`, `pointermove`, `pointerup`, `pointercancel`, `pointerover`, `pointerout` (bubble), `pointerenter`, `pointerleave` (do not bubble) | every pointer: the mouse (`pointerId` 1, every button) and each finger (`pointerType` `"touch"`, `pointerId` = `fingerId` + 2); "Pointer events" below |
| `gotpointercapture`, `lostpointercapture` | `setPointerCapture` and the end of a capture |
| `click`, `dblclick`, `mousedown`, `mouseup`, `mousemove`, `mouseover`, `mouseout`, `mouseenter`, `mouseleave` | every pointer too (compatibility events, with its `pointerId`): each finger clicks on its own |
| `wheel` | the mouse wheel (`deltaX`, `deltaY` in steps, positive down/right) |
| `keydown`, `keyup` | the keyboard and gamepads, to the focused element ("Input and focus") |
| `textinput` | typed text, to the focused element |
| `focus`, `blur` | focus changes |
| `input` | every edit of a text field, every toggle |
| `change` | a text field on blur when its value changed; checkboxes, radios, selects, ranges when they change |
| `submit`, `reset` | forms |
| `scroll`, `resize`, `load`, `unload`, `show`, `hide` | the element or document |
| `dragstart`, `drag`, `dragend`, `dragenter`, `dragover`, `dragleave`, `drop` | HTML5 drag and drop: `draggable="true"` sources, any element under the drag ("Drag and drop") |
| `dragmove`, `dragout`, `dragdrop` | RmlUi's own drag events, still sent to elements with `drag="drag"` or `drag="drag-drop"` |
| `select`, `activate`, `expand`, `collapse`, `sort`, `columnresize` | virtual views ("Long lists"; `event.detail` / parameters say which item or column) |
| `tooltipshow`, `tooltiphide` | an element whose tooltip shows or hides |
| `animationstart`, `animationiteration`, `animationend`, `animationcancel`, `transitionrun`, `transitionstart`, `transitionend`, `transitioncancel` | CSS animations and transitions (`animationName`, `propertyName`, `elapsedTime` in seconds) |
| `toggle`, `close`, `cancel`, `htmlnavigate` | `<details>`, `<dialog>`, links |
| `play`, `pause`, `loop`, `complete`, `error`, `load` | `<lottie>` (they do not bubble) |
| any other name | `dispatchEvent` |

`touch*` (Touch Events: use the pointer events, `pointerType == "touch"`), `contextmenu`,
`beforeinput` and `composition*` never reach documents: `addEventListener` logs a warning
listing the supported events.

A listener gets a `UIEvent`: `type`, `target`, `currentTarget`, `eventPhase` (1 capture,
2 target, 3 bubble), `button` (0 left, 1 middle, 2 right), `clientX`/`clientY` (document px),
`deltaX`/`deltaY`, `ctrlKey`, `shiftKey`, `altKey`, `metaKey`, `key` (browser key names:
`"Enter"`, `"Escape"`, `"ArrowUp"`, `"a"`), `code` (`"KeyA"`; a gamepad's `"GamepadA"`, ...),
`keyCode` (`Input.KEY_*`), `repeat` (`event["repeat"]`), `gamepad` (the pad's id, -1 for the
keyboard), `pointerId`, `pointerType` (`"mouse"`, `"touch"`, `"pen"`), `isPrimary`, `buttons` (1
left, 2 right, 4 middle), `pressure` (0.5 while pressed), `width`, `height`, `fingerId` (the
`Input` finger id, -1 for the mouse), `data` (`textinput`), `animationName`, `propertyName`,
`elapsedTime`, `detail` (a copy of the `dispatchEvent` detail), `defaultPrevented`, and
`stopPropagation()`, `stopImmediatePropagation()`, `preventDefault()`. The event is valid only
while the listener runs. `preventDefault()` skips the element's own action (a checkbox does not
toggle, a key types nothing, the wheel does not scroll) and marks the input as used by the UI.
Adding the same function for the same type and phase twice is ignored. An error in a listener
is logged with its traceback; the other listeners still run.

### Reloads, `onLoad` and lifetimes

Documents load with their scene, so `init` can query elements, in the editor and in exported
games (layout needs no GPU). A later change of the document reloads it in one of two ways:

- **Patch** (the default for `.html` documents; Preferences > HTML UI > Hot Reload, or
  `doc:reload{mode = "patch"}`): the live document is reconciled with the new file. Elements
  the edit did not touch are the same elements: Lua handles stay valid, every listener (with
  or without an `id`), Lua state, scroll positions, form values, focus, running animations and
  the nodes scripts added stay. An attribute, class, inline style or text takes the file's new
  value only where the **file** changed it, so what a script changed live stays unless you edit
  that same value. An element whose tag or data binding (`data-model`, `data-for`, `data-if`,
  `{{ }}` text, ...) changed is replaced with its subtree, as are removed and added elements.
  `<script type="text/lua">` blocks run again only when their text changed. `doc.onReload`
  runs (with the report), **not** `onLoad`/`onUnload`.
- **Rebuild** (`.rml` documents always; a patch that cannot apply: a changed `<body>` tag or
  binding, a data model created after the load; `doc:reload{mode = "rebuild"}`; the Inspector's
  *Reload (Rebuild)*): the document loads again. For every
  element with an `id` it keeps its listeners (moved to the new element), scroll position,
  form value and focus; the rest comes from the file again, animations of the replaced
  elements are cancelled and listeners of elements without an id are dropped with one warning.
  `onUnload` then `onLoad` fire.

A stylesheet-only change restyles in place and keeps everything. Each reload logs one Console
line, `[HTML UI] Patched ui/menu.html: 3 updated, 1 inserted, 0 removed, 0 replaced, 12
runtime nodes kept (2.1 ms)` (click it for the full report), and `doc.lastReloadReport` holds
it: `mode`, `kept`, `updated`, `inserted`, `removed`, `replaced`, `runtimeKept`,
`scriptsRerun`, `fallbackReason` (why a patch became a rebuild), `ms`, `causes` (each replaced
element and why). `inspect_ui {"reload": true}` reports it too.

- `doc.onLoad` runs after every later load and rebuild, `doc.onUnload` before every unload,
  `doc.onReload = function(report) ... end` after every patch:
  `RegisterEvent(self, doc.onLoad, "bind")` or `doc.onLoad = function() ... end`. Redo in
  `onLoad` what a rebuild does not keep. A method that adds listeners from both `init` and
  `onLoad` must pass the **same functions** each time (adding one twice is ignored), or moved
  listeners run twice.
- Element handles are weak: after a rebuild or removal, `el:isValid()` is false and other uses
  raise `UIElement: element no longer exists`. Inside a listener use `event.currentTarget` or
  query again, not a handle captured before.
- Elements from `createElement`, `removeChild` or `cloneNode` are **detached**: they live while
  a Lua handle of them exists and can be inserted again.
- When play stops, everything scripts made in documents goes (listeners, data models, DOM
  changes, animations; `UIDocument.getLiveLuaRefCount()` is 0) and the documents load again as
  they are on disk.

### Scripts inside a document: `<script type="text/lua">`

A document can carry its own Lua, inline or `src="menu.lua"` (relative to the document). Each
script runs after every load and reload **while the game plays** (never in edit mode), in its
own environment (globals readable, its definitions local; `_G.x = 1` shares a value) with
`document` and `anime` bound. Its listeners go with the old elements on reload, so a handler
runs once however often the file was saved; what it started outside its elements (an
`anime.createTimer`, a `Timer.after` / `Coroutine.start`, globals) is not undone: guard it. The
document script is no script instance: `self:invoke` / `self:startCoroutine` do not exist
there, `Timer.after` and `Coroutine.start` do (unowned: they end at Play stop, lua-api 2.13 and 2.17). Errors point at the HTML line
(`ui/menu.html:45: ... [HTMLUI-HTML-0022]`).

```html
<button id="hello">Hello</button>
<script type="text/lua">
document:getElementById("hello"):addEventListener("click", function()
    anime.animate("#hello", { scale = { from = 1.2 }, duration = 300 })
end)
</script>
```

### A complete UI script

The logic of the menu of "Quick start" (attach it to the menu's GameObject): click handlers,
keyboard and gamepad navigation, an entrance animation, and the bind-on-load pattern that
survives hot reload. The examples follow the same pattern.

`scripts/MenuUI.lua`:

```lua
---@class MenuUI : ScriptInstance
local MenuUI = { properties = {} }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    return assert(doc:getElementById(id), "no #" .. id)
end

function MenuUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.index = 1
    -- created once: bind() adds the same functions after every reload (a second add is ignored)
    ---@param event UIEvent
    self.onClick = function(event)
        local button = event.currentTarget
        if button then self:activate(button.id) end
    end
    ---@param event UIEvent
    self.onKey = function(event) self:key(event) end   -- keyboard and gamepad (the d-pad is ArrowUp/ArrowDown)
    RegisterEvent(self, self.doc.onLoad, "bind")
    self:bind()
end

function MenuUI:bind()
    self.buttons = self.doc:querySelectorAll(".btn")
    for _, button in ipairs(self.buttons) do
        button:addEventListener("click", self.onClick)
    end
    self.doc:addEventListener("keydown", self.onKey)
    self.buttons[self.index]:focus()
    anime.animate(self.buttons, { translateY = { from = 24 }, opacity = { from = 0 },
                                  duration = 500, delay = anime.stagger(70), ease = "outCubic" })
end

---@param step integer
function MenuUI:move(step)
    self.index = (self.index - 1 + step) % #self.buttons + 1
    self.buttons[self.index]:focus()       -- Enter, Space or the gamepad's A then clicks it
end

---@param event UIEvent
function MenuUI:key(event)
    -- the arrow keys, the d-pad and the left stick: wrap around (preventDefault: no spatial step too)
    local step = (event.key == "ArrowDown" and 1) or (event.key == "ArrowUp" and -1) or 0
    if step ~= 0 then
        self:move(step)
        event:preventDefault()
    end
end

---@param id string
function MenuUI:activate(id)
    for i, button in ipairs(self.buttons) do
        if button.id == id then self.index = i end   -- the keys go on from a clicked button
    end
    byId(self.doc, "status").textContent = "Selected: " .. id
    print("menu: " .. id)
end

return MenuUI
```

## Data binding

RmlUi's data binding, driven from a Lua table: the document shows the model's values and
updates when Lua assigns them; form controls write back.

| Markup | Does |
|---|---|
| `data-model="hud"` | the element and its children use the model `hud` |
| `{{ health }}`, `{{ item.name }}`, `{{ health * 2 }}` | text from an expression |
| `data-for="item, i : items"` | repeats the element for each entry (`i` is **0-based**) |
| `data-if="health > 0"`, `data-visible="..."` | present / shown only when true |
| `data-class-low="health < 30"` | the class `low` while true |
| `data-style-width="health + '%'"` | an inline style from an expression |
| `data-attr-src="item.icon"`, `data-attr-disabled="gold < price"` | an attribute; on a boolean attribute (`disabled`, `checked`, `selected`, `hidden`, `readonly`, `required`, `multiple`, `autofocus`, `open`, ...) `false` removes it and `true` sets it, as in HTML (`disabled="false"` would disable) |
| `data-attrif-disabled="gold < price"` | the attribute present while true (any attribute) |
| `data-value="volume"`, `data-checked="music"` | two-way binding of a control |
| `data-event-click="heal(10)"` | calls the model's function `heal(event, 10)` |

`assets/ui/hud.html`:

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>HUD</title>
  <link rel="stylesheet" href="hud.css">
</head>
<body>
  <div class="hud" data-model="hud">
    <div class="bar"><div id="health-fill" class="fill" data-style-width="health + '%'" data-class-low="health < 30"></div></div>
    <span id="health-label" class="label">{{ health }} / 100</span>
    <span id="score" class="score">{{ score }}</span>
    <ul id="log" class="log">
      <li data-for="entry : log">{{ entry }}</li>
    </ul>
    <button id="heal" class="heal" data-event-click="heal(10)">Heal</button>
  </div>
</body>
</html>
```

`assets/ui/hud.css`:

```css
body { margin: 0; font-family: system-ui, sans-serif; color: #ffffff; }
/* click-through: the page has no background, the panel lets the pointer through, the button takes it */
.hud { position: absolute; left: 24px; top: 24px; width: 280px; display: flex; flex-direction: column;
       gap: 8px; pointer-events: none; }
.bar { height: 14px; background-color: rgba(0, 0, 0, 0.5); border-radius: 7px; overflow: hidden; }
.fill { height: 100%; background-image: linear-gradient(90deg, #22c55e, #86efac); transition: width 200ms ease-out; }
.fill.low { background-image: linear-gradient(90deg, #ef4444, #f87171); }
.label { font-size: 14px; text-shadow: 0 1px 2px #000000; }
.score { font-size: 28px; font-weight: 700; text-shadow: 0 1px 2px #000000; }
.log { margin: 0; padding: 0; list-style: none; font-size: 12px; opacity: 0.8; }
.heal { pointer-events: auto; align-self: flex-start; padding: 6px 12px; font: inherit; color: #ffffff;
        background-color: #2563eb; border: 0; border-radius: 6px; }
```

`scripts/HudUI.lua`:

```lua
---@class HudUI : ScriptInstance
local HudUI = { properties = {} }

function HudUI:init()
    local doc = UIDocument(self.scene, self.entity)
    -- create models first in init: a loaded document that uses one reloads to bind it
    self.model = doc:createDataModel("hud", {
        health = 100,
        score = 0,
        log = {},
        heal = function(event, amount) self:heal(amount) end,   -- data-event-click="heal(10)"
    })
end

---@param amount number
function HudUI:damage(amount)
    self.model.health = math.max(0, self.model.health - amount)
    table.insert(self.model.log, "-" .. amount .. " HP")
end

---@param amount number
function HudUI:heal(amount)
    self.model.health = math.min(100, self.model.health + amount)
end

return HudUI
```

- The value `createDataModel` returns is a **proxy**: reading gives the current data (also
  what the document wrote through `data-value`/`data-checked`); **assigning a field updates
  the document** on the next frame. Nested tables are proxies too: `model.items[2].price = 7`,
  `table.insert(model.items, {...})`, `#`, `pairs` and `ipairs` work.
- The top-level keys are fixed at creation (assigning a new one is an error); nested tables
  change freely. Tables with keys 1..n are arrays (an empty table is an empty array). Only nil,
  booleans, numbers, strings and tables are stored; functions become `data-event-*` callbacks
  `fn(event, args...)`.
- `data-for` indices are 0-based, Lua's are 1-based: `model.items[i + 1]`. The rows are the
  elements: `querySelectorAll`, `children`, `childElementCount`, `getElementById`, `inspect_ui`
  and selectors such as `li:last-child` see the rows only, in order (the hidden template RmlUi
  keeps after them is left out).
- Proxy methods: `onChange(name, fn)` (after the document wrote a value; returns an id),
  `removeOnChange(id)`, `dirty(name)` / `dirtyAll()` (after changing a table the proxy did not
  see), `get(path)` / `set(path, value)` (`"items[0].name"`, 0-based), `getName()`, `isValid()`.
  These names cannot be data keys.
- A model belongs to the document that created it: it survives reloads and goes with
  `removeDataModel`, the component or the scene (then any use of its proxy raises
  `data model 'hud' was removed`).
- **Edit mode** has no Lua, so a model no script created yet is empty: `{{ }}` shows nothing,
  `data-for` makes no rows, and a `data-style-*` expression that reads a missing value (such as
  `health + '%'`) sets nothing until the model has it, silently. A value that is invalid with
  every variable present (`data-style-width="'abc'"`) is still reported.
- **From a test** (play mode), set values with `eval_lua`:
  `object('HUD', 'UIDocument'):getDataModel('hud').health = 25`; read them back with
  `inspect_ui {"models": true}`.

## Widgets

Engine elements and behaviours a browser leaves to JavaScript libraries: virtual lists, tooltips,
drag and drop, Markdown.

### Long lists (virtual views)

`data-for` makes one element per item: fine for dozens, slow for thousands (more than 500
reports `HTMLUI-HTML-0031`). A virtual view makes only the rows on screen, plus a few
(`overscan`, default 4), and rebinds them as you scroll: a 10 000-row list scrolls in under a
millisecond per frame.

```html
<div data-model="inv">
  <list-view id="items" items="items" as="item, i" item-height="48" key="id" selection="multiple">
    <template><div class="row" data-event-click="pick(i)">{{item.name}} <span>{{item.count}}</span></div></template>
  </list-view>
</div>
```

| Element | Lays out |
|---|---|
| `<list-view>` | one column of rows |
| `<grid-view item-width="96" item-height="96" gap="8">` | as many columns as fit, recomputed on resize |
| `<tree-view children-key="children" indent="16">` | the expanded nodes of a tree, flattened; the template also sees `depth`, `expanded`, `hasChildren`; an element with `tree-toggle` inside a row expands and collapses it |
| `<table-view>` + `<table-column key="name" width="200" min-width="40" sortable resizable>Name</table-column>` | rows of cells under a sticky header; a column's `<template>` makes its cells (default `{{item.<key>}}`); dragging a header edge resizes (`columnresize`), a click on a sortable header fires `sort` `{key, direction}` and sets `sorted-asc`/`sorted-desc`: you sort the data |

- **Give the view a height** (`height`, or a flex/grid parent that sets one) and an
  `overflow` is not needed: the view scrolls. Without one it lays out 20 rows and reports
  `HTMLUI-HTML-0029`.
- `items="expr"` and `as="item, i"` (as `data-for`; `i` is 0-based) bind the rows to a data
  model; the `<template>` is the row. `item-height="48"` (fixed, fastest) or `"auto"` with
  `estimated-height="40"` (each row measured once; rows above the visible one that change height
  do not move what you are looking at). `key="id"` names items for the selection (default: the
  index).
- `anchor="end"` (chat logs): while scrolled to the end, appended items keep the view at the end.
- `selection="none|single|multiple"`: click, Shift+click, Ctrl+click; `select` fires. Each row
  is a `div.htmlui-row` around a copy of your template; it gets `selected`, `odd`/`even` (not
  `:nth-child`: rows are reused), `aria-selected`, and in a tree `expanded`, `has-children`
  (style `.htmlui-row.selected .name`). Up/Down/PageUp/PageDown/Home/End move the active
  row, Enter and double-click fire `activate`; the gamepad's d-pad does the same while focus is in
  the view, and leaves it at an edge.
- From Lua, `el.itemsView` (1-based indices): `setProvider{count, bind = function(row, i) end,
  unbind, key, height, children = function(key) end (nil: the roots), hasChildren}` for data outside a model
  (a tree asks `children` and `hasChildren` once per key: call `refresh()` after its structure changes), `setCount(n)`,
  `refresh()`, `refreshItems{3, 7}`, `scrollToIndex(i, "start|center|end|nearest")`,
  `getVisibleRange()`, `setSelection{...}` / `getSelection()`, `getSelectedKeys()`, `activeIndex`,
  `activate(i)`, `getRow(i)`, `indexOf(el)`, `keyOf(i)`, `indexOfKey(key)`,
  `expand/collapse/toggle(key)`, `isExpanded(key)`, `getNode(i)`, `getColumns()`,
  `setColumnWidth(key, w)`, `sortBy(key, "asc|desc")`, `getState()`.
- A row element is reused for another item as you scroll: keep state in the data, not in the
  row (a class set from Lua on a row moves to whatever item it shows next); style with
  `.selected` and data bindings. Rows are absolutely positioned inside the view: margins between
  rows do nothing, use `gap` (grid) or padding inside the row.
- MCP: `inspect_ui` reports each view's `widget` (kind, count, first..last, pool, selection),
  `assert_ui`'s `{"type": "virtual_window", "max": 30}` and `{"type": "selected", ...}` check
  it, and `edit_ui_view` drives it in play (`scroll_to`, `select`, `activate`, `expand`, `collapse`,
  `sort`; 0-based indices).

### Tooltips

`title="..."` shows its text after 0.5 s of hover, 16 px below the pointer (above when there is
no room), as in a browser. Moving to another element with a tooltip within 0.5 s shows the next
one at once. Keyboard and gamepad focus show the focused element's tooltip too. A tooltip hides
on leave, press, wheel, a key, a drag start and removal.

- `tooltip="#tip"`: a rich tooltip, a copy of the element (or `<template>`) with that id;
  `tooltip-delay="100ms"` per element.
- Lua: `el:setTooltip("text" | element | function() return "Sell for " .. price end, {placement
  = "top|bottom|left|right|auto", delay = 0.25, arrow = true, interactive = false})` (a function
  runs on every show), `el:clearTooltip()`; `doc.tooltip` has `visible`, `source`, `element`,
  `text`, `show(el)`, `hide()`, `configure{enabled = false, delay = 0.5, hopWindow = 0.5}`.
- Style `div.htmlui-tooltip` (and `.htmlui-tooltip-arrow`; the side is a class,
  `.htmlui-tooltip-top` ...): it is the document's own element, positioned by the engine on top of
  everything. `tooltipshow` / `tooltiphide` fire on the source.
- MCP: `inspect_ui` reports `tooltip`, `assert_ui {"kind": "tooltip", "text": "..."}`.

### Drag and drop

HTML5 drag and drop: `draggable="true"` sources, `event.dataTransfer`, `dropzone` targets.

```html
<div class="grid" data-model="inv">
  <div class="slot" draggable="true" dropzone="item move" data-for="item, i : items"
       data-event-dragstart="pick(i)" data-event-drop="drop(i)">{{item.name}}</div>
</div>
```

```lua
-- functions of the data model ("inv"); i is data-for's 0-based index
local handlers = {
    pick = function(event, i) event.dataTransfer:setData("item", { from = i + 1 }); event.dataTransfer.effectAllowed = "move" end,
    drop = function(event, i) swap(event.dataTransfer:getData("item").from, i + 1) end,
}
```

- Events, in order: `dragstart` and `drag` on the source; `dragenter`, `dragover` (on entry and on
  every move, at most once per frame), `dragleave` on the elements under the pointer; `drop` on
  a target that accepts; `dragend` on the source with the final `dataTransfer.dropEffect`
  (`none` when cancelled or not accepted). `event:preventDefault()` in `dragstart` starts no
  drag. **Changed:** `dragover` used to be RmlUi's (once, on entry: now `dragenter`); old
  handlers on `dragover` now run on every move.
- A target accepts when a `dragover` listener calls `event:preventDefault()`, or its nearest
  `dropzone="item weapon [copy|move|link]"` lists one of the drag's types. Unlike a browser,
  `getData` works in `dragenter`/`dragover`, so a target can accept by content.
- `dataTransfer`: `setData(type, value)` (any Lua value, a table by reference), `getData(type)`,
  `types`, `clearData(type?)`, `effectAllowed` (`none copy move link copyMove copyLink linkMove
  all`), `dropEffect`, `setDragImage(el, x, y)` / `setDragImage(nil)`.
- Feedback: the source gets `htmlui-drag-source`, the body `htmlui-dragging`, the target under
  the pointer `htmlui-drop-allowed` or `htmlui-drop-denied`; the ghost (a copy of the source at
  0.7 opacity) is `.htmlui-drag-ghost` (like every `htmlui-*` class, for CSS: `classList` does
  not show them to scripts). Escape or the gamepad's B cancels. Within 32 px of a
  scroll container's edge it scrolls, up to 600 px/s.
- Touch: inside a scroll container (a virtual view's rows), a swipe scrolls and a press of
  400 ms (moving at most 8 px) starts the drag; elsewhere a touch drags at once.
- In a virtual view a row drag carries `getData("htmlui/row")` = `{view, index, key}`, and over
  a view `event.dropIndex` / `event.dropPosition` (`before`, `after`, `on`) say where; the view
  draws `.htmlui-drop-indicator`. A source row scrolled out of view does not end the drag.
- Gamepad or scripted: `UIDrag.begin(el, {data = {item = ...}, effectAllowed = "move"})`,
  `UIDrag.over(target)`, `UIDrag.drop(target)`, `UIDrag.cancel()`, `UIDrag.isActive()`: the same
  events in the same order.
- MCP: `send_input` drags with `to_target`; `inspect_ui` reports `drag` (source, target, types,
  allowed).

### Markdown

```html
<markdown>
  ## Patch notes
  - **Faster** loading, see <https://example.com/notes>
</markdown>
<markdown src="notes/journal.md"></markdown>
<div data-model="chat"><div data-markdown="message.text"></div></div>
```

- The inline source is the text between the tags, as written (dedented); `src` follows the
  `<img src>` path rules and reloads the document when the file changes. From Lua:
  `el:setMarkdown(text)` (any element) and `UIDocument.markdownToHtml(text)`.
- Supported: headings, paragraphs, emphasis, `code`, fenced code (`pre > code[data-lang]`; an
  unclosed fence renders as code, so streamed text works), lists to any depth, quotes, rules,
  tables, links and autolinks, images. The output is a `div.markdown-body` you can style.
- HTML inside Markdown is shown as text, never run: text from players or servers is safe (an
  inline source with markup notes `HTMLUI-HTML-0032`).

## Animation

Two ways, both on the **UI clock** (the engine's frame time before `Engine.timeScale`: a pause
menu animates while the game is frozen; deterministic with `play {fixed_fps}`; still while
`auto_pause` holds the simulation):

- **CSS** transitions and `@keyframes` for state changes and loops declared in the stylesheet
  (hover, `.open`, a pulsing badge). They start on any change, including a class set from Lua.
- **`anime`**, a Lua port of [Anime.js v4](https://animejs.com/documentation/) (4.5.0), for
  sequences, staggers, springs, values driven from game logic and animating numbers in Lua
  tables. C++: `Anime` (`#include "htmlui/anim/Anime.h"`).

```lua
anime.animate("#title", { translateY = { from = -40 }, opacity = { from = 0 }, duration = 600,
                          ease = "outElastic(1, .6)" })
anime.animate(".btn", { scale = 1.05, delay = anime.stagger(80, { from = "center" }),
                        loop = 2, alternate = true })

local tl = anime.createTimeline({ defaults = { duration = 300, ease = "outQuad" } })
tl:add(".menu", { opacity = { from = 0 } })
  :label("buttons")
  :add(".btn", { translateX = { from = -30 }, delay = anime.stagger(60) }, "buttons-=100")
  :call(function() print("menu shown") end, "+=50")

local stats = { hp = 100 }                     -- a plain table: its number and colour fields
anime.animate(stats, { hp = 0, duration = 2000, modifier = anime.utils.round(0),
                       onComplete = function() print("hp", stats.hp) end })
```

- **Targets**: a CSS selector (the documents of every scene of the game, the current scene's
  first), a `UIElement`, an array of them, a plain table, or `root = doc` to scope selectors.
- **Properties**: individual transforms (`translateX/Y/Z` or `x/y/z`, `rotate`, `rotateX/Y/Z`,
  `scale`, `scaleX/Y/Z`, `skew`, `skewX/Y`, `perspective`, composed in Anime's order), any CSS
  property in camelCase or kebab-case, custom properties (`["--glow"] = "8px"`),
  `scrollTop`/`scrollLeft`/`textContent`/`innerHTML`, attributes, table fields.
- **Values**: numbers, `"10px"`, `"45deg"`, `"+=10"`/`"-=10"`/`"*=2"`, colours, `{from, to}`,
  `[from, to]`, keyframe arrays or `{ ["0%"] = {...}, ["50%"] = {...} }`,
  `function(target, index, total)` (**indices are 0-based**, as in Anime).
- **Parameters**: `duration` (ms, default 1000), `delay`, `loop` (count or `true`), `loopDelay`,
  `alternate`, `reversed`, `autoplay` (default true), `playbackRate`, `frameRate`, `ease`
  (default `"out(2)"`), `composition` (`"replace"`, `"none"`, `"blend"`), `modifier`, `id`,
  `root`, `clock = "game"` (scaled game time: freezes with the game).
- **Callbacks** (they get the animation): `onBegin`, `onBeforeUpdate`, `onUpdate`, `onRender`,
  `onLoop`, `onPause`, `onComplete`; `anim:andThen(fn)` is Anime's `then()`.
- **Control**: `play pause resume restart reverse alternate seek(ms) stretch(ms) cancel revert
  complete reset refresh`; state `currentTime`, `progress`, `paused`, `began`, `completed`,
  `cancelled`, `speed`. Timelines add `add set call label sync remove`; positions: ms,
  `"+=100"`, `"-=100"`, `"<"` (end of the previous child), `"<<"` (its start), `"label+=50"`.
- **Easings**: `linear`, `in`, `out`, `inOut`, `outIn` (power, `in(3)`), and `in/out/inOut/outIn`
  + `Quad Cubic Quart Quint Sine Expo Circ Bounce Back Elastic` (`outBack(1.7)`,
  `outElastic(1, .3)`), `cubicBezier(...)`, `steps(...)`, `linear(...)`, `irregular(...)`,
  `spring(m, k, c, v)` as strings, `anime.eases.<name>`, Lua functions, and springs
  (`ease = anime.createSpring({ bounce = .5, duration = 400 })`). An unknown name warns and
  uses `linear`.
- **utils**: `set get remove random createSeededRandom randomPick shuffle clamp round snap wrap
  mapRange lerp damp degToRad radToDeg roundPad padStart padEnd stagger`; with fewer arguments a
  util returns a function (`modifier = anime.utils.round(0)`).
- **Do not combine** `anime` and a CSS `transition` on the same property: `anime` writes the
  inline style every frame, which restarts the transition each time. The menu above
  transitions `translate` and animates `transform` for that reason.
- **Lifetime**: stopping play cancels every animation; a full reload of a document cancels the
  animations of its replaced elements: start them again in `onLoad`. Elements that disappear
  are skipped.
- **Lifetime of handles**: an animation handle kept in a global stays valid after play stops,
  but cancelled.
- Differences from Anime.js: `andThen` for `then` (no Promises), string forms of the ease
  functions, modifiers return numbers, plain tables get colours back as `#rrggbbaa`, no SVG,
  draggable, scroll, layout, text, WAAPI, scope or animatable modules.

### Animating custom properties (@property)

A custom property is untyped text: `transition: --x 1s` and `@keyframes { to { --x: 1 } }` cannot
interpolate it, so it jumps from one value to the next. Register it with `@property` and it gets a
type, an initial value and an inheritance rule, exactly as in Chromium: transitions and keyframes
interpolate it, and every `var(--x)` that reads it follows, frame by frame. This is how one value
drives a gradient angle, an opacity, a size and an effect parameter at once.

```css
@property --ring { syntax: '<percentage>'; inherits: false; initial-value: 0%; }
@property --spin { syntax: '<angle>'; inherits: false; initial-value: 0deg; }
@property --glow { syntax: '<number>'; inherits: false; initial-value: 0; }

.ring {
  width: 96px; height: 96px; border-radius: 50%;
  background: conic-gradient(#40c060 var(--ring), #20242c var(--ring));
  transition: --ring 600ms ease-out;
}
.ring.full { --ring: 100%; }

.loader {
  width: 48px; height: 48px; border-radius: 50%;
  background: conic-gradient(from var(--spin), #4060e0, #e04080, #4060e0);
  animation: spin 1.2s linear infinite;
}
@keyframes spin { to { --spin: 360deg; } }

.badge { opacity: var(--glow); transition: --glow 300ms linear; }
.badge.lit { --glow: 1; }
```

- **Descriptors.** `syntax` (a quoted string), `inherits` (`true`/`false`) and `initial-value` are
  all required, as in CSS; `initial-value` may be left out only for `syntax: '*'`. The initial
  value must not use `var()` or font-relative units (`em`, `rem`, `%` of a length): it has no
  element to resolve them against. An invalid rule is dropped with `HTMLUI-AT-0014`.
- **Syntaxes registered:** `<length>`, `<number>`, `<integer>`, `<percentage>`,
  `<length-percentage>`, `<color>`, `<angle>`, `<time>` and `*`. Combinations and lists
  (`'<length> | auto'`, `'<length>+'`) and other types (`<image>`, `<transform-function>`, ...) warn
  with `HTMLUI-AT-0012` and fall back to an unregistered property whose initial value is set on the
  root (it inherits and changes discretely).
- **`inherits: false`** is honoured: a child that does not set the property reads its initial
  value, not its parent's. With `inherits: true` children read the parent's *computed* value: a
  `<length>` of `2em` is the parent's pixels.
- **Invalid values.** A declaration that does not match the syntax (`--ring: red`), or a `var()`
  that substitutes such a value, is invalid at computed-value time: the property takes its initial
  value (its parent's when it inherits). `check --ui` warns about the ones it can see
  (`HTMLUI-AT-0014`).
- **What `var()` gets** is the computed value: numbers as numbers, lengths in px, angles in
  `deg`, times in `s`, integers rounded, colors as `#rrggbbaa`. `<integer>` interpolates and
  rounds, `*` changes discretely (with `transition-behavior: allow-discrete` at half the time).
- **Name the property** in `transition`: `transition: all` does not include custom properties
  (Chromium). `@starting-style` and `transition-behavior` work as for any property.
- **Anywhere in the document.** A rule after the declarations that use the name applies to them;
  a rule inside `@media` or `@supports` registers when the condition holds when the document loads
  (or is laid out again for a new viewport); inside a style rule it is invalid. When two rules
  name the same property, the last valid one wins.
- **Edit and reload freely.** A document's registrations are its own: a hot reload registers the
  edited definitions (syntax, inherits, initial-value) in edit mode and during play, and unloading
  the document releases them. Only when two documents loaded at the same time register one name
  with different definitions does one definition apply to both: the one of the document loaded
  first. The other gets `HTMLUI-AT-0013` (naming the first) until the first unloads or registers
  the same definition; then its own definition applies. Keep one definition per name across the
  documents of a game (one shared stylesheet does it). A few thousand names can be registered at a
  time; past that a registration warns (`HTMLUI-AT-0012`) and is ignored.
- **From Lua**, `anime` animates a registered property as typed values, and every `var()` follows:

```lua
---@param doc UIDocument
local function lightBadge(doc)
    local badge = doc:querySelector(".badge")
    if badge then
        anime.animate(badge, { ["--glow"] = 1, duration = 400, ease = "outQuad" })
    end
end
```

| Code | Severity | When |
|---|---|---|
| `HTMLUI-AT-0012` `at-property-unsupported-syntax` | warning | a syntax the engine does not register: the root-rule fallback |
| `HTMLUI-AT-0013` `at-property-conflict` | warning | the name is already registered with another definition: the first stays |
| `HTMLUI-AT-0014` `at-property-invalid` | error / warning | an invalid rule (dropped); a declaration that does not match the syntax |

## Images, SVG, Lottie, fonts, icons

### Images and SVG

PNG, JPG and the other formats the engine loads, as `<img>` or CSS backgrounds. SVG is drawn by
LunaSVG and Lottie by ThorVG, both in the engine, rasterized into textures, so `opacity`,
transforms, clipping and filters apply to them as to `<img>`.

| Write | Result |
|---|---|
| `<img src="icons/play.svg" width="24" height="24">` | a vector image rasterized at its size (crisp at any size); `currentColor` in it follows the CSS `color` |
| `<svg width="48" height="48" viewBox="0 0 24 24"> <path .../> </svg>` | inline SVG |
| `background: url(icons/bg.svg) center / contain no-repeat;` | an SVG background, rasterized at its drawn size |
| `image-color: #ff8000;` | (engine property) multiplies every pixel of an image, SVG or Lottie by the colour |

- Paths, shapes, strokes, gradients, patterns, `clipPath`, masks, markers, `<use>`/`<symbol>`,
  CSS inside the SVG, `<text>` (in Roboto) work. `currentColor` follows the element's `color`
  (icons recolor on `:hover`, transitions included): an SVG whose paints are all `currentColor`
  or `none` is rasterized once in white and tinted; one mixing `currentColor` with fixed colors
  is rasterized again when the colour changes (`HTMLUI-SVG-0001`, info). CSS `fill`, `stroke`, `stroke-width`, ...
  on the `<svg>` element apply to its root; rules aimed inside it (`svg path {}`) do not
  (`HTMLUI-SVG-0003`).
- Not supported: SVG filters, SMIL animation, scripts, `<foreignObject>` (`HTMLUI-SVG-0002`),
  external `<image href>` files (`data:` URIs only).
- RmlUi's own `<svg src="icons/play.svg">` element also works (`crop-to-content` trims it to
  the drawing). `.svg` textures outside HTML UI (`Image`, `Sprite`, materials;
  `path.svg?svgScale=2`) use the same rasterizer.

### Lottie

```html
<lottie id="spinner" src="anim/loading.json" autoplay="autoplay" loop="loop" style="width: 96px; height: 96px;"></lottie>
```

| Attribute | Values |
|---|---|
| `src` | a `.json` (Bodymovin) or `.lottie` (dotLottie v1/v2) file, relative to the document |
| `autoplay` | present = play on load (`autoplay="false"` = off); default paused on the first frame |
| `loop` | present / `true` = forever; a number = that many plays in total; default once |
| `speed`, `direction` (`1`/`-1`), `mode` (`normal`/`bounce`) | playback |
| `segment="30 60"`, `marker="idle"`, `animation="id"` | a frame range, a named marker, the animation of a `.lottie` |
| `playonhover` | plays while the pointer is over it |

- `<lottie-player>`, `<dotlottie-player>` and `<dotlottie-wc>` are the same element, without
  their `<script>`. Attributes override a `.lottie` manifest's settings and apply at once when
  changed at run time (`src`/`animation` reload).
- `loop="3"` plays three times in total (two `loop` events, then `complete`; `0` = forever).
  `segment="30 60"` shows frames 30..59. An N-frame animation at F fps shows frames 0..N-1,
  each for 1/F s, as lottie-web does.
- Images of the animation: embedded (`data:`), files next to the `.json`, or the `.lottie`'s
  image folder. Text layers use the fonts embedded in the file, else Roboto.
- Without `width`/`height` the element takes the animation's size; otherwise the animation fits
  inside, centred. It plays on the UI clock.
- Lua (and C++ `UIElement`): `play()`, `pause()`, `stop()`, `seek(seconds)`, `setFrame(n)`,
  `setSpeed(x)`, `setDirection(-1)`, `setLoop(b)`, `setLoopCount(n)`, `setSegment(a, b)`,
  `setMarker(name)`, `isPlaying()`, `getCurrentFrame()`, `getTotalFrames()`, `getDuration()`.
  Events `load`, `error` (`event.detail.message`), `play`, `pause`, `loop`, `complete`. In the
  editor the document is loaded before play starts: check `getTotalFrames() > 0` instead of
  waiting for `load`.
- Not supported: expressions (JavaScript), audio layers, dotLottie themes and state machines,
  ZIP64 or encrypted `.lottie` archives (an `error` event says so).

### Video

```html
<video id="intro" src="video/intro.webm" autoplay muted loop style="width: 100%; height: 100%; object-fit: cover;"></video>
<video src="video/trailer.webm" poster="video/trailer.png" controls></video>
```

**Formats**: WebM with AV1 video and Opus audio plays everywhere, decoded by the engine itself
(dav1d, libopus, libwebm: BSD licences, no patents to license, identical frames on desktop, mobile
and Web); `.mkv` with the same codecs too. **MP4/MOV** (H.264 + AAC, HEVC where the platform has
it) and VP8/VP9 WebM play through each platform's decoders (Android MediaCodec, AVFoundation,
Media Foundation, the browser, Linux with the system's FFmpeg): jasper://docs/video has the
per-platform matrix. `src` may be an `https://` URL (downloaded into the media cache, played while
it downloads when the MP4 is fast start; the browser streams it on the Web).
`canPlayType('video/mp4; codecs="avc1.42E01E, mp4a.40.2"')` answers for the platform it runs on;
`element.videoBackend` (and `inspect_ui`'s media `backend`) says what decodes it (`webm`,
`ffmpeg`, `mediacodec`, `avfoundation`, `mediafoundation`, `browser`). Ogg/Theora, AVI and the rest
are reported (`HTMLUI-HTML-0027`, the fix is the conversion) and show their poster or nothing.

**Converting** (for WebM AV1 everywhere; MP4s are imported as they are): the Project window's
*Convert to WebM (AV1)...* on a video, or `import_files` on a non-MP4 file (e.g. `"/abs/clip.avi"`), writes
`clip.webm` (AV1 with SVT-AV1, libaom or rav1e, keyframes every 2 s, 8-bit 4:2:0, at most 4096
px; Opus 128 kb/s stereo) and does not copy the original; files dropped on the Project window
and video URLs (in `import_files`, or a document's remote `<video src>` when the editor imports
its references) are converted the same way. It runs your own FFmpeg as a separate program:
`JASPER_FFMPEG`, else Preferences > External Tools > FFmpeg (`preferences.ffmpeg_path`: the user sets it;
read-only over MCP), else `ffmpeg` on `PATH` (never shipped or linked). Without FFmpeg, or
without an AV1 encoder in it, the file is copied unconverted with `HTMLUI-IMP-0010` and the
exact command to run; a failed conversion gives `HTMLUI-IMP-0011`. By hand:
`ffmpeg -i in.mp4 -c:v libsvtav1 -preset 6 -crf 32 -g 60 -pix_fmt yuv420p -c:a libopus -b:a 128k out.webm`.

| Attribute | Behaviour |
|---|---|
| `src`, or `<source src type media>` children | the file, relative to the document; the first `<source>` whose `type` passes `canPlayType` and that opens is played |
| `autoplay`, `loop`, `muted` | play once loaded (sound allowed: there is no browser autoplay policy); seamless loop; start muted |
| `controls` | the built-in bar (below); the video becomes focusable |
| `poster` | an image until playback starts or a seek, fitted like the video |
| `preload` | `none`: opened at `play()`, `load()` or a seek; `metadata` / `auto` (default): opened at once, first frame decoded |
| `width`, `height` | size hints, as for `<img>`; without a size the element takes the video's (300 x 150 before it is known) |

- Size and drawing: `object-fit` (default `contain`, as in browsers) and `object-position` place
  the frame in the content box; `border-radius` clips it; `opacity`, `filter`, `mask-image`,
  transforms, effects (`filter: effect(...)`, stateful ones such as `time-trail` and `data-mosh`
  too: they feed on the playing frames) and `backdrop-filter` over a video all work. A video
  cannot be a CSS image (`url(x.webm)` in `background` is `HTMLUI-CSS-0004`): put a `<video>`
  with `object-fit: cover` behind the content (the `video_menu` example).
- Timing: a video plays on the UI clock. It keeps playing under `Engine.timeScale = 0` (a pause
  menu over a video background); to freeze it with the game, `video:pause()` or set
  `video.playbackRate = Engine.timeScale`. It stops while the app is paused or the MCP holds the
  game. With a fixed step (MCP `fixed_fps`) or a pinned clock it is **frame-exact**: frame
  `floor(t * fps)` at time `t`, decoding waits for it; otherwise decoding never blocks a frame:
  when it falls behind, the picture waits (`waiting`, then `playing`), as a browser buffering.
- Audio: the Opus track plays through the engine's audio (global volume applies), synced to the
  picture; more than two channels are mixed down to stereo.
- Lua (and C++ `UIElement`), as `HTMLMediaElement`: `play()`, `pause()`, `load()`,
  `canPlayType(type)`, `fastSeek(t)`; properties `currentTime` (set = seek), `duration`
  (NaN before the metadata), `paused`, `ended`, `seeking`, `loop`, `autoplay`, `controls`,
  `muted`, `defaultMuted`, `volume`, `playbackRate`, `defaultPlaybackRate`, `readyState`,
  `networkState`, `videoWidth`, `videoHeight`, `currentSrc`, `videoBackend` (what decodes it: `webm`, `ffmpeg`, `mediacodec`, `avfoundation`, `mediafoundation`, `browser`), `poster`, `preload`, `error` (`nil`
  or `{code, message}`); constants `UIElement.HAVE_ENOUGH_DATA`, `UIElement.MEDIA_ERR_SRC_NOT_SUPPORTED`, ...
  Out of range values (`volume = 2`) warn and leave the value (HTML throws).
- Events (they do not bubble): `loadstart`, `durationchange`, `loadedmetadata`, `loadeddata`,
  `canplay`, `canplaythrough`, `play`, `playing`, `pause`, `timeupdate` (every 250 ms),
  `seeking`, `seeked`, `ended`, `waiting`, `volumechange`, `ratechange`, `emptied`, `error`
  (`event.detail.message`). In the editor the document loads before play starts: check
  `el.readyState >= UIElement.HAVE_METADATA` instead of waiting for `loadedmetadata`.
- Controls: play/pause, time, a seek bar (click or drag) and mute, shown while paused, hovered or
  focused, fading 2.5 s after the pointer stops while playing. Keys on the focused video: Space,
  Enter or `K` toggle, Left/Right seek 5 s (`J`/`L` 10 s), Home/End, `M` mutes; the gamepad's A
  and d-pad do the same. Restyle with `video videocontrols { ... }` (`videobutton.htmlui-play`,
  `videotime`, `videotimeline`, `videoprogress`, `videothumb`, `videobutton.htmlui-mute`).
- Budget (software decoding with dav1d's SIMD asm on x86-64 and ARM, ms per frame on one core of
  a server EPYC: 0.8 at 256 px, 1.8 at 360p, 5.9 at 720p, 12.6 at 1080p; without it, as on the
  Web, 1.7, 4.3, 13.5 and 28.6; several cores share a video's work): desktop one 1080p30 or two
  720p30 with headroom; mobile 720p30, 1080p30 on recent phones; Web (C, decoding on the main
  thread) 480p or smaller, one at a time. Memory: about 1.5 x width x height x 7 bytes per video (decoder pictures and the decode
  queue), plus the file on Web, where assets live in memory. Keep previews small and pause
  videos nobody sees.
- Not supported: other codecs, streaming or remote URLs at run time (import them), `<track>`
  captions (`HTMLUI-HTML-0002`), `playsinline`/`crossorigin` and the like (dropped, info
  `HTMLUI-HTML-0028`), picture-in-picture, `preservesPitch` (pitch follows `playbackRate`),
  `::-webkit-media-controls-*`, HDR (8-bit SDR is shown; 10-bit is decoded and rounded).

### Particles: `<particle-system>`

`<particle-system src="fx/confetti.prefab" autoplay loop time="ui" scale="1">` plays a prefab's
Particle System (the first of its GameObjects with one; its billboards as they look: the Renderer's
texture or soft disc, sprite sheet frames, additive or alpha blending, sizes, rotations, colours)
and draws its particles in document order: above what comes before it,
below what comes after, inside the element's clip (`overflow: hidden`) and transform. The emitter
is at the centre of its content box (0 x 0 by default: give it a size or position it), one prefab
unit is `scale` CSS px, y up. `time="ui"` (default: the system's `useUnscaledTime`) plays while
the game is paused, `game` follows `Engine.timeScale` (the effect's own Unscaled Time off too);
`loop="false"` plays once, `loop` loops (the effect's Looping on the GameObject's own copy; the
prefab's own setting otherwise); without `autoplay` it waits for `el:play()`.

```html
<div class="panel" style="overflow: hidden">
  <particle-system id="confetti" src="fx/confetti.prefab" autoplay scale="40" style="position: absolute; left: 50%; top: 30%"></particle-system>
  <button id="again">Play again</button>
</div>
```

Lua: `el:play()`, `el:pause()`, `el:stop()`, `el:clear()`, `el:isPlaying()`,
`el:getParticleSystem()` (the system of its hidden GameObject: every `ParticleSystem` API works;
`nil` until the element's first update made the prefab, e.g. in `onLoad`, in edit mode, or when its
`src` failed); `el:clear()` removes the particles (the system plays on), `el:stop()` stops
emitting (what is alive dies out; `el:isPlaying()` until then, as Unity's), `el:isPlaying()` is
false while paused; event `complete` when a system that does not loop has stopped (its last
particle died). `play()` / `pause()` /
`stop()` before the prefab exists are queued (the last one applies, over `autoplay`). The prefab is
made on a hidden GameObject of the document's scene (not in the Hierarchy, not saved; none of its
renderers draws) and goes, with all its children, with the element, its document or a new `src`.
Screen and texture-mode (world) documents both work. Prefabs exist in Play and in exported games: in
edit mode the element is empty. A `src` that names no prefab with a Particle System logs
`HTMLUI-HTML-0029` and adds it to the document's diagnostics (`inspect_ui`). Keep a system under
about 1000 particles (one mesh is rebuilt each frame).

### Fonts

- Built in: Roboto and Noto Sans Arabic; every generic family maps to them.
- Project fonts: `@font-face` with TTF/OTF files, relative to the stylesheet; `font-weight` and
  `font-style` pick the face. A weight or style without a face is synthesized (bold strokes
  `max(1, size / 24)` px wider, italic slanted 12 degrees); load the real face when the design
  depends on it.

```css
@font-face { font-family: "Title"; src: url("fonts/Title.ttf"); font-weight: 400; }
.title { font-family: "Title", system-ui, sans-serif; }
```

- C++ can load a face before documents use it: `HtmlUI::loadFontFace("fonts/Roboto-Bold.ttf")`
  (`#include "htmlui/HtmlUI.h"`).
- A character no loaded font covers draws a missing-glyph box; control characters draw
  nothing. Text is rasterized at each context's pixel ratio (the screen's, or `pixelRatio` for
  documents on meshes), so it stays sharp.

### Icons

`<i data-lucide="house"></i>` (also `<span data-lucide>`) is replaced by the built-in
[Lucide](https://lucide.dev) icon (all 1869 icons and their aliases, offline) exactly as
`lucide.createIcons()` does: classes `lucide lucide-house`, your attributes merged, colored by
`color` and sized by CSS (`.lucide { width: 20px; height: 20px; stroke-width: 1.5 }`). Names in
kebab-case, PascalCase or camelCase; an unknown name warns with the closest ones
(`HTMLUI-HTML-0021`); setting `data-lucide` from Lua swaps the icon. To get an icon as a
file instead: `import_files {"icons": ["lucide:play"]}`.

### Importing remote fonts, images and animations

A document may link what a page links: Google Fonts, CDN stylesheets (Font Awesome, animate.css),
remote images, LottieFiles animations. **The `https://` URLs can stay as written**: the editor
downloads each once into `assets/htmlui-imports/` and records it in
`assets/htmlui-imports/manifest.json`, through which the runtime serves the URL from the project
(offline, also in exports).

- **Automatic**: when the editor loads a document, missing URLs are downloaded in the
  background (`[HTML UI] importing <url>`), then the document reloads (Editor Settings >
  General > HTML UI > Import Remote Resources turns it off).
- **MCP** `import_files`: `{"from_document": "assets/ui/menu.html"}` imports every remote URL
  of a `.html`/`.css` (and with `rewrite`, the default, replaces them with paths relative to the
  file, one project undo step; a Google Fonts link becomes the generated `@font-face` stylesheet);
  `{"fonts": ["Inter:400,700"]}` imports Google Fonts as TTF plus an `@font-face` stylesheet;
  `{"icons": ["lucide:play"]}` writes `.svg` files; `{"sources": ["https://..."]}` imports
  single files. The files go to `to_dir` (default `assets/ui/imported`); a URL that fails is
  listed in the result's failed entries without failing the others.
- **CLI**: `jasperengine-editor import-ui --project <dir> assets/ui/menu.html [more documents]
  [--dest <dir>] [--force] [-q]` imports synchronously, prints the diagnostics compiler-style
  and exits 1 when an import failed. A URL that failed is not retried automatically in the
  same editor session.
- **Inspector** of the UI Document: *Remote resources* with Import, Re-import all and Remove
  unused.
- What each kind becomes: Google Fonts one static TTF per weight/style plus a local `@font-face`
  CSS and the family's license; other stylesheets a local copy with their `url()`s imported;
  images a file of the detected type (WebP/AVIF are saved but not drawable,
  `HTMLUI-IMP-0003`); Lottie a `.json` with its images.
- A URL that is not imported is `HTMLUI-IMP-0001` (error; fonts fall back, images are not
  drawn), a failed download `HTMLUI-IMP-0002`, an entry whose file is gone `HTMLUI-IMP-0009`.
  Licenses are recorded (`HTMLUI-IMP-0008`): Google Fonts ship their license file, other files
  say "unknown": check them before shipping. Downloads use `curl` (missing:
  `HTMLUI-IMP-0005`).

## Input and focus

### Pointer input and pass-through

Documents get the mouse (buttons, move, wheel), multi-touch, the keyboard, gamepads, text with
IME and the clipboard, in play mode, in exported games and through `send_input`. A pointer event
goes to the screen documents first (top scene layer first, then higher `sortOrder`), then to the
component UI, then to documents on meshes, then to the game. Keys, gamepad buttons and text go to
the focused document before the game.

**An event a document handles does not reach the game**: not the component UI under it, nor,
while `Engine.ignoreEventsHandledByUI` is on (the default), the game's `onMouseDown`,
`onKeyDown`, `onCharInput`, `onMouseScroll`, `onTouchStart`, `onGamepadButtonDown`,
`onGamepadAxisMove`, ... callbacks.

**Pointer pass-through: a transparent page lets the pointer through unless you listen on the
page or the document.**

- The pointer is taken where it is over an element of the document, or over a page (`html` /
  `body`) that paints a background (any colour with alpha, gradient or image).
- Where a page has neither, the pointer goes on to the next document below (lower
  `sortOrder` or scene layer), then to the game. A HUD under a transparent menu document still
  gets clicks where the menu has nothing, and the game gets the rest.
- To make a transparent page take every pointer event (a modal, "click outside closes the
  menu"), give it a background (`rgba(0, 0, 0, 0.01)` is enough) or add a pointer listener
  (`click`, `mousedown`, `mouseup`, `mousemove`, `mouseover`, `mouseout`, `wheel`, with
  `addEventListener` or `data-event-*`) on the document or `doc.body`. It is decided per
  document and follows the listeners: removing the last one gives the empty parts back.
  `body` is only as tall as its content, so listen on the document to catch every empty part.

```lua
local doc = UIDocument(self.scene, self.entity)
local menu = doc:getElementById("dropdown")
doc:addEventListener("click", function(event)              -- the page now takes every click
    local target = event.target
    if menu and target and not menu:contains(target) then menu.classList:remove("open") end
end)
```

- `pointer-events: none` removes an element from hit testing: a full-screen HUD wrapper with
  it is click-through, and its children can set `pointer-events: auto`.
- A press that no document takes blurs the focused field. A release, and the moves of a drag,
  go to whoever got the press. The wheel is taken when it scrolls an element (one step is 80
  px) or the pointer is over content.

### Focus, keys and gamepads

**Focus and keys.**

- Tab moves the focus through buttons, links, inputs and `tabindex="0"` elements
  (`tabindex="-1"` takes them out); `el:focus()` from Lua; `autofocus` on load. `:focus` and
  `:focus-visible` match the focused element only, `:focus-within` its ancestors too.
- Keys go to the focused element (bubbling to the document) of the document that took the
  last press. They are consumed when a listener stops or prevents them, or when a text field
  has focus (except Escape and F1-F25); otherwise the game gets them. A text field being typed
  in, in any scene, gets the keys before the other documents (an upper document's focused
  button does not take Space or Enter from it). Enter and Space activate
  a focused button or summary, Enter a link, Space toggles a checkbox.
- **Keyboard menus**: focus the first button on load. The arrow keys move the focus only where
  `nav-up` / `nav-down` / `nav-left` / `nav-right` (or the shorthand `nav`) say so: `auto`
  (the nearest element that way), an `#id`, `none`, `horizontal`, `vertical`, `tree-order`; or
  move it in a `keydown` listener, as the script under "A complete UI script" does (that
  listener serves the gamepad too).

**Gamepads reach the focused document first.** While an element Tab reaches (a button, link,
form control or `tabindex="0"`) has the focus, the gamepad drives the document like a keyboard:

| Gamepad | `keydown` / `keyup` `key` | `code` | Default action |
|---|---|---|---|
| d-pad, left stick | `"ArrowUp"` ... `"ArrowRight"` | `"GamepadDPadUp"` ..., `"GamepadLeftThumbstickUp"` ... | moves the focus to the nearest element that way and scrolls it into view; a focused range adjusts with left/right, an open select moves through its options |
| A | `"Enter"` | `"GamepadA"` | activates the focused element as the keyboard does (Enter: a click on press; Space on a checkbox or radio: a toggle on release; a select opens and chooses); `:active` while held; on a text field it opens the virtual keyboard |
| B | `"Escape"` | `"GamepadB"` | closes an open select or a modal `<dialog>`; otherwise nothing: your menu closes on it |
| X, Y, shoulders, triggers, View, Menu, Guide, stick buttons | the `code` | `"GamepadX"`, `"GamepadY"`, `"GamepadLeftShoulder"`, `"GamepadRightShoulder"`, `"GamepadLeftTrigger"`, `"GamepadRightTrigger"`, `"GamepadView"` (Back), `"GamepadMenu"` (Start), `"GamepadGuide"`, `"GamepadLeftThumbstickButton"`, `"GamepadRightThumbstickButton"` | none |

- `event.gamepad` is the pad's id (`-1` for the keyboard), `event.keyCode` the keyboard key it
  stands for (`Input.KEY_DOWN`, `Input.KEY_ENTER`, `Input.KEY_ESCAPE`; 0 for the others), so a
  listener written for the keyboard (`event.key == "Escape"`) serves the gamepad too.
  `event:preventDefault()` cancels the default action.
- Navigation is spatial unless the focused element declares `nav-*` for that direction (which
  then decides, as for the arrow keys): the nearest focusable element in that direction, inside
  the list (the nearest scroll container) first, then anywhere in the document where it is
  visible. There is no wrap-around. A text field or a closed select never keeps the d-pad.
- Held d-pad and stick directions repeat after 0.4 s, every 0.1 s (`event["repeat"]` is true).
  The stick counts past half way (released below 0.3); triggers count past half way.
- **Consumed**: with such an element focused, the d-pad, the left stick and A are the
  document's (the player does not walk while a menu is used, also at the menu's edge); B and
  the other buttons only when a listener calls `preventDefault()` or `stopPropagation()` (or a
  control uses them); everything else goes on to the game's `onGamepadButtonDown/Up` and
  `onGamepadAxisMove`. A stick the menu takes is reported once at rest to the game. With no
  such element focused (nothing focused, or a clicked panel), the gamepad is the game's; the
  document still hears the keys. Focus a button to give a menu the gamepad (`autofocus`,
  `el:focus()`), blur it (or hide the document) to give it back: a stick still held then
  reaches the game in the next frame at its value.
- `Input.isGamepadButtonPressed` / `Input.getGamepadAxis` always read the device: a game that
  polls checks `Engine.wantsGamepadNavigation()` (true while a document has such a focus, or a
  component UI Button is selected: jasper://docs/ui).
- `:focus-visible` shows on the element the gamepad moves to, and on a clicked element once the
  gamepad is used, so style it (`.btn:focus-visible`).
- Documents on meshes get the gamepad like the keys: the one that took the last press.

### Touch, text fields and the cursor

**Touch.** Every finger is a pointer of its own: its own `:hover` (only while it is down),
`:active`, capture, drag and click. Two fingers press two buttons; lifting one releases and
clicks only its own. Hover-only affordances do not exist on touch: show them by default or on
`:focus`/`:active`, or use `@media (hover: hover)`, evaluated once at load (false on touch
devices). A finger that starts on a scrollable box pans it once it moves past a few pixels,
where `touch-action` allows (then it gets `pointercancel` and loses `:active`); on-screen
controls and game surfaces use `touch-action: none`. Linux desktops have no multi-touch: the
Game view's Simulate Touch with an Alt-pinned finger and `send_input` cover testing.

**Pointer events.** As in a browser (W3C Pointer Events 3): per pointer `pointerover` →
`pointerenter` → `pointerdown` → `pointermove`s → `pointerup` → `click` → `pointerout` →
`pointerleave` (a finger leaves with its release), and the compatibility mouse events after the
pointer event of the same moment (`mousedown` after `pointerdown`). `preventDefault()` on
`pointerdown` suppresses the `mousedown` and the focus change. A finger is captured by the
element it pressed (implicit capture): its moves, up and cancel go there wherever it is, while
`:hover` follows what is under it. `el:setPointerCapture(event.pointerId)` captures the mouse
or a finger for a drag (a custom slider, a joystick); `el:releasePointerCapture(id)`,
`el:hasPointerCapture(id)`; `gotpointercapture` / `lostpointercapture` fire before the
pointer's next event, and the capture ends with the release. Deviation from browsers: the
compatibility mouse events fire for every finger, not only the first (the engine's earlier
advice was `mousedown` for touch, which keeps working with two fingers). `touch-action`: `auto`
(default), `none`, `pan-x`, `pan-y`, `pan-x pan-y`, `manipulation` (= auto: no zoom here); not
inherited, the effective value is the intersection from the touched element up to its scroll
box. Gestures (pinch, rotate) are two pointers: keep each `pointerId`'s position.

```lua
-- a custom slider: the knob follows the finger or mouse that grabbed it
knob:addEventListener("pointerdown", function(e)
    knob:setPointerCapture(e.pointerId)
end)
knob:addEventListener("pointermove", function(e)
    if knob:hasPointerCapture(e.pointerId) then setValue(e.clientX) end
end)
```

**On-screen controls.** `<onscreen-stick>`, `<onscreen-button>` and `<touch-area>` feed the
input actions (`jasper://docs/input` 4.1), so `Input.getAxis("Horizontal")` reads the stick and
mouse-look code reading `Mouse X`/`Mouse Y` works with the touch area. Each takes the first
pointer that presses it; three fingers on three controls work at once.

```html
<onscreen-stick mode="floating" dead-zone="0.1" horizontal-action="Horizontal" vertical-action="Vertical"></onscreen-stick>
<touch-area sensitivity="0.1" invert-y></touch-area>
<onscreen-button action="Jump" value="1" press-on-enter>Jump</onscreen-button>
```

| Element | Attributes (defaults) | State and events |
|---|---|---|
| `onscreen-stick` | `mode` (`fixed`, `floating`: the base appears under the finger, `dynamic`: it follows past `move-threshold`), `handle-range` (1), `dead-zone` (0), `move-threshold` (1), `axes` (`both`, `horizontal`, `vertical`), `snap-x`, `snap-y`, `horizontal-action` (`Horizontal`), `vertical-action` (`Vertical`), `visibility` (`always`, `touch-devices-only`) | `data-pressed`, `data-x`, `data-y` (-1..1, y up); `:active` while held; `stickstart`, `stickmove`, `stickend` (`event.detail.x` / `.y`) |
| `onscreen-button` | `action` (`Fire1`), `value` (1; -1 on an axis), `press-on-enter`, `release-on-leave`, `visibility` | `data-pressed`, `:active`; the usual pointer and click events |
| `touch-area` | `horizontal-action` (`Mouse X`), `vertical-action` (`Mouse Y`), `sensitivity` (0.1), `invert-x`, `invert-y`, `visibility` | `data-pressed`; `areadrag` (`event.detail.dx` / `.dy`, document px) |

A stick without children gets `<div class="stick-base"><div class="stick-knob"></div></div>`
(a 160 px translucent circle and a 64 px knob from the UA sheet; give your own children these
classes to restyle). The stick maths is Joystick Pack's in document px: the base's half width is
the radius; the knob moves with `transform: translate(...)`. Released, removed, hidden with
`display: none`, or with its document hidden or unloaded, a control clears what it wrote.

**Text fields.** Focusing one opens the virtual keyboard (Android, iOS, Web) and places the IME
window at the caret. Composition shows inside the field on Windows, macOS, iOS, Android and
Web; on Linux the input method draws it, and in the editor's Game view composed text arrives
committed. Ctrl/Cmd + C/X/V/A copy, cut, paste and select all. `System.setClipboardText` /
`System.getClipboardText` reach the system clipboard (asynchronous on Web).

Control characters (`\b`, `\r`, `\t`, Escape) are never inserted as text; one that arrives
without its key (the Android soft backspace) acts as that key.

**Cursor.** The CSS `cursor` of the element under the pointer becomes the mouse cursor
(`pointer`, `text`, `grab`, `not-allowed`, the `*-resize` family, ...; others show the arrow).
For the game itself: `Engine.mouseCursor = CursorType.GRAB`.

**Testing input with `send_input`** (play mode): mouse and touch events take
`"target": "<selector>"` (the center of the first visible match; `to_target` for drags and
swipes; `document` picks the UI Document) instead of `x`/`y`, resolved when the event runs, so
a panel opened by the previous click can be targeted. Without `document` the topmost document
with a visible match wins (texture-mode documents are skipped); the result lists each resolved
point and its document. `key` events reach the focused element;
`text` types a string; `ime` (`text`, `cursor`) / `ime_commit` / `ime_cancel` drive a
composition; `mouse_leave` / `mouse_enter` move the pointer out of and into the game; wheel
offsets follow GLFW (`dy > 0` scrolls up); `gamepad` events (`button` `"a"`, `"b"`, `"dpad_down"`,
`"right_bumper"`, ...; `action` `press`/`down`/`up`; or `axis` `"left_y"` with a `value`) drive the
gamepad paths above.

## Responsive layout

The page is the canvas, so responsiveness is about the canvas size:

| `graphics.scaling_mode` | Canvas (= `100vw` x `100vh`) when the window changes |
|---|---|
| `letterbox`, `crop`, `stretch` | fixed at `graphics.canvas_width` x `graphics.canvas_height`: the layout never changes, the result is scaled |
| `fit_width` (default) | width fixed, height follows the window's aspect ratio |
| `fit_height` | height fixed, width follows the window's aspect ratio |
| `native` | the window in screen pixels |

(`set_project_settings {"values": {"graphics.canvas_width": 1280, "graphics.scaling_mode": "fit_width"}}`.)

- Write the layout with flex and grid, `%`, `fr`, `min()`/`max()`/`clamp()` and `vw`/`vh`, and
  `@media` queries on `width`, `height`, `aspect-ratio` and `orientation` for portrait phones
  and small windows. Media queries are evaluated again when the canvas is resized.
- A texture-mode document has its own viewport: its texture size divided by `pixelRatio`.
- Test at several sizes without changing the project: `assert_ui {"viewports": [[1280, 720],
  [1920, 1080], [1024, 768], [720, 1280]]}` runs every check at each size, and
  `inspect_ui {"viewport": [720, 1280]}` returns the tree laid out at that size (both restore
  the real size). For a screenshot at another aspect, `configure_game_view {"aspect": "9:16"}`.

### UI scaling (Canvas Scaler)

Screen documents lay out in the scene's **UI size** when a Canvas Scaler scales it
(`graphics.ui_scale`, the scene setting `canvas_scaler`; modes and formulas: `jasper://docs/ui`),
the canvas above otherwise (the default, Follow Game Canvas). With Scale With Screen Size 1920 x
1080 and `match_mode: expand`, `100vw` x `100vh` is 1920 x 1440 on a 4:3 screen and 2400 x 1080 on
a 20:9 one: design for the reference, let flex and `%` take the extra room. Text is rasterized at
the screen's pixels either way. Preview real devices with `configure_game_view {"resolution":
"2400x1080"}` (or a preset, `"WxH@dpi"`, `"free"`), then `screenshot`; `getBoundingClientRect()`
of `body` is the UI size.

### Safe areas (notches)

`env(safe-area-inset-top|right|bottom|left[, fallback])` is the live safe-area inset of the
screen (`Screen.safeAreaInsets`, the app's extra insets such as a banner included), in the
document's CSS px, as in a mobile browser with `viewport-fit=cover`:

```css
.hud { padding: env(safe-area-inset-top) env(safe-area-inset-right) 0 env(safe-area-inset-left); }
.bottom-bar { position: absolute; left: 0; right: 0; bottom: max(12px, env(safe-area-inset-bottom)); }
```

It is a runtime variable (`var(--jasper-safe-area-inset-<edge>, <fallback or 0px>)`): a rotation or
`Screen.setExtraInsets` lays the document out again without a reload; `calc()`, `min()`, `max()`
and `clamp()` keep it. `--jasper-device-safe-area-inset-*` leave the extras out. Documents on meshes
get 0. Try it with a phone in the Game view (`configure_game_view {"device": "iphone_15_pro"}`);
details in jasper://docs/safe-area.

## The loop in detail

### UI diagnostics

Everything the engine drops, approximates or cannot find is reported with the place in **your**
file, a stable code and a fix:

```text
[HTML UI] ui/menu.css:42:3: warning: 'font-variant-numeric' is not supported; dropped (fix: use a font with tabular figures by default, or fixed-width inline-blocks for digits) [HTMLUI-CSS-0002]
[HTML UI] ui/menu.html: 0 errors, 1 warning, 7 notes
```

- Format `path:line:column: severity: message (fix: suggestion) [code]`. Codes are
  `HTMLUI-<area>-<number>` (areas `GEN HTML CSS SEL AT IMP SVG`), listed with their meaning at
  the end of `jasper://docs/html-ui-support`.
- **Errors**: something the page needs is missing (a file, an import) or will never run
  (JavaScript). **Warnings**: drawn differently from Chrome. **Infos** (notes): exact rewrites
  and harmless drops; counted in the summary, listed with Editor Settings > HTML UI > Verbose
  Diagnostics or `doc:getDiagnostics()`.
- Where they appear: in `external_changes.problems` of the next MCP call after you save (with
  `col`, `severity`, `code`, `suggestion`); in `check --ui`; in the Console at every load
  (`read_console`); in `inspect_ui` (`diagnostics`); from Lua `doc:getDiagnostics()` (infos
  included, each `{severity, code, name, message, suggestion, file, line, column, text}`;
  `eval_lua` with `allow_edit_mode` reads them without playing); in the UI Document's
  Inspector (*Show in Console*). What RmlUi itself rejects comes back as `HTMLUI-GEN-0006` at
  your line.
- Work through errors, then warnings. Infos need no action.

### `check --ui` and the hook

`jasperengine-editor check --project <dir> --ui [--files <paths...>] [--format json]` runs the
same conversion without the editor (about 1 ms per document): each `.html` with its
stylesheets, imports, images and fonts, and each `.css` no document uses. `--ui` is part of the
default `check` and of the `--hook` mode, which shows a UI warning once, after the edit that
introduced it (errors every time). JSON findings carry `code` and `suggestion`. Details:
`docs/mcp/checks.md` in the engine repository; `jasperengine-editor ui-support` prints the support
tables as JSON.

### UI hot reload

The editor checks every document's files (the `.html`, stylesheets, templates, images, fonts,
SVG and Lottie files, imports) every 0.25 s and before every MCP call, in edit mode and during
play:

| Changed | Effect |
|---|---|
| a `.css`, linked or `@import`ed | restyled in place: elements, listeners, scroll and focus stay |
| an image | redrawn with the new file; the document stays |
| the `.html` | patched in place (or rebuilt, see "Reloads, `onLoad` and lifetimes"); a changed `<script type="text/lua">` runs again in play; `onReload` (patch) or `onUnload` then `onLoad` (rebuild) fire |
| an SVG, a Lottie file, a font, a `<script src>` Lua file, a `<markdown src>` file | the document reloads; its scripts run again in play |
| `assets/htmlui-imports/manifest.json` | documents using the imported URL restyle or reload |

Files that did not exist yet count too (creating a missing stylesheet loads it), and a save in
the editor's code editor reloads at once. The call after your save reports
`external_changes.ui_reloaded` (`[{scene, entity, html, stylesheet_only}]`) and the problems of
the changed files. A reload is not an edit: the scene stays saved. C++:
`UIDocumentSystem::reloadChangedFiles(files)` (stamps shared by the poller and the MCP pick-up,
so a change reloads once), `reloadDocument(entity, preserveState)`, `getLoadCount(entity)`,
`getDocumentFiles(entity)`, `getDocumentMessages(entity)`.

### The UI tools' shared rules

`inspect_ui`, `assert_ui`, `set_ui_effect_time` and `send_input`'s `target` share these rules:
`entity` is the GameObject with the UI Document (omitted: the scene's only one, else the only
one of the open scenes, or of the running ones in play); a `ref` they return works as a selector
again; boxes are `[x, y, width, height]` in canvas px (CSS px), origin top left; they wait up to
5 s for a document to load and report why one did not. A `<video>` is inspected like any element
plus its `media` state; in play with `fixed_fps` it is frame-exact, so its screenshots repeat.

### `inspect_ui`

Read-only, edit mode and play. Returns:

- `document` (`html`, `viewport`, `load_count`, `render_target`, `sort_order`, diagnostics
  counts) and the `diagnostics`;
- `elements`: the tree as `{ref, tag, id, class, text, box: [x, y, w, h], flags, pseudo}`.
  `ref` is a selector that finds the element again (`#play`, else a path of `tag:nth-child(n)` steps),
  `box` is in canvas px (the coordinates `send_input` uses), `flags` are `overflow-x`,
  `overflow-y`, `clipped`, `hidden`, `transparent`, `offscreen`, `zero-size`;
- with `"detail": "full"` (up to 20 elements) the box model and every computed style with the
  `file:line:col` and selector of the rule that set it (`user-agent`, `inherited from #panel`,
  `(style attribute)`);
- `"at": [x, y]`: the element stack under a point (empty where the pointer passes through);
- `"models": true` (play): the data models' values;
- `"styles": ["color", "font-size"]`: those computed values on every element;
- `"screenshot": true`: the Game view cropped to the matches, outlined;
- `"file": "assets/ui/x.css"`: a file's diagnostics without a live document;
- `"render_stats": true`: the last frame of the document's context, every counter by its name
  (`drawCalls`, `geometrySubmitted`, `runsMerged`, `runCacheHits`, `instancesDrawn`,
  `stencilDraws`, `stencilClipsElided`, `analyticClips`, `boxShadowTextures`, `layersPushed`,
  `glyphsRasterized`, `msdfGlyphsPending`, `textRegenerations`, `textEffectFallbacks`,
  `atlasBytes`, `atlasEvictions`, ...); with `"detail": "full"` each element also has
  `paint: {path: "primitive" | "mesh", reason}` and, with text, `raster: {mode: "bitmap" | "msdf" |
  "stretched", scale}`;
- a `<video>`'s entry has `media`: `currentSrc`, `backend`, `currentTime`, `duration`, `paused`, `ended`,
  `seeking`, `loop`, `muted`, `volume`, `playbackRate`, `readyState`, `networkState`,
  `videoWidth`/`videoHeight`, `frame` (the index on screen), `droppedFrames`, `decodeMs`,
  `memoryBytes`, `poster` (showing it), `error`, and with `controls` their rectangles
  (`controls: {visible, bar, play, timeline, mute}`, for `send_input` clicks);
- a virtual view's entry has `widget`: `kind`, `count`, `first`/`last` (the items shown,
  0-based), `pool` and `pool_bound` (row elements), `overscan`, `lanes`, `selection`, `selected`,
  `selected_keys`, `active`, `scrollTop`, `totalHeight`, `source` (`model:<name>` or `lua`),
  `sort`; its text line reads `list[0..12 of 10000, pool 22]`;
- `tooltip` (`{visible, source, text, rect, side}`) and `drag` (`{active, source, target, types,
  allowed, effect, programmatic}`) of the document, and with `"reload": true` the last hot reload's
  report (`mode`, the counts, `fallbackReason`, `ms`, `causes`).

Use it to answer "why is this element there / this size / this colour" before editing CSS.

### `assert_ui`

Checks the layout and returns `passed`, each failure with its element `ref`, numbers and
rects, and on failure an image with the failing boxes outlined (red; the overlap partner or
container amber). A failed check is not a tool error. Checks: `exists`, `count` (`count`,
`min`, `max`), `visible` (`unobstructed`: nothing of another document covers it), `text_fits`
(`single_line`, `allow_scroll`), `no_overlap` (`with`), `within_parent` (`container`),
`within_viewport`, `contrast` (`min_ratio`, default WCAG AA; `background` where the page is
transparent), `min_size` (`width`, `height`), `virtual_window` (a view's rows within `max`,
default its own bound), `tooltip` (the selector's tooltip is showing; `text`: it contains that),
`selected` (a view's selection: `count`, `min`, `max`). Zero matches fail. `viewports` repeats the checks
at other sizes. Typical sets:

| Screen | Checks |
|---|---|
| Menu | `count` of the buttons, `visible` + `unobstructed`, `text_fits` + `single_line`, `no_overlap`, `min_size` 44 x 44 (touch), `contrast` |
| HUD | `within_viewport` of every panel at every target aspect, `no_overlap` of the corners, `text_fits` of counters (widest value), `contrast` with `background` (the game shows through) |
| Grid (inventory, shop) | `count` of the slots, `within_parent` with `container`, `no_overlap`, `text_fits` of names and prices |
| Dialog, modal, pause | `visible` + `unobstructed` after opening it, `within_viewport`, `min_size` of its buttons |
| Settings, forms | `text_fits` of labels, `within_parent` of rows, `min_size` of controls |
| Toasts, lists | `no_overlap`, `within_viewport` with the most entries shown |

### `edit_ui_view`

In the `html_ui` tool group (`tool_groups {"enable": ["html_ui"]}` lists it; a call by name
works either way). Drives a virtual view of a playing document (runtime only, not undoable): `{"selector": "#servers",
"action": "scroll_to" | "select" | "activate" | "expand" | "collapse" | "sort", "index": 0-based
| "key": "...", "column": "ping", "direction": "asc" | "desc", "align": "start" | "center" | "end"
| "nearest"}`. `activate` and `sort` fire the view's events, so the script reacts as to a player;
it returns the items shown and the selection. Hover with `send_input` (`target`, then
`frames_after`) to show a tooltip; drag with `to_target` for drag and drop.

### UI screenshots

`screenshot {"view": "game"}` shows the whole Game view (documents of the selected scene in
edit mode, of the running scenes in play). `inspect_ui`/`assert_ui` return cropped, outlined
images of what they matched: cyan for matches, red for failures, amber for the overlap partner
or container of a failure. Texture-mode documents, documents of a scene the Game view does not
show, and other viewports have no image (the tools say so). The structured result says where
the image is: image.canvas_rect is the canvas rectangle it shows, in canvas px.

### UI Documents in the editor

- *Assets > Create > HTML Document* writes `NewDocument.html` and `.css`; *Create > UI > UI
  Document* in the Hierarchy adds the GameObject; drag the `.html` onto the Inspector's
  **Source**.
- The UI Document **Inspector**: Source with **Open** and **Reload**, the component's
  properties, **Remote resources**, **Diagnostics** (counts, *Show in Console*), the
  **Messages** RmlUi logged, and **Show HTML UI Debugger** (RmlUi's element inspector and log,
  also on the Game view toolbar while playing; one at a time; compiled into the editor and Debug
  exports only).
- Export check of an `.rml` document: its references (`<link>`, `<img>`, `<lottie src>`,
  `@import`, `url()`, `@font-face`, `decorator: image()`) are followed; a missing file, a path
  leaving the assets directory or a remote URL fails, an absolute path warns.
- `.html` and `.css` files open in the code editor with HTML and CSS highlighting and
  property suggestions.

## Habits from the web that need a change

| Web habit | In the engine | Reported as |
|---|---|---|
| `<script>` with JavaScript | `<script type="text/lua">` or a Lua script on the GameObject | `HTMLUI-HTML-0006` (error) |
| `onclick="go()"` | `el:addEventListener("click", fn)` or `data-event-click="go()"` with a data model | `HTMLUI-HTML-0005` (error) |
| `document.querySelector(".x")` in a scene script | `local doc = UIDocument(self.scene, self.entity)`, then `doc:querySelector(".x")` (colon); `document` exists only in `<script type="text/lua">` | LuaLS `undefined-global` |
| `doc.getElementById("x")`, `el.classList.add("x")` | colon calls: `doc:getElementById("x")`, `el.classList:add("x")` | LuaLS `param-type-mismatch` / `missing-parameter` |
| `el.innerText = "x"`, `el.onclick = fn` | `el.textContent = "x"`, `el:addEventListener("click", fn)` | LuaLS `inject-field` |
| `getElementByID`, other misspelt members | the DOM names (`getElementById`) | LuaLS `undefined-field` |
| `addEventListener("onclick")`, `"touchstart"` | `"click"`, `"pointerdown"` (`event.pointerType == "touch"`) | LuaLS `param-type-mismatch` (`UIEventType`); a warning at run time |
| `setTimeout`, `setInterval`, `requestAnimationFrame` | `anime.createTimer({ duration = 2000, onComplete = fn })` (UI clock, milliseconds), gameplay `Timer.after(2, fn)` / `Timer.every(0.5, fn)` (seconds, game time; `{unscaled = true}`), `Coroutine.start(function() Coroutine.wait(2) ... end)`, or `RegisterEngineEvent(self, "onUpdate")` | none (Lua has no such globals) |
| `fetch`, `XMLHttpRequest` | call Lua `Http` (`jasper://docs/networking`): in the document's `<script type="text/lua">` `Http.get(url, {owner = document}, function(res) ... end)` (no `self` there; the document's GameObject owns it), in the GameObject's script `{owner = self}`; documents themselves load nothing from the network | none |
| `localStorage` | `UserSettings.setStringForKey(key, value)` / `getStringForKey` | none |
| `alert()`, `confirm()` | a `<dialog>` with `showModal()`, or a modal element | `HTMLUI-HTML-0006` (inside JavaScript) |
| JS animation libraries (GSAP, anime.js `<script>`) | the Lua `anime` API | `HTMLUI-HTML-0007` |
| Tailwind Play CDN | run the Tailwind CLI and `<link>` its CSS output | `HTMLUI-HTML-0007` |
| Google Fonts `<link>` | keep it; the editor imports it (`import_files {"from_document": ...}` or `{"fonts": [...]}`) | `HTMLUI-IMP-0001` until imported |
| remote `<img src="https://...">`, CDN CSS | keep it and import it, or put the file in `assets/ui/<name>_assets/` | `HTMLUI-IMP-0001` until imported |
| WOFF/WOFF2 `@font-face` | TTF or OTF | `HTMLUI-CSS-0022` |
| `@container`, `container-type` | `@media` on the canvas size, or classes set from Lua | `HTMLUI-AT-0001`, `HTMLUI-CSS-0002` |
| `mix-blend-mode`, `background-blend-mode` | `opacity`, or a pre-blended image | `HTMLUI-CSS-0002` |
| `writing-mode: vertical-rl` | `transform: rotate(90deg)` | `HTMLUI-CSS-0002` |
| `columns: 2` | grid or `flex-wrap` | `HTMLUI-CSS-0002` |
| `line-clamp: 2` (the standard shorthand) | `display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; overflow: hidden` | `HTMLUI-CSS-0002` |
| `height: 100dvh`, `-webkit-fill-available` fixes for mobile browser bars | `100vh` is the canvas, exact | none |
| hover-only affordances on touch | visible by default or on `:focus`/`:active`; `@media (hover: hover)` is false on touch devices | `HTMLUI-AT-0006` (info) |
| `data-for` over thousands of items | `<list-view>` / `<grid-view>` / `<table-view>` ("Long lists") | `HTMLUI-HTML-0031` (info) |
| a JavaScript Markdown library | `<markdown>`, `data-markdown`, `el:setMarkdown` | none |
| `<canvas>`, `<iframe>` | Lottie, images, another UI Document, a texture-mode document | `HTMLUI-HTML-0002` |
| an MP4 / MOV / VP9 `<video>` | AV1 + Opus WebM (`import_files` converts it) | `HTMLUI-HTML-0027` |
| `background: url(clip.webm)` | a `<video>` with `object-fit: cover` behind the content | `HTMLUI-CSS-0004` |
| `<div/>` | `<div></div>` | `HTMLUI-HTML-0026` |
| `<input type="date">`, `type="color"` | your own picker in HTML | `HTMLUI-HTML-0008` |
| 0-based `data-for` index in Lua | `model.items[i + 1]` | none |
| a white page by default | the page is transparent: set a background for full-screen menus | none |

### Changes for existing documents (S17 widgets)

- `dragover` follows HTML5: it fires on entry **and on every move** over the element (at most
  once per frame). RmlUi's enter-once event is `dragenter` now; move a handler that should run
  once there. `dragmove`, `dragout` and `dragdrop` are unchanged.
- `title="..."` shows a tooltip (it was ignored with `HTMLUI-HTML-0004`). Turn tooltips off with
  `doc.tooltip:configure{enabled = false}`.
- Hot reloads of `.html` documents patch the live document: `onReload` runs instead of
  `onUnload`/`onLoad`. Preferences > HTML UI > Hot Reload > Rebuild (or `doc:reload{mode =
  "rebuild"}`) brings the old behaviour back.

## Examples

Thirteen polished examples plus `blank`, each a standalone `.html` + `.css` + Lua script (and
`assets/`), compat-clean, tested at several viewports. Get them with
`create_ui_document {"html": "assets/ui/<name>.html", "template": "<name>"}` (files renamed to
your document: `<stem>.css`, `<stem>_assets/`, `scripts/<Stem>UI.lua`), read them in
`.jasperengine/engine-api/html-ui-examples/<name>/` of every project, or as
`jasper://examples/html-ui` (the index with each one's checks) and
`jasper://examples/html-ui/<name>/<file>`.

| `template` | Shows | Its `assert_ui` checks |
|---|---|---|
| `main_menu` | title screen: animated gradient backdrop, icon buttons, keyboard navigation, a slide-in settings panel; flex, grid areas, transitions, keyframes, `backdrop-filter`, anime timeline + stagger, built-in Lucide icons (`data-lucide`), `:focus-visible` | count, visible, text_fits, no_overlap, within_viewport, contrast, min_size of `.menu-button` / `.menu` |
| `pause_menu` | Escape toggles a blurred overlay (`backdrop-filter`) and `Engine.timeScale`; anime entrance | visible, text_fits, no_overlap, within_viewport, contrast, min_size |
| `hud` | health and shield bars, score and timer, minimap, ammo, ability cooldowns, damage flash; data binding (`{{ }}`, `data-style-width`, `data-class-*`) | within_viewport, no_overlap, text_fits, contrast with background, visible |
| `inventory` | a CSS grid of slots from `data-for`, HTML5 drag and drop (`draggable`, `dropzone`, `dataTransfer`, the ghost and drop classes), rich tooltips from `setTooltip`, equipment | count, no_overlap, within_parent, within_viewport, text_fits, visible |
| `settings` | tabs, sliders, switches, selects, key bindings; two-way `data-value`/`data-checked` | visible, no_overlap, min_size, text_fits, within_parent, within_viewport |
| `dialogue` | portrait, typewriter text, choices; `onUpdate` timing | text_fits, within_viewport, contrast, no_overlap, min_size |
| `shop` | filter chips, cards with `data-for` + `data-if`, buy buttons disabled by `data-attr-disabled`, a quantity dialog, an animated gold counter | text_fits, no_overlap, within_parent, contrast, min_size, within_viewport |
| `toasts` | toasts created from Lua (`createElement`, `appendChild`), enter and exit animations | within_viewport, no_overlap, text_fits, contrast |
| `loading` | progress bar, Lottie spinner, rotating tips, "press any key" | visible, text_fits, within_viewport |
| `video_menu` | a title screen over a looping `<video autoplay muted loop>` (`object-fit: cover`) under a gradient scrim; a toggle pauses the video from Lua, its label follows the `play`/`pause` events, an `error` leaves the gradient | count, visible, text_fits, no_overlap, within_viewport, contrast, min_size |
| `server_browser` | 10 000 servers in a `<table-view>`: sort, resizable columns, region chips, keyboard and gamepad selection, row and header tooltips, a details panel | min_size, no_overlap, text_fits, virtual_window, visible, within_viewport |
| `chat_log` | Markdown messages of any height in a `<list-view item-height="auto" anchor="end">`, a composer, teammates writing on game time | contrast, min_size, text_fits, virtual_window, visible, within_viewport |
| `quest_log` | chapters and quests in a `<tree-view>` with keyboard and gamepad navigation, a `<markdown src>` journal | text_fits, virtual_window, visible, within_viewport |
| `blank` | an empty page with a heading (the default template; no script) | visible |

Each example's Lua class is `<Name>UI`, gets the document with
`UIDocument(self.scene, self.entity)`, looks elements up through an asserting `byId`, and binds
its listeners in a method called from `init` and registered on `doc.onLoad`.

## Performance

- **Animate `transform` and `opacity`**, not layout properties (`width`, `top`, `margin`,
  `font-size`): transforms and opacity change one value per frame, layout properties lay the
  document out again. For a bar animated every frame, `transform: scaleX()` beats `width`; a
  width set once per event (the HUD above) is fine.
- **Layers cost passes.** A static document without filters or masks is one offscreen pass and
  uploads nothing per frame. Every `filter`, `backdrop-filter` and `mask-image` draws through
  extra offscreen passes (a blur 4 to 6, a backdrop blur a few more), and `opacity` below 1 on an
  element with several children draws it into a layer: keep them on the few elements that need
  them. Box shadows, blurred `text-shadow`s and `font-effect`s do not need layers (below).
- **Draws are batched.** Consecutive boxes and text of the same clip and transform are one draw:
  a list of 500 cards with rounded borders, box shadows and two labels each is a handful of draws.
  Solid backgrounds, `solid` borders (one colour per rounded corner), `solid` and `auto` outlines
  and every `box-shadow` (blur, spread, inset, several) are drawn as antialiased shapes, without
  shadow textures. What keeps the old (still correct, slower) mesh path: gradients and images as
  backgrounds, `dashed`/`dotted`/`double`/... borders, borders of two colours meeting on a curve,
  `background-clip` other than `border-box`, a `box-shadow` under `opacity` (`inspect_ui` detail
  full says why per element: `paint`). Rounded `overflow` clips whose content stays clear of the
  corners (padding at least the radius) cost nothing; one or two rounded clips the content reaches
  are clipped in the shaders; deeper nesting uses the stencil (more draws).
- **Text follows its on-screen size.** Text under `transform: scale()`, at a fractional pixel
  ratio or on a scaled canvas is rasterized at its real size (as sharp as that font size); while
  a scale animates it draws from distance-field glyphs and is rasterized again once it rests
  (6 frames later). `text-rendering: optimizeSpeed` keeps the old stretched bitmap (cheapest),
  `geometricPrecision` always draws distance fields (any size, 3D, no re-rasterization).
  `text-shadow` blurs and `font-effect` outlines, glows and blurs up to 0.3 em are drawn from the
  distance glyphs without layers; larger ones fall back to layers (or CPU glyphs).
- **Keep documents small**: hundreds of elements, not thousands. Long lists: page or window
  them in Lua. One document per screen; hide the screens you do not show (`doc:hide()`, or a
  class with `display: none`) instead of destroying and rebuilding them.
- **Change the least**: assigning one data-model field or `textContent` updates one element;
  replacing `innerHTML` rebuilds the subtree and drops its listeners.
- **Images**: an SVG is rasterized once per size and colour; a Lottie element re-rasterizes
  only when its frame changes (a 30 fps animation 30 times a second; paused, never). Texture
  documents redraw only when they change.
- Anti-aliasing: Project Settings > Graphics > HTML UI MSAA (`graphics.html_ui_msaa_samples`:
  1, 2, 4 or 8, default 4; devices that cannot multisample use 1).
- Project Settings > Graphics > HTML UI also has *Glyph Atlas Budget (MiB)*
  (`graphics.html_ui_glyph_atlas_mib`: 0 = the platform's default, 24 MiB on desktop and 12 on
  Android, iOS and the web, else 4-256; pages of glyphs not drawn lately are cleared when text needs
  more, and a warning `HTMLUI-GEN-0007` says when that happens all the time), *SDF Primitives*
  (`graphics.html_ui_sdf_primitives`) and *Transform-Aware Text*
  (`graphics.html_ui_transform_aware_text`), both on by default; all three are undoable and go
  into exports.
- Measure: the Game view's statistics overlay (`configure_game_view {"show_stats": true}`)
  shows the documents' passes, draws (merged), instances, stencil draws and the glyph atlas of
  the last frame; `inspect_ui {"render_stats": true}` returns every counter of the document's
  context, `doc:getRenderStats()` the same table in Lua (an in-game HUD),
  `UIDocumentSystem::getRenderStats()` in C++, and `jasperengine-editor benchmark --ui-stats`
  their averages.

## C++

C++ scripts have the same API with get/set methods (`#include "JasperEngine.h"`); events take
`std::function`s and return ids, data models are `UIDataModel` handles over `UIValue` trees.
The headers are in the project's `.jasperengine/engine-api/core/` (`object/ui/UIDocument.h`,
`UIElement.h`, `UIEvent.h`, `UIDataModel.h`, `htmlui/HtmlUI.h`, `htmlui/anim/Anime.h`).

```cpp
UIDocument menu(scene, entity);
UIElement play = menu.getElementById("play");
uint64_t id = play.addEventListener("click", [](UIEvent& event){ event.getTarget().addClass("pressed"); });
play.removeEventListener(id);

UIValue initial = UIValue::object();
initial["gold"] = 10;
initial["items"] = UIValue::array();
UIDataModel shop = menu.createDataModel("shop", initial);
shop.set("gold", 20);                           // paths are 0-based: "items[0].name"
shop.bindEvent("buy", [](UIEvent& event, const std::vector<UIValue>& args){ /* args[0] = i */ });
shop.onChange("gold", [](const UIValue& value){ });
```

- `UIElement` also has `setClass(name, bool)`, `hasClass`, `setStyleProperty` (false and a
  warning when RmlUi rejects the value), `removeStyleProperty`, `getBoundingBox()` (canvas
  pixels), `isPseudoClassSet("hover")` and the Lottie methods. A call on an element that no
  longer exists logs once per method and returns a default.
- `UIDocumentSystem` (the scene's system): `reloadChangedFiles`, `getDiagnostics(entity)`,
  `getLoadCount(entity)`, `getDocumentFiles(entity)`, `setPreferredSampleCount` (MSAA),
  `getRenderStats()`.
- `HtmlUI::loadFontFace(path[, fallback, family, weight, italic])` loads a TTF/OTF face.
- Inspection and layout assertions from C++ (what `inspect_ui`/`assert_ui` use):
  `htmlui::HtmlUIInspector` and `htmlui::HtmlUIAssertions` (`htmlui/HtmlUIInspector.h`,
  `htmlui/HtmlUIAssertions.h`).

## Effects

Effects are GPU shaders you use from CSS like images and filters: `effect(<name>, <param>
<value>, ...)` in `background-image`, `mask-image`, `filter` and `backdrop-filter`. The engine
ships a library of them (gradients, noise, patterns, light, glass, blurs, stylize, colour,
distortion, transitions, pointer effects, and simulations: fluids, particles, feedback trails);
every effect, its parameters and a thumbnail are in
the catalogue `jasper://docs/html-ui-effects` (`jasperengine-editor ui-effects` lists them).
A project can add its own (Writing an effect, below).

### Effects: quick reference

- Names are kebab-case; arguments are always named (`speed 2`, not `2`), separated by commas,
  in any order. Omitted parameters take their defaults: `effect(stripes)` works.
- **Paint effects** draw an image: an image layer in `background-image`/`background`, or a
  mask in `mask-image`. In `filter`/`backdrop-filter` (their *filter form*) they recolour the
  content inside its alpha.
- **Filter effects** process pixels: `filter` processes the element and its children,
  `backdrop-filter` what lies behind the element (effects made for it are marked backdrop in
  the catalogue, like `frosted-glass`).
- Effects mix with everything else in the same property, in CSS order: gradients and `url()`
  layers, `blur()`, `drop-shadow()` and the other filter functions.
- Any other property (`color`, `border-image`, `content`, ...) does not take `effect()`: the
  declaration is dropped with an error.

The examples below are rendered by the engine's tests:

```css
/* an aurora over a dark background */
.hero { background: #0b0d14; background-image: effect(aurora, speed 2, color-b #44ccff); }
/* frosted glass over what is behind a rounded panel */
.panel { backdrop-filter: effect(frosted-glass, blur 14px, tint #ffffff22); border-radius: 16px; }
/* a colour-split title with a standard drop shadow after it */
.title { filter: effect(chromatic-aberration, strength 0.3) drop-shadow(0 2px 4px #0008); }
/* layered: film grain over a gradient over a colour, rounded */
.card { background: effect(grain, amount 0.08), #101218 linear-gradient(#223, #112); border-radius: 12px; }
/* gradient text */
.logo { color: transparent; font-size: 48px; font-weight: bold; background-image: effect(holographic, speed 0.6); background-clip: text; }
/* a dissolve-out transition driven by keyframes, burning at the edge */
@keyframes vanish { from { filter: effect(noise-dissolve, progress 0%) } to { filter: effect(noise-dissolve, progress 100%, edge-color #ff8a00) } }
.closing { animation: vanish 0.6s ease-in forwards; }
/* a sheen that lights up on hover */
.button { background-image: effect(sheen, intensity 0); transition: background-image 300ms; }
.button:hover { background-image: effect(sheen, intensity 0.8); }
/* a pointer light over a frosted panel */
.menu { backdrop-filter: effect(frosted-glass, blur 12px); background-image: effect(spotlight, radius 180px, intensity 0.25); }
```

### Where `effect()` goes, layers and paint areas

| Property | Takes | Notes |
|---|---|---|
| `background-image`, `background` | paint effects | image layers: the first listed is on top, `background-color` below all of them |
| `mask-image`, `-webkit-mask-image` | paint effects | the effect's alpha masks the element |
| `filter` | filter effects, paint effects (filter form) | applied left to right with the standard functions |
| `backdrop-filter` | filter effects, paint effects (filter form) | processes what is behind the element |

- **Paint area**: each effect layer follows its `background-clip` (`border-box` by default,
  `padding-box`, `content-box`) and the element's `border-radius`.
- `background-size`, `background-position` and `background-repeat` do not apply to effect
  layers (a warning): an effect always covers its area. Use its own parameters (`scale`,
  `center`, `density`...) instead.
- `background-blend-mode` sets how an effect layer blends with what is below it: `normal`,
  `multiply`, `screen`, `plus-lighter` (or `lighter`: additive). Other modes give a warning and
  draw `normal`. The `blend` parameter does the same inside `effect()`.

### Effect parameters

Every value is CSS, checked against the parameter's type and range (the catalogue lists both).
A value of the wrong type gives a warning and the default; a value out of range is clamped with
a warning.

| Type | Write | Notes |
|---|---|---|
| number, integer | `2`, `0.35` | integers are rounded |
| percentage | `40%` or `0.4` | |
| length | `12px`, `1.5em`, `2rem`, `10vw`, `5vmin`, `25%` | `%` is of the effect area's shorter side |
| angle | `45deg`, `0.5turn`, `1rad`, `100grad`, or `45` (degrees) | |
| time | `2s`, `400ms` | |
| color | any CSS colour: `#ff8a00`, `#f80c`, `rgb(0 0 0 / 50%)`, `hsl()`, `oklch()`, `transparent`, `currentColor` | |
| position | `center`, `30% 70%`, `left 20px top 10%` | 0 to 1 across the area |
| enum | one of the listed words | |
| boolean | `on`, `off`, `true`, `false`, `yes`, `no`, `1`, `0` | |
| palette | 2 to 8 colours separated by spaces, each with an optional stop: `#00aaff #ff00aa`, `red orange 30% gold` | stops must not decrease |

Every effect also takes four **universal parameters**:

| Param | Type | Default | Meaning |
|---|---|---|---|
| `opacity` | percentage | `100%` | paint: multiplies the effect's alpha; filter: mixes the result with the unfiltered pixels (`0%`: no effect) |
| `time-scale` | number, -10 to 10 | `1` | multiplies the effect's clock; `0` freezes it |
| `time-offset` | time | `0s` | added to the clock: desynchronises repeated elements |
| `blend` | `normal`, `add`, `screen`, `multiply` | `normal` | paint effects only (a warning on a filter effect) |

### Animating effects

- **Time**: animated effects move by themselves on the UI clock (the clock of CSS animations
  and `anime`: it keeps running while the game is paused). Their speed parameters change the
  rate smoothly, without a jump.
- **Transitions and `@keyframes`** interpolate the parameters of the *same* effect:
  `transition: background-image 300ms` between `effect(stripes, density 4)` and
  `effect(stripes, density 8)` animates the density; filter lists animate the same way. A
  different effect, or a different number of layers, switches at once.
- **Custom properties**: `effect(aurora, speed var(--s))` follows `--s` when a class or a
  script changes it. An unregistered custom property changes at once; a registered one
  (`@property`) transitions and animates:

```css
@property --spin { syntax: '<angle>'; inherits: false; initial-value: 0deg; }
@keyframes turn { to { --spin: 360deg; } }
.ring { background-image: effect(conic-spin, rotation var(--spin), speed 0); animation: turn 4s linear infinite; }
```

- **`anime`** animates a custom property the effect reads, for effects driven from Lua:

```lua
local card = doc:querySelector(".card")
anime.animate(card, { ["--glow"] = 0.8, duration = 400, ease = "outQuad" })   -- .card uses effect(..., intensity var(--glow))
for _, fx in ipairs(card:getEffects()) do print(fx.property, fx.name, fx.params.intensity) end
```

- **Deterministic captures** (screenshots, tests): `UIDocument.setEffectTime(1.5)` freezes every
  effect at 1.5 s while the rest of the UI keeps running; `UIDocument.setEffectTime(nil)`
  resumes. `el:getEffects()` lists an element's effects with their current parameters
  (`property`, `index`, `name`, `form` and `params`, the canonical text of each parameter).
- Cost: a parameter changed by a transition, a keyframe, `var()` or `anime` rebuilds that
  element's effect on that frame (CPU only, no shader compile); time-only animation rebuilds
  nothing. For many elements prefer the effect's own `speed` to animating a parameter.

### Gradient text with effects

`background-clip: text` with a transparent text fill paints the text with the element's effect
layers, exactly: the text keeps its anti-aliasing, and a `drop-shadow()` in `filter` shadows
the painted text.

```css
.title { color: transparent; background-image: effect(gradient-pan, colors #ffd36b #ff6b3d #ffd36b, speed 0.3); background-clip: text; }
.banner { color: transparent; font-weight: bold; background-image: effect(aurora, intensity 100); background-clip: text; }
```

- The trigger is CSS's: `background-clip: text` (or `-webkit-background-clip`) together with
  `color: transparent` or `-webkit-text-fill-color: transparent`. With an opaque text colour the
  clip is dropped (an info), as in a browser, where opaque text hides the background: this logo
  is white text over a holographic box, not holographic text.

```css
.logo { color: #fff; background-image: effect(holographic, speed 0.6); background-clip: text; }
```

- The engine draws the text white and paints the effect over it (an info diagnostic says so).
- Unlike a browser it paints the whole content of the element, not only its glyphs: keep such
  an element to its text (no images or coloured boxes inside).

### Backdrop effects

`backdrop-filter: effect(frosted-glass, blur 12px)` blurs, tints and lights what lies behind a
panel; its `border-radius` shapes the glass. Put the panel over something with detail (an image,
the game view, other UI): over a flat colour glass shows nothing. `liquid-glass` is a lens instead:
the panel's rounded edge bends what is behind it over `thickness`, splits its colours and lights
its rim; with `blur` it is frosted too. `fluted-glass` and `glass-tiles` refract it through
reeds and tiles, and `tilt-shift` and `progressive-blur` blur it in a band or a ramp.

```css
.dock { backdrop-filter: effect(liquid-glass, thickness 28px, refraction 1.2, highlight 0.4); border-radius: 24px; }
.sheet { backdrop-filter: effect(liquid-glass, blur 10px, tint-color #8ecbff, tint-intensity 0.15); border-radius: 18px; }
```

A paint effect in `backdrop-filter` recolours what is behind
(`backdrop-filter: effect(stripes, opacity 30%)`). Backdrop effects copy the layer below once more
than a `filter` does; blurring ones (prepass effects such as `frosted-glass`) are the costliest per
pixel: use them on panels, not full screen.

### Stateful effects (simulations)

Effects of cost class `stateful` keep state from frame to frame: smoke and ink that the pointer
stirs (`fluid`, `fluid-smoke`, `smoke-flow`, `smoke-fill`, `fog`, `ink-flow`, `fluid-cursor-trail`,
`chroma-flow`), particles and flocks (`boids`, `particles`, `particle-flow`, `magnetic-filings`,
`particle-field`), pattern growth (`reaction-diffusion`) and feedback on the content
(`time-trail`, `data-mosh`, `pixel-sort`, `pixel-throw`, `shatter`). They are used like any other
effect:

```css
/* rising smoke behind a title; the pointer stirs it */
.intro { background: #0b0d14; background-image: effect(fluid-smoke, colors #b8d4ff #4466aa, speed 24); border-radius: 12px; }
/* a flock that scatters from the pointer */
.lobby { background: #070910; background-image: effect(boids, count 600, cursor-mode repel); }
/* a growing Turing pattern */
.loading { background-image: effect(reaction-diffusion, preset coral); }
/* whatever moves in the panel leaves a ghost trail */
.feed { filter: effect(time-trail, trail-length 0.8s); }
/* Pavel Dobryakov's WebGL fluid: drag to paint glowing dye (transparent: the background shows) */
.splash { background: #000; background-image: effect(fluid, preset calm-water, splat-radius 0.4); }
```

- **`fluid`** is Pavel Dobryakov's WebGL-Fluid-Simulation with his parameters and defaults
  (`density-dissipation`, `velocity-dissipation`, `pressure`, `pressure-iterations`, `curl`,
  `splat-radius`, `splat-force`, `shading`, `colorful`, `color-speed`, `bloom*`, `sunrays*`) on the
  same kernels as the native Fluid Simulation component (`jasper://docs/fluid`). It starts with a
  seeded burst (`splats-on-start`, `splat-count`, `seed`), paints while the pointer drags over it
  (`hover on`: on any movement), and is transparent where there is no dye (alpha = brightness):
  the element's `background` is Pavel's background colour. `preset` (`mobile`, `smoke`, `ink`,
  `calm-water`, `psychedelic`: the native presets) gives every parameter you do not write the
  preset's value; a parameter you write keeps yours, even at its default (`preset ink, bloom on`
  has bloom), as on the native component. Write `preset` literally (a `var()` preset is not
  expanded). Its grids keep Pavel's sizes whatever the element's: 128 cells on the short side for the
  velocity, the element's px for the dye (at most 1024), 256 for the bloom; `bloom off` and
  `sunrays off` skip their passes (`preset mobile` turns both off with the shading).

- **Each element has its own simulation**, kept in GPU images (`inspect_ui` and `getEffects`
  report their size). It starts when the element is first drawn and steps at a fixed 60 Hz,
  whatever the frame rate (at most 4 steps a frame: a long frame slows it down instead of
  stalling). `time-scale` scales the steps, `0` freezes it.
- **It pauses** while the element is not drawn (`display: none`, `visibility: hidden`, scrolled
  or clipped out of view) and resumes where it was, without catching up. Removing the element,
  or changing it to another effect, ends it; a change of a parameter that seeds it (`seed`,
  `preset`: marked in the catalogue) restarts it. A resize keeps it going.
- **Pinned time** (`setEffectTime`, `set_ui_effect_time`): the state at a pinned time is computed
  from scratch (the last 2 s simulated, less for a few fluids, with the pointer's recent path), so two captures at the
  same time are identical whatever happened before. That takes a few frames, during which the
  effect is not drawn (`screenshot` waits for it).
- **Input from scripts**: `el:effectSplat{ x = 120, y = 80, dx = 300, dy = 0, radius = 30,
  color = "#ff8a00" }` pushes dye and velocity into the element's simulations at their next step
  (x, y and radius in the element's CSS px, dx and dy px/s; `effect = "ink-flow"` targets one);
  `el:effectReset()` restarts them. Both return how many simulations took it.

```lua
local pond = doc:querySelector(".pond")   -- .pond { background-image: effect(ink-flow) }
pond:addEventListener("click", function(e)
  local box = pond:getBoundingClientRect()
  pond:effectSplat{ x = e.clientX - box.left, y = e.clientY - box.top, dy = -400, radius = 40, color = "#33aaff" }
end)
for _, fx in ipairs(pond:getEffects()) do print(fx.name, fx.stateful, fx.steps, fx.stateBytes) end
```

- **Cost**: they are the costliest effects (a fluid runs about 20 passes a step): use two or three
  on screen, prefer the fixed-grid ones (fluids, `reaction-diffusion`, `time-trail`) on large
  elements, lower `count` for the particle effects on mobile, and hide an element whose
  simulation should stop (covering it is not enough). A document with more than 8 running at once,
  or 64 MB of their images, gets `HTMLUI-FX-0018`.

### Writing an effect

A project effect is one fragment file with a manifest, declared in CSS with `@effect`. Its
`src` resolves like `url()`, relative to the stylesheet; the name must be lowercase kebab-case,
must not be a built-in effect, and must equal the manifest's `@effect`.

```css
@effect my-glow { src: url("fx/my-glow.frag"); }
.badge { background-image: effect(my-glow, intensity 0.7); border-radius: 12px; }
```

`assets/ui/fx/my-glow.frag`, a complete paint effect:

```text
#version 450
// @effect     my-glow
// @kind       paint
// @category   light
// @summary    A soft glow from the centre of the element, pulsing
// @source     original
// @cost       low
// @time       speed
// @param      color color #59a6ff -- Glow colour
// @param      intensity number 0.6 0 1 -- Strength of the glow
// @param      speed number 1 0 5 -- Pulses per second

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float intensity;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 p = (c.uv - 0.5) * 2.0;
    float pulse = 0.75 + 0.25 * sin(c.anim.x * 6.2831853);
    float glow = exp(-dot(p, p) * 2.5) * params.intensity * pulse;
    return vec4(params.color.rgb, params.color.a * glow);
}

#include "effects/lib/main_paint.glsl"
```

- **Manifest**: comment lines right after `#version 450`. Required: `@effect`, `@kind`
  (`paint` or `filter`), `@category`, `@summary`, `@source`, `@cost` (`low`, `medium`,
  `high`). Optional: `@time` (up to 4 rate parameters, integrated into `c.anim` so a rate
  change never jumps), `@pointer` (`position`, `trail`, `ripples`), `@backdrop yes`,
  `@prepass blur <param>` and `@margin <param>*3` (filters that read around a pixel),
  `@suggest-for <words>`. Each parameter is `// @param <name> <type> <default> [<min> <max>] -- <description>`;
  an enum lists its options as `a|b|c`; a default with spaces is quoted.
- **Uniforms**: `u_fs_effectParams` has one member per parameter, named in camelCase
  (`light-angle` is `lightAngle`), `vec4` for colours, `vec2` for positions, `float` for the
  rest (lengths in CSS px, angles in radians, times in seconds, enums as their index). A
  palette `p` is `vec4 p[8]; vec4 pStops[2]; float pCount;`. Nothing else goes in the block.
- **The function**: a paint effect defines `vec4 jfx_paint(JfxContext c)` and ends with
  `#include "effects/lib/main_paint.glsl"`; a filter effect defines `vec4 jfx_filter(JfxContext c)`
  and ends with `#include "effects/lib/main_filter.glsl"`, reading the content with
  `jfx_src(localPx)` (`effects/lib/jfx_sample.glsl`). Return straight-alpha, linear sRGB
  (colours from parameters already are); clipping to the rounded area, opacity and blending
  are done for you.
- **The context** `c`: `uv` (0 to 1 over the area, y down), `px` and `size` (CSS px), `aspect`,
  `time`, `anim`, `dt`, `dpr`, the pointer (`pointer`, `pointerSmooth`, `pointerInside`,
  `pointerPresence`, `pointerDown`, `pointerVelocity`, `sincePress`, `sinceEnter`), `radii`
  and a per-element `seed`.
- **Includes** resolve against the file's folder, the project and the engine library:
  `effects/lib/jfx_color.glsl` (sRGB, OKLab/OKLCH/HSL mixing, palettes),
  `effects/lib/jfx_noise.glsl` (value, Perlin, simplex, Worley, fbm, curl, domain warp),
  `effects/lib/jfx_hash.glsl`, `effects/lib/jfx_sdf.glsl` (shapes), `effects/lib/jfx_geom.glsl`,
  `effects/lib/jfx_blend.glsl`, `effects/lib/jfx_tone.glsl`.
- **Stateful effects** declare their images and passes in the manifest (`@cost stateful`):
  `// @state <name> rgba8|rgba16f|rg16f|r16f|rgba32f|r32f <w>x<h>|box[*scale] [max <px>]|aspect <short> [max <px>] [linear|nearest]`
  (`rg16f`/`r16f`: half-float two- and one-channel fields, sampled as (r, g, 0, 1) and (r, 0, 0, 1);
  the 32-bit formats must be `nearest`; `aspect 128`: the element's aspect with a 128-texel short
  side at any size, square cells),
  `// @init <pass> <state>` (run once at the start), `// @step <pass> <state> [x<count>|x<param>]
  [add] [scatter <placement>] [if <param>]` (each step, in order; `if`: only while that boolean or
  number parameter is not 0), `// @frame <pass> <state> ...` (same options: once a drawn frame
  after the steps, when they changed the state: a bloom chain or light shafts only the draw
  reads), optional `@warmup <steps> <dt>`, `@rate <hz> max <substeps>`, `@replay <s>`,
  `@reseed <param>` and `@preset <option> <param> <value>, ...` (the options of an enum parameter
  named `preset`; the values its parameters left at their defaults take, as `fluid`'s). Declare each image with
  `JFX_STATE(<name>)` (from `effects/lib/jfx_state.glsl`: `<name>_fetch(ivec2)`,
  `<name>_sample(uv)`, `<name>_size()`), define `vec4 jfx_step(JfxStep s)` (it writes texel
  `s.cell` of pass `s.pass`, compared with `JFX_PASS_<NAME>`, inside `#ifdef JFX_STEP_FORM`),
  read the images in the paint or filter function, and end with
  `#include "effects/lib/main_step.glsl"` after the main include. The built-in simulations in
  `engine/shaderlib/effects` (`fluid-smoke.frag`, `reaction-diffusion.frag`, `boids.frag`) are
  the examples; `jasper://docs/html-ui-effects` lists their images.
- Saving the `.frag` reloads the documents that declared it. `check --ui` reports the CSS
  side, `jasperengine-editor check` compiles the effect and reports `file:line` errors, and
  `jasperengine-editor ui-effects --validate --project <dir>` checks the manifest against the
  compiled shader.

### Effects in the editor and MCP

- **Finding one**: `list_ui_effects` lists the effects (name, kind, category, cost, summary, an
  example); `name` gives one effect with every parameter (type, default, range), `category` and
  `kind` filter. The project's `.frag` effects are in it too (`declared` says whether a document
  declares one with `@effect`).
- **What an element draws**: `inspect_ui` lists each element's effects under `effects` (the
  property, the layer index, the form and every parameter's value as the engine reads it,
  universal ones included; a stateful effect adds `state`: its images' bytes, its steps, its
  simulation time, whether it is paused, its image count, the pinned-clock replays run (`replaying`
  while one spans frames), the splats queued or logged and the scripted path's samples) and marks
  them `fx:<name>` in the outline. `inspect_ui {file}` and
  `jasperengine-editor check --ui` report the codes of "Effect diagnostics" with `line:col`.
- **Stable images**: effects animate with time, so two screenshots differ. `set_ui_effect_time
  {time: 1.5}` pins the effect clock in the edit and play views (renders are then identical, the
  pointer aside; a stateful effect's state is recomputed for the pinned time); `{time: null}`
  makes it live again. In play, `eval_lua` drives a simulation with `el:effectSplat{...}`. Scripts do the same with
  `UIDocument.setEffectTime(1.5)` and `UIDocument.setEffectTime(nil)`.
- **Scripted input, exactly replayed** (stateful effects, edit or play, no Lua): with a pinned
  `time`, `set_ui_effect_time {time: 1, selector: ".canvas", pointer_path: [[40, 120, 0.2],
  [200, 140, 0.5], [360, 90, 0.8]]}` drags the pointer along the path (x, y in the element's CSS
  px, t in effect seconds: down from the first point's time to the last's) in every replay of the
  element's simulations, and `splats: [{x, y, dx, dy, radius, color, strength, t}]` (the area's
  px, px/s; `t` defaults to the pinned time) are applied at the step their time falls in (at most
  8 in one step: more are refused; a splat later than the pinned time waits until the time
  reaches it). Both stay until replaced (`[]` clears them; new splats replace the earlier ones),
  so the same call twice gives the same screenshot, and other times show the same input's
  evolution (`entity` names the document). The replay reaches back to the earliest scripted input
  (at most 30 s before the pinned time; older input is reported and ignored). Velocities are
  clamped to 100000 px/s, strengths to 100. A simulation starts at its element's first draw: take
  a screenshot first if the call says there is none.
- **Project effects**: `write_file` on an effect's `.frag` compiles it in each of its forms and
  returns the errors at their `.frag` line (`HTMLUI-FX-0015`; a manifest error is
  `HTMLUI-FX-0014` at its line). An edit on disk comes back the same way in `external_changes`,
  and the documents that declare the effect reload, also while playing; while the effect does not
  compile it is not drawn. `check_shader {shader: "assets/ui/fx/my-glow.frag"}` and
  `jasperengine-editor check` check it on demand; a renamed or deleted `.frag` is
  `HTMLUI-FX-0013` until the `@effect` follows.
- **Exports** ship the variants the documents use, read from the documents (their stylesheets,
  `@media` blocks included, and style attributes) and from the project's Lua scripts: the paint
  form for `background-image`/`mask-image`, the filter form for `filter`, `backdrop-filter` and
  gradient text, both forms of a paint effect a script names. An effect only reachable another
  way (a name a script builds at runtime) is added in Build Settings (Add Shader, type UI Effect)
  or with `build_project {shaders: {add: ["uieffect/my-glow", "uieffect/my-glow:Flt"]}}`; the
  game compiles nothing. A build with effects carries `licenses/shaders.com-LICENSE.txt` (the
  MIT notice of the effects ported from shaders.com).

### Effect performance

- Low-cost effects: use freely. Medium: up to about half the screen. High: at most two elements
  or a quarter of the screen (more than four high-cost effects in a document give an info).
- Each element with an effect costs one draw (paint) or one pass over its region (filter); a
  blurring effect adds a few passes. Shaders and pipelines are shared: nothing compiles per
  element, per frame or per parameter change.
- `backdrop-filter` effects pay one extra copy of what is below; blurring effects are the
  costliest per pixel: the prepass ones (`frosted-glass`, `liquid-glass` with `blur`,
  `tilt-shift`, `progressive-blur`, `glow`) and the 32-tap streaks (`motion-blur`,
  `zoom-blur`, `angular-blur`). Keep them on panels, not full screen.
- Prefer time-driven parameters (`speed`) to animating a parameter every frame on many elements.
- Stateful effects (simulations): two or three on screen; see "Stateful effects" above.

### Effect diagnostics

| Code | Severity | When |
|---|---|---|
| `HTMLUI-FX-0001` | error | unknown effect name (the message suggests the nearest names, e.g. `glassmorphism` gives `frosted-glass`); the layer or filter is dropped |
| `HTMLUI-FX-0002` | error | a paint effect where only filters go (or the reverse), or `effect()` in another property |
| `HTMLUI-FX-0003` | error | an argument without a parameter name |
| `HTMLUI-FX-0004` | warning | unknown parameter (`speeed` suggests `speed`); ignored |
| `HTMLUI-FX-0005` | warning | a value of the wrong type; the default is used |
| `HTMLUI-FX-0006` | warning | a value out of range; clamped |
| `HTMLUI-FX-0007` | warning | a parameter given twice; the last wins |
| `HTMLUI-FX-0008` | warning | `blend` on a filter effect |
| `HTMLUI-FX-0009` | warning | `background-size`/`-position`/`-repeat` on an effect layer |
| `HTMLUI-FX-0010` | warning | a `background-blend-mode` effect layers cannot use |
| `HTMLUI-FX-0011` | info | gradient text: the text colour was set to white |
| `HTMLUI-FX-0012` | error | `@effect` with a built-in name, or a name declared with another `src` |
| `HTMLUI-FX-0013` | error | the `@effect` file is missing |
| `HTMLUI-FX-0014` | error | the effect's manifest is invalid (reported at the `.frag` line) |
| `HTMLUI-FX-0015` | error | the project effect does not compile (reported at the `.frag` line) |
| `HTMLUI-FX-0016` | info | more than four high-cost effects (or four stateful ones) in one document |
| `HTMLUI-FX-0017` | error | a stateful effect's images use a format the device cannot render (or blend) to; it is not drawn |
| `HTMLUI-FX-0018` | warning, error | more than 8 stateful effects running in a document, 32 in the game or 64 MB of their images (warning); past 64 simulations no more start (error) |

### Effects in a browser

Browsers do not know `effect()` and `@effect`: they drop those declarations and keep the rest.
Put a plain value first and the effect after it, so the same file previews in a browser:

```css
.hero { background: #0b0d14; background-image: effect(stripes, color-a #141a2a, color-b #0b0d14, density 8); }
```
