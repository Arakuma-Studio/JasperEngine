// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIELEMENTINTERNAL_H
#define UIELEMENTINTERNAL_H

// Engine-internal (includes RmlUi): what is behind a UIElement handle. Engine code that holds
// UIElements and needs their RmlUi element (anime targets, phase-2 hooks, inspectors) includes
// this; public headers never do.

#include "UIElement.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <memory>

namespace jasperengine{

    struct UIElementHandle{
        Rml::ObserverPtr<Rml::Element> element;
        // shared by every handle of the element (HtmlUIElementRegistry::token): a detached tree
        // (createElement, removeChild ...) stays alive while a handle of one of its elements does
        std::shared_ptr<int> token;
    };

    struct UIElementAccess{
        // the RmlUi element, nullptr once the handle is invalid (UIElement::isValid)
        static Rml::Element* get(const UIElement& element){ return element.get(); }
        // a weak handle to element (an invalid UIElement for nullptr)
        static UIElement wrap(Rml::Element* element){ return UIElement::wrap(element); }
        // the hidden template RmlUi keeps after the rows of a data-for (the rows lose the attribute):
        // DOM queries, child lists and the inspector skip it, as the rows are the elements the author
        // sees (p4-input; the fork's structural selectors skip it too, P4-5)
        static bool isDataForTemplate(const Rml::Element* element){ return element && element->HasAttribute("data-for"); }
    };

}

#endif //UIELEMENTINTERNAL_H
