// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R25, R26: what each reader of the surface does over a hole (AC32, AC33). The GPU's half is
// render/TerrainHolesGpuTest.cpp, the collider's editor/tests/engine/physics/JoltShapeBuilderTest.cpp.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "object/TerrainOpsJson.h"
#include "pool/TextureDataPool.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/ParticleSubsystem.h"
#include "terrain/TerrainHoles.h"
#include "terrain/TerrainOps.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int MAP_SIZE = 33;
    constexpr float TERRAIN_SIZE = 64.0f;

    class TerrainHolesConsumers : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        Entity entity = NULL_ENTITY;
        std::vector<uint8_t> heights;
        std::vector<uint8_t> holes;
        std::vector<uint8_t> density;
        std::vector<std::string> ids;

        void SetUp() override{
            scene = std::make_unique<Scene>();
            entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            heights.assign(static_cast<size_t>(MAP_SIZE) * MAP_SIZE, 128);
            TerrainComponent terrain;
            terrain.terrainSize = TERRAIN_SIZE;
            terrain.maxHeight = 8.0f;
            terrain.heightMap.setData(id("height"), TextureData(MAP_SIZE, MAP_SIZE, static_cast<unsigned int>(heights.size()),
                                                                ColorFormat::RED, 1, heights.data()));
            scene->addComponent<TerrainComponent>(entity, terrain);
        }

        void TearDown() override{
            scene.reset();
            for (const std::string& name : ids){
                TextureDataPool::remove(name);
            }
        }

        std::string id(const std::string& name){
            ids.push_back(std::string("terrain-holes-consumers-") + ::testing::UnitTest::GetInstance()->current_test_info()->name() + "-" + name);
            return ids.back();
        }

        TerrainComponent& terrain(){
            return scene->getComponent<TerrainComponent>(entity);
        }

        // the whole half of the terrain with local x < 0 is a hole
        void cutWestHalf(){
            holes.assign(static_cast<size_t>(MAP_SIZE) * MAP_SIZE, terraincore::SOLID_VALUE);
            for (int y = 0; y < MAP_SIZE; y++){
                for (int x = 0; x < MAP_SIZE / 2; x++){
                    holes[static_cast<size_t>(y) * MAP_SIZE + x] = terraincore::HOLE_VALUE;
                }
            }
            terrain().holeMap.setData(id("holes"), TextureData(MAP_SIZE, MAP_SIZE, static_cast<unsigned int>(holes.size()),
                                                               ColorFormat::RED, 1, holes.data()));
        }
    };

}

TEST_F(TerrainHolesConsumers, ParticlesPassThroughAHole){
    ParticleSubsystem* particles = scene->getSystem<ParticleSubsystem>().get();
    auto query = [&](float x, float z, float out[3]){
        const float point[3] = {x, 20.0f, z};
        float normal[3];
        particles->gatherTerrains();
        return ParticleSubsystem::sampleTerrain(particles, point, out, normal);
    };
    float surface[3];
    EXPECT_TRUE(query(-10.0f, 0.0f, surface));
    EXPECT_TRUE(query(10.0f, 0.0f, surface));
    cutWestHalf();
    EXPECT_FALSE(query(-10.0f, 0.0f, surface)) << "over the hole there is no surface";
    EXPECT_TRUE(query(10.0f, 0.0f, surface));
}

TEST_F(TerrainHolesConsumers, HeightStaysDefinedUnderAHole){
    cutWestHalf();
    float height = 0.0f;
    Vector3 normal;
    MeshSystem::sampleTerrainSurface(terrain(), -10.0f, 0.0f, height, normal);
    EXPECT_NEAR(height, 8.0f * 128.0f / 255.0f, 1e-3f);
    EXPECT_TRUE(querySurface(terrain(), -10.0f, 0.0f).hole);
    EXPECT_FALSE(querySurface(terrain(), 10.0f, 0.0f).hole);
}

TEST_F(TerrainHolesConsumers, BrushRaysSeeThroughTheRule){
    cutWestHalf();
    MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();
    Transform& transform = scene->getComponent<Transform>(entity);
    const Ray down(Vector3(-10.0f, 50.0f, 0.0f), Vector3(0.0f, -100.0f, 0.0f));
    Vector3 point;
    EXPECT_FALSE(meshSystem->raycastTerrainSurface(down, terrain(), transform, point)) << "a ray down a hole misses";
    EXPECT_TRUE(meshSystem->raycastTerrainSurface(down, terrain(), transform, point, true)) << "a brush paints in it";
    const Ray solid(Vector3(10.0f, 50.0f, 0.0f), Vector3(0.0f, -100.0f, 0.0f));
    EXPECT_TRUE(meshSystem->raycastTerrainSurface(solid, terrain(), transform, point));
    EXPECT_NEAR(point.y, 8.0f * 128.0f / 255.0f, 1e-3f);
}

// T3 R29: while a volume holds the map, a last fill keeps it, all solid, and no reload follows
TEST_F(TerrainHolesConsumers, HoldKeepsTheMap){
    TerrainComponentStore store(scene.get(), entity, terrain());
    terrain().holeMapHolds = 1;
    terraincore::Op cut;
    cut.type = terraincore::OpType::CutHoles;
    cut.shape.type = terraincore::ShapeType::Circle;
    cut.shape.center = Vector2(0.0f, 0.0f);
    cut.shape.radius = 8.0f;
    ASSERT_TRUE(terraincore::applyOps(store, std::span<const terraincore::Op>(&cut, 1)).ok);
    EXPECT_FALSE(terrain().holeMap.empty());
    terraincore::Op fill;
    fill.type = terraincore::OpType::FillHoles;
    terraincore::TerrainOpResult filled = terraincore::applyOps(store, std::span<const terraincore::Op>(&fill, 1));
    ASSERT_TRUE(filled.ok);
    EXPECT_FALSE(filled.ops[0].mapRemoved);
    EXPECT_FALSE(terrain().holeMap.empty()) << "the map stays, all solid";
    EXPECT_FALSE(querySurface(terrain(), 0.0f, 0.0f).hole);

    terrain().holeMapHolds = 0;
    ASSERT_TRUE(terraincore::applyOps(store, std::span<const terraincore::Op>(&cut, 1)).ok);
    filled = terraincore::applyOps(store, std::span<const terraincore::Op>(&fill, 1));
    ASSERT_TRUE(filled.ok);
    EXPECT_TRUE(filled.ops[0].mapRemoved);
    EXPECT_TRUE(terrain().holeMap.empty());
}

// T3 R27: a hole with no interior sample warns, and collision_samples_open counts what opens
TEST_F(TerrainHolesConsumers, CollisionSamplesOpenAndWarning){
    TerrainComponentStore store(scene.get(), entity, terrain());
    terraincore::Op small;
    small.type = terraincore::OpType::CutHoles;
    small.shape.type = terraincore::ShapeType::Circle;
    small.shape.center = Vector2(0.0f, 0.0f);
    small.shape.radius = 1.0f;
    const terraincore::TerrainOpResult first = terraincore::applyOps(store, std::span<const terraincore::Op>(&small, 1));
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.ops[0].collisionSamplesOpen, 0);
    EXPECT_TRUE(first.hasWarning("hole_no_collision"));

    terraincore::Op large = small;
    large.shape.radius = 16.0f;
    const terraincore::TerrainOpResult second = terraincore::applyOps(store, std::span<const terraincore::Op>(&large, 1));
    ASSERT_TRUE(second.ok);
    EXPECT_GT(second.ops[0].collisionSamplesOpen, 0);
    EXPECT_FALSE(second.hasWarning("hole_no_collision"));

    // edit_terrain and Lua read the count from the result JSON
    const terraincore::OpSpace space = terraincore::OpSpace::of(Matrix4(), Vector3(0.0f), Vector3(1.0f), false, false);
    const nlohmann::json out = terraincore::opResultJson(second, space, std::vector<terraincore::Op>{large});
    ASSERT_TRUE(out["ops"][0].contains("collision_samples_open")) << out.dump();
    EXPECT_EQ(out["ops"][0]["collision_samples_open"].get<int>(), second.ops[0].collisionSamplesOpen);
}
