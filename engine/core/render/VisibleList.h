// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VisibleList_h
#define VisibleList_h

// A pass's visible list (spec S43 R4): what of the meshes it may draw is inside its frustum and
// within their distance, mesh by mesh and then submesh by submesh, culled once before the pass
// sorts and draws; and the 64-bit key its opaque queue sorts by (R2).

#include "Export.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <limits>

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
        const AABB* submeshBounds = nullptr;
        size_t submeshStride = sizeof(AABB);   // bytes from one submesh's bounds to the next
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

        // The opaque queue's sort key: sort priority first (lower first, so stencil writers stay
        // ahead of their readers), then draws that fade between detail levels (S42: their dither
        // pipeline) after the others, then pipeline, material and mesh (the state a batch
        // shares), then front to back. Equal keys keep their order (the caller's tie break).
        static uint64_t opaqueKey(int priority, bool fading, uint32_t pipeline, uint32_t material, uint32_t mesh,
                                  float distance, float farClip);
        // a material's state as a sort key: what its draws bind and apply (mixed, any order)
        static uint32_t mixKey(uint32_t key, uint64_t value);
    };

}

#endif /* VisibleList_h */
