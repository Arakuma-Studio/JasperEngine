// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioImport.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // What the engine plays of a project's import settings: the clips whose .meta holds
    // import.audioImporter, resolved to frames against their files. Play registers them in the
    // engine's AudioImportTable; an export writes them as je-audio-imports.json.
    class AudioImportRegistry {
    public:
        // An asset's settings resolved against its file (headers alone unless a trim is `auto`,
        // which reads the silence through the analysis cache). True with defaults when it has none.
        static bool resolve(const Project& project, const std::filesystem::path& asset, ClipImportSettings& out, std::string& error);
        // Every clip with settings, by its asset-relative path, in path order. A section that does
        // not read or does not fit its file is listed in problems ("<path>: <reason>") and left out.
        static std::map<std::string, ClipImportSettings> collect(const Project& project, std::vector<std::string>& problems);

        // At Play start: the table becomes the project's, its problems go to the Console
        static void registerProject(const Project& project);
        // Files or metas changed: their clips' entries follow. Playing voices keep their sample.
        static void filesChanged(const Project& project, const std::vector<std::filesystem::path>& changed);

        // The text of je-audio-imports.json (jasperengine.audio-imports v1); "" for no clips
        static std::string exportText(const std::map<std::string, ClipImportSettings>& clips);
    };

}
