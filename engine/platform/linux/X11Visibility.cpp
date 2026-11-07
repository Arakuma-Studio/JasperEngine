// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "X11Visibility.h"

#include <X11/Xatom.h>

using namespace jasperengine;

void X11Visibility::attach(Display* targetDisplay, Window targetWindow, bool isMapped){
    display = targetDisplay;
    window = targetWindow;
    mapped = isMapped;
    wmHidden = false;
    if (!display || !window) return;
    netWmState = XInternAtom(display, "_NET_WM_STATE", False);
    netWmStateHidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", False);
    wmHidden = hasNetWmState(display, window, netWmStateHidden);
}

bool X11Visibility::handleEvent(const XEvent& event){
    if (!display || event.xany.window != window) return false;
    const bool wasHidden = isHidden();
    switch (event.type){
        case MapNotify:
            mapped = true;
            break;
        case UnmapNotify:
            mapped = false;
            break;
        case PropertyNotify:
            if (event.xproperty.atom == netWmState)
                wmHidden = event.xproperty.state != PropertyDelete && hasNetWmState(display, window, netWmStateHidden);
            break;
        default:
            break;
    }
    return isHidden() != wasHidden;
}

bool X11Visibility::hasNetWmState(Display* display, Window window, Atom state){
    if (!display || !window || state == None) return false;

    const Atom netWmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    bool found = false;
    if (XGetWindowProperty(display, window, netWmState, 0, 64, False, XA_ATOM,
                           &actualType, &actualFormat, &count, &bytesAfter, &data) == Success && data) {
        const Atom* states = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < count; ++i) {
            if (states[i] == state) found = true;
        }
        XFree(data);
    }
    return found;
}
