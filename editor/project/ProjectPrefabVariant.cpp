// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Prefab Variants (editor/project/PREFABS.md, section 19). A variant is a prefab
// whose root GameObject is an instance of another prefab, its base: the nested model of
// section 17 with the root as the nested root. What the variant overrides of its base is
// its nestedInstances[root], the GameObjects it adds are the root's registry children, its
// references into the base stripped ids whose way starts at the root.
//
// In a scene, the root of an instance of a variant roots one instance per level: the
// variant's (named by the GameObject's PrefabComponent, the outermost), its base's, and so on
// down the chain. The helpers here find those levels; the import, the encoding, the override
// model and the export go through them.

#include "Project.h"
#include "component/PrefabComponent.h"
#include "ProjectPrefabInternal.h"
#include "Out.h"
#include "Stream.h"
#include "util/AssetDatabase.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"
#include "util/Util.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor::prefabinternal;

// ---- Levels ----

editor::Prefab::Instance* editor::Project::findInstanceAt(SceneProject* sceneProject, Entity root, const std::filesystem::path& path,
        std::filesystem::path* resolved) {
    if (path.empty()) {
        return findInstanceRootedAt(sceneProject, root, resolved);
    }
    if (!sceneProject || root == NULL_ENTITY) {
        return nullptr;
    }
    Prefab* prefab = getPrefab(path);
    Prefab::Instance* instance = prefab ? prefab->getInstanceByRoot(sceneProject->id, root) : nullptr;
    if (instance && resolved) {
        *resolved = path;
    }
    return instance;
}

const editor::Prefab::Instance* editor::Project::findInstanceAt(const SceneProject* sceneProject, Entity root, const std::filesystem::path& path,
        std::filesystem::path* resolved) const {
    return const_cast<Project*>(this)->findInstanceAt(const_cast<SceneProject*>(sceneProject), root, path, resolved);
}

std::filesystem::path editor::Project::instancePathAt(const Prefab& holder, Entity registryEntity) const {
    if (!holder.registry || registryEntity == NULL_ENTITY || !holder.registry->isEntityCreated(registryEntity)) {
        return std::filesystem::path();
    }
    const PrefabComponent* component = holder.registry->findComponent<PrefabComponent>(registryEntity);
    return component ? std::filesystem::path(component->path) : std::filesystem::path();
}

std::vector<editor::PrefabInstanceLevel> editor::Project::getInstanceLevels(SceneProject* sceneProject, Entity root) {
    std::vector<PrefabInstanceLevel> levels;
    std::filesystem::path path;
    Prefab::Instance* instance = findInstanceRootedAt(sceneProject, root, &path);
    std::unordered_set<std::string> seen;
    while (instance && levels.size() < 64 && seen.insert(path.lexically_normal().generic_string()).second) {
        Prefab* prefab = getPrefab(path);
        if (!prefab) {
            break;
        }
        levels.push_back({path, prefab, instance});
        if (!prefab->isVariant()) {
            break;
        }
        path = instancePathAt(*prefab, prefab->root);
        instance = findInstanceAt(sceneProject, root, path);
    }
    return levels;
}

std::vector<editor::NestedInstanceLevel> editor::Project::getNestedInstanceLevels(SceneProject* sceneProject, const std::filesystem::path& path,
        const Prefab::Instance& instance) {
    std::vector<NestedInstanceLevel> nested;
    Prefab* prefab = getPrefab(path);
    if (!sceneProject || !sceneProject->scene || !prefab || !prefab->registry) {
        return nested;
    }
    Scene* scene = sceneProject->scene;
    if (prefab->isVariant()) {
        NestedInstanceLevel level;
        level.registryEntity = prefab->root;
        level.localRoot = instance.rootEntity;
        level.path = instancePathAt(*prefab, prefab->root);
        level.instance = findInstanceAt(sceneProject, instance.rootEntity, level.path);
        level.variantBase = true;
        nested.push_back(std::move(level));
    }
    for (const Prefab::EntityMember& member : instance.members) {
        if (!scene->isEntityCreated(member.localEntity)) {
            continue;
        }
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(member.localEntity);
        if (!component || (component->path.empty() && component->guid.empty())) {
            continue;
        }
        NestedInstanceLevel level;
        level.registryEntity = member.registryEntity;
        level.localRoot = member.localEntity;
        level.path = component->path;
        Prefab* nestedPrefab = getPrefab(component->path);
        level.instance = nestedPrefab ? nestedPrefab->getInstanceByRoot(sceneProject->id, member.localEntity) : nullptr;
        nested.push_back(std::move(level));
    }
    return nested;
}

std::vector<editor::NestedInstanceLevel> editor::Project::getNestedInstanceLevels(const SceneProject* sceneProject, const std::filesystem::path& path,
        const Prefab::Instance& instance) const {
    return const_cast<Project*>(this)->getNestedInstanceLevels(const_cast<SceneProject*>(sceneProject), path, instance);
}

bool editor::Project::findPrefabHolder(SceneProject* sceneProject, Entity root, const std::filesystem::path& path, PrefabHolderLink& link) {
    link = PrefabHolderLink();
    if (!sceneProject || root == NULL_ENTITY) {
        return false;
    }
    // The variant whose base this level is: the level above it at the same root
    if (!path.empty()) {
        const std::vector<PrefabInstanceLevel> levels = getInstanceLevels(sceneProject, root);
        for (size_t i = 1; i < levels.size(); i++) {
            if (levels[i].path == path) {
                link.path = levels[i - 1].path;
                link.prefab = levels[i - 1].prefab;
                link.instance = levels[i - 1].instance;
                link.registryEntity = levels[i - 1].prefab->root;
                link.variant = true;
                return true;
            }
        }
    }
    // The instance the root is a member of (a nested root)
    for (auto& [otherPath, other] : prefabAssets) {
        auto sceneIt = other.instances.find(sceneProject->id);
        if (sceneIt == other.instances.end()) {
            continue;
        }
        for (Prefab::Instance& holder : sceneIt->second) {
            for (const Prefab::EntityMember& member : holder.members) {
                if (member.localEntity == root) {
                    link.path = otherPath;
                    link.prefab = &other;
                    link.instance = &holder;
                    link.registryEntity = member.registryEntity;
                    link.variant = false;
                    return true;
                }
            }
        }
    }
    return false;
}

Entity editor::Project::prefabLevelKey(Entity root, size_t depth) {
    if (depth == 0) {
        return root;
    }
    return static_cast<Entity>((static_cast<Entity>(depth & 0x1F) << 27) | (root & 0x07FFFFFFu));
}

// ---- Bases ----

std::filesystem::path editor::Project::getVariantBasePath(const Prefab& prefab) {
    const PrefabComponent* component = prefab.getBaseComponent();
    if (!component) {
        return std::filesystem::path();
    }
    PrefabComponent base = *component;
    if (!resolvePrefabReference(base)) {
        return std::filesystem::path();
    }
    return base.path;
}

std::filesystem::path editor::Project::getPrefabBasePath(const std::filesystem::path& filepath) {
    Prefab* prefab = loadPrefabAsset(filepath);
    return prefab ? getVariantBasePath(*prefab) : std::filesystem::path();
}

bool editor::Project::isPrefabVariant(const std::filesystem::path& filepath) {
    if (const Prefab* prefab = getPrefab(filepath); prefab && prefab->registry) {
        return prefab->isVariant();
    }
    return !getPrefabFileBase(filepath).empty();
}

std::vector<std::filesystem::path> editor::Project::getPrefabVariantChain(const std::filesystem::path& filepath, bool* broken) {
    std::vector<std::filesystem::path> chain;
    if (broken) {
        *broken = false;
    }
    std::unordered_set<std::string> seen;
    std::filesystem::path current = filepath;
    while (!current.empty()) {
        if (!seen.insert(current.lexically_normal().generic_string()).second || chain.size() >= 64) {
            Out::error("Prefab Variant %s: its bases make a cycle at %s", filepath.string().c_str(), current.string().c_str());
            if (broken) *broken = true;
            break;
        }
        Prefab* prefab = loadPrefabAsset(current);
        if (!prefab) {
            if (broken) *broken = true;
            break;
        }
        chain.push_back(current);
        if (!prefab->isVariant()) {
            break;
        }
        const std::filesystem::path base = getVariantBasePath(*prefab);
        if (base.empty()) {
            if (broken) *broken = true;
            break;
        }
        current = base;
    }
    return chain;
}

std::filesystem::path editor::Project::getPrefabFileBase(const std::filesystem::path& filepath) {
    if (filepath.empty() || projectPath.empty()) {
        return std::filesystem::path();
    }
    std::filesystem::path relative = filepath.is_absolute() ? normalizeToProjectRelative(filepath) : filepath.lexically_normal();
    if (relative.empty()) {
        return std::filesystem::path();
    }
    // A loaded prefab knows (it may hold changes not saved yet)
    if (const Prefab* prefab = getPrefab(relative); prefab && prefab->registry) {
        const PrefabComponent* component = prefab->getBaseComponent();
        if (!component) {
            return std::filesystem::path();
        }
        const std::filesystem::path resolved = !component->guid.empty() ? assetDatabase.getPath(component->guid) : std::filesystem::path();
        return resolved.empty() ? std::filesystem::path(component->path) : resolved;
    }
    const auto now = std::chrono::steady_clock::now();
    PrefabBaseCache& entry = prefabBaseCache[relative];
    if (entry.checked != std::chrono::steady_clock::time_point{} && now - entry.checked < std::chrono::seconds(1)) {
        const std::filesystem::path resolved = !entry.baseGuid.empty() ? assetDatabase.getPath(entry.baseGuid) : std::filesystem::path();
        return resolved.empty() ? entry.basePath : resolved;
    }
    entry.checked = now;
    const std::filesystem::path fullPath = getProjectPath() / relative;
    std::error_code timeError;
    std::error_code sizeError;
    const auto writeTime = std::filesystem::last_write_time(fullPath, timeError);
    const auto size = std::filesystem::file_size(fullPath, sizeError);
    if (timeError || sizeError) {
        entry.baseGuid.clear();
        entry.basePath.clear();
        return std::filesystem::path();
    }
    if (writeTime != entry.writeTime || size != entry.size) {
        entry.writeTime = writeTime;
        entry.size = size;
        entry.baseGuid.clear();
        entry.basePath.clear();
        try {
            const YAML::Node node = loadPrefabFile(fullPath);
            const Prefab::FileHeader header = Prefab::readHeader(node);
            const YAML::Node members = node.IsMap() ? node["members"] : YAML::Node();
            YAML::Node rootNode;
            if (members && members.IsSequence()) {
                for (const YAML::Node& member : members) {
                    if (header.root != NULL_ENTITY && member["entity"] && YamlNumbers::as<Entity>(member["entity"]) == header.root) {
                        rootNode.reset(member);
                        break;
                    }
                }
                if (!rootNode && members.size() > 0 && header.version >= Prefab::ROOT_LAYOUT_VERSION) {
                    rootNode.reset(members[0]);
                }
            }
            const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);
            if (rootNode && rootNode.IsMap() && header.version >= Prefab::ROOT_LAYOUT_VERSION && rootNode["components"] &&
                    rootNode["components"].IsMap() && rootNode["components"][prefabKey]) {
                const YAML::Node base = rootNode["components"][prefabKey];
                if (base["guid"]) entry.baseGuid = base["guid"].as<std::string>();
                if (base["path"]) entry.basePath = base["path"].as<std::string>();
            }
        } catch (const std::exception&) {
            // not a prefab this can read: no base
        }
    }
    const std::filesystem::path resolved = !entry.baseGuid.empty() ? assetDatabase.getPath(entry.baseGuid) : std::filesystem::path();
    return resolved.empty() ? entry.basePath : resolved;
}

std::vector<std::filesystem::path> editor::Project::findPrefabVariantsOf(const std::filesystem::path& filepath) {
    std::vector<std::filesystem::path> variants;
    const std::filesystem::path key = filepath.lexically_normal();
    for (const std::filesystem::path& file : findPrefabFiles()) {
        const std::filesystem::path relative = file.is_absolute() ? normalizeToProjectRelative(file) : file;
        if (relative.lexically_normal() == key) {
            continue;
        }
        const std::filesystem::path base = getPrefabFileBase(relative);
        if (!base.empty() && base.lexically_normal() == key) {
            variants.push_back(relative);
        }
    }
    std::sort(variants.begin(), variants.end());
    return variants;
}

// ---- Create ----

bool editor::Project::canCreatePrefabVariant(uint32_t sceneId, Entity root, std::string* reason) const {
    auto fail = [reason](const char* message) {
        if (reason) {
            *reason = message;
        }
        return false;
    };
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || root == NULL_ENTITY || !sceneProject->scene->isEntityCreated(root)) {
        return fail("No GameObject");
    }
    if (isMissingPrefab(sceneId, root)) {
        return fail("The Prefab of this instance is missing");
    }
    if (!const_cast<Project*>(this)->findInstanceRootedAt(const_cast<SceneProject*>(sceneProject), root)) {
        return fail("Not the root of a Prefab instance");
    }
    // a nested instance belongs to its prefab (as in Unity, which makes variants of instance
    // roots the scene holds itself)
    PrefabObjectRef ref;
    if (findPrefabObject(sceneId, root, ref) && !ref.isRoot) {
        return fail("A Prefab instance nested in another one belongs to that Prefab");
    }
    return true;
}

bool editor::Project::createPrefabVariant(uint32_t sceneId, const std::filesystem::path& filepath, Entity root, const std::string& guid) {
    if (!filepath.is_relative() || !Util::isPrefabFile(filepath.string())) {
        Out::error("Prefab Variant path must be a project-relative .prefab: %s", filepath.string().c_str());
        return false;
    }
    if (prefabAssets.count(filepath)) {
        Out::error("Prefab already exists at %s", filepath.string().c_str());
        return false;
    }
    std::string reason;
    if (!canCreatePrefabVariant(sceneId, root, &reason)) {
        Out::error("Cannot create a Prefab Variant at %s: %s", filepath.string().c_str(), reason.c_str());
        return false;
    }
    SceneProject* sceneProject = getScene(sceneId);
    Scene* scene = sceneProject->scene;
    std::filesystem::path basePath;
    findInstanceRootedAt(sceneProject, root, &basePath);
    if (basePath.lexically_normal() == filepath.lexically_normal()) {
        return false;
    }

    // The variant's root GameObject: the instance root's name, state and components, its
    // PrefabComponent naming the base. The base's instance at the root stays; its scene
    // data (overrides, removed and added GameObjects, references into it) becomes the
    // variant's below, as Create Prefab does for nested instances (PREFABS.md 17, 18).
    YAML::Node rootNode(YAML::NodeType::Map);
    rootNode["type"] = "Entity";
    rootNode["name"] = scene->getEntityName(root);
    Stream::encodeEntityState(rootNode, root, scene);
    YAML::Node components = Stream::encodeComponents(root, scene, scene->getSignature(root));
    if (components.IsMap() && components.size() > 0) {
        rootNode["components"] = components;
    }

    Prefab variant;
    variant.registry = std::make_unique<EntityRegistry>();
    variant.isModified = true;
    std::vector<Entity> decoded = Stream::decodeEntity(rootNode, variant.registry.get(), &variant.registryEntities);
    if (decoded.size() != 1) {
        Out::error("Prefab Variant %s: its root could not be made", filepath.string().c_str());
        return false;
    }
    variant.root = decoded[0];
    if (!variant.getBaseComponent()) {
        Out::error("Prefab Variant %s: its root has no base", filepath.string().c_str());
        return false;
    }
    // (the base reference by its GUID, its path a hint)
    if (PrefabComponent* baseLink = variant.registry->findComponent<PrefabComponent>(variant.root)) {
        baseLink->name = basePath.stem().string();
        baseLink->path = basePath.string();
        baseLink->guid = getPrefabGuid(basePath);
    }
    PrefabInstanceData layer;
    layer.present = true;
    variant.nestedInstances[variant.root] = layer;

    std::string assetGuid = guid;
    if (!AssetDatabase::isGuid(assetGuid) || !assetDatabase.getPath(assetGuid).empty()) {
        do {
            assetGuid = AssetDatabase::generateGuid();
        } while (!assetDatabase.getPath(assetGuid).empty());
    }

    Prefab::Instance instance;
    instance.instanceId = variant.nextInstanceId++;
    instance.rootEntity = root;
    instance.rootRegistryEntity = variant.root;
    variant.instances[sceneId].push_back(std::move(instance));

    auto [it, inserted] = prefabAssets.emplace(filepath, std::move(variant));
    Prefab& prefab = it->second;

    // The instance root is an instance of the variant now (its outermost level)
    PrefabComponent link;
    link.name = filepath.stem().string();
    link.path = filepath.string();
    link.guid = assetGuid;
    if (PrefabComponent* existing = scene->findComponent<PrefabComponent>(root)) {
        *existing = link;
    } else {
        scene->addComponent<PrefabComponent>(root, link);
    }

    // The base instance's scene data, the GameObjects added to it and the references into it
    // are the variant's
    moveNestedInstancesToAsset(prefab, filepath, sceneProject, root, {{root, prefab.root}});
    for (Entity registryEntity : std::vector<Entity>(prefab.registryEntities)) {
        for (ComponentType type : Catalog::findComponents(prefab.registry.get(), registryEntity)) {
            if (type != ComponentType::PrefabComponent && Catalog::mayHoldReferences(prefab.registry.get(), registryEntity, type, true, true)) {
                remapReferencesToPrefab(sceneId, root, prefab.registry.get(), registryEntity, type, {}, filepath);
            }
        }
    }
    overrideOutsideReferences(sceneId, root);

    assetDatabase.registerAsset(filepath, assetGuid);
    savePrefabToDisk(filepath);
    // the instance as the variant makes it (same ids): what it shows does not change
    refreshPrefabInstances(filepath);
    updateScenePrefabs(sceneProject);
    sceneProject->isModified = true;
    sceneProject->structureVersion++;
    Out::info("Created Prefab Variant of '%s' at '%s'", basePath.generic_string().c_str(), filepath.generic_string().c_str());
    return true;
}

bool editor::Project::removePrefabVariantInstance(uint32_t sceneId, const std::filesystem::path& filepath, Entity root, const YAML::Node& rootNode) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !rootNode || !rootNode.IsMap()) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    // The variant goes (its other instances, made since, become instances of the base: the
    // root's node says which), the root names the base again and is rebuilt from its node
    auto it = prefabAssets.find(filepath);
    if (it != prefabAssets.end()) {
        for (auto& [otherSceneId, instances] : it->second.instances) {
            SceneProject* other = getScene(otherSceneId);
            for (const Prefab::Instance& instance : instances) {
                if (other && other->scene && (otherSceneId != sceneId || instance.rootEntity != root)) {
                    Out::warning("Prefab Variant %s removed: its instance '%s' keeps its GameObjects", filepath.string().c_str(),
                        other->scene->getEntityName(instance.rootEntity).c_str());
                }
            }
        }
        if (it->second.registry) {
            it->second.registry->clear();
        }
        prefabAssets.erase(it);
    }
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);
    const YAML::Node components = rootNode["components"];
    if (components && components.IsMap() && components[prefabKey]) {
        PrefabComponent link = Stream::decodePrefabComponent(components[prefabKey]);
        if (PrefabComponent* existing = scene->findComponent<PrefabComponent>(root)) {
            *existing = link;
        } else {
            scene->addComponent<PrefabComponent>(root, link);
        }
    }
    const bool rebuilt = reimportPrefabInstance(sceneProject, root, rootNode);
    updateScenePrefabs(sceneProject);
    sceneProject->structureVersion++;
    return rebuilt;
}

bool editor::Project::createPrefabVariantAsset(const std::filesystem::path& base, const std::filesystem::path& filepath, const std::string& guid) {
    if (!filepath.is_relative() || !Util::isPrefabFile(filepath.string())) {
        Out::error("Prefab Variant path must be a project-relative .prefab: %s", filepath.string().c_str());
        return false;
    }
    std::error_code ec;
    if (prefabAssets.count(filepath) || std::filesystem::exists(getProjectPath() / filepath, ec)) {
        Out::error("A file already exists at %s", filepath.string().c_str());
        return false;
    }
    const std::string baseGuid = getPrefabGuid(base);
    bool broken = false;
    getPrefabVariantChain(base, &broken);
    if (broken || !loadPrefabAsset(base)) {
        Out::error("Prefab Variant of %s: the prefab (or one of its bases) cannot be read", base.string().c_str());
        return false;
    }

    // The root as an instance of the base has it (a variant base's root with what it
    // overrides): its name, state and components, the default overrides (position,
    // rotation) included. Nothing is overridden yet.
    Scene previewScene;
    const Entity previewRoot = instantiatePrefabPreview(&previewScene, base);
    if (previewRoot == NULL_ENTITY) {
        Out::error("Prefab Variant of %s: the prefab could not be instantiated", base.string().c_str());
        return false;
    }
    YAML::Node rootNode(YAML::NodeType::Map);
    rootNode["type"] = "Entity";
    rootNode["name"] = previewScene.getEntityName(previewRoot);
    Stream::encodeEntityState(rootNode, previewRoot, &previewScene);
    YAML::Node components = Stream::encodeComponents(previewRoot, &previewScene, previewScene.getSignature(previewRoot));
    if (components.IsMap() && components.size() > 0) {
        rootNode["components"] = components;
    }

    Prefab variant;
    variant.registry = std::make_unique<EntityRegistry>();
    variant.isModified = true;
    std::vector<Entity> decoded = Stream::decodeEntity(rootNode, variant.registry.get(), &variant.registryEntities);
    if (decoded.size() != 1) {
        return false;
    }
    variant.root = decoded[0];
    // references into the base's GameObjects (the preview's ids) are not the variant's
    std::unordered_map<Entity, Entity> none;
    for (ComponentType type : Catalog::findComponents(variant.registry.get(), variant.root)) {
        if (type != ComponentType::PrefabComponent) {
            remapEntityPropertiesInComponent(variant.registry.get(), variant.root, type, {}, none);
        }
    }
    PrefabComponent link;
    link.name = base.stem().string();
    link.path = base.string();
    link.guid = baseGuid;
    if (PrefabComponent* existing = variant.registry->findComponent<PrefabComponent>(variant.root)) {
        *existing = link;
    } else {
        variant.registry->addComponent<PrefabComponent>(variant.root, link);
    }
    PrefabInstanceData layer;
    layer.present = true;
    variant.nestedInstances[variant.root] = layer;

    std::string assetGuid = guid;
    if (!AssetDatabase::isGuid(assetGuid) || !assetDatabase.getPath(assetGuid).empty()) {
        do {
            assetGuid = AssetDatabase::generateGuid();
        } while (!assetDatabase.getPath(assetGuid).empty());
    }
    prefabAssets.emplace(filepath, std::move(variant));
    assetDatabase.registerAsset(filepath, assetGuid);
    savePrefabToDisk(filepath);
    if (Prefab* saved = getPrefab(filepath); saved && saved->isModified) {
        prefabAssets.erase(filepath);
        return false;
    }
    Out::info("Created Prefab Variant of '%s' at '%s'", base.generic_string().c_str(), filepath.generic_string().c_str());
    return true;
}
