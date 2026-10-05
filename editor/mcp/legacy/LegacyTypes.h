// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <string>

// Types the editor actions of the former in-editor AI agent were written against.
// The actions survive as MCP tools (see tools/LegacyTools.cpp) until the workstream
// tools replace them.
namespace jasperengine::editor::mcp::legacy {

using Json = nlohmann::json;

struct ToolDefinition {
    std::string name;
    std::string description;
    Json parameters = Json::object();
    bool readOnly = true;
};

enum class ActionFailureSeverity {
    // An exploratory lookup did not find the requested scene/entity/component.
    // The caller should see the miss, but it is not an editor diagnostic.
    ExpectedMiss,
    // The request was invalid or unsupported, without indicating an editor or
    // project failure.
    Warning,
    // The editor, project, filesystem, or external dependency failed.
    Error
};

struct ActionResult {
    bool success = false;
    std::string message;
    Json data = Json::object();
    ActionFailureSeverity failureSeverity = ActionFailureSeverity::Error;
};

} // namespace jasperengine::editor::mcp::legacy
