# Reflections

How JasperEngine reflects the world on 3D surfaces: the environment (the sky), reflection probes
and screen-space reflections (SSR), and how they combine. The design record is
`docs/superpowers/specs/2026-11-04-ssr-probes-design.md`; the agent reference is
`docs/mcp/guide/reflections.md` (`jasper://docs/reflections`).

## The three sources

| Source | What it reflects | Where it comes from |
|---|---|---|
| Environment | the sky, prefiltered by roughness | the scene's Skybox (its maps are the scene's IBL) |
| Reflection Probe | a cubemap of the scene around a point, box-projected inside its volume | a Reflection Probe component: an authored cubemap, or a capture (Static: once; Dynamic: on move or on an interval) |
| Screen Space Reflections | what is on screen, exactly where the reflected ray meets it | Inspector > Scene > Reflections (SSR), the main camera only |

The built-in Lit material reflects the environment, or the probe whose box holds the mesh (the
highest priority, then the nearest; blended over the sky by the probe's Blend Distance). Without a
sky and a probe it reflects the scene's ambient colour (Global Illumination).

## Screen Space Reflections

Inspector > Scene > **Reflections (SSR)**:

| Setting | | Lua (`Scene`) | MCP (`set_scene_settings`) |
|---|---|---|---|
| Enabled | on/off | `ssrEnabled` | `ssr_enabled` |
| Max Distance | metres a reflected ray travels | `ssrMaxDistance` | `ssr_max_distance` |
| Thickness | how thick the depth buffer counts as (metres) | `ssrThickness` | `ssr_thickness` |
| Intensity | 1 is physical; above 1 strengthens the reflection | `ssrIntensity` | `ssr_intensity` |
| Glossy Blur | 0 sharp, up to 1 for rough surfaces | `ssrBlur` | `ssr_blur` |
| Debug View | see below | `ssrDebugMode` | `ssr_debug_mode` |

**SSR replaces the reflection a surface already has, it never adds to it** (Unity HDRP's model).
Where SSR finds what the reflected ray hits, it takes the place of the environment, probe or
ambient reflection by its confidence; where it misses (off screen, behind something) that
reflection stays. So a polished floor in a room with a reflection probe shows the probe's capture
everywhere and the pillars' exact reflection where SSR sees them, never both on top of each other.

The swap is weighed as the surface's own reflection is:

- by its **ambient occlusion**: the material's occlusion map (on the UV set the material names,
  through Stochastic Tiling when the material uses it), a painted terrain's occlusion, and SSAO.
  A crevice that is dark without SSR stays dark with it;
- by the **fog** in front of it (distance fog, or the height fog): a reflection far away fades with
  the surface into the fog.

Surfaces that get no SSR:

- unlit materials (Lighting None, or the mesh does not receive lights), as Unity's Unlit;
- planar mirrors (a Mirror's reflection already is the reflection);
- rough surfaces (roughness 0.6 and above; SSR fades out from 0.3).

Shader-library shaders with lighting, and the built-in Lit drawn in a scene with neither lights
nor a sky, have no environment reflection the composite can rebuild: SSR is added to them.

### Reflection probes and SSR: four per view

The SSR composite rebuilds the probe reflection of each pixel from a slot that holds the probe's
cubemap. A view has **four probe slots**. They go to the probes of the meshes drawn this frame
(culled meshes, transparent ones and those without IBL do not count), the ones covering the most
screen first; a probe that held a slot keeps it unless another covers clearly more (1.25x), so
SSR does not flicker between two probes of a similar size.

A surface under a fifth probe keeps its probe reflection **without SSR** (never a doubled
reflection). The Console says it once per scene:
`SSR: 6 reflection probes in view, 4 have SSR slots; surfaces under 'Probe Attic', 'Probe Stair'
keep their probe reflection without SSR`, and `get_render_stats` lists the probes and their slots.
Fewer, larger probes per view avoid it.

### Debug views

Debug View (`ssr_debug_mode`):

| Value | View | Shows |
|---|---|---|
| 0 | Off | the frame |
| 1 | Reflection | the SSR reflection times its confidence |
| 2 | Normal | the G-buffer's view-space normal |
| 3 | Roughness | the G-buffer's roughness |
| 4 | Metallic | the G-buffer's metallic |
| 5 | Albedo | the G-buffer's base colour |
| 6 | Environment Specular | the reflection SSR swaps out (sky, probe or ambient), rebuilt per pixel |
| 7 | Reflection Source | what SSR swaps against, by colour (below) |
| 8 | Occlusion | the occlusion the swap is weighed by (occlusion map x terrain x SSAO) |

Reflection Source colours: **blue** the sky, **red, green, yellow, cyan** a probe in slot 0, 1,
2, 3 (darker where the probe blends into the sky at its box edge), **orange** a probe without a
slot (no SSR there), **grey** the scene ambient, **magenta** SSR added only, **black** no SSR.

### What is approximate

The swap rebuilds the reflection from the G-buffer, which keeps less than the colour pass. These
leave a small difference where SSR is confident:

- normal maps: the swap uses the vertex normal, so the normal map's detail in the environment
  reflection stays faintly under a sharp SSR reflection;
- hex tiling of the occlusion map, Lit Quality's specular anti-aliasing, and the extended lobes
  (anisotropy bends the reflection; specular and IOR change its strength); clearcoat keeps its own
  environment reflection, which is right;
- the G-buffer keeps the normal in 8 bits per component, so the rebuilt reflection can turn a fraction of a
  degree: where the probe's capture has a sharp edge in that direction (a bright object against a dark wall),
  a thin line of the difference shows under a confident SSR hit;
- the probe blend weight is stored in 62 steps;
- the volumetric fog's marched part is approximated by the analytic height fog over the whole ray;
- with post-processing's exposure or tone curve on, the frame is already display-encoded when SSR
  composites it, so the swap is applied to that colour.

Transparent objects, near-plane videos and HTML UI drawn by the main camera are in the picture SSR
reflects and receive the swap of the surface behind them.

## Cost

SSR off costs nothing. On, it draws a G-buffer pass of the opaque meshes (four targets: depth,
normal/roughness/metallic, albedo and reflection source, occlusion: 13 bytes a pixel, plus its
depth buffer), the
reflection march, an optional blur, and the composite. Pixels SSR does not reflect on (most of a
frame) cost the composite two texture reads; the march skips surfaces that get no SSR.

## Changes in 2026-11

Scenes look different, correctly, where:

- a reflection probe and SSR met: the doubled reflection is gone;
- an occlusion map or SSAO darkened a reflective surface: SSR no longer brightens it;
- fog covered a reflective surface: its reflection fades with it;
- an unlit material or a planar mirror was under SSR: it no longer reflects twice;
- a material's base colour or metallic-roughness map is on the second UV set: SSR now sees it there.
