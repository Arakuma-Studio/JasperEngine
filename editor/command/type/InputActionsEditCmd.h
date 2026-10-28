// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor{

    // An edit of an .inputactions asset (the Input Actions window, edit_input_actions): the
    // file's text before and after, as one step of the project history (S24 names: "Add Action
    // Gameplay/Jump", "Edit Input Actions (3 Changes)"). Writes go through the atomic writer;
    // while the game plays with that asset loaded, the edit applies live.
    class InputActionsEditCmd: public Command{
    private:
        std::filesystem::path file;     // absolute
        std::string before;
        std::string after;
        std::string label;
        bool created;                   // the first edit made the file: undo removes it
        bool write(const std::string& text) const;

    public:
        InputActionsEditCmd(std::filesystem::path file, std::string before, std::string after, std::string label,
                            bool created = false);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return label; }
        const char* typeName() const override { return "InputActionsEditCmd"; }
        bool isNoOp() const override { return before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + before.size() + after.size(); }
        bool releaseOffThread() const override { return true; }

        // bumped by every write of an asset through this command: windows showing it re-read
        static uint64_t getRevision();
    };

}
