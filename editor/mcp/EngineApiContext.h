// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include <string>

namespace jasperengine::editor::mcp {

// The curated engine API index (generated from the Lua bindings and C++ headers) behind
// search_engine_api
class EngineApiContext {
public:
    static Json search(const std::string& query,
                       const std::string& parentFilter,
                       int maxResults);
};

} // namespace jasperengine::editor::mcp
