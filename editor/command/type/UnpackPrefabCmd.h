// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <vector>

namespace jasperengine::editor{

    // Unity's Unpack Prefab (and Unpack Prefab Completely) over a selection: each selected
    // GameObject stands for the outermost prefab instance holding it. One undo step.
    class UnpackPrefabCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        std::vector<Entity> targets;
        bool completely;

        bool wasModified;

        std::vector<PrefabUnpackRecord> records;

    public:
        UnpackPrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> targets, bool completely);

        // Whether the command would unpack anything for these GameObjects
        static bool canUnpack(const Project* project, uint32_t sceneId, const std::vector<Entity>& targets);

        bool execute() override;
        void undo() override;

    };

}
