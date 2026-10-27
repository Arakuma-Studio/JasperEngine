// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTVIRTUALCONTROLS_H
#define INPUTVIRTUALCONTROLS_H

// On-screen controls (docs/specs/S22-gameplay.md, Input Actions' virtual/<name> paths): an HTML UI
// element made a stick or a button feeds InputActions::setVirtual while a finger (or the mouse)
// holds it. Each finger keeps the element it pressed until it lifts, so a stick and a button work
// at once.
//
//   <div id="move" class="stick"></div>   InputActions.bindVirtualStick(doc:getElementById("move"), "move")
//   <div id="jump" class="button"></div>  InputActions.bindVirtualButton(doc:getElementById("jump"), "jump")

#include "Export.h"
#include "object/ui/UIElement.h"

#include <string>

namespace jasperengine {

    class JASPERENGINE_API InputVirtualControls {
    public:
        // a stick: the press and drag from the element's centre, by its half size (the
        // shorter one), up positive, at most 1 long, as virtual/<name> (vec2); (0, 0) on release
        static bool bindStick(UIElement element, const std::string& name);
        // a button: 1 as virtual/<name> while pressed, 0 on release
        static bool bindButton(UIElement element, const std::string& name);
        // the element feeds nothing any more (its control reads 0)
        static void unbind(UIElement element);
    };

}

#endif // INPUTVIRTUALCONTROLS_H
