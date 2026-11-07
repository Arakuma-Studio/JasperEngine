# Particles

A **Particle System** (`ParticleSystemComponent`, Unity's ParticleSystem) lives on a GameObject
with a Transform and renders itself. What it emits is an **effect**: a `.particles` asset (JSON,
shared by every system that plays it) or the system's own **inline effect** (the same schema,
for one-off emitters and migrated scenes). The simulation runs on SIMD kernels over the job
system at a fixed tick (30 Hz by default) and is interpolated per frame, so it is deterministic
for a given seed and frame-rate independent.

## Creating one

- `create_particles {preset: fire|smoke|sparks|explosion|snow|rain|magic|none, position, ...}`
  makes a Particle System GameObject with an inline effect (the overrides `rate`, `life`,
  `size`, `velocity`, `color`, `gradient`, `bursts`, `emitter {shape...}` work as before).
  Given `entity`, it reconfigures that system's effect; `emitter_entity` puts the system on that
  GameObject (a Mesh one emits from its mesh, the MeshComponent disabled); `duration`, `speed` and
  `modules` (a merge patch of the effect's modules, as edit_particle_effect's) set the effect.
- `create_particle_effect {path: "assets/fx/fire.particles", preset}` writes an asset;
  `set_particle_effect {entity, effect: "fx/fire.particles"}` plays it (`effect: "inline"` goes
  back to an inline copy).
- The editor: GameObject > Effects > Particle System; the Inspector's Save as Asset / Make
  Inline buttons move an effect between the two forms.

## The effect JSON

`get_particle_effect {entity | path}` returns the keyed form, defaults left out:

```json
{
  "version": 1, "duration": 2, "looping": true, "tickRate": 30, "maxParticles": 500,
  "simulationSpace": "world",
  "modules": {
    "main": {"startLifetime": {"mode": "twoConstants", "constant": 1, "constantMax": 2}, "startSpeed": 3,
             "startColor": [1, 0.6, 0.2, 1], "gravityModifier": 0.5},
    "emission": {"rateOverTime": 40, "bursts": [{"time": 0, "count": 30}]},
    "shape": {"shape": "cone", "angle": 20, "radius": 0.3},
    "colorOverLifetime": {"color": {"mode": "gradient",
                                    "gradient": {"colorKeys": [[0, 1, 1, 1], [1, 1, 0.3, 0.1]], "alphaKeys": [[0, 1], [1, 0]]}}},
    "sizeOverLifetime": {"size": {"mode": "curve", "curve": [[0, 1], [1, 0]]}},
    "renderer": {"renderMode": "stretchedBillboard", "texture": "fx/spark.png", "blend": "additive"}
  },
  "subEmitters": [{"trigger": "death", "effect": "fx/pop.particles", "emitCount": 5}],
  "exposed": [{"name": "Intensity", "path": "emission.rateOverTime", "type": "float"}]
}
```

- A **curve value** (MinMaxCurve) is a number, or `{mode: twoConstants, constant, constantMax}`,
  `{mode: curve, curve: [[time, value(, inTangent, outTangent)], ...], multiplier}` or
  `twoCurves` with `curveMax`; `driver` (normalizedAge, age, speed, emitterTime, random) with
  `driverMin`/`driverMax` and `wrap` picks what the time axis follows.
- A **colour value** (MinMaxGradient) is `[r, g, b, a]` (sRGB), or `{mode: twoColors | gradient |
  twoGradients, color, colorMax, gradient, gradientMax}`; a gradient is
  `{colorKeys: [[t, r, g, b]], alphaKeys: [[t, a]]}` (at most 8 of each).
- Modules (Unity's names and order): main, emission, shape, startVelocity,
  velocityOverLifetime, limitVelocity, forceOverLifetime, colorOverLifetime, colorBySpeed,
  sizeOverLifetime, sizeBySpeed, rotationOverLifetime, rotationBySpeed, externalForces, noise,
  orbit, collision, subEmitters, textureSheetAnimation, trails, customData, setOverAge, renderer.
  Main and Renderer are always there; the others are present or not, each with `enabled`.
- Unity's fields origin/main's systems had are S20's too: the effect's `startDelay` (a number or
  a random range picked per play), `useUnscaledTime`, `stopAction` (none, disable, destroy,
  callback: `onParticleSystemStopped` in scripts), `emitterVelocityMode` (transform, rigidbody);
  Main's `gravitySource` (physics3D, physics2D) and `inheritVelocityMode` (initial, current);
  Shape's `radiusMode`/`arcMode` (random, loop, pingPong, burstSpread) with their `Spread` and
  `Speed`, `alignToDirection`; Velocity over Lifetime's `offsetX/Y/Z` (the orbit's centre, curves);
  Limit Velocity's `separateAxes` (`limit` is X, `limitY`, `limitZ`) and `space`; Rotation by
  Speed's `separateAxes` (`angularVelocityX/Y`); the noise's `scrollSpeed` curve and up to 4
  `octaves`; Texture Sheet `flipU`/`flipV`; burst counts as curves over the emitter's time; the
  gradient mode `randomColor`; Renderer `velocityAligned` billboards, `meshAlignment`
  (simulation, view, world, local, facing, velocity) and `textureTiling` [x, y, offset x, offset y].

## Editing

- `edit_particle_effect {entity | path, patch}`: an RFC 7386 merge patch of the keyed form,
  one undo step. `null` removes a key (a module: removes it); arrays are replaced whole. A patch
  that leaves the effect invalid is refused with its diagnostics `[{severity, path, message}]`.
- `add_particle_module {module, fields}` / `remove_particle_module {module}`.
- A GameObject with an asset edits the **asset**: every system playing it changes. Make it
  inline first (`set_particle_effect effect: "inline"`) to change one GameObject only.
- An edit that changes values only keeps the live particles (the system rebinds); one that
  changes the structure (modules, capacity, trails) or the seed restarts it.

## Playing and checking

- In Play, a system with `playOnAwake` (create_particles `autoplay`) starts with the scene.
- `control_particles {entity, action: play|pause|stop|clear|restart|emit|seek|step}` works in
  Play and in Edit mode, where the Scene view's Particle Effect preview runs the system (the
  selected systems play there too; the panel's Playback Time slider scrubs).
- `get_particle_stats` reports particle_count (sub-emitter children included), time, ticks, the world budget, simulate/render µs,
  culled; `capture_particles {entity, time, camera: scene|game}` seeks and returns an image.
- Both report `seed` (the one in use, also what a seedless effect drew for this play) and `state_hash`:
  put the seed in the effect (`edit_particle_effect {patch: {seed}}`) and the same time gives the
  same hash, which is how to tell that two runs are the same one.
- `get_particle_migration_report` lists the old particle systems (Points + Particle System action)
  the documents read were converted from; saving writes the new format.

## Lua

```lua
local ps = self.fire           -- a script property of type ParticleSystem, or ParticleSystem(scene, entity)
ps:play()
ps:emit(20, {position = Vector3(0, 1, 0), color = Vector4(1, 0.5, 0.2, 1)})  -- born at the next tick
ps:seek(1.5)
ps:stop(true)                  -- true also clears the particles
ps:setFloat("Intensity", 3)    -- an exposed parameter of the effect
ps:sendTrigger("boom")         -- raises the effect's Trigger sub-emitters named "boom"
ps:onCollision(function(events)
    for i = 1, #events do
        local e = events[i]    -- position, normal, velocity, particleId, killed
        print(e.position.y)
    end
end)
```

Unity's module access works on the effect (inline: the GameObject's own; an asset: copied inline
first, said once): `ps.main.startSpeed = 4` (a number, `MinMaxCurve`, `{min, max}`),
`ps.emission.rateOverTime`, `ps.shape.radius`, `ps.noise.enabled = true`; Unity's names map
(`startLifetime`, `startRotation` in radians as in Unity, `...Multiplier` suffixes,
`emission:getBurst(i)` / `setBurst(i, {time, count, cycles, interval, probability})` /
`burstCount`); the enums are tables of the effect's names (`ParticleSystemShapeType.Cone`,
`ParticleSystemSimulationSpace.World`, ...). `ps:play(true)` / `ps:stop(true, ParticleSystemStopBehavior.StopEmittingAndClear)` /
`ps:clear(true)` take the children too; `GameObject:getParticleSystemsInChildren()`.

Properties: `isPlaying`, `isPaused`, `isEmitting`, `particleCount` (with its sub-emitter children), `time`, `playbackSpeed`,
`seed` (0: a new seed each play), `playOnAwake`, `effect` (asset path, "" inline),
`useUnscaledTime` (the GameObject's own Unscaled Time, beside the effect's: it plays at
`Engine.timeScale` 0; UI particles, jasper://docs/ui).
`onCollision`, `onParticleDeath`, `onSubEmit` deliver one batch per frame on the main thread
(collisions capped by the Collision module's `maxCollisionEvents`). The old setters
(`rate`, `setLifeInitializer`, `addBurst`, `start()`...) still work on the inline effect and
log a deprecation warning once.

## From origin/main and Unity

- **origin/main's version 2 systems** (its Unity-shaped `particlesystem: {version: 2, main: ...}`
  on a GameObject drawing its own quads) become S20 effects when their scene or prefab loads, with
  every module value (radians to degrees, linear colours to sRGB, the shape's rotation order) and
  their quad material in the Renderer (base map, base colour as `tint`, tiling, soft particles,
  blend); the quad MeshComponent and InstancedMesh go (a Mesh-mode system keeps its MeshComponent,
  disabled, as its mesh). Prefab instance overrides convert the same way. What does not carry over
  is in the load report (`get_particle_migration_report`, the log).
- **import_unity_particles {path, object?, dest?, dry_run?, overwrite?}** (the particles group; and the CLI
  `import-unity-particles <file> --project <dir>`, Assets > Import Unity Particle System...)
  turns a Unity `.prefab` / `.unity`'s ParticleSystems into prefabs of S20 systems (inline
  effects; the renderer's material in the Renderer module, its texture copied) and a report:
  per module imported / approximated / unsupported, notes with codes (`shader-approximated`,
  `noise-pattern`, `mesh-missing`, ...). The Unity project is only read; undo removes the files.

## Force fields

A **Force Field** GameObject (`create_entity type: force_field`; shapes sphere, box, cylinder,
infinite; kinds directional, radial, vortex, turbulence, drag; a falloff curve) pushes the
particles of systems whose External Forces module shares one of its layers.

## Performance

Budgets: the world keeps at most its particle budget alive, sub-emitter children included (higher
`priority` effects keep more; `particles.world_budget` in `set_project_settings`, 0 for the platform's default),
Automatic culling pauses off-screen emitters and catches up when they come back, Distance LOD
thins far ones. Collision against the world (`collision.collisionType: world`) is the costly module:
Medium quality caches planes and caps the ray casts per tick (`maxQueriesPerTick`). Sorting
(`renderer.sortMode`) costs a sort per view; additive particles need none.
