// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIEVENT_H
#define UIEVENT_H

#include "Export.h"
#include "UIElement.h"
#include "UIValue.h"

#include <memory>
#include <string>

namespace jasperengine{

    // defined by p1-lua (UIEvent.cpp / HtmlUIScriptInternal.h). Not "UIEventState": Scene.h
    // has an enum of that name
    struct HtmlUIEventState;

    // An event being dispatched to a document element (browser names, phase 1 §3.9.3).
    // Valid only during dispatch; afterwards every getter returns defaults (defined by p1-lua)
    class JASPERENGINE_API UIEvent {
    public:
        bool isValid() const;
        std::string getType() const;                  // DOM name
        UIElement getTarget() const;  UIElement getCurrentTarget() const;
        int getEventPhase() const;                    // 1 capture, 2 target, 3 bubble
        void stopPropagation(); void stopImmediatePropagation();
        void preventDefault(); bool isDefaultPrevented() const;
        std::string getKey() const; std::string getCode() const; int getKeyCode() const; bool isRepeat() const;
        bool getCtrlKey() const; bool getShiftKey() const; bool getAltKey() const; bool getMetaKey() const;
        float getClientX() const; float getClientY() const; int getButton() const;   // DOM button numbering
        float getDeltaX() const; float getDeltaY() const;
        std::string getData() const;                  // textinput / IME
        UIValue getDetail() const;                    // custom events (dispatchEvent)

    private:
        friend struct HtmlUIScriptAccess;   // p1-lua internals (HtmlUIScriptInternal.h) create and read the state
        std::shared_ptr<HtmlUIEventState> state;
    };

}

#endif //UIEVENT_H
