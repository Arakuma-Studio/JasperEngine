// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Coroutines, timers and the Time class for Lua (spec 2026-10-31-coroutines-design.md). The
// wait helpers and starters are lua_CFunctions of ScriptScheduler.cpp; the self:* methods of
// script instances are in ScriptInstanceBase (LuaBinding::pushScriptInstanceBase).

#include "LuaBinding.h"
#include "EngineTime.h"
#include "script/ScriptScheduler.h"

#include "lua.hpp"
#include "LuaBridge.h"

using namespace jasperengine;

void LuaBinding::registerCoroutineClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    // an engine coroutine (self:startCoroutine, Coroutine.start) and the wait helpers, which
    // yield the running one until the condition holds (docs/mcp/guide/lua-api.md "Coroutines and timers")
    luabridge::getGlobalNamespace(L)
        .beginClass<ScriptCoroutine>("Coroutine")
        .addProperty("id", &ScriptCoroutine::getId)
        .addProperty("status", &ScriptCoroutine::getStatus)
        // its error with the coroutine's traceback (status "error"), else nil
        .addProperty("error", [](const ScriptCoroutine* self, lua_State* L) -> luabridge::LuaRef {
            ScriptSchedulerLua::coroutineError(L, self->getId());
            return luabridge::LuaRef::fromStack(L);
        })
        // the script instance that started it (self), nil for Coroutine.start or once it is released
        .addProperty("owner", [](const ScriptCoroutine* self, lua_State* L) -> luabridge::LuaRef {
            ScriptSchedulerLua::coroutineOwner(L, self->getId());
            return luabridge::LuaRef::fromStack(L);
        })
        .addFunction("isDone", &ScriptCoroutine::isDone)
        .addFunction("stop", &ScriptCoroutine::stop)
        .addFunction("__tostring", &ScriptCoroutine::toString)
        .addStaticFunction("start", &ScriptSchedulerLua::start)
        .addStaticFunction("running", &ScriptSchedulerLua::running)
        .addStaticFunction("isInside", &ScriptSchedulerLua::isInside)
        .addStaticFunction("wait", &ScriptSchedulerLua::wait)
        .addStaticFunction("waitRealtime", &ScriptSchedulerLua::waitRealtime)
        .addStaticFunction("nextFrame", &ScriptSchedulerLua::nextFrame)
        .addStaticFunction("waitFrames", &ScriptSchedulerLua::waitFrames)
        .addStaticFunction("waitForFixedUpdate", &ScriptSchedulerLua::waitForFixedUpdate)
        .addStaticFunction("waitForEndOfFrame", &ScriptSchedulerLua::waitForEndOfFrame)
        .addStaticFunction("waitUntil", &ScriptSchedulerLua::waitUntil)
        .addStaticFunction("waitWhile", &ScriptSchedulerLua::waitWhile)
        .addStaticFunction("waitFor", &ScriptSchedulerLua::waitFor)
        .endClass();

    // what self:invoke / invokeRepeating return (a script's timer, in seconds, scaled by
    // default). The global Timer (Timer.after / every / run...) is UtilClassesLua.cpp's, on the
    // C++ Scheduler; its handles read the same fields (id, remaining, interval, unscaled,
    // cancel, isDone). anime.createTimer stays the HTML UI clock
    luabridge::getGlobalNamespace(L)
        .beginClass<ScriptTimer>("ScriptTimer")
        .addProperty("id", &ScriptTimer::getId)
        .addProperty("remaining", &ScriptTimer::getRemaining)
        .addProperty("interval", &ScriptTimer::getInterval)
        .addProperty("unscaled", &ScriptTimer::isUnscaled)
        .addFunction("cancel", &ScriptTimer::cancel)
        .addFunction("isDone", &ScriptTimer::isDone)
        .addFunction("__tostring", &ScriptTimer::toString)
        // oe-port's handle name, so either family of scripts reads both
        .addFunction("isActive", +[](const ScriptTimer* self){ return !self->isDone(); })
        .endClass();

    // Unity's Time (Engine.deltatime and the rest stay; lua-api.md maps the names)
    luabridge::getGlobalNamespace(L)
        .beginClass<Time>("Time")
        .addStaticProperty("deltaTime", &Time::getDeltaTime)
        .addStaticProperty("unscaledDeltaTime", &Time::getUnscaledDeltaTime)
        .addStaticProperty("fixedDeltaTime", &Time::getFixedDeltaTime, &Time::setFixedDeltaTime)
        .addStaticProperty("fixedUnscaledDeltaTime", &Time::getFixedUnscaledDeltaTime)
        .addStaticProperty("time", &Time::getTime)
        .addStaticProperty("fixedTime", &Time::getFixedTime)
        .addStaticProperty("unscaledTime", &Time::getUnscaledTime)
        .addStaticProperty("realtimeSinceStartup", &Time::getRealtimeSinceStartup)
        .addStaticProperty("timeSinceLevelLoad", &Time::getTimeSinceLevelLoad)
        .addStaticProperty("frameCount", &Time::getFrameCount)
        .addStaticProperty("timeScale", &Time::getTimeScale, &Time::setTimeScale)
        .addStaticProperty("maximumDeltaTime", &Time::getMaximumDeltaTime, &Time::setMaximumDeltaTime)
        .addStaticProperty("maximumFixedStepsPerFrame", &Time::getMaximumFixedStepsPerFrame, &Time::setMaximumFixedStepsPerFrame)
        .addStaticProperty("droppedFixedSteps", &Time::getDroppedFixedSteps)
        .addStaticProperty("captureDeltaTime", &Time::getCaptureDeltaTime, &Time::setCaptureDeltaTime)
        .addStaticProperty("smoothDeltaTime", &Time::getSmoothDeltaTime)
        .addStaticProperty("inFixedTimeStep", &Time::isInFixedTimeStep)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
