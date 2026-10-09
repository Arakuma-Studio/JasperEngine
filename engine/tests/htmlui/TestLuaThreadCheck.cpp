// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The engine's Lua state belongs to the thread that made it (LuaBinding::getLuaState). Not
// HTML UI: engine-level coverage hosted by this harness. The check of that rule (its
// allocator compares threads) must see a use from another thread, and must stay quiet on
// the owner's; the tests run with JASPER_LUA_THREAD_CHECK=abort (main.cpp), so any test
// touching the state off the main thread aborts the run.

#include "harness/GpuEnvironment.h"

#include "script/LuaBinding.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    // allocates in the state: a table of strings, then a full collection
    void allocate(lua_State* L){
        lua_newtable(L);
        for (int i = 1; i <= 2000; i++){
            lua_pushfstring(L, "value %d", i);
            lua_rawseti(L, -2, i);
        }
        lua_pop(L, 1);
        lua_gc(L, LUA_GCCOLLECT);
    }

    class LuaThreadCheckTest : public ::testing::Test {
    protected:
        void SetUp() override{
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            mode = LuaBinding::getThreadCheck();
        }

        void TearDown() override{
            LuaBinding::setThreadCheck(mode);
        }

        lua_State* L = nullptr;
        LuaBinding::ThreadCheck mode = LuaBinding::ThreadCheck::Report;
    };

}

TEST_F(LuaThreadCheckTest, TheTestsAbortOnAnOffThreadUse){
    EXPECT_EQ(mode, LuaBinding::ThreadCheck::Abort) << "JASPER_LUA_THREAD_CHECK=abort did not reach the engine";
}

TEST_F(LuaThreadCheckTest, TheOwnerThreadIsQuiet){
    EXPECT_TRUE(LuaBinding::isLuaThread());
    const size_t before = LuaBinding::getOffThreadUseCount();
    allocate(L);
    EXPECT_EQ(LuaBinding::getOffThreadUseCount(), before);
}

TEST_F(LuaThreadCheckTest, AnotherThreadIsSeen){
    // Report only: the use is counted and the run goes on. The main thread waits for the
    // worker, so the state is never used by two threads at once here.
    LuaBinding::setThreadCheck(LuaBinding::ThreadCheck::Report);
    const size_t before = LuaBinding::getOffThreadUseCount();
    bool onLuaThread = true;
    std::thread worker([&](){
        onLuaThread = LuaBinding::isLuaThread();
        allocate(L);
    });
    worker.join();
    EXPECT_FALSE(onLuaThread);
    EXPECT_GT(LuaBinding::getOffThreadUseCount(), before) << "a Lua allocation on another thread went unseen";

    // a collection alone, as the garbage collector would run it from there
    const size_t afterAllocation = LuaBinding::getOffThreadUseCount();
    lua_newtable(L);
    lua_pop(L, 1);
    std::thread collector([&](){ lua_gc(L, LUA_GCCOLLECT); });
    collector.join();
    EXPECT_GT(LuaBinding::getOffThreadUseCount(), afterAllocation) << "a collection on another thread went unseen";
}

TEST_F(LuaThreadCheckTest, AbortStopsTheProcess){
    LuaBinding::setThreadCheck(LuaBinding::ThreadCheck::Abort);
    // the process has threads (the engine's pool): the child runs the test anew, not a fork
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH({
        std::thread worker([&](){ allocate(L); });
        worker.join();
    }, "used from a thread other than its own");
}

TEST_F(LuaThreadCheckTest, EngineGlobalNamesAreTheEngines){
    const std::vector<std::string> names = LuaBinding::getEngineGlobalNames();
    for (const char* name : {"GameObject", "Scene", "Engine", "RegisterEvent", "RegisterEngineEvent", "print", "string", "table"}){
        EXPECT_TRUE(std::binary_search(names.begin(), names.end(), std::string(name))) << name << " is missing";
    }
    ASSERT_EQ(luaL_dostring(L, "threadCheckLeftover = {}"), LUA_OK);
    const std::vector<std::string> later = LuaBinding::getEngineGlobalNames();
    EXPECT_FALSE(std::binary_search(later.begin(), later.end(), std::string("threadCheckLeftover")))
        << "a global a script set is not the engine's";
    EXPECT_EQ(names, later);
    luaL_dostring(L, "threadCheckLeftover = nil");

    // the same names from another thread, without touching the state
    std::vector<std::string> fromWorker;
    std::thread worker([&](){ fromWorker = LuaBinding::getEngineGlobalNames(); });
    worker.join();
    EXPECT_EQ(fromWorker, names);
}
