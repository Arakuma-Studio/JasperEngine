# SSR probes: screen-space reflections that replace, never add to, the reflection a surface already has (design)

Date 2026-11-04. Branch `dc/ssr-probes`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md` (#14).
Dossier: `dc-work/dossiers/ssr-probes.json` (Doriax `a26ec5608`, `1ed266ea3`; context `d72c16153`, `096a36a3c`).
All Jasper behaviour below is inferred from reading the code at `b4f7816ea` unless a line says otherwise; nothing was
built or run for this spec.

## 1. Goals

1. With SSR on, every opaque pixel ends up with the reflection it would have if SSR were the environment: where SSR
   is confident it **replaces** the environment specular the colour pass added (sky IBL, a local reflection probe
   blended over the sky, or the flat scene ambient), where it misses that term stays. No pixel gets both.
2. The swap is occluded exactly as the colour pass occludes the term it replaces: material ambient occlusion (its
   map, on its UV set, through Stochastic Tiling's taps), a painted terrain's occlusion, SSAO, and the scene fog
   (distance or height) in front of the surface.
3. Local reflection probes are rebuilt per pixel by the composite through a small set of per-view probe slots,
   given only to probes of meshes that are drawn into the G-buffer and take IBL, ranked by how much of the screen
   they cover; a probe that gets no slot keeps its probe reflection (never a double one) and is reported.
4. The composite returns the scene colour untouched, after two texture reads, on every pixel SSR does not reflect
   on (most of a frame), and the SSR march skips surfaces SSR will not touch.
5. Humans and agents can see and check all of it: two new SSR debug views (Reflection Source, Occlusion), a
   renamed one (Environment Specular), SSR probe-slot counters in `get_render_stats`, a log line when probes
   overflow, and user and agent docs for reflections, which Jasper does not have today.

## 2. Non-goals

- **SSR before transparents and UI** (Unity's order). Today transparents, near-plane videos, screen fluids and
  HTML UI documents are drawn into the SSR scene colour before the composite (`RenderSystem.cpp:16393-16441`), so
  they are reflected and receive the opaque floor's swap under them. Fixing that splits the colour pass (end it after
  the sky, composite, resume with the opaque depth for the transparent queue), needs an extra colour target and a
  copy, and has no MSAA path (the multisampled depth cannot serve a single-sample transparent pass). It touches the
  colour-pass order `water-reflections` also changes. Recorded as finding F11 and a ledger follow-up with its own
  spec, not done here.
- **The post-processing colour tail.** With exposure, a tone curve or (without the uber pass) the grade on, the
  colour pass writes display-encoded colour (`RenderSystem.cpp:4258-4272`, `includes/post_color.glsl`), and the
  composite treats it as sRGB-encoded linear (`composite.frag:151`). A correct swap needs the linear HDR scene colour,
  which RGBA8 targets do not hold and float targets do not give on every GLES3/WebGL2 device. Documented residual
  and follow-up; the swap stays exact when the tail is off (the default: `SceneSettings.h:127-128`).
- **G-buffer fidelity beyond occlusion and UV sets**: normal maps (the G-buffer keeps the vertex normal,
  `gbuffer.vert:117-131`), hex tiling (S11 `PBR_HEX_*`), Lit Quality's specular anti-aliasing roughness
  (`mesh.frag:349`), the extended lobes (clearcoat keeps its own environment lobe; anisotropy bends the base lobe's
  reflection, `includes/pbr_ext.glsl:522`), and f0 from IOR / KHR_materials_specular. Each leaves a bounded residual
  (section 7.4); writing the shading normal into the G-buffer is the follow-up that removes most of it.
- **The volumetric fog's marched segment**: the composite attenuates by the analytic height fog over the whole ray;
  the volumetric fog replaces the near part of it with its marched medium (same extinction, so the difference is
  its noise and shadowing). Residual, documented.
- New SSR settings, a Lua API (no new runtime behaviour a script drives; `Scene.ssrDebugMode` already exists), a new
  MCP tool.
- `d72c16153` (runtime probe captures prefiltered into roughness mips) and `096a36a3c` (SSR fades out on rough
  surfaces): already in Jasper (dossier: `RenderSystem.cpp:2982-3063`, `ssr.frag:23,92-95`).

## 3. Users and stories

- Human from Unity: puts a Reflection Probe in a hall with a polished marble floor and turns SSR on. The pillars
  reflect in the floor; the rest of the floor shows the probe's capture. Today the floor gets the probe reflection
  *and* the SSR reflection on top, visibly brighter where SSR hits; after this change it matches Unity HDRP, where SSR
  takes over from the probe by its confidence.
- Human: a metal grate with a baked AO map in a corner. Today SSR brightens the occluded slats (SSR is added
  unoccluded) and darkens the open ones (the environment it removes was never occluded by the composite); after,
  the corner keeps its AO with or without SSR.
- Human: a wet street in thick fog. Today a reflection 200 m away shows through the fog at full strength; after, it
  fades with the street.
- Human: an unlit emissive panel or a planar mirror over SSR. Today SSR adds a second reflection; after, SSR leaves
  them alone (Unity: Unlit receives no SSR; a planar mirror already is the reflection).
- Agent asked "the floor is too shiny with SSR": `set_scene_settings ssr_debug_mode 7` (Reflection Source) and a
  Game view `screenshot` show which surfaces SSR swaps against the sky, which against a probe slot, which only get SSR
  added, which none; `get_render_stats` lists the probes with slots and those without. Mode 8 (Occlusion) shows the
  occlusion the swap is weighed by.
- Agent building a level with 6 probe volumes in view: `get_render_stats` says `ssr.probes_without_slot: 2` with
  their names; the guide says the smallest on screen keep their probe reflection without SSR, and how to fix it
  (fewer, larger probes in a view).

## 4. Findings

F1. **Local probes: SSR added on top of the probe reflection.** The G-buffer pass flags a mesh under a probe with
`iblSource = 1.0` (`RenderSystem.cpp:9904`, from `selectReflectionProbe` at `:9989-9991`); the composite rebuilds
the environment only for `0.5` (sky, `composite.frag:124`), so `iblSpec` stays 0 and SSR is added
(`composite.frag:152`) over the probe term the colour pass already added (`includes/ibl.glsl:77-85`,
`mesh.frag:373`). Doriax `a26ec5608` fixes this with four per-view probe slots.

F2. **Occlusion ignored by the swap.** The colour pass multiplies the environment specular by the material AO map
(`mesh.frag:402-405`), the painted terrain's occlusion (`:409-410`) and SSAO (`:428`). The composite subtracts the
rebuilt term unoccluded and adds SSR unoccluded (`composite.frag:150-152`): at full confidence an AO of `a` leaves
`ssr - (1 - a) * env`, darker crevices and unoccluded reflections in them. Doriax adds an R8 G-buffer attachment
for the material AO and multiplies by SSAO; it does not apply the map's UV set selection in full nor Stochastic
Tiling's taps (dossier note).

F3. **No early-out.** Every non-background pixel reads depth, normal, albedo and (sky pixels) a cube level, and runs
the BRDF fit (`composite.frag:79-154`), although SSR's mask is 0 on most pixels. `ssr.frag` marches every pixel
below the roughness cut (`ssr.frag:89-95`), including surfaces the composite should leave alone.

F4. **Probe selection for meshes that are not drawn.** `renderGBufferPass` runs `selectReflectionProbe` (a loop
over every probe) and a 4x4 inverse for every visible-layer opaque mesh before `drawMeshGBuffer` culls it
(`RenderSystem.cpp:9985-9999`, culling at `:9841-9843`). Doriax `1ed266ea3`: only drawn IBL meshes take a slot,
and a slot is matched by everything it holds (probes can share an authored cubemap).

F5. **The composite's environment is not the colour pass's.** The composite takes the first Skybox's
`prefilteredMap` whenever it exists and tints it by the sky colour (`RenderSystem.cpp:10199-10213`); the colour pass
binds the sky maps at load when both maps exist (`:6639-6652`) and tints by `environmentInfo()`, which is white
when the scene has no sky IBL (`:1588-1598`), e.g. with reflection probes only.

F6. **SSR added to surfaces that have no reflection.** Unlit materials (`MaterialLighting::NONE`, Receive Lights
off) and planar mirrors (`USE_MIRROR`) write `iblSource 0` and get SSR added (`composite.frag:146-152`).

F7. **Ambient specular double-counted.** Built-in Lit without IBL adds `envRadianceGGX(ambient)` as its environment
specular (`mesh.frag:381-396`); with `iblSource 0` the composite adds SSR on top of it.

F8. **Fog ignored by the swap.** The colour pass fogs the surface after its specular (`mesh.frag:529`); the
composite adds the swap unattenuated, so distant reflections show through fog.

F9. **The G-buffer reads the wrong UV set.** Base colour and metallic-roughness are always read on UV1
(`gbuffer.vert:144-153`, `gbuffer.frag:67,81`); Lit reads each map on the set the material names
(`includes/pbr.glsl:28-41`, uploaded at `RenderSystem.cpp:8128-8133`). With a map on UV2, SSR sees another albedo
and roughness than the surface drawn.

F10. **Intensity above 1 removes more environment than exists.** `w = mask * intensity` (Inspector range 0..2,
`InspectorWindow.cpp` "Intensity" under Reflections) multiplies `ssr - env` (`composite.frag:150-152`): at 2 the
environment term is subtracted twice.

F11 (out of scope, section 2). Transparents, near-plane videos, screen fluids and HTML UI documents are in the SSR
scene colour (`RenderSystem.cpp:16393-16426` before `:16441`): reflected by SSR, and given the swap of the opaque
surface under them.

F12 (out of scope, section 2). The post-processing colour tail makes the scene colour display-encoded
(`RenderSystem.cpp:4258-4272`); the composite's linear maths is then off by the curve.

## 5. Product decisions

D1. **Replace, never add (Unity HDRP / Godot model).** For every surface whose environment specular the composite
can rebuild exactly, `out = scene + T * ao * (w * ssr - min(w, 1) * env)`, with `w = mask * intensity`, `ao` the
colour pass's occlusion, `T` the fog transmittance. *Why:* it is what the colour pass would have drawn with SSR as
its environment; `min(w, 1)` keeps Intensity a strength multiplier of the reflection (values above 1 still brighten
it, as today) without removing environment that is not there (F10).

D2. **Each opaque pixel carries a reflection source** (8-bit code in G-buffer `color[2].a`, replacing the 0 / 0.5 / 1
flag):

| Code | Name | Surfaces | Composite |
|---|---|---|---|
| 0 | Skip | unlit materials (Lighting None, Receive Lights off), planar mirrors, codes 253-255 | untouched, no march |
| 1 | Add | Lit drawn without lighting (the scene has no lights and no IBL), shader-library lit shaders (their environment is their own) | `scene + T * w * ssr` (no occlusion: these variants apply none) |
| 2 | Ambient | built-in Lit (and forks) without IBL | swaps `ambient * FssEss` |
| 3 | Sky | built-in Lit with IBL, outside every probe | swaps the sky's prefiltered map |
| 4 | Probe without slot | under a probe that got no slot this frame | untouched, no march |
| 5 + 62 s + (q - 1) | Probe s, weight q/62 | under the probe in slot `s` (0..3), blended over the sky by `q / 62` (q 1..62) | swaps `mix(sky, probe * intensity, q / 62)` |

*Why:* one byte, no new attachment for it, exact round trip through RGBA8; the "Skip" and "Add" rows fix F6 and keep
today's behaviour where nothing better is known. *Why skip unlit:* Unity HDRP's Unlit receives no SSR, and an unlit
surface never had a specular for SSR to replace. *Why skip mirrors:* a planar mirror's colour is already the
reflection.

D3. **Four probe slots per view** (`SSR_PROBE_SLOTS 4`, as Doriax). The composite then binds 12 textures (scene,
SSR, three G-buffer, occlusion, SSAO, sky, four probes), 4 below the 16-unit floor of GLES3/WebGL2 fragment stages,
which leaves room for later screen effects. Samplers cannot be indexed dynamically on GLES/WebGL: the slot picks a
branch.

D4. **Slots go to the probes covering the most screen, with hysteresis.** Coverage = the sum, over the drawn
probe-lit meshes of each probe, of their world bounds' projected screen fraction (1 when a corner is behind the
camera). A probe that held a slot last frame competes with 1.25x its coverage. Ties: lower entity id. *Why:* Doriax
gives slots in transform order, so a far vase can take a slot from the floor; ranking by coverage keeps SSR on what
the player sees, and hysteresis stops SSR flickering on small objects when two probes trade places.

D5. **A probe without a slot keeps its probe reflection, without SSR** (code 4), counted in `get_render_stats` and
logged once per scene: `SSR: 6 reflection probes in view, 4 have SSR slots; surfaces under 'Probe Attic', 'Probe
Stair' keep their probe reflection without SSR.` *Why:* never brighter than the truth (the alternative, Doriax's
additive fallback, is the bug), and with D4 the overflow lands on the smallest surfaces.

D6. **Slots are keyed by probe entity**, not by cubemap. Two probes sharing one authored cubemap with different
boxes, intensities or capture positions take two slots (the case `1ed266ea3` fixes by comparing every value;
the entity is the value set). One probe's box, intensity and LOD are the same for every mesh; only the blend weight
is per mesh, and it rides in the code.

D7. **The occlusion is the colour pass's, read the colour pass's way.** A new R8 `color[3]` holds the material AO
map sample (on the map's UV set, through Stochastic Tiling's lattice when the material uses it) times the painted
terrain's occlusion; the composite multiplies by SSAO, sampled at the same coordinate the colour pass samples it.
*Why:* exact cancellation (`a26ec5608` reads it on UV1/UV2 only, plainly). Hex-tiled maps stay a residual (section 2).

D8. **The G-buffer reads every map on the material's UV set** (base colour, metallic-roughness, occlusion), fixing
F9 in the same change that needs UV2 for the occlusion map.

D9. **The swap is attenuated by the scene fog** in front of the surface, evaluated with the colour pass's own values
(distance fog factor, or the analytic height fog transmittance), fixing F8. No texture, a few ALU on reflecting
pixels only.

D10. **One source of the environment** for the colour pass and the composite: a helper returns the sky's
prefiltered map and its colour and rotation as the colour pass binds them (F5).

D11. **Debug views** (`ssr_debug_mode`, Inspector > Scene > Reflections (SSR) > Debug View): 6 is renamed "Environment
Specular" (it now shows the rebuilt term for every source), 7 "Reflection Source" and 8 "Occlusion" are added.
Values 0-6 keep their meaning. `set_scene_settings` refuses values outside 0-8.

D12. **Not taken from Doriax:** its additive fallback for probes without a slot (D5), slot order by first use (D4),
its value-by-value slot match (D6), AO on the plain UV only (D7).

## 6. Design

### 6.1 Reflection sources on the CPU

`engine/core/component/MeshComponent.h`, `Submesh`: `uint8_t ssrSource = 0;` next to `hasIBL`, one of
`SSRSource::{SKIP, ADD, AMBIENT, ENVIRONMENT}` (new enum in `engine/core/render/SSRProbeSlots.h`, below).
`ENVIRONMENT` becomes Sky, Probe or Probe-without-slot per draw.

`RenderSystem::getSubmeshShaderVariant` (`RenderSystem.cpp` ~3720-3970) sets `variant.ssrSource` from what it
already decides:

- `p_unlit` because the material's lighting is `NONE`, the mesh does not receive lights, or (`manifest`) the
  manifest's mode is unlit: `SKIP`. `inputs.mirror`: `SKIP`.
- `p_unlit` only because the scene has no lights and no IBL (`!(environment.lights || useIBL)` with a lit material):
  `ADD`. A manifest shader with a lit mode: `ADD`.
- Built-in Lit or a fork (no manifest), lit: `p_ibl ? ENVIRONMENT : AMBIENT`.

`loadMesh` copies it into `mesh.submeshes[i].ssrSource` beside `hasIBL` (`RenderSystem.cpp:6326`). The enum value is
computed for every variant (one assignment); nothing else runs while SSR is off.

### 6.2 G-buffer variant: occlusion and UV sets

`ShaderPool` (`engine/core/pool/ShaderPool.cpp`): GBUFFER bits 16 `Occ` ("Occlusion Texture") and 17 `Uv2`
("UV Set 2"); `getShaderPropertyCount(GBUFFER)` 16 -> 18; `getShaderPropertyName` cases;
`getGBufferMeshProperties(..., bool occlusionTexture, bool uvSet2)`; `getFallbackProperties` GBUFFER order
`{16, 8, 0}` (`Occ Mrt Bct`). `ShaderVariantCompiler.cpp:436` `addGBufferMeshPropertyDefinitions`:
`HAS_OCCLUSION_TEXTURE`, `HAS_UV_SET2`.

`getSubmeshShaderVariant`, G-buffer part (`RenderSystem.cpp:3952-3966`):

- `g_occlusion = (ssrSource is AMBIENT or ENVIRONMENT) && inputs.occlusionTexture && (p_hasTexture1 || p_hasTexture2)`
  (the colour variant samples the map exactly then: `includes/pbr.glsl:181-190`, `mesh.frag:401-406`), terrains
  included (they read it on the terrain's tiled UV, as Lit).
- `g_uv2 = !inputs.terrain && p_hasTexture2 && material sets any of base colour / metallic-roughness / occlusion
  (when sampled) to set 1`. The material's sets are an input already read for the colour pass; add them to
  `SubmeshShaderInputs` if the variant cannot see them (it must, for the export collector, which calls the same
  function: `editor/shader/ShaderVariantCollector.cpp:647`).
- `g_stochastic` also when the occlusion map is the only map (`stochasticAllowed && inputs.texCoord1 && (base || MR || g_occlusion)`).

Loading (`RenderSystem.cpp:6980-7095`): the G-buffer attribute filter adds `AttributeType::TEXCOORD2` (the two
filters at `:7038` and `:7067`); `loadGBufferTextures` (`:5424`) binds `TextureShaderType::OCCULSION` to
`material.occlusionTexture.getRender(&emptyWhite)` when the variant declares it.

The texture-edit reload that `render-correctness` (Doriax `17ce79969`) adds for G-buffer variants must include the
occlusion map and the UV sets (section 13).

### 6.3 G-buffer shaders and target

`gbuffer.vert`: `a_texcoord2` / `v_uv2` under `HAS_UV_SET2`; `v_uv1` also under `HAS_OCCLUSION_TEXTURE`; the terrain
rule of `:34-43` kept (no texcoord attributes on terrain).

`gbuffer.frag`:

- outputs `layout(location = 3) out vec4 frag_occlusion;`
- material block: after `stochastic` (declared as padding when `Sto` is off and `Uv2` on, the pattern of
  `mesh.frag`'s `stochasticPadding`), `vec4 texCoordSets` under `HAS_UV_SET2`: x base colour, y metallic-roughness,
  z occlusion (0 = UV1, 1 = UV2). Floats, not ints (a GL block is typed by its first member). CPU struct
  `fs_gbuffer_material_t` (`RenderSystem.h:253`) gains the vec4; the upload size logic at `RenderSystem.cpp:9911-9914`
  covers `Sto` and `Uv2`.
- maps read through `selectUV(set)`; with Stochastic Tiling, the lattice of each set as Lit sets them up
  (`includes/pbr.glsl:70-124`: set 1 seed 0 with the base colour's luminance weights, set 2 seed 1, geometric
  weights unless the base colour is on set 2).
- `occlusion = texture(occlusion map).r` (or the stochastic tap) times `terrainSurface.occlusion` under
  `HAS_TERRAIN_PBR`; `mesh.frag`'s `occlusionStrength` is the constant 1.0 (`mesh.frag:210`), so no strength term.
- `frag_albedo.a = code / 255.0` (`gbufferMaterial.params.z`, now the code over 255).

`SokolObject.cpp:770-787` `PIP_GBUFFER`: `color_count = 4`, `colors[3].pixel_format = SG_PIXELFORMAT_R8`;
comments and `Render.h:429` updated. `ensureGBufferFramebuffer` (`RenderSystem.cpp:9805-9831`): four attachments,
`ColorFormat::RED` last. The clear colour (1,1,1,1) gives occlusion 1. R8 is colour-renderable on GL 3.3, GLES3,
WebGL2, Metal, D3D11 and Vulkan; four draw buffers is the GLES3/WebGL2 minimum.

### 6.4 Probe slots: `engine/core/render/SSRProbeSlots.{h,cpp}` (new, pure, unit-tested)

```cpp
namespace jasperengine {
    constexpr int SSR_PROBE_SLOTS = 4;                 // composite.frag, includes/ssr_source.glsl
    constexpr int SSR_PROBE_WEIGHT_STEPS = 62;
    enum class SSRSource : uint8_t { SKIP, ADD, AMBIENT, ENVIRONMENT };
    namespace SSRCode {
        constexpr uint8_t SKIP = 0, ADD = 1, AMBIENT = 2, SKY = 3, PROBE_NO_SLOT = 4, PROBE_BASE = 5;
        uint8_t probe(int slot, float weight);          // 5 + slot * 62 + (max(round(min(weight,1) * 62), 1) - 1)
        bool decodeProbe(uint8_t code, int& slot, int& q);
    }
    static_assert(SSRCode::PROBE_BASE + SSR_PROBE_SLOTS * SSR_PROBE_WEIGHT_STEPS <= 256);

    // the screen fraction (0..1) of a world box's projection; 1 when a corner is behind the eye
    float screenCoverage(const AABB& box, const Matrix4& viewProjection);

    struct SSRProbeCandidate { Entity probe; float coverage; };
    struct SSRProbeAssignment {
        Entity slots[SSR_PROBE_SLOTS];                 // NULL_ENTITY when free
        int slotOf(Entity probe) const;                // -1 = no slot
    };
    // ranks by coverage (x1.25 for probes in `previous`), ties by entity; at most SSR_PROBE_SLOTS
    void assignSSRProbeSlots(const std::vector<SSRProbeCandidate>& candidates, const SSRProbeAssignment& previous,
                             SSRProbeAssignment& out);
}
```

`candidates` holds one entry per distinct probe (coverage summed by the caller).

### 6.5 G-buffer pass: collect, assign, draw

`RenderSystem::selectReflectionProbe` (`RenderSystem.cpp:2700`) gains `Entity* selectedProbe = nullptr` (out).

`renderGBufferPass` (`:9958`) becomes three steps over per-view storage reused every frame
(`rv().ssr`, a struct with `std::vector<GBufferDraw> draws`, the candidate vector, the assignment, last frame's
assignment, the bound probe textures and params, and the stats of 6.8):

1. **Collect.** For each mesh the current loop admits (`:9967-9979`), skip it unless it draws: `mesh.loaded &&
   !mesh.needReload`, not `terrain->needUpdateTexture`, `isBatchVisible(...)` and `selectMeshDraw(...)` true (both
   moved out of `drawMeshGBuffer`, which takes the record's `MeshDraw`), and at least one submesh has a
   `gbufferShader`, `vertexCount > 0`, `!skipsScreenEffects`. Then, only if a drawn submesh has `ssrSource ==
   ENVIRONMENT` and `hasReflectionProbes`: `selectReflectionProbe(meshProbeSamplePosition(...), params, texture,
   &probe)` with weight > 0.001 records `probe`, `params`, `texture`, and adds `screenCoverage(mesh.worldAABB,
   camera.viewProjectionMatrix)` to the probe's candidate. The normal matrix is computed here, once, for drawn
   meshes only (F4).
2. **Assign.** `assignSSRProbeSlots(candidates, previous, current)`; the slot's `fs_composite.probe*[s]` are filled
   from the first record of that probe, its texture kept for 6.6. `previous = current` after the frame.
3. **Draw.** `drawMeshGBuffer(record, ...)`: per submesh, the code from `ssrSource`:
   `SKIP/ADD/AMBIENT` directly; `ENVIRONMENT`: no probe -> `SKY`; slot `s` -> `SSRCode::probe(s, weight)`; no slot ->
   `PROBE_NO_SLOT`.

Overflow log (D5): `Log::warn` once per RenderSystem until the next scene load, naming up to four probes without a
slot by entity name.

### 6.6 Composite (`engine/shaderlib/composite.frag`, `renderSSR` `RenderSystem.cpp:10129-10239`)

New include `engine/shaderlib/includes/ssr_source.glsl`: the code constants and `ssrSourceCode(float alpha)` (round
of `alpha * 255`), used by `ssr.frag` and `composite.frag`. The existing `ibl.glsl`, `fog.glsl` and `height_fog.glsl`
are **not edited** (the MESH zero-cost hashes, `engine/tests/core/shader/ShaderZeroCostTest.cpp`, cover their IBL and
fogged variants): the composite includes `ibl.glsl` without `USE_IBL` (for `ENV_GGX_LODS`, `envBRDFApprox`; its own
copy goes) and `fog.glsl` with `HAS_HEIGHT_FOG` defined (declares `u_fs_fog` and `u_fs_heightFog`, gives
`distanceFogFactor` and `heightFogOpticalDepth`), and keeps its own `probeBoxProject` and `sampleProbe` written as
`ibl.glsl:41-85`, parameterised by slot.

Bindings added: `u_occlusionGBufferTexture` (G-buffer `color[3]`), `u_ssaoTexture` (`rv().currentSSAOTexture`, white
when SSAO is off), `u_probeTexture0..3` (slot textures, `emptyCubeBlack` when free). Slots looked up once in
`loadSSR` (`getTextureIndexByName`), reset in `destroySSR`.

`fs_composite_t` (`RenderSystem.h:282`, mirrored in the shader):

```
mat4 invProjection; mat4 invView;
vec4 params;     // x intensity, y flip G-buffer Y, z debug mode, w flip destination Y
vec4 envColor;   // rgb sky colour (linear), w sky rotation (radians): environmentInfo()
vec4 ambient;    // rgb the scene ambient Lit adds (litFrame.ambient_fogType.rgb), w fog mode: 0 none, 1 distance, 2 height
vec4 projectionW;// the projection's 4th row: clip w of a view position (fog distance)
vec4 probePosition[4]; // xyz capture position
vec4 probeBoxMin[4];   // xyz box min, w intensity
vec4 probeBoxMax[4];   // xyz box max, w max mip LOD (0 = unfiltered capture)
```

The fog blocks get the colour pass's values: the frame fog after the editor's Fog toggle (`fillFrameLighting`,
`RenderSystem.cpp:1604-1615`) and `passHeightFog` as the colour pass uploaded it (`:15994`); mode 0 when the
scene's variants have no fog (`environment.fog` false or the editor toggle off).

Environment (D10): `RenderSystem::skyEnvironment(TextureRender*& prefiltered)` returns `environmentInfo()` and the
texture the colour pass binds (`:6639-6652`'s rule, `emptyCubeBlack` otherwise); the load binding and `renderSSR`
both call it.

Per pixel (`mode` = debug mode):

```
scene, refl at c;  w = refl.a * intensity
if mode == 0 && w < 1/1024: out = scene                                    // most pixels: 2 reads
depth at d; background -> as today
code = ssrSourceCode(albedo.a at d)
if mode == 0 && (code is SKIP or PROBE_NO_SLOT): out = scene              // the blur can spread mask onto them
P, nV, roughness, metallic, albedo, FssEss                                  // as today
env = 0; ao = 1
if code == AMBIENT: env = ambient.rgb
if code == SKY or a probe slot:
    sky = sky prefiltered at rotate(rW), roughness * ENV_GGX_LODS, times envColor.rgb
    env = probe ? mix(sky, sampleProbe(s, probeBoxProject(rW, worldPos, s), lod) * boxMin[s].w, q / 62) : sky
if code >= AMBIENT: ao = occlusion at d * ssao at d
ssr = sRGBToLinear(refl.rgb / max(refl.a, 1e-4)) * FssEss
T = fogTransmittance(P)            // 1 (mode 0), distanceFogFactor(depth01 * dot(projectionW, vec4(P,1))),
                                   // or max(exp(-heightFogOpticalDepth(origin, dir, 0, |worldPos - origin|)), 1 - maxOpacity)
                                   // with origin/dir from invView/invProjection at d (no gl_FragCoord: the destination's may differ)
out = linearTosRGB(max(sRGBToLinear(scene) + T * ao * (w * ssr - min(w, 1) * env * FssEss), 0))
```

The sky lookup is skipped when `q == 62` (the probe covers it fully), as Doriax.

Debug views: 1-5 as today; 6 Environment Specular `env * FssEss` (0 for Skip/Add/no slot); 7 Reflection Source:
Skip black, Add magenta, Ambient mid grey, Sky blue (0.2, 0.45, 1), probe without slot orange, slot 0..3 red, green,
yellow, cyan at `0.35 + 0.65 * q / 62`; 8 Occlusion `ao` (1 for Skip/Add/no slot). Background pixels: as today.

### 6.7 SSR march (`engine/shaderlib/ssr.frag`)

After the roughness test (`:89-95`), read `color[2].a` and write 0 for Skip and Probe-without-slot (no march).
`renderSSR` binds `GBUFFERALBEDOTEXTURE` to the SSR pass too. One RGBA8 fetch, only on pixels that would march.

### 6.8 Editor, MCP, docs

- Inspector (`editor/window/InspectorWindow.cpp:12774`): Debug View names `Off, Reflection, Normal, Roughness,
  Metallic, Albedo, Environment Specular, Reflection Source, Occlusion`; tooltip on the combo: "Reflection Source
  colours what SSR swaps against: blue sky, red/green/yellow/cyan a probe slot, orange a probe without a slot (no
  SSR), grey the ambient, magenta added only, black none." Edits stay `ScenePropertyCmd` (undo).
- `SceneSettings.h:396` comment and `editor/mcp/tools/SceneTools.cpp:832` help: `"SSR debug view: 0 off, 1
  reflection, 2 normal, 3 roughness, 4 metallic, 5 albedo, 6 environment specular, 7 reflection source, 8
  occlusion"`; values outside 0-8 refused (`ssr_debug_mode 9: the SSR debug views are 0-8`), the existing Int path.
- `get_render_stats` (`editor/mcp/tools/ProfilerTools.cpp:220-247`): for the selected scene with SSR on, an `ssr`
  object of the resolved view (the frame's view when the RenderSystem has no stats for it):
  `{"probe_slots":4, "probes_in_view":6, "probes":[{"entity":12,"name":"Probe Hall","slot":0,"coverage":0.41},
  ..., {"entity":31,"name":"Probe Attic","slot":null,"coverage":0.01}], "probes_without_slot":2,
  "meshes_drawn":143}`, from `RenderSystem::getSSRStats()` (filled in step 2 of 6.5). The text summary appends
  `SSR: 4/6 probes with slots` when any probe is in view. The tool description is unchanged (the output is not in
  the budgeted schema); `gen_tool_docs.py --check` decides whether TOOLS.md moves.
- Docs (new): `docs/rendering/reflections.md` for users (what SSR and reflection probes each give, how they combine,
  the four-slot rule and how to stay under it, occlusion and fog, the debug views, what does not receive SSR, the
  residuals of 7.4, cost), and `docs/mcp/guide/reflections.md` for agents (verify with modes 7 and 8 and
  `get_render_stats ssr`; fix overflow; what each source colour means). The agent guide's docs list
  (`editor/mcp/McpInstructions.cpp:195-198`) gains `reflections (SSR, reflection probes: ssr_debug_mode 7)`
  within `tools/mcp/budget.json`'s guide budget.
- Lua: none new. The `Scene.ssrDebugMode` doc text in the Lua API data lists the nine values (`tools/lua_api`,
  regenerated by the build).

### 6.9 Caches and warm-up

- `editor/App.cpp:5365` shader cache `v44` -> `v45` with a line: "v45: SSR reflection sources (G-buffer color[3]
  occlusion, GBUFFER 'Occ'/'Uv2', code in color[2].a), composite rebuilds probes/ambient and reads fog".
- `engine/core/shader/ShaderWarmup.cpp` loader: a pipeline record of a GBUFFER shader with `color_count` 3 is
  upgraded to the four-attachment layout (`colors[3]` R8) so lists recorded before this change still warm the
  pipeline the engine asks for; nothing else changes in the format (version stays 1).

### 6.10 Platforms

All shader work is GLSL 450 through sokol-shdc to GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan; no new backend
code. Probe cubes are chosen by branches on a uniform-derived slot (no dynamic sampler indexing; GLES). Cube and 2D
lookups in the composite use `textureLod`/`texture` outside loops; `ssr.frag`'s new read is before its loop. The
R8 attachment and the 12 composite textures are within every backend's limits (6.3, D3). Windows, macOS and iOS
builds are not built here: the change is in shared shaders and `SokolObject.cpp`'s backend-independent pipeline
description, compiled for those targets by the existing paths.

## 7. Behaviour rules and edge cases

7.1 Codes per submesh, not per mesh: a mesh with a Lit and an Unlit submesh writes Sky (or a probe) and Skip side
by side. A probe slot is taken when any drawn submesh of the mesh is `ENVIRONMENT`.

7.2 The probe weight is per mesh (`meshProbeSamplePosition`), quantised to 62 steps with at least one step:
`round(min(w,1) * 62)`, minimum 1. Between 0 and 0.001 the mesh is Sky (as today's threshold).

7.3 Instanced batches, foliage and terrains take one probe for the batch, as the colour pass. Tilemaps: as meshes.

7.4 Residuals, by design (section 2; documented in `docs/rendering/reflections.md`): normal maps (the swap uses the
vertex normal: normal-map detail of the environment stays visible under a confident SSR hit), hex-tiled AO maps,
specular AA roughness, extended lobes (clearcoat's own environment lobe stays, which is right; anisotropy's bent
reflection and specular/IOR f0 are approximated by the base lobe), probe weight quantisation (at most 1/124 of the
probe-sky difference, only inside a probe's blend band), R8 occlusion quantisation (1/510 of the swap), the
volumetric fog's marched segment, the post-processing colour tail.

7.5 A mesh loaded before SSR was turned on has no G-buffer shader and is skipped (today's rule); it takes no slot.

7.6 Debug views never early-out; Skip pixels in mode 6 and 8 show 0 and 1.

7.7 Intensity 0: every pixel early-outs (the composite copies the scene); the march still runs (out of scope).

7.8 A runtime probe still capturing its first faces has no texture and is not selected (`selectReflectionProbe`
returns false); one with only its base level samples with the 5-tap spread, as `ibl.glsl`.

7.9 The editor's Scene view and Game view keep separate slot assignments and hysteresis (`rv()`).

7.10 Orthographic cameras: fog distance is the window depth (clip w 1), ray origins on the near plane, as the colour
pass's height fog.

7.11 A scene with probes but no sky IBL: the sky term is the black cube times white, so Sky surfaces swap 0 (SSR is
then effectively added, which is the truth for them).

## 8. Performance and cost

- **SSR off: nothing.** No G-buffer pass, no composite, no collect step. The only new work is one enum assignment
  per submesh variant at load; no new uniform, texture or variant bit in any MESH variant (the zero-cost hashes do
  not move).
- **SSR on, per frame (1920x1080 as the yardstick):**
  - G-buffer: +1 R8 attachment, 2.07 MB, written once per covered pixel; an extra texture tap only for submeshes with
    an AO map (`Occ`), an extra varying only with `Uv2`.
  - SSR march: +1 RGBA8 fetch on pixels below the roughness cut.
  - Composite: pixels with `w < 1/1024` read 2 textures (today 5 plus a cube level on sky pixels); reflecting pixels
    add the occlusion and SSAO reads, up to two probe taps (5 for an unfiltered capture), and the fog maths (about
    20 ALU for height fog). Expected cheaper overall; measured, not gated (AC 14).
  - CPU: probe selection and the normal matrix only for drawn meshes (fewer than today), 8 corner transforms per
    drawn probe-lit mesh, a sort over the probes in view; the per-view vectors are reused (no allocation per frame
    once warm).
- Mobile and web: 4 MRT and 12 fragment textures are within the GLES3/WebGL2 minimums; no float targets.

## 9. Compatibility

- No file format changes. Scenes keep `ssrDebugMode` 0-6 with the same meaning (6 renamed only in the UI); 7 and 8
  are new values, unknown to older engines (which draw them as mode 6, the shader's `else`).
- Visible changes on load, all corrections: probe-lit surfaces under SSR lose the doubled reflection, AO and fog
  apply to reflections, unlit surfaces and planar mirrors no longer receive SSR, base colour and roughness on UV2
  reach SSR. Noted in `docs/rendering/reflections.md` ("Changes in 2026-11").
- Shader cache v45 (6.9). GBUFFER keys gain `Occ`/`Uv2` only for materials with an AO map or maps on UV2; exports
  collect them through the same variant function. Prebuilt players are engine builds and are rebuilt with it.
- Warm-up lists recorded before: upgraded on load (6.9).

## 10. Tests

Suites per `docs/testing.md`. "Red" = the test fails on `main` before the change; the run that shows it is kept in
the PR (a run of the new test on the parent commit, with the shaders of that commit).

### 10.1 `jasperengine-core-tests`, CPU: `engine/tests/core/render/SSRProbeSlotsTest.cpp`

- `SSRProbeSlots.CodesFitOneByteAndRoundTrip`: every (slot 0-3, q 1-62) encodes to 5..252 and decodes back;
  Skip/Add/Ambient/Sky/no-slot are 0-4.
- `SSRProbeSlots.WeightTakesAtLeastOneStep`: weight 0.002 -> q 1; 1.0 and 1.7 -> q 62.
- `SSRProbeSlots.LargestCoverageGetsTheSlots`: six candidates, the four largest get slots, slot order stable.
- `SSRProbeSlots.ProbesSharingACubemapTakeTwoSlots` (`1ed266ea3`): two entities, same texture, two slots.
- `SSRProbeSlots.HysteresisKeepsLastFramesHolder`: a holder at 0.30 keeps its slot against 0.36, loses it to 0.40.
- `SSRProbeSlots.TiesBreakByEntity`.
- `SSRProbeSlots.ScreenCoverageClampsAndCountsBehindAsFull`: a box filling half the view 0.5 +- 0.01; a box beside
  the frustum 0; a box around the eye 1.

### 10.2 `jasperengine-editor-tests`: variants and keys

- `editor/tests/stream/SSRSourceVariantTest.cpp` (with `RenderSystem::getSubmeshShaderVariant`, as
  `StochasticTilingTest.cpp:100`):
  `SSRSourceVariant.UnlitAndMirrorSkip`, `.LitWithoutLightsAdds`, `.ManifestLitAdds`, `.LitWithoutIBLIsAmbient`,
  `.LitWithIBLIsEnvironment`, `.OcclusionMapTakesOccOnlyWhenSwapped` (Occ for Ambient/Environment with UV, not for
  Add or Skip), `.MapsOnUV2TakeUv2` (red for F9 in spirit: no such bit before), `.TerrainNeverTakesUv2`.
- `editor/tests/engine/pool/ShaderFallbackTest.cpp`: `ShaderFallback.GBufferGivesUpOcclusionFirst`.
- `editor/tests/stream/ShaderKeyTest.cpp`: GBUFFER names `Occ`, `Uv2` round-trip through key text.

### 10.3 `jasperengine-core-tests`, GPU: `engine/tests/core/render/SSRCompositeGpuTest.cpp`, suite `SSRCompositeGpu`

Scene: perspective camera over a mirror floor (Primitive plane, metallic 1, roughness 0.05), a white unlit box
standing on it so SSR hits over a known block of floor pixels in the middle of the frame, a directional light. "SSR
off" renders the same scene with `ssr_enabled` false. Pixels are read from `GpuEnvironment::target()`; tolerances
are per backend (glcore, lavapipe).

1. `LocalProbeIsReplacedNotAdded` **red**: a static probe whose six faces are solid red (4x4 PNGs written to the
   test's `TempDir`), box around the floor, no sky. In the hit block, SSR on minus "SSR on with the probe removed"
   is within 3/255 per channel. Before: red is higher by about the probe term (> 0.25).
2. `MaterialOcclusionOccludesTheSwap` **red**: the floor gets an AO map black on its left half, white on the right,
   a sky with IBL. Left-half hit pixels equal SSR off within 2/255 (AO 0: nothing to swap); right-half pixels differ
   from SSR off. Before: the left half differs (SSR added, environment subtracted).
3. `OcclusionOnUV2IsReadOnUV2`: the same map on UV2 with a UV2 that mirrors U; the dark half follows UV2.
4. `ScreenOcclusionWeighsTheSwap`: SSAO on; in debug mode 8 a floor pixel at the box's foot equals (material AO 1) x
   the SSAO buffer at the same pixel (SSAO's raw view, `ssao_debug`), within 3/255; red: before, mode 8 does not
   exist and draws mode 6.
5. `FogAttenuatesTheSwap` **red**: exponential fog dense enough that the hit block's fog factor is under 0.02; SSR on
   equals SSR off within 2/255 there. Before: the reflection shows through.
6. `HeightFogAttenuatesTheSwap`: the same with a height fog of the same transmittance (not red-by-design: height fog
   rides on the same code path; red on `main` too, recorded).
7. `UnlitAndMirrorReceiveNoSSR` **red**: an unlit floor (metallic 1, roughness 0) equals SSR off exactly; a planar
   mirror floor the same.
8. `AmbientSpecularIsReplaced` **red**: no sky, no probe, bright ambient, a light; in the hit block SSR on minus
   "SSR on with ambient 0 and the same SSR input" is within 3/255. Before: the ambient specular stays under SSR.
9. `ProbesBeyondTheSlotsKeepTheirReflection`: five probes, each over its own floor tile, the smallest on screen far
   away; its tile equals SSR off within 1/255; `getSSRStats()` reports 5 in view, 1 without a slot; the warning is
   logged once over three frames (`LogCapture`).
10. `CulledAndNonIBLMeshesTakeNoSlot` (`1ed266ea3`): four off-screen probe-lit meshes and one Unlit mesh in a fifth
    probe, plus one visible probe-lit floor: the floor's probe holds slot 0 and `probes_in_view` is 1.
11. `PixelsWithoutReflectionAreUntouched` (guard, not red): rough floor (0.9) and the sky: SSR on equals SSR off to
    the bit.
12. `GBufferReadsTheMaterialsUVSets` **red** (F9): base colour on UV2 (a two-colour texture laid out differently on
    UV1 and UV2); debug mode 5 matches the colour pass's albedo split.
13. `IntensityAboveOneNeverRemovesMoreThanTheEnvironment` **red** (F10): intensity 2, a hit pixel is not darker than
    "SSR on at intensity 1" minus 1/255.

### 10.4 `jasperengine-core-tests`: `engine/tests/core/render/AsyncCreateTest.cpp`

`ShaderWarmup.UpgradesThreeAttachmentGBufferRecords`: a list with a GBUFFER pipeline of 3 colours loads with 4
(R8 last); other records unchanged.

### 10.5 MCP: `tools/mcp/tests/test_ssr_reflections.py` (`CI = True`)

- `test_debug_view_names_and_range`: `set_scene_settings ssr_debug_mode 8` accepted, `9` refused with the range in
  the message; `get_scene_settings` returns 8.
- `test_reflection_source_view`: a scene with a sky, one probe over half the floor and an unlit cube; Game view
  screenshot in mode 7 has blue on the sky-lit half, red on the probe half, black on the unlit cube (colour within
  10/255 of the documented values).
- `test_render_stats_report_probe_slots`: six probes with lit meshes in view; `get_render_stats` `ssr.probe_slots`
  4, `probes_without_slot` 2, both named; the text summary says `SSR: 4/6 probes with slots`.
- `test_render_perf.py::test_probe_ssr_and_shadows_render` (existing) stays green.

### 10.6 Zero cost

`ShaderZeroCost.DefaultVariantsUnchanged` stays green without updating its hash list (MESH variants untouched).

## 11. Acceptance criteria

1. With SSR on, a probe-lit mirror floor under a confident SSR hit equals the same floor without the probe within
   3/255 (10.3 #1, red before).
2. Material AO (on either UV set, plain or Stochastic Tiling), terrain occlusion and SSAO weigh the swap
   (10.3 #2-#4).
3. Distance and height fog attenuate the swap (10.3 #5-#6).
4. Unlit surfaces and planar mirrors are bit-identical with SSR on and off (10.3 #7).
5. Lit without IBL swaps its ambient specular (10.3 #8).
6. At most four probes per view get slots, the largest on screen first, with hysteresis; the rest keep their probe
   reflection, are counted, named and logged once (10.1, 10.3 #9, 10.5).
7. Off-screen meshes, meshes not drawn and meshes with no IBL submesh take no slot and no probe selection
   (10.3 #10).
8. Pixels without SSR are bit-identical to SSR off (10.3 #11).
9. The G-buffer reads base colour, metallic-roughness and occlusion on the material's UV sets (10.3 #3, #12).
10. Intensity above 1 never removes more environment than exists (10.3 #13).
11. Debug views 6-8 as named in the Inspector (undoable) and `set_scene_settings` (0-8, others refused) (10.5).
12. `get_render_stats` reports the `ssr` object (10.5).
13. SSR off costs nothing: no new pass, attachment, uniform or MESH variant change (`ShaderZeroCost` green; render
    stats of an SSR-off scene unchanged in passes and bindings, checked in 10.5's scene with SSR off).
14. The SSR composite pass time on the `test_render_perf.py` probe scene, before and after, is reported in the PR
    (`get_gpu_passes`, label "ssr composite", on a GPU build server; reported, not gated).
15. `tools/check-changed` green; the unit, GPU and MCP tests above pass on glcore and Vulkan (lavapipe); docs
    `docs/rendering/reflections.md`, `docs/mcp/guide/reflections.md`, the guide line, `gen_tool_docs.py --check`
    and `tool_budget.py --check tools/mcp/budget.json` clean.

## 12. Files likely touched

- Shaders: `engine/shaderlib/composite.frag`, `gbuffer.frag`, `gbuffer.vert`, `ssr.frag`; new
  `engine/shaderlib/includes/ssr_source.glsl`. Read only: `includes/ibl.glsl`, `fog.glsl`, `height_fog.glsl`,
  `pbr.glsl`, `stochastic.glsl`.
- Engine: `engine/core/subsystem/RenderSystem.cpp` (variant ~3720-3970, `selectReflectionProbe` 2700, load 6326 and
  6980-7095, `loadGBufferTextures` 5424, G-buffer and SSR 9805-10240, sky helper near 1588), `RenderSystem.h`
  (structs 253, 282; per-view SSR state; `getSSRStats`); new `engine/core/render/SSRProbeSlots.{h,cpp}`;
  `engine/core/component/MeshComponent.h` (`Submesh::ssrSource`); `engine/core/pool/ShaderPool.{h,cpp}`;
  `engine/core/shader/ShaderVariantCompiler.cpp`; `engine/core/shader/ShaderWarmup.cpp`;
  `engine/renders/sokol/SokolObject.cpp`; `engine/core/render/Render.h` (comments); `engine/core/SceneSettings.h`
  (comment).
- Editor: `editor/App.cpp` (cache v45), `editor/window/InspectorWindow.cpp` (Debug View), `editor/mcp/tools/SceneTools.cpp`,
  `editor/mcp/tools/ProfilerTools.cpp`, `editor/mcp/McpInstructions.cpp`.
- Tests: the files of section 10; `editor/tests/engine/pool/ShaderFallbackTest.cpp`, `editor/tests/stream/ShaderKeyTest.cpp`.
- Docs: new `docs/rendering/reflections.md`, `docs/mcp/guide/reflections.md`; `docs/mcp/TOOLS.md` if regenerated;
  `tools/lua_api` doc text for `Scene.ssrDebugMode`.

## 13. Cross-feature dependencies

- `render-correctness`: its G-buffer-variant reload on texture edits (`17ce79969`) must also react to the occlusion
  map and UV sets of 6.2; its "two UV sets without a normal map" work changes `p_hasTexture2`, which `Uv2` reads.
  Whoever lands second extends the other's check. Its normal-less meshes (`a_normal`) touch the same G-buffer
  attribute filters.
- `mesh-memory`: moves `Submesh` storage; `ssrSource` is one more byte in it.
- `draw-perf`: its culling and sort changes in the colour pass do not touch `renderGBufferPass`; if it adds a
  per-frame probe-selection cache, the G-buffer collect step should use it (same selection, one loop).
- `water-reflections` / `water-boats`: water is transparent and never in the G-buffer; F11 (SSR before transparents)
  is the shared follow-up with their colour-pass reordering.
- `export-pipeline`: GBUFFER shader keys gain `Occ`/`Uv2`; the collector's path is unchanged.
- Shader cache version (`editor/App.cpp`): every feature that edits shaders bumps it; merge by taking the next
  number with both comment lines.

## Deviations

Recorded during implementation (branch `dc/ssr-probes`).

1. **G-buffer fallback order** (6.2): `{16, 17, 8, 0}` (Occ, Uv2, Mrt, Bct) instead of `{16, 8, 0}`. Dropping
   `Uv2` keeps every uniform block prefix the draw uploads (the material block is uploaded at its reflected
   size) and only reads the maps on UV1, so it is a valid, nearer fallback than dropping the base colour.
2. **G-buffer Stochastic Tiling with two UV sets**: both lattices are set up whenever `Uv2` is on (Lit sets up
   only the sets its maps use). The set-up is a pure function of the UV and the material, so the taps are the
   same.
3. **"SSR off" in the GPU tests** (10.3) is the same SSR path at Intensity 0 (every pixel returned as the colour
   pass drew it), so a comparison measures the composite alone, not the offscreen-versus-direct colour pass.
4. **GPU test files**: the composite tests (`SSRCompositeGpuTest.cpp`) use only engine API older than the change,
   so the same file runs on the parent commit as the red control; the slot and stats tests are in
   `SSRProbeSlotsGpuTest.cpp`. 10.3 #3 (occlusion on UV2) is part of #12 (`GBufferReadsTheMaterialsUVSets`, both
   maps on UV2). #7's planar mirror is covered by `SSRSourceVariant.UnlitAndMirrorSkip` (the variant decides
   Skip; a Mirror scene adds nothing the unlit case does not test). #9 checks the stats and the one log line,
   not the far tile's pixels (the far tile reflects nothing to compare).
5. **`getSSRStats()`** reports the frame's view (`primaryView`, the Game view in the editor); `get_render_stats`
   reports it for the selected scene whatever `view` names.
6. **Agent guide** (lean MCP rules, lead decision on the guide): one entry in "Other areas", `reflections (SSR
   replaces probe reflections; check with ssr_debug_mode 7)`, instead of a line in the docs list; the eval case
   `guard_ssr_reflection_source` (`tools/mcp/eval/cases.py`) covers it and the new `ssr_debug_mode` help.
7. **Bits and cache version after the merges**: GBUFFER keeps bits 16 (`Occ`) and 17 (`Uv2`), free on `main`
   (S55 left GBUFFER at 16 bits); render-correctness also claims 16 ("Instance Fade"), and the second branch
   to land renumbers. The shader cache is v49, after S54, S55 and S57's v45-v48.
