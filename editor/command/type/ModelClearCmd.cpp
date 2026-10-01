// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelClearCmd.h"

#include "Stream.h"
#include "command/type/ModelLoadCmd.h"
#include "util/ProjectUtils.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
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
        empty.cullingMode = mesh.cullingMode;
        empty.windingOrder = mesh.windingOrder;
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
    deleteCmd = nullptr;
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
    const std::string blocker = prefabNodesBlocker(project, sceneId, collectOwnedEntities(scene, *model));
    if (!blocker.empty()) {
        return fail(blocker);
    }
    return true;
}

std::string editor::ModelClearCmd::prefabNodesBlocker(Project* project, uint32_t sceneId, const std::vector<Entity>& owned){
    // A Prefab instance holds every GameObject of its Prefab: one cannot be left out of a single
    // instance, and deleting it deletes it from the Prefab and all its instances
    for (Entity owner : owned) {
        const std::filesystem::path prefabPath = project->findPrefabPathFor(sceneId, owner);
        if (!prefabPath.empty()) {
            return "The model's GameObjects are part of Prefab '" + prefabPath.stem().string() +
                "': unpack the instance to empty its Model without changing the Prefab";
        }
    }
    return std::string();
}

std::vector<Entity> editor::ModelClearCmd::collectOwnedEntities(Scene* scene, const ModelComponent& model){
    std::vector<Entity> owned;
    std::unordered_set<Entity> seen;
    auto add = [&](Entity e) {
        if (e != NULL_ENTITY && scene->isEntityCreated(e) && seen.insert(e).second) {
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
        if (seen.count(model.skeleton) || (bone && seen.count(bone->model))) {
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

std::vector<editor::ModelClearCmd::ParkedEntity> editor::ModelClearCmd::collectParkedEntities(Scene* scene, const std::vector<Entity>& owned, Entity entity){
    std::vector<ParkedEntity> parked;
    const std::unordered_set<Entity> ownedSet(owned.begin(), owned.end());
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        const Entity candidate = transforms->getEntity(i);
        const Transform& transform = transforms->getComponentFromIndex(i);
        if (candidate == entity || ownedSet.count(candidate) || !ownedSet.count(transform.parent)) {
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
    // Put back in array order, each at its old index once the earlier ones are in place
    std::sort(parked.begin(), parked.end(), [](const ParkedEntity& a, const ParkedEntity& b) {
        return a.transformIndex < b.transformIndex;
    });
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
    Prefab* prefab = project->getPrefab(project->findPrefabPathFor(sceneId, target));
    // A prefab root is not in the Prefab: its components are the instance's already
    if (!prefab || prefab->getRegistryEntity(sceneId, target) == NULL_ENTITY) {
        return;
    }
    prefabOverrides.push_back({target, type, prefab->hasComponentOverride(sceneId, target, type)});
    prefab->setComponentOverride(sceneId, target, type);
}

void editor::ModelClearCmd::restorePrefabOverrides(){
    for (auto it = prefabOverrides.rbegin(); it != prefabOverrides.rend(); ++it) {
        if (it->wasOverridden) {
            continue;
        }
        if (Prefab* prefab = project->getPrefab(project->findPrefabPathFor(sceneId, it->entity))) {
            prefab->clearComponentOverride(sceneId, it->entity, it->type);
        }
    }
    prefabOverrides.clear();
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

    const std::vector<Entity> owned = collectOwnedEntities(scene, model);
    const std::unordered_set<Entity> ownedSet(owned.begin(), owned.end());
    if (!prefabNodesBlocker(project, sceneId, owned).empty()) {
        return false;
    }

    // What the user hung on a model node stays, on the GameObject and where it is in the world
    parkedEntities = collectParkedEntities(scene, owned, entity);
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
    const std::vector<Entity>& tracked = sceneProject->entities;
    auto trackedIndex = [&tracked](Entity e) {
        return static_cast<size_t>(std::distance(tracked.begin(), std::find(tracked.begin(), tracked.end(), e)));
    };
    std::sort(flatRoots.begin(), flatRoots.end(), [&](Entity a, Entity b) { return trackedIndex(a) < trackedIndex(b); });
    std::sort(transformRoots.begin(), transformRoots.end(), [scene](Entity a, Entity b) {
        return ProjectUtils::getTransformIndex(scene, a) < ProjectUtils::getTransformIndex(scene, b);
    });
    std::vector<Entity> roots = flatRoots;
    roots.insert(roots.end(), transformRoots.begin(), transformRoots.end());

    retargetedActions.clear();
    prefabOverrides.clear();
    if (!roots.empty()) {
        // Deleting a node also deletes the actions aimed at it. The ones that are not the
        // model's (a user's Action on a node) stay, aimed at no GameObject until an undo.
        std::vector<Entity> preserved;
        for (Entity virtualChild : ProjectUtils::getVirtualChildren(scene, roots)) {
            if (!ownedSet.count(virtualChild)) {
                preserved.push_back(virtualChild);
            }
        }

        deleteCmd = new DeleteEntityCmd(project, sceneId, roots, true, preserved);
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
    restorePrefabOverrides();

    unparkEntities(sceneProject);

    sceneProject->isModified = wasModified;
}

bool editor::ModelClearCmd::mergeWith(editor::Command* otherCommand){
    return false;
}
