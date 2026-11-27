// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// JLV1, the save value codec, and its JSON view (docs/specs/savegame.md R2, R20, R21; AC1-AC4).

#include "save/SaveValue.h"
#include "script/LuaBinding.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // __saveRoundTrip(v): v encoded and decoded again (an error raised on failure)
    int saveValueRoundTrip(lua_State* L){
        std::string payload;
        savegame::SaveError error;
        if (!savegame::encodeLua(L, 1, payload, error)) return luaL_error(L, "encode: %s", error.message.c_str());
        if (!savegame::decodeLua(L, payload, error)) return luaL_error(L, "decode: %s", error.message.c_str());
        return 1;
    }

    // __saveEncode(v): the payload as a Lua string, or nil, code, message
    int saveValueEncode(lua_State* L){
        std::string payload;
        savegame::SaveError error;
        if (!savegame::encodeLua(L, 1, payload, error)){
            lua_pushnil(L);
            lua_pushstring(L, savegame::errorCodeName(error.code));
            lua_pushlstring(L, error.message.data(), error.message.size());
            return 3;
        }
        lua_pushlstring(L, payload.data(), payload.size());
        return 1;
    }

    std::string runSaveValueLua(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        lua_register(L, "__saveRoundTrip", saveValueRoundTrip);
        lua_register(L, "__saveEncode", saveValueEncode);
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(SaveGameValue, RoundTripsEveryType){
    EXPECT_EQ(runSaveValueLua(R"(
        local function bits(f) return string.pack("<d", f) end
        local function same(a, b, path)
            if type(a) ~= type(b) then error(path .. ": " .. type(a) .. " came back as " .. type(b)) end
            if type(a) == "number" then
                if math.type(a) ~= math.type(b) then error(path .. ": " .. math.type(a) .. " came back as " .. math.type(b)) end
                if math.type(a) == "float" then
                    if bits(a) ~= bits(b) then error(path .. ": float bits changed") end
                elseif a ~= b then error(path .. ": " .. a .. " came back as " .. b) end
            elseif type(a) == "table" then
                for k, v in pairs(a) do
                    if math.type(k) == "integer" and b[k] == nil then error(path .. "[" .. k .. "] missing") end
                    same(v, b[k], path .. "[" .. tostring(k) .. "]")
                end
                for k in pairs(b) do if a[k] == nil then error(path .. ": extra key " .. tostring(k)) end end
            elseif type(a) == "userdata" then
                if rawequal(a, Json.null) then
                    if not rawequal(b, Json.null) then error(path .. ": Json.null changed") end
                else
                    if getmetatable(a) ~= getmetatable(b) then error(path .. ": the vector type changed") end
                    for _, c in ipairs({"x", "y", "z", "w"}) do
                        local ok, va = pcall(function() return a[c] end)
                        if ok and va ~= nil then
                            if string.pack("<f", va) ~= string.pack("<f", b[c]) then error(path .. "." .. c .. " changed") end
                        end
                    end
                end
            elseif a ~= b then error(path .. ": " .. tostring(a) .. " came back as " .. tostring(b)) end
        end
        local nanPayload = string.unpack("<d", string.pack("<I8", 0x7ff8000000000123))
        local value = {
            t = true, f = false,
            minInt = math.mininteger, maxInt = math.maxinteger, zero = 0,
            tenth = 0.1, negZero = -0.0, three = 3.0, inf = math.huge, ninf = -math.huge, nan = nanPayload,
            bytes = "a\0b\255\254c",
            nested = {list = {1, 2, {3, "four"}}, map = {x = 1, [7] = "seven", [-3] = "minus"}},
            sparse = {[1] = "a", [5] = "b"},
            v2 = Vector2(1.5, -2), v3 = Vector3(4, 0, 2), v4 = Vector4(1, 2, 3, 4), q = Quaternion(0.5, 0.5, 0.5, 0.5),
            null = Json.null,
            [1] = "first", [2] = 2.5,
        }
        local back = __saveRoundTrip(value)
        same(value, back, "$")
        assert(math.type(back.three) == "float" and math.type(back.zero) == "integer")
        assert(back.sparse[1] == "a" and back.sparse[5] == "b" and back.sparse["5"] == nil, "integer keys stay integers")
        assert(back.v3.x == 4 and back.v3.z == 2, "a Vector3 comes back as one")
        assert(__saveRoundTrip(42) == 42 and __saveRoundTrip("x") == "x", "a bare value")
    )"), "");
}

TEST(SaveGameValue, ErrorsNameThePath){
    // a named local, not the macro's argument: MSVC 14.44's preprocessor (Visual Studio 2022 17.14) mangles a raw
    // string holding \" in a macro's arguments (C2017, C3688)
    const char* lua = R"(
        local function check(value, expected)
            local payload, code, message = __saveEncode(value)
            assert(payload == nil, "should fail: " .. expected)
            assert(code == "encode", "code " .. tostring(code))
            assert(message == expected, "got '" .. tostring(message) .. "', expected '" .. expected .. "'")
        end
        check({player = {onHit = function() end}}, "cannot save a function at $.player.onHit")
        check({flags = {[true] = 1}}, "cannot save a boolean key at $.flags")
        local a = {}
        a.parent = a
        check({a = a}, "cycle at $.a.parent")
        local deep = {}
        local cursor = deep
        local path = "$"
        for i = 1, 64 do cursor.n = {}; cursor = cursor.n; path = path .. ".n" end
        check(deep, "nesting deeper than 64 at " .. path)
        local ok64 = {}
        cursor = ok64
        for i = 1, 63 do cursor.n = {}; cursor = cursor.n end
        assert(__saveEncode(ok64) ~= nil, "64 levels are allowed")
        check({items = {1, coroutine.create(function() end)}}, "cannot save a thread at $.items[2]")
        check({["a b"] = {f = print}}, "cannot save a function at $[\"a b\"].f")
        local shared = {1}
        assert(__saveEncode({shared, shared}) ~= nil, "a table reached twice is no cycle")
    )";
    EXPECT_EQ(runSaveValueLua(lua), "");
}

TEST(SaveGameValue, EqualTablesGiveEqualBytes){
    EXPECT_EQ(runSaveValueLua(R"(
        local a = {}
        a.zeta = 1; a.alpha = {3, 2, 1}; a[10] = "ten"; a[1] = "one"; a[2] = "two"; a.mid = Vector3(1, 2, 3)
        local b = {}
        b.mid = Vector3(1, 2, 3); b[2] = "two"; b[10] = "ten"; b.alpha = {}; b.alpha[3] = 1; b.alpha[2] = 2; b.alpha[1] = 3
        b[1] = "one"; b.zeta = 1
        assert(__saveEncode(a) == __saveEncode(b), "equal tables, equal bytes")
        assert(__saveEncode({1, 2}) ~= __saveEncode({1, 2.0}), "an integer is not a float")
    )"), "");
}

TEST(SaveGameValue, JsonViewIsABijection){
    const std::vector<std::string> cases = {
        R"({"gold":120,"level":"forest"})",
        R"([1,2,3])",
        R"({})",
        R"({"#3":"three","#-1":"minus","name":"x"})",
        R"({"##x":1,"$$y":2,"#1":true})",
        R"({"pos":{"$vec3":[4.0,0.0,2.5]},"dir":{"$vec2":[1.0,-1.0]},"c":{"$vec4":[1.0,2.0,3.0,4.0]},"r":{"$quat":[0.0,0.0,0.0,1.0]}})",
        R"({"a":{"$float":"nan"},"b":{"$float":"inf"},"c":{"$float":"-inf"},"d":-0.0,"e":0.1,"f":3.0})",
        R"({"blob":{"$bytes":"/w=="},"$bytes:/w==":1})",
        R"([null,false,{"deep":[[]]}])",
        R"({"big":9223372036854775807,"small":-9223372036854775808})",
    };
    for (const std::string& text : cases){
        SCOPED_TRACE(text);
        const nlohmann::json json = nlohmann::json::parse(text);
        std::string payload;
        savegame::SaveError error;
        ASSERT_TRUE(savegame::fromJson(json, payload, error)) << error.message;
        nlohmann::json back;
        ASSERT_TRUE(savegame::toJson(payload, back, error)) << error.message;
        // [[]] is the only non-canonical spelling here: an empty table is {}
        nlohmann::json expected = json;
        if (text == R"([null,false,{"deep":[[]]}])") expected = nlohmann::json::parse(R"([null,false,{"deep":[{}]}])");
        EXPECT_EQ(back.dump(), expected.dump());
        std::string again;
        ASSERT_TRUE(savegame::fromJson(back, again, error)) << error.message;
        EXPECT_EQ(again, payload) << "payload -> JSON -> payload";
    }

    std::string payload;
    savegame::SaveError error;
    EXPECT_FALSE(savegame::fromJson(nlohmann::json::parse(R"({"p":{"$vec5":[1]}})"), payload, error));
    EXPECT_EQ(error.code, savegame::SaveErrorCode::Encode);
    EXPECT_NE(error.message.find("$vec2, $vec3, $vec4, $quat, $float, $bytes"), std::string::npos) << error.message;
    EXPECT_FALSE(savegame::fromJson(nlohmann::json::parse(R"({"#x":1})"), payload, error));
    EXPECT_NE(error.message.find("bad integer key"), std::string::npos) << error.message;
    EXPECT_FALSE(savegame::fromJson(nlohmann::json::parse(R"({"v":{"$vec3":[1,2]}})"), payload, error));
    EXPECT_NE(error.message.find("[x, y, z]"), std::string::npos) << error.message;

    // a Lua table with a sparse integer key and a non-UTF-8 string shows as R21 says
    EXPECT_EQ(runSaveValueLua(R"(
        __savePayload = __saveEncode({[1] = "a", [5] = "b", s = "\255"})
    )"), "");
    lua_State* L = LuaBinding::getLuaState();
    lua_getglobal(L, "__savePayload");
    size_t n = 0;
    const char* bytes = lua_tolstring(L, -1, &n);
    const std::string luaPayload(bytes, n);
    lua_pop(L, 1);
    nlohmann::json view;
    ASSERT_TRUE(savegame::toJson(luaPayload, view, error)) << error.message;
    EXPECT_EQ(view.dump(), R"({"#1":"a","#5":"b","s":{"$bytes":"/w=="}})");
}
