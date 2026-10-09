// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Events of the element API (phase 1 §3.9.3) through C++ listeners: DOM names and fields,
// propagation phases, stop(Immediate)Propagation, synthesized change, custom events, and the
// fork patch Event::PreventDefault (§3.14 patch 1).

#include "TestScriptCommon.h"

#include "Input.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class ScriptEventTest : public ScriptTest {};

}

TEST_F(ScriptEventTest, PhasesAndOrder){
    UIElement list = document->getElementById("list");
    UIElement i1 = document->getElementById("i1");
    std::vector<std::string> order;
    list.addEventListener("click", [&](UIEvent& event){
        order.push_back("list-capture-" + std::to_string(event.getEventPhase()));
        EXPECT_TRUE(event.getCurrentTarget() == list);
        EXPECT_TRUE(event.getTarget() == i1);
    }, true);
    list.addEventListener("click", [&](UIEvent& event){ order.push_back("list-bubble-" + std::to_string(event.getEventPhase())); });
    i1.addEventListener("click", [&](UIEvent& event){ order.push_back("i1-" + std::to_string(event.getEventPhase())); });

    i1.click();
    EXPECT_EQ(order, (std::vector<std::string>{"list-capture-1", "i1-2", "list-bubble-3"}));
}

TEST_F(ScriptEventTest, StopPropagation){
    UIElement list = document->getElementById("list");
    UIElement i1 = document->getElementById("i1");
    std::vector<std::string> order;
    list.addEventListener("click", [&](UIEvent&){ order.push_back("list"); });
    uint64_t first = i1.addEventListener("click", [&](UIEvent& event){ order.push_back("first"); event.stopPropagation(); });
    uint64_t second = i1.addEventListener("click", [&](UIEvent&){ order.push_back("second"); });

    i1.click();
    EXPECT_EQ(order, (std::vector<std::string>{"first", "second"}));

    order.clear();
    i1.removeEventListener(first);
    i1.removeEventListener(second);
    i1.addEventListener("click", [&](UIEvent& event){ order.push_back("immediate"); event.stopImmediatePropagation(); });
    i1.addEventListener("click", [&](UIEvent&){ order.push_back("never"); });
    i1.click();
    EXPECT_EQ(order, (std::vector<std::string>{"immediate"}));
}

TEST_F(ScriptEventTest, EventInvalidAfterDispatch){
    UIElement btn = document->getElementById("btn");
    UIEvent kept;
    btn.addEventListener("click", [&](UIEvent& event){
        EXPECT_TRUE(event.isValid());
        kept = event;
    });
    btn.click();
    EXPECT_FALSE(kept.isValid());
    EXPECT_EQ(kept.getType(), "");
    EXPECT_FALSE(kept.getTarget().isValid());
    EXPECT_EQ(kept.getEventPhase(), 0);
    kept.preventDefault();   // harmless
}

TEST_F(ScriptEventTest, MouseFields){
    UIElement btn = document->getElementById("btn");
    std::vector<int> buttons;
    float x = 0, y = 0;
    btn.addEventListener("mousedown", [&](UIEvent& event){
        buttons.push_back(event.getButton());
        x = event.getClientX();
        y = event.getClientY();
    });

    context()->ProcessMouseMove(350, 40, 0);
    // RmlUi: 0 left, 1 right, 2 middle -> DOM 0, 2, 1
    for (int button = 0; button < 3; button++){
        context()->ProcessMouseButtonDown(button, 0);
        context()->ProcessMouseButtonUp(button, 0);
    }
    EXPECT_EQ(buttons, (std::vector<int>{0, 2, 1}));
    EXPECT_FLOAT_EQ(x, 350.0f);
    EXPECT_FLOAT_EQ(y, 40.0f);
}

TEST_F(ScriptEventTest, WheelAndModifiers){
    UIElement scroller = document->getElementById("scroller");
    float dx = -1, dy = -1;
    std::string type;
    scroller.addEventListener("wheel", [&](UIEvent& event){
        type = event.getType();
        dx = event.getDeltaX();
        dy = event.getDeltaY();
    });
    context()->ProcessMouseMove(50, 225, 0);
    context()->ProcessMouseWheel(Rml::Vector2f(0.0f, 2.0f), 0);
    EXPECT_EQ(type, "wheel");
    EXPECT_FLOAT_EQ(dx, 0.0f);
    EXPECT_FLOAT_EQ(dy, 2.0f);

    UIElement text = document->getElementById("text");
    bool ctrl = false, shift = false, alt = true, meta = true;
    text.addEventListener("keydown", [&](UIEvent& event){
        ctrl = event.getCtrlKey();
        shift = event.getShiftKey();
        alt = event.getAltKey();
        meta = event.getMetaKey();
    });
    text.focus();
    context()->ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_CTRL | Rml::Input::KM_SHIFT);
    EXPECT_TRUE(ctrl);
    EXPECT_TRUE(shift);
    EXPECT_FALSE(alt);
    EXPECT_FALSE(meta);
}

TEST_F(ScriptEventTest, KeyFields){
    UIElement text = document->getElementById("text");
    struct Key { std::string key, code; int keyCode; bool repeat; std::string type; };
    std::vector<Key> keys;
    auto record = [&](UIEvent& event){
        keys.push_back({event.getKey(), event.getCode(), event.getKeyCode(), event.isRepeat(), event.getType()});
    };
    text.addEventListener("keydown", record);
    text.addEventListener("keyup", record);
    text.focus();

    context()->ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_SHIFT);
    context()->ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_SHIFT);   // held: a repeat
    context()->ProcessKeyUp(Rml::Input::KI_A, 0);
    context()->ProcessKeyDown(Rml::Input::KI_A, 0);
    context()->ProcessKeyUp(Rml::Input::KI_A, 0);
    context()->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    context()->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
    context()->ProcessKeyDown(Rml::Input::KI_1, Rml::Input::KM_SHIFT);
    context()->ProcessKeyUp(Rml::Input::KI_1, Rml::Input::KM_SHIFT);

    ASSERT_EQ(keys.size(), 9u);
    EXPECT_EQ(keys[0].key, "A");
    EXPECT_EQ(keys[0].code, "KeyA");
    EXPECT_EQ(keys[0].keyCode, D_KEY_A);
    EXPECT_FALSE(keys[0].repeat);
    EXPECT_TRUE(keys[1].repeat);
    EXPECT_EQ(keys[2].type, "keyup");
    EXPECT_EQ(keys[2].key, "a");
    EXPECT_FALSE(keys[2].repeat);
    EXPECT_EQ(keys[3].key, "a");
    EXPECT_FALSE(keys[3].repeat);
    EXPECT_EQ(keys[5].key, "Enter");
    EXPECT_EQ(keys[5].code, "Enter");
    EXPECT_EQ(keys[5].keyCode, D_KEY_ENTER);
    EXPECT_EQ(keys[7].key, "!");
    EXPECT_EQ(keys[7].code, "Digit1");

    // not a key event: defaults
    UIElement btn = document->getElementById("btn");
    std::string clickKey = "x";
    int clickCode = -1;
    btn.addEventListener("click", [&](UIEvent& event){ clickKey = event.getKey(); clickCode = event.getKeyCode(); });
    btn.click();
    EXPECT_EQ(clickKey, "");
    EXPECT_EQ(clickCode, 0);
}

TEST_F(ScriptEventTest, InputChangeAndTextInput){
    UIElement text = document->getElementById("text");
    int inputs = 0, changes = 0;
    std::string typed;
    text.addEventListener("input", [&](UIEvent& event){ inputs++; EXPECT_EQ(event.getType(), "input"); });
    text.addEventListener("change", [&](UIEvent& event){ changes++; EXPECT_EQ(event.getType(), "change"); });
    text.addEventListener("textinput", [&](UIEvent& event){ typed += event.getData(); });

    text.focus();
    update();
    context()->ProcessTextInput("ab");
    update();
    EXPECT_GT(inputs, 0);
    EXPECT_EQ(changes, 0) << "a text field changes on blur";
    EXPECT_EQ(typed, "ab");
    text.blur();
    EXPECT_EQ(changes, 1);

    // focus and blur without an edit: no change
    text.focus();
    text.blur();
    EXPECT_EQ(changes, 1);

    // a checkbox changes when toggled; a listener on an ancestor hears it (bubbles)
    int boxChanges = 0, bodyChanges = 0;
    UIElement check = document->getElementById("check");
    check.addEventListener("change", [&](UIEvent&){ boxChanges++; });
    document->getBody().addEventListener("change", [&](UIEvent&){ bodyChanges++; });
    check.click();
    EXPECT_TRUE(check.getChecked());
    EXPECT_EQ(boxChanges, 1);
    EXPECT_EQ(bodyChanges, 1);
}

TEST_F(ScriptEventTest, CustomEvents){
    UIElement i1 = document->getElementById("i1");
    UIElement list = document->getElementById("list");
    UIValue received;
    int heard = 0;
    list.addEventListener("score", [&](UIEvent& event){
        heard++;
        received = event.getDetail();
        EXPECT_EQ(event.getType(), "score");
    });

    UIValue detail = UIValue::object();
    detail["points"] = 10;
    detail["name"] = "ann";
    EXPECT_TRUE(i1.dispatchEvent("score", detail));
    EXPECT_EQ(heard, 1);
    EXPECT_TRUE(received == detail);

    // not bubbling: the ancestor does not hear it
    EXPECT_TRUE(i1.dispatchEvent("score", detail, false));
    EXPECT_EQ(heard, 1);

    // prevented: false
    i1.addEventListener("score", [](UIEvent& event){ event.preventDefault(); EXPECT_TRUE(event.isDefaultPrevented()); });
    EXPECT_FALSE(i1.dispatchEvent("score"));
}

TEST_F(ScriptEventTest, UnsupportedEventsWarn){
    UIElement btn = document->getElementById("btn");
    EXPECT_EQ(btn.addEventListener("mouseenter", [](UIEvent&){}), 0u);
    EXPECT_EQ(btn.addEventListener("pointerdown", [](UIEvent&){}), 0u);
    EXPECT_EQ(btn.addEventListener("touchstart", [](UIEvent&){}), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "supported: click"), 3u);
    TestSystem::get().clearLogs();
}

TEST_F(ScriptEventTest, ListenersFreedWithTheirElements){
    UIElement list = document->getElementById("list");
    const size_t before = HtmlUIEventListener::liveCount();
    list.addEventListener("click", [](UIEvent&){});
    document->getElementById("i1").addEventListener("click", [](UIEvent&){});
    EXPECT_EQ(HtmlUIEventListener::liveCount(), before + 2);
    list.setInnerHTML("");
    EXPECT_EQ(HtmlUIEventListener::liveCount(), before + 1);
    ASSERT_TRUE(document->load("ecs/basic.rml"));
    update();
    EXPECT_EQ(HtmlUIEventListener::liveCount(), before);
}

// ---- fork patch: Event::PreventDefault ----

TEST_F(ScriptEventTest, PreventDefaultKeepsCheckboxUnchecked){
    UIElement check = document->getElementById("check");
    check.addEventListener("click", [](UIEvent& event){ event.preventDefault(); });
    check.click();
    EXPECT_FALSE(check.getChecked());

    // the same click through the pointer
    UIRect rect = check.getBoundingClientRect();
    context()->ProcessMouseMove((int)(rect.x + rect.width / 2), (int)(rect.y + rect.height / 2), 0);
    context()->ProcessMouseButtonDown(0, 0);
    context()->ProcessMouseButtonUp(0, 0);
    EXPECT_FALSE(check.getChecked());
}

TEST_F(ScriptEventTest, PreventDefaultOnKeyDown){
    UIElement text = document->getElementById("text");
    text.setValue("");
    text.focus();
    update();

    bool prevent = true;
    text.addEventListener("keydown", [&](UIEvent& event){ if (prevent) event.preventDefault(); });

    // a prevented key down is consumed, and the text it produces is not inserted
    EXPECT_FALSE(context()->ProcessKeyDown(Rml::Input::KI_A, 0));
    EXPECT_FALSE(context()->ProcessTextInput("a"));
    context()->ProcessKeyUp(Rml::Input::KI_A, 0);
    EXPECT_EQ(text.getValue(), "");

    prevent = false;
    context()->ProcessKeyDown(Rml::Input::KI_B, 0);
    context()->ProcessTextInput("b");
    context()->ProcessKeyUp(Rml::Input::KI_B, 0);
    EXPECT_EQ(text.getValue(), "b");

    // a capture listener of an ancestor prevents the field's own key handling (Backspace)
    document->getBody().addEventListener("keydown", [](UIEvent& event){ event.preventDefault(); }, true);
    context()->ProcessKeyDown(Rml::Input::KI_BACK, 0);
    context()->ProcessKeyUp(Rml::Input::KI_BACK, 0);
    EXPECT_EQ(text.getValue(), "b");
}

TEST_F(ScriptEventTest, PreventDefaultOnWheelStopsScrolling){
    UIElement scroller = document->getElementById("scroller");
    scroller.addEventListener("wheel", [](UIEvent& event){ event.preventDefault(); });
    context()->ProcessMouseMove(50, 225, 0);
    EXPECT_FALSE(context()->ProcessMouseWheel(Rml::Vector2f(0.0f, 3.0f), 0));
    for (int i = 0; i < 10; i++)
        update();
    EXPECT_FLOAT_EQ(scroller.getScrollTop(), 0.0f);
}
