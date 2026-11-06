// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-1 input (spec §3.6, p1-input): RmlKeyMap, surrogate combining, the consume rules for
// pointers, wheel, keys and text, control characters, multi-touch, mouse leave, IME
// composition, the virtual keyboard and caret area, the clipboard and the mouse cursor.

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "Input.h"
#include "RmlKeyMap.h"
#include "RmlSystem.h"
#include "RmlTextInputHandler.h"
#include "util/Utf16Combiner.h"
#include "input/InputActions.h"
#include "component/ButtonComponent.h"
#include "component/UIComponent.h"
#include "component/OnScreenControlComponents.h"
#include "object/ui/Button.h"
#include "object/ui/InputField.h"
#include "object/ui/OnScreenControls.h"
#include "subsystem/UISystem.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <functional>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "htmlui-tests-input-p1";

    // fixtures/p1/input/input.html, document px = canvas px (640x360)
    const float buttonX = 100, buttonY = 60;
    const float nameX = 60, nameY = 135;
    const float otherX = 60, otherY = 185;
    const float scrollerX = 350, scrollerY = 90;
    const float touchAX = 340, touchAY = 220;
    const float touchBX = 440, touchBY = 220;
    const float overlayX = 550, overlayY = 90;
    const float helpX = 70, helpY = 315;
    const float bodyX = 600, bodyY = 330;

    // every D_KEY_* of Input.h
    const int allKeys[] = {
        D_KEY_SPACE, D_KEY_APOSTROPHE, D_KEY_COMMA, D_KEY_MINUS, D_KEY_PERIOD, D_KEY_SLASH,
        D_KEY_0, D_KEY_1, D_KEY_2, D_KEY_3, D_KEY_4, D_KEY_5, D_KEY_6, D_KEY_7, D_KEY_8, D_KEY_9,
        D_KEY_SEMICOLON, D_KEY_EQUAL,
        D_KEY_A, D_KEY_B, D_KEY_C, D_KEY_D, D_KEY_E, D_KEY_F, D_KEY_G, D_KEY_H, D_KEY_I, D_KEY_J,
        D_KEY_K, D_KEY_L, D_KEY_M, D_KEY_N, D_KEY_O, D_KEY_P, D_KEY_Q, D_KEY_R, D_KEY_S, D_KEY_T,
        D_KEY_U, D_KEY_V, D_KEY_W, D_KEY_X, D_KEY_Y, D_KEY_Z,
        D_KEY_LEFT_BRACKET, D_KEY_BACKSLASH, D_KEY_RIGHT_BRACKET, D_KEY_GRAVE_ACCENT,
        D_KEY_WORLD_1, D_KEY_WORLD_2,
        D_KEY_ESCAPE, D_KEY_ENTER, D_KEY_TAB, D_KEY_BACKSPACE, D_KEY_INSERT, D_KEY_DELETE,
        D_KEY_RIGHT, D_KEY_LEFT, D_KEY_DOWN, D_KEY_UP, D_KEY_PAGE_UP, D_KEY_PAGE_DOWN,
        D_KEY_HOME, D_KEY_END, D_KEY_CAPS_LOCK, D_KEY_SCROLL_LOCK, D_KEY_NUM_LOCK,
        D_KEY_PRINT_SCREEN, D_KEY_PAUSE,
        D_KEY_F1, D_KEY_F2, D_KEY_F3, D_KEY_F4, D_KEY_F5, D_KEY_F6, D_KEY_F7, D_KEY_F8, D_KEY_F9,
        D_KEY_F10, D_KEY_F11, D_KEY_F12, D_KEY_F13, D_KEY_F14, D_KEY_F15, D_KEY_F16, D_KEY_F17,
        D_KEY_F18, D_KEY_F19, D_KEY_F20, D_KEY_F21, D_KEY_F22, D_KEY_F23, D_KEY_F24, D_KEY_F25,
        D_KEY_KP_0, D_KEY_KP_1, D_KEY_KP_2, D_KEY_KP_3, D_KEY_KP_4, D_KEY_KP_5, D_KEY_KP_6,
        D_KEY_KP_7, D_KEY_KP_8, D_KEY_KP_9, D_KEY_KP_DECIMAL, D_KEY_KP_DIVIDE, D_KEY_KP_MULTIPLY,
        D_KEY_KP_SUBTRACT, D_KEY_KP_ADD, D_KEY_KP_ENTER, D_KEY_KP_EQUAL,
        D_KEY_LEFT_SHIFT, D_KEY_LEFT_CONTROL, D_KEY_LEFT_ALT, D_KEY_LEFT_SUPER,
        D_KEY_RIGHT_SHIFT, D_KEY_RIGHT_CONTROL, D_KEY_RIGHT_ALT, D_KEY_RIGHT_SUPER, D_KEY_MENU,
    };

    class StopPropagationListener : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            event.StopPropagation();
        }
    };

    class InputTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            fixture.reset(new SceneFixture());
            UIDocument& ui = fixture->add<UIDocument>();
            uiDocument = &ui;
            ASSERT_TRUE(ui.load("p1/input/input.rml"));
            document = rmlContext(scene())->GetDocument(0);
            ASSERT_NE(document, nullptr);
            // scrolls land at once, without the smooth-scroll animation
            rmlContext(scene())->SetDefaultScrollBehavior(Rml::ScrollBehavior::Instant, 1.0f);

            Engine::onMouseDown.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ mouseDowns++; }));
            Engine::onMouseScroll.add(callbackTag, std::function<void(float, float, int)>([this](float, float, int){ scrolls++; }));
            Engine::onKeyDown.add(callbackTag, std::function<void(int, bool, int)>([this](int, bool, int){ keyDowns++; }));
            Engine::onKeyUp.add(callbackTag, std::function<void(int, bool, int)>([this](int, bool, int){ keyUps++; }));
            Engine::onCharInput.add(callbackTag, std::function<void(char32_t)>([this](char32_t c){ chars.push_back((uint32_t)c); }));
            Engine::onTouchStart.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ touchStarts++; }));
            Engine::onTouchEnd.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ touchEnds++; }));
            Engine::onTouchCancel.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ touchCancels++; }));
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            Engine::onMouseDown.remove(callbackTag);
            Engine::onMouseScroll.remove(callbackTag);
            Engine::onKeyDown.remove(callbackTag);
            Engine::onKeyUp.remove(callbackTag);
            Engine::onCharInput.remove(callbackTag);
            Engine::onTouchStart.remove(callbackTag);
            Engine::onTouchEnd.remove(callbackTag);
            Engine::onTouchCancel.remove(callbackTag);
            Engine::systemTouchCancel();
            Engine::setIgnoreEventsHandledByUI(true);
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
        }

        Scene& scene(){
            return fixture->getScene();
        }

        Rml::Element* element(const char* id){
            return document->GetElementById(id);
        }

        std::string value(const char* id){
            return element(id)->GetAttribute<Rml::String>("value", "");
        }

        void click(float x, float y, int mods = 0){
            Engine::systemMouseMove(x, y, mods);
            Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, x, y, mods);
            Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, x, y, mods);
        }

        void key(int key, int mods = 0){
            Engine::systemKeyDown(key, false, mods);
            Engine::systemKeyUp(key, false, mods);
        }

        std::unique_ptr<SceneFixture> fixture;
        UIDocument* uiDocument = nullptr;
        Rml::ElementDocument* document = nullptr;
        int mouseDowns = 0, scrolls = 0, keyDowns = 0, keyUps = 0, touchStarts = 0, touchEnds = 0, touchCancels = 0;
        std::vector<uint32_t> chars;
    };

}

// ---- RmlKeyMap ----

TEST(InputKeyMap, CoversEveryKey){
    for (int key : allKeys){
        const bool unmapped = key == D_KEY_WORLD_1 || key == D_KEY_WORLD_2 || key == D_KEY_F25;
        const bool unidentified = key == D_KEY_WORLD_1 || key == D_KEY_WORLD_2;   // layout-dependent
        const Rml::Input::KeyIdentifier rmlKey = RmlKeyMap::toRmlKey(key);
        if (unmapped){
            EXPECT_EQ(rmlKey, Rml::Input::KI_UNKNOWN) << key;
        }else{
            EXPECT_NE(rmlKey, Rml::Input::KI_UNKNOWN) << key;
            EXPECT_EQ(RmlKeyMap::fromRmlKey(rmlKey), key) << key;
        }
        EXPECT_FALSE(RmlKeyMap::domCode(key).empty()) << key;
        EXPECT_EQ(RmlKeyMap::domKey(key, 0) == "Unidentified", unidentified) << key;
    }
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_UNKNOWN), Rml::Input::KI_UNKNOWN);
    EXPECT_EQ(RmlKeyMap::toRmlKey(12345), Rml::Input::KI_UNKNOWN);
    EXPECT_EQ(RmlKeyMap::fromRmlKey(Rml::Input::KI_UNKNOWN), D_KEY_UNKNOWN);
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_UNKNOWN), "");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_UNKNOWN, 0), "Unidentified");

    // a few exact rows
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_A), Rml::Input::KI_A);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_SEMICOLON), Rml::Input::KI_OEM_1);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_KP_0), Rml::Input::KI_NUMPAD0);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_KP_ENTER), Rml::Input::KI_NUMPADENTER);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_PRINT_SCREEN), Rml::Input::KI_SNAPSHOT);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_MENU), Rml::Input::KI_APPS);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_LEFT_ALT), Rml::Input::KI_LMENU);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_RIGHT_SUPER), Rml::Input::KI_RMETA);
    EXPECT_EQ(RmlKeyMap::toRmlKey(D_KEY_F24), Rml::Input::KI_F24);
}

TEST(InputKeyMap, Modifiers){
    const bool apple = RmlKeyMap::isAppleModifiers();

    RmlKeyMap::setAppleModifiers(false);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_SHIFT), Rml::Input::KM_SHIFT);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_CONTROL), Rml::Input::KM_CTRL);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_ALT), Rml::Input::KM_ALT);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_SUPER), Rml::Input::KM_META);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_CAPS_LOCK), Rml::Input::KM_CAPSLOCK);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_NUM_LOCK), Rml::Input::KM_NUMLOCK);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(0), 0);
    const int all = D_MODIFIER_SHIFT | D_MODIFIER_CONTROL | D_MODIFIER_ALT | D_MODIFIER_SUPER | D_MODIFIER_CAPS_LOCK | D_MODIFIER_NUM_LOCK;
    EXPECT_EQ(RmlKeyMap::fromRmlModifiers(RmlKeyMap::toRmlModifiers(all)), all);

    // Apple: Cmd is also Ctrl for RmlUi's text widget (copy/cut/paste/select all)
    RmlKeyMap::setAppleModifiers(true);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_SUPER), Rml::Input::KM_META | Rml::Input::KM_CTRL);
    EXPECT_EQ(RmlKeyMap::toRmlModifiers(D_MODIFIER_SHIFT), Rml::Input::KM_SHIFT);

    RmlKeyMap::setAppleModifiers(apple);
#if defined(__APPLE__)
    EXPECT_TRUE(RmlKeyMap::isAppleModifiers());
#else
    EXPECT_FALSE(RmlKeyMap::isAppleModifiers());
#endif
}

TEST(InputKeyMap, DomKeyAndCode){
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_A), "KeyA");
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_1), "Digit1");
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_SPACE), "Space");
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_LEFT_SHIFT), "ShiftLeft");
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_KP_5), "Numpad5");
    EXPECT_EQ(RmlKeyMap::domCode(D_KEY_LEFT), "ArrowLeft");

    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_A, 0), "a");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_A, D_MODIFIER_SHIFT), "A");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_A, D_MODIFIER_CAPS_LOCK), "A");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_A, D_MODIFIER_CAPS_LOCK | D_MODIFIER_SHIFT), "a");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_1, D_MODIFIER_SHIFT), "!");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_1, D_MODIFIER_CAPS_LOCK), "1");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_SLASH, D_MODIFIER_SHIFT), "?");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_SPACE, 0), " ");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_ENTER, D_MODIFIER_SHIFT), "Enter");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_ESCAPE, 0), "Escape");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_BACKSPACE, 0), "Backspace");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_TAB, 0), "Tab");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_RIGHT_SHIFT, 0), "Shift");
    EXPECT_EQ(RmlKeyMap::domKey(D_KEY_F5, 0), "F5");
}

// ---- surrogates ----

TEST(InputText, Utf16Combiner){
    Utf16Combiner combiner;
    std::vector<uint32_t> out;

    // a valid pair: U+1F600
    combiner.push(0xD83D, out);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(combiner.hasPending());
    combiner.push(0xDE00, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], 0x1F600u);

    // a lone high followed by an ASCII character
    out.clear();
    combiner.push(0xD83D, out);
    combiner.push('a', out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 0xFFFDu);
    EXPECT_EQ(out[1], (uint32_t)'a');

    // a lone low
    out.clear();
    combiner.push(0xDE00, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], 0xFFFDu);

    // two highs: the first is lost
    out.clear();
    combiner.push(0xD83D, out);
    combiner.push(0xD83D, out);
    combiner.push(0xDE00, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 0xFFFDu);
    EXPECT_EQ(out[1], 0x1F600u);

    // a high at the end of the input
    out.clear();
    combiner.push(0xD83D, out);
    combiner.flush(out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], 0xFFFDu);
    EXPECT_FALSE(combiner.hasPending());

    // whole code points pass
    out.clear();
    combiner.push(0x1F600, out);
    combiner.push(0xE9, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 0x1F600u);
    EXPECT_EQ(out[1], 0xE9u);
}

TEST_F(InputTest, SurrogatePairsReachDocumentsCombined){
    click(nameX, nameY);
    ASSERT_EQ(rmlContext(scene())->GetFocusElement(), element("name"));

    // one UTF-16 unit at a time, as Windows WM_CHAR and ImGui deliver them
    Engine::systemCharInput((wchar_t)'a');
    Engine::systemCharInput((wchar_t)0xD83D);
    Engine::systemCharInput((wchar_t)0xDE00);
    Engine::systemCharInput((wchar_t)0xDE00);   // a lone low
    EXPECT_EQ(value("name"), "a\xF0\x9F\x98\x80\xEF\xBF\xBD");

    // a high whose low never comes: U+FFFD at the next frame
    Engine::systemCharInput((wchar_t)0xD83D);
    EXPECT_EQ(value("name"), "a\xF0\x9F\x98\x80\xEF\xBF\xBD");
    Engine::systemDraw();
    EXPECT_EQ(value("name"), "a\xF0\x9F\x98\x80\xEF\xBF\xBD\xEF\xBF\xBD");

    // the field had focus: the game saw none of it
    EXPECT_TRUE(chars.empty());
}

// The game's character event gets whole code points on every platform: a pair typed one unit
// at a time (Windows WM_CHAR, Android) is one call, half of one is no call
TEST_F(InputTest, OnCharInputGetsWholeCodePoints){
    Engine::systemCharInput((wchar_t)0xD83D);
    EXPECT_TRUE(chars.empty()) << "half a character goes nowhere yet";
    Engine::systemCharInput((wchar_t)0xDE00);
    EXPECT_EQ(chars, (std::vector<uint32_t>{0x1F600}));
    // a whole code point passes as it is
    Engine::systemCharInput(U'\U0001F601');
    Engine::systemCharInput(L'a');
    EXPECT_EQ(chars, (std::vector<uint32_t>{0x1F600, 0x1F601, 'a'}));
}

namespace {

    // a component InputField where the document has nothing (x 500..620, y 180..300), focused
    InputField& focusedInputField(SceneFixture& fixture){
        InputField& field = fixture.add<InputField>();
        // its Text child, where the typing goes
        field.setText("");
        field.setPosition(500, 200);
        field.setSize(120, 40);
        GpuEnvironment::drawFrames(2);
        Engine::systemMouseMove(560, 220, 0);
        Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 560, 220, 0);
        Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 560, 220, 0);
        return field;
    }

}

// The component InputField gets the pair as one character, keeps it through later edits and
// erases it with one backspace
TEST_F(InputTest, ComponentInputFieldKeepsSurrogatePairs){
    InputField& field = focusedInputField(*fixture);
    ASSERT_TRUE(field.getComponent<UIComponent>().focused);

    Engine::systemCharInput(L'a');
    Engine::systemCharInput((wchar_t)0xD83D);
    Engine::systemCharInput((wchar_t)0xDE00);
    Engine::systemCharInput(L'b');
    EXPECT_EQ(field.getText(), "a\xF0\x9F\x98\x80" "b");
    EXPECT_EQ(field.getCursorIndex(), 3);

    Engine::systemCharInput(L'\b');
    EXPECT_EQ(field.getText(), "a\xF0\x9F\x98\x80");
    Engine::systemCharInput(L'\b');
    EXPECT_EQ(field.getText(), "a");
}

// A high surrogate whose low one never comes is U+FFFD, for the InputField and the game alike
TEST_F(InputTest, LoneHighSurrogateFlushesEverywhere){
    InputField& field = focusedInputField(*fixture);
    ASSERT_TRUE(field.getComponent<UIComponent>().focused);
    chars.clear();

    Engine::systemCharInput((wchar_t)0xD83D);
    EXPECT_EQ(field.getText(), "");
    Engine::systemDraw();
    EXPECT_EQ(field.getText(), "\xEF\xBF\xBD");
    EXPECT_EQ(chars, (std::vector<uint32_t>{0xFFFD}));
}

// ---- pointer consume rules ----

TEST_F(InputTest, PointerConsumeRules){
    // A button takes pointer state and input; all mouse buttons obey the same rule.
    Engine::systemMouseMove(buttonX, buttonY, 0);
    EXPECT_TRUE(element("button")->IsPseudoClassSet("hover"));
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, buttonX, buttonY, 0);
    EXPECT_TRUE(element("button")->IsPseudoClassSet("active"));
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, buttonX, buttonY, 0);
    EXPECT_FALSE(element("button")->IsPseudoClassSet("active"));
    Engine::systemMouseDown(D_MOUSE_BUTTON_RIGHT, buttonX, buttonY, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_RIGHT, buttonX, buttonY, 0);
    EXPECT_EQ(mouseDowns, 0);

    // the transparent body is the game's
    click(bodyX, bodyY);
    EXPECT_EQ(mouseDowns, 1);

    // a pointer-events: none element lets the click through
    click(overlayX, overlayY);
    EXPECT_EQ(mouseDowns, 2);

    // ignoreEventsHandledByUI off: the game sees consumed clicks too
    Engine::setIgnoreEventsHandledByUI(false);
    click(buttonX, buttonY);
    EXPECT_EQ(mouseDowns, 3);
    Engine::setIgnoreEventsHandledByUI(true);

    uiDocument->hide();
    updateDocuments(scene());
    click(buttonX, buttonY);
    EXPECT_EQ(mouseDowns, 4) << "a hidden document takes no input";
}

TEST_F(InputTest, OpaqueDocumentBlocksLikeACanvas){
    UIDocument& opaque = fixture->add<UIDocument>();
    ASSERT_TRUE(opaque.load("p1/input/opaque.rml"));
    opaque.setSortOrder(10);
    updateDocuments(scene());

    // the body paints a background: the pointer is the document's everywhere
    click(bodyX, bodyY);
    EXPECT_EQ(mouseDowns, 0);

    opaque.hide();
    updateDocuments(scene());
    click(bodyX, bodyY);
    EXPECT_EQ(mouseDowns, 1);
}

// ---- keys ----

TEST_F(InputTest, FocusedTextFieldTakesKeys){
    // nothing focused: keys are the game's
    key(D_KEY_A);
    EXPECT_EQ(keyDowns, 1);
    EXPECT_EQ(keyUps, 1);

    click(nameX, nameY);
    ASSERT_EQ(rmlContext(scene())->GetFocusElement(), element("name"));
    EXPECT_TRUE(Engine::wantsTextComposition());

    // letters are the field's, their release too
    key(D_KEY_A);
    EXPECT_EQ(keyDowns, 1);
    EXPECT_EQ(keyUps, 1);

    // Escape and the function keys are not
    key(D_KEY_F5);
    EXPECT_EQ(keyDowns, 2);
    EXPECT_EQ(keyUps, 2);
    key(D_KEY_ESCAPE);
    EXPECT_EQ(keyDowns, 3);
    EXPECT_EQ(keyUps, 3);

    // a release whose press the field took stays the field's after the focus moved
    Engine::systemKeyDown(D_KEY_B, false, 0);
    click(bodyX, bodyY);
    EXPECT_FALSE(Engine::wantsTextComposition());
    Engine::systemKeyUp(D_KEY_B, false, 0);
    EXPECT_EQ(keyDowns, 3);
    EXPECT_EQ(keyUps, 3);
}

TEST_F(InputTest, StoppedKeyDownIsConsumed){
    Rml::Element* button = element("focusbtn");
    ASSERT_TRUE(button->Focus());

    // a focused element that lets the key propagate: the game's
    key(D_KEY_A);
    EXPECT_EQ(keyDowns, 1);

    StopPropagationListener listener;
    button->AddEventListener(Rml::EventId::Keydown, &listener);
    key(D_KEY_A);
    EXPECT_EQ(keyDowns, 1);
    EXPECT_EQ(keyUps, 1);   // the release of a consumed press is consumed
    button->RemoveEventListener(Rml::EventId::Keydown, &listener);
}

// ---- text ----

TEST_F(InputTest, TextGoesToTheFocusedField){
    // no field: the game gets each code point
    Engine::systemTextInput("h\xC3\xA9");
    ASSERT_EQ(chars.size(), 2u);
    EXPECT_EQ(chars[1], 0xE9u);
    chars.clear();

    click(nameX, nameY);
    Engine::systemTextInput("h\xC3\xA9llo \xF0\x9F\x98\x80");
    EXPECT_EQ(value("name"), "h\xC3\xA9llo \xF0\x9F\x98\x80");
    EXPECT_TRUE(chars.empty());

    // a textarea is a text field too
    click(60, 250);
    ASSERT_EQ(rmlContext(scene())->GetFocusElement(), element("area"));
    Engine::systemTextInput("x");
    EXPECT_EQ(value("area"), "x");
}

TEST_F(InputTest, ControlCharacters){
    click(nameX, nameY);
    Engine::systemTextInput("abcd");
    ASSERT_EQ(value("name"), "abcd");

    // the key down and its character in one frame (desktop): one deletion
    Engine::systemKeyDown(D_KEY_BACKSPACE, false, 0);
    Engine::systemCharInput(L'\b');
    Engine::systemKeyUp(D_KEY_BACKSPACE, false, 0);
    updateDocuments(scene());
    EXPECT_EQ(value("name"), "abc");

    // the character first, then the key (the editor's order): one deletion
    Engine::systemCharInput(L'\b');
    Engine::systemKeyDown(D_KEY_BACKSPACE, false, 0);
    Engine::systemKeyUp(D_KEY_BACKSPACE, false, 0);
    updateDocuments(scene());
    EXPECT_EQ(value("name"), "ab");

    // the character alone (Android's soft backspace): one deletion, at the next update
    Engine::systemCharInput(L'\b');
    EXPECT_EQ(value("name"), "ab");
    updateDocuments(scene());
    EXPECT_EQ(value("name"), "a");

    // control characters are never inserted
    Engine::systemTextInput("\t\x1b" "b");
    updateDocuments(scene());
    EXPECT_EQ(value("name"), "ab");
}

// ---- wheel ----

TEST_F(InputTest, WheelScrollsAnOverflowBox){
    Rml::Element* scroller = element("scroller");
    Engine::systemMouseMove(scrollerX, scrollerY, 0);
    EXPECT_EQ(scroller->GetScrollTop(), 0.0f);

    // y < 0 is a downward wheel (GLFW)
    Engine::systemMouseScroll(0, -1, 0);
    updateDocuments(scene());
    EXPECT_GT(scroller->GetScrollTop(), 0.0f);
    EXPECT_EQ(scrolls, 0);

    // over the transparent body: the game's
    Engine::systemMouseMove(bodyX, bodyY, 0);
    Engine::systemMouseScroll(0, -1, 0);
    EXPECT_EQ(scrolls, 1);
}

// ---- touch ----

TEST_F(InputTest, MultiTouch){
    Rml::Element* a = element("touchA");
    Rml::Element* b = element("touchB");

    Engine::systemTouchStart(0, touchAX, touchAY);
    Engine::systemTouchStart(7, touchBX, touchBY);
    EXPECT_TRUE(a->IsPseudoClassSet("active"));
    EXPECT_TRUE(b->IsPseudoClassSet("active"));
    EXPECT_EQ(touchStarts, 0);

    // a finger on the body is the game's
    Engine::systemTouchStart(9, bodyX, bodyY);
    EXPECT_EQ(touchStarts, 1);
    Engine::systemTouchEnd(9, bodyX, bodyY);
    EXPECT_EQ(touchEnds, 1);

    Engine::systemTouchStart(9, bodyX, bodyY);
    // Cancel clears both, without a click, and the game hears only its own finger.
    Engine::systemTouchCancel();
    EXPECT_FALSE(a->IsPseudoClassSet("active"));
    EXPECT_FALSE(b->IsPseudoClassSet("active"));
    EXPECT_EQ(touchCancels, 1);

    // a tap ends on its own
    Engine::systemTouchStart(4, touchAX, touchAY);
    Engine::systemTouchEnd(4, touchAX, touchAY);
    EXPECT_FALSE(a->IsPseudoClassSet("active"));
    EXPECT_EQ(touchEnds, 1) << "a document-owned release does not reach the game";
}

// ---- ui-multitouch: every finger its own pointer ----

namespace {
    // "<type>:<pointer_id>:<pointer_type>" of the events it hears
    class PointerRecorder : public Rml::EventListener {
    public:
        std::vector<std::string> events;
        void ProcessEvent(Rml::Event& event) override{
            events.push_back(event.GetType() + ":" + std::to_string(event.GetParameter<int>("pointer_id", 0)) + ":" +
                             event.GetParameter<Rml::String>("pointer_type", ""));
        }
    };
}

TEST_F(InputTest, MultiTouchLiftingOneFingerKeepsTheOther){
    Rml::Element* a = element("touchA");
    Rml::Element* b = element("touchB");
    PointerRecorder clicks;
    a->AddEventListener(Rml::EventId::Click, &clicks);
    b->AddEventListener(Rml::EventId::Click, &clicks);

    Engine::systemTouchStart(0, touchAX, touchAY);
    Engine::systemTouchStart(7, touchBX, touchBY);
    Engine::systemTouchEnd(0, touchAX, touchAY);
    EXPECT_FALSE(a->IsPseudoClassSet("active"));
    EXPECT_TRUE(b->IsPseudoClassSet("active")) << "the other finger's element stays :active";
    EXPECT_TRUE(b->IsPseudoClassSet("hover"));
    Engine::systemTouchEnd(7, touchBX, touchBY);
    EXPECT_FALSE(b->IsPseudoClassSet("active"));
    ASSERT_EQ(clicks.events.size(), 2u) << "each finger clicks its element";
    EXPECT_EQ(clicks.events[0], "click:2:touch") << "D1: a finger is pointer fingerId + 2";
    EXPECT_EQ(clicks.events[1], "click:9:touch");
    a->RemoveEventListener(Rml::EventId::Click, &clicks);
    b->RemoveEventListener(Rml::EventId::Click, &clicks);
}

TEST_F(InputTest, PointerEventsOfTheMouseAndFingers){
    Rml::Element* a = element("touchA");
    PointerRecorder recorder;
    for (Rml::EventId id : {Rml::EventId::Pointerdown, Rml::EventId::Pointerup, Rml::EventId::Mousedown})
        a->AddEventListener(id, &recorder);

    Engine::systemMouseMove(touchAX, touchAY, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, touchAX, touchAY, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, touchAX, touchAY, 0);
    Engine::systemTouchStart(3, touchAX, touchAY);
    Engine::systemTouchEnd(3, touchAX, touchAY);
    const std::vector<std::string> expected = {"pointerdown:1:mouse", "mousedown:1:mouse", "pointerup:1:mouse",
                                               "pointerdown:5:touch", "mousedown:5:touch", "pointerup:5:touch"};
    EXPECT_EQ(recorder.events, expected);
    EXPECT_TRUE(Engine::isPointerOverUI(-1)) << "the mouse is over the document";
    Engine::systemMouseLeave();
    for (Rml::EventId id : {Rml::EventId::Pointerdown, Rml::EventId::Pointerup, Rml::EventId::Mousedown})
        a->RemoveEventListener(id, &recorder);
}

TEST_F(InputTest, OnScreenControlsFeedInputActions){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    UIDocument& controls = fixture->add<UIDocument>();
    ASSERT_TRUE(controls.load("p1/input/controls.html"));
    for (int i = 0; i < 3; i++)
        Engine::systemUpdate();
    UIElement stick = controls.getElementById("stick");
    ASSERT_TRUE(stick.isValid());

    // finger 0 on the stick, 40 px right of its centre (half a radius)
    Engine::systemTouchStart(0, 100, 260);
    Engine::systemTouchMove(0, 140, 260);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.01f);
    EXPECT_EQ(stick.getAttribute("data-pressed"), "true");
    EXPECT_EQ(stick.getAttribute("data-x"), "0.500");

    // finger 1 on the look area, at the same time
    Engine::systemTouchStart(1, 400, 100);
    Engine::systemTouchMove(1, 410, 80);
    EXPECT_NEAR(InputActions::getAxis("Mouse X"), 1.0f, 0.01f);
    EXPECT_NEAR(InputActions::getAxis("Mouse Y"), 2.0f, 0.01f) << "a drag up is Mouse Y up";

    // finger 2 on the button
    Engine::systemTouchStart(2, 560, 280);
    EXPECT_NEAR(InputActions::getAxis("Fire1"), 1.0f, 0.01f);
    Engine::systemTouchEnd(2, 560, 280);
    EXPECT_NEAR(InputActions::getAxis("Fire1"), 0.0f, 0.01f);

    // lifting the look finger keeps the stick
    Engine::systemTouchEnd(1, 410, 80);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.01f);

    // the stick removed while held: nothing stays held
    stick.remove();
    Engine::systemUpdate();
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 0.01f);
    Engine::systemTouchEnd(0, 140, 260);
    InputActions::resetRuntimeState();
}

namespace {

    // an element of any document of the scene's context
    Rml::Element* rmlById(Rml::Context* context, const char* id){
        for (int i = 0; i < context->GetNumDocuments(); i++){
            if (Rml::Element* element = context->GetDocument(i)->GetElementById(id))
                return element;
        }
        return nullptr;
    }

    // the translate() of a stick part placePart moved (0, 0 without a transform)
    Rml::Vector2f partOffset(Rml::Element* control, const char* className){
        Rml::ElementList found;
        control->GetElementsByClassName(found, className);
        if (found.empty())
            return Rml::Vector2f(-1000, -1000);
        const Rml::Property* transform = found.front()->GetLocalProperty(Rml::PropertyId::Transform);
        if (!transform)
            return Rml::Vector2f(0, 0);
        float x = 0, y = 0;
        std::sscanf(transform->ToString().c_str(), "translate(%fpx, %fpx)", &x, &y);
        return Rml::Vector2f(x, y);
    }

}

// review HIGH 2: the finger that slid onto a press-on-enter button is captured by the touch area
// it went down on, which gets its up; the button lets go at the lifted finger's leave
TEST_F(InputTest, PressOnEnterButtonReleasesWhenTheSlidingFingerEnds){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    UIDocument& controls = fixture->add<UIDocument>();
    ASSERT_TRUE(controls.load("p1/input/controls-enter.html"));
    for (int i = 0; i < 3; i++)
        Engine::systemUpdate();
    UIElement fire = controls.getElementById("fire");
    ASSERT_TRUE(fire.isValid());

    // down on the area, slid onto Fire: pressed; lifted there: released
    Engine::systemTouchStart(0, 100, 100);
    Engine::systemTouchMove(0, 360, 140);
    EXPECT_NEAR(InputActions::getAxis("Fire1"), 1.0f, 0.01f);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "true");
    Engine::systemTouchEnd(0, 360, 140);
    EXPECT_NEAR(InputActions::getAxis("Fire1"), 0.0f, 0.01f);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "false");
    EXPECT_FALSE(rmlById(rmlContext(scene()), "fire")->IsPseudoClassSet("active"));

    // the same with a cancel
    Engine::systemTouchStart(1, 100, 100);
    Engine::systemTouchMove(1, 360, 140);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "true");
    Engine::systemTouchCancel(1);
    EXPECT_NEAR(InputActions::getAxis("Fire1"), 0.0f, 0.01f);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "false");

    // a second finger holding it keeps it pressed when the first one lifts
    Engine::systemTouchStart(2, 100, 100);
    Engine::systemTouchMove(2, 360, 140);
    Engine::systemTouchStart(3, 380, 150);
    Engine::systemTouchEnd(2, 360, 140);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "true");
    Engine::systemTouchEnd(3, 380, 150);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "false");
    InputActions::resetRuntimeState();
}

// review MEDIUM 3: the Dynamic stick's base follows the finger from its rest centre (the
// transform that moves it is not part of the layout offset)
TEST_F(InputTest, DynamicStickBaseFollowsFromItsRestCentre){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    UIDocument& controls = fixture->add<UIDocument>();
    ASSERT_TRUE(controls.load("p1/input/controls-enter.html"));
    for (int i = 0; i < 3; i++)
        Engine::systemUpdate();
    UIElement dyn = controls.getElementById("dyn");
    ASSERT_TRUE(dyn.isValid());

    // pressed at the rest centre: the base stays
    Engine::systemTouchStart(0, 150, 260);
    Rml::Vector2f base = partOffset(rmlById(rmlContext(scene()), "dyn"), "stick-base");
    EXPECT_NEAR(base.x, 0.0f, 0.01f);
    EXPECT_NEAR(base.y, 0.0f, 0.01f);

    // two radii right: the base follows one radius (to 230)
    Engine::systemTouchMove(0, 310, 260);
    base = partOffset(rmlById(rmlContext(scene()), "dyn"), "stick-base");
    EXPECT_NEAR(base.x, 80.0f, 0.01f);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 1.0f, 0.01f);

    // 10 px further: the base follows by 10 more, not from a shifted rest
    Engine::systemTouchMove(0, 320, 260);
    base = partOffset(rmlById(rmlContext(scene()), "dyn"), "stick-base");
    EXPECT_NEAR(base.x, 90.0f, 0.05f);
    EXPECT_NEAR(base.y, 0.0f, 0.01f);
    Rml::Vector2f knob = partOffset(rmlById(rmlContext(scene()), "dyn"), "stick-knob");
    EXPECT_NEAR(knob.x, 80.0f, 0.05f);

    // back inside the radius: the base does not move, the value is relative to its new centre
    Engine::systemTouchMove(0, 280, 260);
    base = partOffset(rmlById(rmlContext(scene()), "dyn"), "stick-base");
    EXPECT_NEAR(base.x, 90.0f, 0.05f);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.01f);

    // released: back at rest
    Engine::systemTouchEnd(0, 280, 260);
    base = partOffset(rmlById(rmlContext(scene()), "dyn"), "stick-base");
    EXPECT_NEAR(base.x, 0.0f, 0.01f);
    InputActions::resetRuntimeState();
}

// review HIGH 1 and D6: a finger lifted or a button released outside the canvas, a lost focus and a
// pause end the documents' pointers (cancelled: the controls let go)
TEST_F(InputTest, PointersOutsideTheCanvasFocusLossAndPauseReleaseControls){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    UIDocument& controls = fixture->add<UIDocument>();
    ASSERT_TRUE(controls.load("p1/input/controls-enter.html"));
    for (int i = 0; i < 3; i++)
        Engine::systemUpdate();
    UIElement stick = controls.getElementById("stick");
    UIElement fire = controls.getElementById("fire");
    ASSERT_TRUE(stick.isValid());

    // the stick's finger dragged off the canvas and lifted there
    Engine::systemTouchStart(0, 540, 260);
    Engine::systemTouchMove(0, 600, 260);
    EXPECT_EQ(stick.getAttribute("data-pressed"), "true");
    Engine::systemTouchEnd(0, 700, 260);
    EXPECT_EQ(stick.getAttribute("data-pressed"), "false");
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 0.01f);
    EXPECT_FALSE(documentSystem(scene()).getContext()->hasTouch(0));

    // the mouse pressing Fire, released off the canvas
    Engine::systemMouseMove(360, 140, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 360, 140, 0);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "true");
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, -20, 140, 0);
    EXPECT_EQ(fire.getAttribute("data-pressed"), "false");
    EXPECT_NEAR(InputActions::getAxis("Fire1"), 0.0f, 0.01f);
    EXPECT_FALSE(rmlById(rmlContext(scene()), "fire")->IsPseudoClassSet("active"));

    // focus lost while a finger holds the stick and the mouse holds Fire
    Engine::systemTouchStart(1, 540, 260);
    Engine::systemTouchMove(1, 580, 260);
    Engine::systemMouseMove(360, 140, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 360, 140, 0);
    EXPECT_EQ(stick.getAttribute("data-pressed"), "true");
    EXPECT_EQ(fire.getAttribute("data-pressed"), "true");
    Engine::systemFocus(false);
    EXPECT_EQ(stick.getAttribute("data-pressed"), "false");
    EXPECT_EQ(fire.getAttribute("data-pressed"), "false");
    EXPECT_FALSE(documentSystem(scene()).getContext()->hasTouch(1));
    Engine::systemFocus(true);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 360, 140, 0);
    Engine::systemTouchEnd(1, 580, 260);

    // paused while a finger holds the stick
    Engine::systemTouchStart(2, 540, 260);
    Engine::systemTouchMove(2, 580, 260);
    EXPECT_EQ(stick.getAttribute("data-pressed"), "true");
    Engine::systemPause();
    EXPECT_EQ(stick.getAttribute("data-pressed"), "false");
    EXPECT_FALSE(documentSystem(scene()).getContext()->hasTouch(2));
    Engine::systemResume();
    InputActions::resetRuntimeState();
}

// review HIGH 1 and D6, the component UI: a finger lifted or a mouse button released outside
// the canvas, a lost focus and a pause cancel its pointers (the controls let go, no click)
TEST_F(InputTest, ComponentUIPointersOutsideTheCanvasFocusLossAndPauseAreCancelled){
    // focused, as an app is: a pause in an earlier test took the focus and a resume does not give
    // it back (platform services), so the focus loss below would be no change
    Engine::systemFocus(true);
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    // where the document has nothing: x 500..620, y 180..300 (centre (560, 240), radius 60)
    OnScreenStick& stick = fixture->add<OnScreenStick>();
    stick.setPosition(500, 180);
    stick.setSize(120, 120);
    Button& button = fixture->add<Button>();
    button.setPosition(500, 310);
    button.setSize(120, 40);
    int releases = 0;
    button.getComponent<ButtonComponent>().onRelease.add(callbackTag, std::function<void()>([&](){ releases++; }));
    GpuEnvironment::drawFrames(2);
    UISystem& ui = *scene().getSystem<UISystem>();
    OnScreenStickComponent& control = stick.getStick();

    // the stick's finger dragged into the letterbox (off the canvas) and lifted there
    Engine::systemTouchStart(0, 560, 240);
    Engine::systemTouchMove(0, 590, 240);
    EXPECT_TRUE(control.pressed);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.01f);
    Engine::systemTouchEnd(0, 700, 240);
    EXPECT_FALSE(control.pressed);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 0.01f);
    EXPECT_TRUE(ui.getPointers().find(0) == ui.getPointers().end());

    // the right button pressed on the Button, released off the canvas
    Engine::systemMouseMove(560, 330, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_RIGHT, 560, 330, 0);
    ASSERT_TRUE(ui.getPointers().count(-2) == 1);
    EXPECT_TRUE(ui.getPointers().at(-2).down);
    Engine::systemMouseUp(D_MOUSE_BUTTON_RIGHT, 700, 330, 0);
    EXPECT_TRUE(ui.getPointers().count(-2) == 0) << "the right button's press ended";

    // the left button: the Button released without a click
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 560, 330, 0);
    EXPECT_TRUE(button.getComponent<ButtonComponent>().pressed);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 560, -30, 0);
    EXPECT_FALSE(button.getComponent<ButtonComponent>().pressed);
    EXPECT_EQ(releases, 1);
    EXPECT_FALSE(ui.isPointerCaptured(-1));

    // focus lost while a finger holds the stick and the mouse the Button (from a focused app: an
    // earlier test's pause took the focus, and the engine keeps it until it comes back)
    Engine::systemFocusChanged(true);
    Engine::systemFocus(true);
    Engine::systemTouchStart(1, 560, 240);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 560, 330, 0);
    EXPECT_TRUE(control.pressed);
    EXPECT_TRUE(button.getComponent<ButtonComponent>().pressed);
    Engine::systemFocus(false);
    EXPECT_FALSE(control.pressed);
    EXPECT_FALSE(button.getComponent<ButtonComponent>().pressed);
    Engine::systemFocus(true);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 560, 330, 0);
    Engine::systemTouchEnd(1, 560, 240);
    EXPECT_EQ(releases, 2) << "the up after the cancel releases nothing more";

    // paused while a finger holds the stick
    Engine::systemTouchStart(2, 560, 240);
    EXPECT_TRUE(control.pressed);
    Engine::systemPause();
    EXPECT_FALSE(control.pressed);
    Engine::systemResume();
    Engine::systemTouchEnd(2, 560, 240);
    button.getComponent<ButtonComponent>().onRelease.remove(callbackTag);
    InputActions::resetRuntimeState();
}

TEST_F(InputTest, MouseLeaveClearsHover){
    Engine::systemMouseMove(buttonX, buttonY, 0);
    EXPECT_TRUE(element("button")->IsPseudoClassSet("hover"));
    Engine::systemMouseLeave();
    EXPECT_FALSE(element("button")->IsPseudoClassSet("hover"));
    Engine::systemMouseEnter();
    Engine::systemMouseMove(buttonX, buttonY + 1, 0);
    EXPECT_TRUE(element("button")->IsPseudoClassSet("hover"));
}

// ---- IME ----

TEST_F(InputTest, ImeComposition){
    click(nameX, nameY);
    Engine::systemTextInput("x");

    // compose "ka" → か, then commit
    Engine::systemImeComposition("k", 1);
    EXPECT_EQ(value("name"), "xk");
    Engine::systemImeComposition("ka", 2);
    EXPECT_EQ(value("name"), "xka");
    Engine::systemImeComposition("\xE3\x81\x8B", 1);
    EXPECT_EQ(value("name"), "x\xE3\x81\x8B");
    Engine::systemTextInput("\xE3\x81\x8B");
    EXPECT_EQ(value("name"), "x\xE3\x81\x8B");
    EXPECT_FALSE(documentSystem(scene()).getContext()->getTextInputHandler().isComposing());

    // typing goes on after the commit
    Engine::systemTextInput("y");
    EXPECT_EQ(value("name"), "x\xE3\x81\x8By");

    // cancel restores the text the composition replaced
    Engine::systemImeComposition("ab", 2);
    EXPECT_EQ(value("name"), "x\xE3\x81\x8By" "ab");
    Engine::systemImeCancel();
    EXPECT_EQ(value("name"), "x\xE3\x81\x8By");
    Engine::systemTextInput("z");
    EXPECT_EQ(value("name"), "x\xE3\x81\x8Byz");

    // cursor -1 selects the whole composition
    Engine::systemImeComposition("abc", -1);
    auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element("name"));
    ASSERT_NE(input, nullptr);
    int start = 0, end = 0;
    input->GetSelection(&start, &end, nullptr);
    EXPECT_EQ(start, 4);
    EXPECT_EQ(end, 7);
    Engine::systemImeCancel();
    EXPECT_EQ(value("name"), "x\xE3\x81\x8Byz");

    // nothing to compose into without a field
    click(bodyX, bodyY);
    Engine::systemImeComposition("q", 1);
    EXPECT_EQ(value("name"), "x\xE3\x81\x8Byz");
}

TEST_F(InputTest, FocusLossCancelsTheComposition){
    click(nameX, nameY);
    Engine::systemTextInput("a");
    Engine::systemImeComposition("bc", 2);
    EXPECT_EQ(value("name"), "abc");

    click(otherX, otherY);
    EXPECT_EQ(value("name"), "a");
    EXPECT_FALSE(documentSystem(scene()).getContext()->getTextInputHandler().isComposing());
}

// ---- virtual keyboard, caret area ----

TEST_F(InputTest, FocusOpensTheVirtualKeyboardAndPlacesTheCaret){
    TestSystem& system = TestSystem::get();
    const int shows = system.virtualKeyboardShows;
    const int hides = system.virtualKeyboardHides;
    element("name")->SetAttribute("value", "hi");

    // the keyboard opens at the update after the focus change, with the field's value
    click(nameX, nameY);
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardShows, shows + 1);
    EXPECT_EQ(system.virtualKeyboardText, L"hi");

    // a press on the focused field asks again (a keyboard the user closed reopens)
    click(nameX + 20, nameY);
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardShows, shows + 2);

    // the caret area, screen pixels (here the screen is the canvas)
    EXPECT_GT(system.textInputAreaCalls, 0);
    EXPECT_GE(system.textInputArea[0], 40.0f);
    EXPECT_LE(system.textInputArea[0], 240.0f);
    EXPECT_GE(system.textInputArea[1], 120.0f);
    EXPECT_LE(system.textInputArea[1], 150.0f);
    EXPECT_GT(system.textInputArea[3], 0.0f);

    // leaving the field hides it (the component UI hides it too on a press outside its fields)
    click(bodyX, bodyY);
    EXPECT_GE(system.virtualKeyboardHides, hides + 1);
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardShows, shows + 2);
}

// ---- clipboard ----

TEST(InputClipboard, SystemAndRmlSystemRoundTrip){
    System::instance().setClipboardText("caf\xC3\xA9");
    EXPECT_EQ(System::instance().getClipboardText(), "caf\xC3\xA9");

    RmlSystem rmlSystem;
    rmlSystem.SetClipboardText("from rml");
    EXPECT_EQ(System::instance().getClipboardText(), "from rml");
    Rml::String text;
    System::instance().setClipboardText("from system");
    rmlSystem.GetClipboardText(text);
    EXPECT_EQ(text, "from system");
}

TEST_F(InputTest, CopyAndPasteBetweenFields){
    click(nameX, nameY);
    Engine::systemTextInput("hello");

    key(D_KEY_A, D_MODIFIER_CONTROL);
    key(D_KEY_C, D_MODIFIER_CONTROL);
    EXPECT_EQ(System::instance().getClipboardText(), "hello");

    click(otherX, otherY);
    key(D_KEY_V, D_MODIFIER_CONTROL);
    EXPECT_EQ(value("other"), "hello");
    EXPECT_EQ(keyDowns, 0);
}

// ---- cursor ----

TEST(InputCursor, CssNames){
    EXPECT_EQ(RmlSystem::cursorFromCss(""), CursorType::ARROW);
    EXPECT_EQ(RmlSystem::cursorFromCss("default"), CursorType::ARROW);
    EXPECT_EQ(RmlSystem::cursorFromCss("pointer"), CursorType::POINTING_HAND);
    EXPECT_EQ(RmlSystem::cursorFromCss("text"), CursorType::IBEAM);
    EXPECT_EQ(RmlSystem::cursorFromCss("move"), CursorType::RESIZE_ALL);
    EXPECT_EQ(RmlSystem::cursorFromCss("col-resize"), CursorType::RESIZE_EW);
    EXPECT_EQ(RmlSystem::cursorFromCss("ns-resize"), CursorType::RESIZE_NS);
    EXPECT_EQ(RmlSystem::cursorFromCss("nwse-resize"), CursorType::RESIZE_NWSE);
    EXPECT_EQ(RmlSystem::cursorFromCss("nesw-resize"), CursorType::RESIZE_NESW);
    EXPECT_EQ(RmlSystem::cursorFromCss("not-allowed"), CursorType::NOT_ALLOWED);
    EXPECT_EQ(RmlSystem::cursorFromCss("wait"), CursorType::WAIT);
    EXPECT_EQ(RmlSystem::cursorFromCss("progress"), CursorType::PROGRESS);
    EXPECT_EQ(RmlSystem::cursorFromCss("help"), CursorType::HELP);
    EXPECT_EQ(RmlSystem::cursorFromCss("grab"), CursorType::GRAB);
    EXPECT_EQ(RmlSystem::cursorFromCss("Grabbing"), CursorType::GRABBING);
    EXPECT_EQ(RmlSystem::cursorFromCss("zoom-in"), CursorType::ARROW);
    // a list: the first keyword known
    EXPECT_EQ(RmlSystem::cursorFromCss("url(hand.png) 4 4, zoom-in, grab, pointer"), CursorType::GRAB);
}

TEST_F(InputTest, CursorFollowsTheHoveredElement){
    TestSystem& system = TestSystem::get();

    Engine::systemMouseMove(buttonX, buttonY, 0);
    EXPECT_EQ(Engine::getMouseCursor(), CursorType::POINTING_HAND);
    EXPECT_EQ(system.mouseCursor, CursorType::POINTING_HAND);

    Engine::systemMouseMove(helpX, helpY, 0);
    EXPECT_EQ(Engine::getMouseCursor(), CursorType::HELP);

    Engine::systemMouseMove(bodyX, bodyY, 0);
    EXPECT_EQ(Engine::getMouseCursor(), CursorType::ARROW);
    EXPECT_EQ(system.mouseCursor, CursorType::ARROW);
}
