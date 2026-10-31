# Particles: Unity-style modules and a Unity ParticleSystem importer (engine gaps #22, `gaps/particles`)

Status: binding design. Read `docs/engine-gaps/AGENT-BRIEF.md` first: its shared conventions are binding.
Branch `gaps/particles`. Uses the shared `AnimationCurve` / `Gradient` types and Inspector widgets defined in
`2026-10-31-lines-trails-design.md` §6.1 (whichever branch lands first adds them).

## 1 Goals

1. A particle system with **Unity's module model and semantics**: a Main module (duration, looping, prewarm, start
   delay, start lifetime/speed/size (3D)/rotation (3D)/color, gravity modifier, simulation space and speed, scaling
   mode, max particles, play on awake, stop action, random seed), Emission (rate over time, **rate over distance**,
   bursts with cycles/interval/probability), Shape (sphere, hemisphere, cone, cone volume, box/shell/edge, circle,
   edge, rectangle, donut; radius thickness, arc with modes, position/rotation/scale, align to direction, randomize
   direction/position, spherize), and every over-lifetime module the game needs: velocity (+ orbital/radial/speed
   modifier), limit velocity (dampen, drag), inherit velocity, force, color, color by speed, size (separate axes),
   size by speed, rotation (separate axes), rotation by speed, **noise**, texture sheet animation.
2. **Start speed along the emitter shape's direction** (today velocity is a min/max box,
   `engine/core/subsystem/ActionSystem.cpp:926-928`).
3. **Multi-key curves and random-between-two-curves** (`MinMaxCurve`) and the five `MinMaxGradient` modes for every
   value Unity allows (today modifiers are single from/to segments except colour, `ActionSystem.cpp:1019-1110`).
4. A renderer with **render modes** billboard, stretched billboard, horizontal, vertical, mesh (with render alignment),
   **sorting** (none, by distance, oldest/youngest in front, sorting fudge), min/max particle size, pivot, flip.
5. A **Unity ParticleSystem importer**: reads `.prefab`/`.unity` YAML (`ParticleSystem` `!u!198`,
   `ParticleSystemRenderer` `!u!199`, prefab instance overrides), produces an equivalent engine prefab (systems,
   child systems, materials, textures, meshes), with a report of what is unsupported or approximated. Available as
   **Assets > Import Unity Particle System...**, a **CLI** and an **MCP tool**. Coverage driven by the 18 systems of
   the game (§4.1), all of which import with no "unsupported" item except the UI-particle and custom-shader notes.
6. **Old scenes load and play identically** (the current initializer/modifier model keeps its exact code path); an
   explicit, undoable upgrade converts them to modules.
7. Inspector, Lua API (Unity names), MCP, docs, exported builds on every platform, editor Play and the Scene view
   preview.

## 2 Non-goals

- Sub emitters, collision, triggers, lights, the Trails module, custom data, external forces (wind zones), custom
  vertex streams, ring buffer mode, custom simulation space, emission from mesh/skinned mesh/sprite/texture shapes,
  GPU simulation, `cullingMode` (systems always simulate; documented), `ParticleSystem.GetParticles/SetParticles`
  (a later step). The importer reports each of these when enabled (none is enabled in the game's 18 systems).
- Particles drawn inside UI canvases (Coffee UIParticle in the GameOver confetti): feature `ui-scaling-text` (#21).
  The importer imports the system and reports `ui-particle` so #21 can attach it.
- Porting Unity custom shaders (Amplify/WFX): the importer maps known built-in particle shaders and approximates or
  reports the others.

## 3 Users and stories

- **Unity porter**: "Assets > Import Unity Particle System..., pick `WFXMR_FlameThrower Looped.prefab` in the Unity
  project; I get `assets/fx/WFXMR_FlameThrower Looped.prefab` with its texture and material, and a report saying the
  WFX additive shader was mapped to unlit additive. I drop it on the jetpack and set `emission.rateOverTime` from Lua
  as the C# did."
- **Effects artist (new)**: GameObject > Effects > Particle System; edits modules in the Inspector exactly like
  Unity (foldouts with enable checkboxes, curve and gradient fields, "Random Between Two Constants" dropdowns), sees
  the effect live in the Scene view with Play/Pause/Restart and a time readout.
- **Gameplay scripter**: `ps.emission.rateOverTime = 0`, `ps:play()`, `ps:emit(10)`, `ps.main.startColor =
  MinMaxGradient.color(c)`, `ps.isPlaying`, `ps.particleCount`; `onParticleSystemStopped` on its script.
- **Existing project**: old fire/snow systems made with `create_particles` look the same after the update; the
  Inspector offers "Upgrade to modules" with a report.
- **AI agent**: `import_unity_particles {path, dest}` -> report JSON; `create_particles preset=fire` makes a
  module-based system; `set_properties` on `main.startLifetime = {"mode": "two_constants", "min": 0.8, "max": 1}`.

## 4 Research summary

### 4.1 The game's 18 systems (read from the game's YAML, `/tmp/gamesrc/Minigames`)

| # | Where | Notable |
|---|---|---|
| 1 | A Player/PlayerAppereance | Rectangle shape rot (-35,0,0), **rate over distance 5**, force (0,-5,0) world, start colour two colours, simulationSpeed 2, world space |
| 2 | A ExplosionParticles | duration 0.1, **start delay 0.1**, sphere randomDirectionAmount 1, **size two curves x6.92**, rotation two constants, colour 4-key gradient, **velocity two curves** x(7,7,10), limit velocity curve x8 dampen 0.1, **rotation by speed**, **Mesh render mode (Cube), alignment Local** |
| 3,4 | B Start/EndParticles | sphere thickness 1, **texture sheet 2x2 FPS 25**, additive, rate set by script |
| 5,6 | D Eyes | bursts of 1, life 30, **size3D** (x/y/z two constants), `sortingOrder -1` |
| 7-9 | D Rush/Ambush | **prewarm**, simulationSpeed 5, **hemisphere alignToDirection**, rotation separate axes, **Mesh render mode (lowpolySphere.fbx)**, custom opaque fresnel shader, rate over distance 0.2 |
| 10 | WarFX flamethrower | shape disabled, speed 0, size curve, rotation const, 4-key colour/3-key alpha, velocity two constants local, **force two constants world**, **limit velocity curve x20 dampen 1**, max particle size 0.5, Legacy Particles/Additive; scene overrides (prewarm, startSize, size curve key, rate) |
| 11 | H CollectedParticles | **Circle**, burst 10, playOnAwake 0, `Play()` from script |
| 12 | GameOverMenu confetti | Circle, burst 10, drawn in UI by UIParticle (scale 100) |
| 13 | I InvadersExplosion root | shape disabled, **VerticalBillboard**, **sort by distance, fudge 10**, bursts at 0 and 0.1, autodestruct script |
| 14 | I Explosion | **start colour gradient (mode 1)**, **colour two gradients (mode 3)**, velocity two curves world, limit velocity, fudge -10, multiplicative smoke shader |
| 15-18 | I FirePoints | **Cone** angle 64.5 radius ~0, burst 10, playOnAwake 0 |

Usage counts: MinMaxCurve modes constant (everywhere), two constants (start values, rotation, force, velocity),
curve (size, limit velocity, frame over time), two curves (size, velocity); gradient modes colour, two colours,
gradient, two gradients; render modes billboard 13, mesh 4, vertical 1; shapes sphere 6, cone 4, hemisphere 3,
circle 2, rectangle 1, disabled 2. All keys non-weighted, wrap clamp, tangent modes 0/1/34 with explicit slopes.
Scripts use `emission.rateOverTime =`, legacy `emissionRate =`, `Play()`, `SetActive`, `GetComponentInChildren`.

### 4.2 Unity (the reference semantics)

Manual "Particle System modules" (<https://docs.unity3d.com/Manual/ParticleSystemModules.html>) and the scripting
API (`ParticleSystem.MainModule`, `EmissionModule`, `ShapeModule`, `MinMaxCurve`, `MinMaxGradient`,
`ParticleSystemRenderer`). YAML forms (all observed in the game files): `ParticleSystem` serializedVersion 8
(`lengthInSec`, `looping`, `prewarm`, `startDelay`, `moveWithTransform` = simulation space 0 local/1 world,
`scalingMode` 0 hierarchy/1 local/2 shape, `simulationSpeed`, `playOnAwake`, `stopAction`, `autoRandomSeed`,
`randomSeed`), `InitialModule` (v3: `startLifetime`, `startSpeed`, `startColor`, `startSize`+`startSizeY/Z` with
`size3D`, `startRotation` (Z) + `startRotationX/Y` with `rotation3D`, `randomizeRotationDirection`,
`gravityModifier`, `maxNumParticles`), `ShapeModule` (v6: `type` 0 sphere, 2 hemisphere, 4 cone, 5 box,
8 cone volume, 10 circle, 12 edge, 15 box shell, 16 box edge, 17 donut, 18 rectangle; `radius`/`arc` as
`{value, mode, spread, speed}`; `radiusThickness`, `angle`, `length`, `boxThickness`, `donutRadius`, `m_Position`,
`m_Rotation` (Euler degrees), `m_Scale`, `alignToDirection`, `randomDirectionAmount`, `sphericalDirectionAmount`,
`randomPositionAmount`; cone `emitFrom` is encoded in `type` 4 base / 8 volume), `EmissionModule` (v4:
`rateOverTime`, `rateOverDistance`, `m_Bursts[] {time, countCurve, cycleCount, repeatInterval, probability}`),
the modules `ColorModule.gradient`, `SizeModule.curve/y/z + separateAxes`, `RotationModule.x/y/curve +
separateAxes`, `VelocityModule.x/y/z, orbitalX/Y/Z, orbitalOffsetX/Y/Z, radial, speedModifier, inWorldSpace`,
`ClampVelocityModule.x/y/z, magnitude, separateAxis, inWorldSpace, dampen, drag, multiplyDragByParticleSize/
Velocity`, `ForceModule.x/y/z, inWorldSpace, randomizePerFrame`, `UVModule` (v2: `mode`, `timeMode` 0 lifetime/
1 speed/2 FPS, `fps`, `frameOverTime`, `startFrame`, `speedRange`, `tilesX/Y`, `animationType` 0 whole sheet/1
single row, `rowMode`, `rowIndex`, `cycles`, `flipU/V`), `NoiseModule`, `InheritVelocityModule`,
`SizeBySpeedModule`, `RotationBySpeedModule`, `ColorBySpeedModule`. `MinMaxCurve {minMaxState 0 constant, 1 curve,
2 two curves, 3 two constants; scalar (constant / max / multiplier), minScalar (two constants), maxCurve, minCurve}`;
keys `{time, value, inSlope, outSlope, tangentMode, weightedMode, inWeight, outWeight}`. `MinMaxGradient
{minMaxState 0 color, 1 gradient, 2 two colors, 3 two gradients, 4 random color; minColor, maxColor, maxGradient,
minGradient}`; `Gradient {key0..7 (rgba), ctime0..7, atime0..7 in 0..65535, m_Mode, m_NumColorKeys,
m_NumAlphaKeys}`. Renderer `!u!199` v6: `m_RenderMode` 0 billboard, 1 stretch, 2 horizontal, 3 vertical, 4 mesh;
`m_SortMode` 0 none, 1 distance, 2 oldest in front, 3 youngest in front; `m_SortingFudge`, `m_MinParticleSize`,
`m_MaxParticleSize`, `m_CameraVelocityScale`, `m_VelocityScale`, `m_LengthScale`, `m_RenderAlignment` 0 view,
1 world, 2 local, 3 facing, 4 velocity; `m_Pivot`, `m_Flip`, `m_Mesh`, `m_Materials`, `m_SortingOrder`.
Quirks to handle: angles in radians; inactive min/max curves hold stale data; `m_VertexStreams: 0001030405` must be
read as a string (a YAML 1.1 parser reads it as an octal int); prefab overrides use property paths like
`InitialModule.startSize.scalar` and `SizeModule.curve.maxCurve.m_Curve.Array.data[0].value`; `stripped`
documents; built-in assets use guid `0000000000000000f000000000000000` (resources) /
`...e000...` (meshes) with fileIDs from Unity's built-in resource table (meshes 10202 Cube, 10206 Cylinder, 10207
Sphere, 10208 Capsule, 10209 Plane, 10210 Quad; materials 10301 Default-Particle, 10306 Default-Line, 10308
Default-ParticleSystem; shaders 200 Legacy Particles/Additive, 10720 Mobile/Particles/Additive, 10721
Mobile/Particles/Alpha Blended). These ids are well known but were not verifiable from the game repository: the
importer keeps them in one table with a comment, and an unknown built-in id is reported, never guessed.
**Take**: module names, field names, value semantics, evaluation rules. **Leave**: the non-goal modules.

### 4.3 Others

- Godot 4 `GPUParticles3D`/`CPUParticles3D` with `ParticleProcessMaterial` (emission shapes, curves as
  `CurveTexture`, turbulence noise, `CPUParticles3D.convert_from_particles`). **Take**: a CPU path that works on
  GLES3/WebGL (this engine's target), turbulence-as-noise idea. **Leave**: GPU simulation (no compute on WebGL2).
- Unreal Niagara (modules stack, emitter/particle scripts): **leave** (too general); the Unity module set is what
  users bring.
- Unity's own Noise: curl-like 3D noise with strength/frequency/scroll/damping/octaves/quality and remapping.
  **Take** the parameters with our own defined algorithm (§6.3.9); exact Unity noise values are not reproducible.

## 5 Product decisions

1. **Versioned component**: `ParticleSystemComponent.version` 1 = the legacy model (initializers/modifiers, today's
   code path, unchanged, `rand()`), 2 = modules. A file without `version` decodes as 1, so every old scene plays
   identically. New systems (menu, `create_particles`, importer) are version 2. The Lua constructor
   `ParticleSystem(scene)` stays version 1 unless `ParticleSystem(scene, true)` or `ps.version = 2` is used, so old scripts that configure systems through the legacy setters keep their exact behaviour.
   An undoable **Upgrade to modules** (Inspector button, MCP `create_particles {entity, upgrade: true}`, Lua
   `ps:upgrade()`) converts 1 -> 2: exact for life, shapes, velocity/acceleration boxes, colour, alpha, size,
   rotation, scale initializers and bursts (§6.7); modifiers (absolute-time segments with easing) become curves,
   exact when the lifetime is constant, otherwise approximated; the conversion returns a report.
2. **Single GameObject by default**: a version-2 system may target **its own entity** (Action target = self) so one
   GameObject holds Transform + Mesh (quad) + InstancedMesh + Action + ParticleSystem, like Unity's one GameObject;
   nested systems are child GameObjects. The legacy two-entity layout (a Particle System child targeting a Points or
   Mesh emitter) keeps working. The menu, `create_particles` and the importer make the single-GameObject form.
   The implementation must make an ActionComponent on a renderable entity work everywhere (Hierarchy icon,
   Inspector order, prefabs, duplication, `EntityCloner`, Animation tracks targeting it).
3. **Billboard quads by default**: new systems draw as instances of a unit quad (Mesh target + InstancedMesh with the
   new particle alignment modes), not GL points (point sprites are 1 pixel on D3D11 and size-capped on many GPUs).
   Points targets remain supported (billboard only; other render modes warn and draw as billboards).
4. **Axis convention**: the engine is right-handed Y-up; shape axes of version 2 follow the legacy engine shapes
   (cone, hemisphere and circle around **local +Y**, circle/rectangle in the **XZ plane**). The importer converts
   Unity's (+Z axis, XY planes, left-handed) by `S = diag(-1, 1, 1)` (the engine's Unity-facing convention, as in
   `2026-10-27-vertex-animation-design.md`: positions x -> -x, quaternions (x, y, z, w) -> (x, -y, -z, w)) and
   appends `Rx(+90°)` to the shape rotation (§6.10.4). Rotation sign: Unity's view space is left-handed, so a
   positive billboard rotation turns clockwise on screen there; the engine's positive roll turns counter-clockwise;
   the importer negates Z rotations and angular velocities. The sign lives in one constant of the importer and is
   pinned by a test (a rotating arrow texture); if a capture from Unity shows otherwise, only that constant changes.
5. **Deterministic randomness**: version 2 uses a per-system PCG32 stream. `autoRandomSeed` (default true) seeds from
   a process counter at each `play()`; `false` uses `randomSeed`, so a system replays identically (tests, previews).
   Per-particle random values (`random01` for each MinMaxCurve/Gradient and shape sample) are drawn at spawn and
   stored, as Unity does, so "random between two curves" keeps one lerp factor for the particle's life.
6. **Time**: the simulation advances by `Engine.deltatime` (scaled) x `main.simulationSpeed`, or unscaled delta when
   `main.useUnscaledTime`; `Engine.timeScale = 0` freezes scaled systems. Steps longer than 0.1 s are split into
   0.1 s substeps (spawns, limits and drag stay stable after a hitch); prewarm simulates one duration in 1/30 s
   steps at play.
7. **Gravity** = `gravityModifier` x the scene's physics gravity (3D: the Jolt world gravity, default (0,-9.81,0);
   2D scenes: the Box2D gravity scaled to world units), read each frame.
8. **Noise is included** with a documented algorithm (§6.3.9); the importer maps Unity's parameters and notes that the
   pattern differs.
9. **Stop action and callbacks**: `stopAction` None / Disable (deactivate the GameObject) / Destroy (destroy it) /
   Callback (call `onParticleSystemStopped()` on the GameObject's Lua scripts through the per-object callback path,
   `ActionSystemPlayers.cpp` ~941, shared convention); `onStop` of the Action fires too.
10. **Importer output** is an engine prefab per imported Unity object (systems as a GameObject tree), materials as
    `.material` files with library shaders, textures copied; a JSON + text report. It never edits the Unity project.
11. **Limit velocity dampen** is frame-rate independent: the excess speed is multiplied by `(1 - dampen)^(dt x 30)`
    (Unity's per-frame value at 30 fps); drag likewise continuous. Documented as the one place where values may look
    slightly different at other frame rates in Unity.

## 6 Design

### 6.1 Value types (`engine/core/particles/MinMax.h`)

```cpp
enum class MinMaxCurveMode : uint8_t { Constant, Curve, TwoCurves, TwoConstants };   // Unity minMaxState 0,1,2,3
struct JASPERENGINE_API MinMaxCurve {
    MinMaxCurveMode mode = MinMaxCurveMode::Constant;
    float constant = 0;               // Constant; TwoConstants: constantMax; Curve modes: multiplier (Unity "scalar")
    float constantMin = 0;            // TwoConstants (may exceed constant: lerp order kept)
    AnimationCurve curve, curveMin;   // Curve: curve; TwoCurves: curveMin..curve
    float evaluate(float t, float random01) const;  // t = normalized time; random01 per particle (or per system)
    static MinMaxCurve value(float v); static MinMaxCurve between(float a, float b);
    static MinMaxCurve fromCurve(const AnimationCurve& c, float multiplier = 1);
    static MinMaxCurve between(const AnimationCurve& a, const AnimationCurve& b, float multiplier = 1);
};
enum class MinMaxGradientMode : uint8_t { Color, Gradient, TwoColors, TwoGradients, RandomColor };   // 0..4
struct JASPERENGINE_API MinMaxGradient {
    MinMaxGradientMode mode = MinMaxGradientMode::Color;
    Vector4 color = Vector4(1,1,1,1), colorMin = Vector4(1,1,1,1);   // linear RGBA (TwoColors: colorMin..color)
    Gradient gradient, gradientMin;
    Vector4 evaluate(float t, float random01) const;  // RandomColor: gradient.evaluate(random01)
};
```

`evaluate`: Constant -> constant; Curve -> curve(t) x constant; TwoCurves -> lerp(curveMin(t), curve(t), r) x
constant; TwoConstants -> lerp(constantMin, constant, r). Gradients likewise (TwoColors lerp per channel). YAML/JSON:
a number, or `{mode: curve, multiplier, curve}` / `{mode: two_constants, min, max}` / `{mode: two_curves,
multiplier, min, max}`; gradients a colour, or `{mode: gradient|two_colors|two_gradients|random_color, ...}`.

### 6.2 Component (`engine/core/component/ParticleSystemComponent.h`, version-2 fields; legacy fields stay)

Defaults are Unity's defaults for a new system unless noted. Modules have `enabled`.

- `int version = 1` (decode default), runtime `ParticleSimState state`.
- **main**: `duration 5`, `looping true`, `prewarm false`, `startDelay MinMaxCurve(0)` (constant/two constants),
  `startLifetime (5)`, `startSpeed (5)`, `startSize3D false`, `startSize (1)` / `startSizeX/Y/Z (1)`,
  `startRotation3D false`, `startRotation (0)` / `startRotationX/Y/Z (0)` (radians), `flipRotation 0` (0..1:
  fraction of particles spinning the other way; Unity's `randomizeRotationDirection`), `startColor (white)`,
  `gravityModifier (0)`, `simulationSpace Local|World (Local)`, `simulationSpeed 1`, `useUnscaledTime false`
  (the component's top-level field shared with feature #21, §11),
  `scalingMode Hierarchy|Local|Shape (Local)`, `playOnAwake true`, `maxParticles 1000`, `emitterVelocityMode
  Transform|Rigidbody (Transform)`, `stopAction None|Disable|Destroy|Callback (None)`, `autoRandomSeed true`,
  `randomSeed 0`.
- **emission**: `enabled true`, `rateOverTime (10)`, `rateOverDistance (0)`, `bursts: [{time 0, count MinMaxCurve(30),
  cycles 1 (0 = infinite), interval 0.01, probability 1}]` (empty by default).
- **shape**: `enabled true`, `type Cone` with the enum `Sphere, Hemisphere, Cone, ConeVolume, Box, BoxShell,
  BoxEdge, Circle, Edge, Rectangle, Donut`; `radius 1`, `radiusThickness 1` (0 surface .. 1 volume), `radiusMode
  {Random, Loop, PingPong, BurstSpread}`, `radiusSpread 0`, `radiusSpeed (1)`, `angle 25` (degrees), `length 5`,
  `arc 360`, `arcMode` (same enum), `arcSpread 0`, `arcSpeed (1)`, `boxThickness (0,0,0)`, `donutRadius 0.2`,
  `position (0)`, `rotation (0)` (Euler degrees, applied in XYZ order like Unity's shape transform), `scale (1)`,
  `alignToDirection false`, `randomDirectionAmount 0`, `sphericalDirectionAmount 0`, `randomPositionAmount 0`.
- **velocityOverLifetime**: x/y/z, `orbitalX/Y/Z`, `orbitalOffset (Vector3)`, `radial`, `speedModifier (1)`, `space
  Local|World (Local)`.
- **limitVelocityOverLifetime**: `separateAxes false`, `limit (1)` / `limitX/Y/Z`, `space`, `dampen 1`, `drag (0)`,
  `multiplyDragByParticleSize true`, `multiplyDragByParticleVelocity true`.
- **inheritVelocity**: `mode Initial|Current`, `curve (0)`.
- **forceOverLifetime**: x/y/z, `space Local|World`, `randomized false` (per frame re-roll).
- **colorOverLifetime**: `color (gradient white)`; **colorBySpeed**: `color`, `range (0, 1)`.
- **sizeOverLifetime**: `separateAxes false`, `size (curve 0->1 linear)` / `x/y/z`; **sizeBySpeed**: same + `range`.
- **rotationOverLifetime**: `separateAxes false`, `z (45 deg/s in radians)` / `x/y`; **rotationBySpeed**: same +
  `range`.
- **noise**: `strength (1)` / `strengthX/Y/Z` with `separateAxes`, `frequency 0.5`, `scrollSpeed (0)`, `damping true`,
  `octaves 1` (1..4), `octaveMultiplier 0.5`, `octaveScale 2`, `positionAmount (1)`, `rotationAmount (0)`,
  `sizeAmount (0)`.
- **textureSheetAnimation**: `mode Grid|Sprites`, `tilesX 1`, `tilesY 1`, `animation WholeSheet|SingleRow`,
  `rowMode Custom|Random`, `rowIndex 0`, `timeMode Lifetime|Speed|FPS`, `frameOverTime (curve 0->1)`, `fps 30`,
  `speedRange (0,1)`, `startFrame (0)`, `cycles 1`, `flipU 0`, `flipV 0` (fractions), `sprites` (frame ids of the
  target's sprite frames).
- **renderer** (on the system; applied to its target): `renderMode Billboard|StretchedBillboard|HorizontalBillboard|
  VerticalBillboard|Mesh`, `cameraVelocityScale 0`, `velocityScale 0`, `lengthScale 2`, `alignment View|World|
  Local|Facing|Velocity` (Billboard and Mesh), `sortMode None|ByDistance|OldestInFront|YoungestInFront`,
  `sortingFudge 0`, `sortPriority 0` (Unity sorting order), `minParticleSize 0`, `maxParticleSize 0.5` (fractions of
  the viewport height), `pivot (0,0,0)`, `flip (0,0,0)`, `mesh` (path or primitive id, Mesh mode).

### 6.3 Simulation (`engine/core/particles/ParticleSimulation.{h,cpp}`, pure, gtested)

Data per particle (SoA vectors in `ParticleSimState`, capacity `maxParticles`): position, velocity (base, integrated),
animatedVelocity (from velocity over lifetime, not integrated), age, lifetime, startSize3, startRotation3,
angularVelocity3, startColor, random seeds (one 32-bit seed per particle; per-module randoms derived by hashing
seed + module id, so they are stable for the life), texture frame, emitter-relative spawn data. Order per step
(`dt` already scaled and substepped), matching Unity's documented module order:

1. **System clock**: `time += dt` after `startDelay` elapsed. Non-looping: emission stops at `duration`; the system
   stops when no particle is alive (then `stopAction`). Looping: `time` wraps at `duration`, bursts re-arm.
   `tSys = time / duration` drives start values, emission rates and gravity (Unity's "over the duration").
2. **Emission**: `rateOverTime(tSys)` accumulates fractional counts; `rateOverDistance(tSys)` x the emitter's
   world-space travel this step (always world distance, as Unity); bursts at their times (cycles x interval;
   `probability` per cycle; `count` evaluated with a system random). Spawns within a step are spread uniformly over
   it: each new particle's emitter transform is interpolated between last and current, and it is aged by the
   remainder of the step (no clumping for fast emitters, rate-over-distance trails stay continuous). Capacity
   reached: no spawn (Unity).
3. **Spawn**: lifetime, speed, size(3D), rotation(3D), colour from `main` at `tSys` with the particle's randoms;
   flip rotation sign for `flipRotation` fraction; **shape sample** (§6.3.1) gives a local position and direction;
   velocity = direction x startSpeed (+ inherit velocity Initial: emitter velocity x curve(0)); converted to world in
   World space (emitter transform at the spawn instant), else kept local.
4. **Per particle, t = age / lifetime**: `age += dt`; dead when `age >= lifetime` (swap-remove keeps arrays dense;
   order by spawn kept for the age sorts).
5. Gravity: `velocity += gravity x gravityModifier(tSys) x dt` (in simulation space).
6. Force over lifetime: `velocity += F(t) x dt` (F in its space converted to simulation space; `randomized` re-rolls
   the random each step).
7. Inherit velocity Current: `velocity += emitterDelta` (the emitter's velocity change x curve(t)).
8. Velocity over lifetime: `animatedVelocity = V(t)` (+ orbital: rotation of the position around the emitter's centre
   + offset by angular speeds x dt; + radial: `radial(t)` along the direction from the centre); total velocity =
   (velocity + animatedVelocity) x speedModifier(t).
9. Noise: `n = fbm3(p x frequency + scroll x time)` per axis (3D gradient noise, three decorrelated offsets, octaves
   with multiplier/scale), `total += n x strength(t) x positionAmount(t)` as velocity; `damping` divides strength by
   frequency (Unity); rotation and size amounts add `n.x x amount`.
10. Limit velocity: the excess over `limit(t)` (magnitude or per axis) is scaled by `(1 - dampen)^(dt x 30)`; drag:
    `velocity *= max(0, 1 - drag(t) x dt x (size if multiplyBySize) x (|v| if multiplyByVelocity))`.
11. Integrate: `position += total x dt`.
12. Rotation: `rotation3 += (angularVelocity(t) + bySpeed(|v|)) x dt`; size = startSize3 x sizeOverLifetime(t) x
    sizeBySpeed(|v|); colour = startColor x colorOverLifetime(t) x colorBySpeed(|v|) (by-speed `t = clamp((|v| -
    range.x) / (range.y - range.x))`).
13. Texture sheet frame: Lifetime: `frameOverTime(t x cycles mod 1)`; FPS: `startFrame + age x fps`; Speed:
    `remap(|v|, speedRange)`; frame index floor(value x frames) wrapped; row for SingleRow; to a UV rect.

#### 6.3.1 Shapes (shape-local frame, then `position/rotation/scale` of the shape, then the emitter transform)

- Sphere: point at radius `r x lerp(1 - radiusThickness, 1, u)^(1/3)`-style volume sampling between the inner radius
  `radius x (1 - radiusThickness)` and `radius` (uniform in volume); direction = outward normal.
- Hemisphere: the same restricted to y >= 0.
- Cone (`Cone` = from base, `ConeVolume` = volume): base disc of `radius` in XZ at y = 0 (area sampling with
  thickness), arc applies; direction tilts outward by `angle x (distance from axis / radius)` (Unity: points at the
  rim leave at `angle`, the centre straight up); ConeVolume also places the point along the direction up to `length`.
  Radius 0: a point emitting in a cone of `angle`.
- Box: uniform in the box `scale`, direction +Y; BoxShell on faces, BoxEdge on edges (thickness per axis).
- Circle: in XZ, radius with thickness, arc; direction outward (radial in the plane).
- Edge: segment along X of length `2 x radius`, direction +Y.
- Rectangle: in XZ within `scale.x x scale.z`, direction +Y.
- Donut: torus in XZ, ring `radius`, tube `donutRadius` x thickness, direction away from the ring centre.
- Arc modes: Random (uniform angle in [0, arc]), Loop (angle advances `arcSpeed x 360 x time`), PingPong,
  BurstSpread (evenly spread within each burst); `arcSpread` quantizes. Radius modes likewise along the radius.
- `alignToDirection`: start rotation aligned to the direction (Mesh mode); `randomDirectionAmount` blends towards a
  random unit vector, `sphericalDirectionAmount` towards the direction from the shape centre;
  `randomPositionAmount` offsets by a random vector of that length.
- Shape disabled: position at the shape origin, direction +Y (Unity: +Z, converted).

### 6.4 Rendering

- The system writes `InstanceData` of its target InstancedMesh (position, rotation, scale = size x scaling, colour,
  `textureRect`) or `PointRenderData` of a Points target, as today (`syncParticleInstance`, `ActionSystem.cpp:260`),
  in display space (local systems: target-local; world systems: the cached inverse, existing code).
- `InstancedMeshComponent` gains `particleAlignment` (`InstanceAlignment {None, View, Vertical, Horizontal,
  Stretched, World, Local, Facing, Velocity}`; `None` = today's instance rotation; existing
  `instancedBillboard`/`instancedCylindricalBillboard` map to View/Vertical when set) and per instance
  `InstanceData.roll` (radians), `InstanceData.velocity` (simulation velocity, for stretched/velocity),
  `InstanceData.pivot` (shared, from the renderer). `updateInstancedMesh` (`RenderSystem.cpp:11854`) and
  `buildInstanceList` (`:16198`, per camera) build the matrix: View = camera rotation x Rz(roll); Vertical = facing
  the camera around world Y x Rz(roll); Horizontal = face up (XZ plane) x Ry(roll); Stretched = X along the screen
  projection of velocity, length = size x lengthScale + |v| x velocityScale + camera velocity term, width = size;
  Mesh modes: Local (system rotation x particle rotation), World (particle rotation), View (camera x particle),
  Facing (towards the camera position), Velocity (forward along velocity). Flip: negative scale fraction per
  particle (by the particle random) for flipU/flipV and renderer `flip`.
- **Min/max particle size**: in `buildInstanceList`, the projected size is clamped to `[min, max] x viewport
  height` per camera (perspective: size / (distance x 2 tan(fov/2))).
- **Sorting**: ByDistance uses the existing back-to-front instance sort (`sortInstancedMesh`, `:11950`);
  OldestInFront / YoungestInFront order instances by age (oldest last / first); None keeps spawn order. The
  system's place among other transparent objects uses its distance + `sortingFudge` (lower draws on top, Unity) and
  `sortPriority` before distance (`TransparentRenderComparison`).
- The default quad: a 1x1 quad centred at the origin facing +Z (`Primitive` quad), material `library:unlit` with
  `VERTEX_COLOR` off (instance colour is `v_color`), blend alpha, depth write off, texture the engine's built-in
  `particle` soft circle (new 64x64 built-in texture default like `white`/`noise`, `@texture` default name
  `particle`). Additive presets set `blendMode additive`.
- Mesh mode: the target Mesh is the chosen mesh (a primitive or a model's mesh node) with its material.

### 6.5 Legacy model (version 1)

The current fields, `applyParticleInitializers`/`applyParticleModifiers`/`advanceParticle` and
`particlesActionUpdate` (`ActionSystem.cpp:875-1390`) are untouched and only run for `version == 1`. Version 2 runs
`ParticleSimulation` from the same `particlesActionUpdate` entry points (both target kinds). `particleActionStart`
and `particleActionReset` dispatch by version.

### 6.6 Lua API (`engine/core/script/binding/ActionClassesLua.cpp:145`)

`ParticleSystem` (Action) gains, for version 2 (legacy setters keep working on version 1 and are mapped on version
2 as in §6.7, with a one-time deprecation warning):

- Control: `play([withChildren=true])`, `stop([withChildren=true], [ParticleSystemStopBehavior.StopEmitting])`
  (`StopEmittingAndClear` clears), `pause([withChildren])`, `clear([withChildren])`, `emit(count)` (immediate, uses
  the shape and start values), `simulate(t, [withChildren], [restart=true])`, `isPlaying`, `isPaused`, `isStopped`,
  `isEmitting`, `isAlive([withChildren])`, `particleCount`, `time` (rw), `randomSeed`, `useAutoRandomSeed`,
  `version` (rw: 1 -> 2 runs the upgrade), `upgrade()` -> report table.
- Modules as live proxies (`ps.main`, `ps.emission`, `ps.shape`, `ps.velocityOverLifetime`,
  `ps.limitVelocityOverLifetime`, `ps.inheritVelocity`, `ps.forceOverLifetime`, `ps.colorOverLifetime`,
  `ps.colorBySpeed`, `ps.sizeOverLifetime`, `ps.sizeBySpeed`, `ps.rotationOverLifetime`, `ps.rotationBySpeed`,
  `ps.noise`, `ps.textureSheetAnimation`, `ps.renderer`): userdata holding (scene, entity), each field of §6.2 a
  property with the same lowerCamel name; MinMaxCurve fields accept a number (constant) or a `MinMaxCurve`;
  gradients accept a `Vector4`/`Vector3` (sRGB colour) or a `MinMaxGradient`. Unity's
  `var e = ps.emission; e.rateOverTime = 0` and `ps.emission.rateOverTime = 0` both work. Bursts:
  `emission:setBursts({ {time=0, count=10, cycles=1, interval=0.01, probability=1}, ... })`, `getBurst(i)`,
  `burstCount`. `ps.emission.rateOverTimeMultiplier`, `startLifetimeMultiplier` etc. (Unity's multipliers: the
  constant/multiplier field).
- Classes `MinMaxCurve` (`MinMaxCurve(v)`, `MinMaxCurve(min, max)`, `MinMaxCurve(multiplier, curve)`,
  `MinMaxCurve(multiplier, curveMin, curveMax)`, fields `mode` (`ParticleSystemCurveMode.Constant|Curve|TwoCurves|
  TwoConstants`), `constant`, `constantMin`, `constantMax`, `curve`, `curveMin`, `curveMax`, `curveMultiplier`,
  `evaluate(t[, r])`), `MinMaxGradient` (`MinMaxGradient(color)`, `(colorMin, colorMax)`, `(gradient)`,
  `(gradientMin, gradientMax)`, `mode` (`ParticleSystemGradientMode.Color|Gradient|TwoColors|TwoGradients|
  RandomColor`), `color`, `colorMin`, `colorMax`, `gradient`, `gradientMin`, `gradientMax`, `evaluate`), enums
  `ParticleSystemShapeType`, `ParticleSystemSimulationSpace`, `ParticleSystemScalingMode`,
  `ParticleSystemStopAction`, `ParticleSystemRenderMode`, `ParticleSystemSortMode`,
  `ParticleSystemRenderSpace` (alignment), `ParticleSystemAnimationMode/Type/TimeMode`,
  `ParticleSystemShapeMultiModeValue`, `ParticleSystemStopBehavior`, `ParticleSystemInheritVelocityMode`.
- `gameObject:getParticleSystem()` (nil without one; finds the Action on the GameObject or, for the legacy layout,
  the child system targeting it); `gameObject:getParticleSystemsInChildren()`.
- Per-object callback `onParticleSystemStopped()` (stop action Callback).

### 6.7 Serialization, migration and exports

- YAML (`editor/Stream.cpp:9262` encode/decode): version 2 writes `version: 2` and one map per module
  (`main`, `emission`, `shape`, ..., `renderer`) with only non-default values, MinMaxCurve/Gradient in §6.1 shapes;
  legacy keys are not written for version 2. Version 1 writes exactly today's keys (byte-identical round trip).
- Factory (`editor/Factory.cpp:3816`): emits version 2 module literals (`formatMinMaxCurve`, `formatMinMaxGradient`,
  `formatCurve`, `formatGradient`), so exported builds play identically; Catalog properties for every field
  (`main.startLifetime`, `shape.radius`, `emission.bursts`, ... `PropertyType::MinMaxCurve/MinMaxGradient/Curve/
  Gradient`) with update flags (no reload except `maxParticles`, render mode/mesh/alignment changes that rebuild the
  target).
- Upgrade 1 -> 2 (pure function `particles::upgradeLegacy(const ParticleSystemComponent&) -> {component, report}`):
  `rate` -> `rateOverTime`; `loop` -> `looping` with `duration` = the legacy cycle duration
  (`getParticleCycleDuration`, `ActionSystem.cpp:246`) and non-loop -> `duration` = maxParticles / rate (the legacy
  stop rule) ; `lifeInitializer` -> `startLifetime` two constants; position shapes -> `shape` (Box min/max -> Box
  with `position` = centre and `scale` = size, direction irrelevant because `startSpeed = 0`; Sphere/Hemisphere/
  Circle radius/innerRadius -> radius + `radiusThickness = 1 - inner/radius`; legacy Cone (apex at origin, volume
  to `coneHeight`) -> ConeVolume radius 0, angle, length = coneHeight); `velocityInitializer` box -> velocity over
  lifetime x/y/z two constants (exact: a constant additive velocity per particle), `startSpeed 0`;
  `accelerationInitializer` -> force over lifetime two constants (exact); colour/alpha initializers -> `startColor`
  two colours (sRGB flag honoured); `colorGradient` stops -> colour over lifetime gradient (exact); size -> start
  size; rotation (quaternions) -> start rotation 3D Euler (two constants per axis: exact for single-axis ranges,
  approximated otherwise, reported); scale -> `startSize3D`; bursts -> bursts (counts as two constants);
  `localSpace` -> `simulationSpace`; modifiers (fromTime/toTime seconds with an `Ease`) -> the over-lifetime curve
  sampled at 17 keys of normalized time using `life = (minLife + maxLife)/2` (exact when min == max, else reported
  with the max error); position modifiers -> reported unsupported (no Unity equivalent), kept by not upgrading
  unless `force`. The report lists every field with `exact | approximated (max error) | dropped`.
- Exports and Play: no new platform code; the simulation is engine C++.

### 6.8 Editor UX

- Inspector (`editor/window/InspectorWindow.cpp:10871`, rewritten for version 2; version 1 keeps today's UI plus an
  "Upgrade to modules..." banner showing the report preview before applying): the Main module always open, other
  modules as foldouts with an enable checkbox in the header (Unity layout and order: Main, Emission, Shape, Velocity
  over Lifetime, Limit Velocity over Lifetime, Inherit Velocity, Force over Lifetime, Color over Lifetime, Color by
  Speed, Size over Lifetime, Size by Speed, Rotation over Lifetime, Rotation by Speed, Noise, Texture Sheet Animation,
  Renderer); a "+" menu shows disabled modules. MinMaxCurve fields: a value field with a small dropdown arrow for the
  mode (Constant, Curve, Random Between Two Constants, Random Between Two Curves) and the `CurveField` widget (two
  curves in one editor for Two Curves, the band between them shaded); MinMaxGradient likewise with `GradientField`.
  Bursts table (Time, Count, Cycles, Interval, Probability). Unsupported Unity modules are not shown. Undo per edit
  through `PropertyCmd`; multi-edit across selected systems.
- **Scene view particle overlay** (when a system or its parent is selected, edit mode): Play / Pause / Restart /
  Stop, Playback Speed, Playback Time, Particles count, "Simulate Layers: selected / all"; the preview uses a fixed
  seed so restarts repeat; it runs the real `ParticleSimulation` on a scratch copy (existing
  `updateParticlePreviewSnapshot`, `InspectorWindow.cpp:13187`, restores the scene state after the preview).
  Shape gizmo: wire sphere/hemisphere/cone/box/circle/rectangle/donut with handles for radius, angle, length, box
  size.
- Menus: GameObject > Effects > **Particle System** (single GameObject, quad renderer, Unity's default values:
  cone 25 deg, radius 1, rate 10, lifetime 5, speed 5, size 1, white soft particle; in 2D scenes speed and size
  scaled by the scene's pixels per unit and the shape rotated so emission goes up the screen),
  Assets > **Import Unity Particle System...** (§6.10).
- `jasperengine-editor check`: `particles-points-render-mode` (a non-billboard mode on a Points target),
  `particles-max-exceeded-budget` (> 5000 max particles on mobile targets), `particles-legacy` (info: can be upgraded).

### 6.9 MCP / agent surfaces

- `create_particles` (`editor/mcp/tools/WorldTools.cpp:4100`): creates version-2 single-GameObject systems; presets
  (fire, smoke, sparks, explosion, snow, rain, magic) rewritten as module values; existing arguments keep their
  meaning mapped to modules (`life` -> startLifetime, `velocity {min,max}` -> velocity over lifetime two constants,
  or new `speed` along the shape, `gradient` -> colour over lifetime, `size_over_life`, `alpha_over_life` -> curves);
  new optional `renderer: "quads"|"points"` (default quads), `upgrade: true` (on `entity`, returns the report),
  `modules: {...}` (raw module JSON, same shapes as `set_properties`). `emitter_entity` keeps the legacy layout.
- `set_properties` / `get_entity` on every field (`main.startSpeed` etc.); `get_entity` in Play adds `particles:
  {count, time, isPlaying, isEmitting, seed}`.
- New tool **`import_unity_particles`** `{path, object?, dest = "assets/fx", unity_project?, dry_run = false,
  overwrite = false}` -> `{prefabs: [...], report: {...}}` (report §6.10.6). Its description stays under 400
  characters; `tools/mcp/tool_budget.py` must pass. `docs/mcp/TOOLS.md` regenerated.
- Guide: `docs/mcp/guide/particles.md` (new: model, modules, curves, render modes, sorting, costs, Lua, upgrade,
  importer); one line in `McpInstructions.cpp`.

### 6.10 Unity importer (`editor/unity/` new: `UnityYaml.{h,cpp}`, `UnityProject.{h,cpp}`, `UnityParticleImport.{h,cpp}`)

1. **Input**: a `.prefab` or `.unity` file inside a Unity project (the project root = the nearest ancestor holding
   `Assets/` and `ProjectSettings/`; `unity_project` overrides). `object` selects a GameObject path
   (`Player/Camera/CannonHead/EndParticles`); default: every root GameObject that has a ParticleSystem in itself or
   its children (a scene: each such root).
2. **YAML**: Unity's multi-document YAML with `--- !u!<classId> &<fileId> [stripped]` headers and `%TAG !u!` is
   pre-split by document header, each body parsed with yaml-cpp (already in the editor) after rewriting the tag; all
   scalars kept as strings until typed by the reader (fixes the `m_VertexStreams` octal trap); documents indexed by
   fileId. Class ids used: 1 GameObject, 4 Transform, 224 RectTransform, 198 ParticleSystem, 199
   ParticleSystemRenderer, 1001 PrefabInstance, 21 Material (in `.mat`), 114 MonoBehaviour (only to detect known
   scripts: Coffee `UIParticle`, `CFX_AutoDestructShuriken` -> stopAction Destroy).
3. **GUIDs**: `UnityProject` maps guid -> asset path from every `.meta` under `Assets/` (cached per import), resolves
   `{fileID, guid, type}` references; built-in guids handled by a table (meshes 10202 Cube, 10207 Sphere, 10208
   Capsule, 10206 Cylinder, 10210 Quad; materials 10301 Default-Particle, 10308 Default-ParticleSystem, 10306
   Default-Line; shaders 200, 10720, 10721, and Particles/Standard Unlit by name). Unknown built-ins are reported.
4. **Transforms and axes**: the GameObject tree under the selected root becomes engine GameObjects with converted
   transforms (S = diag(-1,1,1): position (x,y,z) -> (-x,y,z), rotation quaternion (x,y,z,w) -> (x,-y,-z,w), scale
   kept); the shape transform converts the same way and gets `Rx(+90 deg)` appended (Unity shape +Z -> engine +Y;
   shape-local axes: engine y = Unity z, engine z = Unity -y, so `scale` and `boxThickness` become (x, z, y)); world-
   space vectors of modules (force, velocity in world space, orbital) convert by S; local-space ones by S then the
   axis swap of the shape frame **only for shape**, not for velocity (velocity/force local vectors are in the system's
   transform frame, converted by S). Start rotations and Z angular velocities are negated (5.4); 3D rotations
   (x,y,z) map to (x,-y,-z) radians.
5. **Prefab instances and overrides**: a `PrefabInstance` is resolved recursively (nested prefabs) and its
   `m_Modifications` applied to the source documents before conversion, by property path (`InitialModule.startSize.
   scalar`, `...m_Curve.Array.data[0].value`, `m_LocalPosition.x`, `m_IsActive`, `m_Enabled`); unknown paths are
   reported, not fatal. Scene overrides of a prefab system are honoured (the E and H flamethrower overrides).
6. **Module mapping**: every §6.2 field from its YAML field (`lengthInSec` -> duration, `moveWithTransform` ->
   simulationSpace, `InitialModule.*`, `ShapeModule.type` table, `arc.mode` etc.), MinMaxCurve/Gradient converted
   (gradient times / 65535, first `m_NumColorKeys`/`m_NumAlphaKeys` keys, colours **as stored**: Unity serializes
   gamma-space colours, converted sRGB -> linear; `m_ColorSpace` ignored), curves: non-weighted keys copied; weighted
   keys converted to the nearest non-weighted Hermite (reported); `m_PreInfinity/m_PostInfinity` 2 -> Clamp, 0/1/4/8
   per Unity's wrap enum. Renderer: render mode, alignment, sort mode, fudge, min/max size, pivot (Unity pivot is in
   particle-size units, X/Y/Z -> converted), flip, `m_SortingOrder` -> `sortPriority`, mesh (built-in -> primitive;
   asset mesh -> the engine model at the same relative path under the project's assets if it was imported, else the
   Unity file is reported `mesh-missing` with its path), `m_CastShadows`/`m_ReceiveShadows` -> mesh shadow flags.
7. **Materials**: one `.material` per Unity material (`dest/materials/<name>.material`), `library:unlit` with blend
   by a table: Mobile/Particles/Additive, Legacy Shaders/Particles/Additive (`_TintColor` x 2 -> baseColor),
   Particles/Additive (Soft), UI/Additive -> additive; Mobile/Particles/Alpha Blended, Legacy Particles/Alpha
   Blended, Particles/Standard Unlit (Fade/Transparent), Default-Particle(System) -> alpha; Legacy Particles/
   Multiply -> multiply; `Particles/Alpha Blended Premultiply` -> premultiplied; Simple/Unlit Vertex Color_Alpha ->
   alpha with depth write off. Unknown or custom shaders (WFX/*, DarkFresnelParticles, Amplify) -> the closest by
   their declared `Blend` state if the `.shader` file is readable (`Blend One One` -> additive, `DstColor SrcAlpha`/
   `DstColor Zero` -> multiply, `SrcAlpha OneMinusSrcAlpha` -> alpha, no Blend + ZWrite On -> opaque), with `_MainTex`,
   `_Color`/`_TintColor` mapped, and reported `shader-approximated` naming the Unity shader. `_InvFade` (soft
   particles) -> the unlit `SOFT` feature with `softness = 1 / _InvFade`. The default particle texture -> the engine's
   built-in `particle` texture.
8. **Textures**: copied to `dest/textures/` keeping names (PNG/JPG/TGA as is; PSD/EXR/TIFF reported
   `texture-format`), the Unity import settings ignored except `alphaIsTransparency`; A8 textures (alpha-only) are
   expanded to white RGB + alpha so additive WFX "Alpha8" shaders keep their look.
9. **Report** (`dest/<name>.import-report.json` and the editor Console / MCP result): `{source, systems: [{path,
   engine_entity, modules: {name: "imported"|"approximated"|"unsupported"|"disabled"}, notes: [{code, message}]}],
   materials: [...], textures: [...], warnings: [...]}`; codes `module-unsupported` (sub emitters, collision, ...
   when enabled), `shape-unsupported` (mesh/sprite shapes), `ui-particle`, `shader-approximated`, `mesh-missing`,
   `weighted-tangents`, `culling-mode`, `ring-buffer`, `custom-simulation-space`, `override-unknown`,
   `texture-format`, `noise-pattern`.
10. **Output**: one engine prefab per selected root at `dest/<root name>.prefab` (`overwrite` false -> a numbered
    name), created through editor commands (one undo step in the editor; files written with the existing safe-write
    helpers), referencing the materials and textures; the Project window refreshes. The CLI writes the same files
    without the GUI: `jasperengine-editor import-unity-particles <file> --project <engine project dir> [--object
    <path>] [--dest assets/fx] [--unity-project <dir>] [--report <json>] [--dry-run]` (registered in
    `editor/main.cpp` next to `bake-vat`, implemented in `editor/cli/CommandLine.cpp`), exit code 0 / 1 (error) / 2
    (imported with warnings).
11. Licensing note in the guide: Asset Store content (e.g. JMO WarFX) may be imported for use in your own game but
    not redistributed; the engine's repository must not contain such files (tests use synthetic fixtures, §9).

### 6.11 Docs to update

`docs/mcp/guide/particles.md` (new), `docs/mcp/guide/lua-api.md` §4.16 (rewritten: modules, control, classes,
enums; legacy setters as a subsection), `docs/unity-glossary.md` (ParticleSystem modules, MinMaxCurve,
MinMaxGradient, stop behaviours, the importer; fix the existing row that maps `ParticleSystem` to an old
`Particles` name), `docs/mcp/guide/shaders.md` (the built-in `particle` default texture), README feature list.

## 7 Behaviour rules and edge cases

- `prewarm` only when `looping` (Unity); prewarm ignores `startDelay`. `startDelay` delays the first emission and the
  system clock, not particles already alive.
- `duration` minimum 0.05 s; `maxParticles` 0..100000 (warn above 5000 on mobile targets).
- Lifetime <= 0: the particle is not spawned. Size negative: drawn mirrored as Unity does (flip), 0: not drawn.
- `play()` on a playing system: no-op; on a paused one: resumes; after stop: restarts at time 0 (new seed when auto).
  `stop()` (StopEmitting): emission stops, particles live out, `isEmitting` false, `isPlaying` true until the last
  dies, then `isStopped` and the stop action. `clear()` removes particles without stopping.
- Child systems: `play/stop/pause/clear(withChildren)` apply to every version-2 system in the GameObject's
  children (Unity), in hierarchy order. `playOnAwake` of children is independent.
- GameObject deactivated: the system stops and clears (Unity); reactivated with `playOnAwake`: plays from 0.
- `emit(n)` while stopped: particles spawn and simulate (the system becomes "alive" without emitting).
- World-space systems moved: alive particles stay in the world; local ones move with the emitter.
  `scalingMode`: Hierarchy uses the full world scale for positions and sizes, Local the system's own scale only
  (parents ignored), Shape scales only the shape positions (not sizes nor velocities).
- Rate over distance in local space still measures world travel; teleporting the emitter (> 100 world units in a
  frame) does not spawn (no burst of thousands), logged once.
- 2D scenes: the simulation is 3D; z stays 0 when shape and forces have no z (the 2D presets keep z 0); billboards
  face the orthographic camera; sprite sorting uses the emitter's place in hierarchy order when the camera draws
  in hierarchy order.
- Editor preview and Play are independent: entering Play discards the preview state.
- Version-1 systems ignore every module field; setting a module field from Lua on a version-1 system raises a Lua
  error telling to call `upgrade()` (so nothing silently does nothing).

## 8 Performance (mobile budgets)

- CPU simulation O(alive x enabled modules); target: 2,000 alive particles with 6 modules < 1.5 ms per frame on a
  2019 mid-range Android (single thread), measured by the fixture `tools/fixtures/particles/bench`
  (`JASPERENGINE_FRAME_STATS=1` reports `particles_sim_ms`); curves evaluated with a per-curve 64-entry lookup table
  rebuilt on change (no Hermite per particle per frame); noise one fbm3 per alive particle per octave.
- Render: one instanced draw per system; instance upload once per frame (`needUpdateInstances`); per-camera
  billboard matrices only for cameras that see the system (existing view mechanism).
- Memory: SoA ~120 bytes per particle capacity + 96 bytes per instance.

## 9 Tests and acceptance

Unit (`editor/tests`, engine-free where possible):
- `MinMaxTest`: the four curve modes and five gradient modes against hand values; per-particle random stability.
- `ParticleShapeTest` (seeded, 10,000 samples per shape): positions inside the volume/surface/edge (thickness 0 on the
  surface within 1e-4, 1 in the volume with the expected radial CDF ± 2 %), arc limits and Loop/PingPong/BurstSpread
  sequences, cone directions (rim particles at `angle` ± 0.5 deg, centre ones vertical), `alignToDirection`,
  `randomDirectionAmount 1` uniform on the sphere (mean |dir| < 0.03).
- `ParticleEmissionTest`: rate 10 for 1 s -> exactly 10 spawns; fractional accumulation across frames; rate over
  distance 5 for a 2-unit move -> 10; bursts with cycles 3 and interval 0.5 -> 3 x count at the right times;
  probability 0.5 over 1000 cycles within ± 5 %; maxParticles cap; substepping of a 0.5 s frame.
- `ParticleSimulationTest`: gravity modifier, force world vs local, velocity over lifetime additive (positions match
  the analytic integral), limit velocity with dampen 1/0.1 at 30 and 60 fps (same result within 1 %), drag, noise
  bounded and seed-stable, size/rotation/colour by speed, texture sheet frames for each time mode (FPS 25 over 1 s ->
  25 frame advances), prewarm equals simulating one duration, start delay, non-looping stop + stop actions,
  `stop()` vs `StopEmittingAndClear`, child propagation, determinism with a fixed seed (two runs identical).
- `ParticleLegacyIdentityTest`: version-1 fixtures (the seven `create_particles` presets and the editor's default)
  simulated 120 frames with `srand(1)` produce particle positions/colours/sizes equal to a capture recorded from
  `main` before the change (committed `editor/tests/fixtures/particles-legacy/*.json`).
- `ParticleUpgradeTest`: each mapping rule; exact cases simulate to the same statistics as version 1 (mean/variance of
  positions at t = 1 s within 2 %); approximated modifiers report their error.
- `ParticleStreamTest`: version 1 files round-trip byte-identically; version 2 round trip of every field; Factory output
  compiles and plays (generator test pattern).
- `UnityYamlTest`, `UnityParticleImportTest` on **synthetic fixtures** in `editor/tests/fixtures/unity-particles/`
  written by hand to the YAML forms of §4.2 (one per module/mode/shape/render mode/override form, plus a nested prefab
  instance with modifications and a `stripped` stub); assert every mapped value, axis conversion (a cone along Unity
  +Z with shape rotation (-90,0,0) emits along engine +Y), colour conversion, and the report codes. No game or Asset
  Store file is committed.
- `UnityGameImportTest` (skipped unless `/tmp/gamesrc/Minigames` exists or `JASPER_UNITY_GAME_DIR` is set): imports
  the 18 systems of §4.1; asserts each produces a prefab, no `module-unsupported`, and only the expected notes
  (`ui-particle` for #12, `shader-approximated` for #2, #7-9, #13, #14, #10 mapped exactly, `mesh-missing` for
  lowpolySphere when the model is not imported).

Pixel / end to end (MCP `tools/mcp/tests/test_particles.py`, screenshots with `htmlui_pixels.py`):
- A fixed-seed system per render mode in front of a black background; billboard quads stay square when the camera
  orbits; horizontal lies flat (top view shows full quads, side view thin lines); vertical stays upright under a
  tilted camera; stretched elongates along motion (bounding box aspect > 2 for velocityScale 0.2 at speed 10); mesh
  mode draws cubes.
- Texture sheet 2x2 with 4 coloured tiles at FPS 1: the dominant colour changes each second.
- Sorting: oldest-in-front vs youngest-in-front with opaque-colour particles changes the top colour.
- Importer via MCP: `import_unity_particles` on a synthetic flamethrower-like fixture creates the prefab; instantiating
  it in Play shows particles; the report JSON matches.
- `create_particles preset=fire` makes a version-2 system; the old preset behaviour is preserved on the legacy
  fixture scene (screenshot equal to the reference captured before the change, tolerance 0.5 %).
- Backends: the render-mode fixture under `SMOKE_BACKEND=gles3` and `vulkan`.
- Lua: `eval_lua` covering module proxies, MinMaxCurve construction, `emit`, `stop` behaviours,
  `onParticleSystemStopped`; `test_stubs.py` passes.

Builds: Linux editor and tests; standalone engine; Android and Web compile.

Acceptance:
1. Every old scene with particles plays identically (identity test) and shows the upgrade banner; upgrading is undoable
   and reports.
2. Start speed follows the shape direction for every shape; every module of §6.2 works with every MinMax mode.
3. The 18 game systems import (CLI, menu, MCP) into prefabs that play in editor Play and in an exported Linux build,
   with only the expected report notes; the flamethrower honours its E/H scene overrides.
4. Render modes, sorting modes, fudge and min/max size work on glcore, gles3 and Vulkan.
5. Inspector (modules, curves, gradients, preview overlay, shape gizmos), Lua (Unity names), MCP (`create_particles`,
   `set_properties`, `import_unity_particles`), docs and stubs cover the feature.

## 10 Integration points

- `engine/core/component/ParticleSystemComponent.h` (version + modules), `engine/core/action/ParticleSystem.{h,cpp}`
  (Lua-facing object), new `engine/core/particles/` (MinMax, ParticleSimulation, shapes, noise, upgrade).
- `engine/core/subsystem/ActionSystem.cpp`: `actionStart` 278-350 (target = self, version dispatch),
  `particleActionStart`/`Reset` ~1200-1290, `particlesActionUpdate` 1296/1356, stop path 352 (stop behaviours,
  stop actions, the per-object callback).
- `engine/core/component/InstancedMeshComponent.h` (alignment, roll, velocity), `RenderSystem.cpp`
  `updateInstancedMesh` 11854, `sortInstancedMesh` 11950, `buildInstanceList` 16198, transparent sort (fudge).
- `engine/core/script/binding/ActionClassesLua.cpp:145-204` (+ module proxies, MinMax classes, enums),
  `GameObjectClassesLua.cpp` (`getParticleSystem`).
- `editor/Stream.cpp:4932/5670/9262`, `editor/Factory.cpp:3816`, `editor/Catalog.cpp` (particle properties),
  `editor/window/InspectorWindow.cpp:10871/13187`, Scene view overlay (`editor/window/SceneWindow.cpp`), gizmos
  (`editor/render/gizmo/`), `editor/command/type/CreateEntityCmd.cpp:633`, `editor/window/HierarchyWindow.cpp:493,1079`,
  `editor/mcp/tools/WorldTools.cpp:4040-4210,4868`, new `editor/unity/*`, `editor/main.cpp`, `editor/cli/CommandLine.cpp`,
  `editor/App.cpp:616` (Assets menu).

## 11 Cross-feature dependencies

- `lines-trails` (#13): `AnimationCurve`, `Gradient`, `CurveField`, `GradientField` (first to land adds them).
- `ui-scaling-text` (#21): particles inside UI (the confetti). #21 adds `ParticleSystemComponent.useUnscaledTime` as
  a top-level component field (its §7.5); this spec uses that same field (no `main.useUnscaledTime` copy: the Lua
  proxy `ps.main.useUnscaledTime` reads and writes it), whichever lands first adds it. #21's HTML
  `<particle-system>` reads Points render data; since version-2 systems default to a quad InstancedMesh target, #21
  must also read InstancedMesh instance data (position, scale, roll, colour, textureRect) — flagged to #21. The
  importer reports `ui-particle` for Coffee `UIParticle` and #21 places the imported prefab in the canvas.
- `script-lifecycle` (#17): `onParticleSystemStopped` uses the per-object callback path; `getComponentInChildren`-style
  lookups map to `getParticleSystemsInChildren`.
- `physics-3d`/`physics-2d`: gravity values read from the worlds (no change there).
- `persistence` (#20): a persistent system keeps its particles across a scene load (world space ones stay put).
- `anim-properties` (#12): clips may animate `emission.rateOverTime` etc. through the catalog paths.

## Implementation notes

Deviations and decisions taken while implementing (branch `gaps/particles`).

- **Field names**: the shape type is `shape.shapeType` and the sheet tiles `textureSheetAnimation.numTilesX/Y` (Unity's
  scripting names), everywhere (files, MCP, Lua, Inspector).
- **Control methods**: `isPlaying()`, `isPaused()`, `isStopped()`, `isEmitting()`, `isAlive()` are methods in Lua
  (the base `Action` class already has `isPlaying` as a method); `particleCount`, `time`, `randomSeed`,
  `useAutoRandomSeed`, `useUnscaledTime`, `version` are properties. `emission:getBurst(i)` / `setBurst(i, b)` are
  0-based like Unity's; `setBursts{...}` takes a table. `ps.main.useUnscaledTime` reads and writes the component's
  top-level `useUnscaledTime` (shared with ui-scaling-text).
- **Gravity source**: `main.gravitySource` (Unity 2021.2's field, Physics3D / Physics2D) picks the world whose gravity
  applies instead of detecting the scene type; new systems in 2D scenes get Physics2D, the importer reads the field.
- **Shape frame**: the engine's shapes lie in their XZ plane emitting along +Y; Unity's along +Z. The importer converts
  a Unity shape rotation as q_engine = S(q_unity) * C with C = Rx(90) * Ry(180), plus one more Rx(90) for Edge (whose
  line emits along its own +Y in both); positions, offsets, velocities and forces mirror x, scale and box
  thickness swap y and z, Z rotations and roll speeds change sign (ROTATION_SIGN in UnityParticleImport.cpp).
  Unity's disabled shape (emit along the system's +Z) becomes an enabled Cone with radius 0, angle 0 and rotation
  (90, 0, 0).
- **Prefab instance ids**: an instanced object's id is (source id XOR instance id) without the sign bit, verified on
  the game's prefab files; a scene's `stripped` placeholders may carry their own ids (older Unity versions, and every
  scene of the game): they are aliased to the instanced object through their `m_CorrespondingSourceObject` /
  `m_PrefabInstance`.
- **Importer scope**: from a scene, each topmost GameObject that has a ParticleSystem becomes a prefab (a parent
  without one is not kept); from a prefab, each root with a system in its tree. The prefab root is placed at the
  origin (a scene root also loses its rotation). Unity's start frame (stored as a share of the frames) is multiplied
  by the frame count. Only Unity's built-in meshes map to engine primitives; a model's mesh is reported `mesh-missing`
  and drawn as spheres. Noise `rotationAmount` is read as degrees (converted to radians): not verified against a
  Unity capture. Existing materials and textures in `dest` are reused unless `overwrite` (prefabs get numbered names).
  The editor and MCP write the files as one undo step (WriteFilesCmd); the CLI writes them directly with the safe
  write helpers. Two extra report codes: `auto-destruct` (CFX_AutoDestructShuriken mapped to stopAction Destroy)
  and `material-missing` / `material-unknown` (a material guid not in the project, an unknown built-in id).
- **Limit velocity** applies to the integrated velocity, not to Velocity over Lifetime's animated part (Unity's own
  behaviour differs slightly with dampen < 1).
- **Stretched billboards**: `cameraVelocityScale` is stored and imported but not applied (the camera's velocity is
  not tracked); length and speed scales are.
- **Points renderer**: a version 2 system drawing on a Points target uses world diameters for sizes and only the
  Billboard look (`check` reports `particles-points-render-mode`).
- **Creation**: GameObject > Effects > Particle System makes the single GameObject version 2 system; the old layout
  is still reachable as `PARTICLE_SYSTEM_LEGACY` (MCP `create_particles emitter_entity`) and a Points variant as
  `PARTICLE_SYSTEM_POINTS`. Add Component > Particle System makes a version 2 component without adding a renderer
  (the Inspector warns that nothing draws it and how to add a renderer).
- **Legacy stop**: `Action.stop()` on a version 2 system is Unity's `Stop(true, StopEmittingAndClear)` followed by the
  stop action.
- **create_particles**: `size_over_life` / `alpha_over_life` of a version 2 system are multipliers of the start values
  (curves from the pair), as Unity's Size/Color over Lifetime.
- **Culling mode**: only Unity's values 1 and 2 (pause when invisible) are reported `culling-mode`; the engine always
  simulates.
- **Legacy golden image**: the version 1 scene of the MCP test uses deterministic values (no random ranges) because
  Play shares `rand()` with everything else; the random presets are covered by the unit capture
  (ParticleLegacyIdentityTest, recorded from the base before the change).
- **Scene overlay**: "Simulate Layers" is a checkbox (the selected systems and their children, or every system of the
  scene); the overlay preview resets the systems and their action state when it stops instead of restoring a
  snapshot (the systems' authored values are never changed by a preview). The Inspector's Action component preview
  of a version 2 system still works as before.
- **Simulation fixes found by the tests**: the FPS frame and emission counts round with a 1e-4 tolerance (a second of
  0.01 s steps sums to 0.99999); a prewarmed system draws its start delay random like a cold one, so both take the
  same particle randoms.
- **Review fixes** (after the first handoff):
  - The upgrade reads what the version 1 target drew: Points and `instancedBillboard` quads become Billboard,
    `instancedCylindricalBillboard` VerticalBillboard, a plain instanced mesh renderMode Mesh with
    `renderer.mesh = "model"` (the GameObject's own mesh, never replaced) and alignment Local (local systems) or World;
    no on-screen size limit (maxParticleSize 0). The report has a `renderer` item.
  - A burst at `time == duration` fires at the end of every cycle (before the wrap), looping or not.
  - min/max particle size are written for Billboard only (Unity) and limit the size before a stretch; other modes are no
    longer rebuilt per camera for the limit alone. `renderer.sortPriority` is added to the shader's queue priority when
    the batch is queued (InstanceParticleRender::sortPriority, PointsComponent::particleSortPriority) instead of
    overwriting it.
  - Loop / PingPong arcs and radii advance per particle (each spawn at its offset in the step); arc, radius and noise
    scroll speeds with a random range take one value per play from the system's seed.
  - Importer: a `Blend` line is tokenized (factors in any case, line comments skipped, `[_SrcBlend] [_DstBlend]` read
    from the material); Unity's Standard particle shaders blend by `_SrcBlend`/`_DstBlend` or `_Mode`; built-in shader ids
    201 (Additive Soft), 202 (Alpha Blended Premultiply), 203 (Alpha Blended), 205 (Multiply), 210/211 (Standard
    Surface / Unlit) were added (202, 203 and 211 agree with what the game's materials using them hold; 201, 205 and 210
    are Unity's documented ids, not seen in the game). Output files of two assets with one name get numbered names;
    the notes of a shared material or texture go to every system using it; a `_MainTex` not in the project draws the
    built-in particle texture (note `texture-missing`); `m_RemovedGameObjects` is honoured; render mode None (5)
    imports with the renderer disabled; `object` matches an exact path first and refuses an ambiguous name; a `dest`
    inside the Unity project is refused; any exception from a malformed file becomes an import error (CLI exit 1).
  - `JASPERENGINE_FRAME_STATS=1` logs `particles_sim_ms` (and the alive count) with the engine's frame stats;
    `tools/fixtures/particles/bench` is the 2000-particle, six-module benchmark.
