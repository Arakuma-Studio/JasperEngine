// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The root-window XGetImage read follows OpenEngine's Engine/Modules/Platform/Source/Window.cpp
// (GetScreenPixelColor), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.

#include "platform/ScreenColor.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#include <cstdlib>
#include <cstring>

using namespace jasperengine::editor;

namespace {

    struct Session {
        Display* display = nullptr;
        Window root = 0;
        Cursor crosshair = 0;
        bool grabbed = false;
        int screenWidth = 0;
        int screenHeight = 0;
    };

    Session& session() {
        static Session s;
        return s;
    }

    // the shift and width of a channel mask (0x00ff0000: 16, 8)
    void maskShape(unsigned long mask, int& shift, int& bits) {
        shift = 0;
        bits = 0;
        if (mask == 0) return;
        while (!(mask & 1ul)) {
            mask >>= 1;
            shift++;
        }
        while (mask & 1ul) {
            mask >>= 1;
            bits++;
        }
    }

    uint8_t channel(unsigned long pixel, unsigned long mask) {
        int shift, bits;
        maskShape(mask, shift, bits);
        if (bits == 0) return 0;
        const unsigned long value = (pixel & mask) >> shift;
        const unsigned long maxValue = (1ul << bits) - 1ul;
        return (uint8_t)((value * 255ul + maxValue / 2ul) / maxValue);
    }

    // copies the image's pixels into rgb at (dx, dy) of the 9x9 block
    void copyImage(XImage* image, int width, int height, int dx, int dy, uint8_t* rgb) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const unsigned long pixel = XGetPixel(image, x, y);
                uint8_t* out = rgb + ((dy + y) * screencolor::kLoupeSize + (dx + x)) * 3;
                out[0] = channel(pixel, image->red_mask);
                out[1] = channel(pixel, image->green_mask);
                out[2] = channel(pixel, image->blue_mask);
            }
        }
    }

    // reads the part of the block inside `window` (its size w x h) at its local (x0, y0)
    bool readWindow(Display* display, Window window, int w, int h, int x0, int y0, uint8_t* rgb) {
        const int half = screencolor::kLoupeSize / 2;
        int left = x0 - half, top = y0 - half;
        int right = x0 + half + 1, bottom = y0 + half + 1;
        const int clipLeft = left < 0 ? 0 : left;
        const int clipTop = top < 0 ? 0 : top;
        const int clipRight = right > w ? w : right;
        const int clipBottom = bottom > h ? h : bottom;
        if (clipRight <= clipLeft || clipBottom <= clipTop) return false;
        XImage* image = XGetImage(display, window, clipLeft, clipTop, (unsigned)(clipRight - clipLeft), (unsigned)(clipBottom - clipTop),
                                  AllPlanes, ZPixmap);
        if (!image) return false;
        copyImage(image, clipRight - clipLeft, clipBottom - clipTop, clipLeft - left, clipTop - top, rgb);
        XDestroyImage(image);
        return true;
    }

}

bool screencolor::liveSupported() {
    return true;
}

bool screencolor::waylandSession() {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return wayland && *wayland;
}

bool screencolor::begin(std::string& why) {
    Session& s = session();
    if (s.display) return true;
    s.display = XOpenDisplay(nullptr);
    if (!s.display) {
        why = "cannot open the X display";
        return false;
    }
    s.root = DefaultRootWindow(s.display);
    XWindowAttributes attributes;
    if (XGetWindowAttributes(s.display, s.root, &attributes)) {
        s.screenWidth = attributes.width;
        s.screenHeight = attributes.height;
    }
    s.crosshair = XCreateFontCursor(s.display, XC_crosshair);
    // every click goes to this connection until end(): no widget, no other window gets it
    s.grabbed = XGrabPointer(s.display, s.root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                             GrabModeAsync, None, s.crosshair, CurrentTime) == GrabSuccess;
    XFlush(s.display);
    if (!s.grabbed) {
        end();
        why = "another program holds the pointer";
        return false;
    }
    return true;
}

void screencolor::end() {
    Session& s = session();
    if (!s.display) return;
    if (s.grabbed) XUngrabPointer(s.display, CurrentTime);
    if (s.crosshair) XFreeCursor(s.display, s.crosshair);
    XCloseDisplay(s.display);
    s = Session();
}

bool screencolor::active() {
    return session().display != nullptr;
}

bool screencolor::cursor(int& x, int& y) {
    Session& s = session();
    if (!s.display) return false;
    Window rootReturn, childReturn;
    int winX, winY;
    unsigned int mask;
    return XQueryPointer(s.display, s.root, &rootReturn, &childReturn, &x, &y, &winX, &winY, &mask);
}

bool screencolor::read9x9(int x, int y, uint8_t rgb[kLoupePixels * 3]) {
    Session& s = session();
    std::memset(rgb, 0, kLoupePixels * 3);
    if (!s.display) return false;
    if (!waylandSession()) return readWindow(s.display, s.root, s.screenWidth, s.screenHeight, x, y, rgb);
    // XWayland's root holds no Wayland surface: only the X window under the cursor (the
    // editor's own) can be read
    Window rootReturn, child = 0;
    int rootX, rootY, winX, winY;
    unsigned int mask;
    if (!XQueryPointer(s.display, s.root, &rootReturn, &child, &rootX, &rootY, &winX, &winY, &mask) || child == 0) return false;
    XWindowAttributes attributes;
    Window unused;
    int localX, localY;
    if (!XGetWindowAttributes(s.display, child, &attributes) ||
        !XTranslateCoordinates(s.display, s.root, child, x, y, &localX, &localY, &unused)) {
        return false;
    }
    return readWindow(s.display, child, attributes.width, attributes.height, localX, localY, rgb);
}

screencolor::Button screencolor::pollButton() {
    Session& s = session();
    if (!s.display) return Button::Idle;
    Button result = Button::Idle;
    while (XPending(s.display) > 0) {
        XEvent event;
        XNextEvent(s.display, &event);
        if (event.type != ButtonPress || result != Button::Idle) continue;
        if (event.xbutton.button == Button1) result = Button::Pick;
        else if (event.xbutton.button == Button3) result = Button::Cancel;
    }
    return result;
}

bool screencolor::toWindow(void* window, int x, int y, float& localX, float& localY) {
    Session& s = session();
    Display* display = s.display;
    const bool own = display == nullptr;
    if (own) display = XOpenDisplay(nullptr);
    if (!display) return false;
    const Window target = (Window)(uintptr_t)window;
    XWindowAttributes attributes;
    Window child;
    int lx = 0, ly = 0;
    const bool ok = target != 0 && XGetWindowAttributes(display, target, &attributes) &&
                    XTranslateCoordinates(display, DefaultRootWindow(display), target, x, y, &lx, &ly, &child);
    if (own) XCloseDisplay(display);
    if (!ok || lx < 0 || ly < 0 || lx >= attributes.width || ly >= attributes.height) return false;
    localX = (float)lx;
    localY = (float)ly;
    return true;
}

bool screencolor::pickOnceSupported() {
    return false;
}

void screencolor::pickOnce(std::function<void(bool, float, float, float)> done) {
    if (done) done(false, 0.0f, 0.0f, 0.0f);
}
