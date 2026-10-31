# Persistence: DontDestroyOnLoad (design)

Feature `persistence` (ledger #20, branch `gaps/persistence`). Binding conventions:
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) "Shared conventions" ("Scene loads": everything that outlives a
load has a documented rule). Lifecycle, frame order and the editor reload fix: `script-lifecycle` spec (6.6, 9.1,
9.4). Coroutine/timer ownership: `coroutines` spec.

## 1. Goals

1. `DontDestroyOnLoad(obj)`: a root GameObject with its children, components, scripts (their Lua state),
   playing sounds and physics bodies survives `SceneManager.loadScene`, in editor Play and in exported builds on
   every platform, like Unity.
2. Unity's model: a special always-loaded scene named **"DontDestroyOnLoad"**, drawn above the loaded scenes,
   shown in the Hierarchy during Play, inspectable by MCP, destroyed when Play stops / the app quits.
3. A first-class singleton pattern (`GameObject.persistSingleton(self)`) because module tables are per instance
   in this engine (a C# `static Instance` has no direct Lua equivalent).
4. `SceneManager.moveGameObjectToScene(obj, scene)` (Unity) to carry a persistent object into a loaded scene.
5. Correct identity: the moved object's script instances are the same tables (other scripts holding them keep
   working), coroutines and timers continue, entity ids are kept when possible.

## 2. Non-goals

- One physics world across scenes: each Scene owns its Box2D/Jolt worlds (`PhysicsSystem.h:57-93`); persistent
  bodies collide only with persistent bodies (documented; moving the object into the level with
  `moveGameObjectToScene` is the way to make it collide there).
- Depth-interleaving persistent world objects with the level: scene layers clear depth and draw in order; the
  persistent layer draws on top (UI overlays, managers and sounds are the target use).
- Moving non-root objects (Unity forbids it too), moving objects between scenes in edit mode.
- Keeping HTML UI document state (DOM, data models) across a move: a moved UIDocument reloads in the target scene
  (documented; DontDestroyOnLoad in `init` happens before documents start, so the usual case loses nothing).

## 3. Users and stories

- Human from Unity: an `AudioManager` with background music, `DontDestroyOnLoad(self)` in `init`; the music keeps
  playing across `SceneManager.loadScene("Level2")`.
- Human: an IAP/ads manager prefab placed in every scene with its own canvases; the duplicate in each newly loaded
  scene removes itself: `if GameObject.persistSingleton(self) ~= self then return end`.
- Agent porting `if (instance == null) { instance = this; DontDestroyOnLoad(gameObject); } else if (instance != this)
  Destroy(gameObject);` writes the one-liner above; other scripts find it with
  `GameObject.findObjectOfType("AdsManagement")` (finds persistent objects too).
- Agent testing: `get_play_state` lists `{"name":"DontDestroyOnLoad","persistent":true}`; `list_runtime_entities
  scene="DontDestroyOnLoad"`; after `eval_lua SceneManager.loadScene("Menu")` and `wait frames=2`, the manager is
  still there with the same script state.

## 4. Research summary

- Unity DontDestroyOnLoad (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Object.DontDestroyOnLoad.html):
  root GameObjects only (children follow), no callbacks; the documented singleton pattern destroys duplicates. In
  Play the Hierarchy shows a "DontDestroyOnLoad" scene; exiting Play destroys them (OnDestroy is called).
  `SceneManager.MoveGameObjectToScene` (…/SceneManagement.SceneManager.MoveGameObjectToScene.html): roots only,
  target loaded. `FindObjectOfType` finds DontDestroyOnLoad objects.
- Godot autoloads (https://docs.godotengine.org/en/stable/tutorials/scripting/singletons_autoload.html): nodes under
  the root that survive `change_scene_*`, added before any scene, reachable by name. Taken: "always present,
  above scenes". Left: project-level autoload list (Unity users expect the runtime call; a later feature can add
  an autoload prefab list on top of this scene).
- Engine facts: `SceneManager::runFactory` removes every scene then re-adds only the loading scene
  (`SceneManager.cpp:113-141`); exported `load_X` deletes scenes not running (`Generator.cpp:2207-2256`); editor
  stale scenes get scripts cleaned and sounds stopped (`Project.cpp:8869-8915`); `~Scene` stops its sounds
  (`Scene.cpp:59-68`); scenes are separate ECS registries; `EntityCloner` copies across registries but resets runtime
  state (`EntityCloner.cpp:612-668`); the loading scene path already keeps a scene alive across loads.

## 5. Product decisions

1. **A persistent Scene owned by SceneManager**, created on the first `DontDestroyOnLoad`, named
   "DontDestroyOnLoad", always an Engine scene layer, re-added after every load like the loading scene, never
   unloaded by `loadScene`.
2. **Layer order**: loaded stack (main + layers + child scenes added later) < persistent scene < loading scene
   (< debug overlay, feature `debug-overlay`). The persistent scene is raised after every load and every
   `addChildScene`. So persistent UI covers the game, the loading screen covers everything.
3. **Its camera is the default UI camera** (`Scene::createDefaultCamera`, `Scene.cpp:175-189`); if a persistent
   object brings a Camera component, the first one becomes the persistent scene's camera.
4. **The move is a true move**, not a copy: same script instance tables, same Lua state, sound voices continue
   (handle transferred), physics bodies re-created in the persistent world with their velocities, actions keep
   their state, entity ids kept when free in the target (else remapped and every engine-held reference updated).
   No lifecycle callbacks fire for the move (Unity).
5. **The move happens at a safe point**: immediately when called outside a scene initialization pass and outside
   iteration (between frames or in the update phase -> deferred to `ObjectManager::update` of this frame), at the
   end of the scene's initialization when called from `init`/`onEnable` of a loading scene (before the scene's
   documents start), and always before the next `runFactory`. `GameObject.isPersistent(obj)` is true from the call.
   `self.scene`/`self.entity` change when the move happens.
6. **Non-root objects**: warning "DontDestroyOnLoad only works for root GameObjects ('<name>' has a parent)" and
   nothing happens (Unity).
7. **Play stop / app quit** destroys the persistent scene after the quit pass (`ScriptRuntime::quitAll` includes it:
   `onApplicationQuit`, `onDisable`, `onDestroy`).
8. **`GameObject.persistSingleton(self)`**: returns the surviving instance of `self`'s class among persistent
   objects; when another exists, `self`'s GameObject is destroyed (deferred) and that other instance is returned.
9. **3D audio** of persistent AudioSources uses the main scene's listener (the persistent AudioSystem does not set
   the listener), and keeps 2D sounds (music) untouched.
10. **Editor Play matches exports**, including reload (script-lifecycle 9.4) so "Restart" + persistence behave the
    same.

## 6. Design: runtime C++

### 6.1 SceneManager (`engine/core/manager/SceneManager.{h,cpp}`)

```cpp
static constexpr uint32_t PERSISTENT_SCENE_ID = 0xFFFFFFFEu;   // never a project scene id
static Scene* getPersistentScene();                 // nullptr until created
static Scene* ensurePersistentScene();              // creates (factory below) and adds as a layer
static void setPersistentSceneFactory(std::function<Scene*()> create, std::function<void(Scene*)> destroy); // editor
static void destroyPersistentScene();               // quit pass already ran; deletes via the factory's destroy
static bool dontDestroyOnLoad(Scene* scene, Entity root);   // mark (+ queue move); false for non-root
static bool isPersistent(Scene* scene, Entity entity);      // in the persistent scene or queued
static bool moveGameObjectToScene(Scene* from, Entity root, Scene* to);
static void flushPersistentMoves();                 // runs queued moves (safe point)
```

- Default factory: `new Scene()`, `setScenePtr(PERSISTENT_SCENE_ID, scene)`, `scene->load()` (default camera),
  physics/audio/UI systems as every Scene. Settings: defaults; `Engine::getScene()` stays the loaded main scene.
- `runFactory` (`SceneManager.cpp:113-141`): `flushPersistentMoves()` first; after `Engine::removeAllScenes()` it
  re-adds the persistent scene with the loading scenes (`addSceneLayer`), and after the factory calls
  `raisePersistentScene()` then `raiseLoadingScene()`. `ScriptRuntime::unloadScenes` (lifecycle) excludes it.
- `addChildScene` (`:647-678`) calls `raisePersistentScene()` (and the existing loading raise) at its end.
- `raisePersistentScene()`: `Engine::removeScene(p); Engine::addSceneLayer(p)` (it is never the main scene).
- `isCoveredByLoading(p)` is true while the loading scene is shown (no UI input underneath, as for game scenes).
- `clearAll()` (`:~760`) calls `destroyPersistentScene()`.
- Queue: `std::vector<{Scene* from, Entity root, Scene* to}>`; entries for scenes being deleted are processed or
  dropped by `ObjectManager::forgetScene` (dropped only if the root no longer exists).

### 6.2 The move (`engine/core/registry/EntityCloner.{h,cpp}`)

```cpp
// Moves `sources` (collectHierarchy order) from `source` to `target`, keeping runtime state. `map` gets old -> new ids.
static std::vector<Entity> move(Scene* source, const std::vector<Entity>& sources, Scene* target,
                                std::unordered_map<Entity, Entity>& map);
```

Steps (all on the main thread, between system updates):
1. Ids: for each source, the same id when `target` has no such entity (`EntityManager::createEntityWithId`, new,
   user range only), else `createUserEntity()`. Names, tags, layers, active flags, hierarchy order copied.
2. Components copied as `clone` does (same per-component code paths), with **move mode** differences:
   - `ScriptComponent`: entries copied including `instance` and the lifecycle flags (script-lifecycle 6.1);
     the source entries' `instance` set to null afterwards (so the source destruction releases nothing).
   - `AudioSourceComponent`: `handle`, state, `lastPosition` kept; the source's `handle` zeroed (so
     `stopSceneSounds`/destruction do not stop the voice).
   - Rigidbody/Rigidbody2D: before destroying the source bodies, read linear/angular velocity, awake state; the
     target bodies are created by the target PhysicsSystem on its next update (as for a clone) and the saved
     velocities applied once they exist (a pending-velocity field on the component, consumed at creation).
     Joints whose both bodies are in the subtree are re-created; joints to bodies outside are dropped with a
     warning "joint '<name>' to an object outside the moved hierarchy was removed".
   - Action/Tween/Animation/AnimationPlayer/ParticleSystem components: running state, time counters kept
     (particles alive are not moved: the emitter restarts its live particles; documented).
   - Event fields (`FunctionSubscribe` in components, e.g. `ButtonComponent.onPress`): copied (subscriptions
     shared, `FunctionSubscribe.h` copy semantics), not reset.
   - Render/mesh GPU state: reset and reloaded as for a clone (pools are shared, so it reloads from cache within
     the frame or the next).
   - UIDocument: reloaded in the target (decision in non-goals).
3. Entity references inside the moved set are remapped (Transform parent/children, action targets, script
   `EntityReference` property values with `sceneId == 0`); references to outside entities are kept as they are for
   script properties with an explicit `sceneId` and cleared (`NULL_ENTITY`) for component fields (clone rule).
4. Source entities destroyed with `scene->destroyEntities` (children first) **without**
   `ObjectManager::cleanupScripts` (no lifecycle callbacks).
5. Notifications (new `ObjectManager::notifyMoved(Scene* from, Scene* to, const std::unordered_map<Entity,Entity>&)`):
   - Lua instances: `self.scene` = target Scene userdata, `self.entity` = new id, cached `gameObject` refreshed.
   - C++ instances: through the entry's `ScriptBase*` (stored by the generated factory, physics-contacts 6.1.1)
     the engine calls the new non-virtual `ScriptBase::rebind(Scene*, Entity)` (public, documented as
     engine-internal). "Subclass kind" scripts (GameObject wrapper subclasses, no `ScriptBase`): the generated
     code emits `extern "C" void rebindCppScript(ScriptEntry&, Scene*, Entity)` that, per class, calls
     `ScriptBase::rebind` (`if constexpr (std::is_base_of_v<ScriptBase, T>)`) or the new
     `EntityHandle::rebind(Scene*, Entity)` (`is_base_of_v<EntityHandle, T>`); set as an `ObjectManager` hook next
     to `setCppScriptHooks` (`Generator.cpp:2377`, `Conector.cpp:196-214`).
   - `ObjectManager::pendingDestroys` entries remapped; `ScriptRuntime::remap` (script-lifecycle start queue);
     physics-contacts' receiver-mask cache recomputed for the new ids (`ReceiversChanged`);
     `ScriptScheduler::rebind(owner, scene, entity)` for coroutine records.
   - Editor: selection and Inspector targets of moved entities follow (`Project` listens via the destroy listener
     path, `ProjectPrefabRuntime.cpp:309`, extended with a move listener).
6. `RegisterEngineEvent` guard: the closure built in `luaRegisterEventImpl` (`LuaBinding.cpp:436-475`) stops
   capturing `Scene*`/entity (upvalues 3-5) and reads `scene`, `entity`, `__scriptRef` from `self` (upvalue 2) with
   `lua_rawget` at call time; this also removes a dangling-pointer hazard after a scene is deleted.

`SceneManager::moveGameObjectToScene(from, root, to)` uses the same `move` (root only, `to` must be running or the
persistent scene, else error "target scene is not loaded"); it un-marks persistence when moving out of the
persistent scene.

### 6.3 Subsystems

- AudioSystem (`AudioSystem.cpp:395-420`): when `scene == SceneManager::getPersistentScene()`, 3D source parameters
  are updated relative to world positions but `set3dListenerParameters` is not called (the main scene sets it).
- RenderSystem: no change (non-base layer: color LOAD, own depth, no post-processing, `RenderSystem.cpp:15192-15199`).
- UIDocumentSystem / UISystem: persistent documents and native UI receive input first (top layer).
- PhysicsSystem: own worlds; `Physics.raycast(SceneManager.persistentScene, ...)` queries it.
- AnimEngine (anime) timers and HTML UI clock: global, unaffected.

## 7. Lua and C++ API

| Lua | Semantics |
|---|---|
| `DontDestroyOnLoad(obj)` | global, Unity name. `obj`: any wrapper or script instance (its GameObject). Marks its root to persist (6.1); non-root: warning, no-op; `nil` ignored; already persistent: no-op |
| `GameObject.dontDestroyOnLoad(obj)` | same function (discoverable with the other GameObject statics) |
| `GameObject.isPersistent(obj) -> bool` | in the persistent scene or queued to move |
| `GameObject.persistSingleton(self) -> instance` | `self` must be a script instance on a root GameObject; looks for another instance of `self.__name` on a persistent object (incl. queued); found: `GameObject.destroy(self)` (deferred) and returns it; else `DontDestroyOnLoad(self)` and returns `self` |
| `SceneManager.persistentScene` | ro: the persistent `Scene` or nil |
| `SceneManager.moveGameObjectToScene(obj, scene)` | moves a root GameObject to a running scene (or the persistent scene); errors as 6.2 |

C++: `SceneManager::dontDestroyOnLoad(scene, root)`, `ScriptBase::dontDestroyOnLoad()` (its own root),
`SceneManager::moveGameObjectToScene`. Prefabs can be instantiated straight into it:
`GameObject.instantiate(prefab, SceneManager.persistentScene)` (after `ensurePersistentScene`; the Lua getter
creates it on demand only through `DontDestroyOnLoad`, so a nil check is documented).

## 8. Editor UX, MCP, exported builds, docs

### 8.1 Editor
- `SceneManager::setPersistentSceneFactory` is set by `Project` at Play start: it creates a `SceneProject` named
  "DontDestroyOnLoad" (owned runtime, `PlayRuntimeScene{sourceSceneId = PERSISTENT_SCENE_ID, persistent = true,
  initialized = true}`, a Play id via `assignPlayRuntimeId`) and returns its Scene; `destroy` releases it with the
  session (`cleanupPlaySession`, `Project.cpp:4242`). `PlayRuntimeScene` gains `bool persistent = false`.
- The load factory's stale-entry logic (`Project.cpp:8869-8915`) and the lifecycle reload retirement skip the
  persistent entry.
- Hierarchy (`HierarchyWindow.cpp:2622-2688`): while Play runs and the persistent scene exists, a last root row
  "DontDestroyOnLoad" (icon `ICON_FA_THUMBTACK`, italic, not renamable, no context menu items that edit the scene
  file) lists its roots live, like running layers (`isRuntimeLayer = true`, `isPersistent = true`). Selecting an
  entity shows it in the Inspector with live editing like other runtime scenes. `getRunningRuntimeLayerScenes`
  (`Project.cpp:9413-9435`) includes it.
- Stop: `ScriptRuntime::quitAll()` (lifecycle) covers it, then sounds stopped, scene deleted; nothing persists into
  edit mode; the edit scenes are restored as today.

### 8.2 MCP
- `runningScenesJson` (`PlayTools.cpp:449-459`) adds `persistent: true` for it; the `scene` parameter resolver
  (`PlayTools.cpp:700-720`) accepts "DontDestroyOnLoad" (name) and its Play id.
- `get_runtime_entity` reports `persistent: true` for entities in it. No new tool.
- Agent guide (`docs/mcp/guide/lua-api.md` and `McpInstructions.cpp` one line): "DontDestroyOnLoad(self) in init;
  singleton: `if GameObject.persistSingleton(self) ~= self then return end`".

### 8.3 Exported builds
- No generated code changes: the persistent scene is created by the engine library; generated `load_X` only deletes
  its own static scenes (`Generator.cpp:2210-2255`) and `runFactory` re-adds the persistent layer before them.
- `Engine::systemShutdown` runs `ScriptRuntime::quitAll()` (lifecycle), then `SceneManager::destroyPersistentScene()`.
- Same code on Linux, Windows, macOS, Android, iOS, Web.

### 8.4 Docs
- lua-api.md: new section "2.x Persistent objects (DontDestroyOnLoad)": rules (root only, layer order, camera,
  physics world, 3D audio listener, UIDocument reload, when `self.scene` changes, singleton pattern, Play stop),
  2.8 SceneManager additions; pitfalls: "module tables are per instance: use `GameObject.persistSingleton` or a
  `require`d table for singletons"; "wrappers made before DontDestroyOnLoad for that object refer to the old scene
  after the move: use `self.gameObject` or re-wrap".
- unity-glossary.md: DontDestroyOnLoad, the DontDestroyOnLoad scene, MoveGameObjectToScene, singleton pattern row.
- prefabs.md: instantiating into the persistent scene.

## 9. Behaviour rules and edge cases

- Call sites and timing (decision 5): from `init` during a scene load -> moved after the scene's `awakeScene`
  passes, before `startDocumentScripts`; from `init` of an instantiated object -> at `ObjectManager::update` of the
  frame (or immediately when instantiate was called between frames); from `onUpdate`/coroutines/timers -> at
  `ObjectManager::update` of this frame; from input callbacks (between frames) -> immediately. Always before
  `runFactory`.
- `DontDestroyOnLoad` then `GameObject.destroy` in the same frame: the destroy wins (the move skips missing roots;
  pending destroys are remapped when the move runs first).
- An object marked in a scene that is unloaded in the same frame is moved first (runFactory flushes).
- Calling it on an object of the loading scene, a child scene or a scene layer: allowed (any running scene).
- Calling it on an object already persistent, or in the persistent scene: no-op.
- `SceneManager.loadScene` from a persistent script: the persistent object is not unloaded; its coroutines and timers
  continue; `Time.timeSinceLevelLoad` resets.
- Destroying the last persistent object leaves the (empty) persistent scene alive until stop/quit (cheap).
- Lua code calling `Engine.removeAllScenes()` directly removes the persistent layer from drawing until the next load
  re-adds it (low-level API; documented).
- References from persistent scripts to objects of an unloaded scene: script tables survive but their GameObject is
  gone (`getScript` returns nil, wrappers report the dead scene as today, `TestEntityHandleLifetime`).
- Scene-wide events registered on the old scene (`self.scene:getPhysicsSystem().beginContact2D`) stay on that scene
  until it unloads; persistent scripts should register them on `self.scene` after the move (documented) or use the
  per-object contact callbacks (physics-contacts).
- `persistSingleton` called on two duplicates in the same init pass: the first call (pass order) wins; the second
  sees the first as queued and destroys itself.
- Entity id collision (two scenes persisting entity 1005): the second gets a new id; its scripts' `self.entity`
  updates; nothing else visible.
- Re-entrancy: the move never runs inside script callbacks; callbacks run after it see the new location.
- Editor pause/step: moves happen at the same frame points.

## 10. Performance

- The move costs one clone of the subtree plus GPU reload; done once per object per session. Target: a 400-entity
  prefab (the game's IAP listener: 373 GameObjects, 2 canvases) moves in < 10 ms on desktop.
- Per frame: one extra scene layer update/draw (empty systems cost the existing per-scene overhead); raising the
  layer after loads is O(scenes).

## 11. Tests

gtest `editor/tests/PersistenceTest.cpp` (harness of script-lifecycle 11 with two SceneManager-registered scene
factories that build scenes in code, like generated `load_X`, to test the exported path):
1. `DontDestroyOnLoad(self)` in `init`: after `loadScene("B")` and one frame, the object exists in the persistent
   scene with its child, `self.scene == SceneManager.persistentScene`, the same instance table (a field set before
   the load is still there), `onDestroy` not called; scene A's objects got `onDestroy`.
2. Entity id kept when free; collision case remaps and `self.entity` matches the new id.
3. Coroutine started before the load keeps running (counter advances across the load); `invokeRepeating` continues.
4. AudioSource playing (test WAV, SoLoud null driver) keeps the same voice handle across the load
   (`AudioSystem::getSoloud().isValidVoiceHandle`).
5. Rigidbody2D with velocity (10, 0) keeps its velocity after the move (within one fixed step).
6. Layer order after load and after `addChildScene`: [main, layers..., persistent, loading].
7. Non-root: warning text, nothing moved. `persistSingleton`: two loads of scene A (which contains the manager)
   leave exactly one manager; the duplicate's `init` returned early; `findObjectsOfType` returns one.
8. `moveGameObjectToScene(obj, Engine.scene)` after a load: object in the main scene, still the same instance.
9. Quit: `ScriptRuntime::quitAll()` calls `onApplicationQuit/onDisable/onDestroy` on persistent scripts; the scene is
   destroyed by `destroyPersistentScene`.
10. `GameObject.destroy` before the move in the same frame: no move, no crash; `destroy(obj, 1.0)` before the move:
    destroyed 1 s later in the persistent scene.
11. RegisterEngineEvent guard after the move: `onUpdate` keeps running; deactivating the moved object stops it
    (guard reads the new scene).
MCP `tools/mcp/tests/test_persistence.py` (editor Play path): scene Menu with a manager script (persistent) and a
button; play; `get_play_state` lists the DontDestroyOnLoad scene; `eval_lua SceneManager.loadScene("Level")`;
`wait frames=3`; `list_runtime_entities scene=DontDestroyOnLoad` shows the manager; its script state (`get_runtime_entity`)
kept a counter; reload Level twice (lifecycle 9.4) and still one manager; `play stop`: no DontDestroyOnLoad scene in
`get_play_state`; Console shows the manager's `onDestroy`.
Exported smoke test (manual or CI where a display exists): a Linux export of the same project logs the same lines.

## 12. Acceptance criteria

- [ ] Persistent objects survive `loadScene` with scripts, coroutines, timers, sounds and bodies, in editor Play and an
      exported Linux build; Web/Android/iOS builds compile the same code.
- [ ] Layer order, camera, audio listener and physics rules as specified and documented.
- [ ] Hierarchy row and live Inspector in Play; MCP lists and inspects the scene.
- [ ] `DontDestroyOnLoad`, `GameObject.dontDestroyOnLoad/isPersistent/persistSingleton`, `SceneManager.persistentScene`,
      `SceneManager.moveGameObjectToScene`, C++ equivalents; stubs and docs; glossary rows.
- [ ] Play stop / quit runs quit, disable and destroy callbacks and leaves nothing behind (no leaked Scene, ASan clean
      in a debug test run).

## 13. Integration points

`engine/core/manager/SceneManager.{h,cpp}:113-141, 223-229, 647-709, 760-800`; `engine/core/registry/EntityCloner.{h,cpp}`;
`engine/core/ecs/EntityManager.h` (`createEntityWithId`); `engine/core/manager/ObjectManager.{h,cpp}:303-440`;
`engine/core/script/LuaBinding.cpp:436-475` (guard), `:858-951`; `engine/core/subsystem/AudioSystem.cpp:182-197, 395-420`;
`engine/core/subsystem/PhysicsSystem.cpp` (pending velocity at body creation, joints); `engine/core/Scene.cpp:59-68,
175-189`; `engine/core/Engine.cpp:1568-1576`; `engine/core/script/binding/GameObjectClassesLua.cpp:468-472`,
`CoreClassesLua.cpp` (SceneManager binding ~880-910); `editor/Project.{h,cpp}:691-737, 4242-4266, 8828-8919, 9152-9256,
9413-9435`; `editor/window/HierarchyWindow.cpp:2622-2688`; `editor/mcp/tools/PlayTools.cpp:436-459, 700-720, 3482`;
`editor/project/ProjectPrefabRuntime.cpp:309`.

## 14. Cross-feature dependencies

- **script-lifecycle** (required first): `ScriptRuntime` (unload exclusion, quitAll, `remap`), `ScriptMessages`
  (receiver masks), the editor reload retirement (9.4), finds across the persistent scene.
- **coroutines**: `ScriptScheduler::rebind`; owned coroutines/timers survive loads with the persistent owner.
- **audio-mixer**: buses/streams are global; streamed music voices transfer the same way (handle kept).
- **debug-overlay**: its overlay scene sits above the persistent and loading scenes.
- **physics-contacts**: per-object callbacks work inside the persistent world (its own contacts).
- Risks: the move touches many components (one code path per component in `EntityCloner`); every component type needs
  a move-mode review (list them in the implementation checklist from `EntityCloner.cpp`). Rendering of persistent world
  objects above the level surprises users who expect depth sorting (documented, `moveGameObjectToScene` remedy).

## Implementation notes

Decisions taken while implementing (branch `gaps/persistence`, on `gaps/script-lifecycle`, with
`main` merged after physics-2d):

1. **When a move runs (decision 5, 9).** A move never runs inside a script callback or a
   system's loop. The safe points are: the end of the outermost `ScriptRuntime::awakeScene` while
   scenes start in a load (`SceneManager::isStartingScenes()`: a load factory runs, or the
   editor's Play start, which brackets its scene inits with `beginStartingScenes` /
   `endStartingScenes` so Play matches the exported first load), `ObjectManager::update` (before
   the pending destroys), the start of every frame (`Engine::runFrame`, before
   `applyPendingLoad`) and `SceneManager::runFactory`. A call between frames (input callbacks,
   MCP `eval_lua`) therefore moves at the start of the next frame, not "immediately" as 9 says:
   input callbacks run inside the UI system's loops, where destroying the originals is unsafe.
   `isPersistent` is true from the call; `SceneManager.persistentScene` exists from the call
   (it is created at the first `DontDestroyOnLoad`, not at the move), so
   `GameObject.instantiate(prefab, SceneManager.persistentScene)` works right after it.
2. **Who does what.** `EntityCloner::move` copies with the move mode and hands the runtime state
   over (the originals keep none of it: instances, voices, subscriptions, running actions) but
   does not destroy the originals; `ObjectManager::moveToScene` does the rest (scripts rebound,
   lifecycle states remapped, pending destroys remapped, editor listener, then the originals
   destroyed). Engine-held state per component (the move-mode review of every `copyComponents`
   type): kept: Transform/RectTransform (remapped), script instances and bound properties,
   AudioSource (voice, sample, state), Action/Tween components (state, time), ParticleSystem
   (its live particles too: better than "restart" in 6.2), Lattice jiggle, every component event
   subscription (Button, UI, Panel, Scrollbar, InputField, AudioSource, Action, players),
   Animation Player and Pose Animator runtimes **when every entity id was kept** (their bindings
   are by id; with a remapped id they restart, keeping their callbacks), Rigidbody/Rigidbody2D
   velocities and awake state (`hasPendingVelocity` fields, applied once when the target's
   physics makes the body). Reloaded as for a clone: mesh/UI/points/lines/sky/terrain/text GPU
   state, cameras' and fluids' render targets (fluids restart), UIDocument, Vertex Animator
   (its draw state is its mesh's), reflection probes, lights. Joint2D/Joint3D to a body outside
   the moved set: not copied, warning "joint '<name>' to an object outside the moved hierarchy
   was removed". Plain data components are copied as they are.
3. **Ids**: kept with `EntityRegistry::recreateEntity` when free in the target (user range), and
   the target's user counter is raised past them (a prefab instantiated there later finds its
   GameObjects above the counter).
4. **Lua instances**: `self.scene`/`self.entity` are set raw; `self.gameObject` follows (its
   cache checks scene and entity). Beyond the spec, GameObject wrappers held in the moved
   instances' own fields that point at a moved GameObject are rebound in place
   (`EntityHandle::rebind`), so `self.body = Rigidbody2D(self.scene, self.entity)` keeps working;
   wrappers held elsewhere still name the old scene (documented pitfall).
   `LuaBinding::removeScriptSubscriptions` also removes a released script's subscriptions from
   the persistent scene's components (a level script listening to the persistent HUD's button
   would otherwise be called after its scene went). The RegisterEngineEvent guard reads
   `self.scene`, `self.entity`, `__scriptRef` and `__released` raw at each call (6.2.6).
5. **C++**: `ScriptBase::rebind` (engine-internal) is called by the engine for every moved
   `ScriptBase`; the generated code gets `moveEntityScripts` (registered with
   `ObjectManager::setCppScriptMoveHook` in the generated `init()` and by the editor's
   `Conector`) for the GameObject-wrapper kind of script and the wrappers entity-reference
   properties own (`rebindScriptObject` / `rebindScriptReference` in `ScriptBase.h`). So 8.3's
   "no generated code changes" does not hold: the hook is the only place those classes are
   known. Added for parity with Lua: `ScriptBase::isPersistent()` and
   `persistSingleton<T>()`.
6. **Cameras**: the persistent scene adopts the first Camera a moved GameObject brings while it
   draws with its default (system entity) camera. Any scene whose camera GameObject is moved
   away falls back to its own default UI camera (`Scene::useDefaultCamera`, new).
7. **Editor**: the factory makes a runtime scene of the session (`PlayRuntimeScene::persistent`,
   a `SceneProject` named "DontDestroyOnLoad", Play id, prepared like a Play copy, paused when
   the game is); its destroy only lets go (`destroyPersistentScene` is called at Stop after the
   scripts are released; the session deletes the copy). Outside Play the editor installs a
   refusing factory (`Project::refusePersistentScenes`, also at App setup): `DontDestroyOnLoad`
   in edit mode logs a warning and moves nothing. The load factory's stale and retire logic skip
   the persistent entry. `getRunningRuntimeLayerScenes` is now sorted by the Engine's layer
   order (the Game view re-activation re-adds layers in that order, so the persistent scene stays
   above later layers). Moved authored GameObjects stay listed in the target's `entities`
   (editable in the Inspector), the selection and the Inspector's scene follow
   (`Project::onPlayObjectsMoved`, through `ObjectManager::setMoveListener`). The Hierarchy row
   is the last root row with `ICON_FA_THUMBTACK` and the child-scene text color: the editor has
   no italic font. Its context menu shows only information.
8. **MCP**: `persistent: true` is present only when true (running scene entry, live entity).
9. **ScriptMessages after merging physics-2d** (one mechanism): `cppMask<T>()` is every bit for a
   `ScriptBase`; the base virtuals (lifecycle ones too, now out of line) mark
   `ScriptBase::inheritedMessages` and `baseMessageRan`, so a non-overriding class stops being
   called and counted after the first message (lifecycle test assertions updated). An instance
   the lifecycle never saw (an entry made by hand, its scene not started) counts as live for
   `send`. Order stays the Script component order (script-lifecycle note 4).
10. **Tests**: `editor/tests/PersistenceTest.cpp` (16 tests) covers 11.1-11.11 except the
    coroutine and `invokeRepeating` parts of 11.3 (the `coroutines` feature is on its own
    branch; engine events across the load are tested instead). Where the box has no audio
    output, 11.4 checks the hand-over on a made-up handle without running a frame with it.
    `tools/mcp/tests/test_persistence.py` covers the MCP story. The exported Linux smoke test
    is not automated.
11. **What other branches wire when they merge** (the hook points are in the code):
    - `coroutines`: `ScriptScheduler::rebind(entry.instance, to, newEntity)` for every moved
      instance in `ObjectManager::moveToScene` (marked comment there); persistent owners are
      never unloaded, so their coroutines survive loads with no other change.
    - `http` (`AsyncOwners`): its `entityDestroyed` hook would end requests owned by a moved
      GameObject when the originals are destroyed: add an `AsyncOwners::entitiesMoved(from, to,
      map)` and call it in `ObjectManager::moveToScene` before `from->destroyEntities`. Owners
      keyed by script instance need nothing.
    - `audio-mixer`: voices are kept with the AudioSource (handle, sample; the original's
      `destroySound` no longer stops anything); per-source bookkeeping it keeps by entity
      (one-shots, fades) must be re-owned in `handOver`/`moveToScene` with the map.
    - `physics-contacts`: receiver masks are recomputed through `notifyReceiversChanged` for
      the old and new ids; contacts never cross worlds.
    - `debug-overlay`: draw above the persistent and loading scenes; count the persistent scene.
