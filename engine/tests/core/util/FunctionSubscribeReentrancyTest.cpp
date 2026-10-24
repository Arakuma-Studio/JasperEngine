// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// util/FunctionSubscribe, the event lists of the engine (Engine::onUpdate, component events,
// physics callbacks), on S09's SlotList: a callback may unsubscribe itself or another
// subscriber, subscribe, call the same event again or destroy the event's owner while the
// event runs, and every subscriber still runs exactly as subscribed when the call started (a
// call in progress finishes on the list it started with, also when the event goes away, so a
// component array relocating its components mid-call loses no callback). A copy has the same
// subscribers, changed apart from the original's; the crash guard (CrashHook) drops a crashing
// subscriber without touching the possibly destroyed event. The cases come from upstream's
// FunctionSubscribe rewrite; FunctionSubscribeCompatTest and SignalTest hold S09's own.

#include <gtest/gtest.h>

#include "util/CrashHook.h"
#include "util/FunctionSubscribe.h"

#include <csignal>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    using Event = FunctionSubscribe<void()>;
    using IntEvent = FunctionSubscribe<void(int)>;
    using BoolEvent = FunctionSubscribe<bool(int)>;

    struct Listener {
        std::vector<std::string>* log = nullptr;
        std::string name;

        void onEvent(int value) {
            log->push_back(name + ":" + std::to_string(value));
        }
    };

}

TEST(FunctionSubscribe, CallsInSubscriptionOrderAndReplacesATag) {
    std::vector<std::string> log;
    Event event;
    event.add("a", [&](){ log.push_back("a"); });
    event.add("b", [&](){ log.push_back("b"); });
    event.add("c", [&](){ log.push_back("c"); });
    // the same tag replaces the subscription, moved to the end
    event.add("a", [&](){ log.push_back("a2"); });

    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"b", "c", "a2"}));
    EXPECT_TRUE(event.hasSubscribers());

    EXPECT_TRUE(event.remove("b"));
    EXPECT_FALSE(event.remove("b"));
    EXPECT_EQ(event.removeByTagSubstring("a"), 1u);
    log.clear();
    event();
    EXPECT_EQ(log, (std::vector<std::string>{"c"}));

    event.clear();
    EXPECT_FALSE(event.hasSubscribers());
    event.call();
    EXPECT_EQ(log.size(), 1u);
}

TEST(FunctionSubscribe, BindsMemberFunctions) {
    std::vector<std::string> log;
    Listener first{&log, "first"};
    auto second = std::make_shared<Listener>(Listener{&log, "second"});

    IntEvent event;
    event.add<Listener, &Listener::onEvent>("first", &first);
    event.add<Listener, &Listener::onEvent>("second", second);
    event.call(7);

    EXPECT_EQ(log, (std::vector<std::string>{"first:7", "second:7"}));
}

TEST(FunctionSubscribe, DisabledEventCallsNothing) {
    int calls = 0;
    Event event;
    event.add("a", [&](){ calls++; });
    event.setEnabled(false);
    EXPECT_FALSE(event.isEnabled());
    event.call();
    EXPECT_EQ(calls, 0);

    BoolEvent query;
    query.add("a", [&](int){ calls++; return true; });
    query.setEnabled(false);
    EXPECT_FALSE(query.call(1));
    EXPECT_FALSE(query.callRet(1, false));
    EXPECT_EQ(calls, 0);
}

TEST(FunctionSubscribe, UnsubscribingItselfDuringCallKeepsTheOthers) {
    std::vector<std::string> log;
    Event event;
    event.add("a", [&](){ log.push_back("a"); });
    event.add("self", [&](){ log.push_back("self"); event.remove("self"); });
    event.add("b", [&](){ log.push_back("b"); });
    event.add("c", [&](){ log.push_back("c"); });

    event.call();
    // no subscriber skipped after the one removed
    EXPECT_EQ(log, (std::vector<std::string>{"a", "self", "b", "c"}));

    log.clear();
    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(FunctionSubscribe, UnsubscribingAnotherDuringCallSkipsIt) {
    std::vector<std::string> log;
    Event event;
    event.add("a", [&](){ log.push_back("a"); });
    event.add("remover", [&](){
        log.push_back("remover");
        event.remove("later");
        event.remove("a");
    });
    event.add("later", [&](){ log.push_back("later"); });
    event.add("last", [&](){ log.push_back("last"); });

    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "remover", "last"}));

    log.clear();
    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"remover", "last"}));
}

TEST(FunctionSubscribe, RemovingByTagSubstringDuringCallSkipsThem) {
    std::vector<std::string> log;
    Event event;
    event.add("script_1_update", [&](){
        log.push_back("first");
        event.removeByTagSubstring("script_2");
    });
    event.add("script_2_update", [&](){ log.push_back("second"); });
    event.add("script_2_late", [&](){ log.push_back("second late"); });
    event.add("script_3_update", [&](){ log.push_back("third"); });

    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"first", "third"}));
}

TEST(FunctionSubscribe, SubscribingDuringCallRunsFromTheNextCall) {
    std::vector<std::string> log;
    Event event;
    int added = 0;
    event.add("spawner", [&](){
        log.push_back("spawner");
        // many, so a plain vector would reallocate under the running loop
        for (int i = 0; i < 64; i++) {
            std::string tag = "spawned" + std::to_string(added++);
            event.add(tag, [&log, tag](){ log.push_back(tag); });
        }
    });
    event.add("b", [&](){ log.push_back("b"); });

    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"spawner", "b"}));

    log.clear();
    event.remove("spawner");
    event.call();
    ASSERT_EQ(log.size(), 65u);
    EXPECT_EQ(log.front(), "b");
    EXPECT_EQ(log[1], "spawned0");
    EXPECT_EQ(log.back(), "spawned63");
}

TEST(FunctionSubscribe, ReplacingItsOwnTagDuringCall) {
    std::vector<std::string> log;
    Event event;
    event.add("self", [&](){
        log.push_back("old");
        event.add("self", [&](){ log.push_back("new"); });
    });
    event.add("b", [&](){ log.push_back("b"); });

    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"old", "b"}));

    log.clear();
    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"b", "new"}));
}

TEST(FunctionSubscribe, NestedCalls) {
    std::vector<std::string> log;
    IntEvent event;
    event.add("a", [&](int depth){
        log.push_back("a" + std::to_string(depth));
        if (depth == 0) {
            event.call(1);
        }
    });
    event.add("b", [&](int depth){
        log.push_back("b" + std::to_string(depth));
        // removed by the nested call: the outer call skips it too
        if (depth == 1) {
            event.remove("c");
        }
    });
    event.add("c", [&](int depth){ log.push_back("c" + std::to_string(depth)); });

    event.call(0);
    EXPECT_EQ(log, (std::vector<std::string>{"a0", "a1", "b1", "b0"}));
}

TEST(FunctionSubscribe, DestroyingTheEventDuringCallFinishesTheCall) {
    std::vector<std::string> log;
    auto event = std::make_unique<Event>();
    // captured by value: the closure must outlive the event that owns it
    std::unique_ptr<Event>* owner = &event;
    event->add("a", [&log](){ log.push_back("a"); });
    event->add("destroyer", [&log, owner](){
        log.push_back("destroyer");
        owner->reset();
        log.push_back("after reset");
    });
    event->add("c", [&log](){ log.push_back("c"); });

    event->call();
    EXPECT_EQ(event, nullptr);
    // the call in progress ends on the list it started with (SlotList::orphan keeps it alive)
    EXPECT_EQ(log, (std::vector<std::string>{"a", "destroyer", "after reset", "c"}));
}

TEST(FunctionSubscribe, ReturnValues) {
    BoolEvent query;
    // nobody subscribed: the default
    EXPECT_FALSE(query.call(1));
    EXPECT_TRUE(query.callRet(1, true));
    EXPECT_FALSE(query.hasSubscribers());

    // the first subscriber answers
    int secondCalls = 0;
    query.add("first", [](int value){ return value > 0; });
    query.add("second", [&](int){ secondCalls++; return false; });
    EXPECT_TRUE(query.call(1));
    EXPECT_FALSE(query.call(-1));
    EXPECT_TRUE(query.callRet(1, false));
    EXPECT_EQ(secondCalls, 0);

    // one that unsubscribes itself still answers once
    BoolEvent once;
    once.add("once", [&](int){ once.remove("once"); return true; });
    EXPECT_TRUE(once.callRet(0, false));
    EXPECT_FALSE(once.callRet(0, false));
    EXPECT_FALSE(once.hasSubscribers());

    FunctionSubscribe<float(float)> ease;
    ease.add("half", [](float t){ return t * 0.5f; });
    EXPECT_FLOAT_EQ(ease.call(3.0f), 1.5f);
    EXPECT_FLOAT_EQ(ease(1.0f), 0.5f);
}

TEST(FunctionSubscribe, CopiesKeepTheSubscriptions) {
    std::vector<std::string> log;
    auto original = std::make_unique<Event>();
    original->add("a", [&](){ log.push_back("a"); });
    original->add("b", [&](){ log.push_back("b"); });

    // a relocated component: copied, then the old one destroyed
    Event copy(*original);
    original.reset();
    copy.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));

    // a change to one copy leaves the other as it was
    Event other;
    other = copy;
    other.remove("a");
    other.add("c", [&](){ log.push_back("c"); });
    log.clear();
    copy.call();
    other.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b", "b", "c"}));

    // self-assignment keeps them
    Event& alias = copy;
    copy = alias;
    log.clear();
    copy.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));

    // removed from every copy: a running call of either skips it
    Event shared(copy);
    copy.add("remover", [&](){ copy.remove("x"); shared.remove("x"); });
    copy.add("x", [&](){ log.push_back("x"); });
    shared = copy;
    log.clear();
    copy.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));
}

TEST(FunctionSubscribe, CopyMadeDuringCallKeepsTheSubscription) {
    std::vector<std::string> log;
    auto event = std::make_unique<Event>();
    std::unique_ptr<Event> moved;
    std::unique_ptr<Event>* owner = &event;
    std::unique_ptr<Event>* target = &moved;
    // relocates its event while it runs, as adding a component can relocate the array
    event->add("relocator", [&log, owner, target](){
        log.push_back("relocator");
        *target = std::make_unique<Event>(**owner);
        owner->reset();
    });
    event->add("b", [&log](){ log.push_back("b"); });

    event->call();
    // still subscribed through the copy, so the running call reaches it
    EXPECT_EQ(log, (std::vector<std::string>{"relocator", "b"}));
    ASSERT_NE(moved, nullptr);
    EXPECT_TRUE(moved->hasSubscribers());
}

// The guard catches a signal raised in the callback (POSIX); a sanitizer's own handlers would
// see it first
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(JASPERENGINE_SANITIZE_ADDRESS) && \
    !defined(JASPERENGINE_SANITIZE_UNDEFINED) && !defined(JASPERENGINE_SANITIZE_THREAD)
namespace {

    struct CrashHandlerScope {
        explicit CrashHandlerScope(CrashHandler handler) {
            CrashHook::set(std::move(handler));
        }
        ~CrashHandlerScope() {
            CrashHook::set(nullptr);
        }
    };

}

TEST(FunctionSubscribe, CrashGuardDropsTheCrashingSubscriber) {
    std::vector<std::string> crashes;
    CrashHandlerScope scope([&](const std::string& tag, const std::string& errorInfo){
        crashes.push_back(tag + " " + errorInfo.substr(0, errorInfo.find(' ')));
    });

    std::vector<std::string> log;
    Event event;
    event.add("a", [&](){ log.push_back("a"); });
    event.add("crasher", [&](){ log.push_back("crasher"); std::raise(SIGFPE); });
    event.add("c", [&](){ log.push_back("c"); });

    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "crasher", "c"}));
    EXPECT_EQ(crashes, (std::vector<std::string>{"crasher SIGFPE"}));

    log.clear();
    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "c"}));
    EXPECT_EQ(crashes.size(), 1u);

    // only the dropped one left: no subscribers
    event.remove("a");
    event.remove("c");
    EXPECT_FALSE(event.hasSubscribers());

    // and its tag can subscribe again
    event.add("crasher", [&](){ log.push_back("again"); });
    EXPECT_TRUE(event.hasSubscribers());
    log.clear();
    event.call();
    EXPECT_EQ(log, (std::vector<std::string>{"again"}));
}

TEST(FunctionSubscribe, CrashGuardWithReturnValueTriesTheNext) {
    std::vector<std::string> crashes;
    CrashHandlerScope scope([&](const std::string& tag, const std::string&){
        crashes.push_back(tag);
    });

    BoolEvent query;
    query.add("crasher", [](int) -> bool { std::raise(SIGFPE); return false; });
    query.add("answer", [](int value){ return value == 2; });

    EXPECT_TRUE(query.callRet(2, false));
    EXPECT_EQ(crashes, (std::vector<std::string>{"crasher"}));
    EXPECT_FALSE(query.callRet(3, true));
    EXPECT_EQ(crashes.size(), 1u);
}

TEST(FunctionSubscribe, CrashGuardAfterTheEventWasDestroyed) {
    std::vector<std::string> crashes;
    CrashHandlerScope scope([&](const std::string& tag, const std::string&){
        crashes.push_back(tag);
    });

    std::vector<std::string> log;
    auto event = std::make_unique<Event>();
    std::unique_ptr<Event>* owner = &event;
    event->add("destroyer", [&log, owner](){
        log.push_back("destroyer");
        owner->reset();
        std::raise(SIGFPE);
    });
    event->add("b", [&log](){ log.push_back("b"); });

    event->call();
    EXPECT_EQ(event, nullptr);
    // the crashing subscriber is dropped from the list the call holds, which goes on
    EXPECT_EQ(log, (std::vector<std::string>{"destroyer", "b"}));
    EXPECT_EQ(crashes, (std::vector<std::string>{"destroyer"}));
}
#endif
