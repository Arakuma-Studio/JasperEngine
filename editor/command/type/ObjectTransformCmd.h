// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
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

        // The old values were given (addEntity with old values) instead of read from the live
        // Transform at execute: a tool that previewed the move in the Transform commits without
        // putting the old pose back first
        bool explicitOld = false;
    };

    class ObjectTransformCmd: public Command{

    private:
        Project* project;
        size_t sceneId;

        std::map<Entity, TransformCmdValue> props;
        PrefabEditUndo prefabUndo;

        bool wasModified;

    public:
        ObjectTransformCmd(Project* project, size_t sceneId, Entity entity, Matrix4 localMatrix);
        ObjectTransformCmd(Project* project, size_t sceneId, Entity entity, Vector3 position, Quaternion rotation, Vector3 scale);
        // No entity yet: addEntity() fills it, so one command (one undo step) moves many
        ObjectTransformCmd(Project* project, size_t sceneId);

        // Adds (or replaces) an entity's new local pose; the old one is read at execute
        void addEntity(Entity entity, Vector3 position, Quaternion rotation, Vector3 scale);
        // The same with the old local pose given (what undo puts back)
        void addEntity(Entity entity, Vector3 position, Quaternion rotation, Vector3 scale,
                       Vector3 oldPosition, Quaternion oldRotation, Vector3 oldScale);
        size_t getEntityCount() const { return props.size(); }

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // Only position/rotation/scale, and a gizmo drag issues one per frame.
        bool affectsStructure() const override { return false; }
    };

}