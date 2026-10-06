# Phase 1: complete integration

Binding inputs: `DESIGN.md` (architecture, names, research facts) and `BRIEF.md` (rules for
implementing agents). This spec assumes everything phase 0 delivers (see
`phase-0-foundation.md`): RmlUi 6.3 at `engine/libs/rmlui`, the backends in
`engine/core/htmlui/` (`RmlSystem`, `RmlFile`, `RmlRenderer` with the required
`RenderInterface` methods and `SetTransform`, `RmlFontEngine` on `STBText`,
`HtmlUIContext`), `UIDocumentComponent` + `UIDocumentSystem` + the `UIDocument` wrapper with
basic pointer input, minimal Catalog/Stream registration, the `jasperengine-htmlui-tests`
googletest target with its golden-image harness, and the SVG benchmark decision (default:
ThorVG for SVG and Lottie).

All file:line references below were checked against the `html-ui` branch before phase 0
landed. Phase 0 adds code next to several of them; implementers locate a hook by the code
quoted here, not by the bare number.

---

## 1. Outcome

When phase 1 is merged:

1. **The renderer is complete.** `RmlRenderer` implements every optional RmlUi 6.3 render
   feature on the engine render abstraction:
   - stencil clip masks, including nested clips and clips under transforms;
   - render layers (`PushLayer` / `CompositeLayers` / `PopLayer`);
   - `SaveLayerAsTexture` (used by `box-shadow`) and `SaveLayerAsMaskImage` (used by
     `mask-image`);
   - filters: opacity, blur, drop-shadow and the colour-matrix family, for both `filter`
     and `backdrop-filter`;
   - gradients: linear, radial and conic, plain and repeating;
   - shader decorators, through a registry that phase 5 fills.

   The UI target is MSAA (4x where the device supports it). The same code runs on GL core,
   GLES3/WebGL2, D3D11, Metal and Vulkan. Pipelines are cached by state, geometry buffers
   and render targets are pooled, and draw calls, passes and pipeline switches are counted
   every frame.
2. **Input is complete.** Documents receive:
   - mouse buttons 1–3, wheel (vertical and horizontal) and enter/leave;
   - key down/up with modifiers, mapped to `Rml::Input::KeyIdentifier`;
   - UTF-8 text input (UTF-16 surrogate pairs are combined);
   - IME composition where the platform allows it;
   - multi-touch with pointer ids.

   Documents get keys and text before the game. A consumed event never reaches the game's
   callbacks while `ignoreEventsHandledByUI` is set. Focusing a text field opens the
   virtual keyboard. `System` gains a clipboard API, implemented on Linux X11, Windows,
   macOS, iOS, Android and Web, and in the editor.
3. **The editor is fully integrated:**
   - Inspector, Add Component, Hierarchy *Create > UI > UI Document*, YAML round-trip,
     exported-game C++ generation, copy/paste/undo/prefab overrides;
   - Project window file types and icons for `.html`/`.css`, and HTML/CSS syntax
     highlighting in the code editor;
   - an export check that every referenced asset exists inside the project;
   - documents visible in the Scene view of UI and 2D scenes in edit mode;
   - hot reload of a document and its dependencies, in edit mode and play mode, keeping
     scroll, focus, form values and event listeners;
   - a toggle for the RmlUi debugger.
4. **Scripting.** Lua and C++ get a browser-named element API (`UIDocument`, `UIElement`,
   `UIEvent`) and data binding (`UIDataModel`) driven from Lua tables or C++ `UIValue`s, with
   two-way binding, `data-for` arrays and `data-event-*`. Element handles are safe after
   their document unloads. LuaLS stubs and API suggestions cover every new class.
5. **Vector graphics run on ThorVG.**
   - SVG (`<svg src>`, inline `<svg>`, `decorator: svg(...)`, tinting) is rasterised by
     ThorVG.
   - A new `<lottie>` element, also on ThorVG, supports `.json` and `.lottie`, autoplay,
     loop, speed, direction, segments and markers, and has a control API. Each element
     updates one reusable RGBA8 texture, and only when the frame changes.
   - `TextureData` loads `.svg` textures through ThorVG; nanosvg is removed.
6. **Animation.** A global Lua table `anime` mirrors Anime.js v4: `animate`, `stagger`,
   `createTimeline`, `createTimer`, `createSpring`, easing names and functions, callbacks,
   playback control and `utils`. It animates element styles (individual transforms, colours,
   lengths) without forcing layout for transform and opacity, and animates plain Lua tables.
7. **Render to texture.** A document in `TEXTURE` mode renders into a `Framebuffer` that
   materials, `Mesh` and `Image` can use. When that texture is shown on a mesh, pointer
   input reaches the document through a ray → triangle → UV hit test.
8. **Font effects.** `font-effect: shadow / outline / glow / blur` render through
   `RmlFontEngine`.

---

## 2. Scope

### In scope (all implemented in this phase)
Every item of §1, with the tests of §5. Two RmlUi fork patches are part of phase 1, because
phase-1 APIs need them; both are recorded in `docs/html-ui/FORK.md`:
- `Event::PreventDefault`, for the Lua `preventDefault()`;
- the SVG plugin rasteriser switched from LunaSVG to ThorVG.

### Belongs to phases 2–5 (do not implement here)
| Phase | Owns |
|---|---|
| 2 | Parsing standard HTML5 with lexbor and converting it to RML; CSS → RCSS rewriting; user-agent stylesheet; load-time diagnostics with suggestions; importers (Google Fonts, LottieFiles, Lucide); `currentColor` icons; web-component mappings. Phase-1 documents and tests are written in what RmlUi 6.3 accepts: XML-valid markup, RCSS properties such as `decorator: linear-gradient(...)`, `font-effect`, and `image-color`. |
| 3 | RmlUi patches for CSS features: `calc()` and friends, grid, border styles, `%` radii, `::before/::after`, `:root/:is/:where`, real `position: fixed`, group `opacity`, `cubic-bezier()`/`steps()` in transitions, `!important`, `inherit`/`initial`/`unset`. |
| 4 | `jasperengine-editor check --ui`, MCP inspect/assert tools, hot reload through `McpAutoSync`, the generated support matrix, the guide example library, and rewriting references inside HTML/CSS when an asset is renamed. Phase 1 only remaps the component's own `path`. |
| 5 | The effects library on the shader-decorator registry. Phase 1 ships the registry and one built-in test effect. |

### Excluded, with reasons
- **Multi-touch on Linux X11.** The X11 layer uses core X11 only, and touch would need
  XInput2 (`libXi`), a new dependency for a platform without touch targets. Linux keeps
  mouse input.
- **Showing the composition inside the document on Linux and in the editor's Game view.**
  - Linux: with `XIMPreeditNothing`/`XIMPreeditPosition` the input method draws the preedit
    itself; the document receives the committed text and positions the IM window.
  - Editor Game view: ImGui exposes no composition events, so committed text arrives as
    characters.
- **Lottie expressions.** They need ThorVG's JerryScript, and DESIGN forbids a JS engine.
  Lottie audio layers are excluded too.
- **UV hits on skinned or morph-target meshes.** The hit test uses the CPU bind pose. The
  user guide documents this.

---

## 3. Detailed design

### 3.0 Conventions and wave-0 contract files

Several packages extend the same classes. So that they can work in parallel, the
coordinator commits the files of this section to `html-ui` **before** any work package
branches (wave 0). These files hold declarations, and two small implementations other
packages call: the RGBA8 dynamic texture and the `HtmlUITexture` struct. Each package then
**defines** its own declarations in files it owns. A declared but undefined non-virtual
member compiles and links as long as nobody calls it, so every worktree builds from the
start.

**Per-package state, no shared private members.** `UIDocumentSystem` gets one opaque state
object per package:

```cpp
// engine/core/subsystem/UIDocumentSystem.h (wave 0 additions, private section)
struct HtmlUIRenderState;   // p1-render  (UIDocumentSystemRender.cpp)
struct HtmlUIInputState;    // p1-input   (UIDocumentSystemInput.cpp)
struct HtmlUIScriptState;   // p1-lua     (UIDocumentSystemScript.cpp)
struct HtmlUIReloadState;   // p1-editor  (UIDocumentSystemReload.cpp)
struct HtmlUITextureState;  // p1-rtt     (UIDocumentSystemTexture.cpp)
std::shared_ptr<HtmlUIRenderState>  renderState;   // shared_ptr: deleter bound at creation,
std::shared_ptr<HtmlUIInputState>   inputState;    // so ~UIDocumentSystem (phase-0 .cpp)
std::shared_ptr<HtmlUIScriptState>  scriptState;   // never needs the complete types
std::shared_ptr<HtmlUIReloadState>  reloadState;
std::shared_ptr<HtmlUITextureState> textureState;
```

Each package creates its state lazily in its own `.cpp` (`if (!inputState) inputState =
std::make_shared<HtmlUIInputState>();`) and defines the struct there. The phase-0
`UIDocumentSystem.cpp` changes only by the one-line calls listed per package.

**Public C++ API without RmlUi types.** Headers outside `engine/core/htmlui/` and outside
`*Internal.h` files never include RmlUi headers, because DESIGN principle 3 keeps RmlUi
swappable. Handles hold a `std::shared_ptr` to an internal struct defined in a `.cpp` or
`*Internal.h`.

Wave-0 files, with the exact contents the coordinator commits:

1. **`engine/core/htmlui/HtmlUITexture.h`** — what every `Rml::TextureHandle` points to in
   phase 1:
   ```cpp
   namespace jasperengine {
   struct HtmlUITexture {
       enum class Kind : uint8_t { STATIC, DYNAMIC, RENDER_TARGET, EXTERNAL };
       Kind kind = Kind::STATIC;
       TextureRender* render = nullptr;            // sampled texture
       std::unique_ptr<TextureRender> ownedRender; // STATIC/DYNAMIC created by the UI code
       std::unique_ptr<Framebuffer> ownedTarget;   // RENDER_TARGET (SaveLayerAsTexture)
       int width = 0, height = 0;
       bool premultiplied = true;                  // false: the UI shader premultiplies
       uint64_t lastUpdateFrame = UINT64_MAX;      // DYNAMIC: one sg_update_image per frame
   };
   }
   ```
   Rules:
   - `RmlRenderer::ReleaseTexture` deletes the struct, which destroys whatever it owns.
   - `EXTERNAL` points at a texture owned by someone else, for example a `Framebuffer` a
     script passes in.
   - A phase-0 handle representation that differs is migrated to this struct by p1-render.
2. **`engine/core/render/TextureRender.h` / `engine/renders/sokol/SokolTexture.{h,cpp}`** —
   an RGBA8 dynamic texture. These are new overloads; the old RGBA32F path
   (`SokolTexture.cpp:331-366`, used for bones) is untouched:
   ```cpp
   bool createDynamicTexture(const std::string& label, int width, int height,
                             ColorFormat format, TextureFilter filter);   // RGBA → SG_PIXELFORMAT_RGBA8,
                                                                         // RED → R8; usage.stream_update,
                                                                         // CLAMP_TO_EDGE, given min/mag filter
   ```
   `updateTexture(const void*, size_t)` stays the update call (one update per frame, as
   sokol requires).
3. **`engine/core/component/UIDocumentComponent.h`** — the full phase-1 field list (§3.8.1).
   Fields that already exist from phase 0 keep their names; the new ones are appended.
4. **`engine/core/subsystem/UIDocumentSystem.h`** — the state pointers above, plus the
   public method declarations listed in §3.5, §3.6, §3.8.6, §3.9.5 and §3.12.
5. **`engine/core/htmlui/UIValue.h`, `UIElement.h`, `UIEvent.h`, `UIDataModel.h`** — the
   public API declarations of §3.9, verbatim.

The wave-0 commit must build (`tools/build.sh linux`) and keep all phase-0 tests green.

---

### 3.1 Render abstraction changes (`engine/core/render/*`, `engine/renders/sokol/*`)

Today, pipeline state is fixed per `PipelineType` in `SokolObject::makePipelineDesc`
(`SokolObject.cpp:265-399`):
- there is no stencil (`pipeline_desc.stencil` is never set);
- there is no premultiplied blend: RTT uses SRC_ALPHA/ONE_MINUS_SRC_ALPHA, and DEFAULT
  writes RGB only;
- sample count 1;
- there is no pipeline cache;
- offscreen depth is `SG_PIXELFORMAT_DEPTH` (`SokolTexture.cpp:475`);
- `SokolFramebuffer::get()` never fills `resolves[]` (`SokolFramebuffer.cpp:299-313`).

The vendored sokol (CHANGELOG 05-Jun-2026, view API) has **no dynamic stencil reference**:
`sg_stencil_state.ref` is baked into the pipeline on every backend (D3D11
`sokol_gfx.h:13756/14310`, Metal 15850/16366, Vulkan 21410/21417, GL 11806). Each clip
nesting level therefore needs its own pipeline, which is why a cache is required.

#### 3.1.1 `engine/core/render/PipelineState.h` (new, p1-render)
```cpp
namespace jasperengine {
enum class BlendMode : uint8_t { NONE, ALPHA, PREMULTIPLIED, ADDITIVE_PREMULTIPLIED };
enum class ColorWriteMask : uint8_t { NONE = 0, R = 1, G = 2, B = 4, A = 8, RGB = 7, RGBA = 15 };
enum class CompareFunc : uint8_t { NEVER, LESS, EQUAL, LESS_EQUAL, GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS };
enum class StencilOp : uint8_t { KEEP, ZERO, REPLACE, INCR_CLAMP, DECR_CLAMP, INVERT, INCR_WRAP, DECR_WRAP };
enum class DepthStencilFormat : uint8_t { NONE, DEPTH, DEPTH_STENCIL };
enum class IndexType : uint8_t { NONE, UINT16, UINT32 };

struct StencilState {
    bool enabled = false;
    CompareFunc compare = CompareFunc::ALWAYS;
    StencilOp failOp = StencilOp::KEEP, depthFailOp = StencilOp::KEEP, passOp = StencilOp::KEEP;
    uint8_t readMask = 0xFF, writeMask = 0xFF, ref = 0;     // same for front and back faces
};
struct RenderTargetFormat {
    bool swapchain = false;          // true: colour/depth/sample count from sg_environment defaults
    ColorFormat color = ColorFormat::RGBA;
    DepthStencilFormat depth = DepthStencilFormat::DEPTH_STENCIL;
    int sampleCount = 1;
};
struct PipelineState {
    RenderTargetFormat target;
    BlendMode blend = BlendMode::PREMULTIPLIED;
    ColorWriteMask writeMask = ColorWriteMask::RGBA;
    StencilState stencil;
    CompareFunc depthCompare = CompareFunc::ALWAYS;
    bool depthWrite = false;
    bool cullBack = false;
    uint64_t hash() const;           // FNV-1a over every field, in declaration order
    bool operator==(const PipelineState&) const;
};
// The formats PIP_DEFAULT / PIP_RTT / ... resolve to, so code drawing inside an existing
// camera pass (composite) can build a matching state
RenderTargetFormat targetFormatFor(PipelineType pipelineType);
}
```

The blend factors (rgb and alpha both, `SG_BLENDOP_ADD`):

| `BlendMode` | src | dst |
|---|---|---|
| `NONE` | blending off | — |
| `ALPHA` | SRC_ALPHA / ONE | ONE_MINUS_SRC_ALPHA |
| `PREMULTIPLIED` | ONE | ONE_MINUS_SRC_ALPHA |
| `ADDITIVE_PREMULTIPLIED` | ONE | ONE |

#### 3.1.2 `ObjectRender` / `SokolObject` (p1-render)
- `bool ObjectRender::endLoadDynamic(PrimitiveType, IndexType)` is new. It records the
  shader, vertex layout and index type into a stored `sg_pipeline_desc baseDesc` (new
  member of `SokolObject`) and creates no pipelines.
- `bool ObjectRender::beginDraw(const PipelineState&)` is a new overload. It fills a copy
  of `baseDesc` from the state, gets the pipeline from `SokolPipelineCache`, and calls
  `sg_apply_pipeline`, skipping the call when the pipeline equals the last one applied.
  The existing `beginDraw(PipelineType, bool)` (`ObjectRender.h:47`) is unchanged.
- Existing `endLoad(...)` users are untouched.
- While editing `SokolObject::beginLoad`, also zero `nodepth_pip`, `rtt_nodepth_pip`,
  `zprepass_pip` and `zprepass_rtt_pip`. Today `SokolObject.cpp:139-154` leaves them
  uninitialised.

#### 3.1.3 `engine/renders/sokol/SokolPipelineCache.{h,cpp}` (new, p1-render)
```cpp
class SokolPipelineCache {
public:
    static sg_pipeline get(const sg_pipeline_desc& base, const PipelineState& state); // creates on miss
    static void releaseShader(sg_shader shader);   // destroys every pipeline made with it
    static void clear();                           // SokolSystem shutdown
    static size_t size();                          // tests
};
```
- The key is `hash(shader.id, layout bytes, index_type, primitive_type) ^ state.hash()`, in
  an `std::unordered_map<uint64_t, Entry{ sg_pipeline, sg_shader }>`.
- `SokolShader::destroyShader` calls `releaseShader` first.
- The cache is main-thread only. Pipelines are never created on loader threads; UI drawing
  runs on the main thread.

#### 3.1.4 Framebuffers with stencil, MSAA and resolve (p1-render)
`FramebufferRender.h` gains:
```cpp
struct FramebufferDesc {
    int width = 0, height = 0;
    ColorFormat color = ColorFormat::RGBA;
    DepthStencilFormat depth = DepthStencilFormat::DEPTH_STENCIL;
    int sampleCount = 1;                    // > 1: MSAA colour + 1-sample resolve texture
    TextureFilter minFilter = TextureFilter::LINEAR, magFilter = TextureFilter::LINEAR;
    TextureWrap wrapU = TextureWrap::CLAMP_TO_EDGE, wrapV = TextureWrap::CLAMP_TO_EDGE;
    const FramebufferRender* shareDepthStencil = nullptr; // reuse its depth-stencil image
    std::string label;
};
bool createFramebuffer(const FramebufferDesc& desc);
int  getSampleCount() const;
```
`SokolFramebuffer` changes:
- **MSAA.** With `sampleCount > 1` it creates:
  - an MSAA colour image (`usage.color_attachment`, `sample_count = n`);
  - a 1-sample resolve image (`usage.resolve_attachment`), which is the image
    `getColorTexture()` returns;
  - an MSAA `DEPTH_STENCIL` image with the same sample count (or the shared one);
  - views for each.
- `get()` fills `resolves[0]` and `depth_stencil`.
- `createFramebufferTexture` (`SokolTexture.cpp:442-496`) accepts the sample count and the
  depth-stencil format. Today it has `sample_count = 1` at 472 and `DEPTH` at 475.
- The Vulkan `initializeAttachments` (`SokolFramebuffer.cpp:167-190`) also covers the
  resolve and MSAA images.

`CameraRender` / `SokolCamera` gain:
```cpp
enum class LoadAction : uint8_t { CLEAR, LOAD, DONTCARE };
enum class StoreAction : uint8_t { STORE, DONTCARE };
void setColorActions(LoadAction load, StoreAction store, Vector4 clearColor = Vector4(0, 0, 0, 0));
void setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth = 1.0f, uint8_t clearStencil = 0);
void applyScissorTopLeft(int x, int y, int w, int h);   // sg_apply_scissor_rect(..., origin_top_left=sg_query_features().origin_top_left)
void applyViewportTopLeft(int x, int y, int w, int h);
```
`setDepthStencilActions` always writes the same load and store action to **both**
`pass.action.depth` and `pass.action.stencil`. Metal reads the stencil store action from
`action->depth.store_action` (`sokol_gfx.h:16156`), so a separate stencil store would be
lost there.

**Sample count choice.** `HtmlUIContext::getSampleCount()` is decided once when the GPU is
ready, in this order:
1. If the override `HtmlUIContext::setPreferredSampleCount(int)` is set (default 4; the
   editor setting is in §3.8.8), use it, clamped to 1/2/4/8.
2. Otherwise use 4 if `sg_query_pixelformat(SG_PIXELFORMAT_RGBA8).msaa` and
   `sg_query_pixelformat(SG_PIXELFORMAT_DEPTH_STENCIL).msaa` are both true.
3. Otherwise use 1.

No engine code calls `sg_query_features`/`sg_query_pixelformat` today; these are the first
calls.

#### 3.1.5 Other render additions (p1-render)
- `TextureRender::createRenderTargetTexture(...)` is **not** added. Render-target textures
  come from `Framebuffer`/`FramebufferRender` with a `FramebufferDesc`.
- `SystemRender::getFeatures()` returns a POD with `originTopLeft`, `msaaRGBA8` and
  `msaaDepthStencil`, cached on the first call.

---

### 3.2 RmlRenderer completeness (`engine/core/htmlui/RmlRenderer.{h,cpp}`, p1-render)

Port the semantics of `/tmp/htmlui-src/rmlui-6.3/Backends/RmlUi_Renderer_GL3.cpp`. Its
constants:
- `MAX_NUM_STOPS 16`;
- `BLUR_SIZE 7`, `BLUR_NUM_WEIGHTS 4`;
- `SigmaToParameters`: max 10 passes, max single-pass sigma 3.0;
- the colour-matrix formulas at lines 1493-1603;
- the gradient parameter mapping at 1625-1689;
- the blur algorithm at 1351-1462;
- the layer stack at 2043-2130;
- the filter composition at 1760-1884.

Several things are different on sokol; they are described below.

#### 3.2.1 Coordinate convention
All UI images follow the engine's render-to-texture convention: **top-left origin on every
backend** (`RenderSystem::isRenderingFlipped`, `RenderSystem.cpp:7764-7776`).
- UI geometry is projected with `ortho(0, w, h, 0)` on top-left-origin backends and with
  `ortho(0, w, 0, h)` on GL (Y flipped). The GL winding flip does not matter because UI
  pipelines do not cull.
- RmlUi rectangles (top-left origin) go to `applyScissorTopLeft` / `applyViewportTopLeft`
  unchanged.
- Texture coordinate (0,0) is the top-left texel everywhere, so GL3's `VerticallyFlipped()`
  becomes the identity.
- Fullscreen passes use the engine's `fullscreen.vert` convention (it already flips v on
  non-GL), carried into `htmlui_post.vert` (§3.3).
- `SaveLayerAsTexture` copies without a flip.

#### 3.2.2 Targets and the layer stack
`RmlRenderTarget` (per rendered context, owned by `HtmlUIRenderState`):
- **`layers`** — `std::vector<Framebuffer>`, `FramebufferDesc{ w, h, RGBA, DEPTH_STENCIL,
  sampleCount }`. Every layer after the first shares layer 0's depth-stencil
  (`shareDepthStencil`), so the stencil clip survives layer switches, as in GL3.
- **`postprocess[4]`** — primary, secondary, tertiary and blend-mask. Each is 1-sample, no
  depth, `LINEAR` + `CLAMP_TO_EDGE`, and created lazily (GL3 `EnsureFramebufferPostprocess`).
- **`layersSize`** — the number of layers in use this frame.
- All of these are recreated only when the size or sample count changes. Layers pushed in an
  earlier frame are reused, which is the render-target pool.

The pass state machine is new compared with GL3, which rebinds framebuffers freely.
- `RmlRenderer` keeps `activeTarget` (layer index, a post-process index, or none) and opens
  a sokol pass only when it needs a different target.
- Before switching, it ends the open pass. MSAA layers resolve into their resolve texture at
  every pass end, because every layer pass lists `resolves[0]`.
- It begins the new pass with these actions:

  | Situation | Colour | Depth-stencil |
  |---|---|---|
  | Layer pushed this frame, first time bound | CLEAR (0,0,0,0) | LOAD/STORE (layer 0 at frame start: CLEAR 0 / STORE) |
  | Layer bound again after another target | LOAD/STORE | LOAD/STORE |
  | Post-process target | CLEAR, or LOAD when a pass writes only a sub-rectangle | none |
- After each pass begins, it re-applies the viewport, the current scissor and nothing else;
  pipelines are applied per draw.
- **Consequence.** A frame with no layers and no filters is exactly **one** sokol pass per
  rendered context. Tests assert this (§5.1).
- Layers are always top-left origin and are never presented directly. The camera-pass
  composite reads `layers[0]`'s colour texture (the resolve texture when MSAA).

#### 3.2.3 Geometry
- **`CompileGeometry`** copies vertices (`Rml::Vertex`: float2 position, ubyte4 premultiplied
  colour, float2 uv, stride 20) and int indices into **pooled DYNAMIC buffers**:
  - size classes are powers of two, minimum 4 KiB;
  - one vertex buffer and one index buffer per geometry;
  - indices are `UINT32`;
  - a buffer released in frame N can be reused from frame N+1, because sokol allows one
    update per buffer per frame (`VALIDATE_UPDATEBUF_ONCE`).
- **Drawing.** One `ObjectRender` per program (colour, texture, gradient, each effect, each
  post variant) is set up with `endLoadDynamic(TRIANGLES, UINT32)`. Each draw swaps
  `replaceVertexBuffer` / `setIndexBuffer` / `addTexture`, then calls `beginDraw(state)`,
  `applyUniformBlock` and `draw(0, count, 1)`. Bindings are applied inside `draw()`
  (`SokolObject.cpp:494-497`), so swapping them between `beginDraw` and `draw` is valid.
- **Vertex uniform block** `u_vs_htmlui { mat4 transform; vec4 translate; }`:
  - `transform` = projection × the `SetTransform` matrix, or projection alone;
  - `translate` = the draw translation in `.xy`.

#### 3.2.4 Clip masks (stencil)
State: `clipMaskEnabled` (`EnableClipMask`) and `stencilRef` (uint8, the current test
value).

`RenderToClipMask(op, geometry, translation)`:

| op | step 1 | step 2 | after |
|---|---|---|---|
| `Set` | stencil "clear" to 0 | draw geometry: ALWAYS, pass REPLACE, ref 1, writeMask colour NONE | `stencilRef = 1` |
| `SetInverse` | stencil "clear" to 1 | draw geometry: ALWAYS, pass REPLACE, ref 0, colour NONE | `stencilRef = 1` |
| `Intersect` | — | draw geometry: ALWAYS, pass INCR_CLAMP, colour NONE | `stencilRef += 1` (clamped at 255) |

- **Stencil clear inside a pass.** Sokol has no mid-pass clear, so a "clear to v" draws a
  fullscreen triangle with the `HTMLUI_POST` `Pas` variant. Its state is colour NONE,
  stencil ALWAYS / REPLACE / ref v. The current scissor limits it, which matches GL3's
  scissored `glClear`.
- **Geometry while `clipMaskEnabled`.** Stencil EQUAL, ref `stencilRef`, writeMask 0, ops
  KEEP. When disabled, stencil is off.
- Pipelines differ by ref value, so the cache holds one per nesting level actually used.
- Nested clips under transforms need nothing extra: the clip geometry is drawn with the
  current transform uniform.

#### 3.2.5 Layers, compositing, filters
These follow GL3; the differences are listed.
- **`PushLayer()`** — grows `layers` if needed, then switches to it with CLEAR on first
  bind.
- **`CompositeLayers(src, dst, blend, filters)`**:
  1. End the open pass, so `src` is resolved.
  2. Set the *post source* to `src`'s colour texture. There is no copy, unlike GL3's
     `BlitLayerToPostprocessPrimary`.
  3. Each filter reads the post source, writes the free post-process target, and that target
     becomes the source. This replaces GL3's primary/secondary swaps.
  4. Switch to `dst` with LOAD and draw the source with `Pas` (opacity 1). The state is
     `PREMULTIPLIED`, or `NONE` for `BlendMode::Replace`, with the current scissor and the
     stencil test when `clipMaskEnabled`.
- **`PopLayer()`** — decrements, then switches back to the top layer on the next draw (lazy).
- **`SaveLayerAsTexture()`**:
  - requires a valid scissor (as GL3);
  - ends the pass;
  - creates a `Framebuffer` of the scissor size (1-sample, no depth);
  - draws the top layer's region into it with `Pas`, `uvTransform` selecting the scissor
    rect;
  - returns `new HtmlUITexture{ RENDER_TARGET, ... }`.
- **`SaveLayerAsMaskImage()`** — ends the pass, copies the top layer into `postprocess[3]`
  with `Pas` (blend NONE), and returns a `MaskImage` compiled filter.
- **Filters** (`CompileFilter` mirrors GL3):

  | Filter | Implementation |
  |---|---|
  | `opacity` | `Pas` with `params.x = value`, multiplying all four premultiplied channels. This replaces GL3's `glBlendColor`, because sokol's `blend_color` is pipeline-baked. |
  | `blur` | below |
  | `drop-shadow` | GL3 algorithm. `Shd` writes `colour × source.a` into a free target with the uv offset `offset / viewport` (sign as GL3 line 1810 but without the y flip, per §3.2.1). If sigma ≥ 0.5 it is blurred with the tertiary target. The source is then drawn over it with `Pas`, PREMULTIPLIED blend, and the result becomes the source. |
  | colour matrix family (`brightness`, `contrast`, `invert`, `grayscale`, `sepia`, `hue-rotate`, `saturate`) | `Cmx` with the GL3 matrices, uploaded column-major |
  | `MaskImage` | `Msk`, sampling the source and `postprocess[3]` |

  An unknown filter name logs `Rml::Log::LT_WARNING` with the name, which `RmlSystem`
  routes to the Console. It never fails silently.
- **Blur** (GL3 `RenderBlur`):
  - downscale passes go into half-size viewports, alternating between the source and the
    temp target;
  - vertical then horizontal `Blr` passes, with weights from `SetBlurWeights` and
    `texCoordMin/Max` from the scissor;
  - the horizontal pass's target begins with CLEAR (0,0,0,0), which also provides GL3's
    1-pixel transparent border;
  - the two upscale "blits" are `Pas` draws with linear sampling, viewport = destination
    rect, `uvTransform` = source rect, blend NONE.
- **`backdrop-filter`** needs no extra code. RmlUi's `ElementEffects`
  (`Source/Core/ElementEffects.cpp:249-279`) pushes layers and composites `backdrop_source
  → temp` with the filters. That works because a lower layer's colour texture is resolved
  and current whenever it is not the active pass.

#### 3.2.6 Shaders (`CompileShader` / `RenderShader`)
- **Gradients.** `linear-gradient`, `radial-gradient` and `conic-gradient` (with
  `repeating`) are mapped exactly as GL3 lines 1647-1673, to `{func, p, v, stops}`. At most
  16 stops; extra stops are dropped with a warning naming the element's decorator. They are
  drawn with `HTMLUI_GRADIENT`.
- **`shader`** (the `decorator: shader("name")` decorator) is looked up in
  `HtmlUIShaderEffects` (§3.3.3). An unknown name logs a warning listing the registered
  names and returns 0, so nothing is drawn.
- `RenderShader` ignores its texture argument, as GL3 does.

#### 3.2.7 Textures
- **`LoadTexture(source)`** — what phase 0 does: `TextureData` loads, then a STATIC
  `HtmlUITexture`. Premultiplying happens on upload, because the engine's image data is
  straight alpha.
- **`GenerateTexture`** — data is already premultiplied RGBA8, so it becomes a STATIC
  texture.
- **Dynamic textures (p1-vector).** Created through
  `HtmlUITexture{DYNAMIC, ownedRender = createDynamicTexture(RGBA, LINEAR)}` and handed to
  RmlUi with `CallbackTextureInterface::SetTextureHandle` (§3.10.3). `RmlRenderer` treats
  every kind the same way when drawing.
- **Samplers:** LINEAR + CLAMP_TO_EDGE for all UI textures.

#### 3.2.8 Statistics
`struct HtmlUIRenderStats { uint32_t passes, drawCalls, pipelineApplies, pipelineCacheMisses,
textureBinds, layersPushed, filtersApplied, geometryLive, geometryBytes; }` is reset at
`BeginFrame`.
- `UIDocumentSystem::getRenderStats()` returns the last frame's sum.
- The editor's frame statistics show it in the Game view stats overlay next to the existing
  per-frame counters. That overlay is the one fed by `beginFrameStats`; `Engine.cpp:1229`
  is where frame statistics start.

#### 3.2.9 Performance rules (tested)
- **No layers or filters.** One pass and zero cache misses after the first frame. Draw calls
  equal the geometry drawn, and every draw sets the pipeline only when the
  (program, stencil ref, blend) triple changes.
- **Upload once.** Geometry and textures are uploaded once; nothing is re-uploaded per frame
  except DYNAMIC textures whose content changed.
- **No per-frame allocation in steady state.** Post-process targets and layer framebuffers
  persist. The test `RmlRendererPerf.SteadyStateNoAllocations` counts
  `Framebuffer::create`/`BufferRender::createBuffer` calls over 10 frames of a static
  document with filters and expects 0 after frame 1.

---

### 3.3 Shaders and registration (p1-render)

#### 3.3.1 Shader list
New ShaderTypes are appended after the last value phase 0 added; phase 0 appends after
`POSTPROCESS` (`Render.h:57`; the comment at line 56 says append only).

| ShaderType | Files (`engine/shaderlib/`) | Variants (property bits) | Uniform blocks / textures (resolved by name) |
|---|---|---|---|
| `HTMLUI` (phase 0) | `htmlui.vert`, `htmlui.frag` | phase-0 bits (colour / `Tex`) | `u_vs_htmlui { mat4 transform; vec4 translate; }`; `u_htmluiTexture` + `u_htmlui_smp` |
| `HTMLUI_GRADIENT` | `htmlui.vert`, `htmlui_gradient.frag` | none | `u_fs_htmluiGradient { vec4 funcAndCount; /* x = func 0..5, y = numStops */ vec4 pv; /* p.xy, v.zw */ vec4 stopColors[16]; vec4 stopPositions[4]; /* packed 4 per vec4 */ }` |
| `HTMLUI_POST` | `htmlui_post.vert`, `htmlui_post.frag` | **exclusive**: bit0 `Pas`, bit1 `Cmx`, bit2 `Msk`, bit3 `Shd`, bit4 `Blr`, bit5 `Unp` | `u_vs_htmluiPost { vec4 uvTransform; /* scale.xy, offset.zw */ }`; `u_fs_htmluiPost { vec4 params; /* x opacity, yz texelOffset */ vec4 texCoordMinMax; vec4 color; vec4 weights; mat4 colorMatrix; }`; `u_htmluiPostTex` + smp, `u_htmluiMaskTex` + smp |
| `HTMLUI_EFFECT` | `htmlui.vert`, `htmlui_effect_<name>.frag` | the properties field is the **effect index** (see §3.3.3) | `u_fs_htmluiEffect { vec4 dimensionsTime; /* w, h, seconds, 0 */ vec4 params[4]; }` |

Notes on the table:
- `htmlui_post.vert` is vertexless (`gl_VertexIndex`, 3 vertices) like `fullscreen.vert`,
  with the same non-GL v flip and Vulkan z fix-up. It applies `uvTransform`.
- `htmlui_post.frag` selects its code with `#ifdef HAS_PASSTHROUGH / HAS_COLOR_MATRIX /
  HAS_BLEND_MASK / HAS_DROP_SHADOW / HAS_BLUR / HAS_UNPREMULTIPLY`.
- `Blr` computes the 7 taps in the fragment shader from `texelOffset`, because the GL3 blur
  vertex shader's varying array does not cross-compile cleanly to every target.
- `Unp` writes `vec4(c.rgb / max(c.a, 1e-5), c.a)` for `TEXTURE`-mode output (§3.12).
- No UI shader converts colour space: inputs are sRGB-encoded and premultiplied (DESIGN
  "Rendering").

#### 3.3.2 Files to touch for each new ShaderType (line numbers from the research)

| File | Where | What |
|---|---|---|
| `engine/core/render/Render.h` | after the last value (`POSTPROCESS` at 57, or phase 0's) | `HTMLUI_GRADIENT, HTMLUI_POST, HTMLUI_EFFECT` |
| `engine/core/pool/ShaderPool.cpp` | `parseShaderTypeToken` 53-116 (before 113) | `htmlui_gradient`, `htmlui_post`, `htmlui_effect` |
| same | `getShaderTypeName` 389-408 | display and short names |
| same | `getShaderPropertyCount` 410-428 | GRADIENT 0, POST 6, EFFECT 0 |
| same | `getShaderPropertyName` 430-508 | `Pas/Passthrough`, `Cmx/ColorMatrix`, `Msk/BlendMask`, `Shd/DropShadow`, `Blr/Blur`, `Unp/Unpremultiply` |
| same | `getShaderStr` 510-533 | special case `HTMLUI_EFFECT`: `"htmlui_effect_" + HtmlUIShaderEffects::name(properties)` |
| same + `ShaderPool.h:162-164` | ~1000-1029 | `static uint32_t getHtmlUIPostProperties(HtmlUIPostVariant)` |
| `engine/core/shader/ShaderVariantCompiler.cpp` | `setupShaderArgs` 367-421 (before `else{return false;}` at 419) | vert/frag names; `addHtmlUIPostPropertyDefinitions` near 347-365; EFFECT: frag `htmlui_effect_<name>.frag` |
| `editor/shader/ShaderVariantCollector.cpp` | scene passes 541-566 | if the scene has any `UIDocumentComponent`: every `HTMLUI` variant, `HTMLUI_GRADIENT`, all 6 `HTMLUI_POST` variants, every registered `HTMLUI_EFFECT` index |
| `editor/Project.cpp` | ~2872-2884 (parallel list) | the same keys |
| `engine/core/subsystem/RenderSystem.cpp` | `getShaderVariants` 1861-1946 | the same keys for an entity with `UIDocumentComponent` |
| `editor/window/dialog/ExportWindow.cpp` | `typeValues[]` 1236-1242 | append the three types |
| `engine/core/script/binding/CoreClassesLua.cpp` | 453-468 (`ShaderType` namespace) | three `addVariable` lines |
| `editor/App.cpp` | 3505-3510 | cache dir `"v29"` → `"v30"` with a changelog comment ("v30: HTML UI gradient, post-process and effect shaders") |

The new types are **not** user-forkable: `ProjectUtils`, `ShaderForkDialog`, `ProjectChecks`
and the MCP `AssetTools` maps stay unchanged.

The shaderlib glob (`engine/CMakeLists.txt:951-967`) picks up the new files. The precompiled
headers (`ShaderPool.cpp:23-46`) are regenerated by the editor's shader build, as for any
built-in type.

#### 3.3.3 Shader-decorator registry (the hook phase 5 fills)
`engine/core/htmlui/HtmlUIShaderEffects.{h,cpp}` (p1-render):
```cpp
struct HtmlUIShaderEffect {
    const char* name;                 // decorator: shader("<name>")
    const char* fragFile;             // "htmlui_effect_<name>.frag"
    bool animated;                    // needs a redraw every frame (time uniform)
    // maps decorator parameters to params[4]; unknown keys -> warning
    void (*fillParams)(const Rml::Dictionary& parameters, float out[16]);
};
class HtmlUIShaderEffects {
public:
    static const std::vector<HtmlUIShaderEffect>& all();   // index = HTMLUI_EFFECT properties value
    static int find(const std::string& name);              // -1 if unknown
    static const char* name(uint32_t index);
};
```
- The registry is a static table. Index 0 is the built-in **`test-pattern`** (diagonal
  stripes from `params[0]` colour A and `params[1]` colour B, `params[2].x` stripe width in
  px). It is used by the golden test.
- Phase 5 appends rows and fragment files; the index order is append-only, like ShaderType.
- `animated` effects make `UIDocumentSystem` request a redraw every frame through
  `Context::RequestNextUpdate(0)`.

#### 3.3.4 Pipeline states used by the UI (all through `PipelineState`)

| Use | blend | writeMask | stencil | target |
|---|---|---|---|---|
| geometry, gradients, effects | PREMULTIPLIED | RGBA | off, or EQUAL ref=n | layer (n samples, DEPTH_STENCIL) |
| clip write | — | NONE | ALWAYS, REPLACE ref 0/1 or INCR_CLAMP | layer |
| stencil clear | — | NONE | ALWAYS, REPLACE ref v | layer |
| layer composite | PREMULTIPLIED / NONE (Replace) | RGBA | as geometry | layer |
| post passes | NONE (or PREMULTIPLIED for drop-shadow's source-over) | RGBA | off | 1 sample, no depth |
| camera-pass composite | PREMULTIPLIED | `targetFormatFor(colorPip)` rules: RGB on swapchain like PIP_DEFAULT, RGBA on RTT | off | `targetFormatFor(colorPip)` |

---

### 3.4 Backends through sokol

| Backend | Platforms | Specifics |
|---|---|---|
| GL core 4.1 | Linux, Windows, macOS (editor + games) | Y-flipped UI projection (§3.2.1). MSAA renderbuffers are resolved by sokol with `glBlitFramebuffer` at pass end; STORE keeps the MSAA content for the next LOAD. Linux GLX asks for 8 stencil bits (`JasperEngineLinux.cpp:109`); this is irrelevant for offscreen targets. |
| GLES3 / WebGL2 | Android, Web | Same as GL. `GL_CLAMP_TO_BORDER` is unavailable, so the blur and drop-shadow rely on `texCoordMinMax` in the shader (as GL3 does on GLES). WebGL2 has `separate_buffer_types`, already satisfied by separate vertex and index buffers. `UINT32` indices are core in WebGL2. MSAA via renderbuffers. The Web swapchain is already 4x (`JasperEngineWeb.cpp:169`), which does not matter for offscreen UI targets. |
| D3D11 | Windows | `DEPTH_STENCIL` = D24S8. MSAA resolve via `ResolveSubresource` inside sokol. Shaders: HLSL 5 from the editor's cross-compiler. |
| Metal | macOS, iOS | The stencil store action follows `depth.store_action` (§3.1.4). `StoreAndMultisampleResolve` keeps MSAA content between split passes (`sokol_gfx.h:14632-14648`). |
| Vulkan | Linux, Windows, macOS | All UI passes are **offscreen** and run before the scene's camera pass, so the rule "one swapchain pass per frame" (`Engine.cpp:778-789` composite path) is unaffected. Pipelines take the stencil ref from the pipeline (`sokol_gfx.h:21410`); the only dynamic states are viewport and scissor (21458-21459). |

Sokol forbids nested passes. Every UI pass begins and ends inside
`UIDocumentSystem::renderOffscreen()`, which runs outside any pass (§3.5). The only UI work
inside a camera pass is the composite draw.

---

### 3.5 Frame integration (p1-render)

- **One render target per Rml context.** A scene's screen-space documents share the scene
  context. They render together into the context's target, ordered by RmlUi z-order:
  `UIDocumentSystem` sets each document's `z-index` to its `sortOrder` and calls
  `PullToFront` in `sortOrder` order whenever an order changes. The target is composited
  once in the camera pass.
- **Texture-mode documents** have their own contexts (§3.12).
- **If phase 0 rendered one target per document**, p1-render replaces that with the
  per-context target. The deviation is recorded in the final report; the reason is that
  layers, filters and stencil are per render pass anyway, and one target saves memory and
  composites.

Declarations (wave 0) and definitions (`engine/core/subsystem/UIDocumentSystemRender.cpp`):
```cpp
void renderOffscreen();                          // renders every context updated since its last render
const HtmlUIRenderStats& getRenderStats() const;
void compositeScreenDocuments(CameraComponent& camera, Transform& cameraTransform, PipelineType colorPip); // inside the camera pass
```

Call sites:
1. **`UIDocumentSystem::draw()`** (phase-0 `.cpp`, one line): `renderOffscreen();`.
   - `Scene::draw` calls systems in registration order (`Scene.cpp:662-666`), and
     `UIDocumentSystem` is registered before `RenderSystem` (`Scene.cpp:31-32` plus phase
     0's line). So the UI passes happen before `RenderSystem::draw` opens any camera pass.
   - With stacked scenes, each scene's UI passes run between the previous scene's camera
     pass and its own, which sokol allows.
2. **`RenderSystem::drawView`** (editor views, `RenderSystem.cpp:10730`): inside
   `if (!frameDrawn){ ... }` (10748-10752), before `drawFramePasses()`, add
   `scene->getSystem<UIDocumentSystem>()->renderOffscreen();`. Editor-only views never call
   `Scene::draw`.
3. **`renderOffscreen()` is idempotent.** Each context has `bool updatedSinceRender`, set by
   `UIDocumentSystem::update()` after `context->Update()`. It renders only when that flag is
   set, then clears it. Animated shader effects, Lottie and RmlUi transitions request
   updates, so they keep the flag set each frame.
4. **Composite.** The phase-0 composite call in `RenderSystem::renderCameraPass` stays (or
   moves) right **after the transparent queue and before `camera.render.endRenderPass()`**
   (`RenderSystem.cpp:9944-9957`), so documents draw in front of component UI. It calls
   `compositeScreenDocuments`:
   - **`CAMERA_UI` or `CAMERA_2D`** (game cameras and the editor's `SceneRender2D` camera):
     a quad covering the canvas rectangle (0,0)–(canvasW, canvasH) in the world space
     where `RectTransform` UI lives, drawn with the camera's view-projection. The editor
     Scene view of UI and 2D scenes therefore shows documents in place while panning and
     zooming.
   - **3D cameras:** a viewport-filling overlay quad in the game camera. Screen-space
     documents are not drawn in the editor Scene view of a 3D scene (its free camera has no
     canvas); they show in the Game view.
   - The composite uses the `HTMLUI` texture variant, or phase 0's composite program, with
     the PREMULTIPLIED state for `colorPip`.
   - Under post-process or SSR the composite sits inside the colour pass like component UI,
     the existing behaviour for UI.

---

### 3.6 Input (p1-input)

#### 3.6.1 Engine API (`engine/core/Engine.h` ~472-505, `Engine.cpp`)
New entry points, declared next to the existing `system*` functions:
```cpp
static void systemTextInput(const std::string& utf8);            // committed text (IME commit, paste, platform strings)
static void systemImeComposition(const std::string& utf8, int cursorCodepoints); // start/update; cursor -1 = select all
static void systemImeCancel();                                     // drop the current composition
static bool wantsTextComposition();                                // a document text field is focused
static void setTextInputArea(const Rect& canvasRect);              // caret area → System::setTextInputArea (screen pixels)
```

Changes to existing functions:

| Function | Change |
|---|---|
| `systemMouseDown`/`Up` (1465/1490) | Documents first, for buttons 0-2 (`D_MOUSE_BUTTON_1..3` map to RmlUi buttons 0, 1, 2). Then `UISystem` (button 1 only, unchanged). Then the game, with `uiEventReceived` covering both systems. |
| `systemMouseMove` (1516) | Documents, then `UISystem`, then the game. |
| `systemMouseScroll` (1542) | Now reaches documents: `ProcessMouseWheel(Vector2f(-xoffset, -yoffset) × wheelScale, mods)`. The engine sends positive y = up (GLFW convention); RmlUi wants positive = down/right. RmlUi multiplies the delta by `UNIT_SCROLL_LENGTH × dp ratio` (`Source/Core/Context.cpp:827-828`). `wheelScale` comes from `HtmlUIContext::setWheelScale(float)`, default 1, so one platform wheel step is one RmlUi scroll unit. The game's `onMouseScroll` runs only if the document did not consume it. |
| `systemMouseEnter`/`Leave` (1549/1554) | Leave → `ProcessMouseLeave()` on every context. Enter → nothing; the next move re-activates. |
| `systemKeyDown` (1559) | New order: **documents → game `onKeyDown` → `UISystem`**. The game is skipped when a document consumed the key and `ignoreEventsHandledByUI`. |
| `systemKeyUp` (1571) | Reaches documents. Consumed iff the matching key down was consumed (per-key bitset in `HtmlUIInputState`), so the game never sees an up without a down. |
| `systemCharInput` (1577) | On platforms where `sizeof(wchar_t) == 2`, combines surrogates: a pending high surrogate waits for the low one; a lone surrogate becomes U+FFFD. The result goes to documents as UTF-8. The game's `onCharInput` and `UISystem::eventOnCharInput` still receive each `wchar_t` unit as today. |
| `systemTextInput` (new) | Documents get the whole string. Then, unless consumed, each code point goes to `onCharInput` and `UISystem` (two surrogate units on 16-bit `wchar_t`). |
| `systemTouchStart/Move/End` (1380/1426/1403) | Documents get `ProcessTouchStart/Move/End({Touch{pointer, pos}}, mods)` with the real pointer id. |
| `systemTouchCancel` (1448/1455) | Now reaches documents (`ProcessTouchCancel`). |

Document dispatch rules:
- Scenes are walked **top layer first** (reverse of `getScenesSnapshot()`, `Engine.cpp:182`),
  using the same `canSendUIEvents` gate (1362); the first scene that consumes stops the
  walk.
- `UISystem` keeps its current all-scenes behaviour.
- Modifiers go to RmlUi on every call, including pointer events, which today rely on
  `Input::getModifiers()`.

#### 3.6.2 `UIDocumentSystem` input methods (`UIDocumentSystemInput.cpp`)
```cpp
enum class PointerPhase { DOWN, MOVE, UP, CANCEL };    // wave 0, UIDocumentSystem.h
bool eventOnPointerDown(int pointer, int button, float x, float y, int mods, bool touch);
bool eventOnPointerUp(int pointer, int button, float x, float y, int mods, bool touch);
bool eventOnPointerMove(int pointer, float x, float y, int mods, bool touch);
bool eventOnTouchCancel(int pointer);
bool eventOnWheel(float dx, float dy, int mods);
void eventOnMouseLeave();
bool eventOnKeyDown(int key, bool repeat, int mods);
bool eventOnKeyUp(int key, int mods);
bool eventOnTextInput(const std::string& utf8);
bool eventOnImeComposition(const std::string& utf8, int cursor);
void eventOnImeCancel();
bool hasFocusedTextInput() const;
```
These replace the phase-0 pointer methods; phase 0's signatures become thin wrappers.

**Coordinates.** Canvas coordinates from `Engine::transformCoordPos` are scaled to context
pixels with the context/canvas ratio phase 0 established. `TEXTURE`-mode contexts get
pointers through the world mapper (§3.12.3). p1-rtt adds that call to `eventOnPointer*`,
after the screen context declined the event.

**Consume rules.** Each returns true when consumed.
- **Pointer** (down/up/move/touch): consumed iff, after processing, the hover element is a
  *blocking element*. A blocking element is non-null, not the context root, and either not
  an `ElementDocument` or an `ElementDocument` whose computed `background-color` alpha > 0
  or which has a decorator.
  - Elements with `pointer-events: none` are skipped by RmlUi's hit test, so transparent
    full-screen containers pass clicks through when authors set it, as over a web canvas.
    The user guide documents this rule.
- **Up:** consumed iff its down was consumed, or the rule above holds.
- **Wheel:** consumed iff `ProcessMouseWheel` returns false, or the hover element is
  blocking.
- **Key down/up:** consumed iff
  - the focus element is not the document/root and `ProcessKeyDown` returned false
    (propagation stopped or `preventDefault`, §3.14); or
  - the focus element is a text control (`input` of a text type, `textarea`, `select`) and
    the key is not one of Escape and F1–F25.
- **Text and IME:** consumed iff a text control has focus.

**Control characters.** `\b`, `\r`, `\n`, `\t` and `0x1B` arriving as text are not inserted.
Several platforms send them as characters (Linux `XLookupString`, Windows `WM_CHAR`,
Android BACK → `\b`, editor `PlayInput` lines 166-221).
- If no matching key down (BACKSPACE, ENTER, TAB, ESCAPE) reached documents in the same
  frame, a synthetic key down/up is sent instead.
- Otherwise the character is dropped.
- This keeps Android's soft-keyboard backspace working without double deletes on desktop.

**Key map.** `engine/core/htmlui/RmlKeyMap.{h,cpp}`:
- `Rml::Input::KeyIdentifier toRmlKey(int dKey)` is a full table over `Input.h:17-138`:
  - `D_KEY_A..Z` → `KI_A..Z`, `D_KEY_0..9` → `KI_0..9`;
  - punctuation → `KI_OEM_*` (US layout, as RmlUi's Win32 backend);
  - F1–F24, keypad → `KI_NUMPAD*`, arrows, Home/End/PgUp/PgDn, Insert/Delete;
  - `LEFT/RIGHT_SHIFT|CONTROL|ALT|SUPER` → `KI_LSHIFT/RSHIFT/LCONTROL/RCONTROL/LMENU/RMENU/LMETA/RMETA`;
  - CAPS/SCROLL/NUM lock, PAUSE, PRINT_SCREEN → `KI_SNAPSHOT`, MENU → `KI_APPS`;
  - `D_KEY_UNKNOWN`, `WORLD_1/2` and `F25` → `KI_UNKNOWN`.
- `int toRmlModifiers(int dMods)`: SHIFT→`KM_SHIFT`, CONTROL→`KM_CTRL`, ALT→`KM_ALT`,
  SUPER→`KM_META`, CAPS→`KM_CAPSLOCK`, NUM→`KM_NUMLOCK`. On `__APPLE__`, SUPER also sets
  `KM_CTRL`, because RmlUi's text widget reads only `ctrl_key` for copy/cut/paste/select-all
  (`WidgetTextInput.cpp:529-618`).
- It also provides the DOM `key`/`code` strings for `UIEvent` (§3.9.3), from the D_KEY plus
  shift state.

**Focus → virtual keyboard and IME.** `engine/core/htmlui/RmlTextInputHandler.{h,cpp}`, an
`Rml::TextInputHandler` installed on every context (passed to `Rml::CreateContext`):
- `OnActivate(ctx)`: remember the context, then
  `System::instance().showVirtualKeyboard(utf8ToWString(current value))`.
- `OnDeactivate` / `OnDestroy`: cancel any composition, then `hideVirtualKeyboard()`.
- Composition state machine: a port of RmlUi's platform-neutral `TextInputMethodEditor_Win32`
  logic (`Backends/RmlUi_Platform_Win32.cpp:598-728`: `StartComposition`, `SetComposition`,
  `ConfirmComposition`, `CancelComposition`, `SetCompositionString`,
  `UpdateCursorPosition`), driven by `eventOnImeComposition` / `eventOnTextInput` (a commit
  while composing confirms) / `eventOnImeCancel`.
- `RmlSystem::ActivateKeyboard(caret, lineHeight)` (phase-0 file, additive) calls
  `Engine::setTextInputArea(Rect(caret.x, caret.y, 1, lineHeight))`, converted from context
  to canvas coordinates. It does not reopen the keyboard: RmlUi calls it on every caret
  move.
- `RmlSystem::DeactivateKeyboard()` does nothing; `OnDeactivate` handles it.
- `RmlSystem::SetClipboardText` / `GetClipboardText` call the `System` clipboard (§3.7).

#### 3.6.3 `System` additions (`engine/core/System.h` after line 86, `System.cpp`)
```cpp
virtual void setClipboardText(const std::string& utf8);
virtual std::string getClipboardText();
virtual void setTextInputArea(float x, float y, float width, float height); // screen pixels, top-left origin
```
- The base implementation keeps an in-process clipboard string, so headless tests work.
  `setTextInputArea` is a no-op there.
- Lua: in `CoreClassesLua.cpp` after line 1340, `System.setClipboardText(text)` and
  `System.getClipboardText()`.

#### 3.6.4 Editor input (`editor/window/PlayInput.cpp`)
- `io.MouseWheelH` is forwarded as the x offset; today line 91 sends only y.
- `systemMouseEnter` / `systemMouseLeave` are sent when the Game view's hovered state
  changes.
- `editor::Platform` (`editor/Platform.h:12`) overrides `setClipboardText` /
  `getClipboardText` with `ImGui::SetClipboardText` / `ImGui::GetClipboardText`. These are
  backed by the editor's X11, Win32 and NSPasteboard implementations
  (`editor/backend/BackendLinux.cpp:2197/2275`, `imgui_impl_win32`, `imgui_impl_osx`).
- ImGui characters are 16-bit (`IMGUI_USE_WCHAR32` is off, `libs/imgui/imconfig.h:70`), so
  astral characters arrive as surrogates and are combined by `systemCharInput` (§3.6.1).

MCP `send_input` (`editor/mcp/tools/PlayTools.cpp`):
- `text` sends the whole UTF-8 string through `Engine::systemTextInput`. Today line 2279
  casts each code point to `wchar_t`, which truncates on Windows.
- New actions:
  - `ime` with `{text, cursor}` → `systemImeComposition`;
  - `ime_commit` with `{text}` → `systemTextInput`;
  - `ime_cancel`;
  - `mouse_leave`, `mouse_enter`.
- `scroll` already has dx/dy (2384-2395).
- Tool docs are regenerated with `tools/mcp/gen_tool_docs.py`.

---

### 3.7 Platform layers (p1-input)

None of the platforms uses sokol_app; each has its own windowing (research §4).

| Platform | Clipboard | IME / text | Virtual keyboard | Other input gaps closed |
|---|---|---|---|---|
| **Linux X11** (`engine/platform/linux/`) | Own the `CLIPBOARD` selection: `XSetSelectionOwner`. Answer `SelectionRequest` for `TARGETS`, `UTF8_STRING`, `STRING`, `TEXT` in the event loop (`JasperEngineLinux.cpp:239`). `getClipboardText`: if we own it, return the local copy; else `XConvertSelection(CLIPBOARD, UTF8_STRING)` and pump `XCheckTypedWindowEvent(SelectionNotify)` for up to 200 ms, then `XGetWindowProperty` (pattern: editor `BackendLinux.cpp:2197-2275`). | `XSetLocaleModifiers("")`, `XOpenIM`. `XCreateIC` with `XIMPreeditPosition\|XIMStatusNothing` if `XNQueryInputStyle` offers it, else `XIMPreeditNothing\|XIMStatusNothing`. `XFilterEvent` before routing. `LinuxInputRouter::handleKey` (21) uses `Xutf8LookupString` → `systemTextInput`, replacing the Latin-1 `XLookupString` path at 43-54. `setTextInputArea` → `XNSpotLocation` (`XSetICValues` with a `XVaNestedList`). `XSetICFocus`/`XUnsetICFocus` on FocusIn/Out (254/259). Editor reference: `BackendLinux.cpp:1021, 1643, 2723`. | none (desktop) | — |
| **Windows** (`engine/platform/win/`) | `OpenClipboard` / `EmptyClipboard` / `SetClipboardData(CF_UNICODETEXT)` and `GetClipboardData(CF_UNICODETEXT)`, with UTF-8↔UTF-16 via `MultiByteToWideChar`/`WideCharToMultiByte`. | In `WinInputRouter::handleMessage` (44), while `Engine::wantsTextComposition()`: `WM_IME_STARTCOMPOSITION` (no default processing); `WM_IME_COMPOSITION` with `GCS_COMPSTR` → `systemImeComposition(utf8, ImmGetCompositionString(GCS_CURSORPOS))` and `GCS_RESULTSTR` → `systemTextInput`; `WM_IME_ENDCOMPOSITION` → `systemImeCancel` if nothing was committed; `WM_IME_CHAR` swallowed. Otherwise `DefWindowProc`, which keeps the legacy behaviour for component `InputField`. `setTextInputArea` → `ImmSetCompositionWindow(CFS_POINT)` + `ImmSetCandidateWindow(CFS_EXCLUDE)`. Link `imm32` in the Windows section of `engine/CMakeLists.txt` (284-359). | none | `WM_MOUSELEAVE` via `TrackMouseEvent(TME_LEAVE)` on the first `WM_MOUSEMOVE` → `systemMouseLeave`/`systemMouseEnter`. `WM_POINTERDOWN/UPDATE/UP` with `PT_TOUCH` (`GetPointerType`) → `systemTouchStart/Move/End(pointerId)`; mouse stays on `WM_MOUSE*`. `WM_CHAR` surrogate pairs combine in Engine (§3.6.1). |
| **macOS** (`engine/platform/mac/`) | `NSPasteboard.generalPasteboard`, `clearContents` / `setString:forType:NSPasteboardTypeString` / `stringForType:`. | New `MacTextInputClient.{h,mm}` holds the `NSTextInputClient` logic shared by `MacViewMetal`, `MacViewGL` and `MacViewVulkan`, replacing the stubs at `MacViewMetal.mm:65-75`, `MacViewGL.mm:86-96` and `MacViewVulkan.mm:87-97`: `setMarkedText:selectedRange:replacementRange:` → `systemImeComposition(utf8, selectedRange.location)`; `unmarkText` → commit the marked text; `insertText:` → `systemTextInput(utf8)` with the whole string (replacing the UTF-16 unit loop at `MacInputRouter.mm:146`); `hasMarkedText`/`markedRange`/`selectedRange`; `firstRectForCharacterRange:` → the last `setTextInputArea`, via `convertRectToScreen:`; on deactivate `[[view inputContext] discardMarkedText]`. | none | — |
| **iOS** (`engine/platform/apple/ios/`) | `UIPasteboard.generalPasteboard.string`. | `EngineView` (MTKView `<UIKeyInput>`, `ios/EngineView.h:6`) owns a hidden `UITextField` (alpha 0.01, 1×1, moved to the text-input area). It keeps the sentinel text `" "` so backspace on empty is detected. Delegate `textField:shouldChangeCharactersInRange:replacementString:`: deleting the sentinel → `systemKeyDown/Up(D_KEY_BACKSPACE)` + `systemCharInput('\b')`; otherwise, when `markedTextRange == nil`, → `systemTextInput`. `UITextFieldTextDidChangeNotification`: if `markedTextRange != nil` → `systemImeComposition(markedText)`, else commit the pending composition. This is the hidden-text-field approach SDL uses on iOS. `UIKeyInput` on `EngineView` is removed; component `InputField` gets the same text through `systemTextInput` → `onCharInput`/`UISystem`. | `showVirtualKeyboard` (`JasperEngineApple.mm:161`) → `[textField becomeFirstResponder]`; `hideVirtualKeyboard` (167) → `resignFirstResponder`. | `pressesBegan/pressesEnded` (iOS 13.4+ `UIKey.keyCode` HID usage → D_KEY table in a new `KeyCodesIOS.h`, modifiers from `UIKey.modifierFlags`) → `systemKeyDown/Up`. iOS today has no key events. |
| **Android** (`engine/platform/android/` + `java/.../MainActivity.java`) | `MainActivity` gets `public void setClipboardText(String)` (posts to the UI thread with `runOnUiThread`, `ClipboardManager.setPrimaryClip(ClipData.newPlainText)`) and `public String getClipboardText()`. `JasperEngineAndroid` calls them over JNI (`AttachCurrentThread` on the game thread, cached `jmethodID`s). | In the `GameTextInputState` diff (`NativeEngine.cpp:442-469`), while `Engine::wantsTextComposition()`: text outside the composing region (`composingRegion.start/end`) is diffed against the previous committed text. Removals → `systemKeyDown/Up(D_KEY_BACKSPACE)` per removed code point; additions → `systemTextInput`; the composing span → `systemImeComposition(span, cursor - start)`, or `systemImeCancel` when the region becomes empty without a commit. Otherwise the legacy unit diff is unchanged, for component `InputField`. Diff in code points, not `wchar_t` units. | Unchanged (`showSoftInput`, `NativeEngine.cpp:376`). | `AMOTION_EVENT_ACTION_SCROLL` (`AXIS_VSCROLL/HSCROLL`) → `systemMouseScroll`. `AINPUT_SOURCE_MOUSE` hover (`ACTION_HOVER_MOVE/ENTER/EXIT`) → `systemMouseMove` / `systemMouseEnter` / `systemMouseLeave`. Both in `handleGameActivityInput` (828). |
| **Web** (`engine/platform/emscripten/`) | `setClipboardText` → `navigator.clipboard.writeText` (`EM_JS`; keydown is a user activation), falling back to a hidden textarea + `document.execCommand('copy')`. Paste: a `paste` listener on the IME textarea and on `document` → `systemTextInput(clipboardData.getData('text/plain'))`. Ctrl/Cmd+V keydown is **not** sent to documents on Web, because RmlUi would ask synchronously for the clipboard, which the async browser clipboard cannot answer. `getClipboardText` returns the text of the last `paste` event. | A hidden `<textarea id="jasper-ime">` is created on startup (`EM_JS`), positioned by `setTextInputArea` (canvas-relative CSS px), and focused on `showVirtualKeyboard`, blurred on hide. `compositionstart/update` → `systemImeComposition(e.data)`; `compositionend` → `systemTextInput(e.data)`; `input` with `inputType == 'insertText'` and not composing → `systemTextInput(e.data)`. The textarea value is cleared after each event. While it is focused, the existing `keypress` → `systemCharInput` path (`JasperEngineWeb.cpp:523-529`) is skipped so text is not inserted twice; keydown/keyup keep working. | Focusing the textarea opens the mobile keyboard. | — |

`setTextInputArea` takes screen pixels. `Engine::setTextInputArea` converts canvas → screen
with `viewRect` (the inverse of `transformCoordPos`).

---

### 3.8 Editor integration (p1-editor)

#### 3.8.1 Component fields (wave-0 contract, `UIDocumentComponent.h`)
```cpp
enum class UIDocumentRenderMode { SCREEN, TEXTURE };
struct UIDocumentComponent {
    std::string path;                  // project-relative .html/.htm/.rml (phase 0)
    int sortOrder = 0;                 // (phase 0)
    bool visible = true;
    bool interactive = true;           // receives input
    UIDocumentRenderMode renderMode = UIDocumentRenderMode::SCREEN;
    int textureWidth = 1024;           // TEXTURE mode
    int textureHeight = 1024;
    Entity inputSurface = NULL_ENTITY; // TEXTURE mode: mesh hit-tested for pointer input; NULL = own entity
    float pixelRatio = 0.0f;           // 0 = automatic (phase-0 dp ratio); TEXTURE mode default 1
    FunctionSubscribe<void()> onLoad;  // after every (re)load
    FunctionSubscribe<void()> onUnload;
    bool needReload = false;           // runtime: set by property changes
};
```
Runtime state (Rml documents, contexts, target framebuffers) lives in `UIDocumentSystem`
state objects keyed by entity, never in the component.

#### 3.8.2 Catalog (`editor/Catalog.h`, `Catalog.cpp`)
Phase 0 registered the type minimally. p1-editor completes every row of the research table.
- `ComponentType::UIDocumentComponent` stays at the position phase 0 gave it (appended after
  `ReflectionProbeComponent`, `Catalog.h:118`).
- Property table `kUIDocumentProperties[]` (after `kTextProperties`, ~534):

  | Property | Type |
  |---|---|
  | `path` | `String` |
  | `sortOrder` | `Int` |
  | `visible` | `Bool` |
  | `interactive` | `Bool` |
  | `renderMode` | `Enum` (`SCREEN`/`TEXTURE`) |
  | `textureWidth`, `textureHeight` | `Int`, clamped 1..8192 |
  | `inputSurface` | `Entity` |
  | `pixelRatio` | `Float` |

  Update flag: a new `UpdateFlags_UIDocument = (uint64_t)1 << 37` (`Catalog.h:58` is
  bit 36).
- Resolve and enumerate functions (~1460 / ~2436), a `kFastComponentResolvers` row
  (3068-3120), `computeComponentName` (before 3362), display name "UI Document" (after
  3423), icon `ICON_FA_FILE_CODE` (after 3481), `getComponentId` (before 3596),
  `getComponentType("uidocument")` (before 3709), `findComponents` (before 3915).
- `updateEntity` (before 4414): on `UpdateFlags_UIDocument` set `needReload = true` for
  `path`/`renderMode`/`texture*`/`pixelRatio`, and re-sort for `sortOrder`.
- `copyComponent` (before 4700), through Stream encode/decode.
- `ProjectUtils::addEntityComponent` (`default:` at 1272) and `removeEntityComponent` (1688)
  cases.
- `McpValues.cpp:735-780`: enum options for `renderMode` (`"screen"`, `"texture"`).

#### 3.8.3 Stream and Factory
- **Stream.** `encodeUIDocumentComponent` / `decodeUIDocumentComponent(const YAML::Node&,
  const UIDocumentComponent* old)` (declared before `Stream.h:476`). Dispatch goes before
  `Stream.cpp:3982` (encode) and `4651` (decode). Keys: `path`, `sortOrder`, `visible`,
  `interactive`, `renderMode` (`screen|texture`), `textureWidth`, `textureHeight`,
  `inputSurface` (entity id), `pixelRatio`. Defaults are omitted.
  - Decode starts from `*old`, as `decodeAudioSourceComponent` (6127) does, and sets
    `needReload = true` when `path` or the render settings changed. Play stop restores via
    `restorePlaySnapshot` → `decodeComponents` (Project.cpp:4646), so runtime state must
    reset here.
- **Factory.** `createUIDocumentComponent(int indentSpaces, EntityRegistry*, Entity,
  sceneName, entityName, assignExisting, entityVarNames)` (declared near `Factory.h:195`,
  defined like `Factory.cpp:1655-1684`). A case goes in the `createComponent` switch
  (2349-2403). It emits every non-default field; `inputSurface` is written through
  `entityVarNames` like other entity references.

#### 3.8.4 Inspector, Add Component, Hierarchy, Project window
- **`InspectorWindow::drawUIDocumentComponent`** (declared after `InspectorWindow.h:467`;
  dispatch branch before 14069). Rows:
  - **Document**: the file field pattern of `drawAudioComponent` (7834-7948). It resolves
    relative to assets, rejects `..`, accepts `Util::isHtmlFile`, uses a new
    `FILE_DIALOG_HTML = 1<<8` in `FileDialogs.h:15-23`, drag-drop `resource_files` only when
    stopped, and adds two buttons: **Open** (`getCodeEditor()->openFile(path, true)`, as in
    9584) and **Reload** (`UIDocument::reload()`).
  - Sort Order, Visible, Interactive.
  - Render Mode.
  - Texture Size (width/height, shown in TEXTURE mode).
  - Input Surface (entity picker, TEXTURE mode).
  - Pixel Ratio (0 shown as "Auto").
  - **Messages**: a read-only list of the last 50 RmlUi log messages for this document
    (warnings and errors with `file:line` when RmlUi gives one), from
    `UIDocumentSystem::getDocumentMessages(entity)` (p1-editor, §3.8.6).
  - **Debugger** toggle button (§3.8.7).
- **`ComponentAddDialog.cpp`**: UI category (90-102), append
  `{ComponentType::UIDocumentComponent, "HTML/CSS document rendered by the engine"}` after
  101.
- **Hierarchy *Create > UI > UI Document***:
  - entry in `getCreateMenuEntries()` after `HierarchyWindow.cpp:502`, icon
    `ICON_FA_FILE_CODE`, mask `CREATE_IN_ALL`;
  - `EntityCreationType::UI_DOCUMENT` after `REFLECTION_PROBE` (`CreateEntityCmd.h:67`);
  - branch in `CreateEntityCmd::execute` before 633: `Transform` + `UIDocumentComponent`,
    name "UI Document";
  - row icon in `getObjectIcon` (938).
  - The same type is added to MCP `EntityTools.cpp:~185-230` (`{"ui_document",
    EntityCreationType::UI_DOCUMENT, "UI Document", "HTML/CSS document"}`),
    `WorldTools.cpp:3217` and the CLI `CommandLine.cpp:775-776`.
- **Project window** (`ProjectWindow.h:32-44`, `ProjectWindow.cpp`):
  - `FileType::HTML` and `FileType::CSS`;
  - branch in the `scanDirectory` chain before 2096;
  - separator colours (723-751);
  - icons: new 512×512 PNGs `editor/resources/icons/html-icon.png` and `css-icon.png`,
    embedded as `html-icon_png.h` / `css-icon_png.h` in the same `xxd -i` format with the
    MIT header (like `audio-icon_png.h`), included at 11-19 and loaded at 3130-3162;
  - no thumbnails (the type is not queued at 2098-2101);
  - double-click already opens the code editor (1279-1301).
  - **Assets > Create > HTML Document** (App.cpp:847-855 pattern, `assetsCreateScript`-like
    helper) writes `NewDocument.html` + `NewDocument.css` from a template: a `<link>` to the
    CSS, a body with one heading and one button, and RCSS valid for RmlUi 6.3.
- **`editor/util/Util.h`** (148-150):
  - `isHtmlFile` (`.html`, `.htm`, `.rml`) and `isCssFile` (`.css`, `.rcss`), modelled on
    `isAudioFile` (138-146);
  - both added to `isAssetFile`, so rename and copy remap the component path through
    `Project::visitAssetPathsInRegistry` (`Project.cpp:481-615`), which gets a
    `UIDocumentComponent::path` visitor next to AudioSource (596-603).
- **`editor/util/ScriptEvents.cpp:24-37`**: a `ScriptEventSource` entry for
  `UIDocumentComponent` with `onLoad`, `onUnload`.

#### 3.8.5 Code editor: HTML and CSS
`editor/window/widget/CustomTextEditor.{h,cpp}`, `editor/window/CodeEditor.cpp`:
- `SyntaxLanguage { None, Cpp, Lua, CMake, Html, Css }` (`CustomTextEditor.h:22-27`).
- `languageForPath` (`CodeEditor.cpp:413-425`): `.html .htm .rml` → Html, `.css .rcss` → Css.
- Cases in `initializeLanguage` (168-262) and `GetLanguageName` (412-419).
- **Tokenizer.** For Html and Css, `tokenizeLine` (544-744) calls
  `HtmlCssLexer::lexLine(language, line, state)` (`editor/window/widget/HtmlCssLexer.{h,cpp}`).
  It is a pure function with no ImGui, so it can be unit-tested (§5.1). The per-line carried
  state (the one used for multi-line comments) gains `inHtmlComment`, `inHtmlTag`,
  `inStyleBlock` and `inCssComment`.

  | Language | Token | Colour class |
  |---|---|---|
  | Html | `<!-- … -->` | comment |
  | Html | `<`/`</` + tag name | keyword |
  | Html | attribute names (`[A-Za-z_:][-A-Za-z0-9_:.]*`) | type |
  | Html | quoted attribute values | string |
  | Html | `&name;` entities | builtin |
  | Html | `<style>…</style>` content | tokenized as CSS |
  | Css | `/* */` | comment |
  | Css | at-rules `@media`, `@keyframes`, `@font-face`, `@import` | preprocessor |
  | Css | selectors `.class`, `#id`, `:pseudo` | builtin |
  | Css | property names (hyphenated identifier followed by `:`) | type |
  | Css | numbers with units (`px em rem % deg s ms dp vw vh`) and `#rgb[a]`/`#rrggbb[aa]` | number |
  | Css | strings | string |
  | Css | `!important` | keyword |
- **Suggestions.** A static list of RCSS property names from RmlUi's
  `StyleSheetSpecification` property registrations (`Source/Core/StyleSheetSpecification.cpp`)
  is generated into a header by `tools/gen_rcss_properties.py` (new, p1-editor) and offered
  as suggestions in Css and in Html `style` attributes.
- **Save → reload.** `CodeEditor::save()` (1402-1430) calls
  `project->notifyHtmlUIFileSaved(path)` after a successful save. That makes the watcher
  (§3.8.6) check now instead of waiting for the next poll.

#### 3.8.6 Hot reload (edit and play mode)
**Engine side** (`engine/core/subsystem/UIDocumentSystemReload.cpp` +
`engine/core/htmlui/HtmlUIDependencies.{h,cpp}`):
```cpp
// HtmlUIDependencies: static scanner (no RmlUi), also used by the exporter check
struct HtmlUIReference { std::string path; /* project-relative, normalized */ std::string raw;
                         enum Kind { STYLESHEET, IMAGE, FONT, SVG, LOTTIE, TEMPLATE, OTHER } kind;
                         std::string fromFile; int line, column; bool remote; bool missing; bool outsideAssets; };
std::vector<HtmlUIReference> collectHtmlUIReferences(const std::string& documentPath); // recursive through stylesheets/@import/templates
```
- **Scanner.** In HTML it reads `link[href]`, `img[src]`, `svg[src]`, `lottie[src]`,
  `template[src]`, `@import` and `url(...)` inside `<style>` and `style=""`. In CSS it reads
  `@import`, `url(...)`, `@font-face src`, `decorator: image(...)`/`svg(...)`.
- **Runtime file log.** `RmlFile::Open` records every path a document opens while it loads,
  through a scope guard set by the load call. `RmlRenderer::LoadTexture` and
  `RmlFontEngine::LoadFontFace` record lazy loads into a per-context set.

`UIDocumentSystem` methods:
```cpp
std::vector<std::string> getDocumentFiles(Entity) const;   // document + union(scanner, runtime log)
void reloadDocument(Entity, bool preserveState = true);
void reloadStyleSheets(Entity);
void reloadTexture(const std::string& path);              // Rml::RenderManager::ReleaseTexture(path) on every context
const std::vector<HtmlUIMessage>& getDocumentMessages(Entity) const;  // HtmlUIMessage{ level, text, file, line }
```

What a change reloads:

| Changed file | Action |
|---|---|
| A `.css`/`.rcss` only | `ElementDocument::ReloadStyleSheet()`. The DOM, element handles, listeners, scroll and focus are untouched. |
| An image | `reloadTexture`. |
| The document, a template, an SVG, a Lottie file or a font | Full reload with state preservation. Fonts also call `RmlFontEngine::reloadFontFile(path)`, an additive phase-0 file change. |

Full reload with state preservation:
1. **Capture**, for every element with an `id`:
   - scroll left/top;
   - form control value / checked / selected index;
   - the focused element's id and its selection range.
2. **Unload, then load.** Data models survive because they belong to the context.
3. **Restore** by id.
4. **Re-attach listeners.**
   - Lua and C++ listeners on elements with an `id`, and on the document or body, are
     re-attached to the element with the same id. p1-lua exposes
     `HtmlUIScriptState::onDocumentReloaded(old→new id map)`.
   - Listeners on elements without an id are dropped; one Console warning gives their count
     and the hint "give the element an id or rebind in onLoad".
5. **Notify.** `onUnload`/`onLoad` fire.
6. **Animations** that targeted removed elements skip those targets (§3.11).

**Editor side** (`editor/util/HtmlUIWatcher.{h,cpp}`, new):
- It is called from `App::engineRender` next to `project.refreshLinkedMaterials()`
  (App.cpp:2460).
- It throttles to 0.25 s, like `materialRefreshIntervalSec` (`Project.h:721-723`), or runs
  immediately after `notifyHtmlUIFileSaved`.
- It walks every open scene project, playing runtime clones included, and every entity with
  `UIDocumentComponent`. It stats `getDocumentFiles(entity)` with `fs::last_write_time` and
  applies the action above per changed file.
- It runs in edit and play mode. Unlike `CodeEditor::checkExternalScriptChanges`
  (630-690), it does not skip while playing. An edit-mode change also sets
  `needUpdateRender` so the Scene view redraws.
- **Scene view in edit mode.** `SceneRender::updateRenderSystem` (`SceneRender.cpp:731-741`)
  and the inline child scene update (App.cpp:2643-2656) also call
  `scene->getSystem<UIDocumentSystem>()->update(0)`.
  - `SceneRender2D::update` (825) draws the selected document's canvas rectangle as an
    outline, like other 2D gizmos.
  - Documents are not pickable in the Scene view; they are selected in the Hierarchy.
  - In edit mode documents load and lay out, and run CSS transitions and `@keyframes`. No
    Lua runs.

#### 3.8.7 RmlUi debugger
- **Building it.** `engine/CMakeLists.txt` option `JASPERENGINE_HTMLUI_DEBUGGER`, default ON
  for the editor build and for `CMAKE_BUILD_TYPE=Debug` exports, OFF otherwise. It compiles
  `engine/libs/rmlui/Source/Debugger` and defines `JASPERENGINE_HTMLUI_DEBUGGER`.
- **API.**
  - `UIDocumentSystem::setDebuggerVisible(bool)` calls `Rml::Debugger::Initialise(context)`
    once per process, then `SetContext(this context)` and `SetVisible`.
  - `UIDocumentSystem::isDebuggerVisible()` reports the state.
  - The debugger is global, so showing it in one scene hides it elsewhere.
  - Without the define, both methods log one warning and return false.
- **Editor.** The Game view toolbar gets a toggle button "HTML UI Debugger" (playing only).
  The Inspector button calls the same method for that document's scene. MCP gets no new
  tool; phase 4 owns MCP.
- **Lua.** `UIDocument.setDebuggerVisible(visible)` is a static method on the main scene's
  system.
- The debugger loads its own embedded font, which phase 0's in-memory
  `LoadFontFace(Span<const byte>)` supports.

#### 3.8.8 Exporter
The exporter copies the whole assets tree (`Exporter.cpp:1587-1657`) and packs it
(`writeNativeResourcePack`, 1084). Documents and everything they reference inside `assets/`
therefore ship with no copy step.

Phase 1 adds **`Exporter::checkHtmlUIReferences()`**, run in `runExport` (527-610) before
`copyAssets`. For every scene in the export and every `UIDocumentComponent` with a `path`,
it runs `collectHtmlUIReferences`. It **fails the export** (`setError`) listing each
`file:line:column` with:
- a missing file;
- a path that resolves outside the assets directory;
- a remote `http(s)://` URL, with the hint "download it into the project's assets
  directory (phase 2 adds importers)".

It **warns** (`Out::warning`) for absolute paths. Lottie `.json` files are also copied into
the Lua tree (`isLuaExportFile`, 706-716). That is harmless; the runtime reads them through
`asset://`.

**MSAA project setting.** `htmlUiMsaaSamples` (1/2/4/8, default 4) is stored with the other
project settings in `Project`. It is shown in the Project Settings window and settable
through MCP `set_project_settings`. Exports write it as
`HtmlUIContext::setPreferredSampleCount(n)` into the generated init code (`Generator.cpp`);
the editor applies it directly when a project opens or the setting changes.

---

### 3.9 Scripting (p1-lua)

#### 3.9.1 C++ public API (wave-0 headers, defined by p1-lua)
`engine/core/htmlui/UIValue.h`:
```cpp
class UIValue {                       // JSON-like tree used by data models (and Lua conversion)
public:
    enum class Type { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT };
    UIValue(); UIValue(bool); UIValue(double); UIValue(int); UIValue(const char*); UIValue(std::string);
    static UIValue array(); static UIValue object();
    Type getType() const; bool isNil() const;
    bool asBool() const; double asNumber() const; const std::string& asString() const;
    size_t size() const;                                   // ARRAY/OBJECT
    UIValue& operator[](size_t);  UIValue& operator[](const std::string&);   // auto-vivify
    const UIValue* find(const std::string&) const;
    void push(UIValue); void erase(size_t); void erase(const std::string&);
    std::vector<std::string> keys() const;                 // insertion order
};
```
`engine/core/htmlui/UIEvent.h`:
```cpp
class UIEvent {                       // valid only during dispatch; afterwards every getter returns defaults
public:
    bool isValid() const;
    std::string getType() const;                  // DOM name (see 3.9.3)
    UIElement getTarget() const;  UIElement getCurrentTarget() const;
    int getEventPhase() const;                    // 1 capture, 2 target, 3 bubble
    void stopPropagation(); void stopImmediatePropagation();
    void preventDefault(); bool isDefaultPrevented() const;
    std::string getKey() const; std::string getCode() const; int getKeyCode() const; bool isRepeat() const;
    bool getCtrlKey() const; bool getShiftKey() const; bool getAltKey() const; bool getMetaKey() const;
    float getClientX() const; float getClientY() const; int getButton() const;   // DOM button numbering
    float getDeltaX() const; float getDeltaY() const;
    std::string getData() const;                  // textinput / IME
    UIValue getDetail() const;                    // custom events (dispatchEvent)
private: std::shared_ptr<struct UIEventState> state;
};
```
`engine/core/htmlui/UIElement.h`:
```cpp
struct UIRect { float x, y, width, height, top, left, right, bottom; };
class UIElement {
public:
    UIElement();                                  // invalid handle
    bool isValid() const;                         // false once the element or its document is gone
    bool operator==(const UIElement&) const;
    // identity / tree
    std::string getId() const; void setId(const std::string&);
    std::string getTagName() const;               // lower case
    std::string getClassName() const; void setClassName(const std::string&);
    UIElement getParentElement() const;
    std::vector<UIElement> getChildren() const;   // element children only
    UIElement getFirstElementChild() const; UIElement getLastElementChild() const;
    UIElement getNextElementSibling() const; UIElement getPreviousElementSibling() const;
    int getChildElementCount() const;
    UIElement querySelector(const std::string&) const;
    std::vector<UIElement> querySelectorAll(const std::string&) const;
    UIElement closest(const std::string&) const; bool matches(const std::string&) const;
    bool contains(const UIElement&) const;
    // mutation
    UIElement appendChild(const UIElement& child);              // moves; returns child
    UIElement insertBefore(const UIElement& child, const UIElement& reference);
    UIElement removeChild(const UIElement& child);              // child stays usable (detached pool)
    UIElement replaceChild(const UIElement& newChild, const UIElement& oldChild);
    void remove();
    UIElement cloneNode(bool deep) const;
    // content
    std::string getTextContent() const; void setTextContent(const std::string&);   // escapes markup
    std::string getInnerHTML() const;   void setInnerHTML(const std::string&);
    // attributes
    std::string getAttribute(const std::string&) const; bool hasAttribute(const std::string&) const;
    void setAttribute(const std::string&, const std::string&); void removeAttribute(const std::string&);
    std::map<std::string, std::string> getAttributes() const;
    // classList / style / dataset
    void addClass(const std::string&); void removeClass(const std::string&);
    bool toggleClass(const std::string&); bool toggleClass(const std::string&, bool force);
    bool containsClass(const std::string&) const;
    std::string getStyle(const std::string& property) const;   // computed value as CSS text
    void setStyle(const std::string& property, const std::string& value);   // inline; "" removes
    void removeStyle(const std::string& property);
    std::string getDataset(const std::string& camelKey) const; void setDataset(const std::string&, const std::string&);
    // form controls
    std::string getValue() const; void setValue(const std::string&);
    bool getChecked() const; void setChecked(bool); bool getDisabled() const; void setDisabled(bool);
    // focus / interaction / scroll / geometry
    void focus(); void blur(); void click();
    void scrollIntoView(bool alignToTop = true);
    void scrollTo(float x, float y, bool smooth = false); void scrollBy(float dx, float dy, bool smooth = false);
    float getScrollTop() const; void setScrollTop(float); float getScrollLeft() const; void setScrollLeft(float);
    float getScrollWidth() const; float getScrollHeight() const;
    float getClientWidth() const; float getClientHeight() const;
    float getOffsetWidth() const; float getOffsetHeight() const; float getOffsetLeft() const; float getOffsetTop() const;
    UIRect getBoundingClientRect() const;
    // events (C++)
    uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false);
    void removeEventListener(uint64_t listenerId);
    bool dispatchEvent(const std::string& type, const UIValue& detail = UIValue(), bool bubbles = true); // false if prevented
    // lottie control (implemented by p1-vector, 3.10.3; logs an error on non-<lottie> elements)
    void play(); void pause(); void stop(); void seek(float seconds); void setFrame(float frame);
    void setSpeed(float speed); void setDirection(int direction); void setLoop(bool loop); void setLoopCount(int count);
    void setSegment(float startFrame, float endFrame); void setMarker(const std::string& marker);
    bool isPlaying() const; float getCurrentFrame() const; float getTotalFrames() const; float getDuration() const;
private: std::shared_ptr<struct UIElementRef> ref;      // holds Rml::ObserverPtr<Rml::Element> + detached-pool token
};
```
`engine/core/htmlui/UIDataModel.h`:
```cpp
class UIDataModel {
public:
    bool isValid() const; std::string getName() const;
    UIValue get(const std::string& path) const;                 // "items[2].name"
    void set(const std::string& path, const UIValue& value);    // dirties the top-level variable
    void dirty(const std::string& topLevelName); void dirtyAll();
    void bindEvent(const std::string& name, std::function<void(UIEvent&, const std::vector<UIValue>& args)> fn);
    uint64_t onChange(const std::string& topLevelName, std::function<void(const UIValue&)> fn); // after a data-value write
    void removeOnChange(uint64_t id);
private: std::shared_ptr<struct UIDataModelState> state;
};
```
`UIDocument` (`engine/core/object/ui/UIDocument.{h,cpp}`, phase-0 wrapper, extended by
p1-lua):
```cpp
UIElement getElementById(const std::string&) const;
UIElement querySelector(const std::string&) const;  std::vector<UIElement> querySelectorAll(const std::string&) const;
UIElement getBody() const;  UIElement createElement(const std::string& tag) const;  UIElement createTextNode(const std::string&) const;
std::string getTitle() const; void setTitle(const std::string&);
bool isLoaded() const; void load(const std::string& path); void reload();   // reload keeps state (3.8.6)
void show(); void hide(); bool isVisible() const;
uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false); // on the document element
void removeEventListener(uint64_t);
UIDataModel createDataModel(const std::string& name, const UIValue& initial);  // object; top-level keys fixed
UIDataModel getDataModel(const std::string& name) const;
void removeDataModel(const std::string& name);
Framebuffer* getTargetTexture() const;                                         // TEXTURE mode (3.12)
static void setDebuggerVisible(bool); static bool isDebuggerVisible();
```

#### 3.9.2 Lifetimes
- **Element handles.** A `UIElementRef` holds `Rml::ObserverPtr<Rml::Element>`. `Element`
  is `EnableObserverPtr<Element>` (`Include/RmlUi/Core/Element.h:47`), so the pointer
  becomes null when the element is destroyed, document unload included.
  - From C++, every method on an invalid handle logs `"UIElement: element no longer exists
    (its document was unloaded or it was removed)"` once per call site and returns a
    default.
  - From Lua, the same message is raised with `luaL_error`. Only `isValid()` is safe on a
    dead handle.
  - Raising errors is done in the binding lambdas *before* any C++ object with a destructor
    is alive. Lua is compiled as C, so `luaL_error` longjmps past C++ frames
    (`LuaBinding.cpp:230`).
- **Detached pool.** `createElement` / `removeChild` / `remove` / `replaceChild` /
  `cloneNode` return elements that no DOM owns (`Rml::ElementPtr`).
  - `HtmlUIScriptState` keeps them in a per-document pool `{ElementPtr, std::shared_ptr<int>
    token}`, and handles copy the token.
  - At the end of each `UIDocumentSystem::update`, entries whose `token.use_count() == 1`
    (no handle left) are destroyed. Re-inserting moves the `ElementPtr` back into the DOM.
  - On document unload the whole pool is freed.
- **Listeners.** `HtmlUIEventListener : Rml::EventListener` holds either a
  `std::function` or a Lua registry ref (`luaL_ref`). It is deleted in `OnDetach`.
  - The Lua ref is released with `luaL_unref` only if `LuaBinding::getLuaState()` is still
    the state that created it. `LuaBinding::cleanup()` closes the state and sets it to
    NULL (`LuaBinding.cpp:531-537`) **before** `removeAllSceneLayers` in
    `Engine::systemShutdown` (`Engine.cpp:1341-1346`), so documents destroyed after that must not
    touch it.
  - Lua listeners run through `LuaBinding::pcallWithTraceback` (`LuaBinding.h:73`); errors
    go to the Console with the traceback.
  - Duplicate `addEventListener(type, sameFunction, sameCapture)` is ignored, as in the DOM
    (compared with `lua_rawequal`).
  - `removeEventListener(type, fn, capture)` finds the listener by the same comparison. The
    per-element registry is `std::unordered_map<Rml::Element*, std::vector<HtmlUIEventListener*>>`,
    cleaned in `OnDetach`.
  - A debug counter `HtmlUIScriptState::liveLuaRefs()` is exposed for tests.
- **Play stop.** The Lua state is never recreated between plays (`Project.cpp:7195`,
  7663-7719). Documents are destroyed with their scenes, which releases every listener and
  data-model ref. A test checks that `liveLuaRefs()` is 0 after stop.
- **Events.** A `UIEvent` handle wraps a `UIEventState` that is invalidated when
  `ProcessEvent` returns.

#### 3.9.3 Events: names and fields
| DOM name used by scripts | RmlUi event | Notes |
|---|---|---|
| `click`, `dblclick`, `mousedown`, `mouseup`, `mousemove`, `mouseover`, `mouseout`, `keydown`, `keyup`, `focus`, `blur`, `scroll`, `resize`, `load`, `unload`, `submit`, `animationend`, `transitionend`, `dragstart`, `drag`, `dragend`, `dragover`, `dragdrop` | same | |
| `wheel` | `mousescroll` | `deltaX/deltaY` from `wheel_delta_x/y` |
| `input` | `change` | RmlUi fires `change` on every edit of text controls and on toggles |
| `change` | synthesized | For text controls: on `blur` if the value differs from the value at `focus`. For checkbox/radio/select/range: on `change`. |
| `textinput` | `textinput` | `data` = inserted text |
| `mouseenter`, `mouseleave`, `pointer*`, `touch*`, `contextmenu`, `beforeinput`, `compositionstart/update/end` | none | `addEventListener` logs a warning naming the supported events; it never fails silently |
| any other name | same name | custom events via `dispatchEvent` |

Fields:
- `button`: DOM numbering. RmlUi 0→0, 1 (right)→2, 2 (middle)→1.
- `clientX/clientY`: from `mouse_x/mouse_y`, in document pixels.
- `key`: the produced character for printable keys with shift applied (US layout), else DOM
  names (`"Enter"`, `"Escape"`, `"ArrowLeft"`, `"Backspace"`, `"Tab"`, `" "`, `"Shift"`, …).
- `code`: `"KeyA"`, `"Digit1"`, `"Space"`, `"ShiftLeft"`, ….
- `keyCode`: the engine `D_KEY_*`.
- Modifier booleans come from `ctrl_key/shift_key/alt_key/meta_key`.

#### 3.9.4 Data binding
**Storage.** One `UIValue` tree per model. RmlUi accesses it through
`HtmlUIValueDefinition : Rml::VariableDefinition` (Get/Set/Size/Child over the tree), modelled
on RmlUi's own Lua plugin (`Source/Lua/LuaDataModel.cpp`, `LuaObjectDef` lines 116-260).
- Top-level variables are bound with `BindCustomDataVariable(name, DataVariable(def,
  &stableStorage[name]))`. `stableStorage` is
  `std::map<std::string, std::unique_ptr<UIValue>>`, so pointers stay stable.
- Nested addresses are re-resolved through `Child` on every RmlUi update, so pointers into
  arrays are never held.

**Creating from Lua.**
```lua
local model = doc:createDataModel("shop", { gold = 10, items = { {name="Sword", price=5} }, buy = function(event, index) ... end })
```
- Function values become event callbacks (`BindEventCallback`, args converted
  `Rml::Variant` → Lua). Everything else becomes the `UIValue` tree. Top-level keys are fixed
  at creation; assigning a new top-level key raises an error naming the key.
- The return value is a **proxy table**: a userdata with `__index`, `__newindex`, `__len`,
  `__pairs`, `__ipairs` over the tree.
  - `model.gold = 20` writes the tree and calls `DirtyVariable("gold")`.
  - `model.items[1].price = 7` goes through nested proxies and dirties `"items"`.
  - Lua 1-based indices map to 0-based `UIValue` arrays.
  - `table.insert(model.items, {...})` and `table.remove` work, because Lua 5.4's table
    library honours `__index`/`__newindex`/`__len`.
  - Assigning a table converts it deeply.
- **Two-way binding.** A `data-value` / `data-checked` write from RmlUi
  (`VariableDefinition::Set`) updates the tree. Lua reads it on the next access, and
  `model:onChange(name, fn)` callbacks fire after the RmlUi update.

**C++.** `UIDataModel` methods (§3.9.1) operate on the same tree.

**Lifetime.**
- A model belongs to the document that created it and is removed (`Context::RemoveDataModel`)
  when that document unloads for good. Hot reload keeps it.
- Proxies and `UIDataModel` handles hold a weak reference. Access after removal raises
  `"data model '<name>' was removed"`.

**Ordering.** RmlUi binds `data-model` while parsing (`XMLNodeHandlerDefault`), so a model
must exist before the document is parsed. Documents load in `UIDocumentSystem::load()`, so
`init` can query elements.
- `createDataModel` on a loaded document whose source contains `data-model="<name>"`
  reloads that document synchronously inside the call, with state preservation (§3.8.6).
- Handles obtained before the call are then invalid; the guide says "create data models
  first in `init`".
- A model created before the first load (C++ code that runs before scene load) is used
  directly.

#### 3.9.5 Lua bindings (`engine/core/script/binding/HtmlUIClassesLua.cpp`, new)
**Registration.**
- `static void registerHtmlUIClasses(lua_State *L);` after `LuaBinding.h:48`.
- The call `registerHtmlUIClasses(L);` after `LuaBinding.cpp:479`.
- The source after `engine/CMakeLists.txt:862`.

**Classes.**
- `UIDocument` is a `deriveClass<UIDocument, GameObject>` with constructors `(Scene*)` and
  `(Scene*, Entity)`, like `Image` at `GameObjectClassesLua.cpp:1126-1158`.
- `UIElement`, `UIEvent`, `UIRect` and `UIDataModel` are value classes.
- Properties use `addProperty` with getter and setter. For `UIElement`: `id`, `tagName`,
  `className`, `textContent`, `innerHTML`, `parentElement`, `children` (array table),
  `firstElementChild`, `lastElementChild`, `nextElementSibling`, `previousElementSibling`,
  `childElementCount`, `value`, `checked`, `disabled`, `scrollTop`, `scrollLeft`,
  `scrollWidth`, `scrollHeight`, `clientWidth`, `clientHeight`, `offsetWidth`,
  `offsetHeight`, `offsetLeft`, `offsetTop`.
- **`classList`, `style` and `dataset`** are small bound value classes holding the element
  handle:
  - `UIClassList` with `add(...)` (varargs), `remove(...)`, `toggle(name[, force])`,
    `contains`;
  - `UIStyle` with `__index`/`__newindex` metamethods mapping camelCase (`backgroundColor`)
    and hyphenated (`["background-color"]`) names to `getStyle`/`setStyle`, plus
    `getPropertyValue`, `setProperty`, `removeProperty`;
  - `UIDataset` with `__index`/`__newindex` over `data-*`.
- `addEventListener(type, fn [, captureOrOptions])` accepts a boolean or `{capture=bool,
  once=bool}`; `once` removes the listener after its first call.
- `UIEvent` properties: `type`, `target`, `currentTarget`, `eventPhase`, `key`, `code`,
  `keyCode`, `repeat`, `ctrlKey`, `shiftKey`, `altKey`, `metaKey`, `clientX`, `clientY`,
  `button`, `deltaX`, `deltaY`, `data`, `detail` (Lua value), `defaultPrevented`. Methods:
  `stopPropagation()`, `stopImmediatePropagation()`, `preventDefault()`.
- `UIDocument`: the methods of §3.9.1, plus the `body` and `title` properties, and
  `onLoad`/`onUnload` as `FunctionSubscribe` properties, as in the `UIComponent` events
  pattern (`ECSClassesLua.cpp:341-352`).
- Lua callbacks are stored as registry refs, never through `FunctionSubscribe`'s
  `LuaFunction`. Its `push_value` overloads cannot push a `UIEvent`
  (`LuaFunctionBase.h:582-597`), and its destructor leaks refs (`LuaFunctionBase.cpp:45-52`
  tests the stack top instead of the ref). Fix that destructor to `luaL_unref` when
  `m_func != LUA_NOREF` and the state is alive. This is a one-line correctness fix in a
  shared file, and the listener tests cover it.

**Stubs and suggestions.**
- `generate_lua_stubs.py` and `generate_api_suggestions.py` glob the binding directory
  (`CMakeLists.txt:363-394`), so the new file is picked up. Every bound type is declared in
  a header under `engine/core/`, so no `-- unresolved:` lines appear
  (`tools/mcp/tests/test_stubs.py:259-269`).
- **Changes to `generate_lua_stubs.py`** (owned by p1-lua):
  1. `_PARAM_TYPE_OVERRIDES` gives precise LuaLS types to `luabridge::LuaRef` parameters,
     which otherwise become `any`:
     - `UIElement.addEventListener.listener` and `UIDocument.addEventListener.listener` →
       `fun(event: UIEvent)`;
     - `createDataModel.initial` → `table`;
     - the anime parameter tables → the `Anime*Params` classes.
  2. Hand-written stub fragments from `engine/core/script/binding/stubs/*.lua` are appended
     to `stubFiles`. They hold `---@class` shapes for parameter tables; p1-anim adds
     `anime.lua` there.
- `editor/Generator.cpp:1504-1513` (`engineApiIncludeDirs`) gains `core/htmlui`.
- The Xcode project (`engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj`, which
  lists sources explicitly, e.g. lines 91-92) gets every new engine source of every package.
  Each package adds its own files there.

---

### 3.10 Vector graphics with ThorVG (p1-vector)

#### 3.10.1 Vendoring
**Source.** ThorVG 1.2.0 from `/tmp/htmlui-src/thorvg` goes to `engine/libs/thorvg/` with a
hand-written `CMakeLists.txt`; ThorVG ships meson.
- Copy `inc/thorvg.h`, `src/common`, `src/renderer/*.cpp|h` (not `gpu_engine`),
  `src/renderer/cpu_engine`, and the loaders `svg`, `lottie` (without `jerryscript`), `png`,
  `jpg`, `raw` and `sfnt`. Add `LICENSE`.
- Generated `config.h` defines:
  - `THORVG_VERSION_STRING "1.2.0"`;
  - `THORVG_CPU_ENGINE_SUPPORT`;
  - `THORVG_SVG_LOADER_SUPPORT`, `THORVG_LOTTIE_LOADER_SUPPORT`, `THORVG_PNG_LOADER_SUPPORT`,
    `THORVG_JPG_LOADER_SUPPORT`, `THORVG_SFNT_LOADER_SUPPORT`, `THORVG_TTF_LOADER_SUPPORT`,
    `THORVG_OTF_LOADER_SUPPORT`.
- Deliberately not defined:
  - `THORVG_THREAD_SUPPORT`: no worker threads, so the output is deterministic for goldens,
    and Web needs no pthreads;
  - `THORVG_FILE_IO_SUPPORT`: all data goes through the engine's `Data`/`ResourcePack`;
  - `THORVG_LOTTIE_EXPRESSIONS_SUPPORT`, OpenMP, the GL/WG engines, `THORVG_LOG_ENABLED`.

**Build.**
- Static library, `-fvisibility=hidden` like `libs/harfbuzz/CMakeLists.txt:40`, `TVG_STATIC`
  defined.
- Wiring in `engine/CMakeLists.txt`:
  - `include_directories(SYSTEM libs/thorvg/inc)` at ~673;
  - `add_subdirectory(libs/thorvg)` at ~706;
  - the warnings foreach (739-743);
  - the link list (941-948).
- The `rmlui` SVG plugin target links `thorvg` instead of `lunasvg`.
- Record it in `NOTICE.md` (MIT).

**Initialisation.** `HtmlUIContext::initialise` calls `tvg::Initializer::init(0)`;
`shutdown` calls `term()`. `TextureData` calls `init` lazily through the same ref-counted
helper `ThorVGRuntime::acquire/release` (`engine/core/util/ThorVGRuntime.{h,cpp}`), because
textures load without HTML UI.

**Fonts for SVG `<text>` and Lottie text layers.** On first acquire, `ThorVGRuntime`
registers the engine's built-in fonts (`DefaultFont.h`, `DefaultFontArabic.h`) with
`tvg::Text::load("sans-serif", data, size, "ttf", false)`.

**Images inside SVG and Lottie.** `Picture::resolver` maps relative paths through the
engine's file layer: document directory first, then the `.lottie` archive.

#### 3.10.2 SVG (fork patch `rmlui: SVG plugin rasterises with ThorVG`)
In `engine/libs/rmlui/Source/SVG/SVGCache.cpp`, replace the LunaSVG calls (lines 16, 80,
118-125, 150-260):
- `lunasvg::Document` → `std::unique_ptr<tvg::Picture>`, loaded with
  `picture->load(data, size, "svg", nullptr, true)`;
- intrinsic size from `picture->size(&w, &h)`;
- `crop_to_content` from `picture->bounds(&x, &y, &w, &h)`;
- rendering: `tvg::SwCanvas::gen()`, `target(buffer, w, w, h, ColorSpace::ABGR8888)`
  (premultiplied, byte order R,G,B,A on little-endian), push a duplicate of the picture
  scaled with `size(w, h)` or translated/scaled for crop, then `draw(true)` + `sync()`. No
  channel swap is needed, unlike LunaSVG's BGRA (line 245-249).

Tinting (`image-color` × `opacity`, line 370), the cache keys, `ElementSVG` (inline `<svg>`
via `rmlui-svgdata-id`), `DecoratorSVG` and `XMLNodeHandlerSVG` are unchanged.

**Fallback.** If the phase-0 benchmark chose **LunaSVG for SVG**:
- `SVGCache.cpp` stays upstream and links the vendored `engine/libs/lunasvg`;
- ThorVG is built with `THORVG_SVG_LOADER_SUPPORT` off and serves Lottie only;
- `TextureData` (§3.10.4) uses LunaSVG instead of ThorVG;
- the fork patch is not made, and `FORK.md` says so.

Everything else in this section is unchanged by the fallback.

#### 3.10.3 `<lottie>` element (engine code; RmlUi's rlottie plugin is not built)
Files:
- `engine/core/htmlui/vector/UILottieElement.{h,cpp}`: `class UILottieElement : public
  Rml::Element`, registered by `HtmlUIContext` with
  `Rml::Factory::RegisterElementInstancer("lottie", ...)`.
- `engine/core/htmlui/vector/DotLottie.{h,cpp}`.
- `engine/core/htmlui/vector/LottiePlayback.{h,cpp}`: a pure, RmlUi-free playback state
  machine, unit-tested on its own.

Attributes (HTML spelling; values re-read in `OnAttributeChange`):

| Attribute | Values | Default |
|---|---|---|
| `src` | `.json` or `.lottie`, relative to the document | — |
| `autoplay` | boolean attribute | absent = paused at the first frame |
| `loop` | boolean, or a count (`loop="3"`) | no loop |
| `speed` | float > 0 | 1 |
| `direction` | `1` / `-1` | 1 |
| `mode` | `normal` / `bounce` (ping-pong) | normal |
| `segment` | `"start end"` in frames | whole animation |
| `marker` | marker name (`LottieAnimation::segment(const char*)`) | — |
| `animation` | id inside a `.lottie` | manifest's first/`activeAnimationId` |

Behaviour:
- **Layout.** `GetIntrinsicDimensions` = the picture size × dp ratio, as RmlUi's
  `ElementLottie` does (`Source/Lottie/ElementLottie.cpp:20-31`).
- **Clock.** Playback time comes from `RmlSystem::GetElapsedTime()`, which is the engine's
  scaled game time, so pause and time scale apply and golden tests can set it.
- **Frame choice.** `frame = segStart + fmod(t × speed × fps, segLen)`, with direction,
  bounce and loop count applied by `LottiePlayback`.
- **Texture.** **One** `HtmlUITexture{DYNAMIC}` per element (RGBA8, sized to the content
  box in pixels), created in the first `CallbackTexture` callback through
  `texture_interface.SetTextureHandle(handle, size)`
  (`Include/RmlUi/Core/CallbackTexture.h:61`).
  - When the frame index changes: `animation->frame(f)`, raster into a persistent CPU buffer
    (`SwCanvas` `ABGR8888`, premultiplied), then `updateTexture` **in place**.
  - Re-raster happens only on a frame change or resize; a resize creates a new texture of
    the new size and releases the old one.
  - This replaces the per-frame `MakeCallbackTexture` of RmlUi's plugin (lines 189-232).
- **Redraw requests.** `OnUpdate` requests the next update at the next frame boundary
  (`Context::RequestNextUpdate(delay)`) only while playing and visible.
- **Tint** via `image-color` × `opacity` on the quad colour, as for SVG.
- **Events** dispatched on the element: `play`, `pause`, `complete` (non-looping end, or the
  last loop of a count), `loop` (each wrap), `load`, and `error` (with `detail.message`).
  Errors are also logged.
- **Control API.** `UIElement::play/pause/stop/seek/setFrame/setSpeed/setDirection/setLoop/
  setLoopCount/setSegment/setMarker/isPlaying/getCurrentFrame/getTotalFrames/getDuration`
  (§3.9.1) are defined in `engine/core/htmlui/vector/UIElementLottie.cpp` by p1-vector.
  - Lua binding: `engine/core/script/binding/HtmlUIVectorLua.cpp` re-opens
    `beginClass<UIElement>` and adds the same methods. LuaBridge allows re-opening a class
    to add members.
  - Registration: a second call `registerHtmlUIVectorClasses(L)` after
    `registerHtmlUIClasses`.
- **`.lottie` (dotLottie)** in `DotLottie.cpp`:
  - a minimal ZIP reader: end-of-central-directory scan, central directory, local headers;
  - methods 0 (stored) and 8 (deflate, through
    `stbi_zlib_decode_noheader_buffer`/`_malloc` from the vendored `stb_image.h:534-535`);
  - reads `manifest.json` with the vendored `libs/json`: `animations[].id`,
    `activeAnimationId`, and v1/v2 manifest fields `autoplay`, `loop`, `speed`,
    `direction`, `playMode`. Element attributes override them;
  - loads `animations/<id>.json` (v1) or `a/<id>.json` (v2), and resolves `images/` / `i/`
    assets for `Picture::resolver`;
  - ZIP64, encryption and multi-disk archives are rejected with an error event.

#### 3.10.4 `TextureData` moves to ThorVG
`loadSvgTexture` (`TextureData.cpp:241-305`) becomes ThorVG:
- `Picture::load(data, size, "svg")`;
- size = intrinsic (96 dpi) × `?svgScale=`, capped at 16384 as today (264-273);
- `SwCanvas` with `ColorSpace::ABGR8888S`, **straight alpha**, matching the engine's
  non-premultiplied texture convention (`ui.frag` and materials). nanosvg also produced
  straight alpha;
- `malloc`'d buffer, `channels = 4`.

The `?svgScale=` parsing (40-66) and `looksLikeSvg` routing (186-195, 333-338) stay. Remove
the nanosvg includes (15-18) and `engine/libs/nanosvg/` with its include line
(`engine/CMakeLists.txt:673`).

**Reason:** one SVG implementation in the engine, with better SVG coverage (CSS styles,
masks, text) than nanosvg. **Regression test:** the four `.svg` assets in the test fixture
load at the same pixel size as with nanosvg, with alpha-weighted mean colour within 2/255.

---

### 3.11 Animation API (p1-anim)

The C++ core is `engine/core/htmlui/anim/`; it has no Lua dependency and is usable from
C++. The Lua face is `engine/core/script/binding/HtmlUIAnimeLua.cpp`, a global `anime`
table bound as `beginClass<Anime>("anime")` with static functions.

Ported from Anime.js 4.5.0 (`/tmp/htmlui-src/anime/src`, MIT):
- easings: `easings/eases/parser.js`, `easings/cubic-bezier`, `easings/steps`,
  `easings/linear`, `easings/spring`;
- timeline position parsing: `timeline/position.js`;
- stagger: `utils/stagger.js`;
- timer semantics: `timer/timer.js`;
- defaults: `core/globals.js:39-55`.

| File | Contents |
|---|---|
| `AnimEasing.{h,cpp}` | `using EaseFn = std::function<double(double)>`; `parseEase(const std::string&)` accepting `linear`, `none`, `in`, `out`, `inOut`, `outIn` (power, default 1.68, or `in(3)`), `in/out/inOut/outIn` + `Quad Cubic Quart Quint Sine Expo Circ Bounce Back(overshoot=1.7) Elastic(amplitude=1, period=.3)` with parameters in parentheses (`outElastic(1, .6)`), and also `cubicBezier(x1,y1,x2,y2)`, `steps(n[, fromStart])`, `linear(p0, p1 [pct], ...)`, `spring(...)`. Unknown names log a warning and use `linear`. Strings are accepted as an engine convenience; Anime 4.5 removed the string forms of `cubicBezier/steps/linear`, and the function forms are also provided. |
| `AnimSpring.{h,cpp}` | Port of `Spring` (`easings/spring/index.js`): parameters `mass, stiffness, damping, velocity` or `bounce, duration`; `solve`, `compute` (settling duration via rest threshold .0005, rest 200 ms, step .02), `onComplete` at the perceived duration. |
| `AnimValue.{h,cpp}` | Value decomposition: numbers with units, relative `+= -= *=`, colours (`#rgb[a]`, `#rrggbb[aa]`, `rgb()/rgba()/hsl()/hsla()`) interpolated per channel in non-premultiplied 0–255 RGBA as Anime does, and complex strings (numbers inside a template string). |
| `AnimTargets.{h,cpp}` | Targets: `UIElement`, a list of elements, a selector string (resolved against every loaded document of the current scene, or the `root` param), and plain tables through a `TableTarget` adapter interface implemented in the Lua binding. |
| `AnimProperty.{h,cpp}` | Writers. **Individual transforms** `translateX/Y/Z`, `rotate`, `rotateX/Y/Z`, `scale`, `scaleX/Y/Z`, `skew`, `skewX/Y`, `perspective` keep a per-element transform state and write one `Rml::Transform` built from `Rml::TransformPrimitive`s in Anime's order (insertion order) to `PropertyId::Transform`. `opacity` → `Rml::Property(float, Unit::NUMBER)`. Single lengths (`width height left top right bottom margin* padding* font-size border-*-width border-radius gap letter-spacing line-height`) → `Rml::Property(v, unit)`. Colours (`color background-color border-*-color image-color`) → `Rml::Property(Colourb)`. Anything else → the complex-string path and `Element::SetProperty(name, string)`. Unit mismatch between from and to is resolved through `Element::ResolveLength`/computed values into px. |
| `AnimTimer.{h,cpp}`, `AnimAnimation.{h,cpp}`, `AnimTimeline.{h,cpp}` | Playback: `duration` (default 1000 ms), `delay`, `loopDelay`, `loop` (count / `true` = infinite), `alternate`, `reversed`, `autoplay` (default true), `playbackRate`, `playbackEase`, `frameRate`, `ease` (default `out(2)`), `composition` (`replace` default, `none`, `blend` = additive sum of active numeric tweens on the same property), keyframes (value arrays; arrays of `{to, duration, ease, delay}`; percentage keyframes `{["0%"]={...}}`), `from/to` objects, function values `function(target, index, total)`. Callbacks `onBegin`, `onBeforeUpdate`, `onUpdate`, `onRender`, `onLoop`, `onPause`, `onComplete`. Methods `play, pause, resume, restart, reverse, alternate, seek(ms), stretch(ms), cancel, revert, complete, then(fn)`. Properties `currentTime, progress, iterationProgress, duration, paused, began, completed, reversed, id`. Timeline: `add(targets, params[, position])`, `add(timer)`, `set(targets, props[, position])`, `call(fn[, position])`, `label(name[, position])`, `sync(animation[, position])`, `remove(...)`, `init()`; positions per `parseTimelinePosition` (numbers, `"+=100"`, `"-=100"`, `"*=2"`, `"<"`, `"<<"`, `"label"`, `"label+=50"`), `defaults`. |
| `AnimEngine.{h,cpp}` | One process-wide engine. `tick(double gameTimeSeconds)` advances only when the time changed, so it is idempotent per frame. `reset()` drops everything. |

Lua surface:
- `anime.animate(targets, params)`, `anime.createTimeline(params)`,
  `anime.createTimer(params)`, `anime.createSpring(params)` (also `anime.spring` alias).
- `anime.stagger(value | {from, to}, {start, from = 'first'|'center'|'last'|'random'|index, reversed, ease, grid = {cols, rows}, axis = 'x'|'y', total})`
  returns a function value.
- Easing: `anime.eases.<name>(...)`, `anime.cubicBezier`, `anime.steps`, `anime.linear`.
- `anime.utils`: `set`, `get`, `remove`, `random`, `randomPick`, `shuffle`, `clamp`,
  `round`, `snap`, `wrap`, `mapRange`, `lerp`, `damp`, `degToRad`, `radToDeg`,
  `roundPad`, `padStart`, `padEnd`.
- Callbacks receive the animation object.
- **Plain table targets** animate numeric fields (`rawget`/`rawset`, numbers only; colour
  strings in tables are interpolated as colours and written back as `#rrggbbaa`).

**Driving.**
- Each `UIDocumentSystem::update(dt)` calls `AnimEngine::instance().tick(Engine::getGameTime())`
  **before** `context->Update()`. This is one line in the phase-0 `.cpp`. Pausing the engine
  pauses animations; time scale applies.
- Style writes do not touch layout for transform and opacity. Test:
  `document->IsLayoutDirty()` stays false after a tick that writes only transforms and
  opacity (`ElementDocument.h:146`).
- **Invalid targets.** Targets whose element handle became invalid are skipped. An animation
  whose targets are all invalid is cancelled without callbacks.
- **Lua refs** (callbacks, function values, table targets) are released by
  `AnimEngine::reset()`, which is called:
  - in `Engine::systemShutdown` before `LuaBinding::cleanup()` (`Engine.cpp:1344`);
  - in the editor's play stop before `LuaBinding::cleanupLuaScripts`
    (`editor/Project.cpp:7712`).

  Both are one-line additive hooks.

**Stubs.** `engine/core/script/binding/stubs/anime.lua` holds `---@class` shapes:
`AnimeAnimationParams`, `AnimeTimelineParams`, `AnimeTimerParams`, `AnimeSpringParams`,
`AnimeStaggerParams`, the `AnimeAnimation`/`AnimeTimeline`/`AnimeTimer` method docs, and
the `anime` table.

**Reference values.** `engine/tests/htmlui/anime/gen_reference.mjs` runs on the host with
node (`/usr/bin/node`, v20) against `/tmp/htmlui-src/anime/src`. It writes
`engine/tests/htmlui/anime/reference.json` (checked in), sampling every easing at 101
points, spring durations, stagger outputs and timeline offsets.

---

### 3.12 Render to texture and world-space input (p1-rtt)

#### 3.12.1 Contexts
Each `TEXTURE`-mode document gets its own `Rml::Context`, named `"tex:<sceneId>:<entity>"`,
with dimensions `textureWidth × textureHeight` and dp ratio `pixelRatio` (or 1). The context
belongs to `HtmlUITextureState`, which `UIDocumentSystem` holds.

**Reason:** a context renders all its documents into one target, and a texture document
needs its own size, target and input space.

Switching `renderMode` at runtime (`needReload`) moves the document between contexts by
reloading it, with state preservation.

#### 3.12.2 Target texture
- Each texture context renders through `RmlRenderer` into its own `RmlRenderTarget`, with
  the same MSAA and stencil as screen documents.
- At the end of its render, one `HTMLUI_POST` `Unp` pass copies `layers[0]` into
  `Framebuffer targetTexture` (RGBA8, 1 sample, no depth, LINEAR, CLAMP). It
  un-premultiplies, because consumers (`Image` through `ui.frag`, mesh materials) expect
  straight-alpha sRGB data.
- The image is top-left origin, the engine's RTT convention, so `Image::setTexture(Framebuffer*)`
  (`Image.cpp:132-138`) and `Mesh::setTexture(Framebuffer*)` (`Mesh.cpp:50-55`) show it
  upright.
- `Framebuffer::create()` bumps its version, so consumers rebind
  (`Texture::isFramebufferOutdated`).
- `UIDocument::getTargetTexture()` returns it (Lua `doc:getTargetTexture()`).
- Texture documents are never composited into the camera pass.

#### 3.12.3 World-space input
`engine/core/htmlui/UIWorldRaycast.{h,cpp}`:
```cpp
struct UIWorldHit { bool hit; float distance; Vector2 uv; };
UIWorldHit raycastMeshUV(Scene* scene, Entity meshEntity, const Ray& worldRay);
```
- The ray comes from `Camera::screenToRay(canvasX, canvasY)` (`Camera.cpp:607-640`) of the
  scene camera.
- The hit test uses the entity's `MeshComponent`:
  - the ray is transformed into local space with the inverse model matrix
    (`GameObject::getModelMatrix()`, `Matrix4::inverse()`);
  - for every submesh's triangles (`indices` from `IndexBuffer`, or non-indexed), it runs
    Möller–Trumbore against `POSITION`;
  - it interpolates `TEXCOORD1` at the nearest hit. Attributes are looked up in
    `MeshComponent::buffer` first, then in every `eBuffers[i]` (`Buffer::getAttribute`,
    `getVector3/getVector2`).
  - This covers primitives (plane, wall, box, sphere), sprites and imported meshes whose CPU
    buffers are resident. Skinned and morph meshes use the bind pose.
- Texture uv (0,0) is the top-left texel, so the document pixel is
  `(u × textureWidth, v × textureHeight)` divided by the dp ratio.

`UIDocumentSystemTexture.cpp` defines:
```cpp
bool dispatchWorldPointer(int pointer, int button, float canvasX, float canvasY, int mods, PointerPhase phase, bool touch);
```
p1-rtt adds one call to it in each `eventOnPointer*` in `UIDocumentSystemInput.cpp`
(p1-input's file). The call goes after the screen context was offered the event and did not
consume it, because screen-space UI is drawn over the 3D world.
- For every interactive texture document whose `inputSurface` (or own entity) has a
  `MeshComponent`, ordered by hit distance: a hit sends the event to that context at the
  mapped point.
- A miss while the document had hover sends `ProcessMouseLeave`.
- Capture: a document that received the down gets move/up until release, even off-surface,
  with the uv clamped.
- The consume rules are the same as §3.6.2. A captured pointer (down received by a world
  document) bypasses the screen context until release.
- Keys and text go to the context that holds focus: the context that received the last
  pointer-down, screen or texture.

---

### 3.13 Font effects (p1-rtt)

`RmlFontEngine` (phase 0) implements the effects path of `FontEngineInterface`. The FreeType
backend's `FontFaceLayer` / `FontFaceHandleDefault` (`Source/Core/FontEngineDefault/`) is
the reference; its glyph-iteration logic is ported and FreeType is not used.

- **`PrepareFontEffects(face, effects)`** returns a handle to a `FontEffectSet`, cached by
  `(face, effects fingerprint)` (`FontEffect::GetFingerprint`).
- **One `EffectLayer` per effect.**
  - For effects with `HasUniqueTexture()` (outline, glow, blur), the layer owns a glyph atlas
    (RGBA8, premultiplied). Each glyph is generated on demand from the STBText alpha bitmap,
    wrapped as `Rml::FontGlyph{bearing, advance, bitmap_data, bitmap_dimensions,
    ColorFormat::A8}` (`Include/RmlUi/Core/FontGlyph.h`). It is sized with
    `GetGlyphMetrics(origin, dims, glyph)` and filled with `GenerateGlyphTexture(dest, dims,
    stride, glyph)`.
  - Shadow (shared texture) reuses the base atlas with the effect's origin offset.
  - Atlas pages are `CallbackTexture`s created through `GenerateTexture`, pooled per layer,
    and regrown by repacking with `stb_rect_pack`.
  - Glyphs are keyed by HarfBuzz glyph index, not by code point.
- **`GenerateString(..., effectsHandle, ...)`** emits `Back`-layer effects first in list
  order, then the base text, then `Front`-layer effects. Each layer's quads are coloured with
  `effect->GetColour()` premultiplied × `opacity`, at the same pen positions as the shaped
  base run.
- **`GetVersion(face)`** increments when any layer atlas is regenerated, so RmlUi regenerates
  geometry.
- The four built-in RmlUi effects are registered by RmlUi Core: `shadow`, `outline`, `glow`,
  `blur`. Nothing extra is registered.

---

### 3.14 RmlUi fork patches in this phase
Each patch is a separate commit prefixed `rmlui:`, with a `docs/html-ui/FORK.md` entry
(file, reason, test).

1. **`rmlui: Event::PreventDefault`** (p1-lua).
   - Adds `void PreventDefault()` and `bool IsDefaultPrevented() const` to
     `Include/RmlUi/Core/Event.h` / `Source/Core/Event.cpp`.
   - `EventDispatcher.cpp` (default-action loop at lines 173-185) skips
     `ProcessDefaultAction` when the event is default-prevented, in addition to the existing
     `IsPropagating` check.
   - `Context::ProcessKeyDown/Up/TextInput` return "consumed" when prevented.
   - Test: `preventDefault()` in a `click` listener on a checkbox keeps it unchecked; in
     `keydown` on an input, the key is not inserted.
2. **`rmlui: SVG plugin rasterises with ThorVG`** (p1-vector, §3.10.2). It is not made under
   the LunaSVG fallback.

---

## 4. Work packages

### Waves
| Wave | Packages | Starts when |
|---|---|---|
| 0 | contract commit (§3.0), by the coordinator | phase 0 merged into `html-ui` |
| 1 | `p1-render`, `p1-input`, `p1-editor`, `p1-lua`, `p1-vector`, `p1-anim` | wave 0 merged |
| 2 | `p1-rtt` | starts with font effects (no dependency) in parallel with wave 1; begins §3.12 after `p1-render` and `p1-input` have merged |

**Merge order into `html-ui`:**
1. `p1-render`;
2. `p1-input`;
3. `p1-lua`;
4. `p1-vector`;
5. `p1-anim`, which needs `p1-lua`'s `UIElement` for its integration tests and merges
   `html-ui` before its final test run;
6. `p1-editor`, which needs `p1-lua`'s listener re-attachment for reload tests;
7. `p1-rtt`.

Each package merges `html-ui` into its branch before finishing (BRIEF).

Worktrees: `/var/lib/orquester/workspaces/jaspersito/JasperEngine-2-wt/<slug>` on branch
`html-ui/<slug>`.

---

### p1-render — renderer completeness
**Goal:** §3.1–§3.5. Every RmlUi render feature is available on every backend through
sokol, with MSAA, a pipeline cache and statistics.

**Owned files.**
- New: `engine/core/render/PipelineState.h`, `engine/renders/sokol/SokolPipelineCache.{h,cpp}`,
  `engine/core/subsystem/UIDocumentSystemRender.cpp`,
  `engine/core/htmlui/HtmlUIShaderEffects.{h,cpp}`.
- Shaders: `engine/shaderlib/htmlui_gradient.frag`, `htmlui_post.vert`, `htmlui_post.frag`,
  `htmlui_effect_test-pattern.frag`.
- Rewritten from phase 0: `engine/core/htmlui/RmlRenderer.{h,cpp}`.
- Changes: `engine/core/render/ObjectRender.{h,cpp}`, `FramebufferRender.{h,cpp}`,
  `CameraRender.{h,cpp}`, `SystemRender.{h,cpp}`,
  `engine/renders/sokol/SokolObject.{h,cpp}`, `SokolFramebuffer.{h,cpp}`,
  `SokolCamera.{h,cpp}`, `SokolShader.cpp` (cache release), `SokolTexture.cpp`
  (`createFramebufferTexture` only), `engine/core/texture/Framebuffer.{h,cpp}` (accepts a
  `FramebufferDesc`).
- The phase-0 composite function in `RenderSystem.cpp`.

**Additive touches.**
- `Render.h` (ShaderType append).
- `ShaderPool.{h,cpp}`, `ShaderVariantCompiler.cpp`, `ShaderVariantCollector.cpp`,
  `editor/Project.cpp` key list, `RenderSystem.cpp` (`getShaderVariants`; one line in
  `drawView` 10748-10752), `ExportWindow.cpp`, `CoreClassesLua.cpp` (`ShaderType`),
  `editor/App.cpp` (cache v30).
- `UIDocumentSystem.cpp` (one line in `draw()`).
- `HtmlUIContext` (`setPreferredSampleCount`/`getSampleCount`).
- `docker/linux.Dockerfile`: add `mesa-vulkan-drivers` so Vulkan tests run headless under
  lavapipe.
- `engine/workspaces/xcode/.../project.pbxproj` (new sources).

**Dependencies:** wave 0.

**Deliverables:** everything in §3.1–§3.5, plus a `docs/mcp/guide/html-ui.md` section
"Rendering features and limits" (MSAA setting, 16 gradient stops, filters list, shader
decorator names).

**Tests:** §5.1 renderer unit tests, §5.2 goldens G01–G18, G21 and G23–G24, on glcore
**and** vulkan; builds web and android.

---

### p1-input — input, clipboard, IME, virtual keyboard
**Goal:** §3.6–§3.7.

**Owned files.**
- New: `engine/core/subsystem/UIDocumentSystemInput.cpp`, `engine/core/htmlui/RmlKeyMap.{h,cpp}`,
  `engine/core/htmlui/RmlTextInputHandler.{h,cpp}`,
  `engine/platform/mac/MacTextInputClient.{h,mm}`, `engine/platform/apple/ios/KeyCodesIOS.h`.
- Changes:
  - `engine/core/Engine.cpp` input functions (1361-1590), plus the new declarations in
    `Engine.h`;
  - `engine/core/System.{h,cpp}` clipboard and text area;
  - all platform input files: `linux/LinuxInputRouter.cpp`, `JasperEngineLinux.cpp`,
    `SystemLinux.{h,cpp}`, `WindowLinux.cpp`; `win/WinInputRouter.cpp`, `SystemWin.{h,cpp}`,
    `JasperEngineWin.cpp`; `mac/MacInputRouter.mm`, `MacView{Metal,GL,Vulkan}.{h,mm}`,
    `SystemMac.{h,mm}`; `apple/JasperEngineApple.{h,mm}`, `apple/ios/EngineView.{h,mm}`;
    `android/NativeEngine.cpp`, `JasperEngineAndroid.{h,cpp}`,
    `android/java/.../MainActivity.java`; `emscripten/JasperEngineWeb.{h,cpp}`;
  - `editor/window/PlayInput.cpp`, `editor/Platform.{h,cpp}`;
  - `send_input` in `editor/mcp/tools/PlayTools.cpp`.

**Additive touches.**
- `RmlSystem.cpp` (clipboard, `ActivateKeyboard`).
- `HtmlUIContext` (installs the text input handler on context creation).
- `CoreClassesLua.cpp` (`System` clipboard functions).
- `engine/CMakeLists.txt` (`imm32` on Windows; new sources).
- Xcode project.

**Dependencies:** wave 0.

**Deliverables:** §3.6–§3.7 complete; `docs/mcp/guide/html-ui.md` "Input" section (consume
rules, `pointer-events: none`, text input, IME per platform, clipboard on Web);
regenerated MCP tool docs for `send_input`.

**Tests:** §5.1 input unit tests; §5.3 `test_html_ui_input.py`; builds linux (glcore), web,
android. Windows, macOS and iOS: §5.4 rules.

---

### p1-editor — editor integration and hot reload
**Goal:** §3.8.

**Owned files.**
- New: `engine/core/subsystem/UIDocumentSystemReload.cpp`,
  `engine/core/htmlui/HtmlUIDependencies.{h,cpp}`, `editor/util/HtmlUIWatcher.{h,cpp}`,
  `editor/window/widget/HtmlCssLexer.{h,cpp}`,
  `tools/gen_rcss_properties.py`, `editor/resources/icons/html-icon.png`, `css-icon.png`,
  `html-icon_png.h`, `css-icon_png.h`.
- Changes:
  - the UIDocument parts of `editor/Catalog.{h,cpp}`, `editor/Stream.{h,cpp}`,
    `editor/Factory.{h,cpp}`;
  - `InspectorWindow.{h,cpp}` drawer, `ComponentAddDialog.cpp`, `HierarchyWindow.{h,cpp}`,
    `CreateEntityCmd.{h,cpp}`, `ProjectWindow.{h,cpp}`, `editor/util/Util.h`,
    `FileDialogs.h`, `CustomTextEditor.{h,cpp}`, `CodeEditor.cpp`;
  - `Exporter.{h,cpp}` (`checkHtmlUIReferences`), `editor/util/ProjectUtils.cpp`,
    `ScriptEvents.cpp`, `McpValues.cpp`;
  - the `EntityTools.cpp`/`WorldTools.cpp`/`CommandLine.cpp` creation-type rows;
  - `SceneRender.cpp`, `SceneRender2D.cpp` (outline);
  - `editor/App.cpp` (watcher call, Assets > Create > HTML Document, debugger toggle in the
    Game view);
  - the `htmlUiMsaaSamples` project setting in `editor/Project.{h,cpp}`,
    `editor/window/dialog/ProjectSettingsWindow.{h,cpp}` and the MCP project-settings tool,
    plus the `Generator.cpp` sample-count init line.

**Additive touches.**
- `editor/Project.{h,cpp}`: `notifyHtmlUIFileSaved`, the `visitAssetPathsInRegistry`
  visitor, play-stop hook placement shared with p1-anim (each adds its own line).
- `RmlFile.cpp`, `RmlFontEngine.cpp` (`reloadFontFile`, dependency log).
- `RmlRenderer::LoadTexture`: one line recording the lazy-load path.
- `UIDocumentSystem` debugger methods in the phase-0 `.cpp`.
- `engine/CMakeLists.txt` (`JASPERENGINE_HTMLUI_DEBUGGER`, sources).

**Dependencies:** wave 0. Reload tests need `p1-lua` merged; listener re-attachment calls
`HtmlUIScriptState::onDocumentReloaded`.

**Deliverables:** §3.8; `docs/mcp/guide/html-ui.md` sections "Editor workflow" and "Hot
reload".

**Tests:** §5.1 editor/unit (Stream round-trip, scanner, export check, tokenizer); §5.3
`test_html_ui_editor.py`, `test_html_ui_reload.py`, `test_html_ui_export.py`; existing
`test_protocol.py`, `test_ui_anchors.py`, `test_stubs.py`, `test_prefab.py`.

---

### p1-lua — element API, events, data binding, stubs
**Goal:** §3.9 and patch 1 of §3.14.

**Owned files.**
- New:
  - `engine/core/htmlui/UIElement.cpp`, `UIEvent.cpp`, `UIDataModel.cpp`, `UIValue.cpp`;
  - `HtmlUIEventListener.{h,cpp}`, `HtmlUIValueDefinition.{h,cpp}`,
    `HtmlUIScriptInternal.h`;
  - `engine/core/subsystem/UIDocumentSystemScript.cpp`;
  - `engine/core/script/binding/HtmlUIClassesLua.cpp`;
  - `engine/core/script/binding/stubs/` (directory and mechanism).
- Changes: `engine/core/object/ui/UIDocument.{h,cpp}` (API), `generate_lua_stubs.py`
  (overrides + stub fragments), RmlUi `Event.h/.cpp`, `EventDispatcher.cpp`, `Context.cpp`
  (fork patch), `docs/html-ui/FORK.md` entry.

**Additive touches.**
- `LuaBinding.h/.cpp` (registration).
- `LuaFunctionBase.cpp` (destructor unref fix).
- `engine/CMakeLists.txt` sources.
- `editor/Generator.cpp` include dir.
- Xcode project.

**Dependencies:** wave 0.

**Deliverables:** §3.9, §3.14.1; `docs/mcp/guide/html-ui.md` "Scripting" (element API
table, events table, data binding with a full example, handle lifetime rules) and the
matching section in `docs/mcp/guide/lua-api.md`.

**Tests:** §5.1 script tests; §5.3 `test_html_ui_lua.py`; `test_stubs.py`.

---

### p1-vector — ThorVG, SVG, Lottie
**Goal:** §3.10 and patch 2 of §3.14.

**Owned files.**
- New: `engine/libs/thorvg/**`, `engine/core/util/ThorVGRuntime.{h,cpp}`,
  `engine/core/htmlui/vector/**`, `engine/core/script/binding/HtmlUIVectorLua.cpp`.
- Changes: `engine/libs/rmlui/Source/SVG/SVGCache.cpp` and the rmlui SVG CMake link,
  `engine/core/texture/TextureData.cpp`. `engine/libs/nanosvg/` is deleted.

**Additive touches.**
- `engine/CMakeLists.txt` (thorvg wiring; remove nanosvg line 673).
- `HtmlUIContext` (registers the `lottie` instancer; `ThorVGRuntime::acquire`).
- `LuaBinding.h/.cpp` (one registration line after p1-lua's).
- `NOTICE.md`, `docs/html-ui/FORK.md`, Xcode project.

**Dependencies:** wave 0, which provides `HtmlUITexture.h` and the RGBA8 dynamic texture.

**Deliverables:** §3.10; `docs/mcp/guide/html-ui.md` "SVG and Lottie" (attributes, control
API, events, `.lottie` support, tinting).

**Tests:** §5.1 vector tests; goldens G19–G20 and G22; §5.3 Lottie checks in
`test_html_ui_lua.py` (owned section).

---

### p1-anim — Anime.js-style animation
**Goal:** §3.11.

**Owned files.**
- New: `engine/core/htmlui/anim/**`, `engine/core/script/binding/HtmlUIAnimeLua.cpp`,
  `engine/core/script/binding/stubs/anime.lua`, `engine/tests/htmlui/anime/**`
  (`gen_reference.mjs`, `reference.json`, tests).

**Additive touches.**
- `UIDocumentSystem.cpp`: the tick line.
- `Engine.cpp` (`AnimEngine::reset()` in `systemShutdown`).
- `editor/Project.cpp` (reset on play stop).
- `LuaBinding.h/.cpp` registration.
- `engine/CMakeLists.txt` sources, Xcode project.

**Dependencies:** wave 0. Integration tests with elements need `p1-lua` merged.

**Deliverables:** §3.11; `docs/mcp/guide/html-ui.md` "Animation" (API reference, easing
list, differences from Anime.js: string easing forms accepted, no SVG/draggable/scroll/
layout/text modules, no WAAPI).

**Tests:** §5.1 animation tests; §5.3 animation checks in `test_html_ui_lua.py` (owned
section).

---

### p1-rtt — render to texture, world input, font effects
**Goal:** §3.12–§3.13.

**Owned files.**
- New: `engine/core/subsystem/UIDocumentSystemTexture.cpp`,
  `engine/core/htmlui/UIWorldRaycast.{h,cpp}`, the font effect code
  `engine/core/htmlui/RmlFontEffects.{h,cpp}`.
- Changes: `RmlFontEngine.{h,cpp}` (effects path).

**Additive touches.**
- `UIDocumentSystemInput.cpp`: one `dispatchWorldPointer` call per pointer method.
- `RmlRenderer` (render-to-`Framebuffer` entry point using the `Unp` variant that p1-render
  ships).
- `UIDocument.cpp`: `getTargetTexture`.
- `engine/CMakeLists.txt` sources, Xcode project.

**Dependencies:**
- Font effects: wave 0.
- §3.12: `p1-render` and `p1-input` merged.

**Deliverables:** §3.12–§3.13; `docs/mcp/guide/html-ui.md` "Documents on meshes" and
"Text effects".

**Tests:** §5.1 raycast/RTT/font tests; goldens G25–G26; §5.3 world-input case in
`test_html_ui_input.py` (owned section).

---

## 5. Test plan

All C++ tests live in `engine/tests/htmlui/` (target `jasperengine-htmlui-tests`, headless
under Xvfb).
- Fixtures: `engine/tests/htmlui/fixtures/p1/`.
- Goldens: `engine/tests/htmlui/golden/p1/`.
- The phase-0 harness's comparison tolerance is used.
- Goldens are rendered at a fixed 512×384 context, sample count 4, game time pinned by the
  harness.

### 5.1 Unit tests (googletest)
**Render** (`RenderTests.cpp`)
- `PipelineState` hash and equality cover every field.
- `SokolPipelineCache` returns the same `sg_pipeline` for an equal state and a new one for a
  different stencil ref. `releaseShader` empties the entries.
- `FramebufferDesc` with 4 samples has a resolve texture. `getColorTexture()` is the
  resolve image. A shared depth-stencil is the same image.
- `CompileFilter`: for each of the 7 colour-matrix filters at values 0, 0.5 and 1, the
  matrix equals GL3's formula within 1e-6. `opacity`, `blur`, `drop-shadow` and an unknown
  name each map correctly; the unknown name logs one warning.
- `CompileShader`: linear/radial/conic, plain and repeating, map to `{func, p, v}` as GL3.
  17 stops are truncated with a warning.
- `SigmaToParameters` table for sigma 0.5, 1, 3, 6, 12, 48, 400 (pass level and sigma).
- **Pass counting.** A static document with no clip or filter takes 1 pass. `filter:
  blur(4px)` takes the exact expected count. Nested `opacity` inside `blur` takes the
  expected count.
- `SteadyStateNoAllocations` (§3.2.9).
- The stencil ref sequence for three nested clips is 1, 2, 3, and the cache holds
  exactly the expected pipelines.
- Unloading a document releases all geometry and textures: `geometryLive` returns to 0 and
  the live `HtmlUITexture` count to 0.

**Input** (`InputTests.cpp`)
- `RmlKeyMap` covers every `D_KEY_*` from `Input.h:17-138`, with `KI_UNKNOWN` only for
  UNKNOWN, WORLD_1/2 and F25. Modifier mapping is checked, including Apple Cmd→Ctrl under
  `__APPLE__` simulation through an injectable flag.
- Surrogate combining: a valid pair, a lone high followed by an ASCII character, a lone low,
  and a high at end of input.
- Consume: clicking a `<button>` is consumed. Clicking a transparent body is not. A
  `pointer-events:none` overlay passes through. A body with a background is consumed.
  `Engine::onMouseDown` is not called when consumed with `ignoreEventsHandledByUI=true`,
  and is called with false.
- Keys: a focused `<input>` consumes letters but not F5. A keyup after a consumed keydown is
  consumed. `preventDefault` in keydown consumes.
- Control characters: `\b` with a preceding BACKSPACE keydown in the same frame deletes one
  character; `\b` alone (Android) deletes one character.
- Wheel scrolls an `overflow:auto` box: scrollTop increases for a downward wheel.
- Multi-touch: two pointers press two buttons simultaneously and both get `:active`. Cancel
  clears both.
- IME state machine (port of the Win32 editor): compose "ka" → "か" → commit; cancel
  restores the text; cursor -1 selects the composition.
- The base `System` clipboard round-trips. `RmlSystem` clipboard calls hit `System`.

**Script** (`ScriptTests.cpp`, Lua state from `LuaBinding::createLuaState`)
- Every `UIElement` method and property from §3.9.1/§3.9.5, through Lua and through C++.
- An invalid handle after `doc:load(other)`: `isValid()` false; a method raises the exact
  message.
- Detached pool: `createElement` without append is freed after the handle is collected
  (`collectgarbage()` plus one update). Re-appending a removed child works.
- Listeners: duplicate add is ignored; remove; `once`; capture vs bubble order;
  `stopPropagation`, `stopImmediatePropagation`; `liveLuaRefs()` returns to 0 after unload.
- Events: field mapping for click (button numbering), key (`key`/`code`/`keyCode`), wheel
  (deltas), input/change (change on blur only for text), custom `dispatchEvent` with
  `detail`.
- Data binding:
  - `data-for` renders N rows; `table.insert` adds a row; `model.items[2].name` updates text;
  - two-way `data-value` on an input updates `model.text` and fires `onChange`;
  - `data-event-click="buy(i)"` calls the Lua function with the index;
  - a new top-level key errors;
  - access after `removeDataModel` errors;
  - `createDataModel` after load reloads the document and binds.
- The C++ `UIDataModel` API mirrors the Lua tests.
- Fork patch: `preventDefault` on a checkbox click keeps it unchecked.

**Editor-side logic** (`EditorLogicTests.cpp`)
The editor has no googletest target, so p1-editor keeps the testable logic free of ImGui and
editor state, and compiles those files into `jasperengine-htmlui-tests` as extra sources:
- the HTML/CSS lexer lives in `editor/window/widget/HtmlCssLexer.{h,cpp}`, a pure function
  `lexLine(language, line, state&) -> tokens` that `CustomTextEditor::tokenizeLine` calls;
- `HtmlUIDependencies` is engine code anyway.

Tests:
- `collectHtmlUIReferences` on a fixture with `link`, `@import`, `url()`, `@font-face`,
  `img`, `svg src`, `lottie src`, a remote URL and a missing file: exact list with
  line/column.
- `lexLine`: HTML and CSS token kinds for fixture lines, including a multi-line comment
  carried across lines and a `<style>` block.

Stream round-trip and Catalog behaviour are tested end to end in `test_html_ui_editor.py`
(§5.3), because `Stream` needs the editor.

**Vector** (`VectorTests.cpp`)
- SVG rasterised by ThorVG at 3 sizes. The output is premultiplied: no channel exceeds alpha.
  `crop_to_content` gives a smaller intrinsic size. `TextureData` SVG output is straight
  alpha and keeps nanosvg-era sizes (§3.10.4).
- `LottiePlayback`: frame for given time, speed, direction, bounce, loop count and segment;
  marker resolution; `complete` and `loop` event counts over a simulated 3.5 loops.
- Lottie texture: the texture id is stable across 60 frames, `updateTexture` is called only
  on frame changes, and resize creates exactly one new texture.
- dotLottie: archives with stored and deflate entries, v1 and v2 manifests,
  `activeAnimationId`, embedded image. ZIP64 and encrypted archives produce an `error`
  event.

**Animation** (`AnimeTests.cpp`)
- Every easing equals `reference.json` within 1e-9 at 101 points.
- Spring settling durations equal the reference.
- Stagger outputs (first, center, last, index, grid, axis, ease) equal the reference.
- Timeline offsets for every position form equal the reference.
- Callback order: onBegin → onUpdate… → onLoop → onComplete. Counts with loop=2 and
  alternate. `reverse`, `seek`, `playbackRate`, `pause/resume`, `restart`.
- Composition `replace` cancels the overlapped tween; `none` keeps both; `blend` sums.
- Individual transforms compose into one transform in insertion order.
- `IsLayoutDirty()` is false after a transform/opacity-only tick and true after a `width`
  tick.
- Plain table targets.
- Invalid element targets are skipped; an animation whose targets are all invalid is
  cancelled silently.
- `reset()` frees every Lua ref.

**RTT and font effects** (`TextureModeTests.cpp`, `FontEffectTests.cpp`)
- `raycastMeshUV`:
  - plane primitive centre → (0.5, 0.5);
  - corners within 1e-4;
  - a rotated and scaled entity;
  - a miss;
  - a back face (hit, as the triangle test is two-sided);
  - the nearest of two overlapping meshes.
- Target texture readback: an opaque red div gives (255,0,0,255). A 50% white div gives
  straight (255,255,255,128) ± 1, which proves un-premultiplication. Orientation: a
  top-left red square appears at texel (0,0).
- Font effects: `PrepareFontEffects` caches by fingerprint; each effect's atlas region size
  equals `GetGlyphMetrics`; `GetVersion` increments after a new glyph is added to an effect
  layer.

### 5.2 Golden images (`engine/tests/htmlui/golden/p1/`)
| ID | Fixture | Exercises |
|---|---|---|
| G01 | `clip_rounded.html` | `overflow:hidden` + `border-radius` clipping an image and text (stencil Set) |
| G02 | `clip_nested.html` | 3 nested rounded clips (Intersect, refs 1–3) |
| G03 | `clip_inverse.html` | a clip that RmlUi turns into `SetInverse` (`box-shadow: inset` with radius) |
| G04 | `clip_transform.html` | rotated (15deg) and scaled `overflow:hidden` container with children |
| G05 | `transforms.html` | translate/rotate/scale/skew, and `perspective` + `rotateY` |
| G06 | `filter_opacity.html` | `filter: opacity(.5)` on overlapping children (group, via a layer) |
| G07 | `filter_blur.html` | `blur(2px)`, `blur(8px)`, `blur(40px)` (downscale passes) |
| G08 | `filter_drop_shadow.html` | drop-shadow with blur 0 and 6px, on text and an image |
| G09 | `filter_color_matrix.html` | 7 tiles: brightness 1.5, contrast 2, grayscale 1, hue-rotate 90deg, invert 1, saturate 3, sepia 1 |
| G10 | `filter_chain.html` | `blur(2px) grayscale(1) opacity(.7)` |
| G11 | `backdrop_filter.html` | `backdrop-filter: blur(10px)` panel over a colourful image and text |
| G12 | `gradients_linear.html` | angles 0/45/90/180deg, 5 stops, `repeating-linear-gradient` |
| G13 | `gradients_radial_conic.html` | radial circle/ellipse, repeating radial, conic, repeating conic |
| G14 | `box_shadow.html` | outer, inset, spread, blur, with border-radius (SaveLayerAsTexture) |
| G15 | `mask_image.html` | `mask-image` with a gradient and with an image (SaveLayerAsMaskImage) |
| G16 | `msaa_rounded.html` | large radii and thin rotated borders with 4x MSAA. The test also renders at sample count 1 and asserts the images differ on edge pixels. |
| G17 | `layers_nested.html` | opacity inside blur inside a backdrop panel (layer depth 3, pass splitting) |
| G18 | `scissor_scroll.html` | `overflow:auto` scrolled to 120px (scissor without a mask) |
| G19 | `svg_file.html` | `<svg src>` at 24/64/256 px, `crop-to-content`, tint via `image-color` |
| G20 | `svg_inline.html` | inline `<svg>` with a gradient, stroke, `<text>` |
| G21 | `shader_effect.html` | `decorator: shader("test-pattern")` |
| G22a–c | `lottie_frames.html` | a `.json` Lottie at t = 0, 0.5, 1.0 s; a `.lottie` with marker "loop" at its first frame |
| G23 | `opacity_text.html` | text with `opacity:.5` over a gradient (premultiplied correctness) |
| G24 | `many_elements.html` | 500 small rounded buttons. Asserts pipeline applies ≤ 10 and passes = 1 (performance golden). |
| G25 | `texture_mode` | a TEXTURE document read back from its target texture (straight alpha) |
| G26 | `font_effects.html` | shadow, outline 2px, glow, blur on 32px text |

G01–G24 must pass on **glcore and vulkan** against the same images. A difference beyond the
tolerance on one backend is a bug to fix, not a tolerance to raise.

### 5.3 MCP end-to-end (`tools/mcp/tests/`)
- **`test_html_ui_editor.py`** (p1-editor):
  - `create_entity type=ui_document`;
  - `set_properties` for each field, with `get_entity` reading them back;
  - `save_scene` and reopen, then check the YAML keys (every field round-trips, defaults are
    omitted, and a changed `path` reloads the document);
  - copy/paste component; undo/redo;
  - prefab override of `sortOrder`;
  - Inspector-visible values;
  - Hierarchy *Create > UI > UI Document* through the menu entry table;
  - `.html`/`.css` file types in `list_assets`;
  - Assets > Create > HTML Document creates two files;
  - screenshot of the Scene view of a UI scene showing the document, checked with a pixel
    probe.
- **`test_html_ui_input.py`** (p1-input, with an owned section for p1-rtt):
  - play; `send_input` click on a button → a Lua handler sets a global → checked with
    `eval_lua`;
  - a click outside on a transparent body reaches the game's `onMouseDown`;
  - typing `"héllo 😀"` into an `<input>` → its value is exactly that string;
  - `ime` / `ime_commit` / `ime_cancel` sequences change the input's value as specified;
  - Ctrl+A, Ctrl+C, then Ctrl+V into a second input: `System.getClipboardText()` matches;
  - wheel scroll changes `scrollTop`;
  - two touch pointers;
  - `mouse_leave` clears `:hover` (style probe through `eval_lua`);
  - Escape key not consumed;
  - p1-rtt section: click on a TEXTURE document shown on a plane in a 3D scene hits the
    right button (`eval_lua` flag).
- **`test_html_ui_reload.py`** (p1-editor):
  - during play, `write_file` changes a CSS colour → within 2 s a screenshot pixel probe
    shows the new colour, and an element handle stored in a Lua global is still valid;
  - `write_file` changes the HTML → the input value, scroll position and focus are kept, a
    listener on an element with an id still fires, and `onLoad` ran twice;
  - in edit mode, a CSS change updates the Scene view (screenshot).
- **`test_html_ui_lua.py`** (p1-lua, with owned sections for p1-vector and p1-anim):
  - the element API through `eval_lua` scripts;
  - a data-bound list grows on insert (screenshot plus `childElementCount`);
  - Lottie `play/pause/seek` and the `complete` event;
  - `anime.animate` on `translateX`, checked with `wait` + `getBoundingClientRect`;
  - a timeline with labels;
  - `liveLuaRefs()` returns to 0 after stop.
- **`test_html_ui_render.py`** (p1-render): Game-view screenshots of a gallery document
  (gradients, filters, svg, backdrop) with pixel probes at listed coordinates (expected
  colours ± 6/255), on the default editor build.
- **`test_html_ui_export.py`** (p1-editor):
  - `run_build` for a linux export of a project whose document references CSS, PNG, TTF,
    SVG and Lottie → success, and the exported `assets/` and the pack contain them;
  - a missing reference and a remote URL → the export fails with `file:line:col` in the
    message.
- **Always also run:** `test_protocol.py`, `test_ui_anchors.py`, `test_stubs.py` (new
  classes, no unresolved types), `test_prefab.py`, `test_play.py`.

Look at every screenshot produced (BRIEF).

### 5.4 Builds
| Target | How | Who |
|---|---|---|
| Linux glcore editor + engine | `JOBS=3 tools/build.sh linux` | every package |
| Linux Vulkan editor + engine | `JOBS=3 tools/build.sh shell linux`, then `cmake -S . -B build/linux/editor-vk -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRAPHIC_BACKEND=vulkan && cmake --build build/linux/editor-vk --target jasperengine-editor jasperengine-htmlui-tests -j3`; run the htmlui tests with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` (or the image's lavapipe ICD) under Xvfb | p1-render, p1-rtt; others when touching render code |
| Web (WebGL2) | `JOBS=3 tools/build.sh web` | every package with engine changes |
| Android (GLES3) | `JOBS=3 tools/build.sh android` | every package with engine changes (p1-input must: JNI + Java) |
| Windows (D3D11/GL/Vulkan), macOS (Metal/GL/Vulkan), iOS (Metal) | cannot be built here | see below |

How the unbuildable targets are kept correct:
- **Shaders.** The Linux editor's shader builder cross-compiles every new ShaderType to
  GLSL 410, GLSL 300 es, HLSL 5 and MSL 2.1. `test_html_ui_render.py` runs MCP
  `generate_shaders` on a fixture project with a UI document for every target platform the
  tool accepts, and expects no failure for any `HTMLUI*` variant.
- **Platform code.** Only inside the existing platform directories and `#if` blocks. Use
  only APIs already linked there or listed in this spec (`imm32`; AppKit/UIKit, already
  linked). No POSIX calls outside the Linux/Android/Web directories.
- **Read-through review.** Each package's report lists every Windows, macOS and iOS hunk.
  The package author re-reads them against the platform API documentation, checking
  signatures, ownership, and threading (UIKit/AppKit on the main thread; JNI attach on
  Android).
- **CMake.** Every new source is added to the Xcode project and to the per-platform CMake
  sections by reading the existing entries (DESIGN, BRIEF).
- **sokol.** The code is backend-neutral: no `#if SOKOL_*` in UI code. The only exception is
  the Metal stencil store note, which is handled generically by setting depth and stencil
  actions together.

---

## 6. Acceptance criteria

**Renderer**
- [ ] All RmlUi 6.3 optional `RenderInterface` methods are implemented: `EnableClipMask`,
  `RenderToClipMask`, `PushLayer`, `CompositeLayers`, `PopLayer`, `SaveLayerAsTexture`,
  `SaveLayerAsMaskImage`, `CompileFilter`/`ReleaseFilter`, `CompileShader`/`RenderShader`/
  `ReleaseShader`.
- [ ] Goldens G01–G24 pass on glcore and on vulkan (lavapipe). G16 shows MSAA edges.
- [ ] A document without layers or filters renders in one offscreen pass. G24 stays within
  its pipeline and pass budget, and `SteadyStateNoAllocations` passes.
- [ ] `PipelineState`, `SokolPipelineCache` and `FramebufferDesc` (MSAA + resolve + shared
  depth-stencil) exist. No other engine rendering changes output: existing MCP screenshot
  tests stay green.
- [ ] `HTMLUI_GRADIENT`, `HTMLUI_POST` and `HTMLUI_EFFECT` are registered in every table of
  §3.3.2. They are exported in builds and cross-compile to every target language. The
  shader cache is bumped to v30.
- [ ] The shader-decorator registry works with `test-pattern` (G21).

**Input**
- [ ] Documents receive wheel (x/y), key up/down with modifiers, text (including astral
  characters), mouse leave, multi-touch with ids and touch cancel.
- [ ] Keys go to documents before the game. The consume rules of §3.6.2 hold, with tests,
  and `ignoreEventsHandledByUI` is respected.
- [ ] Focusing a text field opens the virtual keyboard (Android, iOS, Web) and positions the
  IME (Windows, macOS, Linux, Web).
- [ ] IME composition works on Windows, macOS, iOS, Android and Web; Linux commits through
  XIM.
- [ ] `System::setClipboardText`/`getClipboardText` are implemented on Linux, Windows,
  macOS, iOS, Android, Web and in the editor, and are bound in Lua.

**Editor**
- [ ] UI Document: Catalog, Stream, Factory, Inspector (all rows of §3.8.4), Add Component,
  Hierarchy *Create > UI > UI Document*, MCP/CLI creation types, copy/paste, undo, prefab
  overrides, play-stop restore.
- [ ] `.html`/`.css` file types with icons; HTML and CSS highlighting with RCSS property
  suggestions; Assets > Create > HTML Document.
- [ ] Hot reload in edit and play mode: CSS-only changes reload stylesheets in place;
  HTML/SVG/Lottie/font changes reload with scroll, focus, form values and id-ed listeners
  preserved; images re-upload.
- [ ] Documents show in the Scene view of UI and 2D scenes in edit mode.
- [ ] The RmlUi debugger toggles from the Game view and the Inspector.
- [ ] Export fails with `file:line:col` for missing, outside-assets or remote references, and
  succeeds otherwise with all assets shipped.

**Scripting**
- [ ] Every `UIElement`/`UIDocument`/`UIEvent`/`UIDataModel` member of §3.9.1 is in C++ and
  Lua, with browser names and event mapping per §3.9.3.
- [ ] Element handles are safe after unload; the detached pool and listeners free their
  memory and Lua refs (`liveLuaRefs() == 0` after stop and after shutdown order).
- [ ] Data binding from Lua tables and C++: two-way, `data-for` arrays, `data-event-*`,
  `onChange`, reload on late model creation.
- [ ] `preventDefault` fork patch is in, with a test.
- [ ] LuaLS stubs and API suggestions include all new classes, with typed callbacks and no
  unresolved types (`test_stubs.py`).

**Vector**
- [ ] ThorVG is vendored with the configuration of §3.10.1.
- [ ] RmlUi SVG rasterises with ThorVG (or the LunaSVG fallback is documented) — G19/G20.
- [ ] `<lottie>` supports all attributes of §3.10.3, `.lottie` files, the control API in C++
  and Lua, and its events. It uses one in-place RGBA8 texture per element, updated only on
  frame change (unit test) — G22.
- [ ] `TextureData` SVG is on ThorVG; nanosvg is removed; the regression test passes.

**Animation**
- [ ] `anime.animate`, `createTimeline`, `createTimer`, `createSpring`, `stagger`, the
  easings, callbacks, playback control and `utils` match `reference.json`.
- [ ] Animation drives element styles and plain tables; transform/opacity do not dirty
  layout (test).
- [ ] `AnimEngine` resets on play stop and shutdown.

**Render to texture and fonts**
- [ ] `TEXTURE` documents render into a straight-alpha, top-left-origin `Framebuffer` that
  `Image`, `Mesh` and materials can use (G25).
- [ ] Pointer input on a textured mesh reaches the document through the UV hit test, with
  pointer capture.
- [ ] Font effects shadow, outline, glow and blur render (G26).

**Everywhere**
- [ ] Linux glcore and vulkan, web and android build with zero new warnings in our code.
  Windows/macOS/iOS hunks are listed and reviewed per §5.4.
- [ ] All tests of §5 pass; every produced screenshot has been looked at.
- [ ] `docs/mcp/guide/html-ui.md` (sections named per package), `docs/mcp/guide/lua-api.md`,
  `docs/html-ui/FORK.md` (two patches), `NOTICE.md` (ThorVG) and the MCP tool docs are
  updated.
- [ ] No GPU object leaks: loading and unloading a document 100 times in a test returns
  `HtmlUIRenderStats.geometryLive`, the live texture count and the pipeline cache size to
  their initial values (the pipeline cache may keep pipelines; its size must not grow after
  the first cycle).
