// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_GPU_ENVIRONMENT_H
#define JASPER_TEST_GPU_ENVIRONMENT_H

#include "Engine.h"
#include "texture/Framebuffer.h"

#include <gtest/gtest.h>

#include <functional>

namespace jasperengine { namespace test {

    // The engine as the tests run it. main() (TestMain.cpp) calls initEngine
    // (Engine::systemInit with the TestSystem, no GPU), so CPU-only suites need no display.
    // The first test that calls ensure() creates the headless device and loads the view, once
    // per process: frames render into target(), an engine Framebuffer set with
    // Engine::setFramebuffer. GPU suites run in the standalone engine tree on Linux (Xvfb,
    // Mesa llvmpipe or lavapipe) and macOS (Metal); elsewhere ensure() fails with
    // unavailableReason().
    class GpuEnvironment {
    public:
        static void initEngine(int argc, char** argv);

        // false (and the reason on stderr) when no display or device is available
        static bool ensure();
        static bool isReady();
        // why ensure() failed, or would fail in this build
        static const char* unavailableReason();

        // screen = the target size in pixels, canvas = the preferred canvas size
        static void setView(int screenWidth, int screenHeight, int canvasWidth, int canvasHeight, Scaling scaling);

        // Engine::systemDraw until ready() holds, no shader or pipeline is pending (spec S32) and
        // no mesh waits for its detail levels (spec S54), then 2 more frames; false if it never did within maxFrames frames and a minute
        static bool runFrames(std::function<bool()> ready, int maxFrames = 300);
        static void drawFrames(int count);

        static Framebuffer& target();

        // The goldens' renderer: Mesa's llvmpipe (glcore) or lavapipe (vulkan), the test image's
        // software devices. Strict image tolerances hold there; a hardware GPU may take the looser
        // per-test ones its measurements set (ImageCompare's hardwareTolerance). True before a
        // device exists (nothing is compared then).
        static bool isReferenceRenderer();

        // systemViewDestroyed, systemShutdown, device and display (gtest TearDown)
        static void shutdown();
    };

    // gtest global environment: shuts the engine down after the last test
    class GpuTestEnvironment : public ::testing::Environment {
    public:
        void TearDown() override;
    };

} }

// stops the test when there is no GPU context (the reason is printed)
#define JASPER_REQUIRE_GPU() ASSERT_TRUE(::jasperengine::test::GpuEnvironment::ensure()) << "no headless GPU context"

#endif //JASPER_TEST_GPU_ENVIRONMENT_H
