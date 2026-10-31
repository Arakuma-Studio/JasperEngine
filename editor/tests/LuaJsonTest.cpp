// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Json Lua module and the LuaJson C++ API (feature script-data, spec 6.1 and 9.1-9.8): exact
// output, the array/object, number, null and UTF-8 rules, errors with their place, engine math
// values, __tojson, and encode(decode(s)) over a corpus.

#include "ScriptDataTestSupport.h"

#include "script/LuaJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

using namespace jasperengine;
using scriptdata::ScriptDataTest;

namespace {

class LuaJsonTest : public ScriptDataTest {};

// ---------------------------------------------------------------- 1. scalars, nesting, layout

TEST_F(LuaJsonTest, EncodesScalarsAndNestedTablesWithSortedKeys) {
    EXPECT_EQ(eval("return Json.encode(nil)"), "null");
    EXPECT_EQ(eval("return Json.encode(true)"), "true");
    EXPECT_EQ(eval("return Json.encode(false)"), "false");
    EXPECT_EQ(eval("return Json.encode(42)"), "42");
    EXPECT_EQ(eval("return Json.encode(-7)"), "-7");
    EXPECT_EQ(eval("return Json.encode('hi')"), "\"hi\"");
    // the Lua string: he said "hi", a newline, a tab, one backslash
    EXPECT_EQ(eval("return Json.encode('he said \"hi\"\\n\\t\\\\')"), "\"he said \\\"hi\\\"\\n\\t\\\\\"");
    EXPECT_EQ(eval("return Json.encode({b = 1, a = {true, false}, c = 'x', A = 0})"),
              "{\"A\":0,\"a\":[true,false],\"b\":1,\"c\":\"x\"}");
    EXPECT_EQ(eval("return Json.encode({player = {name = 'Ana', stats = {hp = 10, mp = 2}}, list = {1, {2, {3}}}})"),
              "{\"list\":[1,[2,[3]]],\"player\":{\"name\":\"Ana\",\"stats\":{\"hp\":10,\"mp\":2}}}");
    // a string with zeros keeps them
    EXPECT_EQ(eval("return Json.encode('a\\0b')"), "\"a\\u0000b\"");
    EXPECT_EQ(eval("return #Json.decode('\"a\\\\u0000b\"')"), "3");
}

TEST_F(LuaJsonTest, PrettyAndIndentOutput) {
    EXPECT_EQ(eval("return Json.encode({a = 1, b = {1, 2}, c = {}}, {pretty = true})"),
              "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"c\": []\n}");
    EXPECT_EQ(eval("return Json.encode({a = {x = true}}, {indent = 4})"),
              "{\n    \"a\": {\n        \"x\": true\n    }\n}");
    EXPECT_EQ(eval("return Json.encode({1, 2}, {indent = 0})"), "[\n1,\n2\n]");
    EXPECT_EQ(eval("return Json.encode({1, 2}, {pretty = false})"), "[1,2]");
}

// ---------------------------------------------------------------- 2. arrays and objects

TEST_F(LuaJsonTest, ArrayAndObjectRules) {
    EXPECT_EQ(eval("return Json.encode({})"), "[]");
    EXPECT_EQ(eval("return Json.encode({}, {emptyTable = 'object'})"), "{}");
    EXPECT_EQ(eval("return Json.encode({a = {}, b = {{}}}, {emptyTable = 'object'})"), "{\"a\":{},\"b\":[{}]}");
    EXPECT_EQ(eval("return Json.encode(Json.object())"), "{}");
    EXPECT_EQ(eval("return Json.encode(Json.array())"), "[]");
    EXPECT_EQ(eval("return Json.encode(Json.array({}), {emptyTable = 'object'})"), "[]");
    EXPECT_EQ(eval("return Json.encode(Json.object({}), {emptyTable = 'array'})"), "{}");
    // a table used as both array and object is an object
    EXPECT_EQ(eval("return Json.encode({1, 2, x = 3})"), "{\"1\":1,\"2\":2,\"x\":3}");
    // not exactly 1..n: an object with the integer keys as strings
    EXPECT_EQ(eval("return Json.encode({[1] = 'a', [3] = 'c'})"), "{\"1\":\"a\",\"3\":\"c\"}");
    EXPECT_EQ(eval("return Json.encode({[0] = 'z', 'a'})"), "{\"0\":\"z\",\"1\":\"a\"}");
    EXPECT_EQ(eval("return Json.encode({[-1] = true})"), "{\"-1\":true}");
    EXPECT_EQ(eval("return Json.encode({[2] = 'b'})"), "{\"2\":\"b\"}");
    // marked: always as marked
    EXPECT_EQ(eval("return Json.encode(Json.object({10, 20}))"), "{\"1\":10,\"2\":20}");
    EXPECT_EQ(eval("return Json.encode(Json.array({'a', 'b', name = 'ignored'}))"), "[\"a\",\"b\"]");
    // holes in a marked array, and keys no JSON object can have
    EXPECT_EQ(eval("return Json.tryEncode(Json.array({1, nil, 3}))"), "nil");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({list = Json.array({1, nil, 3})}) return e"), "hole at $.list[2]");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({a = {[1.5] = 1}}) return e"), "invalid key type float at $.a");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({[true] = 1}) return e"), "invalid key type boolean at $");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({[{}] = 1}) return e"), "invalid key type table at $");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({[1] = 'a', ['1'] = 'b', x = 1}) return e"),
              "duplicate key \"1\" (an integer and a string key) at $");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({items = {1, {name = 'x', bad = print}}}) return e"),
              "cannot encode a function at $.items[2].bad");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({['two words'] = {coroutine.create(print)}}) return e"),
              "cannot encode a thread at $[\"two words\"][1]");
}

TEST_F(LuaJsonTest, MarkersOnDecodeAndOnTablesWithMetatables) {
    // decoding marks empty containers, so encode(decode(s)) keeps [] and {}
    EXPECT_EQ(eval("return Json.encode(Json.decode('{\"a\":[],\"b\":{},\"c\":[{}]}'))"), "{\"a\":[],\"b\":{},\"c\":[{}]}");
    EXPECT_EQ(eval("local o = Json.decode('{}') o.score = 3 return Json.encode(o)"), "{\"score\":3}");
    EXPECT_EQ(eval("local a = Json.decode('[]') a[1] = 'x' return Json.encode(a)"), "[\"x\"]");
    // a metatable of the table's own is kept and gets __jsontype
    EXPECT_EQ(eval("local mt = {__index = {}} local t = setmetatable({}, mt) "
                   "local same = Json.object(t) == t return tostring(same and getmetatable(t) == mt and mt.__jsontype)"),
              "object");
    EXPECT_EQ(eval("local mt = {} local t = Json.array(setmetatable({}, mt)) return Json.encode(t, {emptyTable = 'object'})"), "[]");
    // re-marking switches the marker, never the shared marker metatables
    EXPECT_EQ(eval("local t = Json.array() Json.object(t) return Json.encode(t) .. Json.encode(Json.array())"), "{}[]");
    EXPECT_EQ(eval("return tostring(getmetatable(Json.array()).__jsontype)"), "array");
    EXPECT_NE(eval("return Json.array(5)").find("Json.array: a table expected, got number"), std::string::npos);
}

// ---------------------------------------------------------------- 3. numbers

TEST_F(LuaJsonTest, IntegersAndFloatsBothWays) {
    EXPECT_EQ(eval("return Json.encode(1)"), "1");
    EXPECT_EQ(eval("return Json.encode(1.0)"), "1.0");
    EXPECT_EQ(eval("return Json.encode({3.0, -0.5, 1e300, 2^53, 123456.5})"), "[3.0,-0.5,1e+300,9.007199254740992e+15,123456.5]");
    EXPECT_EQ(eval("return math.type(Json.decode('1'))"), "integer");
    EXPECT_EQ(eval("return math.type(Json.decode('1.0'))"), "float");
    EXPECT_EQ(eval("return math.type(Json.decode('1e2'))"), "float");
    EXPECT_EQ(eval("return Json.decode('1e2')"), "100.0");
    EXPECT_EQ(eval("return Json.encode(0.1)"), "0.1");
    EXPECT_EQ(eval("return tostring(Json.decode(Json.encode(0.1)) == 0.1)"), "true");
    EXPECT_EQ(eval("return tostring(Json.decode(Json.encode(1/3)) == 1/3)"), "true");
    EXPECT_EQ(eval("return Json.encode(math.maxinteger)"), "9223372036854775807");
    EXPECT_EQ(eval("return Json.encode(math.mininteger)"), "-9223372036854775808");
    EXPECT_EQ(eval("return tostring(Json.decode('9223372036854775807') == math.maxinteger)"), "true");
    EXPECT_EQ(eval("return tostring(Json.decode('-9223372036854775808') == math.mininteger)"), "true");
    EXPECT_EQ(eval("local v = Json.decode('9007199254740993') return math.type(v) .. ' ' .. tostring(v == 9007199254740993)"),
              "integer true");
    // beyond int64: a float
    EXPECT_EQ(eval("local v = Json.decode('18446744073709551615') return math.type(v) .. ' ' .. tostring(v == 2^64)"),
              "float true");
    EXPECT_EQ(eval("return math.type(Json.decode('100000000000000000000'))"), "float");
    EXPECT_EQ(eval("return math.type(Json.decode('-0'))"), "integer");
    // JSON can hold neither NaN nor infinities
    EXPECT_EQ(eval("local _, e = Json.tryEncode(0/0) return e"), "NaN or infinity at $");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({x = math.huge}) return e"), "NaN or infinity at $.x");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({-math.huge}) return e"), "NaN or infinity at $[1]");
    EXPECT_NE(eval("local _, e = Json.tryDecode('1e400') return e").find("number overflow"), std::string::npos);
}

// ---------------------------------------------------------------- 4. null

TEST_F(LuaJsonTest, NullSentinelBothWays) {
    EXPECT_EQ(eval("return tostring(Json.null)"), "Json.null");
    EXPECT_EQ(eval("return Json.encode(Json.null)"), "null");
    EXPECT_EQ(eval("return Json.encode({1, Json.null, 3})"), "[1,null,3]");
    EXPECT_EQ(eval("return Json.encode({a = Json.null})"), "{\"a\":null}");
    EXPECT_EQ(eval("local t = Json.decode('[1,null,3]') return #t .. tostring(t[2] == Json.null)"), "3true");
    EXPECT_EQ(eval("local t = Json.decode('{\"a\":null}') return tostring(t.a == Json.null and Json.isNull(t.a))"), "true");
    EXPECT_EQ(eval("return tostring(Json.isNull(nil)) .. tostring(Json.isNull(false)) .. tostring(Json.isNull({}))"),
              "falsefalsefalse");
    EXPECT_EQ(eval("return tostring(Json.decode('null') == Json.null)"), "true");
    // null = "nil": members omitted, array holes
    EXPECT_EQ(eval("local t = Json.decode('{\"a\":null,\"b\":1}', {null = 'nil'}) return tostring(t.a) .. t.b"), "nil1");
    EXPECT_EQ(eval("local t = Json.decode('[1,null,3]', {null = 'nil'}) return tostring(t[2]) .. t[3]"), "nil3");
    EXPECT_EQ(eval("return tostring(Json.decode('null', {null = 'nil'}))"), "nil");
    // an object with only nulls is still an object
    EXPECT_EQ(eval("return Json.encode(Json.decode('{\"a\":null}', {null = 'nil'}))"), "{}");
    // tryDecode: a valid null is Json.null, so nil always means failure
    std::vector<std::string> results;
    ASSERT_EQ(run("return Json.tryDecode('null')", &results), "");
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], "Json.null");
    EXPECT_EQ(results[1], "nil");
    // the sentinel cannot be changed from Lua
    EXPECT_EQ(eval("return getmetatable(Json.null)"), "Json.null");
}

// ---------------------------------------------------------------- 5. UTF-8

TEST_F(LuaJsonTest, InvalidUtf8AndUnicodeEscapes) {
    EXPECT_EQ(eval("local _, e = Json.tryEncode('ok\\255') return e"), "invalid UTF-8 at $");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({name = {'\\192\\128'}}) return e"), "invalid UTF-8 at $.name[1]");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({['\\237\\160\\128'] = 1}) return e"), "invalid UTF-8 in a key at $");
    EXPECT_EQ(eval("return Json.encode('a\\255b', {invalidUtf8 = 'replace'})"), "\"a\xEF\xBF\xBD" "b\"");
    EXPECT_EQ(eval("return Json.encode('a\\255b', {invalidUtf8 = 'ignore'})"), "\"ab\"");
    // a truncated sequence is one replacement (its maximal subpart)
    EXPECT_EQ(eval("return Json.encode('\\226\\130', {invalidUtf8 = 'replace'})"), "\"\xEF\xBF\xBD\"");
    EXPECT_EQ(eval("return Json.encode({['k\\255'] = 1}, {invalidUtf8 = 'ignore'})"), "{\"k\":1}");
    // valid UTF-8 passes as it is, escapes decode to UTF-8
    EXPECT_EQ(eval("return Json.encode('\xC3\xA9\xF0\x9F\x98\x80')"), "\"\xC3\xA9\xF0\x9F\x98\x80\"");
    EXPECT_EQ(eval("return Json.decode('\"\\\\u00e9\"')"), "\xC3\xA9");
    EXPECT_EQ(eval("return Json.decode('\"\\\\ud83d\\\\ude00\"')"), "\xF0\x9F\x98\x80");
    EXPECT_EQ(eval("return Json.decode('\"\xC3\xA9\"')"), "\xC3\xA9");
    EXPECT_NE(eval("local _, e = Json.tryDecode('\"\\\\ud83d\"') return e").find("syntax error at line 1"), std::string::npos);
    EXPECT_NE(eval("local _, e = Json.tryDecode('\"\\255\"') return e").find("syntax error at line 1"), std::string::npos);
}

// ---------------------------------------------------------------- 6. errors

TEST_F(LuaJsonTest, ErrorsNameTheirPlace) {
    EXPECT_EQ(eval("local _, e = Json.tryDecode('{\\n  \"a\": 1,\\n  }') return e"),
              "syntax error at line 3, column 3: unexpected '}'; expected string literal");
    EXPECT_EQ(eval("local ok, e = pcall(Json.decode, '[1, 2') return e"),
              "Json.decode: syntax error at line 1, column 6: unexpected end of input; expected ']'");
    EXPECT_NE(eval("local _, e = Json.tryDecode('') return e").find("syntax error at line 1, column 1"), std::string::npos);
    EXPECT_EQ(eval("local ok, e = pcall(Json.encode, {player = {onHit = print}}) return e"),
              "Json.encode: cannot encode a function at $.player.onHit");
    // nesting: 512 levels decode, 513 do not
    EXPECT_EQ(eval("return #Json.decode(string.rep('[', 512) .. string.rep(']', 512))"), "1");
    EXPECT_EQ(eval("local _, e = Json.tryDecode(string.rep('[', 513) .. string.rep(']', 513)) return e"),
              "nesting deeper than 512 at line 1, column 513");
    EXPECT_EQ(eval("local t = {} local c = t for i = 1, 511 do c[1] = {} c = c[1] end return (Json.encode(t):len())"),
              std::to_string(512 * 2));
    EXPECT_NE(eval("local t = {} local c = t for i = 1, 512 do c[1] = {} c = c[1] end local _, e = Json.tryEncode(t) return e")
                  .find("nesting deeper than 512 at $[1][1]"), std::string::npos);
    // cycles (a table reached twice without a cycle is written twice)
    EXPECT_EQ(eval("local t = {} t.self = t local _, e = Json.tryEncode(t) return e"), "cycle at $.self");
    EXPECT_EQ(eval("local a = {} a.b = {c = {a}} local _, e = Json.tryEncode(a) return e"), "cycle at $.b.c[1]");
    EXPECT_EQ(eval("local s = {1} return Json.encode({s, s, {s}})"), "[[1],[1],[[1]]]");
    // options are checked (typos too), also by the try variants
    EXPECT_NE(eval("return Json.encode({}, {prety = true})").find("Json.encode: unknown option 'prety' (options: pretty, indent, invalidUtf8, emptyTable)"),
              std::string::npos);
    EXPECT_NE(eval("return Json.tryEncode({}, {indent = -1})").find("option 'indent' must be an integer from 0 to 64"), std::string::npos);
    EXPECT_NE(eval("return Json.encode({}, {invalidUtf8 = 'drop'})").find("option 'invalidUtf8'"), std::string::npos);
    EXPECT_NE(eval("return Json.encode({}, {emptyTable = true})").find("option 'emptyTable'"), std::string::npos);
    EXPECT_NE(eval("return Json.encode({}, 'pretty')").find("options must be a table, got string"), std::string::npos);
    EXPECT_NE(eval("return Json.decode('1', {nul = 'nil'})").find("Json.decode: unknown option 'nul' (options: null, allowComments)"),
              std::string::npos);
    EXPECT_NE(eval("return Json.tryDecode('1', {null = 'none'})").find("option 'null' must be"), std::string::npos);
    EXPECT_NE(eval("return Json.decode(12)").find("Json.decode: text must be a string, got number"), std::string::npos);
    EXPECT_EQ(eval("local _, e = Json.tryDecode(nil) return e"), "text must be a string, got nil");
    // comments only when asked for
    EXPECT_EQ(eval("return Json.tryDecode('[1, /* two */ 2] // end')"), "nil");
    EXPECT_EQ(eval("return Json.encode(Json.decode('[1, /* [[[ */ 2] // {', {allowComments = true}))"), "[1,2]");
    // tryEncode never raises for a value
    std::vector<std::string> results;
    ASSERT_EQ(run("return Json.tryEncode(print)", &results), "");
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], "nil");
    EXPECT_EQ(results[1], "cannot encode a function at $");
}

// ---------------------------------------------------------------- 7. engine values, __tojson

TEST_F(LuaJsonTest, MathValuesAndToJson) {
    EXPECT_EQ(eval("return Json.encode(Vector3(1, 2.5, -3))"), "{\"x\":1.0,\"y\":2.5,\"z\":-3.0}");
    EXPECT_EQ(eval("return Json.encode(Vector2(0.1, 0))"), "{\"x\":0.1,\"y\":0.0}");
    EXPECT_EQ(eval("return Json.encode(Vector4(1, 2, 3, 4))"), "{\"w\":4.0,\"x\":1.0,\"y\":2.0,\"z\":3.0}");
    EXPECT_EQ(eval("return Json.encode(Quaternion())"), "{\"w\":1.0,\"x\":0.0,\"y\":0.0,\"z\":0.0}");
    EXPECT_EQ(eval("return Json.encode({at = Vector3(0, 1, 0)})"), "{\"at\":{\"x\":0.0,\"y\":1.0,\"z\":0.0}}");
    EXPECT_EQ(eval("local _, e = Json.tryEncode({f = File()}) return e"), "cannot encode a userdata at $.f");
    // __tojson: the value it returns is written
    EXPECT_EQ(eval("local mt = {__tojson = function(self) return {public = self.secret * 2} end} "
                   "return Json.encode({setmetatable({secret = 21}, mt)})"),
              "[{\"public\":42}]");
    // re-entrant: __tojson may encode on its own
    EXPECT_EQ(eval("local mt = {__tojson = function(self) return 'inner:' .. Json.encode({self.v}) end} "
                   "return Json.encode({a = setmetatable({v = 1}, mt)})"),
              "{\"a\":\"inner:[1]\"}");
    EXPECT_EQ(eval("local mt = {__tojson = function(self) return self end} local _, e = Json.tryEncode({x = setmetatable({}, mt)}) return e"),
              "__tojson returned the value itself at $.x");
    EXPECT_NE(eval("local mt = {__tojson = function() error('boom') end} local _, e = Json.tryEncode(setmetatable({}, mt)) return e")
                  .find("__tojson failed ("), std::string::npos);
    EXPECT_EQ(eval("local mt = {__tojson = 5} local _, e = Json.tryEncode(setmetatable({}, mt)) return e"), "__tojson is not a function at $");
    // a __tojson that keeps making new values ends at the depth limit
    EXPECT_NE(eval("local mt mt = {__tojson = function() return setmetatable({}, mt) end} "
                   "local _, e = Json.tryEncode(setmetatable({}, mt)) return e").find("nesting deeper than 512"),
              std::string::npos);
}

// ---------------------------------------------------------------- 8. round trips

TEST_F(LuaJsonTest, RoundTripOverACorpus) {
    // encode(decode(s)) is s re-serialized with sorted keys (nlohmann's dump of it)
    const std::vector<std::string> corpus = {
        "null", "true", "false", "0", "-0", "1", "-1", "123456789", "9223372036854775807", "-9223372036854775808",
        "0.5", "-2.25", "1e-7", "6.02214076e23", "3.0", "\"\"", "\"text\"", "\"\\u00e9\\ud83d\\ude00\\n\\t\\\"\\\\\"",
        "[]", "{}", "[[], {}, [[]], [{}]]", "[1, 2.5, \"three\", true, null]",
        "{\"b\": 1, \"a\": 2, \"c\": {\"z\": [], \"y\": {}}}",
        "{\"utc_timestamp\": 1730332800, \"timezone\": \"Europe/Madrid\", \"dst\": false}",
        "{\"players\": [{\"id\": 1, \"name\": \"Ana\", \"score\": 12.5}, {\"id\": 2, \"name\": \"Bo\", \"score\": null}]}",
        "{\"nested\": {\"deeper\": {\"deepest\": [1, [2, [3, [4]]]]}}}",
        "{\"1\": \"one\", \"2\": \"two\", \"10\": \"ten\"}",
        "{\"\": \"empty key\", \" \": \"space\", \"key with \\\"quotes\\\"\": 1}",
        "[1.7976931348623157e308, 2.2250738585072014e-308, 5e-324, 4294967296, 9007199254740993]",
        "{\"garden\": {\"plots\": [{\"plant\": \"rose\", \"stage\": 2, \"watered\": true}, {\"plant\": null}], \"coins\": 120}}",
    };
    ASSERT_EQ(corpus.size(), 30u);
    for (const std::string& text : corpus) {
        lua_pushlstring(L, text.data(), text.size());
        lua_setglobal(L, "corpusText");
        const std::string expected = nlohmann::json::parse(text).dump();
        EXPECT_EQ(eval("return Json.encode(Json.decode(corpusText))"), expected) << text;
        // and once more through the encoded text: stable
        EXPECT_EQ(eval("return Json.encode(Json.decode(Json.encode(Json.decode(corpusText))))"), expected) << text;
    }
    lua_pushnil(L);
    lua_setglobal(L, "corpusText");
}

// ---------------------------------------------------------------- the C++ API

TEST_F(LuaJsonTest, CppApiForOtherFeatures) {
    // toJson / push (http bodies, native bridge payloads)
    ASSERT_EQ(run("cppValue = {name = 'x', list = {1, 2}, empty = {}, none = Json.null}"), "");
    lua_getglobal(L, "cppValue");
    nlohmann::json out;
    std::string error;
    LuaJson::EncodeOptions options;
    options.emptyAsObject = true;
    ASSERT_TRUE(LuaJson::toJson(L, -1, out, options, error)) << error;
    EXPECT_EQ(out.dump(), "{\"empty\":{},\"list\":[1,2],\"name\":\"x\",\"none\":null}");
    EXPECT_EQ(lua_gettop(L), 1) << "the stack is left as it was";
    lua_pop(L, 1);

    // invalid UTF-8 replaced at conversion: the json dumps without an error handler
    lua_pushstring(L, "bad\xff");
    options.utf8 = LuaJson::InvalidUtf8::Replace;
    ASSERT_TRUE(LuaJson::toJson(L, -1, out, options, error));
    EXPECT_EQ(out.dump(), "\"bad\xEF\xBF\xBD\"");
    lua_pop(L, 1);

    lua_pushcfunction(L, [](lua_State*) { return 0; });
    EXPECT_FALSE(LuaJson::toJson(L, -1, out, LuaJson::EncodeOptions(), error));
    EXPECT_EQ(error, "cannot encode a function at $");
    lua_pop(L, 1);

    const nlohmann::json value = nlohmann::json::parse("{\"a\": [1, null, 2.5], \"b\": {}, \"c\": 18446744073709551615}");
    LuaJson::push(L, value);
    lua_setglobal(L, "pushed");
    EXPECT_EQ(eval("return tostring(pushed.a[2] == Json.null) .. math.type(pushed.a[1]) .. math.type(pushed.c)"), "trueintegerfloat");
    EXPECT_EQ(eval("return Json.encode(pushed)"), "{\"a\":[1,null,2.5],\"b\":{},\"c\":1.8446744073709552e+19}");
    LuaJson::DecodeOptions nilNulls;
    nilNulls.nullAsNil = true;
    LuaJson::push(L, value, nilNulls);
    lua_setglobal(L, "pushed");
    EXPECT_EQ(eval("return tostring(pushed.a[2]) .. pushed.a[3]"), "nil2.5");

    // encode / decode / parse
    std::string text;
    ASSERT_TRUE(LuaJson::decode(L, "{\"k\": [true]}", LuaJson::DecodeOptions(), error)) << error;
    ASSERT_TRUE(LuaJson::encode(L, -1, text, LuaJson::EncodeOptions(), error)) << error;
    EXPECT_EQ(text, "{\"k\":[true]}");
    lua_pop(L, 1);
    const int top = lua_gettop(L);
    EXPECT_FALSE(LuaJson::decode(L, "{", LuaJson::DecodeOptions(), error));
    EXPECT_EQ(lua_gettop(L), top) << "nothing pushed on failure";
    EXPECT_EQ(error, "syntax error at line 1, column 2: unexpected end of input; expected string literal");
    nlohmann::json parsed;
    EXPECT_FALSE(LuaJson::parse(std::string(600, '['), parsed, LuaJson::DecodeOptions(), error));
    EXPECT_EQ(error, "nesting deeper than 512 at line 1, column 513");

    // the sentinel and the markers
    LuaJson::pushNull(L);
    EXPECT_TRUE(LuaJson::isNull(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, 0);
    EXPECT_FALSE(LuaJson::isNull(L, -1));
    lua_pop(L, 1);
    lua_newtable(L);
    LuaJson::markObject(L, -1);
    ASSERT_TRUE(LuaJson::encode(L, -1, text, LuaJson::EncodeOptions(), error));
    EXPECT_EQ(text, "{}");
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setglobal(L, "pushed");
    lua_pushnil(L);
    lua_setglobal(L, "cppValue");
}

TEST_F(LuaJsonTest, DecodesAMegabyteQuickly) {
    // spec 8: 1 MB decodes well under 30 ms on desktop (generous bound: a loaded CI machine)
    ASSERT_EQ(run("local parts = {} for i = 1, 16000 do parts[i] = string.format('{\"id\":%d,\"name\":\"item %d\",\"tags\":[\"a\",\"b\"],\"v\":%d.5}', i, i, i) end "
                  "bigText = '[' .. table.concat(parts, ',') .. ']'"), "");
    EXPECT_GT(std::stoi(eval("return #bigText")), 900000);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(eval("return #Json.decode(bigText)"), "16000");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(ms, 300.0);
    EXPECT_EQ(eval("return tostring(Json.encode(Json.decode(bigText)) == Json.encode(Json.decode(Json.encode(Json.decode(bigText)))))"), "true");
    lua_pushnil(L);
    lua_setglobal(L, "bigText");
}

}
