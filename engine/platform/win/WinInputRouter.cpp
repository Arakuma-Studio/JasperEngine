// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WinInputRouter.h"

#include "KeyCodesWin.h"
#include "SystemWin.h"
#include "WindowWin.h"
#include "WinPen.h"

#include "Engine.h"
#include "Input.h"

#include <imm.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cmath>
#include <string>

// Windows 8 and later, like the pointer messages of WinPen.h
#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

using namespace jasperengine;

namespace {

int mouseButtonFromMessage(UINT message, WPARAM wParam) {
    switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            return D_MOUSE_BUTTON_LEFT;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            return D_MOUSE_BUTTON_RIGHT;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            return D_MOUSE_BUTTON_MIDDLE;
        default:
            return GET_XBUTTON_WPARAM(wParam) == XBUTTON1
                ? D_MOUSE_BUTTON_4 : D_MOUSE_BUTTON_5;
    }
}

// While captured the pointer is clipped in place, so WM_MOUSEMOVE stops
// carrying information and only the WM_INPUT deltas are used.
bool isCaptured() {
    return Engine::getMouseMode() == MouseMode::CAPTURED;
}

bool isInClientArea(HWND window, LPARAM lParam) {
    RECT client{};
    GetClientRect(window, &client);
    const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    return PtInRect(&client, point) != FALSE;
}

// Only touch is taken from the pointer messages. Without EnableMouseInPointer
// the mouse never sends them, and a pen left to DefWindowProc is promoted to
// WM_MOUSE* as before (its pressure is read on the way). On Windows 7 touch
// arrives as mouse messages and no pointer message is sent.
bool isTouchPointer(UINT32 pointerId) {
    return winPointerType(pointerId) == WIN_POINTER_TYPE_TOUCH;
}

// One of the IME strings (GCS_COMPSTR, GCS_RESULTSTR). The sizes are in bytes.
std::wstring compositionString(HIMC context, DWORD index) {
    const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes <= 0) return {};
    std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    const LONG copied = ImmGetCompositionStringW(
        context, index, text.data(), static_cast<DWORD>(bytes));
    if (copied <= 0) return {};
    text.resize(std::min(text.size(), static_cast<size_t>(copied) / sizeof(wchar_t)));
    return text;
}

// The IME counts UTF-16 units; the engine counts code points.
int codepointIndex(const std::wstring& text, LONG units) {
    const size_t end = std::min(text.size(), static_cast<size_t>(units));
    int count = 0;
    for (size_t i = 0; i < end; ++i) {
        // A low surrogate after a high one finishes the same code point
        const bool low = text[i] >= 0xDC00 && text[i] <= 0xDFFF;
        if (low && i > 0 && text[i - 1] >= 0xD800 && text[i - 1] <= 0xDBFF)
            continue;
        ++count;
    }
    return count;
}

std::string toUtf8(const std::wstring& text) {
    return winWideToUtf8(text.data(), static_cast<int>(text.size()));
}

}

void WinInputRouter::setMousePosition(double x, double y) {
    mousePosX = x;
    mousePosY = y;
}

void WinInputRouter::setTextInputArea(float x, float y, float width, float height) {
    textInputX = x;
    textInputY = y;
    textInputWidth = width;
    textInputHeight = height;
    hasTextInputArea = true;
    applyTextInputArea(WindowWin::handle());
}

// The composition string is drawn by the document at the caret, so the IME
// only needs its anchor there; the candidate list opens below the caret and
// never over it.
void WinInputRouter::applyTextInputArea(HWND window) {
    if (!window || !hasTextInputArea) return;
    HIMC context = ImmGetContext(window);
    if (!context) return;

    const LONG left = static_cast<LONG>(std::lround(textInputX));
    const LONG top = static_cast<LONG>(std::lround(textInputY));
    const LONG right = left + std::max<LONG>(static_cast<LONG>(std::lround(textInputWidth)), 1);
    const LONG bottom = top + std::max<LONG>(static_cast<LONG>(std::lround(textInputHeight)), 1);

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = POINT{left, top};
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = POINT{left, bottom};
    candidate.rcArea = RECT{left, top, right, bottom};
    ImmSetCandidateWindow(context, &candidate);

    ImmReleaseContext(window, context);
}

void WinInputRouter::cancelComposition() {
    // Forgotten first: anything the IME reports later about this
    // composition finds nothing open, so no cancel or commit is sent.
    compositionOpen = false;

    HWND window = WindowWin::handle();
    if (!window) return;
    HIMC context = ImmGetContext(window);
    if (!context) return;
    cancellingComposition = true;
    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    cancellingComposition = false;
    ImmReleaseContext(window, context);
}

// A commit and the next composition can come in one message (Korean commits
// a syllable as it starts the next one): commit first, then the composition.
void WinInputRouter::handleComposition(HWND window, LPARAM lParam) {
    std::wstring result;
    std::wstring composition;
    int cursor = -1;
    // A caret move inside the composition can come without GCS_COMPSTR
    const bool compositionChanged = (lParam & (GCS_COMPSTR | GCS_CURSORPOS)) != 0;

    // Everything is read before the engine runs, which may place the IME
    // windows again from inside these calls.
    HIMC context = ImmGetContext(window);
    if (context) {
        if (lParam & GCS_RESULTSTR)
            result = compositionString(context, GCS_RESULTSTR);
        if (compositionChanged) {
            composition = compositionString(context, GCS_COMPSTR);
            if (lParam & GCS_CURSORPOS) {
                const LONG position = ImmGetCompositionStringW(
                    context, GCS_CURSORPOS, nullptr, 0);
                if (position >= 0) cursor = codepointIndex(composition, position);
            }
        }
        ImmReleaseContext(window, context);
    }

    if (!result.empty()) {
        // Confirms the open composition in the document
        Engine::systemTextInput(toUtf8(result));
        compositionOpen = false;
    }

    if (!composition.empty()) {
        Engine::systemImeComposition(toUtf8(composition), cursor);
        compositionOpen = true;
    } else if (compositionOpen && (compositionChanged || lParam == 0)) {
        // Emptied by backspace, or cancelled (no flags at all). Attribute or
        // clause updates alone leave it as it is.
        Engine::systemImeCancel();
        compositionOpen = false;
    }
}

void WinInputRouter::trackMouseLeave(HWND window) {
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = window;
    TrackMouseEvent(&track);
    // Set even if tracking failed, so the engine is not told again per move
    mouseInside = true;
    Engine::systemMouseEnter();
}

void WinInputRouter::reportMouseLeave() {
    if (!mouseInside) return;
    mouseInside = false;
    Engine::systemMouseLeave();
}

bool WinInputRouter::handleMessage(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam, LRESULT& result) {
    switch (message) {
        case WM_INPUT: {
            if (!isCaptured()) break;
            LONG deltaX = 0;
            LONG deltaY = 0;
            if (!WindowWin::readRawMouseDelta(
                    reinterpret_cast<HRAWINPUT>(lParam), deltaX, deltaY))
                break;
            mousePosX += deltaX;
            mousePosY += deltaY;
            Engine::systemMouseMove(float(mousePosX), float(mousePosY),
                                    winKeyModifiers());
            break; // WM_INPUT still has to reach DefWindowProc
        }

        case WM_MOUSEMOVE:
            if (!winIsPointerMouseMessage())
                Input::setPenPressure(-1.0f);
            // A drag holding the capture keeps reporting moves outside the
            // client area; those are not the pointer coming back.
            if (!mouseInside && isInClientArea(window, lParam))
                trackMouseLeave(window);
            if (!isCaptured()) {
                mousePosX = GET_X_LPARAM(lParam);
                mousePosY = GET_Y_LPARAM(lParam);
                Engine::systemMouseMove(float(mousePosX), float(mousePosY),
                                        winKeyModifiers());
            }
            result = 0;
            return true;

        case WM_MOUSELEAVE:
            reportMouseLeave();
            result = 0;
            return true;

        case WM_MOUSEWHEEL:
            Engine::systemMouseScroll(
                0.0f, float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA),
                winKeyModifiers());
            result = 0;
            return true;

        case WM_MOUSEHWHEEL:
            // The engine takes positive x as left (GLFW's convention); Windows
            // reports a tilt to the right as positive.
            Engine::systemMouseScroll(
                -float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA), 0.0f,
                winKeyModifiers());
            result = 0;
            return true;

        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (!winIsPointerMouseMessage())
                Input::setPenPressure(-1.0f);
            if (!isCaptured()) {
                mousePosX = GET_X_LPARAM(lParam);
                mousePosY = GET_Y_LPARAM(lParam);
            }
            // Keeps drags alive when the pointer leaves the client area
            SetCapture(window);
            Engine::systemMouseDown(mouseButtonFromMessage(message, wParam),
                                    float(mousePosX), float(mousePosY),
                                    winKeyModifiers());
            result = (message == WM_XBUTTONDOWN) ? TRUE : 0;
            return true;

        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            if (!isCaptured()) {
                mousePosX = GET_X_LPARAM(lParam);
                mousePosY = GET_Y_LPARAM(lParam);
            }
            ReleaseCapture();
            Engine::systemMouseUp(mouseButtonFromMessage(message, wParam),
                                  float(mousePosX), float(mousePosY),
                                  winKeyModifiers());
            // A drag released outside the window may not get WM_MOUSELEAVE
            if (!isCaptured() && !isInClientArea(window, lParam))
                reportMouseLeave();
            result = (message == WM_XBUTTONUP) ? TRUE : 0;
            return true;

        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP: {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            // -1 for a finger
            Input::setPenPressure(winPenPressure(pointerId));
            if (!isTouchPointer(pointerId)) break;

            // Screen coordinates, unlike the mouse messages
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const int pointer = static_cast<int>(pointerId);
            const float x = float(point.x);
            const float y = float(point.y);
            auto active = std::find(activeTouches.begin(), activeTouches.end(), pointer);

            if (message == WM_POINTERDOWN) {
                if (active == activeTouches.end()) activeTouches.push_back(pointer);
                Engine::systemTouchStart(pointer, x, y);
            } else if (active != activeTouches.end()) {
                if (message == WM_POINTERUPDATE) {
                    Engine::systemTouchMove(pointer, x, y);
                } else {
                    activeTouches.erase(active);
                    Engine::systemTouchEnd(pointer, x, y);
                }
            }
            // Handled, so Windows does not also promote it to mouse messages
            result = 0;
            return true;
        }

        case WM_POINTERCAPTURECHANGED: {
            const int pointer = static_cast<int>(GET_POINTERID_WPARAM(wParam));
            auto active = std::find(activeTouches.begin(), activeTouches.end(), pointer);
            if (active == activeTouches.end()) break;
            activeTouches.erase(active);
            Engine::systemTouchCancel(pointer);
            result = 0;
            return true;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int key = winKeyFromMessage(wParam, lParam);
            if (key != D_KEY_UNKNOWN)
                Engine::systemKeyDown(key, (HIWORD(lParam) & KF_REPEAT) != 0,
                                      winKeyModifiers());
            break; // DefWindowProc turns this into WM_CHAR
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int key = winKeyFromMessage(wParam, lParam);
            if (key != D_KEY_UNKNOWN)
                Engine::systemKeyUp(key, false, winKeyModifiers());
            break;
        }

        case WM_CHAR:
        case WM_SYSCHAR: {
            // One UTF-16 unit. Both halves of a surrogate pair pass (>= 32);
            // Engine::systemCharInput combines them into one character.
            const wchar_t codepoint = static_cast<wchar_t>(wParam);
            // Other control characters would show up as boxes
            if ((codepoint >= 32 && codepoint != 0x7f) || codepoint == '\t' ||
                codepoint == '\b' || codepoint == '\r' || codepoint == 0x1b)
                Engine::systemCharInput(codepoint);
            result = 0;
            return true;
        }

        // While an HTML UI text field has focus the document draws the
        // composition itself, so the IME messages stop here instead of at
        // DefWindowProc, which would open the IME's own composition window and
        // send the commit again as WM_IME_CHAR / WM_CHAR. Otherwise they keep
        // going to DefWindowProc, which the component InputField relies on.
        // The IME's answer to cancelComposition goes to DefWindowProc, which
        // closes its own windows if it had any open.
        case WM_IME_STARTCOMPOSITION:
            if (cancellingComposition || !Engine::wantsTextComposition()) break;
            compositionOpen = false;
            applyTextInputArea(window);
            result = 0;
            return true;

        case WM_IME_COMPOSITION:
            if (cancellingComposition || !Engine::wantsTextComposition()) break;
            handleComposition(window, lParam);
            result = 0;
            return true;

        case WM_IME_ENDCOMPOSITION:
            if (cancellingComposition || !Engine::wantsTextComposition()) {
                // The field lost focus mid-composition: the engine dropped it
                compositionOpen = false;
                break;
            }
            // Closed without a commit, e.g. Escape
            if (compositionOpen) Engine::systemImeCancel();
            compositionOpen = false;
            result = 0;
            return true;

        case WM_IME_CHAR:
            if (cancellingComposition || !Engine::wantsTextComposition()) break;
            result = 0;
            return true;

        default:
            break;
    }

    return false;
}
