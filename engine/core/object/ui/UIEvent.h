// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIEVENT_H
#define UIEVENT_H

#include "Export.h"
#include "UIDataTransfer.h"
#include "UIElement.h"
#include "UIValue.h"

#include <memory>
#include <string>

namespace jasperengine{

    // defined by p1-lua (UIEvent.cpp / HtmlUIScriptInternal.h). Not "UIEventState": Scene.h
    // has an enum of that name
    struct HtmlUIEventState;

    // An event reaching a listener of a document element (browser names: click, keydown,
    // input, change, wheel ...). Valid only while the listener runs; afterwards every getter
    // returns a default.
    class JASPERENGINE_API UIEvent {
    public:
        bool isValid() const;                         // the dispatch is still running
        std::string getType() const;                  // DOM name
        UIElement getTarget() const;  UIElement getCurrentTarget() const;
        int getEventPhase() const;                    // 1 capture, 2 target, 3 bubble
        void stopPropagation(); void stopImmediatePropagation();
        void preventDefault(); bool isDefaultPrevented() const;

        // keys: DOM key ("a", "A", "Enter") and code ("KeyA"), the engine key code (Input.KEY_*).
        // A gamepad's keys (p4-input): key "ArrowUp" ... (d-pad, left stick), "Enter" (A), "Escape" (B),
        // else the code; code "GamepadDPadUp", "GamepadA", ...; keyCode the keyboard key it stands for
        // (or 0); getGamepad() the pad's id (Input), -1 for the keyboard
        std::string getKey() const; std::string getCode() const; int getKeyCode() const; bool isRepeat() const;
        int getGamepad() const;
        bool getCtrlKey() const; bool getShiftKey() const; bool getAltKey() const; bool getMetaKey() const;

        // pointer position in document px; button in DOM numbering (0 left, 1 middle, 2 right)
        float getClientX() const; float getClientY() const; int getButton() const;

        float getDeltaX() const; float getDeltaY() const;   // wheel steps, positive down/right
        // Pointer Events (ui-multitouch; also on the compatibility mouse events and click): pointerId
        // 1 the mouse (every button), fingerId + 2 a finger; pointerType "mouse" / "touch" / "pen";
        // buttons the DOM bitmask (1 left, 2 right, 4 middle); pressure 0.5 while pressed; width /
        // height the contact size; fingerId the Input finger id (-1 for the mouse). 0 / "" / false
        // on events of no pointer
        int getPointerId() const; std::string getPointerType() const; bool isPrimary() const; int getButtons() const;
        float getPressure() const; float getWidth() const; float getHeight() const; int getFingerId() const;
        std::string getData() const;                  // textinput / IME text
        UIValue getDetail() const;                    // custom events (dispatchEvent)

        // CSS animation and transition events (animationstart, ..., transitionend): the keyframes
        // name, the transitioned property, and the seconds the animation or transition had run
        std::string getAnimationName() const; std::string getPropertyName() const; float getElapsedTime() const;

        // drag events (S17): the drag's data (invalid on other events), and over a virtual view
        // where among its rows the pointer is: the 1-based item (0 elsewhere) and "before", "after"
        // or "on" (the row's top and bottom quarters, its middle)
        UIDataTransfer getDataTransfer() const;
        int getDropIndex() const; std::string getDropPosition() const;

    private:
        friend struct HtmlUIScriptAccess;   // p1-lua internals (HtmlUIScriptInternal.h) create and read the state
        std::shared_ptr<HtmlUIEventState> state;
    };

}

#endif //UIEVENT_H
