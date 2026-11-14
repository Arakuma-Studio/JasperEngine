# Physics

Served as `jasper://docs/physics`. 3D physics is Jolt (`RigidbodyComponent`, script class
`Rigidbody`); 2D physics is Box2D (`Rigidbody2DComponent`, script class `Rigidbody2D`). The two
are separate worlds with different units, defaults and API spellings: never mix their
reasoning.

## Bodies and colliders

- A Collider (Collider 2D) is an entry of the body component's `colliders` list (a Jolt or
  Box2D shape), not a separate component. Add the body and its collider to the **same**
  GameObject in one step with `edit_colliders` (`body_type` + `add`; a terrain gets a
  `heightfield` collider). Never create a separate "Rigidbody" GameObject unless asked.
- A body without at least one collider does not collide.
- Body type: **dynamic** for player characters and physical actors, **static** for floors,
  terrain and level geometry, **kinematic** for bodies moved by script that push others.
  A dynamic character needs a static collider under it.
- Collider dimensions and local position are in **mesh-local units**: the engine multiplies
  them by the GameObject's transform scale. Never pre-divide or pre-multiply a size by the
  scale to compensate.
- Never guess a collider size. Omit the size so the collider is fitted to the mesh's measured
  bounds, or read the measured local bounds (`mesh_bounds.local_size`) and pass those. If a
  collider is too big or small, re-read the bounds; don't nudge numbers by feel.
- Growing a collider's height expands it equally up and down about its center: shift its
  position by half the added height to keep the bottom in place.
- 2D primitives and polygon/chain points are in GameObject-local 2D units (points).

### `edit_colliders`

One call edits the colliders of a Rigidbody 2D or Rigidbody, creating the body when it is missing
(`body_type`, other body properties in `body`); without edits it lists them. `dimension` (`2d` /
`3d`) defaults to the existing body's, else the scene's. The edits run in this order: `clear`,
`remove` (indices into the list as it was), `update` (`[{index, ...fields}]`), `add`
(`[{shape, ...fields}]`, default shape `box`). The result returns the colliders. A new shape is
fitted to what the GameObject draws (sprite, mesh or RectTransform in 2D; primitives to the
mesh bounds in 3D) unless `fit: false`; sizes you pass win.

| | shapes and their fields | also |
|---|---|---|
| 2D | `box` (`size`, `center`), `circle` (`radius`), `capsule` (`radius`, `point_a`, `point_b`), `segment` (`point_a`, `point_b`), `polygon` (`points`: any number, 3 or more, concave allowed), `chain` (`points`: 2 or more, `loop`); sizes in points before the GameObject's scale; any number of colliders per body | `density`, `friction`, `restitution`, `material` (a `.physicsmaterial` path or null), `sensor` (alias `is_trigger`), `hit_events`, `contact_events`, `pre_solve_events`, `sensor_events`, `category_bits`, `mask_bits`, `group_index`, `center`, `offset`, `receiver` |
| 3D | `box` (`size`), `sphere` (`radius`), `capsule` / `cylinder` (`radius`, `half_height`), `tapered_capsule` (`top_radius`, `bottom_radius`, `half_height`), `convex_hull` / `mesh` (from the GameObject's mesh, or `source`; or raw `vertices` [[x,y,z]...] and, for a mesh, `indices`: any number), `heightfield` (a terrain's, `samples_size`); any number of colliders per body | `density`, `center`, `rotation`, `is_trigger`, `material` (a `.physicsmaterial` path or null), `receiver`; `friction` and `restitution` without a material are per body (they go to the Rigidbody) |

## Mass

By default mass comes from the collider's size and density; either body can take an explicit mass
(3D: `automaticMass: false` + `mass`, below; 2D: `useAutoMass: false` + `mass`).

| | 3D (Jolt) | 2D (Box2D) |
|---|---|---|
| formula | volume x density | area (in m^2) x density |
| default density | 1000 (kg/m^3, water) | 1 |
| units | meters | collider sizes in points, in metres after the scene's Points Per Meter (`physics_2d_points_per_meter`, 64 by default; "World settings") |
| example | radius-1 sphere ~ 4189 kg, radius-0.5 ~ 524 kg | 100x100 point box ~ 2.4 kg |
| read it (C++) | `getMass()` | `getMass()` |
| read it (Lua) | `body.mass` (property) | `body.mass` (property) or `body:getMass()` |
| author it | density, or `automaticMass: false` + `mass` (kg) | density, or `useAutoMass: false` + `mass` (kg) |

Size forces as `F = mass x desired acceleration` using the real mass: a few hundred newtons
on a multi-tonne body looks like a broken script. If movement needs implausibly large forces
or feels sluggish, lower the collider density (or shrink the collider) instead of inflating
the force. Setting `body.mass` (Lua) / `setMass` (C++) turns `automaticMass` off and is saved like
any field.

## Forces, velocity, teleport

- Continuous forces and torques MUST be applied in `onFixedUpdate`, never `onUpdate`: each
  world accumulates force calls until its next fixed step, so per-frame calls make the force
  depend on the frame rate. Velocity control and held-input polling that drives physics belong
  in `onFixedUpdate` too.
- Do not multiply forces or torques by deltatime: the engine integrates them over the step.
- One-shot impulses are emitted once per press, not every held frame.

| | Rigidbody (3D) | Rigidbody2D |
|---|---|---|
| force at center | `applyForce(force)` | `applyForceToCenter(force [, wake])` |
| force at point | `applyForce(force, point)` | `applyForce(force, point [, wake])` (no 1-argument form) |
| torque | `applyTorque(torque)` | `applyTorque(torque [, wake])` |
| impulse | `applyImpulse(impulse)` | `applyLinearImpulseToCenter(impulse [, wake])` |
| velocity | `linearVelocity` (Lua property) / `setLinearVelocity` (C++) | `linearVelocity` (Lua, points/s) |
| teleport | `body.position`, `body.rotation` (Lua) / `setPosition`, `setRotation` (C++) | `body.position`, `body.angle` (Lua) / `setPosition`, `setAngle` (C++) |

There is NO `applyForceToCenter` on the 3D `Rigidbody`. `wake` defaults to true (a sleeping body
wakes, as Unity's `AddForce`); pass `false` to leave a sleeping body asleep.

- Never move a body by writing its GameObject's transform from `onFixedUpdate`
  (`object.position = ...`, `setPosition`): the body is synced from the transform before the
  step and written back after it, so the write is silently discarded. Teleport through the body.
  GameObjects without a body have no such restriction.
- A transform moved outside the fixed step (`onUpdate`, an animation, a Tween, the editor)
  moves its body at the next step by as much as it was moved, position and rotation each on
  its own: setting only the rotation leaves the body's motion alone. A dynamic or static body
  jumps; a **kinematic** body is moved with a velocity spread over the fixed steps of the
  frame, so it pushes and carries the bodies it touches (a platform carries what stands on
  it), and a part not animated keeps the velocity a script gave it. A kinematic body that a
  fixed step would move further than its own size, or turn more than a quarter turn, is
  teleported instead (a respawn, a reset); one without colliders (a joint anchor) is always
  moved with a velocity. Setting the body's position, rotation, velocity or type from a
  script takes that part over from the transform.
- Never describe transform-only movement as physics.

## 3D bodies, colliders and materials

`RigidbodyComponent` fields (Unity's Rigidbody; Catalog names for `set_properties`, `edit_colliders`
`body` (snake_case accepted: `linear_damping`), Lua properties of the same name; the defaults are
what bodies did before these fields, so old scenes move as they did):

| field | default | |
|---|---|---|
| `linearDamping`, `angularDamping` | 0.05, 0.05 | Unity drag / angularDrag (Jolt's, not Unity's 0) |
| `maxLinearVelocity`, `maxAngularVelocity` | 500 m/s, 47.1 rad/s | Unity maxLinearVelocity / maxAngularVelocity |
| `automaticMass`, `mass` | true, 1 | false: `mass` kg instead of volume x density (setting `body.mass` turns it off) |
| `automaticCenterOfMass`, `centerOfMass` | true, (0,0,0) | body-local, world units; a low one makes a box self-right |
| `automaticInertiaTensor`, `inertiaTensor`, `inertiaTensorRotation` | true, (1,1,1), identity | principal moments, kg m^2 (auto: from the colliders, scaled with an explicit mass) |
| `allowSleeping` | true | false: never sleeps |
| `collideKinematicVsNonDynamic` | false | a kinematic body also touches static and kinematic ones (always with a trigger collider) |
| `interpolation` | `scene` | `none`, `interpolate`, `extrapolate` (Interpolation below) |
| `material` | "" | default `.physicsmaterial` of the colliders without one |

**Type** switches live, in Play too (Inspector, `set_runtime_property`, Lua `body.type`,
`body.isKinematic`): the same body, joints and contacts. A dynamic body made kinematic stops; a
kinematic one made dynamic keeps the velocity it was moving with. A body with a `mesh` or
`heightfield` collider can only be static (an error names the collider).

**Triggers per collider** (Unity Is Trigger): a collider with `isTrigger` passes through and
reports a sensor contact, the body's other colliders still collide; triggers add no mass and do not
spoil continuous collision (`linear_cast` bodies stop at thin walls with a trigger around them). A
kinematic body with a trigger detects static bodies. The old body-wide `sensor` still works.

**Editing colliders from Lua at run time** swaps the shape (the body keeps its id, velocity,
joints and contacts): `body:setColliderRadius(i, r)`, `setColliderBoxSize`, `setColliderHalfHeight`,
`setColliderTaperedRadii`, `setColliderPosition`, `setColliderRotation`, `setColliderVertices(i,
vertices [, indices])`, `setColliderTrigger`, `setColliderMaterial`, `removeCollider(i)` (later
indices shift down). No collider or vertex caps: a 20 000-index mesh collider is fine.

**Materials** are the same `.physicsmaterial` assets as 2D (below): on a collider (`material`) or
the body (its colliders without one). Without materials two colliders combine as before
(geometric mean friction, maximum restitution of the bodies' `friction` / `restitution`).
`setColliderFriction` / `setColliderBounciness` give the collider its own copy (Unity
`collider.material`); drop a material on a GameObject (Scene or Hierarchy) to set it on all its
colliders in one undo step.

## 3D joints

One `Joint3DComponent` per joint (its own GameObject under bodyA: `set_joint` and the Hierarchy's
Joint 3D put it there). bodyA is Unity's "this", bodyB the connected body; without bodyB the joint
holds bodyA to the world.

| Unity | `type` | main fields |
|---|---|---|
| HingeJoint | `hinge` | `axis`, `normal` (Secondary Axis), `useLimits` + `limitsMin` / `limitsMax` (any range, 10..50 too), `useSpring` + `springStiffness` / `springDamper` / `springTargetPosition`, `useMotor` + `motorTargetVelocity` / `motorMaxForce`, `maxFriction` |
| ConfigurableJoint (slider) | `prismatic` | `axis`, limits in metres, spring, motor (N) |
| SpringJoint | `distance` | `minDistance` / `maxDistance` (-1: the length when made), `useSpring` (soft rope) |
| CharacterJoint | `swingtwist` | `twistAxis`, `planeAxis` (Swing Axis), `twistMinAngle` / `twistMaxAngle` (Low / High Twist), `normalHalfConeAngle` (Swing 1: about the swing axis), `planeHalfConeAngle` (Swing 2: about the normal), `swingType`, motor to `motorTargetRotation` |
| ConfigurableJoint | `sixdof` | `x/y/zMotion`, `angularX/Y/ZMotion` (`locked`, `limited`, `free`; with the limits, live), `linear/angularLimitMin/Max`, drives (`useDrive`; a stiffness springs to the target pose, a damping alone damps towards the target velocity, neither: no drive) |
| FixedJoint | `fixed` | |
| | `point`, `cone` (`coneHalfAngle`, 45), `path`, `gear`, `rack_and_pinion`, `pulley` | |

All types: `enableCollision` (default off), `breakForce`, `breakTorque` (infinite), `breakAction`
(as 2D: `ignore`, `callback_only`, `disable`, `destroy`; the callback is `onJointBreak(force,
joint)`, C++ `onJointBreak(float, Joint3D&)`).

**Anchors.** `anchorSpace: local` (new joints): `anchor` in bodyA's local space, `anchorB` the
connected anchor in bodyB's (a world point without bodyB), `autoAnchors` puts it where `anchor` is
when the joint is made; axes in bodyA's frame. A joint in a prefab works wherever it is placed.
`world` is the old meaning (scenes saved before: world points, the bodies collide); the
Inspector's Convert to Local changes one. **Angles and positions** (`angle`, `position`, limits,
targets) are bodyA's relative to the connected body, measured from the pose the joint is made in,
in the engine's angle unit; a hinge or slider range not containing 0 works (Unity's 10..50).

`set_joint` 3D (`dimension: "3d"` when the bodies do not tell): `anchor` (or `anchor_a`),
`anchor_b`, `axis`, `normal`, `anchor_space`, `enable_collision`, `cone_half_angle`, `motor
{enabled, speed, max}`, `spring {enabled, stiffness, damper, target}`, `limits [min, max] | false`
(hinge angles, prismatic metres, distance min/max, swingtwist twist), `break {force, torque,
action}`, any field through `properties`. In Play, `get_runtime_entity` adds `joint3d` (force,
torque, angle, velocity, position, speed, length, broken) and `rigidbody` (mass, velocities,
sleeping).

## Ragdolls

`create_ragdoll {entity}` (the `physics` tool group; GameObject > 3D Object > Ragdoll..., or the Model's component menu) is
Unity's Ragdoll Wizard on a character root: the bones from its humanoid map (or Mixamo, Unity and
Blender names; override with `bones {pelvis, left_hips, left_knee, left_foot, right_hips, ...,
middle_spine, head}`) get a Rigidbody, a capsule / box / sphere collider and a local swing-twist
joint to their parent bone with Unity's limits, masses summing to `total_mass` (20); the root gets a
`Ragdoll` component. One undo step; running it again rebuilds. `start: animated` (kinematic parts
following the animation) or `simulated`. In a script: `local r = self.object:getRagdoll();
r.simulated = true` (the Animation Player pauses, the parts keep the animation's velocity with
`inheritVelocity`), `r:applyImpulse(impulse, point)` (the nearest part; in the same frame as `simulated = true` it reaches
the now dynamic parts), `r:getParts()`. Simulated pauses everything that moves the bones (the
Animation Player, Model animations, Pose and Vertex Animators) and resumes it after;
`ragdoll.onBeforeAnimatedRestore` is told just before the parts go back to the animation.
`selfCollision` off keeps the parts from touching each other.

## Debug draw and world capacity

`configure_game_view {physics_debug: true}` (Game view toolbar Physics, Lua `Physics.debugDraw =
true`) draws every collider while playing (green awake, dark green asleep, blue kinematic, grey
static, yellow triggers), joints in magenta and, with `physics_debug_contacts`, contact points in
red; 2D too. A screenshot then shows them.

Each scene's 3D world holds at most Project Settings > Physics > 3D World Capacity: Max Bodies,
Max Body Pairs, Max Contact Constraints (8192 each, about 3.6 MB per world; `set_project_settings`
`physics.max_bodies`, `physics.max_body_pairs`, `physics.max_contact_constraints`). Past one, the
console names the setting (once per scene); scenes made after a change use it.

The project settings `physics.enabled` (3D, Jolt) and `physics2d.enabled` (2D, Box2D) switch a
backend off for exports: its bodies and joints are left out of the build. C++ code that must
build either way guards on `JASPERENGINE_PHYSICS_3D` / `JASPERENGINE_PHYSICS_2D`.

## World settings

Scene settings (Inspector > Scene > Physics; `set_scene_settings`; Lua `scene.<name>`), saved only
away from their defaults, which are what the worlds did before they were settings. A change in Play
applies before the next step.

| Inspector | MCP / Lua | default | |
|---|---|---|---|
| Points Per Meter (2D) | `physics_2d_points_per_meter` / `physics2DPointsPerMeter` | 64 | canvas points in a metre: sizes and speeds of 2D bodies are points, masses, forces and the settings below metres. Changed with bodies made, each is made again where it is, as fast in points, its mass from its colliders in the new metres (an explicit mass kept); contacts start again (`OnCollisionExit2D` / `Enter2D`). Set it in the scene, not every frame |
| Max Rotation Per Step (2D) | `physics_2d_max_rotation` / `physics2DMaxRotation` | 45 (degrees) | a 2D body turns at most this much a fixed step (45 at 50 Hz is 39 rad/s); Unity's Max Rotation Speed is 360. A Rigidbody 2D with `allowFastRotation` is not capped |
| World > Contact Hertz, Contact Damping Ratio, Contact Push Speed (2D) | `physics_2d_contact_hertz` (30), `..._contact_damping_ratio` (10), `..._contact_push_speed` (3 m/s) | | contact stiffness, bounciness, how fast overlaps separate |
| World > Joint Hertz, Joint Damping Ratio (2D) | `physics_2d_joint_hertz` (60), `physics_2d_joint_damping_ratio` (2) | | joint stiffness |
| World > Bounce Threshold (2D) | `physics_2d_bounce_threshold` / `physics2DBounceThreshold` | 1 m/s | slower collisions do not bounce (Unity Velocity Threshold) |
| World > Max Linear Speed, Warm Starting, Continuous, Sleeping (2D) | `physics_2d_max_linear_speed` (400 m/s), `physics_2d_warm_starting`, `physics_2d_continuous`, `physics_2d_sleep` (true) | | |
| World > Velocity Iterations, Position Iterations (3D) | `physics_3d_velocity_iterations` (10, 1-64), `physics_3d_position_iterations` (2) | | Jolt's solver iterations (Unity Default Solver Velocity Iterations / Solver Iterations) |
| World > Bounce Threshold, Sleep Threshold, Speculative Distance (3D) | `physics_3d_bounce_threshold` (1 m/s), `physics_3d_sleep_threshold` (0.03 m/s), `physics_3d_speculative_distance` (0.02 m) | | |
| World > Collision Steps (3D) | `physics_3d_collision_steps` / `physics3DCollisionSteps` | 1 | collision passes per fixed step, 1-4: more for very fast bodies |

A frame runs at most `Time.maximumFixedStepsPerFrame` fixed steps: 0 (default) is automatic, as many
as `Time.maximumDeltaTime` lets a frame last (400 Hz with 0.25 s: 101), so none are dropped. A cap
set lower drops the rest of a long frame: the Console warns once per Play and
`Time.droppedFixedSteps` and `editor_status` `physics.dropped_fixed_steps` count them.

## 2D bodies, colliders and materials

`Rigidbody2DComponent` fields (Unity's Rigidbody2D; Catalog names for `set_properties`, `edit_colliders`
`body`, Lua properties of the same name; defaults are what bodies had before these fields):

| field | default | |
|---|---|---|
| `gravityScale` | 1 | share of the scene's 2D gravity (0 floats, -1 falls up) |
| `linearDamping`, `angularDamping` | 0, 0 | Unity Linear / Angular Drag |
| `fixedRotation` | false | never turns (Unity Freeze Rotation) |
| `allowFastRotation` | false | not capped by the scene's Max Rotation Per Step (wheels; "World settings") |
| `collisionDetection` | `discrete` | `continuous`: a Box2D bullet, never tunnels through thin or fast bodies |
| `sleepingMode` | `start_awake` | `never_sleep`, `start_asleep` |
| `useAutoMass`, `mass` | true, 1 | false: `mass` kg instead of area x density (setting Lua `body.mass` turns it off) |
| `autoCenterOfMass`, `centerOfMass` | true, (0,0) | body-local, in collider units (points) |
| `autoInertia`, `inertia` | true, 0 | kg m^2 about the centre of mass (auto: from the colliders, scaled with an explicit mass) |
| `material` | "" | default `.physicsmaterial` of the colliders that have none |

Kinematic and static bodies ignore the mass fields. Live edits (Inspector, `set_runtime_property`,
Lua) apply without rebuilding the body.

**Polygons and chains.** A `polygon` takes any simple outline of 3 or more points, concave too: a
convex one of up to 8 points is one Box2D shape, any other is split into convex pieces (ear
clipping merged into pieces of up to 8 points; the same points always give the same pieces). The
pieces act as one collider: contacts, rays and callbacks report its index. An outline that
crosses itself is an error and its convex hull is used instead. `edit_colliders` reports
`pieces` for each polygon. Pieces have inner seams a rolling wheel can catch on: for ground that
wheels roll over (a hill-climb terrain) use a `chain` (one-sided, no seams; counter-clockwise
collides outside). A `chain` of 4 or more points is a Box2D chain (an open chain does not collide
on its first and last edges); of 2 or 3 points (not a loop of 4+) it is two-sided segments, Unity's
EdgeCollider2D.

**Editing from Lua at run time** rebuilds that collider only (the body keeps its velocity,
joints and the contacts of its other colliders): `body:setColliderPoints(i, points)`,
`setColliderRadius`, `setColliderCenter`, `setColliderSegment`, `setColliderLoop`,
`setColliderMaterial`, `removeCollider(i)` (later indices shift down). A procedural terrain of
500 points rebuilt every few seconds is fine.

**Materials** (`.physicsmaterial`, shared with 3D): `friction` (0 and up; a tyre is about 2),
`bounciness` (0-1), `frictionCombine` and `bounceCombine` (`average`, `geometric_mean`,
`minimum`, `multiply`, `maximum`). Create one with the Project window's Create > Physics Material
(2D preset: friction 0.4, geometric mean / maximum, Unity 6) or `edit_physics_material {path,
create: "2d"}`; set it on a collider (`material`) or on the body (its colliders without one). A
collider's own material wins over the body's, and either replaces the collider's inline
`friction` / `restitution`. Two touching colliders combine with the highest mode of the two in
the order Average < Geometric Mean < Minimum < Multiply < Maximum; a collider without a material
has no mode, and two without one combine as before (geometric mean friction, maximum
restitution). Editing a material while playing (the Inspector, Lua `PhysicsMaterial.load(path)`)
changes every collider using it at the next step, resting contacts included. Lua:
`PhysicsMaterial.load(path)`, `PhysicsMaterial.new{friction = 0, frictionCombine =
PhysicsMaterialCombine.MINIMUM}`, `body:setColliderMaterial(0, m)`; `setColliderFriction` on a
collider with a material gives it its own copy (Unity `collider.material`).

`contactEvents` is on for colliders created from now on (Inspector, MCP, Lua `create*Collider`),
as in Unity; scenes keep the value they saved. The Box2D sub-steps of each fixed step are a scene
setting (`physics_2d_sub_steps`, 1-16, default 4; Lua `scene.physics2DSubSteps`): more make long
joint chains and stiff springs steadier.

## 2D joints

One `Joint2DComponent` per joint, with a `type`; it lives on its own GameObject, under its body
(`set_joint` and the Hierarchy's Joint 2D put it there). bodyA is Unity's "this" (the joint's
Rigidbody 2D), bodyB the connected body; without bodyB the joint holds bodyA to the world.

| Unity | `type` | main fields |
|---|---|---|
| WheelJoint2D | `wheel` | `axis` (suspension direction), `enableSpring` + `frequency` + `dampingRatio` (suspension), `useMotor` + `motorSpeed` + `maxMotorTorque`, `useLimits` + `lower/upperTranslation` |
| HingeJoint2D | `revolute` | `useLimits` + `lowerAngle` / `upperAngle`, `useMotor` + `motorSpeed` + `maxMotorTorque`, `enableSpring` |
| SliderJoint2D | `prismatic` | `axis`, `useLimits` + `lower/upperTranslation`, `useMotor` + `motorSpeed` + `maxMotorForce`, `enableSpring` |
| DistanceJoint2D | `distance` | `length` + `autoLength`, `rope` (Unity maxDistanceOnly), `useLimits` + `minLength` / `maxLength`, motor (with a spring) |
| SpringJoint2D | `distance` with `enableSpring` | `frequency`, `dampingRatio` |
| FixedJoint2D | `weld` | `linearFrequency`, `angularFrequency` (0 = rigid), damping ratios |
| RelativeJoint2D | `motor` | `linearOffset` (bodyB's position in bodyA's frame), `angularOffset` + `autoOffset`, `maxForce`, `maxTorque`, `correctionFactor` |
| FrictionJoint2D | `motor` with `correctionFactor: 0` | `maxForce`, `maxTorque` |
| TargetJoint2D | `mouse` | `target` (world point), `frequency`, `dampingRatio`, `maxForce` (0 = 1000 x mass) |

All types: `enableCollision` (the connected bodies collide; default off), `breakForce`,
`breakTorque` (infinite by default), `breakAction`. New joints get Unity's defaults (wheel: 2 Hz,
0.7, axis (0, 1), max motor torque 10000; hinge limits -90..90 off); changing the type gives the
fields still at the old type's defaults the new type's.

**Units.** Anchors, lengths, translations, limits: points. Angles and limits: degrees (the
engine's angle unit), angular speeds degrees/s (Unity: negative `motorSpeed` drives a wheel
forward). Linear speeds points/s. Forces N, torques N m, frequencies Hz.

**Anchors.** `anchorSpace: local` (new joints): `anchorA` in bodyA's local space, `anchorB` in
bodyB's, `axis` in bodyA's frame, all in GameObject units like collider points; a joint inside a
prefab works wherever the prefab is placed. `autoAnchors` (Unity Auto Configure Connected Anchor,
on by default) puts anchorB where anchorA is when the joint is made, again when bodyB changes at
run time. `world` (scenes saved before local anchors, and the Lua `set*Joint` helpers): the anchors
are world points read when the joint is made, and the bodies' relative angle is not kept (the
Inspector's Convert to Local changes a joint to local). A distance joint's anchors are each body's
own point (`autoAnchors` does not apply).

**Limits are stiff springs, not walls** (Box2D 3). A hinge limit acts on the body's own
rotational inertia: a small heavy bob on a long arm swings a few degrees past it (give the arm
its length, a box or capsule from the pivot). A motor far stronger than the body needs (Unity's
default 10000 on a 1 kg slider) pushes a prismatic or wheel joint past its limit (about 18
points): set `maxMotorForce` / `maxMotorTorque` to what the motion needs.

**Breaking.** After each fixed step a joint whose reaction force passes `breakForce` (N) or torque
passes `breakTorque` (N m) breaks: `ignore` nothing; `callback_only` calls `onJointBreak2D(joint)`
once (again after the load drops below); `disable` sets `enabled = false` at once (the callback
already sees it; the joint leaves the world before the next step) and setting `enabled = true`
again, in the callback or later, makes a fresh joint that can break again; `destroy` (default)
the callback, then the Joint 2D component is removed after every callback of the step. The order
of a fixed step: Box2D steps, the joints are checked (`disable` and `destroy` mark theirs), the
step's contact and trigger events run, then the `onJointBreak2D` callbacks, to the scripts of the
joint's GameObject and of bodyA's; C++ scripts override `onJointBreak2D(Joint2D&)`.

`set_joint` creates (`body_a`, `body_b`, `type`, `name`) or edits (`joint`) one: `anchor_a`,
`anchor_b`, `axis` ([x, y]), `anchor_space` (`local` | `world`), `auto_anchors`,
`enable_collision`, `motor {enabled, speed, max}` (max: torque or force by type), `spring
{enabled, frequency, damping}` (weld: its stiffness), `limits [min, max]` (angles for revolute,
translation for prismatic and wheel, length for distance; `false` turns them off), `break {force,
torque, action}` (null force = never), `rope` (distance), `target` (mouse), and any field (Catalog
name) through `properties`; `dimension` when the bodies do not tell (3D: "3D joints" above). The
result has every resolved field. In Play, `get_runtime_entity` adds
`joint2d` read-outs: `reaction_force`, `reaction_torque`, `joint_angle`, `joint_speed`,
`joint_translation`, `joint_linear_speed`, `motor_torque`, `motor_force`, `current_length`,
`limit_state`; and `rigidbody2d`: mass, velocities, Box2D shapes per collider.

**A hill-climb car** (body box, two wheels on suspension, Unity values):

```json
{"tool": "set_joint", "arguments": {"name": "FrontWheelJoint", "type": "wheel", "body_a": "Car", "body_b": "Car/FrontWheel",
  "anchor_a": [73.8, 19.8], "axis": [0, 1], "spring": {"frequency": 3, "damping": 0.35}, "motor": {"enabled": false, "max": 10000}}}
{"tool": "set_joint", "arguments": {"joint": "Car/FrontWheelJoint", "motor": {"enabled": true, "speed": -720}}}
```

## Interpolation

Bodies are drawn interpolated between their last two fixed steps (scene setting
`physics_interpolation`, Inspector > Scene > Physics > Interpolate, Lua
`scene.physicsInterpolation`; on by default), so they move smoothly whatever the frame rate,
drawn up to one step behind the simulation. A body's own `interpolation` (Rigidbody and
Rigidbody 2D: `scene` by default) overrides it: `none` draws the step poses, `interpolate`, or
`extrapolate` (drawn ahead by its velocity: no lag, can overshoot a contact by one step). A camera or anything else that follows a body
reads the GameObject's world position (`object:getWorldPosition()`, C++ `getWorldPosition()`), not
`body.position`, which moves once per step. Teleports through the body (`body.position`,
`setPosition`) are not interpolated, and a body whose transform a script or animation moved
keeps that pose until the next step.

## Floating in water

A dynamic Rigidbody floats in a water surface (a mesh drawn with `library:water`; the
GameObject menu's 3D Object > Water, `create_entity type=water`) when its `buoyancy` is above 0:
before each fixed step, with its centre of mass over the mesh's world bounds (XZ) and not
deeper than the water's floating depth, it gets Jolt's buoyancy impulse at the surface over
the centre of mass, waves included (the same waves the shader draws, on its clock: the scene
clock, which holds while the game is paused and follows the time scale, so bodies and waves stay
together in a pause and in slow motion).

| Rigidbody field | Default | |
|---|---|---|
| `buoyancy` | 0 | 1 neutral, more floats, less sinks, 0 ignores water; around 1.5 floats with a third out |
| `waterDrag` | 0.5 | Jolt's linear drag in the water |
| `waterAngularDrag` | 0.01 | Jolt's angular drag (about 0.01 times the body's size) |

The water material decides per water: `buoyancy` (on by default; off floats nothing) and
`buoyancyDepth` (world units below the surface a body still floats; 0 = no bottom). Where waters
overlap the one whose bottom is highest wins (a pool with a depth inside a lake without). The
submerged volume comes from the colliders (a body with a mesh or heightfield collider does not
float: Jolt has no volume for them; use convex shapes), the mass from their density. The water itself has no collider: what does not float falls through
it. Scripts read the surface with `Water.getHeight(scene, x, z)` / `Water.getNormal` /
`Water.isUnderwater(scene, point)` (Lua API 2.10.1).

### Water Volumes and Buoyancy

For water with a box (or an ocean), a density, a current and drags, add a **Water Volume**
(`WaterVolumeComponent`, e.g. on the Water GameObject) and give floating bodies a **Buoyancy**
(`BuoyancyComponent`) instead of the Rigidbody `buoyancy` factor:
- The volume's surface is its GameObject's world height at `surfaceOffset`, plus the waves of
  its `library:water` material (`waves: from_material`; the material is the only place their
  values live). Below it the water fills the local box `size` (XZ the extent, Y the depth), or
  everything with `infinite`. Where volumes overlap the highest surface wins.
- A Buoyancy floats its dynamic Rigidbody by Archimedes on columns (2x2, 3x3 on the bottom of
  its colliders' bounds, or `customPoints`): each carries its share of the colliders' volume
  (`volumeScale`), pushed up by `density x |g| x` its submerged part and slowed by the volume's
  `linearDrag` / `angularDrag` (`dragScale`) against the water's own motion (`flow` and the
  waves rising; with `GERSTNER` also rolling back and forth under the crests). A box floats as deep as its density is of the water's. It rides the waves the
  shader draws (one scene clock value a frame). A body with a Buoyancy ignores the Rigidbody `buoyancy`.
- `Water.sample(scene, point)` / `Water.sampleHeight(scene, x, z)` and the `sample_water` tool
  read the volumes first, then the `library:water` meshes; they work in edit mode.
- The inspector warns when the mesh is too coarse for the waves (the drawn surface is linear
  between vertices; things float on the exact curve), when a Buoyancy has no dynamic Rigidbody
  or no volume to float in, and when it is as dense as the water.

### Boats: Water Excluders and Water Wakes

- A **Water Excluder** (`WaterExcluderComponent`) keeps water out of a boat's hull, a cabin, a
  dry tunnel: shape `convex_hull` (default: the hull of the GameObject's own mesh or its model's
  mesh nodes), `box` or `sphere` (`center`, `size`, local; Inspector **Fit to Mesh**). A point
  inside an active one is dry: no buoyancy (neither path: a Buoyancy's columns, the Rigidbody
  `buoyancy` factor's centre of mass), `Water.sample` gives `inWater` false with `excluder` set
  (the surface stays: the waterline outside), `Water.isUnderwater` is false, and a camera inside
  sees no underwater fade; the water surface inside is not drawn.
- An excluder never dries the body it belongs to: the nearest GameObject with a Rigidbody at or
  above it. Put the boat's excluder on its Rigidbody's GameObject (or under it): cargo with its
  own Rigidbody inside stays put while the boat floats. A static one (no Rigidbody above) dries
  everything, the Inspector warns when a floating body is under it.
- Disabled, inactive or hidden (Transform `visible` off): it keeps nothing out.
- A **Water Wake** (`WaterWakeComponent`) leaves foam and waves behind a GameObject moving through
  a water's surface in a running scene: `foam`, `waves` (0..2), `width`, `length` (metres along
  local X and Z; 0: the excluder's box or the mesh's). The wakes are visual (the physics does not
  feel them), fade over the scene's `water_wake_fade`, pause with the game and clear at Stop.
- Check: `sample_water` gives each point `excluded_by` and `wake` {foam, height}; `report: true`
  adds the excluders (`hull_status`: ready, pending, no_mesh, flat, no_physics), the wakes
  (`emitting`, `speed`) and which waters draw with them.
## Collision and trigger callbacks

Define the Unity methods on a script of the body's GameObject; no registration, no flag:

```lua
function Coin:onTriggerEnter2D(other)            -- other: Collider2D
    if other:compareTag("Player") then GameObject.destroy(self.gameObject) end
end
function Car:onCollisionEnter2D(c)               -- c: Collision2D
    if c.gameObject.tag == "Ground" and c.thisCollider.index == 1 then GameOver(self) end
end
function Shell:onCollisionEnter(c)               -- c: Collision (3D)
    local body = Rigidbody(self.scene, self.entity)
    body.linearVelocity = body.linearVelocity:reflect(c:getContact(0).normal)
end
```

| 3D | 2D | gets |
|---|---|---|
| `onCollisionEnter/Stay/Exit` | `onCollisionEnter2D/Stay2D/Exit2D` | `Collision` / `Collision2D`, seen from this side |
| `onTriggerEnter/Stay/Exit` | `onTriggerEnter2D/Stay2D/Exit2D` | the other `Collider` / `Collider2D` |

C++ `ScriptBase` scripts override the virtuals of the same names (`onCollisionEnter(const
Collision&)`, `onTriggerEnter2D(const ColliderRef2D&)`...).

**When.** On the main thread, right after the fixed step that caused them (Unity: after
FixedUpdate and the step): 2D first, then 3D; triggers then collisions; Enter, Stay, Exit; pairs
in GameObject order. Once per fixed step (several steps in a frame: several rounds; time scale 0
or paused physics: none). Every script of the GameObject that defines the method gets it, **enabled
or not** (Unity), while the GameObject is active in hierarchy. A collider's `receiver` (Inspector
"Send Callbacks To", `edit_colliders` `receiver`) also gets that collider's callbacks: Unity's child
colliders with their own scripts. Collisions need a dynamic body on one side (or a kinematic one
with `collideKinematicVsNonDynamic`); triggers (a 2D collider's `sensor`, a 3D collider's `isTrigger` or body `sensor`) also
fire with kinematic and static bodies in 3D. One callback per collider pair: a trigger and a
solid collider of one body touching the same collider give both.

**The rules.**
- Sleeping does not end a contact: no Exit when bodies fall asleep, no Stay while they sleep (2D:
  Stay while either body is awake); a pair that no longer touches when it wakes gets its Exit then.
- Rebuilding a body (a collider edit, a scale change) gives no Exit / Enter while it still touches.
- When the other side goes, you always get the Exit: destroyed (`GameObject.destroy` at the end of
  the frame, `destroyImmediate`, Rigidbody removed) at once, while it still exists
  (`other.gameObject` is valid and named); deactivated, body disabled, collider removed, layers or
  `ignoreCollision` changed: at the end of the next step. The departing object gets nothing. Scene
  unload and Play stop send nothing.
- `GameObject.destroy` in a callback lets the step's other callbacks reach the object; so does
  `destroyImmediate` in a contact callback: the callbacks run while the physics system works, so
  the object goes when the step's callbacks are done (and its partners get their Exit then).
  Changes made in a callback (teleports, colliders, layers) apply at the next step.
- An error in a callback is logged (once a second per script and method) and the others still run.
  Callbacks cannot yield: start a coroutine for waits.

**The data** is a copy taken at the step, safe to keep:

| class | members |
|---|---|
| `Collision` | `gameObject`, `rigidbody`, `collider` (the other's), `thisCollider`, `thisRigidbody`, `impulse` (Vector3, N s, Jolt's estimate), `relativeVelocity` (other minus this: a ball landing sees +y), `contactCount`, `getContact(i)` (0-based), `contacts` (a new table each read: prefer `getContact`) |
| `ContactPoint` | `point`, `normal` (from the other collider toward this one: `v:reflect(normal)` bounces), `separation` (< 0 penetrating), `impulse`, `thisCollider`, `otherCollider` |
| `Collision2D` | `gameObject`, `collider`, `rigidbody` (the other's), `thisCollider`, `thisRigidbody`, Unity's `otherCollider` / `otherRigidbody` (**this** side, as in Unity), `relativeVelocity` (points/s; Enter: before the step), `enabled`, `contactCount`, `getContact(i)`, `contacts` |
| `ContactPoint2D` | `point`, `normal`, `separation` (points), `normalImpulse`, `tangentImpulse`, `relativeVelocity`, `thisCollider`, `otherCollider` |
| `Collider` / `Collider2D` | `gameObject`, `rigidbody` (`attachedRigidbody`), `index`, `type`, `isTrigger`, `tag`, `compareTag(tag)`, `receiver`, `isValid()` (false once its GameObject or collider is gone; `gameObject` is then nil, or still there for a removed collider) |

At most 4 contact points per 3D manifold and 2 per Box2D shape pair, 8 per collider pair (a mesh
collider or a concave polygon merges its pieces'). 2D impulses are Box2D's of the step; 3D ones
are estimates. A contact disabled by `preSolve2D` is not touching (Exit, then Enter when enabled).

**Cost.** Only bodies whose scripts (or receivers) define a method are recorded; the rest are
never recorded (one mask test per contact on Jolt's job threads). Agents: `get_runtime_entity` lists `physics.contacts` (other, collider indices, trigger,
`since_step`, asleep) and `physics.callbacks` (the methods found, with their scripts); the
Inspector shows the same under each body in Play.

## Layers, contacts and queries

- Each backend has a Layer Collision Matrix (Project Settings > Physics / Physics 2D), saved in
  project.yaml as the layer pairs that do not collide (`ignoredLayerCollisions`). Pairs it
  allows still need matching collider category/mask bits (`setBitsFilter`, 2D group index).
- `Physics.ignoreLayerCollision` / `Physics2D.ignoreLayerCollision` change it at runtime (in the
  editor, until Play stops). Changing a GameObject's layer re-filters its body.
- `Physics.ignoreCollision(a, b [, ignore = true])` / `Physics2D.ignoreCollision` (GameObjects or
  bodies of one scene): those two bodies never collide, at run time (Unity
  `Physics.IgnoreCollision`); `getIgnoreCollision(a, b)`. Resting pairs are woken. The replacement
  for a Lua `shouldCollide3D` filter.
- Ray casts: `Physics.raycast(scene, ray [, layerMask [, options]])`,
  `Physics.raycast(scene, origin, direction, maxDistance [, layerMask [, options]])`, `raycastAll`,
  and the `Physics2D` ones with `Vector2`. Masks from `LayerMask.getMask(...)`, `Physics.ALL_LAYERS`;
  default is every layer but Ignore Raycast. With a `Ray`, distance is a fraction of
  `ray.direction`; with origin/direction/distance it is in world units. A 3D ray that starts inside
  a collider does not hit it (Unity); `{hitFromInside = true}` reports it at distance 0.
  `Ray:intersects(scene, ...)` keeps its old rule (the start's collider at 0).
- What the player sees, not its colliders: `MeshRaycast.raycast` / `raycastAll` take the same
  arguments (plus an options table) and hit the triangles of meshes, instances, terrains and UI
  rects, no body needed, also in the editor (`raycast_meshes`). Use Physics for gameplay against
  colliders; MeshRaycast for aiming, placement and line of sight on visible geometry.
  `jasper://docs/scene-view` has the details.
- **Global contact events** (every pair of the scene: filter by body; the per-object callbacks
  above are simpler): 2D `beginContact2D`, `endContact2D`, `stayContact2D` (once a
  step per touching shape pair, from the step after its begin, while a body is awake), `hitContact2D`,
  `beginSensorContact2D`, `endSensorContact2D`, `staySensorContact2D` fire for colliders with
  `contactEvents` on (the default for new colliders; sensors always), `preSolve2D` / `shouldCollide2D`
  inside the step. 3D `onContactAdded3D`, `onContactPersisted3D`, `onContactRemoved3D`,
  `onBodyActivated3D`, `onBodyDeactivated3D` run on the main thread after the step, with a
  `Contact3D` snapshot (its setters apply from the next step on, for the rest of that contact). `physicsSystem.synchronousContactEvents3D
  = true` steps the scene's 3D world on the main thread and calls them inside the step with live,
  settable contacts (slower: one thread). A Lua `shouldCollide3D` turns that on by itself, since it
  must run inside the step (prefer `Physics.ignoreCollision` or layers). C++ contact modification
  (conveyor-belt friction): `PhysicsSystem::onContactModify3D`, on Jolt's job threads.
- 2D limitation (Box2D): a kinematic sensor never pairs with static or kinematic bodies, and two
  sensors never pair; make the other body dynamic (gravity scale 0) to detect it.

### Shape casts and overlaps

Unity's queries, on the playing scene's colliders (bodies exist only in Play), all with
`[, layerMask [, options]]` last:

| 3D (`Physics`) | 2D (`Physics2D`, points, angles in the engine's unit) | Returns |
|---|---|---|
| `sphereCast(scene, origin, radius, direction, maxDistance)` | `circleCast(scene, origin, radius, direction, distance)` | the nearest `RaycastHit` or nil |
| `boxCast(scene, center, halfExtents, direction, orientation, maxDistance)` | `boxCast(scene, origin, size, angle, direction, distance)` | " |
| `capsuleCast(scene, point1, point2, radius, direction, maxDistance)` | `capsuleCast(scene, origin, size, CapsuleDirection2D.VERTICAL, angle, direction, distance)` | " |
| `...CastAll` | `...CastAll` | every collider hit once, nearest first |
| `overlapSphere(scene, center, radius)`, `overlapBox(scene, center, halfExtents [, orientation])`, `overlapCapsule(scene, point1, point2, radius)` | `overlapCircle`, `overlapBox(scene, point, size, angle)`, `overlapCapsule`, `overlapPoint(scene, point)`, `overlapArea(scene, pointA, pointB)` | 3D: every `Collider`; 2D: the first `Collider2D` (nil), `...All` every one |
| `checkSphere`, `checkBox`, `checkCapsule` (the overlap's arguments) | | boolean |
| `closestPoint(scene, point, collider)` (convex colliders) | | Vector3 |
| `rigidbody:sweepTest(direction, maxDistance [, options])`, `sweepTestAll` | `rigidbody2d:cast(direction, distance [, options])`, `castAll` | the body's own colliders moved, never hitting itself |

- options: `ignore` (GameObjects, bodies or entities: a caster's own body), `triggers`
  (`QueryTriggerInteraction.USE_GLOBAL` = `Physics.queriesHitTriggers` / `Physics2D.queriesHitTriggers`,
  true; `IGNORE`; `COLLIDE`), 3D `hitFromInside` and `hitBackfaces` (`Physics.queriesHitBackfaces`,
  false), 2D `startInColliders` (`Physics2D.queriesStartInColliders`, false: unlike Unity's default
  true, a ground check from inside the player's own collider does not hit the player), `maxHits` (256).
- A cast that starts in a collider skips it, or with `hitFromInside` / `startInColliders` reports it
  at distance 0 with `point` = the start and `normal` = -direction.
- A hit (`RayReturn`, Unity's `RaycastHit`) has `hit`, `distance`, `point`, `normal`, `body`,
  `colliderIndex`, and `collider` (`Collider` / `Collider2D`), `gameObject`, `rigidbody`, `transform`
  (the GameObject); a CharacterController is hit as its GameObject with `rigidbody` nil (2D
  controllers have no body and are never hit).
- Zero, negative or non-finite sizes, distances and directions raise an error naming the argument.
- An agent asks the same of a running game with `query_physics` (`physics` group; `draw: true` shows
  the shape and hits for 3 s in the Game view's physics lines): "is anything within 2 m of the
  player" is `query_physics {shape: "sphere", mode: "overlap", origin, radius: 2}`, not a fan of rays.
  Its arguments per shape: `origin` (`[x, y]` in 2D), casts `direction` + `max_distance`; sphere
  `radius`; 3D box `half_extents` + `rotation`, 2D box `size` + `angle` or the corners `point_a` +
  `point_b` (overlap, check); 3D capsule `point_a` + `point_b` + `radius`, 2D capsule `size` +
  `angle`. `layer_mask` takes a number or layer names; `hit_from_inside` is 2D's
  `startInColliders` too. It hits colliders; `raycast_meshes` hits drawn triangles.

## Character controllers

A Character Controller (`CharacterControllerComponent`, Unity's) is a kinematic capsule moved by
script on Jolt's CharacterVirtual. `cc:move(motion)` sweeps it at once (world units, no gravity)
and returns `CollisionFlags` (SIDES 1, ABOVE 2, BELOW 4); `cc:simpleMove(speed)` ignores `speed.y`,
adds its own gravity and returns `isGrounded`. Move in `onUpdate` or `onFixedUpdate` (dt is the
fixed step there), never from a contact callback.
- It climbs steps up to `stepOffset`, sticks to the floor going down them, slides on walls and
  treats surfaces steeper than `slopeLimit` as walls. `isGrounded` is Jolt's OnGround: on a too
  steep surface it is false and `isOnSteepSlope` is true (Unity's isGrounded is any contact below).
- After every physics step it rides what it stands on (`movingPlatforms`, turning with it with
  `inheritPlatformYaw`), whether or not a script moved it, and `isGrounded` follows a platform
  that drops away. `move`'s displacement never includes the platform's.
- It pushes dynamic bodies with up to `pushStrength` N (`pushBodies`); with `detectCollisions`
  a kinematic inner body lets rays (`Physics.raycast` returns the controller), triggers and
  dynamic bodies see it. Its contacts with bodies are not in `onContactAdded3D` (no Rigidbody
  there); a trigger's are, with `Rigidbody.valid` false on the controller's side.
- Controllers block each other with `characterCollisions` (both on).
- `onControllerColliderHit(hit)` in its Lua scripts (and `PhysicsSystem.onControllerColliderHit`)
  gets each body or controller a move touched: `hit.entity`, `point`, `normal`, `moveDirection`,
  `moveLength`.
- Write its Transform to teleport it. A new `height`/`radius`/`center` while playing is tried
  at once (`cc:trySetSize(h, r, c)` returns false and keeps the old one when it would not fit).
- `PhysicsSystem:setRootMotion(entity, worldVelocity, yawRate)` (S21's Animator) moves and turns
  it in every fixed step until the next call.
- Never on a GameObject with a Rigidbody: Add Component and `add_component` refuse, and one loaded
  with both does nothing and logs once.

A Character Controller 2D (`CharacterController2DComponent`) is a kinematic, upright capsule moved
by script: `cc:move(motion)` slides it at once (no gravity) and returns `CollisionFlags`;
`cc:simpleMove(speed)` adds its own gravity and returns `isGrounded`. Call them in `onUpdate` or
`onFixedUpdate`, never from a contact callback. Box2D 3.0 has no mover, so the engine slides the
capsule on Box2D's casts (at most 4 iterations a move). It climbs steps up to `stepOffset`
(points), treats surfaces steeper than `slopeLimit` as walls (`isOnSteepSlope`), keeps
`skinWidth` off colliders, sticks to the floor going down steps, is carried by the kinematic or
dynamic body it stands on (`movingPlatforms`) and pushes dynamic bodies it walks into
(`pushBodies`: an impulse `pushStrength x dt` along the move; not what it stands on).
- It has no Box2D body: ray casts, sensors and other controllers do not see it, and it does not
  turn with a rotating platform. There is no `onControllerColliderHit` in 2D.
- It cannot share a GameObject with a Rigidbody 2D: Add Component and `add_component` refuse;
  one loaded with both does nothing and logs once.
- Its layer, the 2D Layer Collision Matrix and `categoryBits`/`maskBits` filter what it hits.
- `get_runtime_entity` shows `isGrounded`, `collisionFlags`, `velocity`, `groundEntity`, ...

## Vehicles (Wheel Collider)

A drivable car is a dynamic Rigidbody (its colliders are the chassis) with a Wheel Collider
(`WheelColliderComponent`, Unity's) on a child GameObject at each wheel: not four bodies on hinge
joints. GameObject > 3D Object > Vehicle... (`run_command` `gameobject.create.3DObject.vehicle`)
makes one on the selected mesh, or a new 2 x 1 x 4 m car, in one undo step.
- The wheels under one dynamic body (its nearest Rigidbody above them) make one vehicle on Jolt's
  vehicle constraint, built again at the next step when a wheel, the drivetrain or the body
  changes. A Wheel Collider on a GameObject with a Rigidbody, or with no dynamic body above it,
  does nothing and logs once.
- The suspension hangs from the GameObject's position + `center` down the body's down axis by
  `suspensionDistance`; the wheel rolls along the GameObject's forward (+Z). It is not a collider:
  a ray down the suspension (Unity's) finds the ground through the body's layer and the Layer Collision Matrix, and
  never hits the body or the bodies below it. Wheels send no contact callbacks.
- Unity's fields and defaults (metres, kg, N, N m): `radius` 0.5, `mass` 20, `wheelDampingRate`
  0.25, `suspensionDistance` 0.3, `forceAppPointDistance` 0, `suspensionSpring` {`spring` 35000,
  `damper` 4500, `targetPosition` 0.5: where it rests under the car's weight},
  `forwardFriction` / `sidewaysFriction` (WheelFrictionCurve: extremum, asymptote, `stiffness`;
  forward slip is the slip ratio, sideways slip the slip angle in radians). The grip is the
  curve's on default surfaces; the body's and the ground's friction scale it as any contact
  combines them (relative to the default 0.2). Jolt's tire model is not PhysX's: a car tuned in
  Unity may need its friction `stiffness` changed by about 30%.
- Without a Vehicle Drivetrain each wheel takes its own `motorTorque` (N m), `brakeTorque` (N m)
  and `steerAngle` (positive right), set by a script every frame or with `set_runtime_property`.
  Read-outs of the last step: `isGrounded`, `rpm`, `sprungMass`, `suspensionCompression`,
  `wc:getGroundHit()` (a WheelHit or nil: `point`, `normal`, `forwardDir`, `sidewaysDir`, `force`,
  `forwardSlip`, `sidewaysSlip`, `collider`) and `wc:getWorldPose()` (position, rotation).
- `driveTransform` (on by default) puts the wheel's GameObject at `getWorldPose()` after each
  step (suspension, steer, spin), so the wheel mesh goes on a child of it and follows,
  interpolated with the body. Turn it off to place a mesh yourself.
- A Vehicle Drivetrain (`VehicleDrivetrainComponent`, on the body; beyond Unity) drives the wheels
  with an engine (`maxTorque`, `minRPM`, `maxRPM`, `torqueCurve`), a gear box (`transmission`
  AUTO or MANUAL with `setGear(n)`, `gearRatios`, `reverseGearRatios`, shift RPMs,
  `clutchStrength`), `differentials` (wheel pairs; none: every axle driven) and `antiRollBars`.
  Its inputs `throttle` (-1..1; AUTO reverses below 0), `steer` (-1..1), `brake`, `handBrake`
  (0..1) act within each wheel's `maxSteerAngle`, `maxBrakeTorque` and `maxHandBrakeTorque`; the
  wheels' `motorTorque`, `brakeTorque` and `steerAngle` then read what it applied. Read-outs:
  `rpm`, `gear`, `speed` (m/s along the body's forward).
- At rest with no input the car sleeps; any changed input wakes it. A wheel in the air keeps it
  awake. Moving the body (`body.position`, its Transform) takes the wheels with it.
- `Physics.debugDraw` draws each wheel's circle and travel; the Scene view gizmo shows the circle,
  the travel and, in Play, the contact and its force.

## Placing objects with physics (`place_with_physics`)

Edit-time placement, like the Scene view's Physics tool (G): a private simulation moves the
GameObjects you name and settles them against what is around them. The scene's own physics
never runs, nothing is added to the scene, and Rigidbodies are not needed. Use it to drop props
onto floors, tables and terrain, scatter objects, push overlapping ones apart, slide something
against a wall, or rest it on the surface a ray hits. 3D scenes only, not during Play.

- **Movers** (`entities`, or `use_selection: true` for the editor's selection): the top-most
  roots; a listed child of a listed parent moves with it. Each mover is one body made of its
  subtree: its authored colliders, else a fitted box, sphere or capsule, else a convex hull, or
  a convex decomposition for concave meshes (`decompose`). Objects longer than 25 m are never
  movers (`refused`, they stay context).
- **Context**: everything else within `context_margin` (default 2 m) of the movers' path is
  static: meshes as triangle meshes, terrains as height fields, authored colliders as authored.
  `context: auto` (default) takes what is around the movers; `context: none` drops everything onto nothing; `context: [...]` uses only those GameObjects;
  `ignore_layers` leaves layers out. `push_neighbours: true` lets small loose objects in the way
  be knocked aside (they are committed too).
- Each call is **a preview** (nothing changes) unless `apply: true`. `apply: true` with the
  same arguments commits exactly the previewed poses (`from_preview: true`, same `state_hash`)
  as one undo step, prefab overrides recorded; if the scene changed in between (any edit, undo
  or redo), it simulates again (`from_preview: false`). Read `hint` and `warnings`.

| mode | needs | does |
|---|---|---|
| `drop` (default) | | gravity (`gravity`, default `[0, -9.81, 0]`) until every mover rests |
| `separate` | | no gravity: overlapping movers are pushed apart until no pair overlaps more than 1 mm |
| `move` | `target` | the group slides rigidly toward `target.position` (where its centre goes) or by `target.delta`, turned by `target.rotation` (`[x,y,z,w]` or `{"euler": [x,y,z]}`) about its centre; it stops at what it hits, then settles. `space: local` reads them in the first mover's parent space |
| `snap` | `ray` | the movers land where `ray` hits (`{origin, direction}` in world space, up to 1 km, or `{screen: [x, y]}`, a Scene view pixel, y down), placed on that surface (aligned to its normal) directly, so nothing between them and the hit stops them, then settled; a ray that hits nothing is an error |
| `gather` | | pulled toward their common centre (`amount`, 1 = all the way), stopping on contact, then settle |
| `distribute` | `axis` | spaced evenly along `x`, `y` or `z` between the outermost two, keeping their order, then settle |

`settle: false` stops after the gesture instead of running until resting. `max_seconds`
(default 10, at most 120) caps the simulated time. `feel` (`precise`, `balanced`, `fast`) trades accuracy for
speed when moving. `jitter: {position, rotation_deg}` offsets (up to `position` m) and turns (up to `rotation_deg` degrees)
every mover by a random amount (from `seed`, default 1) before the first step.

**Drop a crate onto the table under it, check, then commit:**

```json
{"tool": "place_with_physics", "arguments": {"entities": ["Props/Crate"]}}
{"tool": "place_with_physics", "arguments": {"entities": ["Props/Crate"], "apply": true}}
```

**Scatter: make the objects, then drop them with jitter, in one undo step.** Composition, not a
separate tool:

```json
{"tool": "batch", "arguments": {"undo_group": true, "calls": [
  {"tool": "instantiate_prefab", "arguments": {"prefab": "prefabs/Rock.prefab", "position": [0, 4, 0]}},
  {"tool": "instantiate_prefab", "arguments": {"prefab": "prefabs/Rock.prefab", "position": [0.6, 5, 0.3]}},
  {"tool": "instantiate_prefab", "arguments": {"prefab": "prefabs/Rock.prefab", "position": [-0.4, 6, -0.2]}},
  {"tool": "place_with_physics", "arguments": {"entities": ["Rock", "Rock#1", "Rock#2"],
    "jitter": {"position": 0.5, "rotation_deg": 180}, "seed": 3, "apply": true}}
]}}
```

The same batch with the same `seed` gives the same arrangement; another seed another one.
`sculpt_terrain` with `place_object` scatters over a terrain instead of over a spot.

**Push apart objects that intersect** (after a sloppy layout, before a drop):

```json
{"tool": "place_with_physics", "arguments": {"entities": ["Books/Book", "Books/Book#1", "Books/Book#2"], "mode": "separate", "apply": true}}
```

**Slide a sofa against the wall:** aim anywhere behind the wall; a move never jumps through
what is in the way, it stops at the wall.

```json
{"tool": "place_with_physics", "arguments": {"entities": ["Room/Sofa"], "mode": "move", "target": {"delta": [5, 0, 0]}, "apply": true}}
```

**Snap onto the surface under a point:** a world ray, or a pixel of the Scene view (the size
`screenshot` with `view: scene` and `max_width: 0` returns; a `ray.screen` call does not reframe
the view, so the same pixel means the same ray on the apply call).

```json
{"tool": "place_with_physics", "arguments": {"entities": ["Lamp"], "mode": "snap", "ray": {"origin": [2, 10, -1], "direction": [0, -1, 0]}}}
{"tool": "place_with_physics", "arguments": {"entities": ["Lamp"], "mode": "snap", "ray": {"screen": [640, 360]}, "screenshot": true}}
```

**The result** (`structuredContent`):

| field | meaning |
|---|---|
| `applied` | false for a preview |
| `from_preview` | the apply committed the stored result of the preview with the same arguments (the editor keeps the last 8 previews, dropped when their scene's GameObjects change, the scene closes or another project opens); false when it simulated |
| `mode`, `seed`, `state_hash` | what ran; the same arguments give the same `state_hash` |
| `sim_seconds`, `steps`, `all_settled` | simulated time, fixed steps of 1/120 s, every body at rest |
| `entities[]` | `{id, name, path, before, after, moved_m, rotated_deg, settled, settle_step, resting_on, max_penetration_m, collider, warnings}`: world poses (`position`, `rotation` as `[x,y,z,w]`) and `after.local`, the parent-space pose that is committed |
| `context` | `{bodies, region}`: the static bodies used and the region they were gathered from |
| `refused` | movers that could not move, with the reason ("no mesh or collider", "too large (> 25 m)...") |
| `truncated`, `entities_total` | a very large result (over 256 KB) lists its first entities only; `state_hash`, `all_settled` and the warnings still cover them all |
| `warnings`, `hint` | what to do next: an initial overlap deeper than 5 cm (run `separate` first), nothing to land on under a mover (move it over a surface, or add one below), movers simulated with a caveat (skinned meshes in their bind pose, joints ignored), bodies that fell out of the world or never came to rest within `max_seconds`, a mesh still loading |
| `ray` | snap: the world ray used |
| `screenshot` | with `screenshot`: what the image shows |

**Determinism.** Same editor binary, same platform, same scene and the same arguments give the
same `state_hash` and poses: every call builds a fresh world, adds the movers in path order,
steps a fixed 1/120 s, applies the seeded jitter before the first step and reads no clock. It
is not promised across platforms or builds. With `decompose: cached` (the default) a concave
mover's first run asks for its decomposition in the background and uses a hull meanwhile (a
warning says so): a later preview uses the decomposition and can differ. The apply right after
a preview is not affected (it commits the stored preview), but one that simulates again is: for
runs that repeat exactly, pass `decompose: always` (waits for the movers' decompositions, progress
"Decomposing 2/5", one per mover; other decompositions queued in the project are not counted)
or `never`.

**Long runs** are jobs: the call waits up to `timeout_seconds` (default 120), then returns a
`job_id` for `get_job` (progress "step 140/1200, 22 of 30 asleep"); `cancel_job` stops it and
nothing changes. Once an apply has committed (it may still be taking its screenshot), a cancel
comes too late: the job succeeds with the note "already committed; use undo to revert". A run
fails, changing nothing ("the scene changed during the simulation: run it again"), when the
scene is edited while it runs (a GameObject moved or added, an undo or redo), the scene closes,
Play starts or another project opens. While the Scene view's Physics tool is in a gesture or
settling in that scene, a run waits for it to end (its preview poses are in the scene).

**Screenshot** (`screenshot: true` or `{max_width, frame}`): the Scene view, framed on the movers
(`frame: false` keeps the camera). A preview's image shows the previewed poses: they are put in
the scene for the few frames of the capture and taken out again (also before a save or Play
that comes meanwhile), without an undo step. With `wait: false` there is no image to return.

## 2D lighting (not physics, but often confused)

A Light 2D (`light_2d`) adds on top of the scene's 2D ambient (`ambient_light_2d_color`,
`ambient_light_2d_intensity`): dim the ambient to see lights. A Shadow Caster 2D with shape
AUTO_QUAD takes its outline from a sibling mesh, POLYGON uses its own points; enable the
light's `shadows` to cast. Shadow smoothness is per scene: `shadows_quality` (3D) and
`shadows_2d_quality` (2D), 0 none .. 3 high.
