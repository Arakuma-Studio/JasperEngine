// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "save/SaveGameStore.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // A change of SaveGame slots from the Saved Data window or MCP (docs/specs/savegame.md R36):
    // Delete Slot, Restore Backup, Corrupt Newest Generation, Clear All Saves, in the project
    // history. It keeps the bytes of every file of the slots it changes (each bounded by the slot
    // cap), and undo writes them back (writeFileAtomic) unless the game saved one of the slots
    // since (its newest generation differs). A step that failed after it changed files stays in the
    // history (partialFailure). Saved data is not project content: no scene is marked modified.
    class SaveSlotCmd: public Command{
    public:
        enum class Action { Delete, Restore, Corrupt, ClearAll };

    private:
        Action action;
        std::string slot;               // "" for ClearAll
        uint64_t generation = 0;        // Restore
        savegame::CorruptMode mode = savegame::CorruptMode::Flip;

        struct SlotState {
            // file name -> bytes, before the step
            std::map<std::string, std::string> files;
            // the newest generation after the step (0: none), what undo checks
            uint64_t newestAfter = 0;
        };
        std::map<std::string, SlotState> slots;
        std::string error;
        // the step failed after it changed files (a delete stopped midway, a restore whose write
        // outlasted the flush): it stays in the history so undo puts them back
        std::string partial;

        bool changedFiles(const std::vector<std::string>& names) const;

        std::string label() const;
        bool snapshot(const std::vector<std::string>& names);

    public:
        SaveSlotCmd(Action action, std::string slot, uint64_t generation = 0, savegame::CorruptMode mode = savegame::CorruptMode::Flip);

        bool execute() override;
        void undo() override;
        std::string undoRefusal() const override;

        bool affectsStructure() const override { return false; }
        size_t memoryBytes() const override;
        std::string failure() const override { return error; }
        // Why the step stopped partway, empty when it finished (callers tell it with the undo hint)
        const std::string& partialFailure() const { return partial; }

        std::string describe() const override { return label(); }
        const char* typeName() const override { return "SaveSlotCmd"; }
    };

}
