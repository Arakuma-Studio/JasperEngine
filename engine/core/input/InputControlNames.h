// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef InputControlNames_h
#define InputControlNames_h

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine {

    // The device a control belongs to (the Input Manager's bindings, Input.getKey("mouse_left"))
    enum class InputDevice {
        Key,            // keyboard key, code = D_KEY_*
        Mouse,          // mouse button, code = D_MOUSE_BUTTON_*
        GamepadButton,  // gamepad button, code = D_GAMEPAD_BUTTON_*
        GamepadAxis     // gamepad axis, code = D_GAMEPAD_AXIS_* (not a button: gamepad_axis bindings)
    };

    struct InputControl {
        InputDevice device = InputDevice::Key;
        int code = -1;

        bool operator==(const InputControl& other) const { return device == other.device && code == other.code; }
        bool operator!=(const InputControl& other) const { return !(*this == other); }
    };

    // The one table of control names, shared by the runtime (Input Actions' control paths and the
    // Input Manager's bindings, Lua), the editor and the MCP tools (send_input, get_input_state).
    // Two spellings of a control are canonical:
    //   path segments (InputControlPath, docs/specs/S22-gameplay.md R9): keys "space", "a", "0",
    //     "left_shift", "f1", "kp_0"; mouse buttons "left", "right", "middle", "button4".."button8";
    //     gamepad buttons "a", "b", "x", "y", "left_bumper", "start", "dpad_up"...; gamepad axes
    //     "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger"
    //     (keyName, mouseButtonName, gamepadButtonName, gamepadAxisName)
    //   control names (the Input Manager's bindings, Input.getKey("mouse_left"), getPressedControl):
    //     keys as above; mouse "mouse_left", "mouse_right", "mouse_middle", "mouse_4".."mouse_8";
    //     gamepad buttons "gamepad_a" ... "gamepad_dpad_left"; gamepad axes as above
    //     (name, mouseButtonControlName, gamepadButtonControlName)
    // Parsing takes every spelling of both, case-insensitive, plus the aliases: a printable
    // character ("a", ";"), "key_"/"d_key_" prefixes, spaces or dashes for underscores, ctrl,
    // esc, return, del, pageup, numpad0...; Unity's Input Manager names ("left ctrl", "mouse 0",
    // "joystick button 0", "[1]") and KeyCode names ("LeftArrow", "Alpha1", "Mouse0"); the pad's
    // cross, circle, square, triangle, lb, rb, select, ls, rs, lt, rt.
    class JASPERENGINE_API InputControlNames {
    public:
        // ---- parsing (every spelling)
        // D_KEY_* of a key name, else D_KEY_UNKNOWN
        static int parseKey(const std::string& name);
        // D_MOUSE_BUTTON_* of "left", "mouse_left", "mouse 0", "button4", "mouse_4"..., else -1
        static int parseMouseButton(const std::string& name);
        // D_GAMEPAD_BUTTON_* of "a", "gamepad_a", "cross", "lb", "dpad_up", "joystick button 0"..., else -1
        static int parseGamepadButton(const std::string& name);
        // D_GAMEPAD_AXIS_* of "left_x", "lt", "x axis"..., else -1
        static int parseGamepadAxis(const std::string& name);
        // a control name: a key, mouse button or gamepad button (an Input Manager binding's
        // positive/negative list, Input.getKey); false when the name is none of them. A gamepad
        // axis name is reported in `error`.
        static bool parseButton(const std::string& name, InputControl& out, std::string* error = nullptr);
        // any control name, gamepad axes included
        static bool parse(const std::string& name, InputControl& out);

        // ---- path segments
        // a key's canonical name ("space", "left_shift"; the code as a number when it has none)
        static std::string keyName(int key);
        static std::string mouseButtonName(int button);       // "left", "button4"
        static std::string gamepadButtonName(int button);     // "a", "dpad_up"
        static std::string gamepadAxisName(int axis);         // "left_x"

        // ---- control names ("" when the code has none)
        static std::string mouseButtonControlName(int button);    // "mouse_left", "mouse_4"
        static std::string gamepadButtonControlName(int button);  // "gamepad_a", "gamepad_dpad_up"
        static std::string name(const InputControl& control);

        // every control name of a device, in a stable display order (the editor's pickers)
        static const std::vector<std::string>& keyNames();
        static const std::vector<std::string>& mouseButtonControlNames();
        static const std::vector<std::string>& gamepadButtonControlNames();
        static const std::vector<std::string>& gamepadAxisNames();
    };

}

#endif /* InputControlNames_h */
