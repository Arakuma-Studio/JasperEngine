// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R41-R44 (AC35): a cave mouth dug through a terrain by an attached volume whose slot follows the
// terrain's layer. Its golden; and across the hole's edge the colour steps no more than across a strip
// of the same terrain, High and Low.

#include "TerrainGpuFixture.h"

#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelEdit.h"

#include <span>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::voxel;

namespace {

    constexpr float GROUND = 10.0f;     // the terrain's height: a quarter of its 40 m
    constexpr float VIEW_WIDTH = 64.0f;
    constexpr float VIEW_HEIGHT = 36.0f;

    class VoxelTerrainGpu : public TerrainGpuFixture {
    protected:
        std::shared_ptr<VoxelAsset> asset;
        Entity owner = NULL_ENTITY;

        void SetUp() override {
            TerrainGpuFixture::SetUp();
            Engine::setPinnedShaderTime(1.0);
        }

        void TearDown() override {
            Engine::setPinnedShaderTime(-1.0);
            TerrainGpuFixture::TearDown();
        }

        // A flat terrain with one textured layer, a 64 m volume attached over its middle whose slot is
        // that layer, and a mouth dug at the middle
        void ground() {
            makeTerrain(40.0f, [](float, float) { return GROUND / 40.0f; });
            terrain->setTextureLayer(0, writeColorMap("ground", 120, 150, 80));
            setBlendMap(0, [](float, float) { return TerrainGpuColor{255, 0, 0, 255}; });
        }

        bool caveMouth(VoxelQuality quality, const std::string& name) {
            if (!terrain) ground();

            GeneratorDesc generator;
            generator.kind = "terrain";
            VolumeDesc desc;
            desc.dimensions = Int3{64, 64, 64};
            desc.levels = 2;
            desc.bounds = BoundsMode::Open;
            asset = VoxelAssets::create(name, desc, generator, 1);
            owner = scene.createEntity();
            scene.addComponent<Transform>(owner, {});
            Transform& transform = scene.getComponent<Transform>(owner);
            transform.position = Vector3(-32.0f, 0.0f, -32.0f);
            transform.worldPosition = transform.position;
            transform.modelMatrix = Matrix4::translateMatrix(transform.position);
            VoxelVolumeComponent component;
            component.volumeData.path = name;
            component.terrain = terrain->getEntity();
            component.terrainCut = VoxelTerrainCut::Automatic;
            component.collision = false;
            component.quality = quality;
            component.focus = camera.getEntity();
            VoxelMaterialSlot slot;
            slot.name = "ground";
            slot.linkedTerrainLayer = 0;
            component.materialSlots.push_back(slot);
            scene.addComponent<VoxelVolumeComponent>(owner, component);
            if (!settled()) return false;

            EditOp mouth;
            mouth.kind = EditKind::Dig;
            mouth.shape.kind = ShapeKind::Sphere;
            mouth.shape.center = Vec3d{32.0, GROUND + 1.0, 32.0};
            mouth.shape.radius = 7.0;
            const EditResult dug = scene.getSystem<VoxelSystem>()->edit(owner, std::span<const EditOp>(&mouth, 1), EditOptions{});
            if (!dug.ok) {
                std::printf("[gpu] the mouth's edit failed: %s\n", editErrorCode(dug.error));
                return false;
            }
            return settled();
        }

        bool settled() {
            GpuEnvironment::setView(FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH, FRAME_HEIGHT, Scaling::STRETCH);
            std::shared_ptr<VoxelSystem> system = scene.getSystem<VoxelSystem>();
            RenderSystem* render = scene.getSystem<RenderSystem>().get();
            const Entity volume = owner;
            Scene* current = &scene;
            const auto stable = [&]() {
                if ((volume != NULL_ENTITY && !system->isVolumeReady(volume)) || !render->isAllLoaded()) return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++) {
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.needReload || mesh.needVariantReload) return false;
                }
                return true;
            };
            // the volume's first chunks on screen open its holes (R44), and the terrain swaps to its
            // holed variant after that, frames later where pipelines are made on a worker (Vulkan,
            // spec S32): settled is when a few more frames change nothing
            bool ready = false;
            for (int round = 0; round < 4 && !ready; round++) {
                ready = GpuEnvironment::runFrames(stable, 600);
                GpuEnvironment::drawFrames(4);
                ready = ready && stable();
            }
            if (!ready) {
                if (volume != NULL_ENTITY) {
                    const VoxelStats stats = system->getStats(volume);
                    std::printf("[gpu] not settled: ready %d, chunks %d, jobs %d, error '%s'; terrain %u, volume %u\n", system->isVolumeReady(volume) ? 1 : 0, stats.chunks, stats.jobsPending, system->getError(volume).c_str(), terrain ? terrain->getEntity() : 0u, volume);
                }
                for (const std::string& line : render->describeNotLoaded()) std::printf("[gpu] not loaded: %s\n", line.c_str());
                return false;
            }
            return true;
        }

        void lookDown() {
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(-VIEW_WIDTH * 0.5f, VIEW_WIDTH * 0.5f, -VIEW_HEIGHT * 0.5f, VIEW_HEIGHT * 0.5f, 1.0f, 400.0f);
            camera.setPosition(0.0f, 200.0f, 0.0f);
            camera.setUp(0.0f, 0.0f, -1.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
        }

        const uint8_t* pixelAt(const std::vector<uint8_t>& frame, float x, float z) const {
            const int px = static_cast<int>((x + VIEW_WIDTH * 0.5f) / VIEW_WIDTH * FRAME_WIDTH);
            const int py = static_cast<int>((z + VIEW_HEIGHT * 0.5f) / VIEW_HEIGHT * FRAME_HEIGHT);
            if (px < 0 || py < 0 || px >= FRAME_WIDTH || py >= FRAME_HEIGHT) return nullptr;
            return &frame[(static_cast<size_t>(py) * FRAME_WIDTH + px) * 4];
        }

        static float difference(const uint8_t* a, const uint8_t* b) {
            return (std::fabs(float(a[0]) - float(b[0])) + std::fabs(float(a[1]) - float(b[1])) + std::fabs(float(a[2]) - float(b[2]))) / 3.0f;
        }

        // Across every hole texel's edge to a solid neighbour along x, the mean colour difference of a
        // pixel a texel inside and one a texel outside; and of the same pair in the frame of the terrain alone
        void seamSteps(const std::vector<uint8_t>& frame, const std::vector<uint8_t>& terrainOnly, float& acrossEdge, float& acrossTerrain, int& edges) {
            TerrainComponent& terrainComponent = component();
            TerrainComponentStore store(&scene, terrain->getEntity(), terrainComponent);
            const terrain::MapId map{terrain::MapKind::Hole, 0};
            const terrain::MapFormat format = store.format(map);
            acrossEdge = acrossTerrain = 0.0f;
            edges = 0;
            if (!format.exists) return;
            const int width = format.bounds.w, height = format.bounds.h;
            std::vector<uint8_t> texels(static_cast<size_t>(width) * height);
            store.readBytes(map, format.bounds, texels);
            const float texel = TERRAIN_SIZE / static_cast<float>(width);
            for (int ty = 0; ty < height; ty++) {
                for (int tx = 0; tx + 1 < width; tx++) {
                    const bool hole = texels[static_cast<size_t>(ty) * width + tx] < 128;
                    const bool next = texels[static_cast<size_t>(ty) * width + tx + 1] < 128;
                    if (hole == next) continue;
                    const Vector2 a = store.worldOfTexel(map, tx, ty);
                    const Vector2 b = store.worldOfTexel(map, tx + 1, ty);
                    const float midX = (a.x + b.x) * 0.5f, z = a.y;
                    // the hole side is the voxel ground; the same two pixels before the volume were terrain
                    const float holeX = hole ? midX - texel : midX + texel, solidX = hole ? midX + texel : midX - texel;
                    const uint8_t* inside = pixelAt(frame, holeX, z);
                    const uint8_t* outside = pixelAt(frame, solidX, z);
                    const uint8_t* stripA = pixelAt(terrainOnly, holeX, z);
                    const uint8_t* stripB = pixelAt(terrainOnly, solidX, z);
                    if (!inside || !outside || !stripA || !stripB) continue;
                    acrossEdge += difference(inside, outside);
                    acrossTerrain += difference(stripA, stripB);
                    edges++;
                }
            }
            if (edges > 0) {
                acrossEdge /= static_cast<float>(edges);
                acrossTerrain /= static_cast<float>(edges);
            }
        }

        // The step across each edge against the step between the same two pixels while both were terrain
        // of the same tier, 190 m away, where the tile break is half in at High and Medium
        void expectSeamMatches(VoxelQuality quality, TerrainQualitySetting terrainQuality, const std::string& name) {
            ground();
            component().quality = terrainQuality;
            lookDown();
            ASSERT_TRUE(settled());
            std::vector<uint8_t> terrainOnly;
            ASSERT_TRUE(readFrame(terrainOnly));
            ASSERT_TRUE(caveMouth(quality, name));
            ASSERT_TRUE(settled());
            std::vector<uint8_t> frame;
            ASSERT_TRUE(readFrame(frame));
            float acrossEdge = 0.0f, acrossTerrain = 0.0f;
            int edges = 0;
            seamSteps(frame, terrainOnly, acrossEdge, acrossTerrain, edges);
            std::printf("[gpu] %s: colour step across the hole's edge %.2f, across a strip of terrain %.2f, %d edges\n", name.c_str(), acrossEdge, acrossTerrain, edges);
            ASSERT_GT(edges, 8);
            // within 3 % of what the terrain itself steps by over the same distance
            EXPECT_LE(acrossEdge, acrossTerrain * 1.03f);
        }
    };

}

TEST_F(VoxelTerrainGpu, CaveMouth) {
    JASPER_REQUIRE_GPU();
    ASSERT_TRUE(caveMouth(VoxelQuality::High, "mem://gpu-cave-mouth"));
    camera.setPosition(0.0f, 26.0f, 30.0f);
    camera.setTarget(0.0f, GROUND - 2.0f, 0.0f);
    ASSERT_TRUE(settled());
    CompareOptions options;
    if (std::strcmp(HeadlessContext::getBackendName(), "glcore") != 0) {
        options.pixelThreshold = 16;
        options.maxBadFraction = 0.03;
        options.maxMeanDiff = 3.0;
    }
    const CompareResult result = compareTargetWithGolden("voxel_cave_mouth", options);
    std::printf("[gpu] voxel_cave_mouth on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(VoxelTerrainGpu, SeamColourStep) {
    JASPER_REQUIRE_GPU();
    expectSeamMatches(VoxelQuality::High, TerrainQualitySetting::HIGH, "mem://gpu-seam-high");
}

TEST_F(VoxelTerrainGpu, SeamColourStepLow) {
    JASPER_REQUIRE_GPU();
    // Low reads near only (R51), as T3's Low terrain does (T3 R1)
    expectSeamMatches(VoxelQuality::Low, TerrainQualitySetting::LOW, "mem://gpu-seam-low");
}
