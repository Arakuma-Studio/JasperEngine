// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WEBKEYMAP_H
#define WEBKEYMAP_H

// The web platform's keyboard map: KeyboardEvent.code names and the legacy which/keyCode values
// to the engine's D_KEY_* codes. Plain C++ without Emscripten headers, so the unit tests read it
// (engine/tests/core/input/WebKeyMapTest.cpp).

#include "Input.h"

#include <string_view>

namespace jasperengine::web {

    struct KeyCodeName {
        std::string_view code;
        int key;
    };

    // every KeyboardEvent.code the engine has a key for
    inline constexpr KeyCodeName KEY_CODES[] = {
        {"Space", D_KEY_SPACE},
        {"Quote", D_KEY_APOSTROPHE}, /* ' */
        {"Comma", D_KEY_COMMA}, /* , */
        {"Minus", D_KEY_MINUS}, /* - */
        {"Period", D_KEY_PERIOD}, /* . */
        {"Slash", D_KEY_SLASH}, /* / */
        {"Digit0", D_KEY_0},
        {"Digit1", D_KEY_1},
        {"Digit2", D_KEY_2},
        {"Digit3", D_KEY_3},
        {"Digit4", D_KEY_4},
        {"Digit5", D_KEY_5},
        {"Digit6", D_KEY_6},
        {"Digit7", D_KEY_7},
        {"Digit8", D_KEY_8},
        {"Digit9", D_KEY_9},
        {"Semicolon", D_KEY_SEMICOLON}, /* ; */
        {"Equal", D_KEY_EQUAL}, /* = */
        {"KeyA", D_KEY_A},
        {"KeyB", D_KEY_B},
        {"KeyC", D_KEY_C},
        {"KeyD", D_KEY_D},
        {"KeyE", D_KEY_E},
        {"KeyF", D_KEY_F},
        {"KeyG", D_KEY_G},
        {"KeyH", D_KEY_H},
        {"KeyI", D_KEY_I},
        {"KeyJ", D_KEY_J},
        {"KeyK", D_KEY_K},
        {"KeyL", D_KEY_L},
        {"KeyM", D_KEY_M},
        {"KeyN", D_KEY_N},
        {"KeyO", D_KEY_O},
        {"KeyP", D_KEY_P},
        {"KeyQ", D_KEY_Q},
        {"KeyR", D_KEY_R},
        {"KeyS", D_KEY_S},
        {"KeyT", D_KEY_T},
        {"KeyU", D_KEY_U},
        {"KeyV", D_KEY_V},
        {"KeyW", D_KEY_W},
        {"KeyX", D_KEY_X},
        {"KeyY", D_KEY_Y},
        {"KeyZ", D_KEY_Z},
        {"BracketLeft", D_KEY_LEFT_BRACKET}, /* [ */
        {"Backslash", D_KEY_BACKSLASH}, /* \ */
        {"BracketRight", D_KEY_RIGHT_BRACKET}, /* ] */
        {"Backquote", D_KEY_GRAVE_ACCENT}, /* ` */
        {"Escape", D_KEY_ESCAPE},
        {"Enter", D_KEY_ENTER},
        {"Tab", D_KEY_TAB},
        {"Backspace", D_KEY_BACKSPACE},
        {"Insert", D_KEY_INSERT},
        {"Delete", D_KEY_DELETE},
        {"ArrowRight", D_KEY_RIGHT},
        {"ArrowLeft", D_KEY_LEFT},
        {"ArrowDown", D_KEY_DOWN},
        {"ArrowUp", D_KEY_UP},
        {"PageUp", D_KEY_PAGE_UP},
        {"PageDown", D_KEY_PAGE_DOWN},
        {"Home", D_KEY_HOME},
        {"End", D_KEY_END},
        {"CapsLock", D_KEY_CAPS_LOCK},
        {"ScrollLock", D_KEY_SCROLL_LOCK},
        {"NumLock", D_KEY_NUM_LOCK},
        {"PrintScreen", D_KEY_PRINT_SCREEN},
        {"Pause", D_KEY_PAUSE},
        {"F1", D_KEY_F1},
        {"F2", D_KEY_F2},
        {"F3", D_KEY_F3},
        {"F4", D_KEY_F4},
        {"F5", D_KEY_F5},
        {"F6", D_KEY_F6},
        {"F7", D_KEY_F7},
        {"F8", D_KEY_F8},
        {"F9", D_KEY_F9},
        {"F10", D_KEY_F10},
        {"F11", D_KEY_F11},
        {"F12", D_KEY_F12},
        {"F13", D_KEY_F13},
        {"F14", D_KEY_F14},
        {"F15", D_KEY_F15},
        {"F16", D_KEY_F16},
        {"F17", D_KEY_F17},
        {"F18", D_KEY_F18},
        {"F19", D_KEY_F19},
        {"F20", D_KEY_F20},
        {"F21", D_KEY_F21},
        {"F22", D_KEY_F22},
        {"F23", D_KEY_F23},
        {"F24", D_KEY_F24},
        {"Numpad0", D_KEY_KP_0},
        {"Numpad1", D_KEY_KP_1},
        {"Numpad2", D_KEY_KP_2},
        {"Numpad3", D_KEY_KP_3},
        {"Numpad4", D_KEY_KP_4},
        {"Numpad5", D_KEY_KP_5},
        {"Numpad6", D_KEY_KP_6},
        {"Numpad7", D_KEY_KP_7},
        {"Numpad8", D_KEY_KP_8},
        {"Numpad9", D_KEY_KP_9},
        {"NumpadDecimal", D_KEY_KP_DECIMAL},
        {"NumpadDivide", D_KEY_KP_DIVIDE},
        {"NumpadMultiply", D_KEY_KP_MULTIPLY},
        {"NumpadSubtract", D_KEY_KP_SUBTRACT},
        {"NumpadAdd", D_KEY_KP_ADD},
        {"NumpadEnter", D_KEY_KP_ENTER},
        {"NumpadEqual", D_KEY_KP_EQUAL},
        {"ShiftLeft", D_KEY_LEFT_SHIFT},
        {"ControlLeft", D_KEY_LEFT_CONTROL},
        {"AltLeft", D_KEY_LEFT_ALT},
        {"MetaLeft", D_KEY_LEFT_SUPER},
        // the names browsers used before MetaLeft / MetaRight / ContextMenu
        {"OSLeft", D_KEY_LEFT_SUPER},
        {"ShiftRight", D_KEY_RIGHT_SHIFT},
        {"ControlRight", D_KEY_RIGHT_CONTROL},
        {"AltRight", D_KEY_RIGHT_ALT},
        {"MetaRight", D_KEY_RIGHT_SUPER},
        {"OSRight", D_KEY_RIGHT_SUPER},
        {"ContextMenu", D_KEY_MENU},
        {"Menu", D_KEY_MENU},
        // the ISO key between left Shift and Z (GLFW's WORLD_2)
        {"IntlBackslash", D_KEY_WORLD_2},
    };

    // 0: no engine key
    inline int keyFromCode(std::string_view code){
        for (const KeyCodeName& entry : KEY_CODES){
            if (entry.code == code) return entry.key;
        }
        return 0;
    }

    // the legacy which/keyCode, for events without a code (Android soft keyboards, old browsers);
    // location: KeyboardEvent.location (2 the right-hand key, 3 the numeric keypad)
    inline int keyFromKeyCode(int keyCode, int location){
        constexpr int LOCATION_RIGHT = 2;
        constexpr int LOCATION_NUMPAD = 3;
        const bool right = location == LOCATION_RIGHT;
        // F13-F24
        if (keyCode >= 124 && keyCode <= 135) return D_KEY_F13 + (keyCode - 124);
        switch (keyCode){
            case 8: return D_KEY_BACKSPACE;
            case 9: return D_KEY_TAB;
            case 13: return location == LOCATION_NUMPAD ? D_KEY_KP_ENTER : D_KEY_ENTER;
            case 16: return right ? D_KEY_RIGHT_SHIFT : D_KEY_LEFT_SHIFT;
            case 17: return right ? D_KEY_RIGHT_CONTROL : D_KEY_LEFT_CONTROL;
            case 18: return right ? D_KEY_RIGHT_ALT : D_KEY_LEFT_ALT;
            case 19: return D_KEY_PAUSE;
            case 20: return D_KEY_CAPS_LOCK;
            case 27: return D_KEY_ESCAPE;
            case 32: return D_KEY_SPACE;
            case 33: return D_KEY_PAGE_UP;
            case 34: return D_KEY_PAGE_DOWN;
            case 35: return D_KEY_END;
            case 36: return D_KEY_HOME;
            case 37: return D_KEY_LEFT;
            case 38: return D_KEY_UP;
            case 39: return D_KEY_RIGHT;
            case 40: return D_KEY_DOWN;
            case 44: return D_KEY_PRINT_SCREEN;
            case 45: return D_KEY_INSERT;
            case 46: return D_KEY_DELETE;
            // Firefox's own codes for ; = -
            case 59: return D_KEY_SEMICOLON;
            case 61: return D_KEY_EQUAL;
            case 173: return D_KEY_MINUS;
            // the Windows and Command keys (224: Firefox's Meta)
            case 91: return right ? D_KEY_RIGHT_SUPER : D_KEY_LEFT_SUPER;
            case 92: return D_KEY_RIGHT_SUPER;
            case 224: return right ? D_KEY_RIGHT_SUPER : D_KEY_LEFT_SUPER;
            // the context-menu key; on the right (location 2) it is the right Command key of Chrome
            // and Safari on macOS
            case 93: return right ? D_KEY_RIGHT_SUPER : D_KEY_MENU;
            case 106: return D_KEY_KP_MULTIPLY;
            case 107: return D_KEY_KP_ADD;
            case 109: return D_KEY_KP_SUBTRACT;
            case 110: return D_KEY_KP_DECIMAL;
            case 111: return D_KEY_KP_DIVIDE;
            case 144: return D_KEY_NUM_LOCK;
            case 145: return D_KEY_SCROLL_LOCK;
            case 186: return D_KEY_SEMICOLON;
            case 187: return D_KEY_EQUAL;
            case 188: return D_KEY_COMMA;
            case 189: return D_KEY_MINUS;
            case 190: return D_KEY_PERIOD;
            case 191: return D_KEY_SLASH;
            case 192: return D_KEY_GRAVE_ACCENT;
            case 219: return D_KEY_LEFT_BRACKET;
            case 220: return D_KEY_BACKSLASH;
            case 221: return D_KEY_RIGHT_BRACKET;
            case 222: return D_KEY_APOSTROPHE;
            // AltGraph (Firefox on Linux) and the ISO key between left Shift and Z
            case 225: return D_KEY_RIGHT_ALT;
            case 226: return D_KEY_WORLD_2;
            default: break;
        }
        if (keyCode >= 48 && keyCode <= 57) return D_KEY_0 + (keyCode - 48);
        if (keyCode >= 65 && keyCode <= 90) return D_KEY_A + (keyCode - 65);
        if (keyCode >= 96 && keyCode <= 105) return D_KEY_KP_0 + (keyCode - 96);
        if (keyCode >= 112 && keyCode <= 123) return D_KEY_F1 + (keyCode - 112);
        return 0;
    }

}

#endif // WEBKEYMAP_H
