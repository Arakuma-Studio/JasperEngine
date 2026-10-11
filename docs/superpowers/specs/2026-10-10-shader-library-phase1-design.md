# Shader library, phase 1: foundation, unified lighting and the first library

Status: approved design, ready for implementation.
Branch: `shader-library`. Phases 2-4 build on the contracts defined here and must not
redefine them; a later phase may only extend them.

## 1. Goal

Give JasperEngine a library of ready-made shaders that a user picks from a list, the way
Unity offers Standard / Unlit / Particles, plus the engine features that make those shaders
possible, without hand-forking GLSL. Performance is the governing rule: every feature is
paid for only by the objects that use it.

Phase 1 delivers:

1. The shader-system foundation: named library shaders, a properties block (the manifest)
   in shader source, custom textures, required vertex inputs, an engine "frame" uniform
   block, per-material render state, and the shader choice on the material.
2. Unified lighting: unlimited scene lights, a per-object light list under one cap, a
   per-light pixel/vertex/both option, and per-material pixel/vertex/mixed lighting.
3. 24 library shaders (section 9) on top of the built-in Lit shader.
4. Editor, serialization, export, Lua and MCP support for all of the above.
5. Verification tooling: an all-backend compile matrix for library shaders and a fixture
   project rendered by the smoke test.

Out of phase 1 (later phases): extra passes and stencil (phase 2), scene depth/colour in
mesh shaders (phase 3), the post-processing stack (phase 4).

## 2. Background

Research notes (scratch, outside the repo; read them for technique detail):

- `/tmp/shader-research/engine-capabilities.md`: how the shader system works today, with
  file:line evidence. Read sections 1-6 and 11 before touching shader plumbing.
- `/tmp/shader-research/inventory_*.md`: the owner's past Unity shaders, by technique.
- `/tmp/shader-research/unity-default-shaders.md`: Unity's default shaders and their
  properties.
- `/tmp/shader-research/src/JasperRaccoon/Minigames/Assets/Shaders/UnlitLights.shader` and
  `/tmp/shader-research/vl/*.cs`: the owner's original vertex-lighting system.

Key facts about today's engine:

- One built-in shader per `ShaderType`; mesh variants are property bits (MESH uses 28 of
  32 bits; bit 5 is reserved). `MATERIAL_UNLIT` is a variant, not a shader.
- Forks: `customShader` (a project path) on Mesh/UI/Points/Lines/Sky components, interned
  into the `ShaderKey` as a 16-bit custom id. Editor build, export precompile and runtime
  compile all key off that string (`ShaderPool.cpp`, `ShaderVariantCompiler.cpp`,
  `ShaderBuilder.cpp`, `Exporter.cpp`, `RuntimeShaderCompiler.cpp`).
- One user uniform block per stage (`u_vs_customParams` / `u_fs_customParams`) filled by
  name from `shaderUniforms`; no custom textures; render state hard-coded in
  `SokolObject::makePipelineDesc` (always SRC_ALPHA/ONE_MINUS_SRC_ALPHA, LESS_EQUAL).
- Lights: `MAX_LIGHTS = 6` for the whole scene, one shared `u_fs_lighting` block per
  camera, every lit pixel loops over all 6.
- GLSL dialect: `#version 450`, separate `texture2D` + `sampler` (`u_<name>Texture` /
  `u_<name>_smp`), float-or-int-only uniform blocks, VS outputs must match FS inputs after
  dead-code stripping, at most 8 uniform blocks and 16 samplers per stage.

## 3. Concepts and naming (contract)

### 3.1 Shader reference string

Every place that names a shader (material, component, scene default) holds one string:

| Value | Meaning |
|---|---|
| `""` | built-in shader of that type (mesh: the PBR "Lit" shader) |
| `library:<name>` | an engine library shader, `<name>` lowercase snake_case |
| anything else | a project fork, as today (`base` or `a.vert\|b.frag`) |

`library:` is a reserved prefix; project paths may not start with it.

### 3.2 Library files

- Location: `engine/shaderlib/library/`. The existing recursive glob in
  `engine/CMakeLists.txt` embeds them as `library/<file>`.
- A library shader `<name>` is `library/<name>.frag` plus `library/<name>.vert`. If the
  `.vert` does not exist, mesh-type shaders use the shared `library/surface.vert`.
- A depth program (for depth pre-pass and shadow maps) is `library/<name>_depth.vert` +
  `library/<name>_depth.frag` (shared `library/surface_depth.vert` / `.frag` when absent),
  compiled as `ShaderType::DEPTH`. It is used only when the manifest declares `@depth`.
- Shared GLSL lives in `engine/shaderlib/includes/` (section 6).

### 3.3 Variant identity

A compiled library variant is identified by: shader reference + sorted list of enabled
feature defines + the usual property bits. The feature list is folded into the string
interned as the custom id: `library:toon#RAMP,RIM` (features sorted, comma separated,
omitted when empty). `ShaderPool::getShaderStr` must produce a distinct, filesystem-safe
name for each such string so exported `.sdat` files never collide. The compiler parses the
`#...` suffix and adds `#define <FEATURE> 1` for each entry. The same applies to forks that
declare features in their manifest.

## 4. The manifest (contract)

A shader declares its properties, textures, features, inputs and default state in
comment lines at the top of its `.frag` (the `.vert` may also carry lines; both are merged).
Each line is `// @<directive> <arguments>`. Unknown directives are a warning, not an error.
Strings use double quotes. The parser is engine code (`engine/core/shader/ShaderManifest.*`)
because the runtime needs it in exported builds (section 7.6).

```glsl
// @shader mesh
// @name "Toon"
// @category "Stylized"
// @description "Banded diffuse lighting with optional ramp, specular and rim."
// @lighting pixel mixed vertex
// @require normals uv1
// @state cull=back zwrite=on queue=geometry
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property bands range(1, 8) 3 "Bands"
// @property shadowColor color (0.25, 0.25, 0.4, 1) "Shadow colour"
// @property rimPower float 4.0 "Rim power"
// @texture u_baseTexture white "Base map"
// @texture u_rampTexture white "Ramp" feature=RAMP
// @feature RAMP "Ramp texture"
// @feature RIM "Rim light" default=on
// @keywords SPEC_MODE "Specular" NONE HARD SOFT
// @depth
```

Directives:

| Directive | Meaning |
|---|---|
| `@shader <type>` | `mesh`, `ui`, `points`, `lines`, `sky` (phase 4 adds `postprocess`). Required. |
| `@name "<label>"` | display name. Required for library shaders. |
| `@category "<label>"` | grouping in the shader picker. |
| `@description "<text>"` | one-line help shown in the inspector and MCP. |
| `@lighting <modes...>` | supported lighting modes from `none pixel vertex mixed`; the first is the default. Omitted = `none`. |
| `@require <inputs...>` | vertex inputs the shader needs: `normals tangents uv1 uv2 color`. |
| `@state key=value...` | default render state (section 5.2 keys). |
| `@property <member> <kind> <default> ["<label>"] [feature=<F>]` | a member of `u_vs_customParams` and/or `u_fs_customParams`. Kinds: `float`, `range(min, max)`, `int` (uploaded as float), `toggle` (0/1, uploaded as float), `color` (edited as sRGB, stored and uploaded linear), `hdrcolor` (like color, components may exceed 1), `vector2`, `vector3`, `vector4`. Defaults: a number or a parenthesised tuple. `feature=` hides the row unless that feature is on. |
| `@texture <uniform> <default> ["<label>"] [cube] [feature=<F>]` | a texture the shader samples, declared as `uniform texture2D <uniform>; uniform sampler <uniform>_smp;` (or `textureCube`). Defaults: `white`, `black`, `gray`, `normal` (0.5, 0.5, 1). |
| `@feature <DEFINE> ["<label>"] [default=on]` | an optional compile-time feature. |
| `@keywords <GROUP> ["<label>"] <A> <B> ...` | exclusive compile-time choice; defines `<GROUP>_<X>` for the chosen one. First is the default. |
| `@depth` | the shader has its own depth program (section 3.2); required when it moves vertices or discards fragments. |
| `@queue <geometry\|alphatest\|transparent>` | alternative spelling of `@state queue=`. |

Rules:

- Library shaders keep every parameter in float-only blocks (the one-scalar-kind rule);
  `int` and `toggle` are floats in GLSL.
- Unset parameters take the manifest default, not zero (forks without a manifest keep
  today's zero behaviour).
- A manifest error (bad syntax, a `@property` whose member does not exist in the compiled
  blocks, a `@texture` not declared) is reported with file and line by the compile/check
  tooling. A shader with a manifest error still renders if it compiles.
- `time` and `resolution` stay reserved members, written by the engine.

## 5. Material and component data (contract)

### 5.1 Material

`engine/core/texture/Material.h` gains, after the existing members (the first 48 bytes
stay the GPU prefix; nothing is inserted before `name`'s neighbours except as noted):

```cpp
enum class MaterialBlendMode { AUTO, NONE, ALPHA, PREMULTIPLIED, ADDITIVE, MULTIPLY };
enum class MaterialCullMode { AUTO, BACK, FRONT, OFF };
enum class MaterialDepthWrite { AUTO, ENABLED, DISABLED };
enum class MaterialDepthTest { AUTO, LESS, LESS_EQUAL, EQUAL, GREATER_EQUAL, GREATER, NOT_EQUAL, ALWAYS };
enum class MaterialLighting { AUTO, PIXEL, VERTEX, MIXED };

// in Material:
std::string shader;                                  // section 3.1; "" = built-in Lit
ShaderUniformValues shaderParams;                    // by @property member; unset = manifest default
std::vector<std::pair<std::string, Texture>> shaderTextures; // by @texture uniform; unset = manifest default
std::vector<std::string> shaderFeatures;             // enabled @feature defines and chosen @keywords defines
MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
MaterialCullMode cullMode = MaterialCullMode::AUTO;
MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
MaterialDepthTest depthTest = MaterialDepthTest::AUTO;
float depthOffsetFactor = 0.0f;                      // polygon offset (slope scale)
float depthOffsetUnits = 0.0f;                       // polygon offset (constant)
int sortPriority = 0;                                // transparent queue order, lower draws first
MaterialLighting lighting = MaterialLighting::AUTO;  // added by the lighting work, next to alphaMode
```

`MaterialBlendMode::NONE` means opaque (the name `OPAQUE` is a `wingdi.h` macro).
These declarations are already committed on the branch (shared contract commit); do not
rename them. `operator==` covers every new field. An empty `shader` with every new field at its default
must render exactly as today.

### 5.2 Resolving render state

For a mesh submesh the effective state is resolved in this order: explicit material value
-> manifest `@state` -> legacy behaviour.

| State | Keys in `@state` | Legacy behaviour when AUTO everywhere |
|---|---|---|
| blend | `blend=opaque\|alpha\|premultiplied\|additive\|multiply` | `usesAlphaBlend(material)` -> alpha, else opaque |
| cull | `cull=back\|front\|off` | `Submesh::faceCulling` + `MeshComponent::cullingMode` |
| depth write | `zwrite=on\|off` | on when opaque, off when blended |
| depth test | `ztest=less\|lequal\|equal\|gequal\|greater\|notequal\|always` | less-equal (camera `depthTest=false` still forces always) |
| depth offset | `offset=<factor>,<units>` | none |
| queue | `queue=geometry\|alphatest\|transparent` | from blend |

Blend factors (colour; alpha channel keeps the existing offscreen handling):
alpha = SRC_ALPHA, ONE_MINUS_SRC_ALPHA; premultiplied = ONE, ONE_MINUS_SRC_ALPHA;
additive = SRC_ALPHA, ONE; multiply = DST_COLOR, ZERO. Opaque disables blending.

Any non-opaque blend puts the submesh in the transparent queue, sorted by `sortPriority`
ascending, then far-to-near as today. `alphaMode = MASK` keeps its discard behaviour and
stays in the opaque queue.

### 5.3 Components without a material

`UIComponent`, `PointsComponent`, `LineRendererComponent` and `SkyboxComponent` keep
`customShader` + `shaderUniforms` (now accepting `library:` references and manifest
defaults) and gain:

```cpp
std::vector<std::pair<std::string, Texture>> shaderTextures;
std::vector<std::string> shaderFeatures;
MaterialBlendMode blendMode = MaterialBlendMode::AUTO;   // not on Skybox
MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO; // not on Skybox
```

AUTO keeps today's state for these components (alpha blend, depth write on).

### 5.4 Shader precedence on a mesh

`submesh.material.shader` (non-empty) > `MeshComponent::customShader` > scene default mesh
shader > built-in. The component-level fork keeps working for existing projects. The depth
pass uses, in order: the library shader's `@depth` program, `customDepthShader`, built-in.

### 5.5 Lights and scene

```cpp
enum class LightShading { BOTH, PIXEL, VERTEX };
// LightComponent:
LightShading shading = LightShading::BOTH;
// SceneSettings:
int maxLightsPerObject = 8;   // 1..16
```

## 6. Shared GLSL (contract)

### 6.1 `includes/frame.glsl`: engine frame block

Already committed (with `frame_t` in `RenderSystem.h`); the engine side that fills it is
phase 1 work.

Declares one block per stage, `u_vs_frame` (instance `frame`) when `FRAME_VS` is defined
and `u_fs_frame` (instance `frame`) when `FRAME_FS` is defined, with this exact layout
(engine-internal members may be appended after `ambient`, never inserted):

```glsl
mat4 viewMatrix;
mat4 projectionMatrix;
mat4 viewProjectionMatrix;
vec4 cameraPosition;     // xyz world position; w = 1 for orthographic, 0 for perspective
vec4 cameraDirection;    // xyz world forward (the direction the camera looks)
vec4 time;               // x seconds since startup, y sin(x), z cos(x), w frame delta
vec4 resolution;         // xy render target size in pixels, zw 1/size
vec4 mainLightDirection; // xyz direction towards the brightest directional light, w intensity (0 = none)
vec4 mainLightColor;     // rgb linear colour
vec4 ambient;            // rgb scene ambient (global illumination colour * intensity)
```

The engine fills it by name for every draw whose shader declares it (mesh, depth, UI,
points, lines, sky), once per camera pass, and uploads it per draw like other blocks.
A shader includes it only in the stages that use it (an unused declared block is stripped
by GL drivers and breaks slot binding; the include guards prevent that).

### 6.2 `includes/output.glsl`: final colour

`vec4 finalizeColor(vec4 linearColor)` applies scene fog (when `HAS_FOG`) and the linear ->
display encoding used by the engine today. Every library shader writes its output through
it. Phase 4 extends this function (tonemap, grade, vignette, dither) without touching the
shaders.

### 6.3 `includes/surface_vs.glsl` and `library/surface.vert`

The shared vertex path of library mesh shaders. It supports every mesh property bit that
applies to a plain mesh: skinning (both GL texture and storage-buffer paths), morph targets,
instancing (matrix, colour, texture rect, fade), sprite texture rect, UV sets 1-2, vertex
colour (vec3/vec4), normals, tangents, fog, shadows varyings for pixel-lit shaders, and the
Vulkan depth fix-up and `invariant gl_Position` that must match the depth pass.

It exposes a hook so a shader's own `.vert` can move vertices before projection:

```glsl
#define SURFACE_DISPLACE                 // optional, before including
vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv);
```

Outputs (the names library fragment shaders rely on): `v_position` (world), `v_normal`
(world, normalised in VS), `v_tangent` + `v_bitangent` (with tangents), `v_uv1`, `v_uv2`,
`v_color` (vec4, white when the mesh has none), plus the lighting varyings of 6.4. When a
required attribute is missing from the mesh, the output gets a fallback (normal from the
model's +Y, uv = 0, colour = white) so every shader compiles and renders.

`library/surface_depth.vert` / `.frag` are the matching depth programs, with the same hook
and an optional `surfaceDepthDiscard(...)` hook for alpha clip.

### 6.4 `includes/lights.glsl`: unified lighting API

One data layout for both stages. Blocks `u_vs_objectLights` (when `LIGHTS_VS`) and
`u_fs_objectLights` (when `LIGHTS_FS`), instance `objectLights`:

```glsl
// LIGHT_CAPACITY is 4, 8 or 16 (compile-time tier, section 8.3)
vec4 counts;                           // x = pixel lights, y = vertex lights (as floats)
vec4 shR; vec4 shG; vec4 shB;          // L1 SH ambient: rgb_c(n) = dot(sh_c.xyz, n) + sh_c.w
vec4 position_type[LIGHT_CAPACITY];    // xyz world position, w type (0 directional, 1 point, 2 spot)
vec4 direction_range[LIGHT_CAPACITY];  // xyz world direction the light points, w range (<= 0 unlimited)
vec4 color_intensity[LIGHT_CAPACITY];  // rgb linear colour, w intensity
vec4 cone_shadow[LIGHT_CAPACITY];      // x inner cone cos, y outer cone cos, z shadow map index (-1 none), w cascades
vec4 spot_mask[LIGHT_CAPACITY];        // xyz spot projection up, w mask aspect (0 = no mask; mask tile in engine-internal data)
```

Pixel lights occupy indices `[0, counts.x)`, vertex lights `[counts.x, counts.x + counts.y)`.
The SH terms carry the scene ambient plus every light that did not fit the cap.

Functions (stable API for library shaders):

```glsl
struct LightSample { vec3 direction; vec3 radiance; float attenuation; int index; };
int   pixelLightCount();
int   vertexLightCount();
LightSample getLight(int index, vec3 worldPosition);      // direction towards the light, radiance = colour*intensity*attenuation
vec3  ambientLight(vec3 worldNormal);                     // SH ambient, linear, never negative
vec3  vertexDiffuse(vec3 worldPosition, vec3 worldNormal); // sum of Lambert diffuse of the vertex lights
float lightShadow(int index, vec3 worldPosition, vec3 worldNormal); // 1 = lit; pixel stage only, needs USE_SHADOWS
```

Attenuation and spot falloff are the existing KHR_lights_punctual functions in
`punctual.glsl` for both stages. Vertex-evaluated lights contribute diffuse only (no
specular, no shadows, no spot mask texture; the cone falloff still applies). Library
shaders pass `vertexDiffuse` from VS to FS in `v_vertexLight` (provided by `surface_vs.glsl`
when `USE_VERTEX_LIGHTS` is defined).

## 7. Engine requirements

### 7.1 Library resolution and compile

- `ShaderVariantCompiler::resolveCustomShaderSources` resolves `library:` references from
  the embedded library (`getLibrarySources()`), including the `surface.vert` fallback,
  `@depth` programs and the `#FEATURES` suffix.
- Library shaders compile, cache, precompile on export and compile at runtime exactly like
  forks. No editor-only path.
- Editor checks that assume a fork exists on disk (`AssetTools.cpp` `check_shader`,
  `ProjectChecks.cpp`, the inspector shader row, `Catalog::isCustomShaderBuildFailed`)
  accept library references.

### 7.2 Manifest at runtime

- The engine parses manifests (once per source, cached) for library shaders from the
  embedded sources and for forks from their source files.
- Exported builds must have every needed manifest: the exporter ships fork sources that
  carry a manifest even when runtime shader compilation is off. If a manifest cannot be
  found at runtime the object renders with zero-initialised parameters and an error is
  logged once.

### 7.3 Required inputs and variants

- `@require` forces the matching attributes and defines (`HAS_NORMALS`, `HAS_TANGENTS`,
  `HAS_UV_SET1`, `HAS_UV_SET2`, vertex colour) when the mesh has the attribute, independent
  of scene lights and of whether PBR textures are assigned.
- The lighting mode adds the lighting defines (section 8.3); `none` keeps `MATERIAL_UNLIT`.
- `getSubmeshShaderVariant` (`RenderSystem.cpp`) and the editor's `ShaderVariantCollector`
  must compute identical keys; the exporter must precompile every variant a scene uses,
  including feature combinations and depth programs.

### 7.4 Custom textures

- Bound by uniform name, resolved to slots once at load (no per-draw string lookups).
- Unset textures bind the manifest default (engine 1x1 white, black, gray or flat normal).
- Work on mesh (material), UI, points, lines and sky (component).
- Textures referenced by materials/components are collected by the exporter like other
  texture assets.

### 7.5 Render state

- `ObjectRender::endLoad` / `SokolObject::makePipelineDesc` take a render-state struct
  (blend, cull including none, depth write, depth compare, depth bias) for every pipeline
  variant they build (default, no-depth, RTT, RTT-invert), preserving today's per-pipeline
  differences (colour mask, offscreen alpha handling, z-prepass).
- The editor's "disable face culling" debug variant keeps working.
- `MeshComponent::cullingMode` is exposed in the Catalog and inspector (it is Lua-only today).

### 7.6 Frame block

Filled per camera pass for every draw whose shader declares it (section 6.1),
including mesh, depth, UI, points, lines and sky.

### 7.7 Performance rules

- No per-frame heap allocations in the new paths (light selection, state resolution,
  texture binding, frame block).
- No per-draw string comparisons or map lookups; resolve names to slots at load.
- Manifests are parsed once per source and cached.
- Default-state objects (empty `shader`, AUTO everywhere) take exactly today's code path
  and upload, per light slot of their capacity tier, no more data per draw than the
  pre-library layout (0c59f0a0) did for the same number of slots: 64 B per light slot
  (was 80), plus a 64 B lights header (counts, SH), a 32-96 B Lit camera block (2-6
  vectors by fog, shadows, SSAO, IBL) and no separate fog block. Measured (lights, camera
  and fog blocks; GL bytes, 256-byte aligned on Metal/WebGPU in brackets):

  | Draw | 0c59f0a0 | before fix (a1107ddb) | now |
  |---|---|---|---|
  | Lit pixel, tier 8 | 560 (6 slots) [768] | 1088 [1280] | 608 [1024] |
  | Lit pixel, tier 8, fog | 592 [1024] | 1088 | 624 [1024] |
  | Lit pixel, tier 8, shadows + fog | 592 | 1088 | 640 [1024] |
  | Lit pixel, tier 4 | - | 704 | 352 [768] |
  | Lit mixed, tier 8 | - | 1792 (704 VS + 704 FS + 384) | 1008 (400 VS + 576 FS + 32) |
  | Lit vertex, tier 8 | - | 1088 (704 VS + 384) | 480 (448 VS + 32) |
  | fogged unlit sprite / tilemap (MATERIAL_UNLIT) | 32 | 384 | 32 |
  | library pixel shader reading the frame block (toon), tier 8 | - | 1088 | 928 (576 + 352) |
  | library unlit shader with fog, no frame block (unlit) | - | 384 | 32 |

  The 6-slot baseline would be 512 B (+32 fog) in the new layout; tier 8's two extra
  slots are the difference to 0c59f0a0. The frame block (352 B) is the library contract
  and is uploaded only by shaders that declare it.

## 8. Unified lighting

### 8.1 Behaviour

- Scenes may contain any number of lights (no `MAX_LIGHTS` clamp on scene lights). Arrays
  sized by `MAX_LIGHTS` that track scene lights become dynamic or are indexed by the
  per-object list; shadow-casting lights stay limited by shadow-atlas slots, spot masks by
  mask-atlas tiles (lights that do not get a slot/tile render without it, as today).
- Each frame, for each mesh (an instanced batch counts as one object, using its batch
  bounds), the engine selects the lights that reach it: directional lights always; point
  and spot lights whose range sphere intersects the object's world AABB; respecting
  `cullingMask` and disabled lights. They are ranked by estimated contribution at the
  AABB centre (directional lights first, then intensity * luminance * attenuation).
- The top `maxLightsPerObject` (clamped to the tier capacity) go into the object's list;
  every other reaching light is folded into the object's SH ambient, evaluated at the
  AABB centre, so lights never pop.
- The list is split by stage using the material's lighting mode:
  - `PIXEL`: every listed light is evaluated per pixel.
  - `VERTEX`: every listed light is evaluated per vertex.
  - `MIXED`: each light follows its `shading`; `BOTH` goes per pixel.
  - `AUTO`: the shader's default mode (built-in Lit: `PIXEL`).
- Selection is recomputed only when the object or a reaching light changed (transform,
  enabled state, colour/intensity/range/cone), not every frame unconditionally, and never
  allocates.

### 8.2 Built-in Lit

- `mesh.vert` / `mesh.frag` / `punctual.glsl` / `shadows.glsl` move from the shared
  `u_fs_lighting` to `includes/lights.glsl` + `includes/frame.glsl`. Lit supports PIXEL,
  VERTEX and MIXED: vertex lights add diffuse (`vertexDiffuse` * base colour * (1 -
  metallic)), pixel lights keep full PBR with shadows and spot masks, SH ambient replaces
  the flat global illumination term (with no overflow lights it equals today's term).
- Per-camera data that lived in `u_fs_lighting` (eye position, camera direction, PCF
  radius, environment colour/rotation, viewport info) moves to the frame block's
  engine-internal members. The worst-case Lit fragment variant must stay within 8 uniform
  blocks and 16 samplers per stage on every backend (fold fog into the frame block if
  needed).
- Existing forks that declare the old `u_fs_lighting` block keep working: when a shader
  declares it, the engine fills the legacy layout from the object's pixel lights (first 6).
- With 8 or fewer lights reaching an object and PIXEL mode, Lit output is visually
  identical to today.

### 8.3 Variants

- New MESH property bits: `USE_VERTEX_LIGHTS` (vertex stage evaluates lights) and a 2-bit
  light-capacity tier (4, 8, 16) from `maxLightsPerObject` (default 8 -> tier 8). Existing
  `USE_PUNCTUAL` means pixel lights. Use free bits (28-31); never renumber existing bits.
- Light counts are uniforms: lights moving in and out of range never change the variant.
- `ShaderPool` property names/short names, `getMeshProperties`, the compiler's define
  table and `getSubmeshShaderVariant` are updated together (the four places listed in
  `engine-capabilities.md` section 12).

### 8.4 Editor and scripting

- Light inspector: "Shading" combo (Both, Per pixel, Per vertex) with help text.
- Scene settings: "Max lights per object" (1-16) with help text explaining the SH fallback.
- Material inspector: "Lighting" combo listing only the modes the shader supports.
- Lua: `light.shading`, `scene.maxLightsPerObject`, `material.lighting`.

## 9. Library shaders (phase 1)

All are `@shader mesh` unless stated. Every 3D shader supports fog, instancing, skinning,
morph targets and vertex colour through `surface_vs.glsl`. "Lighting" lists the supported
modes, default first. Technique sources are in the research inventories.

| Name | Lighting | Technique and parameters |
|---|---|---|
| `unlit` | none | Base colour x base texture x optional vertex colour (`VERTEX_COLOR`). Alpha clip feature. Covers Unity Unlit/*. |
| `simple_lit` | pixel, vertex, mixed | Blinn-Phong: diffuse + specular colour/gloss (pixel lights only), optional normal map (`NORMAL_MAP`), emission (`EMISSION`), receives shadows from pixel lights. Mobile tier of Lit. |
| `vertex_lit` | vertex | Base colour x texture x (SH ambient + `vertexDiffuse`). The owner's Jasper Arcade shader, now on engine lights. Optional emission. |
| `directional_tint` | none | Kenney's shader: `w = n*n` from the world normal; `c = mix(mix(mix(A, B, w.x), A, w.y), C, w.z)` with A = colour * colorA (Y faces), B = colour * colorB (X faces), C = colour * colorC (Z faces); output texture (tiling/offset) * c. Must match the gist exactly. |
| `fake_lit` | none, vertex | The owner's `General` shader: albedo * tint; shadow colour lerp by `saturate(pow(1 - n.y, shadowPower) * shadowStrength)`; fresnel darkening `saturate(rimScale * pow(1 - n.v, rimPower))` lerped to shadow colour; features `MATCAP` (additive `pow(matcap * gain, power)`), `SHADOW_TEXTURE` (multiply on uv2 or uv1), `VERTEX_AO` (vertex colour r), `EMISSION`, `AXIS_FOG` (camera-relative fog along a chosen world axis, start/length/colour). Vertex mode adds `vertexDiffuse * albedo`. |
| `matcap` | none | View-space normal `n_v.xy * 0.5 + 0.5` into a matcap texture. `@keywords MATCAP_MODE REPLACE MULTIPLY ADD`; base texture/tint; `ADD` uses `pow(m * gain, power)`; optional rim. |
| `toon` | pixel, mixed, vertex | Banded N.L: `bands` steps or `RAMP` texture; shadow colour; `SPECULAR` hard highlight (size, smoothness); `RIM` light (colour, power, only on lit side option); receives shadows. Vertex lights add smooth diffuse. |
| `rim` | none | Force field / fresnel glow: `pow(1 - abs(n.v), power) * intensity * rimColor` plus optional scrolling texture (`TEXTURE`); default state additive, zwrite off, cull off option via state. |
| `triplanar` | none, pixel, vertex | Texture projected on three axes blended by `pow(abs(n), sharpness)`; `@keywords SPACE WORLD OBJECT` (object space multiplies by object scale); `TOP_BOTTOM` feature uses separate top and bottom textures (floor/wall/ceiling by normal sign); tiling; `NOISE_MASK` dirt overlay. Lambert lighting in lit modes. |
| `vertex_color` | none, vertex, pixel | Vertex colour * tint (* texture with `TEXTURE`). Lambert in lit modes. |
| `dissolve` | none, pixel | Noise texture threshold `cutoff`; glowing edge of `edgeWidth` in HDR `edgeColor`; `WORLD_NOISE` samples noise triplanar in world space. Discards, so it has `@depth`. |
| `hologram` | none | Scrolling scanlines (procedural or `SCANLINE_TEXTURE`) along world Y, fresnel rim, flicker from time noise, tint; default blend additive or alpha by keyword. |
| `panner` | none | One or two textures scrolling by speed vectors (`SECOND_LAYER` with `@keywords COMBINE MULTIPLY ADD`), `DISTORTION` noise offset; for lasers, waterfalls, conveyor belts, clouds. |
| `flipbook` | none | Sprite-sheet animation on a mesh: columns, rows, fps, start frame, loop; `FRAME_BLEND` crossfades frames. Frame UVs computed in VS. |
| `water` | none, pixel | Stylised water without scene depth: shallow/deep colour by fresnel, two scrolling normal/noise layers with world-anchored UVs, foam from noise threshold, optional sine `WAVES` in VS (`@depth`), specular from the main light in pixel mode. Default blend alpha. Phase 3 adds depth foam. |
| `foliage` | pixel, vertex, none | Wind sway in VS: phase from world position, amplitude masked by vertex colour or height, gust frequency; alpha cutout; default cull off; translucency term on back-lit faces. `@depth`. |
| `spin_bob` | none, vertex, pixel | Pickup animation in VS: rotate about the object Y axis by `time * spinSpeed` (positions and normals), vertical sine bob; Lambert in lit modes. `@depth`. |
| `decal` | none | Mesh decal: texture * tint, alpha blend, zwrite off, default depth offset (-1, -1) so it sits on the surface. Projected decals are phase 3. |
| `sprite` | none (2D lights when present) | For Sprite/Tilemap/MeshPolygon: respects the sprite texture rect and premultiplied/alpha sprites. Features: `OUTLINE` (8-tap alpha dilation inside the frame rect, width in texels via `textureSize`, colour), `HUE_SHIFT` (rotation about the grey axis, optional speed for cycling), `FLASH` (hit flash colour/amount), `GRAYSCALE` (amount), `COLOR_REPLACE` (RGB-mask texture with three replacement colours), `OVERLAY` (hard-light texture/colour, the "gold skin" effect), `GLOW` (alpha-as-light: `pow(alpha * k, p) * hdrColor`). Supports `USE_LIGHT2D`. |
| `ui_fx` | none, `@shader ui` | UI Image/Text effects: `GRAYSCALE`, `HUE_SHIFT`, `TINT_OVERLAY`, `SHINE` (moving diagonal highlight band, angle/width/speed). Respects the font-atlas variant. |
| `particle` | none, `@shader points` | Point-sprite particles: texture * vertex colour; `GLOW` (alpha-as-light); `EROSION` (noise threshold driven by vertex alpha). Blend comes from the component (alpha, additive, premultiplied, multiply). |
| `gradient_sky` | none, `@shader sky` | Top/horizon/bottom colours with exponents on the view direction's y; horizon offset. |
| `procedural_sky` | none, `@shader sky` | Sky/horizon/ground colours, sun disc and halo from `frame.mainLightDirection`/`mainLightColor`, `CLOUDS` two scrolling noise layers. |
| `panoramic_sky` | none, `@shader sky` | Equirectangular 2D texture, rotation, exposure, tint. |

Each shader:

- has a complete manifest (`@name`, `@category`, `@description`, labels on every property);
- uses only `surface_vs.glsl`, `lights.glsl`, `frame.glsl`, `output.glsl` and the existing
  includes, never re-declaring engine blocks by hand;
- compiles on all six backends for its variant matrix (section 11.1);
- renders correctly in the fixture project (section 11.2).

## 10. Editor, serialization, export, Lua, MCP

- Serialization: scene YAML (submesh materials and the components of 5.3), `.material`
  files (`Stream::encodeMaterial` / `decodeMaterial`), prefab overrides. Default values are
  omitted so old files and new files without the feature are byte-identical.
- Model reloads keep the new material fields (extend `SubmeshOverrideFlags`).
- Export: `Factory.cpp` generated C++ sets every new field; `ShaderVariantCollector` and
  `Exporter` collect and precompile library variants, depth programs and feature
  combinations; textures are shipped.
- Inspector (material section of each submesh, and the 5.3 components):
  - "Shader" picker: "Lit (built-in)", library shaders grouped by `@category` with
    `@description` tooltips, "Custom file...".
  - Parameter rows from the manifest: colour pickers (sRGB editing, linear storage),
    sliders for `range`, checkboxes for `toggle`, drag fields for the rest, texture slots,
    feature checkboxes, keyword combos; rows with `feature=` hide when the feature is off;
    each row resets to the manifest default.
  - "Surface" rows: blend mode, cull, depth write, depth test, depth offset, sort priority;
    AUTO shows the resolved value.
  - Every edit is an undoable command and marks the scene dirty; feature changes trigger a
    variant build without blocking the UI.
- Catalog property paths for every new field (used by MCP `set_property`/`get_property`),
  including `submeshes[N].material.shaderParams.<member>`,
  `submeshes[N].material.shaderTextures.<uniform>` and `submeshes[N].material.shaderFeatures`.
- Lua: `Material.shader`, the render-state fields, `lighting`, `setShaderParam(name, value)`,
  `getShaderParam(name)`, `setShaderTexture(name, path)`, `setShaderFeature(define, on)`;
  the same parameter/texture/feature methods on UI/Points/LineRenderer/Skybox objects;
  `light.shading`; scene `maxLightsPerObject`. Lua stubs (`generate_lua_stubs.py`) and the
  LuaLS plugin types updated.
- MCP: a `list_shaders` tool (library manifests: name, type, description, lighting modes,
  properties, textures, features); `create_material` / `update_material` / `link_material`
  accept the new fields; `get_shader_uniforms` / `set_shader_uniform` / `check_shader` work
  for material shaders and library references. `docs/mcp/guide/shaders.md`,
  `docs/mcp/TOOLS.md` and `editor/mcp/McpInstructions.cpp` describe the library first and
  forking second (the guide must stay within its size budget and
  `test_instructions_name_real_tools` must pass).

## 11. Verification

### 11.1 Compile matrix

`jasperengine-editor shaders --library <name|all> --backend all` compiles, for each
library shader, a matrix that covers: base variant; each feature alone; all features on;
each keyword value; each supported lighting mode; and, for mesh shaders, skinning,
instancing, vertex colour + UV2, texture rect, fog, shadows; plus each `@depth` program.
Non-zero exit and compiler-style output on any failure. It also validates every manifest.
`jasperengine-editor check --project <dir> --shaders` checks library references used by a
project.

### 11.2 Fixture project

`tools/fixtures/shader-library/` builds its scene from Lua at runtime (one file per shader
group, `scripts/<group>.lua`, so groups do not collide): one object per library shader and
lighting mode, with moving lights (more lights than the cap, mixed per-light shading),
sprites, UI, particles and a sky. `tools/smoke-test.sh tools/fixtures/shader-library`
exports, builds, runs and screenshots it without errors or missing-shader log lines.

### 11.3 Regression

- The editor and engine build with `tools/build.sh linux`.
- `jasperengine-editor shaders --backend all` for the built-in variants still compiles.
- `tools/smoke-test.sh tools/fixtures/runtime-shaders` still passes.
- The MCP test suite (`tools/mcp/run-tests.sh`) passes, with new tests for `list_shaders`,
  material shader fields and library compile checks.
- An existing scene with no library shaders renders identically (compare screenshots before
  and after on the fixture projects).

## 12. Definition of done

Every requirement above is implemented, every check in section 11 passes, the work is
merged into `shader-library`, and a review pass found no open correctness bugs.

## 13. Implementation notes

Decisions taken while implementing phase 1, and deviations from the text above. They
override the sections they refer to; later agents follow them.

### 13.1 Shader system (sl/p1-core)

- `@require position` exists (`SURFACE_POSITION`, `v_position`), beside the inputs of
  section 4. `surface_vs.glsl` writes a varying only when a `SURFACE_*` define asks for it
  (the compiler adds them from `@require`, in both stages), because an output the
  fragment stage does not read fails the cross-compile.
- `shaderFeatures` entries may be `!<DEFINE>`: they turn a `default=on` feature off.
- Colour defaults in manifests (`color`, `hdrcolor`) are written sRGB and stored linear,
  like edited values.
- Render state: AUTO everywhere keeps the legacy alpha-blend pipeline even for opaque
  objects (identical to the old path); only an explicit `NONE` (`blend=opaque`) disables
  blending. Library shaders that are opaque say `@state blend=opaque`.

### 13.2 Unified lighting (sl/p1-lighting)

- Variant bits: `Vtl` = 28 (`USE_VERTEX_LIGHTS`), `Lc4` = 29 and `L16` = 30 (capacity
  tier; neither = 8 lights, so default keys did not change).
- The spot mask atlas tile of each pixel light is packed into `counts.zw` of the lights
  block (3 bits per light, 7 = none, lights 0-7 in z, 8-15 in w).
- Fog lives in the frame block (`fogColor_type`, `fogDensity_start_end`): `fog.glsl`
  declares `u_fs_frame` itself, `getFogColor(vec3)` is unchanged. The engine-internal
  frame members appended after `ambient` are `lightingInfo`, `environment`,
  `viewportInfo`, `fogColor_type`, `fogDensity_start_end` (frame.glsl and `frame_t`).

### 13.3 Integration

- One frame block path: `RenderSystem::updatePassFrame(camera, transform)` fills
  `passFrame` once per camera pass, contract members (`fillFrameBlock`) then the
  engine-internal ones (`fillFrameLighting`), before that camera's shadow passes and draws:
  the scene camera (before the frame's shadow maps and again for its colour pass),
  render-to-texture cameras, mirror reflections, reflection probe faces and editor
  views (including their own shadow passes). Every draw resolves its frame slots once at
  load (`ShaderLibraryBindings`, also for the built-in shaders) and uploads the block once
  (`applyFrameBlock`). The editor's "no fog" view setting is applied to the block.
  `frame.resolution` is the pass's target size (camera texture, fixed resolution, or view).
- Lighting modes. `MaterialLighting` gained `NONE` (appended, value 4) so a shader listing
  `none` with another default can be drawn unlit. The effective mode
  (`RenderSystem::resolveLighting`) is `material.lighting` when the shader lists it, else
  the shader's default (first `@lighting` entry, `none` without the directive; built-in
  Lit and forks without a manifest: `PIXEL`, listing pixel, vertex, mixed). An unlisted
  request is logged once per material/shader pair. The editor's lighting combo should list
  `RenderSystem::supportsLighting` modes. A manifest shader in a lit mode gets
  `USE_PUNCTUAL` (pixel, mixed), `USE_VERTEX_LIGHTS` (vertex, mixed), shadows for pixel
  lights, normals (`HAS_NORMALS` when the mesh has them), and never Lit's IBL, SSAO or
  material normal map. Without scene lights (or `receiveLights` off) it compiles
  `MATERIAL_UNLIT`, as Lit does: lit library shaders draw their unlit colour then.
  `ShaderVariantCollector` uses the same functions, so its keys are identical.
- A pixel-lit (`USE_PUNCTUAL`) variant of a manifest shader is compiled with
  `SURFACE_POSITION` and `SURFACE_NORMALS` whatever `@require` says: its fragment stage
  must read `v_position` and `v_normal` under `USE_PUNCTUAL`.
- Light units (`lights.glsl`): every helper returns light to multiply directly by the
  surface colour, with Lambert's 1/PI inside, as Lit's `BRDF_lambertian`: `ambientLight`
  (scene ambient plus the lights over the cap, which the engine folds with 1/PI),
  `vertexDiffuse`, `lambert(LightSample, n)` and `blinnPhong(LightSample, n, v,
  shininess)` (normalised, times the specular colour). Only `LightSample.radiance` is raw.
  `ambientLightScaled` was removed. Lit was adjusted to the same units (renders the same).
  Lit's dielectric diffuse stays about 4% below a library shader's for the same albedo:
  its `albedoColor` is `base * (1 - f0)` (the energy its specular takes), not a unit.
- `v_vertexLight` (surface_vs.glsl and mesh.vert) is `vertexDiffuse`, plus `ambientLight`
  in vertex mode (no `USE_PUNCTUAL`: the fragment stage has no lights block). A library
  fragment shader adds it as is. With `USE_SHADOWS` it must call `lightShadow` for its
  pixel lights (the `v_lightProjPos[]` varyings must be read). Loop pixel lights as
  `for (i < LIGHT_CAPACITY) { if (i >= pixelLightCount()) break; ... }`.
- Varyings link by name: the shader compiler gives each fragment input the location of
  the vertex output of the same name before cross-compiling (glslang numbers each stage
  in declaration order, which broke GL/HLSL/MSL/Vulkan linking when a library `.frag`
  declared its inputs in another order than `surface_vs.glsl`). A fragment shader may
  declare its inputs in any order; every one must still be read (and every output used).
- `simple_lit` builds its normal-map tangent frame from screen-space derivatives (no
  `@require tangents`: `@require` cannot depend on a feature, and declaring tangents for
  every variant would leave unread varyings without `NORMAL_MAP`). The normal map applies
  to pixel lights only.
- Compile matrix: each lit mode at each capacity tier (8, 4, 16), with and without
  shadows, plus the no-scene-lights (unlit) fallback; the first mode at tier 8 with every
  feature set; the mesh cases per mode.
- Export: lit variants depend on the capacity tier, which only a script can change at
  runtime (`scene.maxLightsPerObject`, `Scene::setMaxLightsPerObject`). When any project
  script mentions `maxLightsPerObject`/`MaxLightsPerObject`, the collector adds every tier
  (4, 8, 16) to each lit environment; otherwise only the scene's own tier ships (a tier
  switch then compiles at runtime, or misses its variants without the runtime compiler).
- `check_shader` / `check --shaders` compile a mesh shader that has a manifest in the
  variant of its default lighting mode (UVs, normals, vertex colour, scene lights for a lit
  default) instead of properties 0, so the lit path and the textures it samples are
  checked; the full matrix stays `shaders --library`.
- Library shader signatures include the embedded library hash, so the editor's cached
  builds of library shaders are rebuilt when an include changes; the editor shader cache
  moved to `v31`.
- Lua cannot write `Material.baseColorFactor`, `metallicFactor` or `roughnessFactor` (read
  only on the copy `getMaterial` returns); fixtures use glTF materials or `setColor`.

### 13.4 Fix pass: editor, MCP and Lua (sl/p1-fix-editor)

- Shader-library values kept from a previous shader stay stored (switching back finds them; a
  shader ignores members it does not declare). An edit checks only the entries it names
  against the manifest (`set_properties` `shaderParams.<m>` / `shaderTextures.<u>` /
  `shaderUniforms.<m>`, `update_material` merges); replacing a whole collection checks all of
  it. A `library:` reference the engine does not have refuses value edits with the list of
  library shaders (MCP); the inspector notes it and draws no rows.
- The shader the values of a `set_properties` call follow is the one the call sets: a submesh
  material's own `shader`, else, for a material without one, the mesh's `customShader` (else
  the scene default), as `RenderSystem::getSubmeshShader`; a mesh's `shaderUniforms` follow
  its `customShader`.
- Changing a material's shader (inspector, `set_properties`, `update_material`) puts a
  `lighting` mode the shader it then draws with does not list back to `auto`, in the same
  undo step.
- Shader textures are images: a camera render texture (or a cube map in a 2D slot) is refused
  by MCP rather than linked, remapped and exported for these slots; a render texture goes in a
  built-in material texture or a UI image.
- Lua: `setShaderParam(name, Vector3)` stores alpha 1 for `color`/`hdrcolor` members (0 for the
  others), on materials and components. `Mesh:getMaterial` / `Mesh.material` give a copy that
  remembers (weak-keyed registry table beside the userdata) the shader its submesh draws with
  when the material names none, so `getShaderParam`, `setShaderFeature` and
  `isShaderFeatureEnabled` resolve defaults, default-on features and keyword groups as the
  renderer does. Components gained `removeShaderParam`.
- Inspector: the UI/Points/Lines "Auto" blend and depth-write entries name what
  `RenderSystem::resolveComponentRenderState` resolves (a particle shader's `zwrite=off` shows
  "Auto (Off)"); "Custom file..." opens its popup after the combo closes. Material previews
  (inspector and Project window thumbnails) draw with the material's shader, or the mesh's /
  scene's when it names none, and redraw until the variant and textures are ready.
- `list_shaders type=` takes `mesh`, `ui`, `points`, `lines`, `sky` and refuses anything else
  with that list. The agent guide (`McpInstructions.cpp`) is at most 16 KB (16,384 bytes,
  checked by `test_instructions_name_real_tools`); shader detail lives in
  `docs/mcp/guide/shaders.md`.

### 13.5 Review fixes: engine (sl/p1-fix-engine)

- Lights block (`lights.glsl`): the layout is engine-internal (section 6.4's arrays are
  replaced), the functions are the contract and unchanged; library shaders never read
  `objectLights.*`. Four vectors per light: `position_type` (xyz position; w 0 for a
  directional light, else 1 + range, range 0 unlimited), `direction_inner` (xyz direction,
  w spot inner cos, -2 for a point light), `color_outer` (rgb colour * intensity, w spot
  outer cos, -2 for a point light), `spot_shadow` (xy octahedral spot up, z mask aspect,
  w = (shadow index + 1) | cascades << 6 | mask tile << 9, exact in a float). `Light.color`
  is premultiplied and `Light.intensity` 1. Order in the block: `counts`, the three
  vertex arrays, the SH, `spot_shadow`: the vertex stage declares a prefix (no SH in mixed
  mode, where `ambientLight` is fragment-only) and the engine uploads the reflected size
  of each stage's block.
- The built-in Lit reads `u_fs_litFrame` (`includes/lit_frame.glsl`, `lit_frame_t`), a
  2-6 vector prefix of the camera data it needs, not the frame block. The frame block's
  engine-internal members are now `lightingInfo`, `fogColor_type`, `fogDensity_start_end`
  (`environment` and `viewportInfo` were Lit's only). `fog.glsl` reads Lit's block, else
  the frame block when the fragment stage declared it first, else its own 32-byte
  `u_fs_fog` (MATERIAL_UNLIT variants, library shaders that do not read the frame block).
  Render identity with 0c59f0a0 for a 6-light scene (directional with shadows, 3 point,
  2 spot) holds pixel for pixel.
- Blend define: every colour variant of a shader with a manifest compiles with one of
  `BLEND_NONE`, `BLEND_ALPHA`, `BLEND_PREMULTIPLIED`, `BLEND_ADDITIVE`, `BLEND_MULTIPLY`,
  the blend its pipeline resolves to (AUTO: legacy alpha), folded into the variant
  reference (`ShaderReference::getColorVariantReference`, `library:x#BLEND_ALPHA,F`);
  depth programs do not get it. `finalizeColor` fogs by it (additive fades to black,
  multiply to white, premultiplied to fog * alpha). `BLEND_*` feature/keyword names are a
  manifest error. `RenderSystem::getComponentShaderReference` takes the component's blend.
- Manifest syntax: `@state[<DEFINE>] key=value...` / `@state[!<DEFINE>]` (rules over the
  base `@state`, in order, while a feature or keyword define is on/off; resolution:
  material, rules, `@state`, legacy; resolved once at load into `Submesh::manifestState`,
  so the per-frame queue check does not allocate); `@state shadows=on|off` (`off` keeps
  the submesh out of shadow maps: `SubmeshShaderInputs::castShadows` for its variant, and
  `drawMeshDepth` skips it in shadow passes; the mesh's
  `castShadows` still applies, there is no material override); `@require <inputs>
  feature=<DEFINE>` (or `feature=!<DEFINE>`), resolved per variant from its features in
  the variant, the compiler's `SURFACE_*` defines and the depth program
  (`SubmeshShaderInputs::features`); `@require rawcolor` (`SURFACE_RAW_COLOR`,
  `v_rawColor`: the vertex colour without the instance colour). The displacement hooks
  get the raw vertex colour in both stages.
- Library shaders on a Terrain: `surface_vs.glsl` / `surface_depth_vs.glsl` include
  `terrain_vs.glsl` (with `TERRAIN_NO_SURFACE_VARYINGS`), uv1 is the terrain's tiled UV
  (`getTerrainUV`); painted layers (`HAS_TERRAIN_PBR`) stay Lit's.
- Depth programs: depth bit 13 `Txr` (`HAS_TEXTURERECT`) applies the sprite texture
  rect, and the instance rect in the built-in one, to the alpha-clip UV. A shader with a
  manifest never gets the built-in depth alpha test from its material's alpha mode.
- A colour pass whose depth test is less, greater or not-equal leaves the camera depth
  pre-pass. Offscreen opaque pipelines composite alpha (ONE, ONE_MINUS_SRC_ALPHA). The
  reflection probe face refills the frame block after `capturingReflectionProbe` is set.
- Light picking: a spot whose outer cone misses the bounds sphere is left out; a picked
  directional light changing only colour/intensity does not repick (unless a directional
  light was folded into the SH).
- A Sprite or MeshPolygon made in Lua has one submesh from its construction.
- `JASPERENGINE_READY_SIGNAL=1` logs "Engine ready: ..." once every scene has been loaded
  with no shader build pending for 10 frames; `tools/smoke-test.sh` waits for it by
  default (`SMOKE_GAME_SECONDS=ready`, `SMOKE_READY_TIMEOUT`, `SMOKE_READY_SETTLE`).
- `tools/fixtures/shader-regressions` shows the fixed bugs in one scene
  (`SR_FOCUS=probe|prepass|rtt|terrain|front` for a close-up; the front row shows
  `shadows=off`, `@state[...]` and a Sprite's shader set right after creation).
- Editor shader cache `v32`.

### 13.6 Final integration (sl/p1-final)

- Built together, the three fix branches needed one change: the inspector's UI/Points/Lines
  "Auto" labels call `RenderSystem::resolveComponentRenderState` with the component's
  `shaderFeatures`, so `@state[...]` rules show (the material rows already resolved them).
  A material whose shader resolves `shadows=off` notes "Casts no shadows" under its
  Surface rows.
- Blend define in the library: particle's `OUTPUT` and sprite's `ALPHA` keywords are gone;
  both follow `BLEND_*`. Saved `shaderFeatures` still naming `OUTPUT_*` /
  `ALPHA_PREMULTIPLIED` load and render as if absent (`resolveFeatures` drops undeclared
  entries; MCP refuses to write them anew). With the Premultiplied blend the sprite reads
  its texture as premultiplied (un-premultiplied for the effects, premultiplied again on
  output); the particle premultiplies its straight colour, as its old keyword did.
- `output.glsl` conventions (refining 13.5): `finalizeColor` takes a colour as its texture
  stores it: the premultiplied blend expects it premultiplied (unlit, panner, flipbook,
  decal and the opaque shaders keep that), and the multiply blend now folds alpha in
  (`mix(1, rgb, a)`, alpha 1 out) so transparent parts leave the scene unchanged.
  `finalizeStraightColor` is for a straight-alpha colour a shader computes: under the
  premultiplied blend it fogs the straight colour, encodes it and multiplies by alpha
  (identical to what the alpha blend draws); otherwise it is `finalizeColor`. Particle,
  sprite, hologram, rim and water use it. With the default blend of every shader the
  output is unchanged.
- Shadows by state: `shadows=off` on rim, hologram, water, decal (see-through glows,
  surfaces and overlays) and sprite (its shape is its alpha and it has no depth program: a
  shadow would be the whole quad; Unity's sprites cast none either); flipbook
  `@state[!ALPHA_CLIP] shadows=off` (a blended sheet has no solid shape; with alpha clip its
  depth program cuts the shadow to the frame). Particle, UI and sky shaders are not in
  shadow passes; panner is opaque by default (a blended panner's mesh turns `castShadows`
  off); the opaque, clipping and moving shaders keep their shadows.
- Keyword state: hologram `@state[BLEND_MODE_ALPHA] blend=alpha`, decal
  `@state[DECAL_BLEND_MULTIPLY] blend=multiply`; both keyword groups are labelled "Default
  blend". The output follows the blend actually drawn (`BLEND_*`), so a material blend
  that overrides the keyword still draws right.
- `@require ... feature=` replaces the feature-driven `#undef`s: rim `uv1` with `TEXTURE`
  (rim.vert removed), vertex_color `uv1` with `TEXTURE` (vertex_color.vert removed), matcap
  `uv1` without `MATCAP_MODE_REPLACE`, fake_lit `uv2` with `SHADOW_TEXTURE` and `rawcolor`
  with `VERTEX_AO`, dissolve `normals` with `WORLD_NOISE`, toon `position normals` with
  `RIM` (pixel-lit variants get them anyway, 13.3). The `#undef`s left are not feature
  conditions: they depend on the lighting mode (`USE_PUNCTUAL`), keep an attribute without
  its varying (flipbook's `uv1` becomes `v_frameUV`, triplanar's object space, dissolve's
  world noise varyings), fall back on a mesh without an attribute (fake_lit's `uv2`), or
  leave out of a depth program what it does not read. simple_lit keeps its derivative
  tangent frame (it works on meshes without tangents).
- Raw vertex colour: `SURFACE_NO_VERTEX_COLOR` now only keeps the vertex colour out of
  `v_color`; the attribute is still read for `v_rawColor` and, with the new
  `SURFACE_DISPLACE_COLOR`, for the displacement hook (both stages). foliage: the vertex
  colour never tints (its red is the wind mask, read raw by the hook in both programs),
  `v_color` is the instance colour (tints the leaves, alpha included); fake_lit: `v_color`
  (instance colour) tints the albedo with or without `VERTEX_AO`, whose occlusion is
  `v_rawColor.r`.
- toon's `toonReach` uses `getLightData`; no library shader reads `objectLights.*`.
- `@property` / `@texture` rows take `feature=!<DEFINE>` too (shown, and checked against a
  variant's reflection, while the define is off; `ShaderManifest::isRowShown`): matcap's
  base map and tiling are `feature=!MATCAP_MODE_REPLACE`, so `check_shader` on its default
  REPLACE variant no longer reports the base map as undeclared.
- `check_shader shader="library:<name>[#...]"` checks the variant the engine would draw: the
  named features plus the manifest's defaults (a keyword group's first value, default-on
  features) and the blend define of the resolved state (or a `BLEND_*` the reference
  names); `library:triplanar` alone used to compile with no `SPACE_*` define and fail. The
  same holds for the references `check_shader` (no arguments) and `check --shaders` find in
  use: a bare `library:` customShader or scene default, and every UI/Points/Lines/Sky
  component's shader (with or without features), are checked as resolved variants.
- MCP `list_shaders` detail: `require` lists every input (`position`, `rawcolor` were
  missing), `require_rules` (`{require, feature}`, `"!X"` while off), `state.shadows` and
  `state_rules` (`{feature, state}`).
- Fixture: `GROUPS` lists only groups with a script (core, lit, stylized, effects, sky, 2d,
  checks); an unknown `SL_GROUPS` name or a listed group that fails to load is an error.
  The effects and 2D groups rely on the shaders' state for shadows and blends (no manual
  `castShadows = false`, keyword or blend mode for them).
- Pre-existing, found by the export check: a scene with more than one Skybox never counted
  as loaded (only the first is drawn and loaded, but every one was counted), so its loading
  timed out with "resources did not load" and the ready signal never came. Only the first is
  counted now.
- `docs/shaders/library.md` is generated by `jasperengine-editor shaders --library all --doc
  docs/shaders/library.md` (`editor/shader/ShaderLibraryDoc.cpp`): regenerate it after
  changing a manifest.
