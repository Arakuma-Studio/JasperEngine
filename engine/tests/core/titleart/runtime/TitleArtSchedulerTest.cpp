// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Realtime scheduler with fake render jobs and a fake clock (docs/specs/S81-title-art-runtime.md
// AC4: R12, R13, R14, R15, R17, R19): frame budgets, order, hidden titles, coalescing, the
// cache, retries; and the measured cost model (S80 Deviation 54) on a device slower than the
// desktop estimates say: no late starts, predictions that converge.

#include <gtest/gtest.h>

#include "device/QualityResolver.h"
#include "subsystem/TitleArtRuntime.h"
#include "titleart/render/TitleRenderJob.h"
#include "titleart/render/TitleStepCosts.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleLiveCompose.h"

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // what the fake jobs did, shared by the factory and the tests
    struct FakeLog {
        std::vector<std::string> started;       // the label of each job started
        std::vector<std::string> stepped;       // the label of each step run, in order
        std::vector<uint32_t> splitAt;          // splitOversizedSteps' maxUs
        bool failNext = false;                  // the next job fails at its first step (an allocation)
    };

    // a job of `count` steps of `costUs` each (desktop reference); its image carries the
    // spec's label in its width so a test can tell which state it shows
    class FakeJob : public TitleJob {
    public:
        FakeJob(std::shared_ptr<FakeLog> log, std::string label, int count, uint32_t costUs, bool fail)
            : log(std::move(log)), label(std::move(label)), count(count), costUs(costUs), failing(fail){}

        bool step(const StepBudget&, StepReport* report) override{
            if (failing){
                failure = "out_of_target_memory: the fake device has none";
                return false;
            }
            if (next < count){
                log->stepped.push_back(label);
                next++;
                if (report){
                    report->steps = 1;
                    report->estimatedUs = costUs;
                }
            }
            return done();
        }
        bool done() const override{ return failure.empty() && next >= count; }
        bool failed() const override{ return !failure.empty(); }
        bool waiting() const override{ return false; }
        float progress() const override{ return count ? (float)next / (float)count : 1.0f; }
        uint32_t nextStepCostUs() const override{ return next < count ? costUs : 0; }
        void splitOversizedSteps(uint32_t maxUs) override{ log->splitAt.push_back(maxUs); }
        uint32_t passesDone() const override{ return (uint32_t)next; }
        uint32_t passesTotal() const override{ return (uint32_t)count; }
        const std::string& error() const override{ return failure; }
        bool allocationFailure() const override{ return !failure.empty(); }
        bool floatTargetFailure() const override{ return false; }
        const char* pathName() const override{ return "rgba16f"; }
        TitleDisplayImage takeImage() override{
            TitleDisplayImage image;
            image.width = 1;
            image.height = 1;
            image.bytes = 4;
            image.frame = {0, 0, 1, 1};
            return image;
        }
        std::vector<ShaderKey> shaderKeys() const override{ return {}; }

    private:
        std::shared_ptr<FakeLog> log;
        std::string label;
        int count;
        uint32_t costUs;
        bool failing;
        int next = 0;
        std::string failure;
    };

    // the jobs a test's lives start: `steps` steps of `costUs`, labelled by the state's label
    TitleJobFactory fakeFactory(std::shared_ptr<FakeLog> log, int steps, uint32_t costUs){
        return [log, steps, costUs](const TitleJobSpec& spec) -> std::unique_ptr<TitleJob> {
            log->started.push_back(spec.label);
            const bool fail = log->failNext;
            log->failNext = false;
            return std::make_unique<FakeJob>(log, spec.label, steps, costUs, fail);
        };
    }

    std::unique_ptr<TitleArtLive> makeLive(){
        return std::make_unique<TitleArtLive>("assets/titles/test.titleart", TitleArtRenderMode::REALTIME,
                                              std::make_shared<const TitleArtDoc>(), 0x1234u);
    }

    TitleArtLiveState stateOf(const std::string& label, int priority = 0, bool visible = true){
        TitleArtLiveState state;
        state.label = label;
        state.priority = priority;
        state.visible = visible;
        return state;
    }

    // one frame: steps within `budgetUs`, then every live's end of frame
    TitleFrameReport frame(const std::vector<TitleArtLive*>& lives, const TitleJobFactory& factory, uint32_t budgetUs, double& clock,
                           double factor = 1.0){
        const TitleFrameReport report = TitleArtScheduler::schedule(lives, factory, budgetUs, factor);
        TitleLiveLimits limits;
        size_t cache = 0;
        clock += 1.0 / 60.0;
        for (TitleArtLive* live : lives) live->endFrame(clock, 1.0 / 60.0, limits, cache);
        return report;
    }

    class TitleArtSchedulerTest : public ::testing::Test {};

}

TEST_F(TitleArtSchedulerTest, NoFrameRunsPastTheBudgetButTheGuaranteedStep) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 10, 700);
    auto a = makeLive();
    a->setState(stateOf("a"), 1, {});
    double clock = 0.0;
    int frames = 0;
    while (a->hasWork() && frames < 100){
        const TitleFrameReport report = frame({a.get()}, factory, 2000, clock);
        // R15: whole steps while spent + estimate <= budget; the first always runs
        EXPECT_LE(report.spentUs, 2000.0) << "frame " << frames;
        EXPECT_GE(report.steps, 1u) << "a visible request makes progress every frame";
        frames++;
    }
    EXPECT_EQ(log->stepped.size(), 10u);
    EXPECT_EQ(frames, 5) << "two 0.7 ms steps a frame in 2 ms";
    EXPECT_EQ(a->front().hash, 1u);
    EXPECT_EQ(log->splitAt, std::vector<uint32_t>{2000u}) << "steps above the budget are split into tiles (R15)";

    // a step over the budget runs alone in its frame
    auto slow = std::make_shared<FakeLog>();
    auto b = makeLive();
    b->setState(stateOf("b"), 2, {});
    const TitleFrameReport over = frame({b.get()}, fakeFactory(slow, 3, 5000), 2000, clock);
    EXPECT_EQ(over.steps, 1u);
    EXPECT_EQ(over.overBudgetSteps, 1u);
}

TEST_F(TitleArtSchedulerTest, PlatformFactorScalesEstimates) {
    auto log = std::make_shared<FakeLog>();
    auto a = makeLive();
    a->setState(stateOf("a"), 1, {});
    double clock = 0.0;
    // 300 us steps on a mobile device (x 6): one a frame in 2 ms
    const TitleFrameReport report = frame({a.get()}, fakeFactory(log, 6, 300), 2000, clock, titlePlatformFactor(DevicePlatformGroup::Mobile));
    EXPECT_EQ(report.steps, 1u);
    EXPECT_DOUBLE_EQ(report.spentUs, 1800.0);
    EXPECT_EQ(titlePlatformFactor(DevicePlatformGroup::Web), 4.0);
    EXPECT_EQ(titlePlatformFactor(DevicePlatformGroup::Desktop), 1.0);
}

TEST_F(TitleArtSchedulerTest, PriorityThenVisibilityThenAge) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 1, 3000);
    auto oldLow = makeLive();
    auto newLow = makeLive();
    auto high = makeLive();
    auto hiddenHigh = makeLive();
    oldLow->setState(stateOf("oldLow", 0), 1, {});
    newLow->setState(stateOf("newLow", 0), 2, {});
    high->setState(stateOf("high", 5), 3, {});
    TitleArtLiveState hiddenState = stateOf("hiddenHigh", 5, false);
    hiddenState.renderWhenHidden = true;
    hiddenHigh->setState(hiddenState, 4, {});
    double clock = 0.0;
    std::vector<TitleArtLive*> lives = {newLow.get(), oldLow.get(), hiddenHigh.get(), high.get()};
    // each step is over the budget: one title a frame, in the scheduler's order
    for (int i = 0; i < 4; i++) frame(lives, factory, 1000, clock);
    ASSERT_EQ(log->started.size(), 4u);
    EXPECT_EQ(log->started[0], "high") << "the highest priority first";
    EXPECT_EQ(log->started[1], "hiddenHigh") << "then, at the same priority, visible before hidden";
    EXPECT_EQ(log->started[2], "oldLow") << "then the oldest request";
    EXPECT_EQ(log->started[3], "newLow");
}

TEST_F(TitleArtSchedulerTest, HiddenTitlesGetNoSteps) {
    auto log = std::make_shared<FakeLog>();
    auto counter = makeLive();
    counter->setState(stateOf("counter", 0, false), 1, {});
    double clock = 0.0;
    for (int i = 0; i < 10; i++) frame({counter.get()}, fakeFactory(log, 2, 100), 2000, clock);
    EXPECT_TRUE(log->stepped.empty()) << "R13: a hidden counter costs nothing";
    EXPECT_EQ(counter->status().pending, "hidden");
    // shown again, it renders
    counter->setState(stateOf("counter", 0, true), 1, {});
    frame({counter.get()}, fakeFactory(log, 2, 100), 2000, clock);
    EXPECT_EQ(counter->front().hash, 1u);
}

TEST_F(TitleArtSchedulerTest, NewStateUnderHalfDoneRestarts) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 10, 1000);
    auto a = makeLive();
    a->setState(stateOf("first"), 1, {});
    double clock = 0.0;
    frame({a.get()}, factory, 2000, clock);     // 2 of 10 steps: 20 %
    a->setState(stateOf("second"), 2, {});
    EXPECT_EQ(a->restarts(), 1) << "R12: under 50 % done, the render starts again with the new state";
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    EXPECT_EQ(a->front().hash, 2u);
    EXPECT_EQ(log->started, (std::vector<std::string>{"first", "second"}));
}

TEST_F(TitleArtSchedulerTest, NewStateOverHalfDoneFinishesThenRendersTheLatest) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 10, 1000);
    auto a = makeLive();
    a->setState(stateOf("first"), 1, {});
    double clock = 0.0;
    for (int i = 0; i < 3; i++) frame({a.get()}, factory, 2000, clock);   // 60 %
    // a keyed parameter that changes every frame cannot starve the render (R12)
    a->setState(stateOf("second"), 2, {});
    a->setState(stateOf("third"), 3, {});
    EXPECT_EQ(a->restarts(), 0);
    frame({a.get()}, factory, 2000, clock);
    frame({a.get()}, factory, 2000, clock);
    EXPECT_EQ(a->front().hash, 1u) << "the first render finished and shows";
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    EXPECT_EQ(a->front().hash, 3u) << "then the latest state, never the one in between";
    EXPECT_EQ(log->started, (std::vector<std::string>{"first", "third"}));
}

TEST_F(TitleArtSchedulerTest, CoalescingKeepsOneQueuedRequest) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 1, 100);
    auto a = makeLive();
    TitleArtLiveState hidden = stateOf("x", 0, false);
    for (uint64_t h = 1; h <= 20; h++) a->setState(hidden, h, {});
    double clock = 0.0;
    a->setState(stateOf("latest"), 20, {});
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    EXPECT_EQ(log->started, (std::vector<std::string>{"latest"})) << "a queued request is replaced, never stacked";
    EXPECT_EQ(a->front().hash, 20u);
}

TEST_F(TitleArtSchedulerTest, ShownBeforeStateSwapsFromTheCacheWithoutRendering) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 2, 100);
    auto a = makeLive();
    double clock = 0.0;
    a->setState(stateOf("A"), 1, {});
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    a->setState(stateOf("B"), 2, {});
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    const uint64_t stepsBefore = a->stepsRun();
    // R14: toggling back to the previous text renders nothing
    a->setState(stateOf("A"), 1, {});
    EXPECT_FALSE(a->hasWork());
    frame({a.get()}, factory, 2000, clock);
    EXPECT_EQ(a->front().hash, 1u);
    EXPECT_EQ(a->stepsRun(), stepsBefore);
    // and the state already shown is dropped at once
    a->setState(stateOf("A"), 1, {});
    EXPECT_FALSE(a->hasWork());
}

TEST_F(TitleArtSchedulerTest, AllocationFailuresRetryAfterTwoFourEightSecondsThenStop) {
    auto log = std::make_shared<FakeLog>();
    const TitleJobFactory factory = fakeFactory(log, 1, 100);
    auto a = makeLive();
    double clock = 0.0;
    a->setState(stateOf("A"), 1, {});
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    ASSERT_EQ(a->front().hash, 1u);
    // every later attempt fails: the previous image stays
    auto failing = [log](const TitleJobSpec& spec) -> std::unique_ptr<TitleJob> {
        log->started.push_back(spec.label);
        return std::make_unique<FakeJob>(log, spec.label, 1, 100, true);
    };
    a->setState(stateOf("B"), 2, {});
    std::vector<double> attempts;
    for (int i = 0; i < 60 * 20; i++){
        const size_t before = log->started.size();
        frame({a.get()}, failing, 2000, clock);
        if (log->started.size() > before) attempts.push_back(clock);
    }
    ASSERT_EQ(attempts.size(), 4u) << "the first attempt and three retries";
    EXPECT_NEAR(attempts[1] - attempts[0], 2.0, 0.1);
    EXPECT_NEAR(attempts[2] - attempts[1], 4.0, 0.1);
    EXPECT_NEAR(attempts[3] - attempts[2], 8.0, 0.1);
    EXPECT_EQ(a->front().hash, 1u) << "R19: the previous image stays";
    EXPECT_TRUE(a->status().failed);
    EXPECT_EQ(a->status().pending, "failed");
    // a new state tries again
    a->setState(stateOf("C"), 3, {});
    while (a->hasWork()) frame({a.get()}, factory, 2000, clock);
    EXPECT_EQ(a->front().hash, 3u);
    EXPECT_FALSE(a->status().failed);
}

TEST_F(TitleArtSchedulerTest, QualityTierFollowsTheDeviceTier) {
    EXPECT_STREQ(titleQualityTierFor(DeviceTier::Low), "low");
    EXPECT_STREQ(titleQualityTierFor(DeviceTier::Medium), "medium");
    EXPECT_STREQ(titleQualityTierFor(DeviceTier::High), "high");
    EXPECT_STREQ(titleQualityTierFor(DeviceTier::Ultra), "high");
}

namespace {

    // S80's TitleRenderJob as the scheduler sees it (RendererJob's part that matters here)
    class StepListJob : public TitleJob {
    public:
        explicit StepListJob(std::unique_ptr<TitleRenderJob> j): job(std::move(j)){}
        bool step(const StepBudget& budget, StepReport* report) override{ return job->step(budget, report); }
        bool done() const override{ return job->done(); }
        bool failed() const override{ return job->failed(); }
        bool waiting() const override{ return job->waiting(); }
        float progress() const override{ return job->progress(); }
        uint32_t nextStepCostUs() const override{ return job->nextStepCostUs(); }
        void splitOversizedSteps(uint32_t maxUs) override{ job->splitOversizedSteps(maxUs); }
        uint32_t passesDone() const override{ return job->passesDone(); }
        uint32_t passesTotal() const override{ return job->passesTotal(); }
        const std::string& error() const override{ return job->error(); }
        bool allocationFailure() const override{ return false; }
        bool floatTargetFailure() const override{ return false; }
        const char* pathName() const override{ return "rgba16f"; }
        TitleDisplayImage takeImage() override{
            TitleDisplayImage image;
            image.width = 1;
            image.height = 1;
            image.bytes = 4;
            return image;
        }
        std::vector<ShaderKey> shaderKeys() const override{ return {}; }
        void useCosts(TitleStepCosts* model) override{ job->useCosts(model); }

    private:
        std::unique_ptr<TitleRenderJob> job;
    };

    // a render on a device that runs each kind of step slower than its desktop estimate by the
    // kind's own factor (a software rasterizer's blurs far more than its composites); `clock`
    // is the device's time in us
    std::unique_ptr<TitleRenderJob> slowDeviceRender(std::shared_ptr<double> clock){
        struct Kind {
            const char* label;
            StepKind kind;
            int width, height;
            uint32_t cpuUs;
            double slowdown;
        };
        // the slowest kind first: kinds never measured are predicted by the machine's ratio
        const Kind kinds[] = {{"blur", StepKind::Gpu, 200, 100, 0, 40.0},
                              {"compose", StepKind::Gpu, 100, 100, 0, 8.0},
                              {"mask", StepKind::Cpu, 0, 0, 200, 3.0}};
        TitleStepList list;
        for (int i = 0; i < 40; i++){
            const Kind& k = kinds[i % 3];
            TitleRenderStep step;
            step.kind = k.kind;
            step.label = k.label;
            step.scissor = {0, 0, k.width, k.height};
            step.cpuUs = k.cpuUs;
            const uint32_t estimate = TitleRenderJob::estimateUs(step);
            const double slowdown = k.slowdown;
            step.run = [clock, estimate, slowdown](const TitleRectI&, std::string&){
                *clock += estimate * slowdown;
                return true;
            };
            list.add(std::move(step));
        }
        return std::make_unique<TitleRenderJob>(std::move(list));
    }

    struct SlowDeviceRun {
        int frames = 0;
        int lateFrames = 0;         // frames of more than one step that went over the budget (device time)
        uint32_t reportedLate = 0;  // the scheduler's own count
        double worstFrameUs = 0.0;
    };

    // six renders of a title through the scheduler at `budgetUs`, with `costs` (null: the static
    // estimates, what the scheduler did before)
    SlowDeviceRun runOnSlowDevice(std::shared_ptr<double> clock, TitleStepCosts* costs, uint32_t budgetUs){
        TitleScheduleContext context;
        context.factory = [clock](const TitleJobSpec&) -> std::unique_ptr<TitleJob> {
            return std::make_unique<StepListJob>(slowDeviceRender(clock));
        };
        context.budgetUs = budgetUs;
        context.costs = costs;
        auto live = makeLive();
        SlowDeviceRun run;
        TitleLiveLimits limits;
        double seconds = 0.0;
        for (int render = 0; render < 6; render++){
            live->setState(stateOf("render " + std::to_string(render)), 100 + render, {});
            while (live->hasWork() && run.frames < 10000){
                const double start = *clock;
                const TitleFrameReport report = TitleArtScheduler::schedule({live.get()}, context);
                const double took = *clock - start;
                if (report.steps > 1 && took > budgetUs) run.lateFrames++;
                run.reportedLate += report.lateStarts;
                run.worstFrameUs = std::max(run.worstFrameUs, report.steps > 1 ? took : 0.0);
                size_t cache = 0;
                seconds += 1.0 / 60.0;
                live->endFrame(seconds, 1.0 / 60.0, limits, cache);
                run.frames++;
            }
        }
        return run;
    }

}

TEST_F(TitleArtSchedulerTest, MeasuredCostsKeepASlowDevicesFramesWithinBudget) {
    // S80 Deviation 54 for the runtime (S81 R15): the device's own measurements, not the desktop
    // estimates, decide what fits a frame
    constexpr uint32_t budget = 2000;
    auto clock = std::make_shared<double>(0.0);
    TitleStepCosts costs([clock]{ return *clock; });
    const SlowDeviceRun measured = runOnSlowDevice(clock, &costs, budget);
    EXPECT_EQ(measured.lateFrames, 0) << "no frame of several steps went over the budget (worst " << measured.worstFrameUs << " us)";
    EXPECT_EQ(measured.reportedLate, 0u);
    EXPECT_GT(costs.observations(), 2 * TitleStepCosts::ERROR_WINDOW);
    EXPECT_LT(costs.recentError(), costs.firstError()) << "the predictions converge on the device";
    EXPECT_LT(costs.recentError(), 0.05);
    EXPECT_GT(measured.frames, 6) << "the renders were sliced";

    // the control: the same renders by the static estimates start steps late on this device
    auto clock2 = std::make_shared<double>(0.0);
    const SlowDeviceRun trusted = runOnSlowDevice(clock2, nullptr, budget);
    EXPECT_GT(trusted.lateFrames, 0) << "trusting desktop estimates overruns the budget";
}

TEST_F(TitleArtSchedulerTest, ACostModelStartsFromThePlatformFactor) {
    // a fresh model predicts the platform factor's multiple of an estimate until it measures
    TitleStepCosts mobile({}, titlePlatformFactor(DevicePlatformGroup::Mobile));
    EXPECT_EQ(mobile.predictUs("gpu:blur", 100), 600u);
    mobile.observe("gpu:blur", 100, 150.0);
    EXPECT_EQ(mobile.predictUs("gpu:blur", 100), 150u) << "the first measurement replaces the prior";
}

TEST_F(TitleArtSchedulerTest, RenderTiersFollowTheSimulatedDevice) {
    // S83's mobile tier and S86's quality tier come from S56's platform group and device tier,
    // which the editor's Game view simulation sets: a new tier is a new state (it renders again),
    // while the prebaked image's key stays the same (either tier shows it)
    QualityResolver::reset();
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Desktop);
    auto live = makeLive();
    TitleArtRuntime::setState(*live, stateOf("tiers"));
    EXPECT_EQ(live->state().fxTier, (int)FxTier::Desktop);
    const uint64_t desktop = live->stateHash();
    const uint64_t prebake = TitleArtRuntime::prebakeKeyOf(*live, stateOf("tiers"));

    QualityResolver::setPlatformGroup(DevicePlatformGroup::Mobile);
    TitleArtRuntime::setState(*live, stateOf("tiers"));
    EXPECT_EQ(live->state().fxTier, (int)FxTier::Mobile) << "a phone draws S83's mobile reductions";
    EXPECT_NE(live->stateHash(), desktop) << "the simulated device renders again";
    EXPECT_EQ(TitleArtRuntime::prebakeKeyOf(*live, stateOf("tiers")), prebake);
    EXPECT_EQ(TitleArtRuntime::stateHashOf(*live, stateOf("tiers")), live->stateHash()) << "the hash a prebaked image is given matches";

    QualityResolver::setPlatformGroup(DevicePlatformGroup::Web);
    TitleArtRuntime::setState(*live, stateOf("tiers"));
    EXPECT_EQ(live->state().fxTier, (int)FxTier::Mobile) << "the web takes the mobile tier";

    QualityResolver::setPlatformGroup(DevicePlatformGroup::Desktop);
    QualityResolver::setTier(DeviceTier::Low);
    TitleArtRuntime::setState(*live, stateOf("tiers"));
    EXPECT_EQ(live->state().fxTier, (int)FxTier::Mobile) << "a Low desktop device takes it too";
    QualityResolver::setTier(DeviceTier::High);
    TitleArtRuntime::setState(*live, stateOf("tiers"));
    EXPECT_EQ(live->state().fxTier, (int)FxTier::Desktop);
    QualityResolver::reset();
}

TEST_F(TitleArtSchedulerTest, LiveBudgetsByPlatformGroup) {
    EXPECT_DOUBLE_EQ(titleLiveBudgetMs(DevicePlatformGroup::Desktop), 1.0);
    EXPECT_DOUBLE_EQ(titleLiveBudgetMs(DevicePlatformGroup::Mobile), 0.5);
    EXPECT_DOUBLE_EQ(titleLiveBudgetMs(DevicePlatformGroup::Web), 0.5);
}
