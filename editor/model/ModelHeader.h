// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A model's numbers from its glTF JSON alone (spec S53 R20, R22): triangles, vertices and bytes
// from the accessors' counts and types, never a buffer. A whole project's models are read this
// way at the cost of their JSON chunks (the Audit window, inspect_models).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ModelHeader {
        long long triangles = 0;
        long long vertices = 0;
        uint64_t vertexBytes = 0;
        uint64_t indexBytes = 0;
        int meshes = 0;
        int primitives = 0;
        int materials = 0;
        int nodes = 0;
        int animations = 0;
        int morphTargets = 0;
        bool skinned = false;
        bool unindexed = false;             // a triangle primitive without indices
        bool wideIndices = false;           // 32-bit indices on a primitive 16 bits would reach
        bool blended = false;               // a material drawn BLEND
        std::vector<std::string> requiredExtensions;
        std::vector<std::string> usedExtensions;
        std::vector<std::string> buffers;   // external buffer files, relative to the model
        uint64_t fileBytes = 0;             // the file and its external buffers
    };

    // `file` is a .gltf or .glb; false with the reason when it is not readable glTF
    bool readModelHeader(const std::filesystem::path& file, ModelHeader& out, std::string& error);

}
