// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Profiler table (spec S15 R37, docs/mcp/guide/lua-api.md): samples, counters, the
// recording switch, GPU timing (S98 REQ-26) and the last frame's statistics. Bound with the Lua C
// API (enabled and gpuTiming are properties of the table), so its stub is hand-written
// (stubs/Profiler.lua). In a build without the profiler every function exists and does nothing,
// enabled and gpuTiming stay false, and frameStats still works.
//
// And Lua's Performance table (docs/specs/release-performance-and-gpu-quirks.md R8, R9, R37;
// stubs/Performance.lua): the gameplay frame sampler of every build, for the game's analytics.

#include "LuaBinding.h"
#include "lua.hpp"

#include "Engine.h"
#include "Log.h"
#include "profiler/FrameTimes.h"
#include "profiler/PerfSampler.h"
#include "profiler/Profile.h"
#include "render/SystemRender.h"
#include "script/LuaFunctionBase.h"

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

    // ---- Performance

    void perfSetNumber(lua_State* L, const char* key, double value){
        lua_pushnumber(L, value);
        lua_setfield(L, -2, key);
    }

    void perfPushExcluded(lua_State* L, const PerfSummary& s){
        lua_createtable(L, 0, 6);
        perfSetNumber(L, "paused", s.excludedPaused);
        perfSetNumber(L, "resume", s.excludedResume);
        perfSetNumber(L, "loading", s.excludedLoading);
        perfSetNumber(L, "stall", s.excludedStall);
        perfSetNumber(L, "game", s.excludedGame);
        lua_createtable(L, 0, (int)s.excludedReasons.size());
        for (const auto& [name, seconds] : s.excludedReasons)
            perfSetNumber(L, name.c_str(), seconds);
        lua_setfield(L, -2, "reasons");
        lua_setfield(L, -2, "excluded");
    }

    // R8: the summary as a table (camelCase keys, as the Lua API reads)
    void perfPushSummary(lua_State* L, const PerfSummary& s){
        lua_createtable(L, 0, 24);
        perfSetNumber(L, "seconds", s.seconds);
        lua_pushinteger(L, (lua_Integer)s.frames);
        lua_setfield(L, -2, "frames");
        perfPushExcluded(L, s);
        if (s.seconds <= 0.0)
            return;
        lua_createtable(L, 0, 3);
        perfSetNumber(L, "avg", s.fpsAvg);
        perfSetNumber(L, "p50", s.fpsP50);
        perfSetNumber(L, "p10", s.fpsP10);
        lua_setfield(L, -2, "fps");
        lua_createtable(L, 0, 4);
        perfSetNumber(L, "p50", s.frameMsP50);
        perfSetNumber(L, "p90", s.frameMsP90);
        perfSetNumber(L, "p99", s.frameMsP99);
        perfSetNumber(L, "max", s.frameMsMax);
        lua_setfield(L, -2, "frameMs");
        lua_createtable(L, 0, 2);
        perfSetNumber(L, "avg", s.cpuMsAvg);
        perfSetNumber(L, "p90", s.cpuMsP90);
        lua_setfield(L, -2, "cpuMs");
        if (s.mainThreadCpuMs >= 0.0)
            perfSetNumber(L, "mainThreadCpuMs", s.mainThreadCpuMs);
        lua_pushstring(L, s.threadClock ? "thread_clock" : "unavailable");
        lua_setfield(L, -2, "mainThreadCpu");
        if (s.gpuFrames > 0){
            lua_createtable(L, 0, 2);
            perfSetNumber(L, "avg", s.gpuMsAvg);
            lua_pushinteger(L, (lua_Integer)s.gpuFrames);
            lua_setfield(L, -2, "frames");
            lua_setfield(L, -2, "gpuMs");
        }
        lua_pushinteger(L, (lua_Integer)s.hitches);
        lua_setfield(L, -2, "hitches");
        lua_createtable(L, 0, 4);
        perfSetNumber(L, "nominal", s.thermal[0]);
        perfSetNumber(L, "fair", s.thermal[1]);
        perfSetNumber(L, "serious", s.thermal[2]);
        perfSetNumber(L, "critical", s.thermal[3]);
        lua_setfield(L, -2, "thermal");
        lua_pushinteger(L, (lua_Integer)s.tier);
        lua_setfield(L, -2, "tier");
        lua_pushinteger(L, s.tierChanges);
        lua_setfield(L, -2, "tierChanges");
        const std::pair<const char*, const std::string*> strings[] = {
            {"gpu", &s.gpu}, {"driver", &s.driver}, {"backend", &s.backend}, {"platform", &s.platform}, {"version", &s.version}};
        for (const auto& [key, value] : strings){
            lua_pushstring(L, value->c_str());
            lua_setfield(L, -2, key);
        }
        lua_pushinteger(L, (lua_Integer)s.buildId);
        lua_setfield(L, -2, "buildId");
        lua_createtable(L, (int)s.quirks.size(), 0);
        for (size_t i = 0; i < s.quirks.size(); i++){
            lua_pushstring(L, s.quirks[i].c_str());
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        lua_setfield(L, -2, "quirks");
        if (!s.histogramCounts.empty()){
            lua_createtable(L, 0, 2);
            lua_createtable(L, (int)s.histogramEdgesMs.size(), 0);
            for (size_t i = 0; i < s.histogramEdgesMs.size(); i++){
                lua_pushnumber(L, s.histogramEdgesMs[i]);
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            }
            lua_setfield(L, -2, "edgesMs");
            lua_createtable(L, (int)s.histogramCounts.size(), 0);
            for (size_t i = 0; i < s.histogramCounts.size(); i++){
                lua_pushinteger(L, (lua_Integer)s.histogramCounts[i]);
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            }
            lua_setfield(L, -2, "counts");
            lua_setfield(L, -2, "histogram");
        }
    }

    // onSample's function: called with the summary table; a hot reload or a closed state retires it
    class PerfSampleCallback : public LuaFunctionBase {
    public:
        explicit PerfSampleCallback(lua_State* L) : LuaFunctionBase(L) {}
        void operator()(const PerfSummary& summary){
            if (!isLive())
                return;
            push_function(m_vm, m_func);
            perfPushSummary(m_vm, summary);
            call(1);
        }
    };

    constexpr const char* PERF_SAMPLE_TAG = "lua_performance_onsample";

    int luaPerfSetSampling(lua_State* L){
        const bool on = lua_toboolean(L, 1) != 0;
        const char* reason = luaL_optstring(L, 2, "");
        PerfSampler::markSetByGame();
        PerfSampler::setSampling(on, reason);
        return 0;
    }

    int luaPerfIsSampling(lua_State* L){
        lua_pushboolean(L, PerfSampler::isSampling() ? 1 : 0);
        return 1;
    }

    int luaPerfGetSummary(lua_State* L){
        bool histogram = false;
        if (lua_istable(L, 1)){
            lua_getfield(L, 1, "histogram");
            histogram = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        perfPushSummary(L, PerfSampler::summary(histogram));
        return 1;
    }

    int luaPerfReset(lua_State* L){
        (void)L;
        PerfSampler::reset();
        return 0;
    }

    // Performance.onSample(fn, seconds): fn(summary) at each window of `seconds` (30..3600,
    // default 300) of committed gameplay; onSample(nil) removes it
    int luaPerfOnSample(lua_State* L){
        if (lua_isnoneornil(L, 1)){
            PerfSampler::onWindow.remove(PERF_SAMPLE_TAG);
            return 0;
        }
        luaL_checktype(L, 1, LUA_TFUNCTION);
        const lua_Integer seconds = luaL_optinteger(L, 2, PerfSampler::DEFAULT_WINDOW_SECONDS);
        if (seconds < 30 || seconds > 3600)
            return luaL_error(L, "Performance.onSample: seconds must be 30 to 3600 (got %d)", (int)seconds);
        PerfSampler::setWindowSeconds((int)seconds);
        lua_pushvalue(L, 1);
        PerfSampleCallback callback(L);
        PerfSampler::onWindow.add(PERF_SAMPLE_TAG, std::function<void(const PerfSummary&)>(callback));
        return 0;
    }

    // Profiler.enabled, read and written through the table's metatable
    int luaPerformanceIndex(lua_State* L){
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

    int luaPerformanceNewIndex(lua_State* L){
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
    lua_pushcfunction(L, luaPerformanceIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaPerformanceNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "Profiler");

    static const luaL_Reg performance[] = {
        {"setSampling", luaPerfSetSampling},
        {"isSampling", luaPerfIsSampling},
        {"getSummary", luaPerfGetSummary},
        {"reset", luaPerfReset},
        {"onSample", luaPerfOnSample},
        {nullptr, nullptr},
    };
    luaL_newlib(L, performance);
    lua_setglobal(L, "Performance");
#endif //DISABLE_LUA_BINDINGS
}
