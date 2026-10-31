# `physics-3d`: Rigidbody fields, per-collider triggers and materials, joints with limits/motors/springs/breaking, ragdolls, no caps (design)

Status: binding spec for branch `gaps/physics-3d` (ledger #9). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (shared conventions binding) and
[GAP-REPORT.md §4, §10](../../engine-gaps/GAP-REPORT.md). Siblings:
[physics-2d](2026-10-31-physics-2d-design.md), [physics-contacts](2026-10-31-physics-contacts-design.md).
This spec **owns** shared pieces used by both siblings: the `PhysicsMaterial` asset (§6.1.4), the
`PhysicsCommon.h` enums (§6.1.1), `JoltPairFilter3D` (§6.1.6), per-body interpolation for 2D and 3D
(§6.1.10) and the runtime debug draw for 2D and 3D (§6.1.12).

Abbreviations: `PS` = `engine/core/subsystem/PhysicsSystem.cpp`, `RB` =
`engine/core/object/physics/Rigidbody.cpp`, `JPA` = `engine/core/util/JoltPhysicsAux.h`, `JSB` =
`engine/core/physics/JoltShapeBuilder.cpp`, `GOL` = `engine/core/script/binding/GameObjectClassesLua.cpp`,
Jolt = vendored Jolt 5.2 (`engine/libs/joltphysics`).

## 1. Goals

1. Rigidbody fields Unity users expect, serialized and live: linear/angular damping, max velocities,
   explicit mass, centre of mass, inertia tensor, sleeping, per-body interpolation, `isSleeping`.
2. Safe body-type switching static ↔ kinematic ↔ dynamic from Lua, Inspector and MCP, in Play, without
   Jolt asserts (`RB:381-402` calls `SetMotionType` on bodies created without
   `mAllowDynamicOrKinematic`, `PS:1483-1551`).
3. Per-collider trigger and per-collider material (Unity: `isTrigger` and `material` are per collider);
   a shared `PhysicsMaterial` asset with Unity combine modes, for 3D and 2D.
4. Add, remove, resize and replace colliders at runtime from Lua cheaply (shape swap, body kept).
5. 3D joints: limits, motors, springs, friction, break force/torque for hinge, slider, distance,
   swing-twist, six-DOF; local anchors; enable collision; fix the cone joint (`mHalfConeAngle` stays 0,
   `PS:2349-2396`); joint read-outs; `onJointBreak`.
6. Ragdolls: an editor "Create Ragdoll" wizard (Unity's Ragdoll Wizard) and a `Ragdoll` component that
   switches a skinned character between animated (kinematic bodies following the bones) and simulated
   (dynamic bodies driving the bones and therefore the skin).
7. No caps that differ between editor and device: runtime-sized collider vertex/index data
   (`engine/core/Engine.h:136-146`, `editor/Exporter.cpp:2923-2925`) and configurable world capacity
   (`PS:235-238`, hard-coded 1024), overflow reported instead of silently dropped contacts.
8. A runtime collider/contact debug draw in Play mode and exported builds, toggled from the Game view,
   Lua and MCP.

## 2. Non-goals

Character controller, vehicles / wheel colliders, navmesh (program scope). Articulations. Unity
`ConfigurableJoint` projection, `freeSpin`, separate static friction (Jolt and Box2D have one friction).
Ragdoll get-up blending from the ragdoll pose back to animation (v1 snaps, like Unity; the hook is
left in §6.1.11). Jolt's own `JPH::Ragdoll` class (see D10). Collider shapes on child GameObjects.

## 3. Users and stories

- Porting BACK-MAN's entity (`Prefabs/B- Backrooms/Entity.prefab`, `Scripts/Minigames/Backrooms/RagdollManager.cs`):
  11 bone bodies (masses 3.125 / 1.875 / 1.25, total 20, Ragdoll Wizard pattern), kinematic +
  interpolated while animated, 10 CharacterJoints (e.g. thigh: twist -20..70, swing1 30, swing2 0);
  `Activate()` disables the Animator and sets `isKinematic = false`, `useGravity = true`; `Deactivate()`
  zeroes velocity and goes back. With the wizard and `ragdoll.simulated = true` this is one line.
- Porting kart/garden scripts: `rb.drag = 5`, `freezeRotation`, `isKinematic` toggles
  (`TeamWater/TEnemyBehaviour.cs:70-71`, `Garden/GThirdPersonController.cs:59-60`,
  `EHGame/EHPlayerScript.cs:81`), zero friction materials on bullets
  (`Models/Invaders/PLAYER/New Physic Material`).
- Level designer: a door on a hinge with limits 0..110° and a spring that closes it; a drawbridge on a
  motor; a chain of planks that breaks under a heavy crate.
- Programmer: rebuilds a procedural mesh collider with 20 000 indices from Lua at runtime; resizes a
  capsule when the character crouches.
- Agent (MCP): "make this crate bouncy" → `edit_physics_material` + `edit_colliders material`;
  "create a ragdoll for Hero" → `create_ragdoll`; "show me the colliders while playing" →
  `configure_game_view physics_debug: true` then `screenshot`.

## 4. Research summary

- Unity Rigidbody (2022.3 / Unity 6 names): `drag`/`linearDamping` 0, `angularDrag`/`angularDamping`
  0.05, `mass` 1, `centerOfMass` + `automaticCenterOfMass` + `ResetCenterOfMass`, `inertiaTensor` +
  `inertiaTensorRotation` + `automaticInertiaTensor`, `isKinematic` (ragdoll pattern), `interpolation`
  None/Interpolate/Extrapolate, `IsSleeping`/`Sleep`/`WakeUp`, `maxAngularVelocity` (settings default
  50 rad/s). <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Rigidbody.html>. Take: all
  of these. Leave: Unity's 0 linear drag default (we keep Jolt's 0.05 so old scenes behave the same).
- Unity `PhysicMaterial`: dynamic/static friction 0.6, bounciness 0, `frictionCombine` /
  `bounceCombine` Average | Minimum | Multiply | Maximum, priority Average < Minimum < Multiply <
  Maximum; `Collider.material` (instancing getter) vs `sharedMaterial`.
  <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/PhysicMaterialCombine.html>. Take:
  modes, priority, instancing semantics. Leave: separate static friction. Add Unity 6 2D's "Mean"
  (geometric) mode, needed for 2D defaults and for "legacy".
- Unity joints: `Joint` base (`anchor`, `axis`, `connectedBody`, `connectedAnchor`,
  `autoConfigureConnectedAnchor`, `breakForce`/`breakTorque` ∞, `enableCollision` false,
  `currentForce`/`currentTorque`, `OnJointBreak(float)`), `HingeJoint` (`useLimits` + `limits`,
  `useMotor` + `motor {targetVelocity, force}`, `useSpring` + `spring {spring, damper,
  targetPosition}`, `angle`, `velocity`), `CharacterJoint` (`swingAxis`, `low/highTwistLimit`,
  `swing1Limit`, `swing2Limit`), `SpringJoint` (`spring`, `damper`, `minDistance`, `maxDistance`),
  `ConfigurableJoint` (x/y/z/angular motions Locked/Limited/Free, limits, drives, targets).
  <https://docs.unity3d.com/2022.3/Documentation/Manual/class-HingeJoint.html>,
  <https://docs.unity3d.com/2022.3/Documentation/Manual/class-CharacterJoint.html>.
- Unity Ragdoll Wizard (`UnityCsReference/Editor/Mono/RagdollBuilder.cs`, 2022.3): bones pelvis,
  L/R hips, knee, foot, arm, elbow, middle spine, head; total mass 20; per-bone collider, density and
  CharacterJoint limits (§6.3.3 table); feet get no body.
- Godot 4 "Create Physical Skeleton" (PhysicalBone3D + joint types, `physical_bones_start_simulation`)
  and Unreal Physics Assets (bodies + constraints as a data asset). Take Godot's "editor generates
  bodies, runtime toggles simulation", Unity's baked components (they are editable, prefab-friendly
  and visible to agents). Leave a separate physics-asset file.
- Jolt 5.2: `BodyCreationSettings` (`Body/BodyCreationSettings.h:95-115`: `mAllowDynamicOrKinematic`,
  damping 0.05/0.05, `mMaxAngularVelocity` 0.25·π·60, `mUseManifoldReduction`,
  `mOverrideMassProperties` CalculateMassAndInertia / CalculateInertia / MassAndInertiaProvided),
  `OffsetCenterOfMassShape`, `BodyInterface::SetShape/SetMotionType/SetUserData`,
  per-shape `PhysicsMaterial` (`ConvexShape::SetMaterial`, mesh/heightfield `mMaterials`), contact
  combine functions with sub-shape ids (`Constraints/ContactConstraintManager.h:48-58`), group filters,
  constraint settings: hinge limits must contain 0 (`HingeConstraint.h:45-57`), slider likewise,
  `SpringSettings` FrequencyAndDamping / StiffnessAndDamping, `MotorSettings`, `GetTotalLambda*` per
  constraint (impulses of the last step), swing-twist limits: twist about X, `mPlaneHalfConeAngle`
  about the plane axis (Y), `mNormalHalfConeAngle` about the normal (Z)
  (`Constraints/SwingTwistConstraint.cpp:89`), `EPhysicsUpdateError` from `PhysicsSystem::Update`.
- Engine skinning: bone entities drive the skin through their Transforms
  (`engine/core/subsystem/RenderSystem.cpp:13171-13184` skin bindings, `:13796-13810` bone components),
  so physics writing bone Transforms moves the mesh. **But** the physics write-back computes a child
  body's local pose from the parent's cached `modelMatrix` (`PS:83-90` `setTransformWorldPose`), which
  is stale when the parent (or an ancestor) was written in the same pass: nested dynamic bodies (every
  ragdoll) drift. Fixed here (D11).

## 5. Product decisions

- **D1 Defaults keep old scenes.** Every new Rigidbody field defaults to what Jolt used implicitly
  (damping 0.05/0.05, max angular 47.12 rad/s, max linear 500 m/s, automatic mass/COM/inertia,
  sleeping allowed, interpolation from the scene setting). Missing keys decode to those.
- **D2 Mass is authorable** (`automaticMass = false` + `mass`), still density-based by default; the
  live `setMass` now writes the field (persistent across reloads).
- **D3 Type switching** is always possible except for static-only shapes: bodies are created with
  `mAllowDynamicOrKinematic = true` unless a collider shape `MustBeStatic()` (mesh, heightfield).
  Switching such a body to kinematic/dynamic is refused with an error naming the collider. Lua
  `isKinematic` alias (true → KINEMATIC, false → DYNAMIC).
- **D4 Per-collider trigger** `Collider3D::isTrigger`. Implemented with `ContactSettings::mIsSensor`
  in the contact listener (any sub-shape that is a trigger makes the contact a sensor contact);
  triggers do not count in mass properties (unless every collider is one). The body-level `sensor`
  stays (legacy, Jolt `mIsSensor`), shown in the Inspector only when set, with "Convert to collider
  triggers".
- **D5 Kinematic triggers** see static and kinematic bodies: a kinematic body with any trigger gets
  `CollideKinematicVsNonDynamic` automatically (Unity matrix; physics-contacts D15).
- **D6 Shared `PhysicsMaterial` asset** (`.physicsmaterial`, JSON) for 3D and 2D: `friction`,
  `bounciness`, `frictionCombine`, `bounceCombine`. Collider `material` > body `material` > legacy
  surface (3D: the body's `friction` / `restitution`; 2D: the collider's inline values).
  "Create > Physics Material" (Average/Average, friction 0.6) and "Create > Physics Material 2D"
  (same type, preset friction 0.4, GeometricMean/Maximum = Unity 6 2D).
- **D7 Combine rule (both dimensions).** Modes `AVERAGE`, `GEOMETRIC_MEAN`, `MINIMUM`, `MULTIPLY`,
  `MAXIMUM`; a side without material has no mode; the pair uses the higher-priority mode of the sides
  that have one (priority in that order, Unity 3D's with `GEOMETRIC_MEAN` after `AVERAGE`); with none,
  legacy = geometric-mean friction and maximum restitution (today's Jolt and Box2D rules). Values
  combined are each side's resolved friction/bounciness.
- **D8 Instancing.** Lua `setColliderFriction(i, f)` / `setColliderBounciness(i, b)` on a collider
  that uses an asset makes a runtime instance (copy of the asset) for that collider and edits it
  (Unity `collider.material`); `PhysicsMaterial.load(path)` edits are shared by every user of the asset
  until the scene ends (Unity `sharedMaterial`); nothing is written to disk at runtime.
- **D9 Joints, Unity-style and local.** Same `anchorSpace` (WORLD legacy / LOCAL default for new
  joints) as physics-2d: bodyA = the joint's own Rigidbody, bodyB = connected body; anchors local to
  their body, axes in bodyA's frame. `enableCollision` default false for new joints, **true** for old
  files (Jolt never disabled collision between connected bodies). Hinge/slider limits whose range does
  not contain 0 are re-centred (constraint frame rotated/shifted by the range centre) so Unity values
  like 10..50° work. Break force/torque + `breakAction` + `onJointBreak(force, joint)`.
- **D10 Ragdoll = ordinary components.** The wizard creates Rigidbodies, colliders and SWINGTWIST
  joints on the bone GameObjects (visible, editable, in prefabs, reachable by MCP) and a `Ragdoll`
  component on the character root. Not `JPH::Ragdoll` (its bodies would bypass the component model,
  serialization and every tool).
- **D11 Nested bodies.** The write-back after a step processes bodies in Transform hierarchy order and
  recomputes each written Transform's world matrix (and the stale ancestors between bodies) before its
  children are written, so chains of dynamic bodies are exact.
- **D12 No caps.** `colliders`, `vertices`, `indices` are `std::vector`; indices `uint32_t`; delete
  `MAX_COLLIDERS_3D`, `MAX_COLLIDER_VERTICES_3D`, `MAX_COLLIDER_INDICES_3D`. World capacity is a
  project setting (defaults 8192 bodies, 16384 body pairs, 8192 contact constraints) used by the editor
  and exported builds alike; overflow is logged once per kind.
- **D13 Shape-only rebuilds.** Collider edits on a live body build the new shape and call
  `BodyInterface::SetShape`; the body keeps its id, velocities, joints, sleeping state and contacts of
  unchanged colliders; mass properties re-applied. A full body rebuild only for: `MustBeStatic` change,
  layer table overflow, or body creation.
- **D14 Per-body interpolation** `interpolation` = SCENE (default, the scene's
  `physics_interpolation`) | NONE | INTERPOLATE | EXTRAPOLATE, on 3D and 2D bodies.
- **D15 Debug draw** `Physics.debugDraw` (all scenes; 3D and 2D), `Physics.debugDrawContacts`; Game
  view toolbar "Physics" toggle; works in exported builds (for on-device debugging).

## 6. Design

### 6.1 Runtime C++

#### 6.1.1 Shared enums (`engine/core/component/PhysicsCommon.h`, new)

`RigidbodyInterpolation {SCENE, NONE, INTERPOLATE, EXTRAPOLATE}`, `JointAnchorSpace {WORLD, LOCAL}`
(2D joints use the same enum, Lua `JointAnchorSpace`), `JointBreakAction {IGNORE, CALLBACK_ONLY, DISABLE, DESTROY}`,
`PhysicsMaterialCombine {NONE = 0, AVERAGE, GEOMETRIC_MEAN, MINIMUM, MULTIPLY, MAXIMUM}`, and
`float combineSurface(float a, PhysicsMaterialCombine ma, float b, PhysicsMaterialCombine mb, PhysicsMaterialCombine legacy)`.

#### 6.1.2 `RigidbodyComponent` (`engine/core/component/RigidbodyComponent.h`)

New serialized fields (defaults = legacy):

| field | default | Jolt |
|---|---|---|
| `linearDamping`, `angularDamping` | 0.05, 0.05 | settings / `MotionProperties::SetLinearDamping/SetAngularDamping` |
| `maxLinearVelocity`, `maxAngularVelocity` | 500 m/s, 47.1239 rad/s | `mMaxLinearVelocity`, `mMaxAngularVelocity` |
| `automaticMass`, `mass` | true, 1 kg | false → mass override (§6.1.3) |
| `automaticCenterOfMass`, `centerOfMass` | true, (0,0,0) | body-local, world units (not scaled), like Unity |
| `automaticInertiaTensor`, `inertiaTensor`, `inertiaTensorRotation` | true, (1,1,1) kg·m², identity | diagonal about the COM, in the rotated frame |
| `allowSleeping` | true | `mAllowSleeping` (today live-only `RB:604-611`) |
| `collideKinematicVsNonDynamic` | false | user flag (today live-only); D5 adds it automatically |
| `interpolation` | SCENE | §6.1.10 |
| `material` | "" | default material for colliders without one |

Runtime-only: `previousLinearVelocity/AngularVelocity` (for EXTRAPOLATE), `massDirty`.

`Collider3D` gains `isTrigger` (false), `material` (""), `receiver` (physics-contacts), runtime
`JPH::Ref<JoltSurfaceMaterial> surface` and `std::shared_ptr<PhysicsMaterialRuntime> instance`
(D8). `vertices` → `std::vector<Vector3>`, `indices` → `std::vector<uint32_t>`; `numVertices`,
`numIndices`, `numColliders` removed (sizes); `colliders` → `std::vector<Collider3D>`. All users
updated (PhysicsSystem, Rigidbody, JoltShapeBuilder, PlacementWorld, Water, Ray, EntityCloner,
Catalog, Project, Factory, Stream, McpValues, WorldTools, SceneRender3D, InspectorWindow, tests).
C++ `createMeshCollider(..., std::vector<uint16_t>)` keeps an overload that widens.

#### 6.1.3 Body creation and mass properties (`PS:1483-1551`)

`createGenericJoltBody` additions: damping, max velocities, `mAllowSleeping`,
`mAllowDynamicOrKinematic = !shape->MustBeStatic()`, `mCollideKinematicVsNonDynamic =
collideKinematicVsNonDynamic || (type == KINEMATIC && anyTrigger)`, `mUseManifoldReduction =
!(colliders > 1 && (anyTrigger || mixed surfaces || listener collision bits))` (physics-contacts
§6.1.4), user data via physics-contacts' packer (`entity | mask << 32`; until that merges, the entity),
`mCollisionGroup = CollisionGroup(pairFilter3D, groupId, entity)` (§6.1.6).

Mass (`PhysicsSystem::computeMassProperties3D(body)`), used when any of: not automatic mass/COM/inertia,
a trigger collider, legacy `overrideMassProperties`:
1. `MassProperties total` = Σ over non-trigger colliders (all if every one is a trigger) of
   `leaf->GetMassProperties()` scaled (`ScaledShape` semantics of the built leaf), `Rotate(rot)`,
   `Translate(pos)`; Jolt sums mass and inertia (same origin).
2. `!automaticMass` → scale inertia by `mass / total.mMass`, set mass.
3. `!automaticCenterOfMass` → wrap the body shape in `OffsetCenterOfMassShapeSettings(centerOfMass −
   shape->GetCenterOfMass(), shape)`; inertia re-expressed about the new COM (parallel axis,
   `MassProperties::Translate`).
4. `!automaticInertiaTensor` → `inertia = R · diag(inertiaTensor) · Rᵀ`, R = `inertiaTensorRotation`.
5. `settings.mOverrideMassProperties = MassAndInertiaProvided`, `mMassPropertiesOverride = total`.
Legacy `setOverrideMassAndInertia` (solid box) keeps its meaning and wins.

#### 6.1.4 `PhysicsMaterial` asset (shared; `engine/core/physics/PhysicsMaterial.{h,cpp}`, `engine/core/pool/PhysicsMaterialPool.{h,cpp}`)

File `<name>.physicsmaterial`, UTF-8 JSON, canonical writer like `AnimationClipIO`
(`engine/core/animation/AnimationClipIO.h` contract style: lenient parse, deterministic write):

```json
{
  "format": "jasperengine.physicsmaterial",
  "version": 1,
  "guid": "6f1c…",
  "friction": 0.6,
  "bounciness": 0,
  "frictionCombine": "average",
  "bounceCombine": "average"
}
```

- `friction` ≥ 0 (no upper clamp; Unity allows > 1, the tire uses 2), `bounciness` 0..1 (clamped,
  warning), combine strings `average | geometric_mean | minimum | multiply | maximum`. Unknown keys kept.
  Unity import aliases accepted: `dynamicFriction` → `friction`, `bounce` → `bounciness`.
- `PhysicsMaterialPool::getOrLoad(path)` → `std::shared_ptr<PhysicsMaterialRuntime>` (values +
  generation), through `Data::open` (resource packs), normalised keys like `AnimationClipPool`;
  editor invalidates on save/reimport; bodies whose colliders use a changed key re-resolve at the next
  step (3D: update `JoltSurfaceMaterial` values in place; 2D: shape setters + refilter, physics-2d
  §6.1.6).
- `JoltSurfaceMaterial : JPH::PhysicsMaterial` (JPH RTTI like `PhysicsMaterialSimple`): `friction`,
  `restitution`, `frictionCombine`, `bounceCombine` (NONE = no material), `isTrigger`, `colliderIndex`.
  One per collider, assigned to the leaf shape (`ConvexShape::SetMaterial`, mesh/heightfield
  `mMaterials = {surface}` with every triangle index 0) in `JSB::buildColliderShape(..., const
  JPH::PhysicsMaterial*)`. Values are written only outside the step; job threads read them.
- `world3D.SetCombineFriction(&combineFriction3D)` / `SetCombineRestitution`: for each side
  `m = DynamicCast<JoltSurfaceMaterial>(body.GetShape()->GetMaterial(subShapeId))`; value =
  material value if it has one, else `body.GetFriction()/GetRestitution()` (legacy body surface or
  body `material` resolved into the body's friction/restitution), mode as stored; result
  `combineSurface(...)` with legacy GEOMETRIC_MEAN / MAXIMUM.

#### 6.1.5 Contact listener additions (`JPA:296-389`, ordered before physics-contacts' recording)

In `OnContactAdded` and `OnContactPersisted`: if either sub-shape's `JoltSurfaceMaterial::isTrigger`
→ `ioSettings.mIsSensor = true` (both callbacks: Jolt re-initialises settings every call). Pure reads,
thread-safe.

#### 6.1.6 `JoltPairFilter3D` (shared; in `JPA`)

`class JoltPairFilter3D : public JPH::GroupFilter` owned by `PhysicsSystem`. Every body gets
`CollisionGroup(filter, groupId (cInvalidGroup unless setCollisionGroupID), subGroupId = entity)`.
`CanCollide(g1, g2)`: false if both group ids are valid and equal (keeps `Rigidbody::setCollisionGroupID`
semantics, `RB:597-602`, which now sets the id on our filter instead of a new `GroupFilterTable`); false
if the entity pair is in `ignored` (an `unordered_map<uint64 pairKey, uint32 reasons>`: bit `JOINT`
counts joints with `enableCollision == false` between the pair, bit `SCRIPT` = `Physics.ignoreCollision`,
bit `RAGDOLL` = ragdoll self-collision off); else true. Mutated only outside the step (joint create /
destroy, Lua), read by job threads. Changes wake both bodies (`PS::wakeAround3D`).

#### 6.1.7 Body type switching and collider edits (`RB`)

- `setType(t)`: refuse non-static for `!CanBeKinematicOrDynamic()` (error names the static-only
  collider); stop following; component `type = t`; `SetMotionType(t, Activate unless STATIC)`;
  re-apply motion quality, auto `CollideKinematicVsNonDynamic` (D5), zero velocities when STATIC; keeps
  velocities KINEMATIC → DYNAMIC (ragdoll inherits the follow velocity). Inside a synchronous-mode
  in-step callback (physics-contacts D12) the change is queued until after the step.
- `isKinematic` (get: type == KINEMATIC; set: KINEMATIC/DYNAMIC), `isSleeping()`, `sleep()`, `wakeUp()`
  (= `!IsActive`, `DeactivateBody`, `ActivateBody`).
- Collider API (each marks the collider `dirty`; the next step rebuilds per D13 through
  `PS::syncRigidbodyColliders`, which today destroys the body, `PS:1414-1478`, and now builds the new
  compound and calls `SetShape(body, shape, false, Activate)` + re-applies mass properties):
  `removeCollider(index)` (later indices shift; their leaf user data and `colliderIndex` rewritten),
  `removeAllColliders()`, `setColliderPosition(i, Vector3)`, `setColliderRotation(i, Quaternion)`,
  `setColliderBoxSize(i, Vector3)`, `setColliderRadius(i, r)`, `setColliderHalfHeight(i, h)`,
  `setColliderTaperedRadii(i, top, bottom)`, `setColliderVertices(i, vertices[, indices])`
  (CONVEX_HULL / MESH, source becomes RAW_*), `setColliderTrigger(i, bool)`, `isColliderTrigger(i)`,
  `setColliderMaterial(i, PhysicsMaterial|path|nil)`, `getColliderMaterial(i)`,
  `setColliderFriction(i, f)`, `setColliderBounciness(i, b)` (D8), `getColliderFriction(i)`,
  `getColliderBounciness(i)` (resolved values).
- A body whose last collider is removed leaves the world (no shape) and comes back when one is added.

#### 6.1.8 `Joint3DComponent` (`engine/core/component/Joint3DComponent.h`)

New fields (legacy = value when the key is missing in an old file):

| field | default | legacy | types |
|---|---|---|---|
| `anchorSpace` | LOCAL | WORLD | all |
| `enableCollision` | false | **true** | all |
| `breakForce`, `breakTorque` | +∞ (written only when finite) | +∞ | all |
| `breakAction` | DESTROY | DESTROY | all |
| `useLimits` | false | PRISMATIC true, others false | HINGE, PRISMATIC |
| `limitsMin`, `limitsMax` (existing) | -45, 45 (HINGE deg) / 0, 0 | as stored | HINGE (deg), PRISMATIC (m) |
| `limitsSpringFrequency`, `limitsSpringDamping` | 0 (hard), 0 | 0, 0 | HINGE, PRISMATIC, DISTANCE (soft limits) |
| `useMotor` | false | false | HINGE, PRISMATIC, SWINGTWIST |
| `motorTargetVelocity` | 0 | 0 | deg/s (HINGE), m/s (PRISMATIC) |
| `motorMaxForce` | 1000 | — | N·m (HINGE, SWINGTWIST), N (PRISMATIC); 1000 not Unity's 0, so a motor moves when enabled |
| `useSpring` | false | false | HINGE, PRISMATIC (position drive), DISTANCE (soft min/max) |
| `springStiffness`, `springDamper` | 0, 0 | — | Unity `spring` / `damper` → Jolt `SpringSettings(StiffnessAndDamping)` |
| `springTargetPosition` | 0 | — | deg (HINGE) / m (PRISMATIC) |
| `maxFriction` | 0 | 0 | N·m (HINGE, SWINGTWIST) / N (PRISMATIC) |
| `minDistance`, `maxDistance` | -1, -1 (= rest length) | -1, -1 | DISTANCE |
| `coneHalfAngle` | 45 | **45** (bug fix: was 0, `PS:2370-2385`) | CONE |
| `swingType` | CONE | CONE | SWINGTWIST (CONE / PYRAMID) |
| `motorTargetRotation` | identity | — | SWINGTWIST position drive (constraint space) |
| `motorSpringFrequency`, `motorSpringDamping` | 20 Hz, 1 | — | SWINGTWIST drive |
| `motion[6]` (`xMotion`…`angularZMotion`) | FREE | FREE | SIXDOF: LOCKED / LIMITED / FREE |
| `linearLimitMin/Max`, `angularLimitMin/Max` | (0,0,0) | — | SIXDOF, used by LIMITED axes (m / deg) |
| `useDrive`, `driveStiffness`, `driveDamping`, `angularDriveStiffness`, `angularDriveDamping`, `driveMaxForce`, `driveMaxTorque`, `targetPosition`, `targetRotation`, `targetVelocity`, `targetAngularVelocity` | off / 0 / identity | — | SIXDOF (ConfigurableJoint drives) |

Creation (`PS:1060-1176` and loaders `PS:2149-2880`): LOCAL anchors/axes are converted to world space
with the bodies' current poses (anchor scaled like collider positions; `autoAnchors` → bodyB's point =
bodyA's world anchor), then the existing world-space settings are filled. Per type: HINGE
`mHasLimits`/`mLimitsMin/Max` (re-centred: rotate `mNormalAxis2` about the hinge axis by c = (min+max)/2
so the start pose reads −c, limits [min−c, max−c]; equal limits → ±0.01° with a warning, Jolt asserts
on equal limits without spring), `mLimitsSpringSettings`, `mMaxFrictionTorque`, `mMotorSettings`
(`mMaxTorqueLimit = ±motorMaxForce`; spring mode → `mSpringSettings = StiffnessAndDamping(k, c)`),
then `SetMotorState(Velocity | Position)`, `SetTargetAngularVelocity` / `SetTargetAngle` (rad);
PRISMATIC same with forces (WORLD legacy keeps today's range normalisation `PS:2414-2431`; LOCAL
re-centres by shifting `mPoint1` along the axis by c); DISTANCE `mMinDistance/mMaxDistance`,
`mLimitsSpringSettings`; CONE `mHalfConeAngle`; SWINGTWIST `mSwingType`, friction, motors
(`SetSwingMotorState/SetTwistMotorState(Position)`, `SetTargetOrientationCS`); SIXDOF
`MakeFixedAxis / MakeFreeAxis / SetLimitedAxis`, `mMotorSettings[axis]` + targets. When
`enableCollision == false`, the pair gets the `JOINT` reason in `JoltPairFilter3D`; removed on destroy.

Live setters (`Joint3D`): motor/spring targets and states, limits, friction → constraint setters
(`HingeConstraint::SetLimits`, `SetMotorState`, `SliderConstraint::SetTargetVelocity`, …) + wake
bodies; structural changes → `needUpdateJoint`.

Breaking (after `world3D.Update`, `PS:3141`): force = |linear λ| / dt, torque = |angular λ| / dt from
the type's `GetTotalLambda*` (Fixed: position Vec3 + rotation Vec3; Point: position; Hinge: position,
rotation, limits, motor; Slider: position (2), limits, rotation, motor; Distance: position; Cone:
position, rotation; SwingTwist: position, twist, swing Y/Z, motor; SixDOF: translation/rotation parts
and motors). Actions as physics-2d D6; callback `onJointBreak(force, joint)` (Unity's float first) to
the joint's GameObject and, if different, bodyA's, via `ScriptMessages` (physics-contacts §6.1.1).
Read-outs (Lua/C++): `currentForce`, `currentTorque` (Vector3, last step), `angle` / `velocity` (HINGE,
deg, deg/s: `GetCurrentAngle` + c), `position` / `speed` (PRISMATIC), `currentLength` (DISTANCE),
`broken`.

#### 6.1.9 Write-back of nested bodies (`PS:3151-3165`, `PS:666-721`, `PS:83-111`)

Process the bodies in the order of the Transform component array (hierarchy order: parents first).
Before writing a body's pose, bring its parent chain's world matrices up to date (walk up to the
nearest ancestor that is clean or was written this pass and recompute down with
`RenderSystem::updateTransform`, `resolveWorldTransform` at `PS:39-48` extended to "force" mode for
ancestors whose own ancestor was written); after writing, recompute its `modelMatrix` immediately.
Applies to the 2D write-back too. Interpolation (`PS:3169-3203`) uses the same order.

#### 6.1.10 Per-body interpolation (3D and 2D)

`RigidbodyComponent::interpolation`, `Rigidbody2DComponent::interpolation`. In `PS::interpolate`:
SCENE → the scene flag (today); NONE → pose at the step (alpha 1); INTERPOLATE → today's lerp/slerp;
EXTRAPOLATE → current pose + velocity · alpha · fixedStep (position) and the angular velocity
integrated for alpha · fixedStep (rotation), 2D likewise in points. Lua `interpolation` property on
both body classes (`RigidbodyInterpolation.SCENE/NONE/INTERPOLATE/EXTRAPOLATE`).

#### 6.1.11 Ragdoll (`engine/core/component/RagdollComponent.h`, `engine/core/subsystem/PhysicsSystem` + `engine/core/object/physics/Ragdoll.{h,cpp}`)

```cpp
struct RagdollComponent {
    bool enabled = true;
    bool simulated = false;          // serialized start state; runtime state
    Entity root = NULL_ENTITY;       // subtree holding the parts; none = this GameObject
    Entity animator = NULL_ENTITY;   // AnimationPlayer to pause while simulated; none = auto
    bool inheritVelocity = true;     // simulated starts with the follow velocities
    bool selfCollision = true;       // false: no part collides with another part
    float totalMass = 20;            // informational (wizard), not applied at runtime
    // runtime: parts (Rigidbody entities in root's subtree, root excluded), applied state, saved
    // animator enabled flag
};
```

- Parts: every `RigidbodyComponent` in `root`'s subtree except `root` itself, collected at load and
  when the hierarchy changes (`onComponentAdded/Removed` of Rigidbody under the root).
- Animated (`simulated = false`): parts KINEMATIC (D3 switching), gravity factor kept; they follow
  their bone Transforms with the existing kinematic follow (`PS:889-991`), so they push other bodies;
  the animator (auto = the first `AnimationPlayerComponent` on root, its ancestors, then its
  descendants) is enabled.
- Simulated: animator `enabled = false` (its previous value saved), parts DYNAMIC; with
  `inheritVelocity` they keep the follow velocity, else zero; the write-back (D11) moves the bones and
  the skin.
- Back to animated: parts KINEMATIC, velocities zeroed, animator restored; bones snap to the animated
  pose at the next update (Unity). Hook left: `Ragdoll::onBeforeAnimatedRestore` (no blending in v1).
- `selfCollision = false` adds the `RAGDOLL` reason for every part pair to `JoltPairFilter3D`.
- Lua: `GameObject:getRagdoll() -> Ragdoll|nil`; `ragdoll.simulated` (bool), `ragdoll:setSimulated(b)`,
  `ragdoll.partCount`, `ragdoll:getParts()` (table of Rigidbody), `ragdoll:applyImpulse(impulse,
  point)` (to the part nearest `point`, Unity's "hit bone" idiom), `ragdoll:applyForce(force)` (every
  part, mass-weighted), `inheritVelocity`, `selfCollision`.

#### 6.1.12 Debug draw (3D and 2D)

`engine/core/physics/PhysicsDebugLines.{h,cpp}`: the wireframe generation now in
`editor/render/SceneRender3D.cpp:673-1150` (box, sphere, capsule, tapered capsule, cylinder, hull and
mesh edges, heightfield grid) moves here and is used by both the editor gizmos and the runtime.
`PhysicsSystem::draw()` (`PS:3205`, empty today): when `Physics::debugDraw` is on, fills a
scene-owned `LineRenderer` on a system entity (`EntityManager::createSystemEntity`, invisible to the
Hierarchy and to saving): 3D colliders from the live body poses (green active dynamic, dark green
asleep, blue kinematic, grey static, yellow triggers, magenta joints: anchors, axes, limit arcs); 2D
through `b2World_Draw` with a `b2DebugDraw` filling the same renderer (shapes, joints, AABBs off);
`debugDrawContacts` adds contact points and normals (3D from physics-contacts' snapshots when present,
else from `b2Body_GetContactData` / last-step records). Static geometry edges are cached per collider
(rebuilt on shape change). Off: the renderer is hidden and nothing is computed.

#### 6.1.13 World capacity

Project setting `physics3D: {maxBodies: 8192, maxBodyPairs: 16384, maxContactConstraints: 8192}`
(`editor/Project.h` next to the layer matrices, `project.yaml`), applied with the static
`PhysicsSystem::setWorldCapacity3D(maxBodies, maxBodyPairs, maxContactConstraints)` before scenes are
created: editor (Project load / setting change; existing play scenes keep theirs until Play restarts)
and exported builds (`editor/Generator.cpp` emits the call in `main` next to the layer matrices,
`:2338-2347`). `PS:235-238` reads it. `world3D.Update` returns `EPhysicsUpdateError`: each of
`ManifoldCacheFull`, `BodyPairCacheFull`, `ContactConstraintsFull` logs once per scene with the setting
to raise; body creation failure (`PS:1530-1534`) names `maxBodies`. Acceptance check: per-world memory
at the defaults measured in a test and kept ≤ 4 MB on Linux, else lower defaults are chosen and
written here.

### 6.2 Serialization (`editor/Stream.cpp:8127-8320, 8368-8460`)

Body: every §6.1.2 field (always written); Collider: `isTrigger`, `material`, `receiver`, `vertices`,
`indices` (uint32); `numColliders` still written, read as in physics-2d §6.2. Joint: every §6.1.8
field, `anchorSpace` always written, break values only when finite, "legacy" column on missing keys.
Ragdoll: all fields. Scene settings unchanged; project settings `physics3D`.

### 6.3 Lua API

#### 6.3.1 Rigidbody (`GOL:2144-2240`)

New properties: `linearDamping`, `angularDamping`, `maxLinearVelocity`, `maxAngularVelocity`,
`automaticMass`, `mass` (set → `automaticMass = false`, persistent), `automaticCenterOfMass`,
`centerOfMass` (set → automatic false), `automaticInertiaTensor`, `inertiaTensor`,
`inertiaTensorRotation`, `isKinematic`, `interpolation`, `allowSleeping` (now persisted),
`material`; methods `resetCenterOfMass()`, `resetInertiaTensor()`, `isSleeping()`, `sleep()`,
`wakeUp()`, the collider API of §6.1.7, `getColliderCount()`. `type` setter follows §6.1.7 rules.
Errors: index out of range → Lua error; type switch refused → `Log::error` + false-y no-op (property
setters cannot return), `setType` returns `bool` in C++.

#### 6.3.2 PhysicsMaterial

```lua
local ice = PhysicsMaterial.load("materials/ice.physicsmaterial")   -- shared runtime copy, nil + error log if missing
local m = PhysicsMaterial.new{friction = 0, bounciness = 0.9, frictionCombine = PhysicsMaterialCombine.MINIMUM}
m.friction = 0.1                     -- properties: friction, bounciness, frictionCombine, bounceCombine; path (read-only, "" for new)
body:setColliderMaterial(0, m)       -- also accepts a path string or nil
```

`PhysicsMaterialCombine.AVERAGE/GEOMETRIC_MEAN/MINIMUM/MULTIPLY/MAXIMUM`. Same object usable by
`Rigidbody2D` (physics-2d).

#### 6.3.3 Joint3D (`GOL:2242-2262`) and Ragdoll

`GameObject:getJoint3D() -> Joint3D|nil` (named like physics-2d's `getJoint2D`). Properties = §6.1.8 field names plus Unity aliases: `connectedBody` (= `bodyB`),
`connectedAnchor` (= `anchorB` / LOCAL `anchor` partner), for SWINGTWIST `lowTwistLimit`,
`highTwistLimit` (= twist min/max), `swing1Limit` (= `planeHalfConeAngle`, rotation about the swing
axis `planeAxis`), `swing2Limit` (= `normalHalfConeAngle`), `swingAxis` (= `planeAxis`); read-outs
`currentForce`, `currentTorque`, `angle`, `velocity`, `position`, `speed`, `currentLength`, `broken`.
Enums `JointAnchorSpace`, `JointBreakAction`, `JointMotion.LOCKED/LIMITED/FREE`, `SwingType.CONE/PYRAMID`.
Ragdoll per §6.1.11. Callback: `function M:onJointBreak(force, joint) end`.

Statics: `Physics.debugDraw`, `Physics.debugDrawContacts` (bool, read/write).

### 6.4 Editor UX

- **Rigidbody Inspector** (`editor/window/InspectorWindow.cpp:12277-12824`), Unity order: Type;
  Mass (+ "Automatic" toggle; shows the computed mass greyed when automatic, via a body-less
  `computeMassProperties3D` preview); Linear / Angular Damping; Gravity Factor; Interpolation;
  Collision Detection (motion quality); Allow Sleeping; foldout "Advanced": Max Linear / Angular
  Velocity, Center of Mass (+ Automatic, gizmo sphere at the COM in the Scene view), Inertia Tensor and
  Rotation (+ Automatic), Contacts With Static/Kinematic, Default Material, legacy Friction /
  Restitution as "Surface without material", legacy body Is Trigger (only when set, with "Convert to
  collider triggers"). Collider rows: Is Trigger, Material (asset field, drag from the Project window),
  "Send Callbacks To". Changing Type in Play applies live through §6.1.7.
- **Joint Inspector** (`:12886-…`): Anchor Space (WORLD: "Convert to Local"); Anchor / Connected
  Anchor / Auto Configure; Axis / Secondary Axis; Enable Collision; per type "Limits", "Spring",
  "Motor", "Friction", "Cone", "Swing / Twist" (with Unity labels Low Twist, High Twist, Swing 1,
  Swing 2), "Drives" (SIXDOF: per-axis motion combo boxes, limits, drive stiffness/damping, targets);
  "Break" (Force, Torque with ∞, Action); Play: "Runtime" read-outs. Mutually exclusive motor/spring
  warning.
- **Scene view gizmos** (`editor/render/SceneRender3D.cpp`, joints `createOrUpdateJointLines`): anchor
  points, axis arrows, hinge limit arc, cone/swing-twist cones, slider limit bar; COM marker.
- **Physics Material assets**: Project window `FileType::PHYSICS_MATERIAL` (`editor/window/ProjectWindow.h:33-51`),
  icon, "Create > Physics Material" and "Create > Physics Material 2D" (D6 presets); Inspector asset
  view (four fields, undo via the asset document command like `.anim` documents); drag onto a
  collider's Material field or onto a GameObject (applies to all its colliders, one undo step).
- **Ragdoll wizard**: GameObject menu "3D Object > Ragdoll…" and Inspector context menu on a model
  root; a dialog (`editor/window/dialog/RagdollWizardDialog.{h,cpp}`) with entity slots Pelvis, Left/
  Right Hips, Left/Right Knee, Left/Right Foot, Left/Right Arm, Left/Right Elbow, Middle Spine, Head,
  auto-filled from the model's `.humanoid` mapping when present, else by name (Mixamo `mixamorig*Hips`,
  `*LeftUpLeg`, `*LeftLeg`, `*LeftFoot`, `*LeftArm`, `*LeftForeArm`, `*Spine1`/`*Spine`, `*Head`, and
  common Blender/Unity names); Total Mass (20), Flip Forward, Start As (Animated when the model has an
  Animation Player, else Simulated), Self Collision. "Create" = one undo step that (re)builds: removes
  Rigidbody/Joint components it created before on those bones (marked by the Ragdoll component's part
  list), then per bone (Unity `RagdollBuilder`, masses = density × totalMass / 16):

  | bone | parent | collider | radius scale | density | twist axis / swing axis | twist min/max | swing1 |
  |---|---|---|---|---|---|---|---|
  | Pelvis | — | box (breast boxes split at the spine) | — | 2.5 | — | — | — |
  | Hips L/R | Pelvis | capsule to knee | 0.3 | 1.5 | right / forward | -20 / 70 | 30 |
  | Knee L/R | Hips | capsule to foot | 0.25 | 1.5 | right / forward | -80 / 0 | 0 |
  | Middle Spine | Pelvis | box | — | 2.5 | right / forward | -20 / 20 | 10 |
  | Arm L/R | Middle Spine | capsule to elbow | 0.25 | 1.0 | up / forward | -70 / 10 | 50 |
  | Elbow L/R | Arm | capsule (mirrored length) | 0.20 | 1.0 | forward / up | -90 / 0 | 0 |
  | Head | Middle Spine | sphere r = arm span / 4 | — | 1.0 | right / forward | -40 / 25 | 25 |

  `swing2 = 0`; axes are the world directions (up = pelvis → head, right from the elbows, forward =
  right × up, negated with Flip Forward) converted to each bone's local frame and snapped to its nearest
  local axis; collider sizes are world sizes divided by the bone's world scale (bone collider units are
  mesh-local, `docs/mcp/guide/physics.md`). Joints: SWINGTWIST on the child bone, bodyA = child,
  bodyB = parent, LOCAL, anchor (0,0,0), `enableCollision` false. Bodies: KINEMATIC when Start As
  Animated, else DYNAMIC; `interpolation` INTERPOLATE; `automaticMass` false with the table masses.
  A `Ragdoll` component on the selected root. Missing optional bones (spine, head, arms) are skipped
  with a note in the dialog.

### 6.5 MCP / agent surfaces

- `edit_colliders`: 3D fields `is_trigger`, `material` per collider; body fields through `body`
  (`linear_damping`, `angular_damping`, `mass`, `center_of_mass`, `interpolation`, …: Catalog names);
  remove the 10-collider limit (`WorldTools.cpp:1357`) and the "friction/restitution per body" note.
- `set_joint`: shared args with physics-2d (`motor`, `spring`, `limits` 2D+3D, `break`,
  `enable_collision`, `anchor_space`), 3D `cone_half_angle`; new joints under `body_a`.
- New `edit_physics_material {path, create?: "3d"|"2d", friction?, bounciness?, friction_combine?,
  bounce_combine?}` → the resolved values (one undo step, creates the file when `create`).
- New `create_ragdoll {entity, bones?: {pelvis, left_hips, …: entity refs}, total_mass?, start?:
  "animated"|"simulated", self_collision?, flip_forward?}` → `{parts: [{bone, entity, collider, mass,
  joint}], skipped: [...], warnings}` (one undo step).
- `configure_game_view` gains `physics_debug` (bool) and `physics_debug_contacts`; `screenshot` of the
  Game view then shows the wireframes.
- `get_runtime_entity`: body read-outs (`sleeping`, `mass`, `center_of_mass_world`, velocities) and
  joint read-outs.
- `set_project_settings`: `physics3D.max_bodies` etc.
- Tool budget: the two new tools ≤ 900 characters together (`tools/mcp/tool_budget.py`); details in
  `docs/mcp/guide/physics.md`.

### 6.6 Exported-build path

`editor/Factory.cpp:2352-2420` (body/collider/joint/ragdoll emission with vectors; infinity as
`std::numeric_limits<float>::infinity()`); `RagdollComponent` registered in the component lists
(Catalog `ComponentType`, Factory dispatch `:1071-1075`, Stream, EntityCloner, the engine's component
registry); `.physicsmaterial` files packed with the other data assets (Exporter asset copy list: same
rule as `.anim`); `Generator.cpp` emits `setWorldCapacity3D`; delete `MAX_COLLIDERS_3D`,
`MAX_COLLIDER_VERTICES_3D`, `MAX_COLLIDER_INDICES_3D` (`Engine.h:136-146`, `Exporter.cpp:2923-2925`,
`Project.h:93-95`, `Project.cpp:3750-3760`, `Stream.cpp:3389-3391, 3553-3555`, `Catalog.cpp:1351`).
Debug draw uses `LineRenderer` (all platforms).

### 6.7 Docs

`docs/mcp/guide/physics.md` (Rigidbody fields table, type switching, triggers per collider, materials
and combine rule, joints with the Unity mapping table, ragdolls, debug draw, capacity),
`docs/mcp/guide/lua-api.md`, `docs/unity-glossary.md` (drag → `linearDamping`, `isKinematic`,
`PhysicMaterial` → `PhysicsMaterial` (`.physicsmaterial`), `HingeJoint` → Joint HINGE, `CharacterJoint` →
SWINGTWIST with the limit aliases, `SpringJoint` → DISTANCE with spring, `ConfigurableJoint` → SIXDOF,
`FixedJoint` → FIXED, Ragdoll Wizard → "3D Object > Ragdoll…", `OnJointBreak` → `onJointBreak`),
README (ragdolls, physics materials, joint motors).

## 7. Behaviour rules and edge cases

1. **Old scenes**: identical simulation (D1, legacy columns, body friction path); exceptions, both bug
   fixes: cone joints now swing up to 45°, nested dynamic bodies are written back correctly.
2. **Type switch during contact**: pairs that cease exist end via physics-contacts (queued Exit); a
   kinematic body following its Transform stops following when switched to dynamic (velocity kept).
3. **Mass of kinematic/static** bodies: computed and stored, unused by Jolt; switching to dynamic uses
   it immediately.
4. **Collider removed inside a joint's body**: shape swap keeps the joint (D13). Removing the last
   collider removes the body from the world; its joints are destroyed and rebuilt when it returns.
5. **Materials**: a missing asset path logs once and acts as no material; a material change applies at
   the next step to new and existing contacts: Jolt calls the combine functions every step, cached
   manifolds included (`Constraints/ContactConstraintManager.cpp:865, 1030, 1320`); the bodies using it
   are woken so sleeping contacts pick it up too.
6. **Joint limits**: hinge ranges > 360° clamp to ±180° around the centre with a warning; re-centring
   is reported in the Inspector ("limits measured from the start pose").
7. **Breaking**: per fixed step; with several steps per frame, a break can happen in any of them;
   destroying a body of a joint in `onJointBreak` is safe (after dispatch).
8. **Ragdoll**: switching in the same frame twice applies the last value at the next fixed step; a part
   with a mesh collider cannot become dynamic (wizard never makes one; error otherwise); parts
   reparented away leave the ragdoll at the next collection.
9. **Interpolation EXTRAPOLATE** on bodies hitting things can overshoot visually by up to one step
   (Unity documents the same).
10. **Debug draw** never affects simulation; lines are drawn in the scene's camera like any LineRenderer
    (they are occluded by geometry; a `Physics.debugDrawOnTop` is a non-goal for v1).
11. **Threading**: material values and the pair filter are mutated only between steps; listener code
    is read-only (§6.1.5, §6.1.6).
12. **Determinism**: wizard output depends only on the bone poses and settings (deterministic order:
    the table order); decomposition-free.
13. **Capacity change** affects scenes created afterwards.

## 8. Performance

- Shape swap instead of body rebuild: collider edits stop invalidating joints and contacts.
- Mass computation O(colliders) per rebuild. Manifold reduction is off only on multi-collider bodies
  that need exact per-collider data (cost: more contact points on those).
- Per-contact combine functions: one `DynamicCast` + reads per side (job threads).
- `mAllowDynamicOrKinematic` on static bodies: one `MotionProperties` (~150 bytes) each: 10 000 static
  bodies ≈ 1.5 MB (documented).
- Debug draw off: zero cost; on: O(collider edges) per frame, static edges cached.
- World capacity defaults: measured per-world memory (§6.1.13).

## 9. Tests (`editor/tests/Physics3DBodyTest.cpp`, `Physics3DJointsTest.cpp`, `PhysicsMaterialTest.cpp`, `RagdollTest.cpp`)

Fixed step 1/60, `PhysicsTestScene.h` harness.

- T1 Legacy: a YAML body/collider/joint from today's format decodes to Jolt settings equal to today's
  (damping 0.05, friction 0.2 combine sqrt, enableCollision true, no limits) and 120 steps reproduce a
  recorded fixture (±1e-5); cone joint is the documented exception (swing reaches 45° ±1°).
- T2 Damping: `linearDamping = 1`, gravity 0, v0 = 10, 60 steps of 1/60 s: v = 10·(1 − 1/60)^60 ≈ 3.65
  ±1% (Jolt multiplies the velocity by `max(0, 1 − c·dt)` each step); `angularDamping` likewise; the
  values survive a collider edit and save/load.
- T3 Mass/COM/inertia: `automaticMass = false, mass = 3` → `mass == 3`, inertia scaled; COM (0, -0.5, 0)
  on a box makes it self-right (tilted 60°, settles upright on a floor within 3 s); `inertiaTensor`
  (1, 100, 1) resists spin about y (angular velocity after an angular impulse is 1/100 of x's);
  values survive save/load and collider edits.
- T4 Type switching: static → dynamic (falls), dynamic → kinematic (stops, keeps following when
  moved), kinematic → static, static → kinematic, 100 random switches in Play: no Jolt assert, ids
  stable; a mesh-collider body refuses dynamic with the error.
- T5 Triggers per collider: a body with a solid box and a trigger sphere: rests on the floor via the
  box, the sphere passes through a wall; mass equals the box's alone; kinematic body with a trigger
  detects a static box (with physics-contacts: `onTriggerEnter`; without it: global
  `onContactAdded3D` with `sensor == true`).
- T6 Materials: friction 0 Minimum vs friction 1 on a 30° slope slides; Multiply 2·0.5 = 1 sticks;
  bounciness 1 Maximum bounces to ≥ 90% height; no materials → legacy values exactly; priority table
  test of `combineSurface` (all 25 mode pairs); asset round trip (canonical write, lenient parse,
  Unity aliases); runtime `PhysicsMaterial.load` edit affects every collider using it; instancing
  (`setColliderFriction` on one collider leaves another sharing the asset unchanged).
- T7 Collider edits: `setColliderRadius` on a resting ball keeps body id, joint, sleeping state off,
  no Exit/Enter (with physics-contacts); `removeCollider(0)` of 3 shifts indices; a 20 000-index mesh
  collider built from Lua in an engine-only (non-editor) scene.
- T8 Hinge: limits 10..50 (re-centred) on a door released at 0 → ends within [10, 50] ±1°; spring
  (stiffness 50, damper 5, target 0) returns to 0 ±1° in 3 s; motor 90 deg/s, max force 1000 → speed
  90 ±2%; `angle` read-out matches the bodies' relative rotation.
- T9 Slider limits + motor; distance min/max with spring (soft rope); swing-twist: rotation about
  `planeAxis` stops at `swing1Limit` (= `planeHalfConeAngle`), about the normal at `swing2Limit`, twist
  within [low, high] (pins the alias mapping); six-DOF LOCKED/LIMITED/FREE per axis; cone 45°.
- T10 Breaking: 100 kg box on a fixed joint with `breakForce = 500` breaks on the first step:
  `onJointBreak(force ≥ 500, joint)` once, component removed; DISABLE/IGNORE/CALLBACK_ONLY as in
  physics-2d T7.
- T11 Local anchors: a hinge door prefab instantiated at two places swings identically (relative poses
  equal ±1e-4); `enableCollision = false` → overlapping connected boxes do not push apart; true → they do.
- T12 Nested write-back: chain body (dynamic) → child without body → grandchild body (dynamic) joined by
  point joints, 200 steps: every body Transform world pose equals its Jolt pose ±1e-4 each step
  (fails on today's code: regression for D11).
- T13 Ragdoll: an in-memory 11-bone humanoid skeleton (Transforms + a skinned test mesh with one
  vertex weighted to each bone) through the wizard function (editor-free core in
  `engine/core/physics/RagdollBuilder.{h,cpp}`, the dialog only gathers inputs): 11 bodies, 10 joints
  with the table limits, masses summing to 20 ±1e-4; animated: parts kinematic and following an
  animated bone (position error < 1e-3 after the step); `simulated = true`: falls and comes to rest
  on a floor within 5 s, joint limits respected (every child-parent relative angle within limits +2°),
  the skinned vertex positions follow their bones (bonesMatrix of the mesh equals bone modelMatrix ×
  inverse bind ±1e-4); back to animated: kinematic again, AnimationPlayer re-enabled.
- T14 Interpolation per body: NONE body draws at step poses with the scene setting on; EXTRAPOLATE
  draws ahead by v·alpha·dt; 2D twin.
- T15 Caps gone + capacity: 9000 bodies with capacity 10 000 created; with capacity 100 the 101st
  logs the setting name; contact constraint overflow logs `maxContactConstraints` once; exported
  CMake has no `MAX_COLLIDER*_3D`.
- T16 Debug draw: with `Physics.debugDraw = true` the system LineRenderer has > 0 lines for a scene
  with a box, 0 lines and hidden when off; 2D scene too.
- T17 Serialization round trip of every new field, Factory code compiled in the engine test target.
- MCP e2e `tools/mcp/tests/test_physics_3d.py`: `edit_physics_material` create + `edit_colliders
  material` + `get_entity`; `create_ragdoll` on the test humanoid fixture (`tools/mcp/tests/fixtures`)
  → 11 parts; `set_joint` hinge with limits/motor; `configure_game_view physics_debug` + `screenshot`
  in Play returns an image (non-empty lines count reported).

## 10. Acceptance criteria

1. All tests pass (editor tests, engine tests, MCP e2e); Android (Gradle/NDK) and Web (emsdk) builds
   compile with the new code.
2. BACK-MAN's entity can be rebuilt with the wizard from the Mixamo rig and toggled with
   `ragdoll.simulated = true` in Play and in an exported Linux build, skin following the bones.
3. No `MAX_COLLIDER*` macros remain; `grep numColliders` empty.
4. Old scenes simulate identically except the two documented fixes (T1, T12).
5. Stubs regenerated (`-- unresolved:` empty), `docs/mcp/TOOLS.md` regenerated, budget within limit.

## 11. Integration points

`engine/core/component/RigidbodyComponent.h`, `Joint3DComponent.h`, new `RagdollComponent.h`,
`PhysicsCommon.h`; `engine/core/Engine.h:136-146`; `PS:83-111` (write-back helpers), `:204-266`
(world init, capacity, combine functions, pair filter), `:666-721`, `:889-1015` (follow), `:1060-1176`
(joint dispatch), `:1352-1478` (collider build / sync → SetShape), `:1483-1551` (body creation),
`:2149-2890` (joint loaders), `:3108-3203` (3D step, breaking, write-back order, interpolation),
`:3205` (draw); `RB` (type, mass, colliders, friction `:613-641`, collision group `:593-602`);
`engine/core/object/physics/Joint3D.{h,cpp}`; `JPA:75-389` (layers, listener, pair filter); `JSB:125`
(material parameter), `JSB:295-308`; `engine/core/subsystem/RenderSystem.cpp:13171-13184, 13796-13810`
(skinning, read-only); `GOL:313-372` (enums), `:2144-2262`; `engine/core/script/binding/CoreClassesLua.cpp:544-575`
(`Physics.debugDraw`); `editor/Stream.cpp:8127-8460`; `editor/Factory.cpp:1071-1075, 2352-2420`;
`editor/Catalog.cpp:1340-1400` (collider paths) and the Joint3D/Rigidbody property lists;
`editor/Exporter.cpp:2886-2926`; `editor/Generator.cpp:2338-2347`; `editor/Project.{h,cpp}`
(settings, maxValues); `editor/window/InspectorWindow.cpp:12277-12990`; `editor/window/ProjectWindow.h:33-51`;
`editor/render/SceneRender3D.cpp:673-1150` (moved to `PhysicsDebugLines`); `editor/mcp/tools/WorldTools.cpp:1330-1600, 4616-4655`,
`ViewTools.cpp` (`configure_game_view`), `PlayTools.cpp`; `engine/core/object/environment/Water.cpp`
and `engine/core/physics/PlacementWorld.cpp` (collider storage users).

## 12. Cross-feature dependencies

- **physics-contacts**: requires §6.1.5 (trigger step), §6.1.6 (pair filter for `Physics.ignoreCollision`),
  D5, D13 (invisible rebuilds), the manifold-reduction rule; provides `ScriptMessages` for
  `onJointBreak` (if this branch merges before physics-contacts and script-lifecycle, implement the
  minimal `ScriptMessages` of physics-contacts §6.1.1 with `JointBreak` only) and the packed user data.
- **physics-2d**: consumes `PhysicsMaterial`, `PhysicsCommon.h`, interpolation and debug draw for 2D;
  shares the `set_joint` argument shapes and the anchor-space concept; parallel collider-storage
  refactor (merge conflicts in `Engine.h`, `Exporter.cpp`, `Project.*`, `Stream.cpp`, `Factory.cpp`:
  whoever merges second resolves).
- **anim-layers** / **anim-retarget**: the Ragdoll pauses the whole AnimationPlayer; per-layer
  blending of ragdoll and animation is a future extension (hook in §6.1.11).
- **lines-trails**: debug draw lines use whatever `LineRenderer` width support exists (1 px today).
- **persistence**: a ragdoll on a persistent object keeps its components (nothing scene-specific).
