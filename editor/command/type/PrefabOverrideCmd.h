// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // One operation of the prefab override model (Project's record / revert / apply
    // primitives, editor/project/PREFABS.md) as an undo step. Undo puts back the instance,
    // and for an apply the prefab and every instance using it, exactly as they were
    // (Project::capturePrefabUndoState): same entity ids, so the rest of the history holds.
    class PrefabOverrideCmd: public Command{
    public:
        enum class Operation{
            RecordProperty, RevertProperty, ApplyProperty,
            RecordState, RevertState, ApplyState,
            RecordAddedComponent, RevertAddedComponent, ApplyAddedComponent,
            RecordRemovedComponent, RevertRemovedComponent, ApplyRemovedComponent
        };

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        Operation operation;
        ComponentType componentType;
        std::vector<std::string> properties;  // empty: the whole component
        uint8_t stateBits;                    // PrefabObjectOverrides::State bits
        std::filesystem::path target;         // apply: the prefab (empty: the owning one)
        std::vector<std::filesystem::path> writtenTargets;   // the prefabs the last execute wrote
        PrefabUndoState before;

        bool isApply() const;

    public:
        PrefabOverrideCmd(Project* project, uint32_t sceneId, Entity entity, Operation operation,
            ComponentType componentType = ComponentType::Transform, const std::vector<std::string>& properties = {},
            uint8_t stateBits = 0, const std::filesystem::path& target = {});

        bool execute() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        void undo() override;
    };

}
