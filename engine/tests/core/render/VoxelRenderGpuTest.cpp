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
#include "TestTempDir.h"
#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelEdit.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::voxel;

namespace {

    class BandedSphere final : public BaseSource {
    public:
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int px = origin.x + x * spacing, py = origin.y + y * spacing, pz = origin.z + z * spacing;
                        const double dx = px - 32.0, dy = py - 32.0, dz = pz - 32.0;
                        const int index = sampleIndex(x, y, z);
                        sdf[index] = encodeSdf(std::sqrt(dx * dx + dy * dy + dz * dz) - 22.0);
                        material[index] = packMaterial((px / 4) % 4, (pz / 4 + 2) % 4, 110);
                    }
                }
            }
        }
    };

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
        TestTempDir textureDir;

        void SetUp() override {
            Engine::setPinnedShaderTime(1.0);
            scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.activate();
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

        // Four slots of four textures over a sphere, the first slot in bands along x and the second along
        // z, so the corners of a cell where the bands cross hold all four: what the tiers treat
        // differently (High reads 4, Medium 3, Low 2: R51). The maps go through the array and the
        // projections rather than a white fallback.
        Entity paintedSphere(VoxelQuality quality, const std::string& name) {
            GeneratorDesc generator;
            asset = VoxelAssets::create(name, []{
                VolumeDesc desc;
                desc.dimensions = Int3{64, 64, 64};
                desc.levels = 1;
                return desc;
            }(), generator, 4);
            asset->store->setBase(std::make_shared<BandedSphere>());
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.quality = quality;
            component.collision = false;
            component.focus = camera.getEntity();
            const uint8_t colours[4][3] = {{200, 60, 50}, {60, 190, 70}, {60, 80, 210}, {230, 200, 60}};
            for (int i = 0; i < 4; i++) {
                // A coarse checker in the slot's colour, whose height channel (alpha) follows it
                std::vector<uint8_t> pixels(64 * 64 * 4);
                for (int y = 0; y < 64; y++) {
                    for (int x = 0; x < 64; x++) {
                        const bool on = ((x / 8) + (y / 8) + i) % 2 == 0;
                        uint8_t* texel = &pixels[static_cast<size_t>(y * 64 + x) * 4];
                        for (int c = 0; c < 3; c++) texel[c] = static_cast<uint8_t>(colours[i][c] * (on ? 1.0 : 0.6));
                        texel[3] = 255;
                    }
                }
                const std::string path = textureDir.path().string() + "/slot" + std::to_string(i) + ".png";
                EXPECT_TRUE(writePng(path, pixels, 64, 64));
                VoxelMaterialSlot slot;
                slot.name = "slot";
                slot.layer.colorTexture.setPath(path);
                slot.layer.uvScale = Vector2(0.08f, 0.08f);
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
            slot.layer.pbr = true;      // a slot's tint applies on a PBR slot alone, as on a Terrain Layer
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
            if (!ready || !render->isAllLoaded()) {
                const VoxelStats stats = system->getStats(owner);
                std::printf("[gpu] not settled: ready %d, chunks %d, jobs %d, uploads %d, error '%s'\n", system->isVolumeReady(owner) ? 1 : 0, stats.chunks, stats.jobsPending, stats.uploadMeshesThisFrame, system->getError(owner).c_str());
                for (const std::string& line : render->describeNotLoaded()) std::printf("[gpu] not loaded: %s\n", line.c_str());
            }
            return ready && render->isAllLoaded();
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
    JASPER_REQUIRE_GPU();
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
        // The tiers must really differ on this content: a golden that any tier would match pins nothing
        if (tier.quality != VoxelQuality::High && !std::getenv("JASPER_TEST_UPDATE_GOLDEN")) {
            EXPECT_FALSE(compareTargetWithGolden("voxel_tier_high", CompareOptions{}).passed) << tier.golden;
        }
        scene.destroyEntity(owner);
        GpuEnvironment::drawFrames(2);
    }
}

TEST_F(VoxelRenderGpu, LodSeamGolden) {
    JASPER_REQUIRE_GPU();
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
    JASPER_REQUIRE_GPU();
    sun.setDirection(-0.8f, -0.25f, -0.2f);
    sun.setShadows(true);
    const Entity owner = wavyGround("mem://gpu-shadow", VoxelQuality::High, 3);
    camera.setPosition(60.0f, 90.0f, 60.0f);
    camera.setTarget(120.0f, 40.0f, 120.0f);
    ASSERT_TRUE(settled(owner));
    expectGolden("voxel_shadows");
}
