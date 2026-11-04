// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Engine.h"
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

    // A C++ script written when Engine::onCharInput took wchar_t, and one written now
    struct WideCharScript {
        std::vector<uint32_t> got;
        void onCharInput(wchar_t c) { got.push_back(static_cast<uint32_t>(c)); }
        void subscribe() { REGISTER_ENGINE_EVENT(onCharInput); }
        void unsubscribe() { UNREGISTER_ENGINE_EVENT(onCharInput); }
    };

    struct CharScript {
        std::vector<char32_t> got;
        void onCharInput(char32_t c) { got.push_back(c); }
        void subscribe() { REGISTER_ENGINE_EVENT(onCharInput); }
        void unsubscribe() { UNREGISTER_ENGINE_EVENT(onCharInput); }
    };

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

TEST(FunctionSubscribeCompatTest, RemoveAndSubstring) {
    FunctionSubscribe<void()> event;
    int calls = 0;
    event.add("Script_123_onUpdate", [&]() { calls++; });
    event.add("Script_123_onDraw", [&]() { calls++; });
    event.add("Other_456_onUpdate", [&]() { calls += 10; });
    EXPECT_TRUE(event.remove("Script_123_onDraw"));
    EXPECT_FALSE(event.remove("missing"));
    EXPECT_EQ(event.removeByTagSubstring("_123_"), 1u);
    event();
    EXPECT_EQ(calls, 10);
    event.clear();
    EXPECT_FALSE(event.hasSubscribers());
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

TEST(FunctionSubscribeCompatTest, RemoveSelfDuringCall) {
    FunctionSubscribe<void()> event;
    std::vector<int> order;
    event.add("first", [&]() { order.push_back(1); event.remove("first"); });
    event.add("second", [&]() { order.push_back(2); });
    event.call();
    event.call();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 2})) << "removing itself no longer skips the next subscriber";
}

TEST(FunctionSubscribeCompatTest, RemoveNextDuringCallSkipsIt) {
    FunctionSubscribe<void()> event;
    std::vector<int> order;
    event.add("first", [&]() { order.push_back(1); event.remove("second"); });
    event.add("second", [&]() { order.push_back(2); });
    event.add("third", [&]() { order.push_back(3); });
    event.call();
    EXPECT_EQ(order, (std::vector<int>{1, 3}));
}

TEST(FunctionSubscribeCompatTest, AddDuringCallWaitsForTheNextCall) {
    FunctionSubscribe<void()> event;
    int late = 0;
    event.add("adder", [&]() {
        for (int i = 0; i < 64; i++) event.add("late" + std::to_string(i), [&late]() { late++; });
    });
    event.call();
    EXPECT_EQ(late, 0);
    event.remove("adder");
    event.call();
    EXPECT_EQ(late, 64);
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

TEST(FunctionSubscribeCompatTest, NestedCall) {
    FunctionSubscribe<void(int)> event;
    std::vector<int> seen;
    event.add("a", [&](int depth) {
        seen.push_back(depth);
        if (depth < 2) event.call(depth + 1);
    });
    event.call(1);
    EXPECT_EQ(seen, (std::vector<int>{1, 2}));
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

// Engine::onCharInput passes one whole character (char32_t); a handler declared with wchar_t
// still compiles and subscribes
TEST(FunctionSubscribeCompatTest, CharInputHandlersOfEitherTypeRegister) {
    WideCharScript wide;
    CharScript current;
    wide.subscribe();
    current.subscribe();
    Engine::onCharInput.call(U'a');
    Engine::onCharInput.call(U'\U0001F600');
    wide.unsubscribe();
    current.unsubscribe();
    EXPECT_EQ(current.got, (std::vector<char32_t>{U'a', U'\U0001F600'}));
    ASSERT_EQ(wide.got.size(), 2u);
    EXPECT_EQ(wide.got[0], static_cast<uint32_t>('a'));
    // the deprecated wchar_t form gets the character converted to wchar_t: whole where wchar_t is
    // 32 bits, its low 16 bits on Windows (docs/mcp/guide/cpp-scripting.md)
    EXPECT_EQ(wide.got[1], static_cast<uint32_t>(static_cast<wchar_t>(U'\U0001F600')));
    if constexpr (sizeof(wchar_t) == 4) {
        EXPECT_EQ(wide.got[1], 0x1F600u);
    } else {
        EXPECT_EQ(wide.got[1], 0xF600u);
    }
    Engine::onCharInput.call(U'b');
    EXPECT_EQ(current.got.size(), 2u) << "unsubscribed";
}
