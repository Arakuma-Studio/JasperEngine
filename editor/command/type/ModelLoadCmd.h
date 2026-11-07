// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "util/YamlFootprint.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "math/Matrix4.h"
#include "subsystem/MeshSystem.h"
#include "yaml-cpp/yaml.h"

#include <atomic>
#include <map>
#include <memory>
#include <utility>

namespace jasperengine::editor{

    class ModelLoadCmd: public Command{

    private:
        YAML::Node oldTransform;
        YAML::Node oldMesh;
        YAML::Node oldModel;
        DeleteEntityCmd* oldSubEntitiesDeleteCmd = nullptr;
        CreateEntityCmd* createEntityCmd = nullptr;

        Project* project;
        uint32_t sceneId;
        Entity entity;

        std::string modelPath;
        bool hasMergeStaticMeshesOverride = false;
        bool mergeStaticMeshesOverride = false;

        bool wasModified;
        PrefabEditUndo prefabUndo;
        bool isNewModel = false;
        // the file's clip count for Animation Entities: Auto (S74 R38), read on the first load
        int autoClipCount = -1;
        // A reload that switches the model to the node-hierarchy layout (animation clips spec 9.R3),
        // decided on the first run (the old component a redo starts from no longer says how it was
        // loaded), and the glTF scene root's matrix the legacy model entity stood in for
        bool switchesLayout = false;
        bool movesLegacyRoot = false;
        Matrix4 legacyRoot;
        bool reuseHierarchy = false;
        bool asyncPending = false;
        bool hasDropSurface = false;
        Vector3 dropNormal = Vector3(0, 1, 0);
        bool dropAlign = false;
        Vector3 dropPoint;
        std::shared_ptr<std::atomic<bool>> cancelFlag;

        // Mesh children the reload keeps, so undo only removes the ones it created
        std::vector<Entity> reusedEntities;

        // A node hierarchy is rebuilt rather than refreshed, so a same-file reload records what the
        // user arranged and puts it back by node name once the new nodes exist.
        struct NodeRef {
            int index = -1;
            std::string name;
            bool bone = false; // a joint, which the flat skin path keeps apart from a mesh at the same index
        };
        struct LocalPose {
            Vector3 position;
            Quaternion rotation;
            Vector3 scale;
        };
        // An entity the user hung on a model node; parked at the model while the node is rebuilt
        struct ParkedEntity {
            Entity entity = NULL_ENTITY;
            Entity oldParent = NULL_ENTITY;
            NodeRef parent;
            LocalPose pose;
        };
        // An imported part the user moved away from where the file puts it
        struct MovedPart {
            NodeRef part;
            Entity userParent = NULL_ENTITY; // a local group or the model itself
            NodeRef nodeParent;              // when it was dropped on another model node
            LocalPose pose;
        };
        std::vector<ParkedEntity> parkedEntities;
        std::vector<MovedPart> movedParts;
        // An action of the user's (a track of an entity Animation, a tween) whose target is a model
        // node: kept out of the rebuild's delete (an action is a virtual child of its target) and
        // pointed at the rebuilt node of the same glTF node; undo gives it its old target back
        struct RetargetedAction {
            Entity action = NULL_ENTITY;
            Entity oldTarget = NULL_ENTITY;
            NodeRef node;
        };
        std::vector<RetargetedAction> retargetedActions;

        // The generated mesh entities are destroyed before the load, so their submesh edits are
        // taken aside here. Empty when the asset itself changed.
        MeshSystem::SubmeshOverrides savedSubmeshOverrides;
        // The blend shape weights of the mesh parts by glTF node, carried across a layout switch
        // (R3): its rebuild makes new mesh entities (a legacy model entity's own mesh moves to its
        // node's entity). Taken on the first run, put back after every load of it.
        std::map<int, std::vector<float>> savedMorphWeights;
        void recordMorphWeights(Scene* scene, const ModelComponent& model);
        void restoreMorphWeights(Scene* scene, const ModelComponent& model);

        static std::vector<Entity> collectModelDeleteRoots(Scene* scene, const ModelComponent& model);
        static bool isMappedMeshNode(const ModelComponent& model, Entity entity);
        static NodeRef makeNodeRef(const ModelComponent& model, int nodeIndex, bool bone);
        static Entity findModelNode(const ModelComponent& model, const NodeRef& ref);
        static void attachLocal(Scene* scene, Entity child, Entity parent, const LocalPose& pose);

        void recordArrangement(SceneProject* sceneProject, const ModelComponent& model);
        void restoreArrangement(SceneProject* sceneProject, const ModelComponent& model);
        void recordRetargets(Scene* scene, const ModelComponent& model);
        // the actions point at nothing while the old nodes go and the new ones come
        void detachRetargets(Scene* scene);
        void applyRetargets(Scene* scene, const ModelComponent& model);
        void restoreRetargets(Scene* scene);
        void parkEntities(SceneProject* sceneProject);
        void unparkEntities(SceneProject* sceneProject);

        bool tryLoad();
        void finalizeLoad();
        void schedulePoll();
        void restoreTransform(Scene* scene);

        // Loads still running on a worker, by scene and Model entity
        static std::map<std::pair<uint32_t, Entity>, ModelLoadCmd*>& getPendingLoads();
        void setAsyncPending(bool pending);
        void finishPendingLoad();

    public:
        ModelLoadCmd(Project* project, uint32_t sceneId, Entity entity, const std::string& modelPath);
        ModelLoadCmd(Project* project, uint32_t sceneId, Entity entity, const std::string& modelPath, bool mergeStaticMeshes);
        ModelLoadCmd(Project* project, uint32_t sceneId, const std::string& entityName, const Vector3& position, const std::string& modelPath);
        // a Scene view drop: once loaded, the model's bottom rests on the surface at its position
        // (normal), its up axis turned onto the normal when align (S26 R16)
        void setDropSurface(const Vector3& normal, bool align);
        // Terrain object placement: created quiet, under `parent`, with a local transform
        ModelLoadCmd(Project* project, uint32_t sceneId, const std::string& entityName, Entity parent, const Vector3& position, const Quaternion& rotation, const Vector3& scale, const std::string& modelPath);
        ~ModelLoadCmd() override;

        bool execute() override;
        void undo() override;

        // "Load Model robot.glb (Robot)"; a GameObject made for it: "Create Model robot.glb"
        std::string describe() const override;
        const char* typeName() const override { return "ModelLoadCmd"; }
        size_t memoryBytes() const override;


        // Whether a load into this Model is still running on a worker
        static bool isLoadPending(uint32_t sceneId, Entity entity);
        // Waits for a load into this Model still running on a worker and finishes it now
        static void completePendingLoad(uint32_t sceneId, Entity entity);
    };

}
