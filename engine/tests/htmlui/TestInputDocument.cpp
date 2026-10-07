// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pointer input from the engine's system* entry points to documents: :hover/:active,
// events consumed over document elements (not over the transparent body, not through
// pointer-events: none), touch pointer 0, scenes with UI events off, reference sizes.

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "Input.h"

#include <gtest/gtest.h>

#include <functional>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "htmlui-tests-input";

    // input.html: #button at (100,100) 160x48, #overlay (300,100) 200x150 with
    // pointer-events: none and its #inner (320,120) 50x50; screen = canvas = 640x360
    const float buttonX = 180, buttonY = 124;
    const float bodyX = 600, bodyY = 330;
    const float overlayX = 450, overlayY = 220;
    const float innerX = 345, innerY = 145;

    class DocumentInputTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            fixture.reset(new SceneFixture());
            document = &fixture->add<UIDocument>();
            ASSERT_TRUE(document->load("ecs/input.html"));
            button = document->getElementById("button");
            ASSERT_TRUE(button.isValid());

            Engine::onMouseDown.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ mouseDowns++; }));
            Engine::onMouseUp.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ mouseUps++; }));
            Engine::onMouseMove.add(callbackTag, std::function<void(float, float, int)>([this](float, float, int){ mouseMoves++; }));
            Engine::onTouchStart.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ touchStarts++; }));
            Engine::onTouchEnd.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ touchEnds++; }));
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            Engine::onMouseDown.remove(callbackTag);
            Engine::onMouseUp.remove(callbackTag);
            Engine::onMouseMove.remove(callbackTag);
            Engine::onTouchStart.remove(callbackTag);
            Engine::onTouchEnd.remove(callbackTag);
            // no touch stays down for the next test
            Engine::systemTouchCancel();
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
        }

        void click(float x, float y){
            Engine::systemMouseMove(x, y, 0);
            Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, x, y, 0);
            Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, x, y, 0);
        }

        Scene& scene(){
            return fixture->getScene();
        }

        std::unique_ptr<SceneFixture> fixture;
        UIDocument* document = nullptr;
        UIElement button;
        int mouseDowns = 0, mouseUps = 0, mouseMoves = 0, touchStarts = 0, touchEnds = 0;
    };

}

TEST_F(DocumentInputTest, HoverAndActive){
    EXPECT_FALSE(button.isPseudoClassSet("hover"));

    Engine::systemMouseMove(buttonX, buttonY, 0);
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
    EXPECT_FALSE(button.isPseudoClassSet("active"));

    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, buttonX, buttonY, 0);
    EXPECT_TRUE(button.isPseudoClassSet("active"));

    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, buttonX, buttonY, 0);
    EXPECT_FALSE(button.isPseudoClassSet("active"));
    EXPECT_TRUE(button.isPseudoClassSet("hover"));

    Engine::systemMouseMove(bodyX, bodyY, 0);
    EXPECT_FALSE(button.isPseudoClassSet("hover"));

    // and the states hold through an update
    Engine::systemMouseMove(buttonX, buttonY, 0);
    updateDocuments(scene());
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
}

TEST_F(DocumentInputTest, ConsumedOverElementNotOverBody){
    // over the button: the document takes it, the game does not see it
    click(buttonX, buttonY);
    EXPECT_EQ(mouseDowns, 0);
    EXPECT_EQ(mouseUps, 0);
    EXPECT_EQ(mouseMoves, 0);

    // over the transparent body: the game's
    click(bodyX, bodyY);
    EXPECT_EQ(mouseDowns, 1);
    EXPECT_EQ(mouseUps, 1);
    EXPECT_EQ(mouseMoves, 1);

    // other buttons too (component UI only takes the left one)
    Engine::systemMouseDown(D_MOUSE_BUTTON_RIGHT, buttonX, buttonY, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_RIGHT, buttonX, buttonY, 0);
    EXPECT_EQ(mouseDowns, 1);
    EXPECT_EQ(mouseUps, 1);

    // a hidden document takes nothing
    document->hide();
    updateDocuments(scene());
    click(buttonX, buttonY);
    EXPECT_EQ(mouseDowns, 2);
    EXPECT_EQ(mouseUps, 2);
    EXPECT_FALSE(button.isPseudoClassSet("active"));

    // events the game sees whatever the document: ignoreEventsHandledByUI off
    document->show();
    updateDocuments(scene());
    Engine::setIgnoreEventsHandledByUI(false);
    click(buttonX, buttonY);
    Engine::setIgnoreEventsHandledByUI(true);
    EXPECT_EQ(mouseDowns, 3);
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
}

TEST_F(DocumentInputTest, PointerEventsNoneIsClickThrough){
    // the wrapper lets clicks through to the game
    click(overlayX, overlayY);
    EXPECT_EQ(mouseDowns, 1);
    EXPECT_FALSE(document->getElementById("overlay").isPseudoClassSet("hover"));

    // its pointer-events: auto child does not
    click(innerX, innerY);
    EXPECT_EQ(mouseDowns, 1);
    EXPECT_TRUE(document->getElementById("inner").isPseudoClassSet("hover"));
}

TEST_F(DocumentInputTest, UpAfterConsumedDownIsConsumed){
    Engine::systemMouseMove(buttonX, buttonY, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, buttonX, buttonY, 0);
    EXPECT_EQ(mouseDowns, 0);

    // dragged off the button: still the document's until the release
    Engine::systemMouseMove(bodyX, bodyY, 0);
    EXPECT_EQ(mouseMoves, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, bodyX, bodyY, 0);
    EXPECT_EQ(mouseUps, 0);
    EXPECT_FALSE(button.isPseudoClassSet("active"));

    // after the release, the body is the game's again
    Engine::systemMouseMove(bodyX + 1, bodyY, 0);
    EXPECT_EQ(mouseMoves, 1);

    // a press the game got keeps its release, even if it ends over the button
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, bodyX, bodyY, 0);
    EXPECT_EQ(mouseDowns, 1);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, buttonX, buttonY, 0);
    EXPECT_EQ(mouseUps, 0);   // over content: the document's (as UISystem does)
}

TEST_F(DocumentInputTest, TouchPointer0){
    Engine::systemTouchStart(0, buttonX, buttonY);
    EXPECT_TRUE(button.isPseudoClassSet("active"));
    EXPECT_EQ(touchStarts, 0);

    Engine::systemTouchEnd(0, buttonX, buttonY);
    EXPECT_FALSE(button.isPseudoClassSet("active"));
    EXPECT_EQ(touchEnds, 0);

    // a touch on the body is the game's
    Engine::systemTouchStart(0, bodyX, bodyY);
    Engine::systemTouchEnd(0, bodyX, bodyY);
    EXPECT_EQ(touchStarts, 1);
    EXPECT_EQ(touchEnds, 1);

    // other fingers do not reach documents (multi-touch is phase 1)
    Engine::systemTouchStart(1, buttonX, buttonY);
    EXPECT_FALSE(button.isPseudoClassSet("active"));
    EXPECT_EQ(touchStarts, 2);
    Engine::systemTouchEnd(1, buttonX, buttonY);
    EXPECT_EQ(touchEnds, 2);
}

TEST_F(DocumentInputTest, DisabledSceneEventsIgnored){
    scene().setEnableUIEvents(false);

    Engine::systemMouseMove(buttonX, buttonY, 0);
    EXPECT_FALSE(button.isPseudoClassSet("hover"));
    click(buttonX, buttonY);
    EXPECT_FALSE(button.isPseudoClassSet("active"));
    EXPECT_EQ(mouseDowns, 1);
    EXPECT_EQ(mouseUps, 1);

    // NOT_SET (the default) is not off: documents take input in any scene
    scene().setEnableUIEvents(UIEventState::NOT_SET);
    click(buttonX, buttonY);
    EXPECT_EQ(mouseDowns, 1);
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
}

TEST_F(DocumentInputTest, ReferenceSizeCoordinateMapping){
    // laid out at twice the canvas: document px = canvas px * 2
    documentSystem(scene()).setReferenceSize(1280, 720);
    updateDocuments(scene());

    const Rect box = button.getBoundingBox();
    EXPECT_FLOAT_EQ(box.getX(), 50.0f);
    EXPECT_FLOAT_EQ(box.getWidth(), 80.0f);

    Engine::systemMouseMove(buttonX / 2, buttonY / 2, 0);
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
    Engine::systemMouseMove(buttonX, buttonY, 0);
    EXPECT_FALSE(button.isPseudoClassSet("hover"));

    click(buttonX / 2, buttonY / 2);
    EXPECT_EQ(mouseDowns, 0);
}

TEST_F(DocumentInputTest, ModifiersAndScaledView){
    // a letterboxed view: screen coordinates go through the view rect to the canvas
    GpuEnvironment::setView(800, 600, 640, 360, Scaling::LETTERBOX);
    updateDocuments(scene());
    const Rect view = Engine::getViewRect();
    const float scale = view.getWidth() / 640.0f;

    Engine::systemMouseMove(view.getX() + buttonX * scale, view.getY() + buttonY * scale, D_MODIFIER_SHIFT);
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
    Engine::systemMouseMove(view.getX() + bodyX * scale, view.getY() + bodyY * scale, D_MODIFIER_SHIFT | D_MODIFIER_CONTROL);
    EXPECT_FALSE(button.isPseudoClassSet("hover"));

    GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
}
