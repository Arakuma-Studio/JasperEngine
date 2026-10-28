// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "util/SafeWrite.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor{

    class CommandHistory;

    class Command{
        friend class CommandHistory;
        // a history step's identity, unique for the editor's run (CommandHistory sets it when the
        // command becomes a step): a freed command's address can come back, its serial can't
        unsigned long long historySerial = 0;
        // the write scope the command was made in (util/SafeWrite.h: an MCP call's project
        // directory; "" outside one): CommandHistory runs its execute, undo and redo in a Scope of
        // it again, and its file operations stay beneath it
        std::filesystem::path recordedRoot = safewrite::Scope::current();

    protected:
        bool mergeable = true;
        // set by undo() when a file it had to put back could not be (a link planted since its
        // check): CommandHistory::lastUndoProblem reports it, the console says it
        std::string undoProblem;

    public:
        virtual ~Command() {}

        virtual bool execute() = 0;
        virtual void undo() = 0;

        // A file or folder the step changes when undone (undo) or done again: `entry` true when
        // the entry itself is acted on (a removal, a rename, a folder made), false for a file
        // whose bytes are written (a live link at it is written through)
        struct Touched {
            std::filesystem::path path;
            bool entry = false;
        };
        virtual std::vector<Touched> touchedPaths(bool /*undo*/) const { return {}; }

        // "" when every touched path stays inside `root` once links are resolved, else why, with
        // the fix ("Remove the link, then undo again.")
        std::string escapeFor(const std::filesystem::path& root, bool undo) const {
            if (root.empty()) return std::string();
            for (const Touched& touched : touchedPaths(undo)) {
                const std::string escape = safewrite::escapeOf(root, touched.path, touched.entry);
                if (!escape.empty()) return escape + (undo ? " Remove the link, then undo again." : " Remove the link, then redo again.");
            }
            return std::string();
        }

        // False when undoing or redoing it now would break the project (a child scene cycle; a
        // file it changes resolving outside its write root through a link: escapeFor), so the
        // history stays where it is
        virtual bool canUndo() const { return undoRefusal().empty(); }
        virtual bool canRedo() const { return redoRefusal().empty(); }
        // Why canUndo() / canRedo() refuse now, with how to fix it ("" when they don't, or the
        // command has no words for it): what CommandHistory::lastRefusal reports (the MCP undo
        // tool, the Edit menu)
        virtual std::string undoRefusal() const { return escapeFor(recordedRoot, true); }
        virtual std::string redoRefusal() const { return escapeFor(recordedRoot, false); }

        virtual bool mergeWith(Command*) { return false; }

        // True if this can change what the Hierarchy tree shows. Defaults to true so a
        // new command is never silently stale.
        virtual bool affectsStructure() const { return true; }

        void setNoMerge() { mergeable = false; }
        unsigned long long serial() const { return historySerial; }
        bool canMerge() const { return mergeable; }

        const std::filesystem::path& writeRoot() const { return recordedRoot; }
        void setWriteRoot(std::filesystem::path root) { recordedRoot = std::move(root); }
        // writeRoot(), else the active write scope's root when `path` is inside it (an MCP call
        // undoing a step the GUI made)
        std::filesystem::path rootFor(const std::filesystem::path& path) const {
            return recordedRoot.empty() ? safewrite::scopeRootFor(path) : recordedRoot;
        }
        // Why the last execute() failed ("" when it did not, or the command has no words for it):
        // what a refused redo reports
        virtual std::string failure() const { return std::string(); }
        // undoProblem, cleared
        std::string takeUndoProblem() { return std::exchange(undoProblem, std::string()); }
    };

}
