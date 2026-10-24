# HTML/CSS UI: design

This is the binding design for the HTML/CSS UI system. The phase specs
(`phase-0-foundation.md` … `phase-5-effects.md`, `phase-6-video.md`, `phase-6-stateful-effects.md`) detail each phase; `BRIEF.md` holds the
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
  │                                   │  text, animations … (phase 3); SVG on LunaSVG (phase 1)
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
| Third-party layout engine | RmlUi **6.3** (tag `6.3`), vendored at `engine/libs/rmlui/`: Core and Debugger (phase 0), SVG plugin (vendored by phase 1, on LunaSVG as upstream), RmlUi's `Tests/` (phase 3, for the fork's own unit tests). Lottie and Lua plugins are not vendored. Static library, hidden visibility, no FreeType. |
| Fork bookkeeping | `docs/html-ui/FORK.md`: upstream commit/tag, list of every patch (file, reason, test). Fork commits are prefixed `rmlui:`. |
| Video decoding (phase 6) | `<video>` plays AV1 + Opus WebM only, decoded in software by vendored C/C++ sources built by the engine's CMake and Xcode projects: dav1d 1.5.4 (BSD-2-Clause; its x86-64 NASM and aarch64/armv7 NEON asm with run-time CPU dispatch, C on the Web and 32-bit x86) at `engine/libs/dav1d/`, libopus 1.6.1 (BSD-3-Clause, decoder) at `engine/libs/opus/`, libwebm 1.0.0.32 `mkvparser` (BSD-3-Clause + patent grant) at `engine/libs/libwebm/`. No platform decoders, no FFmpeg in the engine; the editor converts other formats by running a user-installed FFmpeg. |
| HTML5 parser | lexbor 3.1.0 (Apache-2.0) at `engine/libs/lexbor/` (core, dom, html, ns, tag + ports; one source-offset patch). CSS is parsed by the compat layer's own CSS Syntax 3 tokenizer/parser (lexbor's CSS module lacks nesting and exact positions). |
| Vector graphics | Decided by the phase-0 benchmark (`docs/html-ui/svg-benchmark.md`): **LunaSVG** (MIT) at `engine/libs/lunasvg/` for SVG (RmlUi's SVG plugin and `TextureData`), because it is clearly more correct (resvg suite +12.4 points, Fluent emoji); **ThorVG** (MIT, CPU renderer) at `engine/libs/thorvg/` for Lottie only (the engine's own `<lottie>` element). nanosvg is removed in phase 1. |
| Engine backend code | `engine/core/htmlui/` (`HtmlUI`, `RmlRenderer`, `RmlFontEngine`, `RmlSystem`, `RmlFile`, `HtmlUIContext`, `HtmlUIClock`, …), compatibility layer in `engine/core/htmlui/compat/`, effects in `engine/core/htmlui/effects/`, `<video>` in `engine/core/htmlui/media/` (phase 6). |
| ECS component | `UIDocumentComponent` (`engine/core/component/UIDocumentComponent.h`), named after Unity UI Toolkit's UIDocument (the engine uses Unity names, see `docs/unity-glossary.md`). Fields: `filename` (asset-relative `.html`; the name MCP path validation keys on), `sortOrder`, `enabled` (phase 0); `interactive`, `renderMode` (`SCREEN`/`TEXTURE`), `textureWidth`/`textureHeight`, `inputSurface`, `pixelRatio`, `onLoad`/`onUnload` (phase 1). YAML/MCP name `uidocument`. |
| ECS system | `UIDocumentSystem` (`engine/core/subsystem/UIDocumentSystem.{h,cpp}`), registered in `Scene::init` after `UISystem`. |
| GameObject wrapper | `UIDocument` (`engine/core/object/ui/UIDocument.{h,cpp}`); the public element API headers sit next to it (`UIElement.h`, `UIEvent.h`, `UIValue.h`, `UIDataModel.h`) and include no RmlUi header. |
| Lua API | `UIDocument`, `UIElement`, `UIEvent`, `UIDataModel` classes; animation API modelled on Anime.js v4 (`animate`, `stagger`, `createTimeline`, `createSpring`, `utils`) in a global table `anime`. Bindings in `engine/core/script/binding/HtmlUIClassesLua.cpp`. Documents may also carry `<script type="text/lua">` (run per document after load with `document` bound, phase 2). |
| Docs for users and agents | `docs/mcp/guide/html-ui.md`, created by phase 0 (p0-integrate), extended by every later phase, reorganised by phase 4 (p4-guide), plus an "Effects" section from phase 5 and the "Video" and "Stateful effects (simulations)" sections from phase 6; the support matrix `docs/mcp/guide/html-ui-support.md` is generated by phase 4 (`tools/mcp/gen_html_ui_support.py` over `jasperengine-editor ui-support`) from phase 2's support tables; `docs/mcp/guide/html-ui-effects.md` is generated by phase 5. |
| Tests | C++: `engine/tests/htmlui/` (googletest), target `jasperengine-htmlui-tests`, built only by the standalone engine build and run headless by `tools/build.sh htmlui-tests` (`HTMLUI_BACKEND=glcore|vulkan`, lavapipe in the Linux Docker image); goldens in `engine/tests/htmlui/golden/` (phase 0 at the top level, then `p1/`, `compat/`, `browser/`, `examples/`, `effects/`, `property/`, `video/`), regenerated with `JASPER_HTMLUI_UPDATE_GOLDEN=1`; browser references by phase 2's `tools/htmlui/render-reference.mjs` (playwright + Chromium headless shell 153, on the host only), extended by phase 3; MCP: `tools/mcp/tests/test_html_ui*.py`. |
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
  inside the camera colour pass like component UI (post-process applies to them: since the
  shader library's post-processing, they are graded, antialiased and render-scaled with the
  scene, never in-shader coloured; "Integration with origin/main" below).
- Pipeline state is configurable in the render abstraction: a per-object `BlendState`
  override for fixed pipeline types (phase 0) and dynamic `PipelineState` pipelines through
  a pipeline cache (phase 1: blend, stencil, write mask, target format, sample count). The
  fixed types are built from the shader library's `RenderState` since the merge of
  origin/main; both share one `BlendMode`.
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
- Stacked documents: a page (a document, or an HTML document's `<body>`) that paints no
  background and whose document listens for no pointer event (on `<html>`/the document or
  `<body>`: click, dblclick, mouse*, wheel, contextmenu, pointer*, touch*, from Lua/C++
  `addEventListener` or `data-event-*`) is not hit itself; the pointer goes on to the next
  screen document below in z-order, then to the game (fork hook
  `Context::SetPointerPassThrough`, P4-1). A listening page takes the pointer on its empty
  parts, as a browser page does ("click outside closes the menu"). Hover, presses, wheel,
  touches and focus follow it; a press keeps its capture until its release.
- Gamepads reach the focused document first too (phase 4, p4-input): buttons, the left stick's
  directions and the triggers are `keydown`/`keyup` events (`code` `GamepadA`, `GamepadDPadUp`,
  … as in Edge's gamepad key events; `key` `ArrowUp`/`Enter`/`Escape` where the button stands
  for a keyboard key), with spatial navigation (or the element's `nav-*`) for the directions,
  Enter/Space activation with `:active` for A and a cancelable Escape for B; held directions
  repeat on the UI clock. While an element in the Tab order has the focus, the directions and A
  are the document's; other buttons only when a listener uses them; the rest is the game's.
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
| 1 | `phase-1-integration.md` | Full renderer (stencil clip masks, layers, filters, gradients, transforms, MSAA, pipeline cache) with the extension points phase 5 uses; complete input (keys, text, IME, wheel, touch, clipboard, cursor); editor integration (Inspector, Hierarchy, serialization, export check, hot reload with shared stamps, debugger); Lua element API + data binding; SVG (RmlUi SVG plugin vendored, on LunaSVG) and Lottie via ThorVG, nanosvg removed; animation API; the UI clock; render-to-texture with world-space input; font effects, synthetic bold/italic. |
| 2 | `phase-2-compat.md` | lexbor HTML5 → RML with the root mapping, own CSS parser and CSS → RCSS rewriting gated by fork features, user-agent stylesheet, diagnostics with stable codes, support tables, runtime emulations, `<script type="text/lua">`, built-in Lucide icons, importers (Google Fonts, stylesheets, images, LottieFiles), `currentColor` icons, web-component mappings, Chromium reference tool. |
| 3 | `phase-3-fork-patches.md` | RmlUi fork patches, widened to what LLM-written CSS needs: `calc()`/`min()`/`max()`/`clamp()`, CSS values (colours, units, fonts, `line-height: normal`), CSS initial values, cascade (`!important`, CSS-wide keywords, `revert`), selectors (`:root`, `:is`, `:where`, `:has`, focus pseudo-classes, Level 4), `::before`/`::after`/`::marker`/counters, border styles and radii, `outline`, group opacity and stacking, native backgrounds/masks/`object-fit`, text properties, `position: fixed`/`sticky`, box/flex parity, CSS transitions/animations parity, CSS Grid (Taffy port); flips the matching phase-2 fork features. |
| 4 | `phase-4-agent-tooling.md` | `check --ui` (CLI and hook), `ui-support` + generated support matrix, `McpAutoSync` pick-up of UI changes, MCP tools (`inspect_ui`, `assert_ui`, `create_ui_document`, `send_input`/`import_files` extensions), example library, the reorganised guide, MCP tests. |
| 5 | `phase-5-effects.md` | UI effects library (`effect()` in CSS) through RmlUi shader decorators and filters, `ShaderType::UI_EFFECT`, project effects, effects ported from shaders.com (MIT); registered custom properties (`@property`). |
| 6 | `phase-6-video.md`, `phase-6-stateful-effects.md`; fork notes in `phase-3-fork-patches.md` §3.10 (p6-justify, p6-line-clamp); record in `phase-6-acceptance.md` | HTML `<video>`: AV1 + Opus WebM decoded by vendored dav1d (with its asm)/libopus/libwebm on a worker thread, GPU YUV conversion, the UI clock with a frame-exact mode, A/V sync on SoLoud, the HTMLMediaElement API in Lua and C++, built-in controls, the editor's FFmpeg-based import conversion, `inspect_ui` `media`, the `video_menu` example (p6-video). Stateful effects: per-element persistent float state images advanced by fragment step and scatter passes, 18 simulations ported from shaders.com, splats from scripts; `RmlRenderer` records a frame's calls and replays them (p6-stateful-fx). `text-align: justify`, `text-align-last`, `text-justify`, `text-indent` (p6-justify); Chromium's `-webkit-line-clamp`, full UAX #14 line breaking across element boundaries, grapheme-aware ellipsis, per-cluster font fallback (p6-line-clamp). |

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
| 8 | `@property` (registered custom properties) is phase 5's, not phase 3's, because effect animation needs it: a fork patch (P5-4) plus compat, superseding phase 2's `body#htmlui-root` rewrite for the registered syntaxes, gated by `ForkFeature::RegisteredProperties`; registrations belong to the live document that made them | phase 5 §3.11, §3.18 item 2, §4.2.1 |
| 9 | Effects run as decorators **and** filters (custom `FilterInstancer`s: filter and backdrop processing and gradient text are impossible with decorators alone), with one `ShaderType::UI_EFFECT` whose custom-shader id carries the effect (library ids `@effects/<name>`, project `.frag` paths) instead of a `ShaderType` per effect (about 80 enum values and table rows; fork persistence and export reused); phase 1 has no effect registry, only extension points | phase 5 §3.18 items 1, 3, 6; phase 1 §3.3.3 |
| 10 | Fork patches outside phase 3: phase 1 (`Event::PreventDefault`), phase 2 (SVG `currentColor` hook in the SVG plugin), phase 4 (P4-1 pointer pass-through; P4-2 … P4-5 of p4-input: the gamepad navigation target, text fields leaving gamepad keys, `data-style` of a missing variable, the `data-for` template no sibling; P4-6 `Element::UpdateTransformStates` of p4-mcp-tools; P4-E1, a `var()` lifetime fix the examples found; the parity fixes of the corrective packages p4-gaps (P4-G1 … P4-G9) and p4-gaps2 (P4-G10 … P4-G14): rounded replaced content, button/select/text-field centering, form-control pseudo-elements, the box-shadow cache key, boolean `data-attr-*`, button display types, empty-line baselines, the themed checkbox/radio, unnamed radios), phase 5 (P5-1 `-jfx-text-fill` applied before `filter`, P5-2 `Decorator::PaintsOwnClip`, P5-3 effect layers interpolating in image lists, of p5-infra; P5-4 the `@property` var lookup of p5-property; P5-5 a filter's region skipping empty boxes, of p5-fx-filters; P5-6 image-layer lists parsed once per text, of p5-tooling), after phase 5 (P6-J1 `text-align: justify`, `text-align-last`, `text-justify`, and P6-J2 `text-indent`, of p6-justify; P6-L1, Chromium 153's legacy `-webkit-line-clamp`, of p6-line-clamp: phase 3 §2.2 had left line clamping out; P6-L2, every UAX #14 line-breaking rule with Chromium 153's tailoring, also across inline element boundaries, of p6-line-clamp's follow-ups; p6-video and p6-stateful-fx patch nothing); all recorded in `FORK.md` | phase 1 §3.14, phase 2 H8, phase 4 §4.1.1, §4.3.1-§4.3.3, §4.5.1, §4.6.1, phase 5 §3.18, §4.1-§4.6 "as implemented" |
| 11 | One UI clock (unscaled frame time) for RmlUi, Lottie, `anime` and effects | phase 1 §3.0 |
| 12 | Documents stay inside the camera colour pass (post-process applies to them); no compositing after post-process | phase 1 §3.5 |
| 13 | Hot reload: engine entry point `reloadChangedFiles` (phase 1) shared by the editor watcher and `McpAutoSync` (phase 4) | phase 1 §3.8.6, phase 4 §3.5 |
| 14 | The compat pipeline runs at document load in shipped games too (in-memory cache, no export-time precompilation); principle 4 concerns shaders and assets | phase 2 §3.12.2 |
| 15 | The RmlUi debugger is compiled only into the editor, Debug exports and the test build | phase 1 §3.8.7 |
| 16 | Excluded with reasons: X11 multi-touch, in-document IME preedit on Linux and in the editor Game view, Lottie expressions/audio, UV hits on skinned meshes (bind pose); CSS features of phase 3 §2.2 (e.g. `writing-mode`, `@container`, `subgrid`, `mix-blend-mode`) are diagnosed, not implemented (`text-align: justify` and `text-indent`, listed there, were implemented after phase 5 by p6-justify, the legacy `-webkit-line-clamp` box by p6-line-clamp; the standard `line-clamp` stays dropped, as Chromium 153 drops it) | phase 1 §2, phase 3 §2.2 |
| 17 | The browser-parity fixtures that scroll content past a clip edge render without MSAA (sidecar `"samples": 1`): llvmpipe and lavapipe (Mesa 23.2.1, the test image's drivers) let half the samples of the row above and the column left of a scissor rectangle through under MSAA (`MsaaScissor.*` reproduces it with raw sokol calls); the renderer is unchanged | phase 3 §3.12.10, §5.2 |
| 18 | A transparent page that does not listen for the pointer passes it to the documents below it in z-order before the game (phase 1 had every transparent page pass it straight to the game, so an upper full-screen document blocked every lower one, and its own page listeners got the empty-area events); a page with a pointer listener on its document or `<body>` takes them, per document; a fork hook, P4-1 | phase 1 §3.6.2 "as implemented (p4-inspector)" |
| 19 | Gamepads reach the focused document as `keydown`/`keyup` events (Edge's gamepad key codes, the keyboard meaning of the navigation buttons, spatial navigation, a fork entry point P4-2), added by phase 4's corrective package p4-input: phase 1 routed only the keyboard, while phase 4 §3.8.1 §10 assumed gamepads too. The `data-for` template is left out of the Lua DOM and of structural selectors (P4-5) | phase 4 §4.6.1 |
| 20 | Stateful GPU simulations of shaders.com (fluids, particles, reaction-diffusion, pixel sort, ...) are excluded: phase-5 effects are stateless on the GPU, their state (integrated clocks, pointer smoothing, the pointer trail, ripple events) lives on the CPU; cursor trail and cursor ripples are rebuilt on that model (reversed by phase 6: row 25) | phase 5 §2, §3.18 item 4 |
| 21 | Effect colours are linear sRGB (CSS colours converted sRGB → linear in C++), not shaders.com's default p3-linear: hues differ slightly from their website by design | phase 5 §3.9, §3.18 item 5 |
| 22 | Phase 2 behaviours extended for effect layers, not contradicted: `background-blend-mode` (normal, multiply, screen, plus-lighter) and gradient text (`background-clip: text`, the internal `-jfx-text-fill` filter list, fork feature `TextFillFilter`) are exact for `effect()` layers; phase 2's reports and solid-colour approximation stay for gradient and `url()` layers. With the fork's native background layers effects are image layers of `background-image`/`mask-image`, not `decorator` entries | phase 5 §3.2, §3.18 item 7, §4.1 "as implemented" |
| 23 | Merge of origin/main (shader library): the agent guide's HTML/CSS UI section is condensed (the library's test caps the guide at 16 KB; the UI tools' shared rules live in `jasper://docs/html-ui`), and the tool descriptions are shortened so the library's five new tools fit the total cap of phase 4/5 (no cap raised); the frame order and render-state model of "Integration with origin/main" | phase 4 §3.8.3, §3.6.7; phase 5 §3.13; this file, "Integration with origin/main" |
| 24 | `<video>` is supported (phase 2 and the guide said "no `<video>`"): AV1 + Opus WebM only, decoded by vendored software decoders (dav1d, libopus, libwebm; "Names and locations"), never by platform decoders or FFmpeg; "Goal" is unchanged (no browser, no webview, no runtime network). Phase 2's placeholder for `<video>` is replaced by the element (`<audio>`, `<canvas>`, `<iframe>` keep it). The UI clock drives video too (deviation 11 extended), with a frame-exact mode under a pinned clock or a fixed frame step (MCP `fixed_fps`); the audio is slaved to the picture. No RmlUi fork patch was needed (`FORK.md`, "Phase 6") | phase 6 §3.1.5, §3.5, §3.17 |
| 25 | Deviation 20 reversed: stateful effects (`@cost stateful`, 18 ports of shaders.com's simulations) keep per-element GPU state images from a pool, advanced at draw time by fragment passes (a scatter form for the agents' splats) at a fixed 60 Hz step with a substep cap; a pinned effect time recomputes the state from a 2 s replay so captures stay deterministic. Upstream compute kernels and atomic accumulators become fragment passes and additive float targets (`rgba16f` sums of upstream's integer quanta) | phase 6 §3.1–3.13 |
| 26 | Float render targets: `ColorFormat` gains `RGBA16F`, `RGBA32F`, `R32F` (appended after `SRGB8A8`) and `RenderFeatures` their render/blend/filter support; a device without one gets `HTMLUI-FX-0017` for the effects that need it | phase 6 §3.3, §3.15 |
| 27 | `ShaderType::UI_EFFECT` gains property bits 1 `Stp` and 2 `Sct`: a stateful effect has up to three variants per draw form (draw, step, scatter), all of its passes in one step variant selected by a uniform pass index | phase 6 §3.4, §3.15 |

## Integration with origin/main (shader library)

On 17 October 2026 GitHub's `main`, which had merged the shader-library project on 14 October
(phases 1-4: `docs/superpowers/specs/2026-10-10-shader-library-phase{1,2,3,4}-design.md`,
`docs/shaders/`), was merged into the HTML UI work (`hui/sync-origin`). Both sides keep every
feature; what the merge decided:

- **Render state.** The fixed pipeline types are built from the library's `RenderState`
  (`ObjectRender::endLoad(pipelines, state, msaaSamples)`: blend, cull, depth, bias, stencil,
  colour mask) with its new types `PIP_EFFECT`, `PIP_POST`, `PIP_MSAA*`; a phase-0 `BlendState`
  set on an object replaces the state's blend on the colour types. One `BlendMode` (`Render.h`)
  serves `RenderState` and the HTML UI's `PipelineState` (the library's `ADDITIVE`/`MULTIPLY`
  beside the effects' `*_PREMULTIPLIED`), one `StencilOp`; `DepthStencilFormat` is the library's
  `FramebufferDepth`. Offscreen camera passes are depth-stencil (library phase 2), so
  `targetFormatFor` says so and knows the new types; the HTML UI's own targets keep their
  `FramebufferDesc` formats. Single-sampled depth-stencil attachments get a texture view again
  (the library samples them); multisampled images still get none.
- **Shaders.** `ShaderType` keeps the library's published `BLOOM` and appends `HTMLUI` ...
  `UI_EFFECT` after it; editor cache `v33`. There are two libraries: library shaders
  (`library:<name>`, `// @shader` manifests, for materials and components) and the HTML UI's
  library effects (`@effects/<name>`, `// @effect` manifests; `ShaderPool::isLibraryCustomShader`
  keeps its name). Neither is a project fork (`destroyCustomShaders` skips both). A project UI
  effect `.frag` is checked as an effect (`ProjectChecks`, `check_shader`), never as a library
  manifest (its `// @effect` header would read as an invalid one); `guessShaderType` tries the
  effect header first; `ShaderBuilder` reads a fragment-only effect from the assets directory
  first, other forks project-relative first (the library's `Project::findShaderFile`). Export
  collects the UI_EFFECT keys of the documents beside the library's light-capacity,
  post-colour, uber and bloom variants (`collectScene` takes both sides' arguments).
- **Frame order (decision).** Unchanged for the HTML UI: every document pass (layers, filters,
  texture-mode documents) runs at the end of `drawFramePasses()` before any camera pass, and a
  scene's screen documents are composited last in its camera's colour pass, after the
  transparent queue and component UI. The library's passes after the scene (SSR, custom passes,
  bloom and blur, the uber pass with grading, FXAA, CRT, render scale) therefore take the
  documents with the image, as both specs state: deviation 12 here ("post-process applies to
  them") and library phase 4 §5.1 "UI (deviation ...)" (UI the main camera draws is part of its
  image). UI shaders never apply the in-shader (T0) colour, so with only in-shader colour on, a
  document is not graded, with the uber pass it is, like component UI; a HUD that must stay
  untouched and at native resolution goes in a layer scene, whose documents composite in that
  scene's camera pass after the uber pass. The scene-colour copy (library phase 3) is taken
  before the transparent queue, without documents. With `msaaSamples` the composite draws into
  the multisampled scene target through the pipeline cache at its sample count
  (`PIP_MSAA_NODEPTH`). Frame stats name the HTML UI passes (`html ui layer`, `html ui post`,
  `html ui`, `html ui texture`), and `JASPERENGINE_READY_SIGNAL` also waits for the documents.
- **sokol_gfx.h.** Two local patches: the Vulkan descriptor size (320 by default for lavapipe,
  now overridable), and the library's GLES fix in `_sg_gl_end_render_pass` (depth/stencil of a
  pass that has them are invalidated when not stored). The HTML UI stores its layers'
  depth-stencil, so the fix changes nothing for it.
- **Tooling.** `tools/build.sh`: `CPUS`, else the library's `DOCKER_CPUS`, else `JOBS` caps the
  container; empty-array-safe expansions. The agent guide fits the library's 16 KB cap with a
  shorter HTML/CSS UI section (the tools' shared rules moved to `jasper://docs/html-ui`, "The UI
  tools' shared rules"). The tool definitions fit the HTML UI's total cap (99,800 + 837) with
  the library's five new tools by shorter descriptions (deviation 23). `UpdateFlags_UIDocument_Reload`
  moved to bit 38. The phase-0 `BlendStateTest` describes the pipeline types as the render
  state model builds them.
- Interfaces changed: `CONTRACTS.md` (rows citing "the merge of origin/main").
- Verified on an idle 12-core test machine on the merged tree: the HTML UI suites (glcore and
  Vulkan 3,145 tests, 0 failed, the 73 Taffy exclusions skipped; perf gates; RmlUi 426 cases;
  editor tests; web; Android), three full MCP runs and the slow ones, every generator and
  reference check, and the library's checks (`shaders --library all --backend all`, its fixtures
  on GL, GLES3 and Vulkan, `shader-library-export/check.py` on GL and Vulkan, exports on every
  backend); a UI Document added to the library's post-processing fixture draws in the editor
  and in exported games under MSAA, render scale, bloom, CRT and the presets. No golden changed.

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
  `SokolTexture.cpp` ~479); swapchain is depth-stencil. Desktop MSAA sample count 1. (Before
  the merge of origin/main: the fixed types take a `RenderState` now, and offscreen camera
  targets are depth-stencil.)
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
  visibility (see `engine/libs/harfbuzz/CMakeLists.txt`). Every library compiles at
  `JASPERENGINE_CXX_STANDARD` (C++20 by default, `engine/cmake/JasperEngineToolchain.cmake`).
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
- Vector: nanosvg is used by `TextureData.cpp` for `.svg` textures (replaced by LunaSVG in
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
