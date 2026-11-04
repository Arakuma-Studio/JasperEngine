// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef X11Visibility_h
#define X11Visibility_h

#include <X11/Xlib.h>

namespace jasperengine {

    // Whether an X11 top-level window can be seen: mapped, and not _NET_WM_STATE_HIDDEN (window
    // managers such as mutter minimize without unmapping). Fed with the window's events; the
    // window selects StructureNotifyMask | PropertyChangeMask. Shared by exported games (their
    // update-only loop), the editor (minimized windows) and the tests.
    class X11Visibility {
    public:
        // mapped: the state until the first MapNotify or UnmapNotify (a game assumes the window it
        // asked to show is shown); _NET_WM_STATE is read once here
        void attach(Display* display, Window window, bool mapped);

        // MapNotify, UnmapNotify and PropertyNotify of _NET_WM_STATE on the window; true when
        // isHidden() changed
        bool handleEvent(const XEvent& event);

        bool isHidden() const { return !mapped || wmHidden; }
        bool isWmHidden() const { return wmHidden; }

        // whether the window's _NET_WM_STATE lists state (one XGetWindowProperty)
        static bool hasNetWmState(Display* display, Window window, Atom state);

    private:
        Display* display = nullptr;
        Window window = 0;
        Atom netWmState = 0;
        Atom netWmStateHidden = 0;
        bool mapped = true;
        bool wmHidden = false;
    };

}

#endif /* X11Visibility_h */
