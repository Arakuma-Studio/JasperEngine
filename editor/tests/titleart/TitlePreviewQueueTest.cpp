// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/TitlePreviewQueue.h"

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a job of `steps` steps predicted at `costUs` each that advance the fake clock by `actualUs`
    // (the prediction unless given) and log themselves
    struct FakeJob : TitlePreviewJob {
        double* clock;
        std::vector<std::string>* log;
        std::string name;
        int steps;
        uint32_t costUs;
        uint32_t actualUs;
        FakeJob(double* c, std::vector<std::string>* l, std::string n, int s, uint32_t cost, uint32_t actual = 0)
            : clock(c), log(l), name(std::move(n)), steps(s), costUs(cost), actualUs(actual ? actual : cost) {}
        uint32_t nextStepCostUs() const override { return steps > 0 ? costUs : 0; }
        bool step(uint32_t budgetUs) override {
            // whole steps while the time spent and the next prediction fit, at least one
            // (TitleRenderJob's rule)
            uint32_t spent = 0;
            do {
                *clock += actualUs / 1000.0;
                spent += actualUs;
                steps--;
                if (log) log->push_back(name);
            } while (steps > 0 && spent + costUs <= budgetUs);
            return steps == 0;
        }
    };

    class TitlePreviewQueueTest : public ::testing::Test {
    protected:
        double now = 0;
        uint64_t context = 1;
        std::vector<std::string> log;
        TitlePreviewQueue queue{[this] { return now; }, [this] { return context; }};

        uint64_t submit(TitlePreviewPriority priority, const std::string& name, int steps, uint32_t costUs, TitlePreviewOwner owner = 0,
                        bool visible = true, uint32_t actualUs = 0) {
            return queue.request(priority, name, [this, name, steps, costUs, actualUs] {
                return std::make_unique<FakeJob>(&now, &log, name, steps, costUs, actualUs);
            }, owner, visible);
        }

        double frame(double budgetMs = 4.0) {
            const double start = now;
            queue.runFrame(budgetMs);
            return now - start;
        }
    };

}

TEST_F(TitlePreviewQueueTest, ThreeSubmittersStayWithinTheBudget) {
    // S82's canvas thumbnails, S84's tiles and S80's project thumbnails at once
    for (int i = 0; i < 5; i++) submit(TitlePreviewPriority::Layers, "layer" + std::to_string(i), 6, 700, 1);
    for (int i = 0; i < 5; i++) submit(TitlePreviewPriority::Tiles, "tile" + std::to_string(i), 4, 1300, 2);
    for (int i = 0; i < 5; i++) submit(TitlePreviewPriority::Thumbnails, "thumb" + std::to_string(i), 3, 900, 3);
    int frames = 0;
    while (queue.stats().queued > 0 && frames < 1000) {
        EXPECT_LE(frame(), 4.0 + 1e-9) << "frame " << frames;
        frames++;
    }
    EXPECT_EQ(queue.stats().done, 15u);
    EXPECT_LE(queue.stats().maxFrameMs, 4.0 + 1e-9);
    EXPECT_EQ(queue.stats().aloneFrames, 0u);
}

TEST_F(TitlePreviewQueueTest, PriorityThenVisibilityThenAge) {
    submit(TitlePreviewPriority::Prefetch, "prefetch", 1, 100);
    submit(TitlePreviewPriority::Thumbnails, "hidden", 1, 100, 0, false);
    submit(TitlePreviewPriority::Thumbnails, "visible", 1, 100);
    submit(TitlePreviewPriority::Hover, "hover", 1, 100);
    submit(TitlePreviewPriority::Thumbnails, "visible2", 1, 100);
    frame();
    EXPECT_EQ(log, (std::vector<std::string>{"hover", "visible", "visible2", "hidden", "prefetch"}));
}

TEST_F(TitlePreviewQueueTest, CancelAllEmptiesThatOwner) {
    const uint64_t kept = submit(TitlePreviewPriority::Tiles, "kept", 2, 100, 7);
    submit(TitlePreviewPriority::Tiles, "a", 2, 100, 9);
    submit(TitlePreviewPriority::Tiles, "b", 2, 100, 9);
    queue.cancelAll(9);
    EXPECT_EQ(queue.stats().queued, 1u);
    frame();
    EXPECT_EQ(log, (std::vector<std::string>{"kept", "kept"}));
    EXPECT_EQ(queue.state(kept), TitlePreviewState::Done);
    ASSERT_TRUE(queue.result(kept));
    EXPECT_EQ(static_cast<FakeJob*>(queue.result(kept))->name, "kept");
    queue.cancel(kept);
    EXPECT_EQ(queue.state(kept), TitlePreviewState::Unknown);
}

TEST_F(TitlePreviewQueueTest, AnOversizedStepRunsAlone) {
    submit(TitlePreviewPriority::Thumbnails, "small", 3, 500);
    submit(TitlePreviewPriority::Hover, "big", 1, 10000);
    EXPECT_DOUBLE_EQ(frame(), 10.0) << "the big step runs alone in its frame";
    EXPECT_EQ(log, (std::vector<std::string>{"big"}));
    EXPECT_EQ(queue.stats().aloneFrames, 1u);
    // then the queue rests ceil(10 / 4) - 1 = 2 frames
    EXPECT_DOUBLE_EQ(frame(), 0.0);
    EXPECT_DOUBLE_EQ(frame(), 0.0);
    EXPECT_GT(frame(), 0.0);
    EXPECT_EQ(log.back(), "small");
}

TEST_F(TitlePreviewQueueTest, SameKeySharesOneJob) {
    const uint64_t a = submit(TitlePreviewPriority::Thumbnails, "same", 1, 100, 1);
    const uint64_t b = submit(TitlePreviewPriority::Hover, "same", 1, 100, 2);
    frame();
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(queue.result(a), queue.result(b));
    queue.cancelAll(1);
    EXPECT_EQ(queue.state(b), TitlePreviewState::Done) << "the other request keeps it";
}

TEST_F(TitlePreviewQueueTest, DeviceLossRequestsAgain) {
    const uint64_t id = submit(TitlePreviewPriority::Thumbnails, "thumb", 1, 100);
    frame();
    EXPECT_EQ(queue.state(id), TitlePreviewState::Done);
    context = 2;    // a new GPU context: every job is made again from its factory
    frame();
    EXPECT_EQ(log, (std::vector<std::string>{"thumb", "thumb"}));
    EXPECT_EQ(queue.state(id), TitlePreviewState::Done);
    EXPECT_EQ(queue.stats().restarts, 1u);
}

TEST_F(TitlePreviewQueueTest, AnUnlimitedFrameBreaksTheBudget) {
    // the red control of ThreeSubmittersStayWithinTheBudget: without the budget one frame does it all
    for (int i = 0; i < 5; i++) submit(TitlePreviewPriority::Layers, "layer" + std::to_string(i), 6, 700, 1);
    EXPECT_GT(frame(1e9), 4.0);
}

TEST_F(TitlePreviewQueueTest, SideWorkCountsInTheFrame) {
    // R45: a submitter's own main-thread work (a document load) is part of the frame's budget
    for (int i = 0; i < 3; i++) submit(TitlePreviewPriority::Thumbnails, "thumb" + std::to_string(i), 10, 700);
    queue.charge(3.0);
    EXPECT_DOUBLE_EQ(queue.frameLeftMs(), 1.0);
    EXPECT_LE(frame(), 1.0 + 1e-9) << "the steps get what the side work left";
    EXPECT_LE(queue.stats().lastFrameMs, 4.0 + 1e-9);
    EXPECT_DOUBLE_EQ(queue.frameLeftMs(), 4.0) << "a frame's charges end with it";
    queue.charge(4.5);
    EXPECT_DOUBLE_EQ(frame(), 0.0) << "side work that spent the frame leaves no step";
    EXPECT_EQ(queue.stats().overBudgetFrames, 1u);
    EXPECT_DOUBLE_EQ(queue.stats().sideWorkMs, 7.5);
}

TEST_F(TitlePreviewQueueTest, AnOverrunIsCountedAndRested) {
    // a step predicted at 1 ms that takes 6: the frame stops on the clock, says it went over and
    // rests ceil(6 / 4) - 1 = 1 frame
    submit(TitlePreviewPriority::Thumbnails, "liar", 3, 1000, 0, true, 6000);
    submit(TitlePreviewPriority::Thumbnails, "honest", 3, 500);
    EXPECT_DOUBLE_EQ(frame(), 6.0);
    EXPECT_EQ(log, (std::vector<std::string>{"liar"})) << "nothing starts once the frame's time is spent";
    EXPECT_EQ(queue.stats().overBudgetFrames, 1u);
    EXPECT_DOUBLE_EQ(queue.stats().worstStepMs, 6.0);
    EXPECT_EQ(queue.stats().worstStepLabel, "liar") << "a job that names no step: its key";
    ASSERT_EQ(queue.stats().overruns.size(), 1u);
    EXPECT_DOUBLE_EQ(queue.stats().overruns[0].ms, 6.0);
    EXPECT_EQ(queue.stats().overruns[0].longestLabel, "liar");
    EXPECT_DOUBLE_EQ(frame(), 0.0);
    EXPECT_EQ(queue.stats().restFrames, 1u);
    EXPECT_GT(frame(), 0.0);
    EXPECT_EQ(queue.stats().workingFrames, 2u);
    EXPECT_EQ(queue.windowFrames(), 2u) << "the rest frame is no working frame";
    EXPECT_DOUBLE_EQ(queue.p95FrameMs(), 6.0);
    EXPECT_DOUBLE_EQ(queue.stats().maxFrameMs, 6.0);
    queue.resetStats();
    EXPECT_EQ(queue.windowFrames(), 0u);
    EXPECT_DOUBLE_EQ(queue.p95FrameMs(), 0.0);
}
