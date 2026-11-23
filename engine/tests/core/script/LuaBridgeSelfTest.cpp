// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The object of a member call through LuaBridge (engine/libs/luabridge3/PATCHES.md, change 6; S95
// REQ-2): nil, or a class's metatable reached through debug.getmetatable, used to be read as an
// object (a null pointer, or the metatable's null userdata pointer) and crashed the process. Every
// member path now raises the bad-self error a value of another type raises: member functions (const
// and not), C-function members, functions and functors bound as members (pointer and reference),
// overloaded members, property getters and setters (data members, getter methods, proxies), and
// metamethods given nil as their first operand.

#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

namespace luabridgeselftest {

    struct Point {
        float x = 0;
        float y = 0;
        Point() {}
        Point(float a, float b) : x(a), y(b) {}
        float length() const { return x + y; }
        void scale(float factor) { x *= factor; y *= factor; }
        float dot(const Point& other) const { return x * other.x + y * other.y; }
        int raw(lua_State* L) { lua_pushnumber(L, x); return 1; }
        float over(float factor) const { return x * factor; }
        float over(const Point& other) const { return x + other.x; }
        float getY() const { return y; }
        void setY(float value) { y = value; }
        Point operator+(const Point& other) const { return Point(x + other.x, y + other.y); }
        bool operator<(const Point& other) const { return x < other.x; }
        std::string toString() const { return "Point"; }
    };

    void bind(lua_State* L) {
        using luabridge::overload;
        luabridge::getGlobalNamespace(L)
            .beginClass<Point>("Point")
                .addConstructor<void(), void(float, float)>()
                .addProperty("x", &Point::x, &Point::x)
                .addProperty("y", &Point::getY, &Point::setY)
                .addProperty("proxied",
                    +[](const Point* p) { return p->x; },
                    +[](Point* p, float value) { p->x = value; })
                .addFunction("length", &Point::length)
                .addFunction("scale", &Point::scale)
                .addFunction("dot", &Point::dot)
                .addFunction("raw", &Point::raw)
                .addFunction("over", overload<float>(&Point::over), overload<const Point&>(&Point::over))
                .addFunction("proxyPointer", +[](Point* p, float factor) { return p->x * factor; })
                .addFunction("proxyConstPointer", +[](const Point* p) { return p->x; })
                .addFunction("proxyReference", +[](const Point& p) { return p.x; })
                .addFunction("functor", [factor = 2.0f](const Point* p) { return p->x * factor; })
                .addFunction("__add", &Point::operator+)
                .addFunction("__lt", &Point::operator<)
                .addFunction("__tostring", &Point::toString)
            .endClass();
    }

    struct LuaState {
        lua_State* L = luaL_newstate();
        LuaState() { luaL_openlibs(L); bind(L); }
        ~LuaState() { lua_close(L); }
        LuaState(const LuaState&) = delete;
        LuaState& operator=(const LuaState&) = delete;

        // "" on success, else the error message
        std::string run(const std::string& code) {
            if (luaL_loadbuffer(L, code.data(), code.size(), "self") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }

        // the error of `code`, run as a function's body under pcall: "" when it did not raise
        std::string errorOf(const std::string& code) {
            const std::string error = run("local ok, e = pcall(function() " + code + " end)\n"
                                          "if ok then return end\nerror(tostring(e), 0)");
            return error;
        }
    };

TEST(LuaBridgeSelf, NilSelfRaisesInMemberFunctions) {
    LuaState state;
    for (const char* code : {"return Point.length(nil)", "Point.scale(nil, 2)", "return Point.dot(nil, Point(1, 2))",
                             "return Point.raw(nil)", "return Point.over(nil, 2)", "return Point.over(nil, Point())"}) {
        const std::string error = state.errorOf(code);
        EXPECT_NE(error.find("Point expected, got nil"), std::string::npos) << code << ": " << error;
    }
    EXPECT_EQ(state.errorOf("return Point.length(nil)"), "[string \"self\"]:1: bad argument #1 to 'length' (Point expected, got nil)");
    EXPECT_EQ(state.errorOf("local p = Point(1, 2) p.length(nil)"),
              "[string \"self\"]:1: bad argument #1 to 'length' (Point expected, got nil)");
    // the same message as for any other value that is not a Point
    EXPECT_EQ(state.errorOf("return Point.length(3)"), "[string \"self\"]:1: bad argument #1 to 'length' (Point expected, got number)");
}

TEST(LuaBridgeSelf, NilSelfRaisesInMembersBoundAsFunctions) {
    LuaState state;
    for (const char* code : {"return Point.proxyPointer(nil, 2)", "return Point.proxyConstPointer(nil)",
                             "return Point.functor(nil)"}) {
        const std::string error = state.errorOf(code);
        EXPECT_NE(error.find("Point expected, got nil"), std::string::npos) << code << ": " << error;
    }
    // a reference parameter refused nil before: its error is unchanged
    EXPECT_NE(state.errorOf("return Point.proxyReference(nil)"), "");
    EXPECT_EQ(state.run("local p = Point(3, 4)\n"
                        "assert(p:proxyPointer(2) == 6 and p:proxyConstPointer() == 3 and p:functor() == 6 and p:proxyReference() == 3)"), "");
}

TEST(LuaBridgeSelf, NilSelfRaisesInPropertyGettersAndSetters) {
    LuaState state;
    // a script reaches the accessors through the class metatable's tables (debug library)
    ASSERT_EQ(state.run(R"LUA(
accessors = {}
local mt = debug.getmetatable(Point())
for _, t in pairs(mt) do
    if type(t) == 'table' then
        for name, f in pairs(t) do
            if type(name) == 'string' and type(f) == 'function' and (name == 'x' or name == 'y' or name == 'proxied') then
                accessors[#accessors + 1] = f
            end
        end
    end
end
assert(#accessors == 6, 'getters and setters of x, y and proxied: ' .. #accessors)
)LUA"), "");
    EXPECT_EQ(state.run(R"LUA(
for i, f in ipairs(accessors) do
    local ok, e = pcall(f, nil, 1)
    assert(not ok and tostring(e):find('Point expected, got nil', 1, true), i .. ': ' .. tostring(e))
end
)LUA"), "");
}

TEST(LuaBridgeSelf, NilFirstOperandOfAMetamethodRaises) {
    LuaState state;
    for (const char* code : {"return nil + Point(1, 2)", "return nil < Point(1, 2)"}) {
        const std::string error = state.errorOf(code);
        EXPECT_NE(error.find("Point expected, got nil"), std::string::npos) << code << ": " << error;
    }
    EXPECT_EQ(state.run("local p = Point(1, 2) + Point(3, 4)\nassert(p.x == 4 and Point(1, 0) < Point(2, 0))"), "");
}

TEST(LuaBridgeSelf, ClassMetatableIsNotAnObject) {
    LuaState state;
    ASSERT_EQ(state.run("mt = debug.getmetatable(Point(1, 2))\nassert(type(mt) == 'table')"), "");
    for (const char* code : {"return mt + Point(1, 2)", "return Point(1, 2) + mt", "return tostring(mt)",
                             "return Point.length(mt)", "return Point.proxyPointer(mt, 2)", "return Point.over(mt, 2)"}) {
        const std::string error = state.errorOf(code);
        EXPECT_NE(error.find("Point expected, got table"), std::string::npos) << code << ": " << error;
    }
}

// The engine's own bindings, the cases S95 WP-1 found
TEST(LuaBridgeSelf, EngineMembersRaiseOnNil) {
    lua_State* L = jasperengine::LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    for (const char* code : {"return Vector3.length(nil)", "return Vector3.dotProduct(nil, Vector3(1, 2, 3))",
                             "return nil + Vector3(1, 2, 3)", "return nil * Vector3(1, 2, 3)",
                             "return debug.getmetatable(Vector3(1, 2, 3)) + Vector3(1, 2, 3)"}) {
        const std::string chunk = std::string("local ok, e = pcall(function() ") + code + " end)\n"
                                  "if ok then return end\nerror(tostring(e), 0)";
        const bool raised = luaL_loadbuffer(L, chunk.data(), chunk.size(), "self") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK;
        const std::string error = raised && lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        if (raised)
            lua_pop(L, 1);
        EXPECT_TRUE(error.find("expected, got") != std::string::npos || error.find("attempt to") != std::string::npos)
            << code << ": " << (raised ? error : std::string("did not raise"));
    }
}

}
