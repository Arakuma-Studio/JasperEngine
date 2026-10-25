// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Portions ported from OpenEngine, Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT):
// Engine/Include/Engine/Rendering/DynamicResolutionController.h

#ifndef DYNAMICRESOLUTIONCONTROLLER_H
#define DYNAMICRESOLUTIONCONTROLLER_H

#include "Export.h"
#include <cstdint>

namespace jasperengine{

    // The settings of DynamicResolutionController. configure() sanitizes them, so values from
    // scene settings, MCP or Lua can be passed as they are.
    struct DynamicResolutionConfig{
        // the frame time the controller steers toward, ms: the GPU law compares the GPU time
        // with it, the frame law the frame period
        float targetMs = 16.667f;

        // the scale range (each clamped to 0.5..1, max >= min); maxScale is renderScale
        float minScale = 0.6f;
        float maxScale = 1.0f;

        // GPU law: no action while |cost - target| <= target * deadbandFraction (above the
        // frame-to-frame GPU noise, so the scale does not chatter)
        float deadbandFraction = 0.10f;

        // GPU law: the fraction of the computed correction applied per accepted change, and the
        // frame law's fraction of its area-law drop. Asymmetric on purpose: drop decisively to
        // protect the frame rate, rise gently so the loop cannot pump against its own cost change
        float dropGain = 0.90f;
        float raiseGain = 0.25f;

        // hard slew limits in scale units per second (down is 8x up, for the same reason)
        float maxDropPerSecond = 1.20f;
        float maxRisePerSecond = 0.15f;

        // the minimum time an accepted scale holds before another change: every change
        // reallocates the uber source, SSAO/SSR and chain targets, so changes must be rare
        float dwellSeconds = 0.5f;

        // the ladder rung: scales are maxScale - n * quantizeStep (and minScale), which bounds
        // the number of distinct target sizes
        float quantizeStep = 0.05f;

        // GPU law: the EMA weight of incoming GPU samples (1 = no smoothing)
        float sampleSmoothing = 0.35f;

        // a cost (GPU law: the smoothed GPU time; frame law: the mean period of the missed
        // frames) above target * emergencyFraction drops through the dwell. Never through the
        // slew limit or the minimum hold, and a rise never bypasses the dwell
        float emergencyFraction = 1.50f;

        // GPU law: when driving the scale from maxScale to minScale is predicted (by the response
        // fit cost ~ U + S * scale^2) to save less than targetMs * minUsefulSavingFraction, the
        // lever is judged ineffective for the content (geometry-bound) and the controller returns
        // to maxScale instead of spending resolution for nothing. 0 disables the check
        float minUsefulSavingFraction = 0.15f;
        // GPU law: the per-observation decay of the response fit (memory 1 / (1 - decay)
        // settled positions)
        float responseDecay = 0.92f;
        // a cost moving this fraction away from its reference means the content changed: it
        // clears the GPU law's ineffective latch, and resets the frame law's probe interval
        float relatchCostFraction = 0.25f;

        // frame law: the seconds without a missed frame before it probes one rung up, and the
        // cap of the interval, which each failed probe doubles
        float probeSeconds = 2.0f;
        float maxProbeSeconds = 16.0f;
    };

    // One frame's measurement. The renderer fills what the backend has; the controller picks
    // the GPU law when gpuMs > 0 and the frame law otherwise.
    struct DynamicResolutionSample{
        // GPU busy time of the frame, ms (S15 timer queries); 0 when the backend has none. Not a
        // wall-clock period: that folds in vsync waits and CPU-bound idle
        float gpuMs = 0.0f;
        // the part of gpuMs that does not follow the scale (post chain, UI, shadows); 0 = unknown,
        // which degrades the GPU law to its damped form rather than making it wrong
        float unscalableGpuMs = 0.0f;
        // frame law: the wall-clock time between this frame's start and the previous one's, ms
        float framePeriodMs = 0.0f;
        // frame law: the CPU busy time of the frame, ms (frame start to submit end, excluding the
        // present wait); 0 = unknown, read as not CPU-bound
        float cpuBusyMs = 0.0f;
        // the scale the renderer actually rendered at when this was measured (after its pixel
        // snapping); 0 = unknown, the controller's own scale is used
        float appliedScale = 0.0f;
        // whether the scale reaches anything this frame (the post-processed main camera renders
        // through the uber source); false holds the scale and teaches nothing
        bool scaleIsConnected = true;
        // false holds the scale (no measurement this frame)
        bool valid = false;
    };

    // the control law of the last update that had a usable measurement
    enum class DynamicResolutionSource{
        NONE,   // none since the reset
        GPU,    // GPU time (the GPU law)
        FRAME   // frame period and CPU busy time (the frame law)
    };

    // "none", "gpu" or "frame" (the MCP / Lua status value)
    JASPERENGINE_API const char* dynamicResolutionSourceName(DynamicResolutionSource source);

    // Dynamic resolution control law (S13 D5): measurements and delta time in, a render scale
    // out. Pure: no globals, no GPU state, no engine time, no allocation, so its stability
    // (deadband, asymmetric rates, slew limit, dwell, quantization) is tested against synthetic
    // cost models.
    //
    // GPU law (OpenEngine's): an EMA of the GPU time is steered to the target with the area law
    // scale' = scale * sqrt((target - U) / (cost - U)), damped by the gains, slew limited, held by
    // the dwell, snapped to the ladder; U comes from the sample or an online fit of cost against
    // scale^2, which also latches "ineffective" for content the scale does not move.
    //
    // Frame law (ours, for backends without GPU timing): the frame period cannot show headroom
    // under vsync, so it reacts to misses and probes upward:
    //  - a frame misses when its period is above 1.2 x target; it is CPU-bound when its CPU busy
    //    time is at least 0.8 x target. Overload is 3 or more GPU-side (not CPU-bound) misses in
    //    the last 8 frames since the last change; a CPU-bound overload never drops.
    //  - on overload it drops by the area law on the mean period r of the GPU-side misses (target
    //    units): dropGain * (scale - scale / sqrt(r)), rounded to rungs, at least one. r at or
    //    above emergencyFraction drops through the dwell (a 2x load step lands within ~0.2 s).
    //  - after probeInterval seconds without any miss it rises one rung. An overload within
    //    0.75 s of that rise fails the probe: the scale returns to the rung it left (through the
    //    dwell) and the interval doubles, up to maxProbeSeconds.
    //  - the interval resets to probeSeconds when the content cost changes: a slow EMA (0.2 s) of
    //    the period or of the CPU busy time moves more than relatchCostFraction away from its
    //    value latched 0.8 s after the last change (at a held scale).
    // A backend with GPU timing that misses a frame's timestamps holds the scale for 0.5 s before
    // the frame law takes over.
    class JASPERENGINE_API DynamicResolutionController{
    public:
        // frames in the frame law's overload window
        static constexpr int PERIOD_WINDOW = 8;

        struct Stats{
            DynamicResolutionSource source = DynamicResolutionSource::NONE;
            // GPU law: the EMA of the GPU time; frame law: the slow EMA of the frame period, ms
            float smoothedMs = 0.0f;
            // frame law: the slow EMA of the CPU busy time, ms
            float smoothedCpuBusyMs = 0.0f;
            float targetMs = 0.0f;
            float lastRequestedScale = 1.0f;    // before quantization, slew and dwell
            float dwellRemainingSeconds = 0.0f;
            // update() calls taken (after the frame dedup): about one per app frame, above that
            // several views tick it and dwell, slew and EMA run at N x real time
            uint64_t ticks = 0;
            uint32_t acceptedDrops = 0;
            uint32_t acceptedRises = 0;
            uint32_t acceptedChanges = 0;       // drops + rises
            // at minScale and still over the target: the cost that does not scale alone misses it
            bool saturatedLow = false;
            // at maxScale with headroom (the healthy case)
            bool saturatedHigh = false;

            // GPU law: the measured response of the cost to the scale
            bool scaleDisconnected = false;     // the last sample said the scale reaches nothing
            bool responseFitValid = false;      // enough spread in the observed scales to solve
            bool scalingIneffective = false;    // latched: the scale buys too little to be worth it
            float estimatedUnscalableMs = 0.0f; // fitted U
            float estimatedFullRangeSavingMs = 0.0f; // fitted saving from maxScale to minScale

            // frame law
            float probeIntervalSeconds = 0.0f;  // the current probe interval
            float probeRemainingSeconds = 0.0f; // until the next probe (0 at maxScale)
            uint32_t failedRises = 0;
            int missedFrames = 0;               // GPU-side misses in the window
            bool cpuBound = false;              // the window misses, but CPU-bound: no drop
        };

        void configure(const DynamicResolutionConfig& newConfig);
        const DynamicResolutionConfig& getConfig() const { return config; }

        // drops all adaptation state and restarts at startScale (mode change, view resize, scene
        // load, fixed-resolution toggle: the cost history is meaningless across them). The
        // frame dedup survives it
        void reset(float startScale);

        // advances one frame; returns the scale to apply (getScale())
        float update(const DynamicResolutionSample& sample, float deltaSeconds);
        // the same, once per app frame: a frameIndex equal to the previous call's is ignored
        // (returns the scale), so several views ticking it do not run it at N x speed
        float update(const DynamicResolutionSample& sample, float deltaSeconds, uint64_t frameIndex);

        float getScale() const { return scale; }
        const Stats& getStats() const { return stats; }

        // snaps value to the nearest ladder rung (maxScale - n * quantizeStep), clamped to
        // minScale..maxScale. Nearest, not toward minScale: the GPU law's correction near
        // maxScale is a fraction of a rung, which a downward rounding would cancel forever
        float quantizeScale(float value) const;

    private:
        // exponentially weighted least squares of cost against x = scale^2: cost ~ U + S * x,
        // U the resolution-independent cost, S what the scale controls
        struct ResponseFit{
            double n = 0.0;
            double sumX = 0.0;
            double sumY = 0.0;
            double sumXX = 0.0;
            double sumXY = 0.0;

            void decay(double factor);
            void observe(double x, double y);
            bool solve(float& outUnscalableMs, float& outScalableAtFullMs) const;
        };

        float updateGpu(const DynamicResolutionSample& sample);
        float updateFrame(const DynamicResolutionSample& sample, float deltaSeconds);
        // the ladder rung nearest requested within maxStep of the scale (the scale when none is)
        float limitToSlew(float requested, float maxStep) const;
        void accept(float newScale);
        void resetLaws();

        DynamicResolutionConfig config{};
        Stats stats{};
        float scale = 1.0f;
        float dwellRemaining = 0.0f;
        // the slew budget accrues over the time since the last accepted change, not per frame,
        // so a rung wider than one frame's budget is still reachable
        float secondsSinceChange = 0.0f;
        float secondsSinceGpuSample = 0.0f;

        bool hasFrameIndex = false;
        uint64_t lastFrameIndex = 0;

        // GPU law
        float smoothedGpuMs = 0.0f;
        bool hasGpuSample = false;
        ResponseFit fit{};
        bool observedThisPosition = false;
        bool scalingIneffective = false;
        float ineffectiveLatchCostMs = 0.0f;

        // frame law
        float periods[PERIOD_WINDOW] = {};
        uint8_t frameFlags[PERIOD_WINDOW] = {};
        int frameHead = 0;
        bool hasFrameSample = false;
        float smoothedPeriodMs = 0.0f;
        float smoothedCpuMs = 0.0f;
        float probeInterval = 2.0f;
        float probeElapsed = 0.0f;
        bool probePending = false;          // a probe rise inside its failure window
        bool probeReturnPending = false;    // a failed probe waiting to return to probeFromScale
        float probeFromScale = 1.0f;
        bool costRefValid = false;
        float refPeriodMs = 0.0f;
        float refCpuMs = 0.0f;
    };

}

#endif //DYNAMICRESOLUTIONCONTROLLER_H
