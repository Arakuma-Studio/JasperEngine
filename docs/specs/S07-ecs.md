# S07 — ECS performance & features

Wave 1. Depends on S01 (C++20), S02 (gtest), S06 (job system). Read `README.md` first.

## 1. Goal

Make our ECS (`engine/core/ecs`, `engine/core/registry`) scale to 100k entities without changing
what it is: components stay arbitrary C++ types (`std::string`, vectors, `shared_ptr`, Lua refs),
scene files, Lua bindings, prefabs and editor/MCP behaviour stay the same, and component arrays
stay in parent-before-child hierarchy order, so transform propagation is still one linear pass.
On top of that core we add change tracking, deferred structural commands, a read/write-set system
scheduler on the S06 job system with per-system timings, and more than 64 component types.

## 2. Scope

In scope:
- `ComponentArray<T>`: amortized O(1) remove (lazy, order-preserving compaction), sparse-page
  entity→slot index instead of `unordered_map`, paged storage so `T&` survives adds, declared
  ordering policies, and lazy hierarchy re-sort.
- `EntityManager`: dense, paged metadata instead of `std::map`; O(1) system-id allocation.
- `Signature`: 256 component types (a fixed-size mask).
- Change tracking: `Added<T>` / `Changed<T>` / `Removed<T>` since a tick; a typed `view<>` query API.
- `EntityCommandBuffer` and defer scopes for structural changes during iteration.
- `SystemScheduler`: phases, read/write sets, conflict-free waves on S06, every-N-frames, per-system
  timing, a wave trace.
- `EntityRef` (id + creation serial) for deferred/async consumers. `Entity` itself does not change.
- Benchmarks, unit tests, a Systems editor window, MCP tools, a small Lua surface, docs.

Out of scope:
- Their ECS core: archetypes, POD-only components, memcpy move plans, `consteval` type names
  (README do-not-copy list). We take ideas only, no code.
- Parallel or CSR-cached transform propagation (analysis item #12). Propagation stays the single
  linear pass in `RenderSystem` and keeps that shape. `Changed<Transform>` makes a later stream
  possible.
- Reflection-driven serialization and inspectors (S08), the profiler UI (S15) and the job system
  itself (S06).
- Making the existing seven `SubSystem`s run in parallel. They stay main-thread and exclusive
  (see D9).

## 3. Current state (verified)

- `Entity = unsigned`, `NULL_ENTITY 0` (`engine/core/ecs/Entity.h`). User ids 1001+ are monotonic
  and never reused. System ids 1..1000 take the first free slot through a linear `map.count` scan
  (`EntityManager.h:67`).
- Metadata is a `std::map<Entity, EntityMetadata>` (`EntityManager.h:36`), so every name, tag,
  active, layer or signature access is O(log n). `findByName` and `findByTag` scan the map in
  ascending-id order, and first-match-by-lowest-id is relied on (`EntityManager.h:168`).
  `getEntityList()` is ascending.
- `Signature = std::bitset<64>` (`Signature.h`) with `ComponentId = unsigned char`
  (`ComponentManager.h:17`). 53 `registerComponent` calls exist today
  (`EntityRegistry.cpp:10-63`), so the cap is close, and S18–S22 add more types. Past 64,
  `bitset::set` throws. Lua sees a signature as a 64-char bitset string (`LuaBridgeAddon.h:167`).
  `Scene::onEntityDestroyed` loops over all `signature.size()` bits for each system
  (`Scene.h:74`).
- `ComponentArray<T>` (`ComponentArray.h`) is a `std::vector<T>` plus `unordered_map<Entity,size_t>`
  plus `indexToEntity`. `remove()` (`:105`) does `vector::erase` and then reindexes the tail, which is
  O(n) moves of non-trivial types plus O(n) hash writes. Growth moves every element (`grow`, `:41`),
  so any `T&` held across an add dangles. `entitiesDestroyed` (`:294`) already compacts once for a
  batch.
- `ComponentManager::entityDestroyed` (`:144`) visits all arrays, not just the signature's.
  `destroyEntity` called one at a time is therefore O(types × n) per entity, and destroying n
  entities is O(n²).
- Hierarchy order: the Transform array is the source of truth for both hierarchy and sibling
  order (`EntityRegistry::moveChildToIndex` `:529`, `addEntityChild` `:476`,
  `findBranchLastIndex` `:401`, `moveChildUp/Down/Top/Bottom`). Ten dependent arrays (Mesh,
  InstancedMesh, Model, Bone, Polygon, RectTransform, UI, Points, LineRenderer, AudioSource) are
  re-sorted by a hand-written list (`EntityRegistry.cpp:83`). That list runs only after moves, so
  a component added later to a mid-hierarchy entity is out of order until the next move, and the
  order changes after save and reload. Skybox follows the editor list (`sortComponentsByEntityOrder`,
  `editor/util/ProjectUtils.cpp:859`). Direct callers: `ObjectManager.cpp:124`,
  `editor/command/type/PasteEntitiesCmd.cpp:130,373`.
- Propagation: one pass over Transforms by index, with the parent looked up by entity
  (`RenderSystem.cpp:12035-12058`, `updateTransform` `:10254`). Correctness requires every parent
  to come before its children.
- The index-based API is used everywhere: about 238 `getComponentFromIndex`, about 520 `getEntity(`
  and about 361 `getComponentArray` call sites across engine and editor. Scene encoding walks the
  Transform array by index (`editor/Stream.cpp:3667-3700`). Lua init deliberately re-reads by index
  while scripts append (`LuaBinding.cpp:1050`). Its semantics must not change.
- `removeComponent<Transform>` reparents children by scanning all Transforms
  (`changeTransformChildren` `:180`).
- Systems: seven `SubSystem`s run in registration order, single-threaded (`Scene.cpp:31-38`,
  `Scene.h:45`, `SubSystem.h`). There is no access declaration, no timing and no queries.
  `GameObject.destroy` is already deferred to `ObjectManager::update` after the update phase
  (`Engine.cpp:1303`, `ObjectManager.cpp:303-341`). `ObjectManager::createdSince` (`:154`) relies
  on monotonic user ids.
- Existing microbenchmarks to extend: `tools/performance/{component_sort.cpp,entity_lookup.cpp,run_core.py}`
  (compiled against the real headers, with a `--baseline-ref` git comparison).

## 4. Reference (OpenEngine, MIT, read-only; ideas only, no code copied)

Under `external/OpenEngine/Engine/Modules/ECS/Include/ECS/`:
- `ECS.h:78-123`: 20+12-bit generational `EntityHandle`. We take the staleness check but not the
  encoding (D7).
- `ArchetypeTable.h:272-300`: swap-and-pop. It is O(1) but breaks ordering. We get O(1) another
  way (D1).
- `ArchetypeTable.h:285-352`, `ChangeFilter.h`: per-column write versions and a per-(system, query)
  `ChangeGate`. We adopt the entry-sample contract (sample the tick before running and write that
  sample back as the gate) and monotonic 64-bit ticks.
- `Entity.h:110-170`: `CommandBuffer` (SPSC ring, 64-byte SBO, memcpy payloads). We keep the SBO
  idea and use a type-erased move/destroy instead of memcpy. We drop the ring: one buffer per
  system invocation gives a deterministic playback order.
- `Scheduler.h`: a greedy wave packer over Read/Write `DenseSignature` plus `DependsOn`. We adopt it
  and add order preservation (D9). `Systems.h`: every-N-frames and per-system timing.
  `SystemWaveTrace.h`: a wave trace for the editor and MCP. `QueryAccessProbe.h`: a debug access
  check, which we adapt as D10.

Licence: MIT. Nothing is copied verbatim, so no `NOTICE.md` entry is needed. If an implementer
does copy code, the README attribution rule applies.

## 5. Requirements

### Functional — storage & hierarchy order
- **R1** `ComponentArray<T>::remove` and `entityDestroyed` run in O(1) amortized. They release the
  component's resources immediately by moving the value out and destroying it, so strings,
  `shared_ptr` and Lua refs are freed now. They leave a hole and clear the entity→slot mapping. No
  element moves at removal time.
- **R2** *Observational equivalence.* Every index-based observer sees the array exactly as today's
  eager-erase implementation would: compacted, with survivors in their previous relative order.
  Index-based observers are `size()`, `getComponentFromIndex`, `findComponentFromIndex`,
  `getEntity`, `getIndex`, the sort/move operations, and view iteration. Holes are compacted by one
  stable pass from the first hole, triggered lazily by the first such observation. Lookups by
  entity (`findComponent`, `getComponent`, `hasEntity`) never trigger compaction and stay O(1).
- **R3** Stable compaction preserves parent-before-child order by construction. Removing elements
  from a sequence never inverts survivors. No operation other than the explicit move/sort APIs may
  reorder an ordered array.
- **R4** Every registered type declares an ordering policy at registration:
  - `ComponentOrder::Hierarchy`: ordered by the owning entity's Transform index.
  - `Insertion` (the default): append order with stable removal.
  - `Manual`: Skybox, ordered through `sortComponentsByEntityOrder`.
  - `Unordered`: swap-and-pop, allowed only for new types that opt in.

  The ten types in `EntityRegistry.cpp:83` become `Hierarchy`. The hand-written list in
  `sortComponentsByTransform` is replaced by iterating the `Hierarchy` types in the signature. No
  existing type becomes `Unordered`.
- **R5** Lazy hierarchy re-sort. Transform moves (`moveRange` rotate) stay eager, because later
  branch queries need them. `sortComponentsByTransform` only marks the affected `Hierarchy` arrays
  order-dirty. A dirty array re-sorts once, through the existing `sortByKeys` and its sorted fast
  path, on its next index observation. Adding a `Hierarchy` component to an entity whose Transform
  is not last also marks the array dirty. This fixes the "out of order until the next move" defect
  in §3.
- **R6** Paged storage. Components live in fixed pages of at least 64 elements and about 16 KiB
  each, behind a random-access iterator. An add never moves existing elements, so a `T&`, `T*` or
  page span stays valid across adds. It is invalidated only by compaction, sort, move or clear.
  The old `grow()` path is deleted.
- **R7** The entity→slot index is a per-type sparse page table: 4096 `uint32` entries per page,
  allocated on first use and freed when its live count drops to 0. It replaces `unordered_map`.
- **R8** `ComponentManager` keeps a `ComponentId → array` table. `entityDestroyed` and
  `Scene::onEntityDestroyed` visit only the set bits of the signature, via `Signature::forEachSet`.
- **R9** `removeComponent<Transform>` reparents only the entity's branch, the contiguous range up to
  `findBranchLastIndex`, instead of scanning every Transform.

### Functional — entities & signature
- **R10** `EntityManager` stores metadata in a dense slot vector with a free list, plus a paged
  id→slot table (R7 layout). All per-entity getters and setters run in O(1).
  `getEntityList`, `findByName`, `findByTag` and `findAllByTag` keep the ascending-id order and
  lowest-id first-match semantics. They iterate the page table in order and skip empty pages.
  `forEachEntity(fn)` is added for callers that should not allocate.
- **R11** `createSystemEntity` returns the lowest free id in 1..1000. It uses a 1000-bit
  occupancy mask with find-first-zero, so it is O(16 words). `createUserEntity`, `recreateEntity`,
  `setLastUserEntity` and id monotonicity behave exactly as today.
- **R12** `Signature` becomes a 256-bit `ComponentMask`, `kMaxComponentTypes = 256`, which matches
  `ComponentId = unsigned char`. It keeps the `std::bitset` subset in use: `test`, `set`, `reset`,
  `any`, `none`, `count`, `size`, `|`, `&`, `|=`, `&=`, `==`, `to_string`, construction from a
  string. It adds `forEachSet` (via `std::countr_zero`) and `intersects`. Registering a 257th type
  logs an error and leaves that type unregistered. It never wraps the id or invokes UB.

### Functional — change tracking & queries
- **R13** Each registry has a monotonic `uint64` change tick. The tick advances before each
  scheduled system runs, at each command-buffer playback, and at the end of each editor command.
  Each array keeps a parallel `changedTick` and `addedTick` per slot, moved together with the
  components, plus an array-level `lastChangedTick` so a consumer can skip the whole type in O(1).
- **R14** Ticks are stamped on these paths:
  - `addComponent`: added and changed.
  - `registry.markChanged<T>(e)` / `ComponentArray::markChangedAt(i)`.
  - Editor property, transform, multi-property and component-values commands, including undo and
    redo.
  - MCP `set_properties`, `set_transform`, `set_runtime_property` and the other component-setting tools.
  - Lua component-property setters and the object-wrapper setters that already set a
    `needUpdate*` flag.
  - `Stream` decode and prefab override application.
  - The RenderSystem transform pass, for every Transform whose matrices it recomputed.

  Direct `T&` writes in C++ are untracked unless followed by `markChanged`. This is documented.
- **R15** `Removed<T>` is a per-array log of (entity, tick). Entries older than the oldest active
  gate, or older than 2 frames when no gate exists, are trimmed at frame end.
- **R16** Typed views (D4): `view<Read<A>, Write<B>, Optional<C>, Without<D>>()` with
  `.changedSince(gate)`, `.addedSince(gate)`, `.each(fn)` and `.forEachPage(fn)`. The driving
  array is the first `Read`/`Write` type. If that type is `Hierarchy` or `Transform`, iteration is
  parent-before-child. The result set and order are deterministic.

### Functional — commands & scheduling
- **R17** `EntityCommandBuffer` records these operations:
  - create (returns a `PendingEntity`)
  - destroy (registry semantics)
  - destroyHierarchy (`GameObject.destroy` semantics)
  - `add<T>(e, T&&)`, `remove<T>(e)`
  - setParent (`addEntityChild`)
  - setActive, setName, setTag, setLayer
  - `defer(std::function<void(Scene&)>)`

  Payloads are type-erased with a move and destroy, using a 64-byte SBO and falling back to the
  heap. Nothing is memcpy'd. Playback runs on the main thread in FIFO order. It resolves
  `PendingEntity`s, skips operations on stale `EntityRef`s with a warning, batches consecutive
  destroys through `destroyEntities`, and fires the normal hooks.
- **R18** `StructuralDeferScope`: while one is open on the main thread, `destroyEntity`,
  `destroyEntities` and `removeComponent` calls (including Lua `destroyImmediate`) go to the scene's
  main buffer and are played back when the outermost scope closes. Adds stay immediate, which is
  safe because of R6. The following must run inside a scope:
  - physics contact and trigger callback dispatch (`PhysicsSystem.cpp`)
  - UI and HTML-UI event dispatch (`UISystem.cpp`, `UIDocumentSystemInput.cpp`)
  - `ActionSystem` Lua callbacks
  - every scheduler wave

  Anywhere else, Lua structural calls stay immediate, as today.
- **R19** A `SystemDescriptor` declares: name, phase (`FixedUpdate`, `Update`, `PostUpdate`, `Draw`),
  `reads` and `writes` (Signature), `resources` (bitmask: `MainThread`, `Lua`, `GPU`, `Audio`,
  `Physics`, `Structural`), `dependsOn` (names), `everyNFrames` (default 1; dt is accumulated) and
  `enabled`. Existing `SubSystem`s are registered with `MainThread | Exclusive` descriptors.
- **R20** The scheduler builds waves per phase (D9). Systems in a wave run concurrently on S06
  jobs, and `MainThread` systems run on the calling thread. Before a wave starts, every array in
  the wave's read∪write set is settled (compacted and re-sorted) on the main thread. During a
  parallel wave, compaction, sort and structural operations on the registry are forbidden; systems
  record into their own command buffer, which is played back after the wave in registration order.
- **R21** With zero workers (an Emscripten build without pthreads, or `JE_ECS_SERIAL=1`), waves run
  serially in the same order. The results must be bit-identical to the parallel run.
- **R22** Each system records per phase: last, average (120 frames), max (120 frames) and call
  count. Each frame also records a wave trace (waves → systems → start/end µs). Both are exposed to
  S15's CPU profiler as named scopes.

### Functional — handles
- **R23** `EntityRef { Entity id; uint32_t serial; }`. The serial is a per-registry creation
  counter stored in metadata and assigned on create and on `recreateEntity`.
  `registry.makeRef(e)` creates one and `registry.isAlive(ref)` checks it. Command buffers,
  scheduler jobs and async callbacks use `EntityRef`. Scenes, Lua, prefabs, MCP and the editor keep
  using raw `Entity`.

### Platform & backends
- **R24** CPU-only. It must build and pass the tests on GCC 13, Clang/Xcode, MSVC, NDK and
  Emscripten, with and without threads. It needs no atomics wider than 64 bits. `uint64` ticks are
  plain fields: they are written only on the main thread or by systems that own a write set.

### Performance (Release `-O2`, the S02 Linux build server, `tools/performance/ecs_bench`)
Workload: entities with `Transform` plus a `Payload {std::string; std::vector<int>; std::shared_ptr<int>}`,
half of them parented in chains of depth 4, and a second `Hierarchy` type on 50% of them.
- **R25** Absolute budgets (median of 7 runs):

  | Op | 10k | 100k |
  |---|---|---|
  | create entity + 2 components | ≤ 3 ms | ≤ 35 ms |
  | `destroyEntity` one by one (no observation in between) + first observation | ≤ 2 ms | ≤ 25 ms |
  | `removeComponent<Payload>` for 10% random + first observation | ≤ 0.5 ms | ≤ 6 ms |
  | `addComponent<Payload>` to every entity | ≤ 1.5 ms | ≤ 18 ms |
  | index loop (`size()`/`getComponentFromIndex`/`getEntity`) + parent lookup (the propagation shape) | ≤ 0.15 ms | ≤ 2 ms |
  | `view<Read<Transform>,Write<Payload>>().each` | ≤ 0.12 ms | ≤ 1.5 ms |
  | random `findComponent` / `getEntityName`-free metadata getters | ≤ 15 ns/op | ≤ 25 ns/op |
  | `addEntityChild` that moves a 100-entity branch + lazy re-sort of 10 Hierarchy arrays | ≤ 0.3 ms | ≤ 3 ms |
- **R26** Relative budgets against `--baseline-ref` on the same machine:
  - one-by-one destroy of 10k: at least 20× faster
  - random metadata get at 100k: at least 3× faster
  - index-loop iteration: at most 15% slower (paged-storage overhead)
  - `view` iteration: no slower than the baseline index loop
- **R27** Memory overhead per live component, excluding `sizeof(T)`: ≤ 24 B on average at 100k
  (index page, `indexToEntity`, two ticks). Per-entity metadata excluding strings: ≤ 96 B. When
  nothing is being iterated in parallel, the scheduler adds ≤ 2 µs per system per frame.

### Editor UX (human)
- **R28** Window ▸ Analysis ▸ **Systems** (ImGui) has these parts:
  - A table of phase, wave, system, avg, max and last ms, an Enabled checkbox and an Every N field.
  - A "Wave trace" bar view of the last frame.
  - A footer: entities (user/system), component types used out of 256, per-type count, pending
    holes, dirty arrays and ECS memory.

  Enabled and Every N are Play-mode debug state. They are not scene data, so they are not undoable
  and reset when Play stops. In Edit mode the table shows the editor-scene registry stats only.

### Agent UX (MCP, Lua, CLI)
- **R29** MCP tools (in `editor/mcp/tools/`, documented in `docs/mcp`):
  - `get_ecs_stats {runtime?: bool}`: counts, per-type counts, holes, dirty arrays, bytes,
    `changeTick`.
  - `get_system_schedule {runtime?: bool}`: phases, waves, descriptors (reads/writes as component
    names), timings, last wave trace.
  - `set_system_options {name, enabled?, every_n_frames?}`: Play only. In Edit mode it returns an
    error.
  - `query_entities {components: [names], exclude?: [names], changed_since_tick?: int, runtime?: bool, limit?: int=200}`:
    ids, names and paths in deterministic order.

  All component names resolve through `Catalog`. S31's `jasperengine-editor call` exposes the
  tools automatically.
- **R30** Lua:
  - `Scene:getChangeTick()`
  - `Scene:findEntitiesWith({"Transform","MeshComponent"}[, {exclude}])`, returning an id array in
    driving-array order
  - `Scene:findChangedEntities("Transform", sinceTick)`
  - `Scene:markChanged(entity, "MeshComponent")`

  Lua `getSignature` returns a 256-char string, MSB first like `bitset::to_string`. The parser
  accepts any length up to 256, so 64-char strings stored by old scripts still parse. Stubs and the
  single JSON API description (S23) are updated.

### Data & compatibility
- **R31** The scene, prefab and project formats are unchanged and get no version bump. Entity ids,
  hierarchy order, sibling order and component order in the saved YAML are identical to today for
  every scene that loads.
- **R32** The public `EntityRegistry`, `ComponentArray`, `EntityManager` and `SubSystem` signatures
  stay source-compatible. New behaviour is additive. The only intentional observable change is R5
  (dependent arrays are now always hierarchy-ordered).

## 6. Design constraints & decisions

- **D1 O(1) removal plus ordered arrays: holes with lazy stable compaction.** Swap-and-pop is
  rejected for ordered types because it can put a child before its parent. Tombstones that stay
  visible are rejected because about 760 index call sites would need dead-slot checks. Holes are
  invisible to every index observer (R2), so the cost moves from "O(n) per removal" to "O(n) once
  per batch of removals before the next index read". Batches are made explicit by command
  buffers, defer scopes and `ObjectManager::update`. The worst case (remove, read, remove, read)
  equals today's cost and is never worse.
- **D2 The Transform array is the single order authority.** Its rotations stay eager. Dependent
  arrays are derived from it lazily (R5). Parent-before-child for propagation holds because
  (a) moves are branch-preserving rotations, as today, (b) compaction is stable, (c) adds append,
  and an append under an existing parent is followed by the `addEntityChild` move, as today.
- **D3** `ComponentArray` stays a template over arbitrary `T`, with only move-constructibility and
  move-assignability required. Compaction and sort use moves. That is the same requirement
  `erase` and `std::rotate` impose today, so no component changes.
- **D4 API sketch** (`engine/core/ecs/View.h`, `CommandBuffer.h`, `SystemScheduler.h`):
  ```cpp
  enum class ComponentOrder : uint8_t { Insertion, Hierarchy, Manual, Unordered };
  template<class T> void EntityRegistry::registerComponent(ComponentOrder = ComponentOrder::Insertion);
  using Tick = uint64_t;  struct ChangeGate { Tick last = 0; };   // entry-sample contract
  Tick EntityRegistry::changeTick() const;
  template<class T> void EntityRegistry::markChanged(Entity);
  auto v = scene.view<Read<Transform>, Write<MeshComponent>, Without<UIComponent>>();
  v.changedSince(gate).each([](Entity e, const Transform& t, MeshComponent& m){ ... });
  struct EntityRef { Entity id = NULL_ENTITY; uint32_t serial = 0; };
  class EntityCommandBuffer { PendingEntity create(); void destroy(EntityRef);
    template<class T> void add(EntityTarget, T&&); template<class T> void remove(EntityRef);
    void setParent(EntityTarget child, EntityTarget parent, bool keepWorld); void defer(std::function<void(Scene&)>);
    void playback(Scene&); };
  struct SystemContext { Scene& scene; double dt; EntityCommandBuffer& cmd; ChangeGate& gate; };
  struct SystemDescriptor { std::string name; SystemPhase phase; Signature reads, writes;
    uint32_t resources; std::vector<std::string> dependsOn; uint32_t everyNFrames = 1; };
  Scene::registerSystem<T>(SystemDescriptor);            // existing overload = MainThread|Exclusive
  Scene::addSystemFunction(SystemDescriptor, std::function<void(SystemContext&)>);
  ```
- **D5 Ticks.** Ticks are 64-bit and monotonic and never wrap in practice. A gate is sampled when
  the system starts and written back after it runs. A system therefore sees its own writes again
  only if it writes after its sample, which is the documented OpenEngine M14 behaviour.
- **D6 No implicit write tracking.** `getComponent` returns `T&` as today. Stamping every mutable
  access would mark everything as changed on each frame. `Write<T>` in a view declares access for
  the scheduler and does not stamp anything.
- **D7 Generational handles: not in `Entity`.** `Entity` stays a 32-bit raw id because:
  - Scenes store raw ids (`Stream.cpp` `entity:` and parent fields).
  - Lua uses integers.
  - `createdSince` needs monotonic ids.
  - User ids are never reused, so ABA is limited to system ids and editor `recreateEntity` (undo),
    where resolving to the same id is intended.

  `EntityRef`'s serial protects the deferred and async paths only. The serial is a creation
  counter, not a per-slot version, so it needs no storage for dead ids, survives page frees, and
  wraps only after 2³² creations (accepted).
- **D8 Signature width 256, fixed.** Not a dynamic bitset: 32 bytes, no allocation, and it matches
  `ComponentId`. If a later spec needs more, it widens `ComponentId` and `kMaxComponentTypes`
  together.
- **D9 Order-preserving wave packing.** Systems are walked in registration order. System *i* goes
  into wave `1 + max(wave(j))` over the earlier systems *j* that conflict with it. Two systems
  conflict on W∩W, W∩R or R∩W of components, on a shared exclusive resource, on `dependsOn`, or
  when either one is `Exclusive`. Conflicting systems therefore never reorder relative to today,
  and only provably independent systems overlap. A system with `MainThread` runs on the caller,
  while the wave's other jobs run on S06 workers. The plan is rebuilt only when the system set or a
  descriptor changes.
- **D10 Debug access check.** In debug builds a thread-local "current system" access set is
  checked in `getComponentArray<T>()`, `findComponent` and the views. An undeclared access or a
  structural call inside a parallel wave triggers `Log::error` plus an assert. Release builds
  compile this out.
- **D11 Order is not mutated during parallel waves.** Lazy compaction and re-sort mutate on
  observation, so they are settled before each wave (R20). An observation that would mutate during
  a parallel wave is a debug assert and, in release, a no-op that logs an error.
- **D12** Tags are interned to `uint16` ids in metadata (strings are kept in a registry-wide table).
  `getTag` still returns `std::string`, and `findByTag` compares ids.

## 7. Acceptance criteria

Unit tests live in `engine/tests/core/ecs/` (S02 gtest target `jasperengine_core_tests`).

- **AC1** `ComponentArrayTest.DifferentialAgainstEagerModel`: 200 seeds × 2000 random operations
  over a reference copy of today's `ComponentArray.h`:
  - insert, remove, entitiesDestroyed
  - moveEntityToIndex, moveEntityRangeToIndex
  - sortByComponent, sortByEntityOrder
  - interleaved index observations

  Every observation (size, getEntity(i), getIndex, component values) must match the reference
  exactly. Covers R1–R3. A `Payload` with `std::string` and `shared_ptr` must show
  `use_count == 0` right after remove (R1).
- **AC2** `ComponentArrayTest.ReferencesStableAcrossAdds`: the address of an element is unchanged
  after 10k inserts (R6). `ComponentArrayTest.SparseIndexPagesFreed`: ids 1001..200000 are inserted,
  then all are removed, leaving 0 index pages (R7).
- **AC3** `EntityRegistryTest.HierarchyInvariantFuzz`: 100 seeds of random create, addEntityChild,
  moveChildUp/Down/Top/Bottom, removeComponent<Transform>, destroy and paste-style range moves.
  After each step, for every Transform at index i whose parent has a Transform, the parent's index
  is less than i. Every `Hierarchy` array is ordered by Transform index. Sibling order equals a
  reference tree model (R3, R5, R9).
- **AC4** `EntityRegistryTest.LateHierarchyComponentIsOrdered`: adding a MeshComponent to a
  mid-hierarchy entity leaves the Mesh array in Transform order on the next read (R5).
- **AC5** `EntityManagerTest.*`:
  - ascending `getEntityList`
  - lowest-id first match for names and tags
  - lowest free system id after random frees
  - monotonic user ids
  - `recreateEntity` and `setLastUserEntity` behave as on main (R10, R11)
- **AC6** `SignatureTest.*`:
  - bits 0, 63, 64 and 255 set, test and reset
  - `forEachSet` order
  - the `to_string` and `from string` round trip
  - a 64-char legacy string parses to the same low bits
  - the 257th `registerComponent` fails cleanly (R12)
- **AC7** `ChangeTrackingTest.*`: these paths stamp ticks: add, markChanged, an editor-command
  stand-in, and the RenderSystem pass via the extracted function. Gates follow the entry-sample
  contract. `lastChangedTick` skips the type. `Removed<T>` is trimmed (R13–R15). The MCP test
  `tools/mcp/tests/test_ecs.py::test_changed_since` checks that `set_transform` on an entity makes
  the entity appear in `query_entities changed_since_tick=<tick before>`.
- **AC8** `ViewTest.*`:
  - the Read/Write/Optional/Without matrix
  - the driving-order is parent-before-child for Transform
  - `forEachPage` covers the same elements as `each` (R16)
- **AC9** `CommandBufferTest.*`:
  - FIFO playback
  - `PendingEntity` resolution and parenting
  - a stale `EntityRef` is skipped with a warning
  - a non-POD payload (`std::string`, `shared_ptr`, a mock LuaRef counter) is destroyed exactly once
    whether played back or discarded
  - consecutive destroys produce one compaction (counter hook) (R17)
- **AC10** `DeferScopeTest.*`: inside a scope, `destroyImmediate` keeps the entity alive until the
  scope closes, nested scopes play back once, and an outer index loop never sees a shift (R18).
  Manual check: a sample project whose Lua `onCollisionEnter` destroys the other body and adds a
  component to itself runs 1000 frames under ASan (S02 preset) with no error.
- **AC11** `SchedulerTest.*`:
  - Wave packing on synthetic descriptors matches expected waves.
  - Conflicting systems keep registration order.
  - `dependsOn` and `Exclusive` are respected.
  - `everyNFrames=3` runs every third frame with accumulated dt.
  - The existing seven systems run in one wave each, in today's order.

  `SchedulerTest.ParallelEqualsSerial`: 8 synthetic systems over 50k entities for 300 frames
  produce the same state hash with S06 workers and with `JE_ECS_SERIAL=1` (R20, R21).
  `SchedulerTest.AccessCheckFires` (debug only) covers D10.
- **AC12** `SchedulerTest.Timings`: timings and the wave trace are populated. The overhead of 20
  empty systems is ≤ 40 µs/frame (R22, R27).
- **AC13** Benchmarks: `tools/performance/ecs_bench.cpp` plus `run-ecs-bench.py` (in the
  `run_core.py` style, with `--baseline-ref`). The script exits non-zero if any R25 or R26
  threshold is missed or if any behaviour assertion fails. It also reports bytes per component and
  per entity against R27. CI runs it on the S02 build server and stores the CSV as an artifact.
- **AC14** Compatibility (R31):
  - `tools/smoke-test.sh` stays green.
  - The new test `StreamRoundTripTest.ExampleScenesByteIdentical` loads every scene and prefab under
    the examples and template projects, saves them, and gets byte-identical YAML.
  - The existing MCP suite `tools/mcp/tests` passes unchanged.
  - Golden images (S02) are unchanged for every scene except ones that exercise the R5 fix, and
    each such scene is listed in the PR with its before/after.
- **AC15** MCP: `tools/mcp/tests/test_ecs.py` covers `get_ecs_stats` counts after creating and
  deleting via MCP, `get_system_schedule` listing the seven systems with timings in Play,
  `set_system_options` rejected in Edit mode and pausing ActionSystem in Play, and `query_entities`
  include/exclude/limit and the order (R29).
- **AC16** Lua: `engine/tests/core/ecs/LuaEcsTest.cpp` runs a script that uses
  `findEntitiesWith`, `findChangedEntities`, `markChanged`, `getChangeTick` and a `getSignature`
  round trip (R30).
- **AC17** Manual check of the editor Systems window in Play with a template project:
  - the table shows seven systems with non-zero avg ms
  - unchecking AudioSystem silences audio, and stopping Play re-enables it
  - the footer component-type count equals the `list_component_types` count (R28)

## 8. Dependencies & conflicts

- **Depends on:** S01 (`<bit>`, concepts), S02 (gtest target, ASan preset, golden images, CI
  benchmark job), S06 (job submission, counters, `ParallelFor`, worker count 0 on web). Until S06
  lands, the scheduler runs serially (R21) and the rest of S07 can merge.
- **Feeds:** S08 (registration can carry `ComponentOrder` and the reflection table, and inspector
  multi-edit stamps `markChanged`), S15 (system timings and wave trace), S19, S20, S21 (first real
  parallel systems via `SystemDescriptor`), S22, S26 (BVH refit from `Changed<Transform>`),
  S23 (Lua API JSON).
- **Interacts:** S24, because undo `recreateEntity` must assign a new serial and keep the id. S03
  makes no format change here, so no version bump. S09 requires that RAII subscriptions do not hold
  `T&` across compaction.
- **Merge hotspots:**
  - `engine/core/ecs/*` (all of it)
  - `engine/core/registry/EntityRegistry.{h,cpp}`
  - `engine/core/Scene.{h,cpp}` (system registration, hooks; S15, S22 and S23 also touch it)
  - `engine/core/Engine.cpp` frame loop (S06, S09, S15)
  - `RenderSystem.cpp` transform pass (S10–S13 also edit this file; keep the S07 change confined to
    `updateTransform`'s caller loop)
  - `LuaBridgeAddon.h`, `binding/ECSClassesLua.cpp`, `binding/CoreClassesLua.cpp`
  - `PhysicsSystem.cpp` and `UISystem.cpp` callback sites (defer scopes)
  - `editor/command/type/*PropertyCmd*`, `ComponentValuesCmd.cpp` (stamping; S08 and S24 also touch these)
  - `editor/mcp/tools/`

## 9. Risks & defaults

- **Hidden index-observation in hooks.** A `SubSystem::onComponentRemoved` that walks arrays by
  index forces a compaction per destroy and loses the O(1) gain. *Default:* audit the seven
  systems' hooks as part of S07. The R25 destroy benchmark runs through a real `Scene` stand-in
  with the hooks attached, so this is caught.
- **Paged storage slows index loops.** *Default:* the 15% regression budget (R26). If it is
  exceeded, the hot engine loops (the transform pass and the RenderSystem mesh loops) move to
  `forEachPage` spans rather than going back to contiguous storage. Pointer stability on add is a
  correctness fix and outranks a few percent of speed.
- **The R5 order fix changes draw order in rare existing scenes** (a component added after
  creation to a mid-hierarchy entity). *Default:* accept it, because that is the order those scenes
  already get after a save and reload. Changed golden images are listed in the PR (AC14).
- **The 256-char Lua signature string breaks a script that checks `#sig == 64`.** *Default:*
  accept it and document it in the Lua API changelog. No such use exists in the repo's Lua.
- **Untracked direct writes make `Changed<T>` miss changes.** *Default:* document it, and stamp at
  every API boundary in R14. Consumers that cannot tolerate a miss also keep the component's own
  `needUpdate` flags, which are unchanged.
- **Deferring Lua `destroyImmediate` inside callback scopes changes timing**, because the entity
  stays alive until the dispatch loop ends. *Default:* accept it. This matches Unity's guidance
  against DestroyImmediate in callbacks and removes a use-after-free class.
- **Open question: should `Unordered` be used by any existing type?** *Default:* no, in S07. Each
  later spec (for example S20 particle emitters) decides for its own new types.
- **Open question: per-root parallel transform propagation.** *Default:* out of scope for S07. A
  follow-up can build on `Changed<Transform>` and the settled-order guarantee.

## Deviations (implementation)

- **Per-slot ticks are 32-bit offsets from a 64-bit per-array base** (`ecs::TickColumn`). The
  API ticks are `uint64`. When a tick passes base + 2^32 - 1 the column rebases; offsets older
  than the new base become the base, so a change billions of ticks old may be reported again,
  never missed. This keeps R27 (index page 4 B + slot owner 4 B + two ticks 8 B, with room
  for partly filled pages and sparse types) where two 64-bit ticks alone would exceed 24 B.
- **R1: removal destroys the component in place** (`std::destroy_at`) and leaves raw storage,
  instead of moving it out first: the resources go at once whatever the type's move does
  (a copy-only type would otherwise keep them); compaction move-constructs into the holes.
- **Equal sort keys keep their order.** The re-sort (Hierarchy arrays, `sortByComponent`,
  `sortByEntityOrder`) is stable: runs are merged pairwise (a moved branch leaves two or three),
  `std::stable_sort` past 64 runs. `std::sort` left entities without a Transform (equal keys) in
  an unspecified order; the AC1 reference copy uses `std::stable_sort` for that sort only.
- **`moveChildToIndex` re-sorts the types of the whole moved branch**, not only the root's
  (otherwise a child's Mesh stayed out of order: R5/AC3).
- **The change tick is sampled.** `changeTick()` (MCP `change_tick`, Lua `getChangeTick`)
  returns the current tick and makes the next stamp advance first, so "changed since a tick
  read before an edit" always holds. The gate of a scheduled system is the tick it ran at
  (its own writes of that run are not new to it next run; what it records into its command
  buffer is, being stamped at playback); the clock is sampled after each wave.
- **`changedSince`/`addedSince` on a view** pass a row when any of its Read/Write components
  is newer than the gate; MCP `query_entities changed_since_tick` and Lua
  `findChangedEntities` do the same for the listed types.
- **`Structural` systems run alone in their wave** (like `Exclusive`): structural changes are
  refused during a wave of declared systems. A `dependsOn` naming a later-registered system
  moves the dependent after it; a dependency cycle is logged and the cycle runs in
  registration order.
- **Waves run on S06's job system.** A wave of two or more declared systems runs through
  `jobs::parallelFor` (the calling thread takes systems too); when the wave has `MainThread`
  systems, the others are submitted as High-lane tasks and the `MainThread` ones run on the
  calling thread meanwhile. `SystemScheduler::workerCount()` is the process job system's
  worker count (0 before the engine starts it and in single-thread mode, where waves run
  serially as with `JE_ECS_SERIAL=1`). Timings and trace entries are recorded per system on
  the thread that ran it; the trace keeps the wave's order. A system that throws: every
  started system of the wave finishes, then the first exception reaches the caller (a system
  not yet started may be skipped, as in a serial run). The S15 profiler hooks are called on
  the thread that runs the system. `SchedulerTest.WaveRunsOnWorkers` checks that a wave uses
  more than one thread and that `MainThread` systems stay on the caller;
  `SchedulerTestSlow.ParallelEqualsSerial` compares a parallel run with a serial one.
- **Removed<T> logs are capped** at 65536 entries (the older half dropped) for registries that
  never end a frame (prefab and scratch registries); scenes trim them at each frame end.
- **Lua component-field setters** (`addProperty(&T::m, &T::m)` on component classes) stamp
  through `setTrackedMember`, which finds the owning registry and entity by searching the pages
  of the live registries' arrays of that type (`EntityRegistry::markChangedComponent`).
- **Physics defer scopes**: the Jolt contact listener runs on Jolt's threads, serialized by its
  callback mutex, while the main thread waits in `PhysicsSystem::Update`; its scope is opened
  with `anyThread` so those callbacks' structural calls are deferred too.
- **AC14 round trip runs through the editor binaries** (`tools/mcp/tests/test_ecs_roundtrip.py`:
  `scene-dump --save` for every scene, Prefab Mode save for every prefab of `tools/fixtures/*`
  and the MCP fixture). The fixtures are hand-written, so "byte-identical" is checked against
  an editor built from the revision before S07 (`JASPER_BASELINE_EDITOR`: identical for all 20
  scenes and 3 prefabs, first and second save) and, without it, as stability across a second
  save (a prefab's `nextEntity` counter, which every Prefab Mode session raises before S07 as
  after it, left out). The repository has no other example or template scene files.
- **Unit tests** are in `engine/tests/core/ecs`, in S03's `jasperengine-core-tests` target
  (`tools/build.sh unit-tests`); `engine/tests/core/support/TestEngine` initialises the engine
  once for the process (a gtest environment, with a log-capturing `CoreTestSystem` that
  S03's `UserSettingsTest` also uses for its user data directory). `SchedulerTest.AccessCheckFires`
  is compiled only where `JASPERENGINE_ECS_ACCESS_CHECKS` is on (builds without `NDEBUG`), so a
  Release run has no skipped test; the Debug ASan/UBSan run covers it.
- **The Systems window** is not part of the saved panel visibility (it opens closed). It docks
  in the bottom panel with Project and Console: in the Inspector's column its eight-column table
  cut off the system names and the Enabled checkboxes.
- **AC17 is scripted** (`tools/mcp/tests/test_ecs.py::test_systems_window`, the default fixture
  in the harness's Xvfb): Play, `focus_window("systems")` (which returns each row's Enabled
  checkbox rect), the seven systems' Update rows and non-zero averages, the footer's type count
  against `list_component_types`, a real xdotool click on AudioSystem's checkbox (the system
  stops; the others keep running), and AudioSystem enabled again in the next Play. The
  screenshots are `docs/specs/evidence/S07/systems-window*.png`. Audio silence itself is not
  observable from the harness (no audio device); disabling pauses AudioSystem's voices.
- **`move_branch` benchmark**: the 100-entity branch moves under a top-level GameObject about a
  thousand entities before it (a typical reparent); a move across the whole array is bounded by
  the Transform rotation itself, the same before and after S07.
- **R25 and R26 are measured on the benchmark-only server E** (4 dedicated EPYC-Milan vCPUs,
  nothing else running; the shared build servers ran at load 20-90 and gave 1.5-2x the times).
  `run-ecs-bench.py --baseline-archive <git archive oe-port engine>` (7 runs each), three times;
  every R25, R26 and R27 budget is met in the median of the three. Medians (ms; ns/op for
  lookup), 10k / 100k: create 1.43 / 14.7, destroy one by one 1.05 / 23.4, remove 10% 0.16 /
  3.55, add 0.25 / 2.51, index loop 0.025 / 0.41, view 0.035 / 0.60, lookup 6.0 / 11.5,
  move_branch 0.13 / 0.34. R26: destroy 10k 530x, metadata at 100k 14x, index loop 1.2x /
  1.2-1.3x faster than before, view 1.8x / 1.7-2.4x the baseline index loop. R27: 17.3 B per
  component and 76 B per entity at 100k. Destroy at 100k varies most between processes
  (21.5-27.7 ms across the 21 samples; the run medians were 23.4, 26.3 and 22.0): it is bound
  by cache misses on randomly ordered entities. What got it there: Vector3, Quaternion and
  Matrix4 construct and copy inline and trivially (a Transform move is a plain copy); a destroy
  allocates nothing (an intrusive entity free list, arena chunks reserving their page lists,
  operator-new pages kept until an array empties), because any malloc or large free in the
  middle of a mass destroy made glibc consolidate every small block the destroy had freed;
  a destroy or removal prefetches the metadata record, the arrays' slots and the heap blocks
  the components own (pointer-like words of a non-trivially destructible component's first
  line) before it frees anything; the index accessors have no call on their hot path, so a
  caller's loop keeps its sums in registers (R26's index loop).
- **AC10's ASan check** runs as an MCP test against a sample project,
  `tools/fixtures/ecs-collision` (`tools/mcp/tests/test_ecs_collision.py`): boxes fall on a floor
  every third frame, and each `beginContact2D` destroys the other body with
  `GameObject.destroyImmediate` and adds or removes a `Rigidbody2D` on the script's own
  GameObject. Run with an ASan/UBSan editor (`SANITIZE=address,undefined tools/build.sh
  linux`, `JASPER_EDITOR=build/linux/editor-address-undefined/jasperengine-editor`,
  and S02's runtime options as `tools/build.sh` passes them, leak detection included, plus
  `alloc_dealloc_mismatch=0`: ThorVG replaces `operator new` with `malloc` in the static
  engine) it plays 1000 frames with no console error and no sanitizer report, and the editor's
  stderr holds no report after it exits either (run on the S24 merge, server B). The first run stopped in UBSan on an unrelated read: `ShaderData` copied an
  indeterminate `es` flag; its scalar fields are now initialized. AC10 is an MCP test rather
  than a manual step, so it is repeatable; it is in the default suite (with a normal editor it
  checks the same behaviour without the sanitizers).
