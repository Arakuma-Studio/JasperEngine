// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Android soft keyboard's text (engine/platform/android/AndroidTextInput.h): GameTextInput
// holds UTF-16 units, the component InputField code points. Plain C++, so it runs on any host.

#include "AndroidTextInput.h"

#include <gtest/gtest.h>

#include <string>

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
