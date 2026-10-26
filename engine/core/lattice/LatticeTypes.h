// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_TYPES_H
#define LATTICE_TYPES_H

#include "Engine.h"
#include "math/Vector4.h"
#include "render/TextureRender.h"
#include <vector>

#define LATTICE_MIN_RESOLUTION 2
#define LATTICE_MAX_RESOLUTION 16
#define LATTICE_MAX_PER_MESH 4
#define LATTICE_TEXTURE_WIDTH 64
#define LATTICE_RECORD_HEADER 12

namespace jasperengine{

    // GPU deformation data of one mesh (spec §5.1). LatticeRender::prepare fills it,
    // RenderSystem uploads `texels` into `texture` and binds it.
    struct JASPERENGINE_API LatticeMeshState{
        int count = 0;                              // active bindings (0 = no lattice variant)
        std::vector<Vector4> texels;                // packed records, width LATTICE_TEXTURE_WIDTH
        float recordStart[LATTICE_MAX_PER_MESH] = {}; // first texel of each record
        int submesh[LATTICE_MAX_PER_MESH] = {-1, -1, -1, -1}; // per binding, -1 = all submeshes
        uint32_t movingBindings = 0;                // bit b: binding b moves vertices (not at rest, weight not 0)
        float worldPadding = 0.0f;                  // world-space AABB growth (spec §5.5)
        bool needUpload = false;
        bool variantActive = false;                 // count > 0 when the loaded shaders were built
        bool wantsTessellation = false;             // sprite: generate a grid (spec §6.4)
        int tessellation = 1;                       // grid size requested for sprites
        TextureRender texture;                      // RGBA32F, created on demand
    };

}

#endif //LATTICE_TYPES_H
