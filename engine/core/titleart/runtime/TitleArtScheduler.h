// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R13, R15, R20, R21, §6.3; S56 device tiers)
// - Purpose: the engine's one Title Art scheduler, shared by every scene. Each frame, before the
//   camera passes, it runs whole steps of the queued renders until the frame budget is spent:
//   higher priority first, then visible before hidden, then the oldest request. A hidden title
//   gets no steps unless it renders when hidden. At least one step runs in a frame where a
//   runnable request waits, so progress is guaranteed; a step over the budget runs alone.
// - Budgets come from S56's platform group (QualityResolver::getPlatformGroup: Desktop, Mobile,
//   Web) with the project's Title Art settings (title_art.frame_budget_ms, release_delay_s,
//   cache_budget_mib); the editor's Edit mode has its own frame budget. Estimates are S80's (pixels
//   x costWeight, the desktop reference); what the scheduler packs and splits steps by is S80's
//   measured cost model (TitleStepCosts, Deviation 54: one model of the device per engine, the
//   editor's previews have their own with their 4 ms), whose prior is the platform factor (mobile
//   6, web 4, desktop 1): the first measurements replace it, so slicing follows the device, not
//   desktop figures. A step after the frame's first starts only while the frame's measured time
//   plus its prediction fits; a model is made again when the platform group or the device changes.
// - Live composes (R9) run first, whole, for every title that has one, outside the frame budget
//   (each is held to its own live budget when it is decided, TitleLiveCompose.h).
// - Memory (R20): the renderer's target pool is trimmed after the Release Delay without a render;
//   shown-before images of every title stay within the Cache Budget (oldest evicted first).
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "device/DeviceTypes.h"
#include "titleart/runtime/TitleArtLive.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleArtRenderer;
    class AssetResolver;
    class TitleStepCosts;

    // Project Settings > Title Art (R58): per platform group, the editor's Edit mode apart
    struct JASPERENGINE_API TitleArtRuntimeSettings {
        double frameBudgetMs[DEVICE_PLATFORM_GROUP_COUNT] = {4.0, 2.0, 2.0};   // desktop, mobile, web
        double editorFrameBudgetMs = 8.0;
        double releaseDelaySeconds[DEVICE_PLATFORM_GROUP_COUNT] = {10.0, 3.0, 3.0};
        double cacheBudgetMiB[DEVICE_PLATFORM_GROUP_COUNT] = {64.0, 16.0, 16.0};
        int referenceHeight = 1080;
        bool simulateExportCharacters = true;
        // the project's assets directory, project-relative ("assets/"): a document's asset
        // references are relative to it
        std::string assetsPrefix = "assets/";
    };

    // §6.3: estimates are the desktop reference's; a slower platform's steps cost more
    JASPERENGINE_API double titlePlatformFactor(DevicePlatformGroup group);
    // S86's quality tier for "auto": the device's S56 tier (Low, Medium, High/Ultra -> high)
    JASPERENGINE_API const char* titleQualityTierFor(DeviceTier tier);

    // what one frame of the scheduler did (tests, the profiler)
    struct JASPERENGINE_API TitleFrameReport {
        uint32_t steps = 0;
        double spentUs = 0.0;
        uint32_t overBudgetSteps = 0;
        // steps after the frame's first that took it over the budget: they started on a
        // prediction that was too low (what trusting desktop estimates does on a slow device)
        uint32_t lateStarts = 0;
        uint32_t liveComposes = 0;      // R9: live composes that completed this frame
        std::vector<const TitleArtLive*> order;     // the titles that got steps, in the order they ran
    };

    class JASPERENGINE_API TitleArtScheduler {
    public:
        static TitleArtScheduler& get();

        void add(TitleArtLive* live);
        void remove(TitleArtLive* live);
        const std::vector<TitleArtLive*>& entries() const { return lives; }

        // RenderSystem before the cameras (TitleArtRuntime::runFrame)
        void runFrame(double dt, bool editorEditMode);
        // one frame's steps over `entries` with this factory and budget (tests drive it directly)
        static TitleFrameReport schedule(const std::vector<TitleArtLive*>& entries, const TitleJobFactory& factory, uint32_t budgetUs,
                                         double platformFactor);
        static TitleFrameReport schedule(const std::vector<TitleArtLive*>& entries, const TitleScheduleContext& context);

        // the frame budget now (ms): a script's (setFrameBudget), else the settings' for the
        // platform group or the editor's Edit mode
        double frameBudgetMs(bool editorEditMode) const;
        void setFrameBudgetOverride(double ms);
        void configure(const TitleArtRuntimeSettings& settings);
        const TitleArtRuntimeSettings& settings() const { return config; }
        DevicePlatformGroup platformGroup() const;
        // R9: one title's live budget now (the platform group's)
        double liveBudgetMs() const;
        // the runtime's cost model of this device (null before the first frame with work)
        const TitleStepCosts* costModel() const { return costs.get(); }

        // the renderer every live title renders with (made on first use)
        TitleArtRenderer& renderer();
        bool hasRenderer() const { return rendererInstance != nullptr; }
        // the assets the documents name: the editor's (its asset database) or the player's
        void setAssetResolver(std::shared_ptr<const AssetResolver> resolver);
        const AssetResolver* assetResolver();
        // tests: a job factory instead of the renderer's
        void setJobFactoryForTesting(TitleJobFactory factory);
        // tests: the clock (seconds) the retries and the release delay read
        void setClockForTesting(double seconds);

        // the pool's live bytes (get_title, get_vram_report)
        size_t poolBytes() const;
        // every title's GPU side is gone (device loss, the engine's end)
        void releaseAll();

    private:
        TitleArtScheduler();
        std::vector<TitleArtLive*> lives;
        TitleArtRuntimeSettings config;
        double budgetOverrideMs = 0.0;
        std::unique_ptr<TitleArtRenderer> rendererInstance;
        std::shared_ptr<const AssetResolver> resolver;
        bool playerResolver = false;    // the resolver is the default one (made again on a new prefix)
        TitleJobFactory testFactory;
        double clock = 0.0;
        bool testClock = false;
        double idleSeconds = 0.0;
        uint64_t contextId = 0;
        TitleLiveLimits limits;
        std::unique_ptr<TitleStepCosts> costs;
        DevicePlatformGroup costsGroup = DevicePlatformGroup::Desktop;
    };

}
