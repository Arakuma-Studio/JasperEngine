# 03 — World Building: JasperEngine (OURS) vs OpenEngine (THEIRS)

Domain: terrain, grass/foliage, ocean/water, splines and spline tools, pathfinding/navmesh, particles/VFX, procedural noise, trees.
Paths: THEIRS is relative to `external/OpenEngine/`, OURS to the repo root. Every claim below comes from reading the code. Comments and docs were used only where they matched the code.

---

## 1. What THEIRS has

### 1.1 Terrain: three layers (authoring, bake/streaming, rendering)

**Component and authoring model**
- `Engine/Include/Components/Terrain/Terrain.h` is one control component. `BaseSource` is ProceduralNoise, HeightmapAsset (.r16/.r32 raw or 16-bit PNG) or Flat. `Domain` is Planar or Spherical (a cube-sphere planet). Other fields: `SamplesPerMeter`, `StreamingRadius`, `SeaLevel` (keeps the seabed under water coarse), `TargetPixelError`, `MaterialLibraryGuid` (`.terrainmatlib`), 4 splat roles (grass/rock/dirt/snow → `LayerRoleSlot`), per-layer tiling, and `LayerHexTiling` (Mikkelsen hex tiling bitmask).
- **Non-destructive modifier stack** (`Components/Terrain/TerrainModifierVolume.h`, `TerrainModifierEffects.h`, `TerrainModifierBlend.h`, `TerrainSurfaceRules.h`). A *volume* says WHERE: `Rectangle | Circle | SplinePath | SplineArea | Global`, with inner/outer falloff and a cross-volume `Priority`. *Effects* on the same entity say WHAT: `TerrainFlattenEffect`, `TerrainHeightOffsetEffect`, `TerrainNoiseEffect` (fBM plus an erosion-filter block), `TerrainStampEffect`, `TerrainPaintLayerEffect`, `TerrainSurfaceRulesEffect`, `TerrainGroundClaimEffect`, `TerrainGrassEffect`. Effects stack by `StackOrder` and have blend modes, including "Average" pools with named groups. Ground claims let one region own its ground against others.
- **Surface rules** (`TerrainSurfaceRules.h`, `TerrainECS/TerrainSurfaceRuleEval.h`, `TerrainDefaultSurfaceRules.h`, `TerrainRuleNoise.h`): rows of AND-multiplied conditions (slope in degrees or normalized, height in metres or normalized, noise) with feathered bands, writing splat weights procedurally.
- Spline-driven terrain: a `SplinePath` volume plus a Flatten effect cuts a road bed. The route is resampled per station (`TerrainECS/TerrainRoutePolyline.h`).
- Bake: `TerrainECS/Source/Systems/TerrainModifierSystem.cpp` (6.3k LOC) and `TerrainModifierSystem.h` (1.2k LOC). Rebakes only the dirty region (`DirtyRegionLog.h`; `CDLODQuadtree::RebuildRegion`). There is a GPU bake path (`TerrainGpuBake.h`, `CBTTerrain/Shaders/CBT/terrain_height_bake.comp`) and a per-scene bake cache (`TerrainBakeCache.cpp`).
- Erosion: `TerrainECS/Source/Erosion/ErosionFilter.cpp` is a point-local analytic "Fast and Gorgeous Erosion Filter" (Runevision). **MPL-2.0**, see `ThirdParty/RunevisionErosionFilter/UPSTREAM.md`. It runs inside the noise effect. It is not a droplet simulation.

**Data, streaming, scale**
- `Terrain/Heightfield.{h,cpp}`, `Terrain/CDLODQuadtree.{h,cpp}` (min/max quadtree, region rebuild), `Terrain/CDLODSelection.cpp`. The CDLOD selection is now only used service-side (LOD ranges, nav, debug). Rendering is CBT (see the comment at the top of `TerrainECS/Include/TerrainECS/TerrainRenderFeature.h`).
- Tiling and streaming: `TileStreamingManager.{h,cpp}` (800 LOC), `TerrainAtlas.{h,cpp}` (a GPU atlas of height/splat/normal tiles), `AtlasResidencyController`, `PagedHeightSampler`, `TerrainHeightPages.*`, `HeightPageStoreLoader.*`, `TerrainModifierPageOverlay.cpp`. The sizing plan (`TerrainSizingPlan.cpp`) works by samples along the longer side: ≤1025 is one map; up to 8193 is resident tiles; above that, a streamed window around the camera plus a coarse field. Raw DEMs are cooked into a page pyramid (`.Cache/TerrainPages`, shipped as `.geterrain`). The docs measure a 60×15 km terrain at 0.25 m (`docs/terrain/large-heightmaps.mdx`).
- Physics: `TerrainECS/Source/Systems/TerrainPhysicsSystem.cpp` builds Jolt heightfield colliders per tile (`TerrainTileCollider`) and planet face colliders.

**Rendering: CBT/LEB (concurrent binary tree)**
- `Engine/Modules/CBTTerrain` is 46k LOC, about half of it tests. It ports `AnisB/large_cbt` (Benyoub & Dupuy 2024). A GPU-resident 1M-bisector pool is refined in compute (`Shaders/CBT/cbt_kernels.comp` 3k LOC, `cbt_layout.glsl`, `cbt_surface.glsl`). The split metric is screen-space pixel error, so triangle count does not depend on resolution. Planar and spherical domains are supported, and sphere sculpting has a `.tsculpt` sidecar (`SphereSculptLayer.cpp`). It **requires compute, `shaderInt64`, SSBOs, indirect draws and bindless** (`CBTLayout.h` comments; the feature disables itself without int64).
- Terrain sun shadows come from the heightfield (`CBTTerrainECS/Source/TerrainShadowBake.cpp`, `docs/terrain/terrain-shadows.mdx`), with PCSS-consistent penumbra.
- Material shading: `Rendering/Shaders/Includes/terrain_material_albedo.glsl` does triplanar or planar projection (planar is for orthophotos), hex tiling (lines 203–316) and value-noise albedo variation.

**Editor** (`Apps/Editor/Source/Terrain/*`, about 6k LOC; `SceneView/TerrainBrushTool.cpp`, `TerrainModifierGizmo.cpp`)
- The inspector shows the sizing plan before you commit (`TerrainInspector.cpp` 1.7k), modifier inspectors (`TerrainModifierInspectors.cpp` 1.2k), a rule-row editor (`TerrainRuleRows.cpp`), material library minting, a heightmap import inspector (Samples X/Z validation), and a page-cook progress overlay (`TerrainPageCookOverlay.cpp`).
- **The interactive brush is minimal: Raise / Lower / Paint only** (`TerrainBrushTool.cpp`, keys 1/2/3, `[ ]` for radius, `- =` for strength). Strokes become "zone" payloads (`TerrainZonePayloadSaver.cpp`). In-scene volume gizmos edit the procedural volumes.

### 1.2 Grass (`Engine/Modules/TerrainGrass`, 13.6k LOC, ~40% tests)
- Fully GPU-driven procedural blades. Compute kernels: classify, then plan, then place (`Shaders/TerrainGrass/terrain_grass_{classify,plan,place}.comp`). Placement uses a world-anchored cell lattice: the camera only picks cells, so blades never swim. The output is deterministic with no atomics deciding order (`GrassPlacementModel.h`, mirrored bit-for-bit in `grass_placement.glsl`). Draws use `DrawIndirectCount` (`TerrainGrassRenderFeature.cpp:594-677`).
- Blade model: Voronoi clumps (`grass_clump.glsl`, `GrassClumpModel.h`), segments, tilt/bend/twist, tuft fan, LOD1 blade, growth ramp instead of pop-in, root ground colour sampled from the splat (`grass_root_ground_color.glsl`), canopy shade, translucency, presets (Meadow/Lawn/Steppe), a texture-card mode, and wind volumes (`grass_wind_volumes.glsl`, `GrassWindVolumeRing.cpp`). Regional overrides come through `TerrainGrassEffect`. Component: `Engine/Include/Components/Terrain/TerrainGrass.h`.
- Hard dependencies: bindless (`GL_EXT_nonuniform_qualifier`), SSBOs, compute and indirect-count.

### 1.3 Trees (`Packages/eztree`, ~4.3k LOC of code)
- `Native/EZTree/EZTreeGenerator.cpp` (607 LOC) plus `EZTreeOptions/Presets/Rng` is a native C++ port of **EZ-Tree 1.1.0 (MIT, Daniel Greenheck)**. It generates branches recursively, plus leaves and a trellis force, and outputs a `ModelAsset` mesh. It uses glm.
- ECS component, extraction system, runtime materials (bark/leaves/trellis), a wind vertex modifier (`Assets/Shaders/VertexModifiers/ez_tree_wind.glsl`), leaf surface shader, an editor plugin (1.1k LOC) and a stats panel. The bark/leaf textures were made by the engine authors (`Assets/Textures/EZTree/NOTICE.md`). The preset JSONs come from the upstream vcpkg port and are **not in the repo**.

### 1.4 Ocean (`Engine/Modules/Ocean` 27.7k LOC + `Rendering/Shaders/Ocean/*` 45 shaders)
- A port modeled on **Crest 4.23 (MIT, Wave Harmonic)** (`Engine/Modules/Ocean/NOTICE`, `cmake/ports/crest-ocean-upstream`).
- FFT: 256² resolution, **16 cascades** (6 cm ripples to 16 km swell), a spectrum with Pierson-Moskowitz wind cutoff and per-octave power LUT, and horizontal/vertical butterfly passes (`ocean_fft_*.comp`, `OceanFFT.cpp`). Gerstner is legacy only.
- Sim LOD cascades (camera-snapped Texture2DArrays): foam (Jacobian folding), flow, dynamic waves (interactive ripples), clip, albedo, wave mask, seabed depth, and shadow. Also caustics, light shafts, underwater with portals and exclusion volumes, planar reflection, scene grab/refraction and GPU spray particles (`OceanSprayGPU.cpp`).
- Inputs: water bodies, polygon water bodies, **spline inputs** rasterized into depth/flow/clip/albedo (`OceanSplineRaster.cpp`, `ocean_spline_raster_*.comp`), wave impulses, and water interaction.
- Surface mesh: Crest concentric patch rings with fat/slim seams (`OceanRenderFeature.cpp:770+`).
- **Buoyancy**: `Source/Systems/OceanBuoyancySystem.cpp` (166 LOC). Four probes around a Jolt body apply a force at each probe position, plus water-relative linear drag and angular drag. Heights come from `OceanCollisionProvider` (async GPU query readback, a baked FFT collision asset `OceanFFTCollisionAsset.cpp`, or the analytic Gerstner fallback).

### 1.5 Splines
- **Core** `Engine/Modules/Spline` (~1k LOC + tests, depends only on their Mathematics):
  - Point types: CatmullRom, CubicBezier and Linear. Each control point carries radius, roll, rotation and scale.
  - Arc-length LUT and segment AABBs.
  - Queries: `Evaluate`, `EvaluateAtDistance`, `FindClosestPoint` (AABB broad phase plus ternary search), `SignedDistanceToSpline`, XZ variants (`FindClosestPointXZ`, `SignedDistanceToSplineXZ`, `SignedDistanceToSplineBandXZ`), `SampleUniform`, per-point channels, and Douglas-Peucker simplification (`SplineUtility.h`).
  - Files: `SplineEvaluator.{h,cpp}`, `SplineData.*`, `SplineTypes.h`.
- **SplineGeometry** (~4.4k LOC + tests; Types + Mathematics only, **C++20/std::span**): `SplineStripBuilder` (sweeps a profile with world-metre UVs, closed-loop UV snapping, chunked U origin), `SplineProfile` (Bevel and road Crown profiles), `SplineCorner` (mitre/round corners), `SplineEndTaper`, `SplineFillField` + `SplineFillMesher` (water "FitToBanks" flood-fill area mesh), `MeshPlaneCut`, `SplineStep`.
- **SplineLayout** (~3.3k LOC + 8k LOC tests): `FenceLayout.cpp` (2k LOC; posts and spans, racked/stepped grade, crest pitch, span stretch, gate overrides), `TileLayout.cpp` (tile placement fit-to-length), `SpanMitre`, `SeamShear`, `CenterlineSampling`.
- **Components** (`Engine/Include/Components/Spline/*`): `SplineComponent`, `SplinePlacement` (mesh pools, spacing/fit, conform Height/HeightAndSlope to Scene or TerrainOnly, plant mode, jitter, dropout, end taper, max tilt), `SplineExtrude` (path/road/water strip), `SplineWall`, `SplineFence`, `SplineStationJitter`.
- **Editor**:
  - `Apps/Editor/Source/SceneView/SplineTool.cpp` (2.6k LOC): freehand stroke capture, then simplify, smooth and snap to surface or mesh (with a BVH option), then the spline. It can also extend an existing spline, marquee multi-select control points, drag, delete, and edit Bezier handles.
  - `SplineSceneGizmo.cpp`, `SplineToolMenu.cpp`, `SplineDrapePolylines.cpp`.
  - `Apps/Editor/Source/Placement/*` (~5k LOC): controllers rebuild the generated chunks after a debounce (`SplineRebuildGate.cpp`), with ground authority, surface conform, chunk slots and commit.

### 1.6 Pathfinding (`Engine/Modules/Pathfinding` 7.6k LOC, `PathfindingECS` 11.8k LOC; see `Pathfinding/Architecture.md`)
- **Recast/Detour/DetourCrowd** (zlib, via vcpkg, optional via `GE_PATHFINDING_BACKEND_DETOUR`):
  - `DetourNavMap.{h,cpp}` (900 LOC) bakes a tiled navmesh in 64-cell tiles. `BuildTile` runs on job threads, and `AddTile`/`RemoveTile` swap tiles.
  - Queries: findPath + straight path, closest point, and raycast.
  - `ObstacleAvoidance.cpp` wraps `dtCrowd`.
- **Grid/hex A\***: `GridMap.cpp`, `AStarSolver.cpp` (generation-stamped scratch, octile and hex heuristics, slope and step checks), cell reservations for multi-agent spreading, `PathSmoother`, and a parallel batched solve (`AStarSolverPool`, `NavigationWorld::SolveGridPaths`). NavLinks connect maps, with Dijkstra across them.
- **ECS**:
  - `NavigationSurface` (Mesh or Grid mode, agent radius/height/slope/step, cell size, fit bounds, bake state machine and progress).
  - `NavigationAgent` (speed, acceleration, stopping distance, avoidance quality, separation, replan interval).
  - `NavigationSourceCollector.cpp` gathers terrain heights, static collider shapes and mesh renderers.
  - Systems: surface bake, pathfinding, movement and debug draw.
- **Editor**: walkable tint and edge outline in the Scene View, a bake notice, and `SceneView/NavGridBrushTool.cpp` (paints cost/blocked cells into a `.navgrid` asset).

### 1.7 Particles (`Engine/Modules/Particles` 10.3k LOC; `docs/rendering/particles.mdx`)
- **CPU** simulation ("There is no GPU simulation path", per the docs Limits). It is structure-of-arrays (`ParticleChannels.h`, about 50 channels), allocated per declared channel.
- A **processor stack asset** (`.particlestack` as JSON, cooked to binary by `ParticleStackBinary.cpp`). Processors: EmitRate, EmitBurst, Shape, VelocityCone, Acceleration, Drag, LimitSpeed, Noise (curl), Orbit, Collision (plane or physics ray sweep), Event (triggers including game events and sub-emitter spawn), Light, Trail, Property (over-life curves) (`Source/Processors/*`).
- Fixed-tick deterministic simulation with interpolation, seeds, prewarm, simulation speed, 2D/3D, sub-emitters (4 slots), and collision events. Budgets are 4096 per emitter and 65,536 per world.
- Renderer (`Components/Rendering/ParticleRenderer.h`, `Source/Rendering/ParticleExtraction.cpp`): 8 billboard modes, velocity stretch, mesh particles (up to 4), sprite sheet with frame blending, Lit/Unlit, **six-way lightmaps**, emission, ribbon trails, sorting, distance thinning and near fade.
- Editor (`Apps/Editor/Source/Inspectors/Particles/*`): stack editor, parameter fields, and an emitter gizmo. Preview controls are Restart, Pause, Step and time scrub from 0 to 60 s.
- `Engine/Modules/GPUFogParticles` is a textureless fog-particle surface shader (an **Unlicense** port of MirzaBeig/GPU-Fog-Particles).

### 1.8 Noise (`Engine/Modules/Noise`, header-only, ~350 LOC)
- `GradientNoise2D.h` (gradient noise with **analytic derivatives**), `FractalNoise2D.h` (fBM), `GradientCurl3D.h` (divergence-free curl noise from analytic gradients). Golden tests. Depends on their `Types/Types.h` and `Mathematics/Vector3.h` only.

---

## 2. What OURS has

### 2.1 Terrain
- **Component**: `engine/core/component/TerrainComponent.h`, `engine/core/object/Terrain.{h,cpp}`. One heightmap texture (8- or 16-bit, `RED16` for new maps), up to 3 RGB blend maps giving **9 surface layers**. A layer is either legacy colour or full PBR (color/normal/roughness/metallic/AO/height-blend, per-layer UV scale and offset; `terrainLayerFromMaterial` turns a Material into a layer). There are also N foliage layers.
- **LOD**: a **CDLOD quadtree** with vertex morphing (`MeshSystem::createTerrainNode` at `MeshSystem.cpp:2898`, `engine/shaderlib/includes/terrain_vs.glsl` `morphVertex`). Node selection is **per view** (`MAX_TERRAIN_VIEWS 4`: main, mirrors, captures), with a half-resolution node buffer and auto ranges. *The README says "clipmap LOD"; the code is CDLOD. The README is wrong.*
- **Shading**: `engine/shaderlib/includes/terrain_fs.glsl` does triplanar side planes by slope (0.15–0.6), height-based blend bias, and a distance-faded second tiling rate (anti-repetition). There is no hex or stochastic tiling.
- **Physics**: a Jolt HEIGHTFIELD shape from the terrain heightmap (`PhysicsSystem.cpp:1325+`).
- Queries: `MeshSystem::raycastTerrainSurface`, `sampleTerrainSurface` (height and normal), `getTerrainHeightRangeArea`.
- **Not present**: holes, streaming or tiles, multi-terrain stitching, non-destructive modifiers, procedural base source, raw .r16/.r32 import.

### 2.2 Terrain editor (`editor/window/TerrainEditWindow.{h,cpp}` 4.3k LOC, `editor/util/Terrain*`, `editor/command/type/TerrainMapPatchCmd.*`)
- Unity-style tool bar: Sculpt / Texture / Details / Trees.
- **16 brush modes** (`TerrainBrushMode`): Raise, Lower, Smooth, Flatten (pick height), Sharpen, Noise, Terrace, Stamp (grayscale mask relief), Erode, Ramp, PaintBase, PaintLayer, PaintDensity, EraseDensity, PlaceObject, EraseObject.
- Brush settings: circle or square shape, three falloffs, rotation, a grayscale brush mask for every brush, and a time-based flow.
- Strokes accumulate in a float working copy and are quantized only on write-back. Undo is a dirty-region patch (`TerrainMapPatchCmd`).
- **Erosion** (`editor/util/TerrainErosion.cpp`): hydraulic droplets (inertia, capacity, deposit, evaporation, 3×3 spread) and thermal talus, both confined to the brush.
- **Texture paint** with a **slope and height mask** (`paintMinSlope/MaxSlope/MinHeight/MaxHeight`). This is a brush-time cousin of their surface rules.
- **Object placement**: places prefabs or models as entities or instanced meshes with spacing, scale range, rotation jitter and align-to-normal. Erase is undoable.
- **AI/MCP automation**: `editor/mcp/tools/WorldTools.cpp` scripts brush strokes (`TerrainScriptedBrush`) and `set_terrain_maps` generates 16-bit flat/random/fractal height maps (`editor/util/TerrainNoise.h`, value-noise fBM).

### 2.3 Foliage/grass (`MeshSystem.cpp:3060–3590`)
- `TerrainFoliageLayer` has a mesh path, a painted density map, density per m², scale range, yaw jitter, align-to-normal, slope and height windows, draw distance, seed and castShadows.
- **CPU deterministic scatter**: 8 m world cells hashed by (seed, cell, index), so placement is stable as the camera moves. Chunks (`InstancedMeshComponent`, ≤8192 instances) sit on a **toroidal ring** around the camera (`foliageSlotIndex`), so moving refills only the columns that left. Distance shrink-fade starts at 75% of draw distance. Mesh loads are budgeted to 2 per frame. There is a whole-terrain preview mode.
- Shader `engine/shaderlib/library/foliage.{vert,frag,glsl}`: wind sway (direction, gusts, flutter; masked by vertex colour or height), alpha cutout, translucency, and depth/shadow variants that sway with the plant.
- **Not present**: procedural blade grass, clumping, GPU culling, LOD blades, interactive bending.

### 2.4 Water
- `engine/shaderlib/library/water.{frag,vert,glsl}`, `water_vs.glsl`, `water_depth.vert`. Stylised water: fresnel shallow-to-deep colour, two scrolling normal layers, world-anchored foam, **2 sine waves** (`waterWaves`, with analytic slope), and optional DEPTH_FOAM, ABSORPTION and REFRACTION (scene depth and colour). Planar reflections are possible through `MirrorComponent`.
- **Not present**: ocean sim, FFT, Gerstner sets, LOD water mesh, buoyancy, underwater, rivers or splines, caustics.

### 2.5 Particles
- `engine/core/component/ParticleSystemComponent.h`, `engine/core/action/ParticleSystem.{h,cpp}`, simulation in `engine/core/subsystem/ActionSystem.cpp` (`applyParticleInitializers/Modifiers`). This is the Doriax heritage design.
- **CPU, AoS** (`ParticleData`). Fixed initializers (life, position shape Box/Sphere/Hemisphere/Circle/Cone, velocity, acceleration, colour, alpha, size, sprite frames, rotation, scale) and **one linear from→to modifier per property** with easing, plus a colour gradient. Also rate, bursts, loop, local space, and maxPerUpdate.
- Renders as **Points** (`PointsComponent`, sorted) or **InstancedMesh**.
- Shader `engine/shaderlib/library/particle.frag` has glow (alpha as light), noise erosion dissolve, **soft particles**, and near fade.
- **Not present**: noise or turbulence forces, drag, collision, sub-emitters, events, trails, lights, velocity stretch, billboard modes, six-way lighting, deterministic fixed tick, reusable effect assets. The inspector edits fields only (`InspectorWindow.cpp:5435+` handles frames, bursts and gradient).

### 2.6 Splines, navigation, trees, noise
- **Splines: none.** The only cubic splines are glTF animation `CUBICSPLINE` sampling. `LineRendererComponent` draws line segments only.
- **Navmesh, pathfinding, agents: none** (grep finds only UI gamepad navigation).
- **Procedural trees: none.**
- **Noise**: `editor/util/TerrainNoise.h` (value noise, editor only), plus HTML-UI shader effects (`engine/shaderlib/effects/{perlin,simplex,curl,fractal}-noise.frag`), which are not world-side.
- GPU: our sokol-gfx (`engine/libs/sokol/sokol_gfx.h`) **has compute and storage buffers**. `SokolBuffer.cpp` already creates storage-buffer views, but **nothing dispatches compute today**. sokol has no indirect draw and no bindless. GLES3 and WebGL2 (Android, iOS, HTML5 exports) have no compute. We build as C++17 by default (`engine/CMakeLists.txt:5`). THEIRS is C++20.

---

## 3. Gaps: THEIRS has, OURS lacks

| Feature | Their location | Value | Effort |
|---|---|---|---|
| Spline core (CatmullRom/Bezier/Linear, arc length, closest point, signed distance, XZ queries, roll/radius) | `Engine/Modules/Spline/*` | **H** | **S** |
| Spline editor tool (draw stroke → simplify/smooth/snap, extend, handles, marquee) | `Apps/Editor/Source/SceneView/SplineTool.cpp`, `SplineSceneGizmo.cpp` | **H** | M |
| Spline mesh extrude (road crown, path bevel, world-metre UVs) | `SplineGeometry/SplineStripBuilder.*`, `SplineProfile.*`, `SplineCorner.*`, `Components/Spline/SplineExtrude.h` | **H** | M |
| Spline object placement (tiles, conform to ground, jitter, dropout) | `SplineLayout/TileLayout.*`, `Components/Spline/SplinePlacement.h`, `Apps/Editor/Source/Placement/SplinePlacementController.cpp` | H | M |
| Spline walls and fences (posts and spans, mitre, gates, grade) | `SplineLayout/FenceLayout.cpp`, `SplineWall.h`, `SplineFence.h`, `Placement/SplineFence*` | M | L |
| Spline → terrain (flatten road bed, paint along a path) | `TerrainModifierVolume.h` (SplinePath/SplineArea), `TerrainRoutePolyline.*` | **H** | M (as a brush or bake in our editor) |
| Navmesh bake (Recast, tiled, threaded) + Detour queries | `Pathfinding/Source/DetourNavMap.cpp`, `PathfindingECS/Source/NavigationSourceCollector.cpp`, `NavigationMeshBake.cpp` | **H** | M |
| Crowd/avoidance (dtCrowd), agent component, movement system | `Pathfinding/Source/ObstacleAvoidance.cpp`, `PathfindingECS/Systems/NavigationMovementSystem.cpp` | H | M |
| Grid/hex A\* with costs and reservations, nav grid brush | `Pathfinding/Source/GridMap.cpp`, `AStarSolver.cpp`, `SceneView/NavGridBrushTool.cpp` | M (strategy/2D games) | S–M |
| Particle forces: curl noise, drag, limit speed, orbit | `Particles/Source/Processors/*`, `Noise/GradientCurl3D.h` | **H** | S–M |
| Particle collision (plane, physics) + collision events | `ParticleCollisionProcessor.cpp`, `ParticleSimulationSystem.cpp` | M | M |
| Particle trails/ribbons | `ParticleTrailProcessor.cpp`, `ParticleTrailStore.*`, `ParticleExtraction.cpp` | M | M |
| Sub-emitters and particle events | `ParticleEventProcessor.cpp`, `Components/Rendering/Particles.h` | M | M |
| Billboard modes, velocity stretch, sprite frame blending, six-way lighting | `ParticleRenderer.h`, `ParticleExtraction.cpp`, `ParticleMaterials.cpp` | M–H | M |
| Particle effect as an asset (stack), deterministic fixed tick, prewarm, scrub preview | `ParticleStackDocument.*`, `ParticleRuntime.cpp`, `Inspectors/Particles/*` | M | L (would redesign our component) |
| Procedural tree generator | `Packages/eztree/Native/EZTree/*` | **H** | S–M |
| Ocean FFT (multi-cascade), foam, flow, dynamic waves, underwater, caustics | `Engine/Modules/Ocean/*`, `Rendering/Shaders/Ocean/*` | M (H for ocean games) | **XL** |
| Gerstner/analytic waves mirrored on the CPU + buoyancy | `Ocean/Source/Systems/OceanBuoyancySystem.cpp`, `OceanCollisionProvider.cpp` | **H** | S |
| Concentric LOD water mesh (Crest rings) | `Ocean/Source/OceanRenderFeature.cpp:770+` | M | S–M |
| River/lake water mesh from a spline (FitToBanks fill) | `SplineGeometry/SplineFillField.*`, `SplineFillMesher.*` | M | M |
| Hex tiling (anti-repetition) for terrain layers | `Rendering/Shaders/Includes/terrain_material_albedo.glsl:203-316` | M | **S** |
| Planar (orthophoto) layer projection | same file | L | S |
| Surface rules (procedural splat by slope/height/noise, non-destructive) | `Components/Terrain/TerrainSurfaceRules.h`, `TerrainECS/TerrainSurfaceRuleEval.h` | M (we already have masked paint) | M |
| Non-destructive terrain modifier volumes and stacks | `TerrainModifierVolume.h`, `TerrainModifierEffects.h`, `TerrainModifierSystem.cpp` | M | XL |
| Erosion *filter* inside procedural noise (analytic gullies) | `TerrainECS/Source/Erosion/ErosionFilter.cpp` (**MPL-2.0**) | M | S |
| Gradient noise with analytic derivatives, curl 3D (shared engine noise) | `Engine/Modules/Noise/*` | M | **S** |
| Raw DEM import (.r16/.r32, Samples X/Z validation) | `TerrainECS/Source/RawHeightmap.cpp`, `Apps/Editor/Source/Terrain/TerrainHeightmapInspector.cpp` | M | S |
| Large terrain tiling, streaming, page cooking | `TileStreamingManager.cpp`, `TerrainAtlas.cpp`, `TerrainHeightPages.cpp` | L–M (we target mobile/web) | XL |
| GPU procedural blade grass (clumps, LOD, wind volumes) | `Engine/Modules/TerrainGrass/*` | M | XL as is (L for a reduced port) |
| CBT/LEB terrain and planets | `Engine/Modules/CBTTerrain*` | L | XL (not feasible on our backends) |
| Heightfield terrain sun shadows | `CBTTerrainECS/Source/TerrainShadowBake.cpp` | L | M |
| Fog particles (textureless) | `GPUFogParticles/Shaders/Surfaces/gpu_fog_particles.glsl` (Unlicense) | L–M | S |

---

## 4. Things THEIRS does better, and what OURS does better

### THEIRS is better at
1. **Splines as infrastructure.** One curve model feeds terrain modifiers, ocean inputs, extrusion, walls, fences and tile placement. Each consumer chooses its XZ or 3D query (`SplineEvaluator.h`), and per-point radius and roll are built in. We have nothing comparable. This is our largest missing world-building feature.
2. **Navigation.** A production Recast/Detour integration with tiled, threaded bakes, crowd, nav links and a grid/hex alternative. It bakes from terrain, static colliders and meshes without a physics world (`NavigationSourceCollector.cpp`). We have zero navigation.
3. **Particle architecture.** A processor stack over SoA channels (`ParticleChannels.h`) is extensible. Our single from/to modifier per property cannot express curves, forces or multi-phase behaviour. They also have fixed-tick determinism with interpolation (`ParticleRuntime.cpp`), trails, sub-emitters, collisions, and six-way smoke lighting.
4. **Ocean fidelity.** A faithful Crest-class ocean: 16-cascade FFT, foam from the Jacobian, flow, interactive dynamic waves, underwater, and buoyancy with GPU readback and CPU fallback. Our water is a material.
5. **Terrain scale and non-destructive authoring.** DEM import to 60×15 km with paging. The modifier volume and effect stack with priorities, pools and claims can be re-edited at any time and drives roads (SplinePath flatten). Surface rules paint procedurally and stay live.
6. **Grass quality and scale.** GPU placement is deterministic and world-anchored, with clumping, growth fades in place of popping, and splat-coloured roots. Their density targets (21 to 256 blades/m² near the camera) are out of reach for CPU instancing.
7. **Test discipline.** Every module has golden or CPU-mirror tests (`*Tests.cpp` are about 40–50% of LOC). For example, the grass CPU model is pinned against its GLSL mirror.

### OURS is better at
1. **Interactive sculpting UX.** We have 16 brush modes (smooth, flatten-pick, sharpen, terrace, ramp, stamp with mask, hydraulic and thermal erosion, slope/height-masked paint, density paint, object place/erase). Their `TerrainBrushTool.cpp` has **only Raise, Lower and Paint**. Our hydraulic and thermal droplet erosion (`editor/util/TerrainErosion.cpp`) is a real simulation; theirs is an analytic noise filter. Our float working buffer plus dirty-region undo is solid.
2. **Portability.** Our terrain, foliage, water and particles run on GL/GLES, WebGL2, Metal, D3D11 and Vulkan through sokol, with no compute. Their CBT terrain, grass, ocean and fog all need compute and SSBOs, and CBT and grass also need bindless and int64 or indirect-count. On a device without int64, their CBT terrain shuts off entirely (`CBTLayout.h`).
3. **Terrain layer count and PBR per layer.** We have 9 layers with full PBR maps, height blend and per-layer UV transforms. They have **4 splat roles** (grass/rock/dirt/snow) resolved through a material library.
4. **Per-view CDLOD for mirrors and captures** (`TerrainView views[MAX_TERRAIN_VIEWS]`). Each RTT camera gets its own LOD cut.
5. **Foliage that is mesh-agnostic and cheap.** Any glTF or OBJ becomes painted, density-mapped, deterministic, ring-chunked instanced scatter (`MeshSystem.cpp:3350+`), with wind and shadow sway in `library/foliage`. Their grass is blades or cards only. Arbitrary-mesh scatter on their side is SplinePlacement or EZ-Tree, not terrain painting.
6. **AI-agent tooling for terrain.** MCP tools run scripted brush strokes and generate maps (`WorldTools.cpp`). They have a debug port with `set_component` and stroke handlers (`DebugServer/TerrainDebugHandlers.cpp`). The two are comparable; ours is a richer brush API.
7. **Particle shader niceties.** Soft (scene-depth) particles, near fade, noise-erosion dissolve and glow on mobile-cost budgets (`library/particle.frag`). THEIRS has near fade but no depth-based soft particles anywhere in `Particles/` or `Rendering/Shaders` (grep found none, and `GPUFogParticles/.../UPSTREAM.md` says soft fade was dropped because surface shaders cannot read scene depth). They lead on everything else in particles.

---

## 5. Copy/port candidates (ranked by value/effort)

| # | Item | Source paths | Drags in | GPU API impact | Difficulty | License |
|---|---|---|---|---|---|---|
| 1 | **Spline core** | `Engine/Modules/Spline/Include/Spline/{SplineTypes,SplineData,SplineEvaluator,SplineUtility}.h`, `Source/{SplineData,SplineEvaluator}.cpp`, tests in `Tests/` | their `Mathematics/{Vector3,BezierCurve,Geometry,Interpolation,VectorOps}`, `Types/StringId` (channel names). Swap for our `math/Vector3` and inline the Bezier/CR basis | none (CPU) | **S** (~1k LOC; C++20 bits are trivial) | MIT |
| 2 | **Recast/Detour wrapper + grid A\*** | `Engine/Modules/Pathfinding/{Include,Source}/*` (DetourNavMap, ObstacleAvoidance, GridMap, AStarSolver, PathBuffer, PathSmoother, HexGridHelpers, NavigationWorld). Use `PathfindingECS/Source/NavigationSourceCollector.cpp` and `NavigationMeshBake.cpp` as a reference only (heavily ECS-bound) | **recastnavigation** (zlib; add to `libs/`), their Logger/JobSystem (replace with our `thread/` pool or std::async). Bind to Lua and add a `NavMeshSurfaceComponent` + `NavMeshAgentComponent` | none; debug draw through our `LineRendererComponent` | **M** | MIT + zlib |
| 3 | **EZ-Tree generator** | `Packages/eztree/Native/EZTree/{EZTreeGenerator,EZTreeOptions,EZTreePresets,EZTreeRng}.*`, wind `Assets/Shaders/VertexModifiers/ez_tree_wind.glsl`, leaf `Assets/Shaders/Surfaces/ez_tree_leaves.glsl`, textures `Assets/Textures/EZTree/*` | glm (a header-only dependency, or rewrite to our math), their `ModelAsset` output (map to our `Mesh` buffers), upstream preset JSONs (fetch from github.com/dgreenheck/ez-tree at commit `28c16503`) | shaders: re-express as a `library/` mesh shader. Our `foliage` wind can be reused instead | **S–M** | MIT (Greenheck) + MIT; keep both notices |
| 4 | **Noise module + curl noise; particle Noise/Drag/LimitSpeed/Orbit processors as new modifiers** | `Engine/Modules/Noise/Include/Noise/*.h`; logic from `Particles/Source/Processors/{ParticleNoiseProcessor,ParticleDragProcessor,ParticleLimitSpeedProcessor,ParticleOrbitProcessor,ParticleVelocityConeProcessor}.cpp` | Types/Vector3 only for Noise. The processors depend on their `ParticleProcessorContext`/channels, so lift the maths into `ActionSystem::applyParticleModifiers` | none | **S** | MIT |
| 5 | **Buoyancy + CPU-mirrored waves** | `Ocean/Source/Systems/OceanBuoyancySystem.cpp` (4-probe force at position, relative drag); the Gerstner sum from `OceanTypes.h` (`GerstnerWave`, std430) | our Jolt `PhysicsSystem` (AddForce/AddForceAtPosition/AddTorque). Mirror our `library/water.glsl::waterWaves` (or a new N-Gerstner set) in C++ | none for the CPU path | **S** | MIT (Crest-derived ocean; the buoyancy code itself is native) |
| 6 | **Hex tiling** for terrain layers (and any tiling material) | `Engine/Modules/Rendering/Shaders/Includes/terrain_material_albedo.glsl:203-316` (`CBT_HexTriangleGrid`, `CBT_HexHash`, `CBT_HexRot`, `CBT_HexTapUV`) + blend weights | none. Pure GLSL math. Swap their `GE_BTEX` bindless taps for our `texture(sampler2DArray(...))` in `includes/terrain_fs.glsl` | sokol-shdc / GLSL ES 3.0 OK (ivec2 math) | **S** (costs about 3× texture taps per layer; gate it behind a feature) | MIT; the algorithm is Mikkelsen JCGT 2022 |
| 7 | **SplineGeometry strip builder + profiles + corners** (roads, paths, walls) | `Engine/Modules/SplineGeometry/{Include,Source}/{SplineStripBuilder,SplineProfile,SplineCorner,SplineEndTaper,SplineStation,SplineVertex,SplineStep}.*` | Types, Mathematics; **C++20 (`std::span`, defaulted `operator==`)**. Use a span shim or raise us to C++20 (we already allow it: `JASPERENGINE_CXX_STANDARDS 17 20 23`). Needs candidate 1 | output feeds our `Mesh`/`MeshComponent` | **M** | MIT |
| 8 | **Spline tool UX** (freehand draw → Douglas-Peucker → smooth → surface snap; extend; handles) | `Apps/Editor/Source/SceneView/SplineTool.cpp` (`FinalizeSpline`, `SimplifyPolyline`, `SmoothPolyline`, `SnapPointsToSurface`, `TryExtendExistingSpline`) | their gizmo and undo framework, so **reimplement** on our ImGui SceneWindow and command system; lift only the algorithms | none | **M** | MIT |
| 9 | **Tile/fence layout along a spline** | `Engine/Modules/SplineLayout/Source/{TileLayout,FenceLayout,SpanMitre,SeamShear,CenterlineSampling}.cpp` | SplineGeometry, their AssetCore/ECSComponents (only for piece refs; stub them) | none | M–L (FenceLayout is 2k LOC; worth it for fences/rails) | MIT |
| 10 | **Erosion filter** (analytic gullies in our fractal generator/noise brush) | `Engine/Modules/TerrainECS/{Include,Source}/Erosion/ErosionFilter.*` + needs `Noise/GradientNoise2D.h` (derivatives) | Noise module | none (CPU); a GLSL twin is possible later | **S** | **MPL-2.0, file-scoped**: keep those 2 files under MPL with SPDX headers, ship the license text (`ThirdParty/RunevisionErosionFilter/LICENSE-MPL-2.0.txt`), publish modifications to those files |
| 11 | **Crest concentric ring water mesh** | `Ocean/Source/OceanRenderFeature.cpp` `CreateGridMesh` (~line 770 on) + snapping in `ocean_vertex_modifier.glsl` | none (CPU mesh build). Our `water.vert` would need LOD morph and snapping uniforms | vertex-shader only. Works on GLES/WebGL | **S–M** | MIT, Crest attribution required (`Ocean/NOTICE`) |
| 12 | Particle trails + billboard modes + velocity stretch | `Particles/Source/ParticleTrailStore.cpp`, `Processors/ParticleTrailProcessor.cpp`, `Rendering/ParticleExtraction.cpp` (billboard/stretch math) | trail ribbon needs a dynamic-VB ribbon renderer on our side (`LineRendererComponent` is a start) | CPU-built vertex buffers. Fine on sokol | M | MIT |
| 13 | GPU fog particle shader | `Engine/Modules/GPUFogParticles/Shaders/Surfaces/gpu_fog_particles.glsl` | none; adapt to a `library/` points shader (we already have scene depth for soft fade, which they lack) | fragment only | **S** | Unlicense (upstream) + MIT |
| 14 | Raw DEM import (.r16/.r32 + grid inference/validation) | `Engine/Modules/TerrainECS/Source/RawHeightmap.cpp`, `Apps/Editor/Source/Terrain/RawHeightmapSettingsCommit.cpp` | none (CPU); convert into our RED16 heightmap texture | none | **S** | MIT |
| 15 | Surface-rule evaluator (offline "auto-paint" button over our blend maps) | `Engine/Include/Components/Terrain/TerrainSurfaceRules.h`, `TerrainECS/Include/TerrainECS/TerrainSurfaceRuleEval.h`, `TerrainRuleNoise.h` | none; run on the CPU into our blend maps (destructive bake, which fits our model) | none | S–M | MIT |

Notes on GPU dependencies:
- Anything from THEIRS ending in `.comp`, and anything using `GE_BTEX`, `nonuniformEXT`, `DrawIndirectCount` or `shaderInt64`, cannot run on our GLES3/WebGL2 exports. Our sokol has compute (`sg_dispatch`, storage buffers) only on GL 4.3, Metal, D3D11, Vulkan and WebGPU, and has **no indirect draw**. A reduced grass or FFT-ocean port would therefore need either (a) a desktop-only compute tier plus a fallback, or (b) a fragment-shader ping-pong rewrite (a classic FFT ocean is doable that way).
- Their CPU-mirror headers are the most portable assets in the repo, because the CPU side already exists by contract: `GrassPlacementModel.h`, `GrassClumpModel.h`, the `CBTPlanetShading.h` fBM.

---

## 6. Not worth copying (and why)

- **CBT/LEB terrain** (`Engine/Modules/CBTTerrain*`, 50k LOC). It requires compute, int64, SSBOs, indirect and bindless, so it is infeasible on sokol/GLES/WebGL. Our CDLOD with per-view selection is adequate for our target scale. Planets are out of scope.
- **Terrain tile streaming, atlas, page cooking** (`TileStreamingManager`, `TerrainAtlas`, `TerrainHeightPages`, `AtlasResidencyController`). These are tightly coupled to their bindless atlas and CBT and sized for 10+ km DEMs. Our mobile/web-first targets rarely need them. If we need larger worlds, a simpler N×M grid of our existing Terrain entities with edge stitching is cheaper.
- **The full non-destructive modifier system** (`TerrainModifierSystem.cpp` 6.3k LOC plus scene schemas and the page overlay). It is architecturally incompatible with our destructive, map-based brush model and undo. Take the *ideas* instead: a spline-flatten brush or bake, a surface-rule auto-paint pass, and an erosion filter in the generator.
- **The GPU grass module as is** (`TerrainGrass`). It is bindless, compute and indirect-count bound, and its maths is mirrored against those kernels. If we want blades, write a CPU-placed (reusing our deterministic ring-chunk scatter), vertex-procedural blade shader that borrows `grass_clump.glsl` and the wind formulas. Do not port the pipeline.
- **The full Crest-class ocean** (27k LOC + 45 compute shaders). XL effort, compute-only, and a niche for our audience. Take buoyancy, CPU waves and the ring mesh (#5 and #11), and maybe the foam/normal textures (`crest_foam.png`, `crest_wave_normals.png`, MIT, with attribution).
- **Particle stack asset system and its editor** (`ParticleStackDocument/Binary/Compiler/Validation`, `Inspectors/Particles/*`). A large redesign of our component, its serialization (`editor/Stream.cpp`), Lua bindings and inspector. Extend our initializer/modifier model with new modifiers instead (#4, #12), and add curve-based over-life properties later.
- **Spline placement controllers** (`Apps/Editor/Source/Placement/*`). These are editor glue bound to their ECS chunk-entity, rebuild-gate and undo framework. Re-implement them on our Factory/command system using the lifted SplineLayout algorithms.
- **Their terrain brush tool** (`TerrainBrushTool.cpp`). Strictly weaker than ours.
- **PathfindingECS systems verbatim**. These depend on their World/Query/JobSystem/PhysicsECS. Rewrite them as our subsystems, keeping only the source-collection logic (terrain lattice, static colliders, meshes) as a reference.
