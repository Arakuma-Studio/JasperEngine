// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R16-R20, R58: chunks, budgets and what a scene with no volume pays

#include "VoxelTestSupport.h"

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "support/AllocationCounter.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace voxeltest;

namespace {

    class VoxelSystemFixture : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        std::shared_ptr<VoxelSystem> system;
        std::shared_ptr<VoxelAsset> asset;

        void SetUp() override {
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("VoxelSystem", true);
            system = scene->getSystem<VoxelSystem>();
        }
        void TearDown() override {
            system.reset();
            scene.reset();
            asset.reset();
        }

        Entity volume(const VolumeDesc& desc, const std::string& name, VoxelQuality quality = VoxelQuality::High) {
            GeneratorDesc generator;
            generator.kind = "empty";
            asset = VoxelAssets::create(name, desc, generator, 1);
            asset->store->setBase(wavyBase(desc.dimensions.y * 0.5));
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.quality = quality;
            component.collision = false;
            scene->addComponent<VoxelVolumeComponent>(entity, component);
            return entity;
        }
    };

}

TEST_F(VoxelSystemFixture, UploadBudgetPerFrame) {
    const Entity owner = volume(desc(256, 128, 256, 3), "mem://upload", VoxelQuality::Low);
    const VoxelTierSettings low = voxelTierSettings(VoxelTier::Low);
    bool hitMeshCap = false;
    for (int frame = 0; frame < 2000 && !system->isVolumeReady(owner); frame++) {
        scene->update(1.0 / 60.0);
        const VoxelStats stats = system->getStats(owner);
        EXPECT_LE(stats.uploadMeshesThisFrame, low.uploadMeshesPerFrame);
        // One mesh always goes, however big; beyond it the bytes hold
        if (stats.uploadMeshesThisFrame > 1) {
            EXPECT_LE(stats.uploadBytesThisFrame, low.uploadBytesPerFrame);
        }
        hitMeshCap = hitMeshCap || stats.uploadMeshesThisFrame == low.uploadMeshesPerFrame;
    }
    EXPECT_TRUE(system->isVolumeReady(owner));
    EXPECT_TRUE(hitMeshCap);
}

TEST_F(VoxelSystemFixture, FirstSetBlocksIsAllLoaded) {
    const Entity owner = volume(desc(128, 64, 128, 3), "mem://first");
    int reported = 0;
    system->forEachLoading([&](Entity) { reported++; });
    EXPECT_EQ(reported, 1);
    EXPECT_FALSE(system->isVolumeReady(owner));
    system->finishPendingWork();
    scene->update(1.0 / 60.0);
    system->finishPendingWork();
    EXPECT_TRUE(system->isVolumeReady(owner));
    reported = 0;
    system->forEachLoading([&](Entity) { reported++; });
    EXPECT_EQ(reported, 0);
}

TEST_F(VoxelSystemFixture, ChunksAreOwnedAndNotInHierarchy) {
    const Entity owner = volume(desc(128, 64, 128, 3), "mem://owned");
    scene->update(1.0 / 60.0);
    system->finishPendingWork();
    const std::vector<Entity> chunks = system->getGeneratedEntities(owner);
    ASSERT_FALSE(chunks.empty());
    int visible = 0;
    for (Entity chunk : chunks) {
        EXPECT_EQ(system->getGeneratedOwner(chunk), owner);
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(chunk);
        EXPECT_EQ(mesh.voxelOwner, owner);
        EXPECT_EQ(mesh.voxelTier, static_cast<uint8_t>(VoxelTier::High));
        EXPECT_EQ(mesh.buffer.getStride(), 32u);
        const Transform& transform = scene->getComponent<Transform>(chunk);
        EXPECT_EQ(transform.parent, NULL_ENTITY);
        if (transform.visible) visible++;
    }
    EXPECT_GT(visible, 0);
    // The on-screen meshes hold together
    const auto meshes = system->getVisibleMeshes(owner);
    std::vector<const MeshOut*> raw;
    for (const auto& mesh : meshes) raw.push_back(mesh.get());
    EXPECT_TRUE(auditWatertight(raw).watertight);
}

TEST_F(VoxelSystemFixture, DisablingRemovesChunksAndReenablingRebuilds) {
    const Entity owner = volume(desc(64, 64, 64, 2), "mem://disable");
    scene->update(1.0 / 60.0);
    system->finishPendingWork();
    ASSERT_FALSE(system->getGeneratedEntities(owner).empty());
    scene->getComponent<VoxelVolumeComponent>(owner).enabled = false;
    scene->update(1.0 / 60.0);
    EXPECT_TRUE(system->getGeneratedEntities(owner).empty());
    scene->getComponent<VoxelVolumeComponent>(owner).enabled = true;
    scene->update(1.0 / 60.0);
    system->finishPendingWork();
    EXPECT_FALSE(system->getGeneratedEntities(owner).empty());
}

TEST_F(VoxelSystemFixture, EditReachesTheScreenAndTheEventFires) {
    const Entity owner = volume(desc(64, 64, 64, 2), "mem://edit");
    scene->update(1.0 / 60.0);
    system->finishPendingWork();
    const auto before = system->getStats(owner);
    int events = 0;
    VoxelEditEvent seen;
    scene->getComponent<VoxelVolumeComponent>(owner).onEdited.add("test", [&events, &seen](VoxelEditEvent event) {
        events++;
        seen = std::move(event);
    });
    const EditOp op = digOp(sphereShape(32.0, 32.0, 32.0, 6.0));
    const EditResult result = system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{});
    ASSERT_TRUE(result.ok);
    scene->update(1.0 / 60.0);
    EXPECT_EQ(events, 1);
    EXPECT_GT(seen.removedVolume, 0.0f);
    system->finishPendingWork();
    const auto after = system->getStats(owner);
    EXPECT_NE(before.triangles, after.triangles);
    std::vector<const MeshOut*> raw;
    const auto meshes = system->getVisibleMeshes(owner);
    for (const auto& mesh : meshes) raw.push_back(mesh.get());
    EXPECT_TRUE(auditWatertight(raw).watertight);
}

TEST_F(VoxelSystemFixture, PlayCopiesTheStore) {
    // Unpaused here is Play: the system's store is a clone
    const Entity owner = volume(desc(64, 64, 64, 2), "mem://play");
    scene->update(1.0 / 60.0);
    ASSERT_NE(system->getStore(owner), nullptr);
    EXPECT_NE(system->getStore(owner), asset->store.get());
    const uint64_t original = contentHash(*asset->store);
    const EditOp op = digOp(sphereShape(32.0, 32.0, 32.0, 6.0));
    ASSERT_TRUE(system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{}).ok);
    EXPECT_NE(contentHash(*system->getStore(owner)), original);
    EXPECT_EQ(contentHash(*asset->store), original);
    // R30: revert goes back to the loaded state
    system->revertChanges(owner);
    EXPECT_EQ(contentHash(*system->getStore(owner)), original);
    // Edit mode shares it
    system->setPaused(true);
    scene->update(1.0 / 60.0);
    EXPECT_EQ(system->getStore(owner), asset->store.get());
}

TEST_F(VoxelSystemFixture, NoComponentNoCost) {
    EXPECT_EQ(system->getGeneratedEntities(NULL_ENTITY).size(), 0u);
    scene->update(1.0 / 60.0);
    testsupport::AllocationCountScope scope;
    for (int i = 0; i < 100; i++) {
        system->update(1.0 / 60.0);
        system->fixedUpdate(1.0 / 60.0);
    }
    EXPECT_EQ(scope.count(), 0u);
}

TEST_F(VoxelSystemFixture, FailedLoadSurfacesTheErrorAndGoesOn) {
    const Entity entity = scene->createEntity();
    scene->addComponent<Transform>(entity, {});
    VoxelVolumeComponent component;
    component.volumeData.path = "does/not/exist.voxelvolume";
    component.collision = false;
    scene->addComponent<VoxelVolumeComponent>(entity, component);
    scene->update(1.0 / 60.0);
    EXPECT_FALSE(system->getError(entity).empty());
    EXPECT_EQ(system->getStore(entity), nullptr);
    int reported = 0;
    system->forEachLoading([&](Entity) { reported++; });
    EXPECT_EQ(reported, 0);
}
