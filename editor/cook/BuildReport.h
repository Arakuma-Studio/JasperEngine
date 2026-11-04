// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What an export ships and how big it is (spec S53 R21): totals by category (models, textures,
// audio, scripts, scenes, engine, other) and the largest files with their cooked and source
// sizes. Measured on the files themselves before they are packed; written as build-report.json
// next to the export, shown on the export's completion page and in Window > Analysis > Build
// Report.

#pragma once

#include "mcp/McpJson.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    using mcp::Json;

    struct BuildReportFile {
        std::string path;           // in the build ("assets/je-cooked/models/3f...glb")
        std::string category;
        uint64_t bytes = 0;
        std::string source;         // the asset it was cooked from (empty: shipped as it is)
        uint64_t sourceBytes = 0;
    };

    class BuildReport {
    public:
        static constexpr const char* kFileName = "build-report.json";
        static constexpr size_t kLargest = 25;

        std::string platform;           // "linux", "web"...
        std::string method;             // "prebuilt", "compile", "source"
        uint64_t totalBytes = 0;
        std::map<std::string, uint64_t> categories;
        std::vector<BuildReportFile> largest;

        // the categories in display order
        static const std::vector<std::string>& categoryNames();
        // "assets/je-cooked/models/a.glb" -> "models", "lua/main.lua" -> "scripts"...
        static std::string categoryOf(const std::string& path);

        // Adds every file below `root` (paths reported relative to it). `cooked` maps a build
        // path to its source asset and that source's bytes (the model and texture cooks'
        // indexes); files whose category is in `only` when it is not empty.
        void addTree(const std::filesystem::path& root, const std::map<std::string, std::pair<std::string, uint64_t>>& cooked,
                     const std::vector<std::string>& only = {}, const std::string& prefix = {});
        void addFile(const BuildReportFile& file);

        // the cooked textures of a build's assets folder: build path -> (source, its bytes on disk)
        static std::map<std::string, std::pair<std::string, uint64_t>> cookedTextures(const std::filesystem::path& assetsDir,
                                                                                       const std::filesystem::path& sourceAssets);

        Json toJson() const;
        static bool fromJson(const Json& json, BuildReport& out);
        bool write(const std::filesystem::path& file, std::string& error) const;
        static bool load(const std::filesystem::path& file, BuildReport& out, std::string& error);
        // "Build Report (Web): 142 MB; textures 61 MB, models 38 MB..."
        std::string summary() const;
        static std::string formatBytes(uint64_t bytes);

        // the report the last export of this editor wrote (the Build Report window opens it)
        static void setLast(const std::filesystem::path& file);
        static std::filesystem::path last();
    };

}
