// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Constants once per pass (spec S55 R3, AC3): on a fixed scene, the uniform bytes a frame applies
// with GPU Instancing on (a pipeline applied once while it stays, a block applied again only when
// its bytes changed) are at least 60% fewer than with it off, the submission before S55 (the red
// count). The scene's cubes each have their own colour, so none batch: only the constant blocks
// (frame, fog, lights, shadows) are saved.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/environment/Fog.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class UniformBytesGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        Fog fog{&scene};
        std::vector<std::unique_ptr<Primitive>> cubes;

        void SetUp() override{
            scene.setBackgroundColor(0.1f, 0.1f, 0.1f, 1.0f);
            fog.setType(FogType::EXPONENTIAL);
            fog.setDensity(0.01f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 8.0f, 14.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.3f, -1.0f, -0.4f);
            sun.setIntensity(1.5f);
            sun.setShadows(true);
            for (int i = 0; i < 64; i++){
                cubes.push_back(std::make_unique<Primitive>(&scene));
                Primitive& cube = *cubes.back();
                cube.createBox(0.7f, 0.7f, 0.7f);
                cube.setColor(0.2f + 0.01f * (float)i, 0.5f, 0.9f - 0.01f * (float)i);
                cube.setPosition(-7.0f + 2.0f * (float)(i % 8), 0.35f, -7.0f + 2.0f * (float)(i / 8));
            }
            Engine::setScene(&scene);
        }

        void TearDown() override{
            cubes.clear();
            Engine::removeScene(&scene);
            Engine::setGpuInstancingEnabled(true);
        }

        // the uniform bytes of a settled frame (each block as the frame's uniform buffer takes it)
        uint64_t frameBytes(){
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            }));
            GpuEnvironment::drawFrames(2);
            return SystemRender::getUniformBytesLastFrame();
        }
    };

}

TEST_F(UniformBytesGpu, ConstantsAreAppliedOncePerPass){
    JASPER_REQUIRE_GPU();
    Engine::setGpuInstancingEnabled(false);
    const uint64_t before = frameBytes();
    Engine::setGpuInstancingEnabled(true);
    const uint64_t after = frameBytes();
    std::printf("[uniform bytes] GPU Instancing off %llu, on %llu (%.1f%% fewer), kept %llu\n", (unsigned long long)before,
                (unsigned long long)after, before ? 100.0 * (1.0 - (double)after / (double)before) : 0.0,
                (unsigned long long)SystemRender::getDrawBatchStats().uniformBytesKept);
    ASSERT_GT(before, 0u);
    // (none of them batches: the saving is the constant blocks')
    EXPECT_EQ(SystemRender::getDrawBatchStats().instancedDraws, 0u);
    EXPECT_LE(after * 10, before * 4) << "less than 60% fewer uniform bytes";
    EXPECT_NO_ENGINE_ERRORS();
}
