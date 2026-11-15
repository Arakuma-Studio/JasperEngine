// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tinygltf {
    class Model;
}

namespace jasperengine::editor {

    // Converts an FBX into a .glb, which the engine loads in its place
    class FbxImporter {
    public:
        enum class State {
            UpToDate,
            Missing,
            Stale // the FBX or one of its textures changed
        };

        struct Result {
            bool success = false;
            std::string error;
            std::vector<std::string> warnings;
        };

        static bool isFbxFile(const std::filesystem::path& path);

        static State getState(const std::filesystem::path& fbxPath, const std::filesystem::path& glbPath,
                              const std::filesystem::path& assetsRoot);

        // What the model parts of an import come from: the FBX contents and the importer version
        static std::string getSourceId(const std::filesystem::path& fbxPath);
        // The source id a loaded model was imported from, empty when it is no import
        static std::string getImportId(const tinygltf::Model& model);
        // Whether a loaded model is a parse of the .glb as it is now
        static bool isCurrentImport(const tinygltf::Model& model, const std::filesystem::path& glbPath);

        // The warnings of the import that made the .glb (its import report), in the order they came
        static std::vector<std::string> getImportWarnings(const std::filesystem::path& glbPath);
        // One Console line for an import's warnings (S98 REQ-34): the missing textures counted
        // and named (two, then "+N"), the textures whose tiling/offset is dropped counted, the rest
        // counted; empty without warnings
        static std::string summarizeWarnings(const std::string& fileName, const std::vector<std::string>& warnings);

        // Textures the .glb links that no longer exist
        static std::vector<std::filesystem::path> findMissingLinkedFiles(const std::filesystem::path& fbxPath,
                                                                         const std::filesystem::path& glbPath);
        // The files the import links by URI (textures inside the assets folder): an export that
        // ships the import ships them beside it (S98 REQ-9)
        static std::vector<std::filesystem::path> findLinkedFiles(const std::filesystem::path& fbxPath,
            const std::filesystem::path& glbPath);

        // The Material Creation Mode of the FBX models whose sidecar sets none (Project Settings >
        // Mesh, S98 REQ-44): false is None. The open project sets it; a change makes their imports stale.
        static void setDefaultCreatesMaterials(bool materials);

        // Textures inside assetsRoot are linked, others embedded. An up-to-date .glb is kept unless forced.
        // The FBX's Material Creation Mode (its sidecar's import.modelImporter) decides whether
        // materials are made; a change of it makes the .glb stale.
        static Result import(const std::filesystem::path& fbxPath, const std::filesystem::path& glbPath,
                             const std::filesystem::path& assetsRoot, bool force = false);
    };

}
