// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FunctionSubscribe.h"
#include "util/Signal.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    // out of line, so the direct call is a real std::function call too
    volatile int sink = 0;
}

TEST(SignalBench, InvokeOneSubscriberOverDirectCall) {
    constexpr int calls = 20000000;
    std::function<void(int)> direct = [](int v) { sink = sink + v; };
    Signal<int> signal;
    Subscription subscription = signal.subscribe([](int v) { sink = sink + v; });
    FunctionSubscribe<void(int)> event;
    event.add("one", [](int v) { sink = sink + v; });

    auto time = [](auto&& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; i++) body(i & 1);
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
    };
    const double directNs = time([&](int v) { direct(v); });
    const double signalNs = time([&](int v) { signal.invoke(v); });
    const double eventNs = time([&](int v) { event.call(v); });
    std::printf("[bench] std::function %.2f ns, Signal::invoke %.2f ns, FunctionSubscribe::call %.2f ns (budget +5)\n",
                directNs, signalNs, eventNs);
    if (perfSlack() > 0.0) {
        EXPECT_LE(signalNs - directNs, 5.0 * perfSlack());
        EXPECT_LE(eventNs - directNs, 5.0 * perfSlack());
    }
}
