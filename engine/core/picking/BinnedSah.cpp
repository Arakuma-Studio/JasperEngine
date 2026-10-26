// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Copied from OpenEngine's binned-SAH split kernel (Engine/Modules/SceneBvh/Source/
// BinnedSahSplit.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License (NOTICE.md).

#include "BinnedSah.h"

#include <array>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::picking;

namespace {

    struct BinBox {
        float mn[3] = { std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max()};
        float mx[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

        void expand(const float* box){
            for (int a = 0; a < 3; a++){
                mn[a] = box[a] < mn[a] ? box[a] : mn[a];
                mx[a] = box[3 + a] > mx[a] ? box[3 + a] : mx[a];
            }
        }

        void expand(const BinBox& other){
            for (int a = 0; a < 3; a++){
                mn[a] = other.mn[a] < mn[a] ? other.mn[a] : mn[a];
                mx[a] = other.mx[a] > mx[a] ? other.mx[a] : mx[a];
            }
        }

        float area() const{
            return boxSurfaceArea(mn, mx);
        }
    };

}

BinnedSahCandidate picking::findBestBinnedSahSplit(const BinnedSahRange& range){
    BinnedSahCandidate best;
    best.cost = std::numeric_limits<float>::max();

    for (uint32_t axis = 0; axis < 3u; axis++){
        const float axisMin = range.centroidMin[axis];
        const float extent = range.centroidMax[axis] - axisMin;
        if (!(extent > 0.0f)){
            continue;
        }

        const float scale = static_cast<float>(BINNED_SAH_BINS) / extent;

        std::array<BinBox, BINNED_SAH_BINS> binBounds;
        std::array<uint32_t, BINNED_SAH_BINS> binCounts{};

        for (const uint32_t id : range.order){
            const uint32_t bin = binnedSahBinOf(range.centroids[(size_t)id * 3u + axis], axisMin, scale);
            binCounts[bin]++;
            binBounds[bin].expand(&range.bounds[(size_t)id * 6u]);
        }

        // suffix sweep first: rightArea[b] / rightCount[b] describe bins (b, BINS)
        std::array<float, BINNED_SAH_BINS - 1u> rightArea{};
        std::array<uint32_t, BINNED_SAH_BINS - 1u> rightCount{};
        BinBox accumulated;
        uint32_t accumulatedCount = 0;
        for (uint32_t bin = BINNED_SAH_BINS - 1u; bin >= 1u; bin--){
            if (binCounts[bin] > 0){
                accumulated.expand(binBounds[bin]);
            }
            accumulatedCount += binCounts[bin];
            rightArea[bin - 1u] = accumulatedCount > 0 ? accumulated.area() : 0.0f;
            rightCount[bin - 1u] = accumulatedCount;
        }

        // prefix sweep: a split after bin b puts bins [0, b] left, (b, BINS) right
        accumulated = BinBox();
        accumulatedCount = 0;
        for (uint32_t bin = 0; bin + 1u < BINNED_SAH_BINS; bin++){
            if (binCounts[bin] > 0){
                accumulated.expand(binBounds[bin]);
            }
            accumulatedCount += binCounts[bin];
            if (accumulatedCount == 0 || rightCount[bin] == 0){
                continue;
            }

            const float cost = accumulated.area() * static_cast<float>(accumulatedCount) +
                               rightArea[bin] * static_cast<float>(rightCount[bin]);
            if (cost < best.cost){
                best.found = true;
                best.axis = axis;
                best.bin = bin;
                best.axisMin = axisMin;
                best.scale = scale;
                best.cost = cost;
            }
        }
    }

    return best;
}
