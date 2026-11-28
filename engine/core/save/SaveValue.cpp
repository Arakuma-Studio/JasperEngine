// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "save/SaveValue.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "script/LuaJson.h"
#include "util/Base64.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

using namespace jasperengine;

namespace jasperengine::savegame {

namespace {

    enum Tag : uint8_t {
        TagFalse = 0x00, TagTrue = 0x01, TagInteger = 0x02, TagFloat = 0x03, TagString = 0x04, TagTable = 0x05,
        TagVector2 = 0x06, TagVector3 = 0x07, TagVector4 = 0x08, TagQuaternion = 0x09, TagNull = 0x0A
    };

    constexpr uint64_t CANONICAL_NAN = 0x7FF8000000000000ull;

    // ---------------------------------------------------------------- writing

    void putVarint(std::string& out, uint64_t v){
        while (v >= 0x80){
            out.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        out.push_back(static_cast<char>(v));
    }

    uint64_t zigzag(int64_t v){
        return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
    }

    int64_t unzigzag(uint64_t v){
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }

    void saveValuePutLE(std::string& out, uint64_t v, int bytes){
        for (int i = 0; i < bytes; i++) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }

    void putFloat(std::string& out, double d){
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        out.push_back(static_cast<char>(TagFloat));
        saveValuePutLE(out, bits, 8);
    }

    void putFloat32(std::string& out, float f){
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        saveValuePutLE(out, bits, 4);
    }

    void putInteger(std::string& out, int64_t v){
        out.push_back(static_cast<char>(TagInteger));
        putVarint(out, zigzag(v));
    }

    void putString(std::string& out, std::string_view s){
        out.push_back(static_cast<char>(TagString));
        putVarint(out, s.size());
        out.append(s.data(), s.size());
    }

    bool isIdentifier(std::string_view s){
        if (s.empty()) return false;
        for (size_t i = 0; i < s.size(); i++){
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
        }
        return true;
    }

    std::string stringKeyPath(std::string_view key){
        if (isIdentifier(key)) return "." + std::string(key);
        std::string out = "[\"";
        for (char c : key){
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out + "\"]";
    }

    bool validUtf8(std::string_view text){
        const unsigned char* s = reinterpret_cast<const unsigned char*>(text.data());
        const size_t n = text.size();
        size_t i = 0;
        while (i < n){
            const unsigned char c = s[i];
            if (c < 0x80){ i++; continue; }
            size_t length = 0;
            unsigned char low = 0x80, high = 0xBF;
            if (c >= 0xC2 && c <= 0xDF) length = 2;
            else if (c == 0xE0){ length = 3; low = 0xA0; }
            else if (c >= 0xE1 && c <= 0xEC) length = 3;
            else if (c == 0xED){ length = 3; high = 0x9F; }
            else if (c >= 0xEE && c <= 0xEF) length = 3;
            else if (c == 0xF0){ length = 4; low = 0x90; }
            else if (c >= 0xF1 && c <= 0xF3) length = 4;
            else if (c == 0xF4){ length = 4; high = 0x8F; }
            else return false;
            if (i + length > n) return false;
            for (size_t k = 1; k < length; k++){
                const unsigned char b = s[i + k];
                if (k == 1 ? (b < low || b > high) : (b < 0x80 || b > 0xBF)) return false;
            }
            i += length;
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

    // ---------------------------------------------------------------- Lua -> bytes

    struct LuaEncoder {
        lua_State* L;
        std::string& out;
        SaveError& error;
        // the keys down to the value being written, spelled out only for an error (a string per
        // entry would cost more than the encode)
        struct Step {
            bool isInteger;
            lua_Integer integer;
            std::string_view text;
        };
        std::vector<Step> steps;
        std::vector<const void*> open;

        bool fail(const std::string& what){
            std::string path = "$";
            for (const Step& step : steps)
                path += step.isInteger ? "[" + std::to_string(step.integer) + "]" : stringKeyPath(step.text);
            error = makeError(SaveErrorCode::Encode, what + " at " + path, path);
            return false;
        }

        bool value(int index, int depth){
            switch (lua_type(L, index)){
                case LUA_TBOOLEAN:
                    out.push_back(static_cast<char>(lua_toboolean(L, index) ? TagTrue : TagFalse));
                    return true;
                case LUA_TNUMBER:
                    if (lua_isinteger(L, index)) putInteger(out, static_cast<int64_t>(lua_tointeger(L, index)));
                    else putFloat(out, static_cast<double>(lua_tonumber(L, index)));
                    return true;
                case LUA_TSTRING: {
                    size_t n = 0;
                    const char* s = lua_tolstring(L, index, &n);
                    putString(out, std::string_view(s, n));
                    return true;
                }
                case LUA_TTABLE:
                    return table(index, depth + 1);
                case LUA_TUSERDATA:
                    return userdata(index);
                case LUA_TNIL:
                case LUA_TNONE:
                    return fail("cannot save nil");
                default:
                    return fail(std::string("cannot save a ") + luaL_typename(L, index));
            }
        }

        bool userdata(int index){
            if (LuaJson::isNull(L, index)){
                out.push_back(static_cast<char>(TagNull));
                return true;
            }
            if (const Vector2* v = userdataOf<Vector2>(L, index)){
                out.push_back(static_cast<char>(TagVector2));
                putFloat32(out, v->x); putFloat32(out, v->y);
            } else if (const Vector3* v = userdataOf<Vector3>(L, index)){
                out.push_back(static_cast<char>(TagVector3));
                putFloat32(out, v->x); putFloat32(out, v->y); putFloat32(out, v->z);
            } else if (const Vector4* v = userdataOf<Vector4>(L, index)){
                out.push_back(static_cast<char>(TagVector4));
                putFloat32(out, v->x); putFloat32(out, v->y); putFloat32(out, v->z); putFloat32(out, v->w);
            } else if (const Quaternion* q = userdataOf<Quaternion>(L, index)){
                out.push_back(static_cast<char>(TagQuaternion));
                putFloat32(out, q->x); putFloat32(out, q->y); putFloat32(out, q->z); putFloat32(out, q->w);
            } else {
                return fail("cannot save a userdata");
            }
            return true;
        }

        struct Key {
            bool isInteger;
            lua_Integer integer;
            std::string_view text;
            // the text's first 8 bytes, big-endian and zero-padded: most comparisons end here
            uint64_t prefix;
            // the stack slot of its value, 0 when the table was too big to keep it there
            int slot;
        };
        // one list per depth, reused by every table of that depth (no allocation per table)
        std::vector<std::vector<Key>> keyLists;

        static uint64_t prefixOf(std::string_view text){
            uint64_t prefix = 0;
            for (size_t i = 0; i < 8; i++)
                prefix = prefix << 8 | (i < text.size() ? static_cast<unsigned char>(text[i]) : 0u);
            return prefix;
        }

        bool table(int index, int depth){
            index = lua_absindex(L, index);
            if (depth > MaxDepth) return fail("nesting deeper than " + std::to_string(MaxDepth));
            if (!lua_checkstack(L, 4)) return fail("nesting too deep for the Lua stack");
            const void* self = lua_topointer(L, index);
            if (std::find(open.begin(), open.end(), self) != open.end()) return fail("cycle");

            lua_Integer n = 0;
            while (lua_rawgeti(L, index, n + 1) != LUA_TNIL){
                lua_pop(L, 1);
                n++;
            }
            lua_pop(L, 1);

            if (keyLists.size() <= static_cast<size_t>(depth)) keyLists.resize(static_cast<size_t>(depth) + 1);
            // each pair stays on the stack (the key pushed again for lua_next), so the sorted pass
            // reads its value without pushing and looking up the key a second time
            const int base = lua_gettop(L);
            size_t pairs = 0;
            // scoped: the first nested table (the next depth) may grow keyLists and leave it dangling
            {
                std::vector<Key>& keys = keyLists[static_cast<size_t>(depth)];
                keys.clear();
                bool onStack = true;
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    const int type = lua_type(L, -2);
                    Key key{false, 0, {}, 0, 0};
                    if (type == LUA_TNUMBER && lua_isinteger(L, -2)){
                        const lua_Integer k = lua_tointeger(L, -2);
                        if (k >= 1 && k <= n){
                            lua_pop(L, 1);
                            continue;
                        }
                        key.isInteger = true;
                        key.integer = k;
                    } else if (type == LUA_TSTRING){
                        size_t length = 0;
                        // the key stays in the table, so its text outlives this loop
                        const char* s = lua_tolstring(L, -2, &length);
                        key.text = std::string_view(s, length);
                        key.prefix = prefixOf(key.text);
                    } else {
                        const std::string what = type == LUA_TNUMBER ? "float" : luaL_typename(L, -2);
                        lua_settop(L, base);
                        return fail("cannot save a " + what + " key");
                    }
                    if (onStack && !lua_checkstack(L, 3)) onStack = false;
                    if (onStack){
                        key.slot = lua_gettop(L);
                        lua_pushvalue(L, -2);
                    } else {
                        lua_pop(L, 1);
                    }
                    keys.push_back(key);
                }
                std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b){
                    if (a.isInteger != b.isInteger) return a.isInteger;
                    if (a.isInteger) return a.integer < b.integer;
                    if (a.prefix != b.prefix) return a.prefix < b.prefix;
                    return a.text < b.text;
                });
                pairs = keys.size();
            }

            open.push_back(self);
            out.push_back(static_cast<char>(TagTable));
            putVarint(out, static_cast<uint64_t>(n));
            // the keys down to the value being written: this level's slot, rewritten per value
            steps.push_back({true, 0, {}});
            for (lua_Integer i = 1; i <= n; i++){
                lua_rawgeti(L, index, i);
                steps.back() = {true, i, {}};
                const bool ok = value(-1, depth);
                lua_pop(L, 1);
                if (!ok) return false;
            }
            putVarint(out, pairs);
            // indexed again for each pair: the values' tables (the next depth) may grow keyLists
            for (size_t k = 0; k < pairs; k++){
                const Key key = keyLists[static_cast<size_t>(depth)][k];
                if (key.isInteger){
                    putInteger(out, static_cast<int64_t>(key.integer));
                    steps.back() = {true, key.integer, {}};
                } else {
                    putString(out, key.text);
                    steps.back() = {false, 0, key.text};
                }
                bool ok;
                if (key.slot != 0){
                    ok = value(key.slot, depth);
                } else {
                    if (key.isInteger) lua_rawgeti(L, index, key.integer);
                    else {
                        lua_pushlstring(L, key.text.data(), key.text.size());
                        lua_rawget(L, index);
                    }
                    ok = value(-1, depth);
                    lua_pop(L, 1);
                }
                if (!ok) return false;
            }
            steps.pop_back();
            lua_settop(L, base);
            open.pop_back();
            return true;
        }
    };

    // ---------------------------------------------------------------- bytes -> values

    struct Reader {
        const unsigned char* data;
        size_t size;
        size_t pos = 0;
        SaveError& error;

        Reader(std::string_view bytes, SaveError& error)
            : data(reinterpret_cast<const unsigned char*>(bytes.data())), size(bytes.size()), error(error){}

        size_t left() const { return size - pos; }

        bool failAt(size_t offset, const std::string& what){
            error = makeError(SaveErrorCode::Corrupt, what + " at offset " + std::to_string(offset));
            return false;
        }

        bool byte(uint8_t& out){
            if (pos >= size) return failAt(pos, "value cut short");
            out = data[pos++];
            return true;
        }

        bool varint(uint64_t& out){
            const size_t start = pos;
            out = 0;
            for (int i = 0; i < 10; i++){
                if (pos >= size) return failAt(start, "varint cut short");
                const uint8_t b = data[pos++];
                if (i == 9 && (b & 0x80)) return failAt(start, "varint longer than 10 bytes");
                if (i == 9 && b > 1) return failAt(start, "varint overflows 64 bits");
                out |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
                if (!(b & 0x80)) return true;
            }
            return failAt(start, "varint longer than 10 bytes");
        }

        bool fixed(int bytes, uint64_t& out){
            if (left() < static_cast<size_t>(bytes)) return failAt(pos, "value cut short");
            out = 0;
            for (int i = 0; i < bytes; i++) out |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
            pos += bytes;
            return true;
        }

        bool float32(float& out){
            uint64_t bits = 0;
            if (!fixed(4, bits)) return false;
            const uint32_t b32 = static_cast<uint32_t>(bits);
            std::memcpy(&out, &b32, sizeof(out));
            return true;
        }

        bool float64(double& out){
            uint64_t bits = 0;
            if (!fixed(8, bits)) return false;
            std::memcpy(&out, &bits, sizeof(out));
            return true;
        }

        bool string(std::string_view& out){
            const size_t start = pos;
            uint64_t length = 0;
            if (!varint(length)) return false;
            if (length > left()) return failAt(start, "bad string length " + std::to_string(length));
            out = std::string_view(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(length));
            pos += static_cast<size_t>(length);
            return true;
        }

        // Table counts: every value takes at least a byte, every pair two
        bool counts(uint64_t& count, uint64_t perItem, const char* what){
            const size_t start = pos;
            if (!varint(count)) return false;
            if (count > left() / perItem) return failAt(start, std::string("bad ") + what + " " + std::to_string(count));
            return true;
        }

        bool unknownTag(uint8_t tag){
            char hex[8];
            std::snprintf(hex, sizeof(hex), "0x%02X", tag);
            return failAt(pos - 1, std::string("unknown tag ") + hex);
        }

        bool depthCap(int depth){
            if (depth > MaxDepth) return failAt(pos - 1, "nesting deeper than " + std::to_string(MaxDepth));
            return true;
        }

        // A key of a table's pair part: an integer or a string
        bool keyTag(uint8_t& tag){
            if (!byte(tag)) return false;
            if (tag != TagInteger && tag != TagString) return failAt(pos - 1, "a table key that is no integer or string");
            return true;
        }

        bool finish(){
            if (pos != size) return failAt(pos, std::to_string(size - pos) + " bytes after the value");
            return true;
        }
    };

    bool skipValue(Reader& r, int depth){
        uint8_t tag = 0;
        if (!r.byte(tag)) return false;
        uint64_t scratch = 0;
        std::string_view text;
        switch (tag){
            case TagFalse: case TagTrue: case TagNull: return true;
            case TagInteger: return r.varint(scratch);
            case TagFloat: return r.fixed(8, scratch);
            case TagString: return r.string(text);
            case TagVector2: return r.fixed(4, scratch) && r.fixed(4, scratch);
            case TagVector3: return r.fixed(4, scratch) && r.fixed(4, scratch) && r.fixed(4, scratch);
            case TagVector4: case TagQuaternion:
                return r.fixed(4, scratch) && r.fixed(4, scratch) && r.fixed(4, scratch) && r.fixed(4, scratch);
            case TagTable: {
                if (!r.depthCap(depth + 1)) return false;
                uint64_t n = 0, m = 0;
                if (!r.counts(n, 1, "array count")) return false;
                for (uint64_t i = 0; i < n; i++) if (!skipValue(r, depth + 1)) return false;
                if (!r.counts(m, 2, "pair count")) return false;
                for (uint64_t i = 0; i < m; i++){
                    uint8_t keyTag = 0;
                    if (!r.keyTag(keyTag)) return false;
                    if (keyTag == TagInteger ? !r.varint(scratch) : !r.string(text)) return false;
                    if (!skipValue(r, depth + 1)) return false;
                }
                return true;
            }
            default:
                return r.unknownTag(tag);
        }
    }

    // A value pushed on L (nothing on failure)
    bool pushValue(lua_State* L, Reader& r, int depth){
        if (!lua_checkstack(L, 4)) return r.failAt(r.pos, "nesting too deep for the Lua stack");
        uint8_t tag = 0;
        if (!r.byte(tag)) return false;
        switch (tag){
            case TagFalse: lua_pushboolean(L, 0); return true;
            case TagTrue: lua_pushboolean(L, 1); return true;
            case TagNull: LuaJson::pushNull(L); return true;
            case TagInteger: {
                uint64_t v = 0;
                if (!r.varint(v)) return false;
                lua_pushinteger(L, static_cast<lua_Integer>(unzigzag(v)));
                return true;
            }
            case TagFloat: {
                double d = 0;
                if (!r.float64(d)) return false;
                lua_pushnumber(L, static_cast<lua_Number>(d));
                return true;
            }
            case TagString: {
                std::string_view s;
                if (!r.string(s)) return false;
                lua_pushlstring(L, s.data(), s.size());
                return true;
            }
            case TagVector2: {
                float x = 0, y = 0;
                if (!r.float32(x) || !r.float32(y)) return false;
                if (!luabridge::push(L, Vector2(x, y))) return r.failAt(r.pos, "cannot push a vector");
                return true;
            }
            case TagVector3: {
                float x = 0, y = 0, z = 0;
                if (!r.float32(x) || !r.float32(y) || !r.float32(z)) return false;
                if (!luabridge::push(L, Vector3(x, y, z))) return r.failAt(r.pos, "cannot push a vector");
                return true;
            }
            case TagVector4: {
                float x = 0, y = 0, z = 0, w = 0;
                if (!r.float32(x) || !r.float32(y) || !r.float32(z) || !r.float32(w)) return false;
                if (!luabridge::push(L, Vector4(x, y, z, w))) return r.failAt(r.pos, "cannot push a vector");
                return true;
            }
            case TagQuaternion: {
                float x = 0, y = 0, z = 0, w = 0;
                if (!r.float32(x) || !r.float32(y) || !r.float32(z) || !r.float32(w)) return false;
                if (!luabridge::push(L, Quaternion(w, x, y, z))) return r.failAt(r.pos, "cannot push a vector");
                return true;
            }
            case TagTable: {
                if (!r.depthCap(depth + 1)) return false;
                uint64_t n = 0;
                if (!r.counts(n, 1, "array count")) return false;
                const int top = lua_gettop(L);
                // the array's values wait on the stack until the pair count is read, so the table is
                // made with both parts sized (a hash part grown key by key rehashes log2(m) times, a
                // tenth of a large decode); a stack too small for them fills the table as it reads
                const bool staged = n <= (1u << 20) && lua_checkstack(L, static_cast<int>(n) + 4);
                int table = 0;
                if (!staged){
                    lua_createtable(L, static_cast<int>(std::min<uint64_t>(n, 1u << 20)), 0);
                    table = lua_gettop(L);
                }
                for (uint64_t i = 0; i < n; i++){
                    if (!pushValue(L, r, depth + 1)){ lua_settop(L, top); return false; }
                    if (!staged) lua_rawseti(L, table, static_cast<lua_Integer>(i + 1));
                }
                uint64_t m = 0;
                if (!r.counts(m, 2, "pair count")){ lua_settop(L, top); return false; }
                if (staged){
                    lua_createtable(L, static_cast<int>(n), static_cast<int>(std::min<uint64_t>(m, 1u << 20)));
                    table = top + 1;
                    lua_insert(L, table);
                    for (lua_Integer i = static_cast<lua_Integer>(n); i >= 1; i--) lua_rawseti(L, table, i);
                }
                for (uint64_t i = 0; i < m; i++){
                    uint8_t keyTag = 0;
                    if (!r.keyTag(keyTag)){ lua_settop(L, top); return false; }
                    if (keyTag == TagInteger){
                        uint64_t v = 0;
                        if (!r.varint(v)){ lua_settop(L, top); return false; }
                        lua_pushinteger(L, static_cast<lua_Integer>(unzigzag(v)));
                    } else {
                        std::string_view s;
                        if (!r.string(s)){ lua_settop(L, top); return false; }
                        lua_pushlstring(L, s.data(), s.size());
                    }
                    if (!pushValue(L, r, depth + 1)){ lua_settop(L, top); return false; }
                    lua_rawset(L, table);
                }
                return true;
            }
            default:
                return r.unknownTag(tag);
        }
    }

    // ---------------------------------------------------------------- bytes -> JSON

    std::string base64(std::string_view bytes){
        return Base64::encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    }

    // Strict: only the canonical text of some bytes decodes (so the view stays a bijection)
    bool unbase64(const std::string& text, std::string& out){
        const std::vector<unsigned char> bytes = Base64::decode(text);
        out.assign(bytes.begin(), bytes.end());
        return base64(out) == text;
    }

    nlohmann::json floatJson(double d){
        if (std::isfinite(d)) return d;
        if (std::isinf(d)) return {{"$float", d > 0 ? "inf" : "-inf"}};
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        if (bits == CANONICAL_NAN) return {{"$float", "nan"}};
        char hex[32];
        std::snprintf(hex, sizeof(hex), "nan:%016llx", static_cast<unsigned long long>(bits));
        return {{"$float", hex}};
    }

    nlohmann::json stringJson(std::string_view s){
        if (validUtf8(s)) return std::string(s);
        return {{"$bytes", base64(s)}};
    }

    std::string keyJson(std::string_view key){
        if (!validUtf8(key)) return "$bytes:" + base64(key);
        if (!key.empty() && (key[0] == '#' || key[0] == '$')) return std::string(1, key[0]) + std::string(key);
        return std::string(key);
    }

    bool jsonValue(Reader& r, nlohmann::json& out, int depth){
        uint8_t tag = 0;
        if (!r.byte(tag)) return false;
        switch (tag){
            case TagFalse: out = false; return true;
            case TagTrue: out = true; return true;
            case TagNull: out = nullptr; return true;
            case TagInteger: {
                uint64_t v = 0;
                if (!r.varint(v)) return false;
                out = unzigzag(v);
                return true;
            }
            case TagFloat: {
                double d = 0;
                if (!r.float64(d)) return false;
                out = floatJson(d);
                return true;
            }
            case TagString: {
                std::string_view s;
                if (!r.string(s)) return false;
                out = stringJson(s);
                return true;
            }
            case TagVector2: case TagVector3: case TagVector4: case TagQuaternion: {
                const int count = tag == TagVector2 ? 2 : tag == TagVector3 ? 3 : 4;
                const char* name = tag == TagVector2 ? "$vec2" : tag == TagVector3 ? "$vec3" : tag == TagVector4 ? "$vec4" : "$quat";
                nlohmann::json items = nlohmann::json::array();
                for (int i = 0; i < count; i++){
                    float f = 0;
                    if (!r.float32(f)) return false;
                    items.push_back(floatJson(static_cast<double>(f)));
                }
                out = {{name, std::move(items)}};
                return true;
            }
            case TagTable: {
                if (!r.depthCap(depth + 1)) return false;
                uint64_t n = 0;
                if (!r.counts(n, 1, "array count")) return false;
                std::vector<nlohmann::json> arrayPart;
                arrayPart.reserve(static_cast<size_t>(std::min<uint64_t>(n, 4096)));
                for (uint64_t i = 0; i < n; i++){
                    nlohmann::json item;
                    if (!jsonValue(r, item, depth + 1)) return false;
                    arrayPart.push_back(std::move(item));
                }
                uint64_t m = 0;
                if (!r.counts(m, 2, "pair count")) return false;
                if (m == 0 && n > 0){
                    out = nlohmann::json::array();
                    for (auto& item : arrayPart) out.push_back(std::move(item));
                    return true;
                }
                out = nlohmann::json::object();
                for (uint64_t i = 0; i < n; i++) out["#" + std::to_string(i + 1)] = std::move(arrayPart[i]);
                for (uint64_t i = 0; i < m; i++){
                    uint8_t keyTag = 0;
                    if (!r.keyTag(keyTag)) return false;
                    std::string key;
                    if (keyTag == TagInteger){
                        uint64_t v = 0;
                        if (!r.varint(v)) return false;
                        key = "#" + std::to_string(unzigzag(v));
                    } else {
                        std::string_view s;
                        if (!r.string(s)) return false;
                        key = keyJson(s);
                    }
                    nlohmann::json item;
                    if (!jsonValue(r, item, depth + 1)) return false;
                    out[key] = std::move(item);
                }
                return true;
            }
            default:
                return r.unknownTag(tag);
        }
    }

    // ---------------------------------------------------------------- JSON -> bytes

    struct JsonEncoder {
        SaveError& error;
        std::string path = "$";

        bool fail(const std::string& what){
            error = makeError(SaveErrorCode::Encode, what + " at " + path, path);
            return false;
        }

        bool number(const nlohmann::json& item, double& out){
            if (item.is_number()){
                out = item.get<double>();
                return true;
            }
            if (item.is_object() && item.size() == 1 && item.contains("$float") && item["$float"].is_string())
                return special(item["$float"].get<std::string>(), out);
            return false;
        }

        bool special(const std::string& text, double& out){
            if (text == "nan"){
                std::memcpy(&out, &CANONICAL_NAN, sizeof(out));
                return true;
            }
            if (text == "inf"){ out = std::numeric_limits<double>::infinity(); return true; }
            if (text == "-inf"){ out = -std::numeric_limits<double>::infinity(); return true; }
            if (text.size() == 20 && text.compare(0, 4, "nan:") == 0){
                uint64_t bits = 0;
                for (size_t i = 4; i < text.size(); i++){
                    const char c = text[i];
                    int digit;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                    else return false;
                    bits = (bits << 4) | static_cast<uint64_t>(digit);
                }
                std::memcpy(&out, &bits, sizeof(out));
                return std::isnan(out);
            }
            return false;
        }

        bool vector(const std::string& name, const nlohmann::json& items, std::string& out){
            const int count = name == "$vec2" ? 2 : name == "$vec3" ? 3 : 4;
            const char* shape = count == 2 ? "[x, y]" : count == 3 ? "[x, y, z]" : "[x, y, z, w]";
            if (!items.is_array() || items.size() != static_cast<size_t>(count))
                return fail("\"" + name + "\" must be an array of " + std::to_string(count) + " numbers " + shape);
            float values[4] = {0, 0, 0, 0};
            for (int i = 0; i < count; i++){
                double d = 0;
                if (!number(items[static_cast<size_t>(i)], d))
                    return fail("\"" + name + "\" must be an array of " + std::to_string(count) + " numbers " + shape);
                values[i] = static_cast<float>(d);
            }
            const Tag tag = count == 2 ? TagVector2 : count == 3 ? TagVector3 : name == "$quat" ? TagQuaternion : TagVector4;
            out.push_back(static_cast<char>(tag));
            for (int i = 0; i < count; i++) putFloat32(out, values[i]);
            return true;
        }

        bool value(const nlohmann::json& item, std::string& out, int depth){
            switch (item.type()){
                case nlohmann::json::value_t::null: out.push_back(static_cast<char>(TagNull)); return true;
                case nlohmann::json::value_t::boolean:
                    out.push_back(static_cast<char>(item.get<bool>() ? TagTrue : TagFalse));
                    return true;
                case nlohmann::json::value_t::number_integer:
                    putInteger(out, item.get<int64_t>());
                    return true;
                case nlohmann::json::value_t::number_unsigned: {
                    const uint64_t u = item.get<uint64_t>();
                    if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                        return fail("integer " + std::to_string(u) + " is above 2^63-1");
                    putInteger(out, static_cast<int64_t>(u));
                    return true;
                }
                case nlohmann::json::value_t::number_float:
                    putFloat(out, item.get<double>());
                    return true;
                case nlohmann::json::value_t::string:
                    putString(out, item.get_ref<const std::string&>());
                    return true;
                case nlohmann::json::value_t::array: {
                    if (depth + 1 > MaxDepth) return fail("nesting deeper than " + std::to_string(MaxDepth));
                    out.push_back(static_cast<char>(TagTable));
                    putVarint(out, item.size());
                    const size_t length = path.size();
                    for (size_t i = 0; i < item.size(); i++){
                        path += "[" + std::to_string(i + 1) + "]";
                        if (!value(item[i], out, depth + 1)) return false;
                        path.resize(length);
                    }
                    putVarint(out, 0);
                    return true;
                }
                case nlohmann::json::value_t::object:
                    return object(item, out, depth);
                default:
                    return fail("unsupported JSON value");
            }
        }

        struct Entry {
            bool isInteger;
            int64_t integer;
            std::string text;
            const nlohmann::json* value;
            std::string jsonKey;
        };

        bool object(const nlohmann::json& item, std::string& out, int depth){
            if (item.size() == 1){
                const std::string& key = item.begin().key();
                if (key.size() > 1 && key[0] == '$' && key[1] != '$' && key.compare(0, 7, "$bytes:") != 0){
                    const nlohmann::json& inner = item.begin().value();
                    if (key == "$vec2" || key == "$vec3" || key == "$vec4" || key == "$quat") return vector(key, inner, out);
                    if (key == "$float"){
                        double d = 0;
                        if (!inner.is_string() || !special(inner.get<std::string>(), d))
                            return fail("\"$float\" must be \"nan\", \"inf\" or \"-inf\"");
                        putFloat(out, d);
                        return true;
                    }
                    if (key == "$bytes"){
                        std::string bytes;
                        if (!inner.is_string() || !unbase64(inner.get<std::string>(), bytes))
                            return fail("\"$bytes\" must be base64 text");
                        putString(out, bytes);
                        return true;
                    }
                    return unknownDollar(key);
                }
            }
            if (depth + 1 > MaxDepth) return fail("nesting deeper than " + std::to_string(MaxDepth));
            std::vector<Entry> entries;
            entries.reserve(item.size());
            for (auto it = item.begin(); it != item.end(); ++it){
                const std::string& key = it.key();
                Entry entry{false, 0, {}, &it.value(), key};
                if (!key.empty() && key[0] == '#' && !(key.size() > 1 && key[1] == '#')){
                    if (!parseInteger(key.substr(1), entry.integer)) return fail("bad integer key \"" + key + "\" (integer keys are \"#<n>\"; a key starting with # is written ##)");
                    entry.isInteger = true;
                } else if (!key.empty() && (key[0] == '#' || key[0] == '$') && key.size() > 1 && key[1] == key[0]){
                    entry.text = key.substr(1);
                } else if (key.compare(0, 7, "$bytes:") == 0){
                    if (!unbase64(key.substr(7), entry.text)) return fail("bad key \"" + key + "\" (\"$bytes:\" takes base64 text)");
                } else if (!key.empty() && key[0] == '$'){
                    return unknownDollar(key);
                } else {
                    entry.text = key;
                }
                entries.push_back(std::move(entry));
            }
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b){
                if (a.isInteger != b.isInteger) return a.isInteger;
                if (a.isInteger) return a.integer < b.integer;
                return a.text < b.text;
            });
            for (size_t i = 1; i < entries.size(); i++){
                const Entry& a = entries[i - 1];
                const Entry& b = entries[i];
                if (a.isInteger == b.isInteger && a.integer == b.integer && a.text == b.text)
                    return fail("the key \"" + b.jsonKey + "\" is given twice (as \"" + a.jsonKey + "\")");
            }
            size_t n = 0;
            while (n < entries.size() && entries[n].isInteger && entries[n].integer == static_cast<int64_t>(n + 1)) n++;
            out.push_back(static_cast<char>(TagTable));
            putVarint(out, n);
            const size_t length = path.size();
            for (size_t i = 0; i < entries.size(); i++){
                if (i == n) putVarint(out, entries.size() - n);
                const Entry& entry = entries[i];
                if (i >= n){
                    if (entry.isInteger) putInteger(out, entry.integer);
                    else putString(out, entry.text);
                }
                path += entry.isInteger ? "[" + std::to_string(entry.integer) + "]" : stringKeyPath(entry.text);
                if (!value(*entry.value, out, depth + 1)) return false;
                path.resize(length);
            }
            if (n == entries.size()) putVarint(out, 0);
            return true;
        }

        bool unknownDollar(const std::string& key){
            return fail("unknown key \"" + key + "\" in SaveGame JSON (valid: $vec2, $vec3, $vec4, $quat, $float, $bytes; "
                        "a key starting with $ is written $$)");
        }

        static bool parseInteger(const std::string& text, int64_t& out){
            if (text.empty() || text.size() > 20) return false;
            size_t i = 0;
            bool negative = false;
            if (text[0] == '-'){ negative = true; i = 1; }
            if (i >= text.size()) return false;
            // canonical decimal only ("#01" would not come back the same)
            if (text[i] == '0' && text.size() > i + 1) return false;
            if (negative && text == "-0") return false;
            uint64_t magnitude = 0;
            for (; i < text.size(); i++){
                if (text[i] < '0' || text[i] > '9') return false;
                const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
                if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
                magnitude = magnitude * 10 + digit;
            }
            const uint64_t limit = negative ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1
                                            : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            if (magnitude > limit) return false;
            out = negative ? static_cast<int64_t>(0 - magnitude) : static_cast<int64_t>(magnitude);
            return true;
        }
    };

}

SaveError makeError(SaveErrorCode code, std::string message, std::string path){
    SaveError error;
    error.code = code;
    error.message = std::move(message);
    error.path = std::move(path);
    return error;
}

const char* errorCodeName(SaveErrorCode code){
    switch (code){
        case SaveErrorCode::None: return "";
        case SaveErrorCode::Name: return "name";
        case SaveErrorCode::Encode: return "encode";
        case SaveErrorCode::TooLarge: return "too_large";
        case SaveErrorCode::NotFound: return "not_found";
        case SaveErrorCode::Corrupt: return "corrupt";
        case SaveErrorCode::NewerVersion: return "newer_version";
        case SaveErrorCode::NewerFormat: return "newer_format";
        case SaveErrorCode::Migration: return "migration";
        case SaveErrorCode::Io: return "io";
        case SaveErrorCode::Quota: return "quota";
        case SaveErrorCode::Unavailable: return "unavailable";
        case SaveErrorCode::Cancelled: return "cancelled";
    }
    return "";
}

bool encodeLua(lua_State* L, int index, std::string& out, SaveError& error){
    const int top = lua_gettop(L);
    index = lua_absindex(L, index);
    LuaEncoder encoder{L, out, error};
    const size_t start = out.size();
    const bool ok = encoder.value(index, 0);
    lua_settop(L, top);
    if (!ok) out.resize(start);
    return ok;
}

bool decodeLua(lua_State* L, std::string_view payload, SaveError& error){
    Reader reader(payload, error);
    const int top = lua_gettop(L);
    const bool ok = pushValue(L, reader, 0) && reader.finish();
    if (!ok) lua_settop(L, top);
    return ok;
}

bool validatePayload(std::string_view payload, SaveError& error){
    Reader reader(payload, error);
    return skipValue(reader, 0) && reader.finish();
}

bool toJson(std::string_view payload, nlohmann::json& out, SaveError& error){
    Reader reader(payload, error);
    return jsonValue(reader, out, 0) && reader.finish();
}

bool fromJson(const nlohmann::json& value, std::string& payload, SaveError& error){
    JsonEncoder encoder{error};
    std::string out;
    if (!encoder.value(value, out, 0)) return false;
    payload = std::move(out);
    return true;
}

void beginPairTable(std::string& out, uint64_t pairCount){
    out.push_back(static_cast<char>(TagTable));
    putVarint(out, 0);
    putVarint(out, pairCount);
}

void appendString(std::string& out, std::string_view text){
    putString(out, text);
}

void appendInteger(std::string& out, int64_t value){
    putInteger(out, value);
}

bool findField(std::string_view payload, std::string_view key, std::string_view& value){
    SaveError ignored;
    Reader r(payload, ignored);
    uint8_t tag = 0;
    if (!r.byte(tag) || tag != TagTable) return false;
    uint64_t n = 0, m = 0;
    if (!r.counts(n, 1, "array count")) return false;
    for (uint64_t i = 0; i < n; i++) if (!skipValue(r, 1)) return false;
    if (!r.counts(m, 2, "pair count")) return false;
    for (uint64_t i = 0; i < m; i++){
        uint8_t keyTag = 0;
        if (!r.keyTag(keyTag)) return false;
        bool match = false;
        if (keyTag == TagInteger){
            uint64_t scratch = 0;
            if (!r.varint(scratch)) return false;
        } else {
            std::string_view s;
            if (!r.string(s)) return false;
            match = s == key;
        }
        const size_t start = r.pos;
        if (!skipValue(r, 1)) return false;
        if (match){
            value = payload.substr(start, r.pos - start);
            return true;
        }
    }
    return false;
}

bool readString(std::string_view payload, std::string& out){
    SaveError ignored;
    Reader r(payload, ignored);
    uint8_t tag = 0;
    std::string_view s;
    if (!r.byte(tag) || tag != TagString || !r.string(s) || r.pos != r.size) return false;
    out.assign(s.data(), s.size());
    return true;
}

bool readInteger(std::string_view payload, int64_t& out){
    SaveError ignored;
    Reader r(payload, ignored);
    uint8_t tag = 0;
    uint64_t v = 0;
    if (!r.byte(tag) || tag != TagInteger || !r.varint(v) || r.pos != r.size) return false;
    out = unzigzag(v);
    return true;
}

}  // namespace jasperengine::savegame
