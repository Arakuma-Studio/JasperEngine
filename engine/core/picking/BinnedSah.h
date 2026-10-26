// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Copied from OpenEngine's binned-SAH split kernel (Engine/Modules/SceneBvh/Include/SceneBvh/
// BinnedSahSplit.h, Source/BinnedSahSplit.cpp), Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License (NOTICE.md). Our math types; one centroid array per range
// instead of an id-indexed span of floats.

#ifndef BINNEDSAH_H
#define BINNEDSAH_H

#include "Export.h"

#include <cstdint>
#include <span>

namespace jasperengine::picking {

    // The one binned-SAH split kernel of the CPU BVH builders: MeshBvh (triangles) and
    // SceneBvh (entity bounds) both partition with it. It decides the best (axis, bin) split
    // of a range; what a builder does with the decision (leaf policy, node layout, partition)
    // stays its own.
    //
    // 16 bins per axis: split quality flattens out past ~16 while binning keeps costing more.
    inline constexpr uint32_t BINNED_SAH_BINS = 16u;

    // Bin of a centroid coordinate, clamped into [0, BINNED_SAH_BINS). `scale` is
    // BINNED_SAH_BINS / centroid extent on that axis. Exposed so a builder's partition
    // predicate assigns bins exactly as the scorer did.
    inline uint32_t binnedSahBinOf(float centroid, float axisMin, float scale){
        const int32_t bin = static_cast<int32_t>((centroid - axisMin) * scale);
        return static_cast<uint32_t>(bin < 0 ? 0 : (bin >= static_cast<int32_t>(BINNED_SAH_BINS)
                                                     ? static_cast<int32_t>(BINNED_SAH_BINS) - 1 : bin));
    }

    // One range of primitives, described by the arrays the builder keeps. Centroids are the
    // builder's own definition (MeshBvh: the vertex mean, SceneBvh: the box centre); the
    // kernel only needs centroidMin/Max to bound them.
    struct BinnedSahRange {
        // primitive ids of the range, in the builder's current partition order
        std::span<const uint32_t> order;
        // 3 floats per primitive id (indexed by id, not by range position)
        std::span<const float> centroids;
        // 6 floats per primitive id: min xyz, max xyz
        std::span<const float> bounds;
        float centroidMin[3] = {0.0f, 0.0f, 0.0f};
        float centroidMax[3] = {0.0f, 0.0f, 0.0f};
    };

    struct BinnedSahCandidate {
        // false when no axis has centroid extent or no split leaves both sides non-empty;
        // builders fall back to their own degenerate policy
        bool found = false;
        uint32_t axis = 3u;
        uint32_t bin = 0u;      // primitives with bin <= bin on axis go left
        float axisMin = 0.0f;
        float scale = 0.0f;     // feed both back into binnedSahBinOf to partition
        // Al*Nl + Ar*Nr with full surface areas: the SAH numerator, the builder applies its
        // own cost model around it
        float cost = 0.0f;
    };

    // Scores every (axis, bin) candidate of the range and returns the cheapest
    JASPERENGINE_API BinnedSahCandidate findBestBinnedSahSplit(const BinnedSahRange& range);

    // 2 (dx dy + dy dz + dz dx) of a box, 0 for an empty one (the units of BinnedSahCandidate::cost)
    inline float boxSurfaceArea(const float mn[3], const float mx[3]){
        const float dx = mx[0] > mn[0] ? mx[0] - mn[0] : 0.0f;
        const float dy = mx[1] > mn[1] ? mx[1] - mn[1] : 0.0f;
        const float dz = mx[2] > mn[2] ? mx[2] - mn[2] : 0.0f;
        return 2.0f * (dx * dy + dy * dz + dz * dx);
    }

}

#endif // BINNEDSAH_H
