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
            // what the Console tells without a warning: the submeshes a reimport moved (spec S94 F42)
            std::vector<std::string> notes;
        };

        // The model import settings the conversion reads from the FBX's meta sidecar (spec S94 F40,
        // F43): Rest Pose and its Take, and whether a Smoothness Source wants smoothing groups; and
        // the project's FBX Submesh Order (S94 F42)
        struct Options {
            bool firstFrameRestPose = false;
            std::string restPoseTake;   // empty: the first take
            bool smoothingGroups = false;
            // Used Materials (projects made before S94): the submeshes are the used material slots
            // in order, so an empty slot shifts the ones after it; else submesh i is slot i
            bool usedMaterialsOrder = false;

            // stable text of what differs from the defaults, empty for the defaults
            std::string canonical() const;
        };
        // usedMaterialsOrder: the project's FBX Submesh Order is Used Materials (ModelProjectSettings)
        static Options readOptions(const std::filesystem::path& fbxPath, bool usedMaterialsOrder = false);

        static bool isFbxFile(const std::filesystem::path& path);

        static State getState(const std::filesystem::path& fbxPath, const std::filesystem::path& glbPath,
                              const std::filesystem::path& assetsRoot, bool usedMaterialsOrder = false);

        // What the model parts of an import come from: the FBX contents, the version of the parts and
        // the options that change them
        static std::string getSourceId(const std::filesystem::path& fbxPath, bool usedMaterialsOrder = false);
        // The source id a loaded model was imported from, empty when it is no import
        static std::string getImportId(const tinygltf::Model& model);
        // Whether a loaded model is a parse of the .glb as it is now
        static bool isCurrentImport(const tinygltf::Model& model, const std::filesystem::path& glbPath);

        // Textures the .glb links that no longer exist
        static std::vector<std::filesystem::path> findMissingLinkedFiles(const std::filesystem::path& fbxPath,
                                                                         const std::filesystem::path& glbPath);

        // Textures inside assetsRoot are linked, others embedded. An up-to-date .glb is kept unless forced.
        static Result import(const std::filesystem::path& fbxPath, const std::filesystem::path& glbPath,
                             const std::filesystem::path& assetsRoot, bool force = false, bool usedMaterialsOrder = false);
    };

}
