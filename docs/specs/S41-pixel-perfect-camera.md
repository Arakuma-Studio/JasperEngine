# S41 — Pixel Perfect Camera: integer scaling, Pixels Per Unit, one pixel-snap rule, rotation policy, sub-pixel smoothing, pixel zoom

**Family / Depends on.** Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5A (with S43, S44 and
S52; the family lands in waves 5A → 5B → 5C, ledger "Implementation order");
depends on nothing new (it builds on S13's uber pass, S14's import pipeline only through S42, S25's
settings and commands, S26's Scene view, S08's component reflection, S23's `tools/lua_api`). S42,
S48, S49, S50, S51 and S52 depend on the rule this spec defines (§6.2). Owner area:
`engine/core/pixelart/` (`PixelSnap`, `PixelPerfectLayout`, `PixelRotation`),
`engine/core/component/PixelPerfectCameraComponent.h` (components live with the others; the logic is
in `pixelart/`), `engine/core/object/PixelPerfectCamera.{h,cpp}` (the Lua handle),
`engine/shaderlib/includes/pixel_snap.glsl`, the pixel-snap variants of the built-in vertex stages,
`editor/pixelart/` (Inspector, Scene view pixel zoom, gizmo), the Game view's Pixel Scale.

## 1. Goal

Pixel art renders correctly by default and the same on every backend: one base resolution (for
example 320×180) drawn into a low-resolution target and scaled to the screen by a whole number with
bars around it; one pixel size for the whole world (a project-wide **Pixels Per Unit**); the camera
and every sprite, tile, glyph and particle on whole base pixels through **one rounding rule** that
the CPU and the vertex shader compute identically; rotations that stay on the grid; and smooth
camera scrolling that never puts the art between pixels. A **Pixel Perfect Camera** component (Unity's
name, Unity's fields where they make sense) turns it on for a camera. People get integer zoom and a
1× view in the Scene and Game views; agents drive it through the existing generic tools and verify it
through S42's `check_pixel_art`. The test method is pixel parity: the snapped GPU path must equal a
CPU reference to the pixel.

## 2. Scope

**In scope**
- `PixelPerfectCameraComponent` ("Pixel Perfect Camera"): reference resolution, upscale mode
  (Integer letterbox, Integer expand, Stretch Fill), upscale render texture on/off, pixel snapping,
  rotation policy, sub-pixel smoothing, letterbox colour; its runtime layout.
- The layout function shared by the engine, the editor, MCP, S42 and the tests
  (`computePixelPerfectLayout`), and two new project scaling modes on the existing `Scaling` enum
  (`INTEGER`, `INTEGER_EXPAND`) that use it without a component.
- Project setting **Pixels Per Unit**; `pixelsPerUnit` on Sprite and Tilemap (0 = the project's).
- The pixel-snap rule (§6.2): `engine/core/pixelart/PixelSnap.h` and
  `engine/shaderlib/includes/pixel_snap.glsl`, applied by a shader-variant bit to the built-in
  mesh (sprites, tilemaps), depth, UI (images, nine-slice, text) and particle vertex stages; the
  camera snap; shared-edge snapping for tilemap and nine-slice pieces.
- Rotation policy: 90° steps (default) or Rotate On Grid (opt-in, per camera and per sprite).
- Sub-pixel camera smoothing: an offset applied in the upscale blit (or in the projection without
  the render texture), quantised to whole screen pixels.
- Editor: Inspector, Scene view Pixel Zoom (integer steps, 1×, Game scale, pixel grid, pixel-snapped
  move), Scene view snap preview, the camera gizmo's reference frame, Game view Pixel Scale (Auto,
  1×…8×) and 1:1 display.
- Lua API, MCP surface (no new tool: generic tools, two parameters on existing ones), docs.
- Golden and pixel-parity tests; bench.

**Out of scope**
- Validation, fixes, the project preset and texture import defaults: **S42**.
- Palettes, pixel effects and dithering (S43, S48); pixel particle modes and sizes (S49); the grid
  tilemap and autotiling (S50); pixel fonts (S51); instanced sprites (S52). Each of them applies the
  rule of §6.2; this spec gives them the include, the CPU twin and the parity harness.
- Perspective pixel art (2.5D with a perspective camera) and camera roll: the component is inert
  on a perspective camera, and a rolled camera draws unsnapped (§6, D8).
- High-quality rotation filters (RotSprite, cleanEdge): S48 may add one as a sprite shader feature;
  Rotate On Grid here is nearest-neighbour on the base grid.
- A runtime camera zoom field: zoom by changing the Reference Resolution (§9).
- Do-not-copy list (`docs/specs/README.md`): nothing of it applies; no render graph is introduced
  (the blit is the existing fixed-resolution pass).

## 3. Current state (read on `specs/pixel-art` 1b1c34fbd)

- **World units are canvas points.** A 2D camera with `autoResize` (default, `CameraComponent.h:104`;
  default type `CAMERA_UI`, `:42`) spans the canvas: `RenderSystem::updateCameraSizeForCanvas`
  (`engine/core/subsystem/RenderSystem.cpp:12958-12987`) sets `leftClip 0 … rightClip canvasWidth`,
  `bottomClip 0 … topClip canvasHeight`. A sprite's quad is its texture size in points
  (`MeshSystem::createSprite`, `engine/core/subsystem/MeshSystem.cpp:661-665`), with
  `SpriteComponent::width/height` as `unsigned int` (`SpriteComponent.h:29-30`). The agent guide says
  so (`editor/mcp/McpInstructions.cpp:145`, "units are points (pixels of the canvas…)"). UI is laid
  out in the same camera space (`RenderSystem::drawUI` uploads `objectMVP(transform)`,
  `RenderSystem.cpp:11054-11093`). There is **no Pixels Per Unit** anywhere (grep `pixelsPerUnit`:
  only `editor/render/preview/PoseRender.cpp:515`, a preview helper). Box2D converts with
  `pointsToMeterScale2D = 64` (`PhysicsSystem.cpp:296`).
- **Fixed resolution** is a scene setting (`engine/core/SceneSettings.h:410-413`: enabled, 640×360,
  `NEAREST`), serialized by the editor (`editor/Stream.cpp:4363-4366`, `:4534-4544`) and the binary
  codec (`engine/core/serialization/SceneSettingsCodec.cpp:121-124`, `:185-189`), bound to Lua
  (`CoreClassesLua.cpp:3212-3216`) and to MCP (`set_scene_settings` free-form keys
  `fixed_resolution_*`, `editor/mcp/tools/SceneTools.cpp:838-841`). When active
  (`RenderSystem::isFixedResolutionActive`, `RenderSystem.cpp:14035-14045`: the Engine main scene, or
  a secondary view that asks), the main camera's colour pass goes into `fixedResFramebuffer`
  (`ensureFixedResFramebuffer`, `:10280-10305`, colour + the filter the blit samples with), and
  `renderFixedResolutionBlit` (`:10341-10343`) or the uber pass (`:16446-16496`) upscales it to
  `targetViewRect()` (`:13803-13809`, the Engine's view rect). The uber pass already reads a NEAREST
  target through a linear sampler with UV snapped to texel centres (`nearestSource`, `:16460`;
  `engine/shaderlib/uber.frag:37`, `:200`), and its UV is `position * uvScaleOffset.xz + .yw`
  (`uber.vert`), as the plain blit's `params` (`engine/shaderlib/blit.frag`).
- **What is wrong with it for pixel art.**
  - The scale is never an integer on purpose. The view rect comes from the project's `Scaling`
    (`engine/core/Engine.h:167-174`: FITWIDTH, FITHEIGHT, LETTERBOX, CROP, STRETCH, NATIVE;
    `Engine::computeViewLayout`, `engine/core/Engine.cpp:1602-1667`), which fits the canvas aspect
    with a float factor: 640×360 in 1366×768 is 2.13×, so columns come out 2 and 3 px wide (the
    @asynkimo video's "5.45×" failure).
  - The camera still spans the canvas, not the fixed resolution: with the default canvas 1280×720
    (`editor/Project.h:1469-1471`) and fixed resolution 640×360, one world point is half a target
    pixel, so a 16 px sprite draws 8 px wide. The fixed size is read directly from the scene at six
    sites (`RenderSystem.cpp:1576-1580`, `:4192-4193`, `:8195-8196`, `:9297-9301`, `:11915-11917`,
    `:15573`).
  - Nothing snaps. Positions, the camera and vertices are floats; nearest sampling at fractional
    positions makes edges gain and lose a pixel from frame to frame. A CENTER-pivoted sprite of odd
    size puts its corners on half points (`MeshSystem.cpp:687-689`, `:710-713`), so even a camera
    on whole points samples texel edges.
  - Tilemap tiles are separate quads at free float positions (`MeshSystem::createTilemap`,
    `MeshSystem.cpp:926`, `:1012-1020`), so neighbours can round apart and gap. UI nine-slice
    shares its 16 vertices in one indexed mesh (`UISystem.cpp:100-130`), which is already safe.
- **Shader variants.** Built-in vertex stages take defines from property bits
  (`engine/core/shader/ShaderVariantCompiler.cpp:384-412` MESH up to bit 35, depth `:413-432`, UI
  `:450-457`; names `engine/core/pool/ShaderPool.cpp:700-880`). The library vertex path keeps
  `invariant gl_Position` identical with the depth programs (`includes/surface_vs.glsl`). The frame
  block (`includes/frame.glsl`) is per camera pass and may only grow after `ambient`, identically in
  both stages. Particles are instanced quads built in `includes/particles_vs.glsl` (per-instance
  position, size, rotation).
- **Editor views.** The Game view zoom is `GameViewSettings::scale`, a float 1–5
  (`editor/AppSettings.h:133-135`; MCP `configure_game_view {scale}`,
  `editor/mcp/tools/ViewTools.cpp:2072`); its image is drawn at ImGui's scale (`engineScale`,
  `editor/window/GameWindow.h:62`). The 2D Scene view zooms continuously by ±10 % per wheel step
  (`editor/window/SceneWindow.cpp:2118-2129`); MCP `set_scene_camera {zoom}` is "world units per
  pixel" (`ViewTools.cpp:1978`) and takes free-form `display` flags (`:1979`).
- **Web.** The web platform reads `devicePixelRatio` only for `Screen` DPI
  (`engine/platform/emscripten/JasperEngineWeb.cpp:1743`); nothing sizes the canvas backing store by
  it (inferred from grep), so a browser upscale can blur or unevenly scale the integer image.

## 4. Reference

| Source | Files | What we take | What we change |
|---|---|---|---|
| spryte (MIT) | `src/draw.ts:303-311` (`Math.round` of an unrotated sprite's translation), `src/webgl2.ts:66-81` (`COPY_VERT`: `floor(e + 0.5)`, "same rule as draw.sprite()"), `:107-139` (`EACH_VERT`: per-copy snap only when the rotation is 0) | One rule on CPU and GPU; snapping the quad's placed origin (anchor applied), not the object's pivot; rotated quads not vertex-snapped | Our rule adds a tie bias (`floor(p + 0.5 + 2⁻⁷)`, §6.2): spryte compares CPU double with GPU float, which can disagree exactly at the half-pixel lattice that CENTER pivots produce; we snap in target pixels after projection, so every drawable kind (meshes, UI, particles, instancing) shares one function; y orientation is defined so GL's flipped targets round the same way |
| spryte | `src/panels.ts:120-132` (`snapEdges`), `:66`, `:112` | Shared-edge snapping: neighbouring pieces round shared edges once so they never gap or overlap | Our pieces get bit-identical shared-edge inputs on the CPU (one edge array per axis) and the vertex stage snaps them; tilemaps also round tile edges to the texel grid |
| spryte | `browser-test/checks.ts:1` ("draws the same scene two ways and compares every pixel"), `:157-195` (bulk vs loop parity, half-pixel copy, 8-pixel tolerance only for GPU sin/cos) | The pixel-parity test method | Our reference is a CPU compositor (no GPU), the GPU result must equal it with 0 differing pixels for unrotated content on all five backends; rotated content compares GPU paths with spryte's small edge tolerance |
| @asynkimo post (`/tmp/pixelart-research/x-post/fx.json`, text) | the rendering rules | Base resolution scaled by whole numbers with black bars; snap the camera and every sprite; one pixel size; no runtime rotation or 90° steps; particles on the same grid; check at 1× | Written in our own words in `docs/mcp/guide/pixel-art.md` (S42); here they become R1–R20 |
| Unity Pixel Perfect Camera (behaviour only, no code) | — | Names: Assets Pixels Per Unit → project Pixels Per Unit; Reference Resolution; Upscale Render Texture; Pixel Snapping; Crop Frame / Stretch Fill; Current Pixel Ratio readout | One project PPU instead of a per-camera one; the crop choices folded into three Upscale Modes |
| Godot (behaviour only) | — | Stretch mode viewport + scale mode integer; "expand" aspect; snap 2D transforms and vertices | `Scaling::INTEGER` / `INTEGER_EXPAND` as project modes, both reusing the component's layout |

Licences: spryte is MIT; we port two functions' logic (a few lines, rewritten in C++ and GLSL), so
`NOTICE.md` gets one spryte entry naming `draw.ts`, `webgl2.ts` and `panels.ts`.

## 5. Requirements

### Functional — layout and integer scaling

- **R1. Layout function.** `engine/core/pixelart/PixelPerfectLayout.{h,cpp}`:
  `computePixelPerfectLayout(screenW, screenH, refW, refH, mode, forcedScale, smoothing, ppu)` returns
  `{scale k, baseWidth/Height B, targetWidth/Height T, viewport, canvasWidth/Height, cropped}`. All
  values are integers except the canvas (world units). Rules:
  - `Integer` (letterbox, default): `k = max(1, min(⌊sw/Rw⌋, ⌊sh/Rh⌋))`, capped by `forcedScale` when it
    is positive; `B = R`; viewport `(⌊(sw − k·Bw)/2⌋, ⌊(sh − k·Bh)/2⌋, k·Bw, k·Bh)`. A screen smaller than R
    gives k = 1 and a centred crop (`cropped`, logged once).
  - `Integer Expand`: k as above; `B = (⌊sw/k⌋, ⌊sh/k⌋)` (never below R), viewport the centred k·B
    rect (bars only for the remainder, < k px).
  - `Stretch Fill`: B = R rendered at k as `Integer`; the final image fills the aspect-kept fit rect
    `min(sw/Rw, sh/Rh)` with the sharp-bilinear blit (R11); bars only for an aspect mismatch.
  - `T = B`, or `B + (2, 2)` with sub-pixel smoothing (one margin pixel per side, R12).
  - canvas = `B / PPU` world units.
- **R2. Project scaling modes.** `Scaling` (`Engine.h:167`) gains `INTEGER` and `INTEGER_EXPAND`
  (appended; names `integer`, `integer_expand` in `graphics.scaling_mode`, project.yaml and the
  generated `Engine::setScalingMode` switch, `editor/Generator.cpp:2555-2561`).
  `Engine::computeViewLayout` implements them with R1 (reference = the preferred canvas, PPU 1), so a
  project gets integer letterboxing of everything it draws at native resolution without a component.
- **R3. Pixel Perfect Camera.** A new component on a camera GameObject (§6.1 fields). On the Engine
  main scene's main camera (or a secondary view's base layer, as fixed resolution today) it:
  - computes R1 from the destination size each frame the size, reference or mode changed;
  - sets the Engine canvas to `B/PPU` and the view rect to the viewport, so input, Canvas Scaler
    (Follow Game Canvas) and layer scenes see the pixel-perfect layout;
  - sets the camera's clips to span `T/PPU` world units (origin offset `−1/PPU` with smoothing),
    overriding `autoResize`;
  - with **Upscale Render Texture** on (default), renders the main colour pass into the
    fixed-resolution target at T through the existing path (R4) and blits it at k into the viewport;
    off, renders directly into the viewport at native resolution with the snap grid of R7;
  - clears the bars with **Letterbox Color** (default black).

  Reference Resolution (0, 0) means the project canvas (`graphics.canvas_width/height`), so the base
  resolution has one home in the common case.
- **R4. One fixed-resolution path.** Every reader of the fixed size goes through
  `RenderSystem::fixedResolutionSize()` (the six sites of §3), which returns T when a Pixel Perfect
  Camera drives the target, else the scene's setting. `isFixedResolutionActive()` is true for either.
  While a Pixel Perfect Camera drives the main camera, the scene's `fixedResolution*` settings are
  ignored (kept, not changed) and the Inspector says "Driven by Pixel Perfect Camera on <name>".
  SSAO, SSR, lighting viewport, LOD, points sizes and the uber pass follow automatically (they read
  the same function).
- **R5. Layer scenes.** A Pixel Perfect Camera on a layer scene's camera (a HUD scene drawn after the
  blit) takes the main scene's layout (k, B, viewport), always draws directly (its Upscale Render
  Texture is ignored, the Inspector says so) and snaps to the same base grid, so HUD pixels line up
  with world pixels at native resolution. A render-to-texture camera with the component snaps to its
  framebuffer's pixels (k = 1, B = framebuffer size).

### Functional — Pixels Per Unit

- **R6. Pixels Per Unit.** Project setting `graphics.pixels_per_unit` (integer 1–256, default 1:
  world units are base pixels, today's meaning). `Engine::getPixelsPerUnit()`, generated into the
  game's main like the other graphics settings. `SpriteComponent::pixelsPerUnit` and
  `TilemapComponent::pixelsPerUnit` (float, 0 = the project's): their quads are their pixel sizes
  divided by it, so `width/height` keep meaning pixels (Unity's sprite rect) and old scenes are
  unchanged (0 → 1). The texel size of a drawable, in base pixels, is
  `worldScale × PPU_project / PPU_drawable` (with a Pixel Perfect Camera, base pixels per world unit
  = PPU_project); it is 1 for correct pixel art, and S42's `texel_size` rule measures it. Every reader
  of `SpriteComponent::width/height` as a world size (bounds, colliders fitted to sprites, picking,
  `Sprite:getWidth()`-style Lua getters that return world sizes) goes through
  `spriteWorldSize(sprite)`.

### Functional — the pixel-snap rule

- **R7. The rule** (precise statement in §6.2). Space: base pixels of the drawing camera's target
  (x right, y up, origin at the target's bottom-left corner, 1 unit = 1 base pixel), independent of
  the backend's Y flip. Formula: `snap(p) = floor(p + 0.5 + 1/128)`. CPU
  (`engine/core/pixelart/PixelSnap.h`) and GPU (`includes/pixel_snap.glsl`) evaluate the same float32
  operations in the same order.
- **R8. Camera snap.** The camera's world position used for the view (not its Transform) is
  `snapWorld(position, PPU) = snap(position · PPU) / PPU` per axis (its target moves by the same
  delta); the residual `f = position·PPU − snap(position·PPU)` (base px, per axis) feeds R12.
  Picking, audio and Lua see the Transform; `PixelPerfectCamera.snappedPosition` exposes the drawn one.
- **R9. What snaps, and how.** Through a vertex-stage variant bit `Pxs` ("Pixel Snap"), set only for
  draws of a camera with an active Pixel Perfect Camera and Pixel Snapping on:
  - Sprites and tilemaps (built-in `mesh.vert` and library shaders through `surface_vs.glsl`), and
    their depth programs (the same bit on DEPTH, so `invariant gl_Position` still matches);
  - UI images, nine-slice and text drawn by that camera (UI vertex stage);
  - particles (`particles_vs.glsl`: the particle centre is snapped; corners per vertex when the
    particle's rotation is a multiple of 90°; S49 adds pixel sizes, and its Pixel Art module snaps
    the lower-left corner of a particle's pixel box with the same `pixelSnapRound` instead of the
    centre, S49 R2/D4, so odd sizes cover whole pixels);
  - S50, S51 and S52 drawables (they include `pixel_snap.glsl` and call the same functions).

  Per vertex when the draw's model-view-projection is grid-aligned (its 2D linear part in pixel units
  is a signed permutation within a relative 1e-4: 0°, 90°, 180°, 270°, flips); otherwise only the
  draw's pivot is snapped (`pixelSnapCenterDelta`, Rotate On Grid) or nothing (a rotated draw under 90°
  steps never reaches the GPU rotated, R10). 3D meshes (MeshComponent without Sprite or Tilemap) never
  snap.
- **R10. Rotation policy.** Pixel Perfect Camera `rotationMode`: `Steps90` (default) or `OnGrid`.
  Sprite `pixelRotation`: `Camera` (default, the camera's), `Steps90`, `OnGrid`.
  - `Steps90`: for a draw whose 2D linear part is not grid-aligned, RenderSystem rebuilds the model
    matrix with its rotation rounded to the nearest 90° (polar decomposition of the 2×2 part, scale
    and flips kept, about the world pivot) before the MVP, so it is drawn on the grid; the Transform,
    physics, picking and Lua keep the authored angle.
  - `OnGrid`: the angle is kept, the pivot is snapped (`pixelSnapCenterDelta` / CPU twin), vertices are
    not. With Upscale Render Texture the rotated sprite lands on base pixels with nearest sampling (no
    mixels, no blur); without it, it rotates at screen resolution and the Inspector warns.
- **R11. Stretch Fill blit.** A single-pass sharp-bilinear variant of the blit and the uber pass
  (blit/uber feature bit `Psb`): `px = uv·B; seam = floor(px + 0.5); px = seam + clamp((px − seam)·s,
  −0.5, 0.5); uv = px/B` with a linear sampler, `s` = screen px per base px. Each base pixel's interior
  is one flat colour; only the screen pixels on a seam blend. No intermediate target.
- **R12. Sub-pixel smoothing.** Pixel Perfect Camera `subpixelSmoothing` (default off). The camera
  stays snapped (R8) and the residual f moves the final image by `s = round(f · k)` whole screen pixels
  per axis, so scrolling advances by screen pixels while every base pixel stays exactly k×k screen
  pixels and the art never leaves the base grid:
  - Upscale Render Texture on: T = B + 2 (R1); the blit's (or uber pass's) `uvScaleOffset` samples
    the inner B region offset by `s/k` base px; the viewport is unchanged (the margin hides the edge;
    `|s/k| ≤ 0.5`). Zero extra passes; the offset is applied before the uber pass's texel-centre snap.
  - Off: the projection is offset by `s/k` base px and the snap grid of R7 carries the same offset
    (`pixelSnap.offset`), so drawables snap before the offset and move with it.
  - Screen-to-canvas input mapping includes the same shift, so clicks hit what is seen.
- **R13. Shared edges.** Pieces that meet compute each shared edge once and read it from one value:
  - tilemaps (`MeshSystem::createTilemap`, and S50's grid builder): when the scene's main camera has
    an active Pixel Perfect Camera, each tile's local x0, x1, y0, y1 is rounded to the tilemap's texel
    grid (`1/PPU_drawable` units) before the quad is built (rebuilt when that changes), so neighbours
    are bit-identical and the vertex snap moves them together;
  - nine-slice (UI `Image` patch margins) keeps its shared indexed vertices (`UISystem.cpp:100-130`);
    any future sliced builder (S50, S51, S52) builds from one edge array per axis.
- **R14. Zero cost when off.** With no enabled Pixel Perfect Camera and no integer `Scaling`: no
  variant bit, no frame-block member read, no extra pass, target or allocation; the CPU cost is one
  cached pointer test per frame on the main camera.

### Platform & backends

- **R15. Five backends, vertex and fragment shaders only.** The snap is ALU in the vertex stage; the
  smoothing and Stretch Fill are a UV transform in the existing blit/uber fragment stage. GL's flipped
  targets: the snap uniform carries the flip sign so p is computed in the logical orientation (R7).
  D3D11/Metal/Vulkan/GL all put pixel centres at +0.5 window coordinates, so integer p is a pixel edge
  everywhere. No `precise` qualifier is relied on (absent from GLSL 3.30 / ES 3.00 outputs): the tie
  bias absorbs FMA contraction and matrix rounding (≤ 1e-3 px for targets ≤ 4,096 px).
- **R16. Displays.** Players request native-resolution surfaces (sokol_app `high_dpi`) on desktop,
  iOS and Android, so k is computed over physical pixels. On the web, while a Pixel Perfect Camera
  or an integer `Scaling` is active, the canvas backing store is its CSS size × `devicePixelRatio`
  (rounded) and the canvas element gets `image-rendering: pixelated`, so the browser never resamples
  the integer image.

### Performance (budgets; reference: desktop GTX 1060 class, Pixel 6a, iPhone 12, desktop Chrome)

- **R17. GPU.** The vertex snap adds ≤ 14 ALU instructions per vertex (measured from the compiled
  SPIR-V of `mesh.vert` with and without `Pxs`). The integer blit is the existing pass: ≤ 0.15 ms at
  2560×1440 on desktop and ≤ 0.6 ms at 2400×1080 on the Pixel 6a; Stretch Fill adds ≤ 10 % to it.
  The target at 320×180 (+2 with smoothing) costs ≤ 0.5 MB with its depth buffer.
- **R18. CPU.** Layout, camera snap and the frame-block members: ≤ 5 µs per frame. The 90°-step
  rebuild costs ≤ 0.1 µs per rotated draw and nothing for grid-aligned ones. Bench
  `pixel_perfect_bench` (10,000 sprites, 320×180, Upscale Render Texture on): frame CPU time with
  snapping on ≤ 102 % of off; GPU time ≤ 105 % of the same scene without snapping (the vertex stage).

### Editor UX (human)

- **R19. Inspector.** Add Component › Rendering › Pixel Perfect Camera (only offered on cameras; on a
  perspective camera it shows "Pixel Perfect Camera needs an orthographic or UI camera" and is inert).
  Fields with Unity-like names (§6.1); "Reference Resolution: ☑ Use Project Canvas (320×180)";
  read-only **Current Pixel Ratio** "4× — 320×180 in 1280×720, bars 0×0" for the Game view's size
  (in Play the live one); warnings: not the main camera of its scene, Rotate On Grid without Upscale
  Render Texture, the scene's Fixed Resolution overridden, PPU > 1 with UI in the same scene (S42's
  `texel_size` explains). Sprite and Tilemap Inspectors show **Pixels Per Unit** ("0 = Project (1)")
  and Sprite shows **Pixel Rotation**. Project Settings › Graphics shows **Pixels Per Unit** and the
  two new Scaling Mode entries "Integer (Letterbox)" and "Integer (Expand)". Every edit is one undo
  step through the existing property commands.
- **R20. Game view.** When the shown scene's main camera has an enabled Pixel Perfect Camera, the
  toolbar shows **Pixel Scale: Auto | 1× | 2× | … | 8×** (`GameViewSettings::pixelScale`, 0 = Auto;
  scales larger than fit are disabled) which forces k (R1 `forcedScale`) — 1× shows the base
  resolution at one physical pixel per base pixel, the "check at 1×" view — and a readout "320×180
  ×4". The view draws its framebuffer at exactly one framebuffer pixel per physical display pixel
  (image position rounded to physical pixels, no ImGui resampling); the Scale slider snaps to whole
  numbers and magnifies with nearest sampling. The edit-mode Game view preview applies the
  component as Play does.
- **R21. Scene view Pixel Zoom.** A 2D-toolbar toggle **Pixel Zoom** (display flag `pixel_zoom`,
  default on when the scene's main camera has a Pixel Perfect Camera, S42 also when Pixel Art Mode is
  on):
  - the wheel steps through 1/8×, 1/4×, 1/2×, 1×, 2×, 3× … 32× (k× = one base pixel over k physical
    pixels), anchored at the cursor; a readout ("3×") opens the list; **1×** and **Game** (the Game
    view's current k) buttons, commands `sceneView.pixelZoom1x` (Alt+1) and `sceneView.pixelZoomGame`
    (Alt+2) in S25's catalog (rebindable; S25's conflict test decides the defaults);
  - panning keeps the world's pixel grid on whole physical pixels (no shimmer);
  - **Pixel Grid** overlay (`pixel_grid`, from 6×): 1-px lines at 12 % alpha on art-pixel edges
    (`1/PPU` units);
  - **Snap Preview** (`pixel_snap_preview`, default on): the Scene view draws snapped sprites with the
    R7 rule on its own grid (B = view size / k, direct mode), so a half-pixel position shows where the
    game will draw it;
  - **Pixel Move Snap** (`pixel_move_snap`, default on in a scene with a Pixel Perfect Camera): the 2D
    Move and Rect tools round positions and sizes to whole pixels (`1/PPU` units; the pivot's drawn
    corner, R7, lands on the grid); Ctrl disables it while dragging (Unity's increment snap
    modifier is inverted here because on-grid is the norm).
- **R22. Gizmo.** The camera gizmo draws the reference frame (`R/PPU`), and in Integer Expand the
  frame the Game view's size currently shows (dashed), so level designers see the visible area.

### Agent UX (MCP, Lua, CLI)

- **R23. MCP, no new tool** (§6.4). Agents use `add_component`/`set_properties` (component
  `pixelperfectcamera`, YAML property names), `set_project_settings` (`graphics.pixels_per_unit`,
  `graphics.scaling_mode`), `set_scene_camera` (`zoom`, `display.pixel_zoom|pixel_grid|
  pixel_snap_preview|pixel_move_snap`), `configure_game_view` (new `pixel_scale` 0–8) and `screenshot`
  (the Game view at `pixel_scale: 1` is the 1× image). The component's runtime values (`pixel_ratio`,
  `base_resolution`, `target_resolution`, `viewport`, `snapped_position`, `subpixel_offset`,
  `cropped`) are read-only catalog properties shown by `get_entity` (computed for the Game view's
  size) and `get_runtime_entity` (live). S42's `check_pixel_art` reports the layout on a list of
  screen sizes.
- **R24. Lua.** §6.3: `PixelPerfectCamera(scene, entity)` (adds the component when missing, as
  `CanvasGroup`), `Camera:getPixelPerfectCamera()` (nil without), its properties and read-only
  values, `worldToPixel`, `pixelToWorld`, `roundToPixel`; `Engine.getPixelsPerUnit()`;
  `Sprite.pixelsPerUnit`, `Sprite.pixelRotation`, `Tilemap.pixelsPerUnit`; enums
  `PixelUpscaleMode`, `PixelRotation`, `Scaling.INTEGER|INTEGER_EXPAND`. Documented in
  `docs/mcp/guide/lua-api.md` with a ```` ```lua run ```` block (S23 doc test).
- **R25. Docs.** `docs/pixel-art.md` (user docs: Pixel Perfect Camera, PPU, the rule, rotation,
  smoothing, the views) and the "Camera and snapping" section of `docs/mcp/guide/pixel-art.md`
  (`jasper://docs/pixel-art`). S41 lands in wave 5A, before S42, so it creates the page with that
  section and its entry in `docs/mcp/guide/README.md`; S42 then owns the page and adds the rest
  (S42 §6.5).

### Data & compatibility

- **R26. Formats.** No version bump: every addition is an optional component or field with a default
  that reproduces today's output (S03 bump rule, `editor/StreamMigrations.h:19-21`).
  - Scene YAML: component key `pixelPerfectCamera` (§6.1); Sprite `pixelsPerUnit: 0`,
    `pixelRotation: camera`; Tilemap `pixelsPerUnit: 0` — written only when not default.
  - project.yaml: `graphics.pixelsPerUnit` (written when ≠ 1); `scalingMode: integer|integer_expand`.
  - `StreamSchema` key lists gain the new keys (unknown-key preservation, `StreamSchema.cpp:62`,
    `:72`); the S08 component codec table gains the component; the binary scene codec is unchanged
    for scenes without it.
  - Shader keys: `Pxs` is appended after each type's last bit (MESH, DEPTH, UI, PARTICLES) and
    `Psb` after BLIT's; the uber pass's sharp-bilinear variant is the next free `UberFeature` bit
    (`UBER_SHARP_BILINEAR`, name `Psb`). Every existing `shaderKeys` entry reads as before. Bit
    numbers are taken at landing, in landing order, with fixed names: S43's `UBER_PALETTE`, S48's
    `UBER_GLITCH`, S49's particle bits and S52's instancing bits append after whatever is there
    (ledger "Shared conventions"), so no spec hard-codes a number.
- **R27. Old projects** load and re-save byte-identically; a project with fixed resolution keeps it
  exactly (no Pixel Perfect Camera is added by S41; S42's preset offers it).

## 6. Design constraints & decisions

- **D1. A component for the camera, settings for the project.** Unity users look for Pixel Perfect
  Camera on the camera; Godot users look for an integer stretch mode in Project Settings. Both exist
  and share one layout function (R1), so they cannot disagree. The component wins over the scene's
  fixed resolution (it is the stronger statement) and over the project scaling for its scene.
- **D2. World units stay base pixels by default (PPU 1).** JasperEngine's 2D world is already in
  points, as Godot's, GameMaker's and Defold's; PPU 1 keeps every existing scene, Box2D scale, UI
  layout and input mapping unchanged and makes "move 1 unit" mean "move 1 pixel". PPU > 1 exists for
  Unity-style tile units and imported Unity projects; it scales world-space art (sprites, tilemaps,
  S51 world text) and the camera span. UI drawn by the same camera stays in camera units, which is why
  S42 measures texel size and the guide recommends UI in a layer scene when PPU > 1.
- **D3. Snap after projection, in base pixels.** One function serves every drawable kind and camera
  type (UI cameras are y-down in world space, ortho cameras y-up; base pixels are always y-up), works
  with any camera position, and keeps precision in large worlds (clip coordinates are near the
  camera). Spryte snaps translations before projection, which is equivalent for unrotated quads; we
  snap per vertex so non-integer pixel sizes (S42 flags them) still put every edge on the grid.
- **D4. The tie bias.** Half-pixel positions are the common case (CENTER pivots of odd sizes, halves
  of odd widths). `floor(p + 0.5)` alone makes CPU and GPU disagree when matrix rounding or FMA lands
  p a few ULPs either side of k + 0.5. Adding 2⁻⁷ moves every lattice value to k + 0.5078, far from
  both rounding errors (≤ 1e-3 px); the disagreement zone moves to an arbitrary value only continuous
  motion reaches, where a one-frame 1-px difference between two independent drawables is invisible
  and shared edges are bit-identical anyway (R13).
- **D5. Snap only through a variant bit.** A uniform branch would cost every 2D game; a variant costs
  nothing when unused (R14) and one extra variant per used combination, collected into `shaderKeys`
  like every bit. The snap parameters ride the frame block (per pass, already uploaded for library
  shaders), appended after `ambient` in both stages (`frame.glsl`'s contract); the built-in stages
  declare it only under `Pxs`.
- **D6. Sub-pixel smoothing is an offset in the existing pass,** quantised to screen pixels, so it is
  free on the GPU and the base grid is never broken: art pixels stay k×k; only the whole image moves.
- **D7. 90° steps by default.** The rule "no runtime rotation" breaks gameplay code that rotates
  sprites; rounding the drawn rotation keeps the code working and the art on the grid. Rotate On Grid
  is the honest opt-in for spinning coins and swinging arms: on the base target it is
  nearest-neighbour rotation with no mixels.
- **D8. Unsupported cases are inert, not errors.** Perspective cameras: no component effect. A camera
  with roll: its draws are not grid-aligned, so per-vertex snapping turns off and pivots snap
  (Rotate On Grid); S42 warns.
- **D9. Stretch Fill is sharp bilinear in one pass,** not Unity's integer-upscale-then-stretch two
  passes: the same look, no screen-sized intermediate target (mobile fill-rate).
- **D10. Code homes.** The component struct in `engine/core/component/` (with every component, so
  the ECS registry, S08 reflection and the codecs find it); the rule, layout and rotation code in
  `engine/core/pixelart/` (ledger convention); editor parts in `editor/pixelart/`.

### 6.1 Component and API sketch

```cpp
// engine/core/component/PixelPerfectCameraComponent.h
namespace jasperengine {
    enum class PixelUpscaleMode { INTEGER, INTEGER_EXPAND, STRETCH_FILL };
    enum class PixelRotation { CAMERA, STEPS_90, ON_GRID };   // CAMERA: sprites only (follow the camera)

    struct JASPERENGINE_API PixelPerfectCameraComponent {
        bool enabled = true;
        unsigned int referenceWidth = 0;        // 0 x 0: the project canvas
        unsigned int referenceHeight = 0;
        PixelUpscaleMode upscaleMode = PixelUpscaleMode::INTEGER;
        bool upscaleRenderTexture = true;
        bool pixelSnapping = true;
        PixelRotation rotationMode = PixelRotation::STEPS_90;
        bool subpixelSmoothing = false;
        Vector4 letterboxColor = Vector4(0, 0, 0, 1);
        // runtime, never saved
        pixelart::PixelPerfectLayout layout;
        Vector3 snappedPosition;
        Vector2 subpixelOffset;                 // base px, the residual f of R8
    };
}

// engine/core/pixelart/PixelPerfectLayout.h
namespace jasperengine::pixelart {
    struct PixelPerfectLayout {
        int scale = 1;                          // k
        int baseWidth = 0, baseHeight = 0;      // B
        int targetWidth = 0, targetHeight = 0;  // T (B, or B + 2 with smoothing)
        Rect viewport;                          // whole screen pixels
        float canvasWidth = 0, canvasHeight = 0;// B / PPU world units
        bool cropped = false;
    };
    PixelPerfectLayout computePixelPerfectLayout(int screenWidth, int screenHeight, int referenceWidth,
        int referenceHeight, PixelUpscaleMode mode, int forcedScale, bool smoothing, int pixelsPerUnit);
}
```

YAML (written keys only when not default):

```yaml
pixelPerfectCamera:
  enabled: true
  referenceResolution: [0, 0]      # 0,0 = project canvas
  upscaleMode: integer             # integer | integer_expand | stretch_fill
  upscaleRenderTexture: true
  pixelSnapping: true
  rotationMode: steps_90           # steps_90 | on_grid
  subpixelSmoothing: false
  letterboxColor: [0, 0, 0, 1]
```

### 6.2 The pixel-snap rule (the contract S42, S48–S52 honour)

```cpp
// engine/core/pixelart/PixelSnap.h (CPU twin of includes/pixel_snap.glsl; same float32 operations)
namespace jasperengine::pixelart {
    inline constexpr float kSnapBias = 0.0078125f;               // 2^-7 pixel
    inline float snap(float p) { return std::floor(p + 0.5f + kSnapBias); }

    // the frame block's pixelSnap / pixelSnapOffset members
    struct SnapGrid {
        float width = 0, height = 0;   // T: the drawing target in base pixels
        float flipY = 1;               // -1 when the pass's MVP is Y-flipped (GL render targets)
        float offsetX = 0, offsetY = 0;// R12 direct mode: s/k base px; 0 otherwise
    };
    Vector4 snapClip(const Vector4& clip, const SnapGrid& grid);         // per vertex
    Vector4 snapCenterDelta(const Vector4& clipCenter, const SnapGrid& grid); // add to every vertex
    bool isGridAligned(const Matrix4& mvp, const SnapGrid& grid);         // signed permutation, rel. 1e-4
    Matrix4 roundRotation90(const Matrix4& model);                       // R10 Steps90
    Vector3 snapWorld(const Vector3& world, int pixelsPerUnit);          // R8 camera, editor move snap
}
```

```glsl
// engine/shaderlib/includes/pixel_snap.glsl (FRAME_VS frame block members, after ambient)
//   vec4 pixelSnap;        // xy T, z flipY, w k (screen px per base px; read by S48/S49 effects)
//   vec4 pixelSnapOffset;  // xy offset (base px, R12 direct mode), zw 0
vec2 pixelSnapToPixels(vec4 clip){
    vec2 ndc = clip.xy / clip.w;
    ndc.y *= frame.pixelSnap.z;
    return (ndc * 0.5 + 0.5) * frame.pixelSnap.xy - frame.pixelSnapOffset.xy;
}
vec4 pixelSnapFromPixels(vec2 p, vec4 clip){
    vec2 ndc = (p + frame.pixelSnapOffset.xy) / frame.pixelSnap.xy * 2.0 - 1.0;
    ndc.y *= frame.pixelSnap.z;
    return vec4(ndc * clip.w, clip.zw);
}
float pixelSnapRound(float p){ return floor(p + 0.5 + 0.0078125); }
vec4 pixelSnapClip(vec4 clip){
    vec2 p = pixelSnapToPixels(clip);
    return pixelSnapFromPixels(vec2(pixelSnapRound(p.x), pixelSnapRound(p.y)), clip);
}
vec4 pixelSnapCenterDelta(vec4 clipCenter){   // the clip-space delta that puts a pivot on the grid
    vec2 p = pixelSnapToPixels(clipCenter);
    return pixelSnapFromPixels(vec2(pixelSnapRound(p.x), pixelSnapRound(p.y)), clipCenter) - clipCenter;
}
```

- **Space:** base pixels of the drawing target T (x right, y up from its bottom-left corner;
  with smoothing the target includes the 1-px margin), independent of backend flip.
- **When:** a draw of a camera with an active Pixel Perfect Camera and Pixel Snapping, of a kind
  listed in R9. Grid-aligned draws snap per vertex (`pixelSnapClip` on `gl_Position` after every
  displacement hook, before `invariant` output); other draws snap their pivot (`pixelSnapCenterDelta` of
  the model origin, added to every vertex) under Rotate On Grid; Steps90 draws are rotated on the CPU
  first (R10).
- **How CPU and GPU agree:** both evaluate `snap()` on p computed by the same expression in float32;
  the bias (D4) covers the ≤ 1e-3 px difference between CPU and GPU matrix products. CPU users: the
  camera (R8), tile and nine-slice edges (R13), the editor's move snap and Snap Preview (R21), S42's
  measurements, S52's CPU fallback, and the parity tests (AC3).
- **Order inside a vertex stage:** skinning/morph/lattice/displacement hooks → model → MVP →
  `pixelSnapClip` → output. A displacement hook that moves vertices after snapping breaks the rule and
  is not allowed in a `Pxs` variant (the depth program must snap identically).
- **Paths that build the transform per instance** apply the same rotation policy:
  - S52 instances compute cos/sin on the CPU, so the packer applies `roundRotation90` before it
    writes the rotation stream under Steps90 (S52 R11);
  - S49 particles build their rotation in the shader, so they use
    `pixelSnapRotationStep(vec2 axisX)` in `pixel_snap.glsl`, which returns the nearest 90° unit
    axis of the 2D linear part with the CPU's decision (ties at 45° go to the smaller angle);
  - either way, a Steps90 instance then snaps per vertex, and an OnGrid instance keeps its angle and
    snaps its pivot with `pixelSnapCenterDelta` (vertices unsnapped). There is no per-instance
    "no snap" flag: a draw without an active Pixel Perfect Camera simply has no `Pxs` bit.
- **Names other specs call:** CPU `pixelart::snap`, `snapWorld`, `snapClip`, `snapCenterDelta`,
  `isGridAligned`, `roundRotation90`, struct `SnapGrid`; GLSL `pixelSnapRound`, `pixelSnapToPixels`,
  `pixelSnapFromPixels`, `pixelSnapClip`, `pixelSnapCenterDelta`, `pixelSnapRotationStep`; frame
  block members `pixelSnap` (xy T, z flip, w k) and `pixelSnapOffset`. No caller re-implements
  `floor(p + 0.5)` without the bias.

### 6.3 Lua API (S23 `tools/lua_api`)

```lua
local ppc = PixelPerfectCamera(self.scene, cameraEntity)    -- adds one when missing
ppc.referenceResolution = Vector2(320, 180)                  -- Vector2(0, 0): the project canvas
ppc.upscaleMode = PixelUpscaleMode.INTEGER_EXPAND           -- INTEGER, INTEGER_EXPAND, STRETCH_FILL
ppc.upscaleRenderTexture = true
ppc.pixelSnapping = true
ppc.rotationMode = PixelRotation.STEPS_90                   -- STEPS_90, ON_GRID
ppc.subpixelSmoothing = true
ppc.letterboxColor = Vector4(0, 0, 0, 1)
-- read-only: pixelRatio (k), baseResolution, targetResolution (Vector2), viewport (Rect),
-- snappedPosition (Vector3), subpixelOffset (Vector2), cropped (bool)
local p = ppc:worldToPixel(Vector3(10.5, 4, 0))              -- base pixels, unsnapped
local w = ppc:pixelToWorld(Vector2(64, 32))
local onGrid = ppc:roundToPixel(go.position)                 -- the R7 rule on a world position
local ppu = Engine.getPixelsPerUnit()
sprite.pixelsPerUnit = 16; sprite.pixelRotation = PixelRotation.ON_GRID
```

`Camera:getPixelPerfectCamera()` returns nil without the component. Bound getters are properties
(`.claude/rules/scripting.md`).

### 6.4 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `configure_game_view` | `editor_ux` | new `pixel_scale` integer 0–8 (0 = Auto) | +60 (schema only; the meaning goes to `jasper://docs/pixel-art`) |
| `set_scene_camera` | `editor_ux` | none in the schema: `display` (free-form) gains `pixel_zoom`, `pixel_grid`, `pixel_snap_preview`, `pixel_move_snap`; `zoom` stays world units per view pixel | 0 |
| `set_project_settings` / `get_project_settings` | core | registry keys `graphics.pixels_per_unit`, `graphics.scaling_mode` values `integer`, `integer_expand` (data, not schema) | 0 |
| `add_component`, `set_properties`, `get_entity`, `get_runtime_entity` | core | component `pixelperfectcamera` and the Sprite/Tilemap fields through the catalog | 0 |
| — | `pixel_art` | S41 adds no tool to the shared group (S42's `check_pixel_art` reads S41's layout) | 0 |

- `pixel_art` group (tools from S42, S43, S46 and S48; ≤ 12 tools, ≤ 15,000 chars): S41 uses 0 of
  it. The group row (`McpToolGroups.cpp`) is added by S43, the first spec with a tool in it (wave
  5A), with S43's text (S43 §6.1). The family's tool table is in the ledger ("MCP budget").
- `editor_ux` stays within its 24,000 / 21 entry (`tools/mcp/budget.json`); if
  `configure_game_view` passes 1,500 characters, its per-value text moves to the guide page.
- Agent guide: the `## 2D and 3D` section (≈ 1,250 bytes today) has no room for the family's lines,
  so S41 creates one new digest section, `## Pixel art` (conventions: "one new digest section"),
  that every family spec adds its one line to (byte shares in the ledger, the section ≤ 1,300
  bytes). S41's line: "Pixel art: Pixel Perfect Camera component (Pixels Per Unit, snapping, Pixel
  Scale): jasper://docs/pixel-art." (≈ 100 bytes). It names no tool S41 does not register
  (`test_autosync.py`); S42 adds `check_pixel_art` to the same line when it lands. S42 owns the
  section's wording afterwards.

## 7. Acceptance criteria

Unit tests are gtest under `engine/tests/core/pixelart/` and `editor/tests/pixelart/`; GPU tests run
on the headless device (`engine/tests/support/GpuEnvironment.h`) on glcore and Vulkan (lavapipe) in
CI, with D3D11 and Metal in the platform CI jobs and WebGL2 through the S02 web gate. The fixture
project is `tools/fixtures/pixel-art/` (CC0 sprites made for it: 16×16 hero with an odd-width 15×16
frame, 16×16 tiles, a nine-slice panel, a 5×7 bitmap text sheet).

| AC | Verification |
|---|---|
| AC1 (R1, R2) | `PixelPerfectLayoutTest.Table`: every row of a 40-row table (screens 320×180 … 3840×2160, 1366×768, 2400×1080, 1280×800, 800×600, 300×100; references 320×180, 384×216, 256×224; all modes, forced 0/1/3, smoothing on/off, PPU 1/16) gives the listed k, B, T, viewport, canvas and `cropped`. `EngineViewLayoutTest.IntegerModes`: `computeViewLayout` with `INTEGER`/`INTEGER_EXPAND` equals the component layout at PPU 1. |
| AC2 (R7, D4) | `PixelSnapTest`: `snap` on the half-pixel lattice (k + 0.5 for k in −4096…4096) returns k + 1; values k + 0.5 ± 1e-3 return k + 1 too; `snapClip` with `flipY = −1` returns the mirror of `flipY = 1` (no tie asymmetry); `isGridAligned` is true for 0/90/180/270° and flips, false for 0.01°; `roundRotation90` keeps scale and flips. |
| AC3 (R7, R9, §6.2) | `PixelPerfectGpuTest.SnapParity`: 4,096 one-pixel quads at seeded random and half-lattice positions drawn through a Pixel Perfect Camera (snapping on): every quad's lit pixel equals the CPU prediction of `snapClip` on every backend (0 mismatches). **Red control:** the same quads with Pixel Snapping off land on the predicted pixel for fewer than 60 % of the half-lattice positions (the test asserts it, so a snap that silently does nothing fails). The test also logs how many lattice quads disagree with the bias forced to 0 in both twins, the measured reason for D4 (recorded in Deviations). |
| AC4 (R3, R9, R13) | `PixelPerfectGpuTest.ReferenceParity` (the method of §4): the fixture scene (hero at a half-pixel position with a CENTER pivot, tiles, nine-slice panel, text, a 90°-rotated and a flipped sprite) rendered through the component equals a CPU compositor (`editor/tests/pixelart/PixelReference`: texels blitted at the R7-snapped positions, nearest, alpha over, then a nearest ×k upscale) with **0 differing pixels** at k = 1, 3, 4 and in Integer Expand at 1366×768. **Red control:** the same scene with Pixel Snapping off differs in ≥ 1 column. |
| AC5 (R1, the uneven-pixel failure) | `PixelPerfectGpuTest.EvenColumns`: a 1-px checkerboard sprite upscaled through the component at 1366×768 and 1920×1080: every run of equal pixels in the readback is exactly k wide and k tall (`PixelGridProbe`). **Red control:** today's fixed resolution (Fit) at 1366×768 gives runs of 2 and 3. |
| AC6 (R13) | `PixelPerfectGpuTest.TilemapSeams`: a 64×36 tile floor with tile positions offset by 1e-4 and a camera swept over 10,000 fractional positions: no background pixel shows between tiles and no tile column is drawn twice at any position. **Red control:** without the edge rounding, at least one gap appears. |
| AC7 (R10) | `PixelPerfectGpuTest.Rotation`: a sprite at 37° under Steps90 equals the 0° render (it is the nearest step), and its Transform still reads 37° in Lua; under OnGrid with the render texture the result matches a CPU nearest-neighbour rotation about the snapped pivot within 8 edge pixels (spryte's GPU trigonometry tolerance) and contains no partially covered base pixel. |
| AC8 (R12) | `PixelPerfectGpuTest.Smoothing`: camera at x = 10.0, 10.25, 10.5, 10.75 base px with k = 4: the image shifts by 0, 1, 2, 3 screen px (`round(f·k)` per R12), every base pixel stays 4×4, the inner image equals the 10.0 frame shifted; in direct mode the same shifts with the sprites still on their grid. `EngineInputTest.SmoothingShift`: a click maps to the world point drawn under it. |
| AC9 (R11) | `PixelPerfectGpuTest.StretchFill` golden at 1366×768: every base pixel's interior is flat (each interior screen pixel equals the source texel), seam pixels are blends of the two neighbours only. |
| AC10 (R4, R5) | `PixelPerfectGpuTest.FixedResolutionDriven`: with the scene's fixed resolution 640×360 and a component at 320×180, SSAO's and the lighting viewport's sizes read 320×180; removing the component restores 640×360 exactly. A HUD layer scene with its own component draws its 1-px border on the main scene's base grid (pixel-exact against the reference). |
| AC11 (R14) | `PixelPerfectOffTest.ZeroCost`: a 2D scene without the component: the frame's pass list, draw count and `shaderKeys` are identical before and after this spec's build (checked against a stored list), and the frame allocates nothing extra (the core tests' allocation counter). |
| AC12 (R6) | `SpritePpuTest`: a 16×16 sprite with project PPU 16 has a 1×1-unit quad and bounds; with sprite PPU 32 a 0.5×0.5 quad; PPU 0/1 scenes produce the same vertex buffers as before (byte compare). Colliders fitted to sprites, picking bounds and Lua world-size getters use `spriteWorldSize`. |
| AC13 (R15, R16) | The GPU suite above passes on glcore, GLES3, Vulkan, D3D11, Metal and WebGL2 (S02 web gate) with the same goldens; `tools/build.sh web` and `android` pass; the web shell test checks the canvas backing store equals CSS size × devicePixelRatio with `image-rendering: pixelated` while the component is active. |
| AC14 (R17, R18) | `tools/perf/pixel_perfect_bench` on the reference machines: the table of R17/R18 filled in this spec's Deviations (before/after); SPIR-V instruction count delta of `mesh.vert` with `Pxs` ≤ 14. |
| AC15 (R19–R22) | `tools/mcp/tests/test_pixel_perfect_editor.py` (xvfb, isolated editor): add the component with `add_component`, read `pixel_ratio` 4 at a 1280×720 Game view, `configure_game_view {pixel_scale: 1}` then `screenshot` gives 320×180 content at 1:1 (pixel-exact against the AC4 reference); `set_scene_camera {display: {pixel_zoom: true}}` and a wheel step through `send_input` land on 3× from 2×; Pixel Move Snap rounds a dragged position to whole pixels; undo removes the component in one step. Screenshots of the Inspector, Game view toolbar and Scene view grid in the evidence. |
| AC16 (R23, R24) | `tools/mcp/tests/test_pixel_perfect_lua.py` (`test_script.py` style): a script sets every property, reads `pixelRatio`, `baseResolution`, `snappedPosition`, `roundToPixel` (equals `snapWorld`), `Camera:getPixelPerfectCamera()` nil before/handle after; `tools/lua_api/tests` find the bindings; the doc test runs the guide's lua block. |
| AC17 (R26, R27) | `test_runtime_regressions.py` and `editor/tests/StreamRoundTripTest`: every existing fixture scene and project re-saves byte-identically; a scene with the component round-trips; unknown future keys under `pixelPerfectCamera` survive. |
| AC18 (§6.4) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py` (`"configure_game_view"` with `pixel_scale` in a test), `gen_tool_docs.py --check`, `test_protocol.py` (guide size). |

## 8. Dependencies & conflicts

- **Depends on (landed):** S13 (uber pass reads the fixed target; the offset joins its
  `uvScaleOffset`), S08 (component reflection and catalog), S23 (`tools/lua_api`), S25 (settings
  registry, command catalog), S26 (Scene view toolbar and display flags), S31 (budget gates), S02
  (GPU suites, web gate), S32 (variants created asynchronously: a new `Pxs` variant shows a stand-in
  for a frame, never a wrong snap).
- **Family order:** wave 5A with S43, S44 and S52; nothing in the family is needed first.
- **Blocks:** S42 (reads the layout, PPU, rotation policy and rule), S48 (effects on the base target),
  S49 (particle snapping and pixel sizes), S50 (shared-edge tile builder), S51 (glyph quads on the
  grid), all in later waves; S52 (same wave: must call `pixelSnapClip`/`pixelSnapCenterDelta`
  identically, AC3/AC4 harness reused; S52 D10 covers either landing order).
- **Merge hotspots:** `RenderSystem.cpp` (fixed-resolution sites, `updateCamera`, `objectMVP`, blit
  and uber), `Engine.cpp` (`computeViewLayout`), `MeshSystem.cpp` (`createSprite`, `createTilemap`),
  `ShaderVariantCompiler.cpp` and `ShaderPool.cpp` (bits; S49, S52 add theirs after), `mesh.vert`,
  `surface_vs.glsl`, `ui.vert`, `particles_vs.glsl`, `frame.glsl` (append-only members),
  `GameWindow.*`, `SceneWindow.cpp`, `SettingsRows.cpp`, `StreamComponents.cpp`, `StreamSchema.cpp`,
  `Generator.cpp`, `McpToolGroups.cpp`, `McpInstructions.cpp`, `docs/mcp/guide/pixel-art.md` (S42).

## 9. Risks & defaults

- **PPU > 1 with UI in the same scene.** UI is in camera units; with PPU 16 a 16-px icon sized in
  pixels becomes 16 units. Default: PPU 1 (and S42's preset keeps it); the Inspector warns and S42's
  `texel_size` rule measures it; the guide recommends a HUD layer scene.
- **CPU/GPU disagreement near the biased tie.** Only continuous motion reaches it and only between
  independent drawables. Default: accept; AC3 measures the real rate on every backend and the
  deviation is recorded if any backend fails on lattice values.
- **Variant count.** One bit per vertex type, used only in pixel-perfect scenes. Default: accepted;
  S32's async creation and warm-up list cover the first frame.
- **Integer Expand shows more world on wide screens,** which can reveal level edges. Default: Integer
  (letterbox) is the component default; Lua exposes `baseResolution` for gameplay that needs it.
- **Screens smaller than the reference** (a 320×180 game in a 300×100 window): k = 1 with a centred
  crop and one log line. Default: crop, never a fractional downscale.
- **Zoom.** No zoom field: changing `referenceResolution` at runtime reallocates the target (once per
  change). Default: documented; a smooth zoom is out of scope.
- **Camera roll** draws unsnapped (D8). Default: S42 warns; no special path.
- **In-scene HUD with smoothing** moves with the image's sub-pixel shift (≤ 0.5 base px). Default:
  the guide puts HUDs in a layer scene, which is not shifted.
- **Sprite world-size readers** that use `width/height` directly would be wrong at PPU ≠ 1. Default:
  AC12 lists them; a grep for `.width` on `SpriteComponent` in review.

## Milestones

- **M1** `PixelSnap`, `PixelPerfectLayout`, `roundRotation90`, `snapWorld` and their unit tests
  (R1, R7, R8 math); AC1, AC2.
- **M2** The component, Engine layout takeover, `fixedResolutionSize()`, integer blit and letterbox
  colour, `Scaling::INTEGER|INTEGER_EXPAND`, PPU on Sprite/Tilemap (R2–R6, R14); AC5, AC10–AC12.
- **M3** `pixel_snap.glsl`, the `Pxs` variants (mesh, depth, UI, particles), rotation policy, tilemap
  edge rounding (R9, R10, R13, R15); AC3, AC4, AC6, AC7.
- **M4** Sub-pixel smoothing, Stretch Fill, direct mode, layer scenes, web/high-DPI surfaces (R5,
  R11, R12, R16); AC8, AC9, AC13.
- **M5** Editor: Inspector, Game view Pixel Scale and 1:1 display, Scene view Pixel Zoom, grid, Snap
  Preview, Move Snap, gizmo (R19–R22); AC15.
- **M6** Lua, MCP parameter, docs, budgets, bench, compatibility (R17, R18, R23–R27); AC14, AC16–AC18.

## Deviations


Recorded by the implementation (branch `feature/s41-pixel-perfect-camera`).

1. **Guide entry (the ledger's upstream-rule override).** The agent guide gets no `## Pixel art`
   section: the upstream rules of 2026-11-04 give a new area one entry of the "Other areas" index.
   It read "pixel art (a camera's Pixel Perfect Camera component): jasper://docs/<area>", and is
   cut since (deviation 26: its case passes without it, and the guide's byte budget). It
   names no tool: `check_pixel_art` does not exist before S42, and `test_autosync.py` refuses a
   guide name that does not exist; S42 adds it to the entry. Its eval cases are in
   `tools/mcp/eval/cases.py` (`guard_pixel_perfect_camera`, `guard_game_view_pixel_scale`).
2. **YAML key and fields.** The component's key is `pixelperfectcamera`, lowercase as every other
   component key (the YAML and MCP key are one, `ComponentDesc::key`); the sketch's
   `pixelPerfectCamera` names it in MCP and Lua calls (a key alias), not in a scene file. The reference resolution is stored as the struct's
   two fields `referenceWidth` and `referenceHeight` (the sketch's `referenceResolution: [w, h]`);
   Lua keeps `referenceResolution` as a `Vector2`. The read-only layout values are catalog
   properties in the catalog's camelCase: `pixelRatio`, `baseWidth`, `baseHeight`, `targetWidth`,
   `targetHeight`, `viewport`, `cropped`, `snappedPosition`, `subpixelOffset` (R23's `pixel_ratio`,
   `base_resolution`, `target_resolution`).
3. **The per-draw snap mode.** The vertex stage needs to know, per draw, whether to snap every
   vertex or the pivot (or nothing: a shadow map, a pass without a Pixel Perfect Camera). It is a
   `vec4 pixelSnapDraw` appended to the draw's own block (`u_vs_pbrParams`, `u_vs_depthParams`,
   `u_vs_uiParams`) in `Pxs` variants only, `x` = 0 none, 1 every vertex, 2 the pivot; the pivot
   is the MVP's origin column (`mvpMatrix[3]`, an instance's `mvpMatrix * i_matrix_col4`). The
   include adds `pixelSnapApply(clip, mode, pivot)`. Particles have no per-draw block: they snap
   when the frame's `pixelSnap.x > 0`, and `pixelSnapOffset.z` is 1 when the pass's view is on the
   grid (no camera roll), so a quarter-turned quad snaps its corners (§6.2 had `zw 0`).
4. **Direct mode (Upscale Render Texture off).** T = B there (no smoothing margin: sub-pixel
   smoothing moves the projection, R12 "Off"). Stretch Fill needs the target's blit, so without it
   the camera lays out and draws as Integer. Bars of another colour than the background cost one
   pass: it clears the destination to the Letterbox Color and fills the viewport with the
   background (an 8 x 8 texture of it), and the colour pass loads it.
5. **Engine canvas.** `Engine::getCanvasWidth/Height` are integers: the canvas is B / PPU rounded
   to whole units when the PPU does not divide the base; the camera's span is the exact T / PPU.
6. **Test names and homes.** The GPU suites are `PixelPerfectGpu`, `PixelPerfectOffGpu` and
   `PixelPerfectBenchGpu` (docs/testing.md: a GPU suite ends in `Gpu`), in
   `engine/tests/core/pixelart/` with the CPU suites `PixelSnapTest`, `PixelPerfectLayoutTest`,
   `EngineViewLayoutTest`, `SpritePpuTest`, `PixelSnapShaderTest`. The CPU compositor of AC4 is in
   the GPU test itself: the GPU suites run in the engine's test tree, the editor tests have no
   device.
7. **AC3, the bias.** The rule's bias reaches the vertex stage as `frame.pixelSnapOffset.w` (2^-7,
   the CPU twin's constant): `PixelPerfectGpu.SnapParity` sets it to 0 for its red control, and a
   third of its quads lie just under the half pixel, where only the bias rounds them up.
8. **AC4 reference.** The compositor draws ring sprites (one texel of border, so any flip or
   quarter turn draws the same texels): a CENTER-pivoted 15 x 16 sprite at a half pixel, a
   quarter-turned and a flipped 15 x 7 sprite and a floor of ten 16-pixel tiles, against layouts
   written by hand (k = 1, 3, 3 with bars, 4, and Integer (Expand) at 1366 x 768). UI is checked by
   `PixelPerfectGpu.UiImageParity` (1-pixel Images at k = 1 and 3 against the CPU twin). The
   nine-slice panel and the text are left out of the compositor: a CPU reference of their texels
   would need the UI's own nine-slice and glyph rasterisation; they draw with the same rule.
9. **AC6 sweep.** 74 camera positions (64 even ones plus the neighbourhoods of the rounding ties)
   instead of 10,000: each needs a frame and a readback. The red control draws without the rule at
   positions where the unsnapped edges straddle pixel centres.
10. **AC9.** The Stretch Fill check is analytic (each interior pixel equals its texel, each seam
    pixel lies between its two texels) instead of a golden image.
11. **AC10.** The fixed-resolution size is checked through the colour pass's render area (the
    pass list); SSAO, SSR, the lighting viewport, LOD and point sizes read the same
    `fixedResolutionWidth/Height()`.
12. **Rotation policy coverage.** 90 Degree Steps applies to sprites (their `pixelRotation`),
    tilemaps and UI elements (the camera's policy); a sprite's `CAMERA` follows the camera.
13. **Scene view.** The Pixel Zoom, Pixel Grid, Snap Preview and Pixel Move Snap flags default to
    on and take effect only in a 2D scene whose main camera has an applicable Pixel Perfect
    Camera. The toolbar has the Pixel Zoom toggle and a menu beside it with Pixel Grid, Snap
    Preview and Pixel Move Snap (and MCP's `display_settings`). Snap Preview's grid is the view's
    pixels over the zoom's scale at any zoom. The camera frames of R22 are drawn as an overlay of
    the Scene view (a 2D scene's UI camera has no frustum gizmo).
14. **Lua.** The handle has no `getGameObject()` (Camera already is one).
15. **AC6 floor size.** The floor is 40 x 23 tiles of 8 pixels (a 320 x 184 floor over the 320 x 180
    base) instead of 64 x 36 tiles of 5: a tilemap holds at most `MAX_TILEMAP_TILES` (2,048) tiles.
16. **R17, the vertex snap's instruction count.** `PixelSnapShaderTest.VertexCost` counts 11 SPIR-V
    arithmetic instructions on the per-vertex path (`pixelSnapClip` with its helpers; glslang
    without an optimizer, each helper counted): the vertex stage gets half the target and where NDC
    0 lands (`pixelart::snapTerms`, computed once on the CPU), so a pixel is `ndc * half + centre`,
    and both axes round in one `vec2` step with the bias's two additions. The whole `mesh.vert`
    sprite variant goes from 6 to 19 counted instructions. The test pins at most 14.
17. **R17, R18, measured.** On this host only (llvmpipe, no reference device), so the GPU timings of
    R17 stay a tracked follow-up:
    | Measure | Budget | Measured |
    | --- | --- | --- |
    | `pixel_perfect_bench` frame CPU, snapping on vs off (`PixelPerfectBenchGpu.SnappingFrameCost`) | ≤ 102 % | 102.0 % and 104.2 % in two runs on a shared, loaded host (the test applies `perfSlack()`) |
    | `mesh.vert` per-vertex snap path, SPIR-V | ≤ 14 | 11 (item 16) |
    | GPU time, blit times on the reference devices | R17 | not measured: no reference GPU here |
18. **AC13 coverage.** The core GPU suites run on glcore, Vulkan (lavapipe: there the pass's
    matrices are not Y-flipped, so the rule's `flipY = +1` path runs) and GLES3 (an OpenGL ES 3.2
    context on llvmpipe, `GRAPHIC_BACKEND=gles3 tools/build.sh unit-tests`, added for this spec in
    the linux-backends image). D3D11 and Metal have no device on this host. The web half is the
    gate `GATE=pixel-perfect` (test project `tools/test-projects/pixel-perfect`, Chrome at a device
    scale factor of 2, through a new `chrome_flags` key of the gate baselines).
19. **AC17 homes.** `editor/tests/pixelart/PixelPerfectStreamTest.cpp`: the round trip, the kept
    unknown keys, and every scene (75) and `project.yaml` (31) of `tools/fixtures` and the editor's
    test fixtures re-saved without S41's keys and stable on a second save
    (`FixturesResaveWithoutS41Keys`); the AllComponents fixture gains the component's entity and the
    Sprite and Tilemap keys, so `ReflectFixtures.AllComponentsScene` and the `CookedScene` round
    trips cover them. A fixture is compared with its own re-save, not byte for byte with the file:
    older fixtures are not in today's output format whatever S41 does.
20. **Editor checks and the main camera.** In the editor a Scene's camera is the view's own camera,
    so the Scene view's pixel tools and the "not the main camera" warning read the SceneProject's
    `mainCamera`.
21. **The Engine's view and the main scene.** `Engine::setScene` with another scene (or none) and
    `Engine::removeScene` of the main scene drop the Pixel Perfect Camera's view; the new main
    scene's camera sets its own on its update. Without it a scene switch kept the old scene's
    integer layout (`EngineViewLayoutTest.ThePixelPerfectViewGoesWithItsMainScene`).
22. **Variant bits and S55.** `Pxs` is MESH bit 45 and DEPTH bit 19 (S57 and S55 took MESH 36-41 and
    DEPTH 18, the water interaction MESH 42-43, draw-perf MESH 44), UI bit 5, PARTICLES bit 11;
    Stretch Fill is BLIT bit 18 (`Psb`). The per-draw
    `pixelSnapDraw` is part of the per-draw object blocks (`includes/object_params.glsl`,
    `object_depth_params.glsl`), so a pixel-snapped drawable never joins an automatic GPU
    instancing batch (`getSubmeshBatchVariant`).
23. **The frame block's members (R14).** `pixelSnap` and `pixelSnapOffset` come last in `frame_t`,
    after the fragment stage's `postColor`, and only a `Pxs` vertex stage declares them (with the
    post-colour vectors before them, unread): every other program's frame block, and the bytes
    uploaded to it, are what they were. `pixelSnap` holds half the target (`xy`), `flipY` and k;
    `pixelSnapOffset` where NDC 0 lands (`xy`), the on-grid view flag and the bias. The editor's
    shader cache moves to `v56` (`v53` to `v55` are draw-perf's, SSR's and PCSS's).
24. **Shadow maps and uploads.** A depth draw into a shadow map never snaps (its frame block holds
    the camera's grid; `RenderSystem::depthSnapMode`). A block the engine extends with
    `pixelSnapDraw` is uploaded at most at the shader's reflected size (a fork without the member).
25. **AC11's reference.** `PixelPerfectOffGpu.ZeroCost` compares within one process: the frame
    without the component (printed: passes, draws, variants, allocations, pixels) against the same
    frame with the component disabled, and after it was added and removed. A list stored from main
    ages with every change on main: main's dynamic batching took the scene from 4 draws to 3, with
    the same pixels (the image hash recorded on main before S41, 13037468019309210060, is still
    this build's without the component).
26. **The eval of S41's MCP text** (`tools/mcp/eval/run.py`, sonnet, an Orquester-lent account; the
    surface recorded from this branch's editor, and a copy without S41's text):
    | Case | With the text | Without it |
    | --- | --- | --- |
    | `guard_game_view_pixel_scale` (`pixel_scale`'s description) | 5 of 6 pass | 3 of 6 pass (the case fails: half its samples) |
    | `guard_pixel_perfect_camera` (the "Other areas" line) | 2 of 2 pass | 6 of 6 pass |
    The `pixel_scale` description earns its bytes. The guide line does not change this case: the
    agent finds the component through `list_component_types` without it. After S44's sprites line
    the agent guide passed its byte budget (`tools/mcp/budget.json`), so the "Other areas" pixel
    art line is cut; the case stays as a guard of the component's name.
27. **S44's sheets.** An `.aseprite` sheet whose `pixelsPerUnit` is `project` imports with the
    project's Pixels Per Unit (S44 used 1 until S41 landed), and changing the project's value
    imports those sheets again. A sheet Sprite's own Pixels Per Unit, when not 0, wins over its
    sheet's, as it does over the project's for a texture Sprite.
