// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The export's model step (spec S53 R11, R12, R13): every model source cooked for each family
// the export targets (cache hits make it fast), written to assets/je-cooked/models/<hash>.glb
// (identical cooks share a file) with an index.json mapping each asset-relative source path to
// its file per family and whether the player keeps its CPU copy (Read/Write). A cook error fails
// the export, naming the file: nothing ships uncooked behind the user's back. The sources the
// cooks replace (and the external buffers only they read) are left out of the copied assets
// unless Ship Source File is on.

#pragma once

#include "model/ModelCookQueue.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct ModelExportRow {
        std::string assetPath;
        std::map<ModelFamily, ModelCookResult> families;
        std::map<ModelFamily, ModelReadWriteDecision> readWrite;
        // each family's file in je-cooked/models: <SHA-256 of its bytes>.glb, so families whose
        // cooks are the same bytes ship one file (the cache key also names the family)
        std::map<ModelFamily, std::string> files;
    };

    struct ModelExportResult {
        bool ok = false;
        std::string error;
        // asset-relative files the cooks replace (copyAssets skips them)
        std::set<std::string> cookedSources;
        std::vector<ModelExportRow> rows;
        int cacheHits = 0;
        int cooked = 0;
    };

    // What the export reads on the main thread: each model's requests and what uses it
    struct ModelExportInputs {
        std::vector<ModelBatchItem> items;
        std::map<std::string, ModelUsage> usage;
        std::map<std::string, bool> skinned;
        std::map<std::string, std::vector<std::string>> extraFiles;   // a .gltf's .bin files, asset-relative
        ScriptMeshData scripts = ScriptMeshData::Automatic;
        std::vector<std::string> scriptReaders;
    };

    class ModelExport {
    public:
        // The requests of every model source x family (main thread)
        static bool gather(Project& project, const std::vector<ModelFamily>& families, ModelExportInputs& inputs, std::string& error);

        // Cooks the inputs (any thread) into <assetsOut>/je-cooked/models; an empty assetsOut
        // cooks into the cache only. `progress` gets (done, total, path).
        static ModelExportResult cookForExport(Project& project, ModelExportInputs& inputs, const std::filesystem::path& assetsOut, int threads,
                                               const std::function<void(int, int, const std::string&)>& progress,
                                               const std::function<bool()>& cancelled);

        // the one-line-per-model report of the export log
        static std::string toText(const ModelExportResult& result);
    };

}
