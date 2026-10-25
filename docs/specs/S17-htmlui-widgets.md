# S17 — HTML UI widgets: virtualized views, tooltips, typed drag-and-drop, Markdown, live tree patch

Wave 2. Depends on S01 (C++20), S02 (gtest/CI), S05 (reload trigger). Uses S15 (UI timings) and
S16 (UI draw cost) when present. Feeds the example library and the MCP agent guide.

## 1. Goal

Game UIs built from HTML + Lua get the data-heavy widgets that inventories, server browsers, chat
logs, quest logs and leaderboards need. These are virtualized list/grid/tree/table views that keep
a 10k-row list at 60 fps on mobile, real `title` and rich tooltips, HTML5-style drag-and-drop with
typed `dataTransfer` payloads, and a `<markdown>` element. All of them are plain elements usable
from HTML, Lua and data binding. Hot reload also changes: it patches the live tree in place, so
Lua listeners, element handles, runtime-added nodes, animations and form state survive an edit.
Today every reload is a snapshot and rebuild.

## 2. Scope

**In scope**
- `<list-view>`, `<grid-view>`, `<tree-view>`, `<table-view>` (+ `<table-column>`). Each has pooled
  rows, uniform or variable heights (Fenwick prefix sums), overscan, shrink hysteresis, selection,
  keyboard and gamepad navigation, stick-to-end, and two data sources: a data-model array or a Lua
  provider.
- Tooltips: an emulated `title` attribute, rich tooltips (`tooltip="#id"` or Lua), delay/hop/hide
  rules, flip/clamp placement and an optional arrow.
- Drag-and-drop: `draggable`, HTML5 event names (`dragstart`, `drag`, `dragenter`, `dragover`,
  `dragleave`, `drop`, `dragend`), `event.dataTransfer` (typed Lua payloads, `effectAllowed`/
  `dropEffect`, `setDragImage`), the declarative `dropzone`, can-drop classes, drag ghosts, Escape/B
  cancel, auto-scroll inside scroll containers and virtual views, and a programmatic drag API.
- `<markdown>` (inline or `src=`), `data-markdown` binding, `el:setMarkdown()`. The parser is
  ported from OpenEngine and emits HTML/RML styled by the UA sheet.
- Live tree patch hot reload for compiled `.html` documents (3-way reconcile).
- Compat tables, diagnostics, the support matrix, user docs, the example library, goldens, unit
  tests, MCP `inspect_ui`/`assert_ui` coverage, and the Lua API.

**Out of scope**
- Their Yoga/CSS UI core, docking and editor toolkit (README do-not-copy). We stay on our RmlUi
  fork + lexbor + Lua DOM.
- ChartView, SplitView, Dropdown/EnumField controls, context menus.
- Built-in gamepad "pick up / put down" drag (scriptable through the programmatic API instead),
  OS drag-and-drop from outside the game window, accessibility trees, localization.
- Patch reload of `.rml` documents. They keep today's snapshot path.
- The editor's own ImGui UI.

## 3. Current state

- **No virtualization.** RmlUi's `DataViewFor::Update` instantiates one element per array entry
  and keeps them all (`engine/libs/rmlui/Source/Core/DataViewDefault.cpp:534-578`). The aliases
  `item`/`i` are bound once per element with `DataModel::InsertAlias`
  (`Source/Core/DataModel.cpp:183-203`). Views resolve their addresses at initialisation, so a
  pooled row cannot be re-pointed. Every layout dirty, including text changes inside a scroll
  box, re-lays out the whole document (`Element::DirtyLayout` → owner document,
  `Source/Core/Element.cpp:2584-2588`). CSS `contain` is accepted and ignored
  (`engine/core/htmlui/compat/tables/PropertyTable.cpp:274`). The examples use `data-for` for
  every grid (`docs/mcp/guide/html-ui-examples/inventory/inventory.html:40-45`,
  `shop/shop.html:38`).
- **Tooltips dropped.** The element table says `title` → "no tooltips; dropped (Info)"
  (`compat/tables/ElementTable.cpp:231`). The transform reports `HTMLUI-HTML-0015 title-tooltip`
  (`compat/html/transforms/AttributeTransform.cpp:359-361`, `DiagnosticCodes.h:80`) but leaves the
  attribute on the element, so a runtime can read it. The inventory example hand-rolls a hover
  panel.
- **Drag has no data.** `draggable="true"` becomes `drag: drag` with an Info diagnostic "there is
  no HTML5 drop data" (`AttributeTransform.cpp:363-369`). RmlUi fires
  `dragstart/drag/dragend/dragover/dragout/dragmove/dragdrop`
  (`Source/Core/EventSpecification.cpp:30-37`). Here `dragover` means *enter*, and
  `dragover/dragdrop` fire only with `drag: drag-drop|clone` (`Source/Core/Context.cpp:694-704`).
  Lua sees the RmlUi names (`engine/core/object/ui/HtmlUIEventListener.cpp:27-31`). `UIEvent` has
  no payload (`engine/core/object/ui/UIEvent.h`). Scripts keep the source index in Lua state by
  hand (`inventory/InventoryUI.lua:59-76`). A ghost exists only as RmlUi's `drag: clone`
  (`Context.cpp:1332`).
- **No Markdown.**
- **Hot reload rebuilds.** `UIDocumentSystem::reloadDocument`
  (`engine/core/subsystem/UIDocumentSystemReload.cpp:452-532`) loads a new document, then
  restores scroll/value/checked/selection/focus by id (snapshot code at lines 93-240). It moves
  listeners of id'd elements and drops the rest with a warning
  (`UIDocumentSystemScript.cpp:281-319`). It removes all `<script type="text/lua">` listeners and
  re-runs every script (`UIDocumentSystemCompat.cpp:217-240`), and fires `onUnload` then `onLoad`.
  Classes, attributes and nodes added by Lua are lost, along with handles, anime animations and
  listeners on elements without an id. This is documented as such in
  `docs/mcp/guide/html-ui.md:645-668`. Every compiled element carries `data-htmlui-loc`, a
  pre-order index into the compile's source map (`compat/DocumentLoader.cpp:197-207,293-298`),
  and the compiled tree is kept per document (`DocumentLoader::compiledOf`).
- **Agents.** `inspect_ui`/`assert_ui` (`editor/mcp/tools/HtmlUiTools.cpp:1770,1803`) and
  `send_input` (`editor/mcp/tools/PlayTools.cpp`) know nothing about widget state. The example
  library has 10 examples + blank, embedded from `docs/mcp/guide/html-ui-examples` (manifest
  `examples.json`, `CMakeLists.txt:369`, `editor/util/HtmlUiExamples.h`).
- **Gamepad.** Directions reach a focused control only for `range` and open `select`. Everything
  else uses spatial nav (`engine/core/subsystem/UIDocumentSystemInput.cpp:177-190`).

## 4. Reference (OpenEngine, MIT; `OE` = `external/OpenEngine/Engine/Modules/UI`)

| What | Files | Take | Improve |
|---|---|---|---|
| Window core | `OE/Include/UI/Controls/VirtualWindowCore.h` (147), `OE/Source/Controls/VirtualWindowCore.cpp` (241), `OE/Include/UI/VirtualizationCoordinator.h` | **Copy** (keep header): guard compare, ring-slot mapping, grow-only pool, changeset subset fast path, safety-net validation, shrink hysteresis (`kShrinkSlackLines=8`, `kShrinkStableUpdates=45`), `allowShrink` outside event dispatch, `FirstFromPrefixSum` | Variable heights through a Fenwick tree (O(log n) update) instead of their O(n) `m_CumulativeYPx` rebuild (`ListView.cpp:526-548`) |
| Views | `OE/Source/Controls/{ListView,GridView,TreeView,TableView}.cpp`, `OE/Include/UI/Controls/ListView.h` | Behaviour only: overscan 4 (`ListView.h:325`), PageUp/Down/Home/End through the prefix sums (`ListView.cpp:1486-1500`), shift/ctrl selection, pending scroll-to-end, tree flattening with a structure generation | Written against RmlUi + Lua. Not lifted |
| Tooltip | `OE/Source/Interaction/TooltipOverlay.cpp` (549), `OE/Include/UI/Interaction/TooltipOverlay.h:95-96` | Rules: 0.5 s hover delay, 0.5 s hop window, source-element identity (moving onto a child does not restart), park-offscreen measure frame, hide on leave/press | Rich content, keyboard/gamepad focus trigger, per-element delay |
| Placement | `OE/Source/Layout/DropdownPopupPlacement.cpp` (98) | Logic: preferred side, flip when the room is smaller and the other side has more, clamp with an edge clearance, scale-aware | Pure function, four sides, arrow offset |
| Drag-drop | `OE/Source/Interaction/DragDropManager.cpp`, `OE/Include/UI/Interaction/{Payload,DropTarget,Types,DragDropGhostRendererRegistry}.h` | Model: typed payload, `AcceptsPayload` prefilter → `CanDrop` feedback (`Allowed`, `Reason`, `CssClass`, `SuppressGhost`) → `PerformDrop`; `DropLocation` before/after/onto; end-of-drag callback; auto-scroll capability | Exposed as HTML5 events + `dataTransfer` in Lua |
| Markdown | `OE/Source/Parsers/{MarkdownParser.h,MarkdownParser.cpp (512),MarkdownInlineParser.cpp (686),MarkdownDocument.h,MarkdownSyntax.h}`, tests `OE/Tests/MarkdownParserTests.cpp` | **Copy** (C++20 as is): CommonMark-ish blocks, flanking rules, streaming-safe unclosed fences | Nested lists at any depth (theirs caps at 1, `MarkdownParser.cpp` `kMaxListDepth`), images as `<img>`, HTML emitter |
| Hot reload | `OE/Source/UIHotReload.cpp:157-453` (`ReconcileClasses`, `ReconcileChildren`, `ReconcileElement`), tests `OE/Tests/UIHotReloadTests.cpp` | Matching: id → same type + superset of template classes → same type. Keep runtime-added children. Diff classes against the previously applied template | 3-way merge for attributes, inline style and text, not just classes; data-binding-aware replacement |
| Tests | `OE/Tests/{VirtualWindowCoreTests,UIScrollVirtualizationTests,ListViewTests,TreeViewTests,TableViewTests,GridViewTests,TooltipPlacementTests}.cpp` | Case lists to port as gtests | — |

Licence: MIT. Copied files keep the OpenEngine copyright header. `NOTICE.md` gets one entry
covering VirtualWindowCore and the Markdown parser. Ported logic (placement, tooltip rules,
reconcile matching) is credited in the same entry. Nothing is executed from their tree.

## 5. Requirements

### Functional — virtualized views

- **R1** `<list-view>`, `<grid-view>`, `<tree-view>`, `<table-view>` are registered element
  instancers (`engine/core/htmlui/widgets/`). They are usable in `.html` (compat table rows) and
  `.rml`, and work in SCREEN and TEXTURE documents. The row markup is the single `<template>`
  child. A view with zero or several templates is `HTMLUI-HTML-0030` (Error).
- **R2** Only rows that intersect the viewport ± `overscan` (attribute, default 4 rows; grid:
  lines) exist as elements. Pool size ≤ `ceil(viewportHeight / minRowHeight) + 2·overscan + 8`
  (slack). Shrink follows the core's hysteresis and never runs during event dispatch.
- **R3** Heights: `item-height="48"` (uniform, O(1) index↔offset) or `item-height="auto"`, which
  measures bound rows after layout. Unmeasured rows use `estimated-height` (default: the first
  measured row). Offsets live in a Fenwick tree. When a height above the first visible row
  changes, the first visible row keeps its on-screen position (scroll anchoring).
- **R4** Data source A, the data model: inside a `data-model`,
  `<list-view items="inventory" as="item, i">` binds rows to `inventory[k]`. The template uses
  normal bindings (`{{item.name}}`, `data-attr-*`, `data-class-*`, `data-event-click="pick(i)"`).
  `i` is 0-based, as in `data-for`. Dirtying `inventory` updates the count and re-evaluates the
  bound rows only. Per-entry `model:set("inventory[3].name", v)` updates one row.
- **R5** Data source B, the Lua provider: `view:setProvider{ count=n, key=fn(i)?, height=fn(i)?,
  bind=fn(rowEl, i), unbind=fn(rowEl, i)? }` with 1-based `i`. Also `view:setCount(n)`,
  `view:refresh()` (all) and `view:refreshItems{i,...}` (subset). It maps to the core's
  `ChangeSetView::Subset/All`.
- **R6** Pooled rows get the class `htmlui-row`, `aria-rowindex` (1-based), `selected`/
  `aria-selected`. On bind, rows get `odd`/`even` classes (`:nth-child` is meaningless on a ring),
  which is documented. A row element keeps its identity while it shows the same key, so
  listeners and anime animations on a row are not reset by scrolling within the window.
- **R7** Selection: `selection="none|single|multiple"` (default single). Click selects,
  Ctrl/Cmd+click toggles, Shift+click and Shift+arrows extend, Ctrl+A selects all (multiple).
  Selection is stored by key (provider `key` or model index) and survives scrolling and
  recycling. Events: `select` (detail `{indices, keys}`), `activate` (Enter, gamepad A,
  double-click; detail `{index, key}`).
- **R8** Keyboard: the view is one Tab stop. Up/Down (grid: also Left/Right),
  PageUp/PageDown (one viewport), Home/End move the *active* item, scroll it into view
  (`nearest`) and focus its row. If the active row is recycled while focused, focus moves to
  the view and returns when the row is visible again. Gamepad: the d-pad/left stick drive the
  same keys while focus is inside a view. At an edge (first item + Up, etc.) the view does not
  consume the key, so spatial navigation (`nav-*`) leaves the view
  (`UIDocumentSystemInput.cpp` `gamepadControlKey`).
- **R9** `<grid-view item-width item-height gap>`: lanes = `max(1, floor((width+gap)/(item-width+gap)))`,
  recomputed on resize without blank or ghost cells (the core's `SlotCount()` contract).
- **R10** `<tree-view>`: in model mode, the `children-key` (default `children`) and `key`
  (default index path) attributes drive it. The view flattens expanded nodes, so the cost is
  O(visible structure) per structural change. The template gets the extra aliases `depth`,
  `expanded` and `hasChildren`. Right expands or moves to the first child, Left collapses or
  moves to the parent. Events `expand`/`collapse` fire, and Lua has `expand/collapse/toggle(key)`.
- **R11** `<table-view>` with `<table-column key width min-width sortable resizable>` children
  (header content inline, optional per-column `<template>` for the cell, default
  `{{item.<key>}}`). The header is sticky and synced horizontally. Dragging a 6 px header edge
  resizes the column and fires `columnresize`. A header click on a sortable column fires
  `sort` `{key, direction}` and sets `sorted-asc`/`sorted-desc`. The author sorts the data.
- **R12** `anchor="end"`: while scrolled to the end, appended items keep the view at the end
  (chat logs). `view:scrollToIndex(i, "start|center|end|nearest")`, `view:getVisibleRange()`.
- **R13** Runtime diagnostic `HTMLUI-HTML-0031 large-data-for` (Info, once per element) is
  raised when a `data-for` produces more than 500 elements. It suggests `<list-view>`. A view
  without a definite height gets `HTMLUI-HTML-0029 virtual-view-height` (Warning), and the view
  then lays out 20 rows.

### Functional — tooltips

- **R14** `title` is emulated (ElementTable row → "emulated"; `HTMLUI-HTML-0015` is no longer
  reported, and its registry row is kept with updated help). Hovering the element shows the
  text after the delay. It is placed 16 px below the pointer, flipped above when there is no
  room below, and clamped to the document viewport with a 4 px edge pad.
- **R15** A rich tooltip is set with `tooltip="#tip-id"` (an element or `<template>` in the same
  document, cloned on show) or with Lua `el:setTooltip(textOrElementOrFn, opts)`. A function is
  evaluated on each show. `opts`: `placement` (`top|bottom|left|right|auto`, default `top`),
  `delay`, `arrow` (bool), `interactive` (the pointer may enter the tooltip without hiding it).
  Placement is anchored to the element's border box with flip + clamp (§6.3).
- **R16** Rules, timed on `HtmlUIClock`: show delay 0.5 s (overridable per element with
  `tooltip-delay`, and per document with `UIDocument.tooltip.configure{delay, hopWindow}`). Hop:
  once a tooltip is shown, moving to another tooltip source within 0.5 s shows immediately. The
  source identity is the nearest ancestor with `title`/`tooltip`. The tooltip hides on pointer
  leave, mousedown/touch, wheel/scroll of an ancestor, keydown (except modifiers), document
  hide/unload, element removal and drag start. Keyboard focus (`:focus-visible`) and gamepad
  focus show the tooltip of the focused element after the delay and hide it on blur.
- **R17** The tooltip element (`div.htmlui-tooltip[role=tooltip]`, with optional
  `.htmlui-tooltip-arrow`) is appended to the source's own document body with `position: fixed`,
  a top z-index and `pointer-events: none` (unless `interactive`), so author CSS styles it. The
  UA sheet gives it a Chromium-like default. Events `tooltipshow`/`tooltiphide` fire on the
  source. One tooltip per document is visible at a time.

### Functional — drag-and-drop

- **R18** `draggable="true"` compiles to `drag: drag-drop` (no Info diagnostic). `drag="clone"`
  keeps its RmlUi meaning. New HTML5 event names, synthesized from RmlUi's: `dragenter`
  (RmlUi `dragover`), `dragleave` (`dragout`), `dragover` (HTML5 semantics: on enter and on every
  pointer move over the target, at most once per frame), `drop` (`dragdrop`). `dragstart`, `drag`
  and `dragend` keep their names, and `dragmove`/`dragdrop`/`dragout` stay available. The one
  behaviour change, `dragover` now firing repeatedly, is documented in the changelog and in
  `html-ui.md`.
- **R19** `event.dataTransfer` (a `UIDataTransfer` handle) is valid from `dragstart` to `dragend`:
  `setData(type, value)` (type: a string such as `"item"` or `"text/plain"`; value: any Lua
  value, including tables, held by reference until `dragend`), `getData(type)`, `types`,
  `clearData(type?)`, `effectAllowed` (`none|copy|move|link|copyMove|copyLink|linkMove|all`),
  `dropEffect`, `setDragImage(elementOrNil, offsetX, offsetY)`. Unlike HTML5, `getData` also works
  in `dragenter`/`dragover`, so targets can validate by content. This deviation is documented.
- **R20** A target accepts the drop if a `dragover` listener calls `preventDefault()` or its
  `dropzone="item weapon"` lists one of `types` (optionally with `copy|move|link`). Feedback
  classes are `htmlui-drop-allowed`/`htmlui-drop-denied` on the hovered target, `htmlui-drag-source`
  on the source, and `htmlui-dragging` on the document body. `drop` fires only on an accepting
  target. `dragend` reports the final `dropEffect` (`none` when cancelled or not accepted).
- **R21** Ghost: by default a clone of the source at 0.7 opacity
  (`.htmlui-drag-ghost`, styleable). `setDragImage(el, x, y)` uses that element, and
  `setDragImage(nil)` disables the ghost. Escape or gamepad B cancels: `dragleave` on the
  target, then `dragend` with `none`. Hovering within 32 px of a scroll container's or virtual
  view's edge auto-scrolls at up to 600 px/s.
- **R22** Inside virtual views a drag carries the row's index/key (`dataTransfer:getData
  ("htmlui/row")` = `{view=el, index=i, key=k}`). The target view exposes `event.dropIndex` and
  `event.dropPosition` (`before|after|on`, from the row's top/bottom 25 % bands) and draws a
  `.htmlui-drop-indicator`. Dragging a recycled source row does not cancel the drag.
- **R23** A programmatic drag for gamepad/scripted flows: `UIDrag.begin(sourceEl, {data=...,
  effectAllowed=...})`, `UIDrag.over(targetEl)`, `UIDrag.drop(targetEl)` and `UIDrag.cancel()`,
  which fire the same events in the same order as a pointer drag.

### Functional — Markdown

- **R24** `<markdown>` with `src="notes.md"` (asset path rules, tracked as a reload dependency)
  or inline text. The inline source is the raw bytes between the start tag and `</markdown`,
  dedented by the common leading whitespace. The HTML parse of the content is discarded, so
  `<https://x>` and `*` survive. The output is `h1-h6, p, ul/ol/li` (any depth), `pre>code[data-lang]`,
  `blockquote, hr, table/thead/tbody/tr/th/td, a[href], strong, em, code, img, br`, wrapped in
  `div.markdown-body`, with UA styles.
- **R25** Raw HTML inside Markdown is shown as text, never interpreted. Text from servers or
  players is therefore safe. The compile reports `HTMLUI-HTML-0032 markdown-raw-html` (Info) for
  inline sources.
- **R26** Runtime: `el:setMarkdown(text)`, `UIDocument.markdownToHtml(text)` (string), and the data
  binding `data-markdown="expr"`, a registered data view added to the `isBindingAttribute` list
  (`AttributeTransform.cpp:53-65`). Re-rendering 20 KB of Markdown takes ≤ 2 ms desktop.
  Unclosed fences render as code (streaming).

### Functional — hot reload (live tree patch)

- **R27** For compiled `.html` documents, a document change reconciles the live DOM against the
  new compile. The previous compiled tree (kept per document) is the common ancestor. Live
  authored elements map to old nodes through `data-htmlui-loc` plus an *authored set* recorded at
  load: an `Rml::Plugin` `OnElementCreate/Destroy` tracker excludes data-view clones and
  `cloneNode` copies. Old and new trees are matched per OE (id → tag + class superset → tag, in
  sibling order).
- **R28** Merge rules: an attribute, class token, inline-style property or text node takes the
  new authored value **if the authored value changed**, else the live value stays (Lua edits
  survive). Nodes not in the authored set (runtime-added) stay, after the same authored
  predecessor. Removed authored nodes are removed. New nodes are instanced through RmlUi's parser
  under their live parent, so data views bind. Kept elements keep identity, listeners, Lua
  handles, form values, scroll, focus and anime animations.
- **R29** An element whose data-binding attributes (`data-model`, `data-for`, `data-if`, `{{ }}`
  text, `data-*` from `isBindingAttribute`) changed is replaced as a subtree. Its id'd
  descendants' listeners move as they do today. The cause is listed in the reload report.
- **R30** `<script type="text/lua">` blocks whose text is unchanged do not re-run. A changed or
  new script has its scope's listeners removed and re-runs, and a deleted script has its
  listeners removed. The component's `onLoad` does **not** fire on a patch. A new
  `doc.onReload(report)` fires instead. A rebuild still fires `onUnload`/`onLoad`.
- **R31** Every patch ends with a restyle (`compatRestyle`), because inline-style rules are keyed
  by the renumbered `data-htmlui-loc` (`CssRewriter.cpp:2222`). It writes a
  `HtmlUIReloadReport{mode, kept, updated, inserted, removed, replaced, runtimeKept,
  scriptsRerun, fallbackReason, ms}` to the document's messages. If the new file does not
  compile to a document, the live UI stays and the diagnostics are reported (a missing file
  still unloads, as today). `doc:reload{mode="rebuild"}` and an editor preference force today's path.

### Platform & backends

- **R32** Everything is CPU/RmlUi-level, with no new shaders. All five backends and all six
  targets work. TEXTURE documents get tooltips, drags (UV hit testing,
  `engine/core/htmlui/UIWorldRaycast.cpp`) and views. Touch drags a view's content
  (scroll) unless the row is `draggable`. On a draggable row, a 400 ms long-press starts the drag.

### Performance (budgets)

"Mobile tier" means a Snapdragon 778G-class Android device (e.g. Galaxy A54), GLES3, 1080×2400,
release build. The CI proxy is the Linux build server, release build, with budgets ×0.25.

- **R33** A `<list-view>` of 10,000 rows (48 px, 3 text spans + 1 image per row, model mode)
  flick-scrolled at 6,000 px/s for 10 s: HTML UI CPU time (context update + render record) is
  p95 ≤ 4.0 ms and max ≤ 8 ms per frame on mobile tier, which leaves 60 fps. In steady scroll
  there are zero element creations (the plugin counter). Draw calls are bounded by the pool, not
  the count. The same bench with `item-height="auto"` (heights 32–160 px) stays within p95 ≤ 5 ms.
- **R34** View overhead ≤ 24 bytes per item (heights + keys + selection bits) beyond the model
  data. Setting a 10k-entry, 5-field model from Lua takes ≤ 30 ms desktop. Flattening a 10k-node
  tree on expand takes ≤ 2 ms desktop.
- **R35** To make R33 reachable, a fork patch (P7-L, ledger in `docs/html-ui/FORK.md`) makes a
  virtual view with a definite size a **layout boundary**: a layout dirty inside it re-lays out
  only that subtree. A second patch (P7-D) adds *rebindable row aliases*: a `RowIndex` address
  entry resolved at evaluation, plus `DataModel::SetRowIndex(element, index)` that dirties only
  that row's views. Both are gated by `ForkFeatures`. Without them the views stay correct
  (full-document layout, re-instanced row content) and only the perf gate is waived.
- **R36** A patch reload of a 2,000-element document takes ≤ 15 ms desktop, excluding the compile.
  The idle cost of tooltips/drag is 0 when no source is hovered and no drag is active.

### Editor UX (human)

- **R37** Preferences › HTML UI: "Hot reload: Patch (keep runtime state) / Rebuild". The UI
  Document inspector's ⋮ menu has "Reload (Rebuild)". Each reload logs one Console line,
  `[HTML UI] Patched menu.html: 3 updated, 1 inserted, 12 runtime nodes kept (2.1 ms)`.
  Clicking the line opens the full report.
- **R38** The code editor's HTML suggestions (`editor/window/widget/SemanticSuggestions.cpp`)
  know the new tags and attributes. "Create › UI › UI Document from example" lists the new
  examples (R43) automatically through the manifest.

### Agent UX (MCP, Lua API, CLI)

- **R39** `inspect_ui`: each view reports `widget: {kind, count, first, last, pool, overscan,
  selection, active, scrollTop, totalHeight, source: "model:<name>"|"lua"}`. Tooltip state is
  `{visible, source ref, text, rect}`, drag state `{active, source, types, target, allowed,
  effect}`, and `reload: true` returns the last `HtmlUIReloadReport`.
- **R40** `assert_ui` adds the kinds `virtual_window` (`max_pool`, or the pool bound of R2 by
  default), `tooltip` (selector: the source; expects visible and `text` contains) and `selected`
  (`count`). Existing kinds work on rows and the tooltip element.
- **R41** `send_input` drags (`to_target`) drive the HTML5 events. Hover + `frames_after` shows
  tooltips. New action tool `ui_widget {entity, selector, action: scroll_to|select|activate|expand|
  collapse|sort, index|key|column}` is for play mode and is not undoable (runtime only).
- **R42** Lua classes `UIItemsView` (`el.itemsView`, nil on other tags), `UIDataTransfer`,
  `UIDrag`, `UIDocument.tooltip`, `el:setTooltip`, `el:setMarkdown`, `doc.onReload`, all bound
  in `engine/core/script/binding/HtmlUIClassesLua.cpp` and documented in
  `docs/mcp/guide/lua-api.md`. `jasperengine-editor check --ui` reports HTML-0029..0032.

### Docs & examples

- **R43** New examples in `docs/mcp/guide/html-ui-examples` with manifest entries, viewports
  and checks: `server_browser` (table-view 10k rows, sort, resize, selection, rich tooltips),
  `chat_log` (list-view auto heights, `anchor="end"`, Markdown messages), `quest_log`
  (tree-view + `<markdown src>`). `inventory` is rewritten to use `dataTransfer`, `dropzone`, a
  ghost, can-drop classes and `tooltip=`, with no manual drag state. The `create_ui_document`
  `template` enum grows accordingly.
- **R44** `docs/mcp/guide/html-ui.md` gets new sections, "Long lists (virtual views)",
  "Tooltips", "Drag and drop", "Markdown" and a rewritten "Reloads, onLoad and lifetimes". The
  support matrix is regenerated (`tools/mcp/gen_html_ui_support.py`). `docs/html-ui/FORK.md`
  records P7-L/P7-D.

### Data & compatibility

- **R45** No scene or component format change (the reload mode is an editor preference and a
  call argument). Old documents behave the same except for the documented `dragover` repeat,
  the now-visible `title` tooltips (a document can disable them with
  `UIDocument.tooltip.configure{enabled=false}`) and no `onLoad` on patch reloads. All three are
  in the changelog. Diagnostic codes 0029–0032 are appended. 0015 is never reused.

## 6. Design constraints & decisions

### 6.1 Layout of the module
`engine/core/htmlui/widgets/` holds `VirtualWindowCore.{h,cpp}` (copied),
`HeightIndex.{h,cpp}` (Fenwick), `ItemsViewElement.{h,cpp}` (shared base: pool, scroll, selection,
keys), `ListView/GridView/TreeView/TableView*.cpp`, `TooltipManager.{h,cpp}`,
`PopupPlacement.{h,cpp}`, `DragDropSession.{h,cpp}`, `Markdown/` (copied parser +
`MarkdownHtml.cpp` emitter). Live reload lives in `engine/core/subsystem/UIDocumentSystemPatch.cpp`
(new) and `compat/TreeReconcile.{h,cpp}` (pure diff of two `HtmlTree`s → edit script, unit-testable
without RmlUi). Instancers are registered in `HtmlUI::acquire` next to `UILottieElement`
(`engine/core/htmlui/HtmlUI.cpp:95-96`).

### 6.2 Virtual view structure
```
<list-view>                       overflow-y:auto; layout boundary (P7-L); tab stop
  <div class="htmlui-sizer">      height = total (HeightIndex::total())
    <div class="htmlui-row" style="position:absolute; top:Ypx; left:0; right:0">… ×pool
```
Scrolling moves only the container's scroll offset. Rows move (`top`) only on rebind. The view
implements `VirtualWindowCore::Host` (EnsurePool / Rebind / UnbindSlot / SlotBinds /
DestroyTailSlot) and calls `Update` from `OnUpdate` and from scroll events, with
`allowShrink = !inDispatch`.
```cpp
class HeightIndex {                    // Fenwick over row heights (px, float)
public:
    void reset(int count, float estimate); void set(int i, float h); void append(int n, float est);
    float offsetOf(int i) const;       // O(log n)
    int   indexAt(float y) const;      // O(log n) descent
    float total() const;
};
```

### 6.3 Placement (pure, unit-tested)
```cpp
struct PlaceRequest { Rect anchor; Size popup; Rect viewport; Side preferred; float gap=6, pad=4, arrow=0; };
struct PlaceResult  { Rect rect; Side side; float arrowOffset; bool flipped, clamped; };
PlaceResult placePopup(const PlaceRequest&);   // flip if room(preferred) < size && room(opposite) > room(preferred), then clamp
```
`title` tooltips use a 1×1 anchor at the pointer with `gap=16, preferred=Bottom`.

### 6.4 Lua sketch
```lua
local v = doc:getElementById("servers").itemsView
v:setProvider{ count = #servers, key = function(i) return servers[i].id end,
               bind = function(row, i) row:querySelector(".name").textContent = servers[i].name end }
v:scrollToIndex(120, "center"); v:setSelection{3, 4}; print(v:getVisibleRange())
slot:addEventListener("dragstart", function(e) e.dataTransfer:setData("item", inv[i]); e.dataTransfer.effectAllowed = "move" end)
bag:addEventListener("dragover", function(e) if e.dataTransfer:getData("item").kind == "weapon" then e:preventDefault() end end)
bag:addEventListener("drop", function(e) equip(e.dataTransfer:getData("item")) end)
btn:setTooltip(function() return "Sell for " .. price .. " gold" end, { placement = "right", arrow = true })
doc.onReload = function(report) print(report.updated, report.fallbackReason) end
```

### 6.5 Decisions taken now
- Elements are named `list-view` etc. (Unity UI Toolkit names, with the custom-element hyphen),
  not `virtual-list`.
- Data-model rows use 0-based `i` (as `data-for` does) and Lua provider indices are 1-based (as
  Lua is). Both are documented side by side.
- Tooltips and ghosts live in the source document, not an overlay document, so author CSS applies.
- `dataTransfer` values are Lua references, not strings, and stay within one Lua state. Strings
  are only required for `text/plain` (clipboard parity).
- The reconcile diffs compiled `HtmlTree`s rather than loading a shell document and moving its
  elements. That avoids cross-document data-view moves and keeps the source map exact.
- `.rml` documents and `doc:reload{mode="rebuild"}` keep today's code path (unchanged, still
  tested).

## 7. Acceptance criteria

- **AC1** gtests `VirtualWindowCore.*` (ported OE cases: ring shift, subset changeset,
  safety-net repair, lane change without ghost/blank, shrink deferred during dispatch) and
  `HeightIndex.*` (random set/query vs. a naive prefix array, 100k ops) pass.
- **AC2** `HtmlUIWidgets.ListPoolBound`: a 10k list in a 600 px view with 48 px rows has
  pool ≤ 13 + 8 + 8. Scrolling to every 97th index keeps the element-create counter flat after
  warm-up, and `inspect_ui` reports `first/last` matching `HeightIndex::indexAt`.
- **AC3** `HtmlUIWidgets.AutoHeightAnchor`: changing the heights of rows above the viewport
  keeps the first visible row's screen y within 0.5 px.
- **AC4** `HtmlUIWidgets.ModelBinding`: `model:set("items[5000].name","X")` while 5000 is
  visible updates exactly that row's text. Growing/shrinking `items` updates `count` and the
  sizer height. With P7-D present, rebinding creates no elements.
- **AC5** `HtmlUIWidgets.Keyboard` and `TestInputGamepad.ListViewNav`: Down×3, PageDown, End,
  Shift+Up, Ctrl+A produce the expected selection/active index. D-pad Up on the first item moves
  focus to the element above the view (spatial nav). A gamepad A fires `activate`.
- **AC6** `HtmlUIWidgets.Tree`/`.Table`/`.Grid`: expand/collapse/Left/Right; header sort event
  and classes; a column resize by `send_input` drag changes the header and all pooled cells
  equally; a grid resize from 4 to 3 lanes leaves no stale cell.
- **AC7** Goldens (`engine/tests/htmlui/golden/s17/`): list, grid, tree and table at rest and
  after scrolling, a `title` tooltip near each viewport edge (4 flips/clamps), a rich tooltip
  with arrow, drag ghost + allowed/denied classes, and a Markdown sample (every block kind).
  Chromium references come from `tools/htmlui/render-reference.mjs` for the Markdown and table
  fixtures.
- **AC8** `PopupPlacement.*` (ported `TooltipPlacementTests` cases + four-side flip/clamp) and
  `TooltipManager.*` on a fixed `HtmlUIClock`: no show before 0.5 s; shown at 0.5 s; hop within
  0.5 s immediate; hidden on mousedown, wheel, keydown and removal; a child of the source does not
  restart the delay; keyboard focus shows it.
- **AC9** `TestScriptEvents.DataTransfer*`: event order `dragstart → drag* → dragenter →
  dragover* → drop → dragend` (and `dragleave`/`dragend(none)` on Escape and on a non-accepting
  target). A table payload arrives by identity. `dropzone="item"` accepts without Lua.
  `UIDrag.begin/drop` produce the same sequence. Inside a list-view, `dropIndex/dropPosition` are
  correct for the before/after/on bands.
- **AC10** `MarkdownParser.*` (ported OE tests, plus nested lists of depth 3) and
  `MarkdownHtml.*`: emitted RML for a corpus of 30 snippets matches checked-in expectations.
  `<script>` and `<img onerror>` in Markdown appear as text. Inline `<markdown>` keeps
  `<https://a.b>` as a link.
- **AC11** `TreeReconcile.*` (pure): ported `UIHotReloadTests` cases plus 3-way cases (Lua changed
  class/attr/style/text + authored unchanged → live kept; authored changed → authored wins;
  runtime child kept after its predecessor; type change under the same id → replaced).
- **AC12** `TestDocumentReload.Patch*`: after editing the file (text, a class, a new button, a
  removed node), the same `UIElement` handles are valid, listeners on id-less elements still
  fire, an anime animation keeps running, a Lua-appended node stays, the text-field value and
  caret, scroll and focus are kept, an unchanged `<script>` did not re-run (counter), a changed
  one re-ran once, `onReload` fired and `onLoad` did not. Changing `data-for` replaces only that
  subtree. A broken edit keeps the live UI and reports diagnostics. The existing rebuild tests
  still pass with `mode="rebuild"`.
- **AC13** Perf gates in `jasperengine-htmlui-tests` (`HtmlUIWidgetsPerf.ListScroll10k`,
  `.ListScroll10kAuto`, `.PatchReload2k`) on the CI build server: p95 ≤ 1.0 ms, ≤ 1.25 ms and
  ≤ 15 ms. Manual mobile check: the `server_browser` example on the reference Android device
  shows ≥ 58 fps average and p95 UI CPU ≤ 4 ms in the stats overlay over a 10 s flick (recorded in
  `docs/html-ui/s17-acceptance.md`).
- **AC14** MCP tests `tools/mcp/tests/test_html_ui_widgets.py`: `inspect_ui` widget/tooltip/drag/
  reload fields; `assert_ui` `virtual_window`, `tooltip`, `selected`; `ui_widget` actions;
  a `send_input` drag from slot 0 to slot 5 in `inventory` swaps the items (assert via
  `inspect_ui models`). `test_html_ui_examples.py` runs the new examples' manifest checks at all
  viewports.
- **AC15** `ExamplesCompatTest` passes for the new examples with no Warning/Error diagnostics.
  `check --ui` on fixtures reports HTML-0029..0032. The support-matrix generator test shows
  `title` as emulated, `draggable` as mapped, and `list-view`… and `markdown` as engine elements.
- **AC16** Web (WebGL2) and Android export smoke (`tools/smoke-test.sh` + the S02 web gate)
  load `server_browser` and `inventory` with no errors. `title` shows on hover in the web build
  (manual).

## 8. Dependencies & conflicts

- **Depends on:** S01 (std::span, defaulted `==` in copied code), S02 (gtest targets, CI perf
  job), S05 (it replaces the trigger `HtmlUIWatcher` → `HotReloadService`; S17 replaces what
  `reloadDocument` does). It uses S15 UI timings for the perf report if they have landed.
- **Coordinates with:** S16 (UI draw batching; pooled rows reduce mesh churn and the perf gates
  are measured with whatever S16 has landed). S23 (Lua hot reload in Play: the component-script
  reload must not re-run document scripts, and both use `onReload` naming). S22 (input action
  maps: gamepad B cancel and d-pad routing go through the same `GamepadUIKey` path). S31 (`call`
  CLI exposes `ui_widget`).
- **Merge hotspots:** `UIDocumentSystemReload.cpp`/`UIDocumentSystemCompat.cpp` (S05),
  `UIDocumentSystemInput.cpp` (S22), `HtmlUIClassesLua.cpp` and `docs/mcp/guide/lua-api.md`
  (S23 JSON API description), `HtmlUiTools.cpp` (S15/S31), `DiagnosticCodes.{h,cpp}`,
  `compat/tables/ElementTable.cpp`, `compat/ua/useragent.css` (S16), `docs/html-ui/FORK.md` and
  the RmlUi fork (`Element.cpp`, `ElementDocument.cpp`, `DataModel.cpp`, `Context.cpp`; S16),
  `examples.json`, `NOTICE.md`.

## 9. Risks & defaults

- **Layout boundary patch (P7-L) is the biggest risk.** A partial relayout must reproduce
  full-layout results, including scrollbars, `%` sizes and sticky elements. *Default:* the
  boundary applies only when the view's width and height are definite and it is not
  `display: contents`/inline. A debug build cross-checks partial vs. full layout every 60 frames
  in tests. If the patch slips, ship the views without it and waive only AC13's mobile number.
- **Rebindable aliases (P7-D)** touch RmlUi's data binding. *Default:* the `RowIndex` entry is
  used only by views. Without it, rows re-instance their content (correct, slower).
- **3-way merge surprises.** A user reverting an attribute that Lua had changed sees the Lua
  value kept. *Default:* this matches "runtime wins unless the author edited it", it is
  documented, and the report lists `runtimeKept`. Rebuild is one click away.
- **`dragover` semantics change** could double-run old handlers. *Default:* accepted. The old
  enter-once behaviour is still available as `dragenter`, and the changelog says so.
- **`title` now visible** in existing documents. *Default:* on. Opt out per document.
- **Markdown dialect.** *Default:* their subset plus deeper lists and images. No GFM task lists,
  footnotes or HTML passthrough.
- **Touch long-press vs. scroll** on draggable rows. *Default:* 400 ms press without 8 px of
  movement starts a drag. Otherwise the press scrolls.

## Deviations

- **The live patch diffs compiled RML, not the HTML tree (R27).** The previous compile's RML and
  the new one are read back as `RmlNode` trees (`compat/TreeReconcile.*`, pure and unit-tested)
  and reconciled per OE's rules (id, tag + same classes, tag + class superset, tag; texts by
  position). The live elements are found by `data-htmlui-loc`; the authored set is recorded by an
  `Rml::Plugin` during each document parse (`OnDocumentOpen` .. `OnDocumentLoad`), so data-view
  copies, `cloneNode` copies and script-made nodes are runtime nodes.
- **Restyle only when the sheets changed (R31).** A patch rebinds the document to the new
  compile; it rebuilds the style sheet container only when the two compiles wrote different
  sheets (inline styles keyed by renumbered locs, a changed `<style>` or linked file). Rebuilding it
  for a markup-only edit cost 4 ms of a 2,000-element patch for nothing. Changes of files the patch
  does not read again (a font, a `<script src>` Lua file, an SVG, a Lottie file) rebuild the
  document; a `<markdown src>` file is compiled into the RML and patches.
- **Fork patch names (R35).** P7-D is `DataModel::InsertRowAlias` / `SetRowAlias` (an alias of a
  row element re-pointed at another item, resolved through a `#row` address entry), not
  `SetRowIndex`: one alias serves `item` and `i` alike. Besides P7-L and P7-D the branch adds P7-T
  (an inert `<template>` in RML documents), P7-C (`Context::CancelDrag`, `IsDragging`, a prevented
  `dragstart` starts no drag) and P7-H (touch long-press drag inside scroll containers), all in
  `docs/html-ui/FORK.md`.
- **The table header is sticky (R11).** It uses the fork's `position: sticky` (moved at each scroll
  without a layout) instead of a transform: a transformed header painted under the rows.
- **Tooltips are positioned by code (R17).** The UA sheet cannot use `position: fixed` (not in
  the fork's compat output); the engine sets the tooltip's position, `z-index` and
  `pointer-events` inline on an element appended to the source document's body.
- **Engine classes are for CSS (R20, R22).** `htmlui-*` classes (drop feedback, ghost, rows,
  header) follow the compat rule that scripts do not see engine classes (`classList`, `className`);
  rows also get `selected`, `odd`/`even`, `expanded`, `has-children`, which scripts see.
- **Indices.** Lua and C++ `UIItemsView` indices are 1-based; MCP (`inspect_ui` `widget`,
  `edit_ui_view`) and `data-for`'s `i` are 0-based, as the rest of the MCP tools.
- **Lua bindings live in `script/binding/HtmlUIWidgetsLua.cpp` (R42),** registered next to
  `HtmlUIClassesLua.cpp` by `LuaBinding`, to keep that file's size in check. `doc.tooltip` is an
  instance property (`UIDocumentTooltip`).
- **A provider tree caches its structure.** `children(key)` (nil for the roots) and
  `hasChildren(key)` are asked once per key until `refresh()`, which keeps an expand within R34's
  budget with Lua callbacks.
- **`check --ui` (R42, AC15)** reports the compile's widget code HTMLUI-HTML-0030 (a view without
  its `<template>`). 0029 (no definite height) and 0031 (a `data-for` of more than 500 elements)
  need a layout or a data model, so they are runtime diagnostics: the Console, the document's
  messages and `inspect_ui` diagnostics; 0032 is a note, which `check` does not list. Tests:
  `test_html_ui_checks.py::test_cli_ui_widget_codes`, `HtmlUIWidgets.AnchorEndAndRuntimeDiagnostics`,
  `HtmlUIWidgets.MarkdownElements`.
- **Test names.** AC5's gamepad case is `HtmlUIWidgets.GamepadNavigation` (there is no
  `TestInputGamepad` suite); AC6's column resize by `send_input` is
  `test_html_ui_widgets.py::test_column_resize_by_drag` (and `HtmlUIWidgets.Table` in C++); R32's
  TEXTURE documents are `TextureModeTest.WidgetsThroughWorldPointer`.
- **No changelog file exists in the repository;** the behaviour changes (`dragover` repeating,
  visible `title` tooltips, `onReload` instead of `onLoad` on patches) are in
  `docs/mcp/guide/html-ui.md` "Changes for existing documents".
- **Inline Markdown is an `.html` feature.** The compile captures the raw text between `<markdown>`
  and `</markdown>`; an `.rml` document's RmlUi parser reads that content as RML, so `.rml`
  documents use `src=` or `el:setMarkdown`.
- **No Chromium references for AC7.** `tools/htmlui/render-reference.mjs` runs on a developer
  host with the pinned headless shell, never in CI or Docker; Chromium also has no `<markdown>`
  or `<table-view>`, so a reference would be of the HTML they compile to. The goldens are the
  engine's (glcore and Vulkan from one set), each test probing the pixels its fixture defines.
- **AC13's mobile check is not recorded.** No reference phone was available; the gates ran on the
  benchmark host E (`docs/html-ui/s17-acceptance.md`) and the Android build ran on the emulator
  (SwiftShader: no frame-time meaning).
- **`edit_ui_view` is in an `html_ui` tool group (S31, merged after the spec).** New tools go into a
  non-core group; `tool_groups {"enable": ["html_ui"]}` lists it, a call by name works either way.
- **`ui_widget` is named `edit_ui_view` (R41).** S31's convention (merged after the spec) names a
  new tool verb_noun, `edit_<thing>` for a multi-operation tool; its parameters and actions are
  R41's.
- **`assert_ui`'s `virtual_window` takes `max`, not `max_pool` (R40).** The check's existing `max`
  says the most rows; a key of its own did not fit the tool's S31 budget (2,170 characters).
