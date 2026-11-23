// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a call through the Lua bindings costs (docs/specs/S34-player-size.md D6, D8): a value
// type's constructor, operator, method and property (Vector3), an object's method and property
// bound by pointer (a GameObject of a scene), a registry method and a static property. Each case is
// the median of 7 runs of a Lua loop, printed as a [bench] line in nanoseconds per call; the
// benchmark server E compares builds (the shared thunk code, -Os) with them. No budget: the
// numbers are compared between builds of one machine, not against a constant.
//
// OverloadMisses (docs/specs/S92-htmlui-render-cost.md R34, AC20): a call that reaches overload k of
// a set through misses, against overload 1 of the same set, and the dispatch of stock LuaBridge over
// the same overload tables (stockTryOverloadFunctions: its try_overload_functions before S92, kept
// here as the reference). Ratios are printed; the budgets are enforced through perfSlack().

#include "Scene.h"
#include "script/LuaBinding.h"
#include "object/GameObject.h"
#include "object/Mesh.h"
#include "math/Quaternion.h"
#include "ecs/EcsTestUtil.h"
#include "support/AllocationCounter.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

namespace {

    std::string luaBindingBenchRunLua(lua_State* L, const std::string& code){
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

    // Stock LuaBridge's try_overload_functions (engine/libs/luabridge3/LuaBridge.h before S92): an
    // error table per call, a pcall and an error string per miss
    template <bool Member>
    int stockTryOverloadFunctions(lua_State* L){
        const int nargs = lua_gettop(L);
        const int effective_args = nargs - (Member ? 1 : 0);

        lua_pushvalue(L, lua_upvalueindex(1));
        const int idx_overloads = nargs + 1;
        const int num_overloads = static_cast<int>(lua_rawlen(L, idx_overloads));

        lua_createtable(L, num_overloads, 0);
        const int idx_errors = nargs + 2;
        int nerrors = 0;

        lua_pushnil(L);
        while (lua_next(L, idx_overloads) != 0){
            lua_rawgeti(L, -1, 1);
            const int overload_arity = static_cast<int>(lua_tointeger(L, -1));
            if (overload_arity >= 0 && overload_arity != effective_args){
                lua_pushfstring(L, "Skipped overload #%d with unmatched arity of %d instead of %d", nerrors, overload_arity, effective_args);
                lua_rawseti(L, idx_errors, ++nerrors);
                lua_pop(L, 2);
                continue;
            }
            lua_pop(L, 1);

            lua_pushnumber(L, 2);
            lua_gettable(L, -2);
            for (int i = 1; i <= nargs; ++i)
                lua_pushvalue(L, i);

            const int err = lua_pcall(L, nargs, LUA_MULTRET, 0);
            if (err == LUA_OK)
                return lua_gettop(L) - nargs - 4;
            if (err != LUA_ERRRUN)
                return lua_error(L);
            lua_rawseti(L, idx_errors, ++nerrors);
            lua_pop(L, 1);
        }

        lua_pushfstring(L, "All %d overloads returned an error:", nerrors);
        for (int i = 1; i <= nerrors; ++i){
            lua_pushfstring(L, "\n%d: ", i);
            lua_rawgeti(L, idx_errors, i);
        }
        lua_concat(L, nerrors * 2 + 1);
        return lua_error(L);
    }

    // Sets the globals <name> (the overload set's dispatcher, on the top of the stack) and
    // stock<name> (stock's dispatch over the same overload table, its upvalue 1)
    void exposeOverloadSet(lua_State* L, const char* name){
        ASSERT_TRUE(lua_iscfunction(L, -1)) << name;
        ASSERT_NE(lua_getupvalue(L, -1, 1), nullptr) << name;
        ASSERT_TRUE(lua_istable(L, -1)) << name;
        lua_pushcclosure(L, &stockTryOverloadFunctions<true>, 1);
        lua_setglobal(L, (std::string("stock") + name).c_str());
        lua_setglobal(L, name);
    }

    // ns per call of the loop body, the median of 7 runs of a loop taking about 20 ms
    double nsPerCall(lua_State* L, const std::string& setup, const std::string& body){
        auto timed = [&](int iterations){
            const std::string code = setup + "\nfor i = 1, " + std::to_string(iterations) + " do\n" + body + "\nend\n";
            const auto start = std::chrono::steady_clock::now();
            const std::string error = luaBindingBenchRunLua(L, code);
            EXPECT_EQ(error, "") << body;
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
        };
        const double estimate = std::max(timed(50), 1.0);
        const int iterations = std::clamp((int)(20e6 / estimate), 50, 2000000);
        std::vector<double> samples;
        for (int run = 0; run < 7; run++)
            samples.push_back(timed(iterations));
        std::sort(samples.begin(), samples.end());
        return samples[3];
    }

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
        ASSERT_EQ(luaBindingBenchRunLua(L, code), "") << c.name;   // warm-up, and the body must run
        std::vector<double> samples;
        for (int run = 0; run < runs; run++){
            const auto start = std::chrono::steady_clock::now();
            luaBindingBenchRunLua(L, code);
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            samples.push_back(ns / ((double)iterations * c.calls));
        }
        std::sort(samples.begin(), samples.end());
        std::printf("[bench] Lua binding %s: %.1f ns per call\n", c.name, samples[runs / 2]);
    }

    lua_pushnil(L);
    lua_setglobal(L, "benchScene");
    luaBindingBenchRunLua(L, "collectgarbage()");
}

TEST(LuaBindingBench, OverloadMisses){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    luabridge::setGlobal(L, &scene, "benchScene");

    // the dispatchers: Mesh:setShaderUniform (4 overloads, float last), Quaternion.__mul under
    // its number-first wrapper (float, Quaternion, Vector3) and the Vector3 constructor (5 overloads,
    // Vector4 last)
    lua_rawgetp(L, LUA_REGISTRYINDEX, luabridge::detail::getClassRegistryKey<Mesh>());
    lua_pushliteral(L, "setShaderUniform");
    lua_rawget(L, -2);
    exposeOverloadSet(L, "setShaderUniform");
    lua_pop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, luabridge::detail::getClassRegistryKey<Quaternion>());
    lua_pushliteral(L, "__mul");
    lua_rawget(L, -2);
    ASSERT_NE(lua_getupvalue(L, -1, 1), nullptr);
    exposeOverloadSet(L, "quaternionMul");
    lua_pop(L, 2);
    lua_getglobal(L, "Vector3");
    ASSERT_TRUE(lua_getmetatable(L, -1));
    lua_pushliteral(L, "__call");
    lua_rawget(L, -2);
    exposeOverloadSet(L, "vector3New");
    lua_pop(L, 2);

    struct OverloadForm {
        const char* name;
        const char* first;   // overload 1 of the set
        const char* later;   // overload k, reached through misses
        const char* wrong;   // no overload takes it
    };
    const std::string setup = "local m, q, v2, v3, v4 = Mesh(benchScene), Quaternion(), Vector2(1, 2), Vector3(1, 2, 3), Vector4(1, 2, 3, 4)";
    const OverloadForm forms[] = {
        {"setShaderUniform(name, float)", "%s(m, 'u', v4)", "%s(m, 'u', 0.5)", "pcall(%s, m, 'u', true)"},
        {"Quaternion * Vector3", "%s(q, 2.0)", "%s(q, v3)", "pcall(%s, q, true)"},
        {"Vector3(Vector4)", "%s(Vector3)", "%s(Vector3, v4)", "pcall(%s, Vector3, true)"},
    };
    const char* const dispatchers[] = {"setShaderUniform", "quaternionMul", "vector3New"};

    const double slack = testsupport::perfSlack();
    for (std::size_t i = 0; i < std::size(forms); i++){
        auto body = [&](const char* format, const std::string& dispatcher){
            char text[160];
            std::snprintf(text, sizeof(text), format, dispatcher.c_str());
            return std::string(text);
        };
        const std::string patched = dispatchers[i];
        const std::string stock = std::string("stock") + dispatchers[i];
        const double first = nsPerCall(L, setup, body(forms[i].first, patched));
        const double later = nsPerCall(L, setup, body(forms[i].later, patched));
        const double wrong = nsPerCall(L, setup, body(forms[i].wrong, patched));
        const double stockFirst = nsPerCall(L, setup, body(forms[i].first, stock));
        const double stockLater = nsPerCall(L, setup, body(forms[i].later, stock));
        const double stockWrong = nsPerCall(L, setup, body(forms[i].wrong, stock));
        std::printf("[bench] Lua overload %s: overload 1 %.0f ns (stock %.0f, %.2fx), later %.0f ns (stock %.0f, %.1fx faster), "
                    "later/first %.2f, wrong call %.0f ns (stock %.0f, %.2fx)\n",
                    forms[i].name, first, stockFirst, first / stockFirst, later, stockLater, stockLater / later,
                    later / first, wrong, stockWrong, wrong / stockWrong);
        if (slack > 0.0){
            EXPECT_LE(later, (2.0 * first + 1000.0) * slack) << forms[i].name << ": R34, a later overload";
            EXPECT_LE(first, 1.1 * stockFirst * slack) << forms[i].name << ": R34, overload 1";
            EXPECT_LE(wrong, 1.1 * stockWrong * slack) << forms[i].name << ": R34, a wrong call";
        }
    }

    for (const char* name : dispatchers){
        lua_pushnil(L);
        lua_setglobal(L, name);
        lua_pushnil(L);
        lua_setglobal(L, (std::string("stock") + name).c_str());
    }
    lua_pushnil(L);
    lua_setglobal(L, "benchScene");
    luaBindingBenchRunLua(L, "collectgarbage()");
}
