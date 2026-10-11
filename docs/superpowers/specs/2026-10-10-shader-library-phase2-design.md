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
  pass (the sphere takes the whole material). Known: a material whose colour write leaves
  out alpha previews transparent (the preview target keeps its cleared alpha).
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
