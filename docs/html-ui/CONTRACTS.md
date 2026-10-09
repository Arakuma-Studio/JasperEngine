# HTML/CSS UI: cross-phase contracts

Every interface one phase provides and a later phase consumes, with the name the provider
uses. Implementers check their code against this table: a consumer uses these names; a
provider keeps them (or keeps the old name working and reports the change). The specs are
the detail; section references are to `phase-N-*.md` (`P0` … `P5`). Packages are the work
packages of each spec (`p0-foundation`, `p1-render`, …). "Consumers" lists phases/packages
that call or extend the item.

Conventions that apply everywhere:

| Convention | Rule | Ref |
|---|---|---|
| Work branches | `hui/<slug>` from `html-ui`, worktrees `…/JasperEngine-2-wt/<slug>` | BRIEF, every §4 |
| Paths | Engine/compat paths are **asset-relative** (`ui/menu.html`, `FileData` rules, the component's `filename`); editor CLI/MCP output is **project-relative** (`assets/ui/menu.html`) and converts at the boundary | P1 §3.8.6, P2 §3.4, P4 §3.2.1 |
| Log prefix | Every HTML UI Console line starts with `[HTML UI] ` (RmlUi messages, compat diagnostics, importers) | P0 §3.3.2, P2 §3.4 |
| Diagnostic codes | `HTMLUI-<AREA>-NNNN` + kebab-case registry name; areas `GEN HTML CSS SEL AT IMP SVG` (P2), `FX` (P5); never renumbered; new codes take the next free number of their area | P2 §3.4, App. F; P5 §3.13 |
| Diagnostic text | `path:line:col: severity: message (fix: suggestion) [HTMLUI-CSS-0004]` (Console, `check --ui`, MCP); JSON/MCP `code` = the id | P2 §3.4, P4 §3.4.1 |
| Golden update | `JASPER_HTMLUI_UPDATE_GOLDEN=1` (C++ harness on glcore, phase-2 text goldens, phase-4 MCP goldens) | P0 §3.8.2, P2 §5.1, P4 §3.9 |
| Golden layout | `engine/tests/htmlui/golden/`: P0 top level (`harness_*`, `render_*`, full-path names), `p1/`, `compat/` (P2: `reference/`, `engine/`), `browser/` (P3), `examples/` (P4), `effects/`, `property/` (P5) | each §5 |
| Test files | Top-level `engine/tests/htmlui/*.cpp` are globbed; a subdirectory (`compat/`, `anime/`, `effects/`, `property/`) needs one glob line in `engine/tests/htmlui/CMakeLists.txt` (P0's glob is not recursive) | P0 §3.8.1 |
| Docs | `docs/mcp/guide/html-ui.md`: created by p0-integrate, sections added by P1–P3 packages, reorganised by p4-guide, "Effects" section by p5-infra | P0 §4, P4 §3.8.1, P5 §3.13 |

## 1. Build, vendoring, test infrastructure

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `engine/libs/rmlui/` (RmlUi 6.3 Core + Debugger; P1: the Debugger only with `JASPERENGINE_HTMLUI_DEBUGGER`), target `rmlui` (static, `RMLUI_STATIC_LIB`, hidden visibility, `RMLUI_NO_FILE_INTERFACE_DEFAULT`; P1: `RMLUI_SVG_PLUGIN`, links `lunasvg` privately) | vendored lib | P0 p0-foundation | all | P0 §3.2 |
| RmlUi SVG plugin (`Source/SVG`, `Include/RmlUi/SVG`), `RMLUI_SVG_PLUGIN` (upstream, on LunaSVG; registered by `Rml::Initialise`) | vendored lib | P1 p1-vector | P2 H8, P3 §3.9 | P1 §3.10.1, §3.10.5 |
| RmlUi `Tests/` (fork unit tests) | vendored lib | P3 p3-testkit | P3 all | P3 §4, §5.1 |
| `engine/libs/thorvg/` (1.2.0, `thorvg`, `TVG_STATIC`; CPU engine, loaders lottie (no expressions) and png/jpg/raw/sfnt for Lottie assets and text, no SVG loader), `ThorVGRuntime::acquire/release` | vendored lib + helper | P1 p1-vector | P3 | P1 §3.10.1, §3.10.5 |
| `engine/libs/lunasvg/` (3.5.0 + plutovg 1.3.3, `lunasvg`, `LUNASVG_BUILD_STATIC`), `LunaSVGRuntime::ensureFonts/rasterize` (`engine/core/util/`) | vendored lib + helper | P1 p1-vector | P2 H8 (`rasterizeSvg`, `html-shape`) | P1 §3.10.5 |
| `engine/libs/lexbor/` (3.1.0 = master `f4cbbcd`, subset + offset patch, `PATCHES.md`; target `lexbor`, static, hidden visibility, `--exclude-libs,liblexbor.a`; Xcode `lexbor-ios/-macos`) | vendored lib | P2 p2-html | P2 | P2 §3.7.1 |
| `engine/libs/lucide/lucide.pack` | data | P2 p2-runtime | P2, P4 `import_files icons` | P2 §3.11.8 |
| `docs/html-ui/FORK.md` (vendored subset, patch table) | doc | P0 p0-foundation | P1, P2 (H8), P3, P5 append rows | P0 §3.2.5 |
| `JASPERENGINE_HTMLUI_TESTS` option, target `jasperengine-htmlui-tests` (standalone engine only); it sets the compile definition `JASPERENGINE_TEXTURE_READBACK` (sampled Vulkan images readable) | CMake | P0 p0-foundation | P1–P5 tests | P0 §3.2.3, §3.8.1 |
| `tools/build.sh htmlui-tests` (`HTMLUI_BACKEND=glcore\|vulkan`, `HTMLUI_TEST_ARGS`, `RUN=0`, `BUILD_TYPE`); `GRAPHIC_BACKEND=vulkan tools/build.sh linux` → `build/linux/editor-vulkan(-install)` | build script | P0 p0-foundation | P1–P5 | P0 §3.8.3 |
| `docker/linux.Dockerfile`: `mesa-vulkan-drivers vulkan-tools` (lavapipe), Vulkan-Headers v1.3.296 in `/usr/local` (22.04's 1.3.204 lacks `VK_EXT_descriptor_buffer`) | Docker | P0 p0-foundation | all Vulkan runs (no later phase changes Docker) | P0 §3.8.3 |
| `engine/libs/sokol/sokol_gfx.h` `_SG_VK_MAX_DESCRIPTOR_DATA_SIZE` 320 (was 256: the image's lavapipe needs 280-byte descriptors) | vendored lib | P0 p0-foundation | all Vulkan runs | P0 §3.8.3 |
| `JASPERENGINE_INTERNAL` (`engine/core/Export.h`, hidden visibility) on `RmlSystem`, `RmlFile`, `RmlFontEngine`, `RmlRenderer`, `HtmlUIContext` (P1 also `RmlRenderTarget`, `RmlFilterContext`, `RmlFontEffects`, `RmlKeyMap`, `RmlTextInputHandler`, `UILottieElement`, `AnimProperty`); the shared engine links `librmlui.a`, `liblunasvg.a`, `libthorvg.a` with `--exclude-libs` and `engine/libs/rmlui/hidden-symbols.map` (`Rml::`, `tvg::`, `lunasvg::` hidden); `tools/build.sh linux` fails if `nm -D` shows an `Rml::` symbol | C++ / CMake | P0 p0-foundation | every class in `engine/core/htmlui/` that derives from or holds RmlUi types | P0 §3.2.2 |
| Harness (namespace `jasperengine::test`, `engine/tests/htmlui/harness/`): `GpuEnvironment::ensure()` (bool; `JASPER_REQUIRE_GPU()`) `/setView/runFrames/drawFrames/target`, `SceneFixture` (`addDocument`, `add<T>()`, `waitUntilLoaded`), `readTexture` (`Readback.h`), `compareWithGolden`/`compareWithGoldenFile`/`compareTargetWithGolden`/`EXPECT_GOLDEN`/`EXPECT_GOLDEN_OPTIONS`/`CompareOptions`, `RecordingRenderInterface`, `TestSystem` (`get()`, asset root `engine/tests/htmlui/fixtures/`, log capture `countLogs`); shared fixtures in `fixtures/common/` (`base.css`, `checker.png`, `alpha50.png`, `icon.svg`) | test API | P0 p0-foundation | P1–P5 | P0 §3.8.2 |
| `RenderTestContext` (`engine/tests/htmlui/TestRenderCommon.h`: an `Rml::Context` on an `RmlRenderer` with its own `RmlRenderTarget target` (P1: was a `FramebufferRender`; constructor's 5th argument = MSAA sample count, default 1), `renderFrame` = `HtmlUIContext::render` sequence + composite over a #1E1E1E screen + commit, `renderUntilDrawn` (P1: 30 frames, was 10: the UI variants compile one per frame), `readLayer/readScreen/compareScreen`), `PixelProbe`, `beginTestFrame()` (runtime shader compiler frame) | test API | P0 p0-render; P1 p1-render | P1 renderer tests | P0 §4 p0-render |
| `ImageCompare` creating output subdirectories for names with `/` | test API | P0 p0-foundation (`writePng` creates parent directories of goldens and outputs) | P1, P5 | P1 §4 p1-render |
| `jasperengine-htmlui-editor-tests` (option `JASPERENGINE_HTMLUI_EDITOR_TESTS`), `tools/build.sh htmlui-editor-tests` | test target | P2 p2-importers | P2 | P2 §4, §5.6 |
| `tools/htmlui/render-reference.mjs` + `tools/htmlui/package.json` (playwright-core 1.63.0, Chromium headless shell 153 rev 1243; host only) | reference tool | P2 p2-ua | P3 p3-testkit extends (`--fixtures`, `--boxes-only`) | P2 §5.3, P3 §5.2 |
| `BrowserCompare` (no-text fuzzy-bad/max-block gates) + `compareStrict` | test API | P2 p2-ua; strict metric P3 p3-testkit | P2, P3 | P2 §5.3, P3 §5.2 |
| `tools/htmlui/run-rmlui-tests.sh` (host, standalone fork tests) | script | P3 p3-testkit | P3, P5 fork patches | P3 §5.1 |
| `tools/mcp/pngdiff.py` | test lib | P4 p4-examples | P4, P5 | P4 §3.9 |

## 2. Engine core

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `HtmlUI::acquire/release/isInitialized/getReferenceCount`, `loadFontFace`, `getSystemInterface/getFileInterface/getFontEngine`, `registerContext/unregisterContext`, `setDebuggerContext` | C++ (internal) | P0 p0-foundation | P1 (ThorVG, SVG plugin, `lottie` instancer after `Rml::Initialise`), P2 H1 (`registerRuntime`), P5 C6 (`EffectRegistry::initialise`, plugin) | P0 §3.3.1 |
| `RmlSystem` (`LogMessage` with `[HTML UI] `, `JoinPath`, log listener/counters) | C++ | P0 p0-foundation | P1 (clipboard, `ActivateKeyboard`, `SetMouseCursor`, `GetElapsedTime` → clock), P2 H3 (`RmlLogTranslator` first) | P0 §3.3.2 |
| `RmlFile` (`Data::open`) | C++ | P0 p0-foundation | P1 (runtime file log), P2 H3b (`@htmlui/` virtual files) | P0 §3.3.3 |
| static `Engine::getSceneLayers() -> std::vector<Scene*>` (running scenes, main scene and layers, bottom first), static `SceneManager::getScenePtrs() -> std::vector<Scene*>` (every scene registered with `setScenePtr`, by id, also while its scripts' `init` runs) | C++ | P1 p1-integrate | `AnimTargets` selectors, P4 inspectors | P1 §3.15 |
| `HtmlUIClock::now/advance/setFixedTime/getFixedTime/reset` (`engine/core/htmlui/HtmlUIClock.h`); `Engine::systemDraw` advances it (the frame delta before `timeScale`: the fixed delta with `setFixedDeltatime`, else the measured one capped by `maxDeltatime`; 0 while held or paused; editor edit mode goes through `systemDraw` too); `advance` is a no-op while a fixed time is set, and clearing it resumes from the fixed value | C++ | P1 wave-0 commit | P1 (RmlSystem, Lottie, `anime`), P3 FixtureRunner `advance`, P4 goldens, P5 `EffectClock` | P1 §3.0 item 6 |

## 3. Render abstraction and renderer

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `BlendFactor`, `BlendOperation`, `BlendState{premultipliedAlpha(), opaque()}`, `ObjectRender::setBlendState`, `SokolObject::describePipeline` | C++ | P0 p0-render | P1 (composite) | P0 §3.4.1, §3.4.4 |
| `SystemRender::getFrameCount()` (sokol frames committed) | C++ | P0 p0-render | P1 (once-per-frame work) | P0 §3.5.2 |
| `PipelineState`, `BlendMode{NONE, ALPHA, PREMULTIPLIED, ADDITIVE_PREMULTIPLIED, SCREEN_PREMULTIPLIED, MULTIPLY_PREMULTIPLIED}`, `StencilState`, `RenderTargetFormat`, `targetFormatFor` (`engine/core/render/PipelineState.h`, also `LoadAction`/`StoreAction`), `ObjectRender::endLoadDynamic(PrimitiveType, IndexType)/beginDraw(const PipelineState&)`, `ObjectRender::setIndexBuffer(buffer, byteOffset)`, `BufferRender::appendBuffer` | C++ | P1 p1-render | P5 (effect blends) | P1 §3.1.1–3.1.2 |
| `SokolPipelineCache::get/releaseShader/clear/size` (+ `makeDesc`, `apply`, `getMissCount/getApplyCount`; `SystemRender::getPipelineCacheMisses/getPipelineApplies/getPipelineCacheSize`) | C++ | P1 p1-render | P5 C2 | P1 §3.1.3 |
| `FramebufferDesc`, `FramebufferRender::createFramebuffer(desc)`, `getSampleCount`, `Framebuffer::create(desc)`; `CameraRender::setColorActions/setDepthStencilActions/applyScissorTopLeft/applyViewportTopLeft`; `SystemRender::getFeatures` (`RenderFeatures`) | C++ | P1 p1-render | P3 (`SetGeometryFeathering` from MSAA), P5 | P1 §3.1.4–3.1.5 |
| `TextureRender::createDynamicTexture(label, w, h, ColorFormat, TextureFilter)` (RGBA8/R8, stream update; `updateTexture` rejects a wrong size); test harness `readTexturePixels` (RGBA8/R8) | C++ | P1 wave-0 commit | P1 p1-vector (Lottie), P5 | P1 §3.0 item 2 |
| `HtmlUITexture{Kind STATIC/DYNAMIC/RENDER_TARGET/EXTERNAL, render, ownedRender, ownedTarget, width, height, dead, lastUpdateFrame}` + destructor, `createDynamic`, `update`/`canUpdate` (one update per frame) (`engine/core/htmlui/HtmlUITexture.h`) | C++ | P1 wave-0 commit | P1, P2 (callback textures), P5 | P1 §3.0 item 1 |
| `RmlRenderer` (P0: required methods, `SetTransform`, `drawComposite`, `toTargetScissor`, `hasFrame`, live counters, `getTextureRender`, `getGpuHandles`; P1: every optional method, `bool beginFrame(RmlRenderTarget&, contextSize)/bool endFrame()` (false: nothing drawn; changed from P0's `beginFrame(contextSize, targetSize)/endFrame(target, pass)`), `drawComposite(texture, pip, clipFromQuad)` (P0's `drawComposite(texture, pip, flipY)` kept), `takeTexturesWithoutView` (P1 p1-integrate: textures made before the view, P1 §3.15), `drawUnpremultiplied` (P1 p1-rtt), `getCurrentTransform/getCurrentScissor/isClipMaskEnabled/getStencilRef/getPixelScale/getGeometryState/bindTopLayer/bindGeometry`, `getStats`, `RmlCompiledFilter`/`RmlCompiledShader` behind the handles); `RmlRenderTarget` (layers, post-process targets) | C++ | P0 p0-render, rewritten by P1 p1-render | P2 H7 (`LoadTexture` suffix/virtual files), P3 (opacity filter), P5 C2/C4 (bit-0 handle branches) | P0 §3.5.2, P1 §3.2, §3.3.3 |
| `RmlFilterContext` (`primary/secondary/tertiary`, `swap`, `blit`, `blurInPlace`, `getScissor`, `getLayerSize`, `drawFullscreen(..., LoadAction load = CLEAR)`, `postState`, `getRenderer`, `isPrimaryLayer`) | C++ | P1 p1-render | P5 C3 | P1 §3.3.3 |
| Bit-0 handle rule for `CompileShader/CompileFilter` handles | convention | P1 p1-render | P5 §3.7 | P1 §3.3.3 |
| `HtmlUIRenderStats{passes, drawCalls, pipelineApplies, pipelineCacheMisses, …}` (`engine/core/htmlui/HtmlUIRenderStats.h`, wave 0), `UIDocumentSystem::getRenderStats` | C++ | P1 p1-render | P5 (`EffectStats::pipelinesCreated`), editor stats | P1 §3.2.8 |
| `HtmlUIContext` (P0: `create/destroy/loadDocument/update/processMouse*/isPointerOverContent/render/composite/releaseGpuResources/getTarget/getRenderer/getPixelRatio`; P1: `getRenderTarget() -> RmlRenderTarget*`, static `setPreferredSampleCount/getPreferredSampleCount`, `getSampleCount`, `markUpdated`, `composite(pip, clipFromQuad)`, `setWheelScale`, text input handler) | C++ | P0 p0-ecs + p0-render; P1 | P2 H1b (`loadDocument` → `DocumentLoader::load`), P5 C5/C6 (`render` hook before `Context::Render`, pointer hooks) | P0 §3.5.3, P1 §3.1.4, §3.5, §3.6 |
| `UIDocumentSystem::renderOffscreen(const Rect&)` (end of `RenderSystem::drawFramePasses` and in `drawView` for views without editor overlays; idempotent per sokol frame), `drawComposite(camera, cameraTransform, colorPip, flipY)` | C++ | P0 p0-render; P1 p1-render | — | P0 §3.5.5, P1 §3.5 |

## 4. Shaders

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `ShaderType::HTMLUI` (`htmlui.vert/.frag`, bit 0 `Tex`/`HAS_TEXTURE`, `u_vs_htmluiParams { mat4 transform; vec4 translation; }`, `u_htmluiTexture` + `u_htmlui_smp`, attributes `a_position`/`a_color`/`a_texcoord1`, stride 20), `ShaderPool::getHtmlUIProperties(bool)` | shader type | P0 p0-render | P1 (geometry, composite), P5 C4 (`effect_paint.vert`) | P0 §3.4.2–3.4.3 |
| `ShaderType::HTMLUI_GRADIENT`, `HTMLUI_POST` (`Pas Cmx Msk Shd Blr Unp`; tokens `htmluigradient`/`htmluipost`), `HtmlUIPostVariant{PASSTHROUGH, COLOR_MATRIX, BLEND_MASK, DROP_SHADOW, BLUR, UNPREMULTIPLY}` and `HTMLUI_POST_VARIANT_COUNT` (`ShaderPool.h`), `ShaderPool::getHtmlUIPostProperties`, `ShaderPool::getHtmlUIVariants` (all 9 UI variants) | shader types | P1 p1-render | P5 (prepass blur via `RmlFilterContext`) | P1 §3.3 |
| `ShaderType::UI_EFFECT` (appended after `HTMLUI_POST`; bit 0 `Flt`; identity in the custom-shader id) | shader type | P5 p5-infra | — | P5 §3.8 |
| Editor shader cache version bump (one per phase that adds types) | convention | P1, P5 | — | P1 §3.3.2 |

## 5. Text

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `FontFace` (`loadFile/loadMemory(..., std::string* error)`, logs nothing), `FontRegistry` (`registerFace`, `find`, `hasFamily`, `isGenericFamily`, `normalizeFamily`, built-ins, `getOrLoadFile`), `TextShaper` (`ShapeOptions`), `GlyphAtlas` | C++ | P0 p0-font | P1 (font effects, synthesis), P2 H6 (`hasFamily`), P3 (`emoji`/`math` generics, `word_spacing`) | P0 §3.6.2 |
| `RmlFontEngine` (comma-split family lists, glyph pages append-only, one global `GetVersion`, `setRasterScale`; tests: `getChain`, `splitFamilyList`, `getPageCount`) | C++ | P0 p0-font | P1 p1-rtt (effects, synthetic bold/italic, per-context `setRasterScale(RenderManager&, float)`, `reloadFontFile`), P2 H6 (`hasFamily`, `setRenderTextForTesting`), P3 (`word_spacing`, canonical family list) | P0 §3.6.4, P1 §3.13 |

## 6. ECS, component, system, GameObject API

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `UIDocumentComponent{filename, sortOrder, enabled; runtime needReload/loaded/loadFailed/appliedSortOrder/appliedVisible}` | component | P0 p0-ecs | all | P0 §3.7.1 |
| + `interactive, renderMode (UIDocumentRenderMode SCREEN/TEXTURE), textureWidth, textureHeight, inputSurface, pixelRatio, onLoad, onUnload` | component fields | P1 wave-0 / p1-editor | P4 (`create_ui_document`, document JSON), P5 | P1 §3.8.1 |
| Catalog `kUIDocumentProperties` (`filename`, `sortOrder`, `enabled` getter, + P1 fields), `UpdateFlags_UIDocument_Reload` (bit 37), name `UIDocumentComponent` (C++ type, `Catalog::getComponentName`) / `uidocument` (YAML, MCP) / "UI Document" (display), `ComponentType::UIDocumentComponent` | editor registration | P0 p0-ecs; P1 p1-editor | P4 | P0 §3.7.5, P1 §3.8.2 |
| `UIDocumentSystem` (P0: `update`, `loadDocumentNow` (P1 §3.15: also before the view loads), `isDocumentLoaded`, `getElementById`, `getDocumentBody`, `setReferenceSize`, `isAllLoaded`, `setDebuggerVisible`, `getContext`) | C++ | P0 p0-ecs | P1, P2, P4 | P0 §3.7.2 |
| `UIDocumentSystem::getDocumentFiles(Entity)`, `reloadDocument(Entity, bool preserveState = true)`, `reloadStyleSheets(Entity)`, `reloadTexture(const std::string& path)`, `getDocumentMessages(Entity) -> const std::vector<HtmlUIMessage>&` (`HtmlUIMessage{level (HtmlUIMessageLevel{Info, Warning, Error}), text, file, line}`), `getNextUpdateDelay()` (min over the screen and texture-mode contexts), `getDocument(Entity)` (defined by wave 0), `getLoadCount(Entity)`, `reloadChangedFiles(files) -> std::vector<Reloaded{entity, stylesheetOnly}>` (shared stamps) | C++ | P1 p1-editor | P2 H2, P4 (`w4::syncHtmlUi`, inspector), P5 (`EffectRegistry::onProjectFileChanged`) | P1 §3.8.6 |
| `UIDocumentSystem::getCompiledDocument(Entity)`, `getDiagnostics(Entity)` | C++ | P2 p2-html (H1b) | P4 inspector, MCP | P2 §3.16 |
| `UIDocumentSystem::getDataModelNames(Entity)`, `pushDataModel(lua_State*, Entity, name)` | C++ | P1 p1-lua | P4 | P1 §3.9.5 |
| `UIDocumentSystem::getScriptState()`; private hooks `scriptDocumentLoaded(Entity)` / `scriptDocumentUnloading(Entity, Rml::ElementDocument*)` (a reload path that bypasses `applyComponent`/`unloadEntityDocument` calls them: onLoad/onUnload, detached pool) and `HtmlUIScriptState::onDocumentReloaded(Entity, oldToNew)` | C++ (internal) | P1 p1-lua | P1 p1-editor | P1 §3.9.6 |
| static `UIDocumentSystem::setPreferredSampleCount(int)/getPreferredSampleCount()` (exported entry to the internal `HtmlUIContext::setPreferredSampleCount`) | C++ | P1 p1-render (declared, stubbed by wave 0) | P1 p1-editor (project setting, `Generator.cpp`) | P1 §3.0 item 4, §3.8.8 |
| `UIDocumentSystem::getTargetTexture(Entity)` (behind `UIDocument::getTargetTexture`) | C++ | P1 p1-rtt (declared, stubbed by wave 0) | P1 p1-lua (Lua binding) | P1 §3.0 item 4, §3.12.2 |
| `UIDocumentSystem::scriptDocumentReloaded(Entity, oldToNew)` (private hook of the full reload) | C++ | P1 p1-lua (declared, stubbed by wave 0) | P1 p1-editor | P1 §3.0 item 4, §3.8.6 |
| Per-package state `HtmlUIRenderState/InputState/ScriptState/ReloadState/TextureState` (namespace scope, friends of `UIDocumentSystem`) and their `UIDocumentSystem{Render,Input,Script,Reload,Texture}.cpp` (`HtmlUIScriptState` is declared in `HtmlUIScriptInternal.h`); `UIDocumentSystem::releaseScriptState()` (play stop: models removed, documents of every context unloaded at once) | C++ | P1 wave-0 commit (files), each P1 package (contents) | — | P1 §3.0 |
| `UIDocumentSystem::setDebuggerVisible -> bool`, `isDebuggerVisible`; option `JASPERENGINE_HTMLUI_DEBUGGER` | C++/CMake | P0 (plumbing), P1 p1-editor | — | P1 §3.8.7 |
| `UIDocument` (`engine/core/object/ui/`): P0 `setFilename/getFilename/load(filename)->bool/reload/isLoaded/show/hide/setVisible/isVisible/setSortOrder/getSortOrder/getElementById/getBody`; P1 `querySelector(All)`, `createElement`, `createTextNode`, `getTitle/setTitle`, `addEventListener -> uint64_t`/`removeEventListener(uint64_t)`, `createDataModel/getDataModel/removeDataModel`, `getTargetTexture`, static `setDebuggerVisible(bool)/isDebuggerVisible()`; getters load a due document (scripts' `init`, before the view too); P2 `getDiagnostics`, `setColorScheme`, `setReducedMotion`; P5 static `setEffectTime/getEffectTime` | C++ | P0 p0-ecs, P1 p1-lua, P2 p2-html (H4), P5 | P4, P5 | P0 §3.7.4, P1 §3.9.1, P2 §3.12.4, P5 §3.12 |
| `UIElement` (`engine/core/object/ui/UIElement.h`, handle `UIElementHandle`): P0 members kept, P1 DOM-named API (§3.9.1), Lottie control; P5 `getEffects()` | C++ | P0 p0-ecs, P1 p1-lua/p1-vector, P5 | P2 H5, P3 FixtureRunner, P4 | P0 §3.7.4, P1 §3.9.1 |
| `UIEvent`, `UIValue`, `UIDataModel`, `UIRect` (`engine/core/object/ui/`) | C++ | P1 p1-lua | P2, P4 | P1 §3.9.1 |
| `UIElementInternal.h`: `UIElementHandle` (moved from `UIElement.cpp`), `UIElementAccess::get(const UIElement&) -> Rml::Element*` / `wrap(Rml::Element*)` | C++ (internal) | P1 wave-0 commit; p1-lua extends the handle | P1 p1-anim, p1-vector, P2 H5, P3 FixtureRunner, P4 inspector | P1 §3.0 |
| `HtmlUIScriptInternal.h`: `HtmlUIElementRegistry` (handle tokens, detached pool), `HtmlUILua` (`pushValue`, `toValue`, `pushEvent`, `pushElement`, `pushModel`, `call`), `HtmlUILuaRef` (counted registry refs), `HtmlUIScriptState::liveLuaRefs`, `HtmlUIScriptAccess` (friend of `UIEvent`/`UIDataModel`/`UIValue`), `kHtmlUIDeadElementMessage`; `HtmlUIEventListener.h`: `HtmlUIEventListener`, `HtmlUIEventNames` | C++ (internal) | P1 p1-lua | P1 p1-anim (Lua refs, element targets), P2 H5, P4 | P1 §3.9.6 |
| `generate_lua_stubs.py` `OVERRIDES` / `_PARAM_TYPE_OVERRIDES[(class, member, param)]`, stub fragments `engine/core/script/binding/stubs/*.lua` | build tool | P1 p1-lua | P1 p1-anim (`anime.lua`, `Anime*Params`), P5 | P1 §3.9.5–3.9.6 |
| `HTMLUI_P1_STUB: <owner>` markers (wave-0 stubs; none left after phase 1) | convention | P1 wave-0 commit | each P1 package | P1 §3.0 |

## 7. Input and platform

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `canSendDocumentEvents`, pointer routing: screen documents (top layer first), then the component UI (`UISystem`; P1 `UISystem::isPointerOverUI()`: a move over component UI covers the world), then texture-mode documents on meshes (`dispatchWorldPointer`, nearest first), then the game; a pointer a world document captured goes to it first (P1 §3.15) | engine | P0 p0-ecs; P1 p1-input, p1-rtt, p1-integrate | P1, P4 `send_input` | P0 §3.7.3, P1 §3.6.1, §3.15 |
| `Engine::systemTextInput/systemImeComposition/systemImeCancel/wantsTextComposition/setTextInputArea`; `UIDocumentSystem::eventOnPointerDown/Up(int pointer, int button, float x, float y, int mods, bool touch, bool world = true)`, `eventOnPointerMove(int pointer, float x, float y, int mods, bool touch, bool world = true)` (`world = false`: screen documents only, as the engine calls them; P0's mouse overloads kept), `eventOnTouchCancel/eventOnWheel/eventOnMouseLeave/eventOnKeyDown/eventOnKeyUp/eventOnTextInput/eventOnImeComposition/eventOnImeCancel`, `hasFocusedTextInput`, `hasPointerCapture`, `getPointerCursor`; `RmlKeyMap::toRmlKey/fromRmlKey/toRmlModifiers/fromRmlModifiers/domKey/domCode`; `HtmlUIContext::processTouch*/processMouseWheel/setWheelScale/getTextInputHandler`, `RmlTextInputHandler::setCanvasMapper` | C++ | P1 p1-input | P1 p1-lua (`UIEvent` key/code/keyCode), P1 p1-rtt (`dispatchWorldPointer`, texture-context caret mapper), P4 `send_input` | P1 §3.6 |
| `System::setClipboardText/getClipboardText/setTextInputArea`; `System::setMouseCursor(CursorType)` with `CursorType` extended by `WAIT PROGRESS HELP GRAB GRABBING` and `using MouseCursor = CursorType`; `RmlSystem::cursorFromCss` | C++ | P1 p1-input | P3 (`cursor` lists) | P1 §3.6.3 |
| `UIDocumentSystem::dispatchWorldPointer(int pointer, int button, float canvasX, float canvasY, int mods, PointerPhase phase, bool touch, bool covered = false)` (`PointerPhase{DOWN, MOVE, UP, CANCEL}`; `covered`: screen-space UI took it, world documents only lose hover and key focus), `getTargetTexture(Entity)`, `getTextureContext(Entity)`, `getTextureFocus()`; `raycastMeshUV(Scene*, Entity, const Ray&) -> UIWorldHit{hit, distance, uv}` (`engine/core/htmlui/UIWorldRaycast.h`; `TEXCOORD1`, two-sided, bind pose) | C++ | P1 p1-rtt | P4 `send_input` `target`, inspector | P1 §3.12, §3.15 |
| MCP `send_input` event types `text`, `ime` (`text`, `cursor`), `ime_commit` (`text`), `ime_cancel`, `mouse_leave`, `mouse_enter` | MCP | P1 p1-input | P4 (`target`/`to_target` extension) | P1 §3.6.4 |

## 8. Editor, export, hot reload

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `isTextFile` accepts `.html .htm .css .rml .rcss` | editor | P0 p0-ecs (M2) | P4 | P0 §3.7.5 |
| `Util::isHtmlFile/isCssFile`, `FileType::HTML/CSS`, `HtmlCssLexer::lexLine`, `notifyHtmlUIFileSaved`, `HtmlUIWatcher`, `Project::getHtmlUIScenes`, `HtmlUITemplates` | editor | P1 p1-editor | — | P1 §3.8.4–3.8.6, §3.8.9 |
| `HtmlUIFileLog` (load scopes, lazy texture log, message observer), `UIDocumentSystem::getNextUpdateDelay` | engine | P1 p1-editor | P4 (auto-sync, redraw) | P1 §3.8.9 |
| `collectHtmlUIReferences(documentPath, reader = nullptr) -> std::vector<HtmlUIReference{path, raw, kind, fromFile, line, column, remote, missing, outsideAssets, absolute}>` and helpers (free functions in `engine/core/htmlui/HtmlUIDependencies.h`), static `Exporter::checkHtmlUIReferences(projectPath, assetsPath, documents, errors, warnings)` | engine/editor | P1 p1-editor | P2 (dependencies merged via H2; H14 runs `checkDocument` for `.html` in the export check) | P1 §3.8.6, §3.8.8, P2 §3.16 |
| `EntityCreationType::UI_DOCUMENT`, MCP `create_entity type=ui_document` | editor/MCP | P1 p1-editor | P4 | P1 §3.8.4 |
| Project setting `htmlUiMsaaSamples` (applied through `UIDocumentSystem::setPreferredSampleCount`) | editor | P1 p1-editor | — | P1 §3.8.8 |
| `AppSettings::htmlUiAutoImport`, `htmlUiVerboseDiagnostics`; CLI `import-ui`; Inspector remote-resources panel | editor | P2 p2-importers | P4 | P2 §3.13 |
| `w4::syncHtmlUi`, `McpAutoSync` UI tracking, `external_changes.ui_reloaded` | editor/MCP | P4 p4-checks | P5 (`.frag` trigger) | P4 §3.5 |

## 9. Vector graphics and animation

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `<svg>` / `decorator: svg()` on LunaSVG (upstream plugin; the ThorVG rasteriser patch is not made: benchmark decision) | element | P1 p1-vector | P2 H8 (`SvgPreprocess`, `rasterizeSvg`) | P1 §3.10.2, §3.10.5 |
| `<lottie>` (`UILottieElement`, `engine/core/htmlui/vector/`; attributes `src autoplay loop speed direction mode segment marker animation`; events `load error play pause loop complete`, `error` parameter `message`), `LottiePlayback`, `DotLottie` | element | P1 p1-vector | P2 (web-component mapping, H9 adds `playonhover`) | P1 §3.10.3, §3.10.5 |
| `TextureData` SVG on LunaSVG; nanosvg removed | engine | P1 p1-vector | — | P1 §3.10.4 |
| `anime` (`animate`, `createTimeline`, `createTimer`, `createSpring`, `stagger`, `eases`, `utils`), custom-property animation; C++ `Anime` / `AnimParam` / `AnimTargets` (`htmlui/anim/`); `AnimEngine::tick(HtmlUIClock::now())`, `reset()`, `addResetListener`; `AnimLua::getLiveLuaRefs()`; a selector target (no `root`) resolves against the documents of every scene of the game: `Engine::getScene()`, the running layers, then the scenes created but not running yet (P1 §3.15) | Lua/C++ | P1 p1-anim | P2 (`<script type="text/lua">` env), P4 examples, P5 C7 | P1 §3.11 |

## 10. Compatibility layer (phase 2, `jasperengine::htmlui::compat`)

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `SourceRef` (+`cover`), `SourcePos`, `SourceFiles` (+`find`, `slice`, `codePointLength`, `originText`), `stripUtf8Bom`, `sanitizeUtf8`; `CompatTypes.h`: `Span<T>`, `RemoteReference`, `RcssLineMap`, `RmlSourceMap`; `Hash.h`: `sha1Hex`, `sha256Hex`, `fnv1a64`, `toHex` | C++ | P2 p2-core | P2 wave 2, P5 | P2 §3.4, §4 p2-core |
| `Severity{Error, Warning, Info}`, `DiagCode`/`DiagArea` (both in `DiagnosticCodes.h`), `Diagnostic`, `DiagnosticSink` (`report`, `reportAs` for "by case" codes, `take`, `locate`), `toText` (severity words error/warning/info), `toJson` (+`name`), `sortDiagnostics`, `diagnosticSummary`, `logDiagnostics`; `DiagCodeInfo{…, severityByCase, …}`, `diagnosticCodes()`, `findDiagnosticCode(id/name/DiagCode)`, `diagAreaName`, `severityName`, constants `codes::<CamelCaseName>` (a new code = registry row + constant) | C++ | P2 p2-core | P4 (CLI, MCP, `ui-support`), P5 (`DiagArea::FX`, `EffectDiagnostic`) | P2 §3.4 |
| `Support{Native, Rewritten, Approximated, Emulated, Fallback, Unsupported, Ignored}`, property/value/selector/at-rule/media/element/attribute/input-type/unit/function tables (every row: `support, fork, supportWithFork, note, suggestion`; `PropertySupport::rmluiDefaultDiffers`, `ElementSupport::display`, `InputTypeSupport::reported/severity`), `RewriteId` (+`Sizing Flex Grid Content All LineClamp`), `SelectorKind{Selector, PseudoClass, PseudoElement}`, lookups `findProperty/findValue/valuesOf/findPseudoClass/findPseudoElement/findSelector/findAtRule/findMediaFeature/findElement/findAttribute/findInputType/findUnit/findFunction`, `effectiveSupport` (+ row template), `supportName`, `rewriteIdName`, `selectorKindName` | C++ data | P2 p2-core | P3 (fork columns), P4 (`ui-support`, matrix), P5 (effect rows) | P2 §3.5 |
| `ForkFeature` (P2: `Calc BorderStyle PercentRadius PseudoElements RootIsWhere Fixed GroupOpacity TimingFunctions Important CssWideKeywords Grid`; appended by P3: `CurrentColor ExtraUnits LineHeightNormal Has FocusPseudoClasses SelectorsLevel4 ListMarkers Outline BackgroundLayers ObjectFit TextShadow TextProperties Sticky BoxParity AnimationLonghands IndividualTransforms`; by P5: `RegisteredProperties TextFillFilter`), `ForkFeatures::present/none/all/probe/probe(feature)/has/with/bits/fromBits`, `forkFeatureName`, `findForkFeature`, `kPresent` (true = fork has it; each appended value also needs a `kNames` entry and a probe case) | C++ | P2 p2-core; extended by P3, P5 | P3 flips, P4 (`effectiveSupport`), P5 | P2 §3.6, P3 App. A |
| `CompatOptions` (`viewport` is `CompatSize{x, y}`, no RmlUi type), `FileAccess` (both `CompatOptions.h`), `readAssetFile`/`assetFileExists` (`VirtualFiles.h`), `CheckOptions{compat, verifyWithRmlUi, files}`, `CheckReport`, `checkDocument`, `checkStylesheet`, `locateElement`, `collectRemoteReferences(htmlPath[, files])`, `cssPropertyName`, `verifyRcssWithRmlUi` | C++ | P2 p2-core (`CompatApi.cpp`: checkStylesheet, cssPropertyName, verification) / p2-html (`CompatApiDocument.cpp` seam: checkDocument, locateElement, document references) | P4 (`check --ui`, `ExamplesCompatTest`, inspector), P5 (shader collector analysis) | P2 §3.12.1 |
| `VirtualFiles` (`@htmlui/…` in memory, owners = compile keys; served by `RmlFile` (H3b, done by p2-core)), `RmlLogTranslator` (`handle` from `RmlSystem::LogMessage` (H3, done by p2-core), `add(Registration{key, owner, files, mappings, sink})`/`remove`, `LineMapping{url, firstLine, lines}`, `Capture`, `translate`, `findLocation`) | C++ | P2 p2-core | P2 p2-html (registration of loaded compiles), p2-css (`Capture` in the RmlUi-accepts test), p2-runtime (H7) | P2 §3.2, §3.12.5 |
| CSS syntax: `tokenize` (`TokenizeOptions`, `TokenizeError`), `CssAst.h` (`Token`, `ComponentValue`, `Declaration`, `Rule` with raw `block`), `parseStylesheet/parseDeclarationList` + `parseBlockContents/parseRuleList/parseRule/parseDeclaration/parseComponentValueList/parseComponentValue` (`ParseError`), `splitAtCommas/trimWhitespace/extentOf`, `serialize/serializeIdent/serializeString/serializeUrl/formatNumber`, `RcssWriter` (`Sheet`/`DeclarationList` modes) + `RcssMediaCondition` | C++ | P2 p2-core | P2 p2-css, p2-html (style attributes), p2-importers (Google Fonts CSS), P5 | P2 §3.9.1, §3.9.10 |
| `CompatSources.cmake` (globs `compat/{.,css,css/props,html,html/transforms,runtime,tables,ua}/*.cpp`; tests glob `engine/tests/htmlui/compat/*.cpp`), `tools/htmlui/xcode_sync_compat.py` (adds new compat files to the Xcode workspace), `engine/tests/htmlui/compat/CompatTestCommon.h` | build/test | P2 p2-core | P2 wave 2, P3, P5 | P2 §4 p2-core |
| `DocumentLoader::compile/compileString/load/invalidate/restyle/restyleForViewport/fragmentToRml/clearCache` (+ `compile(path, options, files)`, `restyle(document, shared_ptr, options)`, `compiledOf/diagnosticsOf/addRuntimeDiagnostic`, `registerDocument`, `applyAutofocus`, `checkFontFamilies`, `isCompatDocument`, `sameRml`, `FragmentOptions`), `CompatDocument` (`rml`, `sourceMap`, `stylesheets` + line maps, `scripts`, `templates`, `remoteRefs`, `dependencies`, `diagnostics`; + `options`, `hints`, `important`, `styleAttributes`, `dependencyStamps`), `DocumentScript` (+ `sourceFile`, `firstLine`); `HtmlParser`, `HtmlTree`, `RmlSerializer`, `LuaHooks` (engine-internal); `isHtmlDocumentPath` (CompatApi) | C++ | P2 p2-html | P1 hooks (H1b, H2), P4, P5 | P2 §3.12.1, §4 p2-html as implemented |
| `UIDocumentSystem::getCompiledDocument/getDiagnostics/setColorScheme/setReducedMotion/startDocumentScripts`, static `setVerboseDiagnostics`; `UIDocument::getDocumentElement/getDiagnostics/setColorScheme/setReducedMotion`; `UIElement::getLocalName/get+setOuterHTML/insertAdjacentHTML/getContent/show/showModal/close/get+setCssText` (Lua: `localName`, `outerHTML`, `insertAdjacentHTML`, `content`, `show`, `showModal`, `close`, `style.cssText`, `documentElement`, `getDiagnostics`, `setColorScheme`, `setReducedMotion`); on HTML documents `tagName` is the DOM's (upper case) | C++/Lua | P2 p2-html (H4, H5) | P4 inspector, guide | P2 §4 p2-html as implemented |
| `UrlResolver` emits `asset://<path>` for local/imported files (`/@htmlui/…` for virtual ones); `RewriteContext::rootId/rootDirection` | C++ | P2 p2-html (fix of p2-core's emitted form) | p2-css | P2 §3.7.4 |
| `CssRewriter` (header and pass-through: p2-core; `CssRewriter.cpp` with `Impl`: p2-css) (`CssRewriter(options, files, sink, RewriteContext{files, manifest, virtualDirectory, documentPath})`, `rewrite(Span<const StylesheetInput>)` → `RewriteResult{sheets, importantSheet, dependencies, remoteRefs, virtualFiles, dependsOnViewport, animationFillNames, fontFamilies, declaredFontFamilies}`, static `rewriteSelector`, `rewriteDeclarations` (+ optional `CompatOptions`)), `css/props/*`, `css/AtRules.cpp`, `CustomPropertyIndex`, `ColorValue`, `Units`, `ValueNormalizer`; handler registry in `css/props/Rewriters.h` (`registerImageFunctionHandler`, `registerFilterFunctionHandler`, `registerAtRuleHandler`) | C++ | P2 p2-core (seam), p2-css; registry added by P5 p5-infra | P3 (gated branches), P5 C8 | P2 §3.9, P5 §3.5 |
| RML shape: `<body id="htmlui-root">` (HTML root) / `<htmlbody>` (HTML body), root band, `data-htmlui-loc`, UA `<link … origin="user-agent">`, reserved names | output contract | P2 p2-html | P3 (§3.1.1, §3.4.3), P4 (inspector) | P2 §3.2, §3.7.6, §3.8 |
| UA stylesheet `compat/ua/useragent.css` (standard CSS, rewritten per fork-feature set) and its SVGs `compat/ua/*.svg` (embedded, `@htmlui/ua/<name>`); `UserAgentStyle` (`embeddedFiles`, `source`, `get(fork) -> Sheet{rcss, lines, diagnostics, virtualPath, fork, files}`, `virtualPath(fork)`, `registerInternalFiles`, `buildEmbeddedHeader`); embed step `ua/embed_ua.cmake` → `htmlui_ua.h` (stand-in `engine/shaders/htmlui_ua.h`; the editor export writes the real one) | data / C++ | P2 p2-ua (sheet, SVGs); p2-core (`UserAgentStyle`, embed) | P3 additive rules (lists, focus) | P2 §3.10 |
| Runtime: `registerRuntime()` (in `HtmlUI::acquire`), `releaseRuntime()` (after `Rml::Shutdown` in `HtmlUI::release`), `updateRuntime(Rml::Context*)` (in `HtmlUIContext::update`, before `Context::Update`) — declarations and call sites p2-core, bodies p2-runtime (`runtime/RuntimeRegistry.cpp`), `CompatProperties::registerAll()` (skips names the fork registers), element instancers, `html-layer`/`html-shape`/`html-border-image` decorators, `FocusTracker`, `AnimationFill`, `ListMarkers`, `LucideIcons`, `SvgPreprocess`, `htmlui::rasterizeSvg` | C++ | P2 p2-runtime | P3 (feature gating), P5 | P2 §3.11 |
| Hooks H1–H14 into P0/P1 code (`HtmlUI::acquire`, `HtmlUIContext::loadDocument`, `reloadChangedFiles`, `RmlSystem::LogMessage`, `RmlFile::Open`, `UIDocument`, `HtmlUIClassesLua.cpp`, `RmlFontEngine`, `RmlRenderer::LoadTexture`, SVG plugin, `<lottie>`, Inspector, CLI, AppSettings, CMake); H1's runtime call sites, H3 and H3b done by p2-core | hooks | P2 packages | — | P2 §3.16 |
| `HtmlUILua::runDocumentScript(UIDocument&, const DocumentScript&)` (`<script type="text/lua">`) | C++/Lua | P2 p2-html | P4 guide | P2 §3.12.4 |
| `ImportManifest` (`assets/htmlui-imports/manifest.json`; `get/current/reload/notifyFileChanged/load(FileAccess)/parse/find/entries/add/normalizeUrl`, `ImportEntry`), `UrlResolver` (`classify` → `UrlKind`, `resolve(...)` → `ResolvedUrl`, with or without reporting; `parseDataUri`, `extensionForMime`, `dataPath`, `joinPath`, `normalizePath`, `percentDecode`) | C++ | P2 p2-core | P2 importers, p2-html, p2-css | P2 §3.7.4 |
| `editor/htmlui/HtmlUiImporters.h`: `ImportResult`, `importGoogleFonts`, `importLucideIcons`, `importUrl`, `importDocumentReferences`; `JASPERENGINE_IMPORT_MIRROR` | editor C++ | P2 p2-importers | P4 (`import_files`, `create_ui_document`, tests) | P2 §3.13.7 |

## 11. Fork features and fork APIs (phase 3)

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| Root `%`/`em` font-size resolution; `:root` = document element | fork | P3 p3-base / p3-cascade | P2 output | P3 §3.1.1, §3.5.1 |
| CSS initial values (font-size 16px, color black, line-height normal, `min-*: auto`, alignment `normal`, `opacity` not inherited, `visibility` inherited) | fork | P3 p3-base | P2 table data updated by P3 | P3 §3.1.2 |
| `StyleValue`, `ShorthandType::AnyOrder/Custom`, `RegisterShorthandParser`, `LayoutDetails::GetLayoutChildren/GetMinContentWidth/GetMaxContentWidth`, containing-block override, animation layers | fork API | P3 p3-base | P3 wave 1, P5 (`ShorthandType::FallThrough` unchanged) | P3 §3.1.3–3.1.10 |
| `<link origin="user-agent">` → `StyleSheet::SetOrigin(StyleOrigin::UserAgent)`, `revert` | fork | P3 p3-cascade | P2 serializer | P3 §3.4.3 |
| Native `background-*`/`mask-*` accepting registered decorator functions as layers | fork | P3 p3-paint | P2 (`html-layer`, `html-shape`), P5 (`jfx-*` layers) | P3 §3.9.1 |
| `ComputeObjectFitRect` | fork API | P3 p3-paint | P1 SVG/Lottie elements, P2 `HtmlImageElement` | P3 §3.9.3 |
| `TextShapingContext::word_spacing` (engine font engine applies it) | fork API | P3 p3-text | P0/P1 `RmlFontEngine` | P3 §3.10 |
| `Rml::SetGeometryFeathering(bool)` (engine sets it from the target's MSAA) | fork API | P3 p3-paint | P1 `HtmlUIContext` | P3 §3.7.3 |
| `SetMouseCursor` receives the first supported CSS cursor keyword | fork | P3 p3-values | P1 `System::setMouseCursor` | P3 §3.3.8 |

## 12. Agent tooling (phase 4)

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `HtmlUIInspector` (`documents`, `documentInfo`, `query`, `hitTest`, `detail`, `styles`, `elementRect`, `withViewport`, `pushDataModel`), `InspectElement` (+ `effects`, P5) | C++ | P4 p4-inspector | P4 tools, P5 | P4 §3.3.1 |
| `HtmlUIAssertions::run(Scene*, Entity, AssertSpec)` | C++ | P4 p4-inspector | P4 tools | P4 §3.3.2 |
| `ProjectChecks::isUiFile`, `projectFiles(root) -> compat::FileAccess`, `checkUiFiles`, `Finding{…, code, suggestion}` | editor C++ | P4 p4-checks | P4 tools, P5 (`.frag` checks) | P4 §3.4.1 |
| CLI `check --ui` (+ hook), `ui-support` (JSON with phase-2 statuses), `tools/mcp/gen_html_ui_support.py` → `docs/mcp/guide/html-ui-support.md` | CLI/tool | P4 p4-checks | P5 (matrix rows, `ui-effects` next to `ui-support`) | P4 §3.4 |
| MCP `inspect_ui`, `assert_ui`, `create_ui_document`; `send_input {target}`; `import_files` fonts/icons/URLs/`from_document`; resources `jasper://examples/html-ui`; tool budget caps | MCP | P4 p4-mcp-tools | P5 (`effects` key, budget, `MODULES`) | P4 §3.6 |
| Example library `docs/mcp/guide/html-ui-examples/`, `HtmlUiExamples` | data/editor | P4 p4-examples | — | P4 §3.7 |

## 13. Effects (phase 5)

| Name | Kind | Provider | Consumers | Ref |
|---|---|---|---|---|
| `effect(<name>, …)` in `background(-image)`, `mask-image`, `filter`, `backdrop-filter`; `@effect`; typed RCSS `jfx-<name>(…)`; fallback instancers `effect` | CSS | P5 p5-infra | P4 guide/matrix | P5 §3.2, §3.5 |
| `@property` (`CompatAtProperty`, fork patch `rmlui: registered custom properties`) | CSS/fork | P5 p5-property | — | P5 §3.11 |
| `EffectRegistry`, `EffectRuntime` (plugin), `EffectClock` (fixed time or `HtmlUIClock::now()`), `EffectRenderer` | C++ | P5 p5-infra | — | P5 §3.6–3.7 |
| `UIDocument::setEffectTime/getEffectTime`, `UIElement::getEffects` (+ Lua) | C++/Lua | P5 p5-infra | P4 inspector | P5 §3.12 |
| CLI `ui-effects`, `shaders --shader uieffect/…`; MCP `list_ui_effects`, `set_ui_effect_time`; `docs/mcp/guide/html-ui-effects.md` | CLI/MCP/doc | P5 p5-tooling/p5-infra | — | P5 §3.13 |
| Codes `HTMLUI-FX-0001…0016`, `at-property-*` (next free AT numbers) | diagnostics | P5 | P4 (reported like any code) | P5 §3.13 |
