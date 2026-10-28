// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_GPU_ENVIRONMENT_H
#define HTMLUI_TEST_GPU_ENVIRONMENT_H

#include "Engine.h"
#include "texture/Framebuffer.h"

#include <gtest/gtest.h>

#include <functional>

namespace jasperengine { namespace test {

    // The engine as the tests run it. main() calls initEngine (Engine::systemInit with the
    // TestSystem, no GPU), so CPU-only suites need no display. The first test that calls
    // ensure() creates the headless device and loads the view, once per process: frames
    // render into target(), an engine Framebuffer set with Engine::setFramebuffer.
    class GpuEnvironment {
    public:
        static void initEngine(int argc, char** argv);

        // false (and the reason on stderr) when no display or device is available
        static bool ensure();
        static bool isReady();

        // screen = the target size in pixels, canvas = the preferred canvas size
        static void setView(int screenWidth, int screenHeight, int canvasWidth, int canvasHeight, Scaling scaling);

        // Engine::systemDraw until ready() holds, then 2 more frames; false if it never did
        static bool runFrames(std::function<bool()> ready, int maxFrames = 300);
        static void drawFrames(int count);

        static Framebuffer& target();

        // systemViewDestroyed, systemShutdown, device and display (main, after RUN_ALL_TESTS)
        static void shutdown();
    };

} }

// stops the test when there is no GPU context (the reason is printed)
#define JASPER_REQUIRE_GPU() ASSERT_TRUE(::jasperengine::test::GpuEnvironment::ensure()) << "no headless GPU context"

#endif //HTMLUI_TEST_GPU_ENVIRONMENT_H
