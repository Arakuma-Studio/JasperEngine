// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelCoverage.h"

#include <algorithm>

using namespace jasperengine::voxel;

void jasperengine::voxel::buildCoverageMask(Coverage& coverage, const std::vector<uint8_t>& flush, int ringWidthColumns) {
    const int sizeX = coverage.sizeX, sizeZ = coverage.sizeZ;
    const size_t count = static_cast<size_t>(sizeX) * static_cast<size_t>(sizeZ);
    constexpr int FAR_AWAY = 1 << 20;
    std::vector<int> distance(count, FAR_AWAY);
    const auto at = [sizeX](int x, int z) { return static_cast<size_t>(x) + static_cast<size_t>(sizeX) * static_cast<size_t>(z); };
    for (int z = 0; z < sizeZ; z++) {
        for (int x = 0; x < sizeX; x++) {
            int best = flush[at(x, z)] ? FAR_AWAY : 0;
            if (x > 0) best = std::min(best, distance[at(x - 1, z)] + 1);
            if (z > 0) {
                best = std::min(best, distance[at(x, z - 1)] + 1);
                if (x > 0) best = std::min(best, distance[at(x - 1, z - 1)] + 1);
                if (x + 1 < sizeX) best = std::min(best, distance[at(x + 1, z - 1)] + 1);
            }
            distance[at(x, z)] = best;
        }
    }
    for (int z = sizeZ - 1; z >= 0; z--) {
        for (int x = sizeX - 1; x >= 0; x--) {
            int best = distance[at(x, z)];
            if (x + 1 < sizeX) best = std::min(best, distance[at(x + 1, z)] + 1);
            if (z + 1 < sizeZ) {
                best = std::min(best, distance[at(x, z + 1)] + 1);
                if (x + 1 < sizeX) best = std::min(best, distance[at(x + 1, z + 1)] + 1);
                if (x > 0) best = std::min(best, distance[at(x - 1, z + 1)] + 1);
            }
            distance[at(x, z)] = best;
        }
    }
    coverage.flushAndFar.resize(count);
    for (size_t i = 0; i < count; i++) {
        coverage.flushAndFar[i] = (flush[i] && distance[i] > ringWidthColumns) ? 1 : 0;
    }
}
