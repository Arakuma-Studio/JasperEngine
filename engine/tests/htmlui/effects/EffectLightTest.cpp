// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The light package's behaviours beyond the goldens (phase-5 spec 4.5, A.3): the pointer drives
// holographic's tilt (and not when tilt-follows-pointer is off) and cursor-ripples' ripples (a
// press inside distorts, no event leaves the content), noise-dissolve's burning edge is gone at
// both ends of the transition, every transition's invert reveals the content in the order the
// plain one removes it (their masks add up to the content), and sheen draws nothing between its
// sweeps. Rendered through the golden fixtures with the effect clock fixed.

#include "EffectTestCommon.h"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    struct EffectLightTest : ::testing::Test {
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

        const EffectManifest& effect(const std::string& name){
            const EffectManifest* m = EffectRegistry::get().find(name);
            EXPECT_NE(m, nullptr) << name;
            EXPECT_TRUE(m && warmUpEffect(*m)) << name;
            return *m;
        }

        // the fixture with a declaration at t; the pointer at (x, y) shares of #fx when given,
        // pressed `sincePress` seconds before (-1: no press)
        TestImage render(const std::string& fixture, const std::string& declaration, double t, std::optional<Rml::Vector2f> pointer = std::nullopt,
                         float sincePress = -1.f){
            TestImage image;
            std::string error;
            EXPECT_TRUE(scene->load(fixture, declaration, &error)) << error;
            EffectClock::setFixedTime(t);
            EffectRuntime::get().clearTestInput();
            if (pointer){
                const Rml::Rectanglef fx = scene->box("fx");
                const Rml::Vector2f at = fx.Position() + Rml::Vector2f(fx.Width() * pointer->x, fx.Height() * pointer->y);
                EffectRuntime::get().setTestPointer(scene->context(), at, false, sincePress);
            }
            EXPECT_TRUE(scene->capture(image));
            return image;
        }
    };

}

TEST_F(EffectLightTest, HolographicTiltsTowardThePointer){
    effect("holographic");
    const TestImage away = render("paint", "background-image: effect(holographic)", 1.5);
    const TestImage right = render("paint", "background-image: effect(holographic)", 1.5, Rml::Vector2f(0.9f, 0.5f));
    const TestImage left = render("paint", "background-image: effect(holographic)", 1.5, Rml::Vector2f(0.1f, 0.5f));
    EXPECT_GT(maxDifference(away, right), 16) << "the pointer over the element tilts the foil";
    EXPECT_GT(maxDifference(right, left), 16) << "the tilt follows where the pointer is";
    const std::string still = "background-image: effect(holographic, tilt-follows-pointer off)";
    EXPECT_EQ(maxDifference(render("paint", still, 1.5), render("paint", still, 1.5, Rml::Vector2f(0.9f, 0.5f))), 0)
        << "tilt-follows-pointer off ignores the pointer";
    EXPECT_EQ(maxDifference(away, render("paint", still, 1.5)), 0) << "without the pointer both are the same foil";
}

TEST_F(EffectLightTest, CursorRipplesFollowPresses){
    effect("cursor-ripples");
    const TestImage content = render("filter", "filter: none", 1.5);
    // no ripple event: the content as it is
    EXPECT_LE(maxDifference(render("filter", "filter: effect(cursor-ripples, trigger press)", 1.5), content), 1);
    EXPECT_LE(maxDifference(render("filter", "filter: effect(cursor-ripples, trigger press)", 1.5, Rml::Vector2f(0.5f, 0.5f)), content), 1)
        << "a pointer that has not pressed makes no ripple";
    // a press 0.3 s before: a ring 90 px out (wave-speed 300) distorts the content
    const TestImage pressed = render("filter", "filter: effect(cursor-ripples, trigger press)", 1.5, Rml::Vector2f(0.5f, 0.5f), 0.3f);
    EXPECT_GT(maxDifference(pressed, content), 32) << "a press inside makes a ripple";
    // intensity 0: the ripple displaces nothing
    EXPECT_LE(maxDifference(render("filter", "filter: effect(cursor-ripples, intensity 0)", 1.5, Rml::Vector2f(0.5f, 0.5f), 0.3f), content), 1);
    // an old ripple has decayed: with decay 20 /s, 0.3 s leaves exp(-6) of it
    const TestImage faded = render("filter", "filter: effect(cursor-ripples, decay 20)", 1.5, Rml::Vector2f(0.5f, 0.5f), 0.3f);
    EXPECT_LT(maxDifference(faded, content), maxDifference(pressed, content)) << "decay fades the ripple";
}

TEST_F(EffectLightTest, NoiseDissolveBurnIsGoneAtBothEnds){
    effect("noise-dissolve");
    const TestImage content = render("filter", "filter: none", 1.5);
    const TestImage hidden = render("filter", "opacity: 0", 1.5);
    const std::string burn = ", edge-color #ff8a00, edge-width 0.3";
    EXPECT_LE(maxDifference(render("filter", "filter: effect(noise-dissolve, progress 0%" + burn + ")", 1.5), content), 1);
    EXPECT_LE(maxDifference(render("filter", "filter: effect(noise-dissolve, progress 100%" + burn + ")", 1.5), hidden), 1);
    EXPECT_LE(maxDifference(render("filter", "filter: effect(noise-dissolve, progress 0%, invert on" + burn + ")", 1.5), hidden), 1);
    EXPECT_LE(maxDifference(render("filter", "filter: effect(noise-dissolve, progress 100%, invert on" + burn + ")", 1.5), content), 1);
    // half-way the edge burns: the burning render differs from the plain dissolve
    EXPECT_GT(maxDifference(render("filter", "filter: effect(noise-dissolve, progress 50%" + burn + ")", 1.5),
                            render("filter", "filter: effect(noise-dissolve, progress 50%)", 1.5)), 32);
}

TEST_F(EffectLightTest, TransitionInvertIsTheComplement){
    // over a transparent page, the alpha of a transition and of its invert at the same progress
    // add up to the content's alpha (the plain one removes what the inverted one shows)
    for (const char* name : {"noise-dissolve", "block-dissolve", "linear-wipe", "radial-wipe", "iris-wipe", "diamond-wipe", "venetian-blinds", "ripple-wipe"}){
        SCOPED_TRACE(name);
        effect(name);
        const std::string html = "<!DOCTYPE html><html><head><style>html, body { margin: 0; width: 360px; height: 240px; background: transparent; }\n"
                                 "#fx { position: absolute; left: 20px; top: 20px; width: 320px; height: 200px; background: #ffffff; __DECL__; }"
                                 "</style></head><body><div id=\"fx\"></div></body></html>";
        auto alphaOf = [&](const std::string& arguments){
            std::string page = html;
            page.replace(page.find("__DECL__"), 8, "filter: effect(" + std::string(name) + ", " + arguments + ")");
            std::string error;
            EXPECT_TRUE(scene->loadHtml(page, &error)) << error;
            EffectClock::setFixedTime(1.5);
            TestImage image;
            EXPECT_TRUE(scene->capture(image));
            return image;
        };
        const TestImage plain = alphaOf("progress 40%");
        const TestImage inverted = alphaOf("progress 40%, invert on");
        int worst = 0;
        for (size_t i = 3; i < plain.rgba.size(); i += 4){
            const int x = (int)((i / 4) % plain.width), y = (int)((i / 4) / plain.width);
            const bool inside = x >= 20 && x < 340 && y >= 20 && y < 220;
            const int sum = (int)plain.rgba[i] + (int)inverted.rgba[i];
            worst = std::max(worst, std::abs(sum - (inside ? 255 : 0)));
        }
        EXPECT_LE(worst, 2) << "plain + inverted alpha is the content's";
        EXPECT_GT(maxDifference(plain, inverted), 128) << "40% is part-way: the two differ";
    }
}

TEST_F(EffectLightTest, SheenRestsBetweenSweeps){
    effect("sheen");
    const TestImage none = render("paint", "background-image: none", 0.0);
    // period 3 s, duration 0.8 s: the sweep runs from 1.1 s to 1.9 s of each period
    EXPECT_LE(maxDifference(render("paint", "background-image: effect(sheen)", 0.5), none), 1);
    EXPECT_LE(maxDifference(render("paint", "background-image: effect(sheen)", 2.5), none), 1);
    EXPECT_GT(maxDifference(render("paint", "background-image: effect(sheen)", 1.5), none), 32) << "half-way through the period the band crosses";
    EXPECT_GT(maxDifference(render("paint", "background-image: effect(sheen)", 4.5), none), 32) << "and again in the next period";
    // the delay holds the first period back
    EXPECT_LE(maxDifference(render("paint", "background-image: effect(sheen, delay 2s)", 1.5), none), 1);
    EXPECT_GT(maxDifference(render("paint", "background-image: effect(sheen, delay 2s)", 3.5), none), 32);
}
