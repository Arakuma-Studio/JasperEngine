# S93 — HTML/CSS UI gaps from the JasperArcade port

**Status: implemented** on `feature/s93-htmlui-gaps` (packages P1-P6 and the integration). What differs from
the text below is in "Deviations" at the end.

Arcade engine-gaps program (S92-S98). Source: `docs/port/ENGINE_LIMITATIONS.md`, section "UI (HTML/CSS
UI documents)", of the JasperArcade port (read-only, `/var/lib/orquester/workspaces/jaspersito/
JasperArcade-Ported`, called `PORT` below). The port was made against `7f48d7e8d`; every claim in §3 was
checked against the code of this branch's base (`82ccd7c6a`). Paths are relative to the repository root.
"Inferred" means read from the code, not run. S92 owns U27, U34, U35, U36 and U37 (HTML UI render
pipeline); this spec owns the other U items.

## 1. Goal

The port hit 32 HTML UI gaps. Since `7f48d7e8d` the engine fixed some of them: safe areas, the Canvas
Scaler, `System.openURL` and multi-touch. Others are still open: a finger the game holds gets stuck
under a popup, taps miss transformed or clipped content, outlined text stalls the main thread for
seconds, and images decode on the main thread in every scene. Some Unity features are also missing:
scaled-time UI animation, ScrollRect physics, text auto size, world-space UI, text alignment and image
tint. S93 closes every open item to Unity parity or better, with editor UI, Lua, MCP and docs for each
user-facing part, so that the port's workarounds can be deleted. Those workarounds are `Gestures.sync`,
`ScrollView`, `UIWarm`, `UIKit.autosize`, Lua tweens for scaled time, and pre-tinted and cut-out
images.

## 2. Scope

**In scope**
- Pointer ownership (U29, U19) and the multi-touch record (U18).
- Hit testing: transforms, clips, `clip-path` (U2, U13, U32).
- Touch and pointer drag scrolling with uGUI ScrollRect physics (U12).
- Effect glyph cost, the glyph cache across contexts, a prewarm API, and fallback glyphs (U30, U33).
- Asynchronous image decoding and a shared image cache (U31).
- Atlas regions (U16), image tint (U14), border-image slicing (U1), filters on border images (U10).
- CSS correctness (U22, U23, U24, U26, U28), stylesheet injection (U17) and `mix-blend-mode` (U11).
- Text auto size (U7), native Text alignment and auto size (U6).
- Per-document game time (U3) and a World Space render mode (U21).
- The open questions U9 and U25, settled by tests.
- The residue of U8: external links.

**Out of scope**
- **S92:** U27, U34, U35, U36 and U37. That covers RmlRenderer's layers, masks, passes and composite;
  `UIDocumentSystemRender::drawComposite`; `HtmlUIContextRender.cpp`; the "updated" flag in
  `HtmlUIContext::update`; `ElementBackgroundLayers.cpp` and `ElementEffects.cpp`; and HTML UI MSAA.
- **S95:** S30, rebuilding documents after an Android background return. R18 here only keeps the glyph
  cache over a view loss.
- **S98:** the `check` CLI framework. S93 adds the HTML UI checks to `check --ui` through the existing
  checker (R22).
- `background-blend-mode`, scroll snapping and `<video>` following game time. Each is a follow-up, not
  a ledger item.
- From the do-not-copy list: no Yoga/CSS core from OpenEngine. The UI stays on our RmlUi fork
  (`docs/specs/README.md`).

## 3. Current state

### 3.1 Pointer input (U29, U19, U18)
- **Releases.** A release is consumed when it lands over content, captured or not.
  - Touch: `UIDocumentSystem::eventOnPointerUp` sets `consumed` when `processTouchEnd` returns true
    (`engine/core/subsystem/UIDocumentSystemInput.cpp:303-308`). `processTouchEnd` returns
    `isTouchOverContent` (`engine/core/htmlui/HtmlUIContextInput.cpp:88-100`).
  - Mouse: the same, through `isPointerOverContent()` (`UIDocumentSystemInput.cpp:310-315`).
- **Every finger is tracked.** `processTouchStart` pushes every finger into `touches`, also one that
  did not start on content (`HtmlUIContextInput.cpp:56-76`).
- **Moves.** A move over content is consumed for any finger the context tracks
  (`UIDocumentSystemInput.cpp:336-343`).
- **Effect.** `Engine::systemTouchEnd` then skips `Engine::onTouchEnd` (`engine/core/Engine.cpp:3766`).
  This is the stuck joystick of U29.
- **The component UI already does it right.** It consumes a release only when it captured the press
  (`engine/core/subsystem/UISystemPointer.cpp:1191-1212`).
- **Releases outside the canvas.**
  - Fixed since `7f48d7e8d` (`dce43d31e`): the Input touch is removed and the UI pointer cancelled
    (`Engine.cpp:3732-3741`, `3924-3932`).
  - Still open: the game gets no `onTouchEnd` or `onMouseUp` for a pointer it owns.
- **The mouse touch.** `systemMouseUp` calls `Input::removeTouch(0)` only inside the
  `!uiEventReceived` gate (`Engine.cpp:3960-3966`), so the mouse-as-touch stays down after a release
  over UI.
- **Pages that listen (U19).**
  - A transparent page takes every pointer event once the document or `body` has any pointer listener
    (`engine/core/htmlui/HtmlUIContext.cpp:213-216`).
  - That includes move-only and up-only listeners (`isPointerEvent`, `:56-64`).
  - This is documented (`docs/mcp/guide/html-ui.md:1509-1513`) but surprising: a drag tracker on
    `body` blocks the joystick.
- **Multi-touch (U18): fixed.** Each finger is a DOM pointer (`a061d6062`, `dce43d31e`;
  `docs/mcp/guide/html-ui.md:1588-1592`).

### 3.2 Hit testing (U2, U13, U32)
- **How it works now.** `Context::GetElementAtPoint` (`engine/libs/rmlui/Source/Core/Context.cpp:1427-1437`):
  1. projects the point into the element's local space (`element->Project(point)`), then
  2. tests the clip with `GetClippingRegion(element, clip_region)` and `clip_region.Contains(point)`.
- **The bug.** `GetClippingRegion` without a mask list returns the clipping ancestors' layout rectangles
  in document space, untransformed (`ElementUtilities.cpp:186-235`). The two are compared in different
  spaces (inferred):
  - a translated child of an `overflow: hidden` parent is tested at its untranslated position (U13);
  - a child of a scaled panel is tested at a point scaled away from where it is drawn (U2).
- **Rounded clip corners** of ancestors are not tested at all.
- **`clip-path` (U32).** It is drawn as `mask-image: html-shape(...)`, and the support table says
  "no hit-test clipping" (`engine/core/htmlui/compat/tables/ValueTable.cpp:139`).

### 3.3 Touch scrolling (U12)
- **Since `7f48d7e8d`** (`445de6757`), a finger on a scroll container pans it past a slop:
  - `ContextPointer.cpp:481-490` and `619-668`;
  - it cancels the press (no click);
  - it ends with inertia (`:751-769`, `ScrollController::ActivateInertia`).
- **Missing compared with uGUI ScrollRect:**
  - the mouse never drag-scrolls, because `ProcessPointerMove` hands the mouse to `ProcessMouseMove`
    (`:604-605`); the editor harness and desktop players saw "no drag scrolling";
  - no elastic overscroll;
  - no Elasticity or Deceleration Rate;
  - no Lua velocity.
- The port re-implements all of it in `PORT/scripts/shared/ui/ScrollView.lua`: uGUI defaults,
  elasticity 0.1, deceleration 0.135 and a 10 px drag threshold.

### 3.4 Glyphs (U30, U33)
- **Fixed since `7f48d7e8d` (S16; `0df4aea71` and later):**
  - outline, glow and shadow within 0.3 em are drawn on the GPU from MTSDF distance glyphs
    (`engine/core/htmlui/RmlFontEngine.cpp:1209-1219`, `1296-1311`);
  - atlas pages are re-uploaded in place, with no new texture source (`RmlFontEngine.cpp:1083-1102`,
    `RmlRenderer.cpp:966-985`);
  - text versions are per handle (`GetVersion`, `:1746-1758`), so a new glyph no longer regenerates
    every document's text.
- **Still open:**
  - **CPU fallback while glyphs are pending.** Distance glyphs are made on the background lane
    (`:1114-1147`). Until they are ready, `distanceReady` is false, and every effect falls back to the
    CPU (`:1302`, `1324-1327`). The CPU effect is RmlUi's dense convolution, `ConvolutionFilter::Run`
    (`engine/libs/rmlui/Source/Core/ConvolutionFilter.cpp:42-88`, used by `FontEffectOutline.cpp:26,75`
    and `FontEffectGlow.cpp:33`). Its cost is O(area x (2w+1)^2), so the first frame of a big outlined
    text still pays the port's 1-3 s (inferred: the fallback runs on the frame the text is first
    generated).
  - **Effect bitmap keys.** Effect bitmaps are keyed by `effectId` (`RmlFontEngine.cpp:1043`,
    `RmlFontEffects.cpp:11-14`), which is the fingerprint of every property, colour and offset included
    (`PropertyParserFontEffect.cpp:89-94`). An outline and a glow with an offset never share a bitmap.
  - **The cache dies with each context.** `HtmlUIContext::destroy` calls `Rml::ReleaseRenderManagers`
    (`HtmlUIContext.cpp:241-242`), which calls `ReleaseFontResources()` first
    (`engine/libs/rmlui/Source/Core/Core.cpp:443`). That reaches `RmlFontEngine::ReleaseFontResources`
    and `clearGlyphs` (`RmlFontEngine.cpp:1760-1789`), so every scene change starts with an empty glyph
    cache.
  - **No prewarm API.**
- **Fallback glyphs (U33).**
  - Effects apply per face of the chain: `getEffectGlyph(*handle, glyph.face, ...)` and
    `requestDistanceGlyph(..., glyph.face, ...)` (`RmlFontEngine.cpp:1294, 1326, 1469, 1488`). So the
    fallback face gets the effect (inferred; the ledger saw the opposite at `7f48d7e8d`).
  - No diagnostic reports a character drawn by a fallback face. Only Title Art has `glyph_missing`
    (`editor/titleart/TitleArtBuiltinChecks.h:32-34`).

### 3.5 Images (U31, U16, U14, U1, U10)
- **Decoding on the main thread.** `RmlRenderer::LoadTexture` decodes, premultiplies and uploads on the
  main thread when an image is first drawn (`engine/core/htmlui/RmlRenderer.cpp:993-1063`).
- **No sharing across contexts.** Textures belong to the context's renderer and are released with it
  (`HtmlUIContext.cpp:241`). No cache spans contexts.
- **No atlas regions (U16).** A URL addresses a whole file: no fragment or sprite name is parsed
  (`stripTextureFilter`, `RmlRenderer.cpp:1070-1100`).
- **Tint (U14).** `image-color` multiplies `<img>` (`docs/mcp/guide/html-ui.md:1256`). The compat
  decorators of `background-image` and `border-image` ignore it: `Image::colour` returns white or
  `currentColor` for tinted SVGs (`engine/core/htmlui/compat/runtime/DecoratorCommon.cpp:391-396`).
- **border-image slicing (U1).**
  - The intrinsic size of a raster is pixels x dp ratio (`DecoratorCommon.cpp:350`).
  - A NUMBER slice is taken as raw image pixels against that size
    (`HtmlBorderImageDecorator.cpp:241-249`).
  - So 9-slices are wrong at any dp ratio other than 1 (confirmed by reading).
- **Filters on border images (U10).** Unverified: `filter` applies to the element's layer, which
  includes decorators. A golden settles it (R28).

### 3.6 CSS (U22, U23, U24, U26, U28, U17, U11)
- **Inset shadows (U22).**
  - Inset shadows are added to the background primitives after the box (`add_shadows(true)`), or baked
    into `BoxShadowAndBackgroundBorder`
    (`engine/libs/rmlui/Source/Core/ElementBackgroundBorder.cpp:574-603, 641, 164-168`).
  - The background layers are drawn after that (`:176-181`), so a gradient covers the inset shadow.
    CSS paints inset shadows above the background images (Backgrounds 3 §7.1).
- **calc() in shapes and gradient stops (U23).**
  - `clip-path` shape lengths go through `parseLength` of one token (`HtmlShapeDecorator.cpp:35-41,
    424-427`), so `calc(100% - 20px)` cannot be read (inferred).
  - Gradient stops: the fork resolves `position_calculation` (`DecoratorGradient.cpp:95-103`). The loss
    is in the compat rewrite (`compat/css/props/Background.cpp:100-125` serialises only axis positions).
    Unverified: a golden settles it.
- **Stale support table (U24).**
  - `PercentRadius` is present (`compat/ForkFeatures.cpp:26`), but the border-radius rows still say
    "other percentages dropped until the fork has PercentRadius" (`compat/tables/PropertyTable.cpp:161,
    163`, `ValueTable.cpp:117`; `docs/mcp/guide/html-ui-support.md:681`).
  - The generated table has 202 notes that say "until the fork has X". Many of those features are
    present (`ForkFeatures.cpp:22-40`).
- **Caches not keyed by render manager (U26).**
  - `BoxShadowCache` handles are keyed by geometry only (`BoxShadowCache.cpp:37-48`).
  - The StyleSheet decorator cache is keyed by declaration and path (`StyleSheet.cpp:104-123`).
  - Both are shared across render managers. Screen documents and texture-mode documents have different
    renderers.
- **`visibility` readback (U28).** `visibility` is registered as `visible, hidden, collapse=1`
  (`StyleSheetSpecification.cpp:713`), so `hidden` reads back as `collapse`.
- **No stylesheet injection (U17).** No Lua or C++ API adds a stylesheet to a live document.
- **No blend modes (U11).** `mix-blend-mode` is unsupported (`html-ui-support.md:1058`).

### 3.7 Time, text and render modes (U3, U7, U6, U21)
- **UI time (U3).**
  - One global UI clock, advanced by the unscaled delta (`engine/core/htmlui/HtmlUIClock.h:13-17`).
  - It feeds RmlUi through `RmlSystem::GetElapsedTime` (`RmlSystem.cpp:66-68`), and from there
    transitions and animations (`engine/libs/rmlui/Source/Core/Element.cpp:3483, 3598, 3651, 3763`).
  - It also feeds anime (`UIDocumentSystem.cpp:275`) and effects (`effects/EffectRuntime.cpp:70`).
  - Only text effects can follow game time (`text-effect-time: game`, `HtmlTextEffects.cpp:306-308,
    345-347`).
- **Text auto size (U7).** None; the port estimates sizes from average advances
  (`PORT/scripts/shared/ui/UIKit.lua:283-370`).
- **Native Text (U6).**
  - Outline, shadow and Text Effect were added since `7f48d7e8d` (`8fc9ba6ba`;
    `engine/core/component/TextComponent.h:45-58`).
  - There is no alignment: lines are laid out from the left, and only `pivotCentered` moves the whole
    block (`engine/core/subsystem/UISystem.cpp:546-566`).
  - There is no auto size.
- **World-space UI (U21).**
  - A TEXTURE-mode document shown on a mesh is depth-tested (`UIDocumentComponent.h`: SCREEN,
    TEXTURE).
  - Native Text draws in the world with depth state (`engine/core/object/ui/Text.h:131-133`).
  - Missing is the one-step "World Space" of Unity's Canvas: today a user builds a quad, a material and
    the texture link by hand.

### 3.8 Fixed since `7f48d7e8d`
- **U4, safe area:**
  - `env(safe-area-inset-*)` (`9938353c0`, `b0e97e481`; `docs/mcp/guide/html-ui.md:1706-1719`);
  - `Screen.safeArea` and `Screen.safeAreaInsets` (`docs/mcp/guide/lua-api.md:915-916`);
  - iOS, Android and Web providers (`engine/platform/emscripten/JasperEngineWeb.cpp:193-230`).
- **U5, the Canvas Scaler:** Match Width Or Height, Expand and Shrink (`8fc9ba6ba`;
  `engine/core/util/CanvasScaler.h:13-48`).
- **U8, `System.openURL`:**
  - the API (`b870f5e96`; `engine/core/System.cpp:164-175`, Lua `CoreClassesLua.cpp:4058`);
  - an implementation on all six platforms (`SystemWin.cpp:258`, `HostServicesMac.mm:39`,
    `HostServicesLinux.cpp`, `JasperEngineApple.mm:601`, `JasperEngineAndroid.cpp:429`,
    `JasperEngineWeb.cpp:1586`).
  - Residue:
    - the link diagnostic still says "System:openURL is not available"
      (`compat/html/transforms/AttributeTransform.cpp:269-273`);
    - external `<a href>` links only notify (`html-ui.md:294`).
- **U18, multi-touch:** see §3.1.

### 3.9 Unverified
- **U9.** Text that changes every frame does not draw in the headless editor. Possible causes are the
  settle frames (`ElementText.cpp:1219-1322`) and glyph deferral (`RmlFontEngine.cpp:1654-1671`).
- **U25.** The Game view stopped after a stylesheet hot reload in play. Reload was rewritten since
  (`2d8026a38`, S17 live patch).

## 4. Reference

- **Port workarounds (the API shapes games want), all under `PORT/scripts/shared/`:**
  - `Gestures.lua`: pointer ownership and lost releases;
  - `ui/ScrollView.lua`: uGUI ScrollRect;
  - `ui/UIWarm.lua`: prewarm of text, images and hidden layers;
  - `ui/UIKit.lua:283-370`: TMP auto size;
  - `SceneFlow.lua`;
  - `Tween.lua` and `Timer.lua`: scaled-time UI.
- **Unity:**
  - `EventSystem`: the press owner gets the drag and the release; `pointerPress`,
    `eligibleForClick`.
  - `ScrollRect`: Movement Type, Elasticity, Inertia, Deceleration Rate, Scroll Sensitivity;
    `RubberDelta`; SmoothDamp back.
  - TextMeshPro: Alignment, Auto Size, Font Size Min/Max.
  - `Canvas.renderMode` World Space; `Animator.updateMode` Unscaled Time; `Image.color`.
- **W3C:**
  - Pointer Events 3: implicit capture.
  - CSS Masking 1: `clip-path` clips hit testing; `mask-image` does not.
  - Compositing and Blending 1: `mix-blend-mode` formulas.
  - Backgrounds 3: §7.1, painting order.
  - Media Fragments URI 1.0: `#xywh=`.
  - HTML: `img decoding`.
- **Algorithms:** Felzenszwalb and Huttenlocher, "Distance Transforms of Sampled Functions": an exact
  Euclidean distance transform in O(area).
- **OpenEngine:** nothing taken.

## 5. Requirements

### Functional: pointer ownership (P1)
- **R1** A pointer's owner is decided at its press. A touch or mouse button that no document captured
  at its press belongs to whoever got that press: the component UI, a world document or the game.
  - For such a pointer, `UIDocumentSystem::eventOnPointerUp` and `eventOnPointerMove` never return
    consumed, whatever is under it.
  - The documents still see the pointer for hover and for press-on-enter controls.
  - A pointer a document captured stays the document's until its release or cancel (unchanged).
- **R2** A release or cancel outside the canvas, for a pointer the game owns, reaches the game:
  - `onTouchEnd` or `onMouseUp`, with the coordinates clamped to the canvas;
  - without `Engine.allowEventsOutCanvas`;
  - a document-owned pointer is cancelled, as today.
- **R3** `systemMouseUp` always removes the simulated touch 0 when `callTouchInMouseEvent` is on, also
  when the release was consumed.
- **R4** Only press-kind listeners make a transparent page take presses on its empty parts. Press kinds
  are `click`, `dblclick`, `contextmenu`, `mousedown`, `pointerdown` and `touchstart`.
  - A `wheel` or `mousescroll` listener takes only the wheel.
  - Move, up, over, out, enter and leave listeners take nothing.
- **R5** When a listener turns a page opaque, an Info diagnostic `HTMLUI-INPUT-0001` (new) reports it,
  once per document and event type. It names the element and the event type, and says the document now
  takes every press on its empty parts. It reaches the Console in the editor, `doc:getDiagnostics()`
  and `inspect_ui`.

### Functional: hit testing and scrolling (P2)
- **R6** A point hits an element only where the element is painted:
  - its own transform, the transforms between, and those on each clipping ancestor;
  - each clipping ancestor's clip area tested in that ancestor's projected space, rounded corners
    included;
  - an element translated inside an `overflow: hidden` parent is hit where it is drawn (U13);
  - a child drawn outside a scaled parent that does not clip is hit where it is drawn (U2).
- **R7** `clip-path` clips the hit test: `inset`, `circle`, `ellipse`, `polygon` (with its fill rule),
  `path`, `xywh` and `rect`, `calc()` values included. `mask-image` does not (CSS parity).
- **R8** New CSS properties for scroll containers (engine extensions, Unity names):
  - `scroll-drag: touch | pointer` (default `touch`). `pointer` makes the mouse and pen drag-pan too.
  - `scroll-movement: clamped | elastic | unrestricted` (default `clamped`).
  - `scroll-elasticity: <time>` (default `0.1s`).
  - `scroll-inertia: auto | none` (default `auto`).
  - `scroll-deceleration-rate: auto | <number 0..1>` (default `auto`, today's curve).
- **R9** Pan behaviour:
  - A pan that passes the slop cancels the press: `pointercancel`, no click, `:active` removed. Mouse
    included under `scroll-drag: pointer`.
  - Elastic overscroll uses uGUI's `RubberDelta` while dragging, and a SmoothDamp back over the
    elasticity after the release.
  - A number for the deceleration rate gives uGUI's model: `v *= rate^dt`, stop below 1 px/s.
  - All of it runs on the unscaled UI clock.
- **R10** Lua `el.scrollVelocity` (Vector2, read/write, px/s) and `el:stopScrolling()`.

### Functional: glyphs and fonts (P3)
- **R11** No effect glyph is made by a dense convolution. Outline (dilation) uses an exact Euclidean
  distance transform, O(area). Glow and blur use separable passes, O(area x passes). The
  `ConvolutionFilter` dense loop is no longer called for font effects.
- **R12** CPU effect bitmaps are keyed by what changes the bitmap: face, glyph, size, bold, and the
  effect's shape parameters (kind, widths, blur). Not colour, not offset. The offset is applied when
  the quad is placed.
- **R13** The glyph cache outlives HTML UI contexts. Destroying a context:
  - drops that context's GPU textures of the atlas pages;
  - keeps glyph entries, page texels, face handles and effect sets.
  The atlas budget and LRU still bound it. A font file reload or unregister still purges (existing
  `retireHandles`). `Rml::ReleaseRenderManagers` no longer releases font resources (a fork patch, with
  a `FORK.md` entry).
- **R14** A view loss (Android background, GL context loss) keeps the CPU pages, and textures are made
  again on first draw.
- **R15** Prewarm API (Lua):
  - `doc:prewarm([opts])` walks the document, including hidden and `display: none` subtrees (their
    styles resolved). It collects `(face, size, effects, code points)` for every text and `data-warm`
    attribute, and every image URL.
  - `HtmlUI.prewarmText{family, size, weight, style, effect, text}`.
  - `HtmlUI.prewarmImage(path)`, from P4.
  - Rasterising base, distance and effect glyphs and decoding images run on background jobs. Placement
    on the main thread stays within `HtmlUI.prewarmBudgetMs` per frame (default 2).
  - Each call returns a handle with `done`, `progress` and `onDone`.
- **R16** Fallback faces are chosen by the element's weight and style, then synthetic bold and italic.
  The element's `font-effect` and `text-effect` apply to fallback glyphs.
- **R17** Diagnostic `HTMLUI-FONT-0001` (Info) for code points the first available face of the
  element's family list lacks and a fallback face draws:
  - at runtime, once per (document, family, code point set);
  - in `check --ui` for static text of `.html` files, through the existing compat checker.

### Functional: images (P4)
- **R18** Decoding is asynchronous by default.
  - `LoadTexture` reads only the header synchronously, which gives the dimensions for layout.
  - Decode and premultiply run on the Background lane. The upload runs on the main thread when ready.
  - Until then the image draws nothing. Then the element is redrawn, without a relayout.
  - `<img decoding="sync">` forces a synchronous decode.
  - A deterministic clock waits for the decode in the same frame, so goldens stay exact. The
    deterministic clock is play `fixed_fps`, a fixed UI time or tests.
  - On a build without worker threads (single-thread web), decoding stays synchronous, and the cache
    of R19 still applies.
- **R19** A shared image cache.
  - GPU textures are keyed by resolved path, file stamp, filter, wrap and region.
  - They are shared by every context and renderer, and reference counted.
  - Released entries stay in an LRU bounded by `graphics.html_ui_image_cache_mib` (0 = no LRU).
    The default is 64 on desktop and 32 on Android, iOS and the web.
  - Hot reload invalidates by path.
- **R20** Atlas regions in any image URL: `img src`, `background-image`, `border-image-source`,
  `mask-image`, `list-style-image` and `content`.
  - `#xywh=x,y,w,h` and `#xywh=percent:x,y,w,h` (Media Fragments).
  - `#sprite=<name>`, the frame of that name in the texture's Sprite Mode sheet (S45).
  - The region is cropped into its own texture from the cached decode of the file, so `repeat`,
    `round` and `space` work.
  - An unknown sprite or a region out of bounds gives a diagnostic and draws nothing.
- **R21** `border-image-slice` numbers are image pixels at every dp ratio. For rasters they are scaled
  by the same ratio as the intrinsic size.
- **R22** `image-color` multiplies `background-image` and `border-image` images, as it does for
  `<img>`.
- **R23** `filter` applies to an element whose only paint is a `border-image` (U10).
  - A golden decides.
  - If the cause is in the decorator, P4 fixes it.
  - If the cause is in the filter or layer path, the failing golden is handed to S92 and stays red
    there.

### Functional: CSS and style sheets (P5)
- **R24** Inset box shadows paint above the background colour and background images, and below the
  border (Backgrounds 3 §7.1).
- **R25** `calc()` that mixes `%` and lengths evaluates in gradient colour stops, as Chrome does:
  linear, radial and conic, repeating ones included. The shape side of U23 is P2's (R7).
- **R26** No support-table note waits for a fork feature that is present. The notes for present
  features say what happens now, and `html-ui-support.md` is regenerated.
- **R27** `BoxShadowCache` and the StyleSheet decorator cache are keyed by render manager.
- **R28** A keyword property reads back as the first keyword registered for its value:
  `visibility = "hidden"` reads `"hidden"`.
- **R29** Stylesheet injection:
  - `doc:addStyleSheet(cssText [, name])` and `doc:addStyleSheetFile(path)` return a handle with
    `:remove()` and `.enabled`.
  - The sheet goes through the same compat pipeline and diagnostics. It cascades after the document's
    own sheets, can target pseudo-elements, and survives a live patch reload.
  - `inspect_ui` styles name its source `injected:<name>:<line>`.
- **R30** `mix-blend-mode`, all 16 CSS modes, exact (Compositing 1 formulas).
  - The element is drawn as an isolated layer and composited by a blend program that reads a copy of
    the backdrop.
  - `plus-lighter` uses a fixed-function fast path, with no backdrop copy.
  - The backdrop is what the document drew below the element. It is not the 3D scene, because the
    document is isolated (D13).
  - `normal` costs nothing.
- **R31** `text-autosize: none | <length> <length>` (min, max; an engine extension). Within the
  element's definite content box:
  - the element's font size is the largest whole-pixel size in [min, max] at which its text fits
    without overflow, wrapping as laid out;
  - descendants with `em` sizes follow;
  - the fit is computed in the same update, so no intermediate size is shown;
  - Lua reads the fitted size from `getComputedStyle("font-size")`.
- **R32** External links: a click on an `<a href>` with a remote URL calls `System.openURL` unless a
  `htmlnavigate` listener calls `preventDefault()`. The link diagnostic's suggestion is corrected
  (`AttributeTransform.cpp:272`).
- **R33** Text that changes every frame draws every frame, in the editor (headless included) and in
  players (U9).
- **R34** A stylesheet hot reload in play keeps the Game view rendering, and the frame counter
  advances (U25).

### Functional: UI Document and Text components (P6)
- **R35** UI Document **Update Mode**: `Unscaled Time` (default, today's behaviour) or `Game Time`.
  - Game Time follows `Engine.timeScale`, stops while the game is paused or held, and in the editor
    outside play.
  - It drives the document's:
    - CSS transitions and `@keyframes`;
    - anime timelines whose targets are in the document (an anime option `time = "ui"|"game"`
      overrides);
    - phase-5 effect time;
    - Lottie and vector animation;
    - text effects under `text-effect-time: auto`, the new default.
  - Input timing stays on the unscaled clock: double click, long press, pan inertia, gamepad repeat.
  - `set_ui_effect_time` and fixed UI time still pin both clocks.
- **R36** UI Document **Render Mode** `World Space`. The document is drawn as a depth-tested quad
  (depth test on, no depth write) in its scene's transparent pass, at its entity's transform.
  - Size = texture size / `Pixels Per Unit` (default 100).
  - `Billboard` turns the quad to the camera.
  - Pointer input goes through the existing world raycast (`UIWorldRaycast`) against the quad.
  - No mesh or material is needed. The TEXTURE target is reused.
- **R37** Native Text, with TextMeshPro names:
  - **Alignment:** horizontal `Left | Center | Right | Justified`, vertical
    `Top | Middle | Bottom | Baseline`, within a fixed width or height;
  - **Auto Size** with `Font Size Min` and `Font Size Max`, a whole-pixel fit to the fixed box;
  - for every line, outline and shadow included.

### Platform & backends
- **R38** Everything in this spec works on GL 4.1, GLES3/WebGL2, Metal, D3D11 and Vulkan, and on
  Windows, Linux, macOS, Android, iOS and HTML5. Constraints:
  - no compute;
  - the `mix-blend-mode` backdrop copy uses the existing layer copy (`copyLayerInto`);
  - jobs fall back to inline work where there are no threads;
  - off-canvas releases (R2) on Android and iOS come from the platform's touch end and cancel events.
  - The HTML5 build calls `System.openURL` (R32) from the click's event handler, so the popup is
    allowed (`JasperEngineWeb.cpp:1586-1600`).

### Performance (dev host = the shared Linux build host; each budget reported through `perfSlack()`)
- **R39** First draw of a cold `GO!` at 324 px with `font-effect: outline(15px #000), glow(4px 12px 0px
  8px #000)`:
  - **≤ 8 ms** main thread for the frame's `GenerateString` of that text, in two runs: GPU effects
    with distance glyphs pending, and transform-aware text off (the CPU path);
  - the port measured 1.3-3 s for the same text on the old CPU path.
- **R40** One 324 px glyph's outline bitmap (w = 15) on the CPU: **≤ 1.5 ms** single thread.
- **R41** A scene entered a second time:
  - `glyphsRasterized` = 0 and `textEffectFallbacks` = 0 in its first frame;
  - text generation for the port's shell fixture ≤ 2 ms.
- **R42** The frame that first shows a 2048x2048 PNG `<img>`:
  - **≤ 3 ms** of added main-thread time;
  - the image visible within 30 frames at 60 fps;
  - its upload frame ≤ 4 ms;
  - a second scene that shows it: 0 decodes, 0 uploads.
- **R43** Hit testing on the S17 10k-row list fixture costs ≤ 110 % of today. Per-ancestor projection
  runs only when a transform is in the chain.
- **R44** `text-autosize` costs:
  - ≤ 0.3 ms per autosized element per relayout;
  - 0 when its text, box and font did not change;
  - nothing for elements without it.
- **R45** Disabled costs nothing:
  - Unscaled Time takes today's code path;
  - `scroll-movement: clamped` adds no per-frame work;
  - `normal` blend adds no layer;
  - a cache size of 0 keeps no released entries.

### Editor UX (human)
- **R46** The UI Document Inspector shows:
  - **Update Mode** (Unscaled Time, Game Time);
  - **Render Mode** World Space, with **Pixels Per Unit** and **Billboard**.
  Every edit is undoable through `editor/command`.
- **R47** The Text Inspector shows **Alignment** (two button rows as in TMP) and **Auto Size** with
  **Min** and **Max**. Every edit is undoable.
- **R48** Project Settings > Graphics > HTML UI shows **Image Cache (MiB)**.
- **R49** The RCSS property names list (`editor/window/widget/RcssProperties.h`, generated) includes
  the new properties.

### Agent UX (MCP tools, Lua API, CLI)
- **R50** `inspect_ui` with `render_stats` reports:
  - `imagesDecoded`, `imageCacheHits` and `imagesPending`;
  - `glyphsRasterized` (existing) and `textEffectFallbacks` (existing), as cold-path evidence.
  No new tool. Result fields are not added to descriptions.
- **R51** `inspect_ui` `at` and `send_input` follow R1-R9: the element stack under a point is the
  painted one.
- **R52** The new UI Document and Text fields are settable through `create_ui_document` `properties`
  and the generic component tools. Snake-case keys: `update_mode`, `render_mode: "world"`,
  `pixels_per_unit`, `billboard`; Text `horizontal_alignment`, `vertical_alignment`, `auto_size`,
  `font_size_min`, `font_size_max`.
- **R53** Lua:
  - `doc.updateMode`;
  - `doc:addStyleSheet`, `doc:addStyleSheetFile`;
  - `doc:prewarm`, `HtmlUI.prewarmText`, `HtmlUI.prewarmImage`, `HtmlUI.prewarmBudgetMs`;
  - `el.scrollVelocity`, `el:stopScrolling`;
  - Text `horizontalAlignment`, `verticalAlignment`, `autoSize`, `fontSizeMin`, `fontSizeMax`.
  Stubs in `engine/core/script/binding/stubs/`, and `docs/mcp/guide/lua-api.md` updated.
- **R54** The guide (`docs/mcp/guide/html-ui.md`) is updated:
  - pointer ownership;
  - the pass-through rule;
  - scrolling;
  - Update Mode;
  - prewarm;
  - images (async, `decoding`, regions, cache);
  - `mix-blend-mode`;
  - `text-autosize`;
  - stylesheet injection.
  Each rule goes where an agent reads it as it decides (AGENTS.md "MCP text"). An eval case is added
  for any new tool text, and the budget is lowered where text is cut.

### Data & compatibility
- **R55** Every new component field is optional, with a default that keeps today's behaviour, and no
  version bump:
  - UI Document `update_mode` (`unscaled`), `pixels_per_unit` (100) and `billboard` (false);
  - Text alignment `left` and `top`, `auto_size` false.
  `editor/reflect/components/UIReflect.cpp`, `Enums.cpp`, the reflection parity goldens and
  `engine/core/serialization/RuntimeFields.gen.inc` (regenerated) are updated. Old projects load
  unchanged.
- **R56** `graphics.html_ui_image_cache_mib` is optional, with defaults.
- **R57** CSS: an unknown new property is ignored by older engines. No asset format changes.

## 6. Design constraints & decisions

- **D1 Pointer ownership.** The owner of a pointer is decided at its press: W3C implicit capture and
  Unity's `EventSystem.pointerPress`.
  - It is implemented in `UIDocumentSystemInput.cpp`: consumed = captured, for releases and moves.
  - It is not a new system. `HtmlUIInputState::capturedTouches` and `capturedButtons` already hold the
    owners.
  - Game-owned fingers keep reaching the documents' hover, which press-on-enter controls need, but
    never report consumed.
  - The port's `Gestures.sync` becomes unnecessary.
- **D2 Releases outside the canvas** are clamped, not dropped. A game cannot otherwise know a finger
  that slid off the edge was lifted.
- **D3 Pass-through follows press listeners only (behaviour change).**
  `TestInputPassThrough.cpp` and the guide change in the same commit. A modal that wants every pointer
  event keeps the documented background (`rgba(0,0,0,0.01)`) or a `pointerdown` or `click` listener.
- **D4 The hit test equals the paint.**
  - The fork computes per-ancestor projected clip tests in `Context::GetElementAtPoint` (a new helper
    `ElementUtilities::IsPointWithinClip(element, screen_point)`).
  - `clip-path` is tested through a hook `Context::SetHitTestShape(Function<bool(Element*, Vector2f)>)`,
    which the compat layer installs. It uses the parsed basic shape of `HtmlShapeDecorator`, exact
    geometry, no raster.
  - `mask-image` is not hit-tested (CSS Masking 1).
- **D5 Scroll physics are opt-in.**
  - The defaults keep today's touch pan and inertia, so no project changes feel.
  - The values are Unity's when set: `scroll-movement: elastic; scroll-elasticity: 0.1s;
    scroll-deceleration-rate: 0.135` reproduces the port's ScrollView.
  - Mouse drag is opt-in (`scroll-drag: pointer`), because desktop users expect wheel and scrollbar.
- **D6 Effect glyphs.**
  - The GPU distance path (S16) stays primary.
  - The CPU path becomes fast (EDT dilation, separable blur) instead of being avoided by waiting: one
    frame of CPU effect is cheaper than a frame without the text.
  - The bitmaps may differ from RmlUi's kernel by antialiasing. The `TestGoldenFontEffects` goldens are
    rewritten in one commit, every changed image looked at.
- **D7 The glyph cache is process-wide** and lives until `Rml::Shutdown`, a font reload or the budget's
  LRU.
  - The fork patch to `ReleaseRenderManagers` must remove each released render manager's textures from
    every live `CallbackTextureSource` before the manager goes. RmlUi keys those by render manager.
  - The patch adds `CallbackTextureSource::ReleaseRenderManager(RenderManager*)` over a registry of
    live sources.
- **D8 Images decode asynchronously by default.**
  - A pop-in of a few frames beats a 0.4 s stall.
  - `decoding="sync"` and `doc:prewarm()` cover screens that must appear whole.
  - Deterministic clocks wait, the same rule as distance glyphs (`RmlFontEngine.cpp:50-56`).
- **D9 One image cache per process** (`engine/core/htmlui/HtmlUIImageCache.{h,cpp}`).
  - Its textures are `HtmlUITexture` of a shared kind that no renderer destroys.
  - `RmlRenderer::LoadTexture` returns a per-renderer handle onto the shared image.
  - Hot reload (`HtmlUIFileLog::recordLazyFile`) keeps working, keyed by path.
- **D10 Atlas regions use URL fragments.**
  - `#xywh=` is a W3C standard.
  - `#sprite=` reuses S45's sheet data (`engine/core/sprite/SpriteSheetSources.h`).
  - Cropping at decode, not a UV rectangle, makes repeat modes and every decorator work without
    touching S92's layer code.
- **D11 Update Mode is per document, not per scene.** A pause menu (unscaled) and a HUD (game time)
  share a scene.
  - The fork gets a per-document animation clock: `ElementDocument::GetAnimationTime()`, which
    `Element.cpp:3483, 3598, 3651, 3763` use instead of `Clock::GetElapsedTime()`.
  - Input code keeps `GetSystemInterface()->GetElapsedTime()`.
  - `HtmlUIClock` gains `now(UIUpdateMode)`. The game-time clock is `Engine::getGameTime()`, which
    already stops with pause and `timeScale` 0.
  - Naming: Unity's Animator "Update Mode", values "Unscaled Time" and "Game Time".
- **D12 `text-autosize` is an engine CSS extension** (unprefixed, like `text-effect` and
  `text-effect-time`).
  - Chrome ignores it, so the authored `font-size` is the preview.
  - The fit is a bounded binary search over whole pixels, made with the font engine's measure and
    RmlUi's line breaker inside the element's layout. There are no extra frames.
- **D13 `mix-blend-mode` is isolated at the document.**
  - The document renders into its own target before it is composited over the scene (S92 owns that
    composite), so the backdrop is the document's own pixels.
  - A per-mode blend program (16 modes) lands on S92's composite API after S92 merges.
  - `plus-lighter` is fixed function (ONE, ONE).
- **D14 World Space is a render mode, not a new component.**
  - It reuses the TEXTURE target and `UIWorldRaycast`.
  - The quad is drawn by the render system's transparent queue from the document's texture and
    transform, with no serialized mesh.
  - Unity parity: Canvas Render Mode World Space, with a Pixels Per Unit setting.
- **D15 Native Text alignment and auto size use TMP names.**
  - Horizontal alignment applies per line inside a fixed width.
  - Vertical alignment applies inside a fixed height.
  - Without a fixed box, the old pivot behaviour stays.
- **D16 External links open the system browser** (browser parity), cancellable by `preventDefault()`.
  Local `.html` links keep loading in place.
- **D17 Inset shadows.** The fork splits them into their own background type
  (`BackgroundType::BoxShadowInset`), drawn after the layers and before `RenderBorder`. The baked
  texture path (`BoxShadowAndBackgroundBorder`) keeps only the outer shadows.
- **D18 Keyword readback.** `PropertyParserKeyword` keeps the first name registered for each value,
  for conversion back to text.
- **D19 Verdicts that are not engine work.**
  - U15 is a choice of the game's style: `PORT/assets/ui/shared/widgets.css:31-33` sets
    `.tmp { pointer-events: none }`.
  - The ledger retracts U20.

API sketch (Lua):

```lua
local doc = UIDocument(self.scene, self.entity)
doc.updateMode = "game"                            -- or "unscaled"
local sheet = doc:addStyleSheet(".btn::before { content: '>'; }", "press-tint")
sheet:remove()
local job = doc:prewarm{ images = true }           -- job.done, job.progress, job.onDone = fn
HtmlUI.prewarmText{ family = "LINESeedSans", size = 324, weight = 700,
                    effect = "outline(15px #000)", text = "GO!0123456789" }
HtmlUI.prewarmImage("ui/power-ups.png")
list.scrollVelocity = Vector2(0, -1200); list:stopScrolling()
text.horizontalAlignment = "center"; text.autoSize = true; text.fontSizeMin = 18; text.fontSizeMax = 80
```

```css
.list  { overflow-y: auto; scroll-drag: pointer; scroll-movement: elastic;
         scroll-elasticity: 0.1s; scroll-deceleration-rate: 0.135; }
.title { text-autosize: 18px 80px; }
.frame { border-image: url("ui/atlas.png#sprite=panel") 24 fill / 24px; image-color: #ffcc00; }
.static { mix-blend-mode: screen; }
```

## 7. Acceptance criteria

Unit tests sit in `engine/tests/htmlui/` (`jasperengine-htmlui-tests`, goldens on glcore and Vulkan)
or `engine/tests/core/`. MCP tests are `CI = True`. Each AC names its red control: the run of the same
test on the unfixed code.

### Pointer ownership (P1)
- **AC1** `TestInput.cpp`, `HtmlUIInput.GameOwnedTouchReleaseOverDocumentReachesGame`:
  1. a finger starts on empty canvas;
  2. a full-screen document is shown under it;
  3. the finger moves and lifts over the document's button.
  Expected: `Engine::onTouchMove` and `onTouchEnd` fire, the button gets no click, and Input has no
  touch. Red on base: `onTouchEnd` missing.
- **AC2** `HtmlUIInput.CapturedTouchStaysWithDocument`: a press on a button, a move off it, a release
  over empty canvas. Expected: consumed, and the game gets nothing (unchanged behaviour).
- **AC3** `HtmlUIInput.OffCanvasReleaseOfGameTouchIsDelivered`: a game-owned finger released at
  (-40, 300). Expected: `onTouchEnd` with x = 0, without `allowEventsOutCanvas`.
- **AC4** `HtmlUIInput.MouseUpOverUIRemovesSimulatedTouch`: with `callTouchInMouseEvent`, a mouse press
  on the game and a release over a document. Expected: `Input.getTouches()` is empty afterwards.
- **AC5** `TestInputPassThrough.cpp`, `PassThrough.MoveListenerOnBodyDoesNotTakePresses`:
  - a `pointermove` listener on `body` lets a press through, and `HTMLUI-INPUT-0001` is not raised;
  - a `click` listener takes the press and raises `HTMLUI-INPUT-0001` once.
  The existing tests pinned to the old rule are updated in the same commit.
- **AC6** `tools/mcp/tests/test_html_ui_input.py::test_joystick_survives_popup`: `send_input` holds a
  touch, a Lua script shows a popup document under it, and `send_input` lifts it. Expected: the
  script's `onTouchEnd` count is 1 (read from the Console).

### Hit testing and scrolling (P2)
- **AC7** `TestInputHitTransform.cpp` (new), `HitTest.TranslatedChildInClippedParent`: a button with
  `transform: translateY(-200px)` inside an `overflow: hidden` viewport. Expected: `inspect_ui` and
  `GetElementAtPoint` at its drawn centre return it, and at its layout position return the viewport.
  Red on base.
- **AC8** `HitTest.ChildOutsideScaledParent`: a panel with `scale: 0.75` and a child at
  `right: -60px`. Expected: the child is hit at its drawn rectangle, sampled at 9 points.
- **AC9** `HitTest.RoundedClipCorner`: a corner of an ancestor with `overflow: hidden;
  border-radius: 40px` does not hit the child behind the curve.
- **AC10** `HitTest.ClipPathShapes`: one test per shape:
  - `polygon` (both fill rules), `circle`, `ellipse`, `inset` with round, `path`, `xywh`, `rect`;
  - `calc(100% - 20px)` points;
  - sampled inside and outside against the rendered mask, agreeing on 64 sample points.
  Red on base: the corners hit.
- **AC11** `TestScrollPhysics.cpp` (new):
  - `ScrollPhysics.MouseDragPansWithPointerScrollDrag`;
  - `ScrollPhysics.ElasticOverscrollReturns`: release at 120 px over, back within 0.1 s x 6 on a
    fixed UI clock, positions matching a reference SmoothDamp within 0.5 px;
  - `ScrollPhysics.DecelerationRateMatchesUGUI`: velocity after 1 s within 1 % of `v0 * 0.135`;
  - `ScrollPhysics.DragCancelsClick`;
  - `ScrollPhysics.ClampedDefaultUnchanged`: today's inertia trace, recorded before the change.
- **AC12** `test_html_ui_input.py::test_scroll_drag_pointer`: `send_input` mouse drag on a list with
  `scroll-drag: pointer`. Expected: `scrollTop` changes, and no `click` is logged.
- **AC13** Perf, `RenderPerfTest.cpp`: `HitTestPerf.TenThousandRowList` ≤ 110 % of a baseline
  recorded from the base (`perfSlack()`).

### Glyphs (P3)
- **AC14** `GlyphEffectPerf.OutlinedGoFirstDraw` (new, in `RenderPerfTest.cpp`): R39, both
  configurations.
  - Reported through `perfSlack()`, ≤ 8 ms.
  - Red on base with transform-aware text off: the measured time, recorded in the commit message.
- **AC15** `TestFontEffects.cpp`, `FontEffects.DilationMatchesKernel`: EDT outline versus the old
  `ConvolutionFilter` output for 20 glyphs at sizes 12-324, widths 1-24. Each pixel within ±8 of 255;
  coverage area within 1 %. `FontEffects.OutlineBitmap324Under1_5ms`: R40.
- **AC16** `TestFontEngineAtlas.cpp`, `FontEngineAtlas.EffectBitmapSharedAcrossColourAndOffset`: an
  outline and an offset glow of equal width in two colours make one CPU bitmap (bitmap count = 1).
- **AC17** `TestDocumentSystem.cpp`, `DocumentSystem.GlyphCacheSurvivesContext`:
  1. load the shell fixture (`engine/tests/htmlui/fixtures/arcade-shell/`, new, port-like:
     outlined LINESeedSans-style text and a 2048² atlas);
  2. destroy the context;
  3. load it again.
  Expected first-frame stats: `glyphsRasterized == 0`, `textEffectFallbacks == 0`. R41's time goes
  through `perfSlack()`. Red on base.
- **AC18** `DocumentSystem.GlyphCacheSurvivesViewLoss`: `Engine::systemViewDestroyed` and then
  `systemViewLoaded`. Expected: glyphs are not rasterised again; pages are uploaded again.
- **AC19** `TestScriptLua.cpp`, `ScriptLua.PrewarmMakesFirstShowCold-Free`:
  1. `doc:prewarm()` on a document with a `display: none` panel;
  2. wait for `done`;
  3. show the panel.
  Expected: `glyphsRasterized == 0` and `imagesDecoded == 0` in that frame. No frame during the
  prewarm exceeds the 2 ms placement budget plus 1 ms.
- **AC20** `TestGoldenFontEffects.cpp`, `GoldenFontEffects.FallbackGlyphGetsEffect`: "CAÑÓN" in a
  fixture font without Ó plus a fallback face. Expected: the Ó has the outline and is emboldened
  (golden), and `HTMLUI-FONT-0001` is in `doc:getDiagnostics()`.
- **AC21** `test_html_ui_checks.py::test_check_ui_reports_fallback_glyphs`:
  `jasperengine-editor check --ui` on that fixture reports `HTMLUI-FONT-0001` with file:line.

### Images (P4)
- **AC22** `TestRenderRenderer.cpp`, `RenderRendererGpu.ImageDecodeIsAsync`: a 2048² PNG `<img>`.
  - The first frame's main-thread time is within R42, through `perfSlack()`.
  - The texture is blank in frame 1 and complete by frame 30.
  - With `decoding="sync"` it is complete in frame 1.
  - With a fixed UI time it is complete in frame 1.
  - Red on base: frame 1 > 100 ms.
- **AC23** `RenderRendererGpu.ImageCacheSharedAcrossContexts`: two contexts, one a texture-mode
  document, show the same PNG. Expected: one decode; after the first context is destroyed, a new
  context gets `imageCacheHits == 1` and 0 decodes. With `html_ui_image_cache_mib = 0`, 1 decode
  after destruction.
- **AC24** `TestGoldenRender.cpp`, `GoldenRender.AtlasRegions`: `#xywh=` (pixels and percent) and
  `#sprite=`, each on `img`, `background-image` with `repeat`, `border-image` and `mask-image`.
  Compared with goldens of the same pixels as separate files. A bad sprite name gives a diagnostic.
- **AC25** `GoldenRender.BorderImageSliceAtDpRatios`: one 9-slice at dp 0.5, 1, 1.5 and 2. Expected:
  every corner is pixel-identical to its scaled reference and no piece is missing. Red on base at 0.5
  and 2.
- **AC26** `GoldenRender.ImageColorTintsBackgroundAndBorderImage`: R22.
- **AC27** `GoldenRender.FilterOnBorderImage`: `filter: brightness(0.4) hue-rotate(90deg)` on a
  border-image-only element, golden made in Chrome (`tools/htmlui` reference capture). If red after
  P4 and the cause is in the layer path, it is recorded in S92's ledger and marked `DISABLED_` with an
  issue, never skipped.

### CSS and style sheets (P5)
- **AC28** `GoldenRender.InsetShadowAboveGradient`: a box with `linear-gradient` and an inset shadow.
  Golden from Chrome. Red on base.
- **AC29** `GoldenRender.CalcGradientStops`: `linear-gradient(red calc(100% - 10px), blue)` and its
  radial and conic forms. Golden from Chrome.
- **AC30** `ExamplesCompatTest.cpp`, `SupportTables.NoNoteWaitsForAPresentForkFeature`: fails while
  any row's note says "until the fork has X" for a present X. `html-ui-support.md` is regenerated
  (`tools/mcp/gen_html_ui_support.py`).
- **AC31** `TestTextureMode.cpp`, `TextureMode.SharedSheetDecoratorsPerRenderManager`: one CSS with a
  decorator and a box-shadow used by a screen document and a texture-mode document. The two have
  different texture handles, and destroying one leaves the other drawing (golden).
- **AC32** `TestScriptElement.cpp`, `ScriptElement.VisibilityHiddenReadsHidden`: R28.
- **AC33** `ScriptElement.AddStyleSheetTargetsPseudoElements`, covering R29:
  - `::before` styled through an injected sheet;
  - `remove()` restores the old style;
  - the sheet survives a live patch reload;
  - `inspect_ui` sources name it.
- **AC34** `GoldenRender.MixBlendModes`: 16 modes over a gradient backdrop, inside a document, on
  glcore and Vulkan. Golden from Chrome within 2/255. `plus-lighter` draws no backdrop copy (render
  stats). Lands after S92.
- **AC35** `GoldenText.TextAutosizeFits`:
  - three boxes, short, long and multi-line, with `text-autosize: 18px 80px`;
  - the fitted size equals the largest whole px whose layout does not overflow (checked against a
    brute-force relayout);
  - no frame shows another size (frames 0 and 1 identical);
  - `TextAutosizePerf` within R44.
- **AC36** `test_html_ui_lua.py::test_external_link_opens_url`: a click on `<a href="https://...">`
  logs `System.openURL` (a test hook records the call, nothing opens). With `preventDefault`, no call.
- **AC37** `test_html_ui_render.py::test_text_changing_every_frame_draws`: headless editor, a label
  set from `onUpdate` every frame. Five screenshots each show the current number (OCR-free: the
  digits' pixel boxes differ per frame). Settles U9: if green on base, recorded as not reproducible
  and kept as a guard.
- **AC38** `test_html_ui_reload.py::test_css_hot_reload_in_play_keeps_rendering`: in play, the
  stylesheet is rewritten 10 times. `editor_status` frame count advances and the last screenshot shows
  the last style. Settles U25.

### UI Document and Text (P6)
- **AC39** `TestHtmlUIClock.cpp`:
  - `HtmlUIClock.GameTimeDocumentFollowsTimeScale`: two documents, one in Game Time. With
    `Engine.timeScale = 0.5`, a 1 s CSS transition finishes after 2 s in the Game Time document and
    1 s in the other.
  - With `timeScale = 0`, the Game Time one does not advance.
  - anime timelines and `text-effect-time: auto` follow their document.
  - Red on base.
- **AC40** `test_html_ui_lua.py::test_update_mode_property`: `create_ui_document` with
  `properties: {update_mode: "game"}`, then play with `Engine.timeScale = 0`. Expected: a
  `@keyframes` element's computed `left` is unchanged over 30 frames.
- **AC41** `TestRenderP1Golden.cpp`, `RenderP1Golden.WorldSpaceDocumentIsOccluded`: a World Space
  document behind a cube is hidden where the cube covers it (golden on glcore and Vulkan). Pointer
  input through `send_input` hits its button in front and misses behind.
- **AC42** `editor/tests/...` (Inspector command tests): changing Update Mode, Render Mode World
  Space, Pixels Per Unit, Billboard, Text Alignment and Auto Size are each undoable (undo and redo
  round trip of the serialized component).
- **AC43** `engine/tests/core/ui/TextLayoutTest.cpp` (new), `TextLayout.HorizontalAlignmentPerLine`
  and `TextLayout.AutoSizeFitsFixedBox`: line x offsets for Left, Center, Right and Justified, and the
  auto-size result equal to a brute-force fit. Plus `test_ui_scaling.py::test_text_alignment_golden`.
- **AC44** `ReflectParity.*` updated with the new keys. An old scene in
  `editor/tests/fixtures/migrations/` loads with the defaults (no migration needed;
  `StreamMigrations` is unchanged).

### Gates for every package
- **AC45** `tools/check-changed` green for the package's change, with ran, skipped and disabled counts
  reported.
- `WERROR=1 tools/build.sh linux` and `tools/build.sh htmlui-tests` with the package's filter.
- `tools/mcp/tool_budget.py --check tools/mcp/budget.json` where tool text changed.
- `python3 tools/check_raw_writes.py` where files are written (R29 file loads do not write).
- `tools/lua_api/tests/test_lua_api.py` for every binding change (`.claude/rules/scripting.md`).
- **AC46** Exported HTML5 (single-thread and `WEB_THREADS=1`) and Android emulator runs of
  `tools/test-projects` HTML UI projects:
  - no new errors (`tools/android-emulator-test.sh --forbid 'E/JasperEngine'`);
  - AC1's scenario scripted in a test project, read from logcat or the web gate.

## 8. Dependencies & conflicts

- **S92 (render pipeline).**
  - **S92 owns in `RmlRenderer.cpp`:** layers, masks, passes, filters, composite and MSAA
    (`PushLayer`, `PopLayer`, `CompositeLayers`, `ensureMask`, `RenderToClipMask`, `applyFilter`,
    `bindLayer`, `beginPass`, `endPass`, `drawComposite`, `copyLayerInto`). It also owns
    `HtmlUIContextRender.cpp`, `UIDocumentSystemRender.cpp`, `HtmlUIContext::update`,
    `ElementBackgroundLayers.cpp` and `ElementEffects.cpp`.
  - **S93 owns in `RmlRenderer.cpp`, and nothing else there:**
    - P3: `createAtlasTexture`, `createTextureVariant`, `noteDrawn`, `uploadAtlasTexture`,
      `getAtlasTextures`, `updateAtlasTextures` and `addTextStats` (`:900-991`).
    - P4: `LoadTexture`, both `stripTextureFilter` overloads, `GenerateTexture`,
      `GenerateTextureWithWrap` and `createStaticTexture` (`:993-1130` and its definition).
  - **AC27 (U10)** goes to S92 if the cause is in the filter path.
  - **R30 (`mix-blend-mode`)** lands after S92 merges, on the composite API S92 leaves. S92 must keep a
    blend parameter on its layer composite and a backdrop copy (`copyLayerInto`); this is recorded here
    for S92's author.
  - **R36 (World Space)** draws in the transparent pass. It does not touch the HTML UI screen
    composite that S92 moves after the uber pass (U34).
  - **Both specs append to `HtmlUIRenderStats.h`;** keep both sets of counters.
- **S95 (Lua runtime).**
  - `System.openURL` and `Screen.safeArea` are done (§3.8). S95 must not re-spec them; R32 only uses
    `openURL`.
  - S30 (documents rebuilt after an Android background return) is S95's, and R14 is compatible with
    it.
  - Lua bindings are shared files (`HtmlUIClassesLua.cpp`, `GameObjectClassesLua.cpp`, stubs).
    Append-only edits.
  - If S92's LuaBridge overload patch (R66) lands first, it changes nothing here.
- **S98 (tooling).**
  - R17's `check --ui` report goes through the compat checker (`engine/core/htmlui/compat`), not a new
    CLI flag.
  - If S98 reshapes `check`, P3 keeps the code `HTMLUI-FONT-0001` and its checker function.
- **S94:** none. Native Text alignment (P6) touches `UISystem.cpp` text layout only (`:546-600`).
- **Merge hotspots inside S93:**
  - `compat/tables/PropertyTable.cpp` and `ValueTable.cpp`: P2 (scroll properties), P5 (`text-autosize`,
    `mix-blend-mode`, notes). Append rows; resolve by keeping both.
  - `HtmlUIContext.cpp`: P1 owns `isPointerEvent`, `listensForPointer` and `passesPointerThrough`
    (`:47-100`, `191-217`); P3 owns `destroy` (`:219-243`).
  - `HtmlUIClassesLua.cpp` and `stubs/HtmlUI.lua`: P2, P3, P4, P5, P6. Each adds its own block.
  - `docs/mcp/guide/html-ui.md`: one section per package.
  - Generated files: `html-ui-support.md`, `TOOLS.md`, `RcssProperties.h` and `RuntimeFields.gen.inc`
    are regenerated by whoever merges last, never hand-merged.
  - `HtmlShapeDecorator.cpp`: P2 only. The shape `calc()` of U23 is P2's.

## 9. Risks & defaults

- **The RmlUi texture-source lifetime (R13).** Keeping fonts while render managers die needs D7's
  release hook.
  - Default: patch the fork.
  - If a source outlives its manager in a sanitizer run (`SANITIZE=address` on `htmlui-tests` with
    AC17's filter), the fix is in the hook, not by going back to releasing fonts.
- **Effect bitmaps change pixels (D6).** Default: accept within AC15's tolerance and rewrite the
  font-effect goldens in one commit, each image looked at.
- **Async images pop in (D8).** Default async. If a shipped game shows a visible pop, it uses
  `decoding="sync"` or `doc:prewarm`; the guide says so where images are introduced.
- **Single-thread web.** The decode stays synchronous; the shared cache and prewarm still remove
  repeat costs. Default: no browser `createImageBitmap` path (out of scope).
- **Hit testing under 3D transforms (perspective).** A singular projection keeps today's "no hit".
  Default: the clip test uses the same `Project` as the element.
- **Pass-through change (D3).** A project relying on a move-only body listener to block the game loses
  that. Default: accept; `HTMLUI-INPUT-0001` and the guide make the rule visible.
- **World Space sorting.** It is sorted with other transparent objects by distance. Default: no
  per-document sort override; a follow-up if a game needs one.
- **U9 and U25 may not reproduce.** Default: the tests stay as guards, and the verdict is updated to
  "not reproducible on main" in the ledger.
- **`text-autosize` relayout cost.** Default: at most 7 measure passes (binary search over [min, max]
  whole px), and only when inputs change; above R44 the package splits measure from layout before
  merging.

## 10. Triage

| Item | Verdict | Evidence (base `82ccd7c6a`) | This spec |
|---|---|---|---|
| U1 border-image slices vs dp ratio | MISSING | `DecoratorCommon.cpp:350` scales the intrinsic size; `HtmlBorderImageDecorator.cpp:241-249` uses raw NUMBER slices | R21, AC25 (P4) |
| U2 child outside a scaled parent | MISSING (inferred) | `Context.cpp:1430-1437`: local projected point vs untransformed clip rect | R6, AC8 (P2) |
| U3 anime/CSS clock ignores game time | MISSING | `HtmlUIClock.h:13-17` unscaled only; `RmlSystem.cpp:66-68`; `Element.cpp:3483,3598,3651,3763`; only text effects have `game` (`HtmlTextEffects.cpp:306-308`) | R35, D11, AC39-40 (P6) |
| U4 safe-area insets | FIXED | `9938353c0`, `b0e97e481`; `html-ui.md:1706-1719`; `lua-api.md:915-916`; Web `JasperEngineWeb.cpp:193-230` | none |
| U5 CanvasScaler match / Expand | FIXED | `8fc9ba6ba`; `CanvasScaler.h:13-48` | none |
| U6 native Text: outline, alignment | PARTIAL | outline and shadow `8fc9ba6ba` (`TextComponent.h:45-58`); no alignment (`UISystem.cpp:546-566`) | R37, D15, AC43 (P6) |
| U7 real text autosize | MISSING | no property or API (repository search); port estimate `UIKit.lua:283-370` | R31, D12, AC35 (P5) |
| U8 openURL | FIXED + residue | `b870f5e96`; `System.cpp:164-175`; six platform implementations; stale hint `AttributeTransform.cpp:272`; links only notify | R32, D16, AC36 (P5) |
| U9 per-frame text not drawn (headless) | UNVERIFIED | candidates `ElementText.cpp:1219-1322`, `RmlFontEngine.cpp:1654-1671` | R33, AC37 (P5) |
| U10 filter on border-image | UNVERIFIED | layer includes decorators (inferred) | R23, AC27 (P4; S92 if layer path) |
| U11 mix-blend-mode | MISSING | `html-ui-support.md:1058` | R30, D13, AC34 (P5, after S92) |
| U12 touch drag-scroll + inertia | PARTIAL | touch pan and inertia `445de6757` (`ContextPointer.cpp:481-490, 619-668, 751-769`); no mouse drag (`:604-605`), no elastic, no uGUI parameters | R8-R10, D5, AC11-12 (P2) |
| U13 translate inside overflow clip | MISSING (inferred) | `Context.cpp:1430-1437`, `ElementUtilities.cpp:186-235` | R6, AC7 (P2) |
| U14 image tint | PARTIAL | `image-color` on `<img>` (`html-ui.md:1256`); ignored by `DecoratorCommon.cpp:391-396` | R22, AC26 (P4) |
| U15 `.tmp` ignores taps | GAME-SIDE | `PORT/assets/ui/shared/widgets.css:31-33` sets `pointer-events: none` | none |
| U16 atlas regions for border-image | MISSING | no fragment parsing (`RmlRenderer.cpp:1070-1100`) | R20, D10, AC24 (P4) |
| U17 stylesheet injection | MISSING | no API in `HtmlUIClassesLua.cpp` or `UIDocument.h` | R29, AC33 (P5) |
| U18 multi-touch | FIXED | `a061d6062`, `dce43d31e`; `html-ui.md:1588-1592` | none (AC6 covers the mixed case) |
| U19 document-wide listener swallows touches | PARTIAL | by design and documented (`HtmlUIContext.cpp:213-216`, `html-ui.md:1509-1513`); move-only listeners block too (`:56-64`) | R4-R5, D3, AC5 (P1) |
| U20 giant glyphs | RETRACTED | the ledger retracts it | none |
| U21 depth-tested world UI | PARTIAL | TEXTURE mode on a mesh; native Text depth (`Text.h:131-133`); no one-step World Space | R36, D14, AC41 (P6) |
| U22 inset shadow under gradients | MISSING | `ElementBackgroundBorder.cpp:574-603, 641` before layers `:176-181` | R24, D17, AC28 (P5) |
| U23 calc(% ± px) in stops and shapes | MISSING | shapes: `HtmlShapeDecorator.cpp:35-41`; stops: unverified (`Background.cpp:100-125`) | R7/AC10 (P2), R25/AC29 (P5) |
| U24 support table stale | MISSING | `ForkFeatures.cpp:26` present vs `PropertyTable.cpp:161,163`, `ValueTable.cpp:117`; 202 "until the fork has" notes | R26, AC30 (P5) |
| U25 Game view stops after CSS reload | UNVERIFIED | reload rewritten in `2d8026a38` | R34, AC38 (P5) |
| U26 caches not keyed by render manager | MISSING | `BoxShadowCache.cpp:37-48`, `StyleSheet.cpp:104-123` | R27, AC31 (P5) |
| U28 `hidden` reads `collapse` | MISSING | `StyleSheetSpecification.cpp:713` | R28, D18, AC32 (P5) |
| U29 game-owned release swallowed | MISSING (off-canvas half improved by `dce43d31e`, no game event) | `UIDocumentSystemInput.cpp:303-315, 336-343`; `HtmlUIContextInput.cpp:56-100`; `Engine.cpp:3732-3741, 3766, 3960-3966` | R1-R3, D1-D2, AC1-4, AC6 (P1) |
| U30 outlined-glyph stalls | PARTIAL | S16 GPU effects, in-place uploads, per-handle versions; still the dense CPU fallback while pending (`RmlFontEngine.cpp:1302, 1324-1327`; `ConvolutionFilter.cpp:42-88`), keys with offset and colour (`:1043`), cache dropped per context (`HtmlUIContext.cpp:242`, `Core.cpp:443`), no prewarm | R11-R15, R39-R41, D6-D7, AC14-19 (P3) |
| U31 images decoded on main thread | MISSING | `RmlRenderer.cpp:993-1063`; per-context release `HtmlUIContext.cpp:241` | R18-R19, R42, D8-D9, AC22-23 (P4) |
| U32 clip-path hit test | MISSING | `ValueTable.cpp:139` "no hit-test clipping"; `Context.cpp:1437` | R7, D4, AC10 (P2) |
| U33 fallback glyphs: effect and warning | PARTIAL | effects per face (inferred, `RmlFontEngine.cpp:1294, 1326, 1469, 1488`); no diagnostic | R16-R17, AC20-21 (P3) |

Counts (32 items):
- 4 FIXED: U4, U5, U8, U18;
- 7 PARTIAL: U6, U12, U14, U19, U21, U30, U33;
- 16 MISSING: U1, U2, U3, U7, U11, U13, U16, U17, U22, U23, U24, U26, U28, U29, U31, U32;
- 3 UNVERIFIED with deciding tests: U9, U10, U25;
- 1 GAME-SIDE: U15;
- 1 RETRACTED: U20.

## 11. Work packages

Each package runs in its own worktree from main and branch `feature/s93-<pN>-<slug>`, merges main
before landing, and follows §8's ownership.

| Package | Requirements | Owns (files and directories) | Shared (how) | Size | Depends on |
|---|---|---|---|---|---|
| **P1 Pointer ownership** | R1-R5, AC1-AC6 | `engine/core/subsystem/UIDocumentSystemInput.cpp`; `engine/core/htmlui/HtmlUIContextInput.cpp`; `HtmlUIContext.cpp` `:47-100, 191-217`; `engine/core/Engine.cpp` `systemTouchEnd`, `systemTouchMove`, `systemMouseUp`, `systemMouseMove`; `engine/tests/htmlui/TestInput*.cpp`, `TestInputPassThrough.cpp`; `tools/mcp/tests/test_html_ui_input.py` (ownership cases) | `compat/DiagnosticCodes.cpp` (append `HTMLUI-INPUT-0001`); guide "Pointer input" section | M | none |
| **P2 Hit testing and scrolling** | R6-R10, R43, U23 shapes, AC7-AC13 | fork `Source/Core/Context.cpp` (`GetElementAtPoint`), `ContextPointer.cpp`, `ScrollController.{h,cpp}`, `ElementUtilities.cpp` (new `IsPointWithinClip`); `compat/runtime/HtmlShapeDecorator.{h,cpp}`; new `engine/tests/htmlui/TestInputHitTransform.cpp`, `TestScrollPhysics.cpp`; `docs/html-ui/FORK.md` entries | compat tables (append scroll properties); `HtmlUIClassesLua.cpp` (scroll block); `test_html_ui_input.py` (scroll cases, separate functions from P1) | L | none |
| **P3 Glyph cost and fonts** | R11-R17, R39-R41, AC14-AC21 | `engine/core/htmlui/RmlFontEngine.{h,cpp}`, `RmlFontEffects.{h,cpp}`, `RmlFontMsdf.*`; fork `FontEffectOutline.cpp`, `FontEffectGlow.cpp`, `FontEffectBlur.cpp`, `FontEffectShadow.cpp`, `ConvolutionFilter.*`, `Core.cpp` (`ReleaseRenderManagers`), `CallbackTexture.*`; `HtmlUIContext.cpp` `destroy` (`:219-243`); `RmlRenderer.cpp` `:900-991` (atlas); new fixture `engine/tests/htmlui/fixtures/arcade-shell/` | `HtmlUIRenderStats.h` (append); `HtmlUIClassesLua.cpp` (prewarm block); compat checker (font diagnostic); `doc:prewarm` image half calls P4's `HtmlUIImageCache::prewarm` | L | P4 for the image half of `doc:prewarm` (land after P4, or ship text first and add images when P4 is on main) |
| **P4 Images** | R18-R23, R42, R48, R50 image counters, AC22-AC27 | `RmlRenderer.cpp` `LoadTexture`, `stripTextureFilter`, `GenerateTexture*`, `createStaticTexture`; new `engine/core/htmlui/HtmlUIImageCache.{h,cpp}`; `compat/runtime/DecoratorCommon.{h,cpp}` (`Image`), `HtmlBorderImageDecorator.cpp`, `HtmlImageElement.cpp` (`decoding`); project setting `graphics.html_ui_image_cache_mib` and its Project Settings row | `HtmlUIRenderStats.h` (append); `HtmlUIClassesLua.cpp` (`prewarmImage`); Project Settings > Graphics > HTML UI (S92 adds MSAA rows there: append) | L | none (AC27 may hand off to S92) |
| **P5 CSS correctness and style sheets** | R24-R34, R44, AC28-AC38 | fork `ElementBackgroundBorder.cpp`, `BoxShadowCache.cpp`, `StyleSheet.cpp`, `StyleSheetSpecification.cpp`, `PropertyParserKeyword.cpp`, `DecoratorGradient.cpp`, the fork's layout for `text-autosize`; `compat/css/props/Background.cpp`; `compat/tables` notes; `compat/html/transforms/AttributeTransform.cpp`, `compat/runtime/HtmlAnchorElement.cpp`; new `engine/core/subsystem/UIDocumentSystemStyle.cpp`; stage 2: the `mix-blend-mode` property, compat row and blend program | compat tables (append); `HtmlUIClassesLua.cpp` (style-sheet block); stage 2 edits S92's composite API (after S92 merges) | L | stage 1 none; stage 2 (R30, AC34) after S92 |
| **P6 UI Document and Text components** | R35-R37, R46-R47, R52, R55, AC39-AC44 | `engine/core/htmlui/HtmlUIClock.{h,cpp}`, `RmlSystem.cpp` (`GetElapsedTime`), fork `Element.cpp` (animation clock call sites), `ElementDocument.*`, `Clock.*`; `engine/core/htmlui/anim/*`, `HtmlUIAnimeLua.cpp`, `effects/EffectRuntime.cpp` (clock), `HtmlTextEffects.cpp` (`now`, `auto`); `engine/core/component/UIDocumentComponent.h`, `UIDocumentSystem.cpp` (`update` tick), `UIDocumentSystemTexture.cpp` (World Space), `UIWorldRaycast.cpp`; `engine/core/component/TextComponent.h`, `engine/core/object/ui/Text.{h,cpp}`, `UISystem.cpp` `:546-600`; `editor/reflect/components/UIReflect.cpp`, `Enums.cpp`, the UI Document and Text Inspector code (`editor/window/InspectorDefault.cpp`, `InspectorWindow.cpp` sections) | `RenderSystem.cpp` (one transparent-queue draw for World Space; S94 and S92 hotspot, keep it to one call site); `GameObjectClassesLua.cpp` (Text block); `RuntimeFields.gen.inc` and reflection goldens (regenerate) | L | none |

Suggested order:
1. P1, P2, P4 and P6 start at once.
2. P3 starts at once and lands its text half first.
3. P5 stage 1 starts at once; stage 2 waits for S92.

The last package to merge regenerates `html-ui-support.md`, `TOOLS.md` and `RcssProperties.h`, and
runs `tools/mcp/tool_budget.py --check`.

## Deviations

Recorded by the packages and the integration. The code is what is true now.

**P1 Pointer ownership**
- R1 also covers documents on meshes (`UIDocumentSystemTexture.cpp`): they swallowed releases the same way.
- A body move listener gets moves only over the body's children (D3).
- HTML5 off-canvas releases: a window `mouseup` listener only, no `pointerup`/`pointercancel` or
  `setPointerCapture`; the `web-pointer` gate passes with it, and losing window focus already releases held
  buttons.

**P2 Hit testing and scrolling**
- The fork changes are one `rmlui:` commit plus a speed tweak, not one commit per FORK.md row.
- A mouse pans after 10 dp (uGUI's drag threshold), touch after 3 dp.
- A pen no longer pans under `scroll-drag: touch` (the engine has no pen input, so nothing changes today).
- `scroll-elasticity` is stored as text and read as s or ms.
- Elastic overscroll is a separate offset: `scrollTop` and the scrollbars stay in range.
- With several `clip-path` shapes, a point hits only inside all of them.

**P3 Glyph cost and fonts**
- The glyph cache also survives `Rml::Shutdown` (D7 said until then): a scene change would otherwise empty it.
- AC15's "±8 of 255" is read as "at least 247 on every pixel the kernel fills".
- The prewarm test is `ScriptLua.PrewarmMakesFirstShowColdFree`.
- No extra page-upload batching: uploads already happen once per frame.
- FORK ids are S93-G1..G3 (P6 holds S93-F); the commit messages say F1..F3.
- AC21: `check --ui` stays engine-free (loading a font there aborted the process); HTMLUI-FONT-0001 comes from
  an RML/RCSS font resolution plus a cmap read with the runtime's fallback chain. Selectors with
  pseudo-classes or attributes are not evaluated, `@media` counts as matching, and it reports characters a
  fallback face draws, not characters no face has. Text whose first face is built in is not reported (also at
  runtime), which narrows R17.

**P4 Images**
- `image-color` does not tint `mask-image`.
- Project Settings > Graphics > HTML UI Image Cache uses -1 for the platform default (0 keeps none).
- AC27 is a browser-parity fixture, not a `GoldenRender` test; AC23 is the fixture test `RenderRendererGpu`.
- `RmlRendererTest.NoLeaksAfterContextDestroy` and `AfterSceneReload` changed on purpose (the shared cache
  keeps decoded images past a context).
- U10 is not a bug on current code (it matches Chromium).

**P5 CSS and style sheets**
- The "golden from Chrome" criteria are browser-parity fixtures; `MixBlendModeBackdropCopies` is a pixel probe.
- Lua reads the `text-autosize` fit through `el.style.fontSize` (there is no `getComputedStyle`).
- Inset shadows get their own texture only when background images or decorators are present.
- `doc:addStyleSheet` works on `.html` documents and warns on `.rml`.
- FORK rows are S93-C1..C5; the golden-ledger checker accepts new Chromium references.
- `mix-blend-mode` is built on main's composite (`compositeLayersNow`, `ElementEffects`, a `Bmd` post
  variant), not on S92's: see "Merge order" below.
- U9 and U25 did not reproduce; their guard tests stay.

**P6 UI Document and Text**
- The generic component tools keep camelCase keys; only `create_ui_document` also takes snake_case.
- The anime option is the existing `clock`, not a new `time`.
- Game Time has its own clock: `getGameTime()` also runs at edit time.
- AC41 has its own test file and drives the pointer from C++; AC43's MCP test measures where the text lands
  instead of comparing an image.
- Behaviour changes: a fixed UI time also holds the game clock; anime `clock = "game"` stops while the editor
  is paused; `text-effect-time` defaults to `auto`; `world` is a valid render mode.
- The built-in shader cache moves to `v73` (World Space keeps `htmlui.vert`'s depth), the next free number after S94's `v72` in the landing order (#3 `v66`, S92 `v69`, S99 `v70`, S100 `v71`, S94 `v72`).
- World Space documents draw in every camera pass; the UI Document Inspector shows two "Billboard" rows.

**Train C (with S94, user decision)**
- S94 (#19) gives Text Unity TextMesh's Anchor (`anchorHorizontal`, `anchorVertical`: which point of the
  box sits on the origin) and migrates the old pivots into it. R37's Alignment stays beside it under its
  own keys (`horizontalAlignment` Left/Center/Right/Justified, `verticalAlignment`
  Top/Middle/Bottom/Baseline: the lines inside the box), with its own Inspector row, Lua properties and
  MCP keys. The layout applies both: the alignment moves the lines inside the box, then the anchor puts
  the box on the origin (`BitmapText.AnchorAndAlignmentApplyTogether`). R37's vertical alignment no
  longer depends on a baseline pivot, which S94 removed.

**Integration**
- R37 Auto Size with a bitmap font (P6 left it out): the largest scale k whose native size x k lies in
  Font Size Min..Max and fits the fixed box, Min's scale when none fits (a range holding no multiple takes
  Max's, at least x1). Its atlas does not change with k, so a refit only rebuilds the quads. The Inspector
  shows Auto Size for bitmap fonts too.
- HTMLUI-INPUT-0001 is in phase 2's Appendix F and the codes test sample.
- `test_html_ui_checks.py::test_cli_default_includes_ui` reads the `check` summary as a list: its whole-line
  regex broke whenever a check added a category (`titles`, on main too).
- `FullPathGoldenTest.UIParticleSystemsClippedByAPanel` waits for the particles instead of 4 frames: a burst
  at time 0 comes with the first simulation tick (1/30 s of game time), which the few fast frames of a warm
  renderer do not reach. It failed whenever the particle shader was already compiled (later in a batch, or a
  repeat).

**Re-merge with main (CI, PR #4)**
- `project.yaml` omits `htmlUiImageCacheMiB` at its default (-1): it was always written, so an older project
  re-saved with a new line.
- R42's upload-frame bound (<= 4 ms) holds on hardware devices only. On the reference renderer (llvmpipe),
  drawing is the main thread's own CPU work. The frame that first draws a 2048x2048 texture took 80-300 ms in
  a fresh process (5 ms on a repeat), while the image cache's upload took 5 ms of it. The added first-frame
  time and the visibility bound still hold there.

**Merge order (coordinator)**
- S92 rewrites the layer composite that `mix-blend-mode` uses. Merge S92 first, then merge main into S93
  again: region layers must blend too (S92's result cache already keys by blend mode).
