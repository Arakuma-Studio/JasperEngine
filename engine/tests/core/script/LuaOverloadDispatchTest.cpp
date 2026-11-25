// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LuaBridge's call paths (docs/specs/S92-htmlui-render-cost.md R24-R27, engine/libs/luabridge3/PATCHES.md):
// - CallsMatchStockLuaBridge: every kind of call, right and wrong, against overload sets of every
//   parameter kind (the JasperArcade port's matrix: 32 Lua values, wrong self, bodies that raise or
//   throw, C-function overloads, constructors, a coroutine, arity skips). Each result and error
//   message is compared with fixtures/luabridge/overload-dispatch.txt, captured from stock LuaBridge
//   (the header before S92) built by g++ 13 and clang++ 18. A message naming one of several failing
//   arguments depends on the compiler's order of evaluating a call's arguments (g++ right to left,
//   clang left to right): those lines carry a [gcc] and a [clang] variant. The file is not
//   regenerated: it is what stock did, except the 22 "nil self" calls, which stock ran on a null
//   object and which now raise the bad-self error (PATCHES.md, change 6).
// - LaterOverloadBuildsNoErrorString: a call reaching a later overload makes no Lua string.
// - NoLeak*: no C++ argument is left undestroyed by an error (lua_error is a longjmp). The argument
//   type counts its live instances; a 64-character text is decoded on each side of the failing
//   parameter, so whichever order the compiler decodes in, one of them is live when it fails.

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace luaoverloaddispatch {

    struct Vec2 { float x = 0, y = 0; Vec2() {} Vec2(float a, float b) : x(a), y(b) {} };
    struct Vec3 { float x = 0, y = 0, z = 0; Vec3() {} Vec3(float a, float b, float c) : x(a), y(b), z(c) {} };
    struct Vec4 {
        float x = 0, y = 0, z = 0, w = 0;
        Vec4() {}
        Vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
        Vec4(const Vec2&, float a, float b) : x(a), y(b) {}
        Vec4(const Vec3& v, float a) : x(v.x), y(v.y), z(v.z), w(a) {}
        Vec4(const Vec4& v) = default;
    };
    struct Base { virtual ~Base() {} std::string who() const { return "base"; } };
    struct Derived : Base {};
    struct Other {};
    struct Shared : std::enable_shared_from_this<Shared> { int v = 7; };
    enum class Mode { A, B };

    // An argument whose destructor must run: a Lua string decoded into a C++ object that counts
    // its live instances
    struct TrackedText {
        static int live;
        std::string text;
        explicit TrackedText(std::string value) : text(std::move(value)) { ++live; }
        TrackedText(const TrackedText& other) : text(other.text) { ++live; }
        TrackedText(TrackedText&& other) noexcept : text(std::move(other.text)) { ++live; }
        TrackedText& operator=(const TrackedText&) = default;
        TrackedText& operator=(TrackedText&&) = default;
        ~TrackedText() { --live; }
    };
    int TrackedText::live = 0;

}

namespace luabridge {

    // as the engine's EnumWrapper (engine/core/script/LuaBridgeAddon.h)
    template <> struct Stack<luaoverloaddispatch::Mode> {
        static Result push(lua_State* L, luaoverloaddispatch::Mode value) { lua_pushinteger(L, static_cast<lua_Integer>(value)); return {}; }
        static TypeResult<luaoverloaddispatch::Mode> get(lua_State* L, int index) { return static_cast<luaoverloaddispatch::Mode>(lua_tointeger(L, index)); }
        static bool isInstance(lua_State* L, int index) { return lua_type(L, index) == LUA_TNUMBER; }
    };

    template <> struct Stack<luaoverloaddispatch::TrackedText> {
        static Result push(lua_State* L, const luaoverloaddispatch::TrackedText& value) { lua_pushlstring(L, value.text.data(), value.text.size()); return {}; }
        static TypeResult<luaoverloaddispatch::TrackedText> get(lua_State* L, int index) {
            if (lua_type(L, index) != LUA_TSTRING)
                return makeErrorCode(ErrorCode::InvalidTypeCast);
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            return luaoverloaddispatch::TrackedText(std::string(text, length));
        }
        static bool isInstance(lua_State* L, int index) { return lua_type(L, index) == LUA_TSTRING; }
    };

}

namespace luaoverloaddispatch {

    std::string lastApplied;
    int multiCalls = 0;

    struct Mesh {
        void setU(const std::string& n, const Vec4& v) { lastApplied = "vec4 " + n + " " + std::to_string(v.x); }
        void setU(const std::string& n, const Vec3& v) { lastApplied = "vec3 " + n + " " + std::to_string(v.x); }
        void setU(const std::string& n, const Vec2& v) { lastApplied = "vec2 " + n + " " + std::to_string(v.x); }
        void setU(const std::string& n, float v) { lastApplied = "float " + n + " " + std::to_string(v); }
        std::string pickA(int v) { return "int " + std::to_string(v); }
        std::string pickA(float v) { return "float " + std::to_string(v); }
        std::string pickA(const std::string& v) { return "string " + v; }
        std::string pickB(bool v) { return std::string("bool ") + (v ? "t" : "f"); }
        std::string pickB(float v) { return "float " + std::to_string(v); }
        std::string pickC(const std::string& v) { return "string " + v; }
        std::string pickC(int v) { return "int " + std::to_string(v); }
        std::string pickD(float v) { return "float " + std::to_string(v); }
        std::string pickD(const char* v) { return std::string("cstr ") + (v ? v : "(null)"); }
        std::string pickE(Vec3* v) { return v ? "vec3* set" : "vec3* null"; }
        std::string pickE(int v) { return "int " + std::to_string(v); }
        std::string pickF(const Vec3& v) { return "vec3& " + std::to_string(v.x); }
        std::string pickF(const Vec4& v) { return "vec4& " + std::to_string(v.x); }
        std::string pickG(Base* b) { return b ? b->who() : "base null"; }
        std::string pickG(Other*) { return "other"; }
        std::string pickH(std::shared_ptr<Shared> s) { return s ? "shared " + std::to_string(s->v) : "shared null"; }
        std::string pickH(int v) { return "int " + std::to_string(v); }
        std::string pickI(std::vector<float> v) { return "vector " + std::to_string(v.size()); }
        std::string pickI(float v) { return "float " + std::to_string(v); }
        std::string pickI(luabridge::LuaRef r) { return std::string("ref ") + lua_typename(r.state(), r.type()); }
        std::string pickJ(std::function<int(int)> f) { return "function " + std::to_string(f(1)); }
        std::string pickJ(int v) { return "int " + std::to_string(v); }
        std::string pickK(Mode m) { return "mode " + std::to_string(static_cast<int>(m)); }
        std::string pickK(const std::string& s) { return "string " + s; }
        std::string pickL(float a, const Vec3& b) { return "float,vec3 " + std::to_string(a + b.x); }
        std::string pickL(const Vec3& a, float b) { return "vec3,float " + std::to_string(a.x + b); }
        std::string pickL(const Vec3&, const Vec3&, float c = 0) { return "vec3,vec3,float " + std::to_string(c); }
        std::string pickM(std::optional<int> v) { return v ? "opt " + std::to_string(*v) : "opt none"; }
        std::string pickM(const Vec3&) { return "vec3"; }
        std::string pickN(long long v) { return "long long " + std::to_string(v); }
        std::string pickN(double v) { return "double " + std::to_string(v); }
        std::string pickO(unsigned char v) { return "uchar " + std::to_string(v); }
        std::string pickO(float v) { return "float " + std::to_string(v); }
        std::string thrower(int v) { if (v < 100) throw std::runtime_error("body threw"); return "int ok"; }
        std::string thrower(float) { return "float ran after the throw"; }
        std::string allThrow(int) { throw std::runtime_error("first threw"); }
        std::string allThrow(float) { throw std::runtime_error("second threw"); }
        std::string getConst(int) const { return "const int"; }
        std::string getConst(const std::string&) { return "non-const string"; }
        int rawC(lua_State* L) { lua_pushliteral(L, "member cfunction"); return 1; }
        std::string rawC(int v) { return "int " + std::to_string(v); }
        std::string single(const std::string& n, const Vec3& v) { return "single " + n + std::to_string(v.x); }
        std::string singleRev(const Vec3& v, const std::string& n) { return "singleRev " + n + std::to_string(v.x); }
        std::string singleNum(const std::string& n, float f) { return "singleNum " + n + std::to_string(f); }
        std::string throws(const std::string& n) { throw std::runtime_error("threw with " + n); }
        std::string throwsSet(const std::string& n, int) { throw std::runtime_error("set threw with " + n); }
        std::string throwsSet(const std::string& n, const Vec3&) { throw std::runtime_error("set threw vec3 " + n); }
        std::string vecs(std::vector<Vec3> v, const std::string& n) { return "vecs " + std::to_string(v.size()) + n; }
        std::string ptrConst(Vec3* v, const std::string& n) { return std::string(v ? "ptr " : "null ") + n; }
    };

    std::string freeA(int v) { return "free int " + std::to_string(v); }
    std::string freeA(const Vec3& v) { return "free vec3 " + std::to_string(v.x); }
    std::string freeA(float v, float w) { return "free float,float " + std::to_string(v + w); }
    int freeC(lua_State* L) { lua_pushfstring(L, "free cfunction nargs=%d", lua_gettop(L)); return 1; }
    std::string freeB(int v) { return "free int " + std::to_string(v); }

    // The leak cases: the 64-character text on each side of the parameter that fails
    struct Material {
        std::string setShaderUniform(const TrackedText& n, const Vec4&) { return "vec4 " + n.text; }
        std::string setShaderUniform(const TrackedText& n, const Vec3&) { return "vec3 " + n.text; }
        std::string setShaderUniform(const TrackedText& n, const Vec2&) { return "vec2 " + n.text; }
        std::string setShaderUniform(const TrackedText& n, float) { return "float " + n.text; }
        std::string setUniformLast(const Vec4&, const TrackedText& n) { return "vec4 " + n.text; }
        std::string setUniformLast(const Vec3&, const TrackedText& n) { return "vec3 " + n.text; }
        std::string setUniformLast(float, const TrackedText& n) { return "float " + n.text; }
        std::string single(const TrackedText& n, const Vec3&) { return "single " + n.text; }
        std::string singleLast(const Vec3&, const TrackedText& n) { return "singleLast " + n.text; }
        std::string throws(const TrackedText& n) { throw std::runtime_error("threw with " + n.text); }
        std::string throwsSet(const TrackedText& n, int) { throw std::runtime_error("set threw with " + n.text); }
        std::string throwsSet(const TrackedText& n, float) { throw std::runtime_error("set threw float " + n.text); }
    };

    // The R24 case without any argument that holds memory: what a miss costs in Lua strings
    struct ShaderUniforms {
        float last = 0;
        void setShaderUniform(const std::string&, const Vec4& v) { last = v.x; }
        void setShaderUniform(const std::string&, const Vec3& v) { last = v.x; }
        void setShaderUniform(const std::string&, const Vec2& v) { last = v.x; }
        void setShaderUniform(const std::string&, float v) { last = v; }
    };

    void bind(lua_State* L) {
        using luabridge::overload;
        using luabridge::constOverload;
        using luabridge::nonConstOverload;
        luabridge::getGlobalNamespace(L)
            .beginClass<Vec2>("Vec2").addConstructor<void(), void(float, float)>().addProperty("x", &Vec2::x, &Vec2::x).endClass()
            .beginClass<Vec3>("Vec3").addConstructor<void(), void(float, float, float)>().addProperty("x", &Vec3::x, &Vec3::x).endClass()
            .beginClass<Vec4>("Vec4")
                .addConstructor<void(), void(float, float, float, float), void(Vec2, float, float), void(Vec3, float), void(Vec4)>()
                .addProperty("x", &Vec4::x, &Vec4::x)
            .endClass()
            .beginClass<Base>("Base").addConstructor<void()>().addFunction("who", &Base::who).endClass()
            .deriveClass<Derived, Base>("Derived").addConstructor<void()>().endClass()
            .beginClass<Other>("Other").addConstructor<void()>().endClass()
            .beginClass<Shared>("Shared").addProperty("v", &Shared::v, &Shared::v).endClass()
            .beginClass<Mesh>("Mesh")
                .addConstructor<void()>()
                .addFunction("setU",
                    overload<const std::string&, const Vec4&>(&Mesh::setU), overload<const std::string&, const Vec3&>(&Mesh::setU),
                    overload<const std::string&, const Vec2&>(&Mesh::setU), overload<const std::string&, float>(&Mesh::setU))
                .addFunction("pickA", overload<int>(&Mesh::pickA), overload<float>(&Mesh::pickA), overload<const std::string&>(&Mesh::pickA))
                .addFunction("pickB", overload<bool>(&Mesh::pickB), overload<float>(&Mesh::pickB))
                .addFunction("pickC", overload<const std::string&>(&Mesh::pickC), overload<int>(&Mesh::pickC))
                .addFunction("pickD", overload<float>(&Mesh::pickD), overload<const char*>(&Mesh::pickD))
                .addFunction("pickE", overload<Vec3*>(&Mesh::pickE), overload<int>(&Mesh::pickE))
                .addFunction("pickF", overload<const Vec3&>(&Mesh::pickF), overload<const Vec4&>(&Mesh::pickF))
                .addFunction("pickG", overload<Base*>(&Mesh::pickG), overload<Other*>(&Mesh::pickG))
                .addFunction("pickH", overload<std::shared_ptr<Shared>>(&Mesh::pickH), overload<int>(&Mesh::pickH))
                .addFunction("pickI", overload<std::vector<float>>(&Mesh::pickI), overload<float>(&Mesh::pickI), overload<luabridge::LuaRef>(&Mesh::pickI))
                .addFunction("pickJ", overload<std::function<int(int)>>(&Mesh::pickJ), overload<int>(&Mesh::pickJ))
                .addFunction("pickK", overload<Mode>(&Mesh::pickK), overload<const std::string&>(&Mesh::pickK))
                .addFunction("pickL", overload<float, const Vec3&>(&Mesh::pickL), overload<const Vec3&, float>(&Mesh::pickL),
                    overload<const Vec3&, const Vec3&, float>(&Mesh::pickL))
                .addFunction("pickM", overload<std::optional<int>>(&Mesh::pickM), overload<const Vec3&>(&Mesh::pickM))
                .addFunction("pickN", overload<long long>(&Mesh::pickN), overload<double>(&Mesh::pickN))
                .addFunction("pickO", overload<unsigned char>(&Mesh::pickO), overload<float>(&Mesh::pickO))
                .addFunction("thrower", overload<int>(&Mesh::thrower), overload<float>(&Mesh::thrower))
                .addFunction("allThrow", overload<int>(&Mesh::allThrow), overload<float>(&Mesh::allThrow))
                .addFunction("getConst", constOverload<int>(&Mesh::getConst), nonConstOverload<const std::string&>(&Mesh::getConst))
                .addFunction("rawC", overload<lua_State*>(&Mesh::rawC), overload<int>(&Mesh::rawC))
                .addFunction("single", &Mesh::single)
                .addFunction("singleRev", &Mesh::singleRev)
                .addFunction("singleNum", &Mesh::singleNum)
                .addFunction("throws", &Mesh::throws)
                .addFunction("throwsSet", overload<const std::string&, int>(&Mesh::throwsSet), overload<const std::string&, const Vec3&>(&Mesh::throwsSet))
                .addFunction("vecs", &Mesh::vecs)
                .addFunction("ptrConst", &Mesh::ptrConst)
                // proxies: a lambda and a free function taking the object, a lambda raising from its body, a lambda with lua_State*
                .addFunction("proxy",
                    +[](Mesh*, int v) { return "proxy int " + std::to_string(v); },
                    +[](const Mesh*, const Vec3& v) { return "proxy vec3 " + std::to_string(v.x); },
                    +[](Mesh&, const std::string& s, float f) { return "proxy string,float " + s + std::to_string(f); })
                .addFunction("raising",
                    +[](Mesh*, int, lua_State* L) -> int { return luaL_error(L, "lua_error from the body"); },
                    +[](Mesh*, float) -> int { return 42; })
                .addFunction("raisingAll",
                    +[](Mesh*, int, lua_State* L) -> int { return luaL_error(L, "lua_error one"); },
                    +[](Mesh*, float, lua_State* L) -> int { return luaL_error(L, "lua_error two"); })
                .addFunction("multi",
                    +[](Mesh*, int, lua_State* L) -> int { ++multiCalls; lua_pushinteger(L, multiCalls); return 1; },
                    +[](Mesh*, const Vec3&, lua_State* L) -> int { ++multiCalls; lua_pushinteger(L, multiCalls); return 1; })
                .addStaticFunction("stat",
                    overload<int>(&freeA), overload<const Vec3&>(&freeA), overload<float, float>(&freeA), &freeC)
            .endClass()
            .beginClass<Material>("Material")
                .addConstructor<void()>()
                .addFunction("setShaderUniform",
                    overload<const TrackedText&, const Vec4&>(&Material::setShaderUniform), overload<const TrackedText&, const Vec3&>(&Material::setShaderUniform),
                    overload<const TrackedText&, const Vec2&>(&Material::setShaderUniform), overload<const TrackedText&, float>(&Material::setShaderUniform))
                .addFunction("setUniformLast",
                    overload<const Vec4&, const TrackedText&>(&Material::setUniformLast), overload<const Vec3&, const TrackedText&>(&Material::setUniformLast),
                    overload<float, const TrackedText&>(&Material::setUniformLast))
                .addFunction("single", &Material::single)
                .addFunction("singleLast", &Material::singleLast)
                .addFunction("throws", &Material::throws)
                .addFunction("throwsSet", overload<const TrackedText&, int>(&Material::throwsSet), overload<const TrackedText&, float>(&Material::throwsSet))
            .endClass()
            .beginClass<ShaderUniforms>("ShaderUniforms")
                .addConstructor<void()>()
                .addFunction("setShaderUniform",
                    overload<const std::string&, const Vec4&>(&ShaderUniforms::setShaderUniform), overload<const std::string&, const Vec3&>(&ShaderUniforms::setShaderUniform),
                    overload<const std::string&, const Vec2&>(&ShaderUniforms::setShaderUniform), overload<const std::string&, float>(&ShaderUniforms::setShaderUniform))
            .endClass()
            .beginNamespace("ns")
                .addFunction("f", overload<int>(&freeA), overload<const Vec3&>(&freeA), overload<float, float>(&freeA))
                .addFunction("g", &freeC, &freeB)
                .addFunction("h", &freeB, &freeC)
            .endNamespace();
        luabridge::push(L, Mode::B).throw_on_error();
        lua_setglobal(L, "MODE_B");
        luabridge::push(L, std::make_shared<Shared>()).throw_on_error();
        lua_setglobal(L, "SHARED");
        static const Mesh constMesh;
        luabridge::push(L, &constMesh).throw_on_error();
        lua_setglobal(L, "CONST_MESH");
        lua_register(L, "constVec3", +[](lua_State* state) -> int {
            static const Vec3 value(5, 6, 7);
            luabridge::push(state, &value).throw_on_error();
            return 1;
        });
        lua_register(L, "lastU", +[](lua_State* state) -> int { lua_pushstring(state, lastApplied.c_str()); return 1; });
    }

    // Every call: pcall plus whatever came back, one emit() line per call
    const char* const kBehaviour = R"LUA(
local function show(...)
    local t = {}
    for i = 1, select('#', ...) do
        local v = select(i, ...)
        t[i] = type(v) == 'userdata' and 'userdata' or tostring(v)
    end
    return table.concat(t, ' | ')
end
local function call(label, f, ...)
    emit(label .. ' => ' .. show(pcall(f, ...)))
end
local m, v2, v3, v4, b, d, o = Mesh(), Vec2(1, 2), Vec3(1, 2, 3), Vec4(1, 2, 3, 4), Base(), Derived(), Other()
local long = string.rep('u', 64)
local args = {
    {'int', 7}, {'float1.0', 1.0}, {'float1.5', 1.5}, {'big', 2^40}, {'huge', 1e300}, {'neg', -3}, {'inf', math.huge}, {'nan', 0/0},
    {'str', 'abc'}, {'numstr', '12'}, {'empty', ''}, {'long', long}, {'true', true}, {'false', false}, {'nil', nil},
    {'table', {}}, {'tablef', {1.5, 2.5}}, {'tablebad', {'x'}}, {'func', function(x) return x + 1 end}, {'badfunc', function() return 'x' end},
    {'v2', v2}, {'v3', v3}, {'v4', v4}, {'base', b}, {'derived', d}, {'other', o}, {'shared', SHARED}, {'mode', MODE_B},
    {'mesh', m}, {'constmesh', CONST_MESH}, {'coroutine', coroutine.create(function() end)}, {'class', Vec3},
}
local methods = {'pickA','pickB','pickC','pickD','pickE','pickF','pickG','pickH','pickI','pickJ','pickK','pickM','pickN','pickO',
    'thrower','allThrow','getConst','rawC','proxy','raising','raisingAll','multi'}
for _, name in ipairs(methods) do
    emit('--- ' .. name)
    call(name .. '()', m[name], m)
    for i = 1, #args do
        call(name .. '(' .. args[i][1] .. ')', m[name], m, args[i][2])
    end
    call(name .. '(int,int)', m[name], m, 1, 2)
    call('wrong self ' .. name, m[name], 3, 1)
    call('nil self ' .. name, m[name], nil, 1)
    call('other self ' .. name, m[name], v3, 1)
    emit(name .. ' const object => ' .. show(pcall(function() return CONST_MESH[name](CONST_MESH, 1) end)))
    emit(name .. ' const object string => ' .. show(pcall(function() return CONST_MESH[name](CONST_MESH, 'a') end)))
end
emit('--- setU (the R66 set)')
for i = 1, #args do
    call('setU(name,' .. args[i][1] .. ')', m.setU, m, 'u', args[i][2])
    emit(' applied: ' .. lastU())
    call('setU(' .. args[i][1] .. ',v4)', m.setU, m, args[i][2], v4)
    emit(' applied: ' .. lastU())
    call('setU(' .. args[i][1] .. ',1.5)', m.setU, m, args[i][2], 1.5)
    emit(' applied: ' .. lastU())
end
emit('--- two and three arguments')
for i = 1, #args do
    for j = 1, #args, 4 do
        call('pickL(' .. args[i][1] .. ',' .. args[j][1] .. ')', m.pickL, m, args[i][2], args[j][2])
        call('pickL(v3,' .. args[i][1] .. ',' .. args[j][1] .. ')', m.pickL, m, v3, args[i][2], args[j][2])
        call('proxy(' .. args[i][1] .. ',' .. args[j][1] .. ')', m.proxy, m, args[i][2], args[j][2])
    end
end
emit('--- static and namespace functions, C functions in a set')
for i = 1, #args do
    local a = args[i]
    call('stat(' .. a[1] .. ')', Mesh.stat, a[2])
    call('stat(' .. a[1] .. ',1.5)', Mesh.stat, a[2], 1.5)
    call('ns.f(' .. a[1] .. ')', ns.f, a[2])
    call('ns.f(' .. a[1] .. ',1)', ns.f, a[2], 1)
    call('ns.g(' .. a[1] .. ')', ns.g, a[2])
    call('ns.h(' .. a[1] .. ')', ns.h, a[2])
end
call('stat()', Mesh.stat)
call('ns.f()', ns.f)
call('ns.f(1,2,3)', ns.f, 1, 2, 3)
emit('--- constructors (placement)')
for i = 1, #args do
    local a = args[i]
    local function field(...) local ok, r = pcall(Vec4, ...); if ok then return 'ok ' .. tostring(r.x) end return tostring(r) end
    emit('Vec4(' .. a[1] .. ') => ' .. field(a[2]))
    emit('Vec4(' .. a[1] .. ',1) => ' .. field(a[2], 1))
    emit('Vec4(' .. a[1] .. ',1,2) => ' .. field(a[2], 1, 2))
    emit('Vec4(' .. a[1] .. ',1,2,3) => ' .. field(a[2], 1, 2, 3))
    emit('Vec3(' .. a[1] .. ',2,3) => ' .. show(pcall(Vec3, a[2], 2, 3)))
end
emit('Vec4() => ' .. tostring(Vec4().x))
emit('Vec4(1,2,3,4,5) => ' .. show(pcall(Vec4, 1, 2, 3, 4, 5)))
emit('--- non-overloaded, throwing, containers (S92 R26)')
local CV3 = constVec3()
for i = 1, #args do
    call('single(' .. args[i][1] .. ',v3)', m.single, m, args[i][2], v3)
    call('single(long,' .. args[i][1] .. ')', m.single, m, long, args[i][2])
    call('singleRev(' .. args[i][1] .. ',long)', m.singleRev, m, args[i][2], long)
    call('singleNum(long,' .. args[i][1] .. ')', m.singleNum, m, long, args[i][2])
    call('throws(' .. args[i][1] .. ')', m.throws, m, args[i][2])
    call('throwsSet(long,' .. args[i][1] .. ')', m.throwsSet, m, long, args[i][2])
    call('vecs({v3,' .. args[i][1] .. '},long)', m.vecs, m, {v3, args[i][2]}, long)
    call('ptrConst(' .. args[i][1] .. ',long)', m.ptrConst, m, args[i][2], long)
end
call('ptrConst(constv3,long)', m.ptrConst, m, CV3, long)
call('setU(long,constv3)', m.setU, m, long, CV3)
call('single(long,constv3)', m.single, m, long, CV3)
call('single(long)', m.single, m, long)
call('singleRev(v3)', m.singleRev, m, v3)
call('vecs()', m.vecs, m)
emit('--- metamethod-less errors keep their names; error objects that are not strings')
emit('method name in the message => ' .. show(pcall(function() return m:pickF(1) end)))
emit('stat name in the message => ' .. show(pcall(function() return Mesh.stat({}, {}, {}) end)))
emit('ctor name in the message => ' .. show(pcall(function() return Vec4({}, {}, {}, {}, {}) end)))
emit('--- stack discipline: results count, nesting')
emit('multi returns => ' .. show(m:multi(1)))
emit('nested => ' .. show(m:pickA(m:pickN(3) and 1), m:pickC(m:pickA(2.5))))
emit('in a coroutine => ' .. show(coroutine.wrap(function() return m:pickF(v4), pcall(m.pickF, m, 1) end)()))
emit('deep args => ' .. show(pcall(function() return m:setU('a', 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0) end)))
)LUA";

    // Lua's allocator, counting the strings it makes (Lua 5.4 passes the new object's type as osize)
    struct LuaAllocations {
        long strings = 0;
    };

    void* countingAllocator(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
        if (nsize == 0) {
            std::free(ptr);
            return nullptr;
        }
        if (ptr == nullptr && (osize & 0x0F) == LUA_TSTRING)
            static_cast<LuaAllocations*>(ud)->strings++;
        return std::realloc(ptr, nsize);
    }

    struct LuaState {
        LuaAllocations allocations;
        std::vector<std::string> lines;
        lua_State* L = nullptr;

        LuaState() {
            L = lua_newstate(countingAllocator, &allocations);
            luaL_openlibs(L);
            bind(L);
            lua_pushlightuserdata(L, &lines);
            lua_pushcclosure(L, +[](lua_State* state) -> int {
                auto* out = static_cast<std::vector<std::string>*>(lua_touserdata(state, lua_upvalueindex(1)));
                std::size_t length = 0;
                const char* text = luaL_checklstring(state, 1, &length);
                std::string line;
                for (std::size_t i = 0; i < length; i++) {
                    if (text[i] == '\\') line += "\\\\";
                    else if (text[i] == '\n') line += "\\n";
                    else line += text[i];
                }
                out->push_back(std::move(line));
                return 0;
            }, 1);
            lua_setglobal(L, "emit");
        }
        ~LuaState() { lua_close(L); }
        LuaState(const LuaState&) = delete;
        LuaState& operator=(const LuaState&) = delete;

        // "" on success, else the error message
        std::string run(const std::string& code) {
            if (luaL_loadbuffer(L, code.data(), code.size(), "dispatch") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }
    };

    // The expected lines for this compiler: a [gcc] line and the [clang] line after it are the two
    // evaluation orders of one call; another compiler may give either
    std::vector<std::vector<std::string>> readExpected(const std::string& path) {
#if defined(__clang__)
        const char* const mine = "[clang] ";
#elif defined(__GNUC__)
        const char* const mine = "[gcc] ";
#else
        const char* const mine = nullptr;
#endif
        std::vector<std::vector<std::string>> expected;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            const bool gcc = line.rfind("[gcc] ", 0) == 0;
            const bool clang = line.rfind("[clang] ", 0) == 0;
            if (!gcc && !clang) {
                expected.push_back({line});
                continue;
            }
            const std::string text = line.substr(gcc ? 6 : 8);
            if (gcc)
                expected.emplace_back();
            if (mine == nullptr || line.rfind(mine, 0) == 0)
                expected.back().push_back(text);
        }
        return expected;
    }

    constexpr int kLeakCalls = 2000;

    // A NaN as the C runtime prints it (glibc "-nan", MSVC "-nan(ind)", Apple "nan") reduced to
    // "nan": the fixture records what dispatch did, not the runtime's spelling of the value
    std::string canonicalNan(const std::string& line) {
        static const std::regex nan("-?nan(\\([a-z]*\\))?");
        return std::regex_replace(line, nan, "nan");
    }

    std::string longName() {
        return std::string(64, 'u');
    }

TEST(LuaOverloadDispatch, CallsMatchStockLuaBridge) {
    LuaState state;
    ASSERT_EQ(state.run(kBehaviour), "");

    const std::vector<std::vector<std::string>> expected = readExpected(JASPER_TEST_REPO_DIR "/engine/tests/core/fixtures/luabridge/overload-dispatch.txt");
    ASSERT_GT(expected.size(), 2400u) << "the fixture is missing or cut";
    EXPECT_EQ(state.lines.size(), expected.size());

    int mismatches = 0;
    for (std::size_t i = 0; i < state.lines.size() && i < expected.size(); i++) {
        bool found = false;
        const std::string got = canonicalNan(state.lines[i]);
        for (const std::string& candidate : expected[i])
            found = found || canonicalNan(candidate) == got;
        if (!found && ++mismatches <= 20)
            ADD_FAILURE() << "line " << (i + 1) << "\n  got:      " << state.lines[i] << "\n  expected: " << (expected[i].empty() ? std::string("(none)") : expected[i][0]);
    }
    EXPECT_EQ(mismatches, 0);
}

TEST(LuaOverloadDispatch, LaterOverloadBuildsNoErrorString) {
    LuaState state;
    const char* const code = R"LUA(
local u = ShaderUniforms()
for i = 1, 1000 do u:setShaderUniform('u', 0.5) end
assert(u ~= nil)
)LUA";
    ASSERT_EQ(state.run(code), "");   // warm-up: the chunk's own strings

    const long before = state.allocations.strings;
    ASSERT_EQ(state.run(code), "");
    // stock LuaBridge formats a "bad argument" message for each of the three misses
    EXPECT_EQ(state.allocations.strings - before, 0);
}

TEST(LuaOverloadDispatch, NoLeakOnMiss) {
    LuaState state;
    TrackedText::live = 0;
    // overloads 1 to 3 miss before the float one; the check skips the userdata ones, and a
    // decode that fails is raised after the text is destroyed
    ASSERT_EQ(state.run("local m, n = Material(), string.rep('u', 64)\n"
                        "for i = 1, " + std::to_string(kLeakCalls) + " do\n"
                        "  assert(m:setShaderUniform(n, 1.5) == 'float ' .. n)\n"
                        "  assert(m:setUniformLast(1.5, n) == 'float ' .. n)\n"
                        "  assert(m:setShaderUniform(n, Vec2(1, 2)) == 'vec2 ' .. n)\n"
                        "end"), "");
    EXPECT_EQ(TrackedText::live, 0);
}

TEST(LuaOverloadDispatch, NoLeakWhenEveryOverloadFails) {
    LuaState state;
    TrackedText::live = 0;
    ASSERT_EQ(state.run("local m, n, o = Material(), string.rep('u', 64), Other()\n"
                        "for i = 1, " + std::to_string(kLeakCalls) + " do\n"
                        "  assert(not pcall(m.setShaderUniform, m, n, o))\n"
                        "  assert(not pcall(m.setUniformLast, m, o, n))\n"
                        "end"), "");
    EXPECT_EQ(TrackedText::live, 0);

    // the message is still stock's
    const std::string error = state.run("local m = Material()\nm:setShaderUniform('" + longName() + "', Other())");
    EXPECT_EQ(error, "All 4 overloads of setShaderUniform returned an error:\n"
                     "1: bad argument #3 to '?' (Vec4 expected, got Other)\n"
                     "2: bad argument #3 to '?' (Vec3 expected, got Other)\n"
                     "3: bad argument #3 to '?' (Vec2 expected, got Other)\n"
                     "4: [string \"dispatch\"]:2: Error decoding argument #2: The lua object can't be cast to desired type");
}

TEST(LuaOverloadDispatch, NoLeakOnWrongCallWithoutOverloads) {
    LuaState state;
    TrackedText::live = 0;
    ASSERT_EQ(state.run("local m, n, v4 = Material(), string.rep('u', 64), Vec4(1, 2, 3, 4)\n"
                        "for i = 1, " + std::to_string(kLeakCalls) + " do\n"
                        "  assert(not pcall(m.single, m, n, v4))\n"
                        "  assert(not pcall(m.singleLast, m, v4, n))\n"
                        "  assert(not pcall(m.single, m, n))\n"
                        "end"), "");
    EXPECT_EQ(TrackedText::live, 0);

    const std::string error = state.run("local m = Material()\nm:single('" + longName() + "', Vec4(1, 2, 3, 4))");
    EXPECT_EQ(error, "[string \"dispatch\"]:2: bad argument #2 to 'single' (Vec3 expected, got Vec4)");
}

TEST(LuaOverloadDispatch, NoLeakWhenABodyThrows) {
    LuaState state;
    TrackedText::live = 0;
    ASSERT_EQ(state.run("local m, n = Material(), string.rep('u', 64)\n"
                        "for i = 1, " + std::to_string(kLeakCalls) + " do\n"
                        "  local ok, error = pcall(m.throws, m, n)\n"
                        "  assert(not ok and error:find('threw with ' .. n, 1, true), error)\n"
                        "  assert(not pcall(m.throwsSet, m, n, 1))\n"
                        "end"), "");
    EXPECT_EQ(TrackedText::live, 0);
    // a longjmp out of the catch block left the exception on the runtime's caught-exception stack
    EXPECT_EQ(std::current_exception(), nullptr);
}

}
