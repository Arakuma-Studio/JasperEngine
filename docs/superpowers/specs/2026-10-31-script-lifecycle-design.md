# Script lifecycle: Unity lifecycle parity for Lua and C++ scripts (design)

Feature `script-lifecycle` (ledger #17, branch `gaps/script-lifecycle`). Binding conventions:
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) "Shared conventions". Lua reference:
[lua-api.md](../../mcp/guide/lua-api.md). This spec also defines the **per-object script message
mechanism** that `physics-contacts`, `coroutines`, `persistence` and the animation events use (section 6.1).

## 1. Goals

1. Unity's MonoBehaviour lifecycle for Lua and C++ scripts: `init` (Awake), `start` (Start, may wait),
   `onEnable`, `onDisable`, `onDestroy`, `onApplicationPause`, `onApplicationFocus`, `onApplicationQuit`,
   called by name when the script defines them, with Unity's rules for activation, enabling,
   instantiate, destroy, scene load/unload and Play stop.
2. Enabling/disabling an individual script from Lua (`self.enabled = false`, `other.enabled = true`),
   from C++ and from the Inspector during Play, with the callbacks firing.
3. Finding scripts and objects: `obj:getScript("Enemy")`, `getScripts`, `getScriptInChildren/InParent`,
   `GameObject.findObjectOfType("Enemy")` / `findObjectsOfType` across **all running scenes**
   (layers, child scenes, the persistent scene), `GameObject.find("Path/Name")`, `findWithTag`.
4. Unsubscribing events from Lua (`UnregisterEvent`, `UnregisterEngineEvent`, `event:remove(tag)`) and a
   Lua-constructible `Event` (UnityEvent-like: `addListener/removeListener/invoke`).
5. One shared, precisely specified C++ mechanism to call a named method on every script of a GameObject
   (Lua and C++), with delivery rules (alive / active / enabled), used by this feature and by
   `physics-contacts` (onCollision*/onTrigger*) and the Animation Player events.
6. Editor Play matches exported builds: reloading the current scene really rebuilds it (today it is a
   no-op in the editor, section 9.4).

## 2. Non-goals

- **Inspector-wired persistent calls** (UnityEvent lists in the Inspector, Button "On Click ()" rows).
  The engine's established pattern is code: the code editor's event helper (`editor/util/ScriptEvents.cpp`)
  writes `RegisterEvent(...)` lines, and agents port Unity persistent calls to Lua (the game's ~390 calls are
  `SetActive`, `EquipItem`, ... one line each). A persistent-call list needs a new `ScriptPropertyValue`
  alternative, Stream/Factory/Inspector/MCP surfaces; it is a follow-up feature, not this one.
- Calling `onUpdate`/`onFixedUpdate`/`onPostUpdate` by name without `RegisterEngineEvent` (existing scripts
  register them deliberately; `check_script` warns instead, 8.2).
- Script Execution Order settings, `OnValidate`, `Reset`, `OnGUI`, `OnBecameVisible/Invisible`,
  `SendMessage`/`BroadcastMessage` with arbitrary names from Lua (the C++ mechanism supports names; Lua
  gets only `getScript` + direct calls, which is faster and type-checked by LuaLS).
- Collision/trigger callbacks themselves (feature `physics-contacts`; it uses section 6.1).

## 3. Users and stories

- Human porting from Unity: "I put `function Panel:onEnable()` in my script and it runs every time the
  panel is shown; `onDisable` unsubscribes; `self.enabled = false` stops `onUpdate`."
- Human: "I untick a script's checkbox in the Inspector during Play and its `onDisable` runs."
- Human: "`GameObject.findObjectOfType("ScoreManager")` finds the manager in the HUD layer from a script in
  the level scene."
- Agent porting C#: maps `Awake`->`init`, `Start`->`start` (with `Coroutine.wait` inside), `OnEnable`,
  `GetComponent<Enemy>()`->`obj:getScript("Enemy")`, `FindObjectsOfType<BREntity>()`->
  `GameObject.findObjectsOfType("BREntity")`, `event += h` / `-= h` -> `ev:addListener/removeListener`.
- Agent testing: `get_runtime_entity` shows each script's `enabled`, `active_and_enabled`, `started`;
  `set_runtime_property` `scripts[1].enabled=false` fires `onDisable`; `check_script` warns
  "`function M:Update()` is never called by the engine".

## 4. Research summary

- Unity execution order (https://docs.unity3d.com/2022.3/Documentation/Manual/ExecutionOrder.html):
  Awake -> OnEnable per object, all before any Start; Start before the first FixedUpdate/Update of that
  script; OnApplicationPause at end of frame; quit: OnApplicationQuit, OnDisable, OnDestroy.
- Awake (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/MonoBehaviour.Awake.html): called
  even if the script is disabled, only when the GameObject is active (else deferred to activation), once.
- Start (…/MonoBehaviour.Start.html): only if enabled, exactly once, may be a coroutine; objects
  instantiated at runtime get Start before their first Update.
- OnEnable/OnDisable (…/MonoBehaviour.OnEnable.html, OnDisable.html): on activeInHierarchy changes,
  `enabled` changes, Instantiate, Destroy (OnDisable before OnDestroy), scene unload. The enabled
  checkbox gates Start/Update/OnEnable/OnDisable but not Awake (…/MonoBehaviour.html).
- OnDestroy (…/MonoBehaviour.OnDestroy.html): only for objects that were previously active; on Destroy,
  scene unload and exiting Play mode.
- FindObjectOfType (…/Object.FindObjectOfType.html): first *active* loaded object; `includeInactive`
  overload. Godot `get_tree().get_nodes_in_group` and Roblox `FindFirstChildOfClass` take the same shape.
- Godot signals (https://docs.godotengine.org/en/stable/classes/class_object.html): connections are
  dropped when the target object is freed; we take that rule for `Event` listeners owned by a script.
- Taken: Unity's names and rules (users come from Unity). Left: Unity's undefined cross-object order (we
  define deterministic orders: all `init` then all `onEnable`, pre-order hierarchy, scenes in Engine order),
  Unity's "OnDestroy of the old scene after Awake of the new one" gotcha (we unload first, 9.3).

## 5. Product decisions

1. **`init` stays the name of Awake** (existing scripts keep working). New by-name methods: `start`,
   `onEnable`, `onDisable`, `onApplicationPause`, `onApplicationFocus`, `onApplicationQuit`; `onDestroy`
   gains Unity's scene-unload and Play-stop calls. Lowercase per engine style.
2. **Instances exist for every script entry** (enabled or not), created when the scene/instantiate
   creates the GameObject, so `getScript` works on disabled scripts and inactive objects (Unity
   GetComponent). Today disabled entries never get an instance (`LuaBinding.cpp:1081`): that changes.
3. **`init` runs when the GameObject is first active in hierarchy, whether or not the script is
   enabled** (Unity Awake). Scripts on inactive objects init on their first activation, inside
   `setActive(true)`. Migration note in docs: "a disabled script's `init` now runs (Unity Awake); check
   `self.enabled` or remove the script to keep it fully off".
4. **Deterministic passes**: scene load = all instances, all references, all `init` (pre-order), all
   `onEnable` (pre-order), then documents start; `start` runs at the next frame's start point.
5. **Scene unload runs `onDisable` then `onDestroy` for the old scenes before the new scenes' `init`**
   (singletons that clear `Manager.instance = nil` in `onDestroy` do not clobber the new instance).
6. **Play stop = application quit**: `onApplicationQuit` to all, then `onDisable`, then `onDestroy` for
   every live instance (Unity). Exported builds do the same in `Engine::systemShutdown` when it runs (the OS
   may kill a mobile app without it: documented). Pitfall 22 of lua-api.md is rewritten.
7. **`self.enabled` is a real property** of every Lua script instance (intercepted via metatables),
   plus `self.isActiveAndEnabled` (ro) and `self.gameObject` (ro, cached GameObject wrapper).
   Reserved names: a `properties` entry named `enabled`, `isActiveAndEnabled` or `gameObject` is an error.
8. **Deactivation stops the object's coroutines; disabling a script does not** (Unity). Timers (`invoke`)
   keep running in both cases (Unity's Invoke; documented) and die with the instance.
9. **Find functions search all running scenes** in `Engine` order (main first, then layers bottom to top,
   then the persistent scene), entities by ascending id within a scene; active objects only unless
   `includeInactive` is true. Deterministic, documented.
10. **C++ scripts get the same methods as `ScriptBase` virtual overrides**, dispatched through the
    `ScriptMessages` contract that `physics-contacts` defined (one mechanism; a per-class mask skips classes that
    do not override). `ScriptBase` gains `isEnabled()/setEnabled()`.
11. **`Event` is a Lua class implemented in C++** (`ScriptEvent`), whose listeners added with an owner
    script are dropped automatically when that script is released (Godot rule).
12. **Editor reload rebuilds the scene** from its Play-start state (like exports recreate it).

## 6. Design: runtime C++

### 6.1 Shared mechanism: `ScriptMessages` + `ScriptRuntime`

The per-object message mechanism is the **`ScriptMessages` contract already specified by `physics-contacts`
(its section 6.1.1, file `engine/core/script/ScriptMessages.{h,cpp}`, created by whichever branch merges
first)**. This feature adopts it unchanged and extends it as follows; there is no second mechanism.

Per-entry runtime state, new fields in `ScriptEntry` (`engine/core/component/ScriptComponent.h`), runtime
only (never serialized; `EntityCloner` resets them like `instance`, `EntityCloner.cpp:~659`):

```cpp
struct ScriptEntry {
    ... existing fields, plus physics-contacts' cppMessageMask and the entry's ScriptBase* ...
    bool initialized = false;    // init (Awake) ran
    bool enabledLatch = false;   // onEnable ran and onDisable has not since (Unity's "enabled state seen")
    bool started = false;        // start ran (or is running as a coroutine)
    bool startQueued = false;    // in ScriptRuntime's start queue
    bool destroyed = false;      // onDestroy ran (or is not due); no more messages
};
```

Extensions of `ScriptMessages` (all in the same files):

```cpp
enum class ScriptMessage : uint8_t {
    CollisionEnter, ..., JointBreak, JointBreak2D,          // physics-contacts, unchanged order
    // script-lifecycle:
    Init, Start, Enable, Disable, Destroy, ApplicationPause, ApplicationFocus, ApplicationQuit,
    Count                                                    // 22 < 32: fits ScriptMessageMask
};
// methodName(): "init", "start", "onEnable", "onDisable", "onDestroy", "onApplicationPause",
// "onApplicationFocus", "onApplicationQuit" for the new values.

class ScriptMessages {
    ... physics-contacts API: methodName, receivers, send, addReceiversChangedListener ...
    // One entry only (lifecycle targets a single script). Same rules as send() for that entry.
    static bool sendTo(Scene*, Entity, size_t scriptIndex, ScriptMessage,
                       const std::function<int(lua_State*)>& pushArgs,
                       const std::function<void(ScriptBase&)>& callCpp);
    // A method given by name, enabled entries only (animation events: today's callLuaScriptMethod rule).
    static bool sendNamed(Scene*, Entity, const std::string& method, const std::vector<AnimationEventArg>& args);
};
```

Rules added to `send`/`sendTo` by this feature: entries with `!initialized || destroyed` are skipped (an object
never active, or already destroyed, gets no message). `send` stays "enabled or not" (physics-contacts D4);
callers that need "active in hierarchy" check it before calling (physics: per contact; broadcasts below).
`LuaBinding::callLuaScriptMethod` (`LuaBinding.cpp:1016`) becomes `sendNamed` (keeps its behaviour for
`ActionSystemPlayers.cpp:941` and `VertexAnimatorSystem.cpp:1281`). The receiver mask is recomputed when an
instance is created/released or enabled/disabled (physics-contacts' `ReceiversChanged`), which this feature's
`setScriptEnabled` and instance creation trigger.

C++ dispatch uses the contract's `callCpp(ScriptBase&)`: `ScriptBase` (`ScriptBase.h`) gets a virtual
destructor (physics-contacts) and empty virtuals `init()`, `start()`, `onEnable()`, `onDisable()`,
`onDestroy()`, `onApplicationPause(bool)`, `onApplicationFocus(bool)`, `onApplicationQuit()`; the generated
factory's `cppMessageMask<T>()` trait (physics-contacts) covers them too, so non-overriding classes cost nothing.
C++ "subclass kind" scripts (a GameObject wrapper subclass, `ScriptCreateDialog.cpp:117-179`) do not derive from
`ScriptBase` and get no lifecycle methods (documented; they are object wrappers, not behaviours).

Lifecycle driver (new `engine/core/script/ScriptRuntime.{h,cpp}`), built on `ScriptMessages`:

```cpp
class JASPERENGINE_API ScriptRuntime {
public:
    static void awakeScene(Scene*);                                   // init + onEnable passes of a new scene (6.2)
    static void awakeEntities(Scene*, const std::vector<Entity>&);    // instantiate (6.2)
    static void onActiveInHierarchyChanged(Scene*, const std::vector<Entity>& changed, bool nowActive); // 6.3
    static void setScriptEnabled(Scene*, Entity, size_t index, bool enabled);                          // 6.4
    static void flushStarts();                                        // frame start point (6.5, 9.1)
    static void destroyEntities(Scene*, const std::vector<Entity>&);  // onDisable + onDestroy, before release (6.6)
    static void unloadScenes(const std::vector<Scene*>&);             // scene unload: same, whole scenes
    static void quitAll();                                            // onApplicationQuit + unloadScenes(all)
    // every running scene (Engine order) + the persistent scene, entities by id, active-in-hierarchy only
    static void broadcast(ScriptMessage, const std::function<int(lua_State*)>& pushArgs,
                          const std::function<void(ScriptBase&)>& callCpp);
    static bool hasStarted(Scene*);                                   // the scene's scripts were awakened
    static void forgetScene(Scene*);                                  // drops queued starts of a deleted scene
    static void remap(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>&); // persistence moves
};
```

Lua errors in any message are logged as today (`Lua <method>() failed for '<Class>' on '<entity name>':
<traceback>`) and the loop continues.

Latch rule (the only place `onEnable`/`onDisable` are called), `refresh(scene, entity, index)`:

```
want = entry.instance && entry.initialized && !entry.destroyed
       && scene->isEntityActiveInHierarchy(entity) && entry.enabled
if (want && !entry.enabledLatch) { entry.enabledLatch = true;  call onEnable;  queueStart(entry) }
if (!want && entry.enabledLatch) { entry.enabledLatch = false; call onDisable }
```

The entry is looked up again by (scene, entity, index) after each call (callbacks may relocate arrays or
destroy the entity). Because the latch is checked and set before the call, nested changes made by the
callback (a script disabling itself in `onEnable`) produce the matching `onDisable` afterwards and never
a duplicate. `queueStart` adds the entry to the start queue unless `started || startQueued`.

### 6.2 Scene start (`awakeScene`)

`LuaBinding::initializeLuaScripts(scene)` (`LuaBinding.cpp:1066`) becomes:
1. Pass 1: create Lua instances for **every** Lua entry (drop the `enabled` filter at `:1081`, `:1093`,
   `:1107`).
2. Pass 2: resolve references (unchanged).
3. (C++ instances are created and bound by the generated `initScripts` between pass 2 and 4; 7.3.)
4. `ScriptRuntime::awakeScene(scene)`: pass A, pre-order over entities (`ObjectManager::pushActiveScene`
   held as today): for each entry with an instance whose GameObject is active in hierarchy: set
   `initialized = true` **before** calling `init` (so a nested activation does not init twice), call
   `init`. Pass B, same order: `refresh` (onEnable + start queued).
5. `startDocumentScripts()` (unchanged, last).

Pre-order = the scene's hierarchy order (parents before children, siblings in order) as
`EntityCloner::collectHierarchy` produces per root; roots in entity-list order. A new helper
`EntityRegistry::getHierarchyOrder()` returns it.

The generated `initScripts` (`Generator.cpp:784`) is split so Lua and C++ share the passes: it calls
`LuaBinding::createLuaScripts(scene)` (passes 1-2), `createCppScripts` + `bindCppScriptProperties` per
entity (unchanged), then `ScriptRuntime::awakeScene(scene)` and
`scene->getSystem<UIDocumentSystem>()->startDocumentScripts()`. `LuaBinding::initializeLuaScripts(scene)`
keeps its signature for Lua-only paths (editor without plugin, `Project.cpp:8736,8818`) and does the same
sequence without the C++ step.

Instantiate (`ObjectManager::initializeScripts`, `ObjectManager.cpp:368`): Lua instances (passes 1-2 for
the new entities), C++ hook (create + bind), then `ScriptRuntime::awakeEntities(scene, created)` (passes A
and B over `created` in order). So `GameObject.instantiate` returns after `init` and `onEnable` ran (Unity),
and `start` waits for the next frame start point.

### 6.3 Activation changes

`EntityRegistry::setEntityActive(entity, active)` (`EntityRegistry.cpp:344`) computes, before and after the
change, `activeInHierarchy` of `entity`; when it flips, it collects the subtree entities whose
`activeInHierarchy` flipped (descendants whose own `activeSelf` and every ancestor up to `entity` are
active), in pre-order, and calls a new virtual `EntityRegistry::onActiveInHierarchyChanged(changed,
nowActive)`. `Scene` overrides it to call `ScriptRuntime::onActiveInHierarchyChanged`, only when the scene's
scripts have started (a flag set by `awakeScene`, cleared by unload; edit-time scenes never dispatch):

- `nowActive`: pass A `init` for entries not yet `initialized` (pre-order), pass B `refresh` (onEnable).
- `!nowActive`: `refresh` on every entry (onDisable, pre-order), then
  `ScriptScheduler::stopCoroutinesOf(instance)` for every instance in the subtree (feature `coroutines`;
  until it merges this call does not exist).

Synchronous: `go:setActive(false)` returns after the `onDisable` calls. `setEntityActive` stays the single
entry point (Lua `EntityHandle.setActive`, `Scene:setEntityActive`, C++, Inspector, MCP all reach it).
Reparenting (`addEntityChild`) under an inactive/active parent is an activation change too: the same check
runs in `EntityRegistry::addEntityChild` for the moved subtree.

### 6.4 Enabling a script

`ScriptRuntime::setScriptEnabled(scene, entity, index, enabled)`: sets `entry.enabled`, then `refresh`.
Callers: Lua `self.enabled = x` (6.7), C++ `ScriptBase::setEnabled`, the Inspector checkbox and MCP during
Play (8.1, 8.2). Setting the same value is a no-op. Disabling does not stop coroutines or timers (decision 8).

### 6.5 Start

`ScriptRuntime` keeps a FIFO of `{Scene*, Entity, index}`. `flushStarts()` (frame point 9.1) takes the
current queue (entries queued during the flush wait for the next frame), and for each entry: skip if gone,
destroyed, or `started`; clear `startQueued`; if not active-and-enabled now, leave `started=false` (the next
`refresh` that latches queues it again); else `started = true` and call `start`:
- Lua: through `ScriptScheduler::startCoroutine(owner=instance, fn=self.start, self)` when the feature
  `coroutines` is present (so `start` can `Coroutine.wait`); before it merges, a direct pcall.
- C++: the message hook (`start()`).

Scripts of a scene loaded by `SceneManager::loadScene` start in the first frame after the load (the load
happens at `applyPendingLoad`, before the flush, so it is the same frame). The very first scene (loaded inside
the generated `init()`) starts in the first frame.

### 6.6 Destroy, unload, quit

- `ObjectManager::cleanupScripts` (`ObjectManager.cpp:385`) calls `ScriptRuntime::destroyEntities(scene,
  entities)` first: pass 1 `refresh` with `want=false` forced (onDisable, pre-order); pass 2 stop
  coroutines/timers of their instances (feature `coroutines`); pass 3 for entries `initialized &&
  !destroyed`: set `destroyed=true`, call `onDestroy`; entries never initialized get `destroyed=true`
  silently. Then the existing release (`LuaBinding::cleanupLuaScripts(scene, entities)` minus its own
  `onDestroy` loop at `:1186-1196`, and the C++ hook).
- `ScriptRuntime::unloadScenes(scenes)`: the same three passes over all entities of all given scenes
  (pass 1 for all scenes, then pass 2, then pass 3) and clears each scene's "scripts started" flag.
  `LuaBinding::cleanupLuaScripts(scene)` (scene-wide, `:1162`) and the generated `cleanupScripts` call
  `unloadScenes({scene})` first (a no-op for entries already `destroyed`), then release.
- Scene loads: `SceneManager::runFactory` (`SceneManager.cpp:113`) snapshots `Engine::getSceneLayers()`
  before `Engine::removeAllScenes()`, and after re-adding the loading scenes calls
  `ScriptRuntime::unloadScenes(previous - loadingScenes - persistentScene)` **before** `loadFactory()`. It
  exposes that list as `SceneManager::getUnloadingScenes()` while the factory runs (the editor needs it, 9.4).
- `quitAll()`: `ApplicationQuit` to every initialized, not destroyed instance of every running scene and the
  persistent scene (active or not), then `unloadScenes(every running scene + persistent scene)`. Called by the editor's Play stop (`Project.cpp:9190`, before `Engine::onShutdown`)
  and by `Engine::systemShutdown` (`Engine.cpp:1570`, before `onShutdown`).
- Application pause/focus: `Engine::systemPause/systemResume` (`Engine.cpp:1580-1592`) broadcast
  `ApplicationPause` with `true`/`false` (after `AudioSystem::pauseAll` on pause, before `onResume` on resume).
  Focus: `broadcast(ApplicationFocus, bool)` from `Engine::systemFocus(bool hasFocus)`, which feature
  `platform-services` defines (its 6.1: idempotent, fires `Engine.onFocus`, called by every platform; order
  `onFocus(false)` before `onPause`, `onResume` before `onFocus(true)`); the per-object broadcast runs right after
  `Engine.onFocus`. If this feature merges first it adds `systemFocus` with that signature and the Android call
  (`NativeEngine.cpp:1382-1389`) only; platform-services extends it.

### 6.7 Lua instance plumbing

`createLuaScriptInstance` (`LuaBinding.cpp:858`) sets on the instance metatable, besides `__index =
module`, a `__newindex` C function: key `enabled` -> `ScriptRuntime::setScriptEnabled` for this instance
(found through `self.scene`, `self.entity` and `__scriptRef`); keys `isActiveAndEnabled`, `gameObject`
-> error "<key> is read-only"; anything else -> `rawset`. The module table gets (when it has no metatable)
`setmetatable(module, {__index = ScriptInstanceBase})`; if it already has a metatable without `__index`,
`__index` is added; if it has its own `__index` (an inheritance chain the script built), that metatable's
`__index` is replaced by a C closure that tries the original `__index` (table or function) first and then
`ScriptInstanceBase`. `ScriptInstanceBase` is one registry table holding the methods of 7.1 (and those of the
`coroutines` feature) whose own metatable `__index` is a C function answering `enabled`,
`isActiveAndEnabled` and `gameObject`. User fields are never slowed down (they are raw fields of the
instance; methods are found in the module table first). The module table is per instance (the chunk runs
per instance, lua-api.md 1.1), so setting its metatable affects no other script.

## 7. Lua and C++ API

### 7.1 Lua: script instance members (any script table `self` or one returned by `getScript`)

| Member | Kind | Semantics |
|---|---|---|
| `enabled` | rw bool | the entry's enabled flag; assigning fires `onEnable`/`onDisable` (6.4) |
| `isActiveAndEnabled` | ro bool | GameObject active in hierarchy and `enabled` |
| `gameObject` | ro `GameObject` | wrapper of the script's GameObject (cached, refreshed if the object moves scene) |
| `getScript(className)` | method | instance of that class on the same GameObject, or nil |
| `getScripts([className])` | method | list of instances (all scripts when no name) |
| `getScriptInChildren(className [, includeInactive=false])` | method | self first, then descendants pre-order |
| `getScriptsInChildren(className [, includeInactive=false])` | method | list |
| `getScriptInParent(className [, includeInactive=false])` | method | self, then ancestors upward |

`EntityHandle` (so every wrapper) gets the same five `getScript*` methods for its entity. `className` is
a Lua script class name; a non-string argument errors ("getScript: className must be a string"). Disabled
scripts are returned (Unity GetComponent). C++ script entries are skipped (Lua cannot hold them).

### 7.2 Lua: by-name methods the engine calls

| Method | When | Args |
|---|---|---|
| `init(self)` | once, first time the GameObject is active in hierarchy (enabled or not) | - |
| `onEnable(self)` | becomes active-and-enabled (after `init` in load/instantiate/activation) | - |
| `start(self)` | once, at the frame start point after the first `onEnable`; may wait (coroutine) | - |
| `onDisable(self)` | stops being active-and-enabled; before `onDestroy` | - |
| `onDestroy(self)` | once, on destroy, scene unload, Play stop/quit; only if `init` ran | - |
| `onApplicationPause(self, paused)` | app goes to background (`true`) / returns (`false`) | bool |
| `onApplicationFocus(self, focused)` | app focus change (platforms that report it) | bool |
| `onApplicationQuit(self)` | Play stop / app shutdown, before `onDisable`/`onDestroy` | - |

Application messages go to every initialized instance on an active GameObject (enabled or not).

### 7.3 Lua: finding objects (static on `GameObject`)

| Function | Returns |
|---|---|
| `GameObject.find(nameOrPath)` | `GameObject` of the first active object named so in any running scene (`"A/B/C"` = path from a root), else nil |
| `GameObject.findWithTag(tag)` / `GameObject.findGameObjectsWithTag(tag)` | first active / list of active, all scenes; undefined tag errors like `findByTag` |
| `GameObject.findObjectOfType(className [, includeInactive=false])` | first script instance of that Lua class, else nil |
| `GameObject.findObjectsOfType(className [, includeInactive=false])` | list (possibly empty) |

`className` may also be a built-in wrapper name of lua-api.md 1.2 (`"Camera"`, `"AudioSource"`,
`"Rigidbody"`, ...): then the result is that wrapper for every entity having the wrapper's component
(a table `wrapperName -> component` in `LuaBinding`, next to `pushEntityHandleByType`; names without a
component, like `GameObject`, `EntityHandle`, error "not a component type"). Scene order: decision 9.
Inactive = not active in hierarchy, or (for scripts) not enabled: `includeInactive=false` returns only
active-and-enabled scripts, like Unity. Global functions `FindObjectOfType`/`FindObjectsOfType` are not
added (one spelling).

### 7.4 Lua: events

- `UnregisterEvent(self, eventObject, methodName [, tag])` and `UnregisterEngineEvent(self, methodName
  [, tag])`: compute the same tag as registration (`LuaBinding.cpp:399-417`) and call
  `eventObject:remove(tag)`; return true if a subscription was removed. Unknown Engine event name errors
  like registration.
- Every bound `FunctionSubscribe_*` class (`CoreClassesLua.cpp:938-1000` and the others) gets
  `remove(tag) -> bool` and `has(tag) -> bool`.
- `Event()` constructs a `ScriptEvent` userdata:
  - `ev:addListener(fn)` / `ev:addListener(self, "method")` (the second form is owned by `self`);
    adding the same fn (or same owner+method) twice is ignored (DOM rule); returns nothing.
  - `ev:removeListener(fn)` / `ev:removeListener(self, "method")` -> bool; `ev:removeAllListeners()`.
  - `ev:invoke(...)` calls listeners in add order with the given args; a listener error is logged
    with its traceback and the others still run; listeners added during `invoke` run from the next
    invoke; removed ones are skipped at once.
  - `ev.listenerCount` (ro).
  - Also `add(tag, fn)`/`remove(tag)` so `RegisterEvent(self, ev, "method")` works on it.
  - Owned listeners whose owner instance has been released (`releaseLuaScriptInstance` sets
    `rawset(self, "__released", true)`) are dropped at the next `invoke`/`listenerCount`.
  Storage: registry refs, main thread (coroutines feature normalisation); released with the userdata (`__gc`).

### 7.5 C++ scripts

- Methods are `ScriptBase` virtual overrides (6.1): `void init() override`, `start`, `onEnable`, `onDisable`,
  `onDestroy`, `onApplicationPause(bool)`, `onApplicationFocus(bool)`, `onApplicationQuit()`. The generated
  factory (`Generator.cpp:663-683`) stores the entry's `ScriptBase*` and `cppMessageMask<T>()` (physics-contacts
  6.1.1); `ScriptMessages::send/sendTo` call them through `callCpp`. The editor plugin (`Conector`) uses the same
  generated code, so editor Play and exports dispatch identically.
- C++ `init()` runs after properties are bound (the constructor still runs first, before binding; the
  docs recommend `init()` for anything reading properties).
- `ScriptBase` gains `bool isEnabled() const; void setEnabled(bool);` (find its own entry by `this`
  like `isScriptActive`, `ScriptBase.cpp:9`) and `template<class T> T* getScript() const` (first C++ entry
  on the same GameObject whose `className` equals the class name the generated code registers for `T`
  through `JASPERENGINE_SCRIPT_CLASS(T, "Name")`, emitted next to `createCppScripts`). Scene-wide finds
  stay Lua-only (C++ code iterates `Engine::getSceneLayers()` itself).

## 8. Editor UX, MCP, docs

### 8.1 Editor
- Inspector script checkbox (`InspectorWindow.cpp:10293-10316`): on a running Play scene its
  `PropertyCmd<bool>("scripts[N].enabled")` goes through `ScriptRuntime::setScriptEnabled`; the Catalog
  setter (`Catalog.cpp:1100-1124`) checks "scene's scripts started" and calls it instead of writing the
  field. Edit mode unchanged.
- Inspector during Play shows, under each script header, a dim status: `not initialized` (object never
  active), `started`/`waiting to start`, matching MCP fields.
- The Active checkbox during Play already calls `setEntityActive` (fires callbacks; nothing to add).

### 8.2 MCP
- `get_runtime_entity` (`PlayTools.cpp:3482`) script entries gain `enabled`, `initialized`, `started`,
  `active_and_enabled`.
- `set_runtime_property` with `scripts[N].enabled` and `update_script_entry` `enabled` during Play go
  through `setScriptEnabled`; `set_entity_state`/`set_runtime_property` `active` go through
  `setEntityActive` (already).
- `check_script` (`ScriptTools.cpp`) warnings (static text scan of `function <Class>[:.]<name>`):
  Unity-style names never called (`Awake`, `Start`, `Update`, `FixedUpdate`, `LateUpdate`, `OnEnable`,
  `OnDisable`, `OnDestroy`, `awake`, `update`, `fixedUpdate`, `lateUpdate`) with the engine name to use;
  `onUpdate`/`onFixedUpdate`/`onPostUpdate` defined but never passed to `RegisterEngineEvent`; a
  `properties` entry named `enabled`/`isActiveAndEnabled`/`gameObject`.
- Tool descriptions: no new tool (budget). `McpInstructions.cpp` one line: "lifecycle: init (Awake),
  start, onEnable/onDisable, onDestroy are called by name; see lua-api.md 1.1".

### 8.3 Docs
- `lua-api.md` 1.1 rewritten (lifecycle table, order, migration note), 1.3 (Unregister*, `remove`,
  `Event`), 4.1 (`getScript*`), 2.6 (`GameObject.find*`), pitfalls 18 and 22 rewritten, section 6 new
  pitfall "self.enabled / reserved property names".
- `cpp-scripting.md`: detected methods, `init()` after binding, `setEnabled`.
- `unity-glossary.md`: Awake -> `init`, Start, OnEnable/OnDisable, `enabled`, GetComponent<Script> ->
  `getScript`, FindObjectOfType, GameObject.Find, UnityEvent -> `Event`, `-=` -> `removeListener`/`UnregisterEvent`.
- Stubs: `generate_lua_stubs.py` inline `Script.lua` (`:2955-2999`) gains members of 7.1/7.2 as
  `ScriptInstance` fields/methods with doc comments; `Event` class; `GameObject.find*`; keep
  `-- unresolved:` empty.

## 9. Behaviour rules and edge cases

### 9.1 Frame order (shared with `coroutines`, `persistence`, `physics-contacts`)

`Engine::runFrame` (`Engine.cpp:1336`):
1. `SceneManager::applyPendingLoad()`: unload of old scenes (onDisable, onDestroy), new scenes (init,
   onEnable), `onSceneLoaded` stays in `updateLoading`.
2. If not paused/held: **`ScriptRuntime::flushStarts()`** (new, Unity "Start before the first
   FixedUpdate"), the fixed loop (`onFixedUpdate`, scene fixed updates = physics, then physics-contacts
   callbacks, then the coroutines fixed phase), `onUpdate`, coroutines update phase, scenes update,
   `onPostUpdate`, `ObjectManager::update` (destroys: onDisable/onDestroy), compaction.
3. Draw, commit, then the coroutines end-of-frame phase, then `SceneManager::updateLoading()`.
App pause/resume messages are sent from `systemPause/systemResume` (between frames).

### 9.2 Ordering rules
- Within a pass: scenes in Engine order, entities in hierarchy pre-order, entries in component order.
- Scene load: all `init` of the new scene(s) before any `onEnable`; all scenes of a stack are awakened in
  stack order (main first) by the factory, each fully (passes A and B) before the next.
- A script's own `onEnable` always follows its `init`; `onDisable` always precedes its `onDestroy`.
- `start` of an object instantiated during `init` of a loading scene runs in the same flush as the scene's.

### 9.3 Edge cases
- `self.enabled = false` inside `onEnable`: `onDisable` follows immediately after `onEnable` returns
  (latch); `start` is not run (not active-and-enabled at flush; queued again on the next enable).
- `setActive(false)` on the object inside its own `init` during scene load: pass B skips it (no
  `onEnable`); `init` is not called again later.
- Destroy during `onEnable`/`start`: deferred destroy (end of frame) as today; `destroyImmediate` runs
  passes immediately; the caller's loop re-reads entries and skips destroyed ones.
- `GameObject.destroy(self)` in `onDestroy` or `onDisable`: ignored for an entry already `destroyed`.
- An object whose GameObject was never active: no `init`, no `onDestroy` (Unity); its instance is released.
- Re-entrant activation: `setActive(true)` inside an `onDisable` of the same subtree: the latch makes the
  final state consistent; every `onEnable` is paired with at most one `onDisable`.
- Errors in any lifecycle method are logged with traceback; the instance continues (an `init` error
  still marks `initialized`; `onEnable` error still sets the latch).
- `getScript` on a released instance's GameObject returns nil; calling methods on a released table works
  (plain Lua) but `self.enabled` assignment on it is ignored with a warning.
- Find functions during scene load see the scenes running at that moment plus the scene being built
  (`ObjectManager::getActiveScene()` stack) so a script's `init` can find objects of its own scene.
- Editor Play stop when a script errors in `onDestroy`: logged, teardown continues.

### 9.4 Editor reload (`SceneManager.loadScene` of a scene already loaded)
In the editor load factory (`Project.cpp:8834`), entries whose scene is in
`SceneManager::getUnloadingScenes()` are **retired** before `buildRuntimeSceneStack`: their scripts are
released (`conector.cleanup` or `LuaBinding::cleanupLuaScripts`, which only release since the unload pass
ran), sounds stopped, `removeScenePtr`, and the entry moves to `session->retiredRuntimeScenes` (owned
copies deleted in `cleanupPlaySession`; the played in-place scene is restored by Stop as today). The stack
is then rebuilt with fresh copies: from the source scene for child/other scenes
(`createRuntimeCloneFromSource`), and **from the Play-start snapshot** (`playStateSnapshot`) for the played
scene. The Hierarchy and MCP show the new copies (their Play ids are new). This makes Restart (reload) in
the editor behave as in exported builds.

## 10. Performance

- `ScriptMessages::send/sendTo` cost one `lua_getfield` per Lua entry (method lookup through two table hops)
  and a virtual call per C++ entry whose mask bit is set; lifecycle messages push no arguments (no allocation).
- Activation changes: subtree walk only when `activeInHierarchy` of the root flips, and only if the scene
  has started scripts; entities without a `ScriptComponent` cost one `findComponent`.
- `flushStarts` is O(queued). Find functions are linear scans of `ScriptComponent` arrays per scene
  (documented as "not for every frame", like Unity).
- Target: 10,000 entities with scripts, `setActive` on a root of 1,000 scripted children < 2 ms.

## 11. Tests

gtest (`editor/tests/ScriptLifecycleTest.cpp`, harness `editor/tests/ScriptTestHarness.h`: `Engine::systemInit`
with a test `System` whose Lua path is a temp dir, writes `.lua` files, builds a Scene with entities and
`ScriptComponent` entries, `LuaBinding::initializeLuaScripts`, steps frames with
`Engine::setFixedDeltatime(1/60.0)` + `Engine::systemUpdate()`; scripts append to a global `log` table read back):
1. Load order: two objects A (parent) and B (child), two scripts each: log = `initA1 initA2 initB1 initB2
   enableA1 ... enableB2`, then after one frame `startA1 ... startB2` before the first `onUpdate`.
2. Disabled script: `init` runs, `onEnable`/`start` don't; `self.enabled = true` -> `onEnable`, `start`
   next frame; `onUpdate` registered in init is skipped while disabled (existing guard).
3. Inactive at load: no `init`; `setActive(true)` -> `init`, `onEnable` synchronously; deactivate parent ->
   `onDisable` for child scripts in pre-order; reactivate -> `onEnable` only (no second `init`).
4. `onEnable` disabling itself -> `onEnable`, `onDisable`; no `start`.
5. Destroy: `GameObject.destroy(obj)` -> at end of frame `onDisable` then `onDestroy`; never-active object
   destroyed -> neither.
6. Scene unload via `SceneManager.loadScene`: old `onDisable`, old `onDestroy` before new `init` (one log).
7. Quit: `ScriptRuntime::quitAll()` -> `onApplicationQuit` for all, then `onDisable`, then `onDestroy`.
8. Pause: `Engine::systemPause()` -> `onApplicationPause(true)`; resume -> `false`.
9. `getScript`/`getScripts`/`InChildren`/`InParent` incl. disabled scripts and `includeInactive`.
10. `findObjectOfType` across main + layer scene, order, `includeInactive`, wrapper names (`"Camera"`).
11. `GameObject.find("Root/Child")`, `findGameObjectsWithTag`.
12. `UnregisterEngineEvent(self, "onUpdate")` stops calls; `UnregisterEvent` on a Button event; returns.
13. `Event`: add/remove/invoke order, duplicate add ignored, listener error isolated, owned listener
    dropped after its owner is destroyed, `RegisterEvent(self, ev, "m")` works.
14. Re-entrancy: `setActive(false)` on parent inside a child's `onEnable` -> consistent latch, pairs.
15. C++: a `ScriptBase` subclass compiled into the test binary with `init/start/onEnable/onDisable/onDestroy`
    overrides, its entry filled by hand like the generated factory (instance, `ScriptBase*`, mask): same log
    order as the Lua test 1.
16. Reserved property name `enabled` -> instance creation error message; `self.enabled = 1` (non-boolean)
    errors "enabled must be a boolean".
MCP (`tools/mcp/tests/test_lifecycle.py`): create script logging lifecycle, play, `get_runtime_entity`
fields, `set_runtime_property scripts[0].enabled false` -> console shows onDisable; `check_script` warnings
for `function M:Update()`; reload test: a script counting `init` calls in `UserSettings` sees 2 after
`SceneManager.loadScene(current)` in Play (editor reload fix).

## 12. Acceptance criteria

- [ ] All lifecycle methods of 7.2 are called with the rules of 6 and 9, in Play and in an exported Linux
      build (same test scene, same log), Lua and C++.
- [ ] Disabled scripts and inactive objects have instances; `init` deferred to first activation.
- [ ] `self.enabled`, `isActiveAndEnabled`, `gameObject`; Inspector checkbox and MCP fire callbacks in Play.
- [ ] `getScript*`, `GameObject.find*` work across scenes incl. layers; deterministic order.
- [ ] `UnregisterEvent`, `UnregisterEngineEvent`, `remove/has`, `Event` class.
- [ ] Old scene `onDestroy` before new scene `init`; Play stop calls quit/disable/destroy.
- [ ] Editor reload rebuilds the scene (9.4).
- [ ] `ScriptMessages` (physics-contacts contract) extended with the lifecycle messages, `sendTo`, `sendNamed`;
      animation events unchanged (existing anim tests pass).
- [ ] Docs, stubs (`test_stubs.py` green, no `-- unresolved:`), glossary, check_script warnings.

## 13. Integration points

`LuaBinding.cpp:858-951` (instance creation, metatables), `:954` (pushScriptObject), `:1000-1063`
(call helpers), `:1066-1208` (init/cleanup); `ScriptComponent.h:17-28`; `ScriptBase.{h,cpp}`;
`FunctionSubscribe.h` (remove/has bindings); `CoreClassesLua.cpp:938-1000` (FunctionSubscribe classes),
`:763-764` (pause/resume); `GameObjectClassesLua.cpp:468-472` (GameObject statics); `EntityRegistry.cpp:344-367`,
`addEntityChild`; `Scene.h:55-81` (virtual hooks); `ObjectManager.cpp:368-399`; `SceneManager.cpp:113-141`;
`Engine.cpp:1336-1490` (flushStarts), `:1568-1592` (shutdown/pause); `Generator.cpp:648-900, 2377`;
`Conector.cpp:196-214`; `Project.cpp:8700-8920, 9152-9256`; `InspectorWindow.cpp:10293-10316`;
`Catalog.cpp:1100-1124`; `PlayTools.cpp:3482-3526`; `ScriptTools.cpp` (check_script);
`ActionSystemPlayers.cpp:941`; `VertexAnimatorSystem.cpp:1281`; `generate_lua_stubs.py:2955-2999`.

## 14. Cross-feature dependencies

- **physics-contacts**: owns the `ScriptMessages` contract (its 6.1.1); this feature appends the lifecycle
  messages, adds `sendTo`/`sendNamed`, the `!initialized || destroyed` skip rule and the instance-for-every-entry
  rule (so disabled scripts receive collisions, as both specs want). Whichever merges first creates the files;
  the other extends them. Dispatch point: 9.1 step 2.
- **coroutines**: `start` runs as a coroutine; deactivation calls `ScriptScheduler::stopCoroutinesOf`;
  destroy/unload call `ScriptScheduler::cancelOwner`. Whichever merges second wires the calls.
- **persistence**: the persistent scene is included in broadcasts/finds/quit; `runFactory` excludes it
  from unload; moving a script updates `gameObject` cache. Uses 9.4.
- **platform-services**: owns `Engine::systemFocus(bool)`, `Engine.onFocus` and the pause/focus ordering; this
  feature only adds the per-object `onApplicationFocus`/`onApplicationPause` broadcasts to those entry points.
- Risk: decision 3 changes behaviour of existing projects (disabled scripts' `init`, inactive objects'
  late `init`); mitigated by the migration note and the check in tests.
