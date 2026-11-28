// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SAVE_SAVEVALUE_H
#define SAVE_SAVEVALUE_H

// JLV1, the value codec of save files (docs/saved-data.md, docs/specs/savegame.md R20): one Lua
// value as bytes, bit-exact (integers, IEEE doubles, binary strings, integer and string keys,
// Vector2/3/4, Quaternion, Json.null), and equal values give equal bytes. One tag byte per value:
//   0x00 false  0x01 true  0x02 integer (zigzag LEB128)  0x03 float (8 bytes LE)
//   0x04 string (varint length, bytes)
//   0x05 table: varint n, the values of keys 1..n (the longest run of non-nil values); varint m,
//        then m key/value pairs, integer keys ascending, then string keys in byte order
//   0x06 Vector2  0x07 Vector3  0x08 Vector4  0x09 Quaternion (x, y, z, w): float32 LE each
//   0x0A Json.null
// An unknown tag is a decode error: a new tag needs format version 2.
//
// The JSON view (toJson / fromJson) is an exact bijection for the editor and MCP (R21).

#include "Export.h"
#include "json.hpp"

#include <cstdint>
#include <string>
#include <string_view>

typedef struct lua_State lua_State;

namespace jasperengine::savegame {

    enum class SaveErrorCode { None, Name, Encode, TooLarge, NotFound, Corrupt, NewerVersion, NewerFormat,
                               Migration, Io, Quota, Unavailable, Cancelled };

    struct SaveError {
        SaveErrorCode code = SaveErrorCode::None;
        std::string message;
        // Where in the value an encode failed ("$.player.onHit"), else empty
        std::string path;

        bool ok() const { return code == SaveErrorCode::None; }
    };

    JASPERENGINE_API SaveError makeError(SaveErrorCode code, std::string message, std::string path = {});
    // "too_large", "newer_version" ...; "" for None
    JASPERENGINE_API const char* errorCodeName(SaveErrorCode code);

    // Nesting cap of tables, both ways (R2, R19)
    constexpr int MaxDepth = 64;

    // The Lua value at `index` appended to `out`. False with code Encode and the path ("cannot
    // save a function at $.player.onHit", "cycle at $.a.parent"); the stack is left as it was.
    JASPERENGINE_API bool encodeLua(lua_State* L, int index, std::string& out, SaveError& error);
    // One value pushed on success; nothing pushed on failure (code Corrupt, naming the offset)
    JASPERENGINE_API bool decodeLua(lua_State* L, std::string_view payload, SaveError& error);
    // The payload is exactly one well-formed value within the caps (no Lua, no allocation)
    JASPERENGINE_API bool validatePayload(std::string_view payload, SaveError& error);

    JASPERENGINE_API bool toJson(std::string_view payload, nlohmann::json& out, SaveError& error);
    JASPERENGINE_API bool fromJson(const nlohmann::json& value, std::string& payload, SaveError& error);

    // A table of string keys only, built from C++ (the file metadata): after this, `pairCount`
    // times appendString(key) and the value's payload, the keys in byte order
    JASPERENGINE_API void beginPairTable(std::string& out, uint64_t pairCount);
    JASPERENGINE_API void appendString(std::string& out, std::string_view text);
    JASPERENGINE_API void appendInteger(std::string& out, int64_t value);
    // The value of a string key of an encoded table (no copy: a view into `payload`); false when
    // the payload is no table or lacks the key
    JASPERENGINE_API bool findField(std::string_view payload, std::string_view key, std::string_view& value);
    // A string payload's text / an integer payload's value
    JASPERENGINE_API bool readString(std::string_view payload, std::string& out);
    JASPERENGINE_API bool readInteger(std::string_view payload, int64_t& out);

}

#endif // SAVE_SAVEVALUE_H
