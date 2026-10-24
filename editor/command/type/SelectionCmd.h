// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandHistory.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor{

    class Project;

    // A selection change made on its own (a Hierarchy click, a Scene view pick or box select,
    // Select All, MCP select_entities) as an undo step, Unity style. Changes of one gesture
    // (a frame, a box select drag) merge into one step. Never modifies the scene.
    class SelectionCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        SelectionState before;
        SelectionState after;
        uint64_t gesture;

    public:
        SelectionCmd(Project* project, uint32_t sceneId, SelectionState before, SelectionState after, uint64_t gesture);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;

        bool affectsStructure() const override { return false; }

        // "Select Player", "Select 3 Objects", "Deselect All"
        std::string describe() const override;
        const char* typeName() const override { return "SelectionCmd"; }
        bool isNoOp() const override { return before == after; }
        size_t memoryBytes() const override {
            return sizeof(*this) + (before.entities.capacity() + after.entities.capacity()) * sizeof(Entity);
        }
        // Play Mode Changes skip selection steps
        void collectKeepCommands(KeepContext& context) const override;
        // Off-thread release: the destructor only frees two entity vectors; no editor/engine/GPU state (S24 R8).
        bool releaseOffThread() const override { return true; }
    };

}
