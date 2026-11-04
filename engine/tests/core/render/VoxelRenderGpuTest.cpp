// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R42, R49-R52 (AC36, AC39): a sphere of 4 slots at each tier; a seam between levels with no crack
// and no light leak. glcore matches its own goldens tightly; another backend is held to a cross-backend bound.

#include "Engine.h"
#include "Scene.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelEdit.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::voxel;

namespace {

    CompareOptions backendOptions() {
        CompareOptions options;
        if (std::strcmp(HeadlessContext::getBackendName(), "glcore") != 0) {
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    class VoxelRenderGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::shared_ptr<VoxelAsset> asset;

        void SetUp() override {
            Engine::setPinnedShaderTime(1.0);
            scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.35f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override {
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
        }

        // Four slots in four colours, painted so every kind of vertex set is on the sphere
        Entity paintedSphere(VoxelQuality quality, const std::string& name) {
            GeneratorDesc generator;
            asset = VoxelAssets::create(name, []{
                VolumeDesc desc;
                desc.dimensions = Int3{64, 64, 64};
                desc.levels = 1;
                return desc;
            }(), generator, 4);
            EditOp ops[4];
            ops[0].kind = EditKind::Add;
            ops[0].shape.center = Vec3d{32.0, 32.0, 32.0};
            ops[0].shape.radius = 22.0;
            ops[0].material = 0;
            for (int i = 1; i < 4; i++) {
                ops[i].kind = EditKind::Paint;
                ops[i].shape.center = Vec3d{32.0 + 18.0 * std::cos(i * 2.1), 42.0, 32.0 + 18.0 * std::sin(i * 2.1)};
                ops[i].shape.radius = 12.0;
                ops[i].material = i;
                ops[i].falloff = 0.5;
            }
            EXPECT_TRUE(applyEdit(*asset->store, ops).ok);
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.quality = quality;
            component.collision = false;
            component.focus = camera.getEntity();
            const Vector3 colours[4] = {Vector3(0.8f, 0.2f, 0.2f), Vector3(0.2f, 0.8f, 0.2f), Vector3(0.2f, 0.2f, 0.8f), Vector3(0.9f, 0.8f, 0.2f)};
            for (const Vector3& colour : colours) {
                VoxelMaterialSlot slot;
                slot.name = "slot";
                slot.layer.colorFactor = colour;
                component.materialSlots.push_back(slot);
            }
            scene.addComponent<VoxelVolumeComponent>(entity, component);
            return entity;
        }

        Entity wavyGround(const std::string& name, VoxelQuality quality, int levels) {
            GeneratorDesc generator;
            generator.kind = "solid_ground";
            generator.height = 40.0;
            VolumeDesc desc;
            desc.dimensions = Int3{256, 128, 256};
            desc.levels = levels;
            asset = VoxelAssets::create(name, desc, generator, 1);
            // Mounds across the level seam, so the transition has real geometry to carry
            std::vector<EditOp> ops;
            for (int i = 0; i < 24; i++) {
                EditOp op;
                op.kind = i % 3 == 0 ? EditKind::Dig : EditKind::Add;
                op.shape.center = Vec3d{20.0 + i * 9.0, 40.0, 40.0 + (i * 37) % 180};
                op.shape.radius = 9.0 + (i % 5);
                ops.push_back(op);
            }
            EXPECT_TRUE(applyEdit(*asset->store, ops).ok);
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.quality = quality;
            component.collision = false;
            component.focus = camera.getEntity();
            VoxelMaterialSlot slot;
            slot.name = "ground";
            slot.layer.colorFactor = Vector3(0.45f, 0.4f, 0.3f);
            component.materialSlots.push_back(slot);
            scene.addComponent<VoxelVolumeComponent>(entity, component);
            return entity;
        }

        bool settled(Entity owner) {
            std::shared_ptr<VoxelSystem> system = scene.getSystem<VoxelSystem>();
            RenderSystem* render = scene.getSystem<RenderSystem>().get();
            const bool ready = GpuEnvironment::runFrames([&](){
                return system->isVolumeReady(owner) && render->isAllLoaded();
            }, 600);
            GpuEnvironment::drawFrames(3);
            return ready && scene.getSystem<RenderSystem>()->isAllLoaded();
        }

        void expectGolden(const std::string& name) {
            const CompareResult result = compareTargetWithGolden(name, backendOptions());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }
    };

}

TEST_F(VoxelRenderGpu, TierGoldens) {
    const struct {
        VoxelQuality quality;
        const char* golden;
    } tiers[] = {{VoxelQuality::High, "voxel_tier_high"}, {VoxelQuality::Medium, "voxel_tier_medium"}, {VoxelQuality::Low, "voxel_tier_low"}};
    for (const auto& tier : tiers) {
        const Entity owner = paintedSphere(tier.quality, std::string("mem://gpu-") + tier.golden);
        camera.setPosition(32.0f, 44.0f, 92.0f);
        camera.setTarget(32.0f, 32.0f, 32.0f);
        ASSERT_TRUE(settled(owner)) << tier.golden;
        expectGolden(tier.golden);
        scene.destroyEntity(owner);
        GpuEnvironment::drawFrames(2);
    }
}

TEST_F(VoxelRenderGpu, LodSeamGolden) {
    const Entity owner = wavyGround("mem://gpu-seam", VoxelQuality::High, 3);
    camera.setPosition(60.0f, 90.0f, 60.0f);
    camera.setTarget(120.0f, 40.0f, 120.0f);
    ASSERT_TRUE(settled(owner));
    std::shared_ptr<VoxelSystem> system = scene.getSystem<VoxelSystem>();
    const VoxelStats stats = system->getStats(owner);
    EXPECT_GT(stats.chunks, 8);
    // More than one level is on screen, and the geometry holds
    std::vector<const MeshOut*> raw;
    const auto meshes = system->getVisibleMeshes(owner);
    for (const auto& mesh : meshes) raw.push_back(mesh.get());
    EXPECT_TRUE(auditWatertight(raw).watertight);
    expectGolden("voxel_lod_seam");
}

TEST_F(VoxelRenderGpu, ShadowsNoLeak) {
    sun.setDirection(-0.8f, -0.25f, -0.2f);
    sun.setShadows(true);
    const Entity owner = wavyGround("mem://gpu-shadow", VoxelQuality::High, 3);
    camera.setPosition(60.0f, 90.0f, 60.0f);
    camera.setTarget(120.0f, 40.0f, 120.0f);
    ASSERT_TRUE(settled(owner));
    expectGolden("voxel_shadows");
}
