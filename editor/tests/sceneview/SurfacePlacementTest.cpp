// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 R16 (AC10's maths): a dropped object rests its bottom on the surface hit, and Align to
// Surface turns its up axis onto the surface normal

#include "sceneview/SurfacePlacement.h"

#include "util/Angle.h"

#include <gtest/gtest.h>

#include <cfloat>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the distance from the plane (point, normal) of the box's deepest corner below it
    float deepestBelow(const AABB& local, const Vector3& scale, const Quaternion& rotation, const Vector3& position,
                       const Vector3& point, const Vector3& normal){
        float deepest = FLT_MAX;
        for (int corner = 0; corner < 8; corner++){
            const Vector3 mn = local.getMinimum(), mx = local.getMaximum();
            const Vector3 c((corner & 1) ? mx.x : mn.x, (corner & 2) ? mx.y : mn.y, (corner & 4) ? mx.z : mn.z);
            const Vector3 world = position + rotation * Vector3(c.x * scale.x, c.y * scale.y, c.z * scale.z);
            deepest = std::min(deepest, (world - point).dotProduct(normal));
        }
        return deepest;
    }

}

TEST(SurfacePlacement, RestsOnTiltedPlane){
    // a unit cube centred on its origin, dropped on a plane tilted 30 degrees about x
    const AABB cube(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f));
    const float angle = 30.0f * 3.14159265f / 180.0f;
    const Vector3 normal(0.0f, std::cos(angle), std::sin(angle));
    const Vector3 point(2.0f, 1.0f, -3.0f);

    Vector3 position;
    Quaternion rotation;
    sceneview::restOnSurface(point, normal, cube, Vector3(1, 1, 1), Quaternion(), false, position, rotation);
    EXPECT_NEAR(deepestBelow(cube, Vector3(1, 1, 1), rotation, position, point, normal), 0.0f, 1e-4f) << "its lowest corner touches";
    EXPECT_TRUE(rotation == Quaternion()) << "not aligned: the rotation is kept";

    // Align to Surface: its up axis is the normal, and its bottom face lies on the plane
    sceneview::restOnSurface(point, normal, cube, Vector3(2, 1, 2), Quaternion(), true, position, rotation);
    const Vector3 up = rotation * Vector3(0, 1, 0);
    EXPECT_GT(up.dotProduct(normal), std::cos(0.5f * 3.14159265f / 180.0f)) << "within 0.5 degrees";
    EXPECT_NEAR(deepestBelow(cube, Vector3(2, 1, 2), rotation, position, point, normal), 0.0f, 1e-4f);
    // the bottom face: all four bottom corners on the plane
    for (int corner = 0; corner < 4; corner++){
        const Vector3 c((corner & 1) ? 0.5f : -0.5f, -0.5f, (corner & 2) ? 0.5f : -0.5f);
        const Vector3 world = position + rotation * Vector3(c.x * 2.0f, c.y, c.z * 2.0f);
        EXPECT_NEAR((world - point).dotProduct(normal), 0.0f, 1e-3f);
    }
}

TEST(SurfacePlacement, OffsetPivotAndOwnRotation){
    // a model whose pivot is at its bottom, turned 90 degrees about y: on the ground it stays put
    const AABB model(Vector3(-1.0f, 0.0f, -2.0f), Vector3(1.0f, 3.0f, 2.0f));
    const Quaternion yaw(Angle::degToDefault(90.0f), Vector3(0, 1, 0));
    Vector3 position;
    Quaternion rotation;
    sceneview::restOnSurface(Vector3(5, 0, 5), Vector3(0, 1, 0), model, Vector3(1, 1, 1), yaw, true, position, rotation);
    EXPECT_NEAR((position - Vector3(5, 0, 5)).length(), 0.0f, 1e-4f);
    EXPECT_NEAR(rotation.dot(yaw) * rotation.dot(yaw), 1.0f, 1e-5f) << "up already along the normal: the heading is kept";

    // upside-down surface (a ceiling): the up axis turns over
    sceneview::restOnSurface(Vector3(0, 4, 0), Vector3(0, -1, 0), model, Vector3(1, 1, 1), Quaternion(), true, position, rotation);
    EXPECT_NEAR((rotation * Vector3(0, 1, 0)).y, -1.0f, 1e-4f);
    EXPECT_NEAR(deepestBelow(model, Vector3(1, 1, 1), rotation, position, Vector3(0, 4, 0), Vector3(0, -1, 0)), 0.0f, 1e-4f);
}

TEST(SurfacePlacement, NoBoundsLandsTheOrigin){
    Vector3 position;
    Quaternion rotation;
    sceneview::restOnSurface(Vector3(1, 2, 3), Vector3(0, 1, 0), AABB(), Vector3(1, 1, 1), Quaternion(), false, position, rotation);
    EXPECT_NEAR((position - Vector3(1, 2, 3)).length(), 0.0f, 1e-6f);
}
