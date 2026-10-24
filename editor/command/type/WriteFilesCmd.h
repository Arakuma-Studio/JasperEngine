// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Writes (creates or replaces) files as one step of the project command history; undo
    // puts back the previous contents (or deletes files that did not exist, and the
    // directories it created). After execute and undo it runs `afterChange` with the paths.
    // Used by the MCP file tools (w4::writeFilesUndoable) and the pose animation document.
    //
    // A non-empty merge key (setMergeKey) merges the command into the previous one when that
    // one has the same key and writes the same files: the merged step undoes to the contents
    // before the first of them (drags, slider edits).
    //
    // All or nothing: when a file can't be written (a path component that is a file, a
    // read-only directory, a full disk), the files written before it get their previous
    // content back and what it created is removed, so a failed execute changes nothing
    // (CommandHistory::addCommand then drops it without an undo step); reportFailureTo()
    // receives which file failed and why.
    class WriteFilesCmd: public Command{
    public:
        struct Item {
            std::filesystem::path path;     // absolute
            std::string content;
        };

        WriteFilesCmd(std::vector<Item> items, std::function<void(const std::vector<std::filesystem::path>&)> afterChange);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* previousCommand) override;
        bool affectsStructure() const override { return false; }

        void setMergeKey(const std::string& key) { mergeKey = key; }
        const std::string& getMergeKey() const { return mergeKey; }
        // Where execute() writes its failure ("can't write <path>: <reason>; ..."): the
        // command is deleted by the history when it fails, the sink outlives it
        void reportFailureTo(std::shared_ptr<std::string> sink) { failureSink = std::move(sink); }

    private:
        struct Previous {
            bool existed = false;
            std::string content;
        };
        std::vector<Item> items;
        std::vector<Previous> previous;
        std::vector<std::filesystem::path> createdDirs;   // every directory execute() created
        bool captured = false;
        std::string mergeKey;
        std::function<void(const std::vector<std::filesystem::path>&)> afterChange;
        std::shared_ptr<std::string> failureSink;

        std::vector<std::filesystem::path> paths() const;
        // items[i] back to what it was before execute (false when that failed)
        bool restore(size_t i);
        // the created directories that are empty, deepest first
        void removeCreatedDirs();
        void fail(const std::string& message);
    };

}
