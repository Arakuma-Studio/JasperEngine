// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-1 renderer goldens (golden/p1/, spec §5.2 G01-G18, G23, G24): stencil clips, layers,
// filters, backdrop filters, gradients, box shadows, masks and MSAA, drawn by RmlRenderer into
// a 512x384 target with 4x MSAA and composited over #1E1E1E, the UI clock pinned. Each test
// also probes the pixels its fixture defines, so a golden cannot pin a wrong image.

#include "TestRenderCommon.h"

#include "HtmlUIClock.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 512;
    const int height = 384;

    class RmlRendererP1GoldenTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
            HtmlUIClock::setFixedTime(0.0);
        }

        void TearDown() override{
            HtmlUIClock::reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
        }

        // loads p1/<name>.rml, draws it and reads the composited screen back
        bool draw(RenderTestContext& test, const std::string& name, std::vector<uint8_t>& screen){
            if (!test.isValid() || !test.load("p1/" + name + ".rml"))
                return false;
            return test.renderUntilDrawn() && test.readScreen(screen);
        }

        void expectGolden(RenderTestContext& test, const std::string& name){
            CompareResult result = test.compareScreen("p1/" + name);
            EXPECT_TRUE(result.passed) << result.message;
        }
    };

    struct P1Context : public RenderTestContext {
        explicit P1Context(int samples = 4): RenderTestContext(width, height, 0, 0, samples){}
    };

    // the largest channel difference between two pixels
    int pixelDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int x, int y){
        const size_t i = (size_t(y) * width + x) * 4;
        int difference = 0;
        for (int c = 0; c < 4; c++)
            difference = std::max(difference, std::abs(int(a[i + c]) - int(b[i + c])));
        return difference;
    }

}

TEST_F(RmlRendererP1GoldenTest, ClipRounded){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "clip_rounded", screen));
    PixelProbe probe{screen, width};

    // the corner outside the 50 px radius is clipped (the checker image would cover it)
    EXPECT_TRUE(probe.isBackground(44, 44));
    EXPECT_TRUE(probe.isBackground(236, 186));
    // inside: the scaled checker
    EXPECT_TRUE(probe.is(90, 70, 255, 255, 255));
    EXPECT_TRUE(probe.is(120, 70, 0x40, 0x40, 0x40));
    // the text stops at the box's right edge
    EXPECT_TRUE(probe.isBackground(250, 160));
    // the box next to it is untouched
    EXPECT_TRUE(probe.is(375, 115, 0x40, 0xa0, 0x40));

    expectGolden(test, "clip_rounded");
}

TEST_F(RmlRendererP1GoldenTest, ClipNested){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "clip_nested", screen));
    PixelProbe probe{screen, width};

    // #fill shows only where all three clips overlap: a (20..320), b (120..420 x 60..260),
    // c (40..290 x 120..370)
    EXPECT_TRUE(probe.is(200, 200, 0xe0, 0xe0, 0x40));
    // inside a and b, outside c: b's green
    EXPECT_TRUE(probe.is(300, 80, 0x20, 0x80, 0x20));
    // inside a only: a's red
    EXPECT_TRUE(probe.is(60, 80, 0x80, 0x20, 0x20));
    // outside a (c extends there): background
    EXPECT_TRUE(probe.isBackground(200, 340));
    EXPECT_TRUE(probe.isBackground(360, 200));

    expectGolden(test, "clip_nested");
}

TEST_F(RmlRendererP1GoldenTest, ClipInverse){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "clip_inverse", screen));
    PixelProbe probe{screen, width};

    // the 20 px inset shadow along the edges, the background colour in the middle
    EXPECT_TRUE(probe.is(180, 70, 0xc0, 0x20, 0x20));
    EXPECT_TRUE(probe.is(70, 150, 0xc0, 0x20, 0x20));
    EXPECT_TRUE(probe.is(180, 150, 0xe0, 0xe0, 0xe0));
    // outside the rounded corner: nothing
    EXPECT_TRUE(probe.isBackground(62, 62));

    expectGolden(test, "clip_inverse");
}

TEST_F(RmlRendererP1GoldenTest, ClipTransform){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "clip_transform", screen));
    PixelProbe probe{screen, width};

    // the box's centre (220, 170) is fixed by the transform; the children cross there
    EXPECT_TRUE(probe.is(220, 170, 0x20, 0xc0, 0xf0));
    // the wide child is cut at the rotated box's edge: its far ends would reach here
    EXPECT_TRUE(probe.isBackground(60, 130));
    EXPECT_TRUE(probe.isBackground(400, 230));

    expectGolden(test, "clip_transform");
}

TEST_F(RmlRendererP1GoldenTest, Transforms){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "transforms", screen));
    PixelProbe probe{screen, width};

    // translate(20, 10): the box covers 40..120 x 30..90
    EXPECT_TRUE(probe.is(115, 85, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.isBackground(25, 25));
    // the rotated and scaled ones keep their centre
    EXPECT_TRUE(probe.is(190, 60, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(probe.is(320, 60, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(probe.is(440, 60, 0xe0, 0xe0, 0x40));
    // the perspective plane
    EXPECT_TRUE(probe.is(250, 260, 0xc0, 0x40, 0xc0));

    expectGolden(test, "transforms");
}

TEST_F(RmlRendererP1GoldenTest, FilterOpacity){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "filter_opacity", screen));
    PixelProbe probe{screen, width};

    // group opacity: red alone, blue alone, and the overlap shows blue only
    EXPECT_TRUE(probe.is(60, 60, 143, 15, 15));
    EXPECT_TRUE(probe.is(260, 200, 15, 15, 143));
    EXPECT_TRUE(probe.is(150, 120, 15, 15, 143));
    // without a group the overlap mixes both
    EXPECT_TRUE(probe.is(110, 300, 71, 7, 135, 255, 3));

    expectGolden(test, "filter_opacity");
}

TEST_F(RmlRendererP1GoldenTest, FilterBlur){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "filter_blur", screen));
    PixelProbe probe{screen, width};

    // blur(2px): the centre keeps its colour, the white border nearly so
    EXPECT_TRUE(probe.is(80, 130, 255, 0x20, 0x20, 255, 4));
    // the blurred box edge is soft: a mix of white and the background
    const uint8_t* edge = probe.at(30, 130);
    EXPECT_GT(edge[0], 0x40);
    EXPECT_LT(edge[0], 0xf0);
    // blur(40px): no pure colour left, and the blur spreads well outside the box
    const uint8_t* wide = probe.at(400, 130);
    EXPECT_LT(wide[0] - wide[1], 0xc0);
    EXPECT_GT(probe.at(340, 130)[0], 0x30);

    expectGolden(test, "filter_blur");
}

TEST_F(RmlRendererP1GoldenTest, FilterDropShadow){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "filter_drop_shadow", screen));
    PixelProbe probe{screen, width};

    // the hard magenta shadow of the image, 8 px down and right, and the image over it
    EXPECT_TRUE(probe.is(406, 70, 255, 0, 255));
    // its first row (the image's first row, read at the region's first texel centre: it was
    // dropped where rounding put the read just before it)
    for (int x = 404; x < 412; x++)
        EXPECT_TRUE(probe.is(x, 48, 255, 0, 255)) << "the shadow's first row";
    EXPECT_TRUE(probe.is(344, 44, 255, 0, 0));
    EXPECT_TRUE(probe.is(345, 105, 0x80, 0x80, 0x80));
    // the blurred one fades
    const uint8_t* soft = probe.at(408, 210);
    EXPECT_GT(soft[0], 0x80);
    EXPECT_LT(soft[1], 0x70);

    expectGolden(test, "filter_drop_shadow");
}

TEST_F(RmlRendererP1GoldenTest, FilterColorMatrix){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "filter_color_matrix", screen));
    PixelProbe probe{screen, width};

    // the reference tile, then the filters on its top quarter #C03020
    EXPECT_TRUE(probe.is(40, 235, 0xc0, 0x30, 0x20));
    // brightness 1.5
    EXPECT_TRUE(probe.is(40, 55, 255, 72, 48, 255, 3));
    // contrast 2: 2c - 0.5
    EXPECT_TRUE(probe.is(110, 55, 255, 0, 0, 255, 3));
    // grayscale: luminance 0.2126 r + 0.7152 g + 0.0722 b
    EXPECT_TRUE(probe.is(180, 55, 77, 77, 77, 255, 3));
    // invert
    EXPECT_TRUE(probe.is(320, 55, 0x3f, 0xcf, 0xdf, 255, 3));
    // grayscale of the gray quarter stays gray, invert of #808080 is #7F7F7F
    EXPECT_TRUE(probe.is(180, 145, 0x80, 0x80, 0x80, 255, 3));
    EXPECT_TRUE(probe.is(320, 145, 0x7f, 0x7f, 0x7f, 255, 3));

    expectGolden(test, "filter_color_matrix");
}

TEST_F(RmlRendererP1GoldenTest, FilterChain){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "filter_chain", screen));
    PixelProbe probe{screen, width};

    // grayscale(#E04020) = 96, then opacity 0.7 over #1E1E1E: 0.7 * 96 + 0.3 * 30 = 76
    EXPECT_TRUE(probe.is(80, 80, 76, 76, 76, 255, 3));
    // the inner box: grayscale(#20A0E0) = 137 -> 105
    EXPECT_TRUE(probe.is(160, 160, 105, 105, 105, 255, 3));
    EXPECT_TRUE(probe.is(375, 135, 0xe0, 0x40, 0x20));

    expectGolden(test, "filter_chain");
}

TEST_F(RmlRendererP1GoldenTest, BackdropFilter){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "backdrop_filter", screen));
    PixelProbe probe{screen, width};

    // outside the panel the stripes are sharp
    EXPECT_TRUE(probe.is(20, 20, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(probe.isBackground(60, 20));
    // inside, the blue stripe (160..200) bleeds into the gap left of it, which is sharp outside
    EXPECT_TRUE(probe.isBackground(154, 20));
    EXPECT_GT(probe.at(154, 150)[2], 0x60);
    // and lightened by the panel's 25 % white
    EXPECT_GT(probe.at(220, 150)[0], 0x30);

    expectGolden(test, "backdrop_filter");
}

TEST_F(RmlRendererP1GoldenTest, GradientsLinear){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "gradients_linear", screen));
    PixelProbe probe{screen, width};

    // 0deg: red at the bottom, blue at the top; 90deg: red left, blue right
    EXPECT_TRUE(probe.is(65, 117, 250, 0, 5, 255, 8));
    EXPECT_TRUE(probe.is(65, 12, 5, 0, 250, 255, 8));
    EXPECT_TRUE(probe.is(262, 65, 250, 0, 5, 255, 8));
    EXPECT_TRUE(probe.is(368, 65, 5, 0, 250, 255, 8));
    // 180deg: red at the top
    EXPECT_TRUE(probe.is(440, 12, 250, 0, 5, 255, 8));
    // halfway along: an even mix
    EXPECT_TRUE(probe.is(315, 65, 128, 0, 127, 255, 4));
    // five stops: yellow at 25 %, green at 50 %, cyan at 75 %
    EXPECT_TRUE(probe.is(10 + 122, 190, 255, 255, 0, 255, 6));
    EXPECT_TRUE(probe.is(10 + 245, 190, 0, 255, 0, 255, 6));
    EXPECT_TRUE(probe.is(10 + 367, 190, 0, 255, 255, 255, 6));
    // repeating every 100 px: black at 0, white at 50, black at 100
    EXPECT_TRUE(probe.is(10 + 50, 300, 255, 255, 255, 255, 8));
    EXPECT_TRUE(probe.is(10 + 100, 300, 0, 0, 0, 255, 8));
    EXPECT_TRUE(probe.is(10 + 150, 300, 255, 255, 255, 255, 8));
    EXPECT_TRUE(probe.is(10 + 450, 300, 255, 255, 255, 255, 8));

    expectGolden(test, "gradients_linear");
}

TEST_F(RmlRendererP1GoldenTest, GradientsRadialConic){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "gradients_radial_conic", screen));
    PixelProbe probe{screen, width};

    // circle 75 px: white centre, red halfway out
    EXPECT_TRUE(probe.is(85, 85, 255, 255, 255, 255, 8));
    EXPECT_TRUE(probe.is(85 + 37, 85, 255, 0, 0, 255, 12));
    // ellipse: yellow centre
    EXPECT_TRUE(probe.is(255, 60, 255, 255, 0, 255, 8));
    // repeating radial: white at 0 and 40 px, black at 20 px
    EXPECT_TRUE(probe.is(425 + 20, 85, 0, 0, 0, 255, 16));
    EXPECT_TRUE(probe.is(425 + 40, 85, 255, 255, 255, 255, 16));
    // conic from 0deg (up), clockwise: red at the top, green to the lower right, blue to the lower left
    EXPECT_TRUE(probe.is(85, 210, 255, 0, 0, 255, 24));
    // repeating conic: black up, white at 45deg
    EXPECT_TRUE(probe.is(255, 215, 0, 0, 0, 255, 24));

    expectGolden(test, "gradients_radial_conic");
}

TEST_F(RmlRendererP1GoldenTest, BoxShadow){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "box_shadow", screen));
    PixelProbe probe{screen, width};

    // the hard shadow 12 px down and right of #outer
    EXPECT_TRUE(probe.is(165, 135, 0, 0, 0));
    EXPECT_TRUE(probe.is(100, 85, 0x40, 0x80, 0xc0));
    EXPECT_TRUE(probe.is(45, 135, 0xf0, 0xf0, 0xf0));
    // the spread ring around #spread
    EXPECT_TRUE(probe.is(375, 85, 0xe0, 0x20, 0x20));
    // the blurred shadow (offset 6, 8) darkens the backdrop below #blur
    EXPECT_LT(probe.at(286, 136)[0], 0xb0);
    // the inset shadow darkens the top-left inner edge only
    EXPECT_TRUE(probe.is(140, 300, 0x40, 0x80, 0xc0, 255, 6));
    EXPECT_LT(probe.at(140, 225)[2], 0xb0);

    expectGolden(test, "box_shadow");
}

// A box shadow whose texture is larger than the context is rendered in tiles (fork P3-I14,
// SaveLayerIntoTexture). On a scaled canvas (the document drawn at screen resolution, target and
// context sizes differ) the tiles hold target pixels: the texture is made at the target's scale.
// It was made at the context's, so the background and border it carries were drawn scaled and
// shifted (found by phase 4's settings example in the editor's Game view).
TEST_F(RmlRendererP1GoldenTest, BoxShadowTilesOnAScaledCanvas){
    JASPER_REQUIRE_GPU();
    const char* rml = R"(<rml><head><style>
        body { display: block; width: 100%; height: 100%; margin: 0px; }
        #box { position: absolute; left: 50px; top: 40px; width: 300px; height: 220px;
               background-color: #3050c0; border: 4px solid #f0f0f0; box-shadow: 0px 20px 60px #000000c0; }
    </style></head><body><div id="box"/></body></rml>)";
    // 1.5 and 0.75 target pixels per document px; the shadow texture (about 480 x 450) is larger
    // than the 400 x 300 context either way
    for (const float scale : {1.5f, 0.75f}){
        const int targetWidth = (int)(400 * scale);
        const int targetHeight = (int)(300 * scale);
        RenderTestContext test(targetWidth, targetHeight, 400, 300, 1);
        ASSERT_TRUE(test.isValid());
        ASSERT_TRUE(test.loadFromMemory(rml, "p1/box_shadow_tiles.rml"));
        ASSERT_TRUE(test.renderUntilDrawn());
        std::vector<uint8_t> screen;
        ASSERT_TRUE(test.readScreen(screen));
        PixelProbe probe{screen, targetWidth};
        auto at = [scale](float v){ return (int)(v * scale); };
        SCOPED_TRACE("scale " + std::to_string(scale));
        // the background inside the border, near each corner and in the middle
        EXPECT_TRUE(probe.is(at(200), at(150), 0x30, 0x50, 0xc0, 255, 8));
        EXPECT_TRUE(probe.is(at(62), at(52), 0x30, 0x50, 0xc0, 255, 8));
        EXPECT_TRUE(probe.is(at(342), at(256), 0x30, 0x50, 0xc0, 255, 8));
        // the border at the left and bottom edges
        EXPECT_TRUE(probe.is(at(52), at(150), 0xf0, 0xf0, 0xf0, 255, 24));
        EXPECT_TRUE(probe.is(at(200), at(266), 0xf0, 0xf0, 0xf0, 255, 24));
        // the shadow below the box is dark, the far corner outside it is the backdrop
        EXPECT_LT(probe.at(at(200), at(285))[0], 0x18);
        EXPECT_TRUE(probe.isBackground(at(5), at(5)));
    }
}

TEST_F(RmlRendererP1GoldenTest, MaskImage){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "mask_image", screen));
    PixelProbe probe{screen, width};

    // the gradient mask: opaque at the left, fading to nothing at the right
    EXPECT_TRUE(probe.is(32, 100, 0xe0, 0x40, 0x20, 255, 6));
    EXPECT_TRUE(probe.isBackground(228, 100));
    const uint8_t* middle = probe.at(130, 100);
    EXPECT_GT(middle[0], 0x60);
    EXPECT_LT(middle[0], 0xc0);
    // the image mask (alpha 50 %, stretched): everything at half coverage
    EXPECT_TRUE(probe.is(300, 100, 127, 47, 31, 255, 4));

    expectGolden(test, "mask_image");
}

TEST_F(RmlRendererP1GoldenTest, MsaaRounded){
    JASPER_REQUIRE_GPU();
    std::vector<uint8_t> msaa;
    {
        P1Context test(4);
        ASSERT_TRUE(draw(test, "msaa_rounded", msaa));
        EXPECT_EQ(test.target.getSampleCount(), 4);
        expectGolden(test, "msaa_rounded");
    }
    std::vector<uint8_t> single;
    {
        P1Context test(1);
        ASSERT_TRUE(draw(test, "msaa_rounded", single));
    }

    // the same shapes: identical inside, different (smoothed) on the edges
    PixelProbe probe{msaa, width};
    EXPECT_TRUE(probe.is(130, 130, 0xe0, 0xe0, 0xe0));
    EXPECT_EQ(pixelDifference(msaa, single, 130, 130), 0);

    int differentEdgePixels = 0;
    int partialEdgePixels = 0;
    // along the disc's edge: radius 100 around (130, 130)
    for (int degrees = 0; degrees < 360; degrees += 3){
        const float radians = degrees * 3.14159265f / 180.0f;
        const int x = (int)std::lround(130.0f + 100.0f * std::cos(radians));
        const int y = (int)std::lround(130.0f + 100.0f * std::sin(radians));
        if (pixelDifference(msaa, single, x, y) > 8)
            differentEdgePixels++;
        const int r = msaa[(size_t(y) * width + x) * 4];
        if (r > 0x30 && r < 0xd0)
            partialEdgePixels++;
    }
    // (with the SDF primitives, S16 R8, one sample is antialiased as well: the edges barely differ,
    // both are partial)
    if (RmlRenderer::isSdfPrimitives())
        EXPECT_LE(differentEdgePixels, 20);
    else
        EXPECT_GT(differentEdgePixels, 20);
    EXPECT_GT(partialEdgePixels, 20);
}

TEST_F(RmlRendererP1GoldenTest, LayersNested){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "layers_nested", screen));
    PixelProbe probe{screen, width};

    // the faded white box (50 %) inside the blurred one, over the darkened backdrop
    const uint8_t* faded = probe.at(160, 160);
    EXPECT_GT(faded[0], 0x70);
    EXPECT_LT(faded[0], 0xd0);
    // the magenta child is part of the 50 % group: magenta-ish, not opaque magenta
    const uint8_t* child = probe.at(240, 190);
    EXPECT_GT(child[0], child[1] + 0x30);
    EXPECT_LT(child[0], 0xf0);
    // inside the blurred box but outside the faded one: the panel's darkened backdrop (a layer
    // reused in the frame shows nothing of its earlier content)
    EXPECT_LT(probe.at(150, 110)[1], 0xb0);
    EXPECT_GT(probe.at(150, 110)[1], 0x60);
    // the backdrop's temp layer, the blur's and the opacity's, with passes for each
    EXPECT_GE(test.renderer.getStats().layersPushed, 3u);
    EXPECT_GT(test.renderer.getStats().passes, 4u);

    expectGolden(test, "layers_nested");
}

TEST_F(RmlRendererP1GoldenTest, ScissorScroll){
    JASPER_REQUIRE_GPU();
    P1Context test;
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.load("p1/scissor_scroll.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    Rml::Element* scroller = document->GetElementById("scroller");
    ASSERT_NE(scroller, nullptr);
    scroller->SetScrollTop(120.0f);
    ASSERT_TRUE(test.renderFrame());
    EXPECT_FLOAT_EQ(scroller->GetScrollTop(), 120.0f);

    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, width};
    // rows of 60 px scrolled by 120: row 2 at the top, row 5 at the bottom
    EXPECT_TRUE(probe.is(150, 45, 0x30, 0x30, 0xc0));
    EXPECT_TRUE(probe.is(150, 275, 0xc0, 0x30, 0xc0));
    // nothing drawn above or below the box
    EXPECT_TRUE(probe.isBackground(150, 35));
    EXPECT_TRUE(probe.isBackground(150, 285));
    // a scissor, no clip mask: one pass
    EXPECT_EQ(test.renderer.getStats().passes, 1u);

    expectGolden(test, "scissor_scroll");
}

TEST_F(RmlRendererP1GoldenTest, OpacityText){
    JASPER_REQUIRE_GPU();
    P1Context test;
    std::vector<uint8_t> screen;
    ASSERT_TRUE(draw(test, "opacity_text", screen));
    PixelProbe probe{screen, width};

    // white at 50 % over the background: 128 + 30 * 127 / 255
    EXPECT_TRUE(probe.is(120, 310, 143, 143, 143));
    // the gradient at the left edge
    EXPECT_TRUE(probe.is(22, 230, 255, 0, 0, 255, 6));

    expectGolden(test, "opacity_text");
}

TEST_F(RmlRendererP1GoldenTest, ManyElements){
    JASPER_REQUIRE_GPU();
    P1Context test;
    ASSERT_TRUE(test.isValid());

    // 500 small rounded buttons: one pass, few pipeline switches
    std::string rml = "<html><head><link type=\"text/css\" href=\"../common/base.css\"/><link type=\"text/css\" href=\"p1.css\"/>"
                      "<style>#grid { position: absolute; left: 6px; top: 12px; width: 500px; display: flex; flex-wrap: wrap; }"
                      " #grid div { width: 14px; height: 10px; margin-right: 4px; margin-bottom: 3px; border-radius: 4px; border: 1px solid #ffffff; }"
                      "</style></head><body><div id=\"grid\">";
    for (int i = 0; i < 500; i++){
        char colour[32];
        std::snprintf(colour, sizeof(colour), "#%02x%02x%02x", (i * 13) % 256, (i / 25) * 12, 255 - (i % 25) * 8);
        rml += std::string("<div style=\"background-color: ") + colour + "\"/>";
    }
    rml += "</div></body></html>";
    ASSERT_NE(test.loadFromMemory(rml, "p1/many_elements.html"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());

    const HtmlUIRenderStats& stats = test.renderer.getStats();
    EXPECT_EQ(stats.passes, 1u);
    EXPECT_LE(stats.pipelineApplies, 10u);
    EXPECT_EQ(stats.pipelineCacheMisses, 0u);
    EXPECT_GE(stats.geometrySubmitted, 500u);

    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    PixelProbe probe{screen, width};
    // button i at (6 + 20 * (i % 25), 12 + 15 * (i / 25)), its fill inside the 1 px border
    for (int i : {0, 24, 25, 250, 499}){
        const int x = 6 + 20 * (i % 25) + 8;
        const int y = 12 + 15 * (i / 25) + 6;
        EXPECT_TRUE(probe.is(x, y, (i * 13) % 256, (i / 25) * 12, 255 - (i % 25) * 8)) << "button " << i;
    }

    expectGolden(test, "many_elements");
}

// (upstream) unrelated text drawn earlier and a preformatted line's trailing line feed leave a
// glyph's pixels as they are: fractional scaling shows any edge sampling difference
TEST_F(RmlRendererP1GoldenTest, GlyphPixelsIgnoreEarlierText){
    JASPER_REQUIRE_GPU();
    // Unrelated text rendered earlier and a preformatted line's trailing line feed must
    // not change a glyph's pixels. Fractional scaling exposes edge sampling differences.
    const std::string beforeText = "<rml><head><style>body { margin:0; font-family:sans-serif; font-size:64px; color:#fff;} "
                                   "div {display:block;position:absolute;left:20px;top:30px;white-space:pre;}</style></head><body><div>";
    const std::string afterText = "</div></body></rml>";
    std::vector<uint8_t> text[2];
    for (int warm = 0; warm < 2; warm++){
        RenderTestContext test(283, 159, 256, 144, 4);
        ASSERT_TRUE(test.isValid());
        if (warm){
            Rml::ElementDocument* preceding = test.loadFromMemory(beforeText + "W" + afterText, "preceding-text.rml");
            ASSERT_NE(preceding, nullptr);
            ASSERT_TRUE(test.renderUntilDrawn());
            preceding->Close();
            test.context->Update();
        }
        ASSERT_NE(test.loadFromMemory(beforeText + (warm ? "H\n" : "H") + afterText, "same-text.rml"), nullptr);
        ASSERT_TRUE(test.renderUntilDrawn());
        ASSERT_TRUE(test.readScreen(text[warm]));
    }
    ASSERT_EQ(text[0].size(), text[1].size());
    int difference = 0;
    bool inked = false;
    for (size_t i = 0; i < text[0].size(); i++){
        difference = std::max(difference, std::abs(int(text[0][i]) - int(text[1][i])));
        inked |= i % 4 == 0 && text[0][i] > 60;
    }
    EXPECT_TRUE(inked);
    // Same 24-channel tolerance as the reviewed examples' exact fresh-start check.
    EXPECT_LE(difference, 24) << "unrelated prior text or a trailing line feed changed the glyph pixels";
}
