// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include "math/AABB.h"
#include "math/Plane.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(Plane, ConstantIsTheDistanceAlongTheNormal){
    // Plane(normal, constant) is the plane normal . p = constant, stored as d = -constant
    const Plane plane(Vector3::UNIT_Y, 3.0f);
    EXPECT_FLOAT_EQ(plane.d, -3.0f);
    EXPECT_FLOAT_EQ(plane.getDistance(Vector3(0, 3, 0)), 0.0f);
    EXPECT_FLOAT_EQ(Plane(0, 1, 0, 3).d, 3.0f);
}

TEST(Plane, NormalAndPointGiveTheConstant){
    const Plane plane(Vector3::UNIT_Y, Vector3(0, 3, 0));
    EXPECT_EQ(plane.normal, Vector3::UNIT_Y);
    EXPECT_FLOAT_EQ(plane.d, -3.0f);
}

TEST(Plane, ThreePointsAreCounterClockwise){
    const Plane plane(Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0));
    EXPECT_VEC_NEAR(plane.normal, Vector3::UNIT_Z, 1e-6f);
    EXPECT_FLOAT_EQ(plane.d, 0.0f);
}

TEST(Plane, DistanceIsSigned){
    const Plane plane(Vector3::UNIT_Y, 0.0f);
    EXPECT_FLOAT_EQ(plane.getDistance(Vector3(4, 2, 1)), 2.0f);
    EXPECT_FLOAT_EQ(plane.getDistance(Vector3(4, -3, 1)), -3.0f);
}

TEST(Plane, SideOfAPoint){
    const Plane plane(Vector3::UNIT_X, 1.0f);
    EXPECT_EQ(plane.getSide(Vector3(2, 0, 0)), Plane::POSITIVE_SIDE);
    EXPECT_EQ(plane.getSide(Vector3(0, 0, 0)), Plane::NEGATIVE_SIDE);
    EXPECT_EQ(plane.getSide(Vector3(1, 5, 5)), Plane::NO_SIDE);
}

TEST(Plane, SideOfABox){
    const Plane plane(Vector3::UNIT_Y, 0.0f);
    EXPECT_EQ(plane.getSide(AABB(Vector3(0, 1, 0), Vector3(1, 2, 1))), Plane::POSITIVE_SIDE);
    EXPECT_EQ(plane.getSide(AABB(Vector3(0, -2, 0), Vector3(1, -1, 1))), Plane::NEGATIVE_SIDE);
    EXPECT_EQ(plane.getSide(AABB(Vector3(0, -1, 0), Vector3(1, 1, 1))), Plane::BOTH_SIDE);
}

TEST(Plane, NormalizeScalesNormalAndConstant){
    Plane plane(Vector3(0, 2, 0), 4.0f);
    plane.normalize();
    EXPECT_EQ(plane.normal, Vector3::UNIT_Y);
    EXPECT_FLOAT_EQ(plane.d, -2.0f);
    EXPECT_EQ(Plane(0, 0, 3, 3).normalized(), Plane(0, 0, 1, 1));
}

TEST(Plane, NegationFlipsTheSide){
    const Plane plane(Vector3::UNIT_Y, 1.0f);
    const Plane flipped = -plane;
    EXPECT_EQ(flipped.normal, -Vector3::UNIT_Y);
    EXPECT_FLOAT_EQ(flipped.d, 1.0f);
    EXPECT_EQ(flipped.getSide(Vector3(0, 5, 0)), Plane::NEGATIVE_SIDE);
}

TEST(Plane, ProjectVectorDropsTheNormalComponent){
    const Plane plane(Vector3::UNIT_Y, 0.0f);
    EXPECT_VEC_NEAR(plane.projectVector(Vector3(1, 5, 2)), Vector3(1, 0, 2), 1e-6f);
}
