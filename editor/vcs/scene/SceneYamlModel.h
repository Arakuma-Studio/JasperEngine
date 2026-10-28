// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A .scene or .prefab file as the semantic diff and merge see it (docs/specs/
// S29-version-control.md R13): GameObjects by their stable `entity:` id, each with its parent,
// its index among its siblings and its components by YAML key; the document's own keys form the
// pseudo-object "Scene Settings". Nested `children` are flattened; a prefab's `members` are taken
// as they are. The file is first migrated to the current format (S03 StreamMigrations), so a
// baseline written by an older editor compares with what this one writes.
//
// Pure: yaml-cpp, the migrations and the reflection tables' static descriptors (key order, entity
// fields); no engine, no registry, no project. The CLI (merge-scene, scene-diff) and the tests
// use it as it is.

#include "StreamMigrations.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::vcs::scene {

    // The id of the Scene Settings pseudo-object
    constexpr uint64_t kSceneSettingsId = ~uint64_t(0);
    // The component key of a GameObject's own keys (name, active, tag, layer, the unknown ones)
    constexpr const char* kGameObjectKey = "gameObject";
    // The component key of a prefab instance root's instance block
    constexpr const char* kPrefabInstanceKey = "prefabInstance";

    struct SceneObject {
        uint64_t id = 0;
        std::string name;
        uint64_t parentId = 0;              // 0: a root
        int siblingIndex = 0;
        std::string type = "Entity";        // the entity node's `type`
        bool settings = false;              // the Scene Settings pseudo-object
        // by YAML key, in the file's order: the components, kGameObjectKey, kPrefabInstanceKey;
        // for Scene Settings every document key ("scene", "mainCamera", "prefabs"...)
        std::vector<std::pair<std::string, YAML::Node>> components;

        const YAML::Node* component(std::string_view key) const;
        YAML::Node* component(std::string_view key);
    };

    struct SceneYamlModel {
        DocumentKind kind = DocumentKind::Scene;
        int formatVersion = 0;              // after the migration
        MigrationResult migration;
        // by id; Scene Settings is kSceneSettingsId
        std::map<uint64_t, SceneObject> objects;
        // the document's keys in the file's order ("entities"/"members" where the GameObjects go)
        std::vector<std::string> documentKeys;
        // a prefab's `root:` (0 for scenes)
        uint64_t prefabRoot = 0;

        // Parses and migrates; false with *error for text that is no scene or prefab
        static bool parse(std::string_view text, SceneYamlModel& out, std::string* error = nullptr);
        // From a parsed document (cloned first: the node is not changed)
        static bool fromDocument(const YAML::Node& document, SceneYamlModel& out, std::string* error = nullptr);
        // The same for a document nothing else holds (parse's): it is migrated in place, not copied
        static bool fromOwnedDocument(YAML::Node document, SceneYamlModel& out, std::string* error = nullptr);

        // The document again, in the canonical key order of Stream: the document keys, each
        // GameObject's type, entity, name, active, tag, layer, components (in the order a scene
        // writes them), prefabInstance, its other keys and its children
        YAML::Node encode() const;
        // encode() as the text a save writes
        std::string text() const;
        // One GameObject's node as encode() writes it, with the children includeChild accepts
        // (null: all of them)
        YAML::Node entityNode(uint64_t id, const std::function<bool(uint64_t)>& includeChild = nullptr) const;

        const SceneObject* find(uint64_t id) const;
        // The children of a GameObject (0: the roots) in sibling order
        std::vector<uint64_t> childrenOf(uint64_t parentId) const;
        // Every GameObject depth first, in sibling order (Scene Settings first)
        std::vector<uint64_t> treeOrder() const;
        uint64_t maxId() const;
        // a prefab's nextEntity (0 for a scene or when absent)
        uint64_t nextEntity() const;
    };

    // "1001" -> 1001; false for anything that is no unsigned integer
    bool parseId(const YAML::Node& node, uint64_t& out);
    // A value as one line of YAML flow text ("[1, 0, 0]", "{a: 1}", "Player"), "" for no value
    std::string flowString(const YAML::Node& node);
    // Values equal as the diff compares them: maps by keys, sequences in order, numbers within a
    // relative 1e-6, other scalars by text
    bool valuesEqual(const YAML::Node& a, const YAML::Node& b);

}
