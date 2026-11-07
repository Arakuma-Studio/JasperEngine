// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R10-R19, R18a, R49, §6.3)
// - Purpose: the one Title Art renderer. The editor's previews, thumbnails, bakes and checks and
//   S81's Realtime titles all render through begin(): no editor-only path. A request names an
//   immutable document snapshot and what to render (the canvas or a region, at a px_scale; one
//   layer alone; one output's content; field overrides; kept splits; one glyph cell).
// - Pipeline per render (§6.3): overrides are applied to a copy of the snapshot (values never
//   reach the document) and hashed like an edit, so a colour override reuses every cache a colour
//   change would; then the draw tree (DrawTree.h), then per node in draw order: content -> part
//   effects -> layer effects -> clip group -> masks -> blend and opacity onto the parent, each
//   node in a target covering only its region of interest (TitleFrame.h), the whole as the flat
//   step list of a TitleRenderJob.
// - Pixel contract: linear premultiplied RGBA16F intermediates where the device renders and
//   filters them, else the RGBA8 fallback (TitleArtCaps.h) with the same passes; the output pass
//   writes straight-alpha sRGB with a dither of under one code that never moves exact codes.
// - Caches: per-layer node pixels in an LRU of RendererLimits::nodeCacheBytes keyed by (node hash,
//   px_scale, region), plus the cascade of TitleArtHash.h; targets from the renderer's
//   TitleTargetPool, within RendererLimits::poolBytes. A new SystemRender::getContextId() (device
//   loss) drops the pool, the caches and the pipelines before the next begin().
// - Determinism (R17): every random element is seeded from document data (`seed` + an element
//   seed); nothing depends on ids, id order, timing, cache state or slicing.
// - Idle (R49): with no job alive the renderer holds no targets after trim() and does no work.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/render/TitleRenderJob.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class AssetResolver;
    class TitleContentProvider;

    // one glyph of a text layer rendered alone in a cell padded by the style's reach (S81's
    // Glyph Atlas, S85's cells): its back part (rims, glows) or front part (the face)
    struct GlyphCellRequest {
        enum Part : uint8_t { Back, Front };
        std::string layerId;
        uint32_t cluster = 0;       // the grapheme cluster index of LaidOutText
        Part part = Back;
    };

    // the pixels of a job's image (TitleImage)
    enum class TitleOutputFormat : uint8_t {
        Srgb8,              // RGBA8 straight-alpha sRGB, dithered under one code (8-bit bakes, previews)
        Srgb16,             // RGBA16F straight-alpha sRGB, undithered (16-bit bakes; RGBA8 where floats do not render)
        PremultipliedSrgb8, // RGBA8 premultiplied sRGB (displays that blend ONE, ONE_MINUS_SRC_ALPHA)
        Linear              // the accumulator itself: linear premultiplied (S81's live compose)
    };

    struct RenderRequest {
        std::shared_ptr<const TitleArtDoc> doc;     // immutable snapshot (what-if copies are snapshots too)
        float pxScale = 1.0f;                       // output pixels per canvas pixel
        TitleRectI region;                          // canvas pixels, y down from the canvas top-left; empty = canvas
        std::string isolateLayer;                   // render one layer alone, with its effects and masks
        OutputContent content = OutputContent::All;
        std::vector<std::string> contentLayers;     // OutputContent::Layers: these layer ids
        std::vector<FieldOverride> overrides;       // applied to the snapshot copy (applyFieldOverrides)
        std::vector<std::string> keepSplits;        // layer ids whose own composite is kept as a split
        std::optional<GlyphCellRequest> glyphCell;
        int tier = 0;                               // 0 = bake quality; S86's TitleTier for Realtime 3D
        int debugView3D = 0;                        // S86 render_title debug_view: 0 none, 1 normals, 2 environment
        bool includeGuides = false;                 // the editor's views draw guides; outputs never do
        // the output pass (§6.9): 8 writes straight-alpha sRGB with dither into RGBA8; 16 writes
        // straight sRGB-encoded half floats into RGBA16F (16-bit bakes, read back with 8 bytes
        // per pixel)
        int outputBitDepth = 8;
        uint32_t testFlags = 0;                     // TITLE_ART_TEST_* (TitleArtCaps.h): per request
        // the image's pixels; Srgb8 with outputBitDepth 16 writes Srgb16
        TitleOutputFormat output = TitleOutputFormat::Srgb8;
        const AssetResolver* assets = nullptr;      // fonts, images, SVGs (editor or player resolver)
    };

    struct RendererLimits {
        size_t poolBytes = 512u << 20;
        size_t nodeCacheBytes = 128u << 20;
        int maxSide = 8192;                         // larger outputs reduce their scale (problem output_downscaled)
    };

    struct RendererStats {
        uint64_t passes = 0;                // GPU passes run since construction
        uint64_t renders = 0;               // jobs finished
        size_t bytesLive = 0;               // targets lent now (pool) plus cached node pixels
        size_t peakBytes = 0;
        uint64_t cacheHits = 0, cacheMisses = 0;
        const char* path = "";              // "rgba16f" or "rgba8" of the last job
        // jump-flood passes the jobs recorded, and what unbounded fields would have taken (R43)
        uint64_t floodPasses = 0, floodPassesUnbounded = 0;
    };

    class JASPERENGINE_API TitleArtRenderer {
    public:
        explicit TitleArtRenderer(const RendererLimits& limits = {});
        ~TitleArtRenderer();
        TitleArtRenderer(const TitleArtRenderer&) = delete;
        TitleArtRenderer& operator=(const TitleArtRenderer&) = delete;

        // A job for `request`; null only when the request has no document. A job that cannot
        // render (a newer version, out of target memory) fails with error() and problems().
        std::unique_ptr<TitleRenderJob> begin(const RenderRequest& request);
        const RendererStats& stats() const;
        // frees idle targets and the node cache
        void trim();

        // the text and geometry the 2D title passes draw from (TitleContent.h); a renderer starts
        // with the process default
        void setContentProvider(std::shared_ptr<TitleContentProvider> provider);
        // tests: the next job sees a lost device (R19)
        void simulateDeviceLossForTesting();
        // the cache's entries and bytes, the pool's targets (tests, the editor's stats)
        size_t cacheEntries() const;
        const TitleTargetPoolStats& poolStats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
