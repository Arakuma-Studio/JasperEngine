// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "LegacyTypes.h"

#include <string>

namespace jasperengine::editor::mcp::legacy {

class EngineApiContext {
public:
    static Json search(const std::string& query,
                       const std::string& parentFilter,
                       int maxResults);
};

} // namespace jasperengine::editor::mcp::legacy
