// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Prefab.h"

#include "Stream.h"
#include "util/YamlNumbers.h"

namespace jasperengine::editor {

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
    } catch (const std::exception&) {
        // read as a legacy header: the ids in the file still bound the counter
    }
    return header;
}

Prefab::FileHeader Prefab::loadRegistry(const YAML::Node& node) {
    Entity counter = NULL_ENTITY;
    if (registry) {
        counter = registry->getLastEntity();
        registry->clear();
    } else {
        registry = std::make_unique<EntityRegistry>();
    }
    registryEntities.clear();

    Stream::decodeEntitySelection(node, registry.get(), &registryEntities);

    const FileHeader header = readHeader(node);
    Entity highest = counter;
    for (Entity entity : registryEntities) {
        highest = std::max(highest, entity);
    }
    if (header.nextEntity != NULL_ENTITY) {
        highest = std::max<Entity>(highest, header.nextEntity - 1);
    }
    registry->setLastEntity(highest);
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
    return highest + 1;
}

uint32_t Prefab::getInstanceId(uint32_t sceneId, Entity entity) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			if (instance.rootEntity == entity) {
				return instance.instanceId;
			}

			for (const auto& member : instance.members) {
				if (member.localEntity == entity) {
					return instance.instanceId;
				}
			}
		}
	}

	return 0;
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

Entity Prefab::getLocalEntity(uint32_t sceneId, uint32_t instanceId, Entity registryEntity) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			if (instance.instanceId == instanceId) {
				for (const auto& member : instance.members) {
					if (member.registryEntity == registryEntity) {
						return member.localEntity;
					}
				}
			}
		}
	}
	return NULL_ENTITY;
}

Entity Prefab::getRootEntity(uint32_t sceneId, Entity entity) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			if (instance.rootEntity == entity) {
				return instance.rootEntity;
			}

			for (const auto& member : instance.members) {
				if (member.localEntity == entity) {
					return instance.rootEntity;
				}
			}
		}
	}

	return NULL_ENTITY;
}
bool Prefab::hasInstances(uint32_t sceneId) const {
	auto it = instances.find(sceneId);
	return it != instances.end() && !it->second.empty();
}


std::vector<Entity> Prefab::getAllEntities(uint32_t sceneId, uint32_t instanceId) const {
	auto it = instances.find(sceneId);
	if (it == instances.end()) return {};
	for (const auto& instance : it->second) {
		if (instance.instanceId == instanceId) {
			std::vector<Entity> result;
			result.reserve(1 + instance.members.size());
			result.push_back(instance.rootEntity);
			for (const auto& member : instance.members) {
				result.push_back(member.localEntity);
			}
			return result;
		}
	}
	return {};
}

bool Prefab::containsEntity(uint32_t sceneId, Entity entity) const {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (const auto& instance : it->second) {
			if (instance.rootEntity == entity) {
				return true;
			}
			for (const auto& member : instance.members) {
				if (member.localEntity == entity) {
					return true;
				}
			}
		}
	}
	return false;
}

Prefab::Instance* Prefab::getInstance(uint32_t sceneId, Entity entity) {
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (auto& instance : it->second) {
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
	auto it = instances.find(sceneId);
	if (it != instances.end()) {
		for (auto& instance : it->second) {
			if (instance.instanceId == instanceId) {
				return &instance;
			}
		}
	}
	return nullptr;
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
		uint64_t bit = 1ULL << static_cast<int>(componentType);
		instance->overrides[entity] |= bit;
	}
}

void Prefab::clearComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) {
	Instance* instance = getInstance(sceneId, entity);
	if (instance) {
		auto entityIt = instance->overrides.find(entity);
		if (entityIt != instance->overrides.end()) {
			uint64_t bit = 1ULL << static_cast<int>(componentType);
			entityIt->second &= ~bit;
			if (entityIt->second == 0) {
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
			uint64_t bit = 1ULL << static_cast<int>(componentType);
			return (entityIt->second & bit) != 0;
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
