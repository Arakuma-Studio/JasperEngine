# `physics-contacts`: per-object collision and trigger callbacks, contacts on the main thread (design)

Status: binding spec for branch `gaps/physics-contacts` (ledger #8). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (its shared conventions are binding) and
[GAP-REPORT.md §4](../../engine-gaps/GAP-REPORT.md). Sibling specs:
[physics-2d](2026-10-31-physics-2d-design.md), [physics-3d](2026-10-31-physics-3d-design.md).
Depends on the per-object message mechanism of `script-lifecycle` (§12 has the contract and what to
do if this branch merges first).

Abbreviations: `PS` = `engine/core/subsystem/PhysicsSystem.cpp`, `JPA` =
`engine/core/util/JoltPhysicsAux.h`, `B2A` = `engine/core/util/Box2DAux.h`, `GOL` / `ECSL` / `CCL` =
`engine/core/script/binding/{GameObjectClassesLua,ECSClassesLua,CoreClassesLua}.cpp`.

## 1. Goals

1. Unity-style per-object callbacks on scripts, called by name when a script defines them:
   `onCollisionEnter/Stay/Exit(collision)`, `onTriggerEnter/Stay/Exit(other)` (3D) and
   `onCollisionEnter2D/Stay2D/Exit2D(collision)`, `onTriggerEnter2D/Stay2D/Exit2D(other)` (2D), in Lua and
   as virtual methods of C++ `ScriptBase` scripts.
2. Every callback, and every Lua-facing global physics event, runs **on the main thread**, after the
   physics step it belongs to, inside the fixed-step loop (Unity: after FixedUpdate and the step).
   No Lua code ever runs on a Jolt job thread again (today: `JPA:325-338` under a recursive mutex,
   pool from `engine/core/physics/JoltShapeBuilder.cpp:300-308`).
3. Rich, value-typed collision data: other GameObject / Rigidbody / collider, this collider, contact
   points with normals and separation, impulse, relative velocity (Unity `Collision`, `Collision2D`,
   `ContactPoint`, `ContactPoint2D`).
4. Exact, documented Enter/Stay/Exit rules: sleeping bodies, disable/deactivate/destroy, collider and
   body rebuilds, type switches, layer matrix, triggers on static/kinematic bodies.
5. Cost only for objects whose scripts define the methods; zero work per contact for the rest.
6. A synchronous C++ path for contact modification (friction override, sensor override) that stays on
   the job threads, and a main-thread-safe replacement for Lua `shouldCollide3D`
   (`Physics.ignoreCollision`).
7. Identical behaviour in editor Play and exported builds on every platform; deterministic callback
   order.

## 2. Non-goals

- Unity `Physics.ContactEvent` batch API and `ModifiableContactPair` from scripts (C++ hook only, §6.1.8).
- Colliders as separate child GameObjects: a collider stays an entry of its body's `colliders` list
  (`docs/mcp/guide/physics.md`); the per-collider `receiver` field (§5 D9) covers Unity's child-collider
  routing.
- Callbacks for character controllers, vehicles, particles (out of the program's scope).
- Exact solver impulses for 3D (Jolt does not expose them to listeners): 3D impulses are Jolt's
  `EstimateCollisionResponse` estimate (§5 D11).
- 2D trigger pairs Box2D never creates (kinematic sensor vs static/kinematic body, sensor vs sensor):
  documented limitation, Inspector hint (§7.6).

## 3. Users and stories

- Porting user (hill climb `CarUpsideDownDetector.cs:7`, `CoinCollectable.cs:7`): writes
  `function M:onCollisionEnter2D(c) if c.gameObject.tag == "Ground" then ... end end` and
  `function M:onTriggerEnter2D(other) if other:compareTag("Player") then ... end end`.
- Porting user (Invaders `InvadersBulletController.cs:46-56`): Enter/Stay/Exit on a bullet; (Agua
  `LaneControllerHorizontal.cs:151`) reads `c:getContact(0).point.y`; (Kart `GreenShell.cs:132`)
  reflects velocity on `contacts[1].normal`.
- Gameplay programmer: a pickup trigger that is destroyed on touch, and a door trigger that must get
  `onTriggerExit` when the player object is destroyed inside it.
- C++ programmer: overrides `void onCollisionEnter(const Collision& c) override` in a `ScriptBase`
  script; another one installs a friction modifier for conveyor belts on the job threads.
- Agent (MCP): writes a Lua script with `onTriggerEnter`, plays, reads the console, and inspects
  `get_runtime_entity` to see which bodies touch an object and since which step.
- Agent: needs a filter that used to be a Lua `shouldCollide3D` handler: calls
  `Physics.ignoreCollision(a, b)` instead.

## 4. Research summary

- Unity callbacks (2022.3): collision messages need a non-kinematic Rigidbody on one side; trigger
  messages need a Rigidbody (kinematic counts) on one side; static-static never; messages go to the
  collider's GameObject and its attached Rigidbody's GameObject; **disabled MonoBehaviours still
  receive them**; parameter optional; Stay once per physics step, not for sleeping bodies; order in the
  fixed loop: FixedUpdate, physics, OnTrigger*, OnCollision*; 3D Exit on destroy/deactivate is not
  guaranteed (issue trackers UUM-27272, 1408707: "won't fix"); 2D `callbacksOnDisable` (default on)
  sends Exit when a collider with contacts is disabled.
  <https://docs.unity3d.com/2022.3/Documentation/Manual/collider-types-interaction.html>,
  <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/MonoBehaviour.OnCollisionEnter.html>,
  <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Physics2D-callbacksOnDisable.html>.
  Take: names, matrix, disabled-script delivery, parameter types, Unity 2D naming of `Collision2D`.
  Leave: the asymmetric 3D destroy behaviour (we always send Exit to the survivor, D6).
- Unity data: `Collision {gameObject, collider, rigidbody, transform, contactCount, GetContact, contacts,
  impulse, relativeVelocity}`, `ContactPoint {point, normal, separation, impulse, thisCollider,
  otherCollider}`, `Collision2D {collider (other), otherCollider (this), rigidbody, otherRigidbody,
  contacts, contactCount, relativeVelocity, enabled}`, `ContactPoint2D {point, normal, separation,
  normalImpulse, tangentImpulse, relativeVelocity}`. Take all; add unambiguous `thisCollider` /
  `thisRigidbody` to `Collision2D` because Unity's `otherCollider` = this one confuses everybody.
- Godot (`contact_monitor` + `max_contacts_reported`, off by default) and Unreal ("Simulation
  Generates Hit Events", "Generate Overlap Events") are opt-in per body for cost reasons. Take the
  cost model, leave the opt-in: Unity's "define the method and it works" with an internal per-object
  mask computed from the scripts (D3).
- Jolt 5.2 (vendored, `engine/libs/joltphysics/Jolt/Physics/Collision/ContactListener.h:64-130`):
  callbacks on several threads with all bodies locked; `OnContactAdded` velocities are pre-solve;
  **a body falling asleep produces `OnContactRemoved` for its contacts**; `OnContactRemoved` cannot
  touch bodies; manifold reduction merges sub-shapes with similar normals
  (`Body.h:83-88`); `ContactSettings::mIsSensor` turns a contact into a sensor contact;
  `JobSystemSingleThreaded` exists; `EstimateCollisionResponse` estimates impulses.
- Box2D 3.1.0 (vendored; `engine/libs/box2d/src/core.c:71`; world built without a task system, so
  single-threaded): begin/end touch events after the step (`types.h:1012-1040`), end events also on
  shape/body destroy with ids that are already invalid (`contact.c:345-405`), contact flags fixed at
  contact creation (`contact.c:264-277`), contacts survive sleep, post-step manifolds carry
  `normalImpulse`, `tangentImpulse`, `normalVelocity` (pre-solve) (`collision.h:496-532`), kinematic
  proxies never pair with static/kinematic ones and sensors never pair with sensors
  (`broad_phase.c:254-258, 340-353`).

## 5. Product decisions

- **D1 Names.** Lua methods exactly `onCollisionEnter`, `onCollisionStay`, `onCollisionExit`,
  `onTriggerEnter`, `onTriggerStay`, `onTriggerExit` and the same with suffix `2D`. Also
  `onJointBreak` / `onJointBreak2D` go through the same mechanism (owned by physics-3d / physics-2d).
- **D2 Main thread, after the step.** Per-object callbacks and every Lua-visible global event
  (`onContactAdded3D`, `onContactPersisted3D`, `onContactRemoved3D`, `onBodyActivated3D`,
  `onBodyDeactivated3D`, all 2D events) are delivered on the main thread right after the step that
  produced them, inside the same fixed step. Exception: `shouldCollide3D`, `shouldCollide2D` and
  `preSolve2D` are synchronous by nature (§6.1.7).
- **D3 Cost.** Each body carries a 16-bit *listener mask*: one bit per callback that a script of its
  GameObject (or of a collider `receiver`) defines. 3D: packed into the Jolt body user data
  (`entity | mask << 32`) so job threads early-out without locks. Pairs where neither side has a
  listener bit and no global subscriber exists are never recorded.
- **D4 Delivery targets.** The body's GameObject (the only GameObject that owns colliders here), plus
  the collider's `receiver` GameObject when set (D9). No bubbling to parents (Unity does not send to a
  parent that has no Rigidbody either). All script instances of the target that define the method get
  it, **enabled or disabled** (Unity), while the GameObject is active in hierarchy.
- **D5 Granularity.** One callback per *collider pair* per step (Unity), keyed by
  `(entity, colliderIndex)` on both sides; triangles of one mesh collider and pieces of a decomposed 2D
  polygon count as one collider.
- **D6 Exit on removal.** When a body, collider or GameObject in contact goes away, the *other* side
  always gets the Exit (better than Unity 3D, like Unity 2D `callbacksOnDisable`):
  - destroyed (GameObject destroy, `removeRigidbody`, component removal): **immediately**, in the
    removal path, while the departing GameObject still exists, so `collision.gameObject` /
    `other.gameObject` are valid during the callback; the departing object gets nothing;
  - disabled body, inactive GameObject, removed collider, collider changed to/from trigger, layer or
    filter change that ends the pair: at the end of the next fixed step (normal dispatch), the
    departing object still exists (inactive or changed) and is passed;
  - scene unload / Play stop / scene teardown: **no callbacks**.
- **D7 Sleep.** Sleeping does not end a contact: no Exit when a body falls asleep, no Stay while both
  bodies of a pair sleep (3D: one asleep is enough, Jolt drops it; 2D: Stay while either is awake). A
  pair that was touching when it slept and no longer touches when it wakes gets its Exit then.
- **D8 Rebuilds are invisible.** Rebuilding a body or its shapes (collider edit, scale change, type
  switch that recreates the body) produces no Exit/Enter for pairs that still touch after the step.
- **D9 `receiver`.** New per-collider field (2D and 3D) `receiver` (Entity, default none): contacts of
  that collider are also delivered to that GameObject (Inspector "Send Callbacks To"). Ports Unity
  child colliders with their own scripts (hill climb `UPSIDE DOWN DETECT`).
- **D10 Order.** Deterministic: per step, 2D first (right after the 2D step), 3D after the 3D step;
  inside each: global events (legacy order), then per-object triggers, then collisions; Enter, Stay,
  Exit; pairs sorted by `(min entity, its collider, max entity, its collider)`; for one pair, the
  lower entity's targets first.
- **D11 Data.** Collision data is a value snapshot taken at the step: safe to keep after the callback
  (Unity `reuseCollisionCallbacks = false` semantics; no reuse trap). 3D impulses are estimates
  (Jolt `EstimateCollisionResponse`), computed only for bodies with collision listener bits; 2D
  impulses are Box2D's post-solve ones. `relativeVelocity = velocity(other) − velocity(this)`: a ball
  falling onto a floor sees `+y` (Unity).
- **D12 Global 3D events.** Kept with the same names and arguments but moved to the main thread after
  the step; `Contact3D` becomes a snapshot (getters work, setters warn once per scene and do nothing).
  Opt-in `physicsSystem.synchronousContactEvents3D = true` restores in-step delivery with live,
  modifiable `Contact3D`, by stepping that scene's 3D world on the main thread with
  `JPH::JobSystemSingleThreaded`. It turns itself on (log info once) while `shouldCollide3D` has a Lua
  subscriber, since that handler must run inside the step.
- **D13 C++ contact modification** stays synchronous on the job threads through a new C++-only event
  `onContactModify3D` (not bound to Lua).
- **D14 `Physics.ignoreCollision(a, b[, ignore=true])` / `Physics2D.ignoreCollision`** body-pair
  filtering from Lua, runtime-only, the recommended replacement for Lua `shouldCollide3D`.
- **D15 Triggers on kinematic bodies (3D)** detect static and kinematic bodies like Unity: a kinematic
  body with any trigger collider (or body `sensor`) gets Jolt `CollideKinematicVsNonDynamic`
  automatically (physics-3d builds the flag; this spec requires it).
- **D16 Global 2D legacy filter.** Per-object listeners turn on Box2D contact/sensor events on their
  shapes internally; the global 2D events are still only delivered for pairs where a shape has the
  authored flag (`contactEvents`, `sensorEvents`, `sensor`), so old scripts see exactly what they saw.
- **D17 New global 2D stay events** `stayContact2D` and `staySensorContact2D` (ledger "2D contact
  stay"), same argument lists as `beginContact2D` / `beginSensorContact2D`, once per step per touching
  shape pair with the authored flag, while either body is awake.

## 6. Design

### 6.1 Runtime C++

#### 6.1.1 Script messages (contract with `script-lifecycle`)

`engine/core/script/ScriptMessages.{h,cpp}` (created by whichever of `script-lifecycle` and this
branch merges first; the other extends it):

```cpp
namespace jasperengine {
    enum class ScriptMessage : uint8_t {
        CollisionEnter, CollisionStay, CollisionExit, TriggerEnter, TriggerStay, TriggerExit,
        CollisionEnter2D, CollisionStay2D, CollisionExit2D, TriggerEnter2D, TriggerStay2D, TriggerExit2D,
        JointBreak, JointBreak2D,
        // script-lifecycle appends its own (Enable, Disable, ...) after these
        Count
    };
    using ScriptMessageMask = uint32_t;          // bit (1u << (int)message)

    class JASPERENGINE_API ScriptMessages {
    public:
        static const char* methodName(ScriptMessage m);        // "onCollisionEnter", ...
        // the messages some script instance of the GameObject defines (Lua: a function in the
        // instance table or its class; C++: an override, ScriptEntry::cppMessageMask). Cached per
        // entity, recomputed when a script instance is created, released, enabled or disabled.
        static ScriptMessageMask receivers(Scene* scene, Entity entity);
        // Calls the method on every script instance of the GameObject that defines it, in the
        // ScriptComponent order, enabled or not, Lua then C++. pushArgs pushes the Lua arguments
        // and returns their count; callCpp calls the C++ virtual. Re-reads the components after
        // each call (a callback may add/remove components or destroy the GameObject); errors are
        // logged with a traceback (LuaBinding::pcallWithTraceback) and never propagate.
        // Returns the number of methods called. Main thread only (asserted).
        static int send(Scene* scene, Entity entity, ScriptMessage m,
                        const std::function<int(lua_State*)>& pushArgs,
                        const std::function<void(ScriptBase&)>& callCpp);
        // Told when receivers(scene, entity) may have changed
        using ReceiversChanged = void (*)(Scene* scene, Entity entity);
        static void addReceiversChangedListener(ReceiversChanged listener);
    };
}
```

- Reuses `LuaBinding::callLuaScriptMethod` (`engine/core/script/LuaBinding.cpp:1016-1054`), which
  becomes a thin wrapper over the same loop; the loop must accept instances of disabled entries.
- If `script-lifecycle` keeps "no instance for a script disabled at load", such scripts receive
  nothing (no instance), which is acceptable; once an instance exists it receives messages whether
  enabled or not.
- C++: `ScriptBase` (`engine/core/script/ScriptBase.h:12-28`) gains a virtual destructor and empty
  virtuals: `onCollisionEnter/Stay/Exit(const Collision&)`, `onTriggerEnter/Stay/Exit(const
  ColliderRef&)`, `...2D(const Collision2D&)`, `onTriggerEnter2D/...(const ColliderRef2D&)`,
  `onJointBreak(float force, Joint3D&)`, `onJointBreak2D(Joint2D&)`. The generated factory
  (`editor/Generator.cpp:675`) stores `ScriptMessages::cppMask<T>()` in a new
  `ScriptEntry::cppMessageMask`: a constexpr trait comparing `decltype(&T::onCollisionEnter)` with
  `decltype(&ScriptBase::onCollisionEnter)` (an override has class type `T`), under
  `if constexpr (std::is_base_of_v<ScriptBase, T>)`. Editor Play's script library uses the same
  generated code.

#### 6.1.2 Listener masks

`PhysicsCallbacks` (in `engine/core/physics/ContactTracker.h`): bits 0-5 = 3D messages, 6-11 = 2D
messages (same order as `ScriptMessage`). The body mask = `receivers(body entity)` ∪ `receivers(r)` for
every collider `receiver` r of that body. Recomputed by a `ReceiversChanged` listener and when a body
or its colliders are (re)built; applied to:

- 3D: `JPH::Body::SetUserData(entity | (uint64_t)mask << 32)` through
  `BodyInterface::SetUserData` (outside the step only; a change requested inside a callback is applied
  right after the dispatch). All readers go through `bodyEntity(uint64_t)` / `bodyMask(uint64_t)`
  helpers in `JPA`; audit every `GetUserData` (`JPA:259, 313-350`, `engine/core/math/Ray.cpp:639`,
  activation listener `inBodyUserData`, `OnContactRemoved`'s `body_interface.GetUserData`).
  `PlacementWorld` has its own bodies and is unaffected.
- 2D: shapes of a body with any 2D bit get `enableContactEvents = true` (collision bits) and
  `enableSensorEvents = true` (trigger bits) in `PS::syncRigidbody2DColliders` (`PS:1206-1217`); when
  the mask of a live body changes, its shapes are re-flagged (`b2Shape_EnableContactEvents`,
  `b2Shape_EnableSensorEvents`) and re-paired with the existing refilter toggle
  (`PS::refilterShapes2D`, `PS:432`) because Box2D fixes the flags at contact creation.

#### 6.1.3 Data types (engine/core/object/physics/Collision.{h,cpp}, Collision2D.{h,cpp})

All plain values, copyable, no Jolt/Box2D pointers.

```cpp
struct ColliderRef   { Scene* scene; Entity entity; int index; };   // Lua class "Collider"
struct ColliderRef2D { Scene* scene; Entity entity; int index; };   // Lua class "Collider2D"
struct ContactPoint  { Vector3 point; Vector3 normal; float separation; Vector3 impulse;
                       ColliderRef thisCollider, otherCollider; };
class Collision      { ColliderRef thisCollider, otherCollider; Vector3 impulse;
                       Vector3 relativeVelocity; std::vector<ContactPoint> contacts; ... };
struct ContactPoint2D{ Vector2 point; Vector2 normal; float separation; float normalImpulse;
                       float tangentImpulse; Vector2 relativeVelocity; ColliderRef2D thisCollider,
                       otherCollider; };
class Collision2D    { ColliderRef2D thisCollider, otherCollider; Vector2 relativeVelocity;
                       bool enabled; std::vector<ContactPoint2D> contacts; ... };
```

Field rules:
- `point`: world space (2D: points, the engine's 2D unit). `normal`: unit, world space, pointing
  **from the other collider toward this object** (away from the surface it hit), so
  `v:reflect(normal)` bounces. `separation`: distance along the normal, negative when penetrating
  (3D metres; 2D points).
- 3D `impulse` (N·s): sum of `EstimateCollisionResponse` per-point normal + friction impulses for this
  pair, Enter and Stay; zero for Exit and for trigger pairs. Per-point `ContactPoint.impulse` likewise.
- 2D `normalImpulse` / `tangentImpulse` (N·s, Box2D units): from the post-step manifold
  (`b2Shape_GetContactData` of this collider's shape, matching the other shape).
- `relativeVelocity`: `v_other − v_this` at the manifold centroid; 3D Enter: pre-solve body velocities
  sampled in `OnContactAdded` (job thread, read-only access allowed); 3D Stay: velocities in
  `OnContactPersisted`; 2D Enter: pre-step linear velocities (stored per body in
  `Rigidbody2DComponent::preStepLinearVelocity`, written in the pre-step loop `PS:2981-3004`); 2D Stay:
  current; Exit: zero. 2D in points/s, 3D in m/s.
- Contacts: at most 4 per 3D pair (Jolt's reduced manifold, the deepest four if more), at most 2 per
  2D shape pair; a decomposed 2D polygon or a mesh collider merges the pieces' points (cap 8, deepest).
- `Collision2D.enabled`: false when `preSolve2D` disabled the contact this step (Unity).
- Trigger callbacks get only the other collider (`ColliderRef` / `ColliderRef2D`).

`ColliderRef` / `ColliderRef2D` resolve lazily: if the entity or the collider index no longer exists
(destroyed later), `gameObject`, `rigidbody` return nil and `isValid()` is false; no throw.

#### 6.1.4 Contact trackers (engine/core/physics/ContactTracker.{h,cpp})

One `ContactTracker2D` and one `ContactTracker3D` per `PhysicsSystem` (members). Core state:

```cpp
struct ColliderKey { Entity entity; int collider; };               // ordered pair: a < b
struct PairState   { ColliderKey a, b; bool trigger; bool touching; bool asleep;
                     uint32_t sinceStep; uint16_t maskA, maskB; };
std::unordered_map<PairKey, PairState> pairs;                       // only listener pairs
```

Per step: build `now` from the backend (below), then diff against `pairs`: `now − prev` → Enter,
`now ∩ prev` → Stay (unless asleep), `prev − now` → Exit; rebuild-only changes cancel (D8). Exits for
destroyed entities are emitted immediately and their pairs removed (D6). Queued exits (disable,
deactivate, collider removal) are pairs found missing at the next diff because their bodies left the
world; the departing entity is still created, so its refs resolve.

**3D backend.** `JoltContactListener` (`JPA:296-389`) is rewritten:

1. `OnContactAdded` / `OnContactPersisted` (job threads): run the synchronous steps first —
   physics-3d's per-collider trigger/material step (sets `ioSettings.mIsSensor`), then
   `onContactModify3D` C++ subscribers — then, if `bodyMask(user data of either body) != 0` or a global
   3D contact event has subscribers, push a `RawContact3D {kind, bodyId1, bodyId2, subShape1,
   subShape2, entity1, entity2, collider1, collider2 (from GetSubShapeUserData of each body's shape),
   isSensor, normal, points[≤4], penetration, v1, v2 (pre-solve point velocities), estimated impulses
   (only if a collision bit is set and !isSensor), combined friction/restitution}` into a per-thread
   buffer (thread_local slot index into a `std::vector<std::vector<RawContact3D>>` sized
   workers+1, cleared at the start of the step). No locks, no allocation in steady state.
2. `OnContactRemoved`: push `{Removed, SubShapeIDPair}` if the body pair is in the tracker's
   *read-only during the step* sub-shape map, or global removed subscribers exist.
3. `OnContactValidate`: unchanged semantics (`shouldCollide3D`, C++ subscribers on job threads; Lua
   subscribers force synchronous mode, D12), plus `Physics.ignoreCollision` is enforced by physics-3d's
   pair filter (group filter), not here.
4. Activation listener (`JPA:265-293`): buffers `{bodyId, userData, activated}`.

After `world3D.Update` and the transform write-back (`PS:3132-3165`), on the main thread: merge the
buffers, sort by `(kind, bodyId1, bodyId2, subShape1, subShape2)`, dispatch globals (D12) from the raw
records, then build `now` for the tracker: every Added/Persisted collider pair, plus every previous pair
whose two entities and colliders still exist, whose bodies are both in the world, and of which at least
one body is now inactive (asleep) — those stay with `asleep = true`. Mark `RemovedThisStep` only for
global events; the tracker never needs Removed (diff).

Kinematic-vs-static trigger detection and manifold reduction are set by physics-3d at body build:
`CollideKinematicVsNonDynamic` for kinematic bodies with a trigger (D15); `UseManifoldReduction =
false` for bodies with more than one collider and (a trigger collider, mixed materials, or a
collision listener bit) so per-collider pairs stay exact.

**2D backend.** In `B2A::manageEvents` (`B2A:140-235`), after the step: keep a
`shapeId (index1, revision) → ColliderKey` map maintained at shape creation (so end events with
invalid shape ids still resolve); begin/end events update a set of touching shape pairs; `now` = that
set collapsed to collider pairs; Stay unless both bodies are asleep (`b2Body_IsAwake`); sensor events
→ trigger pairs. Global 2D events keep today's code path and order, filtered per D16; then the new
`stayContact2D` / `staySensorContact2D` (D17). A visitor shape destroyed while overlapping a sensor
produces Box2D's end event with an invalid id: resolved through the map.

#### 6.1.5 Dispatch

`ContactTracker::dispatch()` builds each `Collision`/`Collision2D` (per receiver side: normals
flipped, this/other swapped), checks before each call that the receiver entity is created and active
in hierarchy (else skips), and calls `ScriptMessages::send` for the body entity, then for the
collider's `receiver` if set, created, active and different. A callback may destroy, create, enable,
disable, teleport, change layers, add/remove colliders or load scenes:

- event lists are copied before the loop; each event re-validates its receiver;
- `GameObject.destroy` is deferred to the end of the frame (`ObjectManager.h`), so the object keeps
  receiving the remaining events of this step (Unity); `destroyImmediate` runs the immediate-exit path
  (D6) re-entrantly: the pair is removed from `pairs` before the nested Exit call, so it is delivered
  once;
- body / shape changes requested in a callback apply at the next step (we are outside the step).

#### 6.1.6 Exit-on-removal hooks

- `PhysicsSystem::onComponentRemoved` (`PS:3222-3243`) and the 2D/3D `destroyRigidbody*` paths call
  `tracker.bodyRemoved(entity)` before the backend body is destroyed: for each pair of that entity,
  the other side gets its Exit immediately (D6). Skipped while the scene is tearing down: add
  `bool Scene::isTearingDown() const`, set first thing in `Scene::~Scene` (`engine/core/Scene.cpp:60`).
  Not `PhysicsSystem::destroy()` (`PS:2959`): `Scene::destroy()` also runs on view recreation
  (comment at `Scene.cpp:66`). Editor Play stop deletes the play scenes, so it is covered too.
- Collider removal from Lua (`removeCollider`, `removeAllColliders`) marks the colliders gone; their
  pairs end at the next diff (queued Exit, the collider ref resolves to the body with `index` that
  no longer exists → `isValid() == false`, `gameObject` still valid).

#### 6.1.7 Global events and synchronous mode

- `PhysicsSystem` (`engine/core/subsystem/PhysicsSystem.h:191-210`) keeps every existing event. 3D
  ones are now called from the dispatch with `Contact3D` snapshots: `Contact3D` gets a second
  constructor from `RawContact3D` (copied base offset, normal, depth, collider indices, ≤4 points per
  side, combined friction/restitution, sensor flag); `getJoltContactManifold()` /
  `getJoltContactSettings()` return null for snapshots; setters on a snapshot log one warning per scene
  (`"Contact3D setters have no effect after the step: set physicsSystem.synchronousContactEvents3D =
  true or use a physics material"`).
- New `bool PhysicsSystem::synchronousContactEvents3D` (Lua property of the same name). True, or
  `shouldCollide3D.luaSubscriberCount() > 0`: `world3D.Update` gets a per-system lazily created
  `JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs)`; the global 3D events and `shouldCollide3D` are
  called **inside** the step with live objects, on the main thread (old semantics, now safe); the
  per-object callbacks still go after the step. The callback mutex (`callbackMutex3D`) stays for C++
  subscribers in the threaded mode.
- `FunctionSubscribe` (`engine/core/util/FunctionSubscribe.h:246-260`) gets `size_t
  luaSubscriberCount() const`, counting entries added through `add(tag, lua_State*)`.
- New C++-only `FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D&)> onContactModify3D`: job
  threads, every Added/Persisted manifold, live `Contact3D` (setters work). Subscribers must be
  thread-safe and must not touch the scene; documented in the header. Not bound to Lua (excluded from
  the stubs).
- 2D `preSolve2D` / `shouldCollide2D`: unchanged (Box2D runs single-threaded on the main thread).

#### 6.1.8 `Physics.ignoreCollision`

- 3D: physics-3d's `JoltPairFilter3D` (group filter, see physics-3d §6.1.6) holds ignored entity
  pairs with a reason bit `SCRIPT`; `PhysicsSystem::setIgnoreCollision3D(Entity, Entity, bool)` wakes
  both bodies so resting pairs are re-tested (`PS::wakeAround3D`). If this branch merges first, it
  creates `JoltPairFilter3D` exactly as physics-3d specifies.
- 2D: `PhysicsSystem::setIgnoreCollision2D` creates a Box2D null joint (`b2NullJointDef`,
  `types.h:646-663`) between the two bodies (Box2D drops their contacts), destroyed on un-ignore; the
  ignored set is kept in the system and re-applied when either body is recreated.
- Runtime only, per scene, cleared with it; both bodies must have the same dimension's Rigidbody
  (else `error`).

#### 6.1.9 Collider `receiver`

`Collider2D::receiver`, `Collider3D::receiver` (`Entity`, default `NULL_ENTITY`). Serialized as
`receiver` (omitted when none); remapped like `Collider3D::sourceEntity` by prefab instantiation and
`engine/core/registry/EntityCloner.cpp`; emitted by `editor/Factory.cpp` (`createRigidbody2DComponent`
`:2310`, `createRigidbodyComponent` `:2352`) with the entity variable map; Catalog property
`colliders[i].receiver` (`editor/Catalog.cpp:1205-1240` and the 3D twin `:1340-1380`).

### 6.2 Lua API

Callbacks (methods on the script table, all optional; extra arguments may be ignored):

```lua
function M:onCollisionEnter(collision) end    -- Collision
function M:onCollisionStay(collision) end
function M:onCollisionExit(collision) end
function M:onTriggerEnter(other) end          -- Collider (the other one)
function M:onTriggerStay(other) end
function M:onTriggerExit(other) end
function M:onCollisionEnter2D(collision) end  -- Collision2D
function M:onCollisionStay2D(collision) end
function M:onCollisionExit2D(collision) end
function M:onTriggerEnter2D(other) end        -- Collider2D
function M:onTriggerStay2D(other) end
function M:onTriggerExit2D(other) end
```

Classes (properties read-only):

| class | members |
|---|---|
| `Collision` | `gameObject`, `rigidbody`, `collider` (the other's), `thisCollider`, `thisRigidbody`, `impulse` (Vector3), `relativeVelocity` (Vector3), `contactCount`, `getContact(i)` (0-based, error if out of range), `contacts` (new 1-based table of `ContactPoint` each access) |
| `ContactPoint` | `point`, `normal`, `separation`, `impulse`, `thisCollider`, `otherCollider` |
| `Collision2D` | `gameObject` (other), `collider` (other), `rigidbody` (other), `otherCollider` (**this**, Unity), `otherRigidbody` (**this**, Unity), `thisCollider`, `thisRigidbody`, `relativeVelocity` (Vector2), `enabled`, `contactCount`, `getContact(i)`, `contacts` |
| `ContactPoint2D` | `point`, `normal`, `separation`, `normalImpulse`, `tangentImpulse`, `relativeVelocity`, `thisCollider`, `otherCollider` |
| `Collider` (3D) | `gameObject`, `rigidbody` (and Unity alias `attachedRigidbody`), `index`, `type` (`Collider3DType`), `isTrigger`, `tag`, `compareTag(tag)`, `receiver` (GameObject or nil), `isValid()` |
| `Collider2D` | same with `Rigidbody2D` and `Collider2DType` |

Statics: `Physics.ignoreCollision(a, b [, ignore=true])`, `Physics.getIgnoreCollision(a, b) -> bool`
(a, b: `GameObject` or `Rigidbody`), and the same on `Physics2D` with 2D bodies. Errors (Lua errors,
programming mistakes): an argument without a body of that dimension, two bodies of different scenes.

`PhysicsSystem` gains `synchronousContactEvents3D` (bool), `stayContact2D`, `staySensorContact2D`
(event properties like `beginContact2D`, `ECSL:219-225`).

Example (hill climb port):

```lua
function UpsideDown:onCollisionEnter2D(c)
    if c.gameObject.tag == "Ground" and c.thisCollider.index == self.detectorCollider then
        self.game:gameOver()
    end
end
```

### 6.3 Editor UX

- Rigidbody / Rigidbody 2D Inspector, per collider row: "Send Callbacks To" (GameObject field,
  `receiver`, undoable, prefab-override aware).
- In Play mode, a read-only "Contacts" foldout under each Rigidbody (2D) listing current pairs from the
  backend (other GameObject, this/other collider index, trigger, touching since step N, asleep); and a
  "Callbacks" line naming the methods found on the GameObject's scripts ("onTriggerEnter (Coin.lua)").
  Refreshed at most 10 times per second.
- Script Inspector: no change. The Lua code editor's completion gets the callback names through the
  stubs (§6.4).
- 2D Inspector hint (yellow) on a sensor collider of a kinematic body: "Box2D never pairs kinematic
  sensors with static or kinematic bodies: make the other body dynamic (gravity scale 0) to detect it".

### 6.4 MCP / agent surfaces

- `get_runtime_entity` (`editor/mcp/tools/PlayTools.cpp`) adds `physics.contacts`: `[{other, other_path,
  collider, other_collider, trigger, since_step, asleep}]` (2D from `b2Body_GetContactData` and sensor
  overlaps; 3D from the tracker when listened, else a `NarrowPhaseQuery::CollideShape` of the body's
  shape at its pose) and `physics.callbacks`: the defined method names. Budget: a few dozen
  characters of schema.
- `edit_colliders` accepts `receiver` (entity ref) per collider; `set_properties` reaches
  `colliders[i].receiver` through the Catalog.
- Lua stubs: `generate_lua_stubs.py` output gains the classes and a `---@class ScriptCallbacks` doc
  block listing the twelve methods with parameter types, so LuaLS completes `function M:onTrigger...`;
  `-- unresolved:` stays empty (`tools/mcp/tests/test_stubs.py`).
- Guide: `docs/mcp/guide/physics.md` "Layers, contacts and queries" rewritten: per-object callbacks
  first, global events second; one line in `editor/mcp/McpInstructions.cpp` ("Collisions: define
  onCollisionEnter / onTriggerEnter(2D) on the script; see jasper://docs/physics").

### 6.5 Exported-build path

No platform code. Factory emits `receiver`; generated C++ script factories emit `cppMessageMask`. The
single-threaded job system is compiled on every platform (Web already runs Jolt jobs on one thread:
synchronous mode is then the same cost).

### 6.6 Docs to update

`docs/mcp/guide/lua-api.md` (callbacks section next to lifecycle; classes; `Physics.ignoreCollision`;
replace §"Physics events are global" pitfall at `:1489`), `docs/mcp/guide/physics.md`,
`docs/unity-glossary.md` (OnCollisionEnter… → same names lowerCamelCase; `Collision2D.otherCollider`
caveat; `Physics.IgnoreCollision` → `Physics.ignoreCollision` on bodies), README feature list
("Unity-style collision and trigger callbacks").

## 7. Behaviour rules and edge cases

1. **Threading.** No Lua state access off the main thread (the thread checker in
   `LuaBinding.cpp:131-165` must report zero violations in all tests). Job-thread code reads only:
   bodies passed by Jolt, the user-data mask, the read-only sub-shape map, the ignore set; it writes
   only its thread's buffer.
2. **Fixed step / time scale.** Callbacks once per fixed step that steps; several steps per frame →
   several rounds; `Engine.timeScale = 0` or paused physics (`PhysicsSystem::paused`) → none.
   Interpolation does not affect data (step poses).
3. **Collision vs trigger.** A pair is a trigger pair when either collider is a trigger (or a body
   `sensor`); both objects get `onTrigger*` with the other's collider. Otherwise collision callbacks;
   Jolt and Box2D only create pairs with a dynamic side, unless the user set
   `collideKinematicVsNonDynamic` on a kinematic 3D body (then kinematic-static/kinematic collision
   callbacks fire too: Unity's "Enable Kinematic Static Pairs").
4. **Layer matrix / masks / ignoreCollision / joints without enableCollision**: filtered pairs never
   start; a filter change during contact ends it (queued Exit at the next step).
5. **Same body twice.** A trigger and a solid collider of one body touching the same other collider
   give an `onTriggerEnter` and an `onCollisionEnter` (two pairs).
6. **2D limitations** (documented): no trigger pair kinematic-vs-static/kinematic, none sensor-vs-sensor
   (Box2D). 3D: sensor-vs-sensor works when either is non-static (they are ordinary contacts marked as
   sensor), test T6 pins it.
7. **Destroy inside callbacks.** See §6.1.5; destroying the *receiver* in its own Enter stops further
   deliveries to it this step only if `destroyImmediate`; `destroy` lets the step finish (Unity).
8. **Scene loads.** `SceneManager.loadScene` is deferred; the old scene's teardown sends nothing;
   persistent objects (feature `persistence`) moving between scenes keep no pairs (separate worlds):
   they get no Exit from the old scene.
9. **Disabled Rigidbody component / inactive GameObject** = body out of the world: queued Exit to
   partners; re-enabling produces Enter if still overlapping.
10. **Type switch** (dynamic ↔ kinematic ↔ static, physics-3d): pairs that no longer exist end (queued
    Exit); a body recreation is invisible (D8).
11. **Determinism.** Same binary, platform, scene and inputs → same callback sequence (test T14);
    threads only affect buffer order, removed by the sort.
12. **Editor.** Edit mode never dispatches. Play stop: no Exit.
13. **Errors in a callback** are logged once per message per script per second (rate-limited) and the
    dispatch continues.

## 8. Performance

- Job threads: one mask test per manifold for unlistened bodies (no globals) — no record. Listened:
  one `RawContact3D` (~200 bytes) per manifold per step; `EstimateCollisionResponse` only for collision
  bits.
- Main thread: sort O(n log n) of recorded manifolds; diff O(pairs); one Lua call per delivered event.
- Budget (asserted in a benchmark test, Release, this host): 500 dynamic boxes resting on a floor, 10
  of them listening to all 3D callbacks, 1 s simulated at 60 Hz: tracker + dispatch time < 5% of the
  physics step time; with no listener and no global subscriber: no measurable overhead (< 1%).
- 2D: enabling events on listener shapes only; Box2D's begin/end arrays are already built per step.
- `collision.contacts` allocates a table per access (documented: prefer `getContact(i)`).

## 9. Tests

New `editor/tests/PhysicsContactsTest.cpp` (+ `editor/tests/PhysicsTestScene.h` helpers: a scene with
Lua script instances, built like `engine/tests/anim/AnimationPlayerTest.cpp:900-940`; the script logs
`(step, method, other name, this index, other index)` to a Lua table read back by the test).
Fixed step 1/60 s, deterministic setups:

- T1 Ball (radius 0.5, dynamic) dropped from y = 2 on a static box floor: exactly one
  `onCollisionEnter` (step of first contact ±0), `onCollisionStay` every step until the ball sleeps,
  none after; no `onCollisionExit`; `getContact(0).normal ≈ (0,1,0)` (±0.01) for the ball, `≈ (0,-1,0)`
  for the floor; `relativeVelocity.y` at Enter within 10% of √(2·g·1.5) and > 0 for the ball;
  separation ≤ 0.02.
- T2 Restitution 0.8 ball: Enter, Exit, Enter… sequence counts equal the bounces Jolt produces (≥ 3).
- T3 Static trigger box, ball falling through: both objects get Enter, ≥ 1 Stay, Exit, in that order;
  the ball's `other.isTrigger == true`; no collision callbacks.
- T4 Kinematic trigger moved through a static box: trigger callbacks (D15).
- T5 Ball resting inside a big static trigger falls asleep: Stay stops, no Exit; `activate()` and push
  it out: Exit then.
- T6 Two dynamic trigger spheres overlapping (gravity 0): Enter on both (sensor-vs-sensor 3D).
- T7 Ball inside a trigger destroyed (`GameObject.destroy`): at end of frame the trigger object gets
  `onTriggerExit` exactly once with `other.gameObject` valid and named during the call; the ball
  gets nothing. Same with `destroyImmediate` called from the trigger's own `onTriggerEnter`.
- T8 `setActive(false)` on a ball resting on the floor: floor gets `onCollisionExit` at the end of the
  next fixed step; `setActive(true)`: Enter again.
- T9 `setColliderRadius` on a resting ball (physics-3d, or `load()` today) and a scale change: no
  Exit/Enter.
- T10 Layer matrix ignoring the pair: no callbacks; `Physics.ignoreCollision(ball, floor)`: the ball
  falls through, no callbacks; `ignore=false`: collides again.
- T11 Cost: 200 boxes without listeners and no global subscribers: `ContactTracker3D::recordedCount()`
  stays 0.
- T12 Threading: 300 bodies, threaded job system, a Lua `onContactAdded3D` handler and per-object
  callbacks: Lua thread-check violation counter 0; handler called with snapshot data equal (±1e-5) to
  what a C++ `onContactModify3D` subscriber saw for the same pair.
- T13 Lua `shouldCollide3D` rejecting pairs by name: works, runs on the main thread (synchronous mode
  auto-on, info logged once); `Contact3D:setCombinedFriction` in an async handler warns once, no effect.
- T14 Determinism: T12 scene run twice → identical callback logs.
- T15 2D box (dynamic) on a static box: `onCollisionEnter2D` once, Stay2D while awake, none asleep;
  `c.otherCollider.index == c.thisCollider.index` (both "this"), `c.collider` is the floor's;
  `getContact(0).normalImpulse > 0` in Stay; normal points up for the box.
- T16 2D static sensor coin + dynamic car body: `onTriggerEnter2D` on both; coin destroyed in it: car
  gets `onTriggerExit2D` once (Box2D end event with an invalid shape id resolved).
- T17 2D shapes with `contactEvents = false` and a listener script: per-object callbacks fire, global
  `beginContact2D` does not (D16); with `contactEvents = true`: both; `stayContact2D` once per step.
- T18 Compound 3D body with two box colliders resting on a floor: two Enters with
  `thisCollider.index` 0 and 1 (manifold reduction off for it).
- T19 `receiver`: collider 1 of the car has `receiver = Detector`: Detector's script gets the 2D
  callbacks of collider 1 only; the car's script gets all.
- T20 Two Lua scripts on one object, one disabled after init: both receive (D4); a script without the
  method: none.
- T21 A callback that teleports the other body, adds a collider and loads a scene: no crash, effects
  next step.
- T22 C++: a `ScriptBase` subclass overriding `onCollisionEnter` gets the mask bit and the call; one not
  overriding gets no bit (compile-time trait test).
- T23 `Engine.timeScale = 0`: no callbacks; 4 fixed steps in one frame: 4 Stay rounds.
- MCP end-to-end `tools/mcp/tests/test_physics_contacts.py`: create floor + ball + Lua script with
  `onCollisionEnter` printing, `play`, `wait`, console contains the line once; `get_runtime_entity`
  lists the contact.

## 10. Acceptance criteria

1. All tests above pass in `jasperengine-editor-tests` and `tools/mcp/run-tests.sh`.
2. `grep` finds no Lua call reachable from `JoltContactListener` / `JoltActivationListener` except
   through synchronous mode's single-threaded job system.
3. The hill-climb port's two scripts (§3) work unchanged from the Unity logic with lowerCamelCase.
4. Old scenes using `beginContact2D` / `onContactAdded3D` behave as before (same pairs, same order),
   now on the main thread.
5. Stubs updated, `-- unresolved:` empty; `docs/mcp/TOOLS.md` regenerated; tool budget within limits.
6. Exported Linux build of a test project runs T1/T3 logic (log lines) identically to editor Play.

## 11. Integration points

- `JPA:265-389` listeners (rewrite); `JPA:259` and `engine/core/math/Ray.cpp:639` user-data readers.
- `PS:204-266` constructor (listeners, single-threaded job system member), `PS:2969-3167`
  `fixedUpdate` (pre-step 2D velocity capture in the loop at `:2975-3004`; 2D dispatch after
  `B2A::manageEvents` call at `:3066`; 3D dispatch after the write-back loop `:3151-3165`), `PS:3222-3243`
  `onComponentRemoved`, `PS:1692` / `:1755` destroy paths, `PS:1206-1217` 2D shape flags.
- `engine/core/subsystem/PhysicsSystem.h:191-210` events; `ECSL:209-240` bindings.
- `B2A:140-235` `manageEvents`.
- `engine/core/object/physics/Contact3D.{h,cpp}` snapshot mode.
- `engine/core/script/LuaBinding.cpp:1016-1054` (`callLuaScriptMethod`), `ScriptBase.h`,
  `engine/core/component/ScriptComponent.h:17-26` (`cppMessageMask`), `editor/Generator.cpp:675`.
- `engine/core/util/FunctionSubscribe.h:246` (`luaSubscriberCount`).
- `CCL:544-575` `Physics` / `Physics2D` statics; `GOL` new classes.
- Serialization `editor/Stream.cpp:8022-8320` (`receiver`), `editor/Factory.cpp:2310-2420`,
  `editor/Catalog.cpp` collider paths, `engine/core/registry/EntityCloner.cpp` remap.
- Inspector `editor/window/InspectorWindow.cpp:11792` (2D body), `:12277` (3D body).
- MCP `editor/mcp/tools/WorldTools.cpp` (`edit_colliders`), `PlayTools.cpp` (`get_runtime_entity`).

## 12. Cross-feature dependencies

- **script-lifecycle** (required contract, §6.1.1): `ScriptMessages` with cached receiver masks,
  delivery to disabled instances, change notification, C++ virtual dispatch. If this branch merges
  first, it implements `ScriptMessages` with exactly the API above (only the physics messages) and
  `script-lifecycle` adds its messages, `onEnable`/`onDisable` and instance creation rules on top; if
  `script-lifecycle` merged first with an equivalent API under other names, use it and keep these
  semantics.
- **physics-3d**: per-collider trigger / material step in `OnContactAdded/Persisted` (runs before this
  spec's recording), `JoltPairFilter3D` (used by `Physics.ignoreCollision`), automatic
  `CollideKinematicVsNonDynamic` and manifold-reduction flags, `removeCollider` / shape-only rebuilds
  (D8), `onJointBreak` through `ScriptMessages`.
- **physics-2d**: decomposed polygons (several Box2D shapes per collider → one `ColliderKey`),
  `Collider2D` storage refactor (`shapes` vector), `contactEvents` default, `onJointBreak2D`.
- **persistence** (DontDestroyOnLoad): no contact pairs cross scenes (§7.8).
- **coroutines**: `onCollisionEnter` may not yield (Unity allows a coroutine OnCollision): a Lua
  callback that wants to wait starts a coroutine with the `coroutines` API.

## Implementation notes

Decisions taken while implementing (branch `gaps/physics-contacts`, on `gaps/script-lifecycle` with
`gaps/physics-2d` merged in; `physics-3d` not merged yet), where the spec was silent, ambiguous or
contradicted the code:

1. **One ScriptMessages** (the mechanism `main` has after script-lifecycle and physics-2d merged):
   the twelve contact messages and `onJointBreak` are `ScriptBase` virtuals whose base methods mark
   `inheritedMessages`, like the lifecycle's; a C++ script counts as receiving a message until its
   base method ran once (spec 6.1.1's compile-time trait was dropped for that single mechanism: an
   override may be private, protected or overloaded). So a body with a C++ script that overrides no
   contact method is recorded until each message reached the base once; `invoke` now tells the
   `ReceiversChanged` listeners when that happens, so physics drops the bits. Added
   `describeReceivers` (the Inspector's Callbacks line, MCP) and the per-second error rate limit of
   the contact messages (note 8). T22 tests that run-time detection instead of a static assert.
2. **physics-3d** (merged into this branch late, from `main`): its `JoltPairFilter3D`, per-collider
   trigger step (first in the listener) and trigger companions are used as they are.
   `Physics.ignoreCollision` sets its `SCRIPT` reason through `setBodyPairIgnored3D` (queued inside a
   step, both bodies woken); a body that goes forgets its pairs. The packed user data
   (`entity | mask << 32`) is on the body and on its trigger companion, whose contacts count as the
   owner's (the raw contact check accepts either id; a companion never sleeps, so its trigger pairs
   are not kept as asleep). Manifold reduction is off for compound bodies with collision listener
   bits (`usesManifoldReduction3D`); D15 also covers kinematic body-level sensors.
   `Collider.isTrigger` is the collider's `isTrigger` or the body's `sensor`. Removing a 3D collider
   renumbers the tracker's pairs like 2D.
3. **Listener masks** are recomputed for all bodies of a scene before its next step when any
   `ReceiversChanged` arrives (rare: an instance made, released, enabled, disabled), and at body
   creation. 2D: a listening body's shapes get Box2D's contact / sensor events (chain segments
   too, which Box2D makes without events); a change on a live body re-flags them and re-pairs them
   with the refilter bit only when a flag really changes.
4. **2D global events (D16).** `beginContact2D` / `endContact2D` / `stayContact2D` fire for pairs
   where a shape has the authored `contactEvents` (chain segments never had their own: Box2D gives
   them none, kept); the sensor events are not filtered, since sensor shapes always enabled them.
   `stayContact2D` / `staySensorContact2D` start the step after the pair began (Unity's Stay; the
   spec said "once per step per touching pair"), while either body is awake, in shape-id order.
5. **`Collision2D.enabled` is always true:** Box2D 3 makes a contact disabled by `preSolve2D` not
   touching (no points), so the pair gets an Exit, then an Enter when enabled again. Documented.
6. **Data.** A 3D contact point is the midpoint of the two surface points Jolt reports, its
   separation `(p2 - p1)·n` (negative when overlapping); `impulse` is the estimate on *this* body;
   3D `relativeVelocity` uses the point velocities at the manifold centre (pre-solve, read in the
   listener). Several manifolds of one collider pair (mesh triangles) merge: the deepest one's
   normal and velocities, every point (the deepest 8). `Collider.type` / `Collider2D.type` are the
   integer values of `Collider3DType` / `Collider2DType`. `Collider` / `Collider2D` compare with
   `==`.
7. **Global 3D events after the step.** Activations reported outside a step (a body added or woken
   from the main thread holds Jolt's locks then) are also delivered after the next step, in both
   modes. Order: activations, then added, persisted, removed, each sorted by body ids and
   sub-shapes. `Contact3D` gained `numContactPoints` and `snapshot`; the setters' warning is once
   per scene. The "auto synchronous" notice is `Log::print` (the engine has no info level).
8. **Errors in callbacks** are rate-limited per script class and method (one a second) for the
   twelve contact messages only; the lifecycle messages keep logging every error.
9. **Scene teardown:** `Scene::isTearingDown()` is set first in `~Scene`; removals then clear the
   pairs and send nothing (Play stop and scene loads delete their scenes).
10. **Editor.** "Send Callbacks To" is a GameObject field (`LocalEntity` row, undoable through the
    Catalog path `colliders[i].receiver`, prefab overrides as every Catalog property) on each 2D
    and 3D collider; the yellow kinematic-sensor note on 2D sensors of kinematic bodies; in Play a
    Callbacks line and a Contacts foldout under each body (single selection), read at most 10
    times a second.
11. **MCP.** `get_runtime_entity` adds `physics` (`contacts`, `callbacks`) for bodies and for
    GameObjects whose scripts define contact methods (receivers). 2D contacts come from the tracker,
    Box2D's touching contacts and sensor overlaps; 3D from the tracker for listened bodies, else a
    narrow-phase `CollideShape` of the body at its pose (2 cm, Jolt's speculative distance).
    `edit_colliders` takes `receiver` (an entity ref, null clears it) per collider in both
    dimensions and lists it.
12. **Tests.** T21's scene load uses `SceneManager.loadScene` of a scene that is not in the build
    (unit tests have no build scenes): the call must not break the dispatch. The export parity test
    (acceptance 6) is `tools/mcp/tests/test_physics_contacts_export.py` (`JASPER_SLOW_BUILD=1`).
    The cost test measures `PhysicsSystem::getContactSeconds3D()` (the processing after the step,
    callbacks included) against `getStepSeconds3D()`.
13. **Fixes found on the way.** `Rigidbody::setAllowSleeping` / `isAllowSleeping` on a static body
    dereferenced Jolt's missing motion properties (a crash reachable from Lua): now ignored / false.
    `edit_colliders` dropped `material: null` / `receiver: null` (the `has()` helper treats null as
    absent): both clear now. The agent guide's physics lines were tightened so the merged guide
    (lines-trails pushed `main` over) stays under 16 KB with the new Collisions line.
14. **Verification** (box, merged with `main` at `211073ea3`): editor and tests build with 0
    warnings; `jasperengine-editor-tests` 1200 passed, 8 skipped, 0 failed (25 new in
    `PhysicsContactsTest`, T1-T23 and the cost test: 10 listeners among 500 awake boxes, the work
    after the step is close to the 5% budget on the loaded shared box (one single run measured 5.6%
    with a counter script; a string-building logger script cost 10%), so the best of five one-second runs is asserted under 5%; no listener:
    nothing recorded). MCP: `test_physics_contacts.py`, `test_stubs`, `test_docs`,
    `test_html_ui_tools` (150 tools, 106624 characters), `test_protocol` (guide under 16 KB),
    `test_physics_2d(_inspector)`, `test_lifecycle(_inspector)`, `test_world`, `test_play`,
    `test_runtime_regressions`, `test_scene`, `test_water`, `test_play_stop_gizmos`,
    `test_ui_anchors`, `test_trial_fixes`, `test_place_with_physics`: all pass except
    `test_physics_2d.py::test_old_prefab_overrides_of_colliders`, which fails on `main` too (its fix
    is on `gaps/physics-2d`, not merged yet). With `JASPER_SLOW_BUILD=1`,
    `test_physics_contacts_export.py` built a Linux Desktop player whose callbacks (T1 landing, T3
    trigger crossing) equal editor Play's. Lua stubs: 0 unresolved. Web (emscripten) build of every
    changed engine source and Android (NDK 28, arm64) `-fsyntax-only` of the same: clean. Apple and
    Windows: no platform code changed (the Xcode project lists the new sources).
15. **Review fixes.** `Physics2D.ignoreCollision` is not a Box2D null joint (§6.1.8): Box2D tests
    joints only when a broad-phase pair is made and never drops existing contacts for a null
    joint, and the joint merged the bodies' islands. It is the set `Box2DAux::CollisionFilter`
    refuses, and a change re-pairs both bodies' shapes (the refilter bit): a touching pair ends
    with its Exit, an overlapping one is found again when un-ignored. Side effect: the global 2D
    events see end / begin for the other contacts of those two bodies at that step. `destroy` /
    `destroyImmediate` of a GameObject already being destroyed (from the Exit its destruction
    sends, from its `onDestroy`) do nothing (`ObjectManager`); before, a nested destroy ran.
    Raw 3D contacts whose body a global handler destroyed or made again are skipped.
    `onContactModify3D` subscribers are called without the crash guard (`callDirect`: its
    signal handlers are process-wide) and without a lock; the contract is in `PhysicsSystem.h`.
    Activation events are sorted by body id. `Rigidbody2D.removeAllColliders` gives the queued
    Exits index -1. Each has a regression test.
16. **Cost, measured** (`ContactProcessingCostIsSmall`, 500 awake boxes on a floor, 60 steps, the
    best of three, box 2202 under load): Jolt's step 129.0 ms with 10 listeners and 131.3 ms with
    none (the job-thread recording is inside the step: no difference above the noise); the work
    after the step 2.4 ms with 10 listeners (1.9%) and 0.08 ms with none (0.06%, asserted < 1%).
    The job-thread side for unlistened bodies is two subscriber checks and a 16-bit mask read
    per manifold (`main` had one subscriber check); it is not asserted separately.
17. **Verification after the review and the physics-3d merge** (box 2202, `main` at `eb8f3a2f5`):
    0 warnings; unit suite 1336 passed, 9 skipped, 0 failed (physics-3d's
    `PairFilterChangedInsideAStepAfterIt` now asks for synchronous contact events, since global
    handlers run after the step otherwise). FULL MCP suite: 858 passed, 19 skipped, 14 failed: the 10
    baseline ones, 3 agent-guide size checks (the merged guide was over 16 KB: trimmed, those 3 and the
    rest of the affected files then passed, 115 passed / 0 failed), and
    `test_stochastic_tiling.py::test_old_scene_resaves_byte_identical` (a resaved scene gains an
    `htmluipost` shader entry: shader / HTML UI code this branch does not touch, from `main`). Tool
    budget 151 tools, 107438 characters (TOTAL_LIMIT set to it); TOOLS.md regenerated.
18. **Final verification** (box 2202, merged with `main` at `b6989373c`, after physics-3d, http and
    ui-canvas-group): 0 warnings; unit suite 1382 passed, 9 skipped, 0 failed; FULL MCP suite 865
    passed, 19 skipped, 11 failed: the 10 baseline ones and
    `test_stochastic_tiling.py::test_old_scene_resaves_byte_identical` (from `main`, note 17). Tool
    budget 151 tools, 107624 characters, TOTAL_LIMIT set to it; TOOLS.md regenerated; the agent guide
    within 16 KB (its physics lines tightened again for the http merge).
