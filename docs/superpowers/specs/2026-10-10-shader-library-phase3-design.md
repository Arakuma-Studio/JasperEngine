# Shader library, phase 3: scene depth and scene colour

Status: approved scope, refined against the merged phase 1 and 2 code (tag
`shader-library-phase2`).
Depends on: phases 1 and 2. Extends their contracts; does not redefine them.

## 1. Goal

Let mesh shaders read the opaque scene's depth and colour, so the library gains the
effects that need them (soft particles, depth-fade foam water, glass, heat haze, projected
decals, intersection highlights), while scenes that use none of them pay nothing.

## 2. Requirements

### 2.0 Conventions inherited from phases 1-2 (binding)

- Manifest inputs are lowercase like `smoothnormals` / `rawcolor`: the new ones are
  `@require scenedepth` and `@require scenecolor` (with optional `feature=<F>` gating, so a
  shader pays only when the feature is on, e.g. particle `SOFT`).
- Blend modes reach shaders as `BLEND_*` defines; outputs go through `finalizeColor` /
  `finalizeStraightColor`; fragment UVs use `SURFACE_UV_IN` (`includes/surface_uv.glsl`).
- Scene inputs must work for main draws AND extra passes (`MaterialExtraPass`), for
  instanced meshes, sprites and points.
- Phase 1 §13 and phase 2 §5 implementation notes apply.
- New scene setting (serialized, editor, Lua, MCP like other scene settings):
  `SceneSettings::sceneColorScale` — `FULL` or `HALF` (default `HALF`), the resolution of the
  scene-colour copy.

### 2.1 Scene inputs

- Manifest `@require sceneDepth` and `@require sceneColor` (phase 1 section 4 directive,
  new values).
- `includes/scene.glsl` API for library shaders:

  ```glsl
  vec2  screenUV();                       // 0..1, correct orientation on every backend
  float sceneLinearDepth(vec2 uv);        // view-space distance of the opaque surface
  float fragmentLinearDepth();            // view-space distance of the current fragment
  vec3  sceneWorldPosition(vec2 uv);      // reconstructed from depth
  vec3  sceneColor(vec2 uv);              // linear colour of the opaque scene
  ```

- The engine produces scene depth only when a visible draw in the current camera pass
  requires it, and scene colour only when one requires `sceneColor`. Producing them:
  - Depth: reuse the existing opaque depth pre-pass when it is already running (SSAO,
    depth-sampling post passes, depth prepass setting); otherwise run it on demand. The
    format and decode must be correct on GLES3 (no float targets) and match
    `depth_util.glsl`.
  - Colour: one copy of the opaque + sky result taken after the opaque queue and before
    the transparent queue, at a configurable scale (full or half resolution, half by
    default), linear filtering.
- Requiring materials are drawn in the transparent queue; an opaque material that requires
  a scene input is reported as a manifest/state error in the inspector and check tooling.
- The copy contains opaque geometry and the sky only; transparent objects requiring scene
  colour do not see each other (documented, as in URP's opaque texture).
- No new per-frame work, targets or passes for camera passes in which no visible draw
  requires a scene input; GPU memory for the targets is released when no longer needed
  for a while (or kept and reused; justify).
- Works with fixed-resolution and render-to-texture cameras; the editor Scene view shows
  the effects too.
- Mobile cost is visible to the user: the shader picker marks shaders that require scene
  colour or depth, with a tooltip about the extra pass.

### 2.2 Library shaders and features

| Name / feature | Technique |
|---|---|
| `particle` `SOFT` and `unlit` `SOFT` | Soft particles: fade alpha by `saturate((sceneDepth - fragmentDepth) / softness)`; also camera near fade. Works on points and instanced-mesh particles. |
| `water` `DEPTH_FOAM`, `ABSORPTION`, `REFRACTION` | Shoreline/intersection foam from depth difference, depth-based colour absorption, refraction of the scene colour by the water normals. |
| `glass` | Refraction (scene colour offset by normal map and IOR-like strength), tint, fresnel reflection from the reflection probe or a cubemap, optional blur by sampling a mip/half-res copy. |
| `heat_haze` | Screen distortion of the scene colour by scrolling noise, masked by a texture/vertex alpha. |
| `rim` `INTERSECTION` | Force-field highlight where the surface intersects geometry. |
| `projected_decal` | Box projector: reconstruct the world position from scene depth, project into the decal box, clip outside, sample texture; angle fade; drawn as a cube with front-face culling off-camera-safe. |
| `depth_fade` | Fog cards and light shafts: fade by depth intersection and by camera distance (replaces the owner's platform-dependent clip-z fade). |

## 3. Verification

- Compile matrix covers the new requirements and shaders on all six backends.
- Fixture scene group with soft particles intersecting geometry, depth-foam water on a
  shore, glass in front of geometry, heat haze, projected decals on uneven ground and a
  force field intersecting the floor; smoke test passes and the screenshot shows each.
- A scene with none of these shaders runs no depth pre-pass or colour copy it did not run
  before (verified through a pass-count debug log or counter).
- Phase 1 and 2 regression checks still pass.

## 4. Implementation notes

Decisions taken while implementing phase 3, and deviations from the text above. They
override the sections they refer to; later agents follow them. The contract for library
shaders and the editor is also written out in `/tmp/shader-research/p3-api.md`.

### 4.1 Engine (sl/p3-engine)

- Manifest (§2.0, §2.1): `@require scenedepth` / `@require scenecolor` (lower case; §2.1's
  `sceneDepth` spelling is not accepted), with `feature=` gating like every input:
  `ShaderRequire_SceneDepth` (1<<8), `ShaderRequire_SceneColor` (1<<9), mask
  `ShaderRequire_SceneInputs`, defines `SURFACE_SCENE_DEPTH` / `SURFACE_SCENE_COLOR`. Manifest
  errors: a shader type other than mesh or points; a `@texture` named `u_sceneDepthTexture` /
  `u_sceneColorTexture`; a mesh variant reading the scene whose state does not put it in the
  transparent queue (`queue=transparent` or a blend other than opaque), checked for the default
  features and each `feature=` rule adding a scene input. Accessors
  `ShaderManifest::getPossibleSceneInputs()` (what some variant may read: the picker's cost mark)
  and `getRequireNames(flags)`. MCP `list_shaders` gives `scene_inputs` on every entry;
  `docs/shaders/library.md` a "Reads the scene" line.
- `includes/scene.glsl` (fragment stage): `screenUV()`, `sceneLinearDepth(uv)`,
  `fragmentLinearDepth()`, `sceneWorldPosition(uv)`, `sceneColor(uv)`, plus the helpers
  `sceneRawDepth(uv)` and `sceneDistanceFromDepth(uv, depth01)`. Screen UV is y up (NDC-like) on
  every backend; distances are view-space distances in world units from the camera plane
  (computed from rows of the inverse projection, so perspective, orthographic and oblique mirror
  projections agree), the far clip distance where nothing opaque was drawn; the colour is linear
  (fog included), decoded from the copy. Without either define the include declares nothing.
  Engine block `u_fs_sceneInputs` (`scene_inputs_t`: inverse view-projection, inverse projection
  rows z and w, screen UV, copy UV and depth UV transforms), textures `u_sceneDepthTexture` (packed
  RGBA8, nearest) and `u_sceneColorTexture` (linear), bound by name per draw
  (`RenderSystem::applySceneInputs`; far depth and black where the pass has none). The Y
  orientation is a per-pass uniform (backend row order, the pass's flip, the viewport), not a
  compile-time switch.
- Producing them (§2.1): the loaded meshes (main draws and extra passes, instanced, sprites) and
  points whose variant reads the scene are recorded by entity
  (`sceneInputMeshes` / `sceneInputPoints`); each camera pass checks only those, with its draws'
  visibility. Depth: the SSR G-buffer, else the SSAO depth pre-pass, else the depth-reading post
  pass's pre-pass, else its own pre-pass of the opaque meshes (`renderOpaqueDepth`, which
  `renderDepthPrePass` now calls) at the colour viewport's size. The depth pre-pass *setting*
  (`depthPrepassEnabled`) draws into the colour pass's own depth attachment, which cannot be
  sampled while bound: it is not reused, the scene depth is a separate pre-pass (deviation from
  the brief's wording). While any loaded object reads the scene depth every mesh keeps a depth
  program (`MeshShaderEnvironment::sceneDepth`; the export collector adds it from the scene and
  prefab content). Colour: the colour pass is split (depth and stencil stored, resumed with
  everything loaded: `CameraRender::startSplitRenderPass` / `resumeRenderPass`); a 3D pass copies
  after its opaque queue and the sky (so UI, lines and the editor's gizmos are not in it), a
  hierarchy-order pass right before its first draw reading the colour. The copy is the blit
  shader (its parameter is now a UV scale/offset; the fixed-resolution blit draws the same) into
  a linear target at `sceneColorScale`.
- `SceneSettings::sceneColorScale` (`SceneColorScale::FULL` / `HALF`, default HALF): Lua
  `scene.sceneColorScale`, YAML `sceneColorScale: full` (written only when not the default, so
  older scenes re-save unchanged; export codegen likewise), Catalog / MCP `scene_color_scale`
  (`full`, `half`), inspector Scene settings "Scene Color > Scale".
- Exported scene camera drawing straight to the swapchain (no SSR, post-process, fixed
  resolution): while a scene-colour reader is in its view (or was, in the last 30 frames) it draws
  into an offscreen target blitted to the screen (`updateSceneColorRedirect`; the scene keeps
  RTT pipelines for the switch, the matrices are remade with the other Y flip on a switch; the
  target freed after 180 frames unused). The swapchain's MSAA does not apply in those frames
  (as with SSR and post-process). A layer scene drawn straight to the screen gets no colour copy.
- Mirrors: a mirror camera's depth pre-pass draws mirrored in x, so it culls as its
  reversed-winding colour pass does; the depth UV transform mirrors it back.
- Reflection probes: their faces draw the opaque queue only, so readers (transparent) are not in
  them; an opaque reader would get the fallbacks there. Documented rule, no probe support.
- Targets: per view (`RenderView::sceneInputTargets`), one per size and kind, reused across
  passes and frames, freed 180 draws after their last use.
- Opaque-queue reader (§2.1): `RenderSystem::getSubmeshSceneInputs` (main shader and extra pass,
  with the submesh's queue); an error under the material's Surface rows in the inspector, and in
  `check --shaders` / `check_shader` without arguments
  (`ProjectChecks::sceneInputFindings`, per GameObject and submesh). The picker mark and the rest
  of the editor are the editor wave's.
- Diagnostics: with `JASPERENGINE_READY_SIGNAL` a "Scene inputs: ..." line follows "Render
  objects" (readers, last frame's reading passes, depth pre-passes, shared depths, copies,
  targets alive and made). `JASPERENGINE_FRAME_STATS=1` logs sokol's frame statistics 2, 30 and 90
  frames after "Engine ready" (passes, draws, pipelines, live and made images, pipelines, views).
- Pre-existing, fixed: a post-process pass sampling the depth (no SSAO, no SSR) drew the depth
  pre-pass over meshes without a depth program (only casters and the SSAO / pre-pass settings
  had one); their draw failed and flagged a reload every frame (phase 2 tag: 72, 128, 248
  pipelines made at frames 12, 40, 100), and they were missing from its depth. Such a pass now
  gives every mesh a depth program like a scene-depth reader (collector: the chain's forks
  declaring `u_depthTexture`).
- Fixture `tools/fixtures/scene-inputs`: fixture-only forks (`si_world_grid`, `si_depth_grid`,
  `si_intersection`, `si_refraction`, `si_haze`, `si_soft_points`, `si_post_depth`), a
  render-to-texture camera, switches `SI_ORTHO`, `SI_FIXED`, `SI_PREPASS`, `SI_SSAO`, `SI_SSR`,
  `SI_FULL`, `SI_2D`, `SI_INVISIBLE`, `SI_MIRROR`, `SI_POST`, `SI_SCENE=identity|hidden|saved`;
  `Saved.scene` from `generate.py`. MCP tests `test_scene_inputs.py`,
  `test_scene_color_scale.py`.
- Checks run: full `tools/build.sh linux`; `shaders --library all --backend all` (29 shaders, 8700
  compiles, 0 failures) and 44 built-in variants on six backends (Lit tiers and modes with
  shadows, IBL, SSAO, fog, normal map, skinning, morph, instancing and fade, 2D lights, terrain,
  mirror, alpha modes; depth incl. `Txr`; G-buffer; UI; points; lines; sky; SSAO; SSR; composite;
  shadow2d; blit; postprocess), 0 failures; an export `--backend all` of the scene-inputs fixture
  (24 variants x 6 backends, 0 failures); smoke tests with screenshots of scene-inputs (every
  switch above) and of shader-library, stencil, extra-pass, shader-regressions, unified-lighting
  and runtime-shaders; the editor's Scene and Game views of `Saved.scene` (MCP screenshots);
  `tools/fixtures/shader-library-export/check.sh` (all five scenes pass); the MCP suite (262
  passed, 3 slow ones skipped); `SI_SCENE=identity` (6 lights, directional and spot shadows,
  fog) pixel for pixel and frame-stats identical to the phase 2 tag build with SSAO and SSR off,
  on, each alone, and with the depth pre-pass setting (2 / 5 / 5 / 7 / 2 passes, same draws,
  images and pipelines); `SI_SCENE=hidden` (a reader out of view) runs the identity scene's 2
  passes and 11 images; every run's images and views made stay constant between frames 40 and
  100 (targets reused). Only the GL backend was run; the other backends were compiled, not run.

### 4.2 Shaders A (sl/p3-shaders-a)

- Soft particles (§2.2): `particle` and `unlit` gain `SOFT` (`@require scenedepth feature=SOFT`,
  alpha times `saturate((sceneLinearDepth - fragmentLinearDepth) / softness)`) and a separate
  `NEAR_FADE` (invisible at `nearFadeStart`, whole from `nearFadeEnd`, view-space distances),
  not part of `SOFT`: a camera fade alone reads no scene depth. Without a scene input its
  distance comes from the frame block's projection (z and w rows inverted at
  `gl_FragCoord.z`, right in perspective and orthographic cameras, approximate under a
  mirror's oblique near plane); with one it is `fragmentLinearDepth()`. The shared maths is
  `library/soft_fade.glsl` (`softIntersection`, `softViewDistance`, `softRamp`), also used by
  `depth_fade`. `unlit` is opaque by default, so `@state[SOFT]` and `@state[NEAR_FADE]` make it
  `blend=alpha zwrite=off shadows=off` (the transparent queue the scene-depth rule asks for; a
  soft card casts no shadow); a material blend still wins (additive glows), and the
  premultiplied blend fades the whole colour. Points have no queue; the particle's own
  `zwrite=off` is unchanged. Instanced meshes work through `unlit` (instance colour alpha).
- `rim` `INTERSECTION`: `pow(1 - saturate(gap / intersectionWidth), intersectionPower)` times
  `intersectionColor` (HDR), added to the fresnel glow (the pattern brightens it with
  `TEXTURE`); both take the rim colour's alpha, as the additive blend does.
- `projected_decal` (new, Effects): drawn as a unit cube (1 x 1 x 1, -0.5..0.5: the box
  primitive and the export fixture's `box.gltf`), sized by its transform. `@state blend=alpha
  zwrite=off ztest=always cull=front shadows=off`, `@keywords DECAL_BLEND ALPHA MULTIPLY` with
  `@state[DECAL_BLEND_MULTIPLY] blend=multiply` (as `decal`). Its decal space needs the box's
  inverse model matrix in the fragment stage: `projected_decal.vert` computes it in the
  displacement hook (model matrix times the instance matrix, affine inverse through
  `inverse(mat3)`) and passes three `flat` rows, so the decal needs no script or property to
  follow its transform and works instanced. The texture maps the box's XZ like a plane of its
  size (u along +X, v along +Z, `tiling`), projected down the box's Y axis. Fades: `edgeFade`
  towards the box's top and bottom (a slope leaving the box ends softly); `ANGLE_FADE` (default
  on, `angleFadeStart`/`End` in degrees, cosines computed per vertex into a flat varying) with
  the surface normal from the screen derivatives of the reconstructed position, turned towards
  the camera (the derivatives' handedness differs between backends), against the box's Y axis
  in world space (the gradient of its local y). Chosen over an up-axis or height-only fade,
  which cannot tell a stair riser from a tread; it costs no extra depth sample and leaves at
  most a pixel-wide seam along depth steps (not visible in the fixture's stairs at 1280x720).
  The camera inside the box works (the back faces still cover the screen). With
  `ztest=always` a decal drawn after transparent objects would paint over them: the
  description asks for a material sort priority below them (-1); the manifest has no sort key.
  Unlit like `decal`.
- `depth_fade` (new, Effects): texture times HDR tint and vertex/instance colour, both faces,
  `blend=alpha zwrite=off shadows=off`, `@keywords FADE_BLEND ALPHA ADDITIVE` (light shafts).
  `SOFT` default on (`softness`); near (`nearFadeStart`/`End`) and far (`farFadeStart`/`End`)
  camera fades always on (smoothstep ramps of the view distance; they replace the owner's
  `pos.z / pos.w` fade); `SCROLL` (`scrollSpeed`, wrapped with `fract`), `MASK` (red times
  alpha of `u_maskTexture` at the untiled UV, so a grey mask and a white particle sprite both
  shape the card), `EDGE_FADE` (`pow(|n.v|, edgeFadePower)`, `@require normals position
  feature=EDGE_FADE`).
- Every manifest of these shaders states the mobile cost of reading the scene depth.
- Pre-existing, fixed: `check_shader` / `check --shaders` validated a variant's rows with the
  features of its (already resolved) reference resolved again, which turned a default-on
  feature the variant has off back on: `projected_decal` with `!ANGLE_FADE` and `depth_fade`
  with `!SOFT` reported their rows as missing (`ProjectChecks::validateShaderManifest` now takes
  the reference's defines as they are). The `shaders --library` matrix never compiled a lone
  default-on feature off: it now adds a set with each default-on feature off
  (`editor/shader/ShaderLibraryMatrix.cpp`).
- Fixture: group `phase3a` of `tools/fixtures/shader-library` (`scripts/phase3a.lua`, its header
  lists the cells and `PH3A_VIEW` close-ups: soft, near, quads, field, decals, stairs, decals_b,
  fog, inside, enter, ortho). `tools/fixtures/shader-library-export/Phase3A.scene` from
  `generate.py` (`build_phase3a`), checked by `check.py`; only that scene, `project.yaml` and
  `fx_dirt.png` were taken from the regeneration (the other scenes come out identical but for
  their ids).
- MCP tests updated for the new manifests: `test_shader_library_editor.py` (unlit's features,
  rim's `require_rules`) and `test_scene_inputs.py` (the "reads nothing" case is now `toon`;
  `unlit` and `projected_decal` list `scenedepth`).
- Checks run: full `tools/build.sh linux`; `shaders --library all --backend all` (31 shaders,
  9132 compiles, no failure or manifest diagnostic); smoke tests with screenshots of the
  `phase3a` group (every `PH3A_VIEW`, `enter` at three times: outside, inside, outside), of
  `scene-inputs` and of the whole `shader-library`; seen in them: no hard line where soft points
  and soft quads cross the wall and the floor (their hard twins cut), near-faded points thin out
  towards the camera, the shield's band follows the ground line, the bump and the crate, decals
  undistorted on flat ground, on the treads only with the angle fade (stretched down the
  risers without it), clipped at the box's far edge with the camera inside, right in the
  orthographic view, fog cards soft where they cross the ground, the rock and the wall (the
  SOFT-off card cut); `tools/fixtures/shader-library-export/check.sh` (six scenes pass, Phase3A
  mean difference 0.84); the MCP suite (262 passed, 3 slow ones skipped). Only the GL backend
  was run; the others were compiled.

### 4.3 Shaders B (sl/p3-shaders-b)

- `includes/output.glsl` (small addition, no change to the existing functions): `applyFog(vec3)`
  (the scene fog on a colour, identity without `HAS_FOG`) and `finalizeFoggedColor(vec4)`
  (`finalizeStraightColor`'s blend shaping and encoding without the fog). The scene colour has its
  fog already, so a shader that composites its own colour over it writes
  `finalizeFoggedColor(mix(scene, applyFog(own), a))`; the fog is affine in the colour, so this is
  what blending the fogged colour over the scene draws. Water (REFRACTION), glass and heat haze use
  it; `finalizeColor(sceneColor(...))` (as the scene-inputs fixture forks do) fogs the scene twice.
- `water` (§2.2): `DEPTH_FOAM`, `ABSORPTION` (`@require scenedepth feature=...`) and `REFRACTION`
  (`@require scenecolor scenedepth feature=REFRACTION`); the base state already blends, so every
  variant is in the transparent queue. Thickness = the view-ray length through the water
  (`(sceneLinearDepth - fragmentLinearDepth) / dot(ray, camera forward)`, 1 for orthographic
  cameras); depth below the surface = thickness * |n.v| with the normal without the ripples.
  DEPTH_FOAM: a band dense at every contact line, thinning out to none at `shoreFoamWidth` (0.12)
  of depth, holes cut by the foam noise (`shoreFoamBreakup`). ABSORPTION: shallow to deep by
  `1 - exp(-thickness / absorptionDepth)` (at least the phase 1 fresnel term), alpha fading in over
  `shoreFade`. REFRACTION: the scene colour at the screen UV shifted by the ripples' view-space
  bend (`refractionStrength` screen UV per unit, x scaled by the aspect), growing over
  `refractionDepth` of thickness (none at the shore line), scaled so it never leaves the screen
  (no smeared border), the straight sample where the shifted one is in front of the surface; the
  water is mixed over it and written with alpha 1 (with ABSORPTION the shifted sample's thickness
  sets the colour). With the three off the variant compiles to the phase 1 code: the export
  fixture's Lit and Blend scenes (water unlit and pixel lit with WAVES, and in all five blend
  modes) are pixel for pixel identical to c46fa1d6, editor renders and game screenshots.
  Limits (documented in the shader): vertical faces seen from straight above leave no depth gap,
  so no foam there; the rejection of foreground samples leaves a faint line along silhouettes
  crossing the water (as URP's opaque-texture refraction).
- `glass` (new, category Effects, `@lighting none pixel`, `@require normals position scenecolor
  scenedepth`, `uv1` with NORMAL_MAP, `blend=alpha zwrite=off shadows=off`): the view ray bent by
  `refract(-v, n, 1 / ior)` over `thickness`, projected with `frame.projectionMatrix` (|P00|,
  |P11|: a flipped render-to-texture projection keeps screen UV y up) at the fragment's view depth
  (1 for orthographic), so the shift shrinks with distance; same screen clamp and foreground
  rejection as water. Transmission: scene * tint * mix(1, absorptionColor, 1 - exp(-absorption *
  thickness / |n.v|)). Reflection: Schlick with f0 from the ior, `reflectionColor` *
  `reflectionStrength`, times a cubemap with `REFLECTION_MAP` (`@texture u_reflectionTexture black
  cube`). Decision: library shaders have no IBL or reflection-probe binding (Lit's; 13.3), so the
  probe/IBL is not used; wiring it would be an engine change (a probe/IBL block and textures for
  manifest shaders). Pixel lights add a Blinn-Phong glint (shadowed). `NORMAL_MAP` (tiling,
  strength) with simple_lit's derivative tangent frame; `BLUR`: four bilinear taps on a rotated
  square `blurRadius` copy texels out (at the default half-resolution copy each tap averages 2 x 2
  pixels). Lua cannot set a cube shader texture (`Material:setShaderTexture` takes a 2D path), so
  the Lua fixture shows the reflection colour; MCP (`{"cube": path}`) and the inspector can, and
  the export scene does.
- `heat_haze` (new, Effects, `@require uv1 color scenecolor`, `normals position` with
  `EDGE_FADE_FRESNEL`, `scenedepth` with `SOFT`; `blend=alpha zwrite=off cull=off shadows=off`):
  two layers of a noise texture's red/green (`noiseTiling`, `scrollSpeed`, `secondScrollSpeed`, the
  second 1.7 times smaller) shift the scene colour by up to `strength` screen UV; the mask (vertex
  and instance alpha, `MASK_TEXTURE` red times alpha, `@keywords EDGE_FADE UV FRESNEL NONE`,
  SOFT) scales the shift and is the alpha. SOFT fades within `softness` of the opaque scene and
  rejects samples in front. The default noise (gray) shifts nothing: assign a smooth noise.
  Closed volumes should cull back (the far side's UV seam shows through otherwise); EDGE_FADE_UV
  assumes 0..1 UVs (an atlas sprite's rect UVs fade at the rect's place in the atlas).
- `docs/shaders/library.md` regenerated. Its "Reads the scene" line (editor/shader/
  ShaderLibraryDoc.cpp) names both inputs for every scene-reading shader, from
  `getPossibleSceneInputs`, whatever feature adds them; the Inputs line says which feature does.
- Fixture: `tools/fixtures/shader-library` group `phase3b` (`scripts/phase3b.lua`): four pools
  over a checker bed sloping out of the water, rocks and tilted blocks crossing the surface,
  divider walls (all features lit, then each alone); glass before vertical stripes (turned clear
  pane, frosted, thick green normal-mapped, a lens sphere with a red post in front, a lit tinted
  sphere); heat haze over opaque flames, a fresnel sphere portal and a soft-masked quad. Views
  `P3B_VIEW=water|water_low|water_edge|glass|haze`, `P3B_ORTHO=1`. Export:
  `shader-library-export/Phase3B.scene` (generate.py `build_phase3b`: a pool with the three water
  features, a frosted and a normal-mapped pane, a lit glass sphere with REFLECTION_MAP on a
  generated horizontal-cross cubemap, a SOFT haze quad and a fresnel haze sphere, every scroll
  frozen); check.py compares it when present. Only Phase3B.scene, project.yaml's scene list and the
  cubemap were taken from the generate run (the other scenes regenerate identical but for ids).
- Checks run: full `tools/build.sh linux`; `shaders --library all --backend all` (31 shaders, 9378
  compiles, 0 failures, no manifest diagnostic; water 84, glass 65, heat_haze 40 variants);
  smoke tests of `SL_GROUPS=phase3b` (every view, and `P3B_ORTHO=1`; screenshots read: foam along
  the shores, rocks, blocks and dividers; absorption from the clear shore to the deep front;
  refraction of the bed with the rocks, blocks and lip above the surface not pulled into it and no
  smear at the left and bottom borders; frosted pane blurred at the half-resolution copy; lens
  sphere; the post in front of it not refracted), heat haze animated (screenshots 6 s apart: the
  three haze areas changed, mean 25 to 46 of 255, the rest 0), the whole shader-library fixture,
  scene-inputs (default, `SI_SCENE=saved`, `SI_ORTHO=1`); `check.sh` (six scenes pass, Phase3B
  mean difference 0.46); the phase 1 water identity above; the MCP suite (262 passed, 3 skipped).
  Only the GL backend was run.

### 4.4 Editor (sl/p3-editor)

- Shared logic (§2.1 cost visibility): `Catalog` decides and words everything the inspector and
  MCP say, so both agree: `getSceneInputsLabel(bits)` ("depth", "colour", "depth and colour"),
  `getSceneInputsCost(bits, brief)` (brief: "a depth pre-pass and a colour copy per camera"; whole:
  only passes with a visible reader pay, the depth pre-pass and its sharing with SSAO/SSR/post,
  every mesh keeping a depth program, the copy's render-pass split on tile-based GPUs, Scene Color >
  Scale / `scene_color_scale`, avoid on low-end mobile, transparent queue only),
  `getShaderSceneInputsSummary(manifest)` ("Reads the scene depth (with SOFT) and colour"),
  `getFeatureSceneInputs(manifest, define, whenOff)` (the inputs a feature or keyword value turns on
  through its `feature=` rules, minus what the shader always reads; `whenOff` for `feature=!X`
  rules), `getShaderSceneInputs(shader, features)`, `getMaterialSceneInputs(scene, entity, submesh,
  extraPass)`, `getComponentSceneInputs`, and `getSceneInputQueueFix(scene, entity, submesh,
  blend)`: for a submesh that `RenderSystem::getSubmeshSceneInputs` reports as reading from the
  opaque queue, Alpha, only when Alpha does move it to the transparent queue (re-resolved on a
  copy of the submesh; a fork forcing another `queue=` gets no fix).
- Picker: every library-shader list (materials, extra passes, UI/points/lines/sky components,
  scene defaults: `drawLibraryShaderCombo`) marks a shader whose `getPossibleSceneInputs()` is not
  empty with an amber gauge and what it may read; its tooltip is the description, the summary,
  the brief and the whole cost. The closed combo's tooltip adds the summary and brief cost. Feature
  checkboxes and keyword values whose rules turn an input on get the same mark and tooltip ("On,
  it reads the scene depth: ..."). Forks are not in the picker list; their feature rows are marked.
- Info row: `drawShaderManifestRows` ends with "Reads scene <what>: <brief cost>" (amber, wrapped,
  the whole cost in its tooltip) when the variant drawn (the shader with its features) reads the
  scene: a material's shader, its extra pass's, a points component's. Like the other manifest
  rows it shows for a single selection.
- Opaque-queue error (§4.1): the row is wrapped and gets a "Use Alpha blending" button: one
  `MultiPropertyCmd` (one undo step) setting `submeshes[N].material.blendMode` to Alpha on every
  selected GameObject for which `getSceneInputQueueFix` holds. An extra pass reading the scene on
  an opaque submesh gets the same fix (the pass draws at the submesh's queue position; the engine
  has no per-pass queue).
- Scene settings: the Scene Color > Scale help now weighs quality against cost (half: a quarter of
  the pixels, a 2x2 average, right for distortion and mobile; full: sharp, four times the
  bandwidth and memory). No "scene inputs in use" indicator: the engine has no accessor (the
  readers and the last frame's stats are private `RenderSystem` members, only logged as the
  "Scene inputs:" line); an engine getter (e.g. `uint32_t getSceneInputUsage() const`) would be
  needed, outside this branch's files. Not added.
- MCP: `list_shaders` adds `cost_note` to every entry whose `scene_inputs` is not empty (summary:
  one line, also appended to the text line in brackets; detail: plus the whole cost); in detail
  each feature has `scene_inputs` / `scene_inputs_when_off` and each keyword group a
  `scene_inputs` map by value when they turn an input on. `get_entity` adds, for an entity with a
  reader, `scene_inputs` (per reading submesh: `reads`, `extra_pass_reads`, `queue`, and on the
  opaque-queue error `error` and `fix`, the `set_properties` values of the inspector's button;
  per reading points component: `reads`), `scene_inputs_cost`, and a sentence on the text line
  ("ERROR: ..." when one draws opaque). Terrains are skipped as in `sceneInputFindings`.
  `get_shader_uniforms` adds `scene_inputs` and `cost_note` for the material's or component's
  variant. Outputs of entities and shaders without scene inputs are unchanged.
- Docs: `docs/mcp/guide/shaders.md` "Scene depth and colour" (when to use, cost, the
  transparent-queue rule and its fix, the opaque+sky-only copy, what the tools report);
  `McpInstructions` one sentence (16,356 of 16,384 bytes; the shaders paragraph tightened;
  `scene_inputs` and `cost_note` added to the guide test's non-tool words); `TOOLS.md` regenerated.
- Tests: `tools/mcp/tests/test_scene_inputs_editor.py` (forks: depth always, colour with a
  feature; points): list_shaders `cost_note` and that features plus requires make up
  `scene_inputs`,
  get_entity / get_shader_uniforms per variant, the opaque-queue error and fix, applying the fix
  through `set_properties`, undo restoring the error, an extra pass reader, a points reader.
- Checks run: full `tools/build.sh linux`; the MCP suite (268 passed, 3 slow ones skipped);
  inspector screenshots over MCP (harness Xvfb, xdotool hovers) with a temporary manifest hack
  (`unlit` given a `SOFT` feature requiring `scenedepth`, reverted, full rebuild after): the
  picker's gauge on Unlit and its tooltip, the feature checkbox's gauge and tooltip, the info row,
  the opaque-queue error with its button; the button set Alpha (error gone) and undo restored
  Opaque (read back through `get_entity`); the 17 scene and prefab files of the fixture projects
  re-saved with this editor and with a build of c46fa1d6: all byte-identical.

### 4.6 Review fixes: shaders (sl/p3-fix-glsl)

Fixes of the GLSL review at 914391fd (the engine-side `@state priority=` and projected_decal's
`priority=-1` are the engine-fix branch's).

- Includes (additive; existing callers unchanged): `fog.glsl` `getFogFactor(fogDist)` and
  `getFogColorAtDistance(color, fogDist)` (`getFogColor` is that at `gl_FragCoord.z /
  gl_FragCoord.w`); `output.glsl` `fragmentFogDistance()`, `finalizeColorAtFogDistance`,
  `finalizeStraightColorAtFogDistance`, `finalizeBlendedColorAtFogDistance` (the plain ones call
  them), `fadeByFog(color, neutral)` / `fadeByFogAtFogDistance` (a filter fades to white, a glint
  to black), `sceneFogDistance(uv, depth01)` (declared after `includes/scene.glsl` with
  `SURFACE_SCENE_DEPTH`: the fog distance the opaque surface under `uv` got, `depth01 /
  dot(inverseProjectionW, ndc)`, i.e. its depth times its clip w: right in perspective,
  orthographic and oblique projections), `encodeColor(linear)` and
  `finalizeStraightColorOverScene(color, behind, behindWeight)`.
- Compositing over the scene colour (§4.3 correction): the targets hold display-encoded colour
  and the hardware blends those values as stored, so §4.3's in-shader `mix(scene, applyFog(own),
  a)` in linear did not draw what the blend draws (G 0.8, a 0.5 over black: 0.66 against 0.45).
  `finalizeStraightColorOverScene` mixes encoded values: weight 0 is `finalizeStraightColor` over
  the target bit for bit, 1 replaces the target (alpha 1), in between it outputs coverage `a + (1
  - a) w` of `mix(own, behind, (1 - a) w / coverage)` (alpha and premultiplied blends); Additive
  and Multiply cannot remove what is behind and draw `finalizeStraightColor`.
- Transparent objects behind (review item 1): (a) water, glass and heat_haze descriptions and
  `docs/mcp/guide/shaders.md` say plainly that they hide transparent objects behind them. (c)
  Water: the refracted sample's weight is `clamp(refractionStrength * reach * resolution.y / 2)`
  (whole once a unit of ripple shifts it 2 pixels; `reach` the thickness ramp), so at
  `refractionStrength` 0 REFRACTION draws exactly the water without it (transparent objects
  behind included) and the first bit of thickness at the shore blends over the target. Glass:
  not possible (its transmission filter multiplies what is behind by a colour, which an alpha
  blend cannot do to the target): documented. Heat haze already blends by its mask (objects
  behind show where it fades): documented. The REFRACTION-only tint fade over `refractionDepth`
  is gone (it changed the water at strength 0; ABSORPTION's `shoreFade` is the soft shore).
- `projected_decal`: fogged at `sceneFogDistance` of the surface under it, not at the box's
  back face (the multiply blend fades to white by it); the texture sampled with `textureGrad`,
  each UV derivative held to 4 times the smaller one (a depth step under the quad makes one jump:
  no tiny mip at stair nosings and outlines, up to 4:1 stretch kept at grazing views); a box
  scaled to zero has no inverse: the vertex stage sends every point outside (rows `(0,0,0,1)`,
  0, 0) and the fragment test is `!(max <= 0.5)`. The description's and comment's sort-priority
  advice is left to the engine-fix branch.
- `glass`: the seen side is decided by the mesh's normal before the normal map (the mapped
  normal flipped pixel by pixel at grazing views: dark speckle along the pane), a mapped normal
  tilting away is pulled back just past the silhouette (continuous); composite in display values
  as three blended layers would draw it (scene times the transmission filter, the reflection
  mixed by the fresnel, the glint added); the tint and absorption filter fades to white with the
  fog, the glint to black. Additive draws the reflection and glint, Multiply the filter.
- `heat_haze`: the tint fades to white with the fog; the description says that without SOFT a
  shift landing on an object in front smears it into the haze.
- Re-read for the same bug classes: depth_fade, particle / unlit SOFT and NEAR_FADE and rim
  INTERSECTION draw their own fragment (their fog and blends are right). The scene-inputs
  fixture forks `si_refraction` / `si_haze` (`finalizeColor(sceneColor(...))`, fog twice) are
  fixture code, not changed.
- Fixture: `phase3a` `PH3A_FOG=1` (dense exponential scene fog) and a decal box scaled to zero
  height before the stairs; `phase3b` a transparent red box under pool 4 (sortPriority -1),
  `P3B_REFRACTION=zero|off` (strength 0 or REFRACTION off on pools 1 and 4, every scroll stopped:
  the regression check is that the two screenshots are identical), `P3B_VIEW=glass_grazing`.
- Checks run: full `tools/build.sh linux`; `shaders --library all --backend all` (33 shaders, 9810
  compiles, 0 failures, no manifest diagnostic) and built-in `mesh:Fog`, `mesh:Uv1,Nor,Fog`,
  `mesh:Ult,Fog` on six backends; smoke tests of `SL_GROUPS=phase3b` (water, water_low,
  water_edge, glass, glass_grazing, haze, P3B_ORTHO, and the zero/off pair) and `SL_GROUPS=phase3a`
  (decals, stairs, decals_b, and with PH3A_FOG decals, decals_b, inside, ortho) on GL and
  `SMOKE_BACKEND=gles3`: `P3B_REFRACTION=zero` and `=off` 0 differing pixels on both backends
  (with the transparent box visible through pool 4 in both); A/B runs against the 5e8a0a92
  glass and decal (as fixture forks): the grazing normal-mapped pane lost its dark speckle;
  under fog the decal ink went from 107.6 to 104.2 (the surface's fog, the ground around 104.2);
  without fog the decals are unchanged. Not reproduced on llvmpipe: the zero-scaled box drew
  nothing with the old shader too, and the fixture's decal texture showed no mip outline before
  either (both fixes are defensive there). `check.sh` (seven scenes pass; Phase3A mean 0.84,
  Phase3B 0.47; no regeneration needed); the MCP suite (268 passed, 3 skipped).

### 4.7 Review fixes: editor (sl/p3-fix-editor)

Fixes of the phase 3 review of the editor, MCP and fixtures (each reproduced first, each with a
regression test in `tools/mcp/tests/test_scene_inputs_editor.py`):

- `test_list_shaders_cost` already used toon as the shader reading nothing and asserted unlit's
  `SOFT` reading `scenedepth` (fixed by 8e04a5d4); the "(vacuous until ...)" wording of §4.4 is gone.
- Guide: `screenUV` and `fragmentLinearDepth` are declared with either input (`scene.glsl`
  declares them under `SURFACE_SCENE_DEPTH || SURFACE_SCENE_COLOR`); only `sceneLinearDepth` /
  `sceneWorldPosition` need `scenedepth` and `sceneColor` needs `scenecolor`.
  `test_guide_scene_inputs` derives what each function needs from the include's `#if` nesting and
  compares it with the guide's sentence.
- `Catalog::getComponentSceneInputs` of a MeshComponent was 0: `get_shader_uniforms` on a mesh
  whose own `customShader` reads the scene said nothing. It is now the OR over the submeshes
  drawing with the mesh's shader (no shader of their own) of that shader's variant with their
  material's features (the mesh's shader has no features of its own).
- `check --files <shader file>` listed the opaque-queue errors of every material.
  `ProjectChecks::sceneInputFindings(project, forks)` keeps, with a fork list, the materials whose
  colour reference (`getSubmeshShaderReference`) or extra pass reference is one of the forks
  checked (the same strings `shaderForksInUse` collects); `check --shaders` (every shader) passes
  none and reports all. MCP `check_shader` keeps its rule (state findings only without `shader`).
- `tools/fixtures/scene-inputs/generate.py` deletes the previous `Saved.scene` (`delete_scene`
  with its file, in the editor's project copy) before `create_scene`: re-runs work (run twice in
  a row; the regenerated scene differs only in its random id and the camera aspect, not committed).
- Guide additions: an exported game drawing straight to the screen draws offscreen and blits
  while a colour reader is visible and 30 frames after (no swapchain MSAA those frames); a layer
  scene (child scene, `Engine.addSceneLayer`) drawn straight to the screen has no copy, its colour
  readers read black (its depth readers see its own opaque meshes); render-to-texture cameras and
  editor views always have one. Also `@state priority=N` (one sentence, the engine-fix branch's
  manifest key). `list_shaders` does not show it: no accessor for it existed on this base.
- Duplicated comment line in `McpValues::manifestToJson` removed.
- Scene inputs in use (supersedes "No ... indicator" of §4.4): `Catalog::getSceneInputsInUse
  (scene)` maps `RenderSystem::getSceneInputUsage()` (bit 0 depth, bit 1 colour, the scene's last
  frame) to `ShaderRequire_` bits. Inspector: Scene Color gets an "In Use" row (amber gauge and
  "Depth and colour" / "Depth" / "Colour", or a disabled "None"; tooltip: the brief cost).
  `get_scene_settings` without `names` adds `scene_inputs_in_use` (not a setting; a name filter
  returns settings only); `TOOLS.md` regenerated. The accessor did not exist on this base: commit
  24bbcd41 adds a placeholder (from `lastSceneInputStats`: depth when a pre-pass ran or a depth was
  shared, colour when a copy was taken) for the engine-fix branch's real one to replace at merge.
  Checked in the editor (harness Xvfb, a 3D scene with a reader of both in the Scene view): the
  row reads "Depth and colour" in amber.
- Checks run: full `tools/build.sh linux`; the MCP suite (271 passed, 3 skipped; the timing test
  `test_cli_lua_isolated_state` failed under the parallel load and passed alone);
  `tools/fixtures/shader-library-export/check.sh` (all 7 scenes ok); `generate.py` of the
  scene-inputs fixture twice in a row (both 9 variants, 0 failed, 0 findings).

## 5. Definition of done

All requirements implemented, all checks pass, merged into `shader-library`, review pass
found no open correctness bugs.
