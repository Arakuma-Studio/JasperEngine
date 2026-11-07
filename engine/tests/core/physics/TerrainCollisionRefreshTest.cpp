// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R79, AC34: an edit reaches the terrain's collider before the next step, in place through
// SetHeights where it can, with the build's own samples, and what rests on it wakes.

#include "Engine.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "physics/JoltShapeBuilder.h"
#include "physics/JoltTerrainRefresh.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "terrain/TerrainOps.h"
#include "texture/TextureData.h"
#include "../ecs/EcsTestUtil.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyInterface.h"
#include "Jolt/Physics/Collision/CastResult.h"
#include "Jolt/Physics/Collision/NarrowPhaseQuery.h"
#include "Jolt/Physics/Collision/RayCast.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;
    constexpr int MAP = 33;
    constexpr float SIZE = 32.0f;
    constexpr float MAX_HEIGHT = 10.0f;

    class TerrainCollisionRefresh : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        Entity terrainEntity = NULL_ENTITY;
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        // A flat 16-bit map at half the terrain's height, with or without a body on it
        TerrainComponent& makeTerrain(bool runtimeEdits, bool withBody){
            terrainEntity = test::makeTransformEntity(*scene);
            TerrainComponent terrain;
            terrain.terrainSize = SIZE;
            terrain.maxHeight = MAX_HEIGHT;
            terrain.allowRuntimeEdits = runtimeEdits;
            const size_t bytes = static_cast<size_t>(MAP) * MAP * 2;
            unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
            for (size_t i = 0; i < bytes; i += 2){
                raw[i] = 0xFF;
                raw[i + 1] = 0x7F;
            }
            TextureData data(MAP, MAP, static_cast<unsigned int>(bytes), ColorFormat::RED16, 1, raw);
            data.setDataOwned(false);
            terrain.heightMap.setData(std::string("terrain-collision-refresh-") + ::testing::UnitTest::GetInstance()->current_test_info()->name(), data);
            terrain.heightMap.getData().setDataOwned(true);
            // built: the collider reads the map, not a node tree this scene has no system to make
            terrain.needUpdateTerrain = false;
            terrain.heightMapLoaded = true;
            scene->addComponent<TerrainComponent>(terrainEntity, terrain);

            if (withBody){
                RigidbodyComponent rigidbody;
                rigidbody.type = BodyType::STATIC;
                rigidbody.colliders.resize(1);
                rigidbody.colliders[0].type = Collider3DType::HEIGHTFIELD;
                rigidbody.colliders[0].source = Collider3DSource::ENTITY_HEIGHTFIELD;
                scene->addComponent<RigidbodyComponent>(terrainEntity, rigidbody);
            }
            return scene->getComponent<TerrainComponent>(terrainEntity);
        }

        // An edit as a game's applyOps makes it: through the component's store
        terrain::TerrainOpResult edit(terrain::OpType type, float radius, float delta){
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
            TerrainComponentStore store(scene.get(), terrainEntity, terrain);
            terrain::Op op;
            op.type = type;
            op.shape.type = terrain::ShapeType::Circle;
            op.shape.center = Vector2(0.0f, 0.0f);
            op.shape.radius = radius;
            op.delta = delta;
            return terrain::applyOps(store, std::span<const terrain::Op>(&op, 1));
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate){
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                }
            }
        }

        void steps(int count){
            for (int i = 0; i < count; i++){
                resolveTransforms();
                scene->fixedUpdate(DT);
                resolveTransforms();
            }
        }

        // Where a ray straight down at (x, z) meets the world's bodies; NaN when nothing
        float groundAt(float x, float z){
            const JPH::RRayCast ray(JPH::RVec3(x, 50.0f, z), JPH::Vec3(0.0f, -100.0f, 0.0f));
            JPH::RayCastResult hit;
            if (!physics->getNarrowPhaseQuery3D().CastRay(ray, hit)){
                return std::nanf("");
            }
            return 50.0f - 100.0f * hit.mFraction;
        }

        static JPH::HeightFieldShape& field(const JPH::ShapeRefC& shape){
            return const_cast<JPH::HeightFieldShape&>(static_cast<const JPH::HeightFieldShape&>(*shape));
        }

        static std::vector<float> heightsOf(const JPH::HeightFieldShape& shape){
            const unsigned int count = shape.GetSampleCount();
            std::vector<float> heights(static_cast<size_t>(count) * count);
            shape.GetHeights(0, 0, count, count, heights.data(), static_cast<intptr_t>(count));
            return heights;
        }
    };

}

TEST(TerrainCollisionBlock, CoversTheEditGrownByACellInWholeBlocks){
    // 33 samples one unit apart over [-16, 16], padded to 34 by blocks of 2
    const HeightFieldBlock block = JoltTerrainRefresh::blockOver(SIZE, 33, 34, 2, -1.5f, -1.5f, 1.5f, 1.5f, 1.0f, 1.0f);
    // samples 14.5..17.5 reach 14..18; grown by one, 13..19; in blocks, 12..19
    EXPECT_EQ(block.x, 12u);
    EXPECT_EQ(block.w, 8u);
    EXPECT_EQ(block.y, 12u);
    EXPECT_EQ(block.h, 8u);

    // the far edge takes the padding block with it
    const HeightFieldBlock edge = JoltTerrainRefresh::blockOver(SIZE, 33, 34, 2, 15.0f, -16.0f, 16.0f, -15.0f, 1.0f, 1.0f);
    EXPECT_EQ(edge.x + edge.w, 34u);
    EXPECT_EQ(edge.y, 0u);

    // a mirrored axis flips the samples
    const HeightFieldBlock mirrored = JoltTerrainRefresh::blockOver(SIZE, 33, 34, 2, 15.0f, 0.0f, 16.0f, 0.5f, -1.0f, 1.0f);
    EXPECT_EQ(mirrored.x, 0u);

    // past the terrain: nothing
    EXPECT_TRUE(JoltTerrainRefresh::blockOver(SIZE, 33, 34, 2, 40.0f, 40.0f, 50.0f, 50.0f, 1.0f, 1.0f).empty());
}

TEST_F(TerrainCollisionRefresh, SetHeightsMatchesRebuild){
    TerrainComponent& terrain = makeTerrain(true, false);
    std::string error;
    JPH::ShapeRefC shape = JoltShapeBuilder::buildHeightFieldShape(scene.get(), terrain, Vector3(1, 1, 1), MAP, &error);
    ASSERT_TRUE(shape) << error;
    const std::vector<float> before = heightsOf(field(shape));

    ASSERT_TRUE(edit(terrain::OpType::Lower, 4.0f, 3.0f).ok);
    ASSERT_TRUE(terrain.editState && terrain.editState->collisionDirty) << "the edit is waiting for the colliders";
    const TerrainEditState& state = *terrain.editState;
    const HeightFieldBlock block = JoltTerrainRefresh::blockOver(SIZE, MAP, field(shape).GetSampleCount(), field(shape).GetBlockSize(),
                                                                 state.collisionMinX, state.collisionMinZ, state.collisionMaxX, state.collisionMaxZ, 1.0f, 1.0f);
    ASSERT_FALSE(block.empty());
    ASSERT_TRUE(JoltTerrainRefresh::refreshShape(scene.get(), terrain, Vector3(1, 1, 1), MAP, field(shape), block,
                                                 JoltShapeBuilder::sharedPhysicsTempAllocator()));

    JPH::ShapeRefC fresh = JoltShapeBuilder::buildHeightFieldShape(scene.get(), terrain, Vector3(1, 1, 1), MAP, &error);
    ASSERT_TRUE(fresh) << error;
    const std::vector<float> refreshed = heightsOf(field(shape));
    const std::vector<float> rebuilt = heightsOf(field(fresh));
    ASSERT_EQ(refreshed.size(), rebuilt.size());
    // within what 8 bits per sample of a block can say about a 10 unit range
    const float quantum = MAX_HEIGHT / 255.0f;
    for (size_t i = 0; i < rebuilt.size(); i++){
        if (rebuilt[i] == JPH::HeightFieldShapeConstants::cNoCollisionValue){
            EXPECT_EQ(refreshed[i], rebuilt[i]) << i;
            continue;
        }
        EXPECT_NEAR(refreshed[i], rebuilt[i], quantum) << i;
    }
    const size_t centre = static_cast<size_t>(16) * field(shape).GetSampleCount() + 16;
    EXPECT_NEAR(refreshed[centre], 2.0f, quantum) << "the crater";
    EXPECT_NEAR(before[centre], 5.0f, quantum) << "and the ground before it";
}

TEST_F(TerrainCollisionRefresh, AHeightPastTheRangeOrMostOfTheShapeIsBuiltAgain){
    // without Allow Runtime Edits the shape holds only the heights it was built with
    TerrainComponent& terrain = makeTerrain(false, false);
    std::string error;
    JPH::ShapeRefC shape = JoltShapeBuilder::buildHeightFieldShape(scene.get(), terrain, Vector3(1, 1, 1), MAP, &error);
    ASSERT_TRUE(shape) << error;
    ASSERT_TRUE(edit(terrain::OpType::Raise, 3.0f, 3.0f).ok);
    const TerrainEditState& state = *terrain.editState;
    const HeightFieldBlock block = JoltTerrainRefresh::blockOver(SIZE, MAP, field(shape).GetSampleCount(), field(shape).GetBlockSize(),
                                                                 state.collisionMinX, state.collisionMinZ, state.collisionMaxX, state.collisionMaxZ, 1.0f, 1.0f);
    EXPECT_FALSE(JoltTerrainRefresh::refreshShape(scene.get(), terrain, Vector3(1, 1, 1), MAP, field(shape), block,
                                                  JoltShapeBuilder::sharedPhysicsTempAllocator()));

    // reserved, the same raise goes in place; a block of the whole shape does not
    TerrainComponent& reserved = makeTerrain(true, false);
    JPH::ShapeRefC roomy = JoltShapeBuilder::buildHeightFieldShape(scene.get(), reserved, Vector3(1, 1, 1), MAP, &error);
    ASSERT_TRUE(roomy) << error;
    EXPECT_TRUE(JoltTerrainRefresh::refreshShape(scene.get(), reserved, Vector3(1, 1, 1), MAP, field(roomy), block,
                                                 JoltShapeBuilder::sharedPhysicsTempAllocator()));
    const HeightFieldBlock whole{0, 0, field(roomy).GetSampleCount(), field(roomy).GetSampleCount()};
    EXPECT_FALSE(JoltTerrainRefresh::refreshShape(scene.get(), reserved, Vector3(1, 1, 1), MAP, field(roomy), whole,
                                                  JoltShapeBuilder::sharedPhysicsTempAllocator()));
}

TEST_F(TerrainCollisionRefresh, RaysHitTheNewSurface){
    makeTerrain(true, true);
    steps(1);
    const RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(terrainEntity);
    ASSERT_FALSE(body.body.IsInvalid()) << "the heightfield body was made";
    const JPH::Shape* built = body.colliders[0].shape.GetPtr();
    EXPECT_NEAR(groundAt(0.0f, 0.0f), 5.0f, 0.05f);

    ASSERT_TRUE(edit(terrain::OpType::Lower, 4.0f, 3.0f).ok);
    steps(1);
    EXPECT_NEAR(groundAt(0.0f, 0.0f), 2.0f, 0.05f) << "the crater's floor";
    EXPECT_NEAR(groundAt(10.0f, 10.0f), 5.0f, 0.05f) << "the ground away from it";
    EXPECT_EQ(scene->getComponent<RigidbodyComponent>(terrainEntity).colliders[0].shape.GetPtr(), built) << "in place, not built again";
    EXPECT_FALSE(scene->getComponent<TerrainComponent>(terrainEntity).editState->collisionDirty);
}

TEST_F(TerrainCollisionRefresh, AnEditPastTheRangeRebuildsAndStillHits){
    makeTerrain(false, true);
    steps(1);
    ASSERT_TRUE(edit(terrain::OpType::Raise, 4.0f, 3.0f).ok);
    steps(1);
    EXPECT_NEAR(groundAt(0.0f, 0.0f), 8.0f, 0.05f);
}

TEST_F(TerrainCollisionRefresh, SleepingBodyWakes){
    makeTerrain(true, true);
    const Entity crate = test::makeTransformEntity(*scene);
    Transform& transform = scene->getComponent<Transform>(crate);
    transform.position = Vector3(0.0f, 6.0f, 0.0f);
    transform.needUpdate = true;
    RigidbodyComponent rigidbody;
    rigidbody.type = BodyType::DYNAMIC;
    rigidbody.colliders.resize(1);
    rigidbody.colliders[0].type = Collider3DType::BOX;
    rigidbody.colliders[0].width = 1.0f;
    rigidbody.colliders[0].height = 1.0f;
    rigidbody.colliders[0].depth = 1.0f;
    scene->addComponent<RigidbodyComponent>(crate, rigidbody);

    JPH::BodyInterface& bodies = physics->getBodyInterface3D();
    for (int i = 0; i < 600; i++){
        steps(1);
        if (!scene->getComponent<RigidbodyComponent>(crate).body.IsInvalid() &&
            !bodies.IsActive(scene->getComponent<RigidbodyComponent>(crate).body)){
            break;
        }
    }
    const JPH::BodyID id = scene->getComponent<RigidbodyComponent>(crate).body;
    ASSERT_FALSE(bodies.IsActive(id)) << "the crate went to sleep on the ground";
    const float resting = static_cast<float>(bodies.GetPosition(id).GetY());
    EXPECT_NEAR(resting, 5.5f, 0.1f);

    ASSERT_TRUE(edit(terrain::OpType::Lower, 6.0f, 3.0f).ok);
    steps(1);
    EXPECT_TRUE(bodies.IsActive(id)) << "it wakes when the ground under it moves";
    steps(120);
    EXPECT_LT(static_cast<float>(bodies.GetPosition(id).GetY()), resting - 2.0f) << "and falls into the crater";
}

TEST_F(TerrainCollisionRefresh, NoEditNoWork){
    makeTerrain(true, true);
    steps(1);
    const JPH::Shape* built = scene->getComponent<RigidbodyComponent>(terrainEntity).colliders[0].shape.GetPtr();
    steps(10);
    EXPECT_FALSE(scene->getComponent<TerrainComponent>(terrainEntity).editState) << "an unedited terrain has no edit state";
    EXPECT_EQ(scene->getComponent<RigidbodyComponent>(terrainEntity).colliders[0].shape.GetPtr(), built);
}
