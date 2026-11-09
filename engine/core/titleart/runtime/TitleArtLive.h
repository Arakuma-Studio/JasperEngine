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
// - Live compose (R9, TitleLiveCompose.h): a full render of a document with live Exposed
//   parameters keeps the inputs their effects start from with its image; a later state that
//   changes only such parameters (and nothing else of the render) is composed in one frame over
//   them, not time-sliced, and swaps in at that frame's end. Over the live budget, after a live
//   effect followed by one without a live variant, or when the plan could not start from the
//   inputs, it re-renders as any change (the reason is in the status).
// - Costs (R15, S80 Deviation 54): with the scheduler's TitleStepCosts the steps are packed and
//   split by measured, adaptive predictions; without it by the static estimates times the
//   platform factor.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "subsystem/TitleArtRuntime.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/render/TitleRenderJob.h"
#include "titleart/render/TitleTargetPool.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Framebuffer;
    class Texture;
    class TextureRender;
}

namespace jasperengine::titleart {

    class TitleStepCosts;
    class TitleArtRenderer;
    class AssetResolver;
    class TitleGlyphAtlas;
    struct TitleMesh;

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
        // R9: the inputs a live compose of a later state starts from (kept by the render), the
        // field overrides the image shows and the rest of its render (what a change is compared with)
        TitleLiveInputs liveInputs;
        std::vector<FieldOverride> overrides;
        uint64_t renderKey = 0;
        // what live composes changed since the render that kept the inputs (TitleLiveCompose.h)
        std::map<std::string, int> liveTouched;
        size_t liveBytes() const;
        // S86 R63: the meshes of its Dimension 3D layers (the render's)
        std::vector<TitleArtMeshStats> meshes;
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
        int tier = 0;                   // S86's TitleTier (RenderRequest::tier)
        FxTier fxTier = FxTier::Desktop;    // S83's device tier (RenderRequest::fxTier)
        std::string label;
        // R9: the live inputs a full render keeps, and those a live compose starts from
        std::vector<std::string> keepLive;
        TitleLiveInputs liveInputs;
        bool live = false;              // a live compose (it starts from liveInputs)
        // Glyph Atlas (R25-R29): the title's atlas and plan; null renders as Realtime
        std::shared_ptr<TitleGlyphAtlas> atlas;
        TitleGlyphAtlasPlan glyphPlan;
        uint64_t docHash = 0;           // the document's content hash (the atlas style's)
        std::string titlePath;          // the title's path (its prebaked atlas sits beside it)
        // S85 R15: the per-glyph values of the Glyph Atlas layer's cells (null when none animates)
        std::shared_ptr<const std::map<uint32_t, GlyphPose>> glyphPoses;
        // S85 R14, R15: a Realtime title composes its moving layers and glyphs (spec.atlas caches)
        std::shared_ptr<const TitleComposePose> compose;
        // S86 R26, R38: what the build's export cooked for the title (RenderRequest's members)
        std::map<std::string, std::shared_ptr<const TitleMesh>> cookedMeshes3D;
        std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> cookedEnvironments3D;
    };

    // S86 R26, R38: a title's cooked 3D data from its export (three_d/TitleCooked3D.cpp): what a
    // job at `tier` hands the renderer
    class JASPERENGINE_API TitleCooked3DSource {
    public:
        virtual ~TitleCooked3DSource() = default;
        virtual void fill(int tier, TitleJobSpec& spec) = 0;
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
        // predicts, measures and teaches with `model` from now on (jobs of S80's renderer)
        virtual void useCosts(TitleStepCosts* model) { (void)model; }
        // a live compose's job: true once the plan started from every live input it was given
        virtual bool composesLive() const { return false; }
    };

    using TitleJobFactory = std::function<std::unique_ptr<TitleJob>(const TitleJobSpec&)>;

    // the jobs of S80's renderer
    JASPERENGINE_API TitleJobFactory makeRendererJobFactory(TitleArtRenderer& renderer, const AssetResolver* assets);

    // R20: the scheduler-wide limits the live titles share
    struct JASPERENGINE_API TitleLiveLimits {
        size_t cacheBytes = 64u << 20;      // shown-before images (R14) of every title together
        double retryDelays[3] = {2.0, 4.0, 8.0};
    };

    // what one frame's scheduling runs with (TitleArtScheduler::schedule)
    struct JASPERENGINE_API TitleScheduleContext {
        TitleJobFactory factory;
        uint32_t budgetUs = 0;              // R15: the frame's
        double platformFactor = 1.0;        // §6.3: static estimates x this when there is no model
        // measured, adaptive predictions (S80 Deviation 54; null: the static estimates)
        TitleStepCosts* costs = nullptr;
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
        // the document has a Dimension 3D layer (S86: its tier matters)
        bool has3D() const { return hasLayers3D; }
        // what the export cooked for it (a build's; none in the editor)
        void setCooked3D(std::shared_ptr<TitleCooked3DSource> source) { cooked3D = std::move(source); }
        // S87 R17: the document resolved for `code` ("" = the source) becomes the one rendered
        // (Glyph Atlas: its plan made again and a new atlas for it); false when it already is
        bool setLanguage(const std::string& code);
        const std::string& language() const { return languageCode; }

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
        // the next step's prediction in us (the model's, else the estimate x the factor)
        uint32_t nextCostUs(const TitleScheduleContext& context) const;
        // runs one step (starting the job when needed); its cost in us (the larger of its
        // prediction and what it measured) is added to `spentUs`. False when nothing ran (the next
        // step waits for its shaders, or the job is complete until endFrame swaps it in)
        bool runStep(const TitleScheduleContext& context, double& spentUs);
        // once a frame with a model: the running job's steps cut again by what the model learned
        void resplit(const TitleScheduleContext& context);
        // R9: one title's live budget (ms) a live change is predicted against (S83 §6.4)
        void setLiveBudgetMs(double ms) { liveBudget = ms; }
        // R9: a live compose waits (it runs before the frame's slices, not time-sliced)
        bool liveWaiting() const { return liveQueued; }
        // runs it: true when its job completed (it swaps in at endFrame); false when it waits for
        // its shaders, or when it fell back to a re-render (queued like any change)
        bool runLive(const TitleScheduleContext& context);
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
        // S85 R18: an animation's re-render values (the field overrides every request of the state
        // adds, part of its hash) and whether an animation plays (the title's share of the budget)
        void setAnimationOverrides(std::vector<FieldOverride> overrides, uint64_t hash){
            animOverrides = std::move(overrides);
            animOverridesHash = hash;
        }
        const std::vector<FieldOverride>& animationOverrides() const { return animOverrides; }
        uint64_t animationOverridesHash() const { return animOverridesHash; }
        void setAnimating(bool on, double share){ animatingFlag = on; budgetShare = share; }
        // R15: a Glyph Atlas title's glyph values (null: none)
        void setGlyphPoses(std::shared_ptr<const std::map<uint32_t, GlyphPose>> poses){ glyphPoses = std::move(poses); }
        // R18 prerender: the sample times still to render ahead (TitleArtRuntime::prepareAnimation)
        struct PrerenderFrame {
            std::vector<FieldOverride> overrides;
            uint64_t hash = 0;
        };
        void setPrerenderFrames(std::vector<PrerenderFrame> frames){
            prerenderFrames = std::move(frames);
            prerenderNext = 0;
        }
        bool prerendering() const { return prerenderNext < prerenderFrames.size(); }
        const PrerenderFrame& nextPrerenderFrame() const { return prerenderFrames[prerenderNext]; }
        void advancePrerender(){ prerenderNext++; }
        // R14, R15: a Realtime title's moving layers and glyphs (null: the normal render)
        void setComposePose(std::shared_ptr<const TitleComposePose> pose){ composePose = std::move(pose); }
        bool animating() const { return animatingFlag; }
        double animationShare() const { return budgetShare; }
        // the animation library the overrides were planned for (TitleArtRuntime::setAnimation)
        std::shared_ptr<void> animationPlanCache;
        const void* animationPlanKey = nullptr;
        // R55: the characters of the state its Characters set leaves out (they draw as nothing)
        void setMissingCharacters(std::string characters) { missingCharacters = std::move(characters); }
        const TitleGlyphAtlasPlan& glyphAtlasPlan() const { return glyphPlan; }

        // tests: the step and frame counters
        uint64_t stepsRun() const { return totalSteps; }
        int restarts() const { return restartCount; }
        // full renders and live composes that swapped in
        uint64_t rendersDone() const { return renderCount; }
        uint64_t liveComposesDone() const { return liveCount; }
        // R9: the inputs full renders of the document keep (TitleLiveCompose.h)
        const std::vector<std::string>& liveKeeps() const { return keepLive; }


    private:
        struct Running {
            std::unique_ptr<TitleJob> job;
            uint64_t hash = 0;
            TitleArtLiveState state;
            std::vector<FieldOverride> overrides;
            uint64_t renderKey = 0;
            int frames = 0;
            uint32_t estimatedUs = 0;
            double cpuMs = 0.0;
            int overBudget = 0;
            bool started = false;
            bool live = false;          // a live compose (R9)
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
        std::vector<FieldOverride> animOverrides;
        uint64_t animOverridesHash = 0;
        std::shared_ptr<const std::map<uint32_t, GlyphPose>> glyphPoses;
        std::shared_ptr<const TitleComposePose> composePose;
        std::vector<PrerenderFrame> prerenderFrames;
        size_t prerenderNext = 0;
        bool animatingFlag = false;
        double budgetShare = 1.0;
        std::shared_ptr<const TitleArtDoc> doc;
        uint64_t docHash = 0;
        std::vector<std::string> keepLive;      // R9: what full renders keep
        TitleStepCosts* jobCosts = nullptr;     // the model the running job was given
        bool liveQueued = false;                // a live compose of wantedHash waits
        double liveBudget = 1.0;
        std::vector<std::string> liveInputNames;
        std::map<std::string, int> liveChanged;     // the waiting compose's touched layers (front's included)
        std::string liveReason;                 // why the last live param change re-rendered
        uint64_t renderCount = 0, liveCount = 0;
        bool hasLayers3D = false;
        std::shared_ptr<TitleCooked3DSource> cooked3D;
        // S87: the document as stored, and the language `doc` is resolved for
        std::shared_ptr<const TitleArtDoc> sourceDoc;
        std::string languageCode;

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
        // R9: the change from the front image to the wanted state is live (with its inputs)
        bool liveChange();
        uint64_t renderKeyOf(const TitleArtLiveState& state) const;
        TitleJobSpec specOf() const;
        void startJob(const TitleScheduleContext& context);
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
