// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VisibleList.h"

#include "component/CameraComponent.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>

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
            const AABB& local = *reinterpret_cast<const AABB*>(reinterpret_cast<const uint8_t*>(candidate.submeshBounds) + s * candidate.submeshStride);
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

uint64_t VisibleList::opaqueKey(int priority, bool fading, uint32_t pipeline, uint32_t material, uint32_t mesh,
                                float distance, float farClip){
    // 8 bits of priority (-128..127), 1 fading, 15 pipeline, 16 material, 12 mesh, 12 depth
    const uint64_t biased = (uint64_t)(std::clamp(priority, -128, 127) + 128);
    // depth on a log scale of the pass's range: near draws keep their order finely
    const float range = (farClip > 0.0f && std::isfinite(farClip)) ? farClip : 10000.0f;
    const float d = std::clamp(std::isfinite(distance) ? distance : range, 0.0f, range);
    const uint64_t depth = (uint64_t)std::min(4095.0f, 4095.0f * std::log2(1.0f + d) / std::log2(1.0f + range));
    return (biased << 56) | ((uint64_t)(fading ? 1 : 0) << 55) | ((uint64_t)(pipeline & 0x7FFFu) << 40) |
           ((uint64_t)(material & 0xFFFFu) << 24) | ((uint64_t)(mesh & 0xFFFu) << 12) | depth;
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
