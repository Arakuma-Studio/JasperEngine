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

## Mass

Mass is never authored directly; it comes from the collider's size and density.

| | 3D (Jolt) | 2D (Box2D) |
|---|---|---|
| formula | volume x density | area (in m^2) x density |
| default density | 1000 (kg/m^3, water) | 1 |
| units | meters | collider sizes in points, scaled by `pointsToMeterScale2D` (64 points per meter by default) |
| example | radius-1 sphere ~ 4189 kg, radius-0.5 ~ 524 kg | 100x100 point box ~ 2.4 kg |
| read it (C++) | `getMass()` | `getMass()` |
| read it (Lua) | `body.mass` (property) | `body:getMass()` (method) |

Size forces as `F = mass x desired acceleration` using the real mass: a few hundred newtons
on a multi-tonne body looks like a broken script. If movement needs implausibly large forces
or feels sluggish, lower the collider density (or shrink the collider) instead of inflating
the force. `Rigidbody::setMass` only writes the live Jolt body and is lost on reload: persist
mass by authoring density.

## Forces, velocity, teleport

- Continuous forces and torques MUST be applied in `onFixedUpdate`, never `onUpdate`: each
  world accumulates force calls until its next fixed step, so per-frame calls make the force
  depend on the frame rate. Velocity control and held-input polling that drives physics belong
  in `onFixedUpdate` too.
- Do not multiply forces or torques by deltatime: the engine integrates them over the step.
- One-shot impulses are emitted once per press, not every held frame.

| | Rigidbody (3D) | Rigidbody2D |
|---|---|---|
| force at center | `applyForce(force)` | `applyForceToCenter(force, wake)` |
| force at point | `applyForce(force, point)` | `applyForce(force, point, wake)` (no 1-argument form) |
| torque | `applyTorque(torque)` | `applyTorque(torque, wake)` |
| impulse | `applyImpulse(impulse)` | `applyLinearImpulseToCenter(impulse, wake)` |
| velocity | `linearVelocity` (Lua property) / `setLinearVelocity` (C++) | `linearVelocity` (Lua, points/s) |
| teleport | `body.position`, `body.rotation` (Lua) / `setPosition`, `setRotation` (C++) | `body.position`, `body.angle` (Lua) / `setPosition`, `setAngle` (C++) |

There is NO `applyForceToCenter` on the 3D `Rigidbody`. In Lua pass every argument (no C++
default arguments): a missing `wake` reads as false.

- Never move a body by writing its GameObject's transform from `onFixedUpdate`
  (`object.position = ...`, `setPosition`): the body is synced from the transform before the
  step and written back after it, so the write is silently discarded. Teleport through the body.
  GameObjects without a body have no such restriction.
- Never describe transform-only movement as physics.

## Layers, contacts and queries

- Each backend has a Layer Collision Matrix (Project Settings > Physics / Physics 2D), saved in
  project.yaml as the layer pairs that do not collide (`ignoredLayerCollisions`). Pairs it
  allows still need matching collider category/mask bits (`setBitsFilter`, 2D group index).
- `Physics.ignoreLayerCollision` / `Physics2D.ignoreLayerCollision` change it at runtime (in the
  editor, until Play stops). Changing a GameObject's layer re-filters its body.
- Ray casts: `Physics.raycast(scene, ray [, layerMask])`,
  `Physics.raycast(scene, origin, direction, maxDistance [, layerMask])`, `raycastAll`, and the
  `Physics2D` ones with `Vector2`. Masks from `LayerMask.getMask(...)`, `Physics.ALL_LAYERS`;
  default is every layer but Ignore Raycast. With a `Ray`, distance is a fraction of
  `ray.direction`; with origin/direction/distance it is in world units.
- Contacts in Lua: `RegisterEvent(self, self.scene:getPhysicsSystem().beginContact2D, "onHit")`
  fires for every contacting pair in the scene (filter by body).

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

## 2D lighting (not physics, but often confused)

A Light 2D (`light_2d`) adds on top of the scene's 2D ambient (`ambient_light_2d_color`,
`ambient_light_2d_intensity`): dim the ambient to see lights. A Shadow Caster 2D with shape
AUTO_QUAD takes its outline from a sibling mesh, POLYGON uses its own points; enable the
light's `shadows` to cast. Shadow smoothness is per scene: `shadows_quality` (3D) and
`shadows_2d_quality` (2D), 0 none .. 3 high.
