// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Realtime scheduler with fake render jobs and a fake clock (docs/specs/S81-title-art-runtime.md
// AC4: R12, R13, R14, R15, R17, R19): frame budgets, order, hidden titles, coalescing, the
// cache, retries.

#include <gtest/gtest.h>

#include "diag/HangWatchdog.h"
#include "subsystem/TitleArtSystem.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"

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

// The Title Designer's Glyph Atlas preview stalled at "rendering 35/44": the frame guard counted
// commits, and the editor commits only when a view redraws. No commit here, only the main loop.
TEST_F(TitleArtSchedulerTest, TheRuntimeFrameAdvancesOnTheMainLoopWithoutACommit) {
    auto log = std::make_shared<FakeLog>();
    TitleArtScheduler& scheduler = TitleArtScheduler::get();
    scheduler.setJobFactoryForTesting(fakeFactory(log, 8, 100000));
    auto live = makeLive();
    live->setState(stateOf("preview"), 1, {});
    scheduler.add(live.get());
    for (int frame = 0; frame < 5; frame++){
        diag::heartbeat();
        TitleArtSystem::runRuntimeFrame(true);
    }
    EXPECT_EQ(log->stepped.size(), 5u) << "one step over the budget per main-loop frame";
    // R13: whoever asks second in a frame gets nothing
    TitleArtSystem::runRuntimeFrame(true);
    EXPECT_EQ(log->stepped.size(), 5u);
    scheduler.remove(live.get());
    scheduler.setJobFactoryForTesting(nullptr);
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
