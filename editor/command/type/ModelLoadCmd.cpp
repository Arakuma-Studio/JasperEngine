// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelLoadCmd.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "sceneview/SurfacePlacement.h"
#include "model/ModelHeader.h"
#include "model/ModelImportSettings.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"

#include "Stream.h"
#include "animation/PreviewOwner.h"
#include "pose/PoseEditSession.h"
#include "tiny_gltf.h"
#include "util/ProjectUtils.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "Engine.h"
#include "subsystem/MeshSystem.h"
#include "io/FileData.h"
#include "Backend.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
namespace fs = std::filesystem;

editor::ModelLoadCmd::ModelLoadCmd(Project* project, uint32_t sceneId, Entity entity, const std::string& modelPath){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->modelPath = modelPath;

    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::ModelLoadCmd::ModelLoadCmd(Project* project, uint32_t sceneId, Entity entity, const std::string& modelPath, bool mergeStaticMeshes)
    : ModelLoadCmd(project, sceneId, entity, modelPath) {
    hasMergeStaticMeshesOverride = true;
    mergeStaticMeshesOverride = mergeStaticMeshes;
}

editor::ModelLoadCmd::ModelLoadCmd(Project* project, uint32_t sceneId, const std::string& entityName, const Vector3& position, const std::string& modelPath){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = NULL_ENTITY;
    this->modelPath = modelPath;

    this->wasModified = project->getScene(sceneId)->isModified;

    createEntityCmd = new CreateEntityCmd(project, sceneId, entityName, EntityCreationType::MODEL);
    createEntityCmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
    dropPoint = position;
}

void editor::ModelLoadCmd::setDropSurface(const Vector3& normal, bool align){
    hasDropSurface = true;
    dropNormal = normal;
    dropAlign = align;
}

editor::ModelLoadCmd::ModelLoadCmd(Project* project, uint32_t sceneId, const std::string& entityName, Entity parent, const Vector3& position, const Quaternion& rotation, const Vector3& scale, const std::string& modelPath){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = NULL_ENTITY;
    this->modelPath = modelPath;

    this->wasModified = project->getScene(sceneId)->isModified;

    createEntityCmd = new CreateEntityCmd(project, sceneId, entityName, EntityCreationType::MODEL, parent);
    createEntityCmd->setQuiet(true);
    createEntityCmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
    createEntityCmd->addProperty<Quaternion>(ComponentType::Transform, "rotation", rotation);
    createEntityCmd->addProperty<Vector3>(ComponentType::Transform, "scale", scale);
}

editor::ModelLoadCmd::~ModelLoadCmd(){
    if (cancelFlag) cancelFlag->store(true);
    if (asyncPending){
        Scene* scene = project->getScene(sceneId)->scene;
        scene->getSystem<MeshSystem>()->cancelAsyncModelLoad(entity, modelPath);
        setAsyncPending(false);
    }
    delete oldSubEntitiesDeleteCmd;
    delete createEntityCmd;
}

std::vector<Entity> editor::ModelLoadCmd::collectModelDeleteRoots(Scene* scene, const ModelComponent& model) {
    std::vector<Entity> roots;
    roots.insert(roots.end(), model.animations.begin(), model.animations.end());

    if (!model.nodesIdMapping.empty()) {
        // Nodes still under a model node go with it; a part the user moved out is its own root
        std::vector<Entity> nodeRoots;
        for (const auto& node : model.nodesIdMapping) {
            Transform* transform = scene->findComponent<Transform>(node.second);
            if (transform && !ProjectUtils::isModelNode(model, transform->parent)) {
                nodeRoots.push_back(node.second);
            }
        }
        // Undo restores each root at its old transform index, so earlier roots must come first
        std::sort(nodeRoots.begin(), nodeRoots.end(), [scene](Entity a, Entity b) {
            return ProjectUtils::getTransformIndex(scene, a) < ProjectUtils::getTransformIndex(scene, b);
        });
        roots.insert(roots.end(), nodeRoots.begin(), nodeRoots.end());
        return roots;
    }

    if (model.skeleton != NULL_ENTITY) {
        roots.push_back(model.skeleton);
    }
    for (const auto& node : model.meshNodesMapping) {
        roots.push_back(node.second);
    }
    return roots;
}

bool editor::ModelLoadCmd::isMappedMeshNode(const ModelComponent& model, Entity entity) {
    for (const auto& node : model.meshNodesMapping) {
        if (node.second == entity) {
            return true;
        }
    }
    return false;
}

editor::ModelLoadCmd::NodeRef editor::ModelLoadCmd::makeNodeRef(const ModelComponent& model, int nodeIndex, bool bone) {
    // Use the file's name even if the user renamed the entity; unnamed nodes use their index.
    return {nodeIndex, MeshSystem::getModelNodeName(model, nodeIndex), bone};
}

Entity editor::ModelLoadCmd::findModelNode(const ModelComponent& model, const NodeRef& ref) {
    if (ref.index < 0) {
        return NULL_ENTITY;
    }

    // The flat path keeps parts and bones in their own maps, and a re-export can switch paths
    const std::map<int, Entity>* nodes = &model.nodesIdMapping;
    if (nodes->empty()) {
        nodes = ref.bone ? &model.bonesIdMapping : &model.meshNodesMapping;
    }
    // Same index and name first, then the name alone, then the index alone.
    auto sameIndex = nodes->find(ref.index);
    if (sameIndex != nodes->end() && MeshSystem::getModelNodeName(model, ref.index) == ref.name) {
        return sameIndex->second;
    }
    if (!ref.name.empty()) {
        for (const auto& node : *nodes) {
            if (MeshSystem::getModelNodeName(model, node.first) == ref.name) {
                return node.second;
            }
        }
    }
    return sameIndex != nodes->end() ? sameIndex->second : NULL_ENTITY;
}

// The local is kept: an offset from a bone is the user's placement, the file drives the rest
void editor::ModelLoadCmd::attachLocal(Scene* scene, Entity child, Entity parent, const LocalPose& pose) {
    scene->addEntityChild(parent, child, false);
    Transform& transform = scene->getComponent<Transform>(child);
    transform.position = pose.position;
    transform.rotation = pose.rotation;
    transform.scale = pose.scale;
    transform.needUpdate = true;
}

void editor::ModelLoadCmd::recordArrangement(SceneProject* sceneProject, const ModelComponent& model) {
    Scene* scene = sceneProject->scene;

    std::unordered_map<Entity, NodeRef> modelNodes;
    for (const auto& node : model.nodesIdMapping) {
        modelNodes.emplace(node.second, makeNodeRef(model, node.first, scene->findComponent<BoneComponent>(node.second) != nullptr));
    }
    for (const auto& node : model.meshNodesMapping) {
        modelNodes.emplace(node.second, makeNodeRef(model, node.first, false));
    }
    for (const auto& bone : model.bonesIdMapping) {
        modelNodes.emplace(bone.second, makeNodeRef(model, bone.first, true));
    }

    for (Entity candidate : sceneProject->entities) {
        Transform* transform = scene->findComponent<Transform>(candidate);
        if (!transform || candidate == entity || modelNodes.count(candidate)) {
            continue;
        }
        auto parent = modelNodes.find(transform->parent);
        if (parent == modelNodes.end()) {
            continue;
        }
        ParkedEntity parked;
        parked.entity = candidate;
        parked.oldParent = transform->parent;
        parked.parent = parent->second;
        parked.pose = {transform->position, transform->rotation, transform->scale};
        parkedEntities.push_back(parked);
    }

    std::map<int, Entity> defaults = scene->getSystem<MeshSystem>()->getModelNodeDefaultParents(entity, model);
    for (const auto& node : model.meshNodesMapping) {
        Transform* transform = scene->findComponent<Transform>(node.second);
        auto defaultParent = defaults.find(node.first);
        if (!transform || defaultParent == defaults.end() || transform->parent == defaultParent->second) {
            continue;
        }
        MovedPart moved;
        moved.part = makeNodeRef(model, node.first, false);
        auto parent = modelNodes.find(transform->parent);
        if (parent != modelNodes.end()) {
            moved.nodeParent = parent->second;
        } else {
            moved.userParent = transform->parent;
        }
        moved.pose = {transform->position, transform->rotation, transform->scale};
        movedParts.push_back(moved);
    }
}

void editor::ModelLoadCmd::recordMorphWeights(Scene* scene, const ModelComponent& model) {
    savedMorphWeights.clear();
    // the weights a part owns: the targets they cover (MeshComponent::morphTargetCount), so the
    // targets a re-exported file gained keep the defaults the rebuild gives them
    auto weightsOf = [scene](Entity part) {
        std::vector<float> weights;
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(part);
        if (mesh && mesh->morphWeightsSet) {
            const int count = std::clamp(mesh->morphTargetCount, 0, MAX_MORPHTARGETS);
            weights.assign(mesh->morphWeights, mesh->morphWeights + count);
        }
        return weights;
    };
    for (const auto& node : model.meshNodesMapping) {
        std::vector<float> weights = weightsOf(node.second);
        if (!weights.empty()) savedMorphWeights[node.first] = weights;
    }
    // the model entity's own mesh (a lone mesh, a skin without its nodes): the one mesh node
    // no part stands for
    if (model.gltfModel) {
        int unmapped = -1;
        size_t count = 0;
        for (size_t i = 0; i < model.gltfModel->nodes.size(); i++) {
            if (model.gltfModel->nodes[i].mesh < 0 || model.meshNodesMapping.count((int)i)) continue;
            unmapped = (int)i;
            count++;
        }
        if (count == 1) {
            std::vector<float> weights = weightsOf(entity);
            if (!weights.empty()) savedMorphWeights[unmapped] = weights;
        }
    }
}

void editor::ModelLoadCmd::restoreMorphWeights(Scene* scene, const ModelComponent& model) {
    for (const auto& saved : savedMorphWeights) {
        Entity part = NULL_ENTITY;
        auto mesh = model.meshNodesMapping.find(saved.first);
        if (mesh != model.meshNodesMapping.end()) {
            part = mesh->second;
        } else {
            auto node = model.nodesIdMapping.find(saved.first);
            if (node != model.nodesIdMapping.end()) part = node->second;
        }
        MeshComponent* component = part != NULL_ENTITY && scene->isEntityCreated(part) ? scene->findComponent<MeshComponent>(part) : nullptr;
        if (!component) continue;
        for (size_t i = 0; i < MAX_MORPHTARGETS && i < saved.second.size(); i++) {
            component->morphWeights[i] = saved.second[i];
        }
        // the parts are new entities (their load gave them the file's defaults): the carried
        // weights are theirs now, kept by later reloads like saved ones (MeshComponent::morphWeightsSet)
        component->morphWeightsSet = true;
        component->morphTargetCount = std::max(component->morphTargetCount, (int)saved.second.size());
    }
}

namespace {
    template <typename From, typename To>
    void copyPartMeshSettings(const From& from, To& to) {
        to.castShadows = from.castShadows;
        to.receiveShadows = from.receiveShadows;
        to.staticShadowCaster = from.staticShadowCaster;
        to.receiveLights = from.receiveLights;
        to.receiveIBL = from.receiveIBL;
        to.cullingMode = from.cullingMode;
        to.lodEnabled = from.lodEnabled;
        to.lodBias = from.lodBias;
        to.transparent = from.transparent;
        to.autoTransparency = from.autoTransparency;
        to.customShader = from.customShader;
        to.customDepthShader = from.customDepthShader;
        to.renderInReflectionProbes = from.renderInReflectionProbes;
        to.shadowsBillboard = from.shadowsBillboard;
        to.allowGpuInstancing = from.allowGpuInstancing;
        to.lodFadeMode = from.lodFadeMode;
        to.lodAnimateCrossFading = from.lodAnimateCrossFading;
        to.lodFadeTransitionWidth = from.lodFadeTransitionWidth;
        to.lodCullBelow = from.lodCullBelow;
        to.shaderUniforms = from.shaderUniforms;
    }
}

void editor::ModelLoadCmd::recordPartMeshSettings(Scene* scene, const ModelComponent& model) {
    savedPartSettings.clear();
    for (const auto& node : model.meshNodesMapping) {
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(node.second);
        if (!mesh) continue;
        PartMeshSettings settings;
        settings.part = makeNodeRef(model, node.first, false);
        copyPartMeshSettings(*mesh, settings);
        savedPartSettings.push_back(std::move(settings));
    }
}

void editor::ModelLoadCmd::restorePartMeshSettings(Scene* scene, const ModelComponent& model) {
    for (const PartMeshSettings& settings : savedPartSettings) {
        const Entity part = findModelNode(model, settings.part);
        MeshComponent* mesh = part != NULL_ENTITY && scene->isEntityCreated(part) ? scene->findComponent<MeshComponent>(part) : nullptr;
        if (!mesh) continue;
        copyPartMeshSettings(settings, *mesh);
        // the shader, its variants and the uniform blocks follow the settings
        mesh->needReload = true;
        mesh->needUpdateShaderUniforms = true;
    }
}

void editor::ModelLoadCmd::recordRetargets(Scene* scene, const ModelComponent& model) {
    retargetedActions.clear();
    std::unordered_map<Entity, NodeRef> modelNodes;
    for (const auto& node : model.nodesIdMapping) {
        modelNodes.emplace(node.second, makeNodeRef(model, node.first, scene->findComponent<BoneComponent>(node.second) != nullptr));
    }
    for (const auto& node : model.meshNodesMapping) {
        modelNodes.emplace(node.second, makeNodeRef(model, node.first, false));
    }
    for (const auto& bone : model.bonesIdMapping) {
        modelNodes.emplace(bone.second, makeNodeRef(model, bone.first, true));
    }
    if (modelNodes.empty()) return;

    // the loader's own animations go with the model and come back from the file
    std::unordered_set<Entity> generated(model.animations.begin(), model.animations.end());
    for (Entity animation : model.animations) {
        if (const AnimationComponent* component = scene->findComponent<AnimationComponent>(animation)) {
            for (const ActionFrame& frame : component->actions) generated.insert(frame.action);
        }
    }
    auto actions = scene->getComponentArray<ActionComponent>();
    for (size_t i = 0; i < actions->size(); i++) {
        const Entity action = actions->getEntity(i);
        if (generated.count(action) || modelNodes.count(action)) continue;
        auto node = modelNodes.find(actions->getComponentFromIndex(i).target);
        if (node == modelNodes.end()) continue;
        retargetedActions.push_back({action, node->first, node->second});
    }
}

void editor::ModelLoadCmd::detachRetargets(Scene* scene) {
    for (const RetargetedAction& retarget : retargetedActions) {
        if (ActionComponent* action = scene->isEntityCreated(retarget.action) ? scene->findComponent<ActionComponent>(retarget.action) : nullptr) {
            action->target = NULL_ENTITY;
        }
    }
}

void editor::ModelLoadCmd::applyRetargets(Scene* scene, const ModelComponent& model) {
    for (const RetargetedAction& retarget : retargetedActions) {
        ActionComponent* action = scene->isEntityCreated(retarget.action) ? scene->findComponent<ActionComponent>(retarget.action) : nullptr;
        if (!action) continue;
        const Entity node = findModelNode(model, retarget.node);
        if (node == NULL_ENTITY) {
            Log::warn("Node %d '%s' is no longer in '%s': '%s' has no target now", retarget.node.index, retarget.node.name.c_str(),
                model.filename.c_str(), scene->getEntityName(retarget.action).c_str());
        }
        action->target = node;
    }
}

void editor::ModelLoadCmd::restoreRetargets(Scene* scene) {
    for (const RetargetedAction& retarget : retargetedActions) {
        if (ActionComponent* action = scene->isEntityCreated(retarget.action) ? scene->findComponent<ActionComponent>(retarget.action) : nullptr) {
            action->target = retarget.oldTarget;
        }
    }
}

void editor::ModelLoadCmd::parkEntities(SceneProject* sceneProject) {
    if (parkedEntities.empty()) return;
    Scene* scene = sceneProject->scene;
    for (const ParkedEntity& parked : parkedEntities) {
        if (scene->isEntityCreated(parked.entity)) {
            scene->addEntityChild(entity, parked.entity, true);
        }
    }
    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
}

void editor::ModelLoadCmd::unparkEntities(SceneProject* sceneProject) {
    if (parkedEntities.empty()) return;
    Scene* scene = sceneProject->scene;
    for (const ParkedEntity& parked : parkedEntities) {
        if (scene->isEntityCreated(parked.entity) && scene->isEntityCreated(parked.oldParent)) {
            attachLocal(scene, parked.entity, parked.oldParent, parked.pose);
        }
    }
    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
}

void editor::ModelLoadCmd::restoreArrangement(SceneProject* sceneProject, const ModelComponent& model) {
    if (parkedEntities.empty() && movedParts.empty()) return;
    Scene* scene = sceneProject->scene;
    std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();

    for (const ParkedEntity& parked : parkedEntities) {
        if (!scene->isEntityCreated(parked.entity)) {
            continue;
        }
        Entity parent = findModelNode(model, parked.parent);
        if (parent == NULL_ENTITY) {
            Log::warn("Node %d '%s' is no longer in '%s', '%s' was moved to the model", parked.parent.index,
                parked.parent.name.c_str(), model.filename.c_str(), scene->getEntityName(parked.entity).c_str());
            continue;
        }
        attachLocal(scene, parked.entity, parent, parked.pose);
    }

    // Validate against the complete new hierarchy before detaching any parts.
    struct Placement {
        Entity part;
        Entity parent;
        Entity defaultParent;
        const MovedPart* moved;
    };
    std::vector<Placement> placements;
    std::unordered_set<Entity> placedParts;
    for (const MovedPart& moved : movedParts) {
        Entity part = findModelNode(model, moved.part);
        Entity parent = moved.userParent != NULL_ENTITY ? moved.userParent : findModelNode(model, moved.nodeParent);
        std::string reason;
        if (part == NULL_ENTITY || !isMappedMeshNode(model, part)) {
            reason = "it is no longer a mesh part";
        } else if (placedParts.count(part)) {
            reason = "another saved part already matched this node";
        } else if (meshSys->canEditModelPart(model, part, &reason) && (parent == NULL_ENTITY || !scene->isEntityCreated(parent))) {
            reason = "its parent is gone";
        }
        if (!reason.empty()) {
            Log::warn("Mesh part %d '%s' of '%s' stays where the file puts it: %s", moved.part.index,
                moved.part.name.c_str(), model.filename.c_str(), reason.c_str());
            continue;
        }
        placedParts.insert(part);
        placements.push_back({part, parent, scene->getComponent<Transform>(part).parent, &moved});
    }
    // Detach all accepted parts so reversing a parent/child arrangement is independent of index order.
    for (const Placement& placement : placements) {
        scene->addEntityChild(entity, placement.part, false);
    }
    for (const Placement& placement : placements) {
        if (placement.part == placement.parent || scene->isParentOf(placement.part, placement.parent)) {
            Log::warn("Cannot restore mesh part %d '%s' of '%s': it would contain its parent",
                placement.moved->part.index, placement.moved->part.name.c_str(), model.filename.c_str());
            // Back to where the loader put it, unless that is now below the part as well
            if (!scene->isParentOf(placement.part, placement.defaultParent)) {
                scene->addEntityChild(placement.defaultParent, placement.part, false);
            }
            continue;
        }
        attachLocal(scene, placement.part, placement.parent, placement.moved->pose);
    }

    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
}

bool editor::ModelLoadCmd::tryLoad(){
    Scene* scene = project->getScene(sceneId)->scene;
    std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();
    bool useAsync = Engine::isAsyncLoading();
    std::string ext = FileData::getFilePathExtension(modelPath);
    if (ext == "obj"){
        return meshSys->loadOBJ(entity, modelPath, useAsync);
    }
    return meshSys->loadGLTF(entity, modelPath, useAsync, reuseHierarchy, isNewModel);
}

void editor::ModelLoadCmd::finalizeLoad(){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    ModelComponent& newModel = scene->getComponent<ModelComponent>(entity);
    newModel.needUpdateModel = false;

    std::vector<Entity> newSubEntities;
    ProjectUtils::collectModelEntities(scene, newModel, newSubEntities);
    for (const auto& e : newSubEntities){
        if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), e) == sceneProject->entities.end()){
            sceneProject->entities.push_back(e);
        }
    }
    // the nodes came in glTF index order, children before parents: the list (and so the
    // generated scene code) creates parents first, as the Transform array has them
    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);

    for (Entity reused : reusedEntities) {
        if (!isMappedMeshNode(newModel, reused)) {
            Log::warn("Mesh part '%s' is no longer in '%s' and became a plain entity",
                scene->getEntityName(reused).c_str(), newModel.filename.c_str());
        }
    }

    restoreArrangement(sceneProject, newModel);
    applyRetargets(scene, newModel);
    restorePartMeshSettings(scene, newModel);
    if (switchesLayout) {
        restoreMorphWeights(scene, newModel);
    }

    // a drop rests the loaded model on the surface it was dropped on
    if (hasDropSurface && isNewModel){
        Transform& transform = scene->getComponent<Transform>(entity);
        Vector3 position;
        Quaternion rotation;
        sceneview::restOnSurface(dropPoint, dropNormal, sceneview::familyLocalBounds(scene, entity), transform.scale, transform.rotation,
                                 dropAlign, position, rotation);
        transform.position = position;
        transform.rotation = rotation;
        transform.needUpdate = true;
    }

    // Put back on whichever mesh the primitive ended up in, which a merge change moves.
    scene->getSystem<MeshSystem>()->applySubmeshOverrides(savedSubmeshOverrides, entity, newModel);

    sceneProject->isModified = true;

    // The model's file, node mappings and the mesh it built are the instance's now
    project->recordPrefabEdit(sceneId, entity, ComponentType::ModelComponent, {});
    project->recordPrefabEdit(sceneId, entity, ComponentType::MeshComponent, {});
}

void editor::ModelLoadCmd::schedulePoll(){
    auto cancel = cancelFlag;
    Backend::getApp().enqueueMainThreadTask([this, cancel]() {
        if (cancel->load()) return;
        if (tryLoad()){
            setAsyncPending(false);
            finalizeLoad();
            return;
        }
        Scene* scene = project->getScene(sceneId)->scene;
        if (scene->getSystem<MeshSystem>()->isAsyncModelLoadPending(entity, modelPath)){
            schedulePoll();
        } else {
            // Terminal worker failure — drop pending state; rollback happens via the next undo
            setAsyncPending(false);
        }
    });
}

std::map<std::pair<uint32_t, Entity>, editor::ModelLoadCmd*>& editor::ModelLoadCmd::getPendingLoads(){
    static std::map<std::pair<uint32_t, Entity>, ModelLoadCmd*> pendingLoads;
    return pendingLoads;
}

void editor::ModelLoadCmd::setAsyncPending(bool pending){
    asyncPending = pending;
    auto& pendingLoads = getPendingLoads();
    const std::pair<uint32_t, Entity> key{sceneId, entity};
    if (pending){
        pendingLoads[key] = this;
    }else{
        auto it = pendingLoads.find(key);
        if (it != pendingLoads.end() && it->second == this){
            pendingLoads.erase(it);
        }
    }
}

void editor::ModelLoadCmd::finishPendingLoad(){
    if (!asyncPending){
        return;
    }
    // The queued poll stands down: the load ends here, with what the worker parsed
    if (cancelFlag) cancelFlag->store(true);
    Scene* scene = project->getScene(sceneId)->scene;
    std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();
    bool loaded = false;
    while (!(loaded = tryLoad()) && meshSys->isAsyncModelLoadPending(entity, modelPath)){
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    setAsyncPending(false);
    if (loaded){
        finalizeLoad();
    }
}

bool editor::ModelLoadCmd::isLoadPending(uint32_t sceneId, Entity entity){
    return getPendingLoads().count({sceneId, entity}) > 0;
}

void editor::ModelLoadCmd::completePendingLoad(uint32_t sceneId, Entity entity){
    auto& pendingLoads = getPendingLoads();
    auto it = pendingLoads.find({sceneId, entity});
    if (it != pendingLoads.end()){
        it->second->finishPendingLoad();
    }
}

bool editor::ModelLoadCmd::execute(){
    // the reload records the arrangement and moves the parts from their rest, not from a preview
    // of the model (the clip and entity modes, the Inspector's, a pose): they restore first (9.R7)
    if (!createEntityCmd && entity != NULL_ENTITY) {
        SceneProject* previewed = project->getScene(sceneId);
        const ModelComponent* loaded = previewed && previewed->scene && previewed->scene->isEntityCreated(entity)
            ? previewed->scene->findComponent<ModelComponent>(entity) : nullptr;
        // A model with a file (loaded before, or a reload undone and redone before its async load
        // finished) is rebuilt from the rest. A first load stops only a preview of the entity's own
        // values (its Transform becomes oldTransform and decides the root transform): a model
        // created under a previewed root leaves that preview and Record running.
        if (loaded && (!loaded->filename.empty() || PreviewOwner::writes(sceneId, entity))){
            PoseEditSession::suspendActiveFor(sceneId, {entity});
        }
    }
    // An FBX loads from its import, created here the first time
    if (!project->prepareModelFile(modelPath)) {
        return false;
    }

    if (createEntityCmd) {
        if (!createEntityCmd->execute()) {
            return false;
        }
        entity = createEntityCmd->getEntity();
    }

    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    Signature signature = scene->getSignature(entity);
    if (!signature.test(scene->getComponentId<Transform>())){
        Log::error("Entity %u does not have a Transform component", entity);
        return false;
    }
    if (!signature.test(scene->getComponentId<MeshComponent>())){
        Log::error("Entity %u does not have a MeshComponent", entity);
        return false;
    }
    if (!signature.test(scene->getComponentId<ModelComponent>())){
        Log::error("Entity %u does not have a ModelComponent", entity);
        return false;
    }

    Transform& transform = scene->getComponent<Transform>(entity);
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);

    if (hasMergeStaticMeshesOverride && mergeStaticMeshesOverride) {
        std::string reason;
        if (!scene->getSystem<MeshSystem>()->canMergeStaticModel(model, &reason)) {
            Log::warn("Cannot merge static model '%s': %s", model.filename.c_str(), reason.c_str());
            return false;
        }
        if (ProjectUtils::hasCustomMeshParenting(scene, entity)) {
            Log::warn("Cannot merge static model '%s': reset the mesh parenting first", model.filename.c_str());
            return false;
        }
    }

    isNewModel = model.filename.empty();

    // Save old component state
    const bool firstExecution = !oldModel.IsMap();
    // reset, not assignment: assigning to a yaml-cpp node that holds one keeps the old
    // memory alive with it, which would grow on every redo
    oldTransform.reset(Stream::encodeTransform(transform));
    oldMesh.reset(Stream::encodeMeshComponent(mesh, false, false));
    oldModel.reset(Stream::encodeModelComponent(model));

    // This is an import option for the current model asset, not an entity-wide preference.
    // A regular file assignment therefore returns to the default hierarchy behavior; the
    // explicit Hierarchy action is the only path that opts a model into static merging.
    const bool requestedMergeStaticMeshes = hasMergeStaticMeshesOverride
        ? mergeStaticMeshesOverride
        : false;
    const bool mergeStaticMeshesChanged = model.mergeStaticMeshes != requestedMergeStaticMeshes;

    const bool sameModelFile = MeshSystem::getModelFilenameKey(model.filename) == MeshSystem::getModelFilenameKey(modelPath);

    // A model on or under an Animation Player loads in the node-hierarchy layout (animation clips
    // spec 9.R3): one in another layout is rebuilt by this reload, the one step that switches it
    // (its arrangement is put back by node name below).
    if (firstExecution) {
        switchesLayout = sameModelFile && scene->getSystem<MeshSystem>()->needsNodeHierarchyReload(entity);
        movesLegacyRoot = switchesLayout && scene->getSystem<MeshSystem>()->getLegacyRootTransform(entity, legacyRoot);
    }

    // Only a reload that would throw away a local arrangement keeps the mesh children and
    // refreshes their geometry in place. Everything else rebuilds, so node changes still apply.
    reuseHierarchy = sameModelFile && !mergeStaticMeshesChanged && !switchesLayout
        && model.nodesIdMapping.empty() && ProjectUtils::hasCustomMeshParenting(scene, entity);

    reusedEntities.clear();
    if (reuseHierarchy) {
        for (const auto& node : model.meshNodesMapping) {
            reusedEntities.push_back(node.second);
        }
    }

    // A different asset is a fresh import and its own materials win. Reloading the same file keeps
    // the submesh edits, taken now because the meshes holding them are deleted below. A reuse keeps
    // those meshes, so the loader restores them and these ordinals would be the pre-remap ones.
    if (sameModelFile && !reuseHierarchy) {
        savedSubmeshOverrides = scene->getSystem<MeshSystem>()->collectSubmeshOverrides(entity, model);
    } else if (!sameModelFile) {
        for (unsigned int i = 0; i < mesh.numSubmeshes; i++) {
            mesh.submeshes[i].overrideFields = 0;
        }
    }

    // Rebuilt nodes get the user's arrangement back by node name. Only the first run has the
    // parsed glTF to name the nodes; a redo reuses what it recorded.
    if (firstExecution && sameModelFile && !reuseHierarchy) {
        recordArrangement(sceneProject, model);
        recordRetargets(scene, model);
        recordPartMeshSettings(scene, model);
    }
    if (firstExecution && switchesLayout) {
        recordMorphWeights(scene, model);
    }
    parkEntities(sceneProject);
    detachRetargets(scene);

    std::vector<Entity> oldSubEntityRoots;
    if (!reuseHierarchy) {
        oldSubEntityRoots = collectModelDeleteRoots(scene, model);
    }
    prefabUndo.capture(project, sceneId, entity);
    if (!oldSubEntityRoots.empty()) {
        // Unity: loading a model on a prefab instance's GameObject is an instance edit: its old
        // nodes that are prefab GameObjects become removed GameObject overrides, the new ones
        // added GameObjects (DeleteEntityCmd's default)
        oldSubEntitiesDeleteCmd = new DeleteEntityCmd(project, sceneId, oldSubEntityRoots, true);
        if (!oldSubEntitiesDeleteCmd->execute()) {
            delete oldSubEntitiesDeleteCmd;
            oldSubEntitiesDeleteCmd = nullptr;
            restoreRetargets(scene);
            unparkEntities(sceneProject);
            return false;
        }
    }

    // Clear stale model data before loading new model
    if (!reuseHierarchy) {
        model.skeleton = NULL_ENTITY;
        model.bonesIdMapping.clear();
        model.bonesNameMapping.clear();
        model.animations.clear();
        model.meshNodesMapping.clear();
        model.nodesIdMapping.clear();
        model.skinBindings.clear();
    }
    model.mergeStaticMeshes = requestedMergeStaticMeshes;

    // Project Settings > Animation > Animation Entities: Auto (S77 R38): a model placed with many
    // clips makes no Animation entity per clip. Only a first load: a reload keeps the user's choice.
    if (isNewModel && model.animationEntities && project->getModelSettings().animationEntities == AnimationEntitiesMode::Auto) {
        if (autoClipCount < 0) {
            const fs::path absolute = project->resolveAssetPath(FileUtils::pathFromUtf8(modelPath));
            const fs::path read = FbxImporter::isFbxFile(absolute) ? project->getImportedModelPath(absolute) : absolute;
            ModelHeader header;
            std::string headerError;
            autoClipCount = readModelHeader(read, header, headerError) ? header.animations : 0;
        }
        if (autoClipCount >= kAutoAnimationEntitiesClips) {
            model.animationEntities = false;
        }
    }

    // The scene root node gets an entity of its own in the node hierarchy, which carries the
    // matrix the legacy model entity stood in for: taken off the entity, everything stays where
    // it was (undo puts oldTransform back)
    if (movesLegacyRoot) {
        Transform& rootTransform = scene->getComponent<Transform>(entity);
        const Matrix4 local = Matrix4::translateMatrix(rootTransform.position) *
            rootTransform.rotation.getRotationMatrix() * Matrix4::scaleMatrix(rootTransform.scale);
        (local * legacyRoot.inverse()).decompose(rootTransform.position, rootTransform.scale, rootTransform.rotation);
        rootTransform.needUpdate = true;
    }

    if (tryLoad()){
        finalizeLoad();
        return true;
    }

    if (Engine::isAsyncLoading()){
        // Load is in progress on a worker thread — accept the command and finalize when ready
        setAsyncPending(true);
        cancelFlag = std::make_shared<std::atomic<bool>>(false);
        schedulePoll();
        return true;
    }

    // Synchronous failure — restore old state
    restoreTransform(scene);
    ProjectUtils::releaseMeshResources(scene, entity, scene->getComponent<MeshComponent>(entity));
    scene->getComponent<MeshComponent>(entity) = Stream::decodeMeshComponent(oldMesh);
    scene->getComponent<ModelComponent>(entity) = Stream::decodeModelComponent(oldModel);
    if (oldSubEntitiesDeleteCmd) {
        oldSubEntitiesDeleteCmd->undo();
        delete oldSubEntitiesDeleteCmd;
        oldSubEntitiesDeleteCmd = nullptr;
    }
    restoreRetargets(scene);
    unparkEntities(sceneProject);
    if (createEntityCmd) {
        createEntityCmd->undo();
    }
    return false;
}

// The parent is not encoded, so the entity stays where it is in the hierarchy
void editor::ModelLoadCmd::restoreTransform(Scene* scene){
    Transform& transform = scene->getComponent<Transform>(entity);
    transform = Stream::decodeTransform(oldTransform, &transform);
    transform.needUpdate = true;
}

void editor::ModelLoadCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;
    // the reload rebuilds (and records) the model from its rest, not from a preview (clips 9.R7)
    // (a first load's undo writes oldTransform back: from the rest when the entity is previewed)
    if (entity != NULL_ENTITY && scene->isEntityCreated(entity) && (!isNewModel || PreviewOwner::writes(sceneId, entity))){
        PoseEditSession::suspendActiveFor(sceneId, {entity});
    }

    if (asyncPending) {
        // Async load still in flight — cancel it; no sub-entities have been created yet
        if (cancelFlag) cancelFlag->store(true);
        scene->getSystem<MeshSystem>()->cancelAsyncModelLoad(entity, modelPath);
        setAsyncPending(false);
    } else {
        // The attached entities sit on nodes this load created, which are removed below
        parkEntities(sceneProject);
        detachRetargets(scene);

        ModelComponent& model = scene->getComponent<ModelComponent>(entity);
        std::vector<Entity> newSubEntityRoots = collectModelDeleteRoots(scene, model);
        if (reuseHierarchy) {
            // The kept children stay, so only the nodes this load added are removed
            newSubEntityRoots.erase(std::remove_if(newSubEntityRoots.begin(), newSubEntityRoots.end(),
                [this](Entity root) {
                    return std::find(reusedEntities.begin(), reusedEntities.end(), root) != reusedEntities.end();
                }), newSubEntityRoots.end());
        }
        if (!newSubEntityRoots.empty()) {
            DeleteEntityCmd newSubEntitiesDeleteCmd(project, sceneId, newSubEntityRoots, true);
            newSubEntitiesDeleteCmd.execute();
        }
    }

    // Restore old components
    restoreTransform(scene);
    ProjectUtils::releaseMeshResources(scene, entity, scene->getComponent<MeshComponent>(entity));
    scene->getComponent<MeshComponent>(entity) = Stream::decodeMeshComponent(oldMesh);
    scene->getComponent<ModelComponent>(entity) = Stream::decodeModelComponent(oldModel);

    if (oldSubEntitiesDeleteCmd) {
        oldSubEntitiesDeleteCmd->undo();
        delete oldSubEntitiesDeleteCmd;
        oldSubEntitiesDeleteCmd = nullptr;
    }
    restoreRetargets(scene);
    unparkEntities(sceneProject);

    sceneProject->isModified = wasModified;

    prefabUndo.restore(project, sceneId);

    if (createEntityCmd) {
        createEntityCmd->undo();
        entity = NULL_ENTITY;
    }
}

std::string editor::ModelLoadCmd::describe() const{
    const std::string file = CommandNames::file(modelPath);
    if (createEntityCmd){
        return "Create Model " + file;
    }
    return "Load Model " + file + " (" + CommandNames::entity(project, sceneId, entity) + ")";
}

size_t editor::ModelLoadCmd::memoryBytes() const{
    return sizeof(*this) + yamlFootprint(oldTransform) + yamlFootprint(oldMesh) + yamlFootprint(oldModel) +
        (oldSubEntitiesDeleteCmd ? oldSubEntitiesDeleteCmd->memoryBytes() : 0) +
        (createEntityCmd ? createEntityCmd->memoryBytes() : 0) +
        reusedEntities.capacity() * sizeof(Entity) + parkedEntities.capacity() * sizeof(ParkedEntity) +
        movedParts.capacity() * sizeof(MovedPart);
}
