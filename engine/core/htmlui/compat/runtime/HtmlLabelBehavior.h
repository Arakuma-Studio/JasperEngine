// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLLABELBEHAVIOR_H
#define COMPAT_RUNTIME_HTMLLABELBEHAVIOR_H

#include "Export.h"

#include <RmlUi/Core/Element.h>

namespace jasperengine::htmlui::compat {

    // <label> with HTML's activation (phase-2 §3.11.2). RmlUi's label already forwards clicks
    // (for="id", or the first button/input/textarea/progress/select inside), but it stops the
    // label's own click and picks targets HTML does not label (an input type=hidden, any
    // element named by for). This replaces it for every document: the labeled control is the
    // element named by `for` when it is labelable, else the first labelable descendant
    // (button, input other than hidden, meter, output, progress, select, textarea); the click
    // propagates normally and, unless prevented, its default action focuses the control and
    // clicks it (nothing for a disabled control); :hover and :active are mirrored onto the
    // control as RmlUi's label does.
    class JASPERENGINE_INTERNAL HtmlLabelElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlLabelElement, Rml::Element)

        explicit HtmlLabelElement(const Rml::String& tag);
        ~HtmlLabelElement() override;

        // the labeled control (nullptr when none)
        Rml::Element* getControl();
        static bool isLabelable(Rml::Element* element);

    protected:
        void OnPseudoClassChange(const Rml::String& pseudo_class, bool activate) override;
        void ProcessDefaultAction(Rml::Event& event) override;

    private:
        bool forwarding = false;
    };

}

#endif //COMPAT_RUNTIME_HTMLLABELBEHAVIOR_H
