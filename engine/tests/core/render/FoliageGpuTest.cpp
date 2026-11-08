// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC7, AC9, AC11, AC13-AC16, AC28-AC31: what foliage draws, on the headless device. Meshes are small
// OBJ files a test writes, so there are no binary fixtures; the impostor's atlas is made by code,
// since the baker is the editor's (test_foliage_impostor.py covers the bake).

#include "Engine.h"
#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Terrain.h"
#include "picking/MeshRaycast.h"
#include "picking/PickTypes.h"
#include "pool/TextureDataPool.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "terrain/FoliageTypes.h"
#include "terrain/TreeStore.h"
#include "texture/TextureData.h"
#include "util/Angle.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::terrain;

namespace {

    constexpr double PINNED_TIME = 2.5;
    constexpr int RESOLUTION = 65;
    constexpr float SIZE = 128.0f;
    constexpr int ATLAS = 256;

    CompareOptions backendOptions(){
        CompareOptions options;
        if (std::strcmp(HeadlessContext::getBackendName(), "glcore") != 0){
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    // two crossed quads standing on the origin: a blade of grass, or (tall) a tree
    void writeCross(const std::string& path, float width, float height){
        std::ofstream out(path);
        const float w = width * 0.5f;
        out << "v " << -w << " 0 0\nv " << w << " 0 0\nv " << w << ' ' << height << " 0\nv " << -w << ' ' << height << " 0\n";
        out << "v 0 0 " << -w << "\nv 0 0 " << w << "\nv 0 " << height << ' ' << w << "\nv 0 " << height << ' ' << -w << "\n";
        out << "vn 0 0 1\nvn 1 0 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
        out << "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\nf 3/3/1 2/2/1 1/1/1\nf 4/4/1 3/3/1 1/1/1\n";
        out << "f 5/1/2 6/2/2 7/3/2\nf 5/1/2 7/3/2 8/4/2\nf 7/3/2 6/2/2 5/1/2\nf 8/4/2 7/3/2 5/1/2\n";
    }

    struct ImageData {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
    };

    ImageData captureTarget(){
        Framebuffer& target = GpuEnvironment::target();
        ImageData image;
        image.width = static_cast<int>(target.getWidth());
        image.height = static_cast<int>(target.getHeight());
        EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), image.width, image.height, image.pixels));
        return image;
    }

    // what the golden compare reports, between two frames of one run
    CompareResult compareImages(const ImageData& a, const ImageData& b, const CompareOptions& options){
        CompareResult result;
        const size_t pixels = static_cast<size_t>(a.width) * a.height;
        double total = 0.0;
        for (size_t i = 0; i < pixels && i * 4 + 3 < a.pixels.size() && i * 4 + 3 < b.pixels.size(); i++){
            int largest = 0;
            for (size_t c = 0; c < 3; c++) largest = std::max(largest, std::abs(int(a.pixels[i * 4 + c]) - int(b.pixels[i * 4 + c])));
            total += largest;
            result.maxDiff = std::max(result.maxDiff, largest);
            if (largest > options.pixelThreshold) result.badPixels++;
        }
        result.meanDiff = pixels ? total / static_cast<double>(pixels) : 0.0;
        result.badFraction = pixels ? static_cast<double>(result.badPixels) / static_cast<double>(pixels) : 0.0;
        result.passed = result.badFraction <= options.maxBadFraction && result.meanDiff <= options.maxMeanDiff;
        if (!result.passed) result.message = "the frames differ: " + std::to_string(result.badPixels) + " pixels, mean " + std::to_string(result.meanDiff);
        return result;
    }

    class FoliageGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::unique_ptr<Terrain> terrain;
        std::vector<unsigned char> heightPixels;
        TempDir dir;
        std::string grassPath;
        std::string treePath;
        std::vector<std::string> pooled;
        int nextMap = 0;
        // what each impostor atlas points at, for as long as the scene lives
        std::deque<std::vector<unsigned char>> atlases;

        void SetUp() override{
            JASPER_REQUIRE_GPU();
            Engine::setPinnedShaderTime(PINNED_TIME);
            scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setFarClip(1200.0f);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.35f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
            grassPath = (dir.path() / "grass.obj").string();
            treePath = (dir.path() / "tree.obj").string();
            writeCross(grassPath, 0.6f, 0.9f);
            writeCross(treePath, 5.0f, 9.0f);
            makeTerrain();
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
            for (const std::string& id : pooled) TextureDataPool::remove(id);
        }

        TerrainComponent& component(){ return scene.getComponent<TerrainComponent>(terrain->getEntity()); }
        FoliageSystem& foliage(){ return *scene.getSystem<FoliageSystem>(); }
        const FoliageStats& stats(){ return foliage().getStats(terrain->getEntity()); }

        void makeTerrain(){
            terrain = std::make_unique<Terrain>(&scene);
            terrain->setSize(SIZE);
            terrain->setMaxHeight(10.0f);
            heightPixels.assign(static_cast<size_t>(RESOLUTION) * RESOLUTION * 2, 0);
            TerrainComponent& terrainData = component();
            const std::string id = uniqueId("height");
            terrainData.heightMap.setData(id, TextureData(RESOLUTION, RESOLUTION, static_cast<unsigned int>(heightPixels.size()), ColorFormat::RED16, 1, heightPixels.data()));
            terrainData.heightMap.setReleaseDataAfterLoad(false);
            terrainData.needUpdateTexture = true;
            terrainData.needUpdateTerrain = true;
            terrain->setColor(0.2f, 0.35f, 0.15f);
        }

        std::string uniqueId(const std::string& what){
            const std::string id = "foliage-gpu-" + what + "-" + std::to_string(nextMap++);
            pooled.push_back(id);
            return id;
        }

        TerrainFoliageLayer& addGrass(float density = 2.0f, float drawDistance = 60.0f){
            TerrainComponent& terrainData = component();
            TerrainFoliageLayer layer;
            layer.meshPath = AssetRef(grassPath);
            layer.density = density;
            layer.maxSlope = 90.0f;
            layer.drawDistance = drawDistance;
            layer.seed = 11;
            layer.minScale = 1.0f;
            layer.maxScale = 1.0f;
            const std::string id = uniqueId("density");
            static const std::vector<unsigned char> full(static_cast<size_t>(RESOLUTION) * RESOLUTION, 255);
            layer.densityMap.setData(id, TextureData(RESOLUTION, RESOLUTION, static_cast<unsigned int>(full.size()), ColorFormat::RED, 1, const_cast<unsigned char*>(full.data())));
            terrainData.foliageLayers.push_back(layer);
            terrainData.needUpdateFoliage = true;
            return terrainData.foliageLayers.back();
        }

        TerrainTreePrototype& addPrototype(unsigned int id = 1){
            TerrainTreePrototype prototype;
            prototype.id = id;
            prototype.meshPath = AssetRef(treePath);
            component().treePrototypes.push_back(prototype);
            component().needUpdateFoliage = true;
            return component().treePrototypes.back();
        }

        void plant(unsigned int prototype, const std::vector<std::pair<float, float>>& points){
            foliage().update(0.0);
            std::shared_ptr<TreeStore> store = foliage().getOrCreateTreeStore(terrain->getEntity());
            ASSERT_NE(store, nullptr);
            std::vector<TreeRecord> added;
            for (const auto& point : points){
                TreeRecord record{};
                record.u = point.first / SIZE + 0.5f;
                record.v = point.second / SIZE + 0.5f;
                record.scale = 1.0f;
                record.rotation[3] = 32767;
                record.prototypeId = static_cast<uint16_t>(prototype);
                record.heightRatio = 128;
                record.flags = TREE_SNAP_TO_TERRAIN;
                std::memset(record.color, 255, 4);
                added.push_back(record);
            }
            TreeDelta delta;
            store->apply({}, added, {}, &delta);
            foliage().treesChanged(terrain->getEntity(), delta);
        }

        // The impostor a bake would make: a light column on every view, and an upward normal
        void giveImpostor(TerrainTreePrototype& prototype, int views = 4){
            std::vector<unsigned char>& albedo = atlases.emplace_back(static_cast<size_t>(ATLAS) * ATLAS * 4, 0);
            std::vector<unsigned char>& normal = atlases.emplace_back(static_cast<size_t>(ATLAS) * ATLAS * 4, 128);
            // the atlas's cells, as library/impostor.glsl lays them out
            const int tile = ATLAS / (views <= 4 ? 2 : 4);
            for (int y = 0; y < ATLAS; y++){
                for (int x = 0; x < ATLAS; x++){
                    const int local = x % tile;
                    if (std::abs(local - tile / 2) > tile / 8) continue;
                    unsigned char* pixel = &albedo[(static_cast<size_t>(y) * ATLAS + x) * 4];
                    pixel[0] = 230; pixel[1] = 230; pixel[2] = 230; pixel[3] = 255;
                }
            }
            prototype.impostorAlbedo.setData(uniqueId("albedo"), TextureData(ATLAS, ATLAS, static_cast<unsigned int>(albedo.size()), ColorFormat::RGBA, 4, albedo.data()));
            prototype.impostorNormal.setData(uniqueId("normal"), TextureData(ATLAS, ATLAS, static_cast<unsigned int>(normal.size()), ColorFormat::RGBA, 4, normal.data()));
            prototype.impostorViews = views;
            prototype.impostorBoundsCenter = Vector3(0.0f, 4.5f, 0.0f);
            prototype.impostorSize = Vector2(5.0f, 9.0f);
        }

        void look(const Vector3& eye, const Vector3& target){
            camera.setPosition(eye);
            camera.setTarget(target);
            camera.activate();
        }

        bool render(int maxFrames = 600){
            Scene* current = &scene;
            const bool ready = GpuEnvironment::runFrames([current](){
                return !current->getSystem<FoliageSystem>()->hasPendingUpdates()
                    && current->getSystem<RenderSystem>()->isAllLoaded();
            }, maxFrames);
            GpuEnvironment::drawFrames(3);
            return ready;
        }

        void expectGolden(const std::string& name){
            const CompareResult result = compareTargetWithGolden(name, backendOptions());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }

        // the frame's pixels that differ from the sky by more than a little: how much was drawn
        size_t coverage(const Vector4& region = Vector4(0.0f, 0.0f, 1.0f, 1.0f)){
            const ImageData image = captureTarget();
            size_t count = 0;
            const int x0 = static_cast<int>(region.x * image.width), x1 = static_cast<int>(region.z * image.width);
            const int y0 = static_cast<int>(region.y * image.height), y1 = static_cast<int>(region.w * image.height);
            for (int y = y0; y < y1; y++){
                for (int x = x0; x < x1; x++){
                    const unsigned char* pixel = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
                    if (pixel[0] > 150) count++;
                }
            }
            return count;
        }
    };

}

// AC7. Deviation: no golden from the pre-T5 engine exists on this branch's first commit, so the same run
// draws a legacy InstancedMesh with the old shader range and the placement T5 made, and compares them.
TEST_F(FoliageGpu, LegacyLayerLooksTheSame){
    addGrass(1.0f, 40.0f);
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    ASSERT_GT(stats().detailResident, 0u);
    const ImageData foliageImage = captureTarget();

    // The instances, as today's chunks held them, on one Instanced Mesh with distance fade
    const Entity meshEntity = foliage().getDetailEntity(terrain->getEntity(), 0);
    ASSERT_NE(meshEntity, NULL_ENTITY);
    const std::vector<FoliageInstanceInfo> instances = foliage().getInstancesInRadius(terrain->getEntity(), Vector3(0.0f, 0.0f, 0.0f), 500.0f, 1, 2000000);
    ASSERT_GT(instances.size(), 0u);

    Entity legacy = scene.createEntity();
    scene.addComponent<Transform>(legacy);
    scene.addComponent<MeshComponent>(legacy);
    scene.addComponent<ModelComponent>(legacy);
    InstancedMeshComponent instanced;
    instanced.maxInstances = static_cast<unsigned int>(instances.size());
    instanced.needUpdateInstances = true;
    instanced.distanceFade = true;
    instanced.fadeStart = 40.0f * FOLIAGE_FADE_START;
    instanced.fadeEnd = 40.0f;
    for (const FoliageInstanceInfo& info : instances){
        InstanceData data;
        data.position = info.position;
        data.rotation = info.rotation;
        data.scale = Vector3(info.scale, info.scale, info.scale);
        instanced.instances.push_back(data);
    }
    scene.addComponent<InstancedMeshComponent>(legacy, instanced);
    scene.getSystem<MeshSystem>()->loadPlacedModel(legacy, grassPath, true);
    component().foliageLayers.clear();
    component().needUpdateFoliage = true;
    ASSERT_TRUE(render());
    const ImageData legacyImage = captureTarget();

    const CompareResult result = compareImages(foliageImage, legacyImage, []{
        CompareOptions options;
        options.pixelThreshold = 16;
        options.maxBadFraction = 0.005;
        options.maxMeanDiff = 0.6;
        return options;
    }());
    std::printf("[gpu] legacy parity: %zu pixels over the threshold, mean %.4f\n", result.badPixels, result.meanDiff);
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_NO_ENGINE_ERRORS();
}

// AC9: a still camera rebuilds and uploads nothing, and draws by offsets
TEST_F(FoliageGpu, StillCameraUploadsNothing){
    addGrass();
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(stats().uploadBytes, 0u);
    look(Vector3(1.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    GpuEnvironment::drawFrames(1);
    EXPECT_GT(stats().uploadBytes, 0u) << "a moved camera does upload";
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(stats().uploadBytes, 0u);
}

TEST_F(FoliageGpu, DrawsThroughOffsets){
    addGrass();
    TerrainFoliageLayer second = component().foliageLayers[0];
    second.seed = 99;
    component().foliageLayers.push_back(second);
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    const uint32_t draws = stats().draws;
    EXPECT_GE(draws, 2u);
    EXPECT_LE(draws, 8u) << "per layer and level of detail, not per cell";
    EXPECT_GT(Engine::getFrameStats().instances, 0u);
}

// AC31
TEST_F(FoliageGpu, DrawCountEqualsStats){
    addGrass();
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    const uint32_t withFoliage = Engine::getFrameStats().drawCalls;
    const uint32_t foliageDraws = stats().draws;
    ASSERT_GT(foliageDraws, 0u);
    component().foliageLayers.clear();
    component().needUpdateFoliage = true;
    ASSERT_TRUE(render());
    EXPECT_EQ(withFoliage - Engine::getFrameStats().drawCalls, foliageDraws);
}

// AC11
TEST_F(FoliageGpu, WindPinned){
    TerrainFoliageLayer& layer = addGrass();
    layer.windBend = 1.0f;
    component().windBending = 1.0f;
    sun.setShadows(true);
    look(Vector3(0.0f, 5.0f, 22.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    expectGolden("foliage_wind_pinned");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(FoliageGpu, WindZeroUsesNoWindVariant){
    addGrass();
    look(Vector3(0.0f, 5.0f, 22.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    const Entity entity = foliage().getDetailEntity(terrain->getEntity(), 0);
    ASSERT_NE(entity, NULL_ENTITY);
    const InstancedMeshComponent& instanced = scene.getComponent<InstancedMeshComponent>(entity);
    EXPECT_TRUE(instanced.foliage);
    EXPECT_FALSE(instanced.foliageWind);

    component().foliageLayers[0].windBend = 1.0f;
    component().needUpdateFoliage = true;
    ASSERT_TRUE(render());
    const Entity windy = foliage().getDetailEntity(terrain->getEntity(), 0);
    EXPECT_TRUE(scene.getComponent<InstancedMeshComponent>(windy).foliageWind);
}

TEST_F(FoliageGpu, ShadowSwaysWithThePlant){
    TerrainTreePrototype& prototype = addPrototype();
    prototype.bendFactor = 1.0f;
    component().windBending = 2.0f;
    component().windDirection = Vector3(1.0f, 0.0f, 0.0f);
    plant(1, {{0.0f, 0.0f}});
    sun.setShadows(true);
    look(Vector3(0.0f, 25.0f, 25.0f), Vector3(0.0f, 0.0f, 0.0f));
    Engine::setPinnedShaderTime(0.5);
    ASSERT_TRUE(render());
    const ImageData first = captureTarget();
    Engine::setPinnedShaderTime(1.7);
    GpuEnvironment::drawFrames(3);
    const ImageData second = captureTarget();
    CompareOptions strict;
    strict.pixelThreshold = 4;
    strict.maxBadFraction = 0.0;
    const CompareResult result = compareImages(first, second, strict);
    EXPECT_GT(result.badPixels, 0u) << "the shadow moves with the wind";
}

// AC13: three cameras, the bands at each
TEST_F(FoliageGpu, TreesLodBands){
    addPrototype();
    giveImpostor(component().treePrototypes[0]);
    plant(1, {{-10.0f, -10.0f}, {0.0f, 0.0f}, {10.0f, 10.0f}, {-30.0f, 20.0f}, {40.0f, -40.0f}});
    component().billboardStart = 45.0f;
    look(Vector3(0.0f, 8.0f, 40.0f), Vector3(0.0f, 4.0f, 0.0f));
    ASSERT_TRUE(render());
    expectGolden("foliage_trees_lod_bands_near");
    EXPECT_GT(stats().meshTrees, 0u);

    look(Vector3(0.0f, 8.0f, 110.0f), Vector3(0.0f, 4.0f, 0.0f));
    ASSERT_TRUE(render());
    expectGolden("foliage_trees_lod_bands_far");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC14: the dithered band, across a cell edge, leaves no ring
TEST_F(FoliageGpu, FadeBandShowsNoCellRing){
    addPrototype();
    giveImpostor(component().treePrototypes[0]);
    std::vector<std::pair<float, float>> points;
    // close enough to be a wall: a cell's edge would be the only step in it
    for (int i = -40; i <= 40; i++) points.emplace_back(static_cast<float>(i) * 1.5f, 0.0f);
    plant(1, points);
    component().billboardStart = 30.0f;
    component().fadeLength = 20.0f;
    look(Vector3(0.0f, 12.0f, 40.0f), Vector3(0.0f, 5.0f, 0.0f));
    ASSERT_TRUE(render());
    const ImageData image = captureTarget();
    double largest = 0.0;
    std::vector<double> columns(image.width, 0.0);
    for (int y = image.height / 4; y < image.height * 3 / 4; y++){
        for (int x = 0; x < image.width; x++){
            const unsigned char* pixel = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
            columns[x] += (pixel[0] + pixel[1] + pixel[2]) / 3.0;
        }
    }
    for (double& value : columns) value /= image.height / 2.0;
    // the jump between neighbouring runs of 16 columns: a cell's edge would be a step in it
    for (int x = 16; x + 16 < image.width; x += 16){
        double a = 0.0, b = 0.0;
        for (int i = 0; i < 16; i++){
            a += columns[x - 16 + i];
            b += columns[x + i];
        }
        largest = std::max(largest, std::fabs(a - b) / 16.0);
    }
    std::printf("[gpu] the largest step across the band: %.2f/255\n", largest);
    EXPECT_LT(largest, 40.0);
}

// AC15
TEST_F(FoliageGpu, TreeImpostorFar){
    addPrototype();
    giveImpostor(component().treePrototypes[0]);
    plant(1, {{-15.0f, 0.0f}, {0.0f, 0.0f}, {15.0f, 0.0f}});
    component().billboardStart = 1.0f;
    component().fadeLength = 0.5f;
    component().maxMeshTrees = 0;
    look(Vector3(0.0f, 10.0f, 80.0f), Vector3(0.0f, 5.0f, 0.0f));
    ASSERT_TRUE(render());
    EXPECT_EQ(stats().meshTrees, 0u);
    EXPECT_GT(stats().treesVisible, 0u);
    expectGolden("foliage_tree_impostor_far");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC16
TEST_F(FoliageGpu, ShadowLod){
    addPrototype();
    plant(1, {{-8.0f, 0.0f}, {8.0f, 0.0f}});
    sun.setShadows(true);
    look(Vector3(0.0f, 20.0f, 25.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    expectGolden("foliage_shadow_lod");
}

TEST_F(FoliageGpu, ShadowMaskOffCastsNothing){
    TerrainTreePrototype& prototype = addPrototype();
    prototype.shadowLodMask = 0;
    prototype.bendFactor = 0.0f;
    plant(1, {{0.0f, 0.0f}});
    sun.setShadows(true);
    look(Vector3(0.0f, 20.0f, 25.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    const ImageData without = captureTarget();

    component().treePrototypes[0].shadowLodMask = 0x7F;
    component().needUpdateFoliage = true;
    ASSERT_TRUE(render());
    const ImageData with = captureTarget();
    CompareOptions strict;
    strict.pixelThreshold = 8;
    strict.maxBadFraction = 0.0;
    EXPECT_GT(compareImages(without, with, strict).badPixels, 0u) << "the mask decides whether its shadow exists";
}

// AC29: a material with a custom vertex program has no compact variant: the 96-byte stream
TEST_F(FoliageGpu, CustomVertexProgramUsesExpandedStream){
    addGrass();
    look(Vector3(0.0f, 5.0f, 22.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    const Entity entity = foliage().getDetailEntity(terrain->getEntity(), 0);
    ASSERT_NE(entity, NULL_ENTITY);
    EXPECT_FALSE(scene.getComponent<InstancedMeshComponent>(entity).foliageExpanded);
    const size_t compact = coverage();

    // a project's own fork reads what it reads: no compact path is assumed for it
    EXPECT_FALSE(RenderSystem::shaderSupportsCompactInstances("assets/my.shader"));
    EXPECT_TRUE(RenderSystem::shaderSupportsCompactInstances("library:foliage"));
    EXPECT_TRUE(RenderSystem::shaderSupportsCompactInstances(""));
    EXPECT_GT(compact, 0u) << "it still draws";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC30
TEST_F(FoliageGpu, NoFoliageNoPassNoBuffer){
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    EXPECT_FALSE(foliage().hasDrawables());
    EXPECT_TRUE(foliage().getEntities(terrain->getEntity()).empty());
    EXPECT_EQ(stats().gpuBytes, 0u);
}

TEST_F(FoliageGpu, DisabledTerrainDrawsNothing){
    addGrass();
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());
    const uint32_t enabledDraws = Engine::getFrameStats().drawCalls;
    ASSERT_GT(stats().draws, 0u);
    component().enabled = false;
    GpuEnvironment::drawFrames(3);
    EXPECT_LT(Engine::getFrameStats().drawCalls, enabledDraws);
    EXPECT_EQ(stats().draws, 0u);
}

// AC27: picking, with no leaves of its own in the BVH
class MeshRaycastFoliage : public FoliageGpu {};

TEST_F(MeshRaycastFoliage, HitsATreeAndReportsItsId){
    addPrototype();
    plant(1, {{0.0f, 0.0f}, {20.0f, 0.0f}});
    look(Vector3(0.0f, 8.0f, 30.0f), Vector3(0.0f, 4.0f, 0.0f));
    ASSERT_TRUE(render());

    const std::shared_ptr<TreeStore> store = foliage().getTreeStore(terrain->getEntity());
    const uint32_t first = store->records()[0].id;
    // a ray that crosses the first tree's quad, at its height
    const MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(0.0f, 4.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), 30.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, terrain->getEntity());
    EXPECT_EQ(treeIdRecord(hit.foliageId), first);
    EXPECT_EQ(hit.prototype, 0);
    EXPECT_NEAR(hit.point.z, 0.0f, 0.05f) << "LOD0's triangles, not the bounding sphere";

    // past the quad's edge it misses, though it is inside the sphere
    const MeshRaycastHit miss = MeshRaycast::raycast(&scene, Vector3(3.5f, 4.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), 30.0f);
    EXPECT_FALSE(miss.hit && miss.foliageId != 0);
}

TEST_F(MeshRaycastFoliage, GrassIsOptIn){
    addGrass(4.0f, 60.0f);
    look(Vector3(0.0f, 8.0f, 20.0f), Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(render());

    MeshRaycastOptions options;
    const MeshRaycastHit plain = MeshRaycast::raycast(&scene, Vector3(0.0f, 0.5f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), 30.0f, LAYER_MASK_DEFAULT_RAYCAST, options);
    EXPECT_FALSE(plain.hit && plain.foliageId != 0) << "the default kinds leave details out";

    options.kinds = MeshRaycast::ALL | MeshRaycast::DETAIL;
    bool found = false;
    for (int i = -12; i <= 12 && !found; i++){
        const MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(static_cast<float>(i) * 0.5f, 0.5f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), 30.0f, LAYER_MASK_DEFAULT_RAYCAST, options);
        found = hit.hit && hit.entity == terrain->getEntity() && hit.kind == PickKind::DETAIL && hit.prototype == 0;
    }
    EXPECT_TRUE(found) << "asked for, a ray through grass finds one";
}

TEST_F(MeshRaycastFoliage, MatchesDrawnTriangles){
    addPrototype();
    plant(1, {{0.0f, 0.0f}});
    component().maxMeshTrees = 10;
    look(Vector3(0.0f, 4.5f, 40.0f), Vector3(0.0f, 4.5f, 0.0f));
    ASSERT_TRUE(render());
    const ImageData image = captureTarget();
    // Through the frame's row at the tree's height: where the rays hit, the frame shows the tree
    const int y = image.height / 2;
    int agreed = 0;
    int checked = 0;
    const float aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
    const float tangent = std::tan(Angle::defaultToRad(camera.getYFov()) * 0.5f);
    for (int x = image.width / 2 - 40; x < image.width / 2 + 40; x += 4){
        const float ndc = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width) * 2.0f - 1.0f;
        const Vector3 direction = Vector3(ndc * tangent * aspect, 0.0f, -1.0f).normalized();
        const MeshRaycastHit hit = MeshRaycast::raycast(&scene, Vector3(0.0f, 4.5f, 40.0f), direction, 60.0f);
        const unsigned char* pixel = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
        // the tree is grey; the sky is blue
        const bool drawn = std::abs(int(pixel[0]) - int(pixel[2])) < 16;
        if (drawn == (hit.hit && hit.foliageId != 0)) agreed++;
        checked++;
    }
    EXPECT_GE(agreed, checked * 9 / 10) << agreed << " of " << checked;
}
