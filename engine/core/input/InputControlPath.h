// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTCONTROLPATH_H
#define INPUTCONTROLPATH_H

#include "Export.h"
#include "input/InputControlNames.h"

#include <string>

namespace jasperengine {

    // What a control path names (R9 of docs/specs/S22-gameplay.md)
    enum class InputControlKind : int {
        NONE = 0,
        KEY,                // keyboard/<key>
        MOUSE_BUTTON,       // mouse/{left,right,middle,button4..button8}
        MOUSE_DELTA,        // mouse/delta (vec2), mouse/delta/x, mouse/delta/y (component 0, 1)
        MOUSE_SCROLL,       // mouse/scroll (vec2), mouse/scroll/x, mouse/scroll/y
        GAMEPAD_BUTTON,     // gamepad/{a,b,...,dpad_left}
        GAMEPAD_AXIS,       // gamepad/{left_x,...,right_trigger}
        GAMEPAD_STICK,      // gamepad/{left_stick,right_stick} (vec2)
        TOUCH_PRESS,        // touch/primary/press
        TOUCH_POSITION,     // touch/primary/position (vec2)
        TOUCH_DELTA,        // touch/primary/delta (vec2)
        TOUCH_COUNT,        // touch/count
        VIRTUAL             // virtual/<name> (a scalar or vec2 a script sets)
    };

    // A parsed control path: "keyboard/space", "gamepad/left_stick", "mouse/delta/x", ...
    struct JASPERENGINE_API InputControlPath {
        InputControlKind kind = InputControlKind::NONE;
        int index = 0;          // the key, button or axis; GAMEPAD_STICK: 0 left, 1 right
        int component = -1;     // -1 the whole control, 0 x, 1 y (MOUSE_DELTA, MOUSE_SCROLL)
        std::string name;       // VIRTUAL

        // NONE when the path names no control
        static InputControlPath parse(const std::string& path);
        // the canonical path ("" for NONE)
        std::string toString() const;

        bool isValid() const { return kind != InputControlKind::NONE; }
        // a button: pressed or not, with press and release edges
        bool isDigital() const;
        // its value is a vec2 (a stick, a delta, a position, a vec2 virtual control)
        bool isVector2() const;
        // a per-frame motion: no deadzone, no clamp
        bool isDelta() const;
        // the control scheme its device belongs to: "Keyboard&Mouse", "Gamepad", "Touch", "" (virtual)
        const char* deviceGroup() const;

        bool operator==(const InputControlPath& other) const{
            return kind == other.kind && index == other.index && component == other.component && name == other.name;
        }
    };

    // '*' matches any run of characters (rebind exclusions: "mouse/delta*")
    JASPERENGINE_API bool inputPathMatches(const std::string& pattern, const std::string& path);

}

#endif // INPUTCONTROLPATH_H
