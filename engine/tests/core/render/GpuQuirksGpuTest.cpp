// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A GPU quirk changes how a frame is made, never what it shows (docs/specs/release-performance-and-
// gpu-quirks.md R20, R27, AC20, AC23): a lever forced off and on draws the LitCube golden both ways
// after creation is idle, and the same comparison fails for the test-only lever that changes the
// image (the method's red control).

#include "Engine.h"
#include "Scene.h"
#include "device/GpuQuirks.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/SystemRender.h"
#include "sokol/SokolAsyncCreate.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace GpuQuirksGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    void setQuirksGpuEnv(const char* name, const char* value){
    #if defined(_WIN32)
        _putenv_s(name, value ? value : "");
    #else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    #endif
    }

    // the device's quirks with these forced, and the creation set up again in `mode`
    void forceQuirks(const std::vector<GpuQuirkOverride>& overrides, const char* mode){
        GpuQuirks::install(GpuQuirks::resolve(GpuQuirks::runningDevice(), GpuQuirks::builtInRules(), overrides));
        SokolAsyncCreate::shutdown();
        setQuirksGpuEnv("JASPER_ASYNC_SHADERS", mode);
        SokolAsyncCreate::setup();
    }

    class GpuQuirksGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 3.0f, 5.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            ASSERT_TRUE(SystemRender::waitGpuCreationIdle(30000));
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            // the process's own set and creation again
            forceQuirks(GpuQuirks::environmentOverrides(), nullptr);
            setQuirksGpuEnv("JASPER_ASYNC_SHADERS", nullptr);
        }

        // the LitCube, drawn once creation is idle; its frame (compared with the golden when asked)
        std::vector<uint8_t> drawLitCube(bool golden = true){
            scene.setLitQuality(LitQuality::STANDARD);
            std::vector<uint8_t> rgba;
            {
                Primitive cube(&scene);
                Light sun(&scene);
                cube.createBox(1.5f, 1.5f, 1.5f);
                cube.setColor(0.2f, 0.6f, 0.9f);
                cube.setRotation(0.0f, 30.0f, 0.0f);
                sun.setType(LightType::DIRECTIONAL);
                sun.setDirection(-0.4f, -1.0f, -0.6f);
                sun.setIntensity(1.5f);
                sun.setShadows(false);
                Scene* current = &scene;
                EXPECT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
                EXPECT_TRUE(SystemRender::waitGpuCreationIdle(30000));
                GpuEnvironment::drawFrames(1);
                Framebuffer& target = GpuEnvironment::target();
                readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
                if (golden) {
                    EXPECT_GOLDEN("render_lit_cube");
                }
            }
            // the cube's programs go, so the next pass makes its own
            GpuEnvironment::drawFrames(2);
            ShaderPool::clearUnused();
            return rgba;
        }

        static int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
            if (a.size() != b.size() || a.empty())
                return 255;
            int largest = 0;
            for (size_t i = 0; i + 3 < a.size(); i += 4)
                for (int c = 0; c < 3; c++)
                    largest = std::max(largest, std::abs((int)a[i + c] - (int)b[i + c]));
            return largest;
        }
    };

}

// AC20: gl_no_compile_threads forced on and off in parallel mode, both frames the LitCube golden
TEST_F(GpuQuirksGpu, CompileThreadsLeverDrawsTheLitCube){
    JASPER_REQUIRE_GPU();
    // on first: a driver keeps its compile threads once turned on, until the lever turns them off
    forceQuirks({{GpuQuirk::GlNoCompileThreads, true}}, "parallel");
    const bool parallel = SokolAsyncCreate::getMode() == GpuCreationMode::GlParallel;
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    EXPECT_TRUE(parallel) << "llvmpipe lists KHR_parallel_shader_compile";
#else
    EXPECT_FALSE(parallel) << "the lever is GL's: no parallel mode here";
#endif
    EXPECT_FALSE(SokolAsyncCreate::compileThreadsEnabled());
    const std::vector<uint8_t> with = drawLitCube();

    forceQuirks({{GpuQuirk::GlNoCompileThreads, false}}, "parallel");
    EXPECT_EQ(SokolAsyncCreate::compileThreadsEnabled(), parallel) << "the lever applies in GL's parallel mode only";
    const std::vector<uint8_t> without = drawLitCube();
    EXPECT_LE(maxDifference(with, without), 2) << "a quirk changes how a frame is made, never what it shows";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC23: the comparison fails for a lever that changes the image (test_tint, the clear colour)
TEST_F(GpuQuirksGpu, EquivalenceCheckCatchesAVisibleChange){
    JASPER_REQUIRE_GPU();
    GpuQuirk tint;
    ASSERT_TRUE(GpuQuirks::fromName("test_tint", tint)) << "test builds know the test lever";
    forceQuirks({}, nullptr);
    const std::vector<uint8_t> plain = drawLitCube(false);
    forceQuirks({{tint, true}}, nullptr);
    EXPECT_TRUE(GpuQuirks::active(GpuQuirk::TestTint));
    const std::vector<uint8_t> tinted = drawLitCube(false);
    EXPECT_GT(maxDifference(plain, tinted), 50) << "the method sees a visible change";
    forceQuirks({}, nullptr);
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace GpuQuirksGpuTest_cpp
