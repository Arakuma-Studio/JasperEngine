// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R37, §6.2)
// - Purpose: the text an input method is composing (the preedit, e.g. げきあつ before it is
//   converted to 激熱), so the editor can draw it inline at the caret. The platform backends fill
//   it: macOS from `setMarkedText` (libs/imgui/backends/imgui_impl_osx.mm, always), Windows from
//   WM_IME_COMPOSITION while inlineWanted() (editor/backend/BackendWin.cpp); on Linux the input
//   method draws its own preedit at the caret (over-the-spot XIM) and this stays empty.
// - Committed text never goes through here: it arrives as ImGui input characters, like typing.
// - inlineWanted: set by the widget that draws the preedit itself (the Title Designer's canvas
//   text editor) while it has the keyboard; the Windows backend then suppresses the system's
//   composition window. Elsewhere the platform's default composition UI stays.
// - Thread-safety: main thread only (the backends' event handlers and the UI run there).

#pragma once

#include <string>

namespace jasperengine::editor {

    struct ImePreedit {
        std::string text;       // UTF-8
        int caretByte = 0;      // the input method's caret inside `text`, in bytes
        bool active = false;    // a composition is in progress (text is not empty)

        static const ImePreedit& current();
        // an empty text clears; the caret is clamped into the text and onto a code point start
        static void set(ImePreedit preedit);
        static void clear();
        static void setInlineWanted(bool wanted);
        static bool inlineWanted();
    };

}
