# Water reflections: planar reflection, correct layering, Gerstner waves and a game clock for library:water (design)

Date 2026-11-04. Branch `dc/water-reflections`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md` (#15).
Dossier: `dc-work/dossiers/water-reflections.json` (Doriax 2247e891f, 07f1ad23c, d7e77e80e, d62f09407, ca6335cc5;
18beae6d8 is context only). EXTRA-FINDINGS.md has no water entries.

Claims about today's code below are read from the source at `main` `b4f7816ea` (inferred from reading, not run),
unless marked otherwise.

## 1. Goals

1. `library:water` can show the scene mirrored in it: a material feature `PLANAR_REFLECTION` that samples a
   reflection pass the engine renders once per water height and viewer, with a scene-level quality block (resolution,
   layers, shadows, far clip, clip plane offset, a cap) and an automatic fallback to the environment reflection.
2. Transparent objects under (or over) a water are layered correctly: drawn before the water when the surface is
   between them and the camera, after it when they are on the camera's side, and present in the scene-colour copy a
   refracting water samples. Refracting waters stacked on screen see each other.
3. The scene-colour copy reproduces the pass colour exactly (no blend of the captured alpha).
4. The water's shadow lookup uses the smooth surface normal (no speckled shadow edges on ripples) and its caustics
   go dark where the surface is in shadow.
5. A `GERSTNER` wave option (four Gerstner waves, sharp crests, horizontal motion) with an exact CPU twin, so
   buoyancy, `Water.*` queries, Water Volumes and the underwater switch ride the waves on screen.
6. The water animates on a per-scene game clock: it holds while the game is paused (editor Pause, time scale 0,
   app in the background, a held simulation), slows with `Time.timeScale`, and starts from 0 when the scene loads;
   in a stopped scene it follows a Scene view toggle. The CPU waves read the same clock.
7. Forks of `library:water` and `library:underwater` keep every engine behaviour (buoyancy, `Water.*` queries,
   Water Volume waves, the automatic underwater pass, layering, planar reflection), and forking a library shader is
   one undoable step in the Inspector and one MCP call.

## 2. Non-goals

- A Doriax-style `WaterComponent` (its own grid mesh and fields). Jasper's water stays `library:water` on any mesh
  (the redesign the dossier records); everything here is shader features, scene settings and engine roles.
- FFT oceans and GPU readback of water heights (`docs/specs/README.md` do-not-copy list, S22 non-goals).
  Gerstner is analytic and has a CPU twin, so it is allowed (S22-gameplay.md:41 asks only that a new wave model
  updates the mirror, which this spec does).
- Planar reflection for the built-in Lit (glossy floors). `MirrorComponent` keeps doing mirrors; the new
  `@require planarreflection` is open to any library shader or fork, so a later `library:mirror` can use it.
- Reflections of reflections: a reflection pass does not draw reflecting waters, scene-colour or scene-depth readers.
- Planar reflections in render-to-texture cameras and reflection probe faces (they fall back to the environment).
- Moving the rest of the shader library to the scene clock. `frame.time` stays the wall clock (foliage wind,
  `spin_bob`, hologram, flipbooks animate in the editor and in pause menus as today); any shader can opt into the
  scene clock by declaring a `sceneTime` member (6.1).
- Doriax's "Disable underwater" Scene view toggle (ca6335cc5): Jasper never switches the underwater pass in a
  stopped scene (`Water::updateUnderwater` returns unless `Engine::isSceneRunning`, Water.cpp:277), so there is
  nothing to disable.
- Depth programs for forks (`@depth` keeps working for library shaders only). `library:water` casts no shadows and
  draws in the transparent queue, so its depth program does not run in normal use; a fork made by `fork_shader`
  reports that its depth passes use the generic depth shader.
- Wakes, exclusion volumes and hulls: feature `water-boats`, built on this spec's layout (section 13).

## 3. Users and stories

- Human building a lake level: ticks **Planar reflection** on the Water material; the trees and the boat appear
  mirrored in the lake, wobbling with the ripples. On the phone build the scene's **Planar Reflections > Resolution**
  is Quarter; for the lowest tier the game's options menu sets `scene.planarReflectionResolution =
  PlanarReflectionResolution.OFF` and the water shows the sky reflection again, without an extra pass.
- Human with fish (alpha-blended meshes) and particles under a refracting lake: they show through the water, bent
  and tinted, instead of vanishing or floating on top of it.
- Human pausing the game in the editor: the boat and the waves freeze together; slow motion at `Time.timeScale =
  0.25` slows the waves as much as the boat.
- Human wanting rolling ocean swell: ticks **Gerstner waves**, raises **Crest sharpness**; buoys and boats ride the
  sharper crests and drift back and forth with them.
- Human customising the water: picks **Fork "Water"** in the material's Shader picker, edits the colour math in
  `shaders/Water.frag`; the boat still floats, `Water.getHeight` still answers, diving still fades the view.
- Agent: `set_properties` `submeshes[0].material.shaderFeatures` adds `PLANAR_REFLECTION`; `get_render_stats` shows
  `planar_reflections.passes = 1`; `screenshot` shows the mirrored cube; `set_scene_settings
  {planar_reflection_resolution: "quarter"}`; `sample_water` reports `water_time` and confirms the height holds while
  Play is paused.
- Agent: `fork_shader {source: "library:water", target: "material", entity: "Lake"}`, edits the fork, then
  `check` reports nothing and `eval_lua Water.getHeight(scene, 0, 0)` still returns a number.

## 4. Findings

- **F1 No planar reflection on water** (2247e891f). `library/water.frag:548-566` REFLECTION reads only
  `environmentColor` (the sky's prefiltered cube or a reflection probe); nothing binds a mirror target to a library
  shader (`ShaderRequireFlags`, `engine/core/shader/ShaderManifest.h:66-99`, has no such input). The only planar
  reflection is `MirrorComponent`, wired to the built-in Lit's base colour texture (`RenderSystem.cpp:12642-12750`
  `updateMirrors`, `:17645-17760` `renderViewMirrors` for editor views, `mesh.frag:254-261` `USE_MIRROR`). It renders
  a full-resolution pass with its own shadow maps (`:17728-17732`).
- **F2 Transparents behind a refracting water vanish** (2247e891f). A 3D pass copies the scene colour right after its
  opaque queue and atmosphere (`RenderSystem.cpp:16154-16155`); the refracted sample replaces the target once
  `refractionWeight` reaches 1 (`water.frag:408-411`, written with alpha 1 by `finalizeStraightColorOverScene`), so a
  transparent drawn before the water (farther in the sort) is overwritten and is not in the copy. Documented as a
  limit (`water.frag:126-132`, `docs/mcp/guide/shaders.md:542-547`, `docs/shaders/library.md:904`). The 2D path
  already copies right before the first reader (`:16249`).
- **F3 Transparents under a water sort by centre distance** (2247e891f, Doriax `isBehindWater`). The transparent
  queue sorts by `sortPriority`, then distance to each object's transform (`RenderSystem.h:626-634`,
  `RenderSystem.cpp:16074`). A large lake's transform is far from most of its surface, so a fish under it whose centre
  is nearer the camera draws after the water, on top of it, untinted.
- **F4 Overlapping refracting waters** (d7e77e80e). One copy per pass (`RenderSystem.cpp:16016-16019`): a refracting
  pool over a refracting lake samples a copy without the lake.
- **F5 The scene-colour copy blends the captured alpha** (d7e77e80e). `loadSceneColorCopy` ends with
  `endLoad(PIP_EFFECT, ...)` without a blend (`RenderSystem.cpp:9735`), so the render state's default
  `BlendMode::ALPHA` (`render/Render.h:117`) applies (`SokolObject.cpp:349-351`): the copy holds `rgb * a` over the
  black clear (`RenderSystem.cpp:9759`). Wrong wherever the pass target's alpha is below 1, e.g. a scene background
  colour with alpha < 1. Affects every scene-colour reader (glass, heat haze, water).
- **F6 Shadow lookup with the rippled normal** (07f1ad23c). `water.frag:502,516` passes the rippled `n` to
  `lightShadow`, which offsets the receiver along that normal (`includes/shadow_sampling.glsl:297-303`): the shadow
  edge follows the ripples. Speckling is inferred from the maths, not yet seen; the regression test (10) settles it.
  The dossier's correction holds: `surfaceNormal` (`:341`) is taken after `n = -n`, so from below the lookup must keep
  `lightNormal` (up).
- **F7 Caustics ignore shadows** (07f1ad23c). `water.frag:425` takes the sunlight from `frame.mainLightColor`
  unshadowed: a pier's shadow on the water does not darken the caustics under it.
- **F8 Two sine waves only** (2247e891f). `library/water.glsl:17-30`; twin `engine/core/util/WaterWaves.h:36-93`,
  pinned by `WaterWavesTest.GlslTwinPinned` and `WaterWavesGpuTest`.
- **F9 Waves run on the wall clock** (ca6335cc5). `water_vs.glsl:21`, `water.frag:261,318,350,426` read `frame.time.x`
  = `Engine::getShaderTime()` (`RenderSystem.cpp:15223`), which is `getSystemTime()` (`Engine.cpp:1012`) unless pinned
  or under a fixed frame delta. The CPU twin reads the same (`Water.cpp:83`, `WaterSystem.cpp:54,176`). So waves keep
  moving while the editor's Play is paused (physics frozen by `pauseEngineScene`, `editor/Project.cpp:5802-5814`), at
  `Time.timeScale = 0`, and at full speed in slow motion: bodies and waves disagree.
- **F10 Forks lose the engine wiring** (d62f09407). The engine recognises water by the exact name:
  `Water::readMaterial` (`Water.cpp:93`), `Water::updateUnderwater` (`:284,309`), `WaterSystem::gatherVolumes`
  (`WaterSystem.cpp:85-93`, which also reads defaults from the library manifest by name), `CreateEntityCmd.cpp:923,1014`
  (underwater pass detection). `fork_shader` copies only built-in types (`editor/mcp/tools/AssetTools.cpp:2612-2733`,
  `shader_type` enum at `:4052`), and the Inspector's picker offers only "Custom file..." (`InspectorShaderLibrary.cpp:334`).

## 5. Product decisions

1. **Planar reflection is a material feature, its quality a scene setting.** Users tick it where they see the water
   (Unity's old Standard Assets water had the same per-water switch); resolution and layers are per scene because one
   pass serves every water at that height. Unity HDRP's Planar Reflection Probe names are used for the scene block
   (Resolution, Culling Mask as "Layers", Far Clip Plane, Clip Plane Offset).
2. **One reflection pass per water height and viewer, nearest heights first, at most `maxPlanarReflections` (default
   2).** Tiles of one lake share a pass; a third level falls back to the environment reflection instead of tripling
   the cost. Reason: mobile and web budgets.
3. **Default resolution Half; Off is a real quality level.** Off draws no pass, allocates nothing and every
   `PLANAR_REFLECTION` water shows the environment reflection, so a game's quality menu can switch it at runtime.
4. **Reflection passes reuse the viewer's shadow maps** (Shadows on by default) instead of rendering their own as
   mirrors do: shadows are world-space, the cost is the lookups only.
5. **Layering by water rank, not by special queues.** Every distance-sorted transparent draw gets a signed rank: +1
   for each visible water whose surface lies between it and the camera, -1 for each it lies on the camera's side of;
   the queue sorts by `sortPriority`, then rank (higher first), then distance. This fixes the fish (F3), the copy
   (F2), stacked waters (F4) and smoke above the water with one total order. The user's `sortPriority` still wins.
6. **The 3D copy is taken right before the first scene-colour reader draws** (as the 2D path does), and taken again
   before a reader whose screen box overlaps a reader of a higher rank drawn since the last copy (at most 4 copies a
   pass). Glass over glass at the same rank keeps today's single copy (Unity URP's opaque texture behaviour), so no
   project pays more unless waters (or readers) are layered.
7. **The water's clock is the scene clock** (`Scene::getSceneTime`), advanced by the scaled frame delta of the frame,
   held by everything that pauses the game; Unity's `_Time` follows game time the same way. It is per scene, starts at
   0 when the scene starts, and pinned shader time (golden tests) pins it too. Shaders read it through an engine-written
   custom uniform `sceneTime`, beside the existing `time` and `resolution`, so the frame block layout is untouched.
8. **Stopped scenes animate their water by default** (Scene view > Rendering > **Animate water**, on), as Jasper does
   today and Unity's Scene view "Always Refresh" allows; off holds it (Doriax's behaviour).
9. **Gerstner is an opt-in feature under WAVES** with a 0..1 **Crest sharpness** normalised so the surface never
   folds; the new GameObject menu Water turns it on (steepness 0.5). Existing materials keep the sines.
10. **Gerstner flow includes the orbital motion** of the surface: `Water.sample(...).flowVelocity` and the Buoyancy
    drag see water moving back and forth under a crest, so boats bob forward and back as well as up and down.
11. **Engine roles are a manifest line**, `// @role water` / `// @role underwater`, copied with the manifest into any
    fork. Name matching stays as a fast path. A fork that looks like water but lacks the role gets a `check` warning.
12. **Caustics darken with the surface's shadow** (Doriax's choice): the main light's shadow factor at the surface,
    which the pixel-light loop already computes, scales the caustic sunlight. No extra shadow lookup.

## 6. Design

### 6.1 The scene clock (engine)

`engine/core/Scene.{h,cpp}`:

```cpp
// The clock of this scene's game-time animation (library:water's waves and ripples, shaders declaring
// sceneTime): seconds the scene has run, scaled by Time.timeScale; held while the game, the editor's Play
// or this scene is paused and while the simulation is held. 0 when the scene starts. A pinned shader time
// (Engine::setPinnedShaderTime) replaces it everywhere it is read.
double getSceneTime() const;          // pinned value when pinned, else sceneTime
void setSceneTime(double seconds);    // the editor's edit-mode clock, tests
void advanceSceneTime(double delta);  // no-op while sceneTimePaused
void setSceneTimePaused(bool paused);
bool isSceneTimePaused() const;
```

Fields `double sceneTime = 0.0; bool sceneTimePaused = false;` (not serialized). `Engine` gains
`static bool hasPinnedShaderTime()` and `static double getPinnedShaderTime()`.

- `Engine::runFrame` (`Engine.cpp`, inside `if (!paused && !simulationHeld)` beside `gameTime += frameDelta`, line
  ~2277, before the fixed steps): for each running scene, `scene->advanceSceneTime(frameDelta)`. So the fixed steps,
  the update and the draw of a frame see one value, as with today's latched shader clock. Update-only frames
  (`systemUpdate`, hidden games: feature `platform-lifecycle`) advance it the same way.
- `editor::Project::pauseEngineScene` (`editor/Project.cpp:5802`) also calls `scene->setSceneTimePaused(pause)`;
  the editor's frame Step therefore advances it by exactly one frame (it unpauses for one frame).
- Stopped scenes (edit mode): once per editor frame, for each open scene that is not playing,
  `if (displaySettings.animateWater) scene->setSceneTime(scene->getSceneTime() + unscaledEditorDelta)`. One place
  (the editor's per-frame scene update in `Project`/`App`, not per view, so a scene drawn by two views advances once).
- `RenderSystem::drawFramePasses` (`RenderSystem.cpp:15223`) also latches `frameSceneTime = (float)scene->getSceneTime()`
  (float for upload; the waves wrap their phases with `fract`, so precision holds for many hours).
- Engine-written custom uniform `sceneTime`: `ShaderUniforms.cpp:16` adds it to the engine-written names;
  `CustomUniformBlock::writeSceneTime(float)`; `applyCustomUniforms` (`RenderSystem.cpp:401-408`) gains the parameter
  and writes it for every custom block (mesh colour and depth, extra pass, UI, points, lines, sky, post-process).
  Declared nowhere, it costs nothing (the block has no such member).
- `Water::getTime(Scene* scene = nullptr)`: `scene` (null: `Engine::getMainScene()`) `->getSceneTime()`, 0 without a
  scene. `WaterSystem::gatherVolumes` (`:54`) and `WaterSystem::sample` (`:176`) read `Water::getTime(scene)`.
- `library:water` reads `customParams.sceneTime` wherever it read `frame.time.x` (`water_vs.glsl:21`, `water.frag:261,
  318,350,426`).

### 6.2 Waves: GERSTNER and the CPU twin

Manifest additions (`library/water.frag`):

```
// @feature GERSTNER "Gerstner waves (with WAVES: sharp crests, the surface rolling forward)"
// @property waveSteepness range(0, 1) 0.5 "Crest sharpness" feature=GERSTNER
```

The wave set (from `waveDirection` d, `waveHeight` A, `waveLength` L, `waveSpeed` c, steepness
s = clamp(waveSteepness, 0, 0.95)):

| i | direction | length L_i | height A_i | phase offset (cycles) |
|---|---|---|---|---|
| 0 | d | L | A | 0 |
| 1 | d turned +50 degrees | 0.6 L | 0.5 A | 0.37 |
| 2 | d turned -30 degrees | 0.37 L | 0.28 A | 0.71 |
| 3 | d turned +80 degrees | 0.23 L | 0.16 A | 0.13 |

Speed c_i = c * sqrt(L_i / L) (deep-water dispersion). Phase phi_i = (dot(d_i, p) / L_i - fract(t * c_i / L_i) +
offset_i) * 2 pi, k_i = 2 pi / L_i. The surface point at rest position p moves by
`offset = sum_i (d_i.x * (s / (4 k_i)) * cos phi_i, A_i * sin phi_i, d_i.y * (s / (4 k_i)) * cos phi_i)`.
The horizontal amplitudes sum to s / 4 per wave in units of 1 / k_i, so the horizontal map's Jacobian stays positive
(no folding) for s < 1. Tangents: `dPdx = (1 - sum (s/4) d_ix^2 sin phi_i, sum A_i k_i d_ix cos phi_i,
-sum (s/4) d_ix d_iz sin phi_i)`, `dPdz` likewise; normal `normalize(cross(dPdz, dPdx))`. Velocity of the surface
particle: with omega_i = 2 pi c_i / L_i, `(sum d_ix (s/(4k_i)) omega_i sin phi_i, -sum A_i omega_i cos phi_i,
sum d_iz (s/(4k_i)) omega_i sin phi_i)`.

GLSL (`library/water.glsl`, beside `waterWaves`):

```glsl
// the offset of the surface point at rest position xz (world), its tangents along world X and Z; slopeWeights
// scale each wave's share of the tangents (the fragment stage's distance fade; 1 elsewhere)
vec3 waterGerstner(vec2 xz, float time, vec2 direction, float height, float waveLength, float speed,
                   float steepness, vec4 slopeWeights, out vec3 dPdx, out vec3 dPdz);
```

Written unrolled (no indexing a vec4 by a loop variable: GL 3.3 and GLES3 portability).

CPU twin (`engine/core/util/WaterWaves.h`), line-for-line like `waterWaves` (each dot-product term its own statement):

```cpp
struct WaterGerstnerSample{ Vector3 offset; Vector3 dPdx; Vector3 dPdz; Vector3 velocity; };
inline WaterGerstnerSample waterGerstner(Vector2 xz, float time, Vector2 direction, float height,
                                         float waveLength, float speed, float steepness);

// what a material's waves are (Water::readWaves); evaluated here for every CPU reader
struct WaterWaveParams{
    bool waves = false; bool gerstner = false;
    Vector2 direction = Vector2(1.0f, 0.3f);
    float height = 0.0f, length = 1.0f, speed = 0.0f, steepness = 0.0f;
    float getBand() const;            // the most the surface leaves its rest level: 1.5 A (sines), 1.94 A (Gerstner)
    float getHorizontalReach() const; // the most a point moves sideways: 0, or s / 4 * sum 1 / k_i
};
// WaterWaveSample gains: Vector3 normal; Vector3 velocity; (sines: (0, heightRate, 0))
// The surface over a world XZ: for Gerstner, Newton on rest + offset.xz(rest) = xz (start at xz, at most 6 steps: Deviations 12,
// the 2x2 Jacobian from dPdx/dPdz, a plain fixed-point step if its determinant is under 1e-4, stop under
// (1e-5 L)^2); height, slope (from the normal), normal and velocity of the point found.
WaterWaveSample waterSurfaceAt(const WaterWaveParams& params, Vector2 xz, float time);
```

`WaterWavesTest.GlslTwinPinned` pins both GLSL functions (a second SHA-256 constant for `waterGerstner`), with the
same "change both together" failure message.

Vertex stage (`library/water_vs.glsl`): its `u_vs_customParams` appends `float waveSteepness; float sceneTime;`;
`waterWavePosition` under `GERSTNER` uses `waterGerstner(...)` (weights 1) and adds the whole offset
(`motionWorldToLocal`), and stores the rest XZ in a file-scope `vec2 waterRestXZ`. `library/water.vert` under
`GERSTNER` defines `SURFACE_VERTEX`, declares `out vec2 v_waveRest;` and writes it from `waterRestXZ` in
`surfaceVertex` (`includes/surface_vs.glsl:72-75` hook). `water_depth.vert` needs no varying.

Fragment stage (`water.frag`, under `WAVES`): with `GERSTNER`, `in vec2 v_waveRest;`; the tangents come from
`waterGerstner(v_waveRest, ...)` with per-wave weights `1 - smoothstep(0.04, 0.35, footprint / L_i)` (each wave fades
where a pixel covers a good part of it; today's single fade uses L), the normal is bent towards
`cross(dPdz, dPdx)`, `waveDetail` becomes the primary wave's weight (it feeds roughness as today), and the crest foam
term is `0.15 * max(clamp(offset.y / waveHeight, 0, 1), clamp((1 - det J) / max(s, 1e-3), 0, 1))` (J the horizontal
Jacobian: compression at the crests). Without `GERSTNER` the code is today's.

`Water.cpp`: `WaterSurface` replaces its wave fields by `WaterWaveParams waves`; `getHeight`, `getNormal` call
`waterSurfaceAt`. `WaterSystem::Volume` likewise (`WaterSystem.h:58-62`); `sample` fills `flowVelocity` from the
sample's `velocity` (decision 10). `Water::getWaves` stays (the sines; existing callers). Layering (6.5) widens a
water's area by `getHorizontalReach()` and its band by `getBand()`.

### 6.3 Water identification: roles and forks

- `ShaderManifest` (`shader/ShaderManifest.h`): `enum class ShaderRole { NONE, WATER, UNDERWATER }; ShaderRole role`.
  `parse` (`ShaderManifest.cpp`, beside `directive == "depth"` at :773) accepts `@role water|underwater`; another value
  is an error diagnostic; a second `@role` line is an error. `library/water.frag` and `library/underwater.frag` get the
  line.
- `Water` (`object/environment/Water.h`):

```cpp
static bool isWaterShader(const std::string& reference);      // library:water, or a manifest with @role water
static bool isUnderwaterShader(const std::string& reference); // library:underwater, or @role underwater
// the waves of a water material (WAVES, GERSTNER, values set or the shader's own manifest defaults)
static bool readWaves(const Material& material, WaterWaveParams& waves);
```

  Name equality first; otherwise only a fork reference (not `library:`/`builtin:`) asks `ShaderManifests::get`
  (cached, thread-safe). Every `getParam(values, Water::SHADER, ...)` becomes `getParam(values, material.shader, ...)`
  so a fork's own defaults apply.
- Call sites: `Water::readMaterial`, `Water::updateUnderwater`, `WaterSystem::gatherVolumes` (uses `readWaves`),
  `CreateEntityCmd.cpp:923,1014`, `WaterEditorUtils::materialWaveLength`, the RenderSystem water registry (6.4, 6.5).
- **Forking a library shader.** `ForkShaderCmd` (`editor/command/type/ForkShaderCmd.{h,cpp}`) gains a source library
  reference: the files written are the library entry's `.frag` and its `.vert` (or `library/surface.vert` for a mesh
  shader without one, `fullscreen.vert` for post-process), copied verbatim with their manifest lines (so `@role`,
  properties, features and textures carry over). `fork_includes` also copies the `library/*.glsl` the entry includes,
  into `library/` beside the fork. The fork's `#include "library/..."` lines resolve against the engine library as
  `includes/` do (`ShaderVariantCompiler::getCustomShaderIncludeRoot` and the include resolver; extend the fallback if
  it only covers `includes/`). The material keeps its `shaderFeatures`, `shaderParams` and `shaderTextures` (they are
  keyed by name).
- MCP `fork_shader`: new optional `source` (a library reference, e.g. `"library:water"`) and target `material`
  (`entity`, optional `submesh`; default every submesh whose shader is `source`; or `path` of a `.material`, whose
  linked submeshes follow). With `source` the shader type comes from the library manifest and must match the target
  (`material`: mesh; `post_process`: postprocess, replacing that pass's shader and keeping its uniforms; `component` /
  `scene_default`: ui, points, lines, sky). The result says when the library shader has a depth program the fork does
  not use.
- Inspector: the Shader picker (`InspectorShaderLibrary.cpp:333-338`) shows **Fork "<Name>"...** under "Custom
  file..." while a library shader is selected: writes `shaders/<Name>.vert/.frag` (unique name,
  `ProjectUtils::makeUniqueShaderName`) and points the material (or pass, or component) at it, one undo step.
- `check` rule `water-fork-without-role` (warning, `editor/util/ProjectChecks.cpp`): a fork used by a material or pass
  whose manifest declares the water's `buoyancy`, `waveHeight` and `shallowColor` properties (or underwater's
  `waterLevel` and `followWater`) but no `@role`: "add `// @role water` to its manifest so the engine floats bodies,
  answers Water queries and switches the underwater pass".

### 6.4 Planar reflection

**Shader input.** `ShaderRequire_PlanarReflection = 1 << 12` (`ShaderManifest.h`), manifest token `planarreflection`,
define `SURFACE_PLANAR_REFLECTION`, mesh shaders only, fragment stage. New `engine/shaderlib/includes/planar_reflection.glsl`
modelled on `includes/environment.glsl`:

```glsl
uniform u_fs_planarReflection {
    mat4 viewProjection;  // the reflection camera's, logical (oblique near plane)
    vec4 params;          // x the plane's height (world Y), y 1 with a reflection, 0 without; zw unused
} planarReflection;
uniform texture2D u_planarReflectionTexture;
uniform sampler u_planarReflection_smp;
bool planarReflectionPresent();
// the reflection seen at a world point of the water (flattened onto the plane), shifted by a screen offset;
// UV as mesh.frag's USE_MIRROR (0.5 + x / 2, 0.5 - y / 2), clamped inside the target; textureLod level 0
vec3 planarReflectionColor(vec3 worldPosition, vec2 screenShift);
```

**Water manifest.**

```
// @require planarreflection feature=PLANAR_REFLECTION
// @feature PLANAR_REFLECTION "Planar reflection (with REFLECTION: the scene mirrored; a camera pass per water height)"
// @property reflectionDistortion range(0, 0.2) 0.03 "Planar reflection distortion (screen UV per unit of ripple)" feature=PLANAR_REFLECTION
```

In `water.frag`'s REFLECTION block, from above only: with `PLANAR_REFLECTION` and `planarReflectionPresent()`, the
reflection colour is `planarReflectionColor(vec3(v_position.x, params.x, v_position.z), shift)` (shift: the ripples'
view-space bend times `reflectionDistortion`, aspect-corrected and clamped at the borders as REFRACTION's) instead of
`environmentColor`; otherwise today's environment path. The branch is on a uniform. `reflectionColor` and
`reflectionStrength` scale both. Final `u_fs_customParams` tail order (after `meshShoreFade`): `waveSteepness`,
`reflectionDistortion`, `sceneTime` (water-boats appends after these).

**Scene settings** (`engine/core/SceneSettings.h`, `Scene` getters/setters):

```cpp
enum class PlanarReflectionResolution { OFF, QUARTER, HALF, FULL };   // per axis 0, 1/4, 1/2, 1 of the viewer's pass
PlanarReflectionResolution planarReflectionResolution = PlanarReflectionResolution::HALF;
uint32_t planarReflectionLayers = LAYER_MASK_EVERYTHING; // ANDed with the viewer's culling mask
bool planarReflectionShadows = true;     // sample the viewer's shadow maps
float planarReflectionFarClip = 0.0f;    // 0: the viewer's far clip; else min(viewer, this)
float planarReflectionClipOffset = 0.05f;// the oblique near plane this far above the water's rest level
int maxPlanarReflections = 2;            // 1..4 reflection passes per viewer and frame
```

**RenderSystem** (`engine/core/subsystem/RenderSystem.{h,cpp}`):

- Registry `planarReflectionMeshes` (entity set), updated where a submesh's shader inputs are resolved
  (`getSubmeshShaderInputs`, `RenderSystem.cpp:3595-3625`, and mesh destroy): a mesh is in it while one of its submesh
  variants requires `planarreflection`. Empty: everything below is skipped in O(1).
- Per viewer and frame (`gatherReflectionPlanes(viewer camera, eye, culling mask)`): for each registry mesh that is
  render-active, visible, in the viewer's layers and frustum (world AABB), with the eye above its level + 1e-3
  (`level` = world AABB top, as `Water::getSurfaces`): group by level (joined within 0.01 world units), order groups by
  |eye.y - level|, keep the first `maxPlanarReflections`. Resolution `OFF`: no groups. The rest are recorded as
  fallbacks (stats).
- One shared helper extracted from `updateMirrors` / `renderViewMirrors`: `driveReflectionCamera(CameraComponent&
  refCam, Transform& refTransform, const CameraComponent& viewer, const Vector3& viewerEye, const Plane& plane, float
  clipOffset)` (reflected view, reflected eye, `invertCulling`, the viewer's projection, oblique near clip on the
  plane raised by `clipOffset`, frustum planes). Mirrors call it too (no behaviour change for them).
- Primary view (games, the editor's Game view): per slot an internal system camera entity created and driven as
  `createMirrorCamera` does (`RenderSystem.cpp:12581-12607`; `renderToTexture`, `autoResize` off, framebuffer
  `viewport * scale`, at least 16 px a side, RGBA8 + depth, sample count 1, linear filter, clamp), rendered by the
  existing render-to-texture camera loop before the main camera (`hasMultipleCameras = true` while any slot is
  active). Entities and targets of a slot unused for 120 updates are destroyed (the scene-colour redirect's hold
  pattern, `RenderSystem.cpp:9524-9527`).
- Editor secondary views (Scene view): `renderViewWaterReflections(view, camera)` beside `renderViewMirrors`, with a
  `WaterReflectionView` per slot shaped like `MirrorView` (`RenderSystem.h:1047-1061`).
- A reflection pass (`CameraPass::reflection = true`): culling mask = viewer mask & `planarReflectionLayers`; far
  clip per setting; clear to the scene background, sky drawn; no post-processing, SSAO, SSR, screen clouds, editor
  overlays, HTML UI documents, video planes or screen fluids (as other render-to-texture passes); scene inputs forced
  to none and the draws reading them skipped; registry meshes skipped; LOD: the viewer's `LodView` when the viewer is
  the main camera, else full detail as other secondary cameras; terrain cut selected for the pass (as mirrors,
  `selectPassTerrain`). Shadows: on, `passShadows` = the viewer's `CameraShadows` (no `prepareCameraShadows` /
  `renderCameraShadows` for it); off, an empty `CameraShadows` (lights unshadowed).
- Binding per draw: `applyPlanarReflection(render, bindings, mesh, transform)` beside `applyEnvironment`
  (`RenderSystem.cpp:5085`), with `slotFSPlanarReflection` and the texture slot found by name like
  `slotFSEnvironment` (`:4028`). It binds the slot of the mesh's plane for the current viewer, else a 1x1 black
  texture with `params.y = 0` (render-to-texture cameras, probe faces, cap exceeded, Off, eye below the plane).
- Stats `PlanarReflectionStats { uint32_t planes, passes, fallbacks; uint32_t width, height; }` per frame, with a
  `last` copy like `sceneInputStats` (`RenderSystem.h:1783-1791`).

### 6.5 Transparent layering and scene-colour copies

- `engine/core/render/WaterLayering.h` (pure, unit-tested):

```cpp
struct WaterLayer{ Entity entity; float level; float band; Vector2 minXZ; Vector2 maxXZ; }; // area widened by the reach
// +1 per water whose surface lies between the box and the eye over its area, -1 per water the box lies on the eye's
// side of; a box straddling a water's band, outside its area, null or not finite counts 0 for it; self excluded.
// Clamped to [-8, 8].
int waterRank(const AABB& box, const Vector3& eye, const std::vector<WaterLayer>& waters, Entity self);
```

  Eye at or above `level`: between = `box.max.y < level - band`; eye side = `box.min.y > level + band`. Eye below:
  mirrored.
- Registry `waterMeshes` (entity -> `WaterWaveParams`) maintained with the planar registry (6.4) from
  `Water::isWaterShader` of the submesh shaders. During the 3D gather (`RenderSystem.cpp:16040-16076`) each visible
  transparent water mesh adds a `WaterLayer` to the pass's list.
- `TransparentRenderData` (`RenderSystem.h:582`) gains `int8_t waterRank = 0`, set after the gathers (meshes,
  trails, points, lines, particles; each by its world AABB) and before the sort (`RenderSystem.cpp:16393`), only when
  the pass's water list is not empty. `TransparentRenderComparison` (`RenderSystem.h:626`): `sortPriority`, then
  `waterRank` descending, then distance, then order.
- Lazy copy in 3D: `takeSceneColor` moves from after the opaque queue (`:16154-16155`) to the transparent loop: right
  before the first entry that reads the scene colour (the per-entity test `drawsSceneColorReader`, `:9616`). A pass
  whose readers end up not drawn takes no copy.
- Re-copy: the loop keeps the screen rects (`projectedScreenRect(worldAABB, viewProjection)`, NDC, whole screen when
  a corner is behind the eye) and ranks of the readers drawn since the last copy; before a reader, if one of them has
  a higher rank and an overlapping rect, the copy is taken again (at most 4 copies a pass; stats
  `sceneInputStats.colorRecopies`).
- Copy blend (F5): `loadSceneColorCopy` ends with `endLoad(PIP_EFFECT, state)` where `state.blend = BlendMode::NONE`
  (other fields as the boolean overload sets them).

### 6.6 Shadow and caustics in water.frag

- The pixel-light loop passes `fromBelow ? lightNormal : surfaceNormal` to `lightShadow` (`water.frag:516`); Lambert and
  the glint keep `lightNormal` / `n`.
- Caustics (`WATER_CAUSTICS`): the pattern and fade are computed where they are now; the multiplier on `behind` is
  applied after the light loop. With `USE_PUNCTUAL` and `USE_SHADOWS`, the loop records `causticsShadow` = the shadow
  factor of the pixel light that is the main directional light (directional, its direction within 0.999 of
  `frame.mainLightDirection.xyz`), default 1; the caustic sunlight is today's `frame.mainLightColor.rgb *
  (frame.mainLightDirection.w * max(frame.mainLightDirection.y, 0.0))` (`:425`) times `causticsShadow`. Without pixel
  lighting or shadows it is today's.

### 6.7 Editor

- Material Inspector: the new features and properties appear from the manifest (labels above). Under a water material
  with `PLANAR_REFLECTION` an info row reads the scene's state ("Half resolution, 1 pass for this height" / "Off in
  this scene: the environment reflection is drawn") with a **Scene Settings** button; the picker marks the feature's
  cost like scene inputs (gauge, tooltip "renders the scene again: one camera pass per water height").
- Scene settings (`InspectorWindow.cpp`, after "Scene Color" at :12701): section **Planar Reflections**: Resolution
  (Off, Quarter, Half, Full), Layers (the layer mask widget the camera's Culling Mask uses), Shadows, Far Clip Plane,
  Clip Plane Offset, Max Planes, and a read-only **In Use** row from the last frame's stats. Undo through
  `drawScenePropertyRow` as the other rows.
- Scene view gizmo menu (`SceneWindow.cpp`, Rendering section ~:2493): **Animate water** (`SceneDisplaySettings::
  animateWater`, `editor/render/SceneRender.h:28`, default true, saved with the workspace), tooltip "Water and shaders
  on the scene clock move while the scene is stopped (Play always animates them)".
- GameObject > 3D Object > Water (`CreateEntityCmd.cpp:899-931`): adds `GERSTNER` to the features (steepness at its
  default 0.5). `PLANAR_REFLECTION` stays off (cost).
- Stream (`editor/Stream.cpp` ~:4331/:4456, `StreamSchema.cpp:62` key list and an enum table
  `planarReflectionResolution`: off, quarter, half, full): six keys, written only when not default.

### 6.8 MCP

- `get_scene_settings` / `set_scene_settings` (`SceneTools.cpp:800-830`): `planar_reflection_resolution`
  (off|quarter|half|full), `planar_reflection_layers` (in the form the camera's culling mask takes in
  `set_properties`), `planar_reflection_shadows`, `planar_reflection_far_clip`, `planar_reflection_clip_offset`, `max_planar_reflections`
  (1..4); `get_scene_settings` adds read-only `planar_reflections_in_use` {planes, passes, fallbacks, width, height}.
  `McpValues.cpp:801` enum table.
- `get_render_stats` (`ProfilerTools.cpp:1028`): `planar_reflections` (as above) and `scene_inputs.color_recopies`.
- `set_scene_camera` display flags (`ViewTools.cpp:940-958`): `animate_water`.
- `fork_shader`: `source`, target `material` (+ `submesh`, `path`), 6.3.
- `list_shaders`: `role` when set; `PLANAR_REFLECTION` gets a `cost_note` ("one camera pass per water height and
  viewer, scene setting planar_reflection_resolution"); `get_shader_uniforms` lists `sceneTime` as engine-written.
- `sample_water` (`GameplayTools.cpp:421`): adds `water_time` (the scene clock; `shader_time` stays the wall clock);
  Gerstner samples carry the orbital `flow_velocity`.
- `check`: `water-fork-without-role` (6.3) and `planar-reflection-not-flat` (warning: a `PLANAR_REFLECTION` mesh whose
  world bounds are taller than 1% of their larger horizontal extent plus 0.05: "planar reflection mirrors about one
  horizontal plane; use REFLECTION on sloped water"). `docs/mcp/checks.md`.
- No new tool and no group change. Regenerate `docs/mcp/TOOLS.md`; `tool_budget.py --check`.

### 6.9 Lua

- `Water.getTime(scene?)` (the scene clock; nil scene: the main scene). `GameObjectClassesLua.cpp:1116`.
- `Scene` properties (`CoreClassesLua.cpp` beside `sceneColorScale` at :3201): `planarReflectionResolution`,
  `planarReflectionLayers`, `planarReflectionShadows`, `planarReflectionFarClip`, `planarReflectionClipOffset`,
  `maxPlanarReflections`; namespace `PlanarReflectionResolution` (`OFF`, `QUARTER`, `HALF`, `FULL`).
- Features and values through the existing material API (`material:setShaderFeature("GERSTNER", true)`,
  `setShaderParam("waveSteepness", 0.7)`). `WaterSample.flowVelocity` documented with the orbital part.
- The build regenerates the Lua API data and stubs (`tools/lua_api`).

### 6.10 Docs

`docs/mcp/guide/shaders.md` "Water" and the scene-colour paragraphs (`:542-553`, `:583-640`: new features, the clock,
layering, re-copies, forks and `@role`, planar cost), `docs/shaders/library.md` regenerated
(`jasperengine-editor shaders --library all --doc ...`), `docs/mcp/guide/physics.md` "Floating in water" (clock,
Gerstner flow), `docs/mcp/guide/lua-api.md` Water section and Scene properties, `docs/mcp/guide/scene-view.md`
(Animate water), `docs/mcp/checks.md`. Agent guide: at most one line in the shaders digest ("water: planar reflection
is a camera pass per water height, scene `planar_reflection_*`; waves run on the scene clock and pause with the game"),
within the budget.

### 6.11 Export and platforms

Everything runs through sokol-gfx render targets, uniform blocks and 2D textures: GL 3.3, GLES3/WebGL2, Metal, D3D11,
Vulkan; no compute, no SSBO. The projective UV follows `mesh.frag`'s mirror convention, which already serves all
backends; the oblique projection is the mirror path's. Exports collect the new variants with the existing variant
collection (materials name their features); prebuilt players compile them like any library variant. Nothing is
platform-specific to build on Windows, macOS or iOS. Only glcore runs in the headless GPU tests here; the other
backends are covered by the cross-backend bound where the CI runs them, otherwise inferred.

## 7. Behaviour rules and edge cases

- Planar reflection is drawn only from above: the pass of a plane is skipped while the viewer's eye is at or below it,
  and the underside (`fromBelow`) never reads it.
- Waves, crests and Gerstner sideways motion: the lookup flattens the point onto the plane, the distortion comes from
  the ripples; waters on slopes (spline rivers) mirror wrongly by design (check rule).
- Orthographic viewers get the same pass (reflected view, same orthographic projection).
- A water hidden, inactive, culled by layer or frustum, or outside the cap costs no pass; capped planes are the
  farthest in height from the eye; their waters show the environment reflection (consistent with REFLECTION alone).
- Two viewers in a frame (Scene view and Game view) each get their own passes.
- Reflection passes draw no reflecting water, no scene-input reader, no editor gizmo.
- Layering only applies to distance-sorted passes (3D); 2D and `transparentSort` off keep hierarchy order.
- A transparent straddling a water's wave band keeps today's distance order relative to that water.
- Re-copies never exceed 4 per pass; past that the stale copy is used (stats say so).
- The scene clock: held by editor Pause, `Time.timeScale = 0`, app paused, simulation held, `pauseEngineScene`; it
  restarts at 0 for each Play and each scene load; additive and stacked scenes keep their own clocks; a pinned shader
  time overrides all of them.
- `Water.getTime()` with no scene in a game without a main scene returns 0.
- Gerstner `waveSteepness` above 0.95 acts as 0.95; `waveLength` and heights keep today's `max` guards; steepness 0
  gives four plain sine waves (vertical only).
- Forks: a fork without `@role` behaves as today (not water). A fork's own manifest defaults apply to its unset values.
- Old hand-copied forks reading `frame.time.x` keep the wall clock until edited; the CPU follows the scene clock only
  for shaders with `@role water`, which old copies do not have.

## 8. Performance and cost

- Unused (no material with the feature, or Resolution Off): no camera entity, no target, no pass, no per-frame scan
  (the registry is empty; one `empty()` check per frame and per view). Water meshes bind a 1x1 texture and one
  uniform block per draw only when their variant has the feature.
- Planar reflection on (estimate, to be measured in the PR with `get_render_stats` GPU timings on the reference
  scene): one extra camera pass per plane at the chosen resolution: Half draws a quarter of the pixels, plus the
  vertex and CPU cost of the objects in the reflected frustum and layers (reduce with Layers and Far Clip). Memory per
  plane at 1920x1080 Half: 960x540 x (4 + 4) bytes = about 4 MB. No shadow map rendering (decision 4).
- Layering: O(transparent draws x visible waters) per 3D pass, only when a water is visible; nothing otherwise.
- Copies: the first copy costs what it costs today; re-copies only with overlapping readers of different ranks.
- Scene clock: one addition per scene per frame; one float written per custom block that declares `sceneTime`.
- Gerstner: vertex stage four waves instead of two sines; fragment about 30 more ALU instructions per pixel (four
  waves' tangents) and one varying. GERSTNER off: the shader is today's.
- Shadow normal: none. Caustic shadow: a min and a compare inside the existing light loop.
- Copy blend NONE: none (blending disabled is never slower).

## 9. Compatibility

- No scene or asset format version bump: the six scene keys are optional with defaults and written only when not
  default; material features and properties are new names (absent = off / default); the water's uniform blocks only
  grow at their end, so existing materials draw the same values (golden `water_features_off` must match exactly).
- Behaviour changes, intended and documented (docs and release note in the PR): waves and ripples of
  `library:water` now start at 0 when a scene starts and follow time scale and pause; transparents under a water draw
  before it; scene-colour copies show colour where the target alpha is under 1; water shadow edges and caustics change
  slightly with pixel lighting and shadows on.
- `sample_water` gains keys; `fork_shader` gains an optional parameter and a target; `set_scene_camera` gains a flag.
- New GameObject > Water objects enable `GERSTNER`; existing ones are untouched.
- The editor workspace gains `animateWater` (absent: true).

## 10. Tests

Red controls: each test marked **red** fails on `main` before the change (run it first on the unchanged tree, keep the
failing output in the PR). The others test new features (their red is the feature absent).

| Suite | Test | Asserts |
|---|---|---|
| core gtest (GPU, glcore golden) | `SceneColorCopyGpu.CopyKeepsColourWhereTargetAlphaBelowOne` **red** | background (0.2, 0.4, 0.6, 0.5); a REFRACTION water (`refractionStrength` 0.2, colours with alpha 0) over it: centre pixel rgb equals the background rgb within 2/255 (today: halved) |
| core gtest (GPU) | `WaterLayeringGpu.TransparentUnderRefractingWaterStaysVisible` **red** | an alpha-blended red quad 0.5 under a refracting water over a grey floor: the quad's pixel has red above the floor-only reference by > 40/255 (today: equal) |
| core gtest (GPU) | `WaterLayeringGpu.TransparentUnderWaterNearerThanItsCentre` **red** | the red quad under a 40 m lake, nearer the camera than the lake's transform: its pixel is tinted by the water (blue channel raised vs. the quad drawn alone over the floor; today the quad is drawn over the water untinted) |
| core gtest (GPU) | `WaterLayeringGpu.OverlappingRefractingWatersSeeEachOther` **red** | a refracting pool 2 m above a refracting lake, camera above: a pixel inside the pool shows the lake's colour (differs from the pool over the bed alone by > 20/255); `colorRecopies == 1` |
| core gtest | `WaterLayering.Rank` | `waterRank` for eye above/below, beyond/eye side/straddling/outside, nested pool over lake (ranks lake 1, fish 0, pool -1), null box, self excluded |
| core gtest (GPU) | `WaterShadingGpu.ShadowEdgeFollowsSmoothSurface` **red** | per-pixel water (`normalStrength` 2, light normal bias 2) under a box's straight shadow edge, pinned time: the shadowed-pixel mask (ratio to the same scene with `receiveShadows` off < 0.8) differs from the `normalStrength` 0 mask in < 0.5% of the band's pixels (today: more) |
| core gtest (GPU) | `WaterShadingGpu.CausticsDarkInSurfaceShadow` **red** | CAUSTICS with REFRACTION, a box shadowing part of the surface: bed brightness inside the surface shadow with CAUSTICS on / off within 2% (today: > 5% brighter) |
| core gtest (GPU) | `WaterFeaturesGpu.FeaturesOffMatchesTheShaderBeforeThem` (existing) | still exact on glcore (no regression for default materials) |
| core gtest (GPU, golden) | `WaterReflectionGpu.PlanarReflectionMirrorsTheScene` | a red cube above flat water with REFLECTION + PLANAR_REFLECTION, distortion 0: the pixel at the cube's mirrored position is red-dominant; golden on glcore; stats `passes == 1` |
| core gtest (GPU) | `WaterReflectionGpu.FallbackWhenOffOrCapped` | Resolution OFF: same pixels as REFLECTION alone, `passes == 0`; three heights with max 2: the farthest level's water shows the environment, `fallbacks == 1`; two tiles at one level: one pass |
| core gtest (GPU) | `WaterReflectionGpu.OrthographicViewer` | the mirrored cube appears for an orthographic camera too |
| core gtest | `WaterWavesTest.GerstnerMatchesReferenceTable` | 64 samples of `waterGerstner` against a checked-in table, 1e-5 |
| core gtest | `WaterWavesTest.GerstnerInversion` | `waterSurfaceAt` finds rest points with `|rest + offset.xz - xz| < 1e-4 L` for steepness 0..0.9, random points |
| core gtest | `WaterWavesTest.GlslTwinPinned` (extended) | both GLSL functions' SHA-256 |
| core gtest (GPU) | `WaterWavesGpuTest.GerstnerMatchesCpu` | GPU displaced positions of a 32x32 rest grid; CPU `waterSurfaceAt` at each displaced XZ gives the GPU height within 1e-3 A + 1e-4 |
| core gtest | `SceneTime.*` | advances by the scaled delta; holds while time-paused; pinned shader time overrides; `Water::getTime(scene)` follows it |
| core gtest | `ShaderManifestTest.RoleDirective` | `@role water|underwater` parsed; unknown and duplicate are errors |
| core gtest | `WaterTest.ForkWithRoleIsWater` | a fork manifest (in-memory fork reader) with `@role water` makes `getSurfaces`, `readWaves` and `updateUnderwater` treat it as water, with its own defaults |
| editor gtest | `StreamSceneSettingsTest.PlanarReflections` | round trip of the six keys; defaults omitted; a scene without them loads with defaults |
| MCP | `test_water.py::test_waves_hold_while_paused` **red** | Play, Pause: two `sample_water` calls 30 frames apart give the same height and `water_time`; Resume: they differ |
| MCP | `test_water.py::test_waves_follow_time_scale` **red** | `Time.timeScale = 0` via `eval_lua`: height holds over 30 frames; 0.5: `water_time` advances half as fast as `Time.unscaledTime` |
| MCP | `test_water.py::test_edit_mode_animate_water` | stopped scene: `animate_water` true advances `water_time`, false holds it |
| MCP | `test_water.py::test_fork_keeps_buoyancy_queries_underwater` **red** | a fork written by hand from `read_engine_source` (both files, with `@role`) on the Water: Play, a Buoyancy box floats (y above -0.5 after 3 s), `Water.getHeight` is a number, the forked underwater pass switches on with the camera under the surface (today: nil, sinks, stays off) |
| MCP | `test_water.py::test_fork_shader_from_library` | `fork_shader source=library:water target=material`: files exist with `@role water`, material points at the fork, features kept; undo restores `library:water`; `check` clean |
| MCP | `test_water.py::test_planar_reflection_settings` | `set_scene_settings` six keys, undo/redo; `get_render_stats` passes 1 / 0 (off); screenshot artefact of the reflected cube (evidence) |
| MCP | `test_water.py::test_menu_entry_creates_water` (updated) | features now include `GERSTNER` |
| MCP | `test_checks.py` (or the water file) | `water-fork-without-role` and `planar-reflection-not-flat` fire and stay quiet when fixed |

Sanitizer run of `jasperengine-core-tests --gtest_filter=Water*:SceneTime*` once
(`SANITIZE=address,undefined`).

## 11. Acceptance criteria

1. Every test in section 10 passes; every **red** test is shown failing on the unchanged tree first.
2. `WaterFeaturesGpu.FeaturesOffMatchesTheShaderBeforeThem` is exact on glcore.
3. A scene with no `PLANAR_REFLECTION` material creates no reflection camera, target or pass (`get_render_stats`
   `planar_reflections.passes == 0`, no system camera entity beyond today's).
4. With Resolution Off, a `PLANAR_REFLECTION` water renders like REFLECTION alone and no pass runs.
5. Two water tiles at one height cost one pass; a third height over `maxPlanarReflections` falls back.
6. Pausing Play holds waves, ripples, caustics, flow and `Water.sample` heights together; `Time.timeScale` scales them.
7. A fork made with `fork_shader source=library:water` floats bodies, answers `Water.getHeight`, feeds a Water Volume's
   waves, and switches the underwater pass; so does a forked underwater pass.
8. The Inspector shows the new features, the Planar Reflections scene section with In Use, Fork "<Name>"... in the
   Shader picker, Animate water in the Scene view menu; each edit is one undo step.
9. Lua: `Water.getTime(scene)`, the six Scene properties and `PlanarReflectionResolution` work and appear in the stubs.
10. `docs/mcp/TOOLS.md` regenerated (`gen_tool_docs.py --check` clean), `tool_budget.py --check` passes, the
    shader library doc regenerated, `check_agent_docs.py` clean if the guide changed.
11. Screenshots (Scene view and Game view, isolated editor) of: planar reflection on/off, fish under a refracting lake
    before/after, Gerstner crests, attached to the PR.
12. `tools/check-changed` passes.

## 12. Files likely touched

Engine: `engine/core/subsystem/RenderSystem.{h,cpp}` (registries, reflection passes, layering, lazy copy, copy blend,
`applyPlanarReflection`, `sceneTime`), `engine/core/render/WaterLayering.h` (new), `engine/core/Scene.{h,cpp}`,
`engine/core/SceneSettings.h`, `engine/core/Engine.{h,cpp}`, `engine/core/shader/ShaderManifest.{h,cpp}`,
`engine/core/shader/ShaderUniforms.{h,cpp}`, `engine/core/shader/ShaderVariantCompiler.cpp` (require define, fork
include fallback), `engine/core/object/environment/Water.{h,cpp}`, `engine/core/subsystem/WaterSystem.{h,cpp}`,
`engine/core/util/WaterWaves.h`, `engine/core/script/binding/CoreClassesLua.cpp`,
`engine/core/script/binding/GameObjectClassesLua.cpp`.
Shaders: `engine/shaderlib/library/water.frag`, `water.glsl`, `water_vs.glsl`, `water.vert`, `underwater.frag`,
`engine/shaderlib/includes/planar_reflection.glsl` (new).
Editor: `editor/Project.cpp` (pauseEngineScene, edit-mode clock), `editor/render/SceneRender.{h,cpp}`,
`editor/window/SceneWindow.cpp`, `editor/window/InspectorWindow.cpp`, `editor/window/InspectorShaderLibrary.cpp`,
`editor/command/type/ForkShaderCmd.{h,cpp}`, `editor/command/type/CreateEntityCmd.cpp`,
`editor/util/WaterEditorUtils.cpp`, `editor/util/ProjectChecks.{h,cpp}`, `editor/Stream.cpp`,
`editor/StreamSchema.cpp`, `editor/Catalog.cpp`, `editor/mcp/McpValues.cpp`, `editor/mcp/tools/SceneTools.cpp`,
`editor/mcp/tools/ViewTools.cpp`, `editor/mcp/tools/AssetTools.cpp`, `editor/mcp/tools/ProfilerTools.cpp`,
`editor/mcp/tools/GameplayTools.cpp`.
Tests: `engine/tests/core/render/Water*GpuTest.cpp`, `SceneColorCopyGpuTest.cpp` (new),
`engine/tests/core/util/WaterWavesTest.cpp` + reference `.inl`, `engine/tests/core/render/WaterLayeringTest.cpp`
(new), `SceneTimeTest.cpp` (new), `ShaderManifestTest.cpp`, editor stream test, `tools/mcp/tests/test_water.py`,
golden images.
Docs: `docs/mcp/guide/shaders.md`, `physics.md`, `lua-api.md`, `scene-view.md`, `docs/mcp/checks.md`,
`docs/shaders/library.md` (generated), `docs/mcp/TOOLS.md` (generated), `tools/mcp/budget.json` only if measured.

## 13. Cross-feature dependencies

- `water-boats` depends on this spec: the water uniform block tails (6.2, 6.4: append after `sceneTime`), the
  `GERSTNER` twin (`WaterWaveParams`, `waterSurfaceAt`), `Water::isWaterShader`, the `waterMeshes` registry and
  `WaterLayering` (exclusion volumes should treat water draws through the same rank), and the scene clock. Branch it
  from `dc/water-reflections` after section 6.2 lands, or merge this first.
- `draw-perf` changes the transparent cull and sort ("cull before sort, sort by distance band and render state") in
  the same loop (`RenderSystem.cpp:16040-16076`, `RenderSystem.h:626`). Merge rule: `sortPriority`, then `waterRank`,
  then draw-perf's distance band and state keys. Whoever lands second rebases the comparator; both keep a test.
- `sprite-batching` restructures transparent submissions (sprites, UI quads): batches must carry a `waterRank` (one
  per batch, computed from its bounds) or be built after ranking.
- `ssr-probes` touches per-draw environment binding and probe slots (`applyEnvironment`, `:5085`); `applyPlanarReflection`
  sits beside it. Low conflict, same file region.
- `render-correctness` edits shader includes and light loops (implicit LOD in loops); the caustic-shadow change is in
  water.frag's light loop: keep its textures out of the loop.
- `mesh-memory` reworks `MeshComponent`/`Submesh`; this spec adds no MeshComponent field (registries live in
  RenderSystem) but reads submesh variants in `getSubmeshShaderInputs`.
- `export-pipeline` (shader keys for forks and script-created objects): forks of library shaders made by `fork_shader`
  and the new `planarreflection` require must be collected like other variants.
- `code-editor-git` adds scene-level default shader uniforms with inheritance (e3243c29b) in the custom uniform block
  code (`ShaderUniforms`): coordinate the engine-written name list (`time`, `resolution`, `sceneTime`).
- `platform-lifecycle`: hidden games update without drawing; the scene clock advances in the update block, so it
  stays right.
- `agent-authoring` edits the agent guide: one shaders-digest line here, within budget.
- `scripting` and `scene-integrity` touch `CoreClassesLua.cpp` / `Project.cpp`: textual merges only.

## 14. Deviations

Recorded during implementation (branch `dc/water-reflections`):

1. **Primary-view reflection passes have no camera entity.** The spec planned system camera entities as
   `createMirrorCamera` makes. They are `WaterReflectionPass` objects of the `RenderView` (as `MirrorView` is for the
   editor's views) for every view, drawn from `drawCameras` right before the scene camera's pass, with the next free
   terrain view; `update()` sets `hasMultipleCameras` while any can draw, so the shared MVPs are rewritten per pass as
   for other cameras. Reason: no scene entities to create, serialize-proof and reuse across views; the same code serves
   games and the editor.
2. **fork_shader `material` has no `path` (.material file) form.** A `.material` write is a project-scope undo step,
   which cannot join the scene's fork command in one step. Fork on a GameObject's submesh (its linked `.material`
   follows the editor's usual linking rules) instead.
3. **A water's PLANAR_REFLECTION draws inside REFLECTION** (as CAUSTICS inside REFRACTION): a water without REFLECTION
   registers no reflection pass, and the Inspector row says to turn Reflection on.
4. **`get_render_stats` keys:** `planar_reflections` {planes, passes, fallbacks, width, height} and
   `scene_color_copies` {copies, recopies} (the spec named `scene_inputs.color_recopies`; the tool had no
   `scene_inputs` object).
5. **`planar_reflection_layers` in MCP** is `"everything"`, `"nothing"`, a list of layer names (or indices), or the
   mask's number, as a camera's culling mask reads.
6. **The scene colour is still copied after the opaque queue** (before the hierarchy loop's UI, points, lines and
   screen fluids, as before), and the queue's first reader copies it again when the queue drew before it (what is
   under a water); re-copies (that one and the overlapping waters') happen only while nothing but the queue drew
   since the copy, so the HUD and the other overlays never refract. Counts toward the four copies.
7. **Agent guide:** no line added. The lead's decision keeps guidance in `docs/mcp/guide/shaders.md` (Water, Planar
   reflections, Layering, Forks); two eval cases (`bad_water_fork_role`, `guard_water_planar_reflection`) cover the
   new tool text. The live eval needs Orquester accounts and was not run; `test_grade.py` passes.
8. **Sorting with pixel-art S52, and only around the waters:** S52 orders the transparent queue by (Sorting Layer
   rank, Order in Layer, `sortPriority`, distance, order). The water order goes right after `sortPriority`, so
   S52's merge only puts its two keys in front (the comparator says so in a comment). It is (sort distance, rank,
   distance): a draw the waters layer, and each water, sorts at the farthest distance of its group of waters (waters
   whose areas overlap, `waterGroupDistances`), by rank inside it; every other draw at its own distance, so smoke
   over a far lake no longer sorts against a near window. Only flat waters (bounds at most 1% of their size plus
   5 cm tall, `isFlatWater`, as the `planar-reflection-not-flat` check) take part; a sloped river sorts by distance.
9. **Animate water** is written to the workspace only when off (default on), so workspace files of untouched scenes
   do not change.
10. **`Matrix4::obliqueNearClip` handles orthographic projections.** It took the clip-space corner with the
    perspective shortcut (w = -z), which puts an orthographic reflection camera's near plane in the wrong place (the
    orthographic water reflected nothing). It now takes the corner from the inverse projection and the projection's
    own w row (Lengyel's general form); perspective results are unchanged. Mirrors share the helper, so orthographic
    mirrors are fixed too.
11. **`fork_shader`'s text is shorter** so the core listing stays within its 99000 characters (`tools/mcp/budget.json`,
    pinned by `test_tool_budget.py`): the description names the library `source` and no longer explains `material`
    (a mesh source defaults to it; the result lists the submeshes), and some of the existing targets' wording is
    tighter. `source` and `submesh` carry no description.
12. **Newton's method stops at 6 steps, not 4** (`waterSurfaceAt`): it returns as soon as the residual is under
    (1e-5 L)^2, so 6 is only the cap; near-singular Jacobians at steep crests (steepness up to 0.95) take the
    slower fixed-point step, which the larger cap leaves room for. Not measured against 4. The C++ Gerstner twin
    matches the GPU within tolerance (`WaterWavesGpu.GerstnerMatchesCpu`: worst 4.19e-05 on glcore).
13. **Cooked scenes, format 1.4:** the six planar reflection settings are appended to the cooked scene settings
    after code-editor-git's 1.3 default shader values (`SceneSettingsCodec.cpp`, `FORMAT_MINOR` 4), so prebuilt
    exports keep them; a 1.3 record leaves them at their defaults.
14. **Forks made before the scene clock keep the frame clock:** library `water.vert` and `water_depth.vert` define
    `WATER_SCENE_CLOCK`, and `water_vs.glsl` runs the waves on the scene clock only with it; an older fork's copied
    `.vert` (without it) runs them on `frame.time`, as its copied `.frag` does.
15. **Each primary-view reflection pass has its own terrain cut** (the next free slots after the render-to-texture
    cameras', else the viewer's), so no cut's node buffer is updated twice in a frame. The passes are sized from the
    viewer's pass (fixed resolution, or the render scale with dynamic resolution), and the scene keeps its
    render-to-texture pipelines whatever the resolution, so turning it Off and on reloads nothing; the shared MVPs are
    rewritten per camera only while the camera sees a planar water from above.

## Follow-ups (2026-11-07)

Workstream `water-uniforms` (`docs/doriax-catchup/FOLLOWUPS.md`). Measured with the GLES3 reflection of every variant
the library's compile matrix builds (`WaterUniformBudgetSlow`), a fragment stage's vectors being the sum of its blocks
(sokol's GL backends upload each block as plain uniforms, against GL_MAX_FRAGMENT_UNIFORM_VECTORS: 224 guaranteed).

1. **Before:** 11 of the 290 matrix variants were over 224 vectors, 5 of them (30 compiles over 6 backends) also over the
   8 blocks a stage: lit per pixel with shadows at 16 lights 229 (no features) to 248 (every feature, Water Excluders
   and Wakes), and at 8 lights with fog 227 to 231 in 9 blocks (`u_fs_heightFog` the ninth). The plain water was over at
   16 lights before this program; PLANAR_REFLECTION added the ninth block. The built-in Lit in the same scenes: 199
   vectors at 16 lights with shadows, 222 with fog and the post colour; it has no GLES3 cap or fallback of its own.
2. **The lights' records form** (`includes/lights.glsl`, `LIGHTS_FS_RECORDS`, defined by `library/water.frag`): the
   fragment block holds the counts, the SH and two floats a pixel light (its scene light, its pass-dependent
   `spot_shadow.w`), 12 vectors at 16 lights instead of 68; the four vectors of each scene light are rows of
   `u_lightRecords` (RGBA32F, 128 lights a row), uploaded once a frame before the passes while a mesh drawing the form
   is loaded (`RenderSystem::syncLightRecords`). Rejected: capping the lights on GLES3 (changes what it draws there),
   moving the shadow blocks (124 vectors, the Lit's too, per camera pass: one upload a frame per image), packing the
   shadow blocks (12 vectors at most). Light Mode Tiled keeps its own form. A fork made before keeps the block form.
3. **One block for the environment and the planar reflection:** with both requires, `u_fs_environment` holds the planar
   reflection's matrix and parameters after its own (`includes/environment.glsl`, `planar_reflection.glsl`), resolved by
   the block's reflected size, so existing forks get it from the includes.
4. **After:** the matrix's most is 203 vectors (8 lights, fog, post colour, shadows, every feature) and 8 blocks; the
   worst scene at any capacity (16 lights, High shadow filters, fog, post colour, 2D lights, Excluders and Wakes, on a
   mesh, a lattice mesh or a terrain) 210; `shaders --library water --backend all`: 1740 compiled, 0 failed. No new
   property bit; shader cache v54.
5. **Not covered:** S57's capsule and contact shadows. Their `u_fs_lightOcclusion` is a ninth block with fog, and with
   capsules 67 vectors: the built-in Lit with capsule shadows, 2D lights and fog is at 9 blocks and 307 to 339 vectors
   too, so that is the S57 feature's GLES3 budget, not the water's.
6. **The built-in Lit's painted Terrain** ('Tpb', the lead's addition; measured by the pcss-gles workstream): its nine
   layers' `u_fs_terrainLayers` takes 45 vectors, so at 8 lights with shadows, the sky's and a probe's reflections,
   SSAO, the post colour and fog it took 235 (253 and 267 at 16 lights, without and with fog), in 8 blocks and 15 of
   16 textures. `mesh.frag` gives it the records form (not with a 2D shadow atlas, which takes the 16th texture
   then; it never takes the Tiled form): 211 at most (`TerrainUniformBudget`). Its golden
   (`TerrainLightRecordsGpu`) was captured with the block form and matches the records form exactly.
7. **Within the bind limits too** (with pcss-gles' rule: 16 samplers across the stages, 16 fragment textures, on
   every backend): a normal-mapped painted terrain with IBL, SSAO, the post colour, shadows and point or spot lights is
   at 16 textures and 16 samplers before its records. The records share PCSS's raw view's sampler where a variant has
   one (both nearest and clamped; texelFetch reads neither), so the directional-only terrain keeps PCSS.
   `ShaderVariantCompiler::compile` builds a variant still over
   them after PCSS's fallback without the spot lights' masks (`SPOT_MASKS_OFF`, a `spot-masks-left-out` log line, the
   editor's build log "no spot light masks"): a masked spot light lights that terrain with its plain cone. Chosen over
   SSAO (contact darkening under every object on the ground) and over the block form (235 to 267 vectors): masks are
   rare, and without one the terrain draws the same. Pinned by `TerrainUniformBudgetSlow` (normal-mapped, 8 and 16
   lights, High and Medium tiers, directional only or not). The records are uploaded only when one changed, and a
   scene past 128 lights reads the texture's second row (`WaterLightRecordsGpu`).
8. **Open follow-up (next):** S57's capsule and contact shadows on water and the painted terrain, and the built-in Lit
   with capsule shadows, 2D lights and fog on GLES3 (307 to 339 fragment vectors and a ninth block); the painted
   terrain with 2D lights (its 2D lights' block, 50 vectors) keeps the block form and is over 224 with them.
