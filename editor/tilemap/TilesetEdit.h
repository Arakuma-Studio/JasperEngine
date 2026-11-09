// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Tileset documents as the editor builds them (docs/specs/S50-grid-tilemap.md R12, R13, R15, R20,
// R71): slicing a texture, re-slicing with ids kept, adding sheet and Aseprite sources, the alpha
// collision outlines, the contact sheet and the coverage images. Pure over TilesetData, plus a
// small pixel access (TilesetPagePixels) so a caller can hand pixels in memory.
//
// Nothing here writes a file: writeTilesetFile goes through the undoable path
// (ProjectFileWrites::writeUndoable, AtomicFile underneath); callers composing several writes in one
// step use tilesetFileItem.

#pragma once

#include "command/type/WriteFilesCmd.h"
#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"
#include "sprite/SpriteSheet.h"
#include "tilemap/TerrainTemplates.h"
#include "tilemap/TilesetPool.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart::aseprite {
    struct TilesetOutput;
}

namespace jasperengine::editor::tilemap {

    using pixelart::PixelImage;
    using pixelart::PixelProblem;

    // Tiled's slicing: the columns are floor((width - 2 margin + spacing) / (tile + spacing)), the
    // rows alike
    struct TextureSlicing {
        int tileWidth = 16;
        int tileHeight = 16;
        int margin = 0;
        int spacing = 0;
        bool skipEmpty = true;           // fully transparent tiles get no id
    };

    // Page pixels by path, loaded once and kept while the cache lives. The default loader takes
    // what a TilesetRuntime lists as a page: a texture's asset path (sprite::loadSpriteImage over
    // FileData::getSystemPath), or a "<file>#<key>" imported reference (ImportedAssets). Pass a
    // loader to serve pixels from memory (tests, previews of unsaved images).
    class TilesetPagePixels {
    public:
        using Loader = std::function<std::optional<PixelImage>(const std::string& path, std::string* error)>;

        TilesetPagePixels();
        explicit TilesetPagePixels(Loader loader);

        // null when it does not load; lastError() says why
        const PixelImage* get(const std::string& path);
        const std::string& lastError() const { return error; }

    private:
        Loader loader;
        std::map<std::string, std::optional<PixelImage>> pages;
        std::string error;
    };

    // ---- creating and re-slicing
    // A new tileset with one texture source; ids in reading order (0-based), top row first
    bool createFromTexture(const std::string& name, const AssetRef& texture, const TextureSlicing& slicing, const PixelImage& image,
                           TilesetData& out, PixelProblem& error);
    // A texture source appended to `data`, tiles after the existing ids; out: the new source's id
    bool addTextureSource(TilesetData& data, const AssetRef& texture, const TextureSlicing& slicing, const PixelImage& image, int& sourceId,
                          PixelProblem& error);

    struct ResliceReport {
        std::vector<int32_t> kept;       // the ids that stayed (R13), in id order
        std::vector<int32_t> added;      // new ids, in reading order
        std::vector<int32_t> removed;    // tiles whose slot no longer exists (their ids are not reused)
    };
    // Applies a new slicing to a texture source. A tile keeps its id when the new grid has a slot at
    // the same pixel position (R13: its `at` moves, its data stays); new slots take new ids appended
    // after the existing ones; a tile whose slot is gone is removed. Tiles that became transparent
    // keep their ids (skip_empty only decides who gets a new one).
    bool resliceTexture(TilesetData& data, int sourceId, const TextureSlicing& slicing, const PixelImage& image, ResliceReport& report,
                        PixelProblem& error);

    // ---- other sources
    // A sheet source: `frames` by name (empty: every frame), one new tile per frame, from the sheet
    // SpriteSheetPool::get gives for the reference's path
    bool addSheetSource(TilesetData& data, const AssetRef& sheetRef, const std::vector<std::string>& frames, int& sourceId, PixelProblem& error);
    bool addSheetSource(TilesetData& data, const AssetRef& sheetRef, const SpriteSheetData& sheet, const std::vector<std::string>& frames, int& sourceId,
                        PixelProblem& error);
    // An Aseprite source: Aseprite's tile i >= 1 is our tile i - 1 of the source (tile 0 is its empty
    // tile), its user data properties declared in the schema when new (a value of another type than
    // the schema says is skipped, in `warnings`)
    bool addAsepriteSource(TilesetData& data, const AssetRef& file, const pixelart::aseprite::TilesetOutput& tileset, int tilesetIndex,
                           std::vector<std::string>& warnings, int& sourceId, PixelProblem& error);
    // the same, reading the .aseprite file (its tileset `tilesetIndex` of AsepriteImport::tilesets())
    bool addAsepriteSourceFromFile(TilesetData& data, const AssetRef& file, const std::filesystem::path& absolute, int tilesetIndex,
                                   std::vector<std::string>& warnings, int& sourceId, PixelProblem& error);

    // ---- pixels of a resolved tileset (TilesetPool::resolve for an unsaved document)
    // The tile's image at its full size, a trimmed frame placed inside it; false when the tile or its
    // page is unavailable
    bool tileImage(const TilesetRuntime& tileset, int32_t id, TilesetPagePixels& pages, PixelImage& out);

    // R15: the tiles' alpha outlines, written into their collision polygons so the runtime never
    // reads pixels. `tiles` empty: every tile with alpha collision mode; a tile whose image is
    // unavailable fails with tileset_missing, no outline written before it changed
    bool fillAlphaCollisions(TilesetData& data, const TilesetRuntime& tileset, TilesetPagePixels& pages, const std::vector<int32_t>& tiles,
                             size_t& filled, PixelProblem& error);

    // ---- images
    // nearest-neighbour upscale by an integer factor (scale <= 1: a copy)
    PixelImage scaleNearest(const PixelImage& image, int scale);

    struct ContactSheetOptions {
        int columns = 16;
        int scale = 2;                   // integer tile zoom
        bool drawIds = true;
    };
    // Every tile in id order on an opaque ground, each id drawn under its tile (deterministic: the
    // same document gives the same bytes); R71's thumbnails and `edit_tileset` previews
    PixelImage contactSheet(const TilesetRuntime& tileset, TilesetPagePixels& pages, const ContactSheetOptions& options = {});

    // R20: each missing mask of a coverage as a small 3 x 3 cell image: terrains in their colours
    // (dark for none, a checker for "any"), `cellPixels` pixels a cell, the order of coverage.missing
    std::vector<PixelImage> coverageImages(const TilesetData& data, int set, const TerrainCoverage& coverage, int cellPixels = 6);

    // ---- documents and files
    // "<dir>/<stem>.tileset" of a texture path (the convention `<texture>.tileset`, R50, R75)
    std::string tilesetPathForTexture(const std::string& texturePath);
    // a WriteFilesCmd item with the document's bytes (tileset::write)
    WriteFilesCmd::Item tilesetFileItem(const std::filesystem::path& absolute, const TilesetData& data);
    // the document as one undo step (ProjectFileWrites::writeUndoable), the pool's copy invalidated
    // after the write, its undo and its redo
    bool writeTilesetFile(Project* project, const std::filesystem::path& absolute, const TilesetData& data, std::string& error,
                          const std::string& stepName = {});

}
