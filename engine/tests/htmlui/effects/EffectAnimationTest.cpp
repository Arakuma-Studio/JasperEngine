// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Effects animated by CSS and anime (phase-5 spec 3.4, 5.1): a transition of background-image
// interpolates the parameters (getEffects at the midpoint), @keyframes animate filter effects, a
// var() change re-instances once without compiling, anime animating a custom property drives a
// parameter, and a different effect switches discretely. The UI clock is the fixture runner's
// fixed clock, moved by `advance` steps.

#include "EffectTestCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/anim/AnimEngine.h"
#include "htmlui/anim/AnimTargets.h"
#include "htmlui/anim/Anime.h"
#include "htmlui/effects/EffectInstancers.h"
#include "object/ui/UIElementInternal.h"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    std::string page(const std::string& css){
        return "<!DOCTYPE html><html><head><style>html, body { margin: 0; width: 200px; height: 120px; }\n"
               "#fx { position: absolute; left: 10px; top: 10px; width: 160px; height: 80px; }\n" + css +
               "</style></head><body><div id=\"fx\"></div></body></html>";
    }

    // the effects of an element as getEffects() reports them: "name" and every param's text
    struct Uses {
        std::vector<EffectUse> list;
        std::string param(size_t index, const std::string& name) const{
            if (index >= list.size())
                return "<no effect " + std::to_string(index) + ">";
            for (const auto& p : list[index].params){
                if (p.first == name)
                    return p.second;
            }
            return "<no param " + name + ">";
        }
    };

    struct EffectAnimationTest : ::testing::Test {
        std::unique_ptr<EffectScene> scene;

        void SetUp() override{
            TestSystem::get().clearLogs();
            scene = std::make_unique<EffectScene>(200, 120, 1);
            if (!scene->valid())
                GTEST_SKIP() << "no GPU";
        }

        void TearDown() override{
            AnimEngine::instance().reset();
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u) << logText(D_LOG_WARN);
        }

        bool load(const std::string& css){
            std::string error;
            const bool ok = scene->loadHtml(page(css), &error);
            EXPECT_TRUE(ok) << error;
            return ok;
        }

        bool step(FixtureStep::Kind kind, const std::string& name = std::string(), const std::string& value = std::string(), double seconds = 0.0){
            FixtureStep s;
            s.kind = kind;
            s.id = "fx";
            s.name = name;
            s.value = value;
            s.seconds = seconds;
            std::string error;
            const bool ok = scene->fixture().step(s, &error);
            EXPECT_TRUE(ok) << error;
            return ok;
        }

        void advance(double seconds){ step(FixtureStep::Kind::Advance, "", "", seconds); }

        Uses uses(){
            Uses u;
            u.list = collectEffectUses(scene->element("fx"));
            return u;
        }

        void frame(){
            TestImage image;
            EXPECT_TRUE(scene->capture(image));
        }
    };

}

TEST_F(EffectAnimationTest, TransitionMidpointInterpolatesParams){
    ASSERT_TRUE(load("#fx { background-image: effect(stripes, density 4, angle 0deg, color-a #000000); transition: background-image 1s linear; }\n"
                     "#fx.on { background-image: effect(stripes, density 8, angle 90deg, color-a #ffffff); }\n"));
    Uses before = uses();
    ASSERT_EQ(before.list.size(), 1u);
    EXPECT_EQ(before.list[0].property, "background-image");
    EXPECT_EQ(before.param(0, "density"), "4");
    step(FixtureStep::Kind::AddClass, "on");
    advance(0.5);
    Uses mid = uses();
    ASSERT_EQ(mid.list.size(), 1u);
    EXPECT_EQ(mid.list[0].name, "stripes");
    EXPECT_EQ(mid.param(0, "density"), "6");
    EXPECT_EQ(mid.param(0, "angle"), "45deg");
    advance(0.6);
    Uses end = uses();
    EXPECT_EQ(end.param(0, "density"), "8");
    EXPECT_EQ(end.param(0, "angle"), "90deg");
    EXPECT_EQ(end.param(0, "color-a"), "#ffffffff");
}

TEST_F(EffectAnimationTest, KeyframesAnimateFilterEffects){
    ASSERT_TRUE(load("@keyframes px { from { filter: effect(pixelate, size 4px); } to { filter: effect(pixelate, size 20px); } }\n"
                     "#fx { animation: px 1s linear infinite; }\n"));
    advance(0.25);
    Uses quarter = uses();
    ASSERT_EQ(quarter.list.size(), 1u);
    EXPECT_EQ(quarter.list[0].property, "filter");
    EXPECT_EQ(quarter.list[0].name, "pixelate");
    EXPECT_EQ(quarter.param(0, "size"), "8px");
    advance(0.5);
    EXPECT_EQ(uses().param(0, "size"), "16px");
}

TEST_F(EffectAnimationTest, VarChangeReinstancesOnceWithoutCompiling){
    const EffectManifest* stripes = EffectRegistry::get().find("stripes");
    ASSERT_NE(stripes, nullptr);
    ASSERT_TRUE(warmUpEffect(*stripes));
    ASSERT_TRUE(load("#fx { --d: 4; background-image: effect(stripes, density var(--d)); }\n"));
    frame();
    frame();
    EXPECT_EQ(uses().param(0, "density"), "4");
    const EffectStats before = EffectRuntime::get().stats();
    step(FixtureStep::Kind::SetStyle, "--d", "7");
    frame();
    const EffectStats after = EffectRuntime::get().stats();
    EXPECT_EQ(after.reinstances - before.reinstances, 1u) << "a var() change re-instances the decorator once";
    EXPECT_EQ(after.compileCalls - before.compileCalls, 0u) << "a var() change compiles nothing";
    EXPECT_EQ(uses().param(0, "density"), "7");
}

TEST_F(EffectAnimationTest, AnimeDrivesParamsThroughCustomProperties){
    ASSERT_TRUE(load("#fx { --x: 0deg; background-image: effect(stripes, angle var(--x)); }\n"));
    AnimEngine::instance().reset();
    AnimEngine::instance().tick(HtmlUIClock::now());
    UIElement target = UIElementAccess::wrap(scene->element("fx"));
    auto animation = Anime::animate({AnimTargets::fromElement(target)}, AnimParam::object({{"--x", "90deg"}, {"duration", 1000}, {"ease", "linear"}}));
    const double start = HtmlUIClock::now();
    HtmlUIClock::setFixedTime(start + 0.5);
    AnimEngine::instance().tick(HtmlUIClock::now());
    frame();
    EXPECT_EQ(uses().param(0, "angle"), "45deg");
    HtmlUIClock::setFixedTime(start + 1.5);
    AnimEngine::instance().tick(HtmlUIClock::now());
    frame();
    EXPECT_EQ(uses().param(0, "angle"), "90deg");
}

TEST_F(EffectAnimationTest, DifferentEffectSwitchesDiscretely){
    ASSERT_TRUE(load("#fx { background-image: effect(stripes, density 4); transition: background-image 1s linear; }\n"
                     "#fx.on { background-image: effect(spotlight, intensity 1); }\n"));
    step(FixtureStep::Kind::AddClass, "on");
    std::set<std::string> seen;
    for (int i = 0; i < 12; i++){
        advance(0.1);
        Uses now = uses();
        ASSERT_EQ(now.list.size(), 1u) << "at " << (i + 1) * 0.1 << " s: never two effects at once";
        seen.insert(now.list[0].name);
        // the effect shown has its own parameters, never a blend with the other's
        if (now.list[0].name == "stripes"){
            EXPECT_EQ(now.param(0, "density"), "4");
        }
        else
            EXPECT_EQ(now.param(0, "intensity"), "1");
    }
    EXPECT_EQ(uses().list[0].name, "spotlight");
}
