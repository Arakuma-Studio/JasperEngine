// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <cstddef>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;

    // Every tool, resource and prompt of the editor, in tools/list order (McpTools.cpp).
    // Side-effect free: needs no App and does no I/O, so the CLI builds the same registry
    // offline (`jasperengine-editor call --help`, list-tools) as the server does at start.
    void buildToolRegistry(McpRegistry& registry);

    // Tool registrations grouped by editor feature. A new one is also added to the module
    // table of buildToolRegistry (McpTools.cpp), which names its docs section and group.
    void registerCoreTools(McpRegistry& registry);
    void registerProjectTools(McpRegistry& registry);
    void registerSceneTools(McpRegistry& registry);
    void registerEntityTools(McpRegistry& registry);
    void registerComponentTools(McpRegistry& registry);
    void registerPrefabTools(McpRegistry& registry);
    void registerPlayTools(McpRegistry& registry);
    void registerViewTools(McpRegistry& registry);
    void registerShadowTools(McpRegistry& registry);
    void registerConsoleTools(McpRegistry& registry);
    void registerScriptTools(McpRegistry& registry);
    void registerAssetTools(McpRegistry& registry);
    void registerAssetIdentityTools(McpRegistry& registry);
    void registerDocsTools(McpRegistry& registry);
    void registerBuildTools(McpRegistry& registry);
    void registerWorldTools(McpRegistry& registry);
    void registerHtmlUiTools(McpRegistry& registry);
    void registerUiEffectTools(McpRegistry& registry);
    void registerEcsTools(McpRegistry& registry);
    void registerAtmosphereTools(McpRegistry& registry);
    void registerProfilerTools(McpRegistry& registry);
    void registerScriptingTools(McpRegistry& registry);
    void registerGraphTools(McpRegistry& registry);
    void registerOrganisationTools(McpRegistry& registry);

    // list_assets search and saved searches (OrganisationTools.cpp): an R20 search over the asset
    // index, as {entries: [{path, type, guid?, labels?}], total, truncated, query}; *error for a bad re:
    Json searchAssetsJson(Project* project, const std::string& query, const std::string& folder, size_t limit, std::string& error);

}
