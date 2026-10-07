// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIELEMENTINTERNAL_H
#define UIELEMENTINTERNAL_H

// Engine-internal (includes RmlUi): what is behind a UIElement handle. Engine code that holds
// UIElements and needs their RmlUi element (anime targets, phase-2 hooks, inspectors) includes
// this; public headers never do.

#include "UIElement.h"

#include <RmlUi/Core/ObserverPtr.h>

namespace Rml {
    class Element;
}

namespace jasperengine{

    // p1-lua adds the detached-pool token here (phase 1 §3.9.2)
    struct UIElementHandle{
        Rml::ObserverPtr<Rml::Element> element;
    };

    struct UIElementAccess{
        // the RmlUi element, nullptr once the handle is invalid (UIElement::isValid)
        static Rml::Element* get(const UIElement& element){ return element.get(); }
        // a weak handle to element (an invalid UIElement for nullptr)
        static UIElement wrap(Rml::Element* element){ return UIElement::wrap(element); }
    };

}

#endif //UIELEMENTINTERNAL_H
