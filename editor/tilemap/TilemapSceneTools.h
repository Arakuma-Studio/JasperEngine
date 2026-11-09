// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The tile brushes as Scene-view tools (docs/specs/S50-grid-tilemap.md R21, R64, R65, R70, D9, D10):
// one SceneTool per tool mode, sharing TilemapTools, the state the Tile Palette window edits (the
// active layer, the brush, Random, Keep Terrain, Focus On). A stroke writes through a BrushEdit, so
// the map shows it live; Esc reverts it (TileCellPatch::revert) and the release pushes one
// TilemapCellsCmd with R70's undo name.

#pragma once

#include "ecs/Entity.h"
#include "scenetool/SceneTool.h"
#include "tilemap/TilemapBrush.h"
#include "tilemap/TilesetPool.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    struct TilemapLayerComponent;
    struct Transform;
}

namespace jasperengine::editor {
    class Project;
    class SceneRender;
    struct SceneProject;
}

namespace jasperengine::editor::tilemap {

    enum class TileTool { Select, Paint, Box, Line, Flood, Eraser, Picker };
    enum class TileFocusMode { None, Layer, Grid };

    // what the brushes paint with
    struct TileBrushState {
        BrushStamp stamp;                    // the tiles chosen in the palette (or picked)
        std::vector<RandomEntry> random;     // the Random tab's weighted list
        bool randomOn = false;               // the Random toggle
        bool terrainOn = false;              // a terrain chosen: the tools paint terrain
        int terrain = NO_TERRAIN;            // the tileset-global terrain painted
        bool keepTerrain = false;            // the Keep Terrain toggle (R26)
    };

    // the layer the brushes paint on, as a stroke or the overlay needs it
    struct TileLayerContext {
        SceneProject* sceneProject = nullptr;
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        TilemapLayerComponent* layer = nullptr;
        Transform* transform = nullptr;
        TileGridSpec spec;
        std::shared_ptr<const TilesetRuntime> tileset;
        Entity grid = NULL_ENTITY;
        bool linked = false;                 // R30: a linked Grid refuses brushes
    };

    class TilemapTools {
    public:
        static TilemapTools& get();

        void setProject(Project* project);
        Project* project() const { return projectPtr; }

        // the active Tilemap Layer (the Palette's dropdown)
        void setActiveLayer(uint32_t sceneId, Entity layer);
        Entity activeLayer() const { return layerEntity; }
        uint32_t activeScene() const { return layerScene; }
        // the active layer resolved in its scene; `.layer` is null while there is none
        TileLayerContext resolve() const;

        TileBrushState& brush() { return brushState; }

        // the Scene-view tool: `tool` becomes active on the selected scene's view (the Toolbar and
        // the catalog shortcuts); a tool already active in that view is replaced (and cancelled)
        void activate(TileTool tool);
        // leaves tool mode (the Scene toolbar's exit button, Esc with no stroke)
        void deactivate();
        bool isToolActive() const;
        TileTool currentTool() const { return toolMode; }

        TileFocusMode focus() const { return focusMode; }
        void setFocus(TileFocusMode mode);
        // writes the Focus On dim into every layer of `scene` (1: undimmed)
        void applyFocusDim(Scene* scene) const;
        void clearFocusDim(Scene* scene) const;

        // ---- the brush edits of the Palette's and the Scene tools' keys (R64)
        void rotateBrush(int direction);     // +1: clockwise, -1: counter-clockwise
        void flipBrush(bool vertical);
        // the Select tool's rectangle: copied into the brush (then Paint), or deleted as one step
        void copySelection();
        void deleteSelection();
        void clearSelection();
        bool hasSelection() const { return selectionValid; }
        const CellRect& selection() const { return selectionRect; }

        // the status shown under the toolbar and in the Scene view's pill
        const std::string& statusLine() const { return statusText; }
        void setStatus(std::string text) { statusText = std::move(text); }

        // a key the Scene view's Esc path passes through: true when something was cancelled or left
        bool onEscape();
        // cancels the stroke in progress (Esc, deactivate), leaving the tool active
        void cancelStroke();

    private:
        friend class TilemapSceneTool;
        Project* projectPtr = nullptr;
        uint32_t layerScene = 0;
        Entity layerEntity = NULL_ENTITY;
        TileBrushState brushState;
        TileTool toolMode = TileTool::Paint;
        TileFocusMode focusMode = TileFocusMode::None;
        std::string statusText;
        bool selectionValid = false;
        CellRect selectionRect;
        class TilemapSceneTool* activeTool = nullptr;
    };

    // the SceneTool of one mode (D9): id "tilemap.paint", label "Paint (B)"
    class TilemapSceneTool final : public SceneTool {
    public:
        TilemapSceneTool(uint32_t sceneId, TileTool mode);
        ~TilemapSceneTool() override;

        const char* id() const override;
        const char* label() const override;
        bool available(const SceneProject& sceneProject) const override;
        SceneToolPress press(const SceneToolEvent& event) override;
        void drag(const SceneToolEvent& event) override;
        void release(const SceneToolEvent& event) override;
        void hover(const SceneToolEvent& event) override;
        bool key(ImGuiKey key, const SceneToolEvent& event) override;
        void cancel() override;
        void drawOverlay(SceneRender& render, ImDrawList& list) override;
        ImGuiMouseCursor cursor() const override { return ImGuiMouseCursor_Arrow; }
        std::string status() const override { return statusText; }

    private:
        friend class TilemapTools;
        enum class StrokeShape { None, Paint, Box, Line, Flood, Select, Move };

        uint32_t sceneId = 0;
        TileTool mode = TileTool::Paint;
        std::string labelText;

        // what the hover found
        bool hoverValid = false;
        CellCoord hoverCell;                 // the logical cell under the pointer
        CellCoord hoverDisplay;              // the display cell (dual-grid stamps)
        std::string statusText;

        // the stroke in progress
        StrokeShape stroke = StrokeShape::None;
        bool strokeErase = false;
        BrushEdit edit;
        CellCoord strokeStart;               // the press's cell
        CellCoord strokeLast;                // the last cell the stroke reached
        Entity strokeLayer = NULL_ENTITY;
        uint32_t strokeSeed = 0;
        CellRect moveSource;                 // the selection a Move drags (strokeLast moves it)
        uint64_t strokeCounter = 0;
        bool exitRequested = false;          // Esc with nothing to cancel left the tool

        bool targetFor(TileLayerContext& context, bool refuseLinked);
        bool cellsAt(const SceneToolEvent& event, const TileLayerContext& context, CellCoord& logical, CellCoord& display) const;
        void refreshStatus(const TileLayerContext& context);
        void beginStroke(StrokeShape shape, bool erase, Entity layer);
        void paintPoint(const TileLayerContext& context, CellCoord from, CellCoord to);
        void paintShape(const TileLayerContext& context, CellCoord from, CellCoord to);
        void applyAll(const BrushTarget& target, const TileBrushState& brush, std::span<const CellCoord> cells);
        void applyFlood(const TileLayerContext& context, CellCoord cell);
        void pickAt(const TileLayerContext& context, CellCoord logical, CellCoord display);
        const char* undoName() const;
        void finishStroke();
        void drawGrid(const TileLayerContext& context, SceneRender& render, ImDrawList& list);
        void drawCellOutline(const TileLayerContext& context, SceneRender& render, ImDrawList& list, CellCoord cell, ImU32 color, float thickness, bool display);
        void drawGhost(const TileLayerContext& context, SceneRender& render, ImDrawList& list);
        void drawTileGhost(const TileLayerContext& context, SceneRender& render, ImDrawList& list, CellCoord cell, int32_t tile, uint32_t flags);
    };

}
