// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/PrefabEditUndo.h"
#include "Project.h"
#include "component/LatticeComponent.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor{

    // What a lattice edit writes: its resolution, offsets and poses
    struct LatticePointsState{
        int resolutionX = 3;
        int resolutionY = 3;
        int resolutionZ = 3;
        std::vector<Vector3> offsets;
        std::vector<LatticePose> poses;
    };

    // Writes a LatticeComponent's offsets (and with a whole state its resolution and poses)
    // in one undo step: every lattice edit of the editor (LatticeEditorOps) ends here. It
    // bumps the component's revision, marks the scene modified and records the prefab
    // instance overrides: per point (offsets[i]) when only offsets change, else what the
    // edit changed of the component. A continuous edit (a Scene-view drag, an inspector
    // widget held) merges into the previous continuous edit of the same lattice until the
    // history is marked (LatticeEditorOps::endContinuousEdit).
    class LatticePointsCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;

        LatticePointsState oldState;
        LatticePointsState newState;
        bool wholeState;  // resolution and poses are part of the edit
        bool continuous;
        bool wasModified;
        // the first execute ran (offsets left as a clip preview showed them were put back to rest)
        bool executed = false;
        // the points the edit sets by index (setExplicitPoints)
        std::vector<int> explicitPoints;
        PrefabEditUndo prefabUndo;

        static LatticePointsState capture(const LatticeComponent& lattice);
        static void apply(LatticeComponent& lattice, const LatticePointsState& state, bool wholeState);

    public:
        // offsets only (the size must match the lattice's resolution when it runs)
        LatticePointsCmd(Project* project, uint32_t sceneId, Entity lattice, const std::vector<Vector3>& offsets, bool continuous = false);
        // resolution, offsets and poses
        LatticePointsCmd(Project* project, uint32_t sceneId, Entity lattice, const LatticePointsState& state);

        // the points the edit sets on purpose (LatticeEditorOps::setOffsets: an Inspector Offset
        // row, MCP edit_lattice): the new rest of a clip preview even when equal to what it shows;
        // the other points follow "left as shown"
        void setExplicitPoints(std::vector<int> points){ explicitPoints = std::move(points); }

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;

        // the Hierarchy shows nothing of a lattice's points
        bool affectsStructure() const override { return false; }

        // "Move Lattice Points (Cage)" for offsets, "Edit Lattice (Cage)" for a whole state
        std::string describe() const override;
        const char* typeName() const override { return "LatticePointsCmd"; }
        // the lattice is as it was (a drag back where it started)
        bool isNoOp() const override;
        size_t memoryBytes() const override;
    };

}
