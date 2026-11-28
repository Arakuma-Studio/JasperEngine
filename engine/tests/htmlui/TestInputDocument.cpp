// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Scene input configuration and pointer coordinate mapping. Pointer consumption and
// state transitions live in TestInput.cpp and TestInputPassThrough.cpp.

#include "TestDocumentCommon.h"
#include "InspectorTestCommon.h"

#include "Engine.h"
#include "Input.h"
#include "log/Logger.h"
#include "input/InputActions.h"
#include "input/InputScheme.h"
#include "object/ui/UIEvent.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// ---- Back (Engine::systemBack): Escape, gamepad B and Android's Back button go to the top Is Menu
// document's `back` first, then Engine.onBack, then their own path

namespace {

    const char* backTag = "htmlui-tests-back";
    const int backPad = 0;

    class DocumentBackTest : public InspectorTestBase {
    protected:
        std::unique_ptr<SceneFixture> fixture;
        std::optional<UIDocument> document;
        std::vector<uint64_t> listenerIds;
        std::vector<std::string> keydowns;   // the keys documents saw go down
        std::vector<std::string> backs;      // the id of each `back` event's target
        std::vector<std::string> gameBacks;  // Engine.onBack's sources
        std::vector<int> gameKeys;           // Engine.onKeyDown
        bool preventBack = false;
        bool padConnected = false;

        void connectPad(){
            Engine::systemGamepadConnect(backPad, "Test Gamepad");
            padConnected = true;
        }

        void SetUp() override{
            InspectorTestBase::SetUp();
            if (HasFatalFailure())
                return;
            fixture.reset(new SceneFixture());
            Engine::onKeyDown.add(backTag, std::function<void(int, bool, int)>([this](int key, bool, int){ gameKeys.push_back(key); }));
        }

        void TearDown() override{
            Engine::onKeyDown.remove(backTag);
            Engine::onBack.remove(backTag);
            if (padConnected)
                Engine::systemGamepadDisconnect(backPad);
            if (document){
                for (uint64_t id : listenerIds)
                    document->removeEventListener(id);
            }
            listenerIds.clear();
            // the Escape a Back button pressed comes up
            GpuEnvironment::drawFrames(2);
            document.reset();
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << logText(D_LOG_ERROR);
            InspectorTestBase::TearDown();
        }

        void load(const std::string& body, bool menu){
            page("back", "html, body { margin: 0; }\nbutton, select { display: block; width: 200px; height: 30px; }\n", body);
            document.emplace(fixture->addDocument("ui/back.html"));
            ASSERT_TRUE(fixture->waitUntilLoaded());
            document->setMenu(menu);
            // the menu applies, then the next latch releases what the game held (R19)
            GpuEnvironment::drawFrames(2);
            listenerIds.push_back(document->addEventListener("keydown", [this](UIEvent& event){ keydowns.push_back(event.getKey()); }));
            listenerIds.push_back(document->addEventListener("back", [this](UIEvent& event){
                backs.push_back(event.getTarget().getId());
                if (preventBack)
                    event.preventDefault();
            }));
        }

        Rml::Element* element(const char* id){
            Rml::ElementDocument* rml = fixture->getScene().getSystem<UIDocumentSystem>()->getDocument(document->getEntity());
            return rml ? rml->GetElementById(id) : nullptr;
        }

        void escape(){
            Engine::systemKeyDown(D_KEY_ESCAPE, false, 0);
            Engine::systemKeyUp(D_KEY_ESCAPE, false, 0);
        }

        void subscribeGame(){
            Engine::onBack.add(backTag, std::function<void(const std::string&)>([this](const std::string& source){ gameBacks.push_back(source); }));
        }
    };

}

// step 0: an open <select> where Escape goes closes as today; it is not a Back
TEST_F(DocumentBackTest, BackRoutingOpenSelectIsNotABack){
    load("<select id=\"pick\"><option>A</option><option>B</option></select>", false);
    subscribeGame();
    auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element("pick"));
    ASSERT_NE(select, nullptr);
    ASSERT_TRUE(select->Focus(true));
    select->ShowSelectBox();
    ASSERT_TRUE(select->IsSelectBoxVisible());

    escape();
    EXPECT_TRUE(backs.empty()) << "not a Back";
    EXPECT_TRUE(gameBacks.empty());
    EXPECT_EQ(Engine::getLastBackRoute(), BackRoute::Key);
    EXPECT_EQ(keydowns, std::vector<std::string>{"Escape"}) << "today's Escape reached the document";

    // gamepad B closes it, as today, and is not a Back either
    connectPad();
    ASSERT_TRUE(select->IsSelectBoxVisible());
    Engine::systemGamepadButtonDown(backPad, D_GAMEPAD_BUTTON_B);
    Engine::systemGamepadButtonUp(backPad, D_GAMEPAD_BUTTON_B);
    EXPECT_FALSE(select->IsSelectBoxVisible());
    EXPECT_TRUE(backs.empty());
    EXPECT_TRUE(gameBacks.empty());
    EXPECT_EQ(Engine::getLastBackRoute(), BackRoute::Key);
}

// step 1: the top menu's focused element gets `back`, which bubbles; prevented, it is consumed: no
// Escape keydown anywhere, no Engine.onBack, no game key
TEST_F(DocumentBackTest, BackRoutingMenuPreventsBack){
    load("<div id=\"panel\"><button id=\"resume\">Resume</button></div>", true);
    subscribeGame();
    ASSERT_TRUE(element("resume")->Focus(true));
    preventBack = true;

    escape();
    EXPECT_EQ(backs, std::vector<std::string>{"resume"}) << "at the focused element, bubbling to the document";
    EXPECT_EQ(Engine::getLastBackRoute(), BackRoute::Document);
    EXPECT_TRUE(keydowns.empty());
    EXPECT_TRUE(gameBacks.empty());
    EXPECT_TRUE(gameKeys.empty());

    // gamepad B the same
    connectPad();
    Engine::systemGamepadButtonDown(backPad, D_GAMEPAD_BUTTON_B);
    Engine::systemGamepadButtonUp(backPad, D_GAMEPAD_BUTTON_B);
    EXPECT_EQ(backs.size(), 2u);
    EXPECT_TRUE(keydowns.empty());
}

// step 1's default action: the topmost open modal <dialog> of the menu closes and consumes
TEST_F(DocumentBackTest, BackRoutingClosesAModalDialog){
    load("<button id=\"open\">Open</button>\n<dialog id=\"dlg\"><button id=\"inside\">OK</button></dialog>", true);
    subscribeGame();
    UIElement dialog = document->getElementById("dlg");
    ASSERT_TRUE(dialog.isValid());
    dialog.showModal();
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(dialog.hasAttribute("open"));
    // the focus outside the dialog: Escape would not close it as a key
    ASSERT_TRUE(element("open")->Focus(true));

    escape();
    EXPECT_FALSE(dialog.hasAttribute("open"));
    EXPECT_EQ(backs, std::vector<std::string>{"open"});
    EXPECT_EQ(Engine::getLastBackRoute(), BackRoute::Document);
    EXPECT_TRUE(keydowns.empty());
    EXPECT_TRUE(gameBacks.empty());
}

// step 2: nothing in the documents takes it: Engine.onBack gets "escape" and nobody sees the key;
// without a subscriber, today's Escape keydown runs
TEST_F(DocumentBackTest, BackRoutingReachesTheGame){
    load("<button id=\"resume\">Resume</button>", true);
    ASSERT_TRUE(element("resume")->Focus(true));

    escape();
    EXPECT_EQ(backs.size(), 1u) << "the menu heard it and let it go";
    EXPECT_EQ(Engine::getLastBackRoute(), BackRoute::Unconsumed);
    EXPECT_EQ(keydowns, std::vector<std::string>{"Escape"});
    EXPECT_EQ(gameKeys, std::vector<int>{D_KEY_ESCAPE});

    subscribeGame();
    keydowns.clear();
    gameKeys.clear();
    escape();
    EXPECT_EQ(gameBacks, std::vector<std::string>{"escape"});
    EXPECT_EQ(Engine::getLastBackRoute(), BackRoute::Game);
    EXPECT_TRUE(keydowns.empty());
    EXPECT_TRUE(gameKeys.empty());

    // a held Escape's repeats stay swallowed; a repeat never routes Back
    Engine::systemKeyDown(D_KEY_ESCAPE, false, 0);
    Engine::systemKeyDown(D_KEY_ESCAPE, true, 0);
    Engine::systemKeyUp(D_KEY_ESCAPE, false, 0);
    EXPECT_EQ(gameBacks.size(), 2u);
    EXPECT_TRUE(gameKeys.empty());

    // gamepad B: "gamepad"
    connectPad();
    Engine::systemGamepadButtonDown(backPad, D_GAMEPAD_BUTTON_B);
    Engine::systemGamepadButtonUp(backPad, D_GAMEPAD_BUTTON_B);
    EXPECT_EQ(gameBacks.back(), "gamepad");
}

// Android's Back button: not consumed, Escape is pressed (down now, up the frame after the one
// that saw it), and that Escape does not route Back again; consumed, nothing is pressed
TEST_F(DocumentBackTest, BackRoutingAndroidKeepsEscape){
    load("<button id=\"resume\">Resume</button>", true);
    ASSERT_TRUE(element("resume")->Focus(true));

    EXPECT_FALSE(Engine::systemBack(BackSource::BackButton));
    EXPECT_EQ(backs.size(), 1u) << "one Back, not a second one for the engine's Escape";
    EXPECT_EQ(keydowns, std::vector<std::string>{"Escape"});
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_ESCAPE));
    EXPECT_TRUE(Input::getKey(D_KEY_ESCAPE));
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(Input::getKeyUp(D_KEY_ESCAPE));
    EXPECT_FALSE(Input::getKey(D_KEY_ESCAPE));

    subscribeGame();
    keydowns.clear();
    EXPECT_TRUE(Engine::systemBack(BackSource::BackButton));
    EXPECT_EQ(gameBacks, std::vector<std::string>{"back_button"});
    EXPECT_TRUE(keydowns.empty());
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(Input::getKeyDown(D_KEY_ESCAPE));
}

// a menu appearing releases what the game holds (a key, the HUD's stick finger) in the next latch;
// what the menu document holds stays; isMenuShowing follows show, hide, enabled and interactive
TEST_F(DocumentBackTest, MenuOpeningReleasesGameInput){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    InputSchemeTracker::reset(true);
    page("hud", "html, body { margin: 0; }\n#stick { position: absolute; left: 0; top: 160px; width: 200px; height: 200px; }\n",
         "<onscreen-stick id=\"stick\"></onscreen-stick>");
    page("menu", "html, body { margin: 0; }\n#ok { position: absolute; left: 600px; top: 100px; width: 200px; height: 60px; }\n",
         "<button id=\"ok\">OK</button>");
    fixture->addDocument("ui/hud.html");
    UIDocument menu = fixture->addDocument("ui/menu.html");
    menu.setMenu(true);
    menu.hide();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(Engine::isMenuShowing());

    // W held, a finger on the stick half a radius right
    Engine::systemKeyDown(D_KEY_W, false, 0);
    Engine::systemTouchStart(0, 100, 260);
    Engine::systemTouchMove(0, 140, 260);
    GpuEnvironment::drawFrames(1);
    ASSERT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.01f);
    ASSERT_TRUE(Input::getKey(D_KEY_W));

    menu.show();
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(Engine::isMenuShowing());
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(Input::getKeyUp(D_KEY_W)) << "W comes up in the latch after the menu appeared";
    EXPECT_FALSE(Input::getKey(D_KEY_W));
    bool canceled = false;
    for (size_t i = 0; i < Input::getTouchCount(); i++){
        const Touch touch = Input::getTouch(i);
        canceled = canceled || (touch.pointer == 0 && touch.phase == TouchPhase::CANCELED);
    }
    EXPECT_TRUE(canceled) << "the finger is reported Canceled";
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 0.01f);
    Engine::systemKeyUp(D_KEY_W, false, 0);
    Engine::systemTouchEnd(0, 140, 260);

    // a press the document holds when it becomes the menu stays
    menu.setMenu(false);
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(Engine::isMenuShowing());
    Engine::systemMouseMove(700, 130, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 700, 130, 0);
    Rml::ElementDocument* menuRml = fixture->getScene().getSystem<UIDocumentSystem>()->getDocument(menu.getEntity());
    Rml::Element* ok = menuRml->GetElementById("ok");
    ASSERT_TRUE(ok->IsPseudoClassSet("active"));
    menu.setMenu(true);
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(Engine::isMenuShowing());
    EXPECT_TRUE(ok->IsPseudoClassSet("active")) << "the menu's own press";
    EXPECT_TRUE(Input::getMouseButton(D_MOUSE_BUTTON_LEFT)) << "a press the UI took stays held";
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 700, 130, 0);

    // enabled and interactive
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(Engine::isMenuShowing());
    fixture->getScene().getComponent<UIDocumentComponent>(menu.getEntity()).interactive = false;
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(Engine::isMenuShowing());
    fixture->getScene().getComponent<UIDocumentComponent>(menu.getEntity()).interactive = true;
    menu.setVisible(false);
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(Engine::isMenuShowing());
}

// Pause On Focus Loss stays quiet while a menu shows
TEST_F(DocumentBackTest, PauseRequestedOnlyWithoutMenu){
    std::vector<std::string> requests;
    Engine::onPauseRequested.add(backTag, std::function<void(const std::string&)>([&requests](const std::string& reason){ requests.push_back(reason); }));
    Engine::setPauseOnFocusLoss(true);
    load("<button id=\"resume\">Resume</button>", true);
    Engine::systemFocus(false);
    Engine::systemFocus(true);
    EXPECT_TRUE(requests.empty()) << "a menu covers the game already";
    document->setMenu(false);
    GpuEnvironment::drawFrames(1);
    Engine::systemFocus(false);
    Engine::systemFocus(true);
    EXPECT_EQ(requests, std::vector<std::string>{"focus_lost"});
    Engine::setPauseOnFocusLoss(false);
    Engine::onPauseRequested.remove(backTag);
}

// on, with nobody subscribed to Engine.onPauseRequested: one warning in the first frame of a Play
TEST_F(DocumentBackTest, PauseOnFocusLossWarnsWithoutSubscriber){
    Engine::setPauseOnFocusLoss(true);
    Engine::resetApplicationState();
    TestSystem::get().clearLogs();
    GpuEnvironment::drawFrames(3);
    ::jasperengine::Logger::flush();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN), 1u) << logText(D_LOG_WARN);
    Engine::setPauseOnFocusLoss(false);
}
