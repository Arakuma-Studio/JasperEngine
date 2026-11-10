// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// One undo step of cell edits on Tilemap Layers (docs/specs/S50-grid-tilemap.md D10, R70): per
// layer, the changed cells (coordinate, old and new value and terrain). The cells were already
// applied by the stroke (TilemapBrush writes as it goes); execute reapplies them for redo. The
// TilemapSystem rebuilds the touched chunks by their revisions, so writing the store is all an
// edit does.

#pragma once

#include "command/Command.h"
#include "ecs/Entity.h"
#include "tilemap/TerrainSolver.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::tilemap {

    class TilemapCellsCmd : public Command {
    public:
        struct LayerChanges {
            Entity layer = NULL_ENTITY;
            std::vector<TileCellChange> changes;
        };

        // the changes of each layer as TileCellPatch::changes gives them; `name` is the undo step's
        // ("Paint Tiles", "Box Fill", "Flood Fill", "Erase Tiles", "Paint Terrain", "Move Tiles",
        // "Refresh Autotile")
        TilemapCellsCmd(Project* project, uint32_t sceneId, std::vector<LayerChanges> layers, std::string name);
        TilemapCellsCmd(Project* project, uint32_t sceneId, Entity layer, std::vector<TileCellChange> changes, std::string name);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;
        // the destructor only frees the patch vectors
        bool releaseOffThread() const override { return true; }

        std::string describe() const override { return name; }
        const char* typeName() const override { return "TilemapCellsCmd"; }

        const std::vector<LayerChanges>& layers() const { return layerChanges; }

    private:
        Project* project;
        uint32_t sceneId;
        std::vector<LayerChanges> layerChanges;
        std::string name;

        bool apply(bool forward);
    };

}
