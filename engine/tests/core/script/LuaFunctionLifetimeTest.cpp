// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Lua function a C++ subscription holds (Engine.onUpdate's RegisterEngineEvent, a component's
// event) outlives the Lua state it belongs to when the engine shuts down without releasing it,
// and Android relaunches an activity in the same process (a configuration change the manifest
// does not handle): the next engine has a new state, and calling or releasing the old function
// read the closed one (SIGSEGV in lua_rawgeti, found by the S30 emulator check). Such a function
// is never called nor released; one of the live state still is.

#include "script/LuaBinding.h"
#include "script/LuaFunction.h"
#include "lua.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace jasperengine;

namespace {

    int probeCalls(lua_State* L) {
        lua_getglobal(L, "lifetimeCalls");
        const int calls = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return calls;
    }

    void defineProbe(lua_State* L) {
        ASSERT_EQ(luaL_dostring(L, "lifetimeCalls = 0; function lifetimeProbe() lifetimeCalls = lifetimeCalls + 1 end"), LUA_OK);
    }

}

TEST(LuaFunctionLifetime, AFunctionOfAClosedStateIsNeverCalledNorReleased) {
    lua_State* first = LuaBinding::getLuaState();
    ASSERT_NE(first, nullptr);
    defineProbe(first);
    auto held = std::make_unique<LuaFunction<void>>(first, "lifetimeProbe");
    (*held)();
    EXPECT_EQ(probeCalls(first), 1);
    EXPECT_TRUE(held->isLive());
    const uint64_t generation = LuaBinding::getLuaStateGeneration();

    // what an engine restart does to the state (systemShutdown, then systemInit)
    LuaBinding::recreateLuaState();
    lua_State* second = LuaBinding::getLuaState();
    ASSERT_NE(second, nullptr);
    EXPECT_GT(LuaBinding::getLuaStateGeneration(), generation);

    EXPECT_FALSE(held->isLive());
    (*held)();                                   // the closed state's function: nothing runs
    LuaFunction<void> copy(*held);               // a copy of it is as dead
    EXPECT_FALSE(copy.isLive());
    copy();
    LuaFunction<void> assigned(*held);
    assigned = copy;
    EXPECT_FALSE(assigned.isLive());
    held.reset();                                // its reference is not released into the new state

    // the new state's own functions run and release as before
    defineProbe(second);
    LuaFunction<void> fresh(second, "lifetimeProbe");
    EXPECT_TRUE(fresh.isLive());
    fresh();
    fresh();
    EXPECT_EQ(probeCalls(second), 2);
    LuaFunction<int> returning(second, "lifetimeProbe");
    EXPECT_TRUE(returning.isLive());
}
