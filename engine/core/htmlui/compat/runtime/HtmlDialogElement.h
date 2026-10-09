// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLDIALOGELEMENT_H
#define COMPAT_RUNTIME_HTMLDIALOGELEMENT_H

#include "Export.h"

#include <RmlUi/Core/Element.h>

#include <string>

namespace jasperengine::htmlui::compat {

    // <dialog> (phase-2 §3.11.2): the `open` attribute shows it (user-agent sheet). The DOM
    // methods (the Lua hook H5 calls them): show() sets open; showModal() sets open, marks the
    // dialog modal and focuses its autofocus element or first focusable descendant (or the
    // dialog); close(returnValue) removes open, keeps the return value and dispatches `close`
    // in the next update (HTML queues it). Escape while a modal dialog has the focus dispatches
    // a cancelable `cancel` and closes it unless prevented. No backdrop and no focus trap
    // (HTMLUI-HTML-0025 at load).
    class JASPERENGINE_INTERNAL HtmlDialogElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlDialogElement, Rml::Element)

        explicit HtmlDialogElement(const Rml::String& tag);
        ~HtmlDialogElement() override;

        void show();
        void showModal();
        void close(const std::string& returnValue = std::string());
        bool isOpen() const;
        bool isModal() const { return modal; }
        const std::string& getReturnValue() const { return returnValue; }
        void setReturnValue(const std::string& value) { returnValue = value; }

    protected:
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnUpdate() override;
        void ProcessDefaultAction(Rml::Event& event) override;

    private:
        bool modal = false;
        bool closePending = false;
        std::string returnValue;
    };

}

#endif //COMPAT_RUNTIME_HTMLDIALOGELEMENT_H
