// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Deletes files as one step of the project command history, the counterpart of
    // WriteFilesCmd: execute reads each file's contents into memory, then removes it; undo writes
    // them back (recreating missing parent directories). After execute and undo it runs
    // `afterChange` with the paths. Used for user pose templates (PoseTemplates::remove).
    // No dependency beyond the standard library (tested in jasperengine-pose-tests).
    class RemoveFilesCmd: public Command{
    public:
        RemoveFilesCmd(std::vector<std::filesystem::path> files, std::function<void(const std::vector<std::filesystem::path>&)> afterChange);

        // false (nothing removed) when a file is missing or cannot be read
        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

    private:
        std::vector<std::filesystem::path> files;
        std::vector<std::string> contents;
        std::function<void(const std::vector<std::filesystem::path>&)> afterChange;
    };

}
