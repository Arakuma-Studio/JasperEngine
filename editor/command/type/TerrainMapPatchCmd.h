// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "util/TerrainMapUtils.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor{

    // Stroke undo: restores one rect of a terrain map, so the history holds a patch instead of
    // a copy of the whole map. The file is still rewritten in full on every apply, since PNG
    // has no partial write.
    class TerrainMapPatchCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        TerrainMapRef ref;
        TerrainMapPatch patch;

        bool wasModified = false;

        void apply(const std::vector<unsigned char>& regionPixels, bool restoreModifiedState);

    public:
        TerrainMapPatchCmd(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref, TerrainMapPatch patch);

        bool execute() override;
        void undo() override;
        // the map file each apply rewrites in full (beneath the step's write scope)
        std::vector<Touched> touchedPaths(bool undo) const override;

        // "Sculpt Terrain (Island)", "Paint Terrain (Island)", "Paint Details (Island)"
        std::string describe() const override {
            const char* verb = ref.target == TerrainMapTarget::HeightMap ? "Sculpt Terrain" :
                (ref.target == TerrainMapTarget::BlendMap ? "Paint Terrain" : "Paint Details");
            return std::string(verb) + " (" + CommandNames::entity(project, sceneId, entity) + ")";
        }
        const char* typeName() const override { return "TerrainMapPatchCmd"; }
        size_t memoryBytes() const override {
            return sizeof(*this) + patch.path.size() + patch.beforePixels.capacity() + patch.afterPixels.capacity();
        }
        // Off-thread release: the destructor only frees pixel buffers and strings; no editor/engine/GPU state (S24 R8).
        bool releaseOffThread() const override { return true; }

    };

}
