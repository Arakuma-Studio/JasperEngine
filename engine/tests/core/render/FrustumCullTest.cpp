// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// FrustumCull: the culling of batched objects (sprite-batching spec 6.9.1) gives the verdict of
// the six-plane box test RenderSystem::isInsideCamera makes (Plane::getSide), through the
// one-axis path on 2D cameras' planes; its serial changes only with the planes.

#include "render/FrustumCull.h"
#include "component/CameraComponent.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Vector4.h"

#include <gtest/gtest.h>

#include <random>

using namespace jasperengine;

namespace {
    // the planes RenderSystem::updateCameraFrustumPlanes makes of a view-projection
    void frustumCullPlanes(const Matrix4& m, Plane planes[6]){
        struct Rule { int plane; int row; float sign; };
        const Rule rules[6] = {{FRUSTUM_PLANE_LEFT, 0, 1.0f}, {FRUSTUM_PLANE_RIGHT, 0, -1.0f}, {FRUSTUM_PLANE_TOP, 1, -1.0f},
                               {FRUSTUM_PLANE_BOTTOM, 1, 1.0f}, {FRUSTUM_PLANE_NEAR, 2, 1.0f}, {FRUSTUM_PLANE_FAR, 2, -1.0f}};
        for (const Rule& rule : rules){
            Plane& plane = planes[rule.plane];
            plane.normal.x = m[0][3] + rule.sign * m[0][rule.row];
            plane.normal.y = m[1][3] + rule.sign * m[1][rule.row];
            plane.normal.z = m[2][3] + rule.sign * m[2][rule.row];
            plane.d = m[3][3] + rule.sign * m[3][rule.row];
            const float length = plane.normal.normalizeL();
            plane.d /= length;
        }
    }

    bool frustumCullGeneral(const Plane planes[6], float farClip, const AABB& box){
        for (int p = 0; p < 6; p++){
            if (p == FRUSTUM_PLANE_FAR && farClip == 0)
                continue;
            if (planes[p].getSide(box.getCenter(), box.getHalfSize()) == Plane::Side::NEGATIVE_SIDE)
                return false;
        }
        return true;
    }

    int frustumCullCompare(const Matrix4& viewProjection, float farClip, bool expectAxisAligned){
        Plane planes[6];
        frustumCullPlanes(viewProjection, planes);
        FrustumCull cull;
        cull.set(planes, farClip);
        EXPECT_EQ(cull.isAxisAligned(), expectAxisAligned);

        std::mt19937 random(1234);
        std::uniform_real_distribution<float> position(-300.0f, 300.0f);
        std::uniform_real_distribution<float> size(0.0f, 80.0f);
        int mismatches = 0;
        int inside = 0;
        for (int i = 0; i < 10000; i++){
            const float lo[3] = {position(random), position(random), position(random) * 0.1f};
            const float hi[3] = {lo[0] + size(random), lo[1] + size(random), lo[2] + size(random) * 0.1f};
            const AABB box(Vector3(lo[0], lo[1], lo[2]), Vector3(hi[0], hi[1], hi[2]));
            const bool expected = frustumCullGeneral(planes, farClip, box);
            mismatches += cull.inside(lo, hi) != expected;
            inside += expected;
        }
        EXPECT_GT(inside, 100) << "the boxes should test both sides";
        EXPECT_LT(inside, 9900);
        return mismatches;
    }
}

TEST(FrustumCull, AxisAlignedMatchesGeneral){
    const Matrix4 ortho = Matrix4::orthoMatrix(-160.0f, 160.0f, -90.0f, 90.0f, -10.0f, 10.0f);
    EXPECT_EQ(frustumCullCompare(ortho * Matrix4::translateMatrix(Vector3(-20.0f, 7.5f, 0.0f)), 10.0f, true), 0);
}

TEST(FrustumCull, RolledAndPerspectiveMatchGeneral){
    const Matrix4 ortho = Matrix4::orthoMatrix(-160.0f, 160.0f, -90.0f, 90.0f, -10.0f, 10.0f);
    EXPECT_EQ(frustumCullCompare(ortho * Matrix4::rotateMatrix(30.0f, Vector3(0.0f, 0.0f, 1.0f)), 10.0f, false), 0);
    const Matrix4 perspective = Matrix4::perspectiveMatrix(1.0f, 16.0f / 9.0f, 0.1f, 500.0f) *
        Matrix4::lookAtMatrix(Vector3(30.0f, 40.0f, 200.0f), Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_EQ(frustumCullCompare(perspective, 500.0f, false), 0);
    EXPECT_EQ(frustumCullCompare(perspective, 0.0f, false), 0);
}

TEST(FrustumCull, SerialMovesOnlyWithThePlanes){
    Plane planes[6];
    frustumCullPlanes(Matrix4::orthoMatrix(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f), planes);
    FrustumCull cull;
    cull.set(planes, 1.0f);
    const uint32_t first = cull.getSerial();
    EXPECT_NE(first, 0u);
    cull.set(planes, 1.0f);
    EXPECT_EQ(cull.getSerial(), first);
    planes[2].d += 0.5f;
    cull.set(planes, 1.0f);
    EXPECT_NE(cull.getSerial(), first);
    const uint32_t second = cull.getSerial();
    cull.set(planes, 2.0f);
    EXPECT_NE(cull.getSerial(), second);
}
