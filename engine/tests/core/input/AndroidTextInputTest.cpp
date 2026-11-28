// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Android soft keyboard's text (engine/platform/android/AndroidTextInput.h): GameTextInput
// holds UTF-16 units, the component InputField code points. Plain C++, so it runs on any host.

#include "AndroidTextInput.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace AndroidTextInputTest_cpp {

using namespace jasperengine::android;

namespace {

    // the UTF-16 units GameTextInput reports for a text
    std::u16string unitsOf(const std::u32string& text) {
        std::u16string units;
        for (char32_t cp : text) {
            if (cp > 0xFFFF) {
                const char32_t value = cp - 0x10000;
                units.push_back(static_cast<char16_t>(0xD800 + (value >> 10)));
                units.push_back(static_cast<char16_t>(0xDC00 + (value & 0x3FF)));
            } else {
                units.push_back(static_cast<char16_t>(cp));
            }
        }
        return units;
    }

}

TEST(AndroidTextInput, DiffCountsCodePoints) {
    ComponentTextInput input;
    input.reset(U"a\U0001F600");

    // typing after an emoji the field already holds: only the new character
    TextEdit typed = input.update(unitsOf(U"a\U0001F600b"));
    EXPECT_EQ(typed.backspaces, 0u);
    EXPECT_EQ(typed.typed, U"b");

    // deleting the emoji: one backspace, the "a" before it stays
    TextEdit erased = input.update(unitsOf(U"a"));
    EXPECT_EQ(erased.backspaces, 2u) << "b and the emoji";
    EXPECT_TRUE(erased.typed.empty());
    EXPECT_EQ(input.current(), U"a");

    TextEdit emoji = input.update(unitsOf(U"a\U0001F600"));
    EXPECT_EQ(emoji.backspaces, 0u);
    EXPECT_EQ(emoji.typed, U"\U0001F600") << "the pair arrives as one character";
    TextEdit removed = input.update(unitsOf(U"a"));
    EXPECT_EQ(removed.backspaces, 1u);
}

TEST(AndroidTextInput, ShowSoftInputAndDiffAgree) {
    // showSoftInput gets the field's text as a wide string; the keyboard then reports the same
    // text in modified UTF-8: the first report changes nothing
    ComponentTextInput input;
    const std::wstring field = L"\U0001F600x";
    input.reset(wideToCodepoints(field));

    int32_t utf16Length = 0;
    const std::string modified = codepointsToModifiedUtf8(wideToCodepoints(field), utf16Length);
    EXPECT_EQ(utf16Length, 3);
    const TextEdit same = input.update(decodeModifiedUtf8(modified.data(), static_cast<int32_t>(modified.size())));
    EXPECT_EQ(same.backspaces, 0u);
    EXPECT_TRUE(same.typed.empty());
    EXPECT_EQ(input.current(), U"\U0001F600x");
}

TEST(AndroidTextInput, DecodesModifiedAndPlainUtf8) {
    // modified UTF-8: the pair as two 3-byte surrogates, NUL as C0 80
    const std::string modified = "a\xED\xA0\xBD\xED\xB8\x80\xC0\x80";
    EXPECT_EQ(decodeModifiedUtf8(modified.data(), static_cast<int32_t>(modified.size())),
              (std::u16string{u'a', 0xD83D, 0xDE00, 0}));
    // what ART hands over for text outside the BMP: plain 4-byte UTF-8
    const std::string plain = "\xF0\x9F\x98\x80";
    EXPECT_EQ(decodeModifiedUtf8(plain.data(), static_cast<int32_t>(plain.size())), (std::u16string{0xD83D, 0xDE00}));
    // a malformed byte is U+FFFD
    const std::string broken = "\xFFz";
    EXPECT_EQ(decodeModifiedUtf8(broken.data(), static_cast<int32_t>(broken.size())), (std::u16string{0xFFFD, u'z'}));

    std::u32string text;
    std::vector<size_t> unitToCodepoint;
    utf16ToCodepoints(std::u16string{u'a', 0xD83D, 0xDE00, 0xDC00}, text, unitToCodepoint);
    EXPECT_EQ(text, (std::u32string{U'a', 0x1F600, 0xFFFD})) << "a lone low surrogate is U+FFFD";
    EXPECT_EQ(unitToCodepoint, (std::vector<size_t>{0, 1, 2, 2, 3}));
    EXPECT_EQ(codepointsToUtf8(U"a\U0001F600", 0, 2), "a\xF0\x9F\x98\x80");
}


TEST(AndroidTextInput, EditorInfoOfTraits) {
    using jasperengine::AutoCapitalize;
    using jasperengine::EnterKey;
    using jasperengine::KeyboardType;
    using jasperengine::TextEntryTraits;
    constexpr int32_t flags = IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_EXTRACT_UI;
    auto make = [](KeyboardType keyboard, EnterKey enterKey = EnterKey::Default, AutoCapitalize capitalize = AutoCapitalize::Off,
                   bool autocorrect = false, bool secure = false, bool multiline = false, bool signedNumber = false) {
        TextEntryTraits t;
        t.keyboard = keyboard;
        t.enterKey = enterKey;
        t.capitalize = capitalize;
        t.autocorrect = autocorrect;
        t.secure = secure;
        t.multiline = multiline;
        t.signedNumber = signedNumber;
        return t;
    };
    struct Row {
        const char* name;
        TextEntryTraits traits;
        int32_t inputType;
        int32_t imeOptions;
    };
    // the fields of TextEntryTest.TraitsFromAttributes (engine/tests/htmlui/TestTextEntry.cpp)
    const Row rows[] = {
        {"text", make(KeyboardType::Text), 0x1, 0x12000000},
        {"email", make(KeyboardType::Email), 0x21, 0x12000000},
        {"url", make(KeyboardType::Url), 0x11, 0x12000000},
        {"tel", make(KeyboardType::Phone), 0x3, 0x12000000},
        {"search", make(KeyboardType::Search, EnterKey::Search), 0x1, IME_ACTION_SEARCH | flags},
        {"count", make(KeyboardType::Number), 0x2, flags},
        {"amount", make(KeyboardType::Decimal, EnterKey::Default, AutoCapitalize::Off, false, false, false, true), 0x3002, flags},
        {"half", make(KeyboardType::Decimal), 0x2002, flags},
        {"password", make(KeyboardType::Text, EnterKey::Default, AutoCapitalize::Off, false, true), 0x80081, flags},
        {"pin", make(KeyboardType::Number, EnterKey::Default, AutoCapitalize::Off, false, true), 0x12, flags},
        {"notes", make(KeyboardType::Text, EnterKey::Enter, AutoCapitalize::Off, false, false, true), 0x20001, IME_ACTION_NONE | flags},
        {"hidden-keyboard", make(KeyboardType::None), 0x1, flags},
        {"mail-send", make(KeyboardType::Email, EnterKey::Send, AutoCapitalize::Sentences), 0x4021, 0x12000004},
        {"next", make(KeyboardType::Text, EnterKey::Next), 0x1, IME_ACTION_NEXT | flags},
        {"previous", make(KeyboardType::Text, EnterKey::Previous, AutoCapitalize::Words), 0x2001, IME_ACTION_PREVIOUS | flags},
        {"done", make(KeyboardType::Text, EnterKey::Done, AutoCapitalize::Characters), 0x1001, IME_ACTION_DONE | flags},
        {"go", make(KeyboardType::Text, EnterKey::Go, AutoCapitalize::Sentences, true), 0xC001, IME_ACTION_GO | flags},
        {"message", make(KeyboardType::Text, EnterKey::Send, AutoCapitalize::Off, true, false, true), 0x28001, IME_ACTION_SEND | flags},
    };
    for (const Row& row : rows) {
        const EditorInfo info = editorInfoOf(row.traits);
        EXPECT_EQ(info.inputType, row.inputType) << row.name;
        EXPECT_EQ(info.imeOptions, row.imeOptions) << row.name;
        // the IME reports the action it was given: the same enter key comes back
        EXPECT_EQ(enterKeyOfImeAction(info.imeOptions), row.traits.enterKey) << row.name;
    }
    // the action of AC7's e-mail field
    EXPECT_EQ(editorInfoOf(make(KeyboardType::Email, EnterKey::Send)), (EditorInfo{0x21, 0x12000004}));
}

// Gboard finishes the composing word, then sends the action key: the field gets the word before
// Send or Done acts on it (and before Next moves the focus away from it)
TEST(AndroidTextInput, ActionKeyFollowsTheTextOfItsPass) {
    std::vector<std::string> order;
    bool textPending = true;
    editorActionWithPendingText(textPending, [&] { order.push_back("text"); }, [&] { order.push_back("action"); });
    ASSERT_EQ(order.size(), 2u) << "the pass's text did not reach the field before the action";
    EXPECT_EQ(order[0], "text");
    EXPECT_EQ(order[1], "action");
    EXPECT_FALSE(textPending) << "the rest of the pass does not apply it again";

    // nothing typed since: the action alone
    order.clear();
    editorActionWithPendingText(textPending, [&] { order.push_back("text"); }, [&] { order.push_back("action"); });
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "action");
}

// the cache belongs to one activity: a recreated activity's IME starts from its defaults, so the
// first field's EditorInfo (IME_FLAG_NO_FULLSCREEN with it) is set again even when it equals the
// one the old activity had
TEST(AndroidTextInput, EditorInfoCacheIsPerActivity) {
    const EditorInfo text = editorInfoOf(jasperengine::TextEntryTraits());
    {
        ImeEditorInfoCache first;
        EXPECT_TRUE(first.changes(text));
        EXPECT_FALSE(first.changes(text)) << "the same kind of field keeps the keyboard";
    }
    ImeEditorInfoCache recreated;
    EXPECT_TRUE(recreated.changes(text)) << "a recreated activity never got the field's EditorInfo";
}

}  // namespace AndroidTextInputTest_cpp
