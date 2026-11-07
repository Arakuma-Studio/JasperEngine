// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Model Import inspector's preview (spec S53 R16): a model's triangles drawn on the CPU (flat
// shading, optional wireframe), from any angle, at any of its baked levels, for the source or the
// cooked file. Deterministic and GPU-free, so it reads the same in a headless editor.

#pragma once

#include "model/GltfDocument.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ModelPreviewGeometry {
        std::vector<float> positions;                       // xyz, in the scene's space (node transforms applied)
        // the triangles of each level: [0] the model as it is, [l] its JE_mesh_lods level l (a
        // primitive with fewer levels draws its coarsest one)
        std::array<std::vector<uint32_t>, 4> levels;
        int levelCount = 1;
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius = 1.0f;
    };

    // the triangle primitives of the document's default scene (every root node without one)
    bool loadPreviewGeometry(const GltfDocument& doc, ModelPreviewGeometry& out, std::string& error);

    // RGBA8, width x height; yaw and pitch in radians around the model's centre
    std::vector<uint8_t> rasterizePreview(const ModelPreviewGeometry& geometry, int width, int height, float yaw, float pitch,
                                          bool wireframe, int level);

}
