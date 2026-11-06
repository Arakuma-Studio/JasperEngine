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
#include "object/ui/UIEvent.h"

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
    ClickCounter clicks;
    element("b1")->AddEventListener(Rml::EventId::Click, &clicks);
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

    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    EXPECT_EQ(clicks.clicks, 0) << "the upper document's button does not take the typing keys";

    Engine::removeScene(&fixture->getScene());
    lower.reset();
    Engine::setScene(&fixture->getScene());
}
