// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The filter package's behaviours beyond the goldens (phase-5 spec 4.6, A.4): the edge modes on
// wave-distortion (a golden each: stretch, transparent, mirror, wrap), a glitch burst (the
// defaults are calm at both golden times), bulge following the pointer (and not with
// follow-pointer off), the sharp band of tilt-shift and the clear middle of liquid-glass and
// progressive-blur's start (the backdrop exactly), the seed of flow-field, water-ripple's
// chromatic split and dither's source colours. Rendered through the golden fixtures with the
// effect clock fixed.

#include "EffectTestCommon.h"

#include "harness/ImageCompare.h"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    // an opaque background under the test card, so the area's edges have colour
    constexpr const char* kOpaque = "background: linear-gradient(90deg, #2c3e66, #b0583a); ";

    struct EffectFilterTest : ::testing::Test {
        std::unique_ptr<EffectScene> scene;

        void SetUp() override{
            TestSystem::get().clearLogs();
            scene = std::make_unique<EffectScene>();
            if (!scene->valid())
                GTEST_SKIP() << "no GPU";
        }

        void TearDown() override{
            EffectClock::setFixedTime(std::nullopt);
            EffectRuntime::get().clearTestInput();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u) << logText(D_LOG_WARN);
        }

        void effect(const std::string& name){
            const EffectManifest* m = EffectRegistry::get().find(name);
            ASSERT_NE(m, nullptr) << name;
            ASSERT_TRUE(warmUpEffect(*m)) << name;
        }

        // the fixture with a declaration at t; the pointer at (x, y) shares of #fx when given
        TestImage render(const std::string& fixture, const std::string& declaration, double t, std::optional<Rml::Vector2f> pointer = std::nullopt){
            TestImage image;
            std::string error;
            EXPECT_TRUE(scene->load(fixture, declaration, &error)) << error;
            EffectClock::setFixedTime(t);
            EffectRuntime::get().clearTestInput();
            if (pointer){
                const Rml::Rectanglef fx = scene->box("fx");
                const Rml::Vector2f at = fx.Position() + Rml::Vector2f(fx.Width() * pointer->x, fx.Height() * pointer->y);
                EffectRuntime::get().setTestPointer(scene->context(), at, false, -1.f);
            }
            EXPECT_TRUE(scene->capture(image));
            return image;
        }

        // the largest channel difference inside a rectangle of the canvas
        static int maxDifferenceIn(const TestImage& a, const TestImage& b, int x0, int y0, int x1, int y1){
            int worst = 0;
            for (int y = y0; y < y1; y++){
                for (int x = x0; x < x1; x++){
                    const size_t i = ((size_t)y * a.width + x) * 4;
                    for (int k = 0; k < 4; k++)
                        worst = std::max(worst, std::abs((int)a.rgba[i + k] - (int)b.rgba[i + k]));
                }
            }
            return worst;
        }

        static int minAlphaIn(const TestImage& a, int x0, int y0, int x1, int y1){
            int least = 255;
            for (int y = y0; y < y1; y++){
                for (int x = x0; x < x1; x++)
                    least = std::min(least, (int)a.rgba[((size_t)y * a.width + x) * 4 + 3]);
            }
            return least;
        }
    };

}

TEST_F(EffectFilterTest, WaveDistortionEdgeModes){
    effect("wave-distortion");
    const char* modes[] = {"stretch", "transparent", "mirror", "wrap"};
    std::vector<TestImage> images;
    for (const char* mode : modes){
        // a strong, slanted wave, so every mode reads well beyond the area's edges, over an opaque
        // background (the test card's edges are transparent: stretch would read transparent there)
        images.push_back(render("filter", std::string(kOpaque) + "filter: effect(wave-distortion, strength 0.6, frequency 1.5, angle 30deg, edges " + mode + ")", 1.5));
        const CompareResult golden = compareWithGolden(std::string("effects/wave-distortion.") + mode, images.back().rgba, images.back().width, images.back().height);
        EXPECT_TRUE(golden.passed) << mode << ": " << golden.message;
    }
    for (size_t i = 0; i < images.size(); i++){
        for (size_t j = i + 1; j < images.size(); j++)
            EXPECT_GT(maxDifference(images[i], images[j]), 32) << modes[i] << " and " << modes[j] << " render the same";
    }
    // the default is stretch
    EXPECT_EQ(maxDifference(images[0], render("filter", std::string(kOpaque) + "filter: effect(wave-distortion, strength 0.6, frequency 1.5, angle 30deg)", 1.5)), 0);
}

TEST_F(EffectFilterTest, TransparentEdgesClearWhatLeavesTheArea){
    effect("twirl");
    // a strong twirl reads the area's corners from beyond the area: the page shows there with
    // transparent edges, the area's border colours with stretch
    const TestImage stretch = render("filter", std::string(kOpaque) + "filter: effect(twirl, intensity 5, edges stretch)", 1.5);
    const TestImage transparent = render("filter", std::string(kOpaque) + "filter: effect(twirl, intensity 5, edges transparent)", 1.5);
    EXPECT_GT(maxDifference(stretch, transparent), 32);
    // the page beyond the area is untouched by either
    EXPECT_EQ(maxDifferenceIn(stretch, transparent, 0, 0, 360, 18), 0);
}

TEST_F(EffectFilterTest, GlitchBursts){
    effect("glitch");
    const TestImage content = render("filter", "filter: none", 1.5);
    // the default schedule is calm at both golden times (1.5 s and 4 s: no burst frame)...
    EXPECT_LE(maxDifference(content, render("filter", "filter: effect(glitch)", 1.5)), 1);
    // ...and bursts at 2.3 s (slow frame 5 and fast frame 23 both fire)
    const TestImage burst = render("filter", "filter: effect(glitch)", 2.3);
    EXPECT_GT(maxDifference(content, burst), 64) << "a glitch burst displaces the content";
    const CompareResult golden = compareWithGolden("effects/glitch.burst", burst.rgba, burst.width, burst.height);
    EXPECT_TRUE(golden.passed) << golden.message;
    // intensity 0 never glitches
    EXPECT_LE(maxDifference(content, render("filter", "filter: effect(glitch, intensity 0)", 2.3)), 1);
}

TEST_F(EffectFilterTest, BulgeFollowsThePointer){
    effect("bulge");
    const std::string follow = "filter: effect(bulge, follow-pointer on)";
    const TestImage away = render("filter", follow, 1.5);
    const TestImage left = render("filter", follow, 1.5, Rml::Vector2f(0.2f, 0.5f));
    const TestImage right = render("filter", follow, 1.5, Rml::Vector2f(0.8f, 0.5f));
    EXPECT_GT(maxDifference(away, left), 16) << "the bulge moves to the pointer";
    EXPECT_GT(maxDifference(left, right), 16) << "the bulge follows where the pointer is";
    const std::string still = "filter: effect(bulge)";
    EXPECT_EQ(maxDifference(render("filter", still, 1.5), render("filter", still, 1.5, Rml::Vector2f(0.8f, 0.5f))), 0)
        << "follow-pointer off ignores the pointer";
    EXPECT_EQ(maxDifference(away, render("filter", still, 1.5)), 0) << "without the pointer both bulge at the centre";
}

TEST_F(EffectFilterTest, BackdropOpticsKeepTheirClearParts){
    effect("tilt-shift");
    effect("progressive-blur");
    effect("liquid-glass");
    // the backdrop fixture's panel: 220 x 140 at (70, 50)
    const TestImage behind = render("backdrop", "backdrop-filter: none", 1.5);
    // tilt-shift: the sharp band (+-21 px about the panel's middle row) is the backdrop exactly,
    // the panel's top and bottom are blurred
    const TestImage tilt = render("backdrop", "backdrop-filter: effect(tilt-shift)", 1.5);
    EXPECT_LE(maxDifferenceIn(behind, tilt, 80, 110, 280, 130), 1);
    EXPECT_GT(maxDifferenceIn(behind, tilt, 80, 160, 280, 188), 16);
    // progressive-blur: sharp at its start (the left edge), blurred toward the right
    const TestImage progressive = render("backdrop", "backdrop-filter: effect(progressive-blur)", 1.5);
    EXPECT_LE(maxDifferenceIn(behind, progressive, 72, 60, 76, 180), 2);
    EXPECT_GT(maxDifferenceIn(behind, progressive, 230, 60, 288, 188), 16);
    // liquid-glass: the middle (beyond the 24 px rim) shows the backdrop; the rim bends it
    const TestImage glass = render("backdrop", "backdrop-filter: effect(liquid-glass)", 1.5);
    EXPECT_LE(maxDifferenceIn(behind, glass, 100, 80, 260, 160), 1);
    EXPECT_GT(maxDifferenceIn(behind, glass, 70, 50, 290, 74), 16);
    // and the frost blurs the middle too
    const TestImage frosted = render("backdrop", "backdrop-filter: effect(liquid-glass, blur 8px)", 1.5);
    EXPECT_GT(maxDifferenceIn(behind, frosted, 100, 80, 260, 160), 16);
}

TEST_F(EffectFilterTest, ParametersChangeTheLook){
    effect("flow-field");
    effect("water-ripple");
    effect("dither");
    // flow-field's seed is another pattern at speed 0
    EXPECT_GT(maxDifference(render("filter", "filter: effect(flow-field)", 1.5), render("filter", "filter: effect(flow-field, seed 37)", 1.5)), 16);
    // water-ripple's chromatic split separates the colours along the ripples
    EXPECT_GT(maxDifference(render("filter", "filter: effect(water-ripple, amplitude 12px)", 1.5),
                            render("filter", "filter: effect(water-ripple, amplitude 12px, chromatic-split 3)", 1.5)), 16);
    // dither's source mode keeps the content's colours, darkened and brightened
    const TestImage source = render("filter", "filter: effect(dither, color-mode source)", 1.5);
    const TestImage custom = render("filter", "filter: effect(dither)", 1.5);
    EXPECT_GT(maxDifference(source, custom), 64);
    // the red square (32..72 x 72..112): the source mode's inks are reds
    const size_t i = ((size_t)92 * source.width + 52) * 4;
    EXPECT_GT((int)source.rgba[i], (int)source.rgba[i + 1]);
    EXPECT_EQ(minAlphaIn(source, 40, 80, 64, 104), 255);
}

TEST_F(EffectFilterTest, TheFilterRegionIsTheElements){
    effect("crt");
    effect("wave-distortion");
    // the filter fixture has whitespace between #fx's children: the filtered region is still
    // #fx's box (20..340 x 20..220), so an opaque screen or a stretched edge never paints the page
    const TestImage page = render("filter", "filter: none", 1.5);
    for (const char* declaration : {"filter: effect(crt)", "filter: effect(wave-distortion, strength 1)"}){
        const TestImage image = render("filter", declaration, 1.5);
        EXPECT_EQ(maxDifferenceIn(page, image, 0, 0, 360, 19), 0) << declaration;
        EXPECT_EQ(maxDifferenceIn(page, image, 0, 0, 19, 240), 0) << declaration;
        EXPECT_EQ(maxDifferenceIn(page, image, 0, 221, 360, 240), 0) << declaration;
        EXPECT_EQ(maxDifferenceIn(page, image, 341, 0, 360, 240), 0) << declaration;
        EXPECT_GT(maxDifferenceIn(page, image, 20, 20, 340, 220), 32) << declaration;
    }
}

// A running clock redraws an animated filter every frame, as it does a paint effect: a view that
// redraws only when its context asks (the editor's edit view, GetNextUpdateDelay) kept the first
// frame of a document whose effects were all filters (a filter is compiled once per reload, the
// only time it asked)
TEST(EffectRedrawTest, AnAnimatedFilterAsksForTheNextUpdate){
    EffectScene scene(320, 200, 1);
    if (!scene.valid())
        GTEST_SKIP() << "no GPU";
    const EffectManifest* m = EffectRegistry::get().find("wave-distortion");
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(warmUpEffect(*m));
    std::string error;
    ASSERT_TRUE(scene.loadHtml("<!DOCTYPE html><html><head><style>\n"
                               "html, body { margin: 0; width: 320px; height: 200px; background: #101218; }\n"
                               "#fx { position: absolute; left: 20px; top: 20px; width: 200px; height: 120px; background: #e74c3c; "
                               "filter: effect(wave-distortion); }\n"
                               "</style></head><body><div id=\"fx\">FX</div></body></html>",
                               &error)) << error;
    TestImage image;
    for (int i = 0; i < 3; i++){
        ASSERT_TRUE(scene.capture(image));
        EXPECT_EQ(scene.context()->GetNextUpdateDelay(), 0.0) << "capture " << i << ": the live clock animates the filter";
    }
    FixedEffectClock clock(1.5);
    ASSERT_TRUE(scene.capture(image));
    EXPECT_GT(scene.context()->GetNextUpdateDelay(), 0.0) << "a fixed effect clock animates nothing";
}
