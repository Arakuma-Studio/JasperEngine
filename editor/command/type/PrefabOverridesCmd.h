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
        bool mergeWith(Command* otherCommand) override;
    };

    // Apply to Prefab of a removed GameObject: it is deleted from the Prefab asset, so from
    // every instance of it. With a target holding the instance (a variant of the prefab, a
    // prefab it is nested in): Apply as Override there, the GameObject left out by its data.
    class ApplyPrefabRemovalCmd: public Command{
    private:
        Project* project;
        uint32_t sceneId;
        std::filesystem::path path;
        Entity instanceRoot;
        Entity registryEntity;
        std::filesystem::path target;
        PrefabRemovalApply apply;
        PrefabUndoState before;
        bool wasModified = false;

        bool toOuter() const { return !target.empty() && target != path; }

    public:
        ApplyPrefabRemovalCmd(Project* project, uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity,
            const std::filesystem::path& target = {});

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;
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

        // The Overrides dropdown's Revert All: every override of the instance rooted at root
        // (and of the ones nested in it) reverted
        static PrefabOverridesCmd* revertAll(Project* project, uint32_t sceneId, Entity root);
        // Apply All: every override applied to the instance's prefab (the outermost; overrides
        // of nested instances become its overrides of them), but values referencing
        // GameObjects outside it; null in Play mode
        static PrefabOverridesCmd* applyAll(Project* project, uint32_t sceneId, Entity root);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;
    };

}
