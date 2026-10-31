// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaJson.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Log.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "util/NumberText.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace jasperengine;

namespace {

    const char* const NULL_KEY = "jasperengine.json.null";
    const char* const ARRAY_META_KEY = "jasperengine.json.arrayMeta";
    const char* const OBJECT_META_KEY = "jasperengine.json.objectMeta";

    // ---------------------------------------------------------------- registry entries

    int nullToString(lua_State* L){
        lua_pushliteral(L, "Json.null");
        return 1;
    }

    // registry[ARRAY_META_KEY] / [OBJECT_META_KEY]: {__jsontype = kind, __name = "Json.<kind>"}
    void pushMarkerMeta(lua_State* L, const char* key, const char* kind){
        if (lua_getfield(L, LUA_REGISTRYINDEX, key) == LUA_TTABLE)
            return;
        lua_pop(L, 1);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, kind);
        lua_setfield(L, -2, "__jsontype");
        lua_pushstring(L, (std::string("Json.") + kind).c_str());
        lua_setfield(L, -2, "__name");
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }

    bool isMarkerMeta(lua_State* L, int index){
        index = lua_absindex(L, index);
        bool marker = false;
        for (const char* key : {ARRAY_META_KEY, OBJECT_META_KEY}){
            lua_getfield(L, LUA_REGISTRYINDEX, key);
            marker = marker || lua_rawequal(L, -1, index);
            lua_pop(L, 1);
        }
        return marker;
    }

    void mark(lua_State* L, int index, const char* key, const char* kind){
        index = lua_absindex(L, index);
        if (lua_getmetatable(L, index)){
            if (!isMarkerMeta(L, -1)){
                // a table with a metatable of its own keeps it (dkjson's rule)
                lua_pushliteral(L, "__jsontype");
                lua_pushstring(L, kind);
                lua_rawset(L, -3);
                lua_pop(L, 1);
                return;
            }
            lua_pop(L, 1);
        }
        pushMarkerMeta(L, key, kind);
        lua_setmetatable(L, index);
    }

    // rawget(t, name) pushed, its type returned
    int rawField(lua_State* L, int index, const char* name){
        index = lua_absindex(L, index);
        lua_pushstring(L, name);
        return lua_rawget(L, index);
    }

    // ---------------------------------------------------------------- UTF-8

    // Length of the well-formed UTF-8 sequence at s (Unicode table 3-7), or 0 with `bad` set to
    // the length of its maximal ill-formed subpart (at least 1)
    size_t utf8Sequence(const unsigned char* s, size_t n, size_t& bad){
        const unsigned char c = s[0];
        if (c < 0x80) return 1;
        size_t length = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) length = 2;
        else if (c == 0xE0) { length = 3; low = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) length = 3;
        else if (c == 0xED) { length = 3; high = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) length = 3;
        else if (c == 0xF0) { length = 4; low = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) length = 4;
        else if (c == 0xF4) { length = 4; high = 0x8F; }
        else { bad = 1; return 0; }
        size_t i = 1;
        for (; i < length; i++){
            if (i >= n) break;
            const unsigned char k = s[i];
            if (i == 1 ? (k < low || k > high) : (k < 0x80 || k > 0xBF)) break;
        }
        if (i == length) return length;
        bad = i;
        return 0;
    }

    bool validUtf8(const char* text, size_t size){
        const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
        size_t i = 0;
        while (i < size){
            size_t bad = 0;
            const size_t length = utf8Sequence(s + i, size - i, bad);
            if (!length) return false;
            i += length;
        }
        return true;
    }

    std::string fixedUtf8(const char* text, size_t size, bool replace){
        const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
        std::string out;
        out.reserve(size);
        size_t i = 0;
        while (i < size){
            size_t bad = 0;
            const size_t length = utf8Sequence(s + i, size - i, bad);
            if (length){
                out.append(text + i, length);
                i += length;
            } else {
                if (replace) out += "\xEF\xBF\xBD";
                i += bad;
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- encoding

    bool isIdentifier(const char* s, size_t n){
        if (n == 0) return false;
        for (size_t i = 0; i < n; i++){
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
        }
        return true;
    }

    template<class T>
    T* userdataOf(lua_State* L, int index){
        if (!luabridge::isInstance<T>(L, index))
            return nullptr;
        auto result = luabridge::Stack<T*>::get(L, index);
        return result ? *result : nullptr;
    }

    struct Encoder {
        lua_State* L;
        const LuaJson::EncodeOptions& opts;
        std::string& error;
        std::string path = "$";
        std::vector<const void*> open;  // the tables being written (a cycle comes back to one)

        bool fail(const std::string& what){
            error = what + " at " + path;
            return false;
        }

        bool string(const char* s, size_t n, std::string& out, const char* where){
            if (validUtf8(s, n)){
                out.assign(s, n);
                return true;
            }
            if (opts.utf8 == LuaJson::InvalidUtf8::Error)
                return fail(std::string("invalid UTF-8") + where);
            out = fixedUtf8(s, n, opts.utf8 == LuaJson::InvalidUtf8::Replace);
            return true;
        }

        bool value(int index, nlohmann::json& out, int depth){
            index = lua_absindex(L, index);
            switch (lua_type(L, index)){
                case LUA_TNIL:
                    out = nullptr;
                    return true;
                case LUA_TBOOLEAN:
                    out = lua_toboolean(L, index) != 0;
                    return true;
                case LUA_TNUMBER:
                    if (lua_isinteger(L, index)){
                        out = static_cast<std::int64_t>(lua_tointeger(L, index));
                    } else {
                        const double d = static_cast<double>(lua_tonumber(L, index));
                        if (!std::isfinite(d)) return fail("NaN or infinity");
                        out = d;
                    }
                    return true;
                case LUA_TSTRING: {
                    size_t n = 0;
                    const char* s = lua_tolstring(L, index, &n);
                    std::string text;
                    if (!string(s, n, text, "")) return false;
                    out = std::move(text);
                    return true;
                }
                case LUA_TTABLE:
                    return table(index, out, depth + 1);
                case LUA_TUSERDATA:
                case LUA_TLIGHTUSERDATA:
                    return userdata(index, out, depth + 1);
                default:
                    return fail(std::string("cannot encode a ") + luaL_typename(L, index));
            }
        }

        // __tojson(self), the function on top of the stack: the value it returns is written
        // instead (the function is popped)
        bool toJsonMethod(int index, nlohmann::json& out, int depth){
            const int top = lua_gettop(L) - 1;
            lua_pushvalue(L, index);
            if (lua_pcall(L, 1, 1, 0) != LUA_OK){
                const char* message = lua_tostring(L, -1);
                const std::string text = message ? message : "(error object is not a string)";
                lua_settop(L, top);
                return fail("__tojson failed (" + text + ")");
            }
            if (lua_rawequal(L, -1, index)){
                lua_settop(L, top);
                return fail("__tojson returned the value itself");
            }
            const void* self = lua_topointer(L, index);
            open.push_back(self);
            const bool ok = value(-1, out, depth);
            open.pop_back();
            lua_settop(L, top);
            return ok;
        }

        bool userdata(int index, nlohmann::json& out, int depth){
            if (LuaJson::isNull(L, index)){
                out = nullptr;
                return true;
            }
            if (depth > LuaJson::MaxDepth) return fail("nesting deeper than " + std::to_string(LuaJson::MaxDepth));
            if (!lua_checkstack(L, 4)) return fail("nesting too deep for the Lua stack");
            if (luaL_getmetafield(L, index, "__tojson") != LUA_TNIL){
                if (!lua_isfunction(L, -1)){
                    lua_pop(L, 1);
                    return fail("__tojson is not a function");
                }
                return toJsonMethod(index, out, depth);
            }
            if (const Vector2* v = userdataOf<Vector2>(L, index)){
                out = {{"x", NumberText::floatToDouble(v->x)}, {"y", NumberText::floatToDouble(v->y)}};
            } else if (const Vector3* v = userdataOf<Vector3>(L, index)){
                out = {{"x", NumberText::floatToDouble(v->x)}, {"y", NumberText::floatToDouble(v->y)},
                       {"z", NumberText::floatToDouble(v->z)}};
            } else if (const Vector4* v = userdataOf<Vector4>(L, index)){
                out = {{"x", NumberText::floatToDouble(v->x)}, {"y", NumberText::floatToDouble(v->y)},
                       {"z", NumberText::floatToDouble(v->z)}, {"w", NumberText::floatToDouble(v->w)}};
            } else if (const Quaternion* q = userdataOf<Quaternion>(L, index)){
                out = {{"x", NumberText::floatToDouble(q->x)}, {"y", NumberText::floatToDouble(q->y)},
                       {"z", NumberText::floatToDouble(q->z)}, {"w", NumberText::floatToDouble(q->w)}};
            } else {
                return fail(std::string("cannot encode a ") + luaL_typename(L, index));
            }
            for (auto& item : out.items()){
                if (!std::isfinite(item.value().get<double>())){
                    const size_t length = path.size();
                    path += "." + item.key();
                    fail("NaN or infinity");
                    path.resize(length);
                    return false;
                }
            }
            return true;
        }

        bool table(int index, nlohmann::json& out, int depth){
            if (depth > LuaJson::MaxDepth) return fail("nesting deeper than " + std::to_string(LuaJson::MaxDepth));
            if (!lua_checkstack(L, 6)) return fail("nesting too deep for the Lua stack");
            const void* self = lua_topointer(L, index);
            for (const void* p : open){
                if (p == self) return fail("cycle");
            }

            std::string kind;
            if (lua_getmetatable(L, index)){
                const int meta = lua_gettop(L);
                if (rawField(L, meta, "__tojson") != LUA_TNIL){
                    if (!lua_isfunction(L, -1)){
                        lua_settop(L, meta - 1);
                        return fail("__tojson is not a function");
                    }
                    lua_remove(L, meta);
                    return toJsonMethod(index, out, depth);
                }
                lua_pop(L, 1);
                if (rawField(L, meta, "__jsontype") == LUA_TSTRING)
                    kind = lua_tostring(L, -1);
                lua_settop(L, meta - 1);
            }

            open.push_back(self);
            bool ok;
            if (kind == "array"){
                ok = array(index, static_cast<lua_Integer>(lua_rawlen(L, index)), out, depth);
            } else if (kind == "object"){
                ok = object(index, out, depth);
            } else {
                // keys exactly 1..n: an array
                lua_Integer count = 0, max = 0;
                bool sequence = true;
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    lua_pop(L, 1);
                    count++;
                    if (sequence && lua_isinteger(L, -1)){
                        const lua_Integer k = lua_tointeger(L, -1);
                        if (k < 1) sequence = false;
                        else if (k > max) max = k;
                    } else {
                        sequence = false;
                    }
                }
                if (count == 0){
                    out = opts.emptyAsObject ? nlohmann::json::object() : nlohmann::json::array();
                    ok = true;
                } else if (sequence && max == count){
                    ok = array(index, count, out, depth);
                } else {
                    ok = object(index, out, depth);
                }
            }
            open.pop_back();
            return ok;
        }

        bool array(int index, lua_Integer n, nlohmann::json& out, int depth){
            out = nlohmann::json::array();
            const size_t length = path.size();
            for (lua_Integer i = 1; i <= n; i++){
                path += "[" + std::to_string(i) + "]";
                lua_rawgeti(L, index, i);
                if (lua_isnil(L, -1)){
                    lua_pop(L, 1);
                    return fail("hole");
                }
                nlohmann::json item;
                const bool ok = value(-1, item, depth);
                lua_pop(L, 1);
                if (!ok) return false;
                out.push_back(std::move(item));
                path.resize(length);
            }
            return true;
        }

        bool object(int index, nlohmann::json& out, int depth){
            out = nlohmann::json::object();
            const int top = lua_gettop(L);
            const size_t length = path.size();
            lua_pushnil(L);
            while (lua_next(L, index) != 0){
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING){
                    size_t n = 0;
                    const char* s = lua_tolstring(L, -2, &n);
                    if (!string(s, n, key, " in a key")){
                        lua_settop(L, top);
                        return false;
                    }
                    if (isIdentifier(key.data(), key.size())) path += "." + key;
                    else path += "[" + nlohmann::json(key).dump() + "]";
                } else if (lua_isinteger(L, -2)){
                    key = std::to_string(lua_tointeger(L, -2));
                    path += "[" + key + "]";
                } else {
                    const std::string type = lua_type(L, -2) == LUA_TNUMBER ? "float" : luaL_typename(L, -2);
                    lua_settop(L, top);
                    return fail("invalid key type " + type);
                }
                if (out.contains(key)){
                    lua_settop(L, top);
                    path.resize(length);
                    return fail("duplicate key " + nlohmann::json(key).dump() + " (an integer and a string key)");
                }
                nlohmann::json item;
                if (!value(-1, item, depth)){
                    lua_settop(L, top);
                    return false;
                }
                out[key] = std::move(item);
                path.resize(length);
                lua_pop(L, 1);
            }
            return true;
        }
    };

    // ---------------------------------------------------------------- decoding

    void pushValue(lua_State* L, const nlohmann::json& value, const LuaJson::DecodeOptions& opts, int depth){
        luaL_checkstack(L, 4, "Json.decode: nesting too deep");
        if (depth > LuaJson::MaxDepth){
            static bool warned = false;
            if (!warned){
                warned = true;
                Log::warn("LuaJson::push: a value nested deeper than %d became Json.null", LuaJson::MaxDepth);
            }
            LuaJson::pushNull(L);
            return;
        }
        switch (value.type()){
            case nlohmann::json::value_t::null:
                if (opts.nullAsNil) lua_pushnil(L);
                else LuaJson::pushNull(L);
                return;
            case nlohmann::json::value_t::boolean:
                lua_pushboolean(L, value.get<bool>() ? 1 : 0);
                return;
            case nlohmann::json::value_t::number_integer:
                lua_pushinteger(L, static_cast<lua_Integer>(value.get<std::int64_t>()));
                return;
            case nlohmann::json::value_t::number_unsigned: {
                const std::uint64_t u = value.get<std::uint64_t>();
                if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    lua_pushinteger(L, static_cast<lua_Integer>(u));
                else
                    lua_pushnumber(L, static_cast<lua_Number>(u));
                return;
            }
            case nlohmann::json::value_t::number_float:
                lua_pushnumber(L, static_cast<lua_Number>(value.get<double>()));
                return;
            case nlohmann::json::value_t::string: {
                const std::string& s = value.get_ref<const std::string&>();
                lua_pushlstring(L, s.data(), s.size());
                return;
            }
            case nlohmann::json::value_t::array: {
                lua_createtable(L, static_cast<int>(std::min<size_t>(value.size(), 1u << 24)), 0);
                lua_Integer i = 0;
                bool any = false;
                for (const nlohmann::json& item : value){
                    i++;
                    pushValue(L, item, opts, depth + 1);
                    if (lua_isnil(L, -1)){
                        lua_pop(L, 1);
                        continue;
                    }
                    lua_rawseti(L, -2, i);
                    any = true;
                }
                if (!any) LuaJson::markArray(L, -1);
                return;
            }
            case nlohmann::json::value_t::object: {
                lua_createtable(L, 0, static_cast<int>(std::min<size_t>(value.size(), 1u << 24)));
                bool any = false;
                for (auto it = value.begin(); it != value.end(); ++it){
                    pushValue(L, it.value(), opts, depth + 1);
                    if (lua_isnil(L, -1)){
                        lua_pop(L, 1);
                        continue;
                    }
                    const std::string& key = it.key();
                    lua_pushlstring(L, key.data(), key.size());
                    lua_insert(L, -2);
                    lua_rawset(L, -3);
                    any = true;
                }
                if (!any) LuaJson::markObject(L, -1);
                return;
            }
            default:
                // binary values (only a json built in C++ has them) and discarded ones
                if (opts.nullAsNil) lua_pushnil(L);
                else LuaJson::pushNull(L);
                return;
        }
    }

    // Containers nested deeper than MaxDepth, before nlohmann builds them: false with the
    // place of the first one too deep
    bool checkDepth(std::string_view text, bool comments, std::string& error){
        int depth = 0;
        size_t line = 1, column = 0;
        bool inString = false, escape = false;
        for (size_t i = 0; i < text.size(); i++){
            const char c = text[i];
            if (c == '\n'){ line++; column = 0; }
            else column++;
            if (inString){
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (comments && c == '/' && i + 1 < text.size()){
                if (text[i + 1] == '/'){
                    while (i + 1 < text.size() && text[i + 1] != '\n') i++;
                    continue;
                }
                if (text[i + 1] == '*'){
                    i += 2; column += 2;
                    while (i < text.size() && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/')){
                        if (text[i] == '\n'){ line++; column = 0; } else column++;
                        i++;
                    }
                    i++; column++;
                    continue;
                }
            }
            if (c == '"') inString = true;
            else if (c == '[' || c == '{'){
                if (++depth > LuaJson::MaxDepth){
                    error = "nesting deeper than " + std::to_string(LuaJson::MaxDepth) + " at line " + std::to_string(line) +
                            ", column " + std::to_string(column);
                    return false;
                }
            } else if (c == ']' || c == '}'){
                depth--;
            }
        }
        return true;
    }

    // "[json.exception.parse_error.101] parse error at line 3, column 14: syntax error while
    // parsing object key - unexpected '}'; expected string literal" ->
    // "syntax error at line 3, column 14: unexpected '}'; expected string literal"
    std::string parseErrorText(const std::string& what){
        std::string text = what;
        const size_t bracket = text.find("] ");
        if (text.rfind("[json.exception.", 0) == 0 && bracket != std::string::npos)
            text = text.substr(bracket + 2);
        if (text.rfind("parse error ", 0) == 0)
            text = "syntax error " + text.substr(12);
        const size_t context = text.find("syntax error while parsing ");
        if (context != std::string::npos){
            const size_t dash = text.find(" - ", context);
            if (dash != std::string::npos)
                text.erase(context, dash + 3 - context);
        }
        return text;
    }

}

bool LuaJson::toJson(lua_State* L, int index, nlohmann::json& out, const EncodeOptions& opts, std::string& error){
    const int top = lua_gettop(L);
    Encoder encoder{L, opts, error};
    const bool ok = encoder.value(index, out, 0);
    lua_settop(L, top);
    return ok;
}

void LuaJson::push(lua_State* L, const nlohmann::json& value, const DecodeOptions& opts){
    pushValue(L, value, opts, 0);
}

bool LuaJson::encode(lua_State* L, int index, std::string& out, const EncodeOptions& opts, std::string& error){
    nlohmann::json value;
    if (!toJson(L, index, value, opts, error))
        return false;
    try {
        out = value.dump(opts.indent < 0 ? -1 : opts.indent, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception& e){
        error = e.what();
        return false;
    }
    return true;
}

bool LuaJson::parse(std::string_view text, nlohmann::json& out, const DecodeOptions& opts, std::string& error){
    if (!checkDepth(text, opts.allowComments, error))
        return false;
    try {
        out = nlohmann::json::parse(text.begin(), text.end(), nullptr, true, opts.allowComments);
    } catch (const nlohmann::json::exception& e){
        error = parseErrorText(e.what());
        return false;
    }
    return true;
}

bool LuaJson::decode(lua_State* L, std::string_view text, const DecodeOptions& opts, std::string& error){
    nlohmann::json value;
    if (!parse(text, value, opts, error))
        return false;
    push(L, value, opts);
    return true;
}

void LuaJson::pushNull(lua_State* L){
    if (lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY) == LUA_TUSERDATA)
        return;
    lua_pop(L, 1);
    lua_newuserdatauv(L, 1, 0);
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, nullToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "JsonNull");
    lua_setfield(L, -2, "__name");
    // getmetatable(Json.null) is not this table, so nothing can change it
    lua_pushliteral(L, "Json.null");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, NULL_KEY);
}

bool LuaJson::isNull(lua_State* L, int index){
    if (lua_type(L, index) != LUA_TUSERDATA)
        return false;
    index = lua_absindex(L, index);
    if (lua_getfield(L, LUA_REGISTRYINDEX, NULL_KEY) != LUA_TUSERDATA){
        lua_pop(L, 1);
        return false;
    }
    const bool same = lua_rawequal(L, -1, index) != 0;
    lua_pop(L, 1);
    return same;
}

void LuaJson::markArray(lua_State* L, int index){
    mark(L, index, ARRAY_META_KEY, "array");
}

void LuaJson::markObject(lua_State* L, int index){
    mark(L, index, OBJECT_META_KEY, "object");
}
