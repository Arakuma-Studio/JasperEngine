// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Scene input configuration and pointer coordinate mapping. Pointer consumption and
// state transitions live in TestInput.cpp and TestInputPassThrough.cpp.

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

    class DocumentInputTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            fixture.reset(new SceneFixture());
            document = &fixture->add<UIDocument>();
            ASSERT_TRUE(document->load("ecs/input.rml"));
            button = document->getElementById("button");
            ASSERT_TRUE(button.isValid());

            Engine::onMouseDown.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ mouseDowns++; }));
            Engine::onMouseUp.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ mouseUps++; }));
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            Engine::onMouseDown.remove(callbackTag);
            Engine::onMouseUp.remove(callbackTag);
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
        int mouseDowns = 0, mouseUps = 0;
    };

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
