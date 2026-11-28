// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua math surface of the scripting catch-up (Doriax 86722b85d, 077bfa735): AABB and OBB
// getCorners give all 8 corners as copies (the pointer binding gave one), none for a null or
// infinite box; Vector2/3/4 moveTowards, lerp (clamped, Unity's Lerp) and lerpUnclamped.
// S95: Quaternion.euler / angleAxis (REQ-1) and wrong math calls raising errors (REQ-2).

#include "Scene.h"
#include "object/GameObject.h"
#include "script/LuaBinding.h"
#include "LogCapture.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

namespace LuaMathBindingTest_cpp {

using namespace jasperengine;

namespace {

    // runs a chunk; "" on success, else the error message
    std::string runMathLua(const std::string& code){
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
    EXPECT_EQ(runMathLua(R"(
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
    EXPECT_EQ(runMathLua(R"(
        assert(#AABB():getCorners() == 0, "a null AABB has no corners")
        local infinite = AABB()
        infinite:setInfinite()
        assert(#infinite:getCorners() == 0, "an infinite AABB has no corners")
        assert(#OBB():getCorners() == 0, "a null OBB has no corners")
    )"), "");
}

TEST(LuaMath, ObbGetCornersReturnsEight){
    ASSERT_NE(LuaBinding::getLuaState(), nullptr);
    EXPECT_EQ(runMathLua(R"(
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
    EXPECT_EQ(runMathLua(R"(
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

// S95 AC-1: Quaternion.euler is Quaternion(x, y, z), with an order it is fromEulerAngles, and
// RotationOrder.YXZ composes as Unity's Quaternion.Euler: z, then x, then y. angleAxis is the
// angle-axis constructor. Four numbers are always the raw (w, x, y, z) form.
TEST(LuaMath, EulerFactoryMatchesConstructor){
    ASSERT_NE(LuaBinding::getLuaState(), nullptr);
    EXPECT_EQ(runMathLua(R"lua(
        assert(type(Quaternion.euler) == "function" and type(Quaternion.angleAxis) == "function", "the factories exist")
        local function near(a, b)
            -- q and -q are the same rotation
            local d1 = math.abs(a.w - b.w) + math.abs(a.x - b.x) + math.abs(a.y - b.y) + math.abs(a.z - b.z)
            local d2 = math.abs(a.w + b.w) + math.abs(a.x + b.x) + math.abs(a.y + b.y) + math.abs(a.z + b.z)
            return math.min(d1, d2) < 1e-5
        end
        local function nearV(a, b) return (a - b):length() < 1e-4 end
        local X, Y, Z = Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)
        local probe = Vector3(0.3, -0.5, 0.8)
        for x = -180, 180, 45 do
            for y = -180, 180, 45 do
                for z = -180, 180, 45 do
                    local here = " at " .. x .. ", " .. y .. ", " .. z
                    assert(Quaternion.euler(x, y, z) == Quaternion(x, y, z), "default order" .. here)
                    for _, order in ipairs({RotationOrder.XYZ, RotationOrder.ZXY, RotationOrder.ZYX}) do
                        local q = Quaternion()
                        q:fromEulerAngles(x, y, z, order)
                        assert(Quaternion.euler(x, y, z, order) == q, "order " .. order .. here)
                    end
                    local unity = Quaternion.angleAxis(y, Y) * Quaternion.angleAxis(x, X) * Quaternion.angleAxis(z, Z)
                    local q = Quaternion.euler(x, y, z, RotationOrder.YXZ)
                    assert(near(q, unity), "YXZ is Unity's Euler" .. here .. ": " .. tostring(q) .. " vs " .. tostring(unity))
                    -- applied to a vector: around Z first, then X, then Y
                    local stepped = Quaternion.angleAxis(y, Y) * (Quaternion.angleAxis(x, X) * (Quaternion.angleAxis(z, Z) * probe))
                    assert(nearV(q * probe, stepped), "z, then x, then y" .. here)
                end
            end
        end
        for a = -270, 270, 30 do
            for _, axis in ipairs({X, Y, Z, Vector3(0, -1, 0)}) do
                assert(Quaternion.angleAxis(a, axis) == Quaternion(a, axis), "angleAxis " .. a)
            end
        end
        assert(near(Quaternion.angleAxis(90, Vector3(0, 5, 0)), Quaternion(90, Y)), "the axis is normalized")
        assert(Quaternion.angleAxis(90, Vector3(0, 0, 0)) == Quaternion.IDENTITY, "a zero axis: identity")
        assert(Quaternion(0, 0, 0, RotationOrder.YXZ) == Quaternion(0, 0, 0, 2), "four numbers are (w, x, y, z)")
        local ok, message = pcall(Quaternion.euler, 10, 20, 30, 42)
        assert(not ok and tostring(message):find("RotationOrder", 1, true), "a bad order raises: " .. tostring(message))
        assert(not pcall(Quaternion.euler, 10, 20, 30, "YXZ") and not pcall(Quaternion.euler, 10, 20, 30, 1.5),
            "an order that is not a RotationOrder raises")
    )lua"), "");
}

// S95 AC-2: every math constructor and operator, and the matrices' indexed accessors, under
// pcall with wrong arguments (nil, numbers, strings, tables, vectors, matrices, a GameObject, a
// class metatable): each call returns or raises an error, none crashes or reads outside a value.
TEST(LuaMath, WrongCallsRaise){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    GameObject go(&scene);
    luabridge::setGlobal(L, go, "wrongCallsGo");
    EXPECT_EQ(runMathLua(R"lua(
        -- the crashes S32 named
        local ok, message = pcall(Quaternion, nil)
        assert(not ok, "Quaternion(nil) raises")
        ok, message = pcall(Quaternion, Vector3(1, 2, 3))
        assert(not ok, "Quaternion(Vector3) raises (it read three vectors from one)")
        local mt = debug.getmetatable(Vector3())
        assert(type(tostring(mt)) == "string", "tostring of a class metatable")
        assert(not pcall(function() return Vector3() + mt end) and not pcall(function() return mt * 2 end),
            "a class metatable is no instance")
        -- nil first: Lua calls the second operand's metamethod with nil as self
        ok, message = pcall(function() return nil + Vector3() end)
        assert(not ok and tostring(message):find("nil", 1, true), "nil + v raises: " .. tostring(message))
        assert(not pcall(function() return nil < Vector2() end) and not pcall(function() return nil * Quaternion() end),
            "nil < v, nil * q raise")
        assert(not pcall(Quaternion.fromAxes, Quaternion(), Vector3()), "fromAxes takes three vectors")
        assert(pcall(Quaternion.fromAxes, Quaternion(), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)), "three vectors")
        for _, index in ipairs({-1, 3, 4, 2 ^ 40}) do
            assert(not pcall(Matrix3.get, Matrix3(), index, 0), "Matrix3:get(" .. index .. ") raises")
            assert(not pcall(Matrix3.row, Matrix3(), index), "Matrix3:row(" .. index .. ") raises")
        end
        assert(Matrix4():get(3, 3) == 1 and not pcall(Matrix4.set, Matrix4(), 4, 0, 1), "Matrix4 indices 0..3")
        ok, message = pcall(Matrix4.setRow, Matrix4(), 7, Vector4())
        assert(not ok and tostring(message):find("out of range", 1, true), "names the range: " .. tostring(message))

        local v2, v3, v4 = Vector2(1, 2), Vector3(1, 2, 3), Vector4(1, 2, 3, 4)
        local q, m3, m4 = Quaternion(1, 0, 0, 0), Matrix3(), Matrix4()
        local pool = table.pack(nil, 0, 2, -1, 7, 1.5, 2 ^ 40, 1e300, 0 / 0, math.huge, "a", "12", true, {}, print,
                                v2, v3, v4, q, m3, m4, wrongCallsGo, mt, debug.getmetatable(Quaternion()), Rect(0, 0, 1, 1))
        local count = pool.n
        local small = table.pack(nil, 2, "a", {}, v3, q, m4, wrongCallsGo, mt)
        local smallCount = small.n
        local calls = 0
        local function try(fn, ...)
            calls = calls + 1
            local results = table.pack(pcall(fn, ...))
            for i = 2, results.n do
                -- a result can be printed (a value of a bound class is a real one)
                if type(results[i]) == "userdata" then tostring(results[i]) end
            end
        end
        local constructors = { Vector2, Vector3, Vector4, Quaternion, Matrix3, Matrix4, Rect, EdgeInsets, Sphere,
                               Ray, Plane, AABB, OBB, Keyframe, AnimationCurve, GradientColorKey, GradientAlphaKey,
                               Gradient, MinMaxCurve, MinMaxGradient,
                               Quaternion.euler, Quaternion.angleAxis, Quaternion.slerp, Quaternion.nlerp,
                               Quaternion.lookRotation, Matrix4.translateMatrix, Matrix4.rotateMatrix,
                               Matrix4.scaleMatrix, Matrix3.scaleMatrix, Matrix3.rotateMatrix }
        for _, ctor in ipairs(constructors) do
            try(ctor)
            for i = 1, count do try(ctor, pool[i]) end
            for i = 1, smallCount do
                for j = 1, smallCount do try(ctor, small[i], small[j]) end
                try(ctor, small[i], 2, 3)
                try(ctor, 1, small[i], 3)
                try(ctor, 1, 2, 3, small[i])
            end
        end
        local values = { v2, v3, v4, q, m3, m4, Rect(0, 0, 1, 1), AnimationCurve(), Gradient() }
        local operators = {
            function(a, b) return a + b end, function(a, b) return a - b end, function(a, b) return a * b end,
            function(a, b) return a / b end, function(a) return -a end, function(a, b) return a == b end,
            function(a, b) return a < b end, function(a, b) return a <= b end, function(a) return tostring(a) end,
            function(a, b) return a .. b end, function(a) return #a end,
        }
        for _, value in ipairs(values) do
            for _, op in ipairs(operators) do
                for i = 1, count do
                    try(op, value, pool[i])
                    try(op, pool[i], value)
                end
            end
        end
        for _, accessor in ipairs({ m3.get, m3.set, m3.row, m3.column, m3.setRow, m3.setColumn,
                                    m4.get, m4.set, m4.row, m4.column, m4.setRow, m4.setColumn,
                                    q.fromEulerAngles, q.getEulerAngles, q.fromAxes }) do
            for _, self in ipairs({ m3, m4, q }) do
                for i = 1, count do try(accessor, self, pool[i]) end
                for i = 1, smallCount do
                    for j = 1, smallCount do try(accessor, self, small[i], small[j], 1) end
                end
            end
        end
        assert(calls > 5000, "the sweep ran: " .. calls)
    )lua"), "");
}

}  // namespace LuaMathBindingTest_cpp
