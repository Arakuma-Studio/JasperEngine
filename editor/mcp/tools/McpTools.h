// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;

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
    void registerHtmlUiTools(McpRegistry& registry);
    void registerUiEffectTools(McpRegistry& registry);
    void registerSynthesisTools(McpRegistry& registry);

    // SynthesisTools.cpp: the jasper.synthesis recipe of a PNG for get_asset_info / view_asset:
    // {role, mode, quality, seed, size, sources: [{role, path, state}], sources_changed,
    // sources_missing} plus, with full, the raw recipe and `regenerate` (synthesize_texture
    // arguments reproducing it); null when the file has no recipe
    Json synthesisAssetInfo(Project* project, const std::filesystem::path& file, bool full);
    // " Synthesized (tileable, balanced, seed 7, role primary); sources unchanged." or ""
    std::string synthesisAssetText(const Json& info);

}
