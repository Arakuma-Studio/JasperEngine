// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// `jasperengine-editor cook-textures` (spec S14 R29) and the texture part of `check`:
//
//   cook-textures --project P [--platform desktop|mobile|android_etc2|web|all] [--paths ...]
//                 [--report out.json] [--out assets-dir] [--jobs N] [--check]
//   cook-textures --decode in.ktx2 out.png [--level N] [--platform-target bc7|astc_6x6|...]
//
// The first cooks into the project's texture cache and prints the report (R27's fields);
// --check exits 1 when a cook fails or a warning-level problem (R2 conflicts, R6) exists. The
// second decodes a KTX2 level to PNG (a Basis file transcoded to the target first), to verify
// a file.

#pragma once

#include "texture/TextureImportSettings.h"
#include "util/ProjectChecks.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TextureCliOptions {
        std::filesystem::path project;
        std::vector<TextureFamily> families = {TextureFamily::Desktop};
        std::vector<std::string> paths;     // asset- or project-relative files or directories
        std::filesystem::path report;
        std::filesystem::path out;          // also write the export layout: <out>/je-cooked/<family>/
        int jobs = 0;
        bool check = false;
        std::filesystem::path decodeIn;
        std::filesystem::path decodeOut;
        int level = 0;
        std::string target;                 // a GpuTextureFormat name
        bool help = false;
    };

    class TextureCli {
    public:
        static bool parse(int argc, char** argv, TextureCliOptions& options, std::string& error);
        static void printUsage(const std::string& commandName);
        // --decode: no project needed
        static int decode(const TextureCliOptions& options);
        // the cook of a loaded project (its index ready)
        static int cook(Project& project, const TextureCliOptions& options);
        // `check`'s textures: the import problems of every texture source (or of `files`,
        // project-relative), planned for the families the project's exports load
        static std::vector<ProjectChecks::Finding> check(Project& project, const std::vector<std::string>& files, size_t& checked);
    };

}
