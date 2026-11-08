// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VisibleList.h"

#include "component/CameraComponent.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;

bool VisibleList::isInside(const VisibleFrustum& frustum, const AABB& box){
    if (box.isNull() || box.isInfinite())
        return false;

    const Vector3 centre = box.getCenter();
    const Vector3 halfSize = box.getHalfSize();
    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && frustum.farClip == 0)
            continue;
        if (frustum.planes[plane].getSide(centre, halfSize) == Plane::Side::NEGATIVE_SIDE)
            return false;
    }
    return true;
}

VisibleResult VisibleList::cull(const VisibleFrustum& frustum, const VisibleCandidate& candidate){
    VisibleResult result;
    if (candidate.cullBounds){
        if (!isInside(frustum, candidate.worldBounds))
            return result;
        result.distance2 = candidate.worldBounds.squaredDistance(frustum.eye);
        if (std::isfinite(candidate.maxDistance) && result.distance2 > candidate.maxDistance * candidate.maxDistance)
            return result;
    }else if (!candidate.worldBounds.isNull() && !candidate.worldBounds.isInfinite()){
        result.distance2 = candidate.worldBounds.squaredDistance(frustum.eye);
    }

    uint32_t mask = candidate.submeshMask;
    if (candidate.numSubmeshes < 32)
        mask &= (candidate.numSubmeshes == 0) ? 0u : ((1u << candidate.numSubmeshes) - 1u);
    // the submeshes one by one, when the mesh has more than one with bounds of their own
    if (candidate.cullBounds && candidate.submeshBounds && candidate.model && candidate.numSubmeshes > 1){
        for (uint32_t s = 0; s < candidate.numSubmeshes && s < 32; s++){
            if (!(mask & (1u << s)))
                continue;
            const AABB& local = *candidate.submeshBounds[s];
            if (local.isNull() || local.isInfinite())
                continue;
            AABB world = local;
            world.transform(*candidate.model);
            if (!isInside(frustum, world))
                mask &= ~(1u << s);
        }
    }
    result.submeshMask = mask;
    return result;
}

void VisibleList::cull(const VisibleFrustum& frustum, const VisibleCandidate* candidates, size_t count,
                       VisibleResult* results, bool parallel){
    // a body of ~0.2-1 µs: claimed 64 at a time
    if (parallel && count >= 128){
        jobs::ParallelForOptions options;
        options.grain = 64;
        jobs::parallelFor(count, [&](size_t i){
            results[i] = cull(frustum, candidates[i]);
        }, options);
        return;
    }
    for (size_t i = 0; i < count; i++)
        results[i] = cull(frustum, candidates[i]);
}

void VisibleList::groupBatches(const int* priorities, const uint64_t* keys, size_t count, std::vector<uint32_t>& order){
    order.resize(count);
    // each draw's group: the position of the first draw of its key in its priority run, then its own
    std::vector<std::pair<uint32_t, uint32_t>> groups(count);
    std::unordered_map<uint64_t, uint32_t> firstOf;
    for (size_t i = 0; i < count; i++){
        if (i > 0 && priorities[i] != priorities[i - 1])
            firstOf.clear();
        uint32_t first = (uint32_t)i;
        if (keys[i] != 0)
            first = firstOf.emplace(keys[i], (uint32_t)i).first->second;
        groups[i] = {first, (uint32_t)i};
        order[i] = (uint32_t)i;
    }
    // (unique pairs: deterministic)
    std::sort(order.begin(), order.end(), [&groups](uint32_t a, uint32_t b){
        return groups[a] < groups[b];
    });
}

uint32_t VisibleList::mixKey(uint32_t key, uint64_t value){
    // FNV-1a over the value's bytes, folded
    uint64_t h = 1469598103934665603ull ^ key;
    for (int b = 0; b < 8; b++){
        h ^= (value >> (b * 8)) & 0xFFu;
        h *= 1099511628211ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}
