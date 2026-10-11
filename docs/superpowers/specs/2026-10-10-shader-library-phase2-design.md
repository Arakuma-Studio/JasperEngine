# Shader library, phase 2: extra pass and stencil

Status: approved scope, refined against the merged phase 1 code (shader-library 153c9c60).
The data declarations of §2.0 are committed (contract commit); implementation is phase 2 work.
Depends on: phase 1 (`2026-10-10-shader-library-phase1-design.md`), whose contracts
(shader references, manifest, material fields, render-state resolution, frame and lights
includes, `surface_vs.glsl`) this phase extends and must not redefine.

## 1. Goal

Make outlines and stencil effects first-class, the two most reused techniques in the
owner's games after matcap (QuickOutline appears in 7 repos; inverted-hull outlines and
"draw once" translucency in several more), without costing anything for objects that do
not use them.

## 2. Requirements

### 2.0 Committed contract (do not rename)

`engine/core/texture/Material.h`: `MaterialStencilCompare` (AUTO, DISABLED, NEVER, LESS,
EQUAL, LESS_EQUAL, GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS), `MaterialStencilOp` (KEEP,
ZERO, REPLACE, INCR_CLAMP, DECR_CLAMP, INVERT, INCR_WRAP, DECR_WRAP), `MaterialStencil`
(compare, ref, readMask, writeMask, pass, fail, depthFail), `MaterialColorWriteBits`
(`colorWrite` = -1 AUTO or a 4-bit RGBA mask), `MaterialPassOrder` (AFTER, BEFORE),
`MaterialPass` (shader, shaderParams, shaderTextures, shaderFeatures, blendMode, cullMode,
depthWrite, depthTest, depthOffsetFactor, depthOffsetUnits, stencil, colorWrite, order);
`Material::stencil`, `Material::colorWrite`, `Material::extraPass`; `stencil` and
`colorWrite` on `UIComponent`, `PointsComponent`, `LineRendererComponent`.

Resolution follows phase 1 §5.2: explicit value -> manifest (`@state[...]` keyword rules,
then base `@state`) -> legacy (stencil off, all colour channels). A stencil `compare` other
than AUTO makes the whole `MaterialStencil` explicit; AUTO takes the manifest's stencil
rule wholesale. The extra pass resolves its state the same way against its own shader's
manifest. Phase 1 conventions apply unchanged (blend define `BLEND_*`, `finalizeColor` /
`finalizeStraightColor`, `@require ... feature=`, `@state shadows=off`, §13 of the phase 1
spec).

### 2.1 Stencil

- Every colour pass that draws meshes has a depth-stencil attachment on every backend
  (swapchain and offscreen targets, including fixed-resolution and render-to-texture
  cameras). Depth and stencil are cleared together at pass start.
- Render state (phase 1 section 5.2) gains stencil: `stencilRef` (0-255), `stencilReadMask`,
  `stencilWriteMask`, `stencilCompare` (never, less, equal, lequal, greater, notequal,
  gequal, always), `stencilPass`, `stencilFail`, `stencilDepthFail` (keep, zero, replace,
  incr-clamp, decr-clamp, invert, incr-wrap, decr-wrap). AUTO/default = stencil disabled.
  Same state for front and back faces.
- Render state also gains `colorWrite` (rgba mask; `none` for mask-only draws).
- Manifest `@state` (and keyword-conditional `@state[...]`) accepts `stencil=<compare>`
  (`off` or a compare name), `ref=`, `readmask=`, `writemask=`, `pass=`, `fail=`,
  `zfail=` (op names: keep, zero, replace, incr, decr, invert, incrwrap, decrwrap) and
  `colorwrite=<none|rgba|rgb|a|any subset>`, on material, UI, points and lines components.
- The engine `RenderState` (`engine/core/render/Render.h`) and `SokolObject` pipeline
  building carry stencil and colour mask for every pipeline variant; pipelines differing
  only in stencil ref still need distinct pipelines in sokol (the ref is pipeline state),
  which is acceptable because refs are per material.
- Depth pre-pass, shadow and G-buffer passes ignore stencil and colour mask (they keep
  their own state); a material that writes stencil and is in the depth pre-pass must still
  write stencil in its colour pass.
- Sorting: objects with the same queue keep a deterministic order so mask writers can be
  placed before readers with `sortPriority`; opaque-queue objects honour `sortPriority`
  before front-to-back distance when it is non-zero.

### 2.2 Extra pass

- `Material::extraPass` (§2.0): one optional extra draw per submesh.

- The extra pass is drawn immediately before or after the submesh's main draw, in the
  same queue position, with its own pipeline, uniform blocks and textures. It casts no
  shadows and is skipped by the depth pre-pass and the G-buffer pass.
- No `ObjectRender`, pipeline or uniform storage is allocated for submeshes without an
  extra pass.
- Works with skinning, morph targets, instancing and sprites.
- The extra pass's shader receives the same mesh variant bits (skinning, morph,
  instancing, texture rect, fog, vertex colour) as the main draw, the frame block, and the
  object's light list if its manifest asks for lighting; its parameters/textures/features
  are its own. The export collector precompiles its variants.
- Editor: an "Extra pass" section in the material inspector with its own shader picker,
  parameters and state; serialization (scene YAML, `.material`, prefabs, model-reload
  override flags), export (Factory codegen), Catalog paths
  (`submeshes[N].material.extraPass.*`, `...stencil.*`, `...colorWrite`), Lua
  (`material.extraPass`, `material.stencil`, `material.colorWrite`, and the component
  fields) and MCP follow phase 1 section 10.

### 2.3 Smoothed normals

- `@require smoothnormals` makes the mesh provide a per-vertex normal averaged over all
  vertices sharing a position (so hard-edged meshes outline without gaps), as a new
  attribute `a_smoothNormal` computed once at load and cached, only for meshes whose
  material or extra pass requires it.

### 2.4 Library shaders

| Name | Technique |
|---|---|
| `outline` | Inverted hull: extrude along the smoothed normal; `@keywords WIDTH_MODE SCREEN WORLD` (constant pixel width computed in clip space with aspect correction, or world units with optional distance scaling); colour; default `cull=front`, order before/after configurable. |
| `outline_stencil` | QuickOutline style: the main pass writes stencil ref (preset), this pass draws the extruded hull where stencil != ref, so the outline never covers the object; works through walls with `ztest=always` keyword. |
| `silhouette` | Draws the parts of an object hidden behind others in a flat colour or fresnel (`ztest=greater`, stencil to avoid self-overlap). |
| `stencil_mask` | Writes the stencil reference with colour writes off (portals, windows, sprite masks). |
| `sprite_mask` | Sprite-shaped stencil mask with alpha cutoff, for Unity-style sprite masking. |

### 2.5 Presets

The material inspector offers one-click presets that set shader, extra pass and state:
Outline (hull), Outline (stencil), See-through silhouette, Draw-once transparency
(stencil not-equal + replace, so overlapping transparent parts of one object do not stack),
Stencil mask, Visible inside mask, Visible outside mask, Sprite mask. Presets are data
(a table in the editor), not new engine features.

## 3. Verification

- The phase 1 compile matrix covers the new shaders and the extra-pass variants on all six
  backends.
- The shader-library fixture gains a scene group showing every preset (outlined skinned
  character, outlined sprite, masked sprites, see-through silhouette, draw-once translucent
  object) and renders without errors in the smoke test.
- Phase 1 regression checks still pass; scenes without stencil or extra passes render
  identically and allocate nothing new per submesh.

## 4. Definition of done

All requirements implemented, all checks pass, merged into `shader-library`, review pass
found no open correctness bugs.

## 5. Implementation notes

Decisions taken while implementing phase 2, and deviations from the text above. They
override the sections they refer to; later agents follow them.

### 5.1 Stencil (sl/p2-stencil)

- Attachments. The swapchain already was `DEPTH_STENCIL` on every platform (GLX, WGL, NSGL
  and Emscripten ask for 8 stencil bits, Metal views use `Depth32Float_Stencil8`, the D3D11
  and Vulkan contexts create D24S8 / D32S8) except Android, whose EGL config now asks for
  `EGL_STENCIL_SIZE 8` (falling back to a config without stencil, where stencil tests
  always pass). Offscreen: `Framebuffer` creates a `DEPTH_STENCIL` attachment by default
  (camera textures, fixed resolution, editor scene/game views and previews, mirrors,
  reflection probe faces, the SSR scene colour, post-process ping-pong, the Vulkan
  composite target) and the `PIP_RTT`, `PIP_RTT_INVERT`, `PIP_RTT_NODEPTH` and
  `PIP_ZPREPASS_RTT` pipelines expect it; swapchain pipelines take the environment default
  (`DEPTH_STENCIL`). Depth only, as before: shadow atlases (`createFramebuffer(...,
  shadowMap=true)`, `createDepthOnlyFramebuffer`), the SSAO depth pre-pass target and the
  G-buffer (`Framebuffer::setDepthStencil(false)`), drawn by `PIP_DEPTH`,
  `PIP_SHADOW_DEPTH` and `PIP_GBUFFER`, whose depth format stays `DEPTH`. The fullscreen
  effect targets (SSAO, SSR, blur) keep a depth attachment of the `PIP_RTT` format they are
  drawn with. `FramebufferRender::createFramebuffer(..., shadowMap)` (Lua-bound) means
  depth-stencil unless `shadowMap`; an overload takes the choice explicitly.
- Sampling a depth-stencil depth: no engine pass samples the depth texture of a colour
  target (SSAO/SSR/post read the packed depth colour of the depth pre-pass or the G-buffer;
  shadows sample the depth-only atlases), so the choice never changes what a shader reads.
  Every depth attachment still gets a texture view; sokol creates it with the depth aspect
  only (GL/GLES3 `DEPTH24_STENCIL8` samples depth by default, D3D11 SRV
  `R24_UNORM_X8_TYPELESS` over `R24G8_TYPELESS`, Metal samples `Depth32Float_Stencil8`,
  Vulkan view aspect `DEPTH_BIT`). Precision on GL/D3D11 offscreen targets moved from
  32-bit float to 24-bit depth, what the swapchain always had; renders compared pixel for
  pixel (see below).
- Clears: `SokolCamera` passes clear depth (1) and stencil (0) together; sokol's default
  was already clear, so nothing changed for existing passes. Stencil never survives a pass:
  a mask and its readers must draw in the same camera pass.
- `RenderState` gains `stencilEnabled`, `stencilCompare` (`StencilCompare`), `stencilRef`,
  `stencilReadMask`, `stencilWriteMask`, `stencilPass/Fail/DepthFail` (`StencilOp`) and
  `colorWriteMask` (`ColorWriteBits`, the bits of `MaterialColorWriteBits` and of sokol's
  `sg_color_mask`). `SokolObject::applyStencilAndColorMask` sets them on the colour
  pipelines (default, no-depth, RTT, RTT invert, RTT no-depth and the editor's no-cull
  variants, which are rebuilt from `makePipelineDesc`); the swapchain mask is
  `colorWrite & RGB` (alpha was never written to the swapchain), offscreen the full mask;
  0 becomes `SG_COLORMASK_NONE`. Depth, z-prepass, shadow and G-buffer pipelines ignore both.
- Resolution: `resolveRenderState` (RenderSystem.cpp) keeps its old signature and gained an
  overload taking `const MaterialStencil&` and `int colorWrite`; the old one passes AUTO,
  so manifest stencil still applies. `resolveSubmeshRenderState` (unchanged signature)
  passes the material's values; `resolveComponentRenderState` gained an overload with the
  component's (`ui/points/lines.stencil`, `colorWrite`), the old one passes AUTO (the
  editor's labels, sky). The extra pass (sl/p2-pass) should call the new overload with
  `MaterialPass::stencil` / `colorWrite` against its own manifest state.
  `MaterialStencil::compare` AUTO takes the manifest's stencil rule; DISABLED turns it off;
  anything else takes the whole struct (ref and masks clamped to 0..255). Stencil is
  enabled for any compare other than AUTO/DISABLED (ALWAYS included).
- Manifest. `ShaderManifestState` gained `stencil` (a `MaterialStencil`, compare AUTO when
  the manifest names none, DISABLED for `stencil=off`), `stencilSet` (`ShaderStencilFields`:
  which keys a `@state` line set; a `@state[...]` rule replaces only those over the base)
  and `colorWrite` (-1 unset), plus `hasStencil()`. `ref`/`readmask`/`writemask` take 0..255
  decimal or `0x` hex; ops are keep, zero, replace, incr, decr, invert, incrwrap, decrwrap;
  `colorwrite=` is `none` or any subset of `rgba` in any order without repeats. Errors carry
  file:line; stencil values on a line without `stencil=` on it or in the base `@state`
  before it are an error. Accessors for the editor/MCP (`list_shaders` should report
  `state.stencil` and `state.colorWrite` with them): `ShaderManifest::getStencilCompareName`
  / `parseStencilCompare`, `getStencilOpName` / `parseStencilOp`, `getColorWriteName` /
  `parseColorWrite`.
- Depth pre-pass: a submesh whose stencil test is not ALWAYS leaves the camera depth
  pre-pass (`MeshComponent::skipsDepthPrepass`), which would otherwise write its depth where
  the stencil hides it and cut holes in what is behind. A writer that always passes stays in
  it and writes stencil in its colour pass, where its depth equals the pre-pass depth. The
  SSAO depth pre-pass and the G-buffer ignore stencil: SSAO darkens and SSR reflects the
  whole shape of a masked object (visible in the fixture with `STENCIL_EFFECTS=1`).
- Sorting: `MeshComponent::opaqueSortPriority` (set at load, the lowest `sortPriority` of
  the opaque submeshes) orders the opaque queue before the distance, nearest first, with a
  stable sort (ties keep the transform order); with every priority 0 it is the old order.
  Transparent ties (same priority and distance) break by submission order (transform
  order), so the queue is deterministic. A 2D pass (no distance sort: a canvas camera, or
  `transparentSort` off) still draws in transform order and ignores `sortPriority`, as
  before. Priority is per mesh: one mesh's submeshes draw in submesh order.
- Reloads: `Mesh::setMaterial` and `Catalog::getMaterialUpdateFlags` reload the mesh on a
  stencil or colour write change; the component setters (`Image`, `Text`, `Polygon`,
  `Points`, `LineRenderer`: `setStencil`/`getStencil`, `setColorWrite`/`getColorWrite`) do
  too. Serialization, inspector, Catalog paths and MCP are not done (editor wave).
- Lua: `MaterialStencil()` (fields compare, ref, readMask, writeMask, pass, fail,
  depthFail), `material.stencil` (the material's own: `m.stencil.ref = 2` edits it),
  `material.colorWrite`, the same two properties on Image/Text/Polygon/Points/LineRenderer
  (`stencil` is a copy there: set it back), enums `MaterialStencilCompare`,
  `MaterialStencilOp` and `MaterialColorWrite` (AUTO -1, NONE, R, G, B, A, RGB, ALL;
  `MaterialColorWriteBits` gained `MATERIAL_COLOR_WRITE_NONE`, `_RGB` and `_AUTO` for it).
  `extraPass` is not bound here.
- Fixture `tools/fixtures/stencil` (Lua-built, orthographic): inside mask (with a masked
  LineRenderer), outside mask, draw-once on one mesh of overlapping squares beside the same
  mesh blended normally, a sprite masked by a character sprite whose fork
  `shaders/sprite_mask` takes `stencil=always ref=4 pass=replace colorwrite=none` from its
  manifest (alpha cutoff 0.5), a mask behind its reader that `sortPriority -1` draws first;
  the same row drawn by a render-to-texture camera below. `STENCIL_PREPASS=1` (renders
  identically), `STENCIL_EFFECTS=1` (SSAO + SSR), `STENCIL_SCENE=effects` (no stencil: a
  glossy floor, a chrome sphere, boxes, SSAO and SSR; pixel-identical to the build before
  this work).
