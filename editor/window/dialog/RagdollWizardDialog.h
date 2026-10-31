// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Ragdoll Wizard ("3D Object > Ragdoll...", the Inspector's "Create Ragdoll..."): the bone
// slots of a character (filled from its humanoid mapping or bone names), total mass, flip forward,
// start state and self collision; Create makes every part in one undo step (util/RagdollWizard).

#pragma once

#include "physics/RagdollBuilder.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class RagdollWizardDialog {
    private:
        bool isOpen = false;
        bool justOpened = false;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity root = NULL_ENTITY;
        RagdollBones bones;
        RagdollBuildSettings settings;
        std::vector<std::string> notes;
        std::string error;
        // the GameObjects below root a slot can take, Hierarchy order
        std::vector<Entity> candidates;

    public:
        static RagdollWizardDialog& instance();

        void open(Project* project, uint32_t sceneId, Entity root);
        void show();
        bool opened() const { return isOpen; }
    };

}
