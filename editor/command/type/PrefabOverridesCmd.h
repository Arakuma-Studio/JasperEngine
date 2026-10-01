// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace jasperengine::editor{

    // Unity's Overrides dropdown, for removed GameObjects: Revert brings the GameObject back
    // into the instance with the Prefab's current values, in its Prefab place
    class RevertPrefabRemovalCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        std::filesystem::path path;   // prefab holding the GameObject
        Entity instanceRoot;          // instance that removed it
        Entity registryEntity;
        PrefabRemovalRevert revert;
        bool wasModified = false;

    public:
        RevertPrefabRemovalCmd(Project* project, uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity);

        bool execute() override;
        void undo() override;
    };

    // Apply to Prefab of a removed GameObject: it is deleted from the Prefab asset, so from
    // every instance of it
    class ApplyPrefabRemovalCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        std::filesystem::path path;
        Entity instanceRoot;
        Entity registryEntity;
        PrefabRemovalApply apply;
        bool wasModified = false;

    public:
        ApplyPrefabRemovalCmd(Project* project, uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity);

        bool execute() override;
        void undo() override;
    };

    // Apply to Prefab of an overridden component: the instance's values become the Prefab's,
    // and the component follows the Prefab again
    class ApplyPrefabComponentCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        ComponentType componentType;
        ComponentRecovery recovery;
        bool wasModified = false;

    public:
        ApplyPrefabComponentCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType componentType);

        bool execute() override;
        void undo() override;
    };

    // Revert All / Apply All: the steps run in order as one undo step, undone in reverse.
    // A step with nothing left to do (an earlier one covered it) is skipped.
    class PrefabOverridesCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        std::vector<std::unique_ptr<Command>> steps;
        std::vector<bool> stepDone;
        bool executedOnce = false;
        bool wasModified = false;

    public:
        PrefabOverridesCmd(Project* project, uint32_t sceneId, std::vector<std::unique_ptr<Command>> steps);

        bool execute() override;
        void undo() override;
    };

}
