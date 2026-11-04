# S57: Lighting techniques after The Forge

Wave 5. Research: `docs/forge-meshopt-analysis/forge-rendering.md` (§1, §4-6, §8). Sony Bend's
screen-space shadow sample (Apache-2.0) and The Forge (Apache-2.0) are references. Every technique
here is fragment-shader or CPU work.

## 1. Goal

Five lighting features take forward rendering on phones and browsers closer to high-end engines:

- Screen-tile light lists remove the per-object light cap and light popping on large meshes.
- Analytic capsule shadows let characters cast soft shadows without invalidating the static
  shadow cache.
- Contact shadows add small-scale detail that shadow maps miss.
- Weighted blended order-independent transparency removes sorting artefacts for layered glass,
  foliage cards and particles.
- A Kajiya-Kay hair lobe makes hair cards read as hair.

Each is opt-in or tiered, and costs nothing when off.

## 2. Scope

In scope: the five features, their editor, Lua and MCP surfaces, goldens and budgets.

Out of scope:
- The visibility buffer, ASM, SDF shadows, VSM/MSM/ESM and AOIT (`forge-rendering.md` §3, §5).
- Hi-Z SSR: a later spec.

## 3. Current state

- **Lights:** per-object selection with 4, 8 or 16 slots (default 8); the rest fold into SH
  (`Engine.h:28`, `SceneSettings.h:370`, `RenderSystem.cpp:2016-2035`, `lights.glsl`).
- **Shadows:** S10 has stable CSM, PCF, PCSS and a static cache. Contact shadows were deferred to
  a separate spec (`S10-shadows.md:40`).
- **Transparency:** per-object distance sort (`transparentQueue`, `RenderSystem.h:573-651`), and
  no OIT.
- **Hair:** no hair or anisotropic hair lobe. S11 has the PBR lobes.

## 4. Reference

- **Light lists:** VB1 `LightClusters.comp` and `VisibilityBufferShade.frag:296-310`, rewritten
  as a CPU build.
- **Capsules:** UT28 `plane.frag:41-70` and `28_Skinning.cpp:868-882`. Re-derive the closest-
  approach math (the Forge comment credits a Shadertoy).
- **Contact shadows:** port from Sony Bend's `bend_sss_gpu.h` (in
  `/tmp/forge-meshopt-research/bend-dl/x/` when present, else github.com/sony/bend-sss), not from
  The Forge's copy.
- **WBOIT:** McGuire and Bavoil, JCGT 2013. Use the single-blend-state form in
  `forge-rendering.md` §4, because sokol has no per-attachment blend on GL/GLES3.
- **Hair:** UT06 `HairShortCutFillColor.frag:56-83`.

## 5. Requirements

### R1. Screen-tile light lists (Forward+)

- **Build:**
  - Each camera's screen gets a tile grid (default 16x9, or 32x18 at High) built on the CPU each
    frame by a job.
  - Each point or spot light's bounding sphere projects to a screen rect, which writes the
    light's index into the covered tiles' lists.
  - The cap is 32 lights per tile and 256 per view; past it the nearest win.
- **Data:**
  - An RGBA8 or R16UI index texture per view holds the tile header (offset, count) and indices,
    read with `texelFetch`.
  - Light data lives in a texture (RGBA32F, as bones do) or a UBO within sokol's limits.
- **Shader:** `lights.glsl` keeps `pixelLightCount()` and `getLight(i, ...)` and becomes indirect
  through the tile, so library and user shaders get it free. Keep the D3D11 loop-unroll rule.
- **Project setting:** Rendering > Light Mode with Per Object (today) and Tiled (Forward+),
  default Tiled on Desktop and High tiers, Per Object on Low (S56).
- **Zero cost** when Per Object: no texture and no job.
- **Golden:** a scene with 64 lights on one large plane renders every light (red: Per Object
  drops them).

### R2. Capsule shadows

- **Component:** `CapsuleShadowCaster`, a list of up to 8 capsules (bone, radius, length, offset)
  with Fit To Skeleton, which fits named humanoid bones as The Forge does.
- **Shader:** a `USE_CAPSULE_SHADOWS` lit variant applies the sun's (or the main directional
  light's) soft occlusion per receiver pixel, as an analytic closest approach with a smoothstep
  by softness.
- **Settings:** per-scene enable, intensity and softness, with up to 4 casters near the camera
  (the nearest).
- **The static shadow cache stays valid:** a character with capsules can opt out of the shadow
  map (Cast Shadows: Capsules Only).
- **Editor:** capsule gizmos, editable in the Scene view with undo.

### R3. Contact shadows

- **Pass:** a fragment pass after the depth pre-pass marches 8-16 samples toward the main light
  in screen space, with blue-noise jitter. Full resolution on High, half on Medium, off on Low.
  It outputs an R8 mask that the opaque pass multiplies into the main light's shadow term.
- **Dependencies:** it needs the depth pre-pass and turns it on when enabled. The Inspector and
  docs state the cost.
- **Setting:** per-light Contact Shadows (directional and spot) with Length (world units),
  Thickness and Sample Count. The Unity HDRP-like names are acceptable.
- **Zero** when no light enables it.

### R4. Weighted blended OIT

- **Opt-in:** per camera, Transparency Mode with Sorted (today) or Order Independent.
- **Passes:**
  - Accumulation: RGBA16F (colour times alpha times weight, revealage in alpha) and R16F (the
    weight), both with blend ONE,ONE / ZERO,ONE_MINUS_SRC_ALPHA, with opaque depth loaded and
    no depth write.
  - Resolve: a fullscreen pass over the scene.
- **Fallback:** without renderable half-float formats (`sg_query_pixelformat`), fall back to
  Sorted with a one-time warning.
- **Scope:** only alpha-blended materials. Additive and multiply stay sorted. Library shaders get
  the variant.
- **Weight:** McGuire's depth weight, with a per-camera scale.

### R5. Hair lobe

- **Material option:** Hair, in the Lit material's Shading Model list, uses Kajiya-Kay with two
  shifted specular lobes along the tangent (shift and width per lobe, primary and secondary
  colour).
- **Inputs:** a tangent from the mesh, or a flow map.
- **Use:** with alpha-clip or OIT for hair cards.
- **Golden:** a hair card sphere.

### Cross-cutting

- **R6. Surfaces:**
  - **Editor:** each setting appears where Unity users expect (Light Inspector, Camera Inspector,
    Material Inspector, Project Settings > Rendering), undoable, with tooltips stating cost.
  - **Lua:** properties on Light, Camera, Material and the new component.
  - **MCP:** existing set_property and settings tools cover the fields. `get_render_stats` adds
    tile stats (lights per tile max and average) and the OIT and contact pass timings via S15's
    pass list. Document in `docs/mcp/guide/rendering.md`, or the closest existing page.
- **R7. Budgets:** state and measure GPU ms on the linux build for each feature on the bench
  scenes. Default tiers keep Low within today's cost.
- **R8. Backends:** every feature compiles and renders on glcore and Vulkan (goldens). It also
  builds for web and android, and the GLSL stays within the cross-compile subset.

## 6. Design constraints and decisions

- **No compute, SSBOs or atomics:** CPU builds and data textures only.
- **Variants** are created through S32's async path; a feature whose variant isn't ready draws
  without it for that frame.
- **Old scenes** default every new field to today's behaviour, except Light Mode Tiled on desktop:
  it lights the same scene identically when the scene has no more lights per object than the
  cap. A golden checks that.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 | Light-list golden (64 lights), a `TileBuildTest` with CPU projection cases, and the default-scene golden unchanged. |
| AC2 | Capsule golden, a `CapsuleFitTest`, and the static cache staying valid while a capsule character moves (`ShadowCacheTest` extended). |
| AC3 | Contact shadow golden at High and Medium; zero passes when off (pass list test). |
| AC4 | OIT golden (three overlapping coloured planes in both orders give the same image), and the fallback test. |
| AC5 | Hair golden. |
| AC6 | MCP tests for the new fields and stats; screenshots from an isolated editor. |
| AC7 | Web and android builds pass. |

## 8. Dependencies and conflicts

- **Doriax catch-up overlap:**
  - `render-correctness` adds a lint against implicit-derivative texture reads in non-uniform
    control flow in light loops. S57's tiled `getLight` loop must pass it.
  - `draw-perf` adds a directional-only Lit variant (`Dlo`). Tiled mode applies only when
    point or spot lights exist.

- **Depends on** S10, S11, S15, S32 and S56 (tier defaults).
- **Hotspots:** `lights.glsl`, the lit shader includes, `RenderSystem.cpp` and the camera and
  light components. S54 and S55 touch the same files.
