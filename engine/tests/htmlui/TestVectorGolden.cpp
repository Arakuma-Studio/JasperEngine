// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Vector goldens (golden/p1/): G19 svg_file (<svg src> at 24/64/256 px, crop-to-content,
// image-color tint, decorator: svg()), G20 svg_inline (gradients, strokes, dashes, <text>),
// G22a-c lottie_frames (a .json Lottie at t = 0, 0.5 and 1.0 s next to a .lottie at its
// "loop" marker, tinted and letterboxed copies). 512x384, UI clock pinned. Each test also
// probes the pixels its fixture defines, so a golden cannot pin a wrong image. 4x MSAA, like
// the other phase-1 goldens.

#include "TestRenderCommon.h"

#include "HtmlUIClock.h"
#include "vector/UILottieElement.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class VectorGoldenTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
            HtmlUIClock::reset();
            HtmlUIClock::setFixedTime(0.0);
        }

        void TearDown() override{
            HtmlUIClock::reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
        }
    };

    // a colour within a wide tolerance (antialiased strokes, gradients)
    ::testing::AssertionResult near(const PixelProbe& probe, int x, int y, int r, int g, int b, int tolerance){
        return probe.is(x, y, r, g, b, 255, tolerance);
    }

    // pixels brighter than `level` (all channels) inside a rectangle
    size_t brightPixels(const std::vector<uint8_t>& rgba, int width, int x0, int y0, int x1, int y1, int level){
        size_t count = 0;
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                const uint8_t* p = &rgba[((size_t)y * width + x) * 4];
                count += (p[0] > level && p[1] > level && p[2] > level) ? 1 : 0;
            }
        }
        return count;
    }

}

// G19
TEST_F(VectorGoldenTest, SvgFile){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    ASSERT_NE(test.load("p1/vector/svg_file.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, 512};

    // icon.svg at 24, 64 and 256 px: white circle centre, blue square, transparent margin
    EXPECT_TRUE(probe.is(32, 32, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(24, 32, 0x20, 0x60, 0xc0, 255, 3));
    EXPECT_TRUE(probe.is(92, 52, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(68, 52, 0x20, 0x60, 0xc0, 255, 3));
    EXPECT_TRUE(probe.is(268, 148, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(172, 148, 0x20, 0x60, 0xc0, 255, 3));
    EXPECT_TRUE(probe.isBackground(145, 148));
    // crop-to-content: the 40 px circle fills its box; without, a 100 px box with a margin
    EXPECT_TRUE(probe.is(440, 40, 0x30, 0xc0, 0x60, 255, 3));
    EXPECT_TRUE(probe.is(423, 40, 0x30, 0xc0, 0x60, 255, 3));
    EXPECT_TRUE(probe.is(460, 130, 0x30, 0xc0, 0x60, 255, 3));
    EXPECT_TRUE(probe.isBackground(415, 85));
    EXPECT_TRUE(probe.isBackground(436, 130));
    // a white triangle tinted by image-color, and tinted at opacity 0.5 over #1E1E1E
    EXPECT_TRUE(probe.is(52, 345, 255, 128, 0, 255, 3));
    EXPECT_TRUE(probe.is(132, 345, 15, 95, 143, 255, 3));
    EXPECT_TRUE(probe.isBackground(24, 304));
    // decorator: svg(), and with crop-to-content
    EXPECT_TRUE(probe.is(232, 332, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(322, 332, 0x30, 0xc0, 0x60, 255, 3));
    EXPECT_TRUE(probe.isBackground(291, 301));

    CompareResult result = test.compareScreen("p1/svg_file");
    EXPECT_TRUE(result.passed) << result.message;
}

// G20
TEST_F(VectorGoldenTest, SvgInline){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    ASSERT_NE(test.load("p1/vector/svg_inline.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, 512};

    // the inline <svg> is placed at (16, 16), 1:1 with its viewBox
    // linear gradient #ff3030 -> #3030ff over x 10..310
    EXPECT_TRUE(near(probe, 36, 76, 248, 48, 55, 4));
    EXPECT_TRUE(near(probe, 176, 76, 152, 48, 152, 4));
    EXPECT_TRUE(near(probe, 316, 76, 55, 48, 248, 4));
    // rounded corner (rx 16) leaves the background
    EXPECT_TRUE(probe.isBackground(27, 27));
    // radial gradient: white centre fading out
    EXPECT_TRUE(near(probe, 416, 76, 255, 255, 255, 4));
    EXPECT_TRUE(probe.isBackground(416 + 49, 76 + 49));
    // round-capped cubic stroke starts at (20, 150)
    EXPECT_TRUE(near(probe, 36, 166, 0x40, 0xe0, 0xa0, 6));
    // dashed polyline: the first dash from (20, 220) towards (80, 190), a gap after 12 px
    EXPECT_TRUE(near(probe, 39, 235, 0xff, 0xd0, 0x40, 40));
    // the stroked rectangle's top edge, hollow inside
    EXPECT_TRUE(near(probe, 391, 266, 0xe0, 0xe0, 0xe0, 4));
    EXPECT_TRUE(probe.isBackground(391, 306));
    // "Vector 123" in the built-in font, 40 px, baseline at y 300: white glyph pixels
    EXPECT_GT(brightPixels(screen, 512, 36, 280, 300, 320, 200), 400u);
    EXPECT_EQ(brightPixels(screen, 512, 36, 322, 300, 360, 200), 0u);

    CompareResult result = test.compareScreen("p1/svg_inline");
    EXPECT_TRUE(result.passed) << result.message;
}

// G22a-c, drawn through the phase-1 RmlRenderer's HtmlUITexture handles
TEST_F(VectorGoldenTest, LottieFrames){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.load("p1/vector/lottie_frames.rml");
    ASSERT_NE(document, nullptr);

    struct Frame {
        double time;
        const char* golden;
        int boxX;            // the moving box's centre: x 50 -> 150 over 2 s, at 16 + x
        int circleR, circleG, circleB;
    };
    // the circle's fill goes linearly (0.1, 0.3, 0.9) -> (0.1, 0.8, 0.2) over 60 frames
    const Frame frames[] = {
        {0.0, "p1/lottie_frames_0", 16 + 50, 26, 77, 230},
        {0.5, "p1/lottie_frames_05", 16 + 75, 26, 108, 185},
        {1.0, "p1/lottie_frames_1", 16 + 100, 26, 140, 140},
    };

    for (const Frame& frame : frames){
        HtmlUIClock::setFixedTime(frame.time);
        ASSERT_TRUE(test.renderUntilDrawn()) << frame.golden;
        std::vector<uint8_t> screen;
        ASSERT_TRUE(test.readScreen(screen));
        PixelProbe probe{screen, 512};

        // #json: autoplay loop, 200 x 200 at (16, 16): the red box (60 px, centre y 60)
        EXPECT_TRUE(probe.is(frame.boxX, 16 + 60, 230, 51, 26, 255, 3)) << frame.golden;
        EXPECT_TRUE(probe.isBackground(frame.boxX - 40, 16 + 60)) << frame.golden;
        // the circle (r 35, centre (100, 145)) changing colour
        EXPECT_TRUE(probe.is(16 + 100, 16 + 145, frame.circleR, frame.circleG, frame.circleB, 255, 4)) << frame.golden;
        // #dot: the .lottie at its "loop" marker, paused at frame 30: box centre x 100
        EXPECT_TRUE(probe.is(248 + 100, 16 + 60, 230, 51, 26, 255, 3)) << frame.golden;
        EXPECT_TRUE(probe.isBackground(248 + 50, 16 + 60)) << frame.golden;
        // #tinted: frame 0 at 120 px (scale 0.6) tinted (128, 255, 128) at opacity 0.75:
        // the box at (30, 36) of the element is red x tint
        EXPECT_TRUE(probe.is(16 + 30, 240 + 36, 94, 46, 18, 255, 4)) << frame.golden;
        // #wide: 320 x 100, frame 0 scaled to 100 x 100 and centred (x 110..210): the box
        // centre (50, 60) lands on (110 + 25, 30)
        EXPECT_TRUE(probe.is(160 + 135, 260 + 30, 230, 51, 26, 255, 4)) << frame.golden;
        EXPECT_TRUE(probe.is(160 + 50, 260 + 50, 0x30, 0x30, 0x30, 255, 2)) << frame.golden;

        CompareResult result = test.compareScreen(frame.golden);
        EXPECT_TRUE(result.passed) << result.message;
    }

    // one texture per element, alive through the three frames
    UILottieElement* json = rmlui_dynamic_cast<UILottieElement*>(document->GetElementById("json"));
    ASSERT_NE(json, nullptr);
    EXPECT_EQ(json->getStats().texturesCreated, 1u);
    EXPECT_EQ(json->getStats().uploads, 3u);
}
