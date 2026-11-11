// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Tileset Editor (docs/specs/S50-grid-tilemap.md R67): double-click a .tileset. Its sources and
// tile grid on the left; on the right the selected tiles' Name, Probability, Offset, Collision (mode,
// Auto from Alpha, a pixel-snapped polygon editor), Animation (strip with durations, a preview),
// Terrain (a peering painter on the tile's 3x3 regions, 6/12 on hex), Properties from the schema and
// the Navigation Area. Every edit is one undo step written to the file (R70, writeTilesetFile), the
// pool's copy kept in sync; the file is re-read when it changes on disk.

#pragma once

#include "tilemap/TerrainTemplates.h"
#include "tilemap/TilesetPool.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    class ProjectWindow;
}

namespace jasperengine::editor::tilemap {

    class TilesetEditorWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Tileset Editor";

        TilesetEditorWindow(Project* project, ProjectWindow* projectWindow);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // opens the .tileset (absolute or project-relative)
        void open(const std::filesystem::path& file);
        // Assets > Create > 2D > Tileset: "New Tileset.tileset" in `directory`; the file, empty on failure
        std::filesystem::path createEmpty(const std::filesystem::path& directory);
        // "Create Tileset" on a texture, an atlas or an Aseprite file: beside it, opened here
        std::filesystem::path createFromSource(const std::filesystem::path& source);
        void resetForProjectSwitch();

    private:
        Project* project;
        ProjectWindow* projectWindow;
        bool windowOpen = false;
        bool focusRequested = false;

        // ---- the document
        std::filesystem::path file;                  // absolute; empty: none
        std::string assetPath;                       // the pool's key (Assets-relative)
        TilesetData document;
        std::shared_ptr<const TilesetRuntime> runtime;
        bool loaded = false;
        std::string loadError;
        std::filesystem::file_time_type fileTime{};
        double lastDiskCheck = -1.0;
        std::string notice;
        bool noticeIsError = false;

        // ---- the selection
        std::vector<int32_t> selection;              // sorted tile ids
        int32_t rangeAnchor = NO_TILE;

        // ---- the field being held (a drag, a slider, typing): the fields read the document every
        // frame, so the one held shows this copy until it is let go, then commits once
        struct HeldField {
            ImGuiID id = 0;
            int frame = -1;                          // the last frame it was active
            double values[4] = {0.0, 0.0, 0.0, 0.0};
            std::vector<int32_t> tiles;              // the selection when it was taken
        };
        HeldField held;

        // ---- the peering painter: what a click paints (PEERING_ANY, PEERING_NONE, or a set-local terrain)
        int paintValue = PEERING_ANY;

        // ---- the polygon editor
        int activePolygon = 0;
        int dragPoint = -1;
        bool polygonDirty = false;

        // ---- the add-source form
        char sourcePath[260] = {0};

        // ---- slicing
        int sliceSource = -1;
        int sliceWidth = 16, sliceHeight = 16, sliceMargin = 0, sliceSpacing = 0;
        bool sliceSkipEmpty = true;

        // ---- Apply Terrain Template...
        bool templateOpenRequested = false;
        int templateChoice = 0;
        int templateTerrain = 0;
        int templateSecond = -1;
        std::string templateError;

        // ---- the coverage panel (R20)
        int coverageSet = 0;
        bool coverageTransforms = false;
        TerrainCoverage coverage;
        bool coverageValid = false;

        // ---- schema form
        char propertyName[64] = {0};
        int propertyType = 0;

        // ---- terrain set form
        char setName[64] = "Ground";
        int setMode = 0;
        bool setTwoSurface = false;
        char terrainName[64] = "Grass";
        float terrainColor[3] = {0.37f, 0.63f, 0.3f};

        void reload();
        void pollDisk();
        void setDocument(const TilesetData& data);
        // one undo step: applies `change` to a copy of the document, validates it, writes the file
        bool edit(const std::string& name, const std::function<void(TilesetData&)>& change);
        void setNotice(const std::string& text, bool error);

        TileDef* selectedFirst();
        void select(int32_t id, bool shift, bool ctrl);
        bool isSelected(int32_t id) const;
        void forSelected(const std::string& name, const std::function<void(TileDef&)>& change);
        void forTiles(const std::string& name, const std::vector<int32_t>& ids, const std::function<void(TileDef&)>& change);
        // draws a field through `draw` over `values`, the held copy while it is held; true once, on
        // the frame it is let go after an edit (`values` then holds the edited value, `held.tiles`
        // the tiles it applies to)
        template <typename T>
        bool heldField(const char* label, T* values, int count, const std::function<void()>& draw);
        // a colour field (3 or 4 sRGB values): held like the fields above while the Color window
        // changes it; true once, as the window's session ends after a change (or an Applied edit)
        bool heldColor(const char* id, float* values, int count, const char* label, float width);

        void drawToolbar();
        void drawSources();
        void drawTileGrids();
        void drawSelectedInspector();
        void drawCollision(TileDef& first);
        void drawPolygonEditor(TileDef& first);
        void drawAnimation(TileDef& first);
        void drawTerrain(TileDef& first);
        void drawPeeringPainter(TileDef& first, const TerrainSetDef& set);
        void drawProperties(TileDef& first);
        void drawTerrainSets();
        void drawCoverage();
        void drawTemplateModal();
    };

}
