// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"

#include <vector>
#include "project/PrefabUnpackRecord.h"

namespace jasperengine::editor{

    // Unity's Unpack Prefab (and Unpack Prefab Completely) over a selection: each selected
    // GameObject stands for the outermost prefab instance holding it. One undo step.
    class UnpackPrefabCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        std::vector<Entity> targets;
        bool completely;

        bool wasModified;

        std::vector<PrefabUnpackRecord> records;

    public:
        UnpackPrefabCmd(Project* project, uint32_t sceneId, std::vector<Entity> targets, bool completely);

        // Whether the command would unpack anything for these GameObjects
        static bool canUnpack(const Project* project, uint32_t sceneId, const std::vector<Entity>& targets);

        bool execute() override;
        void undo() override;

        // "Unpack Prefab Player", "Unpack Prefab Completely 2 Objects"
        std::string describe() const override {
            return std::string(completely ? "Unpack Prefab Completely " : "Unpack Prefab ") + CommandNames::entities(project, sceneId, targets);
        }
        const char* typeName() const override { return "UnpackPrefabCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this) + targets.capacity() * sizeof(Entity);
            for (const PrefabUnpackRecord& record : records) {
                bytes += sizeof(record) + record.instance.members.size() * sizeof(Prefab::EntityMember) +
                    record.instance.overrides.size() * sizeof(Prefab::ObjectOverrides);
            }
            return bytes;
        }

    };

}
