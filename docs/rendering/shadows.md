# Shadows

JasperEngine's 3D shadows: stable cascaded shadow maps for directional lights, spot and point
light shadows in depth atlases, one filter family on every light, quality tiers per platform, and
a shadow cache that redraws a shadow map only when something in it changed. The design and
acceptance record is `docs/specs/S10-shadows.md`; the agent reference is
`docs/mcp/guide/shadows.md` (`jasper://docs/shadows`).

## Lights

Inspector > Light > **Shadows** (Unity's order):

| Setting | | Lua (`Light`) | MCP property |
|---|---|---|---|
| Shadow Type | No Shadows, Hard Shadows (one bilinear comparison), Soft Shadows (the tier's filter) | `shadowType` (`ShadowType.NONE/HARD/SOFT`) | `shadows`, `softShadows` |
| Strength | 0 (no shadow) to 1 | `shadowStrength` | `shadowStrength` |
| Resolution | Use Quality Settings (0), 256 to 4096 texels per map | `shadowResolution` | `mapResolution` |
| Bias | depth offset towards the light, in **shadow texels** (default 1) | `shadowBias` | `shadowBias` |
| Normal Bias | offset along the receiver's normal, in shadow texels (default 1) | `shadowNormalBias` | `shadowNormalBias` |
| Auto Near Plane, Near / Far Plane | spot and point: the shadow camera's planes (the camera near and the light range when automatic) | `automaticShadowCamera`, `cameraNear`, `cameraFar` | `automaticShadowCamera`, `shadowCameraNear`, `shadowCameraFar` |
| Update Mode | Auto (cached), Every Frame, On Demand | `shadowUpdateMode`, `requestShadowUpdate()` | `shadowUpdateMode` |
| Shadow Distance | directional: metres; 0 takes the scene's, else the tier's | `shadowDistance` | `shadowDistance` |
| Cascade Count | directional: Use Quality Settings (0) or 1 to 4 (the tier caps it) | `numCascades` | `numShadowCascades` |
| Split Mode, split bar | Auto (log/uniform mix) or Manual (`cascadeSplits`, fractions of the distance; drag the bar's handles) | `cascadeSplitMode`, `cascadeSplits` | `cascadeSplitMode`, `cascadeSplits` |
| Angular Diameter | directional: degrees of the light's disc (the sun: 0.53) | `shadowAngularDiameter` | `shadowAngularDiameter` |
| Source Radius | spot and point: metres (default 0.05) | `shadowSourceRadius` | `shadowSourceRadius` |

Biases are in texels, so one value works at every resolution and in every cascade: the depth
offset is `Bias × texel size`, the normal offset `Normal Bias × texel size × (1 − n·l)`. The Low
tier has no receiver-plane bias; there the receiver also moves towards the light by the depth its
slope spans under the 3×3 filter (`2 × texel size × tan` of its angle to the light, at most 16
texels), which keeps grazing surfaces free of acne. A spot or
point light's texel grows with the distance from it, so are its offsets. From the Medium tier on,
the receiver-plane bias compares every filter tap against the plane through the receiver, so a
slope does not shadow itself; the default biases need no tuning.

Mesh Inspector > Lighting: **Cast Shadows**, **Receive Shadows** and **Static Shadow Caster**
(Lua `Mesh.staticShadowCaster`, MCP `staticShadowCaster`).

## Scene and project

Inspector > Scene > **Shadows**:

- **Quality**: Auto (the project's tier of the platform), Hard Only (that tier, every light hard),
  Low, Medium, High, Ultra. Lua `scene.shadowQuality` (`ShadowQuality.AUTO`, `NONE`, `LOW`,
  `MEDIUM`, `HIGH`, `ULTRA`), MCP `shadows_quality`.
- **Shadow Distance** (0: the tier's), **Shadow Fade** (the last fraction of the distance over
  which directional shadows fade to lit, default 0.1), **Cascade Blend** (on: cascades blend into
  each other over a band before each split, dithered on Low). Lua `shadowDistance`,
  `shadowFadeFraction`, `shadowCascadeBlend`; MCP `shadow_distance`, `shadow_fade_fraction`,
  `shadow_cascade_blend`.
- **Effective Tier**: what the scene renders with here. Lua `scene.effectiveShadowTier`.

Project Settings > Graphics > **Shadows**: the tier of each platform (project.yaml
`graphics.shadowTier.<platform>`, MCP `graphics.shadow_tier.<platform>`). The editor uses the tier
of the platform it runs on; set the scene's Quality to preview another tier.

## Quality tiers

| Tier (default of) | Low (Android, iOS) | Medium (Web) | High (Windows, Linux, macOS) | Ultra (opt-in) |
|---|---|---|---|---|
| Cascades at most | 2 | 3 | 4 | 4 |
| Map resolution at most (directional / spot / point face at Use Quality) | 1024 (1024 / 512 / 256) | 2048 (2048 / 1024 / 512) | 2048 (2048 / 1024 / 512) | 4096 (4096 / 1024 / 512) |
| Shadow distance | 40 m | 60 m | 100 m | 150 m |
| Soft filter | 3×3 grid (4 fetches) | 5×5 grid (9 fetches) | 16-tap Vogel disk | PCSS: 16-tap blocker search, 16 to 32-tap Vogel PCF |
| Cascade transition | dither | blend | blend | blend |
| Receiver-plane bias | off | on | on | on |
| Cascade fit freeze (caching) | on | on | on | on |
| Memory (atlases and static caches) | 16 MB | 48 MB | 192 MB | 576 MB |

The memory budget is kept: when the maps of a scene would exceed it, the static layers of the
least important lights go first, then the least important maps are halved (a directional light's
farthest cascade first). `get_shadow_stats` reports what each light got. The measured costs are
in `docs/rendering/shadow-budgets.md`. High and Ultra freeze their cascade fits and have room for
the static caches since the GPU campaign: without them a moving camera with moving casters redrew
every static caster each frame (7.2 ms of shadow passes at High on an RTX 3080 Ti; 0.18 ms now).

GLES3 and WebGL2 have 16 texture units and render the Ultra tier's soft shadows with the High
tier's Vogel disk (no PCSS); so does a painted terrain everywhere. Point lights never take PCSS
(their filter would cross cube faces): they use the Vogel disk at High and Ultra.

**Shader variants.** The High and Ultra filters (the Vogel disk, PCSS and the raw-depth view of
the atlas PCSS reads) live only in the mesh variants with the `Shq` bit (`USE_SHADOWS_HQ`); Low and
Medium variants carry the grid filters alone, so mobile and web shaders stay small. When the
effective tier crosses into or out of High, every lit mesh switches variant (each keeps drawing
its old one until the new one is built). Scenes of Quality Auto, and scenes whose scripts set
`shadowQuality`, ship both; an export then cooks the `Shq` variants only for the shader backends of
platforms whose tier is High or above (GLES3 ships to Android and the web, Vulkan to Android,
Linux and Windows, OpenGL to the desktops, Metal and Direct3D to theirs). A game without the
runtime shader compiler that is asked for a High tier its backend was not cooked for renders
Medium and logs one warning; with the runtime compiler (Project Settings > Player, on by default)
it compiles the variants it lacks.

## How it works

**Cascades.** Each cascade is the bounding sphere of its slice of the camera frustum, seen from
a rotation-only light basis (up is world Y, or Z when the light is near vertical). The sphere's
centre is snapped to the shadow texel grid and its radius quantised, so a camera that turns in
place or moves by less than a texel keeps the exact same shadow matrices and shadow edges do not
crawl. The near plane is pulled towards the light up to the shadow casters, so off-screen casters
still cast. Each slice starts 17.5 % of the previous cascade's depth before its split, the band
where the receiver mixes both; past the shadow distance nothing is shadowed, and the last
`Shadow Fade` of the distance fades to lit.

**Atlases.** Directional cascades and spot lights share the projective atlas, point lights' cube
faces the point atlas; both are depth-only D32 comparison atlases. The slot planner gives slots
to the most important lights (directional lights first, then intensity × screen coverage /
(1 + distance)); a light keeps its slots until a challenger beats it by 25 % for 30 frames. Every
map has its own resolution, packed without overlap. Point faces are rendered with a guard band
wide enough for the widest kernel, so no tap leaves a face and cube edges show no seam.

**Shadow cache.** Each slot remembers what it holds: its shadow camera, rect, the layers drawn
into it, the light's revision. Every update the caster registry reports the casters that moved,
appeared, disappeared or changed (a transform, a mesh reload, a buffer, an alpha-clip texture, a
layer, the static flag, an animation). A slot whose content is unchanged is not drawn; one whose
dynamic casters alone changed gets its static casters' depths copied back from the static cache
inside the rect those casters covered (before and after) and only them redrawn; one whose static
casters changed redraws its static layer. Casters drawn per camera (a terrain's cut, turned
billboards, culled instances) keep their slots uncached. With a still scene and camera the shadow
passes are skipped entirely. Casters are culled against each slot's light frustum only, not against
the camera's receivers: a cached slot outlives the view it was drawn for, and the frame's atlas is
also read by reflection probes, render-to-texture cameras and other views.

Invalidation: a light's slots are redrawn when its type, direction, position, range, cone, near
plane, resolution, cascades, splits, shadow distance, culling mask, update mode or enable change,
when the atlas is recreated, and on `requestShadowUpdate()`. Receiver settings (biases, strength,
filter, fade, the tier's filter) never redraw a map.

**Update modes.** Auto: the cache. Every Frame: always redrawn whole. On Demand: drawn when the
light is enabled or a setting above changes, and on `light:requestShadowUpdate()` (or the
Inspector's Request Update); moving casters wait for it (Unity HDRP's semantics).

## Debug views

Scene view > display settings > Rendering > **Shadow debug** (MCP `shadows_debug_view`, editor
only, never saved): Cascades (tinted per cascade, the blend band hatched), Shadow Factor (the
main light's lit fraction as grey), Texel Density (a checker of its shadow texels), Cache (green
skipped, yellow restored, red redrawn this frame), Atlas (the atlases and static caches over the
view's corner). The views replace the built-in Lit's shading; library shaders draw normally.

## Shader interface

Lit and the shader library call `lightShadow(index, worldPos, worldNormal)` (`includes/lights.glsl`,
in uniform control flow: it takes the position's derivatives for the receiver-plane bias). A shader
that looks the shadow up inside a per-fragment branch (Lit only for surfaces facing the light) takes
`dFdx`/`dFdy` of the position before the branch and calls `lightShadowD(index, worldPos,
worldNormal, dpdx, dpdy)`. Every shadow atlas lookup, the 2D lights' too, takes explicit LOD 0, so
the PCF loops have the same radius on every backend (2D shadows: up to 13 taps at High, D3D11
included); `tools/check_shader_lod.py` fails on an implicit-derivative call in a loop with a
per-fragment exit. The
shadow coordinates are computed in the fragment stage from the blocks of
`includes/shadow_data.glsl`; no vertex varyings carry them.

A pass of its own (volumetric fog, clouds) samples the main directional light's shadow with:

```glsl
#include "includes/shadow_sampling.glsl"   // declares u_fs_shadow_data, u_shadowAtlas

float visibility = sampleDirectionalShadow1(worldPos);   // 1 lit .. 0 shadowed, one tap
```

The engine fills `u_fs_shadow_data` and binds `u_shadowAtlas` (comparison sampler
`u_shadowAtlas_smp`) for any shader that declares them, per camera pass. The block, member for
member (`fs_shadow_data_t` in `RenderSystem.h` must match it):

| Member | Meaning |
|---|---|
| `vec4 header` | x first slot of the main directional light (-1: none), y its cascades, z flags (1 receiver plane, 2 dithered transition, 4 blended transition, 8 PCSS), w debug view |
| `vec4 fitCameraPosition`, `fitCameraForward` | the camera the cascades are fitted to: view depth is `dot(forward, p − position)` |
| `mat4 lightVP[MAX_SHADOW_ATLAS_SLOTS]` | world to the slot's clip space, as the slot was rendered |
| `vec4 atlasRect[]` | xy offset, zw scale in atlas UV (bottom-left origin; `shadowAtlasCoord` flips it for non-GL) |
| `vec4 bias[]` | directional: x depth bias (world, along the light), y normal offset, z texel size, w ortho depth span; spot: per metre of distance from the light, w far plane |
| `vec4 split[]` | directional: x near, y far view depth of the cascade, z view depth where the blend into the next starts; spot: near, far; w the slot's resolution |
| `vec4 filtering[]` | x filter (0 hard, 1 grid, 2 Vogel, 3 PCSS), y radius in texels, z tan(half angular diameter) or source radius, w strength |
| `vec4 fade[]` | x, y the directional fade's start and end view depth; z the slot's cache path this frame (debug) |

Depths are window depths in [0, 1], nearer the light smaller; the comparison sampler returns the
lit fraction. Declare only what you read (an unread block or texture breaks the bind slots on
GL): `SHADOW_DATA_POINT` adds the point light block and atlas, `SHADOW_DATA_RAW` the raw-depth
view (`u_shadowAtlasRaw`, never on GLES), and `SHADOW_SAMPLING_HQ` the Vogel and PCSS filters
(`includes/lights.glsl` defines it in `Shq` variants; without it a soft light takes the 5×5 grid). The array sizes come from the defines the engine gives
mesh shaders (`MAX_SHADOW_ATLAS_SLOTS`, `MAX_SHADOWCASCADES`, `MAX_POINT_SHADOW_ATLAS_SLOTS`,
`MAX_POINT_SHADOW_LIGHTS`, `SHADOW_CUBE_FACES`); a pass of another shader type defines them too.

**Older forks.** A fork of `mesh.vert`/`mesh.frag` written before these shadows declares the legacy
`u_vs_shadows`, `u_fs_shadows`, `u_fs_point_shadows` and `v_lightProjPos[]` and calls
`shadowCalculationPCF` / `shadowCascadedCalculationPCF` (`includes/shadows.glsl`): it keeps
working for directional and spot lights (the engine still fills those blocks), its point lights
read as lit and the Console warns once per shader. `check_shader` reports it; re-fork the
shader to get point shadows, the cascade blend and PCSS.

## Old scenes

Scenes saved before these shadows (scene format 1 and earlier) migrate on load (S03's
migration table, scene 1 → 2; prefab 3 → 4): a light's Bias was in depth units and is reset to
1 texel (logged once per document), the old default Quality Low becomes Auto, and the scene's
cached mesh variant keys lose bit 5 (the retired PCF bit, `Shq` now). Shadow
distances become finite: the tier's, as in Unity. Set `shadowDistance` per light or per scene to
keep longer shadows.
