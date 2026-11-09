# 01 — Rendering: JasperEngine (OURS) vs OpenEngine (THEIRS)

> **Note (2026-11-04, BF23 F28).** This report is historical and stays as written; decisions taken since:
> imgui-node-editor in the shader graph recommendation was rejected by `docs/specs/S21-animation.md` D14,
> and the fragment `SURFACE_*` hooks that recommendation maps a body to do not exist: the hooks are
> vertex-stage (`includes/surface_vs.glsl`), and in a fragment shader `SURFACE_*` only names the inputs a
> variant requires. Procedural materials bake to textures and a `.material` instead
> (`docs/specs/S90-procedural-material.md`).

Paths: THEIRS relative to `external/OpenEngine/`, OURS relative to repo root. All claims below come from reading code, not just file names. "RG" = render graph.

---

## 1. What THEIRS has

### 1.1 Architecture
- **Two-tier split** (`docs/rendering/rendering-architecture.mdx`): `Engine/Modules/Rendering` is a GPU library (~50k lines core + 53k lines backends + ~37k lines shaders). `Engine/{Include,Source}/Engine/Rendering` (~90k lines) is the orchestration layer: `RenderServices.cpp` (hub, 1651 lines, plus 7 `RenderServices*.cpp` siblings), `FrameOrchestrator.cpp`, `IRenderFeature` features, ECS extraction (`Systems/RenderExtractionSystem.cpp`, 3094 lines).
- **Immediate-mode RG**: declared fresh each frame into an arena, then culled by ref-count from sinks (each culled pass records an `RGCullReason`). Scheduling is structural (hazard components, attachment clustering). Barriers come from per-(mip,layer) subresource state; queues are partitioned with timeline semaphores; there is a transient pool and rotating history imports. Code: `Engine/Modules/Rendering/{Include,Source}/Rendering/Core/RenderGraph/` (`RGFrame.h` 863 lines, `RGRecord.cpp` 1055, `RGBarriers.cpp` 657, `RGSchedule.cpp` 472). It is Vulkan-shaped: layouts, barriers, queues.
- **Data-driven pipeline**: `Engine/Assets/RenderPipelines/ForwardPlus.rendergraph` (3140-line JSON, ~100 passes) is loaded by `Source/Engine/Rendering/Pipeline/RenderPipeline.cpp` (2224 lines). It has resource scopes (PerView/Frame/Persistent) and an expression evaluator for sizes and dispatch counts (`Pipeline/ExpressionEvaluator.cpp`). Generic `FullscreenShader` and `ComputeShader` node types (`Pipeline/Nodes/FullscreenShaderNode.cpp` 1624 lines, `ComputeShaderNode.cpp` 1142) mean most post FX need no C++.
- **Backends**: Vulkan primary (`Source/Vulkan`, 31.7k lines, with ray query and acceleration structures, `VulkanDevice.cpp:2828`), Metal (10.5k, includes `MetalHdrOutput.cpp`), WebGPU (7.2k, a "Compatibility" tier), DX12 (3.5k, documented as lagging). `RendererProfile.h` picks Full (bindless, GPU-driven, mesh shaders, RT) or Compatibility (classic bind groups, CPU draw streams). **No GL/GLES/WebGL2/D3D11 and no mobile targets.**
- Conventions: left-handed, **reverse-Z D32F**, negative viewport height, camera-relative "sector" rendering for planet scale (`RenderOrigin.h`).

### 1.2 Lighting
- **Clustered Forward+**: `Shaders/clustered_light_cull.comp` (343 lines). One 64-thread workgroup per cluster, LDS staging, deterministic strongest-first overflow, and Z-slice skipping from `cluster_depth_minmax.comp` (Drobot/HDRP-FPTL style). Traversal is in `Shaders/Includes/clustered_lighting.glsl` (759 lines). There is a cluster debug overlay pass.
- **PBR** (`Shaders/Includes/standard_pbr.glsl`, 462 lines; OpenPBR-flavoured): GGX plus Smith, anisotropic GGX, F82 edge tint, multiscatter energy compensation, a coat layer with internal-reflection darkening, Charlie sheen/fuzz with directional albedo, DICE-style SSS back-transmission, diffuse transmission, and iridescence (`iridescence.glsl`). `specular_aa.glsl` (91 lines) does Kaplanyan/Tokuyoshi normal-variance roughness widening. Surfaces include `standard_pbr_extended.glsl` (clearcoat/sheen/aniso/transmission/iridescence/SSS) and `triplanar_pbr.glsl`, plus parallax occlusion (`parallax_occlusion.glsl`).
- **Photometric light units**: `Include/Components/Rendering/LightPhotometry.h` covers lux, candela and lumen, plus Kelvin to linear RGB.

### 1.3 Shadows (very deep)
- `Source/Engine/Rendering/ShadowMapRenderFeature.cpp` (2466 lines) plus `...Passes.cpp` (1270) and `...ViewFit.cpp` (289).
- **Cascade projection modes** `Stable` / `Close` / authored-texel (`ShadowMapRenderFeature.h:53`):
  - Stable: bounding-sphere fit (`SliceBoundingSphereRadius`, `.cpp:545`) and clip-translation texel snap (`SnapClipTranslationToTexels`, `.cpp:183`).
  - Close: SDSM depth-bounds readback.
  - Authored-texel: texel size is authored and coverage is derived from it.
- **Filters** (`ShadowFilterQuality`): 3x3/5x5 grid, Poisson/Vogel PCF with screen- or shadow-space rotation, PCSS (blocker search plus a min/max pyramid early-out, `ShadowMinMaxPyramid.cpp`), MSM4 moment shadows, DPCF (Treyarch). EVSM shaders also exist (`evsm_*`).
- **Quality details**: cross-cascade blend band (`Includes/shadow_cascade_blend.glsl`), distance fade (`shadow_distance_fade.glsl`), receiver-plane depth bias (`shadow_receiver_plane.glsl`), coloured translucent (glass) shadows, cascade debug colours, and 8 debug modes.
- **Caching**: `CascadeShadowCache.cpp` skips re-rendering byte-identical cascades in static scenes. `PointShadowAtlasPlanner.cpp` (422 lines) plus `PointShadowFaceCull.cpp` give a point-light atlas with per-face culling and LOD.
- **Extras**: ray-traced directional shadow mask with denoiser (`RTShadowMaskService.cpp`, `rt_shadow_mask.comp`, `rt_shadow_denoise.comp` from FidelityFX-Denoiser, MIT). Bend Studio screen-space contact shadows (`Shaders/ScreenSpaceShadows/screen_space_shadow.comp`, **Apache-2.0**). A terrain clearance shadow map.

### 1.4 GI, reflections, AO
- **DDGI**: `DDGIProbeFeature.cpp` (3605 lines) plus ~10 `DDGI*` services and ~20 `ddgi_*` shaders. Hardware ray-query trace or software BVH trace (`ddgi_trace_hw/sw`), probe classify/relocate, glossy resolve, skinned geometry. Port of speedball-gi (MIT).
- **IBL**: `ImageBasedLightingFeature.cpp` (719 lines). GPU prefilter (`sky_specular_prefilter.comp`, `sky_diffuse_convolve.comp`, `brdf_lut`). Box-projected local probes (`localBoxProjection`, `.cpp:703`). Scene reflection probes with recapture schedule (`SceneReflectionProbeEnvironmentSource.cpp`, `ReflectionProbeSystem.cpp`).
- **SSR**: stochastic SSR, a FidelityFX-SSSR-style pipeline (classify → intersect → reproject → temporal → composite; `Shaders/ScreenSpaceReflections/sssr_*`, 13 files; `ScreenSpaceReflectionsNode.cpp` 965 lines).
- **GTAO** with visibility bitmasks and bent normals, temporal and upsample stages (`Shaders/AmbientOcclusion/gtao*.comp`; sweep ported from SpartanEngine, MIT).

### 1.5 Sky, atmosphere, fog, volumetrics
- **Physical sky** (Hillaire 2020 LUTs): transmittance, multiscatter and sky-view LUTs (`sky_*_lut.comp`), `sky_render.frag` (699 lines), star catalogue, moon disc, night tint. `SolarPath.h/.cpp` places the sun from latitude, day and hour (pure math). There is also an analytic tier.
- **HeightFog** module (`Engine/Modules/HeightFog`, SanielX/Height-Fog port, MIT): analytic height and distance optical depth, HG and multi-scatter phase, noise, gradients, sky fill (`Rendering/Shaders/Includes/height_fog_term.glsl`, 377 lines).
- **VolumetricFog** (`Engine/Modules/VolumetricFog`, 1818-line renderer): froxel grid with media, light, temporal reprojection, integrate and composite compute passes, plus local-light shadows (diharaw reference, MIT).
- **VolumetricClouds**: Seb Lague port (MIT). Worley 3D noise is baked by compute; the raymarch is a fullscreen **fragment** shader, with a temporal resolve and composite (`volumetric_clouds_march.frag`, 381 lines). There is also an atmospheric 2D cloud layer.
- **GPUFogParticles**: a textureless fog-particle surface shader (MirzaBeig, **Unlicense**). Also a "fog glow" multi-octave pass.

### 1.6 Post-processing and AA (all HDR)
- **Tonemap** (`tonemap.frag`, `Passes/TonemapPass.cpp`):
  - ACES 2.0 with OCIO tables (`TonemapAces2Tables.h`, 1667 lines, **BSD-3**, generated by `Tools/ShaderGen/gen_aces2_tables.py`).
  - Full AgX with outset matrix (MinimalAgX, MIT).
  - Khronos PBR Neutral (**Apache-2.0**).
  - HDR display headroom extension; HDR10/scRGB/EDR output paths (`SRGBEncodePass.cpp`, `MetalHdrOutput.cpp`).
- **Exposure**: histogram auto exposure (`auto_exposure_histogram/resolve`, `AutoExposureNode.cpp`), pre-exposure, exposure compensation curves.
- **Bloom**: 8-octave bloom down to 1/256 with KinoBloom-style gather (MIT) and lens dirt (SE Natural Bloom, MIT), a separate "scattering" chain, halation.
- **Lens effects**: depth of field as either FidelityFX DoF (`Packages/fidelityfx-dof`, 5 compute shaders, MIT AMD) or fast blur DoF; lens flares and sun glare; heat distortion.
- **Colour**: CAS sharpen, colour grade and filter, `.cube` LUT parser (`Source/Common/CubeLutFileParser.cpp`, 387 lines, pure C++), deband, vignette.
- **Stylised**: VHS stack (8 passes, punikonta, MIT), CRT, film simulation (`Packages/film-simulation`, MIT), chromatic aberration package.
- **AA** (`AntiAliasing.h`): MSAA, TAA (`taa_resolve.comp`, 410 lines: Catmull-Rom history, YCoCg variance clip, depth disocclusion), TAAU upscale, FXAA fast and quality (compute), SMAA 1x LUT-free (compute, 3 passes), two-frame temporal FXAA.
- **Scaling**: dynamic resolution (`DynamicResolutionController.cpp`, 437 lines, a pure control law), render-scale upscale, pixel-perfect upscale.
- **Post volumes**: `PostProcessVolumeExtract`, `PostProcessEffectRegistry.cpp`, 23 effect components (`Include/Components/Rendering/PostProcessEffects/`).

### 1.7 GPU-driven rendering
- **GPU scene and culling**: `GPUScene.cpp` (1496 lines), `GPUCulling.cpp` (1558), `GPUDrawStreamBuilder.cpp` (2887), `draw_command_scatter.comp` (839).
- **Occlusion**: frustum cull plus a **two-phase HZB occlusion cull** (`hzb_culling.comp`, the UE-style prevVisible scheme) with SPD-based HZB build. Draws go through `DrawIndexedIndirectCount`.
- **Bindless** via `BindlessResourceManager.cpp`.
- **LOD and transparency**: LOD crossfade packed into the draw word (`lod_crossfade.glsl`), HLOD (`HlodRuntime.cpp`), GPU-sorted transparency (`sorted_transparent_sort.comp`, bitonic network).
- **Animation**: GPU skinning compute (`animation_skinning.comp`) and morph targets.

### 1.8 Materials and shader authoring
- **Composer model**: a surface shader implements `SurfaceOutput EvaluateSurface(SurfaceInput)`. `ShaderComposer.cpp` (1010 lines) splices it into `Shaders/Adapters/adapter_forward.glsl` / `adapter_vertex.glsl` (1837 lines) with a variant `#define` preamble.
- **Material build**: `MaterialBuildService.cpp`, `ShaderCompileService.cpp` (shaderc), `SlangCompileLane.cpp` (Slang). JSON `.material` documents with validation.
- **Packaging**: `.shaderpkg` packages carry SPIR-V, reflection and meta (`Source/ShaderCache/`). Binding is by name from `.shaderdesc` (`NamedDescriptorWriter.h`).
- **Variants**: `PipelineVariantCache.cpp` (1579 lines), prewarm services, the offline `Tools/MaterialVariantCook` (per-mesh variant table plus WGSL cook).
- **Shader graph**: the graph file **is** a `.glsl` file whose topology lives in `// @sg-*` comments and whose regenerated body is the `EvaluateSurface()` code (`docs/rendering/shader-graph-spec.mdx`).
  - Nodes are annotated GLSL functions (`// @sgnode Add`, `@pure`, `@stage`) under `Shaders/Graph/Nodes/{Math,Color,Input,Motion,Normal,Sampling,Transform,Utility}`, 700 lines.
  - Compiler: `Source/ShaderGraph/` (SgTagParser 632, SgGraphCompiler 773, SgNodeReflector 179, SgGlslSignatureReader 167, SgPropertyBinding 408). It depends only on `MaterialDocument` and nlohmann::json.
  - Editor UI: `Apps/Editor/Source/ShaderGraph` plus `Panels/ShaderGraphPanel.cpp`, ~4.6k lines, written for their CSS/XML UI.
- **Tools**:
  - `Tools/ShaderCook/shadercook.py`: glslc → SPIR-V 1.1 → naga → WGSL, tint-validated, with golden tests.
  - `Tools/ShaderGen`: ACES2 table generator and validator.
  - `Tools/ShaderReflectHost`: a 23-line CMake stub.

### 1.9 Textures
- **Offline compression**: `Tools/TextureCompiler/TextureCompiler.cpp` (394 lines) encodes PNG/TGA/JPG to **KTX2 UASTC 4x4 + Zstd + full mips**, with sRGB/linear in the DFD, normal-map renormalisation per mip, and a `--decode` verify mode. Depends on libktx (Apache-2.0).
- **Runtime**: `TextureService.cpp` (3336 lines) transcodes KTX2 to **BC7, or RGBA32 when BC is unsupported** (`:193`); there is no ASTC/ETC2 path. It does async decode/upload and bindless registration, and includes `BC7TransparentEndpointRepair.cpp`.
- No general texture mip streaming. `Modules/PageStreaming` is terrain height pages only.

### 1.10 Debug and profiling
- Editor panels: RenderGraph (passes, resources, barriers, cull reasons), VRAM, Visual Profiler with per-pass GPU timestamps (`QueryPool.h`), Monitors (`Apps/Editor/Source/Panels/{RenderGraphPanel,VramPanel,VisualProfilerPanel}.cpp`).
- Shadow, DDGI and cluster debug overlays; env flags (`RenderingDebugFlags.h`); `docs/rendering/gpu-debugging-guide.mdx`.

---

## 2. What OURS has

### 2.1 Architecture
- **Backend**: a thin `engine/renders/sokol` layer (~4.5k lines): `SokolCmdQueue`, `SokolPipelineCache`, `SokolFramebuffer`, `SokolTexture`, `SokolShader`, `SokolObject`. It sits over sokol_gfx (GL 3.3 / GLES3 / WebGL2, Vulkan, Metal, D3D11) behind the abstract `engine/core/render/*Render.h` (`Render.h`, `PipelineState.h`, `FramebufferRender.h`…).
- **Renderer**: one monolithic forward renderer, `engine/core/subsystem/RenderSystem.cpp` (**15,396 lines**) plus `.h` (2080), with a hard-coded pass sequence: `drawFramePasses`, `renderShadowMaps`, `renderCameraPass`, `renderDepthPrePass`, `renderGBufferPass` (SSR only), `renderSSAO`, `renderSSR`, `renderPostProcess`, `renderUber`, `renderBloomChain`. There is no render graph and no data-driven pipeline.
- **Limits**: no compute shaders and no indirect draws. sokol has `sg_dispatch`, but not on GLES3/WebGL2, and has no indirect draw API at all.
- **Colour pipeline**: **LDR scene targets** (RGBA8/SRGB8A8; `SceneSettings.h` says "LDR: the scene target is 8-bit"). Tonemap and grade are baked into a CPU-built colour LUT applied at the end of every shader (tier T0).

### 2.2 Lighting and materials
- **Unified lighting**: any number of lights. Each object gets a CPU-built light list (`selectMeshLights` / `buildObjectLights`), capped at `maxLightsPerObject` 1..16 with compile tiers 4/8/16. The remainder is folded into SH ambient. Per-pixel, per-vertex or mixed shading per light (`docs/shaders/library.md#lighting-modes`, `engine/shaderlib/includes/lights.glsl`, `punctual.glsl`).
- **PBR**: glTF-Sample-Viewer metallic-roughness (`engine/shaderlib/includes/pbr.glsl`, `brdf.glsl`). **No clearcoat, sheen, anisotropy, iridescence, energy compensation or specular AA.**
- **2D**: 2D lights, normal maps, 2D shadow casters (`includes/lighting2d.glsl`, `shadow2d.*`). THEIRS has no 2D lighting.

### 2.3 Shadows
- An atlas holds directional cascades (`MAX_SHADOWCASCADES` 4) and spots. A point atlas uses cube faces packed in 2D (`ensureShadowAtlas`, `ensureShadowPointAtlas`). Filtering is hardware-compare PCF 1/3x3/5x5/7x7 (bilinear-weighted grid), radius from a uniform (`includes/shadows.glsl`).
- Slope bias only. The cascade fit is a **tight AABB per frame with no texel snap and no sphere fit** (`RenderSystem::getDirLightProjection`), so it shimmers when the camera moves.
- **Hard cascade switch** with no blend band and no distance fade (`shadowCascadedCalculationPCF`). No PCSS, no contact shadows, no caching.

### 2.4 Environment, GI and screen-space effects
- **IBL**: generated **on the CPU** from an LDR cubemap (`engine/core/texture/IBLEnvironment.cpp`: lambertian 16², GGX 128 with 8 mips), cached.
- **Probes and mirrors**: box reflection probes with one probe per renderable, static or dynamic capture (`ReflectionProbeComponent.h`, `updateReflectionProbes`). Planar mirrors (`MirrorComponent`, `createMirrorCamera`).
- **Sky**: cubemap skybox plus library skies (`procedural_sky`: gradient, sun disc and halo, 2D scrolling clouds; `gradient_sky`, `panoramic_sky`). There is no physical atmosphere.
- **Fog**: linear, exp and exp² distance fog only (`includes/fog.glsl`, `FogComponent`). No height fog and no volumetrics.
- **SSAO**: 32-tap hemisphere kernel plus blur (`shaderlib/ssao.frag`).
- **SSR**: linear march plus binary refine, requires the G-buffer prepass (`shaderlib/ssr.frag`, `gbuffer.*`).

### 2.5 Post-processing (mobile-first, `SceneSettings.h::PostProcessingSettings`)
- **T0, in-shader**: exposure; tonemap NONE/ACES (Narkowicz fit)/Khronos Neutral/Reinhard/AgX (minimal approximation); white balance, lift-gamma-gain, Unity-strip LUT; monochrome; vignette; dither.
- **T1, one uber pass**: FXAA 3.11, chromatic aberration, pixelate, CRT, Sobel edges, film grain, sharpen, render scale.
- **T2**: quarter-resolution dual-filter bloom and blur in LDR.
- **T3**: user post-process passes with library post shaders.
- Also MSAA 1/2/4 and fixed-resolution pixel-art upscale.
- **Not present**: TAA, SMAA, auto exposure, DoF (blur only), lens flare, motion vectors, dynamic resolution.

### 2.6 Shader system
- **Manifest comments** (`@shader`, `@property`, `@texture`, `@feature`, `@state`, `@require`, `@lighting`) parsed by `engine/core/shader/ShaderManifest.cpp` (1598 lines). A library of ~40 shaders (`engine/shaderlib/library/`: toon, matcap, water, glass, decals, dissolve, foliage, outline…). Built-in shaders can be forked.
- **Hooks**: `SURFACE_*` macros (`includes/surface_vs.glsl`). Scene depth and colour inputs for mesh shaders. Stencil and an extra material pass (outlines).
- **Compilation**:
  - Variants are compiled via glslang + SPIRV-Cross (`engine/libs/shadercompiler`, `engine/core/shader/ShaderVariantCompiler.cpp`).
  - The **runtime compiles variants a build lacks** (`RuntimeShaderCompiler.cpp`).
  - The editor collects variants (`editor/shader/ShaderVariantCollector.cpp`), builds a validation matrix on 6 backends (`editor/shader/ShaderLibraryMatrix.cpp`), generates docs (`ShaderLibraryDoc.cpp`), and has material presets (`editor/material/MaterialPresets.cpp`).
- **No shader graph.**

### 2.7 Geometry, textures and tooling
- **Geometry**: CPU frustum culling, instancing with CPU sort (`InstancedMeshComponent`, `InstanceSort.h`), simplified mesh LODs (`core/pool/MeshLodPool.cpp`) selected by screen error with no crossfade, terrain CDLOD/clipmaps, an optional depth prepass.
- **Textures**: RGBA8/R8/R16/RGBA16F/RGBA32F/R32F only (`Render.h:320`). **No GPU-compressed formats, no KTX/Basis**, even though sokol exposes BC/ETC2/ASTC pixel formats.
- **Debug**: `JASPERENGINE_FRAME_STATS` pass/draw logging, `RenderBackendTime.h` CPU-side backend timing, SSAO/SSR debug modes, a face-cull toggle. No GPU timestamps and no render graph view.

---

## 3. Gaps: THEIRS has, OURS lacks

Value is to us, given that our engine targets mobile, web and broad hardware with a sokol backend. Effort is for us to build or port.

| Feature | Their location | Value | Effort |
|---|---|---|---|
| Stable CSM: sphere fit + texel snap | `Source/Engine/Rendering/ShadowMapRenderFeature.cpp:179-190,545` | H | S |
| Cascade blend band + shadow distance fade | `Shaders/Includes/shadow_cascade_blend.glsl`, `shadow_distance_fade.glsl` | H | S |
| Receiver-plane / normal-offset bias, Vogel-rotated Poisson PCF | `Includes/shadow_receiver_plane.glsl`, `shadow_sampling.glsl:235` | M | S |
| PCSS / DPCF soft shadows | `Includes/shadow_sampling.glsl:448-600`, `ShadowMinMaxPyramid.cpp` | M | M |
| Static shadow caching | `CascadeShadowCache.cpp`, `PointShadowAtlasPlanner.cpp` | M (mobile perf) | M |
| Extended PBR lobes (coat, sheen, aniso, iridescence, SSS, multiscatter) | `Includes/standard_pbr.glsl`, `iridescence.glsl`, `Surfaces/standard_pbr_extended.glsl` | H | M |
| Specular AA | `Includes/specular_aa.glsl` | M | S |
| Height fog (analytic) | `Modules/HeightFog`, `Includes/height_fog_term.glsl` | H | S-M |
| GPU texture compression (KTX2/UASTC → BC7/ASTC/ETC2) | `Tools/TextureCompiler`, `TextureService.cpp:193,1504` | H | M |
| HDR scene pipeline (RGBA16F + post tonemap) | whole post chain | H (desktop) | L |
| Physical atmosphere sky (Hillaire LUTs) + GPU IBL prefilter | `Shaders/sky_*`, `Source/Sky/SkyRenderer.cpp`, `ImageBasedLightingFeature.cpp` | H | M-L |
| Shader graph | `Source/ShaderGraph/*`, `Shaders/Graph/Nodes/*`, `docs/rendering/shader-graph-spec.mdx` | H | L (compiler M, ImGui UI L) |
| Volumetric clouds | `Modules/VolumetricClouds` | M | M |
| Volumetric froxel fog | `Modules/VolumetricFog` | M | L (compute + 3D storage images, not on GLES/WebGL2) |
| Clustered Forward+ | `clustered_light_cull.comp`, `Includes/clustered_lighting.glsl` | M | L (needs SSBO or texture-encoded clusters for GLES) |
| GTAO with bent normals | `Shaders/AmbientOcclusion/` | M | M (compute → fragment) |
| Screen-space contact shadows | `Shaders/ScreenSpaceShadows/screen_space_shadow.comp` (Apache-2.0) | M | M |
| SMAA 1x (LUT-free) | `smaa_edges/weights/blend.comp` | M | S-M |
| TAA / TAAU + motion vectors + jitter | `taa_resolve*.comp`, `taa_motion_vectors*`, `ViewMotionVectors.cpp` | M | L |
| Auto exposure (histogram) | `auto_exposure_*`, `AutoExposureNode.cpp` | M (needs HDR) | M |
| HDR multi-octave bloom + lens dirt | `bloom_octave_gather.frag`, `bloom_lens_dirt*.frag` | M | S-M |
| Physical DoF | `Packages/fidelityfx-dof` | L-M | M |
| Lens flare / sun glare | `LensFlareRenderNode.cpp`, `sun_glare*.frag` | L-M | M |
| `.cube` LUT import | `Source/Common/CubeLutFileParser.cpp` | M | S |
| ACES2 / full AgX / HDR display output | `tonemap.frag`, `TonemapAces2Tables.h`, `MetalHdrOutput.cpp` | M | S (bake ACES2 into our CPU LUT); HDR output L |
| Photometric units + Kelvin | `Include/Components/Rendering/LightPhotometry.h` | M | S |
| Solar path (latitude/day/hour) | `Include/Rendering/Sky/SolarPath.h`, `Source/Sky/SolarPath.cpp` | L-M | S |
| Dynamic resolution controller | `DynamicResolutionController.cpp` (pure) | M (mobile) | S-M |
| Extra stylised post (VHS, film sim, CAS, halation, heat distortion) | `vhs*.frag`, `Packages/film-simulation`, `cas.frag` | L-M | S each |
| Data-driven pipeline / JSON post stack | `ForwardPlus.rendergraph`, `RenderPipeline.cpp` | M | L |
| Render graph | `Core/RenderGraph/*` | L-M | XL |
| GPU-driven culling / HZB / indirect | `GPUScene/GPUCulling/GPUDrawStreamBuilder`, `hzb_culling.comp` | L | XL (sokol has no indirect draw) |
| DDGI / RT shadows | `DDGI*`, `RTShadowMaskService.cpp` | L | XL |
| Stochastic SSR | `ScreenSpaceReflections/sssr_*` | L-M | L |
| LOD crossfade (dither) | `Includes/lod_crossfade.glsl` (concept only) | M | S-M (own impl.) |
| GPU timestamps + render debug panels | `QueryPool.h`, `RenderGraphPanel.cpp`, `VramPanel.cpp` | M | M (sokol lacks timestamp queries; needs backend patches) |
| Parallax occlusion mapping | `Includes/parallax_occlusion.glsl` | M | S |

---

## 4. Things THEIRS does better, and what OURS does better

### THEIRS better (evidence)
1. **Shadow stability and quality.**
   - Our cascade fit re-fits a padded AABB every frame (`RenderSystem::getDirLightProjection`) and switches cascades on a hard `nearFar` test (`shadows.glsl::shadowCascadedCalculationPCF`), so shimmer and visible seams are guaranteed.
   - Theirs snaps the clip translation to texels (`ShadowMapRenderFeature.cpp:183`), fits bounding spheres, blends cascades over 17.5% of each range, fades out at max distance, and adds receiver-plane bias plus five filter families.
2. **PBR completeness.** They cover the KHR_materials_* lobe set (coat, sheen, anisotropy, iridescence, transmission) plus energy compensation and specular AA. Ours is core glTF metallic-roughness, so imported glTF assets using those extensions render wrong.
3. **HDR colour pipeline.** Bloom, exposure and tonemap run on real HDR values: an 8-octave bloom chain, histogram exposure, ACES2. Ours blooms an 8-bit target ("a full-white surface blooms like a light"), and our ACES is the Narkowicz fit.
4. **Atmosphere, fog and volumetrics.** Physical sky with LUTs, stars and moon, height fog, froxel fog, raymarched clouds. Ours is a gradient sky and distance fog.
5. **Texture memory.** UASTC KTX2 + Zstd with runtime BC7 transcode. We upload uncompressed RGBA8 everywhere, which hurts most on mobile and web where it matters.
6. **Authoring scale.** Shader graph; post-process volumes; a JSON pipeline where adding a fullscreen effect needs no C++; generic Fullscreen/Compute nodes.
7. **Observability.** RG panel with cull reasons, per-pass GPU timings, VRAM panel, extensive shadow and cluster debug modes. A test suite exists for the RG (`Engine/Modules/Rendering/Tests/RenderGraph/`).
8. **Light scalability.** Clustered culling handles hundreds of lights per pixel. We cap at 16 per *object* and fold the rest into SH.

### OURS better (be fair)
1. **Platform reach.** GL 3.3/GLES3/WebGL2/D3D11/Metal/Vulkan, with Android, iOS and HTML5 exports. THEIRS needs Vulkan, Metal or WebGPU (bindless-class or compat compute) and has no mobile target; DX12 lags.
2. **Mobile-first cost model.** T0 colour runs in-shader with no pass. One uber pass. A quarter-resolution bloom. Every effect costs zero when off, and there is a per-tier cost readout (`RenderSystem::getPostProcessingTiers`). THEIRS' ForwardPlus graph declares ~100 passes, including 16 bloom/scatter downsample passes.
3. **Simplicity and footprint.** ~15k lines of renderer plus ~4.5k of backend against ~240k. Our renderer can be understood by one person.
4. **Shader library UX.** ~40 production shaders (toon, water, glass, decals, dissolve, foliage, outlines with stencil extra pass, matcap, soft particles, heat haze). Manifest-driven inspector properties. A six-backend compile matrix validated from the CLI (`jasperengine-editor shaders --library all --backend all`). Generated docs. Runtime compilation of missing variants. THEIRS ships fewer authored surfaces (8).
5. **2D rendering.** 2D lights, normal-mapped sprites, 2D shadow casters, tilemaps, polygons. THEIRS has essentially no 2D lighting.
6. **Per-light pixel/vertex/mixed lighting modes and MSAA by default.** These suit low-end devices. Planar mirrors and a scene-colour/depth API for mesh shaders also make many effects cheap.
7. **Mesh LOD generation in-engine** (`MeshLodPool`) with a screen-error threshold.

---

## 5. Copy/port candidates

General porting notes:
- THEIRS GLSL is Vulkan-style 450: `layout(set=,binding=)`, `push_constant`, `std430` SSBOs, combined `sampler2D`, reverse-Z, LH coordinates.
- Ours is sokol-style GLSL 450 cross-compiled by glslang + SPIRV-Cross: separate `texture2D` + `sampler`, `uniform u_fs_* {}` blocks, no SSBOs on GLES3/WebGL2, standard depth.
- Every shader port therefore needs: the binding rewrite, push constants turned into a uniform block, SSBOs replaced by uniforms or textures, and reverse-Z depth comparisons and reconstruction inverted.
- Compute (`.comp`) must become fragment passes to work on GL/GLES/WebGL2.
- **Licence**: core THEIRS code is MIT. Attribute in NOTICE.md: "OpenEngine, Copyright (c) Game-Crafters-Guild contributors, MIT", plus the original upstream for third-party-derived files as listed.

### Tier A: high value, small or medium effort

1. **Stable cascaded shadows.**
   - Source:
     - `Engine/Source/Engine/Rendering/ShadowMapRenderFeature.cpp`: `SnapClipTranslationToTexels` (`:179-190`), `SliceBoundingSphereRadius` (`:545+`), extent snap bands (`:78-110`), split lambda.
     - `Engine/Modules/Rendering/Shaders/Includes/shadow_cascade_blend.glsl` (31 lines), `shadow_distance_fade.glsl` (33), `shadow_receiver_plane.glsl` (31).
   - Dependencies: none beyond the math. Re-express it on our `Matrix4` in `RenderSystem::getDirLightProjection` / `computeLightCameras`. In `engine/shaderlib/includes/shadows.glsl`, sample cascade c and c+1 inside the band and `mix`. Add a `shadowFadeFraction` uniform to the existing lighting block.
   - Adaptation: no reverse-Z issue for this math (clip-space XY only). Watch the HLSL loop-unroll cap already documented in `shadows.glsl` (blend doubles the taps).
   - Licence: MIT (OpenEngine).
2. **Extended PBR lobes + specular AA.**
   - Source: `Shaders/Includes/standard_pbr.glsl` (functions `GE_D_GGX_Aniso`, `GE_V_SmithGGX_Aniso`, `GE_FresnelF82`, `GE_MultiscatterCompensation`, `GE_CharlieLobe`, `GE_CharlieDirectionalAlbedo`, `GE_CoatDarkening`, `GE_SubsurfaceTransmission`), `iridescence.glsl`, `specular_aa.glsl`.
   - Dependencies: `surface_io.glsl` (the `SurfaceOutput` struct) and `compat_profile.glsl`. Map those fields onto our `pbr.glsl` material inputs. Add `@feature CLEARCOAT/SHEEN/ANISOTROPY/IRIDESCENCE` variants to the built-in Lit manifest so they cost nothing when off.
   - Also wire `KHR_materials_clearcoat/sheen/anisotropy/iridescence/specular/ior` in our glTF loader.
   - Adaptation: pure ALU, so it works on all backends. Sheen needs the Charlie E-LUT or their analytic albedo fit (they use an analytic fit, no LUT). Multiscatter needs our BRDF LUT (we already have one for IBL).
   - Licence: MIT.
3. **Analytic height fog.**
   - Source: `Engine/Modules/Rendering/Shaders/Includes/height_fog_term.glsl` (377 lines), `height_fog_params_fields.glsl` (72), C++ params in `Engine/Source/Engine/Rendering/HeightFogParamsUBO.cpp` (131), presets in `Engine/Modules/HeightFog/Source/HeightFogEffectPresets.cpp`.
   - Dependencies: `sky_composite.glsl`, only for the sky-backdrop colour source. Stub `GE_HEIGHT_FOG_BACKDROP` with our fog colour or the skybox cubemap.
   - Integrate as fog type 3 in `engine/shaderlib/includes/fog.glsl`, evaluated per fragment in forward shaders (that is what their `forward_fog.glsl` does). Add a sky-fill term to our sky shaders.
   - Adaptation: pure ALU. World-space height axis; direction and distance come from the camera position, which we already have in `litFrame`.
   - Licence: MIT (OpenEngine), derived from SanielX/Height-Fog (MIT); keep `Engine/Modules/HeightFog/ThirdParty/SanielX-Height-Fog/UPSTREAM.md` attribution.
4. **KTX2/UASTC texture pipeline.**
   - Source: `Tools/TextureCompiler/TextureCompiler.cpp` (394 lines: encoder options, per-mip normal renormalise, DFD transfer, decode-verify), `Engine/Source/Engine/Rendering/TextureService.cpp:191-195,1474-1510` (transcode-target selection), `Engine/Source/Assets/Textures/BC7TransparentEndpointRepair.cpp` (109).
   - Dependencies: libktx (Apache-2.0, bundles basisu, Apache-2.0) and zstd (BSD). Their `Image/*` decode and resample modules would be swapped for our `TextureData` / stb.
   - We must add **ASTC 4x4 and ETC2** transcode targets (mobile/web), which THEIRS lacks, and map them to `SG_PIXELFORMAT_ASTC_4x4_RGBA`, `SG_PIXELFORMAT_ETC2_RGBA8` and `SG_PIXELFORMAT_BC7_RGBA` via `sg_query_pixelformat`. Add the cook to the editor export, not to runtime.
   - Licence: MIT tool code; libktx and basisu Apache-2.0 (compatible, NOTICE required).
5. **Shader graph compiler, with a node library in our manifest style.**
   - Source: `Engine/Modules/Rendering/Source/ShaderGraph/{SgTagParser,SgGraphCompiler,SgNodeReflector,SgGlslSignatureReader,SgPropertyBinding,SgPseudoNodes,SgTypes,SgGraphFileIO}.cpp` (~2.5k lines), headers in `Include/Rendering/ShaderGraph/`, nodes `Shaders/Graph/Nodes/**/*.glsl` (700 lines), spec `docs/rendering/shader-graph-spec.mdx`.
   - Dependencies: `MaterialDocument` / `MaterialPropertyBinding` (replace with our `ShaderManifest` property model) and nlohmann::json (we have yaml-cpp; json is header-only and fine). There are platform `#include <windows.h>` / `mach-o/dyld.h` lines for exe-dir lookup; drop them.
   - The "graph file is the GLSL file, topology in `// @sg-*` comments" design fits our `// @shader` / `@property` manifest convention exactly. The emitted body maps to our `SURFACE_*` hook macros instead of `EvaluateSurface()`.
   - UI: THEIRS' editor UI (`Apps/Editor/Source/ShaderGraph`, CSS/XML) is **not** portable; build ours in ImGui (e.g. imgui-node-editor, MIT).
   - Licence: MIT.

### Tier B: valuable, medium effort or narrower

6. **Physical sky + GPU IBL prefilter.**
   - Source: `Shaders/sky_transmittance_lut.comp`, `sky_multiscatter_lut.comp`, `sky_view_lut.comp`, `sky_render.frag`, `Includes/sky_*.glsl`, `sky_specular_prefilter.comp`, `sky_diffuse_convolve.comp`, `Source/Sky/SkyRenderer.cpp` (803), `AtmosphereTransmittance.cpp`, `SolarPath.cpp`, `StarCatalog.cpp`.
   - Convert the LUT compute shaders to fragment passes into 2D RGBA16F targets. RGBA16F rendering on WebGL2 needs `EXT_color_buffer_float`, so offer an RGBA8-encoded fallback or bake the LUTs on CPU once per sun change. Their sky-view LUT format is RGBA32F on desktop and RGBA16F in compat (`sky_view_lut.comp:9-14`).
   - The GPU prefilter would replace our CPU `IBLEnvironment::generate` for dynamic time of day, rendered to cubemap faces via sokol attachments.
   - Licence: MIT.
7. **Volumetric clouds.**
   - Source: `Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_march.frag` (fragment; usable as is after the binding rewrite), `_resolve.frag`, `_composite.frag`, `volumetric_clouds_worley.glsl`.
   - Bake the 3D Worley noise (their `_shape_noise.comp` / `_detail_noise.comp`) on CPU at import, or ship it as a 3D texture asset. sokol supports `SG_IMAGETYPE_3D`, WebGL2 included.
   - The min/max SSBOs become 2 uniforms.
   - Licence: MIT; derived from SebLague/Clouds (MIT, `ThirdParty/SebLague-Clouds/LICENSE`).
8. **SMAA 1x LUT-free.**
   - Source: `Shaders/smaa_edges.comp` (70), `smaa_weights.comp`, `smaa_blend.comp`, `Pipeline/Nodes/SmaaNode.cpp` (341).
   - Rewrite as 3 fragment passes (image stores become render-target writes). No area or search textures needed, which is ideal for us. Add it as a T1 or T2 option next to FXAA.
   - Licence: MIT (their own LUT-free implementation of Jimenez 2012).
9. **`.cube` LUT parser.**
   - Source: `Engine/Modules/Rendering/Source/Common/CubeLutFileParser.cpp` (387) + header, plus optionally `CubeLutColorDomain.cpp`.
   - Dependencies: std only. Feed the parsed 3D LUT into our `bakePostColorLut` (resample to our 16 or 32 strip).
   - Licence: MIT.
10. **Tonemap upgrades baked into our CPU LUT.**
    - Source: full AgX with the outset matrix from `Shaders/tonemap.frag:60-100`, and ACES 2.0 from `Shaders/Includes/tonemap_aces2.glsl` + `Source/Passes/TonemapAces2Tables.h`.
    - Because our tonemap is baked on CPU into the colour LUT, ACES2's per-pixel cost disappears. Port its math to C++ in `bakePostColorLut`.
    - Licence: ACES2 code and tables are **BSD-3-Clause (OpenColorIO)**: compatible, keep the copyright notice and `ThirdParty/OpenColorIO/LICENSE.txt`. AgX is MIT (MinimalAgX). Khronos Neutral is Apache-2.0 (we already have our own Neutral).
11. **Photometry + solar path.**
    - Source: `Engine/Include/Components/Rendering/LightPhotometry.h` (header-only; drop the `Light.h` enum include) and `Engine/Modules/Rendering/{Include,Source}/Rendering/Sky/SolarPath.*`.
    - Effort S. Pairs with item 6.
    - Licence: MIT.
12. **Dynamic resolution controller.**
    - Source: `Engine/Source/Engine/Rendering/DynamicResolutionController.cpp` + `.h`. Pure: cost samples in, scale out. Dependencies: `Types/Types.h` only.
    - Drive our existing `renderScale` from CPU frame time, since sokol has no GPU timestamps.
    - Licence: MIT.
13. **GTAO.**
    - Source: `Shaders/AmbientOcclusion/gtao.comp`, `gtao_sectors.glsl`, `gtao_geometry.glsl`, `gtao_blur.comp`, `gtao_upsample.comp`.
    - Port the sweep to a half-resolution fragment pass replacing `shaderlib/ssao.frag`. Temporal is optional (we have no history buffers). Needs view normals: ours come from the SSR G-buffer or a depth reconstruction.
    - Licence: MIT (Panos Karabelas / SpartanEngine header retained in the file, `ThirdParty/GTAO`).
14. **Contact shadows.**
    - Source: `Shaders/ScreenSpaceShadows/screen_space_shadow.comp` + `Source/Engine/Rendering/ScreenSpaceShadows/*`.
    - Compute with wave-aligned dispatch, so a fragment port loses some efficiency.
    - Licence: **Apache-2.0 (Sony/Bend)**: compatible with MIT distribution, but keep the licence and NOTICE and mark modifications.
15. **Stylised post as `library:` post shaders.**
    - Source: `Shaders/vhs*.frag` (VHS: punikonta, MIT), `Packages/film-simulation/Assets/Shaders/*.frag` (MIT, FidelityFX-derived grain), `Packages/chromatic-aberration/...frag` (MIT), `cas.frag` (63 lines), `halation_*.frag`, `heat_distortion.frag`.
    - They drop into our T3 custom-pass system with only the binding rewrite. Effort S each.
16. **Bloom lens dirt.**
    - Source: `bloom_lens_dirt.frag`, `bloom_lens_dirt_gather.frag`.
    - Composite in our uber pass.
    - Licence: MIT (SE Natural Bloom, KinoBloom).

### Flagged licences in THEIRS rendering scope
- **Apache-2.0**: `ThirdParty/BendScreenSpaceShadows`, `ThirdParty/KhronosPbrNeutral`, `ThirdParty/metal-cpp`.
- **BSD-3-Clause**: `ThirdParty/OpenColorIO` (ACES2 tables and shader).
- **MPL-2.0**: `ThirdParty/RunevisionErosionFilter` (terrain; file-level copyleft, so do not copy into our MIT files).
- **Unlicense**: GPUFogParticles (MirzaBeig).
- **External tools**: naga (MIT/Apache) and tint (BSD-3), fetched binaries.
- Everything else listed under `ThirdParty/` (FidelityFX-Denoiser, GTAO, KinoBloom, MinimalAgX, OCASMDepthVeil, PunikontaVhs, SENaturalBloomDirtyLens, SpeedballGi) is MIT.

---

## 6. Not worth copying (and why)

- **Render graph** (`Core/RenderGraph/*`, ~5k lines): built around Vulkan layouts, barriers, queues and timeline semaphores, all of which sokol already hides. Copy the *ideas* instead:
  - a declared pass list with cull-by-reachability;
  - a debug view of passes and cull reasons.
  A light pass list on top of our `drawFramePasses` gets 80% of the value.
- **GPU-driven pipeline** (GPUScene, GPUCulling, GPUDrawStreamBuilder, HZB two-phase, bindless, `draw_command_scatter.comp`): sokol has **no indirect draw**, and GLES3/WebGL2 have no compute. That is an XL rewrite with zero reach on our mobile/web targets. Our CPU culling plus instancing is right for our scale.
- **DDGI** (`DDGIProbeFeature.cpp` plus ~20 shaders) and **RT shadow mask**: they need ray query or a software BVH, compute and bindless. XL. A baked lightmap or SH-probe solution suits our targets better.
- **Stochastic SSR** (13 compute shaders, temporal): heavy for mobile, and it depends on TAA history. Our linear SSR is adequate. Improve it with Hi-Z later if needed.
- **Froxel volumetric fog**: compute plus 3D storage images, unavailable on GLES/WebGL2. Height fog (5.A.3) plus clouds (5.B.7) give most of the look.
- **Backends** (Vulkan/Metal/DX12/WebGPU): we use sokol. The DX12 backend is incomplete anyway.
- **ShaderCook / WGSL pipeline**: sokol's WebGPU path, if we ever enable it, comes through our own glslang + SPIRV-Cross / sokol-shdc toolchain. Their naga/tint pinning is specific to wgpu-native.
- **MaterialVariantCook, PipelineVariantCache, `.shaderpkg`**: our `ShaderVariantCollector` + `RuntimeShaderCompiler` + `.sdat` cache already cover the same need, and fit sokol.
- **Shader graph editor UI** (`Apps/Editor/Source/ShaderGraph`): it is tied to their CSS/XML UI framework. Rebuild it in ImGui.
- **CascadeShadowCache full logic**: its invalidation keys are tied to GPU LOD-selection state and the RG pool identity. If we want static shadow caching, write a simpler "caster version + light VP hash" check in our renderer.
- **Planet-scale camera-relative sectors** (`RenderOrigin.h`): niche for our 2D/3D indie target.
- **LOD crossfade code**: it is packed into the GPU draw-stream word. Implement our own dithered crossfade in `selectMeshLod` with two draws instead.
