// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "util/Angle.h"
#include "util/Base64.h"
#include "util/Color.h"
#include "util/CrashHook.h"
#include "util/Scheduler.h"
#include "script/ScriptScheduler.h"
#include "LuaReload.h"
#include "LuaWatchdog.h"
#include "util/SubscriptionOwnerIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

using namespace jasperengine;

// ------------------------------------------------------------ Timer
//
// Timer.after / every / nextFrame / afterFrames / afterFixedSteps call a function on the
// engine's scheduler (Scheduler::game(): game, unscaled or real clock); Timer.run drives a
// coroutine that can Timer.wait*. Callbacks run like events: an error goes to the crash hook
// (the editor pauses Play) or the log, and cancels the timer or coroutine. Without an owner a
// timer lives until Play stops; { owner = self } ends it with the script.

namespace je_lua_timer {}
using namespace je_lua_timer;

namespace je_lua_timer {

    const char* const TIMER_HANDLE_META = "JasperEngine.TimerHandle";
    const char* const TIMER_COROUTINE_META = "JasperEngine.TimerCoroutine";

    // A registry reference released with the task that holds it (while the Lua state lives)
    struct LuaRef {
        int ref = LUA_NOREF;

        explicit LuaRef(int ref): ref(ref) {}
        LuaRef(LuaRef&& other) noexcept: ref(std::exchange(other.ref, LUA_NOREF)) {}
        LuaRef& operator=(LuaRef&& other) noexcept {
            if (this != &other) {
                release();
                ref = std::exchange(other.ref, LUA_NOREF);
            }
            return *this;
        }
        LuaRef(const LuaRef&) = delete;
        LuaRef& operator=(const LuaRef&) = delete;
        ~LuaRef() { release(); }

        void release() {
            if (ref != LUA_NOREF && ref != LUA_REFNIL) {
                if (lua_State* L = LuaBinding::getLuaState()) luaL_unref(L, LUA_REGISTRYINDEX, ref);
            }
            ref = LUA_NOREF;
        }
    };

    struct TimerOptions {
        TimeDomain clock = TimeDomain::Game;
        uint32_t count = 0;
        bool catchUp = false;
        uint64_t owner = 0;
        // Timer.every: the first call's delay (upstream's opts.delay); negative: the interval
        double delay = -1.0;
    };

    // a live script instance (upstream's coroutines' rule for opts.owner): the table the
    // instance registry holds under its __scriptRef
    bool isScriptInstance(lua_State* L, int index) {
        index = lua_absindex(L, index);
        if (!lua_istable(L, index)) return false;
        lua_pushliteral(L, "__scriptRef");
        lua_rawget(L, index);
        const int ref = lua_isinteger(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
        lua_pop(L, 1);
        if (ref <= 0) return false;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        const bool same = lua_rawequal(L, -1, index) != 0;
        lua_pop(L, 1);
        return same;
    }

    // The options table at index (absent or nil: defaults). One API for both families of
    // scripts: oe-port's {owner, clock, count, catchUp} and upstream's {owner, unscaled, count,
    // delay} (delay: Timer.every only)
    TimerOptions readOptions(lua_State* L, int index, const char* function = "Timer", bool every = false) {
        if (LuaBinding::isPreviewContext())
            luaL_error(L, "Timer/Coroutine scheduling is unavailable in UI frame previews: use anime.createTimer/anime.createTimeline or record_game");
        TimerOptions options;
        if (lua_isnoneornil(L, index)) return options;
        if (!lua_istable(L, index)) {
            luaL_error(L, "%s: opts must be a table ({owner, clock, unscaled, count, delay, catchUp}), got %s", function,
                       luaL_typename(L, index));
        }
        lua_getfield(L, index, "owner");
        if (!lua_isnil(L, -1)) {
            if (!isScriptInstance(L, -1)) luaL_error(L, "%s: opts.owner must be a live script instance (self)", function);
            options.owner = ownerKeyOf(lua_topointer(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "unscaled");
        if (lua_toboolean(L, -1)) options.clock = TimeDomain::Unscaled;
        lua_pop(L, 1);
        lua_getfield(L, index, "clock");
        if (!lua_isnil(L, -1)) {
            // no C++ objects alive across luaL_error
            const char* clock = luaL_checkstring(L, -1);
            if (std::strcmp(clock, "game") == 0) options.clock = TimeDomain::Game;
            else if (std::strcmp(clock, "unscaled") == 0) options.clock = TimeDomain::Unscaled;
            else if (std::strcmp(clock, "real") == 0) options.clock = TimeDomain::Real;
            else luaL_error(L, "%s: clock must be \"game\", \"unscaled\" or \"real\" (got \"%s\")", function, clock);
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "count");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1)
                luaL_error(L, "%s: opts.count must be an integer of 1 or more", function);
            options.count = static_cast<uint32_t>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "catchUp");
        options.catchUp = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, index, "delay");
        if (!lua_isnil(L, -1)) {
            if (!every) luaL_error(L, "%s: opts.delay is for Timer.every (the first argument is the delay)", function);
            if (lua_type(L, -1) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, -1)) || lua_tonumber(L, -1) < 0.0)
                luaL_error(L, "%s: opts.delay must be a number of seconds (0 or more)", function);
            options.delay = lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
        return options;
    }

    TimerOpts schedulerOptions(const TimerOptions& options, const char* name) {
        TimerOpts opts;
        opts.count = options.count;
        opts.catchUp = options.catchUp;
        opts.ownerKey = options.owner;
        opts.debugName = name;
        return opts;
    }

    void reportError(const char* name, const std::string& error) {
        if (LuaWatchdog::takeInterrupt()) {
            LuaBinding::logLuaError("Script crash: %s: %s", name, error.c_str());
        } else if (!CrashHook::report(name, error)) {
            LuaBinding::logLuaError("Lua Error: %s: %s", name, error.c_str());
        }
    }

    // What a Lua timer handle reads (upstream's Timer object: id, remaining, interval, unscaled,
    // isDone; oe-port's handle: cancel, isActive). `current` is the scheduler task now (a
    // Timer.every with its own first delay is a one-shot, then the repeat)
    struct TimerState {
        uint64_t id = 0;
        uint64_t current = 0;
        double interval = 0.0;     // 0: once
        TimeDomain clock = TimeDomain::Game;
        uint32_t count = 0;        // runs in all (0: until cancelled; a one-shot: 1)
        uint32_t runs = 0;
        bool cancelled = false;
        bool finished = false;     // its last call ran, or the callback returned false

        TimerHandle handle() const { return TimerHandle{&Scheduler::game(), current}; }
        bool active() const { return !cancelled && !finished && Scheduler::game().isActive(handle()); }
    };
    using TimerStatePtr = std::shared_ptr<TimerState>;

    // the userdata holds the state (a shared_ptr, destroyed by __gc)
    void pushHandle(lua_State* L, const TimerStatePtr& state) {
        void* memory = lua_newuserdatauv(L, sizeof(TimerStatePtr), 0);
        new (memory) TimerStatePtr(state);
        luaL_setmetatable(L, TIMER_HANDLE_META);
    }

    TimerStatePtr checkHandle(lua_State* L, int index) {
        return *static_cast<TimerStatePtr*>(luaL_checkudata(L, index, TIMER_HANDLE_META));
    }

    // A timer's function: called with its handle (upstream's `function(t)`); returning false
    // stops a repeating one; an error cancels the timer (a repeating one stops)
    struct TimerCallData {
        LuaRef function{LUA_NOREF};
        const char* name = "Timer";
        // the chunk generation of the function: a hot reload of its script ends the timer
        std::shared_ptr<lua_reload::CallbackRecord> record;
        TimerStatePtr state;
    };

    struct TimerCall {
        std::shared_ptr<TimerCallData> data;

        void operator()() const {
            lua_State* L = LuaBinding::getLuaState();
            TimerState& state = *data->state;
            if (!L || state.cancelled || state.finished) {
                Scheduler::game().current().cancel();
                return;
            }
            if (data->record && data->record->dropped()) {
                state.finished = true;
                Scheduler::game().current().cancel();
                return;
            }
            state.runs++;
            const bool last = state.count > 0 && state.runs >= state.count;
            if (last) state.finished = true;
            lua_rawgeti(L, LUA_REGISTRYINDEX, data->function.ref);
            pushHandle(L, data->state);
            if (LuaBinding::pcallWithTraceback(L, 1, 1) != LUA_OK) {
                const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                lua_pop(L, 1);
                state.finished = true;
                Scheduler::game().current().cancel();
                reportError(data->name, error);
                return;
            }
            const bool stop = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (stop) {
                state.finished = true;
                Scheduler::game().current().cancel();
            }
        }
    };

    std::shared_ptr<lua_reload::CallbackRecord> functionRecord(lua_State* L, int index) {
        lua_pushvalue(L, index);
        std::shared_ptr<lua_reload::CallbackRecord> record = lua_reload::callbackRecordForFunction(L);
        lua_pop(L, 1);
        return record;
    }

    int functionRef(lua_State* L, int index, const char* function) {
        if (!lua_isfunction(L, index)) luaL_error(L, "%s: fn must be a function, got %s", function, luaL_typename(L, index));
        lua_pushvalue(L, index);
        return luaL_ref(L, LUA_REGISTRYINDEX);
    }

    TimerCall makeCall(lua_State* L, int fnIndex, const char* name, const TimerStatePtr& state) {
        TimerCall call;
        call.data = std::make_shared<TimerCallData>();
        call.data->function = LuaRef(functionRef(L, fnIndex, name));
        call.data->name = name;
        call.data->record = functionRecord(L, fnIndex);
        call.data->state = state;
        return call;
    }

    double secondsArg(lua_State* L, int index, const char* function, const char* what) {
        if (lua_type(L, index) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, index)))
            luaL_error(L, "%s: %s must be a number of seconds, got %s", function, what, luaL_typename(L, index));
        return lua_tonumber(L, index);
    }

    int luaTimerAfter(lua_State* L) {
        const double delay = secondsArg(L, 1, "Timer.after", "delay");
        const TimerOptions options = readOptions(L, 3, "Timer.after");
        auto state = std::make_shared<TimerState>();
        state->clock = options.clock;
        state->count = 1;
        TimerCall call = makeCall(L, 2, "Timer.after", state);
        TimerOptions once = options;
        once.count = 0;
        const TimerHandle handle = Scheduler::game().after(delay, call, options.clock, schedulerOptions(once, "Timer.after"));
        state->id = state->current = handle.id;
        pushHandle(L, state);
        return 1;
    }

    int luaTimerEvery(lua_State* L) {
        const double period = secondsArg(L, 1, "Timer.every", "interval");
        if (!(period > 0.0)) return luaL_error(L, "Timer.every: interval must be > 0, got %f", period);
        const TimerOptions options = readOptions(L, 3, "Timer.every", true);
        auto state = std::make_shared<TimerState>();
        state->clock = options.clock;
        state->interval = period;
        state->count = options.count;
        TimerCall call = makeCall(L, 2, "Timer.every", state);
        TimerHandle handle;
        if (options.delay >= 0.0 && options.delay != period) {
            // upstream's opts.delay: the first call after `delay`, then every `period`
            TimerOptions first = options;
            first.count = 0;
            handle = Scheduler::game().after(options.delay, [call, state, options, period]() {
                call();
                if (state->cancelled || state->finished) return;
                TimerOptions rest = options;
                if (rest.count > 0) rest.count -= state->runs;
                const TimerHandle repeat = Scheduler::game().every(period, call, options.clock, schedulerOptions(rest, "Timer.every"));
                state->current = repeat.id;
            }, options.clock, schedulerOptions(first, "Timer.every"));
        } else {
            handle = Scheduler::game().every(period, call, options.clock, schedulerOptions(options, "Timer.every"));
        }
        state->id = state->current = handle.id;
        pushHandle(L, state);
        return 1;
    }

    // the frame and fixed-step timers (once)
    template <typename Schedule>
    int scheduleOnce(lua_State* L, int fnIndex, int optsIndex, const char* name, Schedule&& schedule) {
        const TimerOptions options = readOptions(L, optsIndex, name);
        auto state = std::make_shared<TimerState>();
        state->count = 1;
        TimerCall call = makeCall(L, fnIndex, name, state);
        const TimerHandle handle = schedule(call, schedulerOptions(options, name));
        state->id = state->current = handle.id;
        pushHandle(L, state);
        return 1;
    }

    int luaTimerNextFrame(lua_State* L) {
        return scheduleOnce(L, 1, 2, "Timer.nextFrame", [](const TimerCall& call, TimerOpts opts) {
            return Scheduler::game().nextFrame(call, opts);
        });
    }

    int luaTimerAfterFrames(lua_State* L) {
        const lua_Integer frames = luaL_checkinteger(L, 1);
        if (frames < 1) return luaL_error(L, "Timer.afterFrames: frames must be 1 or more");
        return scheduleOnce(L, 2, 3, "Timer.afterFrames", [frames](const TimerCall& call, TimerOpts opts) {
            return Scheduler::game().afterFrames(static_cast<uint32_t>(frames), call, opts);
        });
    }

    int luaTimerAfterFixedSteps(lua_State* L) {
        const lua_Integer steps = luaL_checkinteger(L, 1);
        if (steps < 1) return luaL_error(L, "Timer.afterFixedSteps: steps must be 1 or more");
        return scheduleOnce(L, 2, 3, "Timer.afterFixedSteps", [steps](const TimerCall& call, TimerOpts opts) {
            return Scheduler::game().afterFixedSteps(static_cast<uint32_t>(steps), call, opts);
        });
    }

    int luaTimerCancelAll(lua_State* L) {
        luaL_checkany(L, 1);
        lua_pushinteger(L, static_cast<lua_Integer>(Scheduler::game().cancelOwner(ownerKeyOf(lua_topointer(L, 1)))));
        return 1;
    }

    int luaHandleCancel(lua_State* L) {
        TimerStatePtr state = checkHandle(L, 1);
        const bool was = state->active();
        if (was) {
            state->cancelled = true;
            state->handle().cancel();
        }
        lua_pushboolean(L, was);
        return 1;
    }

    int luaHandleIsActive(lua_State* L) {
        lua_pushboolean(L, checkHandle(L, 1)->active());
        return 1;
    }

    int luaHandleIsDone(lua_State* L) {
        lua_pushboolean(L, !checkHandle(L, 1)->active());
        return 1;
    }

    double remainingOf(const TimerState& state) {
        TimerInfo info;
        if (!state.active() || !Scheduler::game().info(state.handle(), info)) return 0.0;
        return std::max(0.0, info.remaining);
    }

    std::string formatSeconds(double seconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", seconds);
        return text;
    }

    // "Timer(5, 0.75 s left, every 0.50 s)", "Timer(5, cancelled)", "Timer(5, done)"
    int luaHandleToString(lua_State* L) {
        TimerStatePtr state = checkHandle(L, 1);
        std::string text = "Timer(" + std::to_string(state->id) + ", ";
        if (state->active()) text += formatSeconds(remainingOf(*state)) + " s left";
        else text += state->cancelled ? "cancelled" : "done";
        if (state->interval > 0.0) text += ", every " + formatSeconds(state->interval) + " s";
        text += ")";
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // methods (cancel, isActive, isDone) and upstream's read-only fields (id, remaining,
    // interval, unscaled)
    int luaHandleIndex(lua_State* L) {
        TimerStatePtr state = checkHandle(L, 1);
        const char* key = lua_tostring(L, 2);
        if (!key) return 0;
        if (std::strcmp(key, "cancel") == 0) lua_pushcfunction(L, luaHandleCancel);
        else if (std::strcmp(key, "isActive") == 0) lua_pushcfunction(L, luaHandleIsActive);
        else if (std::strcmp(key, "isDone") == 0) lua_pushcfunction(L, luaHandleIsDone);
        else if (std::strcmp(key, "id") == 0) lua_pushinteger(L, static_cast<lua_Integer>(state->id));
        else if (std::strcmp(key, "remaining") == 0) lua_pushnumber(L, remainingOf(*state));
        else if (std::strcmp(key, "interval") == 0) lua_pushnumber(L, state->interval);
        else if (std::strcmp(key, "unscaled") == 0) lua_pushboolean(L, state->clock == TimeDomain::Unscaled);
        else lua_pushnil(L);
        return 1;
    }

    int luaHandleGc(lua_State* L) {
        auto* state = static_cast<TimerStatePtr*>(luaL_checkudata(L, 1, TIMER_HANDLE_META));
        state->~TimerStatePtr();
        return 0;
    }

    // Coroutine.waitFor(handle) (ScriptScheduler::setTimerAwaitable)
    bool awaitableIs(lua_State* L, int index) {
        return luaL_testudata(L, index, TIMER_HANDLE_META) != nullptr;
    }

    int awaitableStatus(lua_State* L, int index) {
        auto* state = static_cast<TimerStatePtr*>(luaL_testudata(L, index, TIMER_HANDLE_META));
        if (!state) return 0;
        const TimerState& timer = **state;
        if (timer.active()) return (timer.interval > 0.0 && timer.count == 0) ? -2 : -1;
        return timer.cancelled ? 0 : 1;
    }

    int luaHandleEq(lua_State* L) {
        TimerStatePtr a = checkHandle(L, 1);
        TimerStatePtr b = checkHandle(L, 2);
        lua_pushboolean(L, a == b);
        return 1;
    }

    // ---- coroutines

    enum class WaitKind { None, Seconds, Frames, FixedStep, Until };

    struct Coroutine {
        uint64_t id = 0;
        LuaRef thread{LUA_NOREF};
        lua_State* co = nullptr;
        TimerOptions options;
        WaitKind wait = WaitKind::None;
        double seconds = 0.0;
        TimeDomain waitClock = TimeDomain::Game;
        uint32_t frames = 0;
        LuaRef predicate{LUA_NOREF};
        TimerHandle pending;
        bool cancelled = false;
        bool finished = false;
        std::shared_ptr<lua_reload::CallbackRecord> record;
    };
    using CoroutinePtr = std::shared_ptr<Coroutine>;

    // weak: a coroutine lives as long as the task that resumes it next (or the resume running)
    std::unordered_map<lua_State*, std::weak_ptr<Coroutine>> coroutinesByThread;
    std::unordered_map<uint64_t, std::weak_ptr<Coroutine>> coroutinesById;
    uint64_t nextCoroutineId = 1;

    void finish(const CoroutinePtr& coroutine) {
        coroutine->finished = true;
        coroutinesByThread.erase(coroutine->co);
        coroutinesById.erase(coroutine->id);
    }

    void resume(const CoroutinePtr& coroutine);

    void scheduleContinuation(const CoroutinePtr& coroutine) {
        Scheduler& scheduler = Scheduler::game();
        const TimerOpts opts = schedulerOptions(coroutine->options, "Timer.run");
        auto next = [coroutine]() { resume(coroutine); };
        switch (coroutine->wait) {
            case WaitKind::Seconds:
                coroutine->pending = scheduler.after(coroutine->seconds, next, coroutine->waitClock, opts);
                break;
            case WaitKind::Frames:
                coroutine->pending = scheduler.afterFrames(coroutine->frames, next, opts);
                break;
            case WaitKind::FixedStep:
                coroutine->pending = scheduler.afterFixedSteps(1, next, opts);
                break;
            case WaitKind::Until:
                // polled once per frame, the coroutine resumes on the first true
                coroutine->pending = scheduler.nextFrame([coroutine]() {
                    lua_State* L = LuaBinding::getLuaState();
                    if (!L || coroutine->cancelled) return;
                    lua_rawgeti(L, LUA_REGISTRYINDEX, coroutine->predicate.ref);
                    if (LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK) {
                        const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                        lua_pop(L, 1);
                        finish(coroutine);
                        reportError("Timer.waitUntil", error);
                        return;
                    }
                    const bool done = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);
                    if (done) {
                        coroutine->predicate.release();
                        resume(coroutine);
                    } else {
                        scheduleContinuation(coroutine);
                    }
                }, opts);
                break;
            case WaitKind::None:
                finish(coroutine);
                break;
        }
    }

    void resume(const CoroutinePtr& coroutine) {
        lua_State* L = LuaBinding::getLuaState();
        if (!L || coroutine->cancelled || coroutine->finished) return;
        if (coroutine->record && coroutine->record->dropped()) {
            // its script was hot reloaded: the old code's coroutine ends
            finish(coroutine);
            return;
        }
        coroutine->wait = WaitKind::None;
        int results = 0;
        const int status = lua_resume(coroutine->co, L, 0, &results);
        if (status == LUA_YIELD) {
            lua_pop(coroutine->co, results);
            if (coroutine->cancelled) {
                finish(coroutine);
                return;
            }
            scheduleContinuation(coroutine);
            return;
        }
        if (status != LUA_OK) {
            const char* message = lua_tostring(coroutine->co, -1);
            luaL_traceback(L, coroutine->co, message ? message : "(error object is not a string)", 0);
            const std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            finish(coroutine);
            reportError("Timer.run", error);
            return;
        }
        finish(coroutine);
    }

    struct CoroutineUserdata {
        uint64_t id;
    };

    CoroutinePtr checkCoroutine(lua_State* L, int index) {
        auto* data = static_cast<CoroutineUserdata*>(luaL_checkudata(L, index, TIMER_COROUTINE_META));
        auto it = coroutinesById.find(data->id);
        return it != coroutinesById.end() ? it->second.lock() : nullptr;
    }

    int luaTimerRun(lua_State* L) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        const TimerOptions options = readOptions(L, 2);
        auto coroutine = std::make_shared<Coroutine>();
        coroutine->options = options;
        coroutine->id = nextCoroutineId++;
        coroutine->record = functionRecord(L, 1);
        coroutine->co = lua_newthread(L);
        lua_pushvalue(L, 1);
        lua_xmove(L, coroutine->co, 1);
        coroutine->thread = LuaRef(luaL_ref(L, LUA_REGISTRYINDEX));
        coroutinesByThread[coroutine->co] = coroutine;
        coroutinesById[coroutine->id] = coroutine;

        auto* data = static_cast<CoroutineUserdata*>(lua_newuserdatauv(L, sizeof(CoroutineUserdata), 0));
        data->id = coroutine->id;
        luaL_setmetatable(L, TIMER_COROUTINE_META);
        // runs at once, up to its first wait
        resume(coroutine);
        return 1;
    }

    // the coroutine of Timer.run this Lua thread is, or a Lua error
    CoroutinePtr runningCoroutine(lua_State* L, const char* function) {
        auto it = coroutinesByThread.find(L);
        CoroutinePtr coroutine = it != coroutinesByThread.end() ? it->second.lock() : nullptr;
        if (!coroutine) {
            luaL_error(L, "%s must be called inside Timer.run", function);
        }
        return coroutine;
    }

    int yieldFor(lua_State* L, Coroutine& coroutine, WaitKind kind) {
        coroutine.wait = kind;
        return lua_yield(L, 0);
    }

    int luaTimerWait(lua_State* L) {
        if (LuaBinding::isPreviewContext())
            return luaL_error(L, "Timer/Coroutine waits are unavailable in UI frame previews: use anime.createTimer/anime.createTimeline or record_game");
        const double seconds = luaL_checknumber(L, 1);
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.wait");
        coroutine->seconds = seconds;
        coroutine->waitClock = coroutine->options.clock;
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Seconds);
    }

    int luaTimerWaitRealtime(lua_State* L) {
        if (LuaBinding::isPreviewContext())
            return luaL_error(L, "Timer/Coroutine waits are unavailable in UI frame previews: use anime.createTimer/anime.createTimeline or record_game");
        const double seconds = luaL_checknumber(L, 1);
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitRealtime");
        coroutine->seconds = seconds;
        coroutine->waitClock = TimeDomain::Real;
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Seconds);
    }

    int luaTimerWaitFrames(lua_State* L) {
        if (LuaBinding::isPreviewContext())
            return luaL_error(L, "Timer/Coroutine waits are unavailable in UI frame previews: use anime.createTimer/anime.createTimeline or record_game");
        const lua_Integer frames = luaL_optinteger(L, 1, 1);
        if (frames < 1) return luaL_error(L, "Timer.waitFrames: frames must be 1 or more");
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitFrames");
        coroutine->frames = static_cast<uint32_t>(frames);
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Frames);
    }

    int luaTimerWaitFixedStep(lua_State* L) {
        if (LuaBinding::isPreviewContext())
            return luaL_error(L, "Timer/Coroutine waits are unavailable in UI frame previews: use anime.createTimer/anime.createTimeline or record_game");
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitFixedStep");
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::FixedStep);
    }

    int luaTimerWaitUntil(lua_State* L) {
        if (LuaBinding::isPreviewContext())
            return luaL_error(L, "Timer/Coroutine waits are unavailable in UI frame previews: use anime.createTimer/anime.createTimeline or record_game");
        luaL_checktype(L, 1, LUA_TFUNCTION);
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitUntil");
        lua_pushvalue(L, 1);
        coroutine->predicate = LuaRef(luaL_ref(L, LUA_REGISTRYINDEX));
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Until);
    }

    int luaCoroutineCancel(lua_State* L) {
        CoroutinePtr coroutine = checkCoroutine(L, 1);
        if (!coroutine || coroutine->cancelled) {
            lua_pushboolean(L, 0);
            return 1;
        }
        coroutine->cancelled = true;
        // from inside itself it stops at its next wait; otherwise its wait is cancelled now
        const TimerHandle pending = coroutine->pending;
        finish(coroutine);
        coroutine.reset();
        pending.cancel();
        lua_pushboolean(L, 1);
        return 1;
    }

    int luaCoroutineIsActive(lua_State* L) {
        CoroutinePtr coroutine = checkCoroutine(L, 1);
        lua_pushboolean(L, coroutine && !coroutine->cancelled && !coroutine->finished);
        return 1;
    }

    void registerTimer(lua_State* L) {
        luaL_newmetatable(L, TIMER_HANDLE_META);
        lua_pushcfunction(L, luaHandleIndex);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaHandleToString);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaHandleGc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, luaHandleEq);
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Timer");
        lua_setfield(L, -2, "__name");
        lua_pop(L, 1);

        luaL_newmetatable(L, TIMER_COROUTINE_META);
        lua_newtable(L);
        lua_pushcfunction(L, luaCoroutineCancel);
        lua_setfield(L, -2, "cancel");
        lua_pushcfunction(L, luaCoroutineIsActive);
        lua_setfield(L, -2, "isActive");
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "TimerCoroutine");
        lua_setfield(L, -2, "__name");
        lua_pop(L, 1);

        static const luaL_Reg functions[] = {
            {"after", luaTimerAfter},
            {"every", luaTimerEvery},
            {"nextFrame", luaTimerNextFrame},
            {"afterFrames", luaTimerAfterFrames},
            {"afterFixedSteps", luaTimerAfterFixedSteps},
            {"run", luaTimerRun},
            {"wait", luaTimerWait},
            {"waitRealtime", luaTimerWaitRealtime},
            {"waitFrames", luaTimerWaitFrames},
            {"waitFixedStep", luaTimerWaitFixedStep},
            {"waitUntil", luaTimerWaitUntil},
            {"cancelAll", luaTimerCancelAll},
            {nullptr, nullptr},
        };
        luaL_newlib(L, functions);
        lua_setglobal(L, "Timer");
    }

}

void LuaBinding::registerUtilClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    registerTimer(L);
    ScriptScheduler::setTimerAwaitable({&awaitableIs, &awaitableStatus});

    luabridge::getGlobalNamespace(L)
        .beginClass<Angle>("Angle")
        .addStaticFunction("radToDefault", &Angle::radToDefault)
        .addStaticFunction("degToDefault", &Angle::degToDefault)
        .addStaticFunction("defaultToRad", &Angle::defaultToRad)
        .addStaticFunction("defaultToDeg", &Angle::defaultToDeg)
        .addStaticFunction("radToDeg", &Angle::radToDeg)
        .addStaticFunction("degToRad", &Angle::degToRad)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Base64>("Base64")
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Color>("Color")
        .addStaticFunction("linearTosRGB", 
            luabridge::overload<Vector3>(&Color::linearTosRGB),
            luabridge::overload<Vector4>(&Color::linearTosRGB),
            luabridge::overload<const float, const float, const float>(&Color::linearTosRGB),
            luabridge::overload<const float, const float, const float, const float>(&Color::linearTosRGB))
        .addStaticFunction("sRGBToLinear", 
            luabridge::overload<Vector3>(&Color::sRGBToLinear),
            luabridge::overload<Vector4>(&Color::sRGBToLinear),
            luabridge::overload<const float, const float, const float>(&Color::sRGBToLinear),
            luabridge::overload<const float, const float, const float, const float>(&Color::sRGBToLinear))
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}