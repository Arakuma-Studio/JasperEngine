// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;
    class McpContext;

    // Tool registrations grouped by editor feature.
    void registerCoreTools(McpRegistry& registry);
    void registerProjectTools(McpRegistry& registry);
    void registerSceneTools(McpRegistry& registry);
    void registerEntityTools(McpRegistry& registry);
    void registerComponentTools(McpRegistry& registry);
    void registerPrefabTools(McpRegistry& registry);
    void registerPlayTools(McpRegistry& registry);
    void registerViewTools(McpRegistry& registry);
    void registerConsoleTools(McpRegistry& registry);
    void registerScriptTools(McpRegistry& registry);
    void registerAssetTools(McpRegistry& registry);
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
    void registerPoseTools(McpRegistry& registry);

    // VatTools.cpp (vertex animation spec 6): the .vat parts of the asset tools.
    // get_asset_info of a .vat: data.vertex_animation = {guid, mode, mesh, layout, bounds, clips,
    // sockets, stats, recipe, warnings, staleness, stale, rebake?, users}; `text` gets a summary.
    // False (data.error, data.fix) when it doesn't parse.
    bool vatAssetInfo(Project* project, const std::filesystem::path& file, Json& data, std::string& text);
    // the Vertex Animators of the open scenes and prefab stages using a .vat: [{scene, entity}]
    Json vatUsers(Project* project, const std::string& vatRelative);
    // write_file of a .vat: false + error when it doesn't parse or changes a generated field of the
    // existing file (the rebake message); `warnings` the lenient reader's
    bool checkVatText(Project* project, const std::filesystem::path& file, const std::string& content,
                      std::vector<std::string>& warnings, std::string& error);
    // a changed .vat / .vat.glb (file sync, auto-sync): VertexAnimationPool drops it (its
    // animators reload); a .vat's load problems go to `problems` ({path, message, severity?})
    void vatFileChanged(Project* project, const std::filesystem::path& path, Json& problems);
    // the other half of a Vertex Animation: "<x>.vat" <-> "<x>.vat.glb" (empty for other files)
    std::filesystem::path vatPartner(const std::filesystem::path& path);
    // move_asset of one half: the other half's move (pairFrom -> pairTo, empty when it has none)
    // and, when the name changes or for a copy, the .vat's new text (its `mesh`, a copy's new GUID)
    // to write at vatTo. False + error for a destination that breaks the pair.
    bool vatMovePair(Project* project, const std::filesystem::path& from, const std::filesystem::path& to, bool copy,
                     std::filesystem::path& vatFrom, std::filesystem::path& vatTo, std::filesystem::path& pairFrom,
                     std::filesystem::path& pairTo, std::string& vatText, std::string& error);
    // the editor learns written .vat files (the pool drops them, the documents and database see them)
    void vatFilesWritten(Project* project, const std::vector<std::filesystem::path>& paths);

    // the pose documents' files-written hook (their writes reach the asset database and the
    // Project window like any MCP file change): installed once, when the MCP context exists
    void installPoseSyncHook(McpContext& context);

}
