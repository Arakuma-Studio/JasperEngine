// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILESET_POOL_H
#define TILEMAP_TILESET_POOL_H

// Tilesets resolved for drawing (docs/specs/S50-grid-tilemap.md R12, R17, R31): each tile's texture
// page, its rect in the page and its size, the pages' paths, and the animations. Loaded by path
// (exported games use resolved paths) and cached; reloaded when the editor invalidates the file
// (S05) or a source sheet repacks (its SpriteSheetPool generation moves).
//
// Sources: texture: the page is the texture, tile rects from the slicing (margin + at * (tile +
// spacing), top-left origin) and the texture's size (SpriteSheetSources::textureSize, so cooked
// textures resolve too); sheet: SpriteSheetPool::get(sheet), the frame's page and rect (a trimmed
// frame keeps its source size, placed at its offset); aseprite: the tileset strip the Aseprite
// import writes (ImportedAssets "<file>#tileset#<index>", one tile per tileHeight rows, Aseprite's
// empty tile 0 first).

#include "Export.h"
#include "tilemap/Tileset.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct TileRenderInfo{
        bool valid = false;              // false: the id is missing or its source did not resolve
        uint16_t page = 0;
        // the texture rect in the page, normalized, top-left origin (u0, v0) to (u1, v1)
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
        // the drawn quad in tile pixels from the tile image's bottom-left corner (a trimmed sheet
        // frame is smaller than its source size and offset into it), y up
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        // the tile image's full size in pixels (the source size of a trimmed frame)
        int width = 0;
        int height = 0;
        int offsetX = 0;                 // TileDef::offset
        int offsetY = 0;
        int animation = -1;              // index into TilesetRuntime::animations, -1: still
    };

    struct TileAnimationRuntime{
        std::vector<int32_t> frames;     // tile ids
        std::vector<int32_t> endsMs;     // cumulative end time of each frame
        int32_t totalMs = 0;
        bool randomStart = false;
    };

    struct JASPERENGINE_API TilesetRuntime{
        std::string path;
        std::shared_ptr<const TilesetData> data;
        std::vector<std::string> pages;  // texture paths, page index order
        std::vector<int> pageWidths;
        std::vector<int> pageHeights;
        std::vector<TileRenderInfo> tiles;   // by id (size data->nextId)
        std::vector<TileAnimationRuntime> animations;
        // warnings of the last resolve (a source that did not load, a frame not in its sheet)
        std::vector<std::string> warnings;
        // moves on with every reload, so layers rebuild their chunks
        uint64_t generation = 0;

        const TileRenderInfo* tile(int32_t id) const {
            return id >= 0 && (size_t)id < tiles.size() && tiles[(size_t)id].valid ? &tiles[(size_t)id] : nullptr;
        }
        // the tile an animated tile shows at `ms` (its own id when still)
        int32_t animatedTile(int32_t id, int64_t ms) const;
    };

    class JASPERENGINE_API TilesetPool{
    public:
        // cached, else loaded and resolved; null when the file does not load or parse (the warning
        // is logged once per generation)
        static std::shared_ptr<const TilesetRuntime> get(const std::string& path);
        static std::shared_ptr<const TilesetRuntime> find(const std::string& path);
        // a document from memory (the editor's unsaved edits and MCP previews), resolved as get()
        static std::shared_ptr<const TilesetRuntime> resolve(const std::string& path, std::shared_ptr<const TilesetData> data);
        // replaces the cached entry for `path` with `data` (the editor after a write)
        static void set(const std::string& path, std::shared_ptr<const TilesetData> data);
        static void invalidate(const std::string& path);
        // an .aseprite file imported again: the tilesets drawing its strips resolve the new ones
        static void asepriteSourceChanged(const std::string& asepritePath);
        static void clear();
        // moves on with every change of any entry (layers compare it once per frame)
        static uint64_t revision();
        // the error of the last failed load of `path` ("" when it loaded)
        static std::string lastError(const std::string& path);
    };

}

#endif // TILEMAP_TILESET_POOL_H
