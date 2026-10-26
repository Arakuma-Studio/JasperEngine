// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTCONTROLPATH_H
#define INPUTCONTROLPATH_H

#include "Export.h"

#include <string>

namespace jasperengine {

    // The names of the controls, one table for the runtime (InputActions' control paths), the
    // editor and the MCP tools (send_input, get_input_state). Names are lowercase snake_case:
    // keys "space", "a", "0", "left_shift", "f1", "kp_0"; mouse buttons "left", "right",
    // "middle", "button4".."button8"; gamepad buttons "a", "b", "x", "y", "left_bumper",
    // "start", "dpad_up"...; gamepad axes "left_x", "left_y", "right_x", "right_y",
    // "left_trigger", "right_trigger".
    class JASPERENGINE_API InputControlNames {
    public:
        // A key's canonical name; parseKey also takes a printable character ("a", ";"),
        // "key_"/"d_key_" prefixes, spaces or dashes for underscores, any case, and the
        // aliases ctrl, esc, return, del, pageup, numpad0... D_KEY_UNKNOWN for none.
        static int parseKey(const std::string& name);
        static std::string keyName(int key);

        // -1 for none
        static int parseMouseButton(const std::string& name);
        static std::string mouseButtonName(int button);

        // -1 for none; also cross, circle, square, triangle, lb, rb, select, ls, rs
        static int parseGamepadButton(const std::string& name);
        static std::string gamepadButtonName(int button);

        // -1 for none; also lt, rt
        static int parseGamepadAxis(const std::string& name);
        static std::string gamepadAxisName(int axis);
    };

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
