// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R25 (AC24): navigation collects level 0, whatever the volume draws

#include "VoxelTestSupport.h"

#include "Scene.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace voxeltest;

namespace {

    class VoxelNav : public ::testing::Test {
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
        Entity ground(const std::string& name, const Vector3& focusAt) {
            GeneratorDesc generator;
            generator.kind = "solid_ground";
            generator.height = 32.0;
            asset = VoxelAssets::create(name, desc(128, 64, 128, 3), generator, 1);
            const Entity focus = scene->createEntity();
            scene->addComponent<Transform>(focus, {});
            scene->getComponent<Transform>(focus).worldPosition = focusAt;
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.focus = focus;
            component.collision = false;
            scene->addComponent<VoxelVolumeComponent>(entity, component);
            scene->update(1.0 / 60.0);
            return entity;
        }
    };

}

TEST_F(VoxelNav, CollectsLevelZeroNotDrawn) {
    const AABB bounds(0.0f, 0.0f, 0.0f, 128.0f, 64.0f, 128.0f);
    std::vector<Vector3> nearVertices, farVertices;
    std::vector<uint32_t> nearIndices, farIndices;
    ground("mem://nav-near", Vector3(64.0f, 34.0f, 64.0f));
    system->finishPendingWork();
    system->appendNavTriangles(bounds, nearVertices, nearIndices);
    TearDown();
    SetUp();
    ground("mem://nav-far", Vector3(4.0f, 60.0f, 4.0f));
    system->finishPendingWork();
    system->appendNavTriangles(bounds, farVertices, farIndices);
    ASSERT_GT(nearIndices.size(), 0u);
    ASSERT_EQ(nearIndices.size(), farIndices.size());
    ASSERT_EQ(nearVertices.size(), farVertices.size());
    for (size_t i = 0; i < nearVertices.size(); i++) {
        EXPECT_EQ(nearVertices[i].x, farVertices[i].x);
        EXPECT_EQ(nearVertices[i].y, farVertices[i].y);
        EXPECT_EQ(nearVertices[i].z, farVertices[i].z);
    }
    // Level-0 cells: a triangle's edge is at most one voxel's diagonal
    for (size_t i = 0; i + 2 < nearIndices.size(); i += 3) {
        const Vector3 d = nearVertices[nearIndices[i]] - nearVertices[nearIndices[i + 1]];
        EXPECT_LE(d.length(), 1.8f);
    }
}
