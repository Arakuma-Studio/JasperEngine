// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The result model follows OpenEngine's SceneDiff (Apps/Editor/Include/VersionControl/SceneDiff.h,
// MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors; NOTICE.md): objects aligned
// by stable id with their properties, reparenting detected on its own. Ours compares yaml-cpp
// nodes, flattens nested children and keys sequences.

#pragma once

// The semantic diff of a scene or prefab against its baseline (docs/specs/S29-version-control.md
// R14): GameObjects Added / Removed / Modified, Reparented, Reordered; components Added / Removed /
// Modified; properties with their baseline and current values as YAML flow text. Sequences of maps
// are keyed (scripts by type and path, script properties by name, prefabs by guid, a prefab
// instance's overrides, nested instances and added GameObjects by their registry ids); other
// sequences compare whole. Numbers compare within a relative 1e-6. The derived transform values
// (the reflection tables' Derived fields: worldPosition, worldRotation, worldScale, localMatrix,
// modelMatrix, normalMatrix) and the document's derived keys (maxValues, shaderKeys) are not
// compared. A prefab instance's member overrides show on the member GameObjects (by the scene ids
// of memberIds), so a member's property change reads like any GameObject's.

#include "vcs/scene/SceneYamlModel.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::vcs::scene {

    enum class ChangeState : uint8_t { Unchanged, Added, Removed, Modified };
    const char* changeStateName(ChangeState state);    // "unchanged", "added", "removed", "modified"

    struct PropertyDiff {
        std::string path;                   // below the component: "position", "scripts[lua:Enemy.lua].properties[speed].value"
        std::string baseValue;              // YAML flow text ("" when inBase is false)
        std::string currentValue;
        bool inBase = true;
        bool inCurrent = true;
        YAML::Node baseNode;
        YAML::Node currentNode;
    };

    struct ComponentDiff {
        std::string key;                    // "transform", "script", gameObject, "prefabOverrides"...
        ChangeState state = ChangeState::Unchanged;
        std::vector<PropertyDiff> properties;
        YAML::Node baseNode;                // the whole component on each side (undefined when absent)
        YAML::Node currentNode;
    };

    struct ObjectDiff {
        uint64_t id = 0;
        std::string name;
        ChangeState state = ChangeState::Unchanged;
        bool reparented = false;
        uint64_t baseParent = 0;
        uint64_t currentParent = 0;
        bool reordered = false;
        int baseIndex = 0;
        int currentIndex = 0;
        bool settings = false;              // Scene Settings
        // A member of a prefab instance (its values are the instance's overrides)
        bool prefabMember = false;
        uint64_t instanceRoot = 0;
        uint64_t registryEntity = 0;
        std::vector<ComponentDiff> components;

        bool changed() const { return state != ChangeState::Unchanged || reparented || reordered; }
        const ComponentDiff* component(std::string_view key) const;
    };

    struct SceneDiffOptions {
        bool changedOnly = true;            // leave out the GameObjects that did not change
    };

    struct SceneDiffResult {
        std::vector<ObjectDiff> objects;    // Scene Settings, then the current tree's order, then the removed
        size_t added = 0;
        size_t removed = 0;
        size_t modified = 0;                // modified, reparented or reordered

        const ObjectDiff* find(uint64_t id) const;
        bool empty() const { return added == 0 && removed == 0 && modified == 0; }
        size_t propertyCount() const;
    };

    SceneDiffResult diffScenes(const SceneYamlModel& base, const SceneYamlModel& current, const SceneDiffOptions& options = {});

    // "transform.position", "script.scripts[lua:Enemy.lua].properties[speed].value"; the component
    // key alone for a whole value
    std::string propertyPath(const ComponentDiff& component, const PropertyDiff& property);

    // The keys a component's diff and merge leave alone (its Derived fields), and the document's
    bool isDerivedKey(std::string_view component, std::string_view key);
    bool isDerivedDocumentKey(std::string_view key);

    // R14: the key of one item of a keyed sequence ("lua:Enemy.lua" for a script, the name of a
    // script property, a prefab's guid...); false when that sequence is compared whole
    bool sequenceItemKey(std::string_view sequence, const YAML::Node& item, std::string& key);
    // Every item of the sequence has a key, and no two the same
    bool keyedSequence(std::string_view sequence, const YAML::Node& node);

    // The value at a property path below a component's node ("position",
    // "scripts[lua:Enemy.lua].properties[speed].value"); undefined when it has none
    YAML::Node valueAt(const YAML::Node& component, std::string_view path);
    // Sets the value at a path (an undefined value removes it), making the maps and keyed items
    // on the way; false for a path that does not fit the node
    bool setValueAt(YAML::Node component, std::string_view path, const YAML::Node& value);

}
