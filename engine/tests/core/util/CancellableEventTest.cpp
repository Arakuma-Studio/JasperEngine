// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CancellableEvent<Args...> (engine/core/util/CancellableEvent.h,
// docs/specs/cancellable-prioritised-events.md AC12-AC13): priority order, skip after a veto, the
// monitor pass, nesting, re-entrancy, owners, failing listeners and the idle cost.

#include "util/CancellableEvent.h"
#include "util/CrashHook.h"
#include "support/AllocationCounter.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace CancellableEventTest_cpp {

using namespace jasperengine;
using jasperengine::test::LogCapture;

namespace {

    struct CancellableCrashHandlerScope {
        explicit CancellableCrashHandlerScope(CrashHandler handler) {
            CrashHook::set(std::move(handler));
        }
        ~CancellableCrashHandlerScope() {
            CrashHook::set(nullptr);
        }
    };

    std::string joinedCancellableLog(const std::vector<std::string>& log) {
        std::string out;
        for (const std::string& entry : log) out += (out.empty() ? "" : " ") + entry;
        return out;
    }

}

TEST(CancellableEventTest, OrderByPriorityThenAddOrder) {
    CancellableEvent<> event("onTest");
    std::vector<std::string> log;
    std::vector<Subscription> subscriptions;
    auto add = [&](const char* name, int32_t priority) {
        subscriptions.push_back(event.subscribe([&log, name](CancelEventArgs&) { log.push_back(name); }, priority));
    };
    add("a0", 0);
    add("b10", 10);
    add("c-5", -5);
    add("d10", 10);
    add("eMonitor", EventPriority::Monitor);
    add("f0", 0);
    EXPECT_FALSE(event.emit());
    EXPECT_EQ(joinedCancellableLog(log), "b10 d10 a0 f0 c-5 eMonitor");
    EXPECT_EQ(event.size(), 6u);
}

TEST(CancellableEventTest, SkipsDecidersAfterTheVetoThenRunsMonitors) {
    CancellableEvent<int> event("onTest");
    std::vector<std::string> log;
    Subscription vetoer = event.subscribe([&](CancelEventArgs& e, int v) {
        log.push_back("p10");
        if (v == 1) e.cancel();
    }, 10);
    Subscription later = event.subscribe([&](CancelEventArgs&, int) { log.push_back("p5"); }, 5);
    Subscription monitor = event.subscribe([&](CancelEventArgs& e, int) {
        log.push_back(std::string("monitor:") + (e.isCancelled() ? "cancelled" : "ahead") + (e.isMonitorPass() ? ":pass" : ""));
    }, EventPriority::Monitor);

    EXPECT_TRUE(event.emit(1));
    EXPECT_EQ(joinedCancellableLog(log), "p10 monitor:cancelled:pass");
    log.clear();
    EXPECT_FALSE(event.emit(2));
    EXPECT_EQ(joinedCancellableLog(log), "p10 p5 monitor:ahead:pass");
}

TEST(CancellableEventTest, NestedEmitHasItsOwnArgs) {
    CancellableEvent<int> event("onTest");
    bool inner = false;
    bool outerSawCancel = true;
    Subscription outer = event.subscribe([&](CancelEventArgs& e, int v) {
        if (v == 0) {
            inner = event.emit(1);
            outerSawCancel = e.isCancelled();
        }
    }, 10);
    Subscription canceller = event.subscribe([](CancelEventArgs& e, int v) {
        if (v == 1) e.cancel();
    });
    EXPECT_FALSE(event.emit(0));
    EXPECT_TRUE(inner);
    EXPECT_FALSE(outerSawCancel);
}

TEST(CancellableEventTest, SubscribeAndUnsubscribeDuringEmit) {
    CancellableEvent<> event("onTest");
    std::vector<std::string> log;
    Subscription late;
    Subscription second;
    Subscription first = event.subscribe([&](CancelEventArgs&) {
        log.push_back("first");
        if (!late.active()) late = event.subscribe([&](CancelEventArgs&) { log.push_back("late"); }, 10);
        second.reset();
    });
    second = event.subscribe([&](CancelEventArgs&) { log.push_back("second"); });
    EXPECT_FALSE(event.emit());
    EXPECT_FALSE(event.emit());
    EXPECT_EQ(joinedCancellableLog(log), "first late first") << "added during an emit: from the next one, at its priority";

    log.clear();
    Subscription clearer = event.subscribe([&](CancelEventArgs&) { log.push_back("clearer"); event.clear(); }, 20);
    EXPECT_FALSE(event.emit());
    EXPECT_EQ(joinedCancellableLog(log), "clearer") << "clear during an emit skips every listener that has not run";
    EXPECT_FALSE(event.hasListeners());
}

TEST(CancellableEventTest, OwnerRemoval) {
    CancellableEvent<> event("onTest");
    int owner = 0;
    int calls = 0;
    Subscription owned = event.subscribe([&](CancelEventArgs& e) { calls++; e.cancel(); }, 0, ownerKeyOf(&owner));
    Subscription other = event.subscribe([&](CancelEventArgs&) { calls += 10; }, -1);
    EXPECT_TRUE(event.emit());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(SubscriptionOwnerIndex::removeOwner(ownerKeyOf(&owner)), 1u);
    EXPECT_FALSE(event.emit());
    EXPECT_EQ(calls, 11);
    EXPECT_EQ(event.removeOwner(ownerKeyOf(&owner)), 0u);
}

TEST(CancellableEventTest, ThrowingListenerIsLoggedAndTheOthersRun) {
    CancellableEvent<> event("onThrowTest");
    LogCapture log;
    int monitorCalls = 0;
    Subscription thrower = event.subscribe([](CancelEventArgs& e) {
        e.cancel();
        throw std::runtime_error("listener failed");
    }, 10);
    Subscription monitor = event.subscribe([&](CancelEventArgs&) { monitorCalls++; }, EventPriority::Monitor);
    EXPECT_TRUE(event.emit()) << "the cancel made before the throw stands";
    EXPECT_EQ(monitorCalls, 1);
    EXPECT_EQ(log.count(D_LOG_ERROR, "onThrowTest listener"), 1u) << log.describeErrors();
    EXPECT_EQ(event.size(), 2u) << "without a crash hook the listener stays";
}

TEST(CancellableEventTest, CrashGuardRemovesAFailingListener) {
    std::vector<std::string> crashes;
    CancellableCrashHandlerScope scope([&](const std::string& tag, const std::string&) { crashes.push_back(tag); });
    CancellableEvent<> event("onGuardTest");
    int calls = 0;
    Subscription thrower = event.subscribe([](CancelEventArgs&) { throw std::runtime_error("listener failed"); }, 10);
    Subscription other = event.subscribe([&](CancelEventArgs&) { calls++; });
    EXPECT_FALSE(event.emit());
    EXPECT_FALSE(event.emit());
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(crashes, (std::vector<std::string>{"onGuardTest listener"}));
    EXPECT_EQ(event.size(), 1u);
}

TEST(CancellableEventTest, DestroyedDuringEmit) {
    LogCapture log;
    auto event = std::make_unique<CancellableEvent<>>("onDestroyTest");
    int monitorCalls = 0;
    Subscription destroyer = event->subscribe([&](CancelEventArgs&) { event.reset(); }, 10);
    Subscription monitor = event->subscribe([&](CancelEventArgs& e) { monitorCalls++; e.cancel(); }, EventPriority::Monitor);
    EXPECT_FALSE(event->emit());
    EXPECT_EQ(event, nullptr);
    EXPECT_EQ(monitorCalls, 1);
    EXPECT_EQ(log.count(D_LOG_ERROR, "onDestroyTest: a monitor listener cannot cancel"), 1u) << log.describeErrors();
}

TEST(CancellableEventTest, EmitAllocatesNothing) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "no allocation counter on this platform";
    CancellableEvent<int> idle("onIdle");
    CancellableEvent<int> busy("onBusy");
    int sum = 0;
    std::vector<Subscription> subscriptions;
    for (int i = 0; i < 8; i++) subscriptions.push_back(busy.subscribe([&sum](CancelEventArgs&, int v) { sum += v; }, i));
    EXPECT_FALSE(busy.emit(1));
    testsupport::ThreadAllocationCount count;
    for (int i = 0; i < 1000; i++) {
        EXPECT_FALSE(idle.emit(1));
        EXPECT_FALSE(busy.emit(1));
    }
    EXPECT_EQ(count.count(), 0u);
    EXPECT_EQ(sum, 8 * 1001);
}

// AC13 (R19)
TEST(CancellableEventTest, MonitorCancelLogsOnce) {
    CancellableEvent<> event("onMonitorTest");
    LogCapture log;
    Subscription monitor = event.subscribe([](CancelEventArgs& e) { e.cancel(); }, EventPriority::Monitor);
    EXPECT_FALSE(event.emit());
    EXPECT_FALSE(event.emit());
    EXPECT_EQ(log.count(D_LOG_ERROR, "onMonitorTest: a monitor listener cannot cancel; give it a priority instead"), 1u)
        << log.describeErrors();
    EXPECT_EQ(log.errors().size(), 1u) << log.describeErrors();
}

}  // namespace CancellableEventTest_cpp
