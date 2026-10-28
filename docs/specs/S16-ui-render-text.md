# S16 — HTML UI rendering cost & text sharpness

Wave 2. Report: `docs/openengine-analysis/06-ui-framework.md` §1.5, §1.6, §2.1 "Rendering"/"Text",
§3 (rows "Resolution-independent vector text", "GPU text shadow/glow/outline", "One-draw instanced
SDF primitive renderer"), §4.1 items 1–2, §5.5, §5.6. Global decisions: `docs/specs/README.md`.

## 1. Goal

Make our RmlUi-fork HTML UI cheap to draw and sharp under scaling, on every backend including
WebGL2/GLES3. Today a document costs about one draw call per element mesh. A rounded clip costs
stencil passes. Every box shadow and blurred text shadow costs an offscreen layer. Text drawn under
`transform: scale()` is a stretched bitmap. We will:

- batch geometry by state;
- replace stencil clips with scissor or analytic clips where possible;
- draw solid boxes, borders, outlines and box shadows as instanced analytic-SDF primitives;
- rasterize text at its exact on-screen scale, with MSDF glyphs while the scale animates;
- move text effects to the GPU through a distance field.

All of this must keep our CSS coverage (filters, masks, 3D transforms, gradients), world-space UI
and the 661 golden images.

## 2. Scope

**In scope**
- `RmlRenderer` command-stream batching: merged vertex runs, instanced runs, a run cache.
- Scissor and stencil elision, and analytic rounded-rect clips.
- An instanced SDF primitive pipeline `HTMLUI_PRIM` for solid backgrounds, solid borders,
  outlines, and outer/inset box shadows. Textured quads (glyphs, images) also go through
  `HTMLUI_PRIM`.
- Glyph atlas: R8 pages, a memory budget with page LRU eviction, and per-handle versions.
- Text rasterized at its exact on-screen scale, aware of the element's transform, with hysteresis.
- An MSDF/MTSDF glyph atlas (msdfgen, MIT) for animating, large, 3D-transformed and
  `text-rendering: geometricPrecision` text.
- GPU text effects from the MTSDF distance: `font-effect` outline, glow, shadow, blur, and blurred
  CSS `text-shadow`.
- The fork patches these need.
- Render statistics, a benchmark suite, perf gates, golden policy, editor/MCP visibility.

**Out of scope**
- The Slug technique (README global decision: patent not verified clear). Do not port
  `FontAtlas_Slug.cpp` or `slug_functions.glsl`.
- SSBO, bindless, compute or indirect paths (README "Platforms"). OpenEngine's SSBO clip chains,
  transform chains and single `Draw(6, N)` over bindless textures are not ported.
- Replacing RmlUi or our layout/CSS core with theirs (README do-not-copy: Yoga/CSS UI core).
- Gradients, non-solid border styles (dashed, dotted, double, groove, ridge, inset, outset),
  `border-image` and decorators as SDF primitives. They keep today's mesh path. Gradient SDF fill
  is a possible follow-up, not part of this spec.
- The legacy component UI (`UISystem`, `engine/shaderlib/ui.frag`; report §5.5). Separate work.
- Text shaping, bidi and line breaking: unchanged.
- HDR / colour-space-aware compositing (`UITargetSpace.cpp`): not needed until HDR output exists.

## 3. Current state

**Renderer**: `engine/core/htmlui/RmlRenderer.{h,cpp}` (566 + 3039 lines).
- RmlUi's calls are recorded into a `Command` list (48-byte POD) and replayed in `endFrame`
  (`replayCommands`, `RmlRenderer.cpp:2473`). That gives us a full-frame look-ahead today, but we
  do not use it for merging.
- Every `GEOMETRY` command is its own draw. `drawGeometry` (`RmlRenderer.cpp:1390-1420`) does, per
  call: a texture bind, `beginDraw`, an 80-byte `u_vs_htmluiParams` upload (transform +
  translation, `RmlRenderer.cpp:106-133`) and `draw`.
- Draw counters: post passes `:1327`, geometry `:1413`, gradient shader `:2267`.
- Untextured geometry binds `placeholderWhite`, so a text run and a background never share a draw.
- Geometry persists in shared 64 KiB pages (`RmlGeometryPage`, `:62-90`). Translation is applied in
  the vertex shader (`engine/shaderlib/htmlui.vert`).

**Clips**: the fork already uses the scissor for axis-aligned overflow clips. It uses a stencil
clip mask whenever the clipping ancestor has a `border-radius` or a transform
(`engine/libs/rmlui/Source/Core/ElementUtilities.cpp:160-190`: "we can't easily predict if content
is located on the curved region"). Each mask is a stencil clear plus a stencil draw
(`renderToClipMaskNow`, `RmlRenderer.cpp:1493-1535`). With 1 sample the mask is aliased.

**Box shadows**: `GeometryBoxShadow.cpp:180-330` renders every shadow into a texture.
- It pushes a layer, draws stencil clip masks, and runs the blur filter's down/up-sample passes.
- Shadows larger than the window are tiled.
- The result is cached in `BoxShadowCache`. A size, colour or blur change bakes the texture again
  and allocates a new one.

**Blurred CSS `text-shadow`** (fork P3-TX6, `ElementText.cpp:359-395`): every text element and
every frame pays for a layer push, the blur filter's passes and a composite.

**Glyph atlas**: `engine/core/htmlui/RmlFontEngine.{h,cpp}`, stb_truetype rasterization in
`engine/core/util/FontFace.cpp`.
- Pages are 1024², up to 16, append-only, with no eviction (`RmlFontEngine.h:52-54`, `place`
  `:568-596`). Overflow drops glyphs with one log line.
- R8 coverage is expanded to RGBA8 at upload (`makeSource`, `:552-563`): 4 MiB per page instead
  of 1 MiB.
- There is one global version (`GetVersion`, `:816`). Any new page, scale change or reload
  regenerates all text in all contexts.

**Raster scale**: per context only.
- `HtmlUIContextRender.cpp:104-108`: `rasterScale = clamp(round(pixelRatio*4)/4, 1, 4)`.
- A pixel ratio of 1.37 rasterizes at 1.25 and stretches.
- Element transforms are ignored. A `scale(2)` label is a 2× magnified bitmap, and a scale
  pop-in animation is blurry for its whole duration.

**Font effects**: `RmlFontEffects.cpp` (96 lines) runs RmlUi's CPU effects on coverage bitmaps
into RGBA8 effect pages. These glyphs are made at **document resolution** whatever the raster
scale (`RmlFontEngine.cpp:629-635`), so outlines and glows are soft at a pixel ratio of 2.

**MSAA**: each context target defaults to 4× MSAA with a depth-stencil attachment
(`HtmlUIContextRender.cpp:40`; project setting `htmlUiMsaaSamples`, `editor/Project.cpp:1840`).

**Stats**: `HtmlUIRenderStats.h` (passes, drawCalls, pipelineApplies, textureBinds, layersPushed,
filtersApplied, compositesFused, geometryLive, geometryBytes). They are shown in the Game view
overlay (`editor/window/GameWindow.cpp:563`). No MCP tool reports them.

**Benchmarks**: `tools/htmlui-bench` is the standalone **SVG library** benchmark (LunaSVG vs
ThorVG vs nanosvg, `tools/htmlui-bench/README`). It has no UI documents and no draw-call
measurement. The perf gates that exist are CPU-only: `engine/tests/htmlui/compat/compat_perf_test.cpp`
(`CompatPerf.*`) and `engine/tests/htmlui/effects/EffectPerfTest.cpp`, which subtracts backend
time through `RenderBackendTime`. Several tests assert exact draw counts:
- `TestRenderRenderer.cpp:390,406`: `== 5000`;
- `TestRenderGolden.cpp:280`: `>= 5000`;
- `TestRenderP1Golden.cpp:542`: `>= 500`;
- `TestIntegrationP1.cpp:756`: `>= 10`.

**Goldens**: 661 PNGs under `engine/tests/htmlui/golden/`.
- **277 are Chromium references** (`golden/browser/**`, written by
  `tools/htmlui/render-reference.mjs`, strict metric in `BrowserParityTest.cpp`).
- **384 are engine self-goldens** (p1, p6, effects 181, examples 11, compat, property, video,
  root).
- Default compare: channel 24, bad ≤ 0.05 %, mean ≤ 1.0 (`harness/ImageCompare.h:16-20`).
- `JASPER_HTMLUI_UPDATE_GOLDEN=1` rewrites goldens (glcore only).
- `fixtures/p3/known-failures.txt` is empty.

**Instancing**: per-instance vertex buffers already work through
`engine/renders/sokol/SokolObject.cpp:345` (`SG_VERTEXSTEP_PER_INSTANCE`). sokol allows 16
vertex attributes.

## 4. Reference (OpenEngine, MIT, `external/OpenEngine/`)

| File | Take | Do not take / improve |
|---|---|---|
| `Engine/Modules/Rendering/Shaders/UI/sdf_functions.glsl` (338) | `roundedRectSDF` (elliptical corners), `pixelFootprint`, `rectEdgeFootprint`, `sdfCoverage` (box-filter, area-conserving), `erfApprox`, `gaussianShadowAlphaIntegrated`, `evaluateShadowAlpha`: pure math, copy with their header | `computeFillColor` gradient/HSV modes; `uint` packing |
| `Shaders/UI/ui_derivatives.glsl` (43) | derivative helpers | — |
| `Shaders/UI/ui_sdf_common.glsl` (358) | only the per-instance quad expansion idea and `uiTextEffectReachPx` sizing | `push_constant`, SSBO clip and transform chains, HDR paint helpers (we blend premultiplied sRGB like a browser, `engine/shaderlib/htmlui.frag`) |
| `Shaders/UI/text_effects.glsl` (144) | the effect model: outline = threshold, glow = `exp(-2(g/R)^2)`, shadow = offset sample with an erf soft edge, sigma = blur/2. Re-derive it on an MTSDF distance | the Slug distance source |
| `Shaders/UI/text_mask_gamma.glsl` (187) | read only; later tuning | not in this spec (changes every text golden) |
| `Engine/Modules/UI/Include/UI/UIPrimitive.h` (665; 144-byte `UIPrimitive`, `static_assert`s) | the 9×vec4 budget and field set as a model for `HtmlUIPrimitive` | SSBO indices for clips and transforms |
| `Source/UIManager_PrimitiveGen.cpp` (3495) | how per-side borders, inset/outer shadow and spread map to primitive fields | their CSS model |
| `Source/UICompatDrawRuns.cpp` (131), `UIManager_RenderRG.cpp:350-385` | the run-splitting rule (a new run on a texture change) | the render-graph usage |

Licences:
- Copied GLSL keeps OpenEngine's MIT header, and `NOTICE.md` gets an OpenEngine entry if another
  spec has not added one already.
- msdfgen (MIT, Viktor Chlumský) is vendored at `engine/libs/msdfgen`. Only `core/` is taken; no
  `ext/`, no FreeType, no Skia. It gets a `NOTICE.md` entry.
- Glyph outlines come from `stbtt_GetGlyphShape` and are converted to `msdfgen::Shape`.

## 5. Requirements

### Functional — batching and clips
- **R1** In recorded mode, the replay merges consecutive `GEOMETRY` commands into one draw when
  all of these are equal:
  - layer;
  - pipeline state (blend, stencil, write mask);
  - transform matrix;
  - effective scissor;
  - analytic clip;
  - texture image and texture kind.

  Translation is baked on the CPU into a per-frame gathered vertex/index stream (32-bit indices).
  Geometry above 4096 vertices is drawn directly from its persistent buffer and ends the run.
- **R2** Untextured geometry joins the run of the texture bound around it. Every glyph atlas page
  reserves a 4×4 white texel block, and gathered untextured vertices get its centre UV. The
  result must equal sampling `placeholderWhite`: both are exactly 1.0.
- **R3** Run cache: a run whose (geometry handle, translation, texture) sequence hash equals the
  one from the same position in the previous frame reuses last frame's gathered GPU range with no
  copy or upload. Changing one element in a 500-element list re-gathers only its run.
- **R4** Scissor elision: a draw whose transformed bounds (identity or translate/scale-only
  transform) lie inside the current scissor is marked scissor-free. It may merge across scissor
  changes.
- **R5** Stencil elision.
  - Fork patch S16-F2 attaches the clip shape to every `ClipMaskGeometry`: the border-box rect in
    the clipping element's local space, its 4 elliptical radii, and the transform pointer.
  - The shape reaches the renderer through a new optional
    `RenderInterface::RenderToClipMaskShape(op, geometry, translation, const ClipShape&)`. Its
    default forwards to `RenderToClipMask`.
  - At replay, if every draw under a mask lies inside the shape's inner rectangle (the rect
    without its corner boxes), the mask is not drawn. The scissor (shape bounds ∩ scissor) is
    used instead.
- **R6** Analytic clip: a mask list of at most 2 rounded-rect shapes (`Set`, then at most one
  `Intersect`), each under the same transform as the content, becomes `u_fs_htmluiClip`. That is
  2 × (rect, radiiX, radiiY), evaluated by `roundedRectSDF` + `sdfCoverage` on the pre-transform
  document position (a new varying `v_docPos`). There is no stencil write, and the edge is
  antialiased at any sample count. Any other list (SetInverse, more than 2, a different transform)
  uses today's stencil path unchanged.
- **R7** Batching never reorders draws, crosses a layer push/pop, composite, `SaveLayer*`, filter,
  `SHADER` (gradient) or deferred-composite flush, or changes blending. The images of
  `setBatchingForTesting(false)` and `true` match: max channel difference ≤ 1 on every golden
  scene.

### Functional — SDF primitives
- **R8** Pipeline `ShaderType::HTMLUI_PRIM` (`engine/shaderlib/htmlui_prim.{vert,frag}`) draws
  instanced primitives. It uses a static 4-vertex unit-quad buffer plus one per-instance buffer of
  `HtmlUIPrimitive` (≤ 144 bytes, ≤ 14 instance attributes of `FLOAT4`/`FLOAT2`/`UBYTE4N`,
  `static_assert`ed). Modes:
  - `QUAD`: a textured or untextured parallelogram, with UV rect and colour. Used for glyphs,
    images and sheared synthetic italic.
  - `BOX`: fill + 4 border widths + 4 border colours + elliptical radii.
  - `SHADOW_OUTER` and `SHADOW_INSET`: rect, radii, spread, sigma, colour; an outer shadow is
    multiplied by `1 − coverage(border box)`.
  - `MSDF_GLYPH`: see R16.
- **R9** Fork patch S16-F1:
  - `ElementBackgroundBorder::GenerateGeometry`, `GenerateOutline` and `GeometryBoxShadow` emit
    `BoxPrimitive`s through `RenderManager::RenderPrimitives(span, translation)` →
    `RenderInterface::RenderPrimitives`. This happens when the interface reports
    `SupportsPrimitives()` and the box qualifies: background colour solid or none, no
    `border-image`, every visible border side `solid`, outline `solid` or `auto`.
  - A non-qualifying box takes today's mesh path, which is unchanged.
  - Box shadows on qualifying boxes no longer create textures, layers or blur passes, at any
    blur, spread or count.
  - Inline boxes split over lines emit one primitive per fragment.
- **R10** Quad-list detection: `CompileGeometry` classifies a mesh as a quad list when all of
  these hold:
  - its index pattern is RmlUi's `GenerateQuad` order (`MeshUtilities.cpp:66-71`) or
    `RmlFontEngine::addQuad`'s;
  - every quad is a parallelogram (ε = 1e-4 px);
  - its colour is uniform across the 4 vertices;
  - its UVs are affine with the positions.

  It then also stores instances. A quad list or primitive run and a vertex run are separate
  pipelines, so a run of `HTMLUI_PRIM` covers text, backgrounds and borders only when all of them
  are instances. R8–R10 together make that the common case.
- **R11** Primitives work under every transform. 2D, 3D and perspective use the same transform
  uniform, with AA from screen derivatives (`rectEdgeFootprint`). They also work inside layers,
  filters, `mask-image`, group opacity and analytic or stencil clips. They must be equivalent to
  meshes at 1, 2, 4 and 8 samples.

### Functional — text
- **R12** Atlas pages are R8 (`SG_PIXELFORMAT_R8`, sampled `.r` by a coverage texture kind).
  Effect pages that remain CPU (fallback) stay RGBA8.
- **R13** Atlas budget and eviction.
  - The budget is in MiB for base pages and MSDF pages. Defaults: 8 + 4 on Android, iOS and
    HTML5; 16 + 8 on desktop. Project setting `htmlUiGlyphAtlasMiB`.
  - On overflow the page least recently used is cleared and its glyphs are dropped. Only a page
    unused in the current and previous frame may be cleared.
  - Only the face handles that used it get their version bumped. `GetVersion(handle)` becomes per
    handle; a global epoch stays for reloads and scale resets.
  - More than 4 evictions in 120 frames logs diagnostic `HTMLUI-GEN-<next>` "glyph atlas budget
    too small" once per context.
- **R14** Exact-scale text (fork patch S16-F3).
  - `ElementText` computes its effective device scale at render time: context raster scale ×
    the larger singular value of the 2×2 Jacobian of its accumulated transform at the text box
    centre.
  - It passes that scale in a new `TextShapingContext::raster_scale`.
  - The font engine keys glyphs by `round(size × scale × 4)` (¼ device px).
  - The context raster scale is no longer quantized: `pixelRatio` clamped to [0.25, 8].
- **R15** Hysteresis.
  - Text is regenerated only when its effective scale differs from the generated one by more
    than 3 %.
  - While the scale has changed in any of the last 4 rendered frames, the text is "animating":
    it draws MSDF glyphs when all its glyphs are in the MSDF atlas, otherwise it keeps its
    current bitmap geometry stretched (today's look).
  - After 6 stable frames it regenerates once as bitmap at the exact scale.
  - A pending settle keeps `HtmlUIContext::updatedSinceRender` set until the settle frame is
    drawn, so a static context does not keep a stale stretched frame.
- **R16** MSDF/MTSDF atlas.
  - msdfgen generates RGBA8 MTSDF glyphs: 40 px em, distance range 12 px (0.3 em), 1024² pages.
  - Glyphs are made once per (face, glyph, synthetic-bold) and are size-independent.
  - Generation runs on S06's background lane (fallback before S06: a budget of ≤ 1 ms per frame on
    the main thread). It never blocks a frame.
  - MSDF is used for:
    - animating text (R15);
    - device glyph size > 128 px;
    - text under a perspective or non-affine 3D transform;
    - `text-rendering: geometricPrecision`.
  - `text-rendering: optimizeSpeed` turns R14–R15 off for the element (today's behaviour).
  - `auto` and `optimizeLegibility` use the hybrid.
  - Compat `PropertyTable.cpp:774` changes from "ignored" to supported. The support matrix is
    regenerated.
- **R17** GPU text effects.
  - These are drawn as `MSDF_GLYPH` instances with effect parameters:
    - `font-effect` outline, glow, shadow and blur;
    - CSS `text-shadow`, sharp and blurred (fork S16-F3: `ElementText` asks
      `FontEngineInterface::SupportsGpuTextShadow(face, blur)` and on true passes the blur in
      `TextShapingContext` instead of pushing a layer).
  - This applies when the effect's reach (outline width, glow radius, 3σ of the blur) is
    ≤ 0.3 em, i.e. inside the distance range.
  - Effect parameters travel with a per-(page, effect-set) `HtmlUITexture` variant. It shares the
    GPU image, so the fork's `TexturedMeshList` API is unchanged.
  - Beyond the reach, or while glyphs are not ready, today's CPU or layer path is used, and
    `textEffectFallbacks` counts it.
  - GPU effects are evaluated at device resolution, which fixes the document-resolution softness
    of `RmlFontEngine.cpp:629`.
- **R18** `reloadFontFile` (and S05 hot reload) drops that face's bitmap and MSDF glyphs and
  bumps only the affected handles.

### Platform & backends
- **R19** Every path runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan through sokol:
  - instanced vertex attributes only;
  - uniform blocks ≤ 1 KiB per draw;
  - R8 and RGBA8 textures;
  - 32-bit indices;
  - `dFdx`/`dFdy`/`fwidth`.

  No SSBO, texture arrays, integer attributes or compute. `HTMLUI_PRIM` compiles through
  `engine/core/shader` like `HTMLUI`. msdfgen runs on the CPU only and builds with Emscripten and
  the Android NDK.
- **R20** Texture-mode (world-space) documents (`UIDocumentComponent` TEXTURE, `UIWorldRaycast.cpp`)
  get all of R1–R18 with their own pixel ratio. UV hit testing is unchanged.

### Performance (budgets; measured by the suite of §7)
- **R21** Draw calls per frame (GPU draws, posts included):
  - B1 examples: each ≤ 30 % of its baseline, and ≤ 40 draws;
  - B2 `list-500`: ≤ 16 draws, 0 stencil draws, 0 box-shadow textures;
  - B3 `text-wall`: ≤ 4 draws;
  - B5 `text-effects`: 0 layers pushed for effects within the reach.
- **R22** CPU (engine work less `RenderBackendTime`, median of 120 frames after 30 warm-ups,
  Release):
  - B2 animating one card: ≤ 50 % of baseline.
  - B1: ≤ baseline + 5 %.
  - Gathering: ≤ 0.2 ms per 10k gathered vertices on the CI desktop runner.
  - B4 scale pop-in: no frame spends > 2 ms in glyph rasterization, and ≤ 256 bitmap glyph
    rasterizations per frame (the rest wait for the settle).
  - A static context still renders nothing (0 draws, `HtmlUIContextRender.cpp:139`).
- **R23** Memory:
  - glyph atlas GPU bytes for the same glyph set ≤ 30 % of baseline (R8);
  - total atlas ≤ the budget of R13 at all times;
  - B2 box-shadow texture bytes = 0;
  - the gathered stream and run cache ≤ 2 × the frame's gathered bytes (capacity reuse, as
    `RmlTransientBuffer`).
- **R24** GPU: B2 fragment cost at 1280×720 on the CI GL runner (S15 GPU pass timings, when
  available) is ≤ baseline. The SDF fragment shader early-outs to fill colour inside the inner
  rect.

### Editor UX (human)
- **R25** Project Settings → HTML UI gets the following, all undoable through the settings undo
  path:
  - "Glyph atlas budget (MiB)" (R13);
  - "SDF primitives" (on);
  - "Transform-aware text" (on).
- **R26** The Game view stats overlay (`GameWindow.cpp:563`) shows draws, merged runs, instances,
  stencil draws and atlas MiB. `HtmlUIInspector.cpp` shows per text element "raster: bitmap ×1.37
  / msdf / stretched (settling)" and per box "primitive / mesh (reason)".

### Agent UX (MCP, Lua, CLI)
- **R27** `inspect_ui` gains `render_stats: boolean`, which returns the document's
  `HtmlUIRenderStats` (all fields of §6). With `detail: full`, each element gets `paint: {path:
  "primitive"|"mesh", reason}` and text gets `raster: {mode, scale}`. S15's generic render-stats
  tool includes the same struct.
- **R28** Lua: `UIDocument:getRenderStats()` returns the same fields as a table, for in-game
  profiling HUDs. No other Lua API: CSS `text-rendering` is the author control.
- **R29** `jasperengine-editor benchmark` gains `--ui-stats`, which adds the averaged HTML UI
  stats to its report (`editor/App.cpp:3101` loop).

### Data & compatibility
- **R30** New project fields `htmlUiGlyphAtlasMiB` (0 = platform default), `htmlUiSdfPrimitives`
  (true) and `htmlUiTransformAwareText` (true) get defaults when missing (`editor/Stream.cpp`, like
  `htmlUiMsaaSamples` at `:2556`). Old projects load unchanged and need no scene version bump. The
  fields go into exported players through `Generator.cpp` (like `:2224`).
- **R31** The fork patches S16-F1..F3 are recorded in `docs/html-ui/FORK.md` with tests. Each new
  `RenderInterface`/`FontEngineInterface` method has a default that keeps the old behaviour, so
  RmlUi's own tests (426) and the `RecordingRenderInterface` harness pass unchanged.

## 6. Design constraints & decisions

- **D1 Three stacked layers, each behind a test toggle and a project setting.**
  - `RmlRenderer::setBatchingForTesting`;
  - `setSdfPrimitivesForTesting` / `htmlUiSdfPrimitives`;
  - `RmlFontEngine::setTransformAwareTextForTesting` / `htmlUiTransformAwareText`.

  With all three off the output is bit-identical to today. That is the safety net and the
  reference for every equivalence test. Batching is replay-only: immediate mode
  (`setCommandRecordingForTesting(false)`) stays unbatched.
- **D2 Batching is CPU gathering, not a vertex-format change.** Translation differs per geometry
  and SSBOs are banned, so the CPU bakes it.
  - The run cache (R3) makes the steady animated state cheap.
  - Persistent pages stay for large and unbatched geometry.
  - The gather target is a STREAM buffer, reused like `RmlTransientBuffer`.
- **D3 Text and boxes share one pipeline only as instances.** `HTMLUI_PRIM` QUAD mode covers glyph
  quads, so a menu is "one instanced draw per atlas page per clip/transform region".
  - Images break runs on a texture change (UICompatDrawRuns rule).
  - No texture arrays: WebGL2 has them, but sokol image-type switching per page would force every
    texture to be an array.
- **D4 Shadows are analytic.**
  - Outer: erf-integrated Gaussian of the rounded-rect SDF (OpenEngine `evaluateShadowAlpha`,
    sigma = blur/2).
  - Inset: `1 − outer(shrunken shape)`, clipped to the padding box.

  Corners of large blurs differ slightly from a true 2D Gaussian. That is accepted, gated by the
  browser goldens (§7 AC2). If a browser fixture fails, that shadow class (inset, or blur > X)
  falls back to the texture path. The threshold X becomes a constant in
  `GeometryBoxShadow.cpp`, set by the fixtures; the default is no limit.
- **D5 Bitmap stays the default for static text.** stb coverage is what the text goldens and the
  Chromium comparisons were calibrated on. MSDF is only for motion, size, 3D and
  `geometricPrecision`. This improves on Chromium, which shows stretched bitmaps during composited
  scale animations and re-rasters at the end.
- **D6 Effect parameters ride on texture-handle variants (R17).** No vertex-format change. The
  batch key includes the variant.
- **D7 MSAA default stays 4.** The suite also runs at 1 sample with SDF on, as a report (§7 AC14).
  If a later change lowers the default (about 60 MiB saved per 1080p context), that is a separate
  decision with its own golden pass.

**API sketch**

```cpp
// engine/core/htmlui/HtmlUIRenderStats.h (added fields)
uint32_t geometrySubmitted = 0;   // RenderGeometry + RenderPrimitives calls (old drawCalls meaning)
uint32_t runsMerged = 0, runCacheHits = 0, instancesDrawn = 0, verticesGathered = 0;
uint32_t stencilDraws = 0, stencilClipsElided = 0, analyticClips = 0, boxShadowTextures = 0;
uint32_t glyphsRasterized = 0, msdfGlyphsPending = 0, textRegenerations = 0, textEffectFallbacks = 0;
uint32_t atlasBytes = 0, atlasEvictions = 0;

// engine/libs/rmlui Include/RmlUi/Core/RenderInterface.h (fork S16-F1/F2)
struct BoxPrimitive { Rectanglef border_box; CornerSizes2 radii; Vector4f border_widths;
    ColourbPremultiplied fill; Array<ColourbPremultiplied,4> border_colors;
    enum class Kind : uint8_t { Box, ShadowOuter, ShadowInset } kind; float sigma, spread; Vector2f offset; };
virtual bool SupportsPrimitives() { return false; }
virtual void RenderPrimitives(Span<const BoxPrimitive> primitives, Vector2f translation) {}
struct ClipShape { Rectanglef rect; CornerSizes2 radii; const Matrix4f* transform; };
virtual void RenderToClipMaskShape(ClipMaskOperation op, CompiledGeometryHandle g, Vector2f t, const ClipShape&)
    { RenderToClipMask(op, g, t); }
// FontEngineInterface (fork S16-F3)
virtual bool SupportsGpuTextShadow(FontFaceHandle, float blur_radius) { return false; }
// TextShapingContext: float raster_scale = 0 (0 = the render manager's); float shadow_blur = 0
```

New command type `PRIMITIVES` (pool index + count into a `commandPrimitives` side pool).
`Command` stays ≤ 48 bytes.

## 7. Acceptance criteria

Benchmark suite: a new gtest `HtmlUIRenderPerf` (`engine/tests/htmlui/RenderPerfTest.cpp`,
Release-only like `EffectPerfTest`). Its documents are in `engine/tests/htmlui/fixtures/bench/`:

- **B1**: the 10 `editor/util/HtmlUiExamples` documents (goldens `golden/examples/*`).
- **B2** `list-500.html`: 500 cards with `border-radius: 6px`, a 1px border,
  `box-shadow: 0 2px 6px`, a 24px icon and two labels, in a rounded `overflow: auto` container.
  One card's background is animated.
- **B3** `text-wall.html`: 5k glyphs at 12, 16 and 24 px.
- **B4** `scale-popin.html`: 40 buttons with `scale(0.5→1)` over 300 ms, and a
  `rotateY(30deg) perspective` card.
- **B5** `text-effects.html`: 50 labels with `text-shadow: 0 1px 2px`, `font-effect: outline(2px)`
  and `glow`.
- **B6**: B2 as a 1024² TEXTURE-mode document.

Baselines (all toggles off) are measured in the same run. Results go to
`build/<platform>/htmlui-render-bench.json`, and the before/after table goes to
`docs/html-ui/render-benchmark.md`. `tools/htmlui-bench` stays the SVG benchmark; its README gets
one line pointing here.

| AC | Criterion | Verification |
|---|---|---|
| AC1 | All 661 goldens pass with all toggles off, unchanged files | full `engine/tests/htmlui` run with `JASPER_HTMLUI_S16_OFF=1` |
| AC2 | With defaults on, every `golden/browser/**` fixture passes the strict metric. These files are Chromium output and are never regenerated by this work. `known-failures.txt` stays empty | `BrowserParityTest` |
| AC3 | Self-goldens may be updated only with a ledger row in `docs/html-ui/goldens-S16.md`: name, metric before/after, category (`sdf-aa`, `analytic-clip`, `analytic-shadow`, `exact-scale-text`, `gpu-text-effect`), a reason, and for fixtures that `render-reference.mjs` can render, mean diff vs Chromium new ≤ old. ≤ 60 updates without a separate review. Updated files are touched in their own commit | reviewer check, CI script `tools/htmlui/check_golden_ledger.py` (new: every changed golden PNG has a ledger row) |
| AC4 | Batching equivalence: max channel diff ≤ 1 between batching on and off | `RenderBatch.EquivalenceAllGoldenScenes` |
| AC5 | Run merging, white-texel join, run cache hit, no merge across a layer, filter, gradient or blend change; cache invalidated on one translation change | `RenderBatch.*` unit tests on `RecordingRenderInterface` + `TestRenderRecord.cpp` |
| AC6 | The draw-count tests are rewritten. `TestRenderRenderer.cpp:390,406` assert `geometrySubmitted == 5000` and `drawCalls <= 3`. `TestRenderGolden.cpp:280`, `TestRenderP1Golden.cpp:542` and `TestIntegrationP1.cpp:756` assert on `geometrySubmitted` | those tests |
| AC7 | Stencil elision and analytic clip: B2 has `stencilDraws == 0`; a nested rounded clip ×3 still uses stencil and matches its golden; analytic clip at 1 sample shows antialiased corners (edge pixels strictly between in/out colours) | `RenderClip.*` + golden `p1/rounded_clip_{1,4}x` (new) |
| AC8 | Primitives: border widths 0/1/3/7 per side, elliptical radii, outline `auto`, outer/inset/spread/multiple shadows, under `rotate`, `scale`, `rotateY+perspective`, inside `filter: blur`, `mask-image` and `opacity` groups, at 1/4/8 samples; each matches the mesh path within the default compare options | `RenderPrimitive.*` (renders both paths, compares) |
| AC9 | R21 draw budgets, R22 CPU budgets and R23 memory budgets on the Linux CI runner (GL core and Vulkan), with a slack factor as `CompatPerf` | `HtmlUIRenderPerf.B1…B6` |
| AC10 | Sharpness: acutance (mean Sobel magnitude over the ink box) of a `scale(2)` label ≥ 95 % of the same text at `font-size × 2`. It is ≤ 60 % with the toggle off, which proves the test discriminates. Animating mid-frames of B4 with MSDF ≥ 85 %. Pixel ratio 1.37 static text ≥ 95 % of pixel ratio 1.0 | `TextSharpness.*` (new helper in `TestGoldenCommon.h`) |
| AC11 | Hysteresis: B4 ends with exactly one regeneration per label after settle; the context renders the settle frame without input; ≤ 256 rasterizations in every frame | `TextSharpness.SettleRegeneratesOnce` |
| AC12 | Atlas: R8 pages (`getAtlasBytes()` = pages × 1 MiB); with a 2 MiB budget, 4 pages' worth of glyphs cycled across documents → evictions happen, no glyph missing in the drawn frame, only the users' handles bumped; diagnostic logged once | `TestFontEngine.AtlasBudget*` |
| AC13 | GPU text effects: B5 `layersPushed == 0`; `TestGoldenFontEffects.FontEffects` at pixel ratio 2 is sharper (acutance ≥ old) and within ledger; an effect beyond 0.3 em uses the fallback and counts it; `p3/text/text-shadow-{hard,blur-glow,multiple}` pass AC2 | `TestFontEffects.*`, `BrowserParityTest` |
| AC14 | 1-sample report: the suite at `samples=1` with SDF on is printed (pass/fail per golden), not gated | CI log artifact |
| AC15 | World-space: `TestTextureMode.*`, `harness_component_ui_texture` golden, `UIWorldRaycast` tests pass; B6 meets the B2 budgets | existing tests + `HtmlUIRenderPerf.B6` |
| AC16 | Web/mobile: the htmlui test subset (goldens included) passes in the S02 browser (WebGL2) gate; `HTMLUI_PRIM` compiles for all `engine/shaderlib` targets; B2 runs in the Wasm build with ≤ 16 draws | S02 web gate, shader build |
| AC17 | `text-rendering` support: `geometricPrecision` → MSDF at rest, `optimizeSpeed` → stretched bitmap under scale (today's acutance), support matrix regenerated, `check --ui` reports no "ignored" for it | `CompatTables.*`, `TextSharpness.TextRenderingProperty` |
| AC18 | MCP `inspect_ui {render_stats:true}` returns every field; `detail:full` shows `paint`/`raster`; Lua `getRenderStats()` matches | `tools/mcp/tests/test_ui_render_stats.py` (new), `TestScriptElement.RenderStats` |
| AC19 | Old project without the new fields loads with defaults; settings round-trip; export carries them | `EditorLogicTests` + S02 serialization test |
| AC20 | Fork: RmlUi's own tests (426) pass; FORK.md rows S16-F1..F3 with tests; `tools/smoke-test.sh` green | `tools/htmlui/run-rmlui-tests.sh`, smoke test |

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20);
  - S02 (gtest, GPU CI runners, web gate, perf slack);
  - S06 (background lane for MSDF generation). Before S06 lands, use R16's main-thread budget
    fallback; do not block on it.
- **Feeds:**
  - S15 (render stats, frame pass viewer: `HtmlUIRenderStats` fields; S15 owns the generic MCP
    render-stats tool);
  - S17 (virtualized lists benefit from R3's run cache).
- **Touches:** S05 (font hot reload R18), S03 (project fields; no scene version bump needed).
- **Merge hotspots:**
  - `engine/core/htmlui/RmlRenderer.{h,cpp}` (S15 adds timing scopes);
  - `RmlFontEngine.{h,cpp}`;
  - `HtmlUIContextRender.cpp`;
  - fork `ElementText.cpp`, `ElementBackgroundBorder.cpp`, `GeometryBoxShadow.cpp`,
    `ElementUtilities.cpp`, `RenderInterface.h` (S17 also patches the fork and compat tables);
  - `compat/tables/PropertyTable.cpp`;
  - `editor/mcp/tools/HtmlUiTools.cpp` (S17 adds tools);
  - `editor/Project.cpp`, `editor/Stream.cpp`, `editor/Generator.cpp` (every settings spec);
  - `NOTICE.md`.

  Land in this order: batching + clips → primitives → atlas R8/eviction → exact-scale text →
  MSDF + GPU effects. Each step is a separate PR with its own goldens ledger rows.

## 9. Risks & defaults

| Risk / question | Default decision |
|---|---|
| Analytic shadows differ from a true blurred shape at large blur radii on rounded corners, and browser fixtures fail | Gate on AC2. A failing class goes back to the texture path via a threshold constant (D4). Ship with the rest |
| SDF AA vs MSAA'd feathered meshes changes many self-goldens | Expected; ledger (AC3). Over 60 updates means stop and review the AA function (`sdfCoverage` box filter) before continuing |
| Gathering cost on low-end mobile CPUs | Run cache (R3) + 4096-vertex direct path; R22 gate on the Wasm build; batching can be turned off per project only through the test toggle, not a user setting |
| MSDF quality at small sizes (< 12 px) or thin serifs | MSDF is not used for static small text (D5); animating small text accepts MSDF artefacts for ≤ 300 ms |
| msdfgen speed (≈ 0.1–0.5 ms/glyph) | Background generation, size-independent cache, prewarm the glyphs of a string on its first scale change or `PrepareFontEffects` |
| Per-handle versions miss a regeneration | AC12 asserts no missing glyph; fallback: global epoch bump on any doubt (logged in Debug) |
| Effective scale is wrong under perspective (scale varies across the box) | Use the max singular value at the box centre; perspective text uses MSDF (R16) |
| Slug patent status | Not used. Revisit only with a written legal clearance; MSDF covers the need |
| Gamma/contrast tuning à la `text_mask_gamma.glsl` | Out of scope (would change every text golden); candidate follow-up |

## Deviations (implementation record)

- **Toggles (D1).** Batching (`RmlRenderer::setBatchingForTesting`) covers the merged runs, the run
  cache and the clip-mask elision; the analytic rounded clip (R6) is part of the SDF primitives
  toggle (`setSdfPrimitivesForTesting` / `htmlUiSdfPrimitives`) and applies to the unbatched
  replay too (`RmlRenderer::replayScoped`). Elision draws exactly what the mask would, analytic
  clips antialias the curve differently from a stencil mask: with the analytic clip under the
  batching toggle, R7's "max channel difference <= 1 between batching on and off" could not hold
  on any rounded clip. The project settings call these setters (`UIDocumentSystem::
  setHtmlUiRenderOptions`), the names stay D1's. GPU text effects and distance text (R16
  `geometricPrecision`, R17) are part of the text layer: off with transform-aware text. `JASPER_HTMLUI_S16_OFF=1` turns all three off
  for a whole `jasperengine-htmlui-tests` run (AC1).
- **No `MSDF_GLYPH` instance mode.** Distance glyphs are `QUAD` instances (or meshes, unbatched)
  whose texture is an MTSDF page: the texture's kind (`HtmlUITexture::Sample`) and its effect
  variant's parameters (D6) go into `u_fs_htmluiClip` per draw (`params.y`, `text`), which both
  `htmlui.frag` and `htmlui_prim.frag` decode, so distance text also draws with batching off.
- **Distance range.** "12 px (0.3 em)" is read as 12 atlas texels of distance on each side of the
  outline (msdfgen's range of 24), so that an effect reach of 0.3 em is inside it as R17 says.
- **Atlas pages update in place.** A page that gets glyphs keeps its texture (a DYNAMIC image the
  renderer uploads again before the frame draws, `RmlRenderer::updateAtlasTextures`) instead of a
  new texture source: nothing is regenerated for a growing page. Per-handle versions (R13) change
  when a page a handle drew from is cleared (eviction), when the distance glyphs a handle waits for
  arrive, and with the epoch (reloads, scale and render-text resets). A render interface other than
  the engine's (RmlUi's own tests, `RecordingRenderInterface`) still gets a new source per growing
  page, bumping that page's users.
- **Budget split.** `htmlUiGlyphAtlasMiB` is the sum of both budgets, split 2:1 (bitmap : distance,
  as 16 + 8 and 8 + 4); the CPU font-effect pages (RGBA8, 4 MiB) count against the bitmap budget.
  A page cleared to make room keeps its index (parked, no texels, no texture) so glyph entries stay
  valid; the "unused in this frame and the last" rule reads the renderer's draws of the page's
  textures (`HtmlUITexture::drawFrame`) and the strings that used it.
- **Shadows (D4).** The analytic shadow integrates the Gaussian exactly over the rows between the
  corners (separable erf) and numerically (6 rows per curved band) over the corners, instead of
  `erf` of the signed distance: a true Gaussian of the rounded rectangle at every blur, so
  `GeometryBoxShadow::primitive_blur_limit` stays at "no limit". `BoxPrimitive` also carries
  `fill_insets` (the mesh path's one-pixel inset of the fill under opaque borders) beyond the API
  sketch.
- **Translate-only transforms** are folded into the members' translation (outside analytic clips),
  so `translateX(-50%)` centring and slides join the runs around them.
- **Hysteresis (R15).** "The scale changed" is a change above 1e-4 of the last render's on-screen
  scale; the first generation of a text is at its on-screen scale at once; text above 128 device px,
  under perspective or with `geometricPrecision` waits for its distance glyphs as a bitmap at its
  scale. The settle's glyphs are made within 256 glyphs and 2 ms a frame (`PrepareGlyphs`); the
  first generation is never held back.
- **Acutance (AC10)** is the mean Sobel gradient over the ink box's edge pixels (those at least a
  tenth of the box's largest gradient): over the whole box a sharp text twice the size has a lower
  mean (more pixels inside the strokes), which made the 1.37 pixel ratio comparison meaningless.
- **Tests on the GPU renderer.** `RenderBatch.*` drive `RmlRenderer` directly in a
  `RenderTestContext` (draw counts and images) rather than the `RecordingRenderInterface`, which
  does not batch. `TestFontEngine.AtlasBudget*` are GPU tests (`TestFontEngineAtlas.cpp`): the LRU
  rule counts rendered frames. `RenderBatch.EquivalenceAllGoldenScenes` compares every capture of
  the browser-parity fixtures (the golden scenes the documents define); the C++-drawn goldens'
  suites compare their own images.
- **B6** is `list-500` in its own 1024 x 1024 context (what a texture-mode document is: an
  `HtmlUIContext` of the texture's size), not through a scene's `UIDocumentComponent`.
- **textEffectFallbacks** counts a string generated with a CPU font effect and, per frame, each
  blurred `text-shadow` drawn through a layer.
- **S15's render-stats tool** (`get_render_stats`, merged after S16's work) carries the selected
  scene's `HtmlUIRenderStats` as `html_ui` (`forEachField`), the same fields as `inspect_ui`'s.
- **R21 B1 draws (reported, not gated).** After round 2's measured attempts (four texture slots
  per `HTMLUI_PRIM` draw; CSS background gradients as primitives without their mask layers,
  S16-F5) blank, inventory, pause_menu, settings and shop meet "<= 30 % of the baseline and <= 40";
  dialogue (14 for 12), hud (80 for 40), loading (34 for 17), main_menu (36 for 31), toasts (40
  for 24) and video_menu (14 for 9) do not. Their remaining draws, measured per example on the
  benchmark host (`docs/html-ui/render-benchmark.md`, `RmlRenderer::getLastEffectCounts` and
  `getLastRunBreaks`), are effect passes S16 does not change: backdrop-filter and filter blurs
  (several passes each), `text-shadow` blurs beyond the distance glyphs' reach (a layer and a
  blur), opacity groups and their composites, and stencil masks under those layers (an analytic
  clip does not clip a composite); runs split by textures are gone (1 or 0 per example). Merging
  them would change what the browser draws (a blur of a group is not the group of blurs).
  `HtmlUIRenderPerf.B1Examples` prints the target per example, and gates "no more draws than the
  baseline" and the engine time (baseline + 5 %).
- **R22 B2 met.** "<= 50 % of the baseline" for the engine time: 2.477 -> 1.084 ms (44 %), B6 43 %,
  gated by `B2List500` and `B6TextureList500` (round 2: the paint traversal's shortcuts S16-F6,
  culling S16-F4, the benchmark's frames started with the GPU idle).
- **R22 gathering** counts an instance as the 4 vertices it replaces, on warm frames with the run
  cache dropped (`RmlRenderer::clearRunCacheForTesting`).
- **Wall frame times on a software rasterizer** (llvmpipe, the CI and benchmark hosts) are higher
  with S16: llvmpipe pays per instance (B3 text-wall: 128 ms instanced, 38 ms as a vertex run, 37 ms
  baseline). They are not a criterion (R22 excludes the backend, R24 needs GPU pass timings) and
  are reported in `docs/html-ui/render-benchmark.md`.
- **AC8 compare budget.** `RenderPrimitive.MatchesTheMeshPath` uses the default threshold and mean
  with up to 0.5 % bad pixels (default 0.05 %): at 1 and 4 samples the meshes' feathered edges,
  polygonal curves and aliased shadow textures differ from the exact coverage on edge pixels
  (largest measured: 0.29 %, rotated boxes at 1 sample). A sample count the device has no
  targets for (llvmpipe and lavapipe have 1 and 4) is reported and skipped: the HTML UI targets
  take the highest count the device has up to the asked one (`FramebufferRender::getMaxMsaaSamples`),
  since an 8-sample image on lavapipe is invalid Vulkan usage that no layer rejects (it drew into
  memory not its own, the whole-suite-only failures of round 2).
- **Atlas placement (R13).** Glyphs fill the newest page; with room in the budget then the others'
  space, then a new page; with the budget full a new page in place of the least recently used one
  (unused in this frame and the last). A glyph that finds every page used in this frame or the last
  waits one frame (its text is made again, `RmlFontEngine::GetVersion`) instead of being dropped;
  only when no page can ever free up is it reported as not fitting. Filling the newest page keeps a
  document's glyphs together, so the pages of text no longer drawn become free.
- **Shape quads and the scissor.** A box or shadow instance's quad, widened by two device pixels for
  its antialiasing, is cut to the draw's scissor when drawn in document px (no element transform):
  llvmpipe at 4 samples lets samples of a primitive through just outside the scissor's top and left
  edges, which the meshes (ending at the edge) never showed.
- **Glyph placement at fractional pixel ratios.** A glyph atlas draw's translation is rounded to
  whole target pixels (`RmlRenderer::snapTextTranslation`, no element transform), so bitmap text at
  a pixel ratio of 1.37 is drawn 1:1 on target pixels (AC10).
- **Primitive qualification and Chromium.** A border round a curve of under 3 px (the form
  controls' 2 px) keeps the mesh path: Chromium paints such a corner with less than its exact
  coverage, as the meshes' polygon does (`layout/button-display-*` fail AC2 with the exact
  coverage). The rounded-rectangle distance applies every corner that reaches a point (a radius
  may pass the middle of a side: `border-radius: 20px / 50px` on a 60 px box), and an outer shadow
  is cut out half a pixel inside the box's edge, so the box antialiases against the shadow rather
  than against its hole (no lighter seam; `paint/box-shadow-radius-percent`).
- **AC4 ties.** `RenderBatch.EquivalenceAllGoldenScenes` holds max channel difference <= 1 on every
  capture but two, which it lists with their reason and bound: `paint/object-position`, where a
  nearest-filtered texel boundary falls exactly on pixel centres (the folded translation and the
  shader's round the tie differently; up to 120 pixels), and `integration/fixed-modal-backdrop`,
  where the `translate(-50%, -50%)` modal's 30 px shadow tail under a backdrop blur differs by up to
  3.
- **AC1 `p1/texture_effects` traced (round 2).** With `JASPER_HTMLUI_S16_OFF=1` it was 48 pixels
  over the threshold (limit 32). Two causes, measured against oe-port's own render of the test
  (scratch worktree, `JASPER_HTMLUI_WRITE_ACTUAL=1`): (1) oe-port 50c7afd3d alone renders 30 of
  them, on the stencil-clipped disc's edge, and passes (30 <= 32): its golden (2026-10-11) is older
  than that rendering; (2) S16 added 18 on the outlined label in the rotated, filtered panel: the
  white texel block S16 reserves at the start of every glyph page (batching: untextured geometry
  joins a glyph page's run) moved the glyphs in the atlas, and a transformed glyph samples its
  atlas neighbours. The block now sits in the page's last rows, outside the shelf packing
  (`GlyphAtlas::reserveBottom`), so glyphs are packed where they were before S16 whether the frame
  batches or not (first fix, the block only while batching, made batched and unbatched differ on
  transformed text: `RenderBatch.EquivalenceAllGoldenScenes`, p3 `integration/sample-dialog`); the
  S16-off render was pixel-identical to oe-port's (0 pixels over the threshold between them). The other S16-off
  failures were S16's own tests needing S16 on (`S16On`), S16's new goldens, and goldens S16
  updated (the AC1 run uses the pre-S16 files).

## Acceptance round 2 (working notes)

- S17 merged (348362026): FORK.md keeps both sections; `inspect_ui` keeps `reload` and
  `render_stats`; TOOLS.md to be regenerated.
- B2 profile (perf on the benchmark host, `HtmlUIRenderPerf.ProfileTarget`): the main thread's
  time is spread over RmlUi's per-element paint (`Element::Update` 7 %, `Element::Render` 5 %,
  `GetClippingRegion` 5 %, `SetTransform`, `GetRenderBox`, `ElementText::OnRender`,
  `RenderPrimitives`/`toInstance`, `ElementBackgroundBorder::Render` about 3 % each; clip shapes'
  `ComputeBorderMetrics`/`ScaleRadiiToFit` per clipped element). 450 of the 500 cards are outside
  the list's scissor. Changes: S16-F4 culling, clip shapes cached.
- B1: four texture slots per `HTMLUI_PRIM` draw; the benchmark prints the effect draws (shaders,
  composites, filters, clip masks).
- Vulkan `RenderPrimitive.MatchesTheMeshPath` (whole suite only): root cause found. The test made
  layer targets of 8 samples; lavapipe (Mesa 23.2.1, the test image's Vulkan device) has
  `framebufferColorSampleCounts` 1 and 4 only (`vulkaninfo` in the image). An image of 8 samples
  is invalid Vulkan usage that neither sokol nor lavapipe rejects (GL llvmpipe fails the
  framebuffer, which the test skipped); its draws landed in memory not the image's, so what the
  readback saw depended on the heap the earlier tests left: the failure images
  (`primitives-borders-8.mesh.png`, `-transforms-8.sdf.png`) are the right document shifted by a
  linear memory offset with foreign bytes in the first/last rows, on the mesh path as much as the
  primitive path. Fix (012d4cd61): `RmlRenderTarget::ensure` and `HtmlUIContext::getSampleCount`
  take `FramebufferRender::getMaxMsaaSamples` (the device's highest count up to the asked one, as
  the scene MSAA already did), so an editor preference of 8 on such a device is 4, not undefined;
  the comparison skips counts the device lacks; `RenderP1Test.SampleCountChoice` checks the clamp.
- B1 gradients: S16-F5 as first written covered RCSS `decorator:` gradients only; the examples'
  CSS `background` gradients are background-image layers (`ElementBackgroundLayers`), drawn as a
  shader mesh and, under a rounded clip box, a mask layer each. Those are primitives now (one per
  grid of tiles, the clip radii their own). The gradient instances also lacked their translation
  (`translateInstance`), unseen while no CSS gradient took the path.
- B2: S16-F6 keeps each offset parent's clipping region for the render pass (500 cards: 2000
  elements walked their ancestors and built a mask list each).
- B2 profile (perf, main thread, benchmark host): `Element::Update` 8.5 %, `Element::Render` 7.2 %,
  `GetClippingRegion` 4.8 %, `SetTransform` + `Matrix4::operator!=` 6 %, `ElementText::OnRender`
  3.4 %, `malloc`/mask-list `realloc_insert` 3 %. RmlUi 6.3 never cleared `dirty_transform`, so
  every element recomputed its transform each render. S16-F6: clip memo keyed by the first
  clipping offset parent, SetTransform(nullptr) fast path, `dirty_transform` cleared, and elements
  whose paint bounds lie outside the memo's scissor passed over before any state. The benchmark
  starts each frame with the GPU idle (`finishGpu`; llvmpipe's 4 threads otherwise share the 4
  cores with the main thread). B2 on E: 2.389 -> 1.064 ms (44.5 %), B6 2.370 -> 1.046 ms.
- B1 after the background-layer gradients (E): dialogue 43->14, hud 155->80, inventory 46->6,
  loading 57->34, main_menu 105->36, pause_menu 42->2, settings 97->17, shop 26->2, toasts
  82->40, video_menu 32->14; engine time below the baseline in 9 of 11. Met: blank, inventory,
  pause_menu, settings, shop. The rest are effect passes (backdrop/filter blurs, text-shadow
  blurs beyond the MSDF reach, opacity layers and their composites, stencil masks under layers).
- AC1 `p1/texture_effects` (S16 off, against oe-port's golden of 2026-10-11): oe-port 50c7afd3d
  alone already renders 30 pixels over the threshold on the circle's clip edge (it passes: the
  budget is 32), and S16 off adds 18 on the outlined text in the rotated, filtered panel (the
  glyphs' top rows): 48. Experiments that changed nothing: clip shapes off (X1), the primitive
  program not awaited (X2), the render request (X4). Bisect of oe-port's first-parent merges for
  the 30 running in a scratch worktree; S16's 18 under investigation (glyph placement in the atlas).
- Suites at 592d8ed28 (server B, `JASPER_HTMLUI_PERF_SLACK=3`): glcore 3379 ran, 0 failed;
  Vulkan twice, whole suite, 3379 ran, 0 failed each (`MatchesTheMeshPath` skips 8 samples:
  lavapipe has none). An earlier Vulkan crash (`InputGamepadTest.SelectOpensAndNavigates`) came
  from a build whose sources were re-synced mid-build (RenderManager's layout changed between
  objects); it did not recur in four runs, ASan (glcore) reports no error there.
- AC1 run (S16 off, pre-S16 goldens): `RenderClip.RoundedClipIsAnalyticAndAntialiased` compares
  S16's own goldens, now under `S16On`; `s17/views_scrolled` needs oe-port's golden in that run;
  `HtmlUIWidgetsPerf.ModelSetAndTreeFlatten` (S17) once took 12 ms on the loaded host (1.6 ms in
  the next run).
- Merged oe-port e1ca0c71a (upstream round 1, 432a9be1f). Gate re-running on the merged tree
  (htmlui suites on B, the rest on C, timings on E). MCP failures of the earlier gate that were
  not S16's are fixed on BF2's branch (S04 asset type, S17 patch reload, the "Loading" dialog).
- AC1 golden drift: oe-port's own render of `p1/texture_effects` matches its golden at the
  golden's commit 723cbba62 and is 30 pixels off from the phase-3 merge 08ec10485 on (bisect of
  the phase-3 branch between them in progress).
- AC1 drift bisect (stopped: its steps starved on the shared host): oe-port's `p1/texture_effects`
  render matches its golden at 723cbba62 and is off by the 30 disc-edge pixels at every later
  commit tested; the change lies in html-ui phase 3's p3-text branch, between e812f9753 ("rmlui:
  Unicode line breaking and text-transform") and 31c67f7de ("rmlui: native text-shadow") (10
  commits, two of which did not build in the bisect), merged into oe-port by 08ec10485 before any
  S stream. It stays within oe-port's budget (30 <= 32); S16's own share was the 18 text pixels.
- Merged oe-port 05eb8d3b4 (BF2 fixes, S25, c67bbdc8a): S16's three HTML UI project settings are
  rows of S25's settings registry (`SettingsRows.cpp`, search entries; the Project Settings rows
  behind `settingsui::beginRow`; `mcp-keys-before-s25.txt` lists them). S16 adds no editor
  shortcut or menu.
- On the tree with oe-port e1ca0c71a and the white-block fix (server A): glcore 3478 ran, the only
  failure S17's `MarkdownHtml.Perf20KB` timing on the loaded host; AC1 (S16 off, pre-S16
  goldens) 3478 ran, the same single timing failure: `p1/texture_effects` passes. Gate on B
  (e1ca0c71a): `WERROR=1 linux`, TOOLS.md (198 tools), htmlui-editor-tests, unit tests, RmlUi
  tests (463), web + web-gate pass; MCP failures there were the ones BF2 fixed (merged since).
- Benchmark host E was replaced (a new rented box, cold builds): the earlier E runs (S16 alone and
  e1ca0c71a) are recorded; the run on the final tree goes to the new E.
- Vulkan, whole suite, twice on the tree with e1ca0c71a and the white-block fix (server A):
  3478 ran, 0 failed, both runs. Gate on B: android, smoke (glcore and Vulkan) pass.
- MCP on c67bbdc8a (C): the remaining S16 failures were run-to-run differences:
  `test_html_ui_effects::test_determinism_and_live_clock` (the pinned effect time rendered twice)
  and `test_html_ui_examples::test_main_menu_play_golden` (two fixed_fps runs). Text depended on wall
  time: distance glyphs come from background jobs and the settle budget counted milliseconds.
  Under a deterministic clock (fixed_fps, a fixed UI or effect time) distance glyphs are now waited
  for and the budget counts glyphs only; a pixel-ratio change (resized view, screenshot size) draws
  the bitmap at once (092ec068f). Editor unit tests there: 822 ran, 0 failed; htmlui-editor 76.
- Merged oe-port 545618e38 (S14, 11ee1a967); final gate running on it (htmlui suites on B, the
  rest on B, timings on E).
- On 11ee1a967: glcore, AC1 and Vulkan twice 3478 ran, 0 failed; benchmark (E) 9 passed.
- Merged oe-port 697d661e2 (S28, 0ef7ff94f) and 64d735104 (S20, 14dcdfed6): `PARTICLES` keeps
  its upstream `ShaderType` value and `HTMLUI_PRIM` follows it (shader cache v42); `TOOLS.md`
  regenerated by `gen_tool_docs.py` matches the merged one (225 tools).
- `test_html_ui_examples::test_main_menu_play_golden` still differed after 092ec068f, by 9 pixels
  at the top edge of the menu labels: alone (a fresh editor) the first play run differed from the
  later ones, which matched each other; S16-less oe-port runs pass. Between runs the glyph page
  was cleared and refilled (the atlas budget), and a glyph packed at a page's top or left edge
  read its own edge texels there under filtering (clamp to edge) where elsewhere it read the
  padding's 0. Under transform-aware text the pages now keep their first row and column empty
  (`GlyphAtlas::reserveLeading`, f6c0639f0); with it off the glyphs are packed as before S16 (AC1).
  `TestFontEngine.GlyphsDrawTheSameWhereverTheyArePacked` draws text at a fractional offset from
  glyphs at the top of a page and from the same glyphs packed lower: 60 apart without the change,
  equal with it. No golden changed (glcore 3480 ran, 0 failed).
- Merged oe-port 6f47d2c48 (S22, S14b, 7ac1b7ccd): no `ShaderType` or shader source changed
  upstream, the shader cache stays v42.
- `test_html_ui_phase2::test_mistakes_are_reported` (a black first Game view shot, 1 in 7 S16
  runs, none on oe-port): the MCP screenshot of a stopped scene waited for meshes, images and
  effect replays but not for its HTML documents, so it could shoot before a document's first
  frame. That frame waits for the HTML UI shaders, made when a document first draws; the failing
  run shot at 22:05:14, the HTML UI shaders (S16's `htmluiprim` with them) were made at 22:05:14,
  and the document's first frame then stalled 3.1 s in the software rasterizer (hang report,
  `UIDocumentSystem::renderOffscreen`). S16's extra program lengthens that wait; the gap is the
  screenshot's. The shot now also waits for `UIDocumentSystem::isAllLoaded` (documents loaded and
  drawn once), as play's loading and the editor's redraw do (d32267af6). Four copies of the test
  at once, five rounds, on C: 3 of 20 failed without the change, 0 of 20 with it.
- Merged oe-port 424925911 (S26, BF5, S08; 7dc3a625e): build.sh passes every side's environment
  names, the Xcode project keeps every object of both sides (and parses again, 89a19be88). S16
  adds no component field to S08's reflection tables; its reflection goldens take S16's project
  settings, generated render options and shader key (d7848f9e6); `inspect_ui` is 44 characters
  shorter so the default listing fits (6e2e283bc).
