// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Water Excluders and Water Wakes on the headless device
// (docs/superpowers/specs/2026-11-04-water-boats-design.md). The shader clock is pinned, as in
// WaterFeaturesGpuTest, whose scene and golden these reuse.
//
// water_excluders: a water over a red bed with a turned Box, a Sphere and a Convex Hull (a
// cone, apex down, the hull of a boat) crossing its surface: the red bed shows through each hole.
//
// IdleBitsKeepTheWater: the 'Wex' and 'Wwk' variants with nothing to draw (an excluder far out
// of view, a Water Wake not on the water) give water_features_off, pixel for pixel on glcore:
// the exclusion loop and the wake read leave the water as it was.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterInteractionSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace water_boats_gpu_test {

    constexpr double PINNED_TIME = 7.25;

    bool isGoldenBackend(){
        return std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
    }

    CompareOptions exactOnGolden(){
        CompareOptions options;
        if (isGoldenBackend()){
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
        }else{
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    Material waterMaterial(){
        Material material;
        material.name = "Water";
        material.shader = "library:water";
        material.shaderFeatures = {"BUILTIN_TEXTURES"};
        material.lighting = MaterialLighting::PIXEL;
        return material;
    }

    class WaterBoatsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            Engine::setPinnedShaderTime(PINNED_TIME);
            scene.setBackgroundColor(0.12f, 0.14f, 0.18f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.5f);
            sun.setIntensity(1.5f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        void expectGolden(const std::string& name){
            const CompareResult result = compareTargetWithGolden(name, exactOnGolden());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }

        // an excluder without a mesh (Box or Sphere) at a place, turned about Y
        Entity excluder(WaterExcluderShape shape, const Vector3& position, const Vector3& size, float yaw = 0.0f){
            const Entity entity = scene.createEntity();
            Transform transform;
            transform.position = position;
            transform.rotation = Quaternion(yaw, Vector3(0.0f, 1.0f, 0.0f));
            scene.addComponent<Transform>(entity, transform);
            WaterExcluderComponent component;
            component.shape = shape;
            component.size = size;
            scene.addComponent<WaterExcluderComponent>(entity, component);
            return entity;
        }

        // WaterFeaturesGpu.FeaturesOffMatchesTheShaderBeforeThem's scene
        void featuresOffScene(Primitive& ground, Primitive& water){
            camera.setPosition(0.0f, 3.0f, 7.0f);
            camera.setTarget(0.0f, 0.0f, -2.0f);
            camera.activate();
            ground.createPlane(8.0f, 8.0f);
            ground.setPosition(-2.0f, -0.4f, -1.0f);
            ground.setColor(0.55f, 0.45f, 0.3f);
            water.createPlane(24.0f, 24.0f);
            water.setMaterial(waterMaterial());
        }

        uint64_t waterProperties(Entity water){
            return scene.getComponent<MeshComponent>(water).submeshes[0].shaderProperties;
        }
    };

}

using namespace water_boats_gpu_test;

TEST_F(WaterBoatsGpu, ExcludersCutTheSurface){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 6.0f, 6.0f);
    camera.setTarget(0.0f, 0.0f, -1.0f);
    camera.activate();

    Primitive bed(&scene);
    bed.createPlane(16.0f, 16.0f);
    bed.setPosition(0.0f, -0.6f, -1.0f);
    bed.setColor(0.9f, 0.1f, 0.08f);

    Primitive water(&scene);
    water.createPlane(24.0f, 24.0f);
    water.setMaterial(waterMaterial());

    excluder(WaterExcluderShape::BOX, Vector3(-2.5f, 0.0f, 0.0f), Vector3(2.0f, 1.0f, 1.2f), 30.0f);
    excluder(WaterExcluderShape::SPHERE, Vector3(0.5f, 0.0f, 0.5f), Vector3(1.6f, 1.6f, 1.6f));
    // a boat: a cone apex down, its hull a Convex Hull excluder of its own mesh
    Primitive boat(&scene);
    boat.createCylinder(0.0f, 1.0f, 1.2f, 24, 1);
    boat.setPosition(2.5f, 0.2f, -2.0f);
    boat.setColor(0.85f, 0.85f, 0.8f);
    scene.addComponent<WaterExcluderComponent>(boat.getEntity(), WaterExcluderComponent{});

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_TRUE(waterProperties(water.getEntity()) & ShaderPool::getWaterExclusionProperty());
#ifdef JASPERENGINE_PHYSICS_3D
    EXPECT_EQ(scene.getSystem<WaterInteractionSystem>()->getHullInfo(boat.getEntity()).status, WaterHullStatus::READY);
#endif
    expectGolden("water_excluders");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(WaterBoatsGpu, IdleBitsKeepTheWater){
    JASPER_REQUIRE_GPU();
    Primitive ground(&scene);
    Primitive water(&scene);
    featuresOffScene(ground, water);

    // an excluder far out of view and a Water Wake high above the water: both bits, nothing to draw
    excluder(WaterExcluderShape::BOX, Vector3(200.0f, 0.0f, 200.0f), Vector3(2.0f, 2.0f, 2.0f));
    const Entity kite = scene.createEntity();
    Transform kiteTransform;
    kiteTransform.position = Vector3(0.0f, 50.0f, 0.0f);
    scene.addComponent<Transform>(kite, kiteTransform);
    scene.addComponent<WaterWakeComponent>(kite, WaterWakeComponent{});

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const uint64_t properties = waterProperties(water.getEntity());
    EXPECT_TRUE(properties & ShaderPool::getWaterExclusionProperty());
    EXPECT_TRUE(properties & ShaderPool::getWaterWakeProperty());
    EXPECT_FALSE(scene.getSystem<WaterInteractionSystem>()->isWakeActive());
    expectGolden("water_features_off");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(WaterBoatsGpu, WakeUploadsOncePerFrame){
    JASPER_REQUIRE_GPU();
    Primitive ground(&scene);
    Primitive water(&scene);
    featuresOffScene(ground, water);
    // a hull crossing the water
    const Entity hull = scene.createEntity();
    Transform hullTransform;
    hullTransform.position = Vector3(0.0f, 0.0f, -6.0f);
    scene.addComponent<Transform>(hull, hullTransform);
    WaterWakeComponent wake;
    wake.width = 1.0f;
    wake.length = 3.0f;
    scene.addComponent<WaterWakeComponent>(hull, wake);
    ASSERT_TRUE(render()) << "the scene never finished loading";

    WaterInteractionSystem* system = scene.getSystem<WaterInteractionSystem>().get();
    for (int i = 0; i < 20; i++){
        Transform& transform = scene.getComponent<Transform>(hull);
        transform.position = Vector3(0.0f, 0.0f, -6.0f + 0.15f * (float)i);
        transform.needUpdate = true;
        // a second update in the frame (sokol takes one image update per frame; Doriax c5b7a99dc)
        system->update(1.0f / 60.0f);
        const uint64_t before = system->getWakeUploadCount();
        GpuEnvironment::drawFrames(1);
        EXPECT_LE(system->getWakeUploadCount() - before, 1u) << "frame " << i;
    }
    EXPECT_TRUE(system->isWakeActive());
    EXPECT_GT(system->getWakeUploadCount(), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// Opaque water tiles in a scene without Water Excluders and Wakes keep GPU Instancing: their
// variant has neither bit, so they draw as one instanced batch with the shader keys they had
// before (the scene's first excluder makes them draw alone, each choosing its excluders)
TEST_F(WaterBoatsGpu, OpaqueTilesStayInstanced){
    JASPER_REQUIRE_GPU();
    const bool instancing = Engine::isGpuInstancingEnabled();
    Engine::setGpuInstancingEnabled(true);
    camera.setPosition(0.0f, 14.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Material opaque = waterMaterial();
    opaque.blendMode = MaterialBlendMode::NONE;
    std::vector<std::unique_ptr<Primitive>> tiles;
    for (int i = 0; i < 9; i++){
        auto tile = std::make_unique<Primitive>(&scene);
        tile->createPlane(4.0f, 4.0f);
        tile->setPosition(-4.0f + 4.0f * (float)(i % 3), 0.0f, -4.0f + 4.0f * (float)(i / 3));
        tile->setMaterial(opaque);
        tiles.push_back(std::move(tile));
    }
    ASSERT_TRUE(render()) << "the scene never finished loading";
    GpuEnvironment::drawFrames(1);
    const Submesh& first = scene.getComponent<MeshComponent>(tiles[0]->getEntity()).submeshes[0];
    const uint64_t waterBits = ShaderPool::getWaterExclusionProperty() | ShaderPool::getWaterWakeProperty();
    EXPECT_EQ(first.shaderProperties & waterBits, 0u);
    EXPECT_NE(first.batchShaderProperties & ShaderPool::getAutoInstancingProperty(ShaderType::MESH), 0u);
    EXPECT_EQ(first.batchShaderProperties & waterBits, 0u);
    const DrawBatchStats tiled = SystemRender::getDrawBatchStats();
    std::printf("[gpu] opaque water tiles: %u instanced draws for %u draws\n", tiled.instancedDraws, tiled.batchedInstances);
    EXPECT_EQ(tiled.instancedDraws, 1u);
    EXPECT_EQ(tiled.batchedInstances, 9u);

    excluder(WaterExcluderShape::BOX, Vector3(100.0f, 0.0f, 100.0f), Vector3(1.0f, 1.0f, 1.0f));
    Scene* current = &scene;
    const Entity tile = tiles[0]->getEntity();
    ASSERT_TRUE(GpuEnvironment::runFrames([current, tile](){
        return current->getSystem<RenderSystem>()->isAllLoaded() &&
               (current->getComponent<MeshComponent>(tile).submeshes[0].shaderProperties & ShaderPool::getWaterExclusionProperty()) != 0;
    })) << "the excluder's variants never finished loading";
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);

    Engine::setGpuInstancingEnabled(instancing);
    EXPECT_NO_ENGINE_ERRORS();
}
