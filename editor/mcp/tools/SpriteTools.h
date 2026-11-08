// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .aseprite parts of the core asset tools (S44 R36), from the sprites module
#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    // get_asset_info of an .aseprite file: the describe() summary (no cels), the stored and
    // resolved import settings, the outputs with their GUIDs and references, warnings, the
    // cache state; `text` gets the summary line
    Json asepriteAssetInfo(Project* project, const std::filesystem::path& file, std::string& text);
    // view_asset of an .aseprite file: its imported frames as a contact sheet
    bool replyAsepriteView(const CallPtr& call, Project* project, const std::filesystem::path& file, int maxSize);
    // get_asset_info of a sheet source (S45): a texture's Sprite Mode and frames, packed_in of a
    // texture or .aseprite file an atlas packs, an atlas's build; `kind` is get_asset_info's
    Json spriteSheetAssetInfo(Project* project, const std::filesystem::path& file, const std::string& kind, std::string& text);

}
