// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>

namespace jasperengine::editor{

    struct KeepContext;  // command/PlayChangeReview.h

    class Command{
    protected:
        bool mergeable = true;

    public:
        virtual ~Command() {}

        virtual bool execute() = 0;
        virtual void undo() = 0;

        virtual bool mergeWith(Command*) { return false; }

        // True if this can change what the Hierarchy tree shows. Defaults to true so a
        // new command is never silently stale.
        virtual bool affectsStructure() const { return true; }

        // The undo step's name, Unity style "<Verb> <subject>": "Move Player",
        // "Set Intensity (3 Objects)", "Rename File a.png → b.png" (CommandNames has the
        // helpers). CommandHistory captures it after the first successful execute and after
        // every merge, so it is read while the state it names is live. Pure: every command
        // names itself, checked at compile time.
        virtual std::string describe() const = 0;
        // Stable id of the command's type ("PropertyCmd"), for tools and tests
        virtual const char* typeName() const = 0;
        // True after an execute that changed nothing: CommandHistory undoes the command and
        // records no step (a click on a field, a drag that came back where it started)
        virtual bool isNoOp() const { return false; }
        // The memory the command holds (YAML trees, clips, file contents, pixels) for the
        // undo memory budget, an estimate; read once at push and after each merge
        virtual size_t memoryBytes() const { return sizeof(*this); }
        // True only when the destructor frees memory and touches no editor, engine or GPU
        // state: a large one is then destroyed on a worker (BackgroundRelease)
        virtual bool releaseOffThread() const { return false; }
        // Play Mode Changes: adds the rows that keep this play-time edit after Stop
        // (PlayChangeReview). The default adds none, and the review lists the step as one
        // that can't be kept.
        virtual void collectKeepCommands(KeepContext&) const {}

        // What describe() said when the history last captured it (empty before)
        const std::string& name() const { return label; }

        void setNoMerge() { mergeable = false; }
        bool canMerge() const { return mergeable; }

    private:
        friend class CommandHistory;
        std::string label;
    };

}
