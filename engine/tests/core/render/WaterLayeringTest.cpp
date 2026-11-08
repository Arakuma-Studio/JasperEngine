// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// render/WaterLayering.h: the rank the transparent queue layers draws by around the waters
// (docs/superpowers/specs/2026-11-04-water-reflections-design.md 6.5)

#include "render/WaterLayering.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    WaterLayer rankLayer(Entity entity, float level, float band, float halfSize, float x = 0.0f, float z = 0.0f){
        WaterLayer water;
        water.entity = entity;
        water.level = level;
        water.band = band;
        water.minXZ = Vector2(x - halfSize, z - halfSize);
        water.maxXZ = Vector2(x + halfSize, z + halfSize);
        return water;
    }

    AABB rankBox(float x, float y, float z, float half){
        return AABB(Vector3(x - half, y - half, z - half), Vector3(x + half, y + half, z + half));
    }

}

TEST(WaterLayering, Rank){
    const std::vector<WaterLayer> lake = {rankLayer(1, 0.0f, 0.2f, 20.0f)};
    const Vector3 above(0.0f, 5.0f, 8.0f);
    const Vector3 under(0.0f, -3.0f, 8.0f);

    // seen from above: under the surface is behind it, over it in front
    EXPECT_EQ(waterRank(rankBox(0.0f, -1.0f, 0.0f, 0.5f), above, lake, 99), 1);
    EXPECT_EQ(waterRank(rankBox(0.0f, 2.0f, 0.0f, 0.5f), above, lake, 99), -1);
    // seen from below: the other way round
    EXPECT_EQ(waterRank(rankBox(0.0f, 2.0f, 0.0f, 0.5f), under, lake, 99), 1);
    EXPECT_EQ(waterRank(rankBox(0.0f, -1.0f, 0.0f, 0.5f), under, lake, 99), -1);
    // crossing the wave band, or outside the area: not layered by it
    EXPECT_EQ(waterRank(rankBox(0.0f, 0.1f, 0.0f, 0.5f), above, lake, 99), 0);
    EXPECT_EQ(waterRank(rankBox(0.0f, -0.6f, 0.0f, 0.5f), above, lake, 99), 0);
    EXPECT_EQ(waterRank(rankBox(30.0f, -1.0f, 0.0f, 0.5f), above, lake, 99), 0);
    // a box overlapping the area's edge counts
    EXPECT_EQ(waterRank(rankBox(20.3f, -1.0f, 0.0f, 0.5f), above, lake, 99), 1);
    // nothing to rank by
    EXPECT_EQ(waterRank(AABB(), above, lake, 99), 0);
    EXPECT_EQ(waterRank(rankBox(0.0f, -1.0f, 0.0f, 0.5f), above, {}, 99), 0);
    // a water does not rank itself
    EXPECT_EQ(waterRank(rankBox(0.0f, -1.0f, 0.0f, 0.5f), above, lake, 1), 0);
}

TEST(WaterLayering, NestedPoolOverLake){
    // a pool 2 m above a lake, a fish in the pool, seen from above: the lake first (behind the
    // pool's surface), the fish (behind the pool, in front of the lake), the pool last
    const std::vector<WaterLayer> waters = {rankLayer(1, 0.0f, 0.1f, 20.0f), rankLayer(2, 2.0f, 0.1f, 2.0f)};
    const Vector3 eye(0.0f, 8.0f, 1.0f);
    const AABB lakeBox(Vector3(-20.0f, 0.0f, -20.0f), Vector3(20.0f, 0.0f, 20.0f));
    const AABB poolBox(Vector3(-2.0f, 2.0f, -2.0f), Vector3(2.0f, 2.0f, 2.0f));
    const int lake = waterRank(lakeBox, eye, waters, 1);
    const int pool = waterRank(poolBox, eye, waters, 2);
    const int fish = waterRank(rankBox(0.0f, 1.2f, 0.0f, 0.3f), eye, waters, 3);
    EXPECT_EQ(lake, 1);
    EXPECT_EQ(fish, 0);
    EXPECT_EQ(pool, -1);
    EXPECT_GT(lake, fish);
    EXPECT_GT(fish, pool);
}

TEST(WaterLayering, ClampedAndFinite){
    std::vector<WaterLayer> many;
    for (int i = 0; i < 20; i++)
        many.push_back(rankLayer((Entity)(i + 1), 10.0f + i, 0.0f, 50.0f));
    EXPECT_EQ(waterRank(rankBox(0.0f, 0.0f, 0.0f, 0.5f), Vector3(0.0f, 100.0f, 0.0f), many, 99), 8);
    EXPECT_EQ(waterRank(rankBox(0.0f, 0.0f, 0.0f, 0.5f), Vector3(0.0f, -100.0f, 0.0f), many, 99), -8);
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(waterRank(AABB(Vector3(-inf, -inf, -inf), Vector3(inf, inf, inf)), Vector3(0.0f, 100.0f, 0.0f), many, 99), 0);
}

namespace {

    struct SortedDraw{
        const char* name;
        AABB box;
        Entity entity;
        float distance;
        WaterPlace place;
    };

    // the draws in the transparent queue's order around the waters (RenderSystem's comparator)
    std::vector<std::string> waterQueueOrder(std::vector<SortedDraw> draws, const Vector3& eye, const std::vector<WaterLayer>& waters){
        const std::vector<float> groups = waterGroupDistances(waters);
        for (SortedDraw& draw : draws)
            draw.place = waterPlace(draw.box, eye, waters, groups, draw.entity, draw.distance);
        std::stable_sort(draws.begin(), draws.end(), [](const SortedDraw& a, const SortedDraw& b){
            return waterSortOrder(a.place.sortDistance, a.place.rank, a.distance, b.place.sortDistance, b.place.rank, b.distance) < 0;
        });
        std::vector<std::string> names;
        for (const SortedDraw& draw : draws)
            names.push_back(draw.name);
        return names;
    }

}

// smoke over a far lake sorts with the lake, not against a near window that is over no water;
// a fish under the lake draws before the lake
TEST(WaterLayering, FarLakeNearWindow){
    const Vector3 eye(0.0f, 5.0f, 0.0f);
    std::vector<WaterLayer> waters = {rankLayer(1, 0.0f, 0.1f, 10.0f, 0.0f, -100.0f)};
    waters[0].distance = 100.0f;
    const AABB lakeBox(Vector3(-10.0f, 0.0f, -110.0f), Vector3(10.0f, 0.0f, -90.0f));
    const std::vector<SortedDraw> draws = {
        {"window", rankBox(0.0f, 2.0f, -5.0f, 0.5f), 2, 5.4f, {}},
        {"smoke", rankBox(0.0f, 2.0f, -98.0f, 0.5f), 3, 98.1f, {}},
        {"lake", lakeBox, 1, 100.0f, {}},
        {"fish", rankBox(0.0f, -2.0f, -95.0f, 0.5f), 4, 95.2f, {}},
    };
    const std::vector<std::string> expected = {"fish", "lake", "smoke", "window"};
    EXPECT_EQ(waterQueueOrder(draws, eye, waters), expected);
}

// a pool over a lake: one group, sorted where the farther of them is, by rank inside it
TEST(WaterLayering, OverlappingWatersShareAGroup){
    std::vector<WaterLayer> waters = {rankLayer(1, 0.0f, 0.1f, 20.0f), rankLayer(2, 2.0f, 0.1f, 2.0f), rankLayer(3, 0.0f, 0.1f, 2.0f, 100.0f)};
    waters[0].distance = 30.0f;
    waters[1].distance = 12.0f;
    waters[2].distance = 90.0f;
    const std::vector<float> groups = waterGroupDistances(waters);
    EXPECT_FLOAT_EQ(groups[0], 30.0f);
    EXPECT_FLOAT_EQ(groups[1], 30.0f);
    EXPECT_FLOAT_EQ(groups[2], 90.0f) << "a water far from the others is a group of its own";
    const Vector3 eye(0.0f, 8.0f, 1.0f);
    const WaterPlace fish = waterPlace(rankBox(0.0f, 1.2f, 0.0f, 0.3f), eye, waters, groups, 9, 11.0f);
    EXPECT_FLOAT_EQ(fish.sortDistance, 30.0f);
    const WaterPlace bird = waterPlace(rankBox(50.0f, 5.0f, 0.0f, 0.3f), eye, waters, groups, 10, 40.0f);
    EXPECT_EQ(bird.rank, 0);
    EXPECT_FLOAT_EQ(bird.sortDistance, 40.0f) << "over no water: its own distance";
}

// a sloped river makes no layer (it sorts by its distance alone); a flat lake does
TEST(WaterLayering, SlopedRiverIsNotLayered){
    WaterWaveParams waves;
    WaterLayer layer;
    const AABB river(Vector3(-2.0f, 0.0f, -20.0f), Vector3(2.0f, 3.0f, 20.0f));
    EXPECT_FALSE(isFlatWater(river));
    EXPECT_FALSE(makeWaterLayer(1, river, waves, 10.0f, layer));
    const AABB lake(Vector3(-10.0f, 1.0f, -10.0f), Vector3(10.0f, 1.0f, 10.0f));
    ASSERT_TRUE(makeWaterLayer(2, lake, waves, 12.0f, layer));
    EXPECT_EQ(layer.entity, 2u);
    EXPECT_FLOAT_EQ(layer.level, 1.0f);
    EXPECT_FLOAT_EQ(layer.distance, 12.0f);
    // a gentle slope within 1% of its size plus 5 cm still counts as flat
    EXPECT_TRUE(isFlatWater(AABB(Vector3(-10.0f, 0.0f, -10.0f), Vector3(10.0f, 0.2f, 10.0f))));
}
