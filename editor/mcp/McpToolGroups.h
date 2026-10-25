// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    // A named set of tools that tools/list hides until a session enables it (tool_groups,
    // `mcp --tool-groups`, settings mcp.tool_groups). Hiding only saves the client's context:
    // tools/call and batch reach every registered tool whatever the session enabled.
    // The limits of each group (characters, tool count) live in tools/mcp/budget.json, the
    // single source of the tool budget (docs/mcp/conventions.md).
    struct ToolGroupInfo {
        std::string name;          // snake_case, e.g. "profiling"
        std::string description;   // one line: what its tools do
    };

    constexpr const char* CORE_TOOL_GROUP = "core";

    // Every group a tool may be registered into, core first (McpToolGroups.cpp). With
    // JASPER_MCP_SELFTEST_GROUP set, the test-only group "selftest" is appended.
    const std::vector<ToolGroupInfo>& toolGroups();
    const ToolGroupInfo* findToolGroup(const std::string& name);

    // The test-only group (JASPER_MCP_SELFTEST_GROUP=1, or =<n> to pad its tool's
    // description to n characters for the budget gate's red control). 0 when unset.
    int selftestGroupMode();

    // "a,b" or "all" (the --tool-groups / mcp.tool_groups syntax) as group names: "all" is
    // every non-core group; core is implied and dropped. False with error set on an unknown name.
    bool parseToolGroupList(const std::string& text, std::vector<std::string>& groups, std::string& error);

}
