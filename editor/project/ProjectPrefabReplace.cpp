// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Replace Prefab Asset (an instance swapped to another prefab, its overrides carried
// over by name or hierarchy path), instances built for previews (the Project window's prefab
// thumbnails) and the prefab usage scan of the Prefabs dialog. See editor/project/PREFABS.md,
// sections 12 to 14.

#include "Project.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor::prefabinternal;

namespace {

    using editor::Prefab;
    using editor::PrefabInstanceData;
    using editor::PrefabObjectOverrides;
    using editor::PrefabReplaceSettings;

    // The GameObjects of a prefab by a key Replace Prefab matches between two prefabs: the
    // root is "", a GameObject below it its parent's key + "/" + its segment. ByName: the
    // name, "#k" added for the k-th earlier sibling of the same name; ByHierarchy: "#" + its
    // sibling index. GameObjects without a Transform next to the root (actions) get "~" keys.
    struct PrefabKeys {
        std::map<Entity, std::string> keyOf;
        std::map<std::string, Entity> entityOf;
    };

    PrefabKeys buildPrefabKeys(const Prefab& prefab, PrefabReplaceSettings::Match match) {
        PrefabKeys keys;
        if (!prefab.registry || prefab.root == NULL_ENTITY) {
            return keys;
        }
        auto add = [&keys](Entity entity, const std::string& key) {
            if (keys.entityOf.count(key) == 0) {
                keys.keyOf[entity] = key;
                keys.entityOf[key] = entity;
            }
        };
        add(prefab.root, "");
        if (match == PrefabReplaceSettings::Match::None) {
            return keys;
        }

        EntityRegistry* registry = prefab.registry.get();
        std::map<Entity, std::vector<Entity>> children;  // in Transform (hierarchy) order
        std::unordered_set<Entity> withTransform;
        auto transforms = registry->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            withTransform.insert(entity);
            children[transforms->getComponentFromIndex(i).parent].push_back(entity);
        }
        std::vector<Entity> loose;  // top level without a Transform
        for (Entity entity : prefab.registryEntities) {
            if (!withTransform.count(entity) && entity != prefab.root) {
                loose.push_back(entity);
            }
        }

        auto segments = [&](const std::vector<Entity>& siblings) {
            std::vector<std::string> result;
            std::map<std::string, int> seen;
            for (size_t i = 0; i < siblings.size(); i++) {
                if (match == PrefabReplaceSettings::Match::ByHierarchy) {
                    result.push_back("#" + std::to_string(i));
                } else {
                    const std::string name = registry->getEntityName(siblings[i]);
                    const int count = seen[name]++;
                    result.push_back(count == 0 ? name : name + "#" + std::to_string(count));
                }
            }
            return result;
        };

        std::function<void(Entity, const std::string&, size_t)> visit = [&](Entity entity, const std::string& key, size_t depth) {
            add(entity, key);
            auto it = children.find(entity);
            if (it == children.end() || depth > 256) {
                return;
            }
            const std::vector<std::string> names = segments(it->second);
            for (size_t i = 0; i < it->second.size(); i++) {
                visit(it->second[i], key + "/" + names[i], depth + 1);
            }
        };
        visit(prefab.root, "", 0);
        const std::vector<std::string> looseNames = segments(loose);
        for (size_t i = 0; i < loose.size(); i++) {
            visit(loose[i], "~" + looseNames[i], 0);
        }
        return keys;
    }

    std::string componentNames(const std::set<editor::ComponentType>& types) {
        std::string text;
        for (editor::ComponentType type : types) {
            text += (text.empty() ? "" : ", ") + editor::Catalog::getComponentName(type, true);
        }
        return text;
    }

    struct ReplaceContext {
        editor::Project* project = nullptr;
        PrefabReplaceSettings settings;
        editor::PrefabReplaceReport* report = nullptr;
        editor::PrefabMemberIds* memberIds = nullptr;

        void drop(const std::string& text) const {
            if (report) {
                report->dropped.push_back(text);
            }
        }
        void keep() const {
            if (report) {
                report->kept++;
            }
        }
    };

    std::filesystem::path resolvedPath(editor::Project* project, const PrefabComponent& component) {
        return project->resolvePrefabPath(component.guid, component.path);
    }

    bool sameAsset(editor::Project* project, const PrefabComponent& a, const PrefabComponent& b) {
        if (!a.guid.empty() && !b.guid.empty()) {
            return a.guid == b.guid;
        }
        return resolvedPath(project, a) == resolvedPath(project, b);
    }

    // The registry entity of the root in the data of an instance whose prefab is gone (a
    // Missing Prefab): the one overridden object that is not a member, else the one without
    // values (a root's values are its own node's)
    Entity guessDataRoot(const PrefabInstanceData& data) {
        std::vector<Entity> candidates;
        for (const auto& [registryEntity, object] : data.objects) {
            if (!data.memberIds.count(registryEntity)) {
                candidates.push_back(registryEntity);
            }
        }
        if (candidates.size() == 1 && !data.memberIds.empty()) {
            return candidates.front();
        }
        for (const auto& [registryEntity, object] : data.objects) {
            if (!object.values || object.values.IsNull()) {
                return registryEntity;
            }
        }
        return NULL_ENTITY;
    }

    // One level of instance data (keyed by oldPrefab's registry entities) carried over to
    // newPrefab. oldPrefab null: the old asset is gone, only the root matches. localRoot: the
    // level's root in the scene, whose member ids (ctx.memberIds[localRoot]) follow the keys.
    PrefabInstanceData remapInstanceLevel(const ReplaceContext& ctx, const Prefab* oldPrefab, const Prefab& newPrefab,
            const PrefabInstanceData& in, Entity localRoot, const std::string& where, size_t depth) {
        PrefabInstanceData out;
        out.present = true;
        out.legacy = in.legacy;
        out.nestedRoot = in.nestedRoot;
        out.nestedPath = in.nestedPath;
        if (depth > 32 || !newPrefab.registry) {
            return out;
        }
        EntityRegistry* newRegistry = newPrefab.registry.get();

        const PrefabKeys oldKeys = oldPrefab ? buildPrefabKeys(*oldPrefab, ctx.settings.match) : PrefabKeys();
        const PrefabKeys newKeys = buildPrefabKeys(newPrefab, ctx.settings.match);
        const Entity oldRoot = oldPrefab ? oldPrefab->root : guessDataRoot(in);

        auto map = [&](Entity oldEntity) -> Entity {
            if (oldEntity == NULL_ENTITY) {
                return NULL_ENTITY;
            }
            if (oldEntity == oldRoot) {
                return newPrefab.root;
            }
            if (!oldPrefab) {
                return NULL_ENTITY;
            }
            auto key = oldKeys.keyOf.find(oldEntity);
            if (key == oldKeys.keyOf.end()) {
                return NULL_ENTITY;
            }
            auto target = newKeys.entityOf.find(key->second);
            return target != newKeys.entityOf.end() ? target->second : NULL_ENTITY;
        };
        auto oldName = [&](Entity oldEntity) -> std::string {
            if (oldPrefab && oldPrefab->registry && oldPrefab->registry->isEntityCreated(oldEntity)) {
                return oldPrefab->registry->getEntityName(oldEntity);
            }
            return "GameObject " + std::to_string(oldEntity);
        };

        // The ids before this level's keys change: the nested levels look theirs up by them
        std::map<Entity, Entity> levelIds = in.memberIds;
        if (ctx.memberIds && localRoot != NULL_ENTITY) {
            auto it = ctx.memberIds->find(localRoot);
            if (it != ctx.memberIds->end()) {
                for (const auto& [registryEntity, local] : it->second) {
                    levelIds[registryEntity] = local;
                }
            }
        }

        // Overrides: kept on the matching GameObject for the components it has
        for (const auto& [registryEntity, object] : in.objects) {
            const Entity target = map(registryEntity);
            if (target == NULL_ENTITY) {
                ctx.drop("overrides of '" + oldName(registryEntity) + "'" + where + ": no matching GameObject");
                continue;
            }
            if (!ctx.settings.keepOverrides) {
                ctx.drop("overrides of '" + oldName(registryEntity) + "'" + where + " (cleared)");
                continue;
            }
            const PrefabObjectOverrides& overrides = object.overrides;
            PrefabInstanceData::Object kept;
            kept.values = object.values ? YAML::Clone(object.values) : YAML::Node();
            kept.overrides.state = overrides.state;
            kept.overrides.addedComponents = overrides.addedComponents;
            std::set<editor::ComponentType> lost;
            for (const auto& [type, properties] : overrides.properties) {
                if (hasComponent(newRegistry, target, type) || overrides.addedComponents.count(type)) {
                    kept.overrides.properties[type] = properties;
                } else {
                    lost.insert(type);
                }
            }
            for (editor::ComponentType type : overrides.removedComponents) {
                if (hasComponent(newRegistry, target, type)) {
                    kept.overrides.removedComponents.insert(type);
                } else {
                    lost.insert(type);
                }
            }
            if (!lost.empty()) {
                ctx.drop("overrides of " + componentNames(lost) + " on '" + oldName(registryEntity) + "'" + where +
                    ": the new GameObject has no such component");
            }
            if (!kept.overrides.empty()) {
                out.objects[target] = std::move(kept);
                ctx.keep();
            }
        }

        // Removed GameObjects
        for (Entity removed : in.removedGameObjects) {
            const Entity target = map(removed);
            if (target == NULL_ENTITY || target == newPrefab.root) {
                ctx.drop("removal of '" + oldName(removed) + "'" + where + ": no matching GameObject");
            } else if (!ctx.settings.keepOverrides) {
                ctx.drop("removal of '" + oldName(removed) + "'" + where + " (cleared)");
            } else {
                out.removedGameObjects.insert(target);
                ctx.keep();
            }
        }

        // Added GameObjects: below the matching parent, else below the root
        for (const YAML::Node& added : in.addedGameObjects) {
            const std::string name = added["name"] ? added["name"].as<std::string>() : std::string("GameObject");
            if (!ctx.settings.keepOverrides) {
                ctx.drop("added GameObject '" + name + "'" + where + " (cleared)");
                continue;
            }
            Entity parent = added["parentRegistryEntity"] ? added["parentRegistryEntity"].as<Entity>() : NULL_ENTITY;
            Entity target = parent == NULL_ENTITY ? newPrefab.root : map(parent);
            YAML::Node node = YAML::Clone(added);
            if (target == NULL_ENTITY || out.removedGameObjects.count(target)) {
                target = newPrefab.root;
                node.remove("childIndex");
                editor::Out::warning("Replace Prefab: added GameObject '%s'%s now goes below the root, its parent '%s' is not in the new prefab",
                    name.c_str(), where.c_str(), oldName(parent).c_str());
            }
            node["parentRegistryEntity"] = target;
            out.addedGameObjects.push_back(node);
            ctx.keep();
        }

        // Nested instances: the same prefab keeps its data as it is, another one is matched
        // the same way one level down
        for (const PrefabInstanceData& nested : in.nested) {
            if (!ctx.settings.keepOverrides) {
                if (!nested.empty()) {
                    ctx.drop("overrides of the nested prefab '" + oldName(nested.nestedRoot) + "'" + where + " (cleared)");
                }
                continue;
            }
            if (nested.nestedRoot == NULL_ENTITY) {
                // written before registry ids: matched by path when imported
                out.nested.push_back(nested);
                continue;
            }
            const Entity target = map(nested.nestedRoot);
            if (target == NULL_ENTITY || !newPrefab.isNestedRoot(target)) {
                ctx.drop("data of the nested prefab '" + oldName(nested.nestedRoot) + "'" + where + ": no matching nested prefab");
                continue;
            }
            const PrefabComponent& newComponent = newRegistry->getComponent<PrefabComponent>(target);
            const PrefabComponent* oldComponent = oldPrefab && oldPrefab->registry && oldPrefab->registry->isEntityCreated(nested.nestedRoot)
                ? oldPrefab->registry->findComponent<PrefabComponent>(nested.nestedRoot) : nullptr;
            auto local = levelIds.find(nested.nestedRoot);
            const Entity nestedLocal = local != levelIds.end() ? local->second : NULL_ENTITY;

            PrefabInstanceData carried;
            if (oldComponent && sameAsset(ctx.project, *oldComponent, newComponent)) {
                carried = nested;
            } else {
                const Prefab* oldNested = oldComponent ? ctx.project->loadPrefabAsset(resolvedPath(ctx.project, *oldComponent)) : nullptr;
                const Prefab* newNested = ctx.project->loadPrefabAsset(resolvedPath(ctx.project, newComponent));
                if (!newNested) {
                    ctx.drop("data of the nested prefab '" + oldName(nested.nestedRoot) + "'" + where + ": its prefab cannot be read");
                    continue;
                }
                carried = remapInstanceLevel(ctx, oldNested, *newNested, nested, nestedLocal,
                    where + " > " + newRegistry->getEntityName(target), depth + 1);
            }
            carried.nestedRoot = target;
            carried.nestedPath = newComponent.path;
            if (!carried.empty()) {
                ctx.keep();
            }
            out.nested.push_back(std::move(carried));
        }

        // Member ids follow the keys: references from the scene to matched GameObjects stay
        for (const auto& [registryEntity, local] : in.memberIds) {
            const Entity target = map(registryEntity);
            if (target != NULL_ENTITY && target != newPrefab.root) {
                out.memberIds[target] = local;
            }
        }
        if (ctx.memberIds && localRoot != NULL_ENTITY) {
            auto it = ctx.memberIds->find(localRoot);
            if (it != ctx.memberIds->end()) {
                std::unordered_map<Entity, Entity> remapped;
                for (const auto& [registryEntity, local] : it->second) {
                    const Entity target = map(registryEntity);
                    if (target != NULL_ENTITY && target != newPrefab.root) {
                        remapped[target] = local;
                    }
                }
                it->second = std::move(remapped);
            }
        }
        return out;
    }

    // "Crate" and "Crate (2)" are named after the prefab "Crate"
    bool namedAfter(const std::string& name, const std::string& assetName) {
        if (name == assetName) {
            return true;
        }
        if (name.size() <= assetName.size() + 3 || name.compare(0, assetName.size(), assetName) != 0) {
            return false;
        }
        static const std::regex suffix(R"( \(\d+\)$)");
        return std::regex_match(name.substr(assetName.size()), suffix);
    }

    // The keys of an instance root node that are its instance data
    void removeInstanceKeys(YAML::Node& node) {
        std::vector<std::string> keys;
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            if (!editor::Stream::isGameObjectKey(key)) {
                keys.push_back(key);
            }
        }
        for (const std::string& key : keys) {
            node.remove(key);
        }
    }

}

// ---- Replace Prefab ----

bool editor::Project::canReplacePrefab(uint32_t sceneId, Entity entity) const {
    if (isAnyScenePlaying() || entity == NULL_ENTITY) {
        return false;
    }
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity) ||
            !sceneProject->scene->findComponent<PrefabComponent>(entity)) {
        return false;
    }
    PrefabObjectRef ref;
    if (findPrefabObject(sceneId, entity, ref)) {
        return ref.isRoot && ref.localEntity == entity;
    }
    return isMissingPrefab(sceneId, entity);
}

bool editor::Project::capturePrefabInstanceState(uint32_t sceneId, Entity root, YAML::Node& node, PrefabMemberIds& memberIds) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(root)) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    const PrefabComponent* component = scene->findComponent<PrefabComponent>(root);
    if (!component) {
        return false;
    }
    memberIds.clear();
    Prefab* prefab = getPrefab(component->path);
    const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
    if (instance && instance->rootEntity == root) {
        collectPrefabMemberIds(sceneId, *instance, memberIds);
    } else if (const MissingPrefabInstance* missing = getMissingPrefab(sceneId, root)) {
        memberIds = missing->memberIds;
    } else {
        return false;
    }
    node = Stream::encodeEntity(root, scene, this, sceneProject);
    return node && node.IsMap();
}

bool editor::Project::buildReplacedPrefabNode(uint32_t sceneId, Entity root, const YAML::Node& node, const PrefabMemberIds& memberIds,
        const std::filesystem::path& newPath, const PrefabReplaceSettings& settings,
        YAML::Node& outNode, PrefabMemberIds& outMemberIds, PrefabReplaceReport* report) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !node || !node.IsMap()) {
        return false;
    }
    Prefab* newPrefab = loadPrefabAsset(newPath);
    if (!newPrefab || !newPrefab->registry || newPrefab->root == NULL_ENTITY) {
        Out::error("Replace Prefab: cannot read prefab '%s'", newPath.string().c_str());
        return false;
    }
    const std::string prefabKey = componentKey(ComponentType::PrefabComponent);
    const YAML::Node components = node["components"];
    if (!components || !components[prefabKey]) {
        return false;
    }
    const PrefabComponent oldComponent = Stream::decodePrefabComponent(components[prefabKey]);
    std::filesystem::path oldPath = resolvePrefabPath(oldComponent.guid, oldComponent.path);
    const Prefab* oldPrefab = oldPath.empty() ? nullptr : loadPrefabAsset(oldPath);

    ReplaceContext ctx;
    ctx.project = this;
    ctx.settings = settings;
    ctx.report = report;
    outMemberIds = memberIds;
    ctx.memberIds = &outMemberIds;

    const PrefabInstanceData data = Stream::decodePrefabInstanceData(node);
    PrefabInstanceData replaced = remapInstanceLevel(ctx, oldPrefab, *newPrefab, data, root, "", 0);
    replaced.nestedRoot = NULL_ENTITY;
    replaced.nestedPath.clear();
    replaced.legacy = data.legacy && !oldPrefab;

    outNode = YAML::Clone(node);
    removeInstanceKeys(outNode);
    // a Missing Prefab root's children are the scene's GameObjects below it: they stay where
    // they are and become added GameObjects of the new instance
    outNode.remove("children");

    PrefabComponent component;
    component.name = newPath.stem().string();
    component.path = newPath.generic_string();
    component.guid = getPrefabGuid(newPath);
    outNode["components"][prefabKey] = Stream::encodePrefabComponent(component);

    // Unity's changeRootNameToAssetName, for a root still named after its old prefab
    if (settings.renameRoot) {
        const std::string currentName = node["name"] ? node["name"].as<std::string>() : std::string();
        std::string oldAssetName = oldPrefab && oldPrefab->registry ? oldPrefab->registry->getEntityName(oldPrefab->root) : std::string();
        if (oldAssetName.empty()) {
            oldAssetName = std::filesystem::path(oldComponent.path).stem().string();
        }
        if (!oldAssetName.empty() && namedAfter(currentName, oldAssetName)) {
            std::string newName = newPrefab->registry->getEntityName(newPrefab->root);
            if (newName.empty()) {
                newName = newPath.stem().string();
            }
            outNode["name"] = ProjectUtils::makeUniqueEntityName(sceneProject->scene, sceneProject->entities, newName, {root});
        }
    }

    YAML::Node block = Stream::encodePrefabInstanceBlock(replaced);
    block["version"] = Prefab::FILE_VERSION;
    if (replaced.legacy) {
        block["legacyRoot"] = true;
    }
    outNode["prefabInstance"] = block;
    return true;
}

bool editor::Project::rebuildPrefabInstance(uint32_t sceneId, Entity root, const YAML::Node& node, const PrefabMemberIds& memberIds) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !node || !node.IsMap() || !sceneProject->scene->isEntityCreated(root)) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    const std::string prefabKey = componentKey(ComponentType::PrefabComponent);
    const YAML::Node components = node["components"];
    if (!components || !components[prefabKey]) {
        return false;
    }

    const std::vector<Entity> selection = sceneProject->selectedEntities;
    const Entity mainCamera = sceneProject->mainCamera;

    // What the root is now goes: its instance (members, added GameObjects), or its record
    if (PrefabComponent* current = scene->findComponent<PrefabComponent>(root)) {
        Prefab* prefab = getPrefab(current->path);
        const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, root) : nullptr;
        if (instance && instance->rootEntity == root) {
            std::vector<Entity> members;
            for (const Prefab::EntityMember& member : instance->members) {
                members.push_back(member.localEntity);
            }
            const std::filesystem::path path = current->path;
            unimportPrefab(sceneId, path, root, members, true, true);
        }
    }
    // A Missing Prefab record of the root stays: it only counts while the root names its
    // asset and is no instance (getMissingPrefab), and made Missing again by an undo the
    // root is not reported a second time
    const PrefabComponent component = Stream::decodePrefabComponent(components[prefabKey]);

    // The root as the node has it: name, state, components and its PrefabComponent
    if (node["name"]) {
        scene->setEntityName(root, node["name"].as<std::string>());
    }
    Stream::decodeEntityState(root, scene, node);
    for (ComponentType type : Catalog::findComponents(scene, root)) {
        if (type == ComponentType::PrefabComponent || type == ComponentType::Transform) {
            continue;
        }
        if (!components[componentKey(type)]) {
            ProjectUtils::removeEntityComponent(scene, root, type, sceneProject->entities);
        }
    }
    {
        YAML::Node own = YAML::Clone(components);
        own.remove(prefabKey);
        if (own.size() > 0) {
            Stream::decodeComponents(root, transformParent(scene, root), scene, own);
        }
    }
    if (PrefabComponent* existing = scene->findComponent<PrefabComponent>(root)) {
        *existing = component;
    } else {
        scene->addComponent<PrefabComponent>(root, component);
    }

    const PrefabInstanceData data = Stream::decodePrefabInstanceData(node);
    std::unordered_map<Entity, Entity> entityRemap;
    importPrefab(sceneProject, &sceneProject->entities, component.path, root, false, &data, &entityRemap, &memberIds);
    if (getMissingPrefab(sceneId, root)) {
        // Missing: the record keeps every key the node has, and the GameObjects the scene
        // added below the root come back as the node holds them
        setMissingPrefab(sceneProject, root, node, &memberIds);
        if (node["children"]) {
            for (const YAML::Node& child : node["children"]) {
                if (child["entity"] && scene->isEntityCreated(child["entity"].as<Entity>())) {
                    continue;
                }
                Stream::decodeEntity(child, scene, &sceneProject->entities, this, sceneProject, root, true, false, &entityRemap);
            }
        }
    }
    if (!entityRemap.empty()) {
        remapEntityProperties(scene, sceneProject->entities, entityRemap, false);
    }

    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
    CameraTextureLink::resolve(scene);
    if (mainCamera != NULL_ENTITY && scene->isEntityCreated(mainCamera)) {
        sceneProject->mainCamera = mainCamera;
    }
    std::vector<Entity> kept;
    for (Entity entity : selection) {
        if (scene->isEntityCreated(entity)) {
            kept.push_back(entity);
        }
    }
    sceneProject->selectedEntities = kept;
    updateScenePrefabs(sceneProject);
    sceneProject->needUpdateRender = true;
    sceneProject->structureVersion++;
    return true;
}

// ---- Previews ----

Entity editor::Project::instantiatePrefabPreview(Scene* scene, const std::filesystem::path& filepath) {
    if (!scene || filepath.empty()) {
        return NULL_ENTITY;
    }
    // A scene of the import's own: an id no loaded scene has, forgotten right after, so no
    // instance list, selection or Hierarchy ever sees it
    SceneProject preview;
    preview.id = allocateSceneId();
    preview.name = "Prefab preview";
    preview.scene = scene;
    preview.sceneType = SceneType::SCENE_3D;
    preview.prefabPreview = true;
    if (preview.id == NULL_PROJECT_SCENE) {
        return NULL_ENTITY;
    }

    Entity root = scene->createUserEntity();
    PrefabComponent component;
    component.name = filepath.stem().string();
    component.path = filepath.generic_string();
    component.guid = getPrefabGuid(filepath);
    scene->addComponent<PrefabComponent>(root, component);
    preview.entities.push_back(root);

    bool built = false;
    try {
        importPrefab(&preview, &preview.entities, filepath, root, false);
        const PrefabComponent* resolved = scene->findComponent<PrefabComponent>(root);
        const Prefab* prefab = resolved ? getPrefab(resolved->path) : nullptr;
        const Prefab::Instance* instance = prefab ? prefab->getInstance(preview.id, root) : nullptr;
        built = instance && instance->rootEntity == root;
    } catch (const std::exception& e) {
        Out::error("Prefab preview of '%s' failed: %s", filepath.string().c_str(), e.what());
    }
    for (auto& [path, prefab] : prefabAssets) {
        prefab.instances.erase(preview.id);
    }
    if (!built) {
        for (Entity entity : preview.entities) {
            if (scene->isEntityCreated(entity)) {
                scene->destroyEntity(entity);
            }
        }
        return NULL_ENTITY;
    }
    return root;
}

std::vector<std::filesystem::path> editor::Project::getPrefabDependencies(const std::filesystem::path& filepath) {
    std::vector<std::filesystem::path> result;
    std::vector<std::filesystem::path> pending = {filepath};
    while (!pending.empty() && result.size() < 256) {
        const std::filesystem::path path = pending.back();
        pending.pop_back();
        if (path.empty() || std::find(result.begin(), result.end(), path) != result.end()) {
            continue;
        }
        result.push_back(path);
        const Prefab* prefab = loadPrefabAsset(path);
        if (!prefab || !prefab->registry) {
            continue;
        }
        auto components = prefab->registry->getComponentArray<PrefabComponent>();
        for (size_t i = 0; i < components->size(); i++) {
            const PrefabComponent& component = components->getComponentFromIndex(i);
            const std::filesystem::path nested = resolvePrefabPath(component.guid, component.path);
            if (!nested.empty() && nested != path) {
                pending.push_back(nested);
            }
        }
    }
    return result;
}

// ---- Usage (Prefabs dialog) ----

size_t editor::PrefabUsage::totalInstances() const {
    size_t total = 0;
    for (const PrefabSceneUsage& scene : scenes) {
        total += scene.instances + scene.nested;
    }
    return total;
}

size_t editor::PrefabUsage::totalMissing() const {
    size_t total = 0;
    for (const PrefabSceneUsage& scene : scenes) {
        total += scene.missing;
    }
    return total;
}

std::map<std::string, editor::PrefabUsage> editor::Project::collectPrefabUsage() {
    std::map<std::string, PrefabUsage> usage;

    auto keyFor = [](const std::string& guid, const std::filesystem::path& path) {
        return guid.empty() ? "path:" + path.generic_string() : guid;
    };
    auto entryFor = [&](const std::string& givenGuid, const std::filesystem::path& path) -> PrefabUsage& {
        // a reference written before GUIDs means the GUID its path was upgraded to
        const std::string guid = givenGuid.empty() && !path.empty() ? AssetDatabase::legacyGuid(path) : givenGuid;
        PrefabUsage& entry = usage[keyFor(guid, path)];
        if (entry.guid.empty()) {
            entry.guid = guid;
        }
        if (entry.path.empty()) {
            entry.path = path;
        }
        return entry;
    };
    auto sceneFor = [](PrefabUsage& entry, const SceneProject& sceneProject) -> PrefabSceneUsage& {
        for (PrefabSceneUsage& scene : entry.scenes) {
            if (scene.sceneId == sceneProject.id) {
                return scene;
            }
        }
        PrefabSceneUsage scene;
        scene.sceneId = sceneProject.id;
        scene.sceneName = sceneProject.name;
        scene.loaded = sceneProject.scene != nullptr;
        entry.scenes.push_back(scene);
        return entry.scenes.back();
    };

    for (const std::filesystem::path& path : findPrefabFiles()) {
        entryFor(getPrefabGuid(path), path);
    }

    // The nested prefabs of an instance of `path`, minus the ones its layers remove
    std::function<void(const SceneProject&, const std::filesystem::path&, const std::vector<const PrefabInstanceData*>&, size_t)> countNested;
    // A GameObject node of a scene file and what it holds
    std::function<void(const SceneProject&, const YAML::Node&, size_t)> walkNode;

    countNested = [&](const SceneProject& sceneProject, const std::filesystem::path& path,
            const std::vector<const PrefabInstanceData*>& layers, size_t depth) {
        Prefab* asset = depth < 32 ? loadPrefabAsset(path) : nullptr;
        if (!asset || !asset->registry) {
            return;
        }
        for (Entity registryEntity : std::vector<Entity>(asset->registryEntities)) {
            if (!asset->isNestedRoot(registryEntity)) {
                continue;
            }
            bool removed = false;
            for (Entity current = registryEntity; current != NULL_ENTITY && !removed; current = asset->getRegistryParent(current)) {
                for (const PrefabInstanceData* layer : layers) {
                    if (layer && layer->removedGameObjects.count(current)) {
                        removed = true;
                    }
                }
            }
            if (removed) {
                continue;
            }
            const PrefabComponent component = asset->registry->getComponent<PrefabComponent>(registryEntity);
            const std::filesystem::path nestedPath = resolvePrefabPath(component.guid, component.path);
            if (nestedPath.empty()) {
                sceneFor(entryFor(component.guid, component.path), sceneProject).missing++;
                continue;
            }
            sceneFor(entryFor(getPrefabGuid(nestedPath), nestedPath), sceneProject).nested++;
            std::vector<const PrefabInstanceData*> next;
            for (const PrefabInstanceData* layer : layers) {
                if (const PrefabInstanceData* nested = layer ? layer->findNested(registryEntity, component.path) : nullptr) {
                    next.push_back(nested);
                    for (const YAML::Node& added : nested->addedGameObjects) {
                        walkNode(sceneProject, added, depth + 1);
                    }
                }
            }
            auto assetLayer = asset->nestedInstances.find(registryEntity);
            if (assetLayer != asset->nestedInstances.end()) {
                next.push_back(&assetLayer->second);
            }
            countNested(sceneProject, nestedPath, next, depth + 1);
            // the asset may have been read again by a nested load: look it up once more
            asset = loadPrefabAsset(path);
            if (!asset || !asset->registry) {
                return;
            }
        }
    };

    walkNode = [&](const SceneProject& sceneProject, const YAML::Node& node, size_t depth) {
        if (!node || !node.IsMap() || depth > 256) {
            return;
        }
        const YAML::Node components = node["components"];
        const std::string prefabKey = componentKey(ComponentType::PrefabComponent);
        if (components && components.IsMap() && components[prefabKey]) {
            const PrefabComponent component = Stream::decodePrefabComponent(components[prefabKey]);
            const std::filesystem::path path = resolvePrefabPath(component.guid, component.path);
            const PrefabInstanceData data = Stream::decodePrefabInstanceData(node);
            if (path.empty()) {
                sceneFor(entryFor(component.guid, component.path), sceneProject).missing++;
            } else {
                sceneFor(entryFor(getPrefabGuid(path), path), sceneProject).instances++;
                countNested(sceneProject, path, {&data}, depth + 1);
            }
            for (const YAML::Node& added : data.addedGameObjects) {
                walkNode(sceneProject, added, depth + 1);
            }
        }
        if (node["children"]) {
            for (const YAML::Node& child : node["children"]) {
                walkNode(sceneProject, child, depth + 1);
            }
        }
        if (node["members"]) {
            for (const YAML::Node& member : node["members"]) {
                walkNode(sceneProject, member, depth + 1);
            }
        }
    };

    for (const SceneProject& sceneProject : scenes) {
        if (sceneProject.scene) {
            // Loaded: its instances as they are
            std::unordered_set<Entity> members;
            for (const auto& [path, prefab] : prefabAssets) {
                auto sceneIt = prefab.instances.find(sceneProject.id);
                if (sceneIt == prefab.instances.end()) {
                    continue;
                }
                for (const Prefab::Instance& instance : sceneIt->second) {
                    for (const Prefab::EntityMember& member : instance.members) {
                        members.insert(member.localEntity);
                    }
                }
            }
            for (const auto& [path, prefab] : prefabAssets) {
                auto sceneIt = prefab.instances.find(sceneProject.id);
                if (sceneIt == prefab.instances.end()) {
                    continue;
                }
                for (const Prefab::Instance& instance : sceneIt->second) {
                    PrefabSceneUsage& scene = sceneFor(entryFor(getPrefabGuid(path), path), sceneProject);
                    if (members.count(instance.rootEntity)) {
                        scene.nested++;
                    } else {
                        scene.instances++;
                    }
                }
            }
            for (const auto& [root, missing] : sceneProject.missingPrefabs) {
                if (getMissingPrefab(sceneProject.id, root)) {
                    sceneFor(entryFor(missing.guid, missing.path), sceneProject).missing++;
                }
            }
            continue;
        }

        // Closed: read from its file
        std::filesystem::path file = sceneProject.filepath;
        if (file.empty()) {
            continue;
        }
        if (file.is_relative()) {
            file = getProjectPath() / file;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            continue;
        }
        try {
            const YAML::Node sceneNode = YAML::LoadFile(file.string());
            if (sceneNode["entities"]) {
                for (const YAML::Node& entityNode : sceneNode["entities"]) {
                    walkNode(sceneProject, entityNode, 0);
                }
            }
        } catch (const std::exception& e) {
            Out::warning("Prefab usage: cannot read scene '%s': %s", file.string().c_str(), e.what());
        }
    }

    // The prefabs nesting each prefab
    for (const std::filesystem::path& path : findPrefabFiles()) {
        Prefab* prefab = loadPrefabAsset(path);
        if (!prefab || !prefab->registry) {
            continue;
        }
        std::set<std::string> listed;
        auto components = prefab->registry->getComponentArray<PrefabComponent>();
        for (size_t i = 0; i < components->size(); i++) {
            const Entity entity = components->getEntity(i);
            if (!prefab->isNestedRoot(entity)) {
                continue;
            }
            const PrefabComponent& component = components->getComponentFromIndex(i);
            const std::filesystem::path nested = resolvePrefabPath(component.guid, component.path);
            const std::string key = nested.empty() ? keyFor(component.guid, component.path) : keyFor(getPrefabGuid(nested), nested);
            if (listed.insert(key).second) {
                PrefabUsage& entry = nested.empty() ? entryFor(component.guid, component.path) : entryFor(getPrefabGuid(nested), nested);
                entry.nestedIn.push_back(path);
            }
        }
    }

    return usage;
}

// ---- Scene view drop ----

Entity editor::Project::findObjectByRayIgnoring(uint32_t sceneId, float x, float y, const std::unordered_set<Entity>& ignored) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->sceneRender) {
        return NULL_ENTITY;
    }
    std::vector<Entity> pickable;
    for (Entity entity : getPickableEntities(sceneProject)) {
        if (!ignored.count(entity)) {
            pickable.push_back(entity);
        }
    }
    Ray ray = sceneProject->sceneRender->screenToRay(x, y);
    float distance = FLT_MAX;
    size_t index = 0;
    return findBestEntityByRay(pickable, sceneProject->scene, ray, sceneProject->scene, sceneProject->sceneType, distance, index);
}

void editor::Project::refreshScenePrefabList(uint32_t sceneId) {
    updateScenePrefabs(getScene(sceneId));
}
