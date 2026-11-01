// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "platform/LaunchKeys.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <initializer_list>

bool jasperengine::editor::platform::shiftHeld() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    char keys[32];
    XQueryKeymap(display, keys);
    bool held = false;
    for (KeySym sym : {(KeySym)XK_Shift_L, (KeySym)XK_Shift_R}) {
        const KeyCode code = XKeysymToKeycode(display, sym);
        if (code != 0 && (keys[code / 8] & (1 << (code % 8)))) held = true;
    }
    XCloseDisplay(display);
    return held;
}
