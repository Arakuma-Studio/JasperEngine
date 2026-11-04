// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sound helpers shared by the tool modules (SoundTools.cpp, AssetTools.cpp). Main thread only.
#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    // get_asset_info's `audio` of a sound file (null with the reason when it cannot be decoded),
    // measured off the main thread, through the analysis cache: `done` runs on the main thread.
    void assetInfoAudio(const CallPtr& call, const std::filesystem::path& file, const std::string& rel,
                        std::function<void(const Json& audio, const std::string& error)> done);
    // import_files of built-in references (essentials:<id>, library:<id>): the plan, written as one
    // undo step; completes the call
    bool importBuiltinSounds(const CallPtr& call, const std::vector<std::string>& references, const std::filesystem::path& directory, bool overwrite);

}
