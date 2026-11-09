// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The screen transition on the GPU (S48 R12, R13, R17, R32, AC12): the Engine's overlay pass over
// a black scene in the headless target (glcore; Vulkan on lavapipe with GRAPHIC_BACKEND=vulkan),
// read back and compared with ScreenTransition::evaluate, the CPU twin of transition.frag, for
// every kind at coverage 0.25, 0.5 and 0.9 and pixel sizes 1 and 3: every block of screen pixels
// of one art pixel is one colour, the image is the twin's, coverage 1 is all ink, coverage 0 draws
// nothing, and the program exists only once a transition drew.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "pixelart/ScreenTransition.h"
#include "texture/Framebuffer.h"

#include "GpuEnvironment.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::pixelart;

namespace {

    const TransitionKind kGpuKinds[] = {TransitionKind::IRIS, TransitionKind::BLINDS, TransitionKind::SLASH, TransitionKind::PIXEL_DISSOLVE,
                                        TransitionKind::CRT_OFF, TransitionKind::BURN_AWAY, TransitionKind::FADE};

    class ScreenTransitionGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::resetView();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setOrtho(-320.0f, 320.0f, -180.0f, 180.0f, -100.0f, 100.0f);
            camera.activate();
            Engine::setScene(&scene);
            ScreenTransition::reset();
            ScreenTransition::setEngineDrawEnabled(true);
        }

        void TearDown() override{
            ScreenTransition::reset();
            Engine::removeScene(&scene);
        }

        std::vector<uint8_t> drawn(int& width, int& height){
            GpuEnvironment::runFrames([]{ return ScreenTransition::isProgramCreated() || !ScreenTransition::needsDraw(); }, 600);
            GpuEnvironment::drawFrames(2);
            Framebuffer& target = GpuEnvironment::target();
            width = int(target.getWidth());
            height = int(target.getHeight());
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            return rgba;
        }
    };

}

TEST_F(ScreenTransitionGpu, MatchesTheCpuTwinWithHardPixels){
    int largestDifference = 0;
    for (TransitionKind kind : kGpuKinds){
        for (int pixelSize : {1, 3}){
            for (float coverage : {0.25f, 0.5f, 0.9f}){
                TransitionOptions options;
                options.pixelSize = pixelSize;
                options.seed = 5;
                ScreenTransition::setPreview(kind, options, coverage, TransitionPhase::COVER);
                int width = 0, height = 0;
                const std::vector<uint8_t> rgba = drawn(width, height);
                ASSERT_EQ(rgba.size(), size_t(width) * size_t(height) * 4);
                const int artWidth = (width + pixelSize - 1) / pixelSize, artHeight = (height + pixelSize - 1) / pixelSize;
                TransitionImage image;
                ScreenTransition::evaluate(kind, options, coverage, TransitionPhase::COVER, artWidth, artHeight, image);
                const std::string what = std::string(transitionKindName(kind)) + " pixel size " + std::to_string(pixelSize) +
                                         " at " + std::to_string(coverage);
                int blocksBroken = 0, differing = 0, near = 0;
                for (int ay = 0; ay < artHeight; ay++){
                    for (int ax = 0; ax < artWidth; ax++){
                        const uint8_t* first = &rgba[(size_t(ay * pixelSize) * width + size_t(ax * pixelSize)) * 4];
                        // hard edges: every screen pixel of the art pixel is the same colour
                        for (int sy = ay * pixelSize; sy < std::min((ay + 1) * pixelSize, height); sy++){
                            for (int sx = ax * pixelSize; sx < std::min((ax + 1) * pixelSize, width); sx++){
                                const uint8_t* p = &rgba[(size_t(sy) * width + size_t(sx)) * 4];
                                if (p[0] != first[0] || p[1] != first[1] || p[2] != first[2]) blocksBroken++;
                            }
                        }
                        // over the black scene the premultiplied output is the colour
                        const Rgba8 want = image.colors[size_t(ay) * artWidth + ax];
                        const int d = std::max({std::abs(first[0] - want.r), std::abs(first[1] - want.g), std::abs(first[2] - want.b)});
                        if (d > 1) differing++;
                        else if (d == 1) near++;
                        largestDifference = std::max(largestDifference, d);
                    }
                }
                EXPECT_EQ(blocksBroken, 0) << what;
                // the twin's floats may round a field differently on an edge pixel or two
                EXPECT_LE(differing, artWidth * artHeight / 500) << what << " (" << near << " within 1)";
                std::printf("[transition-twin] %s: %d of %d art pixels differ by 1, %d by more\n", what.c_str(), near, artWidth * artHeight, differing);
            }
        }
    }
    std::printf("[transition-twin] largest channel difference: %d of 255\n", largestDifference);
}

TEST_F(ScreenTransitionGpu, CoveredIsAllInkAndIdleDrawsNothing){
    TransitionOptions options;
    options.ink = Vector4(0.2f, 0.4f, 0.6f, 1.0f);
    for (TransitionKind kind : kGpuKinds){
        ScreenTransition::setPreview(kind, options, 1.0f, TransitionPhase::HOLD);
        int width = 0, height = 0;
        const std::vector<uint8_t> rgba = drawn(width, height);
        int wrong = 0;
        for (size_t i = 0; i < rgba.size(); i += 4)
            wrong += (rgba[i] != 51 || rgba[i + 1] != 102 || rgba[i + 2] != 153) ? 1 : 0;
        EXPECT_EQ(wrong, 0) << transitionKindName(kind);
    }
    ScreenTransition::setPreview(TransitionKind::IRIS, options, 0.0f, TransitionPhase::COVER);
    EXPECT_FALSE(ScreenTransition::needsDraw());
    int width = 0, height = 0;
    const std::vector<uint8_t> rgba = drawn(width, height);
    int lit = 0;
    for (size_t i = 0; i < rgba.size(); i += 4)
        lit += (rgba[i] | rgba[i + 1] | rgba[i + 2]) != 0 ? 1 : 0;
    EXPECT_EQ(lit, 0);
}

// R17: no program before the first transition; made on first use and kept
TEST_F(ScreenTransitionGpu, ProgramMadeOnFirstUse){
    ScreenTransition::releaseGpu();
    GpuEnvironment::drawFrames(3);
    EXPECT_FALSE(ScreenTransition::isProgramCreated());
    ScreenTransition::setPreview(TransitionKind::FADE, TransitionOptions(), 0.5f);
    int width = 0, height = 0;
    drawn(width, height);
    EXPECT_TRUE(ScreenTransition::isProgramCreated());
    ScreenTransition::setPreview(TransitionKind::FADE, TransitionOptions(), -1.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(ScreenTransition::isProgramCreated());
}
