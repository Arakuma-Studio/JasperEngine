// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A pass's visible list (spec S55 R4, AC4): culled on the job system's workers and inline the
// results are the same; a mesh's submeshes are culled one by one by their own bounds; distance
// and unbounded meshes as RenderSystem draws them.

#include "render/VisibleList.h"
#include "component/CameraComponent.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {
    // the box x, y, z in [-10, 10] as a camera's six planes (normals inwards: n.p + d >= 0 inside)
    struct BoxFrustum {
        Plane planes[6];
        VisibleFrustum frustum;
        BoxFrustum(){
            planes[FRUSTUM_PLANE_NEAR] = Plane(0, 0, -1, 10.0f);
            planes[FRUSTUM_PLANE_FAR] = Plane(0, 0, 1, 10.0f);
            planes[FRUSTUM_PLANE_LEFT] = Plane(1, 0, 0, 10.0f);
            planes[FRUSTUM_PLANE_RIGHT] = Plane(-1, 0, 0, 10.0f);
            planes[FRUSTUM_PLANE_TOP] = Plane(0, -1, 0, 10.0f);
            planes[FRUSTUM_PLANE_BOTTOM] = Plane(0, 1, 0, 10.0f);
            frustum.planes = planes;
            frustum.farClip = 20.0f;
            frustum.eye = Vector3(0, 0, 0);
        }
    };

    AABB box(const Vector3& centre, float half){
        return AABB(centre - Vector3(half, half, half), centre + Vector3(half, half, half));
    }
}

TEST(VisibleList, JobsAndInlineGiveTheSameList){
    BoxFrustum box3;
    std::mt19937 random(43);
    std::uniform_real_distribution<float> position(-25.0f, 25.0f);
    std::uniform_real_distribution<float> size(0.1f, 4.0f);
    const size_t count = 5000;
    std::vector<VisibleCandidate> candidates(count);
    std::vector<AABB> submeshBounds(count * 3);
    std::vector<const AABB*> submeshPointers(count * 3);
    std::vector<Matrix4> models(count);
    for (size_t i = 0; i < count; i++){
        const Vector3 centre(position(random), position(random), position(random));
        const float half = size(random);
        models[i] = Matrix4::translateMatrix(centre);
        // three submeshes side by side along x, in the mesh's space
        for (int s = 0; s < 3; s++)
            submeshBounds[i * 3 + s] = box(Vector3((float)(s - 1) * half * 2.0f, 0, 0), half);
        for (int s = 0; s < 3; s++)
            submeshPointers[i * 3 + s] = &submeshBounds[i * 3 + s];
        VisibleCandidate& candidate = candidates[i];
        candidate.worldBounds = box(centre, half * 3.0f);
        candidate.model = &models[i];
        candidate.submeshBounds = &submeshPointers[i * 3];
        candidate.numSubmeshes = 3;
        candidate.cullBounds = (i % 7) != 0;
        candidate.maxDistance = (i % 5 == 0) ? 12.0f : std::numeric_limits<float>::infinity();
    }
    std::vector<VisibleResult> parallel(count);
    std::vector<VisibleResult> inline_(count);
    VisibleList::cull(box3.frustum, candidates.data(), count, parallel.data(), true);
    VisibleList::cull(box3.frustum, candidates.data(), count, inline_.data(), false);
    size_t kept = 0;
    size_t partial = 0;
    for (size_t i = 0; i < count; i++){
        EXPECT_EQ(parallel[i].submeshMask, inline_[i].submeshMask) << i;
        EXPECT_EQ(parallel[i].distance2, inline_[i].distance2) << i;
        kept += parallel[i].submeshMask != 0;
        partial += parallel[i].submeshMask != 0 && parallel[i].submeshMask != 7u;
    }
    // (the random scene keeps some, culls some, and splits some meshes)
    EXPECT_GT(kept, 100u);
    EXPECT_LT(kept, count);
    EXPECT_GT(partial, 0u);
}

TEST(VisibleList, CullsSubmeshesByTheirOwnBounds){
    BoxFrustum box3;
    // a mesh across the right plane: its left submesh inside, its right one outside
    Matrix4 model = Matrix4::translateMatrix(Vector3(9.0f, 0.0f, 0.0f));
    AABB bounds[2] = {box(Vector3(-2.0f, 0, 0), 1.0f), box(Vector3(4.0f, 0, 0), 1.0f)};
    const AABB* boundsOf[2] = {&bounds[0], &bounds[1]};
    VisibleCandidate candidate;
    candidate.worldBounds = AABB(Vector3(6.0f, -1.0f, -1.0f), Vector3(14.0f, 1.0f, 1.0f));
    candidate.model = &model;
    candidate.submeshBounds = boundsOf;
    candidate.numSubmeshes = 2;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 1u);

    // the submeshes the pass draws only (a queue's): the culled one stays out, the other too
    candidate.submeshMask = 2u;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 0u);

    // without submesh bounds the mesh's decide for all of them
    candidate.submeshMask = 0xFFFFFFFFu;
    candidate.submeshBounds = nullptr;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 3u);

    // a submesh of unknown (null) bounds is kept with its mesh
    AABB unknown[2] = {AABB(), box(Vector3(4.0f, 0, 0), 1.0f)};
    const AABB* unknownOf[2] = {&unknown[0], &unknown[1]};
    candidate.submeshBounds = unknownOf;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 1u);
}

TEST(VisibleList, SubmeshBoundsApartInMemory){
    // a mesh's submeshes are allocated one by one (StableArray): each submesh's own bounds, wherever they are
    std::vector<std::unique_ptr<AABB>> held;
    held.push_back(std::make_unique<AABB>(box(Vector3(-5, 0, 0), 1.0f)));
    held.push_back(std::make_unique<AABB>(box(Vector3(50, 0, 0), 1.0f)));
    held.push_back(std::make_unique<AABB>(box(Vector3(5, 0, 0), 1.0f)));
    const AABB* bounds[3] = {held[2].get(), held[1].get(), held[0].get()};
    BoxFrustum box3;
    Matrix4 model;
    VisibleCandidate candidate;
    candidate.worldBounds = AABB(Vector3(-6, -1, -1), Vector3(51, 1, 1));
    candidate.model = &model;
    candidate.submeshBounds = bounds;
    candidate.numSubmeshes = 3;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 5u);
}

TEST(VisibleList, DistanceAndUnboundedMeshes){
    BoxFrustum box3;
    VisibleCandidate candidate;
    candidate.worldBounds = box(Vector3(0, 0, 8), 1.0f);
    candidate.numSubmeshes = 1;
    VisibleResult result = VisibleList::cull(box3.frustum, candidate);
    EXPECT_EQ(result.submeshMask, 1u);
    EXPECT_FLOAT_EQ(result.distance2, 49.0f);

    // past its distance from the eye
    candidate.maxDistance = 5.0f;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 0u);
    candidate.maxDistance = 8.0f;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 1u);

    // outside, and unbounded (cullBounds false: drawn wherever the frustum is)
    candidate.maxDistance = std::numeric_limits<float>::infinity();
    candidate.worldBounds = box(Vector3(0, 0, 40), 1.0f);
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 0u);
    candidate.cullBounds = false;
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 1u);

    // a null box is outside, as RenderSystem::isInsideCamera
    candidate.cullBounds = true;
    candidate.worldBounds = AABB();
    EXPECT_EQ(VisibleList::cull(box3.frustum, candidate).submeshMask, 0u);
}
