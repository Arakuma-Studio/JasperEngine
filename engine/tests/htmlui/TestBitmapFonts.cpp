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

#include <cstring>
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
