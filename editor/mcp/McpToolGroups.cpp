// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpToolGroups.h"

#include <cstdlib>

namespace jasperengine::editor::mcp {

namespace {

std::vector<ToolGroupInfo> buildGroups() {
    // A spec adding tools registers them into its group (docs/mcp/conventions.md); a new group
    // is one row here plus, when it needs other limits than the defaults, one budget.json entry.
    // Order: tool_groups and the docs list them in this order.
    std::vector<ToolGroupInfo> groups = {
        {CORE_TOOL_GROUP, "Scenes, GameObjects, components, prefabs, play, views, scripts, assets, build: always listed."},
        {"assets_pipeline", "Asset GUIDs and dependencies, file watching, texture compression and import settings."},
        {"data_safety", "Scene load reports and autosave backups."},
        {"diagnostics", "Jobs, memory, ECS, logging and crash/hang diagnostics."},
        {"editor_ux", "Undo history, command palette, layouts, search, bookmarks and Scene view tools."},
        {"profiling", "GPU/CPU timings, VRAM, frame passes and render statistics."},
        {"rendering_fx", "Fog, clouds and post effects beyond the core set."},
        {"splines", "Splines and what is built on them: roads, walls, placement, terrain and water."},
        {"navigation", "Navigation meshes, agents, crowds and grid paths."},
        {"particles", "Particle processors, forces, trails, sub-emitters and render modes."},
        {"animation", "Blend spaces, state machines, layers, IK, retargeting and curves."},
        {"gameplay", "Character controller, buoyancy and input action maps."},
        {"editor_tools", "Colour picker, curve editor, project templates and movie recording."},
        {"vcs", "Version control: status, locks, commits and scene diffs."},
    };
    if (selftestGroupMode() > 0) {
        groups.push_back({"selftest", "Test-only group of the MCP tests (JASPER_MCP_SELFTEST_GROUP)."});
    }
    return groups;
}

} // namespace

int selftestGroupMode() {
    static const int mode = []() {
        const char* value = std::getenv("JASPER_MCP_SELFTEST_GROUP");
        if (!value || !*value) return 0;
        const int number = std::atoi(value);
        return number > 0 ? number : 0;
    }();
    return mode;
}

const std::vector<ToolGroupInfo>& toolGroups() {
    static const std::vector<ToolGroupInfo> groups = buildGroups();
    return groups;
}

const ToolGroupInfo* findToolGroup(const std::string& name) {
    const std::string key = name.empty() ? std::string(CORE_TOOL_GROUP) : name;
    for (const ToolGroupInfo& group : toolGroups()) {
        if (group.name == key) return &group;
    }
    return nullptr;
}

bool parseToolGroupList(const std::string& text, std::vector<std::string>& groups, std::string& error) {
    groups.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        std::string name = text.substr(start, end - start);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        start = end + 1;
        if (name.empty()) continue;
        if (name == "all") {
            for (const ToolGroupInfo& group : toolGroups()) {
                if (group.name != CORE_TOOL_GROUP) groups.push_back(group.name);
            }
            continue;
        }
        if (!findToolGroup(name)) {
            std::string valid = "all";
            for (const ToolGroupInfo& group : toolGroups()) valid += ", " + group.name;
            error = "unknown tool group \"" + name + "\" (groups: " + valid + ")";
            return false;
        }
        if (name != CORE_TOOL_GROUP) groups.push_back(name);
    }
    return true;
}

}
