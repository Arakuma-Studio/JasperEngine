// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// render/WaterLayering.h: the rank the transparent queue layers draws by around the waters
// (docs/superpowers/specs/2026-11-04-water-reflections-design.md 6.5)

#include "render/WaterLayering.h"

#include <gtest/gtest.h>

#include <limits>

using namespace jasperengine;

namespace {

    WaterLayer layer(Entity entity, float level, float band, float halfSize, float x = 0.0f, float z = 0.0f){
        WaterLayer water;
        water.entity = entity;
        water.level = level;
        water.band = band;
        water.minXZ = Vector2(x - halfSize, z - halfSize);
        water.maxXZ = Vector2(x + halfSize, z + halfSize);
        return water;
    }

    AABB box(float x, float y, float z, float half){
        return AABB(Vector3(x - half, y - half, z - half), Vector3(x + half, y + half, z + half));
    }

}

TEST(WaterLayeringTest, Rank){
    const std::vector<WaterLayer> lake = {layer(1, 0.0f, 0.2f, 20.0f)};
    const Vector3 above(0.0f, 5.0f, 8.0f);
    const Vector3 under(0.0f, -3.0f, 8.0f);

    // seen from above: under the surface is behind it, over it in front
    EXPECT_EQ(waterRank(box(0.0f, -1.0f, 0.0f, 0.5f), above, lake, 99), 1);
    EXPECT_EQ(waterRank(box(0.0f, 2.0f, 0.0f, 0.5f), above, lake, 99), -1);
    // seen from below: the other way round
    EXPECT_EQ(waterRank(box(0.0f, 2.0f, 0.0f, 0.5f), under, lake, 99), 1);
    EXPECT_EQ(waterRank(box(0.0f, -1.0f, 0.0f, 0.5f), under, lake, 99), -1);
    // crossing the wave band, or outside the area: not layered by it
    EXPECT_EQ(waterRank(box(0.0f, 0.1f, 0.0f, 0.5f), above, lake, 99), 0);
    EXPECT_EQ(waterRank(box(0.0f, -0.6f, 0.0f, 0.5f), above, lake, 99), 0);
    EXPECT_EQ(waterRank(box(30.0f, -1.0f, 0.0f, 0.5f), above, lake, 99), 0);
    // a box overlapping the area's edge counts
    EXPECT_EQ(waterRank(box(20.3f, -1.0f, 0.0f, 0.5f), above, lake, 99), 1);
    // nothing to rank by
    EXPECT_EQ(waterRank(AABB(), above, lake, 99), 0);
    EXPECT_EQ(waterRank(box(0.0f, -1.0f, 0.0f, 0.5f), above, {}, 99), 0);
    // a water does not rank itself
    EXPECT_EQ(waterRank(box(0.0f, -1.0f, 0.0f, 0.5f), above, lake, 1), 0);
}

TEST(WaterLayeringTest, NestedPoolOverLake){
    // a pool 2 m above a lake, a fish in the pool, seen from above: the lake first (behind the
    // pool's surface), the fish (behind the pool, in front of the lake), the pool last
    const std::vector<WaterLayer> waters = {layer(1, 0.0f, 0.1f, 20.0f), layer(2, 2.0f, 0.1f, 2.0f)};
    const Vector3 eye(0.0f, 8.0f, 1.0f);
    const AABB lakeBox(Vector3(-20.0f, 0.0f, -20.0f), Vector3(20.0f, 0.0f, 20.0f));
    const AABB poolBox(Vector3(-2.0f, 2.0f, -2.0f), Vector3(2.0f, 2.0f, 2.0f));
    const int lake = waterRank(lakeBox, eye, waters, 1);
    const int pool = waterRank(poolBox, eye, waters, 2);
    const int fish = waterRank(box(0.0f, 1.2f, 0.0f, 0.3f), eye, waters, 3);
    EXPECT_EQ(lake, 1);
    EXPECT_EQ(fish, 0);
    EXPECT_EQ(pool, -1);
    EXPECT_GT(lake, fish);
    EXPECT_GT(fish, pool);
}

TEST(WaterLayeringTest, ClampedAndFinite){
    std::vector<WaterLayer> many;
    for (int i = 0; i < 20; i++)
        many.push_back(layer((Entity)(i + 1), 10.0f + i, 0.0f, 50.0f));
    EXPECT_EQ(waterRank(box(0.0f, 0.0f, 0.0f, 0.5f), Vector3(0.0f, 100.0f, 0.0f), many, 99), 8);
    EXPECT_EQ(waterRank(box(0.0f, 0.0f, 0.0f, 0.5f), Vector3(0.0f, -100.0f, 0.0f), many, 99), -8);
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(waterRank(AABB(Vector3(-inf, -inf, -inf), Vector3(inf, inf, inf)), Vector3(0.0f, 100.0f, 0.0f), many, 99), 0);
}
