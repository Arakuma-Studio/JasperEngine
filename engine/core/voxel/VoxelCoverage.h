// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_COVERAGE_H
#define VOXEL_COVERAGE_H

// R41: the flush-and-far-from-a-cut grid of an attached volume, at voxel-column resolution, which
// the mesher uses to drop triangles the terrain already covers.

#include "Export.h"
#include "VoxelTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::voxel {

    struct Coverage {
        int sizeX = 0, sizeZ = 0;              // columns
        std::vector<float> seedHeight;         // voxels; the seed surface of each column
        std::vector<uint8_t> flushAndFar;      // 1: flush, and more than ringWidth from any cut column
        float heightToleranceVoxels = 0.75f;   // R41: "within 0.75 voxel of the seed surface"

        bool columnCulled(int x, int z) const {
            if (x < 0 || z < 0 || x >= sizeX || z >= sizeZ) return false;
            return flushAndFar[static_cast<size_t>(x) + static_cast<size_t>(sizeX) * static_cast<size_t>(z)] != 0;
        }
        float seedAt(int x, int z) const {
            return seedHeight[static_cast<size_t>(x) + static_cast<size_t>(sizeX) * static_cast<size_t>(z)];
        }
        // A vertex position in voxels; within the columns' flush region and near the seed
        bool vertexCulled(double x, double y, double z) const {
            // A vertex on the volume's far side belongs to the last column
            const int cx = static_cast<int>(x) < sizeX ? static_cast<int>(x) : sizeX - 1;
            const int cz = static_cast<int>(z) < sizeZ ? static_cast<int>(z) : sizeZ - 1;
            if (!columnCulled(cx, cz)) return false;
            const double difference = y - static_cast<double>(seedAt(cx, cz));
            return difference <= heightToleranceVoxels && difference >= -static_cast<double>(heightToleranceVoxels);
        }
    };

    // Builds flushAndFar from a flush grid: cut columns are those with flush 0; a column more than
    // ringWidth from any cut is culled. Chebyshev distance, by two sweeps, so the ring never comes
    // up short on a diagonal.
    JASPERENGINE_API void buildCoverageMask(Coverage& coverage, const std::vector<uint8_t>& flush, int ringWidthColumns);

}

#endif
