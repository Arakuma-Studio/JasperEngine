// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <filesystem>
#include <functional>
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

    private:
        struct Previous {
            bool existed = false;
            std::string content;
        };
        std::vector<Item> items;
        std::vector<Previous> previous;
        std::vector<std::filesystem::path> createdDirs;
        bool captured = false;
        std::string mergeKey;
        std::function<void(const std::vector<std::filesystem::path>&)> afterChange;

        std::vector<std::filesystem::path> paths() const;
    };

}
