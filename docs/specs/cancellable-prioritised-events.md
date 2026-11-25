# Cancellable, Prioritised Events: listener priorities, vetoes the emitter must obey, and a monitor pass

Scripting follow-up. Builds on S09 (events and RAII subscriptions, landed) and S23 (Lua hot reload,
landed). Paths are relative to the repository root. Nothing here is a game mechanic: the feature is a
general event primitive, used the same way by an RPG's damage, a shop's purchase, a puzzle's door or a
platformer's pickup.

## 1. Goal

A script that owns an action (take damage, open a door, buy an item, pick something up) can tell
others it happened (`Event`), but it cannot ask them first: listeners run in the order they were
added and nothing can say "no" (`engine/core/script/ScriptEvent.h:17-21`). This spec adds an optional
`priority` to every Event listener (higher first, ties by registration order) and a
`CancellableEvent` whose listeners get a `CancelEventArgs` they can `cancel()`. The emitter's
`invoke` returns whether it was cancelled, and both languages make ignoring that a diagnostic (LuaLS
`discard-returns` in Lua, `[[nodiscard]]` under `WERROR=1` in C++), because a veto the emitter ignores
protects nothing. Monitor listeners run after the decision, see it, and cannot change it. When nobody
listens an invoke costs one test and allocates nothing; every existing `Event` call keeps its meaning.

## 2. Scope

**In scope**
- Lua `Event`: an optional integer `priority` on `addListener`, `add` and `RegisterEvent` (a new fifth
  argument); dispatch order by priority then registration; `Event.MONITOR`.
- Lua `CancellableEvent()` (an `Event`) and `CancelEventArgs` (`cancel()`, `cancelled`); `invoke`
  returns `cancelled`; skip-after-veto; the monitor pass; errors that name the fix.
- Inspection without new MCP text: `ev:getListeners()`, `CancellableEvent.cancelledBy`, a richer
  `tostring` (all readable through `eval_lua`).
- C++ `CancellableEvent<Args...>` and `CancelEventArgs` on S09's `detail::SlotList`, which gains an
  optional priority with the default path unchanged.
- `ScriptEvent` dispatch without the per-invoke snapshot copy (zero allocations per invoke).
- Lua API data (`tools/lua_api`), a `---@nodiscard` flag in the stub generator, docs and an eval case.

**Out of scope (decisions)**
- **Making existing engine events cancellable.** None becomes cancellable (D7): each reports something
  that already happened, and the engine's three "before" points have their own vetoes.
- **Priorities on engine events from Lua** (`Engine.onUpdate` order across scripts): that is Unity's
  Script Execution Order, a separate feature. `RegisterEvent` with a priority on a `FunctionSubscribe`
  is an error (R4).
- **Persistent listeners** authored in the Inspector (UnityEvent's serialized calls): Lua `Event` has
  none today and gets none.
- **A global bus keyed by event type** (Craftsman's design, section 4): events stay per object.
- **Mutable payload fields** as a mechanism. An emitter that wants a listener to lower the damage
  passes a table; documented as a pattern (R31), no API.
- **Cross-language listening** (a C++ listener on a Lua `CancellableEvent`, or the reverse),
  deferred or asynchronous vetoes, emission from threads other than the main thread (Lua) or outside
  external synchronisation (C++, as `Signal`).
- **MCP tool changes** (R30): inspection goes through `eval_lua`, which already renders the values.

## 3. Current state

All read from the code at the worktree's base commit (not run):

- **Lua `Event`.** `ScriptEvent` (`engine/core/script/ScriptEvent.h:22-69`) keeps
  `std::vector<std::shared_ptr<Listener>>` (`:60`); a `Listener` is a function ref, an owner ref plus
  method name, a tag and a `removed` flag (`:51-57`): no priority. Bound as the Lua class `Event` with
  `addListener`, `removeListener`, `removeAllListeners`, `invoke`, `add`, `remove`, `has`,
  `listenerCount`, `__tostring` (`engine/core/script/binding/ScriptClassesLua.cpp:638-652`).
- **Order.** `addListener` appends (`ScriptEvent.cpp:211-214`, `:230-235`) and ignores a listener
  added twice (`:205-209`, `:225-229`); `addTagged` (RegisterEvent, `add`) removes the old tag then
  appends, so a re-registration moves to the end (`:165`, `:175`).
- **Dispatch.** `luaInvoke` copies the listener vector into a snapshot on every call (`:280`): one heap
  allocation per invoke whenever there is a listener (an empty copy allocates nothing). It skips
  removed listeners and those of released owners (`:282-288`), looks a method up by name at call time
  (`:293-303`), and logs a listener's error with its traceback and goes on (`:307-310`). It returns
  nothing (`:314`). `tostring` gives `Event(N listeners)` (`:341-345`).
- **RegisterEvent.** `RegisterEvent(self, event, methodName [, tag])` (`LuaBinding.cpp:784-799`) reads
  argument 4 with `lua_tostring`, so a number there becomes a tag string (`:797`). For an `Event` it
  calls `addTagged` and returns (`:638-645`) before the hot-reload record that engine events get
  (`:667`, `LuaReload.h:39-43`).
- **Engine events.** `FunctionSubscribe<Ret(Args...)>` (`engine/core/util/FunctionSubscribe.h:104-383`):
  tagged subscribers (`add` = remove the tag, append: `:253-258`), `call` runs every subscriber under
  the crash guard when a `CrashHook` is set (`:134-163`, `:337-350`), `callRet` returns the first
  subscriber's result (`:352-374`). Engine's static events are all notifications of what happened:
  lifecycle, view, input, touch, gamepad, actions (`engine/core/Engine.h:1113-1163`).
- **Existing vetoes.** Three, each with its own shape: physics filters return `false` through
  `callRet`, so only the first subscriber is asked (`PhysicsSystem.h:724-742`,
  `JoltPhysicsAux.h:732`, `Box2DAux.h:128,143`; documented as "return false to skip",
  `docs/mcp/guide/lua-api.md:291-292,299`); `InputActionEvent::consume()` keeps a control from lower
  maps (`engine/core/input/InputActions.h:53-69`); HTML UI's `UIEvent` has `stopPropagation` and
  `preventDefault` (`engine/core/object/ui/UIEvent.h:30-31`). None is general, none orders listeners.
- **Shared core.** `detail::SlotList` (`engine/core/util/Signal.h:73-245`) backs `Signal` and
  `FunctionSubscribe`: `Slot` is `{id, ownerKey, tag, function, removed}` (`:89-95`), `add` appends
  (or queues in `pending` during an invoke, `:106-118`), `invoke` allocates nothing and skips removed
  slots (`:151-162`), `compact` appends `pending` after the outermost invoke (`:231-244`). Owner keys
  go through `SubscriptionOwnerIndex` so a script's release removes its subscriptions.
- **Clears at Stop.** `tools/check_event_clears.py:1-30` checks that every `FunctionSubscribe` member
  (or a class derived from one) is walked by a clear at Stop; `Signal` members are not in its scope
  (their subscribers hold RAII `Subscription`s, `docs/core-utilities.md:113-131`).
- **Docs and API data.** The Lua `Event` section is `docs/mcp/guide/lua-api.md:216-239`; the Lua API
  data for `Event` is the manual table in `tools/lua_api/parser.py:2040-2066` (the methods are
  `lua_CFunction`s the parser cannot read). `tools/lua_api/stubs.py:386-387` emits `---@deprecated`;
  there is no `---@nodiscard` support.
- **Tests.** `ScriptLifecycleTest.EventClass`, `UnregisterEvents`,
  `EventListenersDoNotKeepReleasedScriptsAlive` (`editor/tests/ScriptLifecycleTest.cpp:421,460,822`)
  pin Event's order, duplicate, removal and owner rules. `SignalTest.ZeroAllocInvoke`
  (`engine/tests/core/util/SignalTest.cpp:154-164`) and `SignalBench.InvokeOneSubscriberOverDirectCall`
  (`SignalBenchTest.cpp:21-42`, a +5 ns budget through `perfSlack()`) pin the core's cost.
  `tools/mcp/tests/test_stubs.py:550` (`test_luals_reports_api_mistakes`) maps lines to the LuaLS
  diagnostics it must report.
- **Agents.** `eval_lua` renders a userdata through `luaL_tolstring` (`editor/mcp/tools/PlayTools.cpp:978,1033`)
  and tables as JSON, so a richer `tostring` and a table-returning getter reach agents with no tool change.

## 4. Reference

| Source | Pattern studied | Take | Change |
|---|---|---|---|
| Craftsman `include/mcpe015/event_bus.h`, `src/event_bus.cpp` | A plugin event bus that fires **before** a change. `isCancellable(type)` is the single list of vetoable types; `emit` returns "cancelled" and the comment makes obeying it the caller's duty; on any other type `emit` forces `cancelled = false` after each listener so a listener cannot make an emitter believe it obeyed. A veto is sticky (a later listener cannot clear it) and the chain continues so observers see it. Order is priority descending, then a monotonically increasing registration sequence (`std::stable_sort`), never hash or address order. Lists are copy-on-write under a mutex; a constant-initialised 32-bit mask per type gates inline `emitXxx()` helpers whose out-of-line builders construct the event only when someone listens | The order rule; "only cancellable events can be vetoed"; the sticky veto; "the emitter must obey"; one test when idle; payload built only after that test | Per-object events, not a fixed global type enum (genre-agnostic). A non-cancellable event has **no** cancel at all (a Lua error or a C++ compile error) rather than a flag silently cleared. After a veto the remaining deciding listeners are skipped and observers run in a **monitor pass** (D4) instead of letting every listener run. No mutex or copy-on-write: Lua is main-thread and C++ uses SlotList's depth/pending scheme, allocation-free |
| Craftsman `src/event_bus_test.cpp` | Per cancellable type, a rig installs a cancelling listener and asserts the world did not change | Required of every future engine adopter (R21) | Our primitive's own tests are generic (AC1-AC14) |
| C# `System.ComponentModel.CancelEventArgs` (docs) | `Cancel` on the args a handler receives | The names `CancelEventArgs`, `cancel()`, `cancelled` | A method plus a read-only property (sticky) instead of a settable bool |
| Unity `UnityEvent` (docs) | `AddListener`, `RemoveListener`, `Invoke`; no priority, no veto | Already our `Event`'s names | Priority and veto are additions; `Event` stays UnityEvent-compatible |
| Bukkit `EventPriority` (docs) | `LOWEST…HIGHEST`, then `MONITOR`, which must not change the outcome | The monitor idea and name (`Event.MONITOR`) | Integer priorities instead of six levels; a monitor's `cancel()` is an error, not merely discouraged |

**Origin and IP.** The Craftsman tree is a reconstruction of a retail game; it was read for ideas only.
No code, data, assets or game-specific mechanics are copied: no event type names, no block, player or
chat concepts, no structure layout. Unity, C# and Bukkit are cited for names and semantics from their
public documentation only.

## 5. Requirements

### Functional: priorities (both kinds)

- **R1 Priority argument.** `ev:addListener(fn [, priority])`, `ev:addListener(owner, "method" [, priority])`,
  `ev:add(tag, fn [, priority])` and `RegisterEvent(self, ev, "method" [, tag] [, priority])` (a fifth
  argument; `tag` may be `nil`). `priority` is an integer in [-1,000,000, 1,000,000] or `Event.MONITOR`;
  default 0. Anything else raises `"<caller>: priority must be an integer from -1000000 to 1000000 or
  Event.MONITOR"`.
- **R2 Order.** Listeners run by priority, highest first; equal priorities in the order they were added.
  With every priority 0 the order is exactly today's.
- **R3 Re-adding.** Adding a listener that is already there (same function, or same owner and method)
  is ignored whatever its priority, as today; the docs say to remove it first to change its priority. A
  tag added again replaces its listener, which takes the new priority and goes after the others of that
  priority (today's "moves to the end").
- **R4 Engine events.** `RegisterEvent` with a priority on anything but an `Event` raises
  `"RegisterEvent: priority is for Event and CancellableEvent; engine events run in subscription order"`.
- **R5 Monitor on a plain Event.** Allowed: it runs after every other listener, in registration order.

### Functional: cancellable events

- **R6 Construction.** `CancellableEvent()` makes an event that is an `Event` (every R1-R5 member,
  `listenerCount`, `RegisterEvent`/`UnregisterEvent`, owner release) plus R7-R12.
- **R7 Listener arguments.** A listener receives a `CancelEventArgs` first, then `invoke`'s arguments:
  `fn(e, ...)`, a method `self:m(e, ...)`. `e:cancel()` vetoes; `e.cancelled` (read-only) tells whether
  some listener already did.
- **R8 Dispatch.** Deciding listeners (every priority but MONITOR) run in R2 order until one calls
  `e:cancel()`; the deciding listeners after it are skipped. Then monitor listeners run in registration
  order with `e.cancelled` final. A cancel cannot be undone.
- **R9 Result.** `ev:invoke(...)` returns `true` when a listener cancelled, else `false`; with no
  listener it returns `false` without making or touching a `CancelEventArgs`. The emitter must not
  apply the change when it gets `true`. The stub marks `invoke` `---@nodiscard`, so LuaLS reports
  `discard-returns` on a call whose result is dropped.
- **R10 Errors that name the fix** (raised inside the listener, so R11 logs them):
  - `e:cancel()` in a monitor listener: `"CancelEventArgs:cancel: a monitor listener runs after the
    decision and cannot cancel; give it a priority instead"`;
  - `e:cancel()` after its invoke ended (a stored `e`): `"CancelEventArgs:cancel: the invoke it belongs
    to has ended; cancel inside the listener"`;
  - `ev:cancel()` on a plain `Event`: `"Event:cancel: this Event cannot be cancelled; make it with
    CancellableEvent() and cancel through the listener's first argument"`.
- **R11 Listener errors.** A listener's error is logged with its traceback and the dispatch goes on, as
  `Event` does today (`ScriptEvent.cpp:307-310`). A `cancel()` made before the error stands.
- **R12 Nesting.** A listener may invoke the same event again; the nested dispatch has its own
  `CancelEventArgs` and its own result; the outer one is unaffected.

### Functional: re-entrancy and owners (both kinds)

- **R13** A listener added during an invoke runs from the next invoke; one removed during an invoke is
  skipped if it has not run; `removeAllListeners` during an invoke skips every listener that has not
  run; the running listener stays callable until it returns. (Today's semantics, now without the
  snapshot copy, D5.)
- **R14** Listeners owned by a script instance (method listeners, `RegisterEvent`) are dropped when the
  instance is released, also mid-dispatch (today's rule, `ScriptEvent.cpp:282-288`).

### Functional: inspection

- **R15 `ev:getListeners()`** returns an array, in dispatch order, of `{name, priority, monitor}`.
  `name` is `"<Class>:<method>"` for a method listener (the owner's `__name`, else `"Script"`), the
  script class and method parsed from a `RegisterEvent` tag (`<Class>_<address>_<method>`, the address
  dropped), the tag itself for any other tagged listener, and `"function <source>:<line>"` for a
  function. Same names in every place below.
- **R16 `CancellableEvent.cancelledBy`** (read-only): the name of the listener that cancelled the latest
  completed invoke; `nil` when it went ahead or before the first invoke.
- **R17 `tostring`.** A plain `Event` keeps `Event(N listeners)` byte for byte. A `CancellableEvent`
  gives `CancellableEvent(N listeners, last invoke cancelled by <name>)`, `(…, last invoke went ahead)`
  or `(…, not invoked yet)`.

### Functional: C++

- **R18 `CancellableEvent<Args...>`** (`engine/core/util/CancellableEvent.h`): listeners are
  `void(CancelEventArgs&, Args...)`; `subscribe(fn, priority = 0, ownerKey = 0)` returns a
  `Subscription` (RAII, S09); `[[nodiscard]] bool emit(Args...)` returns cancelled. R2, R5, R8, R12,
  R13 and owner removal through `SubscriptionOwnerIndex` hold. Each listener runs as
  `FunctionSubscribe::call` runs it (crash guard while a `CrashHook` is set, a C++ exception logged
  otherwise).
- **R19** `CancelEventArgs::cancel()` during the monitor pass logs `Log::error` once per event object
  (`"<name>: a monitor listener cannot cancel; give it a priority instead"`, `name` given at
  construction) and is ignored.
- **R20 SlotList priority.** `detail::SlotList::add` takes an optional `int32_t priority` (default 0)
  and inserts after the last slot of priority >= it, also when merging `pending`. With every priority
  0 the code path, order and allocations are today's; `sizeof(Slot)` is unchanged (the field sits in
  the padding after `removed`; a `static_assert` pins it). `Signal` and `FunctionSubscribe` keep their
  public API.
- **R21 Engine adoption rule.** No engine event becomes cancellable here (D7). An engine event made
  cancellable later must (a) be a `CancellableEvent`, (b) obey `emit`'s result at every call site,
  (c) ship a test that installs a cancelling listener and asserts the change did not happen, and
  (d) bind its Lua side so listeners get `CancelEventArgs`. `docs/core-utilities.md` states the rule.

### Platform & backends

- **R22** CPU and Lua only, no platform code: identical on Windows, Linux, macOS, Android, iOS and
  HTML5, any graphics backend. Lua use is main-thread (`JASPER_LUA_THREAD_CHECK=abort` covers it); the
  C++ class is externally synchronised like `Signal`.

### Performance

Measured in release on the build server (x86-64), printed by the benches, gated with `perfSlack()`;
mobile and web follow from the ratios (no absolute mobile numbers are gated).

- **R23 Idle Lua invoke.** `CancellableEvent:invoke` with no listener: 0 C++ heap allocations, 0 Lua
  bytes allocated (`lua_gc(LUA_GCCOUNT)` unchanged over 10,000 invokes with the collector stopped), and
  at most 1.2 times `Event:invoke` with no listener.
- **R24 Busy Lua invoke.** With 8 function listeners: 0 C++ heap allocations per invoke for both kinds
  (today one per invoke); 0 Lua bytes from the dispatch itself (`CancelEventArgs` reused, D9);
  `CancellableEvent:invoke` at most 1.15 times `Event:invoke`.
- **R25 Old code.** `Event:invoke` with 0, 1 and 8 listeners at most 1.05 times the base tree's time
  (the bench is first committed and run on the base tree: that run is the baseline and the red control).
- **R26 C++.** `emit` with no listener: no `CancelEventArgs` constructed, 0 allocations, at most 1 ns
  over a plain `bool` test; with 1 listener at most 5 ns over `Signal::invoke`.
- **R27** `SignalBench` and `SignalTest.ZeroAllocInvoke` keep passing unchanged (R20 costs nothing).
- **R28 Memory.** Each Lua listener grows by 4 bytes; a `CancellableEvent` holds at most one cached
  `CancelEventArgs` per nesting depth it reached (created on first use, at most 16 cached; deeper
  nesting makes a temporary one).

### Editor UX (human)

- **R29** Events carry no authored data, so there is no Inspector row and nothing to undo. The human
  surfaces are the code editor's completion and hover text and LuaLS in an external editor, both fed
  by the generated Lua API data: `CancellableEvent`, `CancelEventArgs`, `Event.MONITOR`, the priority
  overloads and `getListeners` appear with their docs; LuaLS flags a dropped `invoke` result (R9).

### Agent UX (MCP, Lua, CLI)

- **R30 No MCP tool change.** Agents inspect through `eval_lua`: `ev:getListeners()` comes back as JSON,
  `ev.cancelledBy` as a string, `tostring(ev)` as R17. The tool budget is unchanged.
- **R31 Docs.** `docs/mcp/guide/lua-api.md` (served as `jasper://docs/lua-api`) gets: priority on each
  form, `CancellableEvent` with the emitter rule ("read `invoke`'s result; when `true`, do not apply the
  change"), monitor listeners ("side effects go in monitors: they run after the decision"), the payload
  pattern (pass a table a listener may change, read it after `invoke`), and the inspection members.
  `docs/unity-glossary.md` gets a row (UnityEvent has no veto: `CancellableEvent`; C# `CancelEventArgs`).
  `docs/core-utilities.md` gets the C++ class and R21.
- **R32 API data.** `tools/lua_api/parser.py` manual entries for every new member beside
  `('Event', ...)`; `stubs.py` emits `---@nodiscard` for an entry with `'nodiscard': True`; `search_api`
  finds `CancellableEvent`, `CancelEventArgs`, `Event.MONITOR` and `getListeners`.
- **R33 Eval.** `tools/mcp/eval/cases.py` gets a case ("make the door script refuse to open while
  another script says it is locked") graded on writing a `CancellableEvent`, a listener calling
  `e:cancel()`, and an emitter that reads `invoke`'s result; it fails on the base tree's docs.

### Data & compatibility

- **R34** Nothing is stored: no scene, asset or project key changes, no version bump, no migration.
- **R35** Every existing Lua call keeps its meaning: no priority means 0, `RegisterEvent` with four
  arguments is unchanged (a number in position 4 still becomes a tag, as today), `tostring` of an
  `Event` is unchanged, `invoke` of an `Event` still returns nothing.
- **R36** C++: `Signal`, `FunctionSubscribe` and `Subscription` stay source compatible; `Slot`'s size and
  layout before `removed` are unchanged.

## 6. Design constraints & decisions

**D1 Per-object events.** Craftsman's bus is one static registry keyed by a fixed enum of game events.
A general engine cannot know the game's events, and a script already owns its `Event` objects, so the
veto lives on the event object. No type registry, no mask: "one test when idle" is the per-object
empty check (`listeners.empty()` in Lua, `!state || size() == 0` in C++).

**D2 A separate class, not `Event{cancellable = true}`.** A distinct `CancellableEvent` gives LuaLS a
type whose `invoke` is `---@nodiscard` (an option flag would leave every `Event:invoke` either marked or
not), makes "only cancellable events can be vetoed" a property of the type, and keeps `Event` exactly as
it is. In C++, `class CancellableScriptEvent : public ScriptEvent` (a constructor flag) bound with
`deriveClass`, so `luabridge::isInstance<ScriptEvent>` in `RegisterEvent` (`LuaBinding.cpp:639`) keeps
working.

**D3 The veto goes through the listener's first argument.** The alternatives were `ev:cancel()` on the
event (a method listener would have to reach the emitter's object, and a listener of two events could
cancel the wrong one) and "return false" (as the physics filters do; `nil` and `false` are easy to
confuse, and only the first subscriber is asked). `e` is per dispatch, so nesting (R12) is exact, and
it matches the engine's other per-dispatch objects (`InputActionEvent&`, `UIEvent`).

**D4 Skip after a veto, then a monitor pass.** Craftsman lets every listener run after a veto so that
observers see it. Here observers are monitor listeners, so the deciding listeners after a veto are
skipped: a later decider (a shield that would absorb the hit, a discount that would apply) does no
work for an action that will not happen. Monitors run once the outcome is final, which is where side
effects belong; the docs say so (R31). A veto is sticky because nothing can clear it.

**D5 Dispatch storage.** `ScriptEvent` keeps its vector of `shared_ptr<Listener>` but drops the
per-invoke copy: a `depth` counter, `removed` flags and a `pending` vector, as SlotList does. Inserting
by priority uses an upper bound on (priority descending) over the live vector at depth 0, and over the
merge of `pending` when the outermost invoke ends. `removeAllListeners` and `release` during an invoke
mark entries; `compact` runs at depth 0. Observable semantics are R13's, pinned by the existing
`ScriptLifecycleTest.EventClass` cases.

**D6 MONITOR.** `Event.MONITOR` is the Lua integer -2147483648 (`INT32_MIN`, outside R1's range); C++
`EventPriority::Monitor` is the same value. A listener is a monitor exactly when its priority equals it.

**D7 No existing engine event becomes cancellable.**

| Engine event family | Why not cancellable |
|---|---|
| Lifecycle, view, focus, visibility, low memory, safe area, scene loaded (`Engine.h:1113-1138`) | Report a change the platform already made; there is no unwind path |
| Keys, mouse, touch, gamepad, characters, action buttons (`Engine.h:1139-1163`) | Input already happened; who sees it first is the UI's consumption and `InputActionEvent::consume()` |
| Physics contacts and activations | Report a step's result; the filters before a contact (`shouldCollide2D/3D`, `preSolve2D`) already veto by return value |
| Component events (buttons, UI pointer, audio, actions, navigation) | Report state changes; HTML UI documents already have `preventDefault` |

Sibling specs that add "before" points (`dialogue-graphs.md` before a choice,
`text-entry-gamepad-cursor-back-button.md` for a back button a menu may claim) may declare them as
`CancellableEvent`s under R21; this spec does not decide their events. `savegame.md` keeps a plain
`onComplete` callback (its own decision).

**D8 C++ on SlotList with RAII subscriptions, not on `FunctionSubscribe`.** S09 makes `Signal` the
event type for new C++ code; `CancellableEvent` follows it (a `Subscription` per listener, owner keys
for C++ scripts), so no tag API exists to leak a callback past Stop, and `check_event_clears.py` needs
no change (it covers `FunctionSubscribe` and its derived classes only; `Signal`-style members hold RAII
subscriptions). `emit` hides `call`-style access: the only way to fire it is the `[[nodiscard]]` path.

**D9 Reusing `CancelEventArgs` in Lua.** A `CancellableEvent` keeps registry references to up to 16
`CancelEventArgs` userdata, one per nesting depth, made on first use. At dispatch start the one for the
current depth is reset (`cancelled = false`, `monitor = false`, a fresh dispatch serial); at the end it
is marked ended, so a stored `e` fails with R10's second error. Pushing it is a `lua_rawgeti`: no
allocation per invoke.

**API sketch (Lua, as the stubs will read):**

```lua
---@class Event
---@field MONITOR integer  # static: a listener at this priority runs after the decision
---@field listenerCount integer
local Event = {}
---@param fn function @param priority? integer  -- also (owner, "method", priority?)
function Event:addListener(fn, priority) end
---@return {name: string, priority: integer, monitor: boolean}[]
function Event:getListeners() end

---@class CancellableEvent : Event
---@field cancelledBy string|nil  # read-only
local CancellableEvent = {}
---Calls the listeners as fn(e, ...); true when one cancelled: then do not apply the change
---@nodiscard
---@return boolean
function CancellableEvent:invoke(...) end

---@class CancelEventArgs
---@field cancelled boolean  # read-only, sticky
local CancelEventArgs = {}
function CancelEventArgs:cancel() end
```

```lua
-- emitter
function Door:init() self.onOpening = CancellableEvent() end
function Door:open(by)
    if self.onOpening:invoke(self, by) then return false end   -- a listener refused
    self.isOpen = true
    return true
end
-- a deciding listener and a monitor
function Lock:onEnable()
    RegisterEvent(self, self.door.onOpening, "check", nil, 10)
    RegisterEvent(self, self.door.onOpening, "report", nil, Event.MONITOR)
end
function Lock:check(e, door, by) if self.locked then e:cancel() end end
function Lock:report(e, door, by) if e.cancelled then Hud.toast("Locked") end end
```

**API sketch (C++):**

```cpp
// engine/core/util/CancellableEvent.h
namespace jasperengine {
    struct EventPriority {
        static constexpr int32_t Min = -1000000, Max = 1000000;
        static constexpr int32_t Monitor = INT32_MIN;
    };

    class JASPERENGINE_API CancelEventArgs {
    public:
        void cancel();                 // in the monitor pass: Log::error once per event, ignored
        bool isCancelled() const { return cancelled; }
        bool isMonitorPass() const { return monitor; }
    private:
        template<typename...> friend class CancellableEvent;
        bool cancelled = false, monitor = false;
        const char* eventName = nullptr;
        bool* monitorErrorLogged = nullptr;
    };

    template<typename... Args>
    class CancellableEvent {
    public:
        using Listener = std::function<void(CancelEventArgs&, Args...)>;
        explicit CancellableEvent(const char* name);
        CancellableEvent(const CancellableEvent&) = delete;
        [[nodiscard]] Subscription subscribe(Listener fn, int32_t priority = 0, uint64_t ownerKey = 0);
        // true: a listener cancelled; the caller must not apply the change
        [[nodiscard]] bool emit(Args... args) {
            if (!state || state->size() == 0) return false;   // the idle path: one test
            return emitSlow(args...);
        }
        bool hasListeners() const;
        size_t size() const;
        size_t removeOwner(uint64_t ownerKey);
        void clear();
    private:
        std::shared_ptr<detail::SlotList<void(CancelEventArgs&, Args...)>> state;
        bool monitorErrorLogged = false;
        const char* name;
        bool emitSlow(Args... args);
    };
}
```

```cpp
// engine/core/util/Signal.h (SlotList): the one signature change
uint64_t add(Function function, uint64_t ownerKey, std::string tag, int32_t priority = 0);
```

## 7. Acceptance criteria

| AC | Req | Verification |
|---|---|---|
| AC1 | R1, R2, R5 | `ScriptEventTest.PriorityOrder` (`engine/tests/core/script/ScriptEventTest.cpp`, new, for `ScriptEvent.cpp`): listeners at 0, 10, -5, 10, MONITOR, 0 log their order as 10, 10 (first added first), 0, 0, -5, MONITOR. Red: on the base tree the priority argument is ignored or rejected and the log is insertion order |
| AC2 | R1, R4 | `ScriptEventTest.PriorityArgumentErrors`: 1.5, "high", 2^40 raise R1's message; `RegisterEvent(self, Engine.onUpdate, "m", nil, 3)` raises R4's message (`EXPECT` on the Lua error text) |
| AC3 | R3, R35 | `ScriptEventTest.ReAddKeepsTodaysRules`: a function added twice with priorities 0 then 9 stays at 0 (one call); a tag re-added with priority 9 moves before the 0s; `RegisterEvent` with a number in position 4 still registers under that tag. Existing `ScriptLifecycleTest.EventClass`, `UnregisterEvents`, `EventListenersDoNotKeepReleasedScriptsAlive` pass unchanged |
| AC4 | R6-R9 | `ScriptEventTest.CancellableVeto`: listeners at 10 (cancels when its argument is 1), 5, MONITOR; `invoke(1)` returns true, the priority-5 listener did not run, the monitor saw `e.cancelled == true`; `invoke(2)` returns false and all three ran; no listener: `invoke()` returns false |
| AC5 | R8 | `ScriptEventTest.VetoIsSticky`: a later monitor reading `e.cancelled` after a cancel always sees true; `CancelEventArgs` has no way to clear it (`e.cancelled = false` raises the read-only error) |
| AC6 | R10 | `ScriptEventTest.CancelErrorsNameTheFix`: cancel in a monitor, cancel on a stored `e` after `invoke` returned, and `Event():cancel()` each log R10's exact text through R11 (`LogCapture`, one error each) and change no result |
| AC7 | R11 | `ScriptEventTest.ListenerErrorKeepsOthersAndCancel`: a listener that cancels then errors: the error is logged with a traceback, the monitor runs, `invoke` returns true |
| AC8 | R12 | `ScriptEventTest.NestedInvokeHasItsOwnArgs`: a listener invoking the same event with an argument its inner listener cancels; the inner `invoke` returns true, the outer returns false and its `e.cancelled` stays false |
| AC9 | R13, R14 | `ScriptEventTest.ReentrancyWithoutSnapshot`: add during invoke runs next time, remove-next is skipped, `removeAllListeners` from the first listener skips the rest, both kinds. `ScriptLifecycleTest.CancellableEventOwnerReleased` (editor): a script that owns a deciding listener is destroyed from another listener mid-dispatch; its listener does not run and the event still returns the right result |
| AC10 | R15-R17 | `ScriptEventTest.InspectionNames`: `getListeners()` of a method listener, a `RegisterEvent` listener, a tagged function and a plain function gives R15's four name forms in dispatch order; `cancelledBy` after a cancel and nil after a clean invoke; `tostring` of `Event()` is `Event(0 listeners)` (unchanged) and of a cancellable event matches R17 |
| AC11 | R23-R25 | `ScriptEventBench.InvokeCosts` (`engine/tests/core/script/ScriptEventBenchTest.cpp`, prints and gates through `perfSlack()`): 0/1/8 listeners, both kinds; `ThreadAllocationCount` 0 per invoke for both kinds with 8 listeners and for idle `CancellableEvent`; Lua `GCCOUNT` unchanged with the collector stopped; ratios R23-R25. Red control: the same bench run on the base tree, recorded in the PR: `Event:invoke` with 8 listeners allocates once per invoke there |
| AC12 | R18, R20, R26, R27, R36 | `CancellableEventTest` (`engine/tests/core/util/CancellableEventTest.cpp`): order, ties, skip-after-veto, monitor pass, nested emit, subscribe/unsubscribe during emit, owner removal, a throwing listener logged with the others still run, `emit` with no listener allocating nothing (`ThreadAllocationCount`). `SignalTest.SlotPriorityDefaultKeepsAppendOrder` and the `static_assert` on `sizeof(Slot)`. `SignalBench.CancellableEmitOverSignal` gates R26; `SignalTest.ZeroAllocInvoke`, `FunctionSubscribeReentrancyTest.*`, `FunctionSubscribeCompatTest.*` pass unchanged |
| AC13 | R19 | `CancellableEventTest.MonitorCancelLogsOnce`: two monitor cancels over two emits log one error naming the event and the fix; results unaffected |
| AC14 | R9, R29, R32 | `test_stubs.py::test_luals_reports_api_mistakes` gains a line `door.onOpening:invoke(1)` (result dropped) mapped to `discard-returns`, and `test_luals_zero_diagnostics_on_known_good_scripts` gains the Door/Lock example above with zero diagnostics. `tools/lua_api/tests/test_lua_api.py` gains a case for the `nodiscard` flag; `python3 -m lua_api doctest` passes on the new `lua-api.md` blocks |
| AC15 | R7, R8, R30 | `test_core_utilities.py::test_cancellable_event_in_play` (MCP, `CI = True`): two scripts (Door, Lock) in Play; `eval_lua` toggles the lock and calls `door:open()`; the Console shows the monitor's markers; `eval_lua "return door.onOpening:getListeners()"` returns the names and priorities as JSON; `cancelledBy` is `"Lock:check"` after a refused open. A hot reload of Lock (`test_script_reload` helpers) keeps the listener's priority (the order in `getListeners()` is unchanged) |
| AC16 | R31, R33 | The eval case of R33 passes with the new docs and fails on the base tree's docs (`tools/mcp/eval/README.md` procedure, Orquester-managed account); `test_docs.py` passes |
| AC17 | R22 | Web gate (`tools/build.sh web`) and the Android build pass with the engine changes; `AC1`-`AC13` run in the Linux CI lanes and the macOS and Windows unit lanes |
| AC18 | R21 | `docs/core-utilities.md` names R21's four conditions; review check at merge (no engine event adopts it in this change, so there is no rig to run) |

## 8. Dependencies & conflicts

- **Depends on:** S09 (`Signal`, `SlotList`, `Subscription`, `SubscriptionOwnerIndex`, the crash guard;
  landed), S23 (Lua hot reload; landed), the Lua API generator (`tools/lua_api`).
- **Sibling specs (consumers, not dependencies):** `dialogue-graphs.md` (its runner events are plain
  `FunctionSubscribe` until it adopts this; a Lua-facing priority or veto there means moving them to
  `CancellableEvent`, since `FunctionSubscribe` gets no Lua priority, R4),
  `text-entry-gamepad-cursor-back-button.md` and `input-device-awareness.md` may declare
  `CancellableEvent`s for their own "before" points under R21. `culling-group-and-simulation-distance.md`
  and `input-recorder-and-input-scripts.md` depend on `FunctionSubscribe` dispatch, which R20 leaves
  unchanged. `savegame.md` needs no priority (its `onComplete` is a plain callback). `dev-console-for-agents.md` may list a
  `CancellableEvent`'s listeners through `getListeners`; nothing here depends on it.
  `project-test-runner.md` may run AC1-AC13 style tests in projects; no coupling.
- **Merge hotspots:** `engine/core/util/Signal.h` (SlotList, shared by every event: keep R20's default
  path byte-identical), `engine/core/script/ScriptEvent.{h,cpp}`,
  `engine/core/script/binding/ScriptClassesLua.cpp`, `engine/core/script/LuaBinding.cpp`
  (`luaRegisterEvent`), `tools/lua_api/parser.py` (the manual table) and `stubs.py`,
  `docs/mcp/guide/lua-api.md`, `docs/core-utilities.md`, `docs/unity-glossary.md`,
  `tools/mcp/tests/test_core_utilities.py`, `tools/mcp/tests/test_stubs.py`, `tools/mcp/eval/cases.py`.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| An emitter in Lua ignores `invoke`'s result anyway (LuaLS is advisory, the built-in editor may not show the diagnostic) | The docs' first sentence on `CancellableEvent` is the rule; `invoke`'s doc says it; AC14 makes LuaLS report it. A runtime check is impossible (Lua cannot see a dropped result) |
| Skipping deciders after a veto surprises someone porting Craftsman/Bukkit-style code where every listener runs | Documented with the reason (D4); a listener that must see every outcome is a monitor |
| A listener added twice with a new priority keeps the old one (R3) | Today's duplicate rule wins; documented "remove first"; changing it would alter existing scripts' behaviour |
| `RegisterEvent`'s priority is a fifth positional argument with `nil` for the tag | Keeps the four-argument form exactly (a number there is a tag today, R35); the docs show `RegisterEvent(self, ev, "m", nil, 10)` |
| Hot reload: `RegisterEvent` on an `Event` is not in the reload record (`LuaBinding.cpp:638-645`), so its closure keeps the old method after a reload | Out of scope (existing behaviour); the priority lives on the listener entry, so a listener keeps it across a reload. Recorded here so the S23 owner can decide |
| D5's rewrite of `ScriptEvent` dispatch changes code every Lua `Event` uses | The existing `ScriptLifecycleTest` Event cases are the contract and run first; AC11's base-tree bench is the before/after evidence; behaviour is otherwise unchanged |
| SlotList change touches every engine event | R20's default path is the old code path; `static_assert` on the slot size; all `Signal*`/`FunctionSubscribe*` tests and the bench run unchanged (AC12) |
| 16 cached `CancelEventArgs` are too few for deep recursion | Deeper levels make a temporary userdata (allocates); 16 nested invokes of one event is far past any real use |
| Should engine events be cancellable now (a "wants to quit" hook) | No (D7): a quit request exists only on desktop windows and the web's `beforeunload` cannot wait; a later spec can add it under R21 with its platform answers |

## Deviations

- **D9 has no dispatch serial.** The reused `CancelEventArgs` is marked ended when its invoke returns,
  so a stored `e` fails with R10's second error between invokes; used inside a later invoke at the
  same nesting depth it is that invoke's own (the same userdata), so a serial could not tell them
  apart and is left out.
- **`ev:cancel()` on a `CancellableEvent`** also raises, naming `e:cancel()` (R10 only names the plain
  `Event` case; `cancel` is one method of both classes).
- **C++ priorities are not range-checked**: R1's range is the Lua argument's; `subscribe` documents
  `EventPriority::Min..Max` or `Monitor`.
- **A `CancellableEvent` destroyed by its own listener** (as `Signal` allows) keeps its emit valid: the
  live `CancelEventArgs` take over the monitor-error flag (`orphaned`), so R19's "once per event"
  becomes once per pending emit for that case. The crash guard reports a failing listener as
  `<name> listener` (the event has no tags).
- **A nested invoke** goes over the listeners there were when the outermost invoke started, as
  `SlotList` does: one added by a listener waits until the outermost invoke ends (with the snapshot
  copy, an invoke nested after the add saw it). Every other R13 rule is unchanged.
- **`CancellableEvent:invoke`** is bound again on the derived class so the Lua API data has its own
  `---@nodiscard` entry; it checks for the exact class, which skips LuaBridge's walk up to `Event`'s
  (the dispatch is the same and reads the event's kind). `__tostring` is bound again too: LuaBridge
  does not inherit metamethods.
