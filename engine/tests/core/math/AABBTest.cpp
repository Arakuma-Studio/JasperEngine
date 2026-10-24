// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include "math/AABB.h"
#include "math/OBB.h"
#include "math/Plane.h"
#include "math/Sphere.h"

#include <gtest/gtest.h>

#include <limits>

using namespace jasperengine;

namespace {
    AABB unitBox(){
        return AABB(Vector3(-1, -1, -1), Vector3(1, 1, 1));
    }
}

TEST(AABB, DefaultIsNull){
    const AABB box;
    EXPECT_TRUE(box.isNull());
    EXPECT_FALSE(box.isFinite());
    EXPECT_FLOAT_EQ(box.volume(), 0.0f);
}

TEST(AABB, ExtentsMakeItFinite){
    const AABB box(1, 2, 3, 4, 6, 8);
    EXPECT_TRUE(box.isFinite());
    EXPECT_EQ(box.getMinimum(), Vector3(1, 2, 3));
    EXPECT_EQ(box.getMaximum(), Vector3(4, 6, 8));
}

TEST(AABB, CenterSizeAndVolume){
    const AABB box(Vector3(0, 0, 0), Vector3(2, 4, 6));
    EXPECT_EQ(box.getCenter(), Vector3(1, 2, 3));
    EXPECT_EQ(box.getSize(), Vector3(2, 4, 6));
    EXPECT_EQ(box.getHalfSize(), Vector3(1, 2, 3));
    EXPECT_FLOAT_EQ(box.volume(), 48.0f);
}

TEST(AABB, InfiniteBoxHasInfiniteVolume){
    const AABB box(AABB::BOXTYPE_INFINITE);
    EXPECT_TRUE(box.isInfinite());
    EXPECT_EQ(box.volume(), std::numeric_limits<float>::infinity());
}

TEST(AABB, CornersFollowTheEnum){
    const AABB box = unitBox();
    EXPECT_EQ(box.getCorner(AABB::FAR_LEFT_BOTTOM), Vector3(-1, -1, -1));
    EXPECT_EQ(box.getCorner(AABB::NEAR_RIGHT_TOP), Vector3(1, 1, 1));
    EXPECT_EQ(box.getCorner(AABB::FAR_RIGHT_TOP), Vector3(1, 1, -1));
    EXPECT_EQ(box.getCorner(AABB::NEAR_LEFT_BOTTOM), Vector3(-1, -1, 1));
}

TEST(AABB, MergeWithAPointGrowsTheBox){
    AABB box;
    box.merge(Vector3(1, 2, 3));
    EXPECT_TRUE(box.isFinite());
    EXPECT_EQ(box.getMinimum(), Vector3(1, 2, 3));
    box.merge(Vector3(-1, 5, 0));
    EXPECT_EQ(box.getMinimum(), Vector3(-1, 2, 0));
    EXPECT_EQ(box.getMaximum(), Vector3(1, 5, 3));
}

TEST(AABB, MergeWithBoxes){
    AABB box = unitBox();
    box.merge(AABB(Vector3(0, 0, 0), Vector3(3, 3, 3)));
    EXPECT_EQ(box.getMaximum(), Vector3(3, 3, 3));
    EXPECT_EQ(box.getMinimum(), Vector3(-1, -1, -1));
    box.merge(AABB());
    EXPECT_EQ(box.getMaximum(), Vector3(3, 3, 3));
    box.merge(AABB(AABB::BOXTYPE_INFINITE));
    EXPECT_TRUE(box.isInfinite());
}

TEST(AABB, IntersectsOverlappingAndTouchingBoxes){
    const AABB box = unitBox();
    EXPECT_TRUE(box.intersects(AABB(Vector3(0.5f, 0.5f, 0.5f), Vector3(2, 2, 2))));
    EXPECT_TRUE(box.intersects(AABB(Vector3(1, -1, -1), Vector3(2, 1, 1))));
    EXPECT_FALSE(box.intersects(AABB(Vector3(1.1f, 0, 0), Vector3(2, 1, 1))));
    EXPECT_FALSE(box.intersects(AABB()));
    EXPECT_TRUE(box.intersects(AABB(AABB::BOXTYPE_INFINITE)));
}

TEST(AABB, IntersectionIsTheOverlap){
    const AABB overlap = unitBox().intersection(AABB(Vector3(0, 0, 0), Vector3(5, 5, 5)));
    EXPECT_EQ(overlap.getMinimum(), Vector3(0, 0, 0));
    EXPECT_EQ(overlap.getMaximum(), Vector3(1, 1, 1));
    EXPECT_TRUE(unitBox().intersection(AABB(Vector3(2, 2, 2), Vector3(3, 3, 3))).isNull());
}

TEST(AABB, ContainsPointsAndBoxes){
    const AABB box = unitBox();
    EXPECT_TRUE(box.contains(Vector3(0, 0, 0)));
    EXPECT_TRUE(box.contains(Vector3(1, 1, 1)));
    EXPECT_FALSE(box.contains(Vector3(1.01f, 0, 0)));
    EXPECT_TRUE(box.contains(AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f))));
    EXPECT_FALSE(box.contains(AABB(Vector3(0, 0, 0), Vector3(2, 2, 2))));
    EXPECT_TRUE(box.contains(AABB()));
}

TEST(AABB, DistanceToPoints){
    const AABB box = unitBox();
    EXPECT_FLOAT_EQ(box.squaredDistance(Vector3(0.5f, 0, 0)), 0.0f);
    EXPECT_FLOAT_EQ(box.squaredDistance(Vector3(4, 0, 0)), 9.0f);
    EXPECT_FLOAT_EQ(box.distance(Vector3(4, 5, 1)), 5.0f);
}

TEST(AABB, IntersectsSpheres){
    const AABB box = unitBox();
    EXPECT_TRUE(box.intersects(Sphere(Vector3(2, 0, 0), 1.0f)));
    EXPECT_FALSE(box.intersects(Sphere(Vector3(2, 2, 0), 1.0f)));
    EXPECT_TRUE(box.intersects(Sphere(Vector3(0, 0, 0), 0.1f)));
}

TEST(AABB, IntersectsAPlaneOnlyAcrossIt){
    const AABB box = unitBox();
    EXPECT_TRUE(box.intersects(Plane(Vector3::UNIT_Y, 0.5f)));
    EXPECT_FALSE(box.intersects(Plane(Vector3::UNIT_Y, -2.0f)));
}

TEST(AABB, ScaleMultipliesTheCorners){
    AABB box(Vector3(1, 1, 1), Vector3(2, 3, 4));
    box.scale(Vector3(2, 2, 2));
    EXPECT_EQ(box.getMinimum(), Vector3(2, 2, 2));
    EXPECT_EQ(box.getMaximum(), Vector3(4, 6, 8));
}

TEST(AABB, TransformKeepsTheRotatedCornersInside){
    AABB box(Vector3(0, 0, 0), Vector3(2, 1, 1));
    box.transform(Matrix4::rotateMatrix(90.0f, Vector3::UNIT_Z));
    EXPECT_VEC_NEAR(box.getMinimum(), Vector3(-1, 0, 0), 1e-5f);
    EXPECT_VEC_NEAR(box.getMaximum(), Vector3(0, 2, 1), 1e-5f);
}

TEST(AABB, EqualityAndCopy){
    const AABB a = unitBox();
    AABB b = a;
    EXPECT_TRUE(a == b);
    b.setMaximumX(3);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(AABB() == AABB());
}

TEST(AABB, GetOBBKeepsCenterAndHalfSize){
    const OBB obb = AABB(Vector3(0, 0, 0), Vector3(2, 4, 6)).getOBB();
    EXPECT_EQ(obb.getCenter(), Vector3(1, 2, 3));
    EXPECT_EQ(obb.getHalfExtents(), Vector3(1, 2, 3));
    EXPECT_TRUE(AABB().getOBB().isNull());
}
