// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The screen DC read follows OpenEngine's Engine/Modules/Platform/Source/Window.cpp
// (GetScreenPixelColor: GetDC(NULL) + GetPixel), Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License. See NOTICE.md. Here one BitBlt of the 9x9 block per frame.

#include "platform/ScreenColor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include "htmlui/compat/css/RewriteSession.h"
#include "object/ui/Button.h"

using namespace jasperengine::editor;

namespace {

    struct Session {
        bool active = false;
        HWND capture = nullptr;
        bool leftDown = false;
        bool rightDown = false;
    };

    Session& session() {
        static Session s;
        return s;
    }

}

bool screencolor::liveSupported() {
    return true;
}

bool screencolor::waylandSession() {
    return false;
}

bool screencolor::begin(std::string& why) {
    Session& s = session();
    if (s.active) return true;
    s.capture = GetActiveWindow();
    if (!s.capture) s.capture = GetForegroundWindow();
    if (!s.capture) {
        why = "no editor window to capture the mouse";
        return false;
    }
    // the clicks come to the editor's window (which ignores them while sampling), wherever the
    // cursor is
    SetCapture(s.capture);
    SetCursor(LoadCursor(nullptr, IDC_CROSS));
    s.active = true;
    // a button already down when sampling starts is not a pick
    s.leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    s.rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    return true;
}

void screencolor::end() {
    Session& s = session();
    if (!s.active) return;
    if (GetCapture() == s.capture) ReleaseCapture();
    s = Session();
}

bool screencolor::active() {
    return session().active;
}

bool screencolor::cursor(int& x, int& y) {
    POINT point;
    if (!GetCursorPos(&point)) return false;
    x = point.x;
    y = point.y;
    // the crosshair stays while sampling (the window would set its own cursor on a move)
    if (session().active) SetCursor(LoadCursor(nullptr, IDC_CROSS));
    return true;
}

bool screencolor::read9x9(int x, int y, uint8_t rgb[kLoupePixels * 3]) {
    std::memset(rgb, 0, kLoupePixels * 3);
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kLoupeSize;
    info.bmiHeader.biHeight = -kLoupeSize;   // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (memory && bitmap && bits) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        // the virtual screen's pixels (every monitor); what lies off every monitor stays black
        PatBlt(memory, 0, 0, kLoupeSize, kLoupeSize, BLACKNESS);
        ok = BitBlt(memory, 0, 0, kLoupeSize, kLoupeSize, screen, x - kLoupeSize / 2, y - kLoupeSize / 2, SRCCOPY | CAPTUREBLT) != 0;
        GdiFlush();
        const uint8_t* bgra = static_cast<const uint8_t*>(bits);
        for (int i = 0; i < kLoupePixels; i++) {
            rgb[i * 3 + 0] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4 + 0];
        }
        SelectObject(memory, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

screencolor::Button screencolor::pollButton() {
    Session& s = session();
    if (!s.active) return Button::Idle;
    const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    Button result = Button::Idle;
    if (left && !s.leftDown) result = Button::Pick;
    else if (right && !s.rightDown) result = Button::Cancel;
    s.leftDown = left;
    s.rightDown = right;
    return result;
}

bool screencolor::toWindow(void* window, int x, int y, float& localX, float& localY) {
    HWND hwnd = static_cast<HWND>(window);
    if (!hwnd) return false;
    POINT point{x, y};
    if (!ScreenToClient(hwnd, &point)) return false;
    RECT client;
    if (!GetClientRect(hwnd, &client)) return false;
    if (point.x < client.left || point.y < client.top || point.x >= client.right || point.y >= client.bottom) return false;
    localX = (float)point.x;
    localY = (float)point.y;
    return true;
}

bool screencolor::pickOnceSupported() {
    return false;
}

void screencolor::pickOnce(std::function<void(bool, float, float, float)> done) {
    if (done) done(false, 0.0f, 0.0f, 0.0f);
}
