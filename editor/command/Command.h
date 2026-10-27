// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor{

    class CommandHistory;

    class Command{
        friend class CommandHistory;
        // a history step's identity, unique for the editor's run (CommandHistory sets it when the
        // command becomes a step): a freed command's address can come back, its serial can't
        unsigned long long historySerial = 0;

    protected:
        bool mergeable = true;

    public:
        virtual ~Command() {}

        virtual bool execute() = 0;
        virtual void undo() = 0;

        // False when undoing or redoing it now would break the project (a child scene
        // cycle), so the history stays where it is
        virtual bool canUndo() const { return true; }
        virtual bool canRedo() const { return true; }

        virtual bool mergeWith(Command*) { return false; }

        // True if this can change what the Hierarchy tree shows. Defaults to true so a
        // new command is never silently stale.
        virtual bool affectsStructure() const { return true; }

        void setNoMerge() { mergeable = false; }
        unsigned long long serial() const { return historySerial; }
        bool canMerge() const { return mergeable; }
    };

}

