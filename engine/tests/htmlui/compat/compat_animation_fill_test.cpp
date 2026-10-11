// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AnimationFill (phase-2 §3.11.5): -html-animation-fill: forwards/both keeps the end values
// after animationend (every animated property, the last keyframe; the first for an
// alternate animation with an even count), clears them when the animation changes or the
// element leaves its document, and leaves values a script set meanwhile alone.

#include "RuntimeTestCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/compat/runtime/AnimationFill.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    const char* kKeyframes =
        "@keyframes fadein { from { opacity: 0; width: 10px; } to { opacity: 0.5; width: 50px; } } "
        "@keyframes grow { from { height: 10px; } to { height: 40px; } } "
        "div { height: 20px; } ";

    class CompatAnimationFillTest : public RuntimeContextTest {
    protected:
        void SetUp() override{
            HtmlUIClock::reset();
            HtmlUIClock::setFixedTime(0.0);
            RuntimeContextTest::SetUp();
        }

        void TearDown() override{
            RuntimeContextTest::TearDown();
            HtmlUIClock::reset();
        }

        // frames up to `time`, 50 ms apart; the emulation runs in each (updateRuntime skips it
        // when the fork has AnimationLonghands: these tests keep it working for a fork without)
        void at(double time){
            double now = HtmlUIClock::now();
            while (now + 0.05 < time - 1e-9){
                now += 0.05;
                HtmlUIClock::setFixedTime(now);
                emulatedFrame();
            }
            HtmlUIClock::setFixedTime(time);
            emulatedFrame();
        }

        void emulatedFrame(){
            AnimationFill::update(context);
            frame();
        }

        float opacity(const char* id){
            return byId(id)->GetComputedValues().opacity();
        }

        float width(const char* id){
            return byId(id)->GetBox().GetSize().x;
        }
    };

}

TEST_F(CompatAnimationFillTest, ForwardsKeepsTheEndValues){
    ASSERT_NE(show("<div id='fill'/><div id='plain'/><div id='both'/>",
                   std::string(kKeyframes) +
                   "#fill { animation: 0.5s linear-in-out fadein; -html-animation-fill: forwards; } "
                   "#both { animation: 0.5s linear-in-out fadein; -html-animation-fill: both; } "
                   "#plain { animation: 0.5s linear-in-out fadein; }"), nullptr);
    at(0.25);
    EXPECT_NEAR(opacity("fill"), 0.25f, 0.02f);
    EXPECT_EQ(AnimationFill::filledCount(), 0u);

    at(0.6);
    at(0.7);
    // forwards/both: the 100% values stay (opacity and width); without: RmlUi's defaults
    EXPECT_FLOAT_EQ(opacity("fill"), 0.5f);
    EXPECT_FLOAT_EQ(width("fill"), 50.0f);
    EXPECT_FLOAT_EQ(opacity("both"), 0.5f);
    EXPECT_FLOAT_EQ(opacity("plain"), 1.0f);
    EXPECT_TRUE(AnimationFill::isFilled(byId("fill")));
    EXPECT_FALSE(AnimationFill::isFilled(byId("plain")));
    EXPECT_EQ(AnimationFill::filledCount(), 4u);   // two properties on two elements

    // frames later the values hold
    at(2.0);
    EXPECT_FLOAT_EQ(opacity("fill"), 0.5f);
}

TEST_F(CompatAnimationFillTest, AnimationChangeClearsTheValues){
    ASSERT_NE(show("<div id='box'/>", std::string(kKeyframes) +
                   "#box { animation: 0.5s linear-in-out fadein; -html-animation-fill: forwards; }"), nullptr);
    at(0.6);
    at(0.7);
    ASSERT_FLOAT_EQ(opacity("box"), 0.5f);

    // another animation (not touching opacity): the fill goes, the style's values return
    byId("box")->SetProperty("animation", "1s linear-in-out grow");
    at(0.8);
    at(0.9);
    EXPECT_FLOAT_EQ(opacity("box"), 1.0f);
    EXPECT_FALSE(AnimationFill::isFilled(byId("box")));
}

TEST_F(CompatAnimationFillTest, ScriptValuesAreKept){
    ASSERT_NE(show("<div id='box'/>", std::string(kKeyframes) +
                   "#box { animation: 0.5s linear-in-out fadein; -html-animation-fill: forwards; }"), nullptr);
    at(0.6);
    at(0.7);
    byId("box")->SetProperty("opacity", "0.8");
    byId("box")->SetProperty("animation", "1s linear-in-out grow");
    at(0.8);
    EXPECT_FLOAT_EQ(opacity("box"), 0.8f);
    // width was still compat's: removed
    EXPECT_NE(width("box"), 50.0f);
}

TEST_F(CompatAnimationFillTest, AlternateEvenCountEndsAtTheStart){
    ASSERT_NE(show("<div id='box'/>", std::string(kKeyframes) +
                   "#box { animation: 0.25s 2 alternate linear-in-out fadein; -html-animation-fill: forwards; }"), nullptr);
    at(0.6);
    at(0.7);
    EXPECT_FLOAT_EQ(opacity("box"), 0.0f);
    EXPECT_FLOAT_EQ(width("box"), 10.0f);
}

TEST_F(CompatAnimationFillTest, ElementRemovalDropsTheEntry){
    ASSERT_NE(show("<div id='a'/><div id='b'/>", std::string(kKeyframes) +
                   "div { animation: 0.5s linear-in-out fadein; -html-animation-fill: forwards; }"), nullptr);
    at(0.6);
    at(0.7);
    ASSERT_EQ(AnimationFill::filledCount(), 4u);

    // destroyed
    Rml::Element* a = byId("a");
    a->GetParentNode()->RemoveChild(a);
    at(0.8);
    EXPECT_EQ(AnimationFill::filledCount(), 2u);

    // detached but kept alive (a script holding it): the values go, the entry too
    Rml::Element* b = byId("b");
    Rml::ElementPtr kept = b->GetParentNode()->RemoveChild(b);
    at(0.9);
    EXPECT_EQ(AnimationFill::filledCount(), 0u);
    EXPECT_EQ(kept->GetLocalProperty(Rml::PropertyId::Opacity), nullptr);
}

