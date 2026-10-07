// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WinInputRouter_h
#define WinInputRouter_h

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Input.h"

#include <vector>

namespace jasperengine {

    // Win32 input-message translation. The editor and a game cannot share a
    // WNDPROC -- the editor's hands messages to ImGui and keeps a virtual
    // pointer of its own -- so a game's does nothing but pass them here.
    class WinInputRouter {
    public:

        // Returns true when the message was fully handled and the caller should
        // return `result`; false when it should carry on to DefWindowProc, which
        // key messages need so WM_CHAR is still generated and Alt+F4 works.
        bool handleMessage(HWND window, UINT message, WPARAM wParam,
                           LPARAM lParam, LRESULT& result);

        // Keeps the tracked position in step when a game warps the pointer,
        // which reports no motion of its own while captured.
        void setMousePosition(double x, double y);

        // Caret area of the focused HTML UI text field, in client pixels.
        // Places the IME composition and candidate windows now, and again
        // whenever a composition starts.
        void setTextInputArea(float x, float y, float width, float height);

        // Drops whatever the IME is still composing, for a text field that
        // lost focus after the engine already dropped its own copy. Nothing
        // about that composition reaches the engine afterwards.
        void cancelComposition();

    private:
        void applyTextInputArea(HWND window);
        void handleComposition(HWND window, LPARAM lParam);
        void trackMouseLeave(HWND window);
        void reportMouseLeave();

        // The engine keeps seeing a continuous position while captured
        double mousePosX = 0.0;
        double mousePosY = 0.0;

        // TME_LEAVE is armed and the engine was told the pointer is inside
        bool mouseInside = false;

        // A composition string is shown in the document and was not committed
        bool compositionOpen = false;
        // Inside cancelComposition: IME messages it sends are not routed
        bool cancellingComposition = false;

        bool hasTextInputArea = false;
        float textInputX = 0.0f;
        float textInputY = 0.0f;
        float textInputWidth = 0.0f;
        float textInputHeight = 0.0f;

        // Touch pointers between WM_POINTERDOWN and WM_POINTERUP, so a lost
        // capture cancels only fingers the engine knows about
        std::vector<int> activeTouches;
    };

}

#endif /* WinInputRouter_h */
