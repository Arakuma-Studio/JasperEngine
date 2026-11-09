// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC14 (docs/specs/S51-pixel-fonts.md R15): bitmap faces in the HTML UI. A .bitmapfont and a
// .fnt registered by @font-face draw at 1x, 2x and 3x their native size with every font pixel a
// whole block of device pixels, at pixel ratio 1 and 1.5; font-effect outline(1px) and a sharp
// shadow draw pixel styles, a glow on a bitmap face is dropped; no distance page is made.

#include "TestGoldenCommon.h"

#include "HtmlUI.h"
#include "HtmlUIClock.h"
#include "RmlFontEngine.h"
#include "RmlRenderer.h"
#include "FixtureRunner.h"
#include "harness/GpuEnvironment.h"
#include "pixelart/BitmapFont.h"

#include <cstring>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class TestBitmapFonts : public FullPathGoldenTest {
    protected:
        void TearDown() override{
            HtmlUIClock::setFixedTime(std::nullopt);
            GpuEnvironment::resetView();
            FullPathGoldenTest::TearDown();
        }
    };

    // every pixel of the box equals the top-left pixel of its d x d block (blocks from the box)
    bool tbfBlocks(const Screen& screen, const InkBox& box, int d){
        for (int y = box.y0; y <= box.y1; y++){
            for (int x = box.x0; x <= box.x1; x++){
                const uint8_t* p = screen.at(x, y);
                const uint8_t* q = screen.at(box.x0 + (x - box.x0) / d * d, box.y0 + (y - box.y0) / d * d);
                if (std::memcmp(p, q, 3) != 0)
                    return false;
            }
        }
        return true;
    }

    // the colours in a region, as 0xRRGGBB
    std::set<uint32_t> tbfColours(const Screen& screen, const PixelRect& r){
        std::set<uint32_t> out;
        for (int y = std::max(r.y0, 0); y < std::min(r.y1, screen.height); y++)
            for (int x = std::max(r.x0, 0); x < std::min(r.x1, screen.width); x++){
                const uint8_t* p = screen.at(x, y);
                out.insert((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
            }
        return out;
    }

    void tbfCheckScales(const Screen& screen, float ratio){
        auto px = [ratio](int v){ return (int)std::lround(v * ratio); };
        const PixelRect lines[3] = {{0, px(8), screen.width, px(28)}, {0, px(28), screen.width, px(58)}, {0, px(58), screen.width, px(98)}};
        int heights[3] = {};
        for (int k = 1; k <= 3; k++){
            const InkBox box = screen.ink(lines[k - 1], 0, 0, 0, 8);
            ASSERT_FALSE(box.empty()) << "x" << k;
            const int d = std::max(1, (int)std::lround(k * ratio));
            EXPECT_TRUE(tbfBlocks(screen, box, d)) << "x" << k << " at " << ratio << ": every font pixel " << d << " x " << d;
            EXPECT_EQ(box.height() % d, 0) << "x" << k;
            heights[k - 1] = box.height() / d;
            // NEAREST: white and black only
            EXPECT_EQ(tbfColours(screen, lines[k - 1]), (std::set<uint32_t>{0x000000u, 0xffffffu})) << "x" << k;
        }
        // the same glyphs in font pixels at every scale
        EXPECT_EQ(heights[0], heights[1]);
        EXPECT_EQ(heights[1], heights[2]);
    }

}

TEST_F(TestBitmapFonts, ScalesAndEffects){
    GpuEnvironment::setView(512, 384, 512, 384, Scaling::FITWIDTH);
    HtmlUIClock::setFixedTime(0.0);
    SceneFixture fixture;
    fixture.addDocument("s51/bitmap_fonts.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));
    ASSERT_EQ(screen.width, 512);
    EXPECT_TRUE(RmlFontEngine::isBitmapFamily("Hud"));
    EXPECT_TRUE(RmlFontEngine::isBitmapFamily("BmHud"));

    tbfCheckScales(screen, 1.0f);

    // the .fnt face at 2x
    const InkBox fnt = screen.ink({0, 98, 512, 128}, 0, 0, 0, 8);
    ASSERT_FALSE(fnt.empty());
    EXPECT_TRUE(tbfBlocks(screen, fnt, 2));

    // outline(1px): red pixel rings, whole font pixels (2 x 2), no blend
    const PixelRect outline{0, 128, 512, 163};
    EXPECT_GT(screen.countColor(outline, 0xff, 0x00, 0x00, 0), 50u);
    EXPECT_EQ(tbfColours(screen, outline), (std::set<uint32_t>{0x000000u, 0xffffffu, 0xff0000u}));
    // a sharp shadow: blue moved one font pixel (2 device px) right and down
    const PixelRect shadow{0, 163, 512, 198};
    EXPECT_GT(screen.countColor(shadow, 0x00, 0x00, 0xff, 0), 20u);
    EXPECT_EQ(tbfColours(screen, shadow), (std::set<uint32_t>{0x000000u, 0xffffffu, 0x0000ffu}));
    // a glow on a bitmap face is dropped: the plain text only
    const PixelRect glow{0, 198, 512, 240};
    EXPECT_EQ(screen.countColor(glow, 0x00, 0xff, 0x00, 60), 0u);
    EXPECT_GT(screen.countColor(glow, 0xff, 0xff, 0xff, 0), 50u);

    // never a distance glyph for a bitmap face
    EXPECT_EQ(HtmlUI::getFontEngine().getDistancePageCount(), 0u);

    EXPECT_SCREEN_GOLDEN(screen, "s51/bitmap_fonts");
}

TEST_F(TestBitmapFonts, PixelRatioOneAndAHalf){
    // canvas 512x384 on a 768x576 target: a font pixel is round(k * 1.5) device pixels
    GpuEnvironment::setView(768, 576, 512, 384, Scaling::FITWIDTH);
    HtmlUIClock::setFixedTime(0.0);
    SceneFixture fixture;
    fixture.addDocument("s51/bitmap_fonts.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));
    ASSERT_EQ(screen.width, 768);
    tbfCheckScales(screen, 1.5f);
    EXPECT_SCREEN_GOLDEN(screen, "s51/bitmap_fonts_1_5x");
}

namespace {

    // the distinct colours of a capture (straight RGB of opaque pixels), as 0xRRGGBB
    std::set<uint32_t> tbfImageColours(const compat::Image& image){
        std::set<uint32_t> out;
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4)
            out.insert((uint32_t(image.rgba[i]) << 16) | (uint32_t(image.rgba[i + 1]) << 8) | image.rgba[i + 2]);
        return out;
    }

    // the image's rows [y0, y1)
    compat::Image tbfRows(const compat::Image& image, int y0, int y1){
        compat::Image out;
        out.width = image.width;
        out.height = std::max(0, std::min(y1, image.height) - y0);
        out.rgba.assign(image.rgba.begin() + (size_t)y0 * (size_t)image.width * 4,
                        image.rgba.begin() + (size_t)(y0 + out.height) * (size_t)image.width * 4);
        return out;
    }

    bool tbfLoad(FixtureRunner& runner, const std::string& file){
        std::string error;
        if (!runner.load("fixtures/s51/" + file, &error)){
            ADD_FAILURE() << file << ": " << error;
            return false;
        }
        runner.frameAt(0.1, true);
        runner.frameAt(0.2, true);
        return true;
    }

}

TEST(TestBitmapFontsRunner, TransformMagnifiesNearestAndTextShadowIsSharp){
    // R15: a scale(2.5) transform magnifies the face's pixels as blocks, no filtered colours (one
    // sample per pixel: the edges are not blended); a sharp CSS text-shadow draws only its colour
    JASPER_REQUIRE_GPU();
    FixtureRunner runner(256, 160, 1);
    ASSERT_TRUE(runner.valid());
    ASSERT_TRUE(tbfLoad(runner, "bitmap_transform.html"));
    FixtureRunner::Capture capture;
    ASSERT_TRUE(runner.capture(capture));
    const std::set<uint32_t> scaled = tbfImageColours(tbfRows(capture.image, 0, 70));
    EXPECT_TRUE(scaled.count(0xffffffu));
    EXPECT_EQ(scaled, (std::set<uint32_t>{0x000000u, 0xffffffu})) << "filtered texels under the transform";
    const std::set<uint32_t> shadow = tbfImageColours(tbfRows(capture.image, 75, 110));
    EXPECT_TRUE(shadow.count(0x0000ffu)) << "the text-shadow draws";
    EXPECT_EQ(shadow, (std::set<uint32_t>{0x000000u, 0xffffffu, 0x0000ffu}));
    EXPECT_GE(HtmlUI::getFontEngine().getPixelPageCount(), 1u);
}

TEST(TestBitmapFontsRunner, ThreeSizesUseNoMorePagesOrDrawsThanVectorText){
    // R21: bitmap text at 3 sizes adds no draws over the same text in a vector font, its glyphs
    // (each size one block-replicated copy) fit one glyph page, and it stays within S16's
    // per-frame glyph budget
    JASPER_REQUIRE_GPU();
    FixtureRunner runner(320, 120, 1);
    ASSERT_TRUE(runner.valid());
    RmlFontEngine& engine = HtmlUI::getFontEngine();
    ASSERT_TRUE(tbfLoad(runner, "r21_vector.html"));
    const uint32_t vectorDraws = runner.renderer()->getStats().drawCalls;
    ASSERT_TRUE(runner.load("fixtures/s51/r21_bitmap.html"));
    runner.frameAt(0.1, true);
    EXPECT_LE(runner.renderer()->getStats().glyphsRasterized, (uint32_t)RmlFontEngine::settleGlyphBudget);
    runner.frameAt(0.2, true);
    const uint32_t bitmapDraws = runner.renderer()->getStats().drawCalls;
    EXPECT_GT(bitmapDraws, 0u);
    EXPECT_LE(bitmapDraws, vectorDraws);
    EXPECT_EQ(engine.getPixelPageCount(), 1u);
    EXPECT_EQ(engine.getDistancePageCount(), 0u);
    std::printf("[ ui-bench ] S51 R21: bitmap %u draws over %zu glyph page(s), vector %u draws\n", bitmapDraws,
                engine.getPixelPageCount(), vectorDraws);
}

TEST(TestBitmapFontsRunner, AFamilyRegisteredAgainDropsTheOldGlyphs){
    // a family registered with another font draws the new font's glyphs: the old font's go
    // (keyed by its serial, not its address, which a later font may reuse)
    JASPER_REQUIRE_GPU();
    FixtureRunner runner(160, 60, 1);
    ASSERT_TRUE(runner.valid());
    RmlFontEngine& engine = HtmlUI::getFontEngine();
    ASSERT_TRUE(tbfLoad(runner, "swap_a.html"));
    std::shared_ptr<pixelart::BitmapFont> first = RmlFontEngine::bitmapFamilyFont("Swap");
    ASSERT_TRUE(first);
    const uint64_t firstSerial = first->getSerial();
    EXPECT_GT(engine.getBitmapGlyphCount(firstSerial), 0u);
    ASSERT_TRUE(tbfLoad(runner, "swap_b.html"));
    std::shared_ptr<pixelart::BitmapFont> second = RmlFontEngine::bitmapFamilyFont("Swap");
    ASSERT_TRUE(second);
    EXPECT_NE(second->getSerial(), firstSerial);
    EXPECT_EQ(engine.getBitmapGlyphCount(firstSerial), 0u);
    EXPECT_GT(engine.getBitmapGlyphCount(second->getSerial()), 0u);
}
