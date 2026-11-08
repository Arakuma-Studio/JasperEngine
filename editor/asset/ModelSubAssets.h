// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sub-assets of a model file (S04 §6.4): its meshes, materials, animations, skins and
// embedded images, read from the file's structure only (the glTF JSON, the GLB JSON chunk,
// OBJ object / group / usemtl lines) without loading geometry. Also the external URIs a
// glTF references (buffers and images), the model's dependency edges.

#pragma once

#include "asset/SubAssetKeys.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ModelSubAsset {
        SubAssetKind kind;
        std::string name;
        size_t index = 0;      // among the sub-assets of its kind, in file order
        std::string key;       // SubAssetKeys: "<kind>:<name>" or "<kind>#<index>"
    };

    // What a player cannot hold of a model whatever its build (spec S65 R4): vertex inputs and GPU
    // objects, over which a model loads incomplete. One entry per mesh or file.
    struct ModelLimit {
        std::string what;      // "mesh 'Face'", "the file"
        std::string kind;      // "blend shapes", "vertex and index buffers"
        size_t count = 0;
        size_t limit = 0;
    };

    struct ModelContents {
        std::vector<ModelSubAsset> subAssets;
        std::vector<std::string> uris;   // external files, relative to the model's directory
        std::vector<ModelLimit> limits;
    };

    // "Model <model>: <what> has <count> <kind>, more than the maximum allowed (<limit>)", and what
    // that does to it
    std::string modelLimitMessage(const std::string& model, const ModelLimit& limit);
    // Each of the file's limits in the Console: an import shows them, not only the game that loads it.
    void reportModelLimits(const std::string& model, const ModelContents& contents);

    // False when the file cannot be read or parsed (it then has no sub-assets)
    bool scanModelFile(const std::filesystem::path& file, ModelContents& contents);
    // The same from the file's bytes (extension picks the format: .gltf, .glb or .obj)
    bool scanModelBytes(const std::string& extension, const std::string& bytes, ModelContents& contents);
    // A palette's sub-assets (S43 R4a): its ramps, "ramp:<name>". False when it cannot be read.
    bool scanPaletteFile(const std::filesystem::path& file, ModelContents& contents);

}
