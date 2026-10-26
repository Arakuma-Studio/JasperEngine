# S22 — Gameplay: character controller, buoyancy, input

Wave 2. Depends on S01 (C++20), S02 (gtest + `engine/tests/core/`), S03 (versioned writes), S04
(asset GUIDs), S07 (component-type cap), S24 (undo naming). Read `README.md` first.
Delivery order inside the stream: **P1** input edges / delta / touch phases → **P2** 3D
CharacterController → **P3** WaterVolume + Buoyancy → **P4** Input Actions asset, editor, rebinding
→ **P5** CharacterController2D. Each phase is mergeable on its own.

## 1. Goal

Give game code the three gameplay primitives that every Unity-style project expects and we lack.
(1) A Unity-like `CharacterController` built on Jolt `CharacterVirtual`: stairs, slopes, skin width,
`move`/`simpleMove`, `isGrounded`, collision flags, rigidbody pushing and moving platforms. It also
gets a root-motion entry point for S21 and a Box2D equivalent for 2D games.
(2) Buoyancy whose water height comes from a C++ mirror of our water shader's wave function, so a
floating object rides the waves that are drawn.
(3) Input that never loses a tap: frame-edge queries for every device, raw mouse delta, and a small
but complete Input Action system (maps, bindings, composites, prioritised and consuming maps,
runtime rebinding with persistence). It must work the same on desktop, mobile, web, in the editor
Game view and under MCP `send_input`.

## 2. Scope

In scope:
- `Input` frame edges (keys, mouse buttons, touches, gamepad buttons), with press+release in one
  frame, exactly-once semantics in the fixed-update phase, per-frame mouse delta and scroll delta,
  touch phases, and pointer-lock state.
- `InputActions` runtime (`engine/core/input/`), the `.inputactions` asset (JSON), the Input Actions
  editor window, Project Settings > Input, rebinding, override persistence, and virtual controls for
  on-screen sticks and buttons (HTML UI).
- `CharacterControllerComponent` (3D, Jolt) and `CharacterController2DComponent` (Box2D, P5).
- `WaterVolumeComponent`, `BuoyancyComponent`, a CPU wave mirror and water surface queries.
- Lua API, MCP tools, `send_input` extensions, docs (`docs/mcp/guide/physics.md`, a new
  `docs/mcp/guide/input.md`, `docs/mcp/TOOLS.md`), and tests.

Out of scope:
- The Jolt `JPH::Character` "dynamic motor". A controller is kinematic. Physics-driven actors use a
  dynamic Rigidbody, as they do today.
- Ragdolls, vehicles, a 2D buoyancy model, gamepad rumble, per-player device pairing beyond a
  per-map gamepad filter, and interactions beyond press and hold (no tap, slow-tap or multi-tap).
- FFT or Gerstner oceans as a primary path, and GPU readback of water height (README do-not-copy
  list). A new wave model belongs to whoever changes `water.glsl`. That change must update the
  mirror (R28).
- OpenEngine's `InputSystem.cpp` sink, claim and router implementation (analysis 04 §6). We take
  its data model only.
- Editor shortcut rebinding. That is S25. S22 only exposes `InputActions::isControlBound` for S25's
  routing.

## 3. Current state (verified)

**Input.**
- `engine/core/Input.h/.cpp` holds level state only, in `std::map<int,bool>` (`keyPressed`,
  `mousePressed`), plus `touches`, `gamepads` and `modifiers`. It has no edges, no history and no
  delta. Lua binds `isKeyPressed`, `isMousePressed`, `getMouseScroll`, `getTouches` and the gamepad
  getters (`engine/core/script/binding/CoreClassesLua.cpp:1494, 1666-1682`).
- `Input::setMouseScroll` is only called from `Engine::systemMouseScroll` (`Engine.cpp:2025`) and
  is never reset. `getMouseScroll()` therefore returns the last wheel event forever (a bug).
- Events reach `Engine::system*` between frames. `Engine::systemDraw` (`Engine.cpp:1219-1300`)
  runs 0..100 fixed steps (`onFixedUpdate`, then `Scene::fixedUpdate`), then `onUpdate`,
  `Scene::update` and `onPostUpdate`, and skips all of these while `paused || simulationHeld`.
  Lua code that checks a key in `onFixedUpdate` can miss it (a frame with 0 steps) or see it twice.
- UI consumption: HTML documents consume events before `onKeyDown` and the other subscriptions,
  but "Input's polling functions always read the device" (`Engine.h:543-548`).
- Captured mouse: Windows reads `WM_INPUT` raw motion (`WindowWin.cpp:221-226`,
  `WinInputRouter.cpp:244-255`). Linux and Windows routers keep a continuous virtual position
  while captured (`WinInputRouter.h:53`, `LinuxInputRouter.h:49`). Web requests pointer lock and
  accumulates `movementX/Y` (`JasperEngineWeb.cpp:490-495, 782-795`). The lock state is not
  exposed. `System::setMousePosition` warps the pointer (`System.h:144`).
- Gamepads: `generate_gamepad_mappings.py` produces `engine/platform/common/GamepadMappings.h`
  (SDL_GameControllerDB), with per-platform pads in `engine/platform/*/Gamepad*.cpp`.
- Editor: `editor/window/PlayInput.cpp` forwards ImGui input to `Engine::system*`, tracks held
  keys and buttons so nothing stays stuck (`PlayInput.h:20-31`), routes the captured mouse by
  focus (`:34-37`), and yields the pointer to MCP injection (`pointerInjected`).
- MCP `send_input` (`editor/mcp/tools/PlayTools.cpp:3509`) schedules key, mouse, touch and
  gamepad events across frames. `hold_frames` is clamped to ≥ 1 (`:2404`), so a same-frame tap
  cannot be tested.

**Physics.**
- `PhysicsSystem` (`engine/core/subsystem/PhysicsSystem.cpp`, 3227 LOC) runs Box2D v3.0 and Jolt
  5.2 (`engine/libs/joltphysics/Jolt/Core/Core.h:8-10`). `update()` does nothing (`:2948`). All
  stepping is in `fixedUpdate()` (`:2954`), and the Jolt step only runs `if (bodies3d->size() > 0)`
  (`:3127`).
- Layers: `JoltObjectLayerTable` + `JoltObjectLayerPairFilter` (`engine/core/util/JoltPhysicsAux.h:75+`)
  combine the Layer Collision Matrix with category and mask bits.
- Jolt body user data is the `Entity` (`PhysicsSystem.cpp:1538`), and `RayReturn.body` is an
  `Entity` (`engine/core/math/Ray.h:28-34`). Sensors do not set `mCollideKinematicVsNonDynamic`
  (`PhysicsSystem.cpp:1519`), so kinematic bodies do not trigger them.
- `Rigidbody` has `applyForce(force, point)`, `applyTorque` and `getPointVelocity`
  (`engine/core/object/physics/Rigidbody.h:148-161`).
- We have no character controller in either dimension. `CharacterVirtual.h` (with inner body,
  `CharacterContactListener` and `CharacterVsCharacterCollisionSimple`) is vendored but unused.
  Box2D v3.0 has shape casts and overlaps (`engine/libs/box2d/include/box2d/box2d.h:54-103`) but
  **no** mover API (`b2World_CastMover`/`b2SolvePlanes` arrived in v3.1).
- Subsystem order is Action, Mesh, UI, UIDocument, Render, Physics, Audio (`engine/core/Scene.cpp:34-40`).

**Water.**
- `library:water` (`engine/shaderlib/library/water.frag` manifest, `water.vert`, `water_vs.glsl`,
  `water.glsl`) uses two sine waves, `waterWaves(xz, time, direction, height, waveLength, speed)`
  (`water.glsl:15-28`). The second wave is turned 50° (0.6428/0.7660), 0.6× as long, half as high,
  with a +0.37 cycle phase, and phases are wrapped with `fract`. It is a **vertical displacement
  only** at world XZ (`water_vs.glsl:19-24`). The fragment stage uses the same slope (`water.frag:150`).
- Time is `frame.time.x`, which is `RenderSystem::frameTime = (float)Engine::getSystemTime()`:
  **wall clock** since startup, set once per frame in `drawFramePasses` (`RenderSystem.cpp:13280`).
  It ignores pause, time scale and `fixedDeltatime`.
- Parameters live in `Material::shaderParams` (`ShaderUniformValues`, a vector of
  name→`Vector4`), with manifest defaults as fallback (`engine/core/texture/Material.h:261-269`,
  `engine/core/shader/ShaderUniforms.h:20,53`). `WAVES` is in `Material::shaderFeatures`.
- Nothing on the CPU knows the water height. There is no buoyancy and no water volume.

## 4. Reference (OpenEngine, MIT; `OE/` = `external/OpenEngine/`)

- `OE/Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp:578-622` is the character step.
  **Take** `mWalkStairsMinStepForward = max(0.02, radius)`,
  `mWalkStairsStepForwardTest = max(0.15, radius + 0.05)`, `mWalkStairsStepUp = (0, step, 0)`,
  `mStickToFloorStepDown = (0, -step, 0)`, and zero both when the step is 0. **Improve**: they
  step characters after the world update with the default layer filters. We use our layer filters
  and step platform carry after the world update, and moves happen when the script calls them.
- `:1678-1730` (`CreateCharacter`). **Take** `mSupportingVolume = Plane(up, -radius)` and
  `mCharacterPadding = skinWidth`. **Improve**: they set no inner body (rays and dynamic bodies
  cannot see the character), no `CharacterContactListener` (no hit callbacks, no push control) and
  no character-vs-character collision. We add all three.
- `OE/Engine/Modules/PhysicsECS/{Include/PhysicsECS/Components/CharacterController.h,Source/Systems/CharacterControllerSystem.cpp}`.
  **Take**: refuse to coexist with a body component, log once, and treat root motion as
  displacement/dt with a non-finite guard. **Don't take** the desired-velocity mailbox. We use
  Unity's imperative `move`.
- `OE/Engine/Modules/Ocean/Source/Systems/OceanBuoyancySystem.cpp`. **Take** world-space probe
  footprints that ignore transform scale, force at the probe position, and drag relative to water
  velocity. **Improve**: theirs is a spring (`mass × strength × submergence`) run in variable
  `Update`. Ours is Archimedes (ρ·g·V_submerged) in the fixed step, so equilibrium follows from
  densities.
- `OE/Engine/Modules/Ocean/Source/OceanCollisionProvider.cpp`: the analytic fallback idea only. We
  need no GPU query path because our waves are purely analytic.
- `OE/Engine/Modules/Input/Include/Input/InputSystem.h` (`ActionDesc`, `ActionEvent.consumed`, the
  context push/pop priority, `WasKeyPressed` frame snapshots) and
  `OE/Engine/Modules/Input/Include/Input/InputScriptApi.h:240-390` (`ActionBinding` with scale and
  required/forbidden modifiers; `PointerLockState` Unlocked/Pending/Locked). **Take** the model and
  the edge contract ("pressed this frame, also when it came up again in the same frame"). **Don't
  take** `InputSystem.cpp`: its persistence is a stub (`:698-706`) and it has no touch.
- Licence: we take ideas and small snippets only. Any copied snippet keeps "Copyright (c) 2026
  Gamecrafters Guild Limited and contributors" and gets a `NOTICE.md` entry.

## 5. Requirements

### Functional — input edges (P1)
- **R1** `Input` keeps a per-control record for keys (0..`D_KEY_LAST`), mouse buttons (0..7) and
  gamepad buttons (per pad id, 0..`D_GAMEPAD_BUTTON_LAST`): the level, a press count and a release
  count. Events add to a *pending* window. When a frame runs its update phase, `Input::beginFrame()`
  (called first inside the `!paused && !simulationHeld` block of `Engine::systemDraw`) latches
  pending into the *frame* window and clears pending. Edge queries read the frame window. Frames
  that skip the update phase (paused, held) latch nothing, so edges wait for the next running
  frame, which is how MCP step works.
- **R2** Semantics. `getKeyDown(k)` is true iff k had ≥ 1 non-repeat press in the window.
  `getKeyUp(k)` is true iff it had ≥ 1 release. `getKey(k)` is the level. A press and release
  inside one window gives Down = true, Up = true and Key = false. Key-repeat `systemKeyDown(...,
  repeat=true)` is never a press. Mouse buttons, gamepad buttons and touches follow the same rules.
  `anyKeyDown()` covers keys, mouse buttons and gamepad buttons.
- **R3** Fixed phase. Inside `onFixedUpdate` and `Scene::fixedUpdate`, edge queries read a
  separate *fixed* window. Each latch adds its edges to it, and the window is cleared after each
  fixed step that ran. An edge is therefore seen by exactly one fixed step: the first one at or
  after its frame, even across frames that run 0 steps. A frame with 3 steps shows it only in the
  first. Levels are the same in both phases.
- **R4** Mouse delta. `getMouseDelta()` is the sum, over the window, of position changes between
  consecutive `systemMouseMove` events in canvas pixels (the space of `getMousePosition`). The
  first move after `systemMouseEnter` and moves caused by `Engine::setMousePosition` warps are
  excluded. On web the lock path already turns `movementX/Y` into the virtual position, and Windows
  uses `WM_INPUT`, so the delta is raw motion wherever the platform has it. `getMouseScrollDelta()`
  is the per-window sum of wheel offsets. `getMouseScroll()` keeps its old (sticky) behaviour for
  compatibility and is documented as deprecated.
- **R5** Touch. `Touch` gains `phase` (BEGAN, MOVED, STATIONARY, ENDED, CANCELED), `delta`,
  `startPosition` and `time` (seconds since began). `getTouches()` keeps its contents (active
  touches only). `getTouchCount()`/`getTouch(i)` return the frame list, which also holds touches
  that ended or were cancelled in the window. A touch that began and ended in one window appears
  once with phase ENDED and `began = true`. `getTouchDown(pointer)`/`getTouchUp(pointer)` follow R2.
  Mouse-as-touch emulation (`callTouchInMouseEvent`) feeds the same records.
- **R6** Pointer lock. `Input::getPointerLockState()` returns UNLOCKED, PENDING or LOCKED.
  Desktop goes straight to LOCKED on `MouseMode::CAPTURED`. Web stays PENDING until
  `pointerlockchange` reports active, through a new `Engine::systemPointerLockChanged(bool)`. In
  the editor the state is LOCKED while CAPTURED and the Game view has focus (`PlayInput`'s rule).
- **R7** `Input::resetFrameState()` clears levels, windows, deltas and touches. It runs when Play
  starts and stops in the editor (static state survives sessions), and on focus loss on every
  platform. On focus loss the held controls first get release edges, matching `PlayInput::releaseAll`.

### Functional — Input Actions (P4)
- **R8** Asset `*.inputactions`, JSON with `"version": 1`, holding maps → actions → bindings. A
  **map** has `name`, `priority` (int, higher first), `enabled` (initial), `consume` (default
  true), `exclusive` (default false) and `ignoreWhenUIConsumes` (default true). An **action** has
  `name`, `type` (BUTTON | AXIS | VECTOR2), `pressPoint` (0.5), `holdTime` (0 = plain press), and
  `deadzone` (AXIS/VECTOR2, default from Project Settings, 0.15). A **binding** has a stable `id`
  (8 hex chars, generated once), `path`, `scale`, `invert`, `deadzone` (optional override),
  `requiredMods`, `forbiddenMods` and `groups` (control schemes, e.g. "Keyboard&Mouse", "Gamepad",
  "Touch"). A **composite** binding has `composite` set to `axis1d` (negative/positive) or `dpad2d`
  (up/down/left/right), plus `normalize` (default true), and its parts are bindings.
- **R9** Control paths: `keyboard/<key>` (the key names of `send_input`'s `parseKeyName`, shared
  code); `mouse/{left,right,middle,button4..8}`; `mouse/delta/{x,y}`, `mouse/delta` (vec2);
  `mouse/scroll/{x,y}`; `gamepad/{a,b,x,y,...,dpad_left}`; `gamepad/{left_x,...,right_trigger}`;
  `gamepad/{left_stick,right_stick}` (vec2); `touch/primary/{press,position,delta}`;
  `touch/count`; `virtual/<name>` (scalar or vec2 set from script). Triggers are remapped from
  [-1,1] to [0,1] for actions.
- **R10** Evaluation. Actions are evaluated once per latch (R1), right after the raw latch, in map
  priority order (ties: asset order). A BUTTON's value is the max magnitude of its bindings. It is
  *pressed* at value ≥ `pressPoint`. `wasPressedThisFrame`/`wasReleasedThisFrame` also see a
  same-window tap of a bound digital control (R2). AXIS is the sum of its bindings, clamped to
  [-1,1]. VECTOR2 is the sum of its bindings, with a radial deadzone, rescaled, length clamped to 1.
  Delta controls (`mouse/delta`, `scroll`) are not deadzoned and not clamped. In the fixed phase,
  edges follow R3. Values are the frame's.
- **R11** Consumption. When a control actuates a binding in an enabled map with `consume`, it is
  unavailable to bindings in lower-priority maps that window. An enabled `exclusive` map blocks
  every lower map entirely (modal menus). Within one priority level, a binding that requires
  modifiers and is satisfied suppresses bindings of the same key that do not list those modifiers,
  so Ctrl+S does not also fire S. `forbiddenMods` refuses explicitly.
- **R12** UI. When `ignoreWhenUIConsumes` is set and an HTML document or component UI consumed the
  press (`Engine::ignoreEventsHandledByUI` path), it does not count for that map. Raw `Input` edges
  are unaffected (R2), which matches `isKeyPressed` today.
- **R13** Gamepad filter: `map:setGamepad(id)` (-1 = any pad, the default). Any-pad axes take the
  pad with the largest magnitude, and buttons OR across pads.
- **R14** Callbacks: `action.onStarted` (value leaves 0), `onPerformed` (pressed, or hold reached),
  `onCanceled` (released before or after performed). They fire during evaluation, before the fixed
  phase, in priority order. A callback may call `event:consume()` to stop the same control from
  reaching lower maps' callbacks and state this window.
- **R15** Rebinding. `InputActions.startRebind(action, bindingId, opts, onComplete)` waits for the
  next actuated control: a key press, a mouse button, a gamepad button, or an axis past 0.5. Options:
  `groups`, `exclude` (path globs, e.g. `mouse/delta*`), `cancel` (default `keyboard/escape`), and
  `timeout`. While a rebind is active, all maps are suspended. The result is an *override*
  (bindingId → path), not an asset edit. `getConflicts(action, bindingId)` lists bindings in
  enabled maps of the same group with the same path.
- **R16** Persistence. `InputActions.saveOverrides()` writes JSON
  `{version, overrides:{bindingId:path}}` to `UserSettings` key
  `jasperengine.input.<assetGuid>` (S04 GUID; asset path if none). `loadOverrides()` runs when the
  asset loads. Unknown binding ids are kept but ignored (forward safe). `resetOverrides()` clears
  them. This must work on all targets because `UserSettings` is the existing cross-platform store.
- **R17** Injection: `InputActions.setVirtual(name, value|Vector2)` feeds `virtual/<name>` (HTML UI
  on-screen sticks). `InputActions.inject(actionPath, value, frames)` forces an action's value for
  N windows at top priority. MCP uses it (R34).
- **R18** Display: `binding:getDisplayName()` gives "Space", "Ctrl+S", "Left Stick" and so on.
  Gamepad names follow the connected pad (Cross/Circle for PlayStation names from
  `getGamepadName`, else A/B).

### Functional — CharacterController 3D (P2)
- **R19** `CharacterControllerComponent` (serialized, Unity defaults): `center` (0,0,0), `height`
  2, `radius` 0.5, `slopeLimit` 45°, `stepOffset` 0.3, `skinWidth` 0.02, `minMoveDistance` 0.001,
  `mass` 70, `pushStrength` 100 N (Jolt `mMaxStrength`), `pushBodies` true, `movingPlatforms`
  true, `inheritPlatformYaw` true, `detectCollisions` true (creates the inner body),
  `characterCollisions` true, `enhancedEdgeRemoval` false, and `group`/`mask` bits as on
  Rigidbody. The layer is the entity layer. Runtime only (not serialized): `isGrounded`,
  `isOnSteepSlope`, `collisionFlags` (SIDES=1, ABOVE=2, BELOW=4 from the last move), `velocity`
  (actual displacement / dt of the last move), `groundNormal`, `groundEntity`, `groundVelocity`.
- **R20** `move(motion)` is immediate and synchronous (Unity). It sets `velocity = motion/dt` and
  calls `ExtendedUpdate(dt, ...)` with our layer filters and the stair settings from §4. dt is the
  fixed step inside the fixed phase and `Engine::getDeltatime()` otherwise. If dt ≤ 0, dt = 1/60
  and the displacement stays exact. Motion below `minMoveDistance` does nothing. `move` applies no
  gravity. It writes the Transform (world → parent-local) at once and returns `collisionFlags`.
  `simpleMove(speed)` ignores `speed.y`, integrates gravity (`Physics` gravity) on its own vertical
  velocity, which resets when grounded, and returns `isGrounded`.
- **R21** Stairs and slopes. With `stepOffset` > 0, steps up to `stepOffset` are climbed while
  moving horizontally and grounded, and stick-to-floor holds the controller on descending steps of
  the same size. Surfaces steeper than `slopeLimit` are not walkable. `isGrounded` is Jolt
  `OnGround` only. Steep contact sets `isOnSteepSlope` (a documented deviation from Unity, whose
  `isGrounded` is any contact below).
- **R22** Moving platforms. After every Jolt step, `PhysicsSystem` runs a post-step pass for each
  controller. If `movingPlatforms` is set and the controller is grounded on a body with non-zero
  velocity, it is carried by `groundVelocity × dt` (an `ExtendedUpdate` that only carries) and, with
  `inheritPlatformYaw`, rotated about world up by the platform's ω_y·dt. The pass also refreshes
  the ground state, so `isGrounded` follows a platform that drops away. This happens whether or
  not the script called `move` that step. `move`'s own displacement never includes platform
  velocity.
- **R23** Pushing and hits. A `CharacterContactListener`: (a) with `pushBodies` false, it sets
  `mCanReceiveImpulses = false` so dynamic bodies are not pushed; (b) it records every contact of a
  move as a `ControllerColliderHit` {entity, point, normal, moveDirection, moveLength} and
  delivers it to script `onControllerColliderHit(hit)` and to C++
  `PhysicsSystem::onControllerColliderHit` after the move; (c) it filters with the Layer Collision
  Matrix + group/mask exactly like `JoltObjectLayerPairFilter`. Controllers collide with each other
  through one `CharacterVsCharacterCollisionSimple` per PhysicsSystem when `characterCollisions` is
  set.
- **R24** Inner body. With `detectCollisions`, the inner body (`mInnerBodyShape` = the capsule,
  kinematic) has user data = entity, so `Physics.raycast` returns the controller entity and dynamic
  bodies collide with it. Sensors are created with `mCollideKinematicVsNonDynamic = true` so
  triggers see controllers. The inner body's non-sensor contacts are **not** reported through
  `onContactAdded3D/Persisted/Removed` (no `RigidbodyComponent` there). Sensor contacts are
  reported, and their `Rigidbody` wrapper for the controller side reports `isValid() == false`
  instead of crashing.
- **R25** Lifecycle. The controller is created when the component is added or enabled and the
  entity is active, and destroyed on disable, inactive or remove. A Transform write that the
  controller did not make (a teleport) calls `SetPosition` before the next move (no Unity "disable
  first" trap). Changing `height`, `radius` or `center` at runtime goes through `trySetSize`, which
  uses `SetShape` with a penetration check and returns false (keeping the old shape) when blocked,
  for safe crouch and stand. The Jolt world steps when controllers exist even with no rigidbodies
  (lifting the gate at `PhysicsSystem.cpp:3127`).
- **R26** Coexistence. A `RigidbodyComponent` and a `CharacterControllerComponent` on one entity
  are refused. The Add Component dialog and MCP `add_component` reject the second with a message.
  A scene that has both logs one error and disables the controller.
- **R27** Root motion (S21 D9 hook): `PhysicsSystem::setRootMotion(entity, worldVelocity,
  yawRate)`. S21's Animator calls it in Finalize each update with `Δ/dt` (S21 already drops deltas
  above 50 m per frame). The controller stores the latest value. Each following fixed step it runs
  `move(worldVelocity × fixedDt)` with collisions and rotates the Transform by `yawRate × fixedDt`.
  A frame with 0 steps keeps the value, and the next publish replaces it. A non-finite value is
  dropped with a warning once per entity. Script `move` calls in the same step add to it. Without
  a controller S21 writes the Transform itself, so S22 is not involved.

### Functional — water and buoyancy (P3)
- **R28** CPU mirror `engine/core/util/WaterWaves.h`: `WaterWaveSample waterWaves(Vector2 xz,
  float time, Vector2 dir, float height, float waveLength, float speed)` returns {height,
  slope.xy, dh/dt}. It is a line-for-line float32 translation of `water.glsl:15-28`: the same
  constants, the same operation order, `fract(x) = x - floor(x)`, and the same `max` guards. The
  file header names `water.glsl` as its twin. A test pins the GLSL function text hash (AC10), so
  changing either file without the other fails CI.
- **R29** Shader time. `Engine::getShaderTime()` (float) is latched once per frame at the top of
  `systemDraw`. `RenderSystem::drawFramePasses` uses it instead of calling `getSystemTime()`
  (`RenderSystem.cpp:13280`). Buoyancy and `Water.sample` use the same value, so every fixed step of
  frame N and the draw of frame N agree. When `fixedDeltatime > 0`, shader time advances by
  `fixedDeltatime` per frame, starting from the value at the moment fixed mode was enabled. This
  makes `play fixed_fps` captures deterministic.
- **R30** `WaterVolumeComponent`: `size` (local box: XZ extent, Y = depth below the surface),
  `surfaceOffset` (local Y of the surface), `infinite` (ocean: unbounded XZ and depth), `density`
  1000 kg/m³, `flow` (world m/s), `linearDrag` 1, `angularDrag` 0.5, and `waves` (FROM_MATERIAL |
  NONE). FROM_MATERIAL reads the first submesh material of the entity's `MeshComponent`: if its
  shader is `library:water` and `WAVES` is enabled, it reads `waveDirection`, `waveHeight`,
  `waveLength` and `waveSpeed` from `shaderParams`, with manifest defaults for missing values. The
  material is the single source of truth, and volumes do not copy wave parameters. The surface
  height at (x,z) is the entity's world Y at `surfaceOffset` plus the wave height. Overlapping
  volumes: the highest surface wins.
- **R31** `Water.sample(point)` returns {inWater, surfaceHeight, normal, flowVelocity (flow +
  (0, dh/dt, 0)), volume entity}, plus `Water.sampleHeight(x, z)`. Both are C++/Lua and usable in
  edit mode.
- **R32** `BuoyancyComponent` (on an entity with a dynamic `RigidbodyComponent`; otherwise
  inactive with an inspector warning): `columns` (AUTO_2x2 | AUTO_3x3 | CUSTOM list of local
  points), `volumeScale` 1, and `dragScale` 1. AUTO places n×n columns on the bottom face of the
  union local AABB of the colliders. Each column has world height H (AABB height × world scale Y)
  and volume V_i = (Σ collider `Shape::GetVolume()` × volumeScale)/n. In each fixed step, *before*
  `world3D.Update`, each column computes d_i = clamp(h_water − y_bottom_i, 0, H) and applies
  F_i = ρ_w·|g|·V_i·d_i/H upward at the bottom point + up·d_i/2. It also applies linear drag
  −linearDrag·dragScale·ρ_w·V_i·(d_i/H)·(v_point − v_water) at the same point, and angular drag
  −angularDrag·dragScale·ρ_w·V_sub·ω. A body with no submerged column gets nothing. Sleeping bodies
  in flat water (waveHeight 0, flow 0) are not woken. For an upright box this gives the exact
  Archimedes equilibrium d/H = ρ_body/ρ_water.

### Platform & backends
- **R33** Everything runs on all targets. The wave mirror and buoyancy are CPU-only, with no new
  shader features. Input edges hook only `Engine::system*`, so every platform and `PlayInput` get
  them for free. Web pointer lock reports R6. Android and iOS feed touches and their game
  controllers (`JasperEngineGameController.mm`) through the existing paths.
- **R34** MCP `send_input`: `hold_frames: 0` becomes legal for key, mouse and gamepad presses
  (down and up in the same window). A new event `{type:"input_action", name:"Map/Action",
  action:"press|down|up|value", value, hold_frames}` uses R17. `PlayInput` and MCP injection both
  end in `Engine::system*` and never write `Input` directly.

### Performance (budgets; reference devices: desktop = Ryzen 5 5600 class, mobile = Snapdragon 7 Gen 1 class, web = mobile × 1.5)
- **R35** Input latch with no events ≤ 2 µs. Latch plus evaluation of 100 actions / 300 bindings is
  ≤ 50 µs on mobile. Steady-state frames make no heap allocations (dirty-index lists and
  fixed-size arrays; the `std::map` level state becomes a view over arrays).
- **R36** One controller `move` ≤ 0.1 ms desktop / 0.3 ms mobile in a 1k-static-body scene. Ten
  controllers plus the post-step pass ≤ 1 ms/step on mobile.
- **R37** Buoyancy: 100 bodies × 4 columns ≤ 0.1 ms desktop / 0.3 ms mobile per step. With no
  Buoyancy or WaterVolume components it does no work (an empty-array early-out).

### Editor UX (human)
- **R38** Input Actions window (Window > Input Actions; double-click a `.inputactions` asset): three
  panes, Action Maps | Actions | Properties (Unity layout). Add, rename, delete, duplicate and
  drag-reorder maps, actions, bindings and composites. Binding path picker with a **Listen** button
  (the next editor key, mouse or gamepad press). Group filter. Every edit is undoable with a named
  command (S24). Changes during Play apply live. The asset saves through S03 atomic writes.
  A **Debug** tab during Play shows per action: value, phase, consumed-by, and the raw edges this
  frame.
- **R39** Project Settings > Input: default actions asset (loaded at Play and at game start),
  default deadzone, and a "Reset rebinding overrides" button (editor `UserSettings`).
- **R40** Inspectors. CharacterController: a capsule gizmo with the step-offset band and a slope
  cone, runtime fields read-only in Play, and a refusal banner next to a Rigidbody. WaterVolume: a
  box gizmo with the surface plane, "Fit to Mesh", and a warning when the mesh vertex spacing is
  greater than `waveLength·0.6/8` (the drawn surface would deviate from the physics surface by more
  than ~8% of the height). Buoyancy: column gizmos, effective density (mass/volume) with a warning
  at ≥ ρ_water ("will sink or hover"), and a warning when no WaterVolume is in the scene.

### Agent UX (MCP, Lua, docs)
- **R41** Lua (camelCase, Unity names): `Input.getKey/getKeyDown/getKeyUp`,
  `getMouseButton/getMouseButtonDown/getMouseButtonUp`, `getMouseDelta`, `getMouseScrollDelta`,
  `getGamepadButtonDown/Up(id, b)`, `anyKeyDown`, `getTouchCount/getTouch/getTouchDown/getTouchUp`,
  `getPointerLockState`, plus `Input.TOUCH_*` and `POINTER_*` constants. Existing names stay.
  `InputActions` and `InputAction` as in §6. `CharacterController` (wrapper):
  `move`, `simpleMove`, `trySetSize`, `isGrounded`, and so on. `WaterVolume`, `Buoyancy` and
  `Water.sample`. All of it goes into the S23 JSON API description and the Lua stubs.
- **R42** MCP tools: `get_input_state` {fields?: raw|actions|touches|gamepads} returns levels, this
  window's edges, deltas, lock state, enabled maps with priority, action values, phases and
  consumers. `get_input_actions` {path} and `edit_input_actions` {path, ops[]} (undoable ops:
  add_map, add_action, add_binding, add_composite, set, remove, rename, move).
  `sample_water` {points: [[x,y,z]...]}. `get_runtime_entity` exposes the controller's runtime
  fields. The tool docs and the guide (`docs/mcp/guide/input.md` new, `physics.md` updated) tell
  agents to verify taps with `hold_frames: 0`.

### Data & compatibility
- **R43** New components get defaults for every field, are registered in `EntityRegistry`, and
  are encoded in `editor/Stream.cpp` and `editor/Factory.cpp` (exported code). Old scenes load
  unchanged. No scene version bump is needed (purely additive keys, per S03 rules).
- **R44** `.inputactions` carries `"version": 1`. Unknown keys are preserved on save (S03
  unknown-data rule). The S04 GUID sidecar applies. The exporter packages the default asset, and
  generated startup code calls `InputActions::loadDefault(path)`.

## 6. Design constraints & decisions

- **D1 Kinematic only, Unity-imperative.** `move` sweeps immediately against the world as of the
  last step. No velocity mailbox and no dynamic motor. Platform carry is the physics system's job
  (R22), so scripts never handle it.
- **D2 2D controller = our own collide-and-slide (P5), not a Box2D upgrade.** Moving Box2D 3.0 →
  3.1 for `b2World_CastMover` changes shape-def, sensor and cast APIs across `PhysicsSystem.cpp`
  (3227 LOC). That is out of proportion. `CharacterController2DComponent` (capsule, in 2D points
  like other 2D props) implements a mover-style plane solver on `b2World_CastCapsule` +
  `b2World_OverlapCapsule`, ≤ 4 iterations. It supports slopeLimit, stepOffset (up-cast,
  forward-cast, down-cast), skinWidth, `move`/`simpleMove`, isGrounded, collisionFlags, kinematic
  platform carry (`b2Body_GetLinearVelocity`) and pushing dynamic bodies (impulse
  `pushStrength·dt` along the move). If the stream ever upgrades Box2D, only the solver internals
  are replaced.
- **D3 Edges live in `Input`, actions in `engine/core/input/`.** `Input` stays a static facade.
  New files: `engine/core/input/{InputActions,InputActionAsset,InputControlPath,InputRebind}.{h,cpp}`
  using `engine/libs/json/json.hpp` (the engine has no YAML; JSON is readable on every target).
  The control-name table moves from `PlayTools.cpp` (`parseKeyName`/`keyName`) to
  `engine/core/input/InputControlPath.cpp` and is shared by the runtime, the editor and MCP.
- **D4 One latch point.** `Input::beginFrame()` and then `InputActions::evaluate()`, both at the
  start of the running update block in `systemDraw`. No other code clears edges. Mid-frame events
  (rare: a Lua call into `Engine::system*`) go to the next window.
- **D5 Water truth = material + shader time.** No duplicated wave parameters (R30) and no
  per-volume time. The CPU mirror is a header with zero dependencies other than `Vector2/3`, so a
  test can evaluate it without a scene.
- **D6 Buoyancy runs in `PhysicsSystem::fixedUpdate`** (a `WaterSystem` helper class owned by
  PhysicsSystem, not a new SubSystem), just before `world3D.Update`. Character controllers get no
  automatic buoyancy (`Water.sample` lets scripts swim).

API sketch (C++):
```cpp
// engine/core/Input.h (additions)
static bool getKeyDown(int key);  static bool getKeyUp(int key);
static bool getMouseButtonDown(int b);  static bool getMouseButtonUp(int b);
static bool getGamepadButtonDown(int id, int b);  static bool getGamepadButtonUp(int id, int b);
static bool anyKeyDown();
static Vector2 getMouseDelta();  static Vector2 getMouseScrollDelta();
static size_t getTouchCount();  static Touch getTouch(size_t i);
static bool getTouchDown(int pointer);  static bool getTouchUp(int pointer);
static PointerLockState getPointerLockState();
static void beginFrame(); static void endFixedStep(); static void resetFrameState(); // Engine only

// engine/core/input/InputActions.h
class InputAction { public:
  float readValue() const; Vector2 readVector2() const; bool isPressed() const;
  bool wasPressedThisFrame() const; bool wasReleasedThisFrame() const; float getHeldTime() const;
  FunctionSubscribe<void(InputActionEvent&)> onStarted, onPerformed, onCanceled; };
class InputActions { public:
  static bool load(const std::string& path); static bool loadDefault(const std::string& path);
  static InputAction* get(const std::string& mapSlashAction);
  static void enableMap(const std::string&, bool); static void setMapPriority(const std::string&, int);
  static void setGamepad(const std::string& map, int gamepadId);
  static void setVirtual(const std::string& name, Vector2 v);
  static void inject(const std::string& action, Vector2 v, int windows);
  static bool startRebind(const std::string& action, const std::string& bindingId,
                          const RebindOptions&, std::function<void(const RebindResult&)>);
  static void cancelRebind(); static bool saveOverrides(); static void resetOverrides();
  static bool isControlBound(const std::string& path); };   // for S25 routing

// engine/core/subsystem/PhysicsSystem.h (additions)
CollisionFlags moveCharacter(Entity, const Vector3& motion);
bool simpleMoveCharacter(Entity, const Vector3& speed);
bool trySetCharacterSize(Entity, float height, float radius, const Vector3& center);
void setRootMotion(Entity, const Vector3& worldVelocity, float yawRateRad);  // S21 D9
FunctionSubscribe<void(Entity, ControllerColliderHit)> onControllerColliderHit;
WaterSample sampleWater(const Vector3& point) const;
```
Lua:
```lua
if Input.getKeyDown(Input.KEY_SPACE) then jump() end
local move = InputActions.get("Gameplay/Move"):readVector2()
local cc = self.entity:getCharacterController()
cc:simpleMove(Vector3(move.x, 0, move.y) * speed)
InputActions.get("Gameplay/Jump").onPerformed = function(e) vy = 5 end
InputActions.startRebind("Gameplay/Jump", bindingId, {groups={"Gamepad"}}, function(r) InputActions.saveOverrides() end)
local w = Water.sample(self.entity.position)
```
Asset excerpt:
```json
{"version":1,"maps":[{"name":"Gameplay","priority":0,"actions":[
 {"name":"Move","type":"VECTOR2","bindings":[
   {"id":"3f9a12c0","composite":"dpad2d","parts":{"up":{"path":"keyboard/w"},"down":{"path":"keyboard/s"},
    "left":{"path":"keyboard/a"},"right":{"path":"keyboard/d"}},"groups":["Keyboard&Mouse"]},
   {"id":"8b01aa47","path":"gamepad/left_stick","groups":["Gamepad"]}]},
 {"name":"Jump","type":"BUTTON","bindings":[{"id":"c2d4e611","path":"keyboard/space"},{"id":"77e0b9d2","path":"gamepad/a"}]}]},
 {"name":"Menu","priority":100,"enabled":false,"exclusive":true,"actions":[]}]}
```

## 7. Acceptance criteria

Unit tests are gtest in `engine/tests/core/input/`, `engine/tests/core/physics/` and
`engine/tests/core/util/` (S02 layout, no GPU unless marked).
- **AC1** `InputEdgeTest.{PressReleaseSameWindow, HeldAcrossWindows, RepeatIsNotPress,
  PausedFramesKeepPending, ResetOnPlayStart, FocusLossReleases}`: synthetic `Engine::system*`
  calls + `beginFrame()`. For a tap, Down = Up = true and Key = false, then false/false/false next
  window (R1, R2, R7).
- **AC2** `InputEdgeTest.FixedPhaseExactlyOnce`: an edge latched in a frame with 0 steps is seen
  by the first step of the next frame, and in a 3-step frame only by step 1 (R3).
- **AC3** `InputEdgeTest.{MouseDeltaSumsMoves, MouseDeltaExcludesWarpAndEnter,
  ScrollDeltaPerWindow, TouchBeganEndedSameWindow, GamepadEdgesPerPad}` (R4, R5).
- **AC4** `InputActionsTest.{Dpad2dNormalized, RadialDeadzone, PriorityConsumption,
  ExclusiveBlocksLower, ModifierBindingSuppressesPlain, ForbiddenMods, UIConsumedIgnored,
  GamepadFilter, SameWindowTapPerformsAndCancels, CallbackConsume}` (R8–R14).
- **AC5** `InputActionsTest.{RebindCapturesNextControl, RebindCancelKey, OverridesRoundTrip,
  BindingIdStableAcrossReorder, UnknownOverrideIgnored, UnknownKeysPreservedOnSave}` with a
  `UserSettings` fake (R15, R16, R44).
- **AC6** `CharacterControllerTest.Stairs`: static stairs with 0.2 m risers and 0.3 m treads, 6
  steps; controller r 0.4, h 1.8, stepOffset 0.3; `simpleMove` 2 m/s along +X for 4 s at 60 Hz.
  The final foot Y is 1.2 ± 0.05 and `isGrounded` is true. With stepOffset 0.1 the X stays
  < first riser X + 0.05. It also walks down the same stairs without leaving the ground for more
  than 2 consecutive steps (stick-to-floor) (R20, R21).
- **AC7** `CharacterControllerTest.Slopes`: 30° ramp with slopeLimit 45 climbs (ΔY > 1 m in 3 s).
  50° ramp: ΔY < 0.05 m, `isOnSteepSlope` true and `isGrounded` false.
  `CharacterControllerTest.CollisionFlags`: walking into a wall sets SIDES and a low ceiling sets
  ABOVE (R19, R21).
- **AC8** `CharacterControllerTest.{MovingPlatform, RotatingPlatform, PlatformDropsAway}`: a
  kinematic platform at 1 m/s for 2 s carries an idle controller 2.0 ± 0.05 m with no `move` calls.
  90°/s for 1 s changes the controller yaw by 90 ± 3°. A platform removed downward makes
  `isGrounded` false within 2 steps (R22).
- **AC9** `CharacterControllerTest.{PushesDynamicBox, NoPushWhenDisabled, RaycastHitsInnerBody,
  TriggerSeesController, HitCallbackOncePerContact, CharacterVsCharacterBlocks, TeleportSyncs,
  CrouchBlockedUnderCeiling, RefusedWithRigidbody, ScenesWithoutRigidbodiesStep,
  RootMotionVelocityConsumedInFixedSteps}` (1 m/s root motion for 1 s moves 1.0 ± 0.03 m and stops
  at a wall). Pushing: a
  10 kg box moves > 0.5 m in 1 s at 2 m/s, and < 0.01 m with `pushBodies` false (R23–R27).
- **AC10** `WaterWavesTest.MatchesReferenceTable`: 64 (xz, t, params) samples against a checked-in
  table, |Δh| ≤ 1e-5. `WaterWavesTest.GlslTwinPinned`: the SHA-256 of `waterWaves` in
  `engine/shaderlib/library/water.glsl` equals the constant in the test. The failure message says
  to update `WaterWaves.h` (R28).
- **AC11** GPU mirror (golden-class, desktop GL + Vulkan in CI): a test-only shader that includes
  `library/water.glsl` writes `waterWaves(...).x` for a 32×32 grid into an RGBA32F target at shader
  time T. The readback matches `WaterWaves.h` at the same T within 1e-3 × waveHeight + 1e-4 (R28,
  R29).
- **AC12** `BuoyancyTest.Equilibrium`: 1 m³ box at ρ 500 in flat water settles to immersion
  0.50 ± 0.02 m with |v_y| < 0.01 m/s after 10 s. ρ 250 gives 0.25 ± 0.02. ρ 1200 is still
  sinking at 5 s (≥ 1 m below the surface). `BuoyancyTest.RidesWaves`: with WAVES on, the body's
  mean height over 10 s is within 0.02 m of the flat equilibrium plus the mean wave height, and
  its vertical position correlates (r > 0.8) with `sampleHeight` at its XZ. `BuoyancyTest.
  NoWorkWhenAbsent`: no WaterVolume/Buoyancy → 0 wave evaluations (R30–R32, R37).
- **AC13** `ShaderTimeTest.{LatchedOncePerFrame, FixedDeltaAdvancesExactly}` (R29).
- **AC14** MCP `tools/mcp/tests/test_input.py`: a Lua script counts `getKeyDown`/`getKeyUp`. A
  `send_input` key with `hold_frames: 0` gives exactly 1/1 and `get_input_state` shows both edges.
  Gamepad and mouse variants. `input_action` press fires `onPerformed` once. With CAPTURED mode,
  a mouse move of dx=40 gives a summed `getMouseDelta().x` of 40 ± 1 (R4, R34, R42).
- **AC15** MCP `test_input_actions.py`: `edit_input_actions` builds the §6 asset, undo/redo
  restores it byte-for-byte, Play + `send_input` W gives `Gameplay/Move` = (0,1), and enabling Menu
  (exclusive) makes it (0,0) (R38, R42).
- **AC16** MCP `test_character_controller.py`: `add_component` CharacterController on an entity
  with a Rigidbody errors. A stairs fixture scene in Play reaches the top, and `get_runtime_entity`
  reports `isGrounded: true`. `test_buoyancy.py`: `sample_water` equals `eval_lua
  Water.sampleHeight` at the same frame within 1e-5 (R26, R31, R42).
- **AC17** Performance benchmarks (S02 bench target, CI-recorded, fail at > 1.5× budget): R35, R36
  and R37 scenarios on the desktop runner. Mobile numbers are recorded manually once per release on
  the reference Android device, and the result is put in `docs/specs/S22-gameplay.md` §9.
- **AC18** Web: the S02 headless-Chrome gate runs a page that requests CAPTURED and asserts
  `getPointerLockState()` is PENDING before a gesture and LOCKED after a synthetic click (CDP
  `Input.dispatchMouseEvent`). Android/iOS: manual check, two-finger tap → two touches with BEGAN
  then ENDED, each `getTouchDown` true once.
- **AC19** P5 `CharacterController2DTest.{Stairs, Slopes, MovingPlatform, Push}`: the same
  thresholds as AC6–AC9, scaled to 2D points.
- **AC20** `tools/smoke-test.sh` and the existing physics/HTML UI gamepad MCP tests stay green.
  `test_html_ui_gamepad.py` proves UI consumption is unchanged.

## 8. Dependencies & conflicts

- Depends on S01, S02 (tests and bench), S03 (atomic writes, unknown-key preservation), S04
  (asset GUIDs, used as the persistence key), S07 (4–5 new component types against the 64-type
  cap; 53 are used today), S08 (if it lands first, the new components use reflection tables
  instead of hand-written Stream/inspector code), and S24 (named undo).
- Blocks or enables: S21 (root motion via `setRootMotion`, R27 ↔ S21 R9/D9), S19 (nav agents may drive a
  CharacterController through `move`), S18 (water fill produces WaterVolume + `library:water`
  entities), S25 (`isControlBound` for game-vs-editor shortcut routing while the Game view has
  focus), S23 (API JSON description entries), and S27 (the movie recorder benefits from R29).
- Merge hotspots: `engine/core/Engine.cpp` (systemDraw and input handlers; also S06, S09, S28),
  `PhysicsSystem.cpp/.h` (S19 navigation source collection, S20 particle collisions),
  `RenderSystem.cpp:13280` (shader time; S10/S12/S15 edit nearby), `editor/Stream.cpp`,
  `editor/Factory.cpp`, `EntityRegistry.cpp` (every component-adding stream: S18, S20, S21),
  `CoreClassesLua.cpp`, `PlayTools.cpp` (`send_input`; also S31), `PlayInput.cpp`, and
  `ProjectSettingsWindow`.

## 9. Risks & defaults

- **Tessellated surface vs analytic height.** The drawn water is linear between vertices, so
  physics uses the exact curve. Default: an inspector warning (R40). No GPU readback.
- **Wall-clock shader time** stays the default (behaviour preserved). Buoyancy is therefore not
  bit-reproducible run to run unless `fixedDeltatime` is set. All substeps of a frame see one wave
  phase, which is acceptable at ≥ 30 fps. Default: accept. Tests use flat water or fixed delta.
- **`fixedDeltatime` now also drives shader time**, which changes animated-shader captures under
  `play fixed_fps`, making them deterministic. Default: accept and note it in the changelog.
- **Inner-body contacts** could reach code that assumes a `RigidbodyComponent`. Default: filter
  them out of the 3D contact events (R24) and audit `Physics.raycast` consumers in the editor.
- **Jolt stair tuning** fails on some capsules with Jolt defaults (OE finding). Default: the §4
  values, and AC6 guards them.
- **`isGrounded` deviates from Unity** on steep slopes. Default: keep ours, document it, and
  expose `isOnSteepSlope`.
- **Fixed-phase action values** for delta controls repeat the frame delta in every step. Default:
  document "read deltas in update".
- **Local multiplayer** is limited to a per-map gamepad filter. Default: enough for 2–4 players by
  duplicating maps. Full device pairing is a later stream.
- **2D controller quality** comes from our own solver. Default: P5 ships only if AC19 passes.
  Otherwise it slips without blocking P1–P4.
- **Mobile benchmark numbers** are not yet measured. Record them here (AC17) when P2/P3 land.

## 10. Deviations

Recorded during implementation (branch `feat/s22-gameplay`).

- **P4 modifiers.** A press counts with the modifiers held when it was made (`pressMods`), so a
  same-window Ctrl+S tap is still Ctrl+S. A control held across windows counts with the modifiers
  held now: releasing Shift while W stays down makes a `forbiddenMods: [shift]` binding of W
  actuate on the next window, and releasing Ctrl ends a held Ctrl+S.
- **P4 send_input.** The `input_action` event's line in the tool description lists its own keys
  only; `hold_frames`, `down`/`up` and `release_all` work as for the other events. The
  description was shortened elsewhere to stay inside the `send_input` budget (1923 characters).
- **P5 2D controller.**
  - It has no Box2D body (D2 lists none). Rays, `Physics2D` queries, sensors and other 2D
    controllers do not see it. There is no `onControllerColliderHit` in 2D, and it stays upright
    on a rotating platform, carried by the velocity where it stands.
  - Walkability is the contact normal's, as in Jolt. A riser counts as a step when it is no higher
    than `stepOffset`, measured by the contact that stopped the walk lying below the bottom it lands
    with. A riser taller than `stepOffset` is not climbed by perching the round bottom on its edge.
  - A step climbed onto a corner lands up to `radius + skinWidth` further on, as Jolt's
    `mWalkStairsStepForwardTest`. That one frame moves further than the requested motion.
  - Stick-to-floor casts down `stepOffset`. It slides down a nosing that is too steep to stand on,
    and keeps the result only on walkable ground.
  - Pushing skips what it stands on.
  - `GameObject:getCharacterController2D()` adds the component when it is missing, as
    `getRigidbody2D()` does.
  - Add Component, `add_component`, `create_entity`, `edit_colliders` and the add command itself
    refuse a Character Controller 2D next to a Rigidbody 2D.
- **P2 3D controller.**
  - R26 "disables the controller": the controller gives way at runtime and logs once
    (`refusedWithRigidbody`). Its saved `enabled` checkbox is left alone, so removing the Rigidbody
    brings it back.
  - R20 `ExtendedUpdate` is run as its steps (Update, StickToFloor, WalkStairs) with two additions:
    - A stair walk is kept only when what stopped the walk is no higher than `stepOffset` over the
      ground it left and ends below the feet. Without this, Jolt climbs a riser taller than
      `stepOffset` by sliding the round bottom up its edge, or perches on it.
    - When Jolt's StickToFloor lands on a nosing too steep to stand on, a sweep down along it is
      kept only on walkable ground.
    - Stick-to-floor also counts a move that ended supported. The post-step refresh at rest drops
      contacts beyond Jolt's 1 mm tolerance, and a character walking off an edge is past that.
  - The OE stair settings stand (min step forward `max(0.02, r)`, forward test
    `max(0.15, r + 0.05)`), so a climbed step can land up to a radius further on in that frame.
  - Moves skip every inner body (their own and other controllers'); controllers meet only through
    `CharacterVsCharacterCollisionSimple`, so `characterCollisions` off lets two pass through.
  - The post-step pass refreshes contacts only when the ground is not static or has left the world.
  - `GameObject:getCharacterController()` adds the component when it is missing, as `getRigidbody()`.
  - R36 on C (not enforced): one move 0.005 ms; ten controllers + the step 0.60 ms, of which the step
    of the 1k static bodies alone is 0.23 ms and the ten inner bodies 0.27 ms.
