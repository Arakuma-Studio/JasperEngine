// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Tileset pages as ImGui textures (docs/specs/S50-grid-tilemap.md R64, R65, R67, R71): the Tile
// Palette's and the Tileset Editor's tile images and the Scene tools' 50 % ghost. A page uploads
// once per tileset generation (a re-sliced or edited source, a reload); a replaced texture is
// retired, not destroyed: the frame drawn with it may still be in flight.

#pragma once

#include "pixelart/PixelImage.h"
#include "tilemap/TilesetPool.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::tilemap {

    class TilePageTextures {
    public:
        static TilePageTextures& get();

        // the page's texture, null when its image does not load
        ImTextureID page(const std::string& pagePath, const TilesetRuntime& tileset);

        // destroys the textures retired before this frame; once a frame, before drawing
        void beginFrame();
        // a project switch: everything uploaded is retired
        void clear();
        // how many times a page's image was read and decoded
        uint64_t decodes() const { return decodeCount; }

    private:
        struct Entry {
            Texture texture;
            uint64_t generation = 0;
            bool loaded = false;
            // a page that did not load is read again only when its file changes (each tile of it
            // asks every frame): the file's stamp then, and the frame it was last looked at
            bool failed = false;
            int64_t failedStamp = 0;
            uint64_t checkedFrame = 0;
        };
        std::map<std::string, Entry> entries;
        std::vector<Texture> retired;
        uint64_t uploads = 0;
        uint64_t decodeCount = 0;
        uint64_t frame = 0;
    };

    // a tile of a source's grid: texture sources at their (at) positions, others wrapped ten to a
    // row; the grid's column and row counts
    struct TileGridCell {
        int32_t id = NO_TILE;
        int col = 0;
        int row = 0;
    };
    void tileSourceGrid(const TilesetRuntime& tileset, int sourceId, std::vector<TileGridCell>& out, int& columns, int& rows);

    // a tile's image at an integer zoom (the tooltips, R71): the tile's page quad, its size in
    // pixels times zoom, its flips and transpose in the texture's corners
    void drawTileImage(ImDrawList* list, ImVec2 min, float pixelZoom, const TilesetRuntime& tileset, int32_t tile, uint32_t flags = 0, ImU32 tint = IM_COL32_WHITE);

    // the tile image and details of a hover tooltip (id, name, properties)
    void showTileTooltip(const TilesetRuntime& tileset, int32_t tile, int zoom = 3);

}
