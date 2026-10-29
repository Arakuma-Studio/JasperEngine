// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;
    class McpContext;

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
    void registerSceneViewTools(McpRegistry& registry);   // S26
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
    void registerVatTools(McpRegistry& registry);

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
    void registerDiagnosticsTools(McpRegistry& registry);
    void registerAtmosphereTools(McpRegistry& registry);
    void registerProfilerTools(McpRegistry& registry);
    void registerScriptingTools(McpRegistry& registry);
    void registerGraphTools(McpRegistry& registry);
    void registerGameplayTools(McpRegistry& registry);
    void registerSplineTools(McpRegistry& registry);
    void registerParticleTools(McpRegistry& registry);

    void registerPoseTools(McpRegistry& registry);

    // VatTools.cpp (vertex animation spec 6): the .vat parts of the asset tools.
    // get_asset_info of a .vat: data.vertex_animation = {guid, mode, mesh, layout, bounds, clips,
    // sockets, stats, recipe, warnings, staleness, stale, rebake?, users}; `text` gets a summary.
    // False (data.error, data.fix) when it doesn't parse.
    bool vatAssetInfo(Project* project, const std::filesystem::path& file, Json& data, std::string& text);
    // the Vertex Animators of the open scenes and prefab stages using a .vat: [{scene, entity}]
    Json vatUsers(Project* project, const std::string& vatRelative);
    // write_file of a .vat: false + error when it doesn't parse, VatDocument::normalize refuses it,
    // or it changes a generated field of the existing file (VatDocument::generatedChange: the
    // rebake message); `warnings` the lenient reader's
    bool checkVatText(Project* project, const std::filesystem::path& file, const std::string& content,
                      std::vector<std::string>& warnings, std::string& error);

    // the pose documents' files-written hook (their writes reach the asset database and the
    // Project window like any MCP file change): installed once, when the MCP context exists
    void installPoseSyncHook(McpContext& context);

    void registerOrganisationTools(McpRegistry& registry);
    void registerNavigationTools(McpRegistry& registry);
    void registerVcsTools(McpRegistry& registry);   // S29 (VcsTools.cpp)

    // list_assets search and saved searches (OrganisationTools.cpp): an R20 search over the asset
    // index, as {entries: [{path, type, guid?, labels?}], total, truncated, query}; *error for a bad re:
    Json searchAssetsJson(Project* project, const std::string& query, const std::string& folder, size_t limit, std::string& error);

}
