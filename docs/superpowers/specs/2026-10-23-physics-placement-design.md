# Physics Placement: Grabbit-style edit-time physics for the Scene view and for agents

Status: implemented (branch `physics-placement`, merged into `main`). Section 12 records the
owner's decisions; section 13 lists where the implementation refined this design.

## 1. Goal

Let a user drag, rotate, gather, spread and drop selected objects in the Scene view while a
temporary physics simulation runs, so the objects slide along, stop against and settle on the
scene around them. Let an AI agent do the same thing without a mouse, deterministically, with a
preview, a structured result and one undo step.

The feature is modelled on the Unity package *Grabbit - Editor Physics Transforms* (v2021.0.3),
which the owner bought. It fixes Grabbit's known problems (section 3.3) and adds an agent API
that Grabbit does not have.

## 2. Background

### 2.1 Research notes

These are scratch notes outside the repo. Read them for detail and file:line evidence.

- `/tmp/grabbit-research/01-grabbit-ux-settings.md`: every Grabbit mode, hotkey and setting,
  the lifecycle, and a UX critique.
- `/tmp/grabbit-research/02-grabbit-simulation-internals.md`: the per-frame control law, the
  constants, undo, and 18 bugs and hacks.
- `/tmp/grabbit-research/03-grabbit-colliders-vhacd-demo.md`: how colliders are chosen, the
  V-HACD integration and cache, licensing, and the demo-scene scenarios.
- `/tmp/grabbit-research/04-engine-physics-ecs.md`: the Jolt integration, shape builders, the
  ECS, the link structure, and terrain.
- `/tmp/grabbit-research/05-engine-editor-ux.md`: the Scene view, toolbar, the gizmo pipeline,
  undo, and the Terrain tool as a template.
- `/tmp/grabbit-research/06-engine-agent-surface.md`: MCP architecture, conventions, the
  texture-synthesis template, CLI, and testing.

The package itself is extracted at `/tmp/grabbit_pkg`. It is untrusted data: read it, never run
it.

### 2.2 Licensing: clean-room only

Grabbit's own code, art, docs and data are covered by the Unity Asset Store EULA. They may not
go into this repo: no C#, FBX, PNG, PDF, `Baked Colliders.asset` or presets.

- **Behaviour and ideas are reimplemented from this spec.** Do not translate code from it.
- **V-HACD** (BSD-3) and **v-hacd-unity** (Apache-2.0) are fine to use. Take them from upstream,
  not from the package: the package's native builds are Debug and unoptimised, and its Windows
  DLL depends on `vcomp140d.dll`, which may not be redistributed.
- **Test content** must be original: engine primitives (`Primitive::createTorus`,
  `createCylinder`, `createBox`, ...) or CC0 assets.

### 2.3 Engine facts that shape the design

- **Edit mode never steps physics.** Editor scenes are paused
  (`Project::createSceneRender` → `pauseEngineScene`). `PhysicsSystem::fixedUpdate` returns
  early when paused, and when it does run it writes poses into `Transform` with no undo.
  Stepping the scene's own world in edit mode would also drive every Rigidbody and fire game
  callbacks. **We use a separate, throwaway Jolt world.**
- **Jolt is a static library linked PUBLIC into the shared `jasperengine` library**
  (`engine/CMakeLists.txt:1075-1083`), and the editor links `jasperengine`
  (`CMakeLists.txt:786-794`). Calling Jolt from editor code would use a second static copy with
  its own factory and allocator, which risks crashes on Windows and macOS. **Every `JPH::` call
  lives inside `libjasperengine`, behind a `JASPERENGINE_API` header that exposes no Jolt
  types.**
- **The shape builders already exist**, in an anonymous namespace in `PhysicsSystem.cpp`:
  - `collectMeshSourceViews`, `meshSourceEntities`, `meshToBodyMatrix`
  - convex hull (`:1181`), triangle mesh (`:1231`), heightfield (`:1320`)
  - `ensureJoltGlobalInit`, the shared temp allocator and the job system

  Mesh CPU data stays resident (`Buffer::data`; `copyToOwnData` when the model goes away), and
  so do terrain heightmaps (`setReleaseDataAfterLoad(false)`).
- **There is no convex decomposition** (no V-HACD or CoACD). Jolt has `ConvexHullBuilder`;
  `meshoptimizer` is vendored.
- **The editor already has the pieces the tool needs:**
  - Gizmos as scene objects in `ToolsLayer`; tools in `MainToolbar` (Q/W/E/R/T/X).
  - Mouse routing `SceneRender::mouse{Click,Drag,Release}Event`, with an early-out pattern
    already used by the Terrain tool.
  - Overlay `LineRenderer`s (`overlay()`, `LineDrawUtils`).
  - Undo: `ObjectTransformCmd` (records prefab overrides), `MultiPropertyCmd`,
    `CommandHistory::addCommandNoMerge`.
  - Per-project user settings: `workspace.yaml`, like `TerrainEditorSettings`.
  - Cancel hooks: `App::stopTransientPreviews` on save and play, and `prepareForProjectSwitch`.
- **Rendering is on demand.** A running simulation must call `requestRedraw()` every frame.
- **MCP:**
  - There are 141 tools against a budget of fewer than 150, so this feature adds **one** tool.
  - `McpInstructions` is capped at 16 KB and nearly full, so it gets one line.
  - The Texture Synthesis feature (`17b65023e`) is the full-stack template: core → session →
    window → MCP job tool → CLI → docs → C++, MCP and CLI tests.

## 3. What Grabbit does and what we keep

### 3.1 Grabbit's modes (all act on the selection)

| Grabbit | Key | Behaviour |
|---|---|---|
| Place | U | A position handle pulls the bodies toward it (velocity = Δ/dt, clamped); they slide along and stop at contacts. They teleport past a distance threshold. They optionally keep their relative layout. |
| Rotate | I | A rotation handle spins each body toward the same absolute orientation about its own centre. A huge linear drag pins its position. |
| Align | O | A scale handle: inward gathers the bodies on an axis, outward spreads them. |
| Fall | P | Gravity on the selection only, while Space is held. A gizmo sets the gravity direction and strength. |
| Point | [ | Experimental. Pulls the selection to the surface under the cursor while Space is held, with an optional align-to-normal. |

Common to all modes:
- **Unselected objects:** they get static triangle-mesh colliders.
- **Selected objects:** they get a convex hull, or a V-HACD decomposition (cached per mesh
  asset).
- **Authored colliders:** reused under four modes: existing only / generated when none exist /
  generated only / both.

### 3.2 Keep

- The control law: kinematic-feeling velocity targets with a contact-aware speed cap and a
  contact-normal factor.
- The teleport threshold with a visible marker.
- Keeping a multi-selection's formation.
- Hold-to-simulate gravity as a dead-man switch.
- A mode/status overlay.
- Labels that say *why* an object can't be moved.
- Distance-to-surroundings ray indicators.
- Zero setup: no components are added to the scene.
- One undo step per gesture.
- A cached decomposition per mesh.
- The four collider-source rules.

### 3.3 Fix

| Grabbit problem | Our answer |
|---|---|
| Scans the **whole scene** on activation, adds components to every renderer, and makes every Rigidbody kinematic. | Separate world. Context is gathered **lazily** in a region around the movers' swept bounds and grows as the drag moves. The scene is never touched until commit. |
| Settings need a tool restart. | Settings apply live. |
| About 30 raw PhysX knobs. | A **Feel** preset (Precise / Balanced / Fast) plus 4 everyday controls; the rest go under Advanced. |
| Space toggles in some modes and must be held in others, and the docs disagree. | One rule: **hold Space = drop under gravity**, in any physics mode. |
| Release in Place, Rotate or Align freezes objects in mid-air. | **Settle on release** (default on): gravity runs until the bodies rest, then commits. |
| Rotate turns every body to the same absolute orientation about its own centre. | Rotates the group rigidly about the selection centre, with a per-object option in the panel ("Rotate about: Group centre / Each object"). |
| Euler-as-rotation-vector angular law (wrong for large angles); timestep counted twice; velocity law ignores Speed. | `2·log(q)` shortest-arc angular law, one fixed-step accumulator, velocity recomputed every substep. |
| One global "collision found" cap that depends on handler order; the normal factor is inverted. | A per-body contact cap; motion *into* the contact normal is slowed, sliding *along* it is not. |
| Children become separately welded bodies with FixedJoints and are left out of undo. | One compound body per selected root; children are merged into it. |
| V-HACD blocks the editor; Cancel is cosmetic; the cache is keyed by asset reference and goes stale on re-import. | Decomposition runs as a background job with a real cancel. A single hull is used meanwhile and swapped for the decomposition when ready. The cache is keyed by a content hash. |
| Mouse only. | One MCP tool plus a CLI command, deterministic, with preview then apply (section 8). |
| Code defaults that block every object (`maxBoundAxisLength = 0`), plus a 100-voxel resolution. | Sane defaults, enforced by tests. |

### 3.4 New, beyond Grabbit

- **Push neighbours** (off by default): unselected *movable* objects inside the region are
  dynamic too, so the selection can knock them. An object is movable if it has a Rigidbody, or
  is small and is not a static root.
- **Separate**: resolves overlaps with no gravity and no drag. This is "push these apart so they
  don't intersect".
- **Snap** (Grabbit's Point, made solid): Shift-drag the selection across surfaces under the
  cursor. It rests on the surface and can align to its normal.

## 4. Concepts and naming

- **Physics Placement**: the feature name in the UI, docs and code (`PhysicsPlacement*`). It is
  not "Grabbit".
- **Session**: the lifetime of a placement world. It starts when the tool activates or an MCP
  call runs, and ends when the tool deactivates or the call finishes.
- **Movers**: the bodies being placed. These are the top-most selected roots: a selected child
  of a selected parent is dropped. Each mover is one dynamic body whose shape is the compound of
  its subtree's colliders or meshes.
- **Context**: the static environment within the **region**, which is the movers' swept AABB
  plus a margin (default 2 m, or 1× the mover's size if larger).
- **Proxy**: the Jolt body or shape built for an entity. Proxies are cached for the session,
  keyed by entity, mesh content hash, world scale and collider settings.
- **Gesture**: one drag, one Space hold, or one MCP call. Each gesture is one undo step.

## 5. Architecture

Four layers. One simulation core serves the human tool, MCP and the CLI.

```
engine/core/physics/                       (inside libjasperengine; only place that calls JPH::)
  JoltShapeBuilder.{h,cpp}   extracted from PhysicsSystem.cpp, no behaviour change
  PlacementWorld.{h,cpp}     JASPERENGINE_API, no JPH types in the header
  ConvexDecomposition.{h,cpp} V-HACD 4 wrapper + content-hash cache          (phase 3)
editor/physics/
  PlacementSession.{h,cpp}   spec -> world -> run -> commit; shared by tool/MCP/CLI
  PlacementCli.{h,cpp}       `jasperengine-editor place-physics`
editor/window/
  PhysicsPlacementWindow.{h,cpp}   settings panel + scene interaction (Terrain-tool pattern)
editor/mcp/tools/
  PlacementTools.cpp         `place_with_physics`
```

### 5.1 `JoltShapeBuilder` (phase 1, refactor only)

Move these out of `PhysicsSystem.cpp`:
- `ensureJoltGlobalInit`, `sharedPhysicsTempAllocator`, `sharedPhysicsJobSystem`
- `collectMeshSourceViews`, `meshSourceEntities`, `meshToBodyMatrix`
- the hull, mesh and heightfield builders

Each builder takes engine types and returns a `JPH::ShapeRefC`. The header is engine-internal,
not installed. `PhysicsSystem` calls these functions, so runtime behaviour is unchanged; the
existing play-mode physics is the regression test.

While extracting, also fix the heightfield size: `scale = size/N` should be `size/(N-1)`, the
last texel should be sampled, and the entity scale applied. That correction belongs in its own
commit.

### 5.2 `PlacementWorld` (engine, public)

```cpp
namespace jasperengine {
struct PlacementBodyDesc  { Entity entity; ColliderPolicy policy; bool dynamic; };
struct PlacementPose      { Vector3 position; Quaternion rotation; };          // world space
struct PlacementBodyState { Entity entity; PlacementPose pose; Vector3 v, w; bool asleep;
                            std::vector<Entity> restingOn; float maxPenetration; uint32_t contacts; };

class JASPERENGINE_API PlacementWorld {
public:
    explicit PlacementWorld(Scene* scene, const PlacementWorldSettings&);
    ~PlacementWorld();
    // Movers and context
    bool addMover(Entity root, ColliderPolicy, std::string* why);    // false + reason when refused
    void ensureContext(const AABB& region, const ContextFilter&);   // incremental, idempotent
    // Drive
    void setGravity(Vector3);
    void setTarget(Entity, const PlacementPose&, TargetMode);       // follow / none
    void teleport(Entity, const PlacementPose&);
    void step(float dt);                                            // one fixed step
    // Read back
    PlacementBodyState state(Entity) const;
    bool allAsleep() const;
    bool raycast(const Ray&, RayHit&, const std::vector<Entity>& ignore) const;  // triangle-accurate
    uint64_t stateHash() const;                                     // determinism check
};
}
```

**World setup**
- Own `JPH::PhysicsSystem`. Two object layers (`STATIC`, `MOVING`) and two broadphase layers,
  using Jolt's table filters. It does not use the project's layer matrix; the context filter
  handles layers.
- Body limit 4096. Reuse the shared job system and temp allocator, stepping on the main thread.

**Movers**
- `EMotionType::Dynamic`, `EMotionQuality::LinearCast`, gravity factor driven by mode.
- Shape: a `StaticCompoundShape` of the subtree's parts, with world scale baked into the
  vertices.
- Mass from shape volume, density 1.
- `mMaxConvexRadius` scaled by object size, so thin objects such as pencils and screws keep
  their shape.

**Context**
- **Static bodies, per source:**
  - authored Rigidbody colliders: rebuilt from the components, not taken from the paused
    world's possibly stale bodies;
  - plain meshes: `MeshShape`, simplified with `meshopt_simplify` above 20k triangles;
  - terrains: `HeightFieldShape` built through `MeshSystem::sampleTerrainSurface`, so it
    matches the rendered surface.
- `ensureContext` queries entities by `worldAABB` and adds only the ones not already in the
  world. It is cheap to call every frame with the current swept region.
- With Push neighbours on, the movable entities in the region become extra dynamic bodies.
  They are reported in results and committed like movers.

**Collider policy** (Grabbit's four rules, renamed, plus Auto)

| Policy | Mover gets | Context gets |
|---|---|---|
| `auto` (default) | Authored colliders if any; otherwise a fitted primitive when the fit error is under 5% (box, sphere or capsule; an OBB for flat meshes); otherwise a decomposition if the cache has one and the hull covers it poorly; otherwise a convex hull | Authored colliders if any; otherwise the triangle mesh |
| `authored` | Authored colliders only; refused if there are none | Same |
| `generated` | Ignores authored colliders | Same |
| `both` | Union of the two | Same |

### 5.3 Convex decomposition (phase 3)

- **Library:** V-HACD 4.x single header (`VHACD.h`, BSD-3), vendored under
  `engine/libs/vhacd/` with its LICENSE, and listed in `NOTICE.md`.
- **Run:** `CreateVHACD_ASYNC` on a worker. Cancel is real. Return codes are checked.
- **Cache:** `.jasperengine/cache/colliders/<xxh64(vertices, indices, params, algoVersion)>.bin`
  holding float3 hull points per hull. Project-local; `.jasperengine/` is already in the
  generated `.gitignore` (`Project.cpp:5257`).
- **Invalidation:** none needed; a content hash never goes stale.
- **Inputs:** UV-seam vertices are welded before hashing and decomposing.
- **Defaults:** max hulls 8, resolution 400 000, max vertices per hull 64. User-facing as
  *Quality*: Low / Medium / High.
- **Never blocks:** a mover starts with its convex hull. When the job lands, the session swaps
  the shape between steps; the HUD shows "Refining colliders 2/5".
- **Explicit bake:** buttons in the panel ("Bake selection", "Bake scene") and a `bake`
  parameter on the MCP tool.
- **CoACD** (MIT, better on rings and containers but slower) is listed as a possible future
  "High quality" backend. It is not in scope.

### 5.4 `PlacementSession` (editor, shared)

Modelled on `SynthesisSession`.

**Inputs**
- `PlacementSpec`:
  - scene id, movers, mode, gravity, settings, seed, jitter, limits;
  - `context` (`auto` / an entity list / `none`).
- `checkSpec(spec) -> errors`.

**Run**
- `run(spec, progressCb, cancelToken) -> PlacementResult`. It runs on the main thread in
  bounded slices (at most 4 ms per frame), so the editor stays responsive. For the CLI it runs
  straight through.
- Determinism:
  - a fresh world per call;
  - movers inserted in entity-path order;
  - a fixed dt (1/120 s);
  - the seeded jitter is applied before the first step;
  - no wall-clock input;
  - poses rounded to 1e-5 in results.
- The guarantee is "same binary, same platform, same arguments → same `state_hash`". Cross-
  platform determinism would need `JPH_CROSS_PLATFORM_DETERMINISTIC` and is out of scope.

**Commit**
1. Restore any live preview poses.
2. Build one `ObjectTransformCmd` holding every moved entity. This needs an
   `addEntity(entity, pos, rot, scale)` overload. World poses are converted to the parent's
   local space.
3. `runSceneCommand` (MCP) or `addCommandNoMerge` (tool): one undo step, prefab overrides
   recorded.

**Guards**
- Before committing, check the scene still exists and its `structureVersion` hasn't changed.
  Otherwise fail with "scene changed during simulation".
- Refused in play mode.
- Cancelled by `stopTransientPreviews` (save or play), by a project switch, and by Ctrl+Z
  during a gesture.

## 6. Simulation behaviour

This is the per-step control law for a mover that has a target (Move, Rotate, Gather/Spread,
Snap). It is clean-room and derived from observed behaviour, not from Grabbit's code.

```
every fixed step (dt = 1/120):
  for each mover m with target (p*, q*):
      e_p = p* - m.p                         // formation offset already folded into p*
      if |e_p| > teleportDistance: teleport m to p*; continue   // marker shown at that radius
      v = e_p / dt
      w = 2*log(q* * conj(m.q)) / dt         // shortest arc (flip q if w<0)
      cap = maxSpeed
      if m.touchingContext:                  // from Jolt contact listener add/remove, per body
          n = m.averageContactNormal
          into = max(0, -dot(normalize(v), n))
          cap = lerp(maxSpeed, contactSpeed * sizeFactor(m), into)   // slow only *into* contacts
      m.setVelocities(clampLen(v, cap), clampLen(w, maxAngularSpeed))
  world.step(dt)
```

**Group rotation**
- A rotation R about pivot c gives each mover target `p* = c + R(p0 - c)` and `q* = R q0`.
  The pivot is the selection centre. The editor has no pivot setting today, so the panel
  carries its own: "Rotate about: Group centre / Each object".
- With "Each object", each mover gets `q* = R q0` about its own centre.

**Gather/Spread.** For each axis the handle scale s is in [0, 2]:
`p*_axis = c_axis + s·(p0_axis − c_axis)`. Then the same law applies, so objects stop when
they touch. The panel also has buttons:
- **Gather to centre**
- **Distribute evenly along X/Y/Z**: targets are spaced equally between the extremes, keeping
  order.

**Drop (hold Space)**
- Gravity is on: the default is (0, −9.81, 0); the panel sets direction, strength, and
  "toward the surface under the cursor".
- Drag 0. No targets.
- Bodies keep their momentum.

**Settle** (after a release, or in MCP `mode: drop`)
- Gravity runs until every dynamic body is asleep: linear speed below 0.02 m/s and angular
  speed below 2°/s for 0.25 s, Jolt's own sleep with these thresholds.
- Stops at `maxSettleSeconds` (default 4 s interactive, 10 s for MCP).
- Bodies below `killY` (default region min − 50 m) are frozen and reported as fell-out.

**Separate.** No gravity, no targets, high damping. Jolt's penetration recovery with
`mMaxPenetrationDistance` raised steps until no pair overlaps more than 1 mm, or the time limit
hits.

**Feel presets** (the panel's first control)

| | Max speed | Contact speed | Substeps | Solver iterations (velocity / position) |
|---|---|---|---|---|
| Precise | 4 m/s | 0.3 m/s | 2 | 10 / 4 |
| Balanced (default) | 10 m/s | 1 m/s | 1 | 8 / 2 |
| Fast | 25 m/s | 3 m/s | 1 | 4 / 1 |

These are starting values; tune them against the fixture scene. Every preset value can be
overridden under Advanced.

## 7. Human UX

### 7.1 Activation: physics is a modifier on the tools you know

Grabbit adds five new single-key modes. We reuse Unity muscle memory instead:

- **G** (or a new toolbar button after Rect, `ICON_FA_MAGNET`) toggles
  **Physics** on or off. In code this is `MainToolbar::physicsMode`, kept separate from
  `GizmoSelected`.
  - While on, the toolbar button is lit and the Scene view shows an orange **PHYSICS** badge.
    Move, Rotate and Scale gizmos turn orange.
  - G is free today (`MainToolbar::handleShortcuts`). It's Blender's "grab" key, and the same
    filters apply: not while typing, not with modifiers, not with RMB held.
- **With Physics on:**

| Key / tool | Physics behaviour |
|---|---|
| W, Move | Physics Move: drag the handle; the selection follows, slides along and stops at contacts. |
| E, Rotate | Physics Rotate: rotates rigidly about the pivot and stops at contacts. |
| R, Scale | Gather / Spread: per-axis scale-handle drag; inward gathers, outward spreads. |
| Hold Space | Drop: gravity on the selection, plus neighbours if Push neighbours is on. Release Space to stop; that is one undo step. |
| Shift + LMB-drag on a selected object | Snap: the object rides the surface under the cursor, resting on it, aligned to its normal if "Align to surface" is on. |
| Esc during a gesture | Cancels the gesture and restores the poses from before it. |
| Q, T | Unchanged (Hand; Rect is 2D). |

- **2D scenes:** Physics is disabled in phase 1-3, and `toolSupported` greys it out.
- **Shortcut documentation:** list G, Space and Shift-drag in Help > Keyboard Shortcuts
  (`App.cpp:911-943`). Fix the existing "T Toggle local/global" line there while at it; it's
  actually X.

### 7.2 What the user sees

- **HUD** (top-left of the Canvas, `GameWindow::drawStats` style):
  - `PHYSICS · Move` plus a status line: `Ready`, `Simulating`, `Settling…`, `Resting`, or
    `Refining colliders 2/5`;
  - body counts: `3 movers · 41 context`;
  - an always-visible hint row: `Space drop · Shift-drag snap · Esc cancel`.
- **Region:** a faint wire box of the context region, shown only while simulating.
- **Contacts:** small dots, coloured by whether the mover is being slowed by them.
- **Teleport ring:** a dashed circle at `teleportDistance` around the movers' centre. It turns
  red when the handle passes it.
- **Ghost:** a thin outline of each mover's start pose during a gesture, so a cancel is
  predictable.
- **Refused objects:** a floating label with the reason, e.g. "Physics: no mesh or collider",
  "too large (> 25 m), treated as environment", "ignored layer", "mesh still loading". This is
  Grabbit's best idea; keep it.
- **Distance indicators** (Grabbit's axis rays, opt-in from the Gizmos menu): rays along ±X/±Y/±Z
  from the idle selection to the nearest context surface, with distance labels.
- **Collider preview** (Gizmos menu "Physics proxies"): wireframes of the generated hulls and
  decompositions, drawn with `createOrUpdateBodyLines`.

### 7.3 The panel: Window > Physics Placement

This is a `TerrainEditWindow`-style panel. It also opens from a ▾ next to the toolbar button.
Settings apply live and persist per user in `workspace.yaml` (`PhysicsPlacementSettings` next to
`TerrainEditorSettings`).

```
Feel            [ Precise | Balanced | Fast ]
Settle on release   [x]        Keep formation [x]      Push neighbours [ ]
Gravity         (0,-9.81,0)  [Down] [Toward surface]   Strength ──●── 9.81
Colliders       Auto ▾        Quality  Medium ▾        [Bake selection] [Bake scene]
Snap            Align to surface [x]   Offset 0.00
Gather/Spread   [Gather] [Distribute X] [Distribute Y] [Distribute Z]
Region margin   2.0 m          Ignore layers ▾
▸ Advanced      max speed, contact speed, max angular speed, teleport distance, substeps,
                solver iterations, sleep thresholds, max settle seconds, kill Y
Cache           312 hulls · 1.4 MB  [Clear]
```

### 7.4 Undo model

- One gesture (drag, Space hold, Snap stroke, Gather/Distribute click) is one undo step,
  committed on release. When Settle on release is on, the commit waits for the settle to
  finish.
- Ctrl+Z during settling cancels the settle and restores the pre-gesture poses.
- There is no session-level commit; leaving the tool keeps everything. This matches how the
  normal gizmos behave.

## 8. Agent UX

### 8.1 MCP tool: `place_with_physics`

One tool (verb-first, snake_case, budget-friendly), `activity: Edit`,
`allowedWhilePlaying: false`.

**Parameters**

| Parameter | Type / default | Meaning |
|---|---|---|
| `scene` | sceneRef, default the selected scene | |
| `entities` | entity refs, **required** | The movers. `use_selection: true` is the explicit alternative. |
| `mode` | `drop` (default) \| `separate` \| `move` \| `snap` \| `gather` \| `distribute` | |
| `target` | `{position?, rotation?, delta?, space: world\|local}` | Required for `move`; the group moves rigidly toward it and stops at contacts. |
| `ray` | `{origin, direction}` or `{screen: [x, y]}` | Required for `snap`. |
| `axis` | `x\|y\|z` | For `distribute`. |
| `amount` | number | For `gather`. |
| `gravity` | vec3, default [0, −9.81, 0] | |
| `settle` | bool, default true for drop/move/snap | Run until resting after the gesture. |
| `context` | `auto` \| entity refs \| `none` | |
| `context_margin` | metres | |
| `ignore_layers` | layers | |
| `push_neighbours` | bool, default false | |
| `colliders` | `auto` \| `authored` \| `generated` \| `both` | |
| `decompose` | `never` \| `cached` (default) \| `always` | `always` bakes before simulating. |
| `jitter` | `{position, rotation_deg}` | Random offset applied before the first step. |
| `seed` | int, default 1 | |
| `max_seconds` | default 10 | Simulated time, not wall time. |
| `feel` | `precise\|balanced\|fast` | |
| `apply` | bool, **default false** | Preview by default. |
| `screenshot` | bool or options | Scene view, auto-framed on the movers. |
| `wait`, `timeout_seconds` | standard job options | |

**Preview then apply.** `apply: false` simulates and reports without touching the scene. The
same arguments with `apply: true` commit exactly the previewed result: same seed, same
`state_hash`. This mirrors `synthesize_texture` without `output`.

**Result (`structuredContent`)**

```json
{
  "applied": false, "mode": "drop", "seed": 1, "state_hash": "9f2c…",
  "sim_seconds": 2.35, "steps": 282, "all_settled": true,
  "entities": [
    { "id": 41, "name": "Crate", "path": "Props/Crate",
      "before": {"position": [0,3,0], "rotation": [0,0,0,1]},
      "after":  {"position": [0.12,0.5,-0.03], "rotation": [0.01,0.2,0,0.98],
                 "local": {"position": [...], "rotation": [...]}},
      "moved_m": 2.51, "rotated_deg": 23.4, "settled": true, "settle_step": 270,
      "resting_on": [{"id": 7, "path": "Room/Table"}], "max_penetration_m": 0.0008,
      "collider": "decomposed(6)", "warnings": [] }
  ],
  "context": {"bodies": 41, "region": {"min": [...], "max": [...]}},
  "refused": [{"path": "Room", "reason": "too large (> 25 m), used as environment"}],
  "warnings": ["Pebble#3 never came to rest within 10 s"],
  "hint": "All 30 settled on Terrain; re-run with apply:true to commit (one undo step)."
}
```

**Diagnostics** go into `warnings` and `hint`, worded the way `assess()` does it. Each one says
what to do:
- an initial overlap deeper than 5 cm → "use mode:separate first";
- no context found under the movers → "nothing to land on within 2 m; raise context_margin";
- fell out of the world, never slept, or teleported;
- a mover's mesh is still loading.

**Long runs**
- A job (`ctx.jobs().create`) with progress text such as "step 140/1200, 22 of 30 asleep".
- `cancel_job` support.
- `replyWhenJobDone`, so short runs return inline.

**Errors.** Fix-it `isError` results (unknown entity, missing `target` for move, play mode
active), in the existing MCP style.

**Scatter is composition, not a new tool.** One `batch` with `undo_group: true`:
`instantiate_prefab` ×N (or `sculpt_terrain place_object`), then
`place_with_physics {mode: drop, jitter, seed, apply: true}`.

**Documentation**
- `docs/mcp/guide/physics.md` gets a "Placing objects with physics" section, and the guide index
  row is updated.
- One line goes into `McpInstructions.cpp`: "Drop/settle/separate objects in edit mode:
  place_with_physics (preview, then apply:true)".
- Regenerate `TOOLS.md` after adding a `gen_tool_docs.py` MODULES entry.

### 8.2 CLI: `jasperengine-editor place-physics`

```
jasperengine-editor place-physics --project P --scene S --entity Props/Crate [--entity ...]
    [--mode drop|separate|move|snap|gather|distribute] [--gravity x,y,z] [--seed N]
    [--max-seconds S] [--jitter POS,DEG] [--save | --dry-run] [--format text|json] [-q]
```

- Built on the headless `runSceneDump` setup with `setAsyncLoading(false)`.
- Prints the same JSON as the MCP result.
- Exit codes: 0 ok, 1 failure, 2 usage, 3 ran but not settled. This serves CI checks and agents
  that run without an editor.

### 8.3 No Lua hook

The engine has no editor-scripting Lua API. MCP plus the CLI cover agents, and adding a Lua
binding namespace for one feature is not worth it.

## 9. Edge cases

- **Prefab instances:** commit goes through `ObjectTransformCmd`, which records overrides. In
  Prefab Mode the session uses the prefab stage's scene.
- **Skinned meshes:** use the bind pose and warn. `InstancedMeshComponent` instances are not
  movers in v1; they are refused with a reason. As context they use their bounds as boxes.
- **Joints:** movers that have joints are simulated free, with a warning. Joints are not
  recreated in v1.
- **Huge or authoring-only objects:** a size limit (default 25 m on the longest axis). Larger
  objects become context, never movers, and the refusal label says so.
- **Empty or meshless roots:** use the union of the children's shapes. Refused if there are
  none.
- **Parenting:** results are committed in parent-local space. A mover whose parent is also
  selected is dropped from the movers.
- **Scene edits during a session:** if `structureVersion` changes, the world is rebuilt lazily
  before the next gesture.
- **Save, Play, project switch:** cancel the in-flight gesture through `stopTransientPreviews`
  and `prepareForProjectSwitch`.
- **2D scenes:** deferred to a later phase. Box2D v3 has a 128-world cap, so the throwaway world
  must be destroyed after use, and polygons are limited to 8 vertices.

## 10. Testing

- **C++ (`editor/tests/PhysicsPlacementTest.cpp`, plus engine tests for `PlacementWorld`):**
  - a box dropped onto a plane rests at the expected height (±1 mm);
  - a sphere on a ramp rolls off it;
  - a heightfield matches the rendered terrain height at 20 sample points;
  - Separate leaves no pair overlapping by more than 1 mm;
  - group rotation keeps relative poses;
  - a torus dropped over a cylinder peg ends up encircling it (decomposition, phase 3) and
    rests on top (hull only);
  - same arguments give the same `state_hash`;
  - the shape-builder refactor causes no play-mode physics regression.
- **MCP (`tools/mcp/tests/test_place_with_physics.py`):**
  - the schema;
  - the table of fix-it errors;
  - a preview leaves the scene byte-identical;
  - apply matches the preview pose for pose;
  - one undo restores everything;
  - prefab overrides are recorded;
  - job progress and cancel;
  - refused during play;
  - a screenshot is returned;
  - scatter with `batch`, `undo_group`, `jitter` and `seed`.
- **CLI test:** `--dry-run` JSON equals the MCP preview; `--save` writes the scene; exit code 3
  when a body doesn't settle.
- **Fixture project** `tools/fixtures/physics-placement/`, made only from engine primitives:
  - Table + crate: box shapes, open-top crate built from 5 boxes;
  - Ring on peg: torus + cylinder;
  - Bowl: lathe or decomposed sphere half;
  - a 20-box stack; thin plates (paper or pencils);
  - 30 small pebbles scattered over a terrain;
  - Ramp + spheres at 0.35 / 0.66 / 1.5 scale;
  - a large room that exceeds the size limit.

  The scenarios mirror Grabbit's demo, but the content is ours.

## 11. Phases

1. **Agent-first core (no UI):**
   - `JoltShapeBuilder` extraction and the heightfield fix;
   - `PlacementWorld` (hulls, primitives, mesh and heightfield context);
   - `PlacementSession`;
   - the `place_with_physics` tool with `drop`, `separate` and `move`, plus the CLI;
   - docs, tests and the fixture.

   This is the smallest slice that is useful and fully testable, and it de-risks the
   simulation before any mouse work.
2. **Interactive tool:**
   - Physics toggle (G), Physics Move/Rotate, hold-Space Drop, Settle on release;
   - HUD, ghost, teleport ring, refusal labels;
   - the panel, settings persistence, undo per gesture, cancel hooks.
3. **Quality:**
   - V-HACD 4 decomposition with the background job, content-hash cache and bake buttons;
   - Gather/Spread and Distribute;
   - Snap (Shift-drag), with the `snap`, `gather` and `distribute` MCP modes;
   - Push neighbours;
   - distance indicators and the collider preview.
4. **Optional:** a 2D variant on Box2D, and a CoACD "High quality" backend.

## 12. Decisions

1. Physics is a modifier toggled with G on top of W/E/R (no separate tool with sub-modes).
2. Settle on release defaults to on: in a physics tool, people expect released objects to fall
   and rest, not to hang in mid-air. It can be turned off in the panel.
3. V-HACD 4 is accepted, in the desktop editor only (never compiled into exported games).
4. All phases were built together; the interactive tool came first in priority, then the agent
   API.

## 13. Implementation notes (where the build refined the design)

- MCP tool `place_with_physics`; an `apply:true` call with the same arguments on an unchanged
  scene commits the stored preview (`from_preview: true`), so apply always equals the preview.
  Any edit to the scene during a run (`placementSceneEditStamp`) fails it; nothing is committed.
  Runs wait while a Scene-view gesture is busy (`placementSceneBusy`).
- Batch `move` never teleports (it stops at walls); `snap` places the movers on the surface the
  ray hits, then settles (nothing in between stops it); interactive Shift-drag snap hops onto the
  target when blocked or far.
- Settled means asleep and touching something under gravity; nothing sleeps in mid-air. Every
  dynamic body is capped at the feel's max speed; a driven mover passes through what it starts
  inside until they part; sunk bodies are lifted out; rolling resistance stops balls on slopes
  under about 11 degrees.
- Shapes follow the full model matrix (non-uniform parent scale, mirroring); thin bodies get a
  relative hull tolerance and more solver iterations.
- Hull cache files are `<key>.hulls` under `.jasperengine/cache/colliders`; movers' decompositions
  are urgent (ahead of bakes). V-HACD runs multithreaded (deterministic, bounded thread count).
- One finalise path for save, prefab save, auto save, Play and project switch: a released gesture
  settles and commits first, a held one is cancelled. Every undo, redo and command passes through
  `CommandHistory::beforeChange`, so a busy gesture is finalised or cancelled first.
- CLI `place-physics` builds exactly the run the MCP tool builds; `--decompose cached` fills the
  project cache for later runs (bounded by `--decompose-timeout`).
- Fixed along the way in shared code: heightfield colliders now match the rendered terrain, thin
  box/cylinder colliders build, mirrored mesh colliders collide in play mode, and Play/Stop keeps
  editor overlay entities created during Play.
