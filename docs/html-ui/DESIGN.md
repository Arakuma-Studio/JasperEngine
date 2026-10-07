# HTML/CSS UI: design

This is the binding design for the HTML/CSS UI system. The phase specs
(`phase-0-foundation.md` … `phase-5-effects.md`) detail each phase; `BRIEF.md` holds the
rules for the agents implementing them; `CONTRACTS.md` lists every interface one phase
provides to another (name, provider, consumers). When a phase spec and this file disagree,
fix the spec. The specs were reconciled against each other in October 2026; the deviations
from the first version of this file that the specs justified are recorded below, in place,
and summarised in "Recorded deviations".

## Goal

Game UI is written in **standard HTML and CSS**, the way a web developer or a coding agent
already writes it, and is drawn by the engine's own GPU renderer on every platform the
engine supports (Linux, Windows, macOS, Android, iOS, Web). No browser, no webview, no
runtime network access, no JavaScript engine.

Principles:

1. **The authoring format is standard HTML/CSS, not RML.** RmlUi's dialect (RML/RCSS) is an
   internal detail. Whatever we can rewrite, we rewrite; whatever RmlUi cannot do, we patch
   in our fork. A UI written for a browser should look the same in the engine within the
   documented subset.
2. **Never fail silently.** Every property, value, selector, element or attribute that is
   dropped, rewritten with a loss, or unsupported is reported with `file:line:column` and a
   suggested replacement, at load time (Console) and by `jasperengine-editor check --ui`.
3. **Swappable core.** Games talk to `UIDocument` / the Lua element API / HTML+CSS files,
   never to RmlUi types, so a different layout engine (for example Blitz, once mature) can
   replace RmlUi without rewriting game UIs.
4. **Shipped games compile nothing at runtime**: shaders are precompiled variants registered
   in the engine's shader system, fonts/images/SVG/Lottie referenced by a document are
   project assets (remote ones are imported into the project by the editor).
5. **Everything is tested headless**: parser/compat unit tests, golden-image render tests,
   MCP end-to-end tests with screenshots.

## Architecture

```
  ui/menu.html + ui/menu.css             standard HTML/CSS (+ SVG, Lottie, fonts)
          │
  ┌───────▼───────────────────────────┐
  │ Compatibility layer  (phase 2)    │  lexbor HTML5 parse -> RML; own CSS Syntax 3 parser,
  │ engine/core/htmlui/compat/        │  CSS -> RCSS rewrite gated by fork features;
  │                                   │  user-agent stylesheet; diagnostics; runtime emulations
  └───────┬───────────────────────────┘
          │ RML + RCSS
  ┌───────▼───────────────────────────┐
  │ RmlUi fork  (engine/libs/rmlui)   │  style, layout, events, animation, data binding
  │ + our patches (phases 1, 3, 5)    │  calc(), grid, ::before/::after, border-style, cascade,
  │                                   │  text, animations … (phase 3); SVG on ThorVG (phase 1)
  └───────┬───────────────────────────┘
          │ RenderInterface / FontEngineInterface / SystemInterface / FileInterface
  ┌───────▼───────────────────────────┐
  │ Engine backends  (phases 0-1)     │  sokol renderer, font engine on the STBText stack,
  │ engine/core/htmlui/               │  input, files, ECS component + system, Lua API,
  │                                   │  ThorVG, UI clock; effects library (phase 5)
  └───────┬───────────────────────────┘
          ▼
   sokol_gfx: GL core / GLES3 (WebGL2, Android) / D3D11 / Metal / Vulkan
```

## Names and locations

| Thing | Name / path |
|---|---|
| Third-party layout engine | RmlUi **6.3** (tag `6.3`), vendored at `engine/libs/rmlui/`: Core and Debugger (phase 0), SVG plugin (vendored and switched to ThorVG by phase 1), RmlUi's `Tests/` (phase 3, for the fork's own unit tests). Lottie and Lua plugins are not vendored. Static library, hidden visibility, no FreeType. |
| Fork bookkeeping | `docs/html-ui/FORK.md`: upstream commit/tag, list of every patch (file, reason, test). Fork commits are prefixed `rmlui:`. |
| HTML5 parser | lexbor 3.1.0 (Apache-2.0) at `engine/libs/lexbor/` (core, dom, html, ns, tag + ports; one source-offset patch). CSS is parsed by the compat layer's own CSS Syntax 3 tokenizer/parser (lexbor's CSS module lacks nesting and exact positions). |
| Vector graphics | ThorVG (MIT, CPU renderer) at `engine/libs/thorvg/` for Lottie (the engine's own `<lottie>` element) and for SVG (RmlUi's SVG plugin and `TextureData`) unless the phase-0 benchmark decides otherwise; nanosvg is removed in phase 1. |
| Engine backend code | `engine/core/htmlui/` (`HtmlUI`, `RmlRenderer`, `RmlFontEngine`, `RmlSystem`, `RmlFile`, `HtmlUIContext`, `HtmlUIClock`, …), compatibility layer in `engine/core/htmlui/compat/`, effects in `engine/core/htmlui/effects/`. |
| ECS component | `UIDocumentComponent` (`engine/core/component/UIDocumentComponent.h`), named after Unity UI Toolkit's UIDocument (the engine uses Unity names, see `docs/unity-glossary.md`). Fields: `filename` (asset-relative `.html`; the name MCP path validation keys on), `sortOrder`, `enabled` (phase 0); `interactive`, `renderMode` (`SCREEN`/`TEXTURE`), `textureWidth`/`textureHeight`, `inputSurface`, `pixelRatio`, `onLoad`/`onUnload` (phase 1). YAML/MCP name `uidocument`. |
| ECS system | `UIDocumentSystem` (`engine/core/subsystem/UIDocumentSystem.{h,cpp}`), registered in `Scene::init` after `UISystem`. |
| GameObject wrapper | `UIDocument` (`engine/core/object/ui/UIDocument.{h,cpp}`); the public element API headers sit next to it (`UIElement.h`, `UIEvent.h`, `UIValue.h`, `UIDataModel.h`) and include no RmlUi header. |
| Lua API | `UIDocument`, `UIElement`, `UIEvent`, `UIDataModel` classes; animation API modelled on Anime.js v4 (`animate`, `stagger`, `createTimeline`, `createSpring`, `utils`) in a global table `anime`. Bindings in `engine/core/script/binding/HtmlUIClassesLua.cpp`. Documents may also carry `<script type="text/lua">` (run per document after load with `document` bound, phase 2). |
| Docs for users and agents | `docs/mcp/guide/html-ui.md`, created by phase 0 (p0-integrate), extended by every later phase, reorganised by phase 4 (p4-guide), plus an "Effects" section from phase 5; the support matrix `docs/mcp/guide/html-ui-support.md` is generated by phase 4 (`tools/mcp/gen_html_ui_support.py` over `jasperengine-editor ui-support`) from phase 2's support tables; `docs/mcp/guide/html-ui-effects.md` is generated by phase 5. |
| Tests | C++: `engine/tests/htmlui/` (googletest), target `jasperengine-htmlui-tests`, built only by the standalone engine build and run headless by `tools/build.sh htmlui-tests` (`HTMLUI_BACKEND=glcore|vulkan`, lavapipe in the Linux Docker image); goldens in `engine/tests/htmlui/golden/` (phase 0 at the top level, then `p1/`, `compat/`, `browser/`, `examples/`, `effects/`, `property/`), regenerated with `JASPER_HTMLUI_UPDATE_GOLDEN=1`; browser references by phase 2's `tools/htmlui/render-reference.mjs` (playwright + Chromium headless shell 153, on the host only), extended by phase 3; MCP: `tools/mcp/tests/test_html_ui*.py`. |
| Logs and diagnostics | Every HTML UI Console line starts with `[HTML UI] `. Compat diagnostics have stable codes `HTMLUI-<AREA>-NNNN` (areas `GEN HTML CSS SEL AT IMP SVG`, phase 2; `FX`, phase 5) with a kebab-case registry name, printed as `path:line:col: severity: message (fix: suggestion) [HTMLUI-CSS-0004]`. |

The existing component UI (`UISystem`, `RectTransform`, `Image`, `Button`, …) stays as it is
and keeps working; both systems coexist in the same scene.

## Key decisions

### Documents and scenes
- A `UIDocumentComponent` on a GameObject loads one HTML file (asset-relative `filename`) into
  an RmlUi context owned by the scene's `UIDocumentSystem` (one `Rml::Context` per scene,
  created lazily when the scene has a document; one document per component).
- Screen-space by default: drawn over the scene in the camera pass of the scene the
  component lives in, sorted by `sortOrder` (numeric `z-index` inside the context) and in
  front of the existing component UI.
- Render-to-texture mode (`renderMode = TEXTURE`, `UIDocument::getTargetTexture()`), so a
  document can be shown on a mesh (world-space UI, pointer input through a UV hit test) or
  inside an `Image`. Each texture-mode document gets its own context (its own size, target and
  input space).
- HTML root mapping (phase 2, final): the HTML `<html>` element becomes the RML document
  element (`<body id="htmlui-root">`, so `rem`, `:root` and `html {}` rules behave as in CSS
  with no fork patch) and the HTML `<body>` its child `<htmlbody>`. Every author selector gets
  the `#htmlui-root` ancestor ("root band"), which ranks all author rules above the
  user-agent stylesheet. Phase 3 builds on this mapping and does not change the fork's
  document tag.
- Coordinates: CSS pixels map to canvas pixels; `dp`/`dppx` ratio comes from the canvas to
  screen scale (`Engine::Scaling`, `computeViewLayout`). Media queries see the canvas size.

### Rendering
- Implemented on the engine's render abstraction (`engine/core/render/*` → `engine/renders/sokol/*`),
  never on raw GL/Metal/D3D, so one implementation covers every backend.
- RmlUi gives premultiplied sRGB colors: dedicated UI shaders blend premultiplied
  (`ONE, ONE_MINUS_SRC_ALPHA`), without the linear conversion the old `ui.frag` does.
- Each **context** (not each document) renders into one offscreen color+stencil target
  (MSAA when available; phase 0 plain RGBA8, phase 1 depth-stencil + MSAA + layer stack),
  then is composited into the camera pass (in a UI/2D camera as a world-space canvas quad, so
  the editor Scene view shows it). `Context::Render` draws all of a context's documents in
  one call, so per-document targets would need a context per document and lose
  cross-document focus/hover; one target also saves a full-screen target per document. This
  satisfies RmlUi's layers (filters, opacity, masks), stencil clip masks for
  `border-radius`/transforms, and sokol's Vulkan one-swapchain-pass rule: all UI passes run
  at the end of `RenderSystem::drawFramePasses()`, before any camera pass. Documents stay
  inside the camera colour pass like component UI (post-process applies to them).
- Pipeline state is configurable in the render abstraction: a per-object `BlendState`
  override for fixed pipeline types (phase 0) and dynamic `PipelineState` pipelines through
  a pipeline cache (phase 1: blend, stencil, write mask, target format, sample count).
- Gradients, filters (blur, drop-shadow, color matrix) and masks are precompiled shader
  variants registered as `ShaderType`s (`HTMLUI`, `HTMLUI_GRADIENT`, `HTMLUI_POST`; engine
  shaders live in `engine/shaderlib/`, cross-compiled by the editor's ShaderBuilder; exports
  ship `.sdat`). The effects library uses one more type, `UI_EFFECT`, whose variants carry the
  effect's identity in the custom-shader id (built-in `@effects/<name>` and project `.frag`
  effects), drawn through RmlUi shader decorators **and** filters (phase 5).

### Text
- Our own `Rml::FontEngineInterface` on top of the engine's text stack (stb_truetype +
  HarfBuzz + SheenBidi; phase 0 splits `STBText` into shared `FontFace`, `FontRegistry`,
  `TextShaper`, `GlyphAtlas`, which `STBText` and `RmlFontEngine` both use). No FreeType.
  Font effects, synthetic bold/italic and a per-context raster scale come in phase 1.
- Font faces keyed by family, weight, style; `font-family` lists are real fallback chains;
  generic families (`sans-serif`, `serif`, `monospace`, `system-ui`) map to the built-in fonts.
- Web fonts referenced by URL (Google Fonts) are imported into the project by the editor as
  static TTF instances; the runtime only reads TTF/OTF.

### Scripting
- Lua is the scripting language (no JS engine). The element API uses browser names and
  semantics (`document:getElementById`, `querySelector(All)`, `addEventListener`,
  `classList`, `style`, `textContent`, `innerHTML`, `setAttribute`, `dataset`), plus RmlUi
  data binding (`data-model`, `data-for`, `{{ }}`, `data-event-*`) driven from Lua tables.
- C++ scripts get the same API through `UIDocument`.
- Animation: Anime.js v4-shaped API (timelines, stagger, springs, easing names such as
  `outElastic(1, .6)`), driving element styles (and custom properties) per frame; CSS
  transitions/keyframes are handled by RmlUi.
- One **UI clock** (`HtmlUIClock`, phase 1) drives RmlUi animations
  (`SystemInterface::GetElapsedTime`), Lottie, `anime` and effects: unscaled frame time (a
  pause menu animates while the game's time scale is 0), deterministic under a fixed frame
  delta, frozen while a host holds the simulation, with a fixed-time override for tests and
  tools.

### Input
- `Engine::system*` event functions feed the scene's `UIDocumentSystem` next to `UISystem`;
  when a document consumes an event, the game callbacks don't receive it
  (`ignoreEventsHandledByUI`). Mouse wheel, key up, text input, enter/leave and multi-touch
  reach documents; keys are offered to the focused document before the game.
- Platform additions: clipboard API and mouse cursor in `System` (all platforms), IME
  composition where the platform layer supports it.

### Agent tooling (on top of the existing MCP server, `docs/mcp/`)
- `jasperengine-editor check --ui` runs the compatibility diagnostics (same code as runtime,
  `compat::checkDocument`).
- MCP tools to inspect a live document (element tree, computed styles and boxes) and to
  assert layout; screenshots already exist.
- Hot reload has one engine entry point, `UIDocumentSystem::reloadChangedFiles` with
  per-document file stamps (phase 1); phase 1's editor watcher and phase 4's `McpAutoSync`
  pick-up both call it, so a change is reloaded once. Stylesheet-only changes keep the DOM.
- The supported-feature matrix is generated from the compat layer's support tables, so docs
  cannot drift from the code.

### Compatibility and the fork
- Phase 2 rewrites or emulates what RmlUi 6.3 cannot do; each rewrite that a fork patch can
  replace is gated by a `compat::ForkFeature` flag ("the fork has it"). Phase 3 implements the
  features in the fork and flips the flags (appending flags phase 2 did not have); the
  rewrite code stays so both states stay tested. Lucide icons are built into the engine
  (compressed pack); other remote resources (Google Fonts, stylesheets, images, LottieFiles)
  are imported by the editor into `assets/htmlui-imports/` with a manifest the runtime reads.

## Phases

| Phase | Spec | Outcome |
|---|---|---|
| 0 | `phase-0-foundation.md` | RmlUi 6.3 (Core, Debugger) vendored and building everywhere; core render interface on sokol with one offscreen target per context; text stack split (`FontFace`/`FontRegistry`/`TextShaper`/`GlyphAtlas`) and `RmlFontEngine`; a `UIDocumentComponent` draws an HTML document in a scene with pointer input; minimal editor/MCP registration; test harness (`jasperengine-htmlui-tests`, goldens, `tools/build.sh htmlui-tests`, lavapipe in Docker); SVG library benchmark and decision; first `html-ui.md`. |
| 1 | `phase-1-integration.md` | Full renderer (stencil clip masks, layers, filters, gradients, transforms, MSAA, pipeline cache) with the extension points phase 5 uses; complete input (keys, text, IME, wheel, touch, clipboard, cursor); editor integration (Inspector, Hierarchy, serialization, export check, hot reload with shared stamps, debugger); Lua element API + data binding; SVG (RmlUi SVG plugin vendored, on ThorVG) and Lottie via ThorVG, nanosvg removed; animation API; the UI clock; render-to-texture with world-space input; font effects, synthetic bold/italic. |
| 2 | `phase-2-compat.md` | lexbor HTML5 → RML with the root mapping, own CSS parser and CSS → RCSS rewriting gated by fork features, user-agent stylesheet, diagnostics with stable codes, support tables, runtime emulations, `<script type="text/lua">`, built-in Lucide icons, importers (Google Fonts, stylesheets, images, LottieFiles), `currentColor` icons, web-component mappings, Chromium reference tool. |
| 3 | `phase-3-fork-patches.md` | RmlUi fork patches, widened to what LLM-written CSS needs: `calc()`/`min()`/`max()`/`clamp()`, CSS values (colours, units, fonts, `line-height: normal`), CSS initial values, cascade (`!important`, CSS-wide keywords, `revert`), selectors (`:root`, `:is`, `:where`, `:has`, focus pseudo-classes, Level 4), `::before`/`::after`/`::marker`/counters, border styles and radii, `outline`, group opacity and stacking, native backgrounds/masks/`object-fit`, text properties, `position: fixed`/`sticky`, box/flex parity, CSS transitions/animations parity, CSS Grid (Taffy port); flips the matching phase-2 fork features. |
| 4 | `phase-4-agent-tooling.md` | `check --ui` (CLI and hook), `ui-support` + generated support matrix, `McpAutoSync` pick-up of UI changes, MCP tools (`inspect_ui`, `assert_ui`, `create_ui_document`, `send_input`/`import_files` extensions), example library, the reorganised guide, MCP tests. |
| 5 | `phase-5-effects.md` | UI effects library (`effect()` in CSS) through RmlUi shader decorators and filters, `ShaderType::UI_EFFECT`, project effects, effects ported from shaders.com (MIT); registered custom properties (`@property`). |

Phases run in order; within a phase, independent work packages run in parallel git
worktrees (see `BRIEF.md`). A phase is merged into `main` only when its acceptance tests
pass.

## Recorded deviations (accepted at spec reconciliation, October 2026)

Changes against the first version of this file, justified in the phase specs and now
binding (the sections above already say them in place):

| # | Deviation | Where justified |
|---|---|---|
| 1 | One offscreen target per **context** (per scene), not per document; documents ordered by numeric `z-index` | phase 0 D2, phase 1 §3.5 |
| 2 | Render-to-texture is `renderMode = TEXTURE` + `UIDocument::getTargetTexture()`, one context per texture document (not a `targetTexture` field) | phase 1 §3.8.1, §3.12 |
| 3 | HTML root → RML document element `body#htmlui-root`, HTML body → `htmlbody`, author rules ranked by the `#htmlui-root` root band; phase 3 keeps it (no fork base-tag change) | phase 2 §3.7.3, §3.8, §3.17; phase 3 §3.1.1 |
| 4 | lexbor for HTML only; CSS parsed by the compat layer's own CSS Syntax 3 parser | phase 2 §3.17 |
| 5 | Lucide icons built into the engine (compressed pack), not imported per project | phase 2 §3.11.8 |
| 6 | `<script type="text/lua">` in documents runs per document after load | phase 2 §3.7.3, §3.12.4 |
| 7 | Phase 3 scope widened (values, cascade, selectors, text, backgrounds, positioning, box/flex parity, animations, grid) and the fork's initial values switched to CSS initial values | phase 3 §2, §3.1.2 |
| 8 | `@property` (registered custom properties) is phase 5's, not phase 3's | phase 5 §3.11, §3.18 |
| 9 | Effects run as decorators **and** filters, with one `ShaderType::UI_EFFECT`; phase 1 has no effect registry, only extension points | phase 5 §3.18; phase 1 §3.3.3 |
| 10 | Fork patches outside phase 3: phase 1 (`Event::PreventDefault`, SVG plugin on ThorVG), phase 2 (SVG `currentColor` hook in the SVG plugin), phase 5 (`@property` var lookup, `-jfx-text-fill`); all recorded in `FORK.md` | phase 1 §3.14, phase 2 H8, phase 5 §3.18 |
| 11 | One UI clock (unscaled frame time) for RmlUi, Lottie, `anime` and effects | phase 1 §3.0 |
| 12 | Documents stay inside the camera colour pass (post-process applies to them); no compositing after post-process | phase 1 §3.5 |
| 13 | Hot reload: engine entry point `reloadChangedFiles` (phase 1) shared by the editor watcher and `McpAutoSync` (phase 4) | phase 1 §3.8.6, phase 4 §3.5 |
| 14 | The compat pipeline runs at document load in shipped games too (in-memory cache, no export-time precompilation); principle 4 concerns shaders and assets | phase 2 §3.12.2 |
| 15 | The RmlUi debugger is compiled only into the editor, Debug exports and the test build | phase 1 §3.8.7 |
| 16 | Excluded with reasons: X11 multi-touch, in-document IME preedit on Linux and in the editor Game view, Lottie expressions/audio, UV hits on skinned meshes (bind pose); CSS features of phase 3 §2.2 (e.g. `writing-mode`, `@container`, `subgrid`, `mix-blend-mode`) are diagnosed, not implemented | phase 1 §2, phase 3 §2.2 |

## Research facts (verified in code, October 2026)

Engine, for implementers (paths relative to the repo root):
- sokol_gfx only (`engine/libs/sokol/`). Backends: Web GLES3/WebGL2; Android GLES3; Windows
  glcore/d3d11/vulkan; Linux glcore/vulkan; macOS glcore/metal/vulkan; iOS metal
  (`engine/CMakeLists.txt` `GRAPHIC_BACKEND`). Editor: glcore/vulkan/metal.
- Existing UI draw path: `RenderSystem::loadUI/drawUI/destroyUI` (`RenderSystem.cpp` ~5438–5600),
  one draw per entity, scissor in `RenderSystem.cpp` ~9700–9830 (bottom-left origin,
  `CameraRender::applyScissor`), `engine/shaderlib/ui.vert/.frag` (linear conversion, non
  premultiplied).
- Pipeline state hard-coded per pipeline type: `engine/renders/sokol/SokolObject.cpp`
  `makePipelineDesc` (~265–395). Offscreen depth is `SG_PIXELFORMAT_DEPTH` (no stencil,
  `SokolTexture.cpp` ~479); swapchain is depth-stencil. Desktop MSAA sample count 1.
- Frame: `Engine::systemDraw` → each scene layer `draw()` (`Engine.cpp` ~1153); Vulkan allows
  one swapchain pass per frame, stacked scenes go through an offscreen composite
  (`Engine.cpp` ~725–745). Scene layers: `Engine::addSceneLayer`.
- Shaders: GLSL 450 in `engine/shaderlib/`; editor cross-compiles (glslang → SPIR-V →
  SPIRV-Cross) to GLSL 410 / GLSL 300 es / HLSL 5 / MSL 2.1 and caches `.sdat`; runtime only
  loads precompiled shaders (`ShaderPool.cpp`). New shader type touches `ShaderType`
  (`Render.h`), `ShaderBuilder::setupShaderArgs`, `ShaderPool.cpp` tables, `ShaderData.cpp`
  name maps, `ShaderVariantCollector.cpp`, `ExportWindow.cpp`.
- Text: `engine/core/util/STBText.{h,cpp}` (stb_truetype, HarfBuzz 14.3 amalgamated with
  `HB_NO_DRAW/HB_NO_COLOR`, SheenBidi), `FontPool`, built-in fonts `DefaultFont.h`,
  `DefaultFontArabic.h`. No FreeType anywhere.
- Libraries: vendored as source under `engine/libs/<lib>` with their own `CMakeLists.txt`,
  wired in `engine/CMakeLists.txt` (include dirs ~659, add_subdirectory ~690, warnings ~711,
  link ~907). The editor builds the engine as a shared library: vendored libs need hidden
  visibility (see `engine/libs/harfbuzz/CMakeLists.txt`). C++17 default.
- Input: `Engine::system*` (`Engine.h` ~455), UI dispatch loop in `Engine.cpp` ~1300–1500;
  wheel, key up, enter/leave never reach `UISystem`; `systemKeyDown` calls the game first.
  No clipboard API anywhere. IME: committed text only.
- ECS: systems registered in `Scene::init` (`Scene.cpp` ~28); components in
  `registry/EntityRegistry.cpp`; signature `std::bitset<64>` with 52 slots used. Adding a
  component also touches the editor's `Catalog.{h,cpp}`, `Stream.{h,cpp}` (YAML),
  `Factory.cpp` (generated C++ for export), `InspectorWindow.cpp`,
  `ComponentAddDialog.cpp`, `HierarchyWindow.cpp` (Create menu), Lua bindings.
- Files: `FileData`/`Data` with `asset://`, `data://`, `lua://` schemes, `ResourcePack`,
  Android `AAssetManager`, Web preload.
- Lua 5.4.7 + LuaBridge3, `engine/core/script/binding/*`; `FunctionSubscribe<>` callbacks
  accept Lua functions.
- Vector: nanosvg is used by `TextureData.cpp` for `.svg` textures (replaced by ThorVG in
  phase 1).
- MCP: `editor/mcp/` (server, tools in `editor/mcp/tools/*`), checks CLI in
  `editor/util/ProjectChecks.{h,cpp}`, tests in `tools/mcp/tests/` (harness
  `tools/mcp/harness.py`), docs in `docs/mcp/`.

RmlUi 6.3 (tested by parsing real declarations): rejects `border: 1px solid red`,
`display: grid`, `calc()`, `position: sticky`, `border-radius: 50%`,
`background: linear-gradient()`, `background-image: url()`, `text-shadow`, `outline`,
`rgb(0 0 0 / 50%)`, `transition: opacity 200ms ease`; silently misreads `rgba(…, 0.5)`
(alpha 0–255 scale) and `font-family: A, B` (one family named "A, B"); RML must be XML; no
user-agent stylesheet (everything `display: inline`); `fixed` acts like `absolute`;
`opacity` is not a group; no `::before/::after`, `:root`, `:is`, `:where`, `:has`,
`!important`, `inherit`; `:focus` propagates to ancestors; `rem` relative to body;
transitions only on class/pseudo-class changes; easing names only (`cubic-in-out`), seconds
only. Supports flexbox (no `order`), `gap`, `inset`, `var()` (6.3), transforms, filters,
`backdrop-filter`, `mask-image`, `box-shadow`, gradients via `decorator:`, `@keyframes`,
media queries (subset), data binding, Lua plugin, debugger. Lottie plugin uses rlottie
(last release 2020) and creates a new texture per frame; SVG plugin uses LunaSVG, supports
inline `<svg>` since 6.2, tints by multiplying with `image-color`.
