# Phase 4: agent tooling for HTML/CSS UI

This is the binding spec for phase 4. Read `docs/html-ui/DESIGN.md` (binding) and `BRIEF.md`
first, then `docs/mcp/DESIGN.md` and `docs/mcp/WORKSTREAM_BRIEF.md` (the MCP rules apply to
every tool in this phase). Phases 0-3 are merged into `html-ui` before phase 4 starts. Section
3.2 lists exactly what this phase consumes from them.

Paths are relative to the repository root. Line numbers come from `html-ui` at
`d20d3a77`. They were checked when this spec was written: re-find them by the quoted symbol if
the file has moved.

---

## 1. Outcome

When phase 4 is merged, a coding agent (Claude Code, Codex, any MCP client) can build polished
game UI in standard HTML/CSS + Lua with this loop and no human help:

1. **Create.** `create_ui_document` makes a GameObject with a `UIDocumentComponent`, plus the
   starter `.html`/`.css`/Lua files, in one call. The files come from a blank template or one of
   nine polished examples (main menu, pause menu, HUD, inventory, settings, dialogue, shop,
   toasts, loading screen).
2. **Edit.** The agent edits the files with its own tools. Before its next MCP call the live
   documents reload, edit mode and play mode alike, and the call's `external_changes` lists
   every construct the engine drops or rewrites lossily, with `file:line:col`, a stable code and
   a fix. The same findings come from `jasperengine-editor check --ui` and from its `--hook`
   mode right after each edit.
3. **Inspect.** `inspect_ui` returns:
   - the element tree with refs, boxes and problem flags (overflow, clipped, hidden, offscreen);
   - computed styles, each with the `file:line` of the rule that set it;
   - a hit test at a point;
   - data-model values;
   - an outlined, cropped screenshot of the matched elements.
4. **Assert.** `assert_ui` checks the layout: `text_fits`, `no_overlap`, `within_parent`,
   `within_viewport`, `visible`, `contrast`, `min_size`, `exists`/`count`. It can run them at
   several viewport sizes. On failure it returns an image with the failing boxes outlined.
5. **Play.** In play mode, `send_input` clicks, drags and taps elements by CSS selector
   (`target`), and `wait` / `eval_lua` reach the Lua UI API.
6. **Import.** Remote fonts (Google Fonts), icons (Lucide) and Lottie animations come in
   through `import_files`, which can also rewrite the remote URLs in a document.
7. **Learn.** The agent reads the following:
   - `jasper://docs/html-ui`: the guide;
   - `jasper://docs/html-ui-support`: the support matrix, generated from the compatibility
     layer's tables so it cannot drift from the code;
   - the examples, at `.jasperengine/engine-api/html-ui-examples/` in every project and at
     `jasper://examples/html-ui`;
   - typed LuaLS stubs for `UIDocument` / `UIElement` / `UIEvent` / `anime`;
   - `search_engine_api` hits for the Lua UI API.

Every one of these steps is covered by MCP end-to-end tests. Those include an "agent simulation"
that writes typical LLM-style UI code with typical mistakes and drives the whole loop to a clean,
asserted, screenshotted result.

## 2. Scope

### In scope

- `check --ui` in `editor/util/ProjectChecks` and the CLI. It is part of the default set and
  `--all`, works with `--files` and `--format text|json`, and supports `--hook` (UI warnings are
  shown once, when new). The developer subcommand `jasperengine-editor ui-support` dumps the
  compatibility tables as JSON.
- An engine-side inspection and assertion API, `htmlui::HtmlUIInspector` and
  `htmlui::HtmlUIAssertions`. It sits in the engine because RmlUi is linked into the engine
  shared library with hidden visibility, so the editor can never call RmlUi directly. It returns
  plain structs, so the tools never see RmlUi types (DESIGN principle 3).
- MCP tools:
  - new: `inspect_ui`, `assert_ui`, `create_ui_document`;
  - extended: `send_input` (`target`, `to_target`, `document` on mouse and touch events) and
    `import_files` (`fonts`, `icons`, URLs in `sources`, `from_document`);
  - resources: `jasper://examples/html-ui` and `jasper://examples/html-ui/{+path}`.
- Hot reload through `McpAutoSync` / `w4::syncFiles`:
  - `.html/.htm/.css` and the assets documents use (images, `.svg`, `.ttf/.otf`,
    `.json/.lottie`) are tracked;
  - documents reload before the tool runs, in edit mode and live during play;
  - no file is reloaded twice by the auto-sync and phase 1's poller;
  - diagnostics are reported as `problems`.
- Docs:
  - `docs/mcp/guide/html-ui.md`;
  - the generated `docs/mcp/guide/html-ui-support.md` (with `tools/mcp/gen_html_ui_support.py`);
  - an HTML-UI section in `McpInstructions.cpp`;
  - an AGENTS.md section in `Generator.cpp`;
  - updates to `docs/mcp/README.md`, `docs/mcp/checks.md`, `docs/mcp/DESIGN.md` (pick-up list)
    and `docs/mcp/guide/README.md`;
  - a regenerated `TOOLS.md`.
- LuaLS stubs and API search for the HTML UI Lua API: doc comments, typed parameter tables, an
  event-name alias, and `search_engine_api` entries for `anime.*`.
- The example library, made of the nine examples plus `blank` under
  `docs/mcp/guide/html-ui-examples/`:
  - embedded in the editor;
  - written into every project's `.jasperengine/engine-api/html-ui-examples/`;
  - served as MCP resources;
  - used as `create_ui_document` templates;
  - covered by golden images, a zero-diagnostic compat test, and an MCP test that loads each
    example, interacts with it and runs its `assert_ui` checks.
- Test suites (googletest and MCP):
  - `test_html_ui_checks.py`, `test_html_ui_reload.py`, `test_html_ui_examples.py`,
    `test_html_ui_docs.py`, `test_html_ui_tools.py`, `test_html_ui_agent_sim.py`;
  - `engine/tests/htmlui/InspectorTest.cpp`, `AssertionsTest.cpp`, `ExamplesCompatTest.cpp`.

### Out of scope

| Item | Reason |
|---|---|
| A tool that writes data-model values | `eval_lua` already does it during play: `UIDocument(...):getDataModel(name).field = v`, shown in the guide. Edit mode has no Lua, so there is nothing to bind. One tool less against the budget. |
| Cross-checking Lua literals (`getElementById("x")`) against the markup | It needs the scene graph to map a script to its document. A wrong id fails at runtime with a Console error the loop already reads. |
| Changing the `screenshot` tool | `inspect_ui` / `assert_ui` return cropped, outlined images themselves, so `screenshot` stays as it is. |
| The list of event listeners on an element | RmlUi's dispatcher does not expose it. |
| Trimming other modules' tool descriptions to win back budget | Other workstreams own them. See 3.6.7. |
| New compat features or fork patches | Those are phases 2-3. A gap found here goes into their files as a bug fix, reported (BRIEF ownership rules). |
| UI effects (phase 5) | Phase 5. |

---

## 3. Detailed design

### 3.1 Map of the work

```
 agent edits assets/ui/x.html|css|lua ─┐
                                        ▼
 tools/call ──► McpAutoSync::beforeCall (scan: +html/css/fonts/json/lottie)
                 └─► w4::syncFiles ─► w4::syncHtmlUi ─► UIDocumentSystem::reloadChangedFiles (phase 1)
                                       │                    (stamps shared with phase 1's poller)
                                       └─► ProjectChecks::checkUiFiles (files no live doc uses)
                 external_changes {ui_reloaded, problems[{path,line,col,severity,code,message,suggestion}]}
 inspect_ui / assert_ui / create_ui_document   (editor/mcp/tools/HtmlUiTools.cpp)
                 └─► htmlui::HtmlUIInspector / HtmlUIAssertions (engine, RmlUi inside)
                 └─► w3::captureView + crop/outline (screenshots)
 send_input target ──► htmlui_mcp::elementCenter (HtmlUiShared.h) ─► HtmlUIInspector::elementRect
 import_files fonts/icons/urls/from_document ──► phase-2 importers
 jasperengine-editor check --ui [--hook] ──► ProjectChecks::checkUiFiles ──► compat (phase 2)
 jasperengine-editor ui-support ──► compat support tables ──► gen_html_ui_support.py ──► html-ui-support.md
 docs/mcp/guide/html-ui-examples/** ──► CMake ──► html_ui_examples.h ──► editor/util/HtmlUiExamples
                 ├─► Generator::writeEditorTooling → .jasperengine/engine-api/html-ui-examples/
                 ├─► create_ui_document template
                 └─► jasper://examples/html-ui
```

### 3.2 APIs consumed from phases 0-3

Phase 4 consumes the APIs below **exactly**. A reviewer of phases 0-3 checks the merged code
against this list. If a name differs once phases 0-3 are merged, the merged code wins: the p4
package adapts its call sites, examples and docs, and records the difference in its report. If
a consumed capability is missing altogether, the p4 package adds it to the owning file as a
small additive change (BRIEF ownership rules) and lists it in its report.

#### 3.2.1 Phase 2: compatibility layer (`engine/core/htmlui/compat/`)

Reconciled with `phase-2-compat.md` (§3.4, §3.5, §3.12.1, §3.13.7); the names below are phase
2's, and `docs/html-ui/CONTRACTS.md` lists them. Namespace `jasperengine::htmlui::compat`.
Public headers include no lexbor header; the functions phase 4 calls are `JASPERENGINE_API`.

```cpp
// Diagnostics.h, DiagnosticCodes.h (phase 2 §3.4, Appendix F)
enum class Severity : uint8_t { Error, Warning, Info };
struct DiagCode { DiagArea area; uint16_t number; std::string id() const; };   // id(): "HTMLUI-CSS-0004"
struct Diagnostic {
    Severity severity; DiagCode code; std::string message, suggestion, replacement, subject;
    std::string file;                 // ASSET-relative ("ui/menu.css"); "" when generated
    uint32_t line, column, length;    // 1-based, 0 = unknown
    std::string origin;
};
std::string toText(const Diagnostic&);           // "ui/menu.css:12:5: warning: message (fix: s) [HTMLUI-CSS-0004]"
nlohmann::json toJson(const Diagnostic&);
struct DiagCodeInfo { DiagCode code; Severity severity; const char* name /* kebab-case */; const char* title; const char* help; };
Span<const DiagCodeInfo> diagnosticCodes();
const DiagCodeInfo* findDiagnosticCode(std::string_view idOrName);

// CompatOptions.h / CompatApi.h (phase 2 §3.12.1)
struct FileAccess { std::function<bool(const std::string& assetPath, std::string& content)> read;
                    std::function<bool(const std::string& assetPath)> exists; };   // empty = FileData
struct CheckOptions { CompatOptions compat; bool verifyWithRmlUi = true; FileAccess files; };
struct CheckReport { std::vector<Diagnostic> diagnostics; std::vector<std::string> files /* every file read, asset-relative */;
                     std::vector<RemoteReference> remoteRefs; };
CheckReport checkDocument(const std::string& htmlPath, const CheckOptions&);     // htmlPath asset-relative
CheckReport checkStylesheet(const std::string& cssPath, const CheckOptions&);
std::string cssPropertyName(std::string_view rcssProperty);                     // "decorator" -> "background-image"
struct RmlSourceMap { SourceRefList elementRefs; SourceRefList lineRefs; };   // SourceRefList: size(), [i], iteration (SourceRef by value)
// CompatDocument (sourcePath, rml, sourceMap, stylesheets with RcssLineMap, dependencies, diagnostics, files)
// and std::optional<Diagnostic> locateElement(const CompatDocument&, const Rml::Element*)  (engine-internal)

// SupportTables.h (phase 2 §3.5, Appendices A-E)
enum class Support : uint8_t { Native, Rewritten, Approximated, Emulated, Fallback, Unsupported, Ignored };
// propertyTable(), valueTable(), selectorTable(), atRuleTable(), mediaFeatureTable(), elementTable(),
// attributeTable(), inputTypeTable(), unitTable(), functionTable(): Span<const XxxSupport>, each row with
// name, support, fork, (supportWithFork,) output, note, suggestion
Support effectiveSupport(Support base, ForkFeature fork, Support withFork, const ForkFeatures&);
ForkFeatures ForkFeatures::present();
```

How phase 4 uses them (these replace the names an earlier draft of this spec assumed;
the rest of this spec uses phase 2's names):
- **Converting a document without the engine** (`check --ui`, `ExamplesCompatTest`,
  `create_ui_document`, `inspect_ui {file}`): `checkDocument(path, {compat, verifyWithRmlUi =
  false, files = ProjectChecks::projectFiles(root)})` — pure CPU, no `Engine::systemInit`, no
  RmlUi context. A stylesheet on its own: `checkStylesheet`.
- **Paths:** compat paths are asset-relative; phase 4 prints and returns project-relative
  paths by prefixing the assets directory (`assets/ui/menu.css`), and strips it on input.
- **Codes:** the `code` phase 4 reports everywhere (CLI findings, MCP `problems`, hook keys,
  `ui-support`) is `DiagCode::id()` (`HTMLUI-CSS-0004`); `ui-support` also gives the registry
  `name` (`unsupported-value`). Severity names are `error`/`warning`/`info`.
- **Support statuses** (`ui-support`, the matrix): phase 2's seven statuses, lowercase
  (`native`, `rewritten`, `approximated`, `emulated`, `fallback`, `unsupported`, `ignored`),
  each row's status computed with `effectiveSupport(..., ForkFeatures::present())`. After
  phase 3 almost no row is `fallback`.
- **Severities phase 4 tests rely on** (phase 2 Appendix F): a missing local file (linked
  stylesheet, `@import`, image, font, svg, lottie) `HTMLUI-GEN-0001` Error; a remote
  `http(s)` URL without an import `HTMLUI-IMP-0001` Error whose suggestion names
  `import_files`; `<script>` with JavaScript `HTMLUI-HTML-0006` Error and `on*` attributes
  `HTMLUI-HTML-0005` Error, both suggesting Lua `addEventListener`; `javascript:` URLs
  `HTMLUI-HTML-0023` Info (dropped); unsupported elements (`<canvas>`, `<video>`)
  `HTMLUI-HTML-0002` Warning; unsupported properties/values/selectors/at-rules Warning
  (or by case) with a suggestion; lossless rewrites Info.
- **Source mapping** (inspector style sources): the RmlUi property source names a virtual
  sheet (`@htmlui/doc/<key>/<path>.rcss:<line>`); `CompatDocument::stylesheets[i].lines` maps
  the line back to the authored file and line, `elementRefs` maps `data-htmlui-loc` to the
  element's HTML position (`locateElement`).

The importers live on the editor side, in phase 2's facade `editor/htmlui/HtmlUiImporters.h`
(namespace `jasperengine::editor::htmlui`, `phase-2-compat.md` §3.13.7):

```cpp
struct ImportResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> written;                          // project-relative files written
    std::vector<std::pair<std::string, std::string>> replaced; // remote URL -> project-relative path
    std::string fontFaceCss;                                   // project-relative .css with @font-face rules (fonts only)
};
ImportResult importGoogleFonts(const fs::path& projectRoot, const std::vector<std::string>& specs /* "Inter:400,700", "Roboto Mono:400,400i" */, const std::string& destDir);
ImportResult importLucideIcons(const fs::path& projectRoot, const std::vector<std::string>& names /* "play" */, const std::string& destDir);
ImportResult importUrl(const fs::path& projectRoot, const std::string& url /* image, svg, font, LottieFiles page or .json */, const std::string& destDir);
// Every remote URL a .html/.css references (links, @import, url(), src): imported (manifest written); with rewrite the
// returned `replaced` list is what the caller substitutes in the file (the importer never edits the file)
ImportResult importDocumentReferences(const fs::path& projectRoot, const std::string& htmlOrCss, const std::string& destDir);
```

Offline testing: when the environment variable `JASPERENGINE_IMPORT_MIRROR=<dir>` is set, every
importer fetch of `https://<host><path-and-query>` reads the file
`<dir>/<host>/<urlquote(path-and-query, safe="")>` instead. That is Python's
`urllib.parse.quote` with nothing safe, so `/`, `?`, `=`, `&` and `:` are all percent-encoded. A
missing file fails the import with "not found". There is no network access in that mode
(phase 2 §3.13.7 implements it in `HttpClient`).

#### 3.2.2 Phases 0-1 (and phase 2 hooks): component, system, wrapper, hot reload, Lua

`UIDocumentComponent` (`engine/core/component/UIDocumentComponent.h`, phase 0 §3.7.1 +
phase 1 §3.8.1):

- `std::string filename`: path of the document, relative to the project's assets directory,
  as texture paths are (MCP validates `filename` properties, phase 0 D10). MCP tools take and
  return it project-relative and convert, as `McpValues` does for textures.
- `int sortOrder`, `bool enabled` (the component checkbox; the "document" JSON reports it as
  `visible`), `bool interactive`, `UIDocumentRenderMode renderMode` (`SCREEN`/`TEXTURE`),
  `textureWidth`, `textureHeight`.
- Catalog properties `filename`, `sortOrder`, `enabled`, `interactive`, `renderMode`, ….
- MCP component name `uidocument` (YAML key and `getComponentType("uidocument")`, phase 0 C3).

`UIDocumentSystem` (`engine/core/subsystem/UIDocumentSystem.h`) is registered in `Scene::init`
(`engine/core/Scene.cpp:28-34`, after `UISystem`). Its header forward-declares the RmlUi and
compat types it names and includes no RmlUi header, so editor files can include it.

```cpp
HtmlUIContext* getContext() const;                         // phase 0; ->getRmlContext() for the Rml::Context (engine-internal)
Rml::ElementDocument* getDocument(Entity entity) const;    // phase 1 §3.8.6; null until loaded, or after a failed load
bool isDocumentLoaded(Entity entity) const;                // phase 0
// render-to-texture: the component's renderMode == TEXTURE (phase 1 §3.12)
std::shared_ptr<const htmlui::compat::CompatDocument> getCompiledDocument(Entity entity) const;   // phase 2 H1b (source map, dependencies)
const std::vector<htmlui::compat::Diagnostic>& getDiagnostics(Entity entity) const;              // phase 2 H1b: of the last (re)load, compile + runtime
std::vector<std::string> getDocumentFiles(Entity entity) const;   // phase 1 §3.8.6 + phase 2 H2; asset-relative, html included
uint32_t getLoadCount(Entity entity) const;                // phase 1 §3.8.6: +1 on every (re)load, a stylesheet-only refresh included
// phase 1 §3.8.6: reloads each document of this scene that depends on one of `files` (asset-relative) whose
// stamp (mtime, size) differs from the stamp recorded at that document's last (re)load, then records the new
// stamps. Phase 1's editor watcher calls the same function, so a change is reloaded once, by whichever of the
// two sees it first. CSS-only changes refresh the stylesheets in place (elements and listeners kept, phase 2
// H2); any other change rebuilds the document and fires UIDocument.onLoad.
struct Reloaded { Entity entity; bool stylesheetOnly; };
std::vector<Reloaded> reloadChangedFiles(const std::vector<std::string>& files);
std::vector<std::string> getDataModelNames(Entity entity) const;                       // phase 1 §3.9.5
bool pushDataModel(lua_State* L, Entity entity, const std::string& name) const;        // phase 1 §3.9.5: pushes the Lua proxy
```

In the rest of this spec, `getDependencies(entity)` means `getDocumentFiles(entity)`,
`getSourceMap(entity)` means `getCompiledDocument(entity)->sourceMap` plus the stylesheet line
maps, and `isLoaded(entity)` means `isDocumentLoaded(entity)`.

Runtime behaviour phase 4 relies on:

- Documents load, lay out and render in edit-mode scenes (Scene and Game views) without Lua, and
  in play.
- CSS px = canvas px.
- The UI clock (phase 1 `HtmlUIClock`, §3.0 of `phase-1-integration.md`: what RmlUi
  `SystemInterface::GetElapsedTime`, CSS transitions/animations, Lottie and `anime` read)
  advances by the engine's unscaled frame delta. It is not wall clock: under
  `play {fixed_fps}` it is deterministic, and it does not advance while `auto_pause` holds the
  simulation. The golden tests depend on this.

Lua (`engine/core/script/binding/HtmlUIClassesLua.cpp`). The examples and the guide use only
these names.
| Class / table | Members used |
|---|---|
| `UIDocument(scene, entity)` | properties `body`, `loaded`. Methods `getElementById`, `querySelector`, `querySelectorAll` (array table), `createElement`, `createDataModel(name, table)` (returns the model proxy), `getDataModel(name)`, `show`, `hide`. Event `onLoad`, fired after every (re)load: `RegisterEvent(self, doc.onLoad, "method")`. |
| `UIElement` | properties `id`, `tagName`, `className`, `textContent`, `innerHTML`, `value`, `checked`, `disabled`, `style` (`el.style.width = "40%"`), `classList` (`add`, `remove`, `toggle`, `contains`), `dataset`, `parentElement`, `children`, `offsetLeft/Top/Width/Height`. Methods `getAttribute`, `setAttribute`, `removeAttribute`, `hasAttribute`, `querySelector`, `querySelectorAll`, `appendChild`, `removeChild`, `remove`, `addEventListener(type, fn)`, `removeEventListener`, `focus`, `blur`, `click`, `scrollIntoView`, `getBoundingClientRect`. |
| `UIEvent` | properties `type`, `target`, `currentTarget`, `key` (browser key names: `"Escape"`, `"Enter"`, `"ArrowUp"`), `button`, `clientX`, `clientY`. Methods `stopPropagation`, `preventDefault`. Form values are read from `event.target.value` (DOM). |
| `anime` | `animate(targets, params)` returns an Animation (`play`, `pause`, `restart`, `reverse`, `seek`, `cancel`, field `completed`). `stagger(value, opts)`, `createTimeline(opts)` (`add(targets, params, position)`, `play`, `pause`), `createSpring(opts)`, `utils.set`, `utils.remove`. Params: CSS property names (camelCase) or `translateX/Y`, `scale`, `rotate`, `opacity`. Values: number, string, `{from=..., to=...}` or a keyframe array. `duration`/`delay` in ms. `ease` as an Anime.js name (`"outElastic(1, .6)"`). `loop`, `alternate`, `autoplay`, `onComplete`, `onUpdate`. |
| Data binding in markup | `data-model`, `{{ }}`, `data-for`, `data-if`, `data-class-*`, `data-style-*`, `data-attr-*`, `data-value`, `data-checked`, `data-event-*`. Assigning a field of the proxy updates the view. |
| Events used by the examples | `click`, `mouseover`, `mouseout`, `keydown`, `change`, `input`, `focus`, `blur`, `dragstart`, `dragdrop`, `dragend`. |

#### 3.2.3 Phase 0: C++ test harness (`engine/tests/htmlui/`)

The target `jasperengine-htmlui-tests` (googletest, Xvfb; built and run by
`tools/build.sh htmlui-tests`, phase 0 §3.8) provides, in `engine/tests/htmlui/harness/`:

- `GpuEnvironment::ensure()`, `setView(screenW, screenH, canvasW, canvasH, Scaling)`,
  `runFrames(ready, maxFrames)`;
- `SceneFixture`: a headless `Scene` with a `CAMERA_UI` camera,
  `UIDocument addDocument(const std::string& file, int sortOrder = 0)`, `waitUntilLoaded()`;
  it exposes the `Scene*` and, through the returned `UIDocument`, the `Entity`;
- the asset root `engine/tests/htmlui/fixtures/`: phase-4 fixtures are committed under
  `fixtures/p4/`; a test that needs generated files writes them under the test user-data
  directory and loads them with the `data://` scheme (phase 0 `RmlFile`/`FileData`);
- `HtmlUIClock::setFixedTime` (phase 1) to pin the UI clock.

### 3.3 Engine inspector and assertions (`engine/core/htmlui/`)

#### 3.3.1 `HtmlUIInspector.h`

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "Export.h"
#include "ecs/Entity.h"
#include "htmlui/compat/Diagnostics.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace jasperengine {
class Scene;
}

namespace jasperengine::htmlui {

// Read-only introspection of live HTML UI documents for tools (MCP inspect_ui / assert_ui /
// send_input target). Plain structs only: no RmlUi type crosses this header. Coordinates are CSS
// px = canvas px, origin top-left of the document's viewport. Main thread only. Every query
// first brings the document's layout up to date (Rml::ElementDocument::UpdateDocument).
struct InspectRect { float x = 0, y = 0, width = 0, height = 0; };
struct InspectEdges { float top = 0, right = 0, bottom = 0, left = 0; };

struct InspectFlags {
    bool overflowX = false;    // scroll width  > client width  + 0.5 (text or children wider than the content box)
    bool overflowY = false;    // scroll height > client height + 0.5
    bool clipped = false;      // part of the border box is cut by an ancestor with overflow != visible
    bool hidden = false;       // display: none or visibility: hidden on it or an ancestor
    bool transparent = false;  // effective opacity (product with ancestors) < 0.05
    bool offscreen = false;    // border box entirely outside the viewport
    bool zeroSize = false;     // border box width or height < 1
};

struct InspectElement {
    int index = -1;            // position in the returned list
    int parent = -1;           // index of the parent in the list, -1 when not listed
    int depth = 0;             // relative to the first listed ancestor
    std::string ref;           // unique selector: "#id" when the id is unique, else the nearest uniquely-id'd
                               // ancestor (or "body") + " > tag:nth-child(n)" steps; resolves to exactly this element
    std::string tag, id;
    std::vector<std::string> classes;
    std::vector<std::string> pseudo;   // set pseudo-classes among hover, active, focus, checked, disabled
    std::string text;          // own text (direct text children), whitespace collapsed, at most 120 chars ("..." appended)
    InspectRect border;        // border box
    float opacity = 1;         // effective
    int childCount = 0;        // element children (SVG internals are not elements here: <svg> is a leaf)
    InspectFlags flags;
};

struct InspectBoxModel {
    InspectRect content, padding, border, margin;
    InspectEdges paddingEdges, borderEdges, marginEdges;
    float scrollWidth = 0, scrollHeight = 0, clientWidth = 0, clientHeight = 0, scrollLeft = 0, scrollTop = 0;
};

struct InspectStyle {
    std::string property;   // CSS name (compat::cssPropertyName)
    std::string value;      // computed value as CSS text ("16px", "rgba(255, 255, 255, 0.8)", "flex")
    std::string source;     // "assets/ui/menu.css:42:3" (via the compat line maps, 3.2.1), "assets/ui/menu.html:12 (style attribute)",
                            // "inherited from <ref>", "user-agent", "default"
    std::string selector;   // the rule's selector when known (".panel h1")
};

struct InspectDetail {
    InspectElement element;
    InspectBoxModel box;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<InspectStyle> styles;
};

struct DocumentInfo {
    Entity entity = NULL_ENTITY;
    std::string html;                 // project-relative
    bool loaded = false;
    bool renderToTexture = false;
    bool visible = true;
    int sortOrder = 0;
    float width = 0, height = 0;      // viewport
    uint32_t loadCount = 0;
    std::vector<compat::Diagnostic> diagnostics;
    std::vector<std::string> dependencies;
    std::vector<std::string> dataModels;
};

class JASPERENGINE_API HtmlUIInspector {
public:
    // GameObjects with a UIDocumentComponent, in entity order
    static std::vector<Entity> documents(Scene* scene);
    static bool documentInfo(Scene* scene, Entity entity, DocumentInfo& out, std::string& error);
    // Elements matching `selector` ("" = <body>) and up to `depth` levels of their descendants,
    // document order, at most `limit` (truncated = more existed). An invalid selector is an error
    // ("selector \"div[[\" is not valid: ...").
    static bool query(Scene* scene, Entity entity, const std::string& selector, int depth, size_t limit,
                      std::vector<InspectElement>& out, bool& truncated, std::string& error);
    // The element that receives the pointer at (x, y) and its ancestors, topmost first; empty when
    // the point hits no element (it falls through to the game)
    static bool hitTest(Scene* scene, Entity entity, float x, float y, std::vector<InspectElement>& stack, std::string& error);
    // Box model, attributes and computed styles of the element `ref` (any selector: its first match).
    // properties empty = every property whose computed value differs from its initial value, plus
    // display, position, width, height, color, font-size, font-family. Unknown property names are an error.
    static bool detail(Scene* scene, Entity entity, const std::string& ref, const std::vector<std::string>& properties,
                       InspectDetail& out, std::string& error);
    // Computed values of `properties` for every element of `elements` (same order), source omitted
    static bool styles(Scene* scene, Entity entity, const std::vector<std::string>& refs, const std::vector<std::string>& properties,
                       std::vector<std::vector<InspectStyle>>& out, std::string& error);
    // Border box of the first visible (not hidden/transparent/zero-size) element matching selector
    static bool elementRect(Scene* scene, Entity entity, const std::string& selector, InspectRect& out, std::string& error);
    // Lays out the scene's context at width x height (media queries re-evaluated), runs fn, then restores
    // the dimensions and lays out again, before returning. Nothing renders in between.
    static bool withViewport(Scene* scene, float width, float height, const std::function<void()>& fn, std::string& error);
    // Pushes the Lua table bound to data model `name` (UIDocumentSystem::pushDataModel)
    static bool pushDataModel(Scene* scene, Entity entity, const std::string& name, lua_State* L);
};

}
```

Implementation notes:

- `ref` generation is checked by the tests: `query(ref)` returns exactly that element.
- Style sources: the RmlUi `Property::source` (path, line, rule name) points into the generated
  RCSS. It is mapped back with the compiled document's stylesheet line maps
  (`getCompiledDocument(entity)`, 3.2.1). Inline `style` attributes map to the
  element's HTML line. Properties from the compat user-agent sheet say `user-agent`. Inherited
  values name the ancestor's ref. Values that are not set say `default`.
- `cssPropertyName` turns RmlUi names into CSS names. Values are printed as CSS:
  - lengths in px with at most 2 decimals;
  - colors as `rgb()`, or `rgba()` with alpha 0-1;
  - keywords as written.
- `withViewport` uses `Rml::Context::SetDimensions` + `Update`. It keeps the
  `SetDensityIndependentPixelRatio` as is. A scope guard restores the dimensions even if `fn`
  throws.

#### 3.3.2 `HtmlUIAssertions.h`

```cpp
namespace jasperengine::htmlui {

enum class AssertKind { Exists, Count, Visible, TextFits, NoOverlap, WithinParent, WithinViewport, Contrast, MinSize };

struct AssertSpec {
    AssertKind kind = AssertKind::Exists;
    std::string selector;
    std::string with;          // NoOverlap: second set (empty: the selector's matches pairwise)
    std::string container;     // WithinParent: closest ancestor matching this instead of the parent
    int count = -1, min = -1, max = -1;   // Count (Exists = min 1)
    float tolerance = 0.5f;    // px
    float minRatio = 0;        // Contrast; 0 = WCAG AA: 4.5, or 3 for large text (>= 24px, or >= 18.66px and weight >= 700)
    bool hasBackground = false;
    float background[3] = {0, 0, 0};   // Contrast: sRGB 0-1 backdrop assumed where the document is transparent
    bool unobstructed = false; // Visible: also the topmost element at the center of its visible part
    bool allowScroll = false;  // TextFits: overflow inside an overflow:auto/scroll container passes
    bool singleLine = false;   // TextFits: wrapping to a second line fails
    float minWidth = 44, minHeight = 44;   // MinSize
};

struct AssertFailure {
    std::string ref;
    std::string message;       // one sentence with the numbers ("text is 166px wide in a 120px content box")
    InspectRect rect;
    std::string otherRef;      // NoOverlap partner / WithinParent container
    InspectRect otherRect;
};

struct AssertResult {
    bool passed = true;
    size_t matched = 0;
    std::vector<AssertFailure> failures;
    std::vector<std::string> inconclusive;  // Contrast: refs whose backdrop could not be resolved
    std::string error;                      // invalid selector etc. (passed = false)
};

class JASPERENGINE_API HtmlUIAssertions {
public:
    static AssertResult run(Scene* scene, Entity entity, const AssertSpec& spec);
};

}
```

Semantics. These are binding; the C++ tests check each rule.

- **Zero matches** fail every kind except `Count` when `max` is 0 or `count` is 0. The message
  is "selector matched nothing", so a typo never passes silently.
- **Visible**, per match, needs all of the following:
  - no `display:none` on it or an ancestor;
  - `visibility: visible`;
  - effective opacity ≥ 0.05;
  - a border box of at least 1×1;
  - a border box that intersects the viewport;
  - a border box that is not fully cut by ancestor clip rects (padding boxes of ancestors whose
    `overflow` is not `visible`);
  - with `unobstructed`: `hitTest` at the center of the visible part returns the element or one
    of its descendants.
- **TextFits** looks at every element with own text among the matches and their descendants.
  It fails when either holds, beyond `tolerance`:
  - the union of its text line boxes extends past its content box, unless `allowScroll` is set
    and the element or an ancestor up to the match scrolls;
  - an ancestor's clip rect cuts the text.

  With `singleLine`, more than one line also fails. The message gives the overflow in px per
  axis.
- **NoOverlap** takes the pairs of A = matches(selector) and B = matches(with), or A×A. It skips
  pairs where one element is the ancestor of the other, and elements that are not visible
  (hidden, transparent or zero-size). A pair fails when the intersection of the border boxes is
  more than `tolerance` on both axes. The failure reports the intersection rect.
- **WithinParent**: the border box must lie inside the container's padding box, give or take
  `tolerance`. Hidden elements are skipped.
- **WithinViewport**: the border box must lie inside `[0, 0, width, height]`, give or take
  `tolerance`. Hidden elements are skipped.
- **Contrast** uses the WCAG 2.x ratio `(L1 + 0.05) / (L2 + 0.05)`, with relative luminance
  computed from linearized sRGB.
  - Foreground: the computed `color` with its alpha times the effective opacity, composited
    over the backdrop.
  - Backdrop: walk from the element up through its ancestors and composite each
    `background-color` until an opaque layer is reached.
  - A gradient, image or decorator background on that path before an opaque layer makes the
    result inconclusive.
  - Reaching the root without an opaque layer composites over `background` if it was given;
    otherwise the result is inconclusive.
  - An inconclusive element is not a failure.
- **MinSize**: visible matches need width ≥ `minWidth` and height ≥ `minHeight`.

### 3.4 Project checks: `check --ui`, hook, `ui-support`

#### 3.4.1 `ProjectChecks` additions (`editor/util/ProjectChecks.{h,cpp}`)

`Finding` (`ProjectChecks.h:30-37`) gains two fields:

```cpp
std::string code;        // ui: the compat diagnostic code; "" for the other tools
std::string suggestion;  // ui: the fix; "" when none
```

- `toJson` (`ProjectChecks.cpp:101`) adds `code` and `suggestion` only when they are not empty.
  So Lua, shader and C++ findings keep exactly the 6 keys `test_checks.py` checks.
- `toText` (`ProjectChecks.cpp:113`) appends ` (fix: <suggestion>)` when there is a suggestion,
  then ` [<code>]` when there is a code:

  ```
  assets/ui/menu.css:12:5: warning: 'mix-blend-mode' is not supported; dropped (fix: use opacity or a pre-blended image) [HTMLUI-CSS-0002]
  ```
- `toolOrder` puts `ui` after `cpp`.

New section:

```cpp
// ---- HTML/CSS UI: the compatibility layer's diagnostics (engine/core/htmlui/compat), the same code
// that runs when a document loads

// .html, .htm, .css (any case)
static bool isUiFile(const std::string& path);
// Sandboxed project loader: reads/exists for project-relative paths, refusing absolute paths and '..'
// escapes (compat reports those as missing)
// (paths given to compat are asset-relative; the reader resolves them under <project>/assets)
static htmlui::compat::FileAccess projectFiles(const std::filesystem::path& projectRoot);
struct UiCheck {
    std::vector<Finding> findings;   // tool "ui", severity "error"/"warning" (Info dropped), code, suggestion
    size_t documents = 0;            // .html/.htm converted
    size_t stylesheets = 0;          // .css checked on their own
};
// files empty: the whole project, meaning every .html/.htm outside skipped directories except the web
// export's custom shell (project.yaml web.customHtmlShell), then every .css none of those documents
// depends on. files given (absolute): an .html/.htm is converted as a document, a .css is checked on its
// own. A diagnostic located in a linked stylesheet is reported at that stylesheet's path; identical
// findings (a stylesheet linked by several documents) are reported once.
static UiCheck checkUiFiles(const std::filesystem::path& projectRoot, const std::vector<std::filesystem::path>& files);
```

`checkUiFiles` is the one function used by the CLI (`--ui`, `--files`, `--hook`), by
`w4::syncHtmlUi`, and by `inspect_ui {file}`. That is what makes their findings identical.

#### 3.4.2 CLI (`editor/cli/CommandLine.cpp`, `editor/main.cpp`)

- `CheckOptions` (`CommandLine.cpp:1513`) gains `bool ui`.
- `parseCheckArgs` (`:1552`) accepts `--ui`.
- `printCheckUsage` (`:1527`) documents it:

  ```
  --ui                 HTML/CSS UI documents: the compatibility diagnostics (dropped or rewritten
                       constructs, missing or remote files) with file:line:col, code and fix.
  --all                Lua, shaders, C++ and UI. Default: --lua --shaders --ui.
  ```
- The default set becomes `--lua --shaders --ui` and `--all` includes `--ui`. UI checks are pure
  parsing: no engine start, about 1 ms per document.
- `runCheck` (`:1917`), file classification (`:1966-2004`): `ProjectChecks::isUiFile(name)` →
  `uiFiles.push_back(path)` when `ui` is wanted. Without `--files`/`--hook`, `checkUiFiles(root, {})`
  checks the whole project.
- `runChecks` runs the UI check before Lua. Because the UI check needs no engine,
  `Engine::systemInit` only runs when Lua, shaders or C++ checks are wanted.
- `run.checked` gains `{"ui", documents + stylesheets}`, and the summary unit map (`:2045`)
  gains `{"ui", "file"}`:

  ```
  check: 0 errors, 3 warnings (lua 7 files, shaders 0 forks, ui 2 files) in 0.21s
  ```
- Exit codes are unchanged: 0 means no errors (warnings allowed), 1 errors, 2 usage error.

Hook mode:

- `isHookTrackedFile` (`:1655`) and `scanHookFiles` (`:1661`, the extension list) add
  `.html .htm .css`.
- The state file `.jasperengine/check-hook-state.json` goes to `"version": 2` with a new key:

  ```json
  {"version": 2, "files": {...}, "ui_reported": {"assets/ui/menu.css": ["HTMLUI-CSS-0002|'mix-blend-mode' is not supported; dropped", ...]}}
  ```

  A version-1 file is read as before, with `ui_reported` empty.
- For each UI file checked in hook mode:
  - **errors** are always printed;
  - **warnings** are printed only when their key (`code|message`, without the line, so moving
    code does not re-report it) is not in `ui_reported[path]`.
- `ui_reported[path]` is then set to the file's current warning keys, so fixed warnings drop out.
- Exit 2 when anything was printed, otherwise 0. After printed UI warnings, one last line:
  `(UI warnings are shown once, after the edit that introduced them; jasperengine-editor check --ui lists them all)`.
- The log line (`appendHookLog`) counts printed UI warnings as warnings.

New subcommand: `jasperengine-editor ui-support`.

- `editor/main.cpp:28-30` gets a sibling dispatch to `CommandLine::runUiSupportCommand`, declared
  in `CommandLine.h` next to `runCheckCommand`.
- The main help (`:240-241`) lists:
  `ui-support             Print the HTML/CSS support tables as JSON (developer: docs generation).`
- It prints to stdout and exits 0; there are no options, and `-h` prints usage.

```json
{"version": 1, "editor_version": "v0.7.1-...",
 "features": [{"kind": "property", "name": "mix-blend-mode", "support": "unsupported",
               "note": "...", "suggestion": "...", "code": "HTMLUI-CSS-0002"}, ...],
 "diagnostics": [{"code": "HTMLUI-CSS-0002", "name": "unsupported-property", "severity": "warning", "summary": "..."}, ...]}
```

`kind` values are `element`, `attribute`, `property`, `value`, `function`, `unit`, `selector`,
`pseudo_class`, `pseudo_element`, `at_rule`, `media_feature` and `input_type` (one per phase-2
table; selector rows are split into `selector`/`pseudo_class`/`pseudo_element` by their kind).
`support` values are phase 2's statuses, lowercase: `native`, `rewritten`, `approximated`,
`emulated`, `fallback`, `unsupported`, `ignored`, each computed with
`effectiveSupport(..., ForkFeatures::present())`. Entries keep table order.

#### 3.4.3 Support matrix generator (`tools/mcp/gen_html_ui_support.py`)

```
uv run --no-project --with mcp --with pyyaml python tools/mcp/gen_html_ui_support.py
    [--out docs/mcp/guide/html-ui-support.md] [--json-out FILE] [--check]
```

It runs `harness.editor_command() + ["ui-support"]` and writes the Markdown. `--check` exits 1
with a unified diff when the file is stale; that is how tests use it.

Output layout:

```markdown
# HTML/CSS support matrix

Generated by `tools/mcp/gen_html_ui_support.py` from the engine's compatibility tables
(`jasperengine-editor ui-support`, editor <version>). Do not edit by hand. How to build UI:
jasper://docs/html-ui.

Legend: **native**, works as in a browser · **rewritten**, works, the engine translates it ·
**emulated**, works through engine code · **approximated**, works with the limits in Notes ·
**fallback**, approximated until a fork feature lands · **unsupported**, dropped with a
diagnostic; use the replacement · **ignored**, accepted, no effect in a game UI.

## Elements
| Element | Support | Notes | Instead |
## Attributes
## CSS properties
## Values, functions and units
## Selectors, pseudo-classes and pseudo-elements
## At-rules
## Diagnostic codes
| Code | Severity | Meaning |
```

Rows are sorted by name within a section. Pipe characters in text are escaped.

### 3.5 Hot reload through McpAutoSync

#### 3.5.1 What the scan tracks (`editor/mcp/McpAutoSync.cpp`)

- `isTrackedFile` (`:153-157`) adds `.html .htm .css` to the code tier, which is checked whole
  on every call.
- It adds `.ttf .otf .json .lottie` to the rotating tier. Rename the "images" map's meaning to
  "assets": `mapFor` (`:199-201`) sends `Util::isImageFile` plus those extensions to it. Images
  (`.svg` included) are already tracked.
- Update the comments in `McpAutoSync.h` and the pick-up paragraphs in `docs/mcp/DESIGN.md` and
  `docs/mcp/README.md` to list the new types.

#### 3.5.2 The pick-up (`editor/mcp/tools/ScriptTools.cpp`, `W4Shared.h`)

New function `w4::syncHtmlUi`, declared in `W4Shared.h` and defined in `ScriptTools.cpp` next to
`syncFiles`:

```cpp
// HTML UI documents using changed files (.html/.htm/.css and the images, svg, fonts, json/lottie they read).
// Every open scene, prefab stage and running Play scene: UIDocumentSystem::reloadChangedFiles (shares its
// stamps with phase 1's poller: a file is reloaded once, whoever sees it first). Changed .html/.css that no
// live document depends on are checked with ProjectChecks::checkUiFiles. Returns
// {reloaded: [{scene, entity: {id,name,path}, html, stylesheet_only}], current: <documents depending on a
//  changed file the poller had already reloaded>, problems: [{path, line, col, severity, code, message, suggestion}]}
// problems: the diagnostics located in a changed file, from the documents' (re)load or the check; Info dropped.
Json syncHtmlUi(McpContext& ctx, const std::vector<fs::path>& changed);
```

- It converts the changed absolute paths to asset-relative paths (the convention of
  `UIDocumentSystem`, phase 1 §3.8.6) before calling `reloadChangedFiles`, and reports paths
  project-relative.
- Phase 5 hooks `EffectRegistry::onProjectFileChanged(path)` into `reloadChangedFiles` (so
  this path and phase 1's watcher both get it), and adds `.frag` to the extensions that
  trigger `syncHtmlUi`: a `.frag` a live document depends on (through `@effect`, in
  `CompatDocument::dependencies`) reloads it like a `.css`.
- `syncFiles` (`ScriptTools.cpp:403`) calls it right after `reloadTextures` (`:565`) when any
  changed path is a UI file or an asset file.
- Its `problems` are appended to `result["problems"]` and its `reloaded` becomes
  `result["ui_reloaded"]` (omitted when empty), so `sync_files` reports both.
- `isTextFile` (`:114-122`) already accepts `.html .htm .css .rml .rcss` (phase 0 §3.7.5 M2);
  phase 4 relies on it for `write_file` / `read_file` / `code_editor`.

#### 3.5.3 In `McpAutoSync`

**Not playing.** `process` (`:510`) copies `result["ui_reloaded"]` into the report. Problem
entries keep the extra keys (`col`, `severity`, `code`, `suggestion`).

**Playing.** In the playing branch of `beforeCall` (`:582-615`), UI work is not deferred:

- changed `.html/.htm/.css`, fonts and `.json/.lottie` files go through `w4::syncHtmlUi(ctx, paths)`
  at once, which reloads both the running and the edit-time documents;
- they are not added to `deferred`;
- changed images go through `syncHtmlUi` and are also still deferred, so scene textures reload
  after Play as today;
- the report carries `ui_reloaded` and the problems;
- the `deferred` text is added only when non-UI files remain deferred.

**`attach` text (`:~760`).** A problem with `severity` `"warning"` is printed as
`path:line: warning: message [code]`, and errors keep today's format. When `ui_reloaded` is not
empty the text adds:

```
Reloaded UI: assets/ui/menu.html in "Menu"; 1 document(s) were already current.
```

**No double reload.** There are two cases:

- **The auto-sync sees the edit first.** It calls `reloadChangedFiles`, which records the new
  stamps. Phase 1's poller then finds them current and does nothing.
- **The poller sees it first.** It reloads and records the stamps. The auto-sync's scan still
  sees the change in its own map and calls `syncHtmlUi`. `reloadChangedFiles` reloads nothing,
  and the document is counted in `current`. Its diagnostics are still reported, taken from
  `getDiagnostics`.

`load_count` (exposed by `inspect_ui`) proves this in the tests.

**MCP writes.** Files written by tools go through `syncFiles` (`write_file`, the file step of
`create_ui_document`, the `import_files` rewrite). They are reloaded once and not reported as
external, as today.

### 3.6 MCP tools (`editor/mcp/tools/HtmlUiTools.cpp`)

The new module defines `void registerHtmlUiTools(McpRegistry&)`:

- declared in `editor/mcp/tools/McpTools.h`;
- called last in `McpServer::registerAllTools` (`McpServer.cpp:96-112`, after
  `registerWorldTools`);
- listed in `CMakeLists.txt` after `WorldTools.cpp` (`:565`);
- given its own section in `tools/mcp/gen_tool_docs.py` by adding `("HtmlUiTools", "HTML/CSS UI")`
  to `MODULES` (`:28-43`).

It follows `CoreTools.cpp` style: `schema::` builders, `call->fail` with the fix, compact text plus
`structured`.

The shared helper header `editor/mcp/tools/HtmlUiShared.h` exposes this to `PlayTools.cpp`:

```cpp
namespace jasperengine::editor::mcp::htmlui_mcp {
// The canvas point (CSS px = canvas px) at the center of the first visible element matching `selector`.
// documentRef null: every UI Document of the running scenes (else the open scenes), highest sortOrder
// first; the first document with a visible match wins. Fails with a message listing the documents searched.
bool elementCenter(McpContext& ctx, const Json& documentRef, const std::string& selector, double& x, double& y, std::string& error);
}
```

**Document resolution** is shared by all three tools.

- **Scene:**
  - while playing: `w3::resolveRuntimeScene(project, args["scene"])` (null means the played
    scene) and `w3::resolveLiveEntity`;
  - otherwise: `McpRefs::resolveScene` (null means the selected scene) and
    `McpRefs::resolveEntity`.
- **`entity` given:** it must have a `UIDocumentComponent`. If it doesn't, the error is
  `GameObject "<path>" has no UI Document component. UI Documents in scene "<name>": <path> (<html>), ...`.
- **`entity` omitted:** use the scene's only document. If the scene has none, look through every
  running scene (play) or every open scene (edit). Exactly one document must be found:
  - none: `No UI Document in scene "<name>" (or any open scene): create one with create_ui_document.`;
  - several: `Several UI Documents: pass entity, one of: <scene>/<path> (<html>), ...` (at most 10).
- **Not loaded yet:** the call waits for `isLoaded` with
  `call->waitUntil(..., 5000, ...)`. If it is still not loaded, the call fails with
  `The document <html> did not load:` followed by its error diagnostics, one per line, as
  `path:line:col: message`.

Output `document` object, shared by all three tools:

```json
{"entity": {"id": 12, "name": "Menu", "path": "Menu"}, "scene": 1, "html": "assets/ui/main_menu.html",
 "viewport": [1280, 720], "load_count": 3, "render_target": "screen", "sort_order": 0, "visible": true,
 "diagnostics_count": {"error": 0, "warning": 2, "info": 5}}
```

`render_target` is `"screen"` or `"texture"`.

**Diagnostic entries:**
`{"severity", "code", "message", "suggestion", "path", "line", "col"}`. They are listed in source
order, at most 50, with `diagnostics_truncated: true` past that. Infos are only counted.

**Screenshots.** They are built in this module:

1. Call `w3::captureView(call, {view: "game", maxWidth: 0}, ...)`.
2. Decode the PNG with `stbi_load_from_memory`.
3. Map canvas rects into the image with `info.canvas_rect_in_image` and `info.canvas`.
4. Draw 2-px outlines:
   - cyan `#00c8ff`: matched or inspected elements;
   - red `#ff3b30`: failures;
   - amber `#ffcc00`: the overlap partner or container of a failure.
5. Crop to the union of the outlined rects plus 24 px, at least 320 px wide, clamped to the
   canvas rect.
6. Scale down to `max_width` (default 768) with a box filter and encode with
   `stbi_write_png_to_func`.

The image is omitted, with a note in the text, in these cases:

- the document renders to a texture;
- the document's scene is not the one the Game view shows (stopped: the selected scene; play: the
  running scenes);
- a `viewport` override was requested.

#### 3.6.1 `inspect_ui`

Annotations: `readOnly: true`, `idempotent: true`. `allowedWhilePlaying: true`, `activity: Read`.

The description is at most 520 chars and says:

> Inspect a live HTML/CSS UI document: element tree (ref = reusable selector, box [x,y,w,h] in
> canvas px = send_input coords, flags overflow/clipped/hidden/offscreen), diagnostics, and with
> detail full the box model + computed styles with the file:line of the rule that set each. at:
> hit test. file: a .html/.css's diagnostics without a document (same as `jasperengine-editor
> check --ui`). Edit mode has no Lua: dynamic content appears in play.

Input schema:

```cpp
object({
    {"scene", sceneRef()},
    {"entity", entityRef("GameObject with a UI Document (default: the only one).")},
    {"file", assetPath(".html/.css: its diagnostics only, no live document.")},
    {"selector", string("CSS selector (default: whole document).")},
    {"at", vec2("Hit test at canvas px: the element stack there.")},
    {"detail", withDefault(stringEnum("full: box model, attributes, computed styles + sources (max 20 elements).", {"tree", "full"}), "tree")},
    {"depth", integer("Descendant levels (default tree 3, full 0).", 0, 16)},
    {"styles", array(Json{{"type", "string"}}, "Computed CSS properties to show per element.")},
    {"limit", withDefault(integer("", 1, 1000), 200)},
    {"models", boolean("Data-model values (play).")},
    {"viewport", vec2("Lay out at [w,h] first (restored after).")},
    {"screenshot", boolean("Game view image cropped to the matches, outlined.")},
    {"max_width", w3::maxWidthArgSchema(4096, 768)}
})
```

Rules:

- `file` cannot be combined with `selector`, `at`, `detail`, `styles`, `models`, `viewport` or
  `screenshot`. The error: `file inspects a file's diagnostics; drop <names> or pass entity instead.`
- `at` takes precedence over `selector`. `detail: "full"` caps `limit` at 20.
- `viewport` together with `screenshot` fails with
  `screenshot shows the current layout; call again without viewport for the image.`
- `models` outside play returns `models: {}` and the note
  `Data models are filled by Lua, which runs only in play.`

Output (structured):

```json
{"document": {...},
 "diagnostics": [...],
 "elements": [{"i": 0, "parent": -1, "depth": 0, "ref": "body", "tag": "body", "box": [0, 0, 1280, 720]},
              {"i": 3, "parent": 2, "depth": 2, "ref": "#play", "tag": "button", "id": "play", "class": ["btn", "primary"],
               "text": "Play", "box": [472, 248, 336, 56], "pseudo": ["hover"], "flags": ["overflow-x"],
               "styles": {"color": "rgb(255, 255, 255)"}}],
 "truncated": false,
 "hit": [...same element shape, topmost first...],
 "details": [{"ref": "#play", "box": {"content": [..], "padding": [..], "border": [..], "margin": [..],
              "padding_edges": [t, r, b, l], "border_edges": [..], "margin_edges": [..],
              "scroll": {"width": 336, "height": 56, "client_width": 336, "client_height": 56, "left": 0, "top": 0}},
              "attributes": {"id": "play", "class": "btn primary"},
              "styles": [{"property": "background-color", "value": "rgb(79, 140, 255)",
                          "source": "assets/ui/main_menu.css:48:3", "selector": ".btn.primary"}]}],
 "models": {"hud": {"health": 75, "ammo": 24}}}
```

Notes on the shape:

- Boxes are rounded to integers in `elements` and to 1 decimal in `details`.
- Keys whose value is empty or false (`id`, `class`, `text`, `pseudo`, `flags`, `styles`) are
  left out.
- Models are serialized with `w3::luaToJson`, depth 6, at most 100 items.

`file` mode output:

```json
{"file": "assets/ui/menu.css", "diagnostics": [...],
 "documents_using": [{"scene": 1, "entity": {...}, "html": "..."}]}
```

The text is an outline, one line per element with two spaces per depth level:

```
assets/ui/main_menu.html in "Menu" (scene "Main"), 1280x720, load 3: 0 errors, 2 warnings
assets/ui/main_menu.css:31:5: warning: 'mix-blend-mode' is not supported; dropped (fix: ...) [HTMLUI-CSS-0002]
body [0,0 1280x720]
  div.menu [440,120 400x480]
    h1.title "Skyward" [472,152 336x64]
    nav.buttons [472,248 336x288]
      button#play.btn.primary "Play" [472,248 336x56] :hover
      button#settings.btn "Settings" [472,316 336x56] !overflow-x
  div#settings-panel.panel [880,0 400x720] hidden
```

- Flags print as `!overflow-x`, `!overflow-y`, `!clipped`, `hidden`, `transparent`, `offscreen`,
  `zero-size`.
- Requested styles print as `{color: rgb(...); font-size: 16px}`.
- The text is capped at 12,000 characters. Past that it ends with
  `... (N more elements: narrow with selector/depth)`.
- `detail: full` prints the box model and the style lines as `property: value  <- source (selector)`.

Errors:

- an invalid selector: the inspector's message;
- an unknown property in `styles`: `unknown CSS property "colr" (did you mean color?)`, with
  suggestions from the compat property table;
- `file` that is not `.html/.htm/.css`, or does not exist: `McpRefs::resolveProjectPath` error.

#### 3.6.2 `assert_ui`

Annotations: `readOnly: true`, `idempotent: true`. `allowedWhilePlaying: true`, `activity: Read`.

The description is at most 520 chars:

> Assert the layout of a live HTML/CSS UI document; returns passed + each failure (element ref,
> numbers, rects) and on failure an image with failing boxes outlined. A check {type, selector}:
> exists, count (count|min|max), visible (unobstructed), text_fits (allow_scroll, single_line),
> no_overlap (with), within_parent (container), within_viewport, contrast (min_ratio, background
> when the UI is transparent there), min_size (width, height). viewports: also run at other
> sizes. Zero matches fail.

Input schema:

```cpp
object({
    {"scene", sceneRef()},
    {"entity", entityRef("GameObject with a UI Document (default: the only one).")},
    {"checks", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 50}, {"items", object({
        {"type", stringEnum("", {"exists", "count", "visible", "text_fits", "no_overlap", "within_parent",
                                 "within_viewport", "contrast", "min_size"})},
        {"selector", string("")},
        {"with", string("")},
        {"container", string("")},
        {"count", integer("", 0, 100000)}, {"min", integer("", 0, 100000)}, {"max", integer("", 0, 100000)},
        {"tolerance", number("px, default 0.5.", 0, 1000)},
        {"min_ratio", number("Default WCAG AA.", 1, 21)},
        {"background", color("")},
        {"unobstructed", boolean("")}, {"allow_scroll", boolean("")}, {"single_line", boolean("")},
        {"width", number("", 0, 10000)}, {"height", number("", 0, 10000)}
    }, {"type", "selector"})}}},
    {"viewports", array(vec2(""), "[[w,h], ...] (max 6); default the current.")},
    {"screenshot", withDefault(stringEnum("", {"on_failure", "always", "never"}), "on_failure")},
    {"max_width", w3::maxWidthArgSchema(4096, 768)}
}, {"checks"})
```

Handler:

1. Resolve the document.
2. For each viewport (the current one when none is given), run every check through
   `HtmlUIAssertions::run`. A viewport other than the current one runs inside
   `HtmlUIInspector::withViewport`.
3. Collect the results.
4. Take a screenshot when `screenshot` asks for one. Failures at other viewports are listed
   without an image, and the text says so.

`background` arrives as `[r,g,b]` 0-1 sRGB (`schema::color`) and becomes `hasBackground` +
`background`.

Output:

```json
{"document": {...}, "passed": false, "failed": 1, "total": 6,
 "checks": [{"index": 0, "type": "text_fits", "selector": ".btn", "viewport": [1280, 720], "passed": false, "matched": 4,
             "failures": [{"ref": "#buy-legendary", "message": "text is 166px wide in a 120px content box (46px over)",
                           "rect": [520, 400, 120, 40]}],
             "inconclusive": []}]}
```

- Failures are capped at 20 per check, with `failures_truncated`.
- The text is `assert_ui: 5/6 passed.` followed by one line per failure:

  ```
  FAIL text_fits ".btn" @1280x720: #buy-legendary: text is 166px wide in a 120px content box (46px over)
  ```
- A failed assertion is not a tool error: `isError` stays false, and `passed` says it. The tool
  errors (`isError`) are for bad arguments, an invalid selector, or no document.

#### 3.6.3 `create_ui_document`

Annotations: `readOnly: false`, `destructive: false`. `allowedWhilePlaying: false`,
`activity: Edit`.

The description is at most 480 chars:

> Create a GameObject with a UI Document showing an HTML/CSS file (one scene undo step). A missing
> html is created from template (blank or an example: main_menu, pause_menu, hud, inventory,
> settings, dialogue, shop, toasts, loading) with its .css, assets and Lua logic (script: attached).
> Existing files are kept unless overwrite. The html must be inside the assets dir (e.g.
> assets/ui/menu.html). Returns the GameObject, files and the first load's diagnostics.

Input schema (the `template` enum is built from `HtmlUiExamples::names()` + `blank` at
registration):

```cpp
object({
    {"scene", sceneRef()},
    {"parent", entityRef("Parent (default: scene root).")},
    {"name", string("GameObject name (default: from the file name).")},
    {"html", assetPath("Project-relative .html in the assets dir.")},
    {"template", withDefault(stringEnum("", templateNames), "blank")},
    {"script", boolean("Create + attach the template's Lua (default: true when it has one).")},
    {"sort_order", integer("", -32768, 32767)},
    {"overwrite", boolean("Replace existing files.")},
    {"properties", anyObject("Other UI Document properties.")}
}, {"html"})
```

Handler steps:

1. Resolve the scene; it must be stopped (as `sceneStopped` in WorldTools). Validate `html`:
   - it ends in `.html` or `.htm`;
   - it resolves inside the project (`McpRefs::resolveProjectPath`);
   - it is inside `project->getAssetsPath()`. Otherwise:
     `html must be inside the assets directory (<assetsDir>/), e.g. <assetsDir>/ui/<stem>.html.`
2. Files:
   1. Call `HtmlUiExamples::instantiate(template, htmlRel, luaDirRel, out)` (3.7.4).
   2. Files that already exist are kept and listed in `files_kept`, unless `overwrite`.
   3. When the html already exists and `overwrite` is false, nothing is written from the template.
      The existing html is used, and the template Lua is still written and attached if `script` is
      true and its file is missing.
   4. Write with `w4::writeFilesUndoable`: one step of the project history, which runs `syncFiles`.
3. Scene step (one undo step): `UndoStep`-style mark on the scene history.
   1. `CreateEntityCmd(name, EntityCreationType::GAMEOBJECT [, parent])`.
   2. `AddComponentCmd(ComponentType::UIDocumentComponent)`.
   3. A `MultiPropertyCmd` with the component property `filename` set from `html` (converted to the stored assets-relative form), `sortOrder`
      and the `properties` entries (Catalog names through `McpValues::buildPropertyCommand`).
   4. When a script is wanted: `server.runNested(call, "attach_script", {scene, entity, path})`.
   5. `groupSince(mark)`.

   If anything fails, the scene step is rolled back and the project file step is undone, so
   nothing is left behind. The error then starts with
   `Nothing was created (the call was rolled back): `.
4. Wait for the load (`waitUntil isLoaded`, 5 s), then reply.

Output:

```json
{"entity": {...}, "scene": 1, "html": "assets/ui/main_menu.html", "template": "main_menu",
 "files_written": ["assets/ui/main_menu.html", "assets/ui/main_menu.css", "assets/ui/main_menu_assets/logo.svg", "scripts/MainMenuUI.lua"],
 "files_kept": [], "script": {"path": "scripts/MainMenuUI.lua", "attached": true},
 "document": {...}, "diagnostics": [...]}
```

The text says what was created, followed by
`Undo: undo (scene) removes the GameObject; undo scope project removes the files.`

Errors: the scene is playing, html outside assets, not `.html`, an unknown property name (with
the Catalog list), a failure of `attach_script` (its message).

#### 3.6.4 `send_input` extension (`editor/mcp/tools/PlayTools.cpp`)

- Mouse events (`:2285-2300`) and touch events (`:2403`) accept `target` (string selector),
  `to_target` (selector, for `drag` / `swipe`) and `document` (entity ref).
- When `target` is set, the event's point comes from `htmlui_mcp::elementCenter` and overrides
  `x`/`y`/`position` and `space` for that point. `to_target` does the same for `to`.
- Resolution happens inside the event's closure when the event runs, after the earlier events.
  So a panel opened by the previous click can be targeted.
- A failure fails the event with the helper's message, as other event errors do today.
- Schema: the `events` items are free-form objects today, so only the description grows (at most
  +200 chars). Append:
  `mouse/touch: target / to_target = CSS selector of a UI Document element (its center; document: which).`
- `PlayViewCommon.h` gets `Json w3::luaToJson(Project*, lua_State*, int index, int maxDepth, size_t maxItems)`,
  a wrapper around the existing `LuaSerializer` (`PlayTools.cpp:~750`), for `inspect_ui` models.

#### 3.6.5 `import_files` extension (`editor/mcp/tools/AssetTools.cpp`)

The handler is `importFilesTool` (`:1202`) and its registration is at `:2353-2364`.

- `sources` items may also be `http(s)://` URLs, which go through `importUrl`.
- With the extension in place, the suggestions of `HTMLUI-IMP-0001`/`-0009` name it again
  (`engine/core/htmlui/compat/UrlResolver.cpp` and the registry text in `DiagnosticCodes.cpp`).
  Phase 2 says "import it: load the document in the editor (it imports remote resources), or
  run jasperengine-editor import-ui": until this section is implemented `import_files` copies
  local files only ("No downloads"), so naming it sent an agent to a tool that refused (changed
  by p2-integrate). Add "MCP import_files from_document" to both suggestions, and regenerate the
  `.diag.txt` goldens that hold them (`fixtures/compat/css/{background-url,import,font-face}`,
  `fixtures/compat/html/{images,styles_links}`, `compat_url_test.cpp`).
- New `fonts` (string array) uses `importGoogleFonts`.
- New `icons` (string array, `lucide:<name>` or a bare name) uses `importLucideIcons`.
- New `from_document` (project-relative `.html`/`.css`) uses `importDocumentReferences`. The URLs
  in the file are then replaced with paths relative to that file through
  `w4::writeFilesUndoable`, one project undo step, unless `rewrite: false`. For a Google Fonts
  `<link>` / `@import` the replacement is the generated `fontFaceCss`.
- `sources` is no longer required: at least one of `sources`, `fonts`, `icons` or
  `from_document` must be present, otherwise
  `pass sources, fonts, icons or from_document.`
- `to_dir` is required with local `sources`. It defaults to `<assetsDir>/ui/imported` for the
  remote kinds.
- Every written file goes through `syncFiles`.
- The description changes `No downloads.` to:

  > Remote (network): URLs in sources, fonts ["Inter:400,700"] (Google Fonts → .ttf + @font-face
  > css), icons ["lucide:play"] (.svg), from_document (import every remote URL a .html/.css uses
  > and rewrite it).

  That is at most +300 chars.
- Output gains `{"remote": [{"source", "files": [...]}], "font_face_css"?, "rewritten"?: {"file", "replacements": [{"from", "to"}]}, "failed": [{"source", "error"}]}`.
  Partial failure is not `isError` unless nothing was imported.

#### 3.6.6 Resources

Registered in `HtmlUiTools.cpp`:

- `jasper://examples/html-ui` (text/markdown): an index generated from `examples.json`. For each
  example: name, title, description, what it demonstrates, files, and its `checks`. It ends with
  `create_ui_document {html, template}` usage.
- Template `jasper://examples/html-ui/{+path}`: any embedded file (`main_menu/main_menu.css`),
  with mime `text/html`, `text/css`, `text/x-lua`, `image/svg+xml`, `application/json`, or
  `text/plain` for anything else.

`docsIndex` (`DocsTools.cpp:153`) gains one line:
`- jasper://examples/html-ui: HTML/CSS UI examples (menus, HUD, inventory, settings...) to copy`.

#### 3.6.7 Budget

- Measured before phase 4 with `tools/mcp/tool_budget.py` on an empty project: **130 tools,
  92,956 chars**. The README's "under ~90K" target is already exceeded.
- Phase 4 adds 3 tools and must stay within these limits (name + description + compact
  `inputSchema`):
  - `inspect_ui` ≤ 2,300
  - `assert_ui` ≤ 2,400
  - `create_ui_document` ≤ 1,300
  - `send_input` growth ≤ 250
  - `import_files` growth ≤ 600
- The total must stay ≤ 99,800. `test_html_ui_tools.py` asserts every one of these numbers.
- The README sentence becomes:
  "target: under ~100K characters, about 25K tokens; the HTML UI tools brought it to ~99K: new
  tools must pay for themselves by trimming".
- Shared conventions (document resolution, refs, boxes in canvas px) are explained once, in the
  instructions' HTML-UI section (3.8.3), not repeated per tool.
- Phase 5 adds `list_ui_effects`, `set_ui_effect_time` and an `effects` key on `inspect_ui`
  elements (omitted when empty; `InspectElement` gains `std::vector<UIEffectInfo> effects`).
  It raises the caps above in `test_budget` by the limits it states
  (`phase-5-effects.md` §3.13) and adds its module to `gen_tool_docs.py` `MODULES`.

### 3.7 Example library

#### 3.7.1 Location and layout

```
docs/mcp/guide/html-ui-examples/
  examples.json                 manifest (below)
  LICENSE-lucide.txt            ISC license of the Lucide icons inlined in the examples
  blank/      blank.html blank.css BlankUI.lua
  main_menu/  main_menu.html main_menu.css MainMenuUI.lua assets/...
  pause_menu/ pause_menu.html pause_menu.css PauseMenuUI.lua
  hud/        hud.html hud.css HudUI.lua assets/compass.svg
  inventory/  inventory.html inventory.css InventoryUI.lua assets/items/*.svg
  settings/   settings.html settings.css SettingsUI.lua
  dialogue/   dialogue.html dialogue.css DialogueUI.lua assets/portrait.svg
  shop/       shop.html shop.css ShopUI.lua assets/items/*.svg
  toasts/     toasts.html toasts.css ToastsUI.lua
  loading/    loading.html loading.css LoadingUI.lua assets/spinner.json
```

Rules every example follows:

- It is standalone: one `.html` linking one `.css`, design tokens as CSS custom properties in
  `:root`, assets referenced relative to the html as `assets/...`.
- Fonts are generic families only (`system-ui, sans-serif`, `monospace`), mapped to the built-in
  fonts. Icons are inline Lucide SVGs with `stroke="currentColor"` (ISC; notice in
  `LICENSE-lucide.txt`; one line added to `NOTICE.md`). The Lottie spinner is authored by us.
  Nothing remote.
- The style is modern game UI: dark translucent panels, an accent color per example,
  `backdrop-filter`, gradients, shadows, `border-radius`, transitions, keyframes, `anime`
  entrances. It is written the way a web developer writes it: flex and grid, `gap`, `rem`,
  `clamp()`, `var()`, `::before`. Phases 2-3 make all of that work.
- `ExamplesCompatTest` and `test_html_ui_examples.py` require zero warnings and zero errors.
  Infos are allowed.
- Lua:
  - class `<Pascal>UI`, declared `---@class <Pascal>UI : ScriptInstance`;
  - the document is taken with `UIDocument(self.scene, self.entity)`;
  - element lookups go through a local `byId` that `assert`s, so LuaLS sees no nil;
  - listeners are bound in a method registered on `doc.onLoad` and also called once from `init`,
    so they survive hot reload;
  - zero LuaLS diagnostics of Warning or above.
- Sizes: html ≤ 200 lines, css ≤ 450 lines, Lua ≤ 250 lines per example.
- Layout passes its manifest checks at each listed viewport, with media queries where needed.
- Each example states, in a comment at the top of its html, its purpose and the engine features
  it demonstrates.

`examples.json`:

```json
{"version": 1, "examples": [
 {"name": "main_menu", "title": "Main menu", "example": true,
  "description": "Title screen with animated background, icon buttons, keyboard navigation and a slide-in settings panel.",
  "demonstrates": ["flex", "transitions", "keyframes", "anime timeline + stagger", "inline svg icons", ":focus / keyboard nav", "class toggling from Lua"],
  "html": "main_menu.html", "css": "main_menu.css", "lua": "MainMenuUI.lua", "assets": ["assets/logo.svg"],
  "viewports": [[1280, 720], [1920, 1080], [1024, 768], [720, 1280]],
  "checks": [{"type": "count", "selector": ".menu-button", "count": 4}, ...]},
 ...]}
```

`blank` has `"example": false`. Its `checks` are `[{"type": "visible", "selector": "h1"}]`.

#### 3.7.2 The examples

Every example runs in play at 1280×720, starting with `fixed_fps: 60, auto_pause: true` and
then `wait {frames: 90}`, before its checks and golden screenshot. Ids and classes listed here
are the public surface the tests and docs use.

| Example | Content | Lua behaviour | Manifest checks |
|---|---|---|---|
| **main_menu** | A full-bleed backdrop of layered radial gradients, with three blurred blobs drifting on transform keyframes. The logo `h1.logo` uses letter-spacing and `text-shadow`. `nav.menu` holds 4 `button.menu-button` (`#play`, `#settings`, `#credits`, `#quit`), each an inline Lucide icon plus a label, with hover and focus transitions. `footer.version` sits bottom-right. The flyout `aside#settings-panel` slides in from the right (class `open`) and has a `#settings-back` button. | `init`/`onDocumentLoad` bind the following: a click on `#play`, `#credits` or `#quit` logs `main_menu: <id>`; `#settings` adds `open` to the panel and `#settings-back` removes it. An anime timeline staggers the buttons in from `translateY 24px` / opacity 0 (600 ms, `outCubic`, stagger 70 ms). `keydown` ArrowUp/ArrowDown moves focus among the buttons and Enter clicks the focused one. | count `.menu-button` = 4; visible `.menu-button` unobstructed; text_fits `.menu-button` single_line; no_overlap `.menu-button`; within_viewport `.menu`; contrast `.menu-button`; min_size `.menu-button` |
| **pause_menu** | `div#pause.pause.open` is a full-screen dim with `backdrop-filter: blur(8px)`. Inside, the card `.pause-card` holds `h2` "Paused" and 4 `button.pause-button` (`#resume`, `#options`, `#restart`, `#to-menu`). | `init` removes `open` (the game starts unpaused). `RegisterEngineEvent(self, "onKeyDown")`: Escape toggles `open`, sets `Engine.timeScale` to 0 while open and restores it on close. `#resume` closes. The others log `pause_menu: <id>`. Opening fades and scales the card in (anime, 250 ms). | visible `.pause-card` (asserted after Escape); text_fits `.pause-button`; no_overlap `.pause-button`; within_viewport `.pause-card`; contrast `.pause-button`; min_size `.pause-button` |
| **hud** | `div.hud-panel` corners: top-left health and shield bars (`#health-fill`, `#shield-fill` with `data-style-width="health + '%'"`, and labels `{{health}}`); top-center `#score` and `#timer`; top-right minimap `.minimap` (circle, `border-radius: 50%`, compass svg, N/E/S/W labels, player arrow); bottom-right `#ammo` / `#reserve` with weapon name and icon; bottom-left 3 ability slots `.ability` with cooldown overlay heights (`data-style-height`); `#damage-vignette`. Data model `hud`. | `HudUI:damage(n)` lowers shield, then health, and flashes the vignette (anime opacity 0→0.6→0, 400 ms). `HudUI:fire()` decrements ammo. `HudUI:reload()` refills from reserve. `onUpdate` advances the timer (mm:ss) on game time. The property `demo` (bool, default false) fires and damages periodically for previews. | within_viewport `.hud-panel`; no_overlap `.hud-panel`; text_fits `#ammo, #reserve, #score, #timer, .hud-label`; contrast `.hud-label` background [0.5,0.5,0.5]; visible `.minimap` |
| **inventory** | `div.inventory` window with header (title, `#gold` with coin icon, `#close` X); left `.equipment` (head, chest, weapon slots); right `div.grid` a CSS grid of 6×4 `div.slot` from `data-for="item, i : items"` (an icon `img` via `data-attr-src`, a count badge, `data-class-rare`, `data-class-epic`); a hover tooltip `#tooltip` (name, rarity, description) positioned near the slot; footer weight bar `#weight-fill`. Data model `inventory`. | Items: 10 demo items, the rest empty. Drag and drop: slots carry `drag="drag-drop"`; `dragdrop` swaps the model entries and marks them dirty. `mouseover` on a slot fills and positions `#tooltip` (class `show`); `mouseout` hides it. `#close` hides the document. | count `.slot` = 24; no_overlap `.slot`; within_parent `.slot` container `.grid`; within_viewport `.inventory`; text_fits `.tooltip-title` (with the tooltip shown); visible `.grid` |
| **settings** | `div.settings` window: tabs `button.tab` (`#tab-graphics`, `#tab-audio`, `#tab-controls`, `#tab-gameplay`, class `active`) and one `section.panel` each. Rows `.row` with a `label` and a control: `input[type=range]` (master/music/sfx volume, value labels `{{settings.music}}%`), switches (`input[type=checkbox]` styled with `::before`), `select` (resolution, quality), key-binding buttons `.keybind`. Footer `#apply`, `#reset`, `#back`. Data model `settings`, two-way `data-value` / `data-checked`. | Clicking a tab switches the `active` class and panel visibility. A `.keybind` click shows "Press a key…" and captures the next `keydown` into the model. `#reset` restores the defaults. `#apply` logs `settings: apply` plus the JSON-ish model. | visible `#tab-graphics`; no_overlap `.tab`; min_size `.tab`; text_fits `.row label`; within_parent `.row` container `.panel`; within_viewport `.settings` |
| **dialogue** | `div.dialogue` bottom box: portrait `.portrait` (circular svg avatar), name plate `#speaker`, text `#line.dialogue-text`, continue indicator `.continue` (bouncing keyframes), `ul#choices` of `button.choice` shown after the line. | `DialogueUI:say(speaker, text, choices)`. A typewriter reveals 40 chars/s by game time in `onUpdate`. A click or Space completes the line, then advances. Choices branch through a small script table, and a picked choice logs `dialogue: choice <n>`. `init` starts the demo conversation. | text_fits `.dialogue-text` allow_scroll false; within_viewport `.dialogue`; contrast `.dialogue-text`; no_overlap `.choice`; min_size `.choice` |
| **shop** | `div.shop`: header with merchant name and `#gold`; filter chips `button.chip` (All/Weapons/Armor/Potions, class `active`); `div.grid` of `div.card` (icon, `.name`, `.rarity`, `.stats`, `.price`, `button.buy` with `data-attr-disabled="gold < item.price"`); modal `#confirm` (quantity stepper `#qty-minus`/`#qty-plus`, `#confirm-buy`, `#confirm-cancel`). Data model `shop`. | A chip filters the cards (`data-if`). Buy opens `#confirm`. Confirm subtracts gold, animates the counter (anime on a number updating `gold` per frame) and logs `shop: bought <id> x<n>`. A card the player cannot afford has its button disabled. | text_fits `.card .name, .card .price`; no_overlap `.card`; within_parent `.card` container `.grid`; contrast `.price`; min_size `.buy`; within_viewport `.shop` |
| **toasts** | `div#toasts.toast-stack`, top-right column. Toast markup: `div.toast.toast-<kind>` (info/success/warning/error) with an icon, `.message`, and a `.toast-progress` bar. Three `[data-sample]` toasts in the markup show the design in edit mode. | `init` removes the samples. `ToastsUI:show(text, kind, seconds)` creates the toast (`createElement`, `innerHTML`, `appendChild`) and enters with anime (translateX 120%→0, opacity, 300 ms `outBack`). It auto-dismisses after `seconds` (default 3, game time) with an exit animation and then `remove()`. At most 5 (the oldest goes). `init` shows 3 demo toasts. | within_viewport `.toast`; no_overlap `.toast`; text_fits `.toast .message`; contrast `.toast .message` |
| **loading** | `div.loading` full screen: gradient art with keyframed drifting dots, title, `lottie#spinner` (`assets/spinner.json`), `#progress-track` > `#progress-fill` (`data-style-width`), `#percent` `{{progress}}%`, `.tip` (rotating tips, fade), `#press-key` (pulse keyframes, `data-if="progress >= 100"`). Data model `loading`. | Progress is simulated over the property `duration` (float, default 3 s of game time). Tips rotate every 4 s with a cross-fade. At 100 the script logs `loading: done` and shows `#press-key`, and the next key or click logs `loading: continue`. | visible `#spinner, #progress-fill`; text_fits `.tip, #percent`; within_viewport `.loading-card` |

#### 3.7.3 Embedding and the project copy

CMake (`CMakeLists.txt`, after the docs block at `:343-361`):

- `file(GLOB_RECURSE ... CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/mcp/guide/html-ui-examples/*")`
  generates `${GENERATED_DIR}/html_ui_examples.h`:
  - `static const unsigned char` arrays;
  - `struct HtmlUiEmbeddedFile { const char* path; const unsigned char* data; unsigned int size; }`;
  - `html_ui_embedded_files[]`, where `path` is relative to the examples root.
- Each file is a `CMAKE_CONFIGURE_DEPENDS`, the same pattern as `mcp_docs.h`.
- `${EDITOR_DIR}/util/HtmlUiExamples.cpp` is added to the source list next to
  `ProjectChecks.cpp` (`:582`). It is the only translation unit that includes the generated
  header.

`editor/util/HtmlUiExamples.h`:

```cpp
namespace jasperengine::editor {
class HtmlUiExamples {
public:
    struct File { std::string path; std::string_view data; };     // path relative to the examples root
    static const std::vector<File>& files();
    static const nlohmann::json& manifest();                        // examples.json
    static std::vector<std::string> names();                        // manifest order, blank included last
    static bool has(const std::string& name);
    // Writes the whole tree under dir (FileUtils::writeIfChanged; removes files no longer embedded)
    static bool writeTo(const std::filesystem::path& dir, std::string& error);
    struct Instance {
        std::vector<std::pair<std::string, std::string>> files;   // project-relative path -> content
        std::string luaPath;                                       // "" when the template has no Lua
        std::string luaClass;
    };
    // The files of template `name` for a document at htmlRel (project-relative), Lua in luaDirRel (see 3.7.4)
    static bool instantiate(const std::string& name, const std::string& htmlRel, const std::string& luaDirRel,
                            Instance& out, std::string& error);
};
}
```

`Generator::writeEditorTooling` (`Generator.cpp:1795-1808`) adds the call
`HtmlUiExamples::writeTo(projectInternalPath / "engine-api" / "html-ui-examples", error)`, logged
with `Out::error` on failure. So every opened or created project has the examples where an agent
greps. They are regenerated like the stubs and never committed (`.jasperengine/`).

#### 3.7.4 `instantiate` rules

With `stem` the html file name without extension, `dir` its directory, and `Pascal(stem)` the
words of the stem (split at `_`, `-` and spaces) capitalized and joined:

| Template file | Written to | Text substitutions |
|---|---|---|
| `<n>/<n>.html` | `htmlRel` | `href="<n>.css"` → `href="<stem>.css"`; `"assets/` and `'assets/` → `"<stem>_assets/` / `'<stem>_assets/` |
| `<n>/<n>.css` | `<dir>/<stem>.css` | `url(assets/`, `url("assets/` and `url('assets/` → the same with `<stem>_assets/` |
| `<n>/assets/**` | `<dir>/<stem>_assets/**` | none |
| `<n>/<Class>UI.lua` | `<luaDir>/<Pascal(stem)>UI.lua` | every whole-word `<Class>UI` → `<Pascal(stem)>UI`; `"assets/` → `"<stem>_assets/` |

`blank` writes:

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>{Title}</title>
  <link rel="stylesheet" href="{stem}.css">
</head>
<body>
  <main class="screen">
    <h1>{Title}</h1>
  </main>
</body>
</html>
```

The css holds `:root` tokens, `body { margin: 0; font-family: system-ui, sans-serif; color: #f4f6fb; }`
and a centered `.screen` flex container. `{Title}` is the stem in title case. `BlankUI.lua` gets
the document, logs once, and has an `onDocumentLoad` stub. `script` defaults to false for `blank`
and to true for the examples.

### 3.8 Docs, instructions, stubs, API search

#### 3.8.1 `docs/mcp/guide/html-ui.md` (served as `jasper://docs/html-ui`, split by DocsTools above 16 KB)

The file already exists when phase 4 starts: p0-integrate created it (phase 0 feature set),
and phases 1–3 added sections (phase 1 per package: rendering, input, editor workflow, hot
reload, scripting, SVG/Lottie, animation, documents on meshes, text effects; phase 2 "Writing
HTML/CSS for the engine"; phase 3 the newly native features and the §2.2 limits). p4-guide
reorganises it into the outline below, keeping every fact those sections hold (moved into the
matching section, not dropped). Phase 5 later appends one more top-level section, "Effects"
(`phase-5-effects.md` §3.13), and adds its title to `test_guide_served`.

Outline. Every name in it must exist: tools, Lua names and CSS features. The examples are
copied from the example files.

1. **HTML/CSS UI in JasperEngine.** When to use it versus component UI (`create_ui`): menus,
   HUDs and dialogs go to HTML. The model: one document per GameObject, drawn over its scene's
   camera, CSS px = canvas px, `sortOrder`, render-to-texture. No browser and no JavaScript:
   logic is Lua.
2. **Quick start.** `create_ui_document` → edit the files → read `external_changes` →
   `inspect_ui` → `assert_ui` → `screenshot` → `play` → `send_input {target}` → `read_console`,
   shown as a short sequence of tool calls with arguments.
3. **Files.** `assets/ui/<name>.html|.css`, `<name>_assets/`, the Lua script in the Lua dir,
   relative URLs, no remote URLs, what export ships.
4. **HTML.** The elements that work (link to the support matrix): forms (`input` range,
   checkbox, text; `select`; `button`), `img`, inline `svg`, `<lottie>`, and what to use instead
   of `<script>`, `on*` attributes, `<canvas>`, `<video>`, `<iframe>`.
5. **CSS.**
   - Works: flex, grid, `calc`/`clamp`/`var`, transitions, keyframes, transforms, filters,
     `backdrop-filter`, gradients, `::before/::after`, media queries, `!important`.
   - Differs: one paragraph per notable limit (`@container`, `mix-blend-mode`, `writing-mode`,
     multi-column, ... — the phase-3 §2.2 list), with the replacement.
   - Units, fonts (generic families, `@font-face` with project TTF/OTF, Google Fonts via
     `import_files fonts`) and icons (`import_files icons`, `currentColor`).
6. **Lua API.** Getting the document, querying, events (table of types and `UIEvent` fields),
   `classList` / `style` / `textContent` / `innerHTML` / attributes / `dataset`, form values,
   creating and removing elements, show and hide, focus. Hot reload and `onLoad`. A complete
   example script.
7. **Data binding.** The markup attributes, the Lua model proxy, what triggers an update, and
   lists with `data-for`. Example (the HUD). Changing values from a test: `eval_lua` with
   `getDataModel`.
8. **Animation.** CSS transitions and keyframes versus `anime`: `animate`, `stagger`,
   `createTimeline`, `createSpring`, `utils`, the easing names. The UI clock is unscaled game
   frame time. Examples.
9. **Images, SVG, Lottie, fonts, icons.** Importing with `import_files`
   (`fonts` / `icons` / URLs / `from_document`).
10. **Input and focus.** Pointer, keyboard and gamepad reach the focused document first. Events a
    document handles do not reach the game. `pointer-events: none` for click-through HUDs.
11. **Responsive layout.** Canvas size and scaling mode, media queries, testing with `assert_ui
    viewports` and `inspect_ui viewport`.
12. **The loop in detail.** Reading diagnostics (codes, suggestions), the `check --ui` CLI and
    its hook, `inspect_ui` outputs (refs, flags, style sources), `assert_ui` patterns per screen
    type, screenshots with outlines.
13. **Habits from the web that need a change.** A table of about 25 rows, LLM habit → engine
    version:
    - `<script>` → Lua script;
    - `onclick=` → `addEventListener`;
    - `document.querySelector` → `doc:querySelector` (colon);
    - `innerText` → `textContent`;
    - `fetch` / `localStorage` → engine APIs;
    - `@container` queries → media queries or classes from Lua;
    - `mix-blend-mode` → opacity or a pre-blended image;
    - Google Fonts `<link>` → `import_files from_document`;
    - remote `<img>`;
    - `100vh` on mobile;
    - hover-only affordances on touch;
    - ...

    Each row names the diagnostic code.
14. **Examples.** A table of the 9 examples: what each demonstrates, how to get it
    (`create_ui_document template`, `.jasperengine/engine-api/html-ui-examples/`,
    `jasper://examples/html-ui`), and the `checks` they ship.
15. **Performance.** Prefer animating transforms and opacity over layout properties, the cost of
    layers and filters, keeping documents small, hiding instead of rebuilding.
16. **C++.** The `UIDocument` C++ wrapper in brief, pointing to the headers in
    `.jasperengine/engine-api`.

`docs/mcp/guide/README.md` gains rows for `html-ui.md` and `html-ui-support.md`.

#### 3.8.2 `docs/mcp/README.md`, `docs/mcp/checks.md`, `docs/mcp/DESIGN.md`

- **README:**
  - The intro bullet list gains "build HTML/CSS game UI (menus, HUDs) and check its layout".
  - "Changes are picked up automatically" gains the new file types and the UI reload sentence.
  - The tool-budget sentence changes as in 3.6.7.
- **checks.md:**
  - `--ui` goes into the usage block and the option table.
  - The default becomes `--lua --shaders --ui`.
  - The output section gains a UI text example and the extra `code` / `suggestion` JSON keys.
  - `ui` goes into the `tool` list and `checked`.
  - A new section, "What the UI check runs" (compat conversion, the stylesheet rule, the
    exclusions).
  - The hook section gains "UI warnings are shown once" and the html/css tracking.
  - A `ui-support` paragraph.
- **DESIGN.md (mcp):** in "Automatic pick-up", the tracked-file list and one sentence: UI
  documents reload live (also during play) through `UIDocumentSystem::reloadChangedFiles`,
  sharing stamps with the editor's poller.

#### 3.8.3 Instructions (`editor/mcp/McpInstructions.cpp`)

1. Insert a new raw-literal chunk before `## Pitfalls` (`:248`). MSVC limits each chunk, so keep
   it under 16 KB on its own; this one is about 1.4 KB.
2. Replace the `- UI (text, image, ...)` bullet of `## 2D and 3D` (`:192-196`) with one that
   names both systems and points to the new section.

```
## HTML/CSS UI
Menus, HUDs and dialogs: write standard HTML + CSS; logic is Lua (no JavaScript, no browser).
create_ui_document makes a GameObject whose UI Document loads assets/ui/<name>.html (template:
blank, or an example such as main_menu, hud, inventory, settings, shop) and writes its .css and
Lua. Edit the files with your own tools: before your next call the documents reload (also while
playing) and external_changes lists what the engine drops or rewrites, with file:line:col, code
and a fix: fix those first. inspect_ui: element tree (ref = reusable selector, box in canvas px =
send_input coords, overflow/clipped/hidden flags), computed styles with the rule that set them,
hit test (at), data models. assert_ui: checks such as text_fits, no_overlap, within_parent,
within_viewport, visible, contrast, min_size, optionally at other viewports, with an outlined
image of failures. While playing, send_input mouse/touch events take target: "<selector>".
Remote fonts, icons, Lottie and images: import_files (fonts, icons, from_document). Edit mode has
no Lua, so dynamic content appears only in play. Reference: jasper://docs/html-ui (Lua API,
data binding, anime animations), jasper://docs/html-ui-support (what CSS works),
examples in .jasperengine/engine-api/html-ui-examples/.
```

`test_autosync.py`'s `pairs` (`:397-406`) gains:

```python
"create_ui_document": ["html", "template"],
"inspect_ui": ["selector", "at"],
"assert_ui": ["checks", "viewports"],
"import_files": ["fonts", "icons", "from_document"],
```

#### 3.8.4 AGENTS.md (`Generator::writeAgentsFile`, `Generator.cpp:1397-1492`)

After the "Working with the editor (MCP)" bullet list, add:

```
## HTML/CSS UI

Game UI (menus, HUDs, dialogs) is written in standard HTML and CSS with Lua logic, one document per GameObject
(UI Document component; MCP: create_ui_document). Put documents under `assets/ui/`. There is no JavaScript: a
`<script>` or `onclick` is reported, use the Lua element API (`UIDocument(self.scene, self.entity):querySelector(...)`,
`addEventListener`, `classList`, data models, `anime.animate`). `jasperengine-editor check --ui` (also in the default
check and the hook) reports every construct the engine drops or rewrites, with file:line:col and a fix. Supported CSS:
the `jasper://docs/html-ui-support` resource; guide: `jasper://docs/html-ui`; complete examples to copy:
`.jasperengine/engine-api/html-ui-examples/`.
```

The check bullet already present changes to
`(Lua syntax and load, shaders, HTML/CSS UI, C++ build; ...)`. Only new projects get the
section (AGENTS.md is written once); the guide says so.

#### 3.8.5 LuaLS stubs and API search

`generate_lua_stubs.py` (`OVERRIDES` at `:1931`) gets these additions:

- **Signatures.** Typed overloads with doc lines for every HTML UI binding that reads the Lua
  stack or takes tables: `UIDocument.createDataModel`, `getDataModel`, `querySelectorAll`,
  `UIElement.addEventListener` / `removeEventListener`, `classList` methods, `style`, `dataset`,
  every `anime.*`, and the Animation / Timeline methods.
- **Event names.** `---@alias UIEventType` with the event names of 3.2.2 plus `mousedown`,
  `mouseup`, `mousemove`, `dblclick`, `keyup`, `textinput`, `scroll`, `resize`, `animationend`,
  `transitionend` (whichever phase 1 dispatches). `addEventListener(type: UIEventType, fn: fun(event: UIEvent))`.
- **Parameter classes.** `---@class AnimeParams` (fields of 3.2.2 plus `[string]: any` for
  properties), `AnimeStaggerOptions`, `AnimeTimelineOptions`, `AnimeSpringOptions`.
  `UIRect {x, y, width, height}` for `getBoundingClientRect`.
- **Doc comments** on `UIDocument`, `UIElement`, `UIEvent` and `anime` linking
  `jasper://docs/html-ui`.

The stubs are written to a new emitted file, `HtmlUITypes.lua`, alongside the class files.
`test_stubs.py`'s "no `-- unresolved:`" check must stay green.

`generate_api_suggestions.py` (`parse_binding_file` at `:73`):

- It handles `beginNamespace("anime")` + `addFunction`/`addProperty` as `Function`/`Property`
  entries `anime.animate(...)`.
- A new `MANUAL_SYMBOLS` list gives the details (parameter text) of the HTML UI functions bound
  as `lua_CFunction`, so `search_engine_api` returns usable signatures.
- `searchDocs` indexes `html-ui.md` and `html-ui-support.md` automatically (they are embedded
  docs).

### 3.9 Test infrastructure

- **`tools/mcp/pngdiff.py`** is pure Python with no dependency beyond the standard library:
  - `decode(png_bytes) -> (w, h, rgba bytes)`: 8-bit RGB/RGBA, non-interlaced, all five filters;
  - `encode(w, h, rgba) -> bytes`;
  - `crop(img, x, y, w, h)`;
  - `downscale(img, w, h)` (box filter);
  - `compare(a, b, channel_tolerance=24, max_fraction=0.01) -> (ok, fraction, diff_png)`;
  - `flat(img) -> bool`.
- **Goldens** live in `engine/tests/htmlui/golden/examples/<name>.png` at 1280×720. How they are
  compared:
  1. `configure_game_view {aspect: "project", scale: 1}`.
  2. `screenshot {view: "game", max_width: 0}`.
  3. Crop to `canvas_rect_in_image`.
  4. Downscale both the crop and the golden to 640×360.
  5. Compare with tolerance 24 per channel; at most 1% of pixels may differ.
  6. On failure, write `<scratch>/<name>.actual.png` and `<name>.diff.png` and print their paths.

  `JASPER_HTMLUI_UPDATE_GOLDEN=1` (the same switch as phase 0's C++ goldens) rewrites the goldens (full 1280×720 crop). Whoever regenerates them
  must look at every changed image (BRIEF). Determinism comes from `fixed_fps` + `auto_pause`
  and the unscaled UI clock (3.2.2). The test runs each example twice and requires the two
  captures to match (fraction 0) before comparing with the golden.
- **Importer mirror.** Test files set `os.environ["JASPERENGINE_IMPORT_MIRROR"]` at module level,
  before the `Editor` starts (`harness.launch_spec` copies `os.environ`). They build the mirror
  in a temp dir:
  - a Google Fonts CSS2 response naming `https://fonts.gstatic.com/s/karla/v1/karla.ttf`;
  - that TTF (a copy of `libs/imgui/misc/fonts/Karla-Regular.ttf`);
  - the Lucide `play.svg`;
  - a LottieFiles JSON.

  Each mirror entry sits at the URL the importer fetches. Phase 2 must document those URL
  templates in a comment in `HtmlUiImporters.h`; the test copies them as constants.

---

## 4. Work packages

Five packages run in two waves. Each works in
`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2-wt/<slug>` on branch `hui/<slug>`
(BRIEF). Each package's tests that need another package's tools use
`ed.require_tools(...)` / `has_tool` and skip until that package is merged. The phase acceptance
run (section 6) is on the merged `html-ui` with nothing skipped except the LuaLS/clangd
not-installed skips.

**Wave 1** (parallel, starting from `html-ui` with phases 0-3): `p4-inspector`, `p4-checks`,
`p4-examples`, `p4-guide`.
**Wave 2** (after wave 1 is merged): `p4-mcp-tools`.
**Corrective** (after `p4-guide`, next to `p4-examples`, before `p4-mcp-tools`): `p4-input`
(4.6), for what `p4-guide` found missing in the code.

### 4.1 `p4-inspector`: engine inspection and assertion API

- **Goal:** 3.3, complete and unit-tested headless.
- **Owned:**
  - `engine/core/htmlui/HtmlUIInspector.{h,cpp}`
  - `engine/core/htmlui/HtmlUIAssertions.{h,cpp}`
  - `engine/tests/htmlui/InspectorTest.cpp`
  - `engine/tests/htmlui/AssertionsTest.cpp`
- **Additive touches:**
  - `engine/CMakeLists.txt`: two source lines in the htmlui block, or after
    `core/subsystem/UISystem.cpp` at `:874`;
  - `engine/tests/htmlui/CMakeLists.txt`: two test sources;
  - `UIDocumentSystem.{h,cpp}`: only if a consumed accessor of 3.2.2 is missing, and each one
    is reported.
- **Deps:** phases 0-3.
- **Deliverables:**
  - the two headers exactly as in 3.3, plus their implementations;
  - zero new warnings;
  - built in the Linux editor build and the Web build; this file has no GL code.
- **Tests:** 5.1.

#### 4.1.1 `p4-inspector` as implemented

Where the implementation differs from or makes precise §3.3 (and what it found in §3.2.2):

- **Files.** `engine/core/htmlui/HtmlUIInspector.{h,cpp}`, `HtmlUIAssertions.{h,cpp}`, plus
  `HtmlUIInspectorInternal.h` (engine-internal: the document of an entity, selectors, refs,
  boxes, clip regions and hit tests over RmlUi elements, shared by the two `.cpp`). Tests
  `engine/tests/htmlui/InspectorTest.cpp`, `AssertionsTest.cpp` and their fixture
  `InspectorTestCommon.h` (each test writes its files into its own directory, made the assets
  directory, in a 1280×720 canvas). The test CMake globs `*.cpp`, so it needed no line; the
  engine CMake lists the two sources after the htmlui block; the Xcode workspace lists the five
  files in the iOS and macOS engine targets (ids `48544D4C` + sha1 of `p4-inspector:<kind>:<name>`).
  `UIDocumentSystem` was not changed: every accessor of §3.2.2 exists (`pushDataModel` is
  non-const and takes `(lua_State*, Entity, name)`; texture-mode documents are found with
  `getTextureContext`; `getCompiledDocument` follows viewport/root restyles through `compiledOf`).
- **Headers.** As §3.3, with include guards (the engine's style) instead of `#pragma once`.
  `InspectStyle` gains `file`, `line`, `column`: the location `source` names (empty/0 for
  `user-agent`, `default`, `inherited from …`), so a tool rewrites the path without parsing
  `source`. `HtmlUIInspector::withViewport` gains an overload with the entity: the context its
  document is in (a texture-mode document's own context, at its texture size), else the
  scene's; the scene-only form lays out the screen context.
- **Paths are asset-relative** everywhere (`DocumentInfo::html`, `dependencies`, the
  diagnostics, `InspectStyle::source`/`file`: `ui/menu.css:42:3`), as the engine's paths are
  (CONTRACTS "Paths"); the header comments of §3.3 said project-relative. The MCP tools prefix
  the assets directory at their boundary.
- **Selectors.** An HTML document's selectors are CSS, rewritten as Lua's `querySelector` does
  (`CssRewriter::rewriteSelector`: `body`, `html`, `:focus`, …); an `.rml` document's are RCSS
  as written. The `<html>` root can match (`query("html")`). A rewrite error, an empty result
  or an RmlUi rejection (its warning captured, not logged) is the error
  `selector "div[[" is not valid: <reason>`. A selector naming a pseudo-element (`::before`,
  `:after`, `::marker`, …) is refused the same way: pseudo-elements are not elements in RmlUi
  (non-DOM children; their hits go to their element). `""` is `<body>` for `query` only;
  `HtmlUIAssertions` errors with `selector is empty`. SVG internals are never matched or
  listed (`<svg>` is a leaf).
- **Refs.** `html` for an HTML document's root, `body` for `<body>` (an `.rml` document's
  root is `body`). `#id` only for an id that is unique in the document and needs no CSS escape
  (`[A-Za-z_-][A-Za-z0-9_-]*`); other ids are stepped through. Steps are
  `tag:nth-child(n)` counting element siblings as RmlUi does; the tag is left out (bare
  `:nth-child(n)`) for an element whose RML tag the selector rewrite does not name by its DOM
  name. Element `tag`s are DOM names (`LuaHooks::localName`); `htmlui-*` classes, the
  `data-htmlui-*` attributes and the root's internal id are hidden as Lua hides them.
- **Boxes** are bounding rects through CSS transforms, as `UIElement::getBoundingClientRect`
  (the nearest transform state of the element or an ancestor); an inline element's border box
  unites its line fragments. Every call first runs `ElementDocument::UpdateDocument`;
  transforms are the ones of the last drawn frame (RmlUi computes them when rendering).
- **Flags.** `hidden` is `display: none` on the element or an ancestor, or the computed
  (inherited) `visibility` — a `visibility: visible` child of a hidden parent is visible, as in
  CSS. Elements under `display: none` get only `hidden`/`transparent` (they have no layout).
  `clipped` uses the clip regions below; `overflowX/Y` RmlUi's scroll vs client size (layout
  records the scrollable overflow of every block box, `overflow: visible` included).
- **Clip regions** follow `ElementUtilities::GetClippingRegion` in floats: the containing-block
  chain (`GetOffsetParent`), `clip: none|always|<n>`, the scrollport without scrollbars,
  `overflow: clip` per axis with `overflow-clip-margin`.
- **Hit tests** search the entity's document only (`Context::GetElementAtPoint` from it), climb
  from non-DOM parts (scrollbars, pseudo-elements) to their element, and return nothing where
  the engine lets the pointer through to the game: a document or HTML `<body>` without a
  background (`HtmlUIContext::passesPointerThrough`). The stack ends at `<body>` (or is
  `[html]` when the root itself is hit; with the routing rule above, such a page is never the
  hit). `InspectorTest.HitTestFollowsTheEngineRouting` checks
  "empty" against `UIDocumentSystem::eventOnPointerMove` returning "not consumed".
- **Values** (`detail`, `styles`): lengths in px with at most 2 decimals (`em`, `rem`, `vw`, …
  resolved for the element, `font-size` its computed size), percentages as written, numbers
  with at most 3 decimals, colours `rgb()`/`rgba()` (alpha 2 decimals), `currentcolor` resolved,
  keywords and other values as RmlUi prints them. A shorthand name gives its longhands' values
  (one value when they agree: `padding` → `4px 6px 4px 6px`). Custom properties (`--accent`)
  give their specified value as the compat rewrite wrote it (colours as `#rrggbbaa`). A CSS
  property the compat tables know but RmlUi has no value for gives value `""` and source
  `not supported`. An unknown name is the error `unknown CSS property "colr" (did you mean
  color?)` (closest CSS name by edit distance, the text §3.6.1 shows). `properties` empty:
  the seven always-shown properties, then every longhand whose value is not its registered
  initial value, by CSS name, each name once.
- **Sources.** RmlUi's `Property::source` gives the rule's RCSS line; the inspector finds the
  rule in the compiled sheet that line maps to (the sheets as `DocumentLoader::registerDocument`
  maps them: author files, inline `<style>`s, presentational hints, the `!important` sheet),
  scans the rule's block for the last declaration of the property or of a shorthand that sets
  it, and maps that line to `file:line:col` of the authored declaration (`ui/vp.css:5:10` for a
  one-line rule inside `@media`). `selector` is the authored selector list of the rule, or the
  piece of it that matches the element (`.btn.primary` of `.btn.primary, .other`); hints have
  none. `user-agent` is the UA sheet's origin. A local value in the element's inline
  properties, or set without a sheet (a script's `style`, an animation), is
  `<html>:<line> (style attribute)` (the element's line, `locateElement`). Not local: an
  inherited property names the nearest ancestor that sets it (`inherited from #panel`), else
  `default`. `.rml` documents give RmlUi's `path:line` and rule name.
- **`withViewport`** sets whole-pixel dimensions (`HtmlUIContext::setDimensions`, which also
  restyles viewport-compiled documents) and `HtmlUIContext::update()`, from 1 to 16384 px per
  side. The restore finds the context again and skips it when `fn` removed it (a texture-mode
  document reloaded as a screen one).
- **Assertions** (messages are one sentence with px at most 1 decimal, ratios 2):
  - `Exists`/`Count`: `matched 3, expected exactly 2` / `at least` / `at most`, ref = the selector.
  - `Visible`: `display: none [on <ref>]`, `visibility: hidden`, `effective opacity 0.02 (below
    0.05)`, `border box is 0px x 40px`, `border box [x,y wxh] is outside the WxH viewport`,
    `fully clipped by the overflow of <ref>`; `unobstructed` takes the context's topmost element
    at the centre of the visible part: `covered by <ref> at (x, y)` (otherRef/otherRect the
    cover), `covered by #sheet of the UI document ui/top.html at (x, y)` (an element of another
    document; a transparent page is not one: the pointer passes through it, see below),
    `pointer-events: none: the pointer goes through it at (x, y)`.
  - `TextFits`: text line boxes come from the layout (each line's baseline, the strut's
    whole-pixel ascent/descent and half-leading as `InlineBoxBase::GetStrut`, the line height)
    and the font engine's width of each line's text (RmlUi fills its own widths only when it
    draws), hanging white space left out; compared in layout space (no transforms) with the
    content box of the element, or of its block container for inline text. Hidden elements are
    skipped. `text is 212px wide in a 118px content box (94px over)`, `… tall …`,
    `text is cut by the overflow of <ref> (Npx hidden)`, `text wraps to N lines (single line
    expected)`, joined with `; ` per element. `allow_scroll` also ignores the clip of those
    scroll containers. Ellipsised text (`text-overflow`) does not fit.
  - `NoOverlap`: rect/otherRect are the two border boxes, the intersection is in the message
    (`overlaps #b by 20px x 10px at [80,90 20x10]`); a `with` set matching nothing fails
    (`with selector matched nothing`); `matched` counts both sets' distinct elements.
  - `WithinParent`/`WithinViewport`: `extends past the padding box of <ref>: 30px right, 20px
    bottom` / `extends past the 1280x720 viewport: 40px right`; a `container` no ancestor
    matches fails (`no ancestor matches ".x"`).
  - `Contrast` checks the visible elements with own text among the matches and their
    descendants (the matches themselves when none has text): the element and its ancestors are
    composited as rendering groups (background colour, image, content, then the group's
    opacity), the text over its own background first. Image layers (decorators and the fork's
    `background-image` layers) and the transparent root are resolved by computing with two
    extremes: a difference makes the element inconclusive (or, for the root, uses `background`).
    `contrast 1.23:1 (#777777 on #888888) is below 4.5:1` (` (large text)` when 3:1 applied).
  - `MinSize` skips hidden and transparent matches; a zero-size one fails:
    `is 40x40px, smaller than 44x44px`.
- **Input routing through stacked documents (fixed here, at the coordinator's request).** An
  upper screen document used to keep the pointer from the documents below even where it was
  transparent: its `<html>` is `min-height: 100%`, so it was the hit, `isContent` called it
  not content, and the engine passed the pointer to the game. Now a page that paints no
  background, and whose document has no pointer listener (on `<html>` or `<body>`: a listening
  page takes the pointer on its empty parts, as in a browser), is not hit at all and the
  pointer goes on to the next document below in z-order, then to the game (fork patch P4-1 `Rml::Context::SetPointerPassThrough`,
  `HtmlUIContext::passesPointerThrough`, phase-1 spec §3.6.2 "as implemented",
  `TestInputPassThrough.cpp`). `hitTest` stays per entity; `unobstructed` fails only on an
  element of another document over the element.
- **Tests:** `InspectorTest` 21 cases (§5.1 1-10, plus empty and whitespace-only documents,
  pseudo-classes, pseudo-elements, texture-mode documents, reloads between queries and inside
  `withViewport`, stylesheet restyles, unicode text and ids, `.rml` documents, data models, a
  3 302-element tree), `AssertionsTest` 10 cases (every kind: pass, fail with its numbers, edge
  cases of §5.1, other viewports, a document over another), `InputPassThroughTest` 10 cases
  (clicks and hover through an upper page, listening pages taking the pointer (body, document,
  wheel, `data-event-*`, "click outside" closing a dropdown, removal restoring the pass-through), the upper element winning, capture and drag over
  another document, both pages empty, three layers, painted pages, `pointer-events: none`),
  `PointerPassThrough.documents` in the vendored RmlUi suite.

### 4.2 `p4-checks`: `check --ui`, hook, `ui-support`, hot reload pick-up

- **Goal:** 3.4 and 3.5.
- **Owned (the phase-4 changes in):**
  - `editor/util/ProjectChecks.{h,cpp}`
  - `editor/cli/CommandLine.{h,cpp}`
  - `editor/mcp/McpAutoSync.{h,cpp}`
  - `w4::syncHtmlUi` in `editor/mcp/tools/ScriptTools.cpp` and its declaration
    in `W4Shared.h`
  - `tools/mcp/gen_html_ui_support.py` (new)
  - `docs/mcp/guide/html-ui-support.md` (generated)
  - `docs/mcp/checks.md`
  - the pick-up paragraph of `docs/mcp/DESIGN.md`
  - `tools/mcp/tests/test_html_ui_checks.py`
  - `tools/mcp/tests/test_html_ui_reload.py`
- **Additive touches:** `editor/main.cpp` (the `ui-support` dispatch line).
- **Deps:** phases 1-2.
- **Deliverables:**
  - the CLI flag, defaults and hook memory;
  - the `ui-support` JSON;
  - the generator and the generated matrix;
  - auto-sync tracking, play-time reload and reports;
  - docs.
- **Tests:** 5.2, 5.3. The tests that need `inspect_ui` (`load_count`, styles) read the same
  facts through `eval_lua` in play (`UIDocument(...)`), or skip until wave 2. Before the
  phase-end run, wave 2 replaces those skips with the real tool calls (owned by `p4-mcp-tools`;
  see 4.5).

#### 4.2.1 `p4-checks` as implemented

Where the code differs from 3.4, 3.5 and 5.2-5.3 (also in `CONTRACTS.md`):

- **`ProjectChecks`** (`editor/util/ProjectChecks.{h,cpp}`): as 3.4.1, plus
  - `UiCheck::files`: the project-relative files the check covered (checked, or read by a
    document: linked sheets, imports). The hook memory uses it to drop fixed warnings.
  - `assetsRootFromProjectFile(root)` (project.yaml `assetsDir`, default the project root),
    `uiFinding(root, assetsRoot, Diagnostic)` (a compat diagnostic as a `Finding`, used by
    `w4::syncHtmlUi` too) and `uiSupportJson()` (the `ui-support` JSON, so later tools can reuse it).
  - `checkUiFiles` checks only files inside the assets directory (compat paths are
    asset-relative; an `.html` elsewhere cannot be a UI document), skips the web shell with `--files`
    too (the hook would otherwise flag it on every edit), and converts at the project's canvas size
    (project.yaml `canvasWidth`/`canvasHeight`), as documents load.
- **CLI**: as 3.4.2. Hook exit: 2 when there are errors or newly shown UI warnings; non-UI
  warnings keep their old behaviour (printed, exit 0 without errors). The memory is keyed by the
  finding's path (a warning located in a linked css is remembered at the css); after a run every
  covered file takes its current set; entries of files that no longer exist are dropped.
- **`ui-support`**: as 3.4.2, plus: `value` rows carry `property`, `attribute` rows `element`; a
  row with a code also has `severity` (the severity it is reported with); `diagnostics` entries are
  `{code, name, severity (the default), by_case, summary, help}`. The tables have no code column,
  so the code is chosen per kind and status from the codes the rewriters report (unsupported
  property `HTMLUI-CSS-0002`, value `-0004`, selector `HTMLUI-SEL-0001`, at-rule `HTMLUI-AT-0001`,
  element `HTMLUI-HTML-0002`, `on*` `HTMLUI-HTML-0005`, ...); an approximated row is
  `approximated-visible` (Warning) or `approximated-minor` (Info) by the first `(Warning…)` /
  `(Info…)` marker of its note, the convention of the tables' notes. Native, rewritten and emulated
  rows have no code. After phase 3 no row is `fallback` (1870 rows; 86 codes).
- **Matrix** (`gen_html_ui_support.py`): the Markdown names no editor version (it is
  `git describe`, so the committed file would be stale after every commit); the JSON has it.
  Extra sub-tables: input types (Elements), values / functions / units, media features (At-rules);
  the Notes column ends with the row's code.
- **`w4::syncHtmlUi`** returns `{reloaded, current, problems}` as 3.5.2. `syncFiles` and
  `external_changes` name them `ui_reloaded` and `ui_current` (5.3's "`current` is 1" is
  `ui_current`). `w4::isHtmlUiFile(path)` (W4Shared.h) is the file filter. A live document whose
  `.html` was deleted gives an error problem (`HTMLUI-GEN-0001`, "the document file is missing").
  Each reload logs the poller's line, `[HTML UI] Restyled|Reloaded <file> in scene '<name>'`.
- **Play** (3.5.3): changed `.html/.htm/.css`, fonts and `.json/.lottie` go through `w4::syncFiles`
  (which calls `syncHtmlUi`), so open code-editor buffers and `sync_files`' snapshot stay current
  too; images go through `syncHtmlUi` and stay deferred. The `deferred` text is set only when non-UI
  files wait.
- **Text**: `Reloaded UI: <html> in "<GameObject>" (styles only), ...; N document(s) were already
  current.` (four documents listed, then `and N more`).
- **`McpAutoSync`**: the `images` map is `assets` (images, fonts, `.json/.lottie`); new statics
  `isUiCodeFile`, `isAssetFile`, `compactProblems`, `addUiReport`.
- **Phase-2 fix**: `DocumentLoader::restyle(document, shared_ptr, options)` kept the first load's
  diagnostics after a stylesheet-only reload, so `getDiagnostics` (and the pick-up's problems)
  showed stale CSS findings. The binding now takes the new compile's diagnostics, its font check,
  and the document scripts' Lua errors (a restyle does not run them again);
  `CompatHotReloadTest.RestyleReplacesTheDiagnostics` covers it.
- **Deviations from 3.2.1**: `HTMLUI-IMP-0001`'s suggestion names the editor's automatic import and
  `jasperengine-editor import-ui`, not `import_files` (it has no `from_document` until
  p4-mcp-tools); the tests check that it says how to import. The phase-1 poller's interval is
  0.25 s, so 5.3's 2.5 s waits are ample.
- **Tests, wave 1**: `load_count` is read from those Console reload lines (one per reload,
  whoever reloads); computed styles and live diagnostics through `eval_lua` (`allow_edit_mode` in
  edit mode, `UIDocument(scene, entity(...))`). `inspect_ui` is called where it exists
  (`load_count()` in `test_html_ui_reload.py`, the end of `test_parity_cli_mcp`); those and the
  examples part of `test_cli_ui_speed` (it reads `docs/mcp/guide/html-ui-examples`) are the
  `WAVE-2` marks. Known limitation, as for every file type: an external edit stamped within a few
  ms of the end of a call that may write files is taken as that call's own (the settle scan).

### 4.3 `p4-examples`: example library, embedding, goldens

- **Goal:** 3.7 and 3.9 (`pngdiff`, goldens).
- **Owned:**
  - `docs/mcp/guide/html-ui-examples/**`
  - `editor/util/HtmlUiExamples.{h,cpp}`
  - `engine/tests/htmlui/golden/examples/*.png`
  - `engine/tests/htmlui/ExamplesCompatTest.cpp`
  - `tools/mcp/pngdiff.py`
  - `tools/mcp/tests/test_html_ui_examples.py`
- **Additive touches:**
  - `CMakeLists.txt`: the embedding block and one source line;
  - `Generator.cpp` `writeEditorTooling`: one call;
  - `engine/tests/htmlui/CMakeLists.txt`: one source;
  - `NOTICE.md`: one Lucide line.
- **Deps:** phases 0-3.
- **Deliverables:**
  - 9 examples + `blank` + manifest + license;
  - `HtmlUiExamples` with `instantiate` per 3.7.4;
  - the project copy;
  - goldens, inspected by eye.
- **Tests:** 5.4. The `assert_ui` parts call the tool when it exists (wave 2); in wave 1 they
  skip. The golden and behaviour parts run in wave 1: they build the scene with
  `create_entity` + `add_component uidocument` + `set_properties` + `attach_script`, with files
  copied in by the test.

#### 4.3.1 `p4-examples` as implemented

Where the code differs from 3.7, 3.9 and 5.4 (also in `CONTRACTS.md`):

- **Icons**: the examples use the engine's built-in Lucide icons, `<i data-lucide="play"></i>`
  (phase 2 made them native: the same inline SVG `lucide.createIcons()` makes, `currentColor`),
  not hand-copied inline SVG markup. The item pictures of `inventory` and `shop`
  (`assets/items/*.svg`) are Lucide icons saved as files with a fixed stroke colour (an `<img>`
  is drawn from them in Chromium too); `LICENSE-lucide.txt` covers both, `NOTICE.md` has a line
  for the copies. The Lottie spinner and the other SVGs (logo, compass, portrait) are ours.
- **Markup** (the public surface of 3.7.2 kept, with these differences): `dialogue` is a
  `section.dialogue` and its choices are `button.choice` children of `div#choices` (a `<ul>`
  holds only `<li>`; `.choice:nth-child(2)` is the second choice), created by Lua with
  `createElement`/`createTextNode`/`appendChild`; `shop` cards are `article.card`; `main_menu`'s
  `h1.logo` sits in `header#brand` with an SVG emblem, and every menu button logs
  `main_menu: <id>` (`#settings` also opens the panel: 5.4's ArrowDown + Enter checks it).
- **Data binding**: buy buttons used `data-attrif-disabled="gold < item.price"` (RmlUi's boolean
  attribute view; `data-attr-disabled` wrote `disabled="false"`, which disabled; since `p4-gaps`
  they use `data-attr-disabled`, 4.3.2 G9);
  drag and drop is the engine's CSS property `drag: drag-drop` on `.slot` (browsers ignore it;
  RmlUi has no `drag` attribute) with `data-event-dragstart="pick(i)"` /
  `data-event-dragdrop="drop(i)"`. A `data-for` element stays in the DOM, hidden, as the
  **last** child of its parent: `.slot:nth-child(1)` is the first slot. *(p4-input, 4.6.1: the
  Lua DOM, the inspector and `assert_ui` leave the template out, so `.slot` counts 24, as the
  manifest's `count .slot = 24` says.)*
- **Settings switches**: an invisible `<input type="checkbox">` over a `span.knob` whose
  `::before` is the knob (`input:checked + .knob`): the engine generated no `::before`/`::after`
  on `<input>` (Chromium does). Since `p4-gaps` the switch is the checkbox itself (4.3.2 G5).
- **HUD**: `HudUI:damage(n)` takes the shield first, so 5.4's step is `damage(80)` (50 shield,
  then 30 health: `#health-fill` at 70%). The score is shown with thousands separators.
- **Loading**: 5.4's "wait 1.5 s" is the 90 frames every example plays first (3 s of
  `duration`: 50%). `#press-key` is `data-if`: it stays in the DOM, so the test reads
  `offsetHeight > 0`.
- **Toasts**: at most 5 showing; a sixth sends the oldest away with its exit animation (removed
  when it ends). The countdown bar is a CSS animation whose duration Lua sets.
- **Lua**: as 3.7.1, plus: listener functions are made once in `init` and added again in
  `onDocumentLoad` (adding a function twice is ignored); properties are declared with
  `---@field` under the class (the editor's script stubs skip what a script declares), so the
  scripts check clean with or without the project's generated stubs.
- **examples.json**: `assets` lists every asset file; `checks` use `assert_ui`'s field names
  (`single_line`, `unobstructed`, `allow_scroll`, `container`, `background`); viewports per
  example (portrait 720x1280 where the layout is meant for it: menus, dialogue, toasts, loading).
- **`HtmlUiExamples`**: as 3.7.3 plus `find(path)`, `pascalName(stem)` (words of ASCII letters and
  digits capitalized and joined; other characters dropped, since a Lua name is ASCII; `Document`
  when nothing is left, `Ui` in front of a digit) and `titleName(stem)`. `names()` lists the
  examples, then the templates (`blank`). `instantiate` normalizes `\` and a leading `./`,
  rejects paths that leave the project, also renames `'assets/` in Lua, and gives `blank` the
  title in `<title>` and `<h1>`. `writeTo` writes only changed files (`FileUtils::writeFile`) and
  removes files and folders no longer embedded. The CMake block numbers the arrays
  (`html_ui_example_<n>`), the files sorted by path.
- **Project copy and lua-language-server**: `.luarc.json` gains `workspace.ignoreDir`
  `[".vscode", ".jasperengine/engine-api/html-ui-examples"]` (merged into a user's file like
  `workspace.library`): the copy is reference material, and its Lua classes would otherwise be
  defined twice once a template is instantiated (`duplicate-set-field`).
- **Goldens** (3.9): the play test sets `configure_game_view {aspect: "project", scale: 1,
  maximize_on_play: true}`: the headless editor's Game view (1600x900 window) shows the canvas at
  1415x796 while playing (904x509 without maximizing). The crop is box-filtered down to
  1280x720 for the committed golden and both are compared at 640x360 (tolerance 24, at most 1%).
  The test scenes clear to `(0.09, 0.1, 0.13)`. The two runs had to agree within the tolerance on
  all but 0.01% of the pixels: the antialiased edges of rounded boxes under blurred shadows varied
  by a few pixels between play sessions (5 to 42 of 1.1 M pixels measured; `p4-gaps` found the
  cause, 4.3.2 G7, and the runs agree exactly).
  Play keeps the document the editor loaded, so CSS animations that run from the load have a
  phase that depends on the wall time spent in edit mode: the test reloads `main_menu` (infinite
  keyframes, no data model) once play is held (`RESTART_CSS`); a document that makes a data model
  is reloaded by `createDataModel` at play time anyway.
- **Tests, wave 1**: the scenes are built with `create_scene` (2D), `create_entity`,
  `add_component`, `set_properties` and `attach_script` from the project copy's files
  (`assets/ui/<n>/`, Lua in `scripts/`); elements are found with `eval_lua`
  (`getBoundingClientRect`) and driven with canvas coordinates. `WAVE-2` marks: the resource
  check in `test_<n>_files_in_project`, `test_<n>_asserts` (`assert_ui`), `ensure_scene`
  (`create_ui_document {template}`), `center` (`send_input {target}`), the `inspect_ui` hover
  check of `main_menu`. `instantiate`'s renames are tested in C++
  (`editor/htmlui/tests/HtmlUiExamplesTest.cpp`, `htmlui-editor-tests`: every template
  instantiated under another stem compiles clean), as is `writeTo`.
- **Engine fixes the examples found** (in other packages' files, reported):
  - fork P4-E1 (`FORK.md`): a background with `var()` in both its image and its size crashed
    (the image list read from the style's one returned-property storage was freed by the next
    `var()` longhand); transitions with `var()` in two longhands did not run;
  - `UIDocumentSystemReload.cpp`: the reload a late `createDataModel` makes restored the old
    elements' form values into bound controls, which wrote them into the model (a settings
    screen lost every model value);
  - `RmlRenderer::SaveLayerIntoTexture`: a box shadow larger than the context (rendered in tiles,
    P3-I14) drew its background and border scaled and shifted on a scaled canvas (the editor's
    Game view, high-DPI screens).
- **Parity gaps found and worked around in the examples** (fixed by `p4-gaps`, 4.3.2):
  `border-radius` on an `<img>` does not clip the picture (a wrapper with `overflow: hidden`
  does); a `<button>` taller than its content does not center it vertically as Chromium does
  (the examples use flex); a `<select>` with a fixed `height` keeps its text at the top (padding
  instead); no `::before`/`::after` on `<input>`; `offsetWidth`/`offsetHeight` of an element
  hidden with `display: none` (`data-if`) keep their last value instead of 0; `check --ui --files` on a stylesheet alone warns
  about a shorthand whose `var()` only an inline style defines (a default in the sheet avoids it).

#### 4.3.2 `p4-gaps` as implemented (corrective package)

The parity gaps 4.3.1 lists, and the examples' non-deterministic goldens, fixed at their root
(also in `CONTRACTS.md` and `FORK.md` P4-G1 … P4-G9). Each has a browser-parity fixture in the
matching `fixtures/p3/<area>/` with a Chromium reference, passing at the default tolerances, and a
unit test that fails without the fix.

- **G1, `border-radius` on replaced elements** (fork P4-G1): replaced content is trimmed to the
  *content* edge curve (CSS Backgrounds 3 §5.3, Chromium; HtmlImageElement clipped to the padding
  box curve, the fork's img, the SVG plugin's svg (an `<img>` with an SVG source is one), `<lottie>`
  and `input type=image` not at all). The fork's `ReplacedContentGeometry` draws the picture as the
  content box filled like a background (the feathered curves of a background, the quad's texture
  coordinates) when it covers the content box or lies inside the curve, and under a clip mask
  (`ScopedReplacedContentClip`) when it crosses the curve (`object-fit: contain` in a pill): a
  stencil mask alone aliased the curves. Fixture `paint/replaced-radius` (pastel pictures on grey:
  the strict metric judges the trimmed shape, not curve rasterization, see its sidecar).
- **G2, buttons** (fork P4-G2): `Element::HasButtonLayout()` (the engine's `<button>`, button and
  submit inputs): as a block container the content is centered vertically as one block, safely,
  whatever `align-content` says, in the content box (padding, `min-height`, `display: block` and
  `flow-root` too); flex and grid buttons are flex and grid containers, as in Chromium. The
  baseline moves with the centered line; a button without a line has its baseline at its content
  box's bottom. Fixture `layout/button-centering`. (A `display: inline` button stayed an inline box:
  Chromium makes it an inline-block; fixed by p4-gaps2, 4.3.3.)
- **G3, selects and text fields** (fork P4-G3): a select's value is centered in its content box and
  its arrow in its padding box, a single-line text field's line in its content box (not safely, as
  Chromium). The UA chevron (`compat/ua/arrow-down.svg`) is stroked at 2.1 instead of 1.6: at the
  same place its strokes rasterized lighter than Chromium's. Fixture `layout/select-height`.
  Not changed: a select with `appearance: none` kept no border in the engine (Chromium keeps
  html.css's 1px border, and drops the menu list's 4px inner start padding); the companions cleared
  it with the checkbox's (fixed by p4-gaps2, 4.3.3).
- **G4, geometry of elements without a box**: RmlUi keeps the box an element had when it is
  hidden. The element API (`UIElement`: `getBoundingClientRect`, `getBoundingBox`,
  `offsetLeft/Top/Width/Height`, `clientWidth/Height`, `scrollTop/Left/Width/Height`, so Lua) and
  the inspector's boxes (`inspect_ui`, `assert_ui`) report 0 for an element with `display: none`
  on it or an ancestor (a false `data-if`), `display: contents`, or outside a document
  (`inspect::hasLayoutBox`), as browsers do. The fork's own getters are unchanged (RmlUi reads them
  internally). Fixture `layout/display-none-geometry`; `GeometryApiParity.MatchesChromium` compares
  the API and the inspector with its Chromium boxes.
- **G5, `::before`/`::after` on inputs** (fork P4-G5): `Element::HasGeneratedContentBoxes()` decides
  whether they exist (default: not replaced); inputs answer as Chromium 153: checkbox, radio and
  range have them (laid out in the input's content box), text-like and button-like types do not (a
  button input drew them); progress bars (and so the engine's `<meter>`) have them too. Chromium also draws them on `color`, `file` and the date/time types,
  which the engine approximates by a text field or a button: they draw none. A checkbox with
  `appearance: none` and no size stayed 13x13 (the UA sheet's size; Chromium sizes it by its
  content; fixed by p4-gaps2, 4.3.3). Fixture `pseudo/input-before-after`. The settings example's switches are now
  `input[type=checkbox].switch` with `appearance: none`, the knob their `::before`.
- **G6, `check --ui --files` on a stylesheet**: a stylesheet named on its own is checked in the
  context of the project's documents that load it (their checks' findings in the sheet and its
  imports): a variable a style attribute or another sheet defines is no finding, one no document
  defines (a typo) is. `compat::checkStylesheet` (a sheet no document loads) does not report
  HTMLUI-CSS-0012 for a declaration that needs a variable the sheet and its imports do not
  declare (any of them); one they declare with two values is still reported. Never a false
  positive, typos caught when the documents are known. Tests `test_cli_ui_stylesheet_variables`,
  `CompatApi.CheckStylesheetLeavesUndeclaredVariablesToItsDocuments`. `main_menu.css` lost the
  `--value` default that avoided the warning.
- **G7, non-deterministic rendering** (fork P4-G7): the box-shadow cache's key had no pixel size,
  so after the editor's Game view changed scale (play, maximized) an element kept the shadow
  texture (with its background and border) made at the old size while another element held it,
  or got a new one: the same frame had other curve edges from session to session. The key holds
  the feathering and the element's feather unit; background image layers' rounded clips follow the
  feather unit too. `test_html_ui_examples.py` compares its two runs at exactly 0 %
  (`RUNS_MAX_FRACTION = 0.0`, 4.3.1 had 0.01 %). A second cause, found through a flaky
  `MsaaScissor.HtmlUIClipEdge` (one failure in a full run, a few unexpected pixels): the readbacks
  (`glGetTexImage` in the test harness's `Readback.cpp` and in the editor's
  `GraphicUtils::readFramebufferPixels`, which MCP screenshots use) returned a texture with some
  16x16 blocks not rasterized yet, about one render in 70 of a new context: GL requires the read to
  wait for the commands that render into the texture, Mesa 23.2.1's llvmpipe with its rasterizer
  threads did not always (a second read of the same texture was whole, and the screen it was
  composited onto was too; 1000 renders of each sample count gave one image each once the read
  waited). Both readbacks call `glFinish()` first: the renderer itself was deterministic, and no
  test accepts more than before. `MsaaScissor.HtmlUIClipEdgeReadsBackTheSameImage` renders the page
  in 200 new contexts and requires one image (it failed 5 runs out of 5 without the `glFinish`).
  Vulkan's readback already waited (`vkQueueWaitIdle`).
- **G8, the guide's examples table** follows `examples.json` (`main_menu` uses the built-in Lucide
  icons, `data-lucide`; `hud`'s damage flash; `shop`'s quantity dialog and `data-attr-disabled`);
  `test_guide_examples_table` checks the rows, their order and their checks against the manifest.
- **G9, `data-attr-<boolean attribute>`** (fork P4-G9): on HTML's boolean attributes a false value
  removes the attribute and true sets it empty (`data-attrif-*` unchanged); other attributes and
  string or number values are written as before. The shop example's buy buttons use
  `data-attr-disabled`. Tests `data_binding.data-attr-boolean`,
  `ScriptDataModelTest.BooleanAttributesFollowTheModel`.
- **Examples changed**: settings (switches, selects with a height), dialogue (the portrait is a
  rounded `<img>`, no wrapper), shop (`data-attr-disabled`), main_menu (no `--value` default); their
  goldens regenerated and inspected. The guide's notes say what now works (form-control
  pseudo-elements, centering, rounded pictures, boolean `data-attr`, geometry of hidden elements).

#### 4.3.3 `p4-gaps2` as implemented (corrective package)

The parity gaps 4.3.2 left (also in `CONTRACTS.md` and `FORK.md` P4-G10, P4-G11). Each has a
browser-parity fixture with a Chromium reference, passing at the default tolerances, and a unit
test that fails without the fix. Chromium was measured first (computed styles, boxes and pixels of
the pinned headless shell with the references' fonts).

- **G1, controls with `appearance: none`** (UA sheet and the `appearance` rewrite): the
  companions cleared one shared look (`--html-control-bg`, `--html-control-border`) for selects and
  checkboxes alike, so a select lost the 1px border Chromium keeps. Each control now has companions
  of its own and gets exactly html.css's look without its theme: a select keeps `1px solid #767676`
  on ButtonFace (`#efefef`) with no arrow and none of the menu list's inner padding
  (`--html-select-bg`, `--html-select-padding`, `--html-select-arrow-width`; its border colour is no
  companion any more); `<button>` and button, submit and reset inputs get `padding: 1px 6px;
  border: 2px outset` ButtonBorder (black, Chromium 153), the same on hover and press
  (`--html-button-padding`, `-border-width`, `-border-style`, `-border`, `-hover-bg`,
  `-hover-border`, `-active-bg`, `-active-border`; the file input approximation keeps its button
  look, as Chromium's inner button does); a progress bar (and so the engine's `<meter>`) is
  html.css's `::-webkit-progress-bar`/`-value`: the whole box `#808080`, the value `#008000`, no
  rounding (`--html-progress-border-width`, `-radius`, `-bg`, `-track`, `-fill-radius`, `-fill`).
  Checked against Chromium with no other difference: textarea (its 1px frame), the text-like
  fields (2px inset, p3-paint) and range (Chromium keeps the thumb's own look; so does the
  engine). Found on the way and fixed: `input[type=search]` is `box-sizing: border-box` with a
  185px default width (html.css; its auto and none looks are both 8px of padding and border); the
  shadows `appearance: none` cleared were `none`, which is not a valid entry of the box-shadow list
  the outline companion joins (`Invalid substitution` errors for a checked radio and the progress
  track): an empty shadow `0 0 0 0 #0000`. `CompatUaSheet.AppearanceAutoWritesTheSheetsFallbacks`
  covers the new companions. Fixture `paint/appearance-none-controls` (Ahem with `line-height: 1`:
  Chromium measures a select's `line-height: normal` with the fallback font when the web font
  loads after the first layout, and never lays it out again); test
  `AppearanceNone.ControlsKeepTheirOwnHtmlCssLook`.
- **G2, a checkbox or radio with `appearance: none` and no size** (UA sheet, `HtmlInputElement`,
  fork P4-G11): Chromium's theme only sizes an auto width or height (13px, the border box, each
  on its own); without it the control is an inline-block sized by its content: 0x0 alone or with
  `content: ""`, its `::before`/`::after` text or block, plus padding and border (border-box or
  content-box), a positioned `::before` adding nothing. The UA sheet's size is the companion
  `--html-check-size` (`13px`, `auto` under `none`); `HtmlInputElement::GetIntrinsicDimensions`
  reads it: with the theme the intrinsic size is 13px less the borders and padding of a border-box
  control, without a ratio (RmlUi's was 16x16 with ratio 1); without it a checkbox or radio is not
  replaced and is laid out as a block container. Checkboxes and radios are `overflow: visible`
  (Chromium; the UA's `input { overflow: hidden }` clipped their generated boxes and made them
  scroll containers, whose baseline is the bottom margin edge). Their baseline is their last
  line's, and an empty `::before` gives none (P4-G11: zero-height line boxes do not exist for
  baselines, CSS 2.1 §9.4.2; the fork took them). Fixture `layout/checkbox-content-size`; tests
  `AppearanceNone.CheckboxIsSizedByItsContent`, `Layout.Baseline.ZeroHeightLines`. Not changed:
  a themed checkbox or radio in a box that is not 13x13 is drawn as a stretched box (Chromium
  draws its 13px square centered), and an author background shows on a themed checkbox (Chromium
  paints the theme's white).
- **G3, `display: inline` buttons** (fork P4-G10): an element with `HasButtonLayout()` (the
  engine's `<button>`, button, submit and reset inputs) computes `display: inline` and
  `inline-table` as `inline-block` (HTML's button layout; Chromium's computed value with its
  theme); with `appearance: none` Chromium keeps the computed `inline` but lays the button out the
  same, the engine reports `inline-block`. Other display values are unchanged (Chromium also
  blockifies `table` and `list-item` on themed buttons). Fixture `layout/button-display-inline`;
  tests `form.input.button.display_inline`, `ButtonDisplay.InlineIsInlineBlock`.
- **G4, the inputs the engine approximates** (`color`, `date`, `time`, `datetime-local`, `month`,
  `week`, `file`): Chromium draws their `::before`/`::after` around its own shadow content (a
  swatch, date fields, a button and a label), which the engine does not render; they stay without
  generated boxes (`HasGeneratedContentBoxes`, P4-G5). Documented and diagnosed: their
  input-type notes, so the `HTMLUI-HTML-0008` warning each one already gets at load and the
  support matrix, say "no ::before/::after"; the `meter` row says it is drawn as a progress bar,
  also with `appearance: none`; the guide lists them.
- **Examples and goldens**: no example worked around the gaps. The settings example's selects
  (`appearance: none` with their own border, padding and chevron) lose the menu list's 4px inner
  padding and 16px arrow box, as in Chromium; they are on the Graphics tab, outside its golden
  (Audio), so no golden image changed (`test_html_ui_examples.py`, every C++ golden and fixture
  reference unchanged). The guide's form-control paragraph says what `appearance: none` leaves on
  each control, that an unsized custom checkbox needs a size, and that inline buttons are
  inline-blocks.

### 4.4 `p4-guide`: guide, AGENTS.md, stubs, API search

- **Goal:** 3.8.1, 3.8.2 (README part), 3.8.4, 3.8.5.
- **Owned:**
  - `docs/mcp/guide/html-ui.md`
  - `docs/mcp/guide/README.md`
  - `docs/mcp/README.md`
  - the HTML UI parts of `generate_lua_stubs.py` and `generate_api_suggestions.py`
  - `Generator::writeAgentsFile` (`Generator.cpp:1397-1492`)
  - `tools/mcp/tests/test_html_ui_docs.py`
- **Additive touches:** none.
- **Deps:** phases 1-3.
- **Deliverables:** the guide (all 16 sections), the AGENTS.md section, the stubs and API
  search, README updates.
- **Tests:** 5.5. Tool-name checks against `tools/list` skip until wave 2.

#### 4.4.1 `p4-guide` as implemented

Where the result differs from or makes precise 3.8.1, 3.8.2 (README), 3.8.4, 3.8.5 and 5.5 (also in
`CONTRACTS.md`):

- **Guide** (`docs/mcp/guide/html-ui.md`, 86 KB, down from 97 KB): the 16 sections by the titles
  of 3.8.1, every phase-1/2/3 fact moved into the matching section (the component and texture
  mode into 1, `.rml` into 4, the rewrites/limits tables condensed into 5 "CSS that works" /
  "CSS that differs" with their codes, imports and Lucide into 9, editor workflow and hot
  reload into 12), plus a "What Chromium parity means, and where it stops" subsection, the
  pointer pass-through rule in bold in 10, and the edit-mode notes. DocsTools serves a `## `
  section above 6 KB as its `### ` subsections (the section's lead-in goes with the first one),
  so the big sections open with a subsection of their own ("UI Documents: the model",
  "Elements and default styles", "The element API") and subsection titles say what they are
  ("CSS that works", "UI events"); every served part is under 7.5 KB.
- **Snippets** are the guide's own, not copied from the example files (p4-examples was built in
  parallel and is not merged into this branch): a menu (`assets/ui/menu.html`, `menu.css`,
  `scripts/MenuUI.lua`: clicks, keyboard and gamepad focus, an anime stagger, the bind-on-load
  pattern), a HUD (`hud.html`, `hud.css`, `scripts/HudUI.lua`: data binding, a click-through
  panel), `scripts/Terminal.lua` (texture mode) and fragments. Convention: every
  ```` ```html ````/```` ```css ````/```` ```lua ```` block is good; a block preceded by a line
  `` `<project path>`: `` is a complete file; bad examples live in tables. The examples table
  of section 14 follows 3.7.2.
- **Differences from 3.8.1 found in the code** (all three fixed since by `p4-input`, 4.6.1; the
  guide now says what 4.6.1 describes): gamepads did not reach documents (3.8.1 §10 said they
  do): the guide said so and the menu script handled `onGamepadButtonDown` in Lua. In edit mode
  a `data-style-*` expression of a model no script created yet evaluated with empty values and
  RmlUi logged `Syntax error parsing inline property declaration 'width: %;'` once per load: the
  guide explained it. RmlUi keeps the `data-for` element in the DOM (hidden, after its rows):
  the guide said `querySelectorAll` counts it.
- **Stubs** (`generate_lua_stubs.py`): `HtmlUITypes.lua` is generated and holds
  `---@alias UIEventType` (the names of `HtmlUIEventListener.cpp`, RmlUi's event table and the
  engine's elements: `click` ... `transitioncancel`, `toggle`, `close`, `cancel`,
  `htmlnavigate`, the `<lottie>` events; `mouseenter`, `mouseleave`, `pointer*`, `touch*`,
  `contextmenu`, `beforeinput`, `composition*` left out) and `---@class UIDiagnostic`. A custom
  event name of `dispatchEvent` needs `---@cast name UIEventType` to listen to it.
  `getDiagnostics` returns `UIDiagnostic[]`, `getTargetTexture` `Framebuffer|nil`,
  `setColorScheme` / `insertAdjacentHTML` take their literal values. `HTML_UI_CLASS_DOCS` adds
  the guide's URI to `UIDocument`, `UIElement`, `UIEvent` and `anime`; `HTML_UI_FIELD_OVERRIDES`
  replaces stale field docs (`tagName` is upper case in `.html` documents, `innerHTML` is HTML,
  `body`, `documentElement`). The parameter classes 3.8.5 names (`AnimeParams`,
  `AnimeStaggerOptions`, `AnimeTimelineOptions`, `AnimeSpringOptions`) exist since phase 1 as
  `AnimeAnimationParams`, `AnimeStaggerParams`, `AnimeTimelineParams`, `AnimeSpringParams` (in
  `engine/core/script/binding/stubs/anime.lua`), `UIRect` comes from the binding, and the typed
  `createDataModel`/`getDataModel`/`classList`/`style`/`dataset`/anime overrides were phase 1's:
  kept as they are.
- **API search** (`generate_api_suggestions.py`): `anime` is bound with
  `beginClass<Anime>("anime")` + `addStaticFunction`, which the parser already reads (no
  namespace case was needed). `MANUAL_SYMBOLS` replaces the details built from C++ headers (wrong
  for Lua: `removeEventListener(uint64_t listenerId)`, `createDataModel(string, UIValue)`, the
  empty `anime.animate()`) with the Lua signatures, and adds `UIDataModel` (class and proxy
  methods) and `anime.utils` / `anime.eases`. `searchDocs` needed nothing.
- **AGENTS.md**: the section of 3.8.4 plus one sentence on pointer pass-through; the examples
  path is computed like the other engine-api paths. AGENTS.md is written when the project is
  first read through `jasper://project/AGENTS.md`, played or built (not by `create_project`
  itself), so `test_agents_md_section` reads the resource.
- **README** (`docs/mcp/README.md`): the intro bullet, the pick-up paragraph (HTML UI and asset
  types, live reload in edit mode and play, `ui_reloaded`, the UI `problems` keys) and the
  budget sentence of 3.6.7 (its numbers are p4-mcp-tools'); `docs/mcp/guide/README.md` rows for
  `html-ui.md` and `html-ui-support.md`, and the examples folder.
- **Tests** (`test_html_ui_docs.py`, 11 cases): 5.5's cases, with these differences:
  `test_guide_names_exist` is split: the Lua-name part runs now
  (`test_guide_lua_names_in_stubs`: every `doc:`/`el:`/`document:` call and `anime.*` /
  `anime.utils.*` / `anime.eases.*` name of the guide is in the generated stubs), the tool part
  skips until wave 2 (`WAVE-2`; it allows the output field names the guide explains and the
  project-setting values). `check_script` runs a chunk's top level, so a fragment is checked
  inside a function (complete files as they are), and the Lua of `<script type="text/lua">`
  blocks too. `test_guide_css_html_snippets_clean` runs `check --ui` (CLI) on every html/css
  block, giving fragments real files for what they name. Added: `test_guide_structure` (the
  16 titles in order, the rules the guide must state) and `test_guide_snippets_play` (the
  complete files and fragments loaded in a UI scene: clean in edit mode; in play the menu's
  icons, entrance animation, focus, ArrowDown/Enter, gamepad dpad/A, a click through the
  transparent HUD page, the HUD's `{{ }}`, `data-style-width`, `data-class-low`, `data-for`,
  `data-event-click`, the element-API, anime (timeline call, table animation) and pass-through
  fragments run through `eval_lua` as a script would, Lottie playing, `@font-face` loading,
  `font-effect`, texture mode; no Console error or warning). `test_luals_ui_stubs` checks the
  guide's complete scripts, a `GOOD_UI_LUA` and, when present, the example scripts in a
  workspace with the project's stubs (`test_stubs.lua_workspace`); `BAD_UI_LUA` also has
  `mouseenter`, `classList.add` with a dot and `el.onclick = f`.

### 4.5 `p4-mcp-tools`: tools, send_input/import_files extensions, resources, instructions, agent simulation

- **Goal:** 3.6, 3.8.3; turn the wave-1 skips into real calls; the agent simulation.
- **Owned:**
  - `editor/mcp/tools/HtmlUiTools.cpp` (new)
  - `editor/mcp/tools/HtmlUiShared.h` (new)
  - the HTML-UI chunk of `editor/mcp/McpInstructions.cpp`
  - `tools/mcp/tests/test_html_ui_tools.py`
  - `tools/mcp/tests/test_html_ui_agent_sim.py`
  - `docs/mcp/TOOLS.md` (regenerated)
- **Additive touches:**
  - `McpTools.h` and `McpServer.cpp`: register;
  - `CMakeLists.txt`: one source line;
  - `PlayTools.cpp` + `PlayViewCommon.h`: `target`/`to_target`, `w3::luaToJson`;
  - `AssetTools.cpp`: the `import_files` extension;
  - `DocsTools.cpp`: one `docsIndex` line;
  - `gen_tool_docs.py`: `MODULES`;
  - `test_autosync.py`: `pairs`;
  - the wave-1 test files `test_html_ui_checks.py`, `test_html_ui_reload.py`,
    `test_html_ui_examples.py`, `test_html_ui_docs.py`: replace each `require_tools` skip with
    the call it waited for, nothing else.
- **Deps:** `p4-inspector`, `p4-checks`, `p4-examples` merged; `p4-guide` merged before its
  final run.
- **Deliverables:**
  - the 3 tools and 2 extensions with schemas within budget;
  - the resources;
  - the instructions section;
  - TOOLS.md regenerated with `gen_tool_docs.py`.
- **Tests:** 5.6, 5.7, plus the whole HTML UI suite and `test_protocol.py`, `test_autosync.py`,
  `test_checks.py`, `test_stubs.py`, `test_ui_anchors.py`, `test_play.py`, `test_assets.py`.

### 4.6 `p4-input`: gamepads in documents, two data-binding fixes (corrective)

- **Goal:** what 4.4.1 found missing: 3.8.1 §10 (pointer, keyboard **and gamepad** reach the
  focused document first) made true, designed as a browser-like extension; no Console line for a
  well-formed `data-style-*` binding of a model that does not exist yet; the `data-for` template
  out of DOM queries.
- **Owned:** the gamepad parts of `Engine.cpp`/`Engine.h` and `UIDocumentSystemInput.cpp`,
  `engine/tests/htmlui/TestInputGamepad.cpp`, `TestDataBindingDom.cpp`,
  `tools/mcp/tests/test_html_ui_gamepad.py`; fork patches P4-2 … P4-5.
- **Deps:** `p4-guide` merged. `p4-mcp-tools` also edits `PlayTools.cpp` (`target`); `p4-input`
  does not need to (`send_input` already sends gamepad buttons and axes).

#### 4.6.1 `p4-input` as implemented

**Gamepad: the model.** The W3C Gamepad API is polled and gives a page no events and no
navigation; game UIs map the pad to keyboard-like navigation instead. The model chosen is the
one Edge on Xbox and Windows (UWP) uses for gamepad key events, extended with the keyboard
meaning of the navigation buttons:

- Every button, the four directions of the left stick and the two triggers are **keys**:
  `keydown`/`keyup` at the focused element (else the focused document), bubbling, cancelable.
  `event.code` names the pad key in Edge's names: `GamepadDPadUp/Down/Left/Right`,
  `GamepadLeftThumbstickUp/Down/Left/Right`, `GamepadA`, `GamepadB`, `GamepadX`, `GamepadY`,
  `GamepadLeftShoulder`, `GamepadRightShoulder`, `GamepadLeftTrigger`, `GamepadRightTrigger`,
  `GamepadView` (Back), `GamepadMenu` (Start), `GamepadLeftThumbstickButton`,
  `GamepadRightThumbstickButton`, and `GamepadGuide` (no Edge name). `event.key` is what the key
  means: `ArrowUp` … for the d-pad and the stick, `Enter` for A, `Escape` for B, the code for the
  others; `event.keyCode` the engine key it stands for (`D_KEY_UP` …, `D_KEY_ENTER`,
  `D_KEY_ESCAPE`, else 0); `event.gamepad` the pad id (-1 for the keyboard; `UIEvent::getGamepad`).
  So a listener written for the keyboard serves the pad, and one that needs the pad tells them
  apart by `code`. The engine's Lua gamepad API (`onGamepadButtonDown/Up`, `onGamepadAxisMove`,
  `Input.isGamepadButtonPressed/getGamepadAxis`) stays the game's.
- **Default actions** (skipped when a listener calls `preventDefault()`, or when the event was
  stopped, which is how controls report that they used it; a listener that moved the focus
  counts as having handled it):
  - directions: the focused element's `nav-<direction>` decides when it declares one (RmlUi's
    arrow-key rules: `none`, `#id`, `auto`, `horizontal`, `vertical`, `tree-order`); otherwise
    spatial navigation (`ElementDocument::FindNavigationTarget`, fork P4-2): the nearest focusable
    element (visible, `tab-index: auto`, not `focus: none` or disabled) whose box lies in that
    direction (it ends beyond the source's edge and starts at most halfway into the smaller box),
    by the CSS Spatial Navigation distance as Chromium weighs it, `euclidean + along + w ×
    across-gap − √across-overlap` with `w` 30 for left/right (a sideways step stays in its row)
    and 2 for up/down (a vertical step may drift across columns: a settings row's label on the
    left, its control on the right), ties in tree order; first among the elements of the nearest scroll container around the focus (items
    scrolled out of view included: a list scrolls to its next item before the focus leaves),
    then the whole document, where an element inside another scroll container counts only if it
    intersects that container's visible box. No wrap-around. The target gets `Focus(true)`
    (`:focus-visible`) and `ScrollIntoView` with `Nearest` alignment. A focused range adjusts
    (the controls see the arrow: `WidgetSlider`), an open select moves through its options
    (`WidgetDropDown`); a text field and a closed select never keep the d-pad (they see no key,
    `KI_UNKNOWN`).
  - A: the keyboard's activation: `KI_RETURN` for the controls (RmlUi's document clicks a
    focused `tab-index: auto` element, compat's `activateFromKey` clicks buttons, links and
    summaries, a select opens or chooses), `KI_SPACE` on a checkbox or radio (it toggles on the
    release, as in HTML), nothing on a text field (no line break, no implicit submit; the virtual
    keyboard is requested, as a tap does). `:active` on the focused element from the down to the
    up (not when prevented).
  - B: `KI_ESCAPE` for the controls (a modal `<dialog>` cancels and closes); an open select
    closes (`HideSelectBox`). Otherwise nothing: menus close in their `keydown` listener.
  - any key shows `:focus-visible` on the focused element (also one a click focused).
  - The key the controls see is the event's `key_identifier` parameter; scripts see the pad's key
    and code (`gamepad`, `gamepad_key`, `repeat` parameters). The fork's text widget leaves events
    with a `gamepad` parameter to the document (P4-3), so the d-pad leaves a field and B reaches
    the menu.
- **Gamepad focus and consuming.** The documents take the pad while an element **in the Tab
  order** (`tab-index: auto`: a button, link, form control, `tabindex="0"`) of a shown document
  has the focus of the context keys go to (the screen's, or the texture-mode document that took
  the last press, as for keys). RmlUi focuses any clicked element (`focus: auto` is the
  default), so a clicked HUD panel or a `tabindex="-1"` element has the keys but not the pad: the
  d-pad stays the game's. With such a focus, the directions and A are always consumed (also at
  the edge of a menu: the player does not move while a menu is used); B and the other keys when
  a listener or a control stopped or prevented them; an up when its down was consumed (the game
  never gets an up without a down). Without one, the keys still go to the focused document (as
  keyboard keys do) and nothing is consumed. Consumed keys skip `onGamepadButtonDown/Up` while
  `Engine::ignoreEventsHandledByUI`.
- **Axes.** The left stick is a d-pad: a direction is pressed when its axis passes 0.5 and is
  the dominant one, and held until its own component drops below 0.3 (hysteresis, no flapping at
  the diagonal). Triggers (rest −1) press past 0 (half way) and release below −0.2. Left-stick
  axis moves are consumed while a document has gamepad focus or the held stick direction was
  consumed; trigger moves while the trigger key's down was consumed; the right stick never. An
  axis the documents take is first reported to the game once at its rest value (0, triggers −1),
  so a player walking when a menu opens stops. Polling (`Input.getGamepadAxis`) always reads the
  device; `Engine::wantsGamepadNavigation()` (Lua `Engine.wantsGamepadNavigation()`) tells a
  polling game that a document has gamepad focus.
- **Repeat.** A held d-pad or stick direction whose down a document consumed repeats after
  0.4 s, then every 0.1 s (`keydown` with `repeat`), on the UI clock (`HtmlUIClock::now()`,
  `Engine::updateGamepadNavigation` after `flushCharInput` at the start of `systemDraw`): frozen
  with a held simulation, deterministic under a fixed time; a long frame gives one repeat, not a
  burst. A down that arrives again without its up is a repeat.
- **Routing** (`Engine.cpp`): the documents first, top scene layer first, the first scene whose
  documents consume stops the walk (`dispatchToDocuments`, as keys); ups and the disconnect's
  releases go to every scene's documents. `UIDocumentSystem::eventOnGamepadKeyDown(int gamepad,
  GamepadUIKey, bool repeat)`, `eventOnGamepadKeyUp`, `hasGamepadFocus`; `GamepadUIKey`
  (`UIDocumentSystem.h`); `RmlKeyMap::gamepadDomCode/gamepadDomKey/gamepadEngineKey`. A
  disconnect releases what the pad held (`:active`, ups).
- **Platforms.** Every platform layer already funnels its pads into `Engine::systemGamepad*`
  (Linux evdev `GamepadLinux.cpp` and the editor's `BackendLinux.cpp`, Windows XInput
  `GamepadWin.cpp`, macOS and iOS GameController `JasperEngineGameController.mm`, Android
  `NativeEngine.cpp` (GameActivity), Web `JasperEngineWeb.cpp` (the Gamepad API, polled per
  frame), the editor's MCP `send_input`): the routing is platform-independent, with the stick's
  down-positive Y every layer already produces. Built and tested here: Linux (glcore, Vulkan),
  Web and Android builds; Windows, macOS and iOS not built (no code of theirs changed).

**Data binding.**

- `data-style-*` (fork P4-4): in a model created with `allow_missing_variables` (the engine's
  placeholder models of edit mode, and Lua models), an expression that reads a variable the
  model does not have yet leaves the property unset (removed) instead of setting the empty
  default's text (`health + '%'` was `width: %`, a syntax warning per load). The variable's
  arrival sets it. A value invalid with every variable present still warns. The guide's note is
  gone; `test_html_ui_docs.py` no longer exempts the line.
- `data-for` template: the Lua DOM (`UIDocument`/`UIElement` `querySelector(All)`,
  `getElementById`, `children`, `childElementCount`, `first/lastElementChild`,
  `next/previousElementSibling`) and the inspector (`inspect::isElement`: `query`, the tree,
  refs) skip the hidden element RmlUi keeps after the rows (`UIElementAccess::isDataForTemplate`:
  it carries `data-for`, the rows do not). Structural selectors and sibling combinators skip it
  too (fork P4-5), so `li:last-child` is the last row, as with the comment node a browser
  framework leaves. Rows keep their order (inserted before the template). There is no
  `getElementsBy*` in the Lua API.

**Examples** (merged from `p4-examples` while this package ran): the `x > 0 ? x + '%' : '0'`
workaround of the hud, loading and inventory bindings is gone (plain `x + '%'`); the inventory
test counts `.grid > .slot` as 24 without `:not([data-for])`. The menus are gamepad-navigable,
checked in `test_html_ui_examples.py` after each golden (the goldens are unchanged):
`main_menu` (the d-pad through the buttons, A presses, the d-pad stays on the open settings
panel and B closes it), `pause_menu` (Start opens on Resume, B closes; `onGamepadButtonDown`,
as its Escape uses `onKeyDown`), `settings` (the first tab has the focus at load; tabs, every
control of the tab, the footer; ranges adjust, switches toggle, selects open and choose; Escape
or B is Back), `inventory` (slots `tabindex="0"`, the tooltip follows the focus, A picks up and
puts down, B cancels or closes, View toggles the window, the first gamepad key focuses the first
slot), and the shop's confirmation dialog (the d-pad stays on its buttons, B cancels and gives
the focus back to the card's Buy).

**Tests.** `engine/tests/htmlui/TestInputGamepad.cpp` (14: grid and list navigation with
scrolling, `nav-*` overrides, activation and `:active` with a checkbox and `preventDefault`, B as
a cancelable Escape and its pass to the game, a modal dialog, range adjust, select open/navigate/
choose/close, a text field that does not trap, consume vs game with the axis rest value and
`ignoreEventsHandledByUI` off, `:focus-visible`, repeat timing on the fixture clock with the
stick, shoulders/triggers/other buttons as keys and a disconnect, a clicked panel);
`TestTextureMode.cpp` `GamepadKeysFollowTheKeys`; `TestDataBindingDom.cpp` (2); RmlUi unit
tests `Navigation.FindNavigationTarget`, `form.input.text.gamepad_keys`,
`data_binding.style_missing_variables`, `data_binding.for_template_not_a_sibling`;
`tools/mcp/tests/test_html_ui_gamepad.py` (8, play mode through `send_input` gamepad events:
d-pad and A, focus-visible screenshot, B, shoulder and trigger, range and stick repeat, the
unfocused pad is the game's, edit-mode silence, data-for rows).

---

## 5. Test plan

Every MCP test runs with `tools/mcp/run-tests.sh tools/mcp/tests/<file>` against the real editor
under Xvfb, on the default fixture (TappyPlane, canvas 1280×720) unless the case says otherwise.
Shared test data:

- `LLM_HTML` / `LLM_CSS` (5.7);
- `GOOD_HTML` / `GOOD_CSS`: a 30-line panel with a title, two buttons and a list, using flex,
  grid, `var()`, `clamp()` and `::before`, compat-clean.

### 5.1 C++ (`jasperengine-htmlui-tests`)

**`InspectorTest.cpp`**

1. `documents()` lists exactly the GameObjects with a `UIDocumentComponent`.
2. `documentInfo`: html path, viewport 1280×720, `loadCount` 1, `loaded`, diagnostics of a
   document with one unsupported property (code present, location file/line/col right).
3. `query("")` returns body first. The parent and depth links are consistent. `limit` truncates
   and sets `truncated`.
4. `ref` uniqueness: in a document with duplicate classes, nested lists and repeated tags,
   `query(e.ref)` returns exactly `e` for every element.
5. Boxes: a 200×100 div with padding 10, border 2, margin 5 at `left: 40px; top: 30px` gives the
   exact border box, and `detail` gives the exact content, padding and margin boxes and edges.
6. Flags, one case each:
   - a nowrap 300px label in a 100px box is `overflowX`;
   - a child inside `overflow:hidden` is `clipped`;
   - `display:none` parent → `hidden`;
   - opacity 0.02 → `transparent`;
   - `left: 5000px` → `offscreen`;
   - `width: 0` → `zeroSize`.
7. `hitTest`:
   - a button over a panel returns the button, then the panel, then body;
   - a `pointer-events: none` overlay is skipped;
   - a point outside every element returns an empty stack.
8. `detail` styles:
   - `background-color` from `menu.css:12:3` with selector `.btn.primary`;
   - an inline style gives the html line plus `(style attribute)`;
   - `color` inherited from the parent names the parent ref;
   - an unset property requested explicitly gives `default`;
   - an unknown property is an error.
9. `withViewport(720, 1280)`: a `@media (max-width: 800px)` rule changes `flex-direction`. The
   boxes inside `fn` reflect it, and after return the viewport and boxes are back to 1280×720.
10. An invalid selector `div[[` is an error; `elementRect` skips hidden matches.

**`AssertionsTest.cpp`.** Each kind gets a pass case, a fail case (message numbers checked) and
its edge cases:

- zero matches fail;
- `count` with `max: 0` passes on zero;
- `NoOverlap` ignores ancestor/descendant pairs and hidden elements, and `with` works;
- `WithinParent` with `container`;
- `TextFits`: `allowScroll` inside `overflow:auto` passes and fails without it; `singleLine`
  fails on wrapped text; text clipped by an ancestor fails;
- `Contrast`:
  - white on `#000` passes at 21:1;
  - `#777` on `#888` fails;
  - large text uses 3.0;
  - a gradient background is inconclusive;
  - a transparent root is inconclusive without `background` and computed with it;
  - an `rgba` foreground is composited;
- `MinSize` 44 default;
- `Visible` with `unobstructed` fails when a sibling covers the center.

**`ExamplesCompatTest.cpp`.** For each example and `blank`, `compat::checkDocument` (files read
through a `FileAccess` on the source dir, `verifyWithRmlUi = true`) over the
embedded tree (loader on the source dir) gives `ok`, zero Warning and zero Error.

### 5.2 `test_html_ui_checks.py` (CLI + parity)

1. **`test_cli_usage_ui`.**
   - `check --help` documents `--ui` and the new default.
   - `ui-support -h` exits 0.
   - The main `--help` lists `ui-support`.
2. **`test_cli_default_includes_ui`.**
   - A fixture copy with `GOOD_HTML`/`GOOD_CSS` under `assets/ui/` gives exit 0 for `check`, and
     the JSON `checked` has `ui: 1`: one document, and its linked css is not counted as a
     standalone stylesheet.
   - Adding an orphan css makes it 2.
   - `--lua` alone has no `ui` key.
3. **`test_cli_ui_findings_text_and_json`.** Write `LLM_HTML`/`LLM_CSS`, then run
   `check --ui --format json`:
   - `findings[*].tool == "ui"`;
   - the keys are the 6 base keys plus `code`, plus `suggestion` when not empty;
   - paths are project-relative;
   - css diagnostics are reported at the css path;
   - the `<script>` finding is an error at its exact line and column;
   - exit 1;
   - the text lines match `path:line:col: severity: message (fix: ...) [code]`;
   - the order is sorted by path, then line.
4. **`test_cli_ui_files_and_exclusions`.**
   - `--files assets/ui/x.css` checks only that css (`checked == {"ui": 1}`).
   - An html outside the project is skipped.
   - The web shell named in project.yaml `web.customHtmlShell` is not checked by `--ui`.
   - A file in `build/` or `.jasperengine/` is ignored.
   - A missing linked css is an error at the `<link>` line.
   - A missing image is an error.
   - A remote font link is an error whose suggestion mentions `import_files`.
5. **`test_cli_ui_dedupe`.** Two documents linking the same broken css report each css finding
   once.
6. **`test_hook_ui_warnings_once`.**
   1. A Claude Code `Write` of `LLM_CSS` (warnings only) gives exit 2, the warnings on stderr,
      and the "shown once" line.
   2. The same input again gives exit 0, silent.
   3. Adding one more unsupported declaration through Bash `sed` (no file in the hook input)
      gives exit 2 with only the new warning.
   4. Fixing it gives exit 0.
   5. An html with a `<script>` gives exit 2 on every run while it is there (errors always show).
   6. A Codex `apply_patch` adding an html works the same.
   7. The state file has `version: 2` and `ui_reported`.
   8. A hand-written version-1 state file is accepted.
7. **`test_cli_ui_speed`.** `check --ui` on a project holding all 10 template trees (copied from
   `.jasperengine/engine-api/html-ui-examples/` of a project the editor opened) takes < 2 s and
   reports zero warnings.
8. **`test_ui_support_json_and_generated_doc`.**
   - `ui-support` gives valid JSON: version, features with valid kinds and supports, every
     `unsupported`/`fallback`/`approximated` feature has a `code` present in `diagnostics`.
   - `gen_html_ui_support.py --check` exits 0, so the committed matrix is current.
   - Corrupting a row in a temp copy (`--out tmp` then edit, `--check --out tmp`) exits 1 with a
     diff.
9. **`test_parity_cli_mcp`** (wave 2). For `LLM_HTML`, the normalized sets
   `{(path, line, col, severity, code)}` of CLI findings, `inspect_ui {file}` diagnostics,
   `inspect_ui` of a live document loading the same html, and the `external_changes` problems
   after writing it are all equal (live-document infos excluded).

### 5.3 `test_html_ui_reload.py`

Setup: a 2D scene with a document `assets/ui/panel.html` (`GOOD_HTML`) + css. In wave 1 it is
built with generic tools; in wave 2 with `create_ui_document`.

1. **`test_tracked_extensions`.** Edits to `.html`, `.css`, `.ttf`, `.json`, `.lottie` and `.svg`
   are each listed in the next call's `external_changes.changed`. A `.css` under `build/` is not.
2. **`test_css_edit_reloads_before_call`.**
   1. Change `.title { color: ... }` on disk.
   2. Call `inspect_ui {selector: ".title", styles: ["color"]}` immediately.
   3. The new color is shown, `ui_reloaded` lists the document with `stylesheet_only: true`, and
      `load_count` goes up by exactly 1.
3. **`test_no_double_reload_autosync_first`.** After step 2, `wait`/sleep 2.5 s (longer than
   phase 1's poll interval), then `inspect_ui` again: `load_count` is unchanged and there are no
   `external_changes`.
4. **`test_no_double_reload_poller_first`.**
   1. Edit the css and sleep 2.5 s; the poller reloads.
   2. The next call's `external_changes` lists the file, `ui_reloaded` is empty, and `current` is
      1.
   3. `load_count` went up by exactly 1 in total.
5. **`test_html_error_reported`.** Writing an html with an unclosed tag and a `<script>` gives
   `problems` with `path`, `line`, `col`, `severity`, `code` and `suggestion`, and the text line
   reads `assets/ui/panel.html:7: ...`.
6. **`test_orphan_css_checked`.** A new css that no document links, with an unsupported property,
   gives a problem from `ProjectChecks::checkUiFiles` and `ui_reloaded` empty.
7. **`test_reload_during_play`.**
   1. `play start` the scene, edit the css, call `inspect_ui`.
   2. The runtime document shows the new style.
   3. `external_changes` has no `deferred` text for the css and lists `ui_reloaded`.
   4. Lua listeners still work after a css-only reload: `send_input {target: "#ok"}` logs.
   5. After an html change, `onLoad` rebinding works too (click logs).
   6. Stop, then call: no second reload of the edit-time document for the same change
      (`load_count` checked).
8. **`test_mcp_write_not_external`.** `write_file` of the css reloads once (`load_count` +1) and
   the next call has no `external_changes`.
9. **`test_image_dependency_reload`.** Replacing the png an `<img>` uses reloads the document
   (counted) and reloads the scene texture as before.
10. **`test_sync_files_reports_ui`.** `sync_files {paths: [css]}` returns `ui_reloaded` and the
    problems.
11. **`test_read_write_file_html`.** `read_file` / `write_file` accept `.html` / `.css`, which
    was refused before (`isTextFile`).

### 5.4 `test_html_ui_examples.py`

Per example `<n>` (parametrized by reading `examples.json`, as test functions generated at module
level `test_<n>_...`):

1. **`test_<n>_files_in_project`.**
   - `.jasperengine/engine-api/html-ui-examples/<n>/` exists with the manifest's files.
   - `examples.json` there parses.
   - The resource `jasper://examples/html-ui/<n>/<n>.html` matches it (wave 2).
2. **`test_<n>_compat_clean`.** `check --ui --files` on the copied files gives zero findings.
3. **`test_<n>_edit_mode`.**
   1. Build the document in a new 2D scene `UITest_<n>` from the copied files: wave 1 uses
      generic tools, wave 2 uses `create_ui_document {template: n}`.
   2. The document loads, `diagnostics_count` is zero errors and zero warnings, and `screenshot`
      is not flat.
4. **`test_<n>_play_golden`.**
   1. `play {action: start, scene, fixed_fps: 60, auto_pause: true}`, then `wait {frames: 90}`.
   2. Run the per-example interaction step below.
   3. Golden comparison (3.9), with the double-run determinism check.
   4. The console has no errors; `stop`.
5. **`test_<n>_asserts`** (wave 2). In play at the same point, `assert_ui {checks: manifest.checks,
   viewports: manifest.viewports}` gives `passed: true`. Then one deliberate failure: shrink a
   label's width through `eval_lua` `el.style.width = "20px"`, then `text_fits` fails with that
   element's ref and returns an image whose red outline pixels lie inside the reported rect
   mapped into the image.

Interaction steps (before the golden):

| Example | Step | Then |
|---|---|---|
| main_menu | `send_input [{mouse move target: "#settings"}]` | `inspect_ui` shows `pseudo` contains `hover` (wave 2). Click `#settings`: `#settings-panel` has class `open` (via `eval_lua` `UIDocument(...)` in wave 1). Click `#settings-back`: the class is gone. Click `#play`: the console has `main_menu: play`. Key ArrowDown, then Enter: the console has `main_menu: settings`. The golden is taken with the panel closed. |
| pause_menu | key `escape` | `#pause` has `open` and `Engine.timeScale == 0` (`eval_lua`). Golden taken while open. Escape again: closed, timeScale 1. |
| hud | `eval_lua script("HUD"):damage(30)` (or the test's entity path), `wait frames 30` | `#health-fill` width = 70% of the track ±1px (`eval_lua` `getBoundingClientRect`). `fire()` ×3 makes `#ammo` text decrease by 3. Golden after damage. |
| inventory | `send_input [{mouse drag target: ".slot:nth-child(1)", to_target: ".slot:nth-child(6)"}]` | The model's items 1 and 6 are swapped (`eval_lua getDataModel`). Hover a slot: `#tooltip` has `show`. Golden with the tooltip shown. |
| settings | click `#tab-audio`; drag the music slider to its 25% point; click the vsync switch | The `active` tab moved. Model `music` is 25 ±3 and `vsync` toggled (`getDataModel`). Golden on the audio tab. |
| dialogue | `wait {frames: 240}` (line typed), click `.choice:nth-child(2)` | The console has `dialogue: choice 2` and `#line` text changed. Golden with the choices shown, before the click. |
| shop | click a `.buy` the player can afford, then `#confirm-buy` | `#gold` decreased by the price, and the console has `shop: bought`. A too-expensive card's `.buy` has the `disabled` attribute. Golden with the modal open, before confirm. |
| toasts | (`init` shows 3) `wait frames 30` | 3 `.toast` exist. `wait {game_seconds: 4}` leaves 0. Calling `show` 7 times leaves 5. Golden at 3 toasts. |
| loading | `wait {game_seconds: 1.5}` | `#percent` is between 45 and 55. `wait {game_seconds: 2}`: `#press-key` is visible and the console has `loading: done`. Golden at 1.5 s. |

### 5.5 `test_html_ui_docs.py`

1. **`test_guide_served`.**
   - `jasper://docs/html-ui` exists and is split into sections, all 16 present by title (the
     test keeps the list in one constant that phase 5 extends with "Effects").
   - `jasper://docs/html-ui-support` exists.
   - `jasper://docs` lists both.
2. **`test_guide_names_exist`** (wave 2).
   - Every backticked `snake_case` word in `html-ui.md` is a tool, a parameter or an enum value
     (the same rule as `test_instructions_name_real_tools`).
   - Every `doc:`/`el:`/`anime.` call name appears in the generated Lua stubs.
3. **`test_guide_lua_snippets_parse`.** Every ```` ```lua ```` block of `html-ui.md` passes
   `check_script {code, language: lua}` (syntax).
4. **`test_guide_css_html_snippets_clean`.** Every ```` ```html ````/```` ```css ```` block marked
   as a good example (not in the habits table) written to a file gives zero `check --ui`
   warnings.
5. **`test_search_engine_api_ui`.** `search_engine_api` returns:
   - `"querySelector"` → `UIDocument:querySelector` and `UIElement:querySelector`;
   - `"anime animate"` → `anime.animate` with parameter details;
   - `"addEventListener"` → the `UIElement` entry;
   - `"mix-blend-mode"` → a doc hit in `jasper://docs/html-ui-support`.
6. **`test_luals_ui_stubs`** (skips without lua-language-server).
   - Zero Warning+ diagnostics on every example Lua script, copied into the fixture's Lua dir.
   - `BAD_UI_LUA` reports these codes on these lines:
     - `doc:getElementByID` → `undefined-field`
     - `doc:querySelector(42)` → `param-type-mismatch`
     - `el:addEventListener("onclick", f)` → `param-type-mismatch` (`UIEventType`)
     - `anime.animate(el, 5)` → `param-type-mismatch`
     - `el.innerText = "x"` → `inject-field`
     - `document.querySelector("#x")` → `undefined-global`
7. **`test_agents_md_section`.** A project created with `create_project` has
   `## HTML/CSS UI` in AGENTS.md, mentioning `check --ui` and
   `.jasperengine/engine-api/html-ui-examples/`.
8. **`test_stubs_unresolved_empty`.** The HTML UI stub files have no `-- unresolved:` lines;
   `test_stubs.py` keeps its own global check.

### 5.6 `test_html_ui_tools.py`

**Schemas and budget**

1. **`test_tools_listed_with_annotations`.**
   - `inspect_ui` and `assert_ui` are readOnly and idempotent.
   - `create_ui_document` is not readOnly and not destructive.
   - `allowedWhilePlaying` is visible through behaviour: `create_ui_document` during play is
     refused with the registry's message.
   - The `template` enum equals the manifest names plus `blank`.
2. **`test_budget`.** Per-tool sizes and the total are within the limits of 3.6.7, using
   `tool_budget.measure` logic imported from `tools/mcp/tool_budget.py`.

**`create_ui_document`**

3. **`test_create_blank`.**
   - `{html: "assets/ui/menu.html"}` writes the html and css (no Lua).
   - The GameObject is named "Menu" with a `uidocument` component whose `html` reads back
     project-relative (`get_entity`).
   - The document loaded with zero diagnostics, `load_count` 1.
   - `undo` (scene) removes the GameObject; `undo scope: project` removes both files.
4. **`test_create_template_renames`.** `{html: "assets/ui/title_screen.html", template:
   "main_menu"}` writes:
   - `title_screen.css`;
   - `title_screen_assets/logo.svg`;
   - `scripts/TitleScreenUI.lua` with `---@class TitleScreenUI`, attached.

   The html links `title_screen.css` and references `title_screen_assets/`, and the document has
   zero diagnostics.
5. **`test_create_existing_kept`.**
   - An existing html is kept (`files_kept`), and the document loads it.
   - With `overwrite: true` it is replaced.
   - With `script: true` on `blank`, `BlankUI`-derived Lua is created.
6. **`test_create_errors`.**
   - An html outside the assets dir fails with a message naming the assets dir.
   - A `.css` given as `html` fails.
   - An unknown `properties` key fails with the Catalog list, and nothing is created (scene and
     files unchanged).

**`inspect_ui`**

7. **`test_inspect_tree_default`.**
   - On `GOOD_HTML` the outline text starts with the document line, and every element of
     `elements` has a `ref` that re-queried with `selector: ref` returns exactly 1 element with
     the same box.
   - `limit` truncation.
   - `depth`.
8. **`test_inspect_full_styles_sources`.**
   - `selector: ".title", detail: "full"` returns the box model, attributes, and a
     `background-color` style whose `source` is `assets/ui/panel.css:<line>:<col>` (line found by
     searching the css text) with the right selector.
   - Inherited and default sources.
   - An unknown style name gives an error with "did you mean".
9. **`test_inspect_hit_test`.** `at` on the button center (from its box) gives `hit[0].ref` = the
   button; `at` outside gives empty `hit` and a note.
10. **`test_inspect_flags`.** A document with an overflowing label, a clipped child and a hidden
    panel gets the matching flags in structured output and text.
11. **`test_inspect_viewport`.** `viewport: [720, 1280]` changes the layout of a media-query
    panel. The next call without `viewport` shows the original boxes. `viewport` + `screenshot`
    is an error.
12. **`test_inspect_models_play`.**
    - In play with the hud example, `models: true` returns `{"hud": {"health": ...}}`.
    - In edit mode it returns `{}` and the note.
13. **`test_inspect_file_mode`.**
    - `file` gives diagnostics equal to the CLI, and `documents_using` lists the live document.
    - `file` + `selector` is an error.
    - A `.lua` `file` is an error.
14. **`test_inspect_screenshot`.**
    - `selector: "#ok", screenshot: true` returns an image.
    - Decode it with `pngdiff`: cyan outline pixels form a rectangle whose size matches the
      element box times the image scale ±3 px.
    - For a render-to-texture document: no image, and a note.
15. **`test_document_resolution`.**
    - No document gives the error mentioning `create_ui_document`.
    - Two documents and no `entity` give an error listing both paths and htmls.
    - An entity without the component gives the error listing the documents.
    - A document whose html is missing gives the load diagnostics in the error after waiting.
16. **`test_invalid_selector`.** `selector: "div[["` gives `isError` with the inspector's
    message.

**`assert_ui`**

17. **`test_assert_all_kinds_pass_and_fail`.** One document built for it (fixed sizes) runs, in
    one call, a passing and a failing check of every kind. `passed` is false, `failed` equals the
    number of failing checks, each failure has the expected `ref`, and the message contains the
    expected number.
18. **`test_assert_zero_matches_fail`.** A misspelled selector gives "matched nothing" and
    `passed: false`, not `isError`.
19. **`test_assert_viewports`.** A layout that fits at 1280×720 but overflows at 720×1280
    (fixed-width row) has `within_viewport` pass at the first viewport and fail at the second,
    and the result lists the viewport. The image covers only the current-viewport failures, and
    the note says so.
20. **`test_assert_screenshot_modes`.**
    - `never`: no image.
    - `always`: an image with cyan outlines.
    - `on_failure` on a passing set: no image.
21. **`test_assert_contrast_background`.**
    - A transparent HUD label is reported `inconclusive` without `background`.
    - With `background: [0.5,0.5,0.5]` it is computed; it fails for grey text and passes for
      white with a dark panel.

**`send_input` target**

22. **`test_send_input_target_click`.** In play, `{type: mouse, action: click, target: "#ok"}`
    triggers the Lua listener (console). The event result reports the resolved point. An unknown
    selector fails with the list of documents searched.
23. **`test_send_input_target_late_resolution`.** Click `#open`, which shows `#panel`, then click
    `#panel .close` in the same `events` list: the second resolves after the first ran and
    succeeds.
24. **`test_send_input_drag_to_target`.** With the inventory example (`create_ui_document`
    template), `drag target/to_target` swaps the items.
25. **`test_send_input_touch_target`.** `touch tap target` works with `simulate_touch`.

**`import_files`**

26. **`test_import_fonts_icons_urls_mirror`.** With the mirror (3.9):
    - `fonts: ["Karla:400"]` writes a `.ttf` + `font_face_css`;
    - `icons: ["lucide:play"]` writes `play.svg`;
    - `sources: ["https://.../anim.json"]` writes the json;
    - a missing mirror entry gives `failed` with "not found" and not `isError`, because others
      succeeded.
27. **`test_import_from_document_rewrites`.**
    - An html with a Google Fonts `<link>` and a remote `<img>`, run with `from_document`,
      rewrites both to local paths (undoable: `undo scope project` restores the remote URLs).
    - The document then has zero diagnostics.
28. **`test_import_requires_something`.** `{}` gives an error naming the four options. Local
    `sources` without `to_dir` gives an error.

**Resources**

29. **`test_examples_resources`.**
    - `jasper://examples/html-ui` lists 9 examples.
    - Every manifest file is readable through the template URI with the right mime.
    - An unknown path gives a resource-not-found error.

### 5.7 `test_html_ui_agent_sim.py` (end to end, the typical LLM output)

`LLM_HTML` (`assets/ui/shop_llm.html`) and `LLM_CSS` (`assets/ui/shop_llm.css`) read like a
model's first attempt at a shop screen. They contain:

- **Constructs that must be reported** (definitional):
  - `<script src="app.js"></script>`;
  - `<button onclick="buy('sword')">`;
  - `<link href="https://fonts.googleapis.com/css2?family=Karla:wght@400;700&display=swap" rel="stylesheet">`;
  - `<img src="https://example.com/sword.png">`;
  - `<canvas id="fx"></canvas>`.
- **Constructs whose expectation comes from the support table** (`ui-support` JSON, looked up by
  `kind` + `name`):
  - `position: sticky`;
  - `.card:has(.sale)`;
  - `mix-blend-mode: screen` and `@container (min-width: 400px)` (unsupported after phase 3, so
    `LLM_CSS` always has warnings for the hook tests);
  - `text-overflow: ellipsis`;
  - `@media (prefers-color-scheme: dark)`;
  - `aspect-ratio: 1`;
  - `display: grid` with `grid-template-columns: repeat(auto-fill, minmax(160px, 1fr))`;
  - `gap`;
  - `transition: all .2s ease-in-out`;
  - `rgba(0,0,0,.5)`;
  - `border: 1px solid #333`;
  - `border-radius: 50%`;
  - `::before`;
  - `clamp()`;
  - `var(--accent)`;
  - `!important`;
  - `font-family: 'Karla', sans-serif`.

  The rule: a diagnostic of severity ≥ warning exists at the construct's line when its support
  is `unsupported` or `fallback`, and none when it is `native`, `rewritten`, `emulated` or
  `ignored`; for `approximated` the row's diagnostic severity decides (Info: none at warning
  level). Lines are found by substring search in the test's own
  text.
- **A layout bug:** `.buy-legendary { width: 120px; white-space: nowrap }` with the label
  "Purchase Legendary Sword of Infinite Destiny".

Steps:

1. **`test_cli_reports_llm_mistakes`.**
   - `check --ui --files` both files in JSON.
   - Every definitional construct has a finding at its line with a non-empty `suggestion`:
     an error for `<script>` (`HTMLUI-HTML-0006`), `onclick` (`HTMLUI-HTML-0005`) and the two
     remote URLs (`HTMLUI-IMP-0001`), a warning for `<canvas>` (`HTMLUI-HTML-0002`).
   - The table-driven rule holds for every listed construct.
   - Every `code` is in the `ui-support` diagnostics list.
2. **`test_hook_feedback_like_claude_code`.** A PostToolUse `Write` of `LLM_CSS`, then of
   `LLM_HTML`, gives exit 2 with the findings on stderr in compiler style. Running again on the
   unchanged files: the html errors are shown again, the css warnings are not.
3. **`test_editor_loop`.**
   1. `create_ui_document {html: "assets/ui/shop_llm.html"}`: the existing file is used. The
      result's `diagnostics` equal the CLI findings for the same files (parity, normalized).
   2. `inspect_ui`: the outline shows `button.buy-legendary` with `!overflow-x`.
   3. `assert_ui {checks: [{type: text_fits, selector: ".buy-legendary"}]}` fails with the ref,
      the overflow px, and an image whose red rectangle covers the button.
4. **`test_fix_and_reload`.** The test "fixes" the files the way the suggestions say:
   1. remove `<script>` and `<canvas>`;
   2. move `onclick` into `ShopLlmUI.lua` with `addEventListener`;
   3. `import_files {from_document: "assets/ui/shop_llm.html"}` for the font and the image
      (mirror);
   4. replace every construct reported at warning level with the suggested form;
   5. let the long label wrap (`white-space: normal; width: 240px`).

   The next `inspect_ui` call carries `external_changes` with `ui_reloaded` (the document) and
   `problems` empty. `load_count` went up by exactly the number of reload rounds. `check --ui`
   gives zero findings. `assert_ui` `text_fits`, `within_viewport` and `no_overlap` on `.card`
   pass.
5. **`test_screenshot_and_play`.**
   - `screenshot` is not flat, and the shop card area differs from the background (pixel
     variance inside the card's box).
   - `play`, then `send_input {target: ".buy-legendary"}` logs `shop_llm: buy legendary` from
     the Lua listener. `read_console` has no errors. `stop`.

---

## 6. Acceptance criteria

Run on `html-ui` after all five packages merged, from a clean build
(`JOBS=3 tools/build.sh linux`).

**Build and C++**

- [ ] The Linux editor and engine build with zero new warnings in the files phase 4 added or
      changed. The Web build of the engine compiles `HtmlUIInspector` / `HtmlUIAssertions`.
- [ ] `jasperengine-htmlui-tests` passes: `InspectorTest`, `AssertionsTest` and
      `ExamplesCompatTest` (5.1).

**CLI**

- [ ] `check --ui` and the default `check` report UI findings in text and JSON as specified.
      Exit codes are unchanged. Non-UI finding JSON keeps exactly 6 keys.
- [ ] `--hook` shows UI errors always and UI warnings once; state version 2; version 1 is
      accepted.
- [ ] `ui-support` prints the JSON of 3.4.2. `gen_html_ui_support.py --check` passes on the
      committed `docs/mcp/guide/html-ui-support.md`.

**Hot reload**

- [ ] `.html/.htm/.css` and the asset types listed in 3.5.1 are tracked. Documents reload before
      the next call in edit mode and during play.
- [ ] No reload happens twice (5.3 cases 3-4 and 7). `external_changes` carries `ui_reloaded`
      and the full problem entries.

**Tools**

- [ ] `inspect_ui`, `assert_ui` and `create_ui_document` exist with the schemas, outputs, errors
      and annotations of 3.6. `send_input` `target`/`to_target`/`document` and `import_files`
      `fonts`/`icons`/URLs/`from_document`/`rewrite` work.
- [ ] Tool budget: per-tool and total limits of 3.6.7 hold (`test_budget`). `TOOLS.md` was
      regenerated with an "HTML/CSS UI" section. The README budget sentence is updated.

**Examples**

- [ ] 9 examples + `blank` exist per 3.7. Each has:
  - zero compat warnings and errors;
  - a golden image inspected by a human or agent and committed;
  - a passing play test with interaction;
  - passing manifest `assert_ui` checks at every manifest viewport;
  - zero LuaLS warnings (when LuaLS is installed).
- [ ] The examples are present in `.jasperengine/engine-api/html-ui-examples/` of every
      opened/created project, are served as `jasper://examples/html-ui/...`, and are
      instantiable through `create_ui_document template` with the renames of 3.7.4.

**Docs**

- [ ] `docs/mcp/guide/html-ui.md` has all 16 sections, every name in it exists, and its Lua
      snippets parse. It and the support matrix are served under `jasper://docs/`.
- [ ] The instructions carry the HTML-UI section; `test_instructions_name_real_tools` passes
      with the new pairs. New projects' AGENTS.md has the HTML/CSS UI section.
      `docs/mcp/checks.md`, `docs/mcp/README.md`, `docs/mcp/DESIGN.md` (pick-up) and
      `docs/mcp/guide/README.md` are updated.
- [ ] `search_engine_api` finds the Lua UI API and the support matrix rows (5.5 case 5).

**Test suites**

- [ ] `tools/mcp/run-tests.sh -j 2 -k html_ui` passes with no skip, except "lua-language-server
      not installed".
- [ ] These also pass: `test_protocol.py`, `test_autosync.py`, `test_checks.py`,
      `test_stubs.py`, `test_ui_anchors.py`, `test_play.py`, `test_assets.py`, `test_docs.py`.
- [ ] The agent simulation (5.7) passes end to end.

**Process**

- [ ] Every package report lists its additive touches outside its owned files, and every
      difference found between 3.2 and the merged phase 0-3 code.
