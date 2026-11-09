// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef X11Pen_h
#define X11Pen_h

#include <X11/Xlib.h>

namespace jasperengine {

    // A pen's pressure from XInput 2, shared by the editor and the Linux player. libXi is loaded
    // when there, so neither needs it to start; without it (or the extension) the pressure stays
    // -1. Wayland needs nothing of its own: both are X11 clients there (XWayland), and XWayland
    // gives a tablet's tools XInput 2 devices with the same "Abs Pressure" valuator.
    class X11Pen {
    public:
        // selects the slave pointers' events on window (never the master pointer's, so the core
        // pointer events keep arriving) and the device changes on the root; false without XInput 2
        static bool start(Display* display, Window window);
        // another top-level window of the same display (the editor's floating panels); before
        // destroying it, removeWindow
        static void addWindow(Window window);
        static void removeWindow(Window window);
        // true when the event was XInput 2's (the caller drops it): before XFilterEvent, and
        // before the next XNextEvent
        static bool handleEvent(XEvent& event);
        // 0..1 when the latest pointer event came from a pen, -1 from a mouse or a finger
        static float pressure();
        // before XCloseDisplay
        static void stop();
    };

}

#endif /* X11Pen_h */
