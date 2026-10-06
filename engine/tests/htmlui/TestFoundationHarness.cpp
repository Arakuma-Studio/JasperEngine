// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The headless GPU harness itself: a scene renders into the test target, the target reads
// back with the top row first, and the golden comparison holds on glcore and vulkan.

#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/Readback.h"
#include "harness/SceneFixture.h"

#include "object/ui/Image.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Pixels {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;

        bool read(){
            Framebuffer& target = GpuEnvironment::target();
            width = int(target.getWidth());
            height = int(target.getHeight());
            return readTexture(target.getRender().getColorTexture(), width, height, rgba);
        }

        // canvas == screen in these tests: (x, y) from the top-left corner
        ::testing::AssertionResult is(int x, int y, int r, int g, int b, int tolerance = 2) const{
            const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
            if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance && p[3] == 255)
                return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "pixel (" << x << ", " << y << ") is (" << int(p[0]) << ", " << int(p[1])
                                                 << ", " << int(p[2]) << ", " << int(p[3]) << "), expected (" << r << ", " << g
                                                 << ", " << b << ", 255)";
        }
    };

    const int background = 0x1E;

}

TEST(HarnessGpuTest, Clear){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);

    SceneFixture fixture;
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Pixels pixels;
    ASSERT_TRUE(pixels.read());
    ASSERT_EQ(pixels.width, 640);
    ASSERT_EQ(pixels.height, 360);
    EXPECT_TRUE(pixels.is(0, 0, background, background, background));
    EXPECT_TRUE(pixels.is(639, 0, background, background, background));
    EXPECT_TRUE(pixels.is(320, 180, background, background, background));
    EXPECT_TRUE(pixels.is(639, 359, background, background, background));

    EXPECT_GOLDEN("harness_clear");
}

TEST(HarnessGpuTest, ComponentUIImage){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);

    SceneFixture fixture;

    // red near the top-left, blue near the bottom-right: a flip in the scene, the target
    // or the readback moves them
    Image& red = fixture.add<Image>();
    red.setPosition(100, 50);
    red.setSize(200, 100);
    red.setColor(1.0f, 0.0f, 0.0f, 1.0f);

    Image& blue = fixture.add<Image>();
    blue.setPosition(500, 280);
    blue.setSize(100, 60);
    blue.setColor(0.0f, 0.0f, 1.0f, 1.0f);

    ASSERT_TRUE(fixture.waitUntilLoaded());

    Pixels pixels;
    ASSERT_TRUE(pixels.read());

    // inside the red rect (100,50)-(300,150), away from its edges
    EXPECT_TRUE(pixels.is(102, 52, 255, 0, 0));
    EXPECT_TRUE(pixels.is(200, 100, 255, 0, 0));
    EXPECT_TRUE(pixels.is(297, 147, 255, 0, 0));
    // just outside it
    EXPECT_TRUE(pixels.is(97, 100, background, background, background));
    EXPECT_TRUE(pixels.is(200, 47, background, background, background));
    EXPECT_TRUE(pixels.is(303, 100, background, background, background));
    EXPECT_TRUE(pixels.is(200, 153, background, background, background));

    // inside the blue rect (500,280)-(600,340)
    EXPECT_TRUE(pixels.is(502, 282, 0, 0, 255));
    EXPECT_TRUE(pixels.is(597, 337, 0, 0, 255));
    EXPECT_TRUE(pixels.is(550, 277, background, background, background));
    EXPECT_TRUE(pixels.is(550, 343, background, background, background));

    // the mirrored places stay empty
    EXPECT_TRUE(pixels.is(200, 310, background, background, background));
    EXPECT_TRUE(pixels.is(440, 100, background, background, background));

    EXPECT_GOLDEN("harness_component_ui");
}

TEST(HarnessGpuTest, ComponentUITexture){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);

    SceneFixture fixture;

    // checker.png (64x64, 8 px cells, top-left cell red) drawn at 2x: 16 px cells. Textured
    // draws need image views and samplers, which lavapipe before Mesa 24 creates only with
    // sokol's descriptor size limit raised (sokol_gfx.h _SG_VK_MAX_DESCRIPTOR_DATA_SIZE)
    Image& checker = fixture.add<Image>();
    checker.setTexture("common/checker.png");
    checker.setPosition(100, 50);
    checker.setSize(128, 128);

    ASSERT_TRUE(fixture.waitUntilLoaded());

    Pixels pixels;
    ASSERT_TRUE(pixels.read());

    // cell centers: (column, row) -> (100 + 16 * column + 8, 50 + 16 * row + 8)
    EXPECT_TRUE(pixels.is(108, 58, 255, 0, 0));       // (0, 0) red: the image's top-left
    EXPECT_TRUE(pixels.is(140, 58, 255, 255, 255));   // (2, 0) white
    EXPECT_TRUE(pixels.is(108, 90, 255, 255, 255));   // (0, 2) white
    EXPECT_TRUE(pixels.is(220, 170, 255, 255, 255));  // (7, 7) white
    // gray cells are darker than white on every backend (the exact value depends on the UI
    // shader's color conversion, the golden pins it)
    const uint8_t* gray = &pixels.rgba[(size_t(58) * pixels.width + 124) * 4];   // (1, 0)
    EXPECT_LT(gray[0], 200);
    EXPECT_EQ(gray[0], gray[1]);
    EXPECT_EQ(gray[1], gray[2]);
    // outside
    EXPECT_TRUE(pixels.is(97, 100, background, background, background));
    EXPECT_TRUE(pixels.is(231, 100, background, background, background));
    EXPECT_TRUE(pixels.is(160, 181, background, background, background));

    EXPECT_GOLDEN("harness_component_ui_texture");
}
