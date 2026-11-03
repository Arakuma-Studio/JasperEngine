// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// main() of every googletest target (jasperengine_add_test_target compiles this file into each,
// with the target's JASPER_TEST_* directories). The engine is initialised without a GPU, so CPU
// suites run without a display; GPU suites call GpuEnvironment::ensure() (JASPER_REQUIRE_GPU)
// and run under Xvfb (tools/build.sh unit-tests, htmlui-tests).

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "TestPaths.h"
#if defined(JASPER_HTMLUI_TEST_SOURCE_DIR)
#include "media/VideoSourceFactory.h"
#endif

#include <gtest/gtest.h>

#include <cstdlib>

#if !defined(JASPER_TEST_SOURCE_DIR) || !defined(JASPER_TEST_BINARY_DIR) || !defined(JASPER_TEST_ASSET_DIR) || !defined(JASPER_TEST_OUTPUT_DIR)
#error "TestMain.cpp is compiled by jasperengine_add_test_target, which defines the JASPER_TEST_* directories"
#endif

namespace {

    // the start of each test, for EXPECT_NO_ENGINE_ERRORS
    class LogMarker : public ::testing::EmptyTestEventListener {
    public:
        void OnTestStart(const ::testing::TestInfo&) override{
            jasperengine::test::LogCapture::markTestStart();
        }
    };

    void setDefaultEnv(const char* name, const char* value){
#if defined(_WIN32)
        if (!std::getenv(name))
            _putenv_s(name, value);
#else
        setenv(name, value, 0);
#endif
    }

}

int main(int argc, char** argv){
    ::testing::InitGoogleTest(&argc, argv);

    jasperengine::test::TestPaths paths;
    paths.sourceDir = JASPER_TEST_SOURCE_DIR;
    paths.binaryDir = JASPER_TEST_BINARY_DIR;
    paths.assetDir = JASPER_TEST_ASSET_DIR;
    paths.goldenDir = JASPER_TEST_SOURCE_DIR "/golden";
    paths.outputDir = JASPER_TEST_OUTPUT_DIR;
    jasperengine::test::setTestPaths(paths);

    // the engine's Lua state used off its thread aborts the run (LuaBinding::ThreadCheck)
    setDefaultEnv("JASPER_LUA_THREAD_CHECK", "abort");
#if defined(JASPER_TEST_SYNC_GPU_CREATION)
    // a suite that drives its renderer frame by frame (the HTML UI tests' RenderTestContext):
    // shaders and pipelines are created at once, as before spec S32, so a frame's draws and the
    // layout a render computes do not wait for creations off the render thread. Asynchronous
    // creation is the core suite's (AsyncCreateGpu) and the MCP suite's. JASPER_ASYNC_SHADERS set
    // by hand still wins
    setDefaultEnv("JASPER_ASYNC_SHADERS", "sync");
#endif

    jasperengine::test::GpuEnvironment::initEngine(argc, argv);
#if defined(JASPER_HTMLUI_TEST_SOURCE_DIR)
    // jasperengine-htmlui-tests (upstream's engine/tests/htmlui/main.cpp): the video suites'
    // world is the WebM path's alone (the same on every machine): MP4 and the platform's decoders
    // are the editor tests' (VideoMp4Test)
    jasperengine::VideoSourceFactory::setPlatformDecodersEnabledForTesting(false);
#endif
    ::testing::AddGlobalTestEnvironment(new jasperengine::test::GpuTestEnvironment());
    ::testing::UnitTest::GetInstance()->listeners().Append(new LogMarker());

    return RUN_ALL_TESTS();
}
