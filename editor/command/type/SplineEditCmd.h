// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/PrefabEditUndo.h"
#include "Project.h"
#include "component/SplineComponent.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor{

    // What a spline edit writes: the curve's definition
    struct SplineEditState{
        spline::SplineType type = spline::SplineType::CatmullRom;
        bool closed = false;
        Vector3 upAxis = Vector3(0.0f, 1.0f, 0.0f);
        std::vector<spline::SplineKnot> knots;

        bool operator==(const SplineEditState& other) const;
    };

    // The undo step's verb (S24 names, docs/specs/S18-splines.md D13)
    enum class SplineEditKind{
        Draw,           // "Draw Spline"
        MoveKnots,      // "Move Spline Knots"
        InsertKnot,     // "Insert Spline Knot"
        DeleteKnots,    // "Delete Spline Knots"
        EditTangent,    // "Edit Spline Tangent"
        Edit            // "Edit Spline": type, closed, a table edit, a stroke
    };

    // Writes a SplineComponent's definition in one undo step: every knot edit of the editor
    // (the Scene view's Edit Spline context and Draw Spline tool, the inspector's knot table,
    // edit_spline) ends here. It bumps the spline's version (the consumers regenerate), marks the
    // scene modified and records the prefab instance overrides: per knot property
    // (knots[i].position, ...) while the knot count stays, the whole "knots" array when it
    // changes. A continuous edit (a gizmo drag, a held inspector field) merges into the previous
    // continuous edit of the same spline and the same knot selection, until the history is
    // marked; the generated geometry is derived state and never in the history.
    class SplineEditCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;

        SplineEditState oldState;
        SplineEditState newState;
        SplineEditKind kind;
        bool continuous;
        // the selection a continuous edit moves; merging needs the same one
        std::vector<uint32_t> selection;
        bool wasModified;
        PrefabEditUndo prefabUndo;

        static void apply(SplineComponent& spline, const SplineEditState& state);

    public:
        SplineEditCmd(Project* project, uint32_t sceneId, Entity entity, SplineEditState state, SplineEditKind kind,
                      bool continuous = false, std::vector<uint32_t> selection = {});

        static SplineEditState capture(const SplineComponent& spline);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;

        // the Hierarchy shows nothing of a spline's knots
        bool affectsStructure() const override { return false; }
        // "Move Spline Knots (Road)"
        std::string describe() const override;
        const char* typeName() const override { return "SplineEditCmd"; }
        bool isNoOp() const override { return oldState == newState; }
        size_t memoryBytes() const override;
    };

}
