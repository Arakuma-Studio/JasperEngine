// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The soft keyboard of document text fields (docs/specs/text-entry-gamepad-cursor-back-button.md
// part A, AC1 and AC3-AC5): traits from the HTML attributes, the field's text sent back to a
// keyboard that keeps text, the action key's default actions, and traits changing without hiding.

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "Input.h"
#include "htmlui/TextEntryTraits.h"
#include "htmlui/RmlTextInputHandler.h"
#include "AllocationCounter.h"
#include "input/TextEntry.h"
#include "object/ui/UIDocument.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    TextEntryTraits entryTraits(KeyboardType keyboard, EnterKey enterKey = EnterKey::Default, AutoCapitalize capitalize = AutoCapitalize::Off,
                           bool autocorrect = false, bool secure = false, bool multiline = false, int maxLength = -1, bool signedNumber = false){
        TextEntryTraits t;
        t.keyboard = keyboard;
        t.enterKey = enterKey;
        t.capitalize = capitalize;
        t.autocorrect = autocorrect;
        t.secure = secure;
        t.multiline = multiline;
        t.maxLength = maxLength;
        t.signedNumber = signedNumber;
        return t;
    }

    std::string describeTraits(const TextEntryTraits& t){
        return std::string(keyboardTypeName(t.keyboard)) + " enter=" + enterKeyName(t.enterKey) + " cap=" + autoCapitalizeName(t.capitalize) +
               " autocorrect=" + std::to_string(t.autocorrect) + " secure=" + std::to_string(t.secure) + " multiline=" +
               std::to_string(t.multiline) + " max=" + std::to_string(t.maxLength) + " signed=" + std::to_string(t.signedNumber);
    }

    class SubmitCounter : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event&) override{ count++; }
        int count = 0;
    };

    class EnterPreventer : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            hint = event.GetParameter<Rml::String>("enterKeyHint", "<none>");
            event.PreventDefault();
        }
        std::string hint;
    };

    class EnterHintRecorder : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            hints.push_back(event.GetParameter<Rml::String>("enterKeyHint", "<none>"));
        }
        std::vector<std::string> hints;
    };

    class TextEntryTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            fixture.reset(new SceneFixture());
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            TestSystem::get().keepsKeyboardText = false;
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
        }

        void load(const char* path){
            UIDocument& document = fixture->add<UIDocument>();
            ASSERT_TRUE(document.load(path)) << path;
            updateDocuments(scene());
        }

        Scene& scene(){
            return fixture->getScene();
        }

        Rml::Element* element(const char* id){
            Rml::Context* context = rmlContext(scene());
            for (int i = 0; context && i < context->GetNumDocuments(); i++){
                if (Rml::Element* found = context->GetDocument(i)->GetElementById(id))
                    return found;
            }
            return nullptr;
        }

        std::string value(const char* id){
            return element(id)->GetAttribute<Rml::String>("value", "");
        }

        Rml::Element* focused(){
            return rmlContext(scene())->GetFocusElement();
        }

        void focus(const char* id){
            ASSERT_NE(element(id), nullptr) << id;
            ASSERT_TRUE(element(id)->Focus());
            updateDocuments(scene());
        }

        std::unique_ptr<SceneFixture> fixture;
    };

}

TEST_F(TextEntryTest, TraitsFromAttributes){
    load("text-entry/fields.html");

    struct Row {
        const char* id;
        TextEntryTraits expected;
    };
    const Row rows[] = {
        {"text", entryTraits(KeyboardType::Text)},
        {"email", entryTraits(KeyboardType::Email)},
        {"url", entryTraits(KeyboardType::Url)},
        {"tel", entryTraits(KeyboardType::Phone)},
        {"search", entryTraits(KeyboardType::Search, EnterKey::Search)},
        {"count", entryTraits(KeyboardType::Number)},
        {"amount", entryTraits(KeyboardType::Decimal, EnterKey::Default, AutoCapitalize::Off, false, false, false, -1, true)},
        {"offset", entryTraits(KeyboardType::Decimal, EnterKey::Default, AutoCapitalize::Off, false, false, false, -1, true)},
        {"half", entryTraits(KeyboardType::Decimal)},
        {"any", entryTraits(KeyboardType::Decimal)},
        {"password", entryTraits(KeyboardType::Text, EnterKey::Default, AutoCapitalize::Off, false, true)},
        {"pin", entryTraits(KeyboardType::Number, EnterKey::Default, AutoCapitalize::Off, false, true)},
        {"notes", entryTraits(KeyboardType::Text, EnterKey::Enter, AutoCapitalize::Off, false, false, true)},
        {"decimal", entryTraits(KeyboardType::Decimal)},
        {"hidden-keyboard", entryTraits(KeyboardType::None)},
        {"mail-send", entryTraits(KeyboardType::Email, EnterKey::Send, AutoCapitalize::Sentences, false, false, false, 32)},
        {"next", entryTraits(KeyboardType::Text, EnterKey::Next)},
        {"previous", entryTraits(KeyboardType::Text, EnterKey::Previous, AutoCapitalize::Words)},
        {"done", entryTraits(KeyboardType::Text, EnterKey::Done, AutoCapitalize::Characters)},
        {"go", entryTraits(KeyboardType::Text, EnterKey::Go, AutoCapitalize::Sentences, true)},
        {"find", entryTraits(KeyboardType::Text, EnterKey::Search, AutoCapitalize::Off, false)},
        {"short", entryTraits(KeyboardType::Text, EnterKey::Default, AutoCapitalize::Off, false, false, false, 5)},
        {"web", entryTraits(KeyboardType::Url)},
        {"message", entryTraits(KeyboardType::Text, EnterKey::Send, AutoCapitalize::Off, true, false, true, 100)},
    };
    ASSERT_EQ(sizeof(rows) / sizeof(rows[0]), 24u);
    for (const Row& row : rows){
        Rml::Element* field = element(row.id);
        ASSERT_NE(field, nullptr) << row.id;
        EXPECT_TRUE(htmlui::isTextEntryField(field)) << row.id;
        const TextEntryTraits actual = htmlui::textEntryTraitsOf(field);
        EXPECT_EQ(describeTraits(actual), describeTraits(row.expected)) << row.id;
    }

    // focusing a field hands its traits to the platform's keyboard
    TestSystem& system = TestSystem::get();
    const int shows = system.virtualKeyboardShows;
    focus("mail-send");
    EXPECT_EQ(system.virtualKeyboardShows, shows + 1);
    EXPECT_EQ(describeTraits(system.virtualKeyboardTraits), describeTraits(rows[15].expected));

    // an attribute a script changes while focused re-sends the traits
    element("mail-send")->SetAttribute("enterkeyhint", "go");
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardShows, shows + 2);
    EXPECT_EQ(system.virtualKeyboardTraits.enterKey, EnterKey::Go);
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardShows, shows + 2);
}

TEST_F(TextEntryTest, ResyncAfterRejectedText){
    TestSystem& system = TestSystem::get();
    system.keepsKeyboardText = true;
    load("text-entry/form.html");
    focus("short");
    const int updates = system.virtualKeyboardUpdates;

    // the keyboard typed past maxlength=5: the field keeps five, the keyboard gets them back
    Engine::systemTextInputState(u"abcdefg", 7, 7, -1, -1);
    EXPECT_EQ(value("short"), "abcde");
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardUpdates, updates + 1);
    EXPECT_EQ(system.virtualKeyboardUpdateText, L"abcde");
    EXPECT_EQ(system.virtualKeyboardSelection[0], 5);
    EXPECT_EQ(system.virtualKeyboardSelection[1], 5);
    // once: the next frames agree
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardUpdates, updates + 1);

    // an edit the field takes is not echoed (the keyboard deleted one character)
    Engine::systemTextInputState(u"abcd", 4, 4, -1, -1);
    EXPECT_EQ(value("short"), "abcd");
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardUpdates, updates + 1);

    // a script sets the value: one resync
    element("short")->SetAttribute("value", "x");
    updateDocuments(scene());
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardUpdates, updates + 2);
    EXPECT_EQ(system.virtualKeyboardUpdateText, L"x");

    // and the keyboard's later edits start from the field's text
    Engine::systemTextInputState(u"xy", 2, 2, -1, -1);
    EXPECT_EQ(value("short"), "xy");
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardUpdates, updates + 2);
}

TEST_F(TextEntryTest, NoResyncWhereTheKeyboardKeepsNoText){
    TestSystem& system = TestSystem::get();
    load("text-entry/form.html");
    focus("short");
    const int updates = system.virtualKeyboardUpdates;
    Engine::systemTextInput("abcdefg");
    element("short")->SetAttribute("value", "x");
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardUpdates, updates);
}

TEST_F(TextEntryTest, EnterKeyHintActions){
    TestSystem& system = TestSystem::get();
    load("text-entry/form.html");
    SubmitCounter submits;
    element("form")->AddEventListener(Rml::EventId::Submit, &submits);

    // next: the next text field, its traits re-sent, the keyboard kept
    focus("name");
    const int shows = system.virtualKeyboardShows;
    const int hides = system.virtualKeyboardHides;
    Engine::systemEditorAction(EnterKey::Next);
    updateDocuments(scene());
    EXPECT_EQ(focused(), element("mail"));
    EXPECT_EQ(system.virtualKeyboardShows, shows + 1);
    EXPECT_EQ(system.virtualKeyboardTraits.keyboard, KeyboardType::Email);
    EXPECT_EQ(system.virtualKeyboardHides, hides);

    // previous: back
    Engine::systemEditorAction(EnterKey::Previous);
    updateDocuments(scene());
    EXPECT_EQ(focused(), element("name"));
    EXPECT_EQ(system.virtualKeyboardHides, hides);

    // next skips what is not a text field (the button) and stops at the textarea
    focus("code");
    Engine::systemEditorAction(EnterKey::Next);
    updateDocuments(scene());
    EXPECT_EQ(focused(), element("note"));

    // done: blurred, the keyboard closes
    focus("code");
    Engine::systemEditorAction(EnterKey::Done);
    updateDocuments(scene());
    EXPECT_FALSE(htmlui::isTextEntryField(focused()));
    EXPECT_EQ(system.virtualKeyboardHides, hides + 1);

    // send: the form is submitted once, the field blurred
    focus("mail");
    Engine::systemEditorAction(EnterKey::Send);
    updateDocuments(scene());
    EXPECT_EQ(submits.count, 1);
    EXPECT_NE(focused(), element("mail"));

    // a keydown listener's preventDefault stops all of it, and sees the hint
    EnterPreventer prevent;
    element("mail")->AddEventListener(Rml::EventId::Keydown, &prevent);
    focus("mail");
    Engine::systemEditorAction(EnterKey::Send);
    updateDocuments(scene());
    EXPECT_EQ(prevent.hint, "send");
    EXPECT_EQ(submits.count, 1);
    EXPECT_EQ(focused(), element("mail"));
    element("mail")->RemoveEventListener(Rml::EventId::Keydown, &prevent);

    // a textarea's Enter breaks the line
    focus("note");
    Engine::systemTextInput("a");
    Engine::systemEditorAction(EnterKey::Enter);
    Engine::systemTextInput("b");
    EXPECT_EQ(value("note"), "a\nb");
    EXPECT_EQ(focused(), element("note"));

    // a hardware Enter applies the field's own enterkeyhint (D2); the platform's default key
    // carries "" as its hint
    EnterHintRecorder hints;
    element("name")->AddEventListener(Rml::EventId::Keydown, &hints);
    focus("name");
    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    updateDocuments(scene());
    EXPECT_EQ(focused(), element("mail"));
    focus("name");
    Engine::systemEditorAction(EnterKey::Default);
    updateDocuments(scene());
    ASSERT_EQ(hints.hints.size(), 2u);
    EXPECT_EQ(hints.hints[0], "next");
    EXPECT_EQ(hints.hints[1], "next");
    element("name")->RemoveEventListener(Rml::EventId::Keydown, &hints);

    // without a hint, a hardware Enter is today's: no enterKeyHint, the form submitted, the field kept
    EnterHintRecorder plainHints;
    element("plain")->AddEventListener(Rml::EventId::Keydown, &plainHints);
    focus("plain");
    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    ASSERT_EQ(plainHints.hints.size(), 1u);
    EXPECT_EQ(plainHints.hints[0], "<none>");
    EXPECT_EQ(submits.count, 2);
    EXPECT_EQ(focused(), element("plain"));
    element("plain")->RemoveEventListener(Rml::EventId::Keydown, &plainHints);
    element("form")->RemoveEventListener(Rml::EventId::Submit, &submits);
}

TEST_F(TextEntryTest, TraitsChangeWithoutHiding){
    TestSystem& system = TestSystem::get();
    load("text-entry/fields.html");
    const int shows = system.virtualKeyboardShows;
    const int hides = system.virtualKeyboardHides;

    focus("email");
    EXPECT_EQ(system.virtualKeyboardShows, shows + 1);
    EXPECT_EQ(system.virtualKeyboardTraits.keyboard, KeyboardType::Email);
    focus("count");
    EXPECT_EQ(system.virtualKeyboardShows, shows + 2);
    EXPECT_EQ(system.virtualKeyboardTraits.keyboard, KeyboardType::Number);
    EXPECT_EQ(system.virtualKeyboardHides, hides);

    // inputmode="none": the keyboard goes, the field keeps the focus
    focus("hidden-keyboard");
    EXPECT_EQ(system.virtualKeyboardHides, hides + 1);
    EXPECT_EQ(system.virtualKeyboardShows, shows + 2);
    EXPECT_EQ(focused(), element("hidden-keyboard"));
    Engine::systemTextInput("7");
    EXPECT_EQ(value("hidden-keyboard"), "7");

    // blurring hides it once
    focus("email");
    EXPECT_EQ(system.virtualKeyboardShows, shows + 3);
    element("email")->Blur();
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardHides, hides + 2);
    updateDocuments(scene());
    EXPECT_EQ(system.virtualKeyboardHides, hides + 2);
}

// a form unloads its document (the field blurs as it leaves the context) and the game goes to a
// scene without HTML UI before any update: no context flushes the hide the blur left pending, so the
// field going away hides it at once
TEST_F(TextEntryTest, KeyboardHidesWhenTheFocusedFieldsDocumentGoes){
    TestSystem& system = TestSystem::get();
    UIDocument& document = fixture->add<UIDocument>();
    ASSERT_TRUE(document.load("text-entry/fields.html"));
    updateDocuments(scene());
    focus("email");
    ASSERT_TRUE(RmlTextInputHandler::isKeyboardShown());
    const int hides = system.virtualKeyboardHides;
    scene().destroyEntity(document.getEntity());
    fixture.reset();
    EXPECT_EQ(system.virtualKeyboardHides, hides + 1) << "the soft keyboard stayed open";
    EXPECT_FALSE(RmlTextInputHandler::isKeyboardShown());
}

TEST(TextEntryNames, RoundTripTheHtmlValues){
    for (int i = 0; i <= (int)KeyboardType::None; i++){
        KeyboardType parsed = KeyboardType::Text;
        EXPECT_TRUE(keyboardTypeFromName(keyboardTypeName((KeyboardType)i), parsed));
        EXPECT_EQ((int)parsed, i);
    }
    for (int i = 0; i <= (int)EnterKey::Send; i++){
        EnterKey parsed = EnterKey::Send;
        EXPECT_TRUE(enterKeyFromName(i == 0 ? "default" : enterKeyName((EnterKey)i), parsed));
        EXPECT_EQ((int)parsed, i);
    }
    AutoCapitalize capitalize = AutoCapitalize::Words;
    EXPECT_TRUE(autoCapitalizeFromName("on", capitalize));
    EXPECT_EQ(capitalize, AutoCapitalize::Sentences);
    EXPECT_TRUE(autoCapitalizeFromName("NONE", capitalize));
    EXPECT_EQ(capitalize, AutoCapitalize::Off);
    KeyboardType keyboard = KeyboardType::Text;
    EXPECT_FALSE(keyboardTypeFromName("qwerty", keyboard));
    EXPECT_TRUE(keyboardTypeFromName("number", keyboard));
    EXPECT_EQ(keyboard, KeyboardType::Number);
}

// R26: a focused field left idle costs no allocation per frame: the keyboard's per-frame check
// (traits unchanged, the field's text and caret what the keyboard holds) on a keyboard that keeps
// its text (Android, iOS); the time is printed
TEST_F(TextEntryTest, IdleFocusedFieldDoesNotAllocate){
    TestSystem& system = TestSystem::get();
    system.keepsKeyboardText = true;
    load("text-entry/form.html");
    focus("short");
    updateDocuments(scene());
    const int updates = system.virtualKeyboardUpdates;
    RmlTextInputHandler& handler = documentSystem(scene()).getContext()->getTextInputHandler();

    constexpr int frames = 600;
    testsupport::ThreadAllocationCount allocations;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i++)
        handler.flushKeyboard();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / frames;
    std::printf("[bench] text_entry_idle_frame_ns %.1f, %llu allocations in %d frames (budget 0 allocations)\n", ns,
                (unsigned long long)allocations.count(), frames);
    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_EQ(system.virtualKeyboardUpdates, updates) << "idle: nothing sent to the keyboard";
}
