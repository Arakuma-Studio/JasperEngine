# S26 — Scene view: triangle picking (BVH), root-then-drill, click cycling, surface drop, camera bookmarks, ortho toggle, quad views, measure/rulers, layer filter, camera popup

Wave 3. Depends on S01 (C++20), S02 (gtest, benches), S06 (job system), S07 (`Changed<Transform>`),
S24 (selection undo). Uses S03 (atomic writes), S04 (GUIDs), S05 (reload events) and S25 (rebindable
shortcuts). Read `README.md` first.

## 1. Goal

Clicking in the Scene view selects the object under the cursor, not the object whose bounding box
happens to be nearest. That needs triangle-exact picking through cached per-mesh BVHs under a
scene-level BVH. On top of the new pick list we add Unity's selection flow: the first click selects
the prefab or model root, the next click drills in, and repeated clicks walk back through overlapping
hits. Drops land on the real surface. The same BVH is a physics-free `MeshRaycast` for Lua at
runtime. The rest of the stream brings the Scene view up to Unity level for 3D level design: camera
bookmarks, a persp/ortho toggle, quad views, measure and rulers, a Layers filter, per-entity
hide and pick lock, and a camera settings popup. The 2D and UI authoring behaviour we already have
stays as it is.

## 2. Scope

In scope:
- Engine module `engine/core/picking/`: `MeshBvh` (adapted from OpenEngine, BVH8 with
  AVX2/NEON/scalar), `SceneBvh` (binned-SAH build, refit and ray/frustum traversal over entity
  bounds), `MeshRaycast` (the narrow phase for every pickable kind), CPU posing for skinned and
  morphed meshes, the Lua API and a bench.
- Editor: `findBestEntityByRay` replaced by the new pick pipeline; pick roots and drill-down;
  click cycling with an "n/m" pill; surface drop with an Align-to-Normal option; camera bookmarks
  with hover previews; persp/ortho toggle with view-gizmo axis clicks; Alt+F Frame All; Single,
  2-split and Quad layouts; measure tool and rulers; Layers dropdown (visible and pickable);
  per-entity scene visibility and pick lock in the Hierarchy; camera settings popup; MCP tools;
  docs.

Out of scope:
- GPU ID-buffer picking, which would need readback and break the "no compute or readback in the
  pick path" mobile rule. Physics-collider picking: colliders are not what the user sees.
- Alpha-tested picking of sprites, foliage cards and cutout materials. Those pick by their
  triangles (see §9).
- Hover pre-selection highlight, the mark-up tool, spline placement (S18) and the terrain and
  nav-grid brushes (S19). They can reuse `MeshRaycast` later.
- Their `SceneBvh` threaded BLAS and GPU traversal (`ThreadedBvh*`, ported from speedball-gi), their
  ECS-coupled TLAS request/defer machinery, sectors and `RenderExtractionSystem`, and their UI-widget
  code (README do-not-copy: their editor UI toolkit). We port algorithms and UX, not widgets.

## 3. Current state (verified)

- **Picking.** `editor::Project::findBestEntityByRay` (`editor/Project.cpp:6134-6178`) loops over
  every pickable entity, `getPickableEntities` (`:6214`).
  - Terrain is exact (`MeshSystem::raycastTerrainSurface`, `:6151`). Everything else is
    `ray.intersects(getEntityWorldAABB(...))` (`:6165-6170`).
  - `Ray::intersects(AABB)` returns distance 0 when the origin is inside the box
    (`engine/core/math/Ray.cpp:101-103`). A room or a sky dome around the camera therefore wins
    every click, and nothing behind it can be reached.
  - 3D keeps the nearest hit. 2D and UI keep the last-drawn one (higher Transform index, `:6157`).
  - Layer scenes (inline child scenes, a running game's layers) are walked top-down by
    `findObjectByRay` (`:6312-6355`) and `getSceneViewLayers` (`:6223`). Rect select tests the
    projected AABB corners (`selectEntitiesInRect`, `:6181`).
- **Entity bounds.** `getEntityWorldAABB` and `getEntityLocalAABB` (`Project.cpp:5943`, `:6046`) cover
  meshes (including sprites and tilemaps, which are `MeshComponent`s), UI rects, lights, cameras and
  audio. Icons are sized per view (`billboardScreenScale`). Occluders, points and lines are covered
  too, the last two with distance-scaled point boxes.
- **Pick target.** `SceneWindow::resolveStructureSelection` (`editor/window/SceneWindow.cpp:147-178`)
  maps a hit inside a collapsed Hierarchy node to its outermost collapsed ancestor. The Hierarchy
  feeds it through `setStructureSelectionParent` (`HierarchyWindow.cpp:2286-2296`). There is no
  drill-down and no cycling. Shift adds to the selection (`SceneWindow.cpp:192-222`).
- **CPU mesh data is retained.** `MeshComponent::buffer`, `indices` and `eBuffers`
  (`engine/core/component/MeshComponent.h:305-312`) are read on the CPU by
  `MeshSystem::calculateMeshAABB` (`MeshSystem.cpp:2505-2585`), which also builds per-bone bind
  bounds. Skinning and morphing run only on the GPU (`engine/shaderlib/includes/skinning.glsl`),
  with `bonesMatrix` and `morphWeights[8]` on the CPU (`MeshComponent.h:328,340`).
  `skinnedAABB` is the posed bound. Submesh index ranges are `Submesh::attributes[INDEX]`, with
  16-bit indices for generated meshes (`MeshSystem.cpp:735-748`).
- **Drop.** `getModelDropPosition` (`SceneWindow.cpp:360-385`) uses the hit entity's AABB entry
  point, or the terrain surface, or the y=0 plane. There is no normal and no alignment.
- **Camera.**
  - RMB+WASD/QE fly. Wheel during the fly changes `walkSpeedOffset`, shown on a gauge, and Shift
    triples the speed (`SceneWindow.cpp:1492-1540`). MMB orbits and pans, and the wheel zooms
    (`:1584`).
  - F frames the selection and Home frames all, in 3D only (`:1659-1666`). Numpad 1/3/7/9 snap
    to axis views (`:1669-1706`).
  - `handleViewportGizmoClick` (`:1850-1889`) snaps to six axis directions. `ViewportGizmoAxis`
    (`editor/render/gizmo/ViewportGizmo.h:14-22`) has no centre hit, so projection cannot be
    toggled there.
  - Every move is instant (`snapCameraToDirection`, `:1833`; `focusOnEntities`, `:1751`).
  - The 3D editor camera is always perspective, although `Camera::setOrtho` exists and
    `screenToRay` handles ortho (`engine/core/object/Camera.cpp:612-645`).
  - Near and far live in the Gizmos popup (`:2040-2061`). The defaults are in
    `editor/render/SceneRender.h:16-24`.
- **View state.** `Workspace::SceneState` (`editor/Workspace.h:25-29`) holds `displaySettings` and an
  `editorCamera` node (`Stream::encodeEditorCamera`, `editor/Stream.cpp:3268`: type, target, up,
  fov, clips, position, zoom, walkSpeed), keyed by scene path in `.jasperengine/user/`.
- **Missing entirely:** bookmarks, ortho in 3D, split views, measure, rulers, a Layers filter, an
  editor-only per-entity hide or pick lock (`Transform::visible` is game data; the Hierarchy's
  `isLocked` is prefab/model edit locking, `HierarchyWindow.cpp:2445`) and a mesh raycast in Lua
  (only `Physics.raycast`, `engine/core/registry/PhysicsLayers.h:94`; binding in
  `engine/core/script/binding/CoreClassesLua.cpp:554`). The render culling hook already exists:
  `RenderSystem::isLayerVisible(entity, camera.cullingMask)` (`RenderSystem.cpp:7949`).
- **MCP.** `set_scene_camera` and `frame_entity` (`editor/mcp/tools/ViewTools.cpp:1135,1158`),
  `select_entities` (`EntityTools.cpp:1223`). Nothing picks at a screen point.

## 4. Reference (OpenEngine, MIT, © 2026 Gamecrafters Guild Limited and contributors)

| What | Path (under `external/OpenEngine/`) | Take | Improve |
|---|---|---|---|
| Mesh BVH | `Engine/Modules/MeshPicking/Include/MeshPicking/{MeshBvh.h,PickTypes.h}`, `Source/MeshBvh.cpp` (1084 lines) | **Copy and adapt**: binned-SAH binary build, collapse to CWBVH8 with 8-bit quantized child boxes, AVX2/NEON/scalar 8-child slab test, Möller-Trumbore, brute force below 256 tris, serialize/deserialize | Their x86 path is selected at **compile time** (`MeshBvh.cpp:22-27`) and the TU is built with `-mavx2 -mfma` (`CMakeLists.txt:13-16`). That crashes on CPUs without AVX2. We dispatch at runtime (D3). Add cull-mode-aware hits and an any-hit/all-hits traversal. Swap their `Mathematics` and `Types` for ours. Drop `Core/CpuProfiler.h` in favour of S15 scopes. |
| SAH kernel | `Engine/Modules/SceneBvh/{Include/SceneBvh/BinnedSahSplit.h, Source/BinnedSahSplit.cpp}` (152 lines) | **Copy.** It is OE-authored, with no speedball header. One kernel shared by `MeshBvh` and `SceneBvh` | — |
| Scene TLAS | `Engine/Modules/Scene/Source/{TlasBuilder.cpp,TlasRefit.cpp,TlasTraverse.cpp,TlasInternal.h}`, tests `Tests/SceneTlasTests.cpp` | **Adapt**: binned build, bottom-up refit, ray/frustum traverse over `{AABB, leaf}`. Port the relevant test cases | Strip their World, sectors and async `RequestCurrent/WaitCurrent` (`Include/Scene/SceneTlas.h:133-164`). Refit synchronously at query time from S07 change tracking. |
| Pick service | `Apps/Editor/{Include,Source}/Picking/{MeshPickingService,MeshBvhCache,MeshBvhDiskFormat,PickRootResolver}.*` | Options and hit struct, pending-BVH policy, cache coalescing and invalidation, candidate builder (`PickRootResolver.cpp:126-151`) | They support bind pose only (`MeshPickingService.h:62`). We pose skinned and morphed meshes on the CPU (D5). Our prefab/model metadata replaces their subtree heuristic (`PickRootResolver.cpp:46-73`). |
| Click flow | `Apps/Editor/Source/SceneViewController.cpp:1695-1835` | Icons first, 4 px same-spot cycling, Ctrl/Cmd+wheel depth cycling with anchor, "selection inside instance means select the exact hit" | — |
| Drop | `Apps/Editor/Source/SceneView/SceneViewDropPlacement.cpp:60-136` | Ignore the preview's own entities, pending-BVH flag, along-ray fallback | We do not put physics first (D9). We add normal alignment. |
| Other UX | `UI/Controls/Widgets/CameraBookmarksWidget.cpp`, `Panels/SceneCameraSettingsPopup.{h,cpp}`, `Panels/SceneViewPanelQuad.cpp`, `UI/Controls/Widgets/SceneViewRulerOverlay.cpp:29-67` (nice step), `SceneViewMeasureOverlay.*`, `SceneView/SceneViewLayerFilter.cpp`, `SceneView/SceneViewProjection.cpp`, `Panels/HierarchyPanel.cpp:2062` (lock toggle) | Behaviour and constants only (9 slots plus default, 128×72 previews, ruler 64–130 px major-tick band, settings rows) | Our ImGui widgets. Measures are workspace data, not ECS entities (D12). |

Licences: every copied file keeps the OE MIT header and gets a `NOTICE.md` entry. Nothing comes from
`Modules/SceneBvh/{ThreadedBvh*,TlasPacker*}`, so the speedball-gi notice (`ThreadedBvh.h:5-7`) is
**not** needed. If any of that code is ever taken, its notice must be added.

## 5. Requirements

### Functional — picking core (engine)

- **R1 MeshBvh.**
  - `MeshBvh::build(const PickGeometry&)` takes positions, uint32 indices (16-bit indices and
    strips/fans are widened or expanded at gather) and the triangle-to-submesh map.
  - `raycast` returns the closest hit: triangle, submesh, barycentrics and t.
  - `raycastAll` returns every hit sorted by t, capped at 64.
  - Meshes under 256 triangles are brute-forced with no BVH. `LINES` and `POINTS` submeshes have
    no triangles and pick as bounds (R4).
- **R2 Exactness.** For random rays the BVH result equals brute force (same triangle, `|Δt| ≤ 1e-5·t`).
  This holds for every SIMD path, and AVX2, NEON and scalar give bit-identical hit sets.
- **R3 Faces.** A triangle is hit from a side the renderer draws: the submesh cull mode
  (`MeshComponent::cullingMode`, `faceCulling`, `windingOrder`), with double-sided submeshes hit
  from both sides. A camera inside a back-face-culled room therefore picks what is behind the wall.
  Runtime option `hitBackfaces` (default false, as Unity's `queriesHitBackfaces`).
- **R4 Pickable kinds and narrow phase:**
  - `MESH`: triangles in mesh-local space. Sprites and tilemaps are meshes, so gaps between tiles
    miss.
  - `INSTANCE`: an `InstancedMeshComponent` instance. It reuses the shared mesh BVH through the
    instance matrix and reports `instanceIndex`.
  - `TERRAIN`: `raycastTerrainSurface`.
  - `UI`: the RectTransform rect plane (the current rule).
  - `BOUNDS`: points, lines, particle systems (simulated bounds, never per particle), 2D
    occluders, and meshes with no CPU data or whose BVH is pending.
  - `ICON`: lights, cameras, audio and probes, tested in screen space (radius 10 px × overlay
    scale) before any geometry, because they draw on top.
- **R5 LOD.** Picking always uses LOD0 indices, never `Submesh::lodIndices`.
- **R6 Skinned and morphed meshes.** A mesh with `hasSkinning` or non-zero `morphWeights` is
  posed on the CPU for the pick, with the same math as `skinning.glsl` and the morph shader path
  (positions only).
  - The posed copy refits a per-entity clone of the bind-pose BVH.
  - It is cached by pose epoch, which is bumped when `bonesMatrix` or `morphWeights` change.
  - A static pose is posed once. Above 200k vertices the pick falls back to bind-pose triangles
    clipped by `skinnedAABB`.
- **R7 SceneBvh.** One per `Scene`, built lazily on the first query. Leaves are
  `{worldAABB, entity, kind, subIndex}`.
  - Queries refit the leaves whose Transform or bounds changed (S07 `Changed<Transform>`,
    `MeshComponent::needUpdateAABB`) and rebuild when leaves were added or removed or after 64
    refits. With more than 4096 dirty leaves the refit uses S06 `parallelFor`.
  - Traversal: `ray` (closest, all) and `frustum` (rect select).
- **R8 Filters.** Every query takes a `PickFilter`:
  - `layerMask` (0..31);
  - `includeInactive`, which defaults to what is drawn (`isEntityDrawn` semantics, `Project.cpp:6102`);
  - an ignore list;
  - a restrict-to-entity option;
  - the editor-only masks of R22–R23;
  - `waitForBvh` (R9).
- **R9 BVH lifecycle.**
  - Mesh BVHs are shared across entities through `MeshComponent::pickGeometryHash`, a 64-bit hash
    of the positions and indices. `calculateMeshAABB` computes it in the loop it already runs.
  - The cache is reference-counted, and an entry is dropped 30 s after its last user goes away.
  - Gathering runs on the main thread. The build runs as an S06 Background job.
  - In the editor, every distinct mesh of 256 or more triangles is queued for a build when it
    loads.
  - A query hitting a pending mesh behaves by `waitForBvh`. When true, it helps execute or
    builds inline. When false, it returns a `BOUNDS` hit with `pending = true`.
  - Mesh reload (S05 events), buffer change (`needUpdateBuffer`) and destruction invalidate the
    entry.
- **R10 Disk cache (editor only).**
  - Meshes of 65,536 or more triangles serialize their BVH to `.jasperengine/cache/meshbvh/<hash>.bvh`
    as an S03 `Cache` write. The header has magic, version, hash and triangle count.
  - A bad, truncated or stale file is rebuilt, never trusted.
  - LRU cap 512 MB. Exported games never write it.

### Functional — selection flow (editor)

- **R11 Pick list.** A click builds the hit list: icons, then all geometry hits along the ray in
  every view layer, in today's layer order (top layer first). Inside a layer, 3D sorts by distance.
  2D and UI keep the current draw-order rule (later Transform index first). Duplicates by entity
  are removed.
- **R12 Pick root.** The root of the nearest hit is its outermost enclosing entity among:
  1. the nearest prefab-instance root (`PrefabComponent`), applied outward for nested prefabs;
  2. the model root (`ModelComponent`, whose generated children belong to it);
  3. the outermost collapsed Hierarchy ancestor (today's `resolveStructureSelection` rule, kept
     as a root source).

  In Prefab Mode the stage's own root is never a pick root. Candidates are
  `[root, …intermediate prefab roots…, exact hit, deeper hits' exact entities…]`. The Scene
  toolbar's **Exact Pick** toggle (per user, `AppSettings`) turns roots off.
- **R13 Drill and cycle:**
  - A plain click with nothing in the instance selected selects `candidates[0]`.
  - If the current selection is the root or inside the hit's instance, the click selects the exact
    hit.
  - A click within 4 px of the previous click, with the selection in the candidate list, selects
    the next candidate, wrapping around.
  - Ctrl/Cmd+wheel over the view steps through the candidates at the cursor, and the next click
    confirms. Plain wheel stays zoom.
  - A pill near the cursor shows `name  n/m` for 1.5 s.
  - A miss clears the selection, as today.
- **R14 Modifiers.** Shift adds (as today). Ctrl/Cmd toggles. A modifier click targets the exact
  hit when that is already selected (OE rule, `SceneViewController.cpp:1751-1779`). Every
  selection change goes through S24's `SelectionCmd`, so each click is one undo entry.
- **R15 Rect select.** It uses SceneBvh frustum traversal as the broadphase and keeps today's
  all-corners-inside rule. It respects R22–R23.

### Functional — placement, camera, views, tools (editor)

- **R16 Surface drop.** Model, prefab and mesh drops, and the live prefab drag preview
  (`updatePrefabDrag`), resolve in this order:
  1. a triangle, terrain or UI hit (ignoring the preview's own entities, and only pickable
     layers);
  2. otherwise the y=0 plane (3D) or z=0 (2D);
  3. otherwise 10 units along the ray.

  The dropped object's bottom (the local AABB min along the surface normal) sits on the hit
  point. **Align to Surface** (toolbar toggle, also on while Alt is held during a drag) rotates
  the object's up axis onto the hit normal. A drop is refused while the hit is `pending` (status
  text "Preparing surface…"); the preview may show it.
- **R17 Projection.**
  - 3D views toggle Perspective/Orthographic from a toolbar button (label `Persp` or `Iso`) and
    from a click on a new `CENTER` hit of the view gizmo.
  - Ortho half-height is `distance·tan(fov/2)`, so framing is kept. Ortho near and far are
    ±`far/2` around the target, so zooming in never clips.
  - Axis clicks and Numpad set the view to that axis. While the view is aligned to an axis, the
    toggle goes to ortho, as in Unity, and rotating out of the axis returns it to the user's
    projection.
  - The wheel in ortho scales the half-height.
- **R18 Framing.** Alt+F (all scene types) and Home (3D, kept) frame all: the SceneBvh root bounds,
  excluding Terrain and infinite or skybox bounds unless nothing else exists. F stays Frame
  Selected. Frame, axis snap, pane axis changes and bookmark recall animate with the easing
  setting (R21). The projection switch itself is instant.
- **R19 Bookmarks.**
  - A strip in the Scene toolbar: a Home (default) slot, then numbered slots 1–9, then `+`.
  - Click recalls a slot. Ctrl/Cmd+click overwrites it. Right-click offers Rename, Overwrite and
    Delete (Reset for Home).
  - Shortcuts: Ctrl+Shift+1…9 stores and Shift+1…9 recalls (S25 rebindable, scene view focused).
  - A slot stores the pose (position, target, up), projection, ortho size and fov.
  - On store, the current view is read back, downscaled to 160×90 and saved as
    `.jasperengine/user/bookmarks/<sceneKey>/<slot>.png`. Hovering a slot shows that preview, its
    name and the projection.
- **R20 Layouts.** A toolbar menu offers Single, Split (Persp | Top) and Quad (Top Y, Front Z,
  Side X, Persp). 3D scenes only.
  - Each pane has its own camera, projection and ortho size. Axis panes are ortho and
    axis-locked: they pan and zoom but do not orbit, and their gizmo or Numpad changes the axis.
  - The pane under the cursor takes input and shows the gizmos, tools layer and selection outline.
    All panes draw selection outlines and the grid, aligned to the pane's axis plane.
  - Inactive panes redraw only when the scene or their camera changes, and they render without
    shadows and post FX by default (pane menu: Shaded / Shaded + Shadows).
  - A double click on a pane header maximizes it (toggle).
- **R21 Camera settings popup.** A camera icon in the toolbar opens it, per scene, persisted:
  - Field of View 4–120° (default 60);
  - Dynamic Clipping (default off; on, near and far derive from the target distance);
  - Near and Far (moved here from the Gizmos popup, which keeps a "Camera…" link);
  - Camera Speed 0.01–1000 (default 10; replaces `walkSpeedOffset`, migrated as `10 + offset`),
    with Speed Min/Max for the wheel;
  - Shift multiplier (default 3);
  - Acceleration (default on: speed ramps from 25% to 100% over 0.6 s of continuous flight);
  - Easing (default on, 0.25 s ease-out cubic; off means instant);
  - Reset.
- **R22 Layers dropdown (Unity "Layers").** A toolbar dropdown with Everything, Nothing, and the 32
  named layers, each with an eye (visible) and a lock (pickable).
  - Hidden layers are dropped from the Scene view through the editor camera's `cullingMask`,
    together with their icons and gizmos. Locked layers are skipped by picking and rect select but
    still draw.
  - Per user, per project (`workspace.yaml`). The Game view and play mode ignore it.
- **R23 Per-entity hide and pick lock (Unity SceneVisibility).**
  - The Hierarchy gets a left gutter with eye and hand toggles, shown on hover or when set. A
    click applies to the entity and its descendants. Alt+click applies to the entity alone.
  - H hides the selection, Alt+H shows all, and L toggles the pick lock of the selection (S25
    rebindable).
  - Hidden means editor-only `EntityMetadata::editorFlags` bits, honoured by `isLayerVisible` only
    for cameras flagged `editorView`, so the Game view, exports and play never see it.
  - Stored per scene in `workspace.yaml` by entity id. Not undoable (Unity).
  - A banner "N hidden" in the Scene view clears them on click.
- **R24 Measure tool.** A toolbar toggle; Esc ends it.
  - Drag from A to B. Points snap to the surface hit (R16 order), to vertices while V is held
    (nearest vertex of the hit triangle within 12 px) and to the grid when snap-to-grid is on.
  - A pill shows the length (m in 3D; scene units in 2D and UI). Shift adds ΔX/ΔY/ΔZ.
  - Release commits the measure. Committed measures stay world-anchored, can be selected and
    deleted (Delete), and are stored per scene in the workspace. "Clear Measures" is in the tool
    menu.
- **R25 Rulers.** A toolbar toggle draws top and left rulers in 2D and UI views and in ortho
  axis-aligned 3D panes. Major steps follow 1-2-5 within a 64–130 px band (OE
  `ChooseMajorStepWorld`), with 4 minor ticks. A cursor tick follows the mouse. Rulers are hidden
  in perspective views.

### Platform & backends

- **R26.**
  - The picking module is pure CPU C++20: no GPU readback and nothing backend-specific.
  - x86-64 selects AVX2 at runtime from CPUID (OSXSAVE plus AVX2), with the scalar path as
    fallback. The AVX2 functions are compiled with per-function target attributes
    (GCC/Clang `__attribute__((target("avx2")))`, MSVC intrinsics without `/arch`), so the
    rest of the binary stays baseline.
  - arm64 (Android, iOS, macOS) uses NEON. armv7 and WebAssembly use scalar (WASM SIMD is a
    later option).
  - Single-thread web: Background builds run through S06's pump, and runtime queries build
    inline (R9 `waitForBvh`).
- **R27.** Editor rendering for quad panes and ortho uses the existing sokol passes on all five
  backends. No new shader features are needed apart from the grid plane orientation.

### Performance (build-server reference machine, Linux x64, Release; `je-bench-picking`)

- **R28 Pick latency** after builds, single closest-hit ray, p99 over 10k random rays:
  - ≤ **1.0 ms** on a 1M-triangle scene, in two shapes: 1,000 entities × 1k tris, and one 1M-tri
    mesh plus 500 small meshes;
  - ≤ 2.0 ms for `raycastAll` (cycling list);
  - scalar path ≤ 2.5 ms;
  - WebAssembly (headless Chromium) ≤ 4 ms;
  - icon and UI tests ≤ 0.1 ms for 1,000 icons.
- **R29 Build.**
  - MeshBvh build ≥ 1.5 M tris/s on one worker, so 1M tris take ≤ 0.7 s.
  - Gather ≤ 5 ms per 1M tris on the main thread.
  - Memory ≤ 48 B per triangle.
  - Disk-cache load of a 1M-tri BVH ≤ 60 ms.
  - SceneBvh build for 100k leaves ≤ 25 ms. A refit of 100k leaves ≤ 3 ms serial and ≤ 1.2 ms on
    8 workers. Refitting 100 dirty leaves ≤ 0.05 ms.
- **R30 Skinned pick.** Posing and refitting a 50k-vertex, 4-influence character takes ≤ 3 ms. A
  repeat pick at the same pose epoch costs as a static mesh.
- **R31 Zero when unused.**
  - A game that never calls `MeshRaycast` builds no BVHs and does no per-frame work.
  - In the editor, the steady-state per-frame cost with no pick is ≤ 0.05 ms: dirty-flag
    bookkeeping only, since refits happen at query time.
  - Inactive quad panes cost nothing while nothing changes.
  - Quad layout GPU time on the reference scene is ≤ 2.5× Single at the same window size.

### Editor UX (human)

- **R32.** Toolbar order, left to right after the existing items: Bookmarks strip · Layout ▾ ·
  Persp/Iso · Layers ▾ · Measure · Rulers · Exact Pick · Align to Surface · Camera ⚙ · (post FX) ·
  Gizmos ▾. Narrow views collapse the middle items into a `⋯` menu. Every item has a tooltip
  with its shortcut. Unity names: "Layers", "Persp"/"Iso", "Scene Camera", "Field of View",
  "Dynamic Clipping", "Camera Easing", "Camera Acceleration".
- **R33.** A click on a mesh whose BVH is pending (R9) waits on it while showing a busy cursor;
  after 100 ms the status bar says "Preparing picking for <mesh>…". The Scene view never selects
  through a stale AABB.

### Agent UX (MCP tools, Lua API, CLI)

- **R34 MCP tools** (`editor/mcp/tools/ViewTools.cpp`; all reachable through S31's `call`).
  Screen points are pixels of the Scene view image the `screenshot` tool returns at full
  resolution, or `uv` in [0,1].
  - `pick_at {scene, x|u, y|v, pane?, apply: "none"|"select"|"click" = "none", exact?: bool}`
    returns `{hits:[{entity, path, scene_id, kind, submesh, triangle, instance, point, normal,
    distance, pending}], candidates:[…], would_select, selected?}`. `"click"` runs the full R13
    flow, including same-spot cycling.
  - `set_scene_camera` gains `projection: "perspective"|"orthographic"`, `ortho_size`, `fov`, `near`,
    `far`, `view: "top"|"bottom"|"front"|"back"|"left"|"right"`, `frame_all: bool` and
    `camera_settings {speed, acceleration, easing, dynamic_clipping}`. Reads return all of them.
  - `scene_view_bookmark {scene, action: "list"|"store"|"recall"|"delete"|"rename", slot, name?}`.
    `list` returns poses and preview paths.
  - `set_scene_view_layout {scene, layout: "single"|"split"|"quad", active_pane?, pane_settings?}`.
  - `scene_view_measure {scene, action: "add"|"list"|"delete"|"clear", from?, to? (world) |
    from_screen?, to_screen?, snap?: "surface"|"vertex"|"grid"|"none"}` returns
    `{id, from, to, length, delta}`.
  - `set_scene_view_filter {scene, visible_layers?, pickable_layers?, hide?:[entity], show?:[entity],
    lock?:[entity], unlock?:[entity], show_all?: bool}`.
  - `mesh_raycast {scene, origin, direction, max_distance?, layer_mask?, all?, backfaces?}` uses the
    runtime semantics and works in edit and play.
  - Resource `jasper://scene-view/{scene}` returns layout, projection, bookmarks, measures,
    filters and BVH cache stats (`built`, `pending`, `bytes`, `build_ms_total`).
- **R35 Lua** (`engine/core/script/binding/CoreClassesLua.cpp`; distance semantics as `Physics.raycast`):
  ```lua
  local hit = MeshRaycast.raycast(scene, ray [, layerMask [, options]])   -- fraction of ray
  local hit = MeshRaycast.raycast(scene, origin, dir, maxDistance [, layerMask [, options]]) -- world units
  local hits = MeshRaycast.raycastAll(...)            -- same overloads, nearest first
  local hit = MeshRaycast.raycastEntity(scene, entity, ray [, options])
  MeshRaycast.prepare(scene [, entities])             -- queue Background builds (no hitch later)
  -- options = { hitBackfaces=false, includeInactive=false, ignore={e1,e2}, kinds=MeshRaycast.MESH|... }
  -- hit: hit, distance, point, normal (interpolated if the mesh has normals, else face normal),
  --      faceNormal, entity, submesh, triangle, instance, barycentric (Vector3), kind
  ```
  C++: `MeshRaycast::raycast(Scene*, const Ray&, uint32_t layerMask, const MeshRaycastOptions&)`
  returns `MeshRaycastHit`, a superset of `RayReturn`'s fields. The MCP agent guide and
  `docs/` document it next to `Physics.raycast`, including when to use which.

### Data & compatibility

- **R36.** Scene and prefab files are unchanged (no S03 version bump). `workspace.yaml` per-scene
  entries gain the optional keys `view` (`layout`, `activePane`, `panes[]` with camera and
  projection), `cameraSettings`, `bookmarks[]`, `measures[]`, `layerFilter {visible, pickable}`,
  `hiddenEntities[]` and `pickLockedEntities[]`.
  - Missing keys default to today's behaviour.
  - The legacy `editorCamera.walkSpeed` offset migrates to `cameraSettings.speed`.
  - Unknown keys are preserved (S03).
  - Entity ids that no longer exist are dropped on save.
  - Exact Pick and Align to Surface are per-user `AppSettings` (default off).

## 6. Design constraints & decisions

- **D1 Layout.** New engine dir `engine/core/picking/`: `BinnedSah.{h,cpp}`, `MeshBvh.{h,cpp}`,
  `MeshBvhSimd_{avx2,neon}.cpp`, `PickGeometry.{h,cpp}` (gather from `MeshComponent`), `MeshPose.cpp`
  (CPU skin and morph), `SceneBvh.{h,cpp}`, `MeshRaycast.{h,cpp}` and `PickCache.{h,cpp}`. The
  editor-only pieces live in `editor/picking/`: `ScenePicker` (pick list, roots, cycling),
  `BvhDiskCache` and `IconPicker`. `findBestEntityByRay` and `findObjectByRay` become thin
  wrappers over `ScenePicker`, so existing callers keep compiling.
- **D2 API sketch.**
  ```cpp
  struct PickFilter { uint32_t layerMask = LAYER_MASK_EVERYTHING; uint32_t kinds = PICK_ALL;
      bool includeInactive = false, hitBackfaces = false, waitForBvh = true;
      std::span<const Entity> ignore; Entity restrictTo = NULL_ENTITY;
      uint8_t excludeEditorFlags = 0; };            // EDITOR_HIDDEN | EDITOR_PICK_LOCKED
  struct PickHit { Entity entity; PickKind kind; uint32_t submesh, triangle; int32_t instance;
      Vector3 barycentric, point, normal, faceNormal; float distance; bool pending; };
  class SceneBvh { public: void markDirty(Entity); void raycast(const Ray&, const PickFilter&,
      std::vector<PickHit>& out, bool all); void frustum(const Plane[6], const PickFilter&,
      std::vector<Entity>& out); };
  SceneBvh& Scene::getPickBvh();                   // lazy; destroyed with the scene
  ```
- **D3 SIMD dispatch.** One function pointer for the 8-child slab test, chosen once at startup.
  Tests can force a path with `JASPER_PICK_SIMD=scalar|avx2|neon` (the last two only where
  supported).
- **D4 Distances.** Internally t is in world units along a normalized direction. The `Ray`
  overloads convert to the fraction of `ray.direction`, as `Ray::intersects` and `Physics.raycast`
  do (`PhysicsLayers.h:90-93`).
- **D5 Skinning.** We decide CPU posing over a bind-pose fallback, because bind-pose picks on
  animated characters select empty space. Pose math is a single C++ function, tested against a
  GPU readback of `skinning.glsl` in a golden test. The bind-pose fallback is used only above the
  200k-vertex cap.
- **D6 Roots** come from scene metadata (`PrefabComponent`, `ModelComponent`, Hierarchy collapse
  state), not from a subtree walk. The walk is O(depth) parent climbs.
- **D7 2D and UI are preserved.** Draw order (not depth) ranks hits in 2D and UI. UI picks by
  rect. Tile, instance, point, line and polygon sub-selection keep their existing hit tests
  (`SceneRender.h:350-400`) and run before entity picking, as now.
- **D8 Panes.** `SceneRender` gains `beginPaneViewPass(Camera*, Framebuffer*, PaneFlags)`, modelled
  on `beginGameViewPass` (`SceneRender.h:338-344`) but keeping overlay layers. Each pane owns a
  `Camera` and a `Framebuffer`. Gizmo drags are routed to the active pane's camera. A second
  `SceneRender` instance is not allowed.
- **D9 Drop priority.** Visible geometry comes first, and colliders are not consulted. Editor-time
  physics bodies are not guaranteed to exist, and the user aims at what they see. OE's
  collider-first order (`SceneViewDropPlacement.cpp:69-82`) is rejected.
- **D10 Editor-only flags.** `EntityMetadata::editorFlags` is never serialized, never copied by
  `EntityCloner` into play sessions, and never exposed to Lua.
- **D11 Lazy runtime.** The runtime builds a mesh BVH on the first ray that hits that mesh's AABB
  (inline), unless `prepare()` already queued it. There is no eager build in games.
- **D12 Measures** are workspace data (editor-only, per user), not entities. They never reach
  game builds or the scene file.

## 7. Acceptance criteria

Unit tests live in `engine/tests/core/picking/` and `editor/tests/picking/` (S02 layout). MCP tests
are in `tools/mcp/tests/test_scene_view.py` and `test_mesh_raycast.py`. Benches run through
`je-bench-picking` + `tools/perf/check_bench.py`.

- **AC1 (R1, R2)** `MeshBvh.MatchesBruteForceRandomRays`: 10 random meshes (1–200k tris), 2,000 rays
  each, on every available SIMD path. Hit triangles are identical and `|Δt|` is in tolerance.
  `MeshBvh.SimdPathsBitIdentical` and `MeshBvh.{SingleTriangle,Empty,AxisAlignedRay,
  CustomStride,Uint16Indices,TriangleStrip}` cover the edge cases (ported from OE
  `MeshBvhTests.cpp:35-360`).
- **AC2 (R26)** `MeshBvh.ScalarForcedOnAvx2Host` runs under `JASPER_PICK_SIMD=scalar` with the same
  results. The CI Linux job runs the suite twice. A unit test asserts that the AVX2 path reports
  unavailable when CPUID is masked, through an injected stub.
- **AC3 (R3)** `MeshRaycast.BackfaceCulledRoom`: with the camera inside a culled cube room and a box
  behind a wall, the box is hit. With the room double-sided, the wall is hit. With
  `hitBackfaces=true`, the wall is hit.
- **AC4 (R4, R5)** `MeshRaycast.Kinds`:
  - a sprite gap misses;
  - a tilemap hole misses while a tile hits;
  - instance 7 of an instanced mesh is reported;
  - terrain matches `raycastTerrainSurface` within 1e-4;
  - points and particles return `BOUNDS`;
  - the LOD0 triangle is hit while LOD2 is drawn.
- **AC5 (R6, R30)** `MeshPose.MatchesGpuSkinning` (golden, glcore and Vulkan): world positions written
  by a debug variant of the skinned vertex shader into an RGBA32F target for a posed, morphed
  fixture equal CPU posing within 1e-4.
  `MeshRaycast.SkinnedArmRaised` hits the raised arm and misses its bind-pose location. A bench
  threshold covers R30.
- **AC6 (R7, R8, R15)** `SceneBvh.{BuildRefitRebuild,MatchesLinearScan,FrustumMatchesCornerRule,
  LayerMask,EditorFlags,ParallelRefitEqualsSerial}` (cases adapted from OE `SceneTlasTests.cpp`).
  10k moving entities over 100 frames give results equal to a linear scan.
- **AC7 (R9, R10, R33)** `PickCache.{SharedAcrossInstances,InvalidatedOnReload,PendingReturnsBounds,
  WaitBuildsInline,DiskCacheRoundTrip,DiskCacheRejectsCorrupt,DiskCacheRejectsStaleVersion}`.
  MCP `test_scene_view.py::test_pick_waits_for_pending_bvh` imports a 2M-tri model, clicks at once
  with `apply:"click"`, and asserts that the result is the triangle-exact entity.
- **AC8 (R11–R14)** MCP `test_scene_view.py`:
  - `test_room_does_not_steal_click`: a camera inside a hollow room picks the chair;
  - `test_root_then_drill`: click 1 selects the prefab root, click 2 at the same spot the clicked
    child, click 3 the next hit behind;
  - `test_cycle_wraps`;
  - `test_exact_pick_toggle`;
  - `test_ctrl_wheel_cycle`;
  - `test_collapsed_group_is_root`;
  - `test_prefab_mode_root_not_swallowing`;
  - `test_shift_ctrl_modifiers`;
  - `test_selection_undo` (S24): one Ctrl+Z per click.
- **AC9 (D7)** `test_scene_view.py::test_2d_draw_order_kept` and `test_ui_rect_pick_kept`: the existing
  2D and UI MCP pick expectations in `tools/mcp/tests/test_scene.py` and `test_ui_anchors.py` pass
  unchanged.
- **AC10 (R16)** `test_drop_on_surface`: a cube dropped on a 30°-tilted plane rests with its bottom
  face on the plane (distance ≤ 1e-3). With Align to Surface its up axis equals the plane normal
  (≤ 0.5°). `test_drop_refused_while_pending`.
- **AC11 (R17, R18)** `test_projection_toggle`: a view-gizmo centre click (simulated via `pick_at`
  UI hook) and `set_scene_camera{projection}` toggle it, and the screenshot framing differs ≤ 2%
  in object screen size. `test_axis_view_goes_ortho`. `test_alt_f_frames_all` checks that every
  entity's bounds project inside the view. Golden `scene_view_ortho_top.png` (glcore) covers the
  grid orientation.
- **AC12 (R19)** `test_bookmarks`: store slot 3, move, recall restores the pose (≤ 1e-4). The PNG
  exists at 160×90. The slot survives an editor restart. Manual check: hovering slot 3 shows the
  preview within 1 frame.
- **AC13 (R20, R31)** `test_quad_layout`: each pane screenshot shows the expected axis (golden, four
  images). Picking in the Top pane selects by that pane's ray. The bench shows ≤ 2.5× GPU time
  and that idle panes do not redraw (the frame-pass counter from S15 is unchanged over 60 idle
  frames).
- **AC14 (R21)** `test_camera_settings`: fov, speed and easing round-trip through the workspace. A
  legacy workspace with `walkSpeed: 5` loads as speed 15. With easing off, F frames in one frame.
- **AC15 (R22, R23)** `test_layer_filter`: a hidden layer is absent from the Scene screenshot and
  present in the Game view screenshot. A locked layer is not pickable but is drawn.
  `test_hide_and_lock_entity`: H, Alt+H and L behave as specified, export output is byte-identical
  with and without hidden entities, and play mode draws them.
- **AC16 (R24, R25)** `test_measure`: a 3D measure between two cube faces 4 m apart reads 4.000 with
  surface snap, vertex snap hits the corner, and the measure persists in the workspace.
  `RulerStep.NiceSteps` is a unit test of the 1-2-5 selection over 1e-4…1e6 units per px, with the
  band always kept.
- **AC17 (R28, R29)** the `je-bench-picking` thresholds hold on the reference machine.
  `test_mesh_raycast.py::test_web_latency` runs in the S02 web browser gate and holds ≤ 4 ms p99.
- **AC18 (R35)** `test_mesh_raycast.py`: a Lua script in play mode raycasts a static mesh, a skinned
  mesh and an instanced mesh. `raycastAll` order, layer mask, `ignore`, and fraction versus
  world-distance overloads match C++. `prepare()` followed by a raycast causes no inline build
  (stats).
- **AC19 (R34)** every new tool has an MCP schema test and a docs entry. `docs/` and the MCP agent
  guide pages are updated (checked by `test_docs.py`).
- **AC20 (R36)** `Workspace.LoadsLegacyWithoutViewKeys`, `Workspace.PreservesUnknownKeys` and
  `Workspace.DropsDeadEntityIds`. Scene file bytes are unchanged after open and save of every
  `editor/tests/fixtures/scenes/*`.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 and S02 (C++20, gtest, bench gate, web gate).
  - S06 (Background lane, `parallelFor`, pumped single-thread mode).
  - S07 (`Changed<Transform>` and dense `EntityMetadata`, which receives `editorFlags`).
  - S24 (selection undo).
- **Uses:** S03 (`Cache` and `Document` atomic writes for the BVH cache and workspace), S05
  (reload events), S25 (`editor/shortcuts/CommandCatalog`: H, Alt+H, L, Alt+F and Shift/Ctrl+Shift+1…9 are
  registered there with context "Scene view", not as raw `IsKeyPressed`) and S15 (profiler scopes, frame pass counter).
- **Blocks or enables:** S18 (surface conform, spline placement on meshes), S19 (nav-grid brush
  on meshes), S20 (particle collisions could reuse `SceneBvh`) and S22 (gameplay queries without
  physics).
- **Name clash:** S25 "bookmarks" are asset, scene and entity favourites. Ours are *camera
  bookmarks*. UI and MCP names say "camera bookmark" or `scene_view_bookmark` to avoid confusion.
- **Merge hotspots:**
  - `editor/window/SceneWindow.cpp` (toolbar 2195-2235, input 1480-1710), also touched by S25 and
    S27.
  - `editor/Project.cpp` picking block 5943-6420.
  - `editor/window/HierarchyWindow.cpp` row drawing (S25, S29 status overlays).
  - `editor/Workspace.{h,cpp}` (S03, S25 layouts).
  - `engine/core/subsystem/MeshSystem.cpp` `calculateMeshAABB` (S07, S11; S06 lists S26).
  - `engine/core/subsystem/RenderSystem.cpp` `isLayerVisible`.
  - `engine/core/ecs/EntityManager.h` (S07).
  - `editor/mcp/tools/ViewTools.cpp` (S15, S31).
  - `CoreClassesLua.cpp` (S22, S23).
  - `NOTICE.md`.

## 9. Risks & defaults

- **Mobile memory for big meshes at runtime.** *Default:* lazy per-mesh builds (D11). An
  `Engine` setting `meshRaycastMaxTriangles` (default 2M) makes larger meshes answer with `BOUNDS`.
  The stats show bytes used.
- **Alpha-cutout foliage and sprites with transparent margins pick as solid.** *Default:* accepted
  for this stream, and documented. A later follow-up can test alpha against the CPU image kept by
  the thumbnail cache. Users can pick-lock foliage layers (R22).
- **The CPU pose diverges from the GPU** (normalization flags `normAdjustJoint/Weight`, more than
  4 influences). *Default:* one shared pose function, the golden AC5, and a bind-pose fallback
  with a one-time log line when an unsupported attribute layout is seen.
- **Quad views quadruple vertex cost on heavy scenes.** *Default:* idle panes do not redraw, and
  shadows and post FX are off in secondary panes. Quad is opt-in and Single is the default.
- **Collapsed-node grouping could surprise users once drilling exists.**
  *Default:* the collapsed rule stays as a root source (R12). Expanding the node in the Hierarchy
  restores exact picking for it, which matches upstream intent (commit `2e58ed62c`).
- **Ortho editor camera with CSM and fog.** *Default:* shadows use the ortho view frustum (the
  existing cascade code takes any projection). If a backend golden fails, ortho views fall back
  to a single cascade.
- **Disk cache staleness across engine updates.** *Default:* the format version, the triangle
  count and the content hash all have to match. Any mismatch rebuilds.
- **Pick latency on WebAssembly without SIMD.** *Default:* the scalar path with the 4 ms budget.
  WASM SIMD128 is a follow-up behind the same dispatch.

## 10. Deviations (implementation)

Where the implementation differs from the text above, and why.

- **Pose epoch (R6).** `bonesMatrix` and `morphWeights` are plain arrays written from many places
  (animation, Lua, the editor), with no write hook to bump a counter. The posed copy is keyed by a
  content hash of the bone matrices and morph weights the pose reads (`picking::poseHash`), which
  changes exactly when the pose does; a static pose is posed once.
- **`pickGeometryHash` (R9).** Computed on the first pick that needs it (`picking::hashSources`
  over the same sources the gather reads) and reset by `calculateMeshAABB` and buffer uploads,
  instead of inside `calculateMeshAABB`'s loop: a game that never picks never hashes (R31).
- **BOUNDS hits (R4).** A bounds-only object is not hit from inside its box (no distance-0 hit as
  `Ray::intersects` gives), so a large bounds-only object around the camera cannot take every
  click either. Hit normals and face normals face the ray.
- **`PickFilter::accept`.** Besides D2's fields, a predicate: the editor passes the pickable
  GameObjects of each view layer (authored ones, plus the game's while it runs).
- **Ruler step (R25).** 1-2-5 alone cannot keep the 64-130 px band (2 to 5 is a 2.5 ratio, wider
  than the band's 2.03), so 2.5 times a power of ten is used where neither 2 nor 5 lands in it.
- **S25 not landed (R19, R23).** The shortcuts are in an S25-shaped table
  (`editor::sceneViewCommands()`, ids `sceneView.frameAll`, `sceneView.hideSelection`,
  `sceneView.storeBookmark`, ...) read with ImGui keys while the Scene view is focused; they move
  into S25's `CommandCatalog` (context "Scene view") when it lands. The measure tool also has M.
- **MCP names (R34).** S31's conventions require `verb_noun` names: `scene_view_bookmark` is
  `edit_scene_view_bookmarks`, `scene_view_measure` is `measure_scene_view`, `mesh_raycast` is
  `raycast_meshes` ("raycast" added to the verbs). An agent's camera changes (`set_scene_camera`,
  `frame_entity`, bookmark recall) land at once; Camera Easing animates the user's moves only.
  `pick_at` waits for a building BVH (the click R33 describes for the user defers instead);
  `apply` takes `shift` and `ctrl` too; `set_scene_view_layout` takes `capture` (one image per
  pane) and returns each pane's draw count. `editor_status` reports the status bar's message.
- **AC5's target.** The test target is the core tests' RGBA8 framebuffer, not an RGBA32F one: the
  debug shader draws each vertex as a point of its own and writes its world x, y, z and its index
  as 24-bit fixed point over four frames (a step of 1e-6 over [-8, 8], far under the 1e-4
  tolerance), so it runs unchanged on glcore and Vulkan.
- **AC7's model.** The test imports a 140k-triangle generated lattice and holds background builds
  pending with `JASPER_PICK_BUILD_DELAY_MS` (a test knob read once by `PickCache`), instead of
  importing a 2M-triangle model: what the test checks is the pending state, which the knob makes
  deterministic, and a 2M-triangle import costs minutes in the MCP harness.
- **Panes (R20, D8).** The active pane is the Scene view as before (the editor camera, the
  Engine's frame, gizmos and tools layer); the others draw after it through cameras of their own
  with `SceneRender::drawPaneView` (D8's `beginPaneViewPass`, built on `Engine::drawView`). The
  pane under the pointer becomes the active one at the next frame (the editor camera takes that
  pane's pose). An inactive pane redraws when the view redraws for anything else than the active
  camera moving, when the selection, the layout or its own camera changes, and every frame while
  the scene plays. Without shadows a view plans and draws no shadow map and its fragments read
  their lights at shadow strength 0 (`Engine::ViewLayer::shadows`). Icons in inactive panes keep
  the size the active view gives them. AC13's "frame-pass counter unchanged" is measured per pane
  (`draws`), since S15's counter also counts the active view.
- **Hidden GameObjects and shadows (R23).** The Scene view's camera passes skip hidden
  GameObjects; their shadows are still drawn (the shadow passes are the scene's, shared by every
  view).
- **Rulers (R25, R36).** The Rulers toggle is a per-user preference like Exact Pick and Align to
  Surface (`AppSettings`, `editor.scene_view.rulers`).
- **Hierarchy gutter (R23).** Alt+click applies to the GameObject alone; H toggles (shows the
  selection again when all of it is hidden), as Unity's H does.
- **Drops (R16).** The bottom rests on the surface as the deepest corner of the object's local
  bounds along the normal (with Align to Surface, its bottom face); a model rests once loaded
  (`ModelLoadCmd`, so redo lands the same), a prefab in `ImportPrefabCmd`.
- **Workspace (R36).** `view.panes` lists all four panes (the free one too: while another pane is
  active the editor camera holds that one's pose).
