// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a Lua Event's and CancellableEvent's invoke cost (docs/specs/cancellable-prioritised-events.md
// R23-R25): 0, 1 and 8 function listeners. Each case is the median of 7 runs of a precompiled Lua loop of invokes,
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

namespace ScriptEventBenchTest_cpp {

namespace {

    struct InvokeCost {
        const char* constructor;
        int listeners;
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

    // `ev:invoke(i)` in a Lua loop of n iterations over the event at the stack index `event`
    void runInvokes(lua_State* L, int loop, int event, int n) {
        lua_pushvalue(L, loop);
        lua_pushvalue(L, event);
        lua_pushinteger(L, n);
        EXPECT_EQ(lua_pcall(L, 2, 0, 0), LUA_OK) << lua_tostring(L, -1);
    }

    // Each case's median of 7 runs, the cases interleaved in every run so that a ratio compares
    // samples taken under the same load; then its allocations and Lua bytes over 10,000 invokes
    void measureInvokes(lua_State* L, std::vector<InvokeCost>& cases, int calls) {
        constexpr int runs = 7;
        const int base = lua_gettop(L);
        ASSERT_EQ(luaL_loadstring(L, "local ev, n = ...; for i = 1, n do ev:invoke(i) end"), LUA_OK);
        const int loop = base + 1;
        for (const InvokeCost& c : cases) {
            // a new closure per listener: the same function twice is one listener
            const char* listener = std::string(c.constructor) == "Event" ? "function(v) end" : "function(e, v) end";
            const std::string setup = std::string("local ev = ") + c.constructor + "()\n"
                                      "for i = 1, " + std::to_string(c.listeners) + " do ev:addListener(" + listener + ") end\n"
                                      "return ev";
            ASSERT_EQ(luaL_loadstring(L, setup.c_str()), LUA_OK);
            ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK) << lua_tostring(L, -1);
        }
        std::vector<std::vector<double>> samples(cases.size());
        for (size_t k = 0; k < cases.size(); k++) {
            runInvokes(L, loop, loop + 1 + static_cast<int>(k), calls);   // warm-up: the first calls grow Lua's call stack
        }
        for (int run = 0; run < runs; run++) {
            for (size_t k = 0; k < cases.size(); k++) {
                const auto start = std::chrono::steady_clock::now();
                runInvokes(L, loop, loop + 1 + static_cast<int>(k), calls);
                samples[k].push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls);
            }
        }
        for (size_t k = 0; k < cases.size(); k++) {
            InvokeCost& c = cases[k];
            std::sort(samples[k].begin(), samples[k].end());
            c.ns = samples[k][runs / 2];
            lua_gc(L, LUA_GCSTOP);
            const int before = lua_gc(L, LUA_GCCOUNT) * 1024 + lua_gc(L, LUA_GCCOUNTB);
            testsupport::ThreadAllocationCount count;
            runInvokes(L, loop, loop + 1 + static_cast<int>(k), 10000);
            c.allocations = count.count();
            c.luaBytes = lua_gc(L, LUA_GCCOUNT) * 1024 + lua_gc(L, LUA_GCCOUNTB) - before;
            lua_gc(L, LUA_GCRESTART);
            std::printf("[bench] %s:invoke with %d listeners: %.1f ns, %.3f allocations and %.3f Lua bytes per invoke\n",
                        c.constructor, c.listeners, c.ns, c.allocations / 10000.0, c.luaBytes / 10000.0);
        }
        lua_settop(L, base);
        EXPECT_EQ(runBenchLua(L, "collectgarbage()"), "");
    }

}

TEST(ScriptEventBench, InvokeCosts) {
    lua_State* L = jasperengine::LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    std::vector<InvokeCost> cases = {{"Event", 0}, {"CancellableEvent", 0}, {"Event", 1}, {"CancellableEvent", 1},
                                     {"Event", 8}, {"CancellableEvent", 8}};
    measureInvokes(L, cases, 200000);
    const InvokeCost& event0 = cases[0];
    const InvokeCost& cancellable0 = cases[1];
    const InvokeCost& event8 = cases[4];
    const InvokeCost& cancellable8 = cases[5];
    std::printf("[bench] CancellableEvent over Event: %.2fx idle (budget 1.2), %.2fx with 8 listeners (budget 1.15)\n",
                cancellable0.ns / event0.ns, cancellable8.ns / event8.ns);

    if (testsupport::countsAllocations()) {
        for (const InvokeCost& c : cases) {
            EXPECT_EQ(c.allocations, 0u) << c.constructor << ":invoke with " << c.listeners
                                         << " listeners allocated (a copy of the listeners, or a CancelEventArgs)";
        }
    }
    for (const InvokeCost& c : cases) {
        EXPECT_EQ(c.luaBytes, 0) << c.constructor << ":invoke with " << c.listeners << " listeners: the CancelEventArgs is reused";
    }
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(cancellable0.ns, event0.ns * 1.2 * testsupport::perfSlack());
        EXPECT_LE(cancellable8.ns, event8.ns * 1.15 * testsupport::perfSlack());
    }
}

}  // namespace ScriptEventBenchTest_cpp
