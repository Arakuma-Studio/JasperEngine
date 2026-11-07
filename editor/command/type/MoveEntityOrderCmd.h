// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "animation/PlayerModelLayout.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class MoveEntityOrderCmd: public Command{

    friend class Project;
    friend class DeleteEntityCmd;

    private:
        Project* project;
        uint32_t sceneId;

        Entity source;
        Entity target;
        InsertionType type;

        size_t oldIndex;
        bool hasTransform;
        Entity oldParent;

        bool wasModified;
        // The tracked list before the move: its undo puts the GameObjects without a Transform
        // back where they were (the sort keeps them with the GameObject they followed, which the
        // move can take past others)
        std::vector<Entity> trackedBefore;

        // A model moved on or under an Animation Player reloads in the node hierarchy (animation
        // clips spec 9.R3), in this same step
        PlayerModelLayout::Reloads layoutReloads;
        bool executed = false;

        // before `source` moves from under `fromParent` to under `toParent` keeping its world:
        // the clip previews and the pose of the branch or of either place restore first, and the
        // world matrices the move reads are recomputed from that rest
        void prepareReparent(Scene* scene, Entity fromParent, Entity toParent);

    public:
        MoveEntityOrderCmd(Project* project, uint32_t sceneId, Entity source, Entity target, InsertionType type);

        // "" unless a GameObject with a Transform would go before or after one without (refused:
        // only the list would change), else why
        static std::string transformAnchorProblem(EntityRegistry* registry, Entity source, Entity target, InsertionType type);
        // Unity: the GameObjects of a prefab instance (its members, nested instance roots
        // included) cannot be reordered, reparented or moved out of it; the prefab asset is
        // restructured in Prefab Mode. Added GameObjects and instance roots move freely (an
        // added GameObject override), and so do a Prefab Mode stage's own GameObjects (asset
        // edits). Returns why `source` cannot move (empty when it can), and the prefab and
        // instance root to open to restructure it.
        static std::string getPrefabRestructureBlock(Project* project, uint32_t sceneId, Entity source,
            std::filesystem::path* prefabPath = nullptr, Entity* instanceRoot = nullptr);
        static constexpr const char* PREFAB_RESTRUCTURE_MESSAGE =
            "Children of a Prefab instance cannot be rearranged. Open the Prefab to restructure it.";

        bool execute() override;
        void undo() override;

        // "Parent Player" when it goes into another GameObject, "Reorder Player" otherwise
        std::string describe() const override {
            return std::string(type == InsertionType::INTO ? "Parent " : "Reorder ") + CommandNames::entity(project, sceneId, source);
        }
        const char* typeName() const override { return "MoveEntityOrderCmd"; }

    };

}