// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a call through the Lua bindings costs (docs/specs/S34-player-size.md D6, D8): a value
// type's constructor, operator, method and property (Vector3), an object's method and property
// bound by pointer (a GameObject of a scene), a registry method and a static property. Each case is
// the median of 7 runs of a Lua loop, printed as a [bench] line in nanoseconds per call; the
// benchmark server E compares builds (the shared thunk code, -Os) with them. No budget: the
// numbers are compared between builds of one machine, not against a constant.

#include "Scene.h"
#include "script/LuaBinding.h"
#include "object/GameObject.h"
#include "ecs/EcsTestUtil.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

namespace {

    std::string runLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    struct BenchCase {
        const char* name;
        const char* body;      // the loop's body (i: the iteration)
        int calls;             // binding calls in one body
    };

}

TEST(LuaBindingBench, CallsAndProperties){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity entity = makeTransformEntity(scene);
    luabridge::setGlobal(L, &scene, "benchScene");
    lua_pushinteger(L, entity);
    lua_setglobal(L, "benchEntity");

    const BenchCase cases[] = {
        {"Vector3 constructor and operator +", "local w = Vector3(i, 2, 3) + v", 2},
        {"Vector3 method (length)", "s = s + v:length()", 1},
        {"Vector3 property get and set", "v.x = v.y", 2},
        {"Vector3 method with an argument (dotProduct)", "s = s + v:dotProduct(v)", 1},
        {"GameObject method (setPosition)", "go:setPosition(i, 0, 0)", 1},
        {"GameObject method returning a value", "local p = go:getWorldPosition()", 1},
        {"GameObject property get (position)", "local p = go.position", 1},
        {"EntityRegistry method (isEntityCreated)", "local b = benchScene:isEntityCreated(benchEntity)", 1},
        {"static property (Engine.deltatime)", "local d = Engine.deltatime", 1},
    };
    constexpr int iterations = 200000;
    constexpr int runs = 7;

    for (const BenchCase& c : cases){
        const std::string code = std::string("local go = GameObject(benchScene, benchEntity)\n"
                                             "local v = Vector3(1, 2, 3)\nlocal s = 0\n"
                                             "for i = 1, ") + std::to_string(iterations) + " do\n" + c.body + "\nend\n";
        ASSERT_EQ(runLua(L, code), "") << c.name;   // warm-up, and the body must run
        std::vector<double> samples;
        for (int run = 0; run < runs; run++){
            const auto start = std::chrono::steady_clock::now();
            runLua(L, code);
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            samples.push_back(ns / ((double)iterations * c.calls));
        }
        std::sort(samples.begin(), samples.end());
        std::printf("[bench] Lua binding %s: %.1f ns per call\n", c.name, samples[runs / 2]);
    }

    lua_pushnil(L);
    lua_setglobal(L, "benchScene");
    runLua(L, "collectgarbage()");
}
