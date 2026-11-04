# The Forge as reference for JasperEngine: rendering techniques and effects

Clone: `/tmp/forge-meshopt-research/forge/The-Forge`, commit `cd50468` (`F/` below).
JasperEngine repo: `/var/lib/orquester/workspaces/jaspersito/forge-meshopt` (`J/` below).

**Labels.** [V] = verified, I read the file (file:line given). [I] = inferred: from reading code,
from API or spec knowledge not checked here, or an estimate. All costs and GPU numbers are
unmeasured estimates ([I]). Nothing was built or run from the clone.

**Outside the clone.** To settle two provenance questions I fetched Sony's Bend sample
(`/tmp/forge-meshopt-research/bend-dl/x/`) and AMD's AOFX shaders
(`/tmp/forge-meshopt-research/aofx-dl/`). I read them as text only.

**Verdict words.** PORTABLE-AS-IS, PORTABLE-WITH-CHANGES, DESKTOP-FAST-PATH-ONLY, OUT.

---

## 1. Bottom line

| Rank | Technique | Forge source | Gap in J? | Verdict | Cost [I] |
|---|---|---|---|---|---|
| 1 | Screen-tile light lists, built on the CPU, read from a texture | VB1 `LightClusters.comp`, `VisibilityBufferShade.frag:296-310` | Yes | PORTABLE-WITH-CHANGES | M |
| 2 | Analytic capsule soft shadows for skinned characters | UT28 `plane.frag:41-70`, `28_Skinning.cpp:868-882` | Yes | PORTABLE-AS-IS (reimplement, licence) | S |
| 3 | Weighted blended OIT, single-blend-state form | UT06 hair resolve math; the algorithm is McGuire and Bavoil's | Yes | PORTABLE-WITH-CHANGES | M |
| 4 | Screen-space contact shadows | UT09 `ScreenSpaceShadowsGenerate.comp` (really Sony Bend code) | Yes, S10 deferred it | PORTABLE-WITH-CHANGES | M |
| 5 | Kajiya-Kay hair lobe for hair cards | UT06 `HairShortCutFillColor.frag:56-83` | Yes | PORTABLE-AS-IS | S |

Runners-up:
- Hi-Z SSR trace, from FFX SSSR's `HierarchicalRaymarch` (M).
- Stateless vertex-shader ambient particles, from UT03 (S).
- CPU triangle or cluster filter, port of `FilterTriangle` (M-L; measure first).
- Adjacent-index flocking kernel for S20 (S).

**Visibility buffer on GLES3/WebGL2: buildable, not meaningful.** OUT. See §3.

**The main limit of this source.** The Forge has no GL path at all. It dropped GLES2 and D3D11
(`F/README.md:219,263`) and targets D3D12, Vulkan 1.1, Metal and consoles (`F/README.md:3-16`).
Its shaders are FSL, a superset of HLSL (`F/README.md:53`). Nothing here was ever tested on
GL 3.3, GLES3, WebGL2 or D3D11 in this form, so every port is a hand translation to our GLSL 450
with its own golden images.

---

## 2. Surprises

1. **The README describes code that is not in the clone.** [V]
   - `F/Examples_3/Unit_Tests/src` holds 17 tests: 01, 03, 06, 09, 10, 11, 11a, 12, 14, 15a, 16, 19, 21, 28, 34, 36, 39.
   - README sections for UT15 (WBOIT, Volition WBOIT, AOIT, Phenomenological; `:1099-1105`) and UT9a (`:1010`) have no source. 9a is listed as removed at `:272-273`.
   - GTAO is retired (`:732`). A strict search of the example `src/` folders and `Common_3/Renderer` for WBOIT, MBOIT, AOIT, weighted blended, phenomenological and rasterizer-ordered finds one hit: a stale comment, `// Pack AOIT data` (`ParticleUtils.h.fsl:80`), above code that just stores colour and depth.
   - Release "1.64" only says development continues on Codeberg (`:76-78`).
2. **The Forge's screen-space shadows are Sony Bend's code with Sony's notice gone.** [V]
   - Compare `F/.../09_LightShadowPlayground/Shaders/FSL/ScreenSpaceShadowsGenerate.comp.fsl:35-71` with `bend-dl/x/bend_sss_gpu.h:183-226`.
   - The function name, `ma_light_frac`, `major_axis_start`, `start_xy`, `end_xy`, `xy_delta`, `thread_step` and `pixel_distance` match, line for line in the parts I compared.
   - Bend's file is "Copyright 2023 Sony Interactive Entertainment", Apache-2.0 (`bend_sss_gpu.h:3-5`).
   - The Forge file carries only a Forge header (`:1-23`) and no credit: a search for Bend, Sony and Aldridge finds nothing in UT09 or the README (the README's only hits for that search are Drobot, about VRS, `:486,588,1113`). Port from Sony's original.
3. **The "ESM" is not an exponential shadow map.** [V] `F/Common_3/Graphics/ShaderUtilities.h.fsl:339-362` averages raw depth with 9 taps, then does `saturate(exp((z - avg) * k))`. No exponential moments are stored or prefiltered. Our PCF and PCSS are better. Nothing to take.
4. **There is no FXAA, SMAA, TAA or upscaler in the clone.** [V] Search of `Examples_3`, `Common_3/Renderer` and `ShaderUtilities`: nothing. AA here means MSAA only. J's FXAA 3.11 plus MSAA is already ahead for our targets.
5. **The SDF atlas alone is 512 MiB.** 512 x 512 x 1024 x R16F ([V] `SDFConstants.h.fsl:31-33`, `09_LightShadowPlayground.cpp:5423`; size computed). The README warns baking may take "8+ hours" (`:993`).
6. **ASM's shadow core is fragment-only.** It is the most portable of the fancy shadow methods, and its budget (128 MiB atlas) is what rules it out for now. See §5.
7. **The mobile claims come with hardware floors.** VB1 picks GPUs with `MaxBoundTextures > 3075` and `PrimitiveIdSupported == 1` ([V] `Visibility_Buffer/src/GPUCfg/gpu.cfg:7-8`). "Higher-end phones" (`README:213-216`) and "Galaxy S23 and higher" for VRS (`README` UT15a section) mean Vulkan flagships, not our floor.

---

## 3. Visibility buffer and triangle filtering

### 3.1 What it is

**TVB 1.0** [V]
- Compute filters triangles and compacts draws (`Common_3/Renderer/VisibilityBuffer/VisibilityBuffer.cpp:91,270,298`).
- Indirect draws fill a depth and ID target (`Visibility_Buffer.cpp:3106,3143`).
- One full-screen shade fetches triangles from storage buffers (`VisibilityBufferShade.frag:91-102`), computes barycentrics and analytic derivatives (`CalcFullBary`, `VisibilityBufferShadingUtilities.h.fsl:91-150`), and samples bindless textures (`:193-197`).

**TVB 2.0** [V]
- Software rasterization in compute with 64-bit atomics (`Visibility_Buffer2/.../BinRasterizer.comp.fsl:245-247`, `TriangleFiltering.comp.fsl:218`; `uint64_t` in shade at `VisibilityBufferShade.frag:61`).
- OUT outright on two constraints.

### 3.2 What TVB 1.0 truly requires

| Need | Forge use | GLES3/WebGL2 status | Workaround |
|---|---|---|---|
| Primitive ID in the fragment stage | `SV_PrimitiveID`, `VisibilityBufferPass.frag:8-13` | Absent from ESSL 3.00 [I] | De-index: 3 unique vertices per triangle, flat ID varying. Vertex work rises to 3 per triangle, losing the vertex cache [I] |
| Vertex pulling | `LoadByte` on storage buffers | No SSBO | `texelFetch` from data textures (we already do this for bones, VAT and lattice: `J/engine/shaderlib/includes/skinning.glsl:25-28`) |
| Materials | Bindless array indexed per pixel | 16 fragment samplers minimum [I] | One full-screen pass per material (material-depth or stencil), or same-size texture arrays |
| Geometry culling | Compute | GLES3 and WebGL2: no compute (sokol `compute = version >= 310`, WebGL2 is 3.00; [V] `sokol_gfx.h:10292`) | None needed, or CPU jobs |
| Draw submission | Indirect | None in sokol [V per `J/docs/openengine-analysis/01-rendering.md` §GPU-driven] | Plain draws, instancing |
| MSAA shading | MS texture load | sokol `msaa_texture_bindings = false` on GLES3 and Apple GL ([V] `sokol_gfx.h:10168,10293`) | None: FXAA only |

### 3.3 Is a pixel-shader-only, filter-free variant meaningful on GLES3/WebGL2?

**It can be built. It is not worth building.** [I, reasoned from the above]

1. **The bandwidth win does not transfer.**
   - TVB's case is against G-buffers and desktop overdraw (`README:338`).
   - We are forward on tile GPUs, where hidden-surface removal (Apple, PowerVR) or early-Z and forward pixel kill (Mali, Adreno) already remove most overdraw.
   - sokol offers no subpasses or framebuffer fetch, so the VB becomes a stored render target plus depth, loaded again as textures. We would add a store and load to save work the hardware already skips.
2. **The one-draw shade is lost.** Without bindless the shade becomes N full-screen passes, one per material, on tilers.
3. **Vertex load goes up, not down.** De-indexing triples vertex shading, and on tilers binning cost scales with it. With the triangle filter removed, its compensating cull is gone too. That filter is also what the do-not-copy list forbids in GPU form.
4. **Per-pixel cost is high.** At least 3 index fetches, 3 positions, 3 UVs, 3 normals (tangents are derived), a matrix fetch, 3 matrix-vector transforms, `CalcFullBary` in highp, then `textureGrad`.
5. **It would be a second renderer.** Our vertex paths are skinning bones, morphs, VAT, lattice, terrain and the `SURFACE_DISPLACE` hooks, plus library and user shaders (`J/engine/shaderlib/includes/`, `library/`). Each would need a VB-compatible form, matching the forward path for the "same result" rule. Transparency stays forward anyway.
6. **No MSAA** on GLES3/WebGL2.

**Verdict: OUT**, as a primary path and as a desktop fast path; the second path has no payoff at our scene scale. Only micro-triangle overshading would really improve, and `meshoptimizer` LODs (`J/engine/core/pool/MeshLodPool.cpp`) attack that more cheaply.

**Keep as utilities:** the `CalcFullBary` derivation (from the DAIS paper, cited at `:101-102`) if analytic derivatives are ever needed.

### 3.4 Triangle filtering

`FilterTriangle` at `Common_3/Renderer/VisibilityBuffer/Shaders/FSL/TriangleFiltering.h.fsl:78-194` [V] is plain arithmetic: homogeneous back-face test (`:89`), frustum (`:119-125`), small-primitive in 23.8 fixed point (`:146-188`). Compute and buffers are only its harness. Licence: AMD 2016 MIT (`:2`) plus Forge Apache (`:25-43`); origin is GeometryFX (`README:335,379`).

- **GPU form: OUT** (do-not-copy: GPU-driven culling).
- **CPU job form: PORTABLE-WITH-CHANGES.** SIMD over clusters, writing a per-view transient index buffer.

Constraints [I unless marked]:
- WebGL2 buffers are type-separated ([V] `sokol_gfx.h:10296`).
- Dynamic update size is capped per frame (`sokol_gfx.h:479-480`).
- Single-threaded web has no job parallelism ([V] `J/engine/core/Engine.h:9`).

Advice:
- Start at cluster granularity with meshoptimizer's meshlets and cones. J uses meshopt only for LODs, placement and lattice today ([V] grep).
- Try it first on **shadow casters**, where tile GPUs pay vertex cost with no shading payoff, and where S10's cache keeps results valid across frames.
- Treat it as an experiment with a kill criterion. The Forge itself removed CPU cluster culling ([V] `README:406`).
- Overlaps the meshoptimizer research stream.

---

## 4. Order-independent transparency

**What J has.** Per-object distance sort, `transparentQueue` ([V] `J/engine/core/subsystem/RenderSystem.h:573-651`). No OIT ([V] grep; the hits were unrelated).

| Technique | Source | Real requirements | Verdict |
|---|---|---|---|
| VB per-pixel linked list of triangle IDs | 15a `VisibilityBufferPassTransparentVoid.frag:43-53`, resolve `VisibilityBufferShade.frag:369-427` | SSBO, fragment atomics, VB | OUT |
| Fixed-size per-pixel list (particles) | `ParticleUtils.h.fsl:64-86`, `OitCompose.frag:48-86` | SSBO, fragment atomics | OUT |
| TressFX ShortCut, K-nearest depth peel | `HairShortCutDepthPeeling.frag:81-97` | Atomic max on storage texture or buffer | OUT |
| ShortCut's colour resolve | `HairShortCutFillColor`, `HairShortCutResolveColor.frag:62-79` | Fragment only | PORTABLE-AS-IS math (weighted average); AMD MIT |
| WBOIT, AOIT, Phenomenological | README only, **no code in clone** | n/a | WBOIT from the paper; AOIT OUT (needs ROV) |

### Recommendation: weighted blended OIT, opt-in

The portable piece of ShortCut is the idea of a multiplicative "inverse alpha" plus a weighted-average colour resolve. That is the same family as WBOIT.

**The catch that decides the design.** On GL and GLES3, sokol reports `mrt_independent_blend_state = false` ([V] `sokol_gfx.h:10163,10286`; `:12004` has `// FIXME: separate blend state per color attachment`). Classic WBOIT uses different blend functions per attachment, so it cannot run on those backends as published. Use one state for both targets, which works everywhere [I, derivation below]:

```
Blend (both attachments): RGB ONE, ONE;  Alpha ZERO, ONE_MINUS_SRC_ALPHA
RT0 RGBA16F, clear (0,0,0,1): out = vec4(C*a*w, a)   // rgb sums; alpha = product of (1-a), the revealage
RT1 R16F (or RG16F), clear 0: out = vec4(a*w, ...)   // r sums the weight; no alpha channel to care about
Resolve over the scene: rgb = RT0.rgb / max(RT1.r, eps); alpha = 1 - RT0.a  (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
```

- **Formats.** R16F, RG16F and RGBA16F are renderable and blendable when the half-float extension exists; on WebGL2 `EXT_color_buffer_float` implies this ([V] `sokol_gfx.h:10001-10011,10350-10356`). Otherwise they are sample and filter only.
- **Fallback.** Check `sg_query_pixelformat`; if not renderable, use today's sorted path. That is what a device without the format gets, and it is not pixel-identical. Treat OIT as a quality option, not a fast path of one look, and record it in the spec.
- **Depth.** The accumulation pass needs the opaque depth, so it adds a pass with depth load. On tilers that is the real cost, so keep it opt-in per camera and zero when off. MSAA scenes need the 16F format's `.msaa` capability.
- **Domain.** Our scene target is 8-bit display-referred ([V] `J/docs/specs/S13-post-fx.md:32`). Accumulate in the same domain so looks match.
- **Scope limits.** Only alpha-blended materials. Additive and multiply stay as they are. Library shaders need a variant.
- **Weight function.** Scene-scale tuning is required, a known WBOIT weakness [I].
- **Provenance.** McGuire and Bavoil, JCGT 2013, linked at `F/README.md:1102`. Not code from The Forge. I did not search patents.

---

## 5. Shadows

**What J has.** Stable CSM, blend and fade, receiver-plane bias, Vogel PCF, PCSS on Ultra, slot planner, static cache ([V] `J/engine/core/render/shadow/`, `includes/shadow_sampling.glsl`, S10). S10 rules out MSM/EVSM/DPCF, virtual maps, and defers contact shadows ([V] `S10-shadows.md:38,40,43`).

| Technique | Source | Real requirements | Verdict |
|---|---|---|---|
| "ESM" | `ShaderUtilities.h.fsl:323-364` | Filterable depth read | OUT: no value (§2.3) |
| VSM | `meshVSMDepthPass.frag:48-51`, `visibilityBufferShade.frag:100-114`; RG16 UNORM or RG16F (`cpp:7634-7636`) | Render-to-16-bit, filter; blur in compute (`gaussianBlur.comp:36-50`) is trivially fragment | PORTABLE-WITH-CHANGES; excluded by S10 family rule, no reason to reopen |
| MSM (Hamburger 4) | `visibilityBufferShade.frag:119-187`, formats `cpp:7639-7644` | RGBA16 UNORM (not core ES3 [I]) else 16F/32F; 3x3 solve per pixel | PORTABLE-WITH-CHANGES; excluded by S10:38 |
| ASM with parallax-corrected cache | `09_...cpp:2262-3300+` (`ASMTileCache`, `ASMQuadTree`, `ASMFrustum`), `PackedQuads*`, shade `:191-438` | Fragment only for shadows; **R32F and RGBA32F render targets** (`cpp:7827,7861`); 8192x4096 atlas (`ASMShaderDefs.h.fsl:44-45`), 128 MiB computed | PORTABLE-WITH-CHANGES; not now (below) |
| SDF soft shadows | `SDFMeshBakedShadow.comp:60-160`, `upsampleSDFShadow.frag` | March needs only 3D R16F texture and a pass; compute is incidental. 512 MiB atlas, up to 255 objects x 32 steps per pixel (`SDFConstants.h.fsl:37`, `.comp:70,120`), offline bake | DESKTOP-FAST-PATH-ONLY, fallback S10 PCSS; **do not take** |
| Screen-space shadows | §5.1 | Compute, wave ops optional, atomics on output | PORTABLE-WITH-CHANGES |
| Capsule shadows | §5.2 | Uniforms only | PORTABLE-AS-IS |

**ASM** is attractive for large outdoor worlds: only up to 4 tiles per frame are rendered (`ShaderDefs.h.fsl:35-37`) and a moving sun is handled by a warp from a depth-extent map (`visibilityBufferShade.frag:408-412`). Against it:
- About 1000+ lines of CPU quadtree and cache.
- WebGL2 needs `EXT_color_buffer_float` for R32F and RGBA32F targets, with no fallback path given.
- The atlas must shrink a lot for mobile (8192 exceeds the guaranteed texture size on our floor [I]).
- It collides with S10's explicit exclusion (`S10:43`).

Revisit only if a project needs long shadow range on phones. Algorithm credit: Turchyn, GPU Zen 2 (`README:950`).

### 5.1 Contact shadows (rank 4)

**Take from Sony, not from The Forge** (§2.2). Licence and notice steps in §10.

What changes in the port [I]:
- **No wavefront.** Bend dispatches in light-centred wedges so 64 threads share about 4 depth reads each through group-shared memory (`ScreenSpaceShadowsGenerate.comp:77,110-167,197-198`). A fragment shader gets no sharing, so each pixel pays every tap. The analysis doc already notes this loss ([V] `J/docs/openengine-analysis/01-rendering.md:291-294`).
- **Sample budget.** Forge uses 60 samples, 4 hard, 8 fade ([V] `VisibilityBufferResources.h.fsl:49-52`). Use 8-16 with `J/.../BlueNoise64.h` or interleaved-gradient jitter, at half resolution on Medium.
- **Atomics disappear.** `AtomicMin` (`:256-259`) exists because wedges overlap writes. One fragment writes one pixel.
- **Wave early-out disappears** (`:169-185`); it is already optional.
- **Depth.** Needs the depth pre-pass ([V] `RenderSystem.h:392-395`) with our packed RGBA depth ([V] `depth_util.glsl`). Bend's two-tap manual bilinear (`:141-142`, `:155`) suits packed depth well.
- **Order.** Depth pre-pass, then contact mask, then opaque pass multiplying the sun term. No G-buffer is needed.
- **Mobile caveat.** If no other feature wants depth, this adds a full depth pass. That is why it ranks 4 despite the visible payoff. S10 asks for a separate spec (`S10:40`).

### 5.2 Capsule soft shadows (rank 2)

`F/.../28_Skinning/Shaders/FSL/plane.frag:41-70` [V]: closest approach between the light ray and a capsule segment, smoothstepped, multiplied per capsule. `28_Skinning.cpp:868-882` fits 8 capsules to named bones.

Why it fits mobile and web [I]:
- No pass, no target, no extra vertex work. Eight capsules means a small uniform array and a loop.
- Characters can leave the shadow map, so S10's static cache stays valid when they move. That cache is "the largest shadow saving available" ([V] `S10-shadows.md:12`).

Changes:
- Forge uses it only on a ground plane. As a `USE_CAPSULE_SHADOWS` variant, any receiver works. Self-shadowing needs a bias.
- A component to fit capsules to bones with editor gizmos, Lua and MCP.
- Sun direction only at first.

**Licence: reimplement.** The comment says "Modified from the original implementation by Inigo Quilez, shadertoy MlGczG" (`plane.frag:36-37`), under a Forge Apache header. Shadertoy's default licence is non-commercial unless the author states otherwise [I]; I did not read that shader's header. Check it, or re-derive from the geometry. The Forge's Apache header does not settle this.

---

## 6. Lighting

**Screen-tile light lists (rank 1).** "Forward++" (`README:338`).

Forge [V]:
- 8x8 grid, 128 lights (`Visibility_Buffer/.../ShaderDefs.h.fsl:33-37`).
- Built in compute with atomics (`LightClusters.comp:91-101`). Tile test is a projected circle (`:63,92-93`).
- Read in shade (`VisibilityBufferShade.frag:296-310`).
- UT39 scales to 20000 lights with depth-bound subclusters (`ParticleDefs.h.fsl`, `LightClustersDepthBounds.comp`), which needs compute and is not for us.

J today [V]: per-object selection, capacity 4, 8 or 16, default 8, the rest folded into SH (`Engine.h:28`, `SceneSettings.h:370`, `RenderSystem.cpp:2016-2035`, `lights.glsl`). So a large mesh (terrain, a level block) spends its slots on lights far from most of its pixels, and lights pop as selection changes ([V] the limitation is named at `01-rendering.md:200`). Particle lights are out in S20 ([V] `S20:39`).

Port [I]:
- CPU job per frame: project each light's sphere into a 16x9-ish grid. Trivial at hundreds of lights, fine single-threaded on web.
- Upload a small RG8 or RGBA8 index texture, read with `texelFetch`. Light data in a UBO (about 256 x 64 bytes fits ES3's 16 KB minimum; check sokol's uniform-block size limit, not verified) or an RGBA32F texture, as we do for bones.
- Keep the library contract: `lights.glsl` promises `pixelLightCount()` and `getLight(i, ...)` ([V] `:1-18`). Make `getLight` indirect through the tile and library shaders get it free.
- **D3D11 trap:** `lights.glsl:11` warns HLSL unrolls loops that sample shadows or masks. Keep the constant bound, or use `textureLod` and a loop-safe compare.
- Zero when off (shader variant, no texture).
- Bonus: gives S20 a route to particle lights.
- 2D tiles have no depth slicing. A corridor of lights behind each other all land in one tile. Cap per tile, and rely on the attenuation early-out.

Licence: Forge Apache only, and the CPU version is a rewrite. Credit as "after".

**God rays.** `godray.frag:130-162`: 10 dithered shadow-map steps, blurred in compute. **Already have:** S12 volumetric fog with shadow shafts, `effects/godrays.frag`. Skip.

---

## 7. Screen-space effects

### SSR

J: linear view-space march, up to 128 steps plus 6 binary, jitter, glossy blur ([V] `J/engine/shaderlib/ssr.frag:18-22,120-150`, `ShaderVariantCompiler.cpp:732`).

| Technique | Requires | Verdict |
|---|---|---|
| FFX SSSR full pipeline (classify, indirect, temporal denoise; `SSSR_*.comp`) | Compute, storage images, wave ops (`ClassifyTiles:103-114`), indirect args, history (`SSSR.srt.h:54-67`) | OUT as a pipeline |
| `HierarchicalRaymarch` (`SSSR_Intersect.comp:171-267`) | Depth pyramid and a loop. Wave only at `:261`, for an optional early exit | PORTABLE-WITH-CHANGES |
| SPD depth pyramid (`DepthDownsample.comp`) | Compute, atomics, wave | Replace with fragment reduce chain |
| Pixel-Projected Reflections (`PPR_Projection.comp:258-271`, `PPR_Reflection.frag`, `PPR_Holepatching.frag`) | Compute scatter with `AtomicMin` to arbitrary pixels (`:269`) | PORTABLE-WITH-CHANGES at best (below); not recommended |

**Hi-Z port notes** [I]:
- Forge is reversed-Z and reduces with max (`DepthDownsample.comp:96-99`). J's far is 1.0 ([V] `ssr.frag:76`), so reduce with min and flip the tests in `AdvanceRay`.
- Packed RGBA8 depth means decode and re-encode per level.
- **Mip feedback:** sokol's real texture views need GL 4.3 ([V] `sokol_gfx.h:10166,10294`). Rendering level N while sampling N-1 is a feedback problem on GLES3 and WebGL2. Simplest portable answer: one atlas holding all levels, read with `texelFetch` and a per-level offset.
- Expect fewer depth reads and no thin-object tunnelling, at the price of about 7 small passes. SSR is rarely enabled on phones, hence runner-up. `01-rendering.md:321` already pencils this in.
- Licence: AMD 2020 MIT (`SSSR_Intersect.comp:2`) plus Forge Apache on the same file; keep both. `SSSR_ResolveTemporal.comp:54-60` also embeds Playdead MIT, not needed for this port.

**PPR:** a fragment shader cannot scatter. A port would draw one point per pixel with vertex texture fetch, and use depth test for "closest". J already has exact planar reflection by re-render ([V] `MeshComponent.h:430-432`, `CameraComponent.h:106`). PPR would save that second scene pass but only reflect what is on screen, at about one vertex per pixel. Not now.

### AO

- **What's in the clone:** one AO, inline in the shade pass (`Visibility_Buffer/.../shading.h.fsl:203-349`). Mirrored ring-pattern gathers (`:219-248,302-341`); 16, 48, 96 or 160 depth samples at quality 1-4; default 2 (`Visibility_Buffer.cpp:231`). No blur.
- **Requires** `textureGather` (absent in ESSL 3.00 [I]) and a plain depth texture; ours is packed.
- **Verdict: OUT, no gain.** J's 32-kernel hemisphere with bilateral blur is comparable ([V] `ssao.frag`, `ssao_blur.frag`). GTAO was removed upstream (`README:732`). If AO work happens, the MIT Spartan GTAO named in `01-rendering.md:287-290` is the better source.
- **Provenance unsure:** it looks like AMD's HDAO family, from memory. AOFX (MIT) shares mirrored "valley" sampling (`aofx-dl/k.hlsl:102-106`) but not this ring table, so unproven. The file has only a Forge header.
- **Small idea:** `reconstructNormalCS` (`:137-151`) chooses sides by second-order depth extrapolation, stricter than J's 4-neighbour test at `ssao.frag` (normal fallback). It cites a Shadertoy and two blog posts (`:81-84`): reimplement from the atyuwen article's idea, do not copy.

### AA

| Technique | Source | Requires | Verdict |
|---|---|---|---|
| MSAA with per-sample shading on edges ("programmable MSAA", `README:137`) | `MsaaEdgeDetect.frag:91-124`, `VisibilityBufferShade.frag:53-69` | MS texture fetch, `SV_SampleIndex`, per-sample stencil | OUT: sokol `msaa_texture_bindings` false on GLES3 ([V] `:10293`) |
| Software VRS | `fillStencil.frag`, `resolveVRS.comp`, 15a `.cpp:205,3407` | 4x MSAA, programmable sample locations (`README:1152`), `SV_Coverage`, compute resolve | OUT. Dynamic resolution (S13) is our answer |
| HDR10 output | `display.frag:51-60,92` | Swapchain control | OUT (no HDR scene, `S13:32`) |
| FXAA, SMAA, TAA | none in clone | n/a | Nothing to take |

Gaps stand in J (SMAA, TAA; `01-rendering.md:124,165-166,267`), but The Forge has no source for them.

**"In-flight motion vectors"** (`README:192-194`) means recomputing previous position from `prevMVP` per triangle (`fillStencil.frag:147-211`), which needs VB data. Only the depth-reprojection form applies to forward; it handles camera motion only. No code to take.

---

## 8. Materials, hair, skin

**What's in UT06:** Metal, Wood and Hair only (`06_MaterialPlayground.cpp:90-97`) [V].

| Item | Source | Verdict |
|---|---|---|
| GGX PBR | `renderSceneBRDF.frag:84-165` | **Have**, J's is richer (`mesh.frag`, `pbr_ext.glsl`) |
| Oren-Nayar diffuse | `:103-133` (cites a Gamasutra article, `:105`) | PORTABLE-AS-IS; S11 lists as follow-up (`S11:33`). Low value, S |
| Derivative TBN | `:60-80` | **Have** (`tangent_frame.glsl`) |
| vMF map | `:263-266`, only a second normal map slot | Nothing to take. **Have** specular AA |
| IBL prefilter, BRDF LUT | `PBR_*.comp` | Compute. **Have** in fragment (`probe_filter.frag`) |
| **Skin / SSS** | **None.** Grep over UT06 shaders finds nothing [V] | Not in clone |
| TressFX simulation | `HairIntegrate`, `HairLengthConstraints`, `HairLocalConstraints`, `HairShockPropagation`, `HairUpdateFollowHairs`, `.comp` | Compute plus RW buffers. OUT as is; a CPU job port is possible and low priority |
| Strand expansion and coverage | `hair.vert.fsl:65-110`, `hair.h.fsl:73-90+` | PORTABLE-WITH-CHANGES: storage buffers to VTF, as for bones. Needs `.tfx` assets (`Common_3/Resources/AnimationSystem/ThirdParty/OpenSource/TressFX`, AMD MIT) |
| ShortCut | §4 | Depth peel OUT; resolve math portable |
| **Kajiya-Kay two-lobe** | `HairShortCutFillColor.frag:56-83` | **PORTABLE-AS-IS** (rank 5) |

**Hair lobe for cards.** S11 gave J sheen and anisotropy, but no hair model ([V] grep: the only "hair" hits are film-damage). Pure ALU: a tangent, two shifted highlights, a diffuse term. Fits the existing compile-time variant model of S11 ("costs exactly nothing when unused"). Needs the Inspector, Lua and MCP surface. AMD 2017 MIT on that file (`:2`), plus Forge Apache. The formula is published and short, so a rewrite with credit is cleanest.

Cost note: true hair needs OIT or alpha-to-coverage too. Cards with alpha test work without §4.

---

## 9. Particles

**What J has.** S20 is implemented: SoA kernels, jobs, radix sort, 32-byte instances, under `J/engine/core/particles/` ([V] `ParticleRender.h:14,31,39`). S20 rules out GPU simulation and depth collision ([V] `S20:33-38`).

**UT39 mega particles.** One `ParticleSimulate.comp` over one large buffer (`README:292-294`), up to 8M particles (`ParticleDefs.h.fsl`).
- **Requires:** compute, SSBO read-write in the vertex stage (`particle.vert:52-56`), indirect draw args written by compute (`ParticleSimulate.comp:74-77`), bindless textures (`particle.frag:60-62`), fragment atomics for transparency (`ParticleUtils.h.fsl:75`), and a software raster path (`PARTICLE_HW_RASTERIZATION_THRESHOLD`).
- The release note's "Adreno fallback" is a smaller particle count still on storage buffers (`README:170-171`, `ParticleDefs.h.fsl`), not a no-compute path.
- **Verdict: OUT as an architecture.**

**Worth borrowing**

| Idea | Source | Verdict | Value |
|---|---|---|---|
| Adjacent-index flocking: 8 neighbours by array index, no grid | `ParticleSimulate.comp:262-300`, `ParticleBoids.h.fsl:26-46` | PORTABLE-WITH-CHANGES: a CPU kernel for S20 (grep: no flocking there). O(n), crude; works because neighbours in the array stay together | S, niche |
| Stateless vertex-shader particles: position from seed plus uniforms | UT03 `Particle.vert.fsl:25-100` | PORTABLE-AS-IS with instanced quads rather than `SV_PointSize` | S; dust, rain, snow, stars in the millions at zero CPU, even on WebGL2. S20's "no GPU simulation" is about stateful simulation, so state this as a separate small item. No collisions, no scrubbing issue (pure function of time). Origin is Intel's Stardust (README section 3): reimplement |
| Fixed-point packing, 20 bytes per particle | `ParticlePacking.h.fsl:107+`, `PARTICLE_DATA_STRIDE 5` | Low value: our 32 bytes is the render instance and holds different fields | Skip |
| Particle lights | UT39 sections | Via rank 1 tile lists | Follow-on |

---

## 10. Licensing

**The Forge overall.** Apache-2.0, "Copyright (c) 2017 - 2025 The Forge Interactive Inc." ([V] `F/LICENSE:189`). No NOTICE file at the root ([V] `ls`), so §4(d) adds nothing from them. Console platforms are under a separate commercial licence (`README:29,270`) and are not in scope. "The Forge" is marked as a trademark (`README:3`); §6 grants no trademark rights, so credit it factually and do not market with it.

**Patents.** Searching the clone (excluding the large binary SDK folders) found "patent" only in `LICENSE`, `cpu_features/LICENSE`, `agdk/LICENSE`, `bstrlib/gpl.txt`, and a stb_truetype comment ([V]). **No effect mentions a patent.** I ran **no patent search**, so absence here clears nothing. Apache §3 covers only contributors' own patent claims and terminates if you sue. For anything reaching a spec, the README's Slug caution (`J/docs/specs/README.md`) applies equally.

**Non-Apache or uncertain origins** [V unless marked]

| Code | Origin | Action |
|---|---|---|
| `ScreenSpaceShadowsGenerate.comp` | **Sony Interactive Entertainment, Apache-2.0** (`bend_sss_gpu.h:3`). Header missing in Forge | Port from Sony's zip; keep Sony's notice |
| `TriangleFiltering.h.fsl` and copies | AMD 2016 MIT + Forge Apache | Keep both |
| `Hair*.fsl`, `hair.*` | AMD 2017 MIT + Forge Apache | Keep both |
| `SSSR_*`, `DepthDownsample`, `SSSR.srt.h` | AMD 2020 MIT + Forge Apache | Keep both |
| `SSSR_ResolveTemporal.comp:54-60`, `RayQuery.comp:89` | Playdead 2015 MIT | Keep if copied |
| `28_Skinning/plane.frag` | Inigo Quilez, Shadertoy MlGczG | **Do not copy.** Licence unchecked; reimplement |
| `shading.h.fsl:81-84` normal reconstruction | Shadertoy fsVczR, two blogs | **Do not copy.** Reimplement from the idea |
| `RayQuery.comp:122` | Shadertoy 4t2SDh | Not needed |
| `renderSceneBRDF.frag:105` Oren-Nayar | Gamasutra article | Trivial formula; rewrite |
| `shading.h.fsl:203-349` AO | Possibly AMD HDAO [I, unproven] | Not recommended anyway |
| `samplerBlueNoise...2spp.cpp` | Forge header only; table looks like Heitz et al. [I] | Not needed; J has `BlueNoise64.h` |
| UT03 `Particle.vert` | Intel Stardust (README) | Reimplement |
| `Common_3/Graphics/ThirdParty/OpenSource/*` | Vendor SDKs and allocators (ags, nvapi, D3D12MA, VMA, ...) | Nothing rendering-related to take; do not vendor |
| `Common_3/Resources/AnimationSystem/ThirdParty/OpenSource/TressFX` | AMD 2017 MIT | Only if hair assets are wanted |
| `bstrlib` | Dual licence with a GPL text in the folder (`gpl.txt`) | Not needed; do not vendor without reading |

**Header oddity.** Forge headers say "Licensed to the Apache Software Foundation (ASF)... See the NOTICE file" (for example `TriangleFiltering.h.fsl:28-43`). It's ASF boilerplate on non-ASF code, and that NOTICE file does not exist. Keep the text as is when copying; do not "fix" it.

**Apache-2.0 obligations for copied code (§4)** [V `F/LICENSE`]
- (a) Give recipients a copy of the licence.
- (b) Modified files must carry a prominent notice that you changed them.
- (c) Keep all copyright, patent, trademark and attribution notices in source form.
- (d) Carry any upstream NOTICE file (none from The Forge).
- A rewrite from an idea, with no copied expression, has no §4 duty; credit it anyway, as J's Vertex Animation section does.

**What `J/NOTICE.md` expects** [V]
- **Vendored libraries** under `libs/`, `engine/libs/`, `engine/libs/shadercompiler/libs/`: a table row `| [Name](url) | version | location in backticks (licence file) | licence | used for |`, plus a licence file in the directory. `tools/ci/check-notices.py` enforces both. `engine/libs` also needs an `engine/notices.json` entry. Apache libraries also carry their NOTICE (lexbor row, Basis Universal row).
- **Ported shaders** (our likely case): the "Shader code ported from:" table, `| Code | Version | Location | License | Used for |`, naming the commit, the file, and a licence file beside it (pattern: `engine/shaderlib/includes/LICENSE.hextiling.txt`, a provenance line followed by the licence text).
- **File header:** say where it came from (pattern: `hex_tiling.glsl:1-3`, "Adapted from ... (licence), Copyright ..."). J's own style needs `// (c) Eduardo Doria and contributors` plus SPDX MIT on source files; for a port, keep the upstream notice and mark it Apache-2.0 rather than relabelling.
- **Formula-only credits:** a table in prose, as in the Vertex Animation section ("no third-party code is copied").

Suggested row:
`| [Bend Studio Screen Space Shadows](https://www.bendstudio.com/assets/cms/downloads/code_final_candidate.zip) (`bend_sss_gpu.h`, ported to a fragment shader; modified) | SIGGRAPH 2023 release | `engine/shaderlib/includes/contact_shadows.glsl` (`LICENSE.bend-sss.txt`) | Apache-2.0, Copyright 2023 Sony Interactive Entertainment | contact shadows |`

**Open question to settle before the first Apache shader port.** `check-notices.py` does not look at shader ports, and `--third-party-notices` builds a player's notices from library rows ([V] docstring). Shaders compile into binaries (`engine/shaders/*.h`). I did not confirm that the "Shader code ported from" rows reach exported games; §4(a) needs that. [I]

---

## 11. Full walk

"Read" = shaders opened. "List" = file list and README only.

| Test | Depth | Content | Verdict |
|---|---|---|---|
| 01 Transformations | List | Solar system, skybox | Nothing |
| 03 MultiThread | Read vertex shader | Per-thread command buffers; stateless particles | Threaded encoding OUT (sokol is single-context [I]); particles §9 |
| 06 MaterialPlayground | Read | §8 | §8 |
| 09 LightShadowPlayground | Read | §5 | §5 |
| 10 ScreenSpaceReflections | Read | §7 | §7 |
| 11, 11a MultiGPU | List | Linked and unlinked multi-GPU | OUT |
| 12 ZipFileSystem | Read occlusion shader | File system; a flat-colour shader | Nothing for rendering |
| 14 WaveIntrinsics | List | Wave ops demo | OUT (`README` section 14) |
| 15a VisibilityBufferOIT | Read | §3, §4, §7 | OUT |
| 16 Raytracing | Grep | Ray-query path tracer | OUT (do-not-copy spirit; RT API) |
| 19 CodeHotReload | List | C hot reload | Not rendering |
| 21 Animations | List | Ozz | Not rendering; another stream |
| 28 Skinning | Read plane shader | Skinning, capsule shadows | §5.2 |
| 34 Input, 36 Algorithms | List | No rendering | Nothing |
| 39 ParticleSystem | Read | §9, §6 | OUT with borrowings |
| Visibility_Buffer | Read | §3, §6, §7 | OUT with borrowings |
| Visibility_Buffer2 | Read | Compute raster, 64-bit atomics; PQ output | OUT |
| Common_3/Renderer | Read | VB, VB2, particles | As above |
| Common_3/Game | List | flecs, Lua, no rendering ([V] `ls`) | Nothing |

**Not read in depth:** the C++ of UT10, the bulk of `09_...cpp` beyond the parts cited, `DebugVisualize`, older releases on GitHub (`README:736`). Line numbers come from the commit above.
