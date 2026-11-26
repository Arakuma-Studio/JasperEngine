// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef WEBTEXTENTRY_H
#define WEBTEXTENTRY_H

// The hidden IME textarea's attributes for a text field's traits (text-entry spec D4, R24). Plain
// C++ without Emscripten headers, so the unit tests read it (engine/tests/core/input/WebKeyMapTest.cpp).
// autocorrect and spellcheck stay off: the textarea is cleared after each commit, so a suggestion
// has no text to work on.

#include "input/TextEntry.h"

namespace jasperengine::web {

    struct TextAreaAttributes {
        const char* inputmode;
        const char* enterkeyhint;    // "": the attribute removed (the browser's default key)
        const char* autocapitalize;
    };

    inline TextAreaAttributes textAreaAttributesOf(const TextEntryTraits& traits) {
        return {keyboardTypeName(traits.keyboard), enterKeyName(traits.enterKey), autoCapitalizeName(traits.capitalize)};
    }

    // a soft keyboard's line break (beforeinput insertLineBreak) is the field's action key, except
    // where the key is Enter itself (a textarea), which stays the Enter key it was
    inline bool lineBreakIsActionKey(const TextEntryTraits& traits) {
        return traits.enterKey != EnterKey::Enter;
    }

}

#endif // WEBTEXTENTRY_H
