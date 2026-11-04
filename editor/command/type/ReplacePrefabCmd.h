// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "util/YamlFootprint.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>
#include "component/PrefabComponent.h"

namespace jasperengine::editor{

    // Unity's Replace Prefab Asset over a selection of instance roots (live or Missing):
    // each becomes an instance of the new prefab keeping its root (id, place, position and
    // rotation) and the overrides that match the new prefab (PrefabReplaceSettings). One
    // undo step; undo rebuilds each instance exactly as it was.
    class ReplacePrefabCmd: public Command{

    private:
        struct Entry {
            Entity root = NULL_ENTITY;
            YAML::Node oldNode;
            PrefabMemberIds oldMemberIds;
            YAML::Node newNode;
            PrefabMemberIds newMemberIds;
        };

        Project* project;
        uint32_t sceneId;
        std::vector<Entity> roots;
        std::filesystem::path newPath;
        PrefabReplaceSettings settings;

        std::vector<Entry> entries;
        bool prepared = false;
        bool wasModified = false;

    public:
        ReplacePrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> roots, const std::filesystem::path& newPath,
            const PrefabReplaceSettings& settings = PrefabReplaceSettings());

        // The roots of `targets` Replace Prefab can swap (Project::canReplacePrefab)
        static std::vector<Entity> replaceableRoots(const Project* project, uint32_t sceneId, const std::vector<Entity>& targets);

        bool execute() override;
        void undo() override;

        // "Replace Prefab Asset Door.prefab (2 Objects)"
        std::string describe() const override {
            return "Replace Prefab Asset " + CommandNames::file(newPath) + " (" + CommandNames::entities(project, sceneId, roots) + ")";
        }
        const char* typeName() const override { return "ReplacePrefabCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this);
            for (const Entry& entry : entries) {
                bytes += sizeof(entry) + yamlFootprint(entry.oldNode) + yamlFootprint(entry.newNode);
            }
            return bytes;
        }

    };

    // Unity's Unpack on a Missing Prefab instance: the root becomes a plain GameObject (its
    // PrefabComponent and the kept instance data go). Undo makes it a Missing Prefab again.
    class UnpackMissingPrefabCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        std::vector<Entity> roots;

        struct Entry {
            Entity root = NULL_ENTITY;
            PrefabComponent component;
            YAML::Node data;
            PrefabMemberIds memberIds;
        };
        std::vector<Entry> entries;
        bool wasModified = false;

    public:
        UnpackMissingPrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> roots);

        bool execute() override;
        void undo() override;

        std::string describe() const override { return "Unpack Missing Prefab " + CommandNames::entities(project, sceneId, roots); }
        const char* typeName() const override { return "UnpackMissingPrefabCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this);
            for (const Entry& entry : entries) {
                bytes += sizeof(entry) + yamlFootprint(entry.data);
            }
            return bytes;
        }

    };

}
