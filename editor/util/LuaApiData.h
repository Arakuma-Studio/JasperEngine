// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The engine's Lua API as data (S23 R29, R46): the lua-api.json tools/lua_api writes at build
// time, embedded. The jasper://lua-api resource, `doc lua-api-reference`, the project's
// .jasperengine/engine-api/lua-api.json and search_engine_api read it.

#include "mcp/McpJson.h"

#include <string>

namespace jasperengine::editor {

    class LuaApiData {
    public:
        // lua-api.json with engineVersion set to this editor's
        static const mcp::Json& json();
        static std::string jsonText();
        // the Markdown reference
        static const std::string& markdown();
        // one class by its Lua name (null when there is none)
        static mcp::Json findClass(const std::string& name);
    };

}
