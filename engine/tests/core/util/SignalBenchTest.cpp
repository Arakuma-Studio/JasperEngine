// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/CancellableEvent.h"
#include "util/FunctionSubscribe.h"
#include "util/Signal.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <functional>

namespace SignalBenchTest_cpp {

using namespace jasperengine;

namespace {
    // out of line, so the direct call is a real std::function call too
    volatile int signalBenchSink = 0;
}

TEST(SignalBench, InvokeOneSubscriberOverDirectCall) {
    constexpr int calls = 20000000;
    std::function<void(int)> direct = [](int v) { signalBenchSink = signalBenchSink + v; };
    Signal<int> signal;
    Subscription subscription = signal.subscribe([](int v) { signalBenchSink = signalBenchSink + v; });
    FunctionSubscribe<void(int)> event;
    event.add("one", [](int v) { signalBenchSink = signalBenchSink + v; });

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
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(signalNs - directNs, 5.0 * testsupport::perfSlack());
        EXPECT_LE(eventNs - directNs, 5.0 * testsupport::perfSlack());
    }
}

// cancellable-prioritised-events R26: an idle emit is one test (no CancelEventArgs made), and one
// listener costs at most 5 ns over Signal::invoke
TEST(SignalBench, CancellableEmitOverSignal) {
    constexpr int calls = 20000000;
    Signal<int> signal;
    Subscription subscription = signal.subscribe([](int v) { signalBenchSink = signalBenchSink + v; });
    CancellableEvent<int> idle("onIdle");
    CancellableEvent<int> busy("onBusy");
    Subscription listener = busy.subscribe([](CancelEventArgs&, int v) { signalBenchSink = signalBenchSink + v; });
    volatile bool plainFlag = false;

    auto time = [](auto&& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; i++) body(i & 1);
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
    };
    const double flagNs = time([&](int v) { if (plainFlag) signalBenchSink = signalBenchSink + v; });
    const double idleNs = time([&](int v) { if (idle.emit(v)) signalBenchSink = signalBenchSink + 1; });
    const double signalNs = time([&](int v) { signal.invoke(v); });
    const double busyNs = time([&](int v) { if (busy.emit(v)) signalBenchSink = signalBenchSink + 1; });
    std::printf("[bench] bool test %.2f ns, idle CancellableEvent::emit %.2f ns (budget +1); Signal::invoke %.2f ns, "
                "CancellableEvent::emit with 1 listener %.2f ns (budget +5)\n", flagNs, idleNs, signalNs, busyNs);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(idleNs - flagNs, 1.0 * testsupport::perfSlack());
        EXPECT_LE(busyNs - signalNs, 5.0 * testsupport::perfSlack());
    }
}

}  // namespace SignalBenchTest_cpp
