// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's instance edits (editor/project/PREFABS.md, "Editing instances"): a value, name,
// state or component edit on a GameObject of a prefab instance is an override of that
// instance only. The prefab asset changes through Apply (ProjectPrefabOverrideModel.cpp) or
// Prefab Mode. Also: the Overrides dropdown listing, what Apply allows, and the Play mode
// snapshot of the instances' bookkeeping.

#include "Project.h"
#include "component/Transform.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor::prefabinternal;

namespace jasperengine::editor::prefabinternal {

    std::vector<std::string> diffComponentProperties(EntityRegistry* a, Entity aEntity, EntityRegistry* b, Entity bEntity, ComponentType type) {
        std::vector<std::string> changed;
        if (!hasComponent(a, aEntity, type) || !hasComponent(b, bEntity, type)) {
            return {PrefabObjectOverrides::WHOLE_COMPONENT};
        }
        const std::string bText = YAML::Dump(encodeComponent(b, bEntity, type));
        if (YAML::Dump(encodeComponent(a, aEntity, type)) == bText) {
            return changed;
        }
        for (auto& [name, bProperty] : Catalog::findEntityProperties(b, bEntity, type)) {
            const PropertyData aProperty = Catalog::findProperty(a, aEntity, type, name);
            if (!aProperty.ref) {
                changed.push_back(name);
                continue;
            }
            if (samePropertyValue(aProperty, bProperty)) {
                continue;
            }
            switch (bProperty.type) {
                case PropertyType::Bool: case PropertyType::String: case PropertyType::Float: case PropertyType::Double:
                case PropertyType::Vector2: case PropertyType::Vector3: case PropertyType::Vector4: case PropertyType::Quat:
                case PropertyType::Int: case PropertyType::UInt: case PropertyType::Enum: case PropertyType::Entity:
                case PropertyType::EntityReference: case PropertyType::Asset: case PropertyType::Shader: case PropertyType::Curve:
                case PropertyType::Gradient:
                    changed.push_back(name);
                    continue;
                default:
                    break;
            }
            // A value these cannot compare (a material, a texture, a list): b with a's value
            // of it is b again when it did not change
            ScratchEntity probe;
            probe.takeComponents(b, bEntity, Catalog::componentTypeToSignature(b, type));
            Catalog::copyPropertyValue(a, aEntity, probe.registry.get(), probe.entity, type, name);
            if (YAML::Dump(encodeComponent(probe.registry.get(), probe.entity, type)) != bText) {
                changed.push_back(name);
            }
        }
        if (changed.empty()) {
            // something no single property holds (an array's size, say)
            changed.push_back(PrefabObjectOverrides::WHOLE_COMPONENT);
        }
        return changed;
    }

}

bool editor::Project::isEditingPrefabAsset(uint32_t sceneId, Entity entity) const {
    // Prefab Mode (PREFABS.md section 16): a stage's own GameObjects are the asset's, so their
    // edits are asset edits. The GameObjects of the instances nested in it are not: their
    // edits are overrides, which the stage saves as the asset's overrides of them.
    return isPrefabStage(sceneId) && entity != NULL_ENTITY && findPrefabPathFor(sceneId, entity).empty();
}

bool editor::Project::recordPrefabEdit(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties) {
    if (entity == NULL_ENTITY || type == ComponentType::PrefabComponent || isEditingPrefabAsset(sceneId, entity)) {
        return false;
    }
    // (recordPropertyOverride leaves out a component the instance added, which is its own
    // anyway, and an instance root's default overrides)
    return recordPropertyOverride(sceneId, entity, type, properties);
}

bool editor::Project::recordPrefabComponentEdit(uint32_t sceneId, Entity entity, ComponentType type, const YAML::Node& before) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || isEditingPrefabAsset(sceneId, entity)) {
        return false;
    }
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    ScratchEntity old;
    old.decode(before);
    std::vector<std::string> changed = diffComponentProperties(old.registry.get(), old.entity, sceneProject->scene, entity, type);
    if (changed.empty()) {
        return false;
    }
    return recordPrefabEdit(sceneId, entity, type, changed);
}

bool editor::Project::recordPrefabStateEdit(uint32_t sceneId, Entity entity, uint8_t stateBits) {
    if (isEditingPrefabAsset(sceneId, entity)) {
        return false;
    }
    // (an instance root's name is a default override: recordStateOverride leaves it out)
    return recordStateOverride(sceneId, entity, stateBits);
}

bool editor::Project::recordPrefabComponentAdded(uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || type == ComponentType::PrefabComponent || isEditingPrefabAsset(sceneId, entity) ||
        !findPrefabObject(sceneId, entity, ref) || !hasComponent(sceneProject->scene, entity, type)) {
        return false;
    }
    if (hasComponent(ref.prefab->registry.get(), ref.registryEntity, type)) {
        // The prefab GameObject has one, which this instance had removed: it is back, with
        // values of its own
        Prefab::ObjectOverrides& overrides = ref.instance->overrides[entity];
        overrides.removedComponents.erase(type);
        overrides.addWholeComponent(type);
        sceneProject->isModified = true;
        return true;
    }
    return recordAddedComponent(sceneId, entity, type);
}

bool editor::Project::recordPrefabComponentRemoved(uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || type == ComponentType::PrefabComponent || isEditingPrefabAsset(sceneId, entity) ||
        !findPrefabObject(sceneId, entity, ref) || hasComponent(sceneProject->scene, entity, type)) {
        return false;
    }
    auto it = ref.instance->overrides.find(entity);
    if (it != ref.instance->overrides.end() && it->second.addedComponents.erase(type)) {
        // one the instance added: simply gone
        it->second.properties.erase(type);
        if (it->second.empty()) {
            ref.instance->overrides.erase(it);
        }
        sceneProject->isModified = true;
        return true;
    }
    return recordRemovedComponent(sceneId, entity, type);
}

bool editor::Project::getPrefabEditOverrides(uint32_t sceneId, Entity entity, Prefab::ObjectOverrides& overrides) const {
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return false;
    }
    const Prefab::ObjectOverrides* found = ref.instance->findOverrides(entity);
    overrides = found ? *found : Prefab::ObjectOverrides();
    return true;
}

void editor::Project::setPrefabEditOverrides(uint32_t sceneId, Entity entity, const Prefab::ObjectOverrides& overrides) {
    PrefabObjectRef ref;
    if (!findPrefabObject(sceneId, entity, ref)) {
        return;
    }
    if (overrides.empty()) {
        ref.instance->overrides.erase(entity);
    } else {
        ref.instance->overrides[entity] = overrides;
    }
}

bool editor::Project::canApplyPrefabOverrides(std::string* reason) const {
    if (isAnyScenePlaying()) {
        if (reason) {
            *reason = "Prefab overrides cannot be applied in Play mode";
        }
        return false;
    }
    return true;
}

bool editor::Project::isNestedRootApplyTarget(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target) const {
    if (target.empty() || target == ref.path) {
        return false;
    }
    // The prefabs the GameObject roots instances of: a nested root's prefab and its bases,
    // a variant instance root's bases (PREFABS.md 19)
    const std::vector<std::filesystem::path> levels = getInstanceRootLevels(sceneId, ref.localEntity);
    return std::find(levels.begin(), levels.end(), target) != levels.end();
}

bool editor::Project::canApplyPrefabOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties,
        const std::filesystem::path& target, std::string* reason) const {
    auto fail = [reason](const std::string& message) {
        if (reason) {
            *reason = message;
        }
        return false;
    };
    if (!canApplyPrefabOverrides(reason)) {
        return false;
    }
    const SceneProject* sceneProject = getScene(sceneId);
    PrefabObjectRef ref;
    if (!sceneProject || !sceneProject->scene || !findPrefabObject(sceneId, entity, ref)) {
        return fail("Not part of a Prefab instance");
    }
    const std::vector<std::filesystem::path> targets = getPrefabApplyTargets(sceneId, entity);
    if (!target.empty() && std::find(targets.begin(), targets.end(), target) == targets.end()) {
        return fail("Not a Prefab this override can go to");
    }
    if (type == ComponentType::PrefabComponent || !hasComponent(sceneProject->scene, entity, type)) {
        return true; // state, removed components: nothing to check
    }
    // A value referencing a GameObject outside the Prefab cannot go into the Prefab: the
    // GameObjects the target prefab holds (its instance, below the GameObject's own), the
    // ones of the instances nested in it included (stripped objects, section 17)
    const std::filesystem::path resolved = target.empty() ? ref.path : target;
    Project* self = const_cast<Project*>(this);
    const Entity holderRoot = isNestedRootApplyTarget(sceneId, ref, resolved) ? entity :
        self->findHolderInstanceRoot(sceneId, ref.instance->rootEntity, resolved, ref.path);
    const std::unordered_set<Entity> members = self->collectInstanceTreeObjects(sceneId, holderRoot, resolved);
    if (componentHasExternalEntityRef(sceneProject->scene, entity, type, members)) {
        return fail("It references a GameObject outside the Prefab, which the Prefab cannot hold");
    }
    (void)properties;
    return true;
}

std::vector<editor::PrefabOverrideEntry> editor::Project::getPrefabOverrideEntries(uint32_t sceneId, Entity instanceRoot) const {
    std::vector<PrefabOverrideEntry> entries;
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return entries;
    }
    Scene* scene = sceneProject->scene;

    // The overrides by GameObject: each owning instance holds its own GameObjects' (a nested
    // root's are the holding instance's, a variant root's the variant level's)
    std::map<Entity, const Prefab::ObjectOverrides*> objectOverrides;
    std::function<void(const Prefab::Instance*, const std::filesystem::path&, size_t)> collect =
            [&](const Prefab::Instance* instance, const std::filesystem::path& path, size_t depth) {
        if (!instance || depth > 64) {
            return;
        }
        for (const auto& [local, overrides] : instance->overrides) {
            // the root's own entry only at the top: a nested root's belong to the holder
            if (local == instance->rootEntity && depth > 0) {
                continue;
            }
            if (!overrides.empty()) {
                objectOverrides[local] = &overrides;
            }
        }
        for (const NestedInstanceLevel& nested : getNestedInstanceLevels(sceneProject, path, *instance)) {
            collect(nested.instance, nested.path, depth + 1);
        }
    };
    {
        std::filesystem::path rootPath;
        const Prefab::Instance* top = findInstanceAt(sceneProject, instanceRoot, std::filesystem::path(), &rootPath);
        collect(top, rootPath, 0);
    }

    // Added GameObjects by the GameObject they hang from
    std::multimap<Entity, Entity> addedBelow;
    for (const auto& [entity, parent] : getPrefabAddedGameObjects(sceneId, instanceRoot)) {
        addedBelow.emplace(parent, entity);
    }

    // Hierarchy order: the root's branch in Transform order (members without a Transform last)
    std::vector<Entity> order;
    auto transforms = scene->getComponentArray<Transform>();
    if (scene->findComponent<Transform>(instanceRoot)) {
        const size_t first = transforms->getIndex(instanceRoot);
        const size_t last = scene->findBranchLastIndex(instanceRoot);
        for (size_t i = first; i <= last && i < transforms->size(); i++) {
            order.push_back(transforms->getEntity(i));
        }
    } else {
        order.push_back(instanceRoot);
    }
    for (const auto& [entity, overrides] : objectOverrides) {
        if (std::find(order.begin(), order.end(), entity) == order.end()) {
            order.push_back(entity);
        }
    }

    for (Entity entity : order) {
        auto found = objectOverrides.find(entity);
        if (found != objectOverrides.end()) {
            const Prefab::ObjectOverrides& overrides = *found->second;
            if (overrides.state) {
                PrefabOverrideEntry entry;
                entry.kind = PrefabOverrideEntry::Kind::State;
                entry.entity = entity;
                entry.type = ComponentType::PrefabComponent;
                entry.state = overrides.state;
                entries.push_back(entry);
            }
            // components in Inspector order
            std::set<ComponentType> types;
            for (const auto& [type, names] : overrides.properties) types.insert(type);
            types.insert(overrides.addedComponents.begin(), overrides.addedComponents.end());
            types.insert(overrides.removedComponents.begin(), overrides.removedComponents.end());
            for (ComponentType type : types) {
                PrefabOverrideEntry entry;
                entry.entity = entity;
                entry.type = type;
                if (overrides.addedComponents.count(type)) {
                    entry.kind = PrefabOverrideEntry::Kind::AddedComponent;
                } else if (overrides.removedComponents.count(type)) {
                    entry.kind = PrefabOverrideEntry::Kind::RemovedComponent;
                } else {
                    entry.kind = PrefabOverrideEntry::Kind::Property;
                    entry.properties = overrides.properties.at(type);
                }
                entries.push_back(entry);
            }
        }
        auto range = addedBelow.equal_range(entity);
        for (auto it = range.first; it != range.second; ++it) {
            PrefabOverrideEntry entry;
            entry.kind = PrefabOverrideEntry::Kind::AddedGameObject;
            entry.entity = it->second;
            entry.parent = entity;
            entries.push_back(entry);
        }
    }

    for (const PrefabRemovalInfo& removal : getPrefabRemovals(sceneId, instanceRoot)) {
        PrefabOverrideEntry entry;
        entry.kind = PrefabOverrideEntry::Kind::RemovedGameObject;
        entry.entity = NULL_ENTITY;
        entry.removal = removal;
        entries.push_back(entry);
    }
    return entries;
}

editor::PlayPrefabState editor::Project::capturePlayPrefabState(const SceneProject* sceneProject) const {
    PlayPrefabState state;
    if (!sceneProject) {
        return state;
    }
    for (const auto& [path, prefab] : prefabAssets) {
        auto it = prefab.instances.find(sceneProject->id);
        if (it != prefab.instances.end()) {
            state.instances[path] = it->second;
        }
    }
    state.missingPrefabs = sceneProject->missingPrefabs;
    state.valid = true;
    return state;
}

void editor::Project::restorePlayPrefabState(SceneProject* sceneProject) {
    if (!sceneProject || !sceneProject->playPrefabState.valid) {
        return;
    }
    PlayPrefabState& state = sceneProject->playPrefabState;
    // The entities are back with their ids (the play snapshot), so is what describes them
    for (auto& [path, prefab] : prefabAssets) {
        auto saved = state.instances.find(path);
        if (saved != state.instances.end()) {
            prefab.instances[sceneProject->id] = std::move(saved->second);
        } else {
            prefab.instances.erase(sceneProject->id);
        }
    }
    sceneProject->missingPrefabs = std::move(state.missingPrefabs);
    sceneProject->playPrefabState = PlayPrefabState();
    updateScenePrefabs(sceneProject);
    sceneProject->structureVersion++;
}
