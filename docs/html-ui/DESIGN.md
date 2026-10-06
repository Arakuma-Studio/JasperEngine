# HTML/CSS UI: design

This is the binding design for the HTML/CSS UI system. The phase specs
(`phase-0-foundation.md` … `phase-5-effects.md`) detail each phase; `BRIEF.md` holds the
rules for the agents implementing them. When a phase spec and this file disagree, fix the
spec.

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
  │ Compatibility layer  (phase 2)    │  lexbor HTML5 parse -> RML; CSS -> RCSS rewrite;
  │ engine/core/htmlui/compat/        │  user-agent stylesheet; diagnostics
  └───────┬───────────────────────────┘
          │ RML + RCSS
  ┌───────▼───────────────────────────┐
  │ RmlUi fork  (engine/libs/rmlui)   │  style, layout, events, animation, data binding
  │ + our patches (phase 3)           │  calc(), grid, ::before/::after, border-style …
  └───────┬───────────────────────────┘
          │ RenderInterface / FontEngineInterface / SystemInterface / FileInterface
  ┌───────▼───────────────────────────┐
  │ Engine backends  (phases 0-1)     │  sokol renderer, STBText fonts, input, files,
  │ engine/core/htmlui/               │  ECS component + system, Lua API, ThorVG
  └───────┬───────────────────────────┘
          ▼
   sokol_gfx: GL core / GLES3 (WebGL2, Android) / D3D11 / Metal / Vulkan
```

## Names and locations

| Thing | Name / path |
|---|---|
| Third-party layout engine | RmlUi **6.3** (tag `6.3`), vendored at `engine/libs/rmlui/` (Core, Debugger, SVG/Lottie plugins only as needed). Static library, hidden visibility, no FreeType (`RMLUI_FONT_ENGINE=none`). |
| Fork bookkeeping | `docs/html-ui/FORK.md`: upstream commit/tag, list of every patch (file, reason, test). Fork commits are prefixed `rmlui:`. |
| HTML5 parser | lexbor (Apache-2.0) at `engine/libs/lexbor/` (only the modules we need). |
| Vector graphics | ThorVG (MIT, CPU renderer) at `engine/libs/thorvg/` for Lottie, and for SVG unless the phase-0 benchmark decides otherwise. |
| Engine backend code | `engine/core/htmlui/` (`RmlRenderer`, `RmlFontEngine`, `RmlSystem`, `RmlFile`, `HtmlUIContext`, …), compatibility layer in `engine/core/htmlui/compat/`. |
| ECS component | `UIDocumentComponent` (`engine/core/component/UIDocumentComponent.h`), named after Unity UI Toolkit's UIDocument (the engine uses Unity names, see `docs/unity-glossary.md`). |
| ECS system | `UIDocumentSystem` (`engine/core/subsystem/UIDocumentSystem.{h,cpp}`), registered in `Scene::init` after `UISystem`. |
| GameObject wrapper | `UIDocument` (`engine/core/object/ui/UIDocument.{h,cpp}`) |
| Lua API | `UIDocument`, `UIElement`, `UIEvent` classes; animation API modelled on Anime.js v4 (`animate`, `stagger`, `createTimeline`, `createSpring`, `utils`) in a global table `anime`. Bindings in `engine/core/script/binding/HtmlUIClassesLua.cpp`. |
| Docs for users and agents | `docs/mcp/guide/html-ui.md` (+ generated support matrix `docs/mcp/guide/html-ui-support.md`). |
| Tests | C++: `engine/tests/htmlui/` (googletest), built as `jasperengine-htmlui-tests`; golden images: `engine/tests/htmlui/golden/`; MCP: `tools/mcp/tests/test_html_ui*.py`. |

The existing component UI (`UISystem`, `RectTransform`, `Image`, `Button`, …) stays as it is
and keeps working; both systems coexist in the same scene.

## Key decisions

### Documents and scenes
- A `UIDocumentComponent` on a GameObject loads one HTML file (project-relative path) into an
  RmlUi context owned by the scene's `UIDocumentSystem` (one `Rml::Context` per scene; one
  document per component).
- Screen-space by default: drawn over the scene in the camera pass of the scene the
  component lives in, sorted by `sortOrder` and in front of the existing component UI.
  Optional render-to-texture mode (`targetTexture`), so a document can be shown on a mesh
  (world-space UI) or inside an `Image`.
- Coordinates: CSS pixels map to canvas pixels; `dp`/`dppx` ratio comes from the canvas to
  screen scale (`Engine::Scaling`, `computeViewLayout`). Media queries see the canvas size.

### Rendering
- Implemented on the engine's render abstraction (`engine/core/render/*` → `engine/renders/sokol/*`),
  never on raw GL/Metal/D3D, so one implementation covers every backend.
- RmlUi gives premultiplied sRGB colors: dedicated UI shaders blend premultiplied
  (`ONE, ONE_MINUS_SRC_ALPHA`), without the linear conversion the old `ui.frag` does.
- Each document renders into an offscreen color+stencil target (MSAA when available), then
  is composited into the camera pass. This satisfies RmlUi's layers (filters, opacity,
  masks), stencil clip masks for `border-radius`/transforms, and sokol's Vulkan
  one-swapchain-pass rule.
- Pipeline blend/stencil/write-mask state must become configurable in the render
  abstraction (it is hard-coded per `PipelineType` in `SokolObject::makePipelineDesc`).
- Gradients, filters (blur, drop-shadow, color matrix), masks and effect decorators are
  precompiled shader variants registered as `ShaderType`s (engine shaders live in
  `engine/shaderlib/`, cross-compiled by the editor's ShaderBuilder; exports ship `.sdat`).

### Text
- Our own `Rml::FontEngineInterface` on top of the engine's text stack (`STBText`:
  stb_truetype + HarfBuzz + SheenBidi, glyph atlas, fallback chains). No FreeType.
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
  `outElastic(1, .6)`), driving element styles per frame; CSS transitions/keyframes are
  handled by RmlUi.

### Input
- `Engine::system*` event functions feed the scene's `UIDocumentSystem` next to `UISystem`;
  when a document consumes an event, the game callbacks don't receive it
  (`ignoreEventsHandledByUI`). Mouse wheel, key up, text input, enter/leave and multi-touch
  reach documents; keys are offered to the focused document before the game.
- Platform additions: clipboard API in `System` (all platforms), IME composition where the
  platform layer supports it.

### Agent tooling (on top of the existing MCP server, `docs/mcp/`)
- `jasperengine-editor check --ui` runs the compatibility diagnostics (same code as runtime).
- MCP tools to inspect a live document (element tree, computed styles and boxes) and to
  assert layout; hot reload of documents through `McpAutoSync`; screenshots already exist.
- The supported-feature matrix is generated from the compat layer's property tables, so docs
  cannot drift from the code.

## Phases

| Phase | Spec | Outcome |
|---|---|---|
| 0 | `phase-0-foundation.md` | RmlUi vendored and building everywhere; core render interface on sokol; STBText font engine; a `UIDocumentComponent` draws an HTML document in a scene; test harness (unit + golden images); SVG library benchmark and decision. |
| 1 | `phase-1-integration.md` | Full renderer (stencil clip masks, layers, filters, gradients, transforms, MSAA); complete input; editor integration (Inspector, Hierarchy, serialization, export, hot reload); Lua element API + data binding; SVG and Lottie via ThorVG; animation API; render-to-texture. |
| 2 | `phase-2-compat.md` | HTML5 → RML, user-agent stylesheet, CSS rewriting, diagnostics, importers (Google Fonts, LottieFiles, Lucide), `currentColor` icons, web-component mappings. |
| 3 | `phase-3-fork-patches.md` | RmlUi patches: `calc()`/`min()`/`max()`/`clamp()`, border styles, `%` radii, `::before`/`::after`, `:root`/`:is()`/`:where()`, real `position: fixed`, group opacity, `cubic-bezier()`/`steps()`, `!important`, `inherit`/`initial`/`unset`, CSS Grid. |
| 4 | `phase-4-agent-tooling.md` | `check --ui`, MCP tools (inspect, assert), auto-sync reload, generated support matrix and guide, example library, MCP tests. |
| 5 | `phase-5-effects.md` | UI effects library through RmlUi shader decorators, effects ported from shaders.com (MIT). |

Phases run in order; within a phase, independent work packages run in parallel git
worktrees (see `BRIEF.md`). A phase is merged into `main` only when its acceptance tests
pass.

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
- Vector: nanosvg is used by `TextureData.cpp` for `.svg` textures.
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
