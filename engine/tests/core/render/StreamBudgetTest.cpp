// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The stream upload budget (spec S55 R6, AC5): an upload larger than the whole per-frame stream
// budget (Vulkan's stream staging buffer) is made alone in a frame of its own, and lands within
// two frames; before S55 it was refused every frame, and the instances it held never drew. The
// budget is lowered (and enforced on every backend) for the test: an instance view over it.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "sokol/SokolSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class StreamBudgetGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 0.0f, 40.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            SokolSystem::restoreBudgets();
            Engine::removeScene(&scene);
        }

        // pixels of the target brighter than the black background
        static size_t litPixels(){
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            size_t lit = 0;
            for (size_t i = 0; i < rgba.size(); i += 4)
                lit += (rgba[i] > 40 || rgba[i + 1] > 40 || rgba[i + 2] > 40) ? 1 : 0;
            return lit;
        }
    };

}

TEST_F(StreamBudgetGpu, OversizedInstanceViewLandsWithinTwoFrames){
    JASPER_REQUIRE_GPU();
    // an unlit instanced box, no instances yet
    Primitive boxes(&scene);
    boxes.createBox(0.5f, 0.5f, 0.5f);
    boxes.setColor(1.0f, 1.0f, 1.0f);
    boxes.setReceiveLights(false);
    boxes.createInstancedMesh();
    boxes.setMaxInstances(4000);
    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){
        return current->getSystem<RenderSystem>()->isAllLoaded();
    }));
    ASSERT_EQ(litPixels(), 0u);

    // 3,600 instances (96 bytes each: about 340 KB a list) against a 64 KB budget
    SokolSystem::overrideBudget(SokolSystem::STREAM_BUDGET, 64 * 1024, true);
    for (int y = 0; y < 60; y++){
        for (int x = 0; x < 60; x++)
            boxes.addInstance((float)x * 0.6f - 18.0f, (float)y * 0.6f - 18.0f, 0.0f);
    }
    const uint64_t skippedBefore = SystemRender::getStreamSkippedUploads();
    // the batch's own buffer goes alone in the first frame, its culled list alone in the second
    GpuEnvironment::drawFrames(2);
    const size_t lit = litPixels();
    EXPECT_GT(lit, 1000u) << "the instances did not draw within 2 frames";
    // and they keep drawing, nothing waiting any more
    GpuEnvironment::drawFrames(2);
    EXPECT_GE(litPixels(), lit);
    const uint64_t skippedAfter = SystemRender::getStreamSkippedUploads();
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(SystemRender::getStreamSkippedUploads(), skippedAfter) << "uploads still refused";
    EXPECT_GE(skippedAfter, skippedBefore);
    // an upload that waits for its frame is late, not lost: the warning says so, no error
    EXPECT_NO_ENGINE_ERRORS();
}
