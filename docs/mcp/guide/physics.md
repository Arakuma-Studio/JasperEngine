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
  `context: none` drops everything onto nothing; `context: [...]` uses only those GameObjects;
  `ignore_layers` leaves layers out. `push_neighbours: true` lets small loose objects in the way
  be knocked aside (they are committed too).
- Each call is **a preview** (nothing changes) unless `apply: true`. The same arguments with
  `apply: true` commit exactly the previewed poses (same `state_hash`) as one undo step, prefab
  overrides recorded. Read `hint` and `warnings`.

| mode | needs | does |
|---|---|---|
| `drop` (default) | | gravity (`gravity`, default `[0, -9.81, 0]`) until every mover rests |
| `separate` | | no gravity: overlapping movers are pushed apart until no pair overlaps more than 1 mm |
| `move` | `target` | the group slides rigidly toward `target.position` (where its centre goes) or by `target.delta`, turned by `target.rotation` about its centre; it stops at what it hits, then settles. `space: local` reads them in the first mover's parent space |
| `snap` | `ray` | the movers land where `ray` hits (`{origin, direction}` in world space, up to 1 km, or `{screen: [x, y]}`, a Scene view pixel, y down), then settle |
| `gather` | | pulled toward their common centre (`amount`, 1 = all the way), stopping on contact, then settle |
| `distribute` | `axis` | spaced evenly along `x`, `y` or `z` between the outermost two, keeping their order, then settle |

`settle: false` stops after the gesture instead of running until resting. `max_seconds`
(default 10) caps the simulated time. `feel` (`precise`, `balanced`, `fast`) trades accuracy for
speed when moving. `jitter: {position, rotation_deg}` offsets and turns every mover by a random
amount (from `seed`, default 1) before the first step.

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

**Slide a sofa against the wall** (a target just behind the wall): it stops at the wall.

```json
{"tool": "place_with_physics", "arguments": {"entities": ["Room/Sofa"], "mode": "move", "target": {"delta": [2, 0, 0]}, "apply": true}}
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
| `mode`, `seed`, `state_hash` | what ran; the same arguments give the same `state_hash` |
| `sim_seconds`, `steps`, `all_settled` | simulated time, fixed steps of 1/120 s, every body at rest |
| `entities[]` | `{id, name, path, before, after, moved_m, rotated_deg, settled, settle_step, resting_on, max_penetration_m, collider, warnings}`: world poses (`position`, `rotation` as `[x,y,z,w]`) and `after.local`, the parent-space pose that is committed |
| `context` | `{bodies, region}`: the static bodies used and the region they were gathered from |
| `refused` | movers that could not move, with the reason ("no mesh or collider", "too large (> 25 m)...") |
| `warnings`, `hint` | what to do next: an initial overlap deeper than 5 cm (run `separate` first), nothing to land on (raise `context_margin`), bodies that fell out of the world, never came to rest within `max_seconds`, or teleported, a mesh still loading |
| `ray` | snap: the world ray used |
| `screenshot` | with `screenshot`: what the image shows |

**Determinism.** Same editor binary, same platform, same scene and the same arguments give the
same `state_hash` and poses: every call builds a fresh world, adds the movers in path order,
steps a fixed 1/120 s, applies the seeded jitter before the first step and reads no clock. It
is not promised across platforms or builds. One exception: with `decompose: cached` (the
default) a concave mover's first run asks for its decomposition in the background and uses a
hull meanwhile (a warning says so), so a later run uses the decomposition and can differ. For a
preview that `apply` reproduces exactly, pass `decompose: always` (waits for the decomposition,
progress "Decomposing 2/5") or `never`.

**Long runs** are jobs: the call waits up to `timeout_seconds` (default 120), then returns a
`job_id` for `get_job` (progress "step 140/1200, 22 of 30 asleep"); `cancel_job` stops it and
nothing changes. A run fails, changing nothing, when the scene's GameObjects change while it
runs, the scene closes, Play starts or another project opens.

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
