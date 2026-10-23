# 06 — UI framework (runtime game UI + editor UI stack)

Scope: THEIRS = `external/OpenEngine` (MIT), OURS = JasperEngine. Paths for THEIRS are relative to
`external/OpenEngine/`; paths for OURS are relative to the repo root. Everything below was checked in
the code, not inferred from file names. Line counts come from `wc -l`.

TL;DR: the two systems point in opposite directions. **THEIRS** is a lean, retained-mode
"Unity UI Toolkit" clone: XML (`.uxml`) + a CSS subset, Yoga flexbox only, one instanced SDF draw
call, Slug vector text, and a large library of editor-grade widgets. Their whole 313k-line editor runs
on it (no ImGui anywhere). **OURS** is a standards-first HTML/CSS engine on a heavily patched
RmlUi 6.3 fork. It covers far more CSS (grid, inline/block/table layout, media queries, keyframes,
filters, masks, effects, 3D transforms), and adds data binding, a Lua DOM, Lottie, video, gamepad
navigation and world-space UI. It is used only for game UI; the editor is ImGui.

What THEIRS does better: rendering efficiency, text rendering quality, widget breadth
(virtualized views, tooltips, drag-drop payloads), and having the editor itself prove the UI works.

---

## 1. What THEIRS has

### 1.1 Size and structure
- `Engine/Modules/UI`: 166,840 lines in total. 82,529 of those are tests (193 files, about 2,004
  `TEST*` cases). The rest is Source 28.8k (core), Controls 19.0k, Parsers 9.0k, Interaction 1.8k,
  Layout 1.7k, and public headers about 22k.
- Supporting code outside the module:
  - Text module `Engine/Modules/Text` (3.3k lines, FreeType + HarfBuzz + Slug).
  - UI shaders `Engine/Modules/Rendering/Shaders/ui_sdf.{vert,frag}` and `Shaders/UI/*.glsl` (2.6k lines).
  - Game UI host `Engine/Source/Engine/GameUI/` (`GameUIHost`, `GameplayUI`, `UIScaleProjectSettings`).
  - ECS component `Engine/Include/Components/UI/UIDocument.h`.
  - C# wrappers under `Managed/` (tests in `Managed/InteropTests/Ui*.cs`).
- Design doc: `Engine/Modules/UI/UISystemDesign.md` (authored by "Augment Agent" and partly stale:
  it still lists only descendant/child combinators).
- User docs: `docs/ui/{ui-api-guide,ui-control-defaults,game-ui-scaling,text-effects,svg-images,transform}.mdx`,
  `docs/editor/ui-component-library.mdx` and `docs/contributing/editor-ui-guidelines.mdx`.
- Third-party libraries, all through vcpkg (`vcpkg.json`), none vendored under `ThirdParty/`:
  - Yoga (MIT), lexbor (Apache-2.0) and pugixml (MIT).
  - FreeType (FTL/GPLv2) and HarfBuzz (MIT).
  - ThorVG (MIT, SVG raster, `Engine/Source/Assets/SvgRasterizer.cpp`) and tinyexpr (zlib).

### 1.2 Markup
- XML through pugixml (`Source/Parsers/XMLParser.cpp`, `BuildTreePugi`). The editor and games use
  the `.uxml` extension.
- Tags map to registered controls through a fluent registry:
  `Register<Badge>("Badge").Text(&Badge::SetText).Attr("text", …).TagAlias("badge")`
  (`Include/UI/Registration/ElementRegistration.h`, `Source/Controls/ControlRegistration.cpp`).
  Custom attribute types plug in by specializing `Parser<T>`. An unknown tag falls back to
  `UIElement` with a warning.
- Templates are parsed into data-only `UITemplateNode` trees (`Include/UI/UITemplateNode.h`) and
  cloned through the factory.
- There are no `<template>`/`<slot>` or include mechanisms in the markup itself. Composition is done
  in C++ (`Mount` is a portal control, `Source/Controls/Mount.cpp`).

### 1.3 CSS
- **Parser and cascade**:
  - Stylesheets are parsed with lexbor's `lxb_css_stylesheet_parse` (48 `lxb_` calls in
    `Source/Parsers/CSSStylesheetParser.cpp`).
  - Values are parsed by their own code (`CSSValueParsers.cpp`, 3.2k lines).
  - Cascade and selector matching live in `CSSParser.cpp` (2.6k lines) and the
    `UIManager_StyleResolve`/`StyleAnalysis`/`CascadeProbe` files.
- **Selectors** (`Include/UI/Selectors/SelectorTypes.h`):
  - type, `#id`, `.class`, and attribute selectors with every operator (`= ~= |= ^= $= *=`, and the `i` flag);
  - combinators: descendant, `>`, `+`, `~`;
  - pseudo-classes: `:hover :active :focus :focus-visible :focus-within :disabled :enabled :checked :root :empty`,
    `:first/last/only-child`, `:*-of-type`, `:nth-child/nth-last-child/nth-of-type/nth-last-of-type`,
    `:is :not :where`, and custom pseudo-classes resolved by `StringId`;
  - specificity is a lexicographic (A, B, C) triple.
  - Missing: `:has`, pseudo-elements (`::before`/`::after`).
- **Cascade features**:
  - `!important` tiers, `initial`/`inherit`/`unset`, and custom properties (`--x`, `var()`) with
    scoped resolution and an LRU memo (`CSSParser.cpp` ~1816–2080, `CSSVarResolver.cpp`).
  - `calc()`/`min()`/`max()` are folded at parse time, same-type only: `calc(50% - 4px)` is
    rejected (`CSSValueParsers.cpp:509-516`).
  - `@import` works with cycle detection and GUID dependency tracking (`Source/Assets/CssImports.cpp`,
    `UIStyleAsset.cpp`).
- **No** `@media`, `@keyframes`/`animation`, `@font-face`, `@supports`, nesting, CSS gradients
  (gradients exist only as internal `GradientMode` values in `UIPrimitive.h`), `filter`,
  `backdrop-filter` or masks. A grep over the module finds none.
- **About 100 properties** (`StylePropertyId` in `Include/UI/UIStyle.h:596-702`): flex,
  box model, position (relative/absolute only), overflow (`Visible`/`Hidden` only), z-index, order,
  text props (`white-space`, `word-break`, `overflow-wrap`, `text-overflow`, `letter-spacing`),
  backgrounds (image, size, position, repeat, tint, saturation), per-side borders and radii,
  `border-image-slice` (nine-slice), `box-shadow` (incl. inset), a custom `glow`, outline,
  `transform`/`transform-origin` (2D only: `docs/ui/transform.mdx:109` "no 3D transforms, no
  perspective"), transitions, `text-shadow`/`text-glow`/`text-outline` (custom), and the selection color.
- **Default stylesheet**: `Engine/Modules/UI/Assets/defaults.css` is embedded at build time
  (`CMakeLists.txt`, `EmbedTextFile.cmake`).

### 1.4 Layout
- Facebook **Yoga** only (`Source/Layout/YogaLayout.cpp`, `Source/UIManager_Layout.cpp`, 3.2k lines).
- `DisplayMode` is `{None, Block, Inline, Flex}` (`UIStyle.h:31`). Block and Inline are Yoga column
  containers, so there is no inline formatting context, no floats, no grid, no tables, and no
  fixed/sticky positioning.
- Text is a Yoga leaf with measure and baseline callbacks (`YGNodeSetMeasureFunc`/`YGNodeSetBaselineFunc`).
- The cascade and text pre-measure run in parallel per subtree on the job system ("MT-4.2",
  `UIManager_Layout.cpp:1132-1775`). A debug guard asserts that no Yoga mutation happens inside the
  compute region (`YogaLayout.cpp:12-31`).
- Parity with Chrome is measured: `Tests/Flex*ChromeParityTests.cpp`, `TextGeometryChromeParityTests.cpp`,
  and pixel-sampling fixtures under `Tests/ChromeReference/` (PowerShell + HTML).

### 1.5 Rendering
- **One instanced SDF draw call** for the whole UI:
  - Each element emits 144-byte `UIPrimitive` structs (`Include/UI/UIPrimitive.h`). The modes are
    Rect, Slug glyph, Textured, Line, Bezier and Triangle; elliptical corner radii, per-side border
    widths, shadow, glow and gradient modes are packed into the struct.
  - Clips live in an SSBO with parent chains, so a rounded parent clips its children.
  - CSS transforms live in a third SSBO as an affine chain (`UITransformSlot`); the vertex shader
    composes it, so changing a transform rewrites a single slot.
  - Submission is `Draw(6, primitiveCount)` against a bindless texture set (`Source/UIManager_RenderRG.cpp:381`).
  - A "compat profile" splits the list into consecutive texture runs, one draw per run, when bindless
    is not available (`UIManager_RenderRG.cpp:350-367`, `Source/UICompatDrawRuns.cpp`).
- Shaders: `Rendering/Shaders/UI/sdf_functions.glsl` (rounded-rect, border, shadow and glow SDFs),
  `ui_sdf_common.glsl`, `ui_sdf_textures.glsl`, and `text_mask_gamma.glsl` (text contrast/gamma
  matched to Chrome).
- The backends are Vulkan, Metal and WebGPU. The design assumes SSBOs and bindless textures, which do
  not exist on WebGL2/GLES3.0.
- Color-space aware compositing: `UITargetSpace.cpp` and `UITextureSpace.cpp` handle SDR content in
  HDR targets, paper white, and encoded-blend targets.
- **SVG backgrounds** are rasterized at the exact device-pixel size they are drawn. A per-size
  variant cache has an LRU budget and resize hysteresis (`Source/SvgBackgroundRaster.cpp`,
  `BackgroundImageCache.cpp`, `docs/ui/svg-images.mdx`).
- Idle gate: a frame with no input and nothing dirty skips the work (`UIManager.h:957-963`, `Tests/UIIdleGateTests`).

### 1.6 Text
- FreeType outlines are converted into Slug curve/band textures (`Engine/Modules/Text/Source/FontAtlas_Slug.cpp`;
  cubics are reduced to two quadratics at line 79).
- The glyph coverage is evaluated per pixel in the fragment shader (`Shaders/UI/slug_functions.glsl`,
  whose header reads "Ported from Eric Lengyel's reference HLSL (MIT license)"). Glyphs are
  therefore resolution-independent: they stay sharp under transform or scale, and a DPI change does
  not trigger a re-raster (`docs/ui/game-ui-scaling.mdx`).
- HarfBuzz shaping, and color emoji through an RGBA atlas (`kPrimColorGlyphBit`).
- Shadow, glow and outline are computed on the GPU from the distance to the glyph outline
  (`Shaders/UI/text_effects.glsl`, `docs/ui/text-effects.mdx`).
- **No bidi** (no SheenBidi or fribidi anywhere).
- Text controls get word segmentation (a subset of UAX #29 tuned to measured Chrome behaviour,
  `Source/TextSegmentation.cpp`). There is no grapheme or ZWJ handling.
- `MarkdownText` control (`Source/Parsers/MarkdownParser.cpp` and `MarkdownInlineParser.cpp`,
  ~1.3k lines, standard library only): headings, lists, code, quotes, rules, tables and links.

### 1.7 Input and focus
- Capture, target and bubble phases; typed `StringId` events (`Include/UI/UIEvents.h`).
- Unity-style manipulators: `Interaction/Manipulator`, `PointerManipulator`, `ContextMenuManipulator`
  (which can hand off to a native OS menu through `Platform::INativeContextMenu`), `DismissablePopup`,
  `ItemResizeGesture` and `FocusIsInside`.
- Tab order follows `tabindex` (`UIManager_Input.cpp:731-790`). `:focus-visible` matches keyboard
  focus only. Caret blink honours the OS "never blink" setting (`CaretBlink.h`).
- **No gamepad or spatial navigation, no IME composition path, no accessibility, no localization**
  in the UI module (all grepped).
- Drag and drop: `DragDropManager` with typed `Payload`s, `DropTarget` can-drop feedback and
  pluggable ghost renderers (`Interaction/DragDropManager.cpp`, `Payload.h`, `DragDropGhostRendererRegistry`).
- Tooltips: `Interaction/TooltipOverlay.cpp` (549 lines) and `Layout/DropdownPopupPlacement.cpp`
  (flip/clamp placement).

### 1.8 Data binding, scripting and animation
- **No data binding or MVVM.** UI code is imperative C++ (`FindById`, `SetText`, `Overrides()`) or
  C# (`Ui.FindElement("Score") is Ui.Label`, `start.Clicked += …`, `docs/ui/ui-api-guide.mdx`).
- Virtualized views take an `IListDataProvider` (count, ids, heights, change sets) plus
  `ItemFactory`/`ItemBinder` callbacks (`Include/UI/Controls/ListView.h:28-112`).
- Typed programmatic overrides: `el.Overrides().Set(Style::Width, StyleLength::Px(200))`. Through
  `StyleProp<T, StyleImpact>` each one marks either layout or visual dirty
  (`StyleOverrides.h`, `StyleProp.h`).
- Animation is **CSS transitions only** (`TransitionEngine.{h,cpp}`, `StyleInterpolation.cpp`, easing
  from `Mathematics/Easing.h`). There are no keyframes, no script tween API and no Lottie.

### 1.9 Widgets (`Include/UI/Controls`, 47 headers)
Label, Button, TextField, TextArea (incremental wrap, scroll culling), FloatField / IntField /
Vector3Field (drag to change, `tinyexpr` expressions through `Modules/NumericExpression`), Slider,
Toggle, Checkbox, Dropdown (UI or native OS mode), EnumField, MaskField, Foldout, Accordion,
ScrollView and Scrollbar, the **virtualized ListView / GridView / TreeView / TableView** (shared by
`VirtualWindowCore`, with `AxisHeaderBar`/`AxisModel` for column resize and sort), ChartView,
SplitView, Splitter, WeightedPane, FloatingPanel, SearchDialog, SearchFieldWithFilter, PopoverArrow,
MarkdownText, and docking (`DockspaceElement`, `DockTab`, `DockTabBar`, `DockOverlay`,
`Layout/Docking.cpp`, `DockLayoutController`, `DockingHitTest`).

### 1.10 Hot reload, devtools and tests
- **Hot reload** (`Source/UIHotReload.cpp`, 1.2k lines) reconciles the live tree in place, keyed by
  id (`ReconcileElement`/`ReconcileChildren`, lines 157-453). Template-owned nodes carry a binding
  id, so nodes added at runtime and already-wired controls survive a reload, and runtime-added
  classes are diffed and kept. `@import` changes propagate through a GUID graph, and every
  `UIManager` adopts a published reload.
- **Devtools**:
  - `UIManager::EmitDebugCapture` dumps the tree as JSON (`UIManager_Debug.cpp`).
  - The editor's DebugServer backs the MCP tools `get_ui_tree`, `get_panel_tree`, `click_element`,
    `perform_drop`, `move_pointer`, `send_key` and `input_text` (`mcp/src/tools/ui.ts`), which work
    on the **editor itself**.
  - Per-phase frame timings (GenAllPrimitives, BuildYoga, Cascade, Geometry, HitTest) are reported
    by `Tests/UIEmitPerfTests.cpp`.
  - There is no in-app visual element inspector panel.
- **Tests**: 2,004 GoogleTest cases (layout, CSS, text geometry, focus, virtualization, hot reload,
  teardown and threading), with the Chrome reference values hard-coded. No golden PNGs.

### 1.11 Game UI integration
- `UIDocument` component (`Engine/Include/Components/UI/UIDocument.h`) with fields `Layout` (.uxml),
  `Style` (.css), `SortOrder`, and `RenderMode` `Overlay`/`Fullscreen`. All documents are subtrees of
  one game `UIManager` owned by `GameUIHost`, which serves the Player, the Game View and the Scene
  View preview.
- **No world-space or render-to-texture UI.**
- Reference-resolution scaling with modes fit, fill, width, height and platform
  (`UIScaleSettings`, `docs/ui/game-ui-scaling.mdx`).

### 1.12 Editor UI stack: they dogfood their runtime UI
- `Apps/Editor` (313k lines) contains **zero** ImGui references.
  - Theme and layout: `Apps/Editor/Assets/UI` holds 85 CSS files (14.6k lines of theme: `theme/tokens.css`,
    `widgets.css` 2.7k, `views.css` 1.4k) and 15 `.uxml` files.
  - The dock configuration is declarative (`Assets/UI/layout.uxml`: `DockConfig`/`DockablePanel`
    entries with per-panel `layout=`/`style=`).
  - Most panels are built in C++ (`Source/Panels`, 100k lines). The code counts
    `make_unique<UIElement>` ×783, `<Label>` ×543 and `<Button>` ×236. There are 106 inspector files.
- Editor-specific UI code:
  - `Source/UI` (16k lines: AssetField, EntityField, CurveField, reorderable lists, overlays);
  - `Source/Docking` (1.6k lines: floating frames, layout presets, working-layout persistence);
  - `Source/UI/Layout/EditorDockConfigParser.cpp`.
- Because the UI is theirs, the editor also runs in the browser on WebGPU (README), and agents can
  drive editor panels element by element.
- Rules and tokens for editor UI: `docs/contributing/editor-ui-guidelines.mdx`.
- `Engine/Modules/MarkupECS` (5k lines) and `Apps/Editor/Source/Markups` (7.9k lines) are **not UI
  markup**. They are level-review annotations (region and box marks, comment threads, status tags;
  `MarkupECS/Include/MarkupECS/MarkupService.h`). They belong to the world-building comparison, not this one.

---

## 2. What OURS has

### 2.1 HTML/CSS UI (`engine/core/htmlui`, 285 files, 81.8k lines) on a RmlUi 6.3 fork
- Binding design: `docs/html-ui/DESIGN.md`. Fork patch ledger: `docs/html-ui/FORK.md` (761 lines).
  Phase specs: `docs/html-ui/phase-*.md` (22.5k lines of documentation).
- The fork lives at `engine/libs/rmlui` (169k lines including its tests). Our layout additions there:
  - **CSS Grid**, a port of Taffy (`Source/Core/Layout/Grid*.cpp`, `LICENSE-taffy.txt`);
  - block, inline, table and replaced formatting contexts, floats, and `LineClamp.h`;
  - UAX #14 line breaking and `text-align: justify` (`TextJustification.cpp`).
- **Compat layer** (`engine/core/htmlui/compat/`):
  - lexbor parses HTML5, which is then converted to RML (`compat/html/*`);
  - our own CSS Syntax 3 tokenizer and parser (`compat/css/CssTokenizer.cpp`, `CssParser.cpp`), with
    nesting (`Nesting.cpp`), `@media` (`MediaQuery.cpp`, 889 lines), `@import`, `@property`
    (`CompatAtProperty.cpp`) and easing functions;
  - a CSS-to-RCSS rewriter (`CssRewriter.cpp`, 2.3k lines; `SelectorRewriter.cpp`, 2.0k lines),
    gated by `ForkFeatures`;
  - a user-agent stylesheet (`compat/ua`) and the support tables (`compat/tables/*`), from which the
    support matrix is generated;
  - stable diagnostic codes `HTMLUI-<AREA>-NNNN` (`DiagnosticCodes.cpp`), also reported by
    `jasperengine-editor check --ui`.
- **CSS coverage** (DESIGN.md "Phases", row 3; FORK.md): `calc`/`min`/`max`/`clamp`; `!important`,
  `revert` and the CSS-wide keywords; `:root :is :where :has` and the focus pseudo-classes;
  `::before`/`::after`/`::marker` and counters; border styles; outline; group opacity;
  `position: fixed`/`sticky`; transitions and `@keyframes`; gradients; filters, `backdrop-filter`
  and `mask-image`; `box-shadow`; 3D transforms; `object-fit`.
- **Rendering** (`RmlRenderer.cpp`, 3.0k lines):
  - built on sokol, so it runs on GL, GLES3/WebGL2, D3D11, Metal and Vulkan;
  - one offscreen target per context, with MSAA, stencil clip masks, a layer stack, filters and a
    pipeline cache;
  - record and replay; draw counters in `HtmlUIRenderStats.h`;
  - an unchanged context skips rendering and reuses its target (`HtmlUIContextRender.cpp:139`).
- **Effects library**: `effect()` in CSS, stateless and stateful GPU simulations
  (`engine/core/htmlui/effects/`, `BuiltinEffectManifests.gen.cpp`).
- **Text**:
  - `RmlFontEngine.cpp` runs on stb_truetype + HarfBuzz + **SheenBidi**, through the shared
    `FontFace`/`FontRegistry`/`TextShaper`/`GlyphAtlas` stack;
  - font effects are computed on the CPU (`RmlFontEffects.cpp`);
  - glyphs are rasterized into an atlas at a raster scale quantized to 0.25 steps; a scale change
    regenerates the text (`HtmlUIContextRender.cpp:104-108`).
- **Media**: SVG through LunaSVG (`engine/libs/rmlui/Source/SVG`); the choice came from a benchmark
  (`tools/htmlui-bench`, `docs/html-ui/svg-benchmark.md`). Lottie through ThorVG
  (`vector/UILottieElement.cpp`). `<video>` decodes AV1/Opus WebM (`media/`). Lucide icons are built
  in (`compat/runtime/LucideIcons.cpp`).
- **Scripting and binding**:
  - Lua DOM with browser names (`engine/core/object/ui/UIDocument.h`, `UIElement.h`, `UIEvent.h`,
    `UIDataModel.h`; bindings in `engine/core/script/binding/HtmlUIClassesLua.cpp`);
  - **RmlUi data models** (`data-model`, `data-for`, `{{ }}`, `data-event-*`) driven from Lua tables;
  - an Anime.js v4-style `anime` API (`engine/core/htmlui/anim/`);
  - `<script type="text/lua">` inside documents;
  - one UI clock for every animation source (`HtmlUIClock`).
- **Input**:
  - keys, text, IME commit, wheel, touch and clipboard;
  - **gamepad as `keydown`/`keyup` events with spatial navigation (`nav-*`)**
    (`engine/core/subsystem/UIDocumentSystemInput.cpp:177-230`);
  - pointer pass-through between stacked documents (fork hook P4-1).
- **ECS and world-space UI**: `UIDocumentComponent` (`renderMode` SCREEN or TEXTURE, texture size,
  `pixelRatio`, `onLoad`/`onUnload`) and `UIDocumentSystem*.cpp` (8 files). Texture documents are
  world-space UI on meshes, with UV hit testing (`UIWorldRaycast.cpp`).
- **Hot reload** (`UIDocumentSystemReload.cpp`): file stamps with a settle window. Stylesheet-only
  changes restyle in place. Any other change reloads the whole document, after snapshotting and then
  restoring each id'd element's scroll, value, checked state, selection and focus (lines 93-240).
- **Devtools**:
  - the RmlUi Debugger (editor, Debug exports and tests only);
  - `HtmlUIInspector.cpp` (element tree, computed styles, boxes, overflow/clip/hidden flags, effects
    and media);
  - `HtmlUIAssertions.cpp`;
  - MCP tools `inspect_ui`, `assert_ui`, `create_ui_document` and `send_input`
    (`editor/mcp/tools/HtmlUiTools.cpp`, `UIEffectTools.cpp`).
- **Editor-side tooling** (`editor/htmlui`):
  - importers for Google Fonts, stylesheets, images, LottieFiles and video (FFmpeg);
  - the example library (`editor/util/HtmlUiExamples.*`: 10 examples plus a blank template);
  - templates and a file watcher (`editor/util/HtmlUITemplates.*`, `HtmlUIWatcher.*`);
  - an HTML/CSS lexer and suggestions in the code editor (`editor/window/widget/HtmlCssLexer.cpp`,
    `SemanticSuggestions.cpp`, `RcssProperties.h`).
- **Tests**:
  - `engine/tests/htmlui`: 1,136 `TEST*` definitions (3,145 instances per DESIGN.md), 46.7k lines,
    and **661 golden PNGs**;
  - browser references through playwright + headless Chromium (`tools/htmlui/render-reference.mjs`);
  - the RmlUi fork's own tests (426 cases); 24 MCP UI tests (`tools/mcp/tests`); 73 editor importer
    tests (`editor/htmlui/tests`);
  - perf gates `CompatPerf.*` and `EffectPerfTest`.
- **Gaps that exist on our side too**: tooltips are dropped (`compat/tables/ElementTable.cpp:231`:
  `title` → "no tooltips; dropped"); drag carries no drop data (`AttributeTransform.cpp:363-368`);
  there is no list virtualization; `aria-*` is kept but unused (no accessibility); there is no
  localization layer.

### 2.2 Legacy component UI (uGUI-like)
- Components in `engine/core/component/`: `RectTransformComponent` (anchors, presets, scissor),
  `LayoutGroupComponent` (horizontal, vertical, wrap), `TextComponent` (STBText), `ImageComponent`
  (nine-patch), `ButtonComponent` (four state textures), `InputFieldComponent`, `ScrollbarComponent`,
  `PanelComponent` (move/resize), `UIComponent` (buffers, custom shader, stencil), and `UIDocumentComponent`.
- Logic in `engine/core/subsystem/UISystem.cpp` (3.4k lines); wrappers in `engine/core/object/ui/*`.
- It draws one draw call per entity (`RenderSystem::drawUI`, DESIGN.md "Research facts") with the
  old `ui.frag`.

### 2.3 Editor UI stack
- Dear ImGui **1.92.8 docking branch** (`libs/imgui`, `IMGUI_HAS_DOCK`/`IMGUI_HAS_VIEWPORT`), with
  docking and multi-viewport enabled (`editor/App.cpp:1581-1586`).
- 54 editor files contain about 7.8k `ImGui::` calls. The theme is defined in code (`editor/Theme.cpp`, 332 lines).
- HTML UI appears in the editor only as content: Game and Scene views, the import panel, and a stats
  overlay (`editor/window/GameWindow.cpp:562`). The editor's own UI is not built with it.

---

## 3. Gaps: THEIRS has, OURS lacks

| Feature | Their location | Value to us | Effort |
|---|---|---|---|
| Virtualized List/Grid/Tree/Table views (pooled cells, variable heights by prefix sums, change sets, overscan) for inventories, leaderboards, chat logs, server lists | `Engine/Modules/UI/Include/UI/Controls/VirtualWindowCore.h`, `Source/Controls/{VirtualWindowCore,ListView,GridView,TreeView,TableView}.cpp`, `Include/UI/VirtualizationCoordinator.h` | H | M for the core plus a Lua `<virtual-list>`; L for a fork element |
| Tooltips with delay, hide rules and flip/clamp placement (we drop `title`) | `Source/Interaction/TooltipOverlay.cpp`, `Source/Layout/DropdownPopupPlacement.cpp` | M | S–M |
| Drag-and-drop payload model: typed payloads, drop targets, can-drop feedback, ghost renderers (equipment/inventory UIs) | `Source/Interaction/DragDropManager.cpp`, `Include/UI/Interaction/{Payload,DropTarget,DragDropGhostRendererRegistry}.h` | M | M |
| Markdown rich-text element (help screens, patch notes, quest text, tooltips; also editor docs/assistant panes) | `Source/Parsers/MarkdownParser.cpp`, `MarkdownInlineParser.cpp`, `MarkdownDocument.h`, `MarkdownSyntax.h`; `Source/Controls/MarkdownText.cpp` | M | S (parser emitting HTML) |
| Resolution-independent vector text (Slug): sharp under CSS scale/rotate animations, no atlas regeneration on DPI or scale change | `Engine/Modules/Text/Source/FontAtlas_Slug.cpp`, `Rendering/Shaders/UI/slug_functions.glsl`, `text_mask_gamma.glsl` | M–H | XL |
| GPU text shadow/glow/outline from the outline distance (ours are CPU bitmaps in `RmlFontEffects.cpp`) | `Rendering/Shaders/UI/text_effects.glsl`, `docs/ui/text-effects.mdx` | M | L (needs Slug or an SDF atlas) |
| One-draw instanced SDF primitive renderer: analytic AA rounded/elliptical boxes, per-side borders, shadows, clip chains, transform chains | `Include/UI/UIPrimitive.h`, `Source/UIManager_PrimitiveGen.cpp`, `UIManager_RenderRG.cpp`, `UICompatDrawRuns.cpp`, `Shaders/ui_sdf.*`, `Shaders/UI/sdf_functions.glsl` | M (big editor-scale UIs, mobile draw-call budgets) | XL for RmlUi (fork level; needs instance attributes instead of SSBOs on WebGL2); M for our legacy `UISystem` |
| In-place hot-reload reconciliation by id (keeps runtime-added nodes and wired handlers; ours snapshots state and rebuilds, so Lua listeners and scripts restart) | `Source/UIHotReload.cpp` (`ReconcileElement`/`ReconcileChildren`) | M | L (RmlUi DOM, Lua listener ownership) |
| Numeric expressions in number fields (`2*3+1`) | `Engine/Modules/NumericExpression/Source/NumericExpression.cpp` (tinyexpr) | M (ImGui editor UX) | S |
| Editor UI built on the engine's UI: MCP can query and click any editor widget (`get_panel_tree`, `click_element`, `send_key`, `input_text`), and the editor runs on the web | `mcp/src/tools/ui.ts`, `Apps/Editor/Source/DebugServer/*` | M (agent-driven editor QA) | XL by migration; M with ImGui Test Engine instead |
| Per-phase UI frame timings (cascade, layout, geometry, hit test) | `Include/UI/UIManager.h` frame stats, `Tests/UIEmitPerfTests.cpp` | L–M | S |
| Parallel cascade and text pre-measure on the job system | `Source/UIManager_Layout.cpp` (MT-4.2) | L (game UIs are small) | XL |
| Color-space-aware UI compositing (SDR UI into HDR or encoded targets, paper white) | `Source/UITargetSpace.cpp`, `UITextureSpace.cpp` | L now, M once HDR output exists | M |
| Typed C++ style overrides with automatic layout/visual dirtying | `Include/UI/StyleOverrides.h`, `StyleProp.h` | L (our C++ goes through `UIElement` + CSS text) | M |
| Word segmentation for double-click and Ctrl+arrows measured against Chrome | `Source/TextSegmentation.cpp` | L | S |
| Thread-safe posting into the UI thread (handles, coalescing) | `Source/UiDispatcher.cpp`, `UiPostTarget.cpp`, `UiCoalescedPost.cpp` | L | M |
| Editor UI design tokens and guidelines doc | `docs/contributing/editor-ui-guidelines.mdx`, `Apps/Editor/Assets/UI/theme/tokens.css` | L | S |

Gaps neither side has: accessibility (screen reader or platform accessibility trees), UI
localization (string tables, plurals), and a WYSIWYG UI builder.

---

## 4. Things THEIRS does better (and what OURS does better)

### 4.1 THEIRS better
1. **Rendering cost**:
   - Theirs: the whole UI is one `Draw(6, N)` call (or one per texture run); rounded corners,
     borders and shadows are analytic SDFs in the fragment shader; a transform change rewrites one
     slot. Evidence: `UIManager_RenderRG.cpp:350-385`, `UIPrimitive.h`.
   - Ours: RmlUi compiles per-element tessellated meshes and issues one `RenderGeometry` per mesh,
     plus stencil clip masks and layer push/pop (`RmlRenderer.cpp:1327,1413,2267` count draws). That
     is fine for HUDs and menus, but would not hold at editor scale.
2. **Text quality under scaling**:
   - Slug glyphs stay exact under any transform or zoom, and their coverage filter is tuned to
     Chrome (`slug_functions.glsl` header).
   - Our glyph atlas rasterizes at a scale quantized to 0.25 steps (`HtmlUIContextRender.cpp:104-108`).
     A `transform: scale()` pop-in animation scales bitmaps until the next regeneration.
3. **Widget depth for data-heavy UI**:
   - Virtualized List/Grid/Tree/Table views, column headers, drag-drop payloads, tooltips, context
     menus, search dialogs, docking.
   - These are hardened by a 313k-line editor that uses them daily. Ours has the browser form
     controls plus whatever authors write in HTML+Lua; `data-for` instantiates every row.
4. **Hot reload without losing runtime state**: they reconcile the existing tree; we rebuild after
   a snapshot (`UIHotReload.cpp:157-453` vs `UIDocumentSystemReload.cpp:93-240`).
5. **No fork to maintain**:
   - Yoga and lexbor are used upstream as-is, and all of their UI code is their own.
   - We carry a large RmlUi fork (FORK.md lists patches from phase 1 through phase 6, with
     P4-G1…G14, P5-1…6 and P6-J/L) and a rewrite layer gated by `ForkFeature` flags. Every RmlUi
     upgrade is costly.
6. **Agent reach into the editor**: because the editor uses their UI, their MCP can query and click
   any panel. Our MCP only reaches HTML UI documents and typed engine/editor tools; ImGui widgets
   cannot be addressed.
7. **Parity measured inside the tests**: Chrome values are written into the asserts, with comments
   (for example `TextSegmentation.cpp:16-30` records "measured Chrome/150").

### 4.2 OURS better
1. **Standards coverage**, by a wide margin:
   - Theirs has no grid, inline formatting context, tables, floats, fixed/sticky positioning,
     `@media`, `@keyframes`, CSS gradients, `filter`/`backdrop-filter`/masks, 3D transforms,
     `::before`/`::after`, `:has`, or mixed-unit `calc()`.
   - We have all of these: the Taffy grid port, `MediaQuery.cpp`, FORK.md phase 3, and `docs/html-ui/phase-3-fork-patches.md`.
   - We also take real HTML5 (lexbor) and nested CSS. This matches the "LLM-written web CSS" goal.
2. **Reactive model**:
   - We have RmlUi data models driven from Lua tables, a browser-named Lua DOM, `<script type="text/lua">`
     and an Anime.js-style timeline/stagger/spring API.
   - Theirs is imperative C++/C# with no binding.
3. **Motion and media**: keyframes, Lottie (ThorVG), `<video>` (dav1d/opus/webm), and a shader
   effects library with stateful simulations. Theirs has transitions only.
4. **International text**: SheenBidi bidi, UAX #14 line breaking across inline boundaries,
   justification, `-webkit-line-clamp`, and grapheme-aware ellipsis. Theirs has no bidi.
5. **Game-specific input and placement**:
   - gamepad spatial navigation, and pointer pass-through between stacked documents;
   - render-to-texture world-space UI with UV raycasts (`UIWorldRaycast.cpp`); theirs has only
     Overlay/Fullscreen screen-space documents.
6. **Portability**: our renderer runs on WebGL2, GLES3 and D3D11 through sokol. Theirs needs
   SSBOs + bindless (Vulkan, Metal, WebGPU), with a run-split fallback.
7. **Authoring diagnostics and tooling**:
   - every dropped or rewritten feature reports `file:line:col` with a stable code; `check --ui`; a
     generated support matrix;
   - importers (Google Fonts and others), built-in Lucide icons, an example library;
   - the RmlUi debugger plus `inspect_ui`/`assert_ui`.
8. **Visual regression testing**: 661 golden images plus automated Chromium references through
   playwright. Theirs has numeric asserts and manual PowerShell sampling, with no image goldens.
9. **SVG fidelity**: LunaSVG was chosen by benchmark (+12.4 points on the resvg suite,
   `docs/html-ui/svg-benchmark.md`); theirs uses ThorVG.

---

## 5. Copy/port candidates (ranked by value / effort)

### 5.1 Markdown parser → HTML emitter (value M, effort S)
- Source: `Engine/Modules/UI/Source/Parsers/MarkdownParser.{h,cpp}` (539 lines),
  `MarkdownInlineParser.cpp` (686), `MarkdownDocument.h` (69), `MarkdownSyntax.h` (55).
  Tests: `Tests/MarkdownParserTests.cpp` (351).
- Dependencies: only the C++ standard library.
- Adaptation: replace C++20 defaulted `operator==` (`MarkdownDocument.h`) with explicit operators
  for our C++17 build; rename the namespace. Then write about 150 lines that turn
  `MarkdownBlock`/`MarkdownSpan` into HTML (`h1..h6, ul/ol/li, pre/code, blockquote, hr, table, a, strong, em`)
  and expose it in one of two ways:
  - as a compat element transform (`<markdown src="…">` or inline text), styled by our UA sheet; or
  - as a Lua `element:setMarkdown(str)`.
- The same AST can feed a small ImGui renderer for editor docs or AI panes.
- License: MIT; credit OpenEngine in NOTICE.md.

### 5.2 `NumericExpression` for ImGui number fields (value M, effort S)
- Source: `Engine/Modules/NumericExpression/Source/NumericExpression.cpp` (68 lines) and
  `Include/NumericExpression/NumericExpression.h`: `FilterNumericExpressionInput` and
  `TryEvaluateNumericExpression`.
- Dependency: **tinyexpr** (zlib). They take it from vcpkg, so vendor upstream tinyexpr at
  `libs/tinyexpr` with its zlib notice.
- Adaptation: wrap the editor's `ImGui::InputFloat`/`DragFloat` helpers (63 `DragFloat`/`DragScalar`
  sites; better to hook them once in `editor/window/Widgets.h`) so a committed text input is
  evaluated as an expression.

### 5.3 Virtualization core → `<virtual-list>` / Lua helper (value H, effort M)
- Source: `Include/UI/Controls/VirtualWindowCore.h` (147 lines) and `Source/Controls/VirtualWindowCore.cpp`
  (241), plus `Include/UI/VirtualizationCoordinator.h`. Tests: `Tests/VirtualWindowCoreTests.cpp`
  and `UIScrollVirtualizationTests.cpp` (as a reference).
- Dependencies: the C++ standard library and `std::span` (C++20; replace with pointer + size). The
  core deliberately knows nothing about `UIElement`.
- Adaptation: implement `Host` over a pool of RmlUi elements inside a scroll container. Absolutely
  position the slot rows (or use spacer padding), rebind on scroll through a Lua callback
  `bind(slotElement, index)` or a data-model row, and expose it as a compat element plus a Lua API.
- Their `ListView.cpp` (1.9k lines) is a reference for selection, keyboard handling and drag
  hooks, not code to lift.
- License: MIT.

### 5.4 Tooltip and popup placement → runtime emulation of `title` (value M, effort S–M)
- Source to port as **logic**, not code (both depend on `UIElement`/`UIManager`):
  - `Source/Layout/DropdownPopupPlacement.cpp` (98 lines: preferred side, flip when there is no
    room, clamp to the viewport);
  - `Source/Interaction/TooltipOverlay.cpp` (549 lines: show delay, hide when the pointer leaves or
    on a press, reposition, one shared overlay).
- Adaptation: in `UIDocumentSystemInput`, after a hover delay on an element with `title`, show a
  UA-styled `<div class="htmlui-tooltip">` in a top layer and place it with the ported algorithm.
  Flip `ElementTable.cpp:231` from "dropped" to "emulated", and add goldens.
- License: MIT.

### 5.5 SDF box functions for the legacy component UI and effects (value M, effort M)
- Source: `Engine/Modules/Rendering/Shaders/UI/sdf_functions.glsl` (338 lines: elliptical-corner
  rounded box, per-side border, box shadow, glow) and the needed helpers from `ui_sdf_common.glsl`
  and `ui_derivatives.glsl`. These are pure GLSL with no SSBO dependency in the functions themselves.
- Adaptation:
  - Add rounded corners, borders and shadows to `ImageComponent`/`PanelComponent` in
    `engine/shaderlib/ui.frag`, as uniforms per draw.
  - Later, batch `UISystem` with **instanced vertex attributes** (9 × vec4 per instance fits
    sokol's 16-attribute limit) instead of SSBOs, so it keeps working on WebGL2.
- License: MIT.

### 5.6 (Strategic, XL) Slug vector text as an `RmlFontEngine` back end (value M–H)
- Source: `Engine/Modules/Text/Source/FontAtlas_Slug.cpp` (542 lines; outline decomposition and
  curve/band packing) and `SlugGlyphData.h`, plus the shaders `slug_functions.glsl` (376),
  `text_effects.glsl` (144) and `text_mask_gamma.glsl` (187).
- Dependencies:
  - FreeType outline decomposition (`FT_Outline_Decompose`). Replace it with
    `stbtt_GetGlyphShape` and keep their cubic→quadratic reduction (line 79).
  - Textures: RGBA16F (curves) and RG16UI (bands), read with `texelFetch`. Both work on GLES3/WebGL2.
- Integration: RmlUi's `GenerateString` produces textured meshes. Recognize a "Slug" texture handle
  in `RmlRenderer` and carry the per-glyph band/em data in the vertex texcoords or a side buffer,
  with a new `ShaderType` variant. Our CPU font effects could then become GPU text effects.
- License: their port is MIT and cites Lengyel's MIT reference HLSL. Slug was patented
  (US 10,373,352); Lengyel has reportedly released it. **Confirm the patent status before shipping.**

### 5.7 Smaller adopt-the-idea items
- Per-phase UI timing stats added to `HtmlUIRenderStats` (style, layout, render ms), modelled on
  `UIEmitPerfTests.cpp` (S).
- The `title`/tooltip and drag-payload API designs (`Payload.h`, `DropTarget.h`) as templates for a
  Lua `dataTransfer`-like object on our drag events (M).
- An editor UI guidelines and tokens doc for our ImGui theme, modelled on
  `docs/contributing/editor-ui-guidelines.mdx` (S).

---

## 6. Not worth copying (and why)

- **Their layout and CSS core** (Yoga bridge, lexbor stylesheet parsing, the cascade in `CSSParser.cpp`).
  Ours is strictly broader: grid, inline formatting, tables, media queries, keyframes and filters.
  We moved off lexbor's CSS module on purpose, because it lacks nesting and exact source positions
  (DESIGN.md, deviation 4). Swapping would be a regression.
- **Migrating our editor from ImGui to HTML UI, as they did.** RmlUi with per-element meshes,
  `data-for` without virtualization, and no docking or tree/table widgets would need years of the
  widget work they have done. ImGui docking and viewports already cover the editor. For agent
  reach, the ImGui Test Engine or an ID-stack query is far cheaper.
- **Docking** (`Layout/Docking.cpp`, `DockspaceElement`, `Apps/Editor/Source/Docking`). ImGui
  docking already does this.
- **Editor panels and inspectors** (`Apps/Editor/Source/Panels`, `Inspectors`, `Source/UI`). They are
  tied to their `UIElement` tree and C# ABI; our ImGui equivalents exist.
- **MarkupECS and the editor's Markups code**. These are scene-review annotations, not UI. If they
  interest us, they belong to the world-building comparison (03).
- **ThorVG SVG rasterization.** Our benchmark chose LunaSVG for correctness, and ThorVG is already
  used for Lottie.
- **The ChromeReference PowerShell pixel-sampling scripts.** Our playwright + Chromium pipeline and
  golden images are more automated.
- **The C# UI wrapper layer and the native OS context-menu and dropdown bridge.** We script in Lua,
  and games do not need OS menus.
- **`TextSegmentation.cpp`.** RmlUi's text input already navigates by word; the gain is a few
  Chrome edge cases (dots, colons).
- **The parallel cascade (MT-4.2).** Game documents are small, and RmlUi's single-threaded update is
  not a bottleneck in our perf gates.
- **The SSBO/bindless single-draw pipeline as-is.** It cannot run on our WebGL2/GLES3 targets. Take
  only the SDF math (5.5).
