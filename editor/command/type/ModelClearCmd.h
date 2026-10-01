// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/type/DeleteEntityCmd.h"
#include "Project.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "yaml-cpp/yaml.h"

#include <string>
#include <vector>

namespace jasperengine::editor{

    // Empties a Model component: the model file is unloaded and everything its load made goes
    // (the node, part and bone entities, the animations and their tracks, the geometry, materials
    // and textures it put in the GameObject's mesh, the parsed file), leaving a Model with no file.
    // The GameObject, its own components and the entities the user hung on model nodes stay, the
    // latter moved to the GameObject. Undo brings the model back as it was: the same entity ids,
    // the per-node edits, the user's entities back on their nodes.
    class ModelClearCmd: public Command{

    private:
        struct ParkedEntity {
            Entity entity = NULL_ENTITY;
            Entity parent = NULL_ENTITY;
            size_t transformIndex = 0;
            Vector3 position;
            Quaternion rotation;
            Vector3 scale;
        };

        Project* project;
        uint32_t sceneId;
        Entity entity;

        YAML::Node oldModel;
        YAML::Node oldMesh;
        bool hadMesh = false;
        std::vector<ParkedEntity> parkedEntities;
        DeleteEntityCmd* deleteCmd = nullptr;

        bool executed = false;
        bool wasModified = false;

        // Entities the model owns: nodes, parts, bones, animations and their tracks
        static std::vector<Entity> collectOwnedEntities(Scene* scene, const ModelComponent& model);
        // Entities other than the model's own that sit on its nodes, which the delete would take along
        static std::vector<ParkedEntity> collectParkedEntities(Scene* scene, const std::vector<Entity>& owned, Entity entity);

        void unparkEntities(SceneProject* sceneProject);
        void clearComponents(Scene* scene);
        void releasePendingLoad(Scene* scene, const ModelComponent& model);

    public:
        ModelClearCmd(Project* project, uint32_t sceneId, Entity entity);
        ~ModelClearCmd() override;

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;

        // A Model whose file is loaded, or still holds what a load made
        static bool hasContent(const ModelComponent& model);
        // Whether the model of `entity` can be emptied now, with the reason when not
        static bool canClear(Project* project, uint32_t sceneId, Entity entity, std::string* reason = nullptr);
    };

}
