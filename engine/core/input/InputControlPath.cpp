// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputControlPath.h"

#include "Input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace jasperengine;

namespace {

    struct NamedCode {
        int code;
        const char* name;
    };

    // canonical names first: keyName returns the first entry of a key
    const NamedCode KEY_NAMES[] = {
        {D_KEY_SPACE, "space"}, {D_KEY_APOSTROPHE, "apostrophe"}, {D_KEY_COMMA, "comma"},
        {D_KEY_MINUS, "minus"}, {D_KEY_PERIOD, "period"}, {D_KEY_SLASH, "slash"},
        {D_KEY_SEMICOLON, "semicolon"}, {D_KEY_EQUAL, "equal"}, {D_KEY_LEFT_BRACKET, "left_bracket"},
        {D_KEY_BACKSLASH, "backslash"}, {D_KEY_RIGHT_BRACKET, "right_bracket"},
        {D_KEY_GRAVE_ACCENT, "grave_accent"}, {D_KEY_WORLD_1, "world_1"}, {D_KEY_WORLD_2, "world_2"},
        {D_KEY_ESCAPE, "escape"}, {D_KEY_ENTER, "enter"}, {D_KEY_TAB, "tab"}, {D_KEY_BACKSPACE, "backspace"},
        {D_KEY_INSERT, "insert"}, {D_KEY_DELETE, "delete"}, {D_KEY_RIGHT, "right"}, {D_KEY_LEFT, "left"},
        {D_KEY_DOWN, "down"}, {D_KEY_UP, "up"}, {D_KEY_PAGE_UP, "page_up"}, {D_KEY_PAGE_DOWN, "page_down"},
        {D_KEY_HOME, "home"}, {D_KEY_END, "end"}, {D_KEY_CAPS_LOCK, "caps_lock"},
        {D_KEY_SCROLL_LOCK, "scroll_lock"}, {D_KEY_NUM_LOCK, "num_lock"}, {D_KEY_PRINT_SCREEN, "print_screen"},
        {D_KEY_PAUSE, "pause"}, {D_KEY_KP_DECIMAL, "kp_decimal"}, {D_KEY_KP_DIVIDE, "kp_divide"},
        {D_KEY_KP_MULTIPLY, "kp_multiply"}, {D_KEY_KP_SUBTRACT, "kp_subtract"}, {D_KEY_KP_ADD, "kp_add"},
        {D_KEY_KP_ENTER, "kp_enter"}, {D_KEY_KP_EQUAL, "kp_equal"}, {D_KEY_LEFT_SHIFT, "left_shift"},
        {D_KEY_LEFT_CONTROL, "left_control"}, {D_KEY_LEFT_ALT, "left_alt"}, {D_KEY_LEFT_SUPER, "left_super"},
        {D_KEY_RIGHT_SHIFT, "right_shift"}, {D_KEY_RIGHT_CONTROL, "right_control"}, {D_KEY_RIGHT_ALT, "right_alt"},
        {D_KEY_RIGHT_SUPER, "right_super"}, {D_KEY_MENU, "menu"},
        // aliases
        {D_KEY_SPACE, "spacebar"}, {D_KEY_EQUAL, "equals"}, {D_KEY_GRAVE_ACCENT, "backquote"},
        {D_KEY_ESCAPE, "esc"}, {D_KEY_ENTER, "return"}, {D_KEY_DELETE, "del"},
        {D_KEY_RIGHT, "right_arrow"}, {D_KEY_LEFT, "left_arrow"}, {D_KEY_DOWN, "down_arrow"}, {D_KEY_UP, "up_arrow"},
        {D_KEY_RIGHT, "arrow_right"}, {D_KEY_LEFT, "arrow_left"}, {D_KEY_DOWN, "arrow_down"}, {D_KEY_UP, "arrow_up"},
        {D_KEY_PAGE_UP, "pageup"}, {D_KEY_PAGE_DOWN, "pagedown"},
        {D_KEY_LEFT_SHIFT, "shift"}, {D_KEY_LEFT_CONTROL, "left_ctrl"}, {D_KEY_LEFT_CONTROL, "control"},
        {D_KEY_LEFT_CONTROL, "ctrl"}, {D_KEY_LEFT_ALT, "alt"}, {D_KEY_LEFT_SUPER, "super"},
        {D_KEY_RIGHT_CONTROL, "right_ctrl"},
    };

    const NamedCode MOUSE_BUTTON_NAMES[] = {
        {D_MOUSE_BUTTON_LEFT, "left"}, {D_MOUSE_BUTTON_RIGHT, "right"}, {D_MOUSE_BUTTON_MIDDLE, "middle"},
        {D_MOUSE_BUTTON_4, "button4"}, {D_MOUSE_BUTTON_5, "button5"}, {D_MOUSE_BUTTON_6, "button6"},
        {D_MOUSE_BUTTON_7, "button7"}, {D_MOUSE_BUTTON_8, "button8"},
    };

    const NamedCode GAMEPAD_BUTTON_NAMES[] = {
        {D_GAMEPAD_BUTTON_A, "a"}, {D_GAMEPAD_BUTTON_B, "b"}, {D_GAMEPAD_BUTTON_X, "x"}, {D_GAMEPAD_BUTTON_Y, "y"},
        {D_GAMEPAD_BUTTON_LEFT_BUMPER, "left_bumper"}, {D_GAMEPAD_BUTTON_RIGHT_BUMPER, "right_bumper"},
        {D_GAMEPAD_BUTTON_BACK, "back"}, {D_GAMEPAD_BUTTON_START, "start"}, {D_GAMEPAD_BUTTON_GUIDE, "guide"},
        {D_GAMEPAD_BUTTON_LEFT_THUMB, "left_thumb"}, {D_GAMEPAD_BUTTON_RIGHT_THUMB, "right_thumb"},
        {D_GAMEPAD_BUTTON_DPAD_UP, "dpad_up"}, {D_GAMEPAD_BUTTON_DPAD_RIGHT, "dpad_right"},
        {D_GAMEPAD_BUTTON_DPAD_DOWN, "dpad_down"}, {D_GAMEPAD_BUTTON_DPAD_LEFT, "dpad_left"},
        // aliases
        {D_GAMEPAD_BUTTON_CROSS, "cross"}, {D_GAMEPAD_BUTTON_CIRCLE, "circle"}, {D_GAMEPAD_BUTTON_SQUARE, "square"},
        {D_GAMEPAD_BUTTON_TRIANGLE, "triangle"}, {D_GAMEPAD_BUTTON_LEFT_BUMPER, "lb"},
        {D_GAMEPAD_BUTTON_RIGHT_BUMPER, "rb"}, {D_GAMEPAD_BUTTON_BACK, "select"},
        {D_GAMEPAD_BUTTON_LEFT_THUMB, "ls"}, {D_GAMEPAD_BUTTON_RIGHT_THUMB, "rs"},
    };

    const NamedCode GAMEPAD_AXIS_NAMES[] = {
        {D_GAMEPAD_AXIS_LEFT_X, "left_x"}, {D_GAMEPAD_AXIS_LEFT_Y, "left_y"}, {D_GAMEPAD_AXIS_RIGHT_X, "right_x"},
        {D_GAMEPAD_AXIS_RIGHT_Y, "right_y"}, {D_GAMEPAD_AXIS_LEFT_TRIGGER, "left_trigger"},
        {D_GAMEPAD_AXIS_RIGHT_TRIGGER, "right_trigger"},
        // aliases
        {D_GAMEPAD_AXIS_LEFT_TRIGGER, "lt"}, {D_GAMEPAD_AXIS_RIGHT_TRIGGER, "rt"},
    };

    std::string lower(std::string text){
        for (char& c : text)
            c = (char)std::tolower(static_cast<unsigned char>(c));
        return text;
    }

    template<size_t N>
    int findCode(const NamedCode (&table)[N], const std::string& name){
        for (const NamedCode& entry : table){
            if (name == entry.name)
                return entry.code;
        }
        return -1;
    }

    template<size_t N>
    const char* findName(const NamedCode (&table)[N], int code){
        for (const NamedCode& entry : table){
            if (entry.code == code)
                return entry.name;
        }
        return nullptr;
    }

    bool allDigits(const std::string& text, size_t from){
        if (from >= text.size())
            return false;
        return std::all_of(text.begin() + (std::ptrdiff_t)from, text.end(), [](char c){ return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    }

}

int InputControlNames::parseKey(const std::string& text){
    std::string name = lower(text);
    if (name.size() == 1){
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return D_KEY_A + (c - 'a');
        if (c >= '0' && c <= '9') return D_KEY_0 + (c - '0');
        switch (c){
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
    for (char& c : name){
        if (c == ' ' || c == '-')
            c = '_';
    }
    if (name.rfind("key_", 0) == 0) name = name.substr(4);
    if (name.rfind("d_key_", 0) == 0) name = name.substr(6);
    // a prefix may leave a single character ("key_a")
    if (name.size() == 1)
        return parseKey(name);

    const int code = findCode(KEY_NAMES, name);
    if (code >= 0)
        return code;
    if (name.size() >= 2 && name[0] == 'f' && allDigits(name, 1)){
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 25) return D_KEY_F1 + n - 1;
    }
    if (name.rfind("kp_", 0) == 0 && name.size() == 4 && std::isdigit(static_cast<unsigned char>(name[3])))
        return D_KEY_KP_0 + (name[3] - '0');
    if (name.rfind("numpad", 0) == 0 && name.size() == 7 && std::isdigit(static_cast<unsigned char>(name[6])))
        return D_KEY_KP_0 + (name[6] - '0');
    return D_KEY_UNKNOWN;
}

std::string InputControlNames::keyName(int key){
    if (key >= D_KEY_A && key <= D_KEY_Z) return std::string(1, static_cast<char>('a' + key - D_KEY_A));
    if (key >= D_KEY_0 && key <= D_KEY_9) return std::string(1, static_cast<char>('0' + key - D_KEY_0));
    if (key >= D_KEY_F1 && key <= D_KEY_F25) return "f" + std::to_string(key - D_KEY_F1 + 1);
    if (key >= D_KEY_KP_0 && key <= D_KEY_KP_9) return "kp_" + std::to_string(key - D_KEY_KP_0);
    if (const char* name = findName(KEY_NAMES, key))
        return name;
    return std::to_string(key);
}

int InputControlNames::parseMouseButton(const std::string& text){
    const std::string name = lower(text);
    return findCode(MOUSE_BUTTON_NAMES, name);
}

std::string InputControlNames::mouseButtonName(int button){
    if (const char* name = findName(MOUSE_BUTTON_NAMES, button))
        return name;
    return std::to_string(button);
}

int InputControlNames::parseGamepadButton(const std::string& text){
    return findCode(GAMEPAD_BUTTON_NAMES, lower(text));
}

std::string InputControlNames::gamepadButtonName(int button){
    if (const char* name = findName(GAMEPAD_BUTTON_NAMES, button))
        return name;
    return std::to_string(button);
}

int InputControlNames::parseGamepadAxis(const std::string& text){
    return findCode(GAMEPAD_AXIS_NAMES, lower(text));
}

std::string InputControlNames::gamepadAxisName(int axis){
    if (const char* name = findName(GAMEPAD_AXIS_NAMES, axis))
        return name;
    return std::to_string(axis);
}

InputControlPath InputControlPath::parse(const std::string& text){
    InputControlPath path;
    const std::string full = lower(text);
    const size_t slash = full.find('/');
    if (slash == std::string::npos)
        return path;
    const std::string device = full.substr(0, slash);
    const std::string rest = full.substr(slash + 1);
    if (rest.empty())
        return path;

    if (device == "keyboard"){
        const int key = InputControlNames::parseKey(rest);
        if (key != D_KEY_UNKNOWN){
            path.kind = InputControlKind::KEY;
            path.index = key;
        }
    }else if (device == "mouse"){
        if (rest == "delta" || rest == "delta/x" || rest == "delta/y" || rest == "scroll" || rest == "scroll/x" || rest == "scroll/y"){
            path.kind = rest.rfind("delta", 0) == 0 ? InputControlKind::MOUSE_DELTA : InputControlKind::MOUSE_SCROLL;
            if (rest.size() > 2 && rest[rest.size() - 2] == '/')
                path.component = rest.back() == 'x' ? 0 : 1;
        }else{
            const int button = InputControlNames::parseMouseButton(rest);
            if (button >= 0){
                path.kind = InputControlKind::MOUSE_BUTTON;
                path.index = button;
            }
        }
    }else if (device == "gamepad"){
        if (rest == "left_stick" || rest == "right_stick"){
            path.kind = InputControlKind::GAMEPAD_STICK;
            path.index = rest == "left_stick" ? 0 : 1;
        }else if (int axis = InputControlNames::parseGamepadAxis(rest); axis >= 0){
            path.kind = InputControlKind::GAMEPAD_AXIS;
            path.index = axis;
        }else if (int button = InputControlNames::parseGamepadButton(rest); button >= 0){
            path.kind = InputControlKind::GAMEPAD_BUTTON;
            path.index = button;
        }
    }else if (device == "touch"){
        if (rest == "primary/press") path.kind = InputControlKind::TOUCH_PRESS;
        else if (rest == "primary/position") path.kind = InputControlKind::TOUCH_POSITION;
        else if (rest == "primary/delta") path.kind = InputControlKind::TOUCH_DELTA;
        else if (rest == "count") path.kind = InputControlKind::TOUCH_COUNT;
    }else if (device == "virtual"){
        path.kind = InputControlKind::VIRTUAL;
        // the name keeps its case: scripts set it by name
        path.name = text.substr(slash + 1);
    }
    return path;
}

std::string InputControlPath::toString() const{
    const char* axisSuffix = component == 0 ? "/x" : component == 1 ? "/y" : "";
    switch (kind){
        case InputControlKind::KEY: return "keyboard/" + InputControlNames::keyName(index);
        case InputControlKind::MOUSE_BUTTON: return "mouse/" + InputControlNames::mouseButtonName(index);
        case InputControlKind::MOUSE_DELTA: return std::string("mouse/delta") + axisSuffix;
        case InputControlKind::MOUSE_SCROLL: return std::string("mouse/scroll") + axisSuffix;
        case InputControlKind::GAMEPAD_BUTTON: return "gamepad/" + InputControlNames::gamepadButtonName(index);
        case InputControlKind::GAMEPAD_AXIS: return "gamepad/" + InputControlNames::gamepadAxisName(index);
        case InputControlKind::GAMEPAD_STICK: return index == 0 ? "gamepad/left_stick" : "gamepad/right_stick";
        case InputControlKind::TOUCH_PRESS: return "touch/primary/press";
        case InputControlKind::TOUCH_POSITION: return "touch/primary/position";
        case InputControlKind::TOUCH_DELTA: return "touch/primary/delta";
        case InputControlKind::TOUCH_COUNT: return "touch/count";
        case InputControlKind::VIRTUAL: return "virtual/" + name;
        case InputControlKind::NONE: break;
    }
    return "";
}

bool InputControlPath::isDigital() const{
    return kind == InputControlKind::KEY || kind == InputControlKind::MOUSE_BUTTON ||
           kind == InputControlKind::GAMEPAD_BUTTON || kind == InputControlKind::TOUCH_PRESS;
}

bool InputControlPath::isVector2() const{
    switch (kind){
        case InputControlKind::MOUSE_DELTA:
        case InputControlKind::MOUSE_SCROLL:
            return component < 0;
        case InputControlKind::GAMEPAD_STICK:
        case InputControlKind::TOUCH_POSITION:
        case InputControlKind::TOUCH_DELTA:
        case InputControlKind::VIRTUAL:
            return true;
        default:
            return false;
    }
}

bool InputControlPath::isDelta() const{
    return kind == InputControlKind::MOUSE_DELTA || kind == InputControlKind::MOUSE_SCROLL || kind == InputControlKind::TOUCH_DELTA;
}

const char* InputControlPath::deviceGroup() const{
    switch (kind){
        case InputControlKind::KEY:
        case InputControlKind::MOUSE_BUTTON:
        case InputControlKind::MOUSE_DELTA:
        case InputControlKind::MOUSE_SCROLL:
            return "Keyboard&Mouse";
        case InputControlKind::GAMEPAD_BUTTON:
        case InputControlKind::GAMEPAD_AXIS:
        case InputControlKind::GAMEPAD_STICK:
            return "Gamepad";
        case InputControlKind::TOUCH_PRESS:
        case InputControlKind::TOUCH_POSITION:
        case InputControlKind::TOUCH_DELTA:
        case InputControlKind::TOUCH_COUNT:
            return "Touch";
        default:
            return "";
    }
}

bool jasperengine::inputPathMatches(const std::string& pattern, const std::string& path){
    // iterative glob with '*' only
    size_t p = 0, s = 0, star = std::string::npos, mark = 0;
    while (s < path.size()){
        if (p < pattern.size() && pattern[p] == '*'){
            star = p++;
            mark = s;
        }else if (p < pattern.size() && std::tolower(static_cast<unsigned char>(pattern[p])) == std::tolower(static_cast<unsigned char>(path[s]))){
            p++;
            s++;
        }else if (star != std::string::npos){
            p = star + 1;
            s = ++mark;
        }else{
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        p++;
    return p == pattern.size();
}
