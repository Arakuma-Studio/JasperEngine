// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// iOS hardware keyboard -> engine key translation. UIKey.keyCode (iOS 13.4) is the
// USB HID usage of the key (UIKeyboardHIDUsage, keyboard page 0x07), so like the
// macOS virtual keycodes it is positional: W/A/S/D stay on the same physical keys
// across keyboard layouts. The engine's key constants are GLFW's (see core/Input.h).

#ifndef KeyCodesIOS_h
#define KeyCodesIOS_h

#import <UIKit/UIKit.h>

#include "Input.h"

namespace jasperengine {

    inline const short* iosKeycodeTable() {
        static short keys[256];
        static bool initialized = false;
        if (initialized) return keys;

        for (int i = 0; i < 256; ++i) keys[i] = D_KEY_UNKNOWN;

        // A-Z are 0x04-0x1D; the digits run 1-9 (0x1E-0x26), then 0
        for (int i = 0; i < 26; ++i) keys[0x04 + i] = (short)(D_KEY_A + i);
        for (int i = 0; i < 9; ++i) keys[0x1E + i] = (short)(D_KEY_1 + i);
        keys[0x27] = D_KEY_0;

        keys[0x28] = D_KEY_ENTER;
        keys[0x29] = D_KEY_ESCAPE;
        keys[0x2A] = D_KEY_BACKSPACE;      // DeleteOrBackspace
        keys[0x2B] = D_KEY_TAB;
        keys[0x2C] = D_KEY_SPACE;
        keys[0x2D] = D_KEY_MINUS;
        keys[0x2E] = D_KEY_EQUAL;
        keys[0x2F] = D_KEY_LEFT_BRACKET;
        keys[0x30] = D_KEY_RIGHT_BRACKET;
        keys[0x31] = D_KEY_BACKSLASH;
        keys[0x32] = D_KEY_WORLD_2;        // non-US # and ~
        keys[0x33] = D_KEY_SEMICOLON;
        keys[0x34] = D_KEY_APOSTROPHE;
        keys[0x35] = D_KEY_GRAVE_ACCENT;
        keys[0x36] = D_KEY_COMMA;
        keys[0x37] = D_KEY_PERIOD;
        keys[0x38] = D_KEY_SLASH;
        keys[0x39] = D_KEY_CAPS_LOCK;

        // F1-F12 are 0x3A-0x45, F13-F24 are 0x68-0x73
        for (int i = 0; i < 12; ++i) keys[0x3A + i] = (short)(D_KEY_F1 + i);
        for (int i = 0; i < 12; ++i) keys[0x68 + i] = (short)(D_KEY_F13 + i);

        keys[0x46] = D_KEY_PRINT_SCREEN;
        keys[0x47] = D_KEY_SCROLL_LOCK;
        keys[0x48] = D_KEY_PAUSE;
        keys[0x49] = D_KEY_INSERT;
        keys[0x4A] = D_KEY_HOME;
        keys[0x4B] = D_KEY_PAGE_UP;
        keys[0x4C] = D_KEY_DELETE;         // DeleteForward
        keys[0x4D] = D_KEY_END;
        keys[0x4E] = D_KEY_PAGE_DOWN;
        keys[0x4F] = D_KEY_RIGHT;
        keys[0x50] = D_KEY_LEFT;
        keys[0x51] = D_KEY_DOWN;
        keys[0x52] = D_KEY_UP;

        keys[0x53] = D_KEY_NUM_LOCK;       // NumLock, Clear on Apple keyboards
        keys[0x54] = D_KEY_KP_DIVIDE;
        keys[0x55] = D_KEY_KP_MULTIPLY;
        keys[0x56] = D_KEY_KP_SUBTRACT;
        keys[0x57] = D_KEY_KP_ADD;
        keys[0x58] = D_KEY_KP_ENTER;
        // keypad 1-9 are 0x59-0x61, then 0
        for (int i = 0; i < 9; ++i) keys[0x59 + i] = (short)(D_KEY_KP_1 + i);
        keys[0x62] = D_KEY_KP_0;
        keys[0x63] = D_KEY_KP_DECIMAL;
        keys[0x67] = D_KEY_KP_EQUAL;

        // Non-US \ and |, the extra key of ISO keyboards: WORLD_1, as KeyCodesMac
        // maps the ISO key (kVK_ISO_Section)
        keys[0x64] = D_KEY_WORLD_1;
        keys[0x65] = D_KEY_MENU;           // Application
        keys[0x76] = D_KEY_MENU;           // Menu

        keys[0xE0] = D_KEY_LEFT_CONTROL;
        keys[0xE1] = D_KEY_LEFT_SHIFT;
        keys[0xE2] = D_KEY_LEFT_ALT;
        keys[0xE3] = D_KEY_LEFT_SUPER;     // left GUI, Command
        keys[0xE4] = D_KEY_RIGHT_CONTROL;
        keys[0xE5] = D_KEY_RIGHT_SHIFT;
        keys[0xE6] = D_KEY_RIGHT_ALT;
        keys[0xE7] = D_KEY_RIGHT_SUPER;

        initialized = true;
        return keys;
    }

    inline int iosKeyFromHIDUsage(long usage) {
        if (usage < 0 || usage >= 256) return D_KEY_UNKNOWN;
        return iosKeycodeTable()[usage];
    }

    inline int iosKeyModifiers(UIKeyModifierFlags flags) {
        int mods = 0;
        if (flags & UIKeyModifierShift)      mods |= D_MODIFIER_SHIFT;
        if (flags & UIKeyModifierControl)    mods |= D_MODIFIER_CONTROL;
        if (flags & UIKeyModifierAlternate)  mods |= D_MODIFIER_ALT;
        if (flags & UIKeyModifierCommand)    mods |= D_MODIFIER_SUPER;
        if (flags & UIKeyModifierAlphaShift) mods |= D_MODIFIER_CAPS_LOCK;
        return mods;
    }

}

#endif /* KeyCodesIOS_h */
