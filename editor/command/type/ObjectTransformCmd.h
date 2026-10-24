// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/EntityEdit.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include <cstdint>
#include <string>

namespace jasperengine::editor{

    struct TransformCmdValue{
        Vector3 newPosition;
        Quaternion newRotation;
        Vector3 newScale;

        Vector3 oldPosition;
        Quaternion oldRotation;
        Vector3 oldScale;

        bool applied = false;   // the entity had a Transform when it ran
    };

    class ObjectTransformCmd: public Command, public EntityEdit{

    private:
        Project* project;
        size_t sceneId;

        std::map<Entity, TransformCmdValue> props;
        PrefabEditUndo prefabUndo;

        bool wasModified;

    public:
        ObjectTransformCmd(Project* project, size_t sceneId, Entity entity, Matrix4 localMatrix);
        ObjectTransformCmd(Project* project, size_t sceneId, Entity entity, Vector3 position, Quaternion rotation, Vector3 scale);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // Only position/rotation/scale, and a gizmo drag issues one per frame.
        bool affectsStructure() const override { return false; }

        // "Move Player", "Rotate 3 Objects", "Scale Cube"; "Set Transform" when several change
        std::string describe() const override;
        const char* typeName() const override { return "ObjectTransformCmd"; }
        bool isNoOp() const override;
        // A fresh ObjectTransformCmd per GameObject, with its position, rotation and scale now
        void collectKeepCommands(KeepContext& context) const override;

        Project* editedProject() const override { return project; }
        uint32_t editedSceneId() const override { return (uint32_t)sceneId; }
        std::vector<Entity> editedEntities() const override;
        std::string editedLabel() const override { return "Transform"; }
    };

}