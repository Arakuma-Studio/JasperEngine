// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputControlPath.h"

#include "Input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace jasperengine;

namespace {

    std::string lower(std::string text){
        for (char& c : text)
            c = (char)std::tolower(static_cast<unsigned char>(c));
        return text;
    }

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
