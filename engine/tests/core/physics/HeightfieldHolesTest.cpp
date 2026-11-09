// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R22, R24: the collider's holes against Jolt's own answer (AC29, AC30): a sample goes when all its
// adjacent squares are open, so its gap never takes drawn ground, and a hole is open deeper than its rim.

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "physics/JoltShapeBuilder.h"
#include "terrain/TerrainHoles.h"
#include "texture/TextureData.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/CastResult.h"
#include "Jolt/Physics/Collision/RayCast.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "Jolt/Physics/Collision/Shape/SubShapeID.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    class HeightfieldHoles : public ::testing::Test {
    protected:
        Scene scene;
        Entity entity = NULL_ENTITY;
        std::vector<uint8_t> heights;
        std::vector<std::vector<uint8_t>> maps;
        int counter = 0;

        void makeTerrain(int mapSize, float size, float level){
            heights.assign(static_cast<size_t>(mapSize) * mapSize, static_cast<uint8_t>(level * 255.0f));
            entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            TerrainComponent terrain;
            terrain.terrainSize = size;
            terrain.maxHeight = 2.0f;
            terrain.heightMap.setData(std::string("heightfield-holes-") + ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                      TextureData(mapSize, mapSize, static_cast<unsigned int>(heights.size()), ColorFormat::RED, 1, heights.data()));
            scene.addComponent<TerrainComponent>(entity, terrain);
        }

        void cut(int side, int x0, int y0, int x1, int y1){
            maps.emplace_back(static_cast<size_t>(side) * side, terraincore::SOLID_VALUE);
            std::vector<uint8_t>& texels = maps.back();
            for (int y = y0; y < y1; y++){
                for (int x = x0; x < x1; x++){
                    texels[static_cast<size_t>(y) * side + x] = terraincore::HOLE_VALUE;
                }
            }
            scene.getComponent<TerrainComponent>(entity).holeMap.setData(
                std::string("heightfield-holes-map-") + ::testing::UnitTest::GetInstance()->current_test_info()->name() + std::to_string(counter++),
                TextureData(side, side, static_cast<unsigned int>(texels.size()), ColorFormat::RED, 1, texels.data()));
        }

        JPH::ShapeRefC build(unsigned int samples, const Vector3& scale = Vector3(1, 1, 1)){
            std::string error;
            JPH::ShapeRefC shape = JoltShapeBuilder::buildHeightFieldShape(&scene, scene.getComponent<TerrainComponent>(entity), scale, samples, &error);
            EXPECT_TRUE(shape) << error;
            return shape;
        }

        static const JPH::HeightFieldShape& field(const JPH::ShapeRefC& shape){
            return *static_cast<const JPH::HeightFieldShape*>(shape.GetPtr());
        }

        // a ray straight down at terrain-local (x, z), in the shape's space (it starts at -size / 2)
        static bool hitAt(const JPH::ShapeRefC& shape, float x, float z){
            JPH::RayCast ray(JPH::Vec3(x, 30.0f, z), JPH::Vec3(0.0f, -60.0f, 0.0f));
            JPH::RayCastResult hit;
            return shape->CastRay(ray, JPH::SubShapeIDCreator(), hit);
        }
    };

}

TEST_F(HeightfieldHoles, HoleLatticeRule){
    // 8 squares a side over 8 texels, so a texel is a square
    constexpr unsigned int SAMPLES = 9;
    makeTerrain(9, 8.0f, 0.5f);
    struct Case {
        int x0, y0, x1, y1;
        std::vector<std::pair<int, int>> gaps;
        const char* name;
    };
    const std::vector<Case> cases = {
        {3, 3, 4, 4, {}, "1x1: no interior sample"},
        {3, 3, 5, 5, {{4, 4}}, "2x2: one sample"},
        {2, 2, 5, 5, {{3, 3}, {4, 3}, {3, 4}, {4, 4}}, "3x3: its four inner samples"},
        {0, 3, 2, 5, {{0, 4}, {1, 4}}, "at an edge a sample has two squares"},
        {0, 0, 1, 1, {{0, 0}}, "at a corner a sample has one square"},
    };
    for (const Case& c : cases){
        cut(8, c.x0, c.y0, c.x1, c.y1);
        JPH::ShapeRefC shape = build(SAMPLES);
        ASSERT_TRUE(shape);
        for (unsigned int y = 0; y < SAMPLES; y++){
            for (unsigned int x = 0; x < SAMPLES; x++){
                const bool expected = std::find(c.gaps.begin(), c.gaps.end(), std::make_pair(static_cast<int>(x), static_cast<int>(y))) != c.gaps.end();
                EXPECT_EQ(field(shape).IsNoCollision(x, y), expected) << c.name << " sample " << x << ", " << y;
            }
        }
    }

    // mirrored in x, the gap mirrors with the surface: sample i sits at -(-half + spacing * i)
    cut(8, 0, 3, 2, 5);
    JPH::ShapeRefC mirrored = build(SAMPLES, Vector3(-1, 1, 1));
    ASSERT_TRUE(mirrored);
    EXPECT_TRUE(field(mirrored).IsNoCollision(8, 4));
    EXPECT_TRUE(field(mirrored).IsNoCollision(7, 4));
    EXPECT_FALSE(field(mirrored).IsNoCollision(0, 4));

    // no hole map: every sample stands
    scene.getComponent<TerrainComponent>(entity).holeMap = Texture();
    JPH::ShapeRefC plain = build(SAMPLES);
    ASSERT_TRUE(plain);
    for (unsigned int y = 0; y < SAMPLES; y++){
        for (unsigned int x = 0; x < SAMPLES; x++){
            EXPECT_FALSE(field(plain).IsNoCollision(x, y));
        }
    }
}

// R24: navmesh walkable area inside drawn ground inside collider surface. 10,000 rays down: no drawn
// ground lacks a hit, and a point of a hole deeper than the rim has none.
TEST_F(HeightfieldHoles, HolesNestInsideDrawn){
    constexpr int MAP = 40;
    constexpr float SIZE = 64.0f;
    makeTerrain(41, SIZE, 0.4f);
    cut(MAP, 10, 8, 27, 30);
    JPH::ShapeRefC shape = build(41);
    ASSERT_TRUE(shape);

    const terraincore::HoleMapView view{maps.back().data(), MAP, MAP};
    const float cell = SIZE / 40.0f;
    const float texel = SIZE / MAP;
    const float rim = 2.0f * std::max(cell, texel);
    int drawnPoints = 0, deepPoints = 0, rimPoints = 0, rimHits = 0;
    std::mt19937 random(11);
    std::uniform_real_distribution<float> coordinate(0.01f, SIZE - 0.01f);
    for (int i = 0; i < 10000; i++){
        const float x = coordinate(random);
        const float z = coordinate(random);
        const bool hole = terraincore::isHole(view, x / SIZE, z / SIZE);
        // the shape's offset is -size / 2: its space is the terrain's
        const bool hit = hitAt(shape, x - SIZE * 0.5f, z - SIZE * 0.5f);
        if (!hole){
            drawnPoints++;
            EXPECT_TRUE(hit) << "drawn ground without a collider at " << x << ", " << z;
            continue;
        }
        bool deep = true;
        for (float dz = -rim; dz <= rim && deep; dz += texel * 0.5f){
            for (float dx = -rim; dx <= rim; dx += texel * 0.5f){
                if (!terraincore::isHole(view, (x + dx) / SIZE, (z + dz) / SIZE)){
                    deep = false;
                    break;
                }
            }
        }
        if (deep){
            deepPoints++;
            EXPECT_FALSE(hit) << "a body would stand deep in the hole at " << x << ", " << z;
        }else{
            rimPoints++;
            rimHits += hit ? 1 : 0;
        }
    }
    EXPECT_GT(drawnPoints, 1000);
    EXPECT_GT(deepPoints, 100);
    EXPECT_GT(rimPoints, 100);
    EXPECT_GT(rimHits, 0) << "the collider stays under a rim of the drawn hole";
}

// the ops' count (R27) is the collider's own rule
TEST_F(HeightfieldHoles, SamplesOpenMatchJolt){
    makeTerrain(33, 32.0f, 0.5f);
    cut(32, 12, 12, 20, 20);
    JPH::ShapeRefC shape = build(33);
    ASSERT_TRUE(shape);
    std::vector<uint8_t> open;
    std::vector<uint8_t> gaps;
    terraincore::openSquares(terraincore::HoleMapView{maps.back().data(), 32, 32}, 33, 1.0f, 1.0f, open);
    const int count = terraincore::noCollisionSamples(open, 33, gaps);
    int jolt = 0;
    for (unsigned int y = 0; y < 33; y++){
        for (unsigned int x = 0; x < 33; x++){
            jolt += field(shape).IsNoCollision(x, y) ? 1 : 0;
        }
    }
    EXPECT_EQ(jolt, count);
    EXPECT_EQ(count, 7 * 7);
    EXPECT_FALSE(hitAt(shape, 0.0f, 0.0f)) << "a ball falls in";
    EXPECT_TRUE(hitAt(shape, 12.0f, 0.0f));
}
