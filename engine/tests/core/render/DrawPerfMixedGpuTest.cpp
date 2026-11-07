// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Skipping repeated pipelines and bindings (SokolSystem's applied state, draw-perf spec 6.1)
// changes no pixel: one colour pass with sprites drawn by Dynamic Batching (their held draws and
// deferred flushes), boxes instanced by S55's batches, meshes drawn alone and Models sharing
// their buffers, under a shadowed sun, draws the same image with the skipping on and off
// (SystemRender::setRepeatedStateSkipped). glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "object/Sprite.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/Framebuffer.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // an 8x8 checker (its pixels kept for the process: TextureData does not own them)
    TextureData checkerTexture(){
        static std::vector<uint8_t> pixels;
        if (pixels.empty()){
            pixels.resize(8 * 8 * 4);
            for (int i = 0; i < 64; i++){
                const bool dark = ((i % 8) / 2 + (i / 8) / 2) % 2 == 0;
                pixels[i * 4] = dark ? 120 : 240;
                pixels[i * 4 + 1] = dark ? 60 : 120;
                pixels[i * 4 + 2] = 30;
                pixels[i * 4 + 3] = 255;
            }
        }
        return TextureData(8, 8, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    class DrawPerfMixedGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Light>> lights;
        std::vector<std::unique_ptr<Primitive>> boxes;
        std::vector<std::unique_ptr<Model>> models;
        std::vector<std::unique_ptr<Sprite>> sprites;

        void SetUp() override{
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 0.0f, 30.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            sprites.clear();
            boxes.clear();
            models.clear();
            lights.clear();
            Engine::removeScene(&scene);
            Engine::setGpuInstancingEnabled(true);
            Engine::setDynamicBatching(true);
            SystemRender::setRepeatedStateSkipped(true);
        }

        Light& sun(bool shadows){
            lights.push_back(std::make_unique<Light>(&scene));
            Light& light = *lights.back();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(-0.3f, -1.0f, -0.4f);
            light.setIntensity(1.5f);
            light.setShadows(shadows);
            return light;
        }

        Primitive& box(float x, float y, float z, bool lit){
            boxes.push_back(std::make_unique<Primitive>(&scene));
            Primitive& made = *boxes.back();
            made.createBox(0.6f, 0.6f, 0.6f);
            made.setColor(0.8f, 0.5f, 0.2f);
            made.setReceiveLights(lit);
            made.setPosition(x, y, z);
            return made;
        }

        void settle(){
            Scene* current = &scene;
            ASSERT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            })) << "the scene never finished loading";
            GpuEnvironment::drawFrames(2);
        }

        RenderStats frameStats(){
            return SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
        }
    };

}

TEST_F(DrawPerfMixedGpu, MatchesWithoutSkippingRepeatedState){
    JASPER_REQUIRE_GPU();
    // batched sprites (Dynamic Batching), automatically instanced boxes (S55), meshes drawn alone
    // and Models sharing their buffers, in one colour pass: the same image whether repeated
    // pipelines and bindings are skipped or applied again
    Engine::setGpuInstancingEnabled(true);
    Engine::setDynamicBatching(true);
    sun(true);
    const TextureData checker = checkerTexture();
    for (int i = 0; i < 16; i++){
        sprites.push_back(std::make_unique<Sprite>(&scene));
        Sprite& s = *sprites.back();
        s.setTexture("draw-perf-checker", checker);
        s.setSize(1, 1);
        s.setPosition(-4.0f + 0.5f * (float)i, 3.0f, 5.0f);
    }
    for (int i = 0; i < 24; i++)
        box(-6.0f + 0.5f * (float)i, 0.0f, 0.0f, true);
    for (int i = 0; i < 8; i++){
        Primitive& alone = box(-4.0f + 1.0f * (float)i, -2.0f, 2.0f, true);
        alone.setColor(0.1f * (float)i, 0.9f - 0.1f * (float)i, 0.4f);
    }
    for (int i = 0; i < 6; i++){
        models.push_back(std::make_unique<Model>(&scene));
        ASSERT_TRUE(models.back()->loadModel(JASPER_TEST_REPO_DIR "/engine/tests/core/fixtures/gltf/s41/dedup_sphere.obj"));
        models.back()->setPosition(-3.0f + 1.2f * (float)i, -4.0f, 4.0f);
        models.back()->setScale(0.5f);
    }
    settle();
    const std::vector<uint8_t> skipped = readTarget();
    const RenderStats stats = frameStats();
    SystemRender::setRepeatedStateSkipped(false);
    GpuEnvironment::drawFrames(3);
    const std::vector<uint8_t> applied = readTarget();
    const RenderStats appliedStats = frameStats();
    SystemRender::setRepeatedStateSkipped(true);
    std::printf("[draw-perf] mixed pass: %u draws; skipping %u pipelines / %u bindings, applying all %u / %u\n", stats.draws,
                stats.pipelinesApplied, stats.bindingsApplied, appliedStats.pipelinesApplied, appliedStats.bindingsApplied);
    ASSERT_EQ(skipped.size(), applied.size());
    size_t differing = 0;
    for (size_t i = 0; i < skipped.size(); i++){
        if (skipped[i] != applied[i])
            differing++;
    }
    EXPECT_EQ(differing, 0u) << "skipping repeated state changed the image";
    EXPECT_LT(stats.pipelinesApplied, appliedStats.pipelinesApplied);
    EXPECT_NO_ENGINE_ERRORS();
}
