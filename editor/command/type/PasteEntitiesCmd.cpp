// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PasteEntitiesCmd.h"
#include "component/Transform.h"
#include "component/PrefabComponent.h"

#include "Stream.h"
#include "Out.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/BackgroundRelease.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {

    template<typename Fn>
    void forEachEntityNode(YAML::Node node, Fn&& fn);

    // The GameObjects added to a prefab instance, at every nested level of its block
    template<typename Fn>
    void forEachAddedNode(YAML::Node block, Fn&& fn) {
        if (!block || !block.IsMap()) {
            return;
        }
        if (YAML::Node added = block["addedGameObjects"]; added && added.IsSequence()) {
            for (YAML::Node node : added) {
                forEachEntityNode(node, fn);
            }
        }
        if (YAML::Node nested = block["nested"]; nested && nested.IsSequence()) {
            for (YAML::Node entry : nested) {
                forEachAddedNode(entry, fn);
            }
        }
    }

    // Calls fn(node) for every entity node the decoder creates from `node`: the node itself,
    // its children and the scene-local entities of a prefab instance
    template<typename Fn>
    void forEachEntityNode(YAML::Node node, Fn&& fn) {
        if (!node || !node.IsMap()) {
            return;
        }
        if (node["entity"]) {
            fn(node);
        }
        if (YAML::Node children = node["children"]; children && children.IsSequence()) {
            for (YAML::Node child : children) {
                forEachEntityNode(child, fn);
            }
        }
        if (YAML::Node locals = node["prefabLocalEntities"]; locals && locals.IsSequence()) {
            for (YAML::Node local : locals) {
                forEachEntityNode(local, fn);
            }
        }
        forEachAddedNode(node["prefabInstance"], fn);
    }

    // Last index of the branch of `entity` in the Transform array. Linear, unlike
    // EntityRegistry::findBranchLastIndex which rescans its list for every descendant.
    size_t branchLastIndex(Scene* scene, Entity entity) {
        auto transforms = scene->getComponentArray<Transform>();
        size_t index = transforms->getIndex(entity);
        std::unordered_set<Entity> branch{entity};
        while (index + 1 < transforms->size() && branch.count(transforms->getComponentFromIndex(index + 1).parent)) {
            index++;
            branch.insert(transforms->getEntity(index));
        }
        return index;
    }

}

editor::PasteEntitiesCmd::PasteEntitiesCmd(Project* project, uint32_t sceneId, std::shared_ptr<const GameObjectsClip> clip,
                                           EntityClipboard::Placement placement, Entity anchor){
    this->project = project;
    this->sceneId = sceneId;
    this->clip = std::move(clip);
    this->placement = placement;
    this->anchor = anchor;
}

editor::PasteEntitiesCmd::~PasteEntitiesCmd(){
    // The last holder of a large copy frees it on the background thread
    BackgroundRelease::release(std::move(clip));
}

bool editor::PasteEntitiesCmd::hasLinkSource(Scene* scene) const{
    for (Entity entity : createdEntities) {
        if (CameraTextureLink::isLinkSource(scene, entity)) {
            return true;
        }
    }
    return false;
}

void editor::PasteEntitiesCmd::destroyCreated(SceneProject* sceneProject){
    Scene* scene = sceneProject->scene;

    // A component array erases by shifting everything behind the entity, so a pasted block
    // in the middle of the scene is first made roots at the end of it: every destroy below
    // then drops the last elements, instead of moving the rest of the scene once per entity
    std::unordered_set<Entity> created(createdEntities.begin(), createdEntities.end());
    auto transforms = scene->getComponentArray<Transform>();
    size_t first = transforms->size();
    size_t last = 0;
    size_t count = 0;
    Signature blockSignature;
    for (size_t i = 0; i < transforms->size(); i++) {
        Entity entity = transforms->getEntity(i);
        if (created.count(entity)) {
            first = std::min(first, i);
            last = i;
            count++;
            blockSignature |= scene->getSignature(entity);
        }
    }
    if (count > 0 && last - first + 1 == count && last + 1 < transforms->size()) {
        for (size_t i = first; i <= last; i++) {
            Transform& transform = transforms->getComponentFromIndex(i);
            if (!created.count(transform.parent)) {
                transform.parent = NULL_ENTITY;
            }
        }
        transforms->moveEntityRangeToIndex(transforms->getEntity(first), transforms->getEntity(last), transforms->size() - count);
        scene->sortComponentsByTransform(blockSignature);
    }

    // Prefab instances first, through the prefab bookkeeping. An instance nested in another
    // pasted one goes with the outer instance.
    std::unordered_set<Entity> nestedRoots;
    std::vector<std::pair<std::filesystem::path, Entity>> instanceRoots;
    for (Entity entity : createdEntities) {
        if (!scene->isEntityCreated(entity)) {
            continue;
        }
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity);
        if (!component || component->path.empty()) {
            continue;
        }
        const Prefab* prefab = project->getPrefab(component->path);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, entity) : nullptr;
        if (!instance || instance->rootEntity != entity) {
            continue;
        }
        instanceRoots.emplace_back(component->path, entity);
        for (const Prefab::EntityMember& member : instance->members) {
            nestedRoots.insert(member.localEntity);
        }
    }
    for (auto it = instanceRoots.rbegin(); it != instanceRoots.rend(); ++it) {
        const auto& [path, root] = *it;
        if (nestedRoots.count(root) || !scene->isEntityCreated(root)) {
            continue;
        }
        const Prefab* prefab = project->getPrefab(path);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
        std::vector<Entity> members;
        if (instance) {
            members.reserve(instance->members.size());
            for (const Prefab::EntityMember& member : instance->members) {
                members.push_back(member.localEntity);
            }
        }
        project->unimportPrefab(sceneId, path, root, members, false);
    }

    std::vector<Entity> remaining;
    remaining.reserve(createdEntities.size());
    for (auto it = createdEntities.rbegin(); it != createdEntities.rend(); ++it) {
        if (scene->isEntityCreated(*it)) {
            remaining.push_back(*it);
        }
    }
    // One pass over the tracked list for the instances and the rest
    DeleteEntityCmd::destroyEntities(scene, remaining, sceneProject->entities, project, sceneId, false);
    ProjectUtils::reconcileTrackedEntities(scene, sceneProject->entities, project, sceneId);
}

bool editor::PasteEntitiesCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !clip || !clip->roots.IsSequence() || clip->roots.size() == 0){
        return false;
    }
    Scene* scene = sceneProject->scene;
    auto transforms = scene->getComponentArray<Transform>();

    EntityClipboard::Placement where = placement;
    if (where != EntityClipboard::Placement::Root &&
        (anchor == NULL_ENTITY || !scene->isEntityCreated(anchor) || !scene->findComponent<Transform>(anchor))) {
        if (where == EntityClipboard::Placement::Into) {
            return false;
        }
        where = EntityClipboard::Placement::Root;
    }

    const bool firstRun = firstIds.empty();
    wasModified = sceneProject->isModified;
    lastSelected = project->getSelectedEntities(sceneId);

    // Decoded at the scene root: every new Transform is appended, so they form one block
    // at the end of the Transform array, each subtree contiguous and in copy order
    const size_t tailStart = transforms->size();
    const size_t trackedBefore = sceneProject->entities.size();

    createdEntities.clear();
    createdRoots.clear();
    std::unordered_map<Entity, Entity> decodeRemap;

    // A redo decodes the copy with the ids of the first paste, which the undo freed, so
    // it gets them back. The copy is shared, so the ids are swapped in only for the decode.
    struct IdSwap {
        std::vector<std::pair<YAML::Node, Entity>> nodes; // node, its own id
        void restore() {
            for (auto& [node, id] : nodes) {
                node["entity"] = id;
            }
            nodes.clear();
        }
        ~IdSwap() { restore(); }
    } idSwap;
    if (!firstRun) {
        for (YAML::Node rootNode : clip->roots) {
            forEachEntityNode(rootNode, [&](YAML::Node node) {
                const Entity copied = node["entity"].as<Entity>();
                auto first = firstIds.find(copied);
                if (first != firstIds.end() && first->second != copied) {
                    node["entity"] = first->second;
                    idSwap.nodes.emplace_back(node, copied);
                }
            });
        }
    }

    try {
        for (const YAML::Node& rootNode : clip->roots) {
            std::vector<Entity> made = Stream::decodeEntity(rootNode, scene, &sceneProject->entities, project, sceneProject,
                NULL_ENTITY, true, false, &decodeRemap);
            if (made.empty()) {
                continue;
            }
            createdRoots.push_back(made[0]);
            createdEntities.insert(createdEntities.end(), made.begin(), made.end());
        }
    } catch (const std::exception& e) {
        Out::error("Paste failed: %s", e.what());
        // Everything decoded so far is tracked past the old end of the list
        createdEntities.assign(sceneProject->entities.begin() + std::min(trackedBefore, sceneProject->entities.size()), sceneProject->entities.end());
        destroyCreated(sceneProject);
        createdEntities.clear();
        createdRoots.clear();
        return false;
    }
    // Back to the copied ids before they are read below
    idSwap.restore();
    if (createdEntities.empty()){
        return false;
    }

    // Copied entity -> pasted entity. recreateEntity keeps an id that is free, so an entity
    // missing from decodeRemap kept its id: a paste after Cut restores the originals' ids.
    std::unordered_map<Entity, Entity> idMap;
    std::vector<Entity> copiedIds;
    for (const YAML::Node& rootNode : clip->roots) {
        forEachEntityNode(rootNode, [&](const YAML::Node& node) {
            copiedIds.push_back(node["entity"].as<Entity>());
        });
    }
    idMap.reserve(copiedIds.size());
    for (Entity copied : copiedIds) {
        Entity decoded = firstRun ? copied : firstIds[copied];
        auto remapped = decodeRemap.find(decoded);
        Entity pasted = remapped != decodeRemap.end() ? remapped->second : decoded;
        idMap[copied] = pasted;
        if (firstRun) {
            firstIds[copied] = pasted;
        }
    }

    // Prefab members come from the prefab, not the copy: match them by registry entity.
    // Outer instances come first, so a nested root is mapped before its own members.
    for (const ClipboardPrefabInstance& copiedInstance : clip->prefabInstances) {
        auto root = idMap.find(copiedInstance.root);
        if (root == idMap.end()) {
            continue;
        }
        const Prefab* prefab = project->getPrefab(copiedInstance.path);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root->second) : nullptr;
        if (!instance || instance->rootEntity != root->second) {
            continue;
        }
        std::unordered_map<Entity, Entity> registryToLocal;
        for (const Prefab::EntityMember& member : instance->members) {
            registryToLocal[member.registryEntity] = member.localEntity;
        }
        for (const auto& [copiedLocal, registryEntity] : copiedInstance.members) {
            auto local = registryToLocal.find(registryEntity);
            if (local != registryToLocal.end()) {
                idMap[copiedLocal] = local->second;
            }
        }
    }

    // Repoint entity references. A prefab member holds copied values only in its overridden
    // components; the rest came from the prefab, already in this scene's ids.
    std::unordered_map<Entity, Prefab::ObjectOverrides> memberOverrides;
    for (Entity entity : createdEntities) {
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity);
        if (!component || component->path.empty()) {
            continue;
        }
        // every level rooted there (an instance of a Prefab Variant roots its bases' too)
        for (const PrefabInstanceLevel& level : project->getInstanceLevels(project->getScene(sceneId), entity)) {
            const Prefab::Instance* instance = level.instance;
            for (const Prefab::EntityMember& member : instance->members) {
                auto overrides = instance->overrides.find(member.localEntity);
                memberOverrides[member.localEntity] = overrides != instance->overrides.end() ? overrides->second : Prefab::ObjectOverrides();
            }
        }
    }

    ClipboardRemap remap;
    remap.map = &idMap;
    remap.externalNames = &clip->externalNames;
    remap.sourceSceneId = clip->sceneId;
    remap.targetSceneId = sceneId;
    remap.sameScene = clip->project == EntityClipboard::projectKey(project) && clip->sceneId == sceneId;

    for (Entity entity : createdEntities) {
        auto member = memberOverrides.find(entity);
        uint64_t updateFlags = 0;
        for (ComponentType type : Catalog::findComponents(scene, entity)) {
            if (member != memberOverrides.end() && !member->second.hasComponent(type) && !member->second.addedComponents.count(type)) {
                continue;
            }
            updateFlags |= EntityClipboard::remapReferences(scene, entity, type, remap);
        }
        if (updateFlags != 0) {
            Catalog::updateEntity(scene, entity, updateFlags);
        }
    }

    // Place the block: parent the top level of it, then move it after the anchor's branch
    // in one step instead of one reparent (and component re-sort) per GameObject
    const size_t tailEnd = transforms->size();
    if (tailEnd > tailStart) {
        Entity parent = NULL_ENTITY;
        size_t destination = tailStart;
        if (where == EntityClipboard::Placement::Into) {
            parent = anchor;
            destination = branchLastIndex(scene, anchor) + 1;
        } else if (where == EntityClipboard::Placement::After) {
            parent = scene->getComponent<Transform>(anchor).parent;
            destination = branchLastIndex(scene, anchor) + 1;
        }

        Signature blockSignature;
        bool reparented = false;
        for (size_t i = tailStart; i < tailEnd; i++) {
            Transform& transform = transforms->getComponentFromIndex(i);
            if (transform.parent == NULL_ENTITY && parent != NULL_ENTITY) {
                transform.parent = parent;
                transform.needUpdate = true;
                reparented = true;
            }
            blockSignature |= scene->getSignature(transforms->getEntity(i));
        }
        // parents written in place: Animation Players rebind (the sort below bumps it too, but
        // a block already in place is not sorted)
        if (reparented) {
            scene->hierarchyChanged();
        }

        if (destination < tailStart) {
            transforms->moveEntityRangeToIndex(transforms->getEntity(tailStart), transforms->getEntity(tailEnd - 1), destination);
            scene->sortComponentsByTransform(blockSignature);

            // Tracked list back in Transform order, the GameObjects without one where they
            // were. Appended at the end it already was.
            ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
        }
    }

    // Created or moved, each needs its update (but not a reload of the whole scene)
    for (Entity entity : createdEntities) {
        Catalog::updateEntity(scene, entity, ~UpdateFlags_SceneWide);
    }

    // The pasted textures were bound with their references. A pasted camera or fluid may also
    // be what textures elsewhere name (it got back the id it had before a Cut): rebind all.
    if (hasLinkSource(scene)) {
        CameraTextureLink::resolve(scene);
    }

    // The pasted GameObjects become the selection, not the actions that came with them
    const size_t selected = std::min(clip->selectedRoots, createdRoots.size());
    project->replaceSelectedEntities(sceneId, std::vector<Entity>(createdRoots.begin(), createdRoots.begin() + selected));

    sceneProject->isModified = true;

    if (firstRun) {
        Out::info("Pasted %zu GameObject%s", selected, selected == 1 ? "" : "s");
    }

    return true;
}

void editor::PasteEntitiesCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return;
    }

    // Textures elsewhere may name a camera or a fluid about to go
    const bool camera = hasLinkSource(sceneProject->scene);
    destroyCreated(sceneProject);
    createdEntities.clear();
    createdRoots.clear();

    if (camera) {
        CameraTextureLink::resolve(sceneProject->scene);
    }

    std::vector<Entity> selection;
    for (Entity entity : lastSelected) {
        if (sceneProject->scene->isEntityCreated(entity)) {
            selection.push_back(entity);
        }
    }
    project->replaceSelectedEntities(sceneId, selection);

    sceneProject->isModified = wasModified;
}
