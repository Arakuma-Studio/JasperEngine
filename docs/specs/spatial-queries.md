# Spatial Queries: overlaps, shape casts, trigger filtering and non-allocating forms for 3D, 2D and drawn objects

Spec-first. Builds on the shipped physics backends (Jolt 5.2 in `engine/libs/joltphysics`, Box2D 3.1 in
`engine/libs/box2d`), S26 (SceneBvh, MeshRaycast) and S15 (monitors). Paths are relative to the repository
root unless they start with `craftsman-end-motor/`, which means
`/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor/`.

## 1. Goal

A script that asks "what is near me?" (an enemy looking for targets, a pickup magnet, an explosion, an
interaction prompt, a trigger zone written in code) can today only cast rays or loop over every object in
Lua. This spec gives JasperEngine Unity's spatial query set: `Physics.overlapSphere` / `overlapBox` /
`overlapCapsule` / `checkSphere`, `sphereCast` / `boxCast` / `capsuleCast`, the `Physics2D` equivalents,
`QueryTriggerInteraction` and `queriesHitTriggers`, non-allocating forms that write into a caller's table,
and a bounds overlap for drawn objects without colliders. Queries run on the physics engines' own
broad phases, return results in a deterministic order, cost nothing when unused, and an agent can check any
query against a brute-force sweep (`verify`) and see recent queries drawn in the views.

## 2. Scope

**In scope**
- 3D (Jolt): overlap sphere, box (oriented), capsule (two points + radius); check (any hit) of each;
  sphere, box and capsule casts, closest and all hits; raycast gains trigger filtering.
- 2D (Box2D): overlap point, circle, box (size + angle), capsule (size + direction + angle), area (two
  corners); first, all and non-allocating forms; circle, box and capsule casts, closest and all.
- `QueryTriggerInteraction` (UseGlobal, Ignore, Collide) on every query, and the project-wide
  `queriesHitTriggers` per backend (default true, today's raycast behaviour).
- Non-allocating forms (`...NonAlloc`) of the overlaps in Lua and caller-buffer forms in C++.
- Drawn-object overlaps without physics: `MeshRaycast.overlapSphere` / `overlapBox` (+ NonAlloc) on the
  scene's SceneBvh, bounds-exact (leaf bounds against the query shape).
- A brute-force reference (`PhysicsQueryReference`) used by `verify` (MCP) and by the tests.
- Query debug draw (`Physics.debugDrawQueries`, Game view toolbar, `configure_game_view`).
- One new MCP tool, `query_physics`, in the `physics` group; monitors; docs, guide, stubs, eval cases.

**Out of scope (default decisions)**
- `ComputePenetration`, `ClosestPoint`, `Physics.Simulate`, `queriesHitBackfaces` for physics,
  `Physics2D.queriesStartInColliders`, `ContactFilter2D`'s depth and normal-angle filters: later work.
- Non-allocating forms of the casts and of raycast (`raycastNonAlloc`): the closest-hit forms already
  return one value; the `...All` forms allocate (documented). A NonAlloc cast is a follow-up if a
  profile asks for it.
- A spatial hash or second index for Transform-only GameObjects (no renderer, no collider): give them a
  trigger collider (D7 explains why no new index).
- Triangle-exact drawn-object overlaps: `MeshRaycast.raycast` stays the exact tool; overlaps test bounds.
- Queries inside a physics step (contact callbacks): refused with one error per call site, as Jolt holds its
  locks then (`PhysicsSystem.h:577-580`).
- Edit-mode physics queries: the editor scene has no bodies until it runs (D10).
- Nothing in this spec is GPU work; the do-not-copy list (GPU-driven culling) is not touched.

## 3. Current state

All read from the code (not run) unless marked inferred:

- **Lua `Physics` has rays only.** The `Physics` class registers the layer matrix functions,
  `ignoreCollision`, `debugDraw`, and `raycast` / `raycastAll` only (`CoreClassesLua.cpp:2091-2111`);
  `Physics2D` the same (`:2113-2127`). Arguments are `(scene, origin, direction, maxDistance [, layerMask])`
  or `(scene, ray [, layerMask])` (`luaRaycast`, `CoreClassesLua.cpp:888-951`); masks are read as Unity's
  int, low 32 bits (`luaOptLayerMask`, `:700-705`).
- **C++ `Physics` / `Physics2D`** are static classes in `registry/PhysicsLayers.h:71-132` with
  `raycast` and `raycastAll` (3D under `JASPERENGINE_PHYSICS_3D`, 2D under `JASPERENGINE_PHYSICS_2D`).
  They forward to `Ray::cast` (`PhysicsLayers.cpp:238-301`), which runs Jolt's `NarrowPhaseQuery::CastRay`
  with `JoltQueryObjectLayerFilter` (layer, category, mask) and `OnlyStaticBodyFilter`
  (`Ray.cpp:631-687`), or Box2D's `b2World_CastRay` (`Ray.cpp:577-628`). `raycastAll` returns a
  `std::vector<RayReturn>` per call (`PhysicsLayers.cpp:252-256`).
- **No trigger filtering.** Neither `OnlyStaticBodyFilter` (`JoltPhysicsAux.h:377-407`) nor
  `Box2DAux::CastCallback` (`Box2DAux.h:149-178`) looks at sensors, so rays hit triggers today (inferred:
  Jolt's and Box2D's query paths report sensors unless a filter rejects them; `world.c:1920-1940` has no
  sensor test). There is no `queriesHitTriggers` setting. In 3D the trigger colliders of a body that also
  has solid ones live on a separate sensor body (`PhysicsSystem.h:155-157`), so "trigger" is a per-body
  property in Jolt; in Box2D it is per shape.
- **Overlap and cast exist only internally.** `CharacterController2DSystem.cpp:172` calls
  `b2World_OverlapCapsule` and `:239` `b2World_CastCapsule` for the 2D controller; 
  `PhysicsSystem::getContactSummary3D` runs `NarrowPhaseQuery::CollideShape` with an
  `AllHitCollisionCollector` (`PhysicsSystemContacts.cpp:806-851`). None is reachable from Lua or MCP.
- **The bounds path of `Ray::cast` is a linear scan** over every `MeshComponent` (`Ray.cpp:547-574`).
- **SceneBvh** (S26) holds the pickable objects of a scene (meshes, instances one leaf each, terrains, UI,
  points, lines, particles, 2D occluders: `SceneBvh.cpp:680-722`), is created on the first query
  (`Scene.h:229-235`) and serves the editor's picking and the runtime `MeshRaycast`
  (`MeshRaycast.h:77-108`): the analyst note "only for picking" is wrong, it already runs in games. It has
  ray and frustum queries but no box or sphere overlap (`SceneBvh.h:121-131`). Its `update` allocates a
  `std::vector` per query that refits (`SceneBvh.cpp:903-905`), scans every Transform's change tick whenever
  any Transform changed since the previous query (`:880-892`), and `frustum` builds an
  `std::unordered_set` per call (`:1113-1125`).
- **When bodies move.** A Transform edit reaches its Jolt body in the fixed step
  (`PhysicsSystem::updateRigidbodyPosition`, `PhysicsSystem.cpp:1123-1245`), so a query sees the world of
  the last step (inferred; Unity's `autoSyncTransforms = false` behaviour).
- **Threads.** Ray casts are thread-safe outside the step; particles cast rays from jobs
  (`PhysicsRayThreadsTest.cpp:4-7`).
- **Lifetimes.** The Jolt world is created on first need (`PhysicsSystem.h:148-150`); bodies are made
  when the scene runs (`PhysicsSystem::load`, `PhysicsSystem.cpp:4125-4171`), not in edit mode (inferred).
- **Character controllers** are seen by queries through their kinematic inner body when Detect Collisions
  is on (`CharacterControllerComponent.h:37`, `CharacterControllerSystem.cpp:201-204`).
- **Debug draw.** `Physics.debugDraw` fills a LineRenderer on a system entity of each scene
  (`PhysicsSystemDebugDraw.cpp:4-6`); the Game view toggle and `configure_game_view` `physics_debug`,
  `physics_debug_contacts` drive it (`docs/mcp/TOOLS.md`, `configure_game_view`).
- **MCP.** No tool queries physics. `raycast_meshes` is MeshRaycast's ray (`docs/mcp/TOOLS.md`,
  `raycast_meshes`); the `physics` group holds two tools, `create_ragdoll` and `edit_physics_material`
  (`docs/mcp/conventions.md`, Tool groups). Agents write Lua through `eval_lua` to test a physics query.
- **Docs.** `docs/mcp/guide/physics.md:489-510` documents rays only; `docs/mcp/guide/lua-api.md:1309-1328`
  lists the `Physics` / `Physics2D` statics.
- **Settings.** The 3D world capacity lives in project.yaml `physics3DWorld` (`Stream.cpp:3010-3018`) and
  the Layer Collision Matrices reach prebuilt players as `collision3D` / `collision2D` in game.json
  (`PlayerConfig.h:128-129`).

## 4. Reference

| Source | Take | Note |
|---|---|---|
| Unity Scripting API (docs only): `Physics.OverlapSphere`, `OverlapSphereNonAlloc`, `OverlapBox`, `OverlapCapsule`, `CheckSphere`, `CheckBox`, `CheckCapsule`, `SphereCast`, `BoxCast`, `CapsuleCast` (+`All`), `QueryTriggerInteraction`, `Physics.queriesHitTriggers`; `Physics2D.OverlapPoint`, `OverlapCircle`, `OverlapBox`, `OverlapCapsule`, `OverlapArea` (+`All`, `NonAlloc`), `CircleCast`, `BoxCast`, `CapsuleCast`, `CapsuleDirection2D` | Names, argument order, meanings (2D `OverlapX` returns the first hit, 3D `OverlapX` returns all) | Names only. Lua names are camelCase and take the scene first, as `Physics.raycast` does |
| Jolt `NarrowPhaseQuery::CollideShape`, `CastShape`, `RefTarget::SetEmbedded`, `CollideShapeCollector`, `CastShapeCollector` (MIT, vendored) | Shape queries with stack-embedded `SphereShape` / `BoxShape` / `CapsuleShape` and collectors that write into a caller buffer | No Jolt patch |
| Box2D `b2World_OverlapPoint/Circle/Capsule/Polygon`, `b2World_CastCircle/Capsule/Polygon`, `b2MakeOffsetBox` (MIT, vendored) | 2D overlaps and casts | No patch; the README.jasper.md ledger is unchanged |
| `craftsman-end-motor/src/level.cpp` `forEachEntityByPos`, `craftsman-end-motor/src/entity_index_test.cpp`, `craftsman-end-motor/src/hopperperf_test.cpp`, commit `3a859586` | The pattern: AI asked "who is near" by scanning a whole ring of chunks; the fix collects only inside the query box (the scan had been ~7 % of a server tick), and a verify mode compared every indexed lookup with the old global sweep (56,656 lookups, 0 differences) before trusting it | **Ideas only.** No code, data, assets or Minecraft-specific mechanics (chunks, mobs, blocks, ticks) may be copied; the engine feature is the generic overlap query plus the generic verify |

## 5. Requirements

### Functional: 3D queries

- **R1 Overlaps.** `Physics::overlapSphere(scene, center, radius, out, layerMask, qti)`,
  `overlapBox(scene, center, halfExtents, rotation, out, ...)`, `overlapCapsule(scene, point0, point1,
  radius, out, ...)` fill `out` with every collider whose shape intersects the query shape (touching counts:
  separation <= 0) and whose body passes the filters (R5). One `OverlapHit {Entity body; uint32_t
  colliderIndex; bool trigger}` per collider. Return the count.
- **R2 Checks.** `checkSphere`, `checkBox`, `checkCapsule`: true when R1 would return at least one hit;
  stop at the first hit.
- **R3 Casts.** `sphereCast(scene, origin, radius, direction, maxDistance, layerMask, qti)` → `RayReturn`
  (the closest hit: `distance` in world units along the normalized direction, `point` the contact point,
  `normal` the surface normal facing the cast, `body`, `colliderIndex`); `boxCast(center, halfExtents,
  direction, maxDistance, rotation, ...)`, `capsuleCast(point0, point1, radius, direction, maxDistance, ...)`;
  `...All` return every hit, nearest first, one per collider. Colliders the shape overlaps at the start are
  not reported (Unity). Errors as `raycast` for a zero direction or a non-positive distance
  (`PhysicsLayers.cpp:213-226`).
- **R4 Raycast filtering.** `raycast` and `raycastAll` take an optional `qti` (default UseGlobal). Their
  results are unchanged when it is UseGlobal and `queriesHitTriggers` is true.
- **R5 Filters.** Every query applies, in order: the scene's 3D world exists (no world: zero hits, no
  creation, as `Ray::cast` already behaves, `Ray.cpp:635-637`); layer mask, category and mask bits through
  `JoltQueryObjectLayerFilter`; trigger filter: a sensor body passes when qti is Collide, or UseGlobal with
  `Physics::queriesHitTriggers` true; an optional ignore list of entities (C++ and Lua, as
  `OnlyStaticBodyFilter`). Default mask: `DEFAULT_RAYCAST_LAYERS` (every layer but Ignore Raycast).
- **R6 Order.** Overlap results are sorted by `(body, colliderIndex)` ascending; cast `All` results by
  distance, then `(body, colliderIndex)`. Same world, same query → same output on one platform, whatever
  the broad phase's insertion history.
- **R7 Shapes.** Radii and extents must be finite and > 0 (capsule: radius > 0, the points may coincide:
  a sphere); a rotation is normalized; a bad argument logs one error naming it and returns 0 / false /
  no hit.
- **R8 What is found.** Bodies of Rigidbodies (static, kinematic, dynamic, sleeping), trigger companions,
  Character Controller inner bodies, Terrain and Terrain World colliders, ragdoll parts. Mesh colliders
  are surfaces: a sphere wholly inside a closed mesh collider does not overlap it (documented, as Unity).

### Functional: 2D queries

- **R9 Overlaps.** `Physics2D::overlapPoint(scene, point, ...)`, `overlapCircle(scene, center, radius,
  ...)`, `overlapBox(scene, center, size, angleDegrees, ...)`, `overlapCapsule(scene, center, size,
  CapsuleDirection2D, angleDegrees, ...)`, `overlapArea(scene, pointA, pointB, ...)`, each in three forms:
  first (`OverlapHit` with `hit` false when none; the first in R6's order), All (vector) and caller buffer.
  Positions and sizes in scene units (points), as `Physics2D::raycast`.
- **R10 Casts.** `circleCast(scene, origin, radius, direction, distance, ...)`, `boxCast(scene, origin,
  size, angleDegrees, direction, distance, ...)`, `capsuleCast(scene, origin, size, CapsuleDirection2D,
  angleDegrees, direction, distance, ...)` and `...All`, returning `RayReturn` (z = 0).
- **R11 Filters.** Layer through `Box2DLayers::makeQueryFilter`, the shape's category against the mask as
  `CastCallback` does, trigger filter per shape (`b2Shape_IsSensor`) with `Physics2D::queriesHitTriggers`,
  ignore list. Order as R6.

### Functional: drawn objects

- **R12 Bounds overlaps.** `MeshRaycast::overlapSphere(scene, center, radius, out, layerMask, options)`
  and `overlapBox(scene, center, halfExtents, rotation, out, layerMask, options)` return every SceneBvh
  leaf whose world bounds intersect the shape (sphere vs AABB exact; oriented box vs AABB by the separating
  axis test), as `MeshOverlapHit {Entity entity; PickKind kind; int32_t instance; uint64_t foliageId;
  int32_t prototype}`. Options are `MeshRaycastOptions` (kinds, includeInactive, ignore); default kinds
  `MESH | INSTANCE | BOUNDS | TREE` (terrains and UI rects are left out: their bounds are the whole map or a
  screen rect). Order: `(entity, instance)`.
- **R13 Frame snapshot.** Overlaps see leaf bounds as of the first overlap query of the engine frame (or a
  later SceneBvh rebuild): the SceneBvh refits for overlaps at most once per frame. `options.current =
  true` refits now (ray casts keep S26's per-query refit). Documented beside Unity's
  `autoSyncTransforms = false`.

### Functional: trigger setting, debug draw, reference

- **R14 `queriesHitTriggers`.** `Physics::setQueriesHitTriggers(bool)` / `Physics2D::...`, default true,
  process-wide like the layer matrix, set from Project Settings at Play start and in players (R36).
  It changes what UseGlobal means for every query including `raycast`.
- **R15 Debug draw.** `Physics::setDebugDrawQueries(bool)` (Lua `Physics.debugDrawQueries`, not saved)
  records the queries of the main thread in a ring of the last 256 per scene: shape, cast sweep, and hit
  points; drawn for 1 s of unscaled time through the scene's physics debug LineRenderer, green with hits,
  red without, hits as crosses; 2D queries at z = 0. Off: no recording, one relaxed atomic load per query.
  Queries from jobs are never recorded (documented).
- **R16 Reference.** `PhysicsQueryReference` (`engine/core/physics/PhysicsQueryReference.{h,cpp}`) answers
  the same overlap by testing every body (3D: each body's `TransformedShape::CollideShape` against the
  query shape; 2D: each shape with Box2D's shape distance) with the same filters, in R6's order, and
  `compare(a, b)` lists missing and extra hits. Used by `query_physics {verify}` and the tests, never by
  the runtime queries.

### Platform & backends

- **R17** CPU only; every export target; single-threaded web unchanged. 3D queries exist under
  `JASPERENGINE_PHYSICS_3D`, 2D under `JASPERENGINE_PHYSICS_2D`; a disabled backend leaves its Lua
  functions unregistered, as `raycast` today (`CoreClassesLua.cpp:2106-2109`).
- **R18 Threads.** The C++ forms with caller buffers are safe from jobs outside the step, like `raycast`
  (they share no mutable state; the trigger setting is an atomic). Lua forms are main-thread. Inside a
  step every form logs one error per call site and returns nothing.

### Performance (release; `PhysicsQueryBench`; scene `query_city`: 2,000 bodies (boxes, capsules, spheres; a
quarter triggers; half asleep) over a 200 m × 200 m static mesh ground; 2D: the same layout in Box2D;
desktop / mobile / web as S19 §5; p95 per query)

- **R19 Budgets.**

  | Query | Budget (desktop / mobile / web) |
  |---|---|
  | `overlapSphere` r = 5 m (about 10 hits), C++ caller buffer | 0.006 / 0.02 / 0.025 ms |
  | `overlapBox` 5 m half extents, rotated | 0.008 / 0.025 / 0.03 ms |
  | `checkSphere` r = 5 m | 0.003 / 0.01 / 0.012 ms |
  | `sphereCast` r = 0.5 m over 30 m | 0.012 / 0.04 / 0.05 ms |
  | Lua `Physics.overlapSphereNonAlloc` r = 5 m (binding included) | 0.01 / 0.03 / 0.04 ms |
  | 2D `overlapCircle` r = 100 points, caller buffer | 0.004 / 0.012 / 0.015 ms |
  | `MeshRaycast.overlapSphere`, 10,000 leaves, no refit due | 0.01 / 0.03 / 0.04 ms |
  | `raycast` (unchanged path plus the trigger test) | within 5 % of today's `PickingBench` / ray numbers |

  For scale (inferred from LuaBindingBench's per-call cost): a Lua loop over 2,000 objects' positions costs
  about 0.1-0.2 ms desktop per query, which R19 replaces.
- **R20 Allocations.** After warm-up, C++ overlaps and checks with a caller buffer of sufficient capacity,
  and the casts' closest-hit forms, allocate nothing (`AllocationCountScope` = 0, both backends). Lua
  `...NonAlloc` forms allocate nothing on the C++ heap and leave the Lua GC count unchanged when the
  results table already has room. `MeshRaycast` overlaps allocate nothing when no SceneBvh rebuild is due
  (R13 and D7 move SceneBvh's per-query scratch into members).
- **R21 Zero cost.** A scene that never queries pays nothing; with debug draw off a query pays one atomic
  load for it; no per-frame work is added anywhere.

### Editor UX (human)

- **R22 Project Settings > Physics** gains **Queries Hit Triggers** (3D) and **Physics 2D > Queries Hit
  Triggers**, checkboxes, default on, undoable as the other rows (S24), with tooltips naming
  QueryTriggerInteraction.
- **R23 Game view toolbar > Physics** menu (beside the existing Physics debug toggle and Contacts) gains
  **Queries**, drawing R15 while playing; the Scene view shows the same lines (the LineRenderer is in the
  scene). Saved with the other Game view toggles in the editor preferences.
- **R24 Stats.** The Game view Stats panel shows `Physics queries: N (M hits)` for the last frame when any
  ran (counts from R27's monitors).

### Agent UX (MCP, Lua, CLI)

- **R25 `query_physics`** (new, group `physics`, read-only, idempotent, allowed while playing):
  `op` `overlap` | `check` | `cast` | `raycast` (default `overlap`); `world` `3d` | `2d` | `drawn` (default:
  the scene's 2D/3D type); `shape` `{type: sphere|box|capsule|circle|point|area, radius, half_extents,
  size, point1, angle, rotation, direction2d}`; `origin`, `direction`, `max_distance`; `layer_mask`,
  `triggers` `use_global|ignore|collide`, `ignore` (entities), `max_hits` (1..1024, default 64), `verify`,
  `draw`, `scene`. Result: `hits[]` with `entity` `{id, path}`, `collider`, `trigger`, and for casts
  `point`, `normal`, `distance`; `count`, `truncated`; with `verify`, `verify {reference_count, missing[],
  extra[], ok}`; `draw` shows this query for 2 s in both views even with debug draw off.
  - Physics worlds answer only while playing: in edit mode the error says "physics queries need Play: call
    play first; world: drawn works in edit mode". `world: drawn` accepts op `overlap` only, else the error
    names `raycast_meshes`.
  - `next` hints in `structuredContent`: zero hits with a mask that excludes the layers of the bodies inside
    the shape → "no hits: layer_mask excludes N bodies inside the shape (layers: ...)"; zero hits with
    `triggers: ignore` and only triggers inside → names `triggers: collide`; `verify` with mismatches →
    "report: the query and the sweep differ".
- **R26 `configure_game_view`** gains `physics_debug_queries` (boolean), beside `physics_debug_contacts`.
- **R27 Monitors** (`get_frame_times`): `Physics queries` and `Physics query hits` per frame (counted only
  while the profiler or the Stats panel reads them: a relaxed counter add per query otherwise, R21).
- **R28 Lua.** Every C++ query of R1-R14 with the names of §6 (D9), plus `QueryTriggerInteraction` and
  `CapsuleDirection2D` enums, `OverlapHit` and `MeshOverlapHit` classes, `Physics.queriesHitTriggers`,
  `Physics2D.queriesHitTriggers`, `Physics.debugDrawQueries`. NonAlloc forms clear the results table's
  entries from count + 1 to its old length, so `#results == count` afterwards.
- **R29 Docs.** New `docs/scripting/spatial-queries.md` (choosing a query, triggers, NonAlloc, order,
  frame snapshot, 2D units, mesh colliders are surfaces, verify); `docs/mcp/guide/physics.md` "Layers,
  contacts and queries" gets the overlap, cast and trigger lines and a `query_physics` recipe (check a
  pickup radius in Play with `verify`); `docs/mcp/guide/lua-api.md` §2.10 rows; `docs/mcp/guide/scene-view.md`
  one line for `MeshRaycast` overlaps; `docs/unity-glossary.md` rows; `TOOLS.md` regenerated; a hand-written
  stub fragment `engine/core/script/binding/stubs/SpatialQueries.lua` (the NonAlloc tables' shape, the
  option table of MeshRaycast overlaps); the rest of the stubs come from `tools/lua_api`.
- **R30 CLI.** `jasperengine-editor call query_physics ...` works through the shared registry (S31); no
  CLI-specific code.
- **R31 Eval.** Cases in `tools/mcp/eval/cases.py`: `guard_physics_overlap_tool` (prompt: "in Play, list the
  enemies within 6 m of the player"; must call `query_physics {op: overlap}`, not loop in `eval_lua`),
  `guard_physics_query_triggers` (a pickup trigger is missed with `triggers: ignore`; the `next` hint must
  lead to `collide`), `guard_physics_query_verify` ("check the query is right": `verify: true`). Each fails
  on a tool text without the matching phrase.

### Data & compatibility

- **R32 Project settings.** project.yaml gains `physicsQueries: {hitTriggers3D, hitTriggers2D}`, written only
  when one is false; missing → true. No version bump (optional field with a default, AGENTS.md).
- **R33 Players.** game.json (`PlayerConfig`) gains optional `queries_hit_triggers_3d` and
  `queries_hit_triggers_2d` (absent → true; game.json stays v1: an optional key, inferred from how `PlayerConfig::fromJson` treats missing keys); every
  build, Compile builds included, calls the setters from it (`player::applySettings`, S98 D1).
- **R34 Scripts.** Only functions are added; `raycast`'s extra optional argument changes nothing for
  existing calls. `RayReturn` is unchanged.
- **R35 No scene data.** Nothing new is serialized in scenes or prefabs.
- **R36 Play.** The editor applies the project's two settings at Play start and restores them at Stop,
  as it does the layer matrix (inferred route: `Project.cpp:2316` applies the world capacity).
- **R37 Player modules.** S34's module detection treats `Physics.debugDrawQueries` like `Physics.debugDraw`
  (it draws lines: `runtimeshaders`, `ModuleUsageTest.cpp:437-441`); the query functions select no module,
  as `raycast` today: a backend the build leaves out has no bodies to find and no query functions.

## 6. Design constraints & decisions

- **D1 Layer.** Declarations join the existing static classes in `registry/PhysicsLayers.h`; the 3D
  bodies live in a new `engine/core/physics/PhysicsQueries.cpp`, 2D in `PhysicsQueries2D.cpp`, the
  reference in `PhysicsQueryReference.cpp`, the debug ring in `PhysicsQueryDebug.{h,cpp}`. `Ray::cast` is
  touched only to pass the trigger filter. Lua in a new `engine/core/script/binding/PhysicsQueryLua.cpp`,
  which reopens `Physics`, `Physics2D` and `MeshRaycast` (LuaBridge allows re-opening a class) and is
  called from `CoreClassesLua.cpp` after the existing block.
- **D2 No heap in a query.** Query shapes are stack objects marked `SetEmbedded()` (Jolt's documented way
  to use a `RefTarget` without a reference count): `SphereShape`, `BoxShape`, `CapsuleShape` (a capsule
  between two points is a `CapsuleShape` rotated onto the segment). Collectors are ours:
  `BufferCollideCollector` writes `(BodyID, SubShapeID)` into a fixed array of 256 and, past it, into the
  caller's vector; `AnyHitCollector` stops at the first. Body and collider resolution reads
  `GetSubShapeUserData` under the no-lock interface outside the step, as `Ray::cast` does
  (`Ray.cpp:657-668`). `CollideShapeSettings::mMaxSeparationDistance = 0`,
  `mBackFaceMode = IgnoreBackFaces`. Box2D overlaps use `b2World_Overlap*` with a callback writing into the
  same buffer type. If a Jolt or Box2D path allocates despite this, AC6 fails and the implementer records
  the allocation and its fix in Deviations.
- **D3 Dedupe and order.** A body with several colliders reports each collider once: hits are collected,
  sorted by `(body, collider)` with an in-place insertion sort below 32 hits and `std::sort` above (no
  allocation), then adjacent duplicates are dropped (Jolt can report a compound child twice through
  different sub-shape paths). Casts keep the smallest fraction per collider.
- **D4 Triggers.** 3D: `body.IsSensor()` (trigger companions are sensor bodies). 2D: `b2Shape_IsSensor`.
  `QueryTriggerInteraction { UseGlobal = 0, Ignore = 1, Collide = 2 }` (Unity's values). The two globals
  are `std::atomic<bool>` next to `physicsDebugDraw` (`PhysicsLayers.cpp:303-306`).
- **D5 Casts.** 3D: `NarrowPhaseQuery::CastShape` with `ShapeCastSettings` (`mBackFaceModeTriangles =
  IgnoreBackFaces`, `mReturnDeepestPoint = false`); hits with fraction 0 and positive penetration (started
  inside) are dropped (R3); `normal = -penetrationAxis.Normalized()`, `point` = contact point on the hit
  body. 2D: `b2World_CastCircle/Capsule/Polygon` with a callback like `CastCallback` plus the trigger test.
- **D6 No world, no creation.** A query on a scene without a 3D world returns nothing and does not create
  one (`hasWorld3D()`, as `Ray.cpp:637` already does for rays); 2D checks `b2World_IsValid`.
- **D7 Drawn objects use SceneBvh, no new index.** A spatial hash for every Transform would need upkeep for
  every moving GameObject every frame, whether or not anyone queries; SceneBvh is created on first query,
  refits only what changed, and already covers what is drawn (sprites are meshes). Added:
  `SceneBvhTree::overlap(const AABB& box, Visit)` (the query shape's AABB), a leaf test (sphere vs AABB:
  squared distance to the box; OBB vs AABB: 15-axis SAT), `SceneBvh::overlap(shape, filter, out,
  snapshot)`; `update()` keeps `changed` and the dedupe buffer as members and dedupes with a per-entity
  generation stamp instead of an `unordered_set`; with `snapshot` it refits at most once per
  `Engine::getFrameCount()` (R13). Transform-only GameObjects are found by giving them a trigger collider.
- **D8 Debug ring.** `PhysicsQueryDebug` is a per-scene ring (256 entries × 96 bytes, allocated on the
  first recorded query, freed when debug draw goes off for 10 s); `PhysicsSystem::draw` (the existing debug
  pass) appends its lines when non-empty. `query_physics {draw}` pushes one entry with a 2 s lifetime.
- **D9 Lua names.** `Physics.overlapSphere(scene, center, radius [, layerMask [, qti]])` → array of
  `OverlapHit`; `Physics.overlapSphereNonAlloc(scene, center, radius, results [, layerMask [, qti [,
  colliders]]])` → count, `results[i]` the body's entity id (an integer: no userdata per hit, so no GC),
  `colliders[i]` the collider index when that table is given. `checkX` → boolean; casts → `RayReturn` (the
  hit flag false on a miss, as `raycast`); `...All` → array. 2D first forms return an `OverlapHit` or nil.
  `OverlapHit` fields: `body`, `colliderIndex`, `isTrigger` (RayReturn's field names). Entity ids turn into
  objects with `GameObject(scene, id)`.
- **D10 MCP in Play only for physics.** Bodies exist when the scene runs (§3); refusing in edit mode keeps
  answers honest. `world: drawn` uses the selected scene in edit mode and the playing one in Play.
- **D11 `query_physics` budget.** Estimated 1,350 characters (description ~380, schema ~970; inferred from
  the schema helpers, not measured); the `physics` group goes from 2 to 3 tools, far under 15,000.
  `configure_game_view` +40. Nothing enters `core`. `tool_budget.py --check` is the gate; over the limit,
  shape-field detail moves to `jasper://docs/physics` first.

C++ sketch:

```cpp
namespace jasperengine {
    enum class QueryTriggerInteraction : uint8_t { UseGlobal = 0, Ignore = 1, Collide = 2 };
    enum class CapsuleDirection2D : uint8_t { Vertical = 0, Horizontal = 1 };
    struct OverlapHit { Entity body = NULL_ENTITY; uint32_t colliderIndex = 0; bool trigger = false; bool hit = false; };
    struct PhysicsQueryOptions {                                // the trailing arguments, for C++ callers
        uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST;
        QueryTriggerInteraction triggers = QueryTriggerInteraction::UseGlobal;
        std::span<const Entity> ignore;                          // passes through these
    };
    class Physics {   // additions (registry/PhysicsLayers.h)
    public:
        static void setQueriesHitTriggers(bool hit);  static bool getQueriesHitTriggers();
        static void setDebugDrawQueries(bool draw);   static bool isDebugDrawQueries();
#ifdef JASPERENGINE_PHYSICS_3D
        static size_t overlapSphere(Scene*, const Vector3& center, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static size_t overlapBox(Scene*, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static size_t overlapCapsule(Scene*, const Vector3& point0, const Vector3& point1, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static bool checkSphere(Scene*, const Vector3& center, float radius, const PhysicsQueryOptions& = {});
        static bool checkBox(Scene*, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, const PhysicsQueryOptions& = {});
        static bool checkCapsule(Scene*, const Vector3& point0, const Vector3& point1, float radius, const PhysicsQueryOptions& = {});
        static RayReturn sphereCast(Scene*, const Vector3& origin, float radius, const Vector3& direction, float maxDistance, const PhysicsQueryOptions& = {});
        static RayReturn boxCast(Scene*, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, float maxDistance, const Quaternion& rotation, const PhysicsQueryOptions& = {});
        static RayReturn capsuleCast(Scene*, const Vector3& point0, const Vector3& point1, float radius, const Vector3& direction, float maxDistance, const PhysicsQueryOptions& = {});
        static size_t sphereCastAll(Scene*, const Vector3& origin, float radius, const Vector3& direction, float maxDistance, std::vector<RayReturn>& out, const PhysicsQueryOptions& = {});
        static size_t boxCastAll(/* as boxCast */ std::vector<RayReturn>& out, const PhysicsQueryOptions& = {});
        static size_t capsuleCastAll(/* as capsuleCast */ std::vector<RayReturn>& out, const PhysicsQueryOptions& = {});
        static RayReturn raycast(Scene*, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask, QueryTriggerInteraction triggers);  // + Ray and All overloads
#endif
    };
    class Physics2D {  // additions
    public:
        static void setQueriesHitTriggers(bool hit);  static bool getQueriesHitTriggers();
#ifdef JASPERENGINE_PHYSICS_2D
        static OverlapHit overlapPoint(Scene*, const Vector2& point, const PhysicsQueryOptions& = {});
        static size_t overlapPointAll(Scene*, const Vector2& point, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static OverlapHit overlapCircle(Scene*, const Vector2& center, float radius, const PhysicsQueryOptions& = {});
        static size_t overlapCircleAll(Scene*, const Vector2& center, float radius, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static OverlapHit overlapBox(Scene*, const Vector2& center, const Vector2& size, float angleDegrees, const PhysicsQueryOptions& = {});
        static size_t overlapBoxAll(Scene*, const Vector2& center, const Vector2& size, float angleDegrees, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static OverlapHit overlapCapsule(Scene*, const Vector2& center, const Vector2& size, CapsuleDirection2D, float angleDegrees, const PhysicsQueryOptions& = {});
        static size_t overlapCapsuleAll(/* as overlapCapsule */ std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static OverlapHit overlapArea(Scene*, const Vector2& pointA, const Vector2& pointB, const PhysicsQueryOptions& = {});
        static size_t overlapAreaAll(Scene*, const Vector2& pointA, const Vector2& pointB, std::vector<OverlapHit>& out, const PhysicsQueryOptions& = {});
        static RayReturn circleCast(Scene*, const Vector2& origin, float radius, const Vector2& direction, float distance, const PhysicsQueryOptions& = {});
        static RayReturn boxCast(Scene*, const Vector2& origin, const Vector2& size, float angleDegrees, const Vector2& direction, float distance, const PhysicsQueryOptions& = {});
        static RayReturn capsuleCast(Scene*, const Vector2& origin, const Vector2& size, CapsuleDirection2D, float angleDegrees, const Vector2& direction, float distance, const PhysicsQueryOptions& = {});
        // circleCastAll, boxCastAll, capsuleCastAll: std::vector<RayReturn>& out
#endif
    };
    struct MeshOverlapHit { Entity entity = NULL_ENTITY; PickKind kind = PickKind::MESH; int32_t instance = -1; uint64_t foliageId = 0; int32_t prototype = -1; };
    // class MeshRaycast, additions; MeshRaycastOptions gains `bool current = false;` (R13)
    // static size_t overlapSphere(Scene*, const Vector3& center, float radius, std::vector<MeshOverlapHit>& out, uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST, const MeshRaycastOptions& = {});
    // static size_t overlapBox(Scene*, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, std::vector<MeshOverlapHit>& out, uint32_t layerMask = ..., const MeshRaycastOptions& = {});
    namespace physics {   // PhysicsQueryReference.h
        struct QueryShape { enum class Type : uint8_t { Sphere, Box, Capsule, Point2D, Circle2D, Box2D, Capsule2D, Area2D } type;
            Vector3 a, b; Vector3 halfExtents; Quaternion rotation; float radius = 0, angle = 0; CapsuleDirection2D direction2D{}; };
        struct ReferenceDiff { std::vector<OverlapHit> missing, extra; size_t referenceCount = 0; bool ok() const; };
        JASPERENGINE_API void referenceOverlap(Scene*, bool is2D, const QueryShape&, const PhysicsQueryOptions&, std::vector<OverlapHit>& out);
        JASPERENGINE_API ReferenceDiff compare(std::span<const OverlapHit> query, std::span<const OverlapHit> reference);
    }
}
```

Lua:

```lua
local ENEMY = LayerMask.getMask("Enemy")
local near = Physics.overlapSphere(self.scene, self.go.position, 8, ENEMY)        -- array of OverlapHit, by body
for i = 1, #near do local enemy = GameObject(self.scene, near[i].body) end
self.hits = self.hits or {}                                                       -- reused: no garbage per frame
local n = Physics.overlapSphereNonAlloc(self.scene, p, 8, self.hits, ENEMY, QueryTriggerInteraction.Ignore)
for i = 1, n do local id = self.hits[i] end                                       -- entity ids; #self.hits == n
local inZone = Physics.checkBox(self.scene, center, Vector3(2, 1, 2), Quaternion.IDENTITY, -1, QueryTriggerInteraction.Collide)
local blocked = Physics.overlapCapsule(self.scene, foot, head, 0.4)
local hit = Physics.sphereCast(self.scene, eye, 0.3, forward, 20, Physics.DEFAULT_RAYCAST_LAYERS)
if hit.hit then print(hit.distance, hit.body) end
local hits = Physics.boxCastAll(self.scene, c, Vector3(0.5, 0.5, 0.5), dir, 10, rot)
local ray = Physics.raycast(self.scene, eye, forward, 50, -1, QueryTriggerInteraction.Ignore)
Physics.queriesHitTriggers = false ; Physics.debugDrawQueries = true
local first = Physics2D.overlapCircle(self.scene, Vector2(x, y), 48)              -- OverlapHit or nil
local all = Physics2D.overlapBoxAll(self.scene, Vector2(x, y), Vector2(64, 32), 15)
local n2 = Physics2D.overlapCapsuleNonAlloc(self.scene, c, Vector2(20, 60), CapsuleDirection2D.Vertical, 0, self.hits)
local h2 = Physics2D.circleCast(self.scene, Vector2(x, y), 8, Vector2(1, 0), 200)
local seen = MeshRaycast.overlapSphere(self.scene, p, 15, -1, {kinds = MeshRaycast.MESH | MeshRaycast.INSTANCE})
local m = MeshRaycast.overlapBoxNonAlloc(self.scene, c, Vector3(4, 4, 4), Quaternion.IDENTITY, self.hits)
```

MCP shapes:

```json
{"op": "overlap", "world": "3d", "shape": {"type": "sphere", "radius": 6}, "origin": [0, 1, 0],
 "layer_mask": 256, "triggers": "ignore", "verify": true}
{"op": "overlap", "world": "3d", "count": 2, "truncated": false,
 "hits": [{"entity": {"id": 41, "path": "Enemies/Grunt#2"}, "collider": 0, "trigger": false},
          {"entity": {"id": 57, "path": "Enemies/Archer"}, "collider": 1, "trigger": false}],
 "verify": {"reference_count": 2, "missing": [], "extra": [], "ok": true}}
{"op": "cast", "world": "2d", "count": 1, "hits": [{"entity": {"id": 9, "path": "Wall"}, "collider": 0,
 "trigger": false, "point": [128, 40, 0], "normal": [-1, 0, 0], "distance": 96.5}]}
{"op": "overlap", "count": 0, "next": "no hits: layer_mask excludes 3 bodies inside the shape (layers: Enemy)"}
```

## 7. Acceptance criteria

Unit tests in `engine/tests/core/physics/PhysicsQueryTest.cpp` (suites `PhysicsQuery`, `PhysicsQuery2D`;
core tests already build scenes with physics, `PhysicsRayThreadsTest.cpp:27-30`),
`engine/tests/core/picking/SceneBvhTest.cpp` and `MeshRaycastTest.cpp` (appended),
`engine/tests/core/script/LuaPhysicsQueryTest.cpp` (suite `LuaPhysicsQuery`), bench
`engine/tests/core/bench/PhysicsQueryBench.cpp` (keys in `tools/perf/bench-thresholds.json`), editor tests in
`editor/tests/ProjectPhysicsSettingsTest.cpp` and `editor/tests/players/ModuleUsageTest.cpp` (appended), MCP tests appended to
`tools/mcp/tests/test_physics_contacts_lua.py` (`CI = True`). "Red" = the run on the tree without the change.

| AC | Verifies | Check |
|---|---|---|
| AC1 | R1, R6, R16 | `PhysicsQuery.OverlapMatchesReference`: `query_city`, 2,000 random spheres, boxes and capsules (seeded): every result equals `referenceOverlap` element for element. Red: the function does not exist (compile) and, for the claim, a deliberately unsorted variant fails the order check |
| AC2 | R5, D4 | `PhysicsQuery.TriggersFollowInteraction`: a solid body, a trigger body and a body with both (trigger companion): Ignore → solids only; Collide → all; UseGlobal follows `setQueriesHitTriggers` both ways; the same for `raycast` with the new argument |
| AC3 | R4, R34 | `PhysicsQuery.RaycastUnchangedByDefault`: 1,000 seeded rays through `query_city`: `raycast`/`raycastAll` results equal a copy of today's `Ray::cast` output recorded on the pre-change tree into `fixtures/physics/ray_city_hits.txt` (the pre-change run is the control) |
| AC4 | R5, R8 | `PhysicsQuery.LayerAndIgnore`: masks exclude layers, category/mask bits apply, the ignore list removes its bodies, a scene without a 3D world returns 0 and `hasWorld3D()` stays false; a Character Controller with Detect Collisions is found and one without is not; a sleeping body is found |
| AC5 | R2, R3, D5 | `PhysicsQuery.CastsHitNearestAndSkipStartInside`: a sphere cast towards three boxes returns the nearest (distance ± 1e-3, normal facing the cast); a box it starts inside is not reported; `sphereCastAll` returns three in distance order; `checkSphere` agrees with `overlapSphere(...) > 0` on 1,000 queries |
| AC6 | R20, D2 | `PhysicsQuery.NoAllocations`: after one warm-up call, 1,000 calls each of overlapSphere/Box/Capsule (caller buffer with capacity), check*, sphereCast and 2D overlapCircle/Box with `AllocationCountScope` → 0 on Linux (`countsAllocations()`); red control: the same loop through `AllHitCollisionCollector` counts > 0 |
| AC7 | R9, R10, R11 | `PhysicsQuery2D.OverlapsAndCastsMatchReference`: the 2D city, every overlap form against `referenceOverlap`; first forms return the first of the All form; sensors follow the interaction; casts in points; `overlapArea` with swapped corners equals the ordered one |
| AC8 | R7 | `PhysicsQuery.BadArgumentsLogOnce`: radius 0, NaN centre, zero direction: one error each (`LogCapture`), 0 / false / no hit |
| AC9 | R18 | `PhysicsQuery.OverlapsFromJobs`: 8 jobs × 500 overlaps outside the step equal the main-thread answers; run under `SANITIZE=thread` in the sanitizer lane (as `PhysicsRayThreads`); inside a contact callback an overlap returns 0 and logs once |
| AC10 | R12, D7 | `SceneBvh.OverlapMatchesLinearScan` (beside `MatchesLinearScan`, `SceneBvhTest.cpp:94`): 5,000 random leaves, 1,000 sphere and OBB queries equal a brute-force leaf scan |
| AC11 | R13, R20 | `MeshRaycast.OverlapSnapshotRefitsOncePerFrame`: within one frame, move an object between two overlaps: the second sees the old bounds; with `current = true` the new; next frame the new. Allocation count 0 for 1,000 overlaps without a rebuild. Red: today's `update` allocates (`changed`) |
| AC12 | R28, D9 | `LuaPhysicsQuery.NonAllocFillsAndClears`: `overlapSphereNonAlloc` writes ids, `#t == n` after a call with fewer hits than the previous; `collectgarbage("count")` unchanged over 1,000 calls once the table has grown; enums and `OverlapHit` fields readable; 2D first form returns nil on a miss |
| AC13 | R15, R21 | `PhysicsQuery.DebugDrawRecordsOnlyWhenOn`: off → the ring is never allocated; on → 300 queries keep the newest 256; entries expire after 1 s of unscaled time; job queries are not recorded |
| AC14 | R19 | `PhysicsQueryBench` keys `overlap_sphere_p95_ms` (0.0066), `overlap_box_p95_ms` (0.0088), `check_sphere_p95_ms` (0.0033), `sphere_cast_p95_ms` (0.013), `lua_overlap_nonalloc_p95_ms` (0.011), `overlap_circle_2d_p95_ms` (0.0044), `mesh_overlap_p95_ms` (0.011): desktop budgets + 10 % in `bench-thresholds.json`, enforced with `perfSlack()` on the benchmark run; `PickingBench` ray numbers within 5 % |
| AC15 | R19, R17 | Web: the `mesh-raycast` test project gains a 2,000-body scene and times 10,000 `Physics.overlapSphereNonAlloc` and 10,000 `MeshRaycast.overlapSphere` calls in WebAssembly; `GATE=mesh-raycast` fails on a p99 over 0.04 ms each (console lines in `tools/web/gates/mesh-raycast.json`) |
| AC16 | R22, R32 | `ProjectPhysicsSettings.QueriesHitTriggersRoundTrip` (beside `DefaultsWriteNeitherKey`, `ProjectPhysicsSettingsTest.cpp:45`): toggling the 3D and 2D rows is one undo step each, writes `physicsQueries` only when false, a project.yaml without the key loads true (old fixture), and Play start applies it (`Physics::getQueriesHitTriggers()` false inside Play, restored at Stop) |
| AC17 | R33 | `ProjectPhysicsSettings.GameJsonCarriesQueriesHitTriggers` (editor suite, which writes game.json through `writeGameConfig`): game.json without the keys → true; with false → false after `fromJson`; `toJson` writes them only when false |
| AC18 | R25, R30, D10 | `test_physics_contacts_lua.py::test_query_physics_overlap`: in Play, `query_physics` overlap/check/cast/raycast on a scene with three bodies and a trigger give the expected entity paths; `verify: true` reports `ok`; edit mode refuses with the error naming `play`; `world: drawn` answers in edit mode; `world: drawn` with `op: cast` names `raycast_meshes`; the tool is hidden until `tool_groups` enables `physics`; `jasperengine-editor call query_physics --help` lists its parameters (R30) |
| AC19 | R25 | `test_physics_contacts_lua.py::test_query_physics_hints`: a mask that excludes the bodies in the sphere returns the `next` hint naming the layer; `triggers: ignore` with only a trigger inside names `collide` |
| AC20 | R23, R26 | `test_physics_contacts_lua.py::test_query_debug_draw`: `configure_game_view {physics_debug_queries: true}`, a Lua overlap in Play, `screenshot` of the Game view differs from one taken with it off in the query's region (pixel diff > 0 inside its projected bounds, 0 outside); `query_physics {draw: true}` draws with the toggle off |
| AC21 | R27, R24 | `test_physics_contacts_lua.py::test_query_monitors`: after a frame running 10 overlaps, `get_frame_times {monitors: ["Physics queries"]}` reports 10 |
| AC22 | R29, R31, D11 | `gen_tool_docs.py --check`, `tool_budget.py --check`, `test_tool_conventions.py`, `test_autosync.py` (guide names exist); eval cases of R31 fail on the pre-change tool list and pass after (run on Orquester-managed accounts as `tools/mcp/eval/README.md` says; if no account is lent, reported unverified) |
| AC23 | R17, R37 | `ModuleUsage.PhysicsQueryDebugNeedsRuntimeShaders` (beside `PhysicsDebugDrawingNeedsRuntimeShaders`, `ModuleUsageTest.cpp:437`): `Physics.debugDrawQueries = true` needs `runtimeshaders`; `Physics.overlapSphere(...)` alone needs neither it nor a physics module (bodies decide the module, `ModuleUsageTest.cpp:130-136`) |

## 8. Dependencies & conflicts

- **Depends on:** nothing unlanded. Uses S26's SceneBvh and MeshRaycast, S15's monitors, S31's CLI.
- **Sibling specs:** `culling-group-and-simulation-distance.md` freezes bodies as kinematic (they stay in
  the broad phase, so these queries still find them) and also extends `configure_game_view` and the Game
  view Stats panel (merge hotspot, separate parameters). `project-test-runner.md` and
  `input-recorder-and-input-scripts.md` rely on deterministic results: R6's order serves them.
  `dev-console-for-agents.md` may expose a console command over `query_physics`; it should call the C++
  API, not duplicate it. `release-performance-and-gpu-quirks.md` and `build-size-tools.md`: the new code
  adds an estimated 20-30 KB to a player with both backends (inferred, not measured), reported by their
  size tools.
- **Files touched that others touch:** `registry/PhysicsLayers.{h,cpp}`, `math/Ray.cpp`,
  `script/binding/CoreClassesLua.cpp` (one call), `picking/SceneBvh.{h,cpp}`, `picking/MeshRaycast.{h,cpp}`,
  `subsystem/PhysicsSystemDebugDraw.cpp`, `player/PlayerConfig.{h,cpp}`, `editor/Generator.cpp`,
  `editor/Project.{h,cpp}`, `editor/Stream.cpp`, `editor/settings/SettingsRows.cpp`,
  `editor/window/dialog/ProjectSettingsWindow.cpp`, `editor/window/GameWindow.cpp` (toolbar and Stats),
  `editor/mcp/tools/ViewTools.cpp` (`configure_game_view`), a new `editor/mcp/tools/PhysicsQueryTools.cpp`
  registered in the physics module row of `McpTools.cpp`, `profiler/EngineProfiling.cpp`,
  `tools/perf/bench-thresholds.json`, `tools/test-projects/mesh-raycast`, the docs of R29.
- **Blocks:** `culling-group-and-simulation-distance.md` does not need it; AI sample scenes and any future
  perception system want it.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| Jolt's `CollideShape` allocates internally for some shape pairs (mesh, heightfield, compound) | AC6 measures each pair type in `query_city`; a pair that allocates is fixed with a stack allocator passed through Jolt's `TempAllocator` overloads where they exist, else recorded in Deviations with its count |
| Sorting costs on large result sets | Insertion sort under 32 hits, `std::sort` above; `max_hits` caps MCP; documented that `overlapSphereNonAlloc` with hundreds of hits is O(n log n) |
| `queriesHitTriggers` false surprises old raycast scripts | Default true keeps today's behaviour; the row's tooltip says it applies to rays too |
| SceneBvh frame snapshot hides a move made between two queries | R13 documents it with Unity's precedent; `current = true` opts out; ray casts unchanged |
| Overlap of a mesh collider from inside reports nothing | Unity semantics, documented with a convex-collider recommendation |
| Character Controller without Detect Collisions is invisible to queries | Documented (as for rays today) |
| 2D capsule `size` and `direction` semantics differ from Box2D's two-point capsule | Converted as Unity defines (`size` is the bounding box; the round ends along `direction`); `PhysicsQuery2D.OverlapsAndCastsMatchReference` covers both directions |
| `query_physics` over 1,500 characters | Field details move to `jasper://docs/physics`; never an exception without a measured size + 5 % |
| Queries in contact callbacks are refused, but a script expects them | One error per call site naming "outside the physics step: queue the query for onUpdate"; Lua `synchronousContactEvents3D` scripts are warned in the docs |
| Recording queries from many scenes in debug draw | One ring per scene, drawn only for scenes that run; freed after 10 s off |

## Deviations

- **D1 / R15 frame key.** `Engine::getFrameCount()` does not exist and `Engine::getGameFrames()`
  stops while paused and in the editor's edit mode (a `world: drawn` overlap there would never
  refit). `Engine::getFrameIndex()` is added: every engine frame, paused or not, one increment
  beside the frame arena's reset. Tests advance it through `EngineTestAccess`.
- **D2 buffers.** The 3D collector (its `OnBody` and its `TransformedShape` context give the entity,
  trigger flag and collider index with no second body lock) and the 2D callbacks write straight into
  the caller's vector; there is no fixed 256-entry array. The 2D first forms keep only the lowest
  `(body, collider)` hit, with no buffer. The Lua forms use one main-thread buffer each.
- **D2 query box.** Box query shapes use Jolt's default convex radius (5 cm, at most half the
  smallest half extent): the corners are rounded by that much, the faces are exact; shallow pairs
  then need GJK only.
- **D2 overlaps without contact points.** Overlaps and checks do not call
  `NarrowPhaseQuery::CollideShape`: the broad phase lists the bodies, each body's leaf shapes inside
  the query's bounds are listed (`CollectTransformedShapes`), and a convex leaf gets Jolt's own
  convex-vs-convex decision (its bounds test and GJK step on the shapes without their convex radius;
  intersecting cores, where Jolt would run EPA for a depth, overlap); a mesh or height field leaf gets
  its convex-vs-triangles decision triangle by triangle (back faces skipped, bounds, GJK step), and any
  other leaf goes through Jolt's collide. A box query asks the broad phase with its oriented box. The
  answers equal `CollideShape`'s (PhysicsQueryReference keeps using it, as the independent sweep)
  without the penetration depth an overlap never needed: the EPA on every body a 5 m box held wholly
  inside had made `overlapBox` 4x over its budget (0.033 ms). `je-bench-physics-query` on box B (load
  about 10 on 12 cores, three runs): `overlap_box_p95_ms` 0.0084, 0.0084, 0.0071 against 0.0088, and
  every other key within its budget.
- **R3 casts.** Every hit at fraction 0 is dropped (touching at the start counts as overlapping,
  R1), not only those with positive penetration: without `mReturnDeepestPoint` Jolt reports no
  reliable depth there. A Box2D cast callback never returns -1 or 0 for a skipped shape (Box2D 3.1
  carries the value into its next tree as the maximum fraction, and 0 stops the cast).
- **R12 foliage.** Foliage has no SceneBvh leaves, so the overlaps walk FoliageSystem's cells as rays
  do (`FoliageSystem::forEachFoliageInBox`, no allocation): the tree cells under the query's box (or
  the resident ones when they are fewer), each tree first by its position, and a tree is a hit when
  its LOD0 mesh's box under its matrix touches the shape (bounds, as a leaf). The walk pads the box by
  sqrt(3) times the terrain's axis scale ratio, so a turned or unevenly scaled Terrain loses no tree.
  Only resident trees (within Tree Distance plus a cell) and placed details exist on the CPU, so only
  they are found. Details are opt-in (`DETAIL`) as for rays: `foliageId` 0, the layer in `prototype`.
  Lua gains `MeshRaycast.TREE` and `DETAIL` (T5 bound neither); a NonAlloc form writes the Terrain's
  id per tree. `query_physics {world: drawn}` names a tree by `tree_id` and `prototype` as
  `raycast_meshes` does. `je-bench-physics-query` gains `mesh_overlap_trees_p95_ms` (T5's 50,000
  bench trees, r = 5 m spheres at trees, the leaves' budget 0.011): 0.0099, 0.0082, 0.0083 on box B
  (load about 20 on 12 cores, three runs), every other key within its budget.
- **R18 / risks "one error per call site".** One error per query function per process (an atomic
  flag each), for C++ and Lua alike, naming "outside the physics step: queue the query for onUpdate".
- **R22 undo.** The existing Physics rows apply without undo; the two new rows go through a new
  `PhysicsQuerySettingsCmd` (one step per changed row, from the dialog's OK and from
  `set_project_settings physics.queries_hit_triggers` / `physics2d.queries_hit_triggers`).
- **R23 toolbar.** The Game view has no Physics menu and no Contacts item: Queries is a toggle button
  beside Physics (icon when narrow), saved in the editor preferences (`physics_queries`).
- **R27 monitors.** Every query (and every `Physics` / `Physics2D` raycast) does one relaxed add on
  two process-wide counters; the profiler's `publishMonitors` publishes per-frame differences (in
  profiler builds) and the Stats panel the difference since its last draw. Raycasts are also drawn
  by the query debug draw, as a zero-radius cast.
- **R28 Lua extras.** The ignore list is a trailing table (after `qti`, after `colliders` in a
  NonAlloc form). `Physics.overlapBoxNonAlloc` takes `results` before the optional rotation (Unity);
  `MeshRaycast.overlapBoxNonAlloc` takes the rotation first (§6's example). 2D box and capsule
  `angle` arguments are optional (0). The two enums are bound with typed values in
  `PhysicsQueryLua.cpp` (its own `Stack` specializations).
- **R25 details.** `query_physics` `verify` checks physics overlaps only (no drawn-world reference);
  its `raycast` drops the `ignore` bodies after the cast (`Physics.raycast` has no ignore list);
  `world: drawn` refits now (`current`). An unnamed layer is named by its number in the hint.
- **R37 / AC23 modules.** `Physics.raycast` does select a module today: functions bound only under
  `JASPERENGINE_PHYSICS_3D` / `_2D` reach `physics3d` / `physics2d` in the module scan. The queries do
  the same (as `raycast`), so AC23 checks they reach the same modules as `raycast` and need no runtime
  shaders. The scan matches member names, so `MeshRaycast.overlapSphere` / `overlapBox`, which share
  their names with the `Physics` ones, keep the physics module too (the safe side).
- **AC3 fixture.** The fixture is written with `JASPER_TEST_UPDATE_GOLDEN=1` (the variable
  `tools/build.sh` forwards into the image), by the same test on the tree without the change.
- **AC3 on macOS (Mac pass).** The city and the rays draw through `test::cityUniform` and
  `cityUniformInt`, libstdc++'s `uniform_real_distribution` and `uniform_int_distribution` spelled
  out, with the rays' z drawn before x as GCC evaluated `Vector3(along(rng), 6.0f, along(rng))`: the
  standard leaves the distributions and the argument order to the compiler, and macOS (libc++, clang)
  built another city and failed the fixture (ray 1). Same fixture; green on Linux (13 PhysicsQuery
  tests) and on an arm64 Mac (`tools/ci/macos.sh tests`). `NoAllocations` skips with "cannot count
  allocations", the reason the macOS and Windows gates allow.
- **AC15 timing.** A browser's clock is too coarse for one 0.04 ms call: the gate times batches of
  100 calls and takes the p99 of the per-call mean over 100 batches. `MeshRaycast.overlapSphere` is
  timed from C++, `Physics.overlapSphereNonAlloc` through Lua.
- **AC15's budget is reported, not asserted.** `GATE=mesh-raycast` prints each p99 per call with its
  0.04 ms budget every run; queries that find nothing fail it, a p99 over the budget is a warning line
  (the nightly's MCP shard builds the project in Debug, several times slower). CI's browser renders in
  software (SwiftShader) and a loaded host preempts the page, so a hard check measured the runner, not
  the engine; wall-clock budgets are reported, never asserted (AGENTS.md), as core-utilities' AC17 gate
  did in #163; `PhysicsQueryBench` still holds the desktop figures.
- **D8 ring.** An entry holds the `QueryShape`, the sweep and up to 4 hit points (more than 96 bytes);
  the scene's `PhysicsQueryDebug` is made by the first recorded query and its ring freed after 10 s
  with debug draw off and nothing live.
- **R19 on box A.** On box A (a 2.4 GHz Haswell, slower per core than box B) `overlap_box_p95_ms`
  measured 0.0093-0.0112 ms, `overlap_sphere_p95_ms` 0.0060-0.0079 ms and `mesh_overlap_trees_p95_ms`
  0.0106-0.0119 ms. An overlap now tests a body of one convex shape in place, under the read lock that
  keeps the shape alive, instead of through three `TransformedShape` copies; a box query takes a leaf
  wholly inside its core (the box less its rounded edge) without the GJK step that would find the
  same; a tree's instance matrix is written out instead of two 4x4 products of zeros and ones (the
  same values). The answers are the same (`PhysicsQuery.OverlapMatchesReference`,
  `LuaPhysicsQuery.MeshOverlapReadsTrees`). Six quiet runs: overlapBox 0.0064-0.0071 ms in four and
  0.0097-0.0099 in two, still over its 0.0088 there; overlapSphere 0.0050-0.0062 in five and 0.0069
  in one; the trees 0.0099-0.0105 in five and 0.0126 in one.
- **R19 trees on box A, third pass.** The trees' runs spiked (0.0119-0.0161 ms in two of six) in
  stretches of the run, a cold start among them: the walk was bound by how much it touched and
  computed per tree, not by allocation (none) or page faults. Per query it scanned 153 instances in
  3.1 cells and built two 4x4 products for the 31 that passed the point test, to cull 20 of them by
  their sphere's cube. Now the local pads widen only a sphere's radius by the walk's sqrt(3) times
  the axes' ratio (the offset of the sphere's centre as it is, the bound the world cube needs), so
  137 instances in 2.8 cells are scanned and 25 pass; the cube test finds the sphere's world centre
  without a matrix, its reach widened by 1e-5 of the coordinates' size so rounding cannot drop a
  tree the matrices would keep; and a kept tree's world box is computed from the terrain's affine
  matrix and the instance written out (Arvo's form, as `AABB::transform`). Six quiet runs: the trees
  0.0059-0.0066 ms; overlapBox 0.0064-0.0066, overlapSphere 0.0049-0.0057, every budget of the
  bench within its limit. Passing the query's sphere to the walk was measured and left out: its
  cube-against-sphere cull dropped one tree in eleven for a wider API.
- **R25, AC19: no hint for a trigger left out.** The `next` for zero hits with only triggers inside
  (`pass triggers: collide`) is cut: the eval case `guard_physics_query_triggers`, with a prompt that
  no longer mentions triggers, passed 4/4 without it (agents pass `triggers: collide` for a pickup on
  their first query), and AGENTS.md cuts MCP text no case needs. The layer-mask hint stays;
  `test_query_physics_hints` now checks that the trigger case has no `next`.
- **Rebuilt on S96 (#8) when it merged.** S96 (docs/specs/S96-physics-gaps.md) shipped the same 3D
  and 2D overlaps, checks and shape casts, `QueryTriggerInteraction` and `CapsuleDirection2D`
  (spelled `USE_GLOBAL` / `IGNORE` / `COLLIDE`, `VERTICAL` / `HORIZONTAL`), `Physics.queriesHitTriggers`
  / `Physics2D.queriesHitTriggers`, their Lua forms (an options table `{ignore, triggers, ...}` last,
  `Collider` / `Collider2D` results) and a `query_physics` tool first. Those are now S96's
  (`PhysicsQueries3D`, `PhysicsQueries2D`, `QueryOptions`, `PhysicsQueriesLua.cpp`) and this spec
  keeps only what S96 lacks, built on them: the Lua `...NonAlloc` overlaps (Unity's argument order,
  S96's options table, GameObject ids and collider indices, no garbage once the tables have grown);
  `MeshRaycast`'s bounds and tree overlaps; `Physics.debugDrawQueries` and the Game view's Queries
  toggle, fed by S96's queries and raycasts (`physics::noteOverlap` / `noteCast`, which also count
  the `Physics queries` monitors); `PhysicsQueryReference` (now S96's filters, back-face rule and
  collider resolution, `QueryCollider` results); Project Settings > Physics / Physics 2D > Queries
  Hit Triggers (S96 has the globals but no project route) applied to S96's globals; and
  `query_physics` keeps S96's schema plus `mode: verify` and `dimension: drawn` (1,500 characters
  with a description shortened to fit). Dropped, as duplicates of S96: this spec's own query core
  (`PhysicsQueries.cpp` and its `PhysicsQueries2D.cpp`, `OverlapHit`, `PhysicsQueryOptions`, R1-R11's
  C++ signatures and R6's `(body, colliderIndex)` order: S96 orders by GameObject then collider
  index and resolves an attached child's collider to the child), the trailing `qti` / ignore Lua
  arguments, `OverlapHit` in Lua, `Ray::cast`'s `hitTriggers` argument, R18's "inside the step a
  query finds nothing" (S96 queries inside a step through the no-lock query) and R8's "a sphere
  inside a closed mesh does not overlap it" (S96 counts its back faces); `query_physics`'s
  `op` / `world` / `shape` object / `max_hits` / `truncated`, its 2 s `draw` through the debug ring
  (S96's `draw` keeps its 3 s timed lines) and the layer-mask `next` hint (no eval case needs it);
  the C++ rows of `je-bench-physics-query` (`overlap_sphere`, `overlap_box`, `check_sphere`,
  `sphere_cast`, `overlap_circle_2d`: S96's code, which `PhysicsQueriesBenchTest` measures) and the
  tests of the dropped core (`TriggersFollowInteraction`, `RaycastUnchangedByDefault` with its
  `ray_city_hits.txt` fixture, `LayerAndIgnore`, `CastsHitNearestAndSkipStartInside`,
  `NoAllocations`, `BadArgumentsLogOnce`, `OverlapsFromJobs`, the 2D casts); the eval cases
  `guard_physics_overlap_tool` (S96's `guard_physics_overlap_query` asks the same) and
  `guard_physics_query_triggers` (the parameter is S96's).
