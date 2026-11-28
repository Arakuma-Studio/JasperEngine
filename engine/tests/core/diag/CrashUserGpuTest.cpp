// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A user report's capture through the engine's frames (docs/specs/shipped-game-diagnostics.md
// R11-R13, AC11): Engine::systemDraw reads the frame Crash.beginReport asked for, but not while
// shaders or pipelines are being made (a stand-in would be in the image): it reads the next one,
// for up to 30 frames, then fails.

#include "Engine.h"
#include "Scene.h"
#include "diag/UserReport.h"
#include "object/Camera.h"

#include "GpuEnvironment.h"

#include "stb_image.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace CrashUserGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the test's stand-in for "a pipeline is still being created"
    bool creationHeld = false;

    bool heldProbe() {
        return !creationHeld;
    }

    class CrashUserGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override {
            camera.activate();
            scene.setBackgroundColor(1.0f, 0.0f, 0.0f, 1.0f);
            Engine::setScene(&scene);
            diag::userreport::setCreationIdleProbeForTesting(&heldProbe);
            creationHeld = false;
        }

        void TearDown() override {
            diag::userreport::setCreationIdleProbeForTesting(nullptr);
            diag::userreport::reset();
            Engine::removeScene(&scene);
        }
    };

}

TEST_F(CrashUserGpu, CaptureRetriesWhilePipelinesArePending) {
    JASPER_REQUIRE_GPU();
    GpuEnvironment::drawFrames(3);

    bool called = false, ok = false;
    std::string error;
    auto callback = [&](bool shot, const std::string& why) {
        called = true;
        ok = shot;
        error = why;
    };

    // held for 5 frames: no frame is read meanwhile
    creationHeld = true;
    diag::userreport::begin(callback);
    GpuEnvironment::drawFrames(5);
    EXPECT_FALSE(called);
    EXPECT_TRUE(diag::userreport::wantsCapture()) << "still waiting for a frame without stand-ins";

    // then the first frame without one is read, and encoded on a worker: frames until the callback
    // comes, however long the encode job waits for a worker on a loaded machine
    creationHeld = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!called && std::chrono::steady_clock::now() < until) GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(called) << "the callback never ran";
    EXPECT_TRUE(ok) << error;
    ASSERT_TRUE(diag::userreport::draftHasScreenshot());

    // the draft is the red frame (createReport writes it as screenshot.jpg)
    diag::userreport::Options options;
    options.note = "x";
    std::string createError;
    // no handler in this process: the report stays in memory
    const auto report = diag::userreport::create(options, createError);
    ASSERT_TRUE(report) << createError;
    const std::optional<std::string> jpeg = diag::crash::readReportFile(report->id, "screenshot.jpg");
    ASSERT_TRUE(jpeg);
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(jpeg->data()), static_cast<int>(jpeg->size()),
                                                  &width, &height, &channels, 3);
    ASSERT_NE(pixels, nullptr);
    EXPECT_LE(width, 1280);
    const unsigned char* centre = pixels + (static_cast<size_t>(height / 2) * width + width / 2) * 3;
    EXPECT_GE(centre[0], 255 - 24);
    EXPECT_LE(centre[1], 24);
    EXPECT_LE(centre[2], 24);
    stbi_image_free(pixels);
    diag::crash::markHandled(report->id);

    // creation that never finishes: the capture fails after 30 frames
    called = false;
    creationHeld = true;
    diag::userreport::begin(callback);
    GpuEnvironment::drawFrames(diag::userreport::kMaxCaptureFrames - 1);
    EXPECT_FALSE(called) << "29 frames still wait";
    GpuEnvironment::drawFrames(3);
    ASSERT_TRUE(called);
    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "screenshot_unavailable: shaders still building");
    EXPECT_FALSE(diag::userreport::draftHasScreenshot());
}

}  // namespace CrashUserGpuTest_cpp
