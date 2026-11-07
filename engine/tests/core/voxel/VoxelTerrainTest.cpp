// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R38-R46 (AC34, AC35, AC37): an attached volume cuts only where it disagrees with the terrain,
// puts it all back when it goes, and hides the overlap without hiding a cave ceiling.

#include "VoxelTestSupport.h"

#include "Engine.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/TerrainEdit.h"
#include "object/physics/Rigidbody.h"
#include "object/voxel/GroundQuery.h"
#include "pool/TextureDataPool.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/VoxelSystem.h"
#include "../ecs/EcsTestUtil.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyInterface.h"
#include "Jolt/Physics/Collision/CastResult.h"
#include "Jolt/Physics/Collision/NarrowPhaseQuery.h"
#include "Jolt/Physics/Collision/RayCast.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace voxeltest;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    class VoxelTerrain : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        std::shared_ptr<VoxelSystem> system;
        std::shared_ptr<VoxelAsset> asset;
        Entity terrainEntity = NULL_ENTITY;
        std::vector<unsigned char> heights;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;

        void SetUp() override {
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("VoxelSystem", true);
            system = scene->getSystem<VoxelSystem>();

            // A 128 m terrain, flat at 20 m of 40, one metre a texel
            terrainEntity = scene->createEntity();
            scene->addComponent<Transform>(terrainEntity, {});
            Transform& transform = scene->getComponent<Transform>(terrainEntity);
            transform.position = Vector3(64.0f, 0.0f, 64.0f);
            transform.worldPosition = transform.position;
            transform.modelMatrix = Matrix4::translateMatrix(transform.position);
            TerrainComponent terrain;
            terrain.terrainSize = 128.0f;
            terrain.maxHeight = 40.0f;
            heights.assign(128 * 128, 128);
            terrain.heightMap.setData("voxel-terrain-height", TextureData(128, 128, static_cast<unsigned int>(heights.size()), ColorFormat::RED, 1, heights.data()));
            terrain.heightMap.setReleaseDataAfterLoad(false);
            scene->addComponent<TerrainComponent>(terrainEntity, terrain);
        }

        void TearDown() override {
            system.reset();
            scene.reset();
            asset.reset();
            if (physics) {
                Engine::setFixedDeltatime(0.0);
                Engine::setUpdateTime(updateTime);
            }
            // The pool keeps pixels by id, and these die with the fixture
            TextureDataPool::remove("voxel-terrain-height");
        }

        float surfaceHeight() const { return 128.0f / 255.0f * 40.0f; }

        // The terrain gets its heightfield collider, as a scene with physics has it (T1 R79)
        void enablePhysics() {
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            Engine::setFixedDeltatime(DT);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
            terrain.needUpdateTerrain = false;
            terrain.heightMapLoaded = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = BodyType::STATIC;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::HEIGHTFIELD;
            rigidbody.colliders[0].source = Collider3DSource::ENTITY_HEIGHTFIELD;
            scene->addComponent<RigidbodyComponent>(terrainEntity, rigidbody);
        }

        void steps(int count) {
            for (int i = 0; i < count; i++) {
                scene->fixedUpdate(DT);
                scene->update(DT);
            }
        }

        // Where a ray straight down at (x, z) first meets a body; NaN when nothing
        float groundAt(float x, float z) {
            const JPH::RRayCast ray(JPH::RVec3(x, 50.0f, z), JPH::Vec3(0.0f, -100.0f, 0.0f));
            JPH::RayCastResult hit;
            if (!physics->getNarrowPhaseQuery3D().CastRay(ray, hit)) return std::nanf("");
            return 50.0f - 100.0f * hit.mFraction;
        }

        Entity attached(const std::string& name, VoxelTerrainCut cut = VoxelTerrainCut::Automatic, bool collision = false) {
            GeneratorDesc generator;
            generator.kind = "terrain";
            VolumeDesc desc;
            desc.dimensions = Int3{64, 64, 64};
            desc.levels = 2;
            desc.bounds = BoundsMode::Open;
            asset = VoxelAssets::create(name, desc, generator, 1);
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(32.0f, 0.0f, 32.0f);
            transform.worldPosition = transform.position;
            transform.modelMatrix = Matrix4::translateMatrix(transform.position);
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.terrain = terrainEntity;
            component.terrainCut = cut;
            component.collision = collision;
            if (collision) {
                // the seed's collider sits clear of the terrain's, so a ray tells the two apart; a
                // step this small a ball rolls over
                component.overlapDepth = 0.05f;
                // the colliders gather around the cave mouth, world (64, 64)
                component.focus = scene->createEntity();
                scene->addComponent<Transform>(component.focus, {});
                Transform& focus = scene->getComponent<Transform>(component.focus);
                focus.position = Vector3(64.0f, surfaceHeight() + 1.0f, 64.0f);
                focus.worldPosition = focus.position;
                focus.modelMatrix = Matrix4::translateMatrix(focus.position);
            }
            scene->addComponent<VoxelVolumeComponent>(entity, component);
            scene->update(1.0 / 60.0);
            system->finishPendingWork();
            return entity;
        }

        int holeCount() {
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
            if (terrain.holeMap.empty()) return 0;
            TerrainComponentStore store(scene.get(), terrainEntity, terrain);
            const terrain::MapId map{terrain::MapKind::Hole, 0};
            const terrain::MapFormat format = store.format(map);
            std::vector<uint8_t> bytes(static_cast<size_t>(format.bounds.w) * format.bounds.h);
            store.readBytes(map, format.bounds, bytes);
            int count = 0;
            for (uint8_t value : bytes) count += value < 128 ? 1 : 0;
            return count;
        }

        void drain() {
            for (int i = 0; i < 4; i++) {
                system->finishPendingWork();
                scene->update(1.0 / 60.0);
            }
        }
    };

}

TEST_F(VoxelTerrain, AttachDefaults) {
    const Entity owner = attached("mem://attach");
    EXPECT_EQ(holeCount(), 0);
    EXPECT_EQ(scene->getComponent<TerrainComponent>(terrainEntity).holeMapHolds, 1);
    // R35: the seed sits overlapDepth under the terrain
    Store* store = system->getStore(owner);
    ASSERT_NE(store, nullptr);
    EXPECT_NEAR(valueAt(*store, Vec3d{32.0, surfaceHeight(), 32.0}), 0.02, 0.01);
}

TEST_F(VoxelTerrain, CutsOnlyNonFlush) {
    const Entity owner = attached("mem://cuts");
    const EditOp dig = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    const EditResult result = system->edit(owner, std::span<const EditOp>(&dig, 1), EditOptions{});
    ASSERT_TRUE(result.ok);
    EXPECT_GT(result.holesCut, 0);
    drain();
    const int holes = holeCount();
    EXPECT_GT(holes, 60);
    EXPECT_LT(holes, 180);    // about pi * 6^2, plus what the tolerance decides at the rim
    // Far texels stay; and a change smaller than the tolerance stays flush
    const EditOp small = digOp(sphereShape(10.0, surfaceHeight() + 0.9, 10.0, 1.0));
    const EditResult tiny = system->edit(owner, std::span<const EditOp>(&small, 1), EditOptions{});
    ASSERT_TRUE(tiny.ok);
    drain();
    EXPECT_EQ(holeCount(), holes);
}

TEST_F(VoxelTerrain, RemovalRestoresExactly) {
    const Entity owner = attached("mem://removal");
    // A person's hole, outside what the volume touches
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
    {
        TerrainComponentStore store(scene.get(), terrainEntity, terrain);
        terrain::Shape shape;
        shape.type = terrain::ShapeType::Rect;
        shape.min = Vector2(-60.0f, -60.0f);
        shape.max = Vector2(-55.0f, -55.0f);
        terrain::rasterizeRegion(store, shape, terrain::HOLE_VALUE, nullptr);
    }
    const int painted = holeCount();
    ASSERT_GT(painted, 0);
    const EditOp dig = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&dig, 1), EditOptions{}).ok);
    drain();
    EXPECT_GT(holeCount(), painted);
    // Off fills exactly what it cut, and the person's stay
    scene->getComponent<VoxelVolumeComponent>(owner).terrainCut = VoxelTerrainCut::Off;
    drain();
    EXPECT_EQ(holeCount(), painted);
}

TEST_F(VoxelTerrain, FillClosesTheHole) {
    const Entity owner = attached("mem://fill");
    EditOptions options;
    options.captureDelta = true;
    const EditOp dig = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    const EditResult result = system->edit(owner, std::span<const EditOp>(&dig, 1), options);
    ASSERT_TRUE(result.ok);
    drain();
    ASSERT_GT(holeCount(), 0);
    system->applyDelta(owner, result.delta, true);
    drain();
    EXPECT_EQ(holeCount(), 0);
}

TEST_F(VoxelTerrain, OrderHoleAfterMesh) {
    const Entity owner = attached("mem://order");
    const EditOp dig = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&dig, 1), EditOptions{}).ok);
    // R44: nothing has been activated yet, so nothing is cut yet
    EXPECT_EQ(holeCount(), 0);
    drain();
    EXPECT_GT(holeCount(), 0);
}

TEST_F(VoxelTerrain, HoleMapHeldInPlay) {
    const Entity owner = attached("mem://held");
    const EditOp dig = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    EditOptions options;
    options.captureDelta = true;
    const EditResult result = system->edit(owner, std::span<const EditOp>(&dig, 1), options);
    drain();
    ASSERT_GT(holeCount(), 0);
    system->applyDelta(owner, result.delta, true);
    drain();
    // The last fill keeps the map, all solid, while the volume exists
    EXPECT_FALSE(scene->getComponent<TerrainComponent>(terrainEntity).holeMap.empty());
    EXPECT_EQ(holeCount(), 0);
}

TEST_F(VoxelTerrain, CullKeepsCeilingAndRing) {
    const Entity owner = attached("mem://cull");
    // The flush ground far from any cut draws nothing...
    drain();
    EXPECT_EQ(system->getStats(owner).triangles, 0);
    // ...a dig under flush ground keeps its ceiling, and the mouth keeps its ring
    const EditOp cave = digOp(sphereShape(20.0, 8.0, 20.0, 4.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&cave, 1), EditOptions{}).ok);
    drain();
    const int64_t caveTriangles = system->getStats(owner).triangles;
    EXPECT_GT(caveTriangles, 0);
    EXPECT_EQ(holeCount(), 0);    // the ground above is still flush
    const EditOp mouth = digOp(sphereShape(44.0, surfaceHeight(), 44.0, 5.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&mouth, 1), EditOptions{}).ok);
    drain();
    EXPECT_GT(system->getStats(owner).triangles, caveTriangles);
    bool ringAtSurface = false;
    for (const auto& mesh : system->getVisibleMeshes(owner)) {
        for (const MeshVertex& v : mesh->vertices) {
            const double x = v.position[0] / 256.0, y = v.position[1] / 256.0, z = v.position[2] / 256.0;
            const double fromCentre = std::sqrt((x - 44.0) * (x - 44.0) + (z - 44.0) * (z - 44.0));
            if (fromCentre > 5.5 && fromCentre < 8.0 && std::fabs(y - surfaceHeight()) < 0.5) ringAtSurface = true;
        }
    }
    EXPECT_TRUE(ringAtSurface);
}

TEST_F(VoxelTerrain, SeamStep) {
    const Entity owner = attached("mem://seam");
    const EditOp mouth = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&mouth, 1), EditOptions{}).ok);
    drain();
    // Around the mouth, on the ring, the voxel surface stays within overlapDepth + 0.1 voxel of the terrain's
    double worst = 0.0;
    for (const auto& mesh : system->getVisibleMeshes(owner)) {
        for (const MeshVertex& v : mesh->vertices) {
            const double x = v.position[0] / 256.0, y = v.position[1] / 256.0, z = v.position[2] / 256.0;
            const double fromCentre = std::sqrt((x - 32.0) * (x - 32.0) + (z - 32.0) * (z - 32.0));
            if (fromCentre < 7.0 || fromCentre > 9.0) continue;
            worst = std::max(worst, std::fabs(y - (surfaceHeight() - 0.02)));
        }
    }
    EXPECT_LE(worst, 0.02 + 0.1);
}

TEST_F(VoxelTerrain, SampleGroundFallsBackToVoxels) {
    const Entity owner = attached("mem://ground");
    voxel::GroundSample ground = voxel::sampleGround(scene.get(), 40.0f, 40.0f, 60.0f);
    EXPECT_EQ(ground.source, voxel::GroundSource::Terrain);
    EXPECT_NEAR(ground.height, surfaceHeight(), 0.01f);
    const EditOp mouth = digOp(sphereShape(32.0, surfaceHeight(), 32.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&mouth, 1), EditOptions{}).ok);
    drain();
    // World (64, 64) is the volume's (32, 32), now a hole with the bowl under it
    ground = voxel::sampleGround(scene.get(), 64.0f, 64.0f, 60.0f);
    EXPECT_EQ(ground.source, voxel::GroundSource::Voxel);
    EXPECT_NEAR(ground.height, surfaceHeight() - 6.0f, 0.1f);
}

TEST_F(VoxelTerrain, CollisionHoleAfterCollider) {
    // R44: the terrain's collision hole opens only once the voxel collider of that change is live, so
    // a ray down the cave mouth meets one or the other every tick; and a body rolled over the lip
    // neither falls nor stops (T3 R22's rim leaves it up to a collision cell)
    enablePhysics();
    const Entity owner = attached("mem://collider-order", VoxelTerrainCut::Automatic, true);
    steps(30);
    system->finishPendingWork();
    const float surface = surfaceHeight();
    ASSERT_NEAR(groundAt(64.0f, 64.0f), surface, 0.05f);

    // A dish 1 m deep and about 12 m across under world (64, 64)
    const EditOp dish = digOp(sphereShape(32.0, surface + 19.0, 32.0, 20.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&dish, 1), EditOptions{}).ok);
    int openedAt = -1;
    for (int tick = 0; tick < 120; tick++) {
        steps(1);
        // the terrain, or through its hole the new dish's collider: never nothing, never the seed's
        // old collider under an open hole
        const float ground = groundAt(64.0f, 64.0f);
        const bool onTerrain = std::fabs(ground - surface) < 0.02f;
        const bool onDish = std::fabs(ground - (surface - 1.0f)) < 0.15f;
        ASSERT_TRUE(onTerrain || onDish) << "tick " << tick << ": a ray down the cave mouth meets " << ground << " m, the surface is at " << surface;
        if (openedAt < 0 && holeCount() > 0) openedAt = tick;
    }
    ASSERT_GE(openedAt, 0);
    // Through the terrain's hole, the dish floor
    EXPECT_NEAR(groundAt(64.0f, 64.0f), surface - 1.0f, 0.1f);

    const Entity ball = scene->createEntity();
    scene->addComponent<Transform>(ball, {});
    Transform& start = scene->getComponent<Transform>(ball);
    start.position = Vector3(52.0f, surface + 0.5f, 64.0f);
    start.worldPosition = start.position;
    start.modelMatrix = Matrix4::translateMatrix(start.position);
    physics->createRigidbody(ball);
    Rigidbody body(scene.get(), ball);
    body.setType(BodyType::DYNAMIC);
    body.createSphereCollider(0.5f);
    steps(10);
    const JPH::BodyID id = scene->getComponent<RigidbodyComponent>(ball).body;
    physics->getBodyInterface3D().SetLinearVelocity(id, JPH::Vec3(6.0f, 0.0f, 0.0f));
    float lowest = 1e9f;
    bool crossed = false;
    for (int tick = 0; tick < 480 && !crossed; tick++) {
        steps(1);
        const Vector3 at = scene->getComponent<Transform>(ball).worldPosition;
        lowest = std::min(lowest, at.y);
        crossed = at.x > 76.0f;
    }
    const Vector3 end = scene->getComponent<Transform>(ball).worldPosition;
    std::printf("[bench] ball over a cave mouth's lip: lowest centre %.3f m under the surface, ended at x %.2f z %.2f, hole opened %d ticks after the edit\n",
                surface - lowest, end.x, end.z, openedAt);
    EXPECT_TRUE(crossed);
    EXPECT_GT(lowest, surface - 1.0f + 0.5f - 0.1f);
}
