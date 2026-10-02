// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include <cstdint>
#include <filesystem>
#include <vector>
#include "yaml-cpp/yaml.h"

namespace fs = std::filesystem;

namespace jasperengine::editor{

    // Creates prefabs from GameObjects, one per root GameObject (Unity's way when several are
    // dragged into the Project window), as one undo step. Each selected root becomes the root
    // of its prefab and of the instance it turns into. A root that is a prefab instance root
    // is unpacked first (Unity's "Original Prefab"): the new prefab holds its GameObjects,
    // its nested instances staying nested.
    class CreatePrefabCmd: public Command{

    private:
        struct Item {
            fs::path filepath;
            YAML::Node prefabNode;   // the GameObject (with GameObjects without a Transform)
            std::string guid;        // the asset GUID the first execute gave the file, kept by a redo
            bool created = false;
            // An instance root selected: its unpack, put back by the undo
            std::vector<PrefabUnpackRecord> unpacked;
            // The prefab instances nested in the selection, as they were (their overrides
            // become the new prefab's)
            std::vector<std::pair<Entity, YAML::Node>> nestedRoots;
            // Unity's Prefab Variant: the selected instance root becomes an instance of a new
            // variant of its prefab (PREFABS.md 18) instead of the root of an original prefab;
            // the root as it was (an instance of the base)
            bool variant = false;
            YAML::Node variantRootNode;
            Entity variantRoot = NULL_ENTITY;
        };

        Project* project;
        uint32_t sceneId;
        std::vector<Item> items;

        // For undo
        bool wasModified;
        std::vector<Entity> lastSelected;

    public:
        CreatePrefabCmd(Project* project, uint32_t sceneId, const fs::path& filepath, const YAML::Node& prefabNode);
        // variants: every item a Prefab Variant of its instance root's prefab
        CreatePrefabCmd(Project* project, uint32_t sceneId, const std::vector<std::pair<fs::path, YAML::Node>>& prefabs, bool variants = false);
        // One item made a Prefab Variant (the others stay originals): the Project window drop
        // answered "Prefab Variant" for a selection holding instance roots
        void setVariant(size_t index, bool variant) { if (index < items.size()) items[index].variant = variant; }

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}
