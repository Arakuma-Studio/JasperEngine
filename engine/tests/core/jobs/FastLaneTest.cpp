// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The lock-free ring in front of the High and Normal lanes (detail::FastLane): FIFO across its
// overflow, and exactly-once delivery with several producers and consumers (run under TSan).

#include "jobs/detail/Scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace jasperengine::jobs::detail;

namespace{
    // more than the ring holds, so the tail goes through the overflow
    constexpr size_t kItems = 40000;
}

TEST(JobsFastLane, FifoAcrossTheOverflow){
    std::vector<Runnable> items(kItems);
    auto lane = std::make_unique<FastLane>();
    size_t next = 0;
    size_t expected = 0;
    auto take = [&](size_t max){
        Runnable* batch[16];
        const size_t count = lane->popBatch(batch, max);
        for (size_t i = 0; i < count; i++){
            ASSERT_EQ(batch[i], &items[expected]) << "item " << expected << " out of order";
            expected++;
        }
    };
    // fill past the ring, then alternate pushes and takes while the overflow drains
    for (; next < kItems * 7 / 8; next++)
        lane->push(&items[next]);
    while (expected < kItems){
        if (next < kItems)
            lane->push(&items[next++]);
        take(1 + expected % 16);
    }
    EXPECT_FALSE(lane->hasItems());
    EXPECT_EQ(lane->pop(), nullptr);
}

TEST(JobsFastLane, ProducersAndConsumersDeliverEachItemOnce){
    constexpr int kProducers = 3;
    constexpr int kConsumers = 4;
    constexpr size_t kPerProducer = 60000;
    std::vector<Runnable> items(kProducers * kPerProducer);
    std::vector<std::atomic<int>> seen(items.size());
    auto lane = std::make_unique<FastLane>();
    std::atomic<size_t> taken{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; p++){
        threads.emplace_back([&, p]{
            for (size_t i = 0; i < kPerProducer; i++)
                lane->push(&items[p * kPerProducer + i]);
        });
    }
    for (int c = 0; c < kConsumers; c++){
        threads.emplace_back([&, c]{
            Runnable* batch[8];
            while (taken.load() < items.size()){
                const size_t count = lane->popBatch(batch, 1 + (size_t)c * 2);
                for (size_t i = 0; i < count; i++)
                    seen[(size_t)(batch[i] - items.data())].fetch_add(1);
                taken.fetch_add(count);
                if (count == 0)
                    std::this_thread::yield();
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    for (size_t i = 0; i < items.size(); i++)
        ASSERT_EQ(seen[i].load(), 1) << "item " << i;
    EXPECT_FALSE(lane->hasItems());
}
