// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Portions ported from OpenEngine, Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT):
// Engine/Source/Engine/Rendering/DynamicResolutionController.cpp (the GPU law)

#include "DynamicResolutionController.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{
    // below this the scalable cost is indistinguishable from zero and the area law would
    // divide by noise
    constexpr float MIN_SCALABLE_MS = 0.01f;

    // the upper bound of the banked slew budget (secondsSinceChange), so a long quiet stretch
    // cannot let the scale teleport on the first frame the load changes
    constexpr float MAX_SLEW_ACCRUAL_SECONDS = 1.0f;

    // a cost sample reflects the current scale only once the measurement has flushed (GPU
    // timestamps resolve frames late, the EMA needs a few samples): observations taken sooner
    // after a change carry the previous scale and would bias the fit toward "no response"
    constexpr float FIT_SETTLE_SECONDS = 0.15f;

    // the minimum time an accepted scale is held before the next change, an emergency drop
    // included: a position held for less than the settle time can never be measured, and a
    // change reallocates targets. Descent speed is not lost: the slew budget accrues across the
    // hold, so one change can cross many rungs
    constexpr float MIN_HOLD_SECONDS = FIT_SETTLE_SECONDS;

    // fit gates: the variance is over x = scale^2 (0.002 is about a 0.06 spread in scale around
    // mid-range); observations are per settled position, not per frame
    constexpr double MIN_FIT_OBSERVATIONS = 3.0;
    constexpr double MIN_FIT_X_VARIANCE = 0.002;

    // frame law (S13 R9): a miss is a period above 1.2 x target, a CPU-bound frame has CPU busy
    // time at least 0.8 x target, overload is 3 GPU-side misses in the window
    constexpr float MISS_FRACTION = 1.2f;
    constexpr float CPU_BOUND_FRACTION = 0.8f;
    constexpr int OVERLOAD_MISSES = 3;
    // an overload this soon after a probe rise fails it: long enough for the misses of a frame
    // rate halved by vsync to fill the window at 30 Hz, short enough not to blame the probe for
    // a later content change
    constexpr float PROBE_FAIL_SECONDS = 0.75f;
    // the content cost: EMAs of the period and the CPU busy time with this time constant (a
    // single hitch moves them a few per cent, a content change within a fraction of a second)
    constexpr float CONTENT_SMOOTHING_SECONDS = 0.2f;
    // their reference is latched this long after a change, once the EMAs forgot the old scale
    constexpr float COST_SETTLE_SECONDS = 0.8f;
    // GPU timestamps can miss a frame: hold this long before the frame law takes over
    constexpr float GPU_TIMING_GRACE_SECONDS = 0.5f;

    static_assert(PROBE_FAIL_SECONDS < MAX_SLEW_ACCRUAL_SECONDS && COST_SETTLE_SECONDS < MAX_SLEW_ACCRUAL_SECONDS,
                  "secondsSinceChange saturates at MAX_SLEW_ACCRUAL_SECONDS");

    constexpr uint8_t FRAME_MISS = 1;
    constexpr uint8_t FRAME_GPU_MISS = 2;

    bool isFinitePositive(float v){
        return std::isfinite(v) && v > 0.0f;
    }

    float finiteOr(float v, float fallback){
        return std::isfinite(v) ? v : fallback;
    }
}

const char* jasperengine::dynamicResolutionSourceName(DynamicResolutionSource source){
    switch (source){
        case DynamicResolutionSource::GPU:
            return "gpu";
        case DynamicResolutionSource::FRAME:
            return "frame";
        case DynamicResolutionSource::NONE:
            break;
    }
    return "none";
}

void DynamicResolutionController::ResponseFit::decay(double factor){
    n *= factor;
    sumX *= factor;
    sumY *= factor;
    sumXX *= factor;
    sumXY *= factor;
}

void DynamicResolutionController::ResponseFit::observe(double x, double y){
    n += 1.0;
    sumX += x;
    sumY += y;
    sumXX += x * x;
    sumXY += x * y;
}

bool DynamicResolutionController::ResponseFit::solve(float& outUnscalableMs, float& outScalableAtFullMs) const{
    if (n < MIN_FIT_OBSERVATIONS)
        return false;
    const double meanX = sumX / n;
    const double varX = sumXX / n - meanX * meanX;
    if (!(varX > MIN_FIT_X_VARIANCE))
        return false; // the scale has not moved enough to have measured a slope
    const double slope = (sumXY / n - meanX * (sumY / n)) / varX;
    const double intercept = sumY / n - slope * meanX;
    if (!std::isfinite(slope) || !std::isfinite(intercept))
        return false;
    outScalableAtFullMs = static_cast<float>(slope);
    outUnscalableMs = static_cast<float>(intercept);
    return true;
}

void DynamicResolutionController::configure(const DynamicResolutionConfig& newConfig){
    // sanitized rather than trusted: these come from scene settings, MCP and Lua, and a NaN
    // target would silently freeze the loop
    const DynamicResolutionConfig defaults{};
    config = newConfig;

    config.targetMs = finiteOr(config.targetMs, defaults.targetMs);
    if (config.targetMs <= 0.0f)
        config.targetMs = defaults.targetMs;
    config.minScale = std::clamp(finiteOr(config.minScale, defaults.minScale), 0.5f, 1.0f);
    config.maxScale = std::clamp(finiteOr(config.maxScale, defaults.maxScale), config.minScale, 1.0f);
    config.deadbandFraction = std::clamp(finiteOr(config.deadbandFraction, defaults.deadbandFraction), 0.0f, 0.9f);
    config.dropGain = std::clamp(finiteOr(config.dropGain, defaults.dropGain), 0.0f, 1.0f);
    config.raiseGain = std::clamp(finiteOr(config.raiseGain, defaults.raiseGain), 0.0f, 1.0f);
    config.maxDropPerSecond = std::max(finiteOr(config.maxDropPerSecond, defaults.maxDropPerSecond), 0.0f);
    config.maxRisePerSecond = std::max(finiteOr(config.maxRisePerSecond, defaults.maxRisePerSecond), 0.0f);
    config.dwellSeconds = std::max(finiteOr(config.dwellSeconds, defaults.dwellSeconds), 0.0f);
    config.quantizeStep = std::clamp(finiteOr(config.quantizeStep, defaults.quantizeStep), 0.001f, 0.5f);
    config.sampleSmoothing = std::clamp(finiteOr(config.sampleSmoothing, defaults.sampleSmoothing), 0.01f, 1.0f);
    config.emergencyFraction = std::max(finiteOr(config.emergencyFraction, defaults.emergencyFraction), 1.0f);
    config.minUsefulSavingFraction = std::clamp(finiteOr(config.minUsefulSavingFraction, defaults.minUsefulSavingFraction), 0.0f, 1.0f);
    config.responseDecay = std::clamp(finiteOr(config.responseDecay, defaults.responseDecay), 0.5f, 0.999f);
    config.relatchCostFraction = std::max(finiteOr(config.relatchCostFraction, defaults.relatchCostFraction), 0.01f);
    config.probeSeconds = std::clamp(finiteOr(config.probeSeconds, defaults.probeSeconds), 0.1f, 600.0f);
    config.maxProbeSeconds = std::clamp(finiteOr(config.maxProbeSeconds, defaults.maxProbeSeconds), config.probeSeconds, 3600.0f);

    scale = std::clamp(scale, config.minScale, config.maxScale);
    probeInterval = std::clamp(probeInterval, config.probeSeconds, config.maxProbeSeconds);
    stats.targetMs = config.targetMs;
    stats.probeIntervalSeconds = probeInterval;
}

void DynamicResolutionController::reset(float startScale){
    if (!isFinitePositive(startScale))
        startScale = 1.0f;
    scale = std::clamp(startScale, config.minScale, config.maxScale);
    dwellRemaining = 0.0f;
    secondsSinceChange = 0.0f;
    secondsSinceGpuSample = GPU_TIMING_GRACE_SECONDS;
    stats = Stats{};
    resetLaws();
    stats.targetMs = config.targetMs;
    stats.lastRequestedScale = scale;
}

void DynamicResolutionController::resetLaws(){
    smoothedGpuMs = 0.0f;
    hasGpuSample = false;
    fit = ResponseFit{};
    observedThisPosition = false;
    scalingIneffective = false;
    ineffectiveLatchCostMs = 0.0f;

    std::fill(std::begin(periods), std::end(periods), 0.0f);
    std::fill(std::begin(frameFlags), std::end(frameFlags), uint8_t(0));
    frameHead = 0;
    hasFrameSample = false;
    smoothedPeriodMs = 0.0f;
    smoothedCpuMs = 0.0f;
    probeInterval = config.probeSeconds;
    probeElapsed = 0.0f;
    probePending = false;
    probeReturnPending = false;
    probeFromScale = scale;
    costRefValid = false;
    refPeriodMs = 0.0f;
    refCpuMs = 0.0f;

    stats.smoothedMs = 0.0f;
    stats.smoothedCpuBusyMs = 0.0f;
    stats.saturatedLow = false;
    stats.saturatedHigh = false;
    stats.responseFitValid = false;
    stats.scalingIneffective = false;
    stats.estimatedUnscalableMs = 0.0f;
    stats.estimatedFullRangeSavingMs = 0.0f;
    stats.probeIntervalSeconds = probeInterval;
    stats.probeRemainingSeconds = 0.0f;
    stats.missedFrames = 0;
    stats.cpuBound = false;
}

float DynamicResolutionController::quantizeScale(float value) const{
    if (!std::isfinite(value))
        return config.maxScale;
    value = std::clamp(value, config.minScale, config.maxScale);

    // the ladder is anchored at maxScale and steps down, so the top of the range is always
    // exactly reachable; minScale stays reachable through the final clamp, so it need not be a
    // rung. Nearest rung: the GPU law's damped correction near maxScale is a fraction of a rung,
    // which rounding downward would cancel forever. Half a rung of overshoot is far inside the
    // deadband
    const float rungs = std::round((config.maxScale - value) / config.quantizeStep);
    return std::clamp(config.maxScale - rungs * config.quantizeStep, config.minScale, config.maxScale);
}

float DynamicResolutionController::limitToSlew(float requested, float maxStep) const{
    float next = quantizeScale(std::clamp(requested, scale - maxStep, scale + maxStep));
    // the nearest rung can land up to half a step beyond the slew bound, and with frequent
    // changes that bias accumulates into a breach of the per-second contract: step back toward
    // the scale instead, and wait for the budget to accrue when even that is too far
    if (std::fabs(next - scale) > maxStep + 1e-6f){
        next = std::clamp(next > scale ? next - config.quantizeStep : next + config.quantizeStep, config.minScale, config.maxScale);
        if (std::fabs(next - scale) > maxStep + 1e-6f)
            return scale;
    }
    return next;
}

void DynamicResolutionController::accept(float newScale){
    if (newScale < scale)
        stats.acceptedDrops++;
    else
        stats.acceptedRises++;
    stats.acceptedChanges++;

    scale = newScale;
    dwellRemaining = config.dwellSeconds;
    stats.dwellRemainingSeconds = dwellRemaining;
    secondsSinceChange = 0.0f;
    observedThisPosition = false;

    // the frame law's window, probe timer and cost reference describe the previous scale
    std::fill(std::begin(frameFlags), std::end(frameFlags), uint8_t(0));
    probeElapsed = 0.0f;
    probePending = false;
    probeReturnPending = false;
    costRefValid = false;
}

float DynamicResolutionController::update(const DynamicResolutionSample& sample, float deltaSeconds, uint64_t frameIndex){
    if (hasFrameIndex && frameIndex == lastFrameIndex)
        return scale;
    hasFrameIndex = true;
    lastFrameIndex = frameIndex;
    return update(sample, deltaSeconds);
}

float DynamicResolutionController::update(const DynamicResolutionSample& sample, float deltaSeconds){
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        deltaSeconds = 0.0f;
    stats.ticks++;
    dwellRemaining = std::max(dwellRemaining - deltaSeconds, 0.0f);
    stats.dwellRemainingSeconds = dwellRemaining;
    secondsSinceChange = std::min(secondsSinceChange + deltaSeconds, MAX_SLEW_ACCRUAL_SECONDS);
    secondsSinceGpuSample = std::min(secondsSinceGpuSample + deltaSeconds, GPU_TIMING_GRACE_SECONDS);

    // a missing or nonsensical sample holds the scale: measurements resolve late and can drop
    // out (a query pool cap, a frame that never presented); holding is correct, guessing is not
    if (!sample.valid)
        return scale;

    DynamicResolutionSource source;
    if (isFinitePositive(sample.gpuMs)){
        source = DynamicResolutionSource::GPU;
        secondsSinceGpuSample = 0.0f;
    }else if (secondsSinceGpuSample < GPU_TIMING_GRACE_SECONDS || !isFinitePositive(sample.framePeriodMs)){
        return scale;
    }else{
        source = DynamicResolutionSource::FRAME;
    }
    // the other law's history means nothing to this one
    if (source != stats.source){
        resetLaws();
        stats.source = source;
    }

    // the scale reaches nothing this frame: moving it would be pure quality loss with no
    // response to learn from, and any cost change seen now belongs to something else
    stats.scaleDisconnected = !sample.scaleIsConnected;
    if (!sample.scaleIsConnected)
        return scale;

    return source == DynamicResolutionSource::GPU ? updateGpu(sample) : updateFrame(sample, deltaSeconds);
}

float DynamicResolutionController::updateGpu(const DynamicResolutionSample& sample){
    if (!hasGpuSample){
        smoothedGpuMs = sample.gpuMs;
        hasGpuSample = true;
    }else{
        smoothedGpuMs += config.sampleSmoothing * (sample.gpuMs - smoothedGpuMs);
    }
    stats.smoothedMs = smoothedGpuMs;

    const float target = config.targetMs;
    const float error = smoothedGpuMs - target;
    const bool emergency = smoothedGpuMs > target * config.emergencyFraction;

    // measured response: one observation per settled position, not per frame. The fit is a
    // regression over scale, so its memory counts distinct scales; sampling every frame would
    // let a long stay at one scale flood it and collapse the x variance the slope depends on.
    // x is the applied scale: the renderer snaps the extent to pixels, and fitting against the
    // request would model a resolution it never rendered
    if (!observedThisPosition && secondsSinceChange >= FIT_SETTLE_SECONDS){
        const float observedScale = isFinitePositive(sample.appliedScale) ? sample.appliedScale : scale;
        observedThisPosition = true;
        fit.decay(config.responseDecay);
        fit.observe(static_cast<double>(observedScale) * observedScale, smoothedGpuMs);
    }

    float fitUnscalable = 0.0f;
    float fitScalable = 0.0f;
    stats.responseFitValid = fit.solve(fitUnscalable, fitScalable);
    if (stats.responseFitValid){
        const float fullRangeSaving = fitScalable * (config.maxScale * config.maxScale - config.minScale * config.minScale);
        stats.estimatedUnscalableMs = std::clamp(fitUnscalable, 0.0f, smoothedGpuMs);
        stats.estimatedFullRangeSavingMs = fullRangeSaving;

        // a latched verdict is revisited only when the content changes enough to invalidate it
        if (scalingIneffective){
            if (std::fabs(smoothedGpuMs - ineffectiveLatchCostMs) > ineffectiveLatchCostMs * config.relatchCostFraction){
                scalingIneffective = false;
                fit = ResponseFit{}; // the old slope described different content
            }
        }else if (config.minUsefulSavingFraction > 0.0f && fullRangeSaving < target * config.minUsefulSavingFraction){
            // geometry-bound (fill-insensitive) content: driving the scale to the floor would
            // buy almost nothing, so hand the resolution back
            scalingIneffective = true;
            ineffectiveLatchCostMs = smoothedGpuMs;
        }
    }
    stats.scalingIneffective = scalingIneffective;

    if (scalingIneffective && scale >= config.maxScale - 1e-4f){
        stats.lastRequestedScale = config.maxScale;
        stats.saturatedLow = false;
        stats.saturatedHigh = false;
        return scale; // already native: nothing to give back
    }

    // the deadband keeps the loop from chasing noise; it must not gate the walk back to native
    // once the scale has been judged useless
    if (!scalingIneffective && std::fabs(error) <= target * config.deadbandFraction){
        stats.lastRequestedScale = scale;
        stats.saturatedLow = false;
        stats.saturatedHigh = false;
        return scale;
    }

    // ideal scale: cost(s) = U + S * s^2 lands on target at s' = s * sqrt((target - U) / (cost - U)).
    // With U unknown (0) that under-corrects whenever U > 0, which is the safe direction: it
    // converges geometrically instead of overshooting. The sample's split wins, else the fit's U
    float unscalable = 0.0f;
    if (isFinitePositive(sample.unscalableGpuMs))
        unscalable = std::clamp(sample.unscalableGpuMs, 0.0f, smoothedGpuMs);
    else if (stats.responseFitValid)
        unscalable = std::clamp(fitUnscalable, 0.0f, smoothedGpuMs);

    const float scalableCost = std::max(smoothedGpuMs - unscalable, MIN_SCALABLE_MS);
    const float scalableTarget = target - unscalable;

    float ideal;
    if (scalingIneffective){
        // reduced resolution buys nothing for this content: give it back even over budget
        ideal = config.maxScale;
    }else if (scalableTarget <= MIN_SCALABLE_MS){
        // the fixed cost alone misses the target: go to the floor (it still buys real time)
        // and let saturatedLow surface it
        ideal = config.minScale;
    }else{
        ideal = scale * std::sqrt(scalableTarget / scalableCost);
    }
    ideal = std::clamp(ideal, config.minScale, config.maxScale);
    stats.lastRequestedScale = ideal;

    // gain, slew limit (accrued since the last accepted change: most frames change nothing, and
    // a per-frame budget smaller than a rung could never cross one), quantization
    const bool dropping = ideal < scale;
    const float gain = dropping ? config.dropGain : config.raiseGain;
    const float desired = scale + gain * (ideal - scale);
    const float maxStep = (dropping ? config.maxDropPerSecond : config.maxRisePerSecond) * secondsSinceChange;
    float quantized = limitToSlew(desired, maxStep);

    // guaranteed progress: the damped correction can fall below half a rung and quantize back
    // to the scale while the error is still outside the deadband. Then take exactly one rung
    // toward the ideal, when that rung does not step past it
    if (quantized == scale && ideal != scale){
        const float rung = dropping ? scale - config.quantizeStep : scale + config.quantizeStep;
        const bool overshoots = dropping ? rung < ideal : rung > ideal;
        const bool withinSlew = std::fabs(rung - scale) <= maxStep + 1e-6f;
        if (!overshoots && withinSlew)
            quantized = std::clamp(rung, config.minScale, config.maxScale);
    }

    // saturation: pinned against a limit with the error still pointing past it
    stats.saturatedLow = !scalingIneffective && scale <= config.minScale + 1e-4f && error > 0.0f;
    stats.saturatedHigh = scale >= config.maxScale - 1e-4f && error < 0.0f;

    // only a change the way the error asked: from an off-ladder scale (after a reset) the snap
    // can land on the wrong side of it
    if (quantized == scale || (dropping && quantized > scale) || (!dropping && quantized < scale))
        return scale;

    if (secondsSinceChange < MIN_HOLD_SECONDS)
        return scale;

    // the dwell: an emergency drop goes through (the frame rate outranks target churn), a rise
    // never does (a premature rise turns a settled loop back into an oscillating one)
    if (dwellRemaining > 0.0f && !(emergency && quantized < scale))
        return scale;

    accept(quantized);
    return scale;
}

float DynamicResolutionController::updateFrame(const DynamicResolutionSample& sample, float deltaSeconds){
    const float target = config.targetMs;
    const float period = sample.framePeriodMs;
    const float cpuBusy = isFinitePositive(sample.cpuBusyMs) ? sample.cpuBusyMs : 0.0f;

    // the content cost: slow EMAs, so a single hitch does not read as new content
    if (!hasFrameSample){
        smoothedPeriodMs = period;
        smoothedCpuMs = cpuBusy;
        hasFrameSample = true;
    }else{
        const float weight = deltaSeconds / (CONTENT_SMOOTHING_SECONDS + deltaSeconds);
        smoothedPeriodMs += weight * (period - smoothedPeriodMs);
        smoothedCpuMs += weight * (cpuBusy - smoothedCpuMs);
    }
    stats.smoothedMs = smoothedPeriodMs;
    stats.smoothedCpuBusyMs = smoothedCpuMs;

    // the window of the last frames since the last change
    const bool miss = period > target * MISS_FRACTION;
    const bool cpuBound = cpuBusy >= target * CPU_BOUND_FRACTION;
    periods[frameHead] = period;
    frameFlags[frameHead] = static_cast<uint8_t>((miss ? FRAME_MISS : 0) | (miss && !cpuBound ? FRAME_GPU_MISS : 0));
    frameHead = (frameHead + 1) % PERIOD_WINDOW;

    int misses = 0;
    int gpuMisses = 0;
    float gpuMissMs = 0.0f;
    for (int i = 0; i < PERIOD_WINDOW; i++){
        if (frameFlags[i] & FRAME_MISS)
            misses++;
        if (frameFlags[i] & FRAME_GPU_MISS){
            gpuMisses++;
            gpuMissMs += periods[i];
        }
    }
    const bool overload = gpuMisses >= OVERLOAD_MISSES;
    stats.missedFrames = gpuMisses;
    stats.cpuBound = misses >= OVERLOAD_MISSES && !overload;

    // any miss, CPU-bound or not, restarts the wait for a probe
    probeElapsed = miss ? 0.0f : std::min(probeElapsed + deltaSeconds, config.maxProbeSeconds);

    // content change at a held scale: the backoff learned on the old content no longer applies
    if (!costRefValid){
        if (secondsSinceChange >= COST_SETTLE_SECONDS){
            refPeriodMs = smoothedPeriodMs;
            refCpuMs = smoothedCpuMs;
            costRefValid = true;
        }
    }else if (std::fabs(smoothedPeriodMs - refPeriodMs) > refPeriodMs * config.relatchCostFraction ||
              std::fabs(smoothedCpuMs - refCpuMs) > refCpuMs * config.relatchCostFraction){
        probeInterval = config.probeSeconds;
        refPeriodMs = smoothedPeriodMs;
        refCpuMs = smoothedCpuMs;
    }

    // a probe rise that overloads within its window failed
    if (probePending && secondsSinceChange >= PROBE_FAIL_SECONDS)
        probePending = false;
    if (overload && probePending){
        probePending = false;
        probeReturnPending = true;
        stats.failedRises++;
        probeInterval = std::min(probeInterval * 2.0f, config.maxProbeSeconds);
    }
    if (!overload)
        probeReturnPending = false;

    stats.saturatedLow = overload && scale <= config.minScale + 1e-4f;
    stats.saturatedHigh = !overload && scale >= config.maxScale - 1e-4f;

    if (overload){
        float requested;
        bool throughDwell;
        if (probeReturnPending){
            // the rung the probe left was holding: return to it at once
            requested = probeFromScale;
            throughDwell = true;
        }else{
            // the area law on the mean period of the GPU-side misses, damped by the drop gain;
            // under vsync a period is quantized up to whole intervals, which the gain and the
            // unknown fixed cost (it makes the law under-correct) partly offset
            const float ratio = gpuMissMs / static_cast<float>(gpuMisses) / target;
            const float ideal = scale / std::sqrt(ratio);
            const float rungs = std::max(1.0f, std::round(config.dropGain * (scale - ideal) / config.quantizeStep));
            requested = std::max(scale - rungs * config.quantizeStep, config.minScale);
            throughDwell = ratio >= config.emergencyFraction;
        }
        stats.lastRequestedScale = requested;

        const bool held = secondsSinceChange < MIN_HOLD_SECONDS || (dwellRemaining > 0.0f && !throughDwell);
        if (requested < scale && !held){
            const float next = limitToSlew(requested, config.maxDropPerSecond * secondsSinceChange);
            if (next < scale)
                accept(next);
        }
    }else if (scale < config.maxScale - 1e-4f && probeElapsed >= probeInterval){
        // headroom cannot be seen under vsync: probe one rung up
        const float requested = std::min(scale + config.quantizeStep, config.maxScale);
        stats.lastRequestedScale = requested;
        if (secondsSinceChange >= MIN_HOLD_SECONDS && dwellRemaining <= 0.0f){
            const float next = limitToSlew(requested, config.maxRisePerSecond * secondsSinceChange);
            if (next > scale){
                const float from = scale;
                accept(next);
                probeFromScale = from;
                probePending = true;
            }
        }
    }else{
        stats.lastRequestedScale = scale;
    }

    stats.probeIntervalSeconds = probeInterval;
    stats.probeRemainingSeconds = scale < config.maxScale - 1e-4f ? std::max(probeInterval - probeElapsed, 0.0f) : 0.0f;
    return scale;
}
