// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU profiler (spec S15 AC2, AC3, AC12, R18, R19): scope trees and their self times, scopes
// split at frame boundaries, ring overflow without blocking, job workers as named lanes, the
// .jetrace.json round trip, monitors and the always-on frame-time ring.

#include "jobs/JobSystem.h"
#include "profiler/EngineProfiling.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profile.h"

#include "AllocationCounter.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

using namespace jasperengine;

namespace {

    // a profiler that records from a clean history, stopped again afterwards
    class Profiler : public ::testing::Test {
    protected:
        void SetUp() override{
            profiler::setEnabled(false);
            // what earlier tests left in the rings goes with this frame
            nextFrame();
            profiler::clearHistory();
            profiler::setEnabled(true);
            // the first frame after arming is partial
            nextFrame();
        }
        void TearDown() override{
            profiler::setEnabled(false);
            nextFrame();
            profiler::clearHistory();
        }
        static void nextFrame(){
            FrameTimes::beginFrame();
            profiler::frameMark();
        }
        static void spin(std::chrono::microseconds duration){
            const auto end = std::chrono::steady_clock::now() + duration;
            while (std::chrono::steady_clock::now() < end){}
        }
        static const profiler::Frame* lastFrame(){
            return profiler::frameCount() ? profiler::frameAt(profiler::frameCount() - 1) : nullptr;
        }
        static const profiler::TreeRow* row(const std::vector<profiler::TreeRow>& rows, const std::string& path){
            for (const profiler::TreeRow& candidate : rows){
                if (candidate.path == path)
                    return &candidate;
            }
            return nullptr;
        }
    };

}

TEST_F(Profiler, NestedScopesBuildTree){
    for (int frame = 0; frame < 2; frame++){
        {
            JE_PROFILE_SCOPE("TestRoot");
            for (int i = 0; i < 2; i++){
                JE_PROFILE_SCOPE("TestChild");
                spin(std::chrono::microseconds(200));
                {
                    JE_PROFILE_SCOPE("TestLeaf");
                    spin(std::chrono::microseconds(100));
                }
            }
            spin(std::chrono::microseconds(100));
        }
        nextFrame();
    }
    ASSERT_EQ(profiler::frameCount(), 3u);   // the partial frame and the two
    for (size_t f = 1; f < 3; f++){
        const profiler::Frame& frame = *profiler::frameAt(f);
        EXPECT_FALSE(frame.partial);
        const std::vector<profiler::TreeRow> rows = profiler::buildTree(frame, profiler::mainThreadId());
        const profiler::TreeRow* root = row(rows, "TestRoot");
        const profiler::TreeRow* child = row(rows, "TestRoot/TestChild");
        const profiler::TreeRow* leaf = row(rows, "TestRoot/TestChild/TestLeaf");
        ASSERT_TRUE(root && child && leaf);
        EXPECT_EQ(root->depth, 0u);
        EXPECT_EQ(child->depth, 1u);
        EXPECT_EQ(leaf->depth, 2u);
        // the children merge by name: one row, two calls
        EXPECT_EQ(root->calls, 1u);
        EXPECT_EQ(child->calls, 2u);
        EXPECT_EQ(leaf->calls, 2u);
        // self = total - the children's total (1 us)
        EXPECT_NEAR(root->selfMs, root->totalMs - child->totalMs, 0.001);
        EXPECT_NEAR(child->selfMs, child->totalMs - leaf->totalMs, 0.001);
        EXPECT_NEAR(leaf->selfMs, leaf->totalMs, 0.001);
        EXPECT_GE(leaf->totalMs, 0.2);
        EXPECT_GE(child->totalMs, 0.6);
    }
}

TEST_F(Profiler, ScopeSplitAtFrameBoundary){
    {
        JE_PROFILE_SCOPE("TestSpanning");
        spin(std::chrono::microseconds(300));
        nextFrame();
        spin(std::chrono::microseconds(300));
    }
    nextFrame();
    ASSERT_EQ(profiler::frameCount(), 3u);
    const profiler::Frame& first = *profiler::frameAt(1);
    const profiler::Frame& second = *profiler::frameAt(2);
    const std::vector<profiler::TreeRow> a = profiler::buildTree(first, profiler::mainThreadId());
    const std::vector<profiler::TreeRow> b = profiler::buildTree(second, profiler::mainThreadId());
    const profiler::TreeRow* before = row(a, "TestSpanning");
    const profiler::TreeRow* after = row(b, "TestSpanning");
    ASSERT_TRUE(before && after);
    // each frame's tree is complete: the scope is in both, a single call in all
    EXPECT_EQ(before->calls, 1u);
    EXPECT_EQ(after->calls, 0u);
    EXPECT_GE(before->totalMs, 0.25);
    EXPECT_GE(after->totalMs, 0.25);
    // and no part of it lies outside the frame it is in
    for (const profiler::Frame* frame : {&first, &second}){
        for (const profiler::ThreadTimeline& timeline : frame->threads){
            for (const profiler::Interval& interval : timeline.intervals){
                EXPECT_GE(interval.startNs, frame->startNs);
                EXPECT_LE(interval.endNs, frame->endNs);
            }
        }
    }
}

TEST_F(Profiler, RingOverflowDropsAndCounts){
    // far more than one ring holds (1 MiB: 65,536 events): never blocks, counts what it drops
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; i++){
        JE_PROFILE_SCOPE("TestFlood");
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    nextFrame();
    const profiler::Frame* frame = lastFrame();
    ASSERT_TRUE(frame);
    EXPECT_GT(frame->dropped, 0u);
    EXPECT_LT(seconds, 2.0);
    // the scopes that fit are still well formed
    const std::vector<profiler::TreeRow> rows = profiler::buildTree(*frame, profiler::mainThreadId());
    const profiler::TreeRow* flood = row(rows, "TestFlood");
    ASSERT_TRUE(flood);
    EXPECT_GT(flood->calls, 1000u);
    EXPECT_LT(flood->calls, 100000u);
    // and the next frame records again
    {
        JE_PROFILE_SCOPE("TestAfterFlood");
    }
    nextFrame();
    EXPECT_EQ(lastFrame()->dropped, 0u);
    EXPECT_TRUE(row(profiler::buildTree(*lastFrame(), profiler::mainThreadId()), "TestAfterFlood"));
}

TEST_F(Profiler, WorkerThreadsAppear){
    jobs::JobSystem& system = jobs::system();
    if (system.getWorkerCount() == 0)
        GTEST_SKIP() << "single-thread mode: no workers";
    std::atomic<int> done{0};
    for (int round = 0; round < 4; round++){
        system.parallelFor(8, [&](size_t){
            JE_PROFILE_SCOPE("TestJobWork");
            spin(std::chrono::milliseconds(2));
            done++;
        });
    }
    nextFrame();
    EXPECT_EQ(done.load(), 32);
    const profiler::Frame* frame = lastFrame();
    ASSERT_TRUE(frame);
    std::set<std::string> workerLanes;
    for (const profiler::ThreadTimeline& timeline : frame->threads){
        bool work = false;
        for (const profiler::Interval& interval : timeline.intervals)
            work = work || profiler::nameOf(interval.nameId) == "TestJobWork";
        if (!work)
            continue;
        for (const profiler::ThreadInfo& info : profiler::threads()){
            if (info.id == timeline.thread && info.name.rfind("Job Worker #", 0) == 0)
                workerLanes.insert(info.name);
        }
    }
    EXPECT_GE(workerLanes.size(), 2u);
}

TEST_F(Profiler, DynamicNamesIntern){
    const std::string name = std::string("Test") + "Dynamic";
    const uint32_t id = profiler::internName(name);
    EXPECT_EQ(profiler::internName("TestDynamic"), id);
    EXPECT_EQ(profiler::nameOf(id), "TestDynamic");
    {
        JE_PROFILE_SCOPE_DYN(name);
    }
    nextFrame();
    EXPECT_TRUE(row(profiler::buildTree(*lastFrame(), profiler::mainThreadId()), "TestDynamic"));
}

TEST_F(Profiler, CategoriesFollowTheStandardScopes){
    {
        JE_PROFILE_SCOPE("Draw");
        spin(std::chrono::microseconds(500));
        {
            JE_PROFILE_SCOPE("TestInsideDraw");   // inherits Rendering
            spin(std::chrono::microseconds(500));
        }
    }
    nextFrame();
    const profiler::Frame* frame = lastFrame();
    ASSERT_TRUE(frame);
    EXPECT_GE(frame->categoryMs[(size_t)profiler::Category::Rendering], 0.9);
}

TEST_F(Profiler, CaptureRoundTripsThroughJetraceJson){
    const profiler::CaptureId id = profiler::startCapture(3);
    ASSERT_NE(id, 0u);
    for (int frame = 0; frame < 12; frame++){
        {
            JE_PROFILE_SCOPE("TestCaptured");
            spin(std::chrono::microseconds(100));
        }
        JE_PROFILE_COUNTER("Test monitor", frame);
        nextFrame();
    }
    profiler::CaptureInfo info;
    ASSERT_TRUE(profiler::captureInfo(id, info));
    EXPECT_TRUE(info.complete);
    EXPECT_EQ(info.framesRecorded, 3u);

    test::TempDir dir;
    const std::string path = (dir.path() / "capture.jetrace.json").string();
    std::string error;
    ASSERT_TRUE(profiler::saveCapture(id, path, &error)) << error;
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    EXPECT_NE(text.str().find("\"traceEvents\""), std::string::npos);
    EXPECT_NE(text.str().find("\"jasper\":{\"version\":1"), std::string::npos);

    const profiler::CaptureId loaded = profiler::loadCapture(path, error);
    ASSERT_NE(loaded, 0u) << error;
    const std::vector<profiler::Frame>* frames = profiler::captureFrames(loaded);
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames->size(), 3u);
    uint32_t mainThread = 0;
    for (const profiler::ThreadInfo& thread : profiler::captureThreads(loaded)){
        if (thread.main)
            mainThread = thread.id;
    }
    for (const profiler::Frame& frame : *frames){
        const std::vector<profiler::TreeRow> rows = profiler::buildTree(frame, mainThread, [loaded](uint32_t nameId){
            return profiler::captureNameOf(loaded, nameId);
        });
        const profiler::TreeRow* captured = row(rows, "TestCaptured");
        ASSERT_TRUE(captured);
        EXPECT_EQ(captured->calls, 1u);
    }
    const std::vector<std::string> monitors = profiler::captureMonitorNames(loaded);
    EXPECT_NE(std::find(monitors.begin(), monitors.end(), "Test monitor"), monitors.end());

    // a newer major version is refused
    const std::string newer = (dir.path() / "newer.jetrace.json").string();
    std::ofstream(newer) << "{\"traceEvents\":[],\"jasper\":{\"version\":2}}";
    EXPECT_EQ(profiler::loadCapture(newer, error), 0u);
    EXPECT_NE(error.find("version 2"), std::string::npos);
    profiler::dropCapture(id);
    profiler::dropCapture(loaded);
}

TEST_F(Profiler, MonitorsKeepSeries){
    for (int i = 1; i <= 5; i++){
        JE_PROFILE_COUNTER("Test series", i * 10);
        nextFrame();
    }
    profiler::MonitorInfo info;
    ASSERT_TRUE(profiler::monitorInfo("Test series", info));
    EXPECT_DOUBLE_EQ(info.latest, 50.0);
    EXPECT_DOUBLE_EQ(info.maximum, 50.0);
    const std::vector<double> series = profiler::monitorSeries("Test series", 3);
    ASSERT_EQ(series.size(), 3u);
    EXPECT_DOUBLE_EQ(series[0], 30.0);
    EXPECT_DOUBLE_EQ(series[2], 50.0);
}

TEST_F(Profiler, GpuTimesOfTheFrameInProgressReachIt){
    // a light frame's commit can resolve before its frame ends (Vulkan at 1,000+ frames a
    // second): its times wait for the frame instead of being lost (GPU campaign perf2)
    nextFrame();
    const uint64_t current = FrameTimes::currentFrame();
    profiler::GpuPassSample sample;
    sample.label = "camera";
    sample.gpuMs = 0.5;
    sample.valid = true;
    profiler::recordGpuFrame(current, 2, 7, &sample, 1, 0.5, true, false);
    nextFrame();
    const profiler::Frame* frame = lastFrame();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->index, current);
    ASSERT_EQ(frame->gpu.size(), 1u);
    EXPECT_EQ(frame->gpu[0].label, "camera");
    EXPECT_EQ(frame->gpu[0].viewTag, 2u);
    EXPECT_EQ(frame->gpu[0].commit, 7u);
    EXPECT_DOUBLE_EQ(frame->gpu[0].gpuMs, 0.5);

    // a frame already marked takes its late times as before, and once
    sample.label = "late";
    profiler::recordGpuFrame(current, 2, 8, &sample, 1, 0.5, true, false);
    nextFrame();
    const profiler::Frame* marked = profiler::findFrame(current);
    ASSERT_NE(marked, nullptr);
    ASSERT_EQ(marked->gpu.size(), 2u);
    EXPECT_EQ(marked->gpu[1].label, "late");
    EXPECT_TRUE(lastFrame()->gpu.empty());
}

TEST(FrameTimes, RingKeepsTheNewestFrames){
    FrameTimes::clear();
    for (size_t i = 0; i < FrameTimes::CAPACITY + 10; i++){
        FrameTimes::beginFrame();
        FrameTimes::setDrawCalls((uint32_t)i);
        FrameTimes::endFrameWork();
    }
    FrameTimes::beginFrame();
    EXPECT_EQ(FrameTimes::size(), FrameTimes::CAPACITY);
    const FrameTimeSample newest = FrameTimes::latest();
    EXPECT_EQ(newest.drawCalls, (uint32_t)(FrameTimes::CAPACITY + 9));
    EXPECT_LE(newest.cpuMs, newest.frameMs);
    // a GPU time arrives frames late and patches its frame
    FrameTimes::setGpuMs(newest.frame, 1.5, true);
    EXPECT_FLOAT_EQ(FrameTimes::latest().gpuMs, 1.5f);
    EXPECT_TRUE(FrameTimes::latest().flags & FrameTimes::FLAG_GPU_SUM);
}

TEST(FrameTimes, HostWorkMarksItsFrameOnly){
    // the editor marks the frames an MCP tool ran in (S32 R22 leaves them out)
    FrameTimes::clear();
    FrameTimes::beginFrame();
    FrameTimes::markHostWork();
    FrameTimes::beginFrame();
    EXPECT_TRUE(FrameTimes::latest().flags & FrameTimes::FLAG_HOST_WORK);
    FrameTimes::beginFrame();
    EXPECT_FALSE(FrameTimes::latest().flags & FrameTimes::FLAG_HOST_WORK);
}

TEST_F(Profiler, CommandLineCaptureWritesItsFile){
    // a development game's --profile-capture (R38): armed once the scenes are loaded, written
    // when its frames are in
    profiler::setEnabled(false);
    test::TempDir dir;
    const std::string path = (dir.path() / "cli.jetrace.json").string();
    profiler::engineInit({"game", "--profile-capture=" + path, "--profile-frames", "3"});
    for (int frame = 0; frame < 4; frame++){
        nextFrame();
        profiler::engineFrameEnd(false);   // loading: nothing yet
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    for (int frame = 0; frame < 20 && !std::filesystem::exists(path); frame++){
        {
            JE_PROFILE_SCOPE("TestCli");
        }
        nextFrame();
        profiler::engineFrameEnd(true);
    }
    profiler::engineShutdown();
    EXPECT_TRUE(std::filesystem::exists(path)) << "after 20 loaded frames";
}

TEST(ProfilerBench, ScopePair){
    profiler::setEnabled(true);
    FrameTimes::beginFrame();
    profiler::frameMark();
    // the best of a few rounds (a loaded host preempts the odd one), the ring warm
    constexpr int pairs = 10000;
    auto measure = [](const auto& body){
        double best = 1e9;
        for (int round = 0; round < 5; round++){
            const auto start = std::chrono::steady_clock::now();
            body();
            best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / pairs);
            FrameTimes::beginFrame();
            profiler::frameMark();
        }
        return best;
    };
    const double ns = measure([](){
        for (int i = 0; i < pairs; i++){
            JE_PROFILE_SCOPE("BenchPair");
        }
    });
    profiler::setEnabled(false);
    // off: one relaxed load and a branch
    const double offNs = measure([](){
        for (int i = 0; i < pairs; i++){
            JE_PROFILE_SCOPE("BenchPairOff");
        }
    });
    profiler::clearHistory();
    // what the clock alone costs (two reads a pair)
    const double clockNs = measure([](){
        // unsigned: the sum wraps (a signed one overflows, UB that UBSan stops on)
        volatile uint64_t sink = 0;
        for (int i = 0; i < pairs; i++){
            sink = sink + static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            sink = sink + static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }
    });
    std::printf("[bench] profiler scope pair %.1f ns recording (two steady_clock reads: %.1f ns), %.2f ns off\n", ns, clockNs, offNs);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(ns, 50.0 * slack);
        EXPECT_LE(offNs, 2.0 * slack);
    }
}
