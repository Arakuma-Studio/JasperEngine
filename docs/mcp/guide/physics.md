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

## Floating in water

A dynamic Rigidbody floats in a water surface (a mesh drawn with `library:water`; the
GameObject menu's 3D Object > Water, `create_entity type=water`) when its `buoyancy` is above 0:
before each fixed step, with its centre of mass over the mesh's world bounds (XZ) and not
deeper than the water's floating depth, it gets Jolt's buoyancy impulse at the surface over
the centre of mass, waves included (the same waves the shader draws, on its clock).

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

## 2D lighting (not physics, but often confused)

A Light 2D (`light_2d`) adds on top of the scene's 2D ambient (`ambient_light_2d_color`,
`ambient_light_2d_intensity`): dim the ambient to see lights. A Shadow Caster 2D with shape
AUTO_QUAD takes its outline from a sibling mesh, POLYGON uses its own points; enable the
light's `shadows` to cast. Shadow smoothness is per scene: `shadows_quality` (3D) and
`shadows_2d_quality` (2D), 0 none .. 3 high.
