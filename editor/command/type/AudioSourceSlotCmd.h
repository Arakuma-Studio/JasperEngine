// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine::editor {

    class CreateEntityCmd;
    class DeleteEntityCmd;

    // A further Audio Source of a GameObject (docs/specs/S97-audio-gaps.md REQ-24, D6): a hidden
    // child GameObject with a Transform at the local origin and an Audio Source whose slot is the
    // next free number. Add Component > Audio Source on a GameObject that has one adds it.
    class AddAudioSourceSlotCmd : public Command {
    private:
        Project* project;
        uint32_t sceneId;
        Entity owner;
        int slot = 0;
        std::string ownerName;
        std::unique_ptr<CreateEntityCmd> create;

    public:
        AddAudioSourceSlotCmd(Project* project, uint32_t sceneId, Entity owner);
        ~AddAudioSourceSlotCmd() override;

        // Whether Add Component > Audio Source fits `entity`: its own source when it has none, else
        // a slot, which needs a Transform to hang from (a slot itself takes none). "" when it fits
        static std::string refusal(Scene* scene, Entity entity);

        bool execute() override;
        void undo() override;

        // "Add Component Audio Source (Car)", as its first Audio Source
        std::string describe() const override;
        const char* typeName() const override { return "AddAudioSourceSlotCmd"; }

        uint32_t getSceneId() const { return sceneId; }
        Entity getOwner() const { return owner; }
        // the slot's GameObject, once executed
        Entity getSlotEntity() const;
        int getSlot() const { return slot; }
    };

    // Remove Component on a slot: its GameObject goes, the undo brings it back where it was
    class RemoveAudioSourceSlotCmd : public Command {
    private:
        std::string ownerName;
        std::unique_ptr<DeleteEntityCmd> remove;

    public:
        RemoveAudioSourceSlotCmd(Project* project, uint32_t sceneId, Entity slot);
        ~RemoveAudioSourceSlotCmd() override;

        bool execute() override;
        void undo() override;

        std::string describe() const override;
        const char* typeName() const override { return "RemoveAudioSourceSlotCmd"; }
    };

    // Move Up / Move Down of a slot among its GameObject's slots (direction -1 or +1): the two
    // swap their numbers in one step. Null when there is no slot that way
    Command* buildMoveAudioSourceSlot(Project* project, uint32_t sceneId, Entity slot, int direction);

}
