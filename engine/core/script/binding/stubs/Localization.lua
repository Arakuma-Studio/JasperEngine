---@meta _
-- Hand-written shapes of Localization's arguments (engine/core/script/binding/LocalizationClassesLua.cpp,
-- S95 String Tables), appended to the generated stubs. Rules: docs/mcp/guide/lua-api.md, "Localization".

---The arguments of Localization.get and UIElement:setLocalized: the list part fills {0} {1}...,
---the string keys fill {name}; a plural value picks its form by `count` (else the first of the list).
---Values are strings, numbers or booleans.
---@alias LocalizationArgs table<string|integer, string|number|boolean>
