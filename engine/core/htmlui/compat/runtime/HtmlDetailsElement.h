// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLDETAILSELEMENT_H
#define COMPAT_RUNTIME_HTMLDETAILSELEMENT_H

#include "Export.h"
#include "htmlui/compat/runtime/HtmlListItemElement.h"

#include <RmlUi/Core/Element.h>

namespace jasperengine::htmlui::compat {

    // <details> (phase-2 §3.11.2): reacts to its `open` attribute (the user-agent sheet hides
    // everything but the summary while closed) with a `toggle` event in the next update, as
    // browsers queue it (also for a details created open; parameters newState/oldState
    // "open"/"closed"; it does not bubble);
    // opening one of several details with the same `name` closes the others (an exclusive
    // accordion, HTML 2024).
    class JASPERENGINE_INTERNAL HtmlDetailsElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlDetailsElement, Rml::Element)

        explicit HtmlDetailsElement(const Rml::String& tag);
        ~HtmlDetailsElement() override;

        bool isOpen() const;
        void setOpen(bool open);
        void toggle();

    protected:
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnUpdate() override;

    private:
        bool open = false;
        bool pending = false;        // a toggle event waits for the next update
        bool pendingOld = false;
    };

    // <summary>: the marker of a list item (disclosure-open/closed from the user-agent sheet)
    // and, as the first summary of a <details>, its toggle: click, Enter (key down) and Space
    // (key up) open or close the parent details.
    class JASPERENGINE_INTERNAL HtmlSummaryElement : public HtmlListItemElement {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlSummaryElement, HtmlListItemElement)

        explicit HtmlSummaryElement(const Rml::String& tag);
        ~HtmlSummaryElement() override;

        // the details this summary opens (nullptr when it is not a details' first summary)
        HtmlDetailsElement* getDetails();

    protected:
        void ProcessDefaultAction(Rml::Event& event) override;
        std::string markerTextFor(const std::string& listStyleType) override;
    };

}

#endif //COMPAT_RUNTIME_HTMLDETAILSELEMENT_H
