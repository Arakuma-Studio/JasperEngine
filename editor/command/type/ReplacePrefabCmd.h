// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>

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

        bool mergeWith(Command* otherCommand) override;
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

        bool mergeWith(Command* otherCommand) override;
    };

}
