// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEXTENTRYTRAITS_H
#define HTMLUI_TEXTENTRYTRAITS_H

// The soft keyboard of a document's text field (text-entry spec R2, R3, D1, D2): its traits from the
// HTML attributes, and what the action key (Enter with an enterkeyhint) does.

#include "Export.h"
#include "input/TextEntry.h"

namespace Rml {
    class Element;
}

namespace jasperengine::htmlui {

    // <input> of a text type (text, password, search, email, url, tel, number) or <textarea>
    JASPERENGINE_API bool isTextEntryField(Rml::Element* element);

    // The traits of a text field (.html and .rml read the same attributes): `type`, `inputmode`,
    // `enterkeyhint`, `autocapitalize`, `autocorrect`, `maxlength`, <textarea>. Allocation free.
    JASPERENGINE_API TextEntryTraits textEntryTraitsOf(Rml::Element* field);

    // the field's own enterkeyhint, when it names one (a hardware Enter applies it too, D2)
    JASPERENGINE_INTERNAL bool explicitEnterKeyHint(Rml::Element* field, EnterKey& key);

    // The soft keyboard's action key being delivered as Enter (Engine::systemEditorAction): its hint,
    // null at any other time
    JASPERENGINE_INTERNAL const EnterKey* actionKeyInFlight();
    class JASPERENGINE_API ActionKeyScope {
    public:
        explicit ActionKeyScope(EnterKey key);
        ~ActionKeyScope();
        ActionKeyScope(const ActionKeyScope&) = delete;
        ActionKeyScope& operator=(const ActionKeyScope&) = delete;
    private:
        const EnterKey* previous;
        EnterKey key;
    };

    // What an Enter nobody prevented does on a field, by its hint (R3): next / previous focus the
    // next / previous text field in tab order (none: blur), done blurs, the others submit an
    // <input>'s form implicitly and blur it, or break the line of a <textarea>
    JASPERENGINE_INTERNAL void runEnterKeyAction(Rml::Element* field, EnterKey key);

}

#endif //HTMLUI_TEXTENTRYTRAITS_H
