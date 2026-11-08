# Runtime core: failures reported, copies and clips that end where they should (design)

Date 2026-11-04. Branch `dc/runtime-core`. Program: Doriax catch-up, ledger
[docs/doriax-catchup/LEDGER.md](../../doriax-catchup/LEDGER.md) (feature 8).

Doriax references (read-only, `dc-work/analysis/doriax`): 4a33929ec (C++ exceptions in guarded
subscribers), 34e87d375 (a spawn whose init destroyed it), a99226dfd (action speed applied twice),
68e6be0f5 (hidden children), cc9b2502b (physics subscriptions at Stop), 45966b13a (start offset and
random start, preview snapshots). All Jasper line numbers below are from `main` at `b4f7816ea` and
were read, not run.

## 1. Goals

1. **A callback that fails is reported, never fatal and never silent.** A C++ exception thrown by
   any guarded callback (event subscribers, C++ timers, scheduler tasks, coroutine resumes) is
   caught and reported with the subscriber's tag and the exception's type and message, exactly like
   a crash or a Lua error in the same place. Without the editor's hook (exported games) it is
   logged and the game goes on (Unity). The crash guard itself stays correct when guards nest or a
   throw crosses it.
2. **Instantiate never returns a dead root.** When the copy's `init` / `onEnable` destroys it with
   `destroyImmediate`, Lua gets `nil` and C++ an empty `GameObject` (Unity's fake null).
3. **A clip ends on its last pose at any speed.** A non-looping entity Animation with an explicit
   duration and `speed != 1` samples its tracks at the end of the clip, not at `duration / speed`.
4. **Copies keep each child's own visibility.** Instantiate, scene moves (DontDestroyOnLoad,
   `moveGameObjectToScene`), removing a GameObject or its Transform, and the undo of that never
   overwrite a child's `visible` with its parent's. Scene view icons follow Active as picking does.
5. **Nothing a script subscribed survives Stop.** Every C++-side event of every component and of
   the physics system (with `onControllerColliderHit`) is cleared at Stop and on tag removal, from
   one list per holder, and a lint fails CI when an event is added without being cleared.
6. **Edits made during an edit-mode preview are kept.** The Animation window's entity preview and
   the Inspector's action preview restore only what the preview changed: a value a command wrote
   while previewing (Inspector field, MCP `set_properties`, gizmo drag, undo/redo) survives seek and
   Stop, and its undo goes back to the authored value, never to a previewed pose.
7. **Start Offset and Random Start for every action.** Looping copies (coins, torches, swaying
   plants) no longer move in lockstep: an action starts a set number of seconds into itself, or at a
   deterministic random point of one cycle, in Play and builds.
8. **Escape reaches the game.** In a focused Game view during Play, Escape goes to the game
   (`Input.getKeyDown(KeyCode.ESCAPE)`) instead of ImGui's keyboard navigation taking the focus away.

## 2. Non-goals

- Doriax's visibility redesign (own `visible` + computed `visibleInHierarchy`, 68e6be0f5). Active is
  Jasper's inheriting switch (Unity `SetActive`, EntityRegistry.cpp:678-693); `visible` stays a
  per-object flag whose explicit changes cascade (decision 4).
- Catching C++ exceptions on the Web: Emscripten builds without exception catching, a throw aborts
  there as today (documented). No `-fwasm-exceptions` (size and Safari cost; not this feature).
- Exceptions thrown from C++ functions called by Lua through LuaBridge, and `callDirect` on Jolt job
  threads: other boundaries, unchanged.
- Changing how lifecycle messages (`init`, `start`, `onEnable`, physics messages) report errors:
  `ScriptMessages::deliver` already catches C++ exceptions and logs them, as Lua errors there are
  logged (ScriptMessages.cpp:305-330). Section 6.1 states the resulting table.
- Doriax's button hover/press scale with eased transition (45966b13a part b): owned by
  `ui-components` (Unity transitions). Rejected here.
- The never-loaded-mesh override bit (45966b13a part d, Catalog.cpp:516): rejected. The skeptic
  showed `StreamComponents.cpp:50-53,160` saves and restores `numSubmeshes`, so only a registry mesh
  saved with no submeshes reaches it; no flow that produces one was found. Not worth a change
  without a reproduction; recorded for the lead.
- 4a33929ec's unsettled parts: Revert of a Model override rebuilding nodes (prefab domain) and a
  draw while a swapped texture is still loading (render domain). Neither was settled by reading and
  neither is in this mandate; they are handed to the lead as repro tasks for `scene-integrity` and
  `render-correctness`, not specified here.
- Delays (a negative start offset): `startOffset` is clamped to >= 0. A delay is a different feature
  (Animation frame start times already give one).

## 3. Users and stories

- Human, C++ scripts: `onUpdate` throws `std::out_of_range`. Today the editor dies or Play stops with
  "SIGABRT (Abort signal)". After: the Console says
  `Script crash in scene 'Level1' ... Location: Spinner_0x..._onUpdate Error: C++ exception std::out_of_range: vector::_M_range_check ...`
  and Play pauses and stops, as for a Lua error. In an exported game the log gets the same line
  (once a second per subscriber) and the game keeps running.
- Human, Lua: `local e = GameObject.instantiate(self.enemyPrefab)`; the enemy's `init` decides it
  should not exist and calls `GameObject.destroyImmediate(self.gameObject)`. `e` is `nil`, so
  `if e then ... end` is enough; today `e` is a wrapper of a dead entity and the next call errors.
- Human: a door Animation, `duration` 1, `speed` 2, ends half open today; after, fully open.
- Human: 40 coins instantiated from a prefab whose child "Glow" is hidden: today every copy shows
  the glow on the next frame. After: hidden. Same for a player moved with `DontDestroyOnLoad`.
- Human: a camera under an inactive parent no longer draws its icon in the Scene view (it could
  not be clicked anyway).
- Human: scrubbing a tween Animation in the Animation window, they change the tween's duration and
  the target's colour in the Inspector, then press Stop: both edits stay; Ctrl+Z puts the colour back
  to what it was before, not to a scrubbed frame.
- Human: a coin prefab with a spinning RotationTween, Loop on: Inspector "Random Start" on, and the
  coins spin out of step. A torch flicker Animation: "Start Offset" 0.35 on the second torch.
- Agent: `edit_animation {animation: "CoinSpin", random_start: true}`; `get_animation` reports
  `start_offset` / `random_start`; `set_properties {values: {action: {startOffset: 0.5}}}` on a
  standalone tween; `preview_animation` + `set_properties` + `preview_animation stop` keeps the edit.
- Agent: a Lua pause menu `if Input.getKeyDown(KeyCode.ESCAPE)` now works when tested in the
  editor's Game view (driven by a real key in MCP tests).

## 4. Findings

### 4.1 C++ exceptions in guarded callbacks (Doriax 4a33929ec, partial)

- Every guarded call goes through `CrashHook::runGuarded` (engine/core/util/CrashHook.cpp:51-58):
  FunctionSubscribe (FunctionSubscribe.h:136-156), ScriptScheduler coroutine resumes
  (ScriptScheduler.cpp:800-810) and C++ timers (:1054-1061), util Scheduler (Scheduler.cpp:412-428).
  None catches a C++ exception. On POSIX the throw either finds no handler (terminate, then
  `onTerminate` sets the process-wide crash reason, CrashHandler.cpp:505-527, then abort, then the
  guard's SIGABRT handler longjmps back: reported as "SIGABRT (Abort signal)" with the exception in
  flight never finished and a stale pending crash reason), or is caught higher up and leaves the
  guard armed: `callWithCrashGuard` restores the signal handlers and `g_tls.active` only on its two
  normal paths, with no RAII (CrashGuard.h:180-209). A later fault anywhere on that thread then
  longjmps into a dead frame.
- Unguarded paths: `FunctionSubscribe::call` without a hook (exported games, the editor outside
  Play) and `ScriptScheduler`'s unguarded C++ timer (`(*fn)()`, ScriptScheduler.cpp:1063) let the
  exception unwind through the engine's frame: the game terminates.
- Nested guards are broken (found here): `callWithCrashGuard` overwrites the one thread-local
  `sigjmp_buf` and sets `g_tls.active = false` when the inner guard returns. A guarded callback that
  starts an Action (whose `onStart.call()` is guarded too, ActionSystem.cpp:117-120) and faults
  after it returns reaches `crashSignalHandler` with `active == false`, which calls
  `handleFatalSignal`: the editor dies instead of stopping Play.
- On MSVC the SEH `__except(EXCEPTION_EXECUTE_HANDLER)` swallows C++ exceptions (code 0xE06D7363)
  as "EXCEPTION_UNKNOWN (Unknown structured exception)" (CrashGuard.h:40-84): no message.
- Already consistent: `ScriptMessages::deliver` catches and logs (ScriptMessages.cpp:321-330),
  util `Scheduler` catches and logs (Scheduler.cpp:414-420).

### 4.2 Instantiate returning a dead root (Doriax 34e87d375, partial)

`ObjectManager::instantiate` (all four overloads, ObjectManager.cpp:234-301) returns `root` after
`initializeScripts`, which runs `init` and `onEnable`. `destroyImmediate` outside a
`StructuralDeferScope` destroys at once (:398-408). `luaInstantiate` pushes the wrapper unchecked
(GameObjectClassesLua.cpp:454-476); the C++ `GameObject::instantiate` overloads wrap it
(GameObject.cpp:28-70). Entity ids count up (EntityManager.h:216-233), so a dead id is not reused
within the session, but every call on the handle fails. `EntityHandle` has no validity test for C++
callers (EntityHandle.h:17-60).

### 4.3 Clip speed end pose (Doriax a99226dfd, refuted as "already ported"; the skeptic found this)

`ActionSystem::animationApplyEnd` (ActionSystem.cpp:333-338) uses
`endTime = min(timecount, duration / speed)` although `timecount` already runs at the action's speed
(:221) and `durationPassed` compares `timecount >= duration` (:309). Speed 2, duration 1: the tracks
are sampled at clip time 0.5 on the last update. Speed 0 divides by zero (harmless today because
`min` with infinity).

### 4.4 Hidden child shown again (big-claims #8, confirmed; 68e6be0f5 context)

- `prepare<Transform>` sets `needUpdateChildVisibility = true` on every copied Transform
  (EntityCloner.cpp:132-137), for clones (ObjectManager.cpp:207 via `cloneObject`) and moves
  (`EntityCloner::move`, used by `moveToScene`, ObjectManager.cpp:473-480), and the editor's Play
  prefab copies (ProjectPrefabRuntime.cpp:351). `propagateTransforms` then does
  `child.visible = parent.visible` below every flagged Transform (TransformPropagation.h:34-37). A
  child hidden on its own comes back; a copy of a hidden parent hides every child.
- The same overwrite, same cause: `EntityRegistry::changeTransformChildren` (EntityRegistry.cpp:
  232-254, the children of a removed GameObject or Transform go to the grandparent with the flag
  set, so the grandchildren get overwritten) and the undo of a Transform removal
  (`RemoveComponentCmd::restoreTransformPlace`, RemoveComponentCmd.cpp:73-91).
- It contradicts `GameObject::setVisible`'s own rule (GameObject.cpp:159-169: "a child hidden on its
  own is not shown again").
- Scene view icons and camera frustums use `transform.visible` only (SceneRender3D.cpp:923, 937, 950,
  958; no active test in the update loop at :1412-1470) while the icon picker refuses inactive
  objects (IconPicker.cpp:43-49): an inactive camera's icon is drawn but cannot be clicked.

### 4.5 Subscriptions that survive Stop (cc9b2502b, refuted as "already ported"; the skeptic found this)

- `PhysicsSystem::clearSubscriptions` and `removeSubscriptionsByTag` (PhysicsSystem.cpp:517-561)
  miss `onControllerColliderHit` (PhysicsSystem.h:732). A C++ callback subscribed without an owner
  key (`add(tag, lambda)`, `operator=`) outlives `conector.disconnect()` at Stop
  (editor/Project.cpp:10412-10419): even destroying the `std::function` later runs code of the
  unloaded library. Callbacks with an owner key are removed by the generated cleanup
  (Generator.cpp:924); Lua callbacks are guarded by `isLive`.
- The class is wider (found here). `Engine::clearComponentSubscriptions` (Engine.cpp:1478-1599)
  misses every event of `NavMeshAgentComponent` (4), `ParticleSystemComponent` (3),
  `TextEffectComponent` (2), `VideoPlayerComponent` (6), `NavMeshSurfaceComponent` (1) and
  `SplineAnimateComponent` (1). `Scene::removeSubscriptionsByTag` (Scene.cpp:127-221), a second hand
  list, misses those (but SplineAnimate) and `UIDocumentComponent`. Engine's own static events are
  complete in both lists (checked with a script over Engine.h).

### 4.6 Preview restore drops edits (45966b13a part c, confirmed)

- The Animation window's entity preview snapshots whole components of the previewed GameObjects
  (minus the Animation and the key data, AnimationWindow.cpp:420-443) and decodes the snapshot back
  on Stop (:1552) and on every seek (:1383), so any field edited while previewing (a tween's
  duration, an Action's speed or target, a target's colour or material) is silently reverted, while
  the undo history still holds the command.
- The Inspector's action preview refreshes a hand list before restoring (InspectorWindow.cpp:
  12115-12148: the tween family, Particle System, keyframe settings) but not `ActionComponent`,
  `SpriteAnimationComponent` or anything on the target.
- An undo of an edit made during the preview restores the value the command read, which can be a
  previewed (sampled) pose. The clip preview solved both problems already (ClipRecordHooks
  before/after component writes, AnimationClipController.cpp:1430-1504, spec "animation clips" 9.R6).

### 4.7 No start offset or random start (45966b13a part a)

`ActionComponent` (ActionComponent.h:17-41) has no offset; `actionStart` (ActionSystem.cpp:32-50)
starts every run at 0 and children of an Animation get `timecount = timeDiff * speed` (:292). Copies
of a looping tween or Animation move in lockstep. Animator `cycleOffset`, VertexAnimator
`randomStart` and SplineAnimate `startOffset` exist but cover none of tweens, keyframe tracks,
sprite animations or entity Animations.

### 4.8 Escape never reaches the game (4a33929ec, settled by the skeptic)

`ImGuiConfigFlags_NavEnableKeyboard` is on (App.cpp:2340). Escape with the `GameCanvas` child focused
runs `NavUpdateCancelRequest`'s "exit child window" branch (libs/imgui/imgui.cpp:15008-15030), so
`focused` turns false (GameWindow.cpp:1036) before `PlayInput::forward` (:1208, PlayInput.cpp:263)
sees the key, and the game never gets Escape. A game's pause menu cannot be tested in the editor.

## 5. Product decisions

1. **A C++ exception is a callback failure like a crash or a Lua error, everywhere the guard runs.**
   With the editor's hook (Play): the subscriber is removed (a timer cancelled), the hook reports it,
   Play pauses and stops, the same policy `LuaFunctionBase::call` applies to a Lua error
   (LuaFunctionBase.cpp:117-121). Reason: one rule per callback kind; C++ and Lua subscribers behave
   alike.
2. **Without a hook, a C++ exception in a subscriber or C++ timer is logged and the game continues;
   the subscriber stays** (Unity: an exception in `Update` is logged and `Update` runs again next
   frame). Logged once a second per tag (the `ScriptMessages::reportError` rate). Reason: a shipped
   game should not die for one bad callback; the log names it. Zero cost on the normal path
   (table-based exception handling on every ABI we ship).
3. **The message names the type and the text**: `C++ exception <demangled type>: <what()>`
   (`C++ exception of unknown type` for a non-`std::exception`). Same text in the hook, the log and
   MSVC builds.
4. **`visible` keeps Jasper's model**: a per-object flag; an explicit change (`setVisible`,
   Inspector, `visible =`) cascades to the children as today; nothing else does. Copies, moves and
   reparenting keep every object's own value. Reason: the Doriax redesign would change how saved
   scenes render (a hidden parent with visible children, which test_trial_fixes.py:268-293 pins, has
   no faithful migration), and Active already gives Unity's inheritance.
5. **Scene view icons and camera frustums are hidden for objects inactive in hierarchy** (Unity
   draws no gizmos for inactive GameObjects), matching the icon picker.
6. **One clear list per event holder, enforced by a lint.** Each component with events gets a
   `forEachEvent(visitor)` member next to its event declarations; `Engine::clearComponentSubscriptions`
   and `Scene::removeSubscriptionsByTag` iterate one type list; PhysicsSystem gets the same visitor.
   `tools/check_event_clears.py` fails when a `FunctionSubscribe` member is missing from its holder's
   visitor or a holder from the type list. Reason: the bug class is "a hand list forgot one"; three
   hand lists had drifted.
7. **An edit-mode preview restores only what it changed (the clip preview's rule, 9.R6, applied to
   entity Animations and the Inspector's action preview)**: a command that writes a component of a
   previewed GameObject during the preview first gets the authored value back (so its undo records
   it), then its written value becomes part of the authored state the restore re-applies. Reason:
   Unity's preview mode keeps edits; the clip preview already works this way in Jasper.
8. **Start Offset is in seconds of the action's own time; Random Start picks a point of one cycle
   and overrides Start Offset.** Reason: action durations are authored in seconds; Doriax-compatible
   names; one cycle = `ActionSystem::getDuration` (ping-pong: there and back).
9. **Random Start is deterministic**: the phase is a hash of the GameObject id and the run number
   (the same scene plays the same each time; copies and restarts differ). Reason: reproducible bugs
   and tests; VertexAnimator's `randomStart` is seeded the same way (VertexAnimatorSystem.cpp:303-340).
10. **Offsets apply to a new run only** (start from Stopped, `restart()`); a resume after pause, a
    parent Animation holding or looping its child, and editor previews (Animation window, Inspector
    preview) do not apply them. Reason: the preview's playhead is clip time; the Inspector tooltip
    says "Play mode and builds".
11. **A particle-controller action ignores both fields** (Particle Systems have Start Delay and
    Prewarm); the Inspector greys the rows with that tooltip.
12. **Escape belongs to the game while the Game view is focused in Play**; leaving the view is a
    click elsewhere or Ctrl+Tab. Handing a captured cursor back is `scene-integrity`'s (section 13).

## 6. Design

### 6.1 Callback failures (engine/core/util)

`CrashHook` (CrashHook.h/.cpp) gains:

```cpp
// Runs function(context) and catches any C++ exception: false with its description in errorInfo
static bool runCatching(void (*function)(void*), void* context, std::string& errorInfo);
// "C++ exception <type>: <what>" of the exception being handled (call inside a catch)
static std::string describeCurrentException();
// A callback that threw with no hook set (call it in a catch): Log::error("<tag> failed: <the
// exception>"), at most once a second per tag, checked before the exception is described
static void logFailure(const std::string& tag);
```

- `runGuarded` = `callWithCrashGuard` around `runCatching`: the catch is inside the guard, so a
  throw never crosses it. Its result is false for a fault or an exception.
- `describeCurrentException` rethrows `std::current_exception()`: `std::exception` gives
  `typeid(e).name()` demangled (`abi::__cxa_demangle` under `__GNUG__`, the pattern of
  ComponentManager.cpp:36-44; MSVC names are readable) and `what()`; anything else "C++ exception of
  unknown type".
- `FunctionSubscribe::runSlot` (FunctionSubscribe.h:136-156), unguarded branch:
  `try { call(slot.function); } catch (...) { CrashHook::logFailure(slot.tag); return false; }`
  (the slot stays). Guarded branch unchanged (runGuarded now covers exceptions). `callRet` already
  asks the next subscriber when `runSlot` returns false.
- `ScriptScheduler` C++ timers, unguarded path (ScriptScheduler.cpp:1063): the same try/catch with
  tag `timer:C++`; the timer stays scheduled (repeating) as a Lua timer error does.
- `Scheduler` and `ScriptMessages` keep their own catches (already "log and continue").

Resulting policy (written into docs/mcp/guide/debugging.md and cpp-scripting.md):

| Where | Lua error | C++ exception | Crash (fault) |
|---|---|---|---|
| Event subscriber, timer, coroutine, editor Play | hook: removed, Play stops | hook: removed, Play stops | hook: removed, Play stops |
| Same, exported game | logged, kept | logged (1/s per tag), kept | process crash report (S28) |
| Lifecycle / physics messages | logged | logged | as above |
| Web | as above | aborts (no exception catching) | aborts |

`CrashGuard.h`, POSIX `callWithCrashGuard`:

- RAII: a `GuardFrame` struct saves the previous `g_tls.active`, `g_tls.lastSig` and `g_tls.env`
  (`std::memcpy` of the `sigjmp_buf`) and the old `sigaction`s, and restores them in its destructor
  (normal return, longjmp landing, or a throw from a direct caller's `fn`). Nested guards therefore
  hand the outer guard its jump buffer and `active == true` back.
- `sigsetjmp` stays in `callWithCrashGuard`'s own frame (it must; the frame object only holds state).
- MSVC: `winExceptionName/Desc` gain `0xE06D7363` ("CXX_EXCEPTION", "C++ exception") for direct
  callers; the engine's own paths never reach it (the catch is inside). Not buildable here: keep it
  compiling by reading (Windows is specified only by the existing SEH pattern).

### 6.2 Instantiate (engine/core/manager, binding, object)

- `ObjectManager::instantiate` (four overloads): after `initializeScripts`, `return
  scene->isEntityCreated(root) ? root : NULL_ENTITY;` (the target scene for clones). A deferred
  `GameObject.destroy(self)` in `init` still returns the root (it is alive until the end of the
  frame, Unity).
- `luaInstantiate` needs no change beyond that (it pushes nil for `NULL_ENTITY`, :465-468).
- `EntityHandle` gains `explicit operator bool() const` = `scene && entity != NULL_ENTITY &&
  scene->isEntityCreated(entity)`, so C++ writes `if (GameObject go = GameObject::instantiate(p)) {...}`.
  Header doc: "false for an empty handle or a destroyed GameObject (Unity's null)". Not bound to Lua
  (Lua gets nil).

### 6.3 Clip end (ActionSystem.cpp)

`animationApplyEnd`: `endTime = std::min(endTime, animcomp.duration)` (clip time). Comment: "the
duration is in clip time: timecount already runs at the speed". Also `animationUpdate` :295 guards
`iaction->speed > 0` before dividing (a zero or negative child speed: the frame never passes, as
today, without an infinity).

### 6.4 Visibility (engine/core/registry, editor)

- `prepare<Transform>` (EntityCloner.cpp:132-137) no longer touches `needUpdateChildVisibility`: the
  copied value stays (a cascade pending on the source applies to the copy too; otherwise none).
  `needUpdate = true` stays.
- `EntityRegistry::changeTransformChildren` (:247-251) and
  `RemoveComponentCmd::restoreTransformPlace` (:83-87) no longer set the flag (`needUpdate` stays).
- `MeshSystem.cpp:5474` (late glTF nodes take the root's flag) stays: they are new objects.
- `GameObject::setVisible` keeps its cascade; its comment and lua-api.md:1674 get the rule of
  decision 4.
- `SceneRender3D::update` loop (:1412-1470): `const bool active = !scene->hasInactiveEntities() ||
  scene->isEntityActiveInHierarchy(entity);` computed once per entity; the light, camera and audio
  icons, the camera frustum and the light shapes use `transform.visible && active` (the
  `createOrUpdate*Icon` functions and `createCameraFrustum` take a `shown` argument instead of reading
  `transform.visible`). The other overlays already take a `visible` argument; their callers pass
  `active` in too. `hasInactiveEntities()` keeps the common case free of hierarchy walks.

### 6.5 Event clearing (engine/core/component, Engine, Scene, PhysicsSystem, tools)

- Each component header with `FunctionSubscribe` members gets, right after them:

  ```cpp
  // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
  template<typename F> void forEachEvent(F&& f) { f(onStart); f(onPause); f(onStop); f(onStep); }
  ```

  Holders (today): Action, AnimationPlayer, Animator, AudioSource, Button, InputField,
  NavMeshAgent, NavMeshSurface, OnScreenStick, OnScreenButton, TouchArea, Panel, ParticleSystem,
  PoseAnimator, Scrollbar, SplineAnimate, TextEffect, UI, UIDocument, VertexAnimator, VideoPlayer.
- `engine/core/component/ComponentEvents.h` (new): `using ComponentsWithEvents = TypeList<...>`
  (the holders above) and `template<typename F> void forEachComponentEvent(Scene&, F&&)` visiting
  every component of every listed type. `Engine::clearComponentSubscriptions` becomes
  `forEachComponentEvent(*scene, [](auto& e){ e.clear(); })` then
  `scene->getSystem<PhysicsSystem>()->clearSubscriptions()`; `Scene::removeSubscriptionsByTag` the
  same with `removeByTagSubstring(tag)` (its `removeComponentSubscriptionsByTag` helper goes).
- `PhysicsSystem::forEachEvent(F&&)` lists every event inside the same `#ifdef`s as the members,
  with `onControllerColliderHit`; `clearSubscriptions` and `removeSubscriptionsByTag` use it.
- `tools/check_event_clears.py [--root DIR] [--self-test]` (style of tools/check-pollers.py): for
  each header under `engine/core/component/` and `engine/core/subsystem/PhysicsSystem.h`, every
  `FunctionSubscribe<...> name;` member of a struct/class must appear as `f(name)` in that struct's
  `forEachEvent`; every component struct with a `forEachEvent` must be in `ComponentsWithEvents`;
  every `static FunctionSubscribe` of Engine.h must appear in `Engine::clearAllSubscriptions` and
  `Engine::removeSubscriptionsByTag`. Prints `file: struct.member` per miss, exits 1. Run in the
  `unit` CI job next to `check_raw_writes.py` (docs/testing.md:809, :957) and by
  `tools/check-changed` when those files change.

### 6.6 Preview snapshots (editor/animation, windows)

New `editor/animation/ActionPreviewSnapshot.{h,cpp}`, shared by `AnimationWindow` (entity mode)
and `InspectorWindow`'s action preview; it replaces `PreviewEntityState` / `buildPreviewEntityState`
/ `restorePreviewState` (AnimationWindow.h:122-127, .cpp:420-459) and `ActionPreviewState` plus the
refresh list (InspectorWindow.h:320-325, .cpp:12115-12160).

```cpp
class ActionPreviewSnapshot {
public:
    // Encodes the components of `entities` (Stream::encodeComponents) minus `excluded` types;
    // starts listening to scene writes of sceneId (ClipRecordHooks component write listeners)
    void capture(uint32_t sceneId, Scene* scene, const std::vector<Entity>& entities,
                 std::initializer_list<ComponentType> excluded);
    // Puts the authored state back: decodes the snapshot, then re-applies every value a command
    // wrote during the preview (last write wins); clears preview particles
    void restore(Scene* scene) const;
    void release();                       // stops listening, forgets everything
    bool contains(Entity entity) const;
    bool changedSinceApply() const;       // a write arrived: the owner re-applies its preview
private:
    struct Written { Entity entity; ComponentType type; std::string path; /* "" = whole */ 
                     std::shared_ptr<void> value; const reflect::TypeOps* ops; YAML::Node whole; };
    ...
};
```

- Listening: `ClipRecordHooks` gains `addBeforeComponentWriteListener` / `addAfterComponentWriteListener`
  with `(sceneId, entity, ComponentType, property)`, called at the top of
  `beforeComponentWrite` / `afterComponentWrite` for every write (today those only fan out to
  channel and property listeners, so non-animatable properties notify nobody). Call sites are
  already in place (PropertyCmd, ObjectTransformCmd, ComponentValuesCmd, LatticePointsCmd,
  MorphWeightsCmd, for execute, undo and redo). The implementer audits the commands MCP
  `set_properties` and `edit_animation` (entity mode) use and adds the two calls where a component
  write lacks them (ClipRecordHooks.h contract: "a new write site calls before/afterComponentWrite").
- Before a write to a snapshotted entity's component: the authored value of that path (from the
  last written value, else the snapshot) is written into the live component, so the command reads
  and records it (PropertyCmd reads after the hook, PropertyCmd.h:217-220).
- After the write: the new value is captured. A path resolves with `reflect::resolve(desc, comp,
  property)` and is stored with its `TypeOps` (`construct` + `copy` into an owned buffer); `""`
  stores `reflect::encode` of the whole component; a property that does not resolve (a Catalog-only
  name) stores its first segment's whole field. Then `changedSinceApply` is set.
- `restore`: `Stream::decodeComponents` of the snapshot (as today), then each written value in
  order: `reflect::resolve` on the restored component and `ops->copy`, or `reflect::decode` of the
  whole node; `ProjectUtils::clearPreviewParticles`; relative keyframe tracks' `hasBase = false`
  (today's rule, AnimationWindow.cpp:454-457).
- AnimationWindow: seek and Stop call `restore`; when `changedSinceApply()` while paused, the window
  re-seeks to the playhead on its next update (as the clip controller's `needsApply`). The excluded
  types stay those of :427-438 (Animation and the key data). Inspector preview: `capture` with no
  exclusions over the entities it collects (InspectorWindow.cpp:12041-12060).
- Undo after the preview ended: the command's old value is the authored value (the before-write
  rule), so undo never brings a sampled pose back.

### 6.7 Start Offset and Random Start

Engine (`engine/core/component/ActionComponent.h`):

```cpp
// seconds into the action a new run starts at (Play mode and builds; previews start at 0)
float startOffset = 0;
// a new run starts at a random point of one cycle instead (deterministic per GameObject and run)
bool randomStart = false;
// runtime: where the current run started, and how many runs there were (Random Start's seed)
float phase = 0;
uint32_t runs = 0;
```

`ActionSystem` (ActionSystem.h/.cpp):

- `bool previewing = false;` set by an RAII scope inside `updateActionPreview` and
  `updateAnimationPreview` (and the Animation window's seek simulation, which calls them).
- `actionStart(entity, newRun)`: in the `Stopped && newRun` branch, before `actionComponentStart`:
  `action.phase = startPhase(entity, action)` and `action.timecount = action.phase`, where
  `startPhase` returns 0 when `previewing` or for a particle controller (`isParticleController`),
  else `randomStart ? hashUnit(entity, ++runs) * getDuration(entity) : max(0, startOffset)`.
  `hashUnit` = `vat::vatRandomUnit` moved to a shared `engine/core/math/HashRandom.h` as
  `double hashUnit(uint64_t seed, int64_t index)` (VertexAnimatorRuntime keeps calling it).
  A duration of 0 (or non-finite) gives phase 0.
- Children of an Animation: `iaction->timecount = iaction->phase + timeDiff * iaction->speed` (:292)
  and `animationApplyEnd` likewise (:355); a child's `phase` is set by its own new-run start, so it
  stays across the parent's loops.
- Sprite animations: after `spriteActionStart` in a new run with `phase > 0`, a new
  `spriteSeek(mesh, sprite, spriteanim, phaseMs)` walks the frame times like `spriteActionUpdate`
  (loop wraps; a non-looping one holds its last frame and stops on its first update; zero and
  negative frame times as there) and sets the rect once (`setSpriteTextureRect`).
- Tweens and keyframe tracks need nothing else: their updates read `timecount` and wrap or end
  (`tweenUpdate` fmods a looping period; a non-looping one past its end finishes on its first
  update holding the end value; keyframe tracks the same through `locateKey` and their `loop`).
- `Action` (engine/core/action/Action.h/.cpp): `setStartOffset(float)` (clamps to >= 0, warns once
  on a negative or non-finite value), `getStartOffset()`, `setRandomStart(bool)`, `isRandomStart()`.

Editor:

- Reflection (AnimationReflect.cpp:49-54): `JE_FIELD(ActionComponent, startOffset, .unit = Unit::Seconds,
  .min = 0.0f, .flags = OmitIfDefault | CodegenIfChanged, .tooltip = ...)` and
  `JE_FIELD(ActionComponent, randomStart, .flags = OmitIfDefault | CodegenIfChanged, .tooltip = ...)`;
  `actionPostDecode` resets `phase` and `runs`. This gives YAML, exported code, prefab overrides,
  `set_properties` / `get_entity` and the Catalog property rows.
- Inspector (InspectorAnimation.cpp:189-193): rows "Start Offset" (Float, seconds) and "Random Start"
  (Bool) under Speed, with undo through `propertyRow`; Start Offset disabled while Random Start is
  on; both disabled for a particle-controller action. Tooltips: "Seconds into the action each new
  run starts at, in Play mode and builds (previews start at 0)" / "Each new run starts at a random
  point of one cycle (different per GameObject, the same every playthrough); overrides Start Offset".
- MCP: `edit_animation` (entity mode) gains `start_offset` (number, seconds) and `random_start`
  (bool) for the Animation's Action, next to `speed` (WorldTools.cpp:5739-5741); `get_animation`
  reports both for an Animation and for each nested action. Tool description +~30 characters;
  `tools/mcp/tool_budget.py --check tools/mcp/budget.json`, `gen_tool_docs.py`.
- Lua (ActionClassesLua.cpp:97): `Action` rw `startOffset`, `randomStart`; ECSClassesLua.cpp:649-652
  `ActionComponent` `startOffset`, `randomStart` (tracked setters, as `speed`). Stubs and lua-api
  data regenerate from the bindings (docs/scripting.md:238).

### 6.8 Escape in the Game view (editor/window/GameWindow.cpp)

Inside the `GameCanvas` child, while Play runs and the canvas is focused, each frame:
`ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##GameCanvasKeys"))` (imgui_internal.h). The
ownership takes effect in the next `NewFrame`, where `NavUpdateCancelRequest` tests Escape with
`ImGuiKeyOwner_NoOwner` and finds it owned, so the child keeps the focus; `PlayInput::forward` reads
the key with the default owner test (`ImGuiKeyOwner_Any`) and forwards it (PlayInput.cpp:263). When
the canvas loses focus or Play stops, the claim is not renewed and Escape returns to ImGui. The MCP
test (10) also presses Tab, the arrows, Space and Enter in the focused Game view; any of them that
moves ImGui's focus is claimed the same way (with the test as the proof).

### 6.9 Export and platforms

- All engine changes are platform-neutral C++ (no new GPU work). The crash guard change is POSIX
  only (Linux, macOS, Android, iOS); Windows (SEH) gains the C++ exception name only; Web has no
  guard and no exception catching. Windows, macOS and iOS cannot be built here: the implementer keeps
  them compiling by reading (`#if` blocks of CrashGuard.h, no new platform APIs).
- The new fields reach exported games through reflection codegen (`CodegenIfChanged`); a project
  that never sets them generates the same code as today.

## 7. Behaviour rules and edge cases

- A subscriber that throws while another subscriber of the same event is running (nested calls):
  each `runSlot` catches its own; the outer continues.
- A subscriber that throws inside `callRet`: the next subscriber answers (guarded: the thrower is
  removed; unguarded: kept, asked again next time).
- `CrashHook::logFailure` runs on the main thread; a subscriber called from another thread
  (`callDirect`) is not covered (non-goal).
- Instantiate where `init` destroys only a child: the root is returned. Where it destroys the
  parent passed in (with its new child): `nil`. Inside a `StructuralDeferScope` (an action callback):
  the destroy is deferred, the root is returned and is gone at the end of the scope.
- Visibility: a copy of a GameObject made the same frame as its `setVisible(false)` (the cascade
  still pending) carries the pending flag and hides its children like the source does.
- A copy keeps `visible` exactly; Active is not involved.
- Start Offset >= duration: a looping action wraps into its first cycle on the first update; a
  non-looping one ends on its first update holding its end value (and sends `onStop`).
- Random Start with a duration of 0 (an Animation of particle controllers only, an empty track):
  phase 0.
- `restart()` is a new run: a new random phase. `pause()` then `start()`: no new phase.
- A child action reused by two Animations: its phase is set by whichever starts it as a new run.
- Speed and offsets: the offset is in the action's own time, so `speed` 2 with offset 0.5 starts
  half a second into the clip and plays it twice as fast from there.
- Preview: a write to a GameObject outside the snapshot is not tracked (the restore does not touch
  it). A structural command (add/remove component, delete, reparent) already stops previews first
  (PreviewOwner::stopFor sites); undo of a write made during a now-stopped preview behaves as any
  undo.
- Preview: two writes to the same path keep the last; a whole-component write after a path write
  replaces it; a path write after a whole-component write is applied after it.
- Escape while a text field of the game has focus: forwarded as today (the game sees it).

## 8. Performance and cost

- Exceptions: no cost on the normal path (try blocks are table-based on x86-64, ARM64, ARMv7 EHABI
  and Windows x64; Emscripten compiles them out). `logFailure`'s map only grows with failing tags.
- Crash guard: one `memcpy` of a `sigjmp_buf` (~200 bytes) and five `sigaction` restores per guarded
  call, as today (the restores already happen). Only while the editor's hook is set.
- Instantiate: one `isEntityCreated` lookup per call.
- Visibility: removes work (fewer forced cascades). Icons: one `isEntityActiveInHierarchy` per
  annotated GameObject per Scene view update, and only when the scene has inactive entities.
- Event clearing: the same component-array walks as today, at Stop and hot reload only.
- Start offset: two floats, a bool and a counter per ActionComponent (12 bytes); one branch per new
  run. Nothing per frame. Unused: zero.
- Preview snapshot: listeners run only while a preview is active; per write one `resolve` and one
  value copy.
- Escape: one `SetKeyOwner` call per frame while the Game view is focused in Play.

## 9. Compatibility

- No file format change. `startOffset` / `randomStart` are omitted at their defaults, so old scenes
  and prefabs load unchanged and resave byte-identical; no version bump, no migration.
- Behaviour changes, intended: copies keep their children's own `visible` (games that relied on a
  copy re-showing hidden children were relying on the bug; documented in lua-api.md); a C++
  exception in a subscriber of an exported game is logged instead of terminating it; Escape in the
  focused Game view goes to the game.
- Lua: `GameObject.instantiate` already documented "GameObject or nil".

## 10. Tests

Red controls: each test marked (red) must fail on `main` and pass with the change; the
implementer runs it once on a tree without the fix and records the run in the report.

Unit, `jasperengine-core-tests` (engine/tests):

1. `FunctionSubscribeCompatTest.ThrowingSubscriberIsRemovedAndReported` (red; hook set): a subscriber
   throwing `std::runtime_error("boom")` between two counters; both counters run twice over two calls,
   one report whose text starts `thrower: C++ exception std::runtime_error: boom`, size 2 after.
2. `FunctionSubscribeCompatTest.ThrowingSubscriberIsLoggedWithoutAHook` (red): no hook; later
   subscribers still run, the thrower stays (size 3), one log line within a second for two calls.
3. `FunctionSubscribeCompatTest.CallRetAsksTheNextAfterAThrow` (red): `callRet` returns the next
   subscriber's value, with and without a hook.
4. `CrashGuardTest.ThrowThroughTheGuardRestoresItsState` (red, POSIX): a direct `callWithCrashGuard`
   whose `fn` throws, caught by the test; afterwards `detail::g_tls.active` is false and the SIGSEGV
   action equals the one before.
5. `CrashGuardTest.NestedGuardsKeepTheOuterOne` (red, POSIX): outer guard runs an inner guard that
   returns true, then faults (`crashHere`); the outer returns false with SIGSEGV. Red today: the
   process dies, so the test is also added as mode `guard_nested` to engine/tests/crash/CrashTestMain.cpp
   with a ctest expecting exit 0 (the gtest is then the green check).
6. `ComponentEventsTest.ClearReachesEveryEvent` (red): a scene with one component of each
   `ComponentsWithEvents` type; through `forEachEvent` one owner-less subscriber per event; after
   `Engine::clearComponentSubscriptions` every event has size 0. Red without the fix for NavMeshAgent,
   ParticleSystem, TextEffect, VideoPlayer, NavMeshSurface, SplineAnimate (the old function is kept
   for the red run by listing those events in the test explicitly, not through `forEachEvent`).
7. `ComponentEventsTest.RemoveByTagReachesEveryEvent` (red): the same with `Scene::removeSubscriptionsByTag`
   and a tagged subscriber per event (UIDocument among the red ones).
8. `PhysicsEventsTest.ClearSubscriptionsIncludesControllerColliderHit` (red; 3D physics builds) and
   `...RemoveByTagIncludesControllerColliderHit` (red).
9. `HiddenChildVisibilityTest` (engine/tests/core/registry): `CloneKeepsAChildHiddenOnItsOwn` (red),
   `CloneOfAHiddenParentKeepsItsVisibleChild` (red), `MoveToSceneKeepsChildFlags` (red),
   `RemovingAMiddleGameObjectKeepsGrandchildFlags` (red, `changeTransformChildren`),
   `SetVisibleStillCascades` (pins decision 4: hide then show the parent reaches every child). Each runs
   `propagateTransforms(registry, noop)` and reads `Transform::visible`.

Unit, `jasperengine-editor-tests` (editor/tests):

10. `ScriptLifecycleTest.InstantiateReturnsNilWhenInitDestroysTheCopy` (red): a Lua script whose
    `init` calls `GameObject.destroyImmediate(self.gameObject)`; `GameObject.instantiate(prefabOrObject)`
    from another script returns `nil` (assert in Lua, logged); `InstantiateKeepsARootDestroyedLater`
    (deferred `GameObject.destroy` in `init`: a live wrapper); `CppInstantiateOfADestroyedCopyIsFalse`
    (red: `static_cast<bool>(go)` false, `getEntity() == NULL_ENTITY`).
11. `ScriptSchedulerTest.CppTimerExceptionIsLoggedAndTheGameGoesOn` (red): unguarded `Timer::after`
    callback throws; the frame returns, one log line, a repeating timer still fires next time.
12. `ActionSpeed.NonLoopingAnimationEndsOnItsLastKeyAtSpeedTwo` (red; editor/tests/engine/action/
    ActionTimingTest.cpp): Animation duration 1, speed 2, a translate track 0 -> 10 over [0, 1];
    two steps of 0.3 s; the target ends at x = 10 (today 5).
13. `ActionStartOffset.*` (ActionTimingTest.cpp): `TweenStartsAtItsOffset` (0 -> 10 over 1 s, offset
    0.25: after the first 0 s update x = 2.5); `LoopingTweenWrapsAnOffsetPastItsPeriod`;
    `NonLoopingOffsetPastTheEndHoldsTheEnd`; `ResumeAfterPauseKeepsTheTime`; `RestartAppliesItAgain`;
    `RandomStartIsDeterministicAndDiffersBetweenCopies` (two entities, phases differ, a second scene
    with the same ids gives the same phases; a restart gives a new phase); `AnimationChildOffsetShiftsItsTrack`;
    `SpriteAnimationStartsOnTheOffsetFrame` (frames 100 ms each, offset 0.25 s: frame 2);
    `PreviewIgnoresOffsets` (`updateActionPreview` starts at 0); `ParticleControllerIgnoresOffsets`;
    `SpeedDoesNotScaleTheOffset`.
14. `StreamComponentTest.ActionStartOffsetRoundTripsAndOmitsDefaults` (editor/tests/stream): defaults
    write no key; set values round-trip; an old YAML without the keys decodes to defaults; the
    codegen of a changed value emits both statements.
15. `ActionPreviewSnapshotTest` (editor/tests/animation, new): `RestoreKeepsAWrittenProperty`,
    `BeforeWritePutsTheAuthoredValueBack` (a PropertyCmd made while the live value differs records the
    authored value as its undo), `WholeComponentWriteIsKept`, `LastWriteWins`,
    `UnresolvedPathKeepsItsField`, `WritesOutsideTheSnapshotAreIgnored`, `ReleaseStopsListening`.
16. `RemoveComponentCmdTest.UndoOfATransformRemovalKeepsGrandchildFlags` (red, editor/tests/command).

MCP end-to-end (`tools/mcp/tests/`):

17. `test_animation_window.py::test_edits_during_the_entity_preview_are_kept` (red): RobotTweens,
    `preview_animation play` 300 ms, `pause`, `set_properties` the tween's `duration` and a property of
    the target no track drives, `seek` 0.2, `stop`; `get_entity` shows both edits; `undo` twice gives
    back the pre-edit values (not a sampled pose).
18. `test_animation_window.py::test_start_offset_through_edit_animation`: `edit_animation`
    `start_offset` 0.5 / `random_start` true on an entity Animation; `get_animation` reports them;
    undo restores; `set_properties {action: {startOffset}}` on a standalone tween works too.
19. `test_input.py::test_escape_in_the_focused_game_view_reaches_the_game` (red): Play a scene whose
    Lua script logs on `Input.getKeyDown(KeyCode.ESCAPE)`; `focus_window game`; a real Escape (the
    xdotool `key` helper of test_animation_window.py:69-81); `read_console` has the line and the Game
    view is still focused; Tab, arrows, Space and Enter likewise reach the game.
20. `test_scene_view_icons_follow_active` (new file `test_scene_view_icons.py`, red): a point light in
    front of the Scene camera on a black sky; the icon's projected region has icon pixels; with its
    parent inactive, none; active again, back.

Lint and docs:

21. `python3 tools/check_event_clears.py --self-test` (planted misses are found) and the tree pass;
    red control: the tree pass on `main` lists the six components and `PhysicsSystem.onControllerColliderHit`.
22. `tools/lua_api` doc test over the new lua-api.md Lua blocks; `test_stubs.py` sees the new Action
    properties.

## 11. Acceptance criteria

1. A C++ exception in a guarded subscriber, coroutine resume, C++ timer or scheduler task is reported
   through the hook with `C++ exception <type>: <what>` and the subscriber removed (tests 1, 3).
2. Without a hook the exception is logged at most once a second per tag and the game continues
   (tests 2, 11).
3. A throw through `callWithCrashGuard` and nested guards leave the guard state correct (tests 4, 5).
4. `GameObject.instantiate` returns `nil` (C++: a false handle) when the copy's `init` destroyed it
   immediately (test 10).
5. A non-looping Animation with a duration ends on its last key at any positive speed (test 12).
6. Clones, scene moves and Transform removals (and their undo) keep every child's own `visible`;
   `setVisible` still cascades (tests 9, 16).
7. Scene view icons and frustums of inactive objects are hidden (test 20).
8. Every component event and every physics event is cleared at Stop and on tag removal (tests 6-8),
   and `check_event_clears.py` passes on the tree and runs in CI (test 21).
9. Values written during an Animation window or Inspector action preview survive seek and Stop, and
   their undo restores the authored value (tests 15, 17).
10. Start Offset and Random Start work for tweens, keyframe tracks, sprite animations and entity
    Animations, with the rules of section 7 (test 13); they are in the Inspector with undo, Lua, MCP
    (`edit_animation`, `get_animation`, `set_properties`) and saved only when set (tests 14, 18, 22).
11. Escape in the focused Game view reaches the game and the view keeps the focus (test 19).
12. Docs: lua-api.md (instantiate nil rule, `visible` rule, Action `startOffset` / `randomStart`),
    cpp-scripting.md (`if (go)`, the failure table), debugging.md (the failure table and the Console
    line), animation.md (Scene mode `start_offset`, `random_start`; preview keeps edits),
    docs/testing.md (the lint in the unit job); `docs/mcp/TOOLS.md` regenerated; `tool_budget.py --check`
    passes; `check_agent_docs.py` if the agent guide changed.
13. `tools/check-changed` passes; every red control was run red and recorded.

## 12. Files likely touched

- engine/core/util/CrashHook.{h,cpp}, CrashGuard.h, FunctionSubscribe.h
- engine/core/script/ScriptScheduler.cpp
- engine/core/manager/ObjectManager.cpp; engine/core/object/EntityHandle.{h,cpp}
- engine/core/subsystem/ActionSystem.{h,cpp}; engine/core/component/ActionComponent.h;
  engine/core/action/Action.{h,cpp}; engine/core/math/HashRandom.h (new);
  engine/core/animation/VertexAnimatorRuntime.{h,cpp} (use `hashUnit`)
- engine/core/registry/EntityCloner.cpp, EntityRegistry.cpp; engine/core/object/GameObject.cpp (comment)
- engine/core/component/*.h with events (21 components in 19 headers: `forEachEvent`), ComponentEvents.h (new);
  engine/core/Engine.cpp (`clearComponentSubscriptions`), engine/core/Scene.{h,cpp};
  engine/core/subsystem/PhysicsSystem.{h,cpp}
- engine/core/script/binding/ActionClassesLua.cpp, ECSClassesLua.cpp
- editor/reflect/components/AnimationReflect.cpp; editor/window/InspectorAnimation.cpp,
  InspectorWindow.{h,cpp}, AnimationWindow.{h,cpp}, GameWindow.cpp;
  editor/animation/ActionPreviewSnapshot.{h,cpp} (new), ClipRecordHooks.{h,cpp};
  editor/command/type/RemoveComponentCmd.cpp (and any command lacking the write hooks);
  editor/render/SceneRender3D.{h,cpp}; editor/mcp/tools/WorldTools.cpp
- tools/check_event_clears.py (new), tools/check_changed.py, CI unit job script, tools/mcp/budget.json
- tests: engine/tests/core/util/FunctionSubscribeCompatTest.cpp, CrashGuardTest.cpp (new),
  engine/tests/crash/CrashTestMain.cpp + its ctest, engine/tests/core/registry/HiddenChildVisibilityTest.cpp
  (new), engine/tests/core/ComponentEventsTest.cpp (new), editor/tests/ScriptLifecycleTest.cpp,
  ScriptSchedulerTest.cpp, editor/tests/engine/action/ActionTimingTest.cpp, editor/tests/stream/
  StreamComponentTest.cpp, editor/tests/animation/ActionPreviewSnapshotTest.cpp (new),
  editor/tests/command/RemoveComponentCmdTest.cpp, tools/mcp/tests/test_animation_window.py,
  test_input.py, test_scene_view_icons.py (new)
- docs: docs/mcp/guide/lua-api.md, cpp-scripting.md, debugging.md, animation.md, docs/testing.md,
  docs/mcp/TOOLS.md (generated)

Hot files shared with other branches: ActionSystem.cpp, EntityCloner.cpp, Engine.cpp, Scene.cpp,
SceneRender3D.cpp, GameWindow.cpp, WorldTools.cpp, ECSClassesLua.cpp / GameObjectClassesLua.cpp,
InspectorWindow.cpp, budget.json, lua-api.md.

## 13. Cross-feature dependencies

- `scene-integrity`: hands a game-captured cursor back to the editor and works on Play/Scene view
  input; both touch GameWindow.cpp / PlayInput.cpp. Shared rule: Escape in the focused Game view is
  claimed by the game (this spec); if their hand-back uses Escape (Unity's key), it builds on this
  claim and releases the cursor without unfocusing the view. Whoever merges second keeps one claim.
- `ui-components`: owns Doriax's button scale transition (rejected here); Engine.cpp input paths
  near `clearComponentSubscriptions` and ButtonComponent.h (its new events, if any, must join
  `forEachEvent`; the lint enforces it).
- `scripting`: edits Lua bindings (ECSClassesLua.cpp, GameObjectClassesLua.cpp) and regenerates
  stubs; merge conflicts only.
- `render-correctness`: changes the camera gizmo in SceneRender3D.cpp (`createCameraFrustum`); this
  spec changes its visibility argument. Also receives the unsettled texture-swap item (section 2).
- `mesh-memory`: edits `prepare<MeshComponent>` and neighbours in EntityCloner.cpp.
- `draw-perf`: may change the transform pass around `propagateTransforms`; the visibility rule there
  (cascade only below a flagged parent) must stay.
- `sprite-batching`: settled-sprite reuse must see the frame rect `spriteSeek` sets (it goes through
  `setSpriteTextureRect`, the setter every frame change uses).
- `agent-authoring`: the agent guide's byte budget and `tools/mcp/budget.json` (edit_animation grows
  by two parameters); "native animation" authoring rules may mention Random Start for copies.
- Any feature adding a `FunctionSubscribe` member to a component or PhysicsSystem must add it to
  `forEachEvent` (CI fails otherwise).

## Deviations

Recorded during implementation (branch `dc/runtime-core`).

1. **Escape (lead decision, LEDGER "Lead decisions across specs").** A focused Game view delivers Escape to
   the game and, when the game captured the cursor, the same press releases it. This branch does the
   delivery (6.8); `scene-integrity` does the release. The claim (`SetKeyOwner`) takes no lock, so the
   release, reading the key with the default owner test, still sees the press. Docs name the key
   `Input.KEY_ESCAPE` (the Lua binding's name), not `KeyCode.ESCAPE`.
2. **Zero-speed guard (6.3).** `animationUpdate` divides by a child's speed only when it is not 0
   (`!= 0`, not `> 0`): a negative speed keeps today's behaviour; only the division by zero goes.
3. **Scene view icons (6.4).** Instead of threading `active` into each `createOrUpdate*` call,
   `SceneRender2D/3D::hideEntityGizmos(entity)` hides every per-entity icon and line of a GameObject
   inactive in hierarchy after its update (2D views too: camera, sound and 2D light icons, layout,
   body, joint and line overlays). Nothing is destroyed, so toggling Active creates nothing.
4. **ActionPreviewSnapshot API (6.6).** `add(sceneId, scene, entity, excluded = readOnlyTypes())`
   instead of one `capture(entities, excluded)`: the Animation window adds entities while a preview
   runs (new track entities, nested Animations). Both previews leave out the same read-only types
   (the Animation and the key data), so keys authored during a preview stay; the one part of the
   key data a preview writes, a keyframe track's playhead (`index`, `interpolation`, saved fields),
   is taken and put back on its own (the Inspector's old refresh restored those two keys too). A
   path write keeps the value through its `TypeOps`
   (copy); a whole-component write, or a path the reflection cannot copy, keeps the component's
   authored YAML (encoded after the component was put back to its authored state, so it holds no
   previewed value), instead of the spec's "first segment" fallback. Before a path write only that
   value goes back (an Action's preview state and time stay); before a whole write the component does.
5. **The lint's red control (test 21).** On `main` no holder has `forEachEvent`, so the tree pass
   lists every event, not the six components. The red controls are the self-test's planted misses
   and removing `f(onControllerColliderHit)` from PhysicsSystem's visitor (the lint then fails);
   the behaviour's red controls are the gtests 6-8.
6. **Nested guards (test 5).** Kept as the gtest only (no `guard_nested` mode in the crash test
   harness): its red run is the core test binary dying on the fault, run alone.
7. **Names.** Test 10's C++ case is `CppInstantiateOfADestroyedCopyIsEmpty`; `Action::setStartOffset`
   warns on every bad value (not once).
8. **Lean MCP rules (fe3e90b97).** No agent-guide text was added (lead decision: guidance in
   `docs/mcp/guide/<area>.md`). The edit_animation parameters got the eval case
   `guard_random_start_for_copies` (`tools/mcp/eval/cases.py`); the live eval needs Orquester-managed
   accounts and was not run here (`test_grade.py` passes).
9. **Command audit (6.6).** Besides PropertyCmd, ObjectTransformCmd, ComponentValuesCmd,
   LatticePointsCmd and MorphWeightsCmd, `ReflectedValueCmd` (the Inspector's array rows: insert,
   remove, move; Revert to prefab of a field) calls `ClipRecordHooks::before/afterComponentWrite` on
   execute and undo.
10. **Lint wiring (6.5).** There is no CI any more: `tools/check_event_clears.py` runs in
    `tools/build.sh linux` and through `tools/check-changed` when a component header,
    `PhysicsSystem.h` or `Engine.h`/`.cpp` changes (docs/testing.md, check-changed; AGENTS.md's lint
    list). It reads several names and initialisers in one declaration; events outside components,
    PhysicsSystem and Engine (`Log::onMessageReceived`) are outside its scope, as its docstring says.
11. **`CrashHook::logFailure(tag)` (6.1)** describes the exception being handled itself, after its
    once-a-second check (a callback throwing every frame is not rethrown and demangled each time).
12. **edit_animation keeps only `random_start` (6.7).** The default (core) listing is pinned at 99,000
    characters (`test_tool_budget.py`), and after main's merges it measured 98,995: both parameters
    put it at 99,028. A fixed Start Offset goes through `set_properties {action: {startOffset}}` on the
    Animation or a nested action; `get_animation` still reports `start_offset` and `random_start`.
    `edit_animation` stays within its old 3,606 limit.

## Follow-ups (2026-11-07)

1. **The event-clear lint covers every engine event** (`tools/check_event_clears.py`). An event is a
   member of type `FunctionSubscribe<...>`, of a class derived from it (`Ease`) or of an alias of one,
   alone or in a container, anywhere in the engine's sources (not `engine/libs`, `engine/tests`,
   `engine/workspaces`). Declarations are read as statements (type arguments across lines,
   `jasperengine::` qualification, `JASPERENGINE_API static`, `alignas` and attributes); locals,
   parameters, references, pointers, functions and static definitions are not events. A holder's events
   are in its `forEachEvent` (a container by name); a component holder is in `ComponentsWithEvents`;
   any other holder is in the lint's `WALKS` table, with the clears that call its visitor checked
   (PhysicsSystem's two, InputAction's `InputActions::unload` and `resetSession`), or in `TRANSIENT`
   (`LegacyParticleSystem.h`, a view sampled into curves within one call); a static event is in
   `Engine::clearAllSubscriptions` and `Engine::removeSubscriptionsByTag` (Engine's own unqualified,
   not satisfied by another class's same-named event; others as `Class::member`). An event at
   namespace scope fails, and so does an event type the lint cannot read where a declaration could
   be (a macro, an unparsable statement), so nothing is skipped silently.
2. **Real hits fixed.**
   - `Audio::onMusicBeat` and `Audio::onMusicSection` were cleared only in `MusicPlayer::reset`,
     which the editor's Stop runs in `finalizeStop`, after `conector.disconnect()`: a C++ script's
     callback there was destroyed after its library was unloaded, and a tag removal never reached
     them. Both are now in Engine's two clears (test `EngineEventsTest.ClearsReachOtherClassesStaticEvents`,
     red before the fix).
   - `TweenComponent::function`, an `Ease` a C++ script can set to its own lambda, was destroyed with
     the runtime scene after the unload. TweenComponent is now in `ComponentsWithEvents` with a
     `forEachEvent`, and `Ease::clear` keeps only the built-in curve of its type (a custom function
     goes, the ease is linear again), so `clearComponentSubscriptions` destroys the script's function
     while its code is loaded (test `ComponentEventsTest.ClearDropsAScriptsTweenEase`, red before).
   - `InputAction`'s three events were cleared by two hand lists (complete): they now go through
     `InputAction::forEachEvent` (test `InputActionsTest.UnloadAndANewSessionClearEveryActionEvent`
     pins it; no red run, nothing was missed).
3. **check-changed** runs the lint for any changed engine source outside the skipped directories (it
   used to take component headers, PhysicsSystem.h and Engine.h/.cpp only).
