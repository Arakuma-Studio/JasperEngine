// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef InputControlNames_h
#define InputControlNames_h

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine {

    // The device a control belongs to
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

    // The one table of control names shared by the engine (bindings, Lua), the editor (Project
    // Settings > Input) and MCP (send_input). Canonical names:
    //   keys        a..z, 0..9, space, enter, escape, tab, backspace, left, right, up, down,
    //               left_shift, left_control, left_alt, left_super (right_* too), kp_0..kp_9,
    //               kp_enter, f1..f25, page_up, ... (keyName lists every one)
    //   mouse       mouse_left, mouse_right, mouse_middle, mouse_4..mouse_8
    //   gamepad     gamepad_a, gamepad_b, gamepad_x, gamepad_y, gamepad_left_bumper,
    //               gamepad_right_bumper, gamepad_back, gamepad_start, gamepad_guide,
    //               gamepad_left_thumb, gamepad_right_thumb, gamepad_dpad_up/right/down/left
    //   pad axes    left_x, left_y, right_x, right_y, left_trigger, right_trigger
    // Parsing is case-insensitive and also takes Unity's spellings ("left ctrl", "mouse 0",
    // "joystick button 0", "return", "[1]", KeyCode names such as "LeftArrow", "Alpha1", "Mouse0"),
    // which are normalised to the canonical names.
    class JASPERENGINE_API InputControlNames {
    public:
        // a key, mouse button or gamepad button (a binding's positive/negative list, Input.getKey);
        // false when the name is none of them. A gamepad axis name is reported in `error`.
        static bool parseButton(const std::string& name, InputControl& out, std::string* error = nullptr);
        // any control, gamepad axes included
        static bool parse(const std::string& name, InputControl& out);

        // D_KEY_* of a key name (send_input also takes "shift", "ctrl", "esc", "numpad1"...), else D_KEY_UNKNOWN
        static int parseKey(const std::string& name);
        // D_MOUSE_BUTTON_* of "mouse_left", "left", "mouse 0", "mouse_4"..., else -1
        static int parseMouseButton(const std::string& name);
        // D_GAMEPAD_BUTTON_* of "gamepad_a" or "a", "cross", "lb", "dpad_up"..., else -1
        static int parseGamepadButton(const std::string& name);
        // D_GAMEPAD_AXIS_* of "left_x", "lt", "x axis"..., else -1
        static int parseGamepadAxis(const std::string& name);

        // canonical names ("" when the code has none)
        static std::string keyName(int key);
        static std::string mouseButtonName(int button);
        static std::string gamepadButtonName(int button);
        static std::string gamepadAxisName(int axis);
        static std::string name(const InputControl& control);

        // every canonical name of a device, in a stable display order (the editor's pickers)
        static const std::vector<std::string>& keyNames();
        static const std::vector<std::string>& mouseButtonNames();
        static const std::vector<std::string>& gamepadButtonNames();
        static const std::vector<std::string>& gamepadAxisNames();
    };

}

#endif /* InputControlNames_h */
