// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "asset/AssetRef.h"
#include "component/AnimationComponent.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Add a frame to an animation in one undo step, creating its action entity first
    // unless the type is EMPTY. The frame goes on the first free track from `track`.
    class AddActionFrameCmd : public Command {
    private:
        Project* project;
        uint32_t sceneId;
        Entity animEntity;
        uint32_t track;

        std::unique_ptr<CreateEntityCmd> createCmd;
        std::unique_ptr<PropertyCmd<std::vector<ActionFrame>>> actionsCmd;

        // the frame's start; an audio action's sound or a Mixer Snapshot action's snapshot (S40 R22)
        float startTime = 0.0f;
        AssetRef sound;
        bool soundIsEvent = false;
        std::string snapshot;
        std::string undoLabel;

    public:
        AddActionFrameCmd(Project* project, uint32_t sceneId, Entity animEntity, Entity target,
                          EntityCreationType creationType, std::string name, uint32_t track);

        // where the frame starts (default 0)
        void setStartTime(float time) { startTime = time; }
        // what the created audio action plays: a clip, or a Sound Event (S40 R22)
        void setSound(const AssetRef& ref, bool event) { sound = ref; soundIsEvent = event; }
        // the created Mixer Snapshot action's snapshot
        void setSnapshot(const std::string& name) { snapshot = name; }
        // the undo step's name ("Add Audio Action"); default "Add Action"
        void setUndoLabel(const std::string& label) { undoLabel = label; }

        bool execute() override;
        void undo() override;

        // "Add Action (Hero Walk)", the Animation's GameObject
        std::string describe() const override;
        const char* typeName() const override { return "AddActionFrameCmd"; }
        size_t memoryBytes() const override;
    };

}
