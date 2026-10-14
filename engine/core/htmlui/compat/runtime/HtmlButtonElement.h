// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLBUTTONELEMENT_H
#define COMPAT_RUNTIME_HTMLBUTTONELEMENT_H

#include "Export.h"

#include <RmlUi/Core/Element.h>

#include <memory>

namespace jasperengine::htmlui::compat {

    // <button> with HTML semantics (phase-2 §3.11.2; RmlUi has no button element): focusable
    // through the user-agent sheet (tab-index: auto); `disabled` <-> :disabled and no click
    // (the click is stopped at the button before any listener of it runs); Enter (key down) and
    // Space (key up) activate it like a click; the click's default action by `type`: submit
    // (the default) submits its form (ElementForm::Submit(name, value); form="id" names a form
    // elsewhere), reset resets the form's controls, button does nothing. autofocus focuses it
    // after load.
    class JASPERENGINE_INTERNAL HtmlButtonElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlButtonElement, Rml::Element)

        explicit HtmlButtonElement(const Rml::String& tag);
        ~HtmlButtonElement() override;

        // "submit", "reset" or "button" (unknown types are submit, as in HTML)
        std::string getType() const;

        // laid out as Chromium lays out a button: its content centered vertically as one block
        // (the fork's P4-G2)
        bool HasButtonLayout() const override;
        // CSS `appearance` is not none (the --html-appearance companion): its display types as
        // Chromium's theme computes them (the fork's P4-G12)
        bool HasNativeAppearance() const override;

    protected:
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnUpdate() override;
        void ProcessDefaultAction(Rml::Event& event) override;

    private:
        class DisabledGuard;
        std::unique_ptr<DisabledGuard> guard;
        bool updated = false;
    };

}

#endif //COMPAT_RUNTIME_HTMLBUTTONELEMENT_H
