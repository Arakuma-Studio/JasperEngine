# Phase 0: foundation

Binding inputs: `docs/html-ui/DESIGN.md` (architecture), `docs/html-ui/BRIEF.md` (rules for the
implementing agents). This spec details phase 0. Every path is relative to the repository
root. `file:line` references were verified on branch `html-ui` at commit `d20d3a77`; lines
move, so search for the quoted code when a number is off by a few.

Contents:
1. Outcome
2. Scope in / out
3. Detailed design
   - 3.1 Frame flow and ownership overview
   - 3.2 RmlUi vendoring and build integration
   - 3.3 Engine backend core: `HtmlUI`, `RmlSystem`, `RmlFile`
   - 3.4 Render abstraction changes and the `HTMLUI` shader type
   - 3.5 `RmlRenderer`, the per-context offscreen target and the composite
   - 3.6 Text: STBText refactor and `RmlFontEngine`
   - 3.7 ECS, GameObject API, input, editor and MCP integration
   - 3.8 Test infrastructure
   - 3.9 SVG library benchmark
   - 3.10 Decisions and deviations from DESIGN.md
4. Work packages
5. Test plan
6. Acceptance criteria

---

## 1. Outcome

When phase 0 is merged into `html-ui`:

1. RmlUi 6.3 (Core + Debugger) is vendored at `engine/libs/rmlui/` and builds as a static
   library with hidden visibility and no FreeType. It is part of the engine build on Linux
   (glcore, vulkan), Windows (glcore, d3d11, vulkan), macOS (glcore, metal, vulkan), iOS
   (metal), Android (GLES3) and Web (WebGL2), of the editor's shared engine, of every
   exported game (desktop/Web/Android through `engine/CMakeLists.txt`, Apple through the
   Xcode workspace). `docs/html-ui/FORK.md` records upstream and patches.
2. A GameObject with a `UIDocumentComponent` whose `filename` points to a project file
   such as `ui/menu.html` shows that document over the scene: backgrounds, borders,
   rounded borders, images (PNG/JPG/SVG through the engine's image loader), flexbox
   layout, overflow clipping (scissor), CSS transforms, and text in the built-in fonts
   (Latin, Cyrillic, Greek, Arabic with shaping and bidi) or any TTF/OTF loaded through the
   C++ API. The document is laid out in canvas pixels, rendered into a per-scene offscreen
   target at screen resolution, and composited in the scene's main camera pass in front
   of the component UI. This works on every backend; on Linux glcore and vulkan it is
   proven by golden images, on Web by a headless-browser screenshot.
3. Pointer down/up/move (mouse and first touch) reach documents, so `:hover`/`:active`
   styles work and pointer events over document elements are consumed (not passed to the
   game when `ignoreEventsHandledByUI`).
4. The component is a first-class editor component: Add Component (UI category),
   Inspector rows (Source, Sort Order, enabled checkbox), scene save/load (YAML),
   prefab/clipboard, `Instantiate` cloning, C++ export (`Factory`), MCP `add_component` /
   `set_properties` / `get_entity`. Documents show in the editor Game view, in play mode
   and in exported games.
5. A C++ API exists: `UIDocument` (load, reload, show/hide, sort order,
   `getElementById`) and a minimal `UIElement`.
6. `jasperengine-htmlui-tests` (googletest) builds from `engine/` with
   `-DJASPERENGINE_HTMLUI_TESTS=ON`, runs headless under Xvfb on glcore (llvmpipe) and vulkan
   (lavapipe) in the Linux Docker image via `tools/build.sh htmlui-tests`, and passes: unit
   tests plus golden-image tests that render through the real engine renderer, read the
   target back, and compare against `engine/tests/htmlui/golden/*.png`.
7. MCP end-to-end tests `tools/mcp/tests/test_html_ui_component.py` and
   `tools/mcp/tests/test_html_ui_basic.py` pass against the real editor.
8. `docs/html-ui/svg-benchmark.md` holds the LunaSVG / ThorVG / nanosvg benchmark results
   (native x86-64 and wasm) and the decision for phase 1.

## 2. Scope in / out

### In scope (phase 0)

- Vendoring RmlUi 6.3 Core + Debugger, CMake for all platforms, Xcode workspace, editor
  SDK copy, export paths, `FORK.md`.
- `engine/core/htmlui/`: `HtmlUI` (global init/shutdown), `RmlSystem`, `RmlFile`,
  `RmlRenderer` (required RenderInterface methods + `SetTransform`), `HtmlUIContext`
  (one RmlUi context per scene, its offscreen target and composite), `RmlFontEngine`.
- Render abstraction: configurable blend state; new `ShaderType::HTMLUI` (premultiplied
  alpha) with GLSL sources and every registration point.
- Text stack refactor (`FontFace`, `FontRegistry`, `TextShaper`, `GlyphAtlas`) with
  STBText keeping its behavior; `RmlFontEngine` on top.
- ECS: `UIDocumentComponent`, `UIDocumentSystem`, `UIDocument`, `UIElement`; registration;
  pointer down/up/move routing; editor/MCP minimum registration (Catalog, Stream,
  Factory, ProjectUtils, EntityCloner, ComponentAddDialog, minimal Inspector rows, MCP
  sentinels, `write_file` text extensions).
- Test infrastructure (googletest target, headless GL/Vulkan harness, readback, golden
  compare, Docker Vulkan driver, build/run scripts), unit tests, golden images, MCP tests.
- SVG benchmark (`tools/htmlui-bench/`) and decision document.
- Initial user guide `docs/mcp/guide/html-ui.md` describing what phase 0 supports.

### Deferred (not in phase 0)

| Phase | Deferred item |
|---|---|
| 1 | Stencil clip masks (`EnableClipMask`, `RenderToClipMask`: clipping to rounded corners and under transforms), depth-stencil + MSAA offscreen target, layers (`PushLayer`, `CompositeLayers`, `PopLayer`, `SaveLayerAsTexture`, `SaveLayerAsMaskImage`), filters (`CompileFilter`: blur, drop-shadow, color matrix, opacity groups), shaders (`CompileShader`/`RenderShader`: gradients), `mask-image`, `box-shadow` (needs layers). |
| 1 | Font effects (shadow, outline, glow, blur) through `PrepareFontEffects`; synthetic bold and italic; per-context raster scale (phase 0 has one global raster scale). |
| 1 | Complete input: wheel, key down/up to the focused document before the game, text input, enter/leave, multi-touch, focus, mouse cursor (`SetMouseCursor`), clipboard through `System` on all platforms, IME. |
| 1 | Editor polish: Inspector file picker and drag & drop, Hierarchy "Create > UI > UI Document" entry and icon, asset rename/move tracking (`Project::visitAssetPathsInRegistry`), hot reload through `McpAutoSync`, documents shown in the Scene view, export polish. |
| 1 | Lua API (`HtmlUIClassesLua.cpp`: `UIDocument`, `UIElement`, `UIEvent`), data binding, full C++ element API (events, `querySelector`, `classList`, `dataset`), Anime.js-style animation API. |
| 1 | SVG and Lottie through the library chosen by the benchmark (ThorVG for Lottie in any case); replacing nanosvg for document images. |
| 1 | Render-to-texture mode (`targetTexture`, world-space UI); compositing after post-process / fixed-resolution blit. |
| 2 | HTML5 parsing (lexbor) to RML, user-agent stylesheet, CSS-to-RCSS rewriting, diagnostics, importers (Google Fonts, LottieFiles, Lucide), `@font-face`, `currentColor` icons, web-component mappings. Phase 0 documents are XML-well-formed HTML using RCSS syntax. |
| 3 | RmlUi fork patches (`calc()`, grid, `::before/::after`, border styles, `%` radii, ...). |
| 4 | `jasperengine-editor check --ui`, MCP inspect/assert tools, generated support matrix, example library. |
| 5 | Effects library. |

## 3. Detailed design

### 3.1 Frame flow and ownership overview

```
Engine::systemDraw                                   engine/core/Engine.cpp:1131
  scenes[i]->update(dt)                              Engine.cpp:1214-1217
    ... UISystem::update
    UIDocumentSystem::update        <- NEW (registered right after UISystem, Scene.cpp:31)
        lazy HtmlUI::acquire + HtmlUIContext creation, context dimensions = canvas,
        load/unload/show/hide/z-index of documents, Rml::Context::Update()
    RenderSystem::update ...
  scenes[i]->draw()                                  Engine.cpp:1230-1232
    UIDocumentSystem::draw()        (empty: drawing is driven by RenderSystem)
    RenderSystem::draw                               RenderSystem.cpp:9405
      drawFramePasses()                              RenderSystem.cpp:9138
        ... shadow maps, 2D shadows, probe capture (9176)
        UIDocumentSystem::renderOffscreen(viewRect)  <- NEW, last line of drawFramePasses
            HtmlUIContext::render: Rml::Context::Render() records draws into RmlRenderer,
            then ONE offscreen render pass into the context's own target
            (never the swapchain: respects Vulkan's one-swapchain-pass rule)
      drawCameras -> renderCameraPass(main camera)   RenderSystem.cpp:9525
        ... sky, meshes, component UI, transparent renders (9944)
        UIDocumentSystem::drawComposite(pip, flip)   <- NEW, just before
        camera.render.endRenderPass()                RenderSystem.cpp:9957
            one textured quad inside the existing camera pass, premultiplied blend
  SystemRender::commit()
```

`RenderSystem::drawView` (editor views, `RenderSystem.cpp:10730`) calls `drawFramePasses()`
once per frame (`frameDrawn` guard, `:10748`) and then `renderCameraPass`. That alone does
not cover the editor's edit-mode Game view: the Scene view's `systemDraw` runs
`drawFramePasses()` first, with editor overlays on, so it skips the documents, and the Game
view's `drawView` then finds the frame drawn. So `drawView` also calls
`renderOffscreen(desc.viewRect)` for a view without editor overlays (3.5.5); the editor
commits a sokol frame per view (`Engine::drawView`), and `HtmlUIContext::render` runs at most
once per sokol frame (`SystemRender::getFrameCount`), so the stream buffers still take one
update a frame. Documents are not drawn when the pass draws editor overlays (Scene view);
that is phase 1.

Ownership of the new code (names used in section 4):

| Area | Files | Package |
|---|---|---|
| RmlUi library, CMake, FORK.md, skeletons, `HtmlUI`, `RmlSystem`, `RmlFile`, test harness | `engine/libs/rmlui/**`, `engine/core/htmlui/HtmlUI.*`, `RmlSystem.*`, `RmlFile.*`, `engine/tests/htmlui/{CMakeLists.txt,main.cpp,harness/**}` | p0-foundation |
| Renderer, shader, render abstraction | `RmlRenderer.*`, `HtmlUIContextRender.cpp`, `engine/shaderlib/htmlui.*`, render abstraction files | p0-render |
| Text | `engine/core/util/{FontFace,FontRegistry,TextShaper,GlyphAtlas,STBText}.*`, `RmlFontEngine.*` | p0-font |
| ECS, input, editor, MCP | `UIDocumentComponent.h`, `UIDocumentSystem.*`, `HtmlUIContext.{h,cpp}`, `UIDocument.*`, `UIElement.*`, registrations | p0-ecs |
| SVG benchmark | `tools/htmlui-bench/**`, `docs/html-ui/svg-benchmark.md` | p0-svg-bench |
| Full-path goldens, MCP visual test, guide, acceptance | `engine/tests/htmlui/TestGolden*.cpp`, `golden/**`, `tools/mcp/tests/test_html_ui_basic.py`, `docs/mcp/guide/html-ui.md` | p0-integrate |

Public engine headers (`UIDocumentComponent.h`, `UIDocumentSystem.h`, `UIDocument.h`,
`UIElement.h`) never include RmlUi headers (forward declarations only), so user C++
scripts compiled against the engine API snapshot (`editor/Generator.cpp:1504-1511`) need
no RmlUi include path and principle 3 (swappable core) holds. Only files under
`engine/core/htmlui/` include `<RmlUi/...>`.

### 3.2 RmlUi vendoring and build integration

#### 3.2.1 Vendored tree

Source: `/tmp/htmlui-src/rmlui-6.3` = https://github.com/mikke89/RmlUi tag `6.3`, commit
`ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (2026-08-22). Copy exactly:

```
engine/libs/rmlui/
  CMakeLists.txt                 (ours, see below)
  LICENSE.txt                    (upstream, MIT)
  Include/RmlUi/Core.h
  Include/RmlUi/Debugger.h
  Include/RmlUi/Config/**
  Include/RmlUi/Core/**          (includes Containers/robin_hood.h, itlib, their LICENSE.txt)
  Include/RmlUi/Debugger/**
  Source/Core/*.{cpp,h,inl}       (Pool.inl is included by Pool.h)
  Source/Core/Elements/*.{cpp,h}
  Source/Core/Layout/*.{cpp,h}
  Source/Debugger/*.{cpp,h}      (+ Source/Debugger/LICENSE.txt: embedded Courier Prime, OFL)
```

Not vendored: `Samples/`, `Tests/`, `Backends/`, `Utilities/`, `CMake/`, `Dependencies/`,
`Source/SVG`, `Source/Lottie`, `Source/Lua`, `Include/RmlUi/{Lua,Lua.h,SVG,Lottie}`,
`Source/Core/FontEngineDefault/` (FreeType), and every upstream `CMakeLists.txt` inside
`Source/`. `Core.cpp:29-31` includes the FreeType engine only under
`RMLUI_FONT_ENGINE_FREETYPE`, which we never define, so deleting the directory is safe.
The whole `engine/` tree ships with the editor (`CopyEditorEngine.cmake:17-21`) and the
engine API header snapshot (`editor/Project.cpp:4293-4365`), which is why nothing else
is vendored.

The vendoring is one commit with message `rmlui: vendor RmlUi 6.3 (Core, Debugger)`.

#### 3.2.2 `engine/libs/rmlui/CMakeLists.txt`

Follows `engine/libs/harfbuzz/CMakeLists.txt` and `engine/libs/meshoptimizer` (which also
globs). Must configure with CMake 3.22 (Android Studio).

```cmake
# RmlUi 6.3 - https://github.com/mikke89/RmlUi (tag 6.3, ba95ffe8bfb6370efb2cdcca927eaad4710c5413)
# Core and Debugger only. The font engine is the engine's own (engine/core/htmlui/RmlFontEngine),
# so there is no FreeType; see docs/html-ui/FORK.md for the vendored subset and patches.
file(GLOB RMLUI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/Elements/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/Layout/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Debugger/*.cpp
)
add_library(rmlui STATIC ${RMLUI_SOURCES})
target_include_directories(rmlui SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include")
# RMLUI_STATIC_LIB must be PUBLIC: without it Header.h makes RMLUICORE_API dllimport on
# Windows and visibility("default") elsewhere, which overrides -fvisibility=hidden
target_compile_definitions(rmlui
    PUBLIC RMLUI_STATIC_LIB
    PRIVATE RMLUI_VERSION="6.3" RMLUI_NO_FILE_INTERFACE_DEFAULT)
target_compile_features(rmlui PUBLIC cxx_std_17)
# Keep every Rml:: symbol inside libjasperengine when the editor builds it shared
if(NOT MSVC)
    target_compile_options(rmlui PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()
if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(rmlui PRIVATE -pthread)
endif()
```

`RMLUI_NO_FILE_INTERFACE_DEFAULT` removes the `fopen` default (`Core.cpp:101-108`), so a
missing `RmlFile` is an error instead of silently bypassing `ResourcePack` and Android
assets.

Hidden visibility alone does not keep RmlUi out of the editor's `libjasperengine.so`
(found when building): the std templates RmlUi instantiates over its own enums
(`std::vector<Rml::PropertyId>`, ...) keep default visibility, and engine classes that
derive from RmlUi interfaces export methods with `Rml::` parameter types. So:
- `engine/CMakeLists.txt`, shared engine only, not Apple/MSVC:
  `target_link_options(jasperengine PRIVATE "LINKER:--exclude-libs,librmlui.a")`;
- `engine/core/Export.h` defines `JASPERENGINE_INTERNAL` (`visibility("hidden")` on
  GCC/Clang outside Windows); `RmlSystem`, `RmlFile`, `RmlFontEngine`, `RmlRenderer` and
  `HtmlUIContext` are declared `class JASPERENGINE_INTERNAL ...` (internal: only the engine
  uses them; `HtmlUI` stays `JASPERENGINE_API`).
On macOS (ld64 has no `--exclude-libs`) the weak std instantiations stay visible in the
dylib.

#### 3.2.3 `engine/CMakeLists.txt` changes

| Where | Change |
|---|---|
| include dirs block (`:640-692`) | `include_directories(core/htmlui)` after `core/util` (`:662`). RmlUi headers come from the `rmlui` target's PUBLIC include dir. |
| `add_subdirectory` list (`:694-706`) | `add_subdirectory(libs/rmlui)` after `libs/meshoptimizer`. |
| warning suppression foreach (`:732-743`) | add `rmlui` to the list (gets `-w` / `/w`). |
| `JASPERENGINE_SRCS` (`:754-888`) | add every new `.cpp` of section 3 (list in 3.2.6). |
| `target_link_libraries(jasperengine PUBLIC ...)` (`:934-948`) | add `rmlui` to the PUBLIC list (like `harfbuzz`; static libs of a static engine must propagate to the game executable and the test target). |
| C++ standard block (`:983-988`) | `set_target_properties(rmlui PROPERTIES CXX_STANDARD ${JASPERENGINE_CXX_STANDARD})` next to tinygltf: `Rml::String`/`Rml::Vector` are std types crossing into engine code. |
| options (`:1-14`) and end of file | `option(JASPERENGINE_HTMLUI_TESTS "Build jasperengine-htmlui-tests" OFF)`. Honored only when `engine/` is the top-level project (`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`); otherwise `message(FATAL_ERROR ...)` if ON (the editor's shared engine hides RmlUi symbols, so tests must link the static engine). When ON: `add_definitions(-DJASPERENGINE_TEXTURE_READBACK)` before the engine target is defined, and `add_subdirectory(tests/htmlui)` at the very end of the file. |

No per-platform section needs a change: RmlUi is plain C++17 with no platform code
(`Include/RmlUi/Config/Config.h`, `Platform.h` already handle Win32, macOS, Emscripten,
Linux/Android). The editor root `CMakeLists.txt` needs nothing (it adds the engine with
`add_subdirectory` at `:446`, and editor code does not include RmlUi in phase 0).

`engine/renders/sokol/SokolTexture.cpp:16`, `:44`, `:54`: change the three guards
`#if defined(JASPERENGINE_EDITOR) && defined(SOKOL_VULKAN)` to
`#if (defined(JASPERENGINE_EDITOR) || defined(JASPERENGINE_TEXTURE_READBACK)) && defined(SOKOL_VULKAN)`
so the test build gets `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` on color attachments and
`getVulkanImageHandler()` for readback.

#### 3.2.4 Exports, editor SDK copy, Xcode

- Desktop, Web and Android exports build `engine/CMakeLists.txt` as the top-level project
  after `Exporter::copyEngine()` mirrors `core`, `libs`, `platform`, `renders`,
  `shaderlib`, `workspaces` (`editor/Exporter.cpp:2019-2025`); Android Gradle points at the
  same CMake (`engine/workspaces/androidstudio/app/build.gradle:45-46`); iOS/macOS via
  `engine/jasperengine.py:142-181`. RmlUi and `core/htmlui` therefore reach these exports
  with no exporter change. `engine/tests` is not in the copy list, so tests are never
  exported.
- Editor SDK copy (`CopyEditorEngine.cmake:17-21`) and the root `install(DIRECTORY engine ...)`
  (`CMakeLists.txt:938-945`): add an exclusion of `engine/tests` (a `REGEX` matching the
  path segment `engine/tests` exactly, so no vendored lib's `tests` dir is affected) so
  golden PNGs and fixtures are not installed with the editor.
- Xcode workspace `engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj`
  builds from its own file list (`editor/Exporter.cpp:2655`). Add, mirroring the
  `harfbuzz-ios` / `harfbuzz-macos` targets (`project.pbxproj:6648-6679`, link phases
  `:3215`, `:3267`, dependencies `:8307-8312`):
  - static-library targets `rmlui-ios` and `rmlui-macos` with every vendored `.cpp`,
    `GCC_PREPROCESSOR_DEFINITIONS = RMLUI_STATIC_LIB RMLUI_VERSION=\"6.3\" RMLUI_NO_FILE_INTERFACE_DEFAULT`,
    `GCC_SYMBOLS_PRIVATE_EXTERN = YES`, C++17;
  - `RMLUI_STATIC_LIB` in the app targets' definitions;
  - `"$(SRCROOT)/../../libs/rmlui/Include"` and `"$(SRCROOT)/../../core/htmlui"` in every
    `HEADER_SEARCH_PATHS` list that has `libs/harfbuzz` (`:8609`, `8642`, `8830`, `8870`,
    `8916`, `8959`, `9001`, `9037`);
  - every new engine `.cpp` of 3.2.6 in the app targets' sources (pattern: `UISystem.cpp`
    at `:887-888`, `:2886`, `:4498`, `:7543`).
  This cannot be built here; keep the edit mechanical and consistent with the harfbuzz
  entries, and generate fresh 24-hex-digit object IDs that do not collide.
- `tools/rebrand.py:61-67` covers `engine/libs/` generically; nothing to add.

#### 3.2.5 `docs/html-ui/FORK.md` (created in phase 0)

```markdown
# RmlUi fork

| | |
|---|---|
| Upstream | https://github.com/mikke89/RmlUi |
| Base | tag `6.3`, commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (2026-08-22) |
| Location | `engine/libs/rmlui/` |
| Build | static, hidden visibility, `RMLUI_STATIC_LIB` (public), `RMLUI_NO_FILE_INTERFACE_DEFAULT`, no FreeType |

## Vendored subset
(the list of 3.2.1)

## Removed from upstream
(the list of 3.2.1, with the reason for each)

## Patches
Every change to files under engine/libs/rmlui other than CMakeLists.txt is a separate commit
prefixed `rmlui:` and listed here.

| ID | Commit | Files | Reason | Test |
|---|---|---|---|---|
| (none in phase 0) | | | | |

## Updating upstream
1. Copy the new tag over the vendored subset (same include/exclude lists).
2. Re-apply each patch above in order; drop those merged upstream.
3. Rebuild, run jasperengine-htmlui-tests on glcore and vulkan, update this file.
```

#### 3.2.6 New engine source files (all added to `JASPERENGINE_SRCS` and the Xcode list by p0-foundation)

```
engine/core/htmlui/HtmlUI.cpp
engine/core/htmlui/RmlSystem.cpp
engine/core/htmlui/RmlFile.cpp
engine/core/htmlui/RmlRenderer.cpp
engine/core/htmlui/RmlFontEngine.cpp
engine/core/htmlui/HtmlUIContext.cpp
engine/core/htmlui/HtmlUIContextRender.cpp
engine/core/util/FontFace.cpp
engine/core/util/FontRegistry.cpp
engine/core/util/TextShaper.cpp
engine/core/util/GlyphAtlas.cpp
engine/core/subsystem/UIDocumentSystem.cpp
engine/core/object/ui/UIDocument.cpp
engine/core/object/ui/UIElement.cpp
```
Headers: the matching `.h` files plus `engine/core/component/UIDocumentComponent.h`.

### 3.3 Engine backend core: `HtmlUI`, `RmlSystem`, `RmlFile`

#### 3.3.1 `engine/core/htmlui/HtmlUI.{h,cpp}` (internal)

Global, reference-counted owner of RmlUi's process-wide state. Only `UIDocumentSystem`
and tests call it.

```cpp
namespace jasperengine {
    class HtmlUIContext;
    class RmlSystem; class RmlFile; class RmlFontEngine;

    class JASPERENGINE_API HtmlUI {
    public:
        // First call: Rml::SetSystemInterface/SetFileInterface/SetFontEngineInterface, then
        // Rml::Initialise(); subscribes to Engine::onViewDestroyed. Returns false (and logs)
        // if Initialise fails; the count is not taken then.
        static bool acquire();
        // Last call: Rml::Debugger::Shutdown() if initialised, Rml::Shutdown(),
        // unsubscribes. Every HtmlUIContext must be destroyed before.
        static void release();
        static bool isInitialized();
        static int getReferenceCount();

        // Public C++ API for fonts until @font-face (phase 2). Path through FileData
        // (project-relative, asset://, data://). family empty = read from the file.
        static bool loadFontFace(const std::string& path, bool fallback = false,
                                 const std::string& family = "", int weight = 0 /*0 = from file*/, bool italic = false);

        static RmlSystem& getSystemInterface();
        static RmlFile& getFileInterface();
        static RmlFontEngine& getFontEngine();

        // HtmlUIContext registers itself so view loss can drop GPU objects of all of them
        static void registerContext(HtmlUIContext* context);
        static void unregisterContext(HtmlUIContext* context);   // also detaches the debugger from it
        static size_t getContextCount();
        // the context the RmlUi debugger is attached to (nullptr = none)
        static void setDebuggerContext(HtmlUIContext* context, bool visible);
        static HtmlUIContext* getDebuggerContext();
    };
}
```

`onViewDestroyed` handler (fires before scenes are destroyed and before `sg_shutdown`,
`Engine.cpp:1316-1328`): for every registered context call
`HtmlUIContext::releaseGpuResources()` (3.5.6), then `Rml::ReleaseFontResources()` once.
RmlUi regenerates geometry and textures lazily once the view is back. Every `acquire()`
(not only the first) adds the subscription again (same tag, so it is never doubled): the
editor's play-mode stop calls `Engine::clearAllSubscriptions(true)` (`editor/Project.cpp:7666`),
which drops it while edit-mode scenes keep their contexts.

`setDebuggerContext(context, visible)`: a context without an `Rml::Context` (not created)
or `nullptr` shuts the debugger down; a different context than the current host shuts it
down and initialises it again there (`Rml::Debugger::Initialise(host)` both hosts and
inspects that context); then `Rml::Debugger::SetVisible(visible)`.

The interface objects (`RmlSystem`, `RmlFile`, `RmlFontEngine`) are function-local statics
in `HtmlUI.cpp`, so they outlive `Rml::Shutdown()`.

#### 3.3.2 `engine/core/htmlui/RmlSystem.{h,cpp}`

```cpp
class JASPERENGINE_INTERNAL RmlSystem : public Rml::SystemInterface {
public:
    double GetElapsedTime() override;                 // Engine::getSystemTime() (seconds)
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override;
    void SetMouseCursor(const Rml::String& cursor_name) override {}   // phase 1
    void SetClipboardText(const Rml::String& text) override;           // in-process string (phase 1: System clipboard)
    void GetClipboardText(Rml::String& text) override;

    // tests: every message also goes here; counters reset by resetCounters()
    void setLogListener(std::function<void(Rml::Log::Type, const std::string&)> listener);
    size_t getErrorCount() const;    // LT_ERROR + LT_ASSERT
    size_t getWarningCount() const;  // LT_WARNING
    void resetCounters();

    // JoinPath without the RmlUi types (JoinPath calls it)
    static std::string joinPath(const std::string& documentPath, const std::string& path);
};
```

- `LogMessage`: prefix `"[HTML UI] "`; `LT_ERROR`, `LT_ASSERT` -> `Log::error`;
  `LT_WARNING` -> `Log::warn`; `LT_INFO`, `LT_ALWAYS` -> `Log::verbose`; `LT_DEBUG` ->
  `Log::debug`. Returns `true` (never break into a debugger).
- `JoinPath`: RmlUi passes document paths with `:` replaced by `|`
  (`StreamFile::Open`); restore with `Rml::StringUtilities::Replace(document_path, '|', ':')`.
  If `path` contains `"://"` or starts with `/` or has a drive letter (`X:`), return
  `path`. Otherwise return `scheme + simplify(baseDir(document_path without scheme) + path)`
  where `scheme` is the document's `asset://`/`data://` prefix (empty for a
  project-relative path) and `simplify` resolves `.` and `..` like
  `FileData::simplifyPath` (`engine/core/io/FileData.cpp:73-112`) but always with `/`.

#### 3.3.3 `engine/core/htmlui/RmlFile.{h,cpp}`

```cpp
class JASPERENGINE_INTERNAL RmlFile : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override;  // new Data; Data::open(path) != FILEDATA_OK -> delete, return 0
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;   // SEEK_SET/CUR/END -> Data::seek(absolute), false if out of range
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;
    bool LoadFile(const Rml::String& path, Rml::String& out_data) override;  // one Data::open + copy
};
```

The handle is a small struct holding the `Data` and the read position: `Data::seek`
clamps to `length - 1` (`Data.cpp:83-90`), so it cannot stand at the end of the file the
way `fseek` can; `Read`/`Seek`/`Tell` work on that position over `Data::getMemPtr()`.
An empty path returns 0 without opening anything (it would resolve to the asset
directory itself).

`Data::open(const char*)` (`engine/core/io/Data.cpp:124-155`) already resolves
`ResourcePack::readOwned`, then `File::open` -> `FileData::getSystemPath`
(`FileData.cpp:113-142`: `data://`, `asset://`, `lua://`, `shader://`, relative = asset
path) -> `System::platformFopen` (Android `AAssetManager`, Web preloaded FS). No platform
code in `RmlFile`.

### 3.4 Render abstraction changes and the `HTMLUI` shader type

#### 3.4.1 Configurable blend state

`engine/core/render/Render.h` (append after `PipelineType`, `:235-246`):

```cpp
enum class BlendFactor {
    ZERO, ONE,
    SRC_COLOR, ONE_MINUS_SRC_COLOR, SRC_ALPHA, ONE_MINUS_SRC_ALPHA,
    DST_COLOR, ONE_MINUS_DST_COLOR, DST_ALPHA, ONE_MINUS_DST_ALPHA
};

enum class BlendOperation { ADD, SUBTRACT, REVERSE_SUBTRACT };

// Color blending of a render object's color pipelines (PIP_DEFAULT, PIP_DEFAULT_NODEPTH,
// PIP_RTT, PIP_RTT_NODEPTH, PIP_RTT_INVERT). Without one, those pipelines keep their
// built-in straight-alpha blending (SokolObject::makePipelineDesc).
struct BlendState {
    bool enabled = true;
    BlendFactor srcFactorRGB = BlendFactor::SRC_ALPHA;
    BlendFactor dstFactorRGB = BlendFactor::ONE_MINUS_SRC_ALPHA;
    BlendOperation opRGB = BlendOperation::ADD;
    BlendFactor srcFactorAlpha = BlendFactor::ONE;
    BlendFactor dstFactorAlpha = BlendFactor::ONE_MINUS_SRC_ALPHA;
    BlendOperation opAlpha = BlendOperation::ADD;

    static BlendState premultipliedAlpha() {   // ONE, ONE_MINUS_SRC_ALPHA on color and alpha
        BlendState s; s.srcFactorRGB = BlendFactor::ONE; return s;
    }
    static BlendState opaque() { BlendState s; s.enabled = false; return s; }
};
```

`engine/core/render/ObjectRender.{h,cpp}`: add
`void setBlendState(const BlendState& state);` (call between `beginLoad` and `endLoad`;
forwards to the backend).

`engine/renders/sokol/SokolObject.{h,cpp}`:
- members `bool blendOverride = false; BlendState blendState;` (initialised in the
  constructor at `:33-54`, reset in `beginLoad` at `:139-155`, kept by the defaulted
  copy);
- `void setBlendState(const BlendState&)` sets both;
- private helpers `sg_blend_factor getBlendFactor(BlendFactor)`, `sg_blend_op getBlendOp(BlendOperation)`;
- in `makePipelineDesc` (`:265-395`), after the `switch`, for the five color pipeline
  types: if `blendOverride`, overwrite `desc.colors[0].blend` (`enabled`,
  `src_factor_rgb`, `dst_factor_rgb`, `op_rgb`, `src_factor_alpha`, `dst_factor_alpha`,
  `op_alpha`). The write mask stays as each pipeline type sets it (`SG_COLORMASK_RGB` for
  `PIP_DEFAULT*` at `:351`, `SG_COLORMASK_RGBA` for `PIP_RTT*` at `:372`).
  Depth, shadow, G-buffer and z-prepass pipelines ignore the override.

Nothing else changes for existing objects: without `setBlendState` the generated
`sg_pipeline_desc` is byte-identical to today's (unit test 5.1, `BlendStateTest.DefaultUnchanged`
compares `makePipelineDesc` output through a test-only accessor, see 3.4.4).

Phase 1 adds stencil state the same way (`StencilState`, `setStencilState`).

#### 3.4.2 `ShaderType::HTMLUI`

Name: `HTMLUI` (premultiplied-alpha UI). Not RmlUi-specific in name, so a different layout
engine can reuse it.

| Where | Change |
|---|---|
| `engine/core/render/Render.h:57` | append `HTMLUI` after `POSTPROCESS` (the enum is stored in `.sdat` keys; never insert earlier). |
| `engine/shaderlib/htmlui.vert`, `htmlui.frag` | new, picked up by the embed glob (`engine/CMakeLists.txt:956-960`). |
| `engine/core/shader/ShaderVariantCompiler.cpp:367-421` | in `setupShaderArgs`: `else if (shaderType == ShaderType::HTMLUI){ args.vert_file = "htmlui.vert"; args.frag_file = "htmlui.frag"; addHtmlUIPropertyDefinitions(args.defines, properties); }` and the new function next to `addUIPropertyDefinitions` (`:347-352`): bit 0 -> `HAS_TEXTURE`. |
| `engine/core/pool/ShaderPool.cpp:53-116` | `parseShaderTypeToken`: `"htmlui"` -> `ShaderType::HTMLUI`. |
| `ShaderPool.cpp:389-407` | `getShaderTypeName`: `"htmlui"` / `"HTML UI"` (also the `.sdat` basename). |
| `ShaderPool.cpp:410-428` | `getShaderPropertyCount`: `HTMLUI` -> 1. |
| `ShaderPool.cpp:430-508` | `getShaderPropertyName`: `HTMLUI` bit 0 -> `"Tex"` / `"HAS_TEXTURE"` (follow the UI case). |
| `engine/core/pool/ShaderPool.{h,cpp}` (`:162`, `~1000`) | `static uint32_t getHtmlUIProperties(bool texture);` |
| `engine/core/script/binding/CoreClassesLua.cpp:455-467` | `.addVariable("HTMLUI", ShaderType::HTMLUI)` in the ShaderType enum table. |
| `editor/window/dialog/ExportWindow.cpp:1236-1242` | add `ShaderType::HTMLUI` to `typeValues` (comment says keep in sync). |
| `editor/shader/ShaderVariantCollector.cpp:~490-498` | for every entity with a `UIDocumentComponent`: `insertKeys(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(false), 0)` and `(true)`. |
| `editor/Project.cpp:~2838-2843` | same two keys when the registry has a `UIDocumentComponent` (export variant listing). |

Not changed: `editor/shader/ShaderBuilder.cpp` (calls `ShaderVariantCompiler::compile`,
`:286`), `UniformBlockType` / `TextureShaderType` enums (lookups by name, 3.4.3),
shader fork lists (`ProjectUtils.cpp:136-147`, `ShaderForkDialog.cpp:25-35`; the HTML UI
shader is not user-forkable in phase 0), `engine/shaders/*.h`.

#### 3.4.3 GLSL sources

Conventions (from `ui.vert`/`ui.frag` and `ShaderVariantCompiler`): GLSL 450, no
`layout(location/binding)`, attributes found by name (`ShaderData::getAttrIndex`,
`ShaderData.cpp:47-142`: `POSITION` -> `a_position`, `COLOR` -> `a_color`, `TEXCOORD1` ->
`a_texcoord1`), uniform blocks `u_vs_*`, separate `texture2D` + `sampler`, all-float
uniform blocks (flattened to a vec4 array on GL, `spirvcross.cc:138-166`).

`engine/shaderlib/htmlui.vert`:
```glsl
#version 450

// RmlUi geometry (and the document composite): positions in document pixels mapped by
// `transform` (projection * element transform), premultiplied sRGB colors passed through.
uniform u_vs_htmluiParams {
    mat4 transform;
    vec4 translation;   // xy: RenderGeometry translation, zw unused
} htmluiParams;

in vec2 a_position;
in vec4 a_color;
#ifdef HAS_TEXTURE
    in vec2 a_texcoord1;
#endif

// varyings in the same order in both stages: their locations follow declaration order
out vec4 v_color;
#ifdef HAS_TEXTURE
    out vec2 v_uv1;
#endif

void main() {
    v_color = a_color;
    #ifdef HAS_TEXTURE
        v_uv1 = a_texcoord1;
    #endif
    gl_Position = htmluiParams.transform * vec4(a_position + htmluiParams.translation.xy, 0.0, 1.0);
    // no depth test: keep z inside every backend's clip range (GL [-w,w], D3D/Metal/Vulkan [0,w])
    gl_Position.z = 0.5 * gl_Position.w;
}
```

`engine/shaderlib/htmlui.frag`:
```glsl
#version 450

// No sRGB conversion: RmlUi colors and textures are premultiplied sRGB and the engine's
// color targets store sRGB-encoded values, so blending happens in sRGB space like a browser.
in vec4 v_color;
#ifdef HAS_TEXTURE
    in vec2 v_uv1;
    uniform texture2D u_htmluiTexture;
    uniform sampler u_htmlui_smp;
#endif
out vec4 g_finalColor;

void main() {
    vec4 color = v_color;
    #ifdef HAS_TEXTURE
        color *= texture(sampler2D(u_htmluiTexture, u_htmlui_smp), v_uv1);
    #endif
    g_finalColor = color;
}
```

Lookups: `shaderData.getUniformBlockIndexByName("u_vs_htmluiParams")`
(`ShaderData.cpp:231`), `shaderData.getTextureIndexByName("u_htmluiTexture")`
(`ShaderData.cpp:356`). Uniform data: `struct { float transform[16]; float translation[4]; }`
= 80 bytes, column-major (`Rml::Matrix4f` default, `RMLUI_MATRIX_ROW_MAJOR` off).

Vertex layout (one interleaved buffer, `static_assert(sizeof(Rml::Vertex) == 20)`):

| Attribute | `AttributeType` | elements | `AttributeDataType` | normalized | offset |
|---|---|---|---|---|---|
| `a_position` | `POSITION` | 2 | `FLOAT` | no | 0 |
| `a_color` | `COLOR` | 4 | `UNSIGNED_BYTE` | yes (`UBYTE4N`, `SokolObject.cpp:79-81`) | 8 |
| `a_texcoord1` | `TEXCOORD1` | 2 | `FLOAT` | no | 12 |

Indices: `int` from RmlUi -> `AttributeDataType::UNSIGNED_INT` (uint32 index buffers are
core in GLES3/WebGL2).

Blend: `BlendState::premultipliedAlpha()` on every HTML UI pipeline.

Varyings: the stages declare them in the same order (`v_color`, then `v_uv1`). The sources
have no `layout(location)`; glslang numbers varyings in declaration order, so a different
order per stage links `v_uv1` to `v_color` on GL (found by p0-render: the textured variant
failed to link).

#### 3.4.4 Test accessor

To unit-test 3.4.1 without a GPU, add to `SokolObject` (public, documented as test/debug
use): `sg_pipeline_desc describePipeline(PipelineType pipType) const;` returning
`makePipelineDesc(pipType)`; `makePipelineDesc` becomes `const`.

### 3.5 `RmlRenderer`, the per-context offscreen target and the composite

#### 3.5.1 Contexts and targets

- One `Rml::Context` per scene that has at least one `UIDocumentComponent` (DESIGN), owned
  by that scene's `UIDocumentSystem` through an `HtmlUIContext`. All the scene's
  screen-space documents live in it; `sortOrder` becomes the document's numeric `z-index`.
- One offscreen color target per context (not per document, see 3.10 D2), sized to the
  pixel size of the view the canvas is shown in, `RGBA8` color + `DEPTH` (the existing
  `FramebufferRender::createFramebuffer(TEXTURE_2D, w, h, LINEAR, LINEAR, CLAMP_TO_EDGE,
  CLAMP_TO_EDGE, false)`, `SokolTexture.cpp:479`; depth is unused in phase 0). Phase 1
  switches it to depth-stencil + MSAA for clip masks and layers.
- One `RmlRenderer` per `HtmlUIContext`, passed to `Rml::CreateContext(name, dims, renderer)`
  (`Include/RmlUi/Core/Core.h:82`). `Rml::SetRenderInterface` is never called. Each
  renderer owns its pipelines, its streaming buffers and its textures, so contexts do not
  share per-frame state and the "one buffer update per frame" sokol rule
  (`VALIDATE_UPDATEBUF_ONCE`, comments at `RenderSystem.cpp:8237`, `:9888`) holds per
  context.

#### 3.5.2 `engine/core/htmlui/RmlRenderer.{h,cpp}`

```cpp
namespace jasperengine {
    class JASPERENGINE_INTERNAL RmlRenderer : public Rml::RenderInterface {
    public:
        RmlRenderer();
        ~RmlRenderer() override;                    // releaseGpuResources()

        // Rml::RenderInterface, required set + SetTransform
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;
        void SetTransform(const Rml::Matrix4f* transform) override;

        // engine side, called by HtmlUIContext (HtmlUIContextRender.cpp)
        // Pipelines exist once both HTMLUI variants are compiled; false while they compile
        // (ShaderPool::get may hand back a shader that is not created yet).
        bool loadPipelines();
        bool isReady() const;
        // contextSize in document px, targetSize in target pixels
        void beginFrame(Rml::Vector2i contextSize, Rml::Vector2i targetSize);
        // uploads this frame's vertices/indices, runs one offscreen pass into target and
        // replays the recorded draws; then destroys what was released during the frame
        bool endFrame(FramebufferRender& target, CameraRender& pass);
        // the document target as a quad inside the current (camera) pass
        bool drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY);
        // view lost / context destroyed: every GPU object, handles stay valid as "dead"
        void releaseGpuResources();

        // pure function, unit-tested: RmlUi scissor (document px, top-left origin) to a
        // CameraRender::applyScissor rect (target px, bottom-left origin)
        static Rect toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize,
                                    Rml::Vector2i targetSize, bool openGLFlipped);

        // beginFrame ran in that frame (SystemRender::getFrameCount); HtmlUIContext::render
        // renders at most once per sokol frame
        bool hasFrame(uint64_t frame) const;

        // test statistics
        size_t getLiveGeometryCount() const;
        size_t getLiveTextureCount() const;
        size_t getLastFrameDrawCount() const;
        size_t getVertexCapacity() const;
        TextureRender* getTextureRender(Rml::TextureHandle texture) const;   // nullptr when unknown or dead
        // sokol ids of every buffer and image the renderer owns (leak tests)
        void getGpuHandles(std::vector<uint32_t>& buffers, std::vector<uint32_t>& images) const;
    };
}
```

`SystemRender::getFrameCount()` (new, render abstraction): sokol frames committed so far,
incremented by `SystemRender::commit()`.

Internal types (in the `.cpp`):

```cpp
struct RmlGeometry { Rml::Span<const Rml::Vertex> vertices; Rml::Span<const int> indices; };
struct RmlTexture  { TextureRender render; int width = 0; int height = 0; bool dead = false; };
struct DrawCommand {
    uint32_t firstIndex; uint32_t indexCount;
    RmlTexture* texture;          // nullptr = untextured variant
    Rml::Vector2f translation;
    uint32_t transformIndex;      // into the frame's matrix list (0 = identity)
    bool scissorEnabled; Rect scissor;  // already in target pixels, bottom-left origin
};
```

Behavior:

- `CompileGeometry`: `new RmlGeometry{vertices, indices}` (RmlUi guarantees the spans stay
  valid and immutable until `ReleaseGeometry`, `RenderInterface.h` lifetime note). No GPU
  work. Returns the pointer as handle; keeps a live set.
- `RenderGeometry`: only between `beginFrame` and `endFrame` (outside: ignore and
  `Log::error` once per renderer). Appends the geometry's vertices to the frame vertex
  vector and its indices rebased by the frame's current vertex count (no base-vertex
  draw: GLES3.0/WebGL2 lack it, `sokol_gfx.h:238`). Records a `DrawCommand` with the
  current transform and scissor. Empty geometry is skipped.
- `ReleaseGeometry`: erase from live set, `delete`. Data already copied into the frame
  vectors stays valid for the replay.
- `LoadTexture`: `TextureData data; data.loadTextureFromFile(source.c_str())`
  (`TextureData.h:54`; stb_image for PNG/JPG/..., nanosvg for `.svg`). On failure:
  `Log::error("[HTML UI] could not load image '%s'", source)`, return 0. Convert to RGBA8
  (1 channel -> gray, 2 -> gray+alpha, 3 -> alpha 255, `RED16` -> error, return 0),
  premultiply every pixel on the CPU (`c = (c * a + 127) / 255`), no alpha-border fix (the
  engine's `TextureAlphaBorder` path is never used: we create the `TextureRender`
  directly), then `TextureRender::createTexture("htmlui-image", w, h, ColorFormat::RGBA,
  TEXTURE_2D, 1, data, size, LINEAR, LINEAR, CLAMP_TO_EDGE, CLAMP_TO_EDGE)`
  (`TextureRender.h:485-488`; `LINEAR` min filter = no mipmaps,
  `SokolTexture.cpp:303-311`). Sets `texture_dimensions`. Row 0 of `TextureData` is the
  top of the image, matching RmlUi's top-left UV origin (verified by golden `images`).
- `GenerateTexture`: data is already premultiplied RGBA8 (font atlas pages, RmlUi
  callback textures); create the same way.
- `ReleaseTexture`: during a frame (between `beginFrame` and the end of `endFrame`) push
  to a pending list destroyed at the end of `endFrame` (recorded draws may still reference
  it, see 3.6.5 for why this happens with font pages); outside a frame destroy now. Dead
  textures (after `releaseGpuResources`) are just freed.
- `EnableScissorRegion` / `SetScissorRegion`: store; converted with `toTargetScissor`
  at record time.
- `SetTransform(nullptr)`: identity; otherwise push `projection * (*transform)` into the
  frame's matrix list and use its index for following draws.
- `toTargetScissor`: `sx = target.x / context.x`, `sy = target.y / context.y`;
  `x0 = floor(left*sx)`, `x1 = ceil(right*sx)`, `y0 = floor(top*sy)`, `y1 = ceil(bottom*sy)`,
  clamped to the target; `y = openGLFlipped ? y0 : target.y - y1`; returns
  `Rect(x0, y, x1-x0, y1-y0)`. `openGLFlipped` is `Engine::isOpenGL()` (offscreen GL
  rendering is Y-flipped, see projection below). A rect with zero width or height marks
  the command as clipped away: the replay skips it (sokol widens empty scissors to 1x1 on
  Metal/Vulkan/WebGPU, see the comment at `RenderSystem.cpp:9855-9857`).
- Projection: `P = Rml::Matrix4f::ProjectOrtho(0, contextW, contextH, 0, -10000, 10000)`;
  if `Engine::isOpenGL()` then `P = Rml::Matrix4f::Scale(1, -1, 1) * P`, so the target's
  memory row 0 is the top of the document on every backend (the engine's rule for
  offscreen textures, `RenderSystem::isRenderingFlipped`, `RenderSystem.cpp:7764-7776`).
  Document pixels map to target pixels by the ratio `target/context` (this is the
  resolution scaling, 3.5.4).
- `loadPipelines`: `ShaderPool::get(ShaderType::HTMLUI, getHtmlUIProperties(false))` and
  `(true)`; if either `!isCreated()` return false (retry next frame; build failure is
  logged by ShaderPool). Creates (once, recreated when buffers grow):
  - two STREAM buffers via `BufferRender::createBuffer(size, nullptr, VERTEX_BUFFER, BufferUsage::STREAM)`
    and `(..., INDEX_BUFFER, ...)`;
    initial capacity 16384 vertices / 49152 indices, doubled until the frame fits;
  - `ObjectRender colorObject, texturedObject`: `beginLoad(TRIANGLES)`, `setShader`,
    three `addAttribute(shaderData.getAttrIndex(...), &vertexBuffer, ..., stride 20, offset, normalized, false)`
    (an attribute the variant strips returns slot -1 and is skipped),
    `setIndex(&indexBuffer, UNSIGNED_INT, 0)`, for the textured one
    `addTexture(getTextureIndexByName("u_htmluiTexture"), FRAGMENT, &placeholderWhite)`,
    `setBlendState(BlendState::premultipliedAlpha())`,
    `endLoad(PIP_RTT_NODEPTH, false, false, CullingMode::BACK, WindingOrder::CCW)`.
    `PIP_RTT_NODEPTH` matches the target exactly: 1 sample, `RGBA8`, `DEPTH`, depth
    compare always, no depth write, RGBA write mask (`SokolObject.cpp:367-395`);
  - `ObjectRender compositeObject` (textured variant) over a 4-vertex/6-index IMMUTABLE
    mesh: NDC corners (-1,1) (1,1) (1,-1) (-1,-1), UV (0,0) (1,0) (1,1) (0,1), colour
    white; `endLoad(PIP_DEFAULT_NODEPTH | PIP_RTT_NODEPTH, ...)`, premultiplied blend.
- `endFrame`: if `!isReady()` (and `loadPipelines()` still fails) drop the frame, flush
  pending releases, return false. Otherwise grow buffers if needed, `updateBuffer` both
  (once per frame), `pass.setClearColor({0,0,0,0})`, `pass.startRenderPass(&target)`,
  `pass.applyViewport(Rect(0,0,tw,th))`, `pass.applyScissor(Rect(0,0,tw,th))`, then for
  each command: apply its scissor when it changed (full target when disabled), choose
  `colorObject` or `texturedObject`, `beginDraw(PIP_RTT_NODEPTH)` when the object changes,
  for textured `addTexture(slot, FRAGMENT, &texture->render)` (bindings are mutable
  after `endLoad`, as `RenderSystem::drawUI` does at `RenderSystem.cpp:5664-5672`),
  `applyUniformBlock(slot, 80, &uniforms)`, `draw(firstIndex, indexCount, 1)`.
  `pass.endRenderPass()`, destroy pending releases, keep `lastFrameDrawCount`.
- `drawComposite`: `beginDraw(compositePipeline)`; `addTexture(source)`; uniforms
  `transform = flipY ? Scale(1,-1,1) : identity`, translation 0; `draw(0, 6, 1)`.

#### 3.5.3 `engine/core/htmlui/HtmlUIContext.h` (render part in `HtmlUIContextRender.cpp`)

```cpp
namespace jasperengine {
    class RmlRenderer;

    class JASPERENGINE_INTERNAL HtmlUIContext {
    public:
        explicit HtmlUIContext(const std::string& name);   // e.g. "scene-0x55d0c3a0"
        ~HtmlUIContext();                                    // destroy()

        // --- HtmlUIContext.cpp (p0-ecs) ---
        bool create(Rml::Vector2i dimensions);   // HtmlUI::registerContext, Rml::CreateContext(name, dims, renderer)
        void destroy();                            // see 3.5.6
        Rml::Context* getRmlContext() const;
        void setDimensions(Rml::Vector2i dimensions);   // Context::SetDimensions when changed
        Rml::Vector2i getDimensions() const;
        void setDensityRatio(float ratio);              // Context::SetDensityIndependentPixelRatio when changed
        Rml::ElementDocument* loadDocument(const std::string& path);  // nullptr + Log::error on failure
        void unloadDocument(Rml::ElementDocument* document);
        void update();                                  // Context::Update
        bool processMouseMove(Rml::Vector2f position, int modifiers);
        bool processMouseButtonDown(int button, int modifiers);
        bool processMouseButtonUp(int button, int modifiers);
        bool processMouseLeave();
        // true when the hovered/active element is inside a document body (3.7.5)
        bool isPointerOverContent() const;
        void setDebuggerVisible(bool visible);

        // --- HtmlUIContextRender.cpp (p0-render) ---
        // Offscreen: pixel ratio from viewRect, target (re)creation, font raster scale,
        // renderer->beginFrame, Context::Render, renderer->endFrame. False when not drawn.
        bool render(const Rect& viewRect);
        // inside the camera pass; nothing when the last render() did not draw
        void composite(PipelineType colorPipeline, bool flipY);
        void releaseGpuResources();      // Rml::ReleaseTextures(renderer) + renderer->releaseGpuResources() + target
        bool hasRenderedFrame() const;   // at least one successful render() since (re)creation
        float getPixelRatio() const;
        FramebufferRender* getTarget();  // tests (readback of the document layer)
        RmlRenderer& getRenderer();

    private:
        std::string name;
        Rml::Context* context = nullptr;
        std::unique_ptr<RmlRenderer> renderer;
        FramebufferRender target;
        CameraRender pass;
        Rml::Vector2i dimensions{0, 0};
        Rml::Vector2i targetSize{0, 0};
        float pixelRatio = 1.0f;
        float densityRatio = 1.0f;
        bool renderedThisFrame = false;
        bool renderedOnce = false;
    };
}
```

`render(viewRect)` (HtmlUIContextRender.cpp):
1. `targetSize = (round(viewRect.w), round(viewRect.h))`; if any is 0 or the context has
   no visible document: `renderedThisFrame = false`, return false.
2. `pixelRatio = clamp(viewRect.w / dimensions.x, 0.25, 8)`; font raster scale
   `HtmlUI::getFontEngine().setRasterScale(clamp(round(pixelRatio * 4) / 4, 1, 4))`
   (3.6.4; a change bumps the font version and RmlUi regenerates text).
3. (Re)create `target` when the size changed (`destroyFramebuffer` then `createFramebuffer`).
4. `renderer->beginFrame(dimensions, targetSize)`; `context->Render()`;
   `renderedThisFrame = renderer->endFrame(target, pass)`; set `renderedOnce` when true.
5. Return `renderedThisFrame`.

Step 0: if the renderer already began a frame in this sokol frame
(`renderer->hasFrame(SystemRender::getFrameCount())`), return the previous result; an
editor view after the frame's own passes (3.1) must not update the stream buffers twice.

`releaseGpuResources()`: `Rml::ReleaseTextures(renderer)` first (while RmlUi is initialised),
so RmlUi drops its file and callback texture handles and loads them again when it draws
next (compiled geometry is CPU-only and survives), then `renderer->releaseGpuResources()`
and the target.

`composite(colorPipeline, flipY)`: if `!renderedThisFrame` return. Map
`PIP_DEFAULT`/`PIP_DEFAULT_NODEPTH` -> `PIP_DEFAULT_NODEPTH`, every `PIP_RTT*` ->
`PIP_RTT_NODEPTH`; `renderer->drawComposite(target.getColorTexture(), mapped, flipY)`.
The composite quad fills the pass viewport, which `renderCameraPass` set to the view rect
(`RenderSystem.cpp:9650-9711`, `colorPassViewport = viewRect` at `:9702`), so letterbox/crop offsets are handled by the viewport,
not by the quad.

#### 3.5.4 Resolution, `dp`, sRGB and premultiplication

- Layout units: document `px` = canvas pixels; context dimensions = canvas size (or the
  editor's reference size, 3.7.3). Media queries see the canvas size.
- Target resolution = the screen pixels the canvas covers (`viewRect`); vector geometry is
  rasterized at screen resolution through the projection, and glyphs at
  `font-size * rasterScale` (3.6.4), so text stays crisp on scaled canvases.
- `dp`: `Context::SetDensityIndependentPixelRatio(pixelRatio)` (DESIGN: dp ratio from the
  canvas-to-screen scale). Applied by `UIDocumentSystem::update` from the last
  `getPixelRatio()`.
- Colors: RmlUi gives premultiplied sRGB vertex colors and premultiplied textures; the
  engine's color targets are non-sRGB RGBA8 holding sRGB-encoded values (the existing
  `ui.frag` encodes with `linearTosRGB`). The HTML UI shader does no conversion, the
  offscreen pass and the composite blend `ONE, ONE_MINUS_SRC_ALPHA`, which is browser
  behavior. Writes: RGB only on the swapchain (`PIP_DEFAULT*`), RGBA on offscreen targets
  (`PIP_RTT*`), so an editor/RTT target gets correct premultiplied "over" alpha.

#### 3.5.5 RenderSystem hooks (exact)

`engine/core/subsystem/RenderSystem.cpp`:
- include `"subsystem/UIDocumentSystem.h"` with the other includes.
- End of `drawFramePasses()`, after `renderReflectionProbeCapture();` (`:9176`):
  ```cpp
  // HTML UI documents draw into their own offscreen target before any camera pass, so
  // the composite below never needs a second swapchain pass (Vulkan allows one a frame)
  bool renderDocuments = true;
  #ifdef JASPERENGINE_EDITOR
  // a frame drawn for the Scene view (editor overlays on) does not show documents yet (phase 1)
  renderDocuments = rv().secondary || !drawEditorOverlays;
  #endif
  if (renderDocuments)
      scene->getSystem<UIDocumentSystem>()->renderOffscreen(targetViewRect());
  ```
  `targetViewRect()` (`:7563`) is the engine view rect (or the view's in the editor).
- In `renderCameraPass`, right before `camera.render.endRenderPass();` (`:9957`), after
  the transparent renders (`:9944-9955`):
  ```cpp
  //---------Draw HTML UI documents (in front of everything, including component UI)----------
  bool drawDocuments = isMainCamera && !pass.renderToTexture;
  #ifdef JASPERENGINE_EDITOR
  drawDocuments = drawDocuments && !passDrawsOverlays;
  #endif
  if (drawDocuments){
      camera.render.applyScissor(colorPassViewport);
      scene->getSystem<UIDocumentSystem>()->drawComposite(colorPip, isRenderingFlipped(camera));
  }
  ```
  `colorPip` is the pass's pipeline (`:9638`), `isRenderingFlipped` at `:7764`.
- In `drawView`, before the `if (!frameDrawn)` block (found by p0-render, see 3.1):
  ```cpp
  if (!desc.editorOverlays)
      scene->getSystem<UIDocumentSystem>()->renderOffscreen(desc.viewRect);
  ```

Vulkan: the offscreen pass in `drawFramePasses` targets the context's own framebuffer;
the composite is a draw inside the camera's existing pass. Stacked scenes on Vulkan
already render into `Engine::compositeFramebuffer` (`Engine.cpp:778-798`), which takes the
`PIP_RTT` path. No new swapchain pass is ever opened.

Effects: in a camera pass redirected for SSR, post-process or fixed resolution, the
composite lands in that offscreen color buffer like the component UI does, so documents
are post-processed and, with fixed resolution, scaled with the scene. Compositing after
those passes is phase 1.

#### 3.5.6 Lifetime of GPU objects

`HtmlUIContext::destroy()`:
1. `context->UnloadAllDocuments()`; if the RmlUi debugger is attached here,
   `HtmlUI::setDebuggerContext(nullptr, false)` (calls `Rml::Debugger::Shutdown()`).
2. `Rml::RemoveContext(name)` (geometry released through our `ReleaseGeometry`).
3. `Rml::ReleaseTextures(renderer.get())` (file textures, `Core.cpp:386-395`).
4. `Rml::ReleaseRenderManagers()` (`Core.cpp:438-455`): releases font resources (font page
   textures of every render manager) and erases this renderer's render manager. This
   order is required: a `CallbackTextureSource` keyed by a destroyed render manager would
   release through a dangling pointer.
5. `renderer->releaseGpuResources()`; `target.destroyFramebuffer()`;
   `HtmlUI::unregisterContext(this)`.

After destroy, `getLiveGeometryCount() == 0` and `getLiveTextureCount() == 0`; tests assert
it (5.1 `RmlRendererTest.NoLeaksAfterContextDestroy`).

### 3.6 Text: STBText refactor and `RmlFontEngine`

#### 3.6.1 Current state (verified)

- `engine/core/util/STBText.{h,cpp}`: one object = one font chain at one size with its own
  R8 atlas (`STBText.h:27-28`), size = pixel height ascent-to-descent
  (`stbtt_ScaleForPixelHeight`, `STBText.cpp:66`), built-in Roboto then Noto Sans Arabic
  always appended (`:118-122`), shaping private (`shapeRun` `:143-179`, `shapeRunItems`
  `:181-229`, `shapeRange` `:231-267`, `shapeLines` `:293-370`), atlas 512 -> 4096 with
  re-raster on growth (`:430-449`), version bumped per new glyph (`:480`).
- `FontPool` keys by `"<paths>|<size>"` (`UISystem.cpp:201-213`); `UISystem` builds text
  meshes (`UISystem.cpp:335-409`) and re-creates the GPU texture per atlas version
  (`:215-239`).
- `DefaultFont.h` (Roboto Regular 400, no `post` table), `DefaultFontArabic.h` (Noto Sans
  Arabic Regular). HarfBuzz is built with `HB_NO_STYLE`, `HB_NO_MT`
  (`engine/libs/harfbuzz/CMakeLists.txt:24-33`).

#### 3.6.2 New shared pieces (engine/core/util)

```cpp
// FontFace.h: one font file (or collection member), shared by every size and user
class JASPERENGINE_API FontFace {
public:
    // nothing is logged: on failure nullptr and *error = "Font file not found: <path>" or
    // "Failed to initialize font: <label>"; callers log with their own prefix
    static std::shared_ptr<FontFace> loadFile(const std::string& path, int faceIndex = 0, std::string* error = nullptr);
    static std::shared_ptr<FontFace> loadMemory(const unsigned char* data, size_t length, int faceIndex,
                                                bool copy, const std::string& label, std::string* error = nullptr);
    ~FontFace();

    const std::string& getLabel() const;        // path or label
    const std::string& getFamilyName() const;   // name table ID 16, else 1 (UTF-16BE -> UTF-8)
    int getWeight() const;                      // OS/2 usWeightClass, 400 when absent
    bool isItalic() const;                      // OS/2 fsSelection bit 0, else head.macStyle bit 1

    int getUnitsPerEm() const;
    int getAscender() const; int getDescender() const; int getLineGap() const;   // hhea, font units (descender negative)
    int getXHeight() const;            // OS/2 sxHeight (version >= 2), else bbox of 'x', else 0.5 * ascender
    int getUnderlinePosition() const;  // post, negative below baseline; 0 if no post table
    int getUnderlineThickness() const; // post; 0 if no post table
    bool hasPostTable() const;

    uint32_t getGlyphIndex(uint32_t codepoint) const;   // 0 = not covered
    bool hasCodepoint(uint32_t codepoint) const;
    float scaleForPixelHeight(float pixels) const;      // stbtt_ScaleForPixelHeight (STBText semantics)
    float scaleForEmSize(float pixels) const;           // stbtt_ScaleForMappingEmToPixels (CSS font-size)
    // coverage bitmap of one glyph at a scale; offsets relative to pen/baseline, y down
    bool rasterizeGlyph(uint32_t glyphIndex, float scale, std::vector<unsigned char>& pixels,
                        int& width, int& height, int& xoff, int& yoff) const;
    float getGlyphAdvance(uint32_t glyphIndex, float scale) const;

    const stbtt_fontinfo& getInfo() const;
    hb_font_t* getHbFont() const;   // scale (upem, upem): positions in font units
};
```
OS/2, `post`, `head` tables are read with `hb_face_reference_table` and parsed directly
(big-endian offsets: OS/2 `usWeightClass` 4, `fsSelection` 62, `sxHeight` 86 when
`version >= 2`; `post` `underlinePosition` 8, `underlineThickness` 10; `head` `macStyle`
44). `faceIndex` goes to `stbtt_GetFontOffsetForIndex` and `hb_face_create` (STBText always
used 0, `STBText.cpp:57`).

```cpp
// FontRegistry.h: process-wide faces by family, weight and style
class JASPERENGINE_API FontRegistry {
public:
    // family empty -> face's own family; weight 0 -> face's own weight
    static void registerFace(std::shared_ptr<FontFace> face, const std::string& family = "",
                             int weight = 0, int italic = -1 /* -1: from face */, bool fallback = false);
    // CSS font matching (CSS Fonts 4 §5.2 weight rules): exact style preferred, else the other
    // style; then weight: 400 -> 500 -> lighter desc -> heavier asc; 500 -> 400 -> lighter -> heavier;
    // < 400 -> lighter desc -> heavier asc; > 500 -> heavier asc -> lighter desc.
    static std::shared_ptr<FontFace> find(const std::string& family, int weight, bool italic);
    // sans-serif, serif, monospace, system-ui, ui-sans-serif, ui-serif, ui-monospace, cursive,
    // fantasy -> built-in "Roboto" (no serif/monospace face is built in; documented)
    static bool isGenericFamily(const std::string& family);
    static bool hasFamily(const std::string& family);               // generics and built-ins included
    static std::string normalizeFamily(const std::string& family);  // lowercase, quotes/whitespace stripped
    static const std::vector<std::shared_ptr<FontFace>>& getFallbackFaces();
    static std::shared_ptr<FontFace> getBuiltInLatin();    // Roboto, family "Roboto"
    static std::shared_ptr<FontFace> getBuiltInArabic();   // Noto Sans Arabic
    // cache for STBText: the same file parsed once
    static std::shared_ptr<FontFace> getOrLoadFile(const std::string& path, int faceIndex = 0, std::string* error = nullptr);
    static void clear();   // tests; Engine::clearPools does not touch it
};
```
Family names are compared case-insensitively with surrounding quotes and whitespace
stripped. The built-ins are registered lazily on first use. A generic family is
looked up as the family "Roboto" with the normal weight/style matching, so a project's
Roboto Bold serves `sans-serif` bold (the built-in Roboto otherwise). `FontFace` checks the
collection header and table directory bounds before stb_truetype reads the file (it
trusts its input). The file cache holds `weak_ptr`s: a file is parsed once while someone
holds it.

```cpp
// TextShaper.h: HarfBuzz + SheenBidi shaping over a fallback chain (moved out of STBText)
struct ShapedGlyph { uint16_t face; bool rtl; uint32_t glyphIndex; float xOffset, yOffset, xAdvance; size_t cluster; unsigned int line; };
struct ShapeOptions {
    bool kerning = true;          // false: hb feature "kern" = 0
    float letterSpacing = 0.0f;   // pixels added after each cluster (to its last glyph in visual order); != 0 also turns off liga/clig/dlig
    int baseDirection = 0;        // 0 auto (SBLevelDefaultLTR, today's behavior), 1 LTR, 2 RTL
    std::string language;         // empty: guessed as STBText does (STBText.cpp:152-157)
};
class JASPERENGINE_API TextShaper {
public:
    // faces = chain (primary first), scales[i] = pixels per font unit of face i
    static unsigned int selectFace(const std::vector<const FontFace*>& faces, uint32_t codepoint, unsigned int current);
    static void shapeRange(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                           const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                           unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& out);
    // total advance of shapeRange
    static float measure(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                         const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                         const ShapeOptions& options);
    static void decodeUtf8(const char* text, size_t length, std::vector<uint32_t>& codepoints);
};
```

```cpp
// GlyphAtlas.h: shelf packer over an R8 pixel buffer (moved out of STBText)
class JASPERENGINE_API GlyphAtlas {
public:
    GlyphAtlas(unsigned int width, unsigned int height, int padding);
    void reset(unsigned int width, unsigned int height);
    bool pack(int width, int height, int& x, int& y);      // best-height shelf fit (STBText.cpp:386-428)
    void blit(const unsigned char* pixels, int width, int height, int x, int y);
    const std::vector<unsigned char>& getPixels() const;
    unsigned int getWidth() const; unsigned int getHeight() const;
};
```

#### 3.6.3 STBText after the refactor

Public API unchanged (`STBText.h:140-162`), behavior unchanged for `UISystem`, `Text`,
`Button`, `InputField`, `editor/util/GraphicUtils.cpp:56`. Internally:
- `FontFace` struct (`STBText.h:69-80`) replaced by `std::vector<std::shared_ptr<FontFace>>`
  from `FontRegistry::getOrLoadFile` (files) and the built-ins; per-face scale stays
  `scaleForPixelHeight(fontSize)`;
- shaping calls `TextShaper::shapeRange` with default `ShapeOptions` (identical results);
- `resetAtlas`/`packRect` use `GlyphAtlas`; growth and re-rasterization stay in STBText.

Proof: `STBTextCharacterizationTest` (5.1) is written and its expected values recorded
against the unrefactored code first (commit 1 of p0-font), then must pass unchanged after
the refactor. Plus `tools/mcp/run-tests.sh tools/mcp/tests/test_ui_anchors.py` and the
MCP tests that create text.

#### 3.6.4 `engine/core/htmlui/RmlFontEngine.{h,cpp}`

```cpp
class JASPERENGINE_INTERNAL RmlFontEngine : public Rml::FontEngineInterface {
public:
    void Initialize() override;     // registers built-ins in FontRegistry
    void Shutdown() override;       // ReleaseFontResources()
    bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight) override;
    bool LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                      Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;
    bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family,
                      Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;  // copies the data
    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                          Rml::Style::FontWeight weight, int size) override;
    Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects) override; // 0, warns once (phase 1)
    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override;
    int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                       Rml::Character prior_character = Rml::Character::Null) override;
    int GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                       Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                       const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list) override;
    int GetVersion(Rml::FontFaceHandle handle) override;   // one global version
    void ReleaseFontResources() override;                  // drops handles, glyph cache, pages; faces stay registered

    void setRasterScale(float scale);   // physical px per document px; change -> clear glyphs/pages, version++
    float getRasterScale() const;
    size_t getPageCount() const;        // tests
    std::vector<const FontFace*> getChain(Rml::FontFaceHandle handle) const;   // tests
    static std::vector<std::string> splitFamilyList(const std::string& families);
};
```

Handles: `FontFaceHandle` = pointer to an internal
`struct FaceHandle { std::vector<std::shared_ptr<FontFace>> chain; std::vector<float> emScales; int size; Rml::FontMetrics metrics; }`
cached by key `(normalized family list, italic, weight, size)`; never freed until
`ReleaseFontResources`. A successful `LoadFontFace` moves the cached handles out of the
lookup table (they stay valid for the elements holding them, new lookups see the new
face) and bumps the version.

`GetFontFaceHandle`:
1. RmlUi 6.3 passes `font-family: A, B` as one string (DESIGN research facts). Split on
   commas (outside quotes), strip quotes/whitespace, lowercase. (RmlUi 6.3's RCSS parser
   rejects a quoted value such as `'No Such Font', sans-serif` as a syntax error, so in
   phase 0 documents lists are unquoted; phase 2's rewrite handles quotes.) Empty family list -> default chain (RmlUi
   warns "No font face defined" when the handle is 0, `ElementText.cpp:86`; we never
   return 0 for a valid size).
2. For each name: `FontRegistry::find(name, weight, italic)` (a generic name resolves
   through the registry's "Roboto" family); unknown -> skipped and
   `Log::warn("[HTML UI] font-family '%s' is not loaded")` once per name.
3. When no name resolved (empty or all unknown list), built-in Roboto first; then
   `FontRegistry::getFallbackFaces()`, then built-in Roboto and Noto Sans Arabic; remove
   duplicates. (Fallback faces never displace the default font as the primary face.)
4. `weight == Auto` -> 400. Style italic with no italic face -> normal face (synthetic
   italic is phase 1).
5. Metrics from the primary face, em scale `scaleForEmSize(size)`: `size`;
   `ascent = asc*s`; `descent = -desc*s` (positive below); `line_spacing = (asc - desc + gap)*s`;
   `x_height`; `underline_position = -post.position*s` or `size/12` without `post` (built-in
   Roboto has none); `underline_thickness = max(1, post.thickness*s)` or `max(1, size/16)`;
   `has_ellipsis` = some face in the chain covers U+2026.

`GetStringWidth`: decode UTF-8, shape with `ShapeOptions{ kerning = context.font_kerning
!= Style::FontKerning::None, letterSpacing = context.letter_spacing, baseDirection from
context.text_direction (Auto/Ltr/Rtl), language = context.language }`, width =
`round(sum of advances)`, `>= 0`. With `prior_character != Null` and kerning on: width of
`prior + string` minus width of `prior`. Cache results in an `unordered_map` keyed by
(handle, options, prior, string), cleared when it exceeds 8192 entries or on version
change.

Glyph cache and pages:
- Cache key `(FontFace*, glyphIndex, rasterPixelSize)` where `rasterPixelSize =
  round(size * rasterScale * 64)`; value `{page, uv rect, quad offsets in document px}`.
- Pages: `GlyphAtlas(1024, 1024, padding 1)` each, append-only (a glyph never moves), up
  to 16 pages; a glyph that does not fit in any page: `Log::error` once and skipped.
- Each page owns a `std::unique_ptr<Rml::CallbackTextureSource>` whose callback expands the
  R8 coverage to premultiplied RGBA8 `(a,a,a,a)` and calls
  `texture_interface.GenerateTexture(span, {1024,1024})` (pattern of
  `FontFaceLayer.cpp:131-150`, `:196-200`).

`GenerateString`:
1. Shape as in `GetStringWidth`.
2. Rasterize missing glyphs at `emScale * rasterScale` into pages; mark dirty pages.
3. For each dirty page replace its `CallbackTextureSource` (the old one is destroyed, so
   RmlUi calls `ReleaseTexture` on our renderer, deferred to the end of the frame), and
   increment the global version once.
4. Emit one mesh per page used: quad per glyph at
   `round((position + pen + bearing) * rasterScale) / rasterScale` (snap to physical
   pixels; RmlUi's default engine rounds the same way in document pixels), size =
   bitmap size / rasterScale, UVs of the page, colour `colour` multiplied by `opacity`
   (all four premultiplied channels), via `Rml::MeshUtilities::GenerateQuad`; texture =
   `page.source->GetTexture(render_manager)`.
5. Return `round(total advance)`.

Why append-only pages plus deferred texture release: `ElementText` regenerates its
geometry when `GetVersion` changes (`ElementText.cpp:140-150`). An element rendered
earlier in the same `Context::Render` keeps a draw that references the page's previous
texture; that texture stays alive until the replay (3.5.2) and still contains every
glyph that element uses, because glyphs never move. Elements rendered later see the new
version and regenerate. No frame shows missing glyphs.

`LoadFontFace` (file): `FontFace::loadFile(file_name via FileData, face_index)`; family /
weight / italic from the arguments when given (`weight != Auto`, `family` non-empty),
else from the file; `FontRegistry::registerFace(..., fallback_face)`; false + error log if
the file cannot be read or parsed. The Debugger loads its embedded Courier Prime through
the `Span` overload with family `rmlui-debugger-font`.

`ReleaseFontResources`: clear handles, glyph cache, pages (their sources release the
textures), width cache; version++.

Font effects: `PrepareFontEffects` returns 0 and logs once
`"[HTML UI] font-effect is not supported yet (phase 1)"`; `GenerateString` ignores
`font_effects_handle`.

### 3.7 ECS, GameObject API, input, editor and MCP integration

#### 3.7.1 `engine/core/component/UIDocumentComponent.h`

```cpp
#ifndef UIDOCUMENT_COMPONENT_H
#define UIDOCUMENT_COMPONENT_H

#include "Engine.h"
#include <string>

namespace jasperengine{

    // An HTML/CSS document drawn over the scene (Unity's UIDocument). The document is
    // loaded into the scene's UIDocumentSystem; sortOrder orders the scene's documents.
    struct JASPERENGINE_API UIDocumentComponent{
        std::string filename;     // project-relative .html/.rml (asset path rules of FileData)
        int sortOrder = 0;        // higher draws on top (document z-index)
        bool enabled = true;      // shown and receiving pointer input

        // runtime, not serialized
        bool needReload = true;   // filename changed or reload() requested
        bool loaded = false;      // the document exists in the context
        bool loadFailed = false;  // last load failed (error logged once per attempt)
        int appliedSortOrder = 0;
        bool appliedVisible = false;
    };

}

#endif //UIDOCUMENT_COMPONENT_H
```

No RmlUi type: the system maps entity -> `Rml::ElementDocument*`.

Registration:
- `engine/core/registry/EntityRegistry.h:16-66`: include it.
- `engine/core/registry/EntityRegistry.cpp:66`: append
  `registerComponent<UIDocumentComponent>();` after `ReflectionProbeComponent` (existing
  ids unchanged). Slots: 52 -> 53 of 64 (`Signature = std::bitset<64>`,
  `engine/core/ecs/Signature.h:8`).
- `engine/core/JasperEngine.h`: include `UIDocumentComponent.h` (`:39-89`),
  `UIDocument.h` and `UIElement.h` (`:149-158`), `UIDocumentSystem.h` (`:189-194`).
- `engine/core/manager/EntityCloner.cpp:517-575` (+ `prepare<>` specialization pattern
  at `:271-276`): clone `filename`, `sortOrder`, `enabled`; reset runtime fields
  (`needReload = true`, `loaded = false`, `loadFailed = false`).

#### 3.7.2 `engine/core/subsystem/UIDocumentSystem.{h,cpp}`

Registered in `Scene::init` right after `registerSystem<UISystem>();` (`Scene.cpp:31`),
include at `Scene.cpp:10-15`.

```cpp
namespace jasperengine{
    class HtmlUIContext;
    class UIElement;

    class JASPERENGINE_API UIDocumentSystem : public SubSystem {
    public:
        UIDocumentSystem(Scene* scene);
        virtual ~UIDocumentSystem();

        void load() override;                 // nothing heavy: the context is created lazily in update()
        void destroy() override;              // destroy context, HtmlUI::release() if acquired
        void update(double dt) override;
        void draw() override;                 // empty: RenderSystem drives drawing
        void onComponentAdded(Entity entity, ComponentId componentId) override;    // mark needReload
        void onComponentRemoved(Entity entity, ComponentId componentId) override;  // unload that document

        // RenderSystem hooks (3.5.5); bodies are one-line delegations, kept as written in the skeleton
        void renderOffscreen(const Rect& viewRect);                 // context->render(viewRect)
        void drawComposite(PipelineType colorPipeline, bool flipY); // context->composite(...)

        // Engine::system* hooks, canvas coordinates (top-left origin, y down); true = consumed
        bool eventOnPointerDown(int button, float x, float y, int mods);
        bool eventOnPointerUp(int button, float x, float y, int mods);
        bool eventOnPointerMove(float x, float y, int mods);

        // the editor lays documents out at the project canvas whatever the view size
        // (mirrors UISystem::setAnchorReferenceSize, UISystem.h:134-137)
        void setReferenceSize(int width, int height);
        void clearReferenceSize();

        bool loadDocumentNow(Entity entity);           // UIDocument::load: synchronous when the view is loaded
        bool isDocumentLoaded(Entity entity) const;
        UIElement getElementById(Entity entity, const std::string& id) const;
        UIElement getDocumentBody(Entity entity) const;

        bool hasDocuments() const;      // any UIDocumentComponent with a filename
        bool isAllLoaded() const;       // every enabled document loaded or failed, and (if any visible) a frame rendered
        void setDebuggerVisible(bool visible);
        HtmlUIContext* getContext() const;   // internal/tests

    private:
        // owned (p0-ecs): a field naming the hidden HtmlUIContext makes GCC warn that this class
        // has a less visible field (-Wattributes) wherever HtmlUIContext.h is included first,
        // so it points to UIDocumentContext, an HtmlUIContext subclass defined in the .cpp
        UIDocumentContext* context = nullptr;
        std::unordered_map<Entity, Rml::ElementDocument*> documents;
        bool acquired = false;
        int referenceWidth = 0, referenceHeight = 0;
        bool pointerCaptured = false;   // a consumed down is followed by a consumed up
    };
}
```

`update(dt)` (runs paused too: the editor never pauses it, `editor/Project.cpp:4287-4291`):
1. If `!Engine::isViewLoaded()` return.
2. If no component has a non-empty `filename` and there is no context: return (scenes
   without documents, including editor gizmo scenes, never initialise RmlUi).
3. If no context: `acquired = HtmlUI::acquire()` (return if false), create
   `HtmlUIContext("scene-" + pointer)` with the current dimensions.
4. Dimensions = reference size if set, else `Engine::getCanvasWidth/Height()`
   (`UISystem.cpp:1539-1545` pattern); `context->setDimensions`;
   `context->setDensityRatio(context->getPixelRatio())`.
5. For each `UIDocumentComponent` (component array order):
   - `visible = component.enabled && scene->isEntityActiveInHierarchy(entity)`.
   - `needReload`: unload the old document (if any); if `filename` empty -> not loaded;
     else `loadDocument(filename)`; null -> `loadFailed = true` (the context logged
     `"[HTML UI] could not load document '<filename>'"`), else `loaded = true`; set
     `needReload = false`, force re-apply of order and visibility.
   - sort order changed: `doc->SetProperty(Rml::PropertyId::ZIndex, Rml::Property((float)sortOrder, Rml::Unit::NUMBER))`
     (a numeric z-index also stops RmlUi pulling a focused document to the front,
     `Context.cpp:1240-1246`).
   - visibility changed: `doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None)` / `doc->Hide()`.
6. `context->update()`.

`destroy()`: `documents.clear()`, `context.reset()` (runs `HtmlUIContext::destroy`), and
`HtmlUI::release()` when `acquired`. Mark every component `needReload = true`,
`loaded = false` so a reloaded view (`Scene::load` after `systemViewLoaded`) recreates
everything.

Input rules (`eventOnPointer*`): return false when there is no context or no visible
document. Convert canvas coordinates to context coordinates (`x * dims.x /
Engine::getCanvasWidth()`, same for y; identical unless a reference size is set). Mouse
button mapping: `D_MOUSE_BUTTON_1/2/3` -> RmlUi 0/1/2 (left/right/middle); modifiers
`D_MODIFIER_SHIFT/CONTROL/ALT/SUPER/CAPS_LOCK/NUM_LOCK` (`engine/core/Input.h:9-14`) ->
`Rml::Input::KM_SHIFT/KM_CTRL/KM_ALT/KM_META/KM_CAPSLOCK/KM_NUMLOCK`.
- Move: `processMouseMove`; consumed = `isPointerOverContent()`.
- Down: `processMouseMove` to the position, then `processMouseButtonDown`; consumed =
  `isPointerOverContent()`; remember `pointerCaptured = consumed`.
- Up: `processMouseMove`, `processMouseButtonUp`; consumed = `pointerCaptured ||
  isPointerOverContent()`; reset `pointerCaptured`.

`HtmlUIContext::isPointerOverContent()`: the hover (or active) element exists, is not the
context root and is not an `ElementDocument` itself, i.e. the pointer is over an element
inside a body. A transparent full-screen body lets clicks through to the game; authors
make wrapper elements click-through with `pointer-events: none` (supported by RmlUi
6.3). `Context::IsMouseInteracting` is not used because it treats the body as
interactive (`Context.cpp:849-852`).

#### 3.7.3 Engine input hooks (`engine/core/Engine.cpp`)

Gate for documents (new static next to `canSendUIEvents`, `Engine.cpp:1362-1364`):
```cpp
// documents are an explicit request for input: they receive it unless the scene turns
// UI events off (UIEventState::DISABLED) or a loading scene covers it
static bool canSendDocumentEvents(Scene* scene){
    return scene->getEnableUIEvents() != UIEventState::DISABLED && !SceneManager::isCoveredByLoading(scene);
}
```
In each loop over `getScenesSnapshot()`:
- `systemMouseDown` (`:1465-1489`) and `systemTouchStart` (`:1380-1401`, pointer 0 only, as
  button 1): call `scene->getSystem<UIDocumentSystem>()->eventOnPointerDown(...)` first;
  if it returns true set `uiEventReceived = true` and skip that scene's
  `UISystem::eventOnPointerDown` (documents are in front of component UI); else the
  existing UISystem code.
- `systemMouseUp` (`:1490-1514`) and `systemTouchEnd` (`:1403-1424`, pointer 0): call
  `eventOnPointerUp` (gate: `scene->getEnableUIEvents() != UIEventState::DISABLED`, so a
  covered scene still gets the release like UISystem) and the existing UISystem call
  regardless; OR the results.
- `systemMouseMove` (`:1516-1540`) and `systemTouchMove` (`:1426-1446`, pointer 0):
  call `eventOnPointerMove` and the existing UISystem call; OR the results.

Key, wheel, char, enter/leave: unchanged in phase 0.

#### 3.7.4 GameObject API

`engine/core/object/ui/UIDocument.{h,cpp}`:
```cpp
class JASPERENGINE_API UIDocument: public GameObject {
public:
    UIDocument(Scene* scene);                 // adds UIDocumentComponent
    UIDocument(Scene* scene, Entity entity);
    virtual ~UIDocument();

    void setFilename(const std::string& filename);   // loads at the next update
    const std::string& getFilename() const;
    bool load(const std::string& filename);          // setFilename + UIDocumentSystem::loadDocumentNow; false if it failed
    void reload();                                   // needReload = true
    bool isLoaded() const;

    void show(); void hide();
    void setVisible(bool visible); bool isVisible() const;   // component.enabled
    void setSortOrder(int sortOrder); int getSortOrder() const;

    UIElement getElementById(const std::string& id) const;
    UIElement getBody() const;
};
```

`engine/core/object/ui/UIElement.{h,cpp}` (value type, weak reference):
```cpp
struct UIElementHandle;   // defined in UIElement.cpp: holds Rml::ObserverPtr<Rml::Element>

class JASPERENGINE_API UIElement {
public:
    UIElement();
    bool isValid() const;                       // false once the element or document is gone

    std::string getId() const;
    std::string getTagName() const;
    std::string getTextContent() const;         // concatenated text of descendants
    void setTextContent(const std::string& text);   // replaces children with escaped text
    std::string getInnerHTML() const;           // RML of the children (HTML after phase 2)
    void setInnerHTML(const std::string& markup);

    bool hasAttribute(const std::string& name) const;
    std::string getAttribute(const std::string& name) const;
    void setAttribute(const std::string& name, const std::string& value);
    void removeAttribute(const std::string& name);

    bool hasClass(const std::string& name) const;
    void setClass(const std::string& name, bool active);
    bool setStyleProperty(const std::string& name, const std::string& value);   // false + warning if RmlUi rejects it
    void removeStyleProperty(const std::string& name);

    Rect getBoundingBox() const;                // border box, canvas px, top-left origin
    bool isPseudoClassSet(const std::string& pseudoClass) const;   // "hover", "active", "focus"

private:
    friend class UIDocumentSystem;
    std::shared_ptr<UIElementHandle> handle;
};
```
Every method on an invalid element is a no-op returning empty/false.

#### 3.7.5 Editor registration (minimum for save/load, MCP, export)

UpdateFlags: `editor/Catalog.h:16-61` add `UpdateFlags_UIDocument_Reload` (next free bit,
37); `Catalog::updateEntity` (`Catalog.cpp:4215+`) sets `needReload = true` on it.

| # | Location | Change |
|---|---|---|
| C1 | `editor/Catalog.h:116-118` | append `UIDocumentComponent` to `enum class ComponentType` (at the end). |
| C2 | `editor/Catalog.cpp` | `kUIDocumentProperties[]`: `filename` (`PropertyType::String`, `UpdateFlags_UIDocument_Reload`), `sortOrder` (`Int`, none); resolve + enumerate functions; row in `kFastComponentResolvers` (`:3068-3121`). |
| C3 | `Catalog.cpp:3253-3367` `computeComponentName` | `"UIDocumentComponent"` (YAML/MCP key `uidocument`). Corrected by p0-ecs: this name is the C++ type name too, `Factory` generates `scene->findComponent<` + name + `>` with it, so it must be the struct name like every other row. |
| C4 | `Catalog.cpp:3490-3600` `getComponentId` | case (the default returns 0 = MeshComponent's bit). |
| C5 | `Catalog.cpp:3602-3712` `getComponentType` | name -> type. |
| C6 | `Catalog.cpp:3755-3916` `findComponents` | detect the component. |
| C7 | `Catalog.cpp:4416+` `copyComponent` | copy (prefab overrides). |
| C8 | `Catalog.cpp:3132-3154` `findEnabledGetter` | `UIDocumentComponent` -> `enabled` checkbox; display name `"UI Document"` (`:3370-3425`). |
| S1 | `editor/Stream.{h,cpp}` | `encodeUIDocumentComponent` / `decodeUIDocumentComponent` (`filename`, `sortOrder`, `enabled`), hooked in `encodeComponents` (`:3708+`) and `decodeComponents` (`:3985+`) like Image (`:3777-3780`, `:4135-4146`). |
| P1 | `editor/util/ProjectUtils.cpp:903-1277`, `:1367-1695` | `addEntityComponent` / `removeEntityComponent` cases. |
| D1 | `editor/window/dialog/ComponentAddDialog.cpp:90-102` | in the "UI" list (also gates MCP `add_component`, `ComponentTools.cpp:491-497`). |
| I1 | `editor/window/InspectorWindow.{h,cpp}` | `drawUIDocumentComponent`: `propertyRow(RowPropertyType::String, ..., "filename", "Source", ...)`, `propertyRow(RowPropertyType::Int, ..., "sortOrder", "Sort Order", ...)`; dispatch in the chain at `:13969-14073`. |
| F1 | `editor/Factory.{h,cpp}` | `createUIDocumentComponent` (pattern `:1339-1352`), case in `createComponent` (`:2349-2404`; the default silently drops a component from exports). |
| M1 | `editor/mcp/McpRefs.cpp:375`, `:417`, `editor/mcp/tools/ComponentTools.cpp:801` | the "last component type" sentinel `ReflectionProbeComponent` -> `UIDocumentComponent`. |
| M2 | `editor/mcp/tools/ScriptTools.cpp:114-122` `isTextFile` | add `.html`, `.htm`, `.css`, `.rml`, `.rcss` (used by `write_file`/`read_file`, `AssetTools.cpp:965-967`); regenerate `docs/mcp/TOOLS.md` with `tools/mcp/gen_tool_docs.py` if a tool description lists extensions. |
| A1 | `editor/render/SceneRender.cpp:737-740`, `editor/App.cpp:2652-2654`, `:2869-2871`, `:2928-2930` | `->getSystem<UIDocumentSystem>()->update(0);` after each `UISystem` `update(0)` (edit-mode refresh). |
| A2 | `editor/App.cpp:2496-2499` | `if (auto ds = sp.scene->getSystem<UIDocumentSystem>(); ds && !ds->isAllLoaded()) active = true;` (keep redrawing while documents/shaders load), only when the Game view draws that scene (`gameWindow->wantsEditPass(sp.id) \|\| gameWindow->hostsScene(sp.id)`): `isAllLoaded` waits for a rendered frame, which the Scene view never makes in phase 0, so without the gate a selected scene with a document would redraw forever (p0-ecs). |
| A3 | `editor/Project.cpp:1510`, `:1601`, `:4603` | `->getSystem<UIDocumentSystem>()->setReferenceSize(...)` next to each `setAnchorReferenceSize`, and `clearReferenceSize()` next to the play-start `clearAnchorReferenceSize` (`:3054`). |
| A4 | `editor/shader/ShaderVariantCollector.cpp`, `editor/Project.cpp:~2841`, `editor/window/dialog/ExportWindow.cpp:1236-1242` | HTML UI shader keys (owned by p0-render, 3.4.2). |

The MCP `filename` property gets path validation/normalisation for free
(`McpValues.cpp:126-128` recognises `filename`). `add_component`/`set_properties`/
`get_entity` are generic through Catalog (`ComponentTools.cpp:332-379`, `:475-545`,
`McpValues.cpp:2077-2150`). Exports copy every file under assets
(`Exporter.cpp:1587-1657`), so `.html`/`.css`/images ship.

Not in phase 0 (phase 1): Hierarchy Create entry, icon, `Project::visitAssetPathsInRegistry`
/ `collectResourcesInRegistry` (`Project.cpp:481-723`), `ProjectUtils::getEntityClassInfo`,
`PropertyCmd::isNonStructuralProperty`, Lua bindings and `LuaBinding::pushEntityHandleByType`
(an unknown type falls back to `EntityHandle` with a warning, `LuaBinding.cpp:651`).

### 3.8 Test infrastructure

#### 3.8.1 Build

`engine/tests/htmlui/CMakeLists.txt` (added only by the standalone engine build with
`JASPERENGINE_HTMLUI_TESTS=ON`, 3.2.3):

```cmake
# googletest from the repository's yaml-cpp test tree (not shipped with the engine)
set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)         # gtest 1.11 declares 2.8.12: CMake 4 rejects it, 3.x warns
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
target_compile_options(gtest PRIVATE -w)

file(GLOB HTMLUI_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/harness/*.cpp)
add_executable(jasperengine-htmlui-tests ${HTMLUI_TEST_SOURCES}
    ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp)
target_include_directories(jasperengine-htmlui-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/harness
    ${JASPERENGINE_ROOT}/platform/linux ${JASPERENGINE_ROOT}/platform/common)
find_package(X11 REQUIRED)
target_link_libraries(jasperengine-htmlui-tests PRIVATE jasperengine rmlui gtest X11::X11)
if(GRAPHIC_BACKEND STREQUAL "vulkan")
    target_sources(jasperengine-htmlui-tests PRIVATE ${JASPERENGINE_ROOT}/platform/common/VulkanContext.cpp)
    target_link_libraries(jasperengine-htmlui-tests PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(jasperengine-htmlui-tests PRIVATE GL)
endif()
target_compile_definitions(jasperengine-htmlui-tests PRIVATE
    JASPER_HTMLUI_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_HTMLUI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
```
Linux only (`if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux") message(FATAL_ERROR ...)`).
Packages add test files by creating `engine/tests/htmlui/Test*.cpp`; the glob means no
CMake edit.

#### 3.8.2 Harness (`engine/tests/htmlui/harness/`)

| File | Contents |
|---|---|
| `HeadlessContext.{h,cpp}` | `static bool create(int w, int h)`, `destroy()`, `isCreated()`, `getBackendName()`. Opens the X display (`WindowLinux::openDisplay/create`, a window that is never mapped); GL: GLX 4.1 core context (copy of `selectVisual`/`createContext` in `engine/platform/linux/JasperEngineLinux.cpp:96-196`); Vulkan: `VulkanContext::create` with an Xlib surface (`JasperEngineLinux.cpp:203-218`). Never includes RmlUi headers (X11 macros `None`, `Bool` clash with RmlUi enums). The harness never presents: frames render into an engine `Framebuffer` (no swapchain pass), and the swapchain is never acquired. |
| `TestSystem.{h,cpp}` | `class TestSystem : public System` (namespace `jasperengine::test`, like the whole harness): `static TestSystem& get()`; `setScreenSize(w, h)`; `getSokolEnvironment/Swapchain` (Vulkan: `VulkanContext::environment/swapchain`); `getAssetPath()` = `JASPER_HTMLUI_TEST_SOURCE_DIR "/fixtures"` (also the Lua path); user data, cache = `JASPER_HTMLUI_TEST_BINARY_DIR "/userdata"`; shader path = binary dir `/shaders` (empty: the runtime compiler compiles from the embedded shaderlib, `ShaderPool.cpp:546-618`); `isUserDataLoaded()` true. Keeps a copy of every log line: `getLogs()`, `countLogs(type, text)`, `clearLogs()` (type = `D_LOG_ERROR`/`D_LOG_WARN`/`D_LOG_VERBOSE`/`D_LOG_DEBUG`; `Log::debug` prints nothing in Release). |
| `GpuEnvironment.{h,cpp}` | `static void initEngine(argc, argv)`: called by `main` before any test, `Engine::systemInit(argc, argv, new TestSystem)` (no GPU), so CPU-only suites get `System::instance()`, `Log` and `FileData`. `static bool ensure()`: lazily, once per process, `HeadlessContext::create`, an engine `Framebuffer` target set with `Engine::setFramebuffer`, `Engine::systemViewLoaded()`, `systemViewChanged()`, `ShaderPool::setAsyncCompilation(false)`; false (reason on stderr) without a display or device; macro `JASPER_REQUIRE_GPU()` asserts it. `static bool isReady()`; `static void setView(int screenW, int screenH, int canvasW, int canvasH, Scaling)` (screen size of the TestSystem = target size, `Engine::setCanvasSize`, `setScalingMode`, `systemViewChanged`); `static bool runFrames(std::function<bool()> ready, int maxFrames = 300)` (calls `Engine::systemDraw()` until `ready()` then 2 more frames); `static void drawFrames(int count)`; `static Framebuffer& target()`; `static void shutdown()` (`systemViewDestroyed`, `systemShutdown`, device, display), run by the gtest environment `GpuTestEnvironment::TearDown`. |
| `Readback.{h,cpp}` | `bool readTexture(TextureRender& texture, int w, int h, std::vector<uint8_t>& rgba)`: GL `glGetTexImage` (as `editor/util/GraphicUtils.cpp:75-83`, then `sg_reset_state_cache()`); Vulkan staging-buffer copy (as `GraphicUtils.cpp:86-226`) using `getVulkanImageHandler()`. Output rows top-down (offscreen targets are top-left origin on every backend; verified by `HarnessGpuTest.ComponentUIImage` on glcore and vulkan, and by the `orientation` golden). |
| `ImageCompare.{h,cpp}` | `struct CompareOptions { int pixelThreshold = 24; double maxBadFraction = 0.0005; double maxMeanDiff = 1.0; };` `CompareResult` (`passed`, `updated`, `message`, `maxDiff`, `meanDiff`, `badPixels`, `badFraction`, `actualPath`, `diffPath`). `CompareResult compareWithGolden(const std::string& name, const std::vector<uint8_t>& rgba, int w, int h, const CompareOptions& = {});` loads `golden/<name>.png` (stb_image), per-pixel `d = max channel abs diff` over RGBA; fails if `count(d > pixelThreshold) > maxBadFraction * pixels` or `mean(d) > maxMeanDiff` or sizes differ or golden missing. On failure writes `<binary>/htmlui-test-output/<name>.actual.png` and `<name>.diff.png` (golden at 30 % gray + red proportional to `d` where `d > pixelThreshold`; a missing golden writes the actual image only). With `JASPER_HTMLUI_UPDATE_GOLDEN=1` it writes the golden instead and passes (only allowed on glcore; fails on vulkan). Also `compareWithGoldenFile(goldenPath, outputDir, name, ...)`, `compareTargetWithGolden(name, options)` (reads back `GpuEnvironment::target()`), `readPng`, `writePng`. Macros `EXPECT_GOLDEN(name)` and `EXPECT_GOLDEN_OPTIONS(name, options)` read back `GpuEnvironment::target()` and compare. |
| `RecordingRenderInterface.{h,cpp}` | CPU-only `Rml::RenderInterface` for tests without a GPU: stores compiled geometry (copies), generated textures (pixels), loaded textures (source; dimensions read with stb_image through `Data`, 0 when unreadable), every `LoadTexture` request, and the draw list of a frame (geometry, translation, texture, scissor, transform). `beginFrame()` clears the draw list; `getDraws()`, `getLiveGeometryCount()`, `getLiveTextureCount()`, `getCompiledGeometryTotal()`, `getGeometry(h)`, `getTexture(h)`, `getLoadRequests()`. Must outlive `Rml::Shutdown()`. Used by font and context tests. |
| `SceneFixture.{h,cpp}` | Creates a `Scene` with a `CAMERA_UI` camera, background color `#1E1E1E` (opaque), sets it as the engine scene (removed in the destructor). `UIDocument addDocument(const std::string& file, int sortOrder = 0)`; `template<T> T& add()` for any GameObject (objects belong to the fixture; returned handles do not own their entity); `bool waitUntilLoaded(int maxFrames = 300)` = `runFrames` until `RenderSystem::isAllLoaded()` and, once a document was added, `UIDocumentSystem::isAllLoaded()` (`Scene::getSystem` of a system `Scene::init` did not register is undefined behavior in Release, so the document system is only asked when documents exist). |
| `main.cpp` (top level) | `InitGoogleTest`, `GpuEnvironment::initEngine`, adds `GpuTestEnvironment`; GPU init happens only when a test calls `GpuEnvironment::ensure()`, so CPU-only suites run without a display. |

Fixtures live in `engine/tests/htmlui/fixtures/` (asset root of the tests):
`common/base.css` (phase-0 stand-in for the user-agent stylesheet: `body, div, p, h1, h2,
h3, section, header, footer, ul, li, button { display: block; }`, `body { font-family:
sans-serif; font-size: 16px; color: #ffffff; }`), the shared images in `common/`
(`checker.png` 64x64, 8 px cells white/#404040 with the top-left cell red as an orientation
mark; `alpha50.png` 32x32 red at 50 % alpha (255,0,0,128); `icon.svg` 24x24, a #2060c0
rounded square with a white circle), `common/foundation.html` + `common/rmlfile.txt` (the
foundation tests), and the HTML documents of 5.2 in each package's own fixture directory. All
documents use XML-well-formed HTML (`<html><head>...</head><body>...</body></html>`, RmlUi
accepts any root; `<link type="text/css" href="...">` is accepted,
`XMLNodeHandlerHead.cpp:57-62`) and RCSS syntax (colors as `#RRGGBBAA`, `border: 2px
#ff0000`, `decorator: image(...)`).

#### 3.8.3 Running

- `docker/linux.Dockerfile` (headless tools line 15): add `mesa-vulkan-drivers
  vulkan-tools`. Verified in the current image: after installing, lavapipe (Mesa 23.2.1,
  Vulkan 1.3.255) exposes `VK_EXT_descriptor_buffer`, which sokol's Vulkan backend needs.
  The image also needs newer Vulkan headers: 22.04's `libvulkan-dev` is 1.3.204, which
  lacks the `VK_EXT_descriptor_buffer` declarations sokol compiles against (1.3.235+), so no
  Vulkan build ever compiled in this image before. A separate layer installs
  KhronosGroup/Vulkan-Headers `v1.3.296` into `/usr/local` (found first by FindVulkan and the
  compiler); the 1.3.204 loader stays.
- `tools/build.sh`: new target `htmlui-tests` (uses the linux image). Env
  `HTMLUI_BACKEND=glcore|vulkan` (default glcore), `BUILD_TYPE` (default Release),
  `RUN=0` to build only, `HTMLUI_TEST_ARGS` passed to the binary (e.g.
  `--gtest_filter=Golden*`). It runs:
  ```
  cmake -S engine -B build/linux/htmlui-tests-$HTMLUI_BACKEND -G Ninja -DCMAKE_BUILD_TYPE=$BT \
        -DJASPERENGINE_HTMLUI_TESTS=ON -DGRAPHIC_BACKEND=$HTMLUI_BACKEND $CCACHE_FLAGS
  cmake --build build/linux/htmlui-tests-$HTMLUI_BACKEND --target jasperengine-htmlui-tests $JOBS_FLAG
  xvfb-run -a -s "-screen 0 1600x900x24" \
      build/linux/htmlui-tests-$HTMLUI_BACKEND/tests/htmlui/jasperengine-htmlui-tests $HTMLUI_TEST_ARGS
  ```
  with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` for vulkan.
  Usage line added to the header comment. The `linux` target gains
  `GRAPHIC_BACKEND=vulkan` support: output in `build/linux/editor-vulkan` /
  `editor-vulkan-install` (passes `-DGRAPHIC_BACKEND=vulkan`), so MCP tests can run a
  Vulkan editor (`JASPER_EDITOR=build/linux/editor-vulkan-install/bin/jasperengine-editor`).
  The host has lavapipe (Mesa 25.2.8).
- sokol's Vulkan backend caps descriptor data at `_SG_VK_MAX_DESCRIPTOR_DATA_SIZE`
  (`engine/libs/sokol/sokol_gfx.h`), 256 bytes upstream with a FIXME about llvmpipe.
  The image's lavapipe (Mesa 23.2.1) reports 280-byte sampler, image and uniform-buffer
  descriptors (the host's Mesa 25.2.8: 64), so every sampler and texture view failed to
  create and textured draws were silently skipped. p0-foundation raised it to 320
  (one-line change, commented in place).
- Golden update: `JASPER_HTMLUI_UPDATE_GOLDEN=1 HTMLUI_TEST_ARGS=--gtest_filter=<suite>.* tools/build.sh htmlui-tests`
  (glcore only); inspect every changed PNG before committing.

### 3.9 SVG library benchmark (`tools/htmlui-bench/`)

Standalone (not part of the engine or editor build). Built on the host (CMake 3.28, GCC
13) for native numbers and in `jasperengine/web-build` (emcc 6.0.10, node) for wasm.

#### 3.9.1 Layout

```
tools/htmlui-bench/
  README                         (how to run; plain text, one screen)
  CMakeLists.txt                 options LUNASVG_DIR (default /tmp/htmlui-src/lunasvg),
                                 THORVG_DIR (/tmp/htmlui-src/thorvg), NANOSVG_DIR (engine/libs/nanosvg),
                                 STB_DIR (engine/libs/stb)
  thorvg_config.h.in             THORVG_VERSION_STRING, CPU_ENGINE, SVG_LOADER, LOTTIE_LOADER (size probe only),
                                 THREAD, FILE_IO, PARTIAL_RENDER; AVX only for x86 native
  src/Backend.h                  struct SvgBackend { name; bool parse(const std::string&); bool render(int w, int h,
                                 std::vector<uint8_t>& rgbaPremultiplied); bool mutateFill(const std::string& id,
                                 uint32_t rgba); void reset(); size_t retainedBytes(); }
  src/BackendLuna.cpp            lunasvg::Document::loadFromData, Document::render(Bitmap, Matrix::scaled),
                                 getElementById(id).setAttribute("fill", ...) for mutation
  src/BackendThorVG.cpp          tvg::Initializer::init(0), SwCanvas::gen(), target(..., ColorSpace::ABGR8888),
                                 Picture::load(data, size, "svg", nullptr, true), size(w,h), update/draw/sync;
                                 mutation via Picture::paint(Accessor::id(id)) (const_cast) -> Shape::fill
  src/BackendNanoSVG.cpp         nsvgParse (copy of the text) + nsvgRasterize; mutation = re-parse;
                                 output converted to premultiplied
  src/main.cpp                   CLI: svgbench --lib luna|thorvg|nanosvg --list files.txt --sizes 24,48,128,512,1024
                                 --mode time|render|mutate|memory --out results.csv --render-dir DIR --min-time-ms 200
  src/size/*.cpp                 minimal programs for binary size (baseline, each library, ThorVG svg and svg+lottie)
  scripts/fetch_corpus.sh        downloads/clones into build/htmlui-bench/corpus at pinned commits/URLs
  scripts/references.py          resvg reference PNGs (venv: pip install resvg-py numpy pillow) at each size;
                                 Chrome headless cross-check (playwright, already on the host) for Lucide and Fluent at 48 px
  scripts/compare.py             metrics per image vs reference -> CSV
  scripts/run_native.sh, scripts/run_wasm.sh, scripts/report.py (CSV -> markdown tables)
  results/*.csv                  committed summary CSVs used by the report
```

#### 3.9.2 Corpus (all downloaded at run time into `build/htmlui-bench/corpus`, git-ignored)

| Set | Source (license) | Content |
|---|---|---|
| icons | Lucide, 1866 SVGs from `/tmp/htmlui-src/lucide/icons` or `lucide-icons/lucide` at the same commit (ISC) | stroke icons, `currentColor` |
| multicolor | `microsoft/fluentui-emoji` "Color" SVGs (MIT), 300 files chosen by a fixed seed | gradients, many paths |
| illustrations | `thorvg/thorvg.example` `res/svg` (MIT), 81 files | gradients, clipPath, masks, large paths |
| huge | Wikimedia `BlankMap-World.svg` (public domain, 1.1 MB); the thorvg.example files > 300 KB (`Prismatic-Stylized-Mexican-Eagle...` 757 KB, `car.svg` 527 KB) | parse/memory stress |
| conformance | resvg test suite `linebender/resvg` `crates/resvg/tests` (MIT/Apache-2.0), 1725 SVG + reference PNG, sparse clone | per-category correctness |

`currentColor` is replaced by `#000000` in a preprocessed copy used by every library and
the references (nanosvg draws it gray otherwise); native `currentColor` support is
recorded separately in the feature matrix.

#### 3.9.3 Metrics and method

- Single-threaded for all (ThorVG `Initializer::init(0)`); ThorVG with threads reported as
  an extra column. `-O2`, same compiler and flags for all.
- Parse time: median and p95 over repetitions until 200 ms per file.
- Rasterize time at 24, 48, 128, 512, 1024 px (icons, multicolor, illustrations; huge at
  512 and 1024): includes canvas clear and full draw; for ThorVG force a redraw each
  iteration (the probe showed 6.7 µs re-renders when nothing changed because of cached
  work).
- Mutation (retained scene): change the fill of one element by id and re-render at 48 px
  (LunaSVG `setAttribute`, ThorVG `Picture::paint(id)`, nanosvg re-parse); report whether
  each library can modify a loaded document without re-parsing.
- Memory: peak RSS (`getrusage`, one process per library x set), retained heap after
  load (`mallinfo2().uordblks` delta).
- Binary size: stripped minimal executables at `-Os -ffunction-sections -fdata-sections
  -Wl,--gc-sections`, delta against the baseline; for wasm the `.wasm` size delta with
  `em++ -Os`.
- Correctness: premultiplied RGBA8 vs reference; per image mean absolute difference and
  fraction of pixels with any channel difference > 16; an image "passes" when that
  fraction < 1 %. resvg suite: pass rate per category (structure, shapes, painting,
  paint-servers, masking, text, filters) at the reference size. Icon sets: mean
  difference and pass rate vs resvg, with the Chrome cross-check agreement reported.
- AA quality: difference restricted to reference pixels with 0 < alpha < 255 on Lucide at
  24 and 48 px.
- Wasm: the bench compiled with `em++ -O2 -sNODERAWFS=1 -sENVIRONMENT=node
  -sALLOW_MEMORY_GROWTH=1` in `jasperengine/web-build`, run with node on the icons and
  illustrations sets; ThorVG without AVX.

#### 3.9.4 Decision rule (from the owner) and output

ThorVG is required for Lottie, so SVG uses ThorVG unless LunaSVG is clearly better on the
typical workload:
- LunaSVG median Lucide rasterization time at 24 and 48 px (parse excluded and
  included, both reported) is at least 1.5x faster than ThorVG on native x86-64; or
- LunaSVG is clearly more correct: resvg-suite pass rate higher by at least 10 percentage
  points over the structure, shapes, painting, paint-servers and masking categories
  combined, or visibly wrong ThorVG renders in the icon/multicolor sets that LunaSVG gets
  right (listed with images).
Text and filters are reported but not decisive (UI SVGs rarely use them; RmlUi text is
separate).

Output `docs/html-ui/svg-benchmark.md`: environment and versions (full commit hashes),
corpus, method, result tables (time per set and size, memory, binary size native/wasm,
correctness per category, AA, mutation), a feature matrix (`currentColor`, `<style>`
CSS, gradients, clipPath, mask, pattern, markers, dash, filters, text, nested svg,
`preserveAspectRatio`), the decision and its reasons. A preliminary probe (one Lucide
icon, 48 px) measured LunaSVG 88.8 µs vs ThorVG 74.6 µs render, nanosvg 154 µs; the
benchmark replaces this with full data.

### 3.10 Decisions and deviations from DESIGN.md

| # | Decision | Reason |
|---|---|---|
| D1 | One `Rml::Context` per scene (as DESIGN), created lazily only when the scene has a `UIDocumentComponent` with a filename. | Every scene (editor gizmo/overlay scenes too) runs `Scene::init`; RmlUi must not initialise for them. |
| D2 | **Deviation:** one offscreen target per context (per scene), not per document; documents of a scene are ordered by `sortOrder` -> numeric `z-index` inside the context. | `Context::Render` draws all documents of a context in one call (`Context.cpp:222-242`); per-document targets would need one context per document, losing cross-document focus/hover and costing a full-screen RGBA target per document (about 8 MB at 1080p each). Render-to-texture documents (phase 1) get their own context and target. |
| D3 | Documents are composited in the scene's main camera pass after transparent renders; not in render-to-texture cameras; not in passes that draw editor overlays (Scene view). | DESIGN screen-space rule; in front of component UI; Scene view display is editor integration (phase 1). |
| D4 | Offscreen rendering happens in `RenderSystem::drawFramePasses()`. | Runs once per frame before any camera pass for both `draw()` and editor `drawView()`; keeps Vulkan to one swapchain pass. |
| D5 | Target resolution = view pixels, glyph raster scale = view/canvas ratio (quantized to 0.25, clamped 1..4, global for the font engine). | Crisp vector shapes and text on scaled canvases without changing CSS px semantics. |
| D6 | Per-frame streaming of RmlUi geometry into two STREAM buffers per context with CPU index rebasing; compiled geometry keeps only spans. | sokol pools are shared (`SokolSystem.cpp:43-48`); no base-vertex on GLES3.0/WebGL2; one buffer update per frame. |
| D7 | Pointer input is consumed only over elements inside a document body. | A transparent HUD must not swallow game input; `pointer-events: none` for wrappers. |
| D8 | Test target is built from the standalone (static) engine build, not the editor build. | The editor's shared engine hides RmlUi symbols (hidden visibility); tests need RmlUi and engine internals. |
| D9 | Uniform/texture lookups for the HTML UI shader by name; no new `UniformBlockType`/`TextureShaderType`. | Fewer shared enum edits; the by-name path is already used by post-process (`RenderSystem.cpp:5249-5252`). |
| D10 | The component field is `filename` (Inspector label "Source"). | MCP asset-path validation keys on `filename` (`McpValues.cpp:126-128`), same as `AudioSource.filename`. |
| D11 | `dp` ratio = view pixels per canvas pixel. | DESIGN ("dp/dppx ratio comes from the canvas to screen scale"). |
| D12 | RmlUi plugins SVG, Lottie, Lua are not vendored. | DESIGN: our own Lua API and ThorVG; the RmlUi SVG plugin is LunaSVG and Lottie is rlottie. |

## 4. Work packages

Waves: wave 1 runs in parallel; wave 2 starts when p0-foundation is merged into
`html-ui`; wave 3 starts when all wave-2 packages are merged. Worktrees
`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2-wt/<slug>` on branches
`hui/<slug>` (BRIEF).

### Wave 1

#### p0-foundation

Goal: RmlUi builds everywhere; every new file exists as a compiling skeleton with the
signatures of section 3; HtmlUI/RmlSystem/RmlFile work; the test infrastructure runs.

Owns:
- `engine/libs/rmlui/**`, `docs/html-ui/FORK.md`
- `engine/core/htmlui/HtmlUI.{h,cpp}`, `RmlSystem.{h,cpp}`, `RmlFile.{h,cpp}` (final)
- skeletons (declarations exactly as section 3, bodies returning failure/empty, no
  behavior): `RmlRenderer.{h,cpp}`, `RmlFontEngine.{h,cpp}`, `HtmlUIContext.{h,cpp}`,
  `HtmlUIContextRender.cpp`, `engine/core/util/{FontFace,FontRegistry,TextShaper,GlyphAtlas}.{h,cpp}`
  (every stub body carries the comment `// HTMLUI_SKELETON: <owning package>`, which the
  owner removes when it implements the body),
  `UIDocumentComponent.h`, `UIDocumentSystem.{h,cpp}` (with the final one-line bodies of
  `renderOffscreen`/`drawComposite`), `UIDocument.{h,cpp}`, `UIElement.{h,cpp}`
- `engine/tests/htmlui/{CMakeLists.txt,main.cpp,harness/**,fixtures/common/**}`,
  `TestFoundation*.cpp`, `golden/harness_*.png`
- `tools/build.sh` (htmlui-tests target, `GRAPHIC_BACKEND=vulkan` for linux),
  `docker/linux.Dockerfile`

Touches additively: `engine/CMakeLists.txt` (3.2.3, source list 3.2.6),
`engine/renders/sokol/SokolTexture.cpp` (3 guards), `CopyEditorEngine.cmake` and root
`CMakeLists.txt` install (exclude `engine/tests`), `engine/workspaces/xcode/.../project.pbxproj`
(rmlui targets, header paths, all files of 3.2.6).

Not touched: `Scene.cpp`, `EntityRegistry.*`, `RenderSystem.cpp`, `Engine.cpp` (wave 2).

Deliverables and tests: `RmlCoreSmokeTest` (Initialise with a test font engine and
`RecordingRenderInterface`, `LoadDocumentFromMemory`, `Context::Update/Render` produce
geometry, Shutdown clean), `RmlSystemTest` (JoinPath table, log routing and counters),
`RmlFileTest` (open/read/seek/tell/length on fixtures, missing file -> 0,
`asset://` and relative paths, `LoadFile`), `HtmlUILifecycleTest` (acquire/release
refcount, re-initialise after full release), `ImageCompareTest` (identical, within
tolerance, beyond tolerance writes actual/diff, missing golden fails, update mode),
`HarnessGpuTest` (golden `harness_clear` = background only; golden
`harness_component_ui` = an existing component-UI `Image` with a color at a known canvas
rect, proving orientation, readback and compare on glcore and vulkan; golden
`harness_component_ui_texture` = the same with `checker.png`, proving image views and
samplers on lavapipe). Builds: linux
editor (glcore), engine sample, web, android, htmlui-tests glcore + vulkan.

#### p0-svg-bench

Goal: section 3.9. Owns `tools/htmlui-bench/**`, `docs/html-ui/svg-benchmark.md`. No
engine dependency. Deliverables: the bench, the corpus/reference scripts, native and wasm
results, the report with the decision. Tests: the bench's `--selftest` mode (renders
three known SVGs with each backend and checks output non-empty and within tolerance of
each other), run in the report's reproduction section.

### Wave 2 (after p0-foundation is merged)

#### p0-render

Goal: sections 3.4 and 3.5 (except the non-render parts of `HtmlUIContext`).

Owns: `RmlRenderer.{h,cpp}`, `HtmlUIContextRender.cpp`, `engine/shaderlib/htmlui.{vert,frag}`,
`engine/tests/htmlui/TestRender*.cpp`, `golden/render_*.png`,
`engine/tests/htmlui/fixtures/render/**`.

Touches additively: `Render.h` (BlendState, `HTMLUI`), `ObjectRender.{h,cpp}`,
`SokolObject.{h,cpp}`, `SystemRender.{h,cpp}` (`getFrameCount`), `ShaderVariantCompiler.cpp`,
`ShaderPool.{h,cpp}`, `CoreClassesLua.cpp`, `RenderSystem.cpp` (include + the three hooks of
3.5.5), `editor/window/dialog/ExportWindow.cpp`, `editor/shader/ShaderVariantCollector.cpp`,
`editor/Project.cpp` (shader key lines only; both ask
`EntityRegistry::isComponentRegistered<UIDocumentComponent>()` first, a new accessor in
`EntityRegistry.h`/`ComponentManager.h`, so saving never throws whatever the merge order of
p0-render and p0-ecs), `Scene.cpp` (the `UIDocumentSystem`
registration line of p0-ecs, identical, so the hooks resolve before p0-ecs lands),
`engine/CMakeLists.txt` (the test build includes SPIRV-Cross's HLSL and MSL back ends for
`HtmlUIShaderTest`; a standalone game build keeps only its own).

Dependencies: uses `HtmlUI` (final from p0-foundation) and the skeleton
`RmlFontEngine` (no text in its fixtures). Its tests do not depend on p0-ecs: a test
helper `RenderTestContext` (`engine/tests/htmlui/TestRenderCommon.h`) creates an
`Rml::Context` with an `RmlRenderer` and its own `FramebufferRender` target, and
`renderFrame()` runs exactly the sequence of `HtmlUIContext::render` (3.5.3):
`RuntimeShaderCompiler::beginFrame()` (what `Engine::systemDraw` does: without it the main
thread compiles only one variant ever), `beginFrame`, `Context::Update`, `Context::Render`,
`endFrame`, then `drawComposite` of that layer into an opaque #1E1E1E "screen" target, then
`SystemRender::commit()` (one sokol frame per call). Goldens read the screen back (so they
are opaque and also cover the composite); tests read the layer for premultiplied values.
`HtmlUIContextRender.cpp` itself is that same thin sequence plus the composite; it is
exercised end to end by the wave-3 goldens.

Tests: see 5.1/5.2 (render unit tests, renderer-level goldens without text). Builds:
linux glcore + vulkan editor, htmlui-tests glcore + vulkan, web, android.

#### p0-font

Goal: section 3.6.

Owns: `engine/core/util/{FontFace,FontRegistry,TextShaper,GlyphAtlas,STBText}.{h,cpp}`,
`RmlFontEngine.{h,cpp}`, `engine/core/pool/FontPool.*` (only if needed),
`engine/tests/htmlui/TestFont*.cpp`, `TestSTBText*.cpp`, `fixtures/fonts/**` (a static
TTF with OFL license for bold/italic matching tests, e.g. Roboto Bold and Italic from
`googlefonts/roboto` with the OFL text next to them).

Dependencies: none beyond p0-foundation. Font tests are CPU-only
(`RecordingRenderInterface` captures generated textures and meshes).

First commit: `STBTextCharacterizationTest` recorded on the unrefactored STBText.

Tests: 5.1 font list. Also runs `tools/mcp/run-tests.sh tools/mcp/tests/test_ui_anchors.py`
and `test_protocol.py` (STBText users unchanged). Builds: linux, web, android,
htmlui-tests glcore.

#### p0-ecs

Goal: section 3.7.

Owns: `UIDocumentComponent.h`, `UIDocumentSystem.{h,cpp}`, `HtmlUIContext.{h,cpp}`
(everything except `HtmlUIContextRender.cpp`), `UIDocument.{h,cpp}`, `UIElement.{h,cpp}`,
`engine/tests/htmlui/TestDocument*.cpp`, `TestInput*.cpp`, `fixtures/ecs/**`,
`tools/mcp/tests/test_html_ui_component.py`.

Touches additively: `Scene.cpp` (include + registration line), `EntityRegistry.{h,cpp}`,
`JasperEngine.h`, `EntityCloner.cpp`, `Engine.cpp` (3.7.3), editor files of 3.7.5 rows
C1-C8, S1, P1, D1, I1, F1, M1, M2, A1-A3.

Dependencies: p0-foundation. Document loading, layout, z-order and input are tested
without rendering (context `update()` only; no `render()` calls), so the skeleton
renderer is enough. `HtmlUIContext::render/composite` arrive with p0-render.

Tests: 5.1 ECS list; `test_html_ui_component.py` (5.3). Builds: linux glcore editor +
engine sample, htmlui-tests glcore, web, android.

### Wave 3 (after p0-render, p0-font, p0-ecs are merged)

#### p0-integrate

Goal: prove the whole path and close the phase.

Owns: `engine/tests/htmlui/TestGolden*.cpp`, `golden/*.png` except `harness_*` and
`render_*`, `fixtures/golden/**`, `tools/mcp/tests/test_html_ui_basic.py`,
`docs/mcp/guide/html-ui.md` (+ its registration where the guide docs are listed; follow
`docs/mcp/guide/shaders.md`).

May touch any file for integration fixes (small, each listed in the report).

Deliverables: full-path goldens (5.2 G list) on glcore and vulkan; MCP visual test; Web
headless-browser check; exported-game smoke (5.4); the guide (what phase 0 supports:
component, file location, RCSS syntax for now, built-in fonts, `HtmlUI::loadFontFace`,
known limitations from section 2). Runs the whole acceptance checklist (section 6).

## 5. Test plan

Commands: `tools/build.sh htmlui-tests` (glcore) and `HTMLUI_BACKEND=vulkan
tools/build.sh htmlui-tests`; MCP: `tools/mcp/run-tests.sh tools/mcp/tests/<file>`.

### 5.1 Unit tests (googletest)

Foundation (p0-foundation):
- `RmlCoreSmokeTest.InitialiseLoadRenderShutdown`, `.InitialiseFailsWithoutFontEngineIsReported`.
- `RmlSystemTest.JoinPathRelative` (`ui/menu.html` + `menu.css` = `ui/menu.css`),
  `.JoinPathParent` (`ui/a/b.html` + `../c.css` = `ui/c.css`), `.JoinPathScheme`
  (`asset://ui/m.html` + `x.png` = `asset://ui/x.png`), `.JoinPathAbsoluteAndUrl`,
  `.JoinPathPipeRestored`, `.LogRoutingAndCounters`.
- `RmlFileTest.OpenReadSeekTellLength`, `.MissingFileReturnsZero`, `.LoadFile`,
  `.AssetScheme`, `.SeekOutOfRangeFails`.
- `HtmlUILifecycleTest.RefCount`, `.ReinitialiseAfterRelease`,
  `.LoadFontFaceMissingFileFails`.
- `ImageCompareTest.*` (identical, tolerance, failure artifacts, missing golden, update mode).
- `HarnessGpuTest.Clear`, `.ComponentUIImage`, `.ComponentUITexture` (goldens `harness_clear`,
  `harness_component_ui`, `harness_component_ui_texture`: a component-UI `Image` with
  `common/checker.png`, proving textured draws on both backends).

Render (p0-render):
- `BlendStateTest.DefaultUnchanged` (every pipeline type's `describePipeline` equals the
  pre-change values, recorded in the test), `.PremultipliedOverride` (color pipelines get
  ONE/ONE_MINUS_SRC_ALPHA on rgb and alpha; depth/shadow/gbuffer/zprepass untouched),
  `.OpaqueDisablesBlend`.
- `HtmlUIShaderTest.CompilesAllBackends`: `ShaderVariantCompiler::compile(getShaderKey(HTMLUI, p))`
  for `p` in {0, 1} and backends GLCore, GLES3, HLSL, MSL, SPIRV: success; reflection has
  `a_position`, `a_color`, (`p=1`) `a_texcoord1`, block `u_vs_htmluiParams` of 80 bytes,
  (`p=1`) texture `u_htmluiTexture` with sampler pair. `.ShaderPoolNames`: token
  `htmlui` parses, `getShaderTypeName`, property name `Tex`.
- `RmlRendererTest.ScissorConversion` (table: identity, scaled 2x, GL flipped vs not,
  clamping, empty rect), `.GeometryLifetime` (compile/release counts), `.RenderOutsideFrameIgnored`,
  `.GeometryLifetimeThroughContext` (a document's geometry goes back on unload),
  `.LoadTexturePremultiplies` (alpha50.png drawn 1:1 into the layer, read back: (128,0,0,128)
  ±1; a sampled texture cannot be read back on Vulkan, only attachments carry TRANSFER_SRC),
  `.LoadTextureMissingReturnsZero` (+ one error logged), `.LoadTextureSvg`,
  `.GenerateTexture`, `.ReleaseDuringFrameIsDeferred` (texture alive until endFrame,
  destroyed after), `.BufferGrowth` (5000 elements: capacity grows, frame correct),
  `.WaitsForShaders` (endFrame false until pipelines exist, no crash),
  `.NoLeaksAfterContextDestroy` (live geometry/texture counts 0, sokol buffer/image
  handles invalid), `.ViewLossAndRecovery` (`Rml::ReleaseTextures` + `releaseGpuResources`
  then render again produces the same golden), `.DeadTextureHandlesAreFreed` (a dead
  handle is skipped by draws and freed by `ReleaseTexture`). `BlendStateTest
  .CustomFactorsAndOperations` (every factor/operation maps to sokol's).
  `RmlRendererGoldenTest.*` (one per golden of 5.2, each also probing the pixels its
  fixture defines, no `[HTML UI]` warning or error logged) and `.ResolutionScaling` (640x360
  document into a 1280x720 target: sharp edges and scissor at 2x).

Font (p0-font):
- `STBTextCharacterizationTest.LatinLine`, `.ArabicShapingBidi`, `.MultilineWrap`,
  `.FallbackChain`, `.AtlasGrowth` (width, height, quad count, char positions and the
  atlas version sequence equal to the recorded pre-refactor values).
- `FontFaceTest.RobotoMetadata` (family "Roboto", weight 400, not italic, upem 2048,
  no post table), `.ArabicCoverage`, `.CollectionIndex` (out-of-range index fails),
  `.InvalidDataFails`.
- `FontRegistryTest.NearestWeightRules` (each CSS rule with registered 300/400/700
  faces), `.ItalicPreference`, `.GenericFamilies`, `.CaseAndQuotes`.
- `TextShaperTest.KerningToggle` ("AV" narrower with kerning), `.LetterSpacing`
  (n glyphs -> +n*ls), `.RtlOrder`, `.ArabicJoins` (contextual forms differ from the isolated glyphs; the
  built-in Noto Sans Arabic subset has no lam-alef ligature, so the ligature check uses
  Roboto's "fi"; was: fewer glyphs than codepoints for a
  ligature-forming word), `.MatchesSTBTextShaping`.
- `GlyphAtlasTest.PackNoOverlap`, `.FullReturnsFalse`.
- `RmlFontEngineTest.HandleCaching`, `.FamilyListSplitAndFallback` ("Missing, sans-serif":
  valid, warning once), `.EmptyFamilyUsesDefault`, `.MetricsSane` (ascent > 0,
  descent > 0, line_spacing >= ascent + descent, 0 < x_height < ascent,
  underline_thickness >= 1, has_ellipsis), `.EmSizeNotPixelHeight` (size 100: ascent +
  descent within the font's em ratio, not 100), `.StringWidthMatchesShaper`,
  `.PriorCharacterKerning`, `.LetterSpacing`, `.GenerateStringQuads` (quad count =
  inked glyphs, colour premultiplied * opacity, positions snapped, texture valid),
  `.VersionBumpsOnlyOnNewGlyphs`, `.RasterScaleChange` (version bump, quads same
  document size, bitmaps 2x), `.PagesAppendOnly` (old glyph UVs unchanged after new
  glyphs), `.PageOverflowNewPage`, `.LoadFontFaceFileAndMemory`,
  `.LoadFontFaceInvalidFails`, `.ReleaseFontResources`, `.NonBmpCodepoint` (tofu, no
  crash), `.EmptyString`, `.DebuggerFontLoads`.

ECS (p0-ecs), with `GpuEnvironment` (view loaded) but no rendering calls:
- `UIDocumentSystemTest.LazyNoContextWithoutDocuments`, `.LoadsOnUpdate`,
  `.MissingFileFailsWithError`, `.FilenameChangeReloads`, `.EmptyFilenameUnloads`,
  `.RemoveComponentUnloads`, `.DestroyEntityUnloads`, `.DisabledHides`,
  `.InactiveInHierarchyHides`, `.SortOrderZIndex` (z-index values and stacking order),
  `.FocusDoesNotReorder`, `.CanvasResizeUpdatesDimensions`, `.ReferenceSizeOverrides`,
  `.TwoScenesTwoContexts`, `.SceneDestroyReleasesHtmlUI` (refcount back to 0),
  `.ViewDestroyedAndReloaded`, `.DebuggerToggle` (no errors logged).
- `UIDocumentTest.LoadNowAndGetElementById`, `.ShowHide`, `.ReloadKeepsSortOrder`.
- `UIElementTest.TextContent`, `.InnerHTML`, `.Attributes`, `.Classes`,
  `.StyleProperty` (rejected value returns false + warning), `.BoundingBox` (matches
  fixture layout), `.InvalidAfterUnload`.
- `DocumentInputTest.HoverAndActive` (Engine::systemMouseMove/Down/Up at screen coords:
  `:hover`, `:active` set/cleared on the button), `.ConsumedOverElementNotOverBody`
  (`Engine::onMouseDown` callback not called over the button, called over empty body),
  `.PointerEventsNoneIsClickThrough`, `.UpAfterConsumedDownIsConsumed`,
  `.TouchPointer0`, `.DisabledSceneEventsIgnored`, `.ReferenceSizeCoordinateMapping`.
- `UIDocumentClonerTest.InstantiateCopiesFields`.

### 5.2 Golden images

All documents in `engine/tests/htmlui/fixtures/`, rendered at 640x360 canvas = 640x360
screen unless stated, background `#1E1E1E`.

Renderer-level (p0-render, `golden/render_*.png`, `RenderTestContext`, no text):
| Name | Fixture content |
|---|---|
| `render_boxes` | backgrounds, per-side border widths and colors, padding/margin, rounded borders (radius 0/8/24 px), nested boxes |
| `render_flex` | flex row/column, `justify-content` (start/center/space-between), `align-items`, `gap`, `flex-grow`, `flex-wrap` |
| `render_images` | `<img>` of `checker.png` at 1x and scaled 2.5x, `alpha50.png` over white (expect (255,128,128) ±2), `decorator: image(checker.png)`, `icon.svg` |
| `render_alpha` | overlapping `#FF000080`, `#0000FF80` boxes, `opacity: 0.5` subtree, composite over the background |
| `render_scissor` | `overflow: hidden` containers clipping children, nested clips, scrolled container (`SetScrollTop` from test code). The clipped children are in normal flow: RmlUi 6.3 clips only when the content overflows the element's scrollable area, and an absolutely positioned child does not add to it |
| `render_transform` | `transform: rotate(15deg)`, `scale(1.5)`, `translate`, perspective on a box (no clipping) |
| `render_many` | 5000 small boxes (buffer growth) |

Full path (p0-integrate, `golden/*.png`, scene + `UIDocument` + camera composite):
| Name | Fixture content |
|---|---|
| `orientation` | 100x50 red box at top-left (10,10), green at top-right, blue at bottom-left; catches Y flips in offscreen, composite and readback |
| `boxes_borders` | as `render_boxes` through the full path |
| `flexbox` | as `render_flex` plus a centered card layout |
| `text_basic` | Roboto 12/16/24/48 px, colors, `line-height`, `text-align` left/center/right, `letter-spacing`, wrapped paragraph in a 300 px box, `text-decoration: underline` |
| `text_fallback_rtl` | Latin, Cyrillic, Greek, Arabic paragraph with `direction: rtl`, mixed bidi line, `font-family: "Missing Font", sans-serif`, a codepoint no font covers |
| `text_loaded_font` | the fixture Roboto Bold/Italic loaded with `HtmlUI::loadFontFace`, `font-weight: bold`, `font-style: italic` |
| `images` | as `render_images` |
| `alpha_blend` | as `render_alpha` over a scene that also has a colored component-UI `Image` underneath (premultiplied composite) |
| `overflow_scissor` | as `render_scissor` including a scrolled list with text |
| `transform` | as `render_transform` with text |
| `hidpi` | `text_basic` with screen 1280x720, canvas 640x360 (pixel ratio 2, raster scale 2); golden at 1280x720 |
| `letterbox` | canvas 640x360, `Scaling::LETTERBOX`, screen 800x600: document inside the view rect, bars untouched |
| `sort_order_a`, `sort_order_b` | two documents in one scene overlapping; sortOrder 0/1, then swapped |
| `hover_states` | three captures of one button (normal, hover after `Engine::systemMouseMove`, active after `systemMouseDown`): `hover_normal`, `hover_hover`, `hover_active` |
| `component_ui_under` | component UI `Image` + `Text` under a document: the document is in front |
| `two_scenes` | main scene + a layer scene each with a document (scene-layer order) |
| `debugger` | the RmlUi debugger visible on a document (proves its font loads); compared with a looser threshold (it shows live values) |

Every golden passes on glcore and vulkan against the same PNG (generated on glcore).
Where a backend differs beyond tolerance, fix the cause (half-pixel offset, flip,
filtering), do not add per-backend goldens.

### 5.3 MCP end-to-end tests

`tools/mcp/tests/test_html_ui_component.py` (p0-ecs; non-visual):
- `add_component` `UIDocument` with `{"filename": "assets/ui/basic.html", "sortOrder": 2}` (MCP
  paths are project-relative; the fixture's assets are in `assets/`, the component stores
  the asset-relative `ui/basic.html`) on a
  new entity; `get_entity` returns both values and `enabled: true`;
  `list_component_types` lists `UIDocument` with its properties.
- `set_properties` `filename` to a path outside the project -> MCP error (path validation).
- `save_scene`, `close_scene`, `open_scene`: values preserved (YAML key `uidocument`).
- Copy/paste entity and prefab round trip keep the values.
- `write_file` accepts `ui/basic.html` and `ui/basic.css`.
- No console errors other than the expected ones.

`tools/mcp/tests/test_html_ui_basic.py` (p0-integrate; visual, modelled on
`test_ui_anchors.py`: `new_scene`, `screenshot` with `view: "game"`, `decode_png`,
`canvas_rect_in_image`):
1. `create_scene` type UI; `write_file` `ui/basic.html` + `ui/basic.css` (body
   transparent; red box `#FF0000` at canvas (40,40) 200x100; blue box at (400,200)
   160x80; a text line; a button 160x48 at (40,240) with `:hover` color `#00FF00` and
   normal `#808080`).
2. `create_entity` + `add_component` `UIDocument` `filename: ui/basic.html`.
3. Screenshot: red and blue box centers have their colors (±12), text area has non-background
   pixels, background elsewhere; console has no `[HTML UI]` errors.
4. `set_properties` `enabled: false` -> boxes gone; back to true -> visible.
5. Second document with `sortOrder: 1` overlapping the red box -> on top; swap -> under.
6. `save_scene`, reopen, screenshot equals step 3 (same pixels at the probes).
7. `play`, `send_input` mouse move to the button center (`space: canvas`), screenshot ->
   button `#00FF00`; move away -> `#808080`; `stop`.
8. `filename: ui/missing.html` -> console error naming the path, no crash, next
   screenshot shows no document.
9. Always also run `test_protocol.py` and `test_ui_anchors.py`.

### 5.4 Builds and platform checks

| Target | Command | Who | Check |
|---|---|---|---|
| Linux glcore editor + engine sample | `JOBS=3 tools/build.sh linux` | every package | builds, zero new warnings in our code |
| Linux vulkan editor | `GRAPHIC_BACKEND=vulkan JOBS=3 tools/build.sh linux` | p0-foundation, p0-render, p0-integrate | builds; MCP visual test passes with this editor |
| htmlui-tests glcore | `JOBS=3 tools/build.sh htmlui-tests` | all wave 1-3 C++ packages | all tests pass |
| htmlui-tests vulkan | `HTMLUI_BACKEND=vulkan JOBS=3 tools/build.sh htmlui-tests` | p0-foundation, p0-render, p0-integrate | all tests pass |
| Web | `JOBS=3 tools/build.sh web` | every package | builds |
| Web runtime | p0-integrate: export the MCP test project for Web with the editor's exporter (or build `engine/` with that project as `PROJECT_ROOT` in the web image), serve it with `python3 -m http.server`, load it in headless Chrome through playwright on the host, wait for frames, screenshot | p0-integrate | document visible, probe colors as in 5.3 step 3 |
| Android | `JOBS=3 tools/build.sh android` | every package | APK builds (no device or emulator here) |
| Exported Linux game | `tools/smoke-test.sh <project saved by test_html_ui_basic.py with JASPER_KEEP_SCRATCH=1>` | p0-integrate | `build/smoke/game.png` shows the document; no errors in the game log (missing shaders included) |
| Windows, macOS, iOS | cannot be built on this host | p0-foundation wires CMake and Xcode by reading the existing sections | reviewed diff only |

## 6. Acceptance criteria

All must hold before phase 0 is merged into `main`:

- [ ] `engine/libs/rmlui` contains exactly the subset of 3.2.1; `FORK.md` exists with
      upstream URL, tag, commit, subset, removals and an empty patch table.
- [ ] `rmlui` is static, `RMLUI_STATIC_LIB` public, hidden visibility on non-MSVC,
      `-w`, linked into `jasperengine`; `nm -D -C build/linux/editor-install/bin/libjasperengine.so | grep -c 'Rml::'` is 0
      (demangled: the mangled `nm -D` output never contains ` Rml::`).
- [ ] `engine/tests` is excluded from the editor install; exports never contain it.
- [ ] Xcode workspace has `rmlui-ios`/`rmlui-macos` targets, header paths and every
      file of 3.2.6 (diff reviewed).
- [ ] Linux glcore and vulkan editors, engine sample, Web and Android build with zero new
      warnings in non-vendored code.
- [ ] `jasperengine-htmlui-tests` passes completely on glcore and on vulkan in the Docker
      image, including every golden of 5.2; every golden PNG has been looked at.
- [ ] Renderer leak tests pass: no live geometry/textures and no sokol handles after
      context destroy and after scene reload.
- [ ] Existing behavior unchanged: STBText characterization tests pass;
      `test_protocol.py`, `test_ui_anchors.py` and the other existing MCP tests pass with
      the glcore editor; `BlendStateTest.DefaultUnchanged` passes.
- [ ] `test_html_ui_component.py` and `test_html_ui_basic.py` pass (glcore editor), and
      `test_html_ui_basic.py` also passes with the vulkan editor.
- [ ] Web build shows the document in headless Chrome; the exported Linux game shows it
      with no missing-shader errors.
- [ ] A document's pointer events: `:hover`/`:active` work in play mode and the game does
      not receive clicks over document elements, but does over the transparent body.
- [ ] `docs/html-ui/svg-benchmark.md` contains native and wasm results for all three
      libraries and a decision that follows the rule of 3.9.4.
- [ ] `docs/mcp/guide/html-ui.md` describes the phase-0 feature set and limitations and
      is reachable through the MCP docs resources (`test_docs.py` passes).
- [ ] Every package report lists files changed outside its ownership; no TODO stubs left
      from the skeletons (grep the skeleton markers p0-foundation used, e.g.
      `HTMLUI_SKELETON`, returns nothing).
