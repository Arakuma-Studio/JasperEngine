// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The iOS text field's traits for a text field's (engine/platform/apple/ios/TextTraitsMap.h,
// text-entry spec D4, AC6): plain C++, the UIKit values checked against UIKit in EngineView.mm.

#include "TextTraitsMap.h"

#include <gtest/gtest.h>

namespace IosTextTraitsTest_cpp {

using namespace jasperengine;

namespace {

    TextEntryTraits iosEntry(KeyboardType keyboard, EnterKey enterKey = EnterKey::Default, AutoCapitalize capitalize = AutoCapitalize::Off,
                         bool autocorrect = false, bool secure = false, bool signedNumber = false){
        TextEntryTraits t;
        t.keyboard = keyboard;
        t.enterKey = enterKey;
        t.capitalize = capitalize;
        t.autocorrect = autocorrect;
        t.secure = secure;
        t.signedNumber = signedNumber;
        return t;
    }

}

TEST(IosTextTraits, MapOfTraits){
    using namespace jasperengine::ios;
    struct Row {
        const char* name;
        TextEntryTraits traits;
        int keyboardType, returnKeyType, capitalize, autocorrect;
        bool secure, emptyInputView;
    };
    // the fields of TextEntryTest.TraitsFromAttributes (engine/tests/htmlui/TestTextEntry.cpp)
    const Row rows[] = {
        {"text", iosEntry(KeyboardType::Text), KEYBOARD_DEFAULT, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"email", iosEntry(KeyboardType::Email), KEYBOARD_EMAIL_ADDRESS, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"url", iosEntry(KeyboardType::Url), KEYBOARD_URL, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"tel", iosEntry(KeyboardType::Phone), KEYBOARD_PHONE_PAD, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"search", iosEntry(KeyboardType::Search, EnterKey::Search), KEYBOARD_WEB_SEARCH, RETURN_SEARCH, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"count", iosEntry(KeyboardType::Number), KEYBOARD_NUMBER_PAD, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"amount", iosEntry(KeyboardType::Decimal, EnterKey::Default, AutoCapitalize::Off, false, false, true), KEYBOARD_NUMBERS_AND_PUNCTUATION,
         RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"half", iosEntry(KeyboardType::Decimal), KEYBOARD_DECIMAL_PAD, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"password", iosEntry(KeyboardType::Text, EnterKey::Default, AutoCapitalize::Off, true, true), KEYBOARD_DEFAULT, RETURN_DEFAULT,
         CAPITALIZE_NONE, AUTOCORRECT_NO, true, false},
        {"hidden-keyboard", iosEntry(KeyboardType::None), KEYBOARD_DEFAULT, RETURN_DEFAULT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, true},
        {"mail-send", iosEntry(KeyboardType::Email, EnterKey::Send, AutoCapitalize::Sentences), KEYBOARD_EMAIL_ADDRESS, RETURN_SEND,
         CAPITALIZE_SENTENCES, AUTOCORRECT_NO, false, false},
        {"next", iosEntry(KeyboardType::Text, EnterKey::Next), KEYBOARD_DEFAULT, RETURN_NEXT, CAPITALIZE_NONE, AUTOCORRECT_NO, false, false},
        {"previous", iosEntry(KeyboardType::Text, EnterKey::Previous, AutoCapitalize::Words), KEYBOARD_DEFAULT, RETURN_DEFAULT, CAPITALIZE_WORDS,
         AUTOCORRECT_NO, false, false},
        {"done", iosEntry(KeyboardType::Text, EnterKey::Done, AutoCapitalize::Characters), KEYBOARD_DEFAULT, RETURN_DONE, CAPITALIZE_ALL_CHARACTERS,
         AUTOCORRECT_NO, false, false},
        {"go", iosEntry(KeyboardType::Text, EnterKey::Go, AutoCapitalize::Sentences, true), KEYBOARD_DEFAULT, RETURN_GO, CAPITALIZE_SENTENCES,
         AUTOCORRECT_YES, false, false},
    };
    for (const Row& row : rows){
        const TextTraits out = textTraitsOf(row.traits);
        EXPECT_EQ(out.keyboardType, row.keyboardType) << row.name;
        EXPECT_EQ(out.returnKeyType, row.returnKeyType) << row.name;
        EXPECT_EQ(out.autocapitalizationType, row.capitalize) << row.name;
        EXPECT_EQ(out.autocorrectionType, row.autocorrect) << row.name;
        EXPECT_EQ(out.secureTextEntry, row.secure) << row.name;
        EXPECT_EQ(out.emptyInputView, row.emptyInputView) << row.name;
    }
}

}  // namespace IosTextTraitsTest_cpp
