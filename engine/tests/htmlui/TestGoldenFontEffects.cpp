// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Full-path goldens of phase 1's text features (spec §3.13, §5.2): G26 font effects
// (shadow, outline, glow, blur through RmlFontEngine's effect layers) and synthetic
// bold/italic of the built-in Roboto Regular. 512x384 canvas, UI clock pinned.

#include "TestGoldenCommon.h"

#include "HtmlUIClock.h"
#include "util/FontRegistry.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the bounding box of the pixels within tolerance of a colour
    InkBox colorBox(const Screen& screen, const PixelRect& region, int r, int g, int b, int tolerance){
        InkBox box;
        for (int y = std::max(region.y0, 0); y < std::min(region.y1, screen.height); y++){
            for (int x = std::max(region.x0, 0); x < std::min(region.x1, screen.width); x++){
                const uint8_t* p = screen.at(x, y);
                if (std::abs(p[0] - r) > tolerance || std::abs(p[1] - g) > tolerance || std::abs(p[2] - b) > tolerance)
                    continue;
                if (box.count == 0){
                    box.x0 = box.x1 = x;
                    box.y0 = box.y1 = y;
                }else{
                    box.x0 = std::min(box.x0, x);
                    box.x1 = std::max(box.x1, x);
                    box.y0 = std::min(box.y0, y);
                    box.y1 = std::max(box.y1, y);
                }
                box.count++;
            }
        }
        return box;
    }

    // x where each inked run of row y starts, left to right (the stems of "IIIIII")
    std::vector<int> runStarts(const Screen& screen, int y, int x0, int x1){
        std::vector<int> starts;
        bool inside = false;
        for (int x = x0; x < x1; x++){
            const bool ink = screen.at(x, y)[0] > 128;
            if (ink && !inside)
                starts.push_back(x);
            inside = ink;
        }
        return starts;
    }

    class TextFeatureGoldenTest : public FullPathGoldenTest {
    protected:
        void SetUp() override{
            FullPathGoldenTest::SetUp();
            GpuEnvironment::setView(512, 384, 512, 384, Scaling::FITWIDTH);
            HtmlUIClock::setFixedTime(0.0);
        }

        void TearDown() override{
            HtmlUIClock::setFixedTime(std::nullopt);
            FullPathGoldenTest::TearDown();
        }
    };

}

TEST_F(TextFeatureGoldenTest, FontEffects){
    SceneFixture fixture;
    fixture.addDocument("p1/font_effects.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));
    ASSERT_EQ(screen.width, 512);
    ASSERT_EQ(screen.height, 384);

    // the plain line: white text only, no effect colour anywhere near it
    const PixelRect plain{0, 305, 512, 360};
    EXPECT_GT(screen.countColor(plain, 0xff, 0xff, 0xff, 8), 200u);
    EXPECT_EQ(screen.countColor(plain, 0xff, 0x40, 0x40, 40), 0u);

    // shadow: the red copy, 3 px right and down of the white text
    const PixelRect shadowLine{0, 5, 512, 65};
    const size_t shadowRed = screen.countColor(shadowLine, 0xff, 0x40, 0x40, 24);
    EXPECT_GT(shadowRed, 150u);
    InkBox shadowText = colorBox(screen, shadowLine, 0xff, 0xff, 0xff, 8);
    InkBox shadowColor = colorBox(screen, shadowLine, 0xff, 0x40, 0x40, 24);
    ASSERT_FALSE(shadowText.empty());
    ASSERT_FALSE(shadowColor.empty());
    EXPECT_NEAR(shadowColor.x1 - shadowText.x1, 3, 1);
    EXPECT_NEAR(shadowColor.y1 - shadowText.y1, 3, 1);
    EXPECT_NEAR(shadowColor.y0 - shadowText.y0, 3, 1);

    // outline: orange around dark glyphs; the glyph interior keeps the text colour
    const PixelRect outlineLine{0, 65, 512, 125};
    EXPECT_GT(screen.countColor(outlineLine, 0xff, 0x80, 0x00, 24), 400u);
    EXPECT_GT(screen.countColor(outlineLine, 0x20, 0x20, 0x20, 6), 100u);

    // glow: green that spreads beyond the white glyphs by about outline + blur widths
    const PixelRect glowLine{0, 125, 512, 185};
    InkBox glowWhite = colorBox(screen, glowLine, 0xff, 0xff, 0xff, 8);
    InkBox glowInk = screen.inkOnBackground(glowLine, 8);
    ASSERT_FALSE(glowWhite.empty());
    EXPECT_GE(glowWhite.x0 - glowInk.x0, 3);
    EXPECT_LE(glowWhite.x0 - glowInk.x0, 9);
    EXPECT_GT(screen.countColor(glowLine, 0x40, 0xff, 0x40, 60), 200u);

    // blur: soft blue around the white text
    const PixelRect blurLine{0, 185, 512, 245};
    InkBox blurWhite = colorBox(screen, blurLine, 0xff, 0xff, 0xff, 8);
    InkBox blurInk = screen.inkOnBackground(blurLine, 8);
    ASSERT_FALSE(blurWhite.empty());
    EXPECT_GE(blurWhite.x0 - blurInk.x0, 2);
    size_t blueish = 0;
    for (int y = blurLine.y0; y < blurLine.y1; y++){
        for (int x = 0; x < 512; x++){
            const uint8_t* p = screen.at(x, y);
            blueish += (p[2] > p[0] + 20 && p[2] > p[1] + 10);
        }
    }
    EXPECT_GT(blueish, 300u);

    // combined: magenta shadow behind, a black outline, yellow text
    const PixelRect combinedLine{0, 245, 512, 305};
    EXPECT_GT(screen.countColor(combinedLine, 0xff, 0x00, 0xff, 24), 150u);
    EXPECT_GT(screen.countColor(combinedLine, 0xff, 0xff, 0x00, 24), 150u);
    EXPECT_GT(screen.countColor(combinedLine, 0x00, 0x00, 0x00, 12), 100u);

    EXPECT_SCREEN_GOLDEN(screen, "p1/font_effects");
}

TEST_F(TextFeatureGoldenTest, FontSynthesis){
    // only the built-in faces: bold and italic are synthesized
    FontRegistry::clear();

    SceneFixture fixture;
    fixture.addDocument("p1/font_synthesis.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // "IIIIII" at 40 px from x = 20 (the stems only: x < 92)
    InkBox regular = screen.inkOnBackground({0, 10, 92, 65});
    InkBox bold = screen.inkOnBackground({0, 70, 92, 125});
    InkBox italic = screen.inkOnBackground({0, 130, 92, 185});
    InkBox both = screen.inkOnBackground({0, 190, 92, 245});
    ASSERT_FALSE(regular.empty());
    ASSERT_FALSE(bold.empty());
    ASSERT_FALSE(italic.empty());
    ASSERT_FALSE(both.empty());
    // bold stems carry more ink
    EXPECT_GT(bold.count, regular.count * 13 / 10);
    EXPECT_GT(both.count, regular.count * 12 / 10);   // the slanted stems are partly cut at x = 92

    // the advances stay the regular face's (Chromium's fake bold): every stem starts where the
    // regular one does (the dilation spreads right)
    const std::vector<int> regularStems = runStarts(screen, (regular.y0 + regular.y1) / 2, 0, 92);
    const std::vector<int> boldStems = runStarts(screen, (bold.y0 + bold.y1) / 2, 0, 92);
    ASSERT_GE(regularStems.size(), 6u);
    ASSERT_GE(boldStems.size(), 6u);
    EXPECT_NEAR(boldStems[0], regularStems[0], 1);
    EXPECT_NEAR(boldStems[5], regularStems[5], 1);

    // the first stem's left edge near its top against near its bottom: tan 12deg of the rows between
    auto lean = [&screen](const InkBox& box){
        const std::vector<int> top = runStarts(screen, box.y0 + 3, 0, 92);
        const std::vector<int> bottom = runStarts(screen, box.y1 - 3, 0, 92);
        return (top.empty() || bottom.empty()) ? -100.0 : double(top[0] - bottom[0]);
    };
    const double expectedLean = (italic.height() - 7) * 0.2126;
    EXPECT_NEAR(lean(italic), expectedLean, 1.0);
    EXPECT_NEAR(lean(both), expectedLean, 1.0);
    EXPECT_NEAR(lean(regular), 0.0, 1.0);
    EXPECT_NEAR(lean(bold), 0.0, 1.0);
    // the same cap height; bold grows by its 2 px dilation
    EXPECT_NEAR(italic.height(), regular.height(), 1);
    EXPECT_NEAR(bold.height(), regular.height() + 2, 1);

    EXPECT_SCREEN_GOLDEN(screen, "p1/font_synthesis");
}
