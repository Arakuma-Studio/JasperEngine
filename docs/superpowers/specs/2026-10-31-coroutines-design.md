# Coroutines, timers and time values for scripts (design)

Feature `coroutines` (ledger #16, branch `gaps/coroutines`). Binding conventions:
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) "Shared conventions" (async on the main thread, scaled vs
unscaled explicit, owner-bound lifetime). Lifecycle and the frame order: `script-lifecycle` spec (section 9.1).

## 1. Goals

1. Engine-driven Lua coroutines owned by a script: `self:startCoroutine(fn, ...)`, `self:stopCoroutine(co)`,
   `self:stopAllCoroutines()`, with Unity's wait kinds: scaled seconds, realtime seconds, next frame / N
   frames, next fixed update, end of frame, until/while a predicate, and waiting for another coroutine, a
   timer, a Tween/Action, an AudioSource, an AnimationPlayer, an anime animation, or any object with
   `isDone()` (the HTTP request handle of feature `http`).
2. Unity's ownership rules: stopped when the object is deactivated or destroyed, not when the script is
   disabled; errors logged with the coroutine's own traceback and where it was started.
3. Invoke-style timers: `self:invoke`, `self:invokeRepeating`, `self:cancelInvoke`, `self:isInvoking`
   (scaled by default, `unscaled` flag), plus a `Timer` module (`Timer.after`, `Timer.every`) for code that is
   not a script, clearly separated from `anime.createTimer` (HTML UI clock).
4. A Unity-familiar `Time` class (`Time.deltaTime`, `Time.unscaledDeltaTime`, `Time.fixedDeltaTime`,
   `Time.time`, `Time.unscaledTime`, `Time.realtimeSinceStartup`, `Time.timeSinceLevelLoad`, ...) that
   removes the `Engine.updateTime` / `Engine.fixedDeltatime` naming trap.
5. Fix: every Lua callback stored by the engine keeps the **main** Lua thread, never the coroutine it was
   registered from (today a callback registered inside a coroutine dangles or silently never fires).
6. C++ scripts: a timer API (`invoke`, `invokeRepeating`, `Timer::after`) with the same clocks and ownership.

## 2. Non-goals

- C++20 coroutines (`co_await`) for C++ scripts: projects compile with a configurable `cxxStandard`
  (`Generator.cpp` `cxxStandard`, C++17 allowed) on six toolchains; a timer API covers the needs (the game's
  C# coroutines port to Lua). Possible later on top of `ScriptScheduler`.
- Yielding from event handlers (`onUpdate`, input events, `init`): they are called from C with `lua_pcall`;
  the wait helpers raise a clear error there (start a coroutine instead). `start` (lifecycle) does run as a
  coroutine.
- Waiting on scene loads (`SceneManager.loadScene` returns no handle; use `Engine.onSceneLoaded` or
  `Coroutine.waitUntil(function() return not SceneManager.loading end)`).
- Changing `anime.createTimer` (HTML UI animation API; documented as the UI clock).

## 3. Users and stories

- Human from Unity: `self:startCoroutine(function() Coroutine.wait(2); self:spawn() end)`;
  `Coroutine.waitRealtime(1)` keeps counting in the pause menu at `Time.timeScale = 0`.
- Human: `local t = self:invokeRepeating("spawnEnemy", 1, 0.5)` ... `self:cancelInvoke("spawnEnemy")`.
- Agent porting `IEnumerator Start(){ yield return null; Load(); while(true){ yield return new
  WaitForSeconds(5); Save(); } }` writes `function M:start() Coroutine.nextFrame(); self:load(); while true do
  Coroutine.wait(5); self:save() end end`.
- Agent porting `yield return StartCoroutine(Request())` writes `Coroutine.waitFor(self:startCoroutine(self.request, self))`
  or simply calls `self:request()` inside the coroutine (Lua yields through plain calls).
- Agent debugging: a coroutine error in the Console reads
  `Lua coroutine error in Spawner (GameObject 'Spawner', scene 'Level1'), started at Spawner.lua:12: ...
  stack traceback: ...`; `get_runtime_entity` lists the object's coroutines with what each waits for.

## 4. Research summary

- Unity coroutines (https://docs.unity.com/en-us/engine/current/manual/scripting/coroutines-section/coroutines,
  https://docs.unity3d.com/2022.3/Documentation/ScriptReference/MonoBehaviour.StartCoroutine.html): the code up
  to the first yield runs inside `StartCoroutine`; any yield resumes on a later frame; stopped by
  `SetActive(false)` and Destroy, not by `enabled = false`; `StopAllCoroutines` only for that script;
  `StartCoroutine` on an inactive object is an error.
- Resumption points (https://docs.unity3d.com/2022.3/Documentation/Manual/ExecutionOrder.html): `yield null`,
  `WaitForSeconds`, `WaitUntil/WaitWhile` after all Updates, before LateUpdate; `WaitForFixedUpdate` after the
  physics step; `WaitForEndOfFrame` after rendering. `WaitForSeconds` is scaled and never completes at
  timeScale 0; `WaitForSecondsRealtime` is unscaled.
- Invoke (…/MonoBehaviour.Invoke.html, InvokeRepeating.html): scaled; repeat does not fire at timeScale 0;
  community reports it keeps firing on disabled scripts and inactive objects.
- Time (…/Time.html): `deltaTime` returns `fixedDeltaTime` inside FixedUpdate; `unscaledDeltaTime`,
  `fixedUnscaledDeltaTime`, `time` (fixedTime inside FixedUpdate), `unscaledTime`, `realtimeSinceStartup`,
  `timeSinceLevelLoad`, `frameCount`, `maximumDeltaTime`, `smoothDeltaTime`, `captureDeltaTime`,
  `inFixedTimeStep`.
- Roblox `task` (https://create.roblox.com/docs/reference/engine/libraries/task): `task.wait(d)` returns the
  elapsed time; `task.delay`; `task.cancel` closes the thread. Taken: wait helpers return elapsed time; cancel =
  `coroutine.close`.
- Godot `SceneTree.create_timer(time, process_always, process_in_physics, ignore_time_scale)` and `await`
  (https://docs.godotengine.org/en/stable/classes/class_scenetree.html): explicit unscaled flag per timer;
  taken. Defold `timer.delay(delay, repeating, cb)` (https://defold.com/ref/timer/): owned by the script,
  removed with it, at most once per frame; taken. hump.timer `after/every` (https://hump.readthedocs.io/en/latest/timer.html):
  names taken for the `Timer` module.
- Lua 5.4 (https://www.lua.org/manual/5.4/manual.html): yields across `pcall` are allowed; yields across a
  C call without continuation raise "attempt to yield across a C-call boundary"; `lua_closethread` closes a
  suspended thread running its to-be-closed variables; `luaL_traceback(L, co, msg, 0)` gives the coroutine's stack.

## 5. Product decisions

1. **Wait helpers are functions that yield** (`Coroutine.wait(2)`), not yield-instruction objects
   (`coroutine.yield(WaitForSeconds(2))`): shorter, typed by LuaLS, and Lua yields through nested plain calls,
   so helper functions compose. `coroutine.yield()` with no value is accepted as "next frame" (Unity's
   `yield return null`); `coroutine.yield(handle)` (a Coroutine/Timer/awaitable) = `Coroutine.waitFor(handle)`.
   Anything else yielded is an error naming the helpers.
2. **`Coroutine`** is the engine class (handles + static helpers); Lua's own `coroutine` library is untouched
   and still usable for generators inside engine coroutines.
3. **Realtime = the unscaled game clock**: advances by the unscaled frame delta (clamped by
   `Time.maximumDeltaTime`, replaced by `Time.captureDeltaTime` when set), keeps running at time scale 0,
   **stops while the app is paused (background) and while the editor's Play is paused/held** (unlike Unity's
   wall clock: stepping the editor or backgrounding the app does not fire every realtime wait at once). It is the
   HTML UI clock's delta (`Engine.cpp:1359`).
4. **Timers keep running while the script is disabled or its object inactive** (Unity Invoke); coroutines
   stop on deactivation (decision 8 of script-lifecycle). Both are cancelled when the owner instance is
   destroyed/released (destroy, scene unload, Play stop).
5. **Every wait resumes on a later frame** (first eligible check in the frame after the yield), except
   `waitForEndOfFrame` (same frame when yielded before the end-of-frame phase) and `waitForFixedUpdate` (the
   next fixed step, possibly in the next frame).
6. **Timers fire at most once per frame**; a repeating timer whose next due time has already passed is
   rescheduled from now (no catch-up bursts), like Defold.
7. **Unowned work** (`Coroutine.start`, `Timer.after/every` without `owner`) survives scene loads and ends at
   Play stop / shutdown; documented as the tool for `require`d modules and HTML UI document scripts. Scripts
   should use the `self:` forms.
8. **`Time` is the documented time surface**; `Engine.deltatime/updateTime/timeScale/fixedDeltatime/gameTime`
   stay (compatibility) and lua-api.md maps them in one table ("`Engine.updateTime` = `Time.fixedDeltaTime`;
   `Engine.fixedDeltatime` = `Time.captureDeltaTime`, not the fixed step"). `Engine` also gets
   `unscaledDeltatime`, `unscaledTime` and `maxDeltatime` for symmetry.
9. **`anime.createTimer` stays the HTML UI clock** (milliseconds, unscaled, survives scene loads, anime.js
   API); lua-api.md says "for gameplay use `self:invoke` / `Timer.after` (seconds, scaled by default)" and the
   wrong sentence at lua-api.md:569 is replaced.

## 6. Design: runtime C++

### 6.1 Clocks (`engine/core/Engine.{h,cpp}`)

New statics: `double unscaledDeltatime` (frame), `double unscaledTime` (sum), `double fixedTime` (sum of
fixed steps), `double levelLoadGameTime` (gameTime at the last `SceneManager::runFactory`), `double startupTime`
(`stm_now` at `systemInit`), `bool inFixedStep`, `float smoothDeltatime`. In `runFrame` (`Engine.cpp:1356-1359`):
`unscaledDeltatime = (paused || simulationHeld) ? 0 : frameTime;` and inside the update block
`unscaledTime += unscaledDeltatime;`. The fixed loop sets `inFixedStep = true` around `onFixedUpdate` + scene fixed
updates + the fixed phase, and `fixedTime += updateTime` per step. `smoothDeltatime += 0.2f * (deltatime - smoothDeltatime)`
each updated frame. Getters `getUnscaledDeltatime()`, `getUnscaledTime()`, `getFixedTime()`,
`getRealtimeSinceStartup()`, `getTimeSinceLevelLoad()`, `isInFixedStep()`, `getSmoothDeltatime()`.

### 6.2 `jasperengine::Time` (`engine/core/Time.{h,cpp}`, static facade, C++ and Lua)

| Lua `Time.` | C++ | Kind | Value |
|---|---|---|---|
| `deltaTime` | `getDeltaTime()` | ro | inside a fixed step: `fixedDeltaTime`; else `Engine::deltatime` |
| `unscaledDeltaTime` | `getUnscaledDeltaTime()` | ro | inside a fixed step: `fixedUnscaledDeltaTime`; else `Engine::unscaledDeltatime` |
| `fixedDeltaTime` | get/set | rw | `Engine::updateTime` (game seconds per fixed step, default 1/60) |
| `fixedUnscaledDeltaTime` | get | ro | `updateTime / timeScale` (0 when timeScale is 0) |
| `time` | `getTime()` | ro | inside a fixed step `fixedTime`, else `gameTime` |
| `fixedTime` | | ro | sum of fixed steps |
| `unscaledTime` | | ro | `Engine::unscaledTime` |
| `realtimeSinceStartup` | | ro | wall clock since start (includes pauses) |
| `timeSinceLevelLoad` | | ro | `gameTime - levelLoadGameTime` |
| `frameCount` | | ro | `Engine::gameFrames` |
| `timeScale` | | rw | `Engine::timeScale` (same clamp [0,100]) |
| `maximumDeltaTime` | | rw | `Engine::maxDeltatime` (default 0.25; values <= 0 ignored with a warning) |
| `captureDeltaTime` | | rw | `Engine::fixedDeltatime` (0 = measured frames) |
| `smoothDeltaTime` | | ro | exponential average of `deltaTime` |
| `inFixedTimeStep` | | ro | true inside `onFixedUpdate` and the fixed phase |

Bound in `CoreClassesLua.cpp` as `beginClass<Time>("Time")` static properties. `Engine` gains ro
`unscaledDeltatime`, `unscaledTime`, rw `maxDeltatime`.

### 6.3 `ScriptScheduler` (`engine/core/script/ScriptScheduler.{h,cpp}`)

```cpp
enum class TimeMode : uint8_t { Scaled, Unscaled };
using TaskId = uint64_t;   // monotonically increasing, never reused; 0 = none

class JASPERENGINE_API ScriptScheduler {
public:
    // phases, called by Engine::runFrame (6.6)
    static void fixedPhase();
    static void updatePhase();
    static void endOfFramePhase();

    // ownership; owner = ScriptEntry::instance (Lua ref as void*) or a ScriptBase* (C++); nullptr = unowned
    static void stopCoroutinesOf(const void* owner);   // deactivation: coroutines only
    static void cancelOwner(const void* owner);        // destroy/release: coroutines and timers
    static void clear();                               // Play stop, shutdown: everything

    // C++ timers
    static TaskId after(double delay, std::function<void()> fn, TimeMode, const void* owner = nullptr);
    static TaskId every(double delay, double interval, std::function<void()> fn, TimeMode, const void* owner = nullptr, int count = -1);
    static bool cancel(TaskId);
    static bool isPending(TaskId);
    static size_t coroutineCount(); static size_t timerCount();   // stats (debug-overlay)

    // Lua side (bindings call these)
    static int luaStart(lua_State* L, const void* owner, Scene*, Entity, int fnIndex, int nargs); // pushes handle
};
```

Records:

```cpp
struct CoroutineRecord {
    TaskId id; lua_State* thread; int threadRef;          // thread anchored in the registry
    const void* owner; Scene* scene; Entity entity;       // for messages and the inactive check
    std::string className, origin;                        // "Spawner", "Spawner.lua:12"
    std::string methodName;                               // when started by name (stopCoroutine("name"))
    enum class Wait { None, Frames, Seconds, Fixed, EndOfFrame, Until, While, Target } wait;
    TimeMode clock; double due, waitStart, timeout;       // timeout < 0 = none
    uint64_t resumeFrame;                                 // first frame it may resume (decision 5)
    int predicateRef; TaskId targetId; int targetRef;     // waitUntil/While, waitFor
    enum class Status { Running, Suspended, Finished, Stopped, Error } status;
    bool stopRequested; int resultsRef; std::string error;
    int handles;                                          // live Coroutine userdata; record kept while > 0 once ended
};
struct TimerRecord {
    TaskId id; const void* owner; TimeMode clock; double due, interval; int remaining /* -1 = forever */;
    uint64_t firstFrame; std::variant<int /*Lua fn ref*/, std::string /*method name on owner*/, std::function<void()>> callback;
    int ownerRef;  // Lua instance ref for method-name timers
    bool cancelled, fired;
};
```

Storage: `std::vector` of records in creation order (deterministic processing), indexes by id in an
`unordered_map`; timed waits additionally in two min-heaps by `due` (scaled, unscaled) so a frame with nothing due
costs O(1) per clock; frame/predicate/target waits in a list scanned each update phase. Ended records are erased
at the end of each phase unless a handle still references them (then only the result/error stays).

Phase work:
- `updatePhase()` (after `Engine::onUpdate`): (1) timers whose `due <= clock` and `firstFrame <= frame`, in
  creation order, each at most once; repeating: `due += interval`, if `due <= now` then `due = now + interval`;
  callback errors logged, repeating timers continue; (2) coroutines waiting Frames/Seconds/Until/While/Target
  that are eligible (`resumeFrame <= frame`), in creation order: predicates called (error = coroutine error),
  targets polled (6.4); resume those satisfied. Records created during the phase are not processed in it.
- `fixedPhase()` (after each fixed step's scene `fixedUpdate` and the physics-contacts dispatch): coroutines
  waiting `Fixed` whose wait was recorded before this step began.
- `endOfFramePhase()` (after draw/commit): coroutines waiting `EndOfFrame` recorded before the phase began.
- Before resuming, a coroutine whose owner's GameObject is no longer active in hierarchy is stopped (covers
  activation paths that did not call `stopCoroutinesOf`).

Resuming: `lua_resume(rec.thread, mainThread, nargs, &nres)`. `LUA_YIELD`: read the wait the helper recorded
(6.4) or interpret the yielded values (decision 1); `LUA_OK`: finished, results kept in a table ref;
error: status Error, message = `luaL_traceback(main, thread, msg, 0)`, logged (6.7), thread closed with
`lua_closethread(thread, main)`. Stopping: a suspended thread is closed at once (`lua_closethread`, errors of
to-be-closed variables logged); a running/normal one gets `stopRequested` and is closed when control returns to
the scheduler (its next yield or end).

### 6.4 Wait helpers (C functions bound as `Coroutine.*`)

Each helper finds the record of the running thread (`unordered_map<lua_State*, TaskId>`), errors when the
running thread is not an engine coroutine (`"Coroutine.wait: not inside an engine coroutine (start one with
self:startCoroutine or Coroutine.start; event handlers like onUpdate cannot wait)"`), stores the wait in the
record, sets `resumeFrame = Engine::getGameFrames() + 1` (except EndOfFrame/Fixed), and returns
`lua_yield(L, 0)`. On resume the scheduler pushes the helper's results.

| Helper | Wait | Resumes with |
|---|---|---|
| `Coroutine.wait(seconds)` | scaled clock `>= start + seconds` (seconds <= 0: next frame) | elapsed scaled seconds |
| `Coroutine.waitRealtime(seconds)` | unscaled clock | elapsed unscaled seconds |
| `Coroutine.nextFrame()` | next update phase | nothing |
| `Coroutine.waitFrames(n)` | n update phases (integer >= 1) | nothing |
| `Coroutine.waitForFixedUpdate()` | next fixed phase | nothing |
| `Coroutine.waitForEndOfFrame()` | end-of-frame phase | nothing |
| `Coroutine.waitUntil(pred [, timeout [, unscaled]])` | `pred()` truthy (checked each update phase) | `true`, or `false` on timeout |
| `Coroutine.waitWhile(pred [, timeout [, unscaled]])` | `pred()` falsy | same |
| `Coroutine.waitFor(x)` | see below | see below |

`waitFor(x)`, polled each update phase:
- `Coroutine` handle: ended -> `true, ...results` (Finished) or `false, err` (Error: message; Stopped: `"stopped"`).
- `Timer`: fired (one-shot) -> `true`; cancelled -> `false`. A repeating timer is an error ("never done").
- `Action` and subclasses (Tween, Animation, ParticleSystem, ...): `isStopped()` and no start pending -> `true`;
  entity gone -> `false`.
- `AudioSource`: not playing (stopped or never started) -> `true`; entity gone -> `false`.
- `AnimationPlayer`: not playing -> `true`. `AnimeTimer`/`AnimeAnimation`/`AnimeTimeline`: `completed` ->
  `true`, `cancelled` -> `false`.
- Any other table/userdata with a callable `isDone` field: `x:isDone()` truthy -> `true, x` (the protocol
  feature `http` implements on its request handle).
- Otherwise error "Coroutine.waitFor: cannot wait for a <type>". Waiting for the running coroutine itself
  errors.

Other statics: `Coroutine.start(fn, ...)` (unowned, returns handle), `Coroutine.running()` (handle or nil),
`Coroutine.isInside()` (bool). Values of a raw `coroutine.yield(...)` follow decision 1.

### 6.5 Lua API on script instances (via `ScriptInstanceBase`, script-lifecycle 6.7)

Until script-lifecycle merges, this feature installs the same `__index` chain itself (one implementation, the
second feature to merge reuses it).

| Method | Semantics / errors |
|---|---|
| `self:startCoroutine(fnOrMethodName, ...) -> Coroutine` | runs `fn(...)` (or `self[name](self, ...)`) in a new engine coroutine owned by this script until its first wait; error if the GameObject is inactive (`"startCoroutine: GameObject 'X' is inactive"`) or the method does not exist; allowed when the script is disabled |
| `self:stopCoroutine(coOrMethodName)` | stops that coroutine (must be owned by `self`, else error) / every coroutine of `self` started with that method name; unknown/ended: no-op |
| `self:stopAllCoroutines()` | every coroutine owned by `self` (others untouched) |
| `self:invoke(fnOrMethodName, delay [, unscaled=false]) -> Timer` | one call after `delay` seconds; `fn()` without args, or `self:<name>()` resolved when it fires (missing method: error logged once, timer ends) |
| `self:invokeRepeating(fnOrMethodName, delay, interval [, unscaled=false]) -> Timer` | first after `delay`, then every `interval` (> 0, else error) |
| `self:cancelInvoke([timerOrMethodName])` | no arg: all timers of `self`; string: those invoking that method; Timer: that one |
| `self:isInvoking([timerOrMethodName]) -> bool` | pending timers matching, like `cancelInvoke` |

`Coroutine` handle (userdata `ScriptCoroutine`): `status` (ro: `"running"`, `"suspended"`, `"finished"`,
`"stopped"`, `"error"`), `error` (ro string or nil), `owner` (ro script instance or nil), `isDone()`, `stop()`,
`__tostring` (`Coroutine(12, suspended, Spawner.lua:12)`).

`Timer` module and handle (userdata `ScriptTimer`):
- `Timer.after(delay, fn [, opts]) -> Timer`, `Timer.every(interval, fn [, opts]) -> Timer`; `opts`:
  `unscaled` (bool), `owner` (a script instance: lifetime bound to it), `delay` (first delay for `every`,
  default = interval), `count` (`every` only: number of calls, default unlimited). `every` passes the Timer to
  `fn` (so `fn(t)` can `t:cancel()`); returning `false` from an `every` callback cancels it (hump rule).
- Handle: `cancel()`, `isDone()` (fired one-shot or cancelled), `remaining` (ro seconds to next call, 0 when
  done), `interval` (ro, 0 for one-shot), `unscaled` (ro).

### 6.6 Frame integration (`Engine::runFrame`, `Engine.cpp:1336-1490`)

```
applyPendingLoad
if (!paused && !held):
    ScriptRuntime::flushStarts()                       (script-lifecycle)
    fixed loop: onFixedUpdate; scenes fixedUpdate; [physics-contacts dispatch]; ScriptScheduler::fixedPhase()
    Engine::onUpdate; ScriptScheduler::updatePhase(); scenes update; onPostUpdate; ObjectManager::update; compact
draw; commit; one-shot retire
if (the update block ran this frame): ScriptScheduler::endOfFramePhase()
SceneManager::updateLoading()
```

Ownership calls: `LuaBinding::releaseLuaScriptInstance` (`LuaBinding.cpp:1054`) calls
`ScriptScheduler::cancelOwner(instance)` before releasing the ref; script-lifecycle's deactivation pass calls
`stopCoroutinesOf`; `~ScriptBase` calls `cancelOwner(this)`; the editor's Play stop (`Project.cpp:9235`, next to
`AnimEngine::instance().reset()`) and `Engine::systemShutdown` (`Engine.cpp:1570`, before `LuaBinding::cleanup`)
call `clear()`.

### 6.7 Error reporting

`Lua coroutine error in <Class> (GameObject '<name>', scene '<scene name>'), started at <origin>: <message>
stack traceback: ...` via `Log::error` (unowned: `Lua coroutine error (started at <origin>)`). `origin` comes
from `luaL_where(L, 1)` at `startCoroutine`/`Coroutine.start`. Timer callback errors:
`Lua timer error in <Class> (...), created at <origin>: ...`. With the crash guard handler set
(`FunctionSubscribe.h` `JASPERENGINE_CRASH_GUARD`), the same handler is called with the tag
`coroutine:<origin>`.

### 6.8 Main-thread normalisation of stored callbacks

New `lua_State* LuaBinding::mainThread(lua_State* L)` (`lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD)`).
- `LuaFunctionBase` (`LuaFunctionBase.cpp:29-50`): `store_function` takes the ref with the given `L`, then sets
  `m_vm = mainThread(L)`; copy constructor and `operator=` unchanged (they copy the main thread).
- `LuaRefHolder` (`HtmlUIAnimeLua.cpp:56-79`): stores `mainThread(L)`; `isAlive`/`release` compare with it.
- `HtmlUILuaRef` (`HtmlUIClassesLua.cpp:993-1035`): stores `mainThread(L)`; `push(current)` accepts any thread
  whose `mainThread(current)` equals it and pushes on `current`.
- Audit rule written in `LuaBinding.h`: "never store a `lua_State*` received from a binding; store
  `LuaBinding::mainThread(L)`". A grep test (`tools/mcp/tests` or a gtest) is not needed; the unit tests of 10
  cover the three classes.

### 6.9 C++ script API

`ScriptBase` (`engine/core/script/ScriptBase.h`): `TaskId invoke(std::function<void()>, float delay,
TimeMode = TimeMode::Scaled)`, `TaskId invokeRepeating(std::function<void()>, float delay, float interval,
TimeMode = TimeMode::Scaled)`, `bool cancelInvoke(TaskId)`, `void cancelAllInvokes()`, `bool isInvoking() const`
(owner = `this`); `~ScriptBase()` cancels them. Free use: `ScriptScheduler::after/every`. Callbacks run in the
update phase like Lua timers; exceptions are not caught (C++ rules), crash guard applies when enabled.

## 7. Editor UX, MCP, exported builds, docs

- Inspector during Play: under each Lua script, "Coroutines: 2, Timers: 1" (dim text) when non-zero.
- MCP `get_runtime_entity` (`PlayTools.cpp:3482`): each Lua script gains
  `coroutines: [{id, status, waiting: "seconds 1.25 left" | "frames 1" | "until" | "for Coroutine 7" |
  "end of frame" | "fixed update", origin, method}]` and `timers: [{id, remaining, interval, unscaled,
  callback: "spawn" | "function"}]`; `get_play_state` gains `coroutines` and `timers` totals. No new tool; the
  existing `wait` tool already advances frames/game seconds.
- Exported builds: nothing generated; the scheduler lives in the engine library; every platform runs the same
  code (Web without threads included: all on the main loop).
- Docs: lua-api.md new section "2.x Coroutines and timers" (helpers table, ownership rules, frame order,
  examples, pitfalls: cannot wait in onUpdate; realtime stops when paused; timers continue when inactive),
  "2.x Time" with the Engine name map; 2.12 sentence about timers replaced; html-ui guide note
  (`anime.createTimer` vs `Timer.after`); unity-glossary rows: StartCoroutine/StopCoroutine/
  StopAllCoroutines, WaitForSeconds -> `Coroutine.wait`, WaitForSecondsRealtime -> `Coroutine.waitRealtime`,
  `yield return null` -> `Coroutine.nextFrame()`, WaitForFixedUpdate, WaitForEndOfFrame, WaitUntil/WaitWhile,
  `yield return StartCoroutine(x)` -> `Coroutine.waitFor(...)`, Invoke/InvokeRepeating/CancelInvoke/IsInvoking,
  Time.* names, LeanTween.delayedCall -> `self:invoke`. `cpp-scripting.md`: `invoke`.
- Stubs: `Coroutine`, `Timer`, `Time` come from the bindings; the C-function helpers need a hand-written
  fragment `engine/core/script/binding/stubs/Coroutine.lua` (signatures, `---@async`), and the
  `ScriptInstance` methods are added to the inline `Script.lua` (`generate_lua_stubs.py:2955-2999`).
- `McpInstructions.cpp`: one line "waits: self:startCoroutine(fn) + Coroutine.wait/nextFrame/waitUntil; timers:
  self:invoke; time: Time.deltaTime/unscaledDeltaTime".

## 8. Behaviour rules and edge cases

- First segment: `startCoroutine` runs the body synchronously until its first wait or end, then returns the
  handle (status `suspended` or `finished`). An error in the first segment is logged like any coroutine error
  and the handle is returned with status `error` (no Lua error is raised in the caller, Unity-like).
- Time scale 0: scaled waits and scaled timers never complete; fixed phases do not happen; realtime waits,
  unscaled timers, frame and predicate waits keep working.
- App paused / editor Play paused: no phase runs; no clock advances. Editor "step": one frame of everything.
- Scene load: owned coroutines/timers end in the unload pass with their owners (before the new scene's
  `init`); unowned ones and those of persistent objects continue; `Time.timeSinceLevelLoad` resets.
- Destroy during iteration: a coroutine or timer whose owner is destroyed inside another callback of the same
  phase is skipped (records are checked for `cancelled`/ended before each run).
- A coroutine stopping itself (`self:stopAllCoroutines()` inside): code continues to its next wait, which then
  ends it (no further resume). `co:stop()` on a coroutine in status `normal` (it started the running one) is
  deferred the same way.
- Nested starts: a coroutine may start others; each is independent; parent and child ownership are per call.
- `waitFor` a coroutine that already ended returns immediately (no frame).
- Re-entrancy: `startCoroutine` from inside timers, predicates, lifecycle methods and engine events is allowed;
  predicates must not wait (they are called with pcall, so a wait inside errors).
- Lua user coroutines inside an engine coroutine: `coroutine.wrap` generators work; calling a wait helper inside
  them errors (6.4 message).
- Yield across a C call (e.g. inside a `table.sort` comparator): Lua's error is logged with a hint "waits cannot
  cross a C function".
- Hot reload / Play restart: `clear()` releases every thread; no record survives a Play session.
- Garbage: discarded handles do not stop coroutines (Unity); a finished coroutine's results are freed when its
  last handle is collected.

## 9. Performance

- Per resume: one `lua_resume` (~1 us desktop). Per frame with nothing due: O(1) per heap + O(frame/predicate
  waits). Target: 10,000 coroutines waiting seconds + 1,000 timers < 0.3 ms per frame on desktop, < 1 ms on a
  mid Android phone; memory ~1 KB per coroutine (a Lua thread) plus its stack.
- No per-frame allocation in the scheduler when nothing is created (vectors reused, erased in place).

## 10. Tests

gtest `editor/tests/ScriptSchedulerTest.cpp` (harness of script-lifecycle 11; deterministic frames with
`Engine::setFixedDeltatime(1/60.0)`; `Engine::systemUpdate()`), scripts log `Time.frameCount` and events:
1. `startCoroutine` runs to the first wait synchronously; `nextFrame` resumes at frame+1 after `onUpdate`,
   before `onPostUpdate` (log order `update, co, post`).
2. `wait(0.5)` at timeScale 1 resumes after 30 frames, returns ~0.5; at `Time.timeScale = 0` never; with
   `waitRealtime(0.5)` it resumes after 30 frames at timeScale 0.
3. `waitForFixedUpdate` resumes after the physics step (a body's position already advanced); with
   `Engine.updateTime = 1/30` and 60 fps frames it resumes every other frame.
4. `waitForEndOfFrame` resumes in the same frame after `onDraw` (systemDraw path with the GPU test env, or a
   test hook `Engine::runFrame(false)` counting the phase).
5. `waitUntil/waitWhile` with and without timeout; predicate error ends the coroutine with a logged traceback.
6. `waitFor` on: a child coroutine (results), a failed child (`false, msg`), a stopped child, a Timer, a
   PositionTween, an AudioSource (test sound), an object with `isDone`.
7. Ownership: deactivating the GameObject stops its coroutines (status `stopped`), timers continue;
   `self.enabled = false` stops neither; `GameObject.destroy` cancels both; scene load cancels owned, keeps
   `Coroutine.start` ones; `startCoroutine` on an inactive object errors.
8. `stopCoroutine(handle)`, `stopCoroutine("name")`, `stopAllCoroutines()` incl. from inside itself; to-be-closed
   variable closed on stop.
9. Timers: `invoke` at 1.0 fires once at frame 60; `invokeRepeating(…, 0.5, 0.25)`; interval smaller than the frame
   fires once per frame; `cancelInvoke("m")`, `isInvoking`; unscaled timer at timeScale 0; error in a repeating
   callback logged and it continues; `Timer.every` returning false stops; `count`.
10. Main-thread normalisation: inside a coroutine register `RegisterEvent(self, button.onPress, "p")`, an anime
    timer `onComplete`, and a UIDocument listener; end the coroutine, `collectgarbage("collect")` twice, trigger:
    all three fire (today: anime/HTML ones never fire, LuaFunction one is undefined behaviour).
11. Error message format (class, GameObject, scene, origin, traceback lines of the coroutine).
12. `Time` values: `deltaTime` inside `onFixedUpdate` equals `fixedDeltaTime`; `unscaledDeltaTime` at timeScale 0.5;
    `timeSinceLevelLoad` resets after `SceneManager.loadScene`; `captureDeltaTime` alias writes `Engine.fixedDeltatime`.
13. C++: `ScriptScheduler::after/every` and `ScriptBase::invoke` cancelled by deleting the script.
MCP `tools/mcp/tests/test_coroutines.py`: script with a coroutine moving an object 3 times with `Coroutine.wait(0.1)`;
`play`, `wait game_seconds=0.35`, `eval_lua` reads position; `get_runtime_entity` lists the coroutine; error case reads
the console text; `play stop` leaves no records (`get_play_state` after restart shows 0).

## 11. Acceptance criteria

- [ ] All helpers of 6.4 and methods of 6.5 behave as specified in editor Play and in an exported Linux build (same
      test scene produces the same log); Web build compiles and runs the test scene in a browser smoke test if
      available.
- [ ] Ownership rules (deactivate/disable/destroy/unload/stop) as in 5.4 and 8.
- [ ] `Time` class and the Engine additions; lua-api.md maps old names; :569 fixed.
- [ ] The three stored-callback classes keep the main thread; test 10 passes under ASan in a debug build.
- [ ] Errors carry the coroutine traceback and the start origin.
- [ ] MCP fields; stubs with no `-- unresolved:`; glossary rows; C++ timer API documented.

## 12. Integration points

`Engine.cpp:1336-1490` (phases, clocks), `:1164-1194` (startup time), `:1568-1576` (shutdown clear);
`Engine.h:358-375, 504-532`; `CoreClassesLua.cpp:727-745` (Engine props; add `Time` class);
`LuaBinding.cpp:1054-1063` (release -> cancelOwner), `:858` (instance base methods); `LuaFunctionBase.cpp:29-68`;
`HtmlUIAnimeLua.cpp:52-90`; `HtmlUIClassesLua.cpp:993-1035`; `ScriptBase.{h,cpp}`; `Project.cpp:9235` (Play stop);
`PlayTools.cpp:3393, 3482`; `generate_lua_stubs.py:2955-2999`; `docs/mcp/guide/lua-api.md:569`.

## 13. Cross-feature dependencies

- **script-lifecycle**: `start` runs through `ScriptScheduler` (it may wait); deactivation calls
  `stopCoroutinesOf`; destroy/unload/release call `cancelOwner`; shared `ScriptInstanceBase`. Merge either first;
  the second wires the calls (both specs name them).
- **physics-contacts**: its per-object dispatch runs before `fixedPhase()` in each fixed step.
- **http / native-bridge**: request/call handles implement `isDone()` (the protocol of 6.4) so
  `Coroutine.waitFor(req)` works; their callbacks are stored with `LuaBinding::mainThread` (6.8); http drains its
  `MainThreadQueue` right before `Engine::onUpdate` (http spec), so a coroutine waiting on a request resumes in the
  same frame's update phase, after the request's own callback ran.
- **persistence**: persistent objects' coroutines survive loads (owner not unloaded).
- **debug-overlay**: shows `coroutineCount()/timerCount()` in full stats.
- Risk: decision 3 (realtime stops while paused) differs from Unity; documented in the glossary row.

## Implementation notes

Implemented on `gaps/coroutines` (based on `main` with `script-data` and `input`). Where the code differs from
the text above, or the text left a choice, this is what was done and why.

### Files and names
- `Time` is `engine/core/EngineTime.{h,cpp}`, not `Time.{h,cpp}`: the engine's include paths are searched for
  `<...>` includes too, so on case-insensitive file systems (macOS, Windows, iOS) a `Time.h` there would stand in
  for the C library's `<time.h>`.
- Lua bindings: `engine/core/script/binding/CoroutineClassesLua.cpp` (`Coroutine` = C++ `ScriptCoroutine`,
  `Timer` = `ScriptTimer`, `Time`), registered by `LuaBinding::registerCoroutineClasses`. The lua_CFunctions are
  `ScriptSchedulerLua::*` in `ScriptScheduler.cpp`. New sources are in `engine/CMakeLists.txt` and the Xcode
  project (`engine/workspaces/xcode`).
- The C++ `Timer` facade (spec goal 6, `Timer::after`) is `engine/core/script/Timer.h`, included explicitly
  (`#include "Timer.h"`), not by `ScriptBase.h`: a script doing `using namespace jasperengine` with a `Timer`
  class of its own would otherwise stop compiling.
- C++ start of a Lua coroutine (for `script-lifecycle`'s `start`): `ScriptScheduler::startCoroutine(L,
  instanceIndex, fnIndex, nargs, methodName)` instead of the `luaStart` signature of 6.3; its origin is the
  function's definition line (there is no Lua caller).

### Scheduler
- Clocks and frames are the scheduler's own: `ScriptScheduler::beginFrame(scaled, unscaled)` runs in the update
  block of `Engine::runFrame` only while the game events are enabled (`Engine::onUpdate.isEnabled()`). The
  editor's Play pause is `Engine::pauseGameEvents(true)`, during which the update block still runs and
  `Engine::gameTime` still advances; decisions 3 and 8 ("no clock advances while the editor's Play is paused")
  need the gate. Waits therefore count scheduler frames, not `Engine::getGameFrames()`. Consequence (documented):
  a game calling `Engine.pauseGameEvents(true)` also freezes coroutines and timers.
- Records are kept in two vectors in creation (= id) order, found by binary search; there are no heaps: each phase
  scans the hot fields of the records (cheap, deterministic order for free). Compaction runs only when something
  ended or a handle went, so a frame where nothing changes allocates nothing. Measured (test
  `ManyWaitingCoroutinesAndTimersCostLittlePerFrame`, Release, loaded shared host): 10,000 coroutines waiting
  seconds + 1,000 timers cost about 0.055 ms for the whole frame (the spec's target: 0.3 ms).
- A wait recorded during a fixed step (in `onFixedUpdate`) resumes after the next step's physics ("recorded
  before this step began"); Unity would resume a coroutine started in FixedUpdate after the same step.
- `waitFor` returns at once only for a `Coroutine` that already ended (as written); every other awaitable resumes
  on a later frame even when already done. A counted `Timer.every` can be waited for (an endless one raises).
  AudioSource: done = stopped with no play pending (a paused source is not done). A one-shot `self:invoke("m")`
  whose method is missing ends cancelled (waitFor gives false).
- A running coroutine that stopped itself and then reaches its end without another wait ends "finished".
- Handles: one userdata per coroutine/timer while Lua holds it (a weak registry table), so
  `co == Coroutine.running()`; a finished coroutine's results stay while a handle is alive.
- The first segment runs under the caller's debug hook (eval_lua's timeout) and later segments under none:
  `lua_newthread` copies the creating thread's hook, which would keep eval_lua's timeout on the coroutine for good.
- Errors: when the crash guard handler is set (the editor during Play), coroutine and timer errors go to it with
  the tag `coroutine:<origin>` / `timer:<origin>` (C++ timers `timer:C++`), exactly as `LuaFunctionBase` sends
  event-callback errors, so in the editor a coroutine error stops Play like an `onUpdate` error does; without it
  (exported builds) they are logged and the game goes on. Lua's own message part keeps Lua's chunk name
  (`[string "Spawner.lua"]:3: boom`); the header prints the origin as `Spawner.lua:12`.
- Re-entrancy: no reference to a record is held across a Lua call (allocations may run finalizers, which may start
  coroutines and move the vectors); records are never erased while a phase, a resume or a close is on the C stack.

### Review fixes
- No record state read before user Lua runs (an `isDone()`, a predicate, a finaliser, a `__close`) is used after
  it: `pollTarget` and the predicate path find the record again and require it still suspended on the same wait
  with a live thread; results go to the main state and are moved to the thread after that check.
- A thread that runs Lua (resumed by the engine, or by a `coroutine.resume` of user code) is never closed: a stop
  is deferred and the next phase ends it. The wait helpers yield with a continuation that refuses a resume that
  is not the engine's (`coroutine.resume` on an engine coroutine raises; the coroutine then ends with an error),
  and every helper refuses to run unless its coroutine is the one the engine resumed.
- `waitFor` of an ended coroutine after a stop request yields (the coroutine ends there) instead of returning.
- `waitFor` raises on a cycle of coroutines waiting for each other.
- C++ timers keep one callable (`std::shared_ptr<std::function<void()>>`): a repeating `mutable` lambda keeps its
  state.
- Deactivation looks only at the deactivated subtree, through an index of the live owned coroutines by
  (registry, entity) (`ScriptScheduler::stopCoroutinesOfInactive(registry, entities, count)`,
  `hasCoroutinesIn`); nothing is scanned when the scene has none. 1,000 deactivations with 10,000 coroutines
  elsewhere: see `DeactivatingGameObjectsWithoutCoroutinesIsCheapWithManyCoroutines`.
- `Engine::getUnscaledDeltatime()` (same signature) is now also 0 while the game events are paused (the editor's
  Play pause, edit time), and `unscaledTime` stops then; the HTML UI clock keeps its old rule.

### Ownership
- Release: `LuaBinding::releaseLuaScriptInstance` calls `ScriptScheduler::cancelOwner(instance)` (destroy, scene
  unload in the generated `cleanupScripts` and the editor, Play stop); `~ScriptBase` calls `cancelOwner(this)`
  (the generated code deletes C++ scripts by their class). `cancelOwner` forgets the owner pointer of what it
  stopped (a Lua registry ref is reused by the next instance).
- Deactivation: before `script-lifecycle` this branch hooked `EntityRegistry::setEntityActive` / `addEntityChild`;
  with it merged, lifecycle's activation pass calls the scheduler (below). The check before every resume (6.3)
  stays as the catch-all.
- `ScriptScheduler::clear()`: editor Play stop (next to `AnimEngine::reset`), failed Play startup,
  `Engine::systemShutdown` and `LuaBinding::cleanup` (no thread may outlive the state).
- `ScriptScheduler::rebind(owner, scene, entity)` exists for `persistence`.

### Main-thread normalisation (6.8)
- `LuaFunctionBase` stores the ref with the given state and keeps `LuaBinding::mainThread(L)`.
- anime's `LuaRefHolder` keeps the main thread and gained `push(onto)`: `pushTarget` and `tableTarget` pushed on
  the holder's thread but used the caller's stack (a stack mismatch when called from a coroutine).
- `HtmlUILuaRef` keeps the main thread; `push(current)` accepts any thread of the state and pushes on it (so
  `removeEventListener` from a coroutine finds its listener).

### Editor, MCP, docs
- Inspector: a "Running" row "Coroutines: n, Timers: m" (dim, with a tooltip) under a script during Play when
  non-zero (Lua and C++ scripts, single selection).
- MCP: `get_runtime_entity` Lua scripts gain `coroutines` [{id, status, waiting, origin, method}] and `timers`
  [{id, remaining, interval, unscaled, callback}], C++ scripts `timers`; `get_play_state` gains `coroutines` and
  `timers` (and a text line). The two tool descriptions name them (+83 characters; `TOTAL_LIMIT` raised by
  that in `test_html_ui_tools.py`).
- eval_lua: its helpers (`print`, `entity`, `script`, `object`, `getScene`) held a pointer to the call's stack
  context; a coroutine or timer that eval_lua started and that ran later would use it after free. The context is
  now boxed and emptied when the call returns: a late `print` goes to the game's print (the Console), the other
  helpers raise a clear error.
- Stubs: the lua_CFunction signatures are `OVERRIDES` in `generate_lua_stubs.py` (the generator needs one per
  lua_CFunction anyway) with a new `'async': True` that emits `---@async`, instead of a hand-written
  `stubs/Coroutine.lua`; `stubs/Timer.lua` holds `TimerOptions`; the `ScriptInstance` methods are in `Script.lua`.
- `Engine::setMaxDeltatime` now warns when it ignores a value (Time.maximumDeltaTime's rule); added
  `Engine::markLevelLoad` (called by `SceneManager::runFactory` and `setCurrentScene`, the editor's Play start),
  `SceneManager::getSceneNameOf(Scene*)` (error messages), `LuaBinding::mainThread`, `pushScriptInstanceBase`,
  `ScriptScheduler::timerOwner / cancelTimersOf / coroutinesOf / timersOf / luaRefCount`,
  `ScriptBase::isInvoking(TaskId)`.

### Wired with `script-lifecycle` (merged from `main`)
1. One `ScriptInstanceBase`: lifecycle's table (`LuaBinding::pushScriptInstanceBase`) also gets this feature's
   seven methods (`ScriptSchedulerLua::addInstanceMethods`); this branch's own module-metatable install was dropped
   for lifecycle's class/instance scheme.
2. `ScriptRuntime::flushStarts` runs a Lua `start` with `ScriptScheduler::startCoroutine(L, self, start, 1,
   "start")`, so `start` may wait; C++ `start` stays a plain call.
3. Deactivation: `ScriptRuntime::onActiveInHierarchyChanged(!nowActive)` calls, after the `onDisable` calls,
   `ScriptScheduler::stopCoroutinesOfInactive(scene, changed, n)` (the owner index: only those GameObjects are
   looked at). The `EntityRegistry` hooks of this branch were dropped for lifecycle's activation observer.
4. Destroy and unload: `ScriptRuntime::destroyEntities` / `unloadScenes` call `ScriptScheduler::cancelOwner` for
   every script of the GameObjects between `onDisable` and `onDestroy` (Unity's order);
   `releaseLuaScriptInstance` and `~ScriptBase` keep theirs (idempotent). C++ owners are `ScriptEntry::cppScript`.
5. `self.enabled = false` stops nothing.

### Other features
- `http`: put `AsyncOwners::scriptInstanceReleased` next to `ScriptScheduler::cancelOwner` in
  `releaseLuaScriptInstance`; request handles are awaitable through `isDone()`.
- `persistence`: `ScriptScheduler::rebind(owner, scene, entity)`.
- `camera-shake`, `ui-canvas-group`: `Engine::getUnscaledDeltatime()` (float, 0 while paused or held).
- `debug-overlay`: `ScriptScheduler::coroutineCount()` / `timerCount()`.

### Not verified here
- An exported Linux build, the Web build in a browser and Android were not built or run; the new engine sources
  pass an `em++ -fsyntax-only` check (Emscripten 6.0.10, libc++, no threads).
- Test 10 under ASan in a debug build (the unit tests run in Release).
- lua-language-server is not installed on this host: `test_stubs.py`'s LuaLS checks (now with coroutine, timer
  and Time code in the known-good script) skip.

### Verification (after the review fixes and the merges of `main` up to shader-globals)
- Box 2201, Release: 0 warnings; unit suite 1444 passed, 8 skipped, 0 failed (one earlier run aborted once in
  `PersistenceTest.PersistingTheLevelCameraWarns` with a heap error that did not come back in a rerun, alone, or
  next to the coroutine tests; the coroutine, persistence and canvas-group tests are clean under ASan), 57
  coroutine tests; full MCP suite 876 passed, 11 failed (the 10 baseline failures and
  `test_stochastic_tiling.py::test_old_scene_resaves_byte_identical`), 19 skipped.
- 10,000 waiting coroutines + 1,000 timers: about 0.09 ms per frame on the box; 1,000 deactivations of GameObjects
  without coroutines next to 10,000 coroutines: about 0.8 ms in all.
