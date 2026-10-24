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

        // Textures the .glb links that no longer exist
        static std::vector<std::filesystem::path> findMissingLinkedFiles(const std::filesystem::path& fbxPath,
                                                                         const std::filesystem::path& glbPath);

        // Textures inside assetsRoot are linked, others embedded. An up-to-date .glb is kept unless forced.
        static Result import(const std::filesystem::path& fbxPath, const std::filesystem::path& glbPath,
                             const std::filesystem::path& assetsRoot, bool force = false);
    };

}
