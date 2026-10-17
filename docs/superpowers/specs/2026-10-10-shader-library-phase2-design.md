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

Run for the final pass (5.9), beyond the above: the export-equivalence fixture
(`tools/fixtures/shader-library-export/check.sh`, editor render against the exported game for
every scene including the presets' `Stencil.scene`), the stencil and extra-pass fixtures, the
shader-regressions fixture, the MCP suite, a byte-identical re-save of the fixture projects
against the phase 1 tag's editor (differences only in phase 2 data), a pixel-identical 6-light
scene against the phase 1 tag and 0c59f0a0, and the game-build size of `Material`, `Submesh`
and `MeshComponent`.

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

### 5.2 Extra pass (sl/p2-pass)

- Data: `Submesh::extraPass` is a `std::shared_ptr<SubmeshExtraPass>` (render, shader,
  slots, custom blocks, library bindings, order), null for a submesh whose material has no
  `extraPass.shader`: such a submesh allocates nothing (no object, pipeline, uniform storage
  or texture binding). A component copy shares the pointer until its own load, which makes
  a new object (like the value `ObjectRender`s, whose handles a copy shares).
- Load: `RenderSystem::loadSubmeshExtraPass`, at the end of each submesh of `loadMesh`
  (after the depth and G-buffer renders). It binds the same vertex attributes as the colour
  render (`bindSubmeshAttributes`: buffers, the submesh's own attributes, the instance
  buffer), skinning, morph targets, the sprite rect, the frame block, its own custom blocks
  (`MaterialPass::shaderParams`, else its manifest defaults), its own `@texture`s (unset:
  manifest defaults) and the material's PBR textures (a fork of Lit without a manifest
  samples them). While its shader builds the mesh does not load (as for its other shaders).
  A pass whose shader fails to build is logged and skipped (`SubmeshExtraPass::failed`): no
  built-in fallback, which would draw the submesh a second time. A Terrain has no extra pass
  (`RenderSystem::hasExtraPass(submesh, terrain)`).
- Variant: `getExtraPassShaderInputs(submeshInputs, pass)` takes the submesh's
  `SubmeshShaderInputs` (attributes, skinning, morph, instancing and fade, texture rect,
  vertex colour, alpha mode, fog from the scene) with the pass's manifest and features, its
  shader's default lighting mode (`MaterialPass` has no lighting field), and no received
  shadows, IBL or mirror; `getSubmeshShaderVariant` turns them into bits as for any submesh.
  The reference is `getSubmeshExtraPassReference` (features + `BLEND_*` of the pass's
  blend). `getShaderVariants`, `ShaderVariantCollector`, `Project::collectSceneShaderKeys`
  and `check --shaders` use the same functions, so exports precompile the pass variants
  (only the custom key: no built-in fallback). These bits are a subset of what the main draw
  can produce (no shadows/IBL/mirror), so the `shaders --library` matrix needs no extra-pass
  cases.
- State: `resolveExtraPassRenderState` calls the same `resolveRenderState` as the submesh
  with the pass's values, its manifest state (`@state` + `@state[...]` of its features) and
  the submesh's legacy behaviour (material alpha blend, face culling, cull mode, winding).
  It passes the pass's `stencil` and `colorWrite` (the stencil overload), so the pass takes
  them, else its own manifest's stencil and colour write. `Mesh::setMaterial`
  reloads on any pass change but its parameters (rewritten into the blocks) and texture
  files (rebound).
- Draw: inside `drawMesh`'s submesh loop, right before (`BEFORE`) or after (`AFTER`) the
  submesh's draw, so it keeps the submesh's queue position in every colour pass (opaque,
  transparent, hierarchy-order cameras, RTT, mirrors, probes, editor views), with the same
  instances (view slices and LOD buckets), detail level and tilemap chunks. It is in no
  shadow, depth pre-pass or G-buffer pass. A lit pass (its manifest's default lighting is
  lit) gets the object's light list, Lit's camera block and the legacy lighting block when
  declared, and the 2D lights; shadows are never received (an overlay pass).
- Release: `destroyMesh` destroys the pass render always and its shader and textures with the
  other assets; a reload whose material has no pass anymore frees the object; a variant
  change returns the old shader to the pool.

### 5.3 Smoothed normals (sl/p2-pass)

- `@require smoothnormals` (`ShaderRequire_SmoothNormals`, define `SURFACE_SMOOTH_NORMALS`).
  New `AttributeType::SMOOTHNORMAL` (appended), attribute `a_smoothNormal`, 4 signed
  normalized bytes per vertex (xyz, w 0) in its own vertex buffer.
- `surface_vs.glsl` / `surface_depth_vs.glsl`: the hooks read the global `vec3
  surfaceSmoothNormal` (object space, after skinning and the instance matrix, normalized;
  morph targets leave it at the base pose). The attribute is declared only with a
  displacement hook (`SURFACE_DISPLACE` or `SURFACE_DISPLACE_NORMAL`; depth programs:
  `SURFACE_DISPLACE`), since only a hook can read it and a GL driver drops an unread
  attribute; without `SURFACE_SMOOTH_NORMALS`, or on a Terrain, `surfaceSmoothNormal` is the
  hook's `objectNormal`.
- Made by the renderer when a loaded shader (main, depth or pass) declares `a_smoothNormal`
  (`bindSmoothNormals`): per vertex of the submesh's position attribute, the angle-weighted
  sum over the triangles at each vertex of its own normal (the face normal when the mesh has
  none), summed over the vertices welded by position (cells of 1e-5 of the mesh's largest
  extent), normalized; opposite faces that cancel keep the vertex's normal. Cached in
  `MeshComponent::smoothNormals`, keyed by a hash of the positions, normals and indices, so
  reloads reuse it and a geometry change makes a new one; entries no submesh uses after a
  load are freed, and all of it when no shader asks.
- Diagnostics: with `JASPERENGINE_READY_SIGNAL` the engine also logs "Render objects: N
  meshes, N submeshes, N extra passes drawn (N allocated), N smoothed-normal buffers, N object
  pipelines" per scene (`RenderSystem::logRenderObjects`, `ObjectRender::getLivePipelineCount`).
- Lua: `Material.extraPass` (a `MaterialPass`, edited in place on the material copy, then
  `setMaterial`) with every field, `setShaderParam`/`getShaderParam`/`removeShaderParam`,
  `setShaderTexture`/`getShaderTexture`, `setShaderFeature`/`isShaderFeatureEnabled`
  (resolved against the pass's own shader); `MaterialPassOrder`; `MaterialStencil`,
  `MaterialStencilCompare` and `MaterialStencilOp` for `MaterialPass.stencil` (the stencil
  work may already bind them for `Material.stencil`: keep one registration).
- Not done here (editor waves): serialization, Factory codegen, model-reload override flags
  for `extraPass`, inspector, Catalog paths, MCP.
- Fixture: `tools/fixtures/extra-pass` (fixture-only forks `assets/shaders/outline`,
  `assets/shaders/lit_outline`, `assets/shaders/silhouette`; `EP_FOCUS=<case>` close-ups;
  `EP_SCENE=overhead|overhead_pass`: 40 cubes hold 123 object pipelines without passes and
  133 with ten outlined, one pipeline per pass and nothing for the other submeshes). Its
  stencil case (QuickOutline style: material stencil always/replace 1, an AFTER hull with
  `pass.stencil` not-equal 1) checks the pass's own stencil state after the merge.
- An inverted hull after a blended object (which writes no depth) covers it: on transparent
  objects the outline goes `BEFORE` (the fixture's glass box). Presets should pick it.
- The extra pass's shader is requested before the submesh builds any pipeline, so a load
  waiting for it leaves no pipelines behind; the pass render is destroyed before a retry.
  (Pre-existing, not changed: a load that fails after a submesh's `endLoad`, e.g. waiting
  for a later submesh, abandons that submesh's pipelines.)
- Pre-existing, worked around in the fixture: a Lit Sprite with `alphaMode` AUTO decides
  transparency from its image data (`Texture::isTransparent`, false while the data is not
  loaded or once released), so it can load opaque and write depth over its transparent
  texels; the fixture sets `MaterialAlphaMode.BLEND` on its sprites and tilemap.

### 5.4 Editor (sl/p2-editor)

- Serialization (`Stream`): a material (scene YAML submeshes, prefabs, `.material` files), an
  extra pass and the UI/Points/LineRenderer components write `stencil` only when it differs
  from `MaterialStencil()` (a map: `compare` always, the other fields away from their
  defaults; enum names upper case like the other material enums: `NOT_EQUAL`, `INCR_CLAMP`),
  `colorWrite` only when not AUTO (the manifest's names: `rgb`, `none`), and `extraPass` only
  when it differs from `MaterialPass()` (its keys as the material's, plus `order: BEFORE`).
  Absent keys read as the defaults (not merged). Re-saving every fixture project of
  ac833620 (`scene-dump --save`, 16 scene/prefab/material files) with this editor and with
  ac833620's gives byte-identical files.
- Model reloads: `SubmeshOverrideFlags` gained `Stencil` (27), `ColorWrite` (28) and
  `ExtraPass` (29, the whole pass under one bit), part of `SubmeshOverride_ShaderLibrary`;
  `MeshSystem::applySubmeshOverride` restores them. A Catalog path inside the stencil
  (`material.stencil.<field>`) or the pass (`material.extraPass.<anything>`) sets its bit.
- Catalog paths: `submeshes[N].material.stencil` (Custom, `MaterialStencil`),
  `.stencil.compare|ref|readMask|writeMask|pass|fail|depthFail` (Enum/Int), `.colorWrite`
  (Int), `.extraPass` (Custom, `MaterialPass`) and `.extraPass.<field>` for every field
  (including `.extraPass.stencil.*`); the components' `stencil`, `stencil.<field>` and
  `colorWrite`. `.extraPass.shaderParams.<member>` / `.shaderTextures.<uniform>` are entry
  paths (`splitShaderEntryProperty`); `getShaderReferenceForProperty` gives the pass's own
  shader for any `...material.extraPass.` path (no fallback to the mesh's or the scene's
  shader). Update flags follow `Mesh::setMaterial`: the pass's parameters rewrite its
  blocks, its texture files rebind, anything else reloads the mesh;
  `getMaterialUpdateFlags` (linked material files) does the same.
- Export (`Factory`): `formatStencilAndColorWrite` / `formatMaterialPass` write the fields
  away from their defaults (`.stencil.ref = 16;`, `.colorWrite = 7;`,
  `.extraPass.shaderTextures.push_back(...)`); the pass's variants come from the existing
  collector. Asset moves, model clears, the exported texture list and the material preview's
  readiness include the pass's textures. An export with `--backend all` of a scene set up
  only through MCP (stencil mask preset with an explicit reader, an outside-mask preset,
  colour write `r`, an outline preset tuned through `extraPass.shaderParams`, draw-once)
  compiles 42 shaders on six backends without failure and its smoke run draws them as
  configured.
- Inspector: a Stencil row at the end of a material's Surface rows (Auto names the rule it
  resolves to, e.g. "Auto (Not Equal 1, pass Replace)"; ref, masks and ops only while the
  test is explicit; picking a test from Auto starts from the resolved rule), a Color Write
  row (Auto checkbox, R/G/B/A toggles), an "Extra pass" section (shader picker whose empty
  entry is "None", the pass shader's manifest rows, order, blend, cull, depth write/test,
  depth offset, its own stencil and colour write; Auto labels resolved against the pass
  shader's manifest and the submesh's legacy state), a Preset row (menu of
  `getMaterialPresets()`; one applies to every selected submesh as one undo step), and the
  same stencil/colour write rows on UI, Points and LineRenderer. A preset becomes one
  property command per changed field (`editor/material/MaterialChange`), so model overrides
  and prefab overrides name only those fields. Material previews and thumbnails draw the
  pass (the sphere takes the whole material) and wait for its textures.
- MCP: set_properties takes every path above. Stencil names are snake_case like the depth
  test's (`not_equal`, `incr_clamp`; the manifest's `lequal`, `incr`... accepted); ref and
  masks 0..255; `colorWrite` takes `"auto"`, `"none"`, channels of `"rgba"` or -1..15 and
  reads back as the channel name; a `stencil` or `extraPass` object merges into the current
  value and `null` resets / removes it. The pass's values are checked against its own
  shader with phase 1's validators (unknown library reference refuses value edits with the
  library list, leftovers kept and only edited entries checked, a pass shader set in the
  same call is what its values follow); its shader must be a mesh shader. `list_shaders`
  reports the stencil keys a `@state` sets and `colorWrite` in `state` / `state_rules`.
  New tools `list_material_presets` (id, name, description, the fields each sets on a
  default material) and `apply_material_preset` (`preset`, `entity`/`entities`, `submesh`:
  default every submesh; or `path` for a `.material` file, undo scope project). A preset
  applied to a submesh linked to a `.material` file unlinks it in the same undo step (as an
  inline material edit does) and says so. Tests: `test_shader_library_phase2_editor.py`.
- `tools/fixtures/shader-library-export/Stencil.scene` was generated with this branch
  (`generate.py`); `check.sh` passes for all five scenes (Stencil: mean difference 0.57).

### 5.5 Shaders and presets (sl/p2-shaders)

- Library shaders (category "Outline and stencil", sprite_mask in "2D"); every one says
  `shadows=off` and has no `@depth` (they are extra passes or masks: in no shadow pass, and
  out of the depth pre-pass, which skips draws without depth write).
  - `outline` / `outline_stencil` share `library/outline_vs.glsl` (vertex) and
    `library/outline.glsl` (fragment): `@require smoothnormals`, the displacement hook pushes
    the vertex out along `surfaceSmoothNormal`. `WIDTH_MODE_SCREEN` (`screenWidth`, pixels):
    the screen direction of the normal is the derivative of `clip.xy / clip.w` along the world
    normal (`frame.viewProjectionMatrix`), scaled to pixels by `frame.resolution` (aspect and
    perspective correct); the offset of `screenWidth` pixels is applied in the view plane
    (`clipOffset / (P00, P11)`, depth unchanged) and turned back into object space with the
    view rotation's transpose and the normal matrix's transpose (the inverse model matrix),
    because the hook returns an object-space position. It holds for orthographic cameras and
    for flipped RTT projections (the signs cancel). Measured: 4 px at every distance in
    perspective and orthographic views. `WIDTH_MODE_WORLD` (`worldWidth`): along the world
    normal, so a scaled object keeps an even width; `DISTANCE_SCALE` multiplies it by
    `mix(1, max(viewDepth / referenceDistance, 1), distanceScale)`, at most `maxWidth`
    (orthographic: no growth). `TEXTURE` tints by `u_outlineTexture` at uv1 (tiling),
    `VERTEX_COLOR_WIDTH` scales the width by the raw vertex colour's red. Never the vertex or
    instance colour (`SURFACE_NO_VERTEX_COLOR`). The frame block is declared in the vertex
    stage only when read (screen width or distance scale).
  - `outline`: `blend=opaque cull=front zwrite=on`. Order After on opaque objects (the
    object's depth rejects the hidden hull early), Before on blended ones (5.2). Sprites use
    the sprite shader's `OUTLINE` feature (a flat quad has no hull).
  - `outline_stencil`: `@state stencil=notequal ref=1 readmask=1 writemask=0`, `cull=off`
    (open meshes outline from behind), `@keywords VISIBILITY ALWAYS VISIBLE_ONLY`: ALWAYS
    `ztest=always zwrite=off`, `@state[VISIBILITY_VISIBLE_ONLY] ztest=lequal zwrite=on`. The
    main material marks its whole silhouette, hidden parts included, with stencil always /
    ref 1 / write mask 1 / pass replace / zfail replace (depth-fail replace is what keeps a
    through-walls outline an outline instead of filling the hidden shape). The pass keeps
    the submesh's queue position, so a through-walls outline needs its occluders drawn
    before the object: the preset raises the material's `sortPriority` to at least 1 (the
    opaque queue sorts by priority before distance).
  - `silhouette`: `blend=alpha ztest=greater zwrite=off cull=back`, stencil not-equal 2 /
    replace on bit 0x02 (blends once per pixel, across objects too, until the pass ends),
    `@keywords SILHOUETTE_MODE FLAT FRESNEL` (FRESNEL: opacity `mix(fill, 1, pow(1 - |n.v|,
    power))`, `@require normals position feature=SILHOUETTE_MODE_FRESNEL`, the frame block in
    the fragment stage). Drawn Before the main draw, so the object's own depth is not in the
    buffer and only earlier draws (its occluders: priority 1 again) make it pass; with the
    depth pre-pass on, self-occluded concave parts show too.
  - `stencil_mask`: `blend=opaque zwrite=off colorwrite=none`, `stencil=always ref=16
    writemask=0xf0 pass=replace`, depth test on (a hidden mask masks nothing). `ALPHA_CLIP`
    discards below `alphaCutoff` of `u_maskTexture` (tiling). Its `color` reaches the screen
    only if colour writes are turned on (debugging a mask's placement).
  - `sprite_mask`: the same state with `cull=off`, the shape from the material's base colour
    texture at `v_uv1` (`includes/base_color_fs.glsl`; the shared vertex path already applies
    the sprite and instance texture rects, so atlas frames mask with their current frame),
    `alphaCutoff`. Opaque queue, so in a distance-sorted camera it draws before the blended
    sprites it masks; a 2D camera draws in hierarchy order (place the mask first).
- Presets: `editor/material/MaterialPresets.{h,cpp}` (`jasperengine::editor`,
  `getMaterialPresets`, `findMaterialPreset`, plus `isMaterialBlended`). Stencil families:
  0x01 outline_stencil, 0x02 silhouette, 0x0C draw-once (ref 4; 8, 12 for further objects
  that overlap on screen), 0xF0 masks (ref 16; 32 ... 240 for further masks). A material has
  one stencil state, so the material-stencil presets replace each other; the extra-pass
  presets combine with them but not with each other.
  - `outline_hull`: extra pass `library:outline`, After unless `isMaterialBlended` (explicit
    blend, else the manifest's resolved `@state` blend with the material's features, else
    the legacy rule), then Before. Inspecting the blend rather than always using Before:
    Before is correct for both, but on an opaque object it shades the whole hull and then
    overdraws it, while After lets the object's depth reject the hidden hull (mobile
    overdraw). The order is fixed when applied: re-apply after changing the blend.
  - `outline_stencil`: material stencil as above, pass `library:outline_stencil` After,
    `sortPriority >= 1`. `see_through_silhouette`: pass `library:silhouette` Before,
    `sortPriority >= 1`. `draw_once_transparency`: stencil not-equal 4, read/write mask 0x0C,
    pass replace; an opaque material becomes alpha blended. `stencil_mask` / `sprite_mask`:
    shader `library:stencil_mask` / `library:sprite_mask`, the old shader's features and
    lighting cleared, blend/depth write/colour write/stencil back to Auto (the manifest's
    state), `sortPriority <= -1`. `visible_inside_mask` / `visible_outside_mask`: stencil
    equal / not-equal 16 on 0xF0, keeping a mask ref the material already tests. A pass
    already using the preset's shader keeps its parameters (re-applying only fixes the
    order); any other pass is replaced whole. Outline presets keep the main shader.
- Fixture: group `phase2` of `tools/fixtures/shader-library` (`scripts/phase2.lua`, which
  reproduces the presets field for field in Lua), with `PH2_VIEW` close-ups and
  `PH2_ORTHO=1`. Models `skinned_column.gltf` and `overlap_quads.gltf` copied from the
  extra-pass and stencil fixtures. The extra-pass and stencil fixtures keep their fork
  shaders (they test the engine features, not the library).
- `docs/shaders/library.md`: the generated "Default state" line also names `colorwrite` and
  the stencil keys a `@state` line sets (`editor/shader/ShaderLibraryDoc.cpp`).
- Export equivalence: `tools/fixtures/shader-library-export/generate.py` gained
  `Stencil.scene` (every preset through the MCP tool `apply_material_preset`, pass values
  through `submeshes[0].material.extraPass.*`); it needs the editor wave's serialization,
  Catalog paths and MCP tool, so it was not generated on this branch: run `generate.py`
  after the merge, then `check.sh` (which skips the scene, with a message, until it exists).

### 5.6 Engine fixes (sl/p2-enginefix)

- Failed mesh loads. `loadMesh` now asks for every submesh's main, depth, G-buffer and
  extra-pass shaders in a first loop (all at once, so they build together) and waits there,
  before any pipeline exists. A failure after that (a texture still loading, a full pool) runs
  `LoadAttempt`'s release: the renders and extra passes that try began are destroyed.
  `SokolObject::beginLoad` forgets all ten pipeline handles (the no-depth and pre-pass ones
  survived it), so a release never touches handles a component copy shares, and the live
  count counts only valid handles. Lua: `ObjectRender.getLivePipelineCount()`. The note of
  5.2 about abandoned pipelines no longer applies.
- AUTO alpha mode. The cause was not timing: a mesh never loads before its base texture's
  GPU texture exists. A `Texture` that found that texture already in `TexturePool` (a second
  sprite, tilemap or mesh of the same image) had no image data, so `isTransparent()` said
  false. `TextureRender` keeps the image's transparency (`setTransparent`, set by
  `TexturePool::get` when it makes it) and `Texture::isTransparent` falls back to it. The
  per-frame queue comparison in `update` already reloads a mesh once when it changes. Points
  decide their transparency after their texture is requested. Library mesh shaders all state
  `blend=` in their manifests, so AUTO matters to them only through a fork or a shader without
  it; their extra passes resolve with the submesh's legacy blend, which this fixes too. The
  extra-pass fixture no longer forces `MaterialAlphaMode.BLEND`.
- Fork paths. A fork path is project-relative (canonical; what forking writes). One the
  project root does not have is looked up in the assets directory (`shaders/x` for
  `assets/shaders/x` with `assetsDir: assets`): `editor::Project::findShaderFile`, the only
  place of the rule, used by the shader compiler's reader (editor, export precompile,
  `check --shaders`, `check_shader`), the manifest reader, the checks' file matching and
  error paths, MCP existence checks, the inspector's file rows and the rename/delete remap.
  The game already resolved both (its asset root holds the assets directory and every shader
  source at its project-relative path, the latter winning, as the project root wins in the
  editor). Reference strings are not rewritten, so the `.sdat` names the export writes are the
  ones the game looks up, Lua-set ones included. Not changed: a "run" build
  (`Generator.cpp`, `JASPERENGINE_ASSET_PATH` = the assets directory) reads forks from the
  assets directory only, so project-relative forks outside it (e.g. the fork default
  `<project>/shaders`) and the `assets/...` spelling miss their manifests there (read from
  the code, not run).
- Several skyboxes. Nothing else was wrong: every consumer (draw, IBL, reflection probes,
  SSR, loaded count) takes component index 0, the first added (removal keeps the order); the
  others are never loaded and take over when the first is removed. Unity-like (one scene
  skybox); now documented on `SkyboxComponent`, with a warning logged when the sky loads in a
  scene with more than one. Remaining editor quirk (not fixed): reordering skyboxes in the
  Hierarchy changes the file order, hence the active sky, only after a reload or in an export.
- Fixture `tools/fixtures/shader-regressions` cases 6 (alpha), 7 (leak check; logs "[SR]
  leak check passed", `SMOKE_GAME_SECONDS="ready 70"`), 8 (second skybox) and 9 (fork paths
  in `shaderAdditions`).

### 5.7 Review fixes: engine (sl/p2-fix-engine)

- Extra pass storage (E2). `Material::extraPass` is a `MaterialExtraPass`: the `MaterialPass`
  out of line behind a pointer (8 bytes), null while it holds `MaterialPass()`. A value: copies
  are deep (a copy of a default pass stays null), `==` compares by value, an unset pass reads as
  `MaterialPass()`. C++ reads through `->`, `*`, `get()` or the implicit `const MaterialPass&`
  conversion; writes through `edit()` (makes it), `editIfSet()` (null when unset) or by assigning
  a `MaterialPass` (keeps an existing allocation, so a Lua reference to it stays valid). Lua is
  unchanged (`m.extraPass` returns the copy's own pass, made on first access); the Catalog paths
  `...material.extraPass[.*]` point into `edit()`; exported scene code writes
  `material.extraPass.edit().<field> = ...`; `MaterialChange` takes the pass as a
  `MaterialPass`. Game-build sizes (sizeof, Linux GCC, before -> after this branch):
  `Material` 2088 -> 1928 bytes, `Submesh` 7112 -> 6976, `MeshComponent` 128256 -> 126080
  (phase 1: 1888 / 6752 / 122480; what remains of phase 2's growth is the stencil state in
  `Material`, `ShaderManifestState` and `RenderState`, and, from E4, one pipeline handle per
  `ObjectRender`, 8 bytes with padding). The phase 1 `shader`/`shaderParams`/`shaderTextures`/
  `shaderFeatures` (104 bytes) stay inline: about 130 call sites in the engine, editor, Lua and
  MCP for 1.6 KB per mesh, not taken in this pass. Scenes, prefabs and `.material` files
  serialize as before (Stream compares with `MaterialPass()` as it did).
- Queues (E1, E3). The camera pass sorts its opaque queue with `std::sort` on (sortPriority,
  distance, submission order) and its transparent queue on (sortPriority, farthest first,
  submission order), both total orders, so the result is the old stable sort's / priority
  queue's; both are `RenderSystem` members reused across passes (no allocation once grown).
  A reflection probe face draws its opaque meshes in the same order, nearest the probe first
  (it used transform order, so masks, their readers and through-walls outlines came out wrong
  in probes: shader-regressions case 10).
- Effect targets (E4). `FramebufferDepth` (`DEPTH_STENCIL`, `DEPTH`, `NONE`) and
  `Framebuffer::setDepthAttachment` (`setDepthStencil` and the bool overloads stay). The SSAO
  and SSR targets, their blurs and post-process buffer 1 are depth only (no stencil plane:
  D32 instead of D32S8 on Metal and Vulkan, the same 4 bytes as D24S8 on GL and D3D11), drawn
  by the new `PIP_EFFECT` pipeline type (PIP_RTT's colour state, depth never tested or
  written). Post-process buffer 0 keeps depth-stencil (the colour pass draws meshes into it); a
  chain pass picks `PIP_EFFECT` into buffer 1, `PIP_RTT` into buffer 0 or its destination.
  `NONE` (no attachment at all) works too but changed llvmpipe's SSAO/SSR output by 1 on 12
  pixels of the 6-light scene, where a depth plane the pipeline never tests renders pixel for
  pixel as the phase 1 tag; the effects keep it.
- Smoothed normals (E5, E8). One `a_smoothNormal` buffer per vertex buffer: the first submesh
  that asks welds over the triangles of every submesh drawing the same position (and normal)
  attribute and sets the entry on all of them. Only vertices some triangle uses take part
  (with no triangle at all, every vertex with its own normal, as before). Positions and normals
  are read in any component type (a quantized or 4-component attribute); a mesh whose positions
  cannot be read (none, or fewer than 2 components) gets its vertex normal with a warning
  instead of failing to load. Every position the engine can draw today is float (sokol has no
  3-component integer vertex format, so a quantized glTF position fails at pipeline creation
  anyway): the old error was not reachable by a mesh that would otherwise draw. Measured on the
  extra-pass figure scene: 6 smoothed-normal buffers before, 2 after (one per outlined
  figure of three submeshes).
- Fade (E6). The vertex hooks read `float surfaceFadeVisible` (1 shown, 0 faded; 1 without
  `USE_INSTANCE_FADE`), in `surface_vs.glsl` and `surface_depth_vs.glsl`; outline widths
  scale by it (GLSL pass).
- Extra-pass order (E7). `drawMesh` draws every Before pass of the mesh, then the submeshes,
  then every After pass, each in submesh order (5.2's "right before or after the submesh" holds
  for one submesh). A silhouette no longer shows a submesh through its own object's others,
  and a stencil outline tests the stencil its whole object marked. A render-target texture
  redrawn this frame is still rebound by both draws (a Before pass flags the submesh).
- Centroid (E10). `includes/surface_uv.glsl`: `SURFACE_UV_OUT` / `SURFACE_UV_IN` (`centroid`
  under `HAS_TEXTURERECT`); `surface_vs.glsl` and every library fragment stage reading `v_uv1` /
  `v_uv2` use them, so the two stages always agree (GLSL ES 3.00 links only matching
  qualifiers; a GLES export before this declared `centroid out` / `in` for unlit, stencil_mask
  and outline_stencil on a sprite, both stages `centroid` now). A fork written like the
  library ones uses the same include.
- Skyboxes (E11). After a Hierarchy move the editor sorts the SkyboxComponent array by its
  entity list (`EntityRegistry::sortComponentsByEntityOrder`, from `ProjectUtils`), the scene
  file's order; `RenderSystem` remembers the sky it loaded and, when another becomes first
  (a move, or a removal), unloads the old one and reloads the meshes' image-based lighting. The
  sky in use is the same live, after a reload and in an export (5.6's remaining quirk is gone).
- Fixtures and tests: `tools/fixtures/extra-pass` `EP_SCENE=figure` (`models/figure.gltf`,
  three submeshes over one vertex buffer, with the silhouette, stencil outline and hull presets
  on every submesh; `EP_FOCUS=silhouette|stencil|hull`), shader-regressions case 10 (probe
  order), MCP `test_engine_fixes_p2.py` (skybox order; the build before fails it). The 6-light
  scene (directional and spot with shadows, three points, a second spot, fog) renders pixel for
  pixel as the phase 1 tag with SSAO and SSR off, on, each alone, and with a three-pass
  post-process chain.

### 5.8 Review fixes: editor and presets (sl/p2-fix-editor)

Overrides the outline_hull and draw-once notes of 5.5, the last notes of 5.3 about blended
objects, and the "previews transparent" note of 5.4.

- Fork remaps. A fork renamed, moved (`move_asset`, the Project window) or deleted is
  remapped or cleared in submesh materials' `shader` and `extraPass.shader` too (scenes,
  prefabs and `.material` files; library references are never touched), with a mesh reload,
  beside the components' `customShader` / `customDepthShader` and the scene defaults
  (`Project::applyCustomShaderPathChange`, `visitMaterialFiles`). Closed scenes are not
  rewritten (as for every other remap).
- `set_properties` refuses, per entity and component, a whole value (a material, an extra
  pass, a `stencil` block, `shaderParams` / `shaderTextures` / `shaderFeatures` /
  `shaderUniforms`) together with a path inside it in one call: their commands were each
  built from the values before the call, so the later one undid the other, and an entry was
  checked against the old shader. The error gives the one-value form (the inner path nested
  in the object, which merges) or two calls.
- Inspector, several GameObjects selected: a stencil test, an Auto reset or a colour
  channel changes only that on each one's own value (one undo step). A test picked from
  Auto starts from the rule that GameObject's own shader resolves to; an explicit test, Off
  included, keeps its ref, masks and ops.
- Presets judge a material by the shader its submesh draws with: its own, else the mesh's
  `customShader`, else the scene's default mesh shader (`MaterialPreset::apply(material,
  meshShader)`, `meshShaderFor`); a `.material` file (no mesh) by its own shader.
  `isMaterialBlended(material, meshShader)`: the material's blend, else that shader's
  resolved `@state` blend, else the legacy rule.
- Outline (hull) on a blended material is the stencil outline, not an inverted hull: the
  hull's back faces fill the whole shape, which a blended object (no depth write) shows
  through Before and is covered by After. The material marks its silhouette with bit 0x01
  and an After `library:outline_stencil` pass with `VISIBILITY_VISIBLE_ONLY` draws outside
  it (depth tested, so no sort priority change). A new pass starts from the hull's defaults
  (colour and width), a hull pass it replaces keeps its values and features, and re-applying
  on an opaque material turns it back into a hull with them. Opaque materials keep the hull
  After. The mark: alone, the Outline (stencil) state (always, ref 1, write mask 1, replace,
  depth-fail replace); over draw-once, not equal ref 4|1 on read mask 0x0C, write mask 0x0D,
  replace; over a mask reader, its compare with ref 16|1 on read mask 0xF0, write mask 0x01,
  replace (no depth-fail replace when combined: the write mask would write the test's bits
  where the object is hidden; a visible-only outline needs no hidden marks). So draw-once
  and an outline go together, and Draw-once turns a hull outline into this one.
- The through-walls Outline (stencil) keeps its own stencil (its depth-fail mark cannot
  share a test): a test preset (draw-once, mask readers) still removes its pass, it replaces
  the material's test, and applying it sets Visibility back to Always. Replacing an
  outline_stencil pass with another pass preset drops the mark and keeps the test it was
  combined with. A mask preset clears the pass (as before).
- A mask reader masks the extra pass (`maskExtraPass`, after every preset): a pass without
  a stencil test of its own (the hull, most passes) takes the reader's test (write mask 0);
  a silhouette inside a mask tests equal ref on 0xF2 and inverts bit 0x02 (still blends
  once), outside it takes the mask test alone (overlapping hidden parts then blend more than
  once); a visible-only outline inside a mask tests equal ref on 0xF1 (inside the mask,
  outside the mark); outside a mask it keeps its own test, since "mask differs and bit 0x01
  clear" is no single stencil test: its ring is not clipped where it crosses the mask. A
  pass's own explicit test is never replaced; a mask test a preset gave a pass goes back to
  Auto once the material reads no mask.
- MCP `apply_material_preset`, `link_material` and `create_material` require `submesh` to be
  a whole number from 0 (a fix-it error instead of a raw JSON type error; -1 no longer reads
  as 0); any handler's JSON type error reads as a wrong argument type.
- Material previews and thumbnails draw alpha wherever a material or its extra pass writes
  colour without alpha (explicit or the shader's `colorwrite=`), so they show opaque, as the
  game does on its opaque target; a mask writing no colour stays invisible.
- Docs: Sort Priority orders within the opaque queue too (inspector help, MCP error, guides);
  `lua-api.md` lists `MaterialStencilCompare`, `MaterialStencilOp`, `MaterialColorWrite` and
  `ObjectRender.getLivePipelineCount()`. The extra-pass fixture's case 7 (a lit hull before
  a glass box) shows the hull's back faces through the glass, as documented there.
- Tests: `test_shader_library_phase2_review.py` (MCP) and
  `test_shader_library_phase2_inspector.py` (the inspector driven with xdotool in the
  harness's Xvfb; it skips when the default layout moved). Re-saving the 15 scene and prefab
  files of every fixture project with this editor and with 962af759's gives byte-identical
  files.

### 5.9 Final pass (sl/p2-final)

The merge of 5.7 and 5.8 (c2f29ea6), built and checked as a whole; overrides the notes above
where they disagree.

- Merge fixes. `MaterialRender` (previews) still bound the extra pass by non-const reference
  (the merge did not compile); it edits through `editIfSet()`. The preset helpers read the pass
  through the const accessor only (`extraPassOf(const Material&)`) and write through
  `editExtraPass` / `editIfSet()`, so a preset that only looks at a material's pass (every
  preset runs `maskExtraPass`) allocates none: untouched materials stay unallocated, compare and
  serialize as before. (Reads through the Catalog paths `...material.extraPass.*` still call
  `edit()`, as 5.7 says; a pass made that way holds `MaterialPass()`, which compares and
  serializes as no pass and is dropped by the next copy.)
- `sl/p2-verify` (962af759), not recorded above: an extra pass is no longer kept alive by model
  reloads or shared by clones; stencil masks (no colour, no depth write:
  `Submesh::skipsScreenEffects`) stay out of the SSAO depth pre-pass and the G-buffer;
  smoothed-normal entries stable on async loads; library normal matrix made per draw in scenes
  without lights; outline `VERTEX_COLOR_WIDTH` requires `rawcolor`; silhouette fresnel clamped
  (no NaN); preset combinations that broke an outline; extra-pass forks reload on edit; and run
  builds read forks from the project root, then the assets directory (5.6's "Not changed" about
  run builds no longer applies).
- Outline (G1, G3, G5). `outline_vs.glsl` scales the push by `surfaceFadeVisible` (a faded
  instance takes its outline with it). Hard edges are a documented limit (`outline_vs.glsl`,
  both manifests, `docs/shaders/library.md`): the push follows the corner's smoothed normal, so
  the band beside a sharp corner is narrower than the width: SCREEN width/sqrt2 along a box face
  seen square on, WORLD faces offset by width/sqrt3 on a box. A crack-free correction needs a
  miter length per welded vertex (per face vertex reopens the gaps the smoothed normal closes),
  so none is applied. `outline.frag`'s comment block describes the stencil outline for blended
  objects.
- `sprite_mask` (G2) declares only the base colour texture: `includes/base_color_fs.glsl` takes
  `BASE_COLOR_TEXTURE_ONLY` (no `u_fs_pbrParams`, no `applyMaterialAlphaMode`).
- Fixtures (G4, G6, G7). Stencil fixture case F: `library:sprite_mask` in front of its reader
  with no sort priority, drawn first because the mask is in the opaque queue (both rows; the six
  cases are 3.6 apart). `phase2.lua` mirrors the blended Outline (hull) preset (mark bit 1, an
  After `outline_stencil` pass, Visible only, from the hull's look) and checks it.
  `shader-library-export/Stencil.scene` regenerated with `generate.py` (the other scenes
  regenerate identical but for their random ids and a sprite's runtime `transparent` flag, so
  they were kept).
- Docs. The Lua and MCP guides say extra passes draw around all of a mesh's submeshes (5.7);
  the Stencil row's tooltip and the MCP shader guide state that a test other than Always leaves
  the depth pre-pass and that SSAO and SSR ignore the stencil.
- Open items, final status. Fixed: hull outline on blended objects (5.8), mask readers and the
  extra pass (5.8, with the documented limits there: a visible-only outline outside a mask is
  not clipped; a silhouette outside a mask may blend more than once where hidden parts overlap),
  probes and sortPriority (5.7), smoothed normals of non-float positions (5.7), fork renames
  (5.8), centroid (5.7), previews without alpha writes (F1, 5.8), live skybox order (F3, 5.7),
  run-build fork paths (F2, p2-verify). Design limits, documented in this spec, the inspector
  and the guides: stencil readers leave the depth pre-pass and SSAO/SSR ignore the stencil (F4,
  5.1); silhouette with the depth pre-pass shows self-occluded parts (F5, silhouette manifest);
  through-walls outline and silhouette rely on the presets' sort priority (F6); outline width
  beside hard corners (above).
- Checks run on this branch: full `tools/build.sh linux`; `shaders --library all --backend all`
  (29 shaders, 8700 compiles, no failure or manifest diagnostic) and 56 built-in variants on six
  backends (Lit tiers and modes with shadows, IBL, SSAO, fog, normal map, skinning, morph,
  instancing and fade; 2D lights and shadows; terrain including `mesh:Ter,Puc,Nor,Shw,Tpb,Tan,Nmp`;
  mirror; depth incl. `Txr`; G-buffer; UI; points; lines; sky; shadow2d); smoke tests with
  screenshots of shader-library (whole, each group, the phase 2 views), stencil (and its
  pre-pass/effects runs), extra-pass (and `EP_SCENE=figure`), shader-regressions, unified-lighting,
  runtime-shaders and prefab-runtime; `check.sh` on all five export scenes (Stencil: mean
  difference 0.57); the MCP suite (256 passed, 3 slow ones skipped); an export with `--backend
  all` of the export fixture (274 variants on six backends, none failed or missing); the 15
  scene and prefab files of the fixture projects re-saved with this editor and with the phase 1
  tag's: 14 byte-identical, `shader-library-export/Stencil.scene` differing only by phase 2
  data (the extra passes and stencil the phase 1 editor drops, and the precompiled variants of
  the phase 2 mask shaders); the 6-light scene pixel for pixel as the phase 1 tag (SSAO and SSR
  off, on, each alone, a three-pass post-process chain) and as 0c59f0a0 (the same but the
  chain). Game-build sizes (Linux GCC): `Material` 1928, `Submesh` 6976, `MeshComponent` 126080
  bytes (phase 1 tag: 1888 / 6752 / 122480).
