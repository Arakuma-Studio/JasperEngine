# S96 — Physics gaps from the JasperArcade port: vehicles, shape queries, compound bodies, joint ownership, 2D world settings

Arcade engine-gaps program (S92-S98). The source is the "Physics" section of the port's ledger,
`JasperArcade-Ported/docs/port/ENGINE_LIMITATIONS.md` (P1-P20), written against engine commit
`7f48d7e8d` (2026-10-30), plus R13 (ragdoll builder) from its "Rendering" list, which is physics
work. Paths are relative to the repository root; `PORT/` is `JasperArcade-Ported/`. Every claim in
§3 was checked by reading the code at the cited lines on `82ccd7c6a`; "inferred" means from code,
not run; "unverified" means an AC settles it. Ledger ids (P1-P20, R13, E36) are not reused:
requirements are `REQ-n`, acceptance criteria `AC-n`.

## 1. Goal

Most of the port's physics gaps closed after `7f48d7e8d`: the Unity Rigidbody fields, per-collider
triggers, 3D and 2D joints with motors, limits and springs, the CharacterController (3D and 2D),
contact callbacks on the main thread, the Ragdoll wizard and Box2D sub-steps all landed. What is
left is what a Unity user still reaches for and does not find:
- a **WheelCollider** (and a drivetrain beyond Unity's) on Jolt's vehicle constraint;
- the **shape queries** (`SphereCast`, `BoxCast`, `CapsuleCast`, `Overlap*`, `Check*`, the 2D
  ones) and a ray cast with an ignore list and Unity's start-inside rule;
- **compound bodies** from child GameObjects and a way from a concave mesh to a dynamic body;
- `MovePosition` / `MoveRotation`, interpolated;
- joints a script can make and lose without leaking an entity;
- the 2D world's **units, rotation cap and solver tuning** as scene settings, and a fixed-step
  cap that does not bite before `Time.maximumDeltaTime` does.

Every feature gets editor UI with undo, a Lua API, MCP coverage, docs and deterministic tests.

## 2. Scope

**In scope**
- `WheelCollider` component and an optional `VehicleDrivetrain` component (REQ-1..REQ-9).
- 3D and 2D shape casts, overlaps and checks, ray cast options, `RaycastHit` fields, the
  `query_physics` MCP tool (REQ-10..REQ-17).
- Child colliders joining an ancestor's body, mesh-to-convex decomposition, `movePosition` /
  `moveRotation`, capsules with no cylinder part (REQ-18..REQ-24).
- Joint ownership from Lua, `addJoint2D` / `addJoint3D`, a runtime ragdoll builder
  (REQ-25..REQ-29).
- 2D points per metre, 2D max rotation per step and fast-rotating bodies, Box2D and Jolt world
  tuning, the fixed-step cap (REQ-30..REQ-37).

**Out of scope**
- The CharacterController (P1): done (§3.1). Its remaining differences from Unity are documented
  ones (`isGrounded` is Jolt's OnGround, `docs/mcp/guide/physics.md:531-533`).
- The fixed timestep as a project or scene setting: ledger S14 (`Engine.updateTime` global), owned
  by S95. S96 only changes the per-frame step cap (REQ-36, REQ-37).
- Port-side converter fixes (`PORT/tools/unity2je`, `PORT/tools/gen`): they consume REQ-18 and
  REQ-33 but are game-side.
- Tracked vehicles and motorcycles (Jolt's `TrackedVehicleController`, `MotorcycleController`): a
  follow-up; the drivetrain covers wheeled cars and trucks.
- Kinematic or dynamic concave meshes: Jolt 5.2 cannot move a `MeshShape`
  (`engine/libs/joltphysics/Jolt/Physics/Collision/Shape/MeshShape.h:83`, `MustBeStatic()`
  returns true). REQ-20 turns one into convex hulls instead.
- Nothing on the do-not-copy list (`docs/specs/README.md`) is involved. OpenEngine code is read
  for one idea (§4); none is copied.

## 3. Current state and triage

### 3.1 Triage

| Ledger | Verdict | Evidence (current code) | What S96 does |
|---|---|---|---|
| P1 CharacterController | FIXED | `CharacterControllerComponent` on Jolt `CharacterVirtual` (`engine/core/component/CharacterControllerComponent.h:15-58`): `move`, `simpleMove`, `isGrounded`, `slopeLimit`, `stepOffset`, `skinWidth`, `minMoveDistance`, pushing, platforms, `onControllerColliderHit` (`engine/core/object/physics/ControllerColliderHit.h:16-30`, `docs/mcp/guide/lua-api.md:2960`); 2D twin (`CharacterController2DComponent.h`). Commits `fbcd415ec`..`2042c9afc` (S22 P2, P5); tests `engine/tests/core/physics/CharacterControllerTest.cpp`, `tools/mcp/tests/test_character_controller.py` | nothing |
| P2 WheelCollider / vehicle | MISSING | No vehicle code in `engine/core` or `editor` (grep `Vehicle`, `WheelCollider`: only comments). Jolt 5.2 (`engine/libs/joltphysics/Jolt/Core/Core.h:8-10`) ships `VehicleConstraint`, `WheeledVehicleController` and compiles them (`engine/libs/joltphysics/CMakeLists.txt:379-400`) | REQ-1..REQ-9 |
| P3 contact callbacks on job threads | FIXED | Per-object and global 3D/2D events delivered on the main thread after the step (`engine/core/subsystem/PhysicsSystemContacts.cpp:6`, `docs/mcp/guide/lua-api.md:109-111, 311`). Commits `e1c946849`, `2d2b47d22` | nothing |
| P4 drag; sphere cast / overlap / check sphere | PARTIAL | Drag done: `linearDamping`, `angularDamping` (`engine/core/component/RigidbodyComponent.h:145-146`, Lua `GameObjectClassesLua.cpp:5399`; 2D `Rigidbody2DComponent.h:251-252`). Queries missing: Lua `Physics` has only `raycast` / `raycastAll` (`engine/core/script/binding/CoreClassesLua.cpp:2092-2110`), `Physics2D` the same (`:2113-2128`); C++ `Physics` likewise (`engine/core/registry/PhysicsLayers.h:93-101`). Jolt `NarrowPhaseQuery::CastShape` / `CollideShape` (`Jolt/Physics/Collision/NarrowPhaseQuery.h:53, 64`) and Box2D `b2World_Cast*` / `b2World_Overlap*` (`engine/libs/box2d/include/box2d/box2d.h:54-103`) are unused | REQ-10..REQ-17 |
| P5 mesh colliders static only; child colliders become separate static bodies | PARTIAL | A body's `colliders` list with per-collider pose already becomes one Jolt compound (`PhysicsSystem.h:336-342`); `CONVEX_HULL` from the entity mesh works on dynamic bodies (`PhysicsSystem.cpp:1955-1960`). Missing: colliders on child GameObjects cannot join an ancestor's body (a child body only rides a dynamic parent kinematically, `PhysicsSystem.cpp:4660-4664`, and collides with it); a dynamic `MESH` collider is refused with "use a convex hull or primitives" (`PhysicsSystem.cpp:2695`) and nothing makes the hulls. V-HACD exists only for Physics Placement (`engine/core/physics/ConvexDecomposition.h:3-11`) | REQ-18..REQ-20 (the converter's merge is game-side) |
| P6 hinge motors / limits (3D) | FIXED | `Joint3DComponent` `useLimits`, `useMotor`, `useSpring`, friction, breaking, SIXDOF drives (`engine/core/component/Joint3DComponent.h:120-178`), Lua `GameObjectClassesLua.cpp:5517-5523`. Commits `29edb33c8`, `eb8f3a2f5` | nothing |
| P7 raycast ignore list; start inside hits at 0 | PARTIAL | The ignore list exists on the old `Ray:intersects(scene, filter, {entities})` (`engine/core/math/Ray.h:83-86`, already at `7f48d7e8d`) but not on the Unity-style `Physics.raycast`, which passes none (`engine/core/registry/PhysicsLayers.cpp:238-250`). 3D casts use Jolt's defaults (`engine/core/math/Ray.cpp:672, 682`): convex shapes are solid, so a ray from inside hits at fraction 0, where Unity reports nothing (inferred, AC-12 settles it). 2D rays from inside a polygon report nothing (`engine/libs/box2d/src/geometry.c:789-848`: `index` stays -1), which is Unity's `queriesStartInColliders = false`, not its default (inferred). No trigger option on either | REQ-12..REQ-14 |
| P8 interpolated teleports / move-to-pose | PARTIAL | A kinematic body follows its Transform with `MoveKinematic` over the frame's steps (`RigidbodyComponent.h:180-196`, `docs/mcp/guide/physics.md:84-99`). `Rigidbody:setPosition` / `setRotation` are teleports that reset the interpolation (`engine/core/object/physics/Rigidbody.cpp:868-900`: `previousPosition = getPosition()`). No `movePosition` / `moveRotation` in 3D or 2D (grep: none) | REQ-21, REQ-22 |
| P9 per-collider trigger (3D) | FIXED | `Collider3D::isTrigger` (`RigidbodyComponent.h:95-97`), its own sensor body when mixed (`:119-121`), Lua `setColliderTrigger` (`GameObjectClassesLua.cpp:5350`). Commit `29edb33c8` | nothing |
| P10 linear damping exposed | FIXED | as P4's drag (`RigidbodyComponent.h:144-146`). Commit `29edb33c8` | nothing |
| P11 kart over a wall after the bridge | GAME-SIDE | The ledger itself says the shipped Unity geometry and speeds do it, not the engine | nothing |
| P12 Joint2D spring / motor / limits | FIXED | `Joint2DComponent` `enableSpring`, `frequency`, `dampingRatio`, `useMotor`, `motorSpeed`, `maxMotorTorque`, `useLimits`... (`engine/core/component/Joint2DComponent.h:82-99`), live from Lua (`docs/mcp/guide/lua-api.md:2787-2810`), MCP `set_joint` `motor`/`spring` (`editor/mcp/tools/WorldTools.cpp:6597-6628`). Commit `2c3bb2248` | nothing (`PORT/scripts/h/HCar.lua:11-14` can drop its force pair) |
| P13 2D units per metre only from Lua, not saved | MISSING | `pointsToMeterScale2D = 64.0` set in the constructor (`engine/core/subsystem/PhysicsSystem.cpp:307`); the setter only stores the number (`:541-543`), so bodies made before keep the old scale; no `SceneSettings` field (`engine/core/SceneSettings.h:490-497`), no Stream, Inspector or MCP entry. The port sets it from Lua before any body (`PORT/scripts/h/HGame.lua:27-29`) | REQ-30, REQ-31 |
| P14 Box2D sub-steps hard-coded | FIXED | Scene setting `physics2DSubSteps` 1..16 (`SceneSettings.h:496-497`, `PhysicsSystem.cpp:421-426, 4280`, Stream `editor/Stream.cpp:5103-5104, 5297`, Inspector `editor/window/InspectorWindow.cpp:14071`, MCP `editor/mcp/tools/SceneTools.cpp:851`). Commit `2c3bb2248` | nothing |
| P15 chains 16 points, 10 colliders per body | FIXED | `std::vector<Collider2D> colliders` and `std::vector<Vector2> points` (`Rigidbody2DComponent.h:66, 238`) replaced the `HybridArray<..., MAX_COLLIDERS_2D>` of `7f48d7e8d`. Commit `2c3bb2248` | nothing |
| P16 Box2D caps angular speed at 0.25 pi per step | MISSING | `B2_MAX_ROTATION` is a compile-time constant (`engine/libs/box2d/src/constants.h:30`) used per step (`engine/libs/box2d/src/solver.c:70, 112`). Box2D's per-body `allowFastRotation` (`include/box2d/types.h:228-230`) is not exposed (grep: no use in `engine/core`) | REQ-32, REQ-33 |
| P17 2D world softness and restitution threshold | MISSING | The world is made from `b2DefaultWorldDef()` with only gravity set (`PhysicsSystem.cpp:384-386`); `b2World_SetContactTuning`, `SetJointTuning`, `SetRestitutionThreshold`, `SetMaximumLinearSpeed`, `EnableWarmStarting` (`box2d.h:126, 172-189`) are never called. The 3D world never sets `JPH::PhysicsSettings` either (grep: none) | REQ-34, REQ-35 |
| P18 100 fixed steps per frame cap | PARTIAL | `Time.maximumDeltaTime` exists (`engine/core/script/binding/CoroutineClassesLua.cpp:84`, default 0.25 s, `engine/core/Engine.cpp:195`), but the loop also stops at a hard `MAX_UPDATES_PER_FRAME = 100` (`Engine.cpp:2421, 2631`) and warns on every frame that hits it (`:2662-2666`). At 400 Hz a 0.25 s frame is exactly 100 steps | REQ-36, REQ-37 |
| P19 60 Hz vs 50 Hz per-step drag | GAME-SIDE | The game's own per-step math; the engine part (a project fixed step) is ledger S14, owned by S95 | nothing |
| P20 Lua `Joint2D` owns a hidden entity; log spam after its bodies go | PARTIAL | Fixed: `joint.entity` and `entityOwned` are readable (`GameObjectClassesLua.cpp:1275-1277`), `GameObject.destroy(joint)` takes any handle (`:686-698`, `:458-465`), the "Cannot create joint" error is logged once until the joint is made (`PhysicsSystem.cpp:3500-3515`, commit `f0ed35ef5`). Left: `Joint2D(scene)` makes an owned root GameObject (`engine/core/object/physics/Joint2D.cpp:44-46`, `engine/core/object/EntityHandle.cpp:8-13`) that dies on wrapper GC (`:23-26`) and is not under its body, against the convention that a joint lives under bodyA (`docs/mcp/guide/physics.md:144-145, 269-270`); no `go:addJoint2D()`, no `joint.gameObject`, no `destroy()`; same for `Joint3D(scene)` (`GameObjectClassesLua.cpp:5456-5457`) | REQ-25..REQ-28 |
| R13 ragdoll builder on model bones | FIXED (one gap) | `RagdollBuilder` (Unity's wizard, bones guessed from the humanoid map or names: `engine/core/physics/RagdollBuilder.h:4-24`, `RagdollBuilder.cpp:175`), the `Ragdoll` component and Lua class (`GameObjectClassesLua.cpp:5570`), the editor wizard and MCP `create_ragdoll` (`WorldTools.cpp:2068, 6644-6657`). Commits `29edb33c8`, `61508ba58`. Left: no runtime builder, so a model spawned in Play (the port's B) cannot get one | REQ-29 (default owner S96; S94 may keep R13 instead) |
| E36 (S98's) capsule half height 0 refused | MISSING, engine part | `edit_colliders` refuses `half_height` 0 (`WorldTools.cpp:1720-1724`); the shape builder would fail anyway: Jolt rejects a capsule with no cylinder (`Jolt/Physics/Collision/Shape/CapsuleShape.cpp:76-78`), and `JoltShapeBuilder.cpp:366-370` returns NULL | REQ-24 (S96 takes the whole item; S98 drops it, §8) |

Counts over P1-P20: FIXED 8 (P1, P3, P6, P9, P10, P12, P14, P15), PARTIAL 6 (P4, P5, P7, P8,
P18, P20), MISSING 4 (P2, P13, P16, P17), GAME-SIDE 2 (P11, P19). Plus R13 (FIXED with one gap)
and E36 (MISSING, engine part).

### 3.2 What exists that S96 builds on

- **Layout.** `PhysicsSystem` (`engine/core/subsystem/PhysicsSystem.cpp`, 4778 lines) runs both
  worlds in `fixedUpdate` (`:4183`); the 3D step is bracketed by `characters3D->preStep()` and
  `postStep()` (`:4408, 4487`), the pattern REQ-1 follows. Jolt 5.2, Box2D 3.1.0
  (`engine/libs/box2d/README.jasper.md:3`, one local patch ledger there).
- **Queries.** `Ray::cast` (`engine/core/math/Ray.h:98-102`) does the layer, category, mask and
  ignore filtering (`JoltQueryObjectLayerFilter`, `OnlyStaticBodyFilter` in
  `engine/core/util/JoltPhysicsAux.h:377-407`; 2D `Box2DLayers::makeQueryFilter`,
  `Ray.cpp:577-630`). `RayReturn` is `{hit, distance, point, normal, body, colliderIndex}`
  (`Ray.h:28-35`, Lua `MathClassesLua.cpp:1052-1059`): no collider or GameObject handle.
  `ColliderRef` / `ColliderRef2D` exist for callbacks (`engine/core/object/physics/Collision.h:28`,
  `Collision2D.h:27`).
- **Bodies.** Kinematic following and its interpolation state (`RigidbodyComponent.h:180-196`,
  `Rigidbody2DComponent.h:286-312`), `interpolationOf` (`PhysicsSystem.h:426-431`).
- **Joints.** On their own GameObject under bodyA (`set_joint`, Hierarchy); 2D creation retries
  with one log (`PhysicsSystem.cpp:3500-3515`).
- **Editor.** Collider, joint and controller gizmos in `editor/render/SceneRender3D.cpp:644, 996,
  1045`; Inspector sections `editor/window/InspectorPhysics.cpp`, `InspectorPhysics2D.cpp`;
  reflection tables `editor/reflect/components/TransformPhysicsReflect.cpp`, `GameplayReflect.cpp`;
  component ids in `editor/Catalog.h`; scene settings table `editor/mcp/tools/SceneTools.cpp:805-851`.
- **Port shapes.** The kart (`PORT/scripts/c/CCar.lua:64-125, 276-311`) wants rays that ignore its
  own body and hit triggers, and a pose-to-pose move; the hill car (`PORT/scripts/h/HCar.lua`,
  `HGame.lua`) wants metres, Unity's 360 degrees per step and joints it can drop.

## 4. Reference

No reference engine code is copied. What is read:
- **Jolt 5.2 (vendored, MIT).** `Jolt/Physics/Vehicle/VehicleConstraint.h:23-195` (settings,
  `SetVehicleCollisionTester`, step callbacks), `WheeledVehicleController.h:19-37, 89-196`
  (`WheelSettingsWV`, the protected `PostCollide`, `mTireMaxImpulseCallback`),
  `Wheel.h:35-40` (suspension lengths, spring), `Constraints/SpringSettings.h:18, 43-64`
  (`StiffnessAndDamping`), `VehicleCollisionTester.h` (ray, sphere, cylinder testers).
  `NarrowPhaseQuery.h:40-64` (`CollidePoint`, `CollideShape`, `CastShape`),
  `RayCast.h` (`RayCastSettings::mTreatConvexAsSolid`, back-face modes), `PhysicsSettings.h:47-90`.
- **Box2D 3.1.0 (vendored, MIT).** `box2d.h:54-103` (overlaps, casts), `:126-189` (world tuning),
  `types.h:79-140` (`b2WorldDef`), `:228-230` (`allowFastRotation`), `src/solver.c:60-120`.
- **OpenEngine** `Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp:1536-1610`
  (`BoxOverlap`). **Take** the idea: a capped collector deduplicating by body, back faces on for
  overlaps, finite-input checks. **Improve**: per collider (sub-shape) results, our layer table,
  the trigger rule and Lua/MCP surfaces.
- **Unity semantics** (behaviour, not code): `WheelCollider`, `WheelHit`, `WheelFrictionCurve`,
  `JointSpring`; `Physics.SphereCast/BoxCast/CapsuleCast/Overlap*/Check*`,
  `QueryTriggerInteraction`, `Physics.queriesHitTriggers`, `queriesHitBackfaces`; `Physics2D`
  `CircleCast/BoxCast/CapsuleCast/OverlapCircle/OverlapBox/OverlapCapsule/OverlapPoint/OverlapArea`,
  `queriesStartInColliders`, Physics 2D settings (`Max Rotation Speed`, `Bounce Threshold`,
  `Max Linear Correction`); `Rigidbody.MovePosition/MoveRotation/SweepTest`; Time settings
  `Maximum Allowed Timestep`.

## 5. Requirements

### Functional — vehicles (P2)

- **REQ-1 WheelCollider component.** `WheelColliderComponent` on a GameObject below (not on) a
  dynamic Rigidbody's GameObject, Unity's field names and defaults: `center` (0,0,0), `radius`
  0.5 m, `mass` 20 kg, `wheelDampingRate` 0.25, `suspensionDistance` 0.3 m,
  `forceAppPointDistance` 0, `suspensionSpring` {`spring` 35000 N/m, `damper` 4500 N s/m,
  `targetPosition` 0.5}, `forwardFriction` {`extremumSlip` 0.4, `extremumValue` 1,
  `asymptoteSlip` 0.8, `asymptoteValue` 0.5, `stiffness` 1}, `sidewaysFriction` {0.2, 1, 0.5,
  0.75, 1}, plus `enabled`. Runtime inputs `motorTorque` (N m), `brakeTorque` (N m), `steerAngle`
  (the engine's angle unit). Read-outs (not saved): `isGrounded`, `rpm`, `sprungMass`,
  `getGroundHit()` (a `WheelHit`: `point`, `normal`, `forwardDir`, `sidewaysDir`, `force`,
  `forwardSlip`, `sidewaysSlip`, `collider`), `getWorldPose()` (position, rotation of the wheel's
  drawn pose: suspension, steer and spin).
- **REQ-2 One vehicle per body.** The wheels found under a dynamic Rigidbody (nearest Rigidbody
  ancestor; a wheel whose nearest body is not dynamic logs once and does nothing) make one Jolt
  `VehicleConstraint` on it, built at the first step after a wheel or the body changes and torn
  down with them. The wheel pose comes from its GameObject (position + `center`, suspension along
  the body's down axis, forward = the wheel GameObject's forward). The wheel does not collide as a
  collider; its ground contact is a cylinder cast (`VehicleCollisionTesterCastCylinder`) filtered by
  the body's layer through the Layer Collision Matrix, and the body's own colliders and the
  vehicle's other parts are never hit.
- **REQ-3 Unity drive model.** Without a `VehicleDrivetrain` (REQ-5) each wheel is driven by its
  own `motorTorque` and `brakeTorque` and steered by `steerAngle`, Unity's model. The controller is
  an engine class `jasperengine::WheelColliderController` deriving from
  `JPH::WheeledVehicleController` (no Jolt patch): it overrides `PostCollide` to integrate each
  wheel's angular velocity from its torques, inertia (`0.5 mass radius²`) and `wheelDampingRate`,
  and sets `mTireMaxImpulseCallback` so the tire force follows the wheel's friction curves
  (REQ-4).
- **REQ-4 Friction and suspension.** A `WheelFrictionCurve` maps to a three-point
  `JPH::LinearCurve` (0,0), (extremumSlip, extremumValue x stiffness), (asymptoteSlip,
  asymptoteValue x stiffness), constant after it; forward slip is Jolt's slip ratio, sideways slip
  its slip angle converted so Unity's default curves give a car that turns and slides like a Unity
  default car (AC-4, not PhysX-identical, D2). `suspensionSpring` uses
  `ESpringMode::StiffnessAndDamping`; `suspensionDistance` is the travel
  (`mSuspensionMinLength` 0, `mSuspensionMaxLength` = distance) and `targetPosition` the rest point
  as a fraction of travel from full droop, through the preload length. The body's `material`
  combines with the ground's as other contacts do (`VehicleConstraint::SetCombineFriction`).
- **REQ-5 VehicleDrivetrain (beyond Unity).** An optional `VehicleDrivetrainComponent` on the
  vehicle's body uses Jolt's full model: engine (`maxTorque`, `minRPM`, `maxRPM`, a torque curve
  as an `AnimationCurve`-like point list), transmission (`auto`/`manual`, gear ratios, reverse
  ratios, `clutchStrength`, shift RPMs), differentials (which wheel pairs, `ratio`,
  `leftRightSplit`, `limitedSlipRatio`), anti-roll bars (wheel pairs, `stiffness`),
  `maxPitchRollAngle`. Its inputs `throttle` (-1..1), `steer` (-1..1), `brake` (0..1),
  `handBrake` (0..1) drive the wheels; each wheel's `maxSteerAngle`, `maxBrakeTorque`,
  `maxHandBrakeTorque` (new WheelCollider fields, Jolt's defaults 70°, 1500, 4000, used only with
  a drivetrain) bound them. With a drivetrain `motorTorque`, `brakeTorque` and `steerAngle` become
  read-outs of what it applied. Read-outs: `rpm`, `gear`, `speed` (m/s along forward).
- **REQ-6 Interpolation and drawing.** The wheel GameObject's Transform is written after each step
  to `getWorldPose()` when the WheelCollider's `driveTransform` field is on (default on: Unity users
  write `GetWorldPose` into a child mesh by hand; ours does it, D3), interpolated with the body.
  The `WheelCollider` GameObject's children (the wheel mesh) follow it.
- **REQ-7 Sleep and activation.** A vehicle with non-zero inputs or a wheel off the ground stays
  awake; at rest with zero inputs it may sleep (Jolt's `AllowSleep`). Changing any input wakes it.
- **REQ-8 Events.** None new: wheel contacts are not body contacts (Unity's WheelCollider sends no
  `OnCollision*`). The body's colliders keep their own callbacks.
- **REQ-9 Moving and resetting.** Teleporting the body (`body.position`, `movePosition`, a Transform
  write) moves the wheels with it; setting `enabled` false on a wheel removes it from the vehicle at
  the next step (the constraint is rebuilt); removing the last wheel removes the constraint.

### Functional — queries (P4, P7)

- **REQ-10 3D shape queries (C++ and Lua).** On `Physics` (`PhysicsLayers.h`), all against the
  scene's 3D world, layer-masked like `raycast` (default `DEFAULT_RAYCAST_LAYERS`):
  - casts returning the nearest hit (a `RaycastHit`, REQ-13) or `nil`:
    `sphereCast(scene, origin, radius, direction, maxDistance [, layerMask [, options]])`,
    `boxCast(scene, center, halfExtents, direction, orientation, maxDistance [, layerMask [, options]])`,
    `capsuleCast(scene, point1, point2, radius, direction, maxDistance [, layerMask [, options]])`,
    and `...All` versions returning every hit, nearest first, one per collider;
  - overlaps returning an array of `Collider` (`ColliderRef`) without duplicates:
    `overlapSphere(scene, center, radius [, layerMask [, options]])`,
    `overlapBox(scene, center, halfExtents, orientation [, ...])`,
    `overlapCapsule(scene, point1, point2, radius [, ...])`;
  - checks returning a boolean, early out at the first hit: `checkSphere`, `checkBox`,
    `checkCapsule` with the overlap arguments;
  - `closestPoint(scene, point, collider)` (Unity `Physics.ClosestPoint`) and
    `Rigidbody:sweepTest(direction, maxDistance [, options])` / `sweepTestAll` casting the body's
    own colliders (Unity `Rigidbody.SweepTest`), never hitting the body itself.
  A cast that starts overlapping reports the hit at distance 0 with `point` = origin and
  `normal` = -direction (Unity). Zero or non-finite sizes and directions raise a Lua error naming
  the argument.
- **REQ-11 2D shape queries.** On `Physics2D`, in points: `circleCast`, `boxCast` (size, angle),
  `capsuleCast` (size, `CapsuleDirection2D.VERTICAL`/`HORIZONTAL`, angle), each with `...All`;
  `overlapCircle`, `overlapBox`, `overlapCapsule`, `overlapPoint`, `overlapArea(pointA, pointB)`,
  each with `...All` (Unity 2D's `Overlap*` returns the first, `Overlap*All` every one, as
  `Collider2D`); `Rigidbody2D:cast(direction, distance [, options])` for the body's own shapes.
  Built on `b2World_Cast*` / `b2World_Overlap*`, polygons as `b2Polygon` (rotated boxes through
  `b2MakeOffsetBox`).
- **REQ-12 Options table.** Every query (and `raycast` / `raycastAll` in 3D and 2D) takes an
  optional last `options` table: `ignore` (array of GameObjects, Rigidbodies or entities: their
  bodies are skipped), `triggers` (`QueryTriggerInteraction.USE_GLOBAL` default, `IGNORE`,
  `COLLIDE`), `hitFromInside` (3D, default false) or `startInColliders` (2D, default
  `Physics2D.queriesStartInColliders`), `hitBackfaces` (3D meshes, default
  `Physics.queriesHitBackfaces`). Statics: `Physics.queriesHitTriggers` (true, Unity),
  `Physics.queriesHitBackfaces` (false), `Physics2D.queriesHitTriggers` (true),
  `Physics2D.queriesStartInColliders` (false, D5). The old positional forms keep working.
- **REQ-13 RaycastHit fields.** `RayReturn` (Lua class name kept, alias `RaycastHit`) gains
  `collider` (a `ColliderRef` / `ColliderRef2D`, nil without one), `gameObject`, `rigidbody`
  (`Rigidbody` / `Rigidbody2D` handle), `transform`. Existing fields keep their meaning.
- **REQ-14 Start-inside rule (behaviour change).** `Physics.raycast` / `raycastAll` in 3D no
  longer report a collider that contains the origin (Unity: "Raycasts will not detect Colliders for
  which the Raycast origin is inside the Collider"); `hitFromInside = true` gives the old result.
  `Ray:intersects` (the pre-Unity API) is unchanged. 2D keeps its current behaviour (no hit from
  inside), now named `startInColliders = false`.
- **REQ-15 Character controllers in queries.** A CharacterController with `detectCollisions`
  (inner body) is hit by every 3D query and returned as its GameObject (`rigidbody` nil), as
  `raycast` already does (`docs/mcp/guide/physics.md:540-542`). 2D controllers have no body and are
  never hit (documented, as today).
- **REQ-16 Threads.** Queries run on the main thread (the Lua state's) and from C++ jobs between
  steps; a query from inside a step (a contact callback in synchronous mode) uses the no-lock
  narrow phase, as `getNarrowPhaseQuery3D` does (`PhysicsSystem.h:570-580`).
- **REQ-17 MCP `query_physics`.** A new tool in the `physics` group: `{scene, dimension, shape:
  ray|sphere|box|capsule|point, mode: cast|cast_all|overlap|check, origin, direction,
  max_distance, radius, half_extents, size, angle, rotation, point_a, point_b, layer_mask (names
  or numbers), ignore (entities), triggers, hit_from_inside}`; returns the hits (entity, name,
  collider index, point, normal, distance) and, with `draw: true`, draws them for 3 s in the Game
  view's physics debug lines. It runs against the playing scene; outside Play it fails with
  "query_physics reads the running physics world: start Play first (play)." (bodies exist only in
  Play, inferred from `fixedUpdate` being the only builder, AC-16 settles it).

### Functional — bodies and colliders (P5, P8, E36)

- **REQ-18 Attach To Parent Body (3D and 2D).** A new `RigidbodyComponent` / `Rigidbody2DComponent`
  field `attachToParentBody` (Inspector "Attach To Parent Body", default false). When true and an
  ancestor has an enabled body of the same dimension, this GameObject has no body of its own: its
  colliders become sub-shapes of the nearest such ancestor's body (Unity: a collider below a
  Rigidbody belongs to it), posed by the child's Transform relative to the ancestor, adding mass
  (density, or the ancestor's explicit mass), and their contacts and trigger callbacks go to the
  child GameObject (each sub-shape's `receiver` is the child, the ancestor's scripts also get them,
  as Unity). The child's own body fields (type, mass, damping...) are ignored and greyed out.
- **REQ-19 Re-composition.** The ancestor's shape is composed again at the next step when an
  attached child is added, removed, enabled, disabled, re-parented or its local pose or scale
  changes (once per step whatever the number of changes), keeping the body's id, velocity, joints
  and contacts (the existing shape-swap path, `docs/mcp/guide/physics.md:126-130`). An attached
  child moved by an animation every frame recomposes every step: a warning once per body suggests a
  kinematic child body instead when it happens on more than 30 consecutive steps.
- **REQ-20 Decompose into convex hulls.** An editor command on a `MESH` collider (Inspector button
  "Decompose Into Convex Hulls", MCP `edit_colliders` `decompose: {index, quality: low|medium|high,
  max_hulls}`) runs `ConvexDecomposition` (V-HACD, on a background job with a progress bar) and
  replaces the collider, in one undo step, with up to `max_hulls` (default 16) `CONVEX_HULL`
  colliders of `RAW_VERTICES`, keeping its pose, material, trigger flag and density. The refusal
  of a dynamic or kinematic `MESH` (`PhysicsSystem.cpp:2695`) names the command.
- **REQ-21 movePosition / moveRotation (3D).** `Rigidbody:movePosition(p)`, `moveRotation(q)`,
  `movePositionAndRotation(p, q)` (Unity `MovePosition`, `MoveRotation`, `Move`). The target is
  reached at the end of the next fixed step:
  - kinematic: by velocity over that step (`MoveKinematic`), pushing and carrying what it touches;
  - dynamic: placed at the target before the step (Unity), velocity unchanged;
  - both: the drawn pose interpolates from the previous step's pose to the new one (the body's
    interpolation mode applies), unlike `body.position`, which stays a jump.
  Called from `onUpdate` (several frames per step or several steps per frame) the last call before
  a step wins, spread over that frame's steps for kinematic bodies like Transform following.
- **REQ-22 movePosition / moveRotation (2D).** `Rigidbody2D:movePosition(v2)`, `moveRotation(angle)`,
  `movePositionAndRotation(v2, angle)`, the same rules with Box2D velocities for kinematic bodies.
- **REQ-23 Teleport interpolation option.** `Rigidbody:setPosition(p, interpolate)` /
  `setRotation(q, interpolate)` and the 2D ones take an optional `interpolate` (default false: the
  current jump). True keeps the drawn pose sliding from where it was.
- **REQ-24 Capsule with no cylinder part (E36).** A 3D `CAPSULE` with `halfHeight` 0 is a sphere of
  its radius (Jolt refuses 0, `CapsuleShape.cpp:76-78`), in the shape builder, the gizmo, Physics
  Placement and the CharacterController's capsule (`height <= 2 radius`). `edit_colliders`, the
  Inspector (min 0) and Lua `setColliderHalfHeight(i, 0)` accept 0; negative stays an error.
  `TAPERED_CAPSULE` with `halfHeight` 0 is likewise its larger sphere.

### Functional — joints and ragdolls (P20, R13)

- **REQ-25 Lua-made joints live under bodyA.** `Joint2D(scene)` / `Joint3D(scene)` make a
  GameObject named "Joint 2D" / "Joint 3D". When its bodyA is first set (the `bodyA` property or a
  `set*Joint` helper) and it has no parent, it is parented under bodyA keeping its world pose, and
  the handle stops owning it (`entityOwned` false): it then lives and dies with bodyA's
  GameObject, as joints made by `set_joint` and the Hierarchy do. A joint never given a bodyA keeps
  today's ownership (destroyed with its last owning handle).
- **REQ-26 Add and list joints.** `go:addJoint2D(type)` / `go:addJoint3D(type)` make a child joint
  GameObject of `go` with bodyA = `go` (a Lua error when `go` has no body of that dimension) and
  return its handle with Unity's defaults for the type (`makeJoint2D`, `makeJoint3D`);
  `go:getJoints2D()` / `getJoints3D()` return every joint whose bodyA is `go`; `getJoint2D()` /
  `getJoint3D()` stay (the joint on `go` itself).
- **REQ-27 Joint handles.** `Joint2D` / `Joint3D` gain `gameObject` (its GameObject) and
  `destroy([delay])` (`GameObject.destroy` of its GameObject).
- **REQ-28 Bodies destroyed under a joint.** A joint whose bodyA or bodyB was destroyed leaves the
  simulation and logs once (today's rule, kept and now tested for 3D too); with REQ-25 the common
  case (bodyA destroyed) takes the joint with it.
- **REQ-29 Runtime ragdoll builder.** `Ragdoll.create(root [, options])` (Lua) /
  `RagdollBuilder::build` + `apply` (C++): the wizard on a live model, bones from
  `RagdollBuilder::guessBones` unless `options.bones` names them, `totalMass` (20), `start`
  (`animated`/`simulated`, default `simulated`), `selfCollision`, `flipForward`. Returns the
  `Ragdoll` or raises a Lua error listing the missing required bones. Made in one structural batch
  (no partially built ragdoll is stepped).

### Functional — world settings and stepping (P13, P16, P17, P18)

- **REQ-30 2D points per metre, a scene setting.** `SceneSettings::physics2DPointsPerMeter`
  (default 64, today's value; Inspector > Scene > Physics 2D "Points Per Meter"; MCP
  `physics_2d_points_per_meter`; Lua `scene.physics2DPointsPerMeter`, with
  `physicsSystem.pointsToMeterScale2D` kept as its alias). Saved only when not 64, so old scenes
  load unchanged.
- **REQ-31 Changing the scale live.** Setting it with 2D bodies present rebuilds every 2D body and
  joint before the next step at the same poses (positions are points, unchanged), with velocities
  kept in points/s and masses recomputed in the new metres; contacts restart. The 2D
  CharacterControllers, particles, audio, splines and nav bake that read the scale
  (`CharacterController2DSystem.cpp:140`, `ParticleSubsystem.cpp:653`, `AudioSystem.cpp:2565`,
  `SplineSystem.cpp:1543`, `NavBakeCollector.h:75`) read the new value from the next step or bake.
- **REQ-32 2D max rotation per step.** Scene setting `physics2DMaxRotation` (the engine's angle
  unit per step, default 45 degrees = Box2D's 0.25 pi; Unity's Physics 2D "Max Rotation Speed" is
  360) through a Box2D patch: a `maximumRotation` field in `b2WorldDef` (default `B2_MAX_ROTATION`),
  `b2World_SetMaximumRotation`, used at `solver.c:70` instead of the constant; every hunk marked
  `// JASPER PATCH: max rotation`, the ledger in `engine/libs/box2d/README.jasper.md`.
- **REQ-33 Fast-rotating 2D bodies.** `Rigidbody2DComponent::allowFastRotation` (default false;
  Inspector "Allow Fast Rotation", Lua `body.allowFastRotation`, live) maps to Box2D's
  `b2BodyDef::allowFastRotation` and a new `b2Body_SetAllowFastRotation` (3.1 has no runtime setter;
  part of the same patch) for live changes: the body is not capped at all (for wheels).
- **REQ-34 2D world tuning.** Scene settings, defaults = Box2D's (`src/types.c:9-29`), saved only
  when changed, applied at world creation and live: `physics2DContactHertz` (30),
  `physics2DContactDampingRatio` (10), `physics2DContactPushSpeed` (3 m/s),
  `physics2DJointHertz` (60), `physics2DJointDampingRatio` (2), `physics2DBounceThreshold`
  (restitution threshold, 1 m/s; Unity's "Velocity Threshold"), `physics2DMaxLinearSpeed`
  (400 m/s), `physics2DWarmStarting` (true), `physics2DContinuous` (true), `physics2DSleep` (true).
  Inspector > Scene > Physics 2D > "World" foldout with Unity-like labels, Lua `scene.<name>`, MCP
  `physics_2d_<snake>`.
- **REQ-35 3D world tuning.** Scene settings, defaults = Jolt's (`PhysicsSettings.h:47-90`):
  `physics3DVelocityIterations` (10), `physics3DPositionIterations` (2),
  `physics3DBounceThreshold` (`mMinVelocityForRestitution`, 1 m/s), `physics3DSleepThreshold`
  (`mPointVelocitySleepThreshold`, 0.03 m/s), `physics3DSpeculativeDistance` (0.02 m),
  `physics3DCollisionSteps` (1, 1..4: Jolt's collision steps per fixed step, today's constant at
  `PhysicsSystem.cpp:4412`). Same surfaces as REQ-34 under "Physics > World".
- **REQ-36 Fixed-step cap.** The per-frame step cap becomes
  `Time.maximumFixedStepsPerFrame` (Lua static, C++ `Engine::setMaxFixedStepsPerFrame`; default 0 =
  automatic: `ceil(maximumDeltaTime / fixedDeltaTime) + 1`, at least 1). So the cap never drops
  time that `maximumDeltaTime` lets through (400 Hz with 0.25 s gives 101).
- **REQ-37 Dropped steps reported once.** When steps are dropped the engine logs one warning per
  Play session (editor) or per run (player), naming both settings, and counts them:
  `Time.droppedFixedSteps` (read-only, reset at Play start) and `editor_status` `physics`
  `dropped_fixed_steps`.

### Platform & backends

- **REQ-38** Everything is CPU work inside Jolt and Box2D: the same on GL, GLES3/WebGL2, Metal,
  D3D11 and Vulkan, and on Windows, Linux, macOS, Android, iOS and HTML5 (single-threaded Jolt job
  system on web). The Box2D patch is plain C99. `tools/check-physics-off.sh` keeps building with
  3D off, 2D off and both off: new code sits under `JASPERENGINE_PHYSICS_3D` /
  `JASPERENGINE_PHYSICS_2D`.
- **REQ-39** Exported C++ (`editor/Factory.cpp`) writes every new saved field and scene setting
  only when not at its default, and prebuilt players read them (`RuntimeFields.gen.inc`
  regenerated).

### Performance

- **REQ-40 Disabled is free.** No WheelCollider: no vehicle work (one empty-array check per step).
  No `attachToParentBody` child: no extra hierarchy walks (the flag is tested when a body is
  built). World settings at their defaults: no extra calls per step. Queries cost only when
  called.
- **REQ-41 Budgets (desktop, server E, median of 5; mobile and web 4x).** One `sphereCast` or
  `overlapSphere` against 1000 static boxes, ≤ 16 hits: ≤ 15 µs from C++, ≤ 30 µs from Lua;
  `checkSphere` ≤ 8 µs. 20 four-wheel vehicles: ≤ 0.25 ms added to a fixed step. A recompose of a
  body with 32 attached children ≤ 0.2 ms. Queries allocate nothing in C++ when given an output
  vector with capacity.

### Editor UX (human)

- **REQ-42** Add Component > Physics > "Wheel Collider" and "Vehicle Drivetrain"; GameObject >
  3D Object > "Vehicle..." makes a body with a box collider and four WheelCollider children at the
  corners of the selected mesh's bounds (one undo step). Inspector sections with Unity's labels
  ("Suspension Spring", "Forward Friction", "Sideways Friction"), curves drawn as small plots, and
  a read-only Play foldout (grounded, rpm, slips, gear). Gizmos: the wheel circle, the suspension
  travel line, the contact point and force in Play.
- **REQ-43** Rigidbody / Rigidbody 2D Inspector: "Attach To Parent Body" (with a line naming the
  body it joins, or "no parent body: this is its own body"), "Allow Fast Rotation" (2D), a
  "Decompose Into Convex Hulls" button on `MESH` colliders. Scene Inspector: Physics 2D "Points Per
  Meter", "Max Rotation Per Step", "World" foldouts for 2D and 3D. Every edit is one undo step
  (S24 names: "Set Wheel Collider Radius", "Decompose Collider", ...).
- **REQ-44** Gizmos of attached children draw in the ancestor body's colour; `Physics.debugDraw`
  draws wheels and `query_physics` shapes.

### Agent UX (MCP, Lua, CLI)

- **REQ-45** New components work through the generic tools (`add_component`, `set_properties`,
  `get_runtime_entity`, `set_runtime_property`) by their reflection tables; no vehicle tool.
  `edit_colliders` gains `decompose` (REQ-20) and accepts `half_height` 0 (REQ-24); `body` accepts
  `attach_to_parent_body` and `allow_fast_rotation`. `query_physics` (REQ-17) is the one new tool.
  Scene settings through `set_scene_settings` / `get_scene_settings` (REQ-30..REQ-35).
- **REQ-46** `docs/mcp/guide/physics.md` gets sections "Vehicles (Wheel Collider)", "Shape casts
  and overlaps" (in "Layers, contacts and queries"), "Compound bodies", and rows for the new
  settings; `docs/mcp/guide/lua-api.md` documents every Lua member; `docs/unity-glossary.md` maps
  the Unity names; `docs/mcp/TOOLS.md` regenerated; `tools/mcp/budget.json` ratcheted.
- **REQ-47** Eval cases in `tools/mcp/eval/cases.py`, each failing without its text: an agent asked
  for "a drivable car" adds WheelColliders under a dynamic body (not four hinge joints); an agent
  asked to check "is anything within 2 m of the player" uses `query_physics` `overlap` (not a
  hand-rolled raycast fan); an agent told a dynamic body has a mesh collider uses `decompose`.
- **REQ-48** Lua stubs: new fragments `engine/core/script/binding/stubs/Physics.lua` (queries,
  options, `QueryTriggerInteraction`, `RaycastHit`) and `Vehicle.lua`; the rest through
  `tools/lua_api`.

### Data & compatibility

- **REQ-49** Every new saved field and setting is optional with a default equal to today's
  behaviour; no version bump, no migration (AGENTS.md: "An optional new field gets a default and
  no version bump"). Keys: `wheelcollider:`, `vehicledrivetrain:` (component keys follow class
  names), `attachToParentBody`, `allowFastRotation`, scene keys as named in REQ-30..REQ-35.
- **REQ-50** Behaviour changes (REQ-14 start-inside in 3D, REQ-25 Lua joint parenting and
  ownership, REQ-36 the cap) are listed in `docs/upgrading-from-main.md` and the guide's Pitfalls,
  and the tests that pinned the old behaviour change in the same commit and say so.
- **REQ-51** `tools/test-projects/upgrade-from-main/` (generated) loads with no diff; the
  reflection parity goldens (`editor/tests/reflect/parity_golden/`) and
  `editor/tests/reflect/catalog_snapshot.json` gain only added keys.

## 6. Design constraints & decisions

**D1 One vehicle system beside the character system.** `engine/core/subsystem/VehicleSystem.{h,cpp}`
owned by `PhysicsSystem` (a `std::unique_ptr` declared after `world3D`, friend, like
`CharacterControllerSystem`, `PhysicsSystem.h:189-191`), with `preStep()` (rebuild dirty
vehicles, push inputs) and `postStep(dt)` (read-outs, wheel Transforms) called next to
`characters3D` (`PhysicsSystem.cpp:4408, 4487`). The `VehicleConstraint` is a step listener added
to `world3D`; its collision tester uses our object layer table. No Jolt patch.

```cpp
// engine/core/component/WheelColliderComponent.h
struct JASPERENGINE_API WheelFrictionCurve{ float extremumSlip = 0.4f, extremumValue = 1.0f,
    asymptoteSlip = 0.8f, asymptoteValue = 0.5f, stiffness = 1.0f; };
struct JASPERENGINE_API JointSpring3D{ float spring = 35000.0f, damper = 4500.0f, targetPosition = 0.5f; };
struct JASPERENGINE_API WheelColliderComponent{
    bool enabled = true;
    Vector3 center = Vector3::ZERO;
    float radius = 0.5f, mass = 20.0f, wheelDampingRate = 0.25f;
    float suspensionDistance = 0.3f, forceAppPointDistance = 0.0f;
    JointSpring3D suspensionSpring;
    WheelFrictionCurve forwardFriction;
    WheelFrictionCurve sidewaysFriction{0.2f, 1.0f, 0.5f, 0.75f, 1.0f};
    float maxSteerAngle = Angle::degToDefault(70.0f), maxBrakeTorque = 1500.0f, maxHandBrakeTorque = 4000.0f;
    bool driveTransform = true;
    // runtime inputs (not saved)
    float motorTorque = 0.0f, brakeTorque = 0.0f, steerAngle = 0.0f;
    // runtime read-outs (not saved)
    bool isGrounded = false; float rpm = 0.0f, sprungMass = 0.0f;
    Entity vehicleBody = NULL_ENTITY; int wheelIndex = -1;
};
```

Lua: `go:getWheelCollider()`, class `WheelCollider` (properties as fields, `getGroundHit()` returns a
`WheelHit` or nil, `getWorldPose()` returns position, rotation); `go:getVehicleDrivetrain()`,
class `VehicleDrivetrain` (`throttle`, `steer`, `brake`, `handBrake`, `rpm`, `gear`, `speed`,
`setGear(n)`). Bindings in a new `engine/core/script/binding/VehicleClassesLua.cpp`.

**D2 Unity-shaped, not PhysX-identical.** Unity's WheelCollider is PhysX's vehicle SDK; Jolt's tire
model differs. The bar is: Unity's field names, units and defaults, the same qualitative behaviour
(AC-3, AC-4 thresholds), and the numbers documented ("a Unity car may need its stiffness tuned
by about ±30%"). No attempt to replicate PhysX's internals.

**D3 Wheels drive their own Transform.** Unity makes every user write `GetWorldPose` into a mesh;
`driveTransform` (on by default) does it after each step with interpolation. Turn it off to drive a
separate mesh by hand.

**D4 Queries are one module.** `engine/core/physics/PhysicsQueries3D.{h,cpp}` and
`PhysicsQueries2D.{h,cpp}` implement REQ-10..REQ-16 over a shared `QueryOptions`
(`ignore`, `triggers`, `hitFromInside`, `hitBackfaces`, `startInColliders`, `layerMask`,
`maxHits`); `Physics` / `Physics2D` statics in `PhysicsLayers.h` forward to them; `Ray::cast`
takes the options for the start-inside and trigger rules. Lua bindings in a new
`engine/core/script/binding/PhysicsQueriesLua.cpp` as plain `lua_CFunction`s (the `luaRaycast`
pattern, `CoreClassesLua.cpp:891-952`), not `luabridge::overload`, so S92's overload-dispatch work
(R66) does not interact. Collectors cap hits (`maxHits`, default 256 for `All`, 1 for checks) and
deduplicate by (body, sub-shape) as `sortAndDedupeHits` does. Overlaps collide with back faces
(OpenEngine's rule); casts ignore them unless `hitBackfaces`.

```cpp
// engine/core/physics/PhysicsQueries3D.h (sketch)
struct JASPERENGINE_API QueryOptions{
    uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST;
    const std::vector<Entity>* ignore = nullptr;
    QueryTriggerInteraction triggers = QueryTriggerInteraction::USE_GLOBAL;
    bool hitFromInside = false; bool hitBackfaces = false; bool startInColliders = false;
    size_t maxHits = 256;
};
RayReturn sphereCast(Scene*, Vector3 origin, float radius, Vector3 direction, float maxDistance, const QueryOptions& = {});
size_t overlapSphere(Scene*, Vector3 center, float radius, std::vector<ColliderHit>& out, const QueryOptions& = {});
bool checkSphere(Scene*, Vector3 center, float radius, const QueryOptions& = {});
```

**D5 Start-inside, per dimension.** 3D follows Unity (no hit from inside) because the old result
(a hit at 0 on the caster's own capsule) is the bug the port hit (`CCar.lua:131`). 2D keeps no hit
from inside, which is Unity's `queriesStartInColliders = false` rather than its default `true`: the
default `true` makes a ground check from inside the player's own collider hit the player, the most
common Unity 2D pitfall, and existing 2D projects rely on today's rule. Recorded in the glossary.

**D6 Attached children, not collider components.** JasperEngine keeps colliders in the body
component (`docs/mcp/guide/physics.md:10-14`); introducing a Collider component would be a format
change across every scene. `attachToParentBody` gives Unity's runtime behaviour with an opt-in flag
on the child's existing Rigidbody. The ancestor's compound records which sub-shapes came from
which child (sub-shape user data = collider index in a combined list plus the child entity), so
`ColliderRef`, contacts, `receiver` and `RaycastHit.collider` resolve to the child's collider.

**D7 Decomposition is an edit-time bake.** V-HACD stays out of players (size, time); the result is
plain `CONVEX_HULL` colliders in the scene. Re-importing the mesh does not redo it (the button does).

**D8 movePosition through the follow machinery.** `movePosition` sets a target the next step's
`updateRigidbodyPosition` consumes (a per-body `hasMoveTarget`, `moveTargetPosition`,
`moveTargetRotation`), reusing the kinematic follow path and the interpolation state
(`previousPosition` kept, not reset).

**D9 Joint parenting.** Parenting under bodyA happens when bodyA is set, in the Joint handle's
setter (`Joint2D::setBodyA`, the `set*Joint` helpers), never in the physics step, and only for a
root joint GameObject the handle owns; a joint the user parented elsewhere is left alone.

**D10 World settings live in SceneSettings** beside `gravity2D`, `physicsInterpolation` and
`physics2DSubSteps` (`SceneSettings.h:490-497`), owned live by `PhysicsSystem` with setters that
apply to the running world; the Stream, Factory and Inspector code follow `physics2DSubSteps`
(`Stream.cpp:5103-5104, 5297`, `Factory.cpp:3291`, `InspectorWindow.cpp:14071`,
`SceneTools.cpp:851, 1315`). Grouped as `struct Physics2DWorldSettings` / `Physics3DWorldSettings`
inside `SceneSettings` so the setter is one call; saved as flat keys.

**D11 The Box2D patch is minimal.** One world field, one setter, one solver line, and
`b2Body_SetAllowFastRotation` (3.1 has none). With the patch's default the
solver computes exactly what it does today (`AC-27` bit-identical replay).

**D12 Step cap ownership.** S96 owns the cap and the report (`Engine.cpp:2421-2666`); S95 owns
where the fixed step comes from (ledger S14). The automatic cap reads `updateTime` each frame, so
it follows whatever S95 sets.

## 7. Acceptance criteria

Engine tests go in `engine/tests/core/physics/` (core suite), editor-driven engine tests in
`editor/tests/` beside `Physics3DBodyTest.cpp`, MCP tests in `tools/mcp/tests/` (`CI = True`,
`EDITOR = {"create": True}` unless noted). Every simulation test steps a fixed 1/60 s (or the step
named) with `Engine::setFixedDeltatime`, waits for states in game time, never wall time. "Red"
names the run that fails without the change. Budgets go through `JASPER_TEST_PERF_SLACK` (`*Bench`
suites) or `harness.over_budget`.

| AC | Verifies | Check (red control) |
|---|---|---|
| AC-1 | REQ-1, REQ-2, REQ-9 | `VehicleTest.WheelsMakeOneConstraint`: a 1500 kg box body with 4 WheelColliders settles on a plane; `isGrounded` on all 4 within 60 steps, `sprungMass` sums to the body mass ±1%; disabling one wheel rebuilds with 3; removing all removes the constraint (`VehicleSystem::count() == 0`). Red: no component |
| AC-2 | REQ-4 | `VehicleTest.SuspensionRestsAtTargetPosition`: with `targetPosition` 0.5 the rest compression is 50% ±5% of `suspensionDistance`; 0 rests at full droop ±5% |
| AC-3 | REQ-3 | `VehicleTest.MotorTorqueAccelerates`: 400 N m on the rear wheels for 3 s on a flat plane: speed between 8 and 30 m/s, straight within 0.2 m lateral drift; `brakeTorque` 3000 then stops it within 4 s; `rpm` matches `speed / radius` ±5% while not slipping |
| AC-4 | REQ-4 | `VehicleTest.SteerTurnsAndSlides`: 20° steer at 10 m/s turns with a radius within 25% of the bicycle-model radius; with sideways stiffness 0.2 the same input slides (radius > 2x). Records both radii in the test output |
| AC-5 | REQ-5 | `VehicleTest.DrivetrainShiftsAndCaps`: throttle 1 with auto transmission reaches gear ≥ 3 and an rpm under `maxRPM`; handBrake 1 locks the rear wheels (rpm 0); `motorTorque` reads back non-zero on driven wheels |
| AC-6 | REQ-6, REQ-7 | `VehicleTest.WheelTransformsFollowAndSleep`: wheel GameObjects' world poses equal `getWorldPose()` after each step (1e-5); at rest with zero inputs the body sleeps within 300 steps; setting `motorTorque` wakes it the next step |
| AC-7 | REQ-1..REQ-9 determinism | `VehicleTest.ReplayIsBitIdentical`: two runs of 600 steps with scripted inputs give bit-identical body poses (single-threaded job system, as the web build) |
| AC-8 | REQ-41 | `VehicleBench.TwentyCarsStep`: 20 cars driving: the step's added time ≤ 0.25 ms (`JASPER_TEST_PERF_SLACK`) |
| AC-9 | REQ-42, REQ-45 | `tools/mcp/tests/test_vehicles.py`: `add_component` WheelCollider ×4 under a dynamic body, `set_properties` radius, undo/redo each; `play`, a Lua script sets `motorTorque`; `get_runtime_entity` shows `isGrounded` true and the body's z grows; the "Vehicle..." menu (`run_command`) makes the rig in one undo step |
| AC-10 | REQ-10 | `PhysicsQueries3DTest.CastsMatchGeometry`: sphere, box and capsule casts against a box, a sphere, a mesh and a heightfield return the analytic first-contact distance ±1e-3 and the right normal; `...All` returns every collider once, nearest first; `checkSphere` agrees with `overlapSphere` non-empty on 200 random probes |
| AC-11 | REQ-10, REQ-12 | `PhysicsQueries3DTest.FiltersAndOptions`: layer mask, `ignore` (GameObject, Rigidbody, entity), `triggers` IGNORE/COLLIDE/USE_GLOBAL with `queriesHitTriggers` both ways, a trigger collider on a solid body (companion body) reported once as the trigger's collider |
| AC-12 | REQ-14 | `PhysicsQueries3DTest.RayFromInsideIsNotAHit`: a ray from inside a box returns the next collider (or nil), and with `hitFromInside` the box at 0. Red: today's `Physics.raycast` returns the box at 0 (settles P7's inferred claim). The test pinning the old behaviour, if one exists, changes in the same commit |
| AC-13 | REQ-11, REQ-12 | `PhysicsQueries2DTest.CastsAndOverlaps`: circle, box (rotated), capsule casts and `overlap*` on Box2D shapes in points at scale 64 and 1; `startInColliders` false and true from inside a polygon (true: a hit at 0, normal -direction). Red for "false": records today's no-hit (settles the inferred 2D claim) |
| AC-14 | REQ-13, REQ-15 | `PhysicsQueries3DTest.HitFields`: `collider.index`, `gameObject`, `rigidbody`, `transform` match; a CharacterController with `detectCollisions` is hit with `rigidbody` nil |
| AC-15 | REQ-41 | `PhysicsQueriesBench.SphereCastThousandBoxes`: the C++ budgets of REQ-41 and 0 allocations with a reserved output (counting allocator) |
| AC-16 | REQ-17 | `tools/mcp/tests/test_query_physics.py`: outside Play the error text names `play` (and records whether edit-time bodies exist: settles the inferred claim); in Play, cast/overlap/check in 3D and 2D return the expected entity; `draw: true` adds debug lines; the tool is in the `physics` group, hidden by default |
| AC-17 | REQ-10..REQ-13 Lua | `tools/mcp/tests/test_physics_queries_lua.py`: a script calls every new Lua function once with and without options and prints results; `read_console` matches the expected lines. `tools/lua_api/tests/test_lua_api.py` passes with the new stubs |
| AC-18 | REQ-18, REQ-19 | `Physics3DCompoundTest.ChildrenJoinAncestorBody` (editor suite): a dynamic body with two attached children (one rotated, one scaled) has one Jolt body whose mass = sum of densities x volumes ±1%; a contact on a child's collider fires `onCollisionEnter` on the child and the ancestor; moving a child's local position recomposes once in that step, keeping the body id and velocity. `Physics2DCompoundTest.ChildrenJoinAncestorBody` the same in 2D. Red: today two bodies exist and they collide with each other |
| AC-19 | REQ-19 | `Physics3DCompoundTest.AnimatedChildWarnsOnce`: a child moved every step for 40 steps logs one warning (`LogCapture`) |
| AC-20 | REQ-20 | `tools/mcp/tests/test_physics_3d.py::test_decompose_mesh_collider`: `edit_colliders decompose` on a torus `MESH` gives 2..16 `CONVEX_HULL` colliders with the old pose and material, one undo restores the `MESH`; the body then becomes dynamic in Play and falls. Red: the dynamic switch logs the static-only error |
| AC-21 | REQ-21 | `Physics3DBodyTest.MovePositionKinematicPushes` / `MovePositionDynamicTeleports` / `MoveIsInterpolated`: a kinematic body reaches the target after the next step and pushes a dynamic box; a dynamic body is at the target after the step with its velocity unchanged; with interpolation on, the drawn pose at alpha 0.5 lies halfway between the two step poses (setPosition: at the target). Red: no API |
| AC-22 | REQ-22, REQ-23 | `Physics2DReviewTest.MovePosition2D` (the three cases in 2D) and `Physics3DBodyTest.SetPositionInterpolateOption` |
| AC-23 | REQ-24 | `Physics3DBodyTest.ZeroHalfHeightCapsuleIsSphere` (a capsule with half height 0 rests on a plane at its radius ±1e-3) and `tools/mcp/tests/test_physics_3d.py::test_capsule_half_height_zero` (`edit_colliders` accepts 0, refuses -1 with an error naming the field). Red: the MCP call fails with "must be positive" |
| AC-24 | REQ-25, REQ-27, REQ-28 | `tools/mcp/tests/test_physics_contacts_lua.py::test_lua_joint_lives_under_body` (or a new `test_joints_lua.py`): a script makes `Joint2D(scene)`, sets a wheel joint, drops the handle and runs `collectgarbage()`: the joint still works (wheel attached after 60 steps); destroying the chassis removes the joint GameObject and the Console has no "Cannot create joint" line over 120 steps; the same with `Joint3D`. Red: the joint is gone after the GC |
| AC-25 | REQ-26 | the same test file: `go:addJoint2D(Joint2DType.WHEEL)` returns a joint whose GameObject's parent is `go` and whose fields are `makeJoint2D`'s; `go:getJoints2D()` lists it; on a GameObject without a body it raises an error naming the fix |
| AC-26 | REQ-29 | `RagdollTest.RuntimeCreateFromHumanoid` (editor suite, the existing ragdoll fixture): `Ragdoll.create` on a spawned humanoid makes the same parts as the wizard, simulated, and the pelvis falls; a model without knees raises an error listing them |
| AC-27 | REQ-30, REQ-31, REQ-32, REQ-34, D11 | `Physics2DWorldSettingsTest.DefaultsAreBitIdentical`: 600 steps of a stacked scene with every setting at its default equal today's poses bit for bit (red control: none, it is a guard). `...PointsPerMeterRebuilds`: changing 64 → 1 with bodies live keeps positions, keeps velocity in points/s and makes the 1 kg box weigh 1 kg (mass recomputed); `...MaxRotation`: a wheel driven by torque at 50 Hz exceeds 39 rad/s only with 360°/step or `allowFastRotation` (red: capped at 39.3); `...WorldTuning`: a bouncing ball under `physics2DBounceThreshold` 5 m/s stops bouncing earlier than at 1 |
| AC-28 | REQ-30..REQ-35 data | `StreamSceneTest.PhysicsWorldSettingsRoundTrip`: non-default values round-trip; defaults write no key; a `7f48d7e8d`-era scene loads with every default; `FactoryTest` emits them; `ReflectParity.*` and `CatalogSnapshot` regenerated with added keys only |
| AC-29 | REQ-35 | `Physics3DBodyTest.WorldSettingsApply`: velocity iterations 1 vs 10 changes a 10-box stack's drift (measured); collision steps 2 runs two collision passes (Jolt's step listener count); defaults are bit-identical to today over 600 steps |
| AC-30 | REQ-36, REQ-37 | `ScriptSchedulerTest.FixedStepCapFollowsMaximumDeltaTime` (editor suite): at 1/400 s and a 0.25 s frame, 100 steps run and none dropped (red: 100 steps with a drop warning, from the cap); `maximumFixedStepsPerFrame = 10` drops and logs once over 5 such frames; `Time.droppedFixedSteps` counts them |
| AC-31 | REQ-43, REQ-44 | Manual with screenshots from `tools/editor-shot.sh` on a scratch copy: the Wheel Collider Inspector and gizmo, the Attach To Parent Body line, the scene's Physics 2D World foldout |
| AC-32 | REQ-38 | `tools/check-physics-off.sh` passes; `tools/build.sh web` and `tools/build.sh android` build; `test_physics_contacts_export.py`-style export test `test_vehicles_export.py` (`PREBUILT_PLAYERS = ["glcore-release"]`, `JASPER_SLOW_BUILD=1`): an exported car drives (a Lua marker line in the player log after 2 s) |
| AC-33 | REQ-46..REQ-48 | `test_docs.py`; `tools/mcp/gen_tool_docs.py --check`; `tools/mcp/tool_budget.py --check tools/mcp/budget.json`; the three eval cases of REQ-47 fail without their text and pass with it (Orquester accounts) |

## 8. Dependencies & conflicts

- **Depends on (landed):** S22 (CharacterController, the step bracket), the physics-3d,
  physics-2d and physics-contacts work (`eb8f3a2f5`, `2c3bb2248`, `2d2b47d22`), S08 reflection,
  S24 undo, S31 `call`, S34 module usage (`editor/players/ModuleUsage.cpp:409-431`: WheelCollider
  and drivetrain count as `physics3d`).
- **S94 (rendering):** R13 is physics; S96 owns it by default (REQ-29). If S94 keeps R13, it takes
  REQ-29 and AC-26 as written and S96 drops them. No other overlap.
- **S95 (scripting/runtime):** S95 owns ledger S14 (the fixed step as a setting) and LateUpdate
  (S24). Both touch `Engine::runFrame` (`Engine.cpp:2415-2680`) near REQ-36: S96 changes only the
  cap and the warning; whoever lands second merges. Lua binding files are shared:
  S96 puts its new bindings in new files (`PhysicsQueriesLua.cpp`, `VehicleClassesLua.cpp`) and
  touches `CoreClassesLua.cpp` (Scene settings properties), `GameObjectClassesLua.cpp` (Rigidbody,
  Joint, GameObject members) and `CoroutineClassesLua.cpp` (`Time`) with additions only.
- **S92 (R66 Lua overload dispatch):** new bindings avoid `luabridge::overload` (D4); the
  `setPosition(p, interpolate)` optional argument is a lambda with `std::optional`.
- **S98 (E36):** S96 takes E36 whole (REQ-24: the one-line `edit_colliders` check and the shape
  builder); S98 drops it. If S98 has already changed `WorldTools.cpp:1720-1724`, S96 keeps that and
  adds the engine part.
- **Merge hotspots:** `engine/core/subsystem/PhysicsSystem.{h,cpp}` (WP-A hooks, WP-C compose and
  move targets, WP-E world creation), `engine/core/component/RigidbodyComponent.h` and
  `Rigidbody2DComponent.h` (WP-C, WP-E), `editor/window/InspectorPhysics*.cpp` (WP-A, WP-C,
  WP-E), `editor/reflect/components/TransformPhysicsReflect.cpp`, `editor/Catalog.h` (component ids),
  `editor/mcp/tools/WorldTools.cpp` (`edit_colliders`), `editor/mcp/tools/SceneTools.cpp`,
  `docs/mcp/guide/physics.md`, `docs/mcp/guide/lua-api.md`, generated files
  (`docs/mcp/TOOLS.md`, `RuntimeFields.gen.inc`, `catalog_snapshot.json`, parity goldens),
  `tools/mcp/budget.json`.

## 9. Risks & defaults

| Risk / question | Default taken |
|---|---|
| Jolt's tire model cannot match a Unity car's feel with Unity's default curves | D2: same names and defaults, qualitative ACs, a documented tuning note; a per-wheel `frictionScale` is not added unless AC-4 fails by more than 2x |
| A custom `WheeledVehicleController` subclass depends on Jolt's protected members, which a Jolt upgrade may change | The subclass lives in one file with a comment naming the Jolt version; `AC-1..AC-7` catch a break at upgrade |
| REQ-14 changes 3D `Physics.raycast` results for scripts that cast from inside a collider | Accepted as a fix (Unity's rule, the port's bug); `hitFromInside = true` restores the old result; listed in `docs/upgrading-from-main.md` |
| REQ-25 changes Lua joint lifetime (no longer GC-destroyed once it has a bodyA) | Accepted: the old lifetime was a leak-or-vanish bug (P20); a joint without bodyA keeps the old rule |
| Attached children animated every step recompose a compound each step | Measured by REQ-41's budget; warned once (REQ-19); a kinematic child body stays the right tool |
| Points Per Meter changed mid-game drops contacts for one step | Documented; set it in the scene, not at run time |
| 2D `startInColliders` default differs from Unity's | D5, recorded in the glossary |
| `query_physics` outside Play | Play only; AC-16 records whether edit-time bodies exist, and a follow-up may lift it |
| The V-HACD result depends on the library version | It is baked into the scene; reruns are explicit |
| The automatic step cap lets a very low `fixedDeltaTime` run many steps in a long frame | Same as Unity's Maximum Allowed Timestep; `maximumFixedStepsPerFrame` bounds it |

## 10. Work packages

Five packages, each in its own worktree and branch (`feature/s96-<pkg>`), merged into
`feature/s96-physics-gaps`. To keep `PhysicsSystem.{h,cpp}` merges trivial, each package adds its
members in its own block with a `// S96 <pkg>` comment, new logic goes in new `.cpp` files, and the
one shared step function (`fixedUpdate`) gets at most one call line per package. Generated files
(`TOOLS.md`, `RuntimeFields.gen.inc`, `catalog_snapshot.json`, parity goldens, `budget.json`) are
regenerated by whoever merges last, never hand-merged. Each package adds its own section to
`physics.md` / `lua-api.md` (named in its row) and does not edit the others'. Landing order:
WP-E, WP-D, WP-B, WP-C, WP-A (smallest and most independent first).

| Package | Items | Owns (new files in bold) | Touches (shared, additions only) | Size | Depends |
|---|---|---|---|---|---|
| **WP-A Vehicles** | P2: REQ-1..REQ-9, REQ-42, vehicle parts of REQ-41, REQ-45..REQ-48; AC-1..AC-9, AC-32 | **`engine/core/component/WheelColliderComponent.h`, `VehicleDrivetrainComponent.h`, `engine/core/subsystem/VehicleSystem.{h,cpp}`, `engine/core/physics/WheelColliderController.{h,cpp}`, `engine/core/object/physics/WheelCollider.{h,cpp}`, `VehicleDrivetrain.{h,cpp}`, `engine/core/script/binding/VehicleClassesLua.cpp`, `stubs/Vehicle.lua`, `editor/reflect/components/VehicleReflect.cpp`, `editor/window/InspectorVehicle.cpp`, `engine/tests/core/physics/VehicleTest.cpp`, `VehicleBench.cpp`, `tools/mcp/tests/test_vehicles.py`, `test_vehicles_export.py`** | `PhysicsSystem.{h,cpp}` (member + 2 call lines), `editor/Catalog.h` (2 ids), `ComponentFwd.h`, `JasperEngine.h`, `EntityCloner.cpp`, `ModuleUsage.cpp`, `ComponentAddDialog.cpp`, `SceneRender3D.cpp` (wheel gizmo), `LuaBinding.cpp` (one register call), `GameObjectClassesLua.cpp` (`getWheelCollider`, `getVehicleDrivetrain`), the "Vehicle..." menu entry; guide section "Vehicles (Wheel Collider)" | L | none |
| **WP-B Queries** | P4 queries, P7: REQ-10..REQ-17, REQ-40/41 query parts, REQ-47 (two cases), REQ-48; AC-10..AC-17 | **`engine/core/physics/PhysicsQueries3D.{h,cpp}`, `PhysicsQueries2D.{h,cpp}`, `engine/core/script/binding/PhysicsQueriesLua.cpp`, `stubs/Physics.lua`, `editor/mcp/tools/PhysicsQueryTools.cpp`, `engine/tests/core/physics/PhysicsQueries3DTest.cpp`, `PhysicsQueries2DTest.cpp`, `PhysicsQueriesBench.cpp`, `tools/mcp/tests/test_query_physics.py`, `test_physics_queries_lua.py`** | `engine/core/registry/PhysicsLayers.{h,cpp}`, `engine/core/math/Ray.{h,cpp}` (options into `cast`), `MathClassesLua.cpp` (`RayReturn` fields), `CoreClassesLua.cpp` (raycast options), `Rigidbody.{h,cpp}` / `Rigidbody2D.{h,cpp}` (`sweepTest`, `cast`: one block each), MCP registry row; guide "Layers, contacts and queries" | M-L | none |
| **WP-C Bodies and colliders** | P5, P8, E36: REQ-18..REQ-24, REQ-43 (body parts), REQ-47 (decompose case); AC-18..AC-23 | **`engine/core/physics/AttachedColliders.{h,cpp}` (child gathering, recompose), `editor/command/DecomposeColliderCommand.{h,cpp}`, `editor/tests/Physics3DCompoundTest.cpp`, `Physics2DCompoundTest.cpp`** | `PhysicsSystem.{h,cpp}` (compose call, move targets in `updateRigidbodyPosition`), `RigidbodyComponent.h`, `Rigidbody2DComponent.h` (`attachToParentBody`, move target fields), `Rigidbody.{h,cpp}`, `Rigidbody2D.{h,cpp}` (move APIs: a different block from WP-B), `JoltShapeBuilder.cpp`, `CharacterControllerSystem.cpp` (zero-height capsule), `InspectorPhysics.cpp`, `InspectorPhysics2D.cpp`, `TransformPhysicsReflect.cpp`, `WorldTools.cpp` (`edit_colliders`), `GameObjectClassesLua.cpp` (Rigidbody members), `SceneRender3D.cpp` (attached colour); guide "Compound bodies", the teleport rows of "Forces, velocity, teleport" | L | none (WP-B's `sweepTest` uses the composed shape either way) |
| **WP-D Joints and ragdoll** | P20, R13: REQ-25..REQ-29; AC-24..AC-26 | **`tools/mcp/tests/test_joints_lua.py`** | `Joint2D.{h,cpp}`, `Joint3D.{h,cpp}`, `GameObject.{h,cpp}` (`addJoint*`, `getJoints*`), `GameObjectClassesLua.cpp` (Joint and Ragdoll members, GameObject methods), `RagdollBuilder.{h,cpp}` (`build`), `Ragdoll.{h,cpp}`, `editor/tests/RagdollTest.cpp`; guide "2D joints", "3D joints", "Ragdolls" | S | none |
| **WP-E World settings and stepping** | P13, P16, P17, P18: REQ-30..REQ-37, REQ-43 (scene parts); AC-27..AC-30 | **`engine/core/physics/WorldSettings.{h,cpp}` (apply to Box2D / Jolt), `engine/tests/core/physics/Physics2DWorldSettingsTest.cpp`** | `engine/libs/box2d/{include/box2d/types.h, include/box2d/box2d.h, src/world.{h,c}, src/solver.c, src/body.c, README.jasper.md}` (the patch), `SceneSettings.h`, `Scene.{h,cpp}`, `PhysicsSystem.{h,cpp}` (world creation `:384-403`, setters, scale rebuild), `Rigidbody2DComponent.h` (`allowFastRotation`: a different block from WP-C), `Rigidbody2D.{h,cpp}`, `Engine.{h,cpp}` (cap), `EngineTime.{h,cpp}`, `CoroutineClassesLua.cpp` (`Time`), `CoreClassesLua.cpp` (Scene properties), `editor/Stream.cpp`, `editor/Factory.cpp`, `editor/window/InspectorWindow.cpp`, `SceneTools.cpp`, `editor/mcp/tools/*` `editor_status`, `editor/tests/ScriptSchedulerTest.cpp`; guide rows in "Mass" (units) and a "World settings" section | M | none |

Per-package done: its ACs green (targeted: `UNIT_TEST_ARGS=--gtest_filter=...`,
`tools/build.sh mcp-tests -k <file> -j 2`), `tools/check-changed`, the lints of the areas touched
(`check_raw_writes.py` not needed: no new file writes; `check_event_clears.py` if an event is added;
`check_stream_keys.py` for the new keys), WERROR build, and its docs section.

## Deviations

(none yet)
