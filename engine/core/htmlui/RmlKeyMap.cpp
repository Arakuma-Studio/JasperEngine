// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlKeyMap.h"

#include "Input.h"

#include <cstddef>

using namespace jasperengine;
using namespace Rml::Input;

namespace {

    struct KeyEntry {
        int dKey;
        KeyIdentifier rmlKey;
        const char* code;       // DOM code
        const char* key;        // DOM key without shift
        const char* shiftKey;   // DOM key with shift (nullptr: same as key)
    };

    // Every D_KEY_* of Input.h. Punctuation follows the US layout, as RmlUi's Win32 backend
    // (KI_OEM_*). WORLD_1/2 and F25 have no RmlUi key.
    const KeyEntry keyTable[] = {
        {D_KEY_SPACE,         KI_SPACE,      "Space",        " ",  nullptr},
        {D_KEY_APOSTROPHE,    KI_OEM_7,      "Quote",        "'",  "\""},
        {D_KEY_COMMA,         KI_OEM_COMMA,  "Comma",        ",",  "<"},
        {D_KEY_MINUS,         KI_OEM_MINUS,  "Minus",        "-",  "_"},
        {D_KEY_PERIOD,        KI_OEM_PERIOD, "Period",       ".",  ">"},
        {D_KEY_SLASH,         KI_OEM_2,      "Slash",        "/",  "?"},
        {D_KEY_0,             KI_0,          "Digit0",       "0",  ")"},
        {D_KEY_1,             KI_1,          "Digit1",       "1",  "!"},
        {D_KEY_2,             KI_2,          "Digit2",       "2",  "@"},
        {D_KEY_3,             KI_3,          "Digit3",       "3",  "#"},
        {D_KEY_4,             KI_4,          "Digit4",       "4",  "$"},
        {D_KEY_5,             KI_5,          "Digit5",       "5",  "%"},
        {D_KEY_6,             KI_6,          "Digit6",       "6",  "^"},
        {D_KEY_7,             KI_7,          "Digit7",       "7",  "&"},
        {D_KEY_8,             KI_8,          "Digit8",       "8",  "*"},
        {D_KEY_9,             KI_9,          "Digit9",       "9",  "("},
        {D_KEY_SEMICOLON,     KI_OEM_1,      "Semicolon",    ";",  ":"},
        {D_KEY_EQUAL,         KI_OEM_PLUS,   "Equal",        "=",  "+"},
        {D_KEY_A,             KI_A,          "KeyA",         "a",  "A"},
        {D_KEY_B,             KI_B,          "KeyB",         "b",  "B"},
        {D_KEY_C,             KI_C,          "KeyC",         "c",  "C"},
        {D_KEY_D,             KI_D,          "KeyD",         "d",  "D"},
        {D_KEY_E,             KI_E,          "KeyE",         "e",  "E"},
        {D_KEY_F,             KI_F,          "KeyF",         "f",  "F"},
        {D_KEY_G,             KI_G,          "KeyG",         "g",  "G"},
        {D_KEY_H,             KI_H,          "KeyH",         "h",  "H"},
        {D_KEY_I,             KI_I,          "KeyI",         "i",  "I"},
        {D_KEY_J,             KI_J,          "KeyJ",         "j",  "J"},
        {D_KEY_K,             KI_K,          "KeyK",         "k",  "K"},
        {D_KEY_L,             KI_L,          "KeyL",         "l",  "L"},
        {D_KEY_M,             KI_M,          "KeyM",         "m",  "M"},
        {D_KEY_N,             KI_N,          "KeyN",         "n",  "N"},
        {D_KEY_O,             KI_O,          "KeyO",         "o",  "O"},
        {D_KEY_P,             KI_P,          "KeyP",         "p",  "P"},
        {D_KEY_Q,             KI_Q,          "KeyQ",         "q",  "Q"},
        {D_KEY_R,             KI_R,          "KeyR",         "r",  "R"},
        {D_KEY_S,             KI_S,          "KeyS",         "s",  "S"},
        {D_KEY_T,             KI_T,          "KeyT",         "t",  "T"},
        {D_KEY_U,             KI_U,          "KeyU",         "u",  "U"},
        {D_KEY_V,             KI_V,          "KeyV",         "v",  "V"},
        {D_KEY_W,             KI_W,          "KeyW",         "w",  "W"},
        {D_KEY_X,             KI_X,          "KeyX",         "x",  "X"},
        {D_KEY_Y,             KI_Y,          "KeyY",         "y",  "Y"},
        {D_KEY_Z,             KI_Z,          "KeyZ",         "z",  "Z"},
        {D_KEY_LEFT_BRACKET,  KI_OEM_4,      "BracketLeft",  "[",  "{"},
        {D_KEY_BACKSLASH,     KI_OEM_5,      "Backslash",    "\\", "|"},
        {D_KEY_RIGHT_BRACKET, KI_OEM_6,      "BracketRight", "]",  "}"},
        {D_KEY_GRAVE_ACCENT,  KI_OEM_3,      "Backquote",    "`",  "~"},
        {D_KEY_WORLD_1,       KI_UNKNOWN,    "IntlBackslash", "Unidentified", nullptr},
        {D_KEY_WORLD_2,       KI_UNKNOWN,    "IntlRo",       "Unidentified", nullptr},
        {D_KEY_ESCAPE,        KI_ESCAPE,     "Escape",       "Escape", nullptr},
        {D_KEY_ENTER,         KI_RETURN,     "Enter",        "Enter", nullptr},
        {D_KEY_TAB,           KI_TAB,        "Tab",          "Tab", nullptr},
        {D_KEY_BACKSPACE,     KI_BACK,       "Backspace",    "Backspace", nullptr},
        {D_KEY_INSERT,        KI_INSERT,     "Insert",       "Insert", nullptr},
        {D_KEY_DELETE,        KI_DELETE,     "Delete",       "Delete", nullptr},
        {D_KEY_RIGHT,         KI_RIGHT,      "ArrowRight",   "ArrowRight", nullptr},
        {D_KEY_LEFT,          KI_LEFT,       "ArrowLeft",    "ArrowLeft", nullptr},
        {D_KEY_DOWN,          KI_DOWN,       "ArrowDown",    "ArrowDown", nullptr},
        {D_KEY_UP,            KI_UP,         "ArrowUp",      "ArrowUp", nullptr},
        {D_KEY_PAGE_UP,       KI_PRIOR,      "PageUp",       "PageUp", nullptr},
        {D_KEY_PAGE_DOWN,     KI_NEXT,       "PageDown",     "PageDown", nullptr},
        {D_KEY_HOME,          KI_HOME,       "Home",         "Home", nullptr},
        {D_KEY_END,           KI_END,        "End",          "End", nullptr},
        {D_KEY_CAPS_LOCK,     KI_CAPITAL,    "CapsLock",     "CapsLock", nullptr},
        {D_KEY_SCROLL_LOCK,   KI_SCROLL,     "ScrollLock",   "ScrollLock", nullptr},
        {D_KEY_NUM_LOCK,      KI_NUMLOCK,    "NumLock",      "NumLock", nullptr},
        {D_KEY_PRINT_SCREEN,  KI_SNAPSHOT,   "PrintScreen",  "PrintScreen", nullptr},
        {D_KEY_PAUSE,         KI_PAUSE,      "Pause",        "Pause", nullptr},
        {D_KEY_F1,            KI_F1,         "F1",           "F1", nullptr},
        {D_KEY_F2,            KI_F2,         "F2",           "F2", nullptr},
        {D_KEY_F3,            KI_F3,         "F3",           "F3", nullptr},
        {D_KEY_F4,            KI_F4,         "F4",           "F4", nullptr},
        {D_KEY_F5,            KI_F5,         "F5",           "F5", nullptr},
        {D_KEY_F6,            KI_F6,         "F6",           "F6", nullptr},
        {D_KEY_F7,            KI_F7,         "F7",           "F7", nullptr},
        {D_KEY_F8,            KI_F8,         "F8",           "F8", nullptr},
        {D_KEY_F9,            KI_F9,         "F9",           "F9", nullptr},
        {D_KEY_F10,           KI_F10,        "F10",          "F10", nullptr},
        {D_KEY_F11,           KI_F11,        "F11",          "F11", nullptr},
        {D_KEY_F12,           KI_F12,        "F12",          "F12", nullptr},
        {D_KEY_F13,           KI_F13,        "F13",          "F13", nullptr},
        {D_KEY_F14,           KI_F14,        "F14",          "F14", nullptr},
        {D_KEY_F15,           KI_F15,        "F15",          "F15", nullptr},
        {D_KEY_F16,           KI_F16,        "F16",          "F16", nullptr},
        {D_KEY_F17,           KI_F17,        "F17",          "F17", nullptr},
        {D_KEY_F18,           KI_F18,        "F18",          "F18", nullptr},
        {D_KEY_F19,           KI_F19,        "F19",          "F19", nullptr},
        {D_KEY_F20,           KI_F20,        "F20",          "F20", nullptr},
        {D_KEY_F21,           KI_F21,        "F21",          "F21", nullptr},
        {D_KEY_F22,           KI_F22,        "F22",          "F22", nullptr},
        {D_KEY_F23,           KI_F23,        "F23",          "F23", nullptr},
        {D_KEY_F24,           KI_F24,        "F24",          "F24", nullptr},
        {D_KEY_F25,           KI_UNKNOWN,    "F25",          "F25", nullptr},
        {D_KEY_KP_0,          KI_NUMPAD0,    "Numpad0",      "0", nullptr},
        {D_KEY_KP_1,          KI_NUMPAD1,    "Numpad1",      "1", nullptr},
        {D_KEY_KP_2,          KI_NUMPAD2,    "Numpad2",      "2", nullptr},
        {D_KEY_KP_3,          KI_NUMPAD3,    "Numpad3",      "3", nullptr},
        {D_KEY_KP_4,          KI_NUMPAD4,    "Numpad4",      "4", nullptr},
        {D_KEY_KP_5,          KI_NUMPAD5,    "Numpad5",      "5", nullptr},
        {D_KEY_KP_6,          KI_NUMPAD6,    "Numpad6",      "6", nullptr},
        {D_KEY_KP_7,          KI_NUMPAD7,    "Numpad7",      "7", nullptr},
        {D_KEY_KP_8,          KI_NUMPAD8,    "Numpad8",      "8", nullptr},
        {D_KEY_KP_9,          KI_NUMPAD9,    "Numpad9",      "9", nullptr},
        {D_KEY_KP_DECIMAL,    KI_DECIMAL,    "NumpadDecimal", ".", nullptr},
        {D_KEY_KP_DIVIDE,     KI_DIVIDE,     "NumpadDivide", "/", nullptr},
        {D_KEY_KP_MULTIPLY,   KI_MULTIPLY,   "NumpadMultiply", "*", nullptr},
        {D_KEY_KP_SUBTRACT,   KI_SUBTRACT,   "NumpadSubtract", "-", nullptr},
        {D_KEY_KP_ADD,        KI_ADD,        "NumpadAdd",    "+", nullptr},
        {D_KEY_KP_ENTER,      KI_NUMPADENTER, "NumpadEnter", "Enter", nullptr},
        {D_KEY_KP_EQUAL,      KI_OEM_NEC_EQUAL, "NumpadEqual", "=", nullptr},
        {D_KEY_LEFT_SHIFT,    KI_LSHIFT,     "ShiftLeft",    "Shift", nullptr},
        {D_KEY_LEFT_CONTROL,  KI_LCONTROL,   "ControlLeft",  "Control", nullptr},
        {D_KEY_LEFT_ALT,      KI_LMENU,      "AltLeft",      "Alt", nullptr},
        {D_KEY_LEFT_SUPER,    KI_LMETA,      "MetaLeft",     "Meta", nullptr},
        {D_KEY_RIGHT_SHIFT,   KI_RSHIFT,     "ShiftRight",   "Shift", nullptr},
        {D_KEY_RIGHT_CONTROL, KI_RCONTROL,   "ControlRight", "Control", nullptr},
        {D_KEY_RIGHT_ALT,     KI_RMENU,      "AltRight",     "Alt", nullptr},
        {D_KEY_RIGHT_SUPER,   KI_RMETA,      "MetaRight",    "Meta", nullptr},
        {D_KEY_MENU,          KI_APPS,       "ContextMenu",  "ContextMenu", nullptr},
    };

    const KeyEntry* findEntry(int dKey){
        for (const KeyEntry& entry : keyTable){
            if (entry.dKey == dKey)
                return &entry;
        }
        return nullptr;
    }

#if defined(__APPLE__)
    bool appleModifiers = true;
#else
    bool appleModifiers = false;
#endif

}

KeyIdentifier RmlKeyMap::toRmlKey(int dKey){
    const KeyEntry* entry = findEntry(dKey);
    return entry ? entry->rmlKey : KI_UNKNOWN;
}

int RmlKeyMap::fromRmlKey(KeyIdentifier key){
    if (key == KI_UNKNOWN)
        return D_KEY_UNKNOWN;

    for (const KeyEntry& entry : keyTable){
        if (entry.rmlKey == key)
            return entry.dKey;
    }
    return D_KEY_UNKNOWN;
}

int RmlKeyMap::toRmlModifiers(int dMods){
    int result = 0;
    if (dMods & D_MODIFIER_SHIFT) result |= KM_SHIFT;
    if (dMods & D_MODIFIER_CONTROL) result |= KM_CTRL;
    if (dMods & D_MODIFIER_ALT) result |= KM_ALT;
    if (dMods & D_MODIFIER_SUPER){
        result |= KM_META;
        if (appleModifiers) result |= KM_CTRL;
    }
    if (dMods & D_MODIFIER_CAPS_LOCK) result |= KM_CAPSLOCK;
    if (dMods & D_MODIFIER_NUM_LOCK) result |= KM_NUMLOCK;
    return result;
}

int RmlKeyMap::fromRmlModifiers(int rmlMods){
    int result = 0;
    if (rmlMods & KM_SHIFT) result |= D_MODIFIER_SHIFT;
    if (rmlMods & KM_CTRL) result |= D_MODIFIER_CONTROL;
    if (rmlMods & KM_ALT) result |= D_MODIFIER_ALT;
    if (rmlMods & KM_META) result |= D_MODIFIER_SUPER;
    if (rmlMods & KM_CAPSLOCK) result |= D_MODIFIER_CAPS_LOCK;
    if (rmlMods & KM_NUMLOCK) result |= D_MODIFIER_NUM_LOCK;
    return result;
}

std::string RmlKeyMap::domCode(int dKey){
    const KeyEntry* entry = findEntry(dKey);
    return entry ? entry->code : "";
}

std::string RmlKeyMap::domKey(int dKey, int dMods){
    const KeyEntry* entry = findEntry(dKey);
    if (!entry)
        return "Unidentified";

    if (!entry->shiftKey)
        return entry->key;

    bool shifted = (dMods & D_MODIFIER_SHIFT) != 0;
    // caps lock shifts letters only
    if (dKey >= D_KEY_A && dKey <= D_KEY_Z && (dMods & D_MODIFIER_CAPS_LOCK))
        shifted = !shifted;

    return shifted ? entry->shiftKey : entry->key;
}

void RmlKeyMap::setAppleModifiers(bool enabled){
    appleModifiers = enabled;
}

bool RmlKeyMap::isAppleModifiers(){
    return appleModifiers;
}
