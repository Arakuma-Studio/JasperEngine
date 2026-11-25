# S92 — HTML UI render cost, output-resolution UI, Lua overload dispatch

Arcade engine-gaps program (S92-S98), the owner's top priority. Source: the "Priority engine work"
section and items U27, U34, U35, U36, U37, R57, R66 and S33 of
`JasperArcade-Ported/docs/port/ENGINE_LIMITATIONS.md` (the ledger). The port was built against
engine `7f48d7e8d` (2026-10-30). Paths are relative to the repository root. Every claim in §3 was
checked by reading current main (`82ccd7c6a`) at the cited lines. "Inferred" means read from the
code and not run; nothing in this spec was built or run.

**Status: implemented** on `feature/s92-htmlui-render-cost` (packages A, B1, B2, C and D, integrated
with `main` at `82ccd7c6a`). What each package did differently from this text, and the integration's
decisions, are in the Deviations section at the end.

## 1. Goal

A game HUD made of ordinary CSS costs more GPU time than the 3D world on a phone. The port measured
67 to 206 offscreen passes per frame from the UI alone in its game M. A document that did not change
is drawn again every frame. Gradients on rounded boxes, `clip-path` and `filter` each go through
full-target layers every frame. The UI renders into a 4x MSAA target by default and cannot be
switched per scene or from Lua. It is blended over the whole screen. It is scaled down with
`render_scale` and smeared by FXAA. Under an orthographic camera it is not on the screen at all.

S92 fixes these costs at the engine level:
- An unchanged document draws nothing.
- Only what was drawn is composited.
- Rounded gradients and simple clip shapes need no layer.
- Filter and mask results are reused while the element does not change.
- The layers left are sized to their element.
- The UI composites after post-processing, at output resolution and in screen space under any camera.
- MSAA has a mobile default and can be set per scene, per document and from Lua.

S92 also makes a Lua call that reaches a later overload as cheap as one that hits the first. It does
this without the hidden `pcall` and error string per miss, and without the leak on clang.

## 2. Scope

**In scope**
- Frame reuse:
  - The kept document target, decided by a frame signature (R1-R3).
  - Tile-limited composite (R4).
  - Depth-stencil discard (R5).
  - New statistics (R6).
- Layer cost:
  - Gradient primitives extended to 16 stops, conic and repeating gradients (R7).
  - Analytic `clip-path` basic shapes (R8).
  - A layer result cache (R9).
  - Element-sized layers (R10).
- U27:
  - Scissor clips under axis-aligned transforms (R11).
  - Stencil bookkeeping per attachment (R12).
- Composite placement:
  - Screen Space - Overlay after post at output resolution, and Screen Space - Camera (R13, R14).
  - Screen-space composite under every camera type in game views (R15).
- MSAA:
  - Auto project default (R16).
  - Per-scene override (R17).
  - Per texture-mode document (R18).
  - Per-context resolution (R19).
- Editor, MCP, Lua and docs for all of the above (R20-R23).
- LuaBridge overload dispatch: adopt the port's patch, fix the remaining leak paths, add a ledger and
  tests (R24-R28).
- Performance budgets and their measurement (R29-R34).

**Out of scope**
- **U30, U31** (first-draw glyph-effect and image-decode hitches) belong to S93. What S92 does for
  them:
  - S92 makes nothing about the first draw cheaper. The convolution in `RmlFontEngine::getEffectGlyph`
    and the decode in `RmlRenderer::LoadTexture` (`engine/core/htmlui/RmlRenderer.cpp:994-1070`) are
    untouched.
  - Once the new content is drawn, R1 keeps the document target and R9 keeps the layer results. The
    hitch frame is then followed by frames that cost nothing, not by full redraws.
  - When S93 regenerates text (a font `version` bump), R1's signature sees new geometry and redraws
    once. This is correct, and costs no more than today.
- **S32** (the ledger's `Quaternion(nil)` crash, `Quaternion(Vector3)` over-read and
  `tostring(debug.getmetatable(Vector3()))` crash) and **S1** (an enum taken as a number) belong to
  S95. S92 changes the dispatch mechanism in `LuaBridge.h` only, not the bindings in
  `engine/core/script/binding/`.
- Partial (damage-rectangle) redraw of a changed document. A document that changes is redrawn whole,
  in one pass (D15).
- A CPU-side "context is clean" flag that skips RmlUi's render traversal (D1, D14).
- `EXT_multisampled_render_to_texture` on GLES. sokol does not expose it, and Auto makes 1 sample the
  mobile default (D7).
- Converting `drop-shadow()` to an analytic shadow primitive (D16).
- No OpenEngine code is involved. Nothing on the README do-not-copy list is touched. The game stays
  on our RmlUi fork.

## 3. Current state

### 3.1 What the port measured, and what changed since

The ledger's pass counts (M: beach 136, swim 67-85, power-up cards 206, upgrades 111, shop 96,
results 160; one `.btn` 33) were taken on `7f48d7e8d`. **S16 (HTML UI rendering cost and text)
landed after that commit.**
- `engine/core/htmlui/RmlRendererBatch.cpp` did not exist at `7f48d7e8d`. It was added by `d1a4e869f`.
- The S16-F5 gradient primitives (`5201522c7`, `2e03eb094`) are not ancestors of `7f48d7e8d`
  (checked with `git merge-base --is-ancestor`).
- S16 brought several things: batching, SDF box primitives (S16-F1), analytic and elided clip masks
  (S16-F2), off-screen culling (S16-F4), and gradient primitives of at most four stops (S16-F5).

The port's counts are therefore an upper bound. **The current-main numbers for the same patterns are
unverified.** AC10 measures them on current main first; that run is the red control.

### 3.2 Every frame is drawn again (U36)

- `HtmlUIContext::update` calls `markUpdated()` unconditionally (`engine/core/htmlui/HtmlUIContext.cpp:339-350`).
  The scene's screen context is updated every frame (`engine/core/subsystem/UIDocumentSystem.cpp:305`).
- The "nothing changed" exit of `HtmlUIContext::render` is therefore reached only when `render` is
  called twice in one frame (`engine/core/htmlui/HtmlUIContextRender.cpp:143-145`). That happens in
  the editor's Game view (`:81-86`).
- Every frame runs `context->Render()` and replays the commands. The pass count is at least 1
  (`RmlRenderer::beginPass`, `stats.passes++`, `RmlRenderer.cpp:1404-1424`).
- S16's run cache (`RmlRendererBatch.cpp:830-891`) saves CPU gathering only. The GPU passes and draws
  still run.
- The composite is one full-screen blended quad (`RmlRenderer::drawComposite`,
  `RmlRenderer.cpp:3454-3481`: `object->draw(0, 6, 1)`). It is drawn even where the document is empty.
- Every pass stores colour and depth-stencil (`RmlRenderer.cpp:1410-1412`, `StoreAction::STORE`). The
  depth-stencil is stored at the end of the frame too, when nothing reads it again.
- `docs/mcp/guide/html-ui.md:1972-1974` says "A static document without filters or masks is one
  offscreen pass and uploads nothing per frame". That is true of uploads. It is false of passes once a
  second frame passes, because the pass runs anyway.

### 3.3 Layers: gradients, masks, filters, clip-path (U35)

**Gradients on rounded boxes: partly fixed by S16-F5.**
- `MakeGradientTiles` (`engine/libs/rmlui/Source/Core/ElementBackgroundLayers.cpp:582-590, 651-734`)
  emits one gradient primitive over the clip box's fill area with its radii, and no mask layer, when
  `RmlRenderer::SupportsGradientPrimitive` says yes.
- It says yes only for `LINEAR` and `RADIAL` with at most `HtmlUIPrimitive::GRADIENT_STOPS` = 4 stops
  (`engine/core/htmlui/RmlRendererBatch.cpp:400-406`, `engine/core/htmlui/HtmlUIPrimitive.h:16`).
- Everything else takes the mesh path: a mask made by `MakeClipMask` (`ElementBackgroundLayers.cpp:437, 640`),
  then two layers and a mask composite (`:765-808`). That covers:
  - five or more stops, and conic and repeating gradients (`RmlCompiledShader::Function` has all six,
    `engine/core/htmlui/RmlRenderer.h:166`);
  - a border with gaps around the curve (`:711-719`);
  - inline fragments (`num_boxes != 1`);
  - `background-attachment` canvas.
- The port's Button01 body gradient has 7 stops with hard px stops
  (`JasperArcade-Ported/tools/gen/uisprites.py:497-499`), so it still takes the mask path.

**Filters**:
- `ElementEffects` pushes a layer for the element's content (`ElementEffects.cpp:405`). It draws the
  content into it and composites through the filter list (`:510-511`) at every render.
- The renderer runs each filter into post-process targets (`RmlRenderer::compositeLayersNow`,
  `:2193-2308`; `applyFilter` `:2309-2393`).
- Nothing is kept between frames: there is no cache in `RmlRenderer` for a layer's result.
- `BoxShadowCache` keeps box-shadow textures only (S16 §3).
- The port's `.btn::before` carries `filter: drop-shadow(0px 8px 0px rgba(0,0,0,.2))`
  (`JasperArcade-Ported/assets/ui/shared/widgets.css:56`), so every button is a filter layer every
  frame.

**`clip-path`** becomes `mask-image: html-shape(...)` (`engine/core/htmlui/compat/tables/PropertyTable.cpp:244`).
It then takes the mask path of `ElementEffects` (`:496-503`: a layer, the mask layer and a
`SaveLayerAsMaskImage`) for every basic shape, including `inset()`, `circle()` and `ellipse()`,
which are rounded rectangles.

**Layer size**:
- P7-F2 region layers (`RmlRenderer::PushLayerRegion`, `:2060-2090`; pool and 64-px granules,
  `regionFrame`, `:2014-2058`) are used only for a window that extends beyond the viewport.
- A window inside the viewport is "an ordinary layer" (`:2070-2073`), the size of the whole target.
- `ElementBackgroundLayers` masks (`:768, 801`), `ElementEffects` masks (`:496`) and the `text-shadow`
  blur layer (`ElementText.cpp:417`) call `PushLayer()` without a region at all.
- On a tiler, each such pass loads and stores the full target.

### 3.4 MSAA (U37)

- The sample count is one process-wide static, default 4 (`HtmlUIContextRender.cpp:40`), set from the
  project setting only:
  - `htmlUiMsaaSamples` in `project.yaml` (`editor/Stream.cpp:2797, 3611-3612`);
  - Project Settings > Graphics > HTML UI MSAA (`editor/settings/SettingsRows.cpp:2171`);
  - the generated `main.cpp` (`editor/Generator.cpp:2636`) and the prebuilt player's config
    (`engine/core/player/PlayerConfig.h:104`, `editor/ExporterPrebuilt.cpp:652`).
- `UIDocumentSystem::setPreferredSampleCount` (`engine/core/subsystem/UIDocumentSystemRender.cpp:44-46`)
  is C++ only. No Lua binding names it (grep of `engine/core/script/binding/`).
- A texture-mode document uses the same static.
- S16 decided to keep the default at 4 and called lowering it "a separate decision with its own golden
  pass" (`docs/specs/S16-ui-render-text.md:392-394`, D7).
- At one sample the fork feathers mesh edges (`HtmlUIContextRender.cpp:121`). SDF primitives
  (S16-F1, on by default: `editor/Generator.h:161`) and analytic clips antialias at any count.
- The target is RGBA8 colour plus D24S8 (`RmlRenderer.cpp:238`).

### 3.5 Where the UI is composited (U34, R57)

- The scene's screen documents composite inside the main camera's colour pass
  (`engine/core/subsystem/RenderSystem.cpp:20159-20172`). That happens before SSR, the post chain and
  the uber pass (`:20186-20262`).
- With `render_scale` below 1, the camera pass target is smaller (`:15485-15487`). The document,
  rendered at the output size (`renderOffscreen(targetViewRect())`, `:18668`; `targetViewRect`
  `:16947-16952`), is therefore scaled down into it. The uber pass then scales it up again, and FXAA,
  CRT, VHS, glitch and pixelate run over it.
- `drawComposite` fills the viewport only for `CAMERA_PERSPECTIVE`
  (`engine/core/subsystem/UIDocumentSystemRender.cpp:103-107`). For `CAMERA_UI` and `CAMERA_ORTHO`
  it draws the canvas rectangle (0,0)-(w,h) of world space through the camera (`:109-131`). This
  happens in game views too, not only in the editor's Scene view, which the comment names. A game
  ortho camera of 22 world units shows no UI at all (R57).

### 3.6 Clips under transforms (U27)

- An overflow clip whose clipping element has a transform is always a stencil clip mask, with the
  scissor disabled (`engine/libs/rmlui/Source/Core/ElementUtilities.cpp:191-215`).
- Text lines are not culled under a transform (`ElementText.cpp:324-329`).
- S16 can elide the mask (a scissor) or clip analytically (`RmlRendererBatch.cpp:645-800`).
  - It does neither for a scope that contains a layer: `PUSH_LAYER`, `COMPOSITE` and `SHADER` set
    `analytic = false` (`:736-760`).
  - It does not elide when a draw is not admitted by the shape (`:686-690`). A scrolled list's clipped
    rows are exactly such draws.
- The port's lists are a scaled panel holding filtered tiles and text, so they take the stencil path.
- Every non-region layer shares layer 0's depth-stencil. It is loaded and stored at each pass
  (`bindLayer`, `RmlRenderer.cpp:1534-1571`).
- The mask is redrawn when `maskVersionOf(index)` is stale (`ensureMask`, `:1942-1990`). Region layers
  keep their own version. The shared one is a single `sharedMaskVersion` for all full-size layers
  (`:1936-1940`).
- The ledger's fault (text escaping the viewport on a phone, but not on llvmpipe or SwiftShader) is
  **unverified**. Nothing in the code read shows a wrong mask version, so the cause on a tile GPU is
  unknown. R11 takes the port's case off the stencil path. R12 makes the stencil path's bookkeeping
  per attachment and checkable.

### 3.7 Lua overload dispatch (R66, S33)

- `luabridge::detail::try_overload_functions` (`engine/libs/luabridge3/LuaBridge.h:6914-6982`):
  - It makes an error table on every call.
  - For every overload of matching arity, it `lua_pcall`s the overload.
  - A userdata mismatch raises through `luaL_argerror`, whose `pushglobalfuncname` walks
    `package.loaded`.
- Since `7f48d7e8d` only S34 (`2a032453b`: failure paths out of line, registration cold) and
  `583652b17` (`raise_exception_error` recursion) touched the header. The dispatch is unchanged.
  Its lines moved by +43.
- `engine/libs/luabridge3/` has no patch ledger (only `LICENSE.txt`, `README.md`). The local changes
  are inline `// JasperEngine` comments (`LuaBridge.h:90, 619, 6455`). This is against AGENTS.md
  ("A patch to vendored code follows that library's patch ledger beside it").
- Lua is compiled as C (`engine/libs/lua/CMakeLists.txt:48-54`, no C++ language override), so
  `lua_error` is a `longjmp`. A decoded `std::string` argument on the C++ frame is not destroyed when
  a later argument fails. That is S33 (measured by the port with clang; g++ decodes right to left).
- The port's patch (`JasperArcade-Ported/docs/port/engine-patches/luabridge-overload-dispatch.patch`,
  271 lines) adds a no-raise check per overload.
  - `git apply --check` succeeds on a copy of the current header (run by this spec's research step in
    `/tmp`, not in the worktree). Its 10 hunks shift by +43.
  - The bodies of the overloads are unchanged.
  - Leak paths it leaves (inferred): calls where every overload fails (the second pass reruns the
    skipped ones); overloads the check let through that still fail to decode; every wrong call of a
    non-overloaded function (`tupleize`, `LuaBridge.h:5988`); and `raise_exception_error` called inside
    `catch` blocks (`:6554, 6627, 6669`), a `longjmp` out of a handler.
- Scale: 746 `luabridge::overload<` uses in 7 binding files, forming 280 overload sets (200 in
  `engine/core/script/binding/GameObjectClassesLua.cpp`), plus 81 `addConstructor` lists with two or
  more signatures.
- Engine tests drive the Lua state through gtest (`engine/tests/core/script/LuaMathBindingTest.cpp:20-28`).
  `LuaBindingBench.CallsAndProperties` (`engine/tests/core/script/LuaBindingBenchTest.cpp:57-67`)
  prints medians. `engine/tests/core/support/AllocationCounter.h` counts allocations on Linux.

### 3.8 What the games do instead

- **`.lite`** (`JasperArcade-Ported/assets/ui/shared/minigame.css:76-91`,
  `tools/gen/uisprites.py:386-470`): no `clip-path`, no `filter`, no gradient on a rounded box.
  - Plain colour plus hard inset shadows, and the gradient moved onto a square inner box.
  - M uses it. The other games keep the costly look by the owner's decision ("the games keep their
    exact gradient look; the cost gets fixed at engine level").
  - S92's bar: every game can drop `.lite` and pay what `.lite` pays.
- **`htmlUiMsaaSamples: 1`** in `JasperArcade-Ported/project.yaml:7`, the only arcade-wide change.
- **`shared.ui.ScrollView`** draws every item of a scaled list as `filter: opacity(1)`, so the text
  goes through the composite path that clipped correctly on the phone (U27).
- **A perspective camera at 100 units with `yfov = 2 atan(size/100)`** instead of an ortho camera
  (R57). Its side effect: shakes with x/y rotation move the picture by a third of the screen.
- **`shared.Uniform`, `shared.Rot.apply`, `tools/je/lint_lua_perf.py` and an editor-only
  `LuaOverloadGuard`** (R66).

## 4. Triage

| Ledger item | Verdict | Evidence (current main) | What S92 does |
|---|---|---|---|
| Priority 1a: gradient on a rounded box as rounded geometry | PARTIAL | S16-F5 `5201522c7`/`2e03eb094` (after the port): `ElementBackgroundLayers.cpp:582-590, 651-734`; at most 4 stops, linear/radial only (`RmlRendererBatch.cpp:400-406`); mask path otherwise (`:765-808`) | R7: 16 stops, conic and repeating, every layer of a rounded box |
| Priority 1b: cache filter and mask results | MISSING | `ElementEffects.cpp:405-511` redraws every render; no result cache in `RmlRenderer` (`compositeLayersNow` `:2193`) | R9 |
| Priority 1c: scissor and size passes to the element | PARTIAL | region layers only beyond the viewport (`RmlRenderer.cpp:2070-2073`); masks and the text-shadow layer push full-target layers (`ElementBackgroundLayers.cpp:768, 801`, `ElementEffects.cpp:496`, `ElementText.cpp:417`) | R10 |
| Priority 1d: keep the target, composite only what was drawn | MISSING | `HtmlUIContext.cpp:349`, `UIDocumentSystem.cpp:305`; full quad `RmlRenderer.cpp:3454-3481` | R1-R5 |
| Priority 1e: MSAA per scene or from Lua | MISSING | static `HtmlUIContextRender.cpp:40`; project-only (`Stream.cpp:2797`, `Generator.cpp:2636`); no Lua binding | R16-R19, R22 |
| Priority 2: Lua overload dispatch | MISSING | `LuaBridge.h:6914-6982` unchanged since the port; only `2a032453b`, `583652b17` touched the header | R24-R28 |
| U35 | PARTIAL | as 1a-1c | R7-R10, budgets R29-R30 |
| U36 | MISSING | as 1d; depth-stencil stored, `RmlRenderer.cpp:1412` | R1-R6, R31 |
| U37 | MISSING | as 1e; S16 D7 kept 4 (`S16-ui-render-text.md:392`) | R16-R19, R32 |
| U27 | MISSING (fault unverified) | stencil under any transform `ElementUtilities.cpp:191-215`; no text culling `ElementText.cpp:324-329`; S16 elision/analytic off with layers `RmlRendererBatch.cpp:736-760`; shared depth-stencil bookkeeping `RmlRenderer.cpp:1534-1571, 1936-1990` | R11, R12, AC11 |
| U34 | MISSING | composite in the camera pass `RenderSystem.cpp:20159-20172`, before the uber pass `:20218-20262` | R13, R14 |
| R57 | MISSING | world-space canvas for every non-perspective camera, game views included: `UIDocumentSystemRender.cpp:103-131` | R15 |
| R66 | MISSING | as Priority 2 | R24-R27, R34 |
| S33 | MISSING | Lua built as C (`engine/libs/lua/CMakeLists.txt:48-54`): `longjmp` over decoded arguments; extra paths in §3.7 | R26 |
| U30, U31 | S93 | — | §2: kept target and cached layers after the first draw only |

## 5. Reference

- **The port's LuaBridge patch** (`JasperArcade-Ported/docs/port/engine-patches/`: `README.md`, the
  patch, `overload_dispatch_test.cpp`, `wrong_calls.lua`, `run_standalone.sh`):
  - Take the patch as the base of R24 and R25.
  - Port its cases into an engine gtest. The standalone harness compares two checkouts and is not
    vendored.
  - Its measurements are the R34 reference: `setShaderUniform(name, float)` 526 → 4.6 µs,
    `Quaternion * Vector3` 91 → 4.6 µs, on the port's dev host.
- **The port's workarounds** (§3.8), as the API shape games want: a document that costs nothing when
  idle, UI at output resolution, MSAA 1 on phones.
- **Unity**, for names and behaviour:
  - Canvas Render Mode "Screen Space - Overlay": drawn after the camera stack, unaffected by
    post-processing and render scale.
  - "Screen Space - Camera": drawn by the camera, post applies.
  - The URP asset's "Anti Aliasing (MSAA)" options: Disabled, 2x, 4x, 8x.
- **Browsers**, for the cache: Chromium's render surfaces for filters and its raster invalidation keep
  an element's filtered result until its content changes. R9 has the same semantics, keyed by what
  was recorded rather than by a DOM invalidation.
- No OpenEngine files. Nothing on the do-not-copy list.

## 6. Requirements

### Functional: frame reuse (U36)

- **R1 Kept target.** After `context->Render()` records a frame, the renderer computes the frame's
  signature over every recorded command and its parameters:
  - geometry by a serial number given at `CompileGeometry` and never reused, not by pointer;
  - a texture by its serial and content version, bumped by every upload: atlas pages, video frames,
    dynamic and render-to-texture textures, `SaveLayerAsTexture`;
  - compiled filters and shaders by a hash of their parameters;
  - transforms, scissors, clip operations and clip shapes by value;
  - the target size, the sample count and the pixel ratio.
  When the signature equals the last drawn frame's and nothing in R2 made the frame live, nothing is
  replayed: no pass, no draw, no upload. The target keeps its content and its composite tiles (R4).
  - `HtmlUIContext::update` no longer forces a redraw (`HtmlUIContext.cpp:349` goes).
  - A new size, sample count, view loss (`releaseGpuResources`), settings change
    (`UIDocumentSystem::setHtmlUiRenderOptions`) or texture recreation still forces one.
  - This applies to screen and texture-mode contexts alike.
- **R2 Live frames.** A frame is drawn whatever its signature when it reads something the signature
  cannot version. Each source reports itself to the renderer with a reason bit:
  - a video texture;
  - a texture backed by an engine render target (a camera's);
  - a phase-5 effect shader that reads time or keeps state (`EffectRuntime` knows);
  - a `<particle-system>` element;
  - pending MSDF glyphs;
  - a transform-aware text settle (`RenderManager::RequestRender`).
  A C++ entry point (`HtmlUIContext::requestRedraw()`) lets custom native content ask for one frame.
- **R3 Verification mode.** With `JASPER_HTMLUI_VERIFY_KEPT=1` (tests only), a frame R1 would keep
  is drawn anyway into a scratch target and compared with the kept target, byte for byte. A mismatch
  logs `[HTML UI] kept frame differs` with the first command whose source was not versioned, and
  counts in `keptMismatches`.
- **R4 Composite what was drawn.**
  - The renderer marks the 64x64-pixel tiles of layer 0 that received a colour write this frame: each
    draw's bounds after its transform and scissor, and a layer composite's window.
  - The composite draws only those tiles, merged into horizontal runs, in one draw.
  - A frame that drew nothing composites nothing.
  - A kept frame keeps its tile set.
  - The image equals the full-quad composite.
- **R5 Depth-stencil discard.**
  - In a recorded frame whose plan has no stencil use (every clip elided or analytic, no mask), the
    depth-stencil is loaded and stored `DONTCARE`.
  - In a frame that uses it, the last pass binding each depth-stencil attachment stores `DONTCARE`.
  - The plan is known before the replay (`executePlan`), so the last pass is known.
- **R6 Statistics.** `HtmlUIRenderStats` (`engine/core/htmlui/HtmlUIRenderStats.h`) gains:
  - `framesKept` (1 when R1 kept the frame), `liveSources` (R2's reason bits) and `keptMismatches`;
  - `compositeTiles` and `compositePixels`;
  - `layerPixels` (the render area of every layer and post-process pass, in target pixels);
  - `layerCacheHits`, `layerCacheMisses` and `layerCacheBytes`;
  - `samples` and `targetBytes`;
  - `depthStencilStores`.
  They reach MCP `inspect_ui render_stats`, `get_render_stats`, Lua `UIDocument:getRenderStats()`
  and the benchmark through `forEachField` and `add`, which are extended.

### Functional: layer cost (U35)

- **R7 Gradient primitives (extends S16-F5).**
  - Linear, radial and conic gradients and their repeating forms draw as `HTMLUI_PRIM` gradient
    primitives on square and rounded boxes, with up to 16 stops (px, %, hard and transparent stops).
  - So do the decorators (`DecoratorGradient.cpp`) and every `background-image` layer of a rounded
    box (`MakeGradientTiles`).
  - These draw with no layer and no mask.
  - Still on the mesh-and-mask path, with R9 and R10 applied: more than 16 stops, a non-solid border
    around a curve (browser parity, `ElementBackgroundLayers.cpp:711-719`), inline fragments,
    `background-attachment` canvas, and `cross-fade()`.
- **R8 Analytic `clip-path` shapes.** `clip-path: inset(... [round ...])`, `circle()` and `ellipse()`
  clip analytically as an S16-F2 shape (a rectangle with elliptical radii) in the content's shaders.
  This holds on an element without `mask-image` and with no layer inside the clipped content. No layer
  is pushed. `polygon()`, `path()`, a shape with a reference box other than `border-box`, and shapes
  with content holding layers keep the mask path with R9 and R10.
- **R9 Layer result cache.**
  - A layer scope's result is kept as a texture of the layer's window. A scope is everything between
    a `PUSH_LAYER` and its `COMPOSITE`: filters, `mask-image`, a `clip-path` mask, a mesh-path
    rounded-gradient mask, a `text-shadow` blur, `backdrop-filter` excluded.
  - The key is the scope's signature (R1's, translations relative to the window's origin), its
    filters' parameter hashes, the window size, the sample count and the pixel ratio.
  - A hit draws the cached texture with the composite's blend, scissor and clip mask: one draw, no
    pass, no filter.
  - A trailing group opacity is applied at the composite, outside the cached image, so an opacity
    fade hits.
  - A scope that reads a live source (R2) is not cached.
  - A cached result equals the uncached one byte for byte.
  - Budget per renderer: 8 MiB on Android, iOS and HTML5, 32 MiB on desktop, least recently used
    first. An entry unused for 120 frames is freed, and so is everything at `releaseGpuResources`
    and context destruction.
- **R10 Element-sized layers.**
  - Every layer pushed for an element (a filter's content, `mask-image` and `clip-path` masks,
    mesh-path rounded-gradient masks, a `text-shadow` blur, opacity groups) is a region layer that
    covers its window, from the P7-F2 pool in 64-px granules.
  - Fork side: `ElementBackgroundLayers::Render` (`:768, 801`), the `ElementEffects` mask and
    opacity path (`:496`) and `ElementText`'s shadow layer (`:417`) pass their window to
    `RenderManager::PushLayer(region)`.
  - Renderer side: `PushLayerRegion` makes a region layer for windows inside the viewport too. The
    early return at `RmlRenderer.cpp:2070-2073` goes.
  - The post-process targets of its filters have the window's size (the existing `RegionPost` sets).
  - Layers the size of the whole target remain only for windows that cover it.

### Functional: clips under transforms (U27)

- **R11 Scissor for axis-aligned clips.**
  - Applies to an overflow clip with no border radius whose clipping element's transform is a scale
    plus translation (S16's `axisMapOf` valid, no rotation, skew or perspective).
  - It clips with its projected rectangle as a scissor, edges rounded to the nearest target pixel,
    whatever the content holds (layers, filters and text included). No stencil mask is drawn.
  - The projected rectangle also culls text lines (`ElementText.cpp:324-329`) and S16-F4's element
    culling.
  - Rotated, skewed, perspective and rounded clips keep the stencil mask, or S16's analytic clip.
- **R12 Stencil bookkeeping per attachment.**
  - The mask version is tracked per depth-stencil attachment: layer 0's shared one, and each region
    layer's own.
  - A stencil-tested draw runs only on an attachment whose version is the current mask's; otherwise
    `ensureMask` redraws it first.
  - R5's `DONTCARE` store resets that attachment's version to "none".
  - An assertion hook (`RmlRenderer::setStencilAuditForTesting`) records every stencil-tested draw
    with its attachment and version, so a test can prove the invariant on any backend.

### Functional: where documents composite (U34, R57)

- **R13 Screen Space - Overlay (default).**
  - The scene's screen documents composite in the last pass that writes the main camera's output:
    the uber pass (after its full-screen draw, `RenderSystem::renderUber`, `:5357-5466`), the
    fixed-resolution blit, the Pixel Perfect Camera blit, or the scene-colour or MSAA redirect blits
    (`:20264-20283`).
  - They composite at output resolution, over the whole output viewport, Pixel Perfect bars included.
  - No pass is added.
  - `render_scale`, dynamic resolution, FXAA, CRT, VHS, glitch, pixelate and the other uber effects
    do not touch them.
  - With no post pass the camera pass is the output, and they composite there as today.
  - Layer scenes draw after it, as today.
- **R14 Screen Space - Camera.** A scene set to it composites in the camera pass before post, as
  today: the UI scales with `render_scale` and post applies to it.
- **R15 Screen space under every camera** (Screen Space - Overlay; a Camera scene keeps the placement
  through its camera, Deviations, Integration).
  - In game views (exported games, Play, the Game view, Game-view screenshots), screen documents fill
    the camera's viewport for `CAMERA_PERSPECTIVE`, `CAMERA_ORTHO` and `CAMERA_UI` alike.
  - The editor's Scene view keeps today's world-space canvas for UI and 2D scenes
    (`UIDocumentSystemRender.cpp:109-131`).

### Functional: MSAA (U37)

- **R16 Auto.**
  - The project setting accepts 0 = Auto: 1 sample on Android, iOS and HTML5, 4 on Windows, Linux
    and macOS.
  - New projects (the `Project` default, `editor/resources/templates/`) are Auto.
  - A `project.yaml` without the key decodes as 4, its meaning before.
  - 1, 2, 4 and 8 keep their meaning.
  - The prebuilt player's default becomes 0 (`PlayerConfig.h:104`).
- **R17 Per scene.**
  - Scene setting `htmlUiMsaaSamples`: 0 = Project Default, or 1, 2, 4, 8. It drives the scene's
    screen context.
  - It is settable at runtime; the next frame recreates the target and draws once.
- **R18 Per document.**
  - `UIDocumentComponent::msaaSamples`: 0 = the scene's, or 1, 2, 4, 8. It is used by texture-mode
    documents, which have their own context.
  - Screen-mode documents share the scene's context, and the field has no effect there. The
    Inspector hides it in screen mode, and an MCP edit of it on a screen document returns a `next`
    saying the scene setting decides.
- **R19 Resolution per context.**
  - The static `preferredSampleCount` (`HtmlUIContextRender.cpp:40`) becomes a per-context value:
    the component's, else the scene's, else the project's (Auto resolved for the running platform),
    clamped to the device (`FramebufferRender::getMaxMsaaSamples`).
  - `UIDocumentSystem::setPreferredSampleCount` stays as the project-level setter.

### Editor UX (human)

- **R20** Settings and the overlay:
  - The scene Inspector (`editor/window/InspectorWindow.cpp`, where the scene settings live) gets an
    "HTML UI" group:
    - **Render Mode** (Screen Space - Overlay / Screen Space - Camera);
    - **Anti-aliasing (MSAA)** (Project Default / Disabled / 2x / 4x / 8x).
  - Edits are undoable through `ScenePropertyCmd`.
  - The UIDocument Inspector shows **Anti-aliasing (MSAA)** (Scene Default / Disabled / 2x / 4x / 8x)
    in Texture mode.
  - Project Settings > Graphics > HTML UI MSAA gains **Auto**, with the hint "1 sample on phones and
    the web, 4 on desktop".
  - The Game view's UI stats overlay shows kept frames, passes, layer cache hits and bytes, and
    composite pixels.

### Agent UX (MCP, Lua, CLI)

- **R21** MCP: no new tool.
  - `set_scene_settings` and `get_scene_settings` carry `htmlUiRenderMode` and `htmlUiMsaaSamples`
    through the scene-property catalog (`editor/mcp/tools/SceneTools.cpp`, as `sceneColorScale`).
  - Component edits carry `msaaSamples`.
  - `inspect_ui render_stats` and `get_render_stats` return R6's fields.
- **R22** Lua:
  - `Scene.htmlUiMsaaSamples`, and `Scene.htmlUiRenderMode` with `UIRenderMode.OVERLAY` /
    `UIRenderMode.CAMERA`, as properties beside `Scene.sceneColorScale`
    (`engine/core/script/binding/CoreClassesLua.cpp:3461`).
  - `UIDocumentComponent.msaaSamples`.
  - `getRenderStats()` returns the new fields.
  - Stub fragments go in `engine/core/script/binding/stubs/`, and `docs/mcp/guide/lua-api.md` is
    updated.
- **R23** Docs:
  - The cost paragraph of `docs/mcp/guide/html-ui.md` (`:1972-1976`) states S92's costs: idle 0
    passes; a change is 1 pass plus uncached layers; which constructs still layer.
  - Update `:401` (MSAA default) and `:2002` (the settings).
  - Update the render-mode and camera text of `html-ui.md`.
  - The fork ledger `docs/html-ui/FORK.md` gets an "S92 patches" table.
  - Regenerate `docs/mcp/guide/html-ui-support.md` if the support table's `clip-path` note changes.

### Functional: Lua overload dispatch (R66, S33)

- **R24 No pcall or string per miss.**
  - Each overload entry carries a no-raise argument check built from what `Stack<T>::get` accepts
    (the port's patch):
    - numbers and `const char*` through `Stack<T>::get`;
    - string-or-number for `std::string`;
    - `Userdata::isInstance<T>` or nil for a bound class;
    - "can't tell" for `bool`, enums, `LuaRef`, containers, `std::optional`, `std::string_view`,
      `lua_CFunction` overloads and the engine's own `Stack` specialisations.
  - An overload whose check says no is skipped without a call.
  - No error table and no string is made unless every overload fails.
- **R25 Same choice, same message.**
  - Every call picks the overload stock LuaBridge picks.
  - When every overload fails, the message is byte-identical to stock. A second pass keeps the order,
    the `#%d` numbering and the stored body errors.
- **R26 No leak.** No C++ object with a non-trivial destructor is live in any frame `lua_error`
  `longjmp`s over in LuaBridge's call paths: overloaded misses, all-fail calls, wrong calls of
  non-overloaded functions (`tupleize`), and `raise_exception_error` inside `catch`
  (`LuaBridge.h:6554, 6627, 6669`). Arguments decode into a result. The raise happens after the
  decoded values are destroyed and outside any handler.
- **R27 Guard.** In debug builds, `LUABRIDGE_ASSERT` checks that an overload a check rejected does
  fail to decode, and that the second pass never reaches a success.
- **R28 Ledger.** `engine/libs/luabridge3/PATCHES.md` lists S34 D6 (`2a032453b`), `583652b17` and the
  S92 changes, with what each changes and its test.

### Performance (concrete budgets)

- **R29 Passes per frame** (`HtmlUIRenderStats::passes` of the context, one document at 1280x720,
  pixel ratio 1, 4 samples unless said). The fixtures are `engine/tests/htmlui/fixtures/s92/`; each
  reproduces an M pattern with the port's constructs.

  | Fixture / state | Cold (first frame) | Warm (another element changed) | Idle (nothing changed) |
  |---|---|---|---|
  | `static.html`: boxes, text, no layer | 1 | 1 | **0** (0 draws, `framesKept` 1) |
  | `rounded-gradient.html`: rounded box, 7-stop px gradient, 4 layers | 1 (0 layers) | 1 | 0 |
  | `btn.html`: `.btn` (border, radius, 4 gradient layers, outer box-shadow, `drop-shadow` filter) | ≤ 4 | 1 (1 cache hit) | 0 |
  | `card.html`: octagon `clip-path` card, conic band, `drop-shadow` glow, text | ≤ 7 | 1 | 0 |
  | `inset-clip.html`: `clip-path: inset(... round ...)` and `circle()` | 1 (0 layers) | 1 | 0 |
  | `cards4.html`: 4 power-up cards and a timer label changing each frame (port: 206 at `7f48d7e8d`) | ≤ 1 + 4 x 6 | **1** | 0 |
  | `o2-frame.html`: 5 gradients on rounded boxes (port: 30) | 1 | 1 | 0 |

- **R30 Layer area.** In every fixture, `layerPixels` ≤ 1.5 x the sum of the layers' windows rounded
  up to 64-px granules. A layer whose window lies inside the viewport is never the size of the whole
  target.
- **R31 Composite area.**
  - `compositePixels` ≤ the area of the tiles with content.
  - `hud.html` (content on about 10 % of the view): ≤ 20 % of the view.
  - An empty visible document: 0 composite draws.
- **R32 Memory.**
  - At Auto on a phone (1 sample), a 2.6 MP document target is about 21 MB (RGBA8 + D24S8), against
    83 MB plus the resolve at 4 samples. `targetBytes` reports it.
  - The layer cache stays within R9's budget (`layerCacheBytes`).
- **R33 CPU.**
  - The signature (R1) costs ≤ 5 % of the record-and-plan CPU time in `HtmlUIRenderPerf.B1Examples`
    and `B2List500`. This is reported through `perfSlack()`, not a direct wall-clock assert.
  - A kept frame's CPU time ≤ that of the same frame drawn.
- **R34 Lua calls** (`LuaBindingBench`, dev host, through `perfSlack()`):
  - A call reaching overload k through misses costs ≤ 2x + 1 µs a call of overload 1 of the same
    set. Forms: `setShaderUniform(name, float)`, `Quaternion * Vector3`, `Vector3(Vector4)`.
  - A first-overload call costs ≤ 1.1x stock.
  - A wrong call costs ≤ 1.1x stock.

### Platform and backends

- **R35** Every requirement works on GL 4.1, GLES3/WebGL2, Metal, D3D11 and Vulkan through
  sokol-gfx, with no compute, SSBO or indirect draw.
  - Tiles are a small dynamic vertex buffer.
  - Cached results and region layers are RGBA8 textures.
  - R7's stop table is an RGBA8 texture read with `texelFetch` (no filtering; GLES3 and WebGL2 have it).
  - `DONTCARE` actions are what sokol maps per backend.
- **R36** Disabled is free:
  - no visible document: nothing is drawn or allocated;
  - no layer: the cache allocates nothing;
  - Screen Space - Camera: no change to the uber pass.

### Data and compatibility

- **R37** New fields are optional with defaults:
  - scene `htmlUiRenderMode` (default `overlay`) and `htmlUiMsaaSamples` (default 0). Absent used to
    mean the camera pass, so the render mode bumps the scene format (9 -> 10, after S98's 8 -> 9): an older scene gets
    `htmlUiRenderMode: camera` written in by its migration, and a cooked record older than 1.9 reads
    as Camera (Deviations, Integration);
  - component `msaaSamples` (default 0);
  - the project's `htmlUiMsaaSamples` accepts 0 (a missing key decodes as 4).
  Lists to update:
  - `editor/StreamSchema.cpp` (scene, component and project key lists);
  - `RuntimeFields.gen.inc` and the reflection parity goldens, regenerated;
  - the templates in `editor/resources/templates/`, regenerated.
  `tools/test-projects/upgrade-from-main/` must load unchanged.

## 7. Design constraints and decisions

- **D1 A frame signature, not dirty tracking.**
  - Whether a frame changed is decided from what RmlUi recorded, which is correct by construction.
  - The alternative was a fork-wide "render dirty" flag set by every style, layout, scroll, animation,
    data-view and decorator change. It is unsafe to complete, and a missed hook shows stale UI.
  - The price is that RmlUi's render traversal still runs on the CPU every frame. S16-F4 culling and
    S16-F6 clip memos make it cheap.
  - R33 measures it.
- **D2 Serials, not pointers.** Geometry and textures get 64-bit serials at creation. An address
  reused after a release can never make a stale match.
- **D3 64-px tiles, runs, one draw.** The tile size matches the region granule. Runs keep the vertex
  count small (≤ 2 x rows of tiles). A dense document degenerates to one quad per row, not to
  thousands.
- **D4 Overlay is the default for new scenes; old ones keep Screen Space - Camera.**
  - This is Unity's default, and it is the owner's requested U34 behaviour.
  - An absent key changes meaning, so older scenes are migrated to the mode they drew in
    (Deviations, Integration: the lead's decision replaced "every scene, old ones too").
  - A new scene's UI is crisp and unaffected by `render_scale` < 1 or uber effects; one that wants its
    UI under CRT or VHS sets Screen Space - Camera, and an old one switches to Overlay by hand.
  - The tests pinning today's placement are updated in the same commit and say so.
- **D5 R57 in game views only.** The Scene view's world-space canvas is a feature: it is how a UI
  scene is edited in place. Game views follow Unity's overlay canvas, which ignores the camera.
- **D6 MSAA Auto: 1 on phones and the web, 4 on desktop.**
  - SDF primitives and analytic clips antialias at any count.
  - The port compared 1 and 4 on five games: the mean difference was at most 0.077 of 255, all of it
    moving content.
  - Old projects keep their stored value; a missing key stays 4.
  - This supersedes S16 D7 for Auto only, with its own golden pass: the htmlui suite at one sample,
    as S16's `HtmlUIRenderPerf.OneSampleReport`.
- **D7 No `EXT_multisampled_render_to_texture`.** sokol-gfx does not expose it, and Auto makes it
  moot for the default. A project that asks for 4 samples on a phone pays for them.
- **D8 Per-document MSAA for texture-mode documents only.** Screen documents of a scene share one
  context and one target. Per-document counts there would need one context per document, which costs
  more than it saves.
- **D9 Gradient stops in a table texture.**
  - Up to 16 stops per gradient live in a per-renderer RGBA8 table: one row per compiled gradient,
    each stop a colour texel and a 16-bit position split over two channels.
  - The `HTMLUI_PRIM` instance carries the row.
  - The shader loops over the row's stops with `texelFetch`. There are no implicit derivatives in
    the loop (`tools/check_shader_lod.py`).
  - The four-stop instance path of S16-F5 stays for gradients that fit it. The table serves the rest.
- **D10 Cache by signature, not by element.**
  - The cache does not need element identity across frames. An element that moves keeps its
    window-relative signature and hits.
  - Two identical buttons share one entry.
  - The opacity of a fade is applied at the composite (R9), the most common animation over a filter.
- **D11 Rounded scissors.** R11 rounds the projected rectangle's edges to the nearest pixel. Where an
  edge falls inside a pixel, the 4-sample stencil gave partial coverage. The difference is at most
  half a pixel of one edge. The goldens that change are reviewed one by one (AGENTS.md).
- **D12 LuaBridge: the port's patch, then the root fix.**
  - The patch applies cleanly. It is taken as is for R24 and R25, then R26 and R27 are added on top.
  - Its standalone harness is not vendored. Its cases become `LuaOverloadDispatch.*`.
- **D13 Bindings are not touched.** `Quaternion(nil)`, `Quaternion(Vector3)` and the `__tostring`
  crash (ledger S32) are binding bugs owned by S95. S92 changes only `LuaBridge.h`.
- **D14 No CPU skip of `context->Render()`.** See D1. It is a follow-up if R33 shows that a static
  M-like HUD's traversal costs more than 0.3 ms on the reference host.
- **D15 No damage-rectangle redraw.** A changed document is one pass. A partial redraw saves fill
  inside that pass, but it needs per-command diffing across layers. Recorded as a follow-up.
- **D16 `drop-shadow()` is not converted to a shadow primitive.** R9 makes it free after the first
  frame for every content, not only for a single box.

**API sketch**

```cpp
// engine/core/htmlui/HtmlUIContext.h
void setSampleCountOverride(int samples);   // 0: the scene's / project's (R19)
void requestRedraw();                       // R2: custom native content changed

// engine/core/Scene.h (scene settings)
enum class UIRenderMode { OVERLAY, CAMERA };
void setHtmlUiRenderMode(UIRenderMode mode);  UIRenderMode getHtmlUiRenderMode() const;
void setHtmlUiMsaaSamples(int samples);       int getHtmlUiMsaaSamples() const;   // 0: project

// engine/core/component/UIDocumentComponent.h
int msaaSamples = 0;   // TEXTURE mode: 0 = the scene's
```

```lua
scene.htmlUiMsaaSamples = 1
scene.htmlUiRenderMode = UIRenderMode.OVERLAY
local s = doc:getRenderStats()   -- s.framesKept, s.passes, s.layerCacheHits, s.samples ...
```

## 8. Acceptance criteria

Rendering tests run with `tools/build.sh htmlui-tests` (glcore and `HTMLUI_BACKEND=vulkan`), with a
`--gtest_filter` per suite (the program's rule: no full suites). Suite names follow
`docs/testing.md` "Naming". Every behaviour change below has a test that fails on current main: that
run is the red control, recorded in the Deviations section at implementation.

| AC | Requirement | Verification |
|---|---|---|
| AC1 | R1 | `RenderFrameReuse.StaticDocumentDrawsNothing`: `s92/static.html`, 3 frames. Frames 2 and 3 have `passes` 0, `drawCalls` 0, `framesKept` 1, and the composited image equals frame 1's. Red: main gives `passes` 1 |
| AC2 | R1, R2 | `RenderFrameReuse.EveryChangeRedraws`: one case per change: text, class, style property, transform animation, scroll, image load, data-model value, hover, focus caret, video frame, time-based effect, `<particle-system>`, glyph page growth, resize, sample count. The frame after each has `framesKept` 0, and the image matches a freshly loaded document in that state. Cases that settle return to `framesKept` 1 |
| AC3 | R3 | The golden suites (`*Golden*`, `RenderP1Test.*`, `RenderPrimitive.*`, `RenderClip.*`, `RenderFilter*`, effects goldens) run with `JASPER_HTMLUI_VERIFY_KEPT=1` and two frames per golden: `keptMismatches` 0 and no `kept frame differs` line, on glcore and Vulkan |
| AC4 | R4, R31 | `RenderFrameReuse.CompositesOnlyDrawnTiles`: `s92/hud.html` gives `compositePixels` ≤ 0.2 x the view; an empty document gives 0 composite draws; the scene image equals the full-quad composite (`HtmlUIScene` golden, unchanged) |
| AC5 | R5 | `RenderFrameReuse.DepthStencilDiscarded`: a frame without stencil use has `depthStencilStores` 0; a frame with a rotated rounded clip has `depthStencilStores` ≤ the passes that wrote the mask minus 1; goldens unchanged |
| AC6 | R6, R21, R22 | MCP `tools/mcp/tests/test_html_ui_render_cost.py` (`CI = True`): `inspect_ui render_stats` and `get_render_stats` list every new field. A Lua case reads `doc:getRenderStats().framesKept` in Play through the Console |
| AC7 | R7 | `RenderPrimitive.GradientsWithManyStops`, `.ConicAndRepeating`, `.EveryLayerOfARoundedBox`: `layersPushed` 0 and `passes` 1. New browser-parity fixtures `p3/paint/gradient-rounded-16-stops`, `gradient-conic-rounded` and `gradient-hard-px-stops` against Chromium references (`tools/htmlui/render-reference.mjs`); the existing `p3/paint/gradient-*` fixtures pass unchanged |
| AC8 | R8 | `RenderClip.ClipPathBasicShapesAreAnalytic`: `inset() round`, `circle()`, `ellipse()` give `layersPushed` 0. Browser-parity fixture `p3/paint/clip-path-basic-shapes`; `polygon()` still layers, golden unchanged |
| AC9 | R9 | `RenderLayerCost.CacheHitsWhileUnchanged`: the frame-1 miss and frame-2 hit (another element changed) are byte-identical; an opacity fade 1 → 0 over 10 frames gives 10 hits; a moving element hits; a changed child misses once; a live effect inside is never cached. `RenderLayerCost.CacheBudget`: a fixture over 8 MiB evicts LRU and stays ≤ the budget. The leak test counts (`TestRenderRenderer` live-texture counts) return to 0 after unload |
| AC10 | R29 | `RenderLayerCost.ArcadePatterns`: every row of R29's table at its budget, at 4 samples and at 1. The first commit runs it on main and records the numbers in Deviations (the red control; main is expected over budget on `btn`, `card` and `cards4`) |
| AC11 | R10, R30 | `RenderLayerCost.LayersSizedToTheirElement`: `layerPixels` within R30's bound in every s92 fixture. `TestRenderFilterRegion.*` and `TestRenderFilterClip.*` goldens unchanged; RmlUi's `EffectHooks.*` unit tests pass |
| AC12 | R11, R12 | `RenderClip.TextInScaledScrollWithFilterLayers`: the U27 layout (scaled panel, `overflow: hidden` scroll viewport, items with `filter`, text in clipped rows) gives text clipped (golden), `stencilDraws` 0, and lines culled. `RenderClip.RotatedClipKeepsStencil`: a golden. `TestRenderRecord`'s `StencilAuditHoldsAcrossPassBreaks`: every audited draw's attachment version equals the mask version, with filter layers interleaved. The U27 layout is added to the Android render gate (`tools/android-emulator-test.sh --scene`). On a real Mali or Adreno device, "unverified" unless one is available (risk 4) |
| AC13 | R13, R14 | Scene composite tests in `engine/tests/htmlui/TestGoldenScene.cpp`: `HtmlUIScene.OverlayIgnoresRenderScale` gives UI pixels identical at `render_scale` 0.5 and 1.0 over a flat background (red on main). `.OverlayIgnoresFxaaAndCrt` likewise. `.CameraModeKeepsOldPlacement` reproduces main's scaled image. `.PixelPerfectBarsCoveredByOverlay` puts the UI over the bars. Run with `UNIT_TEST_ARGS`/`--gtest_filter` on glcore, Vulkan and `GRAPHIC_BACKEND=gles3` where the scene suites run |
| AC14 | R15 | `HtmlUIScene.OrthoCameraFillsViewport` (camera size 22, the R57 case) and `.UiCameraPannedFillsViewport` show the document at screen pixels (red on main). MCP `test_html_ui_render_cost.py::test_scene_view_keeps_world_canvas` takes a Scene-view screenshot of a 2D scene and matches the canvas position |
| AC15 | R16-R19, R37 | Editor unit tests in `editor/tests/stream/StreamProjectHtmlUiRenderTest.cpp`: a missing key gives 4, 0 round-trips, and the generated `main.cpp` and prebuilt config carry 0. Scene and component key round trips are in their stream tests. `HtmlUIContext.AutoSampleCountPerPlatform` checks the platform table. The upgrade-from-main project loads (`tools/check-changed` picks it) |
| AC16 | R17, R18, R20, R21, R22 | MCP `test_html_ui_render_cost.py`: `set_scene_settings htmlUiMsaaSamples=1` gives `samples` 1 in `inspect_ui render_stats`, and undo restores 4. A texture-mode component with `msaaSamples` 2 gives 2. A screen-mode edit returns the `next` hint. `htmlUiRenderMode` round-trips with undo. Lua: `scene.htmlUiMsaaSamples = 1` in Play is seen through the Console. The scene Inspector edit is undoable (editor test on `ScenePropertyCmd`) |
| AC17 | R24, R25 | `engine/tests/core/script/LuaOverloadDispatchTest.cpp`: the port's cases (`overload_dispatch_test.cpp`: 32 Lua values against sets of every parameter kind, wrong `self`, bodies that raise or throw, C-function overloads, constructors, a coroutine, arity skips) with messages pinned to the strings stock LuaBridge gives, captured before the change. `LuaOverloadDispatch.LaterOverloadBuildsNoErrorString` counts the Lua allocator's string allocations for `setShaderUniform(name, float)` on a test class: stock > 0 (red), patched 0 |
| AC18 | R26 | `LuaOverloadDispatch.NoLeakOnMiss`, `.NoLeakWhenEveryOverloadFails`, `.NoLeakOnWrongCallWithoutOverloads` (`AllocationCounter`, 2000 calls, a 64-character `std::string` placed after the failing parameter so g++ shows it): net 0; red on main for the second and third |
| AC19 | R27, R28 | A debug-build run of AC17 trips no assert. `PATCHES.md` exists and names every local change (review) |
| AC20 | R34 | `LuaBindingBench.OverloadMisses` prints stock-relative ratios. The ratio checks go through `perfSlack()`; the numbers go in Deviations |
| AC21 | R33 | `HtmlUIRenderPerf.B1Examples`/`B2List500` report the signature's share through `perfSlack()`; numbers in Deviations |
| AC22 | R35 | `tools/build.sh web` and `tools/build.sh android` build. `tools/build.sh web-gate` loads an HTML UI page with a kept frame and a cached filter. `python3 tools/check_shader_lod.py` passes. `tools/check-changed` passes |
| AC23 | R23 | Docs updated. `tools/mcp/gen_tool_docs.py` and `gen_html_ui_support.py` rerun, with no diff unless text changed. `tool_budget.py --check tools/mcp/budget.json` passes. If any tool or guide text is added, a case in `tools/mcp/eval/cases.py` fails without it (AGENTS.md "MCP text") |

## 9. Work packages

Five packages, each in its own worktree and branch. Shared files have one owner per region, named
below; anything else in a shared file is a two-line hook that the owner reviews at merge.

| WP | Name | R | Owns | Shares (and how) | Size | Order |
|---|---|---|---|---|---|---|
| A | Frame reuse and tile composite | R1-R6, R29 (`static`, idle column), R31, R33 | `engine/core/htmlui/HtmlUIContext.cpp` (`update`), `HtmlUIContextRender.cpp` `render()` body (`:77-170`), new `engine/core/htmlui/RmlRendererFrame.cpp` (signature, live sources, tiles, verify mode), `HtmlUITexture.h` (serial, content version), `RmlRenderer.cpp` `beginFrame`/`endFrame`/`beginPass`/`endPass`/`drawComposite` (`:1382-1424`, `:3293-3481`); tests `TestRenderFrameReuse.cpp` | `HtmlUIRenderStats.h`: **A adds every R6 field in its first commit** (B2 and C only fill them); `RmlRenderer.h`: A's members in a block marked `// S92 frame reuse`; `RmlRendererBatch.cpp`: one call in `executePlan` (A) | L | First. Its first commit (signature over a command range, serials, R6 fields) is what B2 builds on |
| B1 | Gradients and clips | R7, R8, R11, R12 | fork `DecoratorGradient.cpp`, `ElementBackgroundLayers.cpp` `MakeGradientTiles` (`:651-734`) and `Generate` (`:575-595`), `ElementUtilities.cpp` (`:150-215`), `ElementText.cpp` culling (`:320-340`), the compat `clip-path` lowering (`compat/css/props/Background.cpp` `clip-path` branch); `HtmlUIPrimitive.h`; `RmlRendererBatch.cpp` `toInstance`, `SupportsGradientPrimitive`, `planClipScopes` (`:330-410`, `:600-800`); the `HTMLUI_PRIM` GLSL in `engine/shaderlib/`; `RmlRenderer.cpp` `ensureMask`/`maskVersionOf`/`bindLayer` (`:1534-1571`, `:1936-1990`); tests in `TestRenderPrimitive.cpp`, `TestRenderClip.cpp`, `TestRenderRecord.cpp`, the p3 parity fixtures | `FORK.md` "S92 patches": B1 uses ids S92-F1..F3, B2 S92-F4..F6; `ElementText.cpp`: B1 owns `:320-340`, B2 owns `:410-430` | L | Any time; independent of A |
| B2 | Sized layers and result cache | R9, R10, R29 (cold and warm columns), R30 | new `engine/core/htmlui/RmlRendererLayerCache.cpp`; `RmlRenderer.cpp` `PushLayerRegion`/`pushLayerNow`/`PopLayer`/`compositeLayersNow`/`applyFilter` (`:2060-2393`); fork `ElementEffects.cpp` (`:380-515`), `ElementBackgroundLayers.cpp` `Render` (`:747-810`), `ElementText.cpp` shadow layer (`:410-430`); tests `TestRenderLayerCost.cpp`, fixtures `engine/tests/htmlui/fixtures/s92/` | `RmlRenderer.h`: block `// S92 layer cache`; uses A's signature helper | L | After A's first commit (branch from it or rebase onto it) |
| C | Composite stage and MSAA settings | R13-R23, R32, R37 | `engine/core/subsystem/RenderSystem.cpp` (document composite placement at `:5357-5466`, `:20159-20283`), `UIDocumentSystemRender.cpp`, `UIDocumentSystem.{h,cpp}` (sample settings), `HtmlUIContextRender.cpp` `:40-71` and `HtmlUIContext.h` (per-context samples), `Scene.{h,cpp}`, `UIDocumentComponent.h`, `editor/Stream.cpp`, `StreamSchema.cpp`, `Project.cpp`, `settings/SettingsRows.cpp`, `Generator.cpp`, `ExporterPrebuilt.cpp`, `PlayerConfig.{h,cpp}`, `InspectorWindow.cpp` and the UIDocument Inspector, `SceneTools.cpp` catalog, `CoreClassesLua.cpp`/`HtmlUIClassesLua.cpp` and stub fragments, `docs/mcp/guide/html-ui.md` and `lua-api.md`, regenerated templates and `RuntimeFields.gen.inc`; tests `TestGoldenScene.cpp`, stream tests, `test_html_ui_render_cost.py` | `HtmlUIContextRender.cpp`: C owns `:40-71`, A owns `render()`; the guide's cost paragraph is written by C from A and B2's numbers | L | Any time; the docs paragraph last |
| D | Lua overload dispatch | R24-R28, R34 | `engine/libs/luabridge3/LuaBridge.h`, new `engine/libs/luabridge3/PATCHES.md`, `engine/tests/core/script/LuaOverloadDispatchTest.cpp`, `LuaBindingBenchTest.cpp` | none in S92 | M | Any time; fully independent |

Merge order: A, then B1, B2, C and D in any order. Run `tools/check-changed` after each merge, and
the R29 table (AC10) after A, B1 and B2 are all in.

## 10. Dependencies and conflicts

- **Builds on S16** (`docs/specs/S16-ui-render-text.md`) and the fork patches S16-F1..F6, P7-F1 and
  P7-F2 (`docs/html-ui/FORK.md:721-763`). S92 changes S16 D7 for Auto only (D6).
- **S93 (UI)**:
  - U30 and U31 touch `RmlFontEngine.cpp` and `RmlRenderer::LoadTexture` (`RmlRenderer.cpp:994-1070`).
    A's texture content version is bumped in the upload paths S93 may move: whoever lands second
    keeps the bump.
  - U22 (inset shadows under gradients: `ElementBackgroundBorder`, S16-F1) and U23 (`calc()` in
    gradient stops: compat `Background.cpp`, `DecoratorGradient.cpp`) overlap B1's files.
  - U10, U11 and U14 (filters on border images, `mix-blend-mode`, tint) overlap B2's composite and
    cache: a new blend mode must be part of the cache key.
  - U26 (caches keyed per render manager) agrees with R9, whose cache is per renderer.
  - U2 and U13 (hit tests under transforms) are input-only, with no conflict with R11.
  - U3 (the animation clock): animations change recorded geometry or transforms, so R1 redraws. S93
    should rerun AC2 after its change.
  - U9 (headless labels changing each frame) should be retested after A.
- **S94 (rendering)**: `RenderSystem.cpp` is a hotspot. C moves the document composite into the
  uber, fixed-resolution and redirect passes (`:5357-5466`, `:20159-20283`). S94's `render_scale`,
  post and camera items must not move it back. Merge C early, or coordinate on those line ranges.
- **S95 (Lua)**:
  - S32 and S1 change `engine/core/script/binding/MathClassesLua.cpp`; D changes only `LuaBridge.h`.
    S1's enum ambiguity stays "can't tell" in R24's checks, so S95's fix (a distinct factory) is
    needed regardless.
  - S28 (`renderOffscreen` outside a frame) and S30 (Android background rebuild: `HtmlUIContext::destroy`,
    `HtmlUIContext.cpp:219-242`) touch the lifetimes of A's kept target and B2's cache: both must be
    dropped at `releaseGpuResources` and destroy (R9).
  - C adds properties to `CoreClassesLua.cpp` and `HtmlUIClassesLua.cpp`: append only.
- **S98 (editor, MCP)**: the Game view stats overlay (R20) and `SceneTools.cpp` (R21) may be touched
  by E items. Append only.
- Generated files touched: `RuntimeFields.gen.inc`, `editor/resources/templates/`, and possibly
  `docs/mcp/TOOLS.md` and `html-ui-support.md`. Regenerate them, never edit them by hand.

## 11. Risks and defaults

1. **A live source the signature misses shows a stale frame.**
   - Default: R2's list plus `requestRedraw()`.
   - AC3's verify mode over every golden is the net.
   - The test toggle `RmlRenderer::setFrameReuseForTesting(false)` turns R1 off. It is not a user
     setting: a stale frame is a bug to fix, not an option.
2. **Signature cost on huge documents.** Hashing is linear in commands. The B2List500 and B7 virtual
   list benchmarks bound it (R33). If it exceeds 5 %, hash per S16 run chunk and reuse the run
   cache's hashes.
3. **Cache memory on phones.** The default is 8 MiB with LRU eviction; `layerCacheBytes` is visible.
   A project setting is added only if a game needs it (not now).
4. **The U27 root cause on tile GPUs is unconfirmed.**
   - R11 removes the stencil path for the port's case. R12 makes the rest auditable.
   - The Android emulator does not reproduce it. Without a real Mali or Adreno device, the real-device
     check stays "unverified" in the report.
5. **Overlay by default would change the look of scenes whose UI went through post** (CRT, VHS,
   `render_scale` < 1). Old scenes are migrated to Screen Space - Camera (D4), so only new scenes get
   Overlay; the guide names both modes.
6. **R11's rounded scissor changes goldens with fractional clip edges under scale.** Every changed
   image is reviewed, and the commit says why (AGENTS.md "Rewrite goldens one suite at a time").
7. **Gradient table precision.** A 16-bit position gives 1/65536 of the gradient line: under 0.01 px
   for lines up to 600 px. Hard px stops stay hard. AC7's parity fixtures check it.
8. **More region framebuffers.** Every element layer now comes from the pool. `trimRegions` frees
   unused sizes; `regionTargetsCreated` is watched in AC11, steady at 0 after warm-up.
9. **Lua: a check that says no wrongly would skip a valid overload.** R27's debug assert, and AC17's
   cases covering every parameter kind, guard it. The default for a type the check cannot judge is
   "can't tell" (run it).
10. **Vendored LuaBridge drifting from upstream.** `PATCHES.md` makes a future upgrade reapply three
    named changes.

## Deviations (implementation record)

### S92-A (frame reuse and tile composite)

- **AC10 red control.** `RenderLayerCost.ArcadePatterns` on `2601cb641` (the first commit: main's
  renderer with the new statistics), glcore (llvmpipe), 1280x720, pixel ratio 1. Passes per frame,
  cold / warm / idle:

  | Fixture | 4 samples | 1 sample |
  |---|---|---|
  | `static` | 1 / 1 / 1 | 1 / 1 / 1 |
  | `rounded-gradient` | 12 / 12 / 12 | 10 / 10 / 10 |
  | `btn` | 15 / 15 / 15 | 13 / 13 / 13 |
  | `card` | 32 / 32 / 32 | 28 / 28 / 28 |
  | `inset-clip` | 12 / 12 / 12 | 10 / 10 / 10 |
  | `cards4` | 128 / 128 / 128 | 112 / 112 / 112 |
  | `o2-frame` | 1 / 1 / 1 | 1 / 1 / 1 |

  S16-F5 already draws the O2 frame's gradients (four stops at most) without a layer. With S92-A
  every idle cell is 0 passes, 0 draws, `framesKept` 1 (asserted); cold and warm are unchanged until
  B1 and B2 land.
- **AC21 (R33).** `HtmlUIRenderPerf.B1Examples` and `B2List500`, glcore, dev host under load: the
  signature takes 1.0-5.0 % of the recording and the plan (B1: blank 4.3, dialogue 3.5, hud 4.0,
  inventory 2.9, loading 1.0, main_menu 3.7, pause_menu 4.0, settings 3.0, shop 3.0, toasts 5.0,
  video_menu 3.8; B2 1.1 %; 0.001-0.011 ms a frame). With frame reuse, the static examples keep all
  120 measured frames and cost 0.005-0.062 ms of engine time against 0.094-0.698 ms replayed; the
  animated ones (loading, main_menu, toasts, video_menu) keep none and cost the same as replayed
  (within 0.02 ms). toasts sits at the 5 % bound: the gate takes `perfSlack()`.
- **AC2's oracle.** The image after each change is compared byte for byte with a fresh replay of
  the same frame (frame reuse off, same clock), not with a second document loaded in that state:
  it is the image the frame would have without R1, which is what a stale frame would differ from.
  Glyph page growth and the live sources (video, effect on a running clock, `<particle-system>`)
  are separate tests (`GlyphPageGrowthChangesTheSignature`, `LiveSourcesDrawEveryFrame`,
  `ParticleSystemElementTest.FramesWithParticlesAreAlwaysDrawn`).
- **R2's render-target source.** No HTML UI texture wraps an engine render target today. Any
  EXTERNAL texture that is not a glyph page's variant reports `LIVE_RENDER_TARGET`, so one added
  later is live without further work.
- **R3 timing.** GL compares a kept frame with its replay at once; Vulkan, Metal and D3D11 read back
  once the frame is committed (the next frame, or `RmlRenderer::checkKeptFrame`). The harnesses skip
  the verification frame after a frame with a live source (it is never kept, and a stateful effect
  would step).
- **R4 tiles.** A layer composited into layer 0 marks what its own draws covered, spread by its
  filters (6 sigma of a blur, a drop shadow's offset), within the composite's scissor; a backdrop
  or an effect filter marks the whole scissor. The tile vertex buffer is written at most once a
  sokol frame; a second change in the same frame composites the whole quad.
- **R5 plan.** The plan is per replay position (an op of the batched plan, else a command). A pass
  stores its depth-stencil only when a stencil use follows the next position that surely starts
  another pass; one with no stencil use after it also loads `DONTCARE`. A depth-stencil stored
  `DONTCARE` and bound again is cleared and its mask version reset, so a wrong guess costs a mask
  redraw, never a wrong clip. AC5's bound is checked against the same frame with every
  depth-stencil stored (`setDepthStencilDiscardForTesting(OFF)`): fewer stores, the same image. R12's
  part in R5 (left to S92-A by B1): a `DONTCARE` store resets that depth-stencil's mask version at
  once; with every store dropped (`EVERY_PASS`) each stencil test draws its mask again, the audit
  holds and the image is the same. Since B2 every element layer is a region layer with its own
  depth-stencil, so a dropped one is restored exactly: every stencil write is kept with its scissor
  and its attachment (a region layer's since its push), and a dropped and cleared depth-stencil
  replays its own writes and takes back the version it held before anything tests or writes it.
- **R1 and the replay tests.** Tests that count a replay's passes, draws or run-cache hits on
  repeated identical frames now turn frame reuse off for their scope (`FrameReuseMode`), and the S16
  benchmarks replay every frame (`S16Mode`); R33 is measured with reuse on and off.
  `RenderTestContext::renderUntilDrawn` ends on a replayed frame (its statistics are a drawn
  frame's: B1's tests read them), and B2's cache tests ask for replayed frames; the layer cache's
  switch is part of the frame signature.
- **Not S92-A's, found on the merged tree.** `FullPathGoldenTest.UIParticleSystemsClippedByAPanel`
  fails after the other FullPath goldens in one process (alone it passes) on main `fcfbc222d` too;
  MCP `test_html_ui_basic.py::test_child_scene_document` (a child scene's document at canvas
  640, 320) fails on the spec branch without S92-A too (`f3b98de40`, after C). After B2's follow-up
  (`07480e8ef`, a blurred drop shadow in four passes) `IntegrationTest.TextureDocumentDrawsClipsLayersAndFilters`
  misses its `p1/texture_effects` golden (0.69 % of the pixels, glcore and Vulkan, with the layer
  cache off as the test counts a frame's own layers); it passed on the merge before it.

### WP D: Lua overload dispatch (R24-R28, R34)

- **R26 reaches further than §3.7 listed.** Besides overloaded misses, all-fail calls, `tupleize` and
  the three `catch` blocks, the same longjmp left objects behind in: a bound class that does not
  match inside a container argument (the half-built `std::vector`), a property setter's value,
  every `catch` of the call thunks (nine, not three), the constructor proxies (`ec.message()`
  temporaries, the container of `container_forwarder`) and `read_only_error` (a `std::stringstream`).
  All are fixed (PATCHES.md, change 4). Not changed: registration-time errors (`ClassBase`'s
  `luaL_error(L, "%s", s.c_str())`), which run once when a state is made and are not call paths.
- **A bound class that does not match no longer raises inside a decode.** `Userdata::getClass`
  records `luaL_argerror`'s index and names in a per-thread record owned by the decoding frame (a
  never-reused light userdata token in its first slot past the arguments); the decoder raises the
  same `luaL_argerror` after the decoded values are destroyed. Arguments past the stack top are read
  one slot higher, so a missing argument still reads as "none". A pack of trivially destructible
  values keeps the stock decoder. Residual (inferred): a Lua memory error raised inside a decode
  leaves the record set, but its token never matches again, so it changes nothing.
- **AC17 fixture.** `engine/tests/core/fixtures/luabridge/overload-dispatch.txt` holds the 2473
  results of the port's matrix plus S92's leak paths, captured from the stock header with the test
  file built standalone by g++ 13 and clang++ 18. 640 calls name one of several failing arguments,
  which depends on the compiler's evaluation order (g++ right to left, clang left to right): they
  carry a `[gcc]` and a `[clang]` line; another compiler (MSVC) may give either (unverified there).
- **AC18 counts live instances, not allocations.** `AllocationCounter` counts allocations but not
  frees, so a net count is not available. The tests decode a 64-character text into `TrackedText`,
  a type that counts its live instances (a C++ object whose destructor never ran is exactly what R26
  forbids), on both sides of the failing parameter so either evaluation order shows a leak. The
  same paths with `std::string`, counted by operator new/delete in the port's standalone harness
  (2000 calls each), stock -> S92: g++ string-last wrong call 2000 -> 0, container element 4000 -> 0,
  const object to a pointer 2000 -> 0, throwing bodies 2000 and 2000 -> 0 (and a caught exception
  left on the runtime's stack -> none); clang++ overloaded misses 4000 -> 0, every overload fails
  8000 -> 0, string-first wrong call 2000 -> 0, number decode 2000 -> 0, container 2000 -> 0,
  throwing bodies 2000 and 4000 -> 0. The port's patch alone left all of these except the misses.
- **AC19 debug run** is standalone: the engine's core tests are RelWithDebInfo (`NDEBUG`), so the
  `LuaOverloadDispatch` tests were built without `NDEBUG` with g++ 13 and clang++ 18 against the
  engine's `LuaBridge.h`, gtest and Lua: 6 of 6 pass, no assert. The port harness's 6735 lines are
  byte-identical to stock in that build too, and under ASan and UBSan (g++).
- **Red controls (AC17, AC18).** Core tests with the stock header in place
  (`UNIT_SUITE=core UNIT_TEST_ARGS=--gtest_filter=LuaOverloadDispatch.*:LuaBindingBench.OverloadMisses`):
  `LaterOverloadBuildsNoErrorString` (3011 strings per 1000 calls), `NoLeakOnMiss` (4000 live),
  `NoLeakWhenEveryOverloadFails` (6000), `NoLeakOnWrongCallWithoutOverloads` (2000) and
  `NoLeakWhenABodyThrows` (`std::current_exception()` set) fail; `CallsMatchStockLuaBridge` passes.
- **AC20.** `LuaBindingBench.OverloadMisses` compares with stock's `try_overload_functions` copied
  into the test and run over the same overload tables, so its "stock" uses today's thunks: it
  measures the dispatch, not the decoding change. Stock as a whole was measured with the port's
  harness (stock header vs S92 header, same program, min of 14 runs on the shared dev host, ns per
  call):

  | Form | g++ stock | g++ S92 | clang stock | clang S92 |
  | --- | ---: | ---: | ---: | ---: |
  | `setU(name, Vec4)` overload 1 of 4 | 1211 | 1218 (1.01x) | 1193 | 897 (0.75x) |
  | `setU(name, float)` overload 4 of 4 | 204345 | 1121 | 241055 | 1299 |
  | `Vec4(Vec4)` overload 5 of 5 | 2253 | 593 | 2217 | 695 |
  | `pickF(Vec4)` overload 2 of 2 | 59516 | 911 | 71838 | 905 |
  | `pickA(int)` overload 1 of 3 | 546 | 499 | 624 | 469 |
  | wrong call, every overload fails | 146951 | 142170 (0.97x) | 133346 | 159179 (1.19x) |

  The clang wrong call is over R34's 1.1x on this host, where the port's patch alone (the same code
  on that path) measures 1.17x and runs vary by 2x under the load: inferred noise.

  `LuaBindingBench.OverloadMisses` in the engine's core tests (g++, build box, real bindings, ns per
  call, median of 7):

  | Form | overload 1 (vs stock dispatch) | later overload (stock dispatch) | later/first | wrong call (vs stock) |
  | --- | ---: | ---: | ---: | ---: |
  | `Mesh:setShaderUniform(name, float)` | 509 (0.94x) | 605 (87313) | 1.19 | 83714 (1.06x) |
  | `Quaternion * Vector3` | 320 (0.86x) | 581 (27918) | 1.82 | 53585 (0.97x) |
  | `Vector3(Vector4)` | 229 (0.50x) | 561 (27421) | 2.45 | 55640 (1.03x) |

  Every R34 budget holds there (later <= 2x + 1 us of overload 1, overload 1 and wrong calls
  <= 1.1x stock); enforced only with `JASPER_TEST_PERF_SLACK`.

### Package C (composite stage, MSAA settings)

- **Setting names.** `set_scene_settings` / `get_scene_settings` take `html_ui_render_mode` (`overlay`,
  `camera`) and `html_ui_msaa_samples`: the scene-property catalog is snake_case (`scene_color_scale`).
  The scene file keys are `htmlUiRenderMode` and `htmlUiMsaaSamples`, Lua `scene.htmlUiRenderMode` and
  `scene.htmlUiMsaaSamples`, as specified.
- **R13, Pixel Perfect bars.** The documents composite in the Pixel Perfect blit (the pass that draws the
  bars), at the screen's resolution over the view rect (the k x base image), not into the base image
  scaled up with it. They do not extend into the bars: the document's canvas is the view rect, and
  `FullPathGoldenTest.Letterbox` pins that letterbox bars stay untouched. Covering the bars would need a
  canvas the size of the screen, a layout change outside S92. `HtmlUIScene.PixelPerfectBarsCoveredByOverlay`
  checks the resolution (red before: 0 mixed 2 x 2 blocks, every block one colour).
- **R13, "no pass is added"** is inferred from the code (the documents draw inside the uber, blit, chain
  or SSR composite pass that writes the view); the htmlui suite's build records no pass list, so no test
  counts passes.
- **Cooked scenes.** The two scene settings are appended to the cooked scene settings record as format 1.6
  (`CookedScene.h` `FORMAT_MINOR` 6), so exported games carry them; a 1.5 record reads the defaults
  (`CookedScene.SceneSettingsKeepHtmlUi`).
- **R32 `samples`, `targetBytes`** are filled by `UIDocumentSystemRender.cpp` from the context's target when
  the statistics are read (layers: colour and depth-stencil at every sample plus the resolve image;
  single-sampled post-process targets; region layers), not by the renderer's frame.
- **AC16, Lua in Play.** `eval_lua` captures `print`, so the MCP test reads the printed line from its result
  and the resulting `samples` from `inspect_ui render_stats`, not from `read_console`.
- **R23, FORK.md.** Package C has no fork patch; the "S92 patches" table and its rows (S92-F1..F6) are
  B1's and B2's.
- **Red controls (package C).** With the placement reverted (no later-pass composite, the world canvas for
  every non-perspective camera): `HtmlUIScene.OverlayIgnoresRenderScale` (3644 pixels differ),
  `.OverlayIgnoresFxaaAndCrt` (230400), `.OrthoCameraFillsViewport`, `.UiCameraPannedFillsViewport` and
  `.PixelPerfectBarsCoveredByOverlay` fail; `.CameraModeKeepsOldPlacement`'s golden
  (`overlay_camera_mode_scaled.png`) was captured in that state.

### Package B2 (sized layers and the layer cache)

- **R9 key: a scope signature of its own.** `signatureOf` (A) hashes the state commands as recorded,
  so a scope keyed differently with the state its predecessors left (an element after a filtered one
  started with the scissor on, the first with it off) and by geometry serial (two equal elements are
  two geometries). The cache key (`RmlRenderer::scopeSignature`, `RmlRendererLayerCache.cpp`) hashes
  each draw with the state it is drawn in (scissor relative to the window, transform, clip), geometry
  by its vertices and indices (hashed once per compiled geometry), small static textures (at most 256
  KiB, a clip-path raster) by their texels, and reuses A's texture, filter and shader signatures and
  live-source bits. A glyph page's generation is part of it (A's texture signature): a scope holding
  text misses once when the page grows (`inset-clip`'s warm frame before B1 made its shapes analytic).
- **R9 window.** The result kept is the composite's scissor within what RmlUi asked the layer to cover
  (the region frame's granules around it are empty), so it moves with its element. A blur's halved
  levels start at its window (not at the frame's 64-px granules): a blurred result is the same
  wherever its element is, so equal elements share one entry. A window the viewport crops (an element
  at its edge) starts its levels at the crop: `TestRenderFilterRegion`'s edge blurs differ from the
  same square further in by up to 3 levels of 255, not 2.
- **R9 content keys.** A gradient of more than four stops keys by its stop-table row's stops (not the
  row); the clip mask a scope starts in keys only where a draw is tested against it. An equal scope
  later in the same replay draws the earlier one's result, so cards4's four glows store one.
- **R9 trailing opacity.** Applied in the composite's passthrough for cached scopes only (a miss and a
  hit draw the same image the same way: byte-identical); uncached composites keep the opacity pass.
- **R9 frames.** "Unused for 120 frames" counts replays (a kept frame, R1, replays nothing). A stored
  entry not drawn again in the next replay (its element changes every frame) is freed then, its image
  reused by the next store. An equal scope later in the same replay draws the earlier one's result (a
  row of equal buttons stores one); such a store is kept over the budget until the replay ends.
- **R10 rule.** A window within the viewport is a region layer unless its 64-px granules cover as much
  as the target (then an ordinary layer is no larger and shares layer 0's depth-stencil).
  `RenderFilterRegion.BlurReadsTheContentBeyondTheViewport`'s reference may now be a region layer.
  Backdrop composite fusion works between region layers (placed by their frames).
- **R30 measure.** `layerPixels` counts every layer pass but layer 0's, every post-process pass and the
  cache's copies; a layer's filter chain is several passes, so the bound is checked per pass: layerPixels
  <= 1.5 x the granule-rounded windows of the layers each pass serves (`getLastLayerPassStats`), and no
  pass of a layer within the viewport is the target's size.
- **R29 cold column, met (4x and 1x).** static 1, rounded-gradient 1, btn 3 (<= 4), card 7 (<= 7),
  inset-clip 1, cards4 7 (<= 25), o2-frame 1; `RenderLayerCost.ArcadePatterns` asserts them. Before
  (B2 merged with B1): card 32, cards4 128. What took them there:
  - convex `clip-path: polygon()` of at most 16 points clips in the shaders (S92-F6, `u_fs_htmluiClip`'s
    polygon, the nearest edge's screen distance as coverage): the octagon outer, band, fill and window
    push no layer (each was its layer's pass, the mask layer's, the mask copy and the mask filter);
  - a blurred drop shadow is drawn by the blur's first pass, two halvings go in one pass of four
    bilinear taps (the 4 x 4 box two passes gave, without their 8-bit intermediate), and the passes end
    in temp (no copy): the 10 px glow is 4 post-process passes;
  - card's 7: layer 0, the glow's layer, the glow's 4 post-process passes (shadow + two halvings,
    vertical, horizontal, upscale with the source over it), layer 0 again for the composite and the
    text. cards4: the same, the other three glows drawn from the first's result.
  Warm is 1 pass for every row, idle 1 (A's kept frame makes it 0).
- **U27 gap (from B1).** A filtered element whose window reaches past its ancestors' scissor is composited
  through a rectangle of that scissor; the rectangle now carries its shape (S92-F5) and a scope whose masks
  are all rectangles on the target's axes is analytic: the composite's passthrough clips by coverage at the
  pixel's centre (`params.w`, `texCoordMinMax`), no stencil. Editor shader cache v68.
- **Tests that pin per-frame layer work** run with the cache off (`LayerCacheOff`): TestRenderFilterRegion's
  region tests, `RenderPrimitive.MatchesTheMeshPath`, `.GradientBackgroundsNeedNoMaskLayer`,
  `.GradientsWithManyStops`, `RenderClip.ClipPathBasicShapesAreAnalytic`, `RenderBatch.NoMergeAcrossLayersFiltersShadersOrStencil`,
  `RenderP1Test.PassCounts`, `.BackdropCompositeFusedExactly`, `RenderRecordTest.StencilAuditHoldsAcrossPassBreaks`,
  `RmlRendererP1GoldenTest.LayersNested`. No golden image was rewritten: the htmlui golden, browser-parity
  and scene suites pass within their tolerances on glcore and Vulkan with the blur and polygon changes.
- **Polygon clip-path.** Only convex polygons (both fill rules agree there) of at most 16 points clip
  analytically; a concave or self-intersecting one keeps the mask layer
  (`RenderClip.ClipPathBasicShapesAreAnalytic` now pins that with a concave polygon). Editor shader cache v69.

### Package B1 (gradients and clips)

- **Red controls.** Run on main's renderer: the rounded-gradient and `clip-path` fixtures pushed 6 to 8
  layers in 16 to 25 passes; with B1 they push 0 layers in 1 pass (`RenderPrimitive.*`,
  `RenderClip.ClipPathBasicShapesAreAnalytic`). The U27 list (`RenderClip.TextInScaledScrollWithFilterLayers`):
  stencil draws 28 -> 0, text draws 41 -> 8 (lines culled under the scale).
- **Left to the other packages.** A filtered element whose window reaches past a scaled clip still
  composited through a stencil rectangle: B2 clips it in the shader ("U27 gap (from B1)" above). The
  depth-stencil `DONTCARE` mask-version reset went to A (R5 above).
- **Shader cache.** B1's `HTMLUI_PRIM` change took v67 because S93's branch holds v66 (a host's shared
  cache served one branch's program to the other's editor); B2 then took v68 and v69. `TilemapShaders`'
  pin of the version became a lower bound.
- **Android render gate.** The `htmlui-u27` scene was added to the gate's project and its scene list;
  its golden is written at integration (below), where the local host has KVM.

### Integration

- **Coordinator decisions.** U27, U34 and R57 moved into S92 from S93 and S94 (they live in the render
  and composite code S92 rewrites). The built-in shader cache is v69 here (main: v65; S93 holds v66,
  S94 may take v69/v70): the second of S92 and S94 to merge takes the next free number. The cooked
  scene format minor 1.6 (C) collides with S95's 1.6: the second to merge renumbers. S93's
  `mix-blend-mode` (U11) is built on main's composite: merge S92 first, then S93 re-merges main and
  makes region layers blend too (B2's cache already keys by blend mode). S95 keeps its
  `hardenClassTables` workaround in `LuaBinding.cpp` until S92 and S95 are both on main; D fixes both
  crashes inside LuaBridge (`04393ef4c`).
- **`p1/texture_effects` rewritten.** B2's four-tap halving (`07480e8ef`) makes the 6 px blur's outer
  ring reach one pixel further: 448 of the 451 differing pixels have alpha 0-2 in both images (straight
  white at alpha 1-2 where the old blur left alpha 0), alpha differs by at most 8, 3 edge pixels by the
  text outline by 43. The images look the same: an equivalent result, rewritten on glcore
  (`docs/html-ui/goldens-S16.md`, category `filter-blur`, added to `check_golden_ledger.py`).
- **The editor's Game view outside Play.** `test_html_ui_basic.py::test_child_scene_document` failed
  since C: the Game view frames the project canvas (1280 x 661 in a 944 x 488 view) while the documents
  keep the layout the editor gave them (1280 x 720), and R15's full-viewport composite squashed that
  layout into the view. An Auto Resize UI or 2D camera's clip rectangle is the canvas the view shows;
  the documents' frame is placed in it at its canvas pixels' size from the top-left corner, which is
  what main drew through the camera (`RenderSystem::documentsCanvas`, editor builds only: a game lays
  its documents out in the canvas its camera covers, where both are equal). Red: the test on the
  merged tree; green with the change, the boxes at main's pixel positions.
- **`FullPathGoldenTest.UIParticleSystemsClippedByAPanel`** was order-dependent on main (the burst
  comes with the first simulation tick, which a few fast frames can end before): S93's fix
  (`98fe43c97`, waits for its particles) is cherry-picked.
- **LuaBridge wrong call, quiet box (R34).** The port's harness on a build box at load 2.8 of 16 CPUs,
  pinned to one core, min of 14 interleaved runs, ns per call (stock / port's patch / S92):
  clang++ 86233 / 86776 / 96363 (1.117x), g++ 137250 / 121045 / 121298 (0.884x); a wrong-call-only
  build, min of 8: clang++ stock 89952, port 90929, D's first merge 98836 (1.099x), S92 98374 (1.094x).
  The clang ratio sits at R34's 1.1x, not under it as noise would have it: about 9 us of the ~90 us
  error path come from D's own changes on the raising path (the port's patch alone is 1.01x). It is a
  call that raises an error; every later-overload and first-overload budget holds with a wide margin
  (later overloads 0.6-1.7 % of stock). Accepted as at the budget.
- **Android render gate (AC12).** The `htmlui-u27` golden was written on the local KVM emulator
  (`swangle_indirect`) from a clean run: the first update run caught a 6.8 s first-launch stall with
  Android's ANR scrim over the app (the whole screen dimmed). A fresh run matches it (0.00 %), and the
  gate's other scenes pass against their goldens. On a real Mali or Adreno device: unverified (risk 4).
- **Old scenes keep their render mode (lead's decision, replaces D4's "old ones too").** An absent
  `htmlUiRenderMode` used to mean the camera pass and means Overlay now, which AGENTS.md counts as a
  changed meaning. Scene format 9 -> 10 (`sceneV9ToV10` in `editor/StreamMigrations.cpp`, after S98's 8 -> 9) writes
  `htmlUiRenderMode: camera` into an older scene's settings map, unless it names a mode; a scene with no
  settings map is left alone, since adding one would change how its other absent keys decode. A cooked
  record older than 1.9 reads as Camera (`SceneSettingsCodec.cpp`). New scenes, the templates and scenes
  made at runtime get Overlay. Fixture `editor/tests/fixtures/migrations/scene-v8-html-ui.scene`; tests
  `StreamMigrations.SceneV9ToV10KeepsTheCameraRenderMode`, `Migrations.OlderScenesKeepTheCameraRenderMode`,
  `CookedScene.SceneSettingsKeepHtmlUi` and `CookedScene.ReadsAnOlderMinorsContent`.
- **R15/R57 is Screen Space - Overlay's (lead's decision).** Before S92 a game view drew the screen
  documents through a UI or orthographic camera (the canvas in world space), so an older scene whose
  2D camera pans or zooms showed them moving with it; R15 fills the viewport instead, which would change
  that scene's look. A Screen Space - Camera scene, which an older scene is after its migration, keeps the
  placement through the camera in every view (`RenderSystem` passes `throughCamera` to
  `UIDocumentSystem::drawComposite`); Overlay keeps R15. Test `HtmlUIScene.CameraModeKeepsTheDocumentsInTheCanvasOfItsCamera`
  (a panned UI camera moves the documents in Camera mode, not in Overlay).
- **Numbers at landing (the lead's order: S98, S78, then S92; `NUMBERS.md`).** Scene format 10 (S98 took 9),
  cooked scene minor 1.9 (S96 1.6, S98 1.7, S78 1.8; S92's two values end the settings record, after
  S78's Light Steps), built-in shader cache v69 (S78 took v66; v67 and v68 stay skipped: S92's own
  intermediate programs may be in a host's shared cache under them). S98 deletes the generated
  per-scene C++: the scene's render mode and samples reach a Compile build through the cooked scene,
  the project's HTML UI MSAA through `game.json` (`PlayerConfig::htmlUiMsaa`, applied by `GameRunner`).
- **Verification on the merged tree.** `WERROR=1 tools/build.sh linux` (no warning from S92's sources);
  `htmlui-tests` with S92's suites (frame reuse, layer cost, P1, primitives, clips, records, filters,
  batches, renderer, integration, particles, the scene composite, every `*Golden*` but the effect
  goldens, the compat decorators and shapes, the p3 paint parity fixtures) and
  `JASPER_HTMLUI_VERIFY_KEPT=1`: glcore 281 ran, 1 failed (the `texture_effects` golden, then
  rewritten; its rerun with the integration and full-path goldens: 40 ran, 0 failed), Vulkan 281 ran,
  0 failed, 0 skipped, 0 disabled; unit core (`LuaBridgeSelf`, `LuaOverloadDispatch`,
  `LuaBindingBench`, `*Lua*`) 75/75; unit editor (stream, cooked scene, reflection parity, catalog,
  shader cache pins, `*Lua*`) 227/227; MCP (15 files: render cost, render stats, the HTML UI basic,
  render, Lua and effects files, scene colour scale, the Lua-facing files D ran, regressions,
  upgrade-from-main) 46/46; RmlUi's `EffectHooks`/clip/transform doctest cases 20/20; `web` and
  `web-gate` (PASS); Android `render-gate` x86_64 and arm64-v8a builds; `check_golden_ledger`,
  `check_fanout`, `check-cxx-subset`, `check_raw_writes`, `check_stream_keys`, `check_event_clears`,
  `check_text_encoding`, `check-hardcoded-shortcuts`, `check_shader_lod`, `xcode_sync_engine --check`,
  `tool_budget --check`. `TOOLS.md`, `html-ui-support.md`, `RuntimeFields.gen.inc` and the templates
  regenerate without a change beyond ids.
