# 02 — Core architecture: ECS, scene, memory, jobs, events, serialization, assets, FS, streaming, logging

OURS = JasperEngine (`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2`), C++17 default (`engine/CMakeLists.txt:3-5`, 20/23 optional).
THEIRS = OpenEngine (`external/OpenEngine`, MIT, (c) 2026 Gamecrafters Guild Limited), C++20 throughout (concepts, `consteval`, `std::span`, `std::format`, `std::atomic_ref`, `std::atomic::wait`).
All paths below for THEIRS are relative to `external/OpenEngine/Engine/` unless stated. Verified by reading code; no builds run.

**Big constraint up front:** THEIRS' ECS requires every component to be `std::is_trivially_copyable && std::is_standard_layout && !pointer` (`Modules/ECS/Include/ECS/ECS.h:55-57`); archetype moves are raw `memcpy` plans (`World.h` `CopyStep`). Strings in components are fixed `char[260]` buffers (`Include/Components/SceneBlueprintInstance.h`). OURS components hold `std::string`, `std::vector`, `std::shared_ptr`, Lua refs (`engine/core/component/MeshComponent.h`, `ScriptComponent.h`, etc.). Their ECS core therefore cannot be lifted; their *peripheral* modules (Memory, FileSystem, FileWatcher, Events, Scheduler, parts of JobSystem/AssetCore) can.

---

## 1. What THEIRS has

### ECS (`Modules/ECS`, ~14.9K lines; `Source/World.cpp` 3.1K)
- **Archetype storage, chunked SoA.** `ArchetypeTable.h`: 16 KB cache-line-aligned chunks (`kArchetypeChunkBytes`), 16-byte column alignment, entity-handle column first, columns sorted by type id; swap-and-pop removal (O(1)). `World.h` `Archetype`: per-archetype `shared_mutex`, cached add/remove **archetype graph edges** (`m_AddEdges/m_RemoveEdges`), a dedicated enable/disable edge (two tags in one move), lazily **pre-baked move plans** (`m_MovePlans`, memcpy steps), `CompactChunks`, `PruneEmptyArchetypes`.
- **Entity handles:** 20-bit index + 12-bit version in a `uint32` (`ECS.h:78-81, 93-123`) — generational, 1M entities.
- **Type ids:** `consteval` FNV/Hash64 of the normalized compiler type name (`ComponentTypeName.h`), identical across DLLs/compilers.
- **Queries** (`Query.h`, 1.3K lines): `Read<T>/Write<T>/Optional<T>` qualifiers (`ComponentConcepts.h`), `Each`, `BatchEach`, `Parallel(minBatch)`, `ParallelBatchEach`, `ForEachChunk`, `ParallelChunks`, `Adaptive`; disabled-entity/disabled-component exclusion by construction (`Components.h`: `Disabled`, `DisabledInHierarchy`, `ComponentDisabled<T>` tags). `CachedQuery.h` caches matching archetypes keyed on world structural version.
- **Change detection:** per-(chunk × column) `uint64` write-grant versions with a "stamping facade" that makes un-stamped mutable access fail to compile (`ArchetypeTable.h:285-352`, `World.h` private `GetTable()`); `Changed<T>` gate per (system,query) (`ChangeFilter.h`); per-frame `ComponentDirtyFeed.h`; `Added<T>/Removed<T>` lifecycle buffers (`LifecycleEvents.h`); component hooks (`ComponentHook.h`).
- **Deferred structural changes:** `CommandBuffer` with SPSC power-of-two ring and 64-byte SBO command data (`Entity.h:110-170`, `ECS.h:167+`).
- **Systems & scheduling:** `ISystem` + `SystemManager` with wave execution plan, every-N-frames throttling, per-system timing, slot replacement for hot reload (`Systems.h`); `ECS/Scheduler.h` greedy wave packer from Read/Write `DenseSignature`s + explicit `DependsOn` edges → conflict-free parallel waves on the job pool; `SystemWaveTrace.h` records waves for the editor/MCP.
- **Relations:** `RelationIndex.h` (target → sources cache, rebuilt on structural version bump).
- **Reflection:** `GE_REFLECT(Type, fields...)` + `GE_REFLECT_VERSION` → constexpr field tables (name, offset, size, append-only `FieldTypeId`, `FieldFlags` Hidden/ReadOnly/Transient, enum name tables emitted by a build-time ComponentScanner) (`Reflection.h`); `ReflectionJson.h` dump; `ComponentFieldRegistry`, `ComponentRegistry`, `ComponentFactory`, `BlobComponent` (type-erased components with no C++ type, for scripting).
- **Schema migration on hot reload:** `ComponentMigration.h` `BuildFieldRemap` (keep fields whose name+type+size match, default new ones) + `ComponentLayoutReloadMigrator.cpp` re-packs live instances and resizes columns.
- **Forward compatibility:** `UnresolvedComponentStore.h` keeps unknown components and un-representable field text verbatim so save re-emits them.

### Scene / hierarchy / spatial
- Hierarchy = `Parent{EntityHandle}` + `HierarchyOrder{int}` components (`Include/Components/Hierarchy.h`); children derived (RelationIndex or scan).
- `Source/Engine/Rendering/Systems/TransformHierarchySystem.cpp` (696 lines): flat parallel pass for roots, cached CSR topology rebuilt only when Parent columns/structure change (change-filter gated), dirty-subtree propagation on the job pool above 256 roots (`kParallelMinRoots`, `kMinRootsPerChunk=64`), `WorldTransform.Version` bump + dirty feed; profiler behind `GE_HIER_PROFILE`.
- `Source/ECS/DisabledInHierarchySystem.cpp`: derives `DisabledInHierarchy` tag from the parent chain.
- **Scene CPU BVH** `Modules/Scene` (`SceneTlas.h/.cpp`, `TlasBuilder.cpp`, `TlasRefit.cpp`, `TlasTraverse.cpp`; 2.6K lines of tests): per-entity world-AABB binned-SAH BVH, sectored (`WorldSectorCoord`), on-demand `RequestCurrent/WaitCurrent` (main thread snapshots, refit/rebuild on jobs), `TraverseRay/Sphere/Frustum`, parallel refit. Used for editor picking; tightly coupled to their `Transform`, `LocalBounds`, `MeshRenderer`, `RenderExtractionSystem`.
- `Modules/SceneBvh`: stackless threaded BLAS for GPU software ray traversal, **ported from speedball-gi (MIT, (c) 2026 m3org, norio)** (`ThreadedBvh.h` header) — render/GI domain.

### Scene serialization (`Source/Scene/SceneIO.cpp` 4.3K, `BuiltInSceneSchemas.cpp` 8.8K, `SceneSchemaRegistry`, `ReflectionSceneSchema.cpp`, `FieldSerializerRegistry.cpp`, `SceneValue.cpp`)
- Custom line-based text format: `[scene name=.. version=1]`, `[entity id="e_1"]`, `Component.field = value`, `[resource]`, `[embed]`, `[include]`, `[blueprint]` (prefab) and `[subscene]` sections, `;` comments (sample: `../Tools/Web/smoke-project/Assets/Scenes/WebSmoke.scene`). Diff/merge friendly.
- Error model: `SceneIOError{file,line,message,worldCleared}`, per-assignment `SceneLoadSkip` (entity, component, field, file, line, `preserved`) — the load continues past unknown fields/enumerators (`Include/Scene/SceneIO.h`).
- Blueprints: root-only overrides + `+Comp/-Comp` add/remove directives (`SceneIO.cpp:850-861`).

### Job system (`Modules/JobSystem`, 9.3K lines)
- `WorkStealingThreadPool`: global MPMC queue + per-worker local queues + steal ring + a **Background lane**, priorities (`TaskTypes.h JobPriority`), slab-allocated task envelopes (`TaskSlabPool`), `TaskHandle`, `JobCounter` (wait/join), `TaskDependencyGraph`, `JobChannel`, `BlockingThreads` (dedicated blocking-I/O threads), statistics. No fibers.
- `ParallelFor(pool, count, body, options)` (`WorkStealingThreadPool.h` doc, `ParallelForCore.cpp`): caller participates, helpers published as bare tasks that find the run closed if late, **legal from a worker thread** (no nested-wait deadlock), stop/exception propagation, no steady-state heap alloc; `ParallelAlgorithms.h` chunked variant + `ParallelSort`. Inline mode for single-threaded wasm.
- Deps: `concurrentqueue` (moodycamel; BSD-2 / Boost dual license), `Logger`, `Types`, `Platform/Thread.h`.

### Scheduler (`Modules/Scheduler`, ~330 lines)
- Tick-driven deferred work: `ScheduleNext/After/At`, cancel tokens, FIFO/time ordering, re-entrancy deferral, thread-safe scheduling, owner-thread `ProcessDue()`.

### Memory (`Modules/Memory`, 7.2K incl. tests/bench; depends only on `Types`)
- `Arena` (chained-block bump allocator, `Rewind/Reset`, `std::pmr` resource, ASan poisoning, serial-use assert), `ScratchScope` (per-thread nested scratch stack), `ObjectPool<T>` (fixed slots, intrusive LIFO freelist, live bits, double-delete asserts), `RangeAllocator` (two-level segregated-fit best-fit over offsets — TLSF-like — with `FreeAfter(frame)`/`ReclaimThrough` deferred frees for GPU buffers), `StatsRegistry`/`TrackedBytes` (named, per-residency memory stats), allocation counter/hook, mimalloc fallback overrides (`Source/MimallocOverrides.cpp`), testing helpers (`ScopedAllocationFault`).

### Events / Types
- `Modules/Events/Include/Events/Event.h` (180 lines, C++17-compatible): `Event<Args...>` with RAII, move-only `EventSubscription` (weak state, safe if event dies first).
- `Modules/Types`: `FlatMap`, `StringId/StringHash/Fnv1a`, `NearestName` (did-you-mean), strict `ParseNumber`, `FalseSharing` padding, `ScopedSubscription`; `unordered_dense` (MIT) aliases.
- `Modules/GenerationalVector`: generational slot container (used by Jolt backend & `MeshGPURegistry`).

### Assets
- **AssetCore** (`Modules/AssetCore`): 128-bit `GUID` (v4 random via thread-local `mt19937_64`; `GUID::Derive(parent, subKey)` FNV-based deterministic sub-asset ids, frozen contract — `Source/GUID.cpp:178+`), `AssetRegistry`, `AssetTypes` (`AssetLoadPriority Low..Critical`), `DepEdge`/`DepEdgeKind` (typed dependency edges with `FieldLocator` + ordinal for retargeting — `DepEdge.h`), `AssetIgnoreRules` (`.assetignore`), `PathNormalization` (case-fold, NFC via utf8proc), `SharedFileRead` (Windows share-delete reads so external safe-saves never fail), `AssetEvents`, `AssetReloadInvalidator` (RAII cache-eviction subscriber).
- **AssetDatabase** (`Modules/AssetDatabase`, 7.2K): authoritative git-friendly store `AssetStore_TextJsonl` (`{"format":"assetdb","version":2}` header, upsert/delete/redirect records, delta-append saves with compaction, sorted deterministic snapshot, case-folded path identity, cross-process write lock, parallel parse on job pool); `IAssetDbCache` derived cache (dep graph, fingerprints mtime/size/hash/file-id, KV mirror, provenance) — always deletable — SQLite impl only linked by editor/tools (`AssetDbCache_Sqlite.cpp` 1.7K); `RedirectChain` (GUID forwarding after re-identification); `AssetSourceSnapshot` (warm-start fingerprint snapshot, advisory); `AssetStoreReconciler` (prune ignored, legacy-path fixups, quarantine re-admission). **No per-file .meta sidecars.**
- **Runtime** (`Include/Assets`, `Source/Assets`, ~42K lines; `Modules/AssetRuntime` is just `#include` shims): `AssetManager` (`LoadAssetAsync` → `AssetFuture` with `OnComplete/OnError`, cancellable `AssetLoadHandle`, residency classes Pinned/Transient, `ReloadAssetNow/RequestAsyncReload`, `UnloadAssetWithDependencies`, multiple mounted sources with aliases), `AssetIOService` (dedicated blocking reader threads, priority claim, byte-range reads, continuation on Background jobs, inline mode on wasm), `AssetDecodeGate` (asset decode may occupy at most workers-2 so the frame keeps two), `FileWatchingService` (pattern subscriptions, RAII handles with wait-for-in-flight unsubscribe), `ExpectedWriteLedger` (suppresses watcher echoes of the process's own writes by path+size+mtime), `ParserRegistry`, importers for glTF/FBX/Blend, `TextureCook` (BC7/BCn cook with artifact cache keyed by recipe version + encoder versions + ISA), `MeshLODGenerator/Cache`, `HlodCache`, packages (`Source/Assets/Packages`).

### File system / watching
- `Modules/FileSystem`: `MakeTemporarySiblingPath` + `PublishFile` (atomic temp+rename with 5×200 ms retry, web fallback), `RenameWithRetry`, `ScopedFileLock` (cross-process), `TreeRemoval`, `OrphanedTemporaryFiles` cleanup, OPFS-safe web variants.
- `Modules/FileWatcher` (1.1K lines): Windows `ReadDirectoryChangesW`, macOS FSEvents-woken poll, polling elsewhere (no inotify), extension filters, excluded dirs, `FileIdentity` (dev+inode / volume+index).

### Streaming (`Modules/PageStreaming`, 4.5K)
- Terrain height-field page pyramid (`.gepage` store, 128-sample pages with apron), `PageResidency` budgets, `PageCache` with unload hysteresis, `ResidentSlotPool` (generational key→GPU-slot with frames-in-flight quarantine), async `PageLoader` via `AssetIOService`, cooker. Terrain-specific.

### Logger (`Modules/Logger`, 5.7K incl. tests)
- `std::format` API (`Trace..Critical`), async drain thread, sinks: Console (stderr split option), File (rotation), RingBuffer, Callback, Deferred; per-sink levels, source location, backtrace capture on Error+, `FlushForCrash(timeout)`, cross-DLL shared state (`RedirectToSharedState`).

---

## 2. What OURS has

### ECS (`engine/core/ecs`, 880 lines; `engine/core/registry`)
- `Entity = unsigned` (`ecs/Entity.h`), **no generation**. `EntityManager` (`ecs/EntityManager.h`): `std::map<Entity, EntityMetadata>` (name, signature, active, tag, layer) — every lookup O(log n); system ids 1..1000 reuse the first free slot, user ids 1001+ are monotonic and never reused (so ABA is practically limited to system entities).
- `Signature = std::bitset<64>` → hard cap 64 component types (`ecs/Signature.h`).
- `ComponentArray<T>` (`ecs/ComponentArray.h`): dense `std::vector<T>` + `unordered_map<Entity,size_t>` + `indexToEntity`; **removal is `vector::erase` + reindex (O(n))** to preserve order; `moveEntityRangeToIndex`, `sortByComponent`, `sortByEntityOrder` keep arrays in hierarchy order. Batch `entitiesDestroyed` compacts once. Components are arbitrary C++ types (non-POD OK).
- `ComponentManager` (`ecs/ComponentManager.h`): dense type index via `typeid(T).name()` registry shared across modules.
- `SubSystem` (`ecs/SubSystem.h`): virtual `load/update/fixedUpdate/draw/destroy`, `onComponentAdded/Removed`, `onEntityLayerChanged`. Systems are a fixed ordered list on `Scene` (`Scene.h:45`), single-threaded; no queries — systems iterate component arrays by index.
- `EntityRegistry` (`registry/EntityRegistry.h/.cpp`): hierarchy via `Transform::parent`, `addEntityChild`, `moveChildToIndex/Up/Down/Top/Bottom`, `findBranchLastIndex`, `sortComponentsByTransform`, active-in-hierarchy by walking parents, tags/layers (`TagManager`, `PhysicsLayers`), `EntityCloner` (736 lines).
- Transform (`component/Transform.h`): local + world pos/rot/scale + local/model/normal/MVP matrices, `needUpdate` flags; propagated in `RenderSystem::updateTransform` (`subsystem/RenderSystem.h:1958`) relying on parent-before-child array order.

### Scene/spatial
- `Scene` (`engine/core/Scene.h/.cpp`), `SceneManager`, `ObjectManager`, `PrefabManager/PrefabAsset` (`engine/core/manager`). No scene-level BVH: editor picking scans all entity AABBs (`editor/window/SceneWindow.cpp:192, 369`; `editor/Project.cpp:6168`).

### Threading
- `ThreadPoolManager` (`thread/ThreadPoolManager.h/.cpp`, 138 lines): singleton, one `std::queue<std::function>` under a mutex + condvar, `enqueue` → `std::future`. No priorities, no work stealing, no parallel-for, no cancellation. Nested waits can deadlock — `MeshSystem::parallelForIndexed` explicitly bypasses it with `std::async` threads for that reason (`subsystem/MeshSystem.cpp:1108-1140`).
- `ResourceProgress` (`thread/ResourceProgress.h`): progress bookkeeping for async builds. Async loads in `TextureDataPool` (futures map, `pool/TextureDataPool.cpp:16,119,152`), `SoundPool`, `MeshLodPool`, model parsing.

### Memory / buffers
- No allocators beyond STL; `util/HybridArray.h` (small-buffer array). `buffer/*` are GPU vertex/index buffer descriptions, not allocators.

### Events
- `util/FunctionSubscribe.h` (367 lines): `FunctionSubscribe<Ret(Args...)>` keyed by **string tag**, accepts C++ functions, member pointers and Lua functions, runs Lua callbacks under `CrashGuard` (`util/CrashGuard.h`, POSIX sigsetjmp / Windows SEH).

### Serialization
- YAML via yaml-cpp: `editor/Stream.cpp` (8.6K lines) + `Stream.h`, one hand-written `encodeXComponent/decodeXComponent` pair per component (`Stream.h:347-460`), `decode*` takes the old value for partial updates; finite-value guards (`decodeFinite`, `decodePositiveFinite`). Scene root keys: id, name, scene, sceneType, mainCamera, maxValues, shaderKeys (`Stream.cpp:2957-2979`) — **no format version**, legacy handling via ad-hoc branches (`Stream.cpp:1165, 1512`). Unknown components are not preserved.
- Scene save writes directly with `std::ofstream` (`editor/Project.cpp:5696-5703`) — **not atomic**; a crash mid-write truncates the scene. (Only the CLI uses a temp file: `editor/cli/CommandLine.cpp:1874`.)

### Assets
- `editor/util/AssetDatabase.h/.cpp` (823 lines): Unity-style GUID (32 hex, random; `legacyGuid` derived from path) written **inside the file** as a top-level `guid:` key; **only `.prefab` is tracked** (`isTrackedFile`). Path↔GUID maps, cache file, `refresh()` stat-scan diff (Added/Removed/Moved/Modified), duplicate-GUID resolution, copy/redo-stable GUIDs. Contract in `editor/project/PREFABS.md`.
- All other assets (textures, models, materials, scripts, shaders) are referenced **by path**; editor-side renames rewrite references (`editor/command/type/RenameFileCmd.cpp:47-63` → `remapMaterialFilePath`, `remapAssetFilePath`, …); external moves break references.
- No import settings / meta files, no dependency graph, no cooking cache (except shader variants & `io/ResourcePack` for exports: `resources.pak` obfuscated bundle).
- Prefab system (`editor/project/*`, ~13 files): instance overrides per entity, variants, nested prefabs, missing-prefab handling, replace, refs.

### File I/O / watching
- `io/File`, `io/Data` (asset://, lua:// virtual paths), `io/FileData`, `io/UserSettings`, `io/ResourcePack`.
- No file watcher: `editor/util/HtmlUIWatcher` polls every 0.25 s for HTML UI docs; material file links poll `last_write_time` (`editor/Project.cpp:1615-1640`); code editor polls its open files (`editor/window/CodeEditor.cpp:602`).

### Logging
- `engine/core/Log.h/.cpp`: printf-style `print/verbose/debug/warn/error` → `System::platformLog` (synchronous `printf`, platform overrides). Editor `Out` (`editor/Out.h/.cpp`) adds console window, log file with single `.1` rotation, colored types. No categories, runtime level filtering, source location, or async sink.

### Math
- Own scalar `engine/core/math/*` (6.4K lines; Vector2/3/4, Matrix3/4, Quaternion, AABB, OBB, Ray, Plane, Sphere, Rect). THEIRS `Modules/Mathematics` is a thin glm wrapper — not a gap.

---

## 3. Gaps — THEIRS has, OURS lacks

| # | Feature | Their location | Value | Effort |
|---|---|---|---|---|
| 1 | Atomic file publish (temp sibling + rename + retry), cross-process file lock, orphan temp cleanup | `Modules/FileSystem/{Include/FileSystem/FileSystem.h, RenameWithRetry.h, ScopedFileLock.h}`, `Source/FileSystem*.cpp, OrphanedTemporaryFiles.cpp` | H | S |
| 2 | Native file watcher (RDCW / FSEvents / poll) + subscription service + own-write echo suppression | `Modules/FileWatcher`, `Include/Assets/FileWatchingService.h`, `Include/Assets/ExpectedWriteLedger.h`, `Source/Assets/FileWatchingService.cpp` | H | S–M |
| 3 | External-change hot reload of textures/models/materials with cache invalidation | `AssetManager::RequestAsyncReload`, `Modules/AssetCore/Include/AssetCore/AssetReloadInvalidator.h` | H | M |
| 4 | GUID identity for **all** asset types (binary ones too) + redirects + deterministic sub-asset GUIDs | `Modules/AssetDatabase/AssetStore_TextJsonl.*`, `RedirectChain.*`, `Modules/AssetCore/Source/GUID.cpp` (`Derive`) | H | L |
| 5 | Typed asset dependency graph (referrer→target, kind, field locator) for reload propagation & "missing asset → retarget" | `Modules/AssetCore/Include/AssetCore/DepEdge.h`, `IAssetDbCache.h` | H | M (after #4) |
| 6 | Reflection field tables driving serialization / inspector / tooling | `Modules/ECS/Include/ECS/Reflection.h`, `ComponentFieldRegistry.*`, `Source/Scene/ReflectionSceneSchema.cpp` | H | L (non-POD adaptation) |
| 7 | Scene format version + forward compat (keep unknown components/fields verbatim, per-field load report with line numbers, did-you-mean) | `Modules/ECS/Include/ECS/UnresolvedComponentStore.h`, `Include/Scene/SceneIO.h` (`SceneLoadSkip`), `Modules/Types/Include/Types/NearestName.h` | H | M |
| 8 | Job system: work stealing, priorities/Background lane, task graph, counters, **ParallelFor safe from workers** | `Modules/JobSystem` | H | M (adapt ParallelFor) / L (full port) |
| 9 | Asset I/O service: dedicated blocking-read threads, priority, cancellation, byte ranges; decode gate reserving 2 workers for the frame | `Include/Assets/AssetIOService.h`, `AssetDecodeGate.h`, `Source/Assets/AssetIOService.cpp` | M | M |
| 10 | Allocators: arena, scratch scope (pmr), object pool, TLSF-like range allocator with frame-deferred free, memory stats registry | `Modules/Memory` | M | S–M |
| 11 | Scene-level CPU BVH (binned SAH, refit, ray/sphere/frustum) for picking, raycasts, audio/AI queries | `Modules/Scene/Source/{TlasBuilder,TlasRefit,TlasTraverse,SceneTlas}.cpp` | M | M |
| 12 | Parallel, change-gated transform hierarchy propagation (CSR cache, dirty subtrees) | `Source/Engine/Rendering/Systems/TransformHierarchySystem.cpp` | M | M |
| 13 | O(1) component add/remove, dense entity lookup (vs `std::map` + `vector::erase`) | `Modules/ECS/Include/ECS/ArchetypeTable.h`, `World.cpp` | M | M (in-place fix) / XL (archetypes) |
| 14 | Generational entity handles | `Modules/ECS/Include/ECS/ECS.h:78-123` | L–M | L (scenes store raw ids, Lua) |
| 15 | Query API (Read/Write/Optional, Changed<T>, chunk iteration, parallel) + per-chunk change versions | `Query.h`, `ChangeFilter.h`, `ArchetypeTable.h` | M | XL (needs #13 archetypes) |
| 16 | System scheduling by read/write sets into parallel waves; every-N-frames; per-system timing; wave trace | `Modules/ECS/Include/ECS/{Scheduler.h,Systems.h,SystemWaveTrace.h}` | M | L |
| 17 | Deferred CommandBuffer for structural changes during iteration | `Modules/ECS/Include/ECS/Entity.h:110+` | M | S–M |
| 18 | Component layout migration on C++ script hot reload | `Modules/ECS/Include/ECS/ComponentMigration.h`, `Source/ComponentLayoutReloadMigrator.cpp` | L–M | M |
| 19 | Logger: async drain, multi-sink (rotating file, ring buffer, callback), per-sink levels, source location, backtrace on error, crash flush | `Modules/Logger` | M | M (C++20 `std::format`) |
| 20 | RAII event subscriptions (no string tags) | `Modules/Events/Include/Events/Event.h` | L | S |
| 21 | Tick/time scheduler with cancel tokens | `Modules/Scheduler` | L–M | S |
| 22 | `.assetignore` rules; case-fold/NFC path identity; share-delete reads | `Modules/AssetCore/{AssetIgnoreRules,PathNormalization,SharedFileRead}.*` | L–M | S |
| 23 | Derived-artifact cache keyed by recipe + encoder versions (cook cache) | `Source/Assets/TextureCook.cpp:188-215`, `MeshLODCache.cpp` | M | M (render owner) |
| 24 | Page streaming / resident slot pool with frames-in-flight quarantine | `Modules/PageStreaming` (`ResidentSlotPool.h/.inl`) | L | M |
| 25 | Async load API with futures/callbacks/cancel + residency classes, multi-source mounts | `Include/Assets/AssetManager.h:94-260, 324-700` | M | L |

---

## 4. Things THEIRS does better (evidence) — and what OURS does better

**THEIRS better**
- **Safety of file writes.** `PublishFile` is the one path for every temp-then-replace (`FileSystem.h`); OURS overwrites scenes in place (`Project.cpp:5696`). THEIRS also tolerates external safe-saves on Windows (`SharedFileRead.h`) and suppresses its own watcher echoes (`ExpectedWriteLedger.h`).
- **Asset identity.** Every asset has a GUID in a deterministic, delta-appended text DB; renames outside the editor are survivable; sub-assets get stable derived GUIDs (`GUID::Derive`). OURS: only prefabs; models/textures/materials referenced by path.
- **Dependency tracking.** `DepEdge` with `FieldLocator` enables "who references X" and exact-field retargeting; OURS has no graph (rename fixups are per-type hand code in `RenameFileCmd.cpp`).
- **Change detection / reload.** Native watcher + subscriptions vs OURS' three separate pollers (HtmlUIWatcher, material links, code editor).
- **Threading.** Priorities, Background lane, nested-safe `ParallelFor`, slab tasks, `AssetDecodeGate` reserving workers for the frame. OURS' single FIFO queue already forced a std::async workaround (`MeshSystem.cpp:1111`), and background loads can starve each other.
- **ECS scalability.** O(1) swap-remove, archetype graph edges, cache-friendly SoA chunks, change versions, parallel queries; OURS removal is O(n) (`ComponentArray.h` `remove` → `erase` + `reindex`) and metadata is a `std::map`. OURS also caps at 64 component types (`Signature.h`).
- **Serialization robustness.** Versioned header, per-field skip report with file:line, verbatim preservation of unknown data; OURS has no version key and silently drops what it does not know.
- **Reflection-driven tooling.** One `GE_REFLECT` table feeds scene IO, inspector, MCP and hot-reload migration; OURS maintains ~8.6K lines of hand-written YAML codecs plus hand inspectors per component.
- **Test depth.** Dedicated test suites per module (e.g. `Modules/Scene/Tests/SceneTlasTests.cpp` 2.6K lines, `Modules/Memory/Tests/*`, `Modules/Logger/Tests/*`, `JobSystem/Tests/ParallelAlgorithmsTests.cpp`).

**OURS better / fairer picture**
- **Component ergonomics.** Components can hold `std::string`, vectors, shared pointers and Lua refs; THEIRS forces POD (`ECS.h:55`) and fixed `char[260]` strings (`SceneBlueprintInstance.h`), with a big machinery cost (blob components, field registries) to compensate.
- **Prefabs.** OURS has Unity-style per-entity overrides, variants, nested prefabs, missing-prefab recovery (`editor/project/ProjectPrefab*.cpp`, `PREFABS.md`); THEIRS blueprints have **root-only** overrides (`SceneIO.cpp:855`).
- **Deterministic hierarchy order for free.** OURS keeps component arrays in parent-before-child order (`sortComponentsByTransform`, `moveEntityRangeToIndex`), so transform propagation is a single linear pass with no topology cache.
- **GUID-in-file is merge-friendly.** OURS' `guid:` inside the prefab avoids a single shared DB file that every asset change touches (THEIRS' `.assetdb` JSONL is a merge hotspot, mitigated by delta-append + compaction).
- **Simplicity and portability.** C++17, ~900-line ECS, trivial to debug and to bind to Lua; builds for Web/Android/iOS with small footprint. THEIRS' core is roughly 50K lines of C++20 for the same layers, with comment-level design-doc references (C7, M14, R10…) that show high maintenance cost.
- **Lua callbacks under CrashGuard** (`FunctionSubscribe.h` + `CrashGuard.h`) — user script crashes don't kill the editor; THEIRS' `Event.h` has no such guard.
- **Unity-compatible GameObject semantics** (tags, layers 0..31, activeSelf/activeInHierarchy, physics layer matrix in `registry/PhysicsLayers.*`).

---

## 5. Copy / port candidates (ranked by value ÷ effort)

All THEIRS code is MIT, (c) 2026 Gamecrafters Guild Limited and contributors — keep the copyright + license text in each ported file (or a `NOTICE.md` entry, as already done for Doriax). C++20 → C++17 backports noted.

1. **Atomic publish + rename retry** — H/S
   - Source: `Modules/FileSystem/Include/FileSystem/FileSystem.h` (`MakeTemporarySiblingPath`, `PublishFile`), `Source/FileSystem.cpp`, `Source/FileSystemCore_Native.cpp`, `Source/RenameWithRetry.cpp`, `Source/OrphanedTemporaryFiles.cpp`; optionally `ScopedFileLock_Native.cpp`.
   - Drags: `Logger` → replace with `Log`/`Out`. Web variant (`FileSystemCore_Web.cpp`) is OPFS-specific; we don't edit on web, so skip it.
   - Use in: `Project::saveSceneFile` (`editor/Project.cpp:5696`), project file (`:5193`), prefab saves, `AssetDatabase::saveCache`, `UserSettings`.
   - Difficulty: trivial, C++17-clean.

2. **FileWatcher + ExpectedWriteLedger (+ SharedFileRead on Windows)** — H/S–M
   - Source: `Modules/FileWatcher/{Include/FileWatcher/FileWatcher.h, FileIdentity.h, Source/FileWatcher.cpp, FileIdentity.cpp}`; `Include/Assets/ExpectedWriteLedger.h` + `Source/Assets/ExpectedWriteLedger.cpp`; `Modules/AssetCore/{Include,Source}/SharedFileRead.*`.
   - Drags: `Logger`, `Types` (`Function`, `String` aliases → `std::function`, `std::string`), CoreServices on macOS. Linux is polling only — add an inotify backend (≈200 lines) for parity.
   - Use: feed `AssetDatabase::refresh`, replace `HtmlUIWatcher` and material-link polls, then add texture/model reload (gap #3) in `TextureDataPool`/`ModelPool`.
   - Difficulty: low; check for C++20 (`<span>` etc.) when compiling under 17.

3. **Memory module subset: `RangeAllocator`, `Arena`, `ScratchScope`, `ObjectPool`, `StatsRegistry`** — M/S–M
   - Source: `Modules/Memory/Include/Memory/{RangeAllocator,Arena,ScratchScope,ObjectPool,StatsRecord,StatsKind,StatsSnapshot,TrackedBytes,SerialUseCheck,AddressSanitizer,Residency}.h`, `Source/{RangeAllocator,Arena,ScratchScope,ObjectPool,StatsRegistry,TrackedBytes}.cpp`; tests under `Modules/Memory/Tests/` are worth porting with them.
   - Drags: `Types` only. C++20 bits: `std::span` (Arena, ScratchScope), `<bit>` (RangeAllocator, ObjectPool) — backport with a small span shim or move the engine to C++20 (already an option in `engine/CMakeLists.txt`). `std::pmr` is C++17 (Apple's libc++ lacked `memory_resource` before macOS 14 / iOS 17 — check the deployment targets).
   - Use: `RangeAllocator` for sub-allocating instance/UI/particle GPU buffers with `FreeAfter(frame + framesInFlight)`; `ScratchScope` for per-frame temporary vectors in `RenderSystem`/`MeshSystem`; stats into the profiler/MCP.
   - Skip `MimallocOverrides.cpp` / allocation hook.

4. **Nested-safe `ParallelFor` + priority lanes on our pool** — H/M
   - Option A (recommended): re-implement the `ParallelFor` contract (caller participates; helpers are plain queued tasks that exit if the run is closed; caller waits only on in-flight units; exception propagation) on top of `ThreadPoolManager`, plus a second Background queue. Reference: `Modules/JobSystem/Include/JobSystem/WorkStealingThreadPool.h` (ParallelFor doc block, ~lines 40-95), `Source/ParallelForCore.cpp` (505 lines), `Include/JobSystem/ParallelForOptions.h`, `ParallelAlgorithms.h`.
   - Option B: port the whole `Modules/JobSystem` (9.3K lines) — drags `concurrentqueue` (moodycamel, **BSD-2-Clause or Boost**, compatible but add to third-party notices), `Logger`, `Types/FalseSharing.h`, `Platform/Thread.h`, C++20 `std::atomic::wait`. L effort.
   - Removes `MeshSystem::parallelForIndexed`'s std::async workaround (`MeshSystem.cpp:1111`) and lets texture/model decode run at Background priority.

5. **Scene forward-compat & diagnostics (design port + `NearestName.h`)** — H/M
   - Source to read: `Modules/ECS/Include/ECS/UnresolvedComponentStore.h` (+ `.cpp`), `Include/Scene/SceneIO.h` (`SceneIOError`, `SceneLoadSkip`, `SkipIsOutstanding`); lift verbatim `Modules/Types/Include/Types/NearestName.h` (81 lines, did-you-mean).
   - Adaptation in OURS: add `formatVersion` to the scene/prefab YAML root + an ordered migration-step table in `Stream`; in `decodeComponents` keep unknown component YAML nodes on a per-entity side table and re-emit them in `encodeComponents`; collect per-field skip records (file, YAML mark line) and show them in Console/MCP. Their code is tied to their INI-style parser; the design is what transfers.

6. **Asset identity for all assets: JSONL store + `GUID::Derive` + `DepEdge`** — H/L
   - Source: `Modules/AssetDatabase/{Include/AssetDatabase/IAssetStore.h, AssetRecord.h, AssetStore_TextJsonl.h, RedirectChain.h; Source/AssetStore_TextJsonl.cpp (1.5K), RedirectChain.cpp}`, `Modules/AssetCore/{GUID.h, Source/GUID.cpp, DepEdge.h, Source/DepEdge.cpp, AssetIgnoreRules.*, PathNormalization.*}`.
   - Drags: `nlohmann_json` (MIT, header-only; OURS has yaml-cpp only), `utf8proc` (MIT) for PathNormalization, `JobSystem` (optional parallel parse — stub out), `Types`.
   - Alternative that matches OURS' existing in-file `guid:` design: keep in-file GUIDs for YAML assets (prefab, scene, material) and add `.meta` sidecars or this JSONL store only for binary assets (png/glb/ogg). Decide before porting; the JSONL store is the better fit if we want a single source of truth without sidecar litter.
   - Then put a lightweight dependency index (a JSON cache rebuilt from scene/prefab/material scans) on the `DepEdge` struct; skip SQLite.

7. **Scene CPU BVH (picking/raycast/overlap)** — M/M
   - Source: `Modules/Scene/Source/TlasBuilder.cpp` (binned SAH, 232 lines), `TlasRefit.cpp`, `TlasTraverse.cpp`, `TlasInternal.h`; tests `Modules/Scene/Tests/SceneTlasTests.cpp` for behavior.
   - Drags: their ECS (`World`, `ChangeFilter`), `Components/Transform`, `LocalBounds`, `MeshRenderer`, `RenderExtractionSystem`, `WorldSectorCoord` — strip all of it; keep build/refit/traverse over `{AABB, Entity}` leaves and feed from our `Transform::needUpdate` + mesh AABBs. Replaces the linear AABB scan in `SceneWindow::selectObjectByRay` and gives Lua `Scene:raycast` without physics bodies.
   - Note: `Modules/SceneBvh` (the GPU BLAS) carries an additional upstream MIT notice (speedball-gi, (c) 2026 m3org, norio) — keep it if that code is ever taken.

8. **`Event.h` RAII subscriptions** — L/S. `Modules/Events/Include/Events/Event.h` (180 lines, header-only, C++17-OK, no deps). Use for C++-internal engine/editor signals; keep `FunctionSubscribe` for Lua-facing events.

9. **`Scheduler` (tick/time deferred work)** — L–M/S. `Modules/Scheduler/{Include/Scheduler/Scheduler.h, Source/Scheduler.cpp}` (~330 lines, no deps beyond std). Useful for editor deferred actions and Lua `setTimeout`-style helpers.

10. **Logger sinks (selective)** — M/M. Lift the ideas/structure of `Modules/Logger/{RingBufferSink,FileSink,CallbackSink}` behind OURS' existing printf API (keep `va_list`, avoid `std::format`); add categories + runtime level, source-location macros, Error+ backtrace (`Source/Backtrace.cpp`, check platform deps) and `FlushForCrash` tied to `CrashGuard`.

Smaller in-place ideas (no copying): swap-and-pop removal in `ComponentArray` with a separate hierarchy order index; replace `EntityManager`'s `std::map` with a dense vector indexed by entity (ids are already small, mostly contiguous integers); widen `Signature` beyond 64; a deferred structural CommandBuffer for Lua scripts that destroy/add during iteration; an `AssetDecodeGate`-style cap so background decode never takes all cores.

---

## 6. Not worth copying (and why)

- **ECS core (`World`, `Archetype`, `ArchetypeTable`, `Query`, `CachedQuery`, change filter, dirty feed, lifecycle events)** — POD-only components (`ECS.h:55`), raw memcpy moves, `consteval` type naming, 15K lines of C++20 interlocked with their DLL-image and managed-C# design (`ChangeFilter.h` header: "dual-image hazard"). Adopting it would mean rewriting every OURS component, `Stream.cpp`, all Lua bindings and the editor. Take the ideas (#13 in-place fixes), not the code.
- **`ComponentLayoutReloadMigrator` / `BlobComponent` / `ComponentFactory`** — only meaningful with byte-layout POD components and their C#/native hot-reload ABI.
- **`AssetDbCache_Sqlite` (1.7K) + `AssetStoreReconciler` (1.6K)** — SQL dependency and fixups for their own legacy layouts (`NormalizeLegacyStorePathsIfNeeded`, quarantine re-admission); OURS' project sizes don't need a SQL cache.
- **Their scene text format / `SceneIO.cpp` / `BuiltInSceneSchemas.cpp` (13K)** — switching formats would break every existing JasperEngine project; YAML + versioning (#5) gets the robustness.
- **`Engine/Source/Jobs/*`** (CompileServerClient, HotReloadPipeline, PipeTransport, IncrementalCompilationTask) — .NET/C# compile-server specific (scripting analyst's domain).
- **`Modules/PageStreaming`** — terrain height-page pyramid tied to their CBT terrain and `AssetIOService`; OURS' clipmap terrain would need its own design. `ResidentSlotPool` is the only generic piece (L value).
- **`MimallocOverrides.cpp` / allocation hook** — global `operator new` replacement is risky across our Web/Android/iOS targets; link mimalloc directly if ever wanted.
- **`GenerationalVector`** — small, but OURS has no current consumer, and user entity ids are already never reused.
- **`Modules/Mathematics`** — a glm wrapper; OURS has its own complete math library.
- **`SystemWaveTrace`, `QueryAccessProbe`, `SwapGenerationGuard`** — diagnostics for their parallel-wave scheduler; irrelevant without it.
- **Asset runtime giant files (`Source/Assets/AssetRegistry.cpp` 8.5K, `AssetManager.cpp` 3.3K)** — too entangled (multi-source mounts, packages, HLOD, residency classes) to lift; port the narrow pieces in §5 instead.
