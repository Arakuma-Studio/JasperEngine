// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUAJSON_H
#define LUAJSON_H

#include "Export.h"
#include "json.hpp"

#include <string>
#include <string_view>

typedef struct lua_State lua_State;

// The single JSON <-> Lua conversion of the engine: the `Json` Lua module (IOClassesLua.cpp)
// and every C++ feature that hands JSON to Lua or takes it from Lua (HTTP bodies, native
// bridge payloads) use these functions, so the rules are the same everywhere
// (docs/mcp/guide/lua-api.md, "Json"):
// - a table whose keys are exactly 1..n (n >= 1) is an array; an empty table is `[]` (or `{}`
//   with emptyAsObject); any other table is an object (integer keys as decimal strings);
//   `Json.array(t)` / `Json.object(t)` mark a table (a `__jsontype` field in its metatable);
// - object keys are written sorted (byte order);
// - Lua integers <-> JSON integers (int64); a float keeps its decimal point ("3.0"); NaN and
//   infinities are an error;
// - `Json.null` (a unique userdata) <-> null;
// - Vector2/3/4 and Quaternion are objects {x, y[, z[, w]]}; a metatable `__tojson(self)` gives
//   the value to write instead;
// - errors name the place: "cannot encode a function at $.player.onHit" (array indices as Lua
//   indices: $.items[1]).
// Main thread only (the Lua state is single-threaded); no global state besides the per-state
// registry entries of the null sentinel and the marker metatables.
namespace jasperengine::LuaJson {

    enum class InvalidUtf8 { Error, Replace, Ignore };

    struct EncodeOptions {
        int indent = -1;                        // -1 compact; >= 0: pretty, that many spaces
        InvalidUtf8 utf8 = InvalidUtf8::Error;  // Replace: U+FFFD; Ignore: the bytes dropped
        bool emptyAsObject = false;             // an unmarked empty table is {} instead of []
    };

    struct DecodeOptions {
        bool nullAsNil = false;      // null -> nil: object members omitted, array holes
        bool allowComments = false;  // // and /* */ comments
    };

    constexpr int MaxDepth = 512;

    // Lua value at `index` -> json (strings are valid UTF-8 afterwards, as `utf8` says). False
    // with `error` ("cannot encode a function at $.a[2]", "cycle at $.next"); the stack is
    // left as it was either way.
    JASPERENGINE_API bool toJson(lua_State* L, int index, nlohmann::json& out, const EncodeOptions& opts, std::string& error);
    // json -> one Lua value pushed (empty arrays and objects get the Json.array / Json.object
    // marker, so they encode back the same). Values nested deeper than MaxDepth (only a json
    // built in C++ can have them: parse refuses them) become Json.null.
    JASPERENGINE_API void push(lua_State* L, const nlohmann::json& value, const DecodeOptions& opts = {});

    // toJson + dump
    JASPERENGINE_API bool encode(lua_State* L, int index, std::string& out, const EncodeOptions& opts, std::string& error);
    // Text -> json: false with `error` ("syntax error at line 3, column 14: unexpected '}';
    // expected string literal", "nesting deeper than 512 at line 1, column 513")
    JASPERENGINE_API bool parse(std::string_view text, nlohmann::json& out, const DecodeOptions& opts, std::string& error);
    // parse + push: pushes one value on success, nothing on failure
    JASPERENGINE_API bool decode(lua_State* L, std::string_view text, const DecodeOptions& opts, std::string& error);

    // Json.null
    JASPERENGINE_API void pushNull(lua_State* L);
    JASPERENGINE_API bool isNull(lua_State* L, int index);

    // Json.array(t) / Json.object(t): marks the table at `index` (a table with a metatable of
    // its own gets `__jsontype` set in it)
    JASPERENGINE_API void markArray(lua_State* L, int index);
    JASPERENGINE_API void markObject(lua_State* L, int index);

}

#endif // LUAJSON_H
