// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/Signal.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

TEST(SignalTest, SubscribeInvokeUnsubscribe) {
    Signal<int> signal;
    int sum = 0;
    {
        Subscription subscription = signal.subscribe([&sum](int value) { sum += value; });
        EXPECT_EQ(signal.size(), 1u);
        signal.invoke(2);
        signal(3);
    }
    signal.invoke(100);
    EXPECT_EQ(sum, 5);
    EXPECT_TRUE(signal.empty());
}

TEST(SignalTest, UnsubscribeSelfDuringInvoke) {
    Signal<> signal;
    Subscription self;
    int calls = 0;
    std::string capturedAlive = "alive";
    self = signal.subscribe([&self, &calls, capturedAlive]() {
        calls++;
        self.reset();
        // the running callback (and what it captured) is still there
        EXPECT_EQ(capturedAlive, "alive");
    });
    signal.invoke();
    signal.invoke();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(signal.empty());
}

TEST(SignalTest, UnsubscribeNextDuringInvokeSkipsIt) {
    Signal<> signal;
    std::vector<int> order;
    Subscription second;
    Subscription first = signal.subscribe([&]() { order.push_back(1); second.reset(); });
    second = signal.subscribe([&]() { order.push_back(2); });
    Subscription third = signal.subscribe([&]() { order.push_back(3); });
    signal.invoke();
    EXPECT_EQ(order, (std::vector<int>{1, 3})) << "the removed one is skipped, the next one is not";
}

TEST(SignalTest, SubscribeDuringInvokeNotCalled) {
    Signal<> signal;
    std::vector<Subscription> added;
    int lateCalls = 0;
    Subscription adder = signal.subscribe([&]() {
        // enough to grow the subscriber list
        for (int i = 0; i < 64; i++) added.push_back(signal.subscribe([&lateCalls]() { lateCalls++; }));
    });
    signal.invoke();
    EXPECT_EQ(lateCalls, 0);
    adder.reset();
    added.resize(1);
    signal.invoke();
    EXPECT_EQ(lateCalls, 1);
}

TEST(SignalTest, ClearDuringInvoke) {
    Signal<> signal;
    int calls = 0;
    Subscription a = signal.subscribe([&]() { calls++; signal.clear(); });
    Subscription b = signal.subscribe([&]() { calls++; });
    signal.invoke();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(signal.empty());
}

TEST(SignalTest, NestedInvoke) {
    Signal<int> signal;
    std::vector<int> seen;
    Subscription a = signal.subscribe([&](int depth) {
        seen.push_back(depth);
        if (depth < 3) signal.invoke(depth + 1);
    });
    Subscription b = signal.subscribe([&](int depth) { seen.push_back(100 + depth); });
    signal.invoke(1);
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3, 103, 102, 101}));
}

TEST(SignalTest, HandleOutlivesSignal) {
    Subscription subscription;
    {
        Signal<> signal;
        subscription = signal.subscribe([]() {});
        EXPECT_TRUE(subscription.active());
    }
    EXPECT_FALSE(subscription.active());
    subscription.reset();
}

TEST(SignalTest, SignalOutlivesHandle) {
    Signal<> signal;
    int calls = 0;
    { Subscription subscription = signal.subscribe([&]() { calls++; }); }
    signal.invoke();
    EXPECT_EQ(calls, 0);
}

TEST(SignalTest, SignalDestroyedDuringInvoke) {
    auto signal = std::make_unique<Signal<>>();
    int calls = 0;
    Subscription a = signal->subscribe([&]() { calls++; signal.reset(); });
    Subscription b = signal->subscribe([&]() { calls++; });
    signal->invoke();
    EXPECT_EQ(calls, 2) << "the invoke in progress finishes on the list it started with";
    EXPECT_FALSE(a.active());
}

TEST(SignalTest, MoveHandle) {
    Signal<> signal;
    int calls = 0;
    Subscription first = signal.subscribe([&]() { calls++; });
    Subscription moved = std::move(first);
    EXPECT_FALSE(first.active());
    EXPECT_TRUE(moved.active());
    signal.invoke();
    Subscription assigned;
    assigned = std::move(moved);
    signal.invoke();
    assigned.reset();
    signal.invoke();
    EXPECT_EQ(calls, 2);
}

TEST(SignalTest, RemoveOwner) {
    Signal<> signal;
    int calls = 0;
    int owner = 0;
    Subscription a = signal.subscribe([&]() { calls++; }, ownerKeyOf(&owner));
    Subscription b = signal.subscribe([&]() { calls += 10; });
    EXPECT_EQ(signal.removeOwner(ownerKeyOf(&owner)), 1u);
    signal.invoke();
    EXPECT_EQ(calls, 10);
}

TEST(SignalTest, ZeroAllocInvoke) {
    Signal<int> signal;
    int sum = 0;
    std::vector<Subscription> subscriptions;
    for (int i = 0; i < 8; i++) subscriptions.push_back(signal.subscribe([&sum](int v) { sum += v; }));
    signal.invoke(1);
    testsupport::ThreadAllocationCount count;
    for (int i = 0; i < 1000; i++) signal.invoke(1);
    EXPECT_EQ(count.count(), 0u);
    EXPECT_EQ(sum, 8 * 1001);
}

TEST(ThreadSafeSignalTest, SubscribeAndInvokeFromThreads) {
    ThreadSafeSignal<int> signal;
    std::atomic<int> sum{0};
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};
    std::thread invoker([&]() {
        while (!stop.load()) signal.invoke(1);
    });
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 200; i++) {
                Subscription subscription = signal.subscribe([&sum](int v) { sum.fetch_add(v); });
                std::this_thread::yield();
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    stop = true;
    invoker.join();
    EXPECT_TRUE(signal.empty());
    // unsubscribed before its turn: not called
    int calls = 0;
    Subscription late;
    Subscription first = signal.subscribe([&](int) { late.reset(); calls++; });
    late = signal.subscribe([&](int) { calls += 10; });
    signal.invoke(0);
    EXPECT_EQ(calls, 1);
}
