// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTENTRY_H
#define TEXTENTRY_H

// What a text field asks of the platform's soft keyboard (docs/specs/text-entry-gamepad-cursor-back-button.md
// part A, docs/mcp/guide/html-ui.md "Touch, text fields and the cursor"): the keyboard's layout, its
// action key, capitalisation, correction and length. Derived per field (htmlui::textEntryTraitsOf, the
// component InputField's Keyboard Type) and mapped by each platform (AndroidTextInput.h editorInfoOf,
// ios/TextTraitsMap.h, emscripten/WebTextEntry.h). Desktop platforms ignore them.

#include "Export.h"

#include <cstdint>
#include <string_view>

namespace jasperengine {

    // None: no soft keyboard (inputmode="none"); the field still takes hardware keys
    enum class KeyboardType : uint8_t { Text, Number, Decimal, Phone, Email, Url, Search, None };
    // the action key (HTML enterkeyhint); Default lets the platform choose
    enum class EnterKey : uint8_t { Default, Enter, Done, Go, Next, Previous, Search, Send };
    enum class AutoCapitalize : uint8_t { Off, Sentences, Words, Characters };

    struct TextEntryTraits {
        KeyboardType keyboard = KeyboardType::Text;
        EnterKey enterKey = EnterKey::Default;
        AutoCapitalize capitalize = AutoCapitalize::Off;
        bool autocorrect = false;
        bool secure = false;
        bool multiline = false;
        // a Decimal keyboard with a sign key (type=number with no minimum or a negative one)
        bool signedNumber = false;
        // code points; -1 none
        int maxLength = -1;
        bool operator==(const TextEntryTraits&) const = default;
    };

    // The HTML names ("email", "send", "words"): the Lua options, MCP, the web textarea. Text is
    // "text", Default "" (no enterkeyhint); the readers take the HTML values, case-insensitively, and
    // autoCapitalizeFromName also "none" (Off) and "on" (Sentences).
    JASPERENGINE_API const char* keyboardTypeName(KeyboardType type);
    JASPERENGINE_API bool keyboardTypeFromName(std::string_view name, KeyboardType& type);
    JASPERENGINE_API const char* enterKeyName(EnterKey key);
    JASPERENGINE_API bool enterKeyFromName(std::string_view name, EnterKey& key);
    JASPERENGINE_API const char* autoCapitalizeName(AutoCapitalize capitalize);
    JASPERENGINE_API bool autoCapitalizeFromName(std::string_view name, AutoCapitalize& capitalize);

}

#endif //TEXTENTRY_H
