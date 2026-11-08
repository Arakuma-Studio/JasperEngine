// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VisibleList_h
#define VisibleList_h

// A pass's visible list (spec S55 R4): what of the meshes it may draw is inside its frustum and
// within their distance, mesh by mesh and then submesh by submesh, culled once before the pass
// draws; and the grouping of equal batch keys its GPU Instancing batches form from (R2).

#include "Export.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace jasperengine {

    // the frustum of a pass (CameraComponent::frustumPlanes, a light camera's): farClip 0 has no
    // far plane, as RenderSystem::isInsideCamera
    struct VisibleFrustum {
        const Plane* planes = nullptr;  // 6, CameraComponent's order
        float farClip = 0.0f;
        Vector3 eye;                    // what distances are measured from
    };

    // One mesh the pass may draw. The submeshes it is asked for (submeshMask) are kept when its
    // bounds are inside the frustum and within maxDistance of the eye; with submeshBounds, each
    // submesh's own bounds (in the mesh's space, through model; a null one is the mesh's) too.
    struct VisibleCandidate {
        AABB worldBounds;               // the mesh's; tested unless cullBounds is false
        const Matrix4* model = nullptr;
        const AABB* const* submeshBounds = nullptr;   // one a submesh (the first 32 are read)
        uint32_t numSubmeshes = 0;
        uint32_t submeshMask = 0xFFFFFFFFu;
        bool cullBounds = true;         // false: drawn wherever the frustum is (no bounds to test)
        float maxDistance = std::numeric_limits<float>::infinity();
    };

    struct VisibleResult {
        uint32_t submeshMask = 0;       // the submeshes kept (0: culled)
        float distance2 = 0.0f;         // squared distance of the bounds from the eye (0 inside)
    };

    class JASPERENGINE_API VisibleList {
    public:
        // a box against the frustum, as RenderSystem::isInsideCamera: a null or infinite box is outside
        static bool isInside(const VisibleFrustum& frustum, const AABB& box);
        // one candidate's result
        static VisibleResult cull(const VisibleFrustum& frustum, const VisibleCandidate& candidate);
        // results[i] of candidates[i]; parallel runs them on the job system's workers where there
        // are threads (a single-threaded web build runs them inline): the same results either way
        static void cull(const VisibleFrustum& frustum, const VisibleCandidate* candidates, size_t count,
                         VisibleResult* results, bool parallel);

        // Batches form from consecutive draws (spec S55 R2: the queue's order is draw-perf's):
        // within each run of equal priority, the draws of one batch key are brought together at
        // the place of its first, keeping the queue's order otherwise. keys[i] 0 batches with
        // nothing (stays where it is). order is the permutation to draw in.
        static void groupBatches(const int* priorities, const uint64_t* keys, size_t count, std::vector<uint32_t>& order);
        // a draw's state as a batch key: what it binds and applies (mixed, any order)
        static uint32_t mixKey(uint32_t key, uint64_t value);
    };

}

#endif /* VisibleList_h */
