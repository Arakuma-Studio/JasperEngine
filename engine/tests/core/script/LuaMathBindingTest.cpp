// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua math surface of the scripting catch-up (Doriax 86722b85d, 077bfa735): AABB and OBB
// getCorners give all 8 corners as copies (the pointer binding gave one), none for a null or
// infinite box; Vector2/3/4 moveTowards, lerp (clamped, Unity's Lerp) and lerpUnclamped.

#include "script/LuaBinding.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    // runs a chunk; "" on success, else the error message
    std::string runLua(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaMath, AabbGetCornersReturnsEight){
    ASSERT_NE(LuaBinding::getLuaState(), nullptr);
    EXPECT_EQ(runLua(R"(
        local box = AABB(Vector3(-1, -2, -3), Vector3(1, 2, 3))
        local corners = box:getCorners()
        assert(type(corners) == "table", "a table, got " .. type(corners))
        assert(#corners == 8, "8 corners, got " .. #corners)
        for i = 1, 8 do
            assert(corners[i] == box:getCorner(i - 1), "corner " .. i .. " is CornerEnum " .. (i - 1))
        end
        assert(corners[1] == Vector3(-1, -2, -3) and corners[5] == Vector3(1, 2, 3), "min first, max at NEAR_RIGHT_TOP")
        -- copies: they outlive the box
        box = nil
        collectgarbage("collect")
        assert(corners[8] == Vector3(1, -2, 3), "a copy after the box is gone")
    )"), "");
}

TEST(LuaMath, BoxGetCornersEmptyWhenNullOrInfinite){
    ASSERT_NE(LuaBinding::getLuaState(), nullptr);
    EXPECT_EQ(runLua(R"(
        assert(#AABB():getCorners() == 0, "a null AABB has no corners")
        local infinite = AABB()
        infinite:setInfinite()
        assert(#infinite:getCorners() == 0, "an infinite AABB has no corners")
        assert(#OBB():getCorners() == 0, "a null OBB has no corners")
    )"), "");
}

TEST(LuaMath, ObbGetCornersReturnsEight){
    ASSERT_NE(LuaBinding::getLuaState(), nullptr);
    EXPECT_EQ(runLua(R"(
        local box = OBB(Vector3(1, 1, 1), Vector3(1, 2, 3))
        local corners = box:getCorners()
        assert(#corners == 8, "8 corners, got " .. #corners)
        for i = 1, 8 do
            assert(corners[i] == box:getCorner(i - 1), "corner " .. i)
        end
    )"), "");
}

TEST(LuaMath, VectorMoveTowardsAndLerp){
    ASSERT_NE(LuaBinding::getLuaState(), nullptr);
    EXPECT_EQ(runLua(R"(
        local a, b = Vector3(0, 0, 0), Vector3(10, 0, 0)
        assert(a:moveTowards(b, 3) == Vector3(3, 0, 0), "Vector3 moveTowards")
        assert(a:moveTowards(b, 30) == b, "no overshoot")
        assert(Vector3.lerp(a, b, 0.25) == Vector3(2.5, 0, 0), "the static form")
        assert(a:lerp(b, 2) == b, "lerp clamps")
        assert(a:lerpUnclamped(b, 2) == Vector3(20, 0, 0), "lerpUnclamped extrapolates")

        local p, q = Vector2(0, 0), Vector2(3, 4)
        assert(p:moveTowards(q, 2.5) == Vector2(1.5, 2), "Vector2 moveTowards")
        assert(p:lerp(q, -1) == p and p:lerpUnclamped(q, 2) == Vector2(6, 8), "Vector2 lerp")

        local u, v = Vector4(0, 0, 0, 0), Vector4(2, 2, 2, 2)
        assert(u:moveTowards(v, 2) == Vector4(1, 1, 1, 1), "Vector4 moveTowards")
        assert(u:lerp(v, 0.5) == Vector4(1, 1, 1, 1) and u:lerp(v, 5) == v, "Vector4 lerp")
        assert(u:lerpUnclamped(v, -1) == Vector4(-2, -2, -2, -2), "Vector4 lerpUnclamped")
    )"), "");
}
