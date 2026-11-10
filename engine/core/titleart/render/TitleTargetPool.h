// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R13, R19, R44, R49, §6.3 "Targets")
// - Purpose: the program's one pool of render targets (S81's Realtime intermediates and S86's 3D
//   passes take theirs from it too): keyed by (width, height, format, filter, depth format,
//   sample count), handed out as move-only RAII TargetRefs that return their target on
//   destruction (no manual release).
// - Rules: contents are undefined on acquire (a pass that needs a clear clears). A target unused
//   for TITLE_TARGET_IDLE_RENDERS complete renders is freed (endRender() counts them). The pool
//   never exceeds its byte limit: an acquire beyond it frees idle targets first (least recently
//   used), then fails (problem `out_of_target_memory`), never exhausting the engine's GPU pools.
//   clear() drops every target (device loss: SystemRender::getContextId() changed); a TargetRef
//   held across it reads null and returns nothing. The 2D passes request neither depth nor
//   MSAA; S86's 3D passes request them under their own keys and hand plain colour targets back
//   to the compositor.
// - The GPU objects come from a TitleTargetAllocator (the sokol one in render/, a fake in tests).
// - Thread-safety: main thread (the render thread of the jobs' GPU steps).

#pragma once

#include "Export.h"
#include "render/Render.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class TextureRender;
}

namespace jasperengine::titleart {

    constexpr int TITLE_TARGET_IDLE_RENDERS = 3;

    enum class TitleDepthFormat : uint8_t { None, Depth, DepthStencil };

    struct TargetKey {
        int width = 0;
        int height = 0;
        ColorFormat format = ColorFormat::RGBA;
        TextureFilter filter = TextureFilter::LINEAR;
        TitleDepthFormat depth = TitleDepthFormat::None;
        int samples = 1;

        friend bool operator==(const TargetKey& a, const TargetKey& b) {
            return a.width == b.width && a.height == b.height && a.format == b.format && a.filter == b.filter &&
                   a.depth == b.depth && a.samples == b.samples;
        }
        friend bool operator!=(const TargetKey& a, const TargetKey& b) { return !(a == b); }
    };

    // bytes per texel of a colour format
    JASPERENGINE_API size_t colorFormatBytes(ColorFormat format);
    // whether a colour format is a float one (RGBA16F, RGBA32F, R32F, RG16F, R16F)
    JASPERENGINE_API bool isFloatColorFormat(ColorFormat format);
    // the GPU memory of a target: colour x samples, plus a single-sample resolve when
    // multisampled, plus depth (4 bytes per sample)
    JASPERENGINE_API size_t targetBytes(const TargetKey& key);

    // One GPU render target (the allocator's own subclass holds the framebuffer and its textures)
    class JASPERENGINE_API TitleTarget {
    public:
        virtual ~TitleTarget() = default;
        // the colour texture passes render into: what the editor reads back (bakes, checks,
        // previews, FrameReadback::readTexture) and draws; null when the allocator has none
        virtual TextureRender* colorTexture() const { return nullptr; }
    };

    class JASPERENGINE_API TitleTargetAllocator {
    public:
        virtual ~TitleTargetAllocator() = default;
        // null with `error` when the GPU cannot make it
        virtual std::unique_ptr<TitleTarget> create(const TargetKey& key, std::string* error) = 0;
        virtual size_t bytes(const TargetKey& key) const { return targetBytes(key); }
    };

    class TitleTargetPool;

    // A target lent by the pool; returns it on destruction or reset()
    class JASPERENGINE_API TargetRef {
    public:
        TargetRef() = default;
        TargetRef(TargetRef&& other) noexcept;
        TargetRef& operator=(TargetRef&& other) noexcept;
        TargetRef(const TargetRef&) = delete;
        TargetRef& operator=(const TargetRef&) = delete;
        ~TargetRef();

        // null when empty, failed, or the pool was cleared since
        TitleTarget* get() const;
        template<class T> T* as() const { return static_cast<T*>(get()); }
        explicit operator bool() const { return get() != nullptr; }
        const TargetKey& key() const { return targetKey; }
        void reset();

    private:
        friend class TitleTargetPool;
        TitleTargetPool* pool = nullptr;
        size_t slot = 0;
        uint64_t generation = 0;
        TargetKey targetKey;
    };

    struct TitleTargetPoolStats {
        size_t bytesLive = 0;       // lent out now
        size_t bytesIdle = 0;       // held for reuse
        size_t peakBytes = 0;       // the most lent out at once since resetPeak()
        size_t targets = 0;         // allocated (live + idle)
        uint64_t acquires = 0;
        uint64_t reuses = 0;        // acquires served by an idle target
        uint64_t allocations = 0;
        uint64_t failures = 0;
        uint64_t depthOrMsaaAcquires = 0;   // acquires with a depth format or samples > 1
        uint64_t floatAcquires = 0;         // acquires of a float colour format (the RGBA8 path makes none)
    };

    class JASPERENGINE_API TitleTargetPool {
    public:
        TitleTargetPool(TitleTargetAllocator& allocator, size_t maxBytes);
        ~TitleTargetPool();
        TitleTargetPool(const TitleTargetPool&) = delete;
        TitleTargetPool& operator=(const TitleTargetPool&) = delete;

        // an idle target of this key or a new one; an empty ref with `error`
        // ("out_of_target_memory: ..." or the allocator's message) when it cannot
        TargetRef acquire(const TargetKey& key, std::string* error = nullptr);
        // one complete render ended: targets idle for TITLE_TARGET_IDLE_RENDERS renders are freed
        void endRender();
        // frees every idle target (TitleArtRenderer::trim)
        void trim();
        // drops every target, lent or idle (device loss); lent refs read null from now on
        void clear();

        void setMaxBytes(size_t bytes) { limit = bytes; }
        size_t maxBytes() const { return limit; }
        const TitleTargetPoolStats& stats() const { return counters; }
        void resetPeak() { counters.peakBytes = counters.bytesLive; }

    private:
        friend class TargetRef;
        struct Slot {
            std::unique_ptr<TitleTarget> target;
            TargetKey key;
            size_t bytes = 0;
            bool lent = false;
            uint64_t lastUse = 0;       // the render count when it was last returned
            uint64_t useOrder = 0;      // LRU among idle targets
        };
        TitleTargetAllocator& allocator;
        size_t limit = 0;
        std::vector<Slot> slots;
        std::vector<size_t> freeSlots;
        uint64_t generation = 1;
        uint64_t renders = 0;
        uint64_t useClock = 0;
        TitleTargetPoolStats counters;

        void giveBack(size_t slot, uint64_t refGeneration);
        TitleTarget* targetOf(size_t slot, uint64_t refGeneration) const;
        void freeSlot(size_t slot);
        bool evictIdleFor(size_t bytes);
    };

}
