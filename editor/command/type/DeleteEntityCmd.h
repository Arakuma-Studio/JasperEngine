// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor{

    struct DeleteEntityData{
        Entity entity = NULL_ENTITY;

        size_t entityIndex = 0;
        bool hasTransform = false;
        Entity parent = NULL_ENTITY;

        YAML::Node data;

        // Track if entity was part of a prefab
        NodeRecovery recoveryPrefabData;
        bool isPrefabRoot = false;
        std::filesystem::path prefabPath;
    };

    class DeleteEntityCmd: public Command{

    friend class Project;
    friend class CreateEntityCmd;
    friend class DuplicateEntityCmd;
    friend class PasteEntitiesCmd;
    friend class RemoveComponentCmd;

    private:
        struct PrefabInstanceSnapshot {
            std::filesystem::path path;
            Entity rootEntity = NULL_ENTITY;
            std::vector<Prefab::EntityMember> members;
        };

        Project* project;
        uint32_t sceneId;

        std::vector<Entity> requestedEntities;
        std::vector<Entity> preservedEntities;
        bool allowLockedRoots;

        std::vector<Entity> lastSelected;
        std::vector<Entity> survivingEntities;

        std::vector<DeleteEntityData> entities;
        std::vector<PrefabInstanceSnapshot> prefabInstanceSnapshots;

        bool wasModified;

        // Snapshots encoded just before by the caller (Cut), used by the first execute
        std::unordered_map<Entity, YAML::Node> preEncoded;
        // The snapshots are such encodes: nodes of a copy the clipboard may still be writing out
        bool snapshotsShared = false;

        bool appendEntityData(Entity entity);
        std::vector<size_t> prepareDeleteSet();
        void snapshotEntities();

        DeleteEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
            bool allowLockedRoots, const std::vector<Entity>& preservedEntities);

        static void destroyEntity(EntityRegistry* registry, Entity entity, std::vector<Entity>& entities, Project* project = nullptr, uint32_t sceneId = NULL_PROJECT_SCENE);
        static void destroyEntities(EntityRegistry* registry, const std::vector<Entity>& entitiesToDestroy,
            std::vector<Entity>& entities, Project* project = nullptr, uint32_t sceneId = NULL_PROJECT_SCENE);

    public:
        DeleteEntityCmd(Project* project, uint32_t sceneId, Entity entity, bool allowLockedRoots = false);
        DeleteEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, bool allowLockedRoots = false);
        ~DeleteEntityCmd() override;

        // Encodes of GameObjects about to be deleted, made with Stream::encodeEntity and the
        // project context just before this command runs: its first execute takes these
        // as its snapshots instead of encoding the same GameObjects again
        void setPreEncoded(std::unordered_map<Entity, YAML::Node> snapshots) { preEncoded = std::move(snapshots); }

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}
