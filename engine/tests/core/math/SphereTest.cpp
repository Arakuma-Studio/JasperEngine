// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include "math/AABB.h"
#include "math/Plane.h"
#include "math/Sphere.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(Sphere, ContainsPointsUpToTheSurface){
    const Sphere sphere(Vector3(1, 0, 0), 2.0f);
    EXPECT_TRUE(sphere.contains(Vector3(1, 0, 0)));
    EXPECT_TRUE(sphere.contains(Vector3(3, 0, 0)));
    EXPECT_FALSE(sphere.contains(Vector3(3.1f, 0, 0)));
}

TEST(Sphere, IntersectsOtherSpheres){
    const Sphere a(Vector3::ZERO, 1.0f);
    EXPECT_TRUE(a.intersects(Sphere(Vector3(2, 0, 0), 1.0f)));
    EXPECT_FALSE(a.intersects(Sphere(Vector3(2.1f, 0, 0), 1.0f)));
}

TEST(Sphere, IntersectsPlanesWithinTheRadius){
    const Sphere sphere(Vector3(0, 2, 0), 1.5f);
    EXPECT_TRUE(sphere.intersects(Plane(Vector3::UNIT_Y, 1.0f)));
    EXPECT_FALSE(sphere.intersects(Plane(Vector3::UNIT_Y, 0.0f)));
}

TEST(Sphere, IntersectsBoxes){
    const Sphere sphere(Vector3::ZERO, 1.0f);
    EXPECT_TRUE(sphere.intersects(AABB(Vector3(0.5f, -1, -1), Vector3(2, 1, 1))));
    EXPECT_FALSE(sphere.intersects(AABB(Vector3(1, 1, 1), Vector3(2, 2, 2))));
}

TEST(Sphere, MergeEnclosesBoth){
    Sphere a(Vector3(0, 0, 0), 1.0f);
    a.merge(Sphere(Vector3(4, 0, 0), 1.0f));
    EXPECT_VEC_NEAR(a.center, Vector3(2, 0, 0), 1e-5f);
    EXPECT_FLOAT_EQ(a.radius, 3.0f);
}

TEST(Sphere, MergeWithAContainedSphereChangesNothing){
    Sphere a(Vector3(0, 0, 0), 5.0f);
    a.merge(Sphere(Vector3(1, 0, 0), 1.0f));
    EXPECT_EQ(a.center, Vector3::ZERO);
    EXPECT_FLOAT_EQ(a.radius, 5.0f);
}

TEST(Sphere, MergeWithAContainingSphereBecomesIt){
    Sphere a(Vector3(1, 0, 0), 1.0f);
    a.merge(Sphere(Vector3(0, 0, 0), 5.0f));
    EXPECT_EQ(a.center, Vector3::ZERO);
    EXPECT_FLOAT_EQ(a.radius, 5.0f);
}

TEST(Sphere, AreaAndVolume){
    const Sphere sphere(Vector3::ZERO, 2.0f);
    EXPECT_NEAR(sphere.surfaceArea(), 50.2654825f, 1e-3f);
    EXPECT_NEAR(sphere.volume(), 33.5103216f, 1e-3f);
}
