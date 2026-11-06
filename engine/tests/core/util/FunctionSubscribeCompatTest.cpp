// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FunctionSubscribe.h"
#include "util/CrashHook.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <csignal>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct Listener {
        int calls = 0;
        void onEvent(int value) { calls += value; }
        bool filter(int value) { return value > 0; }
    };

    int freeCalls = 0;
    void freeFunction() { freeCalls++; }

}

TEST(FunctionSubscribeCompatTest, AddSameTagReplaces) {
    FunctionSubscribe<void(int)> event;
    int a = 0;
    int b = 0;
    event.add("tag", [&a](int v) { a += v; });
    event.add("tag", [&b](int v) { b += v; });
    event.call(1);
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(event.size(), 1u);
}

TEST(FunctionSubscribeCompatTest, MemberAndFreeFunctions) {
    FunctionSubscribe<void(int)> event;
    Listener listener;
    event.add<Listener, &Listener::onEvent>("member", &listener);
    event.call(3);
    EXPECT_EQ(listener.calls, 3);
    FunctionSubscribe<void()> plain;
    freeCalls = 0;
    plain.add<&freeFunction>("free");
    plain();
    EXPECT_EQ(freeCalls, 1);
}

TEST(FunctionSubscribeCompatTest, CallRetFirstResultOrDefault) {
    FunctionSubscribe<bool(int)> event;
    EXPECT_TRUE(event.callRet(1, true)) << "no subscribers: the default";
    Listener listener;
    event.add<Listener, &Listener::filter>("first", &listener);
    event.add("second", [](int) { return true; });
    EXPECT_FALSE(event.callRet(-1, true)) << "the first subscriber answers";
    EXPECT_TRUE(event.callRet(5, false));
    event.setEnabled(false);
    EXPECT_TRUE(event.callRet(-1, true)) << "disabled: the default";
}

TEST(FunctionSubscribeCompatTest, SetEnabled) {
    FunctionSubscribe<void()> event;
    int calls = 0;
    event.add("t", [&]() { calls++; });
    event.setEnabled(false);
    EXPECT_FALSE(event.isEnabled());
    event.call();
    event.setEnabled(true);
    event.call();
    EXPECT_EQ(calls, 1);
}

TEST(FunctionSubscribeCompatTest, CopyCopiesSubscribers) {
    FunctionSubscribe<void()> event;
    int calls = 0;
    event.add("t", [&]() { calls++; });
    FunctionSubscribe<void()> copy = event;
    copy.call();
    event.remove("t");
    copy.call();
    event.call();
    EXPECT_EQ(calls, 2);
    FunctionSubscribe<void()> moved = std::move(copy);
    moved.call();
    EXPECT_EQ(calls, 3);
}

TEST(FunctionSubscribeCompatTest, ClearDuringCall) {
    FunctionSubscribe<void()> event;
    int calls = 0;
    event.add("a", [&]() { calls++; event.clear(); });
    event.add("b", [&]() { calls++; });
    event.call();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(event.hasSubscribers());
}

TEST(FunctionSubscribeCompatTest, SubscribeAndOwners) {
    FunctionSubscribe<void()> event;
    int calls = 0;
    int owner = 0;
    {
        Subscription subscription = event.subscribe([&]() { calls++; });
        event.call();
    }
    event.call();
    event.add("owned", [&]() { calls += 10; }, ownerKeyOf(&owner));
    EXPECT_EQ(event.removeOwner(ownerKeyOf(&owner)), 1u);
    event.call();
    EXPECT_EQ(calls, 1);
}

TEST(FunctionSubscribeCompatTest, ZeroAllocCall) {
    FunctionSubscribe<void(int)> event;
    int sum = 0;
    event.add("a", [&sum](int v) { sum += v; });
    event.add("b", [&sum](int v) { sum += v; });
    event.call(1);
    testsupport::ThreadAllocationCount count;
    for (int i = 0; i < 1000; i++) event.call(1);
    EXPECT_EQ(count.count(), 0u);
    EXPECT_EQ(sum, 2002);
}

#if !defined(__EMSCRIPTEN__) && !defined(JASPERENGINE_SANITIZE_THREAD)
namespace {
    // The subscribers' deliberate crash: a null store, which the crash guard's SIGSEGV handler
    // catches. Under ASan and UBSan the same signal is raised instead: UBSan halts on the null
    // store as undefined behaviour before any handler runs (-fno-sanitize-recover), and a raised
    // SIGSEGV goes through the guard's handler as the fault does.
    void crashHere() {
#if defined(JASPERENGINE_SANITIZE_UNDEFINED) || defined(JASPERENGINE_SANITIZE_ADDRESS)
        std::raise(SIGSEGV);
#else
        volatile int* nothing = nullptr;
        *nothing = 1;
#endif
    }
}

TEST(FunctionSubscribeCompatTest, CrashingSubscriberIsRemovedAndReported) {
    std::vector<std::string> reported;
    CrashHook::set([&reported](const std::string& tag, const std::string& error) { reported.push_back(tag + ": " + error); });
    FunctionSubscribe<void()> event;
    int before = 0;
    int after = 0;
    event.add("before", [&]() { before++; });
    event.add("crasher", []() { crashHere(); });
    event.add("after", [&]() { after++; });
    event.call();
    event.call();
    CrashHook::set(nullptr);
    EXPECT_EQ(before, 2);
    EXPECT_EQ(after, 2) << "later subscribers still run";
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_EQ(reported[0].rfind("crasher: ", 0), 0u) << reported[0];
    EXPECT_EQ(event.size(), 2u) << "the crashing subscriber is removed";

    // callRet: the next subscriber answers
    CrashHook::set([](const std::string&, const std::string&) {});
    FunctionSubscribe<int()> ret;
    ret.add("crasher", []() {
        crashHere();
        return 0;
    });
    ret.add("answer", []() { return 42; });
    EXPECT_EQ(ret.callRet(7), 42);
    CrashHook::set(nullptr);
}
#endif
