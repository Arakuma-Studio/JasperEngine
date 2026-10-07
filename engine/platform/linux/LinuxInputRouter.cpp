// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LinuxInputRouter.h"

#include "KeyCodesLinux.h"
#include "WindowLinux.h"

#include "Engine.h"

// XLookupKeysym and XLookupString are declared here, not in Xlib.h
#include <X11/Xutil.h>

#include <clocale>
#include <string>

using namespace jasperengine;

void LinuxInputRouter::setMousePosition(double x, double y) {
    mousePosX = x;
    mousePosY = y;
}

void LinuxInputRouter::handleKey(XKeyEvent& event, bool pressed) {
    const unsigned int keycode = event.keycode & 0xff;

    // Level-0 keysym keeps a key on its printed position, and where a layout
    // spells none the position it sits in answers for it
    const KeySym symbol = XLookupKeysym(&event, 0);
    int key = linuxKeyFromSym(symbol);
    if (key == D_KEY_UNKNOWN) key = linuxKeycodeTable(event.display)[keycode];
    const int mods = linuxKeyModifiers(event.state);

    // handleEvent swallows the release half of a repeat pair, so a press
    // arriving while the key is still held is a repeat.
    const bool repeat = pressed && keyHeld[keycode];
    keyHeld[keycode] = pressed;

    if (key != D_KEY_UNKNOWN) {
        if (pressed) Engine::systemKeyDown(key, repeat, mods);
        else Engine::systemKeyUp(key, false, mods);
    }

    if (!pressed) return;

    // The input method's UTF-8 text (XIM, as the editor backend); without one, XLookupString
    // in the "C" locale gives Latin-1, where each byte is the code point
    std::string text;
    if (inputContext) {
        char buffer[256];
        KeySym ignored = 0;
        Status status = 0;
        int length = Xutf8LookupString(inputContext, &event, buffer, sizeof(buffer), &ignored, &status);
        if (status == XBufferOverflow) {
            std::string large(static_cast<size_t>(length), '\0');
            length = Xutf8LookupString(inputContext, &event, &large[0], length, &ignored, &status);
            if (status == XLookupChars || status == XLookupBoth) text.assign(large.data(), static_cast<size_t>(length));
        } else if (status == XLookupChars || status == XLookupBoth) {
            text.assign(buffer, static_cast<size_t>(length));
        }
    } else {
        char buffer[32];
        KeySym ignored = 0;
        const int length = XLookupString(&event, buffer, sizeof(buffer), &ignored, nullptr);
        for (int i = 0; i < length; ++i) {
            const unsigned char codepoint = static_cast<unsigned char>(buffer[i]);
            if (codepoint < 0x80) {
                text += static_cast<char>(codepoint);
            } else {
                text += static_cast<char>(0xC0 | (codepoint >> 6));
                text += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        }
    }

    // other control characters (Ctrl+letter) are no text
    std::string filtered;
    for (unsigned char c : text) {
        if (c >= 32 || c == '\t' || c == '\b' || c == '\r' || c == 0x1b)
            filtered += static_cast<char>(c);
    }
    if (!filtered.empty()) Engine::systemTextInput(filtered);
}

void LinuxInputRouter::openInputMethod(Display* display, Window window) {
    closeInputMethod();
    if (!display || !window) return;

    // XIM needs the user's locale (a game starts in "C") and XMODIFIERS (@im=ibus...)
    if (!std::setlocale(LC_CTYPE, "") || !XSupportsLocale())
        std::setlocale(LC_CTYPE, "C");
    XSetLocaleModifiers("");

    inputMethod = XOpenIM(display, nullptr, nullptr, nullptr);
    if (!inputMethod) return;

    XIMStyles* styles = nullptr;
    if (XGetIMValues(inputMethod, XNQueryInputStyle, &styles, nullptr) != nullptr) styles = nullptr;
    auto offers = [styles](XIMStyle style) {
        if (!styles) return style == (XIMPreeditNothing | XIMStatusNothing);
        for (unsigned short i = 0; i < styles->count_styles; ++i)
            if (styles->supported_styles[i] == style) return true;
        return false;
    };

    // over-the-spot first, so the input method's window follows the caret (setTextInputArea)
    if (offers(XIMPreeditPosition | XIMStatusNothing)) {
        XPoint spot{0, 0};
        XVaNestedList preedit = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
        inputContext = XCreateIC(inputMethod,
                                 XNInputStyle, XIMPreeditPosition | XIMStatusNothing,
                                 XNClientWindow, window, XNFocusWindow, window,
                                 XNPreeditAttributes, preedit, nullptr);
        XFree(preedit);
        spotLocation = inputContext != nullptr;
    }
    if (!inputContext && offers(XIMPreeditNothing | XIMStatusNothing)) {
        inputContext = XCreateIC(inputMethod,
                                 XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                 XNClientWindow, window, XNFocusWindow, window, nullptr);
    }
    if (!inputContext && offers(XIMPreeditNone | XIMStatusNone)) {
        inputContext = XCreateIC(inputMethod,
                                 XNInputStyle, XIMPreeditNone | XIMStatusNone,
                                 XNClientWindow, window, XNFocusWindow, window, nullptr);
    }
    if (styles) XFree(styles);

    if (!inputContext) {
        XCloseIM(inputMethod);
        inputMethod = nullptr;
        return;
    }

    // the input method may need events the window does not select yet
    long filterMask = 0;
    if (XGetICValues(inputContext, XNFilterEvents, &filterMask, nullptr) == nullptr && filterMask != 0) {
        XWindowAttributes attributes{};
        XGetWindowAttributes(display, window, &attributes);
        XSelectInput(display, window, attributes.your_event_mask | filterMask);
    }
    XSetICFocus(inputContext);
}

void LinuxInputRouter::closeInputMethod() {
    if (inputContext) XDestroyIC(inputContext);
    if (inputMethod) XCloseIM(inputMethod);
    inputContext = nullptr;
    inputMethod = nullptr;
    spotLocation = false;
}

bool LinuxInputRouter::filterEvent(XEvent& event) {
    return inputContext && XFilterEvent(&event, None);
}

void LinuxInputRouter::setFocus(bool focused) {
    if (!inputContext) return;
    if (focused) XSetICFocus(inputContext);
    else XUnsetICFocus(inputContext);
}

void LinuxInputRouter::setTextInputArea(int x, int y, int height) {
    if (!inputContext || !spotLocation) return;
    // the spot is the baseline of the text: the bottom of the caret
    XPoint spot{static_cast<short>(x), static_cast<short>(y + height)};
    XVaNestedList preedit = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
    XSetICValues(inputContext, XNPreeditAttributes, preedit, nullptr);
    XFree(preedit);
}

bool LinuxInputRouter::handleEvent(XEvent& event) {
    Display* display = WindowLinux::display();

    switch (event.type) {
        case KeyPress:
            handleKey(event.xkey, true);
            return true;

        case KeyRelease: {
            // X11 sends a press/release pair for auto-repeat; drop the release
            // half so the engine sees a held key.
            if (display && XPending(display)) {
                XEvent next{};
                XPeekEvent(display, &next);
                if (next.type == KeyPress &&
                    next.xkey.keycode == event.xkey.keycode &&
                    next.xkey.time - event.xkey.time < 20)
                    return true;
            }
            handleKey(event.xkey, false);
            return true;
        }

        case MotionNotify: {
            const int mods = linuxKeyModifiers(event.xmotion.state);
            if (WindowLinux::isRelativeMouse()) {
                const int dx = event.xmotion.x - WindowLinux::lastWarpX();
                const int dy = event.xmotion.y - WindowLinux::lastWarpY();
                if (dx == 0 && dy == 0) return true; // our own warp
                mousePosX += dx;
                mousePosY += dy;
                WindowLinux::centerPointer();
            } else {
                mousePosX = event.xmotion.x;
                mousePosY = event.xmotion.y;
            }
            Engine::systemMouseMove(float(mousePosX), float(mousePosY), mods);
            return true;
        }

        case ButtonPress:
        case ButtonRelease: {
            const bool pressed = event.type == ButtonPress;
            const int mods = linuxKeyModifiers(event.xbutton.state);
            const unsigned int button = event.xbutton.button;

            // X11 reports the wheel as buttons 4-7; the engine follows GLFW: positive y is
            // up, positive x is left (button 6)
            if (button >= Button4 && button <= 7) {
                if (!pressed) return true;
                const float x = (button == 6) ? 1.0f : (button == 7) ? -1.0f : 0.0f;
                const float y = (button == Button4) ? 1.0f
                              : (button == Button5) ? -1.0f : 0.0f;
                Engine::systemMouseScroll(x, y, mods);
                return true;
            }

            if (!WindowLinux::isRelativeMouse()) {
                mousePosX = event.xbutton.x;
                mousePosY = event.xbutton.y;
            }
            int engineButton = D_MOUSE_BUTTON_LEFT;
            if (button == Button2) engineButton = D_MOUSE_BUTTON_MIDDLE;
            else if (button == Button3) engineButton = D_MOUSE_BUTTON_RIGHT;
            else if (button > 7) engineButton = int(button) - 8 + D_MOUSE_BUTTON_4;

            if (pressed)
                Engine::systemMouseDown(engineButton, float(mousePosX),
                                        float(mousePosY), mods);
            else
                Engine::systemMouseUp(engineButton, float(mousePosX),
                                      float(mousePosY), mods);
            return true;
        }

        case EnterNotify:
            Engine::systemMouseEnter();
            return true;

        case LeaveNotify:
            Engine::systemMouseLeave();
            return true;

        default:
            return false;
    }
}
