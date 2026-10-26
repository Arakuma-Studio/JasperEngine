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
#include "LuaReload.h"
#include "util/SubscriptionOwnerIndex.h"

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
    };

    // the options table at index (absent or nil: defaults)
    TimerOptions readOptions(lua_State* L, int index) {
        TimerOptions options;
        if (lua_isnoneornil(L, index)) return options;
        luaL_checktype(L, index, LUA_TTABLE);
        lua_getfield(L, index, "owner");
        if (!lua_isnil(L, -1)) options.owner = ownerKeyOf(lua_topointer(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, index, "clock");
        if (!lua_isnil(L, -1)) {
            // no C++ objects alive across luaL_error
            const char* clock = luaL_checkstring(L, -1);
            if (std::strcmp(clock, "game") == 0) options.clock = TimeDomain::Game;
            else if (std::strcmp(clock, "unscaled") == 0) options.clock = TimeDomain::Unscaled;
            else if (std::strcmp(clock, "real") == 0) options.clock = TimeDomain::Real;
            else luaL_error(L, "Timer: clock must be \"game\", \"unscaled\" or \"real\" (got \"%s\")", clock);
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "count");
        if (!lua_isnil(L, -1)) {
            const lua_Integer count = luaL_checkinteger(L, -1);
            if (count < 1) luaL_error(L, "Timer: count must be 1 or more");
            options.count = static_cast<uint32_t>(count);
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "catchUp");
        options.catchUp = lua_toboolean(L, -1) != 0;
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
        if (!CrashHook::report(name, error)) {
            LuaBinding::logLuaError("Lua Error: %s: %s", name, error.c_str());
        }
    }

    // A timer's function: errors cancel the timer (a repeating one stops)
    struct TimerCall {
        LuaRef function;
        const char* name;
        // the chunk generation of the function: a hot reload of its script ends the timer
        std::shared_ptr<lua_reload::CallbackRecord> record;

        void operator()() {
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return;
            if (record && record->dropped()) {
                Scheduler::game().current().cancel();
                return;
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, function.ref);
            if (LuaBinding::pcallWithTraceback(L, 0, 0) != LUA_OK) {
                const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                lua_pop(L, 1);
                Scheduler::game().current().cancel();
                reportError(name, error);
            }
        }
    };

    struct TimerHandleUserdata {
        uint64_t id;
    };

    void pushHandle(lua_State* L, TimerHandle handle) {
        auto* data = static_cast<TimerHandleUserdata*>(lua_newuserdatauv(L, sizeof(TimerHandleUserdata), 0));
        data->id = handle.id;
        luaL_setmetatable(L, TIMER_HANDLE_META);
    }

    TimerHandle checkHandle(lua_State* L, int index) {
        auto* data = static_cast<TimerHandleUserdata*>(luaL_checkudata(L, index, TIMER_HANDLE_META));
        return TimerHandle{&Scheduler::game(), data->id};
    }

    std::shared_ptr<lua_reload::CallbackRecord> functionRecord(lua_State* L, int index) {
        lua_pushvalue(L, index);
        std::shared_ptr<lua_reload::CallbackRecord> record = lua_reload::callbackRecordForFunction(L);
        lua_pop(L, 1);
        return record;
    }

    int functionRef(lua_State* L, int index) {
        luaL_checktype(L, index, LUA_TFUNCTION);
        lua_pushvalue(L, index);
        return luaL_ref(L, LUA_REGISTRYINDEX);
    }

    int luaTimerAfter(lua_State* L) {
        const double delay = luaL_checknumber(L, 1);
        const TimerOptions options = readOptions(L, 3);
        const int ref = functionRef(L, 2);
        auto record = functionRecord(L, 2);
        pushHandle(L, Scheduler::game().after(delay, TimerCall{LuaRef(ref), "Timer.after", record}, options.clock,
                                              schedulerOptions(options, "Timer.after")));
        return 1;
    }

    int luaTimerEvery(lua_State* L) {
        const double period = luaL_checknumber(L, 1);
        if (!(period > 0.0)) return luaL_error(L, "Timer.every: the period must be more than 0");
        const TimerOptions options = readOptions(L, 3);
        const int ref = functionRef(L, 2);
        auto record = functionRecord(L, 2);
        pushHandle(L, Scheduler::game().every(period, TimerCall{LuaRef(ref), "Timer.every", record}, options.clock,
                                              schedulerOptions(options, "Timer.every")));
        return 1;
    }

    int luaTimerNextFrame(lua_State* L) {
        const TimerOptions options = readOptions(L, 2);
        const int ref = functionRef(L, 1);
        auto record = functionRecord(L, 1);
        pushHandle(L, Scheduler::game().nextFrame(TimerCall{LuaRef(ref), "Timer.nextFrame", record}, schedulerOptions(options, "Timer.nextFrame")));
        return 1;
    }

    int luaTimerAfterFrames(lua_State* L) {
        const lua_Integer frames = luaL_checkinteger(L, 1);
        if (frames < 1) return luaL_error(L, "Timer.afterFrames: frames must be 1 or more");
        const TimerOptions options = readOptions(L, 3);
        const int ref = functionRef(L, 2);
        auto record = functionRecord(L, 2);
        pushHandle(L, Scheduler::game().afterFrames(static_cast<uint32_t>(frames), TimerCall{LuaRef(ref), "Timer.afterFrames", record},
                                                    schedulerOptions(options, "Timer.afterFrames")));
        return 1;
    }

    int luaTimerAfterFixedSteps(lua_State* L) {
        const lua_Integer steps = luaL_checkinteger(L, 1);
        if (steps < 1) return luaL_error(L, "Timer.afterFixedSteps: steps must be 1 or more");
        const TimerOptions options = readOptions(L, 3);
        const int ref = functionRef(L, 2);
        auto record = functionRecord(L, 2);
        pushHandle(L, Scheduler::game().afterFixedSteps(static_cast<uint32_t>(steps), TimerCall{LuaRef(ref), "Timer.afterFixedSteps", record},
                                                        schedulerOptions(options, "Timer.afterFixedSteps")));
        return 1;
    }

    int luaTimerCancelAll(lua_State* L) {
        luaL_checkany(L, 1);
        lua_pushinteger(L, static_cast<lua_Integer>(Scheduler::game().cancelOwner(ownerKeyOf(lua_topointer(L, 1)))));
        return 1;
    }

    int luaHandleCancel(lua_State* L) {
        lua_pushboolean(L, checkHandle(L, 1).cancel());
        return 1;
    }

    int luaHandleIsActive(lua_State* L) {
        lua_pushboolean(L, checkHandle(L, 1).active());
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
        const double seconds = luaL_checknumber(L, 1);
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.wait");
        coroutine->seconds = seconds;
        coroutine->waitClock = coroutine->options.clock;
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Seconds);
    }

    int luaTimerWaitRealtime(lua_State* L) {
        const double seconds = luaL_checknumber(L, 1);
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitRealtime");
        coroutine->seconds = seconds;
        coroutine->waitClock = TimeDomain::Real;
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Seconds);
    }

    int luaTimerWaitFrames(lua_State* L) {
        const lua_Integer frames = luaL_optinteger(L, 1, 1);
        if (frames < 1) return luaL_error(L, "Timer.waitFrames: frames must be 1 or more");
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitFrames");
        coroutine->frames = static_cast<uint32_t>(frames);
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::Frames);
    }

    int luaTimerWaitFixedStep(lua_State* L) {
        CoroutinePtr coroutine = runningCoroutine(L, "Timer.waitFixedStep");
        Coroutine& target = *coroutine;
        coroutine.reset();
        return yieldFor(L, target, WaitKind::FixedStep);
    }

    int luaTimerWaitUntil(lua_State* L) {
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
        lua_newtable(L);
        lua_pushcfunction(L, luaHandleCancel);
        lua_setfield(L, -2, "cancel");
        lua_pushcfunction(L, luaHandleIsActive);
        lua_setfield(L, -2, "isActive");
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "TimerHandle");
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