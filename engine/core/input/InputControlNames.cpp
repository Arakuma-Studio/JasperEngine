// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputControlNames.h"

#include "Input.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

using namespace jasperengine;

namespace {

    std::string lowerTrimmed(const std::string& value) {
        size_t start = 0;
        size_t end = value.size();
        while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) start++;
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
        std::string out = value.substr(start, end - start);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // "left ctrl", "Left-Ctrl", "LEFT_CTRL" -> "left_ctrl"
    std::string underscored(const std::string& lower) {
        std::string out;
        for (char c : lower) {
            if (c == ' ' || c == '-' || c == '_') {
                if (!out.empty() && out.back() != '_') out += '_';
            } else {
                out += c;
            }
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return out;
    }

    // "left_ctrl" -> "leftctrl" (KeyCode names: LeftControl, UpArrow, Alpha1...)
    std::string compact(const std::string& lower) {
        std::string out;
        for (char c : lower) {
            if (c != ' ' && c != '-' && c != '_') out += c;
        }
        return out;
    }

    bool allDigits(const std::string& text, size_t from = 0) {
        if (text.size() <= from) return false;
        return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(from), text.end(),
                           [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    }

    struct KeyEntry { const char* name; int key; };

    // canonical key names, in the editor's picker order
    const KeyEntry canonicalKeys[] = {
        {"space", D_KEY_SPACE}, {"enter", D_KEY_ENTER}, {"escape", D_KEY_ESCAPE}, {"tab", D_KEY_TAB},
        {"backspace", D_KEY_BACKSPACE}, {"left", D_KEY_LEFT}, {"right", D_KEY_RIGHT}, {"up", D_KEY_UP},
        {"down", D_KEY_DOWN}, {"left_shift", D_KEY_LEFT_SHIFT}, {"right_shift", D_KEY_RIGHT_SHIFT},
        {"left_control", D_KEY_LEFT_CONTROL}, {"right_control", D_KEY_RIGHT_CONTROL},
        {"left_alt", D_KEY_LEFT_ALT}, {"right_alt", D_KEY_RIGHT_ALT}, {"left_super", D_KEY_LEFT_SUPER},
        {"right_super", D_KEY_RIGHT_SUPER}, {"menu", D_KEY_MENU},
        {"insert", D_KEY_INSERT}, {"delete", D_KEY_DELETE}, {"home", D_KEY_HOME}, {"end", D_KEY_END},
        {"page_up", D_KEY_PAGE_UP}, {"page_down", D_KEY_PAGE_DOWN},
        {"caps_lock", D_KEY_CAPS_LOCK}, {"scroll_lock", D_KEY_SCROLL_LOCK}, {"num_lock", D_KEY_NUM_LOCK},
        {"print_screen", D_KEY_PRINT_SCREEN}, {"pause", D_KEY_PAUSE},
        {"apostrophe", D_KEY_APOSTROPHE}, {"comma", D_KEY_COMMA}, {"minus", D_KEY_MINUS}, {"period", D_KEY_PERIOD},
        {"slash", D_KEY_SLASH}, {"semicolon", D_KEY_SEMICOLON}, {"equal", D_KEY_EQUAL},
        {"left_bracket", D_KEY_LEFT_BRACKET}, {"backslash", D_KEY_BACKSLASH}, {"right_bracket", D_KEY_RIGHT_BRACKET},
        {"grave_accent", D_KEY_GRAVE_ACCENT}, {"world_1", D_KEY_WORLD_1}, {"world_2", D_KEY_WORLD_2},
        {"kp_decimal", D_KEY_KP_DECIMAL}, {"kp_divide", D_KEY_KP_DIVIDE}, {"kp_multiply", D_KEY_KP_MULTIPLY},
        {"kp_subtract", D_KEY_KP_SUBTRACT}, {"kp_add", D_KEY_KP_ADD}, {"kp_enter", D_KEY_KP_ENTER},
        {"kp_equal", D_KEY_KP_EQUAL},
    };

    // other spellings: the old send_input names, Unity's Input Manager names (underscored) and
    // Unity's KeyCode names (compacted, see aliasKeys())
    const KeyEntry keyAliases[] = {
        {"spacebar", D_KEY_SPACE}, {"return", D_KEY_ENTER}, {"esc", D_KEY_ESCAPE}, {"del", D_KEY_DELETE},
        {"equals", D_KEY_EQUAL}, {"backquote", D_KEY_GRAVE_ACCENT}, {"quote", D_KEY_APOSTROPHE},
        {"shift", D_KEY_LEFT_SHIFT}, {"control", D_KEY_LEFT_CONTROL}, {"ctrl", D_KEY_LEFT_CONTROL},
        {"alt", D_KEY_LEFT_ALT}, {"super", D_KEY_LEFT_SUPER},
        {"left_ctrl", D_KEY_LEFT_CONTROL}, {"right_ctrl", D_KEY_RIGHT_CONTROL},
        {"left_cmd", D_KEY_LEFT_SUPER}, {"right_cmd", D_KEY_RIGHT_SUPER},
        {"left_command", D_KEY_LEFT_SUPER}, {"right_command", D_KEY_RIGHT_SUPER},
        {"left_windows", D_KEY_LEFT_SUPER}, {"right_windows", D_KEY_RIGHT_SUPER},
        {"left_meta", D_KEY_LEFT_SUPER}, {"right_meta", D_KEY_RIGHT_SUPER},
        {"left_apple", D_KEY_LEFT_SUPER}, {"right_apple", D_KEY_RIGHT_SUPER},
        {"left_arrow", D_KEY_LEFT}, {"right_arrow", D_KEY_RIGHT}, {"up_arrow", D_KEY_UP}, {"down_arrow", D_KEY_DOWN},
        {"arrow_left", D_KEY_LEFT}, {"arrow_right", D_KEY_RIGHT}, {"arrow_up", D_KEY_UP}, {"arrow_down", D_KEY_DOWN},
        {"pageup", D_KEY_PAGE_UP}, {"pagedown", D_KEY_PAGE_DOWN}, {"numlock", D_KEY_NUM_LOCK},
        {"capslock", D_KEY_CAPS_LOCK}, {"scrolllock", D_KEY_SCROLL_LOCK}, {"print", D_KEY_PRINT_SCREEN},
        {"sys_req", D_KEY_PRINT_SCREEN}, {"break", D_KEY_PAUSE},
        {"keypad_enter", D_KEY_KP_ENTER}, {"keypad_plus", D_KEY_KP_ADD}, {"keypad_minus", D_KEY_KP_SUBTRACT},
        {"keypad_multiply", D_KEY_KP_MULTIPLY}, {"keypad_divide", D_KEY_KP_DIVIDE},
        {"keypad_period", D_KEY_KP_DECIMAL}, {"keypad_equals", D_KEY_KP_EQUAL},
        {"left_bracket", D_KEY_LEFT_BRACKET}, {"right_bracket", D_KEY_RIGHT_BRACKET},
    };

    // Unity's keypad names: "[1]", "[+]", "[.]"...
    int unityKeypadKey(const std::string& lower) {
        if (lower.size() < 3 || lower.front() != '[' || lower.back() != ']') return D_KEY_UNKNOWN;
        const std::string inner = lower.substr(1, lower.size() - 2);
        if (inner.size() == 1 && std::isdigit(static_cast<unsigned char>(inner[0]))) return D_KEY_KP_0 + (inner[0] - '0');
        if (inner == "+") return D_KEY_KP_ADD;
        if (inner == "-") return D_KEY_KP_SUBTRACT;
        if (inner == "*") return D_KEY_KP_MULTIPLY;
        if (inner == "/") return D_KEY_KP_DIVIDE;
        if (inner == ".") return D_KEY_KP_DECIMAL;
        if (inner == "=") return D_KEY_KP_EQUAL;
        if (inner == "enter") return D_KEY_KP_ENTER;
        return D_KEY_UNKNOWN;
    }

    int singleCharacterKey(char c) {
        if (c >= 'a' && c <= 'z') return D_KEY_A + (c - 'a');
        if (c >= '0' && c <= '9') return D_KEY_0 + (c - '0');
        switch (c) {
            case ' ': return D_KEY_SPACE;
            case '\'': return D_KEY_APOSTROPHE;
            case ',': return D_KEY_COMMA;
            case '-': return D_KEY_MINUS;
            case '.': return D_KEY_PERIOD;
            case '/': return D_KEY_SLASH;
            case ';': return D_KEY_SEMICOLON;
            case '=': return D_KEY_EQUAL;
            case '[': return D_KEY_LEFT_BRACKET;
            case '\\': return D_KEY_BACKSLASH;
            case ']': return D_KEY_RIGHT_BRACKET;
            case '`': return D_KEY_GRAVE_ACCENT;
            default: return D_KEY_UNKNOWN;
        }
    }

    // every key spelling, by its compacted form
    const std::map<std::string, int>& compactKeys() {
        static const std::map<std::string, int> table = [] {
            std::map<std::string, int> t;
            for (const KeyEntry& entry : canonicalKeys) t[compact(entry.name)] = entry.key;
            for (const KeyEntry& entry : keyAliases) t[compact(entry.name)] = entry.key;
            // KeyCode names
            for (int i = 0; i <= 9; i++) {
                t["alpha" + std::to_string(i)] = D_KEY_0 + i;
                t["keypad" + std::to_string(i)] = D_KEY_KP_0 + i;
                t["numpad" + std::to_string(i)] = D_KEY_KP_0 + i;
                t["kp" + std::to_string(i)] = D_KEY_KP_0 + i;
            }
            t["leftcontrol"] = D_KEY_LEFT_CONTROL;
            t["rightcontrol"] = D_KEY_RIGHT_CONTROL;
            return t;
        }();
        return table;
    }

    struct ButtonEntry { const char* name; int button; };

    const ButtonEntry gamepadButtons[] = {
        {"gamepad_a", D_GAMEPAD_BUTTON_A}, {"gamepad_b", D_GAMEPAD_BUTTON_B}, {"gamepad_x", D_GAMEPAD_BUTTON_X},
        {"gamepad_y", D_GAMEPAD_BUTTON_Y}, {"gamepad_left_bumper", D_GAMEPAD_BUTTON_LEFT_BUMPER},
        {"gamepad_right_bumper", D_GAMEPAD_BUTTON_RIGHT_BUMPER}, {"gamepad_back", D_GAMEPAD_BUTTON_BACK},
        {"gamepad_start", D_GAMEPAD_BUTTON_START}, {"gamepad_guide", D_GAMEPAD_BUTTON_GUIDE},
        {"gamepad_left_thumb", D_GAMEPAD_BUTTON_LEFT_THUMB}, {"gamepad_right_thumb", D_GAMEPAD_BUTTON_RIGHT_THUMB},
        {"gamepad_dpad_up", D_GAMEPAD_BUTTON_DPAD_UP}, {"gamepad_dpad_right", D_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"gamepad_dpad_down", D_GAMEPAD_BUTTON_DPAD_DOWN}, {"gamepad_dpad_left", D_GAMEPAD_BUTTON_DPAD_LEFT},
    };

    // the names after "gamepad_" (send_input's own names) and their other spellings
    const ButtonEntry gamepadButtonAliases[] = {
        {"cross", D_GAMEPAD_BUTTON_CROSS}, {"circle", D_GAMEPAD_BUTTON_CIRCLE}, {"square", D_GAMEPAD_BUTTON_SQUARE},
        {"triangle", D_GAMEPAD_BUTTON_TRIANGLE}, {"lb", D_GAMEPAD_BUTTON_LEFT_BUMPER}, {"rb", D_GAMEPAD_BUTTON_RIGHT_BUMPER},
        {"left_shoulder", D_GAMEPAD_BUTTON_LEFT_BUMPER}, {"right_shoulder", D_GAMEPAD_BUTTON_RIGHT_BUMPER},
        {"select", D_GAMEPAD_BUTTON_BACK}, {"view", D_GAMEPAD_BUTTON_BACK}, {"menu", D_GAMEPAD_BUTTON_START},
        {"options", D_GAMEPAD_BUTTON_START}, {"home", D_GAMEPAD_BUTTON_GUIDE},
        {"ls", D_GAMEPAD_BUTTON_LEFT_THUMB}, {"rs", D_GAMEPAD_BUTTON_RIGHT_THUMB},
        {"left_stick", D_GAMEPAD_BUTTON_LEFT_THUMB}, {"right_stick", D_GAMEPAD_BUTTON_RIGHT_THUMB},
        {"up", D_GAMEPAD_BUTTON_DPAD_UP}, {"right", D_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"down", D_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", D_GAMEPAD_BUTTON_DPAD_LEFT},
    };

    // Unity's "joystick button N" on the Xbox layout: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back,
    // 7 Start, 8 left stick, 9 right stick
    const int unityJoystickButtons[] = {
        D_GAMEPAD_BUTTON_A, D_GAMEPAD_BUTTON_B, D_GAMEPAD_BUTTON_X, D_GAMEPAD_BUTTON_Y,
        D_GAMEPAD_BUTTON_LEFT_BUMPER, D_GAMEPAD_BUTTON_RIGHT_BUMPER, D_GAMEPAD_BUTTON_BACK,
        D_GAMEPAD_BUTTON_START, D_GAMEPAD_BUTTON_LEFT_THUMB, D_GAMEPAD_BUTTON_RIGHT_THUMB,
    };

    const ButtonEntry gamepadAxes[] = {
        {"left_x", D_GAMEPAD_AXIS_LEFT_X}, {"left_y", D_GAMEPAD_AXIS_LEFT_Y}, {"right_x", D_GAMEPAD_AXIS_RIGHT_X},
        {"right_y", D_GAMEPAD_AXIS_RIGHT_Y}, {"left_trigger", D_GAMEPAD_AXIS_LEFT_TRIGGER},
        {"right_trigger", D_GAMEPAD_AXIS_RIGHT_TRIGGER},
    };

    const ButtonEntry gamepadAxisAliases[] = {
        {"lt", D_GAMEPAD_AXIS_LEFT_TRIGGER}, {"rt", D_GAMEPAD_AXIS_RIGHT_TRIGGER},
        {"x_axis", D_GAMEPAD_AXIS_LEFT_X}, {"y_axis", D_GAMEPAD_AXIS_LEFT_Y},
        {"left_stick_x", D_GAMEPAD_AXIS_LEFT_X}, {"left_stick_y", D_GAMEPAD_AXIS_LEFT_Y},
        {"right_stick_x", D_GAMEPAD_AXIS_RIGHT_X}, {"right_stick_y", D_GAMEPAD_AXIS_RIGHT_Y},
        {"gamepad_left_x", D_GAMEPAD_AXIS_LEFT_X}, {"gamepad_left_y", D_GAMEPAD_AXIS_LEFT_Y},
        {"gamepad_right_x", D_GAMEPAD_AXIS_RIGHT_X}, {"gamepad_right_y", D_GAMEPAD_AXIS_RIGHT_Y},
        {"gamepad_left_trigger", D_GAMEPAD_AXIS_LEFT_TRIGGER}, {"gamepad_right_trigger", D_GAMEPAD_AXIS_RIGHT_TRIGGER},
    };

    // Unity's "joystick button N" / "JoystickButtonN" -> gamepad button, else -1; a per-joystick
    // name ("joystick 1 button 0") is -2 (a binding names its pad with `gamepad`)
    int unityJoystickButton(const std::string& lower) {
        const std::string name = underscored(lower);
        std::string number;
        if (name.rfind("joystick_button_", 0) == 0) number = name.substr(16);
        else if (name.rfind("joystickbutton", 0) == 0) number = name.substr(14);
        else if (name.rfind("joystick_", 0) == 0 && name.find("_button_") != std::string::npos) return -2;
        else if (name.rfind("joystick", 0) == 0 && name.find("button") != std::string::npos && name.size() > 8 &&
                 std::isdigit(static_cast<unsigned char>(name[8]))) return -2;
        else return -1;
        if (!allDigits(number)) return -1;
        const int n = std::stoi(number);
        if (n >= 0 && n < static_cast<int>(std::size(unityJoystickButtons))) return unityJoystickButtons[n];
        return -1;
    }

}

int InputControlNames::parseKey(const std::string& raw) {
    // a single character is the key that types it (" " is space)
    if (raw.size() == 1) return singleCharacterKey(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[0]))));
    const std::string lower = lowerTrimmed(raw);
    if (lower.empty()) return D_KEY_UNKNOWN;
    if (lower.size() == 1) return singleCharacterKey(lower[0]);
    const int keypad = unityKeypadKey(lower);
    if (keypad != D_KEY_UNKNOWN) return keypad;

    std::string name = underscored(lower);
    if (name.rfind("d_key_", 0) == 0) name = name.substr(6);
    else if (name.rfind("key_", 0) == 0) name = name.substr(4);
    if (name.size() == 1) return singleCharacterKey(name[0]);

    auto& table = compactKeys();
    auto it = table.find(compact(name));
    if (it != table.end()) return it->second;
    // f1..f25
    if (name.size() >= 2 && name[0] == 'f' && allDigits(name, 1)) {
        const int n = std::stoi(name.substr(1));
        if (n >= 1 && n <= 25) return D_KEY_F1 + n - 1;
    }
    return D_KEY_UNKNOWN;
}

int InputControlNames::parseMouseButton(const std::string& raw) {
    const std::string name = underscored(lowerTrimmed(raw));
    if (name == "mouse_left" || name == "left" || name == "mouse" || name == "mouse_button_left") return D_MOUSE_BUTTON_LEFT;
    if (name == "mouse_right" || name == "right" || name == "mouse_button_right") return D_MOUSE_BUTTON_RIGHT;
    if (name == "mouse_middle" || name == "middle" || name == "mouse_button_middle") return D_MOUSE_BUTTON_MIDDLE;
    // the control paths' "button4".."button8" (1-based: the 4th button)
    if (name.rfind("button", 0) == 0 && allDigits(name, 6)) {
        const int n = std::stoi(name.substr(6));
        return (n >= 4 && n <= D_MOUSE_BUTTON_LAST + 1) ? n - 1 : -1;
    }
    // Unity: "mouse 0" / KeyCode.Mouse0 is the left button (0-based); ours: mouse_4..mouse_8 (1-based)
    const std::string flat = compact(name);
    if (flat.rfind("mousebutton", 0) == 0 && allDigits(flat, 11)) {
        // Input.MOUSE_BUTTON_1 .. _8 spelled out: 1-based
        const int n = std::stoi(flat.substr(11));
        return (n >= 1 && n <= D_MOUSE_BUTTON_LAST + 1) ? n - 1 : -1;
    }
    if (flat.rfind("mouse", 0) == 0 && allDigits(flat, 5)) {
        const int n = std::stoi(flat.substr(5));
        // "mouse_4".."mouse_8" (canonical, 1-based: the 4th button) vs "mouse 0".."mouse 6" (Unity, 0-based)
        const bool canonical = name.rfind("mouse_", 0) == 0 && n >= 4 && n <= 8 && raw.find(' ') == std::string::npos;
        if (canonical) return n - 1;
        return (n >= 0 && n <= D_MOUSE_BUTTON_LAST) ? n : -1;
    }
    return -1;
}

int InputControlNames::parseGamepadButton(const std::string& raw) {
    const std::string lower = lowerTrimmed(raw);
    std::string name = underscored(lower);
    for (const ButtonEntry& entry : gamepadButtons) {
        if (name == entry.name || name == std::string(entry.name).substr(8)) return entry.button;
    }
    const std::string bare = name.rfind("gamepad_", 0) == 0 ? name.substr(8) : name;
    for (const ButtonEntry& entry : gamepadButtonAliases) {
        if (bare == entry.name) return entry.button;
    }
    const std::string flatBare = compact(bare);
    for (const ButtonEntry& entry : gamepadButtons) {
        if (flatBare == compact(std::string(entry.name).substr(8))) return entry.button;
    }
    const int unity = unityJoystickButton(lower);
    return unity >= 0 ? unity : -1;
}

int InputControlNames::parseGamepadAxis(const std::string& raw) {
    const std::string name = underscored(lowerTrimmed(raw));
    for (const ButtonEntry& entry : gamepadAxes) {
        if (name == entry.name) return entry.button;
    }
    for (const ButtonEntry& entry : gamepadAxisAliases) {
        if (name == entry.name) return entry.button;
    }
    return -1;
}

bool InputControlNames::parseButton(const std::string& raw, InputControl& out, std::string* error) {
    const std::string lower = lowerTrimmed(raw);
    if (raw.size() == 1 && parseKey(raw) != D_KEY_UNKNOWN) {
        out = {InputDevice::Key, parseKey(raw)};
        return true;
    }
    if (lower.empty()) {
        if (error) *error = "an empty control name";
        return false;
    }
    const std::string name = underscored(lower);

    // the prefixed names first: "a" is the key A, "gamepad_a" the pad's A
    if (name.rfind("gamepad_", 0) == 0) {
        const int button = parseGamepadButton(name);
        if (button >= 0) {
            out = {InputDevice::GamepadButton, button};
            return true;
        }
    }
    const int joystick = unityJoystickButton(lower);
    if (joystick >= 0) {
        out = {InputDevice::GamepadButton, joystick};
        return true;
    }
    if (joystick == -2) {
        if (error) *error = "\"" + raw + "\" names one joystick: use \"joystick button N\" and the binding's gamepad (0-15)";
        return false;
    }
    if (name.rfind("mouse", 0) == 0) {
        const int button = parseMouseButton(raw);
        if (button >= 0) {
            out = {InputDevice::Mouse, button};
            return true;
        }
    }
    const int key = parseKey(raw);
    if (key != D_KEY_UNKNOWN) {
        out = {InputDevice::Key, key};
        return true;
    }
    if (parseGamepadAxis(raw) >= 0) {
        if (error) *error = "\"" + raw + "\" is a gamepad axis: use a gamepad_axis binding";
        return false;
    }
    if (error) *error = "unknown control \"" + raw + "\" (keys a, space, left, left_shift, f1, kp_0...; mouse_left, "
                        "mouse_right, mouse_middle; gamepad_a, gamepad_dpad_up...)";
    return false;
}

bool InputControlNames::parse(const std::string& raw, InputControl& out) {
    if (parseButton(raw, out)) return true;
    const int axis = parseGamepadAxis(raw);
    if (axis >= 0) {
        out = {InputDevice::GamepadAxis, axis};
        return true;
    }
    return false;
}

namespace {

    // a key's canonical name, "" for a code without one
    std::string canonicalKeyName(int key) {
        if (key >= D_KEY_A && key <= D_KEY_Z) return std::string(1, static_cast<char>('a' + key - D_KEY_A));
        if (key >= D_KEY_0 && key <= D_KEY_9) return std::string(1, static_cast<char>('0' + key - D_KEY_0));
        if (key >= D_KEY_F1 && key <= D_KEY_F25) return "f" + std::to_string(key - D_KEY_F1 + 1);
        if (key >= D_KEY_KP_0 && key <= D_KEY_KP_9) return "kp_" + std::to_string(key - D_KEY_KP_0);
        for (const KeyEntry& entry : canonicalKeys) {
            if (entry.key == key) return entry.name;
        }
        return "";
    }

    const char* const mousePathSegments[] = {"left", "right", "middle", "button4", "button5", "button6", "button7", "button8"};

}

std::string InputControlNames::keyName(int key) {
    const std::string name = canonicalKeyName(key);
    // a code without a name stays readable (and unique) in a path
    return name.empty() ? std::to_string(key) : name;
}

std::string InputControlNames::mouseButtonName(int button) {
    if (button >= 0 && button <= D_MOUSE_BUTTON_LAST) return mousePathSegments[button];
    return std::to_string(button);
}

std::string InputControlNames::gamepadButtonName(int button) {
    for (const ButtonEntry& entry : gamepadButtons) {
        if (entry.button == button) return std::string(entry.name).substr(8);
    }
    return std::to_string(button);
}

std::string InputControlNames::gamepadAxisName(int axis) {
    for (const ButtonEntry& entry : gamepadAxes) {
        if (entry.button == axis) return entry.name;
    }
    return std::to_string(axis);
}

std::string InputControlNames::mouseButtonControlName(int button) {
    switch (button) {
        case D_MOUSE_BUTTON_LEFT: return "mouse_left";
        case D_MOUSE_BUTTON_RIGHT: return "mouse_right";
        case D_MOUSE_BUTTON_MIDDLE: return "mouse_middle";
        default: break;
    }
    if (button >= D_MOUSE_BUTTON_4 && button <= D_MOUSE_BUTTON_LAST) return "mouse_" + std::to_string(button + 1);
    return "";
}

std::string InputControlNames::gamepadButtonControlName(int button) {
    for (const ButtonEntry& entry : gamepadButtons) {
        if (entry.button == button) return entry.name;
    }
    return "";
}

std::string InputControlNames::name(const InputControl& control) {
    switch (control.device) {
        case InputDevice::Key: return canonicalKeyName(control.code);
        case InputDevice::Mouse: return mouseButtonControlName(control.code);
        case InputDevice::GamepadButton: return gamepadButtonControlName(control.code);
        case InputDevice::GamepadAxis: {
            for (const ButtonEntry& entry : gamepadAxes) {
                if (entry.button == control.code) return entry.name;
            }
            return "";
        }
    }
    return "";
}

const std::vector<std::string>& InputControlNames::keyNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (char c = 'a'; c <= 'z'; c++) out.emplace_back(1, c);
        for (char c = '0'; c <= '9'; c++) out.emplace_back(1, c);
        for (const KeyEntry& entry : canonicalKeys) out.emplace_back(entry.name);
        for (int i = 0; i <= 9; i++) out.push_back("kp_" + std::to_string(i));
        for (int i = 1; i <= 25; i++) out.push_back("f" + std::to_string(i));
        return out;
    }();
    return names;
}

const std::vector<std::string>& InputControlNames::mouseButtonControlNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (int button = 0; button <= D_MOUSE_BUTTON_LAST; button++) out.push_back(mouseButtonControlName(button));
        return out;
    }();
    return names;
}

const std::vector<std::string>& InputControlNames::gamepadButtonControlNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const ButtonEntry& entry : gamepadButtons) out.emplace_back(entry.name);
        return out;
    }();
    return names;
}

const std::vector<std::string>& InputControlNames::gamepadAxisNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const ButtonEntry& entry : gamepadAxes) out.emplace_back(entry.name);
        return out;
    }();
    return names;
}
