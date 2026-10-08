// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Full-path goldens of text (RmlFontEngine on the engine's text stack, glyph pages through
// RmlRenderer): sizes, colors, alignment, spacing, wrapping, decoration (text_basic);
// Cyrillic, Greek, Arabic RTL, bidi, family fallback, an uncovered codepoint
// (text_fallback_rtl); faces loaded with HtmlUI::loadFontFace (text_loaded_font); the same
// text at pixel ratio 2 (hidpi).

#include "TestGoldenCommon.h"

#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "RmlFontEngine.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/FontRegistry.h"

#include <RmlUi/Core/Context.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // probes of text_basic.html at a pixel ratio (1 at 640x360, 2 at 1280x720)
    void probeTextBasic(const Screen& screen, int s){
        // the four sizes: taller ink for bigger text, each in its color
        InkBox s12 = screen.inkOnBackground({10 * s, 10 * s, 320 * s, 28 * s});
        InkBox s16 = screen.inkOnBackground({10 * s, 30 * s, 320 * s, 52 * s});
        InkBox s24 = screen.inkOnBackground({10 * s, 55 * s, 320 * s, 88 * s});
        InkBox s48 = screen.inkOnBackground({10 * s, 90 * s, 320 * s, 155 * s});
        ASSERT_FALSE(s12.empty());
        ASSERT_FALSE(s16.empty());
        ASSERT_FALSE(s24.empty());
        ASSERT_FALSE(s48.empty());
        EXPECT_LT(s12.height(), s16.height());
        EXPECT_LT(s16.height(), s24.height());
        EXPECT_LT(s24.height(), s48.height());
        // "48 px Hxg": cap height ~ 0.71 em, descender below the baseline
        EXPECT_GT(s48.height(), 40 * s);
        EXPECT_LT(s48.height(), 56 * s);
        EXPECT_NEAR(s12.x0, 11 * s, 2 * s);
        EXPECT_NEAR(s48.x0, 13 * s, 3 * s);
        // solid glyph stems reach the text color
        EXPECT_GT(screen.countColor({10 * s, 90 * s, 320 * s, 155 * s}, 0x60, 0x80, 0xff, 12), size_t(100 * s * s));
        EXPECT_GT(screen.countColor({10 * s, 55 * s, 320 * s, 88 * s}, 0x60, 0xff, 0x60, 12), size_t(30 * s * s));
        EXPECT_GT(screen.countColor({10 * s, 30 * s, 320 * s, 52 * s}, 0xff, 0x60, 0x60, 16), size_t(10 * s * s));

        // spans in their own colors, left to right
        const size_t redCount = screen.countColor({10 * s, 160 * s, 320 * s, 195 * s}, 0xff, 0x00, 0x00, 12);
        const size_t greenCount = screen.countColor({10 * s, 160 * s, 320 * s, 195 * s}, 0x00, 0xff, 0x00, 12);
        const size_t blueCount = screen.countColor({10 * s, 160 * s, 320 * s, 195 * s}, 0x40, 0x40, 0xff, 12);
        EXPECT_GT(redCount, size_t(20 * s * s));
        EXPECT_GT(greenCount, size_t(20 * s * s));
        EXPECT_GT(blueCount, size_t(20 * s * s));

        // underline: a full-width yellow run under the text (24 px font, line 200..228)
        InkBox underline = screen.inkOnBackground({10 * s, 200 * s, 320 * s, 235 * s});
        ASSERT_FALSE(underline.empty());
        int longestRun = 0;
        for (int y = underline.y0; y <= underline.y1; y++){
            int run = 0;
            for (int x = underline.x0; x <= underline.x1; x++){
                const uint8_t* p = screen.at(x, y);
                run = (p[0] > 180 && p[1] > 180 && p[2] < 80) ? run + 1 : 0;
                longestRun = std::max(longestRun, run);
            }
        }
        EXPECT_GT(longestRun, underline.width() * 9 / 10) << "no underline across the text";

        // line-height 40 px: the second line's ink starts 40 px below the first's
        InkBox line1 = screen.ink({10 * s, 240 * s, 310 * s, 280 * s}, 0x30, 0x30, 0x30);
        InkBox line2 = screen.ink({10 * s, 280 * s, 310 * s, 320 * s}, 0x30, 0x30, 0x30);
        ASSERT_FALSE(line1.empty());
        ASSERT_FALSE(line2.empty());
        EXPECT_NEAR(line2.y0 - line1.y0, 40 * s, 1 * s);
        EXPECT_TRUE(screen.is(300 * s, 245 * s, 0x30, 0x30, 0x30));

        // text-align in 300 px boxes from x = 330
        InkBox left = screen.ink({330 * s, 10 * s, 630 * s, 36 * s}, 0x30, 0x30, 0x30);
        InkBox center = screen.ink({330 * s, 40 * s, 630 * s, 66 * s}, 0x30, 0x30, 0x30);
        InkBox right = screen.ink({330 * s, 70 * s, 630 * s, 96 * s}, 0x30, 0x30, 0x30);
        ASSERT_FALSE(left.empty());
        ASSERT_FALSE(center.empty());
        ASSERT_FALSE(right.empty());
        EXPECT_LT(left.x0, 334 * s);
        EXPECT_NEAR(center.centerX(), 480.0f * s, 3.0f * s);
        EXPECT_GT(right.x1, 625 * s);

        // letter-spacing 6 px over "Spacing" (7 glyphs): about 6 * 6 px wider
        InkBox tight = screen.inkOnBackground({330 * s, 105 * s, 630 * s, 132 * s});
        InkBox spaced = screen.inkOnBackground({330 * s, 135 * s, 630 * s, 162 * s});
        ASSERT_FALSE(tight.empty());
        ASSERT_FALSE(spaced.empty());
        EXPECT_NEAR(spaced.width() - tight.width(), 36 * s, 3 * s);

        // the paragraph wraps inside its 300 px box: four 22 px lines (ascender of the first
        // to descender of the last: 3 line steps plus a glyph height)
        InkBox para = screen.ink({330 * s, 170 * s, 640 * s, 360 * s}, 0x30, 0x30, 0x30);
        ASSERT_FALSE(para.empty());
        EXPECT_LE(para.x1, 630 * s);
        EXPECT_GE(para.height(), 22 * 3 * s + 10 * s);
        EXPECT_LT(para.height(), 22 * 4 * s);
        EXPECT_TRUE(screen.is(625 * s, (170 + 88 - 2) * s, 0x30, 0x30, 0x30));
        EXPECT_TRUE(screen.isBackground(625 * s, (170 + 88 + 2) * s));
        EXPECT_TRUE(screen.isBackground(631 * s, 200 * s));
    }

}

TEST_F(FullPathGoldenTest, TextBasic){
    SceneFixture fixture;
    fixture.addDocument("golden/text_basic.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));
    probeTextBasic(screen, 1);

    EXPECT_SCREEN_GOLDEN(screen, "text_basic");
}

TEST_F(FullPathGoldenTest, HiDpi){
    // canvas 640x360 on a 1280x720 screen: the target is 1280x720, glyphs rasterize at 2x
    GpuEnvironment::setView(1280, 720, 640, 360, Scaling::FITWIDTH);
    ASSERT_EQ(Engine::getCanvasWidth(), 640);
    ASSERT_EQ(Engine::getCanvasHeight(), 360);

    SceneFixture fixture;
    fixture.addDocument("golden/text_basic.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));
    ASSERT_EQ(screen.width, 1280);
    ASSERT_EQ(screen.height, 720);
    HtmlUIContext* context = fixture.getScene().getSystem<UIDocumentSystem>()->getContext();
    EXPECT_FLOAT_EQ(context->getPixelRatio(), 2.0f);
    // the context's own raster scale (phase 1: per render manager)
    EXPECT_FLOAT_EQ(HtmlUI::getFontEngine().getRasterScale(context->getRmlContext()->GetRenderManager()), 2.0f);
    probeTextBasic(screen, 2);

    EXPECT_SCREEN_GOLDEN(screen, "hidpi");
}

TEST_F(FullPathGoldenTest, TextFallbackRtl){
    // the one expected warning: "Missing Font" is not loaded (logged once)
    expectedWarnings = 1;

    SceneFixture fixture;
    fixture.addDocument("golden/text_fallback_rtl.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "font-family 'missing font' is not loaded"), 1u);

    // Latin, Cyrillic (#ffc040) and Greek (#40c0ff) lines are drawn in their colors
    InkBox latin = screen.inkOnBackground({10, 10, 320, 42});
    InkBox cyrillic = screen.inkOnBackground({10, 45, 320, 77});
    InkBox greek = screen.inkOnBackground({10, 80, 320, 112});
    EXPECT_GT(latin.width(), 150);
    EXPECT_GT(cyrillic.width(), 100);
    EXPECT_GT(greek.width(), 120);
    EXPECT_GT(screen.countColor({10, 45, 320, 77}, 0xff, 0xc0, 0x40, 16), 40u);
    EXPECT_GT(screen.countColor({10, 80, 320, 112}, 0x40, 0xc0, 0xff, 16), 40u);

    // Arabic, right to right-aligned in its box, joined (connected ink, no gaps per letter)
    InkBox arabic = screen.ink({330, 10, 630, 120}, 0x30, 0x30, 0x30);
    ASSERT_FALSE(arabic.empty());
    EXPECT_GT(arabic.x1, 624);
    EXPECT_GT(arabic.width(), 150);

    // mixed bidi line, the missing family falling back to the default face
    EXPECT_GT(screen.inkOnBackground({10, 130, 320, 165}).width(), 150);
    InkBox missing = screen.inkOnBackground({10, 175, 320, 210});
    EXPECT_GT(missing.width(), 150);
    EXPECT_GT(screen.countColor({10, 175, 320, 210}, 0xff, 0x80, 0xc0, 16), 30u);
    // the uncovered codepoint does not stop the line: " end" follows it
    InkBox uncovered = screen.inkOnBackground({10, 220, 320, 255});
    EXPECT_GT(uncovered.width(), 150);

    EXPECT_SCREEN_GOLDEN(screen, "text_fallback_rtl");
}

TEST_F(FullPathGoldenTest, TextLoadedFont){
    // registered before RmlUi starts: the faces live in the engine's FontRegistry
    ASSERT_TRUE(HtmlUI::loadFontFace("fonts/Roboto-Bold.ttf"));
    ASSERT_TRUE(HtmlUI::loadFontFace("fonts/Roboto-Italic.ttf"));
    ASSERT_NE(FontRegistry::find("Roboto", 700, false), nullptr);
    EXPECT_EQ(FontRegistry::find("Roboto", 700, false)->getWeight(), 700);
    EXPECT_TRUE(FontRegistry::find("Roboto", 400, true)->isItalic());

    {
        SceneFixture fixture;
        fixture.addDocument("golden/text_loaded_font.html");
        ASSERT_TRUE(fixture.waitUntilLoaded());

        Screen screen;
        ASSERT_TRUE(capture(screen));

        // "IIIIII" at 48 px in each face (x < 96, before the word): bold stems are heavier,
        // italic ones lean right
        InkBox regular = screen.inkOnBackground({0, 10, 96, 75});
        InkBox bold = screen.inkOnBackground({0, 80, 96, 145});
        InkBox italic = screen.inkOnBackground({0, 150, 96, 215});
        ASSERT_FALSE(regular.empty());
        ASSERT_FALSE(bold.empty());
        ASSERT_FALSE(italic.empty());
        EXPECT_GT(bold.count, regular.count * 13 / 10);

        auto rowCentroid = [&](const InkBox& box, int y){
            double sum = 0.0, weight = 0.0;
            for (int x = box.x0; x <= box.x1; x++){
                const uint8_t* p = screen.at(x, y);
                const double w = std::max(0, int(p[0]) - goldenBackground);
                sum += w * x;
                weight += w;
            }
            return weight > 0.0 ? sum / weight : 0.0;
        };
        const double regularLean = rowCentroid(regular, regular.y0 + 3) - rowCentroid(regular, regular.y1 - 3);
        const double italicLean = rowCentroid(italic, italic.y0 + 3) - rowCentroid(italic, italic.y1 - 3);
        EXPECT_NEAR(regularLean, 0.0, 1.0);
        EXPECT_GT(italicLean, 3.0);
        // same cap height in the three faces (one family, one size)
        EXPECT_NEAR(bold.height(), regular.height(), 1);
        EXPECT_NEAR(italic.height(), regular.height(), 1);

        // the inline spans pick their faces too
        EXPECT_GT(screen.countColor({10, 230, 640, 265}, 0xff, 0xc0, 0x40, 16), 40u);
        EXPECT_GT(screen.countColor({10, 230, 640, 265}, 0x40, 0xc0, 0xff, 16), 20u);

        EXPECT_SCREEN_GOLDEN(screen, "text_loaded_font");
    }

    // the scene released RmlUi: the next tests see only the built-in faces again
    EXPECT_FALSE(HtmlUI::isInitialized());
    FontRegistry::clear();
}
