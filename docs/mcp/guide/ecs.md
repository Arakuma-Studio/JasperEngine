# Entities, components and systems

Served as `jasper://docs/ecs`. How a scene stores its GameObjects (entities) and components,
what the change tick means, the typed queries, deferred structural changes, the system
scheduler, and the tools, Lua functions and editor window that show them. The code is in
`engine/core/ecs` and `engine/core/registry`.

The four ECS tools (`get_ecs_stats`, `get_system_schedule`, `set_system_options`,
`query_entities`) are in the `diagnostics` tool group: `tool_groups {enable: ["diagnostics"]}`
lists them (they can be called by name without it).

## Storage and order

- A GameObject is an `Entity` (an unsigned id). User ids start at 1001 and are never reused;
  ids 1..1000 are the engine's own (system) entities and the lowest free one is reused.
- Each component type has an array. Components never move when others are added: a
  `T&`/`T*` stays valid across adds. Removing a component (or destroying an entity) is O(1):
  it destroys the component at once (its strings, `shared_ptr` and Lua references are freed)
  and leaves a hole. The first index-based read of the array (`size` excepted:
  `getComponentFromIndex`, `getEntity`, `getIndex`, the move and sort calls, a query) closes
  every hole in one pass that keeps the survivors' order. Lookups by entity
  (`findComponent`, `getComponent`, `hasEntity`) never move anything. A reference taken
  before a removal can move at that next index read: take references after removing.
- Every type declares how its array keeps its order:
  - **Hierarchy**: the order of the GameObjects' Transforms (parents before children) for
    Mesh, InstancedMesh, Model, Bone, Polygon, RectTransform, UI, Points, LineRenderer and
    AudioSource. A component added later to a mid-hierarchy GameObject, or a reparent, makes
    the array re-sort once at its next read, so draw and UI order always follow the hierarchy.
  - **Insertion** (most types): the order they were added in.
  - **Manual**: Skybox (the editor sets its order; the first one is the scene's sky).
  - **Unordered**: swap-and-pop removal, for new types that opt in (none today).
- The Transform array is the hierarchy order itself: a GameObject's children come right after
  it, so the transform pass is one linear walk.
- A GameObject without a Transform (a Skybox, a Fog) keeps its own place among the root
  GameObjects. `move_entity` places a GameObject with a Transform by the hierarchy only: next to
  one without (`after=Skybox`) is refused; use `parent` (and `index`, counted among the
  GameObjects with a Transform) or a sibling with a Transform.
- A scene has up to 256 component types; `get_ecs_stats` shows how many are registered.
- Memory per type: `bytes` (the pages), `element_bytes` (one component) and `heap_bytes` (the arrays
  its components hold outside the pages: a mesh's submeshes and bones, a sprite's frames, a
  tilemap's tiles). Arrays have no fixed capacity: a sprite's mesh is a few KiB, each submesh more.

## The change tick

Every scene has a change tick that only grows. It advances before each scheduled system runs,
at each command-buffer playback and after each editor command (edit, undo, redo). Each
component remembers the tick it was added at and the tick it last changed at:

- stamped automatically: adding a component, editor property and transform edits (and their
  undo/redo), the MCP `set_properties`/`set_transform`/`set_runtime_property` and other
  component tools, the object wrappers' setters (`object.position = ...`), Lua writes to
  component fields (`ui.color = ...`), scene and prefab loading of existing components,
  prefab override changes, and the transform pass for every Transform it recomputes;
- **not** stamped: a C++ write through a `T&` (`getComponent<T>(e).x = 1`). Call
  `registry.markChanged<T>(e)` (Lua `scene:markChanged(e, "TypeName")`) after such a write.

Reading the tick (`get_ecs_stats` `change_tick`, Lua `scene:getChangeTick()`) gives a value
every later change is newer than: pass it to `query_entities changed_since_tick` or
`scene:findChangedEntities(type, tick)` to get what changed since.

## Queries

C++: `registry.view<Read<A>, Write<B>, Optional<C>, Without<D>>()` with `.each(fn)`
(`fn(Entity, const A&, B&, C*)`), `.changedSince(gate)` / `.addedSince(gate)` filters and
`.forEachPage(fn)` (the rows of each storage page, for splitting work into jobs). The first
`Read`/`Write` type drives: rows come in its array's order (parents first for Transform and
the Hierarchy types). `Write<T>` declares access; it does not stamp a change.

MCP: `query_entities {components, exclude?, changed_since_tick?, runtime?, limit?}`.
Lua: `scene:findEntitiesWith({"Transform", "MeshComponent"}, {"UIComponent"})`.

## Structural changes during iteration

Destroying entities or removing components while a loop walks an array is safe inside a
`StructuralDeferScope`: `destroyEntity`, `destroyEntities`, `removeComponent` and Lua
`GameObject.destroyImmediate` are recorded and applied when the outermost scope closes (adds
stay immediate). The engine opens one around physics contact and trigger callbacks, UI and
HTML UI event dispatch, ActionSystem callbacks (`onStart`, `onStep`, ...) and every system
wave. So a `destroyImmediate` in `onCollisionEnter` removes the GameObject right after the
callbacks of that step, not in the middle of them (as Unity advises against
DestroyImmediate in callbacks). Outside those, Lua structural calls are immediate as before.

`EntityCommandBuffer` records the same changes explicitly (create with a `PendingEntity`,
destroy, destroyHierarchy, add, remove, setParent, setActive/Name/Tag/Layer, defer) and plays
them back in order; a command on a GameObject destroyed since (an `EntityRef` that is no
longer alive) is skipped with a warning.

## Systems

A scene runs its systems phase by phase: FixedUpdate (fixed steps), Update, PostUpdate and
Draw. The seven engine systems (ActionSystem, MeshSystem, UISystem, UIDocumentSystem,
RenderSystem, PhysicsSystem, AudioSystem) are exclusive main-thread systems: each runs alone
in its own wave, in that order. Systems added with `Scene::addSystemFunction(descriptor, fn)`
declare what they read and write (component types), the resources they use (Lua, GPU, Audio,
Physics, Structural, MainThread, Exclusive), what they `dependsOn` and `everyNFrames`. A
system goes into the wave after the last earlier system it conflicts with (a write against a
read or write of the same type, a shared resource, a dependency), so conflicting systems keep
their registration order. A wave's arrays are compacted and sorted before it starts; inside a
wave of declared systems structural changes go to each system's command buffer, played back
after the wave in registration order. Each system's change gate tells it what changed since
it last ran. A wave of two or more declared systems runs them concurrently on the job system's
workers (the main thread takes part, and `MainThread` systems run on it); every SubSystem is
`Exclusive`, so it runs alone. Without workers (the web build without threads) or with
`JE_ECS_SERIAL=1` the waves run one after the other on the main thread with the same result:
a wave never depends on the order of its systems.

`get_system_schedule {runtime?}` lists the phases, waves, declarations and timings (last,
average and max ms over 120 runs, call count) and the last frame's wave trace.
`set_system_options {name, enabled?, every_n_frames?}` changes a system of the running game
only: it is debug state, not saved, and gone when Play stops (in Edit mode it is an error).
Disabling AudioSystem pauses its sounds.

Editor: **Window > Analysis > Systems** shows the same table (Enabled and Every N editable in
Play), the wave trace of the last frame, and the scene's entity counts (user/system), the
component types registered out of 256 with a count per type, pending holes, arrays waiting
to re-sort and ECS memory. In Edit mode it shows the selected scene.

## Lua changes

`scene:getSignature(e)` (and `EntityManager:getSignature`) returns a 256-character string
(it was 64 characters): bit 0 is still the last character. Signature strings of any length up
to 256 are accepted, so stored 64-character strings keep working; a script that checks
`#sig == 64` must check for 256.
