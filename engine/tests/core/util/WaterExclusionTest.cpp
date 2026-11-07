// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The volumes of Water Excluders without a scene (util/WaterExclusion.h,
// docs/superpowers/specs/2026-11-04-water-boats-design.md): the shape tests the physics, the
// queries and the shader agree on, the convex hull of a point set and its map seen from above.

#include "util/WaterExclusion.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace jasperengine;

namespace water_exclusion_test {

    // a turned, non-uniformly scaled frame: a point given in its local space goes out to world
    Matrix4 frame(){
        return Matrix4::translateMatrix(Vector3(3.0f, -1.0f, 2.0f)) * Matrix4::rotateYMatrix(0.7f) *
               Matrix4::scaleMatrix(Vector3(2.0f, 0.5f, 1.5f));
    }

    // a triangular prism along Z: the triangle (-1, 0), (1, 0), (0, 2) in XY, z from -1 to 1
    std::vector<Vector3> prism(){
        std::vector<Vector3> points;
        for (float z : {-1.0f, 1.0f}){
            points.push_back(Vector3(-1.0f, 0.0f, z));
            points.push_back(Vector3(1.0f, 0.0f, z));
            points.push_back(Vector3(0.0f, 2.0f, z));
        }
        return points;
    }

    // the map texel at local (x, z) of the hull's map area
    const uint8_t* mapTexel(const WaterHull& hull, const std::vector<uint8_t>& map, float x, float z){
        Vector3 origin;
        Vector3 extent;
        getWaterHullMapArea(hull.bounds, origin, extent);
        const int size = WATER_EXCLUDER_HULL_MAP_SIZE;
        const int tx = std::clamp((int)((x - origin.x) / extent.x * size), 0, size - 1);
        const int tz = std::clamp((int)((z - origin.z) / extent.z * size), 0, size - 1);
        return &map[((size_t)tz * size + tx) * 4];
    }

}

using namespace water_exclusion_test;

TEST(WaterExclusion, BoxAndSphereContain){
    const Matrix4 model = frame();
    const AABB box = getWaterExcluderLocalBox(WaterExcluderShape::BOX, Vector3(0.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, -4.0f), nullptr);
    ASSERT_FALSE(box.isNull());
    // the size's sign does not matter
    EXPECT_FLOAT_EQ(box.getSize().z, 4.0f);

    EXPECT_TRUE(isInsideWaterExcluder(WaterExcluderShape::BOX, box, nullptr, model, model * Vector3(0.9f, 1.9f, 1.9f)));
    EXPECT_FALSE(isInsideWaterExcluder(WaterExcluderShape::BOX, box, nullptr, model, model * Vector3(1.1f, 1.0f, 0.0f)));
    EXPECT_FALSE(isInsideWaterExcluder(WaterExcluderShape::BOX, box, nullptr, model, model * Vector3(0.0f, -0.1f, 0.0f)));

    // the sphere is the ellipsoid inside the box: its corner is out, near its centre in
    EXPECT_TRUE(isInsideWaterExcluder(WaterExcluderShape::SPHERE, box, nullptr, model, model * Vector3(0.0f, 1.0f, 1.9f)));
    EXPECT_FALSE(isInsideWaterExcluder(WaterExcluderShape::SPHERE, box, nullptr, model, model * Vector3(0.9f, 1.9f, 1.9f)));

    // the unit space the shader tests: the box's corners at 0 and 1
    Matrix4 unit;
    ASSERT_TRUE(getWaterExcluderUnitSpace(box, false, model, unit));
    const Vector3 low = unit * (model * Vector3(-1.0f, 0.0f, -2.0f));
    const Vector3 high = unit * (model * Vector3(1.0f, 2.0f, 2.0f));
    EXPECT_NEAR(low.x, 0.0f, 1e-4f);
    EXPECT_NEAR(low.y, 0.0f, 1e-4f);
    EXPECT_NEAR(low.z, 0.0f, 1e-4f);
    EXPECT_NEAR(high.x, 1.0f, 1e-4f);
    EXPECT_NEAR(high.y, 1.0f, 1e-4f);
    EXPECT_NEAR(high.z, 1.0f, 1e-4f);

    // sized or scaled to nothing it keeps nothing out
    EXPECT_TRUE(getWaterExcluderLocalBox(WaterExcluderShape::BOX, Vector3(), Vector3(1.0f, 0.0f, 1.0f), nullptr).isNull());
    const Matrix4 flat = Matrix4::scaleMatrix(Vector3(1.0f, 0.0f, 1.0f));
    EXPECT_FALSE(isInsideWaterExcluder(WaterExcluderShape::BOX, box, nullptr, flat, Vector3(0.0f, 0.0f, 0.0f)));
    EXPECT_FALSE(getWaterExcluderUnitSpace(box, false, flat, unit));
}

TEST(WaterExclusion, HullFromPrism){
    WaterHull hull;
    const WaterHullStatus status = buildWaterHull(prism(), hull);
#ifdef JASPERENGINE_PHYSICS_3D
    ASSERT_EQ(status, WaterHullStatus::READY);
    // five faces (the builder may split a quad into triangles: their planes coincide)
    std::vector<Vector3> normals;
    for (const Vector4& plane : hull.planes){
        const Vector3 normal(plane.x, plane.y, plane.z);
        bool seen = false;
        for (const Vector3& other : normals)
            seen = seen || normal.dotProduct(other) > 0.999f;
        if (!seen)
            normals.push_back(normal);
    }
    EXPECT_EQ(normals.size(), 5u);
    EXPECT_EQ(hull.points.size(), 6u);
    EXPECT_GE(hull.edges.size(), 9u);
    EXPECT_EQ(hull.sourceVertices, 6u);

    const Matrix4 model = frame();
    const AABB box = getWaterExcluderLocalBox(WaterExcluderShape::CONVEX_HULL, Vector3(), Vector3(), &hull);
    ASSERT_FALSE(box.isNull());
    EXPECT_TRUE(isInsideWaterExcluder(WaterExcluderShape::CONVEX_HULL, box, &hull, model, model * Vector3(0.0f, 1.0f, 0.5f)));
    // inside the box, outside the slanted faces
    EXPECT_FALSE(isInsideWaterExcluder(WaterExcluderShape::CONVEX_HULL, box, &hull, model, model * Vector3(0.8f, 1.5f, 0.0f)));
    EXPECT_FALSE(isInsideWaterExcluder(WaterExcluderShape::CONVEX_HULL, box, &hull, model, model * Vector3(0.0f, 1.0f, 1.2f)));
#else
    EXPECT_EQ(status, WaterHullStatus::NO_PHYSICS);
    EXPECT_FALSE(hull.bounds.isNull());
#endif
}

#ifdef JASPERENGINE_PHYSICS_3D
TEST(WaterExclusion, HullMapColumns){
    WaterHull hull;
    ASSERT_EQ(buildWaterHull(prism(), hull), WaterHullStatus::READY);
    std::vector<uint8_t> map;
    buildWaterHullMap(hull, map);
    ASSERT_EQ(map.size(), (size_t)WATER_EXCLUDER_HULL_MAP_SIZE * WATER_EXCLUDER_HULL_MAP_SIZE * 4);

    // over the ridge: bottom 0, top the whole height
    const uint8_t* ridge = mapTexel(hull, map, 0.0f, 0.0f);
    EXPECT_LE(ridge[0], 2);
    EXPECT_GE(ridge[1], 250);
    EXPECT_LT(ridge[2], 128);
    // half way down a slanted face: top at half the height
    const uint8_t* slope = mapTexel(hull, map, 0.5f, 0.0f);
    EXPECT_NEAR(slope[1], 128, 4);
    // outside the end walls (z beyond 1): an empty column, past the walls
    const uint8_t* outside = mapTexel(hull, map, 0.0f, 1.03f);
    EXPECT_GT(outside[2], 128);
    // in the margin beyond the sides (x beyond 1): no column (the top under the bottom)
    const uint8_t* side = mapTexel(hull, map, 1.02f, 0.0f);
    EXPECT_EQ(side[0], 255);
    EXPECT_EQ(side[1], 0);
}

TEST(WaterExclusion, DenseInputIsBounded){
    // a noisy dense box: the snapped input and the hull stay small
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::vector<Vector3> points;
    for (int i = 0; i < 50000; i++)
        points.push_back(Vector3(unit(rng), unit(rng) * 0.5f, unit(rng) * 2.0f));
    WaterHull hull;
    ASSERT_EQ(buildWaterHull(points, hull), WaterHullStatus::READY);
    EXPECT_EQ(hull.sourceVertices, 50000u);
    EXPECT_LE(hull.points.size(), (size_t)WATER_EXCLUDER_HULL_MAX_VERTICES);
    EXPECT_GE(hull.planes.size(), 6u);
}
#endif

TEST(WaterExclusion, FlatAndEmpty){
    WaterHull hull;
    EXPECT_EQ(buildWaterHull({}, hull), WaterHullStatus::NO_MESH);
    const std::vector<Vector3> plane = {Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(1, 0, 1), Vector3(-1, 0, 1)};
    EXPECT_EQ(buildWaterHull(plane, hull), WaterHullStatus::FLAT);
    EXPECT_TRUE(getWaterExcluderLocalBox(WaterExcluderShape::CONVEX_HULL, Vector3(), Vector3(1, 1, 1), &hull).isNull());
    EXPECT_STREQ(getWaterHullStatusName(WaterHullStatus::NO_PHYSICS), "no_physics");
}
