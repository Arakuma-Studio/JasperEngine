// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Coroutines and timers of scripts (spec docs/superpowers/specs/2026-10-31-coroutines-design.md):
// engine-driven Lua coroutines (`self:startCoroutine`, `Coroutine.wait` ...), invoke-style timers
// for Lua (`self:invoke`, `Timer.after`) and C++ (`ScriptBase::invoke`, `ScriptScheduler::after`),
// all run on the main thread at fixed points of Engine::runFrame:
//
//   fixed step: onFixedUpdate, scenes' fixedUpdate (physics), fixedPhase()      (waitForFixedUpdate)
//   onUpdate, updatePhase() (timers, then waits for seconds/frames/predicates/handles), scenes update,
//   onPostUpdate ... draw, commit, endOfFramePhase()                           (waitForEndOfFrame)
//
// Ownership: an owner is a script instance (a Lua instance: its registry ref as a pointer, the
// ScriptEntry::instance; a C++ script: its ScriptBase*), or none. Deactivating the owner's
// GameObject stops its coroutines (not its timers); releasing the owner (destroy, scene unload,
// Play stop) ends both. Unowned work ends at clear() (Play stop, shutdown).

#ifndef SCRIPTSCHEDULER_H
#define SCRIPTSCHEDULER_H

#include "Export.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine {

    class Scene;
    class EntityRegistry;

    // The clock of a timer or a wait: game seconds (times Time.timeScale) or unscaled seconds
    // (the frame time before timeScale; it stops while the app or the editor's Play is paused)
    enum class TimeMode : uint8_t { Scaled, Unscaled };

    // A coroutine or timer: monotonically increasing, never reused; 0 = none
    using TaskId = uint64_t;

    // What get_runtime_entity and the Inspector show of a coroutine
    struct ScriptCoroutineInfo {
        TaskId id = 0;
        std::string status;     // "running", "suspended"
        std::string waiting;    // "seconds 1.25 left", "frames 1", "until", "for Coroutine 7", "end of frame", ...
        std::string origin;     // where it was started: "Spawner.lua:12"
        std::string method;     // the method name when started by name
    };

    // ... and of a timer
    struct ScriptTimerInfo {
        TaskId id = 0;
        double remaining = 0.0; // seconds to the next call
        double interval = 0.0;  // 0 for a one-shot
        bool unscaled = false;
        std::string callback;   // the method name, "function" or "C++"
    };

    class JASPERENGINE_API ScriptScheduler {
    public:
        // ---- frame phases (Engine::runFrame; only while the game events run)
        static void beginFrame(double scaledDelta, double unscaledDelta);
        static void beginFixedStep();
        static void fixedPhase();
        static void updatePhase();
        static void endOfFramePhase();

        // ---- ownership
        // Deactivation: the owner's coroutines stop (its timers continue)
        static void stopCoroutinesOf(const void* owner);
        // The owner instance is released (destroy, scene unload, Play stop): its coroutines and
        // timers end. LuaBinding::releaseLuaScriptInstance and ~ScriptBase call it.
        static void cancelOwner(const void* owner);
        // A GameObject of `registry` was deactivated (or moved under an inactive parent): the
        // coroutines of owners no longer active in hierarchy there stop (EntityRegistry calls it)
        static void stopCoroutinesOfInactive(const EntityRegistry* registry);
        // The owner's GameObject moved (persistence: another scene or entity id)
        static void rebind(const void* owner, Scene* scene, Entity entity);
        // Play stop, shutdown: every coroutine and timer ends (pending to-be-closed variables run)
        static void clear();

        // ---- Lua coroutines from C++ (script-lifecycle: `start` runs as a coroutine)
        // Starts fn(args) as an engine coroutine owned by the script instance at `instanceIndex`
        // of L (nil/none: unowned): the function at `fnIndex` and the `nargs` values after it,
        // which stay on L. It runs now until its first wait; an error is logged like any coroutine
        // error (no Lua error raised). Returns its id (0 when the instance is not a live one).
        static TaskId startCoroutine(lua_State* L, int instanceIndex, int fnIndex, int nargs, const char* methodName = nullptr);

        // ---- C++ timers (callbacks run in the update phase; exceptions are not caught)
        // One call `delay` seconds from now (at the earliest the next frame)
        static TaskId after(double delay, std::function<void()> fn, TimeMode mode = TimeMode::Scaled, const void* owner = nullptr);
        // First call after `delay`, then every `interval` (> 0) seconds; `count` calls (-1: until cancelled).
        // At most one call a frame; a late one is rescheduled from now (no catch-up burst).
        static TaskId every(double delay, double interval, std::function<void()> fn, TimeMode mode = TimeMode::Scaled,
                            const void* owner = nullptr, int count = -1);
        // Cancels a pending timer; false when it is unknown or already done
        static bool cancel(TaskId id);
        // The timer will still be called
        static bool isPending(TaskId id);
        // The owner of a timer (nullptr when unowned or unknown)
        static const void* timerOwner(TaskId id);
        // The pending timers of `owner` (0 when it has none): cancelled / counted
        static size_t cancelTimersOf(const void* owner);

        // ---- stats (MCP get_play_state, the Inspector, the debug overlay)
        static size_t coroutineCount();     // running or suspended
        static size_t timerCount();         // pending
        static size_t coroutineCountOf(const void* owner);
        static size_t timerCountOf(const void* owner);
        static std::vector<ScriptCoroutineInfo> coroutinesOf(const void* owner);
        static std::vector<ScriptTimerInfo> timersOf(const void* owner);
        // Lua registry references the scheduler holds now (tests: nothing leaks)
        static size_t luaRefCount();
    };

    // The Lua `Coroutine` handle (userdata): what a coroutine is doing, and stopping it. Engine
    // internal; Lua gets it from self:startCoroutine / Coroutine.start / Coroutine.running.
    class JASPERENGINE_API ScriptCoroutine {
    private:
        TaskId id = 0;
    public:
        explicit ScriptCoroutine(TaskId id);
        ScriptCoroutine(const ScriptCoroutine& other);
        ScriptCoroutine& operator=(const ScriptCoroutine& other);
        ~ScriptCoroutine();

        // the coroutine's id (get_runtime_entity lists it)
        TaskId getId() const;
        // "running" (its code runs now), "suspended" (waiting), "finished", "stopped" or "error"
        std::string getStatus() const;
        // It ended: finished, stopped or failed
        bool isDone() const;
        // Stops it: a waiting coroutine at once (its to-be-closed variables close), a running one
        // (it stopped itself, or it started the running one) at its next wait
        void stop();
        // "Coroutine(12, suspended, Spawner.lua:12)"
        std::string toString() const;
    };

    // The Lua `Timer` handle (userdata) of self:invoke / Timer.after / Timer.every
    class JASPERENGINE_API ScriptTimer {
    private:
        TaskId id = 0;
    public:
        explicit ScriptTimer(TaskId id);
        ScriptTimer(const ScriptTimer& other);
        ScriptTimer& operator=(const ScriptTimer& other);
        ~ScriptTimer();

        // the timer's id
        TaskId getId() const;
        // Cancels it (no further call); nothing when it is done
        void cancel();
        // It will not be called again: a one-shot that fired, the last call of a counted timer, or cancelled
        bool isDone() const;
        // Seconds to the next call (0 when done)
        double getRemaining() const;
        // Seconds between calls (0 for a one-shot)
        double getInterval() const;
        // It counts unscaled seconds (keeps running at Time.timeScale = 0)
        bool isUnscaled() const;
        // "Timer(5, 0.75 s left, every 0.5 s)"
        std::string toString() const;
    };

    // The Lua side (engine internal): lua_CFunctions bound by binding/CoroutineClassesLua.cpp
    // (Coroutine.*, Timer.*) and installed on every script instance (LuaBinding::pushScriptInstanceBase)
    class JASPERENGINE_API ScriptSchedulerLua {
    public:
        // Coroutine.*
        static int start(lua_State* L);
        static int running(lua_State* L);
        static int isInside(lua_State* L);
        static int wait(lua_State* L);
        static int waitRealtime(lua_State* L);
        static int nextFrame(lua_State* L);
        static int waitFrames(lua_State* L);
        static int waitForFixedUpdate(lua_State* L);
        static int waitForEndOfFrame(lua_State* L);
        static int waitUntil(lua_State* L);
        static int waitWhile(lua_State* L);
        static int waitFor(lua_State* L);
        // Timer.*
        static int timerAfter(lua_State* L);
        static int timerEvery(lua_State* L);
        // self:* (script instances)
        static int startCoroutine(lua_State* L);
        static int stopCoroutine(lua_State* L);
        static int stopAllCoroutines(lua_State* L);
        static int invoke(lua_State* L);
        static int invokeRepeating(lua_State* L);
        static int cancelInvoke(lua_State* L);
        static int isInvoking(lua_State* L);
        // handle properties that are Lua values
        static int coroutineError(lua_State* L, TaskId id);
        static int coroutineOwner(lua_State* L, TaskId id);
        // adds the self:* methods to a ScriptInstanceBase table at `index`
        static void addInstanceMethods(lua_State* L, int index);
    };

}

#endif //SCRIPTSCHEDULER_H
