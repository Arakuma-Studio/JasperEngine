# `physics-2d`: 2D joints with every parameter, runtime-sized and concave colliders, 2D body fields, 2D materials (design)

Status: binding spec for branch `gaps/physics-2d` (ledger #7). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (shared conventions binding) and
[GAP-REPORT.md §4, §10](../../engine-gaps/GAP-REPORT.md). Siblings:
[physics-contacts](2026-10-31-physics-contacts-design.md) (owns every contact callback, including 2D
"stay"), [physics-3d](2026-10-31-physics-3d-design.md) (owns the shared `PhysicsMaterial` asset, the
per-body interpolation enum and the runtime debug draw, for 2D too).

Abbreviations: `PS` = `engine/core/subsystem/PhysicsSystem.cpp`, `RB2` =
`engine/core/object/physics/Rigidbody2D.cpp`, `GOL` = `engine/core/script/binding/GameObjectClassesLua.cpp`,
`B2` = `engine/libs/box2d` (Box2D **3.1.0**, `src/core.c:71`, single-threaded world: no task system).

## 1. Goals

1. Every 2D joint parameter Box2D offers as a serialized `Joint2DComponent` field, editable in the
   Inspector, from Lua and MCP, emitted to exported builds: wheel suspension (frequency, damping,
   axis), translation limits, motors (enable, speed, max torque/force), revolute limits and motor,
   distance spring/limits/motor, prismatic limits/motor/spring, weld stiffness, motor-joint offsets and
   limits, mouse-joint stiffness and force.
2. Unity-style **local anchors** so a joint inside a prefab works wherever the prefab is placed.
3. Break force / torque with Unity's `breakAction` and an `onJointBreak2D(joint)` script callback;
   joint read-outs (reaction force/torque, angle, speed, translation, motor torque/force).
4. No editor-only or export-only caps: any number of colliders per body and of points per
   polygon/chain, the same in the editor and on every device (today `MAX_COLLIDERS_2D` 10 and
   `MAX_COLLIDER_POINTS_2D` 16 with a `uint8_t` count, `engine/core/Engine.h:128-134`, sized from
   authored scenes in exports, `editor/Exporter.cpp:2921-2922`).
5. Concave / many-point polygon colliders (Unity `PolygonCollider2D`): automatic convex decomposition
   into Box2D polygons (Box2D polygons are convex, max 8 vertices, `B2/include/box2d/collision.h:24`).
6. Cheap runtime collider edits from Lua: rebuild only the collider that changed.
7. 2D body properties as component fields (gravity scale, damping, fixed rotation, continuous
   detection, sleeping mode, explicit mass / centre of mass / inertia, default material): today they are
   live-only Box2D calls, lost on reload and absent from the Inspector.
8. 2D physics materials (friction, bounciness, combine modes) using physics-3d's shared asset.
9. `contactEvents` on by default for new colliders (Unity behaviour), 2D sub-steps as a scene setting.

## 2. Non-goals

- Polygon colliders with holes or several paths in one collider (use several colliders), Unity
  `CompositeCollider2D`, `EdgeCollider2D` edge radius, effectors, `RelativeJoint2D` /
  `FrictionJoint2D` / `TargetJoint2D` / `SpringJoint2D` as separate types (they map onto MOTOR, MOUSE
  and DISTANCE with spring, §5 D3).
- Pulley and gear joints (not in Box2D 3.1).
- Patching Box2D's broad phase so kinematic sensors detect static bodies (physics-contacts §7.6).
- Contact callbacks (physics-contacts), per-body interpolation and debug draw (physics-3d).
- Unity scene import.

## 3. Users and stories

- Porting the hill climb car (`Prefabs/H - Hill Climb Racing/PlayerCar.prefab`): two wheel joints on
  the car body, anchors (1.153, 0.31) / (-1.444, 0.31) local to the car, suspension frequency 3, damping
  0.35, angle 90, motor off, max motor torque 10000, enable collision off, break infinite; 13 bodies
  with mass 1 (`useAutoMass: 0`), angular drag 0.05, continuous detection, interpolation on the car;
  tire material friction 2 (`PhysicsMats/TireFriction.physicsMaterial2D`), body material friction 0;
  wheels driven by `AddTorque` from FixedUpdate (`Scripts/Minigames/Hill/DriveCar.cs:103-113`); at
  runtime `HCarAppereance.cs:23-47` swaps each joint's `connectedBody` and `anchor` and the body box
  `offset`/`size`.
- Same game: static terrain made of two 226-point `PolygonCollider2D` paths (`InfiniteTerrain.prefab`,
  closed shapes including the box bottom), moved by transform to leapfrog.
- Gameplay programmer: a rope bridge (revolute joints with limits) whose planks break under load
  (`breakForce`, `onJointBreak2D` plays a sound).
- Agent: "make the wheel suspension softer" → `set_joint {joint: "FrontWheelJoint", properties:
  {frequency: 2}}`; "let the car drive itself" → `useMotor = true, motorSpeed = -720`.
- Agent: rebuilds a procedural terrain chain every few seconds from Lua with 500 points.

## 4. Research summary

- Unity `Joint2D` (`connectedBody`, `enableCollision` false, `breakForce`/`breakTorque` Infinity,
  `breakAction` Ignore/CallbackOnly/Disable/Destroy, default Destroy, `reactionForce`,
  `reactionTorque`, `OnJointBreak2D(Joint2D)`), `AnchoredJoint2D` (`anchor`, `connectedAnchor`,
  `autoConfigureConnectedAnchor` true), `WheelJoint2D` (`suspension {dampingRatio 0.7, frequency 2,
  angle 90}`, `useMotor`, `motor {motorSpeed deg/s, maxMotorTorque 10000}`, `jointTranslation`,
  `jointLinearSpeed`, `jointSpeed`, `jointAngle`, `GetMotorTorque`), `HingeJoint2D` (`useLimits`,
  `limits {min, max}` deg, `useMotor`, `motor`, `jointAngle`, `jointSpeed`, `limitState`,
  `referenceAngle`), `SliderJoint2D`, `DistanceJoint2D` (`autoConfigureDistance`, `distance`,
  `maxDistanceOnly`), `SpringJoint2D` (`frequency`, `dampingRatio`), `FixedJoint2D` (`frequency`,
  `dampingRatio`), `RelativeJoint2D` (`maxForce`, `maxTorque`, `correctionScale` 0.3,
  `autoConfigureOffset`, `linearOffset`, `angularOffset`), `TargetJoint2D`.
  <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Joint2D.html>,
  <https://docs.unity3d.com/2022.3/Documentation/Manual/class-WheelJoint2D.html>,
  <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/JointBreakAction2D.html>.
  Take: field set and names (flattened, lowerCamelCase), break semantics, local anchors, defaults.
  Leave: separate component per joint type (one `Joint2DComponent` with a `type`, as today).
- Unity `PolygonCollider2D` is concave, multi-path, decomposed internally; `EdgeCollider2D` takes 2+
  points. Take concave + any point count, 2-3 point edges. Leave holes/multi-path.
- Unity `PhysicsMaterial2D` (2022.3: friction 0.4, bounciness, no combine; Unity 6 adds
  `frictionCombine` default Mean and `bounceCombine` default Maximum, priority Maximum > Minimum >
  Multiply > Mean > Average), `Rigidbody2D.sharedMaterial` for colliders without one.
  <https://docs.unity.com/en-us/engine/6000.0/script-reference/unityengine/physicsmaterialcombine2d>.
  Take: per-collider combine modes (shared rule with 3D, physics-3d §5 D7). Leave: Unity 6's 2D
  priority order (one rule for both dimensions).
- Unity `Rigidbody2D` (`bodyType`, `useAutoMass`, `mass`, `linearDrag`, `angularDrag` 0.05,
  `gravityScale`, `collisionDetectionMode`, `sleepMode`, `centerOfMass`, `inertia`, `freezeRotation`).
- Box2D 3.1 (vendored): joint defs `B2/include/box2d/types.h:504-903` (wheel default 1 Hz / 0.7,
  `B2/src/joint.c:83-92`; motor joint maxForce 1, maxTorque 1, correction 0.3, `:33-41`; mouse
  4 Hz / 1 / maxForce 1, `:43-51`), live setters `box2d.h:761-1189`, `b2Joint_GetConstraintForce/
  Torque` (`box2d.h:743-746`), no built-in breaking, `b2NullJointDef`, world-level mixing rule only
  (`types.h:67-113`, `contact.c:337-338`), contact friction fixed at contact creation, `b2Shape_SetPolygon`
  in place, chains need ≥ 4 points and cannot be edited in place.
- Godot `PolygonShape2D` / `CollisionPolygon2D` decomposes concave polygons into convex parts
  (build mode Solids) or uses segments (Segments). Same choice as ours (POLYGON = solids, CHAIN =
  segments).

## 5. Product decisions

- **D1 One component, flat fields.** `Joint2DComponent` keeps its `type`; every parameter is a flat
  field (Catalog, Inspector, Lua and MCP need no nested structs). Fields that do not apply to the type
  are kept but hidden.
- **D2 Local anchors.** New field `anchorSpace` (`WORLD` | `LOCAL`). New joints default `LOCAL` (Unity):
  `anchorA` in bodyA's local space, `anchorB` in bodyB's local space, `axis` in bodyA's frame, all in
  GameObject-local 2D units like collider points (scaled by the transform scale). Scenes saved before
  this feature have no `anchorSpace` key and load as `WORLD`, keeping today's world-point semantics.
  bodyA = Unity's "this" (the joint's Rigidbody), bodyB = `connectedBody`.
- **D3 Unity mapping.** WheelJoint2D → WHEEL, HingeJoint2D → REVOLUTE, SliderJoint2D → PRISMATIC,
  DistanceJoint2D → DISTANCE (`maxDistanceOnly` = `rope`), SpringJoint2D → DISTANCE with
  `enableSpring`, FixedJoint2D → WELD, RelativeJoint2D → MOTOR, TargetJoint2D → MOUSE (bodyB unused,
  the target is a world point). FrictionJoint2D: MOTOR with `maxForce`/`maxTorque` and
  `correctionFactor = 0` (documented recipe).
- **D4 Units.** Lengths, anchors, translations, limits: points (like positions; ÷
  `pointsToMeterScale2D` to Box2D). Angles: the engine default angle unit (`Angle::defaultToRad`,
  as `Rigidbody2D.angle`). Angular speeds: default unit per second. Linear speeds: points/s. Forces
  N, torques N·m, frequencies Hz (Box2D units, not scaled; like `applyForce`).
- **D5 Defaults.** New components get Unity's defaults (wheel 2 Hz / 0.7 / axis (0, 1) / max torque
  10000; hinge limits -90..90 off; break Infinity / Destroy; enable collision off). Missing keys in
  old files decode to the values Box2D used before (table in §6.1.2 "legacy"), so old scenes behave
  identically.
- **D6 Breaking.** Checked after every 2D step for joints with finite `breakForce` or
  `breakTorque`, against `|b2Joint_GetConstraintForce|` and `|b2Joint_GetConstraintTorque|`. Unity's
  four actions. Callback `onJointBreak2D(joint)` to the joint's GameObject and, if different, bodyA's
  GameObject (Unity's joint lives on its Rigidbody), via `ScriptMessages` (physics-contacts §6.1.1).
- **D7 Storage without caps.** `Rigidbody2DComponent::colliders` and `Collider2D::points` become
  `std::vector`; `numColliders` / `numVertices` go away (`size()` is the count). `MAX_COLLIDERS_2D` and
  `MAX_COLLIDER_POINTS_2D` are deleted from `Engine.h`, the exporter and every editor check.
- **D8 Concave polygons.** `POLYGON` accepts any simple polygon with ≥ 3 points (concave allowed):
  convex with ≤ 8 points → one Box2D polygon; otherwise a deterministic ear-clipping triangulation
  merged into convex pieces of ≤ 8 vertices (Hertel-Mehlhorn), each a Box2D shape reporting the same
  collider index. Self-intersecting → error + the convex hull. Docs recommend `CHAIN` for terrain that
  wheels roll over (no internal seams; one-sided).
- **D9 Short chains.** `CHAIN` with 2 or 3 points and `loop = false` becomes segment shapes (Unity
  `EdgeCollider2D` takes 2+ points); ≥ 4 → Box2D chain.
- **D10 Per-collider rebuild.** A collider edit rebuilds that collider only; the body, its velocity,
  other shapes, joints and contacts of other colliders stay. Convex polygon edits with the same piece
  count use `b2Shape_SetPolygon` in place. Mass is re-applied once per body after a batch.
- **D11 2D body fields** (D5 legacy-safe): `gravityScale`, `linearDamping`, `angularDamping`,
  `fixedRotation`, `collisionDetection` (DISCRETE / CONTINUOUS = Box2D bullet), `sleepingMode`
  (NEVER_SLEEP / START_AWAKE / START_ASLEEP), `useAutoMass` + `mass`, `autoCenterOfMass` +
  `centerOfMass`, `autoInertia` + `inertia`, `material`. Live Lua setters also write the field.
- **D12 Materials.** Collider `material` (asset path) > body `material` > the collider's inline
  `friction` / `restitution` (today's fields, legacy combine = Box2D's geometric-mean friction, max
  restitution). Combine modes per the shared rule of physics-3d §5 D7, implemented with a small,
  marked patch to vendored Box2D that mirrors upstream Box2D 3.1's API
  (`b2World_SetFrictionCallback`, `b2World_SetRestitutionCallback`, `userMaterialId`).
- **D13 `contactEvents` default true** for colliders created from now on (Inspector, MCP, Lua
  `create*Collider`); files keep their stored value (always written today; missing → false). This is a
  deliberate change for runtime-created colliders (the global `beginContact2D` sees them): noted in
  the changelog/README. Per-object callbacks never depend on the flag (physics-contacts D16).
- **D14 Sub-steps.** Scene setting `physics2DSubSteps` (default 4 = today's `PS:3036`), 1-16.

## 6. Design

### 6.1 Runtime C++

#### 6.1.1 Colliders (`engine/core/component/Rigidbody2DComponent.h`)

```cpp
struct Collider2D {
    std::vector<b2ShapeId> shapes;      // runtime: the Box2D shapes (several for a decomposed polygon)
    b2ChainId chain = b2_nullChainId;   // runtime
    Collider2DType type = Collider2DType::POLYGON;
    Vector2 pointA, pointB; float radius = 0;
    std::vector<Vector2> points;        // was HybridArray vertices + uint8_t numVertices
    bool loop = false;
    float density = 1, friction = 0.6f, restitution = 0;
    std::string material;               // PhysicsMaterial asset path, "" = none (D12)
    bool sensor = false;
    bool enableHitEvents = false, contactEvents = true /* D13 */, preSolveEvents = false, sensorEvents = false;
    uint16_t categoryBits = 1, maskBits = 0xFFFF; int16_t groupIndex = 0;
    Entity receiver = NULL_ENTITY;      // physics-contacts D9
    bool dirty = true;                  // runtime: rebuild this collider at the next sync
    uint64_t decompositionKey = 0;      // runtime: hash(points, scale, radius) of `pieces`
    std::vector<std::vector<b2Vec2>> pieces; // runtime cache of the decomposition (Box2D units)
};
```

`Rigidbody2DComponent::colliders` → `std::vector<Collider2D>`; remove `numColliders`. Every user
(`grep numColliders|numVertices|\.vertices\[` in `engine/core`, `editor`: PhysicsSystem, Rigidbody2D,
Ray, EntityCloner, Catalog, Project, Factory, Stream, McpValues, WorldTools, SceneRender2D,
InspectorWindow, tests) switches to `size()`, `push_back`, `erase`. `getBox2DShape(index)` returns
`shapes.empty() ? b2_nullShapeId : shapes[0]`; new `getBox2DShapes(index)`.

Shape user data stays the collider index for every piece; when a collider is removed, the indices of
later colliders shift: their shapes' user data are rewritten (`b2Shape_SetUserData`) in the same sync.

#### 6.1.2 `Joint2DComponent` (`engine/core/component/Joint2DComponent.h`)

| field | type | new default | legacy (key missing) | used by |
|---|---|---|---|---|
| `anchorSpace` | `JointAnchorSpace {WORLD, LOCAL}` (shared, `PhysicsCommon.h`) | LOCAL | WORLD | all |
| `anchorA`, `anchorB` | Vector2 | (0,0) | as today | all but MOUSE/MOTOR |
| `autoAnchors` | bool | true | true | LOCAL: anchorB computed from anchorA's world point at creation (Unity autoConfigureConnectedAnchor); WORLD: today's meaning (DISTANCE: body origins) |
| `axis` | Vector2 | (0,1) | as stored ((0,0) → (1,0) + warning, today's rule) | WHEEL, PRISMATIC |
| `enableCollision` | bool | false | false | all (`collideConnected`) |
| `breakForce`, `breakTorque` | float | +∞ (key written only when finite) | +∞ | all |
| `breakAction` | `JointBreakAction {IGNORE, CALLBACK_ONLY, DISABLE, DESTROY}` | DESTROY | DESTROY | all |
| `useLimits` | bool | false | false | REVOLUTE, PRISMATIC, WHEEL |
| `lowerAngle`, `upperAngle` | float (deg) | -90, 90 | 0, 0 | REVOLUTE (clamped to [-180, 180], lower ≤ upper) |
| `lowerTranslation`, `upperTranslation` | float (points) | 0, 0 | 0, 0 | PRISMATIC, WHEEL |
| `useMotor` | bool | false | false | REVOLUTE, PRISMATIC, WHEEL, DISTANCE |
| `motorSpeed` | float | 0 | 0 | deg/s (REVOLUTE, WHEEL), points/s (PRISMATIC, DISTANCE) |
| `maxMotorTorque` | float N·m | 10000 | 0 | REVOLUTE, WHEEL |
| `maxMotorForce` | float N | 10000 | 0 | PRISMATIC, DISTANCE |
| `enableSpring` | bool | WHEEL true, else false | WHEEL true, else false | WHEEL (suspension), REVOLUTE, PRISMATIC, DISTANCE |
| `frequency` | float Hz | WHEEL 2, MOUSE 5, else 1 | WHEEL 1, MOUSE 4, else 0 | spring / suspension / mouse stiffness |
| `dampingRatio` | float | WHEEL 0.7, MOUSE 1, else 0.5 | WHEEL 0.7, MOUSE 1, else 0 | idem |
| `length` | float points | 0 | 0 | DISTANCE, used when `!autoLength` |
| `autoLength` | bool | true | true | DISTANCE: length = anchor distance at creation |
| `minLength`, `maxLength` | float points | 0, 0 (0 = length) | 0, 0 | DISTANCE with `useLimits` |
| `rope` | bool | false | as stored | DISTANCE (Unity maxDistanceOnly: limit [0, length], today's code `PS:1890-1896`) |
| `linearFrequency`, `angularFrequency` | Hz | 0, 0 (rigid) | 0, 0 | WELD |
| `linearDampingRatio`, `angularDampingRatio` | | 0, 0 | 0, 0 | WELD |
| `linearOffset` | Vector2 points | (0,0) | (0,0) | MOTOR, in bodyA's frame, used when `!autoOffset` |
| `angularOffset` | float deg | 0 | 0 | MOTOR |
| `autoOffset` | bool | true | false | MOTOR: offsets = current relative pose at creation (Unity autoConfigureOffset) |
| `maxForce` | float N | MOTOR 10000, MOUSE 0 | MOTOR 1, MOUSE 0 | MOTOR; MOUSE (0 = today's `max(10, 1000·mass)`, `PS:2028`) |
| `maxTorque` | float N·m | 10000 | 1 | MOTOR |
| `correctionFactor` | float 0..1 | 0.3 | 0.3 | MOTOR |
| `target` | Vector2 world points | (0,0) | as today | MOUSE |

Runtime-only (not serialized): `b2JointId joint`, `needUpdateJoint`, `referenceAngle` (captured),
`breakArmed` (re-armed when force drops below the threshold, for CALLBACK_ONLY), `broken`.

Creation (`PS::loadJoint2D` and the per-type loaders `PS:1863-2138`): LOCAL anchors →
`localAnchorA = toBox2D(scale(anchorA))` directly (no `b2Body_GetLocalPoint` of a world point);
`autoAnchors` → `localAnchorB = b2Body_GetLocalPoint(B, b2Body_GetWorldPoint(A, localAnchorA))`; axis
→ `localAxisA = normalize(axis)`; then copy every field into the def (`enableSpring`, `hertz`,
`dampingRatio`, `enableLimit`, limits ÷ ptm, `enableMotor`, `motorSpeed` (angular → rad/s, linear
÷ ptm), `maxMotorTorque/Force`, `collideConnected`, weld hertz/ratios, motor offsets (auto: from the
current relative pose), mouse `hertz`, `dampingRatio`, `maxForce`). The legacy table guarantees the
defs equal what `b2Default*JointDef` + today's code produce for old components (test T1).

Live updates (`Joint2D` setters): motor/spring/limit/enable values go straight to Box2D
(`b2RevoluteJoint_SetMotorSpeed`, `b2WheelJoint_SetSpringHertz`, `b2WheelJoint_EnableLimit`,
`b2Joint_SetCollideConnected`, `b2MotorJoint_SetLinearOffset`, `b2MouseJoint_SetTarget`, …, every one
in `box2d.h:761-1189`) and wake the bodies (`b2Joint_WakeBodies`); type, bodies, anchors, axis,
`length` with `autoLength`, `anchorSpace` → `needUpdateJoint = true` (rebuilt before the next step).

Breaking (in `PS::fixedUpdate` after `b2World_Step`, `PS:3038`, before events): for each joint with a
finite threshold: `F = |b2Joint_GetConstraintForce|`, `T = |b2Joint_GetConstraintTorque|`; if `F >
breakForce || T > breakTorque` and armed: IGNORE → nothing; CALLBACK_ONLY → callback, disarm;
DISABLE → `enabled = false` (the joint leaves the world through the existing rule `PS:2990-3001`),
callback; DESTROY → callback, then `scene->removeComponent<Joint2DComponent>` after the event dispatch
of this step. `broken` is set for DISABLE/DESTROY. Callbacks queue into the physics-contacts dispatch
order (after contacts, physics-contacts D10).

Read-outs (C++ `Joint2D` getters, live joint only, else 0): `getReactionForce()` (Vector2 N),
`getReactionTorque()`, `getJointAngle()` (REVOLUTE `b2RevoluteJoint_GetAngle`; WHEEL relative body
angle − reference), `getJointSpeed()` (angular, deg/s: REVOLUTE/WHEEL relative angular velocity),
`getJointTranslation()` (PRISMATIC `b2PrismaticJoint_GetTranslation`; WHEEL: dot(pB − pA, worldAxis)
in points), `getJointLinearSpeed()` (PRISMATIC `GetSpeed`, WHEEL along the axis, points/s),
`getMotorTorque()` / `getMotorForce()` (`b2RevoluteJoint_GetMotorTorque`, `b2WheelJoint_GetMotorTorque`,
`b2PrismaticJoint_GetMotorForce`, `b2DistanceJoint_GetMotorForce`), `getCurrentLength()` (DISTANCE),
`getLimitState()` (`INACTIVE`, `LOWER`, `UPPER`, `EQUAL`: from the current value vs limits ± slop).

#### 6.1.3 Polygon decomposition (`engine/core/physics/PolygonDecomposition2D.{h,cpp}`)

Pure functions (any thread), deterministic for the same input:

```cpp
struct Decomposition2D { std::vector<std::vector<Vector2>> pieces; bool convexInput; bool selfIntersecting; int droppedPieces; };
Decomposition2D decomposePolygon2D(const std::vector<Vector2>& points, float weldDistance, int maxVertices = 8);
```

1. Weld consecutive points closer than `weldDistance` (Box2D linear slop 0.005 m in points), drop the
   closing duplicate, remove collinear points (cross < 1e-6 · scale²); < 3 left → error.
2. Signed area < 0 → reverse (CCW). |area| ≈ 0 → error.
3. Simplicity test O(n²) on non-adjacent edges; self-intersecting → `selfIntersecting = true`,
   result = convex hull (monotone chain) split into fan pieces of ≤ 8 points.
4. Convex and ≤ 8 points → one piece.
5. Else ear clipping (start at index 0, scan forward, first valid ear, O(n²)), then Hertel-Mehlhorn:
   visit diagonals in creation order, remove one when both neighbours stay convex and the merged piece
   has ≤ `maxVertices` vertices.
6. Pieces whose `b2ComputeHull` fails (sliver) are dropped and counted (`droppedPieces`, logged once).

`PS::syncRigidbody2DColliders` (`PS:1179-1350`) uses the cache (`decompositionKey`), builds one
`b2Polygon` per piece with the collider's radius, the same `b2ShapeDef` (user data = index), and
`updateBodyMass = false`; after the body's dirty colliders are rebuilt it calls
`b2Body_ApplyMassFromShapes` once, then re-applies the explicit mass data (D11) if any.

#### 6.1.4 Per-collider sync

`syncRigidbody2DColliders(entity, body)`: if the body was recreated or the world scale changed
(`PS:1660-1666`), every collider is dirty (today's behaviour); otherwise only `dirty` ones are
destroyed (`destroyCollider2D` per shape/chain) and recreated. Removing a collider erases it from the
vector, destroys its shapes and rewrites later shapes' user data. Setters in `RB2` set `dirty` and the
body's `needUpdateColliders`; nothing is rebuilt until the next fixed step (or `ensureRigidbody2DLoaded`).
`b2Shape_SetPolygon` is used when a POLYGON collider stays one convex piece.

#### 6.1.5 2D body fields (`Rigidbody2DComponent`)

| field | default (= legacy) | Box2D |
|---|---|---|
| `gravityScale` | 1 | `b2BodyDef.gravityScale` / `b2Body_SetGravityScale` |
| `linearDamping`, `angularDamping` | 0, 0 | damping |
| `fixedRotation` | false | `fixedRotation` |
| `collisionDetection` | DISCRETE | CONTINUOUS → `isBullet` |
| `sleepingMode` | START_AWAKE | NEVER_SLEEP → `enableSleep = false`; START_ASLEEP → `isAwake = false` |
| `useAutoMass` | true | false → `b2Body_SetMassData{mass, center, inertia}` after shapes |
| `mass` | 1 (kg) | used when `!useAutoMass` |
| `autoCenterOfMass` / `centerOfMass` | true / (0,0) points, body-local | mass data center |
| `autoInertia` / `inertia` | true / 0 (kg·m²) | false → explicit rotational inertia about the centre |
| `material` | "" | default material for colliders without one (D12) |

Applied in `PS::loadRigidbody2D` (`PS:1613-1670`) into the `b2BodyDef`; live setters in `RB2`
(`setGravityScale`, `setLinearDamping`, …, `PS`-side `applyMassFromColliders`) also write the field.
New Lua/C++: `mass` (property; setting it sets `useAutoMass = false`), `useAutoMass`, `centerOfMass`,
`inertia`, `collisionDetection`, `sleepingMode`, `material`.

#### 6.1.6 Materials and the Box2D patch

Physics-3d owns `PhysicsMaterial` (asset `.physicsmaterial`, `PhysicsMaterialPool`,
`PhysicsMaterialCombine` and `combineSurface()`; physics-3d §6.1.4). If this branch merges first it
implements that section verbatim.

Box2D patch (each hunk marked `// JASPER PATCH: surface callbacks (upstream 3.1 API)`):
- `types.h`: `int userMaterialId` in `b2ShapeDef` and `b2ChainDef` (default 0);
  `typedef float b2FrictionCallback(float frictionA, int userMaterialIdA, float frictionB, int userMaterialIdB);`
  and `b2RestitutionCallback` with the same parameters.
- `box2d.h`: `b2World_SetFrictionCallback(b2WorldId, b2FrictionCallback*)`,
  `b2World_SetRestitutionCallback(...)`, `b2Shape_SetUserMaterial(b2ShapeId, int)`,
  `b2Shape_GetUserMaterial`, `b2Chain_SetUserMaterial`.
- `shape.h/.c`: store `userMaterialId` per shape (chains copy it to their segments).
- `world.h/.c`: callback pointers (default NULL).
- `contact.c:337-338`: `friction = world->frictionCallback ? world->frictionCallback(shapeA->friction,
  shapeA->userMaterialId, shapeB->friction, shapeB->userMaterialId) : b2MixFloats(...)`, same for
  restitution. Callbacks run during pair creation (main thread here).

The engine encodes the combine modes in `userMaterialId`: bits 0-3 friction mode, bits 4-7 bounce
mode, 0 = "no material" (legacy). The callbacks call `combineSurface(a, modeA, b, modeB, legacyMode)`
with legacy friction = geometric mean, legacy restitution = maximum: two colliders without material
combine exactly as today. A material change at runtime updates `b2Shape_SetFriction/SetRestitution/
SetUserMaterial` and toggles the refilter bit (`PS::refilterShapes2D`, `PS:432`) so existing contacts
are rebuilt with the new values.

#### 6.1.7 Exit of the caps

Delete `MAX_COLLIDERS_2D`, `MAX_COLLIDER_POINTS_2D` (`Engine.h:128-134`), their exporter capacities
(`editor/Exporter.cpp:2921-2922`), `SceneMaxValues::maxColliders2D` / `maxColliderPoints2D`
(`editor/Project.h:91-92`, computation `editor/Project.cpp:3740-3750`, YAML `editor/Stream.cpp:3387-3388,
3551-3552`: old keys are read and ignored), the checks `editor/Catalog.cpp:1218` and
`editor/mcp/tools/WorldTools.cpp:1049-1050, 1338`. `editor/Factory.cpp:2310-2350` emits
`rigidbody2d.colliders.resize(n)` and `colliders[i].points = { Vector2(..), ... };` (an initializer list;
generated code for very large lists stays one statement per 64 points via `insert`).

### 6.2 Serialization (`editor/Stream.cpp:8022-8125, 8321-8366`)

- Collider: `points` (new key; `vertices` still read when `points` is missing), `material`,
  `receiver`, plus the existing keys; `numColliders` keeps being written (older editors) and, when
  reading, the count is `min(numColliders, colliders.size())` if present, else `colliders.size()`.
- Body: the D11 fields (each written always; missing → defaults above).
- Joint: every §6.1.2 field; `anchorSpace` always written; `breakForce`/`breakTorque` only when finite;
  decode applies the "legacy" column when a key is missing. Old scenes and prefabs load unchanged and
  behave identically (T1, T2).
- Scene settings: `physics2DSubSteps`.

### 6.3 Lua API (`GOL:1990-2128`)

`Joint2D` (class, `EntityHandle`): new `GameObject:getJoint2D() -> Joint2D|nil`. Properties
(read/write unless noted), names = §6.1.2 fields: `type`, `bodyA`, `bodyB` (GameObject or nil; set
accepts GameObject/Rigidbody2D/nil), `connectedBody` (alias of `bodyB`), `anchorSpace`
(`JointAnchorSpace.WORLD/LOCAL`), `anchorA`, `anchorB`, `anchor` / `connectedAnchor` (aliases),
`autoAnchors`, `axis`, `enableCollision`, `breakForce`, `breakTorque`, `breakAction`
(`JointBreakAction.IGNORE/CALLBACK_ONLY/DISABLE/DESTROY`), `useLimits`, `lowerAngle`, `upperAngle`,
`lowerTranslation`, `upperTranslation`, `useMotor`, `motorSpeed`, `maxMotorTorque`, `maxMotorForce`,
`enableSpring`, `frequency`, `dampingRatio`, `length`, `autoLength`, `minLength`, `maxLength`, `rope`,
`linearFrequency`, `angularFrequency`, `linearDampingRatio`, `angularDampingRatio`, `linearOffset`,
`angularOffset`, `autoOffset`, `maxForce`, `maxTorque`, `correctionFactor`, `target`, `enabled`.
Read-only: `reactionForce`, `reactionTorque`, `jointAngle`, `jointSpeed`, `jointTranslation`,
`jointLinearSpeed`, `motorTorque`, `motorForce`, `currentLength`, `limitState`
(`JointLimitState2D.INACTIVE/LOWER/UPPER/EQUAL`), `referenceAngle`, `broken`. Methods kept:
`setDistanceJoint`, `setRevoluteJoint`, … (they keep creating WORLD-space joints from world points,
documented). `getBox2DJoint()` stays C++-only (Box2D ids mean nothing in Lua). Errors: setting a property of a type that does not use it is
allowed (stored); assigning a body without `Rigidbody2D` → Lua error.

Unity-style example:

```lua
local joint = self.frontWheel:getJoint2D()
joint.useMotor = true
joint.motorSpeed = -720      -- deg/s
joint.maxMotorTorque = 10000
function Car:onJointBreak2D(joint) Audio.play(self.snap) end
```

`Rigidbody2D` additions: `mass` (property: get = `getMass()`, set → `useAutoMass = false`),
`useAutoMass`, `centerOfMass`, `inertia`, `collisionDetection` (`CollisionDetectionMode2D.DISCRETE/
CONTINUOUS`), `sleepingMode` (`RigidbodySleepMode2D.NEVER_SLEEP/START_AWAKE/START_ASLEEP`), `material`
(path or `PhysicsMaterial` or nil); collider editing: `removeCollider(index) -> bool` (later indices
shift down), `setColliderPoints(index, points)` (POLYGON, CHAIN; any count), `getColliderPoints(index)`
(table of Vector2), `setColliderLoop(index, bool)`, `setColliderRadius(index, r)`,
`setColliderCenter(index, Vector2)` (CIRCLE), `setColliderSegment(index, a, b)` (SEGMENT, CAPSULE),
`setColliderMaterial(index, material|nil)`, `getColliderMaterial(index)`, `setColliderTrigger(index,
bool)` / `isColliderTrigger(index)` (aliases of the sensor ones, Unity `isTrigger`),
`getColliderPieceCount(index)`. `createPolygonCollider(points)` accepts any count / concave;
`createChainCollider(points, loop)` accepts 2+ (D9). Index out of range → Lua error (programming
mistake), like `getColliderType`.

`PhysicsSystem.subSteps2D` (property). Enums bound: `JointAnchorSpace`, `JointBreakAction` (shared with 3D, defined by whichever
merges first in `engine/core/component/PhysicsCommon.h`, physics-3d §6.1.1),
`JointLimitState2D`, `CollisionDetectionMode2D`, `RigidbodySleepMode2D`.

C++ `Joint2D` (`engine/core/object/physics/Joint2D.h`) mirrors the Lua list with get/set methods.

### 6.4 Editor UX

- **Joint 2D Inspector** (`editor/window/InspectorWindow.cpp:12825-12884`), laid out like Unity's:
  Joint Type; Rigidbody A / Connected Rigidbody (B); Anchor Space (LOCAL hides nothing; WORLD shows a
  "Convert to Local" button that computes local anchors from the bodies' current poses, one undo step);
  Anchor; Connected Anchor + Auto Configure; Axis (WHEEL/PRISMATIC) with an angle helper field
  ("Angle 90°" ↔ axis); Enable Collision; per-type sections "Suspension" (Frequency, Damping Ratio),
  "Spring", "Limits" (Use Limits, lower/upper), "Motor" (Use Motor, Speed, Max Torque/Force),
  "Distance" (Auto Length, Length, Rope, Min/Max), "Stiffness" (WELD), "Offsets" (MOTOR), "Target"
  (MOUSE); "Break" (Break Force, Break Torque with ∞ toggle, Break Action). Play mode adds a read-only
  "Runtime" section (reaction force/torque, angle, speed, translation, motor torque, limit state).
  All rows through `propertyRow` with undo and prefab overrides; multi-select edits.
- **Scene view gizmos** (`editor/render/SceneRender2D.cpp`): anchors (circles A, B, line between), axis
  arrow, translation-limit bar, angle-limit arc for REVOLUTE, length/min/max for DISTANCE; anchor A/B
  handles draggable with the Move tool (one undo step per drag, writes local or world per space).
- **Rigidbody 2D Inspector** (`:11792`): body section adds the D11 fields (Mass greyed when
  Use Auto Mass; Center of Mass / Inertia with Auto toggles; Collision Detection; Sleeping Mode;
  Material asset field). Collider rows: Points list without a cap (add/remove/reorder, paste from
  clipboard as `x,y` lines), piece count readout for POLYGON ("concave: 7 pieces"), Material asset
  field (overrides Friction/Restitution, which grey out), "Send Callbacks To" (physics-contacts).
- **Scene settings > Physics 2D**: Sub Steps.
- Component add menu unchanged (Joint 2D exists). `create_entity type=joint2d` / Hierarchy "2D Object
  > Joint 2D" creates the joint as a child of the selected body when one is selected, LOCAL anchors.

### 6.5 MCP / agent surfaces

- `set_joint` (`editor/mcp/tools/WorldTools.cpp:4633-4655`): description updated (anchors local to
  body_a / body_b by default); new optional args shared with 3D: `motor {enabled, speed, max}`,
  `spring {enabled, frequency, damping}` (2D) / `{enabled, stiffness, damper, target}` (3D),
  `limits [min, max]` (now 2D too: angle or translation by type), `break {force, torque, action}`,
  `enable_collision`, `anchor_space`; everything else through `properties` (Catalog names). A newly
  created joint GameObject is parented under `body_a`. Result returns the joint's resolved fields and,
  in Play, the read-outs.
- `edit_colliders`: polygon `points` any count ≥ 3 (concave), chain ≥ 2; `material`; result reports
  `pieces` for polygons and a warning for > 8-point polygons on static bodies ("consider chain for
  rolling contact"). Remove the 16-point error.
- `set_properties` / `get_entity` reach every new field through the Catalog
  (`editor/Catalog.cpp:833-841` joint list grows; body and collider paths).
- `get_runtime_entity`: joints report read-outs.
- `docs/mcp/guide/physics.md`: new "Joints" section (types, Unity mapping table, units, local anchors,
  breaking), polygon/chain rules (table row "polygon (points, 3-16)" corrected), 2D body fields,
  materials pointer to physics-3d's section.

### 6.6 Exported-build path

`editor/Factory.cpp` emits every new field (joints: one assignment per field that differs from the
struct default, as the existing code does for the others; infinity as
`std::numeric_limits<float>::infinity()`; colliders as in §6.1.7). Box2D patch compiles on every
platform (plain C). No capacities left to inject. `.physicsmaterial` assets are packed like other
data assets (physics-3d §6.5).

### 6.7 Docs

`docs/mcp/guide/physics.md`, `docs/mcp/guide/lua-api.md` (Joint2D, Rigidbody2D additions),
`docs/unity-glossary.md` (WheelJoint2D/HingeJoint2D/… → `Joint2D` types; `connectedBody` → `bodyB`;
`PolygonCollider2D` → POLYGON collider; `EdgeCollider2D` → CHAIN; `PhysicsMaterial2D` →
`PhysicsMaterial`; `useAutoMass`; `OnJointBreak2D` → `onJointBreak2D`), README (concave 2D colliders,
2D joints with motors and suspension).

## 7. Behaviour rules and edge cases

1. **Prefabs.** LOCAL joints follow the prefab instance; `bodyA`/`bodyB`/`receiver` remap on
   instantiation (existing entity-ref remap). WORLD joints keep the world point (legacy).
2. **Changing `bodyB` at runtime** (hill climb car swap): `needUpdateJoint`, rebuilt before the next
   step with `autoAnchors` recomputing the connected anchor from the current poses (Unity).
3. **Joint on inactive body / disabled joint** — unchanged rule (`PS:2990-3001`); a broken DISABLE joint
   stays disabled until a script enables it (fresh joint, `broken = false`).
4. **Break in the same step as a destroy**: the callback still runs (the joint exists until the
   component removal after dispatch); destroying the joint's GameObject in `onJointBreak2D` is safe.
5. **Fixed step / time scale**: breaking is per fixed step; forces are per-step averages (Box2D), so
   thresholds are independent of sub-steps but not of `updateTime` jitter (fixed).
6. **Decomposition determinism**: same points → same pieces on every platform (integer-free float
   ops, no hashing order); the cache key includes scale and radius.
7. **Concave polygon on a dynamic body**: allowed; mass = sum of pieces; seams can catch other pieces
   only at internal edges (Box2D treats pieces as separate shapes).
8. **Chain winding**: today's mirror rule (`PS:1325-1328`) kept; counter-clockwise collides outside.
9. **Mass override + collider rebuild**: explicit mass data re-applied after every rebuild.
10. **Kinematic / static bodies** ignore mass fields (Box2D).
11. **`contactEvents` default** affects only new colliders; per-object callbacks enable events
    internally (physics-contacts D16).
12. **Threading**: none new (Box2D single-threaded; the friction/restitution callbacks are pure C++).
13. **Scene load**: joints and colliders are built in `PS::load` (`PS:2911-2938`) as today; breaking
    never runs on load.

## 8. Performance

- Collider storage: one heap vector per collider (was inline arrays in exports): negligible.
- Decomposition O(n²) per rebuild, cached; 226-point terrain ≈ 70-90 pieces, < 1 ms (assert in a test:
  < 2 ms for 500 points, Release).
- Per-collider rebuild: a 500-point chain edit rebuilds one chain, not the body (T9 measures the
  number of `b2CreateShape` / `b2CreateChain` calls).
- Breaking: two Box2D calls per joint with a finite threshold per step.

## 9. Tests (`editor/tests/Physics2DJointsTest.cpp`, `Physics2DCollidersTest.cpp`, `PolygonDecomposition2DTest.cpp`)

- T1 Legacy equivalence: for each joint type, a component decoded from YAML without the new keys
  builds a joint whose Box2D def fields equal `b2Default*JointDef` + today's overrides (compare the
  live joint's getters: hertz, damping, limits, motor flags, max force/torque, local anchors).
- T2 Old scene file (fixture copied from today's format, WHEEL + DISTANCE rope + REVOLUTE): 120 steps
  give the same body poses (±1e-5) as a recorded run from `main` (fixture CSV committed).
- T3 Hill-climb car: body box + two wheels (circle radius 0.34 world, mass 1 each, useAutoMass false,
  tire material friction 2), WHEEL joints LOCAL anchors (1.153, 0.31) / (-1.444, 0.31) ×64, frequency
  3, damping 0.35, axis (0, 1); dropped onto a flat static box: after 2 s the car is at rest (body
  speed < 0.5 points/s) on both wheels, the suspension is compressed (wheel-to-anchor distance along
  the axis shorter than the authored one) and less so with frequency 6 than with 3 (stiffer spring);
  a prefab instance placed at (+1000, 0) gives the same poses relative to its origin (±1e-3; LOCAL
  anchors).
- T4 Wheel motor: `useMotor`, speed -720 deg/s, torque 10000: wheel angular speed reaches -720 deg/s
  ±2% in < 0.5 s with the car lifted (bodyA static).
- T5 Revolute limits [-30, 30]: a pendulum released at 0 with gravity stays within ±30° (+slop 1°);
  `limitState` reports LOWER/UPPER at the ends.
- T6 Prismatic limits + motor; distance spring frequency 1 Hz oscillation period 1 s ±5%; weld with
  angularFrequency 2 Hz bends then returns; motor joint `autoOffset` holds the relative pose; mouse
  joint `maxForce` 0 = legacy formula.
- T7 Breaking: a body of mass 10 hanging on a revolute joint with `breakForce = 50` (weight 98 N)
  breaks on the first step: one `onJointBreak2D` on the joint GameObject and on bodyA's; DESTROY
  removes the component after the dispatch; DISABLE sets `enabled = false`; IGNORE: no callback, joint
  intact; CALLBACK_ONLY: one callback, joint intact, re-armed after the load is removed.
- T8 Caps gone: a body with 40 colliders and a 300-point chain builds in an editor Play scene and in a
  standalone engine scene (no editor `HybridArray` path) with the same shape counts; the Exporter's
  generated CMake has no `MAX_COLLIDER*_2D` definitions.
- T9 Per-collider rebuild: `setColliderPoints` on collider 2 of 5 calls Box2D create functions for
  collider 2 only; the body id, its linear velocity and a joint attached stay the same.
- T10 Decomposition: square (1 piece), L-shape (2), comb with 9 teeth (≥ 9), 226-point terrain fixture
  extracted from `InfiniteTerrain.prefab` (all pieces convex, ≤ 8 vertices, total area equals the
  polygon's ±0.1%, no piece overlaps: pairwise SAT); duplicate/collinear points welded; a bow-tie
  (self-intersecting) → hull + `selfIntersecting`; same input twice → identical output.
- T11 Concave polygon collider: a ball dropped into a U-shaped static polygon rests inside it.
- T12 Short chain: 2 points → one segment shape; 3 → two.
- T13 2D body fields: gravityScale 0 body does not fall; linearDamping 1 halves velocity in ≈0.69 s;
  CONTINUOUS body (bullet) at 2000 points/s does not tunnel through a 4-point-thick dynamic wall;
  mass override 1 kg reads back `getMass() == 1` after a collider rebuild; fields survive save/load.
- T14 Materials: two boxes sliding with friction materials 0 (Minimum) vs 1 → combined 0 (box keeps
  sliding); none set → geometric mean as before; changing a material at runtime affects an existing
  resting contact on the next step (refilter); bounciness 1 + Maximum bounces.
- T15 Serialization round trip of every new field (Stream encode/decode, Factory-generated code
  compiled in the engine test target equals the decoded component).
- T16 `contactEvents` default: a collider created by Lua or MCP has it true; a file without the key
  reads false.
- MCP e2e `tools/mcp/tests/test_physics_2d.py`: `set_joint` with motor/spring/limits/break, read back
  via `get_entity`; `edit_colliders` with a 40-point concave polygon (no error, `pieces` reported);
  play and `get_runtime_entity` shows joint read-outs.

## 10. Acceptance criteria

1. Tests pass; `jasperengine-editor-tests`, engine tests and `tools/mcp/run-tests.sh` green.
2. `grep -rn "MAX_COLLIDER_POINTS_2D\|MAX_COLLIDERS_2D\|numVertices\b" engine editor` finds nothing in
   2D code (3D removed by physics-3d).
3. The hill-climb car (§3) can be authored entirely in the Inspector and through MCP with the Unity
   values, and behaves the same in Play and in an exported Linux build (T3 run on both).
4. Old scenes load and simulate identically (T1, T2).
5. Stubs regenerated, `-- unresolved:` empty; `docs/mcp/TOOLS.md` regenerated; tool budget passes.
6. Box2D patch hunks all carry the marker comment; Android and Web builds compile.

## 11. Integration points

`engine/core/component/Joint2DComponent.h` (fields), `Rigidbody2DComponent.h`, `Engine.h:128-134`;
`PS:1017-1058` (`loadJoint2D`), `:1179-1350` (colliders), `:1613-1670` (body), `:1783-1835`
(load/destroy collider), `:1863-2147` (joint loaders), `:2969-3067` (fixed update 2D: breaking after
`:3038`, sub-steps `:3036`); `engine/core/object/physics/Joint2D.{h,cpp}`, `Rigidbody2D.{h,cpp}`;
`engine/core/util/Box2DAux.h` (shape map shared with physics-contacts); `B2` patch files listed in
§6.1.6; `GOL:313-363` enums, `:1990-2128` classes; `editor/Stream.cpp:8022-8125, 8321-8366, 3387-3388,
3551-3552`; `editor/Factory.cpp:2310-2350` and the Joint2D factory function; `editor/Catalog.cpp:833-841,
1205-1300`; `editor/Exporter.cpp:2886-2926`; `editor/Project.h:91-92`, `editor/Project.cpp:3740-3750`;
`editor/window/InspectorWindow.cpp:11792-12276, 12825-12884`; `editor/render/SceneRender2D.cpp:400-440`;
`editor/mcp/tools/WorldTools.cpp:1040-1060, 1330-1360, 1470-1600, 4616-4655`; `editor/mcp/McpValues.cpp`
(collider JSON); `engine/core/registry/EntityCloner.cpp`.

## 12. Cross-feature dependencies

- **physics-3d**: owns `PhysicsMaterial` (asset, pool, combine rule; implement verbatim if merging
  first), `JointBreakAction` / `PhysicsCommon.h` (shared enum), per-body interpolation (adds the 2D
  field too), debug draw (draws 2D with `b2World_Draw`), the same anchor-space concept for 3D joints.
  Both refactor collider storage: this branch does 2D, physics-3d does 3D; whichever merges second
  resolves `Engine.h` / `Exporter.cpp` / `Project.*` hunks.
- **physics-contacts**: 2D contact stay and every callback; uses `Collider2D::shapes` (several shapes
  per collider) and the shape → collider map; `ScriptMessages` for `onJointBreak2D` (if this branch
  merges before both physics-contacts and script-lifecycle, implement the minimal `ScriptMessages` of
  physics-contacts §6.1.1 with the `JointBreak2D` message only).
- **input** / **ui-multitouch**: none (the car's controls are game code).

## Implementation notes

Decisions taken while implementing (branch `gaps/physics-2d`, merged before physics-3d and
physics-contacts), where the spec was silent, ambiguous or contradicted itself:

1. **Shared pieces implemented here (physics-3d extends them).** `component/PhysicsCommon.h` has
   all four enums of physics-3d §6.1.1 (`RigidbodyInterpolation` is declared, not used or bound
   yet: per-body interpolation and the runtime debug draw stay physics-3d's) and
   `combineSurface()`. `physics/PhysicsMaterial.{h,cpp}` (§6.1.4 parse/write contract, Unity
   aliases, unknown keys kept, presets) and `pool/PhysicsMaterialPool.{h,cpp}` (normalised keys,
   `Data` reads, invalidate, a global revision bumped by every runtime edit). The Jolt side
   (`JoltSurfaceMaterial`, 3D combine callbacks) is physics-3d's. Because the hill-climb car must be
   authorable in the editor and over MCP (acceptance 3), the asset's editor surfaces of physics-3d
   §6.4/§6.5 were also done here: Project window type, icon, Create > Physics Material / Physics
   Material 2D, the Inspector asset view (undoable file writes, drags merged), a material field on
   the Rigidbody 2D and each collider (drop, pick, clear), and the MCP tool `edit_physics_material`
   (the 3D spec's signature). `PhysicsMaterialCombine` is a plain `int` enum (LuaBridge pushes a
   `uint8_t` enum as a character).
2. **`JointBreakAction::IGNORE`**: `PhysicsCommon.h` `#undef`s Windows' `IGNORE` macro (WinBase.h)
   so the spec's enumerator names compile on Windows.
3. **WORLD anchor space is the complete legacy mode.** WORLD joints take their world points at
   creation and keep Box2D's reference angle 0 (the old code never captured it: a weld or slider
   between two bodies of different angles aligned them). LOCAL joints capture the bodies'
   relative angle (Unity). The Inspector's Convert to Local says so in its tooltip.
4. **LOCAL DISTANCE joints ignore `autoAnchors`**: putting anchorB where anchorA is would make the
   length 0. Their anchors are each body's own point (Unity DistanceJoint2D leaves Auto Configure
   Connected Anchor off by default); the Inspector hides the toggle for them.
5. **Local axis** = `normalize(axis × bodyA scale)`, like the anchors (a mirrored body mirrors its
   axis). **Write-back at creation** (Unity): an auto-configured `anchorB`, an `autoLength` length and
   `autoOffset` offsets are written into the component of the running scene.
6. **A joint without bodyB**: LOCAL joints hold bodyA to a static ground body of the world (Unity
   `connectedBody = null`, and MOUSE's "bodyB unused"); WORLD (legacy) joints without bodyB are not
   created, as before, so an old scene with an unfinished joint does not suddenly pin its body.
7. **Lua `set*Joint` helpers** keep their old behaviour exactly: they set every parameter field to
   the legacy value of the type (Box2D's defaults), WORLD space, then build. The 2-argument
   `setDistanceJoint(a, b)` was broken (it turned `autoAnchors` off and failed on two zero anchors);
   it now uses the body origins as documented.
8. **Type changes** (Inspector combo, `set_joint type`, Lua `joint.type`) give the type-dependent
   fields still at the old type's defaults the new type's (`changeJoint2DType`); a raw
   `set_properties {type}` changes the type alone.
9. **Live vs rebuild.** Two update flags: `UpdateFlags_Joint2D_Params` (motor, spring, limits,
   break, target...: applied to the live joint) and `UpdateFlags_Rigidbody2D_Params` (body fields:
   applied to the live body); the type, bodies, anchors, axis, anchor space, `autoLength`,
   `autoOffset` rebuild the joint. Collider edits made through the Catalog (Inspector in Play,
   `set_runtime_property`) still rebuild the whole body, as before (their values are whole
   `Collider2D` copies); Lua collider edits rebuild only the collider (D10).
10. **Catalog paths.** `numColliders` is gone; the list is the Custom property `colliders`
    (`std::vector<Collider2D>`, one undo step for add/remove), `colliders[i]`, `colliders[i].points`
    and `.points[j]`; the old `colliders[i].vertices[j]` resolves to the same point, and MCP maps
    `vertices` to `points` in `set_properties` names. The YAML key is `points` (`vertices` read).
    `categoryBits`/`maskBits` stay out of the Catalog (uint16 fields, as before). Material paths are
    assets-relative in components, project-relative over MCP (`isAssetPathProperty`).
11. **`receiver`** (physics-contacts D9): stored, serialized (omitted when none), emitted by the
    Factory, remapped by the EntityCloner and reachable as the Catalog entity property
    `colliders[i].receiver` (so editor prefab remaps work); its delivery and the Inspector row
    "Send Callbacks To" are physics-contacts'.
12. **Old polygons.** Old scenes are identical except polygons that were never what their points
    said: a concave polygon of up to 8 points was built as its convex hull, one of 9-16 points
    failed `b2ComputeHull` and fell back to the pointA/pointB box (or nothing). Both now get the
    authored shape. Boxes and convex polygons of up to 8 points build the same Box2D polygon (T2
    passes against the fixture recorded on `main`).
13. **Decomposition** (subagent-written module, reviewed): self-intersection is tested before the
    zero-area check (a symmetric bow-tie has zero signed area but must give its hull); touching
    non-adjacent edges count as self-intersecting; slivers thinner than the weld distance are
    dropped and counted; `b2ComputeHull` failures of pieces are added to the dropped count.
    A chain loop of 3 points is 3 segments, of 2 points 1 segment.
14. **ScriptMessages** (physics-contacts §6.1.1 API, all physics messages declared, only
    `JointBreak2D` sent): `receivers()` is computed on demand (no cache yet: script-lifecycle adds
    the cache and calls `notifyReceiversChanged`, which exists). C++: `ScriptBase` gained a virtual
    destructor and `virtual onJointBreak2D(Joint2D&)`; `ScriptEntry` gained `cppScript` and
    `cppMessageMask`, filled by the generated script factory (`Generator.cpp`) through
    `ScriptMessages::cppScript<T>` / `cppMask<T>`.
15. **Material instancing (physics-3d D8) also in 2D**: `setColliderFriction/Restitution` on a
    collider whose friction comes from a material gives that collider its own copy (Unity
    `collider.material`); the getters return the material's values. Runtime edits of a pooled
    material last until the pool reloads it (invalidate, `clearPools`/`clearUnusedPools`).
16. **Scene view.** Joint gizmos follow the anchor space (local anchors through each body's model
    matrix). The Move tool drags anchor A, anchor B when it is independent (DISTANCE, or
    `autoAnchors` off) and a mouse joint's target, one undo step per drag (the sub-selection
    pattern of polygon points). New Joint 2D GameObjects have a Transform so they can live under
    their body (Hierarchy and `set_joint` put them there, with bodyA set).
17. **MCP.** `set_joint` (2D) accepts no `body_b` (the world); it cannot run in Play, so the joint
    read-outs are in `get_runtime_entity` (`joint2d`, and `rigidbody2d` for bodies); its result
    lists every resolved field. `limits: false` turns limits off; `break.force: null` = never.
18. **Tests.** T15's "Factory-generated code compiled in the engine test target" is replaced by
    checks of the generated code text (every non-default field, 64-point chunks): there is no test
    target that compiles generated scene code. T8's "generated CMake has no MAX_COLLIDER*_2D"
    holds by construction (the macros and their exporter entries are deleted; acceptance 2's grep).
    T4 runs the motor on a static (lifted) car body. T3's car body box sits above the wheels'
    bottoms (with a centred 200 x 40 box and anchors at y +0.31, the chassis reaches the ground
    before the wheels) and starts 4 points above the ground; "compressed" is a positive joint
    translation (static deflection g / (2 pi f)^2: 1.8 points at 3 Hz, 0.44 at 6 Hz).
19. **Box2D 3 limits are soft** (60 Hz joint softness, solved after the motor). T5's "+slop 1°"
    holds for a rod pendulum (box from the pivot): a point bob on a long arm overshoots by ~7°
    (the angular limit sees only the bob's own inertia) and rests 2° past. T6's prismatic motor
    uses 100 N on a 1 kg slider: Unity's default 10000 N pushes it ~18 points past the limit. The
    engine keeps Box2D's solver (no patch: legacy scenes never had limits); the physics guide
    tells agents to size motor forces and give arms their length.
20. **Waking.** A `gravityScale` change through the Catalog / Inspector (body parameters applied
    live) wakes a sleeping dynamic body, so its joints' forces (breaking) and motion follow; the
    Lua `rb.gravityScale` setter keeps Box2D's behaviour (no wake), as before.
21. **Motor joint offset frame.** Box2D 3.1's `b2MotorJoint` holds bodyB's centre minus bodyA's
    centre at `linearOffset` in world space (its header says bodyA's frame). LOCAL motor joints
    keep Unity's meaning (bodyB's origin in bodyA's frame, `autoOffset` writes that back) and the
    engine turns it into Box2D's before every step, so a follower orbits a turning base; WORLD
    (legacy) joints pass the value to Box2D as before.
22. **Auto values are captured once per joint per world.** A joint made again only because one of
    its bodies was (a body or collider edit in Play recreates the body, and Box2D destroys its
    joints) keeps its captured anchor B, length, offsets and reference angle; an edit of the joint
    itself (bodies, anchors, type, space...) or a disable / enable captures them again. WORLD
    joints are rebuilt as before.
23. **Restore over a live component.** Keys the encoder leaves out at their default (`breakForce`
    / `breakTorque` infinite, collider `material` and `receiver`) decode to that default also over
    an existing component (Play's Stop restore), and a script's runtime material objects are
    dropped there; Live distance-joint edits only call Box2D's length setters when the length
    changes (they clear the joint's warm start). Cloning gives a collider's own material copy a
    copy of its own.
24. **Verification** (Linux, this branch): `jasperengine-editor-tests` 979 passed, 8 skipped (the
    known skips), 0 failed (43 new: Physics2DLegacy, Physics2DJoints, Physics2DColliders,
    PolygonDecomposition2D); MCP `test_physics_2d.py` plus `test_stubs`, `test_docs`, `test_world`,
    `test_scene`, `test_play*`, `test_prefab`, `test_place*`, `test_water`, `test_ui_anchors`,
    `test_runtime_regressions`, `test_html_ui_tools` (the budget: 149 tools, 106029 characters)
    123 passed, 7 skipped (no lua-language-server / clangd, slow C++ build); with
    `JASPER_SLOW_BUILD=1`, `test_exported_player_runs_the_car` built a Desktop player and the car
    rested there exactly as in Play (T3 on both). Lua stubs: 0 unresolved. Android (NDK 28, arm64)
    and Web (emscripten) compile checks of every changed engine source (`-fsyntax-only` with the
    platform's defines; a full Android configure needs the Gradle game-activity prefab): clean.
25. **Review fixes** (independent review of the branch; each has a regression test in
    `Physics2DReviewTest.cpp`, `PolygonDecomposition2DTest.cpp`, `test_physics_2d.py` or
    `test_physics_2d_inspector.py`):
    - *Old prefab overrides.* Override names recorded before the caps went are migrated when read
      (`PrefabObjectOverrides::addProperty`): `numColliders` is the `colliders` list,
      `colliders[i].numVertices` its `points`, `colliders[i].vertices[j]` `points[j]`; and
      `Catalog::copyPropertyValue` copies names the lookup resolves but does not list (a single
      point), so rebuilds and Apply to Prefab keep point edits.
    - *Decomposition fidelity.* A point is dropped as "straight on" when it, and every point
      dropped next to it, lies within the weld distance (Box2D's linear slop) of the line between
      the kept points around them: local and absolute (the bbox-relative tolerance flattened 1 m
      hills on a 3 km terrain and a 2000-tooth comb). Zero-area checks are rounding-noise only;
      thin pieces are still dropped by the weld test.
    - *Pieces in the editor.* The Inspector, the Scene view and MCP's `pieces` read
      `PhysicsSystem::updateCollider2DPieces` / `getCollider2DPiecesLocal`: the body's own pieces
      (after the GameObject's scale, unhullable slivers left out), cached on the collider by
      points, scale and units, so nothing is decomposed every frame.
    - *Breaking.* A joint broken with DISABLE and enabled again (Joint2D::setEnabled, or the
      field from the Inspector) is a fresh joint that can break again; `isLive()` (read-outs, the
      Inspector's Runtime section) also requires no pending rebuild and a Box2D joint of the
      component's type. A joint that cannot be made (a body removed) logs once (`loadFailed`).
    - *Materials at run time.* The Box2D patch mixes friction and restitution again at every
      contact update, so a material change no longer refilters the body: resting contacts stay
      (no end / begin events). Unity 2D's `Mean` reads as `geometric_mean`.
    - *C++ messages.* `ScriptMessages::cppMask<T>()` no longer names `T::onJointBreak2D` (a
      protected, private or overloaded override made the generated factory fail to compile): a
      C++ script counts as a receiver of every message until a message reaches the base method,
      which marks it (`ScriptBase::inheritedMessages`); script-lifecycle can keep this mechanism.
    - *Inspector.* Each collider's widgets are under its own ImGui ID; the axis Angle drag ends its
      undo step at release; a physics material's Friction and Bounciness (and each drag) are
      separate project undo steps; copied points keep full float precision.
    - *Units and errors.* Revolute limit defaults (-90 / 90 degrees) and `set_joint`'s half-turn
      clamp follow the project's angle unit; Lua argument errors throw (LuaBridge turns them into
      Lua errors) instead of `luaL_error` longjmps past C++ objects.
    - *Spec 6.1.4.* A polygon edit that stays one convex piece uses `b2Shape_SetPolygon` (the same
      shape id). Box2D 3.1 still drops that shape's contacts there; the body's other shapes and
      contacts are untouched.
26. **Verification after the review fixes and the merge of `main`** (script-data, input, the
    128-bit ECS signature; built on a 16-core box): `jasperengine-editor-tests` 1075 passed, 8
    skipped, 0 failed; MCP `test_physics_2d.py`, `test_physics_2d_inspector.py` (real input),
    `test_stubs`, `test_docs`, `test_html_ui_tools` (150 tools, 106620 characters, the limit),
    `test_prefab`, `test_world`, `test_scene`, `test_play*`, `test_runtime_regressions`: 96 passed,
    8 skipped (no lua-language-server / clangd / xev, slow C++ builds); zero warnings in the
    changed sources; Android and Web syntax checks of the engine sources the fixes touched: clean.
    The exported-player test was not run again after the fixes.
