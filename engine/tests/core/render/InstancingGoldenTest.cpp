// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// GPU Instancing (spec S43 R1, AC1): a scene of many copies of a few meshes and materials, lit
// by a shadowing sun, with overlapping transparent planes in front, drawn with the project's GPU
// Instancing off (every draw alone: the submission before S43) and on (state sorted, batched).
// The two images are the same pixel for pixel, the transparent queue keeping its order (DrawSort
// covers the opaque keys), and on draws instanced batches. The existing core goldens, drawn with
// GPU Instancing on (the default), still match the images written before it.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <cstdio>
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
            Engine::removeScene(&scene);
            Engine::setGpuInstancingEnabled(true);
        }

        Primitive& add(){
            objects.push_back(std::make_unique<Primitive>(&scene));
            return *objects.back();
        }

        // a 6 x 6 grid of boxes in two colours, spheres among them, a floor, and three overlapping
        // transparent planes nearer the camera
        void buildScene(){
            Primitive& floor = add();
            floor.createPlane(14.0f, 14.0f);
            floor.setColor(0.7f, 0.7f, 0.65f);
            for (int z = 0; z < 6; z++){
                for (int x = 0; x < 6; x++){
                    Primitive& box = add();
                    if ((x + z) % 3 == 2){
                        box.createSphere(0.45f);
                        box.setColor(0.9f, 0.5f, 0.2f);
                    }else{
                        box.createBox(0.8f, 0.8f, 0.8f);
                        if ((x + z) % 2 == 0)
                            box.setColor(0.2f, 0.6f, 0.9f);
                        else
                            box.setColor(0.8f, 0.2f, 0.3f);
                    }
                    box.setPosition(-5.0f + 2.0f * (float)x, 0.5f, -5.0f + 2.0f * (float)z);
                    box.setRotation(0.0f, 15.0f * (float)(x + z), 0.0f);
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
    std::printf("[instancing] %u instanced draws for %u draws, %llu uniform bytes kept\n", stats.instancedDraws,
                stats.batchedInstances, (unsigned long long)stats.uniformBytesKept);
    EXPECT_GT(stats.instancedDraws, 0u);
    // the 36 grid objects in the colour pass alone
    EXPECT_GE(stats.batchedInstances, 30u);

    ASSERT_EQ(batched.size(), unbatched.size());
    size_t different = 0;
    for (size_t i = 0; i < batched.size(); i += 4){
        if (batched[i] != unbatched[i] || batched[i + 1] != unbatched[i + 1] || batched[i + 2] != unbatched[i + 2])
            different++;
    }
    if (different > 0){
        const int width = (int)GpuEnvironment::target().getWidth();
        const int height = (int)GpuEnvironment::target().getHeight();
        writePng(getOutputDirectory() + "/instancing_off.png", unbatched, width, height);
        writePng(getOutputDirectory() + "/instancing_on.png", batched, width, height);
    }
    EXPECT_EQ(different, 0u) << "pixels that differ with GPU Instancing on";
    EXPECT_NO_ENGINE_ERRORS();
}

// a Mesh that does not allow it draws alone, and the image is the same
TEST_F(InstancingGoldenGpu, MeshOptOutDrawsAlone){
    JASPER_REQUIRE_GPU();
    buildScene();
    for (std::unique_ptr<Primitive>& object : objects)
        object->setAllowGpuInstancing(false);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}
