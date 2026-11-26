// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Full-path goldens of text (RmlFontEngine on the engine's text stack, glyph pages through
// RmlRenderer): sizes, colors, alignment, spacing, wrapping, decoration (text_basic);
// Cyrillic, Greek, Arabic RTL, bidi, family fallback, an uncovered codepoint
// (text_fallback_rtl); faces loaded with HtmlUI::loadFontFace (text_loaded_font); the same
// text at pixel ratio 2 (hidpi); text-autosize (S93 R31).

#include "TestGoldenCommon.h"
#include "TestDocumentCommon.h"
#include "compat/PerfFixtures.h"

#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "RmlFontEngine.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/FontRegistry.h"
#include "localization/Localization.h"
#include "localization/StringTableRuntime.h"
#include "util/TitleArtLanguage.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <cstdio>
#include <string>

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
    fixture.addDocument("golden/text_basic.rml");
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
    fixture.addDocument("golden/text_basic.rml");
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
    fixture.addDocument("golden/text_fallback_rtl.rml");
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
        fixture.addDocument("golden/text_loaded_font.rml");
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

namespace {

    // the largest whole px in [min, max] at which `ref`, a copy of an element without text-autosize, keeps its content
    // inside its padding box (across, and down when `height`): the fit text-autosize must find, by brute force
    int bruteForceFit(Scene& scene, UIElement ref, int min, int max, bool height){
        for (int size = max; size > min; size--){
            ref.setStyle("font-size", std::to_string(size) + "px");
            updateDocuments(scene);
            if (ref.getScrollWidth() <= ref.getClientWidth() + 0.5f && (!height || ref.getScrollHeight() <= ref.getClientHeight() + 0.5f))
                return size;
        }
        ref.setStyle("font-size", std::to_string(min) + "px");
        updateDocuments(scene);
        return min;
    }

}

// S93 R31 (AC35): text-autosize fits the font size in the frame the document is first drawn: a short word, a long word
// (its width decides), a paragraph in a fixed box (its height decides) and a heading whose box follows its height,
// with an em child; each the largest whole px a brute-force relayout of an unfitted copy finds
TEST_F(FullPathGoldenTest, TextAutosizeFits){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("golden/text_autosize.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen first;
    ASSERT_TRUE(capture(first));
    Screen second;
    ASSERT_TRUE(capture(second));
    EXPECT_TRUE(first.rgba == second.rgba) << "the fitted size shows from the first frame on";
    // (fitted when the document shows: it was laid out hidden)
    EXPECT_NE(document.getElementById("short").getStyle("font-size"), "30px");

    struct Case {
        const char* id;
        int min, max;
        bool height;
    };
    const Case cases[] = {{"short", 18, 80, true}, {"long", 18, 80, true}, {"multi", 18, 80, true}, {"grow", 10, 200, false}};
    for (const Case& c : cases){
        UIElement element = document.getElementById(c.id);
        UIElement ref = document.getElementById(std::string(c.id) + "-ref");
        ASSERT_TRUE(element.isValid() && ref.isValid()) << c.id;
        const int expected = bruteForceFit(fixture.getScene(), ref, c.min, c.max, c.height);
        EXPECT_EQ(element.getStyle("font-size"), std::to_string(expected) + "px") << c.id;
        EXPECT_GT(expected, c.min) << c.id << ": the case must fit above its minimum";
        EXPECT_LT(expected, c.max) << c.id << ": the case must be held below its maximum";
        // the box that follows its content, the em child included, is the copy's at that size
        EXPECT_NEAR(element.getOffsetHeight(), ref.getOffsetHeight(), 0.5f) << c.id;
        EXPECT_LE(element.getScrollWidth(), element.getClientWidth() + 0.5f) << c.id;
    }
    // the brute force restyled hidden copies only: the frame is the first one
    Screen after;
    ASSERT_TRUE(capture(after));
    EXPECT_TRUE(after.rgba == first.rgba) << "the first frame already showed the fitted sizes";
    EXPECT_SCREEN_GOLDEN(second, "text_autosize");
}

// S93 R44: a fit costs at most 0.3 ms per element (perfSlack), and nothing while its text, box and font stay the same
TEST_F(FullPathGoldenTest, TextAutosizePerf){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("golden/text_autosize.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene& scene = fixture.getScene();
    UIElement multi = document.getElementById("multi");
    UIElement other = document.getElementById("short-ref");
    // (the first frame shows the document: its first layout fits it)
    GpuEnvironment::drawFrames(2);

    // a relayout for another element: no trial layout
    const int trials = Rml::ElementDocument::GetTextAutosizeTrialCount();
    for (int i = 0; i < 5; i++){
        other.setStyle("width", std::to_string(150 + i) + "px");
        updateDocuments(scene);
    }
    EXPECT_EQ(Rml::ElementDocument::GetTextAutosizeTrialCount(), trials);

    // a new text: one fit (at most 7 trials for 18..80 px)
    int round = 0;
    const auto refit = [&](){
        multi.setTextContent(round++ % 2 ? "The quick brown fox jumps over the lazy dog" : "The lazy dog sleeps under the quick brown fox");
        updateDocuments(scene);
    };
    const int before = Rml::ElementDocument::GetTextAutosizeTrialCount();
    refit();
    EXPECT_LE(Rml::ElementDocument::GetTextAutosizeTrialCount() - before, 8);
    EXPECT_GT(Rml::ElementDocument::GetTextAutosizeTrialCount() - before, 0);

    // the cost of the fit: the same relayout with and without it
    const double fitted = compat::gatedCpuMs(refit, 4, 0.3);
    multi.setStyle("text-autosize", "none");
    updateDocuments(scene);
    const double plain = compat::gatedCpuMs(refit, 4, 0.3);
    const double cost = fitted - plain;
    std::printf("[bench] TextAutosizePerf: fit %.3f ms (relayout %.3f ms with it, %.3f ms without)\n", cost, fitted, plain);
    if (compat::benchmarkPerfSlack() > 0.0){
        EXPECT_LE(cost, 0.3 * compat::benchmarkPerfSlack());
    }
}

// game-ui-localization.md AC9: the pseudo-locales on a keyed settings panel, Roboto Bold. en-XA
// shows the default language's text accented, 40 % longer and bracketed; ar-XB the same text right
// to left with the root's direction rtl
namespace {

    class GoldenText : public FullPathGoldenTest {
    protected:
        void SetUp() override{
            FullPathGoldenTest::SetUp();
            TitleArtLanguage::resetForTesting();
            Localization::resetForTesting();
            Localization::setEditorMode(true);
            Localization::registerReader(".mem", [](const std::string&, const std::string& language, std::string*) {
                return l10n::StringTable::fromEntries(language, {{"settings.title", "Settings"}, {"settings.resolution", "Resolution"},
                                                                 {"settings.volume", "Music Volume"}, {"settings.percent", "{n, number, percent}"},
                                                                 {"settings.subtitles", "Subtitles"}, {"settings.on", "On"},
                                                                 {"settings.apply", "Apply Changes"}});
            });
            LocalizationManifest manifest;
            manifest.defaultLanguage = "en";
            manifest.rememberLanguage = false;
            manifest.languages = {"en"};
            manifest.namespaces.push_back({"ui", "en", {{"en", "ui.en.mem"}}});
            Localization::setManifest(manifest);
        }

        void TearDown() override{
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
            FullPathGoldenTest::TearDown();
        }

        void renderPseudo(const char* code, const char* golden){
            ASSERT_TRUE(HtmlUI::loadFontFace("fonts/Roboto-Bold.ttf"));
            ASSERT_TRUE(Localization::setLanguage(code));
            {
                SceneFixture fixture;
                fixture.addDocument("golden/l10n_pseudo.rml");
                ASSERT_TRUE(fixture.waitUntilLoaded());
                Screen screen;
                ASSERT_TRUE(capture(screen, 2));
                // the title (blue) and the values (amber) drew
                EXPECT_GT(screen.countColor({0, 0, 640, 360}, 0x40, 0xc0, 0xff, 24), 100u);
                EXPECT_GT(screen.countColor({0, 0, 640, 360}, 0xff, 0xc0, 0x40, 24), 50u);
                EXPECT_SCREEN_GOLDEN(screen, golden);
            }
            EXPECT_FALSE(HtmlUI::isInitialized());
            FontRegistry::clear();
        }
    };

}

TEST_F(GoldenText, PseudoLocaleEnXa){
    renderPseudo("en-XA", "l10n/pseudo_en_xa");
}

TEST_F(GoldenText, PseudoLocaleArXb){
    renderPseudo("ar-XB", "l10n/pseudo_ar_xb");
}
