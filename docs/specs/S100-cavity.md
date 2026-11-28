# S100 — Cavity: Blender-viewport-style ridge and valley shading for games

## 1. Goal

A built-in, runtime **Cavity** effect a game can switch on: convex edges (ridges) get lighter and
concave creases (valleys) get darker. It's the look of Blender's Solid-mode viewport "Cavity"
option, which low-poly and stylised artists like because it reads shape without baked maps. It
works on every opaque 3D material: Lit, Unlit, Toon, Fake Lit and library shaders. It runs on all
five backends and every export target. It costs nothing when off. People drive it from the
Inspector, Lua and MCP.

Why: users want this look in their own games without baking curvature maps, and the Unity
packages that do it (see §4) need a cavity-aware shader per material, or ship only half the effect.

## 2. Scope

**In scope**
- Two modes, as Blender has them. A third choice runs both:
  - **Screen**: thin, crisp lines from the difference between neighbouring pixels' normals.
  - **World**: a wider, soft bevel highlight and crease darkening from depth samples within a
    world-space radius, similar to SSAO.
- One full-screen draw inside the camera's colour pass, after the opaque queue and the SSR
  composite. It runs before the atmosphere composites, transparents, UI and overlays. It
  multiplies the colour that is already there.
- A view-space normal source when SSR's G-buffer isn't running: an optional normals attachment
  on the existing depth pre-pass.
- Settings rows in `PostProcessingSettings`, the Inspector group, Lua properties, MCP keys and
  status, a device-tier cell, docs, golden images and tests.

**Out of scope (follow-ups, not v1)**
- Ridge and valley colours, and Overlay or Additive blend modes. v1 is a monochrome multiply.
- A per-object or per-layer opt-out.
- Distance fade and height fade, beyond the pixel-radius fade in R4.
- Transparent objects, sprites, 2D cameras and UI. They are drawn after the effect or never
  reach the depth pre-pass, so they are untouched.
- An editor-only Blender-style "Solid" Scene view draw mode.
- Temporal accumulation and half resolution.
- A new post-processing preset. Presets replace the whole stack, so a cavity preset would wipe
  a user's other effects.

**Anti-goals** (failure even if it "works")
- On a smooth-shaded mesh, a line along every triangle edge.
- A line at every object silhouette. That's an outline, not cavity.
- Shimmer or crawling noise while the camera moves.
- Lines that vanish at 4K, or that turn into thick smears at 720p.
- Any cost at all with the effect off: no pass, no draw, no attachment, no allocation, no shader
  variant in exports.

## 3. Current state (read 2026-11-21, `main` at 64954dfd7)

- **Rendering is forward only.** `RenderSystem::renderCameraPass` (`engine/core/subsystem/RenderSystem.cpp:18917`):
  - Draws the sky, then the opaque queue (≈`:19763`).
  - Then the SSR composite when SSR is on (`:19773-19788`).
  - Then the atmosphere composites (`:19792`).
  - Then transparents, UI and overlays.
- **The scene colour target is 8-bit and display-encoded.** Each mesh shader applies the tone
  curve (`SceneSettings.h:206`, `:273-274`; `S13-post-fx.md` §2).
- **Scene depth** is packed RGBA8 in a colour target.
  - It comes from `renderOpaqueDepth` (`depth.frag`), or the SSR G-buffer's attachment 0.
  - It is shared through `sharedSceneDepth` (`RenderSystem.cpp:19092-19300`).
  - The background clears to 1.0.
- **Smooth view-space normals exist only in the SSR G-buffer.** They are octahedral, 16 bits split
  over attachments 1 and 3 (`gbuffer.frag`).
  - Without SSR, `ssao.frag:59-75` rebuilds flat face normals from depth.
  - There's no object-ID buffer.
- **The colour pass is the only multisampled pass.** Depth, G-buffer and SSAO are single-sample.
- **Post settings:**
  - They live per scene in `PostProcessingSettings` (`SceneSettings.h:163-364`).
  - The field table is `getFields()` (`SceneSettings.cpp:43-160`). A row there gives YAML and
    cooked compatibility, `set_scene_settings` keys, generic Inspector rows with undo, and export.
  - Inspector effect groups are listed in `editor/window/InspectorPostProcessing.cpp:69-226`.
    Edge Detect is under "Stylised", at `:151`.
  - Lua binds each field by hand in `CoreClassesLua.cpp:2497-2606`.
- **No cavity, curvature or ridge effect exists.** Edge Detect is Sobel on luma (`uber.frag`), not
  geometry. The procmat curvature nodes are baked from heightmaps and are not runtime.
- **What the sampled depth pre-pass misses or draws differently.** `renderOpaqueDepth`
  (`RenderSystem.cpp:11414-11500`) doesn't check `skipsDepthPrepass`; only the in-pass Z pre-pass
  does (`:19676`).
  - `drawMeshDepth` drops stencil masks and stencil-tested submeshes (`:11042`).
  - A forked colour shader without a depth fork is drawn with the built-in depth shader
    (`:3536-3547`, `:7271-7279`), so its pre-pass geometry can differ from what the colour pass
    draws. SSAO already lives with this.
- **The pre-pass draws each submesh with its own depth program.** That's the built-in `depth.frag`,
  library depth shaders (`dissolve_depth`, `flipbook_depth`, `foliage_depth`, `impostor_depth`,
  `surface_depth`, `unlit_depth`), users' forks of `depth.frag`, and terrain and voxel programs.
- **The sampled depth pre-pass only runs when something asks for it**: SSR, SSAO, volumetric fog,
  contact shadows, a user post shader or a scene-depth reader (`:19125-19300`). The optional Z
  pre-pass (`:19576`) runs inside the colour pass and can't be sampled.
- **Isolate and debug.** The Inspector has no isolate button; `EffectDef::isolate`
  (`InspectorPostProcessing.cpp:55-63`) only prices one effect alone for the cost label. Its rows
  are a flat `{field, label}` list with no sub-headers (`:50-53`). `set_post_processing_debug`
  only pins time, render scale and GPU cost. SSAO's debug view is a plain scene field,
  `ssaoDebug` (`SceneSettings.h:481`).

## 4. Reference (read-only, nothing copied)

Four sources were analysed, plus Blender's own code. **No code from any of them may be copied.**

| Source | What it does | Licence |
| --- | --- | --- |
| Blender Workbench (`draw/engines/workbench`) | Both modes; the original | GPL-2.0-or-later |
| malyawka/URP-ScreenSpaceCavity | Both modes, multiplied in a custom lit shader | BSD-3, but a close port of Blender's GPL shader |
| Cavitoon Lite (Unity package) | World only, post multiply | no licence; core loop ported from Blender |
| MattRix CurvatureImageEffect gist | Screen only, 5 taps | no licence |

What we take, as technique only:
- The two-mode split.
- The neutral-at-1 multiply `(1 − valley)(1 + ridge)(1 + curvature)`.
- The Alchemy obscurance term for World. The paper is McGuire et al., "The Alchemy Screen-Space
  Ambient Obscurance Algorithm", HPG 2011, which Blender cites.
- From Cavitoon's design:
  - a cap on the kernel's pixel radius, plus a fade when it is under a pixel;
  - a deterministic, rotated kernel that needs no temporal filter;
  - the rule that the sample count changes quality but not apparent radius.
- From Blender: no curvature across object boundaries, and a bright ridge at silhouettes against
  the background.

Implementers write the shader from this spec and the paper. They don't open the sources above
while writing it.

## 5. Requirements

### Functional

- **R1. Placement.** One full-screen triangle drawn inside the colour pass, immediately after the
  opaque queue and the SSR composite and before `atmosphere.drawComposites`.
  - It reads the scene depth and normals as textures.
  - It writes RGB only, with blend `src = DST_COLOR, dst = SRC_COLOR`, so out = dst · 2 · src.
  - It outputs the factor halved. 0.5 is neutral, so the factor's range is 0..2 on the 8-bit target.
  - Depth test and depth write are off. Alpha is untouched.
- **R2. Normals source.** Cavity always reads normals from the same pass as the depth it reads.
  - When the SSR G-buffer's depth is the scene depth, read the G-buffer's normals.
  - Otherwise the sampled depth pre-pass writes a second RGBA8 attachment with Cavity on. It holds
    16-bit-precision octahedral view-space normals from interpolated vertex normals, without normal
    maps (as Blender does), in the G-buffer's encoding. This covers the cases where the engine
    refuses the G-buffer depth (`RenderSystem.cpp:19089-19092`).
  - When nothing else needs the sampled pre-pass, Cavity turns it on (see Performance).
  - **No pixel may read undefined normals.** The built-in depth shader and every library depth
    shader get a normals output under a define. Meshes whose depth program can't write normals use
    normals rebuilt from depth (as `ssao.frag:59-75` does), or are masked to neutral. That covers
    users' forks of `depth.frag` and terrain or voxel programs whose normal is computed in the
    fragment shader. The builder picks the mechanism. Old projects with forked depth shaders keep
    working unchanged.
  - With Cavity off, the pre-pass is unchanged.
- **R3. Screen mode.**
  - Four taps (up, down, left, right) at a distance of `Line Width × (target height / 1080)` px.
    At most 1 px at the minimum, so lines look the same at any resolution.
  - Curvature is the sum of the central differences of the view-space normal's x (horizontal taps)
    and y (vertical taps). Positive is a ridge.
  - It is shaped by a soft clamp, so Ridge and Valley set the ceiling and floor.
  - It is zero across a depth discontinuity between opposite taps. Use a relative threshold on
    linear depth: we have no object ID, so this replaces Blender's object-ID mask.
  - Screen-up must be "up" on every backend, so ridges and valleys never swap on D3D11 or Metal.
- **R4. World mode.**
  - N depth taps (4, 8 or 16) on a deterministic, rotated disk kernel. The kernel's mean radius
    must not depend on N.
  - The radius is a world-space length projected to pixels. It is capped at
    `Max Pixel Radius × (target height / 1080)`.
  - Per tap, the Alchemy term, with a small distance-proportional bias. Taps above the surface
    plane add to valley; taps below add to ridge.
  - A background tap is treated as a point `Radius` behind the centre depth, so silhouettes get a
    ridge.
  - Taps under one pixel fade out, and normalise by the summed tap weights.
  - The whole term fades to neutral as the projected radius drops under about 1 px, so distant
    objects never go noisy or black.
  - It works with perspective and orthographic cameras.
- **R5. Combination.** `factor = clamp((1 − valley)(1 + ridge)(1 + curvature), 0, 2)`.
  - Background pixels are neutral.
  - The factor is defined on linear light. The apply converts it so the result on the display-encoded
    target is visibly a multiply of the linear colour; the builder picks the conversion and the
    goldens pin it.
  - Blender caps the factor at 4×; ours caps at 2×. Record this as a deviation.
- **R6. Pre-pass misses.** A stencil-tested submesh is missing from the pre-pass, and must not
  get the cavity of the surface behind it. Either it gets a pre-pass draw when Cavity is on, or
  the apply draw masks it. Builder's choice; a test pins it. A forked colour shader without a depth
  fork keeps SSAO's existing limitation (pre-pass geometry from the built-in depth shader). It's
  documented, not fixed.
- **R7. Cameras.** The main camera, as for the other post settings. The Scene view shows it when
  "Show post-processing" is on, the existing toggle (`SceneDisplaySettings.h:28`).
- **R8. Debug view.** A `cavityDebug` field, following SSAO's `ssaoDebug` pattern, replaces the
  scene colour with the factor as grey (0.5 neutral). It's a settings row, so it gets an Inspector
  checkbox and an MCP key for free. No new isolate button or debug tool.

### Settings (`PostProcessingSettings` rows, all optional with defaults, no version bump)

| Field | Snake key | Type | Default | Range | Inspector label |
| --- | --- | --- | --- | --- | --- |
| `cavityEnabled` | `cavity_enabled` | Bool | false | | Cavity (group checkbox) |
| `cavityType` | `cavity_type` | Enum Screen/World/Both | Both | | Type |
| `cavityScreenRidge` | `cavity_screen_ridge` | Float | 1.0 | 0–2 | Screen › Ridge |
| `cavityScreenValley` | `cavity_screen_valley` | Float | 1.0 | 0–2 | Screen › Valley |
| `cavityScreenWidth` | `cavity_screen_width` | Float | 1.0 | 1–4 | Screen › Line Width |
| `cavityWorldRadius` | `cavity_world_radius` | Float | 0.25 | 0.01–5 | World › Radius |
| `cavityWorldRidge` | `cavity_world_ridge` | Float | 1.0 | 0–2.5 | World › Ridge |
| `cavityWorldValley` | `cavity_world_valley` | Float | 1.0 | 0–2.5 | World › Valley |
| `cavityWorldSamples` | `cavity_world_samples` | Enum 4/8/16 | 8 | | World › Samples |
| `cavityMaxPixelRadius` | `cavity_max_pixel_radius` | Float | 24 | 0–64 (0 = no cap) | World › Max Pixel Radius |
| `cavityDebug` | `cavity_debug` | Bool | false | | Debug View |

- **Attenuation is a fixed constant, not a setting.** At game-scale radii, Cavitoon and Blender's
  attenuation knob barely changes the result.
- **Help text says what the user sees.** For example, "brightens convex edges", "darkens creases",
  "world-space width of the soft bevel", and "caps how wide the bevel gets on screen when the
  camera is close".

### Platform & backends

- GL 4.1, GLES3/WebGL2, Metal, D3D11 and Vulkan. It needs no compute, SSBOs, float targets or
  integer textures. The extra pre-pass attachment is RGBA8 MRT.
- Every export target. The shaders and normals depth variants go in the export variant list when a
  scene enables Cavity, or when a project script names `cavityEnabled` (then both depth forms and
  the scene's type), and every type when one names `cavityType`. A script that sets other
  post-processing ships none of them. That follows the existing named-field handling of
  `scriptsSetPostProcessing` (`ShaderVariantCollector::plausibleUberFeatures`).

### Performance (budgets)

| State | Cost |
| --- | --- |
| Off | Nothing. No variant is compiled into exports either. |
| Screen | 1 full-screen draw, about 9 fetches per pixel, plus the pre-pass normals attachment when SSR is off. When nothing else needs the sampled pre-pass, Cavity adds that whole geometry pass (every opaque mesh drawn once more for depth and normals), as URP's DepthNormals pre-pass does for the Unity packages. |
| World | 1 full-screen draw, about 3 + N fetches per pixel. |
| Both | One draw, sharing the centre fetches. |

Device-tier cell `DeviceOverride::Cavity` (appended):
- High: as authored.
- Medium: World samples capped at 8.
- Low: World samples capped at 4. Screen is unchanged.

The tier never turns Cavity off, because it's art direction, not a quality extra.

### Editor UX (human)

- Inspector › Post-processing › Stylised › **Cavity**, beside Edge Detect.
- The group checkbox enables it.
- Type is a dropdown.
- The rows are a flat list, as in the existing groups, with mode-prefixed labels: Screen Ridge,
  Screen Valley, Line Width, World Radius, World Ridge, World Valley, Samples, Max Pixel Radius,
  Debug View. Sub-headers and conditional rows don't exist in the group table, and adding them is
  out of scope.
- Tooltips use the help text plus the cost tier, as the other groups do.
- Undo comes from the existing `ScenePropertyCmd` path. A slider drag is one step.
- The Debug View checkbox shows Cavity alone (R8).

### Agent UX (MCP, Lua)

- **MCP keys:** `set_scene_settings` / `get_scene_settings` get the `post_processing.cavity_*`
  keys from the field table. No new tool.
- **Status:** `get_post_processing_status` reports Cavity as on or off, the resolved type and
  samples after the tier, the normals source (`gbuffer` or `prepass`), and its pass and draw
  count.
- **Guide:** one line in `docs/mcp/guide/post-processing.md`. When asked for a Blender-style cavity,
  edge highlight or bevel look on 3D meshes, use `cavity_*`, not Edge Detect (luma) and not a
  custom post shader.
- **Eval:** a case in `tools/mcp/eval/cases.py` that fails without the guide line or help text.
  Prompt: "make my low-poly rocks show Blender-style edge highlights". The check is that the agent
  sets `post_processing.cavity_enabled` and doesn't fork a post shader or enable Edge Detect.
- **Lua:** a `PostProcessingSettings` property per field (`cavityEnabled`, …), and
  `docs/mcp/guide/lua-api.md`.

### Data & compatibility

- New optional fields with defaults, no version bump:
  - YAML writes only non-default values, so old scenes load byte-identical.
  - The cooked codec is keyed by hashed name, so old players skip unknown fields.
- `ReflectParityTest`'s frozen data is rewritten for the new fields.

## 6. Design constraints & decisions

- **D1. In the colour pass, not a separate post pass.**
  - It runs before fog composites and transparents, so fog covers it and glass and UI are untouched.
  - It runs on every material because it multiplies the result, not the lighting.
  - It needs no intermediate target or colour copy, because the multiply blend reads the
    destination.
  - The rejected options:
    - folding it into SSAO and `mesh.frag`, which reaches only Lit;
    - an uber feature, which is after UI, display-encoded and has no exclusion;
    - a `library:cavity` user shader, which runs too late and gives a generic UI.
- **D2. Compute and apply in one draw.** The draw reads depth and normals textures, which are not
  the attachment being written, so no cavity target is needed.
- **D3. A deterministic kernel.** We have no history buffers (S13 §2), so per-pixel noise would
  show as noise.
- **D4. Real normals, not depth-rebuilt ones.** This is the anti-goal on smooth meshes. Flat
  low-poly meshes still get flat normals from their vertex data.
- **D5. A 2× multiply cap**, from the 8-bit target and blend-source clamping (R5).
- **D6. Clean-room** (§4).
- **D7. A separate depth-and-normals pre-pass, accepted on mobile.** When nothing else needs the
  sampled pre-pass, Cavity turns it on, so every opaque mesh is drawn once more. This was decided
  with the requester on 2026-11-21. Reasons:
  - It's an opt-in art-direction effect and costs nothing off.
  - The extra pass costs about what SSAO's pre-pass costs today.
  - Low-poly stylised scenes, the target, have cheap geometry.

  The rejected option was to write normals and depth from every material shader into a second
  target of the colour pass, pause the pass after the opaque queue (as SSR does at
  `RenderSystem.cpp:19774`) and resume to apply. Why it was rejected:
  - It edits `mesh.frag`, all 32 library mesh shaders and the terrain, foliage and voxel shaders,
    each with an on/off variant.
  - Users' forks don't write the output, so their pixels would be undefined.
  - The multisampled second target averages two surfaces' normals and depths at every silhouette.
  - Pausing a pass on tile-based mobile GPUs writes the whole frame out and reads it back.
  - Every opaque pixel would write two targets.

## 7. Acceptance criteria

1. **Goldens** (glcore, then Vulkan). Each one fails with the effect off.
   - Scene: a faceted low-poly rock, a smooth-shaded sphere and a bevelled cube.
   - Screen, World and Both; perspective and orthographic; MSAA 4×; and both normals sources (SSR
     on and off).
2. **The smooth sphere**, Screen mode, SSR off: no visible triangle-edge lines.
3. **Two objects touching or overlapping**, Screen mode: no curvature line along the boundary
   between them.
4. **Disabled:**
   - pass and draw counts and pre-pass attachments are identical to `main`;
   - `ShaderZeroCostTest` covers the new variants;
   - an export of a project where no scene enables Cavity and no script names `cavityEnabled`
     contains no cavity shader, though a script sets other post-processing.
5. **Default ridge and valley** on a unit cube: the edge pixels are brighter than the faces, and
   inner corners are darker.
6. **Resolution:** the same scene at 720p and 2160p gives lines of the same apparent width, within
   golden tolerance.
7. **Sample counts:** World 4, 8 and 16 give the same apparent bevel width, with more samples
   smoother.
8. **Coverage:**
   - Toon, Unlit and a library shader each show cavity;
   - a transparent object and a UI panel over a cavity edge are unchanged;
   - a stencil-tested submesh in front of another surface shows no foreign cavity (R6);
   - a mesh with a forked `depth.frag` and a terrain show neutral or depth-rebuilt cavity, never
     garbage (R2).
   - With SSR on and a displacing library shader that makes the engine refuse G-buffer depth,
     normals still match depth (R2).
9. **MCP:**
   - the `cavity_*` keys round-trip through MCP and the scene file (`test_post_fx_settings.py`);
   - a golden case in `test_post_fx_golden.py`;
   - status fields;
   - the eval case passes with the change and fails without it.
10. **Lua:** the properties read and write; `tools/lua_api/tests/test_lua_api.py` passes.
11. **Device tiers:** Low and Medium cap the samples (`QualityResolverTest`, `PostProcessingTiersTest`).
12. **Builds:** `tools/build.sh web` and `tools/build.sh android` build, and the smoke test passes.

## 8. Dependencies & conflicts

- Shares the depth pre-pass scheduling (`sharedSceneDepth`) with SSAO, contact shadows and the
  atmosphere. When Cavity adds the normals attachment, SSAO may read it in place of depth-rebuilt
  normals. That is the builder's call and not required.
- With SSAO on, valleys darken twice. Document it, as Cavitoon does; don't compensate.
- Analytic fog is already inside each mesh's colour when Cavity multiplies it, so a heavily fogged
  far object can still show Screen lines. The builder decides whether to attenuate by fog; a
  golden pins the choice.

## 9. Risks & defaults

- **Depth precision.** Packed RGBA8 depth gives about 24 bits. The World bias and the Screen
  depth-discontinuity threshold need tuning at long view distances. Default to relative thresholds.
- **The pre-pass normals attachment**, and the whole pre-pass when nothing else needs it, adds
  vertex work and bandwidth on mobile when Cavity is on. Accepted (D4, D7). A heavy-geometry scene
  pays more. The cost-tier label in the Inspector says so.
- **Defaults** are Type Both, World samples 8 and Radius 0.25, which suit 1-unit-scale props. The
  help text names Radius as the knob to change when nothing shows.

## Deviations (implementation)

- R5: the factor caps at 2×, where Blender caps it at 4× (D5).
- R3: a depth-discontinuity mask replaces Blender's object-ID mask, because there is no ID buffer.
- Editor UX: a flat row list with prefixed labels, because the Inspector group table has no
  sub-headers.
- Agent UX: the guide line is in `docs/mcp/guide/shaders.md`'s intro as well as in
  `post-processing.md`'s Fields. Eval agents asked for edge highlights read `jasper://docs/shaders`
  and forked a shader; they never opened the post-processing doc (0 of 4 samples passed with only
  the post-processing line, 3 of 4 with the shaders line).
- R1: the apply draw tests depth (LESS_EQUAL, no write) against the colour pass's depth, the
  fragment writing the pre-pass's depth nearer by 0.75 of the depth change to its steeper
  non-background neighbour on each axis, so every multisampled sample of the surface passes on a
  sloped face and by a crease. That is the R6 mask: where the colour pass drew something nearer
  than the depth Cavity reads (a stencil-tested submesh, which no pre-pass draws), the test fails
  and the pixel stays as it is. It also keeps Cavity off what a forked
  colour shader draws away from its built-in depth program's geometry.
- R2: the pre-pass normals are world-space (the object block alone; Cavity turns them to view
  space), so a depth program takes no camera block for them: the G-buffer's depth-mismatch rule,
  which counts a library depth program reading the camera block, and the shadow passes (block
  uploads, the position-only index buffer) are as without Cavity. In a shadow pass a 'Cvn'
  program still fetches the normal and writes an output with no attachment.
- R2: a depth pre-pass is reused only at the colour pass's size; SSAO's (the view's size) is not
  at a render scale below 1 or with fixed resolution, and Cavity then draws its own.
- R2: the pre-pass normals are octahedral at 12 bits a component (rg the high bytes, b both low
  nibbles) with a written flag in alpha (cleared to 0), not the G-buffer's 16-bit split, which
  needs two attachments' worth of channels and has no room for the flag. The decode error is
  about 0.03 degrees.
- R2: every 'Cvn' depth fragment stage is wrapped at compile time (`#define main`, then a `main`
  that writes the normals attachment), so forks of `depth.frag` of any age write it. The normal
  comes from the vertex stage's `v_depthNormal` when that stage writes one (`depth.vert`,
  `includes/surface_depth_vs.glsl`, a fork made from them), else the pixel says "none". On a
  "none" pixel Screen is neutral and World rebuilds the normal from depth, so a fork, a mesh
  without normals and a library shader whose hook moves vertices (`spin_bob`, `water`,
  `foliage`, `impostor`) never read garbage and draw no triangle-edge lines. `dissolve` and
  `flipbook`, whose hooks leave the position, keep the vertex normal. The built-in depth program
  takes the rest normal of morph targets, as the G-buffer does.
- R2: the pre-pass normals wait until every loaded mesh has reloaded with its 'Cvn' depth
  programs after Cavity turns on; those frames draw no Cavity (status inactive) rather than
  reading a pre-pass with holes.
- R2: the G-buffer is the normals source when its depth draws every visible opaque mesh as the
  colour pass does (the scene-depth readers' rule, `hasVisibleGBufferDepthMismatch`), whether or
  not a scene-depth reader exists; otherwise the pre-pass, even with SSR on.
- R3: the depth-step mask is a one-surface test between the centre and each tap: each must lie
  within 2 tap widths (at that depth) over the facing (|n.z|, at least 0.15) of the other's tangent
  plane. Computed on a model of the test: a crease between faces up to 80 degrees from the view
  reaches at most 0.46 of the margin, a curved surface 0.24; a step of 0.5% of the depth is masked
  at Line Width 1 and 2, of 1% up to 4; a slanted plane is exactly 0 at any angle.
- R4: where a pixel has no normal and both neighbours of an axis are background, World is
  neutral there (no depth-rebuilt normal from the cleared depth).
- R4: the bias is 0.15 L (not about 0.05 L): a smooth-shaded mesh's facets leave the plane of
  its interpolated normal by up to half their angle, and at 0.05 L a UV sphere's triangles drew a
  grid in World mode (the anti-goal). Creases and bevels are far steeper.
- R4: per tap valley adds `max(h - bias, 0)` and ridge `max(-h - bias, 0)`, both times the
  attenuation and the tap's weight, then a fixed gain of 2 before Valley and Ridge. Ring k of n
  sits at (k + 0.5) / n of the disk, so 4, 8 and 16 samples share the mean radius (half the disk).
- R5: the apply multiplies the display-encoded colour by `factor^(1 / 2.2)`: exact for a
  power-law encoding, close under sRGB and the tone curves.
- §8 fog: Cavity fades with the analytic distance fog (linear, exponential, squared) at the
  pixel's fog distance, so a fogged far object shows none. The height and volumetric fogs are
  drawn over it after, as D1 has it.
- R2: a fork whose fragment stage has no `#version` line gets the wrapper at its top. It then
  builds, or fails, as it does with Cavity off. glslang's default is ES 100, below SPIR-V's 310,
  so such a fork fails with or without Cavity and its mesh draws the built-in depth program
  (`CavityVariants.ForkWithoutVersionBuildsAsWithoutCavity`).
- Number: the spec was S92; it is S100 because #10 keeps S92, S93 to S98 are other open specs and
  #149 takes S99.
- §5 Platform: an export ships Cavity's variants when a scene enables it or a script names
  `cavityEnabled` (`cavityType`: every type), not whenever a script sets post-processing. The broad
  rule doubled every depth program of a scene and added the three full-screen programs to any
  game whose script touched post-processing (see the PR for the counts).
