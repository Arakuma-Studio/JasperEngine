// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <cstddef>
#include <filesystem>
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
    void registerMaterialsLightingTools(McpRegistry& registry);
    void registerAssetIdentityTools(McpRegistry& registry);
    // S14: texture import settings and cooks (in AssetTools.cpp)
    void registerTextureTools(McpRegistry& registry);
    void registerDocsTools(McpRegistry& registry);
    void registerBuildTools(McpRegistry& registry);
    void registerWorldTools(McpRegistry& registry);
    void registerLatticeTools(McpRegistry& registry);
    void registerHtmlUiTools(McpRegistry& registry);
    void registerUiEffectTools(McpRegistry& registry);
    void registerSynthesisTools(McpRegistry& registry);
    void registerPlacementTools(McpRegistry& registry);

    // PlacementTools.cpp: puts back the preview poses a place_with_physics screenshot shows for a
    // few frames (App::stopTransientPreviews: before a save or Play)
    void stopPlacementPreviews(Project* project);

    // SynthesisTools.cpp: the jasper.synthesis recipe of a PNG for get_asset_info / view_asset:
    // {role, mode, quality, seed, size, sources: [{role, path, state}], sources_changed,
    // sources_missing} plus, with full, the raw recipe and `regenerate` (synthesize_texture
    // arguments reproducing it); null when the file has no recipe
    Json synthesisAssetInfo(Project* project, const std::filesystem::path& file, bool full);
    // " Synthesized (tileable, balanced, seed 7, role primary); sources unchanged." or ""
    std::string synthesisAssetText(const Json& info);
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
