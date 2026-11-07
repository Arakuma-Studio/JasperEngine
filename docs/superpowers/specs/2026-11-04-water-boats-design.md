# Water boats: water excluders and wakes for hulls on library:water (design)

Date 2026-11-04. Branch `dc/water-boats` (worktree `/var/lib/orquester/workspaces/jaspersito/dc-wt/water-boats`).
Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`. Depends on `water-reflections` (shader layout of
`library:water`, the fork marker, the water clock); see section 13.

Claims about today's code are inferred from reading it at `main` `b4f7816ea` unless marked otherwise; nothing here
was built or run.

## 1. Goals

1. A **Water Excluder** component (Unity HDRP's `WaterExcluder` name): a Box, a Sphere or the Convex Hull of the
   GameObject's mesh that keeps water out. Inside it:
   - the water surface is not drawn (an open boat shows its deck, not water through it), in every camera pass
     (main, reflection probes, the planar reflection camera of `water-reflections`), edit mode included;
   - a camera sees no underwater fade (`library:underwater` stays off), and `Water.isUnderwater` is false;
   - what floats does not float: cargo in a boat stays dry for both buoyancy paths (Rigidbody `buoyancy` over
     `library:water` meshes, and Buoyancy columns in Water Volumes), the boat's own excluder never drying the boat;
   - `Water.sample` / `sample_water` report the point as excluded and by which excluder.
2. A **Water Wake** component: a GameObject moving through water leaves foam (stern churn, bow spray, breaking bow
   waves) and Kelvin-style diverging and transverse waves behind it, drawn by `library:water` (foam and normals),
   fading over a scene-set time, kept on the water it was made on.
3. Zero cost when unused: a scene without Water Excluders and Water Wakes compiles, binds, uploads and computes
   exactly what it does today (same shader keys, same programs, no per-frame work beyond two array-size checks).
   Mobile and web budgets first: bounded per-pixel loops, a smaller wake map on mobile and web.
4. Humans and agents: Inspector UI with undo and Unity-like names, Scene view gizmos, Add Component entries,
   Lua API, MCP surfaces to drive (components through `add_component` / `set_properties`, scene settings through
   `set_scene_settings`) and to check (`sample_water` reports), docs and the agent guide.
5. Fix the class, not Doriax's instance: exclusions and wakes are engine inputs any water shader can opt into
   (`@require waterexclusion waterwake`), not a hard-coded path of one shader; wakes need no excluder.

## 2. Non-goals

- Wakes moving floating bodies (another boat rocking in a wake) or changing buoyancy: the wake map is visual
  (normals and foam). Physics reads the material's waves only, as today.
- Wakes displacing vertices: the wake waves bend the normal only (Doriax does the same); geometry stays the
  material's waves.
- Wakes carried by a river's flow (FLOW) or a Water Volume's `flow`: the wake map is anchored to the world.
- Wakes in 2D scenes, on non-`library:water` shaders that do not `@require waterwake`, and on secondary cameras far
  from the scene's main camera (the map follows the main camera).
- Holes in an opaque water's depth: `library:water` draws in the transparent queue and has `shadows=off`, so its
  depth program feeds no pass the hole would matter to (`RenderSystem.cpp` depth pre-pass draws opaque meshes only,
  `:16096-16100`). A user who forces an opaque blend on a water material keeps a full depth (documented).
- Non-convex exclusion volumes from a mesh (a catamaran's two hulls as one excluder): use two excluders (one per
  hull mesh) or Box/Sphere shapes. A concave "column" map was evaluated and rejected (section 5, D4).
- An exclusion shape editor with handles (the Scene view shows gizmos; the Inspector edits numbers; Fit to Mesh).
- Doriax items already in Jasper (`b40380c8c` Body3D buoyancy, `1bf9fd48b` buoyancy switch and depth): nothing to do.

## 3. Users and stories

- A human making a fishing game drops a rowing boat model on the lake, adds a Rigidbody and a Buoyancy, and sees
  water inside the boat. They click Add Component > Water Excluder: the default Convex Hull follows the boat
  model, the water inside is gone in the Scene view at once. In Play a crate thrown into the boat lies on its
  floor instead of bobbing up through it.
- The same human adds Water Wake to the boat: driving it leaves a white stern trail and a V of waves that fade
  after the Scene settings' Wake Fade (8 s). On a phone the wake map is 128 x 128 without them choosing anything.
- A human building an aquarium tunnel puts a Box excluder around the glass tube: walking through it, the camera
  sees no underwater fog and scripts reading `Water.isUnderwater(scene, cameraPosition)` get false.
- A submarine game: a Sphere excluder around the cockpit keeps the inside dry while the whole hull is under the
  surface.
- An agent asked "add a speedboat with a wake" calls `add_component` (WaterExcluder, WaterWake), plays,
  moves the boat with a script, and checks with `sample_water {points, report: true}`: the hull is `ready`, the
  emitter is `emitting` at its speed, the points behind the stern have `wake.foam > 0`, the point inside the boat is
  `excluded_by` the boat, and `sample_view_color` reads deck colour, not water, in the hole.
- An agent checking cost asks `sample_water {report: true}` in a scene without excluders: `waters[].exclusion` and
  `waters[].wake` are false (no variant bits), `wake_map.active` false.
- A shader author forking `library:water` (or writing their own ocean) keeps excluders and wakes by keeping
  `@require waterexclusion waterwake` and `#include "includes/water_interaction.glsl"`.

## 4. Findings

Jasper today (read at `b4f7816ea`):

- No exclusion of any kind. `engine/shaderlib/library/water.frag` (580 lines) has no discard path; its
  `main` ends at `:571-579` writing `g_finalColor`. `git grep -i exclu` over `engine/core/object/environment/`,
  `engine/core/subsystem/Water*` and `engine/shaderlib/` has no hits.
- `Water::applyBuoyancy` (`engine/core/object/environment/Water.cpp:222-273`) floats a dynamic Rigidbody whose
  centre of mass is in a buoyant water (`findSurface`, `:261`) with nothing that could keep it dry.
- `WaterSystem::applyBuoyancy` (`engine/core/subsystem/WaterSystem.cpp:195-345`) pushes up each submerged Buoyancy
  column found by `findVolume`; no dry test.
- `Water::updateUnderwater` (`Water.cpp:275-327`) switches `library:underwater` on whenever the main camera is
  under the surface of the water it is in (`:299-306`); a camera in a cabin below the waterline sees the fog.
- `Water::isUnderwater` (`Water.cpp:190-197`) and `WaterSystem::sample` (`WaterSystem.cpp:145-190`) answer from
  the surfaces only; `WaterSample` (`WaterSystem.h:22-36`) has no field for a dry point.
- No wakes: no wake path in `water.frag`, `Water.cpp`, `WaterSystem.cpp` (the only "wake" in the water code is a
  sleeping body, `WaterSystem.cpp:333`).
- What the engine can build on (inferred):
  - Engine-chosen variant bits with export collection and a missing-variant fallback: `MeshShaderEnvironment`
    (`RenderSystem.h:380-410`), `RenderSystem::getSubmeshShaderVariant` (`RenderSystem.cpp:3715-3960`),
    `ShaderVariantCollector::collectFacts` / `toEnvironment` (`editor/shader/ShaderVariantCollector.cpp:108-213`),
    `ShaderPool::getFallbackProperties` (`engine/core/pool/ShaderPool.cpp:1119-1170`; drops only bits whose
    resources are their own blocks and textures), MESH bits up to 35 used (`ShaderPool.cpp:647-652`).
  - Engine inputs a library shader declares: `@require` flags (`engine/core/shader/ShaderManifest.h:66-95`, parse
    `ShaderManifest.cpp:405-420`), bound per draw by reflection (`loadShaderLibraryBindings`,
    `RenderSystem.cpp:3983-4080`; `applyEnvironment`, `:5085-5118`, called at `:8056` and `:8778`).
  - Texture arrays: `TextureType::TEXTURE_ARRAY` (`engine/core/render/Render.h:358-363`), used by the terrain detail
    maps (`RenderSystem.cpp:5713`) and the 1-layer dummy `emptyArrayWhite` (`:1141`).
  - Dynamic textures: `TextureRender::createDynamicTexture(label, w, h, format, filter)` clamps
    (`engine/core/render/TextureRender.h:62-73`); a toroidal wake map needs REPEAT.
  - Mesh geometry without physics: `MeshTriangles::meshSourceEntities` / `collectMeshSourceViews` /
    `isGeometryLoading` (`engine/core/util/MeshTriangles.h:31-63`). Jolt's `ConvexHullBuilder`
    (`engine/libs/joltphysics/Jolt/Geometry/ConvexHullBuilder.h`) exists under `JASPERENGINE_PHYSICS_3D`
    (`engine/CMakeLists.txt:17`, default ON).
  - A golden that pins the water with every feature off: `WaterFeaturesGpu.FeaturesOffMatchesTheShaderBeforeThem`
    (`engine/tests/core/render/WaterFeaturesGpuTest.cpp:121`, `engine/tests/core/golden/water_features_off.png`),
    and the built-in Lit zero-cost hash list (`engine/tests/core/shader/ShaderZeroCostTest.cpp`).

Doriax references (read-only, `/var/lib/orquester/workspaces/jaspersito/dc-work/analysis/doriax`):

| Commit | What | Here |
|---|---|---|
| `1ecdb8b41` Added Water Exclusion volumes | Box/Sphere/Hull component, `isExcluded` loop always compiled into `water.frag` (8 volumes, hull maps in a texture array), `findUnderwater` dry test, `isKeptDry` in buoyancy, convex hull by Jolt with a 128 x 128 map | Specified (D1-D6), as an opt-in engine variant instead of an always-compiled loop |
| `755e7d723` Added water wakes for moving exclusions | Per-water 256 x 256 CPU map around the camera, Kelvin wake stamping, 5 texture reads | Specified (D7-D10), as a separate component, one scene map, 3 reads, 128 on mobile/web |
| `bb82c1d3e` Left no wake foam where there is no wake | `smoothstep(0, 0.03, wake.r)` gate on wake foam | Taken (7.3, test `WaterWakeShader.NoFoamWithoutWake`) |
| `db794e641` Kept wakes on the water they were made on | History samples record their water | Taken, generalised to water bodies (D9, test `WaterWakeMap.StaysOnItsWater`) |
| `c5b7a99dc` Uploaded wake maps once per frame in draw | sokol's one update per image per frame | Taken (D10, test `WaterWakeMap.OneUploadPerFrame`) |
| `a0b400b4e` Kept hidden water exclusions out of the water and its wakes | `visibleInHierarchy` gate | Taken, with Unity's enabled/active rules added (7.1, tests `WaterExcluder.InactiveHiddenOrDisabledKeepNothingOut`, `WaterWake.HiddenLeavesNoTrail`) |
| `b40380c8c`, `1bf9fd48b` | Buoyancy on Body3D, buoyancy switch and depth | Already in Jasper (dossier: skip) |

The dossier's skeptic noted a stencil workaround (a `library:stencil_mask` mesh plus a stencil test on the water);
it covers only the drawn surface in the main view. D2 explains the rejection.

## 5. Product decisions

- **D1. Two components with Unity's names: Water Excluder and Water Wake.** Unity HDRP calls the volume that
  removes water from a boat a Water Excluder; Doriax's "Water Exclusion" is not a Unity term. Wakes are a separate
  component (HDRP splits the same job into a Bow Wave deformer and a Foam Generator): a swimmer, a duck or a jet
  ski wants a wake without a dry volume, and a dry tunnel wants no wake. A boat gets both. Doriax put the wake
  strengths on the exclusion.
- **D2. Analytic exclusion in the water's fragment stage, not stencil.** A stencil mask works only in the pass
  that draws the mask before the water, depends on transparent sorting, and solves neither the camera fade nor
  buoyancy nor queries. The analytic test (point in box, sphere or hull map) runs in every pass that draws the
  water, with the same definition the CPU uses for physics and queries.
- **D3. Engine variant bits the shader opts into.** Two MESH property bits, `'Wex'` (`USE_WATER_EXCLUSION`) and
  `'Wwk'` (`USE_WATER_WAKE`), set only on submeshes whose shader manifest declares
  `@require waterexclusion` / `@require waterwake` (`library:water` declares both; forks copy the line), and only
  while the scene has a Water Excluder / Water Wake component (counted enabled or not, as lights are, so enabling
  one in Play needs no variant switch). Doriax compiles its loop and binds its textures into every water draw
  forever; here a scene without them draws today's program. Both bits are droppable by the missing-variant
  fallback (their resources are one block and two textures of their own), so a build that lacks a variant draws
  the water without holes or wakes instead of nothing.
- **D4. Convex Hull is the default shape, built from the GameObject's own mesh.** Adding the component to a boat
  should just work. The hull is the convex hull of `MeshTriangles::meshSourceEntities` (the entity's mesh, or a
  multi-node model's mesh nodes: not arbitrary children, so cargo, oars and passengers parented to the boat do
  not inflate it), in the excluder's local space. Convex is what an open boat needs: its top is the gunwale, so
  water at the waterline inside the boat is excluded; a per-column min/max of the mesh would end at the inner
  floor, which sits below the outside waterline in a rowing boat, and leave water inside. Without
  `JASPERENGINE_PHYSICS_3D` (no Jolt) the hull falls back to the meshes' local bounds box with a warning.
- **D5. At most 8 excluders per water draw, chosen per draw by the water's bounds and the camera's frustum,
  nearest first.** Doriax picks by the water's bounds only, so twenty boats on one ocean take the 8 slots by
  distance even when off screen. Culling by the drawing camera's frustum first means the cap is reached only with
  more than 8 excluders in view over one water mesh; then the farthest lose their holes and a warning is logged
  once per scene. The per-pixel loop runs only over the draw's count.
- **D6. One definition of "dry", everywhere.** A point is dry when an active excluder contains it: enabled
  component, GameObject active in hierarchy and visible (`Transform::visible`, which propagates to children;
  Doriax `a0b400b4e`). Dry means: not underwater (`Water.isUnderwater`, the underwater pass), not floated (both
  buoyancy paths), `WaterSample.inWater` false with `excluder` set. For buoyancy, an excluder is ignored for the
  body that owns it: the owner is the nearest ancestor-or-self of the excluder's GameObject with a
  `RigidbodyComponent`. (Doriax walks the excluder's ancestors looking for the body, which is the same rule
  without the "nearest" stop, so cargo with its own Rigidbody parented under the boat would wrongly count as the
  owner of a deeper excluder; the stop fixes that.)
- **D7. One wake map per scene, around the main camera, configured in Scene settings.** Doriax keeps a 256 x 256
  map per water; spline rivers and ocean tiles are several water meshes, so wakes would cut at every tile border,
  and N waters cost N maps. Here one toroidal RGBA8 map per scene (R foam, G wave crest, B water-body tag), placed
  ahead of the main camera, texel-snapped, moved without copying (only the rows and columns it uncovers are
  cleared). Scene settings, group "Water Wakes": Wake Resolution (Auto: 256 on desktop, 128 on mobile and web;
  Low 128, Medium 256, High 512), Wake Area (64 m), Wake Fade (8 s). The map exists only while a Water Wake is
  emitting or a wake is still fading.
- **D8. The wake model is Doriax's** (`755e7d723`): bow positions recorded along the track, diverging crests at
  the Kelvin half angle narrowing as 1/Froude past 0.5 (Rabaud and Moisy 2013), an inner crest, transverse
  waves between, breaking bow waves past hull speed, stern churn and hull spray, foam decaying by
  `exp(-2 dt / fade)`. It is physically grounded, cheap and already tuned; we take it (MIT, same copyright header)
  with the follow-up fixes and with Jasper's clocks: wakes advance with the scene's update delta (scaled by the
  time scale, zero while paused) and only in a running scene.
- **D9. Wakes stay on their water body.** A texel remembers the body that wrote it (B channel, 8-bit tag); a water
  draw shows only texels of its own body. A body is: for a spline river or lake, its spline GameObject (all its
  chunks); for `library:water` meshes, the set of meshes at the same rest level (within 1 cm) whose XZ bounds touch
  or overlap (ocean tiles are one body; a pool over a lake, or two ponds side by side, are not). Doriax's fix
  (`db794e641`) tags history samples per water entity, which cuts wakes at tile borders.
- **D10. CPU simulates, the GPU reads three taps.** Doriax reads the map 5 times per water pixel; forward
  differences make it 3 (centre, +X, +Z). The map uploads at most once per frame, in `RenderSystem::draw`
  (`c5b7a99dc`), and not at all while nothing changed.
- **D11. No new MCP tool.** Components are reflected (add/get/set like every component); scene settings join
  `get_scene_settings` / `set_scene_settings`; checking extends `sample_water` with `excluded_by`, `wake` and an
  optional `report`. The tool budget grows by one tool's description edit only.
- **D12. No format version bump.** Two new components and three scene settings with defaults; old projects load
  unchanged and draw unchanged (no component, no bit).

## 6. Design

### 6.1 Engine data

`engine/core/component/WaterExcluderComponent.h`:

```cpp
enum class WaterExcluderShape{ BOX, SPHERE, CONVEX_HULL };

// Keeps water out (a boat, a cabin, a dry tunnel): no water surface drawn inside it, no
// underwater fade for a camera inside, nothing floated inside but the GameObject's own
// Rigidbody (docs/superpowers/specs/2026-11-04-water-boats-design.md)
struct WaterExcluderComponent{
    bool enabled = true;
    WaterExcluderShape shape = WaterExcluderShape::CONVEX_HULL;
    Vector3 center = Vector3(0.0f, 0.0f, 0.0f);   // local, Box and Sphere
    Vector3 size = Vector3(1.0f, 1.0f, 1.0f);     // local; a Sphere is the ellipsoid in this box
};
```

`engine/core/component/WaterWakeComponent.h`:

```cpp
// Foam and waves left on library:water while the GameObject moves through its surface
struct WaterWakeComponent{
    bool enabled = true;
    float foam = 1.0f;     // 0..2: stern churn, hull spray, breaking bow waves
    float waves = 1.0f;    // 0..2: the diverging and transverse waves
    float width = 0.0f;    // local X extent at the waterline, metres; 0: from the Water Excluder or the mesh
    float length = 0.0f;   // local Z extent, metres; 0: as width
};
```

Both plain data (no runtime members): runtime state lives in the system (6.2), keyed by entity, so cloning,
serialization and reflection stay generic.

`engine/core/SceneSettings.h`, after the SSR block:

```cpp
// the wake map of Water Wakes (one per scene, around the main camera)
WaterWakeResolution waterWakeResolution = WaterWakeResolution::AUTO; // AUTO, LOW 128, MEDIUM 256, HIGH 512
float waterWakeArea = 64.0f;   // metres across the map
float waterWakeFade = 8.0f;    // seconds a wake takes to fade (an eighth of the foam left)
```

`Scene` gets getters/setters for the three (as for `sceneColorScale`), clamped on apply: area 8..1024, fade
0.1..120. `WaterWakeResolution::AUTO` resolves to MEDIUM on desktop and LOW on Android, iOS and web
(`WaterInteractionSystem::getWakeMapSize`).

### 6.2 Engine runtime: `WaterInteractionSystem`

New `engine/core/subsystem/WaterInteractionSystem.{h,cpp}`, registered in `Scene::init` right after
`TrailSystem` (after Render, whose update makes this frame's world transforms; `Scene.cpp:72-74`). It declares
hook types `WaterExcluderComponent`, `WaterWakeComponent` and `MeshComponent` (to drop cached hulls).

Pure helpers it uses, testable without a scene:

- `engine/core/util/WaterExclusion.{h,cpp}`:
  - `struct WaterHull{ std::vector<Vector4> planes; std::vector<Vector3> points; std::vector<std::pair<uint16_t,uint16_t>> edges; AABB bounds; size_t sourceVertices; }`
  - `enum class WaterHullStatus{ NONE, PENDING, READY, NO_MESH, FLAT, NO_PHYSICS }` (NONE: not a hull shape).
  - `WaterHullStatus buildWaterHull(const std::vector<Vector3>& localVertices, WaterHull& hull)`: vertices
    quantised to a 1/1024 grid of their bounds and deduplicated (bounds a 100k-vertex model's input to the
    occupied cells), then `JPH::ConvexHullBuilder::Initialize(128 max vertices, tolerance 1e-4 x bounds diagonal)`
    (Doriax used `INT_MAX`; 128 vertices keeps the hull under 252 faces and the map build bounded); planes are the
    non-removed faces (outward normal, distance), edges from the face loops (for the gizmo). Without
    `JASPERENGINE_PHYSICS_3D`: returns `NO_PHYSICS` and the caller uses the bounds box.
  - `void buildWaterHullMap(const WaterHull&, std::vector<uint8_t>& rgba)`: Doriax's `buildWaterExclusionMap`,
    `WATER_EXCLUDER_HULL_MAP_SIZE` = 128 square, area = hull bounds plus two texels each side in XZ; per texel R the
    hull's bottom and G its top as 0..1 of the hull height (an empty column R 255, G 0), B 0.5 + the signed distance
    outside the vertical walls / (2 x 4 texels) (inside below 0.5), A 255; linear between texels so bilinear filtering
    keeps edges straight.
  - `Matrix4 getWaterExcluderUnitSpace(shape, center, size, const WaterHull*, const Matrix4& modelMatrix)`: world to
    0..1 across the box (Box, Sphere) or the hull map's area (Convex Hull).
  - `bool isInsideWaterExcluder(shape, center, size, const WaterHull*, const Matrix4& modelMatrix, const Vector3& p)`:
    Box/Sphere through the unit space (sphere: `|q - 0.5|^2 <= 0.25`); Convex Hull by its exact planes in local
    space. Zero scale (determinant 0) or a null box: false.
- `engine/core/util/WaterWakeMap.{h,cpp}`: the toroidal map and Doriax's stamping (`stampWake`, `getWakeHullSpeed`,
  `getWakeAngle`, `getWakeFootprint`, `stampWaterWake`, `moveWakeMap`), rewritten over a `WakeMap` struct
  (`size`, `texel`, `originX`, `originZ`, `std::vector<float> foam, crest`, `std::vector<uint8_t> tags`,
  `std::vector<uint8_t> rgba`). `stampWake` writes a texel's tag when it raises either value. History samples carry
  their body tag; `stampWaterWake` stamps only the samples whose tag is the body being stamped and skips a pair
  whose tags differ.

System state:

```cpp
struct ExcluderFrame{          // gathered once a frame (update), read by RenderSystem::draw and the queries
    Entity entity;
    WaterExcluderShape shape;  // CONVEX_HULL with a NO_PHYSICS hull is drawn as BOX of its bounds
    Matrix4 worldToUnit;       // three rows go to the shader
    AABB worldBounds;
    int hullLayer = -1;        // in the hull map array, -1 for Box/Sphere
};
std::vector<ExcluderFrame> frameExcluders;
std::unordered_map<Entity, HullCache> hulls;   // signature, status, WaterHull, map, layer
uint64_t hullMapsVersion = 0;                   // bumped when a map or the layer list changes
std::unordered_map<Entity, WakeEmitter> emitters; // smoothed velocity, history deque, stern, time, body
WakeMap wakeMap; bool wakeActive = false; bool wakeDirty = false;
std::vector<WaterBody> frameBodies;             // the frame's water bodies and their tags (D9)
```

`update(double dt)`:

1. If both component arrays are empty: if anything is cached, free it (hulls, emitters, map, `frameExcluders`)
   and bump `hullMapsVersion` once; return. This is the whole cost of an unused feature (two size checks).
2. Excluders (every frame, edit mode included, paused or not): for each component, skip it unless active (D6). A
   CONVEX_HULL computes its signature (Doriax's `getMeshSignature`: source entities, their mesh-to-excluder
   matrices, each `MeshSourceView`'s buffer data pointer, offset and count, the meshes' `verticesAABB`); a changed
   signature rebuilds the hull and its map (`PENDING` while `MeshTriangles::isGeometryLoading`). Box and Sphere
   need no cache. Fill `frameExcluders` (world matrix from `Transform::modelMatrix`). Hull layers are assigned in
   component order to READY hulls; a layer list change bumps `hullMapsVersion`.
3. Wakes, only when `Engine::isSceneRunning(scene)` and `dt > 0`; otherwise, if the scene is not running and
   state exists, clear it (this is Doriax's `clearWaterWakes` at Stop, without an editor call):
   - water bodies of the frame (D9): `Water::getSurfaces` (once) grouped by level and touching bounds, plus the
     spline owners that `SplineSystem::sampleWaterSurface` reports under emitters; tags are stable per body while
     the map holds texels of it (a table; reset when the map fully fades), at most 255 (more: no wake on the extra
     bodies, warned once);
   - each active emitter (D6 rules, plus the component's own `enabled`): footprint (width/length, else the Water
     Excluder on the same GameObject (its box, sphere box or hull bounds), else the bounds of its mesh source
     entities, else 1 x 1 m; times the world scale), centre, velocity smoothed with tau 0.15 s, teleport reset
     (move > max(5 m, 4 x speed x dt + 1 m)), crossing test (the drawn water surface at the centre lies between the
     footprint's world bottom and top; the body found there), history push of the bow (spacing
     max(0.25 m, 0.1 x half length), cap 256 samples / 60 s), all from Doriax `updateWaterWakes`;
   - the map: size from the scene setting, texel = area / size; placed around the main camera's eye moved ahead along
     its horizontal forward by 0.25 x area; a changed size or texel reallocates, a moved origin clears only the
     uncovered rows and columns, a jump of a full map clears all; foam decays, crests are cleared and restamped from
     every emitter's history (an emitter that left the water still spreads what it made there); the RGBA bytes are
     rebuilt (R foam, G crest, B tag, A 255); `wakeActive` = any value above 0.004 or an emitter crossing;
     `wakeDirty` = true.
4. Counters for tests: `getHullBuildCount()`, `getWakeStepCount()`.

Queries (main thread, any time; usable in edit mode):

- `Entity findExcluder(const Vector3& point, Entity floatingBody = NULL_ENTITY) const`: the first active excluder
  containing the point, ignoring those owned by `floatingBody` (D6). Uses `frameExcluders` bounds as a broad phase
  and `isInsideWaterExcluder` from the Transform's current world pose (physics moves bodies between frames).
  Without a frame gathered yet (first frame), it gathers.
- `static Entity getExcluderOwner(Scene*, Entity excluder)`: nearest ancestor-or-self with a
  `RigidbodyComponent`, or `NULL_ENTITY`.
- `bool sampleWake(float x, float z, Entity waterBody, float& foam, float& crest) const`: the CPU map at a point, for
  `Water.sample` and `sample_water`.
- `WaterHullInfo getHullInfo(Entity) const` (status, faces, source vertices, local bounds), `getEmitterInfo(Entity)`
  (emitting, speed, body), `getWakeMapInfo()` (active, size, area, origin), for the Inspector, Lua and MCP.
- `uint8_t getBodyTag(Entity waterMesh) const` for draws (0: none).

### 6.3 Shaders

`engine/core/shader/ShaderManifest.h`: `ShaderRequire_WaterExclusion = 1 << 12`, `ShaderRequire_WaterWake =
1 << 13`, parsed as `waterexclusion` / `waterwake` (`ShaderManifest.cpp:405-420`, the unknown-input message and
`getRequireNames` updated). They add no `SURFACE_*` define (`getRequireDefines` ignores them); they imply
`position`; on a non-mesh shader they are a manifest error.

`engine/core/pool/ShaderPool.cpp`: MESH bit 36 `'Wex'` "Water Exclusion", bit 37 `'Wwk'` "Water Wake"
(`getShaderPropertyName`, `getShaderPropertyCount(MESH)` 36 -> 38), `getWaterExclusionProperty()`,
`getWaterWakeProperty()`; `getFallbackProperties` mesh order becomes `{37, 36, 21, 12, ...}` (wake given up first,
then exclusion). `engine/core/shader/ShaderVariantCompiler.cpp` `addMeshPropertyDefinitions`: `USE_WATER_EXCLUSION`,
`USE_WATER_WAKE`. Bit numbers are this branch's claim; if another catch-up branch merges MESH bits first, take
the next free ones (the short names are the identity used in docs and tests).

New `engine/shaderlib/includes/water_interaction.glsl`:

```glsl
#if defined(USE_WATER_EXCLUSION) || defined(USE_WATER_WAKE)
#define WATER_EXCLUDERS_PER_DRAW 8
uniform u_fs_waterInteraction {
    vec4 exclusion;                                   // x: excluders in the arrays (0..8)
    vec4 exclusionRows[WATER_EXCLUDERS_PER_DRAW * 3]; // world to 0..1 across each volume, three rows
    vec4 exclusionShapes[WATER_EXCLUDERS_PER_DRAW];   // x: 0 box, 1 sphere, 2 hull; y: hull map layer
    vec4 wakeArea;     // xy: world XZ of the map's corner; z: 1 / its world size; w: 1 while it holds a wake
    vec4 wakeParams;   // x: slope per unit of crest per texel; y: 1 / texels across; z: this water's tag / 255
} waterInteraction;
#endif
#ifdef USE_WATER_EXCLUSION
uniform texture2DArray u_waterExclusionMaps;
uniform sampler u_waterExclusionMaps_smp;
// inside one of the draw's excluders (textureLod only: safe in any control flow)
bool waterExcluded(vec3 worldPosition);
#endif
#ifdef USE_WATER_WAKE
uniform texture2D u_waterWakeTexture;
uniform sampler u_waterWake_smp;
// x: wake foam 0..1, yz: the wake waves' slope along world X and Z (0 off the map, on another water, or idle)
vec3 waterWake(vec2 worldXZ);
#endif
```

One block for both bits keeps the uniform block count at +1 (sokol's 16 bind slots) and has one fixed layout, so
dropping either bit by the fallback leaves a valid program. `waterExcluded` is Doriax's `isExcluded` with the
lights' loop form (`for (int i = 0; i < 8; i++){ if (i >= count) break; ... }`). `waterWake`: returns 0 when
`wakeArea.w < 0.5`; fades over the outer 8% of the map; samples `textureLod` at `xz * wakeArea.z` (REPEAT, the map
is toroidal), +X and +Z one texel over for forward differences; zero where `abs(b - tag) >= 0.5 / 255`.

`engine/shaderlib/library/water.frag` (additive, small, so it merges over `water-reflections`):

- manifest: `// @require waterexclusion waterwake`; the `@description` and the header comment get one sentence each
  (Water Excluders cut the surface, Water Wakes add foam and waves; engine variants, nothing without them).
- `#include "includes/water_interaction.glsl"` after `library/water.glsl`.
- After `uv1`/`uv2`: `#ifdef USE_WATER_WAKE vec3 wake = waterWake(xz); #endif`. The ripple strength is calmed by
  `1 - 0.6 * clamp(wake.x * 3, 0, 1)`; the normal bends by `-(wake.y, 0, wake.z)`, sign flipped from below as the
  ripples.
- After the base foam: `foam = max(foam, smoothstep(foamNoise - 0.15, foamNoise + 0.15, wake.x * 1.5) *
  min(0.35 + wake.x * 2.0, 1.0) * smoothstep(0.0, 0.03, wake.x));` (`bb82c1d3e`'s gate).
- Last statements before the output write: `#ifdef USE_WATER_EXCLUSION if (waterExcluded(v_position)) discard;
  #endif`. Discarding after every implicit-derivative read (`fwidth`, the `texture()` taps) keeps neighbours'
  derivatives defined on every backend.

Texture budget: the worst `library:water` variant today binds about 10 fragment textures (ripples, noise, scene
depth, scene colour, environment, spot mask atlas, shadow atlas, point atlas, raw atlas, post colour LUT;
inferred from the includes). This adds 2 (+1 planar reflection from `water-reflections`), under GLES3's and
WebGL2's 16; the library compile matrix checks it (6.8, test `ShaderLibraryMatrix` water).

### 6.4 RenderSystem

`engine/core/subsystem/RenderSystem.{h,cpp}`:

- `MeshShaderEnvironment`: `bool waterExclusion = false; bool waterWake = false;` filled in
  `getMeshShaderEnvironment` from `hasWaterExcluders` / `hasWaterWakes` (component arrays non-empty, enabled or
  not).
- `getSubmeshShaderVariant`: after the post-colour bit, `if (manifestRequire & ShaderRequire_WaterExclusion &&
  environment.waterExclusion) properties |= getWaterExclusionProperty()` and the same for the wake. Nothing for
  shaders that do not require them (built-in Lit unchanged).
- `update`: recompute the two flags; on a change call new `needReloadWaterVariants()`: marks for a variant reload
  only the meshes with a submesh (or extra pass) whose manifest's `getPossibleRequire()` has either flag.
  `onComponentAdded` / `onComponentRemoved` for the two component types set the flags at once (as lights do,
  `RenderSystem.cpp:16926-16967`).
- `ShaderLibraryBindings`: `slotFSWaterInteraction`, `slotWaterExclusionMaps`, `slotWaterWake`, read in
  `loadShaderLibraryBindings` (dummies `emptyArrayWhite` and `emptyBlack` until a draw binds).
- `applyWaterInteraction(ObjectRender&, const ShaderLibraryBindings&, const MeshComponent&, const Transform&,
  const CameraComponent&, const Vector3& eye)`, called next to `applyEnvironment` at `:8056` and `:8778`: returns
  at once when the variant has neither slot. Excluders: from `frameExcluders`, those whose world bounds overlap the
  water mesh's world bounds (Y padded by `max(1, 3 x waveHeight)` of the material) and intersect the drawing
  camera's `frustumPlanes`, skipping hulls without a layer in the current GPU array, sorted by squared distance to
  `eye`, first 8 (D5). Wake: `wakeArea`, `wakeParams` from the system's map, `z` the tag of the mesh's body.
  Uploads `water_interaction_t` (C++ twin of the block, `static_assert` on 35 x 16 bytes) and binds
  `hullMaps`/`wakeTexture` (or the dummies).
- `draw()` start (one place, once per frame): when `hullMapsVersion` changed, destroy and recreate `hullMaps`
  (`createTexture(..., TextureType::TEXTURE_ARRAY, layers, ..., LINEAR, CLAMP_TO_EDGE)`; zero layers: none); when
  the wake map is needed, create or resize `wakeTexture` (dynamic RGBA8, LINEAR, REPEAT) and, if `wakeDirty`,
  `updateTexture` once and clear `wakeDirty` on success (a refused upload past the staging buffer retries next
  frame). `destroy()` frees both.
- `engine/core/render/TextureRender.{h,cpp}` and `engine/renders/sokol/SokolTexture.{h,cpp}`:
  `createDynamicTexture(label, w, h, format, filter, TextureWrap wrap = CLAMP_TO_EDGE)` (the wake map needs
  REPEAT; existing callers unchanged).

### 6.5 Water, WaterSystem and buoyancy

- `WaterSample` (`WaterSystem.h`): `Entity excluder = NULL_ENTITY; float wakeFoam = 0; float wakeHeight = 0;` with
  `getExcluder()`, `isExcluded()`, `getWakeFoam()`, `getWakeHeight()` (height in metres, crest x 0.4).
- `WaterSystem::sample`: after finding the water, `excluder = findExcluder(point)`; when set, `inWater = false`
  (surface, normal and volume stay, so a script still knows the waterline); wake values from `sampleWake` at the
  point for the body found.
- `WaterSystem::applyBuoyancy`: a column whose bottom point is dry for this body (`findExcluder(bottom, entity)`)
  is skipped; a body with every column dry gets nothing (and is not woken).
- `Water::applyBuoyancy`: the centre of mass dry for this body: skip.
- `Water::isUnderwater`: false when dry.
- `Water::updateUnderwater`: a camera eye that is dry is not under (the pass switches off, no hysteresis).
- Each call site fetches `scene->findSystem<WaterInteractionSystem>()` once per step; null or no excluders: no
  extra work.

### 6.6 Editor

- Components: `editor/Catalog.h` (`ComponentType::WaterExcluderComponent`, `WaterWakeComponent`),
  `engine/core/registry/EntityRegistry.{h,cpp}` (register, insertion order), `EntityCloner.cpp` (copy),
  `editor/reflect/components/GameplayReflect.cpp` (field descs, codecs, `kWaterExcluderDesc` key `waterexcluder`
  "Water Excluder" icon `ICON_FA_SHIP`, `kWaterWakeDesc` key `waterwake` "Water Wake" icon `ICON_FA_WATER`),
  `editor/reflect/Enums.{h,cpp}` (`kWaterExcluderShape`: box, sphere, convex_hull; `kWaterWakeResolution`),
  `editor/Stream.h` / `StreamComponents.cpp` (encode every key, decode with defaults), `editor/StreamSchema.cpp`
  (known keys, the three scene settings), `editor/Factory.{h,cpp}` (generated C++ for exports),
  `engine/core/serialization/RuntimeFields.gen.inc` (regenerated), `engine/core/animation/PropertyBindings.cpp`
  (enabled rows), `engine/core/JasperEngine.h` (includes), `editor/tests/reflect/catalog_snapshot.json`.
- Add Component (`editor/window/dialog/ComponentAddDialog.cpp`), category with Water Volume:
  "Water Excluder: keeps water out of a boat or a dry space: no surface inside, no underwater fade, cargo stays
  dry"; "Water Wake: foam and waves behind it while it moves through water".
- Inspector (`editor/window/InspectorPhysics.cpp`, mapped in `InspectorDefault.cpp`):
  - Water Excluder: Shape (Box, Sphere, Convex Hull); Center and Size for Box and Sphere; button "Fit to Mesh"
    (Box/Sphere: center and size from the mesh source entities' local bounds, one `MultiPropertyCmd`, disabled
    without a mesh); a status line for Convex Hull: "Convex hull: 38 faces from 2,410 vertices" / "Waiting for the
    model to load" / warnings "No mesh on this GameObject: it keeps nothing out. Choose Box or Sphere." / "The mesh
    is flat: no hull. Choose Box." / "This build has no 3D physics: the hull is the mesh's bounds box." Warnings
    when no water in the scene requires exclusions, and when a child of this GameObject has a dynamic Rigidbody
    with buoyancy while this excluder has no Rigidbody owner ("everything inside it stays dry, that body too:
    put the Water Excluder on the Rigidbody's GameObject or under it").
  - Water Wake: Foam, Waves (sliders 0..2), Width, Length (0 shows "Auto (2.4 m)" with the resolved value); in
    Play a status line "Emitting at 4.1 m/s on Lake" / "Not on water"; warning when the scene has no water that
    requires wakes.
  - All rows through `propertyRow` (undoable `PropertyCmd`), multi-selection as other components.
- Scene settings (`editor/window/InspectorWindow.cpp`, 3D scenes, after "Reflections (SSR)"): separator "Water
  Wakes", rows Wake Resolution (combo), Wake Area (m), Wake Fade (s), through `ScenePropertyCmd` (undo),
  `editor/Catalog` scene property defaults.
- Scene view (`editor/render/SceneRender3D.{h,cpp}`): `createOrUpdateWaterExcluderLines` (box edges, sphere's three
  circles, hull edges; a distinct colour, brighter when selected), drawn as Water Volume lines are
  (`SceneRender3D.cpp:1534`); a selected Water Wake draws its footprint ellipse at the GameObject's height.
- `editor/util/WaterEditorUtils.{h,cpp}`: helpers for the warnings (scene has a water requiring exclusions/wakes,
  footprint resolution for "Auto").
- Shader variant collection (`editor/shader/ShaderVariantCollector.cpp`): `FACT_WATER_EXCLUDERS`,
  `FACT_WATER_WAKES` in `collectFacts` (component present, enabled or not), mapped in `toEnvironment`, two more
  bits in `environmentBits`; prefab facts follow (a prefab with an excluder makes every scene that can spawn it
  ship the 'Wex' variants). `editor/Project.cpp:3726` probe environment unchanged (it reads the RenderSystem's).
- Compile matrix (`editor/shader/ShaderLibraryMatrix.cpp`): for mesh shaders that `@require` either input, a
  variant "water exclusion + wake" with every feature on, pixel lights, shadows, post colour and both bits: the
  16-texture budget check covers it on every backend.

### 6.7 Lua

`engine/core/script/binding/GameObjectClassesLua.cpp` (next to `WaterVolume`, `:1133-1150`), handles in
`engine/core/object/environment/WaterExcluder.{h,cpp}` and `WaterWake.{h,cpp}` (as `WaterVolume`):

```lua
local ex = self.gameObject:getWaterExcluder()      -- or WaterExcluder(scene, entity)
ex.shape = WaterExcluderShape.BOX                   -- BOX, SPHERE, CONVEX_HULL
ex.center = Vector3(0, 0.5, 0); ex.size = Vector3(2, 1, 5); ex.enabled = true
print(ex:contains(Vector3(0, 0.2, 1)), ex.hullStatus, ex.hullFaces)  -- "ready", 38 (read-only)

local wake = self.gameObject:getWaterWake()          -- or WaterWake(scene, entity)
wake.foam = 1.5; wake.waves = 1; wake.width = 0; wake.length = 0
print(wake.emitting, wake.speed)                     -- read-only, true in water while moving

local s = Water.sample(scene, point)
if s:isExcluded() then print("dry, in", s.excluder) end
print(s.wakeFoam, s.wakeHeight)                      -- 0 where no wake
```

`WaterSample` gains `excluder`, `isExcluded`, `wakeFoam`, `wakeHeight`; `Scene` gains `waterWakeResolution`
(`WaterWakeResolution.AUTO/LOW/MEDIUM/HIGH`), `waterWakeArea`, `waterWakeFade` following the existing scene
setting bindings in `CoreClassesLua.cpp`. `engine/core/object/GameObject.{h,cpp}`: `getWaterExcluder`,
`getWaterWake` (adding the component as `getWaterVolume` does, `GameObject.cpp:377`). `LuaBridgeAddon.h`: the two
enums. C++ scripts use the same handles.

### 6.8 MCP

- Components: core tools as for every reflected component (`add_component component="WaterExcluder"`, values
  `{"waterexcluder": {"shape": "box", "size": [2, 1, 5]}}`; `get_entity`; `set_properties`; undo).
- `get_scene_settings` / `set_scene_settings` (`editor/mcp/tools/SceneTools.cpp:780-1250`): `water_wake_resolution`
  (`auto`, `low`, `medium`, `high`), `water_wake_area`, `water_wake_fade`; enum table registered in
  `McpValues`.
- `sample_water` (`editor/mcp/tools/GameplayTools.cpp:371-498`):
  - each sample adds `excluded_by` (entity JSON or null) and `wake` (`{foam, height}`, null when the scene's wake
    map is not running); `in_water` is false when excluded;
  - new optional `report` (boolean, default false): adds `excluders` (`[{entity, shape, active, hull_status,
    hull_faces, world_bounds}]`), `wakes` (`[{entity, emitting, speed, water}]`), `wake_map` (`{active, size, area,
    origin}`) and `waters` (`[{entity, exclusion, wake}]`: whether each loaded water submesh's variant has the
    bits);
  - `points` becomes optional when `report` is true (schema: neither required; the handler refuses a call with
    neither: "points or report");
  - description updated; text summary gains "dry (Boat)" per excluded point and one line per report section.
  - It stays in its group; `tools/mcp/tool_budget.py --check tools/mcp/budget.json` after the edit.
- `docs/mcp/TOOLS.md` regenerated (`tools/mcp/gen_tool_docs.py`).

### 6.9 Docs and the agent guide

- `docs/mcp/guide/physics.md`, water section: "Water Excluders" and "Water Wakes" subsections (shapes, the dry
  rule and its owner exception, cost, `sample_water` checks, the opaque-water limit).
- `docs/mcp/guide/lua-api.md` 2.10.1: the two handles, the `WaterSample` fields, the scene settings (Lua blocks
  checked by the doc test).
- `docs/shaders/` manifest reference: `@require waterexclusion`, `@require waterwake` and
  `includes/water_interaction.glsl` for custom water shaders; `docs/shaders/library.md` regenerated from the
  manifest.
- `docs/unity-glossary.md`: HDRP Water Excluder -> Water Excluder; HDRP Water Deformer (Bow Wave) and Foam
  Generator -> Water Wake.
- `docs/mcp/guide/README.md`: the physics row's digest gains "water excluders, wakes" (within the guide budget,
  `tool_budget.py --check`).
- `docs/testing.md`: the new GPU golden and the MCP test in their suite lists if those lists name tests.

### 6.10 Export and platforms

- No platform code. Texture arrays, REPEAT dynamic textures and `textureLod` in loops exist on GL 3.3, GLES3 /
  WebGL2, Metal, D3D11 and Vulkan through sokol. Metal and D3D11 cannot be run here: the implementer keeps them
  compiling by the existing patterns (the change is in shared code and GLSL that the shader compiler
  cross-compiles; the library compile matrix compiles the new variants for every backend).
- New engine sources go into `engine/CMakeLists.txt` (as `WaterVolume.cpp`, `:1598`) and the Xcode workspace
  (`engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj`, as `WaterSystem.cpp`).
- Exports ship the 'Wex'/'Wwk' variants of the waters of scenes (and spawnable prefabs) that have the components.
  A script adding a Water Excluder at run time in a scene that had none draws the water through the fallback
  without the bit (no hole), with the fallback's existing log line; the docs say to put the component in a scene
  or prefab (export-pipeline's script-created-objects work may later cover it).

## 7. Behaviour rules and edge cases

### 7.1 Excluders

- Active = component enabled, GameObject active in hierarchy, `Transform::visible` (propagated). Inactive, disabled
  or hidden: no hole, no dryness, no gizmo highlight change.
- Box `size` components are used as absolute values; a zero component or a zero world scale keeps nothing out.
  Negative scale (mirrored) works (the inverse handles it). Sheared parents: exact for Box and Sphere, the hull
  test is in local space (exact).
- Convex Hull source: `MeshTriangles::meshSourceEntities` (own mesh or a multi-node model's mesh nodes); children
  with their own meshes are not part of it. Skinned meshes: the bind pose. A model still loading: `PENDING`, no
  hole until ready. A flat mesh: `FLAT`. No mesh: `NO_MESH`. Each status keeps nothing out but `NO_PHYSICS`
  (bounds box).
- The drawn hole follows the hull map (128 x 128, 8-bit heights: under 1 cm on a 2.5 m hull); the CPU uses the
  exact planes. They differ by at most a map texel at the walls.
- More than 8 excluders in view over one water mesh: the nearest 8 cut holes; one warning per scene.
- A camera exactly at a wall: the CPU test decides the fade; the GPU decides each pixel.
- Water seen from below (UNDERSIDE) through a hole shows the boat's bottom (correct: the hull displaces the water).
- Excluders apply to every water that requires them, pool and lake alike.
- Excluders work in edit mode (Scene and Game views) and in every camera pass of a running scene.
- Dry for buoyancy unless the excluder's owner (nearest ancestor-or-self with a Rigidbody) is the floating body.
  A static excluder (no Rigidbody above it) keeps everything dry, a Rigidbody under it included (Inspector warns).

### 7.2 Wakes

- Emitting requires: active (7.1 rules) and enabled, a running scene, `dt > 0`, the drawn water surface at the
  footprint's centre between its world bottom and top, a smoothed speed above 0.05 m/s.
- Paused game or time scale 0: wakes freeze (no step, no upload). Time scale 2: they evolve twice as fast.
  `play fixed_fps` makes them reproducible.
- Teleport (move above the threshold), deactivation, disabling or hiding: the emitter's history and stern are
  reset; nothing is drawn between the old and new places.
- Leaving the water: no new samples; what was made keeps spreading on that body until it fades. Entering another
  body: new samples on it; the two trails never mix (D9).
- Stop (scene no longer running): every emitter and the map are cleared; the next Play starts clean.
- Camera cut far away: the map moves a full size and is cleared.
- Map size or area changed in Play: the map is reallocated empty.
- More than 255 bodies with wakes: the extra bodies show none (warned once).
- Scenes as layers each have their own map and follow their own main camera.

### 7.3 Shader

- No 'Wwk' bit: no wake code. Bit on, map idle (`wakeArea.w` 0): one uniform branch, the program's output equals
  the program without wake code (test 10.2).
- No 'Wex' bit: no exclusion code. Bit on, no excluder selected (count 0): no discard; output identical to the
  program without it (test 10.2).
- Wake foam needs wake (`bb82c1d3e`): zero wake leaves the foam as the material's.

## 8. Performance and cost

| Situation | CPU | GPU | Memory |
|---|---|---|---|
| No Water Excluder, no Water Wake (every project today) | two component-array size checks a frame in `WaterInteractionSystem::update`; nothing in Render, physics or queries beyond a null/size check | the same programs and bindings as today (no bits) | none |
| Excluders present | gather O(excluders) a frame; per water draw a filter and sort of at most the excluders in view; dry tests O(excluders) per floating body column, broad-phased by bounds | per water pixel: up to 8 x (3 dot products and 6 compares), one `textureLod` per hull whose box holds the pixel; one 560-byte block per water draw | per hull: 64 KB GPU layer + 64 KB CPU map + planes (under 10 KB) |
| Hull (re)build | once per signature change: quantised input, Jolt hull (128 vertices max), map 128 x 128 x faces (under 4.2 M multiply-adds, a few ms, at load) | array recreated once | as above |
| Wake bit present, map idle | none | one uniform branch per water pixel | none |
| Wakes active | per frame: decay over size^2 texels (16 K mobile/web, 65 K desktop), stamps per emitter history, RGBA rebuild; budget 0.3 ms desktop with 4 emitters at 256, 0.2 ms mobile at 128 (bench 10.1) | per water pixel 3 `textureLod` and about 25 ALU; one upload of 64 KB (mobile/web) or 256 KB (desktop) a frame | map: size^2 x (4 + 4 + 1 + 4) bytes: 208 KB at 128, 832 KB at 256 |

## 9. Compatibility

- Old projects: unchanged. No component means no bits: identical shader keys, identical programs, identical
  pixels (golden `water_features_off` must still match exactly on glcore).
- Scene files: two new component keys and three optional scene setting keys, written only when different from the
  defaults (as `sceneColorScale`, `Stream.cpp:4331`); no format version bump. A scene saved with the components
  does not load them in an older editor (unknown components are that editor's concern).
- Shader keys: two MESH bits appended; existing keys keep their meaning; the fallback drops the new bits first.
- Lua and MCP: additions only. `sample_water`'s output gains fields; `points` stops being required (a call that
  passed it is unchanged).
- Owner decisions in `EXTRA-FINDINGS.md` do not touch this feature.

## 10. Tests

Red controls: for new behaviour the red run is the test on `main` before the change (it fails: no component,
field or bit). For the Doriax follow-up fixes folded into the new code (hidden excluders, wakes on their own water,
one upload per frame, no foam without wake) the red control is a documented mutation run: remove the guard, run
the test, record the failure in the commit message, restore.

### 10.1 `jasperengine-core-tests`

- `engine/tests/core/util/WaterExclusionTest.cpp`
  - `WaterExclusion.BoxAndSphereContain`: points inside/outside a rotated, non-uniformly scaled box and sphere;
    zero scale contains nothing. Red: before the change the file does not compile (no API).
  - `WaterExclusion.HullFromPrism`: a triangular-prism vertex set builds READY with 5 planes; points inside and
    just outside each face; a 50,000-point noisy box quantises to fewer than 2,000 hull inputs and at most 128 hull
    vertices.
  - `WaterExclusion.HullMapColumns`: map texels at known XZ hold the prism's bottom and top within 1/255, an empty
    column is (255, 0), wall channel below 0.5 inside, above outside.
  - `WaterExclusion.FlatAndEmpty`: a plane gives FLAT, no vertices NO_MESH.
- `engine/tests/core/subsystem/WaterInteractionSystemTest.cpp`
  - `WaterExcluder.InactiveHiddenOrDisabledKeepNothingOut` (mutation red: drop the visibility check; Doriax
    `a0b400b4e`).
  - `WaterExcluder.HullFollowsTheMesh`: replacing the mesh's geometry rebuilds once (`getHullBuildCount` +1),
    moving the GameObject does not.
  - `WaterExcluder.OwnerIsNearestRigidbody`: excluder on a child of a body: owner is the body; cargo body under
    the boat with an excluder under the cargo: owner is the cargo.
  - `WaterExcluder.NoWorkWhenAbsent`: a scene with water and no components: counters stay 0, no hull, no map.
- `engine/tests/core/physics/BuoyancyTest.cpp` (existing fixture)
  - `BuoyancyTest.CargoKeptDryInExcluder`: a Buoyancy box inside a static Box excluder below the surface gets no
    lift (falls onto a static floor), one outside floats. Red: on `main` both float.
  - `BuoyancyTest.RigidbodyBuoyancyKeptDry`: the same for the Rigidbody `buoyancy` path over a `library:water` mesh.
  - `BuoyancyTest.OwnExcluderStillFloats`: a boat body with a Convex Hull excluder on itself floats as without it.
  - `BuoyancyTest.SampleReportsExcluder`: `Water.sample` inside gives `inWater` false, `excluder` set, surface kept.
- `engine/tests/core/util/WaterWakeMapTest.cpp`
  - `WaterWakeMap.KelvinAngle`: 19.47 degrees below Froude 0.5, 0.5/Fr x that above.
  - `WaterWakeMap.MoveClearsOnlyUncovered`: shifting the origin by 3 texels clears 3 columns, keeps the rest.
  - `WaterWakeMap.FoamDecay`: after `fade` seconds of steps an eighth of the foam is left (exp(-2) within 1%).
  - `WaterWakeMap.StaysOnItsWater`: two bodies side by side; samples on body A stamp no texel tagged B, and a
    pair of samples crossing from A to B stamps nothing between them (mutation red: ignore tags; Doriax
    `db794e641`).
- `engine/tests/core/subsystem/WaterWakeTest.cpp` (scene, pinned clock, fixed delta)
  - `WaterWake.EmitsBehindAMovingBox`: a box with a Water Wake crossing a water at 4 m/s for 2 s: foam behind the
    stern above 0.2, ahead 0; a box above the water emits nothing.
  - `WaterWake.HiddenLeavesNoTrail`: hide, move 10 m, show: no foam along the jump (mutation red).
  - `WaterWake.TeleportResets`, `WaterWake.PauseFreezes` (dt 0: `getWakeStepCount` unchanged, no dirty flag),
    `WaterWake.StopClears`.
  - `WaterWake.OneUploadPerFrame`: two updates before one draw upload once (a counter on the RenderSystem's wake
    upload; mutation red: upload in update; Doriax `c5b7a99dc`).
- `engine/tests/core/shader/ShaderZeroCostTest.cpp`: two new cases (`e.waterExclusion = true`,
  `e.waterWake = true`): every built-in Lit variant's hash is unchanged (red: a bit leaking to Lit adds variants
  missing from the list).
- `engine/tests/core/shader/WaterInteractionVariantTest.cpp`
  - `WaterInteractionVariant.BitsOnlyWithRequireAndComponent`: `library:water` gets 'Wex' only with
    `environment.waterExclusion`, 'Wwk' only with `waterWake`; `library:unlit` never.
  - `WaterInteractionVariant.FallbackDropsWakeThenExclusion`: `getFallbackProperties` lists the key without 'Wwk'
    first, then without 'Wex'.
  - `WaterInteractionVariant.ManifestRequire`: `@require waterexclusion waterwake` parses; on a postprocess shader
    it is an error; `getRequireDefines` adds nothing for them.
- `engine/tests/core/render/WaterBoatsGpuTest.cpp` (headless device, pinned shader time; glcore exact, other
  backends the cross-backend bound, as `WaterFeaturesGpuTest`)
  - `WaterBoatsGpu.ExcludersCutTheSurface`: golden `water_excluders.png`: a water plane over a red ground, a
    rotated Box, a Sphere and a Convex Hull (prism mesh) crossing the surface: red shows inside each, water around.
    Red: on `main` there is no hole (and no component).
  - `WaterBoatsGpu.IdleBitsKeepTheWater`: an excluder far outside the camera and a Water Wake not on water (both
    bits on, count 0, map idle): the image equals `water_features_off.png` exactly on glcore.
  - `WaterFeaturesGpu.FeaturesOffMatchesTheShaderBeforeThem` (existing) passes unchanged.
- Bench (reported, gated by `tools/perf/bench-thresholds.json` like the others): `WaterWakeBench.Step256x4` and
  `Step128x4` against the budgets of section 8.

### 10.2 `jasperengine-editor-tests`

- `editor/tests/stream/StreamComponentTest.cpp`: `STABLE(WaterExcluderComponent, ...)`, `STABLE(WaterWakeComponent,
  ...)` round trips; a scene without the scene setting keys loads the defaults; non-default settings round-trip.
- `editor/tests/reflect/catalog_snapshot.json` updated (catalog snapshot test).
- `editor/tests/shader/ShaderVariantCollectorWaterTest.cpp`: a scene with a water and a Water Excluder collects the
  'Wex' variant of the water material; a scene that can spawn a prefab with one collects it too; a scene without
  collects neither (red: on `main` no 'Wex').
- `editor/tests/mcp/McpEnumTablesTest.cpp`: the two new enum tables.

### 10.3 MCP end to end: `tools/mcp/tests/test_water_boats.py`

Scene "Boats" (3D): the GameObject menu's Water with a Water Volume (20 x 6 x 20), a red static floor at y -2,
a library:underwater pass, an orthographic main camera straight down.

- `test_components_defaults`: `add_component` WaterExcluder / WaterWake; `get_entity` shows `convex_hull`,
  center 0, size 1 / foam 1, waves 1, width 0, length 0; undo removes, redo restores.
- `test_sample_water_excluded`: a Box excluder; `sample_water` inside: `in_water` false, `excluded_by` the box;
  outside: `in_water` true; set `enabled` false, then deactivate, then hide (`set_entity_state`): not excluded each
  time.
- `test_report_variants`: `sample_water {report: true}` before: `waters[0].exclusion` false; after adding the
  excluder: true; `excluders[0].hull_status` `ready` for a cube's hull; after removing: false again.
- `test_hole_in_view`: `sample_view_color` (group `editor_tools` enabled through `tool_groups`) at the pixel over
  the excluder reads the floor's red; next to it the water colour (red control: before the component).
- `test_cargo_stays_dry`: Play `fixed_fps=60`, 360 frames: a Buoyancy crate (density 500) inside a static Box
  excluder rests on a floor below the surface (y < -1), one outside floats (|y| < 0.35).
- `test_camera_inside_no_fade`: Play with the main camera below the surface inside an excluder: the underwater
  pass stays disabled (`eval_lua` reading the pass); moved outside the excluder (still under): enabled.
- `test_wake_in_play`: a box with Water Wake moved by an attached Lua script at 5 m/s across the water; after 120
  frames `sample_water` behind the stern has `wake.foam > 0.1`, ahead 0; `report.wakes[0].emitting` true; a second
  water beside the first (gap 1 m) shows no wake at points across the gap.
- `test_scene_settings`: `set_scene_settings water_wake_resolution="high" water_wake_area=32 water_wake_fade=4`,
  `get_scene_settings` returns them; undo restores.

### 10.4 Other gates

- `jasperengine-editor shaders --library water` (the compile matrix, every backend the image has): passes with the
  new worst-case variant.
- Lua doc test over `docs/mcp/guide/lua-api.md`; `tools/mcp/gen_tool_docs.py --check`;
  `tools/mcp/tool_budget.py --check tools/mcp/budget.json`; `tools/check_agent_docs.py` if the guide README
  changes.
- `tools/check-changed` per commit; the web gate (`tools/build.sh web`, `web-gate`) once, for
  WebGL2 texture arrays and the REPEAT dynamic texture.

## 11. Acceptance criteria

1. Adding a Water Excluder (default Convex Hull) to a GameObject with a mesh crossing a `library:water` surface
   removes the surface inside the hull in the Scene view, Game view and in Play (`WaterBoatsGpu.ExcludersCutTheSurface`,
   `test_hole_in_view`).
2. Box and Sphere shapes with center and size work, rotated and scaled; Fit to Mesh sets them in one undo step.
3. A camera inside an active excluder below the surface keeps `library:underwater` off; outside it, under the
   surface, the pass switches on as today (`test_camera_inside_no_fade`).
4. A Buoyancy body and a Rigidbody `buoyancy` body inside a foreign excluder get no lift; a boat with its own
   excluder floats as before (`BuoyancyTest.*Dry*`, `OwnExcluderStillFloats`, `test_cargo_stays_dry`).
5. `Water.sample` / `sample_water` report `excluded_by` / `excluder` and `inWater` false inside; `Water.isUnderwater`
   is false inside.
6. Disabled, inactive or hidden excluders keep nothing out; hidden emitters leave no trail.
7. A moving GameObject with a Water Wake on a water leaves foam and waves that fade by Wake Fade; none above the
   water, none on another water body, none after Stop, frozen while paused (`WaterWake.*`, `test_wake_in_play`).
8. The wake map uploads at most once per frame and not at all while idle (`WaterWake.OneUploadPerFrame`).
9. A scene without the components draws pixel-identical water (`water_features_off` exact on glcore) and every
   built-in Lit variant hash is unchanged (`ShaderZeroCost.DefaultVariantsUnchanged`); with the components idle
   the water is still pixel-identical (`WaterBoatsGpu.IdleBitsKeepTheWater`).
10. Exports of a scene (or spawnable prefab) with the components ship the 'Wex' / 'Wwk' water variants
    (`ShaderVariantCollectorWaterTest`); a missing variant falls back to the water without them.
11. Wake Resolution Auto gives 256 on desktop and 128 on Android, iOS and web; the three scene settings are editable
    with undo in the Inspector and through `set_scene_settings`, and saved only when not default.
12. Inspector rows, Add Component entries, Scene view gizmos and warnings exist as in 6.6; every edit is undoable.
13. Lua: `WaterExcluder`, `WaterWake`, `WaterExcluderShape`, `WaterWakeResolution`, `GameObject:getWaterExcluder/
    getWaterWake`, the `WaterSample` fields and the scene settings work as documented (doc test green).
14. The library compile matrix compiles the worst-case water variant with both bits for every backend within 16
    fragment textures; the web gate passes.
15. Docs updated (6.9); `TOOLS.md` regenerated; tool and guide budgets pass.
16. The wake bench meets the section 8 budgets.

## 12. Files likely touched

New:
- `engine/core/component/WaterExcluderComponent.h`, `engine/core/component/WaterWakeComponent.h`
- `engine/core/subsystem/WaterInteractionSystem.{h,cpp}`
- `engine/core/util/WaterExclusion.{h,cpp}`, `engine/core/util/WaterWakeMap.{h,cpp}`
- `engine/core/object/environment/WaterExcluder.{h,cpp}`, `engine/core/object/environment/WaterWake.{h,cpp}`
- `engine/shaderlib/includes/water_interaction.glsl`
- tests: `engine/tests/core/util/WaterExclusionTest.cpp`, `engine/tests/core/util/WaterWakeMapTest.cpp`,
  `engine/tests/core/subsystem/WaterInteractionSystemTest.cpp`, `engine/tests/core/subsystem/WaterWakeTest.cpp`,
  `engine/tests/core/shader/WaterInteractionVariantTest.cpp`, `engine/tests/core/render/WaterBoatsGpuTest.cpp`,
  `engine/tests/core/golden/water_excluders.png`, `editor/tests/shader/ShaderVariantCollectorWaterTest.cpp`,
  `tools/mcp/tests/test_water_boats.py`

Changed (hot files marked *):
- *`engine/core/subsystem/RenderSystem.{h,cpp}` (environment, variant bits, bindings, per-draw apply, draw-time
  GPU sync), *`engine/shaderlib/library/water.frag`
- *`engine/core/object/environment/Water.{h,cpp}`, *`engine/core/subsystem/WaterSystem.{h,cpp}`
- *`engine/core/pool/ShaderPool.{h,cpp}`, `engine/core/shader/ShaderVariantCompiler.cpp`,
  `engine/core/shader/ShaderManifest.{h,cpp}`
- `engine/core/render/TextureRender.{h,cpp}`, `engine/renders/sokol/SokolTexture.{h,cpp}`
- `engine/core/Scene.{h,cpp}`, *`engine/core/SceneSettings.h`
- `engine/core/registry/EntityRegistry.{h,cpp}`, `engine/core/registry/EntityCloner.cpp`,
  `engine/core/serialization/RuntimeFields.gen.inc`, `engine/core/animation/PropertyBindings.cpp`,
  `engine/core/JasperEngine.h`, `engine/core/object/GameObject.{h,cpp}`
- *`engine/core/script/binding/GameObjectClassesLua.cpp`, `engine/core/script/binding/CoreClassesLua.cpp`,
  `engine/core/script/LuaBridgeAddon.h`
- `engine/CMakeLists.txt`, `engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj`,
  `engine/tests/core/CMakeLists.txt`, `engine/tests/core/shader/ShaderZeroCostTest.cpp`,
  `engine/tests/core/physics/BuoyancyTest.cpp`, `tools/perf/bench-thresholds.json`
- *`editor/Catalog.h`, *`editor/Factory.{h,cpp}`, *`editor/Stream.{h,cpp}`, `editor/StreamComponents.cpp`,
  *`editor/StreamSchema.cpp`, `editor/reflect/components/GameplayReflect.cpp`, `editor/reflect/Enums.{h,cpp}`,
  `editor/tests/reflect/catalog_snapshot.json`, `editor/tests/stream/StreamComponentTest.cpp`,
  `editor/tests/mcp/McpEnumTablesTest.cpp`
- *`editor/window/InspectorPhysics.cpp`, `editor/window/InspectorDefault.cpp`, `editor/window/InspectorWindow.{h,cpp}`,
  `editor/window/dialog/ComponentAddDialog.cpp`, `editor/render/SceneRender3D.{h,cpp}`,
  `editor/util/WaterEditorUtils.{h,cpp}`
- *`editor/shader/ShaderVariantCollector.cpp`, `editor/shader/ShaderLibraryMatrix.cpp`
- `editor/mcp/tools/GameplayTools.cpp`, *`editor/mcp/tools/SceneTools.cpp`, `editor/mcp/McpValues.cpp`
- docs: `docs/mcp/guide/physics.md`, `docs/mcp/guide/lua-api.md`, `docs/mcp/guide/README.md`, `docs/mcp/TOOLS.md`,
  `docs/shaders/` (manifest reference, `library.md`), `docs/unity-glossary.md`

## 13. Cross-feature dependencies

- `water-reflections` (hard, merge it first): it edits `water.frag` (planar reflection, Gerstner option, shadow
  normal, caustics), `Water.{h,cpp}` (the fork marker and its predicate, which replaces the exact-name test in
  `Water::readMaterial`; this feature's water-body detection and the Inspector's "a water requires exclusions"
  check use that predicate) and the water clock (paused waves). This spec keeps its shader edits additive and its
  uniforms in its own block, so the merge is a few lines; wakes use the scene's update delta, not the shader
  clock. The worst-case texture count is checked jointly by the compile matrix (planar reflection + 2).
- `mesh-memory`: MeshComponent submesh storage changes; this feature iterates submeshes and reads
  `MeshSourceView`s; adapt to its API at merge.
- `draw-perf`, `render-correctness`, `sprite-batching`, `ssr-probes`: `RenderSystem.cpp` merge hotspots; MESH
  property bit numbers (any branch adding MESH bits: first merged keeps 36/37, the other takes the next free;
  `draw-perf`'s directional-only variant may add one).
- `export-pipeline`: `ShaderVariantCollector.cpp` (facts and environment bits); its script-created-object key work
  may later cover a Water Excluder added only by a script.
- `scripting`: `GameObjectClassesLua.cpp` / `CoreClassesLua.cpp` binding hotspots.
- `scene-integrity`, `agent-authoring`: `SceneTools.cpp` and scene-settings plumbing (`Stream.cpp`,
  `StreamSchema.cpp`, `InspectorWindow.cpp`).
- No dependency on the other features.

## 14. Deviations

Recorded during implementation (branch `dc/water-boats`, built on `dc/water-reflections`):

1. **Bit and flag numbers.** `main` had taken MESH bits 36-41 (S57's lighting, LOD fade, auto
   instancing) and water-reflections `ShaderRequire_PlanarReflection = 1 << 12` by the time this
   was built: 'Wex' and 'Wwk' are MESH bits 42 and 43, `ShaderRequire_WaterExclusion` /
   `ShaderRequire_WaterWake` are `1 << 13` / `1 << 14`. A branch merging MESH bits before this one
   takes the next free ones (the short names are the identity).
2. **Lua names.** Bound getters are properties (`.claude/rules/scripting.md`): `WaterSample.excluded`
   (not `isExcluded()`), `WaterWake.emitting` / `speed`, `WaterExcluder.hullStatus` / `hullFaces`.
3. **The wake's automatic footprint and Fit to Mesh** use the meshes a hull is built from (the
   GameObject's mesh, or its model's mesh nodes: `WaterInteractionSystem::getMeshSourceLocalBounds`),
   so the Inspector's "Auto" line, Fit to Mesh and the wake agree.
4. **Opaque water reading the excluders or the wake map is not automatically instanced**
   (`getSubmeshBatchVariant`): both are chosen per mesh. Only a variant with 'Wex' or 'Wwk' (a
   scene with the component) draws alone; without them opaque water tiles batch as before.
   Transparent water was never batched.
5. **Prebuilt players.** A Convex Hull excluder makes the scene need the `physics3d` module
   (`editor/players/ModuleUsage.cpp`): the hull is built by Jolt.
6. **Tests renamed and regrouped.** `WaterInteractionTest.*` (scene-level: excluders, owner rule,
   wakes, pause and stop) replaces the spec's `WaterExcluder.*` / `WaterWake.*` scene suites;
   `WaterBoatsGpu.WakeUploadsOncePerFrame` replaces `WaterWake.OneUploadPerFrame` (the upload is a
   RenderSystem draw); the variant tests are editor tests (`WaterInteractionVariants.*`, they need
   the library manifests and the collector).
7. **The wake bench** is `WaterWakeBench.Step256x4` / `Step128x4` in `jasperengine-core-tests`, the
   budgets enforced through `JASPER_TEST_PERF_SLACK` like the other *Bench suites (AGENTS.md:
   wall-clock budgets through `perfSlack()`), not an entry in `tools/perf/bench-thresholds.json`.
8. **The water's per-draw data lives in its own custom block**, not a uniform block of its own: a
   lit `library:water` with every feature is at the fragment stage's budget of uniform blocks, and
   the `u_fs_waterInteraction` block the spec planned put it over (measured on the first build:
   `checkResourceBudget` reported 10 blocks for the variant with shadows, fog and the post colour).
   `WATER_INTERACTION_PARAMS` (`includes/water_interaction_params.glsl`) appends engine-written
   members (`waterExclusion`, `waterExclusionRows`, `waterExclusionShapes`, `waterWakeArea`,
   `waterWakeParams`; reserved names like `sceneTime`) to the shader's `u_fs_customParams`;
   `CustomUniformBlock::writeWaterInteraction` writes them each draw before the block is uploaded.
   The bits add two textures and no block (`WaterInteractionVariants.WorstWaterVariantsCompileForEveryBackend`).
9. **The wake step is cheaper than Doriax's**, measured by the bench: Doriax's stamping as ported took
   52 ms a step at 256 texels with four hulls. The history keeps a bow sample every 0.5 m (or a fifth
   of the hull's length; Doriax every 0.25 m or a tenth), the crests are stamped again 20 times a
   second at most (once they may have moved half a texel: 16 times at 256 texels over 64 m, 8 at
   128; the foam every step), the crests along straight runs of samples (one stamp while the samples
   between stay within half a texel of its line, at most 16), one transverse wave stamp on each of
   their crests (where the samples' phase peaks; Doriax stamps every sample's), a stamp skips texels
   already at its strength, and the falloffs are polynomial in the squared distance (a crest
   `(1 - d^2)^2`, close to Doriax's `0.5 + 0.5 cos(pi d)`; foam solid to d = 0.45 as Doriax's), with
   no root or cosine per texel; the RGBA texels are packed in a loop that vectorizes.
10. **Cooked scenes carry the wake settings as format 1.5** (after water-reflections' 1.4): D12 holds
    for the project format, but a prebuilt player reads scene settings from the cooked record
    (`cook::writeSceneSettings`), so the three are appended there; a 1.4 record leaves them at
    their defaults (`CookedScene.SceneSettingsKeepWaterWakes`). The editor's shader cache is v52.
11. **The wake bench's desktop budget is not met on a build box.** On shared box D (other builds
    running) `Step128x4` measured 140 us (budget 200) and `Step256x4` 570 us (budget 300). The
    budgets are enforced only with `JASPER_TEST_PERF_SLACK` on a quiet machine
    (`docs/testing.md`), and that run has not been made.
