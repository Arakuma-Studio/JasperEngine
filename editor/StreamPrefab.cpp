// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instance serialization: the per-instance data written next to a prefab root (the
// `prefabInstance` block, see editor/project/PREFABS.md) and its reader, which also reads
// the keys written before it (prefabOverrides, prefabRemovedGameObjects,
// prefabLocalEntities), the decode hook that rebuilds an instance from it, and the
// PrefabComponent codec.

#include "Stream.h"
#include "component/Transform.h"
#include "component/PrefabComponent.h"
#include "Catalog.h"
#include "Out.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {

    using editor::PrefabInstanceData;
    using editor::PrefabObjectOverrides;

    // ---- reading ----

    bool componentTypeOf(const std::string& name, editor::ComponentType& type) {
        try {
            type = editor::Catalog::getComponentType(name);
            return true;
        } catch (const std::exception&) {
            editor::Out::warning("Prefab instance data names an unknown component \"%s\", ignored", name.c_str());
            return false;
        }
    }

    std::set<editor::ComponentType> readComponentList(const YAML::Node& node) {
        std::set<editor::ComponentType> types;
        if (node && node.IsSequence()) {
            for (const YAML::Node& entry : node) {
                editor::ComponentType type;
                if (entry.IsScalar() && componentTypeOf(entry.as<std::string>(), type)) {
                    types.insert(type);
                }
            }
        }
        return types;
    }

    uint8_t stateBit(const std::string& name) {
        if (name == "name") return PrefabObjectOverrides::StateName;
        if (name == "active") return PrefabObjectOverrides::StateActive;
        if (name == "tag") return PrefabObjectOverrides::StateTag;
        if (name == "layer") return PrefabObjectOverrides::StateLayer;
        return 0;
    }

    // One `overrides` entry of a v3 block
    void readObjectEntry(const YAML::Node& entry, PrefabInstanceData& data) {
        if (!entry.IsMap() || !entry["registryEntity"]) {
            return;
        }
        const Entity registryEntity = editor::YamlNumbers::as<Entity>(entry["registryEntity"]);
        PrefabInstanceData::Object& object = data.objects[registryEntity];
        if (const YAML::Node properties = entry["properties"]; properties && properties.IsMap()) {
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                editor::ComponentType type;
                if (!componentTypeOf(it->first.as<std::string>(), type)) {
                    continue;
                }
                // (a copy: `it->second` is a member of the iterator's temporary proxy, which a
                // range-for over it would outlive)
                const YAML::Node names = it->second;
                if (names.IsSequence()) {
                    for (const YAML::Node& name : names) {
                        object.overrides.addProperty(type, name.as<std::string>());
                    }
                } else if (names.IsScalar()) {
                    object.overrides.addProperty(type, names.as<std::string>());
                }
                if (object.overrides.properties[type].empty()) {
                    object.overrides.properties.erase(type);
                }
            }
        }
        if (const YAML::Node state = entry["state"]; state && state.IsSequence()) {
            for (const YAML::Node& name : state) {
                object.overrides.state |= stateBit(name.as<std::string>());
            }
        }
        object.overrides.addedComponents = readComponentList(entry["addedComponents"]);
        object.overrides.removedComponents = readComponentList(entry["removedComponents"]);

        YAML::Node values(YAML::NodeType::Map);
        bool hasValues = false;
        for (const char* key : {"name", "active", "tag", "layer", "components"}) {
            if (entry[key]) {
                values[key] = YAML::Clone(entry[key]);
                hasValues = true;
            }
        }
        if (hasValues) {
            object.values = values;
        }
    }

    void readBlock(const YAML::Node& block, PrefabInstanceData& data) {
        if (!block || !block.IsMap()) {
            return;
        }
        data.present = true;
        if (const YAML::Node ids = block["memberIds"]; ids && ids.IsSequence()) {
            for (const YAML::Node& pair : ids) {
                if (pair.IsSequence() && pair.size() == 2) {
                    data.memberIds[editor::YamlNumbers::as<Entity>(pair[0])] = editor::YamlNumbers::as<Entity>(pair[1]);
                }
            }
        }
        if (const YAML::Node overrides = block["overrides"]; overrides && overrides.IsSequence()) {
            for (const YAML::Node& entry : overrides) {
                readObjectEntry(entry, data);
            }
        }
        if (const YAML::Node removed = block["removedGameObjects"]; removed && removed.IsSequence()) {
            for (const YAML::Node& entry : removed) {
                if (entry.IsScalar()) {
                    data.removedGameObjects.insert(editor::YamlNumbers::as<Entity>(entry));
                }
            }
        }
        if (const YAML::Node added = block["addedGameObjects"]; added && added.IsSequence()) {
            for (const YAML::Node& entry : added) {
                if (entry.IsMap()) {
                    data.addedGameObjects.push_back(YAML::Clone(entry));
                }
            }
        }
        if (const YAML::Node nested = block["nested"]; nested && nested.IsSequence()) {
            for (const YAML::Node& entry : nested) {
                if (!entry.IsMap()) {
                    continue;
                }
                const Entity nestedRoot = entry["prefabRootRegistryEntity"] ?
                    editor::YamlNumbers::as<Entity>(entry["prefabRootRegistryEntity"]) : NULL_ENTITY;
                const std::string path = entry["prefabPath"] ? entry["prefabPath"].as<std::string>() : std::string();
                if (nestedRoot == NULL_ENTITY && path.empty()) {
                    continue;
                }
                readBlock(entry, data.nestedFor(nestedRoot, path));
            }
        }
    }

    // The nested entry a legacy entry tagged with prefabPath/prefabRootRegistryEntity goes to
    PrefabInstanceData& legacyNested(PrefabInstanceData& data, const YAML::Node& entry) {
        const Entity nestedRoot = entry["prefabRootRegistryEntity"] ?
            editor::YamlNumbers::as<Entity>(entry["prefabRootRegistryEntity"]) : NULL_ENTITY;
        return data.nestedFor(nestedRoot, entry["prefabPath"].as<std::string>());
    }

    // Legacy component overrides: the whole of each component listed
    void readLegacyOverride(const YAML::Node& entry, PrefabInstanceData& data) {
        if (!entry["registryEntity"]) {
            return;
        }
        const Entity registryEntity = editor::YamlNumbers::as<Entity>(entry["registryEntity"]);
        PrefabInstanceData::Object& object = data.objects[registryEntity];
        const YAML::Node components = entry["components"];
        if (!components || !components.IsMap()) {
            return;
        }
        if (!object.values) {
            object.values = YAML::Node(YAML::NodeType::Map);
        }
        for (auto it = components.begin(); it != components.end(); ++it) {
            editor::ComponentType type;
            if (!componentTypeOf(it->first.as<std::string>(), type)) {
                continue;
            }
            object.overrides.addWholeComponent(type);
            object.values["components"][it->first.as<std::string>()] = YAML::Clone(it->second);
        }
    }

    // Legacy removed GameObject entries, `nested` holding the entry one level further down
    void readLegacyRemoval(const YAML::Node& entry, PrefabInstanceData& data, size_t depth = 0) {
        if (!entry.IsMap() || depth > 64) {
            return;
        }
        if (entry["prefabPath"]) {
            // entries of the oldest files without the nested root's id were never read
            if (!entry["prefabRootRegistryEntity"]) {
                return;
            }
            PrefabInstanceData& nested = legacyNested(data, entry);
            if (entry["nested"]) {
                readLegacyRemoval(entry["nested"], nested, depth + 1);
            } else if (entry["registryEntity"]) {
                nested.removedGameObjects.insert(editor::YamlNumbers::as<Entity>(entry["registryEntity"]));
            }
        } else if (entry["registryEntity"]) {
            data.removedGameObjects.insert(editor::YamlNumbers::as<Entity>(entry["registryEntity"]));
        }
    }

    // ---- writing ----

    YAML::Node componentList(const std::set<editor::ComponentType>& types) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (editor::ComponentType type : types) {
            list.push_back(editor::Catalog::getComponentName(type, true));
        }
        return list;
    }

    // The keys of one object's overrides (registryEntity first); the values are the caller's
    YAML::Node writeObjectEntry(Entity registryEntity, const PrefabObjectOverrides& overrides) {
        YAML::Node entry;
        entry["registryEntity"] = registryEntity;
        if (!overrides.properties.empty()) {
            YAML::Node properties(YAML::NodeType::Map);
            for (const auto& [type, names] : overrides.properties) {
                YAML::Node list(YAML::NodeType::Sequence);
                for (const std::string& name : names) {
                    list.push_back(name);
                }
                properties[editor::Catalog::getComponentName(type, true)] = list;
            }
            entry["properties"] = properties;
        }
        if (overrides.state != 0) {
            YAML::Node state(YAML::NodeType::Sequence);
            if (overrides.state & PrefabObjectOverrides::StateName) state.push_back("name");
            if (overrides.state & PrefabObjectOverrides::StateActive) state.push_back("active");
            if (overrides.state & PrefabObjectOverrides::StateTag) state.push_back("tag");
            if (overrides.state & PrefabObjectOverrides::StateLayer) state.push_back("layer");
            entry["state"] = state;
        }
        if (!overrides.addedComponents.empty()) {
            entry["addedComponents"] = componentList(overrides.addedComponents);
        }
        if (!overrides.removedComponents.empty()) {
            entry["removedComponents"] = componentList(overrides.removedComponents);
        }
        return entry;
    }

    YAML::Node memberIdList(const std::map<Entity, Entity>& ids) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const auto& [registryEntity, localEntity] : ids) {
            YAML::Node pair(YAML::NodeType::Sequence);
            pair.SetStyle(YAML::EmitterStyle::Flow);
            pair.push_back(registryEntity);
            pair.push_back(localEntity);
            list.push_back(pair);
        }
        return list;
    }

    // The children of `parent` in Transform order that are not in `members`: GameObjects added
    // to the instance below one of its own, written with their parent's registry entity and
    // their index among all its children
    void appendAddedChildren(const editor::Project* project, const editor::SceneProject* sceneProject, const EntityRegistry* registry,
            Entity parent, Entity parentRegistry, const std::unordered_set<Entity>& members, YAML::Node& entries, size_t firstIndex = 0) {
        if (!registry->getSignature(parent).test(registry->getComponentId<Transform>())) {
            return;
        }
        auto transforms = registry->getComponentArray<Transform>();
        size_t childPos = firstIndex;
        for (size_t ti = transforms->getIndex(parent) + 1; ti < transforms->size(); ti++) {
            const Transform& transform = transforms->getComponentFromIndex(ti);
            if (transform.parent != parent) {
                if (transform.parent == NULL_ENTITY) break;
                continue; // a deeper descendant
            }
            const Entity child = transforms->getEntity(ti);
            if (!members.count(child)) {
                YAML::Node node = editor::Stream::encodeEntity(child, registry, project, sceneProject);
                node["parentRegistryEntity"] = parentRegistry;
                node["childIndex"] = childPos;
                entries.push_back(node);
            }
            childPos++;
        }
    }

    // What a live instance holds relative to its prefab, as a `prefabInstance` block (without
    // `version`). The top-level instance's root takes its values from its own node; anything
    // else is written with its values.
    // The GameObjects of an instance and of every instance nested in it, at any depth: the
    // ones a GameObject added below one of them is not
    void collectTreeObjects(const editor::Project* project, const editor::SceneProject* sceneProject, const EntityRegistry* registry,
            const std::filesystem::path& path, const editor::Prefab::Instance& instance, std::unordered_set<Entity>& objects, size_t depth) {
        objects.insert(instance.rootEntity);
        for (const editor::Prefab::EntityMember& member : instance.members) {
            objects.insert(member.localEntity);
        }
        if (depth > 64) {
            return;
        }
        // the instances nested in it, and for a variant its base's at its root
        for (const editor::NestedInstanceLevel& nested : project->getNestedInstanceLevels(sceneProject, path, instance)) {
            if (nested.instance) {
                collectTreeObjects(project, sceneProject, registry, nested.path, *nested.instance, objects, depth + 1);
            }
        }
    }

    // variantBase: the instance is the base level of a variant instance (same root): the
    // GameObjects added below the root are the outermost level's
    YAML::Node encodeLiveInstance(const editor::Project* project, const editor::SceneProject* sceneProject, const EntityRegistry* registry,
            const std::filesystem::path& path, const editor::Prefab::Instance& instance, bool topLevel, size_t depth,
            const std::unordered_set<Entity>& treeObjects, bool variantBase = false) {
        YAML::Node block(YAML::NodeType::Map);

        std::map<Entity, Entity> memberIds;
        for (const editor::Prefab::EntityMember& member : instance.members) {
            memberIds[member.registryEntity] = member.localEntity;
        }
        if (!memberIds.empty()) {
            block["memberIds"] = memberIdList(memberIds);
        }

        // Overrides, by registry entity
        std::map<Entity, std::pair<Entity, const PrefabObjectOverrides*>> byRegistry;
        for (const auto& [localEntity, overrides] : instance.overrides) {
            const Entity registryEntity = instance.getRegistryEntity(localEntity);
            if (registryEntity == NULL_ENTITY || overrides.empty() || !registry->isEntityCreated(localEntity)) {
                continue;
            }
            byRegistry[registryEntity] = {localEntity, &overrides};
        }
        YAML::Node overridesNode(YAML::NodeType::Sequence);
        for (const auto& [registryEntity, pair] : byRegistry) {
            const Entity localEntity = pair.first;
            const PrefabObjectOverrides& overrides = *pair.second;
            YAML::Node entry = writeObjectEntry(registryEntity, overrides);
            if (!topLevel || localEntity != instance.rootEntity) {
                if (overrides.state & PrefabObjectOverrides::StateName) entry["name"] = registry->getEntityName(localEntity);
                if (overrides.state & PrefabObjectOverrides::StateActive) entry["active"] = registry->isEntityActive(localEntity);
                if (overrides.state & PrefabObjectOverrides::StateTag) entry["tag"] = registry->getEntityTag(localEntity);
                if (overrides.state & PrefabObjectOverrides::StateLayer) entry["layer"] = registry->getEntityLayer(localEntity);
                Signature signature;
                for (const auto& [type, names] : overrides.properties) {
                    signature |= editor::Catalog::componentTypeToSignature(registry, type);
                }
                for (editor::ComponentType type : overrides.addedComponents) {
                    signature |= editor::Catalog::componentTypeToSignature(registry, type);
                }
                signature &= registry->getSignature(localEntity);
                if (signature.any()) {
                    YAML::Node components = editor::Stream::encodeComponents(localEntity, registry, signature);
                    if (components.IsMap() && components.size() > 0) {
                        entry["components"] = components;
                    }
                }
            }
            overridesNode.push_back(entry);
        }
        if (overridesNode.size() > 0) {
            block["overrides"] = overridesNode;
        }

        // Prefab GameObjects this instance left out
        if (!instance.removed.empty()) {
            std::vector<Entity> removed;
            for (const auto& [registryEntity, members] : instance.removed) {
                removed.push_back(registryEntity);
            }
            std::sort(removed.begin(), removed.end());
            YAML::Node removedNode(YAML::NodeType::Sequence);
            for (Entity registryEntity : removed) {
                removedNode.push_back(registryEntity);
            }
            block["removedGameObjects"] = removedNode;
        }

        // GameObjects added below the root or a member. The GameObjects of the instances in
        // the tree (nested ones, and the ones a prefab adds to its nested instances) are not
        // added ones; GameObjects added below a nested root belong to the nested instance's
        // block, the ones below a variant instance's root to the variant's (outermost) level.
        const std::unordered_set<Entity>& memberSet = treeObjects;
        const std::vector<editor::NestedInstanceLevel> nestedLevels = project->getNestedInstanceLevels(sceneProject, path, instance);
        std::unordered_set<Entity> nestedRootLocals;
        for (const editor::NestedInstanceLevel& nested : nestedLevels) {
            if (!nested.variantBase) {
                nestedRootLocals.insert(nested.localRoot);
            }
        }
        YAML::Node addedNode(YAML::NodeType::Sequence);
        if (!variantBase) {
            appendAddedChildren(project, sceneProject, registry, instance.rootEntity, instance.rootRegistryEntity, memberSet, addedNode);
        }
        for (const editor::Prefab::EntityMember& member : instance.members) {
            if (nestedRootLocals.count(member.localEntity) || !registry->isEntityCreated(member.localEntity)) {
                continue;
            }
            appendAddedChildren(project, sceneProject, registry, member.localEntity, member.registryEntity, memberSet, addedNode);
        }
        if (addedNode.size() > 0) {
            block["addedGameObjects"] = addedNode;
        }

        // The instances nested here, live or Missing (a variant's base level first, at its root)
        YAML::Node nestedNode(YAML::NodeType::Sequence);
        for (const editor::NestedInstanceLevel& level : nestedLevels) {
            const editor::Prefab::EntityMember memberValue{level.localRoot, level.registryEntity};
            const editor::Prefab::EntityMember* member = &memberValue;
            YAML::Node nestedBlock;
            const editor::Prefab::Instance* nested = level.instance;
            if (nested) {
                if (depth > 64) {
                    continue; // a nesting cycle, which importPrefab refuses anyway
                }
                nestedBlock = encodeLiveInstance(project, sceneProject, registry, level.path, *nested, false, depth + 1, treeObjects, level.variantBase);
            } else if (const editor::MissingPrefabInstance* missing = level.variantBase ? nullptr : project->getMissingPrefab(sceneProject->id, member->localEntity)) {
                // A Missing Prefab nested here keeps what it was loaded with; GameObjects added
                // below its root since go after the ones it kept there
                PrefabInstanceData kept = editor::Stream::decodePrefabInstanceData(missing->data);
                nestedBlock = editor::Stream::encodePrefabInstanceBlock(kept);
                size_t keptAtRoot = 0;
                for (const YAML::Node& entry : kept.addedGameObjects) {
                    if (!entry["parentRegistryEntity"] || editor::YamlNumbers::as<Entity>(entry["parentRegistryEntity"]) == NULL_ENTITY) {
                        keptAtRoot++;
                    }
                }
                YAML::Node added = nestedBlock["addedGameObjects"] ? nestedBlock["addedGameObjects"] : YAML::Node(YAML::NodeType::Sequence);
                appendAddedChildren(project, sceneProject, registry, member->localEntity, NULL_ENTITY, treeObjects, added, keptAtRoot);
                if (added.size() > 0) {
                    nestedBlock["addedGameObjects"] = added;
                }
            } else {
                continue;
            }
            if (nestedBlock.size() == 0) {
                continue;
            }
            YAML::Node entry;
            entry["prefabRootRegistryEntity"] = member->registryEntity;
            entry["prefabPath"] = level.path.generic_string();
            for (auto it = nestedBlock.begin(); it != nestedBlock.end(); ++it) {
                entry[it->first.as<std::string>()] = it->second;
            }
            nestedNode.push_back(entry);
        }
        if (nestedNode.size() > 0) {
            block["nested"] = nestedNode;
        }
        return block;
    }
}

editor::PrefabInstanceData editor::Stream::decodePrefabInstanceData(const YAML::Node& node) {
    PrefabInstanceData data;
    if (!node || !node.IsMap()) {
        return data;
    }
    if (const YAML::Node block = node["prefabInstance"]; block && block.IsMap()) {
        readBlock(block, data);
        // a Missing Prefab record of an instance read from an older file
        data.legacy = block["legacyRoot"] && editor::YamlNumbers::as<bool>(block["legacyRoot"]);
        return data;
    }

    // Written before the `prefabInstance` block: component-level overrides, the instance root
    // a per-instance wrapper
    data.present = true;
    data.legacy = true;
    if (const YAML::Node overrides = node["prefabOverrides"]; overrides && overrides.IsSequence()) {
        for (const YAML::Node& entry : overrides) {
            if (!entry.IsMap()) continue;
            readLegacyOverride(entry, entry["prefabPath"] ? legacyNested(data, entry) : data);
        }
    }
    if (const YAML::Node removed = node["prefabRemovedGameObjects"]; removed && removed.IsSequence()) {
        for (const YAML::Node& entry : removed) {
            readLegacyRemoval(entry, data);
        }
    }
    if (const YAML::Node locals = node["prefabLocalEntities"]; locals && locals.IsSequence()) {
        for (const YAML::Node& entry : locals) {
            if (!entry.IsMap()) continue;
            if (entry["prefabPath"]) {
                YAML::Node clean = YAML::Clone(entry);
                clean.remove("prefabPath");
                clean.remove("prefabRootRegistryEntity");
                legacyNested(data, entry).addedGameObjects.push_back(clean);
            } else {
                data.addedGameObjects.push_back(YAML::Clone(entry));
            }
        }
    }
    return data;
}

YAML::Node editor::Stream::encodePrefabInstanceBlock(const PrefabInstanceData& data) {
    YAML::Node block(YAML::NodeType::Map);
    if (!data.memberIds.empty()) {
        block["memberIds"] = memberIdList(data.memberIds);
    }
    YAML::Node overridesNode(YAML::NodeType::Sequence);
    for (const auto& [registryEntity, object] : data.objects) {
        if (object.overrides.empty()) {
            continue;
        }
        YAML::Node entry = writeObjectEntry(registryEntity, object.overrides);
        if (object.values && object.values.IsMap()) {
            for (auto it = object.values.begin(); it != object.values.end(); ++it) {
                entry[it->first.as<std::string>()] = YAML::Clone(it->second);
            }
        }
        overridesNode.push_back(entry);
    }
    if (overridesNode.size() > 0) {
        block["overrides"] = overridesNode;
    }
    if (!data.removedGameObjects.empty()) {
        YAML::Node removed(YAML::NodeType::Sequence);
        for (Entity registryEntity : data.removedGameObjects) {
            removed.push_back(registryEntity);
        }
        block["removedGameObjects"] = removed;
    }
    if (!data.addedGameObjects.empty()) {
        YAML::Node added(YAML::NodeType::Sequence);
        for (const YAML::Node& entry : data.addedGameObjects) {
            added.push_back(YAML::Clone(entry));
        }
        block["addedGameObjects"] = added;
    }
    YAML::Node nestedNode(YAML::NodeType::Sequence);
    for (const PrefabInstanceData& nested : data.nested) {
        YAML::Node nestedBlock = encodePrefabInstanceBlock(nested);
        if (nestedBlock.size() == 0) {
            continue;
        }
        YAML::Node entry;
        if (nested.nestedRoot != NULL_ENTITY) {
            entry["prefabRootRegistryEntity"] = nested.nestedRoot;
        }
        if (!nested.nestedPath.empty()) {
            entry["prefabPath"] = nested.nestedPath;
        }
        for (auto it = nestedBlock.begin(); it != nestedBlock.end(); ++it) {
            entry[it->first.as<std::string>()] = it->second;
        }
        nestedNode.push_back(entry);
    }
    if (nestedNode.size() > 0) {
        block["nested"] = nestedNode;
    }
    return block;
}

void editor::Stream::encodePrefabInstance(YAML::Node& entityNode, const Entity entity, const EntityRegistry* registry,
        const Project* project, const SceneProject* sceneProject, const Prefab* prefab) {
    auto sceneIt = prefab->instances.find(sceneProject->id);
    if (sceneIt == prefab->instances.end()) {
        return;
    }
    for (const auto& instance : sceneIt->second) {
        if (instance.rootEntity != entity) continue;

        YAML::Node block(YAML::NodeType::Map);
        block["version"] = Prefab::FILE_VERSION;
        std::unordered_set<Entity> treeObjects;
        std::filesystem::path path;
        for (const auto& [candidate, loaded] : project->getPrefabs(sceneProject->id)) {
            if (loaded == prefab) {
                path = candidate;
                break;
            }
        }
        collectTreeObjects(project, sceneProject, registry, path, instance, treeObjects, 0);
        YAML::Node content = encodeLiveInstance(project, sceneProject, registry, path, instance, true, 0, treeObjects);
        for (auto it = content.begin(); it != content.end(); ++it) {
            block[it->first.as<std::string>()] = it->second;
        }
        entityNode["prefabInstance"] = block;
        break;
    }
}

namespace {
    // One map entry of restoreWrittenStyles: an empty value gets its written style, another is walked
    void restoreWrittenStyle(const YAML::Node& key, YAML::Node value) {
        if ((value.IsSequence() || value.IsMap()) && value.size() == 0) {
            // a parsed value comes after its key; a node made in memory has no mark, a clone's
            // marks are all at the start (YAML::Clone does not copy them): both keep their style
            const YAML::Mark keyMark = key.Mark();
            const YAML::Mark written = value.Mark();
            if (!keyMark.is_null() && !written.is_null() && written.pos > keyMark.pos) {
                value.SetStyle(written.line == keyMark.line ? YAML::EmitterStyle::Flow : YAML::EmitterStyle::Default);
            }
            return;
        }
        editor::Stream::restoreWrittenStyles(value);
    }
}

void editor::Stream::restoreWrittenStyles(YAML::Node node) {
    if (node.IsSequence()) {
        for (YAML::Node item : node) {
            restoreWrittenStyles(item);
        }
    } else if (node.IsMap()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            restoreWrittenStyle(it->first, it->second);
        }
    }
}

void editor::Stream::decodePrefabInstance(const YAML::Node& entityNode, Entity entity, EntityRegistry* registry,
        std::vector<Entity>* entities, Project* project, SceneProject* sceneProject,
        std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>& allEntities) {
    // If entity has PrefabComponent, import prefab children from the prefab its GUID names
    if (project && sceneProject) {
        PrefabComponent* prefabComp = registry->findComponent<PrefabComponent>(entity);
        if (prefabComp && (!prefabComp->path.empty() || !prefabComp->guid.empty())) {
            const std::filesystem::path path = prefabComp->path;
            // the data a Missing Prefab copies from here is written again as the file has it
            for (auto it = entityNode.begin(); it != entityNode.end(); ++it) {
                if (it->first.IsScalar() && !isGameObjectKey(it->first.Scalar())) {
                    restoreWrittenStyle(it->first, it->second);
                }
            }
            const PrefabInstanceData data = decodePrefabInstanceData(entityNode);
            std::vector<Entity> prefabEntities = project->importPrefab(
                sceneProject, entities, path, entity, false, &data, entityRemap);
            allEntities.insert(allEntities.end(), prefabEntities.begin(), prefabEntities.end());

            // Missing Prefab: every key the file holds for the instance is kept as written,
            // the ones this version does not know too
            const PrefabComponent* component = registry->findComponent<PrefabComponent>(entity);
            const Prefab* prefab = component ? project->getPrefab(component->path) : nullptr;
            const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneProject->id, entity) : nullptr;
            if (!instance || instance->rootEntity != entity) {
                project->setMissingPrefab(sceneProject, entity, entityNode);
            }
        }
    }
}

bool editor::Stream::isGameObjectKey(const std::string& key) {
    static const std::unordered_set<std::string> keys = {
        "type", "entity", "name", "active", "tag", "layer", "components", "children"
    };
    return keys.count(key) != 0;
}

YAML::Node editor::Stream::encodePrefabInstanceData(const Entity root, const EntityRegistry* registry,
        const Project* project, const SceneProject* sceneProject, const Prefab* prefab) {
    YAML::Node data(YAML::NodeType::Map);
    if (project && sceneProject && prefab) {
        encodePrefabInstance(data, root, registry, project, sceneProject, prefab);
    }
    return data;
}

void editor::Stream::encodeMissingPrefab(YAML::Node& entityNode, Entity entity, const Project* project, const SceneProject* sceneProject) {
    if (!project || !sceneProject || !entityNode.IsMap()) {
        return;
    }
    const YAML::Node& readNode = entityNode;
    if (!readNode["type"]) {
        return;
    }
    const MissingPrefabInstance* missing = project->getMissingPrefab(sceneProject->id, entity);
    if (!missing || !missing->data.IsMap()) {
        return;
    }
    for (auto it = missing->data.begin(); it != missing->data.end(); ++it) {
        const std::string key = it->first.as<std::string>();
        if (isGameObjectKey(key) || readNode[key]) {
            continue;
        }
        entityNode[key] = YAML::Clone(it->second);
    }
}

// ==============================
// PrefabComponent
// ==============================

YAML::Node editor::Stream::encodePrefabComponent(const PrefabComponent& prefab) {
    YAML::Node node;
    node["name"] = prefab.name;
    node["path"] = prefab.path;
    if (!prefab.guid.empty()) {
        node["guid"] = prefab.guid;
    }
    return node;
}

PrefabComponent editor::Stream::decodePrefabComponent(const YAML::Node& node, const PrefabComponent* oldPrefab) {
    PrefabComponent prefab;

    if (oldPrefab) {
        prefab = *oldPrefab;
    }

    if (node["name"]) prefab.name = node["name"].as<std::string>();
    if (node["path"]) prefab.path = node["path"].as<std::string>();
    // A component written before GUIDs gets the one its path resolves to on import
    if (node["guid"]) {
        prefab.guid = node["guid"].as<std::string>();
    } else if (node["path"]) {
        prefab.guid.clear();
    }

    return prefab;
}
