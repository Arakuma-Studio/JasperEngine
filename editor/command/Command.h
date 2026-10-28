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
        // (its trusted roots: the project directory and its assets directory)
        std::vector<std::filesystem::path> recordedRoots = safewrite::Scope::currentRoots();

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
            return root.empty() ? std::string() : escapeFor(std::vector<std::filesystem::path>{root}, undo);
        }
        // ... inside one of `roots` (trusted roots: the project directory, its assets directory)
        std::string escapeFor(const std::vector<std::filesystem::path>& roots, bool undo) const {
            if (roots.empty()) return std::string();
            for (const Touched& touched : touchedPaths(undo)) {
                bool inside = false;
                for (const std::filesystem::path& root : roots) {
                    if (safewrite::escapeOf(root, touched.path, touched.entry).empty()) inside = true;
                }
                if (!inside) {
                    return safewrite::escapeOf(roots.front(), touched.path, touched.entry) +
                           (undo ? " Remove the link, then undo again." : " Remove the link, then redo again.");
                }
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
        virtual std::string undoRefusal() const { return escapeFor(recordedRoots, true); }
        virtual std::string redoRefusal() const { return escapeFor(recordedRoots, false); }

        virtual bool mergeWith(Command*) { return false; }

        // True if this can change what the Hierarchy tree shows. Defaults to true so a
        // new command is never silently stale.
        virtual bool affectsStructure() const { return true; }

        void setNoMerge() { mergeable = false; }
        unsigned long long serial() const { return historySerial; }
        bool canMerge() const { return mergeable; }

        // the first recorded root ("" none)
        std::filesystem::path writeRoot() const { return recordedRoots.empty() ? std::filesystem::path() : recordedRoots.front(); }
        const std::vector<std::filesystem::path>& writeRoots() const { return recordedRoots; }
        void setWriteRoot(std::filesystem::path root) { recordedRoots.clear(); if (!root.empty()) recordedRoots.push_back(std::move(root)); }
        void setWriteRoots(std::vector<std::filesystem::path> roots) { recordedRoots = std::move(roots); }
        // the recorded trusted root holding `path` once resolved, else the active write scope's
        // root when `path` is inside it (an MCP call undoing a step the GUI made)
        std::filesystem::path rootFor(const std::filesystem::path& path, bool entry = false) const {
            return recordedRoots.empty() ? safewrite::scopeRootFor(path) : safewrite::trustedRootFor(recordedRoots, path, entry);
        }
        // Why the last execute() failed ("" when it did not, or the command has no words for it):
        // what a refused redo reports
        virtual std::string failure() const { return std::string(); }
        // undoProblem, cleared
        std::string takeUndoProblem() { return std::exchange(undoProblem, std::string()); }
    };

}
