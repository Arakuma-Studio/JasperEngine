// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_INPUT_PROMPT_H
#define HTMLUI_INPUT_PROMPT_H

#include <RmlUi/Core/Element.h>

#include <cstdint>

namespace Rml {
    class ElementText;
}

namespace jasperengine{

    // <input-prompt action="Map/Action" scheme="current|touch|gamepad|keyboard-mouse">
    // (input-device-awareness R18): the action's binding for the scheme in use as its text
    // (InputActions::getActionDisplayName), with data-glyph (the glyph key), data-scheme and
    // data-empty (no binding for that scheme). It follows the scheme, the pad's family and
    // rebinding; children the author gave replace the text (the attributes still update).
    class UIInputPrompt : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(UIInputPrompt, Rml::Element)

        explicit UIInputPrompt(const Rml::String& tag);

        // the tag's instancer (HtmlUI::initialize)
        static void registerInstancer();
        static void releaseInstancer();

    protected:
        void OnUpdate() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

    private:
        // what the prompt shows was built for (one compare per frame)
        uint32_t schemeRevision = UINT32_MAX;
        uint32_t bindingRevision = UINT32_MAX;
        bool dirty = true;
        bool authorChildren = false;
        bool checkedChildren = false;
        Rml::ElementText* text = nullptr;   // its own text child (no author children)

        void rebuild();
    };

}

#endif // HTMLUI_INPUT_PROMPT_H
