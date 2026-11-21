// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Gamepads reach documents (phase 4, p4-input): Engine::systemGamepad* turns the buttons, the
// left stick's directions and the triggers into keydown/keyup events of the focused document
// (code "GamepadA" ..., key "ArrowDown" / "Enter" / "Escape" where the key stands for a keyboard
// key). The d-pad and the stick move the focus (spatial navigation, nav-* properties, fork
// P4-2), A activates as Enter/Space with :active while held, B is a cancelable Escape; held
// directions repeat on the UI clock. What a focused document consumes does not reach the game.

#include "InspectorTestCommon.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "HtmlUIContext.h"
#include "Input.h"
#include "input/InputScheme.h"
#include "object/ui/UIEvent.h"
#include "subsystem/GamepadCursor.h"
#include "component/ButtonComponent.h"
#include "component/OnScreenControlComponents.h"
#include "input/InputActions.h"
#include "object/ui/Button.h"
#include "object/ui/OnScreenControls.h"
#include "AllocationCounter.h"

#include <chrono>
#include <cstdio>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "htmlui-tests-input-gamepad";
    const int pad = 0;

    // a 3 x 3 grid of buttons (b1 ... b9, rows of three), a status line
    const char* kGridCss =
        "html, body { margin: 0; }\n"
        ".grid { display: grid; grid-template-columns: repeat(3, 100px); gap: 20px; padding: 20px; }\n"
        "button { width: 100px; height: 40px; }\n";
    const char* kGridBody =
        "<div class=\"grid\">\n"
        "<button id=\"b1\">1</button><button id=\"b2\">2</button><button id=\"b3\">3</button>\n"
        "<button id=\"b4\">4</button><button id=\"b5\">5</button><button id=\"b6\">6</button>\n"
        "<button id=\"b7\">7</button><button id=\"b8\">8</button><button id=\"b9\">9</button>\n"
        "</div>";

    class InputGamepadTest : public InspectorTestBase {
    protected:
        std::unique_ptr<SceneFixture> fixture;
        std::optional<UIDocument> document;
        UIDocument& doc(){ return *document; }

        void SetUp() override{
            InspectorTestBase::SetUp();
            if (HasFatalFailure())
                return;
            fixture.reset(new SceneFixture());
            HtmlUIClock::setFixedTime(100.0);
            Engine::onGamepadButtonDown.add(callbackTag, std::function<void(int, int)>([this](int, int button){ gameDowns.push_back(button); }));
            Engine::onGamepadButtonUp.add(callbackTag, std::function<void(int, int)>([this](int, int button){ gameUps.push_back(button); }));
            Engine::onGamepadAxisMove.add(callbackTag, std::function<void(int, int, float)>([this](int, int axis, float value){
                gameAxes.push_back(std::make_pair(axis, value));
            }));
            Engine::systemGamepadConnect(pad, "Test Gamepad");
        }

        void TearDown() override{
            Engine::systemGamepadDisconnect(pad);
            Engine::onGamepadButtonDown.remove(callbackTag);
            Engine::onGamepadButtonUp.remove(callbackTag);
            Engine::onGamepadAxisMove.remove(callbackTag);
            Engine::setIgnoreEventsHandledByUI(true);
            if (document){
                for (uint64_t id : listenerIds)
                    doc().removeEventListener(id);
            }
            listenerIds.clear();
            HtmlUIClock::setFixedTime(std::nullopt);
            document.reset();
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << logText(D_LOG_ERROR);
            InspectorTestBase::TearDown();
        }

        void load(const std::string& css, const std::string& body){
            page("pad", css, body);
            document.emplace(fixture->addDocument("ui/pad.html"));
            ASSERT_TRUE(fixture->waitUntilLoaded());
        }

        UIDocumentSystem& system(){
            return *fixture->getScene().getSystem<UIDocumentSystem>();
        }

        Rml::Context* context(){
            return system().getContext()->getRmlContext();
        }

        Rml::Element* element(const char* id){
            Rml::ElementDocument* rml = system().getDocument(doc().getEntity());
            return rml ? rml->GetElementById(id) : nullptr;
        }

        std::string focusId(){
            Rml::Element* focus = context()->GetFocusElement();
            if (!focus || focus == context()->GetRootElement() || rmlui_dynamic_cast<Rml::ElementDocument*>(focus))
                return "(document)";
            return focus->GetId();
        }

        void focus(const char* id){
            Rml::Element* target = element(id);
            ASSERT_NE(target, nullptr) << id;
            ASSERT_TRUE(target->Focus());
        }

        void press(int button){
            Engine::systemGamepadButtonDown(pad, button);
            Engine::systemGamepadButtonUp(pad, button);
        }

        // a frame at a UI clock time
        void frameAt(double seconds){
            HtmlUIClock::setFixedTime(seconds);
            GpuEnvironment::drawFrames(1);
        }

        // what a document listener sees of each key event
        struct Seen {
            std::string type, key, code;
            int keyCode = 0, gamepad = -2;
            bool repeat = false;
        };
        std::vector<Seen> seen;
        void record(std::function<void(UIEvent&)> also = nullptr){
            for (const char* type : {"keydown", "keyup"}){
                listenerIds.push_back(doc().addEventListener(type, [this, also](UIEvent& event){
                    seen.push_back(Seen{event.getType(), event.getKey(), event.getCode(), event.getKeyCode(), event.getGamepad(), event.isRepeat()});
                    if (also)
                        also(event);
                }));
            }
        }

        std::vector<int> gameDowns, gameUps;
        std::vector<std::pair<int, float>> gameAxes;
        std::vector<uint64_t> listenerIds;
    };

    class ClickCounter : public Rml::EventListener {
    public:
        int clicks = 0;
        void ProcessEvent(Rml::Event&) override{ clicks++; }
    };

}

// the d-pad moves the focus to the nearest button in each direction; at the edge it stays, and
// the focused document still takes the press
TEST_F(InputGamepadTest, GridNavigation){
    load(kGridCss, kGridBody);
    focus("b5");

    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(focusId(), "b2");
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "b5");
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "b8");
    press(D_GAMEPAD_BUTTON_DPAD_LEFT);
    EXPECT_EQ(focusId(), "b7");
    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    EXPECT_EQ(focusId(), "b9");
    // the edges: no wrap
    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "b9");
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(focusId(), "b3");

    // the left stick navigates the same way (pressed past 0.5, released below 0.3)
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, -0.8f);
    EXPECT_EQ(focusId(), "b2");
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, -0.4f);   // still held: no second step
    EXPECT_EQ(focusId(), "b2");
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, 0.9f);
    EXPECT_EQ(focusId(), "b5");
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, 0.0f);

    EXPECT_TRUE(gameDowns.empty()) << "the focused document takes the d-pad";
    EXPECT_TRUE(gameUps.empty());
    EXPECT_TRUE(gameAxes.empty()) << "and the stick";
}

// a scrolling list: the next item comes before the element below the list, the list scrolls to
// it, and from the last item the focus leaves the list
TEST_F(InputGamepadTest, ListNavigationScrolls){
    load("html, body { margin: 0; }\n"
         "#list { display: flex; flex-direction: column; width: 200px; height: 130px; overflow-y: auto; }\n"
         "#list button { flex: none; height: 40px; margin-bottom: 10px; }\n"
         "#after { width: 200px; height: 40px; }\n",
         "<button id=\"before\">Before</button>\n"
         "<div id=\"list\">\n"
         "<button id=\"i1\">1</button><button id=\"i2\">2</button><button id=\"i3\">3</button><button id=\"i4\">4</button>\n"
         "<button id=\"i5\">5</button><button id=\"i6\">6</button><button id=\"i7\">7</button><button id=\"i8\">8</button>\n"
         "</div>\n"
         "<button id=\"after\">After</button>");
    focus("before");
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "i1");

    Rml::Element* list = element("list");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->GetScrollTop(), 0.0f);
    for (int i = 2; i <= 8; i++){
        press(D_GAMEPAD_BUTTON_DPAD_DOWN);
        EXPECT_EQ(focusId(), "i" + std::to_string(i));
    }
    // the last item is scrolled into view (the list is 130px tall, the items 50px apart)
    GpuEnvironment::drawFrames(1);
    EXPECT_GT(list->GetScrollTop(), 250.0f);
    Rml::Element* last = element("i8");
    const float listBottom = list->GetAbsoluteOffset(Rml::BoxArea::Padding).y + list->GetClientHeight();
    EXPECT_LE(last->GetAbsoluteOffset(Rml::BoxArea::Border).y + last->GetOffsetHeight(), listBottom + 0.5f);

    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "after");
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(focusId(), "i8") << "back into the list where it is visible";
}

// nav-* properties override the spatial default: an #id, none, and the default for the rest
TEST_F(InputGamepadTest, NavPropertiesOverride){
    load(std::string(kGridCss) + "#b5 { nav-down: #b1; nav-left: none; }\n", kGridBody);
    focus("b5");
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "b1");
    focus("b5");
    press(D_GAMEPAD_BUTTON_DPAD_LEFT);
    EXPECT_EQ(focusId(), "b5") << "nav-left: none";
    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    EXPECT_EQ(focusId(), "b6") << "undeclared: spatial";
    EXPECT_TRUE(gameDowns.empty());
}

// A clicks the focused button on press (Enter) and holds :active until the release; a checkbox
// toggles on the release (Space); preventDefault keeps both from happening
TEST_F(InputGamepadTest, ActivationAndActive){
    load(kGridCss, std::string(kGridBody) + "<input id=\"check\" type=\"checkbox\">");
    ClickCounter clicks;
    Rml::Element* b5 = element("b5");
    ASSERT_NE(b5, nullptr);
    b5->AddEventListener(Rml::EventId::Click, &clicks);
    record();
    focus("b5");

    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(clicks.clicks, 1);
    EXPECT_TRUE(b5->IsPseudoClassSet("active"));
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_FALSE(b5->IsPseudoClassSet("active"));
    EXPECT_EQ(clicks.clicks, 1);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].type, "keydown");
    EXPECT_EQ(seen[0].key, "Enter");
    EXPECT_EQ(seen[0].code, "GamepadA");
    EXPECT_EQ(seen[0].keyCode, D_KEY_ENTER);
    EXPECT_EQ(seen[0].gamepad, pad);
    EXPECT_EQ(seen[1].type, "keyup");
    EXPECT_EQ(seen[1].code, "GamepadA");
    EXPECT_TRUE(gameDowns.empty());
    EXPECT_TRUE(gameUps.empty()) << "an up whose down a document took";

    Rml::Element* check = element("check");
    ASSERT_NE(check, nullptr);
    focus("check");
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_FALSE(check->HasAttribute("checked")) << "a checkbox toggles on the release";
    EXPECT_TRUE(check->IsPseudoClassSet("active"));
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_TRUE(check->HasAttribute("checked"));
    EXPECT_FALSE(check->IsPseudoClassSet("active"));

    // a listener that prevents the key: no click, no :active (the document still takes A)
    listenerIds.push_back(doc().addEventListener("keydown", [](UIEvent& event){
        if (event.getCode() == "GamepadA")
            event.preventDefault();
    }));
    focus("b5");
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_FALSE(b5->IsPseudoClassSet("active"));
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(clicks.clicks, 1);
    EXPECT_TRUE(gameDowns.empty());
    b5->RemoveEventListener(Rml::EventId::Click, &clicks);
}

// B is a cancelable Escape: a menu that handles it consumes it, otherwise the game gets it
TEST_F(InputGamepadTest, CancelWithB){
    load(kGridCss, kGridBody);
    bool closeMenu = true;
    int closes = 0;
    record([&](UIEvent& event){
        if (closeMenu && event.getType() == "keydown" && event.getKey() == "Escape"){
            closes++;
            event.preventDefault();
        }
    });
    focus("b1");

    press(D_GAMEPAD_BUTTON_B);
    EXPECT_EQ(closes, 1);
    ASSERT_GE(seen.size(), 1u);
    EXPECT_EQ(seen[0].key, "Escape");
    EXPECT_EQ(seen[0].code, "GamepadB");
    EXPECT_EQ(seen[0].keyCode, D_KEY_ESCAPE);
    EXPECT_TRUE(gameDowns.empty()) << "the menu used B";
    EXPECT_TRUE(gameUps.empty());

    // nobody uses it: the game gets B, down and up
    closeMenu = false;
    press(D_GAMEPAD_BUTTON_B);
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(gameDowns, std::vector<int>{D_GAMEPAD_BUTTON_B});
    EXPECT_EQ(gameUps, std::vector<int>{D_GAMEPAD_BUTTON_B});
    EXPECT_EQ(focusId(), "b1");
}

// a range adjusts with left/right while focused; up/down leave it
TEST_F(InputGamepadTest, SliderAdjusts){
    load("html, body { margin: 0; }\n"
         "button, input { display: block; width: 200px; height: 30px; margin: 10px; }\n",
         "<button id=\"top\">Top</button>\n"
         "<input id=\"volume\" type=\"range\" min=\"0\" max=\"10\" step=\"1\" value=\"5\">\n"
         "<button id=\"bottom\">Bottom</button>");
    UIElement volume = doc().getElementById("volume");
    ASSERT_TRUE(volume.isValid());
    focus("volume");

    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    EXPECT_FLOAT_EQ(std::stof(volume.getValue()), 6.0f);
    press(D_GAMEPAD_BUTTON_DPAD_LEFT);
    press(D_GAMEPAD_BUTTON_DPAD_LEFT);
    EXPECT_FLOAT_EQ(std::stof(volume.getValue()), 4.0f);
    EXPECT_EQ(focusId(), "volume");
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "bottom");
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(focusId(), "top");
    EXPECT_FLOAT_EQ(std::stof(volume.getValue()), 4.0f);
    EXPECT_TRUE(gameDowns.empty());
}

// a select: A opens it, the d-pad moves through the options, A chooses, B closes; closed, the
// d-pad leaves it
TEST_F(InputGamepadTest, SelectOpensAndNavigates){
    load("html, body { margin: 0; }\n"
         "button, select { display: block; width: 200px; height: 30px; margin: 10px; }\n",
         "<button id=\"top\">Top</button>\n"
         "<select id=\"quality\"><option value=\"low\">Low</option><option value=\"mid\" selected>Mid</option>"
         "<option value=\"high\">High</option></select>\n"
         "<button id=\"bottom\">Bottom</button>");
    auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element("quality"));
    ASSERT_NE(select, nullptr);
    focus("quality");

    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "bottom") << "a closed select leaves the d-pad to navigation";
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(focusId(), "quality");
    EXPECT_EQ(select->GetValue(), "mid");

    press(D_GAMEPAD_BUTTON_A);
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(select->IsSelectBoxVisible());
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "quality") << "an open select keeps the d-pad";
    EXPECT_TRUE(select->IsSelectBoxVisible());
    press(D_GAMEPAD_BUTTON_A);
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(select->IsSelectBoxVisible());
    EXPECT_EQ(select->GetValue(), "high");

    // B closes an open select (consumed: the game does not get it)
    press(D_GAMEPAD_BUTTON_A);
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(select->IsSelectBoxVisible());
    press(D_GAMEPAD_BUTTON_B);
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(select->IsSelectBoxVisible());
    EXPECT_TRUE(gameDowns.empty());
}

// a text field does not keep the gamepad: the d-pad leaves it, A edits nothing, B reaches the menu
TEST_F(InputGamepadTest, TextFieldDoesNotTrap){
    load("html, body { margin: 0; }\n"
         "button, input { display: block; width: 200px; height: 30px; margin: 10px; }\n",
         "<input id=\"name\" type=\"text\" value=\"abc\">\n"
         "<button id=\"ok\">OK</button>");
    int escapes = 0;
    record([&](UIEvent& event){
        if (event.getType() == "keydown" && event.getKey() == "Escape"){
            escapes++;
            event.preventDefault();
        }
    });
    UIElement name = doc().getElementById("name");
    focus("name");

    press(D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(name.getValue(), "abc");
    EXPECT_EQ(focusId(), "name");
    press(D_GAMEPAD_BUTTON_B);
    EXPECT_EQ(escapes, 1) << "B reaches the document past the field";
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "ok");
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(focusId(), "name");
    EXPECT_EQ(name.getValue(), "abc");
    EXPECT_TRUE(gameDowns.empty());
}

// without a focused element the gamepad is the game's; with one, navigation is the document's,
// other buttons only when a listener uses them; a stick the documents take is reset for the game
TEST_F(InputGamepadTest, ConsumeOrPassToTheGame){
    load(kGridCss, kGridBody);
    ASSERT_EQ(focusId(), "(document)");
    EXPECT_FALSE(Engine::wantsGamepadNavigation());

    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    press(D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(gameDowns, (std::vector<int>{D_GAMEPAD_BUTTON_DPAD_DOWN, D_GAMEPAD_BUTTON_A}));
    EXPECT_EQ(gameUps, (std::vector<int>{D_GAMEPAD_BUTTON_DPAD_DOWN, D_GAMEPAD_BUTTON_A}));
    EXPECT_EQ(focusId(), "(document)") << "no navigation without a focused element";
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.2f);
    ASSERT_EQ(gameAxes.size(), 1u);
    EXPECT_EQ(gameAxes.back(), std::make_pair(D_GAMEPAD_AXIS_LEFT_X, 0.2f));

    gameDowns.clear();
    gameUps.clear();
    gameAxes.clear();
    focus("b1");
    EXPECT_TRUE(Engine::wantsGamepadNavigation());

    // the stick the game had at 0.2 is the menu's now: the game gets 0 once, then nothing
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.3f);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.8f);
    EXPECT_EQ(focusId(), "b2");
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    ASSERT_EQ(gameAxes.size(), 1u);
    EXPECT_EQ(gameAxes[0], std::make_pair(D_GAMEPAD_AXIS_LEFT_X, 0.0f));
    // the right stick is always the game's
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_Y, 0.7f);
    ASSERT_EQ(gameAxes.size(), 2u);
    EXPECT_EQ(gameAxes[1], std::make_pair(D_GAMEPAD_AXIS_RIGHT_Y, 0.7f));

    // a shoulder button nobody listens to is the game's; with a listener that uses it, the menu's
    press(D_GAMEPAD_BUTTON_RIGHT_BUMPER);
    EXPECT_EQ(gameDowns, std::vector<int>{D_GAMEPAD_BUTTON_RIGHT_BUMPER});
    int tabs = 0;
    listenerIds.push_back(doc().addEventListener("keydown", [&](UIEvent& event){
        if (event.getCode() == "GamepadRightShoulder"){
            tabs++;
            event.stopPropagation();
        }
    }));
    press(D_GAMEPAD_BUTTON_RIGHT_BUMPER);
    EXPECT_EQ(tabs, 1);
    EXPECT_EQ(gameDowns, std::vector<int>{D_GAMEPAD_BUTTON_RIGHT_BUMPER});
    EXPECT_EQ(gameUps, std::vector<int>{D_GAMEPAD_BUTTON_RIGHT_BUMPER});

    // the focus goes (a press on the empty page): the gamepad is the game's again
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 900, 600, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 900, 600, 0);
    EXPECT_EQ(focusId(), "(document)");
    EXPECT_FALSE(Engine::wantsGamepadNavigation());
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(gameDowns.back(), D_GAMEPAD_BUTTON_DPAD_UP);

    // ignoreEventsHandledByUI off: the game gets everything, the document still navigates
    focus("b1");
    Engine::setIgnoreEventsHandledByUI(false);
    gameDowns.clear();
    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    EXPECT_EQ(focusId(), "b2");
    EXPECT_EQ(gameDowns, std::vector<int>{D_GAMEPAD_BUTTON_DPAD_RIGHT});
}

// gamepad navigation shows the focus ring: :focus-visible on the element it moves to, and on an
// element a click focused once the gamepad is used
TEST_F(InputGamepadTest, FocusVisible){
    load(kGridCss, kGridBody);
    Rml::Element* b1 = element("b1");
    Rml::Element* b2 = element("b2");
    ASSERT_TRUE(b1 && b2);
    const Rml::Vector2f center = b1->GetAbsoluteOffset(Rml::BoxArea::Border) + b1->GetBox().GetSize() * 0.5f;
    Engine::systemMouseMove(center.x, center.y, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, center.x, center.y, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, center.x, center.y, 0);
    ASSERT_EQ(focusId(), "b1");
    EXPECT_FALSE(b1->IsPseudoClassSet("focus-visible")) << "a click shows no focus ring";

    press(D_GAMEPAD_BUTTON_Y);
    EXPECT_TRUE(b1->IsPseudoClassSet("focus-visible")) << "any gamepad key shows it";
    press(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    EXPECT_EQ(focusId(), "b2");
    EXPECT_TRUE(b2->IsPseudoClassSet("focus-visible"));
    EXPECT_FALSE(b1->IsPseudoClassSet("focus-visible"));
}

// a held direction repeats after 0.4 s, then every 0.1 s, on the UI clock; the repeats are
// keydowns with repeat set; the stick repeats too
TEST_F(InputGamepadTest, RepeatTiming){
    load(".col button { display: block; width: 100px; height: 20px; margin: 4px; }\n",
         "<div class=\"col\">\n"
         "<button id=\"r1\">1</button><button id=\"r2\">2</button><button id=\"r3\">3</button><button id=\"r4\">4</button>\n"
         "<button id=\"r5\">5</button><button id=\"r6\">6</button><button id=\"r7\">7</button><button id=\"r8\">8</button>\n"
         "</div>");
    record();
    focus("r1");
    frameAt(200.0);

    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "r2");
    frameAt(200.2);
    frameAt(200.399);
    EXPECT_EQ(focusId(), "r2") << "no repeat before 0.4 s";
    frameAt(200.401);
    EXPECT_EQ(focusId(), "r3");
    frameAt(200.45);
    EXPECT_EQ(focusId(), "r3");
    frameAt(200.501);
    EXPECT_EQ(focusId(), "r4");
    frameAt(200.601);
    EXPECT_EQ(focusId(), "r5");
    // a long frame gives one repeat, not a burst
    frameAt(201.5);
    EXPECT_EQ(focusId(), "r6");
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_DPAD_DOWN);
    frameAt(202.5);
    EXPECT_EQ(focusId(), "r6");

    int repeats = 0, downs = 0;
    for (const Seen& event : seen){
        if (event.type != "keydown")
            continue;
        downs++;
        if (event.repeat)
            repeats++;
        EXPECT_EQ(event.key, "ArrowDown");
        EXPECT_EQ(event.code, "GamepadDPadDown");
    }
    EXPECT_EQ(downs, 5);
    EXPECT_EQ(repeats, 4);

    // the stick: held up, it repeats on the same clock
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, -1.0f);
    EXPECT_EQ(focusId(), "r5");
    frameAt(202.85);
    EXPECT_EQ(focusId(), "r5");
    frameAt(202.901);
    EXPECT_EQ(focusId(), "r4");
    EXPECT_EQ(seen.back().code, "GamepadLeftThumbstickUp");
    EXPECT_TRUE(seen.back().repeat);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, -0.1f);
    EXPECT_EQ(seen.back().type, "keyup");
    frameAt(203.5);
    EXPECT_EQ(focusId(), "r4");
    EXPECT_TRUE(gameDowns.empty());
}

// shoulders, triggers and the other buttons reach listeners as keys; triggers press past half
// way; a disconnect releases what the pad held
TEST_F(InputGamepadTest, ButtonsAndTriggersAsKeys){
    load(kGridCss, kGridBody);
    record();
    focus("b5");

    press(D_GAMEPAD_BUTTON_LEFT_BUMPER);
    press(D_GAMEPAD_BUTTON_START);
    press(D_GAMEPAD_BUTTON_X);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_TRIGGER, -0.5f);   // a quarter: not pressed
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.6f);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.1f);    // still held
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_TRIGGER, -1.0f);

    std::vector<std::string> codes;
    for (const Seen& event : seen)
        codes.push_back(event.type + " " + event.code + " " + event.key);
    const std::vector<std::string> expected = {
        "keydown GamepadLeftShoulder GamepadLeftShoulder", "keyup GamepadLeftShoulder GamepadLeftShoulder",
        "keydown GamepadMenu GamepadMenu", "keyup GamepadMenu GamepadMenu",
        "keydown GamepadX GamepadX", "keyup GamepadX GamepadX",
        "keydown GamepadRightTrigger GamepadRightTrigger", "keyup GamepadRightTrigger GamepadRightTrigger",
    };
    EXPECT_EQ(codes, expected);
    for (const Seen& event : seen){
        EXPECT_EQ(event.keyCode, 0);
        EXPECT_EQ(event.gamepad, pad);
    }
    // nobody used them: the game got them all (buttons and the trigger axis)
    EXPECT_EQ(gameDowns, (std::vector<int>{D_GAMEPAD_BUTTON_LEFT_BUMPER, D_GAMEPAD_BUTTON_START, D_GAMEPAD_BUTTON_X}));
    EXPECT_EQ(gameAxes.size(), 4u);

    // A held when the pad goes: the button lets go of :active
    Rml::Element* b5 = element("b5");
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_TRUE(b5->IsPseudoClassSet("active"));
    Engine::systemGamepadDisconnect(pad);
    EXPECT_FALSE(b5->IsPseudoClassSet("active"));
    Engine::systemGamepadConnect(pad, "Test Gamepad");
}

// a click on a panel focuses it (RmlUi focuses any clicked element) but it is not in the Tab
// order: the keys go to it, the gamepad stays the game's
TEST_F(InputGamepadTest, AClickedPanelDoesNotTakeTheGamepad){
    load("html, body { margin: 0; }\n"
         "#panel { width: 300px; height: 100px; background-color: #333333; }\n",
         "<div id=\"panel\">HUD</div><button id=\"ok\">OK</button>");
    Engine::systemMouseMove(50, 50, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 50, 50, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 50, 50, 0);
    ASSERT_EQ(focusId(), "panel");
    EXPECT_FALSE(Engine::wantsGamepadNavigation());
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    press(D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(focusId(), "panel");
    EXPECT_EQ(gameDowns, (std::vector<int>{D_GAMEPAD_BUTTON_DPAD_DOWN, D_GAMEPAD_BUTTON_A}));

    // a button in the Tab order takes it
    focus("ok");
    EXPECT_TRUE(Engine::wantsGamepadNavigation());
    press(D_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(gameDowns.size(), 2u);
}

// B closes a modal <dialog> as Escape does (its cancel and close events)
TEST_F(InputGamepadTest, BClosesAModalDialog){
    load("html, body { margin: 0; }\n",
         "<button id=\"open\">Open</button>\n"
         "<dialog id=\"dlg\"><button id=\"inside\">OK</button></dialog>");
    UIElement dialog = doc().getElementById("dlg");
    ASSERT_TRUE(dialog.isValid());
    dialog.showModal();
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(dialog.hasAttribute("open"));
    focus("inside");
    press(D_GAMEPAD_BUTTON_B);
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(dialog.hasAttribute("open"));
}

// ui-components: a stick held while the menu lets go of the pad (the focus goes) reaches the game
// in the next frame as it is, without being centred first (Doriax 71a87b533's case)
TEST_F(InputGamepadTest, StickHeldWhenMenuLetsGoGoesBackToTheGame){
    load(kGridCss, kGridBody);
    focus("b1");
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.9f);
    EXPECT_EQ(focusId(), "b2");
    EXPECT_TRUE(gameAxes.empty()) << "the menu has the stick";

    // the focus goes (a press on the empty page); the stick stays where it is
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 900, 600, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 900, 600, 0);
    ASSERT_FALSE(Engine::wantsGamepadNavigation());
    frameAt(100.05);
    ASSERT_FALSE(gameAxes.empty()) << "the held stick goes back to the game";
    EXPECT_EQ(gameAxes.back(), std::make_pair(D_GAMEPAD_AXIS_LEFT_X, 0.9f));
}

// ui-components: a text field typed in a lower scene keeps Enter; an upper document's focused
// button does not take it (the typing guard)
TEST_F(InputGamepadTest, TypingFieldKeepsEnterFromAnUpperDocument){
    load(kGridCss, kGridBody);
    focus("b1");

    // the lower scene: a text field (the fixture made last is the main scene, under the other)
    std::unique_ptr<SceneFixture> lower(new SceneFixture());
    Engine::addSceneLayer(&fixture->getScene());
    page("field", "", "<input type=\"text\" id=\"name\" style=\"width: 200px;\"/>");
    UIDocument field = lower->addDocument("ui/field.html");
    ASSERT_TRUE(lower->waitUntilLoaded());
    Rml::ElementDocument* rml = lower->getScene().getSystem<UIDocumentSystem>()->getDocument(field.getEntity());
    ASSERT_NE(rml, nullptr);
    Rml::Element* input = rml->GetElementById("name");
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->Focus());
    ASSERT_TRUE(Engine::isTextInputFocused());

    ClickCounter clicks;
    element("b1")->AddEventListener(Rml::EventId::Click, &clicks);
    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    element("b1")->RemoveEventListener(Rml::EventId::Click, &clicks);
    EXPECT_EQ(clicks.clicks, 0) << "the upper document's button does not take the typing keys (Space clicks on its release)";

    Engine::removeScene(&fixture->getScene());
    lower.reset();
    Engine::setScene(&fixture->getScene());
}

// ---- the gamepad cursor (GamepadCursor): a Gamepad Cursor document in the Gamepad scheme

namespace {

    const char* cursorTag = "htmlui-tests-gamepad-cursor";
    constexpr double cursorFrame = 1.0 / 60.0;

    class InputGamepadCursorTest : public InputGamepadTest {
    protected:
        void SetUp() override{
            InputGamepadTest::SetUp();
            if (HasFatalFailure())
                return;
            // game time and the cursor's unscaled time advance one 60 Hz frame per drawn frame
            Engine::setFixedDeltatime(cursorFrame);
            Engine::onMouseDown.add(cursorTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ gameMouseDowns++; }));
        }

        void TearDown() override{
            Engine::onMouseDown.remove(cursorTag);
            Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
            Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, 0.0f);
            Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_Y, 0.0f);
            InputSchemeTracker::setOverride(std::nullopt);
            GamepadCursor::resetSession();
            Engine::setFixedDeltatime(0.0);
            InputGamepadTest::TearDown();
        }

        // the document with Gamepad Cursor on, and the Gamepad scheme from a pad press (X: nobody's)
        void loadCursor(const std::string& css, const std::string& body){
            load(css, body);
            doc().setGamepadCursor(true);
            GpuEnvironment::drawFrames(1);
            usePad();
        }

        void usePad(){
            press(D_GAMEPAD_BUTTON_X);
            GpuEnvironment::drawFrames(1);
            ASSERT_EQ(InputSchemeTracker::getCurrent(), InputScheme::Gamepad);
        }

        // the UI clock advances with the frames (smooth scrolling and transitions run on it)
        void frames(int count){
            for (int i = 0; i < count; i++){
                uiTime += cursorFrame;
                HtmlUIClock::setFixedTime(uiTime);
                GpuEnvironment::drawFrames(1);
            }
        }

        int gameMouseDowns = 0;
        double uiTime = 100.0;
    };

    // pointer moves with the left button held (a drag in progress)
    class HeldMoves : public Rml::EventListener {
    public:
        int moves = 0;
        float lastX = 0.0f;
        void ProcessEvent(Rml::Event& event) override{
            if (event.GetParameter<int>("buttons", 0) & 1){
                moves++;
                lastX = event.GetParameter<float>("mouse_x", 0.0f);
            }
        }
    };

    // canvas px per second at full stick on this 1280x720 canvas
    const float fullSpeed = 1000.0f * 720.0f / 1080.0f;

}

// the left stick moves it at speed x |stick|^2 after the acceleration ramp, slower over a button;
// the game sees the stick at rest; another scheme hides it
TEST_F(InputGamepadCursorTest, CursorMovesAndConsumes){
    loadCursor("html, body { margin: 0; }\n#slow { position: absolute; left: 0; top: 500px; width: 1280px; height: 100px; }\n",
               "<button id=\"slow\">Slow</button>");
    ASSERT_TRUE(GamepadCursor::isActive());
    EXPECT_EQ(GamepadCursor::getPosition(), Vector2(640.0f, 360.0f)) << "the canvas centre: nothing focused";

    GamepadCursor::setPosition(Vector2(100.0f, 200.0f));
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 1.0f);
    // the ramp: each frame's step grows over the 0.15 s acceleration time, then stays
    std::vector<float> steps;
    float last = GamepadCursor::getPosition().x;
    for (int i = 0; i < 12; i++){
        frames(1);
        steps.push_back(GamepadCursor::getPosition().x - last);
        last = GamepadCursor::getPosition().x;
    }
    EXPECT_LT(steps[1], steps[5]);
    EXPECT_LT(steps[5], steps[9]);
    EXPECT_NEAR(steps[10], steps[11], 0.01f) << "full speed after the ramp";

    // one second at full stick
    float start = GamepadCursor::getPosition().x;
    frames(60);
    EXPECT_NEAR(GamepadCursor::getPosition().x - start, fullSpeed, fullSpeed * 0.02f);

    // half way: a quarter as fast
    GamepadCursor::setPosition(Vector2(100.0f, 200.0f));
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.5f);
    frames(12);
    start = GamepadCursor::getPosition().x;
    frames(60);
    EXPECT_NEAR(GamepadCursor::getPosition().x - start, fullSpeed * 0.25f, fullSpeed * 0.25f * 0.02f);

    // over the button: half the speed (Slow Over Controls 0.5)
    GamepadCursor::setPosition(Vector2(100.0f, 550.0f));
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 1.0f);
    frames(12);
    start = GamepadCursor::getPosition().x;
    frames(30);
    EXPECT_NEAR(GamepadCursor::getPosition().x - start, fullSpeed * 0.5f * 0.5f, fullSpeed * 0.25f * 0.02f);

    // the game's stick is at rest
    for (const auto& axis : gameAxes) {
        EXPECT_EQ(axis.second, 0.0f) << "axis " << axis.first;
    }
    EXPECT_EQ(Input::getGamepadAxisForGame(pad, D_GAMEPAD_AXIS_LEFT_X), 0.0f);
    EXPECT_EQ(Input::getGamepadAxis(pad, D_GAMEPAD_AXIS_LEFT_X), 1.0f) << "polling the device still reads it";

    // Keyboard&Mouse: hidden
    InputSchemeTracker::setOverride(InputScheme::KeyboardMouse);
    frames(1);
    EXPECT_FALSE(GamepadCursor::isActive());
}

// A clicks at the cursor through the documents' mouse pointer (:hover follows it), A held drags a
// range; none of it reaches Input, the game's mouse events or the scheme
TEST_F(InputGamepadCursorTest, CursorClicksHoversDrags){
    loadCursor("html, body { margin: 0; }\n"
               "#go { position: absolute; left: 100px; top: 100px; width: 200px; height: 60px; }\n"
               "#drag { position: absolute; left: 100px; top: 300px; width: 600px; height: 30px; }\n",
               "<button id=\"go\">Go</button>\n<div id=\"drag\"></div>");
    ClickCounter clicks;
    element("go")->AddEventListener(Rml::EventId::Click, &clicks);
    const Vector2 mouse = Input::getMousePosition();

    GamepadCursor::setPosition(Vector2(200.0f, 130.0f));
    EXPECT_TRUE(element("go")->IsPseudoClassSet("hover"));
    press(D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(clicks.clicks, 1);
    EXPECT_TRUE(gameDowns == std::vector<int>{D_GAMEPAD_BUTTON_X}) << "A is the cursor's";

    // A held over the drag pad, the stick moves right: pointer moves with the button held reach it
    HeldMoves held;
    Rml::Element* dragPad = element("drag");
    dragPad->AddEventListener(Rml::EventId::Pointermove, &held);
    GamepadCursor::setPosition(Vector2(110.0f, 315.0f));
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_FALSE(Input::getMouseButton(D_MOUSE_BUTTON_LEFT));
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 1.0f);
    frames(30);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_GT(held.moves, 10) << "a drag: moves with the left button held";
    EXPECT_GT(held.lastX, 200.0f);
    dragPad->RemoveEventListener(Rml::EventId::Pointermove, &held);

    EXPECT_EQ(Input::getMousePosition(), mouse) << "the cursor is not the mouse";
    EXPECT_FALSE(Input::getMouseButton(D_MOUSE_BUTTON_LEFT));
    EXPECT_EQ(gameMouseDowns, 0);
    frames(1);
    EXPECT_EQ(InputSchemeTracker::getCurrent(), InputScheme::Gamepad);
    element("go")->RemoveEventListener(Rml::EventId::Click, &clicks);
}

// the right stick scrolls at the cursor at Scroll Speed lines/s; the d-pad moves the focus and the
// cursor jumps to it; a focus set by a script moves it too
TEST_F(InputGamepadCursorTest, CursorScrollsAndJumpsToFocus){
    loadCursor("html, body { margin: 0; }\n"
               "#list { position: absolute; left: 0; top: 0; width: 400px; height: 300px; overflow-y: auto; }\n"
               "#list div { height: 4000px; }\n"
               "#b1, #b2 { position: absolute; left: 600px; width: 200px; height: 40px; }\n"
               "#b1 { top: 100px; } #b2 { top: 300px; }\n",
               "<div id=\"list\"><div></div></div>\n<button id=\"b1\">1</button><button id=\"b2\">2</button>");
    Rml::Element* list = element("list");
    GamepadCursor::setPosition(Vector2(200.0f, 150.0f));
    // one wheel line down at the cursor (the documents' mouse pointer), as the reference
    ASSERT_TRUE(system().eventOnWheel(0.0f, -1.0f, 0));
    frames(30);
    const float lineHeight = list->GetScrollTop();
    ASSERT_GT(lineHeight, 0.0f);
    list->SetScrollTop(0.0f);
    frames(1);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_Y, 1.0f);
    frames(60);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_RIGHT_Y, 0.0f);
    frames(30);
    EXPECT_NEAR(list->GetScrollTop(), 20.0f * lineHeight, 20.0f * lineHeight * 0.05f) << "Scroll Speed 20 lines/s for 1 s";

    // the d-pad: from the element under the cursor (nothing focused) to the next one
    GamepadCursor::setPosition(Vector2(700.0f, 120.0f));
    press(D_GAMEPAD_BUTTON_DPAD_DOWN);
    EXPECT_EQ(focusId(), "b2");
    EXPECT_EQ(GamepadCursor::getPosition(), Vector2(700.0f, 320.0f)) << "the centre of the new focus";
    EXPECT_TRUE(element("b2")->IsPseudoClassSet("focus-visible"));

    doc().getElementById("b1").focus();
    frames(1);
    EXPECT_EQ(GamepadCursor::getPosition(), Vector2(700.0f, 120.0f));
}

// a press the cursor holds ends without a click when it goes: the mouse moved (Keyboard&Mouse) or
// the document hidden
TEST_F(InputGamepadCursorTest, CursorCancelsOnDeactivate){
    loadCursor("html, body { margin: 0; }\n#go { position: absolute; left: 100px; top: 100px; width: 200px; height: 60px; }\n",
               "<button id=\"go\">Go</button>");
    ClickCounter clicks, cancels;
    element("go")->AddEventListener(Rml::EventId::Click, &clicks);
    element("go")->AddEventListener(Rml::EventId::Pointercancel, &cancels);

    GamepadCursor::setPosition(Vector2(200.0f, 130.0f));
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    Engine::systemMouseMove(150.0f, 130.0f, 0);
    Engine::systemMouseMove(170.0f, 130.0f, 0);
    frames(2);
    EXPECT_NE(InputSchemeTracker::getCurrent(), InputScheme::Gamepad);
    EXPECT_FALSE(GamepadCursor::isActive());
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(cancels.clicks, 1);
    EXPECT_EQ(clicks.clicks, 0);

    // back on the pad, then the document goes while A is held
    usePad();
    frames(1);
    ASSERT_TRUE(GamepadCursor::isActive());
    GamepadCursor::setPosition(Vector2(200.0f, 130.0f));
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    doc().hide();
    frames(2);
    EXPECT_FALSE(GamepadCursor::isActive());
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_FALSE(element("go")->IsPseudoClassSet("active")) << "the press ended";
    EXPECT_EQ(clicks.clicks, 0);
    element("go")->RemoveEventListener(Rml::EventId::Click, &clicks);
    element("go")->RemoveEventListener(Rml::EventId::Pointercancel, &cancels);
}

// off (no Gamepad Cursor document) with a pad moving: the overlay never loads, the stick is the game's
TEST_F(InputGamepadCursorTest, CursorZeroCostWhenOff){
    load(kGridCss, kGridBody);
    usePad();
    const uint32_t loads = UIDocumentSystem::getCursorOverlayLoads();
    for (int i = 0; i < 60; i++){
        Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, (i % 2) ? 0.8f : -0.8f);
        frames(1);
    }
    EXPECT_FALSE(GamepadCursor::isActive());
    EXPECT_EQ(UIDocumentSystem::getCursorOverlayLoads(), loads);
    EXPECT_FALSE(gameAxes.empty()) << "nothing took the stick";

    // the cursor's share of a frame, 600 frames with the pad's stick moving: no allocation (R26)
    constexpr int offFrames = 600;
    testsupport::ThreadAllocationCount allocations;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < offFrames; i++){
        Input::setGamepadAxisValue(pad, D_GAMEPAD_AXIS_LEFT_X, (i % 2) ? 0.8f : -0.8f);
        GamepadCursor::update(cursorFrame);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / offFrames;
    std::printf("[bench] cursor_off_frame_ns %.1f, %llu allocations in %d frames (budget: one cached boolean, 0 allocations)\n", ns,
                (unsigned long long)allocations.count(), offFrames);
    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_FALSE(GamepadCursor::isActive());
}

// R26: active and moving, the cursor's frame (one hit test, one pointer move, the overlay) within
// 20 us on a desktop; printed here, enforced with JASPER_TEST_PERF_SLACK (the benchmark run)
TEST_F(InputGamepadCursorTest, CursorFrameBench){
    loadCursor("html, body { margin: 0; }\n"
               ".row { display: flex; flex-wrap: wrap; gap: 4px; }\nbutton { width: 60px; height: 30px; }\n",
               std::string("<div class=\"row\">") +
               "<button>1</button><button>2</button><button>3</button><button>4</button><button>5</button><button>6</button>"
               "<button>7</button><button>8</button><button>9</button><button>10</button><button>11</button><button>12</button></div>");
    ASSERT_TRUE(GamepadCursor::isActive());
    GamepadCursor::setPosition(Vector2(10.0f, 10.0f));
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.6f);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, 0.1f);
    frames(12);   // past the acceleration ramp

    constexpr int benchFrames = 300;
    double total = 0.0;
    uint64_t cursorAllocations = 0;
    std::vector<Vector2> path;
    path.reserve(benchFrames);
    for (int i = 0; i < benchFrames; i++){
        // back and forth across the buttons: a hover change most frames
        if (GamepadCursor::getPosition().x > 700.0f)
            GamepadCursor::setPosition(Vector2(10.0f, 10.0f));
        const testsupport::ThreadAllocationCount allocations;
        const auto start = std::chrono::steady_clock::now();
        GamepadCursor::update(cursorFrame);
        total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        cursorAllocations += allocations.count();
        path.push_back(GamepadCursor::getPosition());
    }
    const double us = total / benchFrames;

    // the documents' own mouse move along the same path (RmlUi's hover and event dispatch): what
    // the cursor's frame may allocate, since the cursor adds nothing of its own
    uint64_t documentAllocations = 0;
    for (const Vector2& point : path){
        const testsupport::ThreadAllocationCount allocations;
        system().eventOnPointerMove(0, point.x, point.y, 0, false, false);
        documentAllocations += allocations.count();
    }

    // still (the stick at rest): no allocation at all
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    Engine::systemGamepadAxisMove(pad, D_GAMEPAD_AXIS_LEFT_Y, 0.0f);
    GamepadCursor::update(cursorFrame);
    const testsupport::ThreadAllocationCount still;
    for (int i = 0; i < benchFrames; i++)
        GamepadCursor::update(cursorFrame);

    std::printf("[bench] cursor_frame_us %.2f (budget 20 us desktop, 60 us mobile), %.1f allocations per moving frame "
                "(the documents' own mouse move: %.1f), %llu in %d still frames\n", us, double(cursorAllocations) / benchFrames,
                double(documentAllocations) / benchFrames, (unsigned long long)still.count(), benchFrames);
    if (testsupport::countsAllocations()){
        EXPECT_EQ(still.count(), 0u) << "an active cursor that does not move allocates nothing";
        EXPECT_LE(cursorAllocations, documentAllocations) << "a moving cursor allocates no more than the documents' pointer move";
    }
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(us, 20.0 * testsupport::perfSlack());
    }
    EXPECT_TRUE(GamepadCursor::isActive());
}

// AC13: the cursor clicks a component UI Button and drags a component On-Screen Stick with A held
TEST_F(InputGamepadCursorTest, GamepadCursorPressesComponentButton){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    // the document has nothing over the component UI: its pointer passes through to it
    loadCursor("html, body { margin: 0; }\n#corner { position: absolute; left: 1100px; top: 600px; width: 100px; height: 60px; }\n",
               "<button id=\"corner\">Corner</button>");
    Button& button = fixture->add<Button>();
    button.setPosition(700, 100);
    button.setSize(200, 80);
    int clicks = 0;
    button.getComponent<ButtonComponent>().onClick.add(cursorTag, std::function<void()>([&clicks](){ clicks++; }));
    OnScreenStick& stick = fixture->add<OnScreenStick>();
    stick.setPosition(100, 300);
    stick.setSize(200, 200);
    stick.getStick().mode = OnScreenStickMode::FIXED;
    stick.getStick().background = stick.getEntity();   // its own rect: centre (200, 400), radius 100
    frames(2);

    GamepadCursor::setPosition(Vector2(800.0f, 140.0f));
    press(D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(clicks, 1) << "A clicked the component Button once";
    EXPECT_EQ(gameMouseDowns, 0);

    // A held on the stick's centre, the cursor moves right: the stick follows, then lets go
    GamepadCursor::setPosition(Vector2(200.0f, 400.0f));
    Engine::systemGamepadButtonDown(pad, D_GAMEPAD_BUTTON_A);
    EXPECT_TRUE(stick.getStick().pressed);
    GamepadCursor::setPosition(Vector2(250.0f, 400.0f));
    frames(1);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.05f) << "half a radius right";
    Engine::systemGamepadButtonUp(pad, D_GAMEPAD_BUTTON_A);
    frames(1);
    EXPECT_FALSE(stick.getStick().pressed);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 1e-4f);
    button.getComponent<ButtonComponent>().onClick.remove(cursorTag);
    InputActions::resetRuntimeState();
}

// the cursor graphic: an overlay above every document, its hot spot on the point, never hit
TEST_F(InputGamepadCursorTest, CursorDrawnOnTop){
    loadCursor("html, body { margin: 0; }\n#go { position: absolute; left: 100px; top: 100px; width: 200px; height: 60px; }\n",
               "<button id=\"go\">Go</button>");
    GamepadCursor::setPosition(Vector2(200.0f, 130.0f));
    frames(1);
    Rml::Context* rml = context();
    Rml::ElementDocument* overlay = nullptr;
    for (int i = 0; i < rml->GetNumDocuments(); i++){
        if (rml->GetDocument(i)->GetSourceURL() == "[gamepad-cursor]")
            overlay = rml->GetDocument(i);
    }
    ASSERT_NE(overlay, nullptr);
    EXPECT_TRUE(overlay->IsVisible());
    Rml::Element* image = overlay->GetElementById("cursor");
    ASSERT_NE(image, nullptr);
    const Rml::Vector2f at = image->GetAbsoluteOffset(Rml::BoxArea::Border);
    EXPECT_NEAR(at.x, 200.0f, 0.5f);
    EXPECT_NEAR(at.y, 130.0f, 0.5f);
    EXPECT_NEAR(image->GetBox().GetSize(Rml::BoxArea::Border).x, 32.0f, 0.5f);
    // above the documents, and the button under it still hovers
    EXPECT_TRUE(element("go")->IsPseudoClassSet("hover"));

    // a hot spot of (16, 16): the graphic's centre on the point
    GamepadCursorSettings settings;
    settings.hotSpot = Vector2(16.0f, 16.0f);
    GamepadCursor::setDefaults(settings);
    GamepadCursor::setPosition(Vector2(200.0f, 130.0f));
    frames(1);
    EXPECT_NEAR(image->GetAbsoluteOffset(Rml::BoxArea::Border).x, 184.0f, 0.5f);
    GamepadCursor::setDefaults(GamepadCursorSettings());

    InputSchemeTracker::setOverride(InputScheme::KeyboardMouse);
    frames(1);
    EXPECT_FALSE(overlay->IsVisible());
}
