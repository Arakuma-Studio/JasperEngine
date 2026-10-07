// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUICLASSESLUA_H
#define HTMLUICLASSESLUA_H

// Value classes of the Lua element API that only Lua has (C++ calls the UIElement methods
// directly): element.classList, element.style and element.dataset hold the element's handle.

#include "object/ui/UIElement.h"

namespace jasperengine{

    // element.classList: add(...), remove(...), toggle(name [, force]), contains(name)
    struct UIClassList {
        UIElement element;
    };

    // element.style: style.backgroundColor / style["background-color"] read the computed value
    // and write the inline style ("" or nil removes it); getPropertyValue, setProperty,
    // removeProperty
    struct UIStyle {
        UIElement element;
    };

    // element.dataset: dataset.userName is the data-user-name attribute (nil when absent;
    // assigning nil removes it)
    struct UIDataset {
        UIElement element;
    };

}

#endif //HTMLUICLASSESLUA_H
