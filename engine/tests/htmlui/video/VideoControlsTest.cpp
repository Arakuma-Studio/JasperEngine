// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The default controls of <video controls> (docs/html-ui/phase-6-video.md §3.10): made and removed
// with the attribute, hidden from the DOM, laid out at the bottom of the content box; the pointer
// (play button, picture, timeline press and drag, mute), the keyboard on the focused video (Space,
// Enter, K, arrows, J/L, Home/End, M; up/down left to navigation; preventDefault), the gamepad
// (A, left/right), and the bar's visibility on the UI clock.

#include "VideoTestCommon.h"

#include "FixtureRunner.h"
#include "harness/GpuEnvironment.h"
#include "harness/TestSystem.h"

#include "HtmlUIClock.h"
#include "media/UIVideoControls.h"
#include "media/UIVideoElement.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class PreventKeys : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            event.PreventDefault();
        }
    };

    class VideoControlsTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            runner = std::make_unique<FixtureRunner>(640, 400, 1);
            if (!runner->valid())
                GTEST_SKIP() << "no HtmlUI or GPU";
            std::string error;
            ASSERT_TRUE(runner->load("fixtures/video/pages/controls.html", &error)) << error;
            runner->frameAt(0.0);
            runner->frameAt(0.0);
            v = rmlui_dynamic_cast<UIVideoElement*>(runner->document()->GetElementById("v"));
            ASSERT_NE(v, nullptr);
            ASSERT_NE(v->getControlsWidget(), nullptr);
        }

        void TearDown() override{
            runner.reset();
            HtmlUIClock::reset();
        }

        void click(float x, float y){
            Rml::Context* context = runner->context();
            context->ProcessMouseMove((int)std::lround(x), (int)std::lround(y), 0);
            context->ProcessMouseButtonDown(0, 0);
            context->ProcessMouseButtonUp(0, 0);
            runner->frameAt(runner->time());
        }

        Rml::Vector2f centre(const Rml::Rectanglef& r){
            return r.Center();
        }

        void key(Rml::Input::KeyIdentifier identifier){
            runner->context()->ProcessKeyDown(identifier, 0);
            runner->context()->ProcessKeyUp(identifier, 0);
            runner->frameAt(runner->time());
        }

        // a gamepad key as UIDocumentSystem dispatches it to the focused element
        bool gamepad(GamepadUIKey which, Rml::Input::KeyIdentifier identifier = Rml::Input::KI_UNKNOWN){
            Rml::Dictionary parameters;
            parameters["key_identifier"] = (int)identifier;
            parameters["gamepad"] = 0;
            parameters["gamepad_key"] = (int)which;
            parameters["repeat"] = 0;
            const bool propagated = runner->context()->GetFocusElement()->DispatchEvent(Rml::EventId::Keydown, parameters);
            runner->frameAt(runner->time());
            return propagated;
        }

        std::unique_ptr<FixtureRunner> runner;
        UIVideoElement* v = nullptr;
    };

}

TEST_F(VideoControlsTest, MadeHiddenFromTheDomAndLaidOut){
    UIVideoControls* controls = v->getControlsWidget();
    // not DOM children: the video's children are its sources (none here)
    EXPECT_EQ(v->GetNumChildren(), 0);
    EXPECT_EQ(runner->document()->QuerySelector("videocontrols"), nullptr);
    const UIVideoControls::Rects rects = controls->getRects();
    const Rml::Vector2f content = v->GetAbsoluteOffset(Rml::BoxArea::Content);
    // the bar spans the content box at its bottom, 40 px high
    EXPECT_FLOAT_EQ(rects.bar.Left(), content.x);
    EXPECT_FLOAT_EQ(rects.bar.Width(), 320.0f);
    EXPECT_FLOAT_EQ(rects.bar.Height(), 40.0f);
    EXPECT_FLOAT_EQ(rects.bar.Bottom(), content.y + 180.0f);
    EXPECT_FLOAT_EQ(rects.play.Width(), 32.0f);
    EXPECT_GT(rects.timeline.Width(), 100.0f);
    EXPECT_GT(rects.mute.Left(), rects.timeline.Right());
    EXPECT_EQ(controls->getTimeText(), "0:00 / 0:02");
    // paused: shown
    EXPECT_TRUE(controls->isShown());
    // the attribute goes: so do the controls
    v->setControls(false);
    runner->frameAt(0.0);
    EXPECT_EQ(v->getControlsWidget(), nullptr);
    EXPECT_EQ(v->GetNumChildren(true), 0);
    v->setControls(true);
    runner->frameAt(0.0);
    EXPECT_NE(v->getControlsWidget(), nullptr);
}

TEST_F(VideoControlsTest, PointerPlaysSeeksAndMutes){
    UIVideoControls::Rects rects = v->getControlsWidget()->getRects();
    click(centre(rects.play).x, centre(rects.play).y);
    EXPECT_FALSE(v->isPaused()) << "the play button plays";
    runner->frameAt(0.5);
    click(centre(rects.play).x, centre(rects.play).y);
    EXPECT_TRUE(v->isPaused()) << "and pauses";
    EXPECT_NEAR(v->getCurrentTime(), 0.5, 1e-9);
    // a press at 3/4 of the timeline seeks to 3/4 of the duration
    rects = v->getControlsWidget()->getRects();
    click(rects.timeline.Left() + rects.timeline.Width() * 0.75f, centre(rects.timeline).y);
    EXPECT_NEAR(v->getCurrentTime(), 0.75 * v->getDuration(), 0.02);
    EXPECT_EQ(v->getControlsWidget()->getTimeText(), "0:01 / 0:02");
    // the mute button
    click(centre(rects.mute).x, centre(rects.mute).y);
    EXPECT_TRUE(v->isMuted());
    click(centre(rects.mute).x, centre(rects.mute).y);
    EXPECT_FALSE(v->isMuted());
    // a click on the picture toggles playback
    click(centre(rects.bar).x, rects.bar.Top() - 40.0f);
    EXPECT_FALSE(v->isPaused());
    click(centre(rects.bar).x, rects.bar.Top() - 40.0f);
    EXPECT_TRUE(v->isPaused());
}

TEST_F(VideoControlsTest, DraggingTheTimelineScrubs){
    const UIVideoControls::Rects rects = v->getControlsWidget()->getRects();
    Rml::Context* context = runner->context();
    const int y = (int)std::lround(centre(rects.timeline).y);
    context->ProcessMouseMove((int)std::lround(rects.timeline.Left() + 4.0f), y, 0);
    context->ProcessMouseButtonDown(0, 0);
    runner->frameAt(0.0);
    const double start = v->getCurrentTime();
    for (int i = 1; i <= 10; i++){
        context->ProcessMouseMove((int)std::lround(rects.timeline.Left() + rects.timeline.Width() * 0.05f * (float)i), y, 0);
        runner->frameAt(0.0);
    }
    context->ProcessMouseButtonUp(0, 0);
    runner->frameAt(0.0);
    EXPECT_LT(start, 0.2);
    EXPECT_NEAR(v->getCurrentTime(), 0.5 * v->getDuration(), 0.1) << "the drag scrubbed to half";
    EXPECT_TRUE(v->isPaused());
}

TEST_F(VideoControlsTest, KeyboardOnTheFocusedVideo){
    // the video is in the Tab order (tabindex 0 from the UA sheet)
    Rml::Element* before = runner->document()->GetElementById("before");
    ASSERT_TRUE(before->Focus(true));
    runner->context()->ProcessKeyDown(Rml::Input::KI_TAB, 0);
    runner->context()->ProcessKeyUp(Rml::Input::KI_TAB, 0);
    runner->frameAt(0.0);
    ASSERT_EQ(runner->context()->GetFocusElement(), v);
    key(Rml::Input::KI_SPACE);
    EXPECT_FALSE(v->isPaused());
    key(Rml::Input::KI_K);
    EXPECT_TRUE(v->isPaused());
    key(Rml::Input::KI_RETURN);
    EXPECT_FALSE(v->isPaused());
    key(Rml::Input::KI_RETURN);
    key(Rml::Input::KI_RIGHT);
    EXPECT_NEAR(v->getCurrentTime(), v->getDuration(), 1e-9) << "+5 s, clamped to the end";
    key(Rml::Input::KI_HOME);
    EXPECT_EQ(v->getCurrentTime(), 0.0);
    key(Rml::Input::KI_L);
    EXPECT_NEAR(v->getCurrentTime(), v->getDuration(), 1e-9);
    key(Rml::Input::KI_J);
    EXPECT_EQ(v->getCurrentTime(), 0.0);
    key(Rml::Input::KI_END);
    EXPECT_NEAR(v->getCurrentTime(), v->getDuration(), 1e-9);
    key(Rml::Input::KI_LEFT);
    EXPECT_NEAR(v->getCurrentTime(), 0.0, 1e-9);
    key(Rml::Input::KI_M);
    EXPECT_TRUE(v->isMuted());
    // the focus stays on the video through all of them
    EXPECT_EQ(runner->context()->GetFocusElement(), v);
    // a listener's preventDefault disables the default actions
    PreventKeys prevent;
    v->AddEventListener(Rml::EventId::Keydown, &prevent);
    key(Rml::Input::KI_M);
    EXPECT_TRUE(v->isMuted());
    v->RemoveEventListener(Rml::EventId::Keydown, &prevent);
}

TEST_F(VideoControlsTest, GamepadOnTheFocusedVideo){
    ASSERT_TRUE(v->Focus(true));
    // A arrives as Enter (UIDocumentSystem's gamepadControlKey): it toggles
    gamepad(GamepadUIKey::A, Rml::Input::KI_RETURN);
    EXPECT_FALSE(v->isPaused());
    gamepad(GamepadUIKey::A, Rml::Input::KI_RETURN);
    EXPECT_TRUE(v->isPaused());
    // left/right seek and are consumed (no spatial navigation away)
    EXPECT_FALSE(gamepad(GamepadUIKey::DPAD_RIGHT)) << "consumed";
    EXPECT_NEAR(v->getCurrentTime(), v->getDuration(), 1e-9);
    EXPECT_FALSE(gamepad(GamepadUIKey::LEFT_STICK_LEFT));
    EXPECT_NEAR(v->getCurrentTime(), 0.0, 1e-9);
    // up/down are not: navigation may move the focus
    EXPECT_TRUE(gamepad(GamepadUIKey::DPAD_DOWN));
}

TEST_F(VideoControlsTest, TheBarHidesWhilePlaying){
    UIVideoControls* controls = v->getControlsWidget();
    // (looping: the end of the 2 s clip would pause it, and a paused video shows its bar)
    v->setLoop(true);
    v->play();
    runner->frameAt(0.1);
    // the pointer is not over the video and it is not focused: hidden right away while playing
    EXPECT_FALSE(controls->isShown());
    // a pointer move over it (at 0.1 s) shows it, and for 2.5 s of UI time after the pointer left
    const UIVideoControls::Rects rects = controls->getRects();
    runner->context()->ProcessMouseMove((int)centre(rects.bar).x, (int)rects.bar.Top() - 40, 0);
    runner->frameAt(0.2);
    EXPECT_TRUE(controls->isShown());
    runner->context()->ProcessMouseMove(600, 390, 0);
    runner->frameAt(2.5);
    EXPECT_TRUE(controls->isShown());
    runner->frameAt(2.7);
    EXPECT_FALSE(controls->isShown());
    v->pause();
    runner->frameAt(2.9);
    EXPECT_TRUE(controls->isShown()) << "paused: shown";
}

TEST(VideoControlsFormat, TimeText){
    EXPECT_EQ(UIVideoControls::formatTime(0.0, false), "0:00");
    EXPECT_EQ(UIVideoControls::formatTime(3.9, false), "0:03");
    EXPECT_EQ(UIVideoControls::formatTime(62.0, false), "1:02");
    EXPECT_EQ(UIVideoControls::formatTime(3723.0, true), "1:02:03");
    EXPECT_EQ(UIVideoControls::formatTime(std::nan(""), false), "0:00");
    EXPECT_EQ(UIVideoControls::formatTime(-4.0, false), "0:00");
}
