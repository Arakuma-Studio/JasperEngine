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

    struct ModelContents {
        std::vector<ModelSubAsset> subAssets;
        std::vector<std::string> uris;   // external files, relative to the model's directory
    };

    // False when the file cannot be read or parsed (it then has no sub-assets)
    bool scanModelFile(const std::filesystem::path& file, ModelContents& contents);
    // The same from the file's bytes (extension picks the format: .gltf, .glb or .obj)
    bool scanModelBytes(const std::string& extension, const std::string& bytes, ModelContents& contents);

}
