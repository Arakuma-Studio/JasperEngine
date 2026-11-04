# S20 — Particles

Wave 2. Owner stream: engine rendering + editor. Status: spec.

## 1. Goal

Replace our one-from→to-modifier-per-property particle action with a **module stack over SoA
channels** (Unity Shuriken-style modules, adapted from OpenEngine's processor stack). It covers
curves and gradients over lifetime, forces (curl noise, drag, limit speed, orbit, force fields),
collisions with events, trails and ribbons, sub-emitters, stretched/oriented billboards and mesh
particles, sprite-sheet frame blending, six-way smoke lighting, a deterministic fixed tick with
interpolation, prewarm and editor scrubbing. Effects become reusable assets. The user's
headline requirement is **performance**: the runtime is built around SIMD kernels over contiguous
arrays, the S06 job system, a 32-byte packed instance format and zero steady-state allocations,
with benchmark-gated budgets on desktop, mobile and web. Our soft-particle, near-fade, erosion
and glow shader features stay.

## 2. Scope

**In scope**
- `engine/core/particles/`: effect document + compiler, SoA channel store, module kernels,
  emitter runtime (fixed tick, interpolation, prewarm, seek), world scheduler (budgets, LOD,
  culling, sub-emitter routing), collision providers, trail store, render extraction/packing,
  radix sort, SIMD abstraction (`engine/core/math/simd/`).
- New render path: instanced camera-facing quads, stretched quads, mesh particles and ribbon
  strips; shader type `particles` with library shaders; six-way lighting; frame blending.
- `.particles` effect asset, `ParticleSystemComponent` redesign, `ForceFieldComponent`,
  migration of every existing particle system.
- Editor module inspector, curve/gradient fields (S27 widget), Scene-view particle overlay
  (play/pause/restart/scrub/speed/stats), MCP tools, Lua API, docs.

**Out of scope (decided)**
- **GPU simulation.** No compute, no transform feedback, no float-texture ping-pong path in this
  spec (README: no compute requirement; sokol exposes no transform feedback). CPU SIMD + jobs must
  meet the budgets in §5. Revisit only if AC benchmarks fail with the CPU path finished.
- **Depth-buffer collision.** It needs a GPU→CPU depth readback. sokol has no portable async
  readback, and a synchronous one stalls WebGL2 and tiled mobile GPUs. Physics, heightfield and
  plane colliders plus a cached contact plane (R18) replace it.
- Particle lights (scene limit `MAX_LIGHTS 6`, `engine/core/Engine.h:17`), the OpenEngine
  multi-phase state machine (`StackPhase`, `kMaxStackPhases`), a node/graph VFX editor,
  half-resolution particle compositing (follow-up; it interacts with S13 dynamic resolution),
  self-shadowing/shadow casting, GPU fog particles (separate small item, Unlicense port).
- Their render graph, GPU-driven culling and SSBO instance buffers (README do-not-copy list).

## 3. Current state

- **Data**: `engine/core/component/ParticleSystemComponent.h` (293 lines). It holds AoS
  `ParticleData` (life, position, velocity, acceleration, quaternion, scale, time; ~76 B), one
  initializer plus one `{fromTime,toTime,from,to,Ease}` modifier per property, a colour gradient,
  bursts, rate, loop, localSpace and maxPerUpdate. `engine/core/action/ParticleSystem.{h,cpp}` is
  an `Action` wrapper with ~70 setters, bound to Lua at
  `engine/core/script/binding/ActionClassesLua.cpp:140`.
- **Topology**: a particle system is an *action entity* (`ActionComponent` +
  `ParticleSystemComponent`) whose `action.target` is a `PointsComponent` or
  `InstancedMeshComponent` entity (`editor/command/type/CreateEntityCmd.cpp:601-606`).
- **Simulation**: `engine/core/subsystem/ActionSystem.cpp:674-1300`, main thread only, with two
  copy-pasted paths (Points / InstancedMesh). Each frame it walks *every* slot up to
  `maxParticles`, dead ones too (`:1167`). `findUnusedParticle` is a linear scan per spawn
  (`:674`), so a burst is O(n²) in the worst case. The integrator is velocity-Verlet with a
  per-particle acceleration (`:1008-1015`). Modifier semantics: inside `[fromTime,toTime]` seconds
  of age the property is *set* to `lerp(from,to,ease(t))`; outside, it keeps its last value
  (`:832-848`). The variable frame `dt` makes results frame-rate dependent and non-reproducible.
  There is no prewarm. The animation-window seek re-simulates incrementally
  (`editor/window/AnimationWindow.cpp:1116`).
- **Rendering**: Points use `gl_PointSize` (`engine/shaderlib/includes/points_vs.glsl`). That gives
  square sprites only, clamped to the GL/GLES point-size range. D3D11 has no point size, and
  `SokolObject.cpp:133` maps `POINTS` straight to the primitive with no emulation. Each particle
  exists three times: `ParticleData`, `PointData` and `PointRenderData` (13 floats = 52 B,
  `RenderSystem.cpp:9624-9629`). `sortPoints` runs `std::sort` with two matrix multiplies per
  comparison every frame (`RenderSystem.cpp:10571-10577`).
- **Shader niceties to keep**: `engine/shaderlib/library/particle.frag`, with GLOW, EROSION, SOFT
  (scene-depth soft particles via `library/soft_fade.glsl`) and NEAR_FADE.
- **Editor/agents**: field-only inspector (`editor/window/InspectorWindow.cpp:10191`), YAML
  codec (`editor/Stream.cpp:4436`, `:5075`), C++ export (`editor/Factory.cpp:3441`), and the MCP
  `create_particles` tool with hard-coded presets (`editor/mcp/tools/WorldTools.cpp:3469-3780`,
  `:4411`).
- **Missing**: curves, noise/drag/limit/orbit, force fields, collision, events, sub-emitters,
  trails, billboard modes, stretch, mesh orientation, frame blending, lit/six-way lighting, fixed
  tick, reusable assets, budgets/LOD, threading and SIMD.

## 4. Reference (OpenEngine, MIT; copied code keeps its header plus a `NOTICE.md` entry)

Paths are under `external/OpenEngine/Engine/Modules/`.

| Area | Files | Take / improve |
|---|---|---|
| Channels | `Particles/Include/Particles/ParticleChannels.h`, `Source/ParticleChannels.cpp` | Take the idea: channels allocated per declared mask, fixed capacity, `RemoveSwapBack`, debug undeclared-read asserts. **Improve**: theirs stores `Vector3`/`Vector4` per element (AoS inside a channel). Ours splits components into separate float arrays, aligned to 16 B and padded to a multiple of 8, so 4-wide SIMD needs no shuffles. |
| Values | `Particles/Include/Particles/ParticleValue.h`, `Source/ParticleValue.cpp` | Take `ParticleValueMode` (Constant/Range/Curve/CurveRange), the drivers, the wraps and `ParticleRandom` (stateless hash of seed/particle/processor/stream). **Improve**: curves are baked to 64-entry LUTs at compile time, not evaluated as key lists per particle. |
| Runtime | `Particles/Include/Particles/ParticleRuntime.h`, `Source/ParticleRuntime.cpp` | Take fixed-tick accumulate (`kMaxStepsPerFrame`), interpolation factor, chunked `BeginSeek/ContinueSeek`, `QueueSpawn`, collision records and dropped-time stat. **Improve**: theirs is single-threaded, caps 4096 per emitter (`kMaxParticlesPerEmitter`) and runs processors over index batches (`std::span<const uint32>`). Ours runs over contiguous ranges, in parallel chunks, up to 2^20 per emitter. |
| Processors | `Particles/Source/Processors/{EmitRate,EmitBurst,Shape,VelocityCone,Acceleration,Drag,LimitSpeed,Noise,Orbit,Collision,Event,Trail,Property}Processor.cpp` | Port the maths into our SIMD kernels. Their parameter blocks inform our module fields. |
| Noise | `Noise/Include/Noise/GradientCurl3D.h` (+ `Tests/GradientCurl3DTests.cpp`, `NoiseGoldenTests.cpp`) | **Copy** the scalar version as the reference implementation and golden source. Write a 4-wide SIMD version that matches it within 1e-5. |
| Trails | `Particles/Include/Particles/ParticleTrailStore.h`, `Source/ParticleTrailStore.cpp`, `Processors/ParticleTrailProcessor.cpp` | Take the fixed ring pool, the spacing rule (replace head when closer than spacing) and fade-after-death. Store the rings as SoA. |
| Document | `Particles/Include/Particles/ParticleStackDocument.h`, `Source/{ParticleStackDocument,ParticleStackValidation,ParticleStackCompiler}.cpp` | Take validation with path diagnostics, `HaveSameStructure` (param-only edits keep live particles) and `ContinuousEmissionPeak` (capacity hint). Drop phases. |
| Rendering | `Particles/Include/Components/Rendering/ParticleRenderer.h`, `Source/Rendering/ParticleExtraction.cpp`, `Rendering/Shaders/Includes/particle_six_way.glsl`, `Rendering/Shaders/Particles/particle_vertex.glsl` | Take the billboard modes, draw orders, velocity stretch, frame blending, the six-way decode layouts and response, and distance thinning. **Improve**: their `ParticleRenderData` is 128 B per particle in an SSBO. Ours is 32 B as per-instance vertex attributes. |
| Editor | `Apps/Editor/Source/Inspectors/Particles/*` | Behaviour reference only (UI toolkit is do-not-copy). |
| Docs | `docs/rendering/particles.mdx` ("Limits") | Their stated limits (CPU only, tick-aligned timing, ray instead of sphere sweep, no self-shadow) are accepted here too. |

Licence: all MIT. Copy into `engine/core/particles/` and `engine/core/math/noise/` with their
headers, plus a `NOTICE.md` entry.

## 5. Requirements

### Functional

- **R1. Effect asset.** A `.particles` file (JSON, schema `version: 1`) holds `duration`,
  `looping`, `prewarm`, `startDelay`, `simulationSpace` (Local/World), `tickRate` (Hz, default
  30; 0 = variable step), `interpolate`, `maxParticles`, `seed`, `scalingMode`
  (Hierarchy/Local/Shape), the ordered module list, `subEmitters[]` (effect references) and
  `exposed[]` (named parameters bound to module fields). It has an S04 GUID. A component
  references it, or holds an **inline** effect (same schema) for migrated or one-off emitters.
  "Save as Asset" converts inline to an asset.
- **R2. Module set (Unity names and order).** Each module has an enable checkbox. Values are
  `MinMaxCurve` (Constant / Random Between Two Constants / Curve / Random Between Two Curves)
  or `MinMaxGradient` (Color / Gradient / Random Between Two Colors / Random Between Two
  Gradients). Curve drivers: Normalized Age (default), Age (s), Speed, Emitter Time, Random.
  Wraps: Clamp/Repeat/PingPong.
  - *Main*: Start Lifetime, Start Speed, Start Size (uniform or 3D), Start Rotation (2D or 3D),
    Start Color, Gravity Modifier, Inherit Velocity.
  - *Emission*: Rate over Time, Rate over Distance, Bursts (time, count, cycles, interval,
    probability).
  - *Shape*: Sphere, Hemisphere, Cone (+volume), Box (volume/shell/edge), Circle, Donut, Edge,
    Mesh (vertex/edge/triangle-area from a model asset). Radius thickness, arc, randomize
    direction, spherize direction, and a transform offset.
  - *Start Velocity* (legacy-compatible): random between two vectors, in simulation space.
  - *Velocity over Lifetime* (linear XYZ, orbital XYZ, offset, radial, speed modifier), *Limit
    Velocity* (limit curve, dampen, linear drag, quadratic drag, multiply by size/velocity),
    *Force over Lifetime* (XYZ, space, randomize per tick or per particle), *Noise* (curl;
    strength per axis, frequency, scroll speed, octaves 1–3, octave multiplier/scale, damping,
    position/rotation/size amounts, quality High = 3D curl, Medium = 2-potential, Low = cached
    LUT field), *Orbit* (axis, center, angular speed, radial pull), *External Forces* (multiplier,
    layer mask; samples `ForceFieldComponent`s, R6).
  - *Color over Lifetime*, *Color by Speed*, *Size over Lifetime*, *Size by Speed*, *Rotation
    over Lifetime*, *Rotation by Speed*.
  - *Texture Sheet Animation*: Grid (tiles X/Y, whole sheet or single row, random row) or
    Sprites (explicit rects, ≤ 64, from `SpriteFrameData`). Time mode Lifetime curve / FPS /
    Speed range. Start frame, cycles, **frame blending**.
  - *Collision* (R7), *Trails* (R9), *Sub Emitters* (R10), *Custom Data* (4 floats from curves,
    passed to custom shaders), *Renderer* (R11).
  - *Set over Age* (legacy-compatible): sets Position/Velocity/Acceleration/Color/Alpha/Size/
    Rotation/Scale to a curve over Age (s) inside an active window `[from,to]`. Outside the
    window the value is left as it is (old semantics, `ActionSystem.cpp:832-848`).
- **R3. Combine rules.** Over-lifetime size/colour multiply the start value (Unity). Velocity
  modules add. Limit Velocity runs after all forces. Collision runs after integration. The
  integrator is velocity-Verlet with per-tick constant acceleration, identical to today's
  (`ActionSystem.cpp:1008-1015`), so migrated motion matches.
- **R4. Fixed tick and interpolation.** The runtime advances in whole ticks of `1/tickRate`
  (≤ 4 per frame; excess time is dropped and counted). Rendering interpolates position,
  rotation and size between the last two ticks with `alpha = accumulator/tick`. `tickRate = 0`
  uses a bounded variable step (≤ 1/20 s) without interpolation storage. Simulation speed scale,
  pause and step are supported.
- **R5. Determinism.** For one build, a given (effect, seed, input transforms, tick sequence)
  gives bit-identical channel data **independent of worker count and chunking**. Randomness is
  `hash(seed, spawnIndex, moduleId, stream)` with no stored RNG state. Burst probability and
  emission fractional accumulators live per emitter. Cross-platform bit equality is not promised.
  Scalar and SIMD builds agree within tolerance (AC4).
- **R6. Force fields.** `ForceFieldComponent` (shape Sphere/Box/Cylinder/Infinite; falloff
  curve; type Directional (wind: direction, strength, turbulence, pulse frequency), Radial
  (attract/repel), Vortex (axis, strength), Turbulence (curl noise), Drag; layer bits). Per tick
  the world scheduler gathers active fields into a flat array with AABBs. Each emitter culls them
  against its bounds once per tick, so per-particle cost is paid only for fields that overlap it.
- **R7. Collisions.** Modes:
  - *Planes*: ≤ 8 per module, infinite planes from entity transforms or literals.
  - *World 3D*: Jolt ray cast from previous to current position, extended by radius × radius
    scale, using `PhysicsSystem::getNarrowPhaseQuery3D()` (`PhysicsSystem.h:177-180`) and a
    layer mask. It runs only while physics is not stepping.
  - *Terrain*: height + normal from `MeshSystem::sampleTerrainSurface` (`MeshSystem.h:231`),
    behind a thread-safe const read path.
  - *2D*: Box2D ray cast for 2D scenes.

  Response parameters: dampen, bounce, lifetime loss, min kill speed, max kill speed, radius
  scale, "kill on collide", collides-with mask. Each contact can raise a collision event
  (R10/R26).
- **R8. Collision quality (performance).** High: query every particle every tick. Medium
  (default): at most `maxQueriesPerTick` (default 256) world queries per emitter per tick,
  round-robin. Particles not queried test against their **cached contact plane** (point + normal
  from their last hit or probe), which is invalidated after `planeCacheTicks` (default 8). Low:
  the cached plane only, refreshed every 16 ticks.
- **R9. Trails & ribbons.** Mode *Per Particle* (each particle leaves a trail; ratio 0–1 picks
  which) or *Ribbon* (connects live particles in spawn order, ribbon count 1–8). Settings:
  lifetime, min vertex distance, max points per trail (≤ 64, default 16), world space, die with
  particle, width over trail (curve), colour over lifetime / over trail (gradient), inherit
  particle colour, size affects width/lifetime, texture mode Stretch / Tile / Distribute Per
  Segment, and "generate lighting data" (tangents for lit trails). Storage is a fixed SoA ring
  pool sized at bind.
- **R10. Sub-emitters & events.** Each entry is (trigger Birth / Death / Collision / Trigger
  (Lua/event), child effect, inherit flags Color/Size/Rotation/Lifetime/Velocity × factor,
  emit count, probability). There are ≤ 8 entries per effect and depth ≤ 3. Spawn requests are
  queued and consumed by the child on the **same tick** after the parent finishes, ordered by
  hierarchy depth then emitter id, so the result is deterministic. A child instance is pooled
  and owned by the parent instance. The child shares the world budget.
- **R11. Renderer module.** Render modes:
  - *Billboard*: faces the camera plane.
  - *Facing Camera Position*: the default for perspective, no wide-angle smear.
  - *Stretched Billboard*: camera scale, speed scale, length scale and freeze-in-view; the axis
    is the velocity seen on screen.
  - *Horizontal Billboard*, *Vertical Billboard* (locked to world or emitter Y), *World/Local
    Oriented*.
  - *Mesh*: ≤ 4 meshes, chosen per particle by random or index, with Align Z to Velocity.
  - *None*: trails only.

  Further settings: pivot, flip, material/shader (`library:particles` or a fork), blend (Alpha,
  Premultiplied, Additive, Multiply), sort mode (None, By Distance, Oldest/Youngest in Front,
  By Depth), sorting fudge/priority, min/max screen size (fraction of viewport, default max
  0.5), render layers, receive shadows (per-vertex main light) and lighting mode (R12).
- **R12. Lighting.** *Unlit*. *Lit*: per-vertex, from the main directional light, ambient/IBL
  irradiance and up to `MAX_LIGHTS` scene lights. *Six-Way*: two linear response maps in the
  three layouts of `particle_six_way.glsl`; main light per pixel, others per vertex;
  contrast, emission map/colour/intensity. Lit modes use the frame light block the PBR path
  already binds.
- **R13. Shader features kept.** `library:particles` (new shader type `particles`) keeps GLOW,
  EROSION, SOFT (scene depth, `@require scenedepth`) and NEAR_FADE with the same parameter names
  and semantics as `library/particle.frag`. It adds FRAME_BLEND, LIT, SIXWAY, STRETCH (vertex
  stage), CUSTOM_DATA and RECEIVE_SHADOWS. The `@shader points` `library:particle` stays for
  `PointsComponent` users.
- **R14. Playback.** Play on awake, play/pause/stop (emitting-only or clear), restart, emit(n)
  with optional overrides (position, velocity, size, colour, lifetime), seek(t), playback speed,
  `startDelay`, duration/looping. Prewarm simulates one full duration, spread over frames at
  ≤ 1 ms per frame, with the effect hidden until done, or synchronously at play when
  `prewarmSync`.
- **R15. Culling & LOD.** Emitter bounds are either computed (union of particle positions + max
  size, a per-chunk reduction) or authored (fixed AABB, no reduction cost). Per camera there is
  frustum culling on bounds and a distance cull. Culling mode: Automatic (looping → pause,
  catch up on view, max 0.5 s), Pause, Pause and Catch-up, Always Simulate. Distance LOD per
  emitter: emission scale and render thinning curves over camera distance (thinning drops
  particles by stable hash, so it never flickers). A **significance budget** caps live
  particles world-wide (defaults: desktop 262 144, mobile 65 536, web 131 072; project setting).
  When over budget, lower-priority / smaller-on-screen emitters get proportionally less emission
  (never killed). The per-emitter cap is 2^20.
- **R16. Pooling.** Channel storage, trail rings and child instances come from
  capacity-class pools (powers of two, S06 `ObjectPool`/`Arena`). Restart and replay reuse
  storage. Destroying an emitter returns its storage to the pool, trimmed after 10 s unused.

### Platform & backends

- **R17.** All features run on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan with only
  per-instance vertex attributes (divisor 1), a static corner/index buffer and `sg_append_buffer`
  streaming (`engine/renders/sokol/SokolBuffer.cpp:87`). There are no SSBOs, no compute, no
  `gl_PointSize`, and no `gl_VertexID`-only paths. Our `AttributeDataType`
  (`engine/core/render/Render.h:212`) gains `HALF` (sokol `SG_VERTEXFORMAT_HALF2/HALF4`, core in
  GLES3).
- **R18.** SIMD: SSE2 baseline on x86-64, NEON on arm64/armv7-neon, wasm `simd128` on web
  (`-msimd128`, default ON; `JASPERENGINE_WASM_SIMD=OFF` builds scalar). A scalar path is always
  compiled and selectable at runtime (`particles.forceScalar`) for tests. No AVX in v1.
- **R19.** Threads: S06 `jobs::parallelFor`. Single-thread web (`EMSCRIPTEN_THREAD_SUPPORT`
  defaults OFF, `engine/CMakeLists.txt:208`) runs the same jobs inline. All budgets in R21 for
  web assume one thread.

### Performance

Reference machines: **CI** = the S06 build-server reference machine (Linux x64, Release, 8
workers). **Mid desktop** = 6-core Zen 3 / Alder Lake class + GTX 1660 / RX 6600 class.
**Mid mobile** = Snapdragon 778G / Apple A14 class, 1080p, 4 workers. **Web** = Chrome stable on
mid desktop, wasm-simd, single thread. The **reference stack** is: Rate + Cone shape, Start
Lifetime/Speed/Size/Color ranges, gravity, Limit Velocity (drag), Noise High (1 octave), Color +
Size over Lifetime (curves), Texture Sheet 8×8 with frame blending, Billboard, additive (no sort).

- **R20. Memory.** The reference stack uses ≤ 64 B of channel storage per particle (fixed
  tick, interpolated). The instance record is **32 B** (R27). Trails take ≤ 24 B per point.
- **R21. Simulation budgets** (wall time per tick, all live particles, jobs on):

  | Target | Live particles | Reference stack | Without Noise |
  |---|---|---|---|
  | Mid desktop / CI | 100 000 | ≤ 1.0 ms (≤ 4.0 ms on 1 thread) | ≤ 0.5 ms (≤ 1.5 ms on 1 thread) |
  | Mid mobile | 10 000 | ≤ 0.8 ms | ≤ 0.4 ms |
  | Web (1 thread) | 30 000 | ≤ 3.0 ms | ≤ 1.5 ms |

- **R22. Render-prep budgets** (cull + interpolate + appearance + pack + upload, per frame):
  100 k ≤ 0.6 ms desktop. Back-to-front sort adds ≤ 0.5 ms per sorted view at 100 k (radix, 16-bit
  keys). Mobile: 10 k ≤ 0.3 ms. Web: 30 k ≤ 1.2 ms. A culled emitter costs ≤ 2 µs per frame.
- **R23. GPU.** 100 k reference particles, each ≤ 16 px wide, at 1080p: ≤ 1.5 ms on the mid
  desktop GPU, with one draw call per emitter per view (plus one for trails). Mobile 10 k
  (≤ 24 px) ≤ 2.0 ms.
- **R24. Scaling and SIMD.** Parallel speedup ≥ 0.6 × (workers+1) at 100 k on CI. SIMD speedup
  vs scalar ≥ 2.5× on the Noise kernel and ≥ 2× on the full reference tick (single thread, CI
  and web).
- **R25. Zero allocations.** In steady state (looping effects, no structural edits) a frame of
  simulate + render-prep performs **0 heap allocations** (counted by the bench binary's
  allocation hook, S06 AC style). Spawning sub-emitter children from pools also allocates
  nothing.
- **R26. Disabled cost.** A scene with no particle systems spends ≤ 5 µs per frame in the
  particle scheduler. A disabled module costs nothing (it is not compiled into the program).

### Editor UX (human)

- **R27. Inspector.** The Particle System component inspector shows the Unity module list:
  Main is always open, other modules are collapsible with an enable checkbox, and "+" adds
  hidden modules. Each `MinMaxCurve` field has a mode dropdown and inline curve preview. Click
  opens the shared S27 curve editor; gradients open the S27 gradient/colour picker. Multi-edit
  and undo come via S24 (one undo step per drag). Validation diagnostics (path + message) show
  inline. Editing an asset's effect edits the asset (all users update). Editing an inline effect
  edits only that entity.
- **R28. Preview overlay.** With an emitter selected, the Scene view shows a "Particle Effect"
  panel (Unity-style): Play/Pause, Restart, Stop, Playback Speed, **Playback Time** slider
  (scrub 0–60 s or 0–duration×4), particle count, sim/render ms, "Simulate Layers: selection
  only / all", Show Bounds, Show Colliders and Show Force Fields gizmos. Scrubbing uses seek
  (R29). Edits during preview keep live particles when the structure is unchanged
  (`HaveSameStructure`), and restart otherwise. Preview runs in Edit mode without entering Play.
  Selecting an `.particles` asset in the Project window previews it in the Inspector preview pane.
- **R29. Seek.** Seek(t) restarts and runs ticks to t, chunked at ≤ 4 ms per editor frame. The
  preview keeps **checkpoints** (full channel snapshots) every 1 s of simulated time, capped at
  32 MB per previewed emitter (oldest dropped), so backward and repeated scrubs start from the
  nearest checkpoint. The animation window's particle seek (`AnimationWindow.cpp:1116`) uses
  the same API.

### Agent UX (MCP tools, Lua API, CLI)

- **R30. MCP tools** (editor/mcp, documented in `docs/mcp/TOOLS.md` and the agent guide):
  - `particles_create_effect {path, preset?, effect?}`
  - `particles_get_effect {path|entity}` (full JSON + diagnostics)
  - `particles_edit_effect {path|entity, patch}` (JSON merge-patch on modules; undoable; returns
    diagnostics)
  - `particles_add_module` / `particles_remove_module {target, module}`
  - `particles_assign {entity, effect}`
  - `particles_control {entity, action: play|pause|stop|clear|restart|emit|seek|step, count?, time?, ticks?}`
  - `particles_stats {entity?}` (live, budget share, ticks, dropped time, sim/render µs,
    culled, sorted views)
  - `particles_capture {entity, time, camera?}` (renders a preview at a seeked time; returns an
    image resource)
  - `particles_migration_report {}`

  `create_particles` (WorldTools.cpp:4411) is reimplemented on top of these with the same
  presets and arguments, emitting the new component.
- **R31. Lua.** `ParticleSystem` (Unity naming, `docs/unity-glossary.md:52`) gets `play()`,
  `pause()`, `stop(clear?)`, `restart()`, `emit(count [, {position,velocity,size,color,lifetime}])`,
  `seek(t)`, properties `isPlaying`, `isEmitting`, `particleCount`, `time`, `playbackSpeed`,
  `seed`, `effect` (path/GUID), `setFloat/setVector3/setColor/getFloat(name)` for exposed
  parameters, and `sendTrigger(name)` (Trigger sub-emitters). Events:
  `onCollision(function(events))` delivers a batched array (position, normal, velocity,
  particleId, killed) on the main thread after the sim, capped by `maxCollisionEvents` (default
  64 per tick). `onParticleDeath` and `onSubEmit` work the same way. Listeners use S09 RAII
  subscriptions. The old setters remain as deprecated shims (D9).

### Data & compatibility

- **R32. Migration.** Scenes with the legacy component load and convert losslessly where the
  old semantics are expressible, which is all fields (mapping in D9). The scene version bump and
  the migration go through S03. Loading never writes files; the result is an inline effect. A
  one-line log per converted entity plus `particles_migration_report` lists what changed.
- **R33.** Exported projects (`editor/Factory.cpp`) emit the new component. Asset effects are
  packaged as data files and loaded by GUID/path. Inline effects are emitted as JSON string
  literals. The runtime loads JSON (`engine/libs/json`) and compiles it at bind (≤ 0.3 ms per
  effect on CI).

## 6. Design constraints & decisions

- **D1. Topology.** The new `ParticleSystemComponent` lives on an entity with a `Transform` and
  renders itself. There is no Points/InstancedMesh target and no `ActionComponent` requirement.
  For compatibility, `ActionSystem` treats an `ActionComponent` whose target carries a
  `ParticleSystemComponent` (and which has no other action kind) as a controller that forwards
  start/stop/pause. Existing timelines and `add_actions` keep working.
- **D2. Pipeline per frame** (S06 jobs, no frame latency):
  1. Main thread gathers emitter inputs (transform, budgets, culling decision from last frame's
     bounds, force fields).
  2. `parallelFor` over **work units** with chunks of 4096 particles. Small emitters are packed
     several per unit. Each emitter tick has these stages: emission/spawn (per emitter), update
     kernels (per chunk), collision (per chunk; physics query is read-only, scheduled after
     `PhysicsSystem` update), death marking + per-chunk bounds, then compaction (per emitter,
     swap-back, O(dead × channels)) and sub-emitter routing (serial, deterministic order).
  3. Per visible (emitter, view): interpolate + evaluate *pure* appearance modules at the
     interpolated age (colour/size/rotation/frame over lifetime are functions of age, random and
     start values, so they are not stored or ticked), then thinning, optional radix sort, and pack
     into a `FrameArena` staging block, in parallel.
  4. Render thread: one `sg_append_buffer` per (emitter, view) into a shared particle stream
     arena (capacity 2 × budget × 32 B, grown on demand outside steady state), then one instanced
     draw.

  Modules that depend on speed or are Set-mode are evaluated in the tick and stored.
- **D3. SoA layout.** `ParticleChannels` holds one `float*`/`uint32_t*` per component (px, py,
  pz, vx, …), 16 B aligned, capacity padded to 8. Core set: position ×3, previous position ×3,
  velocity ×3, age, inverse lifetime, spawnIndex, start size, start colour (RGBA8),
  rotation (+ previous), and size (+ previous) when size is ticked. Optional channels are
  added only by enabled modules: angular velocity, 3D rotation quaternion (mesh), size3D,
  custom ×4, cached contact plane ×4, trail head, collision flags. Kernels receive
  `ChunkView{begin, end, float* const*}` and never see indices.
- **D4. SIMD.** `engine/core/math/simd/SimdFloat4.h` provides a minimal `f4`/`i4`/`m4` set
  (load/store aligned, fma-or-mul-add, min/max, select, floor, rsqrt+Newton, cmp, int
  ops for hashing). Back ends are SSE2 / NEON / wasm simd128 / scalar. Kernels are templates on
  the back end. `sin/cos` use a shared polynomial in both paths. FMA contraction is disabled
  (`-ffp-contract=off` for `particles/`) so scalar and SIMD stay within tolerance. Jolt's SIMD
  types are not used, so particles do not depend on `JASPERENGINE_PHYSICS_3D`.
- **D5. Curves & gradients.** The runtime types are `Curve` (keys with in/out tangents, Unity
  semantics) and `Gradient` (≤ 8 colour + ≤ 8 alpha keys, Blend/Fixed), defined in
  `engine/core/math/Curve.h`. If S21/S27 lands a shared `Curve` first, use theirs. The compiler
  bakes each to a 64-entry float LUT (gradient: RGBA float LUT). Evaluation is clamp/wrap, index,
  lerp; that path is SIMD for arithmetic with scalar gathers, and the LUT stays in L1.
- **D6. Instance format (32 B).** `FLOAT3` position · `UBYTE4N` colour (sRGB-encoded, linearised
  in the VS) · `HALF2` size xy · `USHORT2N` (rotation/2π, frame/frameCount) · `HALF4` (velocity
  or stretch axis xyz, emissive intensity). Mesh mode adds a second stream with `SHORT4N`
  rotation quaternion (8 B, 40 B total). Trails: `FLOAT3` centre · `HALF4` tangent+half-width ·
  `UBYTE4N` colour · `USHORT2N` (u, side) = 28 B per vertex, expanded to camera-facing in the VS.
  Quad corners come from a static 4-vertex buffer + 6 indices.
- **D7. Sorting.** Sorting happens only when the blend is Alpha/Premultiplied *and* the sort mode
  is not None. Additive and Multiply never sort. The sort is an LSD radix sort on 16-bit
  quantised view depth (or age for Oldest/Youngest) over an index array, then a gather-pack. It
  runs per view that sees the emitter. Between emitters the existing transparent queue order
  (bounds centre + priority) applies.
- **D8. Collisions under threads.** Collision kernels write ray requests per chunk. A collision
  job batch-issues them against Jolt's `NarrowPhaseQuery` (thread-safe for reads outside the
  step; asserted with `isSteppingWorld3D()`) and applies the response. Planes and terrain run
  inline in the chunk.
- **D9. Migration mapping (legacy → modules).** `maxParticles` → `maxParticles`; `rate` → Rate
  over Time; `bursts` → Bursts; `loop` → looping; `duration` → `getParticleCycleDuration`
  (`ActionSystem.cpp:244`); `localSpace` → Local; `maxPerUpdate` → Emission `maxPerTick`;
  `lifeInitializer` → Start Lifetime range; position shapes → Shape (Box min/max → centre/size
  volume; Sphere/Hemisphere/Circle radius + `innerRadius` → radius thickness =
  1 − inner/radius; Cone angle/height); velocity/acceleration initializers → Start Velocity /
  Force over Lifetime (per-particle random vector); colour/alpha/size/rotation/scale/sprite
  initializers → Main start values / Texture Sheet start frame; each enabled modifier → a Set over
  Age entry with the ease sampled to a 16-key curve; `colorGradient` → Color over Lifetime
  (Set mode, sRGB flag kept). The target `PointsComponent`/`InstancedMeshComponent` is mapped to
  Renderer settings (texture, frame rects, blend, depth, custom shader + features: points
  `library:particle` → `library:particles` with the same feature/property values). The
  component is then removed if it has no authored points/instances and no other system targets
  it; otherwise it is kept and a warning is logged. The legacy struct survives as load-only
  `editor/migration/LegacyParticleSystem.h`. The deprecated Lua/C++ setters hold a legacy struct
  and re-run the converter, giving a structure-same rebind that keeps live particles.
- **D10. Module registry.** Each module is a descriptor: id, display name, stage, update order,
  channel mask, parameter field table (S08 reflection format), compile → kernel ops. The
  compiled program is a flat op list with its parameter blocks and LUTs in one allocation.
  Inspector, JSON codec, MCP patch validation and Lua exposed-parameter binding all read the
  field table.
- **D11. API sketch.**

```cpp
namespace jasperengine::particles {
struct EmitterInput { const Matrix4* world; uint32_t seed; uint32_t budget; float speed; bool emitting; };
class ParticleEffect;                       // document + compiled program, shared_ptr, hot-swappable
class EmitterRuntime {                      // owns channels, trails, children; no GPU objects
public:
  void bind(std::shared_ptr<const ParticleEffect>, uint32_t capacity);
  bool rebind(std::shared_ptr<const ParticleEffect>);   // false if structure differs
  void reset(uint32_t seed);
  void advance(const EmitterInput&, double dt);         // whole ticks + accumulator
  void step(const EmitterInput&);                        // one tick
  void beginSeek(const EmitterInput&, double t, SeekState&);
  bool continueSeek(const EmitterInput&, SeekState&, double budgetMs);
  void emit(uint32_t count, const EmitOverrides* = nullptr);
  void sendTrigger(uint32_t nameHash);
  uint32_t count() const; float interpolation() const;
  std::span<const CollisionEvent> collisionEvents() const;
};
class ParticleWorld;   // per Scene: emitters, force fields, budgets, scheduling, render extraction
}
```

  Lua: `local ps = entity:getComponent("ParticleSystem"); ps:play(); ps:setFloat("Intensity", 2);
  ps:onCollision(function(ev) for _,e in ipairs(ev) do ... end end)`.

- **D12. Where code lives.** Simulation in `engine/core/particles/`. `ParticleSubsystem`
  replaces the particle part of `ActionSystem` (lines 20–1300 move out). Rendering joins
  `RenderSystem` as `drawParticles`/`drawTrails`. Points/InstancedMesh code paths for particles
  are deleted after migration.

## 7. Acceptance criteria

Unit tests use S02 gtest in `engine/tests/core/particles/`. Benchmarks are the
`je-bench-particles` executable, gated by `tools/perf/check_bench.py` (S06), unless stated.

- **AC1 (R1, R33).** `ParticleEffectCodec.RoundTrip`: every module with non-default values
  survives JSON → compile → JSON byte-identically. `.Validation`: 30 invalid documents each
  produce the expected path diagnostic. Compile of the reference stack takes ≤ 0.3 ms on CI.
- **AC2 (R2, R3).** `ParticleModules.*`: one analytic test per module, a single particle with a
  fixed seed checked against closed-form values (gravity parabola, drag exponential decay, limit
  dampen, orbit radius stays within 1e-3, colour/size curves at ages 0/0.5/1, texture-sheet
  frame and blend weight, bursts with cycles/probability over 1000 seeds within ±3σ).
- **AC3 (R4, R5).** `ParticleDeterminism.WorkerCountIndependent`: the reference stack + collision
  + sub-emitters runs 600 ticks at workers {0,1,2,8} and chunk sizes {256, 4096}. A hash of all
  channels is identical. `.InterpolationContinuity`: at 30 Hz tick and 144 Hz frames, the rendered
  position of a constant-velocity particle deviates ≤ 1e-4 from the analytic line.
- **AC4 (R18, D4).** `ParticleSimd.MatchesScalar`: each kernel over 10 000 random particles
  matches scalar within 1e-5 relative. `CurlNoise.Golden` passes the copied OpenEngine golden
  values for both paths. Runs on x64 CI, arm64 (Android build server tests) and wasm (S02 web
  gate).
- **AC5 (R7, R8, D8).** `ParticleCollision.*`: plane bounce restitution exact to 1e-4.
  Kill-speed thresholds work. Terrain collision against a 33×33 test heightmap gives zero
  tunnelling at 30 Hz with speeds ≤ 50 m/s. A Jolt box floor (World mode, Medium quality,
  10 000 particles) gives ≤ 0.5 % tunnelling and ≤ 256 queries per tick. Collision events carry
  position/normal within 1e-3.
- **AC6 (R9).** `ParticleTrails.*`: spacing rule, ring wrap, die-with-particle vs fade, and ribbon
  ordering. Golden image `particles_trails.png` on GL and Metal/D3D11 (S02 golden harness).
- **AC7 (R10).** `ParticleSubEmitters.*`: Birth/Death/Collision/Trigger spawn the expected
  counts with inherited colour/size/velocity. Depth-3 chains are deterministic (AC3 hash). Pooled
  children allocate nothing after warm-up.
- **AC8 (R11–R13, R17).** Golden images (`particles_billboard_modes`, `particles_stretched`,
  `particles_mesh`, `particles_frame_blend`, `particles_sixway`, `particles_soft_nearfade`)
  match on GL 3.3, GLES3 (WebGL2 headless), Metal, D3D11 and Vulkan within the S02 tolerance.
  The soft/near-fade image equals today's `library:particle` output for the same parameters
  (migration parity scene).
- **AC9 (R14, R29).** `ParticleSeek.MatchesPlayback`: seek(7.3 s) equals playing to 7.3 s
  (channel hash). A backward scrub from 20 s to 19.5 s with checkpoints runs ≤ 0.5 s simulated
  time. Checkpoint memory stays ≤ 32 MB. Prewarm: the first visible frame of a prewarmed looping
  effect equals the state at `duration` (hash), and no frame spends > 1 ms prewarming.
- **AC10 (R15).** `ParticleBudget.*`: 100 emitters exceeding the world budget converge to
  ≤ budget live particles with the priority-ordered distribution. Thinning is stable (the same
  particles are hidden at the same distance across frames). Culling modes pause/catch up as
  specified. An off-screen Automatic emitter costs ≤ 2 µs per frame (bench).
- **AC11 (R20–R24, perf).** `je-bench-particles` scenarios: `ref_100k`, `ref_100k_nonoise`,
  `ref_100k_sorted`, `ref_10k`, `trails_10k`, `collision_world_10k`, `many_small_1000x100`.
  Each reports per-tick sim, render-prep, scalar vs SIMD and 1-vs-N-thread figures. CI gates
  R21/R22 desktop rows, R24 ratios and R20 bytes per particle. Web: the same bench built for
  wasm and run headless in the S02 browser gate gates the web rows. Mobile rows and R23 GPU
  numbers come from a manual sign-off on the named reference devices with the `particles_perf`
  sample scene, recorded in `docs/perf/particles.md` (Android GPU timings via S15 pass timers).
- **AC12 (R25, R26).** `je-bench-particles --count-allocs`: 0 allocations over 600 steady-state
  frames of `ref_100k` + `trails_10k` + sub-emitters. An empty-scene particle scheduler takes
  ≤ 5 µs.
- **AC13 (R32, D9).** `ParticleMigration.*`: before the old code is deleted, the legacy system
  records trajectories (positions/colours/sizes at 1/60 s steps) for 12 fixture scenes covering
  every initializer/modifier/shape/burst/gradient/sprite case, using min==max initializers. These
  are committed as CSV. The migrated effects reproduce them within 1e-3 world units / 1/255
  colour at tick rate 60. `tools/mcp/tests/test_particles_migration.py` opens the legacy sample
  projects, checks `particles_migration_report`, saves, reloads and finds no diffs. Old Lua
  scripts using the deprecated setters still run (`test_particles_lua.py`).
- **AC14 (R27, R28).** `tools/mcp/tests/test_particles_editor.py`: add/remove/enable modules and
  edit a curve key through the inspector command path. Undo/redo restores JSON exactly. The
  preview overlay play/scrub changes `particles_stats.time`. A param-only edit during preview
  keeps `particleCount` (no restart). Manual check: in the Scene view select the migrated
  "Fire" sample, scrub 0→5→2 s, and the image updates within 100 ms of each release.
- **AC15 (R30, R31).** `tools/mcp/tests/test_particles_tools.py` covers every tool, including
  error paths (invalid patch → diagnostics, unknown module). `particles_capture` at t=2 matches
  its golden. `test_particles_lua.py` covers play/stop/emit/seek, exposed parameters,
  `onCollision` batches (≤ cap, main thread) and `sendTrigger`.
- **AC16.** `tools/smoke-test.sh` stays green. The export smoke test builds a project with an
  asset effect and an inline (migrated) effect for Linux and HTML5, and both render in the
  headless player.

## 8. Dependencies & conflicts

- **Depends on**:
  - S01 (C++20: `std::span`, concepts for kernels)
  - S02 (gtest, golden images, web gate, bench harness)
  - S03 (scene version bump + migration hook)
  - S04 (effect GUIDs, dependency edges effect → textures/meshes/child effects)
  - S06 (`parallelFor`, `FrameArena`, `ObjectPool`, `RangeAllocator`, alloc-count harness)
  - S08 (field tables; until it lands, D10 tables are local and S08 adapts them)
  - S24 (undo)
  - S27 (curve + gradient widgets; the inspector shows read-only previews until it lands)
- **Soft**:
  - S05 (hot reload of `.particles` and textures)
  - S09 (event subscriptions for Lua callbacks)
  - S15 (pass timers, CPU profiler zones `particles.*`, MCP render stats include particle
    counters)
  - S21 (shared `Curve` type; animation-window particle seek)
  - S22 (wind: `ForceFieldComponent` Directional is the wind source the foliage/water work may
    read)
- **Merge hotspots**:
  - `engine/core/subsystem/ActionSystem.cpp` (S21 also edits it)
  - `engine/core/subsystem/RenderSystem.cpp` (S10–S13, S15)
  - `editor/Stream.cpp`, `editor/Factory.cpp`, `editor/window/InspectorWindow.cpp` (S08, S24)
  - `editor/mcp/tools/WorldTools.cpp`
  - `engine/core/render/Render.h` + `engine/renders/sokol/SokolObject.cpp` (HALF vertex format)
  - `engine/CMakeLists.txt` (`-msimd128`, `-ffp-contract=off`, new sources)
  - `NOTICE.md`

  Do the `ActionSystem` extraction (D12) as the first PR to shrink the conflict window.

## 9. Risks & defaults

- **Fill rate, not simulation, limits mobile.** Default: the max screen size 0.5 clamp, distance
  thinning on by default for looping effects, and the half-resolution compositing follow-up
  ticketed. The R23 mobile budget assumes small particles.
- **Moving the emitter onto the target entity (D1/D9) can surprise scripts** that call
  `ParticleSystem(scene, actionEntity)`. Default: the shim resolves "this entity, else its
  `ActionComponent` target" and logs a one-time deprecation note.
- **Half vertex formats** on old GLES3 drivers. They are core in ES 3.0. If a driver test fails,
  pack size into `USHORT2N` with a per-emitter max-size uniform (a flag, same 32 B).
- **Determinism vs SIMD/compiler.** Bit-identical results are only promised per build. Default:
  `-ffp-contract=off` and shared polynomials. Replays across platforms are out of scope.
- **Jolt queries from workers** may contend. Default: Medium quality (≤ 256 rays per emitter
  per tick) and batched queries per chunk. If profiling shows contention, the collision stage
  runs on one job per emitter.
- **Web single-thread** may miss 30 k with heavy stacks. Default: web projects get the 131 072
  world budget, and the editor shows a "Web: est. X ms" hint per effect from the bench
  per-module costs table.
- **S27 curve widget late.** Default: ship with read-only curve previews plus keyframe tables
  (time/value/tangents) in the inspector. MCP/Lua editing is unaffected.
- **Analysis report recommended extending the old model instead** (`03-world-building.md:231`).
  This is overridden by the user's explicit request for the module stack and high performance.
  The migration (D9) is what keeps that safe.

## 10. Deviations

What the implementation does differently from the text above, and why.

- **Up is +Y.** Shapes emit along the engine's up axis (+Y), not Unity's +Z; the Cone, Circle,
  Donut and Rectangle lie in the XZ plane. The Cone's Volume mode spawns uniformly over the
  frustum's volume.
- **The legacy struct lives in the engine** (`engine/core/particles/LegacyParticleSystem.h`, not
  `editor/migration/`): the deprecated Lua/C++ setters (D9) edit it at run time in exported
  games too.
- **ParticleSystem is a GameObject** (`engine/core/object/ParticleSystem.*`, moved from
  `action/`): the system is on its own GameObject (D1). `ParticleSystem(scene, actionEntity)`
  resolves the old action's target and warns once.
- **LUT sizes** are adaptive (65 entries doubling up to 1025 until linear interpolation is within
  tolerance) rather than a fixed 64 (D5), so steep eases keep their shape.
- **Speed-driven modules** (Color/Size/Rotation by Speed) are evaluated at render time from the
  stored velocity, like the age-driven ones, instead of being stored per tick (D2).
- **Sub-emitter children** spawn only what their parents ask for (their own Emission module is
  ignored); trails inherit the particle's start colour and size at birth.
- **Extra fields** the migration needs: Main `startSizeLinked` (one random pick for XYZ, the old
  scale initializer), Renderer `untexturedShape` (a billboard without a texture draws Unity's
  soft disc; migrated systems keep the old points' square), the Set over Age module (the old
  from-to modifiers, D9). A burst at exactly the cycle's end fires, as the old system did.
- **Noise qualities**: High is the three-potential curl noise (OpenEngine's field, golden-tested),
  Medium two potentials, Low a periodic grid baked at compile time and sampled trilinearly.
  On x86-64 processors with AVX2 the High/Medium field runs 8 samples at a time
  (`CurlNoiseAvx2.cpp`, picked at run time); SSE2 stays the baseline and the results are
  bit-identical.
- **Shader variants**: FRAME_BLEND, LIT, SIXWAY, RECEIVE_SHADOWS, CUSTOM_DATA, MESH and TRAIL are
  engine property bits of the `particles` shader type picked from the Renderer settings, not
  user-facing features of `library:particles` (which keeps GLOW, EROSION, SOFT, NEAR_FADE).
  Receive Shadows is one per-pixel tap of the main directional light's shadow. Mesh particles
  cull back faces (both sides with Flip).
- **Data**: an inline effect is stored in the scene YAML as its canonical JSON string
  (`inlineEffect`); effect and texture paths are assets-relative with their GUIDs. Scene format
  2 → 3 and prefab format 4 → 5 run the migration; an old system without a target gets a
  Transform at the origin.
- **Mesh source "GameObject"** is the GameObject's own MeshComponent; a migrated Mesh target keeps
  its MeshComponent disabled as that source.
- **Editor**: S27's curve widget has not landed, so curves and gradients show previews with key
  tables (the spec's fallback, §9). (Note, 2026-11-04, BF23 F26: S27's widget has landed since,
  and `InspectorParticles.cpp` draws curves with `CurveField` and gradients with `GradientField`.)
  The editor had no Inspector preview pane for assets: a
  `.particles` file picked in the Project window takes the Inspector (a live preview and its
  modules) until the GameObject selection changes. In Edit mode the particle subsystem is paused
  and the Scene view's Particle Effect panel simulates the selected systems (and the ones a tool
  controls).
- **MCP**: the tools are the `particles` group (S31) and follow S31's verb_noun rule (gated by
  test_tool_conventions): `create_particle_effect`, `get_particle_effect`,
  `edit_particle_effect`, `add_particle_module`, `remove_particle_module`,
  `set_particle_effect` (particles_assign), `control_particles`, `get_particle_stats`,
  `capture_particles`, `get_particle_migration_report`. `capture_particles` captures the Scene
  or Game view after the seek (`camera: scene | game`) rather than taking a camera GameObject. `create_particles` keeps its arguments; `emitter_entity` puts the system on that
  GameObject (a Mesh one emits its mesh).
- **AC16's HTML5 run** has no script of its own (as S04's): the smoke test's export of
  `tools/fixtures/particles-export` (an asset effect and a migrated inline one, its
  `ParticlesProbe.lua` logging what they simulate) is built with `emcmake cmake` in the web
  image and opened with `tools/web/browser_gate.py`; the Android run is the particles test
  project on the emulator (`android-expect.txt`).
- **Goldens**: the seven `particles_*` goldens are drawn on glcore and matched on Vulkan
  (lavapipe); GLES3/WebGL2, Metal and D3D11 have no headless device on the build servers. The
  soft/near-fade golden is the new renderer's: the old points were pixel-sized squares, so the
  "equals today's `library:particle` output" image cannot match pixel for pixel; the migration
  keeps the same shader features and parameters instead.
- **Benchmarks**: the benchmark server has 2 cores, so the 8-worker budgets of R21/R22 are
  checked as `single thread / (9 x 0.6)` with the parallel efficiency gated as measured
  (docs/perf/particles.md). The sorted-view budget is checked on one thread. The web rows run
  in the particles test project's page (`?particles_bench=1`) rather than a wasm build of
  je-bench-particles, and R24's web speedups compare the simd128 build with the scalar build
  (inside one build the scalar kernels are auto-vectorized for simd128). Mobile and GPU (R23)
  rows are manual sign-offs on the named devices. OpenEngine and Unity are compared at the
  design level only (neither is runnable on the servers).
- **Ribbons** with several strips use a fixed stride per strip (the effect's Max Particles over
  its ribbon count), so their triangles are made once at load; a single ribbon packs only its
  live particles.
- **File keys**: the Collision module's type is `collisionType` (a module entry's `type` names
  the module).
- **emit(n)** queues the particles: they are born at the system's next tick (deterministic, off
  the main thread's call); on a stopped system only they play. Collision, death and sub-emit
  events gather over a frame's ticks, the caps per frame.
- **Immediate calls** (seek, step, the editor's preview) work out a moved GameObject's matrices
  before they simulate, instead of using the last frame's.

## 11. Upstream absorption (sync #4)

origin/main grew its own Unity-shaped particle model (its "version 2" Particle System: modules on
the component, simulated by its ActionSystem into the GameObject's InstancedMesh quads or a Points
component) and a Unity ParticleSystem importer. S20 is the main version: it absorbs what was
unique there, reads every upstream file losslessly and drops the second model.

- **Fields taken over** (ParticleModules' table, so the codec, Inspector, MCP patches and Lua see
  them): `startDelay` as a MinMaxCurve (a random range picked per play), `useUnscaledTime`,
  `stopAction` (Disable deactivates, Destroy destroys at the frame's end, Callback sends
  `onParticleSystemStopped`), `emitterVelocityMode` (Rigidbody: the body's velocity for Inherit
  Velocity and Rate over Distance); Main `gravitySource` (Physics2D: Box2D's gravity in scene
  units) and `inheritVelocityMode` (Current: the emitter's change of velocity every tick); Shape
  `radiusMode`/`arcMode` (Random, Loop, Ping-Pong, Burst Spread) with `radiusSpread`/`arcSpread`
  and `radiusSpeed`/`arcSpeed`, `alignToDirection`; Velocity over Lifetime `offsetX/Y/Z` curves
  (an older file's `offset` vector reads as constants); Limit Velocity `separateAxes`, `limitY`,
  `limitZ`, `space`; Rotation by Speed `separateAxes` (3D, into the quaternion); Noise
  `scrollSpeed` (a curve over the emitter time) and four octaves; Texture Sheet `flipU`/`flipV`;
  burst counts as curves over the emitter time; the gradient mode Random Color; the Renderer's
  Velocity-aligned billboards, `meshAlignment` (Simulation, View, World, Local, Facing, Velocity;
  an older file's `alignToVelocity` reads as Velocity) and `textureTiling`. Stretched and Velocity-aligned mesh particles follow the total velocity (Velocity over Lifetime and Noise included, as origin/main's `totalVelocity`): such a program keeps the last tick's animated velocity in `ChAnimVelX/Y/Z`, 12 bytes per particle, no others pay. A teleport (an emitter
  moving more than 100 units in a tick) spawns nothing over distance, said once. Deactivating a
  GameObject stops and clears its system; Play On Awake plays it again on reactivation. Play starts every system as a load does (`ParticleSubsystem::resetForPlay`, before the scripts' init: the editor plays the edit-time scene in place, and the Scene view's preview had stopped or played some), and an empty system is never culled (nothing of it is drawn to be seen: its old bounds held a stopped and cleared one paused for good).
- **Its files** (`editor/stream/UpstreamParticles`, run by the scene 2 -> 3 / prefab 4 -> 5
  migration, which every origin/main scene (format 0) and prefab (format 3) goes through): a
  `particlesystem` with `version: 2` becomes an inline effect on its action's target (itself, as
  origin/main made them). Values convert: rotations and angular speeds radians -> degrees (the
  noise's rotation amount is radians per second in both), linear colours -> sRGB (hex was sRGB
  already), Two Constants and Two Curves keep (min, max) as (first, second), the shape's Euler
  X-then-Y-then-Z -> S20's Z-then-X-then-Y (the same turn), Cone Volume / Box Shell / Box Edge ->
  emit-from modes, billboard alignments -> render modes (World in a Local system is
  LocalOriented), origin/main's defaults where S20's differ (Texture Sheet 30 fps, drag by
  velocity on). The quad's material moves into the Renderer (`u_baseTexture` or the base colour
  map, `baseColor` -> `tint`, `tiling`, SOFT / NEAR_FADE and their parameters, the blend, depth
  write, shadows, the MeshComponent's enabled); the quad MeshComponent and the empty InstancedMesh
  go; Mesh mode keeps the MeshComponent, disabled, as the GameObject mesh source. A Points target
  gives its texture, frames (Sprites mode picks them by id), shader features and blend. Prefab
  instance overrides of version 2 fields convert their values and name `inlineEffect` (Play On
  Awake and the seed stay their own). Before this the migration read a version 2 system as an
  old one and dropped every value (`StreamMigrations.cpp`'s legacy branch).
- **The importer** (`editor/unity/`) writes S20 systems: inline effects with the renderer's
  material in the Renderer module (no `.material` files), Unity's sRGB colours as they are,
  degrees, the culling mode imported; MCP `import_unity_particles`, the CLI
  `import-unity-particles` and Assets > Import Unity Particle System... are back.
- **Lua**: Unity's module proxies on `ParticleSystem` (`ps.main.startSpeed`, `ps.emission`,
  `ps.shape`, ... with Unity's names and radians), `clear/play/stop/pause(withChildren)`,
  `ParticleSystemStopBehavior` and the enum tables; `GameObject:getParticleSystemsInChildren()`;
  origin/main's constructors `ParticleSystem(scene, true)`, `MinMaxCurve(min, max)`,
  `MinMaxCurve(multiplier, curve[, maxCurve])`, `MinMaxGradient(min, max)` and
  `MinMaxGradient(gradient[, maxGradient])` (its benchmark script, `tools/fixtures/particles/bench`, runs).
- **UI particles** (origin/main's ui-scaling-text): the component's own `useUnscaledTime`
  (OmitIfDefault: written only when on; Inspector "Unscaled Time", Lua `ps.useUnscaledTime`, the
  exported code) ticks a system on the unscaled frame time, as the effect's Unscaled Time does
  (either one: a prefab with a shared asset plays unscaled without a copy); an old system's
  `useUnscaledTime` key migrates onto it. A Particle System under a RectTransform draws in place,
  between its siblings, inside the nearest panel's scissor (as UI Points do). GameObject > UI >
  Particle System is one GameObject: origin/main's sparkle through the old system's converter, the
  soft image `ui/particle_soft.png`, unscaled time; `create_particles` under a UI element or in a
  UI scene turns the Transform 180 degrees about X (y down), Local space, unscaled time, the soft
  image, and replies `ui: true`. The HTML `<particle-system>` finds the first Particle System of its
  prefab (an older prefab's controller action is left idle) and draws the render extraction's
  billboards (`UIParticleSource`: positions, sizes, rotations, colours, sheet frames, the soft disc
  of an untextured system); `time="ui"` is the component's `useUnscaledTime` (`game` also turns the
  effect's off), `loop=` the effect's Looping (the GameObject's own copy), `clear()` Unity's Clear,
  `isPlaying()` S20's, `complete` when a system that does not loop stops. Not kept: a Canvas
  Group's alpha does not fade a Particle System (it fades Points; S20's shader has no group alpha).
  Tests: `engine/tests/htmlui/TestParticleSystemElement.cpp` (origin/main's, on S20 systems, with
  its golden; time, loop, clear, pause, complete, an old prefab layout) and
  `TestGoldenUIScaling.cpp`'s `UIParticleSystemsClippedByAPanel`.
- **Dropped**: origin/main's ParticleSystemComponent version 2 fields and their simulation
  (`particles/MinMax`, `ParticleFields`, `ParticleNoise`, `ParticleShapes`,
  `ParticleSimulation`, `ParticleUpgrade`, `ActionSystemParticles.cpp`), its overlay and shape
  gizmo (S20's Particle Effect overlay and `ParticleCompiler::shapeOutline` draw them), its
  `create_particles` version 2 arguments (`create_particles` keeps S20's, plus `duration`,
  `speed` and a `modules` merge patch).
- **Tests**: origin/main's ParticleEmission/Shape/Simulation/MinMax tests are
  `engine/tests/core/particles/ParticleUpstreamBehaviourTest.cpp` and
  `ParticleUpstreamModulesTest.cpp` (with the absorbed fields'), its stream and upgrade tests
  `editor/tests/stream/UpstreamParticlesTest.cpp`, its legacy identity test
  `editor/tests/stream/LegacyParticlePresetsTest.cpp` (S20's model draws from its own random
  streams: counts and mean colours are compared, not single particles), its importer tests
  `editor/tests/unity/`, and a scene and prefab saved by origin/main's editor
  (`editor/tests/fixtures/migrations/upstream-v2/`) are `UpstreamParticleFilesTest.cpp`.
