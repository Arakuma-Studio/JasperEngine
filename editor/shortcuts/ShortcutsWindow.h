// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Edit > Shortcuts... (Unity's Shortcuts window, S25 R6-R7) and Help > Keyboard Shortcuts (R5):
// the command catalog as a table, Command / Shortcut / Context, grouped, with a search field
// (Filter mode). In the editor mode a click on a shortcut captures the next chord: Enter or a
// click elsewhere keeps it, Esc cancels, Backspace clears. While it listens the dispatcher is
// off and the captured key does not fire another command. A chord another command already has
// in the same context asks "Ctrl+D is already used by Edit / Duplicate" with Reassign / Cancel;
// one that only shadows a panel binding is shown, not refused. Each row and the whole table
// reset to the defaults; reference rows (mouse gestures) are read-only. Changes are written to
// shortcuts.yaml at once. The reference mode lists the same rows read-only, with an
// "Edit Shortcuts..." button.

#include "CommandCatalog.h"
#include "ShortcutDispatcher.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class ShortcutsWindow {
    public:
        ShortcutsWindow(CommandCatalog& catalog, ShortcutDispatcher& dispatcher);

        void openEditor();
        void openReference();
        bool isOpen() const { return open; }
        void show();

        // The chord being captured, for tests and agents ("" when not capturing)
        const std::string& capturingCommand() const { return captureId; }

        // Rebinds and saves (the window's Reassign and MCP set_shortcut's force): the other
        // command loses the chord; false with *error
        static bool reassign(CommandCatalog& catalog, const std::string& id, const std::vector<Chord>& chords, std::string* error);
        // Writes shortcuts.yaml and rebuilds nothing else (the menus follow the catalog)
        static bool save(CommandCatalog& catalog, std::string* error);

    private:
        CommandCatalog& catalog;
        ShortcutDispatcher& dispatcher;
        bool open = false;
        bool openRequested = false;
        bool reference = false;
        char searchBuffer[128] = "";
        std::string group;           // "" every group
        std::string captureId;       // the row listening
        int captureSlot = 0;         // which of its bindings (0 the first, 1 adds a second)
        Chord captured;
        bool conflictPending = false;
        std::string conflictWith;
        std::vector<Chord> pendingChords;    // what Reassign applies

        void drawTable();
        void beginCapture(const std::string& id, int slot);
        void endCapture(bool keep);
        void pollCapture();
        void drawConflictPopup();
        void apply(const std::string& id, std::vector<Chord> chords);
    };

}
