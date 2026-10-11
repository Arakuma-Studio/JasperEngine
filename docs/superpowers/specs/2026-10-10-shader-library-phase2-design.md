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
