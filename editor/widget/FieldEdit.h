// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The edit contract of the editor's field widgets (docs/specs/S27-editor-tools.md §6.1, R3): a
// field (colorField, curveField, gradientField) edits a copy of its value and reports phases; the
// glue that owns the value maps them to undo:
//   Begin     -> FieldEditSession::begin(history, "Change <Label>", window)
//   Changing  -> merging PropertyCmds for each selected GameObject (inside that one step)
//   Committed -> nothing: the window commits the session itself (close, Enter, retarget)
//   Cancelled -> nothing: the window cancels the session itself (Esc), which restores the values
//   Applied   -> the value changed in one go outside a window (a field's menu: Paste, Reset, a
//                preset; a mode switch): one undo step of its own
// The widgets never touch a CommandHandle; a glue without undo (a preference) just writes the
// value on Changing, Applied and Cancelled (the field gives back the original value on Cancelled).

#include <cstdint>
#include <string>

namespace jasperengine::editor {
    class CommandHistory;
}

namespace jasperengine::editor::ui {

    enum class EditPhase : uint8_t {
        None,
        Begin,       // the field's window opened on this field (the value is unchanged)
        Changing,    // the value changed (live)
        Committed,   // the window closed or moved on: the session's step is done
        Cancelled,   // Esc: the value is the original again
        Applied      // the value changed, and that edit is complete
    };

    // The one undo step of a field window's session: an S24 InteractiveEdit on the edited scene's
    // history. One at a time; a begin commits an open one first. While open, the App's
    // per-widget drag step (CommandHistory::beforeRecord) stays out of the way, so every slider
    // of the window adds to this one step.
    class FieldEditSession {
    public:
        static void begin(CommandHistory* history, std::string name, const void* owner);
        // commit / cancel the session `owner` began (nothing when another owner's is open)
        static void commit(const void* owner);
        static void cancel(const void* owner);
        static bool isOpen();
        static bool isOpenFor(const void* owner);
    };

    // The glue's answer to Begin, whichever window opened (Color, Curve, Gradient): where its
    // edits go and the step's "Change <label>". A window sets itself as the handler as it opens.
    using BeginEditHandler = void (*)(CommandHistory* history, const char* label);
    void setBeginEditHandler(BeginEditHandler handler);
    void beginFieldEdit(CommandHistory* history, const char* label);

}
