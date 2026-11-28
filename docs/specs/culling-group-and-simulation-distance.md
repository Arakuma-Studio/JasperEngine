# Culling Groups and Simulation Culling: distance bands and visibility for scripts, and a no-code rule that freezes far or unloaded GameObjects

Spec-first. Builds on S07 (system scheduler), S15 (monitors), S21 (Animator culling), S56 (Device Tiers) and
T4 (Terrain Worlds). Paths are relative to the repository root unless they start with
`craftsman-end-motor/`, which means `/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor/`.

## 1. Goal

Large or busy scenes need two things the engine does not give today. Scripts need to know cheaply which of
many points are on screen and how far they are from the player, without testing each one in Lua every
frame: Unity's **CullingGroup** answers that with bounding spheres, distance bands and state-change
callbacks. And a game needs a rule, without code, that stops far GameObjects from simulating and never lets
one simulate over ground that is not loaded, where it would fall out of the world: **Simulation Culling**,
a component that skips the update callbacks of its scripts, stops its Animator and freezes its bodies while
it is beyond a Simulation Distance from every focus or over unloaded ground, optionally updating a middle
band at a reduced rate. Both run on the CPU, cost in proportion to what uses them, cost nothing in a scene
without them, and work on every target including single-threaded web.

## 2. Scope

**In scope**
- `CullingGroup` (C++ and Lua, Unity's API): bounding spheres (fixed or following GameObjects), distance
  bands from a reference point, visibility against one camera or any enabled camera, `onStateChanged`,
  `isVisible`, `getDistance`, `queryIndices`, `eraseSwapBack`, ownership by the creating script.
- **Simulation Culling** component (the explicit scope decision for "simulation distance"): a per-GameObject
  rule with Full, Reduced and Culled states, applied to its GameObject and descendants, acting on exactly
  three things: the scripts' update callbacks (`onUpdate`, `onPostUpdate`, `onFixedUpdate`; C++ and Lua),
  the Animator, and Rigidbody / Rigidbody 2D bodies. Script messages `onSimulationCulled(reason)` and
  `onSimulationResumed()` so a script can stop anything else (a NavMesh Agent, a sound).
- Simulation focuses (the main camera by default, up to 16 more from scripts) and the ground rule: Terrain
  World tiles (T4) and a script-provided check.
- A Device Tier cell **Simulation Distance** (multiplier) so weak devices simulate a smaller radius.
- Stats panel lines, runtime debug overlay fields, monitors, `get_render_stats` `culling`,
  `configure_game_view` `culling_debug`, Lua API and stubs, docs, eval cases.

**Out of scope (default decisions)**
- Occlusion: JasperEngine has no occlusion culling (no occlusion or Hi-Z code in `engine/core`, inferred
  from a search), so visibility is the camera frustum only. Unity's CullingGroup also uses occlusion data;
  documented difference.
- GPU culling of any kind (README do-not-copy list: GPU-driven culling). This is a CPU feature.
- Culling NavMesh Agents, Particle Systems, Audio Sources, Character Controllers, coroutines and timers:
  particles and animators already have culling modes (`ParticleEffect.h:42`, `AnimatorTypes.h:27-32`);
  agents and sounds are stopped by scripts in `onSimulationCulled`; a Character Controller moves only when a
  script calls it, so culled scripts stop it; coroutines and `Timer` callbacks keep running (documented: they
  are time-based and pausing them changes their meaning).
- Network relevancy, server tick budgets, level-of-detail of scripts beyond one reduced band.
- Saving the culled state: it is runtime-only; a frozen body is saved (by whoever saves) with its real
  velocity (D9).
- Evaluation in edit mode: groups and components evaluate only while the scene runs (Play, players).

## 3. Current state

All read from the code (not run) unless marked inferred:

- **No CullingGroup, no simulation distance.** A search of `engine/`, `editor/` and `docs/specs/` finds no
  `CullingGroup`, `SimulationDistance` or similar; the analyst note is right.
- **Animator culling.** `AnimatorCullingMode {AlwaysAnimate, CullUpdateTransforms, CullCompletely}`
  (`AnimatorTypes.h:27-32`); `AnimatorSystem::visible` tests each skinned mesh's world AABB against every
  enabled camera (`AnimatorSystem.cpp:325-339`); culled animators skip sampling or everything
  (`:424-431`) and are counted in `stats.culled`, reported as the "Animators culled" monitor
  (`EngineProfiling.cpp:205`).
- **Particle culling.** `CullingMode {Automatic, Pause, PauseAndCatchUp, AlwaysSimulate}`
  (`ParticleEffect.h:42`), applied by `particles::cullingStep` (`ParticleWorld.h:34`,
  `ParticleSubsystem.cpp:1108`).
- **Frustum tests exist.** `RenderSystem::isInsideCamera` has AABB, point and sphere forms
  (`RenderSystem.cpp:16132-16181`); the sphere form is six plane distances. Each camera's
  `frustumPlanes[6]` (`CameraComponent.h:76`) is rebuilt in `RenderSystem::updateCamera`
  (`RenderSystem.cpp:15156`, `:15217`), which `RenderSystem::update` (`:17511`) calls (`:17769`).
- **System order.** `Scene::init` registers every subsystem in a fixed order (`Scene.cpp:66-116`):
  Animator early, `RenderSystem` at `:102` ("after Render, whose update makes this frame's world
  transforms", `:103-104`), then Trail, Water Interaction, Navigation, Tilemap, Physics, Particles, Audio.
  Phases are FixedUpdate, Update, PostUpdate, Draw (`SystemScheduler.h:24`). Every registered subsystem runs
  in every scene (`Scene.h:485-509`).
- **Scripts' update callbacks.** Lua scripts subscribe with `RegisterEngineEvent(self, "onUpdate")`
  (`docs/mcp/guide/lua-scripting.md:27-30`) to `Engine::onUpdate`, `onFixedUpdate`, `onPostUpdate`
  (`Engine.h:1120-1122`). Every scene script's engine-event call goes through one guard,
  `isScriptActive(scene, entity, instance)`: the Lua closure built in `luaRegisterEventImpl`
  (`LuaBinding.cpp:562-611`) and the C++ `REGISTER_ENGINE_EVENT` wrapper (`FunctionSubscribe.h:386-417`).
  The guard does not know which event it guards (it gates input events too).
- **Per-script delta time.** Lua `Engine.deltatime` is `Engine::getScriptDeltatime`
  (`CoreClassesLua.cpp:2404`, `Engine.cpp:1191-1193`), which asks `HitStop::getScriptDeltaTime` for the
  script whose callback runs (`HitStop.h:93-104`; the Lua guard opens `HitStop::ScriptScope`,
  `LuaBinding.cpp:626`). So a per-script delta is already plumbed.
- **Script messages.** `ScriptMessage` has 39 values and must stay under 64 (`ScriptMessages.h:31-46`).
- **Bodies can switch motion type.** Jolt bodies are created with `mAllowDynamicOrKinematic`
  (`PhysicsSystem.cpp:2861`); `SetMotionType` is already used for body type changes (`:2737`). Box2D bodies
  change type with `b2Body_SetType` (`Rigidbody2D.cpp:974`).
- **Terrain Worlds (T4).** `TerrainWorld::isAreaReady(x, z, radius, level)` with levels Render and
  Collision (`TerrainWorld.h:67`, `TerrainWorldRuntime.h:76`). Its implementation builds a vector of missing
  tiles on every call (`TerrainWorldRuntime.cpp:665-691`); absent tiles (the designer's world edge) are
  skipped (`:673`). Collision readiness needs fine collision on the tile (`:680-681`), which T4 builds only
  round focuses and awake dynamic bodies ("subjects", T4 R21); past 64 transient subjects T4 puts the
  farthest bodies to sleep (`frozen_bodies`, T4 R21). T4 R22 gives a dynamic body over a loading tile a
  coarse collider; script-moved objects get nothing, and `getSurfaceAt` answers from the overview with
  `exact = false` (`TerrainWorld.h:60-62`).
- **Device Tiers (S56).** `DeviceOverride` has 14 cells, stored as ints, append-only
  (`DeviceSettings.h:15-36`); the LOD biases are the Multiplier kind; `DeviceSettings` is carried to players
  in game.json (`PlayerConfig.h:115-117`).
- **Stats and overlays.** The Game view Stats panel prints rendering lines (`GameWindow.cpp:1533-1620`);
  the runtime debug overlay's `DebugStatsSnapshot` (`DebugStats.h:19-41`, stub
  `stubs/DebugOverlay.lua`); monitors are registered in `EngineProfiling.cpp:158-207`. `get_frame_times`
  lists monitors; `get_render_stats` reports a view's last frame (`docs/mcp/TOOLS.md`). There is no
  `get_frame_stats` tool (the analyst note names one; the equivalent is `get_render_stats`).

## 4. Reference

| Source | Take | Note |
|---|---|---|
| Unity Scripting API (docs only): `CullingGroup` (`targetCamera`, `SetBoundingSpheres`, `SetBoundingSphereCount`, `SetBoundingDistances`, `SetDistanceReferencePoint`, `onStateChanged`, `IsVisible`, `GetDistance`, `QueryIndices`, `EraseSwapBack`, `enabled`, `Dispose`), `CullingGroupEvent`, `BoundingSphere`, `AnimatorCullingMode` | Names and semantics: bands as indices into ascending distances (band 0 nearest, the last band beyond the last distance); events only on change | Names only. Lua: camelCase, sphere indices 1-based (D3) |
| Unreal Engine docs (concept only): Significance Manager, actor tick intervals | A reduced-rate band where update callbacks run every N frames with the accumulated delta | Concept; nothing taken |
| `craftsman-end-motor/src/entity_residency_test.cpp` (header, sections 1-6), `craftsman-end-motor/src/level.cpp` `entityChunkIsTicking` | The defect: entities kept simulating over released world data, which answered "empty", so they fell out of the world and were destroyed, wiping a server's population whenever it emptied; the rule that fixed it: an entity simulates only while the ground under it is resident and it is inside a focus's disc, freezes in place otherwise, and resumes the next update once both hold | **Ideas only.** No code, data, assets or Minecraft-specific mechanics (chunks, ticking tables, mob types) may be copied; here the rule is a generic component with a pluggable ground check |
| `craftsman-end-motor/include/mcpe015/perf2.h` (profile for weak GPUs) | Weak devices update moving actors only within a smaller radius while players always update | Here: a Device Tier multiplier on Simulation Distance, and focuses that always simulate their own GameObject (R18) |

## 5. Requirements

### Functional: CullingGroup

- **R1 Lifetime.** `CullingGroup::create(scene)` returns a handle; `dispose()` ends it (handle invalid,
  `isValid()` false, later calls log one warning and do nothing). Created inside a Lua script's callback, the
  group belongs to that script instance and is disposed when the instance is released (destroy, scene
  unload, reload); otherwise it belongs to the scene and dies with it. At most 256 groups per scene: the 257th
  `create` fails with an error naming the limit.
- **R2 Spheres.** `setBoundingSpheres(span)` copies the spheres; `setBoundingSphereCount(n)` uses the first
  `n` (shrinking forgets the rest's state; growing adds spheres at the origin with radius 0 until set);
  `setSphere(i, sphere)`; `setSphereTarget(i, entity, radius, offset)` makes sphere `i` follow the entity's
  world position plus `offset` at each evaluation (a destroyed entity's sphere stops following and keeps its
  last position, one warning per group); `eraseSwapBack(i)` moves the last sphere and its state into `i`
  (Unity). At most 100,000 spheres per group; radius must be finite and >= 0.
- **R3 Distances.** `setBoundingDistances(span)`: 0 to 8 ascending, finite, positive values; band `b` holds
  distances in `[d[b-1], d[b])`, band 0 below `d[0]`, band `n` at or beyond `d[n-1]`; no distances: every
  sphere is in band 0. The distance of a sphere is `max(0, |center - reference| - radius)`.
  `setDistanceReferencePoint(Vector3)` or `(entity)` (followed each evaluation); none set: the target camera's
  position, else the scene's main camera's (`Scene::getCamera`, `Scene.h:204`), else the origin.
  `bandHysteresis` (metres, default 0, Unity's behaviour): a sphere moves to a farther band only when its
  distance exceeds the band edge by more than it.
- **R4 Visibility.** `targetCamera` (a Camera GameObject, default none). None: visible when inside the
  frustum of any enabled camera of the scene (the Animator's rule, `AnimatorSystem.cpp:325-339`). Set:
  that camera only; disabled or destroyed → every sphere invisible. Test: the sphere is not wholly behind
  one of the six planes (far plane skipped when the camera's far clip is 0, as `isInsideCamera`). Frustum
  only (§2).
- **R5 Evaluation.** Once per frame while the scene runs, after this frame's world transforms and camera
  planes (D1), for every enabled group: refresh followed positions, compute bands and visibility, collect
  changes. `enabled = false` stops evaluation and events; states freeze until re-enabled (the next
  evaluation reports what changed meanwhile).
- **R6 Events.** `onStateChanged` is called once per sphere whose visibility or band changed, after the
  whole group was evaluated, in ascending index order, with `(index, isVisible, wasVisible, band,
  previousBand)`; C++ gets `CullingGroupEvent` with the same fields plus `hasBecomeVisible()` and
  `hasBecomeInvisible()`. Spheres start invisible with band -1 (unknown): the first evaluation reports every
  sphere (documented, so a script needs no separate initial query).
- **R7 Queries.** `isVisible(i)`, `getDistance(i)` (the band; -1 before the first evaluation),
  `queryIndices(visible, out, first)`, `queryIndices(band, out, first)`, `queryIndices(visible, band, out,
  first)` (Unity's three forms; `first` skips that many matches) return the count written into `out`.
- **R8 Re-entry.** Inside `onStateChanged` a script may change spheres, distances, the camera or `enabled`
  (applied at the next evaluation) and may dispose the group (the remaining events of that evaluation are
  dropped). Creating a group inside a callback is allowed; it evaluates from the next frame.
- **R9 Determinism.** For the same inputs (positions, cameras, settings) the states and the event sequence
  are identical across runs on one platform; parallel evaluation (D2) gives the same results as serial.

### Functional: Simulation Culling

- **R10 Component.** `SimulationCullingComponent` (Inspector "Simulation Culling"), fields and defaults:
  `enabled` (true), `simulationDistance` (64 m), `reducedRateDistance` (0 = no reduced band),
  `reducedUpdateInterval` (4 frames, 2..60), `hysteresis` (2 m), `boundsRadius` (0 = automatic: the radius of
  the GameObject's rendered or collider bounds when it starts, else 0.5 m), `simulateWhenVisible` (true),
  `freezeWhenGroundNotLoaded` (true), `cullScripts` (true), `cullAnimator` (true), `cullPhysics` (true).
  Runtime read-only: `state` (Full, Reduced, Culled), `reason` (none, distance, ground_not_loaded),
  `focusDistance` (metres to the nearest focus, from the sphere's surface).
- **R11 Focuses.** The simulation focuses of a scene are the main camera, plus up to 16 added by
  `SimulationCulling.addFocus(scene, position | gameObject)` (returns an id > 0; `setFocus`, `removeFocus`).
  When scripts added at least one, the main camera is a focus only if `SimulationCulling.cameraIsFocus`
  is true (default true). A focus never stops its own GameObject's simulation (a player's GameObject that is
  a focus is always Full).
- **R12 States.** With `d` = distance to the nearest focus (bounds sphere surface, scaled distances per R16):
  - `d <= reducedRateDistance` (or `reducedRateDistance` 0 and `d <= simulationDistance`) → Full;
  - `reducedRateDistance < d <= simulationDistance` → Reduced;
  - `d > simulationDistance` → Culled, reason distance; with `simulateWhenVisible` and the sphere visible in
    any enabled camera → Reduced instead (Full when there is no reduced band);
  - moving outward needs `d` past the edge by more than `hysteresis`;
  - the ground rule (R13) overrides all: ground not loaded → Culled, reason ground_not_loaded.
  A disabled component, or one on an inactive GameObject, is Full and acts on nothing.
- **R13 Ground rule.** With `freezeWhenGroundNotLoaded`, the GameObject is over unloaded ground when a Terrain
  World of the scene has a present tile within `boundsRadius + 1 m` of its XZ position that is not loaded
  (CpuReady, Uploading or Resident: D11), or the scene's script ground check (D12) returns false for it.
  Absent tiles (no tile there) never count as unloaded (T4's world edge, `TerrainWorldRuntime.cpp:673`).
  Checked at the first evaluation, then every 8 frames per component (round robin), and at once when the
  GameObject moved more than one tile size since its last check.
- **R14 What Culled does** to the component's GameObject and every descendant whose nearest
  Simulation Culling ancestor-or-self is this component:
  - scripts (`cullScripts`): their `onUpdate`, `onPostUpdate` and `onFixedUpdate` engine callbacks, Lua and
    C++, are skipped; every other engine event (input, focus, pause), component events, physics callbacks,
    coroutines and timers are unchanged;
  - Animator (`cullAnimator`): treated as `CullCompletely` (no time, states, events or root motion advance);
  - bodies (`cullPhysics`): each dynamic Rigidbody and Rigidbody 2D is frozen (D9): kinematic, zero velocity,
    in the broad phase (others collide with it and queries find it); kinematic and static bodies are
    untouched; resuming restores the motion type and the saved velocities and wakes the body.
- **R15 What Reduced does.** Scripts' `onUpdate` and `onPostUpdate` run on one frame in
  `reducedUpdateInterval`, on frames where `(frameCount + entity) % interval == 0` (spreading the load);
  inside them `Engine.deltatime` (and C++ `Engine::getScriptDeltatime`) is the scaled time since that
  GameObject's previous reduced call (hit stop still applies on top); `onFixedUpdate`, the Animator and bodies
  are unchanged. Entering Full or Reduced resets the accumulator.
- **R16 Device Tier.** `DeviceOverride::SimulationDistance` (cell 14, Multiplier 0.25..1, key
  `simulation_distance`, label "Simulation Distance") multiplies `simulationDistance` and
  `reducedRateDistance` of every component. Defaults: Low 0.6, Medium 0.8, High and Ultra inherit (1). It is
  not thermal-adjusted. `SimulationCulling.distanceScale` (Lua, runtime, default 1) multiplies on top.
- **R17 Messages.** After a GameObject's state is applied, its scripts (on the component's GameObject only,
  enabled or not, active in hierarchy) receive `onSimulationCulled(reason)` on Full/Reduced → Culled and
  `onSimulationResumed()` on Culled → Full/Reduced, in entity order (C++ `ScriptBase` overrides too). Reduced ↔
  Full sends nothing.
- **R18 Start and transitions.** The first evaluation of a scene runs when it starts, after bodies are
  materialised (`PhysicsSystem::load`, `PhysicsSystem.cpp:4125`) and before its first fixed step, so nothing
  over unloaded ground simulates for a frame. Later, at most 64 GameObjects change state per frame, nearest
  focus distance first for resumes, farthest first for distance culls; transitions with reason
  ground_not_loaded are never deferred. A GameObject instantiated into a running scene is evaluated in the
  frame it is created (before its first update callback).
- **R19 Zero cost.** With no CullingGroup and no Simulation Culling component in a scene, its culling system
  returns after one check, the script guard adds one relaxed atomic load per callback, the Animator and
  physics add nothing, and nothing is allocated.

### Platform & backends

- **R20** CPU only; every export target; single-threaded web: evaluation runs inline (S06's `parallelFor`
  falls back to the calling thread). Native builds evaluate groups of more than 4,096 spheres with
  `parallelFor` in chunks of 1,024.
- **R21** Physics freezing exists under `JASPERENGINE_PHYSICS_3D` / `_2D`; a build without a backend skips
  that part. The Terrain World ground check exists with `JASPERENGINE_TERRAIN_WORLD`.

### Performance (release; `CullingBench`; desktop / mobile / web as S19 §5; main thread)

- **R22 Budgets.**

  | Work | Budget (desktop / mobile / web) |
  |---|---|
  | One group, 10,000 fixed spheres, 4 distances, one camera, no change | 0.12 / 0.4 / 0.5 ms |
  | The same with every sphere following a GameObject | 0.2 / 0.65 / 0.8 ms |
  | 1,000 Simulation Culling components, 2 focuses, no state change | 0.06 / 0.2 / 0.25 ms |
  | 64 state changes (scripts, Animator, bodies) | 0.15 / 0.5 / 0.6 ms |
  | Ground rule, 1,000 components over a Terrain World (round robin) | 0.02 / 0.06 / 0.08 ms |
  | No group, no component (system + guards, 1,000 script callbacks) | 0.002 / 0.006 / 0.008 ms |
  | Script guard, per callback, with components present | <= 5 ns desktop |

- **R23 Allocations.** After the first evaluation, evaluations and event dispatch allocate nothing on the
  C++ heap (event buffer, follow list and state arrays are kept); a Lua `onStateChanged` call passes five
  numbers and booleans (no table per event); `queryIndices` into a table with room leaves the Lua GC count
  unchanged.
- **R24 Memory.** 20 bytes per sphere, plus 16 bytes per followed sphere; 1 byte per entity id for the
  Simulation Culling flags (only while a component exists) and 32 bytes per component of runtime state.

### Editor UX (human)

- **R25 Inspector.** Add Component > Miscellaneous > **Simulation Culling**: rows Simulation Distance,
  Reduced Rate Distance, Reduced Update Interval (shown when Reduced Rate Distance > 0), Hysteresis, Bounds
  Radius ("0 = automatic"), Simulate When Visible, Freeze When Ground Not Loaded, a **Cull** foldout (Scripts,
  Animator, Physics). In Play, read-only State, Reason and Focus Distance. Every edit is a `PropertyCmd` (undo,
  multi-edit) as every reflected row.
- **R26 Project Settings > Quality > Device Tiers** gets the **Simulation Distance** column (S56's table,
  same editing and reset rules as LOD Bias).
- **R27 Game view.** The toolbar's Gizmos menu gets **Culling**: while playing, draws Simulation Culling
  spheres (green Full, yellow Reduced, red Culled by distance, grey Culled by ground) and Culling Group
  spheres (cyan visible, dark blue invisible), and the focuses as crosses, in the Game and Scene views.
  Saved with the Game view toggles.
- **R28 Stats.** The Game view Stats panel adds, when any exists: `Culling groups: G (S spheres, V visible)` and
  `Simulation: F full, R reduced, C culled (N ground)  Script updates skipped: K`. The runtime debug overlay's
  `DebugStatsSnapshot` gains `cullingGroups`, `cullingSpheres`, `cullingVisible`, `simulationFull`,
  `simulationReduced`, `simulationCulled`, `scriptUpdatesSkipped`.

### Agent UX (MCP, Lua, CLI)

- **R29 `get_render_stats`** gains `culling`: `{groups, spheres, visible, per_band: [counts], evaluate_ms,
  simulation: {components, full, reduced, culled, ground_not_loaded, focuses, script_updates_skipped,
  transitions_deferred}}` for the running scenes (omitted when nothing exists). Description +~40 characters.
- **R30 `configure_game_view`** gains `culling_debug` (boolean): R27's drawing; a screenshot shows it.
- **R31 Live state** through the generic tools: `get_runtime_entity` lists the component with `state`,
  `reason`, `focusDistance`; `set_runtime_property` edits any field in Play; `add_component` /
  `set_properties` author it (reflection, S08). No new tool.
- **R32 Monitors** (`get_frame_times`): `Culling spheres`, `Culling visible`, `Culling ms`,
  `Simulation culled`, `Script updates skipped`.
- **R33 Lua.** `CullingGroup(scene)` with the members of §6, `BoundingSphere(center, radius)`,
  `SimulationCulling(scene, entity)` handle (fields of R10, `state`/`reason` as strings `"full"`,
  `"reduced"`, `"culled"` / `"none"`, `"distance"`, `"ground_not_loaded"`), statics
  `SimulationCulling.addFocus/setFocus/removeFocus/cameraIsFocus/distanceScale/setGroundCheck`, script
  messages `onSimulationCulled(reason)` / `onSimulationResumed()`.
- **R34 Docs.** New `docs/scripting/culling-groups.md` (CullingGroup recipes: wake enemies on screen, LOD of
  AI by band; Simulation Culling: setup, focuses, ground rule, reduced band and deltatime, what is not culled,
  multiplayer focuses, debugging); `docs/mcp/guide/profiling.md` section "Culling" (read `get_render_stats`
  culling, turn on `culling_debug`, recipe: check far enemies freeze over an unloaded tile);
  `docs/mcp/guide/lua-scripting.md` one line naming the two messages; `docs/mcp/guide/lua-api.md` rows;
  `docs/device-tiers.md` the new cell; `docs/unity-glossary.md` rows; hand-written stub fragment
  `engine/core/script/binding/stubs/CullingGroup.lua` (the `onStateChanged` callback type, the
  `setGroundCheck` function type, `queryIndices` table contract); `stubs/DebugOverlay.lua` gains R28's fields;
  `TOOLS.md` regenerated.
- **R35 Eval.** Cases in `tools/mcp/eval/cases.py`: `guard_culling_stats` ("are far enemies still
  simulating?": must read `get_render_stats` culling or `get_runtime_entity`, not guess from
  screenshots), `guard_culling_debug` ("show me what is culled": `configure_game_view {culling_debug}` then
  `screenshot`). Each fails on a tool text without the phrase it relies on.

### Data & compatibility

- **R36 New component, no bump.** `SimulationCullingComponent` is a new optional component: a reflection
  table in `editor/reflect/components/GameplayReflect.cpp` with its stream codec (the
  `JE_STREAM_CODEC` pattern, `GameplayReflect.cpp:43`), factory codegen for Compile builds,
  `editor/tests/reflect/catalog_snapshot.json`, the `ReflectParity` golden key, and
  `engine/core/serialization/RuntimeFields.gen.inc` regenerated (AGENTS.md generator table). Runtime
  fields are not saved. Old scenes load unchanged; no scene version bump (optional new data).
- **R37 Device tier cell.** Appended as int 14 (`DEVICE_OVERRIDE_COUNT` 15); the tier JSON writes
  `simulation_distance` only when it differs from the defaults (S56's codec), so old projects and old
  game.json files load with the defaults; no version bump.
- **R38 Messages.** `ScriptMessage` appends `SimulationCulled`, `SimulationResumed` before `Count` (41 of 63);
  method names `onSimulationCulled`, `onSimulationResumed`. A script that already defined a method of that
  name starts receiving it (documented in the release note; no such name exists in the repository's
  fixtures or templates, to be checked by the implementer with a search).
- **R39 Event clears.** `CullingGroup::onStateChanged` subscriptions and the ground check function are
  cleared at Stop with their scene (`tools/check_event_clears.py` passes).

## 6. Design constraints & decisions

- **D1 One system.** `CullingSystem` (`engine/core/subsystem/CullingSystem.{h,cpp}`) is registered in
  `Scene::init` right after `RenderSystem` (with the comment "after Render's world transforms and camera
  planes, before physics reads frozen bodies"). Its `update` is the R5 evaluation for groups and Simulation
  Culling; it returns at once when it owns no group and the scene has no Simulation Culling component (the
  component array's size). It also runs `evaluateNow()` from the scene start hook (R18) and for instantiated
  GameObjects. Its state is lazily allocated.
- **D2 Data.** Groups are slots `{generation, enabled, flags}` with structure-of-arrays storage: `float x[],
  y[], z[], r[]`, `uint8_t state[]` (bit 7 visible, bits 0-3 band, 0xF = unknown), a sparse follow list
  `{index, entity, offset}`, distances `float[8]`. The loop is branch-light: squared distances against squared
  band edges where `bandHysteresis` is 0; the frustum test is six `dot + d < -r` comparisons on planes copied
  once per evaluation. Changes are pushed into a kept `std::vector<CullingGroupEvent>`. Groups above 4,096
  spheres split into chunks on `jobs::parallelFor`; chunk events are merged in index order (R9).
- **D3 Lua indices.** Sphere indices are 1-based in Lua and 0-based in C++; bands are 0-based everywhere
  (band 0 = nearest), -1 = unknown. `onStateChanged(index, isVisible, wasVisible, band, previousBand)`.
- **D4 Frustum function.** The sphere test moves to a free function `math::sphereInFrustum(planes, farClip,
  center, radius)` in `engine/core/math/Plane.h`; `RenderSystem::isInsideCamera`'s sphere form calls it, so
  the two cannot drift.
- **D5 Ownership.** A group created during a Lua script callback records the owner key the registration code
  already computes (`SubscriptionOwnerIndex`, `LuaBinding.cpp:657-661`); releasing the owner disposes its
  groups. The Lua `CullingGroup` userdata is a handle `{scene, slot, generation}`; garbage collection of the
  handle does not dispose (an explicit `dispose()` or the owner's release does), so a group stored in a local
  keeps working.
- **D6 Simulation group.** Simulation Culling components are one internal group per scene (spheres following
  their GameObjects, distances `reducedRateDistance` and `simulationDistance` per component, so the band test
  is per sphere, not per group) evaluated against the focus list (minimum distance over focuses: one extra
  loop over at most 17 points). State per GameObject lives in `std::vector<uint8_t> simFlags` indexed by entity
  id (bits: culled-scripts, reduced, animator, physics), sized to the scene's entity capacity when the first
  component appears, and propagated down the hierarchy only on transitions (children are walked through
  `EntityRegistry` hierarchy order; a descendant with its own component stops the walk).
- **D7 Script guard.** The guard learns the event's family at registration: `luaRegisterEventImpl` adds an
  upvalue `family` (0 other, 1 update/post-update, 2 fixed-update) from the method name, and
  `engineEventCallback` compares the event's address with `Engine::onUpdate`, `onPostUpdate`,
  `onFixedUpdate`. At call time: `if (family && SimulationCulling::anyActive() &&
  SimulationCulling::skip(scene, entity, family))` return. `anyActive()` is one relaxed atomic load (R19).
  `skip` reads `simFlags[entity]`; for Reduced it compares the frame phase and keeps the accumulated delta in
  a per-entity float, which `Engine::getScriptDeltatime` reads through the `HitStop::ScriptScope` the guard
  already opens (`HitStop::getScriptDeltaTime` gains the reduced-band term). Skipped calls are counted.
- **D8 Animator.** `AnimatorSystem::prepare` (where it reads `cullingMode`, `AnimatorSystem.cpp:424`) treats
  a GameObject whose `simFlags` has the animator bit as `CullCompletely` and counts it in `stats.culled` (the
  existing "Animators culled" monitor includes it).
- **D9 Freeze.** `PhysicsSystem::setSimulationFrozen3D(entity, bool)` / `2D`: freezing saves the linear and
  angular velocity and the awake state in the component's runtime part, sets the motion type kinematic
  (`SetMotionType(..., DontActivate)`) or `b2Body_SetType(kinematic)` with zero velocity; thawing restores
  dynamic, the velocities and wakes it. While frozen, the Lua and C++ Rigidbody getters of `velocity`,
  `angularVelocity` and `isKinematic` return the saved and authored values and setters write the saved state
  (applied at thaw), so scripts and save systems see the real state. A body type change by a script while
  frozen updates the authored type and is applied at thaw. Applied between steps only (never inside the
  step: queued to the next pre-step point, as layer refreshes are, `PhysicsSystem.h:108-112`).
- **D10 Why kinematic, not sleep or disable.** A sleeping body wakes on contact and then falls; a disabled one
  leaves the broad phase (others pass through it, queries miss it). Kinematic with zero velocity stays put
  whatever touches it, which is the guarantee R13 needs.
- **D11 Terrain World check.** T4 gains `TerrainWorldRuntime::isAreaLoaded(x, z, radius)`: every present tile
  within the radius is CpuReady, Uploading or Resident; early-out, no allocation (the existing
  `isAreaReady` keeps its vector). Not the Collision level: T4 builds fine collision only round subjects, and a
  frozen kinematic body is not a subject, so waiting for it would never end; on thaw the body becomes a
  subject again and T4 R22 covers it with a coarse collider in the same pump. A Simulation focus does not
  stream terrain: the docs pair `SimulationCulling.addFocus` with `TerrainWorld:addFocus` for a player.
- **D12 Script ground check.** `SimulationCulling.setGroundCheck(scene, function(x, y, z, radius) return
  loaded end)` (C++: `setGroundCheck(scene, std::function<bool(const Vector3&, float)>)`) for worlds a game
  streams itself (tilemaps, voxel chunks, procedural levels). Called only for components within simulation
  distance (a far one is culled anyway), at most 256 calls per frame (round robin), on the main thread. One
  function per scene; `nil` removes it.
- **D13 Ordering in the frame.** Frame N: `CullingSystem::update` after Render decides states; messages and
  `onStateChanged` are dispatched there. Frame N+1: fixed steps see frozen bodies, the Animator and scripts
  see the flags. One frame of latency, as Unity's CullingGroup (its results come from the previous frame's
  culling).
- **D14 MCP budget.** No new tool. `get_render_stats` description +~40, `configure_game_view` +~30 (inferred
  from the schema helpers, not measured; both outside `core`); `tool_budget.py --check` gates them.

C++ sketch:

```cpp
namespace jasperengine {
    struct BoundingSphere { Vector3 position; float radius = 0.0f; };
    struct CullingGroupEvent {
        uint32_t index; bool isVisible, wasVisible; int8_t currentDistance, previousDistance;   // bands, -1 unknown
        bool hasBecomeVisible() const { return isVisible && !wasVisible; }
        bool hasBecomeInvisible() const { return !isVisible && wasVisible; }
    };
    class JASPERENGINE_API CullingGroup {                 // a handle; copies share the group
    public:
        static constexpr uint32_t MAX_SPHERES = 100000;  static constexpr size_t MAX_DISTANCES = 8;
        static CullingGroup create(Scene* scene);         // invalid handle past 256 groups (error logged)
        void dispose();  bool isValid() const;
        void setEnabled(bool enabled);  bool isEnabled() const;
        void setTargetCamera(Entity camera);  Entity getTargetCamera() const;   // NULL_ENTITY: any enabled camera
        void setBoundingSpheres(std::span<const BoundingSphere> spheres);
        void setBoundingSphereCount(uint32_t count);  uint32_t getBoundingSphereCount() const;
        void setSphere(uint32_t index, const BoundingSphere& sphere);
        void setSphereTarget(uint32_t index, Entity entity, float radius, const Vector3& offset = Vector3::ZERO);
        void eraseSwapBack(uint32_t index);
        void setBoundingDistances(std::span<const float> distances);
        void setDistanceReferencePoint(const Vector3& point);  void setDistanceReferencePoint(Entity entity);
        void setBandHysteresis(float metres);
        bool isVisible(uint32_t index) const;  int getDistance(uint32_t index) const;
        uint32_t queryIndices(bool visible, std::span<uint32_t> out, uint32_t first = 0) const;
        uint32_t queryIndices(int band, std::span<uint32_t> out, uint32_t first = 0) const;
        uint32_t queryIndices(bool visible, int band, std::span<uint32_t> out, uint32_t first = 0) const;
        FunctionSubscribe<void(CullingGroupEvent)>& onStateChanged();
    private:
        Scene* scene = nullptr; uint32_t slot = 0, generation = 0;
    };
    enum class SimulationState : uint8_t { Full, Reduced, Culled };
    enum class SimulationCullReason : uint8_t { None, Distance, GroundNotLoaded };
    struct SimulationCullingComponent {                   // engine/core/component/SimulationCullingComponent.h
        bool enabled = true;
        float simulationDistance = 64.0f, reducedRateDistance = 0.0f, hysteresis = 2.0f, boundsRadius = 0.0f;
        int reducedUpdateInterval = 4;
        bool simulateWhenVisible = true, freezeWhenGroundNotLoaded = true;
        bool cullScripts = true, cullAnimator = true, cullPhysics = true;
        // runtime, not saved
        SimulationState state = SimulationState::Full; SimulationCullReason reason = SimulationCullReason::None;
        float focusDistance = 0.0f;
    };
    class JASPERENGINE_API SimulationCulling {           // statics over the scene's CullingSystem
    public:
        static int addFocus(Scene*, const Vector3& position);  static int addFocus(Scene*, Entity entity);
        static bool setFocus(Scene*, int id, const Vector3& position);  static bool removeFocus(Scene*, int id);
        static void setCameraIsFocus(Scene*, bool);  static bool isCameraFocus(Scene*);
        static void setDistanceScale(float scale);   static float getDistanceScale();      // times the Device Tier cell
        static void setGroundCheck(Scene*, std::function<bool(const Vector3&, float)> check);
        static bool anyActive();                                                           // one relaxed load
        static bool isCulled(Scene*, Entity);  static SimulationState getState(Scene*, Entity);
    };
    // CullingSystem::Stats { groups, spheres, visible, perBand[9], evaluateMs, components, full, reduced, culled,
    //                        groundNotLoaded, focuses, scriptUpdatesSkipped, transitionsDeferred }
    // CullingSystem: Stats getStats() const; size_t getMemoryBytes() const;   // R24, AC6
    // PhysicsSystem: void setSimulationFrozen3D(Entity, bool); void setSimulationFrozen2D(Entity, bool);
    // TerrainWorldRuntime: bool isAreaLoaded(float x, float z, float radius) const;   // no allocation
    // DeviceOverride::SimulationDistance = 14; QualityResolver::getSimulationDistanceScale()
}
```

Lua:

```lua
-- wake enemies when they come on screen or near, and run their AI by band
local group = CullingGroup(self.scene)
group:setBoundingDistances({15, 40, 90})                     -- bands 0..3
group:setBoundingSphereCount(#self.enemies)
for i, enemy in ipairs(self.enemies) do group:setSphereTarget(i, enemy.entity, 1.0) end
group:setDistanceReferencePoint(self.player)                 -- a GameObject (followed) or a Vector3
group.targetCamera = nil                                     -- any enabled camera (default)
group.onStateChanged = function(index, isVisible, wasVisible, band, previousBand)
    self.enemies[index].think = (band <= 1) or isVisible
end
local visibleNow = {}                                        -- reused
local n = group:queryIndices(true, visibleNow)               -- visible spheres; also (band, t) and (true, band, t)
print(group:isVisible(1), group:getDistance(1))
group:eraseSwapBack(3) ; group.enabled = false ; group:dispose()

-- Simulation Culling: no code needed; scripts may react
function Grunt:onSimulationCulled(reason)                    -- "distance" or "ground_not_loaded"
    NavMeshAgent(self.scene, self.entity).isStopped = true
end
function Grunt:onSimulationResumed() NavMeshAgent(self.scene, self.entity).isStopped = false end
local culling = SimulationCulling(self.scene, self.entity)
print(culling.state, culling.reason, culling.focusDistance)  -- "culled", "ground_not_loaded", 12.5
culling.simulationDistance = 80
local id = SimulationCulling.addFocus(self.scene, playerGameObject)   -- multiplayer: one per player
SimulationCulling.setGroundCheck(self.scene, function(x, y, z, r) return myWorld:isLoaded(x, z, r) end)
SimulationCulling.distanceScale = 0.75
```

MCP shapes:

```json
{"tool": "get_render_stats", "culling": {"groups": 2, "spheres": 1200, "visible": 310,
  "per_band": [140, 420, 380, 260], "evaluate_ms": 0.031,
  "simulation": {"components": 800, "full": 120, "reduced": 260, "culled": 420, "ground_not_loaded": 37,
                 "focuses": 1, "script_updates_skipped": 1260, "transitions_deferred": 0}}}
{"tool": "configure_game_view", "args": {"culling_debug": true}}
{"tool": "get_runtime_entity", "components": {"SimulationCulling": {"simulationDistance": 64, "state": "culled",
  "reason": "ground_not_loaded", "focusDistance": 41.2}}}
```

## 7. Acceptance criteria

Unit tests in `engine/tests/core/subsystem/CullingSystemTest.cpp` (suites `CullingGroup`,
`SimulationCulling`), `engine/tests/core/terrain/TerrainWorldResidencyTest.cpp` (suite
`TerrainWorldResidency`, beside `TerrainWorldStreamingTest.cpp`), `engine/tests/core/script/LuaCullingGroupTest.cpp`
(suite `LuaCullingGroup`), `engine/tests/core/animation/AnimatorSystemTest.cpp` (appended),
`engine/tests/core/device/QualityResolverTest.cpp` (appended), bench `engine/tests/core/bench/CullingBench.cpp`
(keys in `tools/perf/bench-thresholds.json`), editor tests in `editor/tests/reflect/` (catalog and parity) and
`editor/tests/` (Inspector rows), MCP tests appended to `tools/mcp/tests/test_render_stats.py` (`CI = True`).
"Red" = the run on the tree without the change.

| AC | Verifies | Check |
|---|---|---|
| AC1 | R3, R4, R5, R6 | `CullingGroup.BandsVisibilityAndEvents`: 5 spheres, distances {10, 20}, a camera: the first evaluation reports all 5 with previous band -1; moving one sphere across 10 m and out of the frustum reports exactly that sphere once with the right (isVisible, wasVisible, band, previousBand); nothing else fires; hysteresis 1 m keeps a sphere at 10.5 m in band 0; `enabled = false` stops events and re-enabling reports only what changed meanwhile |
| AC2 | R4 | `CullingGroup.AnyCameraOrTarget`: two cameras looking apart: with no target a sphere seen by either is visible; with a target only that camera counts; a disabled target → all invisible; far clip 0 skips the far plane; orthographic camera planes work |
| AC3 | R2, R7 | `CullingGroup.FollowEraseAndQuery`: followed entities move and their spheres follow; a destroyed one keeps its last position with one warning; `eraseSwapBack` moves state; the three `queryIndices` forms with `first` match a brute-force filter over 1,000 random spheres |
| AC4 | R9, R20 | `CullingGroup.ParallelEqualsSerial`: 20,000 spheres evaluated with jobs and inline give identical states and identical event sequences (memcmp) |
| AC5 | R1, R8, D5 | `LuaCullingGroup.OwnershipAndReentry`: a group created in a script's `init` is disposed when the script's GameObject is destroyed (`isValid` false from a held handle); a callback that disposes its group stops the remaining events; a callback that changes distances sees them applied next frame; the 257th group errors |
| AC6 | R23, R19, R24 | `CullingGroup.NoAllocationsAndZeroCost`: after the first evaluation, 1,000 evaluations with changes allocate 0 (`AllocationCountScope`); a scene with neither groups nor components: `CullingSystem::update` allocates 0 and touches no array (a counter of evaluated spheres stays 0); `LuaCullingGroup.CallbackMakesNoGarbage`: Lua GC count unchanged over 100 frames of events; `CullingSystem::getMemoryBytes()` for 10,000 spheres, 2,000 followed, is within 5 % of R24's 232,000 bytes |
| AC7 | R10, R12, R14 | `SimulationCulling.DistanceCullsScriptsAnimatorAndBodies`: a GameObject 100 m from the camera with a C++ and a Lua script counting `onUpdate`/`onFixedUpdate`/`onPostUpdate`, an Animator and a dynamic body: counts stop, Animator time stops, the body does not move under gravity for 300 fixed steps; input events still reach the script; moved to 10 m → all resume the next frame and the body keeps its pre-freeze velocity. Red: without the component the body falls and counts grow |
| AC8 | R13, D11 | `TerrainWorldResidency.GroundNotLoadedFreezes` (the Craftsman defect as a test): a Terrain World with tile I/O held by T4's test hook, a dynamic body and a script-moved GameObject (moving down 1 m/s in `onUpdate`) 30 m from the camera over a present tile that is Cold: both stay at their start height for 600 fixed steps with reason `ground_not_loaded`; releasing the hook loads the tile and both resume within 2 frames of CpuReady; over an absent tile (world edge) they are not frozen. Red: without the component the script-moved object sinks below -100 m |
| AC9 | R13 | `TerrainWorldResidency.IsAreaLoadedNoAlloc`: `isAreaLoaded` agrees with "all present tiles CpuReady or later" on 1,000 random areas, allocates 0, and is true where `isAreaReady(Collision)` is false (no fine collision yet) |
| AC10 | R15, D7 | `SimulationCulling.ReducedBandAccumulatesDelta`: interval 4 at 60 fixed fps: `onUpdate` runs every 4th frame (phase = entity id mod 4), `Engine.deltatime` inside equals the sum of the 4 frames' scaled deltas (± 1e-6), `onFixedUpdate` runs every step; two GameObjects with ids 0 and 1 run on different frames |
| AC11 | R11, R16 | `SimulationCulling.FocusesAndTierScale`: two focuses, distance = min; a focus GameObject is never culled; removing all script focuses with `cameraIsFocus` false culls everything by distance; `QualityResolverTest.SimulationDistanceCell` (beside `CellsCapAndInherit`): tier Low resolves 0.6, a project cell of 0.5 wins, clamped to 0.25..1; a scale of 0.5 halves the effective distances |
| AC12 | R17, R38 | `SimulationCulling.MessagesOncePerTransition`: Full → Culled sends one `onSimulationCulled("distance")`, Culled → Reduced one `onSimulationResumed()`, Reduced ↔ Full nothing; disabled scripts receive them; order by entity id |
| AC13 | R18 | `SimulationCulling.FirstEvaluationBeforeFirstStep`: (a) a scene that starts with 500 dynamic bodies with components over an unloaded tile (a C++ ground check returning false, set before the scene starts): none moves in the first fixed step, all 500 are Culled after the start evaluation (ground transitions are never deferred); (b) 500 Full components whose focus jumps 1 km away: 64 change per frame, farthest first, all Culled after 8 frames, `transitions_deferred` reported meanwhile |
| AC14 | D9, R21 | `SimulationCulling.FrozenBodyReportsRealState`: while frozen, `Rigidbody.velocity` returns the saved velocity, a script's `velocity = v` is applied at thaw, `isKinematic` reads the authored false; 2D the same (`SimulationCulling.FrozenBody2D`); other bodies collide with the frozen one as with a kinematic; `Physics.overlapSphere` (when `spatial-queries.md` has landed) finds it; with `JASPERENGINE_PHYSICS_2D` off (the existing module-off build of the 2D backend) the 2D test is compiled out and the 3D one still passes |
| AC15 | D8 | `AnimatorSystemTest` `SimulationCulledAnimatorAdvancesNothing` (appended near the CullCompletely test, `AnimatorSystemTest.cpp:545`): culled animator time and events stay, "Animators culled" counts it |
| AC16 | R22 | `CullingBench` keys `group_10k_eval_ms` (0.132), `group_10k_follow_eval_ms` (0.22), `simulation_1k_eval_ms` (0.066), `simulation_64_transitions_ms` (0.165), `ground_1k_ms` (0.022), `culling_zero_cost_ms` (0.0022): desktop budgets + 10 %, enforced with `perfSlack()` on the benchmark run |
| AC17 | R20, R22 | Web: the `core-utilities` test project gains a scene with a 10,000-sphere group and 1,000 Simulation Culling components; `GATE=core-utilities` checks console lines for the evaluation p99 (<= 0.5 ms group, <= 0.25 ms simulation) and the expected event count after a scripted camera move (`tools/web/gates/core-utilities.json`) |
| AC18 | R36, R37 | `ReflectParity` golden gains the component's keys; `catalog_snapshot.json` updated; a scene saved without the component loads unchanged (existing fixtures, no diff); `DeviceSettings` JSON without `simulation_distance` loads the defaults (`QualityResolverTest.OldTierJsonLoads`) |
| AC19 | R25, R26 | Editor test `InspectorSimulationCulling.RowsAndUndo` (in `editor/tests/`, with the other Inspector tests): the rows and foldout exist with Unity-like labels, Reduced Update Interval hides at distance 0, one edit is one undo step, multi-edit of two GameObjects; the Device Tier column edits and resets |
| AC20 | R29, R31 | `test_render_stats.py::test_culling_block`: a fixture scene with 3 enemies (Simulation Culling, 20 m distance) and a Lua CullingGroup; in Play with `fixed_fps`, `get_render_stats` reports `culling.simulation.culled == 2` for the two beyond 20 m and the group's `per_band`; moving the camera with `set_runtime_property` changes them after one `wait` frame; `get_runtime_entity` shows `state: culled`, `reason: distance`; without groups or components `culling` is absent |
| AC21 | R27, R30 | `test_render_stats.py::test_culling_debug_screenshot`: `configure_game_view {culling_debug: true}` and a Game view `screenshot` differ from one with it off where the spheres project (pixel diff inside, none outside) |
| AC22 | R32, R28 | `test_render_stats.py::test_culling_monitors`: `get_frame_times {monitors: ["Simulation culled", "Script updates skipped"]}` report the expected numbers for the fixture; `DebugOverlay.getStats()` (via `eval_lua`) has the R28 fields |
| AC23 | R34, R35, R39, D14 | `gen_tool_docs.py --check`, `tool_budget.py --check`, `test_tool_conventions.py`, `test_autosync.py`, `check_event_clears.py`, `check_agent_docs.py` pass; R35's eval cases fail on the pre-change tool list and pass after (Orquester-managed accounts; else reported unverified) |

## 8. Dependencies & conflicts

- **Depends on (landed):** S07 scheduler, S15 monitors, S21 Animator, S56 Device Tiers, T4 Terrain Worlds
  (the ground rule's built-in provider; without the module the rule uses only the script check).
- **Sibling specs:** `spatial-queries.md` (frozen bodies stay queryable; both extend `configure_game_view`, the
  Game view toolbar and the Stats panel: separate parameters and lines, a textual merge);
  `release-performance-and-gpu-quirks.md` (if it adds Device Tier cells, both append to `DeviceOverride`:
  whichever lands second takes the next int, and the tier table and JSON keys are merged; never renumber);
  `savegame.md` (games that save body state read the real velocity through D9's getters);
  `input-recorder-and-input-scripts.md` and `project-test-runner.md` (R9 determinism makes replays and tests
  stable); `cancellable-prioritised-events.md` (if it changes `FunctionSubscribe` dispatch, `onStateChanged`
  and the guard follow it); `shipped-game-diagnostics.md` and `dev-console-for-agents.md` (can read
  `CullingSystem::Stats`; they must not add a second counter); `build-size-tools.md` (the new code is small,
  estimated under 20 KB, inferred).
- **Files touched that others touch:** `engine/core/Scene.cpp` (registration), `script/LuaBinding.cpp` and
  `util/FunctionSubscribe.h` (the guard), `pixelart/HitStop.cpp` (script delta), `script/ScriptMessages.{h,cpp}`,
  `subsystem/AnimatorSystem.cpp`, `subsystem/PhysicsSystem.{h,cpp}`, `object/physics/Rigidbody.cpp`,
  `object/physics/Rigidbody2D.cpp`, `subsystem/TerrainWorldRuntime.{h,cpp}`, `device/DeviceSettings.{h,cpp}`,
  `device/QualityResolver.{h,cpp}`, `math/Plane.h`, `subsystem/RenderSystem.cpp` (one call),
  `debug/DebugStats.{h,cpp}`, `profiler/EngineProfiling.cpp`, `editor/window/GameWindow.cpp`,
  `editor/mcp/tools/ViewTools.cpp`, `editor/mcp/tools/ProfilerTools.cpp`, `editor/reflect/components/GameplayReflect.cpp`,
  `editor/tests/reflect/catalog_snapshot.json`, `engine/core/serialization/RuntimeFields.gen.inc`,
  `tools/perf/bench-thresholds.json`, `tools/test-projects/core-utilities`, the docs of R34.
- **Blocks:** nothing indexed; large-world samples and multiplayer templates want it.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| The first evaluation reports every sphere (a burst of callbacks on creation) | Documented (R6); scripts can ignore events with `previousBand == -1`; 10,000 Lua calls cost ~5 ms once (inferred from LuaBindingBench's call cost) |
| A culled GameObject's coroutine or timer moves it anyway | Documented: only update callbacks are culled; such scripts check `SimulationCulling(...).state` or react to `onSimulationCulled` |
| A script reads `Engine.deltatime` outside its callbacks in the reduced band | Outside a guarded callback there is no script scope: it reads the frame's delta, as with hit stop today |
| Frozen bodies act as walls | Kinematic is deliberate (D10); documented with the alternative of disabling `cullPhysics` |
| A body whose script moves it via `MovePosition` while frozen | Writes go to saved state (D9); the move applies at thaw; documented |
| Hierarchies: a culled parent with an always-simulating child | The child gets its own component with `enabled` false ("always simulate"): nearest component wins (R14) |
| A Simulation focus far from every T4 focus leaves ground unloaded forever | Documented pairing (D11); `get_render_stats` `ground_not_loaded` makes it visible; the `profiling.md` recipe shows it |
| Visibility from shadow-casting objects off screen (shadows visible, object not) | Frustum only, documented; raise `boundsRadius` for tall objects |
| The guard change touches every script callback path | One relaxed load when unused (R19, AC6, AC16 `culling_zero_cost_ms`); the family is computed once at registration |
| `DeviceOverride` append conflicts with another spec | Append-only ints; second to land renumbers only its own new cell before merge; JSON keys are names, not ints |
| Ground check callbacks are slow | 256 calls per frame cap, round robin; budget measured in AC16 only for the T4 provider; Lua cost documented |
| Multiple running scenes (child scenes) | Each scene has its own system, focuses and groups; the main camera is the scene's own (`Scene::getCamera`) |

## Deviations

- **D1 registration and start.** `CullingSystem` is registered right after `PhysicsSystem` (still after
  Render's transforms and camera planes): its update order against physics does not matter (physics steps
  in the fixed phase of the next frame) and the bodies exist when it first runs. The first evaluation
  (R18) and the evaluation of GameObjects instantiated since the last update run in
  `CullingSystem::preUpdate`, called by `Engine::runFrame` at the start of every frame, before the
  scripts' starts and the fixed steps (a scene has no start hook that runs after `PhysicsSystem::load`
  in every host; the editor loads Play scenes paused).
- **R36 codec.** The component uses the reflection table's generic encoding (as the other table-only
  components added since S08, e.g. Water Excluder), not a hand-written `JE_STREAM_CODEC`.
- **R24 memory.** A sphere takes 20 bytes (four floats and a packed word: its state and its follow
  slot), a followed sphere 20 more (index, entity, offset). Simulation Culling keeps 1 byte per entity
  id, plus a 32-byte reduced-band clock per entity id once a GameObject enters the reduced band (the
  phase and delta are per GameObject, descendants included).
- **R11, R33 `cameraIsFocus`.** It is a scene setting, so Lua has
  `SimulationCulling.setCameraIsFocus(scene, bool)` / `isCameraFocus(scene)` rather than a static
  property; `distanceScale` is a static property.
- **R27, R26 editor.** The Game view toolbar has no Gizmos menu: **Culling** is a toggle beside
  **Physics** (saved with the Game view settings). The Device Tiers table has a row per setting, so
  Simulation Distance is a row.
- **R13 ground checks.** A GameObject culled by unloaded ground is checked again every frame (so it
  resumes within 2 frames of its tile loading, AC8); the others round robin every 8 frames.
- **R15 reduced band** applies only with Cull > Scripts on (it only changes scripts).
- **R18 pacing.** Reduced to Full changes count in the 64 a frame too (after resumes and culls).
- **D9 2D inside a step.** Simulation Culling never decides inside a step; a 2D freeze asked for inside
  one is ignored (the 3D one is queued to the step's end).
- **R23 allocations** hold for the evaluation inline (`CullingGroupTest.NoAllocationsAndZeroCost`: 0 in the
  whole process over 1,000 evaluations with changes). Groups above 4,096 spheres evaluated on S06's
  `parallelFor` saw about 55 allocations on the calling thread over 1,000 evaluations: the job system
  publishing its helpers, not the culling code (measured, not fixed here).
- **D12 cap at start.** The first evaluation of a scene and of a new GameObject calls the script ground
  check for every component (AC13 starts 500 over unloaded ground); the 256-a-frame cap applies to the
  later evaluations.
- **R33 `onStateChanged`** is write-only in Lua (reads nil). C++ `onSimulationCulled` takes
  `const std::string& reason`.
- **Tests.** The scripted cases (AC5, AC6's Lua part, AC7's script counts, AC10, AC12) run whole engine
  frames with Lua scripts and are in `editor/tests/culling/SimulationCullingScriptTest.cpp`, which has
  the script harness; the engine cases are in `engine/tests/core/subsystem/CullingSystemTest.cpp`
  (fixtures `CullingGroupTest` and `SimulationCullingTest`, per docs/testing.md's naming), AC7's Animator
  in `AnimatorSystemTest` (AC15) and its bodies in `SimulationCullingTest.DistanceFreezesBodies`. The
  Inspector test is `editor/tests/reflect/InspectorSimulationCullingTest.cpp`. The bench is a
  `je-bench-culling` executable gated by `tools/perf/check_bench.py`.
- **AC17 (web gate)** is not done: no web build or `core-utilities` gate change was made in this
  change; the single-threaded path (parallelFor inline) is covered by `CullingGroupTest.ParallelEqualsSerial`.
- **AC17 (integration pass).** `tools/test-projects/core-utilities` holds the two sets in scene layers
  of their own (a 10,000-sphere group; 1,000 Simulation Culling components), so each scene's
  `CullingSystem::getStats().evaluateMs` is one of them; the gate reads 200 frames of each from 4 s of
  game time (after the Performance line it also checks in order). The scripted move turns the group's
  camera away (10,000 events, every one becoming invisible) and moves the simulation camera 30 m back
  (500 culled). R22's figures are release ones, so the gate runs on `BUILD_TYPE=Release`: a Debug
  build measured 2.19 ms and 0.505 ms. Release runs on build box B, shared with other jobs: the group
  p99 0.515, 0.355 and 0.385 ms (p50 0.19-0.23), the simulation p99 0.095, 0.085 and 0.090 ms, the
  events as expected each time; the first run's group p99 was over its 0.5 ms budget, the next two under.
- **AC17 (budgets reported, not asserted).** The gate prints both p99s with R22's web budgets every run,
  and a p99 over its budget is a warning line, not a failure; the gate's required lines are the
  measurement lines and the events after the camera move. R22's figures are device budgets, and the
  merge queue's hosted runner renders in software (SwiftShader): there the group's p50 alone was 0.63 ms
  and its p99 1.005 ms, the simulation's p99 0.34 ms (run 45811956539), so a hard check measured the
  runner, not the engine. The gate had never run in CI before (a PR run does not select it). Wall-clock
  budgets are reported, never asserted (AGENTS.md); the desktop benches (AC16) still enforce theirs
  with `perfSlack()`.
- **R22 on box A.** `je-bench-culling` measured `group_10k_follow_eval_ms` 0.25-0.29 ms (budget 0.22),
  `group_10k_eval_ms` 0.126-0.133 ms (0.132) and `ground_1k_ms` 0.017-0.024 ms (0.022) on box A (a
  2.4 GHz Haswell), run after run between steady runs of a main-thread-only loop: costs at the edge,
  not noise. A group's evaluation now chooses the cameras it tests once and tests each sphere's planes
  without a branch apiece; a group on the workers reads its followed spheres' positions in the same
  chunks (a destroyed GameObject is still dropped and warned about once:
  `CullingGroupTest.ManyFollowsReadOnWorkers`) and is claimed 256 spheres at a time, so a worker that
  wakes late holds little of it; the ground rule finds the TerrainWorldSystem once a pass instead of
  once a check. Six quiet runs: follow 0.099-0.170 ms, fixed 0.080-0.113 ms, ground 0.002-0.009 ms.
