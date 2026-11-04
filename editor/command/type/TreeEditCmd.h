// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An edit of a Terrain's trees (T5 R52): deltas of the changed records, never the file. A stroke
// merges by its id, and memoryBytes reports what the deltas hold.

#include "command/Command.h"
#include "ecs/Entity.h"
#include "terrain/TreeStore.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class Project;

    class TreeEditCmd : public Command {
    public:
        // After runs after execute and undo in the same step: what a whole tool also changes with the
        // trees (a convert empties its host). strokeId 0 merges with nothing.
        TreeEditCmd(Project* project, uint32_t sceneId, Entity terrain, std::string name, terrain::TreeDelta delta, uint64_t strokeId = 0);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* other) override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return delta.removed.empty() && delta.added.empty(); }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "TreeEditCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + name.capacity() + delta.memoryBytes(); }

        const terrain::TreeDelta& getDelta() const { return delta; }

        // Steps with more in them than the delta: called after the delta is applied or undone
        std::function<void(bool undone)> alongside;

    private:
        void apply(bool undo);

        Project* project;
        uint32_t sceneId;
        Entity terrain;
        std::string name;
        terrain::TreeDelta delta;
        uint64_t strokeId;
        bool firstExecute = true;
    };

}
