// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTTRAITSMAP_H
#define TEXTTRAITSMAP_H

// The UITextInputTraits of a text field's traits (text-entry spec D4, R23), without UIKit: the
// UIKit enum values are constants here, checked against UIKit by static_asserts in EngineView.mm.
// Plain C++, so the core tests run it on any host (engine/tests/core/input/IosTextTraitsTest.cpp).

#include "input/TextEntry.h"

namespace jasperengine::ios {

    // UIKeyboardType
    constexpr int KEYBOARD_DEFAULT = 0;
    constexpr int KEYBOARD_NUMBERS_AND_PUNCTUATION = 2;
    constexpr int KEYBOARD_URL = 3;
    constexpr int KEYBOARD_NUMBER_PAD = 4;
    constexpr int KEYBOARD_PHONE_PAD = 5;
    constexpr int KEYBOARD_EMAIL_ADDRESS = 7;
    constexpr int KEYBOARD_DECIMAL_PAD = 8;
    constexpr int KEYBOARD_WEB_SEARCH = 10;
    // UIReturnKeyType
    constexpr int RETURN_DEFAULT = 0;
    constexpr int RETURN_GO = 1;
    constexpr int RETURN_NEXT = 4;
    constexpr int RETURN_SEARCH = 6;
    constexpr int RETURN_SEND = 7;
    constexpr int RETURN_DONE = 9;
    // UITextAutocapitalizationType
    constexpr int CAPITALIZE_NONE = 0;
    constexpr int CAPITALIZE_WORDS = 1;
    constexpr int CAPITALIZE_SENTENCES = 2;
    constexpr int CAPITALIZE_ALL_CHARACTERS = 3;
    // UITextAutocorrectionType
    constexpr int AUTOCORRECT_NO = 1;
    constexpr int AUTOCORRECT_YES = 2;

    struct TextTraits {
        int keyboardType = KEYBOARD_DEFAULT;
        int returnKeyType = RETURN_DEFAULT;
        int autocapitalizationType = CAPITALIZE_NONE;
        int autocorrectionType = AUTOCORRECT_NO;
        bool secureTextEntry = false;
        // KeyboardType::None: an empty inputView, so no keyboard comes up
        bool emptyInputView = false;
    };

    inline TextTraits textTraitsOf(const TextEntryTraits& traits) {
        TextTraits out;
        switch (traits.keyboard) {
            case KeyboardType::Number: out.keyboardType = KEYBOARD_NUMBER_PAD; break;
            // the decimal pad has no minus key
            case KeyboardType::Decimal: out.keyboardType = traits.signedNumber ? KEYBOARD_NUMBERS_AND_PUNCTUATION : KEYBOARD_DECIMAL_PAD; break;
            case KeyboardType::Phone: out.keyboardType = KEYBOARD_PHONE_PAD; break;
            case KeyboardType::Email: out.keyboardType = KEYBOARD_EMAIL_ADDRESS; break;
            case KeyboardType::Url: out.keyboardType = KEYBOARD_URL; break;
            case KeyboardType::Search: out.keyboardType = KEYBOARD_WEB_SEARCH; break;
            case KeyboardType::None: out.emptyInputView = true; break;
            case KeyboardType::Text: break;
        }
        switch (traits.enterKey) {
            case EnterKey::Go: out.returnKeyType = RETURN_GO; break;
            case EnterKey::Next: out.returnKeyType = RETURN_NEXT; break;
            case EnterKey::Search: out.returnKeyType = RETURN_SEARCH; break;
            case EnterKey::Send: out.returnKeyType = RETURN_SEND; break;
            case EnterKey::Done: out.returnKeyType = RETURN_DONE; break;
            // iOS has no Previous key; Enter and Default are its Return
            case EnterKey::Previous: case EnterKey::Enter: case EnterKey::Default: break;
        }
        switch (traits.capitalize) {
            case AutoCapitalize::Sentences: out.autocapitalizationType = CAPITALIZE_SENTENCES; break;
            case AutoCapitalize::Words: out.autocapitalizationType = CAPITALIZE_WORDS; break;
            case AutoCapitalize::Characters: out.autocapitalizationType = CAPITALIZE_ALL_CHARACTERS; break;
            case AutoCapitalize::Off: break;
        }
        out.autocorrectionType = (traits.autocorrect && !traits.secure) ? AUTOCORRECT_YES : AUTOCORRECT_NO;
        out.secureTextEntry = traits.secure;
        return out;
    }

}

#endif // TEXTTRAITSMAP_H
