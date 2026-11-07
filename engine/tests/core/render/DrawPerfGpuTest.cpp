// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Cheaper frames (docs/superpowers/specs/2026-11-04-draw-perf-design.md), with GPU Instancing off
// so that each draw is one draw: a pass applies a pipeline and bindings equal to the ones it
// applied last only once (SokolSystem's applied state), the opaque queue is culled before its sort
// and sorted by distance band and render state, a rigid mesh's bounds ignore a bone palette it
// holds, the swapchain's pipelines are not made while the frame draws into an engine framebuffer,
// and a scene lit by directional lights alone draws the built-in Lit's 'Dlo' variant (the same
// image). Only engine API older than these changes is used: every test here fails without them.
// glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "texture/Framebuffer.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "render/LightingTiers.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // MESH 'Dlo' (ShaderPool::getDirectionalOnlyProperty), by its number: older engines have no name for it
    constexpr uint64_t DIRECTIONAL_ONLY_BIT = 1ull << 42;

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    class DrawPerfGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::vector<std::unique_ptr<Light>> lights;
        std::vector<std::unique_ptr<Primitive>> boxes;
        std::vector<std::unique_ptr<Model>> models;

        void SetUp() override{
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 0.0f, 30.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            // each draw one draw (S55's batches would merge the equal ones)
            Engine::setGpuInstancingEnabled(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            boxes.clear();
            models.clear();
            lights.clear();
            Engine::removeScene(&scene);
            Engine::setGpuInstancingEnabled(true);
            Engine::setDynamicBatching(true);
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
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

        // the mean luminance of the target's centre (a 20 x 20 square)
        float centreLuminance(){
            const std::vector<uint8_t> rgba = readTarget();
            Framebuffer& target = GpuEnvironment::target();
            const int width = (int)target.getWidth();
            const int height = (int)target.getHeight();
            double sum = 0.0;
            int count = 0;
            for (int y = height / 2 - 10; y < height / 2 + 10; y++){
                for (int x = width / 2 - 10; x < width / 2 + 10; x++){
                    const uint8_t* p = rgba.data() + ((size_t)y * width + x) * 4;
                    sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                    count++;
                }
            }
            return count ? (float)(sum / count) : 0.0f;
        }

        // the last frame's pipelines and bindings applied and draws, every pass
        RenderStats frameStats(){
            return SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
        }
    };

}

TEST_F(DrawPerfGpu, RepeatedDrawsApplyStateOnce){
    JASPER_REQUIRE_GPU();
    sun(false);
    // 30 Models of one file: one set of vertex and index buffers (MeshBufferPool), one material, so
    // one pipeline and equal bindings, all in one distance band
    for (int i = 0; i < 30; i++){
        models.push_back(std::make_unique<Model>(&scene));
        ASSERT_TRUE(models.back()->loadModel(JASPER_TEST_REPO_DIR "/engine/tests/core/fixtures/gltf/s41/dedup_sphere.obj"));
        models.back()->setPosition(-5.0f + 2.0f * (float)(i % 6), -4.0f + 2.0f * (float)(i / 6), 0.0f);
        models.back()->setScale(0.4f);
    }
    settle();
    const RenderStats stats = frameStats();
    std::printf("[draw-perf] 30 equal Models: %u draws, %u pipelines and %u bindings applied\n", stats.draws,
                stats.pipelinesApplied, stats.bindingsApplied);
    ASSERT_GE(stats.draws, 30u);
    EXPECT_LT(stats.pipelinesApplied * 4, stats.draws) << "a pipeline applied again for equal draws";
    EXPECT_LT(stats.bindingsApplied * 2, stats.draws) << "bindings applied again for equal draws";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, SortGroupsStatesInABand){
    JASPER_REQUIRE_GPU();
    sun(false);
    // 20 boxes along the view, alternately lit and unlit (two pipelines), nearest first alternating;
    // their nearest points 16.7 to 22.5 m from the camera: one distance band (16 to 32 m)
    for (int i = 0; i < 20; i++)
        box(-3.0f + 0.3f * (float)i, 0.0f, 13.0f - 0.3f * (float)i, i % 2 == 0);
    settle();
    // the colour pass: the one that drew the boxes
    const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
    const PassRecord* colour = nullptr;
    for (const PassRecord& pass : list.passes){
        if (pass.executed && (!colour || pass.draws > colour->draws))
            colour = &pass;
    }
    ASSERT_NE(colour, nullptr);
    std::printf("[draw-perf] 20 alternating boxes: the colour pass '%s' drew %u with %u pipelines applied\n", colour->label,
                colour->draws, colour->pipelines);
    ASSERT_GE(colour->draws, 20u);
    EXPECT_LE(colour->pipelines, 2u) << "the two states of one band were not drawn as two runs";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, OpaqueQueueIsCulledBeforeTheSort){
    JASPER_REQUIRE_GPU();
    sun(false);
    // 10 in front of the camera, 90 behind it
    for (int i = 0; i < 100; i++){
        const float z = (i < 10) ? 0.0f : 40.0f + (float)i;
        box(-4.5f + (float)(i % 10), 0.0f, z, true);
    }
    settle();
    const Engine::FrameStats& frame = Engine::getFrameStats();
    std::printf("[draw-perf] 100 boxes, 90 behind the camera: %u visible, %u culled by the visible list\n", frame.visible, frame.culled);
    EXPECT_GE(frame.culled, 90u) << "the queue was not culled before its sort";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, RigidMeshIgnoresABonePalette){
    JASPER_REQUIRE_GPU();
    Primitive& rigid = box(0.0f, 0.0f, 0.0f, true);
    settle();
    MeshComponent& mesh = rigid.getComponent<MeshComponent>();
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++)
        ASSERT_FALSE(mesh.submeshes[s].hasSkinning);
    // a palette entry it holds (a posed palette read from a file, a script's write) moves nothing:
    // its vertices have no bones
    mesh.bonesMatrix[3] = Matrix4::scaleMatrix(Vector3(5.0f, 5.0f, 5.0f));
    rigid.setPosition(1.0f, 0.0f, 0.0f);
    GpuEnvironment::drawFrames(2);
    const Transform& transform = rigid.getComponent<Transform>();
    const AABB expected = transform.modelMatrix * mesh.aabb;
    EXPECT_TRUE(mesh.skinnedAABB.isNull());
    EXPECT_NEAR(mesh.worldAABB.getMinimum().x, expected.getMinimum().x, 1e-4f);
    EXPECT_NEAR(mesh.worldAABB.getMaximum().x, expected.getMaximum().x, 1e-4f);
    EXPECT_NEAR(mesh.worldAABB.getMaximum().y, expected.getMaximum().y, 1e-4f);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, EngineFramebufferSkipsSwapchainPipelines){
    JASPER_REQUIRE_GPU();
    sun(false);
    Primitive& made = box(0.0f, 0.0f, 0.0f, true);
    settle();
    // the frames draw into the test's target (GpuEnvironment: Engine::setFramebuffer), never the
    // swapchain: no mesh makes its pipelines
    const Submesh& submesh = made.getComponent<MeshComponent>().submeshes[0];
    EXPECT_EQ(submesh.render.getPipelineId(PIP_DEFAULT), 0u) << "a swapchain pipeline was made";
    EXPECT_NE(submesh.render.getPipelineId(PIP_RTT), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, SunOnlySceneDrawsDirectionalOnly){
    JASPER_REQUIRE_GPU();
    sun(true);
    Primitive& made = box(0.0f, 0.0f, 0.0f, true);
    settle();
    const Submesh& submesh = made.getComponent<MeshComponent>().submeshes[0];
    EXPECT_NE(submesh.shaderProperties & DIRECTIONAL_ONLY_BIT, 0u) << "a sun-only scene's Lit is not 'Dlo'";
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 42), "Dlo");

    // a point light, even switched off, takes the general variant
    lights.push_back(std::make_unique<Light>(&scene));
    lights.back()->setType(LightType::POINT);
    lights.back()->setEnabled(false);
    settle();
    EXPECT_EQ(made.getComponent<MeshComponent>().submeshes[0].shaderProperties & DIRECTIONAL_ONLY_BIT, 0u)
        << "a point light in the scene, and the Lit is still 'Dlo'";

    // and keeps it until the scene loads again: lights that come and go reload nothing
    lights.pop_back();
    settle();
    EXPECT_EQ(made.getComponent<MeshComponent>().submeshes[0].shaderProperties & DIRECTIONAL_ONLY_BIT, 0u)
        << "the point light gone, and the Lit is 'Dlo' again (every mesh reloaded)";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, DirectionalOnlyDrawsTheSameImage){
    JASPER_REQUIRE_GPU();
    // the object lists, not Tiled (a point light under Automatic would draw the general variant's
    // tiled form)
    LightingTiers::setLightMode(LightMode::PER_OBJECT);
    sun(true);
    for (int i = 0; i < 6; i++)
        box(-3.0f + 1.2f * (float)i, -0.5f + 0.2f * (float)i, 26.0f - 0.5f * (float)i, true).setScale(1.5f);
    settle();
    ASSERT_NE(boxes[0]->getComponent<MeshComponent>().submeshes[0].shaderProperties & DIRECTIONAL_ONLY_BIT, 0u);
    const std::vector<uint8_t> directional = readTarget();
    // the general variant: a point light that gives no light
    lights.push_back(std::make_unique<Light>(&scene));
    lights.back()->setType(LightType::POINT);
    lights.back()->setEnabled(false);
    settle();
    const uint64_t general = boxes[0]->getComponent<MeshComponent>().submeshes[0].shaderProperties;
    ASSERT_EQ(general & DIRECTIONAL_ONLY_BIT, 0u);
    ASSERT_EQ(general & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::TILED_LIGHTS), 0u);
    const std::vector<uint8_t> generalImage = readTarget();
    ASSERT_EQ(generalImage.size(), directional.size());
    size_t differing = 0;
    for (size_t i = 0; i < generalImage.size(); i++){
        const int d = (int)generalImage[i] - (int)directional[i];
        if (d > 1 || d < -1)
            differing++;
    }
    EXPECT_EQ(differing, 0u) << "the 'Dlo' variant draws another image";
    LightingTiers::setLightMode(LightMode::AUTOMATIC);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, ArrivingPointLightIsDrawnByDirectionalOnlyMeshes){
    JASPER_REQUIRE_GPU();
    LightingTiers::setLightMode(LightMode::PER_OBJECT);
    sun(false).setIntensity(0.2f);
    Primitive& lit = box(0.0f, 0.0f, 24.0f, true);
    lit.setScale(3.0f);
    settle();
    ASSERT_NE(lit.getComponent<MeshComponent>().submeshes[0].shaderProperties & DIRECTIONAL_ONLY_BIT, 0u);
    const float before = centreLuminance();
    // a bright point light in front of the box: the very next frame shows it, whether the box
    // still draws 'Dlo' (the light in its ambient) or already the general variant
    lights.push_back(std::make_unique<Light>(&scene));
    Light& bulb = *lights.back();
    bulb.setType(LightType::POINT);
    bulb.setPosition(0.0f, 0.0f, 27.0f);
    bulb.setRange(20.0f);
    bulb.setIntensity(30.0f);
    GpuEnvironment::drawFrames(1);
    const float arriving = centreLuminance();
    settle();
    const float settled = centreLuminance();
    std::printf("[draw-perf] a point light arriving: luminance %.1f before, %.1f its first frame, %.1f settled\n", before, arriving, settled);
    EXPECT_GT(arriving, before + 10.0f) << "the arriving point light was not drawn";
    EXPECT_GT(settled, before + 10.0f);
    LightingTiers::setLightMode(LightMode::AUTOMATIC);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, LightsComingAndGoingReloadNothing){
    JASPER_REQUIRE_GPU();
    // the object lists: under Automatic on Medium and above a point light switches the scene to
    // Light Mode Tiled and back (S57's own variant change), which is not what this checks
    LightingTiers::setLightMode(LightMode::PER_OBJECT);
    sun(false);
    for (int i = 0; i < 20; i++)
        box(-4.75f + 0.5f * (float)i, 0.0f, 0.0f, true);
    lights.push_back(std::make_unique<Light>(&scene));
    lights.back()->setType(LightType::POINT);
    settle();
    // what each box draws with once a point light was seen: the general variant's pipelines
    std::vector<uint64_t> properties;
    std::vector<uint32_t> pipelines;
    for (const auto& made : boxes){
        const Submesh& submesh = made->getComponent<MeshComponent>().submeshes[0];
        properties.push_back(submesh.shaderProperties);
        pipelines.push_back(submesh.render.getPipelineId(PIP_RTT));
        EXPECT_EQ(submesh.shaderProperties & DIRECTIONAL_ONLY_BIT, 0u);
    }
    for (int round = 0; round < 3; round++){
        lights.pop_back();
        settle();
        lights.push_back(std::make_unique<Light>(&scene));
        lights.back()->setType(round % 2 ? LightType::SPOT : LightType::POINT);
        settle();
    }
    for (size_t i = 0; i < boxes.size(); i++){
        const Submesh& submesh = boxes[i]->getComponent<MeshComponent>().submeshes[0];
        EXPECT_EQ(submesh.shaderProperties, properties[i]) << "box " << i << " changed variant";
        EXPECT_EQ(submesh.render.getPipelineId(PIP_RTT), pipelines[i]) << "box " << i << " reloaded";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(DrawPerfGpu, StaticSceneUpdateCost){
    JASPER_REQUIRE_GPU();
    // 2,000 static meshes, no light, no mesh LOD, no terrain: what a frame costs with those off
    // (printed only: docs/perf/draw-perf.md)
    for (int i = 0; i < 2000; i++)
        box(-10.0f + 0.5f * (float)(i % 40), -6.0f + 0.5f * (float)(i / 40 % 25), -(float)(i / 1000) * 2.0f, false);
    settle();
    constexpr int FRAMES = 20;
    const auto start = std::chrono::steady_clock::now();
    GpuEnvironment::drawFrames(FRAMES);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / FRAMES;
    std::printf("[bench] 2,000 static unlit meshes, lights, LOD and terrain off: %.2f ms a frame (update and draw)\n", ms);
    EXPECT_NO_ENGINE_ERRORS();
}
