// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Profiler table (spec S15 R37, docs/mcp/guide/lua-api.md): samples, counters, the
// recording switch, GPU timing (S98 REQ-26) and the last frame's statistics. Bound with the Lua C
// API (enabled and gpuTiming are properties of the table), so its stub is hand-written
// (stubs/Profiler.lua). In a build without the profiler every function exists and does nothing,
// enabled and gpuTiming stay false, and frameStats still works.

#include "LuaBinding.h"
#include "lua.hpp"

#include "Engine.h"
#include "Log.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profile.h"
#include "render/SystemRender.h"

#include <cstring>
#include <vector>

using namespace jasperengine;

namespace {

    // Profiler.gpuTiming holds one GPU timing consumer (the lease get_gpu_passes takes) while true
    bool luaGpuTiming = false;

    void setLuaGpuTiming(bool on){
        if (on == luaGpuTiming)
            return;
        if (!on){
            luaGpuTiming = false;
            SystemRender::releaseGpuTiming();
            return;
        }
#if JASPERENGINE_PROFILER
        const GpuTimingState state = SystemRender::getGpuTimingState();
        if (!state.available){
            static bool logged = false;
            if (!logged){
                logged = true;
                Log::warn("Profiler.gpuTiming stays false: GPU timing is unavailable here (%s; logged once)", state.reason.c_str());
            }
            return;
        }
        luaGpuTiming = true;
        SystemRender::acquireGpuTiming();
#endif
    }

#if JASPERENGINE_PROFILER
    // the open beginSample calls of the main thread (Lua runs there), and whether each recorded
    std::vector<bool>& sampleStack(){
        static std::vector<bool> stack;
        return stack;
    }
#endif

    int luaBeginSample(lua_State* L){
        const char* name = luaL_checkstring(L, 1);
#if JASPERENGINE_PROFILER
        sampleStack().push_back(profiler::isEnabled() && profiler::beginScope(profiler::internName(name)));
#else
        (void)name;
#endif
        return 0;
    }

    int luaEndSample(lua_State* L){
#if JASPERENGINE_PROFILER
        std::vector<bool>& stack = sampleStack();
        if (stack.empty()){
            static bool logged = false;
            if (!logged){
                logged = true;
                Log::warn("Profiler.endSample() without a Profiler.beginSample() (ignored; logged once)");
            }
            return 0;
        }
        const bool begun = stack.back();
        stack.pop_back();
        if (begun)
            profiler::endScope();
#endif
        (void)L;
        return 0;
    }

    // Profiler.sample(name, fn, ...): fn(...) inside a sample; its results, or its error rethrown
    int luaSample(lua_State* L){
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        const int args = lua_gettop(L) - 2;
#if JASPERENGINE_PROFILER
        const bool begun = profiler::isEnabled() && profiler::beginScope(profiler::internName(name));
#else
        (void)name;
#endif
        const int base = 2;
        const int status = LuaBinding::pcallWithTraceback(L, args, LUA_MULTRET);
#if JASPERENGINE_PROFILER
        if (begun)
            profiler::endScope();
#endif
        if (status != LUA_OK)
            return lua_error(L);
        return lua_gettop(L) - (base - 1);
    }

    int luaCounter(lua_State* L){
        const char* name = luaL_checkstring(L, 1);
        const double value = luaL_checknumber(L, 2);
#if JASPERENGINE_PROFILER
        profiler::counter(name, value);
#else
        (void)name;
        (void)value;
#endif
        return 0;
    }

    int luaFrameStats(lua_State* L){
        const Engine::FrameStats& stats = Engine::getFrameStats();
        const FrameTimeSample frame = FrameTimes::latest();
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, (lua_Integer)stats.drawCalls);
        lua_setfield(L, -2, "drawCalls");
        lua_pushinteger(L, (lua_Integer)stats.setPassCalls);
        lua_setfield(L, -2, "setPassCalls");
        lua_pushinteger(L, (lua_Integer)stats.instances);
        lua_setfield(L, -2, "instances");
        lua_pushinteger(L, (lua_Integer)stats.triangles);
        lua_setfield(L, -2, "triangles");
        lua_pushnumber(L, frame.cpuMs);
        lua_setfield(L, -2, "cpuMs");
        lua_pushnumber(L, frame.fps);
        lua_setfield(L, -2, "fps");
        // the newest GPU time that arrived (they come a few frames late, while GPU timing runs)
        for (size_t i = FrameTimes::size(); i-- > 0 && FrameTimes::size() - i <= 8;){
            const FrameTimeSample sample = FrameTimes::at(i);
            if (sample.gpuMs >= 0.0f){
                lua_pushnumber(L, sample.gpuMs);
                lua_setfield(L, -2, "gpuMs");
                break;
            }
        }
        // with Profiler.gpuTiming: the passes of the newest frame whose GPU times all arrived
        if (luaGpuTiming){
            const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, true);
            if (list.gpuFrameValid){
                lua_createtable(L, (int)list.passes.size(), 0);
                lua_Integer n = 0;
                for (const PassRecord& pass : list.passes){
                    if (!pass.gpuValid)
                        continue;
                    lua_createtable(L, 0, 2);
                    lua_pushstring(L, pass.label);
                    lua_setfield(L, -2, "name");
                    lua_pushnumber(L, pass.gpuMs);
                    lua_setfield(L, -2, "ms");
                    lua_rawseti(L, -2, ++n);
                }
                lua_setfield(L, -2, "gpuPasses");
            }
        }
        return 1;
    }

    // Profiler.enabled, read and written through the table's metatable
    int luaIndex(lua_State* L){
        const char* key = lua_tostring(L, 2);
        if (key && std::strcmp(key, "enabled") == 0){
#if JASPERENGINE_PROFILER
            lua_pushboolean(L, profiler::isEnabled());
#else
            lua_pushboolean(L, 0);
#endif
            return 1;
        }
        if (key && std::strcmp(key, "gpuTiming") == 0){
            lua_pushboolean(L, luaGpuTiming);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    int luaNewIndex(lua_State* L){
        const char* key = lua_tostring(L, 2);
        if (key && std::strcmp(key, "enabled") == 0){
#if JASPERENGINE_PROFILER
            profiler::setEnabled(lua_toboolean(L, 3) != 0);
#endif
            return 0;
        }
        if (key && std::strcmp(key, "gpuTiming") == 0){
            setLuaGpuTiming(lua_toboolean(L, 3) != 0);
            return 0;
        }
        lua_rawset(L, 1);
        return 0;
    }

}

void LuaBinding::releaseProfilerState(){
    // a closed state's scripts no longer want GPU timing
    setLuaGpuTiming(false);
}

void LuaBinding::registerProfilerClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    static const luaL_Reg functions[] = {
        {"beginSample", luaBeginSample},
        {"endSample", luaEndSample},
        {"sample", luaSample},
        {"counter", luaCounter},
        {"frameStats", luaFrameStats},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, luaIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "Profiler");
#endif //DISABLE_LUA_BINDINGS
}
