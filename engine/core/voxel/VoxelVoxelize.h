// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_VOXELIZE_H
#define VOXEL_VOXELIZE_H

// R36: a triangle mesh as a signed distance grid, a stamp or a base. Distances come from the nearest
// triangle within the band; the sign from the majority of ray parity along the three axes, which is what
// gives an answer on a mesh with holes in it, and says so through nonWatertight.

#include "Export.h"
#include "VoxelEdit.h"

#include <cstdint>
#include <memory>
#include <span>

namespace jasperengine::voxel {

    struct VoxelizeOptions {
        double voxelSize = 1.0;         // metres per sample of the field
        double bandVoxels = 8.0;        // how far from the surface the field keeps true distances
        const double* transform = nullptr;   // 16 values, column major, or null for identity
    };

    // positions: x y z per vertex, indices: three per triangle
    JASPERENGINE_API std::shared_ptr<StampField> voxelizeMesh(std::span<const float> positions, std::span<const uint32_t> indices, const VoxelizeOptions& options);

}

#endif
