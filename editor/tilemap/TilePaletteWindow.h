// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Window > 2D > Tile Palette (docs/specs/S50-grid-tilemap.md R64): the tools' toolbar (Select, Paint,
// Box, Line, Flood, Eraser, Picker), the Active Tilemap, rotate and flip, Random, Keep Terrain and
// Focus On, and the Tiles, Terrains, Random and Stamps tabs. It edits TilemapTools, which the
// Scene-view tools read (TilemapSceneTools.h). Shortcuts are catalog commands (S25, rebindable).

#pragma once

#include "ecs/Entity.h"
#include "tilemap/TilemapBrush.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    struct TilesetRuntime;
}

namespace jasperengine::editor {
    class CommandCatalog;
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::tilemap {

    class TilePaletteWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Tile Palette";

        explicit TilePaletteWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // the window has the keyboard (the catalog's Tile Palette context, with a Scene tool active too)
        bool takesKeys() const;
        void resetForProjectSwitch();
        // the tools' and the brush's keys (the Tile Palette context)
        void bindCommands(CommandCatalog& catalog);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        bool focused = false;
        int zoom = 2;

        // the tiles chosen in the Tiles tab: a rectangle of a source's grid
        int selectionSource = -1;
        int selectionMinCol = 0, selectionMinRow = 0, selectionMaxCol = -1, selectionMaxRow = -1;
        // a drag in progress
        bool draggingTiles = false;
        int dragSource = -1, dragCol0 = 0, dragRow0 = 0, dragCol1 = 0, dragRow1 = 0;

        char stampName[64] = "Stamp";

        void drawToolbar();
        void drawActiveTilemap();
        void drawTiles(const std::shared_ptr<const TilesetRuntime>& tileset);
        void drawTerrains(const std::shared_ptr<const TilesetRuntime>& tileset);
        void drawRandom(const std::shared_ptr<const TilesetRuntime>& tileset);
        void drawStamps(const std::shared_ptr<const TilesetRuntime>& tileset);
        // the Tiles tab's selection as the brush's stamp
        void setStampFromSelection(const TilesetRuntime& tileset);
        std::vector<int32_t> selectedTiles(const TilesetRuntime& tileset) const;
        // the Grid of the selection and its layers, name order: the dropdown's list
        void gridLayers(SceneProject*& sceneProject, Entity& grid, std::vector<Entity>& layers) const;
        void createLayer(SceneProject* sceneProject, Entity grid);
        // the tile set document for edits, with the layer's file; false when none
        bool writeTileset(const TilesetData& data, const std::string& stepName);
        void syncActiveLayerFromSelection();
    };

}
