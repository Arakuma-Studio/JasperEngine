// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R15 (AC13): the queue's order, its cap with workers and its per-frame slice with none

#include "voxel/VoxelWorkQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::voxel;

namespace {

    void busyMs(double ms) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < ms) {
        }
    }

}

TEST(VoxelWorkQueue, SingleThreadStaysInBudget) {
    jobs::Config config;
    config.workers = 0;
    jobs::JobSystem system(config);
    WorkQueue queue(&system);
    const double jobMs = 1.0;
    for (uint64_t i = 0; i < 200; i++) {
        queue.push(i, WorkPriority::Far, static_cast<double>(i), [jobMs] { busyMs(jobMs); });
    }
    int frames = 0;
    size_t done = 0;
    std::vector<uint64_t> finished, dropped;
    while (done < 200 && frames < 2000) {
        const auto start = std::chrono::steady_clock::now();
        queue.pump();
        system.pump(4.0);
        const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        queue.pump();
        queue.takeFinished(finished, dropped);
        done += finished.size();
        frames++;
        // The pump's 4 ms, to within one job and this host's jitter (Evidence: wall clock goes through perfSlack)
        EXPECT_LT(frameMs, 4.0 + jobMs * 40.0);
    }
    EXPECT_EQ(done, 200u);
    // At most four a frame, one at least
    EXPECT_GE(frames, 50);
    EXPECT_LE(queue.peakInFlight(), 4u);
}

TEST(VoxelWorkQueue, FocusReprioritizes) {
    jobs::Config config;
    config.workers = 0;
    jobs::JobSystem system(config);
    WorkQueue queue(&system);
    std::vector<uint64_t> order;
    for (uint64_t i = 0; i < 6; i++) {
        queue.push(i, i == 0 ? WorkPriority::Edit : WorkPriority::Far, static_cast<double>(i), [i, &order] { order.push_back(i); });
    }
    // The focus moved: the last far block is now the nearest
    queue.reprioritize([](uint64_t id, WorkPriority& priority, double& distance) {
        if (id == 5) {
            priority = WorkPriority::Near;
            distance = 0.0;
        }
    });
    for (int frame = 0; frame < 20 && order.size() < 6; frame++) {
        queue.pump();
        system.pump(100.0);
    }
    ASSERT_EQ(order.size(), 6u);
    EXPECT_EQ(order[0], 0u);    // edit first
    EXPECT_EQ(order[1], 5u);    // then near
    EXPECT_EQ(order[2], 1u);    // then far, by distance
}

TEST(VoxelWorkQueue, InFlightCap) {
    jobs::Config config;
    config.workers = 4;
    jobs::JobSystem system(config);
    WorkQueue queue(&system);
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    for (uint64_t i = 0; i < 40; i++) {
        queue.push(i, WorkPriority::Far, 0.0, [&running, &peak] {
            const int now = running.fetch_add(1) + 1;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            busyMs(1.0);
            running.fetch_sub(1);
        });
    }
    std::vector<uint64_t> finished, dropped;
    size_t done = 0;
    while (done < 40) {
        queue.pump();
        queue.takeFinished(finished, dropped);
        done += finished.size();
        std::this_thread::yield();
    }
    EXPECT_LE(queue.peakInFlight(), 3u);   // max(1, workers - 1)
    EXPECT_LE(peak.load(), 3);
}

TEST(VoxelWorkQueue, CancelDropsQueuedWork) {
    jobs::Config config;
    config.workers = 0;
    jobs::JobSystem system(config);
    WorkQueue queue(&system);
    bool ran = false;
    queue.push(1, WorkPriority::Far, 0.0, [&ran] { ran = true; });
    EXPECT_TRUE(queue.cancel(1));
    queue.pump();
    system.pump(100.0);
    std::vector<uint64_t> finished, dropped;
    queue.takeFinished(finished, dropped);
    EXPECT_FALSE(ran);
    EXPECT_TRUE(finished.empty());
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0], 1u);
}

// A finished job tells the host before any pump: an editor that sleeps while idle wakes for it
TEST(VoxelWorkQueue, FinishedJobNotifiesBeforeThePump) {
    jobs::Config config;
    config.workers = 2;
    jobs::JobSystem system(config);
    WorkQueue queue(&system);
    std::atomic<int> notified{0};
    WorkQueue::setFinishedNotifier([&notified]() { notified++; });
    std::atomic<bool> ran{false};
    queue.push(1, WorkPriority::Visible, 0.0, [&ran] { ran = true; });
    queue.pump();
    // nothing pumps again: the notice comes from the worker
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (notified.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    WorkQueue::setFinishedNotifier(nullptr);
    EXPECT_TRUE(ran.load());
    EXPECT_EQ(notified.load(), 1);
    std::vector<uint64_t> finished, dropped;
    while (finished.empty() && std::chrono::steady_clock::now() < deadline) {
        queue.pump();
        queue.takeFinished(finished, dropped);
    }
    EXPECT_EQ(finished, std::vector<uint64_t>{1});
}
