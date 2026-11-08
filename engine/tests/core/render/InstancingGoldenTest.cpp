// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// GPU Instancing (spec S55 R1, AC1): a scene of many copies of a few meshes and materials, lit
// by a shadowing sun, with overlapping transparent planes in front, drawn with the project's GPU
// Instancing off (every draw alone: the submission before S55) and on (state sorted, batched).
// The two images are the same pixel for pixel, the transparent queue keeping its order (DrawSort
// covers the grouping), and on draws instanced batches. The existing core goldens, drawn with
// GPU Instancing on (the default), still match the images written before it.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/LightingTiers.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "render/shadow/ShadowTypes.h"
#include "registry/EntityCloner.h"
#include "component/MeshComponent.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class InstancingGoldenGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::vector<std::unique_ptr<Primitive>> objects;
        std::vector<std::unique_ptr<Model>> models;

        void SetUp() override{
            scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 7.0f, 11.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.5f);
            sun.setIntensity(1.5f);
            sun.setShadows(true);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            objects.clear();
            models.clear();
            Engine::removeScene(&scene);
            Engine::setGpuInstancingEnabled(true);
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            Engine::setFixedDeltatime(0.0);
        }

        Primitive& add(){
            objects.push_back(std::make_unique<Primitive>(&scene));
            return *objects.back();
        }

        // a 6 x 6 grid of glTF boxes and spheres (one vertex buffer per file, shared by its Models:
        // the meshes GPU Instancing batches) in two colours, a floor (too small to be instanced:
        // dynamic batching's), and three overlapping transparent planes nearer the camera
        void buildScene(){
            Primitive& floor = add();
            floor.createPlane(14.0f, 14.0f);
            floor.setColor(0.7f, 0.7f, 0.65f);
            for (int z = 0; z < 6; z++){
                for (int x = 0; x < 6; x++){
                    models.push_back(std::make_unique<Model>(&scene));
                    Model& model = *models.back();
                    const bool sphere = (x + z) % 3 == 2;
                    EXPECT_TRUE(model.loadModel(sphere ? "gltf/instancing/sphere.gltf" : "gltf/instancing/box.gltf"));
                    if (sphere)
                        model.setColor(0.9f, 0.5f, 0.2f);
                    else if ((x + z) % 2 == 0)
                        model.setColor(0.2f, 0.6f, 0.9f);
                    else
                        model.setColor(0.8f, 0.2f, 0.3f);
                    model.setPosition(-5.0f + 2.0f * (float)x, 0.5f, -5.0f + 2.0f * (float)z);
                    model.setRotation(0.0f, 15.0f * (float)(x + z), 0.0f);
                }
            }
            const float colors[3][4] = {{1.0f, 0.2f, 0.2f, 0.5f}, {0.2f, 1.0f, 0.2f, 0.5f}, {0.2f, 0.2f, 1.0f, 0.5f}};
            for (int i = 0; i < 3; i++){
                Primitive& glass = add();
                glass.createPlane(2.5f, 2.5f);
                glass.setColor(colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
                glass.setTransparent(true);
                glass.setRotation(70.0f, 0.0f, 0.0f);
                glass.setPosition(-1.0f + 0.9f * (float)i, 2.0f, 4.0f + 0.4f * (float)i);
            }
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        // render(), then the frames until a pass of that label runs: SSAO's full-screen shaders build
        // in the background, and its depth pre-pass starts with them, not with the meshes
        bool renderWithPass(const char* label){
            if (!render())
                return false;
            return GpuEnvironment::runFrames([label](){ return passCounts(label).found; });
        }

        // the frame's passes of a label (prefix "shadow": the shadow atlas, its static layer and
        // the point atlas), their draws and instances
        struct PassCounts { uint32_t draws = 0; uint32_t instances = 0; uint32_t viewTag = 0; bool found = false; };
        static PassCounts passCounts(const char* label){
            PassCounts counts;
            const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
            for (const PassRecord& pass : list.passes){
                if (!pass.executed || std::strcmp(pass.label, label) != 0)
                    continue;
                counts.draws += pass.draws;
                counts.instances += pass.instances;
                counts.viewTag = pass.viewTag;
                counts.found = true;
            }
            return counts;
        }

        // an attachment of the frame's first pass of that label (the Frame Debugger's preview):
        // attachment 0..3 colour, 4 depth (shown as depth); empty when it never came. On Vulkan the
        // preview's first capture comes back black while its pipeline is being made, ready all the
        // same: a capture counts once it holds something, and these passes all draw or clear above 0
        static std::vector<uint8_t> passImage(const char* label, int attachment){
            std::vector<uint8_t> rgba;
            for (int attempt = 0; attempt < 120; attempt++){
                rgba = capturePassImage(label, attachment);
                for (size_t i = 0; i < rgba.size(); i += 4){
                    if (rgba[i] != 0 || rgba[i + 1] != 0 || rgba[i + 2] != 0)
                        return rgba;
                }
                GpuEnvironment::drawFrames(1);
            }
            return rgba;
        }

        static std::vector<uint8_t> capturePassImage(const char* label, int attachment){
            const PassCounts counts = passCounts(label);
            PassPreviewRequest request;
            request.viewTag = counts.viewTag;
            request.label = label;
            request.attachment = attachment;
            request.mode = attachment == 4 ? 2 : 0;
            request.maxSize = 512;
            const uint64_t serial = SystemRender::getPassPreview().serial;
            SystemRender::requestPassPreview(request);
            std::vector<uint8_t> rgba;
            for (int frame = 0; frame < 30; frame++){
                GpuEnvironment::drawFrames(1);
                const PassPreviewResult result = SystemRender::getPassPreview();
                if (result.serial == serial)
                    continue;
                EXPECT_TRUE(result.ready) << label << ": " << result.reason;
                if (result.ready && SystemRender::getPassPreviewTexture())
                    readTexture(*SystemRender::getPassPreviewTexture(), result.width, result.height, rgba);
                break;
            }
            SystemRender::cancelPassPreview();
            return rgba;
        }

        static size_t differentPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
            if (a.size() != b.size())
                return a.size() + b.size();
            size_t different = 0;
            for (size_t i = 0; i < a.size(); i += 4){
                if (std::memcmp(&a[i], &b[i], 3) != 0)
                    different++;
            }
            return different;
        }

        // The frames compare exactly, except on Metal. There each program is compiled on its own
        // (fast math or not), and the world position and normal a batch variant computes from
        // vertex attributes round a few ULP away from its base's, which reads the same matrices
        // (bit for bit: packInstance) from a uniform block. The clip position is
        // [[invariant]] and stays exact (the shadow atlas and depth pre-pass compare exactly
        // everywhere), but those few ULP move a shadow comparison on a curved mesh's terminator: on
        // an M4 Max, 62-69 isolated pixels of the spheres' shadowed sides, at most 9/255. So on
        // Metal a frame may differ in at most 1 pixel in 1000, by at most 16/255; a real
        // difference (a wrong matrix, a lost instance) changes thousands of pixels or more.
        static void expectSameFrame(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const char* what){
            ASSERT_EQ(a.size(), b.size()) << what;
            size_t different = 0;
            int largest = 0;
            for (size_t i = 0; i < a.size(); i += 4){
                int pixel = 0;
                for (size_t c = 0; c < 3; c++)
                    pixel = std::max(pixel, std::abs((int)a[i + c] - (int)b[i + c]));
                different += pixel > 0 ? 1 : 0;
                largest = std::max(largest, pixel);
            }
            std::printf("[instancing] %s: %zu pixels differ, by at most %d/255\n", what, different, largest);
#if defined(SOKOL_METAL)
            EXPECT_LE(different, a.size() / 4 / 1000) << what;
            EXPECT_LE(largest, 16) << what;
#else
            EXPECT_EQ(different, 0u) << what;
#endif
        }

        static std::vector<uint8_t> readTarget(){
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }
    };

}

TEST_F(InstancingGoldenGpu, BatchedFrameMatchesUnbatched){
    JASPER_REQUIRE_GPU();
    buildScene();

    Engine::setGpuInstancingEnabled(false);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const std::vector<uint8_t> unbatched = readTarget();
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);

    // the meshes load again with their batch variants
    Engine::setGpuInstancingEnabled(true);
    ASSERT_TRUE(render()) << "the batch variants never finished loading";
    const std::vector<uint8_t> batched = readTarget();
    const DrawBatchStats stats = SystemRender::getDrawBatchStats();
    std::printf("[instancing] %u instanced draws for %u draws, %llu uniform bytes kept; %u offered, split by pipeline %u, "
                "elements %u, bindings %u (parts %x), uniforms %u (slots %x)\n", stats.instancedDraws, stats.batchedInstances,
                (unsigned long long)stats.uniformBytesKept, stats.offeredDraws, stats.splitPipeline, stats.splitElements,
                stats.splitBindings, stats.splitBindingParts, stats.splitUniforms, stats.splitUniformSlots);
    // what the first box loaded (a test that does not batch says why)
    const Submesh& first = scene.getComponent<MeshComponent>(models[0]->getEntity()).submeshes[0];
    std::printf("[instancing] box: batch shader %s (properties %llx), colour variant %s (%s), depth variant %s (%s)\n",
                first.batchShader ? (first.batchShader->isCreated() ? "built" : "not built") : "none",
                (unsigned long long)first.batchShaderProperties, first.batchRender.isBatchVariant() ? "linked" : "not linked",
                first.batchRender.getBatchLinkFailure() ? first.batchRender.getBatchLinkFailure() : "-",
                first.batchDepthRender.isBatchVariant() ? "linked" : "not linked",
                first.batchDepthRender.getBatchLinkFailure() ? first.batchDepthRender.getBatchLinkFailure() : "-");
    EXPECT_GT(stats.instancedDraws, 0u);
    // the 36 grid objects in the colour pass alone
    EXPECT_GE(stats.batchedInstances, 30u);

    ASSERT_EQ(batched.size(), unbatched.size());
    if (differentPixels(batched, unbatched) > 0){
        const int width = (int)GpuEnvironment::target().getWidth();
        const int height = (int)GpuEnvironment::target().getHeight();
        writePng(getOutputDirectory() + "/instancing_off.png", unbatched, width, height);
        writePng(getOutputDirectory() + "/instancing_on.png", batched, width, height);
    }
    expectSameFrame(unbatched, batched, "pixels that differ with GPU Instancing on");
    EXPECT_NO_ENGINE_ERRORS();
}

// A runtime clone of an instanced mesh holds none of the original's batch variants (their
// pipelines and shaders, which the clone's load and destroy let go of): it loads its own, and
// once it is destroyed the original still draws instanced, and the frame is the one before the
// clone (an early version of this test saw 421 pixels differ; see the spec's Follow-ups)
TEST_F(InstancingGoldenGpu, CloneOfAnInstancedMeshLeavesTheOriginalsBatches){
    JASPER_REQUIRE_GPU();
    buildScene();
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const std::vector<uint8_t> before = readTarget();
    const Entity original = models[0]->getEntity();
    ASSERT_TRUE(scene.getComponent<MeshComponent>(original).submeshes[0].batchRender.isBatchVariant());

    const std::vector<Entity> copies = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, original), &scene);
    ASSERT_FALSE(copies.empty());
    const Submesh& copied = scene.getComponent<MeshComponent>(copies[0]).submeshes[0];
    EXPECT_FALSE(copied.batchRender.isBatchVariant()) << "the clone holds the original's batch variant";
    EXPECT_FALSE(copied.batchDepthRender.isBatchVariant());
    EXPECT_FALSE(copied.batchShader);
    EXPECT_FALSE(copied.batchDepthShader);

    ASSERT_TRUE(render()) << "the clone never finished loading";
    EXPECT_TRUE(scene.getComponent<MeshComponent>(copies[0]).submeshes[0].batchRender.isBatchVariant());
    for (auto copy = copies.rbegin(); copy != copies.rend(); ++copy)
        scene.destroyEntity(*copy);
    ASSERT_TRUE(render());
    GpuEnvironment::drawFrames(2);

    EXPECT_TRUE(scene.getComponent<MeshComponent>(original).submeshes[0].batchRender.isBatchVariant());
    EXPECT_GT(SystemRender::getDrawBatchStats().instancedDraws, 0u);
    const std::vector<uint8_t> after = readTarget();
    const size_t different = differentPixels(before, after);
    if (different > 0){
        const int width = (int)GpuEnvironment::target().getWidth();
        const int height = (int)GpuEnvironment::target().getHeight();
        writePng(getOutputDirectory() + "/instancing_before_clone.png", before, width, height);
        writePng(getOutputDirectory() + "/instancing_after_clone.png", after, width, height);
    }
    EXPECT_EQ(different, 0u) << "pixels that differ once the clone is gone";
    EXPECT_NO_ENGINE_ERRORS();
}

// a Mesh that does not allow it draws alone, and the image is the same
TEST_F(InstancingGoldenGpu, MeshOptOutDrawsAlone){
    JASPER_REQUIRE_GPU();
    buildScene();
    for (std::unique_ptr<Primitive>& object : objects)
        object->setAllowGpuInstancing(false);
    for (std::unique_ptr<Model>& model : models)
        model->setAllowGpuInstancing(false);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// The shadow maps and the camera's depth passes batch too (R1): the sun's shadows redrawn every
// frame (no shadow cache to keep them from the measured frame), the camera's depth pre-pass and
// SSAO's depth pass on. With GPU Instancing on, those passes draw the same instances in fewer
// draws, and their images (the shadow atlas's depth, the depth pre-pass's packed depth) and the
// frame are the same pixel for pixel as with it off.
TEST_F(InstancingGoldenGpu, ShadowAndDepthPassesBatch){
    JASPER_REQUIRE_GPU();
    sun.setShadowUpdateMode(ShadowUpdateMode::EVERY_FRAME);
    scene.setDepthPrepassEnabled(true);
    scene.setSSAOEnabled(true);
    buildScene();

    Engine::setGpuInstancingEnabled(false);
    ASSERT_TRUE(renderWithPass("depth pre-pass")) << "the scene never finished loading, or the depth pre-pass never ran";
    const PassCounts shadowOff = passCounts("shadow");
    const PassCounts depthOff = passCounts("depth pre-pass");
    const std::vector<uint8_t> frameOff = readTarget();
    const std::vector<uint8_t> shadowImageOff = passImage("shadow", 4);
    // the control: the atlas read again, nothing changed
    const std::vector<uint8_t> shadowImageOffAgain = passImage("shadow", 4);
    const std::vector<uint8_t> depthImageOff = passImage("depth pre-pass", 0);

    Engine::setGpuInstancingEnabled(true);
    ASSERT_TRUE(renderWithPass("depth pre-pass")) << "the batch variants never finished loading, or the depth pre-pass never ran";
    const PassCounts shadowOn = passCounts("shadow");
    const PassCounts depthOn = passCounts("depth pre-pass");
    const std::vector<uint8_t> frameOn = readTarget();
    const std::vector<uint8_t> shadowImageOn = passImage("shadow", 4);
    const std::vector<uint8_t> depthImageOn = passImage("depth pre-pass", 0);
    std::printf("[instancing] shadow: %u draws (%u instances) off, %u (%u) on; depth pre-pass: %u (%u) off, %u (%u) on\n",
                shadowOff.draws, shadowOff.instances, shadowOn.draws, shadowOn.instances,
                depthOff.draws, depthOff.instances, depthOn.draws, depthOn.instances);

    ASSERT_TRUE(shadowOff.found && shadowOn.found) << "the frame drew no shadow pass";
    ASSERT_TRUE(depthOff.found && depthOn.found) << "the frame drew no depth pre-pass";
    // the 36 grid Models in each: fewer draws, the same instances
    EXPECT_LT(shadowOn.draws + 30, shadowOff.draws);
    EXPECT_EQ(shadowOn.instances, shadowOff.instances);
    EXPECT_GT(shadowOn.instances, shadowOn.draws);
    EXPECT_LT(depthOn.draws + 30, depthOff.draws);
    EXPECT_EQ(depthOn.instances, depthOff.instances);

    const auto describe = [](const char* name, const std::vector<uint8_t>& image){
        uint8_t low = 255;
        uint8_t high = 0;
        for (size_t i = 0; i < image.size(); i += 4){
            low = std::min(low, image[i]);
            high = std::max(high, image[i]);
        }
        std::printf("[instancing] %s: %zu bytes, red %u..%u\n", name, image.size(), (unsigned)low, (unsigned)high);
    };
    describe("shadow atlas off", shadowImageOff);
    describe("shadow atlas off, again", shadowImageOffAgain);
    describe("shadow atlas on", shadowImageOn);
    ASSERT_FALSE(shadowImageOff.empty());
    ASSERT_FALSE(depthImageOff.empty());
    EXPECT_EQ(differentPixels(shadowImageOff, shadowImageOffAgain), 0u) << "the atlas read twice, nothing changed: the readback does not hold";
    EXPECT_EQ(differentPixels(shadowImageOff, shadowImageOn), 0u) << "shadow atlas pixels that differ";
    EXPECT_EQ(differentPixels(depthImageOff, depthImageOn), 0u) << "depth pre-pass pixels that differ";
    expectSameFrame(frameOff, frameOn, "frame pixels that differ");
    EXPECT_NO_ENGINE_ERRORS();
}

// Light Mode Tiled (S57): the batch variants take the colour program's 'Til' with its bindings, and
// the frame is the same pixel for pixel.
TEST_F(InstancingGoldenGpu, BatchedFrameMatchesUnderTiledLights){
    JASPER_REQUIRE_GPU();
    buildScene();
    std::vector<std::unique_ptr<Light>> lights;
    for (int i = 0; i < 12; i++){
        lights.push_back(std::make_unique<Light>(&scene));
        Light& light = *lights.back();
        light.setType(LightType::POINT);
        light.setPosition(-5.0f + 2.0f * (float)(i % 4), 1.5f, -4.0f + 3.0f * (float)(i / 4));
        light.setColor((i % 3) == 0 ? 1.0f : 0.3f, (i % 3) == 1 ? 1.0f : 0.3f, (i % 3) == 2 ? 1.0f : 0.3f);
        light.setIntensity(8.0f);
        light.setRange(4.0f);
    }
    LightingTiers::setLightMode(LightMode::TILED);

    Engine::setGpuInstancingEnabled(false);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    ASSERT_TRUE(scene.getSystem<RenderSystem>()->getLightingStats().tiledLights);
    const std::vector<uint8_t> unbatched = readTarget();
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);

    Engine::setGpuInstancingEnabled(true);
    ASSERT_TRUE(render()) << "the batch variants never finished loading";
    ASSERT_TRUE(scene.getSystem<RenderSystem>()->getLightingStats().tiledLights);
    const std::vector<uint8_t> batched = readTarget();
    const DrawBatchStats stats = SystemRender::getDrawBatchStats();
    const Submesh& first = scene.getComponent<MeshComponent>(models[0]->getEntity()).submeshes[0];
    std::printf("[instancing] tiled: %u instanced draws for %u draws, batch properties %llx\n", stats.instancedDraws,
                stats.batchedInstances, (unsigned long long)first.batchShaderProperties);
    EXPECT_NE(first.batchShaderProperties & ShaderPool::getLightingProperty(ShaderPool::LightingFeature::TILED_LIGHTS), 0u);
    EXPECT_GT(stats.instancedDraws, 0u);
    EXPECT_GE(stats.batchedInstances, 30u);
    expectSameFrame(unbatched, batched, "pixels that differ with GPU Instancing on under Tiled lights");
    EXPECT_NO_ENGINE_ERRORS();
}

// A mesh mid-fade (S54 R6, contract C3) among draws that batch: it stays single draws on the fade
// variant, its dither and its levels the same pixel for pixel with GPU Instancing on and off, at the
// same fade time (a fixed frame delta).
TEST_F(InstancingGoldenGpu, FadingMeshAmongBatchesMatchesUnbatched){
    JASPER_REQUIRE_GPU();
    sun.setShadowUpdateMode(ShadowUpdateMode::EVERY_FRAME);
    scene.setMeshLodEnabled(true);
    scene.setMeshLodCrossFade(true);
    scene.setMeshLodCrossFadeDuration(0.5f);
    Engine::setFixedDeltatime(1.0 / 60.0);
    buildScene();
    Primitive sphere(&scene);
    sphere.createSphere(1.5f, 48, 24);
    sphere.setColor(0.85f, 0.55f, 0.2f);
    sphere.setPosition(0.0f, 2.5f, 3.0f);

    Engine::setGpuInstancingEnabled(false);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    MeshComponent& mesh = sphere.getComponent<MeshComponent>();
    MeshComponent* fading = &mesh;
    ASSERT_TRUE(GpuEnvironment::runFrames([fading](){ return fading->numLods > 2; })) << "the sphere got no levels";
    // a step of its LOD Bias under 2x (a bigger jump in a frame is a camera cut, which snaps) from
    // level 0 to level 1
    const CameraComponent& cameraComponent = camera.getComponent<CameraComponent>();
    const float distance = mesh.worldAABB.distance(Vector3(0.0f, 7.0f, 11.0f));
    const float budget = scene.getMeshLodThreshold() * 2.0f * std::tan(cameraComponent.yfov * 0.5f) /
                         (float)GpuEnvironment::target().getHeight() * distance;
    ASSERT_GT(mesh.lodError[2], 1.6f * mesh.lodError[1]) << "the step must reach level 1 and stay there";
    const float biasFine = budget / (0.8f * mesh.lodError[1]);
    const float biasCoarse = budget / (1.25f * mesh.lodError[1]);

    // the sphere steady at level 0, its fade variant made; 15 frames into a fade
    const auto midFade = [&](uint32_t* steady){
        sphere.setLodBias(biasFine);
        EXPECT_TRUE(render());
        EXPECT_TRUE(GpuEnvironment::runFrames([fading](){ return fading->lodFadeReady; }, 600)) << "the fade variant was never made";
        GpuEnvironment::drawFrames(40);
        EXPECT_EQ(sphere.getCurrentLod(), 0);
        *steady = mesh.submeshes[0].render.getAppliedPipelineId();
        sphere.setLodBias(biasCoarse);
        GpuEnvironment::drawFrames(15);
        return readTarget();
    };

    uint32_t steadyOff = 0;
    const std::vector<uint8_t> unbatched = midFade(&steadyOff);
    EXPECT_EQ(scene.getSystem<RenderSystem>()->getLodStats().fadeDraws, 2u);
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);

    Engine::setGpuInstancingEnabled(true);
    uint32_t steadyOn = 0;
    const std::vector<uint8_t> batched = midFade(&steadyOn);
    const LodFrameStats lod = scene.getSystem<RenderSystem>()->getLodStats();
    const DrawBatchStats stats = SystemRender::getDrawBatchStats();
    std::printf("[instancing] mid-fade: %u fading, %u fade draws, %u instanced draws for %u draws\n", lod.fading, lod.fadeDraws,
                stats.instancedDraws, stats.batchedInstances);
    EXPECT_EQ(lod.fading, 1u);
    EXPECT_EQ(lod.fadeDraws, 2u) << "the fading mesh's two levels, single draws";
    EXPECT_NE(mesh.submeshes[0].render.getAppliedPipelineId(), steadyOn) << "a fading draw uses the fade variant";
    EXPECT_GT(stats.instancedDraws, 0u);
    EXPECT_GE(stats.batchedInstances, 30u);
    expectSameFrame(unbatched, batched, "pixels that differ mid-fade with GPU Instancing on");
    EXPECT_NO_ENGINE_ERRORS();
}

// Shadow maps cast about one detail level coarser (S54's SHADOW_LOD_COARSER, at the default Shadow LOD
// Bias 1): that factor is the error a level may have, and a level is the last one of the chain within
// that error (selectMeshLod). What that is in levels depends on how fast a generated chain's error
// grows: measured here across the range of distances where the choice changes.
TEST_F(InstancingGoldenGpu, ShadowLodCoarserIsOneLevel){
    JASPER_REQUIRE_GPU();
    scene.setMeshLodEnabled(true);
    // (a sphere of many triangles: S54's recipe stops a small mesh's chain early)
    Primitive sphere(&scene);
    sphere.createSphere(1.5f, 48, 24);
    const MeshComponent& mesh = sphere.getComponent<MeshComponent>();
    const MeshComponent* loaded = &mesh;
    ASSERT_TRUE(GpuEnvironment::runFrames([loaded](){ return loaded->numLods > 2; })) << "the sphere got no chain of levels: " << mesh.numLods;

    std::printf("[instancing] sphere: %u levels, errors", mesh.numLods);
    for (unsigned int l = 0; l < mesh.numLods; l++)
        std::printf(" %g", mesh.lodError[l]);
    std::printf("\n");
    ASSERT_GE(mesh.numLods, 3u);

    const auto levelWithin = [&mesh](float allowed){
        unsigned int level = 0;
        for (unsigned int l = 1; l < mesh.numLods; l++){
            if (mesh.lodError[l] > allowed)
                break;
            level = l;
        }
        return level;
    };
    // log-spaced, from where the first level comes in to where the last one has
    const float first = mesh.lodError[1];
    const float last = mesh.lodError[mesh.numLods - 1];
    ASSERT_GT(first, 0.0f);
    constexpr int STEPS = 256;
    constexpr float COARSER = 2.0f;   // RenderSystemShared.h's SHADOW_LOD_COARSER
    float coarser = 0.0f;
    for (int i = 0; i < STEPS; i++){
        const float allowed = first * std::pow(last / first, (float)i / (float)(STEPS - 1));
        coarser += (float)levelWithin(allowed * COARSER) - (float)levelWithin(allowed);
    }
    coarser /= (float)STEPS;
    std::printf("[instancing] a shadow map's budget: %.2f levels coarser on average\n", coarser);
    EXPECT_GE(coarser, 0.5f);
    EXPECT_LE(coarser, 1.5f);
    EXPECT_NO_ENGINE_ERRORS();
}
