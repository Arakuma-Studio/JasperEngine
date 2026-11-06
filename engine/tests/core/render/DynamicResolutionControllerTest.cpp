// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Portions ported from OpenEngine, Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT):
// Engine/Tests/DynamicResolutionControllerTests.cpp

// The dynamic resolution control law (S13 AC7, AC8, AC22). The controller is a pure function
// of (cost sample, dt): every test drives it against a closed-loop synthetic cost model where
// changing the scale changes the cost being measured, the feedback that makes dynamic
// resolution oscillate when it is built wrong. The GPU law's tests are OpenEngine's; the frame
// law's (PeriodLaw*) are ours.

#include "render/DynamicResolutionController.h"

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace jasperengine;

namespace{
constexpr float DT = 1.0f / 60.0f;

// cost(s) = unscalable + scalableAtFull * s^2. The square is the point: the
// lever moves internal pixel AREA, and the unscalable term is the post-TAA
// chain / UI / shadow raster that render at fixed resolution no matter what
// the scale does.
struct SyntheticGpu
{
    float unscalable = 4.0f;
    float scalableAtFull = 12.0f;

    float cost(float scale) const { return unscalable + scalableAtFull * scale * scale; }
};

DynamicResolutionSample makeSample(float gpuMs, float unscalableMs = 0.0f)
{
    DynamicResolutionSample s{};
    s.gpuMs = gpuMs;
    s.unscalableGpuMs = unscalableMs;
    s.valid = true;
    return s;
}

// Mechanics config: deadband / slew / dwell / quantize behaviour. Response
// detection is OFF here because most of these tests feed a cost that does not
// depend on the scale, and a cost that ignores the lever is by definition a
// dead lever — the detector would (correctly) hand the resolution back and mask
// the mechanic under test. The detector has its own tests, which use
// responseConfig() and closed-loop models.
DynamicResolutionConfig testConfig()
{
    DynamicResolutionConfig c{};
    c.targetMs = 10.0f;
    // OpenEngine's range and ladder (the engine's are 0.6 and 0.05)
    c.minScale = 0.5f;
    c.quantizeStep = 0.02f;
    c.dwellSeconds = 0.10f;
    c.minUsefulSavingFraction = 0.0f;
    return c;
}

// Production shape: the response detector armed at its default threshold.
DynamicResolutionConfig responseConfig()
{
    DynamicResolutionConfig c = testConfig();
    c.minUsefulSavingFraction = DynamicResolutionConfig{}.minUsefulSavingFraction;
    return c;
}

// The first accepted change cannot land until the minimum hold has elapsed
// (a position has to be measurable, and each change reallocates pool targets).
// Drive frames until it lands and return the resulting scale.
float driveUntilFirstChange(DynamicResolutionController& c, float costMs, int maxFrames = 240)
{
    const float start = c.getScale();
    for (int i = 0; i < maxFrames; ++i)
    {
        c.update(makeSample(costMs), DT);
        if (c.getScale() != start)
            break;
    }
    return c.getScale();
}

// Run the closed loop: measure the model at the current scale, feed it back.
// Returns the scale trace.
std::vector<float> runClosedLoop(DynamicResolutionController& c, const SyntheticGpu& model,
                                 int frames, bool provideUnscalableHint = false)
{
    std::vector<float> trace;
    trace.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        const float cost = model.cost(c.getScale());
        c.update(makeSample(cost, provideUnscalableHint ? model.unscalable : 0.0f), DT);
        trace.push_back(c.getScale());
    }
    return trace;
}

int countChanges(const std::vector<float>& trace, size_t from)
{
    int changes = 0;
    for (size_t i = from + 1; i < trace.size(); ++i)
        if (trace[i] != trace[i - 1])
            ++changes;
    return changes;
}
} // namespace

// ── Applied vs requested scale ─────────────────────────────────────────────

TEST(DynamicResolutionController, DisconnectedLeverHoldsAndLearnsNothing)
{
    // No post-processed view: the render scale is wired to nothing. Moving it
    // would be pure quality loss, and any cost change seen meanwhile belongs to
    // something else — so hold, and do not let those samples teach the response
    // fit that scaling "does not work".
    DynamicResolutionConfig cfg = responseConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(cfg.maxScale);

    for (int i = 0; i < 600; ++i)
    {
        DynamicResolutionSample s = makeSample(40.0f); // far over a 10 ms target
        s.scaleIsConnected = false;
        c.update(s, DT);
    }
    EXPECT_FLOAT_EQ(c.getScale(), cfg.maxScale) << "must not move a lever that is not connected";
    EXPECT_TRUE(c.getStats().scaleDisconnected);
    EXPECT_FALSE(c.getStats().responseFitValid) << "disconnected frames must not feed the fit";
    EXPECT_FALSE(c.getStats().scalingIneffective)
        << "'no post-processed view' is not the same finding as 'scaling does not help here'";

    // Reconnecting must let it work normally again.
    const SyntheticGpu fillBound{3.0f, 24.0f};
    runClosedLoop(c, fillBound, 1500);
    EXPECT_LT(c.getScale(), cfg.maxScale);
    EXPECT_FALSE(c.getStats().scaleDisconnected);
}

TEST(DynamicResolutionController, ResponseFitUsesAppliedScaleNotRequested)
{
    // The pipeline snaps the internal extent to even pixels and only splits
    // when TAA is active, so the applied ratio is not the requested scale. The
    // fit must model the resolution actually rendered. Here the renderer
    // applies HALF of whatever is requested; a fit against the request would
    // recover a wrong U, a fit against the applied ratio recovers the right one.
    const SyntheticGpu model{6.0f, 20.0f};
    DynamicResolutionConfig cfg = responseConfig();
    // The fit only conditions once the controller has had reason to visit a
    // spread of scales -- a loop that converges in two steps never measures a
    // slope, which is a real property of the design and not a test artifact.
    // A target just above the 6 ms unscalable floor keeps it sweeping the whole
    // range, which is also exactly the situation where the fit matters.
    cfg.targetMs = 6.5f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(cfg.maxScale);

    for (int i = 0; i < 2000; ++i)
    {
        const float applied = c.getScale() * 0.5f;
        DynamicResolutionSample s = makeSample(model.cost(applied));
        s.appliedScale = applied;
        c.update(s, DT);
    }
    ASSERT_TRUE(c.getStats().responseFitValid);
    EXPECT_NEAR(c.getStats().estimatedUnscalableMs, model.unscalable, 1.5f)
        << "fitted U = " << c.getStats().estimatedUnscalableMs
        << " — the fit modelled the requested scale, not the rendered one";
}

// ── Basic response ─────────────────────────────────────────────────────────

TEST(DynamicResolutionController, InvalidSampleHoldsScale)
{
    DynamicResolutionController c;
    c.configure(testConfig());
    c.reset(0.8f);

    DynamicResolutionSample none{};
    none.valid = false;
    EXPECT_FLOAT_EQ(c.update(none, DT), 0.8f);

    // The measurement resolves several frames late and can legitimately drop
    // out; a NaN or non-positive reading must hold, never be acted on.
    EXPECT_FLOAT_EQ(c.update(makeSample(std::nanf("")), DT), 0.8f);
    EXPECT_FLOAT_EQ(c.update(makeSample(0.0f), DT), 0.8f);
    EXPECT_FLOAT_EQ(c.update(makeSample(-3.0f), DT), 0.8f);
}

TEST(DynamicResolutionController, DeadbandSuppressesChatter)
{
    DynamicResolutionConfig cfg = testConfig();
    cfg.deadbandFraction = 0.10f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(0.9f);

    // Anything within +/-10% of the 10 ms target is "on target".
    for (int i = 0; i < 200; ++i)
    {
        const float wobble = (i % 2 == 0) ? 10.9f : 9.1f;
        EXPECT_FLOAT_EQ(c.update(makeSample(wobble), DT), 0.9f)
            << "in-deadband cost must never move the ladder (frame " << i << ")";
    }
}

TEST(DynamicResolutionController, OverBudgetDropsAndUnderBudgetRises)
{
    DynamicResolutionController c;
    c.configure(testConfig());
    c.reset(0.9f);

    for (int i = 0; i < 60; ++i)
        c.update(makeSample(20.0f), DT);
    EXPECT_LT(c.getScale(), 0.9f) << "sustained over-budget must reduce the scale";
    const float dropped = c.getScale();

    for (int i = 0; i < 600; ++i)
        c.update(makeSample(3.0f), DT);
    EXPECT_GT(c.getScale(), dropped) << "sustained headroom must give resolution back";
}

TEST(DynamicResolutionController, ResponseIsAsymmetric)
{
    // Same relative error in both directions; the drop must travel further per
    // unit time than the rise. Protecting frame rate is urgent, spending
    // headroom is not.
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 0.0f;
    cfg.quantizeStep = 0.001f;

    DynamicResolutionController down;
    down.configure(cfg);
    down.reset(0.75f);
    for (int i = 0; i < 30; ++i)
        down.update(makeSample(20.0f), DT);

    DynamicResolutionController up;
    up.configure(cfg);
    up.reset(0.75f);
    for (int i = 0; i < 30; ++i)
        up.update(makeSample(5.0f), DT);

    const float dropTravel = 0.75f - down.getScale();
    const float riseTravel = up.getScale() - 0.75f;
    EXPECT_GT(dropTravel, riseTravel * 2.0f)
        << "drop " << dropTravel << " vs rise " << riseTravel;
}

// ── Limits ─────────────────────────────────────────────────────────────────

TEST(DynamicResolutionController, RateLimitBoundsPerSecondTravel)
{
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 0.0f;
    cfg.quantizeStep = 0.001f;
    cfg.maxDropPerSecond = 0.5f;
    cfg.maxRisePerSecond = 0.1f;
    cfg.emergencyFraction = 1000.0f; // keep the dwell path out of this test

    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    // The contract is travel per SECOND, not per frame: the slew budget accrues
    // between accepted changes so that a ladder rung wider than one frame's
    // budget is still reachable. One second of catastrophic over-budget must
    // therefore stay within one second of budget (plus at most the rung the
    // last accepted change landed on).
    const float dropStart = c.getScale();
    for (int i = 0; i < 60; ++i)
        c.update(makeSample(200.0f), DT);
    const float dropped = dropStart - c.getScale();
    EXPECT_GT(dropped, 0.0f) << "a catastrophic frame must actually move the scale";
    EXPECT_LE(dropped, cfg.maxDropPerSecond * 1.0f + cfg.quantizeStep + 1e-4f)
        << "one second of drop exceeded one second of slew budget";

    c.reset(0.5f);
    const float riseStart = c.getScale();
    for (int i = 0; i < 60; ++i)
        c.update(makeSample(0.5f), DT);
    const float risen = c.getScale() - riseStart;
    EXPECT_GT(risen, 0.0f) << "a rung wider than one frame's budget must still be reachable";
    EXPECT_LE(risen, cfg.maxRisePerSecond * 1.0f + cfg.quantizeStep + 1e-4f)
        << "one second of rise exceeded one second of slew budget";
}

TEST(DynamicResolutionController, ClampsToConfiguredRange)
{
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 0.0f;
    cfg.minScale = 0.6f;
    cfg.maxScale = 0.9f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);
    EXPECT_FLOAT_EQ(c.getScale(), 0.9f) << "Reset must clamp into range";

    for (int i = 0; i < 2000; ++i)
        c.update(makeSample(500.0f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), 0.6f);

    for (int i = 0; i < 4000; ++i)
        c.update(makeSample(0.1f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), 0.9f);
}

TEST(DynamicResolutionController, ConfigureSanitizesHostileValues)
{
    DynamicResolutionConfig cfg{};
    cfg.targetMs = std::nanf("");
    cfg.minScale = -5.0f;
    cfg.maxScale = 42.0f;
    cfg.quantizeStep = 0.0f;
    cfg.sampleSmoothing = 0.0f;
    cfg.deadbandFraction = 5.0f;

    DynamicResolutionController c;
    c.configure(cfg);
    const auto& out = c.getConfig();
    EXPECT_TRUE(std::isfinite(out.targetMs) && out.targetMs > 0.0f);
    EXPECT_GE(out.minScale, 0.5f);
    EXPECT_LE(out.maxScale, 1.0f);
    EXPECT_GE(out.maxScale, out.minScale);
    EXPECT_GT(out.quantizeStep, 0.0f);
    EXPECT_GT(out.sampleSmoothing, 0.0f);
    EXPECT_LE(out.deadbandFraction, 0.9f);
}

// ── Quantization ───────────────────────────────────────────────────────────

TEST(DynamicResolutionController, QuantizeLandsOnLadderAndKeepsMaxReachable)
{
    DynamicResolutionConfig cfg = testConfig();
    cfg.minScale = 0.5f;
    cfg.maxScale = 1.0f;
    cfg.quantizeStep = 0.03f; // deliberately not a divisor of the range
    DynamicResolutionController c;
    c.configure(cfg);

    EXPECT_FLOAT_EQ(c.quantizeScale(1.0f), 1.0f) << "the top rung must be exactly reachable";
    EXPECT_FLOAT_EQ(c.quantizeScale(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(c.quantizeScale(0.0f), 0.5f);

    // Round to nearest: never further than half a rung from the request.
    for (float s = 0.5f; s <= 1.0f; s += 0.007f)
    {
        const float q = c.quantizeScale(s);
        EXPECT_GE(q, cfg.minScale - 1e-6f);
        EXPECT_LE(q, cfg.maxScale + 1e-6f);
        EXPECT_LE(std::fabs(q - s), cfg.quantizeStep * 0.5f + 1e-4f)
            << "quantized " << s << " to " << q << " — further than half a rung";

        // Every rung is maxScale - n*step. The clamped endpoints are the two
        // deliberate exceptions: minScale need not fall on the ladder.
        if (q > cfg.minScale + 1e-6f && q < cfg.maxScale - 1e-6f)
        {
            const float rungs = (cfg.maxScale - q) / cfg.quantizeStep;
            EXPECT_LT(std::fabs(rungs - std::round(rungs)), 1e-3f)
                << "quantized " << q << " is off the ladder";
        }
    }
}

TEST(DynamicResolutionController, ScaleAlwaysSitsOnALadderRung)
{
    // Every extent the pool ever sees comes from a rung, which is what bounds
    // the number of distinct render-target sizes it has to allocate.
    DynamicResolutionConfig cfg = testConfig();
    cfg.quantizeStep = 0.05f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    SyntheticGpu model{3.0f, 14.0f};
    for (int i = 0; i < 400; ++i)
    {
        model.scalableAtFull = 6.0f + 14.0f * static_cast<float>((i / 40) % 3);
        c.update(makeSample(model.cost(c.getScale())), DT);
        if (c.getScale() <= cfg.minScale + 1e-6f)
            continue; // the clamped floor is a deliberate off-ladder endpoint
        const float rungs = (cfg.maxScale - c.getScale()) / cfg.quantizeStep;
        EXPECT_LT(std::fabs(rungs - std::round(rungs)), 1e-3f) << "off-ladder scale " << c.getScale();
    }
}

// ── Dwell ──────────────────────────────────────────────────────────────────

TEST(DynamicResolutionController, DwellRateLimitsAcceptedChanges)
{
    // Each accepted change reallocates ~20 render-extent pool textures, so the
    // dwell timer is a hard budget on how often that can happen.
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 0.5f;
    cfg.emergencyFraction = 1000.0f;
    cfg.quantizeStep = 0.01f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    std::vector<float> trace;
    for (int i = 0; i < 600; ++i) // 10 seconds
    {
        c.update(makeSample(14.0f), DT);
        trace.push_back(c.getScale());
    }
    const int changes = countChanges(trace, 0);
    EXPECT_LE(changes, 21) << "10 s at a 0.5 s dwell allows at most ~20 changes, saw " << changes;
    EXPECT_GT(changes, 0);
}

TEST(DynamicResolutionController, EmergencyDropBypassesDwellButNotRateLimit)
{
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 5.0f; // effectively blocks ordinary changes
    cfg.emergencyFraction = 1.5f;
    cfg.quantizeStep = 0.01f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    // Mild over-budget: dwell holds after the first accepted change.
    const float afterFirst = driveUntilFirstChange(c, 12.0f);
    ASSERT_LT(afterFirst, 1.0f);
    for (int i = 0; i < 120; ++i) // 2 s, well inside the 5 s dwell
        c.update(makeSample(12.0f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), afterFirst) << "non-emergency changes must respect dwell";

    // A spike past 1.5x target must get through immediately.
    const float spikeStart = c.getScale();
    for (int i = 0; i < 30; ++i)
        c.update(makeSample(40.0f), DT);
    EXPECT_LT(c.getScale(), afterFirst) << "an emergency spike must move the scale despite dwell";
    EXPECT_LE(spikeStart - c.getScale(),
              cfg.maxDropPerSecond * (30.0f * DT) + cfg.quantizeStep + 1e-4f)
        << "emergency must bypass dwell, never the slew limit";


    // Establish a new accepted position with an unspent, narrower slew budget.
    DynamicResolutionConfig limitedCfg = cfg;
    limitedCfg.maxDropPerSecond = 0.1f;
    DynamicResolutionController limited;
    limited.configure(limitedCfg);
    limited.reset(limitedCfg.maxScale);
    const float limitedStart = driveUntilFirstChange(limited, 12.0f);
    ASSERT_LT(limitedStart, limitedCfg.maxScale);
    const float emergencyAllowance = limitedCfg.maxDropPerSecond * (30.0f * DT)
        + limitedCfg.quantizeStep + 1e-4f;
    ASSERT_GT(limitedStart - limitedCfg.minScale, emergencyAllowance);
    for (int i = 0; i < 30; ++i)
        limited.update(makeSample(40.0f), DT);
    EXPECT_LT(limited.getScale(), limitedStart);
    EXPECT_LE(limitedStart - limited.getScale(), emergencyAllowance)
        << "an emergency must preserve the narrower per-second slew limit";
}

TEST(DynamicResolutionController, EmergencyNeverBypassesDwellUpward)
{
    // A premature rise is what turns a settled loop into an oscillating one.
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 1.0f;
    cfg.quantizeStep = 0.01f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(0.7f);

    const float afterFirst = driveUntilFirstChange(c, 1.0f); // arms the dwell
    ASSERT_GT(afterFirst, 0.7f);
    for (int i = 0; i < 50; ++i) // 0.83 s, inside the 1 s dwell
        c.update(makeSample(1.0f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), afterFirst) << "rises must always wait out the dwell";
}

// ── Closed-loop stability (the reason the controller is shaped this way) ────

TEST(DynamicResolutionController, ConvergesAndSettlesUnderSyntheticCostModel)
{
    const SyntheticGpu model{4.0f, 24.0f}; // 28 ms at 1.0, 10 ms target
    DynamicResolutionConfig cfg = testConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    const std::vector<float> trace = runClosedLoop(c, model, 900);

    const float settled = model.cost(c.getScale());
    EXPECT_LE(std::fabs(settled - cfg.targetMs), cfg.targetMs * cfg.deadbandFraction * 1.6f)
        << "settled at " << settled << " ms (scale " << c.getScale() << ")";

    // No limit cycle: once converged, the ladder must stop moving.
    EXPECT_LE(countChanges(trace, 600), 2) << "controller is hunting instead of settling";
}

TEST(DynamicResolutionController, ConvergesFromBelowWithoutOvershooting)
{
    const SyntheticGpu model{2.0f, 5.0f}; // only 7 ms at 1.0 — plenty of headroom
    DynamicResolutionConfig cfg = testConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(0.5f);

    const std::vector<float> trace = runClosedLoop(c, model, 1800);
    EXPECT_FLOAT_EQ(c.getScale(), cfg.maxScale) << "cheap scene must return to full resolution";

    // Monotone climb: the rise path must never overshoot and bounce back down.
    for (size_t i = 1; i < trace.size(); ++i)
        ASSERT_GE(trace[i], trace[i - 1] - 1e-6f)
            << "rise path reversed at frame " << i << " (" << trace[i - 1] << " -> " << trace[i]
            << ")";
}

TEST(DynamicResolutionController, ResistsOscillationUnderNoisyMeasurements)
{
    // The measurement is a real GPU timing with real jitter. Smoothing plus the
    // deadband must absorb it rather than translating it into ladder churn.
    const SyntheticGpu model{4.0f, 24.0f};
    DynamicResolutionConfig cfg = testConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    uint32_t rng = 0x1234567u;
    auto noise = [&rng]() -> float
    {
        rng = rng * 1664525u + 1013904223u;
        const float u = static_cast<float>((rng >> 8) & 0xFFFFu) / 65535.0f; // [0,1]
        return 0.90f + 0.20f * u;                                            // +/-10%
    };

    std::vector<float> trace;
    for (int i = 0; i < 1800; ++i)
    {
        c.update(makeSample(model.cost(c.getScale()) * noise()), DT);
        trace.push_back(c.getScale());
    }

    const int lateChanges = countChanges(trace, 1200);
    EXPECT_LE(lateChanges, 6) << "+/-10% measurement noise produced " << lateChanges
                              << " late ladder changes — the loop is chattering";

    const float settled = model.cost(c.getScale());
    EXPECT_LE(std::fabs(settled - cfg.targetMs), cfg.targetMs * 0.30f)
        << "noisy loop settled at " << settled << " ms";
}

TEST(DynamicResolutionController, UnscalableHintConvergesFasterThanBlindLoop)
{
    // Without the hint the ratio law under-corrects by the unscalable residual
    // and has to walk in; with it, the first correction is nearly exact. Both
    // must converge — the hint is an accuracy improvement, not a correctness
    // requirement.
    // 20 ms at 1.0 with 4 ms fixed. Solving 4 + 16s^2 = 10 puts the answer at
    // s = 0.61 — comfortably inside [0.5, 1.0], so the target is reachable and
    // the two paths differ only in how fast they get there.
    const SyntheticGpu model{4.0f, 16.0f};
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 0.0f;
    cfg.quantizeStep = 0.005f;

    auto framesToConverge = [&](bool hint) -> int
    {
        DynamicResolutionController c;
        c.configure(cfg);
        c.reset(1.0f);
        for (int i = 0; i < 3000; ++i)
        {
            const float cost = model.cost(c.getScale());
            c.update(makeSample(cost, hint ? model.unscalable : 0.0f), DT);
            if (std::fabs(model.cost(c.getScale()) - cfg.targetMs) <=
                cfg.targetMs * cfg.deadbandFraction)
                return i;
        }
        return -1;
    };

    const int blind = framesToConverge(false);
    const int hinted = framesToConverge(true);
    ASSERT_GT(blind, 0);
    ASSERT_GT(hinted, 0);
    EXPECT_LE(hinted, blind) << "hinted " << hinted << " vs blind " << blind;
}

TEST(DynamicResolutionController, UnreachableTargetSaturatesAndFlags)
{
    // The unscalable cost alone busts the budget: no scale can hit the target.
    // The controller must park at the floor and say so, not thrash forever.
    const SyntheticGpu model{18.0f, 20.0f};
    DynamicResolutionConfig cfg = responseConfig(); // 10 ms target
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    const std::vector<float> trace = runClosedLoop(c, model, 900, /*hint=*/true);
    EXPECT_FLOAT_EQ(c.getScale(), cfg.minScale);
    EXPECT_TRUE(c.getStats().saturatedLow)
        << "an unreachable target must surface, not sit as an invisible floor";
    EXPECT_EQ(countChanges(trace, 600), 0) << "saturated loop must be quiet";
}

TEST(DynamicResolutionController, TracksAStepChangeInSceneCost)
{
    // Walking from a cheap vista into an expensive town and back: the classic
    // DRS workload. Both transitions must settle, and the expensive leg must
    // sit at a lower scale than the cheap one.
    DynamicResolutionConfig cfg = testConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(1.0f);

    SyntheticGpu cheap{3.0f, 6.0f};  // 9 ms at 1.0 — under a 10 ms target
    SyntheticGpu heavy{3.0f, 24.0f}; // 27 ms at 1.0; target lands at s = 0.54

    runClosedLoop(c, cheap, 600);
    const float cheapScale = c.getScale();
    EXPECT_FLOAT_EQ(cheapScale, cfg.maxScale);

    runClosedLoop(c, heavy, 900);
    const float heavyScale = c.getScale();
    EXPECT_LT(heavyScale, cheapScale);
    EXPECT_LE(std::fabs(heavy.cost(heavyScale) - cfg.targetMs),
              cfg.targetMs * cfg.deadbandFraction * 1.6f);

    const std::vector<float> back = runClosedLoop(c, cheap, 1800);
    EXPECT_FLOAT_EQ(c.getScale(), cfg.maxScale) << "must recover full resolution when the load lifts";
    EXPECT_LE(countChanges(back, 1500), 2);
}

// ── Measured response / geometry-bound content ─────────────────────────────

// The real-world case on this project: the town scene is geometry-bound, so
// DepthPrepass + RenderEntities dominate and barely respond to render scale —
// 0.76 measured as no useful win. A whole-frame controller with no response
// model grinds to minScale, halves the resolution, saves nothing, and stays
// there. It must instead measure that the lever is dead and hand the pixels
// back. Note that classifying passes as "pre-TAA therefore scalable" would not
// catch this: those passes ARE the scalable set, they simply do not respond.
TEST(DynamicResolutionController, GeometryBoundContentReturnsToNativeInsteadOfGrindingDown)
{
    const SyntheticGpu geometryBound{19.0f, 1.0f}; // 20 ms at 1.0, 19.25 ms at 0.5
    DynamicResolutionConfig cfg = responseConfig();    // 10 ms target — unreachable
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(cfg.maxScale);

    const std::vector<float> trace = runClosedLoop(c, geometryBound, 3000);

    EXPECT_TRUE(c.getStats().scalingIneffective)
        << "the controller must notice the lever does nothing here";
    EXPECT_FLOAT_EQ(c.getScale(), cfg.maxScale)
        << "settled at " << c.getScale() << " — resolution spent for no measurable saving";
    EXPECT_FALSE(c.getStats().saturatedLow)
        << "'stuck at the floor' is the wrong diagnosis for dead-lever content";

    // It must probe — that is how the response gets measured — but not live down there.
    const float lowest = *std::min_element(trace.begin(), trace.end());
    EXPECT_LT(lowest, cfg.maxScale) << "never probed, so the verdict was not measured";
    EXPECT_EQ(countChanges(trace, 2400), 0) << "must be quiet once it has given up";
}

// The contrast case: an expensive, unreachable target where the lever DOES
// work. Dropping is still right there — it cannot reach target but it buys real
// milliseconds — so the ineffective verdict must not swallow it.
TEST(DynamicResolutionController, FillBoundContentStillDropsWhenTargetUnreachable)
{
    const SyntheticGpu fillBound{18.0f, 20.0f}; // 38 ms at 1.0, 23 ms at 0.5
    DynamicResolutionConfig cfg = responseConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(cfg.maxScale);

    for (int i = 0; i < 1200; ++i){
        c.update(makeSample(fillBound.cost(c.getScale())), DT);
        EXPECT_FALSE(c.getStats().scalingIneffective)
            << "useful fill-bound content must not be misclassified (frame " << i << ")";
    }
    EXPECT_FALSE(c.getStats().scalingIneffective)
        << "a 15 ms full-range saving is not 'ineffective'";
    EXPECT_FLOAT_EQ(c.getScale(), cfg.minScale);
    EXPECT_TRUE(c.getStats().saturatedLow);
}

TEST(DynamicResolutionController, ResponseFitRecoversTheUnscalableComponent)
{
    const SyntheticGpu model{8.0f, 16.0f}; // U = 8 ms
    DynamicResolutionConfig cfg = responseConfig();
    cfg.targetMs = 14.0f; // reachable at s = 0.61
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(cfg.maxScale);

    runClosedLoop(c, model, 1500);
    ASSERT_TRUE(c.getStats().responseFitValid);
    EXPECT_NEAR(c.getStats().estimatedUnscalableMs, model.unscalable, 1.5f)
        << "fitted U = " << c.getStats().estimatedUnscalableMs;
    EXPECT_NEAR(c.getStats().estimatedFullRangeSavingMs,
                model.scalableAtFull * (1.0f - 0.25f), 3.0f);
}

TEST(DynamicResolutionController, IneffectiveVerdictIsRevisitedWhenContentChanges)
{
    // Give up on a geometry-bound frame, then walk into fill-bound content: the
    // latch must clear so DRS starts working again. Without the latch it would
    // re-probe forever; without the clear it would never work again.
    DynamicResolutionConfig cfg = responseConfig();
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(cfg.maxScale);

    const SyntheticGpu geometryBound{19.0f, 1.0f};
    runClosedLoop(c, geometryBound, 3000);
    ASSERT_TRUE(c.getStats().scalingIneffective);
    ASSERT_FLOAT_EQ(c.getScale(), cfg.maxScale);

    const SyntheticGpu fillBound{3.0f, 24.0f}; // 27 ms at 1.0; target lands at 0.54
    runClosedLoop(c, fillBound, 3000);
    EXPECT_FALSE(c.getStats().scalingIneffective)
        << "the verdict must not outlive the content it was measured on";
    EXPECT_LT(c.getScale(), cfg.maxScale);
    EXPECT_LE(std::fabs(fillBound.cost(c.getScale()) - cfg.targetMs),
              cfg.targetMs * cfg.deadbandFraction * 1.6f);
}

TEST(DynamicResolutionController, ResetClearsAdaptationState)
{
    DynamicResolutionController c;
    c.configure(testConfig());
    c.reset(1.0f);
    for (int i = 0; i < 300; ++i)
        c.update(makeSample(40.0f), DT);
    EXPECT_LT(c.getScale(), 1.0f);
    EXPECT_GT(c.getStats().acceptedDrops, 0u);

    c.reset(0.85f);
    EXPECT_FLOAT_EQ(c.getScale(), 0.85f);
    EXPECT_EQ(c.getStats().acceptedDrops, 0u);
    EXPECT_EQ(c.getStats().acceptedRises, 0u);
    EXPECT_FLOAT_EQ(c.getStats().smoothedMs, 0.0f);

    // The smoothed cost must seed from the next sample, not blend against a
    // stale pre-reset value.
    c.update(makeSample(12.0f), DT);
    EXPECT_FLOAT_EQ(c.getStats().smoothedMs, 12.0f);
}

TEST(DynamicResolutionController, ZeroDeltaTimeCannotMoveTheScale)
{
    // A paused/stepped frame has dt == 0; the slew limit must collapse to zero
    // travel rather than dividing by it.
    DynamicResolutionConfig cfg = testConfig();
    cfg.dwellSeconds = 0.0f;
    DynamicResolutionController c;
    c.configure(cfg);
    c.reset(0.8f);
    for (int i = 0; i < 100; ++i)
        EXPECT_FLOAT_EQ(c.update(makeSample(90.0f), 0.0f), 0.8f);
}


// ── Frame law (S13 R9): frame periods and CPU busy time, no GPU timing ──────

namespace{
    // a 60 Hz display: a frame takes whole refresh intervals (vsync)
    constexpr float REFRESH_MS = 1000.0f / 60.0f;

    float vsyncPeriod(float costMs){
        return REFRESH_MS * std::max(1.0f, std::ceil(costMs / REFRESH_MS - 1e-4f));
    }

    DynamicResolutionSample periodSample(float periodMs, float cpuBusyMs){
        DynamicResolutionSample s{};
        s.framePeriodMs = periodMs;
        s.cpuBusyMs = cpuBusyMs;
        s.valid = true;
        return s;
    }

    DynamicResolutionConfig periodConfig(){
        DynamicResolutionConfig c{};
        c.targetMs = REFRESH_MS;
        return c;
    }

    // a deterministic noise factor in 1 +- amplitude
    struct Lcg{
        uint32_t state = 0x2468ACEu;
        float next(float amplitude){
            state = state * 1664525u + 1013904223u;
            const float u = static_cast<float>((state >> 8) & 0xFFFFu) / 65535.0f;
            return 1.0f + amplitude * (2.0f * u - 1.0f);
        }
    };
}

TEST(DynamicResolutionController, PeriodLawNoiseHoldsSteady){
    // AC8 (a): frame periods within 8 % of the target (no vsync quantisation: a variable
    // refresh display) are no reason to move
    const SyntheticGpu model{2.0f, REFRESH_MS - 2.0f};
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(1.0f);
    Lcg noise;
    std::vector<float> trace;
    for (int i = 0; i < 600; i++){
        c.update(periodSample(model.cost(c.getScale()) * noise.next(0.08f), 4.0f), DT);
        trace.push_back(c.getScale());
    }
    EXPECT_LE(countChanges(trace, 0), 2);
    EXPECT_LE(c.getStats().acceptedChanges, 2u);
    EXPECT_EQ(c.getStats().source, DynamicResolutionSource::FRAME);
}

TEST(DynamicResolutionController, PeriodLawVsyncNoiseHoldsSteady){
    // the same under vsync: a cost with 10 % headroom and 8 % noise always makes the interval
    const SyntheticGpu model{2.0f, REFRESH_MS * 0.9f - 2.0f};
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(1.0f);
    Lcg noise;
    std::vector<float> trace;
    for (int i = 0; i < 600; i++){
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale()) * noise.next(0.08f)), 4.0f), DT);
        trace.push_back(c.getScale());
    }
    EXPECT_EQ(countChanges(trace, 0), 0);
    EXPECT_FLOAT_EQ(c.getScale(), 1.0f);
}

TEST(DynamicResolutionController, PeriodLawLoadStepDropsFast){
    // AC8 (b): the load doubles (the frame rate halves under vsync): the scale drops within 0.5 s
    SyntheticGpu model{2.0f, 10.0f}; // 12 ms at 1: fits
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(1.0f);
    for (int i = 0; i < 120; i++)
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
    ASSERT_FLOAT_EQ(c.getScale(), 1.0f);

    model = SyntheticGpu{4.0f, 20.0f}; // 24 ms at 1
    int dropFrame = -1;
    for (int i = 0; i < 120 && dropFrame < 0; i++){
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
        if (c.getScale() < 1.0f)
            dropFrame = i;
    }
    ASSERT_GE(dropFrame, 0) << "a doubled load never dropped";
    EXPECT_LE((dropFrame + 1) * DT, 0.5f) << "dropped after " << dropFrame + 1 << " frames";
    EXPECT_GT(c.getStats().acceptedDrops, 0u);

    // and it settles where the frame fits again
    for (int i = 0; i < 600; i++)
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
    EXPECT_LE(model.cost(c.getScale()), REFRESH_MS * 1.2f) << "scale " << c.getScale();
}

TEST(DynamicResolutionController, PeriodLawCpuBoundNeverDrops){
    // AC8 (c): every frame misses, but the CPU is busy for most of the target: a lower
    // resolution would only cost quality
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(1.0f);
    for (int i = 0; i < 600; i++)
        c.update(periodSample(2.0f * REFRESH_MS, 0.8f * REFRESH_MS + 0.5f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), 1.0f);
    EXPECT_EQ(c.getStats().acceptedChanges, 0u);
    EXPECT_TRUE(c.getStats().cpuBound);
}

TEST(DynamicResolutionController, PeriodLawProbesUpWithHeadroom){
    // headroom cannot be seen under vsync: one rung up after probeSeconds without a miss
    const SyntheticGpu model{1.0f, 4.0f};
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(0.6f);
    for (int i = 0; i < 100; i++) // 1.67 s
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 3.0f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), 0.6f) << "probed before probeSeconds";
    EXPECT_GT(c.getStats().probeRemainingSeconds, 0.0f);
    for (int i = 0; i < 40; i++) // 2.33 s
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 3.0f), DT);
    EXPECT_NEAR(c.getScale(), 0.65f, 1e-4f) << "one rung after probeSeconds";
    for (int i = 0; i < 60 * 30; i++)
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 3.0f), DT);
    EXPECT_FLOAT_EQ(c.getScale(), 1.0f) << "a cheap scene climbs back to the top";
    EXPECT_FLOAT_EQ(c.getStats().probeRemainingSeconds, 0.0f);
}

TEST(DynamicResolutionController, PeriodLawFailedRiseDoublesProbe){
    // AC8 (d): the frame fits at 0.8 and misses at 0.85: every probe fails, returns to 0.8 and
    // doubles the interval, up to maxProbeSeconds
    const SyntheticGpu model{4.0f, 18.5f}; // 15.8 ms at 0.8, 17.4 ms at 0.85
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(0.8f);
    std::vector<float> intervals;
    uint32_t failed = 0;
    for (int i = 0; i < 60 * 60 && intervals.size() < 5; i++){
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
        if (c.getStats().failedRises != failed){
            failed = c.getStats().failedRises;
            intervals.push_back(c.getStats().probeIntervalSeconds);
        }
        EXPECT_GE(c.getScale(), 0.8f - 1e-4f) << "a failed probe returns to the rung it left, frame " << i;
    }
    ASSERT_GE(intervals.size(), 4u);
    EXPECT_FLOAT_EQ(intervals[0], 4.0f);
    EXPECT_FLOAT_EQ(intervals[1], 8.0f);
    EXPECT_FLOAT_EQ(intervals[2], 16.0f);
    EXPECT_FLOAT_EQ(intervals[3], 16.0f) << "capped at maxProbeSeconds";
    // a failed probe returns at once: after the last failure it sits at 0.8 again
    for (int i = 0; i < 60; i++)
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
    EXPECT_NEAR(c.getScale(), 0.8f, 1e-4f);
}

TEST(DynamicResolutionController, PeriodLawCostChangeResetsProbe){
    // the backoff learned on one content is forgotten when the content's cost changes
    const SyntheticGpu model{4.0f, 18.5f};
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(0.8f);
    for (int i = 0; i < 60 * 20 && c.getStats().failedRises < 2; i++)
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
    ASSERT_GE(c.getStats().failedRises, 2u);
    // settle at the held rung, then the CPU side of the content changes by far more than 25 %
    for (int i = 0; i < 90; i++)
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 4.0f), DT);
    ASSERT_GT(c.getStats().probeIntervalSeconds, 2.0f);
    // (the reset interval may be spent at once: the frames since the last miss already exceed it,
    // so a probe follows, fails again on this content and doubles from 2 to 4, not from 8 to 16)
    float shortest = c.getStats().probeIntervalSeconds;
    for (int i = 0; i < 60; i++){
        c.update(periodSample(vsyncPeriod(model.cost(c.getScale())), 9.0f), DT);
        shortest = std::min(shortest, c.getStats().probeIntervalSeconds);
    }
    EXPECT_FLOAT_EQ(shortest, 2.0f);
    EXPECT_LE(c.getStats().probeIntervalSeconds, 4.0f);
}

TEST(DynamicResolutionController, GpuTimeSelectsTheGpuLaw){
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(1.0f);
    DynamicResolutionSample s = periodSample(REFRESH_MS, 3.0f);
    s.gpuMs = 8.0f;
    c.update(s, DT);
    EXPECT_EQ(c.getStats().source, DynamicResolutionSource::GPU);
    EXPECT_FLOAT_EQ(c.getStats().smoothedMs, 8.0f);
    EXPECT_STREQ(dynamicResolutionSourceName(DynamicResolutionSource::GPU), "gpu");
    EXPECT_STREQ(dynamicResolutionSourceName(DynamicResolutionSource::FRAME), "frame");
}

TEST(DynamicResolutionController, FrameIndexTicksOncePerFrame){
    // several views ticking it in one app frame do not run it at N x speed
    DynamicResolutionController c;
    c.configure(periodConfig());
    c.reset(1.0f);
    for (uint64_t frame = 1; frame <= 10; frame++){
        for (int view = 0; view < 3; view++)
            c.update(periodSample(REFRESH_MS, 3.0f), DT, frame);
    }
    EXPECT_EQ(c.getStats().ticks, 10u);
}

TEST(DynamicResolutionControllerBench, Tick){
    // AC22: a tick costs at most 0.01 ms (the frame law's window and the GPU law's fit)
    const SyntheticGpu model{4.0f, 18.5f};
    DynamicResolutionController frameLaw;
    frameLaw.configure(periodConfig());
    frameLaw.reset(0.8f);
    DynamicResolutionController gpuLaw;
    gpuLaw.configure(periodConfig());
    gpuLaw.reset(1.0f);
    const int ITERATIONS = 100000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; i++){
        frameLaw.update(periodSample(vsyncPeriod(model.cost(frameLaw.getScale())), 4.0f), DT);
        DynamicResolutionSample s{};
        s.gpuMs = model.cost(gpuLaw.getScale()) * (1.0f + 0.05f * (float)((i * 7) % 11 - 5) / 5.0f);
        s.valid = true;
        gpuLaw.update(s, DT);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const double perTick = ms / (2.0 * ITERATIONS);
    // the budget is for optimised builds: a debug build gets 4x; under a sanitizer (perfSlack 0)
    // it is only printed
#if defined(NDEBUG)
    const double budget = 0.01 * testsupport::perfSlack();
#else
    const double budget = 0.04 * testsupport::perfSlack();
#endif
    std::printf("[bench] DynamicResolutionController tick: %.5f ms (budget %.3f ms)\n", perTick, budget);
    if (budget > 0.0) {
        EXPECT_LE(perTick, budget);
    }
    EXPECT_GT(frameLaw.getStats().ticks + gpuLaw.getStats().ticks, 0u);
}
