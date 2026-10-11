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
  When the stencil work merges, the pass's `stencil` and `colorWrite` must be passed there
  exactly where `resolveSubmeshRenderState` passes the material's. `Mesh::setMaterial`
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
  133 with ten outlined, one pipeline per pass and nothing for the other submeshes).
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
