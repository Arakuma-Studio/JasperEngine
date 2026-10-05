// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace jasperengine::editor{

    struct DeleteEntityData{
        Entity entity = NULL_ENTITY;

        size_t entityIndex = 0;
        bool hasTransform = false;
        Entity parent = NULL_ENTITY;

        // The tracked GameObjects with a Transform, in Transform order, as they were just before
        // this delete ran (shared by the GameObjects deleted together), and this one's place
        // there. Undo puts it back after the nearest one before it that exists again: a raw
        // Transform index is no position to go back to, as the array also holds the editor's
        // gizmo entities (a camera's or a light's icon and lines), made and dropped per frame
        std::shared_ptr<const std::vector<Entity>> transformOrder;
        size_t transformOrderPos = 0;

        YAML::Node data;

        // Track if entity was part of a prefab
        NodeRecovery recoveryPrefabData;
        bool isPrefabRoot = false;
        std::filesystem::path prefabPath;
        // A Prefab GameObject removed from its instance only (removed GameObject override)
        PrefabBranchRecovery removedFromInstance;
    };

    // What deleting a GameObject of a prefab instance that comes from the Prefab does
    enum class PrefabMemberDelete {
        RemoveFromInstance,  // Unity: removed from this instance only, a removed GameObject override
        DeleteFromPrefab     // a Prefab edit: gone from the Prefab asset and all its instances
    };

    class DeleteEntityCmd: public Command{

    friend class Project;
    friend class CreateEntityCmd;
    friend class DuplicateEntityCmd;
    friend class ModelClearCmd;
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
        PrefabMemberDelete prefabMemberDelete = PrefabMemberDelete::RemoveFromInstance;

        std::vector<Entity> lastSelected;
        std::vector<Entity> survivingEntities;
        std::vector<Entity> trackedBeforeDelete; // the scene's tracked list, restored by undo

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
        void recordTransformOrder();

        DeleteEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
            bool allowLockedRoots, const std::vector<Entity>& preservedEntities);

        static void destroyEntity(EntityRegistry* registry, Entity entity, std::vector<Entity>& entities, Project* project = nullptr, uint32_t sceneId = NULL_PROJECT_SCENE);
        // reconcile false leaves the tracked list to the caller
        static void destroyEntities(EntityRegistry* registry, const std::vector<Entity>& entitiesToDestroy,
            std::vector<Entity>& entities, Project* project = nullptr, uint32_t sceneId = NULL_PROJECT_SCENE, bool reconcile = true);

    public:
        DeleteEntityCmd(Project* project, uint32_t sceneId, Entity entity, bool allowLockedRoots = false);
        DeleteEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, bool allowLockedRoots = false);
        ~DeleteEntityCmd() override;

        // Encodes of GameObjects about to be deleted, made with Stream::encodeEntity and the
        // project context just before this command runs: its first execute takes these
        // as its snapshots instead of encoding the same GameObjects again
        void setPreEncoded(std::unordered_map<Entity, YAML::Node> snapshots) { preEncoded = std::move(snapshots); }

        // Set before the first execute; a user's delete removes from the instance only
        void setPrefabMemberDelete(PrefabMemberDelete mode) { prefabMemberDelete = mode; }

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}
