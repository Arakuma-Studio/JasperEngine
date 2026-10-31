# Particles

The **Particle System** (component `ParticleSystemComponent`, MCP name `particlesystem`, alias
`particles`; Lua/C++ class `ParticleSystem`) follows Unity's ParticleSystem: the same modules,
field names, MinMaxCurve / MinMaxGradient values, render modes and sorting, simulated on the CPU
(every platform, editor Play, the Scene view preview and exported builds alike).

Create one with GameObject > Effects > Particle System, `create_entity type=particle_system`,
`create_particles` (presets) or Lua `ParticleSystem(scene, true)`. It is **one GameObject**:
Transform + Mesh (a 1 x 1 quad drawn with `library:unlit`, the built-in soft `particle` texture,
alpha blended, no depth write) + Instanced Mesh (the particles) + Action + Particle System, its
action targeting itself. A child GameObject with its own system is a nested system. Unity
projects import with the [Unity importer](#the-unity-importer).

## Versions

`version` 2 is the module model (everything below). A scene saved before it has `version` 1 (the
legacy initializer / modifier model): it loads and plays exactly as before, and the Inspector
offers **Upgrade to modules...** (its popup lists each legacy value as `exact`, `approximated`
with its largest difference, or `dropped`), as do MCP `create_particles {entity, upgrade: true}`
(returns the report; `force: true` drops what has no equivalent, a position modifier) and Lua
`ps:upgrade()`. The upgrade is one undo step. `jasperengine-editor check` reports
`particles-legacy` (info) for each version 1 system.

## Modules

Unity's order and defaults (a new Unity system's). Angles of the shape are degrees; rotations
and angular speeds are radians (the Inspector shows degrees); colours are linear in files and
sRGB in the Inspector, Lua and `#RRGGBB`.

| Module | Fields (`module.field`) |
|---|---|
| `main` | `duration` 5, `looping` true, `prewarm`, `startDelay`, `startLifetime` 5, `startSpeed` 5, `startSize3D`, `startSize` 1 (`startSizeY`, `startSizeZ` when 3D), `startRotation3D`, `startRotation` (`startRotationX`, `startRotationY`), `flipRotation` (0..1), `startColor`, `gravityModifier`, `gravitySource` (Physics3D, Physics2D), `simulationSpace` (Local, World), `simulationSpeed` 1, `scalingMode` (Hierarchy, Local, Shape), `playOnAwake` true, `maxParticles` 1000, `emitterVelocityMode` (Transform, Rigidbody), `stopAction` (None, Disable, Destroy, Callback), `autoRandomSeed` true, `randomSeed` |
| `emission` | `enabled`, `rateOverTime` 10, `rateOverDistance`, `bursts` [{`time`, `count` (a curve value), `cycles` (0 = forever), `interval`, `probability`}] |
| `shape` | `enabled`, `shapeType` (Sphere, Hemisphere, Cone, ConeVolume, Box, BoxShell, BoxEdge, Circle, Edge, Rectangle, Donut), `radius` 1, `radiusThickness` 1 (0 surface .. 1 volume), `radiusMode` / `radiusSpread` / `radiusSpeed`, `angle` 25, `length` 5, `arc` 360, `arcMode` (Random, Loop, PingPong, BurstSpread) / `arcSpread` / `arcSpeed`, `boxThickness`, `donutRadius`, `position`, `rotation` (Euler degrees, X then Y then Z), `scale`, `alignToDirection`, `randomDirectionAmount`, `sphericalDirectionAmount`, `randomPositionAmount` |
| `velocityOverLifetime` | `x`, `y`, `z`, `orbitalX/Y/Z` (rad/s), `orbitalOffsetX/Y/Z`, `radial`, `speedModifier` 1, `space` |
| `limitVelocityOverLifetime` | `separateAxes`, `limit` (`limitX/Y/Z`), `space`, `dampen` 1, `drag`, `multiplyDragByParticleSize`, `multiplyDragByParticleVelocity` |
| `inheritVelocity` | `mode` (Initial, Current), `curve` |
| `forceOverLifetime` | `x`, `y`, `z`, `space`, `randomized` (a new random each frame) |
| `colorOverLifetime` | `color` (a gradient, white) |
| `colorBySpeed` | `color`, `range` (speeds mapped to 0..1) |
| `sizeOverLifetime` | `separateAxes`, `size` (x), `y`, `z` (curves 0 -> 1) |
| `sizeBySpeed` | the same + `range` |
| `rotationOverLifetime` | `separateAxes`, `x`, `y`, `z` (45 deg/s) |
| `rotationBySpeed` | the same + `range` |
| `noise` | `separateAxes`, `strength` (`strengthY`, `strengthZ`), `frequency` 0.5, `scrollSpeed`, `damping` true, `octaves` 1..4, `octaveMultiplier`, `octaveScale`, `positionAmount`, `rotationAmount`, `sizeAmount` |
| `textureSheetAnimation` | `mode` (Grid, Sprites), `numTilesX`, `numTilesY`, `animation` (WholeSheet, SingleRow), `rowMode` (Custom, Random), `rowIndex`, `timeMode` (Lifetime, Speed, FPS), `frameOverTime` (0..1 of the frames), `fps`, `speedRange`, `startFrame` (frames), `cycles`, `flipU`, `flipV`, `sprites` (frame ids) |
| `renderer` | `renderMode` (Billboard, Stretch, HorizontalBillboard, VerticalBillboard, Mesh), `cameraVelocityScale`, `velocityScale`, `lengthScale` 2, `alignment` (View, World, Local, Facing, Velocity), `sortMode` (None, Distance, OldestInFront, YoungestInFront), `sortingFudge`, `sortPriority` (Unity's Order in Layer), `minParticleSize`, `maxParticleSize` 0.5 (shares of the viewport height), `pivot`, `flip`, `mesh` (quad, cube, sphere, cylinder, capsule, plane) |

Plus the component's `useUnscaledTime` (it keeps playing at time scale 0, UI particles) and
`version`. <a id="fields"></a>Every field is a property: `set_properties
{entity, values: {particlesystem: {"main.startLifetime": {"mode": "two_constants", "min": 0.8,
"max": 1}}}}`, `get_entity`, prefab overrides, animation clips (`emission.rateOverTime`).

### MinMaxCurve and MinMaxGradient

| Form (YAML / MCP JSON) | Meaning |
|---|---|
| `2.5` | Constant |
| `{mode: two_constants, min: 0.8, max: 1}` | random between two constants (picked per particle) |
| `{mode: curve, multiplier: 2, curve: {keys: [[0, 0], [1, 1]]}}` | a curve over the normalized time, times the multiplier |
| `{mode: two_curves, multiplier: 1, min: <curve>, max: <curve>}` | random between two curves (one blend per particle for its life) |
| `"#FF8000"`, `"#FF800080"`, `[r, g, b, a]` (linear) | Color |
| `{mode: gradient, gradient: {colors: [[0, "#FFFFFF"], [1, "#FF0000"]], alphas: [[0, 1], [1, 0]]}}` | Gradient (over the time) |
| `{mode: two_colors, min: <colour>, max: <colour>}`, `{mode: two_gradients, min, max}`, `{mode: random_color, gradient}` | the random ones |

Curves and gradients are those of [lines-trails.md](lines-trails.md) (AnimationCurve, Gradient).
The time is the particle's age / lifetime for the over-lifetime modules and the system's time /
duration for the `main` start values, the emission rates and the gravity (Unity's).

## How it plays

- `play()` starts a stopped system at time 0 (a new seed when `autoRandomSeed`), resumes a paused
  one; a looping system with `prewarm` starts as if one whole cycle had played. `stop()` stops
  the emission, the particles live out, then the system is stopped and `stopAction` runs
  (Disable / Destroy the GameObject, or `onParticleSystemStopped()` on its Lua scripts);
  `StopEmittingAndClear` removes them at once. `startDelay` holds the first emission.
- In Play, `playOnAwake` systems start when the scene starts or their GameObject becomes active;
  a deactivated GameObject stops and clears its system.
- Time: the scaled frame delta (`Engine.timeScale`) times `main.simulationSpeed`, or the unscaled
  one with `useUnscaledTime`; frames longer than 0.1 s are split, so bursts, limits and drag stay
  stable after a hitch. Spawns of a frame spread over it (the emitter's movement interpolated),
  so fast emitters and rate over distance leave even trails. A jump of more than 100 units in a
  frame does not emit over distance.
- Start speed goes along the shape's direction: up (+Y) for cones, boxes, rectangles and edges,
  outwards for spheres, circles and donuts. Cones, hemispheres and circles turn around local +Y;
  circles, rectangles and donuts lie in the XZ plane.
- Gravity: `gravityModifier` x the scene's 3D gravity (default (0, -9.81, 0)), or with
  `gravitySource` Physics2D the 2D world's in the scene's units (Box2D gravity x points per meter;
  the 2D presets and the menu in 2D scenes use it).
- Randomness is deterministic per system (PCG32): with `autoRandomSeed` off, `randomSeed` replays
  the same particles (tests, previews).
- Limit velocity's `dampen` is applied per 1/30 s, so it looks the same at any frame rate (Unity
  applies it per frame: at other frame rates the two differ slightly). Noise is a 3D gradient
  noise of its own (Unity's pattern is not reproducible); its parameters mean the same.

## Rendering

The particles are instances of the GameObject's mesh, built for each camera:

| Render mode | Particle orientation |
|---|---|
| Billboard | `alignment`: View (facing the camera plane), World, Local (the GameObject's rotation), Facing (towards the camera position), Velocity |
| Stretch | along its velocity, turned to the camera: length = size x `lengthScale` + speed x `velocityScale` |
| HorizontalBillboard | lying flat (XZ), turned by its rotation |
| VerticalBillboard | upright, facing the camera around world Y |
| Mesh | the `mesh` (or a model the importer put there) with the 3D rotation, `alignment` as Billboard's |

`minParticleSize` / `maxParticleSize` keep each particle's projected size within those shares
of the viewport height. Sorting: `Distance` back to front, `OldestInFront` / `YoungestInFront`
by age, `None` in spawn order; among other transparent objects the system sorts by its distance
plus `sortingFudge` (lower draws on top), after `sortPriority`. The texture: the material's base
map (`u_baseTexture` of `library:unlit`; additive presets set the material's blend to additive);
`textureSheetAnimation` picks a tile of it per particle (row 0 at the top), or a sprite frame of
the GameObject's sprite frames in Sprites mode. A system drawn on a Points GameObject
(`create_particles renderer: points`, or the legacy layout) draws billboards only
(`particles-points-render-mode` warns otherwise).

## MCP

- `create_particles`: `preset` fire, smoke, sparks, explosion, snow, rain, magic (3D meters; 2D
  scenes scaled by `scale`, 40 px per unit by default, flat, 2D gravity); `life`, `speed` (along
  the shape), `size`, `velocity` / `acceleration` (a value or `{min, max}`: velocity over
  lifetime / force), `color`, `gradient` (colour over lifetime), `alpha_over_life`,
  `size_over_life` (multipliers of the start size), `bursts`, `emitter` {shape, radius, angle,
  length, arc, min, max, thickness}, `rate`, `max_particles`, `loop`, `duration`, `texture`,
  `autoplay` (playOnAwake), `modules` (raw fields: `{"main": {"startSpeed": 3}}`), `renderer`
  quads | points. `entity` reconfigures one; `upgrade: true` converts a version 1 system;
  `emitter_entity` makes a legacy (version 1) system drawing on an existing Points / Mesh.
- `get_entity` in Play adds `particles: {count, time, isPlaying, isPaused, isEmitting, seed}`.
- `import_unity_particles`: [the importer](#the-unity-importer).
- `check`: `particles-points-render-mode`, `particles-max-exceeded-budget` (more than 5000 max
  particles), `particles-legacy`.

## Lua

```lua
local ps = self.gameObject:getParticleSystem()
ps.emission.rateOverTime = 0                     -- Unity's ps.emission.rateOverTime = 0
ps.main.startColor = MinMaxGradient(Vector4(1, 0.5, 0, 1))
ps.main.startLifetime = MinMaxCurve(0.8, 1.0)
ps.emission:setBursts({{time = 0, count = 10}})
ps:play(); ps:emit(5); ps:stop()
function MyScript:onParticleSystemStopped() GameObject.destroy(self.gameObject) end
```

The whole API: [lua-api.md](lua-api.md) (4.16). Unity's `isPlaying` / `isStopped` properties are
the methods `isPlaying()` / `isStopped()` here (as every Action's).

## The editor

The Inspector shows Main, then each module with its enable checkbox (the "+ Module" menu turns
others on); MinMaxCurve rows have a mode arrow (Constant, Curve, Random Between Two Constants,
Random Between Two Curves: one editor, the band between the curves shaded), gradients likewise.
The Action component's Play / Pause / Stop previews the system in the Scene view in edit mode,
with a fixed seed so a restart repeats; the scene goes back to its state when the preview stops.

## The Unity importer

Unity ParticleSystems (`.prefab` or `.unity` files of a Unity project) import as engine prefabs:
**Assets > Import Unity Particle System...**, MCP `import_unity_particles {path, object?, dest = "assets/fx",
unity_project?, dry_run, overwrite}` or the CLI:

```
jasperengine-editor import-unity-particles <file> --project <engine project> [--object <path>]
    [--dest assets/fx] [--unity-project <dir>] [--report out.json] [--overwrite] [--dry-run]
```

(exit 0 imported, 1 error, 2 imported with notes). The Unity project (the folder holding `Assets/` and
`ProjectSettings/` around the file) is only read.

- **What is imported**: by default every root GameObject with a ParticleSystem in itself or its children (in a scene:
  each topmost GameObject that has one); `object` picks one by path (`Player/Camera/EndParticles`) or name. Each
  becomes `dest/<name>.prefab` (a numbered name when one exists, unless `overwrite`): the GameObject tree with its
  transforms, each ParticleSystem a version 2 system on its own GameObject. Prefab instances in the file are resolved
  with their overrides (property paths, array elements, removed components), nested prefabs too.
- **Axes**: Unity is left handed: positions and velocities mirror x, rotations become (w, x, -y, -z), billboard rolls
  and Z rotation speeds change sign, shape rotations are converted to the engine's shape frame (a cone rotated
  (-90, 0, 0) in Unity emits up here too).
- **Colours**: Unity's colours are gamma (sRGB): stored linear here (they look the same).
- **Materials**: one `.material` per Unity material in `dest/materials/` drawn with `library:unlit`: blend from the
  shader (Unity's particle shaders by name; a custom shader by its `Blend` line: `One One` additive, `DstColor ...`
  multiply, `SrcAlpha OneMinusSrcAlpha` alpha, none with `ZWrite On` opaque, reported `shader-approximated`),
  `_MainTex` (with its tiling), `_TintColor` (x 2 in Unity's legacy particle shaders) or `_Color`, `_InvFade` of a
  soft shader as the `SOFT` feature. Unity's default particle material and texture use the engine's built-in soft
  particle texture (`PARTICLE_TEXTURE`).
- **Textures**: copied to `dest/textures/` (PNG/JPG/TGA; PSD/EXR/TIFF are reported `texture-format`: convert them);
  Alpha8 textures become white with their alpha. Existing materials and textures are kept unless `overwrite`.
- **Meshes**: Unity's built-in Cube/Sphere/Cylinder/Capsule/Plane/Quad become the engine primitives; a model's mesh
  is reported `mesh-missing` (spheres meanwhile): import the model and set `renderer.mesh`.
- **Scripts**: `CFX_AutoDestructShuriken` sets `main.stopAction` Destroy; Coffee `UIParticle` is reported
  `ui-particle` (put the prefab under a UI element).
- **Report** (`dest/<name>.import-report.json`, the Console, the MCP result): per system its modules
  (`imported`, `approximated`, `unsupported`, `disabled`) and notes: `module-unsupported` (sub emitters, collision,
  triggers, lights, trails, custom data, external forces when enabled), `shape-unsupported` (mesh, sprite, texture
  shapes: a cone instead), `noise-pattern` (same parameters, another noise), `weighted-tangents`, `culling-mode`,
  `ring-buffer`, `custom-simulation-space`, `override-unknown`, `texture-format`, `mesh-missing`, `ui-particle`,
  `shader-approximated`.
- **Licensing**: Asset Store content (e.g. JMO WarFX) may be imported for your own game but not redistributed: keep
  it out of public repositories.

## Costs

The simulation is O(alive particles x enabled modules) on one thread: about 2,000 particles with
6 modules fit in 1.5 ms a frame on a 2019 mid-range phone (the over-lifetime curves are baked
into 64-entry tables; noise is one 3D noise per particle and octave). One instanced draw per
system; camera-facing modes rebuild their instance matrices per camera. Memory: about 120 bytes
per particle of capacity plus 96 per instance. `check` warns above 5000 max particles.

## Legacy (version 1)

Version 1 systems keep the initializer / modifier fields (`lifeInitializer`, `velocityInitializer`,
`sizeModifier`, ...), draw on a Points or Mesh GameObject they target, and use `rand()`. Their
setters still work in Lua; on a version 2 system they set the module fields they map to, with
a deprecation warning.
