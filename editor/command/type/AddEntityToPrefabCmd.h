// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor{

    class AddEntityToPrefabCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        Entity parent;
        std::filesystem::path target;   // empty: the prefab whose GameObject the parent is

        bool wasModified;

        Entity originalParent;
        size_t originalTransformIndex;
        bool hasTransform;
        // The prefab instances in the branch as they were: their scene data goes into the
        // prefab, an undo gives it back
        std::vector<std::pair<Entity, YAML::Node>> nestedRoots;

    public:
        // target: a prefab holding the parent's instance (Apply to an outer prefab), empty
        // for the prefab whose GameObject the parent is
        AddEntityToPrefabCmd(Project* project, uint32_t sceneId, Entity entity, Entity parent, const std::filesystem::path& target = {});

        bool execute() override;
        void undo() override;

    };

}
