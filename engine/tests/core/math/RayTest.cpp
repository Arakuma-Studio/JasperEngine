// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A ray's direction is also its length: hits past origin + direction are misses, and
// RayReturn::distance is the fraction of the direction travelled.

#include "MathTestHelpers.h"

#include "math/AABB.h"
#include "math/Plane.h"
#include "math/Ray.h"
#include "math/Sphere.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(Ray, GetPointWalksAlongTheDirection){
    const Ray ray(Vector3(1, 0, 0), Vector3(0, 10, 0));
    EXPECT_EQ(ray.getPoint(0.5f), Vector3(1, 5, 0));
    EXPECT_EQ(ray.getOrigin(), Vector3(1, 0, 0));
    EXPECT_EQ(ray.getDirection(), Vector3(0, 10, 0));
}

TEST(Ray, HitsAPlaneInFront){
    const Ray ray(Vector3(0, 5, 0), Vector3(0, -10, 0));
    const RayReturn hit = ray.intersects(Plane(Vector3::UNIT_Y, 0.0f));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit.distance, 0.5f);
    EXPECT_EQ(hit.point, Vector3(0, 0, 0));
    EXPECT_EQ(hit.normal, Vector3::UNIT_Y);
}

TEST(Ray, MissesAParallelPlane){
    const Ray ray(Vector3(0, 5, 0), Vector3(10, 0, 0));
    EXPECT_FALSE(ray.intersects(Plane(Vector3::UNIT_Y, 0.0f)));
}

TEST(Ray, MissesAPlaneBeyondItsLength){
    const Ray ray(Vector3(0, 5, 0), Vector3(0, -2, 0));
    EXPECT_FALSE(ray.intersects(Plane(Vector3::UNIT_Y, 0.0f)));
}

TEST(Ray, MissesAPlaneBehindIt){
    const Ray ray(Vector3(0, 5, 0), Vector3(0, 10, 0));
    EXPECT_FALSE(ray.intersects(Plane(Vector3::UNIT_Y, 0.0f)));
}

TEST(Ray, HitsTheNearFaceOfABox){
    const Ray ray(Vector3(-5, 0, 0), Vector3(10, 0, 0));
    const RayReturn hit = ray.intersects(AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1)));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit.distance, 0.4f);
    EXPECT_EQ(hit.point, Vector3(-1, 0, 0));
    EXPECT_EQ(hit.normal, Vector3(-1, 0, 0));
}

TEST(Ray, StartingInsideABoxHitsAtZero){
    const Ray ray(Vector3(0, 0, 0), Vector3(10, 0, 0));
    const RayReturn hit = ray.intersects(AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1)));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit.distance, 0.0f);
}

TEST(Ray, MissesABoxBesideIt){
    const Ray ray(Vector3(-5, 3, 0), Vector3(10, 0, 0));
    EXPECT_FALSE(ray.intersects(AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1))));
    EXPECT_FALSE(ray.intersects(AABB()));
}

TEST(Ray, HitsTheTopOfABoxFromAbove){
    const Ray ray(Vector3(0, 4, 0), Vector3(0, -4, 0));
    const RayReturn hit = ray.intersects(AABB(Vector3(-1, -1, -1), Vector3(1, 2, 1)));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit.distance, 0.5f);
    EXPECT_EQ(hit.normal, Vector3(0, 1, 0));
}

TEST(Ray, HitsTheNearSideOfASphere){
    const Ray ray(Vector3(0, 0, -10), Vector3(0, 0, 20));
    const RayReturn hit = ray.intersects(Sphere(Vector3::ZERO, 2.0f));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit.distance, 0.4f);
    EXPECT_VEC_NEAR(hit.point, Vector3(0, 0, -2), 1e-5f);
    EXPECT_VEC_NEAR(hit.normal, Vector3(0, 0, -1), 1e-5f);
}

TEST(Ray, FromInsideASphereHitsTheFarSide){
    const Ray ray(Vector3::ZERO, Vector3(4, 0, 0));
    const RayReturn hit = ray.intersects(Sphere(Vector3::ZERO, 2.0f));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit.distance, 0.5f);
}

TEST(Ray, MissesASphereBesideIt){
    const Ray ray(Vector3(0, 5, -10), Vector3(0, 0, 20));
    EXPECT_FALSE(ray.intersects(Sphere(Vector3::ZERO, 2.0f)));
}
