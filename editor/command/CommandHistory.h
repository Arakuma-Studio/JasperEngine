// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Command.h"
#include <vector>
#include <cstddef>
#include <functional>

namespace jasperengine::editor{

    class CommandHistory{

    private:
        std::vector<Command*> list;
        size_t index = 0;
        size_t sceneId = 0;

        // Play mode: the scene is restored from a snapshot on Stop, so commands made while
        // playing can't outlive the session, and edit-time commands can't be undone during it
        bool inPlaySession = false;
        size_t playFloor = 0;               // first command of the play session
        std::vector<Command*> editRedo;     // edit-time redo tail, set aside while playing

        size_t undoFloor() const { return inPlaySession ? playFloor : 0; }

    public:
        // Called after any command apply/undo/redo with the scene id and affectsStructure().
        static std::function<void(size_t, bool)> onSceneModified;
        // Called before an undo() or redo() step, with the history's scene id (the Pose Animation
        // window puts a posed character's rest bones back: the step runs on the rest, which the
        // pose saves again afterwards)
        static std::function<void(size_t)> beforeStep;

        // Asked before the scene's history changes, so a tool previewing edits live (the
        // Physics Placement tool's running gesture) can settle them first: the command then
        // records the true values. Returning false drops an undo or a redo; a command is always
        // added. Re-entrant: the tool's own commit comes through here too.
        enum class Change{
            Add,
            Undo,
            Redo
        };
        static std::function<bool(size_t, Change)> beforeChange;

        explicit CommandHistory(size_t sceneId = 0);
        ~CommandHistory();

        bool addCommand(Command* cmd);
        void addCommandNoMerge(Command* cmd);
        // A step the editor takes by itself (a model reloaded because its file changed on disk):
        // undone like any other, but placed at the current position without ending the redo tail
        // that a user edit would drop. In Play it goes below the play floor, so Stop keeps it.
        bool insertCommand(Command* cmd);

        // False when the history did not move, also when the command refused (Command::canUndo)
        bool undo();
        bool redo();

        bool canUndo() const;
        bool canRedo() const;

        void clear();

        // Undo groups (MCP batch): mark() before a run of commands, groupSince(mark) after
        // collapses them into one undo step. mark() also stops the next command merging
        // into the one before it. groupSince is a no-op (false) if the history was undone
        // past the mark, or a play session began or ended in between.
        size_t mark();
        bool groupSince(size_t mark);
        // Current position (what mark() returns, without its side effect)
        size_t position() const { return index; }
        // Deletes the redo tail (the commands after position()): a call that rolled itself back
        // with undo() leaves nothing to redo
        void dropRedo();

        void beginPlaySession();
        // rolledBack: the scene was restored to its pre-play state, so the play-time commands
        // are dropped and the edit redo tail comes back; otherwise the play-time commands stay
        void endPlaySession(bool rolledBack);
    };

}
