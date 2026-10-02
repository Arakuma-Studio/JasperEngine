// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Prefab.h"

#include "Stream.h"
#include "util/YamlNumbers.h"

#include <unordered_set>

namespace jasperengine::editor {

// ---- PrefabObjectOverrides ----

bool PrefabObjectOverrides::hasProperty(ComponentType type, const std::string& property) const {
    auto it = properties.find(type);
    return it != properties.end() && (it->second.count(property) || it->second.count(WHOLE_COMPONENT));
}

bool PrefabObjectOverrides::isWholeComponent(ComponentType type) const {
    auto it = properties.find(type);
    return it != properties.end() && it->second.count(WHOLE_COMPONENT);
}

void PrefabObjectOverrides::addProperty(ComponentType type, const std::string& property) {
    if (property.empty()) {
        return;
    }
    if (property == WHOLE_COMPONENT) {
        addWholeComponent(type);
        return;
    }
    std::set<std::string>& set = properties[type];
    if (!set.count(WHOLE_COMPONENT)) {
        set.insert(property);
    }
}

void PrefabObjectOverrides::addWholeComponent(ComponentType type) {
    std::set<std::string>& set = properties[type];
    set.clear();
    set.insert(WHOLE_COMPONENT);
}

void PrefabObjectOverrides::merge(const PrefabObjectOverrides& other) {
    for (const auto& [type, names] : other.properties) {
        for (const std::string& name : names) {
            addProperty(type, name);
        }
    }
    state |= other.state;
    addedComponents.insert(other.addedComponents.begin(), other.addedComponents.end());
    for (ComponentType type : other.removedComponents) {
        if (!addedComponents.count(type)) {
            removedComponents.insert(type);
        }
    }
}

// ---- PrefabInstanceData ----

const PrefabInstanceData* PrefabInstanceData::findNested(Entity registryRoot, const std::string& path) const {
    if (registryRoot != NULL_ENTITY) {
        for (const PrefabInstanceData& entry : nested) {
            if (entry.nestedRoot == registryRoot) {
                return &entry;
            }
        }
    }
    // the oldest entries name the nested prefab by its path only
    if (!path.empty()) {
        for (const PrefabInstanceData& entry : nested) {
            if (entry.nestedRoot == NULL_ENTITY && entry.nestedPath == path) {
                return &entry;
            }
        }
    }
    return nullptr;
}

PrefabInstanceData* PrefabInstanceData::findNested(Entity registryRoot, const std::string& path) {
    return const_cast<PrefabInstanceData*>(static_cast<const PrefabInstanceData*>(this)->findNested(registryRoot, path));
}

PrefabInstanceData& PrefabInstanceData::nestedFor(Entity registryRoot, const std::string& path) {
    if (PrefabInstanceData* found = findNested(registryRoot, path)) {
        if (found->nestedRoot == NULL_ENTITY) {
            found->nestedRoot = registryRoot;
        }
        return *found;
    }
    PrefabInstanceData entry;
    entry.present = true;
    entry.nestedRoot = registryRoot;
    entry.nestedPath = path;
    nested.push_back(std::move(entry));
    return nested.back();
}

bool PrefabInstanceData::empty() const {
    for (const auto& [registryEntity, object] : objects) {
        if (!object.overrides.empty()) {
            return false;
        }
    }
    if (!removedGameObjects.empty() || !addedGameObjects.empty()) {
        return false;
    }
    for (const PrefabInstanceData& entry : nested) {
        if (!entry.empty()) {
            return false;
        }
    }
    return true;
}

// ---- Prefab ----

Prefab::FileHeader Prefab::readHeader(const YAML::Node& node) {
    FileHeader header;
    if (!node || !node.IsMap()) {
        return header;
    }
    try {
        if (node["version"]) {
            header.version = YamlNumbers::as<int>(node["version"]);
        }
        if (node["guid"]) {
            header.guid = node["guid"].as<std::string>();
        }
        if (node["nextEntity"]) {
            header.nextEntity = YamlNumbers::as<Entity>(node["nextEntity"]);
        }
        if (node["root"]) {
            header.root = YamlNumbers::as<Entity>(node["root"]);
        }
    } catch (const std::exception&) {
        // read as a legacy header: the ids in the file still bound the counter
    }
    return header;
}

namespace {

    // Every entity node of a prefab file: the members and their children
    template<typename Fn>
    void forEachPrefabNode(const YAML::Node& node, Fn&& fn) {
        if (!node || !node.IsMap()) {
            return;
        }
        if (node["members"] && node["members"].IsSequence()) {
            for (const YAML::Node& member : node["members"]) {
                forEachPrefabNode(member, fn);
            }
            return;
        }
        fn(node);
        if (node["children"] && node["children"].IsSequence()) {
            for (const YAML::Node& child : node["children"]) {
                forEachPrefabNode(child, fn);
            }
        }
    }

}

Prefab::FileHeader Prefab::loadRegistry(const YAML::Node& node, const std::string& legacyRootName) {
    Entity counter = NULL_ENTITY;
    if (registry) {
        counter = registry->getLastEntity();
        registry->clear();
    } else {
        registry = std::make_unique<EntityRegistry>();
    }
    registryEntities.clear();
    nestedInstances.clear();
    stripped.clear();
    addedObjects.clear();
    root = NULL_ENTITY;

    Stream::decodeEntitySelection(node, registry.get(), &registryEntities);

    FileHeader header = readHeader(node);
    const bool legacyFile = header.version < FILE_VERSION;
    needsUpgrade = legacyFile;

    // Stripped objects: references into the nested instances
    if (node && node.IsMap() && node["stripped"] && node["stripped"].IsSequence()) {
        for (const YAML::Node& entry : node["stripped"]) {
            if (!entry.IsMap() || !entry["entity"] || !entry["source"] || !entry["source"].IsSequence()) {
                continue;
            }
            std::vector<Entity> source;
            for (const YAML::Node& id : entry["source"]) {
                source.push_back(YamlNumbers::as<Entity>(id));
            }
            const Entity id = YamlNumbers::as<Entity>(entry["entity"]);
            if (source.size() >= 2 && id != NULL_ENTITY && !registry->isEntityCreated(id)) {
                stripped[id] = std::move(source);
            }
        }
    }

    Entity highest = counter;
    for (Entity entity : registryEntities) {
        highest = std::max(highest, entity);
    }
    for (const auto& [id, source] : stripped) {
        highest = std::max(highest, id);
    }
    if (header.nextEntity != NULL_ENTITY) {
        highest = std::max<Entity>(highest, header.nextEntity - 1);
    }
    registry->setLastEntity(highest);

    if (legacyFile) {
        // Files older than version 3 held copies of the nested prefabs' GameObjects below
        // their nested roots: the nested import brings the real ones
        std::unordered_set<Entity> nestedRoots;
        for (Entity entity : registryEntities) {
            const PrefabComponent* component = registry->findComponent<PrefabComponent>(entity);
            if (component && (!component->path.empty() || !component->guid.empty())) {
                nestedRoots.insert(entity);
            }
        }
        std::vector<Entity> stale;
        for (Entity entity : registryEntities) {
            const Transform* transform = registry->findComponent<Transform>(entity);
            for (Entity ancestor = transform ? transform->parent : NULL_ENTITY, depth = 0;
                    ancestor != NULL_ENTITY && depth <= registryEntities.size(); depth++) {
                if (nestedRoots.count(ancestor)) {
                    stale.push_back(entity);
                    break;
                }
                const Transform* parentTransform = registry->findComponent<Transform>(ancestor);
                ancestor = parentTransform ? parentTransform->parent : NULL_ENTITY;
            }
        }
        // children first
        for (auto it = stale.rbegin(); it != stale.rend(); ++it) {
            registry->destroyEntity(*it);
        }
        const std::unordered_set<Entity> staleSet(stale.begin(), stale.end());
        registryEntities.erase(std::remove_if(registryEntities.begin(), registryEntities.end(),
            [&staleSet](Entity entity) { return staleSet.count(entity) != 0; }), registryEntities.end());
    }

    // The asset-level data of the nested instances. A legacy file has none: its nested
    // roots' own values are the old wrappers', made into overrides when first imported.
    const std::string prefabKey = Catalog::getComponentName(ComponentType::PrefabComponent, true);
    forEachPrefabNode(node, [&](const YAML::Node& entityNode) {
        // a GameObject this prefab adds to a nested instance, below one of its GameObjects
        if (!legacyFile && entityNode["entity"] && (entityNode["addedTo"] || entityNode["childIndex"])) {
            const Entity id = YamlNumbers::as<Entity>(entityNode["entity"]);
            if (registry->isEntityCreated(id)) {
                AddedObject& added = addedObjects[id];
                if (entityNode["addedTo"]) added.parent = YamlNumbers::as<Entity>(entityNode["addedTo"]);
                if (entityNode["childIndex"]) added.childIndex = YamlNumbers::as<int>(entityNode["childIndex"]);
            }
        }
        const YAML::Node components = entityNode["components"];
        if (!entityNode["entity"] || !components || !components.IsMap() || !components[prefabKey]) {
            return;
        }
        const Entity id = YamlNumbers::as<Entity>(entityNode["entity"]);
        if (!registry->isEntityCreated(id)) {
            return;
        }
        PrefabInstanceData data;
        if (!legacyFile && entityNode["prefabInstance"]) {
            data = Stream::decodePrefabInstanceData(entityNode);
        }
        data.present = true;
        data.legacy = legacyFile;
        nestedInstances[id] = std::move(data);
    });

    std::vector<Entity> topLevel;
    for (Entity entity : registryEntities) {
        const Transform* transform = registry->findComponent<Transform>(entity);
        if (!transform || transform->parent == NULL_ENTITY) {
            topLevel.push_back(entity);
        }
    }

    if (!legacyFile) {
        if (header.root != NULL_ENTITY && registry->isEntityCreated(header.root)) {
            root = header.root;
        } else {
            for (Entity entity : topLevel) {
                if (registry->findComponent<Transform>(entity)) {
                    root = entity;
                    break;
                }
            }
            if (root == NULL_ENTITY && !topLevel.empty()) {
                root = topLevel.front();
            }
        }
        if (root != NULL_ENTITY) {
            // the root comes first, as the file writes it
            auto it = std::find(registryEntities.begin(), registryEntities.end(), root);
            if (it != registryEntities.end() && it != registryEntities.begin()) {
                registryEntities.erase(it);
                registryEntities.insert(registryEntities.begin(), root);
            }
        }
        return header;
    }

    // Upgrade: the old per-instance wrapper becomes the prefab's root GameObject. It takes
    // the next id, which no scene can have used for anything of this prefab, the same on
    // every load until the upgraded file is written.
    root = registry->createUserEntity();
    registry->setEntityName(root, legacyRootName.empty() ? std::string("Prefab") : legacyRootName);
    bool anyTransform = false;
    for (Entity entity : topLevel) {
        if (registry->findComponent<Transform>(entity)) {
            anyTransform = true;
            break;
        }
    }
    if (anyTransform) {
        registry->addComponent<Transform>(root, {});
        for (Entity entity : topLevel) {
            if (registry->findComponent<Transform>(entity)) {
                registry->addEntityChild(root, entity, false);
            }
        }
    }
    registryEntities.insert(registryEntities.begin(), root);
    return header;
}

Entity Prefab::getNextEntity() const {
    if (!registry) {
        return NULL_ENTITY;
    }
    Entity highest = registry->getLastEntity();
    for (Entity entity : registryEntities) {
        highest = std::max(highest, entity);
    }
    for (const auto& [id, source] : stripped) {
        highest = std::max(highest, id);
    }
    return highest + 1;
}

Entity Prefab::allocateId() {
    if (!registry) {
        registry = std::make_unique<EntityRegistry>();
    }
    const Entity id = getNextEntity();
    registry->setLastEntity(id);
    return id;
}

Entity Prefab::findStripped(const std::vector<Entity>& source) const {
    for (const auto& [id, way] : stripped) {
        if (way == source) {
            return id;
        }
    }
    return NULL_ENTITY;
}

Entity Prefab::addStripped(const std::vector<Entity>& source) {
    if (source.size() < 2) {
        return NULL_ENTITY;
    }
    Entity id = findStripped(source);
    if (id == NULL_ENTITY) {
        id = allocateId();
        stripped[id] = source;
    }
    return id;
}

Entity Prefab::getAddedObjectNestedRoot(Entity registryEntity) const {
    if (!registry || registryEntity == NULL_ENTITY || !registry->isEntityCreated(registryEntity) || isInstanceRoot(registryEntity)) {
        return NULL_ENTITY;
    }
    Entity current = getRegistryParent(registryEntity);
    for (size_t depth = 0; current != NULL_ENTITY && depth <= registryEntities.size(); depth++) {
        if (isInstanceRoot(current)) {
            return current;
        }
        current = getRegistryParent(current);
    }
    return NULL_ENTITY;
}

std::vector<Entity> Prefab::getAddedObjectTops(Entity nestedRoot) const {
    std::vector<Entity> tops;
    if (!registry || !isInstanceRoot(nestedRoot)) {
        return tops;
    }
    auto transforms = registry->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        if (transforms->getComponentFromIndex(i).parent == nestedRoot) {
            tops.push_back(transforms->getEntity(i));
        }
    }
    return tops;
}

void Prefab::takeAssetData(Prefab&& loaded) {
    const Entity counter = registry ? registry->getLastEntity() : NULL_ENTITY;
    registry = std::move(loaded.registry);
    if (registry && registry->getLastEntity() < counter) {
        registry->setLastEntity(counter);
    }
    registryEntities = std::move(loaded.registryEntities);
    root = loaded.root;
    nestedInstances = std::move(loaded.nestedInstances);
    stripped = std::move(loaded.stripped);
    addedObjects = std::move(loaded.addedObjects);
    needsUpgrade = loaded.needsUpgrade;
}

Entity Prefab::Instance::getRegistryEntity(Entity localEntity) const {
    if (localEntity == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    if (localEntity == rootEntity) {
        return rootRegistryEntity;
    }
    for (const EntityMember& member : members) {
        if (member.localEntity == localEntity) {
            return member.registryEntity;
        }
    }
    return NULL_ENTITY;
}

Entity Prefab::Instance::getLocalEntity(Entity registryEntity) const {
    if (registryEntity == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    if (registryEntity == rootRegistryEntity) {
        return rootEntity;
    }
    for (const EntityMember& member : members) {
        if (member.registryEntity == registryEntity) {
            return member.localEntity;
        }
    }
    return NULL_ENTITY;
}

const Prefab::ObjectOverrides* Prefab::Instance::findOverrides(Entity localEntity) const {
    auto it = overrides.find(localEntity);
    return it != overrides.end() ? &it->second : nullptr;
}

uint32_t Prefab::getInstanceId(uint32_t sceneId, Entity entity) const {
	const Instance* instance = getInstance(sceneId, entity);
	return instance ? instance->instanceId : 0;
}

Entity Prefab::getRegistryEntity(uint32_t sceneId, Entity entity) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			for (const auto& member : instance.members) {
				if (member.localEntity == entity) {
					return member.registryEntity;
				}
			}
		}
	}

	return NULL_ENTITY;
}

Entity Prefab::getObjectRegistryEntity(uint32_t sceneId, Entity entity) const {
	const Instance* instance = getInstance(sceneId, entity);
	return instance ? instance->getRegistryEntity(entity) : NULL_ENTITY;
}

Entity Prefab::getLocalEntity(uint32_t sceneId, uint32_t instanceId, Entity registryEntity) const {
	const Instance* instance = getInstanceById(sceneId, instanceId);
	return instance ? instance->getLocalEntity(registryEntity) : NULL_ENTITY;
}

Entity Prefab::getRootEntity(uint32_t sceneId, Entity entity) const {
	const Instance* instance = getInstance(sceneId, entity);
	return instance ? instance->rootEntity : NULL_ENTITY;
}

bool Prefab::hasInstances(uint32_t sceneId) const {
	auto it = instances.find(sceneId);
	return it != instances.end() && !it->second.empty();
}

std::vector<Entity> Prefab::getAllEntities(uint32_t sceneId, uint32_t instanceId) const {
	const Instance* instance = getInstanceById(sceneId, instanceId);
	if (!instance) return {};
	std::vector<Entity> result;
	result.reserve(1 + instance->members.size());
	result.push_back(instance->rootEntity);
	for (const auto& member : instance->members) {
		result.push_back(member.localEntity);
	}
	return result;
}

bool Prefab::containsEntity(uint32_t sceneId, Entity entity) const {
	return getInstance(sceneId, entity) != nullptr;
}

Prefab::Instance* Prefab::getInstance(uint32_t sceneId, Entity entity) {
	return const_cast<Instance*>(static_cast<const Prefab*>(this)->getInstance(sceneId, entity));
}

const Prefab::Instance* Prefab::getInstance(uint32_t sceneId, Entity entity) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			if (instance.rootEntity == entity) {
				return &instance;
			}
			for (const auto& member : instance.members) {
				if (member.localEntity == entity) {
					return &instance;
				}
			}
		}
	}
	return nullptr;
}

Prefab::Instance* Prefab::getInstanceById(uint32_t sceneId, uint32_t instanceId) {
	return const_cast<Instance*>(static_cast<const Prefab*>(this)->getInstanceById(sceneId, instanceId));
}

const Prefab::Instance* Prefab::getInstanceById(uint32_t sceneId, uint32_t instanceId) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			if (instance.instanceId == instanceId) {
				return &instance;
			}
		}
	}
	return nullptr;
}

void Prefab::setComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) {
	Instance* instance = getInstance(sceneId, entity);
	if (instance) {
		instance->overrides[entity].addWholeComponent(componentType);
	}
}

void Prefab::clearComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) {
	Instance* instance = getInstance(sceneId, entity);
	if (instance) {
		auto entityIt = instance->overrides.find(entity);
		if (entityIt != instance->overrides.end()) {
			entityIt->second.properties.erase(componentType);
			if (entityIt->second.empty()) {
				instance->overrides.erase(entityIt);
			}
		}
	}
}

bool Prefab::hasComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) const {
	const Instance* instance = getInstance(sceneId, entity);
	if (instance) {
		auto entityIt = instance->overrides.find(entity);
		if (entityIt != instance->overrides.end()) {
			return entityIt->second.hasComponent(componentType);
		}
	}
	return false;
}

Entity Prefab::getRegistryParent(Entity registryEntity) const {
	if (!registry || registryEntity == NULL_ENTITY || !registry->isEntityCreated(registryEntity)) {
		return NULL_ENTITY;
	}
	const Transform* transform = registry->findComponent<Transform>(registryEntity);
	return transform ? transform->parent : NULL_ENTITY;
}

bool Prefab::isNestedRoot(Entity registryEntity) const {
	if (!registry || registryEntity == NULL_ENTITY || registryEntity == root || !registry->isEntityCreated(registryEntity)) {
		return false;
	}
	const PrefabComponent* component = registry->findComponent<PrefabComponent>(registryEntity);
	return component && (!component->path.empty() || !component->guid.empty());
}

bool Prefab::isVariant() const {
	return getBaseComponent() != nullptr;
}

const PrefabComponent* Prefab::getBaseComponent() const {
	if (!registry || root == NULL_ENTITY || !registry->isEntityCreated(root)) {
		return nullptr;
	}
	const PrefabComponent* component = registry->findComponent<PrefabComponent>(root);
	return component && (!component->path.empty() || !component->guid.empty()) ? component : nullptr;
}

bool Prefab::isInstanceRoot(Entity registryEntity) const {
	return isNestedRoot(registryEntity) || (registryEntity != NULL_ENTITY && registryEntity == root && isVariant());
}

bool Prefab::isRegistryEntityAbsent(const Instance& instance, Entity registryEntity) const {
	if (instance.removed.empty() || registryEntity == NULL_ENTITY) {
		return false;
	}
	// The registry hierarchy is acyclic; the bound only guards a corrupt one
	Entity current = registryEntity;
	for (size_t depth = 0; current != NULL_ENTITY && depth <= registryEntities.size(); depth++) {
		if (instance.removed.count(current)) {
			return true;
		}
		current = getRegistryParent(current);
	}
	return false;
}

} // namespace jasperengine::editor

namespace jasperengine::editor {

Prefab::Instance* Prefab::getInstanceByRoot(uint32_t sceneId, Entity root) {
	auto it = instances.find(sceneId);
	if (it == instances.end() || root == NULL_ENTITY) {
		return nullptr;
	}
	for (Instance& instance : it->second) {
		if (instance.rootEntity == root) {
			return &instance;
		}
	}
	return nullptr;
}

} // namespace jasperengine::editor
