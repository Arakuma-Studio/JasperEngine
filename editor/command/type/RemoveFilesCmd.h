// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Deletes files as one step of the project command history, the counterpart of
    // WriteFilesCmd: execute reads each file's contents into memory, then removes it; undo writes
    // them back (recreating missing parent directories). After execute and undo it runs
    // `afterChange` with the paths. Used for user pose templates (PoseTemplates::remove).
    class RemoveFilesCmd: public Command{
    public:
        RemoveFilesCmd(std::vector<std::filesystem::path> files, std::function<void(const std::vector<std::filesystem::path>&)> afterChange);

        // false (nothing removed) when a file is missing or cannot be read, or one cannot be
        // removed (the files removed before it are written back)
        bool execute() override;
        std::vector<Touched> touchedPaths(bool undo) const override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        // "Delete File walk.poseanim", "Delete 2 Files" (S24)
        std::string describe() const override {
            return std::string("Delete ") + (files.size() == 1 ? "File " : "") + CommandNames::files(files);
        }
        const char* typeName() const override { return "RemoveFilesCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this);
            for (const std::filesystem::path& file : files) bytes += file.native().size();
            for (const std::string& content : contents) bytes += content.capacity();
            return bytes;
        }
        // the destructor only frees paths, contents and the callback (S24 R8)
        bool releaseOffThread() const override { return true; }

    private:
        std::vector<std::filesystem::path> files;
        std::vector<std::string> contents;
        std::function<void(const std::vector<std::filesystem::path>&)> afterChange;
    };

}
