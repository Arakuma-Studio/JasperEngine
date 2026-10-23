// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/type/CreateEntityCmd.h"
#include "command/PrefabEditUndo.h"
#include "component/AnimationComponent.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Create a keyframe-track action, seed its first key, and register it on the
    // animation — all in one undo step. execute() runs create first so the new
    // entity id is known before the key and ActionFrame writes.
    class CreateTrackKeyCmd : public Command {
    private:
        Project* project;
        uint32_t sceneId;
        Entity animEntity;

        std::unique_ptr<CreateEntityCmd> createCmd;
        Entity trackEntity = NULL_ENTITY;
        std::vector<ActionFrame> oldActions;
        bool wasModified = false;
        PrefabEditUndo prefabUndo;

    public:
        CreateTrackKeyCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                          EntityCreationType creationType, std::string name, float localTime,
                          Vector3 value);
        CreateTrackKeyCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                          EntityCreationType creationType, std::string name, float localTime,
                          Quaternion value);

        bool execute() override;
        void undo() override;

        Entity getEntity() const { return trackEntity; }

        // "Add Key (Walk)": the Animation the track joins
        std::string describe() const override { return "Add Key (" + CommandNames::entity(project, sceneId, animEntity) + ")"; }
        const char* typeName() const override { return "CreateTrackKeyCmd"; }
        size_t memoryBytes() const override {
            return sizeof(*this) + oldActions.capacity() * sizeof(ActionFrame) + (createCmd ? createCmd->memoryBytes() : 0);
        }
    };

}
