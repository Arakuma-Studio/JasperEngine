// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Renderer-level goldens (golden/render_*.png): RmlUi documents without text drawn by
// RmlRenderer into a 640x360 layer, composited over #1E1E1E. Each test also probes the
// pixels its fixture defines, so a golden cannot pin a wrong image.

#include "TestRenderCommon.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class RmlRendererGoldenTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            // documents without text: RmlUi logs nothing
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
        }

        // loads, draws and reads the composited screen back
        bool draw(RenderTestContext& test, const std::string& document, std::vector<uint8_t>& screen){
            if (!test.isValid() || !test.load(document))
                return false;
            return test.renderUntilDrawn() && test.readScreen(screen);
        }
    };

}

TEST_F(RmlRendererGoldenTest, Boxes){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "render/boxes.rml", screen));
    PixelProbe probe{screen, 640};

    // #solid: 4 px white border around #3060c0, border box (20,20)-(148,108)
    EXPECT_TRUE(probe.is(80, 60, 0x30, 0x60, 0xc0));
    EXPECT_TRUE(probe.is(21, 60, 255, 255, 255));
    EXPECT_TRUE(probe.is(146, 60, 255, 255, 255));
    EXPECT_TRUE(probe.isBackground(150, 60));
    EXPECT_TRUE(probe.isBackground(80, 18));
    // #sides: per-side widths and colors, border box (180,20)-(320,112)
    EXPECT_TRUE(probe.is(250, 20, 255, 0, 0));      // top 2 px red
    EXPECT_TRUE(probe.is(317, 66, 0, 255, 0));      // right 6 px green
    EXPECT_TRUE(probe.is(250, 107, 0, 0, 255));     // bottom 10 px blue
    EXPECT_TRUE(probe.is(186, 66, 255, 255, 0));    // left 14 px yellow
    EXPECT_TRUE(probe.is(250, 66, 0x40, 0x40, 0x40));
    // radius 0 keeps its corner, 24 cuts it
    EXPECT_TRUE(probe.is(341, 21, 0xc0, 0x30, 0x30));
    EXPECT_TRUE(probe.isBackground(541, 21));
    EXPECT_TRUE(probe.is(583, 63, 0x30, 0x30, 0xc0));
    EXPECT_TRUE(probe.isBackground(441, 21));
    EXPECT_TRUE(probe.is(480, 60, 0x30, 0xc0, 0x30));
    // nested: outer padding, middle margin and padding, inner rounded
    EXPECT_TRUE(probe.is(30, 160, 0x60, 0x60, 0x60));
    EXPECT_TRUE(probe.is(50, 180, 0xa0, 0x60, 0x20));
    EXPECT_TRUE(probe.is(110, 210, 0x20, 0xa0, 0xa0));
    // the ring is hollow
    EXPECT_TRUE(probe.isBackground(410, 220));
    EXPECT_TRUE(probe.is(345, 220, 0xff, 0xc0, 0x00));

    CompareResult result = test.compareScreen("render_boxes");
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(RmlRendererGoldenTest, Flex){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "render/flex.rml", screen));
    PixelProbe probe{screen, 640};

    // flex-start: 40 px items, 10 px gaps from x = 10
    EXPECT_TRUE(probe.is(30, 25, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(80, 25, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.is(130, 25, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(probe.is(200, 25, 0x30, 0x30, 0x30));
    // center: 140 px of items in 300 starts at x = 90
    EXPECT_TRUE(probe.is(50, 75, 0x30, 0x30, 0x30));
    EXPECT_TRUE(probe.is(110, 75, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(210, 75, 0x40, 0x40, 0xe0));
    // space-between: the last item ends at the row's end
    EXPECT_TRUE(probe.is(160, 125, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.is(305, 125, 0x40, 0x40, 0xe0));
    // flex-grow 1:2:1 of 300 - 20 px gaps - 30 px bases: widths 72.5, 135, 72.5
    EXPECT_TRUE(probe.is(15, 175, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(160, 175, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.is(305, 175, 0x40, 0x40, 0xe0));
    // align-items start / center / end of a 30 px item in an 80 px row
    EXPECT_TRUE(probe.is(20, 215, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(20, 285, 0x30, 0x30, 0x30));
    EXPECT_TRUE(probe.is(125, 250, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(125, 215, 0x30, 0x30, 0x30));
    EXPECT_TRUE(probe.is(230, 285, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(230, 215, 0x30, 0x30, 0x30));
    // wrap: four 40 px items and three gaps per 200 px line, the fifth on the second line;
    // the lines stretch over the 140 px (align-content), so the second starts at y = 85
    EXPECT_TRUE(probe.is(350, 25, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(500, 25, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(350, 100, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.is(350, 65, 0x30, 0x30, 0x30));
    // nested rows of growing cells
    EXPECT_TRUE(probe.is(380, 220, 0xd0, 0xa0, 0x30));
    EXPECT_TRUE(probe.is(440, 220, 0x50, 0x50, 0x50));
    // column with space-between, centered
    EXPECT_TRUE(probe.is(590, 35, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.is(590, 325, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(probe.is(560, 35, 0x30, 0x30, 0x30));

    CompareResult result = test.compareScreen("render_flex");
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(RmlRendererGoldenTest, Images){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "render/images.rml", screen));
    PixelProbe probe{screen, 640};

    // checker.png at 1x: 8 px cells, the top-left one red
    EXPECT_TRUE(probe.is(24, 24, 255, 0, 0));
    EXPECT_TRUE(probe.is(32, 24, 0x40, 0x40, 0x40));
    EXPECT_TRUE(probe.is(40, 24, 255, 255, 255));
    // at 2.5x: 20 px cells
    EXPECT_TRUE(probe.is(118, 28, 255, 0, 0));
    EXPECT_TRUE(probe.is(140, 28, 0x40, 0x40, 0x40));
    EXPECT_TRUE(probe.is(160, 28, 255, 255, 255));
    EXPECT_TRUE(probe.is(118, 48, 0x40, 0x40, 0x40));
    // alpha50 (255, 0, 0, 128) over white and over the background
    EXPECT_TRUE(probe.is(330, 50, 255, 127, 127));
    EXPECT_TRUE(probe.is(305, 25, 255, 255, 255));
    EXPECT_TRUE(probe.is(316, 136, 143, 15, 15));
    // the image decorator
    EXPECT_TRUE(probe.is(404, 24, 255, 0, 0));
    EXPECT_TRUE(probe.is(412, 24, 0x40, 0x40, 0x40));
    // icon.svg: white circle on the blue rounded square, at 1x and 3x
    EXPECT_TRUE(probe.is(512, 32, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(504, 32, 0x20, 0x60, 0xc0, 255, 3));
    EXPECT_TRUE(probe.is(576, 56, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(552, 56, 0x20, 0x60, 0xc0, 255, 6));

    CompareResult result = test.compareScreen("render_images");
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(RmlRendererGoldenTest, Alpha){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "render/alpha.rml", screen));
    PixelProbe probe{screen, 640};

    // #FF000080 over #1E1E1E: 128 + 30 * 127 / 255
    EXPECT_TRUE(probe.is(60, 60, 143, 15, 15));
    // #0000FF80 over it: premultiplied "over" in the layer, then over the background
    EXPECT_TRUE(probe.is(150, 130, 71, 7, 135, 255, 3));
    EXPECT_TRUE(probe.is(260, 200, 15, 15, 143));
    // opacity 0.5 is a group (CSS; phase 3, p3-paint): the subtree is drawn opaque into a layer composited at 50%
    EXPECT_TRUE(probe.is(350, 50, 143, 143, 143));
    EXPECT_TRUE(probe.is(400, 100, 15, 143, 15, 255, 3));
    // a quarter-alpha white over the background
    EXPECT_TRUE(probe.is(300, 290, 87, 87, 87, 255, 3));

    // the layer itself holds premultiplied color and coverage
    std::vector<uint8_t> layer;
    ASSERT_TRUE(test.readLayer(layer));
    PixelProbe layerProbe{layer, 640};
    EXPECT_TRUE(layerProbe.is(60, 60, 128, 0, 0, 128, 1));
    EXPECT_TRUE(layerProbe.is(150, 130, 64, 0, 128, 192, 2));
    EXPECT_TRUE(layerProbe.is(10, 10, 0, 0, 0, 0, 0));

    CompareResult result = test.compareScreen("render_alpha");
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(RmlRendererGoldenTest, Scissor){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.load("render/scissor.rml");
    ASSERT_NE(document, nullptr);
    Rml::Element* list = document->GetElementById("list");
    ASSERT_NE(list, nullptr);
    test.context->Update();
    list->SetScrollTop(50.0f);
    ASSERT_TRUE(test.renderUntilDrawn());
    EXPECT_FLOAT_EQ(list->GetScrollTop(), 50.0f);

    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, 640};

    // the big child shows only inside #clip
    EXPECT_TRUE(probe.is(100, 100, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(probe.is(100, 30, 0x30, 0x30, 0x60));
    EXPECT_TRUE(probe.isBackground(240, 100));
    EXPECT_TRUE(probe.isBackground(100, 130));
    EXPECT_TRUE(probe.isBackground(10, 100));
    // nested clips intersect
    EXPECT_TRUE(probe.is(300, 60, 0x30, 0x60, 0x30));
    EXPECT_TRUE(probe.is(350, 110, 0x60, 0x30, 0x30));
    EXPECT_TRUE(probe.is(400, 160, 0x20, 0xc0, 0xc0));
    EXPECT_TRUE(probe.isBackground(430, 160));
    EXPECT_TRUE(probe.isBackground(400, 190));
    // the list scrolled by 50 px: stripe 2 (blue) at its top, stripe 12 (red) at its bottom
    EXPECT_TRUE(probe.is(540, 25, 0x40, 0x40, 0xff));
    EXPECT_TRUE(probe.is(540, 35, 0xff, 0xff, 0x40));
    EXPECT_TRUE(probe.is(540, 215, 0xff, 0x40, 0x40));
    EXPECT_TRUE(probe.isBackground(540, 225));
    EXPECT_TRUE(probe.isBackground(540, 15));
    // after the clipped draws the scissor is off again
    EXPECT_TRUE(probe.is(320, 280, 0x80, 0x80, 0x80));

    CompareResult result = test.compareScreen("render_scissor");
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(RmlRendererGoldenTest, Transform){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "render/transform.rml", screen));
    PixelProbe probe{screen, 640};

    EXPECT_TRUE(probe.is(70, 70, 0x60, 0x60, 0x60));
    // rotated about its center: the center stays, a corner moves out
    EXPECT_TRUE(probe.is(210, 70, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.isBackground(161, 41));
    // scaled 1.5x about its center: 150 px wide from x = 295
    EXPECT_TRUE(probe.is(370, 70, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.is(440, 70, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.isBackground(450, 70));
    // translated by (30, 20)
    EXPECT_TRUE(probe.is(140, 270, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(probe.isBackground(30, 205));
    // perspective: the box keeps its center, its sides are no longer parallel
    EXPECT_TRUE(probe.is(300, 255, 0xe0, 0xc0, 0x40));

    CompareResult result = test.compareScreen("render_transform");
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(RmlRendererGoldenTest, Many){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    // 5000 boxes of 4x4 px on a 5 px grid, 125 per row: many geometries in few buffers
    std::string rml = "<html><head><link type=\"text/css\" href=\"../common/base.css\"/><link type=\"text/css\" href=\"render.css\"/>"
                      "<style>#grid { position: absolute; left: 7px; top: 30px; width: 627px; display: flex; flex-wrap: wrap; }"
                      " #grid div { width: 4px; height: 4px; margin-right: 1px; margin-bottom: 1px; }</style></head><body><div id=\"grid\">";
    for (int i = 0; i < 5000; i++){
        char colour[32];
        std::snprintf(colour, sizeof(colour), "#%02x%02x%02x", (i * 7) % 256, (i / 125) * 6, 255 - (i % 125) * 2);
        rml += std::string("<div style=\"background-color: ") + colour + "\"/>";
    }
    rml += "</div></body></html>";

    // the document's url places it next to the fixtures, for its stylesheet links
    ASSERT_NE(test.loadFromMemory(rml, "render/many.html"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    EXPECT_LT(test.renderer.getGeometryBufferCount(), 64u);
    // every box submitted (S16: batched into a few draws)
    EXPECT_GE(test.renderer.getStats().geometrySubmitted, 5000u);

    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, 640};
    // box i at (7 + 5 * (i % 125), 30 + 5 * (i / 125))
    for (int i : {0, 1, 124, 125, 2500, 4999}){
        const int x = 7 + 5 * (i % 125) + 1;
        const int y = 30 + 5 * (i / 125) + 1;
        EXPECT_TRUE(probe.is(x, y, (i * 7) % 256, (i / 125) * 6, 255 - (i % 125) * 2)) << "box " << i;
    }
    EXPECT_TRUE(probe.isBackground(7 + 4, 30 + 4));
    EXPECT_TRUE(probe.isBackground(320, 240));

    CompareResult result = test.compareScreen("render_many");
    EXPECT_TRUE(result.passed) << result.message;
}

// the document laid out at 640x360 and drawn into a 1280x720 target: shapes are rasterized
// at the target's resolution (no upscaling blur), the scissor scales with them
TEST_F(RmlRendererGoldenTest, ResolutionScaling){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(1280, 720, 640, 360);
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.load("render/scissor.rml");
    ASSERT_NE(document, nullptr);
    test.context->Update();
    document->GetElementById("list")->SetScrollTop(50.0f);
    ASSERT_TRUE(test.renderUntilDrawn());

    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, 1280};
    // #clip (20,20)-(220,120) in document px is (40,40)-(440,240) in target px, sharp edges
    EXPECT_TRUE(probe.is(200, 200, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(probe.is(439, 200, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(probe.isBackground(440, 200));
    EXPECT_TRUE(probe.is(200, 239, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(probe.isBackground(200, 240));
    // scrolled list at 2x
    EXPECT_TRUE(probe.is(1080, 50, 0x40, 0x40, 0xff));
    EXPECT_TRUE(probe.is(1080, 430, 0xff, 0x40, 0x40));
    EXPECT_TRUE(probe.isBackground(1080, 450));
}
