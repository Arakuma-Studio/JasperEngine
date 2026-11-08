// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelClearCmd.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"

#include "Stream.h"
#include "command/type/ModelLoadCmd.h"
#include "pose/PoseEditSession.h"
#include "util/ProjectUtils.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

namespace {

    // Textures a mesh holds keep their pooled data and render until destroyed. The render
    // system destroys them with a loaded mesh only, so this also covers one never drawn.
    void releaseMeshResources(Scene* scene, Entity entity, MeshComponent& mesh) {
        scene->getSystem<RenderSystem>()->prepareMeshForDataReload(entity, mesh);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            Material& material = mesh.submeshes[s].material;
            material.baseColorTexture.destroy();
            material.metallicRoughnessTexture.destroy();
            material.normalTexture.destroy();
            material.occlusionTexture.destroy();
            material.emissiveTexture.destroy();
            for (auto& shaderTexture : material.shaderTextures)
                shaderTexture.second.destroy();
            if (MaterialPass* pass = material.extraPass.editIfSet())
                for (auto& shaderTexture : pass->shaderTextures)
                    shaderTexture.second.destroy();
        }
    }

    // The Mesh Renderer of a GameObject whose model was unloaded: no geometry, submeshes or
    // bones, and the renderer settings the user chose.
    MeshComponent emptyModelMesh(const MeshComponent& mesh) {
        MeshComponent empty{};
        empty.enabled = mesh.enabled;
        empty.customShader = mesh.customShader;
        empty.customDepthShader = mesh.customDepthShader;
        empty.shaderUniforms = mesh.shaderUniforms;
        empty.needUpdateShaderUniforms = true;
        empty.receiveLights = mesh.receiveLights;
        empty.receiveIBL = mesh.receiveIBL;
        empty.castShadows = mesh.castShadows;
        empty.receiveShadows = mesh.receiveShadows;
        empty.shadowsBillboard = mesh.shadowsBillboard;
        empty.renderInReflectionProbes = mesh.renderInReflectionProbes;
        empty.transparent = mesh.transparent;
        empty.autoTransparency = mesh.autoTransparency;
        empty.lodEnabled = mesh.lodEnabled;
        empty.lodBias = mesh.lodBias;
        empty.allowGpuInstancing = mesh.allowGpuInstancing;
        empty.cullingMode = mesh.cullingMode;
        empty.windingOrder = mesh.windingOrder;
        empty.mirroredWinding = mesh.mirroredWinding;
        return empty;
    }

}

editor::ModelClearCmd::ModelClearCmd(Project* project, uint32_t sceneId, Entity entity){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
}

editor::ModelClearCmd::~ModelClearCmd(){
    delete deleteCmd;
}

bool editor::ModelClearCmd::hasContent(const ModelComponent& model){
    // The skeleton and animation fields alone are references the user can set on an empty Model
    return !model.filename.empty() || !model.loadedFilename.empty() || model.gltfModel || model.objModel ||
        !model.nodesIdMapping.empty() || !model.meshNodesMapping.empty() ||
        !model.bonesIdMapping.empty() || !model.bonesNameMapping.empty() ||
        !model.skinBindings.empty() || !model.morphNameMapping.empty();
}

bool editor::ModelClearCmd::canClear(Project* project, uint32_t sceneId, Entity entity, std::string* reason){
    auto fail = [reason](const std::string& message) {
        if (reason) *reason = message;
        return false;
    };
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)) {
        return fail("No GameObject selected");
    }
    Scene* scene = sceneProject->scene;
    const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
    if (!model) {
        return fail("The GameObject has no Model");
    }
    if (!hasContent(*model)) {
        return fail("The Model has no file");
    }
    if (ModelLoadCmd::isLoadPending(sceneId, entity)) {
        return fail("The model is still loading");
    }
    return true;
}

std::vector<Entity> editor::ModelClearCmd::collectOwnedEntities(Scene* scene, Entity entity, const ModelComponent& model){
    std::vector<Entity> owned;
    std::unordered_set<Entity> seen;
    auto add = [&](Entity e) {
        if (e != NULL_ENTITY && e != entity && scene->isEntityCreated(e) && seen.insert(e).second) {
            owned.push_back(e);
        }
    };

    for (const auto& node : model.nodesIdMapping) add(node.second);
    for (const auto& node : model.meshNodesMapping) add(node.second);
    for (const auto& bone : model.bonesIdMapping) add(bone.second);
    for (const auto& bone : model.bonesNameMapping) add(bone.second);
    // The skeleton field can point anywhere; it is the model's only when it is one of its bones
    if (model.skeleton != NULL_ENTITY && scene->isEntityCreated(model.skeleton)) {
        const BoneComponent* bone = scene->findComponent<BoneComponent>(model.skeleton);
        if (seen.count(model.skeleton) || (bone && bone->model == entity)) {
            add(model.skeleton);
        }
    }
    const std::unordered_set<Entity> nodes = seen;

    for (Entity animation : model.animations) {
        add(animation);
    }
    // An animation's tracks are the model's when they drive it: a node, or the model itself for a
    // channel without a node. A track the user pointed elsewhere stays with what it drives.
    for (Entity animation : model.animations) {
        const AnimationComponent* animationComp = animation != NULL_ENTITY && scene->isEntityCreated(animation) ?
            scene->findComponent<AnimationComponent>(animation) : nullptr;
        if (!animationComp) continue;
        for (const ActionFrame& frame : animationComp->actions) {
            if (frame.action == NULL_ENTITY || !scene->isEntityCreated(frame.action)) continue;
            const ActionComponent* action = scene->findComponent<ActionComponent>(frame.action);
            if (!action) continue;
            if (action->target == NULL_ENTITY || nodes.count(action->target) || seen.count(action->target)) {
                add(frame.action);
            }
        }
    }
    return owned;
}

std::vector<editor::ModelClearCmd::ParkedEntity> editor::ModelClearCmd::collectParkedEntities(Project* project, uint32_t sceneId, Scene* scene, const std::vector<Entity>& owned, Entity entity){
    std::vector<ParkedEntity> parked;
    const std::unordered_set<Entity> ownedSet(owned.begin(), owned.end());
    // A Prefab GameObject hung on a node is the Prefab's: below a node the instance removes,
    // it is out of the instance too, as Unity leaves out what is below a removed GameObject
    std::unordered_set<Entity> prefabMembers;
    for (const auto& [prefabPath, prefab] : project->getPrefabs(sceneId)) {
        auto sceneInstances = prefab->instances.find(sceneId);
        if (sceneInstances == prefab->instances.end()) {
            continue;
        }
        for (const Prefab::Instance& instance : sceneInstances->second) {
            for (const Prefab::EntityMember& member : instance.members) {
                if (!ownedSet.count(member.localEntity)) {
                    prefabMembers.insert(member.localEntity);
                }
            }
        }
    }
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        const Entity candidate = transforms->getEntity(i);
        const Transform& transform = transforms->getComponentFromIndex(i);
        if (candidate == entity || ownedSet.count(candidate) || !ownedSet.count(transform.parent) ||
            prefabMembers.count(candidate)) {
            continue;
        }
        ParkedEntity entry;
        entry.entity = candidate;
        entry.parent = transform.parent;
        entry.transformIndex = i;
        entry.position = transform.position;
        entry.rotation = transform.rotation;
        entry.scale = transform.scale;
        parked.push_back(entry);
    }
    return parked;
}

void editor::ModelClearCmd::unparkEntities(SceneProject* sceneProject){
    Scene* scene = sceneProject->scene;
    for (const ParkedEntity& parked : parkedEntities) {
        if (!scene->isEntityCreated(parked.entity) || !scene->isEntityCreated(parked.parent)) {
            continue;
        }
        ProjectUtils::moveEntityOrderByIndex(scene, sceneProject->entities, parked.entity, parked.parent, parked.transformIndex, true, false);
        Transform& transform = scene->getComponent<Transform>(parked.entity);
        transform.position = parked.position;
        transform.rotation = parked.rotation;
        transform.scale = parked.scale;
        transform.needUpdate = true;
    }
    if (!parkedEntities.empty()) {
        ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    }
    parkedEntities.clear();
}

void editor::ModelClearCmd::overrideInPrefab(Entity target, ComponentType type){
    // Unity: the cleared component of a prefab instance's GameObject (its root included) is an
    // override of that instance, all of it
    prefabUndo.capture(project, sceneId, target);
    project->recordPrefabEdit(sceneId, target, type, {});
}

void editor::ModelClearCmd::releasePendingLoad(Scene* scene, const ModelComponent& model){
    // A reload of this file still parsing on a worker (an undo or a scene open starts one) is
    // dropped with the model, unless another Model of the scene waits for the same file
    if (!model.needUpdateModel || model.filename.empty()) {
        return;
    }
    std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();
    if (!meshSys->isAsyncModelLoadPending(entity, model.filename)) {
        return;
    }
    const std::string key = MeshSystem::getModelFilenameKey(model.filename);
    auto models = scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); i++) {
        if (models->getEntity(i) == entity) continue;
        const ModelComponent& other = models->getComponentFromIndex(i);
        if (other.needUpdateModel && !other.filename.empty() && MeshSystem::getModelFilenameKey(other.filename) == key) {
            return;
        }
    }
    meshSys->cancelAsyncModelLoad(entity, model.filename);
}

void editor::ModelClearCmd::clearComponents(Scene* scene){
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    releasePendingLoad(scene, model);

    // Drops the parsed file (and its pool entry once no other Model shares it)
    scene->getSystem<MeshSystem>()->destroyModel(model);
    const bool enabled = model.enabled;
    model = ModelComponent{};
    model.skeleton = NULL_ENTITY;
    model.enabled = enabled;
    model.needUpdateModel = false;

    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
        releaseMeshResources(scene, entity, *mesh);
        *mesh = emptyModelMesh(*mesh);
    }
}

bool editor::ModelClearCmd::execute(){
    executed = false;

    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<ModelComponent>(entity)) {
        return false;
    }

    // A load of this Model still running would put the model back after it is emptied (a redo
    // can come right behind the redo of that load), so it lands now
    ModelLoadCmd::completePendingLoad(sceneId, entity);
    // what is saved below (the mesh's blend shape weights, the world kept by what hangs on the
    // model's nodes) is the rest: a clip preview of, under or above the model restores and a pose
    // suspends first (animation clips 9.R7, final review I1)
    PoseEditSession::suspendActiveFor(sceneId, {entity});

    const ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    if (!hasContent(model)) {
        return false;
    }

    wasModified = sceneProject->isModified;

    // reset, not assignment: assigning to a yaml-cpp node that holds one keeps the old
    // memory alive with it, which would grow on every redo. The mesh is saved as a scene
    // saves a model's: its geometry, textures and bones come back from the file.
    oldModel.reset(Stream::encodeModelComponent(model));
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    hadMesh = mesh != nullptr;
    oldMesh.reset(hadMesh ? Stream::encodeMeshComponent(*mesh, false, false, false) : YAML::Node());

    const std::vector<Entity> owned = collectOwnedEntities(scene, entity, model);
    const std::unordered_set<Entity> ownedSet(owned.begin(), owned.end());

    // What the user hung on a model node stays, on the GameObject and where it is in the world
    parkedEntities = collectParkedEntities(project, sceneId, scene, owned, entity);
    for (const ParkedEntity& parked : parkedEntities) {
        scene->addEntityChild(entity, parked.entity, true);
    }
    if (!parkedEntities.empty()) {
        ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    }

    // Undo restores each root at its old index, so earlier roots come first: the entities
    // without a Transform by their place in the tracked list, then the hierarchy roots
    std::vector<Entity> flatRoots;
    std::vector<Entity> transformRoots;
    for (Entity owner : owned) {
        const Transform* transform = scene->findComponent<Transform>(owner);
        if (!transform) {
            flatRoots.push_back(owner);
        } else if (!ownedSet.count(transform->parent)) {
            transformRoots.push_back(owner);
        }
    }
    std::unordered_map<Entity, size_t> trackedIndex;
    for (size_t i = 0; i < sceneProject->entities.size(); i++) {
        trackedIndex.emplace(sceneProject->entities[i], i);
    }
    auto trackedPosition = [&trackedIndex](Entity e) {
        auto it = trackedIndex.find(e);
        return it != trackedIndex.end() ? it->second : trackedIndex.size();
    };
    std::sort(flatRoots.begin(), flatRoots.end(), [&](Entity a, Entity b) { return trackedPosition(a) < trackedPosition(b); });
    std::sort(transformRoots.begin(), transformRoots.end(), [scene](Entity a, Entity b) {
        return ProjectUtils::getTransformIndex(scene, a) < ProjectUtils::getTransformIndex(scene, b);
    });
    std::vector<Entity> roots = flatRoots;
    roots.insert(roots.end(), transformRoots.begin(), transformRoots.end());

    retargetedActions.clear();
    prefabUndo.clear();
    if (!roots.empty()) {
        // Deleting a node also deletes the actions aimed at it. The ones that are not the
        // model's (a user's Action on a node) stay, aimed at no GameObject until an undo.
        std::vector<Entity> preserved;
        for (Entity virtualChild : ProjectUtils::getVirtualChildren(scene, roots)) {
            if (!ownedSet.count(virtualChild)) {
                preserved.push_back(virtualChild);
            }
        }

        // Nodes that are Prefab GameObjects of an instance are removed from that instance only
        // (removed GameObject overrides): the Prefab and its other instances keep them
        deleteCmd = new DeleteEntityCmd(project, sceneId, roots, true, preserved);
        deleteCmd->setPrefabMemberDelete(PrefabMemberDelete::RemoveFromInstance);
        if (!deleteCmd->execute()) {
            delete deleteCmd;
            deleteCmd = nullptr;
            unparkEntities(sceneProject);
            return false;
        }

        for (Entity kept : preserved) {
            ActionComponent* action = scene->findComponent<ActionComponent>(kept);
            if (action && ownedSet.count(action->target)) {
                retargetedActions.push_back({kept, action->target});
                action->target = NULL_ENTITY;
                overrideInPrefab(kept, ComponentType::ActionComponent);
            }
        }
    }

    clearComponents(scene);

    // In a Prefab instance the emptied Model is this instance's own: an override, as an
    // edit after Override Prefab Component, so the Prefab and its other instances keep theirs
    overrideInPrefab(entity, ComponentType::ModelComponent);
    if (hadMesh) {
        overrideInPrefab(entity, ComponentType::MeshComponent);
    }

    sceneProject->isModified = true;
    executed = true;
    return true;
}

void editor::ModelClearCmd::undo(){
    if (!executed) {
        return;
    }
    executed = false;

    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return;
    }
    Scene* scene = sceneProject->scene;
    // the restored values are the rest: a preview started since (on the emptied GameObject or
    // above it) must not snapshot over them (9.R7)
    PoseEditSession::suspendActiveFor(sceneId, {entity});

    if (hadMesh) {
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            releaseMeshResources(scene, entity, *mesh);
            *mesh = Stream::decodeMeshComponent(oldMesh);
        }
    }
    if (ModelComponent* model = scene->findComponent<ModelComponent>(entity)) {
        scene->getSystem<MeshSystem>()->destroyModel(*model);
        // Reloads the file into the restored nodes, as opening the scene does
        *model = Stream::decodeModelComponent(oldModel);
    }

    if (deleteCmd) {
        deleteCmd->undo();
        delete deleteCmd;
        deleteCmd = nullptr;
    }

    for (const RetargetedAction& retargeted : retargetedActions) {
        if (ActionComponent* action = scene->findComponent<ActionComponent>(retargeted.entity)) {
            action->target = retargeted.target;
        }
    }
    retargetedActions.clear();
    prefabUndo.restore(project, sceneId);
    prefabUndo.clear();

    unparkEntities(sceneProject);

    sceneProject->isModified = wasModified;
}
