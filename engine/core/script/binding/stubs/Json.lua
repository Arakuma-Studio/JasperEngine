---@meta _
-- Hand-written shapes of the Json module (engine/core/script/binding/IOClassesLua.cpp, feature
-- script-data), appended to the generated stubs. Rules: docs/mcp/guide/lua-api.md, "Json".

---The JSON null (`Json.null`): a unique value, so arrays keep their length and objects their
---keys. `Json.isNull(v)` or `v == Json.null` tests it; `tostring(Json.null)` is "Json.null".
---@class JsonNull

---What Json.decode returns: tables for objects and arrays, strings, integers and floats,
---booleans, Json.null (nil with `null = "nil"`)
---@alias JsonValue table|string|number|boolean|JsonNull|nil

---@class JsonEncodeOptions
---@field pretty? boolean # indent 2 (default false: compact)
---@field indent? integer # spaces per level, 0..64 (implies pretty)
---@field invalidUtf8? "error"|"replace"|"ignore" # strings that are no UTF-8: raise (default), U+FFFD, or drop the bytes
---@field emptyTable? "array"|"object" # an unmarked empty table: [] (default) or {}

---@class JsonDecodeOptions
---@field null? "sentinel"|"nil" # JSON null: Json.null (default) or nil (object members omitted, array holes)
---@field allowComments? boolean # // and /* */ comments (default false)
