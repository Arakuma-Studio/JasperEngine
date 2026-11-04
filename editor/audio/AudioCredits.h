// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // What an export's audio files are licensed under, from each file's provenance (spec S36 R22):
    // licenses/THIRD-PARTY-AUDIO.txt and its warnings. Pure functions of their inputs, so every
    // export method uses the same code.
    class AudioCredits {
    public:
        static constexpr const char* FILE_NAME = "THIRD-PARTY-AUDIO.txt";

        struct Entry {
            std::string path;           // assets-relative, '/' separated
            std::string licence;
            std::string licenceUrl;
            std::string author;
            std::string source;
            std::string url;
            std::string attribution;
        };

        struct Report {
            // files with a licence, sorted by licence, then path
            std::vector<Entry> listed;
            // licence: generated: placeholders, sorted
            std::vector<std::string> placeholders;
            // licence: unknown (or a provenance without a licence), sorted
            std::vector<std::string> unknown;

            bool empty() const { return listed.empty() && placeholders.empty() && unknown.empty(); }
        };

        // The audio files under assetsDir that the export ships. A clip without a provenance (the
        // user's own) is not listed and warns nothing.
        static Report collect(const std::filesystem::path& assetsDir, const std::vector<std::filesystem::path>& shippedRelativePaths);
        // The text of the file: a section per licence, a line per clip; Kenney's packs grouped by pack
        static std::string render(const Report& report, const std::string& projectName);
        // W-AUDIO-LICENCE and W-AUDIO-PLACEHOLDER: "" when there is nothing to say
        static std::string licenceWarning(const Report& report);
        static std::string placeholderWarning(const Report& report);
    };

}
