// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R9-R24, §6.3)
// - Purpose: one Realtime (or Glyph Atlas) title inside the `titleart` module: the state it must
//   show, its request (R12), the render job that makes it (S80's TitleRenderJob through a
//   TitleJob), the double buffer (R17), the cache of images shown before (R14, R20), readiness
//   (R18), failures and retries (R19) and the pixel path (R21). TitleArtScheduler runs its steps.
// - Requests: a state whose hash equals the displayed image's is dropped; one held in the cache
//   swaps in at the next frame without rendering; else it queues, replacing a queued one. A new
//   state while rendering restarts the render when under half of its estimated cost is done,
//   else the render finishes and the latest state renders next (R12).
// - Double buffering: a render writes its own target; when its last step completes the image
//   becomes the front (the companion's), the old front the previous one (crossfade) and then a
//   cache entry. The companion never shows a half-made image.
// - Failures (R19): the previous image stays; an allocation failure retries after 2, 4 and 8 s
//   and then waits for a new state; a float target that fails while the caps said it would
//   work switches the session to the RGBA8 path once and restarts.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "subsystem/TitleArtRuntime.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleRenderJob.h"
#include "titleart/render/TitleTargetPool.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Framebuffer;
    class Texture;
    class TextureRender;
}

namespace jasperengine::titleart {

    class TitleArtRenderer;
    class AssetResolver;
    class TitleGlyphAtlas;

    // S81's flags in S80's per-request set (RenderRequest::testFlags, after TITLE_ART_TEST_S80_LAST_BIT)
    constexpr uint32_t TITLE_ART_TEST_FAIL_TARGETS = 1u << 6;   // every target allocation of the request fails
    constexpr uint32_t TITLE_ART_TEST_SLOW_STEPS = 1u << 7;     // every step's estimate x 64 (tiles, over-budget steps)
    constexpr uint32_t TITLE_ART_SLOW_STEP_FACTOR = 64;

    // a finished image of a title: the display target (S80's output encoding, straight-alpha sRGB)
    struct JASPERENGINE_API TitleDisplayImage {
        // what owns the target's pool when it is not the renderer's (a Glyph Atlas title's
        // atlas): declared first, so it outlives the target
        std::shared_ptr<void> owner;
        TargetRef target;
        int width = 0, height = 0;
        TitleRectI frame;               // canvas pixels the image covers
        size_t bytes = 0;
        uint64_t hash = 0;              // the state it shows
        // the glow output of the same state (R35), when it was asked for
        TargetRef glow;
        int glowWidth = 0, glowHeight = 0;
        // a prebaked image shipped with the build (R11): a texture of the file, no target
        std::shared_ptr<Texture> prebaked;
        bool valid() const { return width > 0 && height > 0; }
        // the colour texture and the framebuffer of a GPU target (null for a test's image)
        TextureRender* texture() const;
        Framebuffer* framebuffer() const;
        Framebuffer* glowFramebuffer() const;
    };

    // What a job is asked to render
    struct JASPERENGINE_API TitleJobSpec {
        std::shared_ptr<const TitleArtDoc> doc;
        std::vector<FieldOverride> overrides;
        float pxScale = 1.0f;
        TitleRectI region;              // canvas pixels, image space; empty = the canvas
        uint32_t testFlags = 0;
        bool glow = false;              // also the glow output (half resolution)
        int tier = 0;
        std::string label;
        // Glyph Atlas (R25-R29): the title's atlas and plan; null renders as Realtime
        std::shared_ptr<TitleGlyphAtlas> atlas;
        TitleGlyphAtlasPlan glyphPlan;
        uint64_t docHash = 0;           // the document's content hash (the atlas style's)
        std::string titlePath;          // the title's path (its prebaked atlas sits beside it)
    };

    // The scheduler's view of one render (S80's TitleRenderJob in a game; a fake in tests)
    class JASPERENGINE_API TitleJob {
    public:
        virtual ~TitleJob() = default;
        // whole steps while their estimates fit (always one that is ready); true when done
        virtual bool step(const StepBudget& budget, StepReport* report) = 0;
        virtual bool done() const = 0;
        virtual bool failed() const = 0;
        virtual bool waiting() const = 0;
        virtual float progress() const = 0;
        virtual uint32_t nextStepCostUs() const = 0;
        virtual void splitOversizedSteps(uint32_t maxUs) = 0;
        virtual uint32_t passesDone() const = 0;
        virtual uint32_t passesTotal() const = 0;
        virtual const std::string& error() const = 0;
        // the job ran out of target memory (a retry may work), the job's float target failed
        virtual bool allocationFailure() const = 0;
        virtual bool floatTargetFailure() const = 0;
        // "rgba16f" or "rgba8"
        virtual const char* pathName() const = 0;
        // the finished image (moved out once)
        virtual TitleDisplayImage takeImage() = 0;
        // the shaders it binds (S81 R18, R47)
        virtual std::vector<ShaderKey> shaderKeys() const = 0;
    };

    using TitleJobFactory = std::function<std::unique_ptr<TitleJob>(const TitleJobSpec&)>;

    // the jobs of S80's renderer
    JASPERENGINE_API TitleJobFactory makeRendererJobFactory(TitleArtRenderer& renderer, const AssetResolver* assets);

    // R20: the scheduler-wide limits the live titles share
    struct JASPERENGINE_API TitleLiveLimits {
        size_t cacheBytes = 64u << 20;      // shown-before images (R14) of every title together
        double retryDelays[3] = {2.0, 4.0, 8.0};
    };

    class JASPERENGINE_API TitleArtLive {
    public:
        TitleArtLive(std::string titlePath, TitleArtRenderMode mode, std::shared_ptr<const TitleArtDoc> doc, uint64_t documentHash);
        ~TitleArtLive();
        TitleArtLive(const TitleArtLive&) = delete;
        TitleArtLive& operator=(const TitleArtLive&) = delete;

        const std::string& titlePath() const { return path; }
        TitleArtRenderMode mode() const { return renderMode; }
        const std::shared_ptr<const TitleArtDoc>& document() const { return doc; }
        uint64_t documentHash() const { return docHash; }

        // ---- the state (TitleArtSystem, once a frame) ----
        void setState(const TitleArtLiveState& state, uint64_t stateHash, std::vector<FieldOverride> overrides);
        const TitleArtLiveState& state() const { return wanted; }
        uint64_t stateHash() const { return wantedHash; }
        // render the current state again (Lua refresh, rerender, device loss)
        void invalidate();
        // a prebaked image of `hash` (R11): shown at once while it is the state
        void setPrebaked(TitleDisplayImage image);

        // ---- the scheduler's side ----
        // something to render, and whether it may run now (visible or renderWhenHidden)
        bool hasWork() const;
        bool runnable() const;
        int priority() const { return wanted.priority; }
        bool visible() const { return wanted.visible; }
        uint64_t requestOrder() const { return requestSerial; }
        // the next step's estimate in us (scaled by the platform factor), 0 when nothing runs
        uint32_t nextCostUs(double platformFactor) const;
        // runs one step (starting the job when needed); its estimated cost in us is added to
        // `spentUs` (platform factor applied). False when nothing ran (the next step waits for
        // its shaders, or the job is complete until endFrame swaps it in)
        bool runStep(const TitleJobFactory& factory, double platformFactor, uint32_t budgetUs, double& spentUs);
        // the running job finished its steps (it swaps in at endFrame)
        bool jobDone() const { return running.job && running.job->done(); }
        // at the end of the frame's steps: finished images swap in (R17), cache hits swap in,
        // the crossfade advances, retries wait (`now` in seconds)
        void endFrame(double now, double dt, TitleLiveLimits& limits, size_t& cacheBytesInUse);
        // the cache entries' bytes (the scheduler evicts across titles)
        size_t cacheBytes() const;
        // drops the oldest cache entry; false when none
        bool evictOldest();
        // the GPU side is gone (device loss): images dropped, the state renders again
        void dropGpu();

        // ---- what it shows ----
        const TitleDisplayImage& front() const { return frontImage; }
        const TitleDisplayImage* previous(float* blend) const;
        TitleArtLiveStatus status() const;

        // Glyph Atlas (R25, R30, R31): what its document gives per glyph; a title that falls back
        // renders as Realtime (the plan's `fallback` says why)
        void setGlyphPlan(TitleGlyphAtlasPlan plan) { glyphPlan = std::move(plan); }
        // the atlas of a Glyph Atlas title that renders per glyph (null before its first job, and
        // for a title that falls back to Realtime)
        const std::shared_ptr<TitleGlyphAtlas>& glyphAtlas() const { return atlas; }
        // R55: the characters of the state its Characters set leaves out (they draw as nothing)
        void setMissingCharacters(std::string characters) { missingCharacters = std::move(characters); }
        const TitleGlyphAtlasPlan& glyphAtlasPlan() const { return glyphPlan; }

        // tests: the step and frame counters
        uint64_t stepsRun() const { return totalSteps; }
        int restarts() const { return restartCount; }

    private:
        struct Running {
            std::unique_ptr<TitleJob> job;
            uint64_t hash = 0;
            TitleArtLiveState state;
            std::vector<FieldOverride> overrides;
            int frames = 0;
            uint32_t estimatedUs = 0;
            double cpuMs = 0.0;
            int overBudget = 0;
            bool started = false;
        };
        struct Cached {
            TitleDisplayImage image;
            uint64_t lastUse = 0;
        };

        std::string path;
        TitleArtRenderMode renderMode;
        TitleGlyphAtlasPlan glyphPlan;
        // before the images: their targets may be its pool's (TitleDisplayImage::owner keeps it too)
        std::shared_ptr<TitleGlyphAtlas> atlas;
        std::string missingCharacters;
        std::shared_ptr<const TitleArtDoc> doc;
        uint64_t docHash = 0;

        TitleArtLiveState wanted;
        uint64_t wantedHash = 0;
        std::vector<FieldOverride> wantedOverrides;
        bool hasWanted = false;
        bool queued = false;            // a request for wantedHash waits
        uint64_t requestSerial = 0;     // age order (R13)
        Running running;
        bool cacheSwapPending = false;

        TitleDisplayImage frontImage;
        TitleDisplayImage previousImage;
        float fade = 1.0f;              // crossfade 0..1 (1: the front alone)
        std::list<Cached> cache;
        uint64_t useClock = 0;

        // status
        bool failedState = false;
        std::string failure;
        uint64_t failedHash = 0;
        int retries = 0;
        double retryAt = -1.0;
        bool retryWaiting = false;
        double clock = 0.0;
        std::string path8;              // the last job's path
        bool shadersWaiting = false;
        int lastFrames = 0;
        double lastGpuMs = 0.0;
        double lastCpuMs = 0.0;
        int lastOverBudget = 0;
        uint64_t totalSteps = 0;
        int restartCount = 0;

        void queueWanted();
        void startJob(const TitleJobFactory& factory, uint32_t budgetUs, double platformFactor);
        void finishJob(TitleLiveLimits& limits, size_t& cacheBytesInUse);
        // the error by value: it is often the failed job's own, and the job goes first
        void failJob(std::string error, bool allocation, bool floatTarget);
        void pushToCache(TitleDisplayImage image);
        bool takeFromCache(uint64_t hash, TitleDisplayImage& out);
    };

    // R5: instance values (the component's overrides) as the S80 field overrides of the document's
    // Exposed targets; an override of a name the document does not expose is skipped
    JASPERENGINE_API std::vector<FieldOverride> titleArtFieldOverrides(const TitleArtDoc& doc, const std::vector<TitleArtOverride>& overrides);

    // the session's pixel path (R21): forced to RGBA8 by JASPER_TITLEART_FORCE_RGBA8=1, the
    // Android property debug.jasperengine.titleart_rgba8, or a float target that failed
    JASPERENGINE_API bool titleArtForceRgba8();
    JASPERENGINE_API void setTitleArtForceRgba8(bool force, const char* reason);

}
