// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R13, R14, R18, R19, R44, R49, §6.3)
// - Purpose: what one TitleArtRenderer shares between its jobs: the GPU layer, the target pool,
//   the cache of computed results (coverage uploads, distance fields, heights, shaded stacks,
//   node pixels, LUTs), the counters, and the formats of the job being planned or run.
// - Cache (R14): entries keyed by 64-bit content keys (TitleArtHash's per-layer hashes chained
//   with what a result depends on: frame, px_scale, path), held by shared_ptr so a job keeps
//   what it reads alive while the cache may drop it; least recently used entries go first when
//   the bytes pass RendererLimits::nodeCacheBytes. Entries are immutable once inserted: a job
//   inserts a result only when the step that completes it ran.
// - Device loss (R19): reset() drops the pool's targets, every entry and every program; refs
//   held by jobs then read null and their next GPU step fails with `device_lost`.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleContent.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitleTargetPool.h"

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

namespace jasperengine::titleart {

    struct JASPERENGINE_API TitleCacheEntry {
        uint64_t key = 0;
        size_t bytes = 0;
        TargetRef target;                       // a rendered result, or
        std::unique_ptr<TitleUpload> upload;    // an uploaded one (coverage, LUT, picture)
        TitleRectI rect;                        // node pixels: the rect relative to the node's anchor
        bool positionFree = true;               // node pixels: valid at any whole-pixel offset
        std::array<float, 4> encoding{};        // scalar fields: their encoding descriptor
        float value = 0.0f;                     // results with a scalar of their own

        TextureRender* texture() const;
        size_t measure() const;
    };

    class JASPERENGINE_API TitleRenderCache {
    public:
        explicit TitleRenderCache(size_t limitBytes) : limit(limitBytes) {}

        // the entry of `key` (made most recently used), or null
        std::shared_ptr<TitleCacheEntry> find(uint64_t key);
        // stores a finished entry (replacing one of the same key) and evicts past the limit
        void insert(const std::shared_ptr<TitleCacheEntry>& entry);
        void clear();
        size_t bytes() const { return total; }
        size_t size() const { return entries.size(); }
        void setLimit(size_t bytes) { limit = bytes; }
        uint64_t hits = 0, misses = 0;

    private:
        size_t limit;
        size_t total = 0;
        std::list<std::shared_ptr<TitleCacheEntry>> order;      // most recent first
        std::unordered_map<uint64_t, std::list<std::shared_ptr<TitleCacheEntry>>::iterator> entries;

        void evict();
    };

    class JASPERENGINE_API TitleRenderServices {
    public:
        explicit TitleRenderServices(const RendererLimits& limits);
        ~TitleRenderServices();

        TitleGpu gpu;
        SokolTitleTargetAllocator allocator;
        TitleTargetPool pool;
        TitleRenderCache cache;
        TitleArtCaps caps;                      // of the job being planned (formats are per request)
        RendererLimits limits;
        std::shared_ptr<TitleContentProvider> content;
        uint64_t contextId = 0;                 // SystemRender::getContextId() the GPU objects belong to
        uint64_t generation = 1;                // bumped by every reset(): jobs of an older one fail
        uint32_t testFlags = 0;
        bool simulatedLoss = false;             // tests: the next check sees a new context
        // S86's 3D layer renderer (three_d/Title3DRenderer.h), made by the first 3D layer planned
        std::shared_ptr<void> title3D;

        // a target for a buffer kind of the current caps; empty with `error`
        TargetRef acquire(TitleBuffer buffer, int width, int height, std::string* error = nullptr);
        TargetKey keyFor(TitleBuffer buffer, int width, int height) const;
        // true when the device was lost since the GPU objects were made (then reset() ran)
        bool checkDevice();
        void reset();
        // a gradient's 256 x 1 RGBA8 LUT, a curve's (16-bit in RG), cached by content
        std::shared_ptr<TitleCacheEntry> gradientLut(const TitleGradient& gradient);
        // the LUT texture of an element: rows fill gradient, finish gradient, gloss curve, satin contour
        std::shared_ptr<TitleCacheEntry> elementLuts(const TitleGradient& fill, const TitleGradient& finish,
                                                     const TitleCurve* gloss, const TitleCurve* satin);
        std::shared_ptr<TitleCacheEntry> curveLut(const TitleCurve& curve);
    };

    // ---- LUT baking (CPU; render/TitleLuts.cpp) ----

    // 256 RGBA8 texels of a gradient: sRGB-encoded stops lerped as Pachislop's code does
    // (interp `rgb`) or through OKLCH on the shorter hue arc (`vivid`), straight stop alpha,
    // Photoshop midpoints
    JASPERENGINE_API void bakeGradient(const TitleGradient& gradient, uint8_t out[256 * 4]);
    // 256 texels of a curve as 16-bit values in RG (B 0, A 255)
    JASPERENGINE_API void bakeCurve16(const TitleCurve& curve, uint8_t out[256 * 4]);
    JASPERENGINE_API uint64_t gradientKey(const TitleGradient& gradient);
    JASPERENGINE_API uint64_t curveKey(const TitleCurve& curve);

}
