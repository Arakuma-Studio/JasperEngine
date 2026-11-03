// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include "math/AABB.h"
#include "math/OBB.h"
#include "math/Sphere.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(OBB, DefaultIsNull){
    EXPECT_TRUE(OBB().isNull());
    EXPECT_TRUE(OBB(Vector3::ZERO, Vector3(1, 1, 1)).isFinite());
}

TEST(OBB, AxisAlignedBoxRoundTripsThroughAABB){
    const OBB obb(Vector3(1, 2, 3), Vector3(1, 1, 2));
    const AABB box = obb.toAABB();
    EXPECT_VEC_NEAR(box.getMinimum(), Vector3(0, 1, 1), 1e-5f);
    EXPECT_VEC_NEAR(box.getMaximum(), Vector3(2, 3, 5), 1e-5f);
}

TEST(OBB, RotatedBoxHasALargerAABB){
    OBB obb(Vector3::ZERO, Vector3(1, 1, 1));
    obb.setOrientation(Quaternion(45.0f, Vector3::UNIT_Z));
    const AABB box = obb.toAABB();
    EXPECT_NEAR(box.getMaximum().x, 1.41421356f, 1e-4f);
    EXPECT_NEAR(box.getMaximum().z, 1.0f, 1e-4f);
}

TEST(OBB, OrientationSetsTheRotatedAxes){
    OBB obb(Vector3::ZERO, Vector3(1, 2, 3));
    const Quaternion q(30.0f, Vector3(0, 1, 1).normalized());
    obb.setOrientation(q);
    EXPECT_VEC_NEAR(obb.getAxisX(), q * Vector3::UNIT_X, 1e-5f);
    EXPECT_VEC_NEAR(obb.getAxisY(), q * Vector3::UNIT_Y, 1e-5f);
    EXPECT_VEC_NEAR(obb.getAxisZ(), q * Vector3::UNIT_Z, 1e-5f);
}

TEST(OBB, IntersectsOtherBoxes){
    const OBB a(Vector3::ZERO, Vector3(1, 1, 1));
    OBB rotated(Vector3(2.2f, 0, 0), Vector3(1, 1, 1));
    EXPECT_FALSE(a.intersects(rotated));
    rotated.setOrientation(Quaternion(45.0f, Vector3::UNIT_Z));
    EXPECT_TRUE(a.intersects(rotated));    // its corner reaches 2.2 - 1.414
    EXPECT_FALSE(a.intersects(OBB()));
}

TEST(OBB, IntersectsAABBAndSphere){
    const OBB obb(Vector3::ZERO, Vector3(1, 1, 1));
    EXPECT_TRUE(obb.intersects(AABB(Vector3(0.5f, 0.5f, 0.5f), Vector3(3, 3, 3))));
    EXPECT_FALSE(obb.intersects(AABB(Vector3(2, 2, 2), Vector3(3, 3, 3))));
    EXPECT_TRUE(obb.intersects(Sphere(Vector3(1.5f, 0, 0), 0.6f)));
    EXPECT_FALSE(obb.intersects(Sphere(Vector3(3, 0, 0), 0.5f)));
}

TEST(OBB, EncloseGrowsToTheOtherBox){
    OBB obb(Vector3::ZERO, Vector3(1, 1, 1));
    obb.enclose(Vector3(3, 0, 0));
    const AABB box = obb.toAABB();
    EXPECT_GE(box.getMaximum().x, 3.0f - 1e-5f);
    EXPECT_LE(box.getMinimum().x, -1.0f + 1e-5f);
}

TEST(OBB, GetOrientationReturnsTheOrientationItWasGiven){
    OBB obb(Vector3::ZERO, Vector3(1, 2, 3));
    const Quaternion q(70.0f, Vector3(1, 2, 3).normalized());
    obb.setOrientation(q);
    const Quaternion back = obb.getOrientation();
    EXPECT_ROTATION_NEAR(back, q, 1e-4f);
    for (const Vector3& v : {Vector3::UNIT_X, Vector3::UNIT_Y, Vector3::UNIT_Z, Vector3(0.3f, -2, 5)})
        EXPECT_VEC_NEAR(back * v, q * v, 1e-4f);
}

TEST(OBB, TransformRotatesAnAlreadyRotatedBox){
    OBB obb(Vector3::ZERO, Vector3(1, 2, 3));
    const Quaternion first(40.0f, Vector3::UNIT_Z);
    const Quaternion second(90.0f, Vector3::UNIT_X);
    obb.setOrientation(first);
    obb.transform(Vector3::ZERO, second, Vector3(1, 1, 1));
    const Quaternion both = second * first;
    EXPECT_VEC_NEAR(obb.getAxisX(), both * Vector3::UNIT_X, 1e-4f);
    EXPECT_VEC_NEAR(obb.getAxisY(), both * Vector3::UNIT_Y, 1e-4f);
    EXPECT_VEC_NEAR(obb.getAxisZ(), both * Vector3::UNIT_Z, 1e-4f);
}
