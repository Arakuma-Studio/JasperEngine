// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The eyedropper's screen reads (docs/specs/S27-editor-tools.md R4, §6.3), one implementation per
// platform: ScreenColorX11.cpp (XGetImage on the root window, one display connection per
// session, the pointer grabbed with a crosshair), ScreenColorWin.cpp (GDI BitBlt from the screen
// DC, the mouse captured with a crosshair) and ScreenColorMac.mm (NSColorSampler, the system loupe:
// one asynchronous pick, no live preview, no Screen Recording permission). The screen read
// approach follows OpenEngine's Engine/Modules/Platform/Source/Window.cpp (MIT, NOTICE.md),
// reading a 9x9 block once per frame instead of a pixel per call.
//
// Main thread. Coordinates are the OS screen's pixels (every monitor).

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor::screencolor {

    constexpr int kLoupeSize = 9;
    constexpr int kLoupePixels = kLoupeSize * kLoupeSize;

    enum class Button : uint8_t { Idle, Pick, Cancel };

    // A live session (Windows, X11): the pointer captured with a crosshair, so clicks reach no
    // widget. False with `why` where there is none (macOS uses pickOnce).
    bool liveSupported();
    bool begin(std::string& why);
    void end();
    bool active();

    // the OS cursor in screen pixels
    bool cursor(int& x, int& y);
    // the 9x9 sRGB pixels around (x, y), rows top first, RGB8 (243 bytes); pixels off every
    // screen are black. False when the screen cannot be read (a Wayland session outside our
    // own windows).
    bool read9x9(int x, int y, uint8_t rgb[kLoupePixels * 3]);
    // a button since the last poll: left picks, right cancels
    Button pollButton();

    // a screen point in a native window's client area (`window`: an ImGuiViewport's
    // PlatformHandleRaw); false when the window does not contain it
    bool toWindow(void* window, int x, int y, float& localX, float& localY);

    // macOS: the system colour sampler, once; done(picked, sRGB 0-1) on the main thread
    bool pickOnceSupported();
    void pickOnce(std::function<void(bool picked, float r, float g, float b)> done);

    // a Wayland session (the X11 editor under XWayland): only the editor's windows can be read
    bool waylandSession();

}
