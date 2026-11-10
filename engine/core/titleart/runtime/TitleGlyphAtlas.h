// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R25-R32)
// - Purpose: Glyph Atlas, the mode for text that changes every frame. Each glyph is rendered once
//   with the full look by S80's renderer (RenderRequest::glyphCell: its back part, the rims,
//   shadows and glows, and its front part, the face) into RGBA8 atlas pages; the layers below and
//   above the text layer render once per state as the back and front plates. A text change lays
//   the string out (TitleTextLayout, the renderer's own layout), then composes: the back plate,
//   every back cell, every front cell, the front plate (so neighbours' rims stay behind every
//   face), then S80's output pass. With every glyph cached a change costs that compose and output
//   (a handful of passes), never a render.
// - Cells: keyed by the cluster's text, the style (the document's content hash, the instance's
//   values other than the text, the resolution), the character's and its line's adjustments
//   (CharAdjust: offset, scale, rotation, face material; LineAdjust) and the phase. A cell is
//   rendered from the cell document (the text layer with the unsafe members of R31 made safe:
//   warp and distort off, hull and box plates as contour, masks dropped, unsafe effects disabled)
//   with its character's and line's adjustments as the single glyph's, so per-character looks
//   render exactly. A cell renders at the sub-pixel phase
//   (1/32 px) the string puts its glyph at (its character offset moved by that fraction, part of
//   the key), so its quad moves by whole pixels from the single glyph's place to the string's and
//   is never resampled. The string itself snaps to the pixel grid (its first pen to a whole
//   pixel, under half a pixel from Realtime's place), so a glyph keeps its phase from one value to
//   the next and a counter's change reuses every cell.
// - Atlas: pages of atlasPageSize, at most atlasMaxPages, packed by a skyline. A full atlas frees
//   the least recently used page no shown or wanted string uses; when none can go, the job fails
//   with R29's sentence. The string stays on its previous image until every glyph it needs is in.
// - A cell keeps only the pixels its content and reach can cover (S80's reach rectangle, not the
//   64-pixel ROI grid).
// - The text layer's opacity (and its groups') and a blend other than normal apply when the
//   glyphs compose: into their own target, then over the back plate (S80's blend pass).
// - Prebaked atlas (R32): the export renders a title's cells and plates in the editor and ships
//   them (snapshot(): pages and plates read back, the manifest); a player's first job of that
//   style restores them (restore()) and renders no glyph the export rendered.
// - Cells hold linear premultiplied colour in RGBA8 (S80's RGBA8-path encoding of accumulators).
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitleRenderJob.h"
#include "titleart/render/TitleTargetPool.h"
#include "titleart/runtime/TitleArtLive.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleTextLayout;

    // tests: compose each glyph's back and front cells together, in string order (the R26 red
    // control: rims then cover the neighbour's face)
    constexpr uint32_t TITLE_ART_TEST_GLYPH_STRING_ORDER = 1u << 8;

    // a style's atlas as files hold it (R32): the manifest (page size, skylines, cells, anchors,
    // plates) and the RGBA8 pixels (linear premultiplied, rows top first) of the pages and plates
    struct JASPERENGINE_API TitleGlyphAtlasSnapshot {
        Json manifest;
        std::vector<std::vector<uint8_t>> pages;
        std::vector<std::vector<uint8_t>> plates;
    };

    // the style a Glyph Atlas title's cells and prebaked files are keyed by: the document's content
    // hash, the instance's values other than the text layer's text, the resolution
    JASPERENGINE_API uint64_t titleGlyphStyleHash(uint64_t documentHash, const std::vector<FieldOverride>& overrides, const std::string& textLayer,
                                                  float pxScale);

    class JASPERENGINE_API TitleGlyphAtlas {
    public:
        struct Cell {
            int page = -1;
            TitleRectI back, front;         // atlas pixels (empty: the part has no pixels)
            TitleRectI backPixels, frontPixels;     // the output pixels the single glyph's render covered
            double anchorX = 0.0, anchorY = 0.0;    // the single glyph's position, output pixels
            uint64_t lastUse = 0;
            bool blank = false;             // a cluster without ink: no quad
        };
        struct Stats {
            uint64_t cellRenders = 0;       // cells rendered since creation
            uint64_t composes = 0;          // composes run
            uint64_t composePasses = 0;     // GPU passes of the last compose (plates, cells, output)
            uint64_t evictions = 0;         // pages freed for new cells
            // the last string's snap to the pixel grid: where its glyphs sit from their exact
            // places (output pixels, under half a pixel)
            double snapX = 0.0, snapY = 0.0;
            uint64_t styleHash = 0;         // the last job's style (titleGlyphStyleHash)
            uint64_t restoredCells = 0;     // cells a prebaked atlas brought (R32)
        };

        TitleGlyphAtlas();
        ~TitleGlyphAtlas();
        TitleGlyphAtlas(const TitleGlyphAtlas&) = delete;
        TitleGlyphAtlas& operator=(const TitleGlyphAtlas&) = delete;

        void setLimits(int pageSize, int maxPages);
        int pageSize() const { return pageSide; }
        int maxPages() const { return pageLimit; }
        int pageCount() const { return (int)pages.size(); }
        size_t cellCount() const { return cells.size(); }
        size_t bytes() const;
        const Stats& stats() const { return counters; }

        const Cell* find(const std::string& key);
        // copies a rendered glyph's two parts into the pages; false with `error` when the atlas is
        // full (no page can be freed: `inUse` holds the keys a string needs) or a target fails.
        // False with `deferred` when a copy's first draw must wait for a later frame
        // (TitleFirstDraws): nothing is registered, and the same call retries the copies then
        bool insert(const std::string& key, const TitleImage& back, const TitleImage& front, double anchorX, double anchorY,
                    const std::set<std::string>& inUse, bool& full, bool& deferred, std::string& error);
        void insertBlank(const std::string& key);
        // the keys of the shown string (they stay while another one renders)
        void setShown(std::set<std::string> keys) { shownKeys = std::move(keys); }
        const std::set<std::string>& shown() const { return shownKeys; }

        // the plates of a state (the renderer's linear images), by their hash
        TitleImage* plate(uint64_t hash);
        void storePlate(uint64_t hash, TitleImage image);

        // the position of a cluster's glyph rendered alone (output pixels), by style and cluster
        bool anchor(const std::string& key, double& x, double& y) const;
        void storeAnchor(const std::string& key, double x, double y);

        // the cell document of a style (built once per style)
        std::shared_ptr<const TitleArtDoc> cellDocument(uint64_t styleHash) const;
        void storeCellDocument(uint64_t styleHash, std::shared_ptr<const TitleArtDoc> doc);

        // the string's (0) and the single glyph's (1) layouts, kept across strings
        TitleTextLayout& layout(int which, const AssetResolver* assets);

        TitleGpu& gpu() { return gpuLayer; }
        TitleTargetPool& pool() { return targetPool; }
        TitleTarget* pageTarget(int page) const;
        Stats& mutableStats() { return counters; }

        // the GPU side is gone (device loss): pages, cells and plates dropped
        void clear();

        // R32: everything the atlas holds (pages and plates read back from the GPU)
        bool snapshot(TitleGlyphAtlasSnapshot& out, std::string& error);
        // a snapshot's pages, cells, anchors and plates into this atlas (pages it had are dropped);
        // false with an empty error while the copy program builds
        bool restore(const TitleGlyphAtlasSnapshot& in, std::string& error);
        // a prebaked manifest and its PNGs (titleArtGlyphPrebakePath); false when there is none
        static bool readPrebaked(const std::string& manifestPath, TitleGlyphAtlasSnapshot& out, std::string& error);
        // a snapshot as those files (`manifestPath` a file system path; the PNGs beside it); the
        // bytes written are added to `bytes`
        static bool writePrebaked(const std::string& manifestPath, const TitleGlyphAtlasSnapshot& snapshot, size_t& bytes, std::string& error);
        // a style's prebaked files are looked for once
        bool prebakeTried(uint64_t styleHash) const { return triedPrebakes.count(styleHash) > 0; }
        void markPrebakeTried(uint64_t styleHash) { triedPrebakes.insert(styleHash); }
        // the uploads of the last restore can go (their copies ran in an earlier frame)
        void releaseUploads() { uploads.clear(); }

    private:
        struct Page {
            TargetRef target;
            std::vector<int> skyline;       // the filled height of each column
            uint64_t lastUse = 0;
            bool cleared = false;
        };

        SokolTitleTargetAllocator allocator;
        TitleTargetPool targetPool;
        TitleGpu gpuLayer;
        int pageSide = 1024;
        int pageLimit = 4;
        std::vector<Page> pages;
        // a cell whose page copies went to a later frame keeps its place until they draw
        struct PendingInsert {
            std::string key;
            int page = -1;
            TitleRectI rect;
        };
        PendingInsert pendingInsert;
        std::map<std::string, Cell> cells;
        std::set<std::string> shownKeys;
        std::map<uint64_t, TitleImage> plates;
        std::map<uint64_t, std::shared_ptr<const TitleArtDoc>> cellDocs;
        std::map<std::string, std::pair<double, double>> anchors;
        std::set<uint64_t> triedPrebakes;
        std::vector<std::unique_ptr<TitleUpload>> uploads;     // a restore's sources, released at the next job
        std::unique_ptr<TitleTextLayout> layouts[2];
        const AssetResolver* layoutAssets = nullptr;
        uint64_t clock = 0;
        Stats counters;

        bool place(Page& page, int width, int height, TitleRectI& out);
        bool allocate(int width, int height, const std::set<std::string>& inUse, int& page, TitleRectI& rect, bool& full, std::string& error);
        bool newPage(std::string& error);
        void freePage(int index);
        bool copyCell(int page, const TitleImage& from, const TitleRectI& region, const TitleRectI& into, std::string& error);
        bool copyUpload(TitleTarget* target, TextureRender& from, int width, int height, std::string& error);
    };

    // R32: the strings an export prebakes for a Glyph Atlas title: its initial text, and each
    // digit at every place a digit stands in it (a counter's every value of that length)
    JASPERENGINE_API std::vector<std::string> titleGlyphPrebakeStrings(const std::string& initial);

    // R32: `strings` rendered through `live` (a Glyph Atlas title) one after the other until its
    // atlas holds `maxCells` cells; `overrides` are the instance's (their text replaced); false
    // with `error` when a string fails (the cells before it stay)
    JASPERENGINE_API bool prebakeTitleGlyphAtlas(TitleArtLive& live, const TitleJobFactory& factory, const TitleArtLiveState& state,
                                                 const std::vector<FieldOverride>& overrides, const std::string& textLayer,
                                                 const std::vector<std::string>& strings, size_t maxCells, std::string& error);

    // the Glyph Atlas job of `spec` (TitleJobSpec::atlas set): plates and missing cells through
    // `renderer`, then the compose
    JASPERENGINE_API std::unique_ptr<TitleJob> makeGlyphAtlasJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec);

}
