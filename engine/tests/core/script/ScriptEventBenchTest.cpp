// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a Lua Event's invoke costs (docs/specs/cancellable-prioritised-events.md R23-R25): 0, 1 and
// 8 function listeners. Each case is the median of 7 runs of a precompiled Lua loop of invokes,
// printed as a [bench] line in nanoseconds per invoke. The C++ heap allocations per invoke are
// counted (ThreadAllocationCount, Linux) and the Lua bytes with the collector stopped. R25 compares
// these numbers with the same bench run on the base tree (the PR records both runs).

#include "script/LuaBinding.h"
#include "support/AllocationCounter.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

    struct InvokeCost {
        double ns = 0.0;
        uint64_t allocations = 0;
        int luaBytes = 0;
    };

    std::string runBenchLua(lua_State* L, const std::string& code) {
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    // `benchEvent:invoke(i)` in a precompiled Lua loop of `calls` iterations
    InvokeCost measureInvokes(lua_State* L, int calls) {
        constexpr int runs = 7;
        InvokeCost cost;
        EXPECT_EQ(luaL_loadstring(L, "local ev, n = benchEvent, ...; for i = 1, n do ev:invoke(i) end"), LUA_OK);
        const int loop = lua_gettop(L);
        auto run = [&](int n) {
            lua_pushvalue(L, loop);
            lua_pushinteger(L, n);
            EXPECT_EQ(lua_pcall(L, 1, 0, 0), LUA_OK) << lua_tostring(L, -1);
        };
        run(calls);   // warm-up: the listeners' first calls grow Lua's call stack
        std::vector<double> samples;
        for (int i = 0; i < runs; i++) {
            const auto start = std::chrono::steady_clock::now();
            run(calls);
            samples.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls);
        }
        std::sort(samples.begin(), samples.end());
        cost.ns = samples[runs / 2];

        lua_gc(L, LUA_GCSTOP);
        const int before = lua_gc(L, LUA_GCCOUNT) * 1024 + lua_gc(L, LUA_GCCOUNTB);
        testsupport::ThreadAllocationCount count;
        run(10000);
        cost.allocations = count.count();
        cost.luaBytes = lua_gc(L, LUA_GCCOUNT) * 1024 + lua_gc(L, LUA_GCCOUNTB) - before;
        lua_gc(L, LUA_GCRESTART);
        lua_settop(L, loop - 1);
        return cost;
    }

    // benchEvent = <constructor>() with `listeners` listeners `listener`
    InvokeCost measureEvent(lua_State* L, const char* constructor, const char* listener, int listeners, int calls) {
        const std::string setup = std::string("benchEvent = ") + constructor + "()\n"
                                  "for i = 1, " + std::to_string(listeners) + " do benchEvent:addListener(" + listener + ") end\n";
        EXPECT_EQ(runBenchLua(L, setup), "");
        const InvokeCost cost = measureInvokes(L, calls);
        EXPECT_EQ(runBenchLua(L, "benchEvent = nil; collectgarbage()"), "");
        std::printf("[bench] %s:invoke with %d listeners: %.1f ns, %.3f allocations and %.3f Lua bytes per invoke\n",
                    constructor, listeners, cost.ns, cost.allocations / 10000.0, cost.luaBytes / 10000.0);
        return cost;
    }

}

TEST(ScriptEventBench, InvokeCosts) {
    lua_State* L = jasperengine::LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    constexpr int calls = 200000;
    // a new closure per listener: the same function twice is one listener
    const char* eventListener = "function(v) end";

    const InvokeCost event0 = measureEvent(L, "Event", eventListener, 0, calls);
    const InvokeCost event1 = measureEvent(L, "Event", eventListener, 1, calls);
    const InvokeCost event8 = measureEvent(L, "Event", eventListener, 8, calls);

    if (testsupport::countsAllocations()) {
        EXPECT_EQ(event0.allocations, 0u);
        EXPECT_EQ(event1.allocations, 0u) << "Event:invoke copied its listeners";
        EXPECT_EQ(event8.allocations, 0u) << "Event:invoke copied its listeners";
    }
    EXPECT_EQ(event8.luaBytes, 0);
}
