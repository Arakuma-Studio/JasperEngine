// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "subsystem/VoxelSystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // One undo step of a Voxel Volume's edit (docs/specs/T6-voxel-volumes.md R29): one brush stroke or
    // one MCP call. It holds the old and new block references per touched block and level (the delta
    // of VoxelSystem::edit with EditOptions::captureDelta), so undo and redo swap references: O(touched
    // blocks) and byte-exact. The blocks a step keeps out of the store count against R7's budget
    // (VoxelAssets::sharedBudget) as long as the step lives; the history's caps (S24, Editor Settings >
    // Undo) free it like any other. Selection and panel state are not in it.
    //
    // The edit has already happened when this is made: the first execute only records it.
    // displayName is the step's name, "Dig Sphere": the GameObject's name is added.
    // continuous: the ticks of one stroke, merged until the history is marked (CommandHistory::mark),
    // keeping each block's oldest state; use CommandHistory::addCommand for them.
    class VoxelEditCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        std::string displayName;
        std::vector<voxel::SlotChange> delta;
        bool continuous;

        // held, so an Add Component undone and redone finds the same content; the file reloading
        // (S05) replaces it in the system, and this step then has nothing to act on
        std::shared_ptr<VoxelAsset> volume;
        std::shared_ptr<voxel::MemoryBudget> budget;
        int64_t charged = 0;
        bool recorded = false;
        bool applied = true;
        bool wasModified = false;

        void applyDelta(bool undoing);
        // what the store does not hold now: the state a swap would bring back
        void updateCharge();

    public:
        VoxelEditCmd(Project* project, uint32_t sceneId, Entity entity, std::string displayName, std::vector<voxel::SlotChange> delta,
                     bool continuous = false);
        ~VoxelEditCmd() override;

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;

        // the Hierarchy shows no block of a volume
        bool affectsStructure() const override { return false; }
        // "Dig Sphere (Cave)"
        std::string describe() const override;
        const char* typeName() const override { return "VoxelEditCmd"; }
        bool isNoOp() const override { return delta.empty(); }
        size_t memoryBytes() const override;

        // the volume this step edits
        const std::shared_ptr<VoxelAsset>& asset() const { return volume; }
    };

}
