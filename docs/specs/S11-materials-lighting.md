# S11 — Materials & lighting

Wave 2. Extended PBR lobes with their glTF extensions, specular anti-aliasing, direct-light
energy compensation, photometric light units with Kelvin colour temperature, and hex tiling
(anti-repetition) for terrain layers and standard materials.

## 1. Goal

glTF assets that use `KHR_materials_*` (car paint, fabric, brushed metal, glass, soap films)
should render close to the Khronos reference. Today they fall back to plain metallic-roughness.
Highlights should stop shimmering on dense normal maps. Lights should be authorable in lux,
candela, lumen and Kelvin. Large tiled terrain and floors should not show a visible grid. Each
feature is a compile-time variant or CPU-side conversion that costs **exactly nothing** when
it is unused, so existing scenes, forks and exported games stay byte-for-byte as they are.

## 2. Scope

**In scope**
- Built-in Lit (`engine/shaderlib/mesh.frag`) lobes: clearcoat (with coat normal), sheen,
  anisotropy, iridescence, transmission + volume (thickness and attenuation), specular + IOR,
  and emissive strength. They are loaded from `KHR_materials_clearcoat`, `_sheen`,
  `_anisotropy`, `_iridescence`, `_transmission`, `_volume`, `_specular`, `_ior`,
  `_emissive_strength`. `KHR_materials_unlit` is also loaded (it maps to Lighting = None).
- Direct-light multiscatter energy compensation and geometric specular AA, as a scene "Lit
  Quality" tier.
- Photometric units (Engine/unitless, Lux, Candela, Lumen) and colour temperature on
  `LightComponent`. Conversion happens on the CPU only.
- Hex tiling: per terrain PBR layer, and per built-in Lit material.
- Inspector UX, Lua, MCP, docs, material presets, golden images with vendored Khronos
  sample assets.

**Out of scope**
- OpenEngine's OpenPBR-only extras: F82 edge tint, Oren-Nayar diffuse, coat darkening dial,
  fuzz, DICE SSS, and `KHR_materials_diffuse_transmission` / `_dispersion`. They are possible
  follow-ups.
- `KHR_texture_transform` (the loader warns and ignores it).
- `KHR_lights_punctual` import. This is a follow-up, made trivial by R14.
- HDR scene targets and a mip-chained transmission buffer.
- Lobes in library shaders (`library:*`) and in the G-buffer/SSR path.
- Their material system (`MaterialBuildService`, Slang lane, shader graph,
  `PipelineVariantCache`, `.shaderpkg`): see analysis 01 §325. We port **shader math only**,
  never their `SurfaceOutput` adapter architecture or bindless taps.
- Nothing here touches the do-not-copy list (README). CBT terrain is not copied: we take only
  the hex-lattice math from `terrain_material_albedo.glsl`, not the CBT terrain.

## 3. Current state (verified in code)

- **Material model.** `engine/core/texture/Material.h`, `struct Material`. It has
  base/metal/rough/emissive/occlusion/normal and `foliageTransmission`. The "shader part"
  prefix (`baseColorFactor` … `foliageTransmission`, 12 floats) is memcpy'd as the fragment
  UBO: `RenderSystem.cpp:7507` and `:6808`,
  `applyUniformBlock(slotFSParams, sizeof(float) * 12, &material)`. Materials are stored
  **inline, 16 per `MeshComponent`** (`HybridArray<Submesh, MAX_SUBMESHES>`,
  `MeshComponent.h:312`, `Engine.h:81`). Every byte added to `Material` is paid 16× per mesh.
  `MaterialExtraPass` (Material.h) is the existing out-of-line value pattern for that reason.
- **Override flags.** `SubmeshOverrideFlags : uint32_t` (`MeshComponent.h:35-75`) uses bits
  0–29. `Submesh::overrideFields` is `uint32_t` (`:254`), serialized as a number
  (`editor/Stream.cpp:1932,1971`).
- **Shading.** `mesh.frag` is glTF-Sample-Viewer metallic-roughness:
  - `f0_ior` is the constant 0.04 (`mesh.frag:248`), and f90 is the reflectance×50 hack
    (`:272`).
  - There is no clearcoat, sheen, anisotropy, iridescence, transmission or specular AA
    (`includes/pbr.glsl`, `includes/brdf.glsl`).
  - IBL uses the **analytic** Karis/Lazarov DFG fit (`includes/ibl.glsl:7-16`, no LUT
    texture). It **already** applies Fdez-Agüera multiscatter to the ambient term
    (`ibl.glsl:88-110`).
  - Correction to analysis 01 §235: we have no BRDF LUT. Only the *direct* light loop lacks
    energy compensation.
- **Variant space is full.** Mesh variant properties are a `uint32_t`, and bits 0–31 are
  assigned (`ShaderVariantCompiler.cpp:362-395`, `ShaderPool::getShaderPropertyName`). Bit 5
  is the only free bit ("reserved, was USE_SHADOWS_PCF"). `ShaderKey = customId<<48 |
  type<<32 | properties` (`ShaderPool.cpp:702-704`).
  (Note, 2026-11-04, BF23 F26: no longer so. Commit d20c7ee61 widened the properties to 64 bits
  for every shader type, and `ShaderKey` is a struct of properties, type and customId.)
  - Features of library shaders and forks travel in the **customId** reference
    `"<base>#DEF1,DEF2"` (`ShaderReference`, `ShaderManifest.cpp:1256-1357`). It is resolved
    to defines in `ShaderVariantCompiler::resolveCustomShaderSources` (`:803-845`).
  - The built-in Lit is customId 0 and takes no features: `getColorVariantReference("")`
    returns `""`.
- **Texture slots.** GLES3/WebGL2 guarantee 16 fragment textures. The worst-case non-terrain
  Lit variant binds 13:
  - 5 PBR maps;
  - SSAO;
  - 3 IBL cubes (`mesh.frag:128-140`);
  - spot-mask atlas;
  - shadow atlas + point atlas (`includes/lights.glsl:99-132`);
  - post-colour LUT (`includes/post_color.glsl:55`).

  A painted terrain + post colour is already at 16 (`mesh.frag:68-74` drops its emissive map).
  Per-texture UV sets are `u_fs_texCoordSets`, and `set1.yzw` are unused
  (`includes/pbr.glsl:24-36`).
- **glTF loader.** tinygltf (`engine/libs/tinygltf`). The material read is in
  `MeshSystem.cpp:4757-4815`: core fields, plus one custom extra
  (`jasperengine_foliage_transmission`). **No `KHR_*` extension is read**: grep finds none
  outside glslang.
- **Lights.** `engine/core/component/LightComponent.h` has `color` and unitless `intensity`.
  Shaders use KHR_lights_punctual attenuation (1/d², `includes/punctual.glsl`). Intensity is
  read raw in several places: light ranking `RenderSystem.cpp:1023`, the upload `:1494-1509`,
  and the main light `:3563-3573`. There is no unit or temperature.
- **Exposure.** There is no HDR target. `PostProcessingSettings::exposure` (stops) and tone
  mapping are baked into the in-shader colour LUT (T0, `SceneSettings.h:59-72`,
  `util/PostColorLut.h`). Output is 8-bit.
- **Terrain.**
  - `includes/terrain_fs.glsl`: 9 layers from 3 blend maps, one `texture2DArray`, explicit
    `textureGrad` gradients, triplanar sides, and a distance-faded second tiling rate
    (`:18-23`, `:116-127`).
  - The `u_fs_terrainLayers.slices[i].w` slot is unused (`:42`). `TerrainSurfaceLayer` is in
    `TerrainComponent.h:74-113`.
  - There is no hex/stochastic tiling.
- **Field fan-out.** One material field touches about 10 files: `Material.h`, `MeshSystem.cpp`
  (load + `applySubmeshOverride`), `editor/Stream.cpp:4022/4097`, `editor/Catalog.cpp`
  (property table + override map), `editor/window/InspectorWindow.cpp:6795-6808`,
  `editor/material/MaterialChange.cpp:63`, `editor/mcp/McpValues.cpp:1411-1479`,
  `editor/mcp/tools/AssetTools.cpp:517-611`, `editor/Factory.cpp:1157` (exported C++), and
  `engine/core/script/binding/CoreClassesLua.cpp:1236-1260`.
- **Tooling that exists:**
  - `jasperengine-editor shaders --library all --backend all` (`editor/shader/ShaderLibraryMatrix.cpp`);
  - `ShaderVariantCollector` (export precompilation);
  - `jasperengine-editor benchmark --project --scene`;
  - `tools/mcp/pngdiff.py` (golden comparison);
  - `editor/material/MaterialPresets.cpp`.

## 4. Reference (OpenEngine, MIT; Khronos, Apache-2.0)

| Source | Take | Improve / differ |
|---|---|---|
| `external/OpenEngine/Engine/Modules/Rendering/Shaders/Includes/specular_aa.glsl` (91 lines) | `GE_SpecularAAFloor`, `GE_NormalVarianceKernel` (Tokuyoshi-Kaplanyan, variance 0.5, threshold 0.18), the coat variant | Call it once, before the light loop, in uniform control flow (D3D11/GLES derivative safety) |
| `.../Includes/standard_pbr.glsl` (462) | `GE_D_GGX_Aniso`, `GE_V_SmithGGX_Aniso`, `GE_CharlieL/Lambda/VisCharlie/CharlieLobe`, **`GE_CharlieDirectionalAlbedo`** (analytic, no LUT), `GE_MultiscatterCompensation` + hoisted per-pixel factor | Layering and Fresnel follow the **KHR spec formulas**, not OpenPBR (no F82, no coat-darkening dial) |
| `.../Includes/iridescence.glsl` (94) | Belcour-Barla thin-film Fresnel, nm parameterisation, 30 nm fade | Force `highp` in the include (mobile) |
| `.../Surfaces/standard_pbr_extended.glsl` (137) | Reference for parameter plumbing only | Our inputs come from `Material` + `u_fs_pbrExt`, not their `Mat.uParams*` |
| `.../Includes/terrain_material_albedo.glsl:203-316` | `CBT_HexTriangleGrid` (colour-slotted simplex), `CBT_HexColor`, `CBT_HexRot`, `CBT_HexCenter`, `CBT_HexTapUV`, falloff 0.6 / exp 7, and the "block compiled as both GLSL and C++" test idea | Replace the `fract(sin)` hash with an integer (PCG2D) hash (fp32-stable for large ids on mobile). Pass **analytic gradients** (`R·dUV`) to `textureGrad`, which removes their mip caveat |
| `external/OpenEngine/Engine/Include/Components/Rendering/LightPhotometry.h` (117) | `KelvinToLinearRGB` (Tanner-Helland, max-channel normalised), lumen→candela via 4π, the forward/inverse pair for unit changes | **Not** their 203-nit anchor: we use EV100 pre-exposure (§6.5) because our pipeline is 8-bit with T0 exposure |
| Khronos glTF-Sample-Renderer (Apache-2.0; our `pbr.glsl`/`brdf.glsl`/`ibl.glsl` already derive from it) | KHR lobe formulas: clearcoat layering, sheen albedo scaling, specular/IOR f0/f90, iridescence substitution, transmission/volume | Texture-free substitutes: Charlie albedo fit instead of the sheen E-LUT, the GGX-prefiltered env at sheen roughness instead of a Charlie cube, scene-colour copy instead of the transmission framebuffer |

**Licences.**
- OpenEngine files carry no per-file header. Ported includes get
  `// Adapted from OpenEngine (MIT), Copyright (c) 2026 Gamecrafters Guild Limited and contributors`.
- Khronos-derived code gets the Apache-2.0 notice.
- Both get `NOTICE.md` entries.
- The Mikkelsen JCGT 2022 algorithm itself is unencumbered.

## 5. Requirements

### Functional
- **R1 Data model.**
  - `Material` gains `float emissiveStrength = 1` (outside the UBO prefix). The upload scales
    `emissiveFactor` by it in a 12-float stack copy.
  - It also gains `MaterialPbrExtensions` held **out of line** (§6.1). The prefix layout and
    offsets are unchanged.
  - Each lobe has an explicit `enabled` flag. Animating a factor to 0 never switches variant.
- **R2 Lobes.** Pixel and mixed lighting on the built-in Lit (and forks that implement the
  hooks) evaluate, for every pixel light and for IBL/ambient:
  - **Clearcoat:** factor, roughness, optional coat normal. Base attenuated by
    `1 − clearcoat·F_c`.
  - **Sheen:** colour, roughness. Charlie lobe, with base scaled by `1 − max(sheenColor)·E(NdotV)`
    using the analytic albedo.
  - **Anisotropy:** strength, rotation, optional direction map. Anisotropic GGX for direct
    light, bent reflection vector for IBL.
  - **Iridescence:** factor, IOR, thickness min/max in nm, optional thickness map. Replaces the
    Fresnel in direct and IBL.
  - **Transmission:** factor. Diffuse reduced by `1 − transmission`; the refracted scene-colour
    copy is added, blurred by roughness with ≤ 5 taps.
  - **Volume:** thickness, attenuation colour/distance. Beer-Lambert tint and IOR refraction
    offset; requires transmission.
  - **Specular/IOR:** `f0 = min(((ior−1)/(ior+1))²·specularColor, 1)·specular`,
    `f90 = specular`.

  Vertex and None lighting ignore lobes (no defines are emitted).
- **R3 Lit Quality.** `SceneSettings::litQuality` is `STANDARD` (today's shader exactly) or
  `HIGH`. HIGH adds geometric specular AA (base + coat roughness) and direct-light multiscatter
  compensation, through the one global mesh bit 5 (`USE_LIT_QUALITY`, short name `Lqh`).
  - A scene file without the key loads as STANDARD.
  - `SceneSettings()` defaults to HIGH, so new scenes get it.
  - `editor/Factory.cpp` writes the value explicitly.
- **R4 glTF import.** `MeshSystem` reads the 9 material extensions plus `KHR_materials_unlit`
  from `tinygltf::Material::extensions` and `TextureInfo` (`texCoord`, `scale` for the coat
  normal).
  - Values are clamped to the spec ranges. Non-finite values fall back to defaults with one
    warning per model.
  - An unsupported extension listed in `extensionsUsed` logs one warning naming it. One in
    `extensionsRequired` logs an error but the model still loads.
  - Colour textures (sheen colour, specular colour) are decoded sRGB→linear in the shader, as
    `getBaseColor` does.
- **R5 Extension maps.** Each lobe owns at most one map, with glTF-native channels:
  - clearcoat R=factor G=roughness;
  - clearcoat normal RGB;
  - sheen RGB=colour A=roughness;
  - specular RGB=colour A=factor;
  - iridescence R=factor G=thickness;
  - transmission R=transmission G=volume thickness;
  - anisotropy RG=direction B=strength.

  When glTF supplies two different images for one lobe map, they are **packed on the job
  system** into one RGBA8 image, at the larger size, bilinear. The pool key is built from both
  sources' GUIDs (S04) plus channel layout.
- **R6 Texture budget.** A variant binds at most **3** extension maps (slots
  `u_pbrExtTexture0..2`, UV set from `texCoordSets.set1.yzw`). Over budget, maps drop in
  reverse priority. The order is clearcoat > specular > transmission > sheen > anisotropy >
  iridescence > clearcoat normal. A dropped map falls back to its factor, with an inspector
  warning and an MCP report. The rule is the same on every platform.
- **R7 Hex tiling.**
  - `TerrainSurfaceLayer` gains `hexTiling` (bool) and `hexRotation` (0..1, default 1). A hex
    layer replaces its flat near+far projection pair with 3 hex taps; side planes stay planar.
  - `hasTerrainSurfaceLayers` counts `hexTiling`.
  - Lit materials gain `hexTiling`, `hexRotation` and `hexContrast` (0..1, default 0.6). Every
    texture sampled from UV set 0 is hex-sampled, and the tangent-space normal is rotated back
    by the tile rotation.
  - Unlit Lit variants hex-sample the base colour too.
- **R8 Photometry.** `LightComponent` gains:
  - `LightUnit intensityUnit = ENGINE`: ENGINE, LUX (directional only), CANDELA, LUMEN
    (point/spot);
  - `bool useColorTemperature = false`;
  - `float colorTemperature = 6500` (1000–20000 K).

  `SceneSettings` gains `float physicalLightEV100 = 15`. Every renderer consumer reads
  runtime-only `resolvedColor` / `resolvedIntensity` (§6.5), never the authored values.
- **R9 Unit changes preserve brightness.** The inspector, Lua `light:setIntensityUnit(unit,
  true)` and MCP convert the value with the inverse function. A type change that invalidates
  the unit converts to the nearest valid unit (lux ↔ candela at 1 m, same number).

### Platform & backends
- **R10** All features compile and run on GL 3.3, GLES3/WebGL2, Metal (macOS/iOS), D3D11 and
  Vulkan. They need no compute, SSBO or extra extension:
  - `textureGrad`, `uint`, and derivatives only;
  - `highp` declared in `iridescence.glsl`, `hex_tiling.glsl` and `specular_aa.glsl`;
  - ≤ 16 fragment textures in every variant (R6);
  - one extra 128-byte UBO.
- **R11 Transmission** uses the existing scene-colour copy (`includes/scene.glsl`, phase 3).
  A transmission submesh draws in the transparent queue after the copy, with BLEND_NONE and
  depth write on. RenderSystem treats `PBR_TRANSMISSION` exactly as a manifest
  `@require scenecolor`, including the Scene Color Scale and mobile cost.

### Performance (budgets; measured per §7)
- **R12 Zero cost when unused.** A material with no lobe/hex enabled resolves to customId 0
  and the exact pre-S11 variant. With `litQuality = STANDARD`, the preprocessed source of every
  existing variant is unchanged. A light in ENGINE units without temperature costs one branch
  on the CPU when it is resolved on change.
- **R13 Budgets.** Fragment cost relative to the same draw with Lit/pixel, 1 directional
  light + IBL, desktop GL:

  | Feature | Budget |
  |---|---|
  | HIGH quality | ≤ +5% |
  | Clearcoat | ≤ +25% |
  | Sheen | ≤ +15% |
  | Anisotropy | ≤ +15% |
  | Iridescence | ≤ +30% |
  | Specular/IOR | ≤ +3% |
  | Transmission (excluding the scene-colour copy) | ≤ +20% |
  | Hex Lit | ≤ 3× texture taps of the hexed maps |
  | 4-layer hex terrain | ≤ 1.75× terrain pass time on desktop GL, ≤ 1.4× on Vulkan (4K) |

  - **Hex terrain revision (GPU campaign perf3), approved by the user 2026-11-02** (it was ≤ 1.35×
    the terrain pass time; `tools/perf/gpu_campaign.py pbr` checks the new bounds by backend):
    the 4-layer hex terrain against the colour-layer Terrain Baseline at 3840x2160. Evidence on the
    RTX 3080 Ti (docs/perf/gpu-campaign.md, S11 perf3), measured 1.71× GL and 1.38× Vulkan (from
    3.20× and 2.93×) with the output unchanged:
    - the reference draws its colour layers with two bilinear taps each from an unmipmapped
      array (its layers ask for no mipmaps); a hex layer needs the surface-layer path and three
      trilinear taps (§6.4, R7). Those two extra taps a layer alone cost 0.28 ms on GL and 0.29 on
      Vulkan, +24-25 % of the reference; the surface path +8 % on GL (0 on Vulkan); mipmaps
      nothing (+0.4 %). So the method costs ≥ 1.32× on GL before any lattice math;
    - the lattice set-up (hashes, rotations, weights), once a fragment, costs ~0.45 ms on GL and
      ~0.13 ms on Vulkan for the same code (by difference; each further set-up measured 0.39 ms on
      GL, 0.08 on Vulkan): NVIDIA's GL compile of the variant, not the work, makes the difference;
    - with PBR maps on the layers the hex terrain is now 0.78× (GL) and 0.72× (Vulkan) the same
      terrain without hex.

  On export to Android/iOS/HTML5 the **Mobile PBR tier** (`Reduced` by default, project
  export setting) strips iridescence, anisotropy maps and the coat normal. Stripping is
  decided by one function shared by runtime and exporter.
- **R14** Packing two 2048² maps takes ≤ 40 ms on one job-system worker and never runs on the
  main thread. Light resolution stays out of the per-frame path: it is recomputed only when a
  light or the EV100 changes.

### Editor UX (human)
- **R15 Material inspector** (built-in Lit rows only, `InspectorWindow.cpp:6795`):
  - "Emission Intensity" (float, 0–∞, drag) goes under Emissive Factor.
  - A new **"Advanced Surface"** group has collapsing headers with a checkbox in the header:
    Clear Coat, Sheen, Anisotropy, Iridescence, Transmission, Volume (disabled unless
    Transmission), Specular & IOR, Hex Tiling.
  - A footer shows the variant (`builtin:lit` codes, *Compiling…*/*Ready*), **"Extension maps
    n / 3"**, and dropped-map warnings.
  - On a terrain submesh the group is disabled with a tooltip; on a fork lacking `u_fs_pbrExt`
    it shows "This shader does not implement advanced surface".
  - All rows multi-edit and undo through `MaterialChange` (S24 names: "Change Clear Coat
    Roughness").
- **R16 Light inspector:**
  - an Intensity row with a unit combo (Engine / Lux / Candela / Lumen, filtered by type);
  - "Use Color Temperature";
  - "Temperature" (K) with a blackbody gradient bar and presets: Candle 1900, Tungsten 2700,
    Halogen 3200, Daylight 5600, Overcast 6500, Blue Sky 10000;
  - Color is relabelled "Filter" while temperature is on (Unity naming).
- **R17 Scene settings:** Lighting gets "Lit Quality" and "Physical Light Exposure (EV100)"
  with presets: Sunny 15, Overcast 12, Interior 7, Night 2. A hint says it affects only lights
  authored in physical units.
- **R18 Terrain inspector:** per layer, "Hex Tiling" + "Rotation".
- **R19 Material presets** (`MaterialPresets.cpp`): Car Paint (clearcoat), Velvet (sheen),
  Brushed Metal (anisotropy), Soap Bubble (iridescence), Clear Glass (transmission+volume),
  Tiled Floor (hex).

### Agent UX (MCP, Lua, CLI)
- **R20 Lua** (`CoreClassesLua.cpp`, copy-then-`setMaterial` semantics unchanged): flat
  `Material` properties:
  - clearcoat: `clearcoatEnabled/Factor/Roughness/NormalScale`;
  - sheen: `sheenEnabled/Color/Roughness`;
  - anisotropy: `anisotropyEnabled/Strength/Rotation`;
  - iridescence: `iridescenceEnabled/Factor/Ior/ThicknessRange` (Vector2);
  - transmission: `transmissionEnabled/Factor`;
  - volume: `volumeEnabled`, `thicknessFactor`, `attenuationColor`, `attenuationDistance`;
  - specular: `specularEnabled/Factor/Color`, `ior`;
  - emissive: `emissiveStrength`;
  - hex tiling: `hexTilingEnabled/Rotation/Contrast`.

  Textures stay read-only as today. `Light`: `intensityUnit` (`LightUnit.ENGINE|LUX|CANDELA|LUMEN`),
  `useColorTemperature`, `colorTemperature`, `setIntensityUnit(unit, preserveBrightness)`.
  `scene.litQuality`, `scene.physicalLightEV100`; terrain layer `hexTiling`, `hexRotation`.
  All of these go into S23's JSON API description.
- **R21 MCP:**
  - `create_material` / `update_material` and the component property paths accept a `pbr`
    object (§6.6). The material read output includes it, plus `emissiveStrength`.
  - New tool **`get_material_variant {entity, submesh}`** returns `{reference, defines[],
    customId, ready, extensionMaps:{used, max:3, dropped[]}, litQuality}`.
  - New tool **`set_light_unit {entity, unit, preserveBrightness=true}`**.
  - `list_material_presets` lists the R19 presets.
  - New guide `docs/mcp/guide/materials.md`, served as `jasper://docs/materials`. Update
    `lua-api.md`, `shaders.md` (forking: the S11 hooks) and `docs/shaders/README.md`.
- **R22** `jasperengine-editor shaders --library all --backend all` also compiles the
  `builtin:lit` lobe matrix (§6.3) and fails on any error.

### Data & compatibility
- **R23** Materials serialize a nested `pbr:` map (in scenes and `.material` assets, through
  the same `Stream.cpp` encoder) **only when it differs from the defaults**.
  `emissiveStrength` is written only when ≠ 1. Light fields are written only when non-default.
  Re-saving a pre-S11 scene therefore produces a byte-identical file. Older files load with
  defaults; unknown-key preservation is S03's.
- **R24** `SubmeshOverrideFlags` and `overrideFields` widen to `uint64_t`; the YAML stays
  numeric, so it is compatible. New bits are `SubmeshOverride_PbrExtensions` (the whole
  struct, all-or-nothing) and `SubmeshOverride_EmissiveStrength`. Both are included in
  `SubmeshOverride_Material`, and a model reload keeps edited lobes.
- **R25** The generated C++ (`Factory.cpp`) emits the new material, light, terrain and scene
  fields.

## 6. Design constraints & decisions

### 6.1 Out-of-line extensions
`engine/core/util/OutOfLineValue.h` is a generic version of `MaterialExtraPass`: deep copy,
value compare, unset == `T()`, `get()` never allocates, and `edit()` allocates.
`Material::pbr` is an `OutOfLineValue<MaterialPbrExtensions>`. `sizeof(Material)` grows by
≤ 16 bytes (pointer + `emissiveStrength` + padding).

```cpp
struct MaterialPbrExtensions {            // engine/core/texture/MaterialPbr.h
    struct Clearcoat   { bool enabled=false; float factor=0, roughness=0, normalScale=1;
                         Texture map /*R,G*/, normalMap; int mapTexCoord=0, normalTexCoord=0; } clearcoat;
    struct Sheen       { bool enabled=false; Vector3 color{0,0,0}; float roughness=0; Texture map; int mapTexCoord=0; } sheen;
    struct Anisotropy  { bool enabled=false; float strength=0, rotation=0; Texture map; int mapTexCoord=0; } anisotropy;
    struct Iridescence { bool enabled=false; float factor=0, ior=1.3f; Vector2 thicknessNm{100,400}; Texture map; int mapTexCoord=0; } iridescence;
    struct Transmission{ bool enabled=false; float factor=0; Texture map /*R, G=thickness*/; int mapTexCoord=0; } transmission;
    struct Volume      { bool enabled=false; float thickness=0, attenuationDistance=INFINITY; Vector3 attenuationColor{1,1,1}; } volume;
    struct Specular    { bool enabled=false; float factor=1, ior=1.5f; Vector3 color{1,1,1}; Texture map; int mapTexCoord=0; } specular;
    struct Hex         { bool enabled=false; float rotation=1, contrast=0.6f; } hex;
    bool operator==(const MaterialPbrExtensions&) const;
};
// pure, shared by RenderSystem (runtime) and ShaderVariantCollector (export), unit-tested
struct PbrVariant { std::vector<std::string> defines; std::vector<std::string> droppedMaps; };
PbrVariant getPbrVariant(const Material&, MaterialLighting resolved, bool terrain, PbrTier tier);
```

When S08 lands first, these fields are declared in its reflection table, and Catalog/Stream/MCP
are driven from it. Otherwise they are hand-wired in the files listed in §3, and S08 migrates
them.

### 6.2 Variant transport: `builtin:lit` references
- The mesh bit space is full, so **lobes and hex travel in the customId reference**, the same
  way library features do.
- `getSubmeshShaderReference` returns `""` (customId 0) when `getPbrVariant` yields no
  defines. Otherwise it returns `"builtin:lit#<sorted defines>"`, or `"<fork>#..."` for a fork.
  - For forks, the PBR defines are appended **after** manifest feature resolution, as the
    `BLEND_*` define is (`ShaderManifest.cpp:1343-1357`), so a manifest never filters them out.
- `ShaderReference::BUILTIN_PREFIX = "builtin:"`. `resolveCustomShaderSources` maps
  `builtin:lit` to the embedded `mesh.vert` / `mesh.frag` with the defines.
- `getCustomShaderBasename` encodes `builtin:lit` as `lit_` plus 2-letter codes
  (`Cc Cm Cn Sh Sm An Am Ir Im Tr Tm Vo Sp Sq Hx Th Lq Ts`), always ≤ 40 characters. This keeps
  `.sdat` paths short on Windows.
- `getShaderVariants` and `ShaderVariantCollector` pick these keys up unchanged, so export
  precompiles them.
- While a new variant compiles, the draw uses the customId-0 variant, so there are no stalls.
  `getShaderVariants` already lists the customId-0 key beside every fork key
  (`RenderSystem.cpp:2903-2908`). The implementer must confirm that the async draw path
  actually falls back to it while compiling, and add that fallback if it does not (AC12
  checks this).

Defines:
- `PBR_CLEARCOAT`, `PBR_SHEEN`, `PBR_ANISOTROPY`, `PBR_IRIDESCENCE`, `PBR_TRANSMISSION`,
  `PBR_VOLUME`, `PBR_SPECULAR`, `PBR_HEX_TILING`, `TERRAIN_HEX_TILING`,
  `TERRAIN_HEX_STOCHASTIC` (with `TERRAIN_HEX_TILING` only: a layer of the terrain takes
  Stochastic Tiling, §6.4);
- map presence: `PBR_CLEARCOAT_MAP`, `PBR_CLEARCOAT_NORMAL_MAP`, `PBR_SHEEN_MAP`,
  `PBR_ANISOTROPY_MAP`, `PBR_IRIDESCENCE_MAP`, `PBR_TRANSMISSION_MAP`, `PBR_SPECULAR_MAP`.

`includes/pbr_ext_slots.glsl` assigns slots 0–2 by the fixed priority, using only `#if` chains.

### 6.3 Shader layout
New includes in `engine/shaderlib/includes/`:
- `pbr_ext.glsl`: lobe BRDFs and the `u_fs_pbrExt` block;
- `pbr_ext_slots.glsl`;
- `iridescence.glsl`;
- `specular_aa.glsl`;
- `hex_tiling.glsl`: the lattice block between `// HEX_LATTICE_BEGIN/END` markers. It is pure
  math, uses `f`-suffixed literals and no samplers, and is compiled as C++ by the test.

`mesh.frag` gets `#if`-guarded hooks only: `MaterialInfo` fields, the f0/f90 computation, the
per-light lobe sum, the IBL lobe sum, and the transmission composite. Guarded hooks keep R12
true by construction. The one block is:

```glsl
uniform u_fs_pbrExt {          // std140, 8 vec4, declared only if any PBR_* is defined
  vec4 clearcoat;     // factor, roughness, normalScale, specular f0.r
  vec4 sheen;         // rgb colour (linear), roughness
  vec4 anisotropy;    // strength, cos(rot), sin(rot), specular f0.g
  vec4 iridescence;   // factor, ior, thicknessMin nm, thicknessMax nm
  vec4 transmission;  // factor, thickness, attenuationDistance, specular f0.b
  vec4 attenuation;   // rgb, ((ior - 1) / (ior + 1))^2 (1 for ior < 1)
  vec4 specular;      // rgb colour, factor
  vec4 iorHex;        // ior, hexRotation, hexContrast, sheen L(0.5)
} pbrExt;
```

The shader matrix (R22) covers:
- each lobe alone × {pixel, mixed} × {no map, map};
- all lobes + 3 maps;
- transmission+volume;
- `PBR_HEX_TILING` × {unlit, pixel};
- `TERRAIN_HEX_TILING` × terrain PBR;
- `USE_LIT_QUALITY` on the base variants;

on all 6 backends. Lit Quality, lobes and hex apply to the built-in Lit only. Library shaders
may `#include` `hex_tiling.glsl` / `specular_aa.glsl` later.

### 6.4 Terrain hex
- The `TERRAIN_HEX_TILING` define is emitted when any layer has `hexTiling`.
- `slices[i].w` is the layer's hex rotation, and `< 0` means off. The CPU writes −1 for
  non-hex layers. Inside the variant, each layer branches on it uniformly.
- Gradients are `R_k · terrainFlatDX/DY`. This is exact, and every sample is `textureGrad`.
- With hex on, a layer skips the far tiling-break sample, and the flat colour, surface and normal
  samples its hex taps replace.
- Hex adds no textures.
- The hex variant compiles Stochastic Tiling's path only when a layer takes it
  (`TERRAIN_HEX_STOCHASTIC`, emitted beside `TERRAIN_HEX_TILING`; a layer with both takes
  Stochastic Tiling, as before). Without it the variant held the path's lattices for nothing
  (255 registers and spills on NVIDIA against the colour terrain's 128: GPU campaign perf3).
- The first hex layer's taps (and their weight powers) are set up once a fragment; a layer that
  tiles as it does (scale, offset, rotation) and each of its maps take them, the others set up
  their own: the same inputs give the same values.

### 6.5 Photometry
`engine/core/util/LightPhotometry.h` is header-only and ported from OE:

```cpp
enum class LightUnit { ENGINE, LUX, CANDELA, LUMEN };
Vector3 kelvinToLinearRGB(float kelvin);                       // max channel == 1
float   exposureScale(float ev100) { return 1.0f / (1.2f * std::exp2(ev100)); }
float   toEngineIntensity(float value, LightUnit, float ev100); // LUX/CANDELA: v*scale; LUMEN: v/(4π)*scale
float   fromEngineIntensity(float engine, LightUnit, float ev100);
void    resolveLight(LightComponent&, float ev100);           // fills resolvedColor/resolvedIntensity
```

- Rationale (Lagarde pre-exposure): a white Lambertian lit by E lux has luminance E/π, and
  our Lit outputs `I/π` for intensity I. So `I = E·exposureScale(EV100)` is physically
  pre-exposed. For example, a 100 000 lx sun at EV100 15 gives 2.54.
- Spot lumen uses 4π, so cone edits do not change brightness (Unity "reflector off"). This is
  documented.
- ENGINE units are untouched, so old lights look identical.
- `LightSystem` resolves on change (dirty flag on the light or on the EV100).

### 6.6 MCP `pbr` object
The shape mirrors §6.1:
`{"clearcoat":{"enabled":true,"factor":1,"roughness":0.05,"map":"tex/cc.png","normalMap":null},"specular":{"ior":1.45},"hex":{"enabled":true}}`.
- Partial objects merge into the current values, and `null` clears a map.
- Ranges are validated with the R4 clamps, and an error names the field.
- Catalog paths are `submeshes[i].material.pbr.<lobe>.<field>`.

### 6.7 Vendored test assets (Khronos glTF-Sample-Assets; licences verified from each model's `metadata.json`)
They live in `engine/tests/render/assets/khronos/<Model>/`, keeping the original files plus
`metadata.json` and `LICENSE.md`. Each model is ≤ 6 MB and the set is ≤ 30 MB; a model over
budget is dropped from the list.

| Model | Covers | Licence |
|---|---|---|
| ClearCoatTest | clearcoat, coat normal | CC-BY-4.0 (Ed Mackey, Analytical Graphics) |
| SheenTestGrid | sheen | CC0-1.0 |
| AnisotropyStrengthTest | anisotropy | CC0-1.0 |
| AnisotropyRotationTest | anisotropy rotation/map | CC-BY-4.0 (Ed Mackey, AGI) |
| IridescenceDielectricSpheres, IridescenceMetallicSpheres | iridescence | CC0-1.0 |
| TransmissionTest | transmission | CC0-1.0 (Adobe) |
| TransmissionRoughnessTest, AttenuationTest | rough transmission, volume | CC-BY-4.0 (Ed Mackey, AGI) |
| SpecularTest | specular | CC-BY-4.0 (Ed Mackey, AGI) |
| IORTestGrid | IOR | CC0-1.0 |
| EmissiveStrengthTest | emissive strength | CC-BY-4.0 (Ed Mackey, AGI) |
| MetalRoughSpheresNoTextures | multiscatter, spec AA baseline | CC0-1.0 |

- CC-BY-4.0 models are credited in `NOTICE.md`, in a "Test assets" table.
- **`Compare*` models are excluded**: they embed the Khronos glTF logo under
  `LicenseRef-LegalMark-Khronos` (trademark).
- No HDR environment is vendored. Goldens light with the engine procedural sky + IBL + one
  directional light.

## 7. Acceptance criteria

The unit tests use S02's gtest framework, in `engine/tests/core/`. The MCP tests go in
`tools/mcp/tests/test_materials_pbr.py`. Goldens are compared with `tools/mcp/pngdiff.py`
(defaults: tolerance 24, max fraction 0.01) on the GL-core build server. They are stored in
`engine/tests/render/golden/s11/` and updated with `JASPER_RENDER_UPDATE_GOLDEN=1`.

- **AC1 (R12)** `ShaderZeroCostTest.DefaultVariantsUnchanged` checks that, for the full mesh
  property matrix that `ShaderLibraryMatrix` already enumerates, the preprocessed GLSL of
  `mesh.frag` with no S11 define equals the pre-S11 source. A checked-in hash list is generated
  on the S11 base commit. `MaterialPbrTest.DefaultHasNoReference` checks that a default
  material yields `""` / customId 0.
- **AC2 (R12, R23)** Re-encoding every scene in `tools/fixtures/*` with the S11 build produces
  byte-identical YAML (MCP `test_materials_pbr.py::test_legacy_resave_identical`). The
  `unified-lighting`, `shader-library` and `post-processing` fixtures render pixel-identically
  (tolerance 0) to pre-S11 goldens with Lit Quality Standard.
- **AC3 (R1, R24)** `MaterialPbrTest.SizeBudget`: `sizeof(Material)` ≤ pre-S11 + 16 bytes.
  `MaterialPbrTest.OutOfLineSemantics`: copy, compare, an unset value reads defaults, and
  `edit()` allocates once. `SubmeshOverrideTest.PbrSurvivesReload`: an edited clearcoat
  survives `MeshSystem` reload.
- **AC4 (R4)** `GltfMaterialExtensionsTest.<Ext>` (one per extension). Hand-written minimal
  `.gltf` fixtures load to the exact expected factors, textures and texCoords; missing
  extensions give defaults; out-of-range values are clamped; NaN warns once.
  `GltfMaterialExtensionsTest.UnsupportedWarns`: `KHR_texture_transform` produces exactly one
  warning. Every §6.7 model loads with zero errors.
- **AC5 (R5, R6)**
  - `TexturePackTest.TwoSourcesRG` produces the packed bytes expected.
  - `TexturePackTest.MixedSizes` resizes to the larger size.
  - `PbrVariantTest.BudgetDropsByPriority`: all 7 maps leave clearcoat, specular and
    transmission, and report the other 4 dropped.
  - `PbrVariantTest.VertexLightingStripsLobes`.
  - `PbrVariantTest.MobileReducedTier`.
- **AC6 (R2, R22)** `jasperengine-editor shaders --library all --backend all` exits 0 with the
  §6.3 matrix included, and compiles each variant on 6 backends. A static check in the same
  command confirms every compiled variant reflects ≤ 16 fragment textures.
- **AC7 (R2) goldens.** One golden per §6.7 model, from fixture `tools/fixtures/pbr-extended`
  (fixed camera per model). Each is reviewed once by a human against the Khronos
  glTF-Sample-Viewer screenshot in the model's `screenshot/` folder; the review is recorded in
  the fixture README. A `lobe-off` twin of each scene, with lobes disabled through MCP, must
  **differ** from the golden by more than 2% of pixels. This proves the lobe has an effect.
- **AC8 (R3) energy.** White-furnace check: a roughness 1.0, metallic 1 sphere in a uniform
  white ambient with HIGH gives mean sphere luminance ≥ 0.95× the base colour; STANDARD gives
  < 0.9×. The test is `test_materials_pbr.py::test_furnace`, which reads the screenshot.
  Specular AA: in the `spec-aa` scene (a high-frequency normal map, camera dolly) the
  frame-to-frame luminance variance of highlight pixels drops ≥ 40% with HIGH
  (`test_spec_aa_shimmer`, 30 frames, `fixed_fps`).
- **AC9 (R7)**
  - `HexTilingTest.*` compiles the GLSL lattice block as C++ and checks:
    - weights sum to 1 ± 1e-5;
    - each simplex has one vertex per colour;
    - the slot that changes across an edge has weight < 1e-4 on it;
    - hash determinism for negative and ±2³⁰ ids.
  - Goldens `hex-floor` (Lit) and `hex-terrain` (4 layers) are checked.
  - An autocorrelation test (`test_hex_repetition`) on a 64-repeat plane checks that the peak
    at the texture period is ≥ 50% lower than without hex.
- **AC10 (R8, R9)**
  - `PhotometryTest.Kelvin`:
    - 6500 K is within 0.03 of white;
    - 1900 K has blue < 0.25;
    - the blue/red ratio increases monotonically from 1000 to 20000 K;
    - max channel == 1.
  - `PhotometryTest.Sun`: 100 000 lx at EV100 15 gives 2.543 ± 0.01.
  - `PhotometryTest.RoundTrip`: every unit at three EV100 values, within relative 1e-5.
  - `test_light_units_preserve`: `set_light_unit` to Lux/Candela/Lumen leaves the screenshot
    unchanged (pngdiff default tolerance).
- **AC11 (R13) performance.**
  - `jasperengine-editor benchmark --project tools/fixtures/pbr-extended --scene "Lobes Off"`
    is within ±2% of the pre-S11 build's FPS on the same server.
  - The per-lobe scenes (`"Lobe <name>"`, 200 full-screen-ish spheres) meet the R13 ratios in
    frame time against the `"Lit Baseline"` scene. When S15 is present they use its GPU pass
    timings.
  - `hex-terrain` ≤ 1.75× (desktop GL) / ≤ 1.4× (Vulkan) the terrain pass time of
    `terrain-baseline` at 4K (R13, revised 2026-11-02).
  - The packing benchmark is ≤ 40 ms (gtest `TexturePackBench`, reported, threshold
    asserted).
- **AC12 (R15–R19) manual**, done on the editor (GL), and recorded in the fixture README:
  - open a ClearCoatTest submesh: the Advanced Surface group shows Clear Coat enabled with
    maps;
  - toggling it shows *Compiling…* then *Ready* without a frame stall above 50 ms (frame
    stats);
  - Ctrl+Z restores it;
  - multi-select 3 meshes and set Sheen Roughness; all three change and undo together;
  - assign 4 maps: the footer shows "Extension maps 3 / 3" plus a warning;
  - Light: switch Engine→Lux and the viewport is unchanged; enable temperature and the Filter
    label appears;
  - preset Car Paint applies a clearcoat.
- **AC13 (R20)** `test_materials_pbr.py::test_lua`: a play-mode script sets
  `m.clearcoatEnabled = true; m.clearcoatFactor = 1; mesh:setMaterial(m)`,
  `light.intensityUnit = LightUnit.LUX`, and `scene.litQuality`. `get_material_variant` then
  reports `PBR_CLEARCOAT`, and the screenshot changes.
- **AC14 (R21)** MCP tests cover:
  - `update_material` with a partial `pbr` merges and validates (a bad range gives an error
    naming the field);
  - `get_material_variant` returns the dropped maps;
  - `jasper://docs/materials` resolves, checked by the `test_docs.py` link check.
- **AC15 (R11, R10)** In the `shader-library-export` style fixture `pbr-extended-export`,
  HTML5 and Android exports log no runtime shader compile (`notPrecompiledShaders` empty). The
  transmission scene renders the refracted background on GLES3 (smoke test screenshot is not
  flat). `tools/smoke-test.sh pbr-extended` is green.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20);
  - S02 (gtest target, golden harness and CI running MCP tests);
  - S03 (unknown-key preservation; no version bump is needed because the change is additive);
  - S04 (GUIDs for packed-map pool keys);
  - S05 (a source texture change re-packs its packed map);
  - S08 (soft: the reflection table if it lands first, §6.1).
- **Coordinates with:**
  - S14: compression of packed maps; coat normal as an RG normal format;
  - S15: GPU timings for AC11;
  - S13: bloom from emissive strength;
  - S12: fog on transmission draws;
  - S10: shadows. Transmission submeshes cast normal shadows. `lights.glsl` and `mesh.frag`
    are shared.
  - S23: Lua JSON API;
  - S24: undo names;
  - S25: new settings are searchable;
  - S27: Kelvin presets could reuse its swatch widget (not required).
- **Blocks:** the follow-up `KHR_lights_punctual` import, and library shaders adopting
  `hex_tiling.glsl` / `specular_aa.glsl`.
- **Merge hotspots:**
  - shaders: `mesh.frag`, `includes/pbr.glsl`, `ibl.glsl`, `lights.glsl` (S10),
    `terrain_fs.glsl` (S18);
  - engine data: `Material.h`, `MeshComponent.h`, `TerrainComponent.h` (S18),
    `LightComponent.h` (S10), `SceneSettings.h` (S10, S12, S13);
  - the glTF block of `MeshSystem.cpp` (S04, S21 glTF extras);
  - `RenderSystem.cpp` (everyone);
  - shader system: `ShaderPool.cpp`, `ShaderManifest.cpp`, `ShaderVariantCompiler.cpp`;
  - editor: `InspectorWindow.cpp`, `Catalog.cpp`, `Stream.cpp`, `McpValues.cpp`,
    `AssetTools.cpp`, `CoreClassesLua.cpp`, `Factory.cpp`.

## 9. Risks & defaults

1. **16-texture limit.** The default is the fixed 3-map budget with priority drop (R6), the
   same on all platforms. A dynamic per-variant budget is rejected: it would make results
   differ by environment (SSAO/IBL on or off).
2. **Variant growth.**
   - Variants exist only for combinations that are used (collector), and explicit `enabled`
     flags stop factor animation from recompiling.
   - Vertex/None lighting strips lobes, and the mobile Reduced tier strips more.
   - The editor warns when a project exceeds 64 `builtin:lit` variants (Project Settings and
     `get_material_variant`).
3. **Old forks** ignore lobes. The default: the inspector flags it, and the docs tell users to
   re-fork.
4. **LDR scene-colour copy.** Refracted highlights clip and transparent objects are invisible
   through transmission, the same as `library:glass`. Accepted and documented. An HDR copy
   would need its own spec.
5. **Sheen IBL and transmission blur are approximations** next to the Sample Viewer (no
   Charlie cube, no mip chain). The default is human-reviewed goldens (AC7), not numeric
   parity with Khronos.
6. **Lit Quality changes the look of new scenes.** New scenes default to HIGH; loaded scenes
   with no key stay STANDARD.
7. **Physical units on an 8-bit pipeline.** Pre-exposure through EV100 (default 15) keeps
   values in range. If S13 or a later HDR spec adds auto-exposure, EV100 becomes its input;
   that is not done here.
8. **Mobile precision.** Iridescence, hex hash and spec AA are `highp`, and the hash is an
   integer hash. If a GLES device lacks `highp` in fragment shaders, the export check fails
   loudly rather than shading wrongly. WebGL2 and GLES3 require `highp`, so the default
   assumes it.
9. **CC-BY asset attribution** must ship in `NOTICE.md`. The assets are test-only and never
   packaged into exports; the `.gitattributes` / export filter excludes `engine/tests/`.

## Deviations (implementation record)

Decisions taken while implementing, where the spec was silent, wrong in a detail or in conflict
with other streams:

1. **Lit Quality is a define, not mesh property bit 5.** S10 (`USE_SHADOWS_HQ`) and S12
   (`HAS_HEIGHT_FOG`) both claim bit 5, and S12 asks S11 not to. High adds `USE_LIT_QUALITY` to the
   `builtin:lit#<defines>` reference instead (basename code `Lq`), so a Standard scene, and every
   variant without it, keeps its key and its preprocessed source (`ShaderZeroCost` hash list
   unchanged).
2. **16 textures and 8 uniform blocks.** The scene-colour copy needs a texture slot and the
   scene-inputs block: transmission variants drop SSAO (they draw in the transparent queue, which
   has none) and 2D lighting, and a variant in a scene with 2D light shadows reserves one of the 3
   map slots (`getPbrVariant(..., reservedMaps)`, shown in the Inspector footer and
   `get_material_variant`). No `u_fs_pbrExt` block: after S12 added `u_fs_heightFog` a lobe
   variant had no uniform block to spare, so the lobes' values (and, with a second UV set, the
   UV sets instead of `u_fs_texCoordSets`) are the last members of `u_fs_pbrParams` under
   `#ifdef PBR_EXT`. The `builtin_lit` matrix checks both limits on six backends.
3. **13 Khronos models** (§6.7 as verified), goldens written by the MCP test's own scenes
   (`test_khronos_<Model>`, a test per model since BF16), not by a `tools/fixtures/pbr-extended` scene per model: the fixture
   keeps the benchmark scenes and the export check. `JASPER_TEST_UPDATE_GOLDEN=1` (S02's name) and
   `JASPER_RENDER_UPDATE_GOLDEN=1` both write them.
4. **AC8 furnace on direct light.** The environment split sum is already multiscatter
   compensated (Standard and High alike), so a uniform-ambient furnace cannot separate them.
   `test_furnace` lights a rough white metal with the sun only: High is >= 1.15x Standard at
   roughness 1, within 8% at roughness 0.2.
5. **`KHR_materials_unlit`** loads as `library:unlit` (Lighting None) rather than a lobe.
6. **Packed glTF maps** are keyed by model file and image indices (`pack|<layout>|a|b`); S04
   asset identity was not landed. They are regenerated on load (exports repack at runtime).
7. **Transmission thickness** is in world units (the glTF value times the node scale is not
   applied); refraction reads the LDR opaque copy (Risk 4).
8. **Variant collection** collects the mobile tier's variants too (Android, iOS, HTML5 exports),
   and both Lit Qualities when a script mentions `litQuality`.
9. **Lua** exposes flat Material properties (S23's JSON material API had not landed).
10. **`KHR_xmp`, `KHR_xmp_json_ld`** are read as known (metadata only) and give no warning.
11. **MCP** `set_terrain_maps` layers take `hexTiling`/`hexRotation`; project setting
    `graphics.mobile_pbr_tier`.
12. **AC2 re-save.** The checked-in fixtures were saved by older editors: today's build (with or
    without S11) rewrites them on a first save (scene keys other streams added, the S03 version
    line, `maxValues` of what a headless load sees). `test_legacy_resave_identical` therefore
    checks that a save adds no S11 key to a file that had none and that a second save is
    identical (shader keys as a set); `Compat.CorpusRoundTrip` covers byte stability of
    editor-written scenes.
13. **AC1 golden.** `RenderGpu.LitCube` pins its scene to Standard (its pre-S11 golden matches
    unchanged); `RenderGpu.LitCubeHigh` has a golden of its own.
14. **Benchmark fixture** scenes are generated into `build/pbr-extended`
    (`tools/fixtures/pbr-extended/generate.py`), not checked in (200 spheres are 12 MB of scene
    file each); `tools/fixtures/pbr-extended-export` is checked in. Its terrain uses an 8-bit
    height map: a generated 16-bit one is `R16`, which GLES3/WebGL2 cannot sample (a terrain
    limitation outside S11).
15. **No mesh property bit, no cache version bump.** S10 took bit 5 (`Shq`) and S12 claims it too;
    S11 needs none: the lobes, hex tiling and Lit Quality travel as `builtin:lit#<defines>`
    custom-shader references (`lit_<codes>` basenames), so no existing shader key or cached
    program changes (the zero-cost hash list proves the defaults preprocess as before). The
    collector's environment bits for Lit Quality and the mobile tier are 23 and 24 (S10's
    `shadowsHQ` is 22). The hash list was regenerated once, from oe-port's shader library after
    S10 had changed the built-in Lit.
16. **Compat corpus and model images.** A scene holding a model with textures stores them as ids
    carrying the model file's absolute path (engine behaviour before S11), which a bare decode
    cannot load; `Compat.CorpusRoundTrip` leaves such scenes out of its byte check (the
    `pbr-extended-export` Lobes scene holds ClearCoatTest). Maps packed from a model's images
    (`pack|...`) are never written: the model packs them again when it loads.
17. **Other streams' goldens.** S10's and S12's render goldens (GPU unit tests, `test_shadows.py`,
    `test_atmosphere.py`) were drawn before Lit Quality; their scenes now set Standard (the same
    shader), since a new scene is High.
18. **AC12 frame stall: deferred to S32.** S11's side is done: a lobe variant compiles on a job
    (S06), the plain variant draws meanwhile, and the stand-in reloads once the new one is built
    (a bug left it in place until the next texture change; fixed). What remains is engine-wide:
    the GL link of a new program on the GL thread (1.2-1.3 s on llvmpipe, the same for variants
    without S11) and the driver's compile at its first draw; S32 "asynchronous shader/pipeline
    creation" (KHR_parallel_shader_compile on GL/WebGL2, off-thread Vulkan pipelines, async
    Metal/D3D11, prewarm) takes it. Measurements: `tools/fixtures/pbr-extended/README.md`.
19. **Per-draw lobe values in the block's free floats (GPU campaign perf3).** What a lobe derives
    from its uniforms alone is made once a draw by `MaterialPbr::getUniformBlock` into the `w` the
    §6.3 block left unused: the Specular & IOR lobe's dielectric f0 (without a specular map) and
    its IOR term, and the Charlie sheen visibility's L(0.5) (without a sheen map). The block stays
    8 vec4 (128 bytes). Computed on the CPU the values can differ from the shader's in the last
    bit (its reciprocal, pow and exp are approximations): on the RTX 3080 Ti at 4K no pixel of the
    Specular scene changed on GL or Vulkan, Lobe Sheen none on Vulkan and 2 of 8.3 M by 1/255 on
    GL; the goldens are unchanged.
20. **`TERRAIN_HEX_STOCHASTIC`** (basename code `Ts`), §6.2 and §6.4: the hex terrain variant
    compiles Stochastic Tiling's path only for a terrain with a Stochastic Tiling layer.
- **AC11 on a GPU (GPU campaign, RTX 3080 Ti, GL, 3840x2160, S15 pass timers;
  `tools/fixtures/pbr-extended/README.md`, docs/perf/gpu-campaign.md).** Within R13: Lobes Off
  (-0.2 %), Clearcoat (+21.6 %), Anisotropy (+11.1 %), Iridescence (+27.0 %). Over: Sheen +16.5 %
  (≤ 15), Specular/IOR +8.8 % (≤ 3), HIGH quality +7.9 % (≤ 5), and the 4-layer hex terrain 5.16x
  the terrain pass (≤ 1.35x); Transmission's +206 % includes the scene-colour copy the budget
  leaves out. A hex layer skipping the near/far path it overwrites made no measurable difference
  (the compiler drops it) and was not kept; the cost is the hex taps (three explicit-gradient taps
  per map and layer). Open for a follow-up; 1080p was not usable (a 0.24 ms frame clocks the GPU down).
- **AC11 in perf2 (the same machine; docs/perf/gpu-campaign.md S11):** the hex terrain 5.2x -> 3.2x
  on GL and 7.1x -> 3.0x on Vulkan, output unchanged: a hex layer's taps are made where each map
  samples them instead of kept live through the layer (register pressure over the layer loop),
  unweighted side planes are not fetched, and the flat projection a hex layer overwrites is not
  sampled. Sheen is within R13 on Vulkan (+7.9 %), +16.0 % on GL; Specular +8.3 % GL, +3.8 %
  Vulkan. On GL the lobes' cost does not grow with the pixels (Specular +0.07 ms at 1080p, +0.054
  ms at 4K) and half of it stays with the lobe shading removed; the hex variant with every hex call
  removed still takes 2.3x the terrain: a Lit variant with extra defines costs more than the
  default variant with the same code, which is where the rest of R13 is (open).
- **AC11 in perf3 (the same machine, GL / Vulkan at 4K, 5 runs; docs/perf/gpu-campaign.md S11):**
  Sheen +16.3 % -> **+13.1 %** GL (+9.6 -> +7.6 % Vulkan), Specular/IOR +8.5 % -> **+2.3 %** GL
  (+3.9 -> +4.4 % Vulkan), both within R13 on GL; the hex terrain 3.20x -> 1.71x GL, 2.93x ->
  1.38x Vulkan, within R13 as revised (≤ 1.75x GL, ≤ 1.4x Vulkan, approved by the user
  2026-11-02; it was 1.35x). The output is
  the same (every scene captured at 4K before and after: Vulkan 0 pixels differ, GL 0 but 2 of
  8.3 M by 1/255 in Lobe Sheen; goldens unchanged). perf2's per-variant overhead was not the
  variant: the hex variant held Stochastic Tiling's lattices (255 registers and spills, now 128,
  Deviation 20), and the Specular lobe's f0 chain is latency-bound in the 128-register Lit
  variant (now made once a draw, Deviation 19); a lobe variant shaded as the plain Lit costs
  +0.3 %. Lobes Off -0.2 % (≤ 2 %).
