// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The export's texture step (spec S14 R9): every texture source under the assets tree cooked
// for each family the export targets (cache hits make it fast), written to
// assets/je-cooked/<family>/<k>.ktx2 (identical cooks share a file) with an index.json mapping
// each asset-relative source path to its file, logical size, format and flags. A cook error
// fails the export, naming the file: nothing is silently shipped as RGBA8. The sources the
// cooks replace are left out of the copied assets unless `shipSource` is set or a reference
// reads the source file itself (HTML UI images, terrain layers, a glTF's external images).
// This is also the API the export pipeline of S30 calls (cookForExport).

#pragma once

#include "texture/TextureCookReport.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TextureExportResult {
        bool ok = false;
        std::string error;
        // asset-relative sources the cooks replace (copyAssets skips them)
        std::set<std::string> cookedSources;
        std::vector<TextureReportRow> rows;
        int cacheHits = 0;
        int cooked = 0;
    };

    class TextureExport {
    public:
        // The settings and references of every texture source (main thread)
        static bool gather(Project& project, std::map<std::string, TextureCookInputs>& inputs, std::string& error);

        // Cooks `inputs` for `families` (any thread) into <assetsOut>/je-cooked; an empty
        // assetsOut cooks into the cache only (`cook-textures`). `progress` gets (done, total, path).
        static TextureExportResult cookForExport(Project& project, const std::map<std::string, TextureCookInputs>& inputs,
                                                 const std::vector<TextureFamily>& families, const std::filesystem::path& assetsOut,
                                                 int threads, const std::function<void(int, int, const std::string&)>& progress,
                                                 const std::function<bool()>& cancelled);

        // Writes <assetsOut>/je-cooked/<family>/ for the cooked items (their artifacts and index.json)
        static bool writeFamily(TextureCookCache& cache, TextureFamily family, const std::vector<TextureBatchItem>& items,
                                const std::filesystem::path& assetsOut, std::string& error);
    };

}
