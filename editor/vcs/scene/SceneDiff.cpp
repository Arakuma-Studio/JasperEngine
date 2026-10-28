// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/scene/SceneDiff.h"

#include "reflect/Reflect.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace jasperengine::editor::vcs::scene {

namespace {

    // The transform values a save derives from position, rotation, scale and the parent
    // (docs/specs/S29-version-control.md §3, R14); the reflection tables' Derived fields of
    // every component are added to these
    const char* const kDerivedKeys[] = {"worldPosition", "worldRotation", "worldScale", "localMatrix", "modelMatrix", "normalMatrix"};

    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>& derivedKeys() {
        static const auto table = []() {
            std::map<std::string, std::set<std::string, std::less<>>, std::less<>> out;
            for (const char* key : kDerivedKeys) out["transform"].insert(key);
            for (const reflect::ComponentDesc* desc : reflect::all()) {
                for (const reflect::FieldDesc& field : desc->body.fields) {
                    if (field.flags & reflect::Derived) out[desc->key].insert(field.yamlKey ? field.yamlKey : field.name);
                }
            }
            return out;
        }();
        return table;
    }

    std::string joined(const std::string& path, std::string_view key) {
        return path.empty() ? std::string(key) : path + "." + std::string(key);
    }

    bool defined(const YAML::Node& node) {
        return node && node.IsDefined();
    }

    PropertyDiff makeProperty(const std::string& path, const YAML::Node& a, const YAML::Node& b) {
        PropertyDiff diff;
        diff.path = path;
        diff.inBase = defined(a);
        diff.inCurrent = defined(b);
        if (diff.inBase) {
            diff.baseValue = flowString(a);
            diff.baseNode = a;
        }
        if (diff.inCurrent) {
            diff.currentValue = flowString(b);
            diff.currentNode = b;
        }
        return diff;
    }

    // The keys of two maps: a's in order, then b's new ones
    std::vector<std::string> unionKeys(const YAML::Node& a, const YAML::Node& b) {
        std::vector<std::string> keys;
        std::set<std::string> seen;
        for (const YAML::Node* map : {&a, &b}) {
            if (!defined(*map) || !map->IsMap()) continue;
            for (auto it = map->begin(); it != map->end(); ++it) {
                const std::string key = it->first.Scalar();
                if (seen.insert(key).second) keys.push_back(key);
            }
        }
        return keys;
    }

    // the items of a keyed sequence by their key, in order
    std::vector<std::pair<std::string, YAML::Node>> keyedItems(std::string_view field, const YAML::Node& sequence) {
        std::vector<std::pair<std::string, YAML::Node>> items;
        if (!defined(sequence) || !sequence.IsSequence()) return items;
        for (const YAML::Node& item : sequence) {
            std::string key;
            sequenceItemKey(field, item, key);
            items.emplace_back(key, item);
        }
        return items;
    }

    void compareValues(std::string_view component, std::string_view field, const YAML::Node& a, const YAML::Node& b,
                       const std::string& path, bool componentLevel, std::vector<PropertyDiff>& out) {
        if (valuesEqual(a, b)) return;
        const bool maps = defined(a) && defined(b) && a.IsMap() && b.IsMap();
        if (maps) {
            for (const std::string& key : unionKeys(a, b)) {
                if (componentLevel && isDerivedKey(component, key)) continue;
                compareValues(component, key, a[key], b[key], joined(path, key), false, out);
            }
            return;
        }
        const bool sequences = defined(a) && defined(b) && a.IsSequence() && b.IsSequence();
        if (sequences && keyedSequence(field, a) && keyedSequence(field, b)) {
            const auto itemsA = keyedItems(field, a);
            const auto itemsB = keyedItems(field, b);
            std::map<std::string, YAML::Node> byKeyB(itemsB.begin(), itemsB.end());
            std::set<std::string> inA;
            for (const auto& [key, item] : itemsA) {
                inA.insert(key);
                auto it = byKeyB.find(key);
                compareValues(component, std::string_view(), item, it != byKeyB.end() ? it->second : YAML::Node(YAML::NodeType::Undefined),
                              path + "[" + key + "]", false, out);
            }
            for (const auto& [key, item] : itemsB) {
                if (!inA.count(key)) compareValues(component, std::string_view(), YAML::Node(YAML::NodeType::Undefined), item, path + "[" + key + "]", false, out);
            }
            return;
        }
        out.push_back(makeProperty(path, a, b));
    }

    ComponentDiff compareComponent(const std::string& key, const YAML::Node& a, const YAML::Node& b, bool documentLevel) {
        ComponentDiff diff;
        diff.key = key;
        diff.baseNode = a;
        diff.currentNode = b;
        if (documentLevel && isDerivedDocumentKey(key)) return diff;
        if (!defined(a) && !defined(b)) return diff;
        if (!defined(a) || !defined(b)) {
            diff.state = defined(b) ? ChangeState::Added : ChangeState::Removed;
            // what it holds, top-level key by key
            const YAML::Node& present = defined(b) ? b : a;
            if (present.IsMap()) {
                for (auto it = present.begin(); it != present.end(); ++it) {
                    const std::string field = it->first.Scalar();
                    if (isDerivedKey(key, field)) continue;
                    diff.properties.push_back(defined(b) ? makeProperty(field, YAML::Node(YAML::NodeType::Undefined), it->second)
                                                         : makeProperty(field, it->second, YAML::Node(YAML::NodeType::Undefined)));
                }
            } else {
                diff.properties.push_back(makeProperty("", a, b));
            }
            return diff;
        }
        // a document key that is a value itself (mainCamera: 3) or a list compared whole
        if (!(a.IsMap() && b.IsMap())) {
            std::vector<PropertyDiff> properties;
            compareValues(key, key, a, b, "", false, properties);
            diff.properties = std::move(properties);
        } else {
            compareValues(key, key, a, b, "", true, diff.properties);
        }
        if (!diff.properties.empty()) diff.state = ChangeState::Modified;
        return diff;
    }

    // the components of two objects, current's order first
    std::vector<std::string> componentKeys(const SceneObject* a, const SceneObject* b) {
        std::vector<std::string> keys;
        std::set<std::string> seen;
        for (const SceneObject* object : {b, a}) {
            if (!object) continue;
            for (const auto& [key, node] : object->components) {
                if (seen.insert(key).second) keys.push_back(key);
            }
        }
        return keys;
    }

    YAML::Node componentOf(const SceneObject* object, const std::string& key) {
        if (!object) return YAML::Node(YAML::NodeType::Undefined);
        const YAML::Node* node = object->component(key);
        return node ? *node : YAML::Node(YAML::NodeType::Undefined);
    }

    std::map<uint64_t, uint64_t> memberIdsOf(const YAML::Node& block) {
        std::map<uint64_t, uint64_t> ids;
        if (!defined(block) || !block.IsMap()) return ids;
        const YAML::Node list = block["memberIds"];
        if (!defined(list) || !list.IsSequence()) return ids;
        for (const YAML::Node& pair : list) {
            uint64_t registry = 0, local = 0;
            if (pair.IsSequence() && pair.size() == 2 && parseId(pair[0], registry) && parseId(pair[1], local)) ids[registry] = local;
        }
        return ids;
    }

    std::set<uint64_t> idSet(const YAML::Node& list) {
        std::set<uint64_t> ids;
        if (!defined(list) || !list.IsSequence()) return ids;
        for (const YAML::Node& item : list) {
            uint64_t id = 0;
            if (parseId(item, id)) ids.insert(id);
        }
        return ids;
    }

    // The overrides of one instance level (the root's block, or a nested one inside it) as
    // ObjectDiffs of its members; what else the block holds stays the root's
    struct InstanceDiffer {
        uint64_t root;
        std::map<uint64_t, ObjectDiff>& members;

        ObjectDiff& member(uint64_t sceneId, uint64_t registryEntity) {
            ObjectDiff& diff = members[sceneId];
            diff.id = sceneId;
            diff.prefabMember = true;
            diff.instanceRoot = root;
            diff.registryEntity = registryEntity;
            return diff;
        }

        uint64_t sceneIdOf(uint64_t registryEntity, const std::map<uint64_t, uint64_t>& a, const std::map<uint64_t, uint64_t>& b) {
            auto it = b.find(registryEntity);
            if (it != b.end()) return it->second;
            it = a.find(registryEntity);
            if (it != a.end()) return it->second;
            // a member the memberIds do not name: one id per registry entity below the root
            return (uint64_t(1) << 61) + root * 4096 + registryEntity;
        }

        // the block's own keys (version, memberIds...) the root keeps
        void diffLevel(const YAML::Node& a, const YAML::Node& b, std::vector<PropertyDiff>& rootProperties, const std::string& prefix) {
            const auto idsA = memberIdsOf(a);
            const auto idsB = memberIdsOf(b);
            for (const std::string& key : unionKeys(a, b)) {
                const YAML::Node valueA = defined(a) ? a[key] : YAML::Node(YAML::NodeType::Undefined);
                const YAML::Node valueB = defined(b) ? b[key] : YAML::Node(YAML::NodeType::Undefined);
                if (key == "overrides") {
                    diffOverrides(valueA, valueB, idsA, idsB);
                } else if (key == "removedGameObjects") {
                    const std::set<uint64_t> removedA = idSet(valueA);
                    const std::set<uint64_t> removedB = idSet(valueB);
                    for (uint64_t registryEntity : removedB) {
                        if (!removedA.count(registryEntity)) member(sceneIdOf(registryEntity, idsA, idsB), registryEntity).state = ChangeState::Removed;
                    }
                    for (uint64_t registryEntity : removedA) {
                        if (!removedB.count(registryEntity)) member(sceneIdOf(registryEntity, idsA, idsB), registryEntity).state = ChangeState::Added;
                    }
                } else if (key == "nested" && keyedSequence(key, valueA.IsDefined() ? valueA : YAML::Node(YAML::NodeType::Sequence)) &&
                           keyedSequence(key, valueB.IsDefined() ? valueB : YAML::Node(YAML::NodeType::Sequence))) {
                    const auto itemsA = keyedItems(key, valueA);
                    const auto itemsB = keyedItems(key, valueB);
                    std::map<std::string, YAML::Node> nestedA(itemsA.begin(), itemsA.end());
                    std::map<std::string, YAML::Node> nestedB(itemsB.begin(), itemsB.end());
                    std::set<std::string> keys;
                    for (const auto& [k, item] : itemsA) keys.insert(k);
                    for (const auto& [k, item] : itemsB) keys.insert(k);
                    for (const std::string& nestedKey : keys) {
                        auto ia = nestedA.find(nestedKey);
                        auto ib = nestedB.find(nestedKey);
                        diffLevel(ia != nestedA.end() ? ia->second : YAML::Node(YAML::NodeType::Undefined),
                                  ib != nestedB.end() ? ib->second : YAML::Node(YAML::NodeType::Undefined), rootProperties,
                                  prefix + "nested[" + nestedKey + "].");
                    }
                } else {
                    compareValues(kPrefabInstanceKey, key, valueA, valueB, prefix + key, false, rootProperties);
                }
            }
        }

        void diffOverrides(const YAML::Node& a, const YAML::Node& b, const std::map<uint64_t, uint64_t>& idsA, const std::map<uint64_t, uint64_t>& idsB) {
            std::map<uint64_t, YAML::Node> entriesA, entriesB;
            for (const auto* pair : {&a, &b}) {
                if (!defined(*pair) || !pair->IsSequence()) continue;
                for (const YAML::Node& entry : *pair) {
                    uint64_t registryEntity = 0;
                    if (entry.IsMap() && parseId(entry["registryEntity"], registryEntity)) (pair == &a ? entriesA : entriesB)[registryEntity] = entry;
                }
            }
            std::set<uint64_t> registryEntities;
            for (const auto& [id, entry] : entriesA) registryEntities.insert(id);
            for (const auto& [id, entry] : entriesB) registryEntities.insert(id);
            for (uint64_t registryEntity : registryEntities) {
                auto ia = entriesA.find(registryEntity);
                auto ib = entriesB.find(registryEntity);
                const YAML::Node entryA = ia != entriesA.end() ? ia->second : YAML::Node(YAML::NodeType::Undefined);
                const YAML::Node entryB = ib != entriesB.end() ? ib->second : YAML::Node(YAML::NodeType::Undefined);
                if (valuesEqual(entryA, entryB)) continue;
                ObjectDiff& diff = member(sceneIdOf(registryEntity, idsA, idsB), registryEntity);
                if (diff.state == ChangeState::Unchanged) diff.state = ChangeState::Modified;
                for (const YAML::Node* entry : {&entryB, &entryA}) {
                    if (diff.name.empty() && defined(*entry) && (*entry)["name"] && (*entry)["name"].IsScalar()) diff.name = (*entry)["name"].Scalar();
                }
                // the overridden values, component by component
                const YAML::Node componentsA = defined(entryA) ? entryA["components"] : YAML::Node(YAML::NodeType::Undefined);
                const YAML::Node componentsB = defined(entryB) ? entryB["components"] : YAML::Node(YAML::NodeType::Undefined);
                for (const std::string& key : unionKeys(componentsA, componentsB)) {
                    ComponentDiff component = compareComponent(key, defined(componentsA) ? componentsA[key] : YAML::Node(YAML::NodeType::Undefined),
                                                                defined(componentsB) ? componentsB[key] : YAML::Node(YAML::NodeType::Undefined), false);
                    // a component only one side overrides: still the prefab's component, its
                    // values changed (an override added or reverted)
                    if (component.state == ChangeState::Added || component.state == ChangeState::Removed) component.state = ChangeState::Modified;
                    if (component.state != ChangeState::Unchanged) diff.components.push_back(std::move(component));
                }
                // name, active, tag, layer of the member
                YAML::Node ownA(YAML::NodeType::Map), ownB(YAML::NodeType::Map);
                for (const char* key : {"name", "active", "tag", "layer"}) {
                    if (defined(entryA) && entryA[key]) ownA[key] = entryA[key];
                    if (defined(entryB) && entryB[key]) ownB[key] = entryB[key];
                }
                ComponentDiff own = compareComponent(kGameObjectKey, ownA, ownB, false);
                if (own.state != ChangeState::Unchanged) diff.components.push_back(std::move(own));
                // which properties are overridden at all (the Overrides dropdown's list)
                YAML::Node listsA(YAML::NodeType::Map), listsB(YAML::NodeType::Map);
                for (const char* key : {"properties", "state", "addedComponents", "removedComponents"}) {
                    if (defined(entryA) && entryA[key]) listsA[key] = entryA[key];
                    if (defined(entryB) && entryB[key]) listsB[key] = entryB[key];
                }
                ComponentDiff lists = compareComponent("prefabOverrides", listsA, listsB, false);
                if (lists.state != ChangeState::Unchanged) diff.components.push_back(std::move(lists));
            }
        }
    };

    // GameObjects whose relative order among siblings present on both sides changed (those off
    // the longest common subsequence of the two orders)
    std::set<uint64_t> reorderedOf(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
        std::set<uint64_t> moved;
        if (a == b) return moved;
        const size_t n = a.size(), m = b.size();
        if (n > 4096 || m > 4096) {
            // too many siblings for the table: the ones whose rank among them changed
            for (size_t i = 0; i < std::min(n, m); i++) {
                if (a[i] != b[i]) moved.insert(b[i]);
            }
            return moved;
        }
        std::vector<std::vector<uint16_t>> table(n + 1, std::vector<uint16_t>(m + 1, 0));
        for (size_t i = n; i-- > 0;) {
            for (size_t j = m; j-- > 0;) {
                table[i][j] = a[i] == b[j] ? static_cast<uint16_t>(table[i + 1][j + 1] + 1) : std::max(table[i + 1][j], table[i][j + 1]);
            }
        }
        std::set<uint64_t> kept;
        size_t i = 0, j = 0;
        while (i < n && j < m) {
            if (a[i] == b[j]) {
                kept.insert(a[i]);
                i++;
                j++;
            } else if (table[i + 1][j] >= table[i][j + 1]) {
                i++;
            } else {
                j++;
            }
        }
        for (uint64_t id : b) {
            if (!kept.count(id)) moved.insert(id);
        }
        return moved;
    }

}

const char* changeStateName(ChangeState state) {
    switch (state) {
        case ChangeState::Unchanged: return "unchanged";
        case ChangeState::Added: return "added";
        case ChangeState::Removed: return "removed";
        case ChangeState::Modified: return "modified";
    }
    return "unchanged";
}

bool isDerivedKey(std::string_view component, std::string_view key) {
    const auto& table = derivedKeys();
    auto it = table.find(component);
    return it != table.end() && it->second.count(key) != 0;
}

bool isDerivedDocumentKey(std::string_view key) {
    // recomputed from the GameObjects by every save, and the format stamp the migration sets
    return key == "maxValues" || key == "shaderKeys" || key == "formatVersion" || key == "version";
}

bool sequenceItemKey(std::string_view sequence, const YAML::Node& item, std::string& key) {
    auto scalar = [&item](const char* name) -> std::string {
        const YAML::Node value = item[name];
        return value && value.IsScalar() ? value.Scalar() : std::string();
    };
    if (sequence == "memberIds") {
        if (!item.IsSequence() || item.size() != 2 || !item[0].IsScalar()) return false;
        key = item[0].Scalar();
        return true;
    }
    if (!item.IsMap()) return false;
    if (sequence == "scripts") {
        const std::string type = scalar("type");
        const std::string path = scalar("path");
        if (path.empty()) return false;
        key = (type.empty() ? std::string("script") : type) + ":" + path;
        return true;
    }
    const char* field = nullptr;
    if (sequence == "properties") field = "name";
    else if (sequence == "prefabs") field = item["guid"] ? "guid" : "path";
    else if (sequence == "cppScripts") field = "path";
    else if (sequence == "overrides") field = "registryEntity";
    else if (sequence == "nested") field = "prefabRootRegistryEntity";
    else if (sequence == "addedGameObjects") field = "entity";
    if (!field) return false;
    key = scalar(field);
    return !key.empty();
}

bool keyedSequence(std::string_view sequence, const YAML::Node& node) {
    if (!node || !node.IsSequence()) return false;
    std::set<std::string> keys;
    for (const YAML::Node& item : node) {
        std::string key;
        if (!sequenceItemKey(sequence, item, key) || !keys.insert(key).second) return false;
    }
    return true;
}

namespace {

    struct PathStep {
        std::string name;       // a map key
        bool keyed = false;     // then an item of the sequence under it
        std::string key;
    };

    bool parsePath(std::string_view path, std::vector<PathStep>& steps) {
        steps.clear();
        size_t i = 0;
        while (i < path.size()) {
            PathStep step;
            while (i < path.size() && path[i] != '.' && path[i] != '[') step.name.push_back(path[i++]);
            if (step.name.empty()) return false;
            steps.push_back(step);
            while (i < path.size() && path[i] == '[') {
                const size_t close = path.find(']', i);
                if (close == std::string_view::npos) return false;
                PathStep item;
                item.name = steps.back().name;
                item.keyed = true;
                item.key = std::string(path.substr(i + 1, close - i - 1));
                steps.push_back(item);
                i = close + 1;
            }
            if (i < path.size()) {
                if (path[i] != '.') return false;
                i++;
            }
        }
        return !steps.empty();
    }

    // the item of a keyed sequence (an undefined node when none)
    YAML::Node keyedItem(const YAML::Node& sequence, const std::string& field, const std::string& key, size_t* index = nullptr) {
        if (!sequence || !sequence.IsSequence()) return YAML::Node(YAML::NodeType::Undefined);
        for (size_t i = 0; i < sequence.size(); i++) {
            std::string itemKey;
            if (sequenceItemKey(field, sequence[i], itemKey) && itemKey == key) {
                if (index) *index = i;
                return sequence[i];
            }
        }
        return YAML::Node(YAML::NodeType::Undefined);
    }

}

YAML::Node valueAt(const YAML::Node& component, std::string_view path) {
    std::vector<PathStep> steps;
    if (!parsePath(path, steps)) return YAML::Node(YAML::NodeType::Undefined);
    // reset() moves the handle; assigning a node would write into the tree
    YAML::Node node;
    node.reset(component);
    for (const PathStep& step : steps) {
        if (!node || !node.IsDefined()) return YAML::Node(YAML::NodeType::Undefined);
        if (step.keyed) {
            node.reset(keyedItem(node, step.name, step.key));
        } else {
            if (!node.IsMap() || !node[step.name]) return YAML::Node(YAML::NodeType::Undefined);
            const YAML::Node next = node[step.name];
            node.reset(next);
        }
    }
    return node;
}

bool setValueAt(YAML::Node component, std::string_view path, const YAML::Node& value) {
    std::vector<PathStep> steps;
    if (!parsePath(path, steps) || !component.IsMap()) return false;
    const bool removing = !value || !value.IsDefined();
    YAML::Node node;
    node.reset(component);
    for (size_t i = 0; i < steps.size(); i++) {
        const PathStep& step = steps[i];
        const bool last = i + 1 == steps.size();
        if (step.keyed) {
            if (!node.IsSequence()) return false;
            size_t index = 0;
            YAML::Node item = keyedItem(node, step.name, step.key, &index);
            if (last) {
                if (removing) {
                    if (item.IsDefined()) node.remove(index);
                    return true;
                }
                if (item.IsDefined()) node[index] = YAML::Clone(value);
                else node.push_back(YAML::Clone(value));
                return true;
            }
            if (!item.IsDefined()) return !removing ? false : true;
            node.reset(item);
            continue;
        }
        if (!node.IsMap()) return false;
        if (last) {
            if (removing) node.remove(step.name);
            else node[step.name] = YAML::Clone(value);
            return true;
        }
        if (!node[step.name]) {
            if (removing) return true;
            const bool sequenceNext = steps[i + 1].keyed;
            node[step.name] = YAML::Node(sequenceNext ? YAML::NodeType::Sequence : YAML::NodeType::Map);
        }
        YAML::Node next = node[step.name];
        node.reset(next);
    }
    return true;
}

const ComponentDiff* ObjectDiff::component(std::string_view key) const {
    for (const ComponentDiff& diff : components) {
        if (diff.key == key) return &diff;
    }
    return nullptr;
}

const ObjectDiff* SceneDiffResult::find(uint64_t id) const {
    for (const ObjectDiff& diff : objects) {
        if (diff.id == id) return &diff;
    }
    return nullptr;
}

size_t SceneDiffResult::propertyCount() const {
    size_t count = 0;
    for (const ObjectDiff& object : objects) {
        for (const ComponentDiff& component : object.components) count += component.properties.size();
    }
    return count;
}

std::string propertyPath(const ComponentDiff& component, const PropertyDiff& property) {
    if (property.path.empty()) return component.key;
    // an item of a keyed document list: "prefabs[<guid>]"
    return property.path.front() == '[' ? component.key + property.path : component.key + "." + property.path;
}

SceneDiffResult diffScenes(const SceneYamlModel& base, const SceneYamlModel& current, const SceneDiffOptions& options) {
    SceneDiffResult result;
    std::map<uint64_t, ObjectDiff> diffs;
    std::map<uint64_t, std::map<uint64_t, ObjectDiff>> membersByRoot;

    std::set<uint64_t> ids;
    for (const auto& [id, object] : base.objects) ids.insert(id);
    for (const auto& [id, object] : current.objects) ids.insert(id);
    for (uint64_t id : ids) {
        const SceneObject* a = base.find(id);
        const SceneObject* b = current.find(id);
        ObjectDiff diff;
        diff.id = id;
        diff.settings = id == kSceneSettingsId;
        diff.name = b ? b->name : a->name;
        if (a) {
            diff.baseParent = a->parentId;
            diff.baseIndex = a->siblingIndex;
        }
        if (b) {
            diff.currentParent = b->parentId;
            diff.currentIndex = b->siblingIndex;
        }
        diff.state = !a ? ChangeState::Added : !b ? ChangeState::Removed : ChangeState::Unchanged;
        std::vector<PropertyDiff> instanceProperties;
        for (const std::string& key : componentKeys(a, b)) {
            if (key == kPrefabInstanceKey && !diff.settings) {
                InstanceDiffer instance{id, membersByRoot[id]};
                instance.diffLevel(componentOf(a, key), componentOf(b, key), instanceProperties, std::string());
                continue;
            }
            ComponentDiff component = compareComponent(key, componentOf(a, key), componentOf(b, key), diff.settings);
            if (component.state != ChangeState::Unchanged) diff.components.push_back(std::move(component));
        }
        if (!instanceProperties.empty()) {
            ComponentDiff instance;
            instance.key = kPrefabInstanceKey;
            instance.state = ChangeState::Modified;
            instance.baseNode = componentOf(a, kPrefabInstanceKey);
            instance.currentNode = componentOf(b, kPrefabInstanceKey);
            instance.properties = std::move(instanceProperties);
            diff.components.push_back(std::move(instance));
        }
        if (a && b) {
            diff.reparented = !diff.settings && a->parentId != b->parentId;
            if (!diff.components.empty()) diff.state = ChangeState::Modified;
        }
        diffs.emplace(id, std::move(diff));
    }

    // Reordered: among the siblings both sides have under one parent
    std::set<uint64_t> parents;
    for (const auto& [id, object] : current.objects) {
        if (!object.settings) parents.insert(object.parentId);
    }
    // each side's children by parent, in sibling order (one pass, not one per parent)
    auto childrenByParent = [](const SceneYamlModel& model) {
        std::map<uint64_t, std::vector<std::pair<int, uint64_t>>> out;
        for (const auto& [id, object] : model.objects) {
            if (!object.settings) out[object.parentId].emplace_back(object.siblingIndex, id);
        }
        for (auto& [parent, children] : out) std::sort(children.begin(), children.end());
        return out;
    };
    const auto childrenA = childrenByParent(base);
    const auto childrenB = childrenByParent(current);
    const std::vector<std::pair<int, uint64_t>> none;
    for (uint64_t parent : parents) {
        if (parent != 0 && (!base.find(parent) || !current.find(parent))) continue;
        auto ia = childrenA.find(parent);
        auto ib = childrenB.find(parent);
        std::vector<uint64_t> orderA, orderB;
        for (const auto& [index, id] : ia != childrenA.end() ? ia->second : none) {
            const SceneObject* other = current.find(id);
            if (other && other->parentId == parent) orderA.push_back(id);
        }
        for (const auto& [index, id] : ib != childrenB.end() ? ib->second : none) {
            const SceneObject* other = base.find(id);
            if (other && other->parentId == parent) orderB.push_back(id);
        }
        for (uint64_t id : reorderedOf(orderA, orderB)) diffs[id].reordered = true;
    }

    auto emit = [&](ObjectDiff& diff) {
        if (options.changedOnly && !diff.changed()) return;
        if (diff.state == ChangeState::Added) result.added++;
        else if (diff.state == ChangeState::Removed) result.removed++;
        else if (diff.changed()) result.modified++;
        result.objects.push_back(std::move(diff));
    };
    auto emitMembers = [&](uint64_t root) {
        auto it = membersByRoot.find(root);
        if (it == membersByRoot.end()) return;
        std::vector<ObjectDiff*> members;
        for (auto& [id, member] : it->second) members.push_back(&member);
        std::sort(members.begin(), members.end(), [](const ObjectDiff* x, const ObjectDiff* y) { return x->registryEntity < y->registryEntity; });
        for (ObjectDiff* member : members) emit(*member);
        membersByRoot.erase(it);
    };
    // an override of a GameObject the file holds itself (the instance root): its diff
    for (auto& [root, members] : membersByRoot) {
        for (auto it = members.begin(); it != members.end();) {
            auto own = diffs.find(it->first);
            if (own == diffs.end() || own->second.prefabMember) {
                ++it;
                continue;
            }
            for (ComponentDiff& component : it->second.components) own->second.components.push_back(std::move(component));
            if (own->second.state == ChangeState::Unchanged && !own->second.components.empty()) own->second.state = ChangeState::Modified;
            it = members.erase(it);
        }
    }
    std::set<uint64_t> emitted;
    for (uint64_t id : current.treeOrder()) {
        emitted.insert(id);
        emit(diffs[id]);
        emitMembers(id);
    }
    for (uint64_t id : base.treeOrder()) {
        if (!emitted.insert(id).second) continue;
        emit(diffs[id]);
        emitMembers(id);
    }
    return result;
}

}
